# DAZG-Orbit: Reproducible Two-Process Q16/HE Inference

[简体中文](README.zh-CN.md)

DAZG-Orbit is a source-first, offline-buildable reproducibility repository for
two validated CIFAR-100 execution lanes. It packages the maintained C++
two-process runtime, fixed-point model assets, frozen evaluation inputs,
bit-exact references, a minimal Python Q16 oracle, pinned dependencies, and
fail-closed experiment runners.

## Validated results

| Lane | Samples | Arithmetic gate | Accuracy gate |
|---|---:|---:|---:|
| Frozen P60 diagnostic | 10 | strict exact 10/10 | Top-1 10/10; Top-5 10/10 |
| Checkpoint 013 balanced evaluation | 100 | logits exact 100/100; mismatch 0; max delta 0 | Top-1 72/100; Top-5 91/100 |

No retraining is required. The checkpoint-013 release file is a weights-only
artifact containing 77 tensors. N=1000 is intentionally not included in this
release.

> **Security boundary.** Both validated lanes use the packaged `reveal`
> correctness backend and declare `security_claim=0`. They validate arithmetic,
> two-process orchestration, ciphertext linear operations, and accuracy
> preservation. They do not establish a no-reveal end-to-end private-inference
> deployment claim. See [docs/SECURITY_BOUNDARY.md](docs/SECURITY_BOUNDARY.md).

## Repository contents

```text
Datatype/ HE/ Layer/ Model/ OT/ Operator/ Utils/  DAZG-Orbit C++ runtime
Extern/                                           pinned third-party source
experiments/n10_p60/                              frozen N=10 lane
experiments/n100_checkpoint013/                   checkpoint-013 balanced N=100 lane
checkpoints/                                      weights-only checkpoint 013
python/                                           Q16 model and oracle exporter
scripts/                                          build, verification, and release tools
results/                                          compact reviewed result summaries
docs/                                             architecture and reproduction documentation
```

Raw logs, PDFs, previous packages, build products, caches, and unrelated
historical artifacts are excluded.

## Supported environment

The reference environment is Ubuntu 22.04/24.04 or WSL2 Ubuntu on x86-64 with
AVX2. Required build tools are GCC/G++, CMake, Ninja, OpenSSL development
headers, GMP development headers, Python 3, and NumPy. Microsoft SEAL 4.1.2,
Intel HEXL 1.2.6, emp-tool, and emp-ot are vendored as pinned source snapshots.

Install system dependencies automatically:

```bash
./reproduce.sh setup
```

The script uses `apt`; inspect `scripts/setup_ubuntu.sh` before running it on a
shared machine.

## Reproduce from a fresh clone

```bash
git clone https://github.com/<OWNER>/<REPOSITORY>.git
cd <REPOSITORY>

./reproduce.sh verify
DAZG_JOBS=8 ./reproduce.sh build
./reproduce.sh stage-s
./reproduce.sh n10
./reproduce.sh n100
```

For a release archive, verify its manifest first:

```bash
./reproduce.sh verify-manifest
```

After dependencies are installed, the complete sequence is:

```bash
DAZG_JOBS=8 ./reproduce.sh all
```

Generated build products and run evidence are written under `build/` and
`runs/`; both directories are ignored by Git.


### WSL note

The runners support repositories located on Windows-mounted paths such as
a Windows-mounted WSL path. JSON evidence publication includes retry and recovery logic for
temporary DrvFS/indexer locks. A successful two-process sample is not rejected
solely because an atomic report rename was briefly blocked.

## Commands

| Command | Purpose |
|---|---|
| `./reproduce.sh setup` | Install Ubuntu build dependencies |
| `./reproduce.sh verify-manifest` | Verify every file in a release archive |
| `./reproduce.sh verify` | Verify assets, expected metrics, branding, source headers, paths, and syntax |
| `./reproduce.sh build` | Build vendored HEXL/SEAL and isolated N=10/N=100 executors |
| `./reproduce.sh stage-s` | Run the frozen checkpoint-013 Stage-S contract |
| `./reproduce.sh n10` | Run the frozen P60 N=10 lane |
| `./reproduce.sh n100` | Run the checkpoint-013 balanced N=100 lane |
| `./reproduce.sh oracle-n100` | Regenerate the N=100 Q16 oracle; requires CPU PyTorch |
| `./reproduce.sh clean` | Remove local `build/` and `runs/` |
| `./reproduce.sh release --output PATH` | Create a checked GitHub release archive |

