#include "nfapi_p7_uplink_processor.h"
#include <string.h>
#include <stdio.h>

void __copy_srs_indication_report_tlv(const nfapi_srs_report_tlv_t *src, nfapi_srs_report_tlv_t *dst)
{
    dst->tag = src->tag;
    dst->length = src->length;
    for (int i = 0; i < (dst->length + 3) / 4; ++i)
    {
        dst->value[i] = src->value[i];
    }
}

void __copy_srs_indication_PDU(const nfapi_nr_srs_indication_pdu_t *src, nfapi_nr_srs_indication_pdu_t *dst)
{
    dst->handle = src->handle;
    dst->rnti = src->rnti;
    if(dst->rnti == 0xFFF0) {
        log_warn("SRS PDU with RESERVED RNTI 0xFFF0 detected, this may indicate an issue in uplink processing");
    }
    dst->timing_advance_offset = src->timing_advance_offset;
    dst->timing_advance_offset_nsec = src->timing_advance_offset_nsec;
    dst->srs_usage = src->srs_usage;
    dst->report_type = src->report_type;
    __copy_srs_indication_report_tlv(&src->report_tlv, &dst->report_tlv);
}

int process_srs_indication(const nfapi_nr_srs_indication_t *incoming, nfapi_nr_srs_indication_t *outgoing, int num_pnfs, nfapi_nr_srs_indication_t **srs_buf)
{
    outgoing->header = incoming->header;
    outgoing->sfn = incoming->sfn;
    outgoing->slot = incoming->slot;
    outgoing->control_length = 0; // TODO: how is this used?
    outgoing->number_of_pdus = 0;
    for (int i = 0; i < num_pnfs; i++)
    {
        if (srs_buf[i] == NULL)
        {
            continue;
        }
        outgoing->control_length += srs_buf[i]->control_length;
        outgoing->number_of_pdus += srs_buf[i]->number_of_pdus;
    }
    outgoing->pdu_list = calloc(outgoing->number_of_pdus, sizeof(nfapi_nr_srs_indication_pdu_t));

    // Loop through each PNF's SRS indication and merge PDUs
    uint8_t srs_index = 0;
    for (int i = 0; i < num_pnfs; i++)
    {
        if (srs_buf[i] == NULL)
        {
            continue;
        }
        nfapi_nr_srs_indication_t *pnf_srs_ind = srs_buf[i];
        for (size_t j = 0; j < pnf_srs_ind->number_of_pdus; j++)
        {
            __copy_srs_indication_PDU(&pnf_srs_ind->pdu_list[j], &outgoing->pdu_list[srs_index]);
            srs_index++;
        }
    }
    log_debug("Merged SRS_INDICATION has %d PDUs", outgoing->number_of_pdus);
    return 0;
}

void __copy_crc_indication_CRC(const nfapi_nr_crc_t *src, nfapi_nr_crc_t *dst)
{
    memset(dst, 0, sizeof(nfapi_nr_crc_t));
    dst->handle = src->handle;
    dst->rnti = src->rnti;
    if(dst->rnti == 0xFFF0) {
        log_warn("CRC INDICATION with RESERVED RNTI 0xFFF0 detected, this may indicate an issue in uplink processing");
    }
    dst->harq_id = src->harq_id;
    dst->tb_crc_status = src->tb_crc_status;
    dst->num_cb = src->num_cb;
    if (dst->num_cb > 0)
    {
        log_info("Copying CBG CRC status for RNTI %d, NumCB: %d", dst->rnti, dst->num_cb);
        const uint16_t cb_len = (dst->num_cb / 8) + 1;
        dst->cb_crc_status = calloc(cb_len, sizeof(uint8_t));
        for (int cb = 0; cb < cb_len; ++cb)
        {
            dst->cb_crc_status[cb] = src->cb_crc_status[cb];
        }
    }
    dst->ul_cqi = src->ul_cqi;
    dst->timing_advance = src->timing_advance;
    dst->rssi = src->rssi;
}

