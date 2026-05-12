/**
 * @file stats.c
 * @brief Statistics implementation
 */

#include "stats.h"
#include <stdio.h>
#include <string.h>
#include <syslog.h>

void stats_init(struct bridge_stats *stats) {
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    stats->start_time = time(NULL);
}

void stats_report(const struct bridge_stats *stats) {
    if (!stats) return;

    time_t now = time(NULL);
    time_t uptime = now - stats->start_time;
    if (uptime == 0) uptime = 1; // Avoid division by zero

    fprintf(stderr, "\n=== Packet Statistics (uptime: %ld seconds) ===\n", uptime);
    for (int i = 0; i < BRIDGE_IF_COUNT; i++) {
        unsigned long rx = atomic_load(&stats->iface[i].rx_packets);
        unsigned long tx = atomic_load(&stats->iface[i].tx_packets);
        unsigned long drop = atomic_load(&stats->iface[i].dropped);
        unsigned long pcap_drop = atomic_load(&stats->iface[i].pcap_drop);
        unsigned long err = atomic_load(&stats->iface[i].errors);

        fprintf(stderr, "  Interface %d:\n", i);
        fprintf(stderr, "    RX:        %10lu packets (%lu pps)\n", rx, rx / uptime);
        fprintf(stderr, "    TX:        %10lu packets (%lu pps)\n", tx, tx / uptime);
        fprintf(stderr, "    Dropped:   %10lu packets\n", drop);
        fprintf(stderr, "    PcapDrop:  %10lu packets\n", pcap_drop);
        fprintf(stderr, "    Errors:    %10lu\n", err);
    }
    fprintf(stderr, "==========================================\n");

    // Also log to syslog
    SLOG(LOG_INFO, "Stats: if0 rx=%lu tx=%lu drop=%lu | if1 rx=%lu tx=%lu drop=%lu",
           atomic_load(&stats->iface[0].rx_packets),
           atomic_load(&stats->iface[0].tx_packets),
           atomic_load(&stats->iface[0].dropped),
           atomic_load(&stats->iface[1].rx_packets),
           atomic_load(&stats->iface[1].tx_packets),
           atomic_load(&stats->iface[1].dropped));
}
