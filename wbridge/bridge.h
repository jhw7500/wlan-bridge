/**
 * @file bridge.h
 * @brief Bridge core management
 */

#ifndef BRIDGE_H
#define BRIDGE_H

#include "bridge_types.h"

/**
 * Create a new bridge context
 *
 * @return Allocated context, or NULL on error
 */
struct bridge_context *bridge_create(void);

/**
 * Initialize the bridge (open handles, threads, etc.)
 *
 * @param ctx Bridge context
 * @param if0 Name of first interface
 * @param if1 Name of second interface
 * @return 0 on success, -1 on error
 */
int bridge_init(struct bridge_context *ctx, const char *if0, const char *if1);

/**
 * Run the bridge main loop (waits for signals)
 *
 * @param ctx Bridge context
 */
void bridge_run(struct bridge_context *ctx);

/**
 * Stop the bridge and cleanup resources
 *
 * @param ctx Bridge context
 */
void bridge_cleanup(struct bridge_context *ctx);

#endif // BRIDGE_H
