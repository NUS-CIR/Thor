#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>

#define main nfapi_proxy_program_main
#include "../nfapi-proxy.c"
#undef main

#define TEST_SFN 7
#define TEST_SLOT 3

static int pack_p7(void *message, uint8_t *buffer)
{
    int size = nfapi_nr_p7_message_pack(message, buffer, MAX_P7_MESSAGE_SIZE, 0);
    assert(size > 0);
    assert(nfapi_nr_p7_update_checksum(buffer, size) == 0);
    return size;
}

static void reset_proxy(void)
{
    reset_uplink_aggregation_locked();
    initialize_lookup_tables();
    initialize_socket_states();
    proxy_info.next_connection_order = 0;
    proxy_info.p7_uplink_sequence_num = 0;
}

static void connect_two_pnfs(void)
{
    assert(register_pnf_locked(0, -1));
    assert(!register_pnf_locked(1, -1));
    assert(set_pnf_ready_locked(1, true));
    assert(proxy_info.primary_pnf == 0);
}

static void set_dl_route(uint16_t rnti, uint16_t pnf)
{
    proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_prev[rnti] = pnf;
    proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_curr[rnti] = pnf;
    proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_next[rnti] = pnf;
}

static void set_ul_route(uint16_t rnti, uint16_t pnf)
{
    proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_prev[rnti] = pnf;
    proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[rnti] = pnf;
    proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_next[rnti] = pnf;
}

static void test_membership_lifecycle(void)
{
    char status[64];

    reset_proxy();
    assert(active_pnf_count_locked() == 0);
    assert(proxy_info.primary_pnf == -1);
    format_l1_list_locked(status, sizeof(status));
    assert(strcmp(status, "primary=-1;secondary=;not_ready=") == 0);

    assert(register_pnf_locked(0, -1));
    assert(active_pnf_count_locked() == 1);
    assert(proxy_info.primary_pnf == 0);
    format_l1_list_locked(status, sizeof(status));
    assert(strcmp(status, "primary=0;secondary=;not_ready=") == 0);

    assert(!register_pnf_locked(1, -1));
    assert(active_pnf_count_locked() == 2);
    assert(proxy_info.primary_pnf == 0);
    format_l1_list_locked(status, sizeof(status));
    assert(strcmp(status, "primary=0;secondary=1;not_ready=1") == 0);
    struct control_response migration = {0};
    handle_migration(0x1002, 1, &migration);
    assert(migration.status == -1);
    assert(set_pnf_ready_locked(1, true));
    format_l1_list_locked(status, sizeof(status));
    assert(strcmp(status, "primary=0;secondary=1;not_ready=") == 0);

    set_dl_route(0x1002, 1);
    set_ul_route(0x1002, 1);
    proxy_info.downlink_rnti_to_pnf.pdsch_to_tx_data_mapping[1][TEST_SFN][TEST_SLOT][0] = 1;
    proxy_info.uplink_fapi_info[TEST_SFN][TEST_SLOT].rx_ind_expected = 2;
    proxy_info.uplink_fapi_info[TEST_SFN][TEST_SLOT].rx_raw_buf[1] = malloc(8);
    disconnect_pnf_locked(1, "unit test");

    assert(active_pnf_count_locked() == 1);
    assert(proxy_info.primary_pnf == 0);
    assert(proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_curr[0x1002] == 0);
    assert(proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[0x1002] == 0);
    assert(proxy_info.downlink_rnti_to_pnf.pdsch_to_tx_data_mapping[1][TEST_SFN][TEST_SLOT][0] == UINT16_MAX);
    assert(proxy_info.uplink_fapi_info[TEST_SFN][TEST_SLOT].rx_ind_expected == 0);
    assert(proxy_info.uplink_fapi_info[TEST_SFN][TEST_SLOT].rx_raw_buf[1] == NULL);

    assert(!register_pnf_locked(1, -1));
    assert(!proxy_info.pnf_ready[1]);
    assert(set_pnf_ready_locked(1, true));
    set_dl_route(0x1001, 0);
    set_ul_route(0x1001, 0);
    disconnect_pnf_locked(0, "unit test primary removal");
    assert(proxy_info.primary_pnf == 1);
    assert(proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_curr[0x1001] == 1);
    assert(proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[0x1001] == 1);

    disconnect_pnf_locked(1, "unit test last removal");
    assert(proxy_info.primary_pnf == -1);
    assert(proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_curr[0x1001] == INVALID_PNF_ID);
    assert(register_pnf_locked(0, -1));
    assert(proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_curr[0x1001] == 0);
}

