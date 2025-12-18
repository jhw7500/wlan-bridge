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
#include <getopt.h>
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

struct config {
    int dispatch_budget;
    int enable_affinity;
    int enable_rt;
    int rt_priority;
    int enable_mlock;
    int snaplen;
    int pcap_buffer_bytes;
    int timeout_ms;
    int enable_immediate;
    int enable_promisc;
};

static struct {
    pthread_t threads[2];
    pcap_t *rx[2];
    pcap_t *tx[2];
    atomic_int ready; // bit0: if0 ready, bit1: if1 ready
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} ifs;

static struct config cfg;

static int env_to_int(const char *key, int default_value)
{
    const char *env = getenv(key);
    if (!env || !*env) {
        return default_value;
    }
    char *end = NULL;
    long v = strtol(env, &end, 10);
    if (!end || end == env || *end != '\0') {
        return default_value;
    }
    return (int)v;
}

static int clamp_int(int v, int min_v, int max_v)
{
    if (v < min_v) return min_v;
    if (v > max_v) return max_v;
    return v;
}

static int get_dispatch_budget(void)
{
    return clamp_int(cfg.dispatch_budget, 1, 4096);
}

static void print_usage(FILE *out, const char *prog)
{
    fprintf(out,
            "Usage: %s [options] <interface0> <interface1>\n"
            "\n"
            "Options (CLI overrides env; env overrides defaults):\n"
            "  --dispatch-budget N     Max packets per dispatch (env DUMB_DISPATCH_BUDGET, default 64)\n"
            "  --no-affinity           Disable CPU pinning (env DUMB_AFFINITY=0, default enabled)\n"
            "  --no-rt                 Disable SCHED_FIFO (env DUMB_RT=0, default enabled)\n"
            "  --rt-priority N         SCHED_FIFO priority (env DUMB_RT_PRIORITY, default 50)\n"
            "  --no-mlock              Disable mlockall (env DUMB_MLOCK=0, default enabled)\n"
            "  --snaplen N             pcap snaplen bytes (env DUMB_SNAPLEN, default 1600)\n"
            "  --pcap-buffer BYTES     pcap RX buffer bytes (env DUMB_PCAP_BUFFER, default 4194304)\n"
            "  --timeout-ms N          pcap timeout ms (env DUMB_TIMEOUT_MS, default 1)\n"
            "  --no-immediate          Disable immediate mode (env DUMB_IMMEDIATE=0, default enabled)\n"
            "  --no-promisc            Disable promisc (env DUMB_PROMISC=0, default enabled; may break bridging)\n"
            "  -h, --help              Show help\n",
            prog);
}

static void init_config_from_env(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.dispatch_budget = env_to_int("DUMB_DISPATCH_BUDGET", 64);
    cfg.enable_affinity = env_to_int("DUMB_AFFINITY", 1) ? 1 : 0;
    cfg.enable_rt = env_to_int("DUMB_RT", 1) ? 1 : 0;
    cfg.rt_priority = env_to_int("DUMB_RT_PRIORITY", 50);
    cfg.enable_mlock = env_to_int("DUMB_MLOCK", 1) ? 1 : 0;
    cfg.snaplen = env_to_int("DUMB_SNAPLEN", DUMB_SNAPLEN);
    cfg.pcap_buffer_bytes = env_to_int("DUMB_PCAP_BUFFER", DUMB_PCAP_BUFFER_SIZE_BYTES);
    cfg.timeout_ms = env_to_int("DUMB_TIMEOUT_MS", 1);
    cfg.enable_immediate = env_to_int("DUMB_IMMEDIATE", 1) ? 1 : 0;
    cfg.enable_promisc = env_to_int("DUMB_PROMISC", 1) ? 1 : 0;

    cfg.dispatch_budget = clamp_int(cfg.dispatch_budget, 1, 4096);
    cfg.rt_priority = clamp_int(cfg.rt_priority, 1, 99);
    cfg.snaplen = clamp_int(cfg.snaplen, 64, 65535);
    cfg.pcap_buffer_bytes = clamp_int(cfg.pcap_buffer_bytes, 256 * 1024, 64 * 1024 * 1024);
    cfg.timeout_ms = clamp_int(cfg.timeout_ms, 0, 1000);
}

