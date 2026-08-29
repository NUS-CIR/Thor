#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdbool.h>
#include <fcntl.h>
#include <time.h>

#include "nfapi_nr_interface.h"
#include "nfapi_nr_interface_scf.h"
#include "nr_fapi_p7_utils.h"

static volatile bool terminate = false;
static bool quiet_output = false;
static int test_payload_size = 1024;
#define printf(...) (quiet_output ? 0 : fprintf(stdout, __VA_ARGS__))

// Structure to track scheduled uplink transmissions
typedef struct {
    uint16_t sfn;
    uint16_t slot;
    uint16_t rnti;
    uint8_t harq_id;
    bool scheduled;
} uplink_schedule_t;

#define MAX_SCHEDULED_UL 100
uplink_schedule_t ul_schedule[MAX_SCHEDULED_UL];
int ul_schedule_count = 0;

void sigint_handler(int signum)
{
    printf("Received signal %d, terminating...\n", signum);
    terminate = true;
}

// Create a simple UDP socket
int create_udp_socket(uint16_t port)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(sock);
        return -1;
    }
    
    return sock;
}

// Send SLOT_INDICATION message
int send_slot_indication(int sock, struct sockaddr_in *proxy_addr, uint16_t sfn, uint16_t slot)
{
    nfapi_nr_slot_indication_scf_t slot_ind;
    memset(&slot_ind, 0, sizeof(slot_ind));
    
    slot_ind.header.message_id = NFAPI_NR_PHY_MSG_TYPE_SLOT_INDICATION;
    slot_ind.header.message_length = 0;
    slot_ind.sfn = sfn;
    slot_ind.slot = slot;
    
    uint8_t buffer[65536];
    int packed_len = nfapi_nr_p7_message_pack(&slot_ind, buffer, sizeof(buffer), 0);
    if (packed_len <= 0)
    {
        fprintf(stderr, "Failed to pack SLOT_INDICATION\n");
        return -1;
    }
    
    nfapi_nr_p7_update_checksum(buffer, packed_len);
    
    int ret = sendto(sock, buffer, packed_len, 0, (struct sockaddr *)proxy_addr, sizeof(*proxy_addr));
    if (ret < 0)
    {
        perror("sendto SLOT_INDICATION");
        return -1;
    }
    
    if (slot == 0)
    {
        printf("Sent SLOT_INDICATION: SFN=%d, Slot=%d\n", sfn, slot);
    }
    return 0;
}

