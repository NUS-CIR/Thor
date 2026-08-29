/*
 * Copyright (c) National University of Singapore.
 * Licensed under the MIT License
 *
 * Unit tests for the Thor fronthaul proxy datapath.
 *
 * These drive thor_handle_rx_packet() directly against a private mempool, so
 * they need no NIC, no veth pair and no root beyond what EAL itself wants
 * (run with --no-huge and they need none). Each test builds mbufs by hand,
 * feeds them in, and inspects the TX array and the DU table afterwards.
 *
 *   build/tests/test_datapath --no-huge -m 256 --no-pci
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>

#include "thor_fhaul_proxy.h"

/* ------------------------------------------------------------------------- */
/* tiny test harness                                                         */
/* ------------------------------------------------------------------------- */

static int tests_run;
static int tests_failed;
static const char *current_test;
static int current_test_failed;

#define CHECK(cond)                                                       \
    do                                                                    \
    {                                                                     \
        if (!(cond))                                                      \
        {                                                                 \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            current_test_failed = 1;                                      \
        }                                                                 \
    } while (0)

#define CHECK_EQ(actual, expected)                                              \
    do                                                                          \
    {                                                                           \
        long long _a = (long long)(actual);                                     \
        long long _e = (long long)(expected);                                   \
        if (_a != _e)                                                           \
        {                                                                       \
            printf("    FAIL %s:%d: %s == %lld, expected %lld\n",               \
                   __FILE__, __LINE__, #actual, _a, _e);                        \
            current_test_failed = 1;                                            \
        }                                                                       \
    } while (0)

#define RUN(fn)                                        \
    do                                                 \
    {                                                  \
        current_test = #fn;                            \
        current_test_failed = 0;                       \
        tests_run++;                                   \
        printf("  %s\n", current_test);                \
        fn();                                          \
        if (current_test_failed)                       \
            tests_failed++;                            \
    } while (0)

/* ------------------------------------------------------------------------- */
/* fixtures                                                                  */
/* ------------------------------------------------------------------------- */

static struct rte_mempool *g_pool;

static const struct rte_ether_addr MB_MAC = {{0x00, 0x11, 0x22, 0x33, 0x44, 0x00}};
static const struct rte_ether_addr RU_MAC = {{0x00, 0x11, 0x22, 0x33, 0x44, 0x01}};
static const struct rte_ether_addr DU0_MAC = {{0x00, 0x11, 0x22, 0x33, 0x44, 0x10}};
static const struct rte_ether_addr DU1_MAC = {{0x00, 0x11, 0x22, 0x33, 0x44, 0x11}};
static const struct rte_ether_addr DU2_MAC = {{0x00, 0x11, 0x22, 0x33, 0x44, 0x12}};
static const struct rte_ether_addr OTHER_MAC = {{0x00, 0x11, 0x22, 0x33, 0x44, 0xFF}};

#define RU_VLAN 5
#define DU0_VLAN 10
#define DU1_VLAN 11
#define DU2_VLAN 12

/* Fresh config with num_dus DUs active in slots 0..num_dus-1. */
static void config_init(struct middlebox_config *cfg, int num_dus)
{
    static const struct rte_ether_addr *macs[] = {&DU0_MAC, &DU1_MAC, &DU2_MAC};
    static const uint16_t vlans[] = {DU0_VLAN, DU1_VLAN, DU2_VLAN};

    if (num_dus > MAX_NUM_DUS)
        rte_exit(EXIT_FAILURE, "test asked for %d DUs, ceiling is %d\n", num_dus, MAX_NUM_DUS);

    memset(cfg, 0, sizeof(*cfg));
    cfg->mbuf_pool = g_pool;
    cfg->middlebox_addr = MB_MAC;
    cfg->ru_config.ru_addr = RU_MAC;
    cfg->ru_config.vlan = RU_VLAN;
    cfg->num_prbs = 106;
    cfg->xran_nprb = XRAN_CONVERT_NUMPRB(106);

    cfg->cache = rte_zmalloc("test_cache", sizeof(struct middlebox_cache), RTE_CACHE_LINE_SIZE);
    if (cfg->cache == NULL)
        rte_exit(EXIT_FAILURE, "cannot allocate test cache\n");

    for (int i = 0; i < num_dus; i++)
    {
        cfg->du_configs[i].du_addr = *macs[i];
        cfg->du_configs[i].vlan = vlans[i];
        cfg->du_configs[i].active = true;
    }
    cfg->num_dus = num_dus;
}

static void config_fini(struct middlebox_config *cfg)
{
    thor_cache_flush(cfg);
    rte_free(cfg->cache);
    cfg->cache = NULL;
}

static void free_all(struct rte_mbuf **bufs, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++)
        rte_pktmbuf_free(bufs[i]);
}

/*
 * Build an O-RAN fronthaul frame. Layout matches what the proxy parses:
 * ether | xran_ecpri_hdr | radio_app_common_hdr | data_section_hdr | IQ.
 * The VLAN tag is assumed already stripped by the NIC, as on the real port.
 */
