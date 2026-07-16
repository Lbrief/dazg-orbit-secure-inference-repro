#!/usr/bin/env bash
# DAZG-Orbit Project Source File
# Component: scripts/release_check.sh
# Purpose: Build, verification, release, and reproducibility automation.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
"$ROOT/scripts/static_checks.sh"

BAD_SIZE="$(find "$ROOT" \
  -path "$ROOT/build" -prune -o \
  -path "$ROOT/runs" -prune -o \
  -path "$ROOT/dist" -prune -o \
  -type f -size +99M -print -quit)"
[[ -z "$BAD_SIZE" ]] || { echo "[RELEASE CHECK FAIL] file exceeds GitHub 100 MB limit: $BAD_SIZE"; exit 10; }

if grep -RIlE --binary-files=without-match \
  -e '-----BEGIN (RSA |OPENSSH |EC |DSA )?PRIVATE KEY-----|ghp_[A-Za-z0-9]{20,}|AKIA[0-9A-Z]{16}|sk-[A-Za-z0-9]{20,}' \
  "$ROOT" --exclude-dir=build --exclude-dir=runs --exclude-dir=dist --exclude-dir=.git | grep -q .; then
  echo '[RELEASE CHECK FAIL] possible credential or private key found'
  exit 11
fi

[[ -f "$ROOT/LICENSE_NOTICE.md" ]] || { echo '[RELEASE CHECK FAIL] LICENSE_NOTICE.md missing'; exit 12; }
[[ -f "$ROOT/CITATION.cff" ]] || { echo '[RELEASE CHECK FAIL] CITATION.cff missing'; exit 13; }

printf '[RELEASE CHECK PASS]\n'
