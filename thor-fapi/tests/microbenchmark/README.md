# nFAPI proxy microbenchmark

This harness records processing timestamps inside the proxy and drives its
current optimized paths with free-running localhost VNF/PNF stubs. It retains
the current downlink mirroring implementation and
`UL_SEGMENTATION_REASSEMBLY`; the benchmark does not enable either legacy path.

Build and run it from the repository root:

```bash
cmake -S . -B build-microbenchmark \
  -DENABLE_MICROBENCHMARK=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF
cmake --build build-microbenchmark -j2
bash tests/microbenchmark/run_microbenchmark.sh --duration 10 --ues 3
```

The runner writes raw events to `/tmp/proxy_message_log.csv`, writes the final
DL/UL summary to `/tmp/proxy_message_log_summary.json`, and prints that summary
to the terminal. The JSON contains separate `dl` and `ul` sections, each with
overall and per-message sample count, median, p90, p99, and maximum latency.
Use `--output PATH` to select another raw CSV, `--result PATH` to select the
summary JSON, and `--build-dir PATH` for another benchmark-enabled build.

Payload size is configurable per UE and defaults to 1 KiB:

```bash
# Three UEs, 1 KiB per UE
bash tests/microbenchmark/run_microbenchmark.sh --ues 3 --payload-size 1024

# Four UEs, 10 KiB per UE
bash tests/microbenchmark/run_microbenchmark.sh --ues 4 --payload-size 10240
```

The harness supports 1-16 UEs and rejects combinations whose aggregate payload
would exceed 60 KiB in one UDP/nFAPI message. RNTIs start at `0x1001`. Halfway
through each run, the runner pseudo-randomly selects one configured UE and
migrates it to the opposite L1. Supply `--seed INTEGER` to reproduce the same
selection; the seed, selected RNTI, and target L1 are printed with the run.

The CSV has no header and contains:

```text
timestamp_us,direction,message_type,sfn,slot,pnf_index,event_type
```

`NORTH` measures VNF-to-L1 processing from receive to the last routed send.
`SOUTH` measures L1-to-VNF processing from the final contributing arrival to
the forwarded or fully segmented send. The analyzer matches events in
timestamp order so repeated SFN/slot keys remain valid after SFN wraparound.

Two environment variables tune recording:

- `NFAPI_MICROBENCHMARK_LOG` selects the output CSV.
- `NFAPI_MICROBENCHMARK_CAPACITY` selects the in-memory event count (default:
  1,000,000). Events beyond the capacity are dropped with one warning.

The recorder buffers events in memory and flushes only during graceful
SIGINT/SIGTERM shutdown. Abrupt termination does not produce a complete CSV.
