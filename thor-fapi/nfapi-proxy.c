#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/sctp.h>
#include <fcntl.h>

#include "log.h"
#include "common_types.h"
#include "microbenchmark.h"

#include "nfapi-proxy-common.h"
#include "nfapi_p7_downlink_processor.h"
#include "nfapi_p7_uplink_processor.h"

#include "nr_fapi_p7_utils.h"

#include "nfapi_nr_interface.h"
#include "nfapi_nr_interface_scf.h"

#ifndef GIT_COMMIT_HASH
#define GIT_COMMIT_HASH "unknown"
#endif

#ifndef GIT_COMMIT_DATE
#define GIT_COMMIT_DATE "unknown"
#endif

#ifndef BUILD_TIME
#define BUILD_TIME "unknown"
#endif

#ifndef MAX_NUM_PNF
#define MAX_NUM_PNF 2
#endif

// Maximum P7 message buffer size (64KB)
#ifndef MAX_P7_MESSAGE_SIZE
#define MAX_P7_MESSAGE_SIZE 65536
#endif

#define DEFAULT_CONTROL_SOCKET_PATH "/var/run/thor_nfapi_proxy.sock"
struct
{
    // timing information
    int current_sfn;
    int current_slot;

    // PNF list
    int pnf_list[MAX_NUM_PNF];

    // UE rnti-to-PNF mapping
    // TODO: expose this as shared memory to interact with the RIC/ controller
    uplink_rnti_to_pnf_mapping_t uplink_rnti_to_pnf;
    downlink_rnti_to_pnf_mapping_t downlink_rnti_to_pnf;

    // TODO: expose this as shared memory to interact with the fronthaul middlebox
    uplink_scheduling_info_t uplink_scheduling_info[256][20];

    // uplink FAPI info
    uplink_fapi_info_t uplink_fapi_info[MAX_SFN][MAX_SLOT];

    // Test mode
    bool test_mode;
    uint32_t test_vnf_port;
    uint32_t test_pnf_ports[MAX_NUM_PNF];

    // Slingshot mode
    bool slingshot_mode;

    // Primary PNF index
    int primary_pnf;
    bool pnf_ready[MAX_NUM_PNF];
    uint64_t pnf_connection_order[MAX_NUM_PNF];
    uint64_t next_connection_order;
    pthread_mutex_t membership_mutex;
    atomic_bool mapping_update_pending;

    // Control socket
    int control_sock;
    char control_socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];

    // P5
    uint32_t p5_vnf_sctp_port;
    uint32_t p5_sctp_port;
    int p5_max_fd;
    int p5_north_sock;
    uint32_t p5_north_sock_state;
    int p5_south_sock[MAX_NUM_PNF];
    uint32_t p5_south_sock_state[MAX_NUM_PNF];

    // P7
    uint32_t p7_udp_port;
    int p7_max_fd;
    int p7_north_sock;
    int p7_south_sock[MAX_NUM_PNF];
    uint32_t p7_vnf_udp_port;
    uint32_t p7_pnf_udp_port[MAX_NUM_PNF];

    // P7 sequence number for segmented uplink messages
    uint8_t p7_uplink_sequence_num;
} proxy_info;

uint8_t terminate = 0;
pthread_t p5_thread;
pthread_t p5_listen_thread;
pthread_t p7_thread;

struct vnf_p5_cache_t
{
    bool pnf_param_request_received;
    nfapi_nr_pnf_param_request_t pnf_param_request;
    bool pnf_config_request_received;
    nfapi_nr_pnf_config_request_t pnf_config_request;
    bool pnf_start_request_received;
    nfapi_nr_pnf_start_request_t pnf_start_request;
    bool param_request_received;
    nfapi_nr_param_request_scf_t param_request;
    bool config_request_received;
    nfapi_nr_config_request_scf_t config_request;
    bool start_request_received;
    nfapi_nr_start_request_scf_t start_request;
} vnf_p5_cache;

struct pnf_p5_cache_t
{
    bool pnf_param_response_received;
    nfapi_nr_pnf_param_response_t pnf_param_response;
    bool pnf_config_response_received;
    nfapi_nr_pnf_config_response_t pnf_config_response;
    bool pnf_start_response_received;
    nfapi_nr_pnf_start_response_t pnf_start_response;
    bool param_response_received;
    nfapi_nr_param_response_scf_t param_response;
    bool config_response_received;
    nfapi_nr_config_response_scf_t config_response;
    bool start_response_received;
    nfapi_nr_start_response_scf_t start_response;
} pnf_p5_cache;

struct control_command
{
    char cmd[16];  // Command type: "migrate", "debug"
    char arg0[16]; // RNTI or other integer argument
    char arg1[16]; // PNF index or other integer argument
};

#define INVALID_PNF_ID UINT16_MAX

static int active_pnf_count_locked(void)
{
    int count = 0;
    for (int i = 0; i < MAX_NUM_PNF; i++)
        count += proxy_info.pnf_list[i] != -1;
    return count;
}

static int ready_pnf_count_locked(void)
{
    int count = 0;
    for (int i = 0; i < MAX_NUM_PNF; i++)
        count += proxy_info.pnf_list[i] != -1 && proxy_info.pnf_ready[i];
    return count;
}

static int select_oldest_pnf_locked(void)
{
    int selected = -1;
    for (int i = 0; i < MAX_NUM_PNF; i++)
    {
        if (proxy_info.pnf_list[i] == -1)
            continue;
        if (selected == -1 || proxy_info.pnf_connection_order[i] < proxy_info.pnf_connection_order[selected])
            selected = i;
    }
    return selected;
}

static int select_oldest_ready_pnf_locked(void)
{
    int selected = -1;
    for (int i = 0; i < MAX_NUM_PNF; i++)
    {
        if (proxy_info.pnf_list[i] == -1 || !proxy_info.pnf_ready[i])
            continue;
        if (selected == -1 || proxy_info.pnf_connection_order[i] < proxy_info.pnf_connection_order[selected])
            selected = i;
    }
    return selected;
}

static void build_ready_pnf_list_locked(int ready_list[MAX_NUM_PNF])
{
    for (int i = 0; i < MAX_NUM_PNF; i++)
        ready_list[i] = proxy_info.pnf_list[i] != -1 && proxy_info.pnf_ready[i] ? i : -1;
}

static int format_l1_list_locked(char *buffer, size_t buffer_size)
{
    int used = snprintf(buffer, buffer_size, "primary=%d;secondary=", proxy_info.primary_pnf);
    bool first = true;
    for (int i = 0; i < MAX_NUM_PNF && used >= 0 && used < (int)buffer_size; i++)
    {
        if (proxy_info.pnf_list[i] == -1 || i == proxy_info.primary_pnf) continue;
        used += snprintf(buffer + used, buffer_size - used, "%s%d", first ? "" : ",", i);
        first = false;
    }
    if (used >= 0 && used < (int)buffer_size)
        used += snprintf(buffer + used, buffer_size - used, ";not_ready=");
    first = true;
    for (int i = 0; i < MAX_NUM_PNF && used >= 0 && used < (int)buffer_size; i++)
    {
        if (proxy_info.pnf_list[i] == -1 || proxy_info.pnf_ready[i]) continue;
        used += snprintf(buffer + used, buffer_size - used, "%s%d", first ? "" : ",", i);
        first = false;
    }
    return used;
}

static void remap_pnf_locked(uint16_t old_pnf, uint16_t new_pnf)
{
    for (int rnti = 0; rnti < RNTI_MAX; rnti++)
    {
        if (proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_prev[rnti] == old_pnf) proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_prev[rnti] = new_pnf;
        if (proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_curr[rnti] == old_pnf) proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_curr[rnti] = new_pnf;
        if (proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_next[rnti] == old_pnf) proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_next[rnti] = new_pnf;
#ifdef STAGED_HARQ_UPLINK
        for (int h = 0; h < MAX_NR_HARQ_PROCESSES; h++)
        {
            if (proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_prev[rnti][h] == old_pnf) proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_prev[rnti][h] = new_pnf;
            if (proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[rnti][h] == old_pnf) proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[rnti][h] = new_pnf;
            if (proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_next[rnti][h] == old_pnf) proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_next[rnti][h] = new_pnf;
        }
#else
        if (proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_prev[rnti] == old_pnf) proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_prev[rnti] = new_pnf;
        if (proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[rnti] == old_pnf) proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[rnti] = new_pnf;
        if (proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_next[rnti] == old_pnf) proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_next[rnti] = new_pnf;
#endif
    }
    memset(proxy_info.downlink_rnti_to_pnf.pdsch_to_tx_data_mapping, -1, sizeof(proxy_info.downlink_rnti_to_pnf.pdsch_to_tx_data_mapping));
}

static void reset_uplink_aggregation_locked(void);

static bool register_pnf_locked(int pnf_index, int p5_sock)
{
    bool first_active = active_pnf_count_locked() == 0;
    proxy_info.p5_south_sock[pnf_index] = p5_sock;
    proxy_info.p5_south_sock_state[pnf_index] = 0;
    proxy_info.pnf_list[pnf_index] = pnf_index;
    // Preserve zero-controller single-L1 startup. Connections added while an
    // L1 is already active, including recycled secondary IDs, require explicit
    // controller admission.
    proxy_info.pnf_ready[pnf_index] = first_active;
    proxy_info.pnf_connection_order[pnf_index] = ++proxy_info.next_connection_order;
    if (first_active)
    {
        proxy_info.primary_pnf = pnf_index;
        remap_pnf_locked(INVALID_PNF_ID, (uint16_t)pnf_index);
    }
    return first_active;
}

static bool set_pnf_ready_locked(int pnf_index, bool ready)
{
    if (pnf_index < 0 || pnf_index >= MAX_NUM_PNF || proxy_info.pnf_list[pnf_index] == -1)
        return false;

    if (proxy_info.pnf_ready[pnf_index] == ready)
        return true;

    int ready_before = ready_pnf_count_locked();
    proxy_info.pnf_ready[pnf_index] = ready;
    if (ready)
    {
        if (ready_before == 0)
            remap_pnf_locked(INVALID_PNF_ID, (uint16_t)pnf_index);
        log_info("PNF %d transitioned to ready", pnf_index);
    }
    else
    {
        int replacement = select_oldest_ready_pnf_locked();
        remap_pnf_locked((uint16_t)pnf_index,
                         replacement == -1 ? INVALID_PNF_ID : (uint16_t)replacement);
        reset_uplink_aggregation_locked();
        log_info("PNF %d transitioned to not_ready; routing fallback is %d", pnf_index, replacement);
    }
    return true;
}

static void reset_uplink_aggregation_locked(void)
{
    for (int sfn = 0; sfn < MAX_SFN; sfn++)
    {
        for (int slot = 0; slot < MAX_SLOT; slot++)
        {
            uplink_fapi_info_t *entry = &proxy_info.uplink_fapi_info[sfn][slot];
            for (int i = 0; i < MAX_NUM_PNF; i++)
            {
                if (entry->rx_buf[i] != NULL) { free_rx_data_indication(entry->rx_buf[i]); free(entry->rx_buf[i]); }
                if (entry->crc_buf[i] != NULL) { free_crc_indication(entry->crc_buf[i]); free(entry->crc_buf[i]); }
                if (entry->uci_buf[i] != NULL) { free_uci_indication(entry->uci_buf[i]); free(entry->uci_buf[i]); }
                if (entry->srs_buf[i] != NULL) { free_srs_indication(entry->srs_buf[i]); free(entry->srs_buf[i]); }
                free(entry->rx_raw_buf[i]);
                free(entry->crc_raw_buf[i]);
            }
            memset(entry, 0, sizeof(*entry));
        }
    }
}

static void disconnect_pnf_locked(int pnf_index, const char *reason)
{
    if (pnf_index < 0 || pnf_index >= MAX_NUM_PNF || proxy_info.pnf_list[pnf_index] == -1) return;
    int p5_sock = proxy_info.p5_south_sock[pnf_index];
    int p7_sock = proxy_info.p7_south_sock[pnf_index];
    proxy_info.p5_south_sock[pnf_index] = -1;
    proxy_info.p7_south_sock[pnf_index] = -1;
    proxy_info.p5_south_sock_state[pnf_index] = 0;
    proxy_info.p7_pnf_udp_port[pnf_index] = 0;
    proxy_info.pnf_list[pnf_index] = -1;
    proxy_info.pnf_ready[pnf_index] = false;
    proxy_info.pnf_connection_order[pnf_index] = 0;
    if (p5_sock != -1) close(p5_sock);
    if (p7_sock != -1) close(p7_sock);
    int new_primary = select_oldest_pnf_locked();
    if (proxy_info.primary_pnf == pnf_index) proxy_info.primary_pnf = new_primary;
    int routing_replacement = select_oldest_ready_pnf_locked();
    remap_pnf_locked((uint16_t)pnf_index,
                     routing_replacement == -1 ? INVALID_PNF_ID : (uint16_t)routing_replacement);
    reset_uplink_aggregation_locked();
    log_warn("PNF %d disconnected (%s); primary PNF is now %d", pnf_index, reason, proxy_info.primary_pnf);
}

// The P7 dispatcher holds membership_mutex while processing a slot.
static bool apply_pending_mapping_updates_locked(void)
{
    if (!atomic_load_explicit(&proxy_info.mapping_update_pending, memory_order_acquire))
        return false;

    bool updated = false;
    if (atomic_load_explicit(&proxy_info.mapping_update_pending, memory_order_relaxed))
    {
        for (int rnti = 0; rnti < RNTI_MAX; rnti++)
        {
#ifndef STAGED_HARQ_UPLINK
            if (proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_next[rnti] !=
                proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[rnti])
            {
                updated = true;
                proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[rnti] =
                    proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_next[rnti];
            }
#endif
            if (proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_next[rnti] !=
                proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_curr[rnti])
            {
                updated = true;
                proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_curr[rnti] =
                    proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_next[rnti];
            }
        }
        atomic_store_explicit(&proxy_info.mapping_update_pending, false,
                              memory_order_release);
    }
    return updated;
}

// Control response structure
struct control_response
{
    int status;
    char message[256];
};

enum
{
    P5_STATE_PNF_PARAM_REQUEST = 1,
    P5_STATE_PNF_PARAM_RESPONSE,
    P5_STATE_PNF_CONFIG_REQUEST,
    P5_STATE_PNF_CONFIG_RESPONSE,
    P5_STATE_PNF_START_REQUEST,
    P5_STATE_PNF_START_RESPONSE,
    P5_STATE_PARAM_REQUEST,
    P5_STATE_PARAM_RESPONSE,
    P5_STATE_CONFIG_REQUEST,
    P5_STATE_CONFIG_RESPONSE,
    P5_STATE_START_REQUEST,
    P5_STATE_START_RESPONSE
} p5_state_enum;

void sigint_handler(int signum)
{
    log_info("Received signal %d, terminating...", signum);
    terminate = 1;
}

static const char *microbenchmark_message_type(uint16_t message_id)
{
    switch (message_id)
    {
    case NFAPI_NR_PHY_MSG_TYPE_DL_TTI_REQUEST: return "DL_TTI_REQUEST";
    case NFAPI_NR_PHY_MSG_TYPE_UL_TTI_REQUEST: return "UL_TTI_REQUEST";
    case NFAPI_NR_PHY_MSG_TYPE_SLOT_INDICATION: return "SLOT_INDICATION";
    case NFAPI_NR_PHY_MSG_TYPE_UL_DCI_REQUEST: return "UL_DCI_REQUEST";
    case NFAPI_NR_PHY_MSG_TYPE_TX_DATA_REQUEST: return "TX_DATA_REQUEST";
    case NFAPI_NR_PHY_MSG_TYPE_RX_DATA_INDICATION: return "RX_DATA_INDICATION";
    case NFAPI_NR_PHY_MSG_TYPE_CRC_INDICATION: return "CRC_INDICATION";
    case NFAPI_NR_PHY_MSG_TYPE_UCI_INDICATION: return "UCI_INDICATION";
    case NFAPI_NR_PHY_MSG_TYPE_SRS_INDICATION: return "SRS_INDICATION";
    case NFAPI_NR_PHY_MSG_TYPE_RACH_INDICATION: return "RACH_INDICATION";
    default: return NULL;
    }
}