static struct rte_mbuf *make_packet(const struct rte_ether_addr *src,
                                    const struct rte_ether_addr *dst,
                                    uint8_t msg_type, uint16_t ru_port_id,
                                    uint16_t subframe, uint16_t slot, uint16_t symbol,
                                    uint16_t iq_len, uint8_t iq_fill)
{
    struct rte_mbuf *m = rte_pktmbuf_alloc(g_pool);
    if (m == NULL)
        rte_exit(EXIT_FAILURE, "mbuf pool exhausted\n");

    uint16_t total = (uint16_t)(IQ_OFFSET + iq_len);
    char *data = rte_pktmbuf_append(m, total);
    if (data == NULL)
        rte_exit(EXIT_FAILURE, "mbuf too small for %u bytes\n", total);
    memset(data, 0, total);

    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)data;
    rte_ether_addr_copy(src, &eth->src_addr);
    rte_ether_addr_copy(dst, &eth->dst_addr);
    eth->ether_type = rte_cpu_to_be_16(0xAEFE);

    struct xran_ecpri_hdr *ecpri =
        (struct xran_ecpri_hdr *)(data + sizeof(struct rte_ether_hdr));
    ecpri->cmnhdr.bits.ecpri_ver = 1;
    ecpri->cmnhdr.bits.ecpri_mesg_type = msg_type;
    ecpri->cmnhdr.bits.ecpri_payl_size = rte_cpu_to_be_16((uint16_t)(total - sizeof(struct rte_ether_hdr) - 4));
    ecpri->ecpri_xtc_id = rte_cpu_to_be_16(ru_port_id & 0x000F);

    struct radio_app_common_hdr *app =
        (struct radio_app_common_hdr *)(data + RADIO_HDR_END - sizeof(struct radio_app_common_hdr));
    app->data_feature.data_direction = 1;
    app->data_feature.payl_ver = 1;
    app->sf_slot_sym.value = rte_cpu_to_be_16((uint16_t)((subframe << 12) | (slot << 6) | symbol));

    if (iq_len > 0)
        memset(data + IQ_OFFSET, iq_fill, iq_len);

    return m;
}

/* IQ payload with a distinct byte pattern at one PRB, zeroes elsewhere --
 * mimics a DU that was scheduled exactly one PRB. */
static struct rte_mbuf *make_uplane_prb(const struct rte_ether_addr *src,
                                        uint16_t subframe, uint16_t slot, uint16_t symbol,
                                        int num_prbs, int owned_prb, uint8_t pattern)
{
    struct rte_mbuf *m = make_packet(src, &MB_MAC, ECPRI_IQ_DATA, 0,
                                     subframe, slot, symbol,
                                     (uint16_t)(num_prbs * PRB_9_SIZE), 0);
    uint8_t *iq = rte_pktmbuf_mtod_offset(m, uint8_t *, IQ_OFFSET);
    memset(iq + owned_prb * PRB_9_SIZE, pattern, PRB_9_SIZE);
    return m;
}

static const struct rte_ether_hdr *eth_of(const struct rte_mbuf *m)
{
    return rte_pktmbuf_mtod(m, const struct rte_ether_hdr *);
}

/* ------------------------------------------------------------------------- */
/* downlink: C-plane                                                         */
/* ------------------------------------------------------------------------- */

/* A single DU's C-plane packet is forwarded to the RU immediately. */
static void test_dl_cplane_single_du(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 1);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    struct rte_mbuf *m = make_packet(&DU0_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, 3, 32, 0xAA);
    thor_handle_rx_packet(&cfg, m, tx, &tx_idx);

    CHECK_EQ(tx_idx, 1);
    if (tx_idx == 1)
    {
        CHECK(rte_is_same_ether_addr(&eth_of(tx[0])->dst_addr, &RU_MAC));
        CHECK(rte_is_same_ether_addr(&eth_of(tx[0])->src_addr, &MB_MAC));
    }
    CHECK_EQ(cfg.stats.dl_cp_from_du[0], 1);
    CHECK_EQ(cfg.stats.dl_to_ru, 1);

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/* With two DUs, the RU sees nothing until both have sent for that slot. */
static void test_dl_cplane_waits_for_all_dus(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 2);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    thor_handle_rx_packet(&cfg, make_packet(&DU0_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, 3, 32, 0xAA), tx, &tx_idx);
    CHECK_EQ(tx_idx, 0); /* still waiting on DU1 */

    thor_handle_rx_packet(&cfg, make_packet(&DU1_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, 3, 32, 0xBB), tx, &tx_idx);
    CHECK_EQ(tx_idx, 1);

    CHECK_EQ(cfg.stats.dl_cp_from_du[0], 1);
    CHECK_EQ(cfg.stats.dl_cp_from_du[1], 1);
    CHECK_EQ(cfg.stats.dl_to_ru, 1);

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/* Packets for different (symbol, subframe, slot) keys are tracked separately. */
static void test_dl_distinct_cache_keys(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 2);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    thor_handle_rx_packet(&cfg, make_packet(&DU0_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, 3, 32, 0xAA), tx, &tx_idx);
    thor_handle_rx_packet(&cfg, make_packet(&DU1_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, 4, 32, 0xBB), tx, &tx_idx);
    CHECK_EQ(tx_idx, 0); /* different symbols, neither key is complete */

    thor_handle_rx_packet(&cfg, make_packet(&DU1_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, 3, 32, 0xBB), tx, &tx_idx);
    thor_handle_rx_packet(&cfg, make_packet(&DU0_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, 4, 32, 0xAA), tx, &tx_idx);
    CHECK_EQ(tx_idx, 2);

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/* C-plane and U-plane for the same slot occupy different cache entries. */
static void test_dl_cplane_uplane_do_not_collide(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 2);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    thor_handle_rx_packet(&cfg, make_packet(&DU0_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, 3, 32, 0xAA), tx, &tx_idx);
    thor_handle_rx_packet(&cfg, make_packet(&DU0_MAC, &MB_MAC, ECPRI_IQ_DATA, 0, 1, 0, 3, 32, 0xAA), tx, &tx_idx);
    CHECK_EQ(tx_idx, 0);
    CHECK_EQ(cfg.stats.cache_evictions, 0); /* neither displaced the other */

    thor_handle_rx_packet(&cfg, make_packet(&DU1_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, 3, 32, 0xBB), tx, &tx_idx);
    thor_handle_rx_packet(&cfg, make_packet(&DU1_MAC, &MB_MAC, ECPRI_IQ_DATA, 0, 1, 0, 3, 32, 0xBB), tx, &tx_idx);
    CHECK_EQ(tx_idx, 2);

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/* A DU repeating the same key evicts the stale entry rather than double-counting. */
static void test_dl_duplicate_evicts_entry(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 2);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    thor_handle_rx_packet(&cfg, make_packet(&DU0_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, 3, 32, 0xAA), tx, &tx_idx);
    thor_handle_rx_packet(&cfg, make_packet(&DU0_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, 3, 32, 0xAA), tx, &tx_idx);
    CHECK_EQ(tx_idx, 0);
    CHECK_EQ(cfg.stats.cache_evictions, 1);

    /* After the eviction only DU0's second packet is held, so DU1 completes it. */
    thor_handle_rx_packet(&cfg, make_packet(&DU1_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, 3, 32, 0xBB), tx, &tx_idx);
    CHECK_EQ(tx_idx, 1);

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/* ------------------------------------------------------------------------- */
/* downlink: U-plane IQ merge                                                */
/* ------------------------------------------------------------------------- */

