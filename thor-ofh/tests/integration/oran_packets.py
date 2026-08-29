#!/usr/bin/env python3

# Copyright (c) National University of Singapore
# Licensed under the MIT License.

"""
O-RAN Fronthaul packet crafting module using scapy.

This module provides scapy packet classes for crafting O-RAN Fronthaul packets
including eCPRI headers, radio application headers, and C-plane/U-plane sections.

Based on O-RAN.WG4.CUS.0-R003-v14.00 specification.
"""

import struct
from typing import List, Optional, Tuple

from scapy.all import Packet, Raw, Ether, Dot1Q
from scapy.fields import (
    BitField,
    ByteField,
    IntField,
    ShortField,
    XByteField,
    XShortField,
    XIntField,
    FieldLenField,
    PacketListField,
    ConditionalField,
    StrFixedLenField,
    StrLenField,
    LEShortField,
)

# Constants from ranbooster_common.h
ECPRI_IQ_DATA = 0x00  # eCPRI Message Type for IQ Data (U-plane)
ECPRI_RT_CONTROL_DATA = 0x02  # eCPRI Message Type for Real-Time Control Data (C-plane)

NUM_PRB = 273
NUM_SUBCARRIERS_PRB = 12
IQ_BIT_WIDTH_COMPRESSED = 9
IQ_BIT_WIDTH_UNCOMPRESSED = 16
COMP_PARAM_HEADER_SIZE = 1