static pcap_t *open_pcap_handle(const char *ifname, int is_rx, char errbuf[PCAP_ERRBUF_SIZE])
{
    pcap_t *h = pcap_create(ifname, errbuf);
    if (!h) {
        return NULL;
    }

    if (pcap_set_snaplen(h, cfg.snaplen) != 0) {
        fprintf(stderr, "WARN: snaplen set failed on %s: %s\n", ifname, pcap_geterr(h));
    }

    if (is_rx) {
        if (pcap_set_buffer_size(h, cfg.pcap_buffer_bytes) != 0) {
            fprintf(stderr, "WARN: buffer size set failed on %s: %s\n", ifname, pcap_geterr(h));
        }
        if (pcap_set_promisc(h, cfg.enable_promisc ? 1 : 0) != 0) {
            fprintf(stderr, "WARN: promisc set failed on %s: %s\n", ifname, pcap_geterr(h));
        }
        if (pcap_set_timeout(h, cfg.timeout_ms) != 0) { // ms timeout
            fprintf(stderr, "WARN: timeout set failed on %s: %s\n", ifname, pcap_geterr(h));
        }
        if (cfg.enable_immediate) {
            if (pcap_set_immediate_mode(h, 1) != 0) {
                fprintf(stderr, "WARN: immediate mode set failed on %s: %s\n", ifname, pcap_geterr(h));
            }
        }
    }

    int act_rc = pcap_activate(h);
    if (act_rc < 0) {
        fprintf(stderr, "FATAL: activate %s failed: %s\n", ifname, pcap_statustostr(act_rc));
        pcap_close(h);
        return NULL;
    } else if (act_rc > 0) {
        fprintf(stderr, "WARN: activate %s warning: %s\n", ifname, pcap_statustostr(act_rc));
    }

    return h;
}

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
    if (!both_ready() || ifs.tx[peer] == NULL) {
        atomic_fetch_add(&stats.dropped[i], 1);
        return; // 아직 준비 전이면 드롭
    }

    // snaplen에 의해 잘린(caplen < len) 패킷은 캡처된 길이만큼만 전달
    int ret = pcap_inject(ifs.tx[peer], data, hdr->caplen);
    if (ret < 0) {
        // 에러 카운터 증가
        atomic_fetch_add(&stats.dropped[i], 1);
        atomic_fetch_add(&stats.errors[peer], 1);

        // 에러는 로그만 남기고 계속 (rate-limited)
        static atomic_ulong error_count = 0;
        unsigned long err_cnt = atomic_fetch_add(&error_count, 1);
        if (err_cnt % 1000 == 0) { // 1000개마다 한 번만 로그
            const char *err = pcap_geterr(ifs.tx[peer]);
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

    // 입력 전용으로 설정: 일부 환경에서는 activate 이후에만 제대로 적용됨.
    // 이 설정이 무시되면(=outgoing까지 캡처되면) 브릿지가 자기 자신을 다시 캡처해
    // 재주입하는 루프가 생겨 ping DUP/큰 지연이 발생할 수 있음.
    if (pcap_setdirection(ifs.rx[i], PCAP_D_IN) != 0) {
        fprintf(stderr, "WARNING: pcap_setdirection(%u) ignored: %s\n", i, pcap_geterr(ifs.rx[i]));
    }

    // CPU affinity 설정: 인터페이스별로 전용 코어 할당
    if (cfg.enable_affinity) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(i, &cpuset); // if0 -> CPU0, if1 -> CPU1
        if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
            fprintf(stderr, "WARNING: pthread_setaffinity_np(%u) failed: %s\n", i, strerror(errno));
        } else {
            fprintf(stderr, "Thread %u pinned to CPU %u\n", i, i);
        }
    }

    // Real-time 스케줄링 설정: SCHED_FIFO priority 50
    if (cfg.enable_rt) {
        struct sched_param sp = {.sched_priority = cfg.rt_priority};
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
            fprintf(stderr, "WARNING: pthread_setschedparam(%u) failed: %s (need CAP_SYS_NICE or root)\n",
                    i, strerror(errno));
        } else {
            fprintf(stderr, "Thread %u set to SCHED_FIFO priority %d\n", i, cfg.rt_priority);
        }
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
    const int budget = get_dispatch_budget();
    int stats_check_counter = 0;
    while (keep_running) {
        int rc = pcap_dispatch(ifs.rx[i], budget, ph, (unsigned char *)ifp);
        if (rc == PCAP_ERROR) {
            const char *err = pcap_geterr(ifs.rx[i]);
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
            if (pcap_stats(ifs.rx[i], &pstat) == 0) {
                atomic_store(&stats.pcap_drop[i], pstat.ps_drop + pstat.ps_ifdrop);
            }
        }
    }

    fprintf(stderr, "Thread %u exiting gracefully\n", i);
    return NULL;
}

