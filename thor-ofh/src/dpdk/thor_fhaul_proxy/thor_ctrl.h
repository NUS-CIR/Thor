/*
 * Copyright (c) National University of Singapore.
 * Licensed under the MIT License
 *
 * Runtime control channel for adding and removing L1s (DUs).
 *
 * A background thread serves a UNIX domain socket and speaks a line protocol.
 * It never touches the DU table itself: commands are handed to the datapath
 * lcore over a lock-free SPSC ring and applied between RX bursts, which is the
 * only point at which the merge cache can be safely invalidated. The thread
 * then blocks on a completion ring so each client gets a synchronous reply.
 *
 *   add <mac> <vlan>   ->  OK slot=<n> active_dus=<n>
 *   del <mac>          ->  OK slot=<n> active_dus=<n>
 *   list               ->  OK active_dus=<n> / du[<slot>] mac=<mac> vlan=<v> ...
 *   stats              ->  OK active_dus=<n> / <counter>=<value> ...
 *   ping               ->  OK pong
 *   quit               ->  closes the connection
 *
 * Every reply opens with a status line ("OK ..." or "ERR <reason>") and closes
 * with a lone "." line, so a client can frame a reply without knowing which
 * command produced it.
 */

#ifndef THOR_CTRL_H_
#define THOR_CTRL_H_

#include "thor_fhaul_proxy.h"

struct thor_ctrl;

/*
 * Bind sock_path and start the control thread. Any stale socket file at that
 * path is removed first. Returns NULL on failure (the reason is printed).
 */
struct thor_ctrl *thor_ctrl_start(const char *sock_path);

/*
 * Apply any pending control commands. Must be called from the datapath lcore,
 * outside of packet processing, since applying a command flushes the merge
 * cache. Cheap when idle: one empty-ring check.
 */
void thor_ctrl_poll(struct thor_ctrl *ctrl, struct middlebox_config *config);

/* Stop the control thread and unlink the socket. */
void thor_ctrl_stop(struct thor_ctrl *ctrl);

#endif /* THOR_CTRL_H_ */
