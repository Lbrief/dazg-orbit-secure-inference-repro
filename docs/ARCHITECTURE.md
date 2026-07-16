# Architecture and lane isolation

## Shared runtime

The first-party C++ runtime is organized under `Datatype/`, `HE/`, `OT/`, `Operator/`, `Layer/`, `Model/`, and `Utils/`. Both experiment executors use the same fixed-point and two-process infrastructure, but they are compiled in separate CMake trees.

## N=10 lane

`experiments/n10_p60/` contains the frozen P60 source, 77 Q16 parameters, 10 fixed inputs and labels, the PCOI K3/S2 route, the scorer, and the frozen gate metadata.

This lane is compiled with `DAZG_CHECKPOINT013_STAGE_S_TAIL_FIX=OFF`, preserving the P60 Stage-S evaluator that produced the validated 10/10 result.

## N=100 lane

`experiments/n100_checkpoint013/` contains the checkpoint-013 executor, 77 Q16 tensors, balanced N=100 inputs and labels, the 100x100 Q16 oracle, Top-5 reference rows, and the frozen Stage-S table.

This lane is compiled with `DAZG_CHECKPOINT013_STAGE_S_TAIL_FIX=ON`. The central Stage-S evaluator remains frozen; only the deterministic tails are enforced:

```text
x <= -8 -> 0
-8 < x < +8 -> frozen Stage-S evaluator
x >= +8 -> x
```

Stride-2 K3 transition layers use the generic correctness path rather than the unvalidated compact polyphase optimization.

## Build isolation

`scripts/build.sh` creates:

```text
build/n10/
build/n100/
```

The two build trees prevent incompatible Stage-S compile definitions or stale CMake state from crossing lane boundaries. Convenience symlinks under `build/v720_bin` and `build/stage4_bin` point to the corresponding isolated executables.
