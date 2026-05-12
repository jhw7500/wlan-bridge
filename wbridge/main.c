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
        // rc > 0 means handled (help or version), rc < 0 means error
        int exit_code = (rc > 0) ? 0 : 1;
        bridge_cleanup(ctx);
        return exit_code;
    }

    openlog(BRIDGE_NAME, LOG_PID | LOG_CONS, LOG_LOCAL0);
    SLOG(LOG_INFO, "%s v%s started (%s <-> %s)", BRIDGE_NAME, BRIDGE_VERSION, if0_name, if1_name);
    SLOG(LOG_INFO, "Features: %s", BRIDGE_FEATURES);
    config_log(&ctx->config);

    struct sigaction sa_term = {.sa_handler = sighandler, .sa_flags = SA_RESTART};
    struct sigaction sa_usr1 = {.sa_handler = sigusr1_handler, .sa_flags = SA_RESTART};
    sigemptyset(&sa_term.sa_mask);
    sigemptyset(&sa_usr1.sa_mask);
    sigaction(SIGINT, &sa_term, NULL);
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGUSR1, &sa_usr1, NULL);

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
