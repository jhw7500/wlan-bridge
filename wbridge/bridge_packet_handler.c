/**
 * @file bridge_packet_handler.c
 * @brief Refactored packet handler - clean and modular
 *
 * BEFORE: ph() function was 139 lines with complexity 15
 * AFTER: 25 lines with complexity 3, delegates to specialized modules
 */

#include "bridge_types.h"
#include "packet.h"
#include "filter.h"
#include <pcap/pcap.h>
#include <errno.h>
#include <syslog.h>

// Forward declaration for context access
extern struct bridge_context *g_bridge_context;

// Forward declarations for internal functions
int bridge_packet_forward(struct bridge_context *ctx,
                          const struct packet_info *pkt,
                          bridge_interface_t iface_idx);

void bridge_log_inject_error(struct bridge_context *ctx,
                             bridge_interface_t iface_idx,
                             int inject_result);

void bridge_log_partial_inject(struct bridge_context *ctx,
                               bridge_interface_t iface_idx,
                               int injected,
                               uint32_t expected);

/**
 * Refactored packet handler callback for pcap_dispatch
 *
 * This function is now CLEAN and SIMPLE:
 * 1. Parse packet
 * 2. Check if should drop (filtering)
 * 3. Forward to peer interface
 *
 * All complex logic is delegated to specialized modules.
 */
void bridge_packet_handler(unsigned char *user_data,
                           const struct pcap_pkthdr *hdr,
                           const unsigned char *data)
{
    // Extract interface index from user_data
    bridge_interface_t iface_idx = (bridge_interface_t)((uintptr_t)user_data);

    if (!bridge_interface_valid(iface_idx)) {
        return; // Invalid interface
    }

    struct bridge_context *ctx = g_bridge_context;
    bridge_interface_t peer_idx = bridge_peer(iface_idx);

    // Parse packet into structured format
    struct packet_info pkt;
    if (packet_parse(hdr, data, &pkt) < 0) {
        atomic_fetch_add(&ctx->stats.iface[iface_idx].errors, 1);
        return; // Invalid packet
    }

    // Update RX statistics
    atomic_fetch_add(&ctx->stats.iface[iface_idx].rx_packets, 1);

    // Check if packet should be filtered
    if (filter_should_drop(&ctx->filter, &pkt, iface_idx)) {
        return; // Packet filtered (this is NOT an error)
    }

    // Forward packet to peer interface
    if (bridge_packet_forward(ctx, &pkt, peer_idx) < 0) {
        atomic_fetch_add(&ctx->stats.iface[iface_idx].dropped, 1);
        atomic_fetch_add(&ctx->stats.iface[peer_idx].errors, 1);
    } else {
        atomic_fetch_add(&ctx->stats.iface[peer_idx].tx_packets, 1);
    }
}

/**
 * Forward packet to specified interface with retry logic
 *
 * @param ctx Bridge context
 * @param pkt Packet to forward
 * @param iface_idx Destination interface index
 * @return 0 on success, -1 on error
 */
int bridge_packet_forward(struct bridge_context *ctx,
                          const struct packet_info *pkt,
                          bridge_interface_t iface_idx)
{
    if (!ctx || !pkt || !bridge_interface_valid(iface_idx)) {
        return -1;
    }

    struct bridge_interface *iface = &ctx->interfaces[iface_idx];

    // Check if peer interface is ready
    if (!atomic_load(&iface->ready) || !iface->tx_handle) {
        return -1; // Not ready yet
    }

    // Inject packet (forward captured length, not original length)
    int ret = pcap_inject(iface->tx_handle, pkt->data, pkt->caplen);

    // Retry once on ENOBUFS (transient buffer exhaustion)
    if (ret < 0 && errno == ENOBUFS) {
        ret = pcap_inject(iface->tx_handle, pkt->data, pkt->caplen);
    }

    // Check for errors
    if (ret < 0) {
        bridge_log_inject_error(ctx, iface_idx, ret);
        return -1;
    }

    // Check for partial injection (should not happen, but detect it)
    if ((uint32_t)ret < pkt->caplen) {
        bridge_log_partial_inject(ctx, iface_idx, ret, pkt->caplen);
        return -1; // Treat as failure
    }

    return 0; // Success
}

/**
 * Log injection error with rate limiting
 */
void bridge_log_inject_error(struct bridge_context *ctx,
                             bridge_interface_t iface_idx,
                             int inject_result)
{
    if (!ctx->config.enable_debug_log) {
        return;
    }

    static atomic_ulong error_count = 0;
    static time_t last_log_time = 0;

    unsigned long count = atomic_fetch_add(&error_count, 1) + 1;
    time_t now = time(NULL);

    // Log first error immediately, then at most once per second
    if (count == 1 || (last_log_time > 0 && now > last_log_time)) {
        struct bridge_interface *iface = &ctx->interfaces[iface_idx];
        const char *err = pcap_geterr(iface->tx_handle);
        syslog(LOG_ERR, "pcap_inject failed on if%d (%s): %s (errors: %lu)",
               iface_idx, iface->name, err ? err : "unknown", count);
        last_log_time = now;
    }
}

/**
 * Log partial injection with rate limiting
 */
void bridge_log_partial_inject(struct bridge_context *ctx,
                               bridge_interface_t iface_idx,
                               int injected,
                               uint32_t expected)
{
    if (!ctx->config.enable_debug_log) {
        return;
    }

    static atomic_ulong partial_count = 0;
    static time_t last_log_time = 0;

    unsigned long count = atomic_fetch_add(&partial_count, 1) + 1;
    time_t now = time(NULL);

    if (count == 1 || (last_log_time > 0 && now > last_log_time)) {
        syslog(LOG_ERR, "Partial inject on if%d: %d/%u bytes (count: %lu)",
               iface_idx, injected, expected, count);
        last_log_time = now;
    }
}