static void test_not_ready_p7_gate(void)
{
    int north[2];
    int primary[2];
    int secondary[2];
    reset_proxy();
    assert(register_pnf_locked(0, -1));
    assert(!register_pnf_locked(1, -1));
    assert(!proxy_info.pnf_ready[1]);
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, north) == 0);
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, primary) == 0);
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, secondary) == 0);
    proxy_info.p7_south_sock[0] = primary[0];
    proxy_info.p7_south_sock[1] = secondary[0];

    nfapi_nr_tx_data_request_t request = {0};
    request.header.message_id = NFAPI_NR_PHY_MSG_TYPE_TX_DATA_REQUEST;
    request.SFN = TEST_SFN;
    request.Slot = TEST_SLOT;
    request.Number_of_PDUs = 1;
    request.pdu_list[0].PDU_length = 4;
    request.pdu_list[0].PDU_index = 0;
    request.pdu_list[0].num_TLV = 1;
    request.pdu_list[0].TLVs[0].length = 4;
    memset(request.pdu_list[0].TLVs[0].value.direct, 0x5a, 4);

    uint8_t input[MAX_P7_MESSAGE_SIZE];
    int input_size = pack_p7(&request, input);
    assert(send(north[1], input, input_size, 0) == input_size);
    assert(nfapi_p7_process_north(north[0]) == 0);
    uint8_t output[MAX_P7_MESSAGE_SIZE];
    assert(recv(primary[1], output, sizeof(output), 0) == input_size);
    assert(recv(secondary[1], output, sizeof(output), MSG_DONTWAIT) == -1 && errno == EAGAIN);

    assert(set_pnf_ready_locked(1, true));
    assert(send(north[1], input, input_size, 0) == input_size);
    assert(nfapi_p7_process_north(north[0]) == 0);
    assert(recv(primary[1], output, sizeof(output), 0) == input_size);
    assert(recv(secondary[1], output, sizeof(output), 0) == input_size);

    close(north[0]);
    close(north[1]);
    close(primary[0]);
    close(primary[1]);
    close(secondary[0]);
    close(secondary[1]);
    proxy_info.p7_south_sock[0] = -1;
    proxy_info.p7_south_sock[1] = -1;
}

static void test_primary_promotion_then_id_zero_readd(void)
{
    char status[64];
    struct control_response migration = {0};

    reset_proxy();
    assert(register_pnf_locked(0, -1));
    assert(!register_pnf_locked(1, -1));
    assert(set_pnf_ready_locked(1, true));
    set_dl_route(0x1001, 0);
    set_ul_route(0x1001, 0);

    disconnect_pnf_locked(0, "unit test promote ID 1");
    assert(proxy_info.primary_pnf == 1);
    assert(proxy_info.pnf_ready[1]);
    assert(proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_curr[0x1001] == 1);
    assert(proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[0x1001] == 1);

    assert(!register_pnf_locked(0, -1));
    assert(proxy_info.primary_pnf == 1);
    assert(!proxy_info.pnf_ready[0]);
    format_l1_list_locked(status, sizeof(status));
    assert(strcmp(status, "primary=1;secondary=0;not_ready=0") == 0);

    handle_migration(0x1001, 0, &migration);
    assert(migration.status == -1);
    assert(proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_next[0x1001] == 1);
    assert(set_pnf_ready_locked(0, true));
    assert(proxy_info.primary_pnf == 1);
    memset(&migration, 0, sizeof(migration));
    handle_migration(0x1001, 0, &migration);
    assert(migration.status == 0);
    assert(proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_next[0x1001] == 0);
    format_l1_list_locked(status, sizeof(status));
    assert(strcmp(status, "primary=1;secondary=0;not_ready=") == 0);
}

