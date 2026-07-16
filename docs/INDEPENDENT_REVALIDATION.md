# Independent clean-room revalidation

The release archive was extracted into a new directory with no `build/` or `runs/` state and rerun on 2026-07-16.

## Commands executed

```bash
./reproduce.sh verify-manifest
./reproduce.sh verify
DAZG_JOBS=8 ./reproduce.sh build
./reproduce.sh stage-s
./reproduce.sh n10
./reproduce.sh n100
```

## Reproduced gates

| Gate | Result |
|---|---:|
| Manifest and asset validation | PASS |
| Offline HEXL/SEAL build | PASS |
| Isolated N=10 executor build | PASS |
| Isolated N=100 executor build | PASS |
| Stage-S frozen-table contract | PASS |
| N=10 strict exact | 10/10 |
| N=10 Top-1 / Top-5 | 10/10 / 10/10 |
| N=100 sample status | 100/100 PASS |
| N=100 logits exact | 100/100 |
| N=100 mismatched logits / max delta | 0 / 0 |
| N=100 reference Top-5 rows exact | 100/100 |
| N=100 Top-1 / Top-5 | 72/100 / 91/100 |
| N=100 server/client rc=0 | 100/100 / 100/100 |

The independently rebuilt executable hashes matched the bundled clean-validation identities:

- N=10: `196012615c95f48da81ee6c9a02897262f4c9c9b9152a4f5cee77436b3c49062`
- N=100: `b7ed3da1609a76123fcf93bc237d5d942e1e421c14bf5636805bd930e71cb5ad`

Measured wall times in this run were about 6.881 s/sample for N=10 and 1.938 s/sample for N=100. Timing is environment-dependent; the exactness and accuracy gates are deterministic.

The N=100 resume path was also rerun and completed from validated cache in 5 seconds. A second build invocation completed successfully without source-tree or CMake target collisions.

## Boundary

Both lanes use the packaged `reveal` correctness backend and declare `security_claim=0`. This validates arithmetic, orchestration, and accuracy preservation, not no-reveal end-to-end private deployment.
