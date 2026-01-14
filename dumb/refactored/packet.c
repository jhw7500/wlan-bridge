/**
 * @file packet.c
 * @brief Packet parsing implementation
 */

#include "packet.h"
#include <string.h>
#include <arpa/inet.h>

int packet_parse(const struct pcap_pkthdr *hdr,
                 const uint8_t *data,
                 struct packet_info *pkt)
{
    if (!hdr || !data || !pkt) {
        return -1;
    }

    memset(pkt, 0, sizeof(*pkt));

    // Minimum size check
    if (hdr->caplen < sizeof(struct ethhdr)) {
        return -1;
    }

    // Store raw data and sizes
    pkt->data = data;
    pkt->caplen = hdr->caplen;
    pkt->len = hdr->len;

    // Parse Ethernet header
    pkt->eth = (const struct ethhdr *)data;

    // Check for multicast/broadcast (LSB of first byte)
    pkt->is_multicast = (pkt->eth->h_dest[0] & 0x01) ? 1 : 0;

    // Parse EtherType and VLAN
    uint16_t ethertype = ntohs(pkt->eth->h_proto);
    const uint8_t *payload = data + sizeof(struct ethhdr);
    size_t payload_len = hdr->caplen - sizeof(struct ethhdr);

    // Handle 802.1Q VLAN tag
    if (ethertype == ETH_P_8021Q && payload_len >= sizeof(struct vlan_hdr)) {
        const struct vlan_hdr *vlan = (const struct vlan_hdr *)payload;

        pkt->has_vlan = 1;
        pkt->vlan_id = ntohs(vlan->h_vlan_TCI) & 0x0FFF; // Extract VLAN ID

        ethertype = ntohs(vlan->h_vlan_encapsulated_proto);
        payload += sizeof(struct vlan_hdr);
        payload_len -= sizeof(struct vlan_hdr);
    }

    pkt->ethertype = ethertype;
    pkt->l2_payload = payload;
    pkt->l2_payload_len = payload_len;

    return 0;
}