/* Two DUs each own one PRB; the merged packet must carry both. */
static void test_dl_uplane_iq_merge(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 2);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;
    const int num_prbs = 106;

    thor_handle_rx_packet(&cfg, make_uplane_prb(&DU0_MAC, 1, 0, 3, num_prbs, 0, 0xA5), tx, &tx_idx);
    thor_handle_rx_packet(&cfg, make_uplane_prb(&DU1_MAC, 1, 0, 3, num_prbs, 7, 0x5A), tx, &tx_idx);

    CHECK_EQ(tx_idx, 1);
    if (tx_idx == 1)
    {
        const uint8_t *iq = rte_pktmbuf_mtod_offset(tx[0], const uint8_t *, IQ_OFFSET);
        CHECK_EQ(rte_pktmbuf_pkt_len(tx[0]), IQ_OFFSET + num_prbs * PRB_9_SIZE);

        for (int b = 0; b < PRB_9_SIZE; b++)
        {
            CHECK_EQ(iq[0 * PRB_9_SIZE + b], 0xA5); /* DU0's PRB survived */
            CHECK_EQ(iq[7 * PRB_9_SIZE + b], 0x5A); /* DU1's PRB was merged in */
        }
        /* An unscheduled PRB stays empty. */
        for (int b = 0; b < PRB_9_SIZE; b++)
            CHECK_EQ(iq[3 * PRB_9_SIZE + b], 0x00);
    }

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/* The merge is a bitwise OR, including where two DUs overlap on a PRB. */
static void test_dl_uplane_merge_is_bitwise_or(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 2);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    thor_handle_rx_packet(&cfg, make_uplane_prb(&DU0_MAC, 1, 0, 3, 4, 1, 0xF0), tx, &tx_idx);
    thor_handle_rx_packet(&cfg, make_uplane_prb(&DU1_MAC, 1, 0, 3, 4, 1, 0x0F), tx, &tx_idx);

    CHECK_EQ(tx_idx, 1);
    if (tx_idx == 1)
    {
        const uint8_t *iq = rte_pktmbuf_mtod_offset(tx[0], const uint8_t *, IQ_OFFSET);
        for (int b = 0; b < PRB_9_SIZE; b++)
            CHECK_EQ(iq[1 * PRB_9_SIZE + b], 0xFF);
    }

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/*
 * Exercise both halves of the merge loop: a payload that is not a multiple of
 * 64 bytes takes the AVX-512 path plus the scalar tail.
 */
