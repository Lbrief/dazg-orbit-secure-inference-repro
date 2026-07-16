#!/usr/bin/env bash
# DAZG-Orbit Project Source File
# Component: scripts/build.sh
# Purpose: Build, verification, release, and reproducibility automation.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
JOBS="${DAZG_JOBS:-$(nproc 2>/dev/null || echo 2)}"
"$ROOT/scripts/check_platform.sh"
"$ROOT/scripts/bootstrap_dependencies.sh"
# shellcheck disable=SC1091
source "$ROOT/build/deps/paths.env"

configure_lane() {
  local lane="$1" n10="$2" n100="$3" tail_fix="$4"
  cmake -S "$ROOT" -B "$ROOT/build/$lane" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DHEXL_DIR="$HEXL_DIR" -DSEAL_DIR="$SEAL_DIR" \
    -DDAZG_BUILD_N10="$n10" -DDAZG_BUILD_N100="$n100" \
    -DDAZG_CHECKPOINT013_STAGE_S_TAIL_FIX="$tail_fix"
}

# The two lanes intentionally use separate build trees. P60 retains its frozen
# Stage-S semantics; checkpoint-013 enables the corrected positive-tail rule.
configure_lane n10 ON OFF OFF
cmake --build "$ROOT/build/n10" --parallel "$JOBS" --target \
  qahl_v720_to_h8_exact_executor

configure_lane n100 OFF ON ON
cmake --build "$ROOT/build/n100" --parallel "$JOBS" --target \
  dazg_checkpoint013_stage_s_contract_test \
  dazg_checkpoint013_n100_executor

ln -sfn "$ROOT/build/n10/v720_bin" "$ROOT/build/v720_bin"
ln -sfn "$ROOT/build/n100/stage4_bin" "$ROOT/build/stage4_bin"
echo '[BUILD PASS] Isolated N=10 and N=100 executors are ready.'
