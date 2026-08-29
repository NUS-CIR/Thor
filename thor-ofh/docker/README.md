# Containerised thor_fhaul_proxy

```bash
./build_image.sh              # runtime image  (~118 MB)
./build_image.sh test         # test image     (~207 MB)
./build_image.sh test --run   # build it and run the whole suite
./build_image.sh all
```

The image is **self-contained**: it downloads DPDK and clones `o-du/phy` at the
pinned commit itself, so it needs neither `RTE_SDK` nor an initialised
submodule. Only Docker. **amd64 only** — the IQ merge is AVX-512 intrinsics.

## Stages

| Stage | Contains |
|---|---|
| `builder` | DPDK 24.11.2 built from source, patched fhi_lib, the proxy and unit-test binaries |
| `runtime` | the proxy, DPDK runtime libraries and PMDs, `thor_ctl.sh` |
| `test` | `runtime` + python/scapy + the test suite |

## Running the tests

The test image runs the whole suite — unit tests, then the veth integration
tests — and needs no hardware:

```bash
docker run --rm \
    --cap-add NET_ADMIN --cap-add NET_RAW --cap-add IPC_LOCK \
    --shm-size 1g \
    ranbooster/thor-proxy:test

# or
docker compose -f docker/docker-compose.yml run --rm tests
# pytest arguments pass straight through
docker compose -f docker/docker-compose.yml run --rm tests integration -k vlan
```

The veth pair is created inside the container's own network namespace, so this
touches nothing on the host and concurrent runs cannot collide.

Why each capability is needed:

| | |
|---|---|
| `NET_ADMIN` | create the veth pair, set MTU and promiscuous mode |
| `NET_RAW` | AF_PACKET sockets — for both the DPDK PMD and scapy |
| `IPC_LOCK` | EAL memory locking |
| `--shm-size 1g` | EAL heap; the 64 MB default is not enough even with `--no-huge` |

No hugepages and no privileged mode: the tests run DPDK with `--no-huge` and
`--no-pci`.

## Running the proxy against real hardware

Edit the `proxy` service in `docker-compose.yml` — the device, MACs, VLAN and
PRB count are all deployment-specific. See [`../config/README.md`](../config/README.md)
for the topology and `../config/*/run_middlebox.sh` for working argument sets.

```bash
config/protoru/setup_interfaces.sh      # on the host: create and bind the VFs
docker compose -f docker/docker-compose.yml up proxy
```

Requirements beyond the test setup:

- **hugepages** reserved on the host and `/dev/hugepages` mounted;
- **`/dev/vfio/<group>`** for the VF, where the group comes from
  `readlink /sys/bus/pci/devices/<pci-addr>/iommu_group`;
- `IPC_LOCK`, and `SYS_NICE` if the datapath is to run at `SCHED_RR` — drop the
  latter and pass `--no-rt` if you would rather it did not;
- `cpuset` pinning the datapath away from the L1 cores. It polls flat out.

Adding and removing L1s at runtime works the same as on the host; the control
socket is on a named volume so other containers can reach it:

```bash
docker compose -f docker/docker-compose.yml run --rm ctl list
docker compose -f docker/docker-compose.yml run --rm ctl add 00:11:22:33:44:88 5
```

## Which drivers are built

`bus/*`, `mempool/*`, `common/*` and the NIC drivers actually in use:

| | |
|---|---|
| `ice`, `iavf` | Intel E810 and its VFs — what both deployments run on |
| `i40e` | Intel X710 / XL710 |
| `ixgbe`, `e1000` | Intel 82599 / X5xx, igb / em |
| `mlx5`, `mlx4` | Mellanox CX-4..7, including the BlueField-3 VF in the liteon setup |
| `af_packet`, `tap`, `null`, `ring` | software PMDs; the veth tests run on `af_packet` |

The full DPDK set is ~150 plugins, almost none of them reachable here. Two
build-time assertions keep the trimmed set honest:

1. every plugin in the PMD directory resolves its shared libraries — EAL
   `dlopen()`s all of them at startup and aborts on the first failure, so a
   missing library is fatal rather than a merely absent driver;
2. every driver named in `REQUIRED_PMDS` exists, so a typo in the driver list or
   a missing build dependency fails the build instead of turning into "no such
   device" against real hardware.

To add a NIC, extend both `DPDK_DRIVERS` and `REQUIRED_PMDS` in the Dockerfile.
`/usr/local/share/dpdk-meson-setup.log` inside the image records what meson
actually enabled.

## Build arguments

| Argument | Default | |
|---|---|---|
| `DPDK_VERSION` | `24.11.2` | DPDK release to build |
| `PHY_COMMIT` | `6ef1d2b…` | `o-du/phy` commit, matching the `3p/phy` submodule |
| `TARGET_MARCH` | `icelake-server` | `-march=` for the proxy |
| `DPDK_DRIVERS` | see above | meson `enable_drivers` list |
| `REQUIRED_PMDS` | see above | drivers asserted to exist |
| `UBUNTU_VERSION` | `24.04` | base image |

`TARGET_MARCH` is deliberately **not** `native`: a native build tunes for
whatever machine built the image, and the AVX-512 in the IQ merge would then
`SIGILL` on any narrower host. `icelake-server` matches what fhi_lib is built
with (`WIRELESS_SDK_TARGET_ISA=snc`). Lower it only for a CPU that still has
AVX-512 — the merge has no scalar fallback.

## Troubleshooting

- **`Cannot init plugins` / `libX.so: cannot open shared object file`** — a PMD
  dependency is missing from the runtime stage. The first build-time assertion
  should have caught it; if you added a driver, add its runtime library too.
- **Every capture is empty, no error** — `libpcap0.8t64` is missing from the
  test stage, so scapy cannot compile its BPF filter. The harness now raises
  instead of reporting zero packets, so this should be self-evident.
- **`EAL: Cannot get hugepage information`** — mount `/dev/hugepages` and
  reserve pages on the host, or pass `--no-huge` for a software PMD.
- **`SIGILL` immediately at startup** — the image was built for a wider CPU than
  it is running on. Rebuild with a lower `TARGET_MARCH`.