static void test_dl_uplane_merge_unaligned_tail(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 2);

    /* 5 PRBs * 28 = 140 bytes: two 64-byte blocks plus a 12-byte tail. */
    const uint16_t iq_len = 5 * PRB_9_SIZE;
    CHECK_EQ(iq_len % 64, 12);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    struct rte_mbuf *a = make_packet(&DU0_MAC, &MB_MAC, ECPRI_IQ_DATA, 0, 1, 0, 3, iq_len, 0xF0);
    struct rte_mbuf *b = make_packet(&DU1_MAC, &MB_MAC, ECPRI_IQ_DATA, 0, 1, 0, 3, iq_len, 0x0F);
    thor_handle_rx_packet(&cfg, a, tx, &tx_idx);
    thor_handle_rx_packet(&cfg, b, tx, &tx_idx);

    CHECK_EQ(tx_idx, 1);
    if (tx_idx == 1)
    {
        const uint8_t *iq = rte_pktmbuf_mtod_offset(tx[0], const uint8_t *, IQ_OFFSET);
        for (uint16_t i = 0; i < iq_len; i++)
            CHECK_EQ(iq[i], 0xFF);
    }

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/* DUs scheduled on several disjoint PRBs each interleave correctly. */
static void test_dl_uplane_merge_disjoint_prb_blocks(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 2);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;
    const int num_prbs = 8;

    /* DU0 owns the even PRBs, DU1 the odd ones. */
    struct rte_mbuf *a = make_packet(&DU0_MAC, &MB_MAC, ECPRI_IQ_DATA, 0, 1, 0, 3,
                                     (uint16_t)(num_prbs * PRB_9_SIZE), 0);
    struct rte_mbuf *b = make_packet(&DU1_MAC, &MB_MAC, ECPRI_IQ_DATA, 0, 1, 0, 3,
                                     (uint16_t)(num_prbs * PRB_9_SIZE), 0);
    uint8_t *iq_a = rte_pktmbuf_mtod_offset(a, uint8_t *, IQ_OFFSET);
    uint8_t *iq_b = rte_pktmbuf_mtod_offset(b, uint8_t *, IQ_OFFSET);
    for (int prb = 0; prb < num_prbs; prb++)
        memset((prb % 2 == 0 ? iq_a : iq_b) + prb * PRB_9_SIZE,
               (uint8_t)(0xA0 + prb), PRB_9_SIZE);

    thor_handle_rx_packet(&cfg, a, tx, &tx_idx);
    CHECK_EQ(tx_idx, 0);
    thor_handle_rx_packet(&cfg, b, tx, &tx_idx);
    CHECK_EQ(tx_idx, 1);

    if (tx_idx == 1)
    {
        const uint8_t *iq = rte_pktmbuf_mtod_offset(tx[0], const uint8_t *, IQ_OFFSET);
        for (int prb = 0; prb < num_prbs; prb++)
            for (int b_off = 0; b_off < PRB_9_SIZE; b_off++)
                CHECK_EQ(iq[prb * PRB_9_SIZE + b_off], (uint8_t)(0xA0 + prb));
    }

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/* A DU sending a shorter payload must not make the merge read past its end. */
static void test_dl_uplane_merge_mismatched_lengths(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 2);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    thor_handle_rx_packet(&cfg, make_packet(&DU0_MAC, &MB_MAC, ECPRI_IQ_DATA, 0, 1, 0, 3, 8 * PRB_9_SIZE, 0xF0), tx, &tx_idx);
    thor_handle_rx_packet(&cfg, make_packet(&DU1_MAC, &MB_MAC, ECPRI_IQ_DATA, 0, 1, 0, 3, 2 * PRB_9_SIZE, 0x0F), tx, &tx_idx);

    CHECK_EQ(tx_idx, 1);
    if (tx_idx == 1)
    {
        /* The base packet keeps its own length ... */
        CHECK_EQ(rte_pktmbuf_pkt_len(tx[0]), IQ_OFFSET + 8 * PRB_9_SIZE);
        const uint8_t *iq = rte_pktmbuf_mtod_offset(tx[0], const uint8_t *, IQ_OFFSET);
        /* ... merged where the shorter payload overlapped ... */
        for (int i = 0; i < 2 * PRB_9_SIZE; i++)
            CHECK_EQ(iq[i], 0xFF);
        /* ... and untouched beyond it. */
        for (int i = 2 * PRB_9_SIZE; i < 8 * PRB_9_SIZE; i++)
            CHECK_EQ(iq[i], 0xF0);
    }

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/* ------------------------------------------------------------------------- */
/* uplink                                                                    */
/* ------------------------------------------------------------------------- */

/* A PUSCH packet from the RU is replicated to every active DU, retargeted. */
static void test_ul_pusch_fanout(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 2);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    struct rte_mbuf *m = make_packet(&RU_MAC, &MB_MAC, ECPRI_IQ_DATA,
                                     0 /* < MAX_PDSCH_PUSCH_PORT: PUSCH */,
                                     1, 0, 3, 64, 0xC3);
    thor_handle_rx_packet(&cfg, m, tx, &tx_idx);

    CHECK_EQ(tx_idx, 2);
    if (tx_idx == 2)
    {
        CHECK(rte_is_same_ether_addr(&eth_of(tx[0])->dst_addr, &DU0_MAC));
        CHECK(rte_is_same_ether_addr(&eth_of(tx[1])->dst_addr, &DU1_MAC));
        CHECK(rte_is_same_ether_addr(&eth_of(tx[0])->src_addr, &MB_MAC));
        CHECK(rte_is_same_ether_addr(&eth_of(tx[1])->src_addr, &MB_MAC));
    }
    CHECK_EQ(cfg.stats.ul_from_ru, 1);
    CHECK_EQ(cfg.stats.ul_to_du[0], 1);
    CHECK_EQ(cfg.stats.ul_to_du[1], 1);

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/* PRACH (ru_port_id >= MAX_PDSCH_PUSCH_PORT) fans out the same way. */
static void test_ul_prach_fanout(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 2);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    struct rte_mbuf *m = make_packet(&RU_MAC, &MB_MAC, ECPRI_IQ_DATA,
                                     MAX_PDSCH_PUSCH_PORT, 1, 0, 3, 12 * PRB_9_SIZE, 0x77);
    thor_handle_rx_packet(&cfg, m, tx, &tx_idx);

    CHECK_EQ(tx_idx, 2);
    CHECK_EQ(cfg.stats.ul_to_du[0], 1);
    CHECK_EQ(cfg.stats.ul_to_du[1], 1);
    if (tx_idx == 2)
    {
        CHECK(rte_is_same_ether_addr(&eth_of(tx[0])->dst_addr, &DU0_MAC));
        CHECK(rte_is_same_ether_addr(&eth_of(tx[1])->dst_addr, &DU1_MAC));
    }

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/* The UL copy carries the payload through unchanged. */
static void test_ul_payload_preserved(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 1);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    struct rte_mbuf *m = make_packet(&RU_MAC, &MB_MAC, ECPRI_IQ_DATA, 0, 2, 1, 9, 128, 0x9E);
    thor_handle_rx_packet(&cfg, m, tx, &tx_idx);

    CHECK_EQ(tx_idx, 1);
    if (tx_idx == 1)
    {
        CHECK_EQ(rte_pktmbuf_pkt_len(tx[0]), IQ_OFFSET + 128);
        const uint8_t *iq = rte_pktmbuf_mtod_offset(tx[0], const uint8_t *, IQ_OFFSET);
        for (int i = 0; i < 128; i++)
            CHECK_EQ(iq[i], 0x9E);
    }

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/* ------------------------------------------------------------------------- */
/* VLAN retagging                                                            */
/* ------------------------------------------------------------------------- */

