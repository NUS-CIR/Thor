#ifndef NFAPI_P7_UPLINK_PROCESSOR_H
#define NFAPI_P7_UPLINK_PROCESSOR_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

#include "common_types.h"
#include "nfapi_nr_interface.h"
#include "nfapi_nr_interface_scf.h"

int process_rx_data_indication(const nfapi_nr_rx_data_indication_t *incoming, nfapi_nr_rx_data_indication_t *outgoing, int num_pnfs, nfapi_nr_rx_data_indication_t **rx_buf);
int process_crc_indication(const nfapi_nr_crc_indication_t *incoming, nfapi_nr_crc_indication_t *outgoing, int num_pnfs, nfapi_nr_crc_indication_t **crc_buf);
int process_srs_indication(const nfapi_nr_srs_indication_t *incoming, nfapi_nr_srs_indication_t *outgoing, int num_pnfs, nfapi_nr_srs_indication_t **srs_buf);
int process_uci_indication(const nfapi_nr_uci_indication_t *incoming, nfapi_nr_uci_indication_t *outgoing, int num_pnfs, nfapi_nr_uci_indication_t **uci_buf);

/**
 * Structure holding the raw buffers and their sizes for segmented merge
 */
typedef struct
{
    uint8_t *buffer;      // Raw packet buffer
    uint32_t buffer_size; // Size of the raw packet buffer
    int pnf_index;        // PNF index this buffer came from
} uplink_raw_buffer_t;

/**
 * Peek the number of PDUs from a raw packed P7 message buffer.
 * The offset of the PDU count field varies by message type.
 *
 * @param buffer Raw packed message buffer
 * @param buffer_size Size of the buffer
 * @param message_id The message ID to determine field layout
 * @return Number of PDUs, or -1 on error
 */
int peek_p7_pdu_count(const uint8_t *buffer, uint32_t buffer_size, uint16_t message_id);

/**
 * Get the offset of the PDU data (after header, SFN, Slot, and PDU count fields)
 * for a given message type.
 *
 * @param message_id The message ID
 * @return Offset in bytes to PDU list data, or -1 if unsupported
 */
int get_pdu_list_offset(uint16_t message_id);

/**
 * Create two segmented packets from two raw packed P7 uplink indication buffers.
 * The second buffer becomes segment 0 with updated PDU count = total PDUs.
 * The first buffer's PDU data becomes segment 1.
 *
 * This allows the VNF to reassemble them using its built-in segmentation support,
 * avoiding expensive unpack/copy/repack operations.
 *
 * @param first_buf First arriving buffer (will become segment 1 payload)
 * @param first_size Size of first buffer
 * @param second_buf Second arriving buffer (will become segment 0 with updated header)
 * @param second_size Size of second buffer
 * @param message_id The P7 message ID
 * @param sequence_num The sequence number to use for both segments
 * @param segment0_out Output buffer for segment 0 (caller must provide buffer of sufficient size)
 * @param segment0_size_out Output: size of segment 0
 * @param segment1_out Output buffer for segment 1 (caller must provide buffer of sufficient size)
 * @param segment1_size_out Output: size of segment 1
 * @return 0 on success, -1 on error
 */
int create_segmented_uplink_merge(
    uint8_t *first_buf, uint32_t *first_size,
    uint8_t *second_buf, uint32_t *second_size,
    uint16_t message_id, uint8_t sequence_num);

#endif // NFAPI_P7_UPLINK_PROCESSOR_H