// Send RX_DATA_INDICATION message based on scheduled UL
int send_rx_data_indication_for_schedule(int sock, struct sockaddr_in *proxy_addr, uint16_t sfn, uint16_t slot)
{
    // Find all scheduled UL transmissions for this slot
    int num_scheduled = 0;
    uint16_t scheduled_rntis[MAX_SCHEDULED_UL];
    uint8_t scheduled_harq_ids[MAX_SCHEDULED_UL];
    
    for (int i = 0; i < ul_schedule_count; i++)
    {
        if (ul_schedule[i].scheduled && 
            ul_schedule[i].sfn == sfn && 
            ul_schedule[i].slot == slot)
        {
            scheduled_rntis[num_scheduled] = ul_schedule[i].rnti;
            scheduled_harq_ids[num_scheduled] = ul_schedule[i].harq_id;
            ul_schedule[i].scheduled = false; // Mark as processed
            num_scheduled++;
        }
    }
    
    if (num_scheduled == 0)
    {
        return 0; // No scheduled UL for this slot
    }
    
    nfapi_nr_rx_data_indication_t rx_data_ind;
    memset(&rx_data_ind, 0, sizeof(rx_data_ind));
    
    rx_data_ind.header.message_id = NFAPI_NR_PHY_MSG_TYPE_RX_DATA_INDICATION;
    rx_data_ind.header.message_length = 0;
    rx_data_ind.sfn = sfn;
    rx_data_ind.slot = slot;
    rx_data_ind.number_of_pdus = num_scheduled;
    
    // Add PDUs for each scheduled UE
    rx_data_ind.pdu_list = malloc(sizeof(nfapi_nr_rx_data_pdu_t) * num_scheduled);
    for (int i = 0; i < num_scheduled; i++)
    {
        rx_data_ind.pdu_list[i].rnti = scheduled_rntis[i];
        rx_data_ind.pdu_list[i].harq_id = scheduled_harq_ids[i];
        rx_data_ind.pdu_list[i].pdu_length = test_payload_size;
        rx_data_ind.pdu_list[i].pdu = malloc(test_payload_size);
        memset(rx_data_ind.pdu_list[i].pdu, 0xB0 + i, test_payload_size);
    }
    
    uint8_t buffer[65536];
    int packed_len = nfapi_nr_p7_message_pack(&rx_data_ind, buffer, sizeof(buffer), 0);
    
    // Free allocated memory
    for (int i = 0; i < num_scheduled; i++)
    {
        free(rx_data_ind.pdu_list[i].pdu);
    }
    free(rx_data_ind.pdu_list);
    
    if (packed_len <= 0)
    {
        fprintf(stderr, "Failed to pack RX_DATA_INDICATION\n");
        return -1;
    }
    
    nfapi_nr_p7_update_checksum(buffer, packed_len);
    
    int ret = sendto(sock, buffer, packed_len, 0, (struct sockaddr *)proxy_addr, sizeof(*proxy_addr));
    if (ret < 0)
    {
        perror("sendto RX_DATA_INDICATION");
        return -1;
    }
    
    printf("Sent RX_DATA_INDICATION: SFN=%d, Slot=%d, NumPdus=%d (scheduled UL)\n", sfn, slot, num_scheduled);
    for (int i = 0; i < num_scheduled; i++)
    {
        printf("  PDU %d: RNTI=0x%04x\n", i, scheduled_rntis[i]);
    }
    return 0;
}

// Send CRC_INDICATION message based on scheduled UL
int send_crc_indication_for_schedule(int sock, struct sockaddr_in *proxy_addr, uint16_t sfn, uint16_t slot)
{
    // Find all scheduled UL transmissions for this slot (that haven't been processed yet)
    int num_scheduled = 0;
    uint16_t scheduled_rntis[MAX_SCHEDULED_UL];
    uint8_t scheduled_harq_ids[MAX_SCHEDULED_UL];
    
    for (int i = 0; i < ul_schedule_count; i++)
    {
        if (!ul_schedule[i].scheduled && 
            ul_schedule[i].sfn == sfn && 
            ul_schedule[i].slot == slot)
        {
            scheduled_rntis[num_scheduled] = ul_schedule[i].rnti;
            scheduled_harq_ids[num_scheduled] = ul_schedule[i].harq_id;
            num_scheduled++;
        }
    }
    
    if (num_scheduled == 0)
    {
        return 0; // No scheduled UL for this slot
    }
    
    nfapi_nr_crc_indication_t crc_ind;
    memset(&crc_ind, 0, sizeof(crc_ind));
    
    crc_ind.header.message_id = NFAPI_NR_PHY_MSG_TYPE_CRC_INDICATION;
    crc_ind.header.message_length = 0;
    crc_ind.sfn = sfn;
    crc_ind.slot = slot;
    crc_ind.number_crcs = num_scheduled;
    
    // Add CRCs for each scheduled UE
    crc_ind.crc_list = malloc(sizeof(nfapi_nr_crc_t) * num_scheduled);
    for (int i = 0; i < num_scheduled; i++)
    {
        crc_ind.crc_list[i].rnti = scheduled_rntis[i];
        crc_ind.crc_list[i].harq_id = scheduled_harq_ids[i];
        crc_ind.crc_list[i].tb_crc_status = 1; // CRC pass
    }
    
    uint8_t buffer[65536];
    int packed_len = nfapi_nr_p7_message_pack(&crc_ind, buffer, sizeof(buffer), 0);
    free(crc_ind.crc_list);
    
    if (packed_len <= 0)
    {
        fprintf(stderr, "Failed to pack CRC_INDICATION\n");
        return -1;
    }
    
    nfapi_nr_p7_update_checksum(buffer, packed_len);
    
    int ret = sendto(sock, buffer, packed_len, 0, (struct sockaddr *)proxy_addr, sizeof(*proxy_addr));
    if (ret < 0)
    {
        perror("sendto CRC_INDICATION");
        return -1;
    }
    
    printf("Sent CRC_INDICATION: SFN=%d, Slot=%d, NumCrcs=%d (scheduled UL)\n", sfn, slot, num_scheduled);
    for (int i = 0; i < num_scheduled; i++)
    {
        printf("  CRC %d: RNTI=0x%04x\n", i, scheduled_rntis[i]);
    }
    
    return 0;
}