/* A tagged downlink packet leaves carrying the RU's VLAN. */
static void test_vlan_retag_downlink(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 1);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    struct rte_mbuf *m = make_packet(&DU0_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, 3, 32, 0xAA);
    m->ol_flags |= RTE_MBUF_F_RX_VLAN | RTE_MBUF_F_RX_VLAN_STRIPPED;
    m->vlan_tci = DU0_VLAN;
    thor_handle_rx_packet(&cfg, m, tx, &tx_idx);

    CHECK_EQ(tx_idx, 1);
    if (tx_idx == 1)
    {
        CHECK(tx[0]->ol_flags & RTE_MBUF_F_TX_VLAN);
        CHECK_EQ(tx[0]->vlan_tci, RU_VLAN);
    }

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/* Each uplink copy is tagged with its own destination DU's VLAN. */
static void test_vlan_retag_uplink_per_du(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 2);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    struct rte_mbuf *m = make_packet(&RU_MAC, &MB_MAC, ECPRI_IQ_DATA, 0, 1, 0, 3, 64, 0x11);
    m->ol_flags |= RTE_MBUF_F_RX_VLAN | RTE_MBUF_F_RX_VLAN_STRIPPED;
    m->vlan_tci = RU_VLAN;
    thor_handle_rx_packet(&cfg, m, tx, &tx_idx);

    CHECK_EQ(tx_idx, 2);
    if (tx_idx == 2)
    {
        CHECK_EQ(tx[0]->vlan_tci, DU0_VLAN);
        CHECK_EQ(tx[1]->vlan_tci, DU1_VLAN);
    }

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/* An untagged ingress packet must not acquire a tag on egress. */
static void test_vlan_untagged_stays_untagged(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 1);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    thor_handle_rx_packet(&cfg, make_packet(&DU0_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, 3, 32, 0xAA), tx, &tx_idx);

    CHECK_EQ(tx_idx, 1);
    if (tx_idx == 1)
        CHECK_EQ((tx[0]->ol_flags & RTE_MBUF_F_TX_VLAN) != 0, 0);

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/* ------------------------------------------------------------------------- */
/* filtering and malformed input                                             */
/* ------------------------------------------------------------------------- */

/* Traffic from an unknown source is dropped. */
static void test_drop_unknown_source(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 2);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    thor_handle_rx_packet(&cfg, make_packet(&OTHER_MAC, &MB_MAC, ECPRI_IQ_DATA, 0, 1, 0, 3, 32, 0), tx, &tx_idx);

    CHECK_EQ(tx_idx, 0);
    CHECK_EQ(cfg.stats.dropped_unmatched, 1);

    config_fini(&cfg);
}

/* Traffic not addressed to the middlebox is dropped, in both directions. */
static void test_drop_not_addressed_to_middlebox(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 2);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    thor_handle_rx_packet(&cfg, make_packet(&DU0_MAC, &OTHER_MAC, ECPRI_IQ_DATA, 0, 1, 0, 3, 32, 0), tx, &tx_idx);
    thor_handle_rx_packet(&cfg, make_packet(&RU_MAC, &OTHER_MAC, ECPRI_IQ_DATA, 0, 1, 0, 3, 32, 0), tx, &tx_idx);

    CHECK_EQ(tx_idx, 0);
    CHECK_EQ(cfg.stats.dropped_unmatched, 2);

    config_fini(&cfg);
}

/*
 * Header fields are wider on the wire than the cache dimensions they index:
 * slot_id and symb_id are 6 bits (vs NUM_SLOTS=2, NUM_SYMBOLS=14), subframe_id
 * is 4 bits (vs NUM_SUBFRAMES=10) and ru_port_id is 4 bits (vs
 * NUM_ANTENNA_PORTS=8). Out-of-range values must be dropped, not indexed.
 */
