#!/usr/bin/env python3

# Copyright (c) National University of Singapore.
# Licensed under the MIT License.

"""
Adding and removing L1s while the datapath is running.

Commands arrive on the control socket, are handed to the datapath lcore over a
lock-free ring, and are applied between RX bursts. Applying one flushes the
merge cache, because cache entries complete on a fill count compared against the
number of active L1s -- a count that has just changed.
"""

import pytest

import packets as pk
import thor_testenv as env
from thor_testenv import (
    DU0_MAC,
    DU0_VLAN,
    DU1_MAC,
    DU1_VLAN,
    DU2_MAC,
    DU2_VLAN,
    NUM_PRBS,
    RU_MAC,
    UNKNOWN_MAC,
)


# ---------------------------------------------------------------------------
# control protocol
# ---------------------------------------------------------------------------


def test_list_reports_the_startup_l1s(ctrl):
    dus = ctrl.active_dus()
    assert len(dus) == 2
    assert {d["mac"].lower() for d in dus} == {DU0_MAC.lower(), DU1_MAC.lower()}
    assert {d["vlan"] for d in dus} == {str(DU0_VLAN), str(DU1_VLAN)}


def test_ping(ctrl):
    reply = ctrl.command("ping")
    assert reply.ok and reply.status == "pong"


def test_add_rejected_when_table_full(ctrl):
    reply = ctrl.add(DU2_MAC, DU2_VLAN)
    assert not reply.ok
    assert "no free du slot" in reply.status
    assert len(ctrl.active_dus()) == 2


def test_duplicate_add_rejected(proxy_factory, ctrl_sock_path):
    proxy_factory(dus=[(DU0_MAC, DU0_VLAN)])
    with env.CtrlClient(ctrl_sock_path) as ctrl:
        reply = ctrl.add(DU0_MAC, DU0_VLAN)
        assert not reply.ok
        assert "already registered" in reply.status
        assert len(ctrl.active_dus()) == 1


def test_remove_unknown_l1_rejected(ctrl):
    reply = ctrl.delete(UNKNOWN_MAC)
    assert not reply.ok
    assert "no such du" in reply.status
    assert len(ctrl.active_dus()) == 2


def test_malformed_commands_rejected(ctrl):
    for bad in ("add", "add zz:zz 5", "add 02:00:00:00:00:20", "del", "wibble",
                "add 02:00:00:00:00:20 9999"):
        reply = ctrl.command(bad)
        assert not reply.ok, f"{bad!r} should have been rejected"
    # The connection survives every rejection.
    assert ctrl.command("ping").ok


def test_slot_is_reused_after_removal(proxy_factory, ctrl_sock_path):
    proxy_factory(dus=[(DU0_MAC, DU0_VLAN), (DU1_MAC, DU1_VLAN)])
    with env.CtrlClient(ctrl_sock_path) as ctrl:
        assert ctrl.delete(DU0_MAC).fields["slot"] == "0"
        assert ctrl.add(DU2_MAC, DU2_VLAN).fields["slot"] == "0"

        dus = {d["mac"].lower(): d for d in ctrl.active_dus()}
        assert set(dus) == {DU1_MAC.lower(), DU2_MAC.lower()}


def test_several_clients_can_connect(proxy, ctrl_sock_path):
    clients = [env.CtrlClient(ctrl_sock_path).connect() for _ in range(4)]
    try:
        for c in clients:
            assert c.command("ping").ok
    finally:
        for c in clients:
            c.close()


# ---------------------------------------------------------------------------
# effect on the datapath
# ---------------------------------------------------------------------------


def test_added_l1_starts_receiving_uplink(proxy_factory, ctrl_sock_path,
                                          capture, middlebox_mac):
    """Start with one L1, add a second, and watch the fan-out widen."""
    proxy_factory(dus=[(DU0_MAC, DU0_VLAN)])

    with capture() as cap:
        env.send(pk.uplane_ul(RU_MAC, middlebox_mac, num_prbs=NUM_PRBS))
        got = cap.wait_for(1)
    assert len(got) == 1 and got[0].dst.lower() == DU0_MAC.lower()

    with env.CtrlClient(ctrl_sock_path) as ctrl:
        reply = ctrl.add(DU1_MAC, DU1_VLAN)
        assert reply.ok, str(reply)
        assert reply.fields["active_dus"] == "2"

    with capture() as cap:
        env.send(pk.uplane_ul(RU_MAC, middlebox_mac, num_prbs=NUM_PRBS))
        got = cap.wait_for(2)

    assert len(got) == 2, "the newly added L1 is not receiving uplink traffic"
    assert {p.dst.lower() for p in got} == {DU0_MAC.lower(), DU1_MAC.lower()}


def test_removed_l1_stops_receiving_uplink(proxy, ctrl, capture, middlebox_mac):
    assert ctrl.delete(DU1_MAC).ok

    with capture() as cap:
        env.send(pk.uplane_ul(RU_MAC, middlebox_mac, num_prbs=NUM_PRBS))
        got = cap.wait_for(2, timeout=1.0)

    assert len(got) == 1, f"removed L1 still receiving traffic: {[p.dst for p in got]}"
    assert got[0].dst.lower() == DU0_MAC.lower()


def test_removed_l1_downlink_is_dropped(proxy, ctrl, capture, middlebox_mac):
    """Once removed, an L1's downlink is indistinguishable from a stranger's."""
    assert ctrl.delete(DU1_MAC).ok
    before = ctrl.stats()["dropped_unmatched"]

    with capture() as cap:
        env.send(pk.cplane(DU1_MAC, middlebox_mac))
        got = cap.wait_for(1, timeout=1.0)

    assert got == []
    assert ctrl.stats()["dropped_unmatched"] == before + 1


