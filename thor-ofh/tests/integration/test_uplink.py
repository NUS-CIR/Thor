#!/usr/bin/env python3

# Copyright (c) National University of Singapore.
# Licensed under the MIT License.

"""
Uplink: RU -> middlebox -> every active L1.

Uplink traffic is not merged; it is replicated to each active DU with the
Ethernet addresses rewritten. PRACH and PUSCH follow the same path, differing
only in the eCPRI RU port ID.
"""

import packets as pk
import thor_testenv as env
from thor_testenv import DU0_MAC, DU1_MAC, NUM_PRBS, RU_MAC


def test_pusch_replicated_to_every_l1(proxy, capture, middlebox_mac):
    with capture() as cap:
        env.send(pk.uplane_ul(RU_MAC, middlebox_mac, num_prbs=NUM_PRBS, pattern=0x5C))
        got = cap.wait_for(2)

    assert len(got) == 2, f"expected one copy per L1, got {len(got)}"
    assert {p.dst.lower() for p in got} == {DU0_MAC.lower(), DU1_MAC.lower()}
    for pkt in got:
        assert pkt.src.lower() == middlebox_mac.lower()


def test_prach_replicated_to_every_l1(proxy, capture, middlebox_mac):
    """PRACH rides an ru_port_id >= 4 and is broadcast to all L1s as-is."""
    with capture() as cap:
        env.send(pk.prach_ul(RU_MAC, middlebox_mac, num_prbs=12, pattern=0x77))
        got = cap.wait_for(2)

    assert len(got) == 2
    assert {p.dst.lower() for p in got} == {DU0_MAC.lower(), DU1_MAC.lower()}
    for pkt in got:
        assert pk.iq_bytes(pkt) == bytes([0x77]) * (12 * pk.PRB_9_SIZE)


def test_uplink_payload_is_unmodified(proxy, capture, middlebox_mac):
    sent = pk.uplane_ul(RU_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                        pattern=0x9E, subframe=3, slot=1, symbol=11)

    with capture() as cap:
        env.send(sent)
        got = cap.wait_for(2)

    assert len(got) == 2
    for pkt in got:
        assert pk.header_bytes(pkt) == pk.header_bytes(sent)
        assert pk.iq_bytes(pkt) == pk.iq_bytes(sent)


def test_uplink_copies_are_independent(proxy, capture, middlebox_mac):
    """Each L1 gets its own copy: rewriting one must not disturb the other."""
    with capture() as cap:
        env.send(pk.uplane_ul(RU_MAC, middlebox_mac, num_prbs=NUM_PRBS, pattern=0x42))
        got = cap.wait_for(2)

    assert len(got) == 2
    by_dst = {p.dst.lower(): p for p in got}
    assert pk.iq_bytes(by_dst[DU0_MAC.lower()]) == pk.iq_bytes(by_dst[DU1_MAC.lower()])


def test_single_l1_gets_one_copy(proxy_factory, capture, middlebox_mac):
    proxy_factory(dus=[(DU0_MAC, 10)])

    with capture() as cap:
        env.send(pk.uplane_ul(RU_MAC, middlebox_mac, num_prbs=NUM_PRBS))
        got = cap.wait_for(1)

    assert len(got) == 1
    assert got[0].dst.lower() == DU0_MAC.lower()


def test_sustained_uplink_stream(proxy, capture, middlebox_mac, ctrl):
    count = 20
    batch = [pk.uplane_ul(RU_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                          symbol=i % 14, seq_id=i % 256)
             for i in range(count)]

    with capture() as cap:
        env.send(batch)
        got = cap.wait_for(count * 2, timeout=5.0)

    assert len(got) == count * 2, f"expected {count * 2} copies, got {len(got)}"

    stats = ctrl.stats()
    assert stats["ul_from_ru"] == count
    assert stats["du0_ul"] == count
    assert stats["du1_ul"] == count
    assert stats["alloc_failed"] == 0
    assert stats["tx_failed"] == 0