int nfapi_p7_process_north(int sock)
{
    log_trace("Processing P7 message from VNF");
    uint8_t buffer[65536];
    int message_size = 0;
    message_size = peek_p7_udp_message_size(sock);
    if (message_size < 0)
    {
        return -1;
    }

    int ret = read_p7_udp_message(sock, buffer, message_size);
    if (ret < 0)
    {
        return -1;
    }
    nfapi_nr_p7_message_header_t messageHeader;
    const bool result = nfapi_nr_p7_message_header_unpack(buffer, message_size, &messageHeader, sizeof(messageHeader), 0);
    if (!result)
    {
        log_error("Failed to unpack P7 message header");
        return -1;
    }

    if(NFAPI_P7_GET_MORE(messageHeader.m_segment_sequence) && messageHeader.message_id != NFAPI_NR_PHY_MSG_TYPE_TX_DATA_REQUEST) {
        log_warn("Segmented messages are not supported. Dropping message from VNF, Message ID: 0x%02x", messageHeader.message_id);
    }

    uint16_t benchmark_sfn = 0;
    uint16_t benchmark_slot = 0;
    const char *benchmark_type = microbenchmark_message_type(messageHeader.message_id);
    if (benchmark_type != NULL &&
        peek_nr_nfapi_p7_sfn_slot(buffer, message_size, &benchmark_sfn, &benchmark_slot))
    {
        microbenchmark_log_event("NORTH", benchmark_type, benchmark_sfn,
                                 benchmark_slot, -1, "ARRIVE");
    }

    int ready_pnf_list[MAX_NUM_PNF];
    build_ready_pnf_list_locked(ready_pnf_list);
    int data_primary = proxy_info.primary_pnf;
    if (data_primary < 0 || !proxy_info.pnf_ready[data_primary])
        data_primary = select_oldest_ready_pnf_locked();

#ifdef DL_LEGACY
    // TODO:
    // 1. based on the number of PNFs we have, we should preallocate the necessary messages for each of them
    // 2. then based on the received message, we need to unpack it, and process them accordingly
    // 3. for downlink, we need to check the rnti, and determine which PNF to pack into
    // 4. for uplink, we need to extract the scheduling information, and generate a bitmap for that particular slot
    // 5. then, for each PNF, we need to pack the messages accordingly, and send them out
    void *outgoing_msg = NULL;
    bool outgoing_mask[MAX_NUM_PNF] = {false};

    uint32_t struct_size = 0;

    uint8_t write_buffer[MAX_NUM_PNF][65536];
    uint32_t write_buffer_size[MAX_NUM_PNF] = {0};

    switch (messageHeader.message_id)
    {
    case NFAPI_NR_PHY_MSG_TYPE_DL_TTI_REQUEST:
        struct_size = sizeof(nfapi_nr_dl_tti_request_t);
        nfapi_nr_dl_tti_request_t incoming_dl_tti_req;
        nfapi_nr_p7_message_unpack(buffer, message_size, &incoming_dl_tti_req, sizeof(incoming_dl_tti_req), 0);
        log_debug("DL_TTI_REQUEST SFN: %d, Slot: %d, NumPdus: %d, NumGroup: %d", incoming_dl_tti_req.SFN, incoming_dl_tti_req.Slot, incoming_dl_tti_req.dl_tti_request_body.nPDUs, incoming_dl_tti_req.dl_tti_request_body.nGroup);

        // PDU types include: 0: PDCCH, 1: PDSCH, 2: CSI-RS, 3: SSB
        outgoing_msg = malloc(sizeof(nfapi_nr_dl_tti_request_t) * MAX_NUM_PNF);

        ret = process_nfapi_dl_tti_request(&incoming_dl_tti_req, (nfapi_nr_dl_tti_request_t *)outgoing_msg, ready_pnf_list, MAX_NUM_PNF, &proxy_info.downlink_rnti_to_pnf, outgoing_mask);
        if (ret < 0)
        {
            log_error("Failed to process DL_TTI_REQUEST message");
            free(outgoing_msg);
            outgoing_msg = NULL;
            return -1;
        }
        break;
    case NFAPI_NR_PHY_MSG_TYPE_TX_DATA_REQUEST:
        struct_size = sizeof(nfapi_nr_tx_data_request_t);
        nfapi_nr_tx_data_request_t incoming_tx_data_req;
        nfapi_nr_p7_message_unpack(buffer, message_size, &incoming_tx_data_req, sizeof(incoming_tx_data_req), 0);
        log_debug("TX_DATA_REQUEST SFN: %d, Slot: %d, NumPdus: %d", incoming_tx_data_req.SFN, incoming_tx_data_req.Slot, incoming_tx_data_req.Number_of_PDUs);

        outgoing_msg = malloc(sizeof(nfapi_nr_tx_data_request_t) * MAX_NUM_PNF);

        ret = process_nfapi_tx_data_request(&incoming_tx_data_req, (nfapi_nr_tx_data_request_t *)outgoing_msg, ready_pnf_list, MAX_NUM_PNF, &proxy_info.downlink_rnti_to_pnf, outgoing_mask);
        if (ret < 0)
        {
            log_error("nfapi_p7_process_north: Failed to process TX_DATA_REQUEST message");
            free(outgoing_msg);
            outgoing_msg = NULL;
            return -1;
        }
        break;
    case NFAPI_NR_PHY_MSG_TYPE_UL_TTI_REQUEST:
        struct_size = sizeof(nfapi_nr_ul_tti_request_t);
        nfapi_nr_ul_tti_request_t incoming_ul_tti_req;
        nfapi_nr_p7_message_unpack(buffer, message_size, &incoming_ul_tti_req, sizeof(incoming_ul_tti_req), 0);
        log_debug("UL_TTI_REQUEST SFN: %d, Slot: %d, NumPdus: %d, NumGroup: %d", incoming_ul_tti_req.SFN, incoming_ul_tti_req.Slot, incoming_ul_tti_req.n_pdus, incoming_ul_tti_req.n_group);

        // PDU types include: 0: PRACH, 1: PUSCH, 2: PUCCH, 3: SRS
        outgoing_msg = malloc(sizeof(nfapi_nr_ul_tti_request_t) * MAX_NUM_PNF);

        uplink_scheduling_info_t scheduling_info;
        ret = process_nfapi_ul_tti_request(&incoming_ul_tti_req, (nfapi_nr_ul_tti_request_t *)outgoing_msg, ready_pnf_list, MAX_NUM_PNF, &proxy_info.uplink_rnti_to_pnf, &scheduling_info, outgoing_mask);
        if (ret < 0)
        {
            log_error("Failed to process UL_TTI_REQUEST message");
            free(outgoing_msg);
            outgoing_msg = NULL;
            return -1;
        }

        // Reset fapi_info for this SFN/Slot to prevent stale counts from accumulating
        // across SFN cycles. If a previous cycle's indication was lost (UDP is unreliable),
        // the counts would be permanently corrupted without this reset.
        {
            uplink_fapi_info_t *fapi_entry = &proxy_info.uplink_fapi_info[incoming_ul_tti_req.SFN][incoming_ul_tti_req.Slot];
            if (fapi_entry->rx_ind_count != 0 || fapi_entry->crc_ind_count != 0 ||
                fapi_entry->uci_ind_count != 0 || fapi_entry->srs_ind_count != 0 ||
                fapi_entry->rx_ind_expected != 0 || fapi_entry->crc_ind_expected != 0 ||
                fapi_entry->uci_ind_expected != 0 || fapi_entry->srs_ind_expected != 0)
            {
                log_warn("UL_TTI_REQUEST SFN %d Slot %d: stale fapi_info detected (rx=%d/%d crc=%d/%d uci=%d/%d srs=%d/%d), resetting",
                         incoming_ul_tti_req.SFN, incoming_ul_tti_req.Slot,
                         fapi_entry->rx_ind_count, fapi_entry->rx_ind_expected,
                         fapi_entry->crc_ind_count, fapi_entry->crc_ind_expected,
                         fapi_entry->uci_ind_count, fapi_entry->uci_ind_expected,
                         fapi_entry->srs_ind_count, fapi_entry->srs_ind_expected);
            }
            for (int b = 0; b < MAX_NUM_PNF; b++)
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

        for (int i = 0; i < MAX_NUM_PNF; i++)
        {
            if (outgoing_mask[i] && ready_pnf_list[i] != -1)
            {
                log_debug("UL_TTI_REQUEST SFN: %d, Slot: %d sent to PNF %d", incoming_ul_tti_req.SFN, incoming_ul_tti_req.Slot, i);
                nfapi_nr_ul_tti_request_t *out_msg = (nfapi_nr_ul_tti_request_t *)(outgoing_msg + i * struct_size);
                if (out_msg->n_ulsch > 0)
                {
                    proxy_info.uplink_fapi_info[incoming_ul_tti_req.SFN][incoming_ul_tti_req.Slot].rx_ind_expected++;
                    proxy_info.uplink_fapi_info[incoming_ul_tti_req.SFN][incoming_ul_tti_req.Slot].crc_ind_expected++;
                    log_debug("Expecting RX_DATA.indication and CRC.indication for SFN %d, Slot %d from PNF %d", incoming_ul_tti_req.SFN, incoming_ul_tti_req.Slot, i);
                    log_debug("  Number of ULSCH PDUs: %d", out_msg->n_ulsch);
                    log_debug("  Number of ULCCH PDUs: %d", out_msg->n_ulcch);
                }
                if (out_msg->n_ulcch > 0)
                {
                    proxy_info.uplink_fapi_info[incoming_ul_tti_req.SFN][incoming_ul_tti_req.Slot].uci_ind_expected++;
                    log_debug("Expecting UCI.indication for SFN %d, Slot %d from PNF %d", incoming_ul_tti_req.SFN, incoming_ul_tti_req.Slot, i);
                }
                // TODO: how to handle SRS PDUs?
            }
        }
        break;
    case NFAPI_NR_PHY_MSG_TYPE_UL_DCI_REQUEST:
        struct_size = sizeof(nfapi_nr_ul_dci_request_t);
        nfapi_nr_ul_dci_request_t incoming_ul_dci_req;
        nfapi_nr_p7_message_unpack(buffer, message_size, &incoming_ul_dci_req, sizeof(incoming_ul_dci_req), 0);
        log_debug("UL_DCI_REQUEST SFN: %d, Slot: %d, NumPdus: %d", incoming_ul_dci_req.SFN, incoming_ul_dci_req.Slot, incoming_ul_dci_req.numPdus);

        outgoing_msg = malloc(sizeof(nfapi_nr_ul_dci_request_t) * MAX_NUM_PNF);
        ret = process_nfapi_ul_dci_request(&incoming_ul_dci_req, (nfapi_nr_ul_dci_request_t *)outgoing_msg, MAX_NUM_PNF, outgoing_mask);
        if (ret < 0)
        {
            log_error("Failed to process UL_DCI_REQUEST message");
            free(outgoing_msg);
            outgoing_msg = NULL;
            return -1;
        }
        break;
    default:
        log_warn("Received unknown message, ID: 0x%02x", messageHeader.message_id);
        break;
    }

    // pack the messages for each of the PNFs
    for (int i = 0; i < MAX_NUM_PNF; i++)
    {
        if (outgoing_mask[i] && ready_pnf_list[i] != -1)
        {
            write_buffer_size[i] = nfapi_nr_p7_message_pack(((uint8_t *)outgoing_msg) + i * struct_size, write_buffer[i], sizeof(write_buffer[i]), 0);
            nfapi_nr_p7_update_checksum(write_buffer[i], write_buffer_size[i]);
            if (write_buffer_size[i] <= 0)
            {
                log_error("Failed to pack message for PNF %d", i);
                free(outgoing_msg);
                outgoing_msg = NULL;
                return -1;
            }
        }
    }

    // execute send for each of the outgoing messages
    for (int i = 0; i < MAX_NUM_PNF; i++)
    {
        if (outgoing_mask[i] && ready_pnf_list[i] != -1 && proxy_info.p7_south_sock[i] != -1)
        {
            ret = send(proxy_info.p7_south_sock[i], write_buffer[i], write_buffer_size[i], 0);
            if (ret < 0)
            {
                log_error("send");
                free(outgoing_msg);
                outgoing_msg = NULL;
                return -1;
            }
            if (benchmark_type != NULL)
                microbenchmark_log_event("NORTH", benchmark_type, benchmark_sfn,
                                         benchmark_slot, i, "DEPART");
        }
    }

    // free the earlier allocated memory
    if (outgoing_msg)
    {
        free(outgoing_msg);
        outgoing_msg = NULL;
    }
#else
    uint8_t *output_ptr[MAX_NUM_PNF] = {0};
    int output_size_ptr[MAX_NUM_PNF] = {0};

    if (ready_pnf_count_locked() == 0)
    {
        log_warn("Dropping P7 message because no PNF is ready");
        return 0;
    }

    switch (messageHeader.message_id)
    {
    case NFAPI_NR_PHY_MSG_TYPE_DL_TTI_REQUEST:
        ret = handle_nfapi_dl_tti_request(&buffer, message_size, ready_pnf_list, MAX_NUM_PNF, data_primary, &proxy_info.downlink_rnti_to_pnf, output_ptr, (int *)&output_size_ptr);
        if (ret < 0)
        {
            log_error("Failed to handle DL_TTI_REQUEST message");
            return -1;
        }
        break;
    case NFAPI_NR_PHY_MSG_TYPE_TX_DATA_REQUEST:
        ret = handle_nfapi_tx_data_request(&buffer, message_size, ready_pnf_list, MAX_NUM_PNF, data_primary, output_ptr, (int *)&output_size_ptr);
        if (ret < 0)
        {
            log_error("Failed to handle TX_DATA_REQUEST message");
            return -1;
        }
        break;
    case NFAPI_NR_PHY_MSG_TYPE_UL_TTI_REQUEST:
        ret = handle_nfapi_ul_tti_request(&buffer, message_size, ready_pnf_list, MAX_NUM_PNF, data_primary, &proxy_info.uplink_rnti_to_pnf, &proxy_info.uplink_fapi_info[0][0], output_ptr, (int *)&output_size_ptr);
        if (ret < 0)
        {
            log_error("Failed to handle UL_TTI_REQUEST message");
            return -1;
        }
        break;
    case NFAPI_NR_PHY_MSG_TYPE_UL_DCI_REQUEST:
        ret = handle_nfapi_ul_dci_request(&buffer, message_size, ready_pnf_list, MAX_NUM_PNF, data_primary, output_ptr, (int *)&output_size_ptr);
        if (ret < 0)
        {
            log_error("Failed to handle UL_DCI_REQUEST message");
            return -1;
        }
        break;
    default:
        log_warn("Received unknown message, ID: 0x%02x", messageHeader.message_id);
        break;
    }

    // execute send for each of the outgoing messages
    for (int i = 0; i < MAX_NUM_PNF; i++)
    {
        if (ready_pnf_list[i] != -1 && proxy_info.p7_south_sock[i] != -1 && output_size_ptr[i] > 0)
        {
            log_trace("Sending message to PNF %d, Size: %d", proxy_info.pnf_list[i], output_size_ptr[i]);
            ret = send(proxy_info.p7_south_sock[i], output_ptr[i], output_size_ptr[i], 0);
            if (ret < 0)
            {
                log_error("send");
                return -1;
            }
            if (benchmark_type != NULL)
                microbenchmark_log_event("NORTH", benchmark_type, benchmark_sfn,
                                         benchmark_slot, i, "DEPART");
        }
    }
    memset(output_ptr, 0, sizeof(output_ptr));
    memset(output_size_ptr, 0, sizeof(output_size_ptr));
#endif
    return 0;
}

int nfapi_p7_process_south(int sock, int pnf_index)
{
    log_trace("Processing P7 message from PNF %d", pnf_index);
    uint8_t buffer[65536];
    int message_size = 0;
    message_size = peek_p7_udp_message_size(sock);
    if (message_size < 0)
    {
        return -1;
    }

    int ret = read_p7_udp_message(sock, buffer, message_size);
    if (ret < 0)
    {
        return -1;
    }
    nfapi_nr_p7_message_header_t messageHeader;
    const bool result = nfapi_nr_p7_message_header_unpack(buffer, message_size, &messageHeader, sizeof(messageHeader), 0);
    if (!result)
    {
        log_error("Failed to unpack P7 message header");
        return -1;
    }

    if (!proxy_info.pnf_ready[pnf_index] &&
        messageHeader.message_id != NFAPI_NR_PHY_MSG_TYPE_SLOT_INDICATION &&
        messageHeader.message_id != NFAPI_NR_PHY_MSG_TYPE_TIMING_INFO)
    {
        log_trace("Dropping P7 message 0x%02x from not_ready PNF %d", messageHeader.message_id, pnf_index);
        return 0;
    }

    if(NFAPI_P7_GET_MORE(messageHeader.m_segment_sequence)) {
        log_warn("Segmented messages are not supported. Dropping message from PNF %d, Message ID: 0x%02x", pnf_index, messageHeader.message_id);
        return -1;
    }

    uint16_t benchmark_sfn = 0;
    uint16_t benchmark_slot = 0;
    const char *benchmark_type = microbenchmark_message_type(messageHeader.message_id);
    const bool benchmark_admitted = benchmark_type != NULL &&
        (messageHeader.message_id != NFAPI_NR_PHY_MSG_TYPE_SLOT_INDICATION ||
         pnf_index == proxy_info.primary_pnf) &&
        peek_nr_nfapi_p7_sfn_slot(buffer, message_size, &benchmark_sfn, &benchmark_slot);
    if (benchmark_admitted)
        microbenchmark_log_event("SOUTH", benchmark_type, benchmark_sfn,
                                 benchmark_slot, pnf_index, "ARRIVE");

    // TODO:
    // 1. based on the number of PNFs we have, we need to check for duplicate messages (e.g., SLOT_INDICATION)
    // 2. for uplink messages that need to be merged (e.g., RX_DATA.indication, CRC.indication), we need to buffer them until we have received all expected messages
    // 3. then, we need to pack the messages accordingly, and send them out to the VNF
    void *outgoing_msg = NULL;

    uint32_t struct_size = 0;

    uint8_t write_buffer[65536];
    uint32_t write_buffer_size = 0;

    switch (messageHeader.message_id)
    {
    case NFAPI_NR_PHY_MSG_TYPE_SLOT_INDICATION:
        struct_size = sizeof(nfapi_nr_slot_indication_scf_t);
        nfapi_nr_slot_indication_scf_t *incoming_slot_ind = calloc(1, sizeof(nfapi_nr_slot_indication_scf_t));
        nfapi_nr_p7_message_unpack(buffer, message_size, incoming_slot_ind, sizeof(*incoming_slot_ind), 0);
        log_trace("SLOT_INDICATION SFN: %d, Slot: %d", incoming_slot_ind->sfn, incoming_slot_ind->slot);

        uint8_t *slot_ind_count = &proxy_info.uplink_fapi_info[incoming_slot_ind->sfn][incoming_slot_ind->slot].slot_ind_count;
        if (pnf_index == proxy_info.primary_pnf)
        {
            if (incoming_slot_ind->slot == 0)
            {
                log_debug("Received SLOT_INDICATION SFN: %d, Slot: %d from PNF %d", incoming_slot_ind->sfn, incoming_slot_ind->slot, pnf_index);
            }
            proxy_info.current_sfn = incoming_slot_ind->sfn;
            proxy_info.current_slot = incoming_slot_ind->slot;

            outgoing_msg = incoming_slot_ind;
            *slot_ind_count = 0;

            // Controller migrations are promoted only at this slot boundary.
            // Most slots have no staged migration and avoid scanning all RNTIs.
            if (apply_pending_mapping_updates_locked())
            {
                log_info("UE to PNF mapping updated at SFN %d Slot %d", incoming_slot_ind->sfn, incoming_slot_ind->slot);
            }
        }
        else
        {
            free(incoming_slot_ind);
        }
        break;
    case NFAPI_NR_PHY_MSG_TYPE_RACH_INDICATION:
        struct_size = sizeof(nfapi_nr_rach_indication_t);
        nfapi_nr_rach_indication_t *incoming_rach_ind = calloc(1, sizeof(nfapi_nr_rach_indication_t));
        nfapi_nr_p7_message_unpack(buffer, message_size, incoming_rach_ind, sizeof(*incoming_rach_ind), 0);
        log_debug("RACH_INDICATION SFN: %d, Slot: %d, NumPdus: %d", incoming_rach_ind->sfn, incoming_rach_ind->slot, incoming_rach_ind->number_of_pdus);

        uint8_t *rach_ind_count = &proxy_info.uplink_fapi_info[incoming_rach_ind->sfn][incoming_rach_ind->slot].rach_ind_count;
        (*rach_ind_count)++;

        if (*rach_ind_count == 1)
        {
            outgoing_msg = incoming_rach_ind;
            *rach_ind_count = 0;
        }
        break;
    case NFAPI_NR_PHY_MSG_TYPE_UCI_INDICATION:
        struct_size = sizeof(nfapi_nr_uci_indication_t);
        nfapi_nr_uci_indication_t *incoming_uci_ind = calloc(1, sizeof(nfapi_nr_uci_indication_t));
        nfapi_nr_p7_message_unpack(buffer, message_size, incoming_uci_ind, sizeof(*incoming_uci_ind), 0);
        log_debug("UCI_INDICATION SFN: %d, Slot: %d, NumPdus: %d", incoming_uci_ind->sfn, incoming_uci_ind->slot, incoming_uci_ind->num_ucis);

        uint8_t *uci_ind_count = &proxy_info.uplink_fapi_info[incoming_uci_ind->sfn][incoming_uci_ind->slot].uci_ind_count;
        (*uci_ind_count)++;
        log_debug("UCI_INDICATION count for SFN %d, Slot %d: %d", incoming_uci_ind->sfn, incoming_uci_ind->slot, *uci_ind_count);

        uint8_t *uci_ind_expected = &proxy_info.uplink_fapi_info[incoming_uci_ind->sfn][incoming_uci_ind->slot].uci_ind_expected;
        log_debug("UCI_INDICATION expected for SFN %d, Slot %d: %d", incoming_uci_ind->sfn, incoming_uci_ind->slot, *uci_ind_expected);

        // No indication expected for this slot - discard to avoid stale state
        if (*uci_ind_expected == 0)
        {
            log_warn("UCI_INDICATION for SFN %d Slot %d: expected=0 but received (count=%d), discarding", incoming_uci_ind->sfn, incoming_uci_ind->slot, *uci_ind_count);
            free_uci_indication(incoming_uci_ind);
            free(incoming_uci_ind);
            *uci_ind_count = 0;
        }
        else if (*uci_ind_expected == 1)
        {
            outgoing_msg = incoming_uci_ind;
            *uci_ind_count = 0;
            *uci_ind_expected = 0;
        }
        else
        {
            // allocate memory to buffer the UCI indication
            proxy_info.uplink_fapi_info[incoming_uci_ind->sfn][incoming_uci_ind->slot].uci_buf[pnf_index] = calloc(1, sizeof(nfapi_nr_uci_indication_t));

            // copy UCI indication to buffer
            copy_uci_indication(incoming_uci_ind, proxy_info.uplink_fapi_info[incoming_uci_ind->sfn][incoming_uci_ind->slot].uci_buf[pnf_index]);

            // once we have received all UCI indications from all expected PNFs, we can proceed
            if (*uci_ind_count == *uci_ind_expected)
            {
                outgoing_msg = calloc(1, sizeof(nfapi_nr_uci_indication_t));
                ret = process_uci_indication(incoming_uci_ind, (nfapi_nr_uci_indication_t *)outgoing_msg, MAX_NUM_PNF, proxy_info.uplink_fapi_info[incoming_uci_ind->sfn][incoming_uci_ind->slot].uci_buf);
                if (ret < 0)
                {
                    log_error("Failed to process UCI_INDICATION message");
                    free(outgoing_msg);
                    outgoing_msg = NULL;
                    return -1;
                }
                for (int i = 0; i < MAX_NUM_PNF; i++)
                {
                    if (proxy_info.uplink_fapi_info[incoming_uci_ind->sfn][incoming_uci_ind->slot].uci_buf[i] != NULL)
                    {
                        free_uci_indication(proxy_info.uplink_fapi_info[incoming_uci_ind->sfn][incoming_uci_ind->slot].uci_buf[i]);
                        free(proxy_info.uplink_fapi_info[incoming_uci_ind->sfn][incoming_uci_ind->slot].uci_buf[i]);
                        proxy_info.uplink_fapi_info[incoming_uci_ind->sfn][incoming_uci_ind->slot].uci_buf[i] = NULL;
                    }
                }
                *uci_ind_count = 0;
                *uci_ind_expected = 0;
            }
        }
        break;
    case NFAPI_NR_PHY_MSG_TYPE_SRS_INDICATION:
        struct_size = sizeof(nfapi_nr_srs_indication_t);
        nfapi_nr_srs_indication_t *incoming_srs_ind = calloc(1, sizeof(nfapi_nr_srs_indication_t));
        nfapi_nr_p7_message_unpack(buffer, message_size, incoming_srs_ind, sizeof(*incoming_srs_ind), 0);
        log_debug("SRS_INDICATION SFN: %d, Slot: %d, NumPdus: %d", incoming_srs_ind->sfn, incoming_srs_ind->slot, incoming_srs_ind->number_of_pdus);

        // increment counter to the number of SRS indication received
        uint8_t *srs_ind_count = &proxy_info.uplink_fapi_info[incoming_srs_ind->sfn][incoming_srs_ind->slot].srs_ind_count;
        (*srs_ind_count)++;

        // get the expected number of SRS indication
        uint8_t *srs_ind_expected = &proxy_info.uplink_fapi_info[incoming_srs_ind->sfn][incoming_srs_ind->slot].srs_ind_expected;

        // No indication expected for this slot - discard to avoid stale state
        if (*srs_ind_expected == 0)
        {
            log_warn("SRS_INDICATION for SFN %d Slot %d: expected=0 but received (count=%d), discarding", incoming_srs_ind->sfn, incoming_srs_ind->slot, *srs_ind_count);
            free_srs_indication(incoming_srs_ind);
            free(incoming_srs_ind);
            *srs_ind_count = 0;
        }
        // if we only expect messages coming from one PNF, there is no need to go through the merge logic
        else if (*srs_ind_expected == 1)
        {
            outgoing_msg = incoming_srs_ind;
            *srs_ind_count = 0;
            *srs_ind_expected = 0;
        }
        // if there are messages coming from more than one PNF, we need to merge them as one
        else
        {
            // allocate memory to buffer the SRS indication
            proxy_info.uplink_fapi_info[incoming_srs_ind->sfn][incoming_srs_ind->slot].srs_buf[pnf_index] = calloc(1, sizeof(nfapi_nr_srs_indication_t));

            // copy SRS indication to buffer
            copy_srs_indication(incoming_srs_ind, proxy_info.uplink_fapi_info[incoming_srs_ind->sfn][incoming_srs_ind->slot].srs_buf[pnf_index]);

            // once we have received all SRS indications from all expected PNFs, we can proceed
            if (*srs_ind_count == *srs_ind_expected)
            {
                outgoing_msg = calloc(1, sizeof(nfapi_nr_srs_indication_t));
                ret = process_srs_indication(incoming_srs_ind, (nfapi_nr_srs_indication_t *)outgoing_msg, MAX_NUM_PNF, proxy_info.uplink_fapi_info[incoming_srs_ind->sfn][incoming_srs_ind->slot].srs_buf);
                if (ret < 0)
                {
                    log_error("Failed to process SRS_INDICATION message");
                    free(outgoing_msg);
                    outgoing_msg = NULL;
                    return -1;
                }
                for (int i = 0; i < MAX_NUM_PNF; i++)
                {
                    if (proxy_info.uplink_fapi_info[incoming_srs_ind->sfn][incoming_srs_ind->slot].srs_buf[i] != NULL)
                    {
                        free_srs_indication(proxy_info.uplink_fapi_info[incoming_srs_ind->sfn][incoming_srs_ind->slot].srs_buf[i]);
                        free(proxy_info.uplink_fapi_info[incoming_srs_ind->sfn][incoming_srs_ind->slot].srs_buf[i]);
                        proxy_info.uplink_fapi_info[incoming_srs_ind->sfn][incoming_srs_ind->slot].srs_buf[i] = NULL;
                    }
                }
                *srs_ind_count = 0;
                *srs_ind_expected = 0;
            }
        }
        break;
#ifndef UL_SEGMENTATION_REASSEMBLY
    case NFAPI_NR_PHY_MSG_TYPE_CRC_INDICATION:
        struct_size = sizeof(nfapi_nr_crc_indication_t);
        nfapi_nr_crc_indication_t *incoming_crc_ind = calloc(1, sizeof(nfapi_nr_crc_indication_t));
        nfapi_nr_p7_message_unpack(buffer, message_size, incoming_crc_ind, sizeof(*incoming_crc_ind), 0);
        log_debug("CRC_INDICATION SFN: %d, Slot: %d, NumPdus: %d", incoming_crc_ind->sfn, incoming_crc_ind->slot, incoming_crc_ind->number_crcs);
        // debug print all CRCs
        for (int i = 0; i < incoming_crc_ind->number_crcs; i++)
        {
            log_debug("  CRC %d: RNTI: 0x%04x, HARQ PID: %d, CRC status: %d", i, incoming_crc_ind->crc_list[i].rnti, incoming_crc_ind->crc_list[i].harq_id, incoming_crc_ind->crc_list[i].tb_crc_status);
        }

        // increment counter to the number of CRC indication received
        uint8_t *crc_ind_count = &proxy_info.uplink_fapi_info[incoming_crc_ind->sfn][incoming_crc_ind->slot].crc_ind_count;
        (*crc_ind_count)++;
        log_trace("CRC_INDICATION count: %d", *crc_ind_count);

        // get the expected number of CRC indication
        uint8_t *crc_ind_expected = &proxy_info.uplink_fapi_info[incoming_crc_ind->sfn][incoming_crc_ind->slot].crc_ind_expected;
        log_trace("CRC_INDICATION expected from %d PNFs", *crc_ind_expected);

        // No indication expected for this slot - discard to avoid stale state
        if (*crc_ind_expected == 0)
        {
            log_warn("CRC_INDICATION for SFN %d Slot %d: expected=0 but received (count=%d), discarding", incoming_crc_ind->sfn, incoming_crc_ind->slot, *crc_ind_count);
            free_crc_indication(incoming_crc_ind);
            free(incoming_crc_ind);
            *crc_ind_count = 0;
        }
        // if we only expect messages coming from one PNF, there is no need to go through the merge logic
        else if (*crc_ind_expected == 1)
        {
            outgoing_msg = incoming_crc_ind;
            *crc_ind_count = 0;
            *crc_ind_expected = 0;
        }
        // if there are messages coming from more than one PNF, we need to merge them as one
        else
        {
            // allocate memory to buffer the CRC indication
            proxy_info.uplink_fapi_info[incoming_crc_ind->sfn][incoming_crc_ind->slot].crc_buf[pnf_index] = calloc(1, sizeof(nfapi_nr_crc_indication_t));

            // copy CRC indication to buffer
            copy_crc_indication(incoming_crc_ind, proxy_info.uplink_fapi_info[incoming_crc_ind->sfn][incoming_crc_ind->slot].crc_buf[pnf_index]);

            // once we have received all CRC indications from all expected PNFs, we can proceed
            if (*crc_ind_count == *crc_ind_expected)
            {
                outgoing_msg = calloc(1, sizeof(nfapi_nr_crc_indication_t));

                ret = process_crc_indication(incoming_crc_ind, (nfapi_nr_crc_indication_t *)outgoing_msg, MAX_NUM_PNF, proxy_info.uplink_fapi_info[incoming_crc_ind->sfn][incoming_crc_ind->slot].crc_buf);
                if (ret < 0)
                {
                    log_error("Failed to process CRC_INDICATION message");
                    free(outgoing_msg);
                    outgoing_msg = NULL;
                    return -1;
                }

                for (int i = 0; i < MAX_NUM_PNF; i++)
                {
                    if (proxy_info.uplink_fapi_info[incoming_crc_ind->sfn][incoming_crc_ind->slot].crc_buf[i] != NULL)
                    {
                        free_crc_indication(proxy_info.uplink_fapi_info[incoming_crc_ind->sfn][incoming_crc_ind->slot].crc_buf[i]);
                        free(proxy_info.uplink_fapi_info[incoming_crc_ind->sfn][incoming_crc_ind->slot].crc_buf[i]);
                        proxy_info.uplink_fapi_info[incoming_crc_ind->sfn][incoming_crc_ind->slot].crc_buf[i] = NULL;
                    }
                }
                *crc_ind_count = 0;
                *crc_ind_expected = 0;
            }
        }
        break;
    case NFAPI_NR_PHY_MSG_TYPE_RX_DATA_INDICATION:
        struct_size = sizeof(nfapi_nr_rx_data_indication_t);
        nfapi_nr_rx_data_indication_t *incoming_rx_data_ind = calloc(1, sizeof(nfapi_nr_rx_data_indication_t));
        nfapi_nr_p7_message_unpack(buffer, message_size, incoming_rx_data_ind, sizeof(*incoming_rx_data_ind), 0);
        log_debug("RX_DATA_INDICATION SFN: %d, Slot: %d, NumPdus: %d", incoming_rx_data_ind->sfn, incoming_rx_data_ind->slot, incoming_rx_data_ind->number_of_pdus);
        // debug print all PDUs
        for (int i = 0; i < incoming_rx_data_ind->number_of_pdus; i++)
        {
            log_debug("  RX_DATA PDU %d: RNTI: 0x%04x, HARQ PID: %d, PDU Length: %d", i, incoming_rx_data_ind->pdu_list[i].rnti, incoming_rx_data_ind->pdu_list[i].harq_id, incoming_rx_data_ind->pdu_list[i].pdu_length);
        }

        // increment counter to the number of RX_DATA indication received
        uint8_t *rx_data_ind_count = &proxy_info.uplink_fapi_info[incoming_rx_data_ind->sfn][incoming_rx_data_ind->slot].rx_ind_count;
        (*rx_data_ind_count)++;
        log_trace("RX_DATA_INDICATION count: %d", *rx_data_ind_count);

        // get the expected number of RX_DATA indication
        uint8_t *rx_ind_expected = &proxy_info.uplink_fapi_info[incoming_rx_data_ind->sfn][incoming_rx_data_ind->slot].rx_ind_expected;
        log_trace("RX_DATA_INDICATION expected from %d PNFs", *rx_ind_expected);

        // No indication expected for this slot - discard to avoid stale state
        if (*rx_ind_expected == 0)
        {
            log_warn("RX_DATA_INDICATION for SFN %d Slot %d: expected=0 but received (count=%d), discarding", incoming_rx_data_ind->sfn, incoming_rx_data_ind->slot, *rx_data_ind_count);
            free_rx_data_indication(incoming_rx_data_ind);
            free(incoming_rx_data_ind);
            *rx_data_ind_count = 0;
        }
        // if we only expect messages coming from one PNF, there is no need to go through the merge logic
        else if (*rx_ind_expected == 1)
        {
            outgoing_msg = incoming_rx_data_ind;
            *rx_data_ind_count = 0;
            *rx_ind_expected = 0;
        }
        // if there are messages coming from more than one PNF, we need to merge them as one
        else
        {
            // allocate memory to buffer the RX_DATA indication
            proxy_info.uplink_fapi_info[incoming_rx_data_ind->sfn][incoming_rx_data_ind->slot].rx_buf[pnf_index] = calloc(1, sizeof(nfapi_nr_rx_data_indication_t));

            // copy RX_DATA indication to buffer
            copy_rx_data_indication(incoming_rx_data_ind, proxy_info.uplink_fapi_info[incoming_rx_data_ind->sfn][incoming_rx_data_ind->slot].rx_buf[pnf_index]);

            // once we have received all RX_DATA indications from all expected PNFs, we can proceed
            if (*rx_data_ind_count == *rx_ind_expected)
            {
                outgoing_msg = calloc(1, sizeof(nfapi_nr_rx_data_indication_t));
                ret = process_rx_data_indication(incoming_rx_data_ind, (nfapi_nr_rx_data_indication_t *)outgoing_msg, MAX_NUM_PNF, proxy_info.uplink_fapi_info[incoming_rx_data_ind->sfn][incoming_rx_data_ind->slot].rx_buf);
                if (ret < 0)
                {
                    log_error("Failed to process RX_DATA_INDICATION message");
                    free(outgoing_msg);
                    outgoing_msg = NULL;
                    return -1;
                }
                for (int i = 0; i < MAX_NUM_PNF; i++)
                {
                    if (proxy_info.uplink_fapi_info[incoming_rx_data_ind->sfn][incoming_rx_data_ind->slot].rx_buf[i] != NULL)
                    {
                        free_rx_data_indication(proxy_info.uplink_fapi_info[incoming_rx_data_ind->sfn][incoming_rx_data_ind->slot].rx_buf[i]);
                        free(proxy_info.uplink_fapi_info[incoming_rx_data_ind->sfn][incoming_rx_data_ind->slot].rx_buf[i]);
                        proxy_info.uplink_fapi_info[incoming_rx_data_ind->sfn][incoming_rx_data_ind->slot].rx_buf[i] = NULL;
                    }
                }
                *rx_data_ind_count = 0;
                *rx_ind_expected = 0;
            }
        }
        break;
#else
    case NFAPI_NR_PHY_MSG_TYPE_CRC_INDICATION:
    {
        struct_size = sizeof(nfapi_nr_crc_indication_t);
        
        // Peek SFN and Slot from the raw buffer (after P7 header)
        uint16_t sfn;
        uint16_t slot;
        peek_nr_nfapi_p7_sfn_slot(buffer, message_size, &sfn, &slot);
        int num_crcs = peek_p7_pdu_count(buffer, message_size, NFAPI_NR_PHY_MSG_TYPE_CRC_INDICATION);
        log_debug("CRC_INDICATION SFN: %d, Slot: %d, NumCrcs: %d from PNF %d", sfn, slot, num_crcs, pnf_index);

        // increment counter to the number of CRC indication received
        uint8_t *crc_ind_count = &proxy_info.uplink_fapi_info[sfn][slot].crc_ind_count;
        (*crc_ind_count)++;
        log_trace("CRC_INDICATION count: %d", *crc_ind_count);

        // get the expected number of CRC indication
        uint8_t *crc_ind_expected = &proxy_info.uplink_fapi_info[sfn][slot].crc_ind_expected;

        // No indication expected for this slot - discard to avoid stale state
        if (*crc_ind_expected == 0)
        {
            log_warn("CRC_INDICATION for SFN %d Slot %d: expected=0 but received (count=%d), discarding", sfn, slot, *crc_ind_count);
            *crc_ind_count = 0;
        }
        // if we only expect messages coming from one PNF, there is no need to go through the merge logic
        else if (*crc_ind_expected == 1)
        {
            // Forward the raw buffer directly (no unpack/repack needed)
            nfapi_nr_p7_update_checksum(buffer, message_size);
            ret = send(proxy_info.p7_north_sock, buffer, message_size, 0);
            if (ret < 0)
            {
                log_error("Failed to send CRC_INDICATION to VNF");
                return -1;
            }
            if (benchmark_admitted)
                microbenchmark_log_event("SOUTH", benchmark_type, benchmark_sfn,
                                         benchmark_slot, pnf_index, "DEPART");
            *crc_ind_count = 0;
            *crc_ind_expected = 0;
        }
        // if there are messages coming from more than one PNF, use segmented merge
        else
        {   
            log_debug("Received CRC_INDICATION SFN: %d, Slot: %d from PNF %d (count=%d/%d), buffering for merge", sfn, slot, pnf_index, *crc_ind_count, *crc_ind_expected);
            // buffer the first received CRC indication raw buffer for this SFN/Slot/PNF index
            if(*crc_ind_count == 1)
            {
                for(int i=0; i < MAX_NUM_PNF; i++) {
                    proxy_info.uplink_fapi_info[sfn][slot].crc_raw_buf[i] = malloc(MAX_MSG_SIZE);
                }
                memcpy(proxy_info.uplink_fapi_info[sfn][slot].crc_raw_buf[pnf_index], buffer, message_size);
                proxy_info.uplink_fapi_info[sfn][slot].crc_raw_buf_size[pnf_index] = message_size;
                log_trace("Buffered CRC_INDICATION from PNF %d for SFN %d Slot %d (size=%d)", pnf_index, sfn, slot, message_size);
            }
            // once we have received all CRC indications from all expected PNFs, we can proceed
            else if (*crc_ind_count == *crc_ind_expected)
            {
                memcpy(proxy_info.uplink_fapi_info[sfn][slot].crc_raw_buf[pnf_index], buffer, message_size);
                proxy_info.uplink_fapi_info[sfn][slot].crc_raw_buf_size[pnf_index] = message_size;
                log_trace("Buffered CRC_INDICATION from PNF %d for SFN %d Slot %d (size=%d), all expected CRC_INDICATIONs received, proceeding to merge", pnf_index, sfn, slot, message_size);

                uint8_t *first_buf;
                uint32_t first_size;
                uint8_t *second_buf;
                uint32_t second_size;

                first_buf = proxy_info.uplink_fapi_info[sfn][slot].crc_raw_buf[0];
                first_size = proxy_info.uplink_fapi_info[sfn][slot].crc_raw_buf_size[0];
                second_buf = proxy_info.uplink_fapi_info[sfn][slot].crc_raw_buf[1];
                second_size = proxy_info.uplink_fapi_info[sfn][slot].crc_raw_buf_size[1];
                
                if (first_buf != NULL && second_buf != NULL)
                {
                    // Create segmented merge
                    uint8_t seq_num = proxy_info.p7_uplink_sequence_num++;
                    
                    ret = create_segmented_uplink_merge(
                        first_buf, &first_size,
                        second_buf, &second_size,
                        NFAPI_NR_PHY_MSG_TYPE_CRC_INDICATION, seq_num);

                    log_debug("Created segmented CRC_INDICATION merge for SFN %d Slot %d, segment 0 size: %d, segment 1 size: %d", sfn, slot, first_size, second_size);
                    // log the number of pdus in segment0
                    int num_pdus_segment0 = peek_p7_pdu_count(first_buf, first_size, NFAPI_NR_PHY_MSG_TYPE_CRC_INDICATION);
                    log_debug("Segmented CRC_INDICATION merge for SFN %d Slot %d, segment 0 NumPdus: %d", sfn, slot, num_pdus_segment0);
                    
                    if (ret == 0)
                    {
                        // Update checksums and send both segments
                        nfapi_nr_p7_update_checksum(first_buf, first_size);
                        ret = send(proxy_info.p7_north_sock, first_buf, first_size, 0);
                        if (ret < 0)
                        {
                            log_error("Failed to send CRC_INDICATION segment 0");
                        }
                        
                        nfapi_nr_p7_update_checksum(second_buf, second_size);
                        ret = send(proxy_info.p7_north_sock, second_buf, second_size, 0);
                        if (ret < 0)
                        {
                            log_error("Failed to send CRC_INDICATION segment 1");
                        }
                        else if (benchmark_admitted)
                            microbenchmark_log_event("SOUTH", benchmark_type,
                                                     benchmark_sfn, benchmark_slot,
                                                     pnf_index, "DEPART");
                        
                        log_debug("Sent CRC_INDICATION as 2 segments (seq=%d)", seq_num);
                    }
                    else
                    {
                        log_error("Failed to create segmented CRC_INDICATION merge");
                    }
                }
                
                // Free all raw buffers
                for (int i = 0; i < MAX_NUM_PNF; i++)
                {
                    if (proxy_info.uplink_fapi_info[sfn][slot].crc_raw_buf[i] != NULL)
                    {
                        free(proxy_info.uplink_fapi_info[sfn][slot].crc_raw_buf[i]);
                        proxy_info.uplink_fapi_info[sfn][slot].crc_raw_buf[i] = NULL;
                        proxy_info.uplink_fapi_info[sfn][slot].crc_raw_buf_size[i] = 0;
                    }
                }
                *crc_ind_count = 0;
                *crc_ind_expected = 0;
            }
        }
        break;
    }
    case NFAPI_NR_PHY_MSG_TYPE_RX_DATA_INDICATION:
    {
        struct_size = sizeof(nfapi_nr_rx_data_indication_t);
        
        // Peek SFN and Slot from the raw buffer (after P7 header)
        uint16_t sfn;
        uint16_t slot;
        peek_nr_nfapi_p7_sfn_slot(buffer, message_size, &sfn, &slot);
        int num_pdus = peek_p7_pdu_count(buffer, message_size, NFAPI_NR_PHY_MSG_TYPE_RX_DATA_INDICATION);
        log_debug("RX_DATA_INDICATION SFN: %d, Slot: %d, NumPdus: %d from PNF %d", sfn, slot, num_pdus, pnf_index);

        // increment counter to the number of RX_DATA indication received
        uint8_t *rx_data_ind_count = &proxy_info.uplink_fapi_info[sfn][slot].rx_ind_count;
        (*rx_data_ind_count)++;
        log_trace("RX_DATA_INDICATION count: %d", *rx_data_ind_count);

        // get the expected number of RX_DATA indication
        uint8_t *rx_ind_expected = &proxy_info.uplink_fapi_info[sfn][slot].rx_ind_expected;

        // No indication expected for this slot - discard to avoid stale state
        if (*rx_ind_expected == 0)
        {
            log_warn("RX_DATA_INDICATION for SFN %d Slot %d: expected=0 but received (count=%d), discarding", sfn, slot, *rx_data_ind_count);
            *rx_data_ind_count = 0;
        }
        // if we only expect messages coming from one PNF, there is no need to go through the merge logic
        else if (*rx_ind_expected == 1)
        {
            // Forward the raw buffer directly (no unpack/repack needed)
            nfapi_nr_p7_update_checksum(buffer, message_size);
            ret = send(proxy_info.p7_north_sock, buffer, message_size, 0);
            if (ret < 0)
            {
                log_error("Failed to send RX_DATA_INDICATION to VNF");
                return -1;
            }
            if (benchmark_admitted)
                microbenchmark_log_event("SOUTH", benchmark_type, benchmark_sfn,
                                         benchmark_slot, pnf_index, "DEPART");
            *rx_data_ind_count = 0;
            *rx_ind_expected = 0;
        }
        // if there are messages coming from more than one PNF, use segmented merge
        else
        {
            log_debug("Received RX_DATA_INDICATION SFN: %d, Slot: %d from PNF %d (count=%d/%d), buffering for merge", sfn, slot, pnf_index, *rx_data_ind_count, *rx_ind_expected);
            // buffer the first received RX_DATA indication raw buffer for this SFN/Slot/PNF index
            if(*rx_data_ind_count == 1)
            {
                for(int i=0; i<MAX_NUM_PNF; i++) {
                    proxy_info.uplink_fapi_info[sfn][slot].rx_raw_buf[i] = malloc(MAX_MSG_SIZE);
                }
                memcpy(proxy_info.uplink_fapi_info[sfn][slot].rx_raw_buf[pnf_index], buffer, message_size);
                proxy_info.uplink_fapi_info[sfn][slot].rx_raw_buf_size[pnf_index] = message_size;
                log_trace("Buffered RX_DATA_INDICATION from PNF %d for SFN %d Slot %d (size=%d)", pnf_index, sfn, slot, message_size);
            }
            // once we have received all RX_DATA indications from all expected PNFs, we can proceed
            else if (*rx_data_ind_count == *rx_ind_expected)
            {
                memcpy(proxy_info.uplink_fapi_info[sfn][slot].rx_raw_buf[pnf_index], buffer, message_size);
                proxy_info.uplink_fapi_info[sfn][slot].rx_raw_buf_size[pnf_index] = message_size;
                log_trace("Buffered RX_DATA_INDICATION from PNF %d for SFN %d Slot %d (size=%d)", pnf_index, sfn, slot, message_size);
                // For segmented merge: PNF0 becomes segment 0 (first segment), PNF1 becomes segment 1
                // In create_segmented_uplink_merge: second_buf -> segment 0, first_buf -> segment 1
                // So we assign: first_buf = PNF1, second_buf = PNF0
                uint8_t *first_buf;
                uint32_t first_size;
                uint8_t *second_buf;
                uint32_t second_size;

                first_buf = proxy_info.uplink_fapi_info[sfn][slot].rx_raw_buf[0];
                first_size = proxy_info.uplink_fapi_info[sfn][slot].rx_raw_buf_size[0];
                second_buf = proxy_info.uplink_fapi_info[sfn][slot].rx_raw_buf[1];
                second_size = proxy_info.uplink_fapi_info[sfn][slot].rx_raw_buf_size[1];
                
                if (first_buf != NULL && second_buf != NULL)
                {
                    // Create segmented merge
                    uint8_t seq_num = proxy_info.p7_uplink_sequence_num++;
                    
                    ret = create_segmented_uplink_merge(
                        first_buf, &first_size,
                        second_buf, &second_size,
                        NFAPI_NR_PHY_MSG_TYPE_RX_DATA_INDICATION, seq_num);

                    log_debug("Created segmented merge for RX_DATA_INDICATION SFN %d Slot %d, seq_num=%d, segment0_size=%d, segment1_size=%d", sfn, slot, seq_num, first_size, second_size);
                    // peek the number of PDUs recorded in segment0
                    int segment0_num_pdus = peek_p7_pdu_count(first_buf, first_size, NFAPI_NR_PHY_MSG_TYPE_RX_DATA_INDICATION);
                    log_debug("Segment 0 for RX_DATA_INDICATION SFN %d Slot %d contains %d PDUs", sfn, slot, segment0_num_pdus);
                    
                    if (ret == 0)
                    {
                        // Update checksums and send both segments
                        nfapi_nr_p7_update_checksum(first_buf, first_size);
                        ret = send(proxy_info.p7_north_sock, first_buf, first_size, 0);
                        if (ret < 0)
                        {
                            log_error("Failed to send RX_DATA_INDICATION segment 0");
                        }
                        
                        nfapi_nr_p7_update_checksum(second_buf, second_size);
                        ret = send(proxy_info.p7_north_sock, second_buf, second_size, 0);
                        if (ret < 0)
                        {
                            log_error("Failed to send RX_DATA_INDICATION segment 1");
                        }
                        else if (benchmark_admitted)
                            microbenchmark_log_event("SOUTH", benchmark_type,
                                                     benchmark_sfn, benchmark_slot,
                                                     pnf_index, "DEPART");
                        
                        log_debug("Sent RX_DATA_INDICATION as 2 segments (seq=%d)", seq_num);
                    }
                    else
                    {
                        log_error("Failed to create segmented RX_DATA_INDICATION merge");
                    }
                }
                
                // Free all raw buffers
                for (int i = 0; i < MAX_NUM_PNF; i++)
                {
                    if (proxy_info.uplink_fapi_info[sfn][slot].rx_raw_buf[i] != NULL)
                    {
                        free(proxy_info.uplink_fapi_info[sfn][slot].rx_raw_buf[i]);
                        proxy_info.uplink_fapi_info[sfn][slot].rx_raw_buf[i] = NULL;
                        proxy_info.uplink_fapi_info[sfn][slot].rx_raw_buf_size[i] = 0;
                    }
                }
                *rx_data_ind_count = 0;
                *rx_ind_expected = 0;
            }
        }
        break;
    }
#endif
    case NFAPI_NR_PHY_MSG_TYPE_TIMING_INFO:
        struct_size = sizeof(nfapi_nr_timing_info_t);
        nfapi_nr_timing_info_t *incoming_timing_info = calloc(1, sizeof(nfapi_nr_timing_info_t));
        nfapi_nr_p7_message_unpack(buffer, message_size, incoming_timing_info, sizeof(*incoming_timing_info), 0);
        outgoing_msg = incoming_timing_info;
        break;
    default:
        log_warn("Received unknown message, ID: 0x%02x", messageHeader.message_id);
        break;
    }

    if (outgoing_msg)
    {
        // pack the mssage for VNF
        write_buffer_size = nfapi_nr_p7_message_pack(outgoing_msg, write_buffer, sizeof(write_buffer), 0);
        ret = nfapi_nr_p7_update_checksum(write_buffer, write_buffer_size);
        if (ret < 0)
        {
            log_error("Failed to update checksum for message to VNF");
            free(outgoing_msg);
            outgoing_msg = NULL;
            return -1;
        }
        if (write_buffer_size <= 0)
        {
            log_error("Failed to pack message for VNF");
            free(outgoing_msg);
            outgoing_msg = NULL;
            return -1;
        }

        // execute send for the outgoing message
        ret = send(proxy_info.p7_north_sock, write_buffer, write_buffer_size, 0);
        if (ret < 0)
        {
            log_error("send");
            free(outgoing_msg);
            outgoing_msg = NULL;
            return -1;
        }
        if (benchmark_admitted)
            microbenchmark_log_event("SOUTH", benchmark_type, benchmark_sfn,
                                     benchmark_slot, pnf_index, "DEPART");
        // free the earlier allocated memory
        free(outgoing_msg);
        outgoing_msg = NULL;
    }
    return 0;
}

void nfapi_p7_thread(void)
{
    log_info("Starting P7 processing thread");
    struct sched_param sp = {.sched_priority = 90};
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    while (!terminate)
    {
        fd_set p7_fdset;
        FD_ZERO(&p7_fdset);
        int north_sock;
        int south_sock[MAX_NUM_PNF];
        int max_fd = -1;

        pthread_mutex_lock(&proxy_info.membership_mutex);
        north_sock = proxy_info.p7_north_sock;
        if (north_sock != -1)
        {
            FD_SET(north_sock, &p7_fdset);
            max_fd = north_sock;
        }
        for (int i = 0; i < MAX_NUM_PNF; i++)
        {
            south_sock[i] = proxy_info.p7_south_sock[i];
            if (south_sock[i] != -1)
            {
                FD_SET(south_sock[i], &p7_fdset);
                if (south_sock[i] > max_fd) max_fd = south_sock[i];
            }
        }
        pthread_mutex_unlock(&proxy_info.membership_mutex);

        struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
        int n = select(max_fd + 1, &p7_fdset, NULL, NULL, &timeout);
        if (n < 0)
        {
            if (errno == EINTR || errno == EBADF) continue;
            log_error("P7 select: %s", strerror(errno));
            break;
        }
        if (n == 0) continue;

        pthread_mutex_lock(&proxy_info.membership_mutex);
        if (north_sock != -1 && north_sock == (int)proxy_info.p7_north_sock && FD_ISSET(north_sock, &p7_fdset))
        {
            if (nfapi_p7_process_north(north_sock) < 0)
                log_warn("Dropping invalid P7 message from VNF");
        }
        for (int i = 0; i < MAX_NUM_PNF; i++)
        {
            if (south_sock[i] != -1 && south_sock[i] == (int)proxy_info.p7_south_sock[i] && FD_ISSET(south_sock[i], &p7_fdset))
            {
                if (nfapi_p7_process_south(south_sock[i], i) < 0)
                    log_warn("Dropping invalid P7 message from PNF %d", i);
            }
        }
        pthread_mutex_unlock(&proxy_info.membership_mutex);
    }
    log_info("Exiting P7 processing thread");
}

int nfapi_p5_process_north(int sock)
{
    uint32_t stack_buffer_size = 32;
    uint8_t stack_buffer[stack_buffer_size];
    uint8_t *dynamic_buffer = 0;

    uint8_t *read_buffer = &stack_buffer[0];
    int message_size = 0;

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    struct sctp_sndrcvinfo sndrcvinfo;
    (void)memset(&sndrcvinfo, 0, sizeof(struct sctp_sndrcvinfo));

    message_size = peek_p5_sctp_message_size(sock, &addr, &addr_len, &sndrcvinfo);
    if (message_size < 0)
    {
        return -1;
    }
    if (message_size > stack_buffer_size)
    {
        dynamic_buffer = (uint8_t *)malloc(message_size);
        read_buffer = dynamic_buffer;
    }
    int ret = read_p5_sctp_message(sock, read_buffer, message_size, &addr, &addr_len, &sndrcvinfo);
    if (ret < 0)
    {
        if (dynamic_buffer)
        {
            free(dynamic_buffer);
            dynamic_buffer = 0;
        }
        return -1;
    }

    nfapi_nr_p4_p5_message_header_t messageHeader;
    const bool result = nfapi_nr_p5_message_header_unpack(read_buffer, message_size, &messageHeader, sizeof(messageHeader), 0);
    if (!result)
    {
        log_error("Failed to unpack P5 message header\n");
        return -1;
    }

    switch (messageHeader.message_id)
    {
    case NFAPI_NR_PHY_MSG_TYPE_PNF_PARAM_REQUEST:
        log_debug("Received PNF_PARAM_REQUEST message, ID: 0x%02x", messageHeader.message_id);
        if (!vnf_p5_cache.pnf_param_request_received)
        {
            nfapi_nr_p5_message_unpack(read_buffer, message_size, &vnf_p5_cache.pnf_param_request, sizeof(vnf_p5_cache.pnf_param_request), 0);
            vnf_p5_cache.pnf_param_request_received = true;
        }
        proxy_info.p5_north_sock_state = P5_STATE_PNF_PARAM_REQUEST;
        break;
    case NFAPI_NR_PHY_MSG_TYPE_PNF_CONFIG_REQUEST:
        log_debug("Received PNF_CONFIG_REQUEST message, ID: 0x%02x", messageHeader.message_id);
        if (!vnf_p5_cache.pnf_config_request_received)
        {
            nfapi_nr_p5_message_unpack(read_buffer, message_size, &vnf_p5_cache.pnf_config_request, sizeof(vnf_p5_cache.pnf_config_request), 0);
            vnf_p5_cache.pnf_config_request_received = true;
        }
        proxy_info.p5_north_sock_state = P5_STATE_PNF_CONFIG_REQUEST;
        break;
    case NFAPI_NR_PHY_MSG_TYPE_PNF_START_REQUEST:
        log_debug("Received PNF_START_REQUEST message, ID: 0x%02x", messageHeader.message_id);
        if (!vnf_p5_cache.pnf_start_request_received)
        {
            nfapi_nr_p5_message_unpack(read_buffer, message_size, &vnf_p5_cache.pnf_start_request, sizeof(vnf_p5_cache.pnf_start_request), 0);
            vnf_p5_cache.pnf_start_request_received = true;
        }
        proxy_info.p5_north_sock_state = P5_STATE_PNF_START_REQUEST;
        break;
    case NFAPI_NR_PHY_MSG_TYPE_PARAM_REQUEST:
        log_debug("Received PARAM_REQUEST message, ID: 0x%02x", messageHeader.message_id);
        if (!vnf_p5_cache.param_request_received)
        {
            nfapi_nr_p5_message_unpack(read_buffer, message_size, &vnf_p5_cache.param_request, sizeof(vnf_p5_cache.param_request), 0);
            vnf_p5_cache.param_request_received = true;
        }
        proxy_info.p5_north_sock_state = P5_STATE_PARAM_REQUEST;
        break;
    case NFAPI_NR_PHY_MSG_TYPE_CONFIG_REQUEST:
        log_debug("Received CONFIG_REQUEST message, ID: 0x%02x", messageHeader.message_id);
        if (!vnf_p5_cache.config_request_received)
        {
            nfapi_nr_p5_message_unpack(read_buffer, message_size, &vnf_p5_cache.config_request, sizeof(vnf_p5_cache.config_request), 0);
            // Track the VNF P7 port, and use it to create connected UDP socket with the VNF
            // BUG? Inconsistent endianness handling in nfapi?
            // VNF puts it in host order, but PNF puts it in network order
            proxy_info.p7_vnf_udp_port = vnf_p5_cache.config_request.nfapi_config.p7_vnf_port.value;
            // Create the connected UDP socket to the VNF
            struct sockaddr_in vnf_addr;
            memset(&vnf_addr, 0, sizeof(vnf_addr));
            vnf_addr.sin_family = AF_INET;
            vnf_addr.sin_port = htons(proxy_info.p7_vnf_udp_port);
            if (inet_pton(AF_INET, "127.0.0.1", &vnf_addr.sin_addr) <= 0)
            {
                log_error("inet_pton");
                return -1;
            }
            struct sockaddr_in proxy_addr;
            memset(&proxy_addr, 0, sizeof(proxy_addr));
            proxy_addr.sin_family = AF_INET;
            proxy_addr.sin_port = htons(proxy_info.p7_udp_port);
            if (inet_pton(AF_INET, "127.0.0.1", &proxy_addr.sin_addr) <= 0)
            {
                log_error("inet_pton");
                return -1;
            }
            proxy_info.p7_north_sock = create_p7_connected_udp_socket(&proxy_addr, &vnf_addr);
            if (proxy_info.p7_north_sock < 0)
            {
                log_error("Failed to create P7 connected UDP socket to VNF");
                return -1;
            }
            if (proxy_info.p7_north_sock > proxy_info.p7_max_fd)
            {
                proxy_info.p7_max_fd = proxy_info.p7_north_sock;
            }
            log_info("Created P7 connected UDP socket to VNF on source port %d and destination port %d", proxy_info.p7_vnf_udp_port, proxy_info.p7_udp_port);
            log_info("Created P7 connected UDP socket to VNF at %d", proxy_info.p7_north_sock);
            vnf_p5_cache.config_request_received = true;
        }
        proxy_info.p5_north_sock_state = P5_STATE_CONFIG_REQUEST;
        break;
    case NFAPI_NR_PHY_MSG_TYPE_START_REQUEST:
        log_debug("Received START_REQUEST message, ID: 0x%02x", messageHeader.message_id);
        if (!vnf_p5_cache.start_request_received)
        {
            nfapi_nr_p5_message_unpack(read_buffer, message_size, &vnf_p5_cache.start_request, sizeof(vnf_p5_cache.start_request), 0);
            vnf_p5_cache.start_request_received = true;
        }
        proxy_info.p5_north_sock_state = P5_STATE_START_REQUEST;
        break;
    default:
        log_warn("Received unknown message, ID: 0x%02x", messageHeader.message_id);
        break;
    }
    return 0;
}

int nfapi_p5_process_south(int sock, int pnf_index)
{
    uint32_t stack_buffer_size = 32;
    uint8_t stack_buffer[stack_buffer_size];
    uint8_t *dynamic_buffer = 0;

    uint8_t *read_buffer = &stack_buffer[0];
    int message_size = 0;

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    struct sctp_sndrcvinfo sndrcvinfo;
    (void)memset(&sndrcvinfo, 0, sizeof(struct sctp_sndrcvinfo));

    message_size = peek_p5_sctp_message_size(sock, &addr, &addr_len, &sndrcvinfo);
    if (message_size < 0)
    {
        return -1;
    }
    if (message_size > stack_buffer_size)
    {
        dynamic_buffer = (uint8_t *)malloc(message_size);
        read_buffer = dynamic_buffer;
    }
    int ret = read_p5_sctp_message(sock, read_buffer, message_size, &addr, &addr_len, &sndrcvinfo);
    if (ret < 0)
    {
        if (dynamic_buffer)
        {
            free(dynamic_buffer);
            dynamic_buffer = 0;
        }
        return -1;
    }

    nfapi_nr_p4_p5_message_header_t messageHeader;
    const bool result = nfapi_nr_p5_message_header_unpack(read_buffer, message_size, &messageHeader, sizeof(messageHeader), 0);
    if (!result)
    {
        log_error("Failed to unpack P5 message header");
        return -1;
    }

    switch (messageHeader.message_id)
    {
    case NFAPI_NR_PHY_MSG_TYPE_PNF_PARAM_RESPONSE:
        if (!pnf_p5_cache.pnf_param_response_received)
        {
            pnf_p5_cache.pnf_param_response_received = true;
            nfapi_nr_p5_message_unpack(read_buffer, message_size, &pnf_p5_cache.pnf_param_response, sizeof(pnf_p5_cache.pnf_param_response), 0);
        }
        proxy_info.p5_south_sock_state[pnf_index] = P5_STATE_PNF_PARAM_RESPONSE;
        log_debug("Received PNF_PARAM_RESPONSE message, ID: 0x%02x", messageHeader.message_id);
        break;
    case NFAPI_NR_PHY_MSG_TYPE_PNF_CONFIG_RESPONSE:
        if (!pnf_p5_cache.pnf_config_response_received)
        {
            pnf_p5_cache.pnf_config_response_received = true;
            nfapi_nr_p5_message_unpack(read_buffer, message_size, &pnf_p5_cache.pnf_config_response, sizeof(pnf_p5_cache.pnf_config_response), 0);
        }
        proxy_info.p5_south_sock_state[pnf_index] = P5_STATE_PNF_CONFIG_RESPONSE;
        log_debug("Received PNF_CONFIG_RESPONSE message, ID: 0x%02x", messageHeader.message_id);
        break;
    case NFAPI_NR_PHY_MSG_TYPE_PNF_START_RESPONSE:
        if (!pnf_p5_cache.pnf_start_response_received)
        {
            pnf_p5_cache.pnf_start_response_received = true;
            nfapi_nr_p5_message_unpack(read_buffer, message_size, &pnf_p5_cache.pnf_start_response, sizeof(pnf_p5_cache.pnf_start_response), 0);
        }
        proxy_info.p5_south_sock_state[pnf_index] = P5_STATE_PNF_START_RESPONSE;
        log_debug("Received PNF_START_RESPONSE message, ID: 0x%02x", messageHeader.message_id);
        break;
    case NFAPI_NR_PHY_MSG_TYPE_PARAM_RESPONSE:
        nfapi_nr_param_response_scf_t param_resp;
        memset(&param_resp, 0, sizeof(param_resp));
        nfapi_nr_p5_message_unpack(read_buffer, message_size, &param_resp, sizeof(param_resp), 0);
        // Track the PNF P7 port, and use it to create connected UDP socket with the PNF
        // BUG? Inconsistent endianness handling in nfapi?
        // PNF puts it in network order, but VNF puts it in host order
        proxy_info.p7_pnf_udp_port[pnf_index] = ntohs(param_resp.nfapi_config.p7_pnf_port.value);
        // Create the connected UDP socket to the PNF
        struct sockaddr_in pnf_addr;
        memset(&pnf_addr, 0, sizeof(pnf_addr));
        pnf_addr.sin_family = AF_INET;
        pnf_addr.sin_port = htons(proxy_info.p7_pnf_udp_port[pnf_index]);
        if (inet_pton(AF_INET, "127.0.0.1", &pnf_addr.sin_addr) <= 0)
        {
            log_error("inet_pton");
            return -1;
        }
        struct sockaddr_in proxy_addr;
        memset(&proxy_addr, 0, sizeof(proxy_addr));
        proxy_addr.sin_family = AF_INET;
        proxy_addr.sin_port = htons(proxy_info.p7_udp_port);
        if (inet_pton(AF_INET, "127.0.0.1", &proxy_addr.sin_addr) <= 0)
        {
            log_error("inet_pton");
            return -1;
        }
        proxy_info.p7_south_sock[pnf_index] = create_p7_connected_udp_socket(&proxy_addr, &pnf_addr);
        log_debug("Creating P7 connected UDP socket to PNF %d on port %d with socket %d", pnf_index, proxy_info.p7_pnf_udp_port[pnf_index], proxy_info.p7_south_sock[pnf_index]);
        if (proxy_info.p7_south_sock[pnf_index] < 0)
        {
            log_error("Failed to create P7 connected UDP socket to PNF %d", pnf_index);
            return -1;
        }
        if (proxy_info.p7_south_sock[pnf_index] > proxy_info.p7_max_fd)
        {
            proxy_info.p7_max_fd = proxy_info.p7_south_sock[pnf_index];
        }
        log_info("Created P7 connected UDP socket to PNF %d on port %d", pnf_index, proxy_info.p7_pnf_udp_port[pnf_index]);
        log_info("Created P7 connected UDP socket to PNF at %d", proxy_info.p7_south_sock[pnf_index]);

        if (!pnf_p5_cache.param_response_received)
        {
            pnf_p5_cache.param_response_received = true;
            memcpy(&pnf_p5_cache.param_response, &param_resp, sizeof(param_resp));
        }
        proxy_info.p5_south_sock_state[pnf_index] = P5_STATE_PARAM_RESPONSE;
        log_debug("Received PARAM_RESPONSE message, ID: 0x%02x", messageHeader.message_id);
        break;
    case NFAPI_NR_PHY_MSG_TYPE_CONFIG_RESPONSE:
        if (!pnf_p5_cache.config_response_received)
        {
            pnf_p5_cache.config_response_received = true;
            nfapi_nr_p5_message_unpack(read_buffer, message_size, &pnf_p5_cache.config_response, sizeof(pnf_p5_cache.config_response), 0);
        }
        proxy_info.p5_south_sock_state[pnf_index] = P5_STATE_CONFIG_RESPONSE;
        log_debug("Received CONFIG_RESPONSE message, ID: 0x%02x", messageHeader.message_id);
        break;
    case NFAPI_NR_PHY_MSG_TYPE_START_RESPONSE:
        if (!pnf_p5_cache.start_response_received)
        {
            pnf_p5_cache.start_response_received = true;
            nfapi_nr_p5_message_unpack(read_buffer, message_size, &pnf_p5_cache.start_response, sizeof(pnf_p5_cache.start_response), 0);
        }
        proxy_info.p5_south_sock_state[pnf_index] = P5_STATE_START_RESPONSE;
        log_debug("Received START_RESPONSE message, ID: 0x%02x", messageHeader.message_id);
        break;
    case NFAPI_NR_PHY_MSG_TYPE_STOP_INDICATION:
        log_info("Received STOP_INDICATION from PNF %d; removing L1 gracefully", pnf_index);
        free(dynamic_buffer);
        disconnect_pnf_locked(pnf_index, "P5 STOP.indication");
        return 0;
    default:
        log_warn("Received unknown message, ID: 0x%02x", messageHeader.message_id);
        break;
    }
    free(dynamic_buffer);
    return 0;
}

int nfapi_send_p5_north_to_south(void)
{
    for (int i = 0; i < MAX_NUM_PNF; i++)
    {
        if (proxy_info.p5_south_sock[i] == -1)
        {
            continue;
        }

        uint8_t write_buffer[65536];
        int message_size = 0;

        uint32_t curr_p5_south_sock_state = proxy_info.p5_south_sock_state[i];

        log_debug("PNF %d current south state: %d, north state: %d", i, curr_p5_south_sock_state, proxy_info.p5_north_sock_state);

        if (proxy_info.p5_north_sock_state > curr_p5_south_sock_state)
        {
            // forward message to south
            switch (curr_p5_south_sock_state + 1)
            {
            case P5_STATE_PNF_PARAM_REQUEST:
                log_debug("Forwarding PNF_PARAM_REQUEST to PNF %d", i);
                message_size = nfapi_nr_p5_message_pack(&vnf_p5_cache.pnf_param_request, sizeof(vnf_p5_cache.pnf_param_request), write_buffer, sizeof(write_buffer), 0);
                break;
            case P5_STATE_PNF_CONFIG_REQUEST:
                log_debug("Forwarding PNF_CONFIG_REQUEST to PNF %d", i);
                message_size = nfapi_nr_p5_message_pack(&vnf_p5_cache.pnf_config_request, sizeof(vnf_p5_cache.pnf_config_request), write_buffer, sizeof(write_buffer), 0);
                break;
            case P5_STATE_PNF_START_REQUEST:
                log_debug("Forwarding PNF_START_REQUEST to PNF %d", i);
                message_size = nfapi_nr_p5_message_pack(&vnf_p5_cache.pnf_start_request, sizeof(vnf_p5_cache.pnf_start_request), write_buffer, sizeof(write_buffer), 0);
                break;
            case P5_STATE_PARAM_REQUEST:
                log_debug("Forwarding PARAM_REQUEST to PNF %d", i);
                message_size = nfapi_nr_p5_message_pack(&vnf_p5_cache.param_request, sizeof(vnf_p5_cache.param_request), write_buffer, sizeof(write_buffer), 0);
                break;
            case P5_STATE_CONFIG_REQUEST:
                log_debug("Forwarding CONFIG_REQUEST to PNF %d", i);
                // Override the VNF P7 port to the proxy's P7 port
                // BUG? inconsistent endianness handling in nfapi?
                // VNF puts it in host order, but PNF puts it in network order
                vnf_p5_cache.config_request.nfapi_config.p7_vnf_port.value = proxy_info.p7_udp_port;
                message_size = nfapi_nr_p5_message_pack(&vnf_p5_cache.config_request, sizeof(vnf_p5_cache.config_request), write_buffer, sizeof(write_buffer), 0);
                break;
            case P5_STATE_START_REQUEST:
                log_debug("Forwarding START_REQUEST to PNF %d", i);
                message_size = nfapi_nr_p5_message_pack(&vnf_p5_cache.start_request, sizeof(vnf_p5_cache.start_request), write_buffer, sizeof(write_buffer), 0);
                break;
            default:
                if (curr_p5_south_sock_state + 1 == P5_STATE_START_RESPONSE)
                {
                    log_debug("All P5 messages have been forwarded to PNF %d", i);
                    return 0;
                }
                break;
            }
            proxy_info.p5_south_sock_state[i] = curr_p5_south_sock_state + 1;
            int bytes_sent = sctp_sendmsg(proxy_info.p5_south_sock[i], write_buffer, message_size, NULL, 0, 0, 0, 0, 0, 0);
            if (bytes_sent < 0)
            {
                log_error("sctp_sendmsg to PNF %d failed: %s", i, strerror(errno));
                disconnect_pnf_locked(i, "P5 send failed");
                continue;
            }
        }
    }
    return 0;
}

int nfapi_send_p5_south_to_north(void)
{
    for (int i = 0; i < MAX_NUM_PNF; i++)
    {
        if (proxy_info.p5_south_sock[i] == -1)
        {
            continue;
        }

        uint8_t write_buffer[65536];
        int message_size = 0;

        log_debug("nfapi_send_p5_south_to_north: PNF %d current south state: %d, north state: %d", i, proxy_info.p5_south_sock_state[i], proxy_info.p5_north_sock_state);

        if (proxy_info.p5_south_sock_state[i] > proxy_info.p5_north_sock_state)
        {
            // forward message to north
            switch (proxy_info.p5_south_sock_state[i])
            {
            case P5_STATE_PNF_PARAM_RESPONSE:
                message_size = nfapi_nr_p5_message_pack(&pnf_p5_cache.pnf_param_response, sizeof(pnf_p5_cache.pnf_param_response), write_buffer, sizeof(write_buffer), 0);
                break;
            case P5_STATE_PNF_CONFIG_RESPONSE:
                message_size = nfapi_nr_p5_message_pack(&pnf_p5_cache.pnf_config_response, sizeof(pnf_p5_cache.pnf_config_response), write_buffer, sizeof(write_buffer), 0);
                break;
            case P5_STATE_PNF_START_RESPONSE:
                message_size = nfapi_nr_p5_message_pack(&pnf_p5_cache.pnf_start_response, sizeof(pnf_p5_cache.pnf_start_response), write_buffer, sizeof(write_buffer), 0);
                break;
            case P5_STATE_PARAM_RESPONSE:
                // Override the PNF P7 port to the proxy's P7 port
                // BUG? inconsistent endianness handling in nfapi?
                // PNF puts it in network order, but VNF puts it in host order
                pnf_p5_cache.param_response.nfapi_config.p7_pnf_port.value = htons(proxy_info.p7_udp_port);
                message_size = nfapi_nr_p5_message_pack(&pnf_p5_cache.param_response, sizeof(pnf_p5_cache.param_response), write_buffer, sizeof(write_buffer), 0);
                break;
            case P5_STATE_CONFIG_RESPONSE:
                message_size = nfapi_nr_p5_message_pack(&pnf_p5_cache.config_response, sizeof(pnf_p5_cache.config_response), write_buffer, sizeof(write_buffer), 0);
                break;
            case P5_STATE_START_RESPONSE:
                message_size = nfapi_nr_p5_message_pack(&pnf_p5_cache.start_response, sizeof(pnf_p5_cache.start_response), write_buffer, sizeof(write_buffer), 0);
                break;
            default:
                break;
            }
            proxy_info.p5_north_sock_state = proxy_info.p5_south_sock_state[i];
            int bytes_sent = sctp_sendmsg(proxy_info.p5_north_sock, write_buffer, message_size, NULL, 0, 0, 0, 0, 0, 0);
            if (bytes_sent < 0)
            {
                log_error("sctp_sendmsg to PNF");
                return -1;
            }
        }
    }
    return 0;
}

int nfapi_p5_proxy_process(void)
{
    int ret = 0;
    // north to south messages
    ret = nfapi_send_p5_north_to_south();
    if (ret < 0)
    {
        log_error("Error sending P5 message from north to south");
        return -1;
    }

    // south to north messages
    ret = nfapi_send_p5_south_to_north();
    if (ret < 0)
    {
        log_error("Error sending P5 message from south to north");
        return -1;
    }

    return 0;
}

void nfapi_p5_thread(void)
{
    log_info("Starting P5 processing thread");
    struct sched_param sp = {.sched_priority = 20};
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    while (!terminate)
    {
        fd_set p5_fdset;
        FD_ZERO(&p5_fdset);
        int north_sock;
        int south_sock[MAX_NUM_PNF];
        int max_fd = -1;

        pthread_mutex_lock(&proxy_info.membership_mutex);
        north_sock = proxy_info.p5_north_sock;
        if (north_sock != -1) { FD_SET(north_sock, &p5_fdset); max_fd = north_sock; }
        for (int i = 0; i < MAX_NUM_PNF; i++)
        {
            south_sock[i] = proxy_info.p5_south_sock[i];
            if (south_sock[i] != -1)
            {
                FD_SET(south_sock[i], &p5_fdset);
                if (south_sock[i] > max_fd) max_fd = south_sock[i];
            }
        }
        pthread_mutex_unlock(&proxy_info.membership_mutex);

        struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
        int n = select(max_fd + 1, &p5_fdset, NULL, NULL, &timeout);
        if (n < 0)
        {
            if (errno == EINTR || errno == EBADF) continue;
            log_error("P5 select: %s", strerror(errno));
            break;
        }

        pthread_mutex_lock(&proxy_info.membership_mutex);
        if (n > 0 && north_sock != -1 && north_sock == (int)proxy_info.p5_north_sock && FD_ISSET(north_sock, &p5_fdset))
        {
            if (nfapi_p5_process_north(north_sock) < 0)
            {
                pthread_mutex_unlock(&proxy_info.membership_mutex);
                log_error("VNF P5 connection was lost");
                terminate = 1;
                break;
            }
        }
        for (int i = 0; n > 0 && i < MAX_NUM_PNF; i++)
        {
            if (south_sock[i] != -1 && south_sock[i] == (int)proxy_info.p5_south_sock[i] && FD_ISSET(south_sock[i], &p5_fdset))
            {
                if (nfapi_p5_process_south(south_sock[i], i) < 0)
                    disconnect_pnf_locked(i, "P5 connection lost");
            }
        }
        if (nfapi_p5_proxy_process() < 0)
            log_warn("P5 forwarding failed; affected L1 will be removed when its socket reports the disconnect");
        pthread_mutex_unlock(&proxy_info.membership_mutex);
    }
    log_info("Exiting P5 processing thread");
}

void nfapi_p5_south_thread(void)
{
    log_info("Listening for incoming connections from PNFs");
    struct sched_param sp = {.sched_priority = 20};
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    struct sockaddr_in listen_addr = {0};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(proxy_info.p5_sctp_port);
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    int sock = create_p5_sctp_socket();
    if (sock < 0 || bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0 || listen(sock, MAX_NUM_PNF) < 0)
    {
        log_error("Failed to create P5 listener: %s", strerror(errno));
        if (sock >= 0) close(sock);
        return;
    }

    while (!terminate)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
        int ready = select(sock + 1, &rfds, NULL, NULL, &timeout);
        if (ready < 0)
        {
            if (errno == EINTR) continue;
            log_error("P5 listener select: %s", strerror(errno));
            break;
        }
        if (ready == 0) continue;

        struct sockaddr_in incoming_addr;
        socklen_t addr_len = sizeof(incoming_addr);
        int conn_sock = accept(sock, (struct sockaddr *)&incoming_addr, &addr_len);
        if (conn_sock < 0)
        {
            if (errno != EINTR) log_error("accept: %s", strerror(errno));
            continue;
        }

        pthread_mutex_lock(&proxy_info.membership_mutex);
        int pnf_index = -1;
        for (int i = 0; i < MAX_NUM_PNF; i++)
        {
            if (proxy_info.pnf_list[i] == -1) { pnf_index = i; break; }
        }
        if (pnf_index == -1)
        {
            pthread_mutex_unlock(&proxy_info.membership_mutex);
            log_warn("Rejecting PNF connection: capacity %d is full", MAX_NUM_PNF);
            close(conn_sock);
            continue;
        }

        bool first_active = register_pnf_locked(pnf_index, conn_sock);
        pthread_mutex_unlock(&proxy_info.membership_mutex);

        log_info("PNF %s:%d connected as ID %d%s", inet_ntoa(incoming_addr.sin_addr), ntohs(incoming_addr.sin_port),
                 pnf_index, first_active ? " (primary)" : "");
    }
    close(sock);
}

