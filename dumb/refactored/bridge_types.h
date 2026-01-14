/**
 * @file bridge_types.h
 * @brief Core type definitions for the L2 bridge
 *
 * This header defines the fundamental types used throughout the bridge,
 * promoting type safety and reducing magic numbers.
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
#define BRIDGE_NAME "dumb-bridge"
#define BRIDGE_FEATURES "VLAN(802.1Q), IP-Filter, MAC-Filter, RT-Scheduling, Modular"

// Interface enumeration (replaces magic 0/1)
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

// 802.1Q VLAN header (unchanged, but documented)
struct vlan_hdr {
    uint16_t h_vlan_TCI;              // Tag Control Information
    uint16_t h_vlan_encapsulated_proto; // Encapsulated EtherType
} __attribute__((packed));

// Packet information (extracted from raw data)
struct packet_info {
    const uint8_t *data;              // Raw packet data
    uint32_t caplen;                  // Captured length
    uint32_t len;                     // Original length
    uint16_t ethertype;               // EtherType (after VLAN if present)
    const struct ethhdr *eth;         // Ethernet header
    const uint8_t *l2_payload;        // L2 payload (after Ethernet/VLAN)
    size_t l2_payload_len;            // L2 payload length

    // Flags
    uint8_t is_multicast : 1;         // Multicast/broadcast packet
    uint8_t has_vlan : 1;             // Has 802.1Q VLAN tag
    uint8_t _reserved : 6;

    uint16_t vlan_id;                 // VLAN ID (if has_vlan)
};

// Bridge configuration (extracted from global cfg)
struct bridge_config {
    // Performance tuning
    int dispatch_budget;              // Max packets per pcap_dispatch
    int snaplen;                      // Pcap snaplen bytes
    int pcap_buffer_bytes;            // Pcap RX buffer size
    int timeout_ms;                   // Pcap timeout ms

    // Feature flags
    uint8_t enable_affinity : 1;      // CPU pinning
    uint8_t enable_rt : 1;            // Real-time scheduling
    uint8_t enable_mlock : 1;         // Memory locking
    uint8_t enable_immediate : 1;     // Pcap immediate mode
    uint8_t enable_promisc : 1;       // Promiscuous mode
    uint8_t enable_mac_filter : 1;    // MAC-based filtering
    uint8_t enable_ip_filter : 1;     // IP-based filtering
    uint8_t enable_debug_log : 1;     // Verbose logging

    int rt_priority;                  // SCHED_FIFO priority (1-99)
};

// Per-interface statistics
struct bridge_interface_stats {
    atomic_ulong rx_packets;          // Received packets
    atomic_ulong tx_packets;          // Transmitted packets
    atomic_ulong dropped;             // Dropped (inject failed)
    atomic_ulong pcap_drop;           // Dropped by pcap (kernel overflow)
    atomic_ulong errors;              // Other errors
};

// Bridge statistics
struct bridge_stats {
    struct bridge_interface_stats iface[BRIDGE_IF_COUNT];
    time_t start_time;                // Program start time
};

// Per-interface state
struct bridge_interface {
    char name[IFNAMSIZ];              // Interface name
    pcap_t *rx_handle;                // RX pcap handle
    pcap_t *tx_handle;                // TX pcap handle
    pthread_t thread;                 // Worker thread
    uint8_t mac[ETH_ALEN];            // MAC address
    uint32_t ipv4;                    // IPv4 address (network byte order)
    atomic_int ready;                 // Interface ready flag
};

// Packet filter state
struct packet_filter {
    const struct bridge_config *config; // Configuration reference
    const struct bridge_interface *interfaces; // Interface array
};

// Forward declarations for opaque context
struct bridge_context;

#endif // BRIDGE_TYPES_H