static void cleanup(void) {
    for (int i = 0; i < 2; i++) {
        if (ifs.rx[i]) {
            pcap_close(ifs.rx[i]);
            ifs.rx[i] = NULL;
        }
        if (ifs.tx[i]) {
            pcap_close(ifs.tx[i]);
            ifs.tx[i] = NULL;
        }
    }

    // Mutex 및 condition variable 정리
    pthread_cond_destroy(&ifs.cond);
    pthread_mutex_destroy(&ifs.mutex);
}

int main(int argc, char **argv)
{
    char errbuf[PCAP_ERRBUF_SIZE];

    init_config_from_env();

    static struct option long_opts[] = {
        {"dispatch-budget", required_argument, 0, 1},
        {"no-affinity", no_argument, 0, 2},
        {"no-rt", no_argument, 0, 3},
        {"rt-priority", required_argument, 0, 4},
        {"no-mlock", no_argument, 0, 5},
        {"snaplen", required_argument, 0, 6},
        {"pcap-buffer", required_argument, 0, 7},
        {"timeout-ms", required_argument, 0, 8},
        {"no-immediate", no_argument, 0, 9},
        {"no-promisc", no_argument, 0, 10},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 1:
            cfg.dispatch_budget = clamp_int(atoi(optarg), 1, 4096);
            break;
        case 2:
            cfg.enable_affinity = 0;
            break;
        case 3:
            cfg.enable_rt = 0;
            break;
        case 4:
            cfg.rt_priority = clamp_int(atoi(optarg), 1, 99);
            break;
        case 5:
            cfg.enable_mlock = 0;
            break;
        case 6:
            cfg.snaplen = clamp_int(atoi(optarg), 64, 65535);
            break;
        case 7:
            cfg.pcap_buffer_bytes = clamp_int(atoi(optarg), 256 * 1024, 64 * 1024 * 1024);
            break;
        case 8:
            cfg.timeout_ms = clamp_int(atoi(optarg), 0, 1000);
            break;
        case 9:
            cfg.enable_immediate = 0;
            break;
        case 10:
            cfg.enable_promisc = 0;
            break;
        case 'h':
            print_usage(stdout, argv[0]);
            return 0;
        default:
            print_usage(stderr, argv[0]);
            return 1;
        }
    }

    if (argc - optind != 2) {
        print_usage(stderr, argv[0]);
        return 1;
    }
    const char *if0 = argv[optind];
    const char *if1 = argv[optind + 1];

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
    memset(ifs.rx, 0, sizeof(ifs.rx));
    memset(ifs.tx, 0, sizeof(ifs.tx));

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
        const char *ifname = (i == 0) ? if0 : if1;
        ifs.rx[i] = open_pcap_handle(ifname, 1, errbuf);
        if (!ifs.rx[i]) {
            cleanup();
            return 1;
        }
        ifs.tx[i] = open_pcap_handle(ifname, 0, errbuf);
        if (!ifs.tx[i]) {
            cleanup();
            return 1;
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
    if (cfg.enable_mlock) {
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
            fprintf(stderr, "WARNING: mlockall() failed: %s (need CAP_IPC_LOCK or root)\n", strerror(errno));
            fprintf(stderr, "         Continuing without memory locking (performance may be affected)\n");
        } else {
            fprintf(stderr, "Memory locked to prevent page faults\n");
        }
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
        if (ifs.rx[i]) {
            pcap_breakloop(ifs.rx[i]);
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
