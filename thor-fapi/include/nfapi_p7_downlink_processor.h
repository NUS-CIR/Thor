#ifndef P7_DOWNLINK_PROCESSOR_H
#define P7_DOWNLINK_PROCESSOR_H

#include <stdint.h>
#include <stdlib.h>

#include "log.h"

#include "common_types.h"
#include "nfapi_nr_interface.h"
#include "nfapi_nr_interface_scf.h"

#ifdef DL_LEGACY
int process_nfapi_dl_tti_request(const nfapi_nr_dl_tti_request_t *incoming, nfapi_nr_dl_tti_request_t *outgoing, int *pnf_list, int num_pnfs, downlink_rnti_to_pnf_mapping_t *rnti_map, bool *outgoing_mask);
int process_nfapi_tx_data_request(const nfapi_nr_tx_data_request_t *incoming, nfapi_nr_tx_data_request_t *outgoing, int *pnf_list, int num_pnfs, downlink_rnti_to_pnf_mapping_t *rnti_map, bool *outgoing_mask);
int process_nfapi_ul_tti_request(const nfapi_nr_ul_tti_request_t *incoming, nfapi_nr_ul_tti_request_t *outgoing, int *pnf_list, int num_pnfs, uplink_rnti_to_pnf_mapping_t *rnti_map, uplink_scheduling_info_t *scheduling_info, bool *outgoing_mask);
int process_nfapi_ul_dci_request(const nfapi_nr_ul_dci_request_t *incoming, nfapi_nr_ul_dci_request_t *outgoing, int num_pnfs, bool *outgoing_mask);
#else
int handle_nfapi_dl_tti_request(void *input_buffer, int input_size, int *pnf_list, int num_pnfs, int primary_pnf, downlink_rnti_to_pnf_mapping_t *rnti_map, uint8_t **output_buffers, int *output_sizes);
int handle_nfapi_tx_data_request(void *input_buffer, int input_size, int *pnf_list, int num_pnfs, int primary_pnf, uint8_t **output_buffers, int *output_sizes);
int handle_nfapi_ul_tti_request(void *input_buffer, int input_size, int *pnf_list, int num_pnfs, int primary_pnf, uplink_rnti_to_pnf_mapping_t *rnti_map, uplink_fapi_info_t *fapi_info, uint8_t **output_buffers, int *output_sizes);
int handle_nfapi_ul_dci_request(void *input_buffer, int input_size, int *pnf_list, int num_pnfs,  int primary_pnf, uint8_t **output_buffers, int *output_sizes);
#endif

#endif // P7_DOWNLINK_PROCESSOR_H