int process_crc_indication(const nfapi_nr_crc_indication_t *incoming, nfapi_nr_crc_indication_t *outgoing, int num_pnfs, nfapi_nr_crc_indication_t **crc_buf)
{
    outgoing->header = incoming->header;
    outgoing->sfn = incoming->sfn;
    outgoing->slot = incoming->slot;
    outgoing->number_crcs = 0;
    for (int i = 0; i < num_pnfs; i++)
    {
        if (crc_buf[i] == NULL)
        {
            continue;
        }
        outgoing->number_crcs += crc_buf[i]->number_crcs;
    }
    outgoing->crc_list = calloc(outgoing->number_crcs, sizeof(nfapi_nr_crc_t));
    uint8_t crc_index = 0;
    for (int i = 0; i < num_pnfs; i++)
    {
        if (crc_buf[i] == NULL)
        {
            continue;
        }
        for (int j = 0; j < crc_buf[i]->number_crcs; j++)
        {
            __copy_crc_indication_CRC(&crc_buf[i]->crc_list[j], &outgoing->crc_list[crc_index]);
            crc_index++;
        }
    }
    log_debug("Merged CRC_INDICATION has %d CRCs", outgoing->number_crcs);
    return 0;
}

void __copy_rx_data_indication_PDU(const nfapi_nr_rx_data_pdu_t *src, nfapi_nr_rx_data_pdu_t *dst)
{
    memset(dst, 0, sizeof(nfapi_nr_rx_data_pdu_t));
    dst->handle = src->handle;
    dst->rnti = src->rnti;
    if(dst->rnti == 0xFFF0) {
        log_warn("RX_DATA PDU with RESERVED RNTI 0xFFF0 detected, this may indicate an issue in uplink processing");
    }
    dst->harq_id = src->harq_id;
    dst->pdu_length = src->pdu_length;
    dst->ul_cqi = src->ul_cqi;
    dst->timing_advance = src->timing_advance;
    dst->rssi = src->rssi;
    dst->pdu = calloc(dst->pdu_length, sizeof(uint8_t));
    memcpy(dst->pdu, src->pdu, src->pdu_length);
}

int process_rx_data_indication(const nfapi_nr_rx_data_indication_t *incoming, nfapi_nr_rx_data_indication_t *outgoing, int num_pnfs, nfapi_nr_rx_data_indication_t **rx_buf)
{
    memset((void *)outgoing, 0, sizeof(nfapi_nr_rx_data_indication_t));

    outgoing->header = incoming->header;
    outgoing->sfn = incoming->sfn;
    outgoing->slot = incoming->slot;
    outgoing->number_of_pdus = 0;
    for (int i = 0; i < num_pnfs; i++)
    {
        if (rx_buf[i] == NULL)
        {
            continue;
        }
        outgoing->number_of_pdus += rx_buf[i]->number_of_pdus;
    }
    outgoing->pdu_list = calloc(outgoing->number_of_pdus, sizeof(nfapi_nr_rx_data_pdu_t));
    uint8_t pdu_index = 0;
    for (int i = 0; i < num_pnfs; i++)
    {
        if (rx_buf[i] == NULL)
        {
            continue;
        }
        for (int j = 0; j < rx_buf[i]->number_of_pdus; j++)
        {
            __copy_rx_data_indication_PDU(&rx_buf[i]->pdu_list[j], &outgoing->pdu_list[pdu_index]);
            pdu_index++;
        }
    }
    log_debug("Merged RX_DATA_INDICATION has %d PDUs", outgoing->number_of_pdus);
    return 0;
}

void __copy_uci_indication_sr_pdu_0_1(const nfapi_nr_sr_pdu_0_1_t *src, nfapi_nr_sr_pdu_0_1_t *dst)
{
    dst->sr_indication = src->sr_indication;
    dst->sr_confidence_level = src->sr_confidence_level;
}

void __copy_uci_indication_sr_pdu_2_3_4(const nfapi_nr_sr_pdu_2_3_4_t *src, nfapi_nr_sr_pdu_2_3_4_t *dst)
{
    dst->sr_bit_len = src->sr_bit_len;
    const uint16_t sr_len = (dst->sr_bit_len / 8) + 1;
    dst->sr_payload = calloc(sr_len, sizeof(*dst->sr_payload));
    for (int i = 0; i < sr_len; ++i)
    {
        dst->sr_payload[i] = src->sr_payload[i];
    }
}