void connect_to_vnf_p5(void)
{
    int sock = create_p5_sctp_socket();
    if (sock < 0)
    {
        log_error("create_p5_sctp_socket");
        return;
    }

    // connect to VNF
    struct sockaddr_in vnf_addr;
    memset(&vnf_addr, 0, sizeof(vnf_addr));
    vnf_addr.sin_family = AF_INET;
    vnf_addr.sin_port = htons(proxy_info.p5_vnf_sctp_port);
    inet_pton(AF_INET, "127.0.0.1", &vnf_addr.sin_addr);

    // make automatic retry to connect
    while (connect(sock, (struct sockaddr *)&vnf_addr, sizeof(vnf_addr)) < 0 && !terminate)
    {
        log_info("Retrying connection to VNF...");
        sleep(5);
    }
    // print source and destination address and port
    struct sockaddr_in local_addr;
    socklen_t addr_len = sizeof(local_addr);
    if (getsockname(sock, (struct sockaddr *)&local_addr, &addr_len) < 0)
    {
        log_error("getsockname");
        close(sock);
        return;
    }
    log_info("Connected to VNF");
    log_info("Local address: %s:%d", inet_ntoa(local_addr.sin_addr), ntohs(local_addr.sin_port));
    log_info("Remote address: %s:%d", inet_ntoa(vnf_addr.sin_addr), ntohs(vnf_addr.sin_port));

    proxy_info.p5_north_sock = sock;
    if (sock > proxy_info.p5_max_fd)
    {
        proxy_info.p5_max_fd = sock;
    }
    log_info("Added VNF connection on socket %d", sock);
    log_debug("Max FD is now %d", proxy_info.p5_max_fd);
}

