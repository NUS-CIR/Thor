#!/bin/bash

# Copyright (c) National University of Singapore.
# Licensed under the MIT License.

# Usage: run_middlebox.sh <lcore_mask> [num_dus]
#
# num_dus defaults to 1: the middlebox starts with a single L1 and the second
# is attached at runtime over the control socket. Pass 2 to bring both up at
# start, as before.
#
# Requires RANBOOSTER_PATH; source setup_ranbooster_env.sh first.

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "Usage: $0 <lcore_mask> [num_dus]   (num_dus: 1 or 2, default 1)"
    exit 1
fi

if [ -z "${RANBOOSTER_PATH:-}" ]; then
    echo "RANBOOSTER_PATH is not set. Run 'source setup_ranbooster_env.sh' first." >&2
    exit 1
fi

LCORE=$1
NUM_DUS=${2:-1}

if [ "$NUM_DUS" -lt 1 ] || [ "$NUM_DUS" -gt 2 ]; then
    echo "num_dus must be 1 or 2, got $NUM_DUS"
    exit 1
fi

# Middlebox port: VF 0 of the Mellanox CX-7 PF ens4f0np0. Mellanox VFs are
# driven through the mlx5 PMD and stay bound to mlx5_core, so unlike the Intel
# VFs below they are not handled by setup_interfaces.sh.
MIDDLEBOX_VF_PCI=0000:43:00.3   # ens4f0v0
MIDDLEBOX_VF_MAC=00:11:22:33:44:44

# L1 ports, one per PF. See config/README.md for the full VF map.
PNF0_VF_PCI=0000:70:11.1        # ens6f2np2 vf 1
PNF0_VF_MAC=00:11:22:33:44:88
PNF1_VF_PCI=0000:70:19.1        # ens6f3np3 vf 1
PNF1_VF_MAC=00:11:22:33:44:AB

RU_MAC=E8:C7:4F:25:80:5B        # Liteon O-RU
RU_VLAN=5
NUM_PRBS=273                    # 100 MHz @ 30 kHz SCS

# L1s are added and removed at runtime over this socket:
#   config/thor_ctl.sh add 00:11:22:33:44:AB 5
#   config/thor_ctl.sh del 00:11:22:33:44:88
#   config/thor_ctl.sh list
CTRL_SOCK=/var/run/thor_fhaul_proxy.sock

# Only the L1s being started are listed; the rest are attached later.
DU_ARGS="$PNF0_VF_MAC $RU_VLAN"
if [ "$NUM_DUS" -eq 2 ]; then
    DU_ARGS="$DU_ARGS $PNF1_VF_MAC $RU_VLAN"
fi

echo "Starting middlebox with $NUM_DUS L1(s); control socket at $CTRL_SOCK"

# Non-DPDK ARGS: middlebox port, RU MAC, RU vlan, number of PRBs,
#                then <DU MAC> <DU VLAN> per L1, then NUM_DUS
# gdb -ex=r -ex=bt --batch --args \
$RANBOOSTER_PATH/build/thor_fhaul_proxy_dpdk/thor_fhaul_proxy_dpdk -l $LCORE \
                -a $MIDDLEBOX_VF_PCI \
                --file-prefix middlebox -- \
                $MIDDLEBOX_VF_PCI $RU_MAC $RU_VLAN $NUM_PRBS \
                $DU_ARGS $NUM_DUS \
                --ctrl-sock $CTRL_SOCK