static void test_mapping_scan_only_when_migration_pending(void)
{
    const uint16_t rnti = 0x1001;
    struct control_response migration = {0};

    reset_proxy();
    connect_two_pnfs();
    set_dl_route(rnti, 0);
    set_ul_route(rnti, 0);

    // A differing staged table alone must not trigger work on an idle slot.
    proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_next[rnti] = 1;
    proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_next[rnti] = 1;
    assert(!apply_pending_mapping_updates_locked());
    assert(proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_curr[rnti] == 0);
    assert(proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[rnti] == 0);

    handle_migration(rnti, 1, &migration);
    assert(migration.status == 0);
    assert(atomic_load(&proxy_info.mapping_update_pending));
    assert(apply_pending_mapping_updates_locked());
    assert(proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_curr[rnti] == 1);
    assert(proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[rnti] == 1);
    assert(!atomic_load(&proxy_info.mapping_update_pending));

    // Once consumed, later slots return immediately without another scan.
    assert(!apply_pending_mapping_updates_locked());
}

static void test_control_socket_permissions_and_cleanup(void)
{
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    assert(snprintf(path, sizeof(path), "/tmp/nfapi_proxy_unit_%ld.sock", (long)getpid()) > 0);
    unlink(path);

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(sock >= 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    assert(bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(sock, 1) == 0);
    assert(configure_control_socket_permissions(path) == 0);

    struct stat st;
    assert(lstat(path, &st) == 0);
    assert(S_ISSOCK(st.st_mode));
    assert((st.st_mode & 0777) == 0660);

    destroy_control_socket_at_path(&sock, path);
    assert(sock == -1);
    assert(lstat(path, &st) == -1 && errno == ENOENT);
}

static void test_control_stream_protocol(void)
{
    struct control_command cmd;
    struct control_response resp;

    char list[] = "list_l1\n";
    assert(parse_control_line(list, &cmd, &resp));
    assert(strcmp(cmd.cmd, "list_l1") == 0);

    char migrate[] = "migrate 4097 1\n";
    assert(parse_control_line(migrate, &cmd, &resp));
    assert(strcmp(cmd.arg0, "4097") == 0);
    assert(strcmp(cmd.arg1, "1") == 0);

    char missing_argument[] = "migrate 4097\n";
    assert(!parse_control_line(missing_argument, &cmd, &resp));
    char extra_argument[] = "list_l1 unexpected\n";
    assert(!parse_control_line(extra_argument, &cmd, &resp));

    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    memset(&resp, 0, sizeof(resp));
    snprintf(resp.message, sizeof(resp.message), "primary=0;secondary=1;not_ready=");
    assert(send_stream_reply(pair[0], &resp) == 0);
    char reply[128] = {0};
    assert(read(pair[1], reply, sizeof(reply) - 1) > 0);
    assert(strcmp(reply, "OK primary=0;secondary=1;not_ready=\n.\n") == 0);
    close(pair[0]);
    close(pair[1]);
}

static void test_control_socket_path_configuration(void)
{
    char too_long[sizeof(proxy_info.control_socket_path) + 1];
    memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';

    assert(set_control_socket_path(DEFAULT_CONTROL_SOCKET_PATH));
    assert(strcmp(proxy_info.control_socket_path,
                  "/var/run/thor_nfapi_proxy.sock") == 0);
    assert(set_control_socket_path("/tmp/nfapi_proxy_unit_override.sock"));
    assert(!set_control_socket_path(""));
    assert(!set_control_socket_path(too_long));
}

static void test_dl_tti_split(void)
{
    reset_proxy();
    connect_two_pnfs();
    set_dl_route(0x1001, 0);
    set_dl_route(0x1002, 1);

    nfapi_nr_dl_tti_request_t request = {0};
    request.header.message_id = NFAPI_NR_PHY_MSG_TYPE_DL_TTI_REQUEST;
    request.SFN = TEST_SFN;
    request.Slot = TEST_SLOT;
    request.dl_tti_request_body.nPDUs = 2;
    for (int i = 0; i < 2; i++)
    {
        request.dl_tti_request_body.dl_tti_pdu_list[i].PDUType = NFAPI_NR_DL_TTI_PDSCH_PDU_TYPE;
        request.dl_tti_request_body.dl_tti_pdu_list[i].PDUSize = sizeof(nfapi_nr_dl_tti_pdsch_pdu);
        request.dl_tti_request_body.dl_tti_pdu_list[i].pdsch_pdu.pdsch_pdu_rel15.rnti = 0x1001 + i;
        request.dl_tti_request_body.dl_tti_pdu_list[i].pdsch_pdu.pdsch_pdu_rel15.pduIndex = i;
    }

    uint8_t input[MAX_P7_MESSAGE_SIZE];
    int input_size = pack_p7(&request, input);
    uint8_t *outputs[MAX_NUM_PNF] = {0};
    int sizes[MAX_NUM_PNF] = {0};
    assert(handle_nfapi_dl_tti_request(input, input_size, proxy_info.pnf_list, MAX_NUM_PNF,
                                       proxy_info.primary_pnf, &proxy_info.downlink_rnti_to_pnf,
                                       outputs, sizes) == 0);
    assert(sizes[0] > 0 && sizes[1] > 0);

    for (int i = 0; i < MAX_NUM_PNF; i++)
    {
        nfapi_nr_dl_tti_request_t decoded = {0};
        assert(nfapi_nr_p7_message_unpack(outputs[i], sizes[i], &decoded, sizeof(decoded), 0));
        assert(decoded.dl_tti_request_body.nPDUs == 1);
        assert(decoded.dl_tti_request_body.dl_tti_pdu_list[0].pdsch_pdu.pdsch_pdu_rel15.rnti == 0x1001 + i);
    }
}

static void test_tx_data_mirroring(void)
{
    reset_proxy();
    connect_two_pnfs();

    nfapi_nr_tx_data_request_t request = {0};
    request.header.message_id = NFAPI_NR_PHY_MSG_TYPE_TX_DATA_REQUEST;
    request.SFN = TEST_SFN;
    request.Slot = TEST_SLOT;
    request.Number_of_PDUs = 1;
    request.pdu_list[0].PDU_length = 4;
    request.pdu_list[0].PDU_index = 0;
    request.pdu_list[0].num_TLV = 1;
    request.pdu_list[0].TLVs[0].length = 4;
    memset(request.pdu_list[0].TLVs[0].value.direct, 0x5a, 4);

    uint8_t input[MAX_P7_MESSAGE_SIZE];
    int input_size = pack_p7(&request, input);
    uint8_t *outputs[MAX_NUM_PNF] = {0};
    int sizes[MAX_NUM_PNF] = {0};
    assert(handle_nfapi_tx_data_request(input, input_size, proxy_info.pnf_list, MAX_NUM_PNF,
                                        proxy_info.primary_pnf, outputs, sizes) == 0);
    for (int i = 0; i < MAX_NUM_PNF; i++)
    {
        assert(outputs[i] == input);
        assert(sizes[i] == input_size);
    }
}

static void test_ul_tti_mirroring_and_expectations(void)
{
    reset_proxy();
    connect_two_pnfs();
    set_ul_route(0x1001, 0);
    set_ul_route(0x1002, 1);

    nfapi_nr_ul_tti_request_t request = {0};
    request.header.message_id = NFAPI_NR_PHY_MSG_TYPE_UL_TTI_REQUEST;
    request.SFN = TEST_SFN;
    request.Slot = TEST_SLOT;
    request.n_pdus = 2;
    request.n_ulsch = 2;
    for (int i = 0; i < 2; i++)
    {
        request.pdus_list[i].pdu_type = NFAPI_NR_UL_CONFIG_PUSCH_PDU_TYPE;
        request.pdus_list[i].pdu_size = sizeof(nfapi_nr_pusch_pdu_t);
        request.pdus_list[i].pusch_pdu.rnti = 0x1001 + i;
        request.pdus_list[i].pusch_pdu.pusch_data.harq_process_id = 0;
        request.pdus_list[i].pusch_pdu.pusch_data.new_data_indicator = 1;
    }

    uint8_t input[MAX_P7_MESSAGE_SIZE];
    int input_size = pack_p7(&request, input);
    uint8_t *outputs[MAX_NUM_PNF] = {0};
    int sizes[MAX_NUM_PNF] = {0};
    assert(handle_nfapi_ul_tti_request(input, input_size, proxy_info.pnf_list, MAX_NUM_PNF,
                                       proxy_info.primary_pnf, &proxy_info.uplink_rnti_to_pnf,
                                       &proxy_info.uplink_fapi_info[0][0], outputs, sizes) == 0);
    assert(proxy_info.uplink_fapi_info[TEST_SFN][TEST_SLOT].rx_ind_expected == 2);
    assert(proxy_info.uplink_fapi_info[TEST_SFN][TEST_SLOT].crc_ind_expected == 2);

    for (int pnf = 0; pnf < MAX_NUM_PNF; pnf++)
    {
        nfapi_nr_ul_tti_request_t decoded = {0};
        assert(sizes[pnf] > 0);
        assert(nfapi_nr_p7_message_unpack(outputs[pnf], sizes[pnf], &decoded, sizeof(decoded), 0));
        assert(decoded.n_pdus == 2);
        assert(decoded.pdus_list[pnf].pusch_pdu.rnti == 0x1001 + pnf);
        assert(decoded.pdus_list[1 - pnf].pusch_pdu.rnti == 0xFFF0);
    }
}

static void test_ul_dci_primary_only(void)
{
    reset_proxy();
    connect_two_pnfs();

    nfapi_nr_ul_dci_request_t request = {0};
    request.header.message_id = NFAPI_NR_PHY_MSG_TYPE_UL_DCI_REQUEST;
    request.SFN = TEST_SFN;
    request.Slot = TEST_SLOT;
    request.numPdus = 1;
    request.ul_dci_pdu_list[0].PDUType = 0;
    request.ul_dci_pdu_list[0].PDUSize = sizeof(nfapi_nr_ul_dci_request_pdus_t);

    uint8_t input[MAX_P7_MESSAGE_SIZE];
    int input_size = pack_p7(&request, input);
    uint8_t *outputs[MAX_NUM_PNF] = {0};
    int sizes[MAX_NUM_PNF] = {0};
    assert(handle_nfapi_ul_dci_request(input, input_size, proxy_info.pnf_list, MAX_NUM_PNF,
                                       proxy_info.primary_pnf, outputs, sizes) == 0);
    assert(outputs[0] == input && sizes[0] == input_size);
    assert(outputs[1] == NULL && sizes[1] == 0);
}

static void open_north_socket(int pair[2])
{
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0);
    proxy_info.p7_north_sock = pair[0];
}

