#!/bin/bash

# Copyright (c) National University of Singapore.
# Licensed under the MIT License.
#
# Add and remove L1s on a running thor_fhaul_proxy.
#
#   config/thor_ctl.sh list
#   config/thor_ctl.sh add 00:11:22:33:44:AB 5
#   config/thor_ctl.sh del 00:11:22:33:44:88
#   config/thor_ctl.sh stats
#   config/thor_ctl.sh ping
#
# Point it at another socket with THOR_CTRL_SOCK=/path/to.sock.
# The socket is root-owned, so this needs sudo.

set -u

CTRL_SOCK="${THOR_CTRL_SOCK:-/var/run/thor_fhaul_proxy.sock}"

usage() {
    sed -n '6,16p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-2}"
}

[ "$#" -lt 1 ] && usage

case "$1" in
    -h|--help|help) usage 0 ;;
esac

if [ ! -S "$CTRL_SOCK" ]; then
    echo "error: no control socket at $CTRL_SOCK" >&2
    echo "       is the middlebox running, and was it started with --ctrl-sock?" >&2
    exit 1
fi

if ! command -v nc >/dev/null 2>&1; then
    echo "error: nc (netcat) is required" >&2
    exit 1
fi

# One command per invocation. The trailing "quit" makes the proxy close the
# connection as soon as it has written the reply, so nc exits on EOF instead of
# waiting out a -q timer (which could also expire before a slow reply arrived).
# Replies still end with a lone "." line, so stop reading there. -w bounds a
# proxy that has stopped accepting, which the -S check above cannot detect.
printf '%s\nquit\n' "$*" | nc -U -w5 "$CTRL_SOCK" | while IFS= read -r line; do
    [ "$line" = "." ] && break
    printf '%s\n' "$line"
done
