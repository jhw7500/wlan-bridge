#define _GNU_SOURCE
#include <stdatomic.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <pcap/pcap.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/mman.h>

// Graceful shutdown flag
static volatile sig_atomic_t keep_running = 1;

// 운영 환경 기본값(i.MX8MM + 88W9098 가정)
// - MTU 1500 기준으로 충분한 스냅렌스(헤더/VLAN 여유 포함)
// - 값은 필요 시 여기만 수정하면 됨
#define DUMB_MTU 1500
#define DUMB_SNAPLEN 1600
#define DUMB_PCAP_BUFFER_SIZE_BYTES (4 * 1024 * 1024)

static struct {
    pthread_t threads[2];
    pcap_t *interfaces[2];
    atomic_int ready; // bit0: if0 ready, bit1: if1 ready
} ifs;

static inline int both_ready(void) {
    int r = atomic_load_explicit(&ifs.ready, memory_order_acquire);
    return (r & 0x3) == 0x3;
}

static void sighandler(int sig) {
    (void)sig;
    keep_running = 0;
}

static void ph(unsigned char *ifp, const struct pcap_pkthdr *hdr, const unsigned char *data)
{
    unsigned int i = (unsigned int)((uintptr_t)ifp);
    unsigned int peer = i ^ 1;

    if (!(hdr && data)) return;

    // 상대 인터페이스 준비 여부 확인
    if (!both_ready() || ifs.interfaces[peer] == NULL) {
        return; // 아직 준비 전이면 드롭
    }

    // snaplen에 의해 잘린(caplen < len) 패킷은 캡처된 길이만큼만 전달
    int ret = pcap_inject(ifs.interfaces[peer], data, hdr->caplen);
    if (ret < 0) {
        // 에러는 로그만 남기고 계속
        const char *err = pcap_geterr(ifs.interfaces[peer]);
        fprintf(stderr, "pcap_inject(%u->%u) failed: %s\n", i, peer, err ? err : "unknown");
    }
}

static void *thr(void *ifp)
{
    unsigned int i = (unsigned int)((uintptr_t)ifp);

    // CPU affinity 설정: 인터페이스별로 전용 코어 할당
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(i, &cpuset); // if0 -> CPU0, if1 -> CPU1
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
        fprintf(stderr, "WARNING: pthread_setaffinity_np(%u) failed: %s\n", i, strerror(errno));
    } else {
        fprintf(stderr, "Thread %u pinned to CPU %u\n", i, i);
    }

    // Real-time 스케줄링 설정: SCHED_FIFO priority 50
    struct sched_param sp = {.sched_priority = 50};
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        fprintf(stderr, "WARNING: pthread_setschedparam(%u) failed: %s (need CAP_SYS_NICE or root)\n",
                i, strerror(errno));
    } else {
        fprintf(stderr, "Thread %u set to SCHED_FIFO priority 50\n", i);
    }

    // 입력 전용으로 설정 (장치가 지원 안 하면 무시될 수 있음)
    if (pcap_setdirection(ifs.interfaces[i], PCAP_D_IN) != 0) {
        fprintf(stderr, "pcap_setdirection(%u) ignored: %s\n", i, pcap_geterr(ifs.interfaces[i]));
    }

    // 본인 준비 플래그 set
    atomic_fetch_or_explicit(&ifs.ready, (1 << i), memory_order_release);

    // 두 쪽 다 준비될 때까지 잠깐 대기 (busy-wait 최소화)
    for (int t = 0; t < 200; ++t) { // 최대 ~200ms
        if (both_ready()) break;
        usleep(1000);
    }

    // pcap_loop 대신 dispatch를 사용하여 keep_running 체크 가능하게 함
    while (keep_running) {
        int rc = pcap_dispatch(ifs.interfaces[i], -1, ph, (unsigned char *)ifp);
        if (rc == PCAP_ERROR) {
            const char *err = pcap_geterr(ifs.interfaces[i]);
            fprintf(stderr, "pcap_dispatch(%u) error: %s\n", i, err ? err : "unknown");
            keep_running = 0;
            break;
        }
        // rc == 0: timeout, continue
        // rc > 0: processed packets, continue
    }

    fprintf(stderr, "Thread %u exiting gracefully\n", i);
    return NULL;
}

