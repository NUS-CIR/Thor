/*
 * Copyright (c) Microsoft Corporation.
   Copyright (c) National University of Singapore.
 * Licensed under the MIT License
 */

#include <rte_common.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ether.h>
#include <rte_bus_pci.h>
#include <rte_dev.h>
#include <rte_malloc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#include "thor_fhaul_proxy.h"
#include "thor_ctrl.h"

static volatile bool force_quit;

static const struct rte_eth_conf port_conf_default = {
    .rxmode = {
        .mtu = ETHER_JUMBO_FRAME_SIZE,
        .max_lro_pkt_size = ETHER_JUMBO_FRAME_SIZE,
        .mq_mode = RTE_ETH_MQ_RX_NONE,
        .offloads = RTE_ETH_RX_OFFLOAD_CHECKSUM | RTE_ETH_RX_OFFLOAD_VLAN_STRIP},
    .txmode = {
        .offloads = RTE_ETH_TX_OFFLOAD_VLAN_INSERT | RTE_ETH_TX_OFFLOAD_MULTI_SEGS | RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_TCP_CKSUM | RTE_ETH_TX_OFFLOAD_UDP_CKSUM,
        .mq_mode = RTE_ETH_MQ_TX_NONE,
    },
};

static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM)
    {
        printf("\nSignal %d received, shutting down...\n", signum);
        force_quit = true;
    }
}

