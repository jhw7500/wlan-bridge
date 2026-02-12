/**
 * @file config.c
 * @brief Configuration management implementation
 */

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <limits.h>
#include <errno.h>

// Default values
#define WBRIDGE_SNAPLEN 1600
#define WBRIDGE_PCAP_BUFFER_SIZE_BYTES (4 * 1024 * 1024)

static int env_to_int(const char *key, int default_value) {
    const char *env = getenv(key);
    if (!env || !*env) return default_value;
    char *end = NULL;
    long v = strtol(env, &end, 10);
    if (!end || end == env || *end != '\0') return default_value;
    return (int)v;
}

static int clamp_int(int v, int min_v, int max_v) {
    if (v < min_v) return min_v;
    if (v > max_v) return max_v;
    return v;
}

static int safe_atoi(const char *str, int default_value) {
    if (!str || !*str) return default_value;
    char *end = NULL;
    errno = 0;
    long v = strtol(str, &end, 10);
    if (errno != 0 || end == str || *end != '\0') return default_value;
    if (v < INT_MIN || v > INT_MAX) return default_value;
    return (int)v;
}

void config_init_defaults(struct bridge_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->dispatch_budget = 64;
    cfg->enable_affinity = 1;
    cfg->enable_rt = 1;
    cfg->rt_priority = 50;
    cfg->enable_mlock = 1;
    cfg->snaplen = WBRIDGE_SNAPLEN;
    cfg->pcap_buffer_bytes = WBRIDGE_PCAP_BUFFER_SIZE_BYTES;
    cfg->timeout_ms = 1;
    cfg->enable_immediate = 1;
    cfg->enable_promisc = 1;
    cfg->enable_mac_filter = 0;
    cfg->enable_ip_filter = 0;
    cfg->enable_debug_log = 1;
}

void config_load_from_env(struct bridge_config *cfg) {
    cfg->dispatch_budget = env_to_int("WBRIDGE_DISPATCH_BUDGET", cfg->dispatch_budget);
    cfg->enable_affinity = env_to_int("WBRIDGE_AFFINITY", cfg->enable_affinity) ? 1 : 0;
    cfg->enable_rt = env_to_int("WBRIDGE_RT", cfg->enable_rt) ? 1 : 0;
    cfg->rt_priority = env_to_int("WBRIDGE_RT_PRIORITY", cfg->rt_priority);
    cfg->enable_mlock = env_to_int("WBRIDGE_MLOCK", cfg->enable_mlock) ? 1 : 0;
    cfg->snaplen = env_to_int("WBRIDGE_SNAPLEN", cfg->snaplen);
    cfg->pcap_buffer_bytes = env_to_int("WBRIDGE_PCAP_BUFFER", cfg->pcap_buffer_bytes);
    cfg->timeout_ms = env_to_int("WBRIDGE_TIMEOUT_MS", cfg->timeout_ms);
    cfg->enable_immediate = env_to_int("WBRIDGE_IMMEDIATE", cfg->enable_immediate) ? 1 : 0;
    cfg->enable_promisc = env_to_int("WBRIDGE_PROMISC", cfg->enable_promisc) ? 1 : 0;
    cfg->enable_mac_filter = env_to_int("WBRIDGE_MAC_FILTER", cfg->enable_mac_filter) ? 1 : 0;
    cfg->enable_ip_filter = env_to_int("WBRIDGE_IP_FILTER", cfg->enable_ip_filter) ? 1 : 0;
    cfg->enable_debug_log = env_to_int("WBRIDGE_DEBUG", cfg->enable_debug_log) ? 1 : 0;

    // Apply clamps
    cfg->dispatch_budget = clamp_int(cfg->dispatch_budget, 1, 4096);
    cfg->rt_priority = clamp_int(cfg->rt_priority, 1, 99);
    cfg->snaplen = clamp_int(cfg->snaplen, 64, 65535);
    cfg->pcap_buffer_bytes = clamp_int(cfg->pcap_buffer_bytes, 256 * 1024, 64 * 1024 * 1024);
    cfg->timeout_ms = clamp_int(cfg->timeout_ms, 0, 1000);
}

