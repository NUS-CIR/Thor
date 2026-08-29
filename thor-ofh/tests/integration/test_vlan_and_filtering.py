#!/usr/bin/env python3

# Copyright (c) National University of Singapore.
# Licensed under the MIT License.

"""
VLAN retagging and ingress filtering.

The proxy relies on the NIC to strip the ingress VLAN tag and to insert the
egress one, so these tests only pass on a PMD that supports both offloads --
af_packet does. Traffic that matches neither the RU nor an active L1, or that
is not addressed to the middlebox, must be dropped.
"""

import packets as pk
import thor_testenv as env
from thor_testenv import (
    DU0_MAC,
    DU0_VLAN,
    DU1_MAC,
    DU1_VLAN,
    NUM_PRBS,
    RU_MAC,
    RU_VLAN,
    UNKNOWN_MAC,
)


# ---------------------------------------------------------------------------
# VLAN retagging
# ---------------------------------------------------------------------------


def test_downlink_retagged_to_the_ru_vlan(proxy, capture, middlebox_mac):
    with capture() as cap:
        env.send([
            pk.cplane(DU0_MAC, middlebox_mac, vlan=DU0_VLAN),
            pk.cplane(DU1_MAC, middlebox_mac, vlan=DU1_VLAN),
        ])
        got = cap.wait_for(1)

    assert len(got) == 1
    assert got[0].haslayer("Dot1Q"), "the merged packet lost its VLAN tag"
    assert got[0]["Dot1Q"].vlan == RU_VLAN


def test_uplink_retagged_per_l1(proxy, capture, middlebox_mac):
    """Each copy carries the VLAN of the L1 it is addressed to."""
    with capture() as cap:
        env.send(pk.uplane_ul(RU_MAC, middlebox_mac, num_prbs=NUM_PRBS, vlan=RU_VLAN))
        got = cap.wait_for(2)

    assert len(got) == 2
    vlans = {p.dst.lower(): p["Dot1Q"].vlan for p in got}
    assert vlans[DU0_MAC.lower()] == DU0_VLAN
    assert vlans[DU1_MAC.lower()] == DU1_VLAN


def test_untagged_traffic_stays_untagged(proxy, capture, middlebox_mac):
    """Retagging is conditional on the ingress packet having been tagged."""
    with capture() as cap:
        env.send(pk.uplane_ul(RU_MAC, middlebox_mac, num_prbs=NUM_PRBS, vlan=None))
        got = cap.wait_for(2)

    assert len(got) == 2
    for pkt in got:
        assert not pkt.haslayer("Dot1Q"), "an untagged packet acquired a VLAN tag"


def test_vlan_does_not_disturb_the_iq_merge(proxy, capture, middlebox_mac):
    """Header offsets must be measured after the tag is stripped, not before."""
    with capture() as cap:
        env.send([
            pk.uplane_dl(DU0_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                         owned=[2], pattern=0xF0, vlan=DU0_VLAN),
            pk.uplane_dl(DU1_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                         owned=[2], pattern=0x0F, vlan=DU1_VLAN),
        ])
        got = cap.wait_for(1)

    assert len(got) == 1
    assert got[0]["Dot1Q"].vlan == RU_VLAN
    assert pk.prb_at(got[0], 2) == bytes([0xFF]) * pk.PRB_9_SIZE


# ---------------------------------------------------------------------------
# ingress filtering
# ---------------------------------------------------------------------------


def test_unknown_source_is_dropped(proxy, ctrl, capture, middlebox_mac):
    before = ctrl.stats()["dropped_unmatched"]

    with capture() as cap:
        env.send(pk.uplane_ul(UNKNOWN_MAC, middlebox_mac, num_prbs=NUM_PRBS))
        got = cap.wait_for(1, timeout=1.0)

    assert got == [], "traffic from an unknown source was forwarded"
    assert ctrl.stats()["dropped_unmatched"] == before + 1


def test_traffic_not_addressed_to_the_middlebox_is_dropped(proxy, ctrl, capture,
                                                           middlebox_mac):
    before = ctrl.stats()["dropped_unmatched"]

    with capture() as cap:
        env.send([
            pk.cplane(DU0_MAC, UNKNOWN_MAC),                       # DU -> elsewhere
            pk.uplane_ul(RU_MAC, UNKNOWN_MAC, num_prbs=NUM_PRBS),  # RU -> elsewhere
        ])
        got = cap.wait_for(1, timeout=1.0)

    assert got == []
    assert ctrl.stats()["dropped_unmatched"] == before + 2


def test_out_of_range_header_fields_are_dropped(proxy, ctrl, capture, middlebox_mac):
    """
    slot_id and symb_id are 6 bits on the wire but index arrays of 2 and 14;
    subframe_id is 4 bits against 10, and ru_port_id 4 bits against 8. Values
    past those bounds must be rejected rather than indexed.
    """
    before = ctrl.stats()["dropped_malformed"]

    payload = bytes(4 * pk.PRB_9_SIZE)
    bad = [
        pk.with_raw_header(DU0_MAC, middlebox_mac, msg_type=0, ru_port_id=0,
                           subframe=1, slot=2, symbol=3, payload=payload),
        pk.with_raw_header(DU0_MAC, middlebox_mac, msg_type=0, ru_port_id=0,
                           subframe=1, slot=0, symbol=14, payload=payload),
        pk.with_raw_header(DU0_MAC, middlebox_mac, msg_type=0, ru_port_id=0,
                           subframe=10, slot=0, symbol=3, payload=payload),
        pk.with_raw_header(DU0_MAC, middlebox_mac, msg_type=0, ru_port_id=8,
                           subframe=1, slot=0, symbol=3, payload=payload),
        # eCPRI one-way delay measurement: a type the merge cache has no slot for
        pk.with_raw_header(DU0_MAC, middlebox_mac, msg_type=5, ru_port_id=0,
                           subframe=1, slot=0, symbol=3, payload=payload),
    ]

    with capture() as cap:
        env.send(bad)
        got = cap.wait_for(1, timeout=1.0)

    assert got == [], "a malformed frame was forwarded"
    assert ctrl.stats()["dropped_malformed"] == before + len(bad)


def test_proxy_survives_malformed_traffic(proxy, ctrl, capture, middlebox_mac):
    """After a burst of junk, ordinary traffic must still flow."""
    payload = bytes(4 * pk.PRB_9_SIZE)
    env.send([
        pk.with_raw_header(DU0_MAC, middlebox_mac, msg_type=0, ru_port_id=15,
                           subframe=15, slot=63, symbol=63, payload=payload)
        for _ in range(20)
    ])

    with capture() as cap:
        env.send([
            pk.cplane(DU0_MAC, middlebox_mac, symbol=2),
            pk.cplane(DU1_MAC, middlebox_mac, symbol=2),
        ])
        got = cap.wait_for(1)

    assert len(got) == 1, "the datapath stopped forwarding after malformed input"
    assert ctrl.command("ping").ok, "the control channel stopped responding"
