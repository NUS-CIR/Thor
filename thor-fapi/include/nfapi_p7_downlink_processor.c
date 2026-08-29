#include "nfapi_p7_downlink_processor.h"
#include "nr_fapi_p7_utils.h"
#include <string.h>
#include <stdio.h>

#ifdef DL_LEGACY
int process_nfapi_dl_tti_request(const nfapi_nr_dl_tti_request_t *incoming, nfapi_nr_dl_tti_request_t *outgoing, int *pnf_list, int num_pnfs, downlink_rnti_to_pnf_mapping_t *rnti_map, bool *outgoing_mask)
{
    // initialize outgoing messages
    for (int i = 0; i < num_pnfs; i++)
    {
        memset((void *)(outgoing + i), 0, sizeof(nfapi_nr_dl_tti_request_t));
        outgoing_mask[i] = false;

        nfapi_nr_dl_tti_request_t *out_msg = (nfapi_nr_dl_tti_request_t *)(outgoing + i);
        memcpy(&out_msg->header, &incoming->header, sizeof(nfapi_nr_p7_message_header_t));
        out_msg->SFN = incoming->SFN;
        out_msg->Slot = incoming->Slot;
        out_msg->dl_tti_request_body.nPDUs = 0;
        out_msg->dl_tti_request_body.nGroup = 0;
    }

    // Track local PDU indices per PNF for pduIndex rewriting
    uint16_t pnf_pdsch_pdu_count[MAX_NUM_PNF] = {0};
    // Track global PDSCH order to align with TX_DATA PDU ordering
    uint16_t pdsch_global_idx = 0;

    // Loop through each incoming PDU
    for (size_t j = 0; j < incoming->dl_tti_request_body.nPDUs; j++)
    {
        uint16_t rnti;
        uint16_t pnf_index;
        int target_pnf_idx = -1;

        // Determine which PNF(s) should get this PDU
        switch (incoming->dl_tti_request_body.dl_tti_pdu_list[j].PDUType)
        {
        case NFAPI_NR_DL_TTI_SSB_PDU_TYPE:
        case NFAPI_NR_DL_TTI_PDCCH_PDU_TYPE:
        case NFAPI_NR_DL_TTI_CSI_RS_PDU_TYPE:
            // Broadcast PDUs go to first PNF only
            target_pnf_idx = 0;
            break;

        case NFAPI_NR_DL_TTI_PDSCH_PDU_TYPE:
            // UE-specific PDU - look up which PNF handles this RNTI
            rnti = incoming->dl_tti_request_body.dl_tti_pdu_list[j].pdsch_pdu.pdsch_pdu_rel15.rnti;
            pnf_index = rnti_map->ue_rnti_to_pnf_curr[rnti];

            // Find the index in pnf_list that matches this PNF
            for (int i = 0; i < num_pnfs; i++)
            {
                if (pnf_list[i] == pnf_index)
                {
                    target_pnf_idx = i;
                    break;
                }
            }
            break;

        default:
            log_warn("Unknown PDU type %d", incoming->dl_tti_request_body.dl_tti_pdu_list[j].PDUType);
            return -1;
        }

        // Skip if no valid target found or PNF is disabled
        if (target_pnf_idx < 0 || pnf_list[target_pnf_idx] == -1)
        {
            continue;
        }

        // Bounds check
        if (j >= NFAPI_NR_MAX_DL_TTI_PDUS)
        {
            log_error("PDU count exceeds maximum: %d >= %d", j, NFAPI_NR_MAX_DL_TTI_PDUS);
            return -1;
        }

        // Add PDU to the target PNF's outgoing message
        nfapi_nr_dl_tti_request_t *out_msg = (nfapi_nr_dl_tti_request_t *)(outgoing + target_pnf_idx);
        uint16_t out_pdu_idx = out_msg->dl_tti_request_body.nPDUs;

        out_msg->dl_tti_request_body.dl_tti_pdu_list[out_pdu_idx].PDUType = incoming->dl_tti_request_body.dl_tti_pdu_list[j].PDUType;
        out_msg->dl_tti_request_body.dl_tti_pdu_list[out_pdu_idx].PDUSize = incoming->dl_tti_request_body.dl_tti_pdu_list[j].PDUSize;

        switch (incoming->dl_tti_request_body.dl_tti_pdu_list[j].PDUType)
        {
        case NFAPI_NR_DL_TTI_SSB_PDU_TYPE:
            memcpy(&out_msg->dl_tti_request_body.dl_tti_pdu_list[out_pdu_idx].ssb_pdu,
                   &incoming->dl_tti_request_body.dl_tti_pdu_list[j].ssb_pdu,
                   sizeof(nfapi_nr_dl_tti_ssb_pdu));
            break;

        case NFAPI_NR_DL_TTI_PDCCH_PDU_TYPE:
            memcpy(&out_msg->dl_tti_request_body.dl_tti_pdu_list[out_pdu_idx].pdcch_pdu,
                   &incoming->dl_tti_request_body.dl_tti_pdu_list[j].pdcch_pdu,
                   sizeof(nfapi_nr_dl_tti_pdcch_pdu));
            break;

        case NFAPI_NR_DL_TTI_CSI_RS_PDU_TYPE:
            memcpy(&out_msg->dl_tti_request_body.dl_tti_pdu_list[out_pdu_idx].csi_rs_pdu,
                   &incoming->dl_tti_request_body.dl_tti_pdu_list[j].csi_rs_pdu,
                   sizeof(nfapi_nr_dl_tti_csi_rs_pdu));
            break;

        case NFAPI_NR_DL_TTI_PDSCH_PDU_TYPE:
            memcpy(&out_msg->dl_tti_request_body.dl_tti_pdu_list[out_pdu_idx].pdsch_pdu,
                   &incoming->dl_tti_request_body.dl_tti_pdu_list[j].pdsch_pdu,
                   sizeof(nfapi_nr_dl_tti_pdsch_pdu));

            // Rewrite pduIndex to be sequential for this PNF
            out_msg->dl_tti_request_body.dl_tti_pdu_list[out_pdu_idx].pdsch_pdu.pdsch_pdu_rel15.pduIndex = pnf_pdsch_pdu_count[target_pnf_idx]++;

            // Store mapping for TX_DATA processing using global PDSCH order (matches TX_DATA PDU list)
            if (pdsch_global_idx >= NFAPI_NR_MAX_DL_TTI_PDUS)
            {
                log_error("PDSCH global index exceeds maximum: %d >= %d", pdsch_global_idx, NFAPI_NR_MAX_DL_TTI_PDUS);
                return -1;
            }
            rnti_map->pdsch_to_tx_data_mapping[target_pnf_idx][incoming->SFN][incoming->Slot][pdsch_global_idx++] = pnf_list[target_pnf_idx];
            break;
        }

        out_msg->dl_tti_request_body.nPDUs++;
        outgoing_mask[target_pnf_idx] = true;
    }

    // Log summary for each PNF
    for (int i = 0; i < num_pnfs; i++)
    {
        if (outgoing_mask[i])
        {
            nfapi_nr_dl_tti_request_t *out_msg = (nfapi_nr_dl_tti_request_t *)(outgoing + i);
            log_debug("Outgoing DL_TTI.request for PNF %d has %d PDUs", pnf_list[i], out_msg->dl_tti_request_body.nPDUs);
        }
    }

    return 0;
}

