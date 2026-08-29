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

#include "nfapi_nr_interface.h"
#include "nfapi_nr_interface_scf.h"
#include "nr_fapi_p7_utils.h"

static volatile bool terminate = false;
static bool quiet_output = false;
static int test_payload_size = 1024;
static int test_ue_count = 3;
#define printf(...) (quiet_output ? 0 : fprintf(stdout, __VA_ARGS__))

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

// Send DL_TTI_REQUEST message with multiple UEs
int send_dl_tti_request(int sock, struct sockaddr_in *proxy_addr, uint16_t sfn, uint16_t slot)
{
    nfapi_nr_dl_tti_request_t dl_tti_req;
    memset(&dl_tti_req, 0, sizeof(dl_tti_req));
    
    dl_tti_req.header.message_id = NFAPI_NR_PHY_MSG_TYPE_DL_TTI_REQUEST;
    dl_tti_req.header.message_length = 0; // Will be set by pack function
    dl_tti_req.SFN = sfn;
    dl_tti_req.Slot = slot;
    dl_tti_req.dl_tti_request_body.nPDUs = test_ue_count;
    dl_tti_req.dl_tti_request_body.nGroup = 0;
    for (int i = 0; i < test_ue_count; i++)
    {
        nfapi_nr_dl_tti_request_pdu_t *pdu =
            &dl_tti_req.dl_tti_request_body.dl_tti_pdu_list[i];
        pdu->PDUType = NFAPI_NR_DL_TTI_PDSCH_PDU_TYPE;
        pdu->PDUSize = sizeof(nfapi_nr_dl_tti_pdsch_pdu);
        pdu->pdsch_pdu.pdsch_pdu_rel15.rnti = 0x1001 + i;
        pdu->pdsch_pdu.pdsch_pdu_rel15.pduIndex = i;
    }
    
    uint8_t buffer[65536];
    int packed_len = nfapi_nr_p7_message_pack(&dl_tti_req, buffer, sizeof(buffer), 0);
    if (packed_len <= 0)
    {
        fprintf(stderr, "Failed to pack DL_TTI_REQUEST\n");
        return -1;
    }
    
    nfapi_nr_p7_update_checksum(buffer, packed_len);
    
    int ret = sendto(sock, buffer, packed_len, 0, (struct sockaddr *)proxy_addr, sizeof(*proxy_addr));
    if (ret < 0)
    {
        perror("sendto DL_TTI_REQUEST");
        return -1;
    }
    
    printf("Sent DL_TTI_REQUEST: SFN=%d, Slot=%d, NumPdus=%d\n",
           sfn, slot, dl_tti_req.dl_tti_request_body.nPDUs);
    return 0;
}

// Send TX_DATA_REQUEST message with multiple UEs
int send_tx_data_request(int sock, struct sockaddr_in *proxy_addr, uint16_t sfn, uint16_t slot)
{
    nfapi_nr_tx_data_request_t tx_data_req;
    memset(&tx_data_req, 0, sizeof(tx_data_req));
    
    tx_data_req.header.message_id = NFAPI_NR_PHY_MSG_TYPE_TX_DATA_REQUEST;
    tx_data_req.header.message_length = 0;
    tx_data_req.SFN = sfn;
    tx_data_req.Slot = slot;
    tx_data_req.Number_of_PDUs = test_ue_count;
    
    // Add PDUs for each UE (matching DL_TTI order)
    for (int i = 0; i < test_ue_count; i++)
    {
        tx_data_req.pdu_list[i].PDU_length = test_payload_size;
        tx_data_req.pdu_list[i].PDU_index = i;
        tx_data_req.pdu_list[i].num_TLV = 1;
        tx_data_req.pdu_list[i].TLVs[0].length = test_payload_size;
        // Set dummy data with different patterns per UE
        memset(tx_data_req.pdu_list[i].TLVs[0].value.direct, 0xA0 + i,
               test_payload_size);
    }
    
    uint8_t buffer[65536];
    int packed_len = nfapi_nr_p7_message_pack(&tx_data_req, buffer, sizeof(buffer), 0);
    if (packed_len <= 0)
    {
        fprintf(stderr, "Failed to pack TX_DATA_REQUEST\n");
        return -1;
    }
    
    nfapi_nr_p7_update_checksum(buffer, packed_len);
    
    int ret = sendto(sock, buffer, packed_len, 0, (struct sockaddr *)proxy_addr, sizeof(*proxy_addr));
    if (ret < 0)
    {
        perror("sendto TX_DATA_REQUEST");
        return -1;
    }
    
    printf("Sent TX_DATA_REQUEST: SFN=%d, Slot=%d, NumPdus=%d\n", sfn, slot, tx_data_req.Number_of_PDUs);
    return 0;
}

