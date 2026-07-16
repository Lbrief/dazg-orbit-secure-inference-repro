# Installation

## Supported environment

- Ubuntu 22.04 or 24.04, including WSL2 Ubuntu;
- x86-64 CPU with AVX2, AES-NI, and SSE4.1;
- GCC/G++ 11 or newer;
- CMake 3.20 or newer and Ninja;
- Python 3.10 or newer with NumPy;
- approximately 8 GB RAM and 5 GB free disk space.

CUDA, Phantom-FHE, and model retraining are not required.

## Automated setup

```bash
./reproduce.sh setup
```

Equivalent Ubuntu packages:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config \
  libssl-dev libgmp-dev \
  python3 python3-venv python3-pip python3-numpy
```

## Verify before building

```bash
./reproduce.sh verify
```

This checks all fixed assets against SHA-256 manifests, validates the N=10 and N=100 reference summaries, rejects non-portable paths and release-forbidden files, and parses every first-party Python and shell entry point.

## Build

```bash
DAZG_JOBS=8 ./reproduce.sh build
```

Use a smaller `DAZG_JOBS` value on memory-constrained machines. Dependencies are compiled from `Extern/`; the build script performs no network download.