int process_nfapi_tx_data_request(const nfapi_nr_tx_data_request_t *incoming, nfapi_nr_tx_data_request_t *outgoing, int *pnf_list, int num_pnfs, downlink_rnti_to_pnf_mapping_t *rnti_map, bool *outgoing_mask)
{
    // initialize outgoing messages
    for (int i = 0; i < num_pnfs; i++)
    {
        memset((void *)(outgoing + i), 0, sizeof(nfapi_nr_tx_data_request_t));
        outgoing_mask[i] = false;

        nfapi_nr_tx_data_request_t *out_msg = (nfapi_nr_tx_data_request_t *)(outgoing + i);
        memcpy(&out_msg->header, &incoming->header, sizeof(nfapi_nr_p7_message_header_t));
        out_msg->SFN = incoming->SFN;
        out_msg->Slot = incoming->Slot;
        out_msg->Number_of_PDUs = 0;
    }

    // Track local PDU indices per PNF for pduIndex rewriting
    uint16_t pnf_tx_pdu_count[MAX_NUM_PNF] = {0};

    // Loop through each incoming TX_DATA PDU
    for (int j = 0; j < incoming->Number_of_PDUs; j++)
    {
        // Bounds check
        if (j >= NFAPI_NR_MAX_DL_TTI_PDUS)
        {
            log_error("TX_DATA PDU index exceeds maximum: %d >= %d", j, NFAPI_NR_MAX_DL_TTI_PDUS);
            return -1;
        }

        // Look up which PNF should receive this TX_DATA PDU based on DL_TTI mapping
        // Loop through each PNF to find the match
        int target_pnf_idx = -1;
        for (int i = 0; i < num_pnfs; i++)
        {
            if (pnf_list[i] == -1)
            {
                continue;
            }

            uint16_t mapped_pnf = rnti_map->pdsch_to_tx_data_mapping[i][incoming->SFN][incoming->Slot][j];
            if (mapped_pnf == pnf_list[i])
            {
                target_pnf_idx = i;
                break;
            }
        }

        // Skip if no valid target found
        if (target_pnf_idx < 0)
        {
            continue;
        }

        // Add PDU to the target PNF's outgoing message
        nfapi_nr_tx_data_request_t *out_msg = (nfapi_nr_tx_data_request_t *)(outgoing + target_pnf_idx);
        uint16_t out_pdu_idx = out_msg->Number_of_PDUs;

        // Copy the PDU data
        out_msg->pdu_list[out_pdu_idx].PDU_length = incoming->pdu_list[j].PDU_length;
        out_msg->pdu_list[out_pdu_idx].num_TLV = incoming->pdu_list[j].num_TLV;
        memcpy(&out_msg->pdu_list[out_pdu_idx].TLVs,
               &incoming->pdu_list[j].TLVs,
               sizeof(nfapi_nr_tx_data_request_tlv_t) * incoming->pdu_list[j].num_TLV);

        // Rewrite pdu_index to be sequential for this PNF
        out_msg->pdu_list[out_pdu_idx].PDU_index = pnf_tx_pdu_count[target_pnf_idx]++;

        out_msg->Number_of_PDUs++;
        outgoing_mask[target_pnf_idx] = true;
    }

    // Log summary for each PNF
    for (int i = 0; i < num_pnfs; i++)
    {
        if (outgoing_mask[i])
        {
            nfapi_nr_tx_data_request_t *out_msg = (nfapi_nr_tx_data_request_t *)(outgoing + i);
            log_debug("Outgoing TX_DATA.request for PNF %d has %d PDUs", pnf_list[i], out_msg->Number_of_PDUs);
        }
    }

    // Clear the mapping for this specific slot to prevent stale data
    for (int i = 0; i < num_pnfs; i++)
    {
        memset(rnti_map->pdsch_to_tx_data_mapping[i][incoming->SFN][incoming->Slot], -1,
               sizeof(rnti_map->pdsch_to_tx_data_mapping[i][incoming->SFN][incoming->Slot]));
    }

    return 0;
}

int process_nfapi_ul_tti_request(const nfapi_nr_ul_tti_request_t *incoming, nfapi_nr_ul_tti_request_t *outgoing, int *pnf_list, int num_pnfs, uplink_rnti_to_pnf_mapping_t *rnti_map, uplink_scheduling_info_t *scheduling_info, bool *outgoing_mask)
{
    // initialize outgoing messages
    for (int i = 0; i < num_pnfs; i++)
    {
        memset((void *)(outgoing + i), 0, sizeof(nfapi_nr_ul_tti_request_t));
        outgoing_mask[i] = false;

        nfapi_nr_ul_tti_request_t *out_msg = (nfapi_nr_ul_tti_request_t *)(outgoing + i);
        memcpy(&out_msg->header, &incoming->header, sizeof(nfapi_nr_p7_message_header_t));
        out_msg->SFN = incoming->SFN;
        out_msg->Slot = incoming->Slot;
        out_msg->n_pdus = 0;
        out_msg->rach_present = 0;
        out_msg->n_ulsch = 0;
        out_msg->n_ulcch = 0;
        out_msg->n_group = 0;
    }

    // Initialize the scheduling info structure
    scheduling_info->sfn = incoming->SFN;
    scheduling_info->slot = incoming->Slot;
    memset(scheduling_info->prb_symbol_map, 0, sizeof(scheduling_info->prb_symbol_map));

    // Loop through each incoming UL_TTI PDU
    for (int j = 0; j < incoming->n_pdus; j++)
    {
        uint16_t rnti;
        uint16_t pnf_index;
        int target_pnf_idx = -1;

        // Determine which PNF(s) should get this PDU
        switch (incoming->pdus_list[j].pdu_type)
        {
        case NFAPI_NR_UL_CONFIG_PRACH_PDU_TYPE:
            // PRACH goes to first PNF only
            target_pnf_idx = 0;
            break;

        case NFAPI_NR_UL_CONFIG_PUCCH_PDU_TYPE:
            // PUCCH goes to first PNF only (control channel)
            target_pnf_idx = 0;
            break;

        case NFAPI_NR_UL_CONFIG_SRS_PDU_TYPE:
            // UE-specific SRS - look up which PNF handles this RNTI
            rnti = incoming->pdus_list[j].srs_pdu.rnti;
#ifdef STAGED_HARQ_UPLINK
            pnf_index = rnti_map->ue_rnti_to_pnf_curr[rnti][0]; // SRS not per HARQ process
#else
            pnf_index = rnti_map->ue_rnti_to_pnf_curr[rnti];
#endif

            // Find the index in pnf_list that matches this PNF
            for (int i = 0; i < num_pnfs; i++)
            {
                if (pnf_list[i] == pnf_index)
                {
                    target_pnf_idx = i;
                    break;
                }
            }
            break;

        case NFAPI_NR_UL_CONFIG_PUSCH_PDU_TYPE:
            // UE-specific PUSCH - look up which PNF handles this RNTI
            rnti = incoming->pdus_list[j].pusch_pdu.rnti;
#ifdef STAGED_HARQ_UPLINK
            // staged mapping per HARQ process; promote on NDI
            uint8_t harq_pid = incoming->pdus_list[j].pusch_pdu.pusch_data.harq_process_id;

            if (rnti_map->ue_rnti_to_pnf_curr[rnti][harq_pid] != rnti_map->ue_rnti_to_pnf_next[rnti][harq_pid] && incoming->pdus_list[j].pusch_pdu.pusch_data.new_data_indicator == 1)
            {
                rnti_map->ue_rnti_to_pnf_curr[rnti][harq_pid] = rnti_map->ue_rnti_to_pnf_next[rnti][harq_pid];
                log_info("Promoted UL RNTI 0x%04x to PNF %d for HARQ process %d on NDI", rnti, rnti_map->ue_rnti_to_pnf_curr[rnti][harq_pid], harq_pid);
            }
            pnf_index = rnti_map->ue_rnti_to_pnf_curr[rnti][harq_pid];
#else
            pnf_index = rnti_map->ue_rnti_to_pnf_curr[rnti];
#endif

            // Find the index in pnf_list that matches this PNF
            for (int i = 0; i < num_pnfs; i++)
            {
                if (pnf_list[i] == pnf_index)
                {
                    target_pnf_idx = i;
                    break;
                }
            }
            break;
        default:
            log_warn("Unsupported PDU type %d in UL_TTI.request", incoming->pdus_list[j].pdu_type);
            return -1;
        }

        // Skip if no valid target found or PNF is disabled
        if (target_pnf_idx < 0 || pnf_list[target_pnf_idx] == -1)
        {
            continue;
        }

        // Add PDU to the target PNF's outgoing message
        nfapi_nr_ul_tti_request_t *out_msg = (nfapi_nr_ul_tti_request_t *)(outgoing + target_pnf_idx);
        uint16_t out_pdu_idx = out_msg->n_pdus;

        out_msg->pdus_list[out_pdu_idx].pdu_type = incoming->pdus_list[j].pdu_type;
        out_msg->pdus_list[out_pdu_idx].pdu_size = incoming->pdus_list[j].pdu_size;

        // Copy the appropriate PDU based on type
        switch (incoming->pdus_list[j].pdu_type)
        {
        case NFAPI_NR_UL_CONFIG_PRACH_PDU_TYPE:
            memcpy(&out_msg->pdus_list[out_pdu_idx].prach_pdu,
                   &incoming->pdus_list[j].prach_pdu,
                   sizeof(nfapi_nr_prach_pdu_t));
            out_msg->rach_present = incoming->rach_present;
            break;

        case NFAPI_NR_UL_CONFIG_PUCCH_PDU_TYPE:
            memcpy(&out_msg->pdus_list[out_pdu_idx].pucch_pdu,
                   &incoming->pdus_list[j].pucch_pdu,
                   sizeof(nfapi_nr_pucch_pdu_t));
            out_msg->n_ulcch++;
            break;

        case NFAPI_NR_UL_CONFIG_SRS_PDU_TYPE:
            memcpy(&out_msg->pdus_list[out_pdu_idx].srs_pdu,
                   &incoming->pdus_list[j].srs_pdu,
                   sizeof(nfapi_nr_srs_pdu_t));
            break;

        case NFAPI_NR_UL_CONFIG_PUSCH_PDU_TYPE:
            memcpy(&out_msg->pdus_list[out_pdu_idx].pusch_pdu,
                   &incoming->pdus_list[j].pusch_pdu,
                   sizeof(nfapi_nr_pusch_pdu_t));
            out_msg->n_ulsch++;
            break;
        }

        out_msg->n_pdus++;
        outgoing_mask[target_pnf_idx] = true;
    }

    // Log summary for each PNF
    for (int i = 0; i < num_pnfs; i++)
    {
        if (outgoing_mask[i])
        {
            nfapi_nr_ul_tti_request_t *out_msg = (nfapi_nr_ul_tti_request_t *)(outgoing + i);
            log_debug("Outgoing UL_TTI.request for PNF %d has %d PDUs (ULSCH: %d, ULCCH: %d)",
                      pnf_list[i], out_msg->n_pdus, out_msg->n_ulsch, out_msg->n_ulcch);
        }
    }

    return 0;
}

