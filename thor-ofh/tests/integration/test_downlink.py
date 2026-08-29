#!/usr/bin/env python3

# Copyright (c) National University of Singapore.
# Licensed under the MIT License.

"""
Downlink: DUs -> middlebox -> RU.

The proxy holds each DU's packet for a given (eCPRI type, RU port, symbol,
subframe, slot) until every active L1 has contributed, then sends the RU a
single merged packet.
"""

import packets as pk
import thor_testenv as env
from thor_testenv import DU0_MAC, DU1_MAC, NUM_PRBS, RU_MAC, UNKNOWN_MAC


def test_cplane_forwarded_once_both_dus_contributed(proxy, capture, middlebox_mac):
    with capture() as cap:
        env.send([
            pk.cplane(DU0_MAC, middlebox_mac),
            pk.cplane(DU1_MAC, middlebox_mac),
        ])
        got = cap.wait_for(1)

    assert len(got) == 1, f"expected one merged C-plane packet, got {len(got)}"
    assert got[0].dst.lower() == RU_MAC.lower()
    assert got[0].src.lower() == middlebox_mac.lower()


def test_cplane_held_until_every_du_contributed(proxy, capture, middlebox_mac):
    """One L1 alone must not reach the RU: the other's contribution is missing."""
    with capture() as cap:
        env.send(pk.cplane(DU0_MAC, middlebox_mac))
        got = cap.wait_for(1, timeout=1.0)

    assert got == [], f"packet reached the RU with only one L1 reporting: {got}"


def test_uplane_iq_merge_combines_disjoint_prbs(proxy, capture, middlebox_mac):
    """DU0 owns PRBs 0-3, DU1 owns 8-11; the RU must receive both sets."""
    with capture() as cap:
        env.send([
            pk.uplane_dl(DU0_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                         owned=range(0, 4), pattern=0xA5),
            pk.uplane_dl(DU1_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                         owned=range(8, 12), pattern=0x5A),
        ])
        got = cap.wait_for(1)

    assert len(got) == 1
    merged = got[0]
    assert merged.dst.lower() == RU_MAC.lower()

    for prb in range(0, 4):
        assert pk.prb_at(merged, prb) == bytes([0xA5]) * pk.PRB_9_SIZE, \
            f"DU0's PRB {prb} missing from the merged packet"
    for prb in range(8, 12):
        assert pk.prb_at(merged, prb) == bytes([0x5A]) * pk.PRB_9_SIZE, \
            f"DU1's PRB {prb} missing from the merged packet"
    for prb in (4, 5, 6, 7, 12):
        assert pk.prb_at(merged, prb) == bytes(pk.PRB_9_SIZE), \
            f"unscheduled PRB {prb} is not empty"


def test_uplane_merge_is_bitwise_or(proxy, capture, middlebox_mac):
    """Where two L1s write the same PRB, the merge ORs the bits together."""
    with capture() as cap:
        env.send([
            pk.uplane_dl(DU0_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                         owned=[3], pattern=0xF0),
            pk.uplane_dl(DU1_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                         owned=[3], pattern=0x0F),
        ])
        got = cap.wait_for(1)

    assert len(got) == 1
    assert pk.prb_at(got[0], 3) == bytes([0xFF]) * pk.PRB_9_SIZE


def test_uplane_merge_full_grid(proxy, capture, middlebox_mac):
    """Every PRB scheduled, split between the two L1s, nothing left empty."""
    even = [p for p in range(NUM_PRBS) if p % 2 == 0]
    odd = [p for p in range(NUM_PRBS) if p % 2 == 1]

    with capture() as cap:
        env.send([
            pk.uplane_dl(DU0_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                         owned=even, pattern=0x11),
            pk.uplane_dl(DU1_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                         owned=odd, pattern=0x22),
        ])
        got = cap.wait_for(1)

    assert len(got) == 1
    for prb in range(NUM_PRBS):
        expected = 0x11 if prb % 2 == 0 else 0x22
        assert pk.prb_at(got[0], prb) == bytes([expected]) * pk.PRB_9_SIZE, \
            f"PRB {prb} wrong after merge"


def test_downlink_preserves_oran_headers(proxy, capture, middlebox_mac):
    """Only the Ethernet addresses are rewritten; the O-RAN headers pass through."""
    sent = pk.uplane_dl(DU0_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                        owned=[0], pattern=0xC3, subframe=2, slot=1, symbol=9)

    with capture() as cap:
        env.send([
            sent,
            pk.uplane_dl(DU1_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                         owned=[1], pattern=0x3C, subframe=2, slot=1, symbol=9),
        ])
        got = cap.wait_for(1)

    assert len(got) == 1
    assert pk.header_bytes(got[0]) == pk.header_bytes(sent), \
        "eCPRI / radio app / data section headers were modified in flight"


def test_distinct_slots_tracked_independently(proxy, capture, middlebox_mac):
    """Two symbols in flight at once must not complete each other."""
    with capture() as cap:
        env.send([
            pk.cplane(DU0_MAC, middlebox_mac, symbol=3),
            pk.cplane(DU1_MAC, middlebox_mac, symbol=4),
        ])
        got = cap.wait_for(1, timeout=1.0)
    assert got == [], "packets for different symbols were merged together"

    with capture() as cap:
        env.send([
            pk.cplane(DU1_MAC, middlebox_mac, symbol=3),
            pk.cplane(DU0_MAC, middlebox_mac, symbol=4),
        ])
        got = cap.wait_for(2)
    assert len(got) == 2, "both symbols should complete once the other L1 reports"


def test_cplane_and_uplane_do_not_share_a_cache_entry(proxy, capture, middlebox_mac):
    """Same slot, different eCPRI type: two independent merges, two packets out."""
    with capture() as cap:
        env.send([
            pk.cplane(DU0_MAC, middlebox_mac, symbol=5),
            pk.uplane_dl(DU0_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                         owned=[0], symbol=5),
            pk.cplane(DU1_MAC, middlebox_mac, symbol=5),
            pk.uplane_dl(DU1_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                         owned=[1], symbol=5),
        ])
        got = cap.wait_for(2)

    assert len(got) == 2, f"expected a C-plane and a U-plane packet, got {len(got)}"


def test_sustained_downlink_stream(proxy, capture, middlebox_mac, ctrl):
    """A run across every symbol produces exactly one merged packet per symbol."""
    symbols = list(range(14))
    batch = []
    for sym in symbols:
        batch.append(pk.uplane_dl(DU0_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                                  owned=[0], pattern=0x0F, symbol=sym))
        batch.append(pk.uplane_dl(DU1_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                                  owned=[0], pattern=0xF0, symbol=sym))

    with capture() as cap:
        env.send(batch)
        got = cap.wait_for(len(symbols), timeout=5.0)

    assert len(got) == len(symbols), \
        f"expected {len(symbols)} merged packets, got {len(got)}"
    for pkt in got:
        assert pk.prb_at(pkt, 0) == bytes([0xFF]) * pk.PRB_9_SIZE

    stats = ctrl.stats()
    assert stats["dl_to_ru"] == len(symbols)
    assert stats["du0_dl_up"] == len(symbols)
    assert stats["du1_dl_up"] == len(symbols)
    assert stats["alloc_failed"] == 0
    assert stats["tx_failed"] == 0
