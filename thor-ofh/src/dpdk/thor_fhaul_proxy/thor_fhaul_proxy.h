/*
 * Copyright (c) Microsoft Corporation.
   Copyright (c) National University of Singapore.
 * Licensed under the MIT License
 *
 * Shared types and the packet-processing datapath for the Thor fronthaul proxy.
 *
 * The datapath lives here (rather than in thor_fhaul_proxy.c) so that it can be
 * driven directly by the unit tests in tests/unit without a NIC: everything in
 * this header operates on mbufs and an explicit state struct, never on a port.
 */

#ifndef THOR_FHAUL_PROXY_H_
#define THOR_FHAUL_PROXY_H_

#include <rte_common.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ether.h>
#include <stdbool.h>
#include <stdio.h>

#include "ranbooster_common.h"

#define RX_RING_SIZE 4096
#define TX_RING_SIZE 4096

#define NUM_MBUFS 8191 * 2
#define MBUF_CACHE_SIZE 32
#define BURST_SIZE 32

/* Number of DU (L1) slots the proxy can hold. Slots are allocated and released
 * at runtime through the control socket, so this is a ceiling, not a count: the
 * proxy may run with fewer, and which slots are occupied changes over time.
 * Everything below iterates over slots rather than assuming a count, so raising
 * this ceiling needs no other change. */
#define MAX_NUM_DUS 2

/* Worst case TX fan-out for one RX burst: every packet is an uplink packet that
 * gets replicated to every active DU. */
#define MAX_TX_BURST (BURST_SIZE * MAX_NUM_DUS)

#define VLAN_VID_MASK 0x0FFF
#define ETHER_JUMBO_FRAME_SIZE 9600

#define IQ_OFFSET (sizeof(struct rte_ether_hdr) +           \
                   sizeof(struct xran_ecpri_hdr) +          \
                   sizeof(struct radio_app_common_hdr) +    \
                   sizeof(struct data_section_hdr))

/* Smallest frame we can pull a slot/subframe/symbol out of. */
#define RADIO_HDR_END (sizeof(struct rte_ether_hdr) +       \
                       sizeof(struct xran_ecpri_hdr) +      \
                       sizeof(struct radio_app_common_hdr))

#define XRAN_CONVERT_NUMPRB(x) ((x) > 255 ? 0 : (x))

#define PRB_9_SIZE (1 + ((NUM_SUBCARRIERS_PRB * IQ_BIT_WIDTH_COMPRESSED * 2) / 8))
#define PRB_16_SIZE (NUM_SUBCARRIERS_PRB * 2 * 2)

/* eCPRI message types we cache: index 0 (ECPRI_IQ_DATA) and 2 (ECPRI_RT_CONTROL_DATA). */
#define ECPRI_TYPE 3

struct prb_9
{
    uint8_t data[PRB_9_SIZE];
};

struct prb_16
{
    uint8_t data[PRB_16_SIZE];
};

struct ru_config
{
    struct rte_ether_addr ru_addr;
    uint16_t vlan;
};

struct du_config
{
    struct rte_ether_addr du_addr;
    uint16_t vlan;
    bool active;
};

struct middlebox_stats
{
    uint64_t dl_cp_from_du[MAX_NUM_DUS];
    uint64_t dl_up_from_du[MAX_NUM_DUS];
    uint64_t dl_to_ru;
    uint64_t ul_from_ru;
    uint64_t ul_to_du[MAX_NUM_DUS];
    uint64_t dropped_unmatched;
    uint64_t dropped_malformed;
    uint64_t cache_evictions;
    uint64_t cache_flushes;
    uint64_t tx_failed;
    uint64_t alloc_failed;
};

/*
 * Per-(DU, eCPRI type, RU port, symbol, subframe, slot) landing area for
 * downlink packets waiting to be merged. Kept out of struct middlebox_config so
 * that a test can own several independent instances.
 */
