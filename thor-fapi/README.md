# Thor nFAPI proxy

Thor is an nFAPI proxy between one VNF and one or more PNF/L1 instances. It
forwards P5 setup traffic and routes P7 messages according to an RNTI-to-L1
lookup table. The current build supports up to two simultaneously connected
L1s (`MAX_NUM_PNF=2`).

The proxy can operate with one L1. A second L1 may join later, and either L1
may disconnect without restarting the proxy.

## Build

```bash
cmake -S . -B build
cmake --build build -j2
```

The production and unit-test targets use the current optimized paths:

- The legacy DL and UL_TTI implementations are disabled.
- `TX_DATA.request` uses the current mirroring implementation.
- `UL_SEGMENTATION_REASSEMBLY` is enabled for uplink CRC and RX_DATA traffic.

The build produces:

- `build/nfapi-proxy` — the proxy
- `build/control_client` — the local runtime-control client
- `build/test_vnf_stub` and `build/test_pnf_stub` — P7 integration-test peers
- `build/nfapi_proxy_unit_tests` — focused routing and message-path tests

## Docker

Build the production image:

```bash
./build_image.sh
# Build, run all container tests, then produce the runtime image:
./build_image.sh --test
```

Use `--tag registry.example/thor-nfapi-proxy:TAG` to select another image tag
and `--no-cache` for a clean rebuild. The equivalent direct Docker command is
`docker build --platform linux/amd64 -t thor-nfapi-proxy:release .`.

The multi-stage image contains only `nfapi-proxy`, `control_client`, and their
runtime libraries in its final stage. It uses the same optimized DL mirroring
and `UL_SEGMENTATION_REASSEMBLY` build options as a native build. This package
is intentionally restricted to `linux/amd64`; builds targeting another
architecture fail during image construction.

Run the supplied Compose example on a Linux host:

```bash
docker compose up -d nfapi-proxy
docker compose --profile tools run --rm controller list_l1
```

The production service uses host networking because the proxy currently
connects to its VNF at `127.0.0.1:50001`. It listens for PNF P5 SCTP traffic on
port `50002` and P7 UDP traffic on port `50012`. The control socket remains at
`/var/run/thor_nfapi_proxy.sock` and is shared with the optional controller
container through the `thor-runtime` volume.

Run the complete unit and two-L1 P7 integration suite inside Docker:

```bash
docker compose --profile test build tests
docker compose --profile test run --rm tests
```

The Docker host must provide SCTP kernel support for the unit tests and normal
P5 operation. `lsmod | grep sctp` can be used to check it; on systems that use
loadable modules, run `sudo modprobe sctp` when necessary. Docker Desktop does
not provide the Linux host-network semantics required by this Compose example.

## Run

In normal mode the proxy connects to the VNF P5 endpoint on `127.0.0.1:50001`,
listens for PNF P5 connections on SCTP port `50002`, and uses UDP port `50012`
for P7 traffic:

```bash
build/nfapi-proxy
```

Available options:

```text
--p5-vnf-port <port>   VNF P5 SCTP port (default: 50001)
--p5-port <port>       PNF-facing P5 SCTP port (default: 50002)
--p7-port <port>       Proxy P7 UDP port (default: 50012)
--ctrl-sock <path>     Control socket path (default: /var/run/thor_nfapi_proxy.sock)
--slingshot-mode       Apply each migration command to all RNTIs
--test-mode            Bypass P5 and configure local P7 test peers directly
--test-vnf-port <port> VNF test-peer UDP port (default: 60001)
--test-pnf-port <list> Comma-separated PNF test-peer UDP ports
--help                 Show command-line help
```

## Dynamic L1 membership

The proxy does not wait for two L1s. Forwarding starts after the first PNF
completes setup.

- The oldest connected L1 is the primary. Therefore, the first L1 is always the
  initial primary.
- Later connections are secondary L1s. A new L1 is brought to the VNF's
  current P5 state using cached setup messages, but remains `not_ready` and is
  excluded from P7 routing until the controller admits it.
- When the primary disconnects, the oldest surviving secondary becomes the
  primary.
- When any L1 disconnects, every DL and UL lookup-table entry that referenced
  it is changed immediately to the replacement primary. If no L1 remains, the
  entries become unroutable until a new primary connects.
- A PNF `STOP.indication` is consumed by the proxy as a graceful removal
  signal. Membership and routing cleanup run immediately, without waiting for
  the P5/SCTP connection-loss timeout.
- Disconnect cleanup also clears transient PDSCH/TX_DATA associations and
  per-slot uplink aggregation state, preventing traffic from being sent to or
  awaited from a stale L1.
- L1 IDs are capacity-slot indexes. With the current build they are `0` and
  `1`; an ID can be reused after its previous connection is removed.
- The first L1 is ready automatically so single-L1 startup does not require a
  controller. A secondary connection, including a recycled secondary ID,
  requires `set_ready` after its synchronization/drain procedure completes.
- P7 requests received while no L1 is ready are dropped.