static void process_uplink(void *message, int pnf_index)
{
    uint8_t buffer[MAX_P7_MESSAGE_SIZE];
    int size = pack_p7(message, buffer);
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0);
    assert(send(pair[1], buffer, size, 0) == size);
    assert(nfapi_p7_process_south(pair[0], pnf_index) == 0);
    close(pair[0]);
    close(pair[1]);
}

static uint16_t receive_message_id(int north_peer)
{
    uint8_t buffer[MAX_P7_MESSAGE_SIZE];
    int size = recv(north_peer, buffer, sizeof(buffer), 0);
    assert(size > 0);
    nfapi_nr_p7_message_header_t header = {0};
    assert(nfapi_nr_p7_message_header_unpack(buffer, size, &header, sizeof(header), 0));
    return header.message_id;
}

static void test_simple_uplink_indications(void)
{
    int north[2];
    reset_proxy();
    connect_two_pnfs();
    open_north_socket(north);

    nfapi_nr_slot_indication_scf_t slot = {0};
    slot.header.message_id = NFAPI_NR_PHY_MSG_TYPE_SLOT_INDICATION;
    slot.sfn = TEST_SFN;
    slot.slot = TEST_SLOT;
    process_uplink(&slot, 0);
    assert(receive_message_id(north[1]) == NFAPI_NR_PHY_MSG_TYPE_SLOT_INDICATION);

    process_uplink(&slot, 1);
    uint8_t scratch[64];
    assert(recv(north[1], scratch, sizeof(scratch), MSG_DONTWAIT) == -1 && errno == EAGAIN);

    nfapi_nr_rach_indication_t rach = {0};
    rach.header.message_id = NFAPI_NR_PHY_MSG_TYPE_RACH_INDICATION;
    rach.sfn = TEST_SFN;
    rach.slot = TEST_SLOT;
    process_uplink(&rach, 0);
    assert(receive_message_id(north[1]) == NFAPI_NR_PHY_MSG_TYPE_RACH_INDICATION);

    proxy_info.uplink_fapi_info[TEST_SFN][TEST_SLOT].uci_ind_expected = 1;
    nfapi_nr_uci_indication_t uci = {0};
    uci.header.message_id = NFAPI_NR_PHY_MSG_TYPE_UCI_INDICATION;
    uci.sfn = TEST_SFN;
    uci.slot = TEST_SLOT;
    process_uplink(&uci, 0);
    assert(receive_message_id(north[1]) == NFAPI_NR_PHY_MSG_TYPE_UCI_INDICATION);

    proxy_info.uplink_fapi_info[TEST_SFN][TEST_SLOT].srs_ind_expected = 1;
    nfapi_nr_srs_indication_t srs = {0};
    srs.header.message_id = NFAPI_NR_PHY_MSG_TYPE_SRS_INDICATION;
    srs.sfn = TEST_SFN;
    srs.slot = TEST_SLOT;
    process_uplink(&srs, 0);
    assert(receive_message_id(north[1]) == NFAPI_NR_PHY_MSG_TYPE_SRS_INDICATION);

    close(north[0]);
    close(north[1]);
    proxy_info.p7_north_sock = -1;
}

