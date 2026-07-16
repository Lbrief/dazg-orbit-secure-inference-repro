# Contributing

1. Do not modify frozen weights, inputs, labels, oracle logits, or expected metrics without creating a new versioned experiment lane.
2. Keep N=10 and N=100 build semantics isolated.
3. Run `./reproduce.sh verify` and `./reproduce.sh build` before submitting changes.
4. Changes to arithmetic or packing must pass the relevant full reproduction, not only compilation or Top-5 checks.
5. Do not weaken fail-closed gates or describe the reveal backend as no-reveal security.
6. Do not commit `build/`, `runs/`, raw logs, PDFs, caches, or private credentials.

A pull request should explain the scientific contract being changed and include compact machine-readable validation evidence.
