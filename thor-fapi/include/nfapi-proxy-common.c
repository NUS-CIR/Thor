#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <netinet/sctp.h>

#include "log.h"

#include "nfapi-proxy-common.h"

#include "nfapi_nr_interface.h"
#include "nfapi_nr_interface_scf.h"

int create_p7_connected_udp_socket(struct sockaddr_in* src_addr, struct sockaddr_in* dst_addr) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        log_error("socket");
        return -1;
    }

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
        log_error("setsockopt");
        close(sock);
        return -1;
    }

    if (bind(sock, (struct sockaddr *)src_addr, sizeof(*src_addr)) < 0) {
        log_error("bind");
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)dst_addr, sizeof(*dst_addr)) < 0) {
        log_error("connect");
        close(sock);
        return -1;
    }
    return sock;
}

int peek_p7_udp_message_size(int sock) {
    uint32_t header_buffer_size = NFAPI_NR_P7_HEADER_LENGTH;   // NFAPI_NR_P7_HEADER_LENGTH
    uint8_t header_buffer[header_buffer_size];

    int flags = MSG_PEEK | MSG_DONTWAIT;
    int ret = recv(sock, header_buffer, header_buffer_size, flags);
    if(ret == -1) {
        log_error("recv");
        return -1;
    }

    if(ret < NFAPI_NR_P7_HEADER_LENGTH) {
        log_error("Failed to peek UDP message size errorno: %d", errno);
        return -1;
    }

    nfapi_nr_p7_message_header_t header;
    const bool result = nfapi_nr_p7_message_header_unpack(header_buffer, header_buffer_size, &header, sizeof(header), 0);
    if(!result) {
        log_error("Failed to unpack P7 message header");
        return -1;
    }
    return header.message_length;
}

int read_p7_udp_message(int sock, uint8_t* buffer, uint32_t buffer_size) {
    int flags = 0;
    int ret = recv(sock, buffer, buffer_size, flags);
    if(ret < 0) {
        log_error("recv");
        return -1;
    }
    return ret;
}

int create_p5_sctp_socket() {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
    if (sock < 0) {
        log_error("socket");
        return -1;
    }

    struct sctp_initmsg initmsg;
    memset(&initmsg, 0, sizeof(initmsg));
    initmsg.sinit_num_ostreams = 5;
    initmsg.sinit_max_instreams = 5;
    if (setsockopt(sock, IPPROTO_SCTP, SCTP_INITMSG, &initmsg, sizeof(initmsg)) < 0) {
        log_error("setsockopt");
        close(sock);
        return -1;
    }

    int no_delay = 1;
    if (setsockopt(sock, IPPROTO_SCTP, SCTP_NODELAY, &no_delay, sizeof(no_delay)) < 0) {
        log_error("setsockopt");
        close(sock);
        return -1;
    }

    struct sctp_event_subscribe events;
    memset(&events, 0, sizeof(events));
    events.sctp_data_io_event = 1;
    if (setsockopt(sock, IPPROTO_SCTP, SCTP_EVENTS, &events, sizeof(events)) < 0) {
        log_error("setsockopt");
        close(sock);
        return -1;
    }
    return sock;
}

int peek_p5_sctp_message_size(int sock, struct sockaddr_in* addr, socklen_t* addr_len, struct sctp_sndrcvinfo* sndrcvinfo) {
    uint32_t header_buffer_size = NFAPI_NR_P5_HEADER_LENGTH;   // NFAPI_NR_P5_HEADER_LENGTH
    uint8_t header_buffer[header_buffer_size];

    int flags = MSG_PEEK;
    int ret = sctp_recvmsg(sock, header_buffer, header_buffer_size, (struct sockaddr *)addr, addr_len, sndrcvinfo, &flags);
    if(ret == -1) {
        perror("sctp_recvmsg");
        return -1;
    }

    if(ret < NFAPI_NR_P5_HEADER_LENGTH) {
        log_error("Failed to peek sctp message size errorno: %d", errno);
        return -1;
    }

    nfapi_nr_p4_p5_message_header_t header;
    const bool result = nfapi_nr_p5_message_header_unpack(header_buffer, header_buffer_size, &header, sizeof(header), 0);
    if(!result) {
        log_error("Failed to unpack P5 message header");
        return -1;
    }

    return header.message_length + header_buffer_size;
}

int read_p5_sctp_message(int sock, uint8_t* buffer, uint32_t buffer_size, struct sockaddr_in* addr, socklen_t* addr_len, struct sctp_sndrcvinfo* sndrcvinfo) {
    int flags = 0;
    int ret = sctp_recvmsg(sock, buffer, buffer_size, (struct sockaddr *)addr, addr_len, sndrcvinfo, &flags);
    if(ret < 0) {
        log_error("sctp_recvmsg");
        return -1;
    }
    if (flags & MSG_NOTIFICATION) {
        log_error("Received SCTP notification");
        return -1;
    } 
    if ((flags & 0x80) != 0x80) {
        return -1;
    }
    return ret;
}