struct middlebox_cache
{
    struct rte_mbuf *pkts[MAX_NUM_DUS][ECPRI_TYPE][NUM_ANTENNA_PORTS][NUM_SYMBOLS][NUM_SUBFRAMES][NUM_SLOTS];
    int num[ECPRI_TYPE][NUM_ANTENNA_PORTS][NUM_SYMBOLS][NUM_SUBFRAMES][NUM_SLOTS];
};

struct middlebox_config
{
    /* Number of *active* DU slots. The merge threshold is compared against this,
     * so it must always equal the number of du_configs[] entries with active set. */
    int num_dus;
    uint16_t xran_nprb;
    uint16_t num_prbs;
    uint16_t nic_port_id;
    struct rte_ether_addr middlebox_addr;
    struct ru_config ru_config;
    struct du_config du_configs[MAX_NUM_DUS];
    struct rte_mempool *mbuf_pool;
    struct middlebox_cache *cache;
    struct middlebox_stats stats;
};

/* ------------------------------------------------------------------------- */
/* DU slot management                                                        */
/* ------------------------------------------------------------------------- */

static inline int
thor_du_find_by_mac(const struct middlebox_config *config,
                    const struct rte_ether_addr *mac)
{
    for (int i = 0; i < MAX_NUM_DUS; i++)
    {
        if (config->du_configs[i].active &&
            rte_is_same_ether_addr(&config->du_configs[i].du_addr, mac))
            return i;
    }
    return -1;
}

/*
 * Drop every cached downlink packet. A change to the DU set invalidates all
 * in-flight merge state: entries are released only when their fill count
 * reaches config->num_dus, so a partially filled entry recorded under the old
 * DU count would otherwise either never complete or complete too early.
 */
static inline void
thor_cache_flush(struct middlebox_config *config)
{
    struct middlebox_cache *cache = config->cache;

    for (int du = 0; du < MAX_NUM_DUS; du++)
        for (int type = 0; type < ECPRI_TYPE; type++)
            for (int port = 0; port < NUM_ANTENNA_PORTS; port++)
                for (int sym = 0; sym < NUM_SYMBOLS; sym++)
                    for (int sf = 0; sf < NUM_SUBFRAMES; sf++)
                        for (int slot = 0; slot < NUM_SLOTS; slot++)
                        {
                            struct rte_mbuf *m = cache->pkts[du][type][port][sym][sf][slot];
                            if (m != NULL)
                            {
                                rte_pktmbuf_free(m);
                                cache->pkts[du][type][port][sym][sf][slot] = NULL;
                            }
                        }

    memset(cache->num, 0, sizeof(cache->num));
    config->stats.cache_flushes++;
}

/*
 * Add a DU. Returns the slot index, or a negative errno:
 *   -EEXIST if the MAC is already registered, -ENOSPC if every slot is taken.
 * Must only be called from the datapath lcore -- see thor_ctrl_poll().
 */
static inline int
thor_du_add(struct middlebox_config *config, const struct rte_ether_addr *mac,
            uint16_t vlan)
{
    if (thor_du_find_by_mac(config, mac) >= 0)
        return -EEXIST;

    for (int i = 0; i < MAX_NUM_DUS; i++)
    {
        if (!config->du_configs[i].active)
        {
            rte_ether_addr_copy(mac, &config->du_configs[i].du_addr);
            config->du_configs[i].vlan = vlan;
            config->du_configs[i].active = true;
            config->num_dus++;
            thor_cache_flush(config);
            return i;
        }
    }
    return -ENOSPC;
}

/*
 * Remove the DU with this MAC. Returns the freed slot index, or -ENOENT.
 * Must only be called from the datapath lcore -- see thor_ctrl_poll().
 */
static inline int
thor_du_del(struct middlebox_config *config, const struct rte_ether_addr *mac)
{
    int slot = thor_du_find_by_mac(config, mac);
    if (slot < 0)
        return -ENOENT;

    config->du_configs[slot].active = false;
    config->du_configs[slot].vlan = 0;
    memset(&config->du_configs[slot].du_addr, 0, sizeof(struct rte_ether_addr));
    config->num_dus--;
    thor_cache_flush(config);
    return slot;
}

