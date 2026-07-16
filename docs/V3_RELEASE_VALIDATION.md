# DAZG-Orbit v3 release validation

The DAZG-Orbit-branded source tree was rebuilt and rerun after the active-code
renaming, English source-header insertion, acceleration comments, and public
weights-only checkpoint generation. No old executable or cached sample result
was used for the validation summarized here.

## Commands

```bash
./reproduce.sh verify
DAZG_JOBS=8 ./reproduce.sh build
./reproduce.sh stage-s
./reproduce.sh n10
./reproduce.sh n100
./reproduce.sh oracle-n100
```

## Results

| Gate | Result |
|---|---:|
| Active branding scan | PASS; no legacy active name |
| Maintained source headers | PASS; 219 files |
| Offline HEXL/SEAL build | PASS |
| Stage-S frozen-table contract | PASS |
| N=10 strict exact | 10/10 |
| N=10 Top-1 / Top-5 | 10/10 / 10/10 |
| N=100 sample status | 100/100 PASS |
| N=100 logits exact | 100/100 |
| N=100 mismatch / max delta | 0 / 0 |
| N=100 frozen Top-5 rows exact | 100/100 |
| N=100 Top-1 / Top-5 | 72/100 / 91/100 |
| Regenerated Q16 oracle | PASS; SHA-256 `ae2cd654ddce520317bcd3b4d0c78c7d3279c81ed46643533617d67ee2665fe7` |
| Retraining | not required |
| N=1000 | not included or executed |

## Executable identity

- N=10: `ec570a3e191f7fd3363724a2b8702812cc646b25004bfb1174ecaf8862752891`
- N=100: `7fa8ccaef9d5b2e0f2b853ef063d5e1fd2bf8c664287271e67935b6ce9290b77`
- Stage-S contract: `afcf9ddcf6a10ec69b8c763e002c35d5f311b992ddeb4d1762c59caa6e372a4e`

## Latest timing

- N=10 pair wall mean: 7.074003 s/sample;
  HE compute mean: 3169880.3 us/sample;
  rotations: 125/sample.
- N=100 pair wall total: 199.043357 s;
  mean: 1.990434 s/sample.

Timing is environment-dependent. Exactness and accuracy gates are the release
acceptance criteria. Both lanes use the reveal correctness backend and declare
`security_claim=0`.