// Send RACH_INDICATION message
int send_rach_indication(int sock, struct sockaddr_in *proxy_addr, uint16_t sfn, uint16_t slot)
{
    nfapi_nr_rach_indication_t rach_ind;
    memset(&rach_ind, 0, sizeof(rach_ind));
    
    rach_ind.header.message_id = NFAPI_NR_PHY_MSG_TYPE_RACH_INDICATION;
    rach_ind.header.message_length = 0;
    rach_ind.sfn = sfn;
    rach_ind.slot = slot;
    rach_ind.number_of_pdus = 1;
    
    // Add a simple RACH PDU
    rach_ind.pdu_list = malloc(sizeof(nfapi_nr_prach_indication_pdu_t) * rach_ind.number_of_pdus);
    rach_ind.pdu_list[0].phy_cell_id = 0;
    rach_ind.pdu_list[0].symbol_index = 0;
    rach_ind.pdu_list[0].slot_index = slot;
    rach_ind.pdu_list[0].freq_index = 0;
    rach_ind.pdu_list[0].num_preamble = 1;
    rach_ind.pdu_list[0].preamble_list[0].preamble_index = 0;
    
    uint8_t buffer[65536];
    int packed_len = nfapi_nr_p7_message_pack(&rach_ind, buffer, sizeof(buffer), 0);
    if (packed_len <= 0)
    {
        fprintf(stderr, "Failed to pack RACH_INDICATION\n");
        return -1;
    }
    
    nfapi_nr_p7_update_checksum(buffer, packed_len);
    
    int ret = sendto(sock, buffer, packed_len, 0, (struct sockaddr *)proxy_addr, sizeof(*proxy_addr));
    if (ret < 0)
    {
        perror("sendto RACH_INDICATION");
        return -1;
    }
    
    printf("Sent RACH_INDICATION: SFN=%d, Slot=%d\n", sfn, slot);
    return 0;
}

// Send UCI_INDICATION message
int send_uci_indication(int sock, struct sockaddr_in *proxy_addr, uint16_t sfn, uint16_t slot)
{
    nfapi_nr_uci_indication_t uci_ind;
    memset(&uci_ind, 0, sizeof(uci_ind));
    
    uci_ind.header.message_id = NFAPI_NR_PHY_MSG_TYPE_UCI_INDICATION;
    uci_ind.header.message_length = 0;
    uci_ind.sfn = sfn;
    uci_ind.slot = slot;
    uci_ind.num_ucis = 0;  // Set to 0 for simplicity
    uci_ind.uci_list = NULL;
    
    uint8_t buffer[65536];
    int packed_len = nfapi_nr_p7_message_pack(&uci_ind, buffer, sizeof(buffer), 0);
    if (packed_len <= 0)
    {
        fprintf(stderr, "Failed to pack UCI_INDICATION\n");
        return -1;
    }
    
    nfapi_nr_p7_update_checksum(buffer, packed_len);
    
    int ret = sendto(sock, buffer, packed_len, 0, (struct sockaddr *)proxy_addr, sizeof(*proxy_addr));
    if (ret < 0)
    {
        perror("sendto UCI_INDICATION");
        return -1;
    }
    
    printf("Sent UCI_INDICATION: SFN=%d, Slot=%d\n", sfn, slot);
    return 0;
}

