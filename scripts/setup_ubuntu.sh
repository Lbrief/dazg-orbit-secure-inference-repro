#!/usr/bin/env bash
# DAZG-Orbit Project Source File
# Component: scripts/setup_ubuntu.sh
# Purpose: Build, verification, release, and reproducibility automation.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
set -Eeuo pipefail
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config \
  libssl-dev libgmp-dev \
  python3 python3-venv python3-pip python3-numpy
cat <<'EOF'
[OK] System packages installed.
Optional oracle regeneration also requires CPU PyTorch; see requirements-oracle.txt.
EOF
