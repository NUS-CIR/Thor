#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#include "nfapi_nr_interface.h"
#include "nfapi_nr_interface_scf.h"

#ifndef MAX_NUM_PNF
#define MAX_NUM_PNF 2
#endif

#ifndef MAX_MSG_SIZE
#define MAX_MSG_SIZE 65536
#endif

#ifndef RNTI_MAX
#define RNTI_MAX 65536
#endif

#ifndef MAX_NR_HARQ_PROCESSES
#define MAX_NR_HARQ_PROCESSES 16
#endif

#ifndef MAX_SFN
#define MAX_SFN 1024
#endif
#ifndef MAX_SLOT
#define MAX_SLOT 20
#endif
typedef struct
{
    uint16_t sfn;
    uint16_t slot;
    uint16_t prb_symbol_map[273][14]; // max 273 PRBs and 14 symbols
} uplink_scheduling_info_t;

typedef struct
{
#ifdef STAGED_HARQ_UPLINK
    uint16_t ue_rnti_to_pnf_prev[RNTI_MAX][MAX_NR_HARQ_PROCESSES]; // previous mapping per HARQ process
    uint16_t ue_rnti_to_pnf_curr[RNTI_MAX][MAX_NR_HARQ_PROCESSES]; // current mapping per HARQ process
    uint16_t ue_rnti_to_pnf_next[RNTI_MAX][MAX_NR_HARQ_PROCESSES]; // staged mapping per HARQ process
#else
    uint16_t ue_rnti_to_pnf_prev[RNTI_MAX]; // used to keep track of the previously used
    uint16_t ue_rnti_to_pnf_curr[RNTI_MAX]; // current mapping
    uint16_t ue_rnti_to_pnf_next[RNTI_MAX]; // used to prepare to commit updates
#endif
} uplink_rnti_to_pnf_mapping_t;

typedef struct
{
    uint16_t ue_rnti_to_pnf_prev[RNTI_MAX]; // used to keep track of the previously used
    uint16_t ue_rnti_to_pnf_curr[RNTI_MAX]; // current mapping
    uint16_t ue_rnti_to_pnf_next[RNTI_MAX]; // used to prepare to commit updates
    uint16_t pdsch_to_tx_data_mapping[MAX_NUM_PNF][MAX_SFN][MAX_SLOT][NFAPI_NR_MAX_DL_TTI_PDUS];
} downlink_rnti_to_pnf_mapping_t;

typedef struct
{
    uint8_t slot_ind_count;
    uint8_t rach_ind_count;
    uint8_t uci_ind_count;
    uint8_t rx_ind_count;
    uint8_t crc_ind_count;
    uint8_t srs_ind_count;

    uint8_t slot_ind_expected;
    uint8_t rach_ind_expected;
    uint8_t uci_ind_expected;
    uint8_t rx_ind_expected;
    uint8_t crc_ind_expected;
    uint8_t srs_ind_expected;

    nfapi_nr_rx_data_indication_t *rx_buf[MAX_NUM_PNF];
    nfapi_nr_crc_indication_t *crc_buf[MAX_NUM_PNF];
    nfapi_nr_uci_indication_t *uci_buf[MAX_NUM_PNF];
    nfapi_nr_srs_indication_t *srs_buf[MAX_NUM_PNF];

    // Raw buffers for segmentation-based merging
    uint8_t *rx_raw_buf[MAX_NUM_PNF];
    uint32_t rx_raw_buf_size[MAX_NUM_PNF];
    uint8_t *crc_raw_buf[MAX_NUM_PNF];
    uint32_t crc_raw_buf_size[MAX_NUM_PNF];
} uplink_fapi_info_t;

#endif // COMMON_TYPES_H