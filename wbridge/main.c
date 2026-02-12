/**
 * @file main.c
 * @brief Bridge entry point
 */

#include "bridge.h"
#include "config.h"
#include "stats.h"
#include <signal.h>
#include <syslog.h>
#include <unistd.h>
#include <stdio.h>
#include <stdatomic.h>

static volatile sig_atomic_t g_print_stats_requested = 0;
static struct bridge_context *g_ctx = NULL;

static void sighandler(int sig) {
    (void)sig;
    if (g_ctx) {
        atomic_store(&g_ctx->keep_running, 0);
    }
}

static void sigusr1_handler(int sig) {
    (void)sig;
    g_print_stats_requested = 1;
}

int main(int argc, char **argv) {
    const char *if0_name = NULL;
    const char *if1_name = NULL;

    struct bridge_context *ctx = bridge_create();
    if (!ctx) {
        fprintf(stderr, "FATAL: Failed to create bridge context\n");
        return 1;
    }
    g_ctx = ctx;

    config_init_defaults(&ctx->config);
    config_load_from_env(&ctx->config);

    int rc = config_parse_args(&ctx->config, argc, argv, &if0_name, &if1_name);
    if (rc != 0) {
        bridge_cleanup(ctx);
        return rc > 0 ? 0 : 1;
    }

    openlog(BRIDGE_NAME, LOG_PID | LOG_CONS, LOG_LOCAL0);
    syslog(LOG_INFO, "%s v%s started (%s <-> %s)", BRIDGE_NAME, BRIDGE_VERSION, if0_name, if1_name);
    syslog(LOG_INFO, "Features: %s", BRIDGE_FEATURES);

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    signal(SIGUSR1, sigusr1_handler);

    if (bridge_init(ctx, if0_name, if1_name) != 0) {
        fprintf(stderr, "FATAL: Bridge initialization failed\n");
        bridge_cleanup(ctx);
        return 1;
    }

    fprintf(stderr, "%s v%s running (%s <-> %s)\n", BRIDGE_NAME, BRIDGE_VERSION, if0_name, if1_name);
    fprintf(stderr, "Press Ctrl+C to stop, send SIGUSR1 (kill -10 %d) for stats\n", getpid());

    while (atomic_load(&ctx->keep_running)) {
        sleep(1);
        if (g_print_stats_requested) {
            g_print_stats_requested = 0;
            stats_report(&ctx->stats);
        }
    }

    fprintf(stderr, "\nShutting down...\n");
    stats_report(&ctx->stats);
    bridge_cleanup(ctx);
    closelog();

    return 0;
}