int process_nfapi_ul_dci_request(const nfapi_nr_ul_dci_request_t *incoming, nfapi_nr_ul_dci_request_t *outgoing, int num_pnfs, bool *outgoing_mask)
{
    for (int i = 0; i < num_pnfs; i++)
    {
        if (i == 0)
        {
            memcpy((void *)(outgoing + i), (void *)incoming, sizeof(nfapi_nr_ul_dci_request_t));
            outgoing_mask[i] = true;
        }
        else
        {
            memset((void *)(outgoing + i), 0, sizeof(nfapi_nr_ul_dci_request_t));
            outgoing_mask[i] = false;
        }
    }
    return 0;
}
#else
static uint8_t dl_scratch_buffer[MAX_NUM_PNF][MAX_MSG_SIZE];

static void __copy_dl_tti_beamforming(const nfapi_nr_tx_precoding_and_beamforming_t *src,
                                    nfapi_nr_tx_precoding_and_beamforming_t *dst)
{
  dst->num_prgs = src->num_prgs;
  dst->prg_size = src->prg_size;
  dst->dig_bf_interfaces = src->dig_bf_interfaces;
  for (int prg = 0; prg < dst->num_prgs; ++prg) {
    dst->prgs_list[prg].pm_idx = src->prgs_list[prg].pm_idx;
    for (int dbf_if = 0; dbf_if < dst->dig_bf_interfaces; ++dbf_if) {
      dst->prgs_list[prg].dig_bf_interface_list[dbf_if].beam_idx = src->prgs_list[prg].dig_bf_interface_list[dbf_if].beam_idx;
    }
  }
}

static void __copy_dl_tti_request_pdcch_pdu(const nfapi_nr_dl_tti_pdcch_pdu_rel15_t *src, nfapi_nr_dl_tti_pdcch_pdu_rel15_t *dst)
{
  dst->BWPSize = src->BWPSize;
  dst->BWPStart = src->BWPStart;
  dst->SubcarrierSpacing = src->SubcarrierSpacing;
  dst->CyclicPrefix = src->CyclicPrefix;
  dst->StartSymbolIndex = src->StartSymbolIndex;
  dst->DurationSymbols = src->DurationSymbols;
  for (int fdr_idx = 0; fdr_idx < 6; ++fdr_idx) {
    dst->FreqDomainResource[fdr_idx] = src->FreqDomainResource[fdr_idx];
  }
  dst->CceRegMappingType = src->CceRegMappingType;
  dst->RegBundleSize = src->RegBundleSize;
  dst->InterleaverSize = src->InterleaverSize;
  dst->CoreSetType = src->CoreSetType;
  dst->ShiftIndex = src->ShiftIndex;
  dst->precoderGranularity = src->precoderGranularity;
  dst->numDlDci = src->numDlDci;
  for (int dl_dci = 0; dl_dci < dst->numDlDci; ++dl_dci) {
    nfapi_nr_dl_dci_pdu_t *dst_dci_pdu = &dst->dci_pdu[dl_dci];
    const nfapi_nr_dl_dci_pdu_t *src_dci_pdu = &src->dci_pdu[dl_dci];
    dst_dci_pdu->RNTI = src_dci_pdu->RNTI;
    dst_dci_pdu->ScramblingId = src_dci_pdu->ScramblingId;
    dst_dci_pdu->ScramblingRNTI = src_dci_pdu->ScramblingRNTI;
    dst_dci_pdu->CceIndex = src_dci_pdu->CceIndex;
    dst_dci_pdu->AggregationLevel = src_dci_pdu->AggregationLevel;
    __copy_dl_tti_beamforming(&src_dci_pdu->precodingAndBeamforming, &dst_dci_pdu->precodingAndBeamforming);
    dst_dci_pdu->beta_PDCCH_1_0 = src_dci_pdu->beta_PDCCH_1_0;
    dst_dci_pdu->powerControlOffsetSS = src_dci_pdu->powerControlOffsetSS;
    dst_dci_pdu->PayloadSizeBits = src_dci_pdu->PayloadSizeBits;
    for (int i = 0; i < 8; ++i) {
      dst_dci_pdu->Payload[i] = src_dci_pdu->Payload[i];
    }
  }
}

static void __copy_dl_tti_request_pdsch_pdu(const nfapi_nr_dl_tti_pdsch_pdu_rel15_t *src, nfapi_nr_dl_tti_pdsch_pdu_rel15_t *dst)
{
  dst->pduBitmap = src->pduBitmap;
  dst->rnti = src->rnti;
  dst->pduIndex = src->pduIndex;
  dst->BWPSize = src->BWPSize;
  dst->BWPStart = src->BWPStart;
  dst->SubcarrierSpacing = src->SubcarrierSpacing;
  dst->CyclicPrefix = src->CyclicPrefix;
  dst->NrOfCodewords = src->NrOfCodewords;
  for (int cw = 0; cw < dst->NrOfCodewords; ++cw) {
    dst->targetCodeRate[cw] = src->targetCodeRate[cw];
    dst->qamModOrder[cw] = src->qamModOrder[cw];
    dst->mcsIndex[cw] = src->mcsIndex[cw];
    dst->mcsTable[cw] = src->mcsTable[cw];
    dst->rvIndex[cw] = src->rvIndex[cw];
    dst->TBSize[cw] = src->TBSize[cw];
  }
  dst->dataScramblingId = src->dataScramblingId;
  dst->nrOfLayers = src->nrOfLayers;
  dst->transmissionScheme = src->transmissionScheme;
  dst->refPoint = src->refPoint;
  dst->dlDmrsSymbPos = src->dlDmrsSymbPos;
  dst->dmrsConfigType = src->dmrsConfigType;
  dst->dlDmrsScramblingId = src->dlDmrsScramblingId;
  dst->SCID = src->SCID;
  dst->numDmrsCdmGrpsNoData = src->numDmrsCdmGrpsNoData;
  dst->dmrsPorts = src->dmrsPorts;
  dst->resourceAlloc = src->resourceAlloc;
  for (int i = 0; i < 36; ++i) {
    dst->rbBitmap[i] = src->rbBitmap[i];
  }
  dst->rbStart = src->rbStart;
  dst->rbSize = src->rbSize;
  dst->VRBtoPRBMapping = src->VRBtoPRBMapping;
  dst->StartSymbolIndex = src->StartSymbolIndex;
  dst->NrOfSymbols = src->NrOfSymbols;
  dst->PTRSPortIndex = src->PTRSPortIndex;
  dst->PTRSTimeDensity = src->PTRSTimeDensity;
  dst->PTRSFreqDensity = src->PTRSFreqDensity;
  dst->PTRSReOffset = src->PTRSReOffset;
  dst->nEpreRatioOfPDSCHToPTRS = src->nEpreRatioOfPDSCHToPTRS;
  __copy_dl_tti_beamforming(&src->precodingAndBeamforming, &dst->precodingAndBeamforming);
  dst->powerControlOffset = src->powerControlOffset;
  dst->powerControlOffsetSS = src->powerControlOffsetSS;
  dst->isLastCbPresent = src->isLastCbPresent;
  dst->isInlineTbCrc = src->isInlineTbCrc;
  dst->dlTbCrc = src->dlTbCrc;
  dst->maintenance_parms_v3.ldpcBaseGraph = src->maintenance_parms_v3.ldpcBaseGraph;
  dst->maintenance_parms_v3.tbSizeLbrmBytes = src->maintenance_parms_v3.tbSizeLbrmBytes;
}

static void __copy_dl_tti_request_csi_rs_pdu(const nfapi_nr_dl_tti_csi_rs_pdu_rel15_t *src, nfapi_nr_dl_tti_csi_rs_pdu_rel15_t *dst)
{
  dst->bwp_size = src->bwp_size;
  dst->bwp_start = src->bwp_start;
  dst->subcarrier_spacing = src->subcarrier_spacing;
  dst->cyclic_prefix = src->cyclic_prefix;
  dst->start_rb = src->start_rb;
  dst->nr_of_rbs = src->nr_of_rbs;
  dst->csi_type = src->csi_type;
  dst->row = src->row;
  dst->freq_domain = src->freq_domain;
  dst->symb_l0 = src->symb_l0;
  dst->symb_l1 = src->symb_l1;
  dst->cdm_type = src->cdm_type;
  dst->freq_density = src->freq_density;
  dst->scramb_id = src->scramb_id;
  dst->power_control_offset = src->power_control_offset;
  dst->power_control_offset_ss = src->power_control_offset_ss;
  __copy_dl_tti_beamforming(&src->precodingAndBeamforming, &dst->precodingAndBeamforming);
}

static void __copy_dl_tti_request_ssb_pdu(const nfapi_nr_dl_tti_ssb_pdu_rel15_t *src, nfapi_nr_dl_tti_ssb_pdu_rel15_t *dst)
{
  dst->PhysCellId = src->PhysCellId;
  dst->BetaPss = src->BetaPss;
  dst->SsbBlockIndex = src->SsbBlockIndex;
  dst->SsbSubcarrierOffset = src->SsbSubcarrierOffset;
  dst->ssbOffsetPointA = src->ssbOffsetPointA;
  dst->bchPayloadFlag = src->bchPayloadFlag;
  dst->bchPayload = src->bchPayload;
  dst->ssbRsrp = src->ssbRsrp;
  __copy_dl_tti_beamforming(&src->precoding_and_beamforming, &dst->precoding_and_beamforming);
}