void handle_migration(int rnti, int pnf_index, struct control_response *resp)
{
    if (pnf_index < 0 || pnf_index >= MAX_NUM_PNF || proxy_info.pnf_list[pnf_index] == -1)
    {
        log_error("Invalid PNF index in control command");
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "Invalid PNF index: %d", pnf_index);
        return;
    }
    if (!proxy_info.pnf_ready[pnf_index])
    {
        log_error("PNF %d is connected but not ready", pnf_index);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "L1 %d is connected but not ready", pnf_index);
        return;
    }

    if (proxy_info.slingshot_mode)
    {
        log_info("Migrating all RNTIs to PNF %d", pnf_index);
        for (int i = 0; i < 65536; i++)
        {
            // proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_curr[i] = pnf_index;
            proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_next[i] = pnf_index;
#ifdef STAGED_HARQ_UPLINK
            for (int h = 0; h < MAX_NR_HARQ_PROCESSES; h++)
            {
                proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_next[i][h] = pnf_index;
                proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[i][h] = pnf_index;
            }
#else
            // proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[i] = pnf_index;
            proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_next[i] = pnf_index;
#endif
        }
        resp->status = 0;
        atomic_store_explicit(&proxy_info.mapping_update_pending, true,
                              memory_order_release);
        log_info("Migrated all RNTIs to PNF %d", pnf_index);
    }
    else
    {
        // proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_curr[rnti] = pnf_index;
        proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_next[rnti] = pnf_index;
        log_info("Set DL RNTI 0x%04x to PNF %d", rnti, pnf_index);
#ifdef STAGED_HARQ_UPLINK
        // Stage uplink mapping per HARQ process; promote on PUSCH NDI
        for (int h = 0; h < MAX_NR_HARQ_PROCESSES; h++)
        {
            proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_next[rnti][h] = pnf_index;
        }
        log_info("Staged UL RNTI 0x%04x to PNF %d for all HARQ processes (activates on NDI)", rnti, pnf_index);
#else
        // proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[rnti] = pnf_index;
        proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_next[rnti] = pnf_index;
        log_info("Set UL RNTI 0x%04x to PNF %d", rnti, pnf_index);
#endif
        resp->status = 0;
        atomic_store_explicit(&proxy_info.mapping_update_pending, true,
                              memory_order_release);
    }
}