static void test_drop_out_of_range_header_fields(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 1);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    thor_handle_rx_packet(&cfg, make_packet(&DU0_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, NUM_SLOTS, 3, 32, 0), tx, &tx_idx);
    thor_handle_rx_packet(&cfg, make_packet(&DU0_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, NUM_SYMBOLS, 32, 0), tx, &tx_idx);
    thor_handle_rx_packet(&cfg, make_packet(&DU0_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, NUM_SUBFRAMES, 0, 3, 32, 0), tx, &tx_idx);
    thor_handle_rx_packet(&cfg, make_packet(&DU0_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, NUM_ANTENNA_PORTS, 1, 0, 3, 32, 0), tx, &tx_idx);

    CHECK_EQ(tx_idx, 0);
    CHECK_EQ(cfg.stats.dropped_malformed, 4);

    config_fini(&cfg);
}

/* eCPRI message types the cache has no room for must be dropped. */
static void test_drop_unsupported_ecpri_type(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 1);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    /* 0x05 = one-way delay measurement, beyond the cache's ECPRI_TYPE bound. */
    thor_handle_rx_packet(&cfg, make_packet(&DU0_MAC, &MB_MAC, 0x05, 0, 1, 0, 3, 32, 0), tx, &tx_idx);

    CHECK_EQ(tx_idx, 0);
    CHECK_EQ(cfg.stats.dropped_malformed, 1);

    config_fini(&cfg);
}

/* A runt frame must be dropped before any header is dereferenced. */
static void test_drop_runt_frame(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 1);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    struct rte_mbuf *m = rte_pktmbuf_alloc(g_pool);
    char *data = rte_pktmbuf_append(m, 20);
    if (data == NULL)
        rte_exit(EXIT_FAILURE, "cannot append to mbuf\n");
    memset(data, 0, 20);
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)data;
    rte_ether_addr_copy(&DU0_MAC, &eth->src_addr);
    rte_ether_addr_copy(&MB_MAC, &eth->dst_addr);

    thor_handle_rx_packet(&cfg, m, tx, &tx_idx);

    CHECK_EQ(tx_idx, 0);
    CHECK_EQ(cfg.stats.dropped_malformed, 1);

    config_fini(&cfg);
}

/* ------------------------------------------------------------------------- */
/* dynamic L1 add/remove                                                     */
/* ------------------------------------------------------------------------- */

static void test_du_add_and_remove(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 0);

    CHECK_EQ(cfg.num_dus, 0);

    int slot0 = thor_du_add(&cfg, &DU0_MAC, DU0_VLAN);
    CHECK_EQ(slot0, 0);
    CHECK_EQ(cfg.num_dus, 1);

    int slot1 = thor_du_add(&cfg, &DU1_MAC, DU1_VLAN);
    CHECK_EQ(slot1, 1);
    CHECK_EQ(cfg.num_dus, 2);

    /* Adding the same MAC twice is refused. */
    CHECK_EQ(thor_du_add(&cfg, &DU0_MAC, DU0_VLAN), -EEXIST);
    CHECK_EQ(cfg.num_dus, 2);

    CHECK_EQ(thor_du_del(&cfg, &DU0_MAC), 0);
    CHECK_EQ(cfg.num_dus, 1);

    /* Removing an unknown MAC is refused. */
    CHECK_EQ(thor_du_del(&cfg, &DU0_MAC), -ENOENT);
    CHECK_EQ(thor_du_del(&cfg, &OTHER_MAC), -ENOENT);

    config_fini(&cfg);
}

static void test_du_slots_are_reused(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 0);

    CHECK_EQ(thor_du_add(&cfg, &DU0_MAC, DU0_VLAN), 0);
    CHECK_EQ(thor_du_add(&cfg, &DU1_MAC, DU1_VLAN), 1);
    CHECK_EQ(thor_du_del(&cfg, &DU0_MAC), 0);

    /* Slot 0 is free again and taken by the next arrival. */
    CHECK_EQ(thor_du_add(&cfg, &DU2_MAC, DU2_VLAN), 0);
    CHECK_EQ(cfg.num_dus, 2);

    config_fini(&cfg);
}

static void test_du_table_full(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 0);

    struct rte_ether_addr mac = DU0_MAC;
    for (int i = 0; i < MAX_NUM_DUS; i++)
    {
        mac.addr_bytes[5] = (uint8_t)(0x20 + i);
        CHECK_EQ(thor_du_add(&cfg, &mac, 5), i);
    }
    CHECK_EQ(cfg.num_dus, MAX_NUM_DUS);

    mac.addr_bytes[5] = 0xEE;
    CHECK_EQ(thor_du_add(&cfg, &mac, 5), -ENOSPC);
    CHECK_EQ(cfg.num_dus, MAX_NUM_DUS);

    config_fini(&cfg);
}

/*
 * A DU joining drops the half-filled merge state that was keyed to the old DU
 * count, so a stale contribution can never be counted towards the new set.
 */
