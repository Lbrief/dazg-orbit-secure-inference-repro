#!/usr/bin/env bash
# DAZG-Orbit Project Source File
# Component: scripts/run_n10.sh
# Purpose: Build, verification, release, and reproducibility automation.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
[[ -x "$ROOT/build/v720_bin/qahl_v720_to_h8_exact_executor" ]] || "$ROOT/scripts/build.sh"
mkdir -p "$ROOT/runs/n10"
export DAZG_ORBIT_V720_OUTPUT_BASE="$ROOT/runs/n10"
export DAZG_ORBIT_V743R8P74_ADAPTER_POLICY=reveal
cd "$ROOT"
python3 -u "$ROOT/experiments/n10_p60/run_v720.py" --root "$ROOT"
