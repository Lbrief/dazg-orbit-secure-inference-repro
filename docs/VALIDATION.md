# DAZG-Orbit release validation

The release source tree was rebuilt after active branding changes, English
source-header insertion, acceleration comments, and the weights-only checkpoint
export. The validation did not reuse an old executable or completed sample
cache.

## Validation sequence

```text
asset, branding, header, path, and syntax verification
-> offline Intel HEXL build
-> offline Microsoft SEAL build
-> isolated P60 N=10 build
-> isolated checkpoint-013 N=100 build
-> Stage-S frozen-table contract
-> complete N=10 two-process run
-> complete N=100 two-process run
-> N=100 Python Q16 oracle regeneration
```

## Results

| Check | Result |
|---|---:|
| Active branding scan | PASS |
| Maintained source headers | PASS; 219 files |
| Vendored dependency build | PASS |
| P60 N=10 strict exact | 10/10 |
| P60 N=10 Top-1 / Top-5 | 10/10 / 10/10 |
| Checkpoint-013 Stage-S contract | PASS |
| Regenerated N=100 oracle SHA-256 | exact match |
| N=100 two-process logits exact | 100/100 |
| N=100 mismatched logit elements | 0 |
| N=100 maximum absolute logit delta | 0 |
| N=100 frozen Top-5 rows exact | 100/100 |
| N=100 Top-1 / Top-5 | 72/100 / 91/100 |
| N=1000 executed | no |
| Retraining required | no |

Machine-readable evidence is stored in `results/`; the latest human-readable
summary is `docs/V3_RELEASE_VALIDATION.md`. Raw build and execution logs are
deliberately excluded from the release archive and are regenerated under
`build/` and `runs/`.

## Interpretation

The validation establishes reproducibility for the bundled assets and reveal
correctness backend. It does not establish no-reveal confidentiality, and it
does not validate N=1000.
