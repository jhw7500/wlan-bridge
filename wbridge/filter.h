/**
 * @file filter.h
 * @brief Packet filtering logic
 *
 * Determines whether a packet should be forwarded or dropped
 * based on MAC address, IP address, and ARP filters.
 */

#ifndef FILTER_H
#define FILTER_H

#include "packet.h"

/**
 * Initialize packet filter
 *
 * @param filter Filter structure to initialize
 * @param config Bridge configuration
 * @param interfaces Interface array
 */
void filter_init(struct packet_filter *filter,
                const struct bridge_config *config,
                const struct bridge_interface *interfaces);

/**
 * Check if packet should be dropped
 *
 * @param filter Packet filter state
 * @param pkt Packet information
 * @param iface_idx Receiving interface index
 * @return 1 if packet should be dropped, 0 if should be forwarded
 */
int filter_should_drop(const struct packet_filter *filter,
                       const struct packet_info *pkt,
                       bridge_interface_t iface_idx);

/**
 * Check if packet destination MAC matches bridge or peer interface
 *
 * @param filter Packet filter state
 * @param pkt Packet information
 * @param iface_idx Receiving interface index
 * @return 1 if MAC matches, 0 otherwise
 */
int filter_mac_is_self_or_peer(const struct packet_filter *filter,
                                const struct packet_info *pkt,
                                bridge_interface_t iface_idx);

/**
 * Check if packet destination IP is a local bridge IP
 *
 * @param filter Packet filter state
 * @param pkt Packet information
 * @return 1 if IP is local, 0 otherwise
 */
int filter_ip_is_local(const struct packet_filter *filter,
                       const struct packet_info *pkt);

/**
 * Check if ARP request/reply targets a bridge interface IP
 *
 * @param filter Packet filter state
 * @param pkt Packet information
 * @return 1 if ARP is for bridge, 0 otherwise
 */
int filter_arp_is_for_bridge(const struct packet_filter *filter,
                             const struct packet_info *pkt);

#endif // FILTER_H
