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
#include <syslog.h>
#include <time.h>

// Graceful shutdown flag
static volatile sig_atomic_t keep_running = 1;

// Packet statistics (atomic for thread-safety)
static struct {
    atomic_ulong rx_packets[2];   // Received packets per interface
    atomic_ulong tx_packets[2];   // Transmitted packets per interface
    atomic_ulong dropped[2];      // Dropped packets (inject failed)
    atomic_ulong pcap_drop[2];    // Dropped by pcap (kernel buffer overflow)
    atomic_ulong errors[2];       // Other errors
    time_t start_time;            // Program start time
} stats;

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
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} ifs;

static inline int both_ready(void) {
    int r = atomic_load_explicit(&ifs.ready, memory_order_acquire);
    return (r & 0x3) == 0x3;
}

static void sighandler(int sig) {
    (void)sig;
    keep_running = 0;
}

static void print_stats(int sig) {
    (void)sig;
    time_t now = time(NULL);
    time_t uptime = now - stats.start_time;

    fprintf(stderr, "\n=== Packet Statistics (uptime: %ld seconds) ===\n", uptime);
    for (int i = 0; i < 2; i++) {
        unsigned long rx = atomic_load(&stats.rx_packets[i]);
        unsigned long tx = atomic_load(&stats.tx_packets[i]);
        unsigned long drop = atomic_load(&stats.dropped[i]);
        unsigned long pcap_drop = atomic_load(&stats.pcap_drop[i]);
        unsigned long err = atomic_load(&stats.errors[i]);

        fprintf(stderr, "  Interface %d:\n", i);
        fprintf(stderr, "    RX:        %10lu packets (%lu pps)\n", rx, uptime > 0 ? rx/uptime : 0);
        fprintf(stderr, "    TX:        %10lu packets (%lu pps)\n", tx, uptime > 0 ? tx/uptime : 0);
        fprintf(stderr, "    Dropped:   %10lu packets\n", drop);
        fprintf(stderr, "    PcapDrop:  %10lu packets\n", pcap_drop);
        fprintf(stderr, "    Errors:    %10lu\n", err);
    }
    fprintf(stderr, "==========================================\n");

    // syslog에도 기록
    syslog(LOG_INFO, "Stats: if0 rx=%lu tx=%lu drop=%lu | if1 rx=%lu tx=%lu drop=%lu",
           atomic_load(&stats.rx_packets[0]),
           atomic_load(&stats.tx_packets[0]),
           atomic_load(&stats.dropped[0]),
           atomic_load(&stats.rx_packets[1]),
           atomic_load(&stats.tx_packets[1]),
           atomic_load(&stats.dropped[1]));
}

static void ph(unsigned char *ifp, const struct pcap_pkthdr *hdr, const unsigned char *data)
{
    unsigned int i = (unsigned int)((uintptr_t)ifp);
    unsigned int peer = i ^ 1;

    if (!(hdr && data)) return;

    // RX 카운터 증가
    atomic_fetch_add(&stats.rx_packets[i], 1);

    // 상대 인터페이스 준비 여부 확인
    if (!both_ready() || ifs.interfaces[peer] == NULL) {
        atomic_fetch_add(&stats.dropped[i], 1);
        return; // 아직 준비 전이면 드롭
    }

    // snaplen에 의해 잘린(caplen < len) 패킷은 캡처된 길이만큼만 전달
    int ret = pcap_inject(ifs.interfaces[peer], data, hdr->caplen);
    if (ret < 0) {
        // 에러 카운터 증가
        atomic_fetch_add(&stats.dropped[i], 1);
        atomic_fetch_add(&stats.errors[peer], 1);

        // 에러는 로그만 남기고 계속 (rate-limited)
        static atomic_ulong error_count = 0;
        unsigned long err_cnt = atomic_fetch_add(&error_count, 1);
        if (err_cnt % 1000 == 0) { // 1000개마다 한 번만 로그
            const char *err = pcap_geterr(ifs.interfaces[peer]);
            fprintf(stderr, "pcap_inject(%u->%u) failed: %s (total errors: %lu)\n",
                    i, peer, err ? err : "unknown", err_cnt + 1);
        }
    } else {
        // TX 카운터 증가
        atomic_fetch_add(&stats.tx_packets[peer], 1);
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

    // 본인 준비 플래그 set 및 condition variable로 통지
    pthread_mutex_lock(&ifs.mutex);
    atomic_fetch_or_explicit(&ifs.ready, (1 << i), memory_order_release);
    pthread_cond_broadcast(&ifs.cond); // 다른 스레드 깨우기

    // 두 쪽 다 준비될 때까지 대기 (busy-wait 제거)
    while (!both_ready()) {
        pthread_cond_wait(&ifs.cond, &ifs.mutex);
    }
    pthread_mutex_unlock(&ifs.mutex);

    fprintf(stderr, "Thread %u: both interfaces ready, starting packet forwarding\n", i);

    // pcap_loop 대신 dispatch를 사용하여 keep_running 체크 가능하게 함
    int stats_check_counter = 0;
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

        // 주기적으로 pcap 통계 확인 (매 1000번 루프마다)
        if (++stats_check_counter >= 1000) {
            stats_check_counter = 0;
            struct pcap_stat pstat;
            if (pcap_stats(ifs.interfaces[i], &pstat) == 0) {
                atomic_store(&stats.pcap_drop[i], pstat.ps_drop + pstat.ps_ifdrop);
            }
        }
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

    // Mutex 및 condition variable 정리
    pthread_cond_destroy(&ifs.cond);
    pthread_mutex_destroy(&ifs.mutex);
}

int main(int argc, char **argv)
{
    char errbuf[PCAP_ERRBUF_SIZE];

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <interface0> <interface1>\n", argv[0]);
        return 1;
    }

    // syslog 초기화
    openlog("dumb-bridge", LOG_PID | LOG_CONS, LOG_DAEMON);

    // 시그널 핸들러 등록
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    signal(SIGUSR1, print_stats);  // 통계 출력

    // 통계 초기화
    memset(&stats, 0, sizeof(stats));
    stats.start_time = time(NULL);

    atomic_init(&ifs.ready, 0);
    memset(ifs.interfaces, 0, sizeof(ifs.interfaces));

    // Mutex 및 condition variable 초기화
    if (pthread_mutex_init(&ifs.mutex, NULL) != 0) {
        fprintf(stderr, "FATAL: pthread_mutex_init failed\n");
        return 1;
    }
    if (pthread_cond_init(&ifs.cond, NULL) != 0) {
        fprintf(stderr, "FATAL: pthread_cond_init failed\n");
        pthread_mutex_destroy(&ifs.mutex);
        return 1;
    }

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

    // 3) 스레드 생성 후 메모리 잠금 (더 안전함)
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf(stderr, "WARNING: mlockall() failed: %s (need CAP_IPC_LOCK or root)\n", strerror(errno));
        fprintf(stderr, "         Continuing without memory locking (performance may be affected)\n");
    } else {
        fprintf(stderr, "Memory locked to prevent page faults\n");
    }

    // 메인 루프: 시그널 대기
    fprintf(stderr, "Bridge running. Press Ctrl+C to stop, send SIGUSR1 for stats.\n");
    fprintf(stderr, "  Usage: kill -USR1 %d\n", getpid());
    syslog(LOG_INFO, "Bridge started (PID %d)", getpid());

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

    // 최종 통계 출력
    print_stats(0);

    fprintf(stderr, "Shutdown complete.\n");
    syslog(LOG_INFO, "Bridge stopped");
    closelog();
    return 0;
}