static void cleanup(void) {
    for (int i = 0; i < 2; i++) {
        if (ifs.interfaces[i]) {
            pcap_close(ifs.interfaces[i]);
            ifs.interfaces[i] = NULL;
        }
    }
}

int main(int argc, char **argv)
{
    char errbuf[PCAP_ERRBUF_SIZE];

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <interface0> <interface1>\n", argv[0]);
        return 1;
    }

    // 시그널 핸들러 등록
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    // 메모리 잠금: 페이지 폴트 방지 (실시간 성능 향상)
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf(stderr, "WARNING: mlockall() failed: %s (need CAP_IPC_LOCK or root)\n", strerror(errno));
        fprintf(stderr, "         Continuing without memory locking (performance may be affected)\n");
    } else {
        fprintf(stderr, "Memory locked to prevent page faults\n");
    }

    atomic_init(&ifs.ready, 0);
    memset(ifs.interfaces, 0, sizeof(ifs.interfaces));

    // 1) 두 인터페이스를 모두 연다
    for (int i = 0; i < 2; ++i) {
        ifs.interfaces[i] = pcap_create(argv[i + 1], errbuf);
        if (!ifs.interfaces[i]) {
            fprintf(stderr, "FATAL: create %s failed: %s\n", argv[i + 1], errbuf);
            return 1;
        }

        // 활성화 이전 설정들: 캡처 지연 최소화를 위해 즉시 모드 on, 큰 스냅렌스 유지
        if (pcap_set_snaplen(ifs.interfaces[i], DUMB_SNAPLEN) != 0) {
            fprintf(stderr, "WARN: snaplen set failed on %s: %s\n", argv[i + 1], pcap_geterr(ifs.interfaces[i]));
        }
        if (pcap_set_buffer_size(ifs.interfaces[i], DUMB_PCAP_BUFFER_SIZE_BYTES) != 0) {
            fprintf(stderr, "WARN: buffer size set failed on %s: %s\n", argv[i + 1], pcap_geterr(ifs.interfaces[i]));
        }
        if (pcap_set_promisc(ifs.interfaces[i], 1) != 0) {
            fprintf(stderr, "WARN: promisc set failed on %s: %s\n", argv[i + 1], pcap_geterr(ifs.interfaces[i]));
        }
        if (pcap_set_timeout(ifs.interfaces[i], 1) != 0) { // 1ms timeout
            fprintf(stderr, "WARN: timeout set failed on %s: %s\n", argv[i + 1], pcap_geterr(ifs.interfaces[i]));
        }
        if (pcap_set_immediate_mode(ifs.interfaces[i], 1) != 0) {
            fprintf(stderr, "WARN: immediate mode set failed on %s: %s\n", argv[i + 1], pcap_geterr(ifs.interfaces[i]));
        }

        int act_rc = pcap_activate(ifs.interfaces[i]);
        if (act_rc < 0) {
            fprintf(stderr, "FATAL: activate %s failed: %s\n", argv[i + 1], pcap_statustostr(act_rc));
            return 1;
        } else if (act_rc > 0) {
            // non-fatal warning from libpcap (e.g., promisc not supported)
            fprintf(stderr, "WARN: activate %s warning: %s\n", argv[i + 1], pcap_statustostr(act_rc));
        }
    }

    // 2) 스레드를 그 다음에 시작
    for (int i = 0; i < 2; ++i) {
        if (pthread_create(&ifs.threads[i], NULL, thr, (void *)((uintptr_t)i))) {
            fprintf(stderr, "FATAL: pthread_create(%d) failed\n", i);
            cleanup();
            return 1;
        }
    }

    // 메인 루프: 시그널 대기
    fprintf(stderr, "Bridge running. Press Ctrl+C to stop.\n");
    while (keep_running) {
        sleep(1);
    }

    // Graceful shutdown
    fprintf(stderr, "\nShutting down gracefully...\n");

    // pcap_breakloop으로 스레드 깨우기
    for (int i = 0; i < 2; i++) {
        if (ifs.interfaces[i]) {
            pcap_breakloop(ifs.interfaces[i]);
        }
    }

    // 스레드 종료 대기
    for (int i = 0; i < 2; i++) {
        pthread_join(ifs.threads[i], NULL);
    }

    // 리소스 정리
    cleanup();

    fprintf(stderr, "Shutdown complete.\n");
    return 0;
}
