# Data and weights

The repository includes only fixed experiment tensors, not the full CIFAR-100
dataset.

## Checkpoint 013

`checkpoints/dazg_orbit_checkpoint013_weights.pt` contains the 77 model tensors
from checkpoint 013 and excludes optimizer state, scheduler state, training
arguments, and local paths.

- SHA-256: `f7db0ff0bae94d806275c23ff46ded8fbdebc8a72cbf193004e915d293c28c3e`
- tensor-content fingerprint: `b9d36a2e92431eac464a0f5cb8d529e28fa095ab96038bf5f040ae639bb1362f`
- source checkpoint SHA-256: `5a103795fff72d3bbc0bcffac4839f49afe4eb1bd97541f9bab8bcc75eb98569`
- retraining required: no

Runtime execution uses the frozen Q16 payloads under
`experiments/n100_checkpoint013/assets/payload/`. CPU PyTorch is needed only if
you choose to regenerate the Q16 oracle with `./reproduce.sh oracle-n100`.

Before public redistribution, confirm that the checkpoint, Q16 tensors, and
fixed CIFAR-100-derived inputs may be distributed.
