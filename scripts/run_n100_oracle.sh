#!/usr/bin/env bash
# DAZG-Orbit Project Source File
# Component: scripts/run_n100_oracle.sh
# Purpose: Build, verification, release, and reproducibility automation.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
mkdir -p "$ROOT/runs/oracle"
python3 "$ROOT/python/q16_oracle_reference.py" \
  --assets "$ROOT/experiments/n100_checkpoint013/assets" \
  --out "$ROOT/runs/oracle/checkpoint013_balanced_n100_logits_q16_i64.npy"
sha256sum "$ROOT/runs/oracle/checkpoint013_balanced_n100_logits_q16_i64.npy"
