/**
 * @file test_filter.c
 * @brief Unit tests for packet filtering logic
 *
 * Demonstrates how modular code enables comprehensive testing.
 */

#include "../filter.h"
#include "../packet.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <netinet/ip.h>
#include <arpa/inet.h>

// Test helper: Create mock bridge configuration
static struct bridge_config create_test_config(int enable_mac, int enable_ip) {
    struct bridge_config cfg = {0};
    cfg.enable_mac_filter = enable_mac;
    cfg.enable_ip_filter = enable_ip;
    cfg.enable_debug_log = 0; // Disable logs during tests
    return cfg;
}

// Test helper: Create mock interface with MAC and IP
static void setup_test_interface(struct bridge_interface *iface,
                                const char *name,
                                const uint8_t mac[ETH_ALEN],
                                const char *ipv4_str)
{
    memset(iface, 0, sizeof(*iface));
    strncpy(iface->name, name, IFNAMSIZ - 1);
    memcpy(iface->mac, mac, ETH_ALEN);

    if (ipv4_str) {
        struct in_addr addr;
        inet_pton(AF_INET, ipv4_str, &addr);
        iface->ipv4 = addr.s_addr; // network byte order
    }
}

// Test helper: Create Ethernet packet with specific destination MAC
static void create_test_packet_mac(struct packet_info *pkt,
                                   const uint8_t dst_mac[ETH_ALEN],
                                   int is_multicast)
{
    static uint8_t packet_data[128];
    static struct ethhdr eth;

    memset(&eth, 0, sizeof(eth));
    memcpy(eth.h_dest, dst_mac, ETH_ALEN);
    eth.h_proto = htons(ETH_P_IP); // IPv4

    memcpy(packet_data, &eth, sizeof(eth));

    memset(pkt, 0, sizeof(*pkt));
    pkt->data = packet_data;
    pkt->caplen = sizeof(eth) + 20; // Eth + minimal IP
    pkt->eth = (const struct ethhdr *)packet_data;
    pkt->ethertype = ETH_P_IP;
    pkt->is_multicast = is_multicast;
    pkt->l2_payload = packet_data + sizeof(eth);
    pkt->l2_payload_len = 20;
}

// Test helper: Create IPv4 packet with specific destination IP
static void create_test_packet_ipv4(struct packet_info *pkt,
                                    const char *dst_ip_str)
{
    static uint8_t packet_data[128];
    static struct ethhdr eth;
    static struct iphdr ip;

    // Ethernet header
    memset(&eth, 0, sizeof(eth));
    eth.h_dest[0] = 0x00; // Unicast
    eth.h_proto = htons(ETH_P_IP);

    // IPv4 header
    memset(&ip, 0, sizeof(ip));
    ip.version = 4;
    ip.ihl = 5;
    ip.ttl = 64;
    ip.protocol = IPPROTO_TCP;

    struct in_addr dst_addr;
    inet_pton(AF_INET, dst_ip_str, &dst_addr);
    ip.daddr = dst_addr.s_addr;

    memcpy(packet_data, &eth, sizeof(eth));
    memcpy(packet_data + sizeof(eth), &ip, sizeof(ip));

    memset(pkt, 0, sizeof(*pkt));
    pkt->data = packet_data;
    pkt->caplen = sizeof(eth) + sizeof(ip);
    pkt->eth = (const struct ethhdr *)packet_data;
    pkt->ethertype = ETH_P_IP;
    pkt->is_multicast = 0;
    pkt->l2_payload = packet_data + sizeof(eth);
    pkt->l2_payload_len = sizeof(ip);
}

//
// TEST CASES
//

