# Reproducibility protocol

## Immutable assets

`./reproduce.sh verify` checks:

- all P60 N=10 payloads against `experiments/n10_p60/PAYLOAD_MANIFEST.json`;
- all 77 checkpoint-013 Q16 tensors against `payload_manifest.json`;
- the Stage-S table size and SHA-256;
- the original checkpoint-013 SHA-256;
- compact reference and validation summaries.

## Build

`./reproduce.sh build` compiles vendored Intel HEXL 1.2.6 and Microsoft SEAL 4.1.2, then builds isolated N=10 and N=100 executors. The build does not download source code.

## N=10 gate

`./reproduce.sh n10` performs route/scorer self-tests, sample-0 checks, and the complete 10-sample P60 diagnostic run. It passes only when:

- all 10 reconstructed outputs are strict exact;
- Top-1 is 10/10;
- Top-5 is 10/10;
- the selected route is `main0_pcoi`;
- no fallback occurs.

## N=100 gate

`./reproduce.sh n100` performs, in order:

1. asset and Q16-oracle preflight;
2. frozen Stage-S contract test;
3. sample 0 exactness;
4. first 10 balanced samples exactness;
5. full balanced N=100;
6. final accuracy gate: Top-1 72/100 and Top-5 91/100.

Any failure stops the run and packages the first failing evidence. The target has no N=1000 branch.

## Determinism

Secret shares, ports, process scheduling, and timings can vary. Reconstructed logits, predictions, exactness gates, and accuracy are deterministic for the bundled assets. Binary SHA-256 values can vary across compilers; source and asset identities are the portable invariants.

## Output locations

Local run products are written below `runs/`, which is excluded from Git and release archives. Only compact, reviewed summaries are tracked under `results/`.