static void test_du_add_flushes_cache(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 2);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;
    unsigned int free_before = rte_mempool_avail_count(g_pool);

    thor_handle_rx_packet(&cfg, make_packet(&DU0_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, 3, 32, 0xAA), tx, &tx_idx);
    CHECK_EQ(tx_idx, 0); /* DU0's packet is cached, waiting for DU1 */

    /* DU1 leaves and rejoins. Both transitions must flush, and the packet DU0
     * parked under the old membership must be released, not left to complete. */
    uint64_t flushes_before = cfg.stats.cache_flushes;
    CHECK_EQ(thor_du_del(&cfg, &DU1_MAC), 1);
    CHECK_EQ(thor_du_add(&cfg, &DU1_MAC, DU1_VLAN), 1);
    CHECK_EQ(cfg.stats.cache_flushes, flushes_before + 2);
    CHECK_EQ(rte_mempool_avail_count(g_pool), free_before);
    CHECK_EQ(cfg.num_dus, 2);

    /* DU1 alone is not enough: DU0's contribution was flushed and must be resent. */
    thor_handle_rx_packet(&cfg, make_packet(&DU1_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, 3, 32, 0xBB), tx, &tx_idx);
    CHECK_EQ(tx_idx, 0);

    thor_handle_rx_packet(&cfg, make_packet(&DU0_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, 3, 32, 0xAA), tx, &tx_idx);
    CHECK_EQ(tx_idx, 1);

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/* Removing a DU must not wedge a slot that was waiting on it. */
static void test_du_remove_unblocks_pending_merge(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 2);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    thor_handle_rx_packet(&cfg, make_packet(&DU0_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, 3, 32, 0xAA), tx, &tx_idx);
    CHECK_EQ(tx_idx, 0); /* waiting for DU1 */

    /* DU1 goes away. With one DU left, DU0's next packet must go straight out. */
    CHECK_EQ(thor_du_del(&cfg, &DU1_MAC), 1);
    CHECK_EQ(cfg.num_dus, 1);

    thor_handle_rx_packet(&cfg, make_packet(&DU0_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, 3, 32, 0xAA), tx, &tx_idx);
    CHECK_EQ(tx_idx, 1);

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/* After a removal, the ex-DU's traffic is treated as unknown. */
static void test_du_removed_traffic_is_dropped(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 2);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    thor_du_del(&cfg, &DU1_MAC);
    thor_handle_rx_packet(&cfg, make_packet(&DU1_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, 3, 32, 0xBB), tx, &tx_idx);

    CHECK_EQ(tx_idx, 0);
    CHECK_EQ(cfg.stats.dropped_unmatched, 1);

    /* And uplink no longer fans out to it. */
    thor_handle_rx_packet(&cfg, make_packet(&RU_MAC, &MB_MAC, ECPRI_IQ_DATA, 0, 1, 0, 3, 64, 0), tx, &tx_idx);
    CHECK_EQ(tx_idx, 1);

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/*
 * The merge picks its base packet from the lowest *occupied* slot, not slot 0.
 * After the DU in slot 0 leaves, the remaining DU sits in slot 1 and its
 * downlink must still be forwarded.
 */
static void test_merge_with_sparse_slots(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 2);

    CHECK_EQ(thor_du_del(&cfg, &DU0_MAC), 0);
    CHECK_EQ(cfg.num_dus, 1);
    CHECK(!cfg.du_configs[0].active);
    CHECK(cfg.du_configs[1].active);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    thor_handle_rx_packet(&cfg, make_uplane_prb(&DU1_MAC, 1, 0, 3, 8, 2, 0x3C), tx, &tx_idx);

    CHECK_EQ(tx_idx, 1);
    if (tx_idx == 1)
    {
        const uint8_t *iq = rte_pktmbuf_mtod_offset(tx[0], const uint8_t *, IQ_OFFSET);
        CHECK_EQ(iq[2 * PRB_9_SIZE], 0x3C);
        CHECK(rte_is_same_ether_addr(&eth_of(tx[0])->dst_addr, &RU_MAC));
    }

    /* And a DU rejoining lands back in the free slot 0, restoring 2-DU merging. */
    CHECK_EQ(thor_du_add(&cfg, &DU2_MAC, DU2_VLAN), 0);
    tx_idx = 0;
    thor_handle_rx_packet(&cfg, make_uplane_prb(&DU1_MAC, 1, 0, 4, 8, 0, 0x0F), tx, &tx_idx);
    CHECK_EQ(tx_idx, 0);
    thor_handle_rx_packet(&cfg, make_uplane_prb(&DU2_MAC, 1, 0, 4, 8, 0, 0xF0), tx, &tx_idx);
    CHECK_EQ(tx_idx, 1);
    if (tx_idx == 1)
    {
        const uint8_t *iq = rte_pktmbuf_mtod_offset(tx[0], const uint8_t *, IQ_OFFSET);
        CHECK_EQ(iq[0], 0xFF); /* both contributions present */
    }

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/* ------------------------------------------------------------------------- */
/* resource accounting                                                       */
/* ------------------------------------------------------------------------- */