void test_multicast_should_always_forward(void) {
    printf("TEST: Multicast packets should always forward\n");

    struct bridge_config cfg = create_test_config(1, 1);
    struct bridge_interface ifaces[BRIDGE_IF_COUNT];

    uint8_t mac0[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    setup_test_interface(&ifaces[0], "eth0", mac0, "192.168.1.10");
    setup_test_interface(&ifaces[1], "eth1", mac0, "192.168.1.11");

    struct packet_filter filter;
    filter_init(&filter, &cfg, ifaces);

    // Create multicast packet (broadcast MAC: ff:ff:ff:ff:ff:ff)
    uint8_t broadcast_mac[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    struct packet_info pkt;
    create_test_packet_mac(&pkt, broadcast_mac, 1);

    // Should NOT be dropped
    int dropped = filter_should_drop(&filter, &pkt, BRIDGE_IF0);
    assert(dropped == 0);

    printf("  ✓ PASS\n");
}

void test_mac_filter_drops_self_mac(void) {
    printf("TEST: MAC filter should drop packets to self MAC\n");

    struct bridge_config cfg = create_test_config(1, 0); // MAC filter ON
    struct bridge_interface ifaces[BRIDGE_IF_COUNT];

    uint8_t mac0[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t mac1[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    setup_test_interface(&ifaces[0], "eth0", mac0, NULL);
    setup_test_interface(&ifaces[1], "eth1", mac1, NULL);

    struct packet_filter filter;
    filter_init(&filter, &cfg, ifaces);

    // Create packet destined to eth0's MAC
    struct packet_info pkt;
    create_test_packet_mac(&pkt, mac0, 0);

    // Should be dropped (packet received on eth0 with dst=eth0)
    int dropped = filter_should_drop(&filter, &pkt, BRIDGE_IF0);
    assert(dropped == 1);

    printf("  ✓ PASS\n");
}

void test_mac_filter_drops_peer_mac(void) {
    printf("TEST: MAC filter should drop packets to peer MAC\n");

    struct bridge_config cfg = create_test_config(1, 0);
    struct bridge_interface ifaces[BRIDGE_IF_COUNT];

    uint8_t mac0[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t mac1[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    setup_test_interface(&ifaces[0], "eth0", mac0, NULL);
    setup_test_interface(&ifaces[1], "eth1", mac1, NULL);

    struct packet_filter filter;
    filter_init(&filter, &cfg, ifaces);

    // Create packet destined to eth1's MAC (peer of eth0)
    struct packet_info pkt;
    create_test_packet_mac(&pkt, mac1, 0);

    // Should be dropped
    int dropped = filter_should_drop(&filter, &pkt, BRIDGE_IF0);
    assert(dropped == 1);

    printf("  ✓ PASS\n");
}

void test_mac_filter_forwards_other_mac(void) {
    printf("TEST: MAC filter should forward packets to other MAC\n");

    struct bridge_config cfg = create_test_config(1, 0);
    struct bridge_interface ifaces[BRIDGE_IF_COUNT];

    uint8_t mac0[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t mac1[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    uint8_t other_mac[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    setup_test_interface(&ifaces[0], "eth0", mac0, NULL);
    setup_test_interface(&ifaces[1], "eth1", mac1, NULL);

    struct packet_filter filter;
    filter_init(&filter, &cfg, ifaces);

    // Create packet destined to some other MAC
    struct packet_info pkt;
    create_test_packet_mac(&pkt, other_mac, 0);

    // Should NOT be dropped
    int dropped = filter_should_drop(&filter, &pkt, BRIDGE_IF0);
    assert(dropped == 0);

    printf("  ✓ PASS\n");
}

void test_ip_filter_drops_local_ip(void) {
    printf("TEST: IP filter should drop packets to local IP\n");

    struct bridge_config cfg = create_test_config(0, 1); // IP filter ON
    struct bridge_interface ifaces[BRIDGE_IF_COUNT];

    uint8_t mac0[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t mac1[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    setup_test_interface(&ifaces[0], "eth0", mac0, "192.168.1.10");
    setup_test_interface(&ifaces[1], "eth1", mac1, "192.168.1.11");

    struct packet_filter filter;
    filter_init(&filter, &cfg, ifaces);

    // Create packet destined to 192.168.1.10 (local IP)
    struct packet_info pkt;
    create_test_packet_ipv4(&pkt, "192.168.1.10");

    // Should be dropped
    int dropped = filter_should_drop(&filter, &pkt, BRIDGE_IF0);
    assert(dropped == 1);

    printf("  ✓ PASS\n");
}

void test_ip_filter_forwards_remote_ip(void) {
    printf("TEST: IP filter should forward packets to remote IP\n");

    struct bridge_config cfg = create_test_config(0, 1);
    struct bridge_interface ifaces[BRIDGE_IF_COUNT];

    uint8_t mac0[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t mac1[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    setup_test_interface(&ifaces[0], "eth0", mac0, "192.168.1.10");
    setup_test_interface(&ifaces[1], "eth1", mac1, "192.168.1.11");

    struct packet_filter filter;
    filter_init(&filter, &cfg, ifaces);

    // Create packet destined to 192.168.1.100 (remote IP)
    struct packet_info pkt;
    create_test_packet_ipv4(&pkt, "192.168.1.100");

    // Should NOT be dropped
    int dropped = filter_should_drop(&filter, &pkt, BRIDGE_IF0);
    assert(dropped == 0);

    printf("  ✓ PASS\n");
}

void test_ip_filter_forwards_ipv4_multicast(void) {
    printf("TEST: IP filter should forward IPv4 multicast\n");

    struct bridge_config cfg = create_test_config(0, 1);
    struct bridge_interface ifaces[BRIDGE_IF_COUNT];

    uint8_t mac0[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t mac1[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    setup_test_interface(&ifaces[0], "eth0", mac0, "192.168.1.10");
    setup_test_interface(&ifaces[1], "eth1", mac1, "192.168.1.11");

    struct packet_filter filter;
    filter_init(&filter, &cfg, ifaces);

    // Create packet destined to 224.0.0.1 (multicast)
    struct packet_info pkt;
    create_test_packet_ipv4(&pkt, "224.0.0.1");
    pkt.is_multicast = 1; // Mark as multicast

    // Should NOT be dropped
    int dropped = filter_should_drop(&filter, &pkt, BRIDGE_IF0);
    assert(dropped == 0);

    printf("  ✓ PASS\n");
}

void test_filter_disabled_forwards_all(void) {
    printf("TEST: With filters disabled, forward all unicast\n");

    struct bridge_config cfg = create_test_config(0, 0); // All filters OFF
    struct bridge_interface ifaces[BRIDGE_IF_COUNT];

    uint8_t mac0[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t mac1[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    setup_test_interface(&ifaces[0], "eth0", mac0, "192.168.1.10");
    setup_test_interface(&ifaces[1], "eth1", mac1, "192.168.1.11");

    struct packet_filter filter;
    filter_init(&filter, &cfg, ifaces);

    // Packet to self MAC - should forward (filter disabled)
    struct packet_info pkt1;
    create_test_packet_mac(&pkt1, mac0, 0);
    assert(filter_should_drop(&filter, &pkt1, BRIDGE_IF0) == 0);

    // Packet to local IP - should forward (filter disabled)
    struct packet_info pkt2;
    create_test_packet_ipv4(&pkt2, "192.168.1.10");
    assert(filter_should_drop(&filter, &pkt2, BRIDGE_IF0) == 0);

    printf("  ✓ PASS\n");
}

//
// TEST RUNNER
//

int main(void) {
    printf("\n========================================\n");
    printf("  Packet Filter Unit Tests\n");
    printf("========================================\n\n");

    test_multicast_should_always_forward();
    test_mac_filter_drops_self_mac();
    test_mac_filter_drops_peer_mac();
    test_mac_filter_forwards_other_mac();
    test_ip_filter_drops_local_ip();
    test_ip_filter_forwards_remote_ip();
    test_ip_filter_forwards_ipv4_multicast();
    test_filter_disabled_forwards_all();

    printf("\n========================================\n");
    printf("  All tests PASSED ✓\n");
    printf("========================================\n\n");

    return 0;
}
