#!/usr/bin/env bash
# DAZG-Orbit Project Source File
# Component: scripts/static_checks.sh
# Purpose: Build, verification, release, and reproducibility automation.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"

python3 "$ROOT/scripts/verify_assets.py"
python3 "$ROOT/scripts/verify_results.py"
python3 "$ROOT/scripts/check_branding.py"
python3 "$ROOT/scripts/check_source_headers.py"
python3 "$ROOT/scripts/test_runtime_io.py"

# Release-forbidden files are checked only in source-controlled paths. Local
# build/run products are intentionally ignored by Git and regenerated locally.
if find "$ROOT" \
  -path "$ROOT/build" -prune -o \
  -path "$ROOT/runs" -prune -o \
  -path "$ROOT/dist" -prune -o \
  -type f \( \
    -iname '*.pdf' -o -iname '*.log' -o -iname '*.core' -o \
    -iname '*.pyc' -o -iname '*.pyo' -o -iname '*.tmp' -o \
    -iname '*.tar' -o -iname '*.tar.gz' -o -iname '*.tgz' -o \
    -iname '*.zip' \
  \) -print -quit | grep -q .; then
  echo '[STATIC CHECK FAIL] release-forbidden source file found'
  exit 4
fi

if find "$ROOT" \
  -path "$ROOT/build" -prune -o \
  -path "$ROOT/runs" -prune -o \
  -path "$ROOT/dist" -prune -o \
  -type l -lname '/*' -print -quit | grep -q .; then
  echo '[STATIC CHECK FAIL] absolute symlink found'
  exit 5
fi

# Check first-party and release documentation for local machine paths. Vendored
# third-party sources are excluded because upstream build files may legitimately
# contain platform examples.
if grep -RInE '/home/[^[:space:]]+|/mnt/[a-z]/|[A-Za-z]:\\\\' \
  "$ROOT/Datatype" "$ROOT/HE" "$ROOT/Layer" "$ROOT/Model" \
  "$ROOT/OT" "$ROOT/Operator" "$ROOT/Utils" "$ROOT/experiments" \
  "$ROOT/python" "$ROOT/scripts" "$ROOT/docs" "$ROOT/README.md" \
  "$ROOT/README.zh-CN.md" \
  --exclude-dir=__pycache__ --exclude='*.pt' --exclude='*.npy' \
  --exclude=static_checks.sh; then
  echo '[STATIC CHECK FAIL] non-portable local path found'
  exit 6
fi

bash -n "$ROOT/reproduce.sh" "$ROOT"/scripts/*.sh

# Parse Python without creating __pycache__ files.
python3 - "$ROOT" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
paths = []
for base in [root / 'scripts', root / 'python', root / 'experiments/n10_p60', root / 'experiments/n100_checkpoint013/tools']:
    paths.extend(p for p in base.rglob('*.py') if '__pycache__' not in p.parts)
for path in sorted(paths):
    compile(path.read_bytes(), str(path), 'exec')
print(f'[PYTHON PARSE PASS] files={len(paths)}')
PY

# The two experiment lanes must remain isolated at configure time.
grep -q 'configure_lane n10 ON OFF OFF' "$ROOT/scripts/build.sh"
grep -q 'configure_lane n100 OFF ON ON' "$ROOT/scripts/build.sh"
grep -q 'DAZG_CHECKPOINT013_STAGE_S_TAIL_FIX' "$ROOT/HE/src/tfhe/DAZGRLut.cpp"

printf '[STATIC CHECKS PASS]\n'