/* ------------------------------------------------------------------------- */
/* Datapath                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Copy an mbuf and rewrite it for delivery to dst_mac, retagging with dst_vlan
 * if the ingress packet carried a VLAN tag. Returns NULL if the pool is empty.
 */
static inline struct rte_mbuf *
thor_clone_for(struct middlebox_config *config, struct rte_mbuf *buf,
               const struct rte_ether_addr *dst_mac, uint16_t dst_vlan)
{
    struct rte_mbuf *out = rte_pktmbuf_copy(buf, config->mbuf_pool, 0, UINT32_MAX);
    if (out == NULL)
    {
        config->stats.alloc_failed++;
        return NULL;
    }

    struct rte_ether_hdr *ethhdr = rte_pktmbuf_mtod(out, struct rte_ether_hdr *);
    rte_ether_addr_copy(dst_mac, &ethhdr->dst_addr);
    rte_ether_addr_copy(&config->middlebox_addr, &ethhdr->src_addr);

    if (out->ol_flags & RTE_MBUF_F_RX_VLAN)
    {
        out->ol_flags |= RTE_MBUF_F_TX_VLAN;
        out->vlan_tci = dst_vlan;
    }

    return out;
}

/*
 * Process one received packet.
 *
 * Takes ownership of buf: it is either freed, parked in the merge cache, or
 * (after being copied) appended to tx_bufs. tx_bufs must have room for
 * MAX_TX_BURST entries.
 */
static inline void
thor_handle_rx_packet(struct middlebox_config *config, struct rte_mbuf *buf,
                      struct rte_mbuf **tx_bufs, uint16_t *tx_idx)
{
    struct middlebox_cache *cache = config->cache;
    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(buf, struct rte_ether_hdr *);

    /* Anything shorter than this cannot carry a radio application header. */
    if (unlikely(rte_pktmbuf_pkt_len(buf) < RADIO_HDR_END))
    {
        config->stats.dropped_malformed++;
        rte_pktmbuf_free(buf);
        return;
    }

    /* ---------------- uplink: RU -> DUs ---------------- */
    if (rte_is_same_ether_addr(&config->ru_config.ru_addr, &eth_hdr->src_addr))
    {
        if (!rte_is_same_ether_addr(&config->middlebox_addr, &eth_hdr->dst_addr))
        {
            /* not addressed to the middlebox, ignore it */
            config->stats.dropped_unmatched++;
            rte_pktmbuf_free(buf);
            return;
        }

        config->stats.ul_from_ru++;

        struct xran_ecpri_hdr *ecpri_hdr =
            rte_pktmbuf_mtod_offset(buf, struct xran_ecpri_hdr *, sizeof(struct rte_ether_hdr));
        uint16_t ru_port_id = rte_be_to_cpu_16(ecpri_hdr->ecpri_xtc_id) & 0x000F;

        /*
         * PRACH (ru_port_id >= MAX_PDSCH_PUSCH_PORT) and PUSCH are both
         * broadcast to every active DU as-is. In OAI there is always an UL
         * packet for an UL slot, so no scheduling history is consulted; the
         * per-DU IQ routing hook goes here.
         */
        (void)ru_port_id;

        for (int du_idx = 0; du_idx < MAX_NUM_DUS; du_idx++)
        {
            if (!config->du_configs[du_idx].active)
                continue;

            struct rte_mbuf *du_buf = thor_clone_for(config, buf,
                                                     &config->du_configs[du_idx].du_addr,
                                                     config->du_configs[du_idx].vlan);
            if (du_buf == NULL)
                continue;

            tx_bufs[(*tx_idx)++] = du_buf;
            config->stats.ul_to_du[du_idx]++;
        }

        rte_pktmbuf_free(buf);
        return;
    }