static void handle_l1_readiness(int pnf_index, bool ready, struct control_response *resp)
{
    if (!set_pnf_ready_locked(pnf_index, ready))
    {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "Invalid or disconnected L1 ID: %d", pnf_index);
        return;
    }
    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message), "L1 %d is %s", pnf_index, ready ? "ready" : "not_ready");
}

static void execute_control_command(const struct control_command *cmd,
                                    struct control_response *resp)
{
    memset(resp, 0, sizeof(*resp));
    log_debug("Received control command: %s %s %s", cmd->cmd, cmd->arg0, cmd->arg1);

    // list of supported commands
    // 1. migrate <RNTI> <PNF>
    // 2. list_l1
    // 3. set_ready <L1 ID>
    // 4. set_not_ready <L1 ID>
    // 5. debug <on|off>
    if (strcmp(cmd->cmd, "migrate") == 0)
    {
        int rnti = atoi(cmd->arg0);
        int pnf_index = atoi(cmd->arg1);
        pthread_mutex_lock(&proxy_info.membership_mutex);
        handle_migration(rnti, pnf_index, resp);
        pthread_mutex_unlock(&proxy_info.membership_mutex);
    }
    else if (strcmp(cmd->cmd, "list_l1") == 0 || strcmp(cmd->cmd, "l1s") == 0)
    {
        pthread_mutex_lock(&proxy_info.membership_mutex);
        format_l1_list_locked(resp->message, sizeof(resp->message));
        resp->status = 0;
        pthread_mutex_unlock(&proxy_info.membership_mutex);
    }
    else if (strcmp(cmd->cmd, "set_ready") == 0 || strcmp(cmd->cmd, "set_not_ready") == 0)
    {
        int pnf_index = atoi(cmd->arg0);
        pthread_mutex_lock(&proxy_info.membership_mutex);
        handle_l1_readiness(pnf_index, strcmp(cmd->cmd, "set_ready") == 0, resp);
        pthread_mutex_unlock(&proxy_info.membership_mutex);
    }
    else if (strcmp(cmd->cmd, "debug") == 0)
    {
        if (strcmp(cmd->arg0, "on") == 0)
        {
            log_set_level(LOG_DEBUG);
            log_info("Set log level to DEBUG");
            resp->status = 0;
        }
        else if (strcmp(cmd->arg0, "off") == 0)
        {
            log_set_level(LOG_INFO);
            log_info("Set log level to INFO");
            resp->status = 0;
        }
        else
        {
            log_error("Invalid argument for debug command");
            resp->status = -1;
            snprintf(resp->message, sizeof(resp->message), "Invalid argument for debug command: %s", cmd->arg0);
        }
    }
    else
    {
        log_error("Unknown control command: %s", cmd->cmd);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "Unknown command: %s", cmd->cmd);
    }
}