// Send SRS_INDICATION message
int send_srs_indication(int sock, struct sockaddr_in *proxy_addr, uint16_t sfn, uint16_t slot)
{
    nfapi_nr_srs_indication_t srs_ind;
    memset(&srs_ind, 0, sizeof(srs_ind));
    
    srs_ind.header.message_id = NFAPI_NR_PHY_MSG_TYPE_SRS_INDICATION;
    srs_ind.header.message_length = 0;
    srs_ind.sfn = sfn;
    srs_ind.slot = slot;
    srs_ind.number_of_pdus = 1;
    
    // Add a simple SRS PDU
    srs_ind.pdu_list = malloc(sizeof(nfapi_nr_srs_indication_pdu_t) * srs_ind.number_of_pdus);
    srs_ind.pdu_list[0].rnti = 0x1234;
    srs_ind.pdu_list[0].timing_advance_offset = 0;
    
    uint8_t buffer[65536];
    int packed_len = nfapi_nr_p7_message_pack(&srs_ind, buffer, sizeof(buffer), 0);
    if (packed_len <= 0)
    {
        fprintf(stderr, "Failed to pack SRS_INDICATION\n");
        return -1;
    }
    
    nfapi_nr_p7_update_checksum(buffer, packed_len);
    
    int ret = sendto(sock, buffer, packed_len, 0, (struct sockaddr *)proxy_addr, sizeof(*proxy_addr));
    if (ret < 0)
    {
        perror("sendto SRS_INDICATION");
        return -1;
    }
    
    printf("Sent SRS_INDICATION: SFN=%d, Slot=%d\n", sfn, slot);
    return 0;
}