void __copy_uci_indication_harq_pdu_0_1(const nfapi_nr_harq_pdu_0_1_t *src, nfapi_nr_harq_pdu_0_1_t *dst)
{
    dst->num_harq = src->num_harq;
    dst->harq_confidence_level = src->harq_confidence_level;
    for (int i = 0; i < dst->num_harq; ++i)
    {
        dst->harq_list[i].harq_value = src->harq_list[i].harq_value;
    }
}

void __copy_uci_indication_harq_pdu_2_3_4(const nfapi_nr_harq_pdu_2_3_4_t *src, nfapi_nr_harq_pdu_2_3_4_t *dst)
{
    dst->harq_crc = src->harq_crc;
    dst->harq_bit_len = src->harq_bit_len;
    const uint16_t harq_length = (dst->harq_bit_len / 8) + 1;
    dst->harq_payload = calloc(harq_length, sizeof(*dst->harq_payload));
    for (int i = 0; i < harq_length; ++i)
    {
        dst->harq_payload[i] = src->harq_payload[i];
    }
}

void __copy_uci_indication_csi_part1(const nfapi_nr_csi_part1_pdu_t *src, nfapi_nr_csi_part1_pdu_t *dst)
{
    dst->csi_part1_crc = src->csi_part1_crc;
    dst->csi_part1_bit_len = src->csi_part1_bit_len;
    const uint16_t payload_length = (dst->csi_part1_bit_len / 8) + 1;
    dst->csi_part1_payload = calloc(payload_length, sizeof(*dst->csi_part1_payload));
    for (int i = 0; i < payload_length; ++i)
    {
        dst->csi_part1_payload[i] = src->csi_part1_payload[i];
    }
}

void __copy_uci_indication_csi_part2(const nfapi_nr_csi_part2_pdu_t *src, nfapi_nr_csi_part2_pdu_t *dst)
{
    dst->csi_part2_crc = src->csi_part2_crc;
    dst->csi_part2_bit_len = src->csi_part2_bit_len;
    const uint16_t payload_length = (dst->csi_part2_bit_len / 8) + 1;
    dst->csi_part2_payload = calloc(payload_length, sizeof(*dst->csi_part2_payload));
    for (int i = 0; i < payload_length; ++i)
    {
        dst->csi_part2_payload[i] = src->csi_part2_payload[i];
    }
}

void __copy_uci_indication_PUSCH(const nfapi_nr_uci_pusch_pdu_t *src, nfapi_nr_uci_pusch_pdu_t *dst)
{
    dst->pduBitmap = src->pduBitmap;
    dst->handle = src->handle;
    dst->rnti = src->rnti;
    dst->ul_cqi = src->ul_cqi;
    dst->timing_advance = src->timing_advance;
    dst->rssi = src->rssi;

    // Bit 0 not used in PUSCH PDU
    // HARQ
    if ((dst->pduBitmap >> 1) & 0x01)
    {
        __copy_uci_indication_harq_pdu_2_3_4(&src->harq, &dst->harq);
    }
    // CSI Part 1
    if ((dst->pduBitmap >> 2) & 0x01)
    {
        __copy_uci_indication_csi_part1(&src->csi_part1, &dst->csi_part1);
    }
    // CSI Part 2
    if ((dst->pduBitmap >> 3) & 0x01)
    {
        __copy_uci_indication_csi_part2(&src->csi_part2, &dst->csi_part2);
    }
}

void __copy_uci_indication_PUCCH_0_1(const nfapi_nr_uci_pucch_pdu_format_0_1_t *src, nfapi_nr_uci_pucch_pdu_format_0_1_t *dst)
{
    dst->pduBitmap = src->pduBitmap;
    dst->handle = src->handle;
    dst->rnti = src->rnti;
    if(dst->rnti == 0xFFF0) {
        log_warn("UCI PDU with RESERVED RNTI 0xFFF0 detected, this may indicate an issue in uplink processing");
    }
    dst->pucch_format = src->pucch_format;
    dst->ul_cqi = src->ul_cqi;
    dst->timing_advance = src->timing_advance;
    dst->rssi = src->rssi;

    // SR
    if (dst->pduBitmap & 0x01)
    {
        __copy_uci_indication_sr_pdu_0_1(&src->sr, &dst->sr);
    }
    // HARQ
    if ((dst->pduBitmap >> 1) & 0x01)
    {
        __copy_uci_indication_harq_pdu_0_1(&src->harq, &dst->harq);
    }
}

