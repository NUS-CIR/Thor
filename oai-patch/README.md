# OAI L1 patch: PRB skipping

[`prb-skipping.patch`](./prb-skipping.patch) patches the OpenAirInterface5G (OAI) gNB L1 so each L1 decodes only the uplink PRBs of UEs routed to it.

[Thor-FAPI](../thor-fapi) mirrors `UL_TTI.request` to all L1s. On L1s that do not own a UE, it replaces that UE's PUSCH/PUCCH RNTI with the reserved value `0xFFF0`. 
With this patch, OAI skips any PUSCH/PUCCH PDU carrying `0xFFF0` in `phy_procedures_gNB_uespec_RX()`, so those PDUs are not decoded and produce no CRC, RX_DATA, or UCI indications. 
PDUs with any other RNTI follow the normal OAI path.

Apply the patch to every OAI L1 attached to Thor.

## Usage

Tested with OAI `2026.w08` and `2026.w09`, the versions used in the paper.

```bash
git clone https://gitlab.eurecom.fr/oai/openairinterface5g.git
cd openairinterface5g
git checkout 2026.w09   # or 2026.w08
git apply /path/to/Thor/oai-patch/prb-skipping.patch
```

Then build `nr-softmodem` as usual.

## License

[OAI Public License V1.1](https://openairinterface.org/legal/oai-public-license/).
