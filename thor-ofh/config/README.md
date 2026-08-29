# Deployment configuration

```
config/
  thor_ctl.sh                add/remove L1s on a running middlebox
  liteon/                    Liteon O-RU  — middlebox on a Mellanox VF
  protoru/                   ProtoRU      — everything on one Intel PF
```

Each deployment directory holds two scripts, run in this order:

| | |
|---|---|
| `setup_interfaces.sh` | create the SR-IOV VFs, set their MACs/VLAN/MTU, bind to `vfio-pci` |
| `run_middlebox.sh` | launch `thor_fhaul_proxy_dpdk` against those VFs |

## The shape of the problem

The proxy is a **single-port** middlebox. It has exactly one NIC port, and the
O-RU and every L1 are reachable through it — they are told apart by **MAC
address alone**. There is no "L1 side" and "RU side" interface.

```
                        ┌──────────────────────────────┐
                        │   thor_fhaul_proxy_dpdk      │
                        │                              │
     L1 #0  ──┐         │  downlink: hold each L1's    │
              ├──── one │  packet until all have sent, │ ───►  O-RU
     L1 #1  ──┘   port  │  then OR the IQ and forward  │
              ▲         │                              │  ◄──
              └─────────│  uplink:  replicate to every │
                        │  active L1, retag per L1     │
                        └──────────────────────────────┘
```

Everything the proxy emits is re-sourced to the middlebox MAC, so on the wire
each direction is unambiguous:

```
  DOWNLINK                                   UPLINK
  L1 → middlebox → RU                        RU → middlebox → L1s

  src=L1_0  dst=MB   ┐                       src=RU   dst=MB
  src=L1_1  dst=MB   ┘ merge                        │
                     ↓                              ├──► src=MB dst=L1_0
  src=MB    dst=RU                                  └──► src=MB dst=L1_1
```

A packet is dropped unless its source matches the RU or an **active** L1 *and*
its destination is the middlebox MAC. That is why the MACs in
`setup_interfaces.sh` and `run_middlebox.sh` must agree exactly — a mismatch
shows up as a rising `dropped_unmatched` in `thor_ctl.sh stats`, not as an error.

## liteon — middlebox on the Mellanox NIC

The middlebox port is a **Mellanox ConnectX-7 VF** on the BlueField-3, while the
two L1s sit on **Intel E810 VFs** on two different PFs. L1↔middlebox traffic
therefore leaves the host and comes back through the top-of-rack switch,
alongside the RU.

```
  ┌───────────────────────── host ──────────────────────────┐
  │                                                          │
  │  ens4f0np0  (Mellanox CX-7, BlueField-3, 0000:43:00.0)   │
  │    └─ vf 0  ens4f0v0   43:00.3  00:11:22:33:44:44 ─── MIDDLEBOX
  │                                  (mlx5_core / mlx5 PMD)  │
  │                                                          │
  │  ens6f2np2  (Intel E810, 0000:70:00.2)                   │
  │    ├─ vf 0  70:11.0    00:11:22:33:44:77                 │
  │    ├─ vf 1  70:11.1    00:11:22:33:44:88 ───────────── L1 #0
  │    └─ vf 2  70:11.2    00:11:22:33:44:99                 │
  │                                                          │
  │  ens6f3np3  (Intel E810, 0000:70:00.3)                   │
  │    ├─ vf 0  70:19.0    00:11:22:33:44:AA                 │
  │    ├─ vf 1  70:19.1    00:11:22:33:44:AB ───────────── L1 #1
  │    └─ vf 2  70:19.2    00:11:22:33:44:AC                 │
  │                                                          │
  │  ens6f1np1  (Intel E810, 0000:70:00.1)                   │
  │    └─ vf 0..2  70:09.0-.2  ...:55 / :56 / :57  (spare)   │
  └──────────────────────────┬───────────────────────────────┘
                             │  all VLAN 5, MTU 9600
                        ┌────┴─────┐
                        │  switch  │
                        └────┬─────┘
                             │
                     Liteon O-RU  E8:C7:4F:25:80:5B
                     273 PRBs (100 MHz @ 30 kHz SCS)
```

Note the asymmetry: `setup_interfaces.sh` provisions only the **Intel** VFs.
The Mellanox VF is left alone on purpose — mlx5 VFs are driven through the mlx5
PMD and must stay bound to `mlx5_core`, so they need no `vfio-pci` bind. Create
it once with `echo 1 > /sys/class/net/ens4f0np0/device/sriov_numvfs` and set its
MAC with `ip link set ens4f0np0 vf 0 mac 00:11:22:33:44:44 trust on`.

## protoru — everything on one Intel PF

Middlebox and both L1s are VFs of the *same* PF, so L1↔middlebox traffic never
leaves the NIC: it is switched internally by the E810's embedded switch. Only
RU traffic crosses the wire. Fewer moving parts, and no switch config needed.

```
  ┌───────────────────────── host ──────────────────────────┐
  │  ens6f2np2  (Intel E810, 0000:70:00.2)                   │
  │    ├─ vf 0  70:11.0  00:11:22:33:44:77 ────────────── L1 #0
  │    ├─ vf 1  70:11.1  00:11:22:33:44:88 ────────────── L1 #1
  │    └─ vf 2  70:11.2  00:11:22:33:44:99 ───────────── MIDDLEBOX
  │                    │                                     │
  │              embedded switch  (VF↔VF stays on-chip)      │
  └────────────────────┼─────────────────────────────────────┘
                       │  VLAN 5, MTU 9600
                  ProtoRU  50:7c:6f:45:f3:04
                  106 PRBs (40 MHz @ 30 kHz SCS)
```

## Bringing it up

```bash
source setup_ranbooster_env.sh          # exports RANBOOSTER_PATH
config/protoru/setup_interfaces.sh      # once per boot
config/protoru/run_middlebox.sh 1       # lcore 1, starts with one L1
```

`run_middlebox.sh` locates the binary through `RANBOOSTER_PATH` and refuses to
start if it is unset, so the environment script has to be sourced first.

`run_middlebox.sh <lcore> [num_dus]` starts with **one** L1 by default; pass `2`
to bring both up immediately. At least one L1 is always required at startup —
the rest attach at runtime:

```bash
sudo config/thor_ctl.sh list
sudo config/thor_ctl.sh add 00:11:22:33:44:88 5
sudo config/thor_ctl.sh del 00:11:22:33:44:77
sudo config/thor_ctl.sh stats
```

At most `MAX_NUM_DUS` (2) L1s may be active at once. `active_dus` in a reply is
the number of contributions the downlink merge now waits for before forwarding
to the RU.

## Notes

- **VLAN 5 throughout.** The VF VLAN is set by `ip link set ... vlan 5`, so the
  NIC strips it on ingress and re-inserts it on egress; the proxy re-tags per
  destination using the mbuf VLAN offload. Change it in both the setup script
  and `run_middlebox.sh` together.
- **MTU 9600** everywhere — 273 PRBs of compressed IQ is 7644 bytes plus
  headers, well past a 1500-byte MTU.
- **`trust on`** is set on the `ens6f1`/`ens6f3` VFs but not `ens6f2`. Left as
  found; if an L1 on `ens6f2` cannot set promiscuous mode, that is the reason.
- The scapy model of the headers the proxy parses lives in
  `tests/integration/oran_packets.py`. It is a convenient way to hand-craft
  fronthaul frames when debugging a live setup, not just for the tests.