// Receive and process downlink requests, tracking schedule and generating immediate responses
int receive_and_process_request(int sock, int pnf_index, struct sockaddr_in *proxy_addr)
{
    uint8_t buffer[65536];
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    
    ssize_t recv_len = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&from_addr, &from_len);
    if (recv_len < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return 0; // No data available
        }
        perror("recvfrom");
        return -1;
    }
    
    nfapi_nr_p7_message_header_t header;
    if (!nfapi_nr_p7_message_header_unpack(buffer, recv_len, &header, sizeof(header), 0))
    {
        fprintf(stderr, "Failed to unpack message header\n");
        return -1;
    }
    
    switch (header.message_id)
    {
    case NFAPI_NR_PHY_MSG_TYPE_DL_TTI_REQUEST:
    {
        nfapi_nr_dl_tti_request_t dl_tti_req;
        nfapi_nr_p7_message_unpack(buffer, recv_len, &dl_tti_req, sizeof(dl_tti_req), 0);
        printf("Received DL_TTI_REQUEST: SFN=%d, Slot=%d, NumPdus=%d\n", 
               dl_tti_req.SFN, dl_tti_req.Slot, dl_tti_req.dl_tti_request_body.nPDUs);
        
        // Log the RNTIs
        for (int i = 0; i < dl_tti_req.dl_tti_request_body.nPDUs; i++)
        {
            if (dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[i].PDUType == NFAPI_NR_DL_TTI_PDSCH_PDU_TYPE)
            {
                uint16_t rnti = dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[i].pdsch_pdu.pdsch_pdu_rel15.rnti;
                printf("  PDSCH PDU %d: RNTI=0x%04x\n", i, rnti);
            }
        }
        break;
    }
    case NFAPI_NR_PHY_MSG_TYPE_TX_DATA_REQUEST:
    {
        nfapi_nr_tx_data_request_t tx_data_req;
        nfapi_nr_p7_message_unpack(buffer, recv_len, &tx_data_req, sizeof(tx_data_req), 0);
        printf("Received TX_DATA_REQUEST: SFN=%d, Slot=%d, NumPdus=%d\n",
               tx_data_req.SFN, tx_data_req.Slot, tx_data_req.Number_of_PDUs);
        break;
    }
    case NFAPI_NR_PHY_MSG_TYPE_UL_TTI_REQUEST:
    {
        nfapi_nr_ul_tti_request_t ul_tti_req;
        memset(&ul_tti_req, 0, sizeof(ul_tti_req));
        if (!nfapi_nr_p7_message_unpack(buffer, recv_len, &ul_tti_req, sizeof(ul_tti_req), 0))
        {
            fprintf(stderr, "Failed to unpack UL_TTI_REQUEST\n");
            return -1;
        }
        printf("Received UL_TTI_REQUEST: SFN=%d, Slot=%d, NumPdus=%d\n",
               ul_tti_req.SFN, ul_tti_req.Slot, ul_tti_req.n_pdus);
        
        // Process scheduled uplink transmissions and generate responses immediately
        int num_pusch = 0;
        uint16_t scheduled_rntis[NFAPI_NR_MAX_TX_REQUEST_PDUS];
        uint8_t scheduled_harq_ids[NFAPI_NR_MAX_TX_REQUEST_PDUS];
        
        for (int i = 0; i < ul_tti_req.n_pdus &&
                        i < NFAPI_NR_MAX_TX_REQUEST_PDUS; i++)
        {
            if (ul_tti_req.pdus_list[i].pdu_type == NFAPI_NR_UL_CONFIG_PUSCH_PDU_TYPE)
            {
                uint16_t rnti = ul_tti_req.pdus_list[i].pusch_pdu.rnti;
                // Use a simple HARQ ID of 0 for testing
                uint8_t harq_id = 0;
                printf("  PUSCH PDU %d: RNTI=0x%04x, HARQ=%d\n", i, rnti, harq_id);
                
                if (num_pusch < NFAPI_NR_MAX_TX_REQUEST_PDUS)
                {
                    scheduled_rntis[num_pusch] = rnti;
                    scheduled_harq_ids[num_pusch] = harq_id;
                    num_pusch++;
                }
            }
        }
        
        // Generate RX_DATA_INDICATION immediately for scheduled UL
        if (num_pusch > 0)
        {
            nfapi_nr_rx_data_indication_t rx_data_ind;
            memset(&rx_data_ind, 0, sizeof(rx_data_ind));
            
            rx_data_ind.header.message_id = NFAPI_NR_PHY_MSG_TYPE_RX_DATA_INDICATION;
            rx_data_ind.header.message_length = 0;
            rx_data_ind.sfn = ul_tti_req.SFN;
            rx_data_ind.slot = ul_tti_req.Slot;
            rx_data_ind.number_of_pdus = num_pusch;
            
            rx_data_ind.pdu_list = malloc(sizeof(nfapi_nr_rx_data_pdu_t) * num_pusch);
            if (!rx_data_ind.pdu_list)
            {
                fprintf(stderr, "Failed to allocate pdu_list\n");
                break;
            }
            
            for (int i = 0; i < num_pusch; i++)
            {
                rx_data_ind.pdu_list[i].rnti = scheduled_rntis[i];
                rx_data_ind.pdu_list[i].harq_id = scheduled_harq_ids[i];
                rx_data_ind.pdu_list[i].pdu_length = test_payload_size;
                rx_data_ind.pdu_list[i].pdu = malloc(test_payload_size);
                if (!rx_data_ind.pdu_list[i].pdu)
                {
                    fprintf(stderr, "Failed to allocate pdu data\n");
                    break;
                }
                memset(rx_data_ind.pdu_list[i].pdu, 0xB0 + i,
                       test_payload_size);
            }
            
            uint8_t tx_buffer[65536];
            int packed_len = nfapi_nr_p7_message_pack(&rx_data_ind, tx_buffer, sizeof(tx_buffer), 0);
            
            for (int i = 0; i < num_pusch; i++)
            {
                if (rx_data_ind.pdu_list[i].pdu)
                {
                    free(rx_data_ind.pdu_list[i].pdu);
                }
            }
            free(rx_data_ind.pdu_list);
            
            if (packed_len > 0)
            {
                nfapi_nr_p7_update_checksum(tx_buffer, packed_len);
                int send_ret = sendto(sock, tx_buffer, packed_len, 0, (struct sockaddr *)proxy_addr, sizeof(*proxy_addr));
                if (send_ret < 0)
                {
                    perror("sendto RX_DATA_INDICATION");
                }
                else
                {
                    printf("Sent RX_DATA_INDICATION: SFN=%d, Slot=%d, NumPdus=%d (immediate response)\n", 
                           ul_tti_req.SFN, ul_tti_req.Slot, num_pusch);
                    for (int i = 0; i < num_pusch; i++)
                    {
                        printf("  PDU %d: RNTI=0x%04x\n", i, scheduled_rntis[i]);
                    }
                }
            }
            
            // Generate CRC_INDICATION immediately as well
            nfapi_nr_crc_indication_t crc_ind;
            memset(&crc_ind, 0, sizeof(crc_ind));
            
            crc_ind.header.message_id = NFAPI_NR_PHY_MSG_TYPE_CRC_INDICATION;
            crc_ind.header.message_length = 0;
            crc_ind.sfn = ul_tti_req.SFN;
            crc_ind.slot = ul_tti_req.Slot;
            crc_ind.number_crcs = num_pusch;
            
            crc_ind.crc_list = malloc(sizeof(nfapi_nr_crc_t) * num_pusch);
            for (int i = 0; i < num_pusch; i++)
            {
                crc_ind.crc_list[i].rnti = scheduled_rntis[i];
                crc_ind.crc_list[i].harq_id = scheduled_harq_ids[i];
                crc_ind.crc_list[i].tb_crc_status = 1; // CRC pass
                crc_ind.crc_list[i].num_cb = 0;
                crc_ind.crc_list[i].cb_crc_status = NULL;
            }
            
            packed_len = nfapi_nr_p7_message_pack(&crc_ind, tx_buffer, sizeof(tx_buffer), 0);
            free(crc_ind.crc_list);
            
            if (packed_len > 0)
            {
                nfapi_nr_p7_update_checksum(tx_buffer, packed_len);
                int send_ret = sendto(sock, tx_buffer, packed_len, 0, (struct sockaddr *)proxy_addr, sizeof(*proxy_addr));
                if (send_ret < 0)
                {
                    perror("sendto CRC_INDICATION");
                }
                else
                {
                    printf("Sent CRC_INDICATION: SFN=%d, Slot=%d, NumCrcs=%d (immediate response)\n", 
                           ul_tti_req.SFN, ul_tti_req.Slot, num_pusch);
                    for (int i = 0; i < num_pusch; i++)
                    {
                        printf("  CRC %d: RNTI=0x%04x\n", i, scheduled_rntis[i]);
                    }
                }
            }
        }
        break;
    }
    case NFAPI_NR_PHY_MSG_TYPE_UL_DCI_REQUEST:
    {
        nfapi_nr_ul_dci_request_t ul_dci_req;
        nfapi_nr_p7_message_unpack(buffer, recv_len, &ul_dci_req, sizeof(ul_dci_req), 0);
        printf("Received UL_DCI_REQUEST: SFN=%d, Slot=%d, NumPdus=%d\n",
               ul_dci_req.SFN, ul_dci_req.Slot, ul_dci_req.numPdus);
        break;
    }
    default:
        printf("Received unknown message: 0x%02x\n", header.message_id);
        break;
    }
    
    return 1;
}

