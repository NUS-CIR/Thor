#!/usr/bin/env python3

# Copyright (c) National University of Singapore.
# Licensed under the MIT License.

"""
O-RAN fronthaul frames for the integration tests.

Built on oran_packets.py, which already models the exact headers
thor_fhaul_proxy parses. The layout the proxy assumes is

    Ethernet(14) | eCPRI(8) | radio app common(4) | data section(4) | IQ

so IQ data starts at byte 30 (the proxy's IQ_OFFSET) and no compression header
is present -- hence ud_comp_hdr=False throughout.
"""

from __future__ import annotations

from typing import Iterable, Optional

from scapy.all import Ether, Packet, raw

from oran_packets import (
    ECPRI_IQ_DATA,
    ECPRI_RT_CONTROL_DATA,
    craft_cplane_section1_packet,
    craft_uplane_dl_packet,
    craft_uplane_ul_packet,
)

IQ_OFFSET = 30
PRB_9_SIZE = 28

# ru_port_id values at or above this mean PRACH rather than PUSCH; it mirrors
# MAX_PDSCH_PUSCH_PORT in src/lib/ranbooster_common.h.
MAX_PDSCH_PUSCH_PORT = 4


def prb_payload(num_prbs: int, owned: Optional[Iterable[int]] = None,
                pattern: int = 0xA5) -> bytes:
    """
    IQ payload in which only `owned` PRBs carry data.

    This is what a single DU emits: it fills the PRBs it was scheduled and
    leaves the rest zeroed, so that OR-ing the DUs' payloads together
    reconstructs the full resource grid.
    """
    buf = bytearray(num_prbs * PRB_9_SIZE)
    for prb in owned or ():
        if prb >= num_prbs:
            raise ValueError(f"PRB {prb} outside a {num_prbs}-PRB grid")
        buf[prb * PRB_9_SIZE:(prb + 1) * PRB_9_SIZE] = bytes([pattern]) * PRB_9_SIZE
    return bytes(buf)


def iq_bytes(pkt: Packet) -> bytes:
    """The IQ payload of a captured frame, VLAN tag (if any) excluded."""
    data = raw(pkt)
    if data[12:14] == b"\x81\x00":  # 802.1Q tag re-inserted by the capture
        data = data[:12] + data[16:]
    return data[IQ_OFFSET:]


def header_bytes(pkt: Packet) -> bytes:
    """Everything between the Ethernet header and the IQ payload."""
    data = raw(pkt)
    if data[12:14] == b"\x81\x00":
        data = data[:12] + data[16:]
    return data[14:IQ_OFFSET]


def prb_at(pkt: Packet, prb: int) -> bytes:
    iq = iq_bytes(pkt)
    return iq[prb * PRB_9_SIZE:(prb + 1) * PRB_9_SIZE]


def uplane_dl(src_mac: str, dst_mac: str, *, num_prbs: int,
              owned: Optional[Iterable[int]] = None, pattern: int = 0xA5,
              subframe: int = 1, slot: int = 0, symbol: int = 3,
              ru_port_id: int = 0, vlan: Optional[int] = None,
              seq_id: int = 0) -> Packet:
    """A DU's downlink U-plane packet carrying its share of the PRB grid."""
    return craft_uplane_dl_packet(
        src_mac=src_mac,
        dst_mac=dst_mac,
        du_port_id=0,
        band_sector_id=0,
        cc_id=0,
        ru_port_id=ru_port_id,
        frame_id=0,
        subframe_id=subframe,
        slot_id=slot,
        symbol_id=symbol,
        section_id=0,
        start_prb=0,
        num_prbs=num_prbs,
        prb_data=prb_payload(num_prbs, owned, pattern),
        vlan_id=vlan,
        seq_id=seq_id,
        ud_comp_hdr=False,
    )


def uplane_ul(src_mac: str, dst_mac: str, *, num_prbs: int,
              pattern: int = 0x5C, subframe: int = 1, slot: int = 0,
              symbol: int = 3, ru_port_id: int = 0,
              vlan: Optional[int] = None, seq_id: int = 0) -> Packet:
    """The RU's uplink U-plane packet (PUSCH when ru_port_id < 4)."""
    return craft_uplane_ul_packet(
        src_mac=src_mac,
        dst_mac=dst_mac,
        du_port_id=0,
        band_sector_id=0,
        cc_id=0,
        ru_port_id=ru_port_id,
        frame_id=0,
        subframe_id=subframe,
        slot_id=slot,
        symbol_id=symbol,
        section_id=0,
        start_prb=0,
        num_prbs=num_prbs,
        prb_data=bytes([pattern]) * (num_prbs * PRB_9_SIZE),
        vlan_id=vlan,
        seq_id=seq_id,
        ud_comp_hdr=False,
    )


def prach_ul(src_mac: str, dst_mac: str, *, num_prbs: int = 12,
             pattern: int = 0x77, subframe: int = 1, slot: int = 0,
             symbol: int = 3, vlan: Optional[int] = None) -> Packet:
    """A PRACH uplink packet: same shape, but on an ru_port_id >= 4."""
    return uplane_ul(src_mac, dst_mac, num_prbs=num_prbs, pattern=pattern,
                     subframe=subframe, slot=slot, symbol=symbol,
                     ru_port_id=MAX_PDSCH_PUSCH_PORT, vlan=vlan)


def cplane(src_mac: str, dst_mac: str, *, subframe: int = 1, slot: int = 0,
           symbol: int = 3, ru_port_id: int = 0, num_prbc: int = 32,
           section_id: int = 0, vlan: Optional[int] = None,
           direction: int = 1) -> Packet:
    """A DU's C-plane section type 1 packet."""
    return craft_cplane_section1_packet(
        src_mac=src_mac,
        dst_mac=dst_mac,
        du_port_id=0,
        band_sector_id=0,
        cc_id=0,
        ru_port_id=ru_port_id,
        frame_id=0,
        subframe_id=subframe,
        slot_id=slot,
        symbol_id=symbol,
        direction=direction,
        section_id=section_id,
        start_prbc=0,
        num_prbc=num_prbc,
        vlan_id=vlan,
    )


def with_raw_header(src_mac: str, dst_mac: str, *, msg_type: int,
                    ru_port_id: int, subframe: int, slot: int, symbol: int,
                    payload: bytes = b"", vlan: Optional[int] = None) -> Packet:
    """
    A frame assembled from raw header bytes.

    Used for cases the crafting helpers deliberately cannot express, such as
    header fields that overflow the proxy's cache dimensions.
    """
    from scapy.all import Dot1Q, Raw

    ecpri = bytes([0x10, msg_type]) + (len(payload) + 8).to_bytes(2, "big")
    ecpri += (ru_port_id & 0xFFFF).to_bytes(2, "big") + b"\x00\x00"
    radio = bytes([0x90, 0x00]) + (((subframe & 0xF) << 12) |
                                   ((slot & 0x3F) << 6) |
                                   (symbol & 0x3F)).to_bytes(2, "big")
    data_section = b"\x00\x00\x00" + bytes([len(payload) // PRB_9_SIZE & 0xFF])

    body = Raw(ecpri + radio + data_section + payload)
    if vlan is not None:
        return Ether(src=src_mac, dst=dst_mac, type=0x8100) / Dot1Q(vlan=vlan, type=0xAEFE) / body
    return Ether(src=src_mac, dst=dst_mac, type=0xAEFE) / body
