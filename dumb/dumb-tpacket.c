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
#include <netinet/ip.h>

// AF_PACKET headers
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <poll.h>
#include <ifaddrs.h>

// Graceful shutdown flag
static volatile sig_atomic_t keep_running = 1;

// Cache line size for alignment (prevents false sharing)
#define BRIDGE_CACHE_LINE_SIZE 64

// Per-thread statistics (cache-line aligned to prevent false sharing)
struct thread_stats {
    atomic_ulong rx_packets;
    atomic_ulong tx_packets;
    atomic_ulong dropped;
    atomic_ulong ring_full;
    atomic_ulong errors;
    char padding[BRIDGE_CACHE_LINE_SIZE - (5 * sizeof(atomic_ulong))];
} __attribute__((aligned(BRIDGE_CACHE_LINE_SIZE)));

// Packet statistics (cache-line aligned)
static struct {
    struct thread_stats per_thread[2];
    time_t start_time;
} stats;

static uint8_t iface_mac[2][ETH_ALEN];
static uint32_t iface_ipv4[2]; // network order, 0 if unset
static int enable_mac_filter = 0;
static int enable_ip_filter = 0;

// TPACKET_V3 설정 (성능 최적화)
// - 처리량(iperf) 목표면 블록/링을 키우고 retire timeout을 늘릴 수 있음
// - 레이턴시(ping) 목표면 retire timeout을 줄이는 것이 핵심
// - 8MB로 증가하여 버스트 트래픽 처리 능력 향상
#define BLOCK_SIZE (64 * 1024)         // 64KB blocks
#define FRAME_SIZE 2048                // 2KB per frame
#define BLOCK_NR 128                   // 128 blocks = 8MB ring (최적화: 4MB → 8MB)
#define FRAME_NR ((BLOCK_SIZE * BLOCK_NR) / FRAME_SIZE)
#define RETIRE_TIMEOUT_MS 1            // 최적화: 2ms → 1ms (더 낮은 레이턴시)

// TX_RING 설정(TPACKET_V2 기반)
// - TPACKET_V3는 RX 쪽에 유리하고, TX_RING은 커널/환경에 따라 V2가 가장 호환성이 좋음
// - TX_RING 실패 시 sendto()로 자동 fallback
#define TX_BLOCK_SIZE (64 * 1024)
#define TX_FRAME_SIZE 2048
#define TX_BLOCK_NR 128                // 8MB (최적화: 4MB → 8MB)
#define TX_FRAME_NR ((TX_BLOCK_SIZE * TX_BLOCK_NR) / TX_FRAME_SIZE)

// Interface 구조체
struct interface {
    char name[IFNAMSIZ];
    int ifindex;
    int sock_rx;         // RX socket (TPACKET_V3)
    int sock_tx;         // TX socket (raw)
    void *ring_rx;       // mmap'd RX ring buffer
    size_t ring_size;
    struct tpacket_req3 req;
    // TX ring (optional)
    void *ring_tx;
    size_t ring_tx_size;
    struct tpacket_req tx_req;
    unsigned int tx_frame_idx;
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
        unsigned long rx = atomic_load(&stats.per_thread[i].rx_packets);
        unsigned long tx = atomic_load(&stats.per_thread[i].tx_packets);
        unsigned long drop = atomic_load(&stats.per_thread[i].dropped);
        unsigned long ring_full = atomic_load(&stats.per_thread[i].ring_full);
        unsigned long err = atomic_load(&stats.per_thread[i].errors);

        fprintf(stderr, "  Interface %d (%s):\n", i, interfaces[i].name);
        fprintf(stderr, "    RX:        %10lu packets (%lu pps)\n", rx, uptime > 0 ? rx/uptime : 0);
        fprintf(stderr, "    TX:        %10lu packets (%lu pps)\n", tx, uptime > 0 ? tx/uptime : 0);
        fprintf(stderr, "    Dropped:   %10lu packets\n", drop);
        fprintf(stderr, "    RingFull:  %10lu events\n", ring_full);
        fprintf(stderr, "    Errors:    %10lu\n", err);
    }
    fprintf(stderr, "==========================================\n");

    syslog(LOG_INFO, "Stats: %s rx=%lu tx=%lu drop=%lu | %s rx=%lu tx=%lu drop=%lu",
           interfaces[0].name, atomic_load(&stats.per_thread[0].rx_packets),
           atomic_load(&stats.per_thread[0].tx_packets), atomic_load(&stats.per_thread[0].dropped),
           interfaces[1].name, atomic_load(&stats.per_thread[1].rx_packets),
           atomic_load(&stats.per_thread[1].tx_packets), atomic_load(&stats.per_thread[1].dropped));
}