// Send UL_TTI_REQUEST message with multiple UEs
int send_ul_tti_request(int sock, struct sockaddr_in *proxy_addr, uint16_t sfn, uint16_t slot)
{
    nfapi_nr_ul_tti_request_t ul_tti_req;
    memset(&ul_tti_req, 0, sizeof(ul_tti_req));
    
    ul_tti_req.header.message_id = NFAPI_NR_PHY_MSG_TYPE_UL_TTI_REQUEST;
    ul_tti_req.header.message_length = 0;
    ul_tti_req.SFN = sfn;
    ul_tti_req.Slot = slot;
    ul_tti_req.n_pdus = test_ue_count;
    ul_tti_req.n_ulsch = test_ue_count;
    ul_tti_req.n_ulcch = 0;
    ul_tti_req.n_group = 0;
    
    for (int i = 0; i < test_ue_count; i++)
    {
        nfapi_nr_ul_tti_request_number_of_pdus_t *pdu = &ul_tti_req.pdus_list[i];
        pdu->pdu_type = NFAPI_NR_UL_CONFIG_PUSCH_PDU_TYPE;
        pdu->pdu_size = sizeof(nfapi_nr_pusch_pdu_t);
        pdu->pusch_pdu.rnti = 0x1001 + i;
        pdu->pusch_pdu.handle = i;
        pdu->pusch_pdu.pusch_data.harq_process_id = 0;
        pdu->pusch_pdu.pusch_data.new_data_indicator = 1;
    }
    
    uint8_t buffer[65536];
    int packed_len = nfapi_nr_p7_message_pack(&ul_tti_req, buffer, sizeof(buffer), 0);
    if (packed_len <= 0)
    {
        fprintf(stderr, "Failed to pack UL_TTI_REQUEST\n");
        return -1;
    }
    
    nfapi_nr_p7_update_checksum(buffer, packed_len);
    
    int ret = sendto(sock, buffer, packed_len, 0, (struct sockaddr *)proxy_addr, sizeof(*proxy_addr));
    if (ret < 0)
    {
        perror("sendto UL_TTI_REQUEST");
        return -1;
    }
    
    printf("Sent UL_TTI_REQUEST: SFN=%d, Slot=%d, NumPdus=%d\n",
           sfn, slot, ul_tti_req.n_pdus);
    return 0;
}

// Send UL_DCI_REQUEST message
int send_ul_dci_request(int sock, struct sockaddr_in *proxy_addr, uint16_t sfn, uint16_t slot)
{
    nfapi_nr_ul_dci_request_t ul_dci_req;
    memset(&ul_dci_req, 0, sizeof(ul_dci_req));
    
    ul_dci_req.header.message_id = NFAPI_NR_PHY_MSG_TYPE_UL_DCI_REQUEST;
    ul_dci_req.header.message_length = 0;
    ul_dci_req.SFN = sfn;
    ul_dci_req.Slot = slot;
    ul_dci_req.numPdus = 1;
    
    // Add a simple DCI PDU with minimal structure
    ul_dci_req.ul_dci_pdu_list[0].PDUType = 0; // Generic PDU type
    ul_dci_req.ul_dci_pdu_list[0].PDUSize = sizeof(nfapi_nr_ul_dci_request_pdus_t);
    
    uint8_t buffer[65536];
    int packed_len = nfapi_nr_p7_message_pack(&ul_dci_req, buffer, sizeof(buffer), 0);
    if (packed_len <= 0)
    {
        fprintf(stderr, "Failed to pack UL_DCI_REQUEST\n");
        return -1;
    }
    
    nfapi_nr_p7_update_checksum(buffer, packed_len);
    
    int ret = sendto(sock, buffer, packed_len, 0, (struct sockaddr *)proxy_addr, sizeof(*proxy_addr));
    if (ret < 0)
    {
        perror("sendto UL_DCI_REQUEST");
        return -1;
    }
    
    printf("Sent UL_DCI_REQUEST: SFN=%d, Slot=%d\n", sfn, slot);
    return 0;
}