static int pack_crc(uint16_t rnti, uint8_t *buffer)
{
    nfapi_nr_crc_t crc = {0};
    crc.rnti = rnti;
    crc.harq_id = 0;
    crc.tb_crc_status = 1;
    nfapi_nr_crc_indication_t message = {0};
    message.header.message_id = NFAPI_NR_PHY_MSG_TYPE_CRC_INDICATION;
    message.sfn = TEST_SFN;
    message.slot = TEST_SLOT;
    message.number_crcs = 1;
    message.crc_list = &crc;
    return pack_p7(&message, buffer);
}

static int pack_rx(uint16_t rnti, uint8_t *buffer)
{
    uint8_t payload[4] = {1, 2, 3, 4};
    nfapi_nr_rx_data_pdu_t pdu = {0};
    pdu.rnti = rnti;
    pdu.harq_id = 0;
    pdu.pdu_length = sizeof(payload);
    pdu.pdu = payload;
    nfapi_nr_rx_data_indication_t message = {0};
    message.header.message_id = NFAPI_NR_PHY_MSG_TYPE_RX_DATA_INDICATION;
    message.sfn = TEST_SFN;
    message.slot = TEST_SLOT;
    message.number_of_pdus = 1;
    message.pdu_list = &pdu;
    return pack_p7(&message, buffer);
}