/* Sustained traffic must not leak mbufs. */
static void test_no_mbuf_leak_under_load(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 2);

    unsigned int free_before = rte_mempool_avail_count(g_pool);

    for (int round = 0; round < 200; round++)
    {
        struct rte_mbuf *tx[MAX_TX_BURST];
        uint16_t tx_idx = 0;
        uint16_t symbol = (uint16_t)(round % NUM_SYMBOLS);

        thor_handle_rx_packet(&cfg, make_uplane_prb(&DU0_MAC, 1, 0, symbol, 16, 0, 0x11), tx, &tx_idx);
        thor_handle_rx_packet(&cfg, make_uplane_prb(&DU1_MAC, 1, 0, symbol, 16, 1, 0x22), tx, &tx_idx);
        thor_handle_rx_packet(&cfg, make_packet(&RU_MAC, &MB_MAC, ECPRI_IQ_DATA, 0, 1, 0, symbol, 64, 0x33), tx, &tx_idx);
        thor_handle_rx_packet(&cfg, make_packet(&OTHER_MAC, &MB_MAC, ECPRI_IQ_DATA, 0, 1, 0, symbol, 64, 0), tx, &tx_idx);

        CHECK_EQ(tx_idx, 3); /* 1 merged downlink + 2 uplink copies */
        free_all(tx, tx_idx);
    }

    CHECK_EQ(cfg.stats.dl_to_ru, 200);
    CHECK_EQ(cfg.stats.dropped_unmatched, 200);

    config_fini(&cfg);
    CHECK_EQ(rte_mempool_avail_count(g_pool), free_before);
}

/* Packets left parked in the cache are released on flush, not leaked. */
static void test_cache_flush_releases_mbufs(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 2);

    unsigned int free_before = rte_mempool_avail_count(g_pool);

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;
    for (uint16_t symbol = 0; symbol < NUM_SYMBOLS; symbol++)
        thor_handle_rx_packet(&cfg, make_packet(&DU0_MAC, &MB_MAC, ECPRI_RT_CONTROL_DATA, 0, 1, 0, symbol, 32, 0xAA), tx, &tx_idx);

    CHECK_EQ(tx_idx, 0);
    CHECK(rte_mempool_avail_count(g_pool) < free_before); /* held in the cache */

    thor_cache_flush(&cfg);
    CHECK_EQ(rte_mempool_avail_count(g_pool), free_before);

    config_fini(&cfg);
}

/* A full RX burst of uplink packets to a full DU table must fit in the TX array. */
static void test_tx_burst_bound(void)
{
    struct middlebox_config cfg;
    config_init(&cfg, 0);

    struct rte_ether_addr mac = DU0_MAC;
    for (int i = 0; i < MAX_NUM_DUS; i++)
    {
        mac.addr_bytes[5] = (uint8_t)(0x20 + i);
        thor_du_add(&cfg, &mac, 5);
    }

    struct rte_mbuf *tx[MAX_TX_BURST];
    uint16_t tx_idx = 0;

    for (int i = 0; i < BURST_SIZE; i++)
        thor_handle_rx_packet(&cfg, make_packet(&RU_MAC, &MB_MAC, ECPRI_IQ_DATA, 0, 1, 0, 3, 64, 0), tx, &tx_idx);

    CHECK_EQ(tx_idx, BURST_SIZE * MAX_NUM_DUS);
    CHECK(tx_idx <= MAX_TX_BURST);

    free_all(tx, tx_idx);
    config_fini(&cfg);
}

/* ------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "EAL initialisation failed\n");

    g_pool = rte_pktmbuf_pool_create("TEST_POOL", 16383, 32, 0,
                                     ETHER_JUMBO_FRAME_SIZE + RTE_PKTMBUF_HEADROOM + 100,
                                     SOCKET_ID_ANY);
    if (g_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create test mbuf pool: %s\n", rte_strerror(rte_errno));

    printf("\nthor_fhaul_proxy datapath unit tests\n\n");

    printf("downlink C-plane:\n");
    RUN(test_dl_cplane_single_du);
    RUN(test_dl_cplane_waits_for_all_dus);
    RUN(test_dl_distinct_cache_keys);
    RUN(test_dl_cplane_uplane_do_not_collide);
    RUN(test_dl_duplicate_evicts_entry);

    printf("downlink U-plane IQ merge:\n");
    RUN(test_dl_uplane_iq_merge);
    RUN(test_dl_uplane_merge_is_bitwise_or);
    RUN(test_dl_uplane_merge_unaligned_tail);
    RUN(test_dl_uplane_merge_disjoint_prb_blocks);
    RUN(test_dl_uplane_merge_mismatched_lengths);

    printf("uplink:\n");
    RUN(test_ul_pusch_fanout);
    RUN(test_ul_prach_fanout);
    RUN(test_ul_payload_preserved);

    printf("VLAN retagging:\n");
    RUN(test_vlan_retag_downlink);
    RUN(test_vlan_retag_uplink_per_du);
    RUN(test_vlan_untagged_stays_untagged);

    printf("filtering and malformed input:\n");
    RUN(test_drop_unknown_source);
    RUN(test_drop_not_addressed_to_middlebox);
    RUN(test_drop_out_of_range_header_fields);
    RUN(test_drop_unsupported_ecpri_type);
    RUN(test_drop_runt_frame);

    printf("dynamic L1 add/remove:\n");
    RUN(test_du_add_and_remove);
    RUN(test_du_slots_are_reused);
    RUN(test_du_table_full);
    RUN(test_du_add_flushes_cache);
    RUN(test_du_remove_unblocks_pending_merge);
    RUN(test_du_removed_traffic_is_dropped);
    RUN(test_merge_with_sparse_slots);

    printf("resource accounting:\n");
    RUN(test_no_mbuf_leak_under_load);
    RUN(test_cache_flush_releases_mbufs);
    RUN(test_tx_burst_bound);

    printf("\n%d tests, %d failed\n\n", tests_run, tests_failed);

    rte_eal_cleanup();
    return tests_failed == 0 ? 0 : 1;
}