// Receive and process uplink indications, return slot info from SLOT_INDICATION
int receive_and_process_indication(int sock, uint16_t *sfn_out, uint16_t *slot_out, bool *got_slot_ind)
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
    
    uint8_t segment = NFAPI_P7_GET_SEGMENT(header.m_segment_sequence);
    uint8_t more = NFAPI_P7_GET_MORE(header.m_segment_sequence);
    if (segment > 0 || more)
    {
        printf("Received segmented message 0x%02x: segment=%u more=%u\n",
               header.message_id, segment, more);
        // Count only the final segment as one complete logical indication. The
        // unit suite validates the segment payload/count construction itself.
        return more ? 0 : 1;
    }

    switch (header.message_id)
    {
    case NFAPI_NR_PHY_MSG_TYPE_SLOT_INDICATION:
    {
        nfapi_nr_slot_indication_scf_t slot_ind;
        if (nfapi_nr_p7_message_unpack(buffer, recv_len, &slot_ind, sizeof(slot_ind), 0))
        {
            *sfn_out = slot_ind.sfn;
            *slot_out = slot_ind.slot;
            *got_slot_ind = true;
            printf("Received SLOT_INDICATION: SFN=%d, Slot=%d\n", slot_ind.sfn, slot_ind.slot);
        }
        break;
    }
    case NFAPI_NR_PHY_MSG_TYPE_RX_DATA_INDICATION:
        printf("Received RX_DATA_INDICATION\n");
        nfapi_nr_rx_data_indication_t rx_data_ind;
        if (nfapi_nr_p7_message_unpack(buffer, recv_len, &rx_data_ind, sizeof(rx_data_ind), 0))
        {
            // Process received data PDUs
            for (int i = 0; i < rx_data_ind.number_of_pdus; i++)
            {
                nfapi_nr_rx_data_pdu_t *pdu = &rx_data_ind.pdu_list[i];
                printf("  RX_DATA_INDICATION PDU %d: RNTI=0x%04x, Length=%d\n", i, pdu->rnti, pdu->pdu_length);
            }
        }
        break;
    case NFAPI_NR_PHY_MSG_TYPE_CRC_INDICATION:
        printf("Received CRC_INDICATION\n");
        nfapi_nr_crc_indication_t crc_ind;
        if (nfapi_nr_p7_message_unpack(buffer, recv_len, &crc_ind, sizeof(crc_ind), 0))
        {
            printf(" CRC_INDICATION: NumCRCs=%d\n", crc_ind.number_crcs);
        }
        break;
    case NFAPI_NR_PHY_MSG_TYPE_RACH_INDICATION:
        printf("Received RACH_INDICATION\n");
        break;
    case NFAPI_NR_PHY_MSG_TYPE_UCI_INDICATION:
        printf("Received UCI_INDICATION\n");
        break;
    case NFAPI_NR_PHY_MSG_TYPE_SRS_INDICATION:
        printf("Received SRS_INDICATION\n");
        break;
    default:
        printf("Received unknown message: 0x%02x\n", header.message_id);
        break;
    }
    
    return 1;
}

