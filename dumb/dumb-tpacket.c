// AF_PACKET + TPACKET_V3 버전
// libpcap 완전 제거, 커널 mmap ring buffer 직접 사용

#define _GNU_SOURCE
#include <stdatomic.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/mman.h>
#include <syslog.h>
#include <time.h>

// AF_PACKET headers
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <poll.h>

// Graceful shutdown flag
static volatile sig_atomic_t keep_running = 1;

// Packet statistics (atomic for thread-safety)
static struct {
    atomic_ulong rx_packets[2];
    atomic_ulong tx_packets[2];
    atomic_ulong dropped[2];
    atomic_ulong ring_full[2];  // Ring buffer full events
    atomic_ulong errors[2];
    time_t start_time;
} stats;

// TPACKET_V3 설정
#define BLOCK_SIZE (4 * 1024)          // 4KB blocks
#define FRAME_SIZE 2048                // 2KB per frame
#define BLOCK_NR 256                   // 256 blocks = 1MB ring
#define FRAME_NR ((BLOCK_SIZE * BLOCK_NR) / FRAME_SIZE)

// Interface 구조체
struct interface {
    char name[IFNAMSIZ];
    int ifindex;
    int sock_rx;         // RX socket (TPACKET_V3)
    int sock_tx;         // TX socket (raw)
    void *ring_rx;       // mmap'd RX ring buffer
    size_t ring_size;
    struct tpacket_req3 req;
    pthread_t thread;
};

static struct interface interfaces[2];

// 시그널 핸들러
static void sighandler(int sig) {
    (void)sig;
    keep_running = 0;
}

// 통계 출력
static void print_stats(int sig) {
    (void)sig;
    time_t now = time(NULL);
    time_t uptime = now - stats.start_time;

    fprintf(stderr, "\n=== TPACKET_V3 Statistics (uptime: %ld sec) ===\n", uptime);
    for (int i = 0; i < 2; i++) {
        unsigned long rx = atomic_load(&stats.rx_packets[i]);
        unsigned long tx = atomic_load(&stats.tx_packets[i]);
        unsigned long drop = atomic_load(&stats.dropped[i]);
        unsigned long ring_full = atomic_load(&stats.ring_full[i]);
        unsigned long err = atomic_load(&stats.errors[i]);

        fprintf(stderr, "  Interface %d (%s):\n", i, interfaces[i].name);
        fprintf(stderr, "    RX:        %10lu packets (%lu pps)\n", rx, uptime > 0 ? rx/uptime : 0);
        fprintf(stderr, "    TX:        %10lu packets (%lu pps)\n", tx, uptime > 0 ? tx/uptime : 0);
        fprintf(stderr, "    Dropped:   %10lu packets\n", drop);
        fprintf(stderr, "    RingFull:  %10lu events\n", ring_full);
        fprintf(stderr, "    Errors:    %10lu\n", err);
    }
    fprintf(stderr, "==========================================\n");

    syslog(LOG_INFO, "Stats: %s rx=%lu tx=%lu drop=%lu | %s rx=%lu tx=%lu drop=%lu",
           interfaces[0].name, atomic_load(&stats.rx_packets[0]),
           atomic_load(&stats.tx_packets[0]), atomic_load(&stats.dropped[0]),
           interfaces[1].name, atomic_load(&stats.rx_packets[1]),
           atomic_load(&stats.tx_packets[1]), atomic_load(&stats.dropped[1]));
}

// AF_PACKET 소켓 생성 및 인터페이스 바인딩
static int create_socket(const char *ifname, int *ifindex) {
    int sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sock < 0) {
        fprintf(stderr, "FATAL: socket() failed: %s\n", strerror(errno));
        return -1;
    }

    // 인터페이스 인덱스 조회
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr, "FATAL: ioctl(SIOCGIFINDEX) failed for %s: %s\n", ifname, strerror(errno));
        close(sock);
        return -1;
    }
    *ifindex = ifr.ifr_ifindex;

    // 소켓을 인터페이스에 바인딩
    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = *ifindex;

    if (bind(sock, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        fprintf(stderr, "FATAL: bind() failed for %s: %s\n", ifname, strerror(errno));
        close(sock);
        return -1;
    }

    return sock;
}