    /* ---------------- downlink: DUs -> RU ---------------- */
    int du_idx = thor_du_find_by_mac(config, &eth_hdr->src_addr);
    if (du_idx < 0)
    {
        /* matched neither the RU nor any DU */
        config->stats.dropped_unmatched++;
        rte_pktmbuf_free(buf);
        return;
    }

    if (!rte_is_same_ether_addr(&config->middlebox_addr, &eth_hdr->dst_addr))
    {
        config->stats.dropped_unmatched++;
        rte_pktmbuf_free(buf);
        return;
    }

    struct xran_ecpri_hdr *ecpri_hdr =
        rte_pktmbuf_mtod_offset(buf, struct xran_ecpri_hdr *, sizeof(struct rte_ether_hdr));
    uint16_t ru_port_id = rte_be_to_cpu_16(ecpri_hdr->ecpri_xtc_id) & 0x000F;
    uint8_t ecpri_message_type = ecpri_hdr->cmnhdr.bits.ecpri_mesg_type;

    struct radio_app_common_hdr *app_common_hdr =
        rte_pktmbuf_mtod_offset(buf, struct radio_app_common_hdr *, RADIO_HDR_END - sizeof(struct radio_app_common_hdr));
    struct radio_app_common_hdr radio_hdr_cpy = *app_common_hdr;
    radio_hdr_cpy.sf_slot_sym.value = rte_be_to_cpu_16(radio_hdr_cpy.sf_slot_sym.value);

    uint16_t slot = radio_hdr_cpy.sf_slot_sym.slot_id;
    uint16_t subframe = radio_hdr_cpy.sf_slot_sym.subframe_id;
    uint16_t symbol = radio_hdr_cpy.sf_slot_sym.symb_id;

    /*
     * The cache is indexed directly by these header fields, all of which are
     * wider on the wire than the array dimensions they index. Reject anything
     * out of range instead of corrupting memory.
     */
    if (unlikely(ecpri_message_type >= ECPRI_TYPE ||
                 ru_port_id >= NUM_ANTENNA_PORTS ||
                 symbol >= NUM_SYMBOLS ||
                 subframe >= NUM_SUBFRAMES ||
                 slot >= NUM_SLOTS))
    {
        config->stats.dropped_malformed++;
        rte_pktmbuf_free(buf);
        return;
    }

    if (ecpri_message_type == ECPRI_IQ_DATA)
    {
        if (unlikely(rte_pktmbuf_pkt_len(buf) < IQ_OFFSET))
        {
            config->stats.dropped_malformed++;
            rte_pktmbuf_free(buf);
            return;
        }
        config->stats.dl_up_from_du[du_idx]++;
    }
    else
    {
        config->stats.dl_cp_from_du[du_idx]++;
    }

    rte_ether_addr_copy(&config->ru_config.ru_addr, &eth_hdr->dst_addr);
    rte_ether_addr_copy(&config->middlebox_addr, &eth_hdr->src_addr);

    if (buf->ol_flags & RTE_MBUF_F_RX_VLAN)
    {
        buf->ol_flags |= RTE_MBUF_F_TX_VLAN;
        buf->vlan_tci = config->ru_config.vlan;
    }

    /*
     * This DU already has a packet parked in this slot, so the previous round
     * never completed (a DU went quiet). Evict the whole entry and start over.
     */
    if (cache->pkts[du_idx][ecpri_message_type][ru_port_id][symbol][subframe][slot] != NULL)
    {
        for (int id = 0; id < MAX_NUM_DUS; id++)
        {
            struct rte_mbuf *stale = cache->pkts[id][ecpri_message_type][ru_port_id][symbol][subframe][slot];
            if (stale != NULL)
            {
                rte_pktmbuf_free(stale);
                cache->pkts[id][ecpri_message_type][ru_port_id][symbol][subframe][slot] = NULL;
            }
        }
        cache->num[ecpri_message_type][ru_port_id][symbol][subframe][slot] = 0;
        config->stats.cache_evictions++;
    }

