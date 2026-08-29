/*
 * Copyright (c) National University of Singapore.
 * Licensed under the MIT License
 */

#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_ring.h>
#include <rte_thread.h>

#include "thor_ctrl.h"

#define CTRL_MAX_CLIENTS 8
#define CTRL_MAX_INFLIGHT 16
#define CTRL_LINE_MAX 512
#define CTRL_RING_SIZE 32
/* How long the control thread waits for the lcore to apply a command. */
#define CTRL_ACK_TIMEOUT_MS 2000

enum ctrl_cmd_type
{
    CTRL_CMD_ADD_DU,
    CTRL_CMD_DEL_DU,
    CTRL_CMD_LIST,
    CTRL_CMD_STATS,
    CTRL_CMD_PING,
};

struct ctrl_du_entry
{
    uint8_t active;
    struct rte_ether_addr mac;
    uint16_t vlan;
};

/*
 * One request/response slot. Ownership moves between the two threads: the
 * control thread fills in the request half, the lcore fills in the response
 * half. Slots come from free_ring and are only recycled once the reply has
 * been read, so a slot is never touched by both threads at once.
 */
struct ctrl_msg
{
    enum ctrl_cmd_type type;
    struct rte_ether_addr mac;
    uint16_t vlan;

    int status; /* 0 on success, negative errno otherwise */
    int slot;
    int active_dus;
    struct ctrl_du_entry dus[MAX_NUM_DUS];
    struct middlebox_stats stats;
};

/* Comfortably inside sockaddr_un::sun_path (108 on Linux), with room for NUL. */
#define CTRL_SOCK_PATH_MAX 104

struct thor_ctrl
{
    char sock_path[CTRL_SOCK_PATH_MAX];
    int listen_fd;
    rte_thread_t thread;
    atomic_bool running;

    struct rte_ring *req_ring;  /* control thread -> lcore */
    struct rte_ring *resp_ring; /* lcore -> control thread */
    struct rte_ring *free_ring; /* slot allocator, control thread only */

    struct ctrl_msg slots[CTRL_MAX_INFLIGHT];
};

/* ------------------------------------------------------------------------- */
/* lcore side                                                                */
/* ------------------------------------------------------------------------- */

void thor_ctrl_poll(struct thor_ctrl *ctrl, struct middlebox_config *config)
{
    if (ctrl == NULL)
        return;

    void *ptr;
    while (rte_ring_dequeue(ctrl->req_ring, &ptr) == 0)
    {
        struct ctrl_msg *msg = ptr;

        msg->status = 0;
        msg->slot = -1;

        switch (msg->type)
        {
        case CTRL_CMD_ADD_DU:
            msg->slot = thor_du_add(config, &msg->mac, msg->vlan);
            if (msg->slot < 0)
                msg->status = msg->slot;
            break;

        case CTRL_CMD_DEL_DU:
            msg->slot = thor_du_del(config, &msg->mac);
            if (msg->slot < 0)
                msg->status = msg->slot;
            break;

        case CTRL_CMD_STATS:
            msg->stats = config->stats;
            break;

        case CTRL_CMD_LIST:
        case CTRL_CMD_PING:
            break;
        }

        /* Always report the resulting table so replies are self-describing. */
        msg->active_dus = config->num_dus;
        for (int i = 0; i < MAX_NUM_DUS; i++)
        {
            msg->dus[i].active = config->du_configs[i].active ? 1 : 0;
            msg->dus[i].mac = config->du_configs[i].du_addr;
            msg->dus[i].vlan = config->du_configs[i].vlan;
        }

        if (rte_ring_enqueue(ctrl->resp_ring, msg) != 0)
        {
            /* Cannot happen: the response ring is as deep as the slot pool. The
             * slot is deliberately leaked rather than freed from this thread. */
            fprintf(stderr, "thor_ctrl: response ring full, dropping reply\n");
        }
    }
}

/* ------------------------------------------------------------------------- */
/* control thread side                                                       */
/* ------------------------------------------------------------------------- */

static struct ctrl_msg *ctrl_slot_get(struct thor_ctrl *ctrl)
{
    void *ptr;
    if (rte_ring_dequeue(ctrl->free_ring, &ptr) != 0)
        return NULL;
    return ptr;
}