#define SLOT_INTERVAL_NS 500000L

int main(int argc, char *argv[])
{
    quiet_output = argc >= 5;
    if (argc < 4)
    {
        printf("Usage: %s <local_port> <proxy_port> <pnf_index> [iterations [payload_size]]\n", argv[0]);
        return 1;
    }
    
    uint16_t local_port = atoi(argv[1]);
    uint16_t proxy_port = atoi(argv[2]);
    int pnf_index = atoi(argv[3]);
    if (argc >= 6) test_payload_size = atoi(argv[5]);
    if (test_payload_size <= 0 || test_payload_size > 60 * 1024)
    {
        fprintf(stderr, "Invalid payload size: %d\n", test_payload_size);
        return 1;
    }
    bool is_primary = (pnf_index == 0); // PNF 0 is primary
    
    printf("PNF Test Stub %d starting on port %d, proxy port %d%s\n", 
           pnf_index, local_port, proxy_port, is_primary ? " (PRIMARY)" : "");
    
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
    
    int sock = create_udp_socket(local_port);
    if (sock < 0)
    {
        return 1;
    }
    
    // Set socket to non-blocking for receive
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    
    struct sockaddr_in proxy_addr;
    memset(&proxy_addr, 0, sizeof(proxy_addr));
    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_port = htons(proxy_port);
    inet_pton(AF_INET, "127.0.0.1", &proxy_addr.sin_addr);
    
    uint16_t sfn = 0;
    uint16_t slot = 0;

    if (argc < 5) sleep(2); // One-exchange peers may be started in any order.

    if (argc >= 5)
    {
        int iterations = atoi(argv[4]);
        struct timespec next_tick;
        clock_gettime(CLOCK_MONOTONIC, &next_tick);

        for (int iter = 0; iter < iterations && !terminate; iter++)
        {
            int absolute_slot = 4 + iter;
            uint16_t indicate_sfn = (absolute_slot / 20) % 1024;
            uint16_t indicate_slot = absolute_slot % 20;
            send_slot_indication(sock, &proxy_addr, indicate_sfn, indicate_slot);
            if (is_primary && iter == 0)
                send_rach_indication(sock, &proxy_addr, indicate_sfn, indicate_slot);

            next_tick.tv_nsec += SLOT_INTERVAL_NS;
            if (next_tick.tv_nsec >= 1000000000L)
            {
                next_tick.tv_nsec -= 1000000000L;
                next_tick.tv_sec++;
            }

            struct timespec now;
            do
            {
                receive_and_process_request(sock, pnf_index, &proxy_addr);
                clock_gettime(CLOCK_MONOTONIC, &now);
            } while (!terminate &&
                     (now.tv_sec < next_tick.tv_sec ||
                      (now.tv_sec == next_tick.tv_sec && now.tv_nsec < next_tick.tv_nsec)));
        }

        close(sock);
        return 0;
    }
    
    // Single exchange test: send one SLOT_INDICATION, wait for requests, respond, and exit
    
    // Calculate slot to indicate (4 slots ahead)
    uint16_t indicate_sfn = sfn;
    uint16_t indicate_slot = slot + 4;
    if (indicate_slot >= 20)
    {
        indicate_slot -= 20;
        indicate_sfn++;
        if (indicate_sfn >= 1024)
        {
            indicate_sfn = 0;
        }
    }
    
    // Send SLOT_INDICATION (4 slots ahead of current)
    printf("PNF %d sending SLOT_INDICATION: SFN=%d, Slot=%d\n", pnf_index, indicate_sfn, indicate_slot);
    send_slot_indication(sock, &proxy_addr, indicate_sfn, indicate_slot);
    
    // Send RACH indication from primary PNF
    if (is_primary)
    {
        usleep(5000);
        send_rach_indication(sock, &proxy_addr, indicate_sfn, indicate_slot);
    }
    
    // Wait for and process incoming requests from VNF
    printf("PNF %d waiting for requests...\n", pnf_index);
    int requests_received = 0;
    for (int i = 0; i < 500 && !terminate; i++)
    {
        int ret = receive_and_process_request(sock, pnf_index, &proxy_addr);
        if (ret > 0)
        {
            requests_received++;
            // Primary PNF expects DL_TTI, TX_DATA, UL_TTI, UL_DCI (4 requests)
            // Secondary PNF expects DL_TTI, TX_DATA, UL_TTI (3 requests)
            int expected_requests = is_primary ? 4 : 3;
            if (requests_received >= expected_requests)
            {
                printf("PNF %d received %d requests, responses sent\n", pnf_index, requests_received);
                break;
            }
        }
        usleep(10000); // Wait 10ms between polls
    }
    
    // Give some time for responses to be delivered
    usleep(100000);
    
    close(sock);
    printf("PNF Test Stub %d stopped\n", pnf_index);
    return 0;
}