    /*
     * The caching scheme is strictly for TDD: UL and DL C/U-plane data never
     * share a cache entry.
     */
    cache->pkts[du_idx][ecpri_message_type][ru_port_id][symbol][subframe][slot] = buf;
    cache->num[ecpri_message_type][ru_port_id][symbol][subframe][slot]++;

    if (cache->num[ecpri_message_type][ru_port_id][symbol][subframe][slot] != config->num_dus)
        return; /* still waiting on the other DUs */

    /* Every active DU has contributed -- build the merged packet for the RU.
     * Slot 0 is not necessarily populated: DU slots are allocated and released
     * at runtime, so start from the lowest slot that actually holds a packet. */
#define CACHED(_id) cache->pkts[_id][ecpri_message_type][ru_port_id][symbol][subframe][slot]

    int base_slot = -1;
    for (int id = 0; id < MAX_NUM_DUS; id++)
    {
        if (CACHED(id) != NULL)
        {
            base_slot = id;
            break;
        }
    }
    if (unlikely(base_slot < 0))
        return; /* unreachable: the fill count says there is at least one */

    struct rte_mbuf *new_buf = rte_pktmbuf_copy(CACHED(base_slot),
                                                config->mbuf_pool, 0, UINT32_MAX);
    if (unlikely(new_buf == NULL))
    {
        config->stats.alloc_failed++;
    }
    else
    {
        if (ecpri_message_type == ECPRI_IQ_DATA)
        {
            /*
             * Merge the IQ samples of the remaining DUs into the copy with a
             * bitwise OR. Each DU writes only the PRBs it was scheduled and
             * zeroes the rest, so OR reconstructs the full grid.
             */
            uint8_t *new_iq_ptr = rte_pktmbuf_mtod_offset(new_buf, uint8_t *, IQ_OFFSET);
            uint32_t new_iq_size = rte_pktmbuf_pkt_len(new_buf) - IQ_OFFSET;

            for (int id = base_slot + 1; id < MAX_NUM_DUS; id++)
            {
                struct rte_mbuf *du_buf = CACHED(id);
                if (du_buf == NULL)
                    continue;

                uint8_t *du_iq_ptr = rte_pktmbuf_mtod_offset(du_buf, uint8_t *, IQ_OFFSET);

                /* Never read past the shorter of the two payloads. */
                uint32_t du_iq_size = rte_pktmbuf_pkt_len(du_buf) - IQ_OFFSET;
                uint32_t iq_data_size = RTE_MIN(du_iq_size, new_iq_size);

                uint32_t simd_loop_count = iq_data_size / 64;
                uint32_t remaining_bytes = iq_data_size % 64;
                for (uint32_t simd_idx = 0; simd_idx < simd_loop_count; simd_idx++)
                {
                    __m512i du_data = _mm512_loadu_si512((__m512i_u *)(du_iq_ptr + simd_idx * 64));
                    __m512i new_data = _mm512_loadu_si512((__m512i_u *)(new_iq_ptr + simd_idx * 64));
                    __m512i merged_data = _mm512_or_si512(du_data, new_data);
                    _mm512_storeu_si512((__m512i_u *)(new_iq_ptr + simd_idx * 64), merged_data);
                }
                for (uint32_t byte_idx = iq_data_size - remaining_bytes; byte_idx < iq_data_size; byte_idx++)
                    new_iq_ptr[byte_idx] |= du_iq_ptr[byte_idx];
            }
        }
        /* For C-plane the DUs are expected to issue identical sections, so the
         * copy of the first one is forwarded unchanged. */

        tx_bufs[(*tx_idx)++] = new_buf;
        config->stats.dl_to_ru++;
    }

    cache->num[ecpri_message_type][ru_port_id][symbol][subframe][slot] = 0;
    for (int id = 0; id < MAX_NUM_DUS; id++)
    {
        if (CACHED(id) != NULL)
        {
            rte_pktmbuf_free(CACHED(id));
            CACHED(id) = NULL;
        }
    }
#undef CACHED
}

#endif /* THOR_FHAUL_PROXY_H_ */