static void ctrl_slot_put(struct thor_ctrl *ctrl, struct ctrl_msg *msg)
{
    rte_ring_enqueue(ctrl->free_ring, msg);
}

/*
 * Hand a request to the lcore and wait for it to come back. Returns 0 on
 * success, -ETIMEDOUT if the datapath never picked it up (in which case the
 * slot is leaked on purpose -- the lcore may still be holding it).
 */
static int ctrl_submit(struct thor_ctrl *ctrl, struct ctrl_msg *msg)
{
    if (rte_ring_enqueue(ctrl->req_ring, msg) != 0)
        return -EAGAIN;

    const struct timespec nap = {.tv_sec = 0, .tv_nsec = 200 * 1000}; /* 200us */
    for (int waited_us = 0; waited_us < CTRL_ACK_TIMEOUT_MS * 1000; waited_us += 200)
    {
        void *ptr;
        if (rte_ring_dequeue(ctrl->resp_ring, &ptr) == 0)
        {
            /* Commands are submitted one at a time, so this is our reply. */
            return (ptr == msg) ? 0 : -EPROTO;
        }
        nanosleep(&nap, NULL);
    }
    return -ETIMEDOUT;
}

static int ctrl_write_all(int fd, const char *buf, size_t len)
{
    while (len > 0)
    {
        ssize_t n = write(fd, buf, len);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static int ctrl_printf(int fd, const char *fmt, ...)
{
    char buf[CTRL_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if (n >= (int)sizeof(buf))
        n = (int)sizeof(buf) - 1;
    return ctrl_write_all(fd, buf, (size_t)n);
}

/* Emit a complete single-line error reply, terminator included. */
static int ctrl_err(int fd, const char *fmt, ...)
{
    char buf[CTRL_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;

    if (ctrl_printf(fd, "ERR %s\n", buf) < 0)
        return -1;
    return ctrl_printf(fd, ".\n");
}

static const char *ctrl_strerror(int status)
{
    switch (-status)
    {
    case EEXIST:
        return "du already registered";
    case ENOENT:
        return "no such du";
    case ENOSPC:
        return "no free du slot";
    case ETIMEDOUT:
        return "datapath did not respond";
    case EAGAIN:
        return "control queue full";
    default:
        return "unknown error";
    }
}

static void ctrl_format_mac(const struct rte_ether_addr *mac, char *out, size_t len)
{
    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac->addr_bytes[0], mac->addr_bytes[1], mac->addr_bytes[2],
             mac->addr_bytes[3], mac->addr_bytes[4], mac->addr_bytes[5]);
}

static void ctrl_reply_list(int fd, const struct ctrl_msg *msg)
{
    ctrl_printf(fd, "OK active_dus=%d\n", msg->active_dus);
    for (int i = 0; i < MAX_NUM_DUS; i++)
    {
        if (!msg->dus[i].active)
            continue;
        char mac[32];
        ctrl_format_mac(&msg->dus[i].mac, mac, sizeof(mac));
        ctrl_printf(fd, "du[%d] mac=%s vlan=%u\n", i, mac, msg->dus[i].vlan);
    }
    ctrl_printf(fd, ".\n");
}

static void ctrl_reply_stats(int fd, const struct ctrl_msg *msg)
{
    const struct middlebox_stats *s = &msg->stats;

    ctrl_printf(fd, "OK active_dus=%d\n", msg->active_dus);
    ctrl_printf(fd, "dl_to_ru=%lu\n", s->dl_to_ru);
    ctrl_printf(fd, "ul_from_ru=%lu\n", s->ul_from_ru);
    ctrl_printf(fd, "dropped_unmatched=%lu\n", s->dropped_unmatched);
    ctrl_printf(fd, "dropped_malformed=%lu\n", s->dropped_malformed);
    ctrl_printf(fd, "cache_evictions=%lu\n", s->cache_evictions);
    ctrl_printf(fd, "cache_flushes=%lu\n", s->cache_flushes);
    ctrl_printf(fd, "tx_failed=%lu\n", s->tx_failed);
    ctrl_printf(fd, "alloc_failed=%lu\n", s->alloc_failed);
    for (int i = 0; i < MAX_NUM_DUS; i++)
    {
        if (!msg->dus[i].active &&
            s->dl_cp_from_du[i] == 0 && s->dl_up_from_du[i] == 0 && s->ul_to_du[i] == 0)
            continue;
        ctrl_printf(fd, "du[%d] dl_cp=%lu dl_up=%lu ul=%lu\n",
                    i, s->dl_cp_from_du[i], s->dl_up_from_du[i], s->ul_to_du[i]);
    }
    ctrl_printf(fd, ".\n");
}

/* Returns 0 to keep the connection open, -1 to close it. */
static int ctrl_handle_line(struct thor_ctrl *ctrl, int fd, char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;

    char *end = line + strlen(line);
    while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';

    if (*line == '\0')
        return 0;

    if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0)
        return -1;

    struct ctrl_msg *msg = ctrl_slot_get(ctrl);
    if (msg == NULL)
    {
        ctrl_err(fd, "control queue exhausted");
        return 0;
    }
    memset(msg, 0, sizeof(*msg));

    char verb[16] = {0};
    char mac_str[64] = {0};
    unsigned int vlan = 0;
    int nfields = sscanf(line, "%15s %63s %u", verb, mac_str, &vlan);

    if (nfields >= 1 && strcmp(verb, "add") == 0)
    {
        if (nfields != 3)
        {
            ctrl_err(fd, "usage: add <mac> <vlan>");
            ctrl_slot_put(ctrl, msg);
            return 0;
        }
        if (rte_ether_unformat_addr(mac_str, &msg->mac) != 0)
        {
            ctrl_err(fd, "invalid mac address '%s'", mac_str);
            ctrl_slot_put(ctrl, msg);
            return 0;
        }
        if (vlan > VLAN_VID_MASK)
        {
            ctrl_err(fd, "vlan %u out of range", vlan);
            ctrl_slot_put(ctrl, msg);
            return 0;
        }
        msg->type = CTRL_CMD_ADD_DU;
        msg->vlan = (uint16_t)vlan;
    }
    else if (nfields >= 1 && strcmp(verb, "del") == 0)
    {
        if (nfields != 2)
        {
            ctrl_err(fd, "usage: del <mac>");
            ctrl_slot_put(ctrl, msg);
            return 0;
        }
        if (rte_ether_unformat_addr(mac_str, &msg->mac) != 0)
        {
            ctrl_err(fd, "invalid mac address '%s'", mac_str);
            ctrl_slot_put(ctrl, msg);
            return 0;
        }
        msg->type = CTRL_CMD_DEL_DU;
    }
    else if (nfields >= 1 && strcmp(verb, "list") == 0)
    {
        msg->type = CTRL_CMD_LIST;
    }
    else if (nfields >= 1 && strcmp(verb, "stats") == 0)
    {
        msg->type = CTRL_CMD_STATS;
    }
    else if (nfields >= 1 && strcmp(verb, "ping") == 0)
    {
        msg->type = CTRL_CMD_PING;
    }
    else
    {
        ctrl_err(fd, "unknown command '%s'", verb);
        ctrl_slot_put(ctrl, msg);
        return 0;
    }

    int ret = ctrl_submit(ctrl, msg);
    if (ret != 0)
    {
        ctrl_err(fd, "%s", ctrl_strerror(ret));
        if (ret != -ETIMEDOUT) /* on timeout the lcore may still own the slot */
            ctrl_slot_put(ctrl, msg);
        return 0;
    }

    if (msg->status != 0)
        ctrl_err(fd, "%s", ctrl_strerror(msg->status));
    else
    {
        switch (msg->type)
        {
        case CTRL_CMD_LIST:
            ctrl_reply_list(fd, msg);
            break;
        case CTRL_CMD_STATS:
            ctrl_reply_stats(fd, msg);
            break;
        case CTRL_CMD_PING:
            ctrl_printf(fd, "OK pong\n.\n");
            break;
        default:
            ctrl_printf(fd, "OK slot=%d active_dus=%d\n.\n", msg->slot, msg->active_dus);
            break;
        }
    }

    ctrl_slot_put(ctrl, msg);
    return 0;
}

struct ctrl_client
{
    int fd;
    char buf[CTRL_LINE_MAX];
    size_t len;
};

static void ctrl_client_close(struct ctrl_client *c)
{
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    c->len = 0;
}

/* Drain readable bytes and dispatch every complete line. */
static void ctrl_client_read(struct thor_ctrl *ctrl, struct ctrl_client *c)
{
    ssize_t n = read(c->fd, c->buf + c->len, sizeof(c->buf) - c->len - 1);
    if (n <= 0)
    {
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            return;
        ctrl_client_close(c);
        return;
    }
    c->len += (size_t)n;
    c->buf[c->len] = '\0';

    char *start = c->buf;
    char *nl;
    while ((nl = strchr(start, '\n')) != NULL)
    {
        *nl = '\0';
        if (ctrl_handle_line(ctrl, c->fd, start) < 0)
        {
            ctrl_client_close(c);
            return;
        }
        start = nl + 1;
    }

    /* Keep the partial tail. A line longer than the buffer is dropped. */
    size_t remaining = c->len - (size_t)(start - c->buf);
    if (remaining >= sizeof(c->buf) - 1)
    {
        ctrl_err(c->fd, "line too long");
        remaining = 0;
    }
    else if (remaining > 0)
        memmove(c->buf, start, remaining);
    c->len = remaining;
}

static uint32_t ctrl_thread_main(void *arg)
{
    struct thor_ctrl *ctrl = arg;
    struct ctrl_client clients[CTRL_MAX_CLIENTS];

    for (int i = 0; i < CTRL_MAX_CLIENTS; i++)
    {
        clients[i].fd = -1;
        clients[i].len = 0;
    }

    while (atomic_load(&ctrl->running))
    {
        struct pollfd pfds[CTRL_MAX_CLIENTS + 1];
        int idx_map[CTRL_MAX_CLIENTS + 1];
        int nfds = 0;

        pfds[nfds].fd = ctrl->listen_fd;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        idx_map[nfds] = -1;
        nfds++;

        for (int i = 0; i < CTRL_MAX_CLIENTS; i++)
        {
            if (clients[i].fd < 0)
                continue;
            pfds[nfds].fd = clients[i].fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            idx_map[nfds] = i;
            nfds++;
        }

        int ready = poll(pfds, (nfds_t)nfds, 200 /* ms, so shutdown is prompt */);
        if (ready <= 0)
            continue;

        for (int p = 0; p < nfds; p++)
        {
            if (pfds[p].revents == 0)
                continue;

            if (idx_map[p] < 0)
            {
                int fd = accept(ctrl->listen_fd, NULL, NULL);
                if (fd < 0)
                    continue;

                int placed = -1;
                for (int i = 0; i < CTRL_MAX_CLIENTS; i++)
                {
                    if (clients[i].fd < 0)
                    {
                        clients[i].fd = fd;
                        clients[i].len = 0;
                        placed = i;
                        break;
                    }
                }
                if (placed < 0)
                {
                    ctrl_err(fd, "too many control connections");
                    close(fd);
                }
                continue;
            }

            struct ctrl_client *c = &clients[idx_map[p]];
            if (pfds[p].revents & (POLLHUP | POLLERR | POLLNVAL))
            {
                ctrl_client_close(c);
                continue;
            }
            ctrl_client_read(ctrl, c);
        }
    }

    for (int i = 0; i < CTRL_MAX_CLIENTS; i++)
        ctrl_client_close(&clients[i]);

    return 0;
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * EAL falls back to the main lcore when the control cpuset works out empty --
 * for instance when the whole process was pinned with taskset to the datapath
 * core. That silently reintroduces the starvation the control cpuset exists to
 * avoid, so check once here rather than hanging on the first command.
 */
static void ctrl_check_affinity(rte_thread_t tid)
{
    rte_cpuset_t set;
    if (rte_thread_get_affinity_by_id(tid, &set) != 0)
        return;

    for (unsigned int lcore = 0; lcore < RTE_MAX_LCORE; lcore++)
    {
        if (!rte_lcore_is_enabled(lcore))
            continue;
        rte_cpuset_t lcore_set = rte_lcore_cpuset(lcore);
        for (unsigned int cpu = 0; cpu < CPU_SETSIZE; cpu++)
            if (CPU_ISSET(cpu, &lcore_set))
                CPU_CLR(cpu, &set);
    }

    if (CPU_COUNT(&set) == 0)
        fprintf(stderr,
                "thor_ctrl: WARNING control thread can only run on datapath "
                "core(s); commands will stall while the datapath holds the CPU\n");
}

struct thor_ctrl *thor_ctrl_start(const char *sock_path)
{
    struct thor_ctrl *ctrl = calloc(1, sizeof(*ctrl));
    if (ctrl == NULL)
    {
        fprintf(stderr, "thor_ctrl: out of memory\n");
        return NULL;
    }

    if (strlen(sock_path) >= sizeof(ctrl->sock_path))
    {
        fprintf(stderr, "thor_ctrl: socket path too long: %s\n", sock_path);
        free(ctrl);
        return NULL;
    }
    snprintf(ctrl->sock_path, sizeof(ctrl->sock_path), "%s", sock_path);

    ctrl->req_ring = rte_ring_create("thor_ctrl_req", CTRL_RING_SIZE, SOCKET_ID_ANY,
                                     RING_F_SP_ENQ | RING_F_SC_DEQ);
    ctrl->resp_ring = rte_ring_create("thor_ctrl_resp", CTRL_RING_SIZE, SOCKET_ID_ANY,
                                      RING_F_SP_ENQ | RING_F_SC_DEQ);
    ctrl->free_ring = rte_ring_create("thor_ctrl_free", CTRL_RING_SIZE, SOCKET_ID_ANY,
                                      RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (ctrl->req_ring == NULL || ctrl->resp_ring == NULL || ctrl->free_ring == NULL)
    {
        fprintf(stderr, "thor_ctrl: failed to create rings: %s\n", rte_strerror(rte_errno));
        goto fail;
    }

    for (int i = 0; i < CTRL_MAX_INFLIGHT; i++)
        rte_ring_enqueue(ctrl->free_ring, &ctrl->slots[i]);

    ctrl->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctrl->listen_fd < 0)
    {
        fprintf(stderr, "thor_ctrl: socket(): %s\n", strerror(errno));
        goto fail;
    }

    unlink(ctrl->sock_path); /* clear a socket left behind by a previous run */

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ctrl->sock_path);

    if (bind(ctrl->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "thor_ctrl: bind(%s): %s\n", ctrl->sock_path, strerror(errno));
        goto fail;
    }
    if (listen(ctrl->listen_fd, CTRL_MAX_CLIENTS) < 0)
    {
        fprintf(stderr, "thor_ctrl: listen(): %s\n", strerror(errno));
        goto fail;
    }

    atomic_store(&ctrl->running, true);
    /*
     * Deliberately not pthread_create(): that inherits the caller's affinity,
     * which is the datapath lcore. main() then raises that lcore to SCHED_RR 99
     * and busy-polls it forever, so an inherited-affinity control thread is
     * never scheduled and the socket stops being accept()ed. This places the
     * thread on the EAL control cpuset -- the init-time affinity minus lcores.
     */
    int rc = rte_thread_create_control(&ctrl->thread, "thor-ctrl",
                                       ctrl_thread_main, ctrl);
    if (rc != 0)
    {
        fprintf(stderr, "thor_ctrl: rte_thread_create_control(): %s\n",
                strerror(rc < 0 ? -rc : rc));
        atomic_store(&ctrl->running, false);
        goto fail;
    }
    ctrl_check_affinity(ctrl->thread);

    printf("Control socket listening on %s\n", ctrl->sock_path);
    return ctrl;

fail:
    if (ctrl->listen_fd > 0)
        close(ctrl->listen_fd);
    rte_ring_free(ctrl->req_ring);
    rte_ring_free(ctrl->resp_ring);
    rte_ring_free(ctrl->free_ring);
    free(ctrl);
    return NULL;
}

void thor_ctrl_stop(struct thor_ctrl *ctrl)
{
    if (ctrl == NULL)
        return;

    atomic_store(&ctrl->running, false);
    rte_thread_join(ctrl->thread, NULL);

    close(ctrl->listen_fd);
    unlink(ctrl->sock_path);

    rte_ring_free(ctrl->req_ring);
    rte_ring_free(ctrl->resp_ring);
    rte_ring_free(ctrl->free_ring);
    free(ctrl);
}
