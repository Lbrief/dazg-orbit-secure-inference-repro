# DAZG-ORBIT: Certified Active-Orbit Executor Scheduling for High-Performance Homomorphic Private Inference

[简体中文](README.zh-CN.md)

DAZG-Orbit is a source-first reproducibility repository for validated
two-process fixed-point and homomorphic-encryption inference on CIFAR-100.

The repository includes the maintained C++ runtime, frozen model weights,
deterministic evaluation inputs, bit-exact references, a compact Python Q16
oracle, pinned dependencies, and fail-closed experiment runners.

## Current release scope

This public release currently provides the fully audited CIFAR-100 experiment
lanes.

We follow an **audit-first staged-release policy**: an experiment is added only
after its source tree, model identity, preprocessing, fixed-point arithmetic,
reference outputs, two-process execution, accuracy metrics, and build
instructions have all passed the same reproducibility checks.

CIFAR-100 is released first because it offers a compact but non-trivial
100-class benchmark for validating packing, ciphertext linear operations,
activation semantics, numerical exactness, and accuracy preservation. Additional
datasets, models, sample scales, and hardware profiles will be added as
independent versioned lanes after they pass the same clean-build and validation
process.

## Validated results

| Lane | Samples | Arithmetic gate | Accuracy gate |
|---|---:|---:|---:|
| Frozen P60 diagnostic | 10 | strict exact 10/10 | Top-1 10/10; Top-5 10/10 |
| Checkpoint 013 balanced evaluation | 100 | logits exact 100/100; mismatch 0; max delta 0 | Top-1 72/100; Top-5 91/100 |

No retraining is required. The public checkpoint is a weights-only artifact
containing 77 tensors.

N=1000 is intentionally not included in this release. It will be published as a
separate lane after its fixed input set, oracle, execution identity, failure
recovery, and aggregate metrics are independently audited.

> **Security boundary.** The validated lanes use the packaged `reveal`
> correctness backend and declare `security_claim=0`. They validate arithmetic
> consistency, two-process orchestration, ciphertext linear operations, and
> accuracy preservation. They do not establish a complete no-reveal
> end-to-end private-inference deployment claim.

## Repository structure

```text
Datatype/ HE/ Layer/ Model/ OT/ Operator/ Utils/  DAZG-Orbit C++ runtime
Extern/                                           pinned third-party source
experiments/n10_p60/                              frozen N=10 lane
experiments/n100_checkpoint013/                   checkpoint-013 N=100 lane
checkpoints/                                      weights-only checkpoint
python/                                           Q16 model and oracle exporter
scripts/                                          build and verification tools
results/                                          compact reviewed summaries
docs/                                             technical documentation
```

Raw logs, PDFs, old release archives, failed historical packages, build
products, caches, and unrelated experimental files are excluded.

## Supported environment

Reference platforms:

- Ubuntu 22.04 or 24.04
- WSL2 Ubuntu
- x86-64 CPU with AVX2
- GCC/G++
- CMake and Ninja
- OpenSSL and GMP development packages
- Python 3 and NumPy

Microsoft SEAL 4.1.2, Intel HEXL 1.2.6, emp-tool, and emp-ot are included as
pinned source snapshots.

Install the required Ubuntu packages:

```bash
bash ./reproduce.sh setup
```

## Reproduce from a fresh clone

```bash
git clone https://github.com/Lbrief/dazg-orbit-secure-inference-repro.git
cd dazg-orbit-secure-inference-repro

bash ./reproduce.sh verify
DAZG_JOBS=8 bash ./reproduce.sh build
bash ./reproduce.sh stage-s
bash ./reproduce.sh n10
bash ./reproduce.sh n100
```

After dependencies are installed, the complete sequence can be run with:

```bash
DAZG_JOBS=8 bash ./reproduce.sh all
```

Build products and run evidence are written under `build/` and `runs/`. Both
directories are excluded by `.gitignore`.

Using `bash ./reproduce.sh ...` is recommended because it does not depend on the
executable permission bit being preserved by Windows-mounted filesystems or
archive extraction tools.

## Commands