int handle_nfapi_dl_tti_request(void *input_buffer, int input_size, int *pnf_list, int num_pnfs, int primary_pnf, downlink_rnti_to_pnf_mapping_t *rnti_map, uint8_t **output_buffers, int *output_sizes)
{
    nfapi_nr_dl_tti_request_t incoming_dl_tti_req;
    nfapi_nr_p7_message_unpack(input_buffer, input_size, &incoming_dl_tti_req, sizeof(nfapi_nr_dl_tti_request_t), 0);
    log_debug("DL_TTI.request size: %d", input_size);
    log_debug("DL_TTI.request SFN: %d, Slot: %d, NumPdus: %d, NumGroup: %d", incoming_dl_tti_req.SFN, incoming_dl_tti_req.Slot, incoming_dl_tti_req.dl_tti_request_body.nPDUs, incoming_dl_tti_req.dl_tti_request_body.nGroup);

    if(incoming_dl_tti_req.dl_tti_request_body.nGroup > 0)
    {
        log_warn("DL_TTI.request with nGroup > 0 not supported");
    }
    // initialize outgoing messages
    nfapi_nr_dl_tti_request_t outgoing_dl_tti_req[MAX_NUM_PNF];
    for (int i = 0; i < num_pnfs; i++)
    {
        memcpy(&outgoing_dl_tti_req[i].header, &incoming_dl_tti_req.header, sizeof(nfapi_nr_p7_message_header_t));
        outgoing_dl_tti_req[i].SFN = incoming_dl_tti_req.SFN;
        outgoing_dl_tti_req[i].Slot = incoming_dl_tti_req.Slot;
        outgoing_dl_tti_req[i].dl_tti_request_body.nPDUs = 0;
        outgoing_dl_tti_req[i].dl_tti_request_body.nGroup = 0;
    }

    // loop through each incoming PDU and distribute to appropriate PNF
    for (size_t j = 0; j < incoming_dl_tti_req.dl_tti_request_body.nPDUs; j++)
    {
        uint16_t rnti;
        uint16_t pnf_index;
        int target_pnf_idx = -1;

        // determine which PNF(s) should get this PDU
        switch (incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[j].PDUType)
        {
        case NFAPI_NR_DL_TTI_SSB_PDU_TYPE:
        case NFAPI_NR_DL_TTI_PDCCH_PDU_TYPE:
        case NFAPI_NR_DL_TTI_CSI_RS_PDU_TYPE:
            // broadcast PDUs go to the primary PNF only
            target_pnf_idx = primary_pnf;
            break;

        case NFAPI_NR_DL_TTI_PDSCH_PDU_TYPE:
            // UE-specific PDU - look up which PNF handles this RNTI
            rnti = incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[j].pdsch_pdu.pdsch_pdu_rel15.rnti;
            pnf_index = rnti_map->ue_rnti_to_pnf_curr[rnti];

            // find the index in pnf_list that matches this PNF
            for (int i = 0; i < num_pnfs; i++)
            {
                if (pnf_list[i] == pnf_index)
                {
                    target_pnf_idx = i;
                    break;
                }
            }
            break;

        default:
            log_warn("Unknown PDU type %d", incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[j].PDUType);
            return -1;
        }

        // add PDU to the target PNF's outgoing message
        uint8_t pdu_index = outgoing_dl_tti_req[target_pnf_idx].dl_tti_request_body.nPDUs;
        switch (incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[j].PDUType)
        {
        case NFAPI_NR_DL_TTI_SSB_PDU_TYPE:
            outgoing_dl_tti_req[target_pnf_idx].dl_tti_request_body.dl_tti_pdu_list[pdu_index].PDUType = NFAPI_NR_DL_TTI_SSB_PDU_TYPE;
            outgoing_dl_tti_req[target_pnf_idx].dl_tti_request_body.dl_tti_pdu_list[pdu_index].PDUSize = incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[j].PDUSize;
            __copy_dl_tti_request_ssb_pdu(&incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[j].ssb_pdu.ssb_pdu_rel15, &outgoing_dl_tti_req[target_pnf_idx].dl_tti_request_body.dl_tti_pdu_list[pdu_index].ssb_pdu.ssb_pdu_rel15);
            break;
        case NFAPI_NR_DL_TTI_CSI_RS_PDU_TYPE:
            outgoing_dl_tti_req[target_pnf_idx].dl_tti_request_body.dl_tti_pdu_list[pdu_index].PDUType = NFAPI_NR_DL_TTI_CSI_RS_PDU_TYPE;
            outgoing_dl_tti_req[target_pnf_idx].dl_tti_request_body.dl_tti_pdu_list[pdu_index].PDUSize = incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[j].PDUSize;
            __copy_dl_tti_request_csi_rs_pdu(&incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[j].csi_rs_pdu.csi_rs_pdu_rel15, &outgoing_dl_tti_req[target_pnf_idx].dl_tti_request_body.dl_tti_pdu_list[pdu_index].csi_rs_pdu.csi_rs_pdu_rel15);
            break;
        case NFAPI_NR_DL_TTI_PDSCH_PDU_TYPE:
            outgoing_dl_tti_req[target_pnf_idx].dl_tti_request_body.dl_tti_pdu_list[pdu_index].PDUType = NFAPI_NR_DL_TTI_PDSCH_PDU_TYPE;
            outgoing_dl_tti_req[target_pnf_idx].dl_tti_request_body.dl_tti_pdu_list[pdu_index].PDUSize = incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[j].PDUSize;
            __copy_dl_tti_request_pdsch_pdu(&incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[j].pdsch_pdu.pdsch_pdu_rel15, &outgoing_dl_tti_req[target_pnf_idx].dl_tti_request_body.dl_tti_pdu_list[pdu_index].pdsch_pdu.pdsch_pdu_rel15);
            break;
        case NFAPI_NR_DL_TTI_PDCCH_PDU_TYPE:
            outgoing_dl_tti_req[target_pnf_idx].dl_tti_request_body.dl_tti_pdu_list[pdu_index].PDUType = NFAPI_NR_DL_TTI_PDCCH_PDU_TYPE;
            outgoing_dl_tti_req[target_pnf_idx].dl_tti_request_body.dl_tti_pdu_list[pdu_index].PDUSize = incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[j].PDUSize;
            __copy_dl_tti_request_pdcch_pdu(&incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[j].pdcch_pdu.pdcch_pdu_rel15, &outgoing_dl_tti_req[target_pnf_idx].dl_tti_request_body.dl_tti_pdu_list[pdu_index].pdcch_pdu.pdcch_pdu_rel15);
            break;
        default:
            log_warn("Unknown PDU type %d", incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[j].PDUType);
            return -1;
        }
        outgoing_dl_tti_req[target_pnf_idx].dl_tti_request_body.nPDUs++;
    }

    // DEBUG:
    // DO SOME CHECKS ON THE OUTGOING MESSAGE's PDU_LIST AND SEE IF THEY OVERLAP IN RESOURCE USAGE
    // for (int i = 0; i < num_pnfs; i++)
    // {
    //     // ONLY PRIMARY PNF SHOULD HAVE SSB/ PDCCH/ CSI-RS
    //     if (i != primary_pnf && outgoing_dl_tti_req[i].dl_tti_request_body.nPDUs > 0)
    //     {
    //         for (int j = 0; j < outgoing_dl_tti_req[i].dl_tti_request_body.nPDUs; j++)
    //         {
    //             if (outgoing_dl_tti_req[i].dl_tti_request_body.dl_tti_pdu_list[j].PDUType == NFAPI_NR_DL_TTI_SSB_PDU_TYPE ||
    //                 outgoing_dl_tti_req[i].dl_tti_request_body.dl_tti_pdu_list[j].PDUType == NFAPI_NR_DL_TTI_PDCCH_PDU_TYPE ||
    //                 outgoing_dl_tti_req[i].dl_tti_request_body.dl_tti_pdu_list[j].PDUType == NFAPI_NR_DL_TTI_CSI_RS_PDU_TYPE)
    //             {
    //                 log_error("PNF %d has SSB/PDCCH/CSI-RS PDU but is not the primary PNF", pnf_list[i]);
    //                 return -1;
    //             }
    //         }
    //     }
    //     // ONLY PRIMARY PNF SHOULD HAVE PDSCH FOR 0xFFFF RNTI
    //     for (int j = 0; j < outgoing_dl_tti_req[i].dl_tti_request_body.nPDUs; j++)
    //     {
    //         if (outgoing_dl_tti_req[i].dl_tti_request_body.dl_tti_pdu_list[j].PDUType == NFAPI_NR_DL_TTI_PDSCH_PDU_TYPE)
    //         {
    //             uint16_t rnti = outgoing_dl_tti_req[i].dl_tti_request_body.dl_tti_pdu_list[j].pdsch_pdu.pdsch_pdu_rel15.rnti;
    //             if (rnti == 0xFFFF && i != primary_pnf)
    //             {
    //                 log_error("PNF %d has PDSCH PDU for RNTI 0xFFFF but is not the primary PNF", pnf_list[i]);
    //                 return -1;
    //             }   
    //         }
    //     }
    // }   
    // DEBUG: THE MESSAGE LENGTH SUM OF ALL PNFS SHOULD EQUAL THE ORIGINAL MESSAGE LENGTH
    // int total_outgoing_pdus = 0;
    // for (int i = 0; i < num_pnfs; i++)
    // {
    //     total_outgoing_pdus += outgoing_dl_tti_req[i].dl_tti_request_body.nPDUs;
    // }
    // if (total_outgoing_pdus != incoming_dl_tti_req.dl_tti_request_body.nPDUs)
    // {
    //     log_error("Total outgoing PDUs %d does not match incoming PDUs %d", total_outgoing_pdus, incoming_dl_tti_req.dl_tti_request_body.nPDUs);
    //     return -1;
    // }

    // DEBUG: ENSURE THAT THE PDSCH PDUS ARE NOT OVERLAPPING IN RESOURCE USAGE ACROSS PNFS BY THEIR RBSTART AND RBSIZE
    // scan through the incoming PDUs and compare each PDSCH PDU's RB allocation 
    // for(int i=0; i <incoming_dl_tti_req.dl_tti_request_body.nPDUs; i++)
    // {
    //     if(incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[i].PDUType == NFAPI_NR_DL_TTI_PDSCH_PDU_TYPE)
    //     {
    //         uint16_t rnti = incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[i].pdsch_pdu.pdsch_pdu_rel15.rnti;
    //         uint16_t bwp_start_1 = incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[i].pdsch_pdu.pdsch_pdu_rel15.BWPStart;
    //         uint16_t rb_start = incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[i].pdsch_pdu.pdsch_pdu_rel15.rbStart;
    //         uint16_t rb_size = incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[i].pdsch_pdu.pdsch_pdu_rel15.rbSize;
    //         // check against all other PDSCH PDUs
    //         for(int j=i+1; j <incoming_dl_tti_req.dl_tti_request_body.nPDUs; j++)
    //         {
    //             if(incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[j].PDUType == NFAPI_NR_DL_TTI_PDSCH_PDU_TYPE)
    //             {
    //                 uint16_t rnti_2 = incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[j].pdsch_pdu.pdsch_pdu_rel15.rnti;
    //                 uint16_t bwp_start_2 = incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[j].pdsch_pdu.pdsch_pdu_rel15.BWPStart;
    //                 uint16_t rb_start_2 = incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[j].pdsch_pdu.pdsch_pdu_rel15.rbStart;
    //                 uint16_t rb_size_2 = incoming_dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[j].pdsch_pdu.pdsch_pdu_rel15.rbSize;
    //                 if(!((bwp_start_1 + rb_start + (rb_size - 1) < bwp_start_2 + rb_start_2) || (bwp_start_2 + rb_start_2 + (rb_size_2 -1) < bwp_start_1 + rb_start)))
    //                 {
    //                     log_error("Detected overlapping PDSCH RB resources (RNTI 0x%04x BWPStart: %d RBStart: %d, RBSize: %d; RNTI 0x%04x  BWPStart: %d RBStart: %d, RBSize: %d)",
    //                                 rnti, bwp_start_1, rb_start, rb_size,
    //                                 rnti_2, bwp_start_2, rb_start_2, rb_size_2);
    //                 }
    //             }
    //         }
    //     }
    // }

    // for (int i = 0; i < num_pnfs; i++)
    // {
    //     for (int j = 0; j < outgoing_dl_tti_req[i].dl_tti_request_body.nPDUs; j++)
    //     {
    //         if (outgoing_dl_tti_req[i].dl_tti_request_body.dl_tti_pdu_list[j].PDUType == NFAPI_NR_DL_TTI_PDSCH_PDU_TYPE)
    //         {
    //             uint16_t bwp_start_1 = outgoing_dl_tti_req[i].dl_tti_request_body.dl_tti_pdu_list[j].pdsch_pdu.pdsch_pdu_rel15.BWPStart;
    //             uint16_t rb_start_1 = outgoing_dl_tti_req[i].dl_tti_request_body.dl_tti_pdu_list[j].pdsch_pdu.pdsch_pdu_rel15.rbStart;
    //             uint16_t rb_size_1 = outgoing_dl_tti_req[i].dl_tti_request_body.dl_tti_pdu_list[j].pdsch_pdu.pdsch_pdu_rel15.rbSize;
    //             for (int k = i + 1; k < num_pnfs; k++)
    //             {
    //                 for (int l = 0; l < outgoing_dl_tti_req[k].dl_tti_request_body.nPDUs; l++)
    //                 {
    //                     if (outgoing_dl_tti_req[k].dl_tti_request_body.dl_tti_pdu_list[l].PDUType == NFAPI_NR_DL_TTI_PDSCH_PDU_TYPE)
    //                     {
    //                         uint16_t bwp_start_2 = outgoing_dl_tti_req[k].dl_tti_request_body.dl_tti_pdu_list[l].pdsch_pdu.pdsch_pdu_rel15.BWPStart;
    //                         uint16_t rb_start_2 = outgoing_dl_tti_req[k].dl_tti_request_body.dl_tti_pdu_list[l].pdsch_pdu.pdsch_pdu_rel15.rbStart;
    //                         uint16_t rb_size_2 = outgoing_dl_tti_req[k].dl_tti_request_body.dl_tti_pdu_list[l].pdsch_pdu.pdsch_pdu_rel15.rbSize;
    //                         if (!((bwp_start_1 + rb_start_1 + rb_size_1 <= bwp_start_2) || (bwp_start_2 + rb_start_2 + rb_size_2 <= bwp_start_1)))
    //                         {
    //                             log_error("PNF %d and PNF %d have overlapping PDSCH RB resources (PNF %d BWPStart: %d RBStart: %d, RBSize: %d; PNF %d BWPStart: %d RBStart: %d, RBSize: %d)",
    //                                       pnf_list[i], pnf_list[k],
    //                                       pnf_list[i], bwp_start_1, rb_start_1, rb_size_1,
    //                                       pnf_list[k], bwp_start_2, rb_start_2, rb_size_2);
    //                         }
    //                     }
    //                 }
    //             }
    //         }
    //     }
    // }

    // finalize outgoing buffers
    for (int i = 0; i < num_pnfs; i++)
    {
        // if no PDUs, skip
        if (outgoing_dl_tti_req[i].dl_tti_request_body.nPDUs == 0)
        {
            output_buffers[i] = NULL;
            output_sizes[i] = 0;
            continue;
        }
        // pack the outgoing message
        int packed_size = nfapi_nr_p7_message_pack(&outgoing_dl_tti_req[i], dl_scratch_buffer[i], 65536, 0);
        if (packed_size < 0)
        {
            log_error("Failed to pack DL_TTI.request for PNF %d", pnf_list[i]);
            return -1;
        }

        // update the checksum
        nfapi_nr_p7_update_checksum(dl_scratch_buffer[i], packed_size);

        // allocate output buffer and copy
        output_buffers[i] = dl_scratch_buffer[i];
        output_sizes[i] = packed_size;
        log_debug("DL_TTI.request for PNF %d, Size: %d", pnf_list[i], packed_size);
    }

    return 0;
}

