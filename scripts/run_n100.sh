#!/usr/bin/env bash
# DAZG-Orbit Project Source File
# Component: scripts/run_n100.sh
# Purpose: Build, verification, release, and reproducibility automation.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
[[ -x "$ROOT/build/stage4_bin/dazg_checkpoint013_n100_executor" ]] || "$ROOT/scripts/build.sh"
RUN_BASE="$ROOT/runs/n100"
RESULTS="$RUN_BASE/results"
SESSION="$RUN_BASE/session"
mkdir -p "$RESULTS" "$SESSION"
TABLE="$ROOT/experiments/n100_checkpoint013/assets/stage_s/stage_s_gelu_q16_i64.bin"
python3 "$ROOT/experiments/n100_checkpoint013/tools/preflight.py" \
  --assets "$ROOT/experiments/n100_checkpoint013/assets" --out "$SESSION/preflight.json"
python3 "$ROOT/experiments/n100_checkpoint013/tools/run_stage_s_contract.py" \
  --binary "$ROOT/build/stage4_bin/dazg_checkpoint013_stage_s_contract_test" \
  --table "$TABLE" --out "$SESSION/stage_s_contract.json"
export DAZG_ORBIT_STAGE_S_Q16_TABLE="$TABLE"
cd "$ROOT"
python3 -u "$ROOT/experiments/n100_checkpoint013/tools/run_n100.py" \
  --binary "$ROOT/build/stage4_bin/dazg_checkpoint013_n100_executor" \
  --repo "$ROOT" \
  --assets "$ROOT/experiments/n100_checkpoint013/assets" \
  --package-root "$ROOT/experiments/n100_checkpoint013" \
  --run-base "$RUN_BASE" \
  --downloads "$RESULTS" \
  --session-dir "$SESSION"
