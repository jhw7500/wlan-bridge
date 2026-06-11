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

// 802.1X EAPOL ethertype — 일부 libc 헤더에 없을 수 있어 fallback 정의
#ifndef ETH_P_PAE
#define ETH_P_PAE 0x888E
#endif

/**
 * IEEE 802.1D Bridge Group Address: 01:80:C2:00:00:00 .. 01:80:C2:00:00:0F
 * (STP BPDU, LACP, 802.1X group, LLDP 등 link-local 프로토콜 목적지).
 * 802.1D-2004 §7.12.6: 브릿지는 이 범위를 절대 포워딩하면 안 된다 —
 * 포워딩 시 토폴로지 루프 형성 또는 STP 혼란 유발.
 * (moal_bridge_is_link_local()과 동일 범위 — 엔진 간 패리티)
 */
static int dst_is_8021d_link_local(const struct packet_info *pkt)
{
    const uint8_t *d = pkt->eth->h_dest;
    return d[0] == 0x01 && d[1] == 0x80 && d[2] == 0xC2 &&
           d[3] == 0x00 && d[4] == 0x00 && (d[5] & 0xF0) == 0x00;
}

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

    // 802.1X EAPOL (0x888E): 포트-로컬 프로토콜 — 절대 브릿징하지 않는다.
    // 4-way handshake 프레임은 unicast(STA MAC)로도 오므로 ethertype으로 판정.
    // packet_parse가 802.1Q를 벗긴 inner ethertype이라 VLAN-tagged도 잡힘.
    // enable_* 플래그와 무관하게 무조건 적용 (안전 불변식 — moal 가드 패리티).
    if (pkt->ethertype == ETH_P_PAE) {
        if (filter->config->enable_debug_log) {
            filter_debug_log("eapol-filter: dropped EAPOL frame (never bridged)");
        }
        return 1;
    }

    // IEEE 802.1D link-local (01:80:C2:00:00:00..0F): 절대 포워딩 금지 —
    // STP/LACP/LLDP. EAPOL group 주소(:03)도 이 범위에 포함된다.
    // enable_* 플래그와 무관하게 무조건 적용.
    if (dst_is_8021d_link_local(pkt)) {
        if (filter->config->enable_debug_log) {
            filter_debug_log("ll-filter: dropped 802.1D link-local frame "
                             "(dst 01:80:C2:00:00:%02x)", pkt->eth->h_dest[5]);
        }
        return 1;
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