# PRB sizes (in bytes)
PRB_9_SIZE = 1 + ((NUM_SUBCARRIERS_PRB * IQ_BIT_WIDTH_COMPRESSED * 2) // 8)  # 28 bytes
PRB_16_SIZE = NUM_SUBCARRIERS_PRB * 2 * 2  # 48 bytes

# Direction
UPLINK_DIRECTION = 0
DOWNLINK_DIRECTION = 1

# Filter Index
STANDARD_CHANNEL_FILTER = 0
PRACH_FILTER = 3

class eCPRIHeader(Packet):
    """
    eCPRI Common Header.
    
    Based on eCPRI Specification V2.0:
    - Version: 4 bits (should be 1 for eCPRI)
    - Reserved: 3 bits
    - C: 1 bit (concatenation indicator)
    - Message Type: 8 bits
    - Payload Size: 16 bits
    """
    name = "eCPRI Header"
    fields_desc = [
        BitField("version", 1, 4),
        BitField("reserved", 0, 3),
        BitField("concatenation", 0, 1),
        ByteField("message_type", ECPRI_IQ_DATA),
        ShortField("payload_size", 0),
    ]


class eCPRIRtcId(Packet):
    """
    eCPRI Real-time Control ID for O-RAN.
    
    Contains RTC ID and Sequence ID fields:
    - rtc_id: 16 bits (duPortId:4 + bandSectorId:4 + ccId:4 + ruPortId:4)
    - seq_id: 16 bits (seqId:8 + ebit:1 + subSeqId:7)
    """
    name = "eCPRI RTC ID"
    fields_desc = [
        # rtc_id broken down
        BitField("du_port_id", 0, 4),  # Reserved bits
        BitField("band_sector_id", 0, 4),  # Band Sector ID
        BitField("cc_id", 0, 4),  # Component Carrier ID
        BitField("ru_port_id", 0, 4),  # RU Port ID
        # seq_id broken down
        ByteField("seq_id", 0),  # Sequence ID
        BitField("ebit", 1, 1),  # E bit
        BitField("sub_seq_id", 0, 7),  # Sub-sequence ID
    ]


class RadioAppCommonHeader(Packet):
    """
    Radio Application Common Header (for U-plane and C-plane).
    
    8 bytes total:
    - dataDirection: 1 bit
    - payloadVersion: 3 bits
    - filterIndex: 4 bits
    - frameId: 8 bits
    - subframeId: 4 bits
    - slotId: 6 bits
    - startSymbolId: 6 bits
    """
    name = "Radio App Common Header"
    fields_desc = [
        BitField("data_direction", DOWNLINK_DIRECTION, 1),
        BitField("payload_version", 1, 3),
        BitField("filter_index", 0, 4),
        ByteField("frame_id", 0),
        BitField("subframe_id", 0, 4),
        BitField("slot_id", 0, 6),
        BitField("start_symbol_id", 0, 6),
    ]


class CPlaneRadioAppHeader(Packet):
    """
    C-plane Radio Application Header.
    
    4 bytes (follows Radio App Common Header):
    - sectionType: 8 bits
    - timeOffset: 16 bits (only for some section types)
    - frameStructure: 8 bits
    - cpLength: 16 bits (only for some section types)
    - numSections: 8 bits (only present in some formats)
    """
    name = "C-Plane Radio App Header"
    fields_desc = [
        ByteField("section_type", 1),  # 1 for scheduled data, 3 for PRACH
        ByteField("num_sections", 1),
        # For section type 1: udCompHdr
        BitField("ud_iq_width", 0, 4),
        BitField("ud_comp_meth", 0, 4),
        ByteField("reserved", 0),
    ]


class DataSectionHeader(Packet):
    """
    Data Section Header for U-plane.
    
    4 bytes:
    - sectionId: 12 bits
    - rb: 1 bit (resource block indicator)
    - symInc: 1 bit (symbol increment)
    - startPrbu: 10 bits
    - numPrbu: 8 bits (0 means 256)
    """
    name = "Data Section Header"
    fields_desc = [
        BitField("section_id", 0, 12),
        BitField("rb", 0, 1),
        BitField("sym_inc", 0, 1),
        BitField("start_prbu", 0, 10),
        ByteField("num_prbu", 0),  # 0 means 256 PRBs
    ]


class DataCompressionHeader(Packet):
    """
    Data Compression Header.
    
    1 byte:
    - udIqWidth: 4 bits (IQ bit width, 0 means 16)
    - udCompMeth: 4 bits (compression method)
    """
    name = "Data Compression Header"
    fields_desc = [
        BitField("ud_iq_width", IQ_BIT_WIDTH_COMPRESSED, 4),
        BitField("ud_comp_meth", 1, 4),  # 1 = Block floating point
    ]


class CPlaneSectionHeader(Packet):
    """
    C-plane Section Header (common fields).
    
    Used for section type 1 and 3.
    """
    name = "C-Plane Section Header"
    fields_desc = [
        BitField("section_id", 0, 12),
        BitField("rb", 0, 1),
        BitField("sym_inc", 0, 1),
        BitField("start_prbc", 0, 10),
        ByteField("num_prbc", 0),  # 0 means 256
        BitField("re_mask", 0xFFF, 12),
        BitField("num_symbol", 14, 4),
    ]


class CPlaneSection1(Packet):
    """
    C-plane Section Type 1 (for scheduled DL/UL data).
    
    8 bytes total including extension flag and beam_id.
    """
    name = "C-Plane Section 1"
    fields_desc = [
        # Section header (4 bytes) 
        BitField("section_id", 0, 12),
        BitField("rb", 0, 1),
        BitField("sym_inc", 0, 1),
        BitField("start_prbc", 0, 10),
        ByteField("num_prbc", 0),  # 0 means 256
        # Section-specific fields
        BitField("re_mask", 0xFFF, 12),
        BitField("num_symbol", 14, 4),
        BitField("ef", 0, 1),  # Extension flag
        BitField("beam_id", 0, 15),
    ]


class CPlaneSection3(Packet):
    """
    C-plane Section Type 3 (for PRACH).
    
    12 bytes total.
    """
    name = "C-Plane Section 3"
    fields_desc = [
        # Section header (4 bytes)
        BitField("section_id", 0, 12),
        BitField("rb", 0, 1),
        BitField("sym_inc", 0, 1),
        BitField("start_prbc", 0, 10),
        ByteField("num_prbc", 12),  # PRACH uses 12 PRBs
        # Section-specific fields
        BitField("re_mask", 0xFFF, 12),
        BitField("num_symbol", 14, 4),
        BitField("ef", 0, 1),  # Extension flag
        BitField("beam_id", 0, 15),
        # Frequency offset (3 bytes, 24 bits signed)
        BitField("freq_offset", 0, 24),
        ByteField("reserved", 0),
    ]


class CPlaneSection1Header(Packet):
    """
    Complete C-plane Section Type 1 header.
    
    Includes section type, time offset, etc.
    """
    name = "C-Plane Section 1 Header"
    fields_desc = [
        ByteField("num_sections", 1),
        ByteField("section_type", 1),
        BitField("ud_iq_width", 0, 4),
        BitField("ud_comp_meth", 0, 4),
        ByteField("reserved", 0),
    ]


class CPlaneSection3Header(Packet):
    """
    Complete C-plane Section Type 3 header (for PRACH).
    
    Includes time offset and frame structure.
    """
    name = "C-Plane Section 3 Header"
    fields_desc = [
        ByteField("num_sections", 1),
        ByteField("section_type", 3),
        ShortField("time_offset", 0),
        BitField("fft_size", 12, 4),  # FFT size
        BitField("scs", 1, 4),  # Subcarrier spacing
        ShortField("cp_length", 0),
        BitField("ud_iq_width", 0, 4),
        BitField("ud_comp_meth", 0, 4),
    ]


def craft_ethernet_header(
    src_mac: str,
    dst_mac: str,
    vlan_id: Optional[int] = None,
    ether_type: int = 0xAEFE,  # eCPRI EtherType
) -> Packet:
    """
    Craft an Ethernet header with optional VLAN tag.
    
    Args:
        src_mac: Source MAC address
        dst_mac: Destination MAC address
        vlan_id: VLAN ID (optional)
        ether_type: EtherType (default: 0xAEFE for eCPRI)
    
    Returns:
        Scapy Ether packet (with optional Dot1Q)
    """
    if vlan_id is not None:
        return Ether(src=src_mac, dst=dst_mac, type=0x8100) / Dot1Q(vlan=vlan_id, type=ether_type)
    else:
        return Ether(src=src_mac, dst=dst_mac, type=ether_type)


def craft_ecpri_header(
    message_type: int,
    payload_size: int,
    du_port_id: int = 0,
    band_sector_id: int = 0,
    cc_id: int = 0,
    ru_port_id: int = 0,
    seq_id: int = 0,
) -> Packet:
    """
    Craft eCPRI header.
    
    Args:
        message_type: eCPRI message type (0=IQ data, 2=RT control)
        payload_size: Size of payload in bytes
        du_port_id: DU port ID (0-15)
        band_sector_id: Band sector ID (0-15)
        cc_id: Component carrier ID (0-15)
        ru_port_id: RU port ID (0-15)
        seq_id: Sequence ID
    
    Returns:
        eCPRI header as Packet
    """    
    ecpri_header = eCPRIHeader(
        version=1,
        reserved=0,
        concatenation=0,
        message_type=message_type,
        payload_size=payload_size,
    )
    ecpri_rtc = eCPRIRtcId(
        du_port_id=du_port_id,
        band_sector_id=band_sector_id,
        cc_id=cc_id,
        ru_port_id=ru_port_id,
        seq_id=seq_id,
        ebit=0,
        sub_seq_id=0,
    )
    ecpri = ecpri_header / ecpri_rtc
    return ecpri


def craft_radio_app_header(
    direction: int,
    frame_id: int,
    subframe_id: int,
    slot_id: int,
    start_symbol_id: int,
    filter_index: int = 0,
) -> Packet:
    """
    Craft Radio Application Common Header.
    
    Args:
        direction: 0=uplink, 1=downlink
        frame_id: Frame ID (0-255)
        subframe_id: Subframe ID (0-9)
        slot_id: Slot ID (0-63)
        start_symbol_id: Start symbol ID (0-13)
        filter_index: Filter index
    
    Returns:
        Radio app header as Packet
    """
    radio_app_common_hdr = RadioAppCommonHeader(
        data_direction=direction,
        payload_version=1,
        filter_index=filter_index,
        frame_id=frame_id,
        subframe_id=subframe_id,
        slot_id=slot_id,
        start_symbol_id=start_symbol_id,
    )
    return radio_app_common_hdr


def craft_data_section_header(
    section_id: int,
    start_prbu: int,
    num_prbu: int,
    rb: int = 0,
    sym_inc: int = 0,
) -> Packet:
    """
    Craft Data Section Header for U-plane.
    
    Args:
        section_id: Section ID (0-4095)
        start_prbu: Start PRB (0-1023)
        num_prbu: Number of PRBs (0 means 256)
        rb: Resource block indicator
        sym_inc: Symbol increment
    
    Returns:
        Data section header as Packet
    """
    data_section_hdr = DataSectionHeader(
        section_id=section_id,
        rb=rb,
        sym_inc=sym_inc,
        start_prbu=start_prbu,
        num_prbu=num_prbu,
    )
    return data_section_hdr


def craft_compression_header(
    iq_width: int = IQ_BIT_WIDTH_COMPRESSED,
    comp_meth: int = 1,  # Block floating point
) -> Packet:
    """
    Craft Data Compression Header.
    
    Args:
        iq_width: IQ bit width (0 means 16)
        comp_meth: Compression method (1 = block floating point)
    
    Returns:
        Compression header as Packet
    """
    compression_hdr = DataCompressionHeader(
        ud_iq_width=iq_width,
        ud_comp_meth=comp_meth,
    )
    return compression_hdr


def craft_prb_data(
    num_prbs: int,
    compressed: bool = True,
    data: Optional[bytes] = None,
) -> Packet:
    """
    Craft PRB IQ data.
    
    Args:
        num_prbs: Number of PRBs
        compressed: Whether to use compressed (9-bit) or uncompressed (16-bit) IQ
        data: Optional pre-defined data (if None, zeros are used)
    
    Returns:
        PRB data
    """
    if compressed:
        prb_size = PRB_9_SIZE  # 28 bytes per PRB (1 byte exponent + 27 bytes IQ)
    else:
        prb_size = PRB_16_SIZE  # 48 bytes per PRB
    
    total_size = num_prbs * prb_size
    
    if data is not None:
        if len(data) >= total_size:
            return Raw(data[:total_size])
        else:
            return Raw(data + bytes(total_size - len(data)))
    else:
        return Raw(total_size)


def craft_cplane_section1_header(
    section_type: int = 1,
    num_sections: int = 1,
    iq_width: int = IQ_BIT_WIDTH_COMPRESSED,
    comp_meth: int = 1,
) -> Packet:
    """
    Craft C-plane Section Type 1 header.
    
    Args:
        section_type: Section type (1=scheduled data, 3=PRACH)
        num_sections: Number of sections
        iq_width: IQ bit width
        comp_meth: Compression method
    Returns:
        Section 1 header
    """
    cplane_section1_header = CPlaneSection1Header(
        section_type=section_type,
        num_sections=num_sections,
        ud_iq_width=iq_width,
        ud_comp_meth=comp_meth,
    )
    return cplane_section1_header


def craft_cplane_section1(
    section_id: int,
    start_prbc: int,
    num_prbc: int,
    re_mask: int = 0xFFF,
    num_symbol: int = 14,
    beam_id: int = 0,
) -> Packet:
    """
    Craft C-plane Section Type 1 body.
    
    Args:
        section_id: Section ID
        start_prbc: Start PRB
        num_prbc: Number of PRBs (0 means 256)
        re_mask: Resource element mask
        num_symbol: Number of symbols
        beam_id: Beam ID
    
    Returns:
        Section 1 body
    """
    cplane_section1 = CPlaneSection1(
        section_id=section_id,
        rb=0,
        sym_inc=0,
        start_prbc=start_prbc,
        num_prbc=num_prbc,
        re_mask=re_mask,
        num_symbol=num_symbol,
        ef=0,
        beam_id=beam_id,
    )
    
    return cplane_section1


def craft_cplane_section3_header(
    time_offset: int = 0,
    scs: int = 1,
    fft_size: int = 12,
    cp_length: int = 0,
    num_sections: int = 1,
) -> Packet:
    """
    Craft C-plane Section Type 3 header (for PRACH).
    
    Args:
        time_offset: Time offset
        scs: Subcarrier spacing
        fft_size: FFT size
        cp_length: Cyclic prefix length
        num_sections: Number of sections
    
    Returns:
        Section 3 header
    """
    cplane_section3_hdr = CPlaneSection3Header(
        section_type=3,
        time_offset=time_offset,
        scs=scs,
        fft_size=fft_size,
        cp_length=cp_length,
        num_sections=num_sections,
    )
    return cplane_section3_hdr


def craft_cplane_section3(
    section_id: int,
    start_prbc: int = 0,
    num_prbc: int = 12,  # PRACH typically uses 12 PRBs
    re_mask: int = 0xFFF,
    num_symbol: int = 14,
    beam_id: int = 0,
    freq_offset: int = 0,
) -> Packet:
    """
    Craft C-plane Section Type 3 body (for PRACH).
    
    Args:
        section_id: Section ID
        start_prbc: Start PRB
        num_prbc: Number of PRBs
        re_mask: Resource element mask
        num_symbol: Number of symbols
        beam_id: Beam ID
        freq_offset: Frequency offset (24-bit signed)
    
    Returns:
        Section 3 body
    """
    cplane_section3 = CPlaneSection3(
        section_id=section_id,
        rb=0,
        sym_inc=0,
        start_prbc=start_prbc,
        num_prbc=num_prbc,
        re_mask=re_mask,
        num_symbol=num_symbol,
        ef=0,
        beam_id=beam_id,
        freq_offset=freq_offset,
        reserved=0,
    )
    return cplane_section3


def craft_uplane_dl_packet(
    src_mac: str,
    dst_mac: str,
    du_port_id: int,
    band_sector_id: int,
    cc_id: int,
    ru_port_id: int,
    frame_id: int,
    subframe_id: int,
    slot_id: int,
    symbol_id: int,
    section_id: int,
    start_prb: int,
    num_prbs: int,
    prb_data: Optional[bytes] = None,
    vlan_id: Optional[int] = None,
    seq_id: int = 0,
    ud_comp_hdr: bool = False,
) -> Packet:
    """
    Craft a complete U-plane downlink packet.
    
    Args:
        src_mac: Source MAC address
        dst_mac: Destination MAC address
        ru_port_id: RU port ID
        frame_id: Frame ID
        subframe_id: Subframe ID
        slot_id: Slot ID
        symbol_id: Symbol ID
        section_id: Section ID
        start_prb: Start PRB
        num_prbs: Number of PRBs (0 means 273)
        prb_data: PRB IQ data (optional)
        vlan_id: VLAN ID (optional)
        seq_id: Sequence ID
    
    Returns:
        Complete packet as bytes
    """
    # For PRB data generation, use actual number of PRBs
    actual_num_prbs = num_prbs if num_prbs != 0 else NUM_PRB
    
    # Build payload
    radio_hdr = craft_radio_app_header(DOWNLINK_DIRECTION, frame_id, subframe_id, slot_id, symbol_id)
    data_section_hdr = craft_data_section_header(section_id, start_prb, num_prbs)
    if ud_comp_hdr:
        comp_hdr = craft_compression_header()
    prb_payload = craft_prb_data(actual_num_prbs, compressed=True, data=prb_data)
    
    if ud_comp_hdr:
        payload = radio_hdr / data_section_hdr / comp_hdr / prb_payload
    else:
        payload = radio_hdr / data_section_hdr / prb_payload
    
    # eCPRI header
    ecpri_hdr = craft_ecpri_header(ECPRI_IQ_DATA, len(payload) + 4, du_port_id=du_port_id,
                                  band_sector_id=band_sector_id, cc_id=cc_id,
                                  ru_port_id=ru_port_id, seq_id=seq_id)
    
    # Ethernet header
    eth = craft_ethernet_header(src_mac, dst_mac, vlan_id)

    # combine all parts
    pkt = eth / ecpri_hdr / payload
    
    return pkt


def craft_uplane_ul_packet(
    src_mac: str,
    dst_mac: str,
    du_port_id: int,
    band_sector_id: int,
    cc_id: int,
    ru_port_id: int,
    frame_id: int,
    subframe_id: int,
    slot_id: int,
    symbol_id: int,
    section_id: int,
    start_prb: int,
    num_prbs: int,
    prb_data: Optional[bytes] = None,
    vlan_id: Optional[int] = None,
    seq_id: int = 0,
    ud_comp_hdr: bool = False,
) -> Packet:
    """
    Craft a complete U-plane uplink packet.
    
    Args:
        src_mac: Source MAC address (RU)
        dst_mac: Destination MAC address (middlebox)
        ru_port_id: RU port ID
        frame_id: Frame ID
        subframe_id: Subframe ID
        slot_id: Slot ID
        symbol_id: Symbol ID
        section_id: Section ID
        start_prb: Start PRB
        num_prbs: Number of PRBs (0 means 256/273)
        prb_data: PRB IQ data (optional)
        vlan_id: VLAN ID (optional)
        seq_id: Sequence ID
    
    Returns:
        Complete packet
    """
    # Build payload
    radio_hdr = craft_radio_app_header(UPLINK_DIRECTION, frame_id, subframe_id, slot_id, symbol_id)
    data_section_hdr = craft_data_section_header(section_id, start_prb, num_prbs)
    if ud_comp_hdr:
        comp_hdr = craft_compression_header()
    
    # For num_prbs=0, use 273 PRBs
    actual_num_prbs = num_prbs if num_prbs != 0 else 273
    prb_payload = craft_prb_data(actual_num_prbs, compressed=True, data=prb_data)
    
    if ud_comp_hdr:
        payload = radio_hdr / data_section_hdr / comp_hdr / prb_payload
    else:
        payload = radio_hdr / data_section_hdr / prb_payload
    
    # eCPRI header
    ecpri_hdr = craft_ecpri_header(ECPRI_IQ_DATA, len(payload) + 4, du_port_id=du_port_id,
                                  band_sector_id=band_sector_id, cc_id=cc_id,
                                  ru_port_id=ru_port_id, seq_id=seq_id)
    
    # Ethernet header
    eth = craft_ethernet_header(src_mac, dst_mac, vlan_id)

    # combine all parts
    pkt = eth / ecpri_hdr / payload

    return pkt 


def craft_uplane_prach_packet(
    src_mac: str,
    dst_mac: str,
    du_port_id: int,
    band_sector_id: int,
    cc_id: int,
    ru_port_id: int,
    frame_id: int,
    subframe_id: int,
    slot_id: int,
    symbol_id: int,
    prb_data: List[Tuple[int, bytes]],
    vlan_id: Optional[int] = None,
    seq_id: int = 0,
    ud_comp_hdr: bool = False,
) -> Packet:
    """
    Craft a complete U-plane PRACH packet with multiple sections.
    
    PRACH packets typically have multiple sections (one per DU) and use
    uncompressed IQ (16-bit) with 12 PRBs per section.
    
    Args:
        src_mac: Source MAC address (RU)
        dst_mac: Destination MAC address (middlebox)
        ru_port_id: RU port ID (should be >= 4 for PRACH)
        frame_id: Frame ID
        subframe_id: Subframe ID
        slot_id: Slot ID
        symbol_id: Symbol ID
        num_sections: Number of sections
        sections_data: List of (section_id, prb_data) tuples
        vlan_id: VLAN ID (optional)
        seq_id: Sequence ID
        ud_comp_hdr: bool = False,
    Returns:
        Complete packet as bytes
    """
    # Build payload
    # Radio app header with numOfSections
    radio_hdr = craft_radio_app_header(UPLINK_DIRECTION, frame_id, subframe_id, slot_id, symbol_id)
    data_section_hdr = craft_data_section_header(section_id=0, start_prbu=0, num_prbu=12)
    prb_payload = craft_prb_data(num_prbs=12, compressed=True)
    
    payload = radio_hdr / data_section_hdr / prb_payload
    
    # eCPRI header
    ecpri_hdr = craft_ecpri_header(ECPRI_IQ_DATA, len(payload) + 4, du_port_id=du_port_id,
                                  band_sector_id=band_sector_id, cc_id=cc_id,
                                  ru_port_id=ru_port_id, seq_id=seq_id)
    
    # Ethernet header
    eth = craft_ethernet_header(src_mac, dst_mac, vlan_id)

    # combine all parts
    pkt = eth / ecpri_hdr / payload
    
    return pkt


def craft_cplane_section1_packet(
    src_mac: str,
    dst_mac: str,
    du_port_id: int,
    band_sector_id: int,
    cc_id: int,
    ru_port_id: int,
    frame_id: int,
    subframe_id: int,
    slot_id: int,
    symbol_id: int,
    direction: int,
    section_id: int,
    start_prbc: int,
    num_prbc: int,
    vlan_id: Optional[int] = None,
    seq_id: int = 0,
) -> Packet:
    """
    Craft a complete C-plane Section Type 1 packet (for scheduled DL/UL data).
    
    Args:
        src_mac: Source MAC address
        dst_mac: Destination MAC address
        ru_port_id: RU port ID
        frame_id: Frame ID
        subframe_id: Subframe ID
        slot_id: Slot ID
        symbol_id: Symbol ID
        direction: 0=uplink, 1=downlink
        section_id: Section ID
        start_prbc: Start PRB
        num_prbc: Number of PRBs
        vlan_id: VLAN ID (optional)
        seq_id: Sequence ID
    
    Returns:
        Complete packet
    """
    # Build payload
    radio_hdr = craft_radio_app_header(direction, frame_id, subframe_id, slot_id, symbol_id)
    section1_hdr = craft_cplane_section1_header(num_sections=1)
    section1_body = craft_cplane_section1(section_id, start_prbc, num_prbc)
    
    payload = radio_hdr / section1_hdr / section1_body
    
    # eCPRI header
    ecpri_hdr = craft_ecpri_header(ECPRI_RT_CONTROL_DATA, len(payload) + 4, du_port_id=du_port_id,
                                  band_sector_id=band_sector_id, cc_id=cc_id,
                                  ru_port_id=ru_port_id, seq_id=seq_id)
    
    # Ethernet header
    eth = craft_ethernet_header(src_mac, dst_mac, vlan_id)
    
    # combine all parts
    pkt = eth / ecpri_hdr / payload

    # if less than minimum Ethernet frame size, pad with zeros
    if len(pkt) < 64:
        pkt = pkt / Raw(b'\x00' * (64 - len(pkt)))

    return pkt


def craft_cplane_section3_packet(
    src_mac: str,
    dst_mac: str,
    du_port_id: int,
    band_sector_id: int,
    cc_id: int,
    ru_port_id: int,
    frame_id: int,
    subframe_id: int,
    slot_id: int,
    symbol_id: int,
    section_id: int,
    freq_offset: int,
    vlan_id: Optional[int] = None,
    seq_id: int = 0,
    num_prbc: int = 12,
) -> Packet:
    """
    Craft a complete C-plane Section Type 3 packet (for PRACH).
    
    Args:
        src_mac: Source MAC address
        dst_mac: Destination MAC address
        ru_port_id: RU port ID
        frame_id: Frame ID
        subframe_id: Subframe ID
        slot_id: Slot ID
        symbol_id: Symbol ID
        section_id: Section ID
        freq_offset: Frequency offset (24-bit signed)
        vlan_id: VLAN ID (optional)
        seq_id: Sequence ID
        num_prbc: Number of PRBs (default 12 for PRACH)
    
    Returns:
        Complete packet as bytes
    """
    # Build payload
    radio_hdr = craft_radio_app_header(DOWNLINK_DIRECTION, frame_id, subframe_id, slot_id, symbol_id)
    section3_hdr = craft_cplane_section3_header(num_sections=1)
    section3_body = craft_cplane_section3(section_id, num_prbc=num_prbc, freq_offset=freq_offset)
    
    payload = radio_hdr / section3_hdr / section3_body
    
    # eCPRI header
    ecpri_hdr = craft_ecpri_header(ECPRI_RT_CONTROL_DATA, len(payload) + 4, du_port_id=du_port_id,
                                  band_sector_id=band_sector_id, cc_id=cc_id,
                                  ru_port_id=ru_port_id, seq_id=seq_id)
    
    # Ethernet header
    eth = craft_ethernet_header(src_mac, dst_mac, vlan_id)
    
    # combine all parts
    pkt = eth / ecpri_hdr / payload

    # if less than minimum Ethernet frame size, pad with zeros
    if len(pkt) < 64:
        pkt = pkt / Raw(b'\x00' * (64 - len(pkt)))

    return pkt