int main(int argc, char *argv[])
{
    quiet_output = argc >= 4;
    if (argc < 3)
    {
        printf("Usage: %s <local_port> <proxy_port> [iterations [payload_size [ue_count]]]\n", argv[0]);
        return 1;
    }
    
    uint16_t local_port = atoi(argv[1]);
    uint16_t proxy_port = atoi(argv[2]);
    if (argc >= 5) test_payload_size = atoi(argv[4]);
    if (argc >= 6) test_ue_count = atoi(argv[5]);
    if (test_payload_size <= 0 || test_ue_count <= 0 ||
        test_ue_count > NFAPI_NR_MAX_TX_REQUEST_PDUS ||
        (long)test_payload_size * test_ue_count > 60 * 1024)
    {
        fprintf(stderr, "Invalid payload/UE combination (UEs 1-%d, aggregate payload <= 60 KiB)\n",
                NFAPI_NR_MAX_TX_REQUEST_PDUS);
        return 1;
    }
    
    printf("VNF Test Stub starting on port %d, proxy port %d\n", local_port, proxy_port);
    
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

    if (argc < 4) sleep(2); // One-exchange peers may be started in any order.

    if (argc >= 4)
    {
        int iterations = atoi(argv[3]);
        bool have_last = false;
        uint16_t last_sfn = 0;
        uint16_t last_slot = 0;

        for (int iter = 0; iter < iterations && !terminate;)
        {
            uint16_t current_sfn = 0;
            uint16_t current_slot = 0;
            bool got_slot_ind = false;
            int receive_result = receive_and_process_indication(
                sock, &current_sfn, &current_slot, &got_slot_ind);
            if (receive_result < 0) break;

            if (!got_slot_ind ||
                (have_last && current_sfn == last_sfn && current_slot == last_slot))
            {
                usleep(50);
                continue;
            }

            have_last = true;
            last_sfn = current_sfn;
            last_slot = current_slot;
            int pattern_index = (current_sfn * 20 + current_slot) % 5;
            if (pattern_index <= 2)
            {
                send_dl_tti_request(sock, &proxy_addr, current_sfn, current_slot);
                send_tx_data_request(sock, &proxy_addr, current_sfn, current_slot);
            }
            else if (pattern_index == 4)
            {
                send_ul_tti_request(sock, &proxy_addr, current_sfn, current_slot);
                send_ul_dci_request(sock, &proxy_addr, current_sfn, current_slot);
            }
            iter++;
        }

        close(sock);
        return 0;
    }
    
    // Single exchange test: wait for one SLOT_INDICATION, send messages, wait for responses
    uint16_t sfn = 0;
    uint16_t slot = 0;
    bool got_slot_ind = false;
    
    // Poll for SLOT_INDICATION
    printf("VNF waiting for SLOT_INDICATION...\n");
    for (int i = 0; i < 1000 && !got_slot_ind && !terminate; i++)
    {
        if (receive_and_process_indication(sock, &sfn, &slot, &got_slot_ind) < 0)
        {
            break;
        }
        if (!got_slot_ind)
        {
            usleep(10000); // Wait 10ms between polls
        }
    }
    
    if (!got_slot_ind)
    {
        fprintf(stderr, "VNF did not receive SLOT_INDICATION, exiting\n");
        close(sock);
        return 1;
    }
    
    // Send downlink messages for the received slot
    printf("VNF sending messages for SFN=%d, Slot=%d\n", sfn, slot);
    send_dl_tti_request(sock, &proxy_addr, sfn, slot);
    usleep(1000);
    
    send_tx_data_request(sock, &proxy_addr, sfn, slot);
    usleep(1000);
    
    send_ul_tti_request(sock, &proxy_addr, sfn, slot);
    usleep(1000);
    
    send_ul_dci_request(sock, &proxy_addr, sfn, slot);
    usleep(1000);
    
    // Wait for responses (RX_DATA, CRC, RACH, etc.)
    printf("VNF waiting for responses...\n");
    int responses_received = 0;
    for (int i = 0; i < 500 && !terminate; i++)
    {
        uint16_t dummy_sfn, dummy_slot;
        bool dummy_got_slot;
        int ret = receive_and_process_indication(sock, &dummy_sfn, &dummy_slot, &dummy_got_slot);
        if (ret > 0)
        {
            responses_received++;
            // We expect at least RX_DATA and CRC (2 responses minimum)
            if (responses_received >= 3)
            {
                printf("VNF received %d responses, test complete\n", responses_received);
                break;
            }
        }
        usleep(10000); // Wait 10ms between polls
    }
    
    close(sock);
    printf("VNF Test Stub stopped\n");
    return 0;
}
