#!/usr/bin/env bash
# DAZG-Orbit Project Source File
# Component: reproduce.sh
# Purpose: Build, verification, release, and reproducibility automation.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
case "${1:-help}" in
  setup) exec "$ROOT/scripts/setup_ubuntu.sh" ;;
  verify) exec "$ROOT/scripts/static_checks.sh" ;;
  verify-manifest) exec "$ROOT/scripts/verify_manifest.sh" ;;
  build) exec "$ROOT/scripts/build.sh" ;;
  stage-s) exec "$ROOT/scripts/run_stage_s.sh" ;;
  n10) exec "$ROOT/scripts/run_n10.sh" ;;
  n100) exec "$ROOT/scripts/run_n100.sh" ;;
  oracle-n100) exec "$ROOT/scripts/run_n100_oracle.sh" ;;
  all) "$ROOT/scripts/static_checks.sh"; "$ROOT/scripts/build.sh"; "$ROOT/scripts/run_n10.sh"; exec "$ROOT/scripts/run_n100.sh" ;;
  clean) exec "$ROOT/scripts/clean.sh" ;;
  release) shift; exec "$ROOT/scripts/package_release.sh" "$@" ;;
  *)
    cat <<'EOF'
Usage: ./reproduce.sh COMMAND
  setup       install Ubuntu packages
  verify      verify assets, frozen results, paths, and syntax
  verify-manifest verify the release archive MANIFEST.sha256 when present
  build       build vendored HEXL/SEAL and both isolated executors
  stage-s     run the checkpoint-013 frozen Stage-S contract test
  n10         reproduce the frozen P60 N=10 experiment
  n100        reproduce checkpoint-013 balanced N=100
  oracle-n100 regenerate the frozen N=100 Q16 oracle (requires PyTorch)
  all         verify, build, run N=10, then run N=100
  clean       remove build/ and runs/
  release     create a checked GitHub-ready source archive under dist/
EOF
    ;;
esac
