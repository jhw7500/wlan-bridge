/**
 * @file filter.c
 * @brief Packet filtering implementation
 */

#include "filter.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <netinet/ip.h>
#include <net/if_arp.h>
#include <arpa/inet.h>

// Rate-limited debug logging.
// caller가 __FILE__/__LINE__ 정보를 매크로로 자동 전달 → "[file:line] msg" 형식.
static void filter_debug_log_impl(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void filter_debug_log_impl(const char *file, int line, const char *fmt, ...) {
    static atomic_ulong log_count = 0;
    static time_t last_log_time = 0;

    unsigned long count = atomic_fetch_add(&log_count, 1) + 1;
    time_t now = time(NULL);

    // Log first occurrence, then every 1000 hits or every second
    if (count == 1 || count % 1000 == 0 || (last_log_time > 0 && now > last_log_time)) {
        char prefixed_fmt[512];
        snprintf(prefixed_fmt, sizeof(prefixed_fmt), "[%s:%d] %s", file, line, fmt);
        va_list args;
        va_start(args, fmt);
        vsyslog(LOG_DEBUG, prefixed_fmt, args);
        va_end(args);
        last_log_time = now;
    }
}

#define filter_debug_log(fmt, ...) \
    filter_debug_log_impl(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

void filter_init(struct packet_filter *filter,
                const struct bridge_config *config,
                const struct bridge_interface *interfaces)
{
    if (!filter) return;

    memset(filter, 0, sizeof(*filter));
    filter->config = config;
    filter->interfaces = interfaces;
}

int filter_mac_is_self_or_peer(const struct packet_filter *filter,
                                const struct packet_info *pkt,
                                bridge_interface_t iface_idx)
{
    if (!filter || !pkt || !bridge_interface_valid(iface_idx)) {
        return 0;
    }

    if (!filter->config->enable_mac_filter) {
        return 0; // Filter disabled
    }

    const uint8_t *dst_mac = pkt->eth->h_dest;
    bridge_interface_t peer_idx = bridge_peer(iface_idx);

    // Check self MAC
    if (memcmp(dst_mac, filter->interfaces[iface_idx].mac, ETH_ALEN) == 0) {
        if (filter->config->enable_debug_log) {
            filter_debug_log("mac-filter: dropped packet to self MAC on if%d", iface_idx);
        }
        return 1;
    }

    // Check peer MAC
    if (memcmp(dst_mac, filter->interfaces[peer_idx].mac, ETH_ALEN) == 0) {
        if (filter->config->enable_debug_log) {
            filter_debug_log("mac-filter: dropped packet to peer MAC on if%d", iface_idx);
        }
        return 1;
    }

    return 0;
}

int filter_ip_is_local(const struct packet_filter *filter,
                       const struct packet_info *pkt)
{
    if (!filter || !pkt) {
        return 0;
    }

    if (!filter->config->enable_ip_filter) {
        return 0; // Filter disabled
    }

    // Only check IPv4 unicast packets
    if (pkt->ethertype != ETH_P_IP) {
        return 0;
    }

    if (pkt->l2_payload_len < sizeof(struct iphdr)) {
        return 0; // Too short
    }

    const struct iphdr *ip4 = (const struct iphdr *)pkt->l2_payload;

    // Skip IPv4 multicast (224.0.0.0/4)
    uint32_t dst_ip_host = ntohl(ip4->daddr);
    if (dst_ip_host >= 0xE0000000 && dst_ip_host <= 0xEFFFFFFF) {
        return 0; // Multicast, should forward
    }

    // Check if destination IP matches any bridge interface
    for (int i = 0; i < BRIDGE_IF_COUNT; i++) {
        if (filter->interfaces[i].ipv4 != 0 &&
            filter->interfaces[i].ipv4 == ip4->daddr) {

            if (filter->config->enable_debug_log) {
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &ip4->daddr, ip_str, sizeof(ip_str));
                filter_debug_log("ip-filter: dropped packet to local IP %s", ip_str);
            }
            return 1;
        }
    }

    return 0;
}

int filter_arp_is_for_bridge(const struct packet_filter *filter,
                             const struct packet_info *pkt)
{
    if (!filter || !pkt) {
        return 0;
    }

    if (!filter->config->enable_ip_filter) {
        return 0; // Filter disabled
    }

    // Only check ARP packets
    if (pkt->ethertype != ETH_P_ARP) {
        return 0;
    }

    // Minimum ARP packet size: arphdr(8) + addresses(6+4+6+4=20) = 28
    const size_t min_arp_size = 28;
    if (pkt->l2_payload_len < min_arp_size) {
        return 0;
    }

    const struct arphdr *arp = (const struct arphdr *)pkt->l2_payload;

    // Check for IPv4 over Ethernet ARP only
    if (ntohs(arp->ar_hrd) != ARPHRD_ETHER ||
        ntohs(arp->ar_pro) != ETH_P_IP ||
        arp->ar_hln != ETH_ALEN ||
        arp->ar_pln != 4) {
        return 0;
    }

    // ARP packet structure: [arphdr 8] [sender_mac 6] [sender_ip 4] [target_mac 6] [target_ip 4]
    const uint8_t *arp_data = pkt->l2_payload + sizeof(struct arphdr);

    // Extract target IP (offset: sender_mac(6) + sender_ip(4) + target_mac(6) = 16)
    uint32_t target_ip;
    memcpy(&target_ip, arp_data + 16, 4);

    // Check if target IP matches any bridge interface
    for (int i = 0; i < BRIDGE_IF_COUNT; i++) {
        if (filter->interfaces[i].ipv4 != 0 &&
            filter->interfaces[i].ipv4 == target_ip) {

            if (filter->config->enable_debug_log) {
                char ip_str[INET_ADDRSTRLEN];
                struct in_addr addr = {.s_addr = target_ip};
                inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
                filter_debug_log("arp-filter: dropped ARP for bridge IP %s", ip_str);
            }
            return 1;
        }
    }

    return 0;
}

int filter_should_drop(const struct packet_filter *filter,
                       const struct packet_info *pkt,
                       bridge_interface_t iface_idx)
{
    if (!filter || !pkt || !bridge_interface_valid(iface_idx)) {
        return 1; // Drop invalid packets
    }

    // L2 bridge principle: Always forward multicast/broadcast
    // (except for specific ARP requests targeting the bridge itself)
    if (packet_is_multicast(pkt)) {
        // Special case: ARP for bridge IP should not be forwarded
        if (pkt->ethertype == ETH_P_ARP &&
            filter_arp_is_for_bridge(filter, pkt)) {
            return 1; // Drop, let kernel handle
        }
        return 0; // Forward all other multicast/broadcast
    }

    // Unicast filtering (optional)

    // MAC filter: Drop if destination is self or peer MAC
    if (filter_mac_is_self_or_peer(filter, pkt, iface_idx)) {
        return 1;
    }

    // IP filter: Drop if destination is local IP
    if (filter_ip_is_local(filter, pkt)) {
        return 1;
    }

    // Forward packet
    return 0;
}