void __copy_uci_indication_PUCCH_2_3_4(const nfapi_nr_uci_pucch_pdu_format_2_3_4_t *src, nfapi_nr_uci_pucch_pdu_format_2_3_4_t *dst)
{
    dst->pduBitmap = src->pduBitmap;
    dst->handle = src->handle;
    dst->rnti = src->rnti;
    if(dst->rnti == 0xFFF0) {
        log_warn("UCI PDU with RESERVED RNTI 0xFFF0 detected, this may indicate an issue in uplink processing");
    }
    dst->pucch_format = src->pucch_format;
    dst->ul_cqi = src->ul_cqi;
    dst->timing_advance = src->timing_advance;
    dst->rssi = src->rssi;
    // SR
    if (dst->pduBitmap & 0x01)
    {
        __copy_uci_indication_sr_pdu_2_3_4(&src->sr, &dst->sr);
    }
    // HARQ
    if ((dst->pduBitmap >> 1) & 0x01)
    {
        __copy_uci_indication_harq_pdu_2_3_4(&src->harq, &dst->harq);
    }
    // CSI Part 1
    if ((dst->pduBitmap >> 2) & 0x01)
    {
        __copy_uci_indication_csi_part1(&src->csi_part1, &dst->csi_part1);
    }
    // CSI Part 2
    if ((dst->pduBitmap >> 3) & 0x01)
    {
        __copy_uci_indication_csi_part2(&src->csi_part2, &dst->csi_part2);
    }
}

void __copy_uci_indication_UCI(const nfapi_nr_uci_t *src, nfapi_nr_uci_t *dst)
{
    dst->pdu_type = src->pdu_type;
    dst->pdu_size = src->pdu_size;
    switch (dst->pdu_type)
    {
    case NFAPI_NR_UCI_PUSCH_PDU_TYPE:
        __copy_uci_indication_PUSCH(&src->pusch_pdu, &dst->pusch_pdu);
        break;
    case NFAPI_NR_UCI_FORMAT_0_1_PDU_TYPE:
        __copy_uci_indication_PUCCH_0_1(&src->pucch_pdu_format_0_1, &dst->pucch_pdu_format_0_1);
        break;
    case NFAPI_NR_UCI_FORMAT_2_3_4_PDU_TYPE:
        __copy_uci_indication_PUCCH_2_3_4(&src->pucch_pdu_format_2_3_4, &dst->pucch_pdu_format_2_3_4);
        break;
    default:
        log_error(1 == 0, "Unknown UCI.indication PDU Type %d\n", src->pdu_type);
        break;
    }
}

int process_uci_indication(const nfapi_nr_uci_indication_t *incoming, nfapi_nr_uci_indication_t *outgoing, int num_pnfs, nfapi_nr_uci_indication_t **uci_buf)
{
    memset((void *)outgoing, 0, sizeof(nfapi_nr_uci_indication_t));

    outgoing->header = incoming->header;
    outgoing->sfn = incoming->sfn;
    outgoing->slot = incoming->slot;
    outgoing->num_ucis = 0;
    for (int i = 0; i < num_pnfs; i++)
    {
        if (uci_buf[i] == NULL)
        {
            continue;
        }
        outgoing->num_ucis += uci_buf[i]->num_ucis;
    }
    outgoing->uci_list = calloc(outgoing->num_ucis, sizeof(nfapi_nr_uci_t));
    uint8_t pdu_index = 0;
    for (int i = 0; i < num_pnfs; i++)
    {
        if (uci_buf[i] == NULL)
        {
            continue;
        }
        for (int j = 0; j < uci_buf[i]->num_ucis; j++)
        {
            __copy_uci_indication_UCI(&uci_buf[i]->uci_list[j], &outgoing->uci_list[pdu_index]);
            pdu_index++;
        }
    }
    log_debug("Merged UCI_INDICATION has %d PDUs", outgoing->num_ucis);
    return 0;
}