static void process_packed_uplink(uint8_t *buffer, int size, int pnf_index)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0);
    assert(send(pair[1], buffer, size, 0) == size);
    assert(nfapi_p7_process_south(pair[0], pnf_index) == 0);
    close(pair[0]);
    close(pair[1]);
}

static void assert_segment_pair(int north_peer, uint16_t message_id)
{
    uint8_t buffers[2][MAX_P7_MESSAGE_SIZE];
    nfapi_nr_p7_message_header_t headers[2] = {0};
    for (int i = 0; i < 2; i++)
    {
        int size = recv(north_peer, buffers[i], sizeof(buffers[i]), 0);
        assert(size > 0);
        assert(nfapi_nr_p7_message_header_unpack(buffers[i], size, &headers[i], sizeof(headers[i]), 0));
        assert(headers[i].message_id == message_id);
    }
    assert(NFAPI_P7_GET_MORE(headers[0].m_segment_sequence) !=
           NFAPI_P7_GET_MORE(headers[1].m_segment_sequence));
}

static void test_crc_rx_segmentation(void)
{
    int north[2];
    uint8_t first[MAX_P7_MESSAGE_SIZE];
    uint8_t second[MAX_P7_MESSAGE_SIZE];
    uint8_t scratch[64];

    reset_proxy();
    connect_two_pnfs();
    open_north_socket(north);

    proxy_info.uplink_fapi_info[TEST_SFN][TEST_SLOT].crc_ind_expected = 2;
    int first_size = pack_crc(0x1001, first);
    int second_size = pack_crc(0x1002, second);
    process_packed_uplink(first, first_size, 0);
    assert(recv(north[1], scratch, sizeof(scratch), MSG_DONTWAIT) == -1 && errno == EAGAIN);
    process_packed_uplink(second, second_size, 1);
    assert_segment_pair(north[1], NFAPI_NR_PHY_MSG_TYPE_CRC_INDICATION);
    assert(proxy_info.uplink_fapi_info[TEST_SFN][TEST_SLOT].crc_ind_expected == 0);

    proxy_info.uplink_fapi_info[TEST_SFN][TEST_SLOT].rx_ind_expected = 2;
    first_size = pack_rx(0x1001, first);
    second_size = pack_rx(0x1002, second);
    process_packed_uplink(first, first_size, 0);
    assert(recv(north[1], scratch, sizeof(scratch), MSG_DONTWAIT) == -1 && errno == EAGAIN);
    process_packed_uplink(second, second_size, 1);
    assert_segment_pair(north[1], NFAPI_NR_PHY_MSG_TYPE_RX_DATA_INDICATION);
    assert(proxy_info.uplink_fapi_info[TEST_SFN][TEST_SLOT].rx_ind_expected == 0);

    close(north[0]);
    close(north[1]);
    proxy_info.p7_north_sock = -1;
}