int config_parse_args(struct bridge_config *cfg, int argc, char **argv,
                      const char **if0, const char **if1) {
    static struct option long_opts[] = {
        {"dispatch-budget", required_argument, 0, 1},
        {"no-affinity", no_argument, 0, 2},
        {"no-rt", no_argument, 0, 3},
        {"rt-priority", required_argument, 0, 4},
        {"no-mlock", no_argument, 0, 5},
        {"snaplen", required_argument, 0, 6},
        {"pcap-buffer", required_argument, 0, 7},
        {"timeout-ms", required_argument, 0, 8},
        {"no-immediate", no_argument, 0, 9},
        {"no-promisc", no_argument, 0, 10},
        {"mac-filter", no_argument, 0, 11},
        {"ip-filter", no_argument, 0, 12},
        {"no-debug", no_argument, 0, 13},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0},
    };

    int opt;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 1: cfg->dispatch_budget = clamp_int(safe_atoi(optarg, 64), 1, 4096); break;
        case 2: cfg->enable_affinity = 0; break;
        case 3: cfg->enable_rt = 0; break;
        case 4: cfg->rt_priority = clamp_int(safe_atoi(optarg, 50), 1, 99); break;
        case 5: cfg->enable_mlock = 0; break;
        case 6: cfg->snaplen = clamp_int(safe_atoi(optarg, WBRIDGE_SNAPLEN), 64, 65535); break;
        case 7: cfg->pcap_buffer_bytes = clamp_int(safe_atoi(optarg, WBRIDGE_PCAP_BUFFER_SIZE_BYTES), 256 * 1024, 64 * 1024 * 1024); break;
        case 8: cfg->timeout_ms = clamp_int(safe_atoi(optarg, 1), 0, 1000); break;
        case 9: cfg->enable_immediate = 0; break;
        case 10: cfg->enable_promisc = 0; break;
        case 11: cfg->enable_mac_filter = 1; break;
        case 12: cfg->enable_ip_filter = 1; break;
        case 13: cfg->enable_debug_log = 0; break;
        case 'h': config_print_usage(stdout, argv[0]); return 1;
        default: config_print_usage(stderr, argv[0]); return -1;
        }
    }

    if (argc - optind != 2) {
        config_print_usage(stderr, argv[0]);
        return -1;
    }

    *if0 = argv[optind];
    *if1 = argv[optind + 1];

    return 0;
}

void config_print_usage(FILE *out, const char *prog) {
    fprintf(out,
            "Usage: %s [options] <interface0> <interface1>\n"
            "\n"
            "Options (CLI overrides env; env overrides defaults):\n"
            "  --dispatch-budget N     Max packets per dispatch (env WBRIDGE_DISPATCH_BUDGET, default 64)\n"
            "  --no-affinity           Disable CPU pinning (env WBRIDGE_AFFINITY=0, default enabled)\n"
            "  --no-rt                 Disable SCHED_FIFO (env WBRIDGE_RT=0, default enabled)\n"
            "  --rt-priority N         SCHED_FIFO priority (env WBRIDGE_RT_PRIORITY, default 50)\n"
            "  --no-mlock              Disable mlockall (env WBRIDGE_MLOCK=0, default enabled)\n"
            "  --snaplen N             pcap snaplen bytes (env WBRIDGE_SNAPLEN, default 1600)\n"
            "  --pcap-buffer BYTES     pcap RX buffer bytes (env WBRIDGE_PCAP_BUFFER, default 4194304)\n"
            "  --timeout-ms N          pcap timeout ms (env WBRIDGE_TIMEOUT_MS, default 1)\n"
            "  --no-immediate          Disable immediate mode (env WBRIDGE_IMMEDIATE=0, default enabled)\n"
            "  --no-promisc            Disable promisc (env WBRIDGE_PROMISC=0, default enabled)\n"
            "  --mac-filter            Enable MAC-based reinject skip (env WBRIDGE_MAC_FILTER=1, default off)\n"
            "  --ip-filter             Enable IP-based reinject skip (env WBRIDGE_IP_FILTER=1, default off)\n"
            "  --no-debug              Disable verbose logs (env WBRIDGE_DEBUG=0, default on)\n"
            "  -h, --help              Show help\n",
            prog);
}