int handle_nfapi_tx_data_request(void *input_buffer, int input_size, int *pnf_list, int num_pnfs, int primary_pnf, uint8_t **output_buffers, int *output_sizes)
{
    uint16_t sfn;
    uint16_t slot;
    peek_nr_nfapi_p7_sfn_slot(input_buffer, input_size, &sfn, &slot);

    log_debug("TX_DATA.request size: %d", input_size);
    log_debug("TX_DATA.request SFN: %d, Slot: %d", sfn, slot);

    // we simply mirror the incoming message to all PNFs as we have already mapped PDUs in DL_TTI processing
    for (int i = 0; i < num_pnfs; i++)
    {
        // pack the outgoing message
        output_buffers[i] = input_buffer;
        output_sizes[i] = input_size;
        log_debug("TX_DATA.request for PNF %d, Size: %d", pnf_list[i], input_size);
    }

    return 0;
}

#ifdef UL_TTI_LEGACY
int handle_nfapi_ul_tti_request(void *input_buffer, int input_size, int *pnf_list, int num_pnfs, int primary_pnf, uplink_rnti_to_pnf_mapping_t *rnti_map, uplink_fapi_info_t *fapi_info, uint8_t **output_buffers, int *output_sizes)
{
    nfapi_nr_ul_tti_request_t incoming_ul_tti_req;
    nfapi_nr_p7_message_unpack(input_buffer, input_size, &incoming_ul_tti_req, sizeof(nfapi_nr_ul_tti_request_t), 0);
    log_debug("UL_TTI.request size: %d", input_size);
    log_debug("UL_TTI.request SFN: %d, Slot: %d, NumPdus: %d", incoming_ul_tti_req.SFN, incoming_ul_tti_req.Slot, incoming_ul_tti_req.n_pdus);

    nfapi_nr_ul_tti_request_t outgoing_ul_tti_req[MAX_NUM_PNF];
    // initialize outgoing messages
    for (int i = 0; i < num_pnfs; i++)
    {
        memcpy(&outgoing_ul_tti_req[i].header, &incoming_ul_tti_req.header, sizeof(nfapi_nr_p7_message_header_t));
        outgoing_ul_tti_req[i].SFN = incoming_ul_tti_req.SFN;
        outgoing_ul_tti_req[i].Slot = incoming_ul_tti_req.Slot;
        outgoing_ul_tti_req[i].n_pdus = 0;
        outgoing_ul_tti_req[i].rach_present = 0;
        outgoing_ul_tti_req[i].n_ulsch = 0;
        outgoing_ul_tti_req[i].n_ulcch = 0;
        outgoing_ul_tti_req[i].n_group = 0;
    }

    // dump the fapi_info for this SFN/Slot
    // they all should be zero at this point
    log_debug("UL_TTI.request SFN %d Slot %d: Initial FAPI info - expected RX_DATA: %d, expected CRC: %d, expected UCI: %d",
              incoming_ul_tti_req.SFN, incoming_ul_tti_req.Slot,
              fapi_info[incoming_ul_tti_req.SFN * MAX_SLOT + incoming_ul_tti_req.Slot].rx_ind_expected,
              fapi_info[incoming_ul_tti_req.SFN * MAX_SLOT + incoming_ul_tti_req.Slot].crc_ind_expected,
              fapi_info[incoming_ul_tti_req.SFN * MAX_SLOT + incoming_ul_tti_req.Slot].uci_ind_expected);
    if (fapi_info[incoming_ul_tti_req.SFN * MAX_SLOT + incoming_ul_tti_req.Slot].rx_ind_expected != 0 ||
        fapi_info[incoming_ul_tti_req.SFN * MAX_SLOT + incoming_ul_tti_req.Slot].crc_ind_expected != 0 ||
        fapi_info[incoming_ul_tti_req.SFN * MAX_SLOT + incoming_ul_tti_req.Slot].uci_ind_expected != 0)
    {
        log_warn("UL_TTI.request SFN %d Slot %d: FAPI info not zero at start of processing, resetting!", incoming_ul_tti_req.SFN, incoming_ul_tti_req.Slot);
    }
    // Always reset to prevent stale counts from accumulating across SFN cycles
    {
        uplink_fapi_info_t *fapi_entry = &fapi_info[incoming_ul_tti_req.SFN * MAX_SLOT + incoming_ul_tti_req.Slot];
        for (int b = 0; b < num_pnfs; b++)
        {
            if (fapi_entry->rx_buf[b] != NULL) { free_rx_data_indication(fapi_entry->rx_buf[b]); free(fapi_entry->rx_buf[b]); fapi_entry->rx_buf[b] = NULL; }
            if (fapi_entry->crc_buf[b] != NULL) { free_crc_indication(fapi_entry->crc_buf[b]); free(fapi_entry->crc_buf[b]); fapi_entry->crc_buf[b] = NULL; }
            if (fapi_entry->uci_buf[b] != NULL) { free_uci_indication(fapi_entry->uci_buf[b]); free(fapi_entry->uci_buf[b]); fapi_entry->uci_buf[b] = NULL; }
            if (fapi_entry->srs_buf[b] != NULL) { free_srs_indication(fapi_entry->srs_buf[b]); free(fapi_entry->srs_buf[b]); fapi_entry->srs_buf[b] = NULL; }
        }
        fapi_entry->rx_ind_count = 0;  fapi_entry->rx_ind_expected = 0;
        fapi_entry->crc_ind_count = 0; fapi_entry->crc_ind_expected = 0;
        fapi_entry->uci_ind_count = 0; fapi_entry->uci_ind_expected = 0;
        fapi_entry->srs_ind_count = 0; fapi_entry->srs_ind_expected = 0;
    }

    // loop through each incoming PDU and distribute to appropriate PNF
    for (int j = 0; j < incoming_ul_tti_req.n_pdus; j++)
    {
        uint16_t rnti;
        uint16_t pnf_index;
        int target_pnf_idx = -1;

        // determine which PNF(s) should get this PDU
        switch (incoming_ul_tti_req.pdus_list[j].pdu_type)
        {
        case NFAPI_NR_UL_CONFIG_PRACH_PDU_TYPE:
            // PRACH goes to primary PNF only
            target_pnf_idx = primary_pnf;
            outgoing_ul_tti_req[target_pnf_idx].rach_present = 1;
            break;
        case NFAPI_NR_UL_CONFIG_PUCCH_PDU_TYPE:
            // PUCCH goes to primary PNF only (control channel)
            // target_pnf_idx = primary_pnf;
            rnti = incoming_ul_tti_req.pdus_list[j].pucch_pdu.rnti;
#ifdef STAGED_HARQ_UPLINK
            pnf_index = rnti_map->ue_rnti_to_pnf_curr[rnti][0];
#else
            pnf_index = rnti_map->ue_rnti_to_pnf_curr[rnti];
#endif
            // find the index in pnf_list that matches this PNF
            for (int i = 0; i < num_pnfs; i++)
            {
                if (pnf_list[i] == pnf_index)
                {
                    target_pnf_idx = i;
                    break;
                }
            }
            outgoing_ul_tti_req[target_pnf_idx].n_ulcch++;
            if (outgoing_ul_tti_req[target_pnf_idx].n_ulcch == 1)
            {
                // update expected count for UCI.indication processing
                fapi_info[incoming_ul_tti_req.SFN * MAX_SLOT + incoming_ul_tti_req.Slot].uci_ind_expected++;
                log_debug("UL_TTI.request SFN %d Slot %d: Incremented expected UCI count to %d",
                          incoming_ul_tti_req.SFN, incoming_ul_tti_req.Slot,
                          fapi_info[incoming_ul_tti_req.SFN * MAX_SLOT + incoming_ul_tti_req.Slot].uci_ind_expected);
            }
            break;
        case NFAPI_NR_UL_CONFIG_SRS_PDU_TYPE:
            // UE-specific SRS - look up which PNF handles this RNTI
            rnti = incoming_ul_tti_req.pdus_list[j].srs_pdu.rnti;
#ifdef STAGED_HARQ_UPLINK
            pnf_index = rnti_map->ue_rnti_to_pnf_curr[rnti][0]; // SRS not per HARQ process
#else
            pnf_index = rnti_map->ue_rnti_to_pnf_curr[rnti];
#endif
            // find the index in pnf_list that matches this PNF
            for (int i = 0; i < num_pnfs; i++)
            {
                if (pnf_list[i] == pnf_index)
                {
                    target_pnf_idx = i;
                    break;
                }
            }
            break;
        case NFAPI_NR_UL_CONFIG_PUSCH_PDU_TYPE:
            // UE-specific PUSCH - look up which PNF handles this RNTI
            rnti = incoming_ul_tti_req.pdus_list[j].pusch_pdu.rnti;
#ifdef STAGED_HARQ_UPLINK
            // staged mapping per HARQ process; promote on NDI
            uint8_t harq_pid = incoming_ul_tti_req.pdus_list[j].pusch_pdu.pusch_data.harq_process_id;
            if (rnti_map->ue_rnti_to_pnf_curr[rnti][harq_pid] != rnti_map->ue_rnti_to_pnf_next[rnti][harq_pid] && incoming_ul_tti_req.pdus_list[j].pusch_pdu.pusch_data.new_data_indicator == 1)
            {
                rnti_map->ue_rnti_to_pnf_curr[rnti][harq_pid] = rnti_map->ue_rnti_to_pnf_next[rnti][harq_pid];
                log_info("Promoted UL RNTI 0x%04x to PNF %d for HARQ process %d on NDI", rnti, rnti_map->ue_rnti_to_pnf_curr[rnti][harq_pid], harq_pid);
            }
            pnf_index = rnti_map->ue_rnti_to_pnf_curr[rnti][harq_pid];
#else
            pnf_index = rnti_map->ue_rnti_to_pnf_curr[rnti];
#endif
            // find the index in pnf_list that matches this PNF
            for (int i = 0; i < num_pnfs; i++)
            {
                if (pnf_list[i] == pnf_index)
                {
                    target_pnf_idx = i;
                    break;
                }
            }
            outgoing_ul_tti_req[target_pnf_idx].n_ulsch++;
            if (outgoing_ul_tti_req[target_pnf_idx].n_ulsch == 1)
            {
                // update expected count for RX_DATA.indication and CRC.indication processing
                fapi_info[incoming_ul_tti_req.SFN * MAX_SLOT + incoming_ul_tti_req.Slot].rx_ind_expected++;
                fapi_info[incoming_ul_tti_req.SFN * MAX_SLOT + incoming_ul_tti_req.Slot].crc_ind_expected++;
                log_debug("UL_TTI.request SFN %d Slot %d: Incremented expected RX/CRC count to %d",
                          incoming_ul_tti_req.SFN, incoming_ul_tti_req.Slot,
                          fapi_info[incoming_ul_tti_req.SFN * MAX_SLOT + incoming_ul_tti_req.Slot].rx_ind_expected);
            }
            break;
        default:
            log_warn("Unsupported PDU type %d in UL_TTI.request", incoming_ul_tti_req.pdus_list[j].pdu_type);
            return -1;
        }

        // add PDU to the target PNF's outgoing message
        uint8_t pdu_index = outgoing_ul_tti_req[target_pnf_idx].n_pdus;
        outgoing_ul_tti_req[target_pnf_idx].pdus_list[pdu_index] = incoming_ul_tti_req.pdus_list[j];
        outgoing_ul_tti_req[target_pnf_idx].n_pdus++;
    }

    // finalize outgoing buffers
    for (int i = 0; i < num_pnfs; i++)
    {
        // if no PDUs, skip
        if (outgoing_ul_tti_req[i].n_pdus == 0)
        {
            output_buffers[i] = NULL;
            output_sizes[i] = 0;
            continue;
        }
        // pack the outgoing message
        int packed_size = nfapi_nr_p7_message_pack(&outgoing_ul_tti_req[i], dl_scratch_buffer[i], 65536, 0);
        if (packed_size < 0)
        {
            log_error("Failed to pack UL_TTI.request for PNF %d", pnf_list[i]);
            return -1;
        }

        // update the checksum
        nfapi_nr_p7_update_checksum(dl_scratch_buffer[i], packed_size);

        // allocate output buffer and copy
        output_buffers[i] = dl_scratch_buffer[i];
        output_sizes[i] = packed_size;
        log_debug("UL_TTI.request for PNF %d, Size: %d", pnf_list[i], packed_size);

        // log number of ULSCH and ULCCH PDUs
        log_debug("UL_TTI.request for PNF %d has %d ULSCH PDUs and %d ULCCH PDUs", pnf_list[i], outgoing_ul_tti_req[i].n_ulsch, outgoing_ul_tti_req[i].n_ulcch);
    }

    return 0;
}
#else
int handle_nfapi_ul_tti_request(void *input_buffer, int input_size, int *pnf_list, int num_pnfs, int primary_pnf, uplink_rnti_to_pnf_mapping_t *rnti_map, uplink_fapi_info_t *fapi_info, uint8_t **output_buffers, int *output_sizes)
{
    nfapi_nr_ul_tti_request_t incoming_ul_tti_req;
    nfapi_nr_p7_message_unpack(input_buffer, input_size, &incoming_ul_tti_req, sizeof(nfapi_nr_ul_tti_request_t), 0);
    log_debug("UL_TTI.request size: %d", input_size);
    log_debug("UL_TTI.request SFN: %d, Slot: %d, NumPdus: %d", incoming_ul_tti_req.SFN, incoming_ul_tti_req.Slot, incoming_ul_tti_req.n_pdus);

    nfapi_nr_ul_tti_request_t outgoing_ul_tti_req[MAX_NUM_PNF];
    // initialize outgoing messages
    for (int i = 0; i < num_pnfs; i++)
    {
        memcpy(&outgoing_ul_tti_req[i].header, &incoming_ul_tti_req.header, sizeof(nfapi_nr_p7_message_header_t));
        outgoing_ul_tti_req[i].SFN = incoming_ul_tti_req.SFN;
        outgoing_ul_tti_req[i].Slot = incoming_ul_tti_req.Slot;
        outgoing_ul_tti_req[i].n_pdus = 0;
        outgoing_ul_tti_req[i].rach_present = 0;
        outgoing_ul_tti_req[i].n_ulsch = 0;
        outgoing_ul_tti_req[i].n_ulcch = 0;
        outgoing_ul_tti_req[i].n_group = 0;
    }

    uint16_t ulcch[MAX_NUM_PNF] = {0};
    uint16_t ulsch[MAX_NUM_PNF] = {0};

    // Reset fapi_info for this SFN/Slot to prevent stale counts from accumulating across SFN cycles
    {
        uplink_fapi_info_t *fapi_entry = &fapi_info[incoming_ul_tti_req.SFN * MAX_SLOT + incoming_ul_tti_req.Slot];
        if (fapi_entry->rx_ind_count != 0 || fapi_entry->crc_ind_count != 0 ||
            fapi_entry->uci_ind_count != 0 || fapi_entry->srs_ind_count != 0 ||
            fapi_entry->rx_ind_expected != 0 || fapi_entry->crc_ind_expected != 0 ||
            fapi_entry->uci_ind_expected != 0 || fapi_entry->srs_ind_expected != 0)
        {
            log_warn("UL_TTI.request V2 SFN %d Slot %d: stale fapi_info detected, resetting", incoming_ul_tti_req.SFN, incoming_ul_tti_req.Slot);
        }
        for (int b = 0; b < num_pnfs; b++)
        {
            if (fapi_entry->rx_buf[b] != NULL) { free_rx_data_indication(fapi_entry->rx_buf[b]); free(fapi_entry->rx_buf[b]); fapi_entry->rx_buf[b] = NULL; }
            if (fapi_entry->crc_buf[b] != NULL) { free_crc_indication(fapi_entry->crc_buf[b]); free(fapi_entry->crc_buf[b]); fapi_entry->crc_buf[b] = NULL; }
            if (fapi_entry->uci_buf[b] != NULL) { free_uci_indication(fapi_entry->uci_buf[b]); free(fapi_entry->uci_buf[b]); fapi_entry->uci_buf[b] = NULL; }
            if (fapi_entry->srs_buf[b] != NULL) { free_srs_indication(fapi_entry->srs_buf[b]); free(fapi_entry->srs_buf[b]); fapi_entry->srs_buf[b] = NULL; }
        }
        fapi_entry->rx_ind_count = 0;  fapi_entry->rx_ind_expected = 0;
        fapi_entry->crc_ind_count = 0; fapi_entry->crc_ind_expected = 0;
        fapi_entry->uci_ind_count = 0; fapi_entry->uci_ind_expected = 0;
        fapi_entry->srs_ind_count = 0; fapi_entry->srs_ind_expected = 0;
    }

    // loop through each incoming PDU and distribute to appropriate PNF
    for (int j = 0; j < incoming_ul_tti_req.n_pdus; j++)
    {
        uint16_t rnti;
        uint16_t pnf_index;
        int target_pnf_idx = -1;

        // determine which PNF(s) should get this PDU
        switch (incoming_ul_tti_req.pdus_list[j].pdu_type)
        {
        case NFAPI_NR_UL_CONFIG_PRACH_PDU_TYPE:
            // PRACH goes to primary PNF only
            target_pnf_idx = primary_pnf;
            outgoing_ul_tti_req[target_pnf_idx].rach_present = 1;
            break;
        case NFAPI_NR_UL_CONFIG_PUCCH_PDU_TYPE:
            // PUCCH goes to primary PNF only (control channel)
            target_pnf_idx = primary_pnf;
            ulcch[target_pnf_idx]++;
            if (ulcch[target_pnf_idx] == 1)
            {
                // update expected count for UCI.indication processing
                fapi_info[incoming_ul_tti_req.SFN * MAX_SLOT + incoming_ul_tti_req.Slot].uci_ind_expected++;
                log_debug("UL_TTI.request SFN %d Slot %d: Incremented expected UCI count to %d",
                          incoming_ul_tti_req.SFN, incoming_ul_tti_req.Slot,
                          fapi_info[incoming_ul_tti_req.SFN * MAX_SLOT + incoming_ul_tti_req.Slot].uci_ind_expected);
            }
            break;
        case NFAPI_NR_UL_CONFIG_SRS_PDU_TYPE:
            // UE-specific SRS - look up which PNF handles this RNTI
            rnti = incoming_ul_tti_req.pdus_list[j].srs_pdu.rnti;
#ifdef STAGED_HARQ_UPLINK
            pnf_index = rnti_map->ue_rnti_to_pnf_curr[rnti][0]; // SRS not per HARQ process
#else
            pnf_index = rnti_map->ue_rnti_to_pnf_curr[rnti];
#endif
            // find the index in pnf_list that matches this PNF
            for (int i = 0; i < num_pnfs; i++)
            {
                if (pnf_list[i] == pnf_index)
                {
                    target_pnf_idx = i;
                    break;
                }
            }
            // TODO: SRS is not handled!
            break;
        case NFAPI_NR_UL_CONFIG_PUSCH_PDU_TYPE:
            // UE-specific PUSCH - look up which PNF handles this RNTI
            rnti = incoming_ul_tti_req.pdus_list[j].pusch_pdu.rnti;
#ifdef STAGED_HARQ_UPLINK
            // staged mapping per HARQ process; promote on NDI
            uint8_t harq_pid = incoming_ul_tti_req.pdus_list[j].pusch_pdu.pusch_data.harq_process_id;
            if (rnti_map->ue_rnti_to_pnf_curr[rnti][harq_pid] != rnti_map->ue_rnti_to_pnf_next[rnti][harq_pid] && incoming_ul_tti_req.pdus_list[j].pusch_pdu.pusch_data.new_data_indicator == 1)
            {
                rnti_map->ue_rnti_to_pnf_curr[rnti][harq_pid] = rnti_map->ue_rnti_to_pnf_next[rnti][harq_pid];
                log_info("Promoted UL RNTI 0x%04x to PNF %d for HARQ process %d on NDI", rnti, rnti_map->ue_rnti_to_pnf_curr[rnti][harq_pid], harq_pid);
            }
            pnf_index = rnti_map->ue_rnti_to_pnf_curr[rnti][harq_pid];
#else
            pnf_index = rnti_map->ue_rnti_to_pnf_curr[rnti];
#endif
            // find the index in pnf_list that matches this PNF
            for (int i = 0; i < num_pnfs; i++)
            {
                if (pnf_list[i] == pnf_index)
                {
                    target_pnf_idx = i;
                    break;
                }
            }
            ulsch[target_pnf_idx]++;
            if (ulsch[target_pnf_idx] == 1)
            {
                // update expected count for RX_DATA.indication and CRC.indication processing
                fapi_info[incoming_ul_tti_req.SFN * MAX_SLOT + incoming_ul_tti_req.Slot].rx_ind_expected++;
                fapi_info[incoming_ul_tti_req.SFN * MAX_SLOT + incoming_ul_tti_req.Slot].crc_ind_expected++;
                log_debug("UL_TTI.request SFN %d Slot %d: Incremented expected RX/CRC count to %d",
                          incoming_ul_tti_req.SFN, incoming_ul_tti_req.Slot,
                          fapi_info[incoming_ul_tti_req.SFN * MAX_SLOT + incoming_ul_tti_req.Slot].rx_ind_expected);
            }
            break;
        default:
            log_warn("Unsupported PDU type %d in UL_TTI.request", incoming_ul_tti_req.pdus_list[j].pdu_type);
            return -1;
        }

        if (incoming_ul_tti_req.pdus_list[j].pdu_type == NFAPI_NR_UL_CONFIG_PRACH_PDU_TYPE)
        {
            // add PDU to the target PNF's outgoing message
            uint8_t pdu_index = outgoing_ul_tti_req[target_pnf_idx].n_pdus;
            outgoing_ul_tti_req[target_pnf_idx].pdus_list[pdu_index] = incoming_ul_tti_req.pdus_list[j];
            outgoing_ul_tti_req[target_pnf_idx].n_pdus++;
        }
        else
        {
            for (int i = 0; i < num_pnfs; i++)
            {
                if (i == target_pnf_idx)
                {
                    // add PDU to the target PNF's outgoing message
                    uint8_t pdu_index = outgoing_ul_tti_req[i].n_pdus;
                    outgoing_ul_tti_req[i].pdus_list[pdu_index] = incoming_ul_tti_req.pdus_list[j];
                    outgoing_ul_tti_req[i].n_pdus++;

                    if (incoming_ul_tti_req.pdus_list[j].pdu_type == NFAPI_NR_UL_CONFIG_PUSCH_PDU_TYPE)
                    {
                        outgoing_ul_tti_req[i].n_ulsch++;
                    }
                    else if (incoming_ul_tti_req.pdus_list[j].pdu_type == NFAPI_NR_UL_CONFIG_PUCCH_PDU_TYPE)
                    {
                        outgoing_ul_tti_req[i].n_ulcch++;
                    }
                }
                else
                {
                    if(incoming_ul_tti_req.pdus_list[j].pdu_type == NFAPI_NR_UL_CONFIG_PUSCH_PDU_TYPE)
                    {
                        // add PDU to the target PNF's outgoing message
                        uint8_t pdu_index = outgoing_ul_tti_req[i].n_pdus;
                        outgoing_ul_tti_req[i].pdus_list[pdu_index] = incoming_ul_tti_req.pdus_list[j];
                        outgoing_ul_tti_req[i].n_pdus++;

                        // modify the RNTI to use RESERVED RNTI
                        outgoing_ul_tti_req[i].pdus_list[pdu_index].pusch_pdu.rnti = 0xFFF0;
                        outgoing_ul_tti_req[i].n_ulsch++;
                    }
                    if(incoming_ul_tti_req.pdus_list[j].pdu_type == NFAPI_NR_UL_CONFIG_PUCCH_PDU_TYPE)
                    {
                        // add PDU to the target PNF's outgoing message
                        uint8_t pdu_index = outgoing_ul_tti_req[i].n_pdus;
                        outgoing_ul_tti_req[i].pdus_list[pdu_index] = incoming_ul_tti_req.pdus_list[j];
                        outgoing_ul_tti_req[i].n_pdus++;

                        // modify the RNTI to use RESERVED RNTI
                        outgoing_ul_tti_req[i].pdus_list[pdu_index].pucch_pdu.rnti = 0xFFF0;
                        outgoing_ul_tti_req[i].n_ulcch++;
                    }
                }
            }
        }
    }

    // DEBUG: Ensure that the ULSCH PDUs do not have overlapping RB allocations
    // for(int i=0; i<incoming_ul_tti_req.n_pdus; i++)
    // {
    //     if(incoming_ul_tti_req.pdus_list[i].pdu_type == NFAPI_NR_UL_CONFIG_PUSCH_PDU_TYPE)
    //     {
    //         uint16_t rnti = incoming_ul_tti_req.pdus_list[i].pusch_pdu.rnti;
    //         uint16_t bwp_start_1 = incoming_ul_tti_req.pdus_list[i].pusch_pdu.bwp_start;
    //         uint16_t rb_start = incoming_ul_tti_req.pdus_list[i].pusch_pdu.rb_start;
    //         uint16_t rb_size = incoming_ul_tti_req.pdus_list[i].pusch_pdu.rb_size;
    //         log_debug("PUSCH PDU: RNTI=%d, BWPStart=%d, RBStart=%d, RBSize=%d", rnti, bwp_start_1, rb_start, rb_size);
    //         for(int j=i+1; j<incoming_ul_tti_req.n_pdus; j++)
    //          {
    //             if(incoming_ul_tti_req.pdus_list[j].pdu_type == NFAPI_NR_UL_CONFIG_PUSCH_PDU_TYPE)
    //              {
    //                  uint16_t rnti_2 = incoming_ul_tti_req.pdus_list[j].pusch_pdu.rnti;
    //                  uint16_t bwp_start_2 = incoming_ul_tti_req.pdus_list[j].pusch_pdu.bwp_start;
    //                  uint16_t rb_start_2 = incoming_ul_tti_req.pdus_list[j].pusch_pdu.rb_start;
    //                  uint16_t rb_size_2 = incoming_ul_tti_req.pdus_list[j].pusch_pdu.rb_size;
    //                  if(!((bwp_start_1 + rb_start + (rb_size - 1) < bwp_start_2 + rb_start_2) || (bwp_start_2 + rb_start_2 + (rb_size_2 -1) < bwp_start_1 + rb_start)))
    //                  {
    //                      log_error("Detected overlapping PUSCH RB resources (RNTI 0x%04x BWPStart: %d RBStart: %d, RBSize: %d; RNTI 0x%04x  BWPStart: %d RBStart: %d, RBSize: %d)",
    //                                 rnti, bwp_start_1, rb_start, rb_size,
    //                                 rnti_2, bwp_start_2, rb_start_2, rb_size_2);
    //                  }
    //              }
    //          }
    //     }
    // }

    // finalize outgoing buffers
    for (int i = 0; i < num_pnfs; i++)
    {
        // if no PDUs, skip
        if (outgoing_ul_tti_req[i].n_pdus == 0)
        {
            output_buffers[i] = NULL;
            output_sizes[i] = 0;
            continue;
        }
        // pack the outgoing message
        int packed_size = nfapi_nr_p7_message_pack(&outgoing_ul_tti_req[i], dl_scratch_buffer[i], 65536, 0);
        if (packed_size < 0)
        {
            log_error("Failed to pack UL_TTI.request for PNF %d", pnf_list[i]);
            return -1;
        }

        // update the checksum
        nfapi_nr_p7_update_checksum(dl_scratch_buffer[i], packed_size);

        // allocate output buffer and copy
        output_buffers[i] = dl_scratch_buffer[i];
        output_sizes[i] = packed_size;
        log_debug("UL_TTI.request for PNF %d, Size: %d", pnf_list[i], packed_size);

        // log number of ULSCH and ULCCH PDUs
        log_debug("UL_TTI.request for PNF %d has %d ULSCH PDUs and %d ULCCH PDUs", pnf_list[i], outgoing_ul_tti_req[i].n_ulsch, outgoing_ul_tti_req[i].n_ulcch);
    }

    return 0;
}
#endif

int handle_nfapi_ul_dci_request(void *input_buffer, int input_size, int *pnf_list, int num_pnfs, int primary_pnf, uint8_t **output_buffers, int *output_sizes)
{
    nfapi_nr_ul_dci_request_t incoming_ul_dci_req;
    nfapi_nr_p7_message_unpack(input_buffer, input_size, &incoming_ul_dci_req, sizeof(nfapi_nr_ul_dci_request_t), 0);
    log_debug("UL_DCI.request size: %d", input_size);
    log_debug("UL_DCI.request SFN: %d, Slot: %d, NumPdus: %d", incoming_ul_dci_req.SFN, incoming_ul_dci_req.Slot, incoming_ul_dci_req.numPdus);

    uint8_t target_pnf_index = primary_pnf;

    for (int i = 0; i < num_pnfs; i++)
    {
        // initialize other output buffers to zero size
        if (i != target_pnf_index)
        {
            output_buffers[i] = NULL;
            output_sizes[i] = 0;
            continue;
        }
        output_buffers[i] = input_buffer;
        output_sizes[i] = input_size;
        log_debug("UL_DCI.request for PNF %d, Size: %d", pnf_list[i], input_size);
    }

    return 0;
}
#endif