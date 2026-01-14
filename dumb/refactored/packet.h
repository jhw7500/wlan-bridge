/**
 * @file packet.h
 * @brief Packet parsing and validation
 *
 * Extracts packet parsing logic from the monolithic packet handler.
 * Makes the code testable and easier to understand.
 */

#ifndef PACKET_H
#define PACKET_H

#include "bridge_types.h"
#include <pcap/pcap.h>

/**
 * Parse a raw packet into structured information
 *
 * @param hdr Pcap packet header
 * @param data Raw packet data
 * @param pkt Output packet information structure
 * @return 0 on success, -1 if packet is invalid
 */
int packet_parse(const struct pcap_pkthdr *hdr,
                 const uint8_t *data,
                 struct packet_info *pkt);

/**
 * Validate packet minimum size
 *
 * @param pkt Packet information
 * @return 1 if valid, 0 if too small
 */
static inline int packet_is_valid_size(const struct packet_info *pkt) {
    return pkt && pkt->caplen >= sizeof(struct ethhdr);
}

/**
 * Check if packet is multicast or broadcast
 *
 * @param pkt Packet information
 * @return 1 if multicast/broadcast, 0 if unicast
 */
static inline int packet_is_multicast(const struct packet_info *pkt) {
    return pkt && pkt->is_multicast;
}

/**
 * Get VLAN ID from packet
 *
 * @param pkt Packet information
 * @param vlan_id Output VLAN ID
 * @return 0 on success, -1 if no VLAN tag
 */
static inline int packet_get_vlan_id(const struct packet_info *pkt,
                                     uint16_t *vlan_id) {
    if (!pkt || !pkt->has_vlan) {
        return -1;
    }
    *vlan_id = pkt->vlan_id;
    return 0;
}

#endif // PACKET_H