void set_sched_fifo()
{
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_RR);

    if (sched_setscheduler(0, SCHED_RR, &param) == -1)
    {
        fprintf(stderr, "Failed to set SCHED_FIFO: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

static inline int
port_init(uint16_t port, struct rte_mempool *mbuf_pool, uint16_t mtu)
{
    struct rte_eth_conf port_conf = port_conf_default;
    struct rte_eth_dev_info dev_info;

    const uint16_t rx_rings = 1, tx_rings = 1;
    int retval;

    if (port >= rte_eth_dev_count_avail())
        return -1;

    port_conf.rxmode.mtu = mtu;
    port_conf.rxmode.max_lro_pkt_size = mtu;

    retval = rte_eth_dev_info_get(port, &dev_info);
    if (retval != 0)
        return retval;

    /*
     * Keep only the offloads this device actually advertises. On the production
     * Intel VFs every requested offload is supported and this is a no-op; on a
     * software PMD (af_packet over veth, used by the integration tests) the
     * checksum offloads are absent and an unmasked request makes
     * rte_eth_dev_configure() fail outright.
     */
    uint64_t rx_wanted = port_conf.rxmode.offloads;
    uint64_t tx_wanted = port_conf.txmode.offloads;
    port_conf.rxmode.offloads &= dev_info.rx_offload_capa;
    port_conf.txmode.offloads &= dev_info.tx_offload_capa;

    if (rx_wanted != port_conf.rxmode.offloads)
        printf("WARNING: port %u does not support Rx offloads 0x%" PRIx64 ", disabled\n",
               port, rx_wanted & ~port_conf.rxmode.offloads);
    if (tx_wanted != port_conf.txmode.offloads)
        printf("WARNING: port %u does not support Tx offloads 0x%" PRIx64 ", disabled\n",
               port, tx_wanted & ~port_conf.txmode.offloads);

    if ((port_conf.rxmode.offloads & RTE_ETH_RX_OFFLOAD_VLAN_STRIP) == 0)
        printf("WARNING: port %u cannot strip VLAN tags; VLAN retagging is disabled\n", port);

    retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (retval != 0)
        return retval;

    retval = rte_eth_dev_set_mtu(port, mtu);
    if (retval != 0)
    {
        printf("Failed to set MTU %u on port %u: %s (device max frame %u)\n",
               mtu, port, rte_strerror(-retval), dev_info.max_rx_pktlen);
        return retval;
    }

    retval = rte_eth_rx_queue_setup(port, 0, RX_RING_SIZE,
                                    rte_eth_dev_socket_id(port), NULL, mbuf_pool);
    if (retval < 0)
        return retval;

    retval = rte_eth_tx_queue_setup(port, 0, TX_RING_SIZE,
                                    rte_eth_dev_socket_id(port), NULL);
    if (retval < 0)
        return retval;

    retval = rte_eth_dev_start(port);
    if (retval < 0)
        return retval;

    return 0;
}

int get_port_from_pci(const char *pci_address, uint16_t *port_id)
{
    int ret = rte_eth_dev_get_port_by_name(pci_address, port_id);
    if (ret != 0)
    {
        fprintf(stderr, "Error: Could not find port ID for PCI address %s\n", pci_address);
        return ret;
    }
    return 0;
}

static void
parse_mac_address(const char *mac_str, struct rte_ether_addr *mac_addr)
{
    printf("Parsing MAC address: %s\n", mac_str);
    if (rte_ether_unformat_addr(mac_str, mac_addr) != 0)
    {
        rte_exit(EXIT_FAILURE, "Invalid MAC address format: %s\n", mac_str);
    }
}

static void
print_mac_address_from_portid(uint16_t port)
{
    struct rte_ether_addr mac_addr;
    rte_eth_macaddr_get(port, &mac_addr);
    printf("Port %u MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
           port,
           mac_addr.addr_bytes[0],
           mac_addr.addr_bytes[1],
           mac_addr.addr_bytes[2],
           mac_addr.addr_bytes[3],
           mac_addr.addr_bytes[4],
           mac_addr.addr_bytes[5]);
}

void check_link_status(uint16_t port_id)
{
    struct rte_eth_link link;
    if (rte_eth_link_get_nowait(port_id, &link) != 0)
    {
        printf("Failed to get link status for port %u\n", port_id);
        exit(-1);
    }

    if (!link.link_status)
    {
        printf("Link is down\n");
    }
    else
    {
        printf("Link is up - speed %u Mbps - %s\n",
               link.link_speed,
               (link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX) ? "full-duplex" : "half-duplex");
    }
}

static void print_stats(struct middlebox_config *config)
{
    struct middlebox_stats *s = &config->stats;

    printf("===============================\n");
    printf("Packet counts in the last second:\n");
    printf("DOWNLINK:\n");
    for (int i = 0; i < MAX_NUM_DUS; i++)
    {
        if (!config->du_configs[i].active)
            continue;
        printf("    DU %d: %lu CP packets/s, %lu UP packets/s\n",
               i, s->dl_cp_from_du[i], s->dl_up_from_du[i]);
        s->dl_cp_from_du[i] = 0;
        s->dl_up_from_du[i] = 0;
    }
    printf("    RU: %lu packets/s\n", s->dl_to_ru);
    s->dl_to_ru = 0;

    printf("UPLINK:\n");
    printf("    RU: %lu packets/s\n", s->ul_from_ru);
    s->ul_from_ru = 0;
    for (int i = 0; i < MAX_NUM_DUS; i++)
    {
        if (!config->du_configs[i].active)
            continue;
        printf("    DU %d: %lu packets/s\n", i, s->ul_to_du[i]);
        s->ul_to_du[i] = 0;
    }
}

struct lcore_args
{
    struct middlebox_config *config;
    struct thor_ctrl *ctrl;
    unsigned int stats_interval_s;
};

int lcore_main(void *args)
{
    struct lcore_args *largs = (struct lcore_args *)args;
    struct middlebox_config *config = largs->config;
    uint64_t last_timestamp = 0;

    while (!force_quit)
    {
        /*
         * Apply pending add/remove-L1 commands here, between bursts. This is
         * the only safe point: applying one flushes the merge cache, whose
         * entries are keyed against the DU count.
         */
        thor_ctrl_poll(largs->ctrl, config);

        struct rte_mbuf *mx_bufs[BURST_SIZE];
        struct rte_mbuf *mx_tx_bufs[MAX_TX_BURST];
        uint16_t mx_tx_idx = 0;
        uint16_t nb_rx = rte_eth_rx_burst(config->nic_port_id, 0, mx_bufs, BURST_SIZE);

        if (largs->stats_interval_s > 0)
        {
            uint64_t current_timestamp = rte_rdtsc();
            if (last_timestamp == 0)
                last_timestamp = current_timestamp;
            else if (current_timestamp - last_timestamp >=
                     rte_get_tsc_hz() * largs->stats_interval_s)
            {
                last_timestamp = current_timestamp;
                print_stats(config);
            }
        }

        for (int rx_idx = 0; rx_idx < nb_rx; rx_idx++)
            thor_handle_rx_packet(config, mx_bufs[rx_idx], mx_tx_bufs, &mx_tx_idx);

        if (mx_tx_idx > 0)
        {
            uint16_t nb_tx = rte_eth_tx_burst(config->nic_port_id, 0, mx_tx_bufs, mx_tx_idx);

            if (unlikely(nb_tx < mx_tx_idx))
            {
                config->stats.tx_failed += mx_tx_idx - nb_tx;
                for (uint16_t i = nb_tx; i < mx_tx_idx; i++)
                    rte_pktmbuf_free(mx_tx_bufs[i]);
            }
        }
    }

    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s <EAL args> -- <PORT> <RU_MAC> <RU_VLAN> <NUM_PRBS>\n"
            "           <DU_MAC> <DU_VLAN> [<DU_MAC> <DU_VLAN>]... [<NUM_DUS>] [options]\n"
            "\n"
            "  PORT       device name of the middlebox port (PCI address, or a\n"
            "             vdev name such as net_af_packet0)\n"
            "  DU_MAC     at least one L1 must be given at startup; further L1s can\n"
            "             be added and removed at runtime over --ctrl-sock, up to a\n"
            "             ceiling of %d at any one time\n"
            "  NUM_DUS    how many of the listed DUs to start active; defaults to\n"
            "             all of them, and must be at least 1\n"
            "\n"
            "Options:\n"
            "  --ctrl-sock <path>   serve add/remove-L1 commands on this UNIX socket\n"
            "  --stats-interval <s> print packet counters every <s> seconds (0 = off)\n"
            "  --mtu <bytes>        port MTU (default %d); lower it only for software\n"
            "                       PMDs that cannot do jumbo frames\n"
            "  --no-rt              do not raise the datapath to SCHED_RR priority\n",
            prog, MAX_NUM_DUS, ETHER_JUMBO_FRAME_SIZE);
}

int main(int argc, char *argv[])
{
    struct middlebox_config config;
    struct thor_ctrl *ctrl = NULL;
    const char *ctrl_sock_path = NULL;
    unsigned int stats_interval_s = 0;
    uint16_t mtu = ETHER_JUMBO_FRAME_SIZE;
    bool use_rt_prio = true;

    /* Line-buffered so a supervising process (the integration tests) sees
     * progress even when stdout is a pipe. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    int eal_argc = rte_eal_init(argc, argv);
    if (eal_argc < 0)
        rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
    printf("Received %d DPDK arguments and we have %d total arguments\n", eal_argc, argc);

    argc -= eal_argc;
    argv += eal_argc;

    force_quit = false;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    memset(&config, 0, sizeof(config));

    /* Split the application arguments into flags and positionals. */
    const char *positional[16];
    int npos = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--ctrl-sock") == 0)
        {
            if (++i >= argc)
                rte_exit(EXIT_FAILURE, "--ctrl-sock requires a path\n");
            ctrl_sock_path = argv[i];
        }
        else if (strcmp(argv[i], "--stats-interval") == 0)
        {
            if (++i >= argc)
                rte_exit(EXIT_FAILURE, "--stats-interval requires a value\n");
            stats_interval_s = (unsigned int)atoi(argv[i]);
        }
        else if (strcmp(argv[i], "--mtu") == 0)
        {
            if (++i >= argc)
                rte_exit(EXIT_FAILURE, "--mtu requires a value\n");
            mtu = (uint16_t)atoi(argv[i]);
        }
        else if (strcmp(argv[i], "--no-rt") == 0)
        {
            use_rt_prio = false;
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            usage(argv[0]);
            return 0;
        }
        else
        {
            if (npos >= (int)RTE_DIM(positional))
                rte_exit(EXIT_FAILURE, "Too many arguments (max %zu DUs)\n",
                         (RTE_DIM(positional) - 5) / 2);
            positional[npos++] = argv[i];
        }
    }

    /* PORT, RU_MAC, RU_VLAN, NUM_PRBS and at least one <DU_MAC> <DU_VLAN> pair. */
    if (npos < 6)
    {
        usage(argv[0]);
        rte_exit(EXIT_FAILURE,
                 "Expected at least 6 positional arguments (including one L1), got %d\n",
                 npos);
    }

    /* Middlebox */
    const char *mb_port_name = positional[0];
    if (get_port_from_pci(mb_port_name, &config.nic_port_id) != 0)
        rte_exit(EXIT_FAILURE, "No such port: %s\n", mb_port_name);
    rte_eth_macaddr_get(config.nic_port_id, &config.middlebox_addr);
    printf("Middlebox port: %s\n", mb_port_name);

    /* RU configuration */
    parse_mac_address(positional[1], &config.ru_config.ru_addr);
    config.ru_config.vlan = (uint16_t)atoi(positional[2]);
    printf("RU MAC address: %s, VLAN: %d\n", positional[1], config.ru_config.vlan);

    config.num_prbs = (uint16_t)atoi(positional[3]);
    config.xran_nprb = XRAN_CONVERT_NUMPRB(config.num_prbs);
    printf("Number of PRBs: %d\n", config.num_prbs);

    /*
     * The remaining positionals are <DU_MAC> <DU_VLAN> pairs, optionally
     * followed by an explicit DU count. An odd remainder means the last value
     * is that count -- which keeps the historic 9-argument invocation working.
     */
    int rest = npos - 4;
    int num_du_args = rest / 2;
    int num_dus = num_du_args;

    if (rest % 2 == 1)
    {
        num_dus = atoi(positional[npos - 1]);
        if (num_dus > num_du_args)
            rte_exit(EXIT_FAILURE,
                     "NUM_DUS is %d but only %d DU address/VLAN pairs were given\n",
                     num_dus, num_du_args);
    }

    if (num_dus < 1)
        rte_exit(EXIT_FAILURE, "At least one L1 must be configured at startup\n");

    if (num_du_args > MAX_NUM_DUS)
        rte_exit(EXIT_FAILURE, "At most %d DUs are supported, got %d\n",
                 MAX_NUM_DUS, num_du_args);

    for (int i = 0; i < num_dus; i++)
    {
        parse_mac_address(positional[4 + i * 2], &config.du_configs[i].du_addr);
        config.du_configs[i].vlan = (uint16_t)atoi(positional[4 + i * 2 + 1]);
        config.du_configs[i].active = true;
        printf("DU%d MAC address: %s, VLAN: %d\n",
               i + 1, positional[4 + i * 2], config.du_configs[i].vlan);
    }
    config.num_dus = num_dus;
    printf("Number of DUs: %d\n", config.num_dus);

    config.cache = rte_zmalloc("thor_cache", sizeof(struct middlebox_cache),
                               RTE_CACHE_LINE_SIZE);
    if (config.cache == NULL)
        rte_exit(EXIT_FAILURE, "Cannot allocate the packet cache (%zu bytes)\n",
                 sizeof(struct middlebox_cache));

    config.mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS,
                                               MBUF_CACHE_SIZE, 0, ETHER_JUMBO_FRAME_SIZE + RTE_PKTMBUF_HEADROOM + 100, SOCKET_ID_ANY);
    if (config.mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool for middlebox\n");

    if (port_init(config.nic_port_id, config.mbuf_pool, mtu) != 0)
        rte_exit(EXIT_FAILURE, "Cannot init port %" PRIu16 "\n", config.nic_port_id);

    printf("Initialized port %d for middlebox\n", config.nic_port_id);
    print_mac_address_from_portid(config.nic_port_id);

    check_link_status(config.nic_port_id);

    if (rte_lcore_count() > 1)
        printf("\nWARNING: Too many lcores enabled. Only 1 used.\n");

    if (ctrl_sock_path != NULL)
    {
        ctrl = thor_ctrl_start(ctrl_sock_path);
        if (ctrl == NULL)
            rte_exit(EXIT_FAILURE, "Cannot start the control channel on %s\n", ctrl_sock_path);
    }

    if (use_rt_prio)
        set_sched_fifo();

    printf("Middlebox ready\n");
    fflush(stdout);

    struct lcore_args largs = {
        .config = &config,
        .ctrl = ctrl,
        .stats_interval_s = stats_interval_s,
    };
    lcore_main(&largs);

    thor_ctrl_stop(ctrl);
    thor_cache_flush(&config);
    rte_eth_dev_stop(config.nic_port_id);
    rte_eth_dev_close(config.nic_port_id);
    rte_free(config.cache);
    rte_eal_cleanup();

    printf("Middlebox stopped\n");
    return 0;
}