## Fail-closed acceptance gates

N=10 passes only when all ten samples are strict exact and both Top-1 and Top-5
are 10/10.

N=100 passes only when all conditions hold:

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

DAZG-Orbit documents acceleration logic directly in the maintained source:

- block-circulant cyclic-NTT encoding;
- sparse baby-step/giant-step scheduling;
- reuse of repeated ciphertext rotations;
- omission of inactive packed diagonals;
- K3/S2 phase packing with correctness-first fallback;
- bounded parallel share/ciphertext conversion;
- runtime counters for rotations, `mul_plain`, `add_inplace`, communication, and
  protocol rounds where the backend exposes them.

See [docs/CIPHERTEXT_ACCELERATION.md](docs/CIPHERTEXT_ACCELERATION.md).

## Checkpoint and data assets

The public checkpoint is:

```text
checkpoints/dazg_orbit_checkpoint013_weights.pt
```

- tensors: 77;
- SHA-256: `f7db0ff0bae94d806275c23ff46ded8fbdebc8a72cbf193004e915d293c28c3e`;
- tensor-content fingerprint: `b9d36a2e92431eac464a0f5cb8d529e28fa095ab96038bf5f040ae639bb1362f`;
- optimizer/scheduler state: excluded;
- retraining required: false.

The repository contains only the fixed evaluation tensors needed by the
published experiments, not the complete CIFAR-100 dataset. See
[docs/DATA_AND_WEIGHTS.md](docs/DATA_AND_WEIGHTS.md).

## Documentation

- [Installation](docs/INSTALL.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Ciphertext acceleration](docs/CIPHERTEXT_ACCELERATION.md)
- [Reproducibility protocol](docs/REPRODUCIBILITY.md)
- [Validation](docs/VALIDATION.md)
- [v3 release validation](docs/V3_RELEASE_VALIDATION.md)
- [v4 release validation](docs/V4_RELEASE_VALIDATION.md)
- [Performance](docs/PERFORMANCE.md)
- [Data and weights](docs/DATA_AND_WEIGHTS.md)
- [Security boundary](docs/SECURITY_BOUNDARY.md)
- [Branding and attribution](docs/BRANDING_AND_ATTRIBUTION.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [GitHub publication](docs/GITHUB_RELEASE.md)

## GitHub publication

1. Create an empty GitHub repository. Do not initialise it with an additional
   README, `.gitignore`, or license because this tree already contains them.
2. Review `LICENSE_NOTICE.md`, choose a first-party license, and confirm that the
   checkpoint and fixed evaluation tensors may be redistributed.
3. Run the release checks:

```bash
./reproduce.sh verify
DAZG_JOBS=8 ./reproduce.sh build
./reproduce.sh stage-s
./reproduce.sh n10
./reproduce.sh n100
./reproduce.sh release --output dist/dazg-orbit-secure-inference-repro.tar.gz
```

4. Commit and push the source tree, not the generated `.tar.gz`:

```bash
git init -b main
git add .
git status
git commit -m "Initial DAZG-Orbit reproducibility release"
git remote add origin https://github.com/<OWNER>/<REPOSITORY>.git
git push -u origin main
```

## Attribution and licensing

DAZG-Orbit maintains the integration, acceleration, correctness gates, and
reproduction layer. Accurate upstream provenance is retained in
[NOTICE_UPSTREAM.md](NOTICE_UPSTREAM.md) and `THIRD_PARTY_LOCK.json`. Vendored
third-party copyright and license notices are not rewritten.

Before making the repository public, select a license for DAZG-Orbit-maintained
code and confirm redistribution rights for the checkpoint and fixed
CIFAR-100-derived tensors. See [LICENSE_NOTICE.md](LICENSE_NOTICE.md).
