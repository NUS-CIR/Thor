/*
 * Copyright (c) National University of Singapore
 * Licensed under the MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>

#define DEFAULT_CONTROL_SOCKET_PATH "/var/run/thor_nfapi_proxy.sock"
#define RESPONSE_TIMEOUT_SECONDS 2

struct control_command
{
    char cmd[16];  // Command type: "add_du", "remove_du", "list_dus", "status"
    char arg0[16]; // RNTI in string format
    char arg1[16]; // PNF index or other integer argument
};

// Control response structure
struct control_response
{
    int status;
    char message[256];
};

void print_usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s migrate <decimal RNTI> <L1 ID> - Migrate a UE to a ready L1\n", prog);
    printf("  %s list_l1                        - List connected L1 IDs and readiness\n", prog);
    printf("  %s set_ready <L1 ID>              - Admit a connected L1 to P7 routing\n", prog);
    printf("  %s set_not_ready <L1 ID>          - Remove an L1 from P7 routing before drain\n", prog);
    printf("  %s debug <on/off>                 - Enable or disable debug mode\n", prog);
}

int send_command(struct control_command *cmd, struct control_response *resp)
{
    int sock_fd;
    struct sockaddr_un addr;

    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0)
    {
        perror("socket");
        return -1;
    }

    // Set up server address. Match the proxy's environment override.
    const char *control_socket_path = getenv("THOR_CTRL_SOCK");
    if (control_socket_path == NULL || *control_socket_path == '\0')
        control_socket_path = DEFAULT_CONTROL_SOCKET_PATH;
    if (strlen(control_socket_path) >= sizeof(addr.sun_path))
    {
        fprintf(stderr, "Control socket path is too long: %s\n", control_socket_path);
        close(sock_fd);
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, control_socket_path, sizeof(addr.sun_path) - 1);
    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("connect");
        close(sock_fd);
        return -1;
    }

    char request[64];
    int request_length;
    if (cmd->arg1[0] != '\0')
        request_length = snprintf(request, sizeof(request), "%s %s %s\n",
                                  cmd->cmd, cmd->arg0, cmd->arg1);
    else if (cmd->arg0[0] != '\0')
        request_length = snprintf(request, sizeof(request), "%s %s\n",
                                  cmd->cmd, cmd->arg0);
    else
        request_length = snprintf(request, sizeof(request), "%s\n", cmd->cmd);
    if (request_length < 0 || request_length >= (int)sizeof(request))
    {
        fprintf(stderr, "Control command is too long\n");
        close(sock_fd);
        return -1;
    }

    size_t sent = 0;
    while (sent < (size_t)request_length)
    {
        ssize_t n = send(sock_fd, request + sent,
                         (size_t)request_length - sent, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            perror("send");
            close(sock_fd);
            return -1;
        }
        sent += (size_t)n;
    }
    shutdown(sock_fd, SHUT_WR);

    // Wait for response with timeout
    struct timeval tv;
    tv.tv_sec = RESPONSE_TIMEOUT_SECONDS;
    tv.tv_usec = 0;
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char reply[320] = {0};
    size_t used = 0;
    while (used < sizeof(reply) - 1)
    {
        ssize_t n = recv(sock_fd, reply + used, sizeof(reply) - 1 - used, 0);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            perror("recv");
            close(sock_fd);
            return -1;
        }
        if (n == 0) break;
        used += (size_t)n;
        reply[used] = '\0';
        if (strstr(reply, "\n.\n") != NULL) break;
    }
    if (used == 0 || (strncmp(reply, "OK", 2) != 0 && strncmp(reply, "ERR", 3) != 0))
    {
        fprintf(stderr, "Invalid response from control socket\n");
        close(sock_fd);
        return -1;
    }

    memset(resp, 0, sizeof(*resp));
    char *newline = strchr(reply, '\n');
    if (newline != NULL) *newline = '\0';
    char *message;
    if (strncmp(reply, "OK", 2) == 0)
    {
        resp->status = 0;
        message = reply + 2;
    }
    else
    {
        resp->status = -1;
        message = reply + 3;
    }
    if (*message == ' ') message++;
    snprintf(resp->message, sizeof(resp->message), "%s", message);

    close(sock_fd);
    return 0;
}

int main(int argc, char *argv[])
{
    struct control_command cmd;
    struct control_response resp;

    if (argc < 2)
    {
        print_usage(argv[0]);
        return 1;
    }

    memset(&cmd, 0, sizeof(cmd));

    // list of supported commands
    // 1. migrate <RNTI> <PNF>
    // 2. list_l1
    // 3. set_ready|set_not_ready <L1 ID>
    // 4. debug <on|off>
    if (strcmp(argv[1], "migrate") == 0)
    {
        if (argc != 4)
        {
            print_usage(argv[0]);
            return 1;
        }
        strncpy(cmd.cmd, "migrate", sizeof(cmd.cmd) - 1);
        strncpy(cmd.arg0, argv[2], sizeof(cmd.arg0) - 1); // RNTI
        strncpy(cmd.arg1, argv[3], sizeof(cmd.arg1) - 1); // PNF index
    }
    else if (strcmp(argv[1], "list_l1") == 0)
    {
        if (argc != 2)
        {
            print_usage(argv[0]);
            return 1;
        }
        strncpy(cmd.cmd, "list_l1", sizeof(cmd.cmd) - 1);
    }
    else if (strcmp(argv[1], "set_ready") == 0 || strcmp(argv[1], "set_not_ready") == 0)
    {
        if (argc != 3)
        {
            print_usage(argv[0]);
            return 1;
        }
        strncpy(cmd.cmd, argv[1], sizeof(cmd.cmd) - 1);
        strncpy(cmd.arg0, argv[2], sizeof(cmd.arg0) - 1);
    }
    else if (strcmp(argv[1], "debug") == 0)
    {
        if (argc != 3)
        {
            print_usage(argv[0]);
            return 1;
        }
        strncpy(cmd.cmd, "debug", sizeof(cmd.cmd) - 1);
        if (strcmp(argv[2], "on") == 0)
        {
            strncpy(cmd.arg0, "on", sizeof(cmd.arg0) - 1);
        }
        else if (strcmp(argv[2], "off") == 0)
        {
            strncpy(cmd.arg0, "off", sizeof(cmd.arg0) - 1);
        }
        else
        {
            fprintf(stderr, "Invalid argument for debug command: %s\n", argv[2]);
            print_usage(argv[0]);
            return 1;
        }
    }
    else
    {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        print_usage(argv[0]);
        return 1;
    }

    if (send_command(&cmd, &resp) < 0)
    {
        fprintf(stderr, "Error: Failed to communicate with nfapi-proxy\n");
        const char *control_socket_path = getenv("THOR_CTRL_SOCK");
        if (control_socket_path == NULL || *control_socket_path == '\0')
            control_socket_path = DEFAULT_CONTROL_SOCKET_PATH;
        fprintf(stderr, "Make sure nfapi-proxy is running and the control socket exists at %s\n",
                control_socket_path);
        return 1;
    }

    if (!resp.status)
    {
        if (strcmp(cmd.cmd, "list_l1") == 0)
            fprintf(stdout, "%s\n", resp.message);
        else
            fprintf(stdout, "OK\n");
        return 0;
    }
    else
    {
        fprintf(stderr, "Error: %s\n", resp.message);
        return 1;
    }
}