// TPACKET_V3 RX ring 설정
static int setup_rx_ring(struct interface *iface) {
    // TPACKET_V3 설정
    iface->req.tp_block_size = BLOCK_SIZE;
    iface->req.tp_frame_size = FRAME_SIZE;
    iface->req.tp_block_nr = BLOCK_NR;
    iface->req.tp_frame_nr = FRAME_NR;
    iface->req.tp_retire_blk_tov = 64;  // 64ms timeout
    iface->req.tp_feature_req_word = TP_FT_REQ_FILL_RXHASH;

    // TPACKET_V3 활성화
    int version = TPACKET_V3;
    if (setsockopt(iface->sock_rx, SOL_PACKET, PACKET_VERSION, &version, sizeof(version)) < 0) {
        fprintf(stderr, "FATAL: setsockopt(PACKET_VERSION) failed: %s\n", strerror(errno));
        return -1;
    }

    // RX ring 설정
    if (setsockopt(iface->sock_rx, SOL_PACKET, PACKET_RX_RING, &iface->req, sizeof(iface->req)) < 0) {
        fprintf(stderr, "FATAL: setsockopt(PACKET_RX_RING) failed: %s\n", strerror(errno));
        return -1;
    }

    // Ring buffer mmap
    iface->ring_size = iface->req.tp_block_size * iface->req.tp_block_nr;
    iface->ring_rx = mmap(NULL, iface->ring_size, PROT_READ | PROT_WRITE, MAP_SHARED, iface->sock_rx, 0);
    if (iface->ring_rx == MAP_FAILED) {
        fprintf(stderr, "FATAL: mmap() failed: %s\n", strerror(errno));
        return -1;
    }

    fprintf(stderr, "Interface %s: TPACKET_V3 ring setup complete (%zu bytes, %u blocks)\n",
            iface->name, iface->ring_size, iface->req.tp_block_nr);

    // Promiscuous mode: 브리지 목적이면 반드시 필요(호스트로 향하지 않는 프레임도 수신)
    struct packet_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.mr_ifindex = iface->ifindex;
    mreq.mr_type = PACKET_MR_PROMISC;
    if (setsockopt(iface->sock_rx, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        fprintf(stderr, "WARNING: PACKET_MR_PROMISC failed on %s: %s\n", iface->name, strerror(errno));
    }

    return 0;
}

// RX/TX 스레드
static void *interface_thread(void *arg) {
    unsigned int idx = (unsigned int)((uintptr_t)arg);
    struct interface *rx_iface = &interfaces[idx];
    struct interface *tx_iface = &interfaces[idx ^ 1];

    // CPU affinity 설정
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(idx, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
        fprintf(stderr, "WARNING: pthread_setaffinity_np(%u) failed: %s\n", idx, strerror(errno));
    } else {
        fprintf(stderr, "Thread %u pinned to CPU %u\n", idx, idx);
    }

    // RT 스케줄링
    struct sched_param sp = {.sched_priority = 50};
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        fprintf(stderr, "WARNING: pthread_setschedparam(%u) failed: %s\n", idx, strerror(errno));
    } else {
        fprintf(stderr, "Thread %u set to SCHED_FIFO priority 50\n", idx, idx);
    }

    fprintf(stderr, "Thread %u: RX from %s, TX to %s\n", idx, rx_iface->name, tx_iface->name);

    // TPACKET_V3 block descriptor 순회
    unsigned int block_idx = 0;
    struct pollfd pfd;
    pfd.fd = rx_iface->sock_rx;
    pfd.events = POLLIN | POLLERR;
    pfd.revents = 0;

    while (keep_running) {
        // Block이 준비될 때까지 poll (timeout 100ms)
        int ret = poll(&pfd, 1, 100);
        if (ret < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "ERROR: poll() failed: %s\n", strerror(errno));
            atomic_fetch_add(&stats.errors[idx], 1);
            break;
        }
        if (ret == 0) continue;  // Timeout

        // Block descriptor 가져오기
        struct tpacket_block_desc *pbd = (struct tpacket_block_desc *)
            ((uint8_t *)rx_iface->ring_rx + block_idx * BLOCK_SIZE);

        // Block이 사용자 공간에서 사용 가능한지 확인
        if ((pbd->hdr.bh1.block_status & TP_STATUS_USER) == 0) {
            continue;
        }

        // Block 내의 모든 패킷 처리
        unsigned int num_pkts = pbd->hdr.bh1.num_pkts;
        struct tpacket3_hdr *ppd = (struct tpacket3_hdr *)((uint8_t *)pbd + pbd->hdr.bh1.offset_to_first_pkt);

        for (unsigned int i = 0; i < num_pkts; i++) {
            // 패킷 데이터
            uint8_t *pkt_data = (uint8_t *)ppd + ppd->tp_mac;
            uint32_t pkt_len = ppd->tp_snaplen;

            // RX 카운터
            atomic_fetch_add(&stats.rx_packets[idx], 1);

            // TX (sendto)
            const struct ethhdr *eh = (const struct ethhdr *)pkt_data;
            struct sockaddr_ll dest_addr;
            memset(&dest_addr, 0, sizeof(dest_addr));
            dest_addr.sll_family = AF_PACKET;
            dest_addr.sll_protocol = htons(ETH_P_ALL);
            dest_addr.sll_ifindex = tx_iface->ifindex;
            dest_addr.sll_halen = ETH_ALEN;
            memcpy(dest_addr.sll_addr, eh->h_dest, ETH_ALEN);

            ssize_t sent = sendto(tx_iface->sock_tx, pkt_data, pkt_len, 0,
                                  (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            if (sent < 0) {
                atomic_fetch_add(&stats.dropped[idx], 1);
                atomic_fetch_add(&stats.errors[idx ^ 1], 1);
            } else {
                atomic_fetch_add(&stats.tx_packets[idx ^ 1], 1);
            }

            // 다음 패킷으로
            ppd = (struct tpacket3_hdr *)((uint8_t *)ppd + ppd->tp_next_offset);
        }

        // Block을 커널에 반환
        pbd->hdr.bh1.block_status = TP_STATUS_KERNEL;
        block_idx = (block_idx + 1) % rx_iface->req.tp_block_nr;
    }

    fprintf(stderr, "Thread %u exiting gracefully\n", idx);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <interface0> <interface1>\n", argv[0]);
        fprintf(stderr, "Example: %s eth0 wlan0\n", argv[0]);
        return 1;
    }

    // syslog 초기화
    openlog("dumb-tpacket", LOG_PID | LOG_CONS, LOG_DAEMON);

    // 시그널 핸들러
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    signal(SIGUSR1, print_stats);

    // 통계 초기화
    memset(&stats, 0, sizeof(stats));
    stats.start_time = time(NULL);

    // 인터페이스 초기화
    for (int i = 0; i < 2; i++) {
        strncpy(interfaces[i].name, argv[i + 1], IFNAMSIZ - 1);

        // RX 소켓 생성
        interfaces[i].sock_rx = create_socket(interfaces[i].name, &interfaces[i].ifindex);
        if (interfaces[i].sock_rx < 0) {
            return 1;
        }

        // TX 소켓 생성 (별도)
        interfaces[i].sock_tx = create_socket(interfaces[i].name, &interfaces[i].ifindex);
        if (interfaces[i].sock_tx < 0) {
            return 1;
        }

        // TX 경로에서 qdisc 우회(가능한 경우)로 레이턴시/오버헤드 감소
        int qdisc_bypass = 1;
        if (setsockopt(interfaces[i].sock_tx, SOL_PACKET, PACKET_QDISC_BYPASS, &qdisc_bypass, sizeof(qdisc_bypass)) < 0) {
            fprintf(stderr, "WARNING: PACKET_QDISC_BYPASS failed on %s: %s\n", interfaces[i].name, strerror(errno));
        }

        // TPACKET_V3 RX ring 설정
        if (setup_rx_ring(&interfaces[i]) < 0) {
            return 1;
        }
    }

    // 스레드 생성
    for (int i = 0; i < 2; i++) {
        if (pthread_create(&interfaces[i].thread, NULL, interface_thread, (void *)((uintptr_t)i)) != 0) {
            fprintf(stderr, "FATAL: pthread_create(%d) failed: %s\n", i, strerror(errno));
            return 1;
        }
    }

    // 메모리 잠금 (스레드 생성 후)
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf(stderr, "WARNING: mlockall() failed: %s\n", strerror(errno));
    } else {
        fprintf(stderr, "Memory locked to prevent page faults\n");
    }

    // 메인 루프
    fprintf(stderr, "TPACKET_V3 bridge running. Press Ctrl+C to stop, send SIGUSR1 for stats.\n");
    fprintf(stderr, "  Usage: kill -USR1 %d\n", getpid());
    syslog(LOG_INFO, "TPACKET_V3 bridge started (PID %d)", getpid());

    while (keep_running) {
        sleep(1);
    }

    // Graceful shutdown
    fprintf(stderr, "\nShutting down gracefully...\n");

    for (int i = 0; i < 2; i++) {
        pthread_join(interfaces[i].thread, NULL);
    }

    // 리소스 정리
    for (int i = 0; i < 2; i++) {
        if (interfaces[i].ring_rx) {
            munmap(interfaces[i].ring_rx, interfaces[i].ring_size);
        }
        if (interfaces[i].sock_rx >= 0) {
            close(interfaces[i].sock_rx);
        }
        if (interfaces[i].sock_tx >= 0) {
            close(interfaces[i].sock_tx);
        }
    }

    // 최종 통계
    print_stats(0);

    fprintf(stderr, "Shutdown complete.\n");
    syslog(LOG_INFO, "TPACKET_V3 bridge stopped");
    closelog();

    return 0;
}
