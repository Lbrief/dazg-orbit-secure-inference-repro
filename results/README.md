# Result summaries

Only compact, reviewed, machine-readable summaries are tracked. Raw logs,
execution tensors, generated result archives, and build products are recreated
locally and excluded from Git.

## Frozen reference evidence

- `n10_reference.json`: validated P60 reference result and operation counters.
- `n100_reference.json`: validated checkpoint-013 balanced N=100 result.
- `n100_reference_per_sample.csv`: 100 reference-run sample measurements.

## Latest DAZG-Orbit release validation

- `clean_repository_validation.json`: full post-branding build and execution summary.
- `n10_clean_reproduction.json`: fresh isolated N=10 reproduction summary.
- `n10_clean_per_sample.csv`: 10 per-sample measurements.
- `n100_clean_reproduction.json`: fresh isolated N=100 reproduction summary.
- `n100_clean_per_sample.csv`: 100 per-sample measurements.

Reference and validation timing values may differ across machines. Exactness,
accuracy, asset hashes, and fail-closed gates are the deterministic criteria.
