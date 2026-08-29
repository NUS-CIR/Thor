# Thor nFAPI proxy tests

The repository has two complementary test layers:

- `nfapi_proxy_unit_tests` exercises routing and message handlers directly.
- `run_tests.sh` starts the proxy and UDP stubs for an end-to-end P7 test with
  two configured L1s.

Both are built with the production optimized behavior: current DL/TX mirroring
(no `DL_LEGACY` or `UL_TTI_LEGACY`) and
`UL_SEGMENTATION_REASSEMBLY` for uplink CRC/RX_DATA traffic.

## Build

From the repository root:

```bash
cmake -S . -B build
cmake --build build -j2
```

This creates:

- `build/nfapi-proxy`
- `build/control_client`
- `build/nfapi_proxy_unit_tests`
- `build/test_vnf_stub`
- `build/test_pnf_stub`

## Unit tests

Run the CTest suite:

```bash
ctest --test-dir build --output-on-failure
```

Current unit coverage includes:

- Starting with zero, one, and two connected L1s.
- Primary selection, secondary removal, primary promotion, last-L1 removal,
  reconnect behavior, and controller-owned readiness transitions.
- Removing primary ID 0, promoting ID 1, then reusing ID 0 as a not-ready
  secondary without displacing the older primary ID 1.
- Rejecting migrations to connected-but-not-ready L1s and gating P7 traffic
  until the controller marks the L1 ready.
- Skipping mapping-table promotion work on idle slots and applying a staged
  controller migration exactly once at the next primary slot boundary.
- Applying control-socket mode `0660` and removing its filesystem path during
  shutdown cleanup.
- Remapping stale DL/UL routes, clearing PDSCH/TX_DATA associations, and
  resetting pending uplink aggregation after disconnect.
- Evicting an L1 after a P5 read or send failure, and immediately removing it
  after a graceful P5 `STOP.indication`.
- Splitting `DL_TTI.request` PDSCH PDUs by RNTI.
- Mirroring `TX_DATA.request` to all ready L1s.
- Mirroring `UL_TTI.request`, tracking expected uplink indications, preserving
  the real RNTI on the owning L1, and masking it with `0xFFF0` on the other L1.
- Sending `UL_DCI.request` to the primary only.
- Primary-only `SLOT.indication` forwarding and the RACH, UCI, and SRS handler
  paths. The SRS unit case injects its expected-count state directly because
  optimized `UL_TTI.request` does not yet derive `srs_ind_expected`.
- Two-source `CRC.indication` and `RX_DATA.indication` segmented output.

The membership assertions also verify the controller response format:

```text
primary=0;secondary=1;not_ready=
```

## Automated P7 integration test

Run the orchestration script from the repository root:

```bash
bash tests/run_tests.sh
```

Optional arguments:

```text
--duration <seconds>  Test duration (default: 10)
--build-dir <path>    Build directory (default: build)
```

The script starts:

1. `nfapi-proxy` in P5-bypassing test mode.
2. One VNF UDP stub.
3. Two PNF/L1 UDP stubs.
4. A timed exchange of the supported downlink requests and uplink indications.

It then scans the component logs for failures and terminates all child
processes. Logs are written to:

```text
/tmp/nfapi-proxy-test.log
/tmp/vnf-stub-test.log
/tmp/pnf-stub-0-test.log
/tmp/pnf-stub-1-test.log
```

The VNF stub understands the segmented CRC and RX_DATA output produced by the
current `UL_SEGMENTATION_REASSEMBLY` path. It counts the final segment as the
completion of one logical indication.

## Microbenchmark

An opt-in benchmark build records in-proxy ARRIVE/DEPART timestamps and uses
continuous 500-us PNF slot clocks. See
[microbenchmark/README.md](microbenchmark/README.md) for build, runner, CSV,
and analysis details. Supplying an iteration count as the final VNF/PNF stub
argument selects continuous mode; omitting it preserves the one-exchange test
behavior described below.

## Docker test suite

From the repository root, build and run the containerized test target:

```bash
docker compose --profile test build tests
docker compose --profile test run --rm tests
```

This runs `ctest` followed by the two-L1 P7 integration test in one isolated
container. The Docker host must have SCTP kernel support for the P5 lifecycle
unit tests.

## Test mode

Test mode bypasses SCTP/P5 setup and creates connected localhost UDP sockets
for the configured test peers:

```bash
THOR_CTRL_SOCK=/tmp/nfapi_proxy_control.sock build/nfapi-proxy --test-mode \
  --test-vnf-port <VNF-port> \
  --test-pnf-port <PNF-port>[,<PNF-port>] \
  --p7-port <proxy-port>
```

Defaults and constraints:

- VNF peer port: `60001`
- Proxy P7 port: `50012`
- Up to `MAX_NUM_PNF` PNF ports are accepted; the current build allows two.
- The first configured PNF is primary.
- Test RNTI `0x1001` is assigned to L1 0.
- Test RNTI `0x1002` is assigned to L1 1 when present, otherwise to the primary.
- Test RNTI `0x1003` is assigned to L1 0.
- The examples use a control socket under `/tmp` so the proxy can run without
  root. Use the same `THOR_CTRL_SOCK` value with `control_client`.

## Manual one-L1 test

Use three terminals from the repository root.

Terminal 1 — proxy:

```bash
THOR_CTRL_SOCK=/tmp/nfapi_proxy_control.sock build/nfapi-proxy --test-mode \
  --test-vnf-port 60001 \
  --test-pnf-port 60010 \
  --p7-port 50012
```

Terminal 2 — VNF stub:

```bash
build/test_vnf_stub 60001 50012
```

Terminal 3 — sole PNF/L1 stub:

```bash
build/test_pnf_stub 60010 50012 0
```

While the proxy is running, membership should report:

```bash
THOR_CTRL_SOCK=/tmp/nfapi_proxy_control.sock build/control_client list_l1
# primary=0;secondary=;not_ready=
```

## Manual two-L1 test

Use four terminals from the repository root.

Terminal 1 — proxy:

```bash
THOR_CTRL_SOCK=/tmp/nfapi_proxy_control.sock build/nfapi-proxy --test-mode \
  --test-vnf-port 60001 \
  --test-pnf-port 60010,60011 \
  --p7-port 50012
```

Terminal 2 — VNF stub:

```bash
build/test_vnf_stub 60001 50012
```

Terminal 3 — primary PNF/L1 stub:

```bash
build/test_pnf_stub 60010 50012 0
```

Terminal 4 — secondary PNF/L1 stub:

```bash
build/test_pnf_stub 60011 50012 1
```

Membership should report:

```bash
THOR_CTRL_SOCK=/tmp/nfapi_proxy_control.sock build/control_client list_l1
# primary=0;secondary=1;not_ready=
```

All PNF stubs send to the proxy's configured P7 port (`50012` in these
examples). Their first argument is their own local UDP port.

## Stub behavior

`test_vnf_stub` sends:

- `DL_TTI.request`
- `TX_DATA.request`
- `UL_TTI.request`
- `UL_DCI.request`

It receives:

- `SLOT.indication`
- `RACH.indication`
- `RX_DATA.indication`
- `CRC.indication`
- `UCI.indication`
- `SRS.indication`

`test_pnf_stub` performs the inverse role: it validates downlink requests and
sends the uplink indications. The stubs intentionally use small, synthetic
messages; interoperability with a real VNF and L1 remains a separate system
test.