The normal add sequence is to connect the L1, wait for external synchronization,
run `set_ready`, and then migrate UEs to it. The normal graceful-drain sequence
is to migrate affected UEs away, allow the staged mapping to take effect on a
primary `SLOT.indication`, optionally run `set_not_ready`, stop the drained L1,
and query membership again. Sudden disconnects use the immediate fallback
behavior described above.

## Runtime control

The proxy creates a Unix stream (`SOCK_STREAM`) control socket at
`/var/run/thor_nfapi_proxy.sock`. The socket uses mode `0660`. When the proxy is
started with `sudo`, ownership is assigned to the invoking `SUDO_UID` and
`SUDO_GID`, allowing that user to run `control_client` without sudo. The proxy
closes and removes the socket during normal SIGINT/SIGTERM shutdown; startup
also removes a stale path left by an abnormal exit.

The path follows the RANBooster convention while remaining distinct so both
proxies can run on the same host. Override it with `--ctrl-sock <path>` or
`THOR_CTRL_SOCK`; the command-line option takes precedence. Tests use an
isolated path under `/tmp`.

Commands are newline-delimited text. Responses begin with `OK` or `ERR` and end
with a line containing only `.`. This permits direct inspection with common
Unix-socket tools:

```bash
printf 'list_l1\n' | sudo nc -U -q1 /var/run/thor_nfapi_proxy.sock
```

### Query L1 membership

```bash
build/control_client list_l1
```

Examples:

```text
primary=0;secondary=1;not_ready=1
primary=0;secondary=1;not_ready=
primary=0;secondary=;not_ready=
primary=-1;secondary=;not_ready=
```

`secondary` is a comma-separated list when a build allows more than two L1s.
An empty value means there is no connected secondary. `not_ready` contains
connected L1 IDs that are excluded from data-plane routing. `primary=-1` means
no L1 is connected.

### Admit or drain an L1

```bash
build/control_client set_ready <L1-ID>
build/control_client set_not_ready <L1-ID>
```

`set_ready` admits an already connected L1 to P7 routing. `set_not_ready`
removes it from P7 routing immediately and remaps any residual routes to the
oldest ready L1, or makes them unroutable if none remains. P5 setup and
SLOT/TIMING synchronization traffic remain available while an L1 is
`not_ready`.

### Migrate an RNTI

```bash
build/control_client migrate <decimal-RNTI> <L1-ID>
```

For example, migrate RNTI `0x1002` (decimal `4098`) to L1 ID `1`:

```bash
build/control_client migrate 4098 1
```

The target must be present and ready in `list_l1`; disconnected, not-ready, and
out-of-range IDs are rejected. Migrations are staged in the `next` DL and UL
tables and are promoted when the proxy forwards the next primary
`SLOT.indication`. Slots without a pending migration bypass the full RNTI-table
scan. In
`--slingshot-mode`, the supplied RNTI is ignored and all RNTIs are staged for
the target L1.

### Logging

```bash
build/control_client debug on
build/control_client debug off
```

These commands switch the running proxy between DEBUG and INFO logging.

## P7 routing behavior

| Direction | Message | Current behavior |
| --- | --- | --- |
| VNF → L1 | `DL_TTI.request` | Routes UE-specific PDSCH PDUs by RNTI; sends SSB, PDCCH, and CSI-RS PDUs to the primary. |
| VNF → L1 | `TX_DATA.request` | Mirrors the original packed message to every ready L1. |
| VNF → L1 | `UL_TTI.request` | Mirrors PUSCH/PUCCH scheduling to ready L1s; the owning L1 keeps the real RNTI and non-owning L1s receive reserved RNTI `0xFFF0`. PRACH goes to the ready primary; SRS follows its RNTI mapping. |
| VNF → L1 | `UL_DCI.request` | Sends the original message to the primary only. |
| L1 → VNF | `SLOT.indication` | Forwards the primary L1's indication immediately; secondary indications are discarded. |
| L1 → VNF | `RACH.indication` | Forwards directly. |
| L1 → VNF | `UCI.indication`, `SRS.indication` | Forwards a single expected indication or aggregates the expected per-L1 indications. The current optimized UL_TTI path does not yet derive `srs_ind_expected`, so production SRS aggregation remains incomplete. |
| L1 → VNF | `CRC.indication`, `RX_DATA.indication` | Waits for the count derived from `UL_TTI.request`, then forwards the per-L1 payloads as an nFAPI segment sequence when `UL_SEGMENTATION_REASSEMBLY` is enabled. |

## Tests

Run the unit tests from the repository root:

```bash
ctest --test-dir build --output-on-failure
```

The unit suite covers membership add/remove/failover, stale-route cleanup, P5
failure eviction, all currently supported DL and UL message paths, TX_DATA
mirroring, UL_TTI reserved-RNTI masking with `0xFFF0`, and segmented CRC and
RX_DATA output.

Run the two-L1 P7 integration test with:

```bash
bash tests/run_tests.sh
```

See [tests/README.md](tests/README.md) for test-mode, one-L1, two-L1, and manual
debugging instructions. The opt-in proxy latency harness is documented in
[tests/microbenchmark/README.md](tests/microbenchmark/README.md).
