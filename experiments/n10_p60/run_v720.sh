#!/usr/bin/env bash
# DAZG-Orbit Project Source File
# Component: experiments/n10_p60/run_v720.sh
# Purpose: Build, verification, release, and reproducibility automation.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
set -Eeuo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
export PYTHONUNBUFFERED=1
exec python3 -u "$SCRIPT_DIR/run_v720.py" --root "$ROOT" "$@"