/**
 * Helper: Read a big-endian uint16 from buffer
 */
static inline uint16_t read_be16(const uint8_t *buf)
{
    return ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
}

/**
 * Helper: Write a big-endian uint16 to buffer
 */
static inline void write_be16(uint8_t *buf, uint16_t val)
{
    buf[0] = (val >> 8) & 0xFF;
    buf[1] = val & 0xFF;
}

/**
 * Helper: Read a big-endian uint32 from buffer
 */
static inline uint32_t read_be32(const uint8_t *buf)
{
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

/**
 * Helper: Write a big-endian uint32 to buffer
 */
static inline void write_be32(uint8_t *buf, uint32_t val)
{
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
}

int get_pdu_list_offset(uint16_t message_id)
{
    // P7 Header: 18 bytes
    // After header: SFN (2) + Slot (2) + PDU count field (2)
    // PDU list starts at offset 24 for most indication messages
    
    switch (message_id)
    {
    case NFAPI_NR_PHY_MSG_TYPE_RX_DATA_INDICATION:
        // Header(18) + SFN(2) + Slot(2) + number_of_pdus(2) = 24
        return NFAPI_NR_P7_HEADER_LENGTH + 2 + 2 + 2;
    case NFAPI_NR_PHY_MSG_TYPE_CRC_INDICATION:
        // Header(18) + SFN(2) + Slot(2) + number_crcs(2) = 24
        return NFAPI_NR_P7_HEADER_LENGTH + 2 + 2 + 2;
    case NFAPI_NR_PHY_MSG_TYPE_UCI_INDICATION:
        // Header(18) + SFN(2) + Slot(2) + num_ucis(2) = 24
        return NFAPI_NR_P7_HEADER_LENGTH + 2 + 2 + 2;
    case NFAPI_NR_PHY_MSG_TYPE_SRS_INDICATION:
        // Header(18) + SFN(2) + Slot(2) + control_length(2) + number_of_pdus(2) = 26
        return NFAPI_NR_P7_HEADER_LENGTH + 2 + 2 + 2 + 2;
    default:
        return -1;
    }
}

int peek_p7_pdu_count(const uint8_t *buffer, uint32_t buffer_size, uint16_t message_id)
{
    // P7 Header: 18 bytes
    // SFN: 2 bytes (offset 18-19)
    // Slot: 2 bytes (offset 20-21)
    // PDU count: 2 bytes (offset 22-23) for most messages
    
    if (buffer == NULL || buffer_size < NFAPI_NR_P7_HEADER_LENGTH + 6)
    {
        return -1;
    }
    
    switch (message_id)
    {
    case NFAPI_NR_PHY_MSG_TYPE_RX_DATA_INDICATION:
    case NFAPI_NR_PHY_MSG_TYPE_CRC_INDICATION:
    case NFAPI_NR_PHY_MSG_TYPE_UCI_INDICATION:
        // PDU count at offset 22 (after header + SFN + Slot)
        return read_be16(&buffer[NFAPI_NR_P7_HEADER_LENGTH + 4]);
    case NFAPI_NR_PHY_MSG_TYPE_SRS_INDICATION:
        // For SRS: control_length is at offset 22, number_of_pdus at offset 24
        if (buffer_size < NFAPI_NR_P7_HEADER_LENGTH + 8)
        {
            return -1;
        }
        return read_be16(&buffer[NFAPI_NR_P7_HEADER_LENGTH + 6]);
    default:
        return -1;
    }
}

int create_segmented_uplink_merge(
    uint8_t *first_buf, uint32_t *first_size,
    uint8_t *second_buf, uint32_t *second_size,
    uint16_t message_id, uint8_t sequence_num)
{
    if (first_buf == NULL || second_buf == NULL)
    {
        return -1;
    }
    
    // Get the offset where PDU list data starts
    int pdu_list_offset = get_pdu_list_offset(message_id);
    if (pdu_list_offset < 0)
    {
        log_error("Unsupported message type for segmented merge: 0x%04x", message_id);
        return -1;
    }
    
    // Get PDU counts from both buffers
    int first_pdu_count = peek_p7_pdu_count(first_buf, *first_size, message_id);
    int second_pdu_count = peek_p7_pdu_count(second_buf, *second_size, message_id);
    
    if (first_pdu_count < 0 || second_pdu_count < 0)
    {
        log_error("Failed to peek PDU count from buffers");
        return -1;
    }
    
    uint16_t total_pdu_count = (uint16_t)(first_pdu_count + second_pdu_count);
    
    log_trace("Creating segmented merge: first_pdus=%d, second_pdus=%d, total=%d",
              first_pdu_count, second_pdu_count, total_pdu_count);
    
    // ========== Create Segment 0 ==========
    // segment0_out = first_buf; // We can reuse the first buffer for segment 0 to avoid extra copying
    // memcpy(segment0_out, first_buf, first_size);
    
    // Update the PDU count field in segment 0 to the total
    switch (message_id)
    {
    case NFAPI_NR_PHY_MSG_TYPE_RX_DATA_INDICATION:
    case NFAPI_NR_PHY_MSG_TYPE_CRC_INDICATION:
    case NFAPI_NR_PHY_MSG_TYPE_UCI_INDICATION:
        // PDU count at offset 22
        write_be16(&first_buf[NFAPI_NR_P7_HEADER_LENGTH + 4], total_pdu_count);
        break;
    case NFAPI_NR_PHY_MSG_TYPE_SRS_INDICATION:
        // number_of_pdus at offset 24
        write_be16(&first_buf[NFAPI_NR_P7_HEADER_LENGTH + 6], total_pdu_count);
        break;
    default:
        return -1;
    }
    
    // Calculate the total message length after reassembly
    // This is: second buffer size + first buffer's PDU data size
    // uint32_t second_pdu_data_size = second_size - pdu_list_offset;
    // uint32_t total_reassembled_length = first_size + second_pdu_data_size;
    
    // Update message_length in segment 0 header (offset 4-7)
    // write_be32(&segment0_out[4], total_reassembled_length);
    
    // Set M=1 (more segments), segment=0, sequence=sequence_num
    uint16_t mss_seg0 = NFAPI_NR_P7_SET_MSS(1, 0, sequence_num);
    write_be16(&first_buf[8], mss_seg0);
    
    // Segment 0 size is the original first buffer size;
    // *segment0_size_out = first_size; 
    
    // ========== Create Segment 1 ==========
    // Segment 1 contains:
    // - P7 header (18 bytes, with M=0, segment=1, same sequence)
    // - PDU list data from first buffer (skipping header + SFN + Slot + PDU count)
    
    // Copy the header from second buffer
    // segment1_out = second_buf; // We can reuse the second buffer for segment 1 to avoid extra copying
    // memcpy(segment1_out, second_buf, NFAPI_NR_P7_HEADER_LENGTH);
    
    // Update message_length in segment 1 header
    uint32_t second_pdu_data_size = *second_size - pdu_list_offset;
    uint32_t segment1_length = NFAPI_NR_P7_HEADER_LENGTH + second_pdu_data_size;
    write_be32(&second_buf[4], segment1_length);
    
    // Set M=0 (last segment), segment=1, sequence=sequence_num
    uint16_t mss_seg1 = NFAPI_NR_P7_SET_MSS(0, 1, sequence_num);
    write_be16(&second_buf[8], mss_seg1);
    
    // Copy the PDU list data from second buffer (after header + SFN/Slot/count)
    // memcpy(&segment1_out[NFAPI_NR_P7_HEADER_LENGTH], 
    //        &second_buf[pdu_list_offset], 
    //        second_pdu_data_size);
    memmove(&second_buf[NFAPI_NR_P7_HEADER_LENGTH], 
           &second_buf[pdu_list_offset], 
           second_pdu_data_size);
    
    *second_size = segment1_length;
    
    log_debug("Segment 0: size=%d, M=1, segment=0, seq=%d", *first_size, sequence_num);
    log_debug("Segment 1: size=%d, M=0, segment=1, seq=%d", *second_size, sequence_num);
    return 0;
}