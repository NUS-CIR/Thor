# Thor: Fine-grained Per-UE Baseband Routing in vRAN

This repository contains the reference implementation of **Thor**, presented in the ACM MobiCom'26 paper, *"Thor: Fine-grained Per-UE Baseband Routing in vRAN"*.

Today's vRANs bind every UE in a cell to a single L1 (PHY) implementation. 
Thor removes that constraint: it lets multiple L1 instances run concurrently within the same cell and routes each UE to its designated L1, enabling incremental L1 updates, in-cell A/B testing, and per-UE service differentiation. 
Thor works over the standard FAPI and O-RAN fronthaul (OFH) interfaces and requires no changes to the L2+ stack or the RU.

## Overview

Thor interposes two lightweight, standards-compliant middleboxes on the existing vRAN datapath:

- [`thor-fapi/`](./thor-fapi) — a user-space nFAPI middlebox at the MAC–PHY interface. It virtualizes the FAPI endpoint between one L2+/MAC stack and multiple L1s, routes control-plane scheduling messages per UE using an RNTI-to-L1 mapping, mirrors bulk user-data messages, and merges uplink indications back into a single logical FAPI stream.
- [`thor-ofh/`](./thor-ofh) — a DPDK-based fronthaul middlebox at the OFH interface. It merges the downlink IQ output of multiple L1s into a single coherent resource grid and mirrors uplink IQ samples to all L1s, enabling concurrent multi-L1 operation over an unmodified O-RU.

Both directories include their own build scripts, unit/integration tests, and the control clients used to manage L1 lifecycles at runtime (add/ remove/ activate/ deactivate).

## Getting started

Requirements: an x86-64 host with AVX-512 support (Ice Lake or newer, required by Thor-OFH), ≥ 8 GB RAM, ≥ 10 GB free disk, and Docker. 
The bundled tests need no NIC, DPDK hugepages, or radio hardware.

```bash
git clone https://github.com/NUS-CIR/Thor.git
cd Thor
```

> Note: Instructions for the end-to-end setup (Thor-CTRL and the full over-the-air testbed deployment) will be added at a later stage.

### Running the tests
Build the image and run the unit/integration tests for each middlebox:

```bash
cd thor-fapi/ && ./build_image.sh --test
```

```bash
cd thor-ofh/ && ./build_image.sh all --run
```

See [`thor-fapi/README.md`](./thor-fapi/README.md) and [`thor-ofh/README.md`](./thor-ofh/README.md) for build options, runtime control commands, and deployment details.

## Citation

If you find this work relevant to your research, please cite the following:
```
@INPROCEEDINGS{2026-MOBICOM-Thor,
  author={Xin Zhe Khooi and Satis Kumar Permal and Dong Hyeok Kim and Robert Schmidt and Min Suk Kang and Mun Choon Chan},
  booktitle={ACM MobiCom},
  title={{Thor: Fine-grained Per-UE Baseband Routing in vRAN}},
  year={2026},
  doi={10.1145/3795866.3844159}
}
```

## License

Thor-specific components are released under the [MIT License](./LICENSE) unless otherwise stated. 

Third-party dependencies remain under their own licenses: `thor-ofh` builds on RANBooster (MIT, see [`thor-ofh/LICENSE`](./thor-ofh/LICENSE)) and O-RAN SC `o-du/phy` (Apache 2.0, see [`thor-ofh/NOTICE`](./thor-ofh/NOTICE)); `thor-fapi` integrates OpenAirInterface5G components under the [OAI Public License V1.1](https://openairinterface.org/legal/oai-public-license/).

## Contact

For any questions, or if you have any comments or feedback, there are two ways to reach out.

- File a GitHub issue under this repo.
- Drop an email to `khooixz [at] comp [dot] nus [dot] edu [dot] sg`.