static int get_mac(const char *ifname, uint8_t mac[ETH_ALEN]) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) != 0) {
        close(fd);
        return -1;
    }
    memcpy(mac, ifr.ifr_hwaddr.sa_data, ETH_ALEN);
    close(fd);
    return 0;
}

static int is_local_ipv4(uint32_t ip) {
    for (int k = 0; k < 2; ++k) {
        if (iface_ipv4[k] != 0 && iface_ipv4[k] == ip) return 1;
    }
    return 0;
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
    iface->req.tp_retire_blk_tov = RETIRE_TIMEOUT_MS;
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

    // Outgoing 프레임을 다시 캡처해 재전송하는 루프를 줄이기 위해(지원되는 경우)
    int ignore_outgoing = 1;
    if (setsockopt(iface->sock_rx, SOL_PACKET, PACKET_IGNORE_OUTGOING, &ignore_outgoing, sizeof(ignore_outgoing)) < 0) {
        // 커널/헤더에 따라 미지원일 수 있으니 경고만
        fprintf(stderr, "WARNING: PACKET_IGNORE_OUTGOING failed on %s: %s\n", iface->name, strerror(errno));
    }

    fprintf(stderr, "Interface %s: TPACKET_V3 ring setup complete (%zu bytes, %u blocks, retire=%dms)\n",
            iface->name, iface->ring_size, iface->req.tp_block_nr, RETIRE_TIMEOUT_MS);

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

static int setup_tx_ring(struct interface *iface)
{
    // TX_RING은 TPACKET_V2로 호환성 확보
    int version = TPACKET_V2;
    if (setsockopt(iface->sock_tx, SOL_PACKET, PACKET_VERSION, &version, sizeof(version)) < 0) {
        fprintf(stderr, "WARNING: setsockopt(PACKET_VERSION=TPACKET_V2) failed on %s: %s\n", iface->name, strerror(errno));
        return -1;
    }

    memset(&iface->tx_req, 0, sizeof(iface->tx_req));
    iface->tx_req.tp_block_size = TX_BLOCK_SIZE;
    iface->tx_req.tp_frame_size = TX_FRAME_SIZE;
    iface->tx_req.tp_block_nr = TX_BLOCK_NR;
    iface->tx_req.tp_frame_nr = TX_FRAME_NR;

    if (setsockopt(iface->sock_tx, SOL_PACKET, PACKET_TX_RING, &iface->tx_req, sizeof(iface->tx_req)) < 0) {
        fprintf(stderr, "WARNING: setsockopt(PACKET_TX_RING) failed on %s: %s\n", iface->name, strerror(errno));
        return -1;
    }

    iface->ring_tx_size = (size_t)iface->tx_req.tp_block_size * (size_t)iface->tx_req.tp_block_nr;
    iface->ring_tx = mmap(NULL, iface->ring_tx_size, PROT_READ | PROT_WRITE, MAP_SHARED, iface->sock_tx, 0);
    if (iface->ring_tx == MAP_FAILED) {
        iface->ring_tx = NULL;
        fprintf(stderr, "WARNING: mmap(TX_RING) failed on %s: %s\n", iface->name, strerror(errno));
        return -1;
    }

    iface->tx_frame_idx = 0;

    fprintf(stderr,
            "Interface %s: TX_RING setup complete (%zu bytes, %u blocks)\n",
            iface->name,
            iface->ring_tx_size,
            iface->tx_req.tp_block_nr);

    return 0;
}

static inline struct tpacket2_hdr *tx_frame_ptr(const struct interface *iface, unsigned int frame_idx)
{
    return (struct tpacket2_hdr *)((uint8_t *)iface->ring_tx + (frame_idx * iface->tx_req.tp_frame_size));
}

static inline int tx_frame_is_available(const struct tpacket2_hdr *hdr)
{
    // kernel doc / selftests: availability is "not (SEND_REQUEST|SENDING)"
    return !(hdr->tp_status & (TP_STATUS_SEND_REQUEST | TP_STATUS_SENDING));
}

static int tx_ring_enqueue(unsigned int tx_idx, struct interface *tx_iface, const uint8_t *pkt, uint32_t pkt_len)
{
    if (!tx_iface->ring_tx) {
        return -1;
    }

    struct tpacket2_hdr *hdr = tx_frame_ptr(tx_iface, tx_iface->tx_frame_idx);

    // 사용 가능한 프레임이 아니면 드롭(또는 바쁜 대기할 수 있지만, 레이턴시 우선으로 드롭)
    if (!tx_frame_is_available(hdr)) {
        atomic_fetch_add(&stats.per_thread[tx_idx].ring_full, 1);
        return -2;
    }

    // kernel doc/selftests: payload starts at TPACKET2_HDRLEN - sizeof(sockaddr_ll)
    const uint32_t data_off = (uint32_t)(TPACKET2_HDRLEN - sizeof(struct sockaddr_ll));
    const uint32_t max_data_len = (uint32_t)tx_iface->tx_req.tp_frame_size - data_off;
    uint8_t *data = (uint8_t *)hdr + data_off;
    if (pkt_len > max_data_len) {
        return -3;
    }

    memcpy(data, pkt, pkt_len);
    hdr->tp_len = pkt_len;
    hdr->tp_snaplen = pkt_len;
    hdr->tp_status = TP_STATUS_SEND_REQUEST;

    // 다음 프레임으로
    tx_iface->tx_frame_idx = (tx_iface->tx_frame_idx + 1) % tx_iface->tx_req.tp_frame_nr;

    return 0;
}

static int tx_ring_flush(struct interface *tx_iface)
{
    if (!tx_iface->ring_tx) {
        return 0;
    }
    // 커널에 전송 요청 flush (0바이트 sendto)
    if (sendto(tx_iface->sock_tx, NULL, 0, 0, NULL, 0) < 0) {
        return -1;
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
        fprintf(stderr, "Thread %u set to SCHED_FIFO priority 50\n", idx);
    }

    fprintf(stderr, "Thread %u: RX from %s, TX to %s\n", idx, rx_iface->name, tx_iface->name);

    // TPACKET_V3 block descriptor 순회
    unsigned int block_idx = 0;
    struct pollfd pfd;
    pfd.fd = rx_iface->sock_rx;
    pfd.events = POLLIN | POLLERR;
    pfd.revents = 0;

    while (keep_running) {
        // Block이 준비될 때까지 poll (timeout은 retire timeout보다 조금 크게)
        int ret = poll(&pfd, 1, 10);
        if (ret < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "ERROR: poll() failed: %s\n", strerror(errno));
            atomic_fetch_add(&stats.per_thread[idx].errors, 1);
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

        // Block 내의 모든 패킷 처리 (배치 최적화: atomic 연산 최소화)
        unsigned int num_pkts = pbd->hdr.bh1.num_pkts;
        struct tpacket3_hdr *ppd = (struct tpacket3_hdr *)((uint8_t *)pbd + pbd->hdr.bh1.offset_to_first_pkt);

        // 로컬 카운터 (배치 업데이트용)
        uint64_t local_rx_count = 0;
        uint64_t local_tx_count = 0;
        uint64_t local_drop_count = 0;
        uint64_t local_error_count = 0;

        int tx_flush_needed = 0;
        for (unsigned int i = 0; i < num_pkts; i++) {
            // 패킷 데이터
            uint8_t *pkt_data = (uint8_t *)ppd + ppd->tp_mac;
            uint32_t pkt_len = ppd->tp_snaplen;

            // RX 카운터 (로컬)
            local_rx_count++;

            if (pkt_len >= sizeof(struct ethhdr)) {
                const struct ethhdr *eh = (const struct ethhdr *)pkt_data;

                // MAC 필터: 목적지가 self/peer이면 재주입 생략 (옵션)
                if (enable_mac_filter) {
                    if (memcmp(eh->h_dest, iface_mac[idx], ETH_ALEN) == 0 ||
                        memcmp(eh->h_dest, iface_mac[idx ^ 1], ETH_ALEN) == 0) {
                        goto next_pkt;
                    }
                }

                // IP 필터: 목적지 IPv4가 로컬이면 재주입 생략 (옵션)
                if (enable_ip_filter && eh->h_proto == htons(ETH_P_IP)) {
                    if (pkt_len >= sizeof(struct ethhdr) + sizeof(struct iphdr)) {
                        const struct iphdr *ip4 = (const struct iphdr *)(pkt_data + sizeof(struct ethhdr));
                        if (is_local_ipv4(ip4->daddr)) {
                            goto next_pkt;
                        }
                    }
                }
            }

            // TX (sendto)
            const struct ethhdr *eh = (const struct ethhdr *)pkt_data;
            int tx_rc = tx_ring_enqueue(idx ^ 1, tx_iface, pkt_data, pkt_len);
            if (tx_rc == 0) {
                local_tx_count++;
                tx_flush_needed = 1;
            } else if (tx_rc == -1) {
                // TX_RING 미사용: sendto() fallback
                struct sockaddr_ll dest_addr;
                memset(&dest_addr, 0, sizeof(dest_addr));
                dest_addr.sll_family = AF_PACKET;
                dest_addr.sll_protocol = eh->h_proto;
                dest_addr.sll_ifindex = tx_iface->ifindex;
                dest_addr.sll_halen = ETH_ALEN;
                memcpy(dest_addr.sll_addr, eh->h_dest, ETH_ALEN);

                ssize_t sent = sendto(tx_iface->sock_tx, pkt_data, pkt_len, 0,
                                      (struct sockaddr *)&dest_addr, sizeof(dest_addr));
                if (sent < 0) {
                    local_drop_count++;
                    local_error_count++;

                    static atomic_ulong send_err_count[2];
                    unsigned long err_cnt = atomic_fetch_add(&send_err_count[idx], 1) + 1;
                    if (err_cnt == 1 || (err_cnt % 1000) == 0) {
                        fprintf(stderr,
                                "sendto(%s->%s) failed (count=%lu): %s\n",
                                rx_iface->name,
                                tx_iface->name,
                                err_cnt,
                                strerror(errno));
                    }
                } else {
                    local_tx_count++;
                }
            } else {
                // TX_RING 사용 중이지만 프레임이 없거나 전송 flush 실패 등
                local_drop_count++;
                local_error_count++;
            }

            // 다음 패킷으로
        next_pkt:
            ppd = (struct tpacket3_hdr *)((uint8_t *)ppd + ppd->tp_next_offset);
        }

        // 배치 업데이트: 블록당 한 번만 atomic 연산 (성능 최적화)
        atomic_fetch_add(&stats.per_thread[idx].rx_packets, local_rx_count);
        atomic_fetch_add(&stats.per_thread[idx ^ 1].tx_packets, local_tx_count);
        atomic_fetch_add(&stats.per_thread[idx].dropped, local_drop_count);
        atomic_fetch_add(&stats.per_thread[idx ^ 1].errors, local_error_count);

        if (tx_flush_needed) {
            if (tx_ring_flush(tx_iface) != 0) {
                atomic_fetch_add(&stats.per_thread[idx ^ 1].errors, 1);
            }
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

    enable_mac_filter = getenv("DUMB_MAC_FILTER") ? 1 : 0;
    enable_ip_filter = getenv("DUMB_IP_FILTER") ? 1 : 0;
    memset(iface_ipv4, 0, sizeof(iface_ipv4));
    memset(iface_mac, 0, sizeof(iface_mac));

    // syslog 초기화
    openlog("dumb-tpacket", LOG_PID | LOG_CONS, LOG_LOCAL0);

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

        // TX_RING 설정(실패하면 sendto fallback)
        if (setup_tx_ring(&interfaces[i]) < 0) {
            fprintf(stderr, "WARNING: %s TX_RING disabled, falling back to sendto()\n", interfaces[i].name);
        }

        // MAC 저장 (필터 옵션용)
        if (get_mac(interfaces[i].name, iface_mac[i]) != 0) {
            fprintf(stderr, "WARN: failed to read MAC for %s\n", interfaces[i].name);
        }
    }

    // IPv4 수집 (필요 시)
    if (enable_ip_filter) {
        struct ifaddrs *ifa = NULL;
        if (getifaddrs(&ifa) == 0) {
            for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
                if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
                for (int i = 0; i < 2; ++i) {
                    if (strncmp(p->ifa_name, interfaces[i].name, IFNAMSIZ) == 0) {
                        struct sockaddr_in *sin = (struct sockaddr_in *)p->ifa_addr;
                        iface_ipv4[i] = sin->sin_addr.s_addr;
                    }
                }
            }
            freeifaddrs(ifa);
        } else {
            fprintf(stderr, "WARN: getifaddrs failed: %s (ip-filter may be ineffective)\n", strerror(errno));
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

        // Ring buffer 사전 fault-in (페이지 폴트 방지)
        // mlockall()은 미래 할당을 잠그지만 실제 페이지는 첫 접근 시 할당됨
        // 여기서 모든 페이지를 미리 터치하여 런타임 페이지 폴트 제거
        for (int i = 0; i < 2; i++) {
            // RX ring pre-fault
            if (interfaces[i].ring_rx) {
                volatile uint8_t *ring = (volatile uint8_t *)interfaces[i].ring_rx;
                for (size_t p = 0; p < interfaces[i].ring_size; p += 4096) {
                    ring[p] = ring[p];  // 읽기로 페이지 fault-in
                }
            }
            // TX ring pre-fault
            if (interfaces[i].ring_tx) {
                volatile uint8_t *ring = (volatile uint8_t *)interfaces[i].ring_tx;
                for (size_t p = 0; p < interfaces[i].ring_tx_size; p += 4096) {
                    ring[p] = ring[p];  // 읽기로 페이지 fault-in
                }
            }
        }
        fprintf(stderr, "Ring buffers pre-faulted to eliminate runtime page faults\n");
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
        if (interfaces[i].ring_tx) {
            munmap(interfaces[i].ring_tx, interfaces[i].ring_tx_size);
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
