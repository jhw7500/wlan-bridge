/**
 * @file bridge_types.h
 * @brief Core type definitions for the L2 bridge
 */

#ifndef BRIDGE_TYPES_H
#define BRIDGE_TYPES_H

#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#include <net/if.h>
#include <netinet/if_ether.h>
#include <pcap/pcap.h>
#include <pthread.h>
#include <syslog.h>

// rsyslog-style "[file:line]" prefix를 자동 부착하는 syslog 매크로.
// wifi_*.sh의 `logger -p ... "[$tag:$LINENO] ..."` 형식과 통일.
// __FILE__은 Makefile이 짧은 상대 경로 (예: "main.c")로 전달.
#define SLOG(prio, fmt, ...) \
    syslog((prio), "[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)

// Version information
#define BRIDGE_VERSION "2.0.0-refactored"
#define BRIDGE_NAME "wbridge"
#define BRIDGE_FEATURES "VLAN(802.1Q), IP-Filter, MAC-Filter, RT-Scheduling, Modular"

// Interface enumeration
typedef enum {
    BRIDGE_IF0 = 0,
    BRIDGE_IF1 = 1,
    BRIDGE_IF_COUNT = 2
} bridge_interface_t;

// Get peer interface
static inline bridge_interface_t bridge_peer(bridge_interface_t iface) {
    return (iface == BRIDGE_IF0) ? BRIDGE_IF1 : BRIDGE_IF0;
}

// Validate interface index
static inline int bridge_interface_valid(int idx) {
    return idx >= 0 && idx < BRIDGE_IF_COUNT;
}

// 802.1Q VLAN header
struct vlan_hdr {
    uint16_t h_vlan_TCI;
    uint16_t h_vlan_encapsulated_proto;
} __attribute__((packed));

// Packet information
struct packet_info {
    const uint8_t *data;
    uint32_t caplen;
    uint32_t len;
    uint16_t ethertype;
    const struct ethhdr *eth;
    const uint8_t *l2_payload;
    size_t l2_payload_len;

    uint8_t is_multicast : 1;
    uint8_t has_vlan : 1;
    uint8_t _reserved : 6;

    uint16_t vlan_id;
};

// Bridge configuration
struct bridge_config {
    int dispatch_budget;
    int snaplen;
    int pcap_buffer_bytes;
    int timeout_ms;

    uint8_t enable_affinity : 1;
    uint8_t enable_rt : 1;
    uint8_t enable_mlock : 1;
    uint8_t enable_immediate : 1;
    uint8_t enable_promisc : 1;
    uint8_t enable_mac_filter : 1;
    uint8_t enable_ip_filter : 1;
    uint8_t enable_debug_log : 1;

    int rt_priority;
};

// Per-interface statistics
struct bridge_interface_stats {
    atomic_ulong rx_packets;
    atomic_ulong tx_packets;
    atomic_ulong dropped;
    atomic_ulong pcap_drop;
    atomic_ulong errors;
};

// Bridge statistics
struct bridge_stats {
    struct bridge_interface_stats iface[BRIDGE_IF_COUNT];
    time_t start_time;
};

// Per-thread local counters (non-atomic, for batch update after pcap_dispatch)
struct dispatch_counters {
    unsigned long rx_packets;
    unsigned long tx_packets;
    unsigned long dropped;
    unsigned long errors;
};

// Per-interface state
struct bridge_interface {
    char name[IFNAMSIZ];
    pcap_t *rx_handle;
    // 송신 전용 raw AF_PACKET 소켓(SOCK_RAW, protocol=0). protocol=0 이라
    // ptype_all 등록이 없어 RX 링/skb clone 비용이 0. sendto() 로만 사용.
    int tx_fd;
    int ifindex;   // sendto() 의 sll_ifindex 지정용
    pthread_t thread;
    uint8_t mac[ETH_ALEN];
    uint32_t ipv4;
    atomic_int ready;
};

// Packet filter state
struct packet_filter {
    const struct bridge_config *config;
    const struct bridge_interface *interfaces;
};

// Bridge context
struct bridge_context {
    struct bridge_config config;
    struct bridge_stats stats;
    struct bridge_interface interfaces[BRIDGE_IF_COUNT];
    struct packet_filter filter;
    struct dispatch_counters local_counters[BRIDGE_IF_COUNT];
    atomic_int keep_running;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

#endif // BRIDGE_TYPES_H
