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

// Per-interface state
struct bridge_interface {
    char name[IFNAMSIZ];
    pcap_t *rx_handle;
    pcap_t *tx_handle;
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
    atomic_int keep_running;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

#endif // BRIDGE_TYPES_H
