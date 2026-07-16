#!/usr/bin/env bash
# DAZG-Orbit Project Source File
# Component: scripts/verify_manifest.sh
# Purpose: Build, verification, release, and reproducibility automation.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
[[ -f "$ROOT/MANIFEST.sha256" ]] || {
  echo '[MANIFEST] MANIFEST.sha256 is generated in release archives; not present in the development tree.'
  exit 0
}
cd "$ROOT"
sha256sum -c MANIFEST.sha256
echo '[MANIFEST VERIFY PASS]'
