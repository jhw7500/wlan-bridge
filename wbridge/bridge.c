/**
 * @file bridge.c
 * @brief Bridge core implementation
 */

#define _GNU_SOURCE
#include "bridge.h"
#include "config.h"
#include "stats.h"
#include "filter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <ifaddrs.h>
#include <arpa/inet.h>

// Global pointer for the packet handler callback
struct bridge_context *g_bridge_context = NULL;

// Forward declaration of the packet handler from bridge_packet_handler.c
extern void bridge_packet_handler(unsigned char *user_data,
                                 const struct pcap_pkthdr *hdr,
                                 const unsigned char *data);

// Private helpers (from dumb.c)
static int get_mac(const char *ifname, uint8_t mac[ETH_ALEN]) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    if (ioctl(fd, SIOCGIFHWADDR, &ifr) != 0) {
        close(fd);
        return -1;
    }

    memcpy(mac, ifr.ifr_hwaddr.sa_data, ETH_ALEN);
    close(fd);
    return 0;
}

static pcap_t *open_pcap_handle(const char *ifname, int is_rx, const struct bridge_config *cfg, char errbuf[PCAP_ERRBUF_SIZE]) {
    pcap_t *h = pcap_create(ifname, errbuf);
    if (!h) return NULL;

    pcap_set_snaplen(h, cfg->snaplen);

    if (is_rx) {
        pcap_set_buffer_size(h, cfg->pcap_buffer_bytes);
        pcap_set_promisc(h, cfg->enable_promisc);
        pcap_set_timeout(h, cfg->timeout_ms);
        if (cfg->enable_immediate) {
            pcap_set_immediate_mode(h, 1);
        }
    }

    int rc = pcap_activate(h);
    if (rc < 0) {
        syslog(LOG_ERR, "Failed to activate %s: %s", ifname, pcap_statustostr(rc));
        pcap_close(h);
        return NULL;
    }
    return h;
}

static int both_ready(struct bridge_context *ctx) {
    int r = atomic_load_explicit(&ctx->interfaces->ready, memory_order_acquire);
    // Actually we need to check ready flag of both interfaces
    int r0 = atomic_load(&ctx->interfaces[0].ready);
    int r1 = atomic_load(&ctx->interfaces[1].ready);
    return r0 && r1;
}

static void *bridge_thread_func(void *arg) {
    bridge_interface_t i = (bridge_interface_t)((uintptr_t)arg);
    struct bridge_context *ctx = g_bridge_context;
    struct bridge_interface *iface = &ctx->interfaces[i];

    if (pcap_setdirection(iface->rx_handle, PCAP_D_IN) != 0) {
        syslog(LOG_WARNING, "pcap_setdirection ignored on %s", iface->name);
    }

    if (ctx->config.enable_affinity) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(i, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    }

    if (ctx->config.enable_rt) {
        struct sched_param sp = {.sched_priority = ctx->config.rt_priority};
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }

    pthread_mutex_lock(&ctx->mutex);
    atomic_store(&iface->ready, 1);
    pthread_cond_broadcast(&ctx->cond);

    while (!both_ready(ctx)) {
        pthread_cond_wait(&ctx->cond, &ctx->mutex);
    }
    pthread_mutex_unlock(&ctx->mutex);

    syslog(LOG_INFO, "Interface %s (if%d) worker thread started", iface->name, i);

    int stats_counter = 0;
    while (atomic_load(&ctx->keep_running)) {
        int rc = pcap_dispatch(iface->rx_handle, ctx->config.dispatch_budget,
                               bridge_packet_handler, (unsigned char *)((uintptr_t)i));
        if (rc == PCAP_ERROR) {
            atomic_store(&ctx->keep_running, 0);
            break;
        }

        if (++stats_counter >= 1000) {
            stats_counter = 0;
            struct pcap_stat ps;
            if (pcap_stats(iface->rx_handle, &ps) == 0) {
                atomic_store(&ctx->stats.iface[i].pcap_drop, ps.ps_drop + ps.ps_ifdrop);
            }
        }
    }

    syslog(LOG_INFO, "Interface %s worker thread exiting", iface->name);
    return NULL;
}

struct bridge_context *bridge_create(void) {
    struct bridge_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    atomic_init(&ctx->keep_running, 1);
    pthread_mutex_init(&ctx->mutex, NULL);
    pthread_cond_init(&ctx->cond, NULL);

    return ctx;
}

int bridge_init(struct bridge_context *ctx, const char *if0, const char *if1) {
    char errbuf[PCAP_ERRBUF_SIZE];
    g_bridge_context = ctx;

    const char *names[2] = {if0, if1};
    for (int i = 0; i < 2; i++) {
        struct bridge_interface *iface = &ctx->interfaces[i];
        strncpy(iface->name, names[i], IFNAMSIZ - 1);

        iface->rx_handle = open_pcap_handle(names[i], 1, &ctx->config, errbuf);
        if (!iface->rx_handle) return -1;

        iface->tx_handle = open_pcap_handle(names[i], 0, &ctx->config, errbuf);
        if (!iface->tx_handle) return -1;

        if (get_mac(names[i], iface->mac) != 0) return -1;
    }

    // IP address collection (best effort)
    if (ctx->config.enable_ip_filter) {
        struct ifaddrs *ifa = NULL;
        if (getifaddrs(&ifa) == 0) {
            for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
                if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
                for (int i = 0; i < 2; i++) {
                    if (strcmp(p->ifa_name, names[i]) == 0) {
                        ctx->interfaces[i].ipv4 = ((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr;
                    }
                }
            }
            freeifaddrs(ifa);
        }
    }

    filter_init(&ctx->filter, &ctx->config, ctx->interfaces);
    stats_init(&ctx->stats);

    for (int i = 0; i < 2; i++) {
        if (pthread_create(&ctx->interfaces[i].thread, NULL, bridge_thread_func, (void *)((uintptr_t)i)) != 0) {
            return -1;
        }
    }

    if (ctx->config.enable_mlock) {
        mlockall(MCL_CURRENT | MCL_FUTURE);
    }

    return 0;
}

void bridge_run(struct bridge_context *ctx) {
    // In our implementation, threads are already running.
    // This function just waits for shutdown signal.
    while (atomic_load(&ctx->keep_running)) {
        sleep(1);
        // Signal handling happens via global flags set in sighandlers (main.c)
    }
}

void bridge_cleanup(struct bridge_context *ctx) {
    if (!ctx) return;

    atomic_store(&ctx->keep_running, 0);

    for (int i = 0; i < 2; i++) {
        if (ctx->interfaces[i].rx_handle) pcap_breakloop(ctx->interfaces[i].rx_handle);
    }

    for (int i = 0; i < 2; i++) {
        if (ctx->interfaces[i].thread) pthread_join(ctx->interfaces[i].thread, NULL);
        if (ctx->interfaces[i].rx_handle) pcap_close(ctx->interfaces[i].rx_handle);
        if (ctx->interfaces[i].tx_handle) pcap_close(ctx->interfaces[i].tx_handle);
    }

    pthread_mutex_destroy(&ctx->mutex);
    pthread_cond_destroy(&ctx->cond);
    free(ctx);
    g_bridge_context = NULL;
}