def test_added_l1_participates_in_the_merge(proxy_factory, ctrl_sock_path,
                                            capture, middlebox_mac):
    """After an L1 joins, the RU must wait for both before anything is sent."""
    proxy_factory(dus=[(DU0_MAC, DU0_VLAN)])

    # With one L1 the downlink goes straight through.
    with capture() as cap:
        env.send(pk.uplane_dl(DU0_MAC, middlebox_mac, num_prbs=NUM_PRBS, owned=[0]))
        got = cap.wait_for(1)
    assert len(got) == 1

    with env.CtrlClient(ctrl_sock_path) as ctrl:
        assert ctrl.add(DU1_MAC, DU1_VLAN).ok

    # Now DU0 alone is not enough ...
    with capture() as cap:
        env.send(pk.uplane_dl(DU0_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                              owned=[0], pattern=0x0F, symbol=6))
        got = cap.wait_for(1, timeout=1.0)
    assert got == [], "downlink forwarded without the newly added L1's contribution"

    # ... and once DU1 reports, both contributions come out merged.
    with capture() as cap:
        env.send(pk.uplane_dl(DU1_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                              owned=[0], pattern=0xF0, symbol=6))
        got = cap.wait_for(1)
    assert len(got) == 1
    assert pk.prb_at(got[0], 0) == bytes([0xFF]) * pk.PRB_9_SIZE


def test_removal_unblocks_a_pending_merge(proxy, ctrl, capture, middlebox_mac):
    """
    An L1 that goes away must not wedge the slot it was holding up: after the
    removal, the surviving L1's traffic flows on its own.
    """
    with capture() as cap:
        env.send(pk.uplane_dl(DU0_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                              owned=[0], symbol=7))
        got = cap.wait_for(1, timeout=1.0)
    assert got == [], "should still be waiting for DU1"

    assert ctrl.delete(DU1_MAC).ok

    with capture() as cap:
        env.send(pk.uplane_dl(DU0_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                              owned=[0], pattern=0x5A, symbol=7))
        got = cap.wait_for(1)

    assert len(got) == 1, "the slot stayed wedged after the other L1 was removed"
    assert pk.prb_at(got[0], 0) == bytes([0x5A]) * pk.PRB_9_SIZE


def test_membership_change_flushes_stale_contributions(proxy, ctrl, capture,
                                                       middlebox_mac):
    """
    A contribution parked under the old membership must be discarded, not
    completed later against a set it was never recorded under.
    """
    env.send(pk.uplane_dl(DU0_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                          owned=[0], pattern=0xAA, symbol=8))

    flushes_before = ctrl.stats()["cache_flushes"]
    assert ctrl.delete(DU1_MAC).ok
    assert ctrl.add(DU1_MAC, DU1_VLAN).ok
    assert ctrl.stats()["cache_flushes"] == flushes_before + 2

    # DU1 alone must not complete the merge using DU0's flushed packet.
    with capture() as cap:
        env.send(pk.uplane_dl(DU1_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                              owned=[1], pattern=0xBB, symbol=8))
        got = cap.wait_for(1, timeout=1.0)
    assert got == [], "a flushed contribution was still counted towards the merge"

    # DU0 resends and the merge completes with only the fresh contributions.
    with capture() as cap:
        env.send(pk.uplane_dl(DU0_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                              owned=[0], pattern=0xCC, symbol=8))
        got = cap.wait_for(1)
    assert len(got) == 1
    assert pk.prb_at(got[0], 0) == bytes([0xCC]) * pk.PRB_9_SIZE
    assert pk.prb_at(got[0], 1) == bytes([0xBB]) * pk.PRB_9_SIZE


def test_vlan_follows_the_l1_it_was_added_with(proxy_factory, ctrl_sock_path,
                                               capture, middlebox_mac):
    """An L1 added at runtime is tagged with the VLAN given in its add command."""
    proxy_factory(dus=[(DU0_MAC, DU0_VLAN)])

    with env.CtrlClient(ctrl_sock_path) as ctrl:
        assert ctrl.add(DU1_MAC, 42).ok

    with capture() as cap:
        env.send(pk.uplane_ul(RU_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                              vlan=env.RU_VLAN))
        got = cap.wait_for(2)

    assert len(got) == 2
    vlans = {p.dst.lower(): p["Dot1Q"].vlan for p in got}
    assert vlans[DU0_MAC.lower()] == DU0_VLAN
    assert vlans[DU1_MAC.lower()] == 42


@pytest.mark.slow
def test_control_channel_works_under_load(proxy, ctrl, capture, middlebox_mac):
    """Reconfiguring while traffic flows must not wedge the datapath."""
    for round_idx in range(5):
        env.send([pk.uplane_ul(RU_MAC, middlebox_mac, num_prbs=NUM_PRBS,
                               symbol=i % 14) for i in range(10)])

        assert ctrl.delete(DU1_MAC).ok, f"round {round_idx}: delete failed"
        assert ctrl.add(DU1_MAC, DU1_VLAN).ok, f"round {round_idx}: add failed"

    # The datapath is still healthy afterwards.
    with capture() as cap:
        env.send(pk.uplane_ul(RU_MAC, middlebox_mac, num_prbs=NUM_PRBS))
        got = cap.wait_for(2)

    assert len(got) == 2
    stats = ctrl.stats()
    assert stats["alloc_failed"] == 0, "mbufs leaked across membership changes"
    assert stats["tx_failed"] == 0
