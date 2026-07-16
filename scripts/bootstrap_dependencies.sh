#!/usr/bin/env bash
# DAZG-Orbit Project Source File
# Component: scripts/bootstrap_dependencies.sh
# Purpose: Build, verification, release, and reproducibility automation.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
JOBS="${DAZG_JOBS:-$(nproc 2>/dev/null || echo 2)}"
DEPS="$ROOT/build/deps"
HEXL_PREFIX="$DEPS/hexl"
SEAL_PREFIX="$DEPS/seal"
mkdir -p "$ROOT/build/_deps" "$DEPS"

HEXL_CONFIG="$(find "$HEXL_PREFIX" -type f \( -name 'HEXLConfig.cmake' -o -name 'hexlConfig.cmake' \) -print -quit 2>/dev/null || true)"
if [[ -z "$HEXL_CONFIG" ]]; then
  cmake -S "$ROOT/Extern/hexl" -B "$ROOT/build/_deps/hexl-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$HEXL_PREFIX" \
    -DHEXL_BENCHMARK=OFF -DHEXL_TESTING=OFF -DHEXL_DOCS=OFF \
    -DHEXL_EXPERIMENTAL=OFF -DHEXL_SHARED_LIB=OFF
  cmake --build "$ROOT/build/_deps/hexl-build" --parallel "$JOBS"
  cmake --install "$ROOT/build/_deps/hexl-build"
  HEXL_CONFIG="$(find "$HEXL_PREFIX" -type f \( -name 'HEXLConfig.cmake' -o -name 'hexlConfig.cmake' \) -print -quit)"
fi
[[ -n "$HEXL_CONFIG" ]] || { echo '[ERROR] HEXL config not produced'; exit 2; }
HEXL_DIR="$(dirname "$HEXL_CONFIG")"

SEAL_CONFIG="$(find "$SEAL_PREFIX" -type f -name 'SEALConfig.cmake' -print -quit 2>/dev/null || true)"
if [[ -z "$SEAL_CONFIG" ]]; then
  cmake -S "$ROOT/Extern/SEAL" -B "$ROOT/build/_deps/seal-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$SEAL_PREFIX" \
    -DSEAL_BUILD_DEPS=OFF \
    -DSEAL_USE_MSGSL=OFF -DSEAL_USE_ZLIB=OFF -DSEAL_USE_ZSTD=OFF \
    -DSEAL_USE_INTEL_HEXL=ON -DHEXL_DIR="$HEXL_DIR" \
    -DSEAL_BUILD_EXAMPLES=OFF -DSEAL_BUILD_TESTS=OFF -DSEAL_BUILD_BENCH=OFF \
    -DSEAL_BUILD_SEAL_C=OFF -DBUILD_SHARED_LIBS=OFF
  cmake --build "$ROOT/build/_deps/seal-build" --parallel "$JOBS"
  cmake --install "$ROOT/build/_deps/seal-build"
  SEAL_CONFIG="$(find "$SEAL_PREFIX" -type f -name 'SEALConfig.cmake' -print -quit)"
fi
[[ -n "$SEAL_CONFIG" ]] || { echo '[ERROR] SEAL config not produced'; exit 3; }
SEAL_DIR="$(dirname "$SEAL_CONFIG")"
printf 'HEXL_DIR=%q\nSEAL_DIR=%q\n' "$HEXL_DIR" "$SEAL_DIR" > "$DEPS/paths.env"
echo "[OK] HEXL_DIR=$HEXL_DIR"
echo "[OK] SEAL_DIR=$SEAL_DIR"