static void test_p5_failure_eviction(void)
{
    reset_proxy();
    assert(nfapi_p5_process_south(-1, 0) == -1);

    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    int stale_fd = pair[0];
    close(pair[0]);
    close(pair[1]);
    assert(register_pnf_locked(0, stale_fd));
    proxy_info.p5_north_sock_state = P5_STATE_PNF_PARAM_REQUEST;
    assert(nfapi_send_p5_north_to_south() == 0);
    assert(active_pnf_count_locked() == 0);
    assert(proxy_info.primary_pnf == -1);
}

static void open_p5_pair(int pair[2])
{
    int listener = create_p5_sctp_socket();
    assert(listener >= 0);

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listener, 1) == 0);

    socklen_t address_len = sizeof(address);
    assert(getsockname(listener, (struct sockaddr *)&address, &address_len) == 0);
    pair[1] = create_p5_sctp_socket();
    assert(pair[1] >= 0);
    assert(connect(pair[1], (struct sockaddr *)&address, sizeof(address)) == 0);
    pair[0] = accept(listener, NULL, NULL);
    assert(pair[0] >= 0);
    close(listener);
}

static void test_p5_stop_indication_eviction(void)
{
    reset_proxy();
    assert(register_pnf_locked(0, -1));

    int p5_pair[2];
    open_p5_pair(p5_pair);
    assert(!register_pnf_locked(1, p5_pair[0]));

    set_dl_route(0x1002, 1);
    set_ul_route(0x1002, 1);
    proxy_info.downlink_rnti_to_pnf.pdsch_to_tx_data_mapping[1][TEST_SFN][TEST_SLOT][0] = 1;
    proxy_info.uplink_fapi_info[TEST_SFN][TEST_SLOT].crc_ind_expected = 2;
    proxy_info.uplink_fapi_info[TEST_SFN][TEST_SLOT].crc_raw_buf[1] = malloc(8);

    nfapi_nr_stop_indication_scf_t stop = {0};
    stop.header.message_id = NFAPI_NR_PHY_MSG_TYPE_STOP_INDICATION;
    uint8_t buffer[64];
    int size = nfapi_nr_p5_message_pack(&stop, sizeof(stop), buffer, sizeof(buffer), 0);
    assert(size > 0);
    assert(sctp_sendmsg(p5_pair[1], buffer, size, NULL, 0, 0, 0, 0, 0, 0) == size);

    assert(nfapi_p5_process_south(p5_pair[0], 1) == 0);
    assert(active_pnf_count_locked() == 1);
    assert(proxy_info.primary_pnf == 0);
    assert(proxy_info.pnf_list[1] == -1);
    assert(proxy_info.p5_south_sock[1] == -1);
    assert(proxy_info.downlink_rnti_to_pnf.ue_rnti_to_pnf_curr[0x1002] == 0);
    assert(proxy_info.uplink_rnti_to_pnf.ue_rnti_to_pnf_curr[0x1002] == 0);
    assert(proxy_info.downlink_rnti_to_pnf.pdsch_to_tx_data_mapping[1][TEST_SFN][TEST_SLOT][0] == UINT16_MAX);
    assert(proxy_info.uplink_fapi_info[TEST_SFN][TEST_SLOT].crc_ind_expected == 0);
    assert(proxy_info.uplink_fapi_info[TEST_SFN][TEST_SLOT].crc_raw_buf[1] == NULL);

    close(p5_pair[1]);
}

int main(void)
{
    initialize_global_structures();
    initialize_lookup_tables();
    initialize_socket_states();
    log_set_level(LOG_ERROR);

    test_membership_lifecycle();
    test_p5_failure_eviction();
    test_p5_stop_indication_eviction();
    test_not_ready_p7_gate();
    test_primary_promotion_then_id_zero_readd();
    test_mapping_scan_only_when_migration_pending();
    test_control_socket_permissions_and_cleanup();
    test_control_socket_path_configuration();
    test_control_stream_protocol();
    test_dl_tti_split();
    test_tx_data_mirroring();
    test_ul_tti_mirroring_and_expectations();
    test_ul_dci_primary_only();
    test_simple_uplink_indications();
    test_crc_rx_segmentation();

    reset_uplink_aggregation_locked();
    pthread_mutex_destroy(&proxy_info.membership_mutex);
    puts("nfapi proxy unit tests: PASS");
    return 0;
}
