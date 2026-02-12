/**
 * @file stats.h
 * @brief Statistics tracking and reporting
 */

#ifndef BRIDGE_STATS_H
#define BRIDGE_STATS_H

#include "bridge_types.h"

/**
 * Initialize statistics
 *
 * @param stats Statistics structure to initialize
 */
void stats_init(struct bridge_stats *stats);

/**
 * Print current statistics to stderr and syslog
 *
 * @param stats Statistics to report
 */
void stats_report(const struct bridge_stats *stats);

#endif // BRIDGE_STATS_H