| Command | Purpose |
|---|---|
| `bash ./reproduce.sh setup` | Install Ubuntu dependencies |
| `bash ./reproduce.sh verify-manifest` | Verify a packaged release manifest |
| `bash ./reproduce.sh verify` | Verify assets, metrics, paths, branding, and syntax |
| `DAZG_JOBS=8 bash ./reproduce.sh build` | Build HEXL, SEAL, and both executors |
| `bash ./reproduce.sh stage-s` | Run the checkpoint-013 Stage-S contract |
| `bash ./reproduce.sh n10` | Reproduce the frozen P60 N=10 lane |
| `bash ./reproduce.sh n100` | Reproduce the checkpoint-013 N=100 lane |
| `bash ./reproduce.sh oracle-n100` | Regenerate the N=100 Q16 oracle; requires PyTorch |
| `bash ./reproduce.sh clean` | Remove local build and run directories |
| `bash ./reproduce.sh release --output PATH` | Create a checked source archive |

## Fail-closed acceptance gates

### N=10

The N=10 lane passes only when:

- all 10 samples complete;
- server and client return code zero for every sample;
- strict arithmetic exactness is 10/10;
- Top-1 is 10/10;
- Top-5 is 10/10.

### N=100

The N=100 lane passes only when:

- completed samples: 100/100;
- server/client return code zero: 100/100;
- two-process logits exact: 100/100;
- mismatched logit elements: 0;
- maximum absolute logit delta: 0;
- frozen reference Top-5 rows exact: 100/100;
- Top-1: 72/100;
- Top-5: 91/100.

Sample 0 and the first ten balanced samples are hard gates before the remaining
N=100 evaluation.

## Ciphertext acceleration

The maintained source documents the main acceleration mechanisms:

- block-circulant cyclic-NTT encoding;
- sparse baby-step/giant-step scheduling;
- reuse of repeated ciphertext rotations;
- omission of inactive packed diagonals;
- K3/S2 phase packing with correctness-first fallback;
- bounded parallel share/ciphertext conversion;
- runtime counters for rotations, `mul_plain`, `add_inplace`, communication, and
  protocol rounds where supported.

See [docs/CIPHERTEXT_ACCELERATION.md](docs/CIPHERTEXT_ACCELERATION.md).

## Checkpoint and evaluation assets

Public checkpoint:

```text
checkpoints/dazg_orbit_checkpoint013_weights.pt
```

Properties:

- tensors: 77;
- SHA-256:
  `f7db0ff0bae94d806275c23ff46ded8fbdebc8a72cbf193004e915d293c28c3e`;
- tensor-content fingerprint:
  `b9d36a2e92431eac464a0f5cb8d529e28fa095ab96038bf5f040ae639bb1362f`;
- optimizer state: excluded;
- scheduler state: excluded;
- retraining required: false.

The repository contains only the fixed CIFAR-100 tensors required by the
published N=10 and N=100 experiments, not the complete CIFAR-100 dataset.

Future releases will expand the public benchmark coverage with additional
datasets, models, larger deterministic sample sets, more performance counters,
and separately validated secure-execution configurations.

## Documentation

- [Installation](docs/INSTALL.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Ciphertext acceleration](docs/CIPHERTEXT_ACCELERATION.md)
- [Reproducibility protocol](docs/REPRODUCIBILITY.md)
- [Validation](docs/VALIDATION.md)
- [Performance](docs/PERFORMANCE.md)
- [Data and weights](docs/DATA_AND_WEIGHTS.md)
- [Security boundary](docs/SECURITY_BOUNDARY.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [GitHub publication](docs/GITHUB_RELEASE.md)

## Attribution and licensing

DAZG-Orbit maintains the integration, ciphertext-acceleration logic,
correctness gates, experiment isolation, and reproducibility layer.

Upstream provenance is retained in
[NOTICE_UPSTREAM.md](NOTICE_UPSTREAM.md) and `THIRD_PARTY_LOCK.json`.
Third-party copyright and license notices are not rewritten.

Before redistributing a public release, confirm the license for
DAZG-Orbit-maintained code and the redistribution rights for the checkpoint and
fixed CIFAR-100-derived tensors. See [LICENSE_NOTICE.md](LICENSE_NOTICE.md).
