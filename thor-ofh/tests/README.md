# thor_fhaul_proxy tests

Two layers, both driving the real code:

| | what it exercises | needs root | runtime |
|---|---|---|---|
| `tests/unit/` | the datapath (`thor_handle_rx_packet`) against a private mempool — no NIC | no | < 1 s |
| `tests/integration/` | the real binary over a **veth pair**, driven by scapy | yes | ~100 s |

```sh
tests/build.sh              # build the proxy and the unit test binary
tests/run_tests.sh          # unit, then integration
tests/run_tests.sh unit     # unit only
tests/run_tests.sh integration -k dynamic   # extra args go to pytest
```

## Why a veth pair fits

The proxy is a **single-port** middlebox: the RU and every L1 sit on the same
wire and are told apart only by MAC address. One veth pair reproduces that
exactly.

```
     ┌──────────────────────────┐              ┌──────────────────────────┐
     │  thor_fhaul_proxy_dpdk   │              │      pytest + scapy      │
     │  (DPDK, net_af_packet)   │              │   (raw AF_PACKET socket) │
     └────────────┬─────────────┘              └────────────┬─────────────┘
                  │                                         │
             ┌────┴─────┐                              ┌────┴─────┐
             │ thor-mb  │◄────────  veth  ────────────►│ thor-net │
             └──────────┘                              └──────────┘
             MAC = the middlebox MAC              injects DU/RU traffic,
             (read from sysfs)                    captures what comes back
```

Both ends share a wire, so a packet socket on `thor-net` also sees the frames
the harness itself sent. The capture filters on **source MAC = middlebox MAC**:
every frame the proxy forwards is rewritten to that address, and the harness
never uses it as a source. The filter also pins the eCPRI ethertype so kernel
chatter cannot leak in.

## Setup the harness performs

Per `tests/integration/thor_testenv.py`:

- creates the veth pair, brings both ends up, sets them promiscuous
- disables segmentation offloads (`tso/gso/gro/lro`) so frames stay byte-exact
- disables IPv6 on both ends — otherwise the kernel emits MLD/RS frames *from
  the proxy's own MAC*, which the capture filter would pick up
- reloads scapy's interface cache, which is snapshotted at import time
- starts the proxy with `--no-huge` (no hugepage setup needed, and it cannot
  disturb another DPDK application) and `--no-pci` (never touches a real NIC)

## Known limitation: MTU

`net_af_packet` hardcodes `max_rx_pktlen` to 1518, so the proxy cannot run at
its production 9600-byte MTU over veth. The integration tests pass `--mtu 1500`
and use 32 PRBs. **Full-size and jumbo payloads are covered by the unit tests**,
which build mbufs directly and are bound by no PMD.

`net_tap` would allow jumbo frames, but it advertises no VLAN offloads at all,
which would cost the VLAN retag coverage — a worse trade.

## What is covered

**Unit** (`tests/unit/test_datapath.c`, 31 tests) — C-plane merge and cache
keying, U-plane IQ merge (AVX-512 path, scalar tail, mismatched lengths,
bitwise-OR semantics), uplink fan-out, PRACH, VLAN retagging, malformed and
out-of-range headers, DU slot add/remove/reuse/exhaustion, cache flush on
membership change, and mbuf accounting under sustained load.

**Integration** (39 tests) — the same behaviours end to end over the wire, plus
the control channel: `test_downlink.py`, `test_uplink.py`,
`test_vlan_and_filtering.py`, `test_dynamic_l1.py`.

Both suites were mutation-checked: deliberately breaking the AVX-512 tail, the
uplink fan-out, the per-L1 VLAN, and the cache-flush-on-membership-change each
produced failures in exactly the tests meant to catch them.

## Runtime L1 management

The proxy serves add/remove-L1 commands on a UNIX socket (`--ctrl-sock`).
Commands are applied by the datapath lcore between RX bursts, never by the
socket thread — see `src/dpdk/thor_fhaul_proxy/thor_ctrl.h`.

Start with a single L1 and attach the rest as they come up:

```console
$ config/liteon/run_middlebox.sh 1          # one L1; pass "2" for both at start
$ sudo config/thor_ctl.sh list
OK active_dus=1
du[0] mac=00:11:22:33:44:88 vlan=5
$ sudo config/thor_ctl.sh add 00:11:22:33:44:AB 5
OK slot=1 active_dus=2
$ sudo config/thor_ctl.sh del 00:11:22:33:44:88
OK slot=0 active_dus=1
```

Or speak the protocol directly:

```console
$ sudo nc -U /var/run/thor_fhaul_proxy.sock
list
OK active_dus=1
du[0] mac=00:11:22:33:44:88 vlan=5
.
add 00:11:22:33:44:AB 5
OK slot=1 active_dus=2
.
```

Every reply opens with `OK`/`ERR` and closes with a lone `.`, so a client can
frame it without knowing which command produced it. The socket is root-only,
which is deliberate for a privileged datapath.

From Python:

```python
from thor_testenv import CtrlClient
with CtrlClient("/tmp/thor.sock") as c:
    c.add("02:00:00:00:00:11", vlan=11)
    print(c.active_dus(), c.stats())
```

## Python environment

Everything runs from the repo's `venv/`:

```sh
python3 -m venv venv
venv/bin/pip install -r tests/requirements.txt
```

`tests/run_tests.sh` uses `venv/bin/python` directly and re-execs itself under
`sudo -E` for the integration layer.

## Troubleshooting

- **`thor-mb` left behind after a crash** — harmless; `VethPair.setup()` deletes
  any stale pair before creating a new one. To clear it by hand:
  `sudo ip link del thor-mb`.
- **`Cannot init port 0`** — usually a proxy from an earlier run still holding
  the EAL file prefix: `sudo pkill -f thor_fhaul_proxy_dpdk`.
- **Everything drops as `dropped_unmatched`** — the DU/RU MACs given to the
  proxy do not match what the harness is sending, or frames are not addressed
  to the middlebox MAC.
