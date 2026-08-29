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

#include "nfapi_nr_interface.h"
#include "nfapi_nr_interface_scf.h"

// P7
int create_p7_connected_udp_socket(struct sockaddr_in* src_addr, struct sockaddr_in* dst_addr);
int peek_p7_udp_message_size(int sock);
int read_p7_udp_message(int sock, uint8_t* buffer, uint32_t buffer_size);

// P5
int create_p5_sctp_socket();
int peek_p5_sctp_message_size(int sock, struct sockaddr_in* addr, socklen_t* addr_len, struct sctp_sndrcvinfo* sndrcvinfo);
int read_p5_sctp_message(int sock, uint8_t* buffer, uint32_t buffer_size, struct sockaddr_in* addr, socklen_t* addr_len, struct sctp_sndrcvinfo* sndrcvinfo);