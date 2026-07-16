#!/usr/bin/env bash
# DAZG-Orbit Project Source File
# Component: scripts/check_platform.sh
# Purpose: Build, verification, release, and reproducibility automation.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
set -Eeuo pipefail
ARCH="$(uname -m)"
[[ "$ARCH" == "x86_64" || "$ARCH" == "amd64" ]] || {
  echo "[PLATFORM FAIL] unsupported architecture: $ARCH; this release targets x86-64"
  exit 2
}
if [[ -r /proc/cpuinfo ]]; then
  FLAGS="$(grep -m1 '^flags' /proc/cpuinfo || true)"
  missing=()
  for flag in avx2 aes sse4_1; do
    [[ " $FLAGS " == *" $flag "* ]] || missing+=("$flag")
  done
  if ((${#missing[@]})); then
    echo "[PLATFORM FAIL] missing CPU flags: ${missing[*]}"
    exit 3
  fi
fi
echo "[PLATFORM PASS] arch=$ARCH required_flags=avx2,aes,sse4_1"
