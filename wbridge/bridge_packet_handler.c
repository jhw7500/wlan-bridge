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
#include <string.h>
#include <syslog.h>
#include <sys/socket.h>
#include <linux/if_packet.h>

// Forward declaration for context access
extern struct bridge_context *g_bridge_context;

// Forward declarations for internal functions
int bridge_packet_forward(struct bridge_context *ctx,
                          const struct packet_info *pkt,
                          bridge_interface_t iface_idx);

void bridge_log_inject_error(struct bridge_context *ctx,
                             bridge_interface_t iface_idx,
                             int err_no);

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
    struct dispatch_counters *lc = &ctx->local_counters[iface_idx];

    // Update RX statistics (local counter, flushed after pcap_dispatch)
    lc->rx_packets++;

    // Parse packet into structured format
    struct packet_info pkt;
    if (packet_parse(hdr, data, &pkt) < 0) {
        lc->errors++;
        return; // Invalid packet
    }

    // Check if packet should be filtered
    if (filter_should_drop(&ctx->filter, &pkt, iface_idx)) {
        return; // Packet filtered (this is NOT an error)
    }

    // Forward packet to peer interface
    if (bridge_packet_forward(ctx, &pkt, peer_idx) < 0) {
        lc->dropped++;
        lc->errors++;
    } else {
        lc->tx_packets++;
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
    if (!atomic_load(&iface->ready) || iface->tx_fd < 0) {
        return -1; // Not ready yet
    }

    // 송신 전용 소켓은 bind 하지 않으므로 목적지 인터페이스를 매 프레임 지정한다.
    // SOCK_RAW 는 프레임(이더넷 헤더 포함)을 그대로 전송하므로 sll_ifindex 만
    // 필수지만, tpacket 엔진과 동일하게 protocol/dest MAC 도 채워둔다.
    struct sockaddr_ll dst;
    memset(&dst, 0, sizeof(dst));
    dst.sll_family   = AF_PACKET;
    dst.sll_protocol = pkt->eth->h_proto;
    dst.sll_ifindex  = iface->ifindex;
    dst.sll_halen    = ETH_ALEN;
    memcpy(dst.sll_addr, pkt->eth->h_dest, ETH_ALEN);

    // Inject packet (forward captured length, not original length)
    ssize_t ret = sendto(iface->tx_fd, pkt->data, pkt->caplen, 0,
                         (struct sockaddr *)&dst, sizeof(dst));

    // Retry once on ENOBUFS (transient buffer exhaustion)
    if (ret < 0 && errno == ENOBUFS) {
        ret = sendto(iface->tx_fd, pkt->data, pkt->caplen, 0,
                     (struct sockaddr *)&dst, sizeof(dst));
    }

    // Check for errors
    if (ret < 0) {
        bridge_log_inject_error(ctx, iface_idx, errno);
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
                             int err_no)
{
    if (!ctx->config.enable_debug_log) {
        return;
    }

    static atomic_ulong error_count = 0;
    static atomic_long last_log_time = 0;

    unsigned long count = atomic_fetch_add(&error_count, 1) + 1;
    long now = (long)time(NULL);

    // Log first error immediately, then at most once per second
    long prev = atomic_load(&last_log_time);
    if (count == 1 || (prev > 0 && now > prev)) {
        struct bridge_interface *iface = &ctx->interfaces[iface_idx];
        SLOG(LOG_ERR, "sendto failed on if%d (%s): %s (errors: %lu)",
               iface_idx, iface->name, strerror(err_no), count);
        atomic_store(&last_log_time, now);
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
    static atomic_long last_log_time = 0;

    unsigned long count = atomic_fetch_add(&partial_count, 1) + 1;
    long now = (long)time(NULL);

    long prev = atomic_load(&last_log_time);
    if (count == 1 || (prev > 0 && now > prev)) {
        SLOG(LOG_ERR, "Partial inject on if%d: %d/%u bytes (count: %lu)",
               iface_idx, injected, expected, count);
        atomic_store(&last_log_time, now);
    }
}
