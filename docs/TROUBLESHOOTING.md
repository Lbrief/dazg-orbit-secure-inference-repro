# Troubleshooting

## Unsupported CPU instruction

The portable build disables AVX-512 and requires AVX2, AES-NI, and SSE4.1. Check:

```bash
grep -m1 -oE 'avx2|aes|sse4_1' /proc/cpuinfo | sort -u
```

## Stale or partial CMake state

```bash
./reproduce.sh clean
./reproduce.sh build
```

N=10 and N=100 are configured in separate build trees, so repeated builds do not append duplicate `add_subdirectory` entries.

## Stage-S table error

Run:

```bash
./reproduce.sh verify
./reproduce.sh stage-s
```

The table must be 8,388,616 bytes with the SHA-256 documented in `scripts/verify_assets.py`.

## Port conflict or interrupted run

Each pair selects a local ephemeral port. Stop stale executors, remove only local run products, and resume:

```bash
pkill -TERM -f 'qahl_v720_to_h8_exact_executor|dazg_checkpoint013_n100_executor' || true
rm -rf runs/n10 runs/n100
```

Do not delete bundled assets or edit reference metrics to force a pass.

## N=100 stops before 100 samples

This is expected fail-closed behavior. Inspect the generated result archive under `runs/n100/results/`; sample 0 and the first 10 rows must pass before the remaining rows are attempted.


## WSL run stops with `pair runner rc=1` after both parties return zero

On repositories extracted under a Windows-mounted WSL path, Windows Defender or filesystem
indexing can briefly block the atomic promotion of `pair_report.json`.  A
complete temporary file may remain as `pair_report.json.tmp.*` even though the
server and client both completed successfully.

Release 1.1.1 retries the atomic rename, falls back to a verified copy, and lets
the parent runner recover a complete temporary report.  Re-run:

```bash
./reproduce.sh n100
```

Previously verified samples are reused only when binary, input, oracle, and
reference hashes still match.  Do not delete the run directory unless a clean
rerun is required.