static bool parse_control_line(char *line, struct control_command *cmd,
                               struct control_response *resp)
{
    char extra[2];
    memset(cmd, 0, sizeof(*cmd));
    int fields = sscanf(line, "%15s %15s %15s %1s",
                        cmd->cmd, cmd->arg0, cmd->arg1, extra);
    int expected = 0;
    if (fields >= 1)
    {
        if (strcmp(cmd->cmd, "migrate") == 0) expected = 3;
        else if (strcmp(cmd->cmd, "set_ready") == 0 ||
                 strcmp(cmd->cmd, "set_not_ready") == 0 ||
                 strcmp(cmd->cmd, "debug") == 0) expected = 2;
        else if (strcmp(cmd->cmd, "list_l1") == 0 ||
                 strcmp(cmd->cmd, "l1s") == 0) expected = 1;
    }
    if (fields != expected)
    {
        memset(resp, 0, sizeof(*resp));
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "Invalid control command syntax");
        return false;
    }
    return true;
}

static int send_stream_reply(int fd, const struct control_response *resp)
{
    char reply[320];
    const char *message = resp->message[0] != '\0' ? resp->message : "OK";
    int length = snprintf(reply, sizeof(reply), "%s %s\n.\n",
                          resp->status == 0 ? "OK" : "ERR", message);
    if (length < 0 || length >= (int)sizeof(reply)) return -1;
    size_t sent = 0;
    while (sent < (size_t)length)
    {
        ssize_t n = send(fd, reply + sent, (size_t)length - sent, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

void handle_control_command(void)
{
    int client = accept(proxy_info.control_sock, NULL, NULL);
    if (client < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            log_error("accept control client: %s", strerror(errno));
        return;
    }

    struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char line[128] = {0};
    size_t used = 0;
    while (used < sizeof(line) - 1)
    {
        ssize_t n = read(client, line + used, sizeof(line) - 1 - used);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        used += (size_t)n;
        if (memchr(line, '\n', used) != NULL) break;
    }
    line[used] = '\0';

    struct control_command cmd;
    struct control_response resp;
    if (used == 0)
    {
        memset(&resp, 0, sizeof(resp));
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Empty control command");
    }
    else if (parse_control_line(line, &cmd, &resp))
    {
        execute_control_command(&cmd, &resp);
    }
    if (send_stream_reply(client, &resp) < 0)
        log_warn("Failed to reply to control client: %s", strerror(errno));
    close(client);
}

void control_input_loop(void)
{
    uint8_t curr_log_level = log_get_level();

    // prevent main from exiting
    while (!terminate)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(proxy_info.control_sock, &rfds);

        struct timeval tv;
        tv.tv_sec = 1;     // wake up periodically so we can check 'terminate'
        tv.tv_usec = 0;

        int nfds = proxy_info.control_sock + 1;
        int rc = select(nfds, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) {
                continue; // interrupted by signal; re-check terminate
            }
            log_error("select on control socket: %s", strerror(errno));
            break;
        }
        if (rc == 0) {
            // timeout — just loop again so we can respond to terminate quickly
            continue;
        }

        if (FD_ISSET(proxy_info.control_sock, &rfds)) {
            handle_control_command();
        }
    }
    log_info("NFAPI Proxy shutting down");
    sleep(2);
}

static bool parse_id_env(const char *name, unsigned long *value)
{
    const char *text = getenv(name);
    if (text == NULL || *text == '\0') return false;
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
    {
        log_warn("Ignoring invalid %s value '%s'", name, text);
        return false;
    }
    *value = parsed;
    return true;
}

static int configure_control_socket_permissions(const char *path)
{
    unsigned long sudo_uid;
    unsigned long sudo_gid;
    bool have_uid = parse_id_env("SUDO_UID", &sudo_uid);
    bool have_gid = parse_id_env("SUDO_GID", &sudo_gid);

    if ((have_uid && (unsigned long)(uid_t)sudo_uid != sudo_uid) ||
        (have_gid && (unsigned long)(gid_t)sudo_gid != sudo_gid))
    {
        log_error("SUDO_UID or SUDO_GID is outside the supported range");
        return -1;
    }

    if (geteuid() == 0 && have_uid && have_gid &&
        chown(path, (uid_t)sudo_uid, (gid_t)sudo_gid) < 0)
    {
        log_error("Failed to set control socket ownership: %s", strerror(errno));
        return -1;
    }
    if (chmod(path, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP) < 0)
    {
        log_error("Failed to set control socket permissions: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static void destroy_control_socket_at_path(int *sock, const char *path)
{
    if (*sock != -1)
    {
        close(*sock);
        *sock = -1;
    }
    if (unlink(path) < 0 && errno != ENOENT)
        log_warn("Failed to remove control socket %s: %s", path, strerror(errno));
}

static void destroy_control_socket(void)
{
    destroy_control_socket_at_path(&proxy_info.control_sock,
                                   proxy_info.control_socket_path);
}

void create_control_socket(void)
{
    int sock_fd;
    struct sockaddr_un addr;
    const char *path = proxy_info.control_socket_path;

    unlink(path);
    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0)
    {
        log_error("Failed to create control socket: %s", strerror(errno));
        exit(-1);
    }

    // Set socket to non-blocking
    int flags = fcntl(sock_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        log_error("Failed to set socket to non-blocking: %s", strerror(errno));
        close(sock_fd);
        exit(-1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        log_error("Failed to bind control socket: %s", strerror(errno));
        close(sock_fd);
        exit(-1);
    }
    if (listen(sock_fd, 8) < 0)
    {
        log_error("Failed to listen on control socket: %s", strerror(errno));
        close(sock_fd);
        unlink(path);
        exit(-1);
    }
    if (configure_control_socket_permissions(path) < 0)
    {
        close(sock_fd);
        unlink(path);
        exit(-1);
    }

    log_info("Stream control socket initialized at %s with mode 0660", path);
    proxy_info.control_sock = sock_fd;
}

void nfapi_proxy_test_init(void)
{
    log_info("Initializing NFAPI Proxy in TEST MODE");

    // In test mode, we skip P5 setup and directly create P7 UDP sockets
    // Create P7 UDP socket to VNF (test stub)
    struct sockaddr_in vnf_addr;
    memset(&vnf_addr, 0, sizeof(vnf_addr));
    vnf_addr.sin_family = AF_INET;
    vnf_addr.sin_port = htons(proxy_info.test_vnf_port);
    if (inet_pton(AF_INET, "127.0.0.1", &vnf_addr.sin_addr) <= 0)
    {
        log_error("inet_pton for VNF");
        return;
    }

    struct sockaddr_in proxy_vnf_addr;
    memset(&proxy_vnf_addr, 0, sizeof(proxy_vnf_addr));
    proxy_vnf_addr.sin_family = AF_INET;
    proxy_vnf_addr.sin_port = htons(proxy_info.p7_udp_port);
    if (inet_pton(AF_INET, "127.0.0.1", &proxy_vnf_addr.sin_addr) <= 0)
    {
        log_error("inet_pton for proxy VNF addr");
        return;
    }

    proxy_info.p7_north_sock = create_p7_connected_udp_socket(&proxy_vnf_addr, &vnf_addr);
    if (proxy_info.p7_north_sock < 0)
    {
        log_error("Failed to create P7 connected UDP socket to VNF (test stub)");
        return;
    }
    if (proxy_info.p7_north_sock > proxy_info.p7_max_fd)
    {
        proxy_info.p7_max_fd = proxy_info.p7_north_sock;
    }
    log_info("Created P7 connected UDP socket to VNF test stub on port %d", proxy_info.test_vnf_port);

    // Create P7 UDP sockets to PNFs (test stubs)
    for (int i = 0; i < MAX_NUM_PNF; i++)
    {
        if (proxy_info.test_pnf_ports[i] == 0)
            continue;

        struct sockaddr_in pnf_addr;
        memset(&pnf_addr, 0, sizeof(pnf_addr));
        pnf_addr.sin_family = AF_INET;
        pnf_addr.sin_port = htons(proxy_info.test_pnf_ports[i]);
        if (inet_pton(AF_INET, "127.0.0.1", &pnf_addr.sin_addr) <= 0)
        {
            log_error("inet_pton for PNF %d", i);
            return;
        }

        struct sockaddr_in proxy_pnf_addr;
        memset(&proxy_pnf_addr, 0, sizeof(proxy_pnf_addr));
        proxy_pnf_addr.sin_family = AF_INET;
        proxy_pnf_addr.sin_port = htons(proxy_info.p7_udp_port);
        if (inet_pton(AF_INET, "127.0.0.1", &proxy_pnf_addr.sin_addr) <= 0)
        {
            log_error("inet_pton for proxy PNF addr %d", i);
            return;
        }

        proxy_info.p7_south_sock[i] = create_p7_connected_udp_socket(&proxy_pnf_addr, &pnf_addr);
        if (proxy_info.p7_south_sock[i] < 0)
        {
            log_error("Failed to create P7 connected UDP socket to PNF %d (test stub)", i);
            return;
        }
        if (proxy_info.p7_south_sock[i] > proxy_info.p7_max_fd)
        {
            proxy_info.p7_max_fd = proxy_info.p7_south_sock[i];
        }
        log_info("Created P7 connected UDP socket to PNF %d test stub on port %d", i, proxy_info.test_pnf_ports[i]);

        // Add PNF to the list
        proxy_info.pnf_list[i] = i;
        // Test-mode endpoints are explicitly configured at startup and are
        // therefore admitted immediately.
        proxy_info.pnf_ready[i] = true;
        proxy_info.pnf_connection_order[i] = ++proxy_info.next_connection_order;
    }

    proxy_info.primary_pnf = select_oldest_pnf_locked();

    // Set up default RNTI to PNF mappings for test mode
    // Map test RNTIs to different PNFs for testing message splitting/merging
    log_info("Setting up default RNTI to PNF mappings for test mode");

    // RNTI 0x1001 -> PNF 0
    proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_curr[0x1001] = 0;
#ifdef STAGED_HARQ_UPLINK
    for (int h = 0; h < MAX_NR_HARQ_PROCESSES; h++)
    {
        proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[0x1001][h] = 0;
        proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_next[0x1001][h] = 0;
    }
#else
    proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[0x1001] = 0;
    proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_next[0x1001] = 0;
#endif
    log_info("  RNTI 0x1001 -> PNF 0");

    // Put RNTI 0x1002 on the secondary when present, otherwise on the primary.
    int test_secondary_pnf = proxy_info.pnf_list[1] != -1 ? 1 : proxy_info.primary_pnf;
    proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_curr[0x1002] = test_secondary_pnf;
#ifdef STAGED_HARQ_UPLINK
    for (int h = 0; h < MAX_NR_HARQ_PROCESSES; h++)
    {
        proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[0x1002][h] = test_secondary_pnf;
        proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_next[0x1002][h] = test_secondary_pnf;
    }
#else
    proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[0x1002] = test_secondary_pnf;
    proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_next[0x1002] = test_secondary_pnf;
#endif
    log_info("  RNTI 0x1002 -> PNF %d", test_secondary_pnf);

    // RNTI 0x1003 -> PNF 0
    proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_curr[0x1003] = 0;
#ifdef STAGED_HARQ_UPLINK
    for (int h = 0; h < MAX_NR_HARQ_PROCESSES; h++)
    {
        proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[0x1003][h] = 0;
        proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_next[0x1003][h] = 0;
    }
#else
    proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[0x1003] = 0;
    proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_next[0x1003] = 0;
#endif
    log_info("  RNTI 0x1003 -> PNF 0");

    // Start P7 thread only
    pthread_create(&p7_thread, NULL, (void *)nfapi_p7_thread, NULL);
}

void nfapi_proxy_init(void)
{
    connect_to_vnf_p5();
    pthread_create(&p5_listen_thread, NULL, (void *)nfapi_p5_south_thread, NULL);
    pthread_create(&p5_thread, NULL, (void *)nfapi_p5_thread, NULL);
    pthread_create(&p7_thread, NULL, (void *)nfapi_p7_thread, NULL);
}

void start_proxy(void)
{
    if (proxy_info.test_mode)
    {
        nfapi_proxy_test_init();
    }
    else
    {
        nfapi_proxy_init();
    }
    log_info("NFAPI Proxy started");
}

void initialize_socket_states(void)
{
    proxy_info.control_sock = -1;
    // P5
    proxy_info.p5_north_sock = -1;
    proxy_info.p5_north_sock_state = 0;
    for (int i = 0; i < MAX_NUM_PNF; i++)
    {
        proxy_info.p5_south_sock[i] = -1;
        proxy_info.p5_south_sock_state[i] = 0;
        proxy_info.pnf_ready[i] = false;
    }
    proxy_info.p5_max_fd = 0;

    // P7
    proxy_info.p7_north_sock = -1;
    for (int i = 0; i < MAX_NUM_PNF; i++)
    {
        proxy_info.p7_south_sock[i] = -1;
    }
    proxy_info.p7_max_fd = 0;
}

void initialize_lookup_tables(void)
{
    atomic_store_explicit(&proxy_info.mapping_update_pending, false,
                          memory_order_relaxed);
    // initialize ue_rnti_to_pnf
    for (int i = 0; i < 65536; i++)
    {
        for (int h = 0; h < MAX_NR_HARQ_PROCESSES; h++)
        {
#ifdef STAGED_HARQ_UPLINK
            proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[i][h] = 0;
            proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_prev[i][h] = 0;
            proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_next[i][h] = 0;
#else
            proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[i] = 0;
            proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_next[i] = 0;
            proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_prev[i] = 0;
#endif
        }

        proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_curr[i] = 0;
        proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_next[i] = 0;
        proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_prev[i] = 0;
    }

    // initialize PDSCH to TX_DATA mapping to -1 to avoid stale zeros
    memset(proxy_info.downlink_rnti_to_pnf.pdsch_to_tx_data_mapping, -1,
           sizeof(proxy_info.downlink_rnti_to_pnf.pdsch_to_tx_data_mapping));

    // PNF lists
    for (int i = 0; i < MAX_NUM_PNF; i++)
    {
        proxy_info.pnf_list[i] = -1;
    }

    // The oldest connected PNF is primary; there is no primary before the first connection.
    proxy_info.primary_pnf = -1;
}

void setup_logger(void)
{
    if (proxy_info.test_mode)
    {
        printf("Running in TEST MODE\n");
        printf("  VNF test port: %d\n", proxy_info.test_vnf_port);
        for (int i = 0; i < MAX_NUM_PNF; i++)
        {
            if (proxy_info.test_pnf_ports[i] != 0)
            {
                printf("  PNF %d test port: %d\n", i, proxy_info.test_pnf_ports[i]);
            }
        }
        printf("  Proxy P7 port: %d\n", proxy_info.p7_udp_port);
#ifdef MICROBENCHMARK_LOGGING
        // Hot-path logging would dominate the latency being measured.
        log_set_level(LOG_WARN);
#else
        log_set_level(LOG_TRACE);
#endif
    }
    else
    {
        log_set_level(LOG_INFO);
    }
}

static bool set_control_socket_path(const char *path)
{
    if (path == NULL || *path == '\0' ||
        strlen(path) >= sizeof(proxy_info.control_socket_path))
        return false;
    snprintf(proxy_info.control_socket_path,
             sizeof(proxy_info.control_socket_path), "%s", path);
    return true;
}

void print_usage(const char *prog_name)
{
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  --slingshot-mode         Enable Slingshot mode\n");
    printf("  --test-mode              Enable test mode (bypass P5 setup)\n");
    printf("  --test-vnf-port <port>   VNF P7 UDP port for test mode (default: 60001)\n");
    printf("  --test-pnf-port <ports>  PNF P7 UDP ports for test mode, comma-separated (e.g., 60010,60011)\n");
    printf("  --p5-vnf-port <port>     VNF P5 SCTP port (default: 50001)\n");
    printf("  --p5-port <port>         PNF-facing P5 SCTP port (default: 50002)\n");
    printf("  --p7-port <port>         Proxy P7 UDP port (default: 50012)\n");
    printf("  --ctrl-sock <path>       Control socket path (default: %s)\n", DEFAULT_CONTROL_SOCKET_PATH);
    printf("  --help                   Show this help message\n");
}

void parse_args(int argc, char *argv[])
{
    // default values
    proxy_info.test_mode = false;
    proxy_info.slingshot_mode = false;
    proxy_info.test_vnf_port = 60001;
    proxy_info.p5_vnf_sctp_port = 50001;
    proxy_info.p5_sctp_port = 50002;
    proxy_info.p7_udp_port = 50012;
    const char *control_path = getenv("THOR_CTRL_SOCK");
    if (control_path == NULL || *control_path == '\0')
        control_path = DEFAULT_CONTROL_SOCKET_PATH;
    if (!set_control_socket_path(control_path))
    {
        fprintf(stderr, "Invalid control socket path: %s\n", control_path);
        exit(1);
    }

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--slingshot-mode") == 0)
        {
            printf("Enabling Slingshot mode\n");
            proxy_info.slingshot_mode = true;
        }
        else if (strcmp(argv[i], "--test-mode") == 0)
        {
            proxy_info.test_mode = true;
        }
        else if (strcmp(argv[i], "--test-vnf-port") == 0 && i + 1 < argc)
        {
            proxy_info.test_vnf_port = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--test-pnf-port") == 0 && i + 1 < argc)
        {
            char *ports_str = argv[++i];
            char *token = strtok(ports_str, ",");
            int pnf_idx = 0;
            while (token != NULL && pnf_idx < MAX_NUM_PNF)
            {
                proxy_info.test_pnf_ports[pnf_idx++] = atoi(token);
                token = strtok(NULL, ",");
            }
        }
        else if (strcmp(argv[i], "--p7-port") == 0 && i + 1 < argc)
        {
            proxy_info.p7_udp_port = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--p5-port") == 0 && i + 1 < argc)
        {
            proxy_info.p5_sctp_port = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--p5-vnf-port") == 0 && i + 1 < argc)
        {
            proxy_info.p5_vnf_sctp_port = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--ctrl-sock") == 0 && i + 1 < argc)
        {
            if (!set_control_socket_path(argv[++i]))
            {
                fprintf(stderr, "Invalid control socket path: %s\n", argv[i]);
                exit(1);
            }
        }
        else if (strcmp(argv[i], "--help") == 0)
        {
            print_usage(argv[0]);
            exit(0);
        }
        else
        {
            printf("Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            exit(1);
        }
    }
}

void initialize_global_structures(void)
{
    memset(&proxy_info, 0, sizeof(proxy_info));
    memset(&vnf_p5_cache, 0, sizeof(vnf_p5_cache));
    memset(&pnf_p5_cache, 0, sizeof(pnf_p5_cache));
    pthread_mutex_init(&proxy_info.membership_mutex, NULL);
    atomic_init(&proxy_info.mapping_update_pending, false);
}

void arm_signal_handlers(void)
{
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
}

void print_build_info(void)
{
    printf("=================================\n");
    printf("THOR nFAPI proxy\n");
    printf("VERSION: commit hash: %s (date: %s)\n", GIT_COMMIT_HASH, GIT_COMMIT_DATE);
    printf("BUILD: %s \n", BUILD_TIME);

#ifdef STAGED_HARQ_UPLINK
    printf("BUILD OPTION: STAGED_HARQ_UPLINK enabled\n");
#endif

#ifdef DL_LEGACY
    printf("BUILD OPTION: DL_LEGACY enabled\n");
#endif

#ifdef UL_TTI_LEGACY
    printf("BUILD OPTION: UL_TTI_LEGACY enabled\n");
#endif

#ifdef UL_SEGMENTATION_REASSEMBLY
    printf("BUILD OPTION: UL_SEGMENTATION_REASSEMBLY enabled\n");
#endif

#ifdef MICROBENCHMARK_LOGGING
    printf("BUILD OPTION: MICROBENCHMARK_LOGGING enabled\n");
#endif

    printf("=================================\n");
    printf("\n");
}

int main(int argc, char *argv[])
{
    arm_signal_handlers();

    int ret = 0;
    print_build_info();
    initialize_global_structures();
    initialize_lookup_tables();
    initialize_socket_states();

    parse_args(argc, argv);
    setup_logger();

    start_proxy();
    create_control_socket();
    control_input_loop();
    destroy_control_socket();
    pthread_join(p7_thread, NULL);
    microbenchmark_flush();

    return 0;
}
