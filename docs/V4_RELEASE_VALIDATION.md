# DAZG-Orbit v4 release validation

Version 1.1.1 hardens N=100 evidence publication on WSL repositories stored on
Windows-mounted filesystems.

## Observed failure in v3

A user-side clean run completed both server and client for balanced sample 27,
but the runner exited with `pair runner rc=1`. The returned evidence showed:

- `server_rc = 0`;
- `client_rc = 0`;
- a complete `pair_report.json.tmp.*` file;
- no final `pair_report.json`;
- diagnostic rerun of sample 27: logits exact, mismatch 0, max delta 0, and
  reference Top-5 exact.

The model computation therefore succeeded. The failure occurred while promoting
a complete temporary JSON report to its final name on a Windows-mounted WSL
path.

## v4 correction

- retry atomic `os.replace()` publication;
- verified-copy fallback when the mount continues to block replacement;
- parent-runner recovery of complete `pair_report.json.tmp.*` evidence;
- fail closed when neither a final nor a valid temporary report exists;
- automated simulation of a DrvFS/indexer lock in `scripts/test_runtime_io.py`.

## Acceptance checks

```bash
./reproduce.sh verify
DAZG_JOBS=8 ./reproduce.sh build
./reproduce.sh stage-s
./reproduce.sh n10
./reproduce.sh n100
```

The arithmetic and accuracy requirements remain unchanged:

- N=10 strict exact 10/10, Top-1 10/10, Top-5 10/10;
- N=100 logits exact 100/100, mismatch 0, max delta 0;
- N=100 Top-1 72/100 and Top-5 91/100;
- reveal correctness backend, `security_claim=0`.
