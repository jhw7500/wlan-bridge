/**
 * @file config.h
 * @brief Configuration management for the bridge
 */

#ifndef BRIDGE_CONFIG_H
#define BRIDGE_CONFIG_H

#include "bridge_types.h"
#include <stdio.h>

/**
 * Initialize configuration with default values
 *
 * @param cfg Configuration structure to initialize
 */
void config_init_defaults(struct bridge_config *cfg);

/**
 * Override configuration from environment variables
 *
 * @param cfg Configuration structure to update
 */
void config_load_from_env(struct bridge_config *cfg);

/**
 * Parse command line arguments and override configuration
 *
 * @param cfg Configuration structure to update
 * @param argc Argument count
 * @param argv Argument array
 * @param if0 Output pointer for interface 0 name
 * @param if1 Output pointer for interface 1 name
 * @return 0 on success, 1 if help shown, -1 on error
 */
int config_parse_args(struct bridge_config *cfg, int argc, char **argv,
                      const char **if0, const char **if1);

/**
 * Print usage information
 *
 * @param out Output stream
 * @param prog Program name
 */
void config_print_usage(FILE *out, const char *prog);

#endif // BRIDGE_CONFIG_H
