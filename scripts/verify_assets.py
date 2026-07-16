#!/usr/bin/env python3
# DAZG-Orbit Project Source File
# Component: scripts/verify_assets.py
# Purpose: Fail-closed experiment, verification, or Q16 oracle component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
errors: list[str] = []


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


# Frozen P60 N=10 manifest and gate.
n10_root = ROOT / "experiments/n10_p60"
n10_manifest = json.loads((n10_root / "PAYLOAD_MANIFEST.json").read_text(encoding="utf-8"))
for row in n10_manifest["payloads"]:
    path = ROOT / row["path"]
    if not path.is_file():
        errors.append(f"N10 missing {row['path']}")
    elif sha256(path) != row["sha256"]:
        errors.append(f"N10 SHA mismatch {row['path']}")

n10_gate = json.loads((n10_root / "P60_FULL_RESULT_GATE.json").read_text(encoding="utf-8"))
if n10_gate.get("status") != "pass":
    errors.append("N10 frozen gate is not pass")

# Checkpoint-013 Q16 payloads.
assets = ROOT / "experiments/n100_checkpoint013/assets"
payload_manifest = json.loads((assets / "payload/payload_manifest.json").read_text(encoding="utf-8"))
entries = [
    row
    for row in payload_manifest["files"]
    if row.get("dtype") == "uint64" or str(row.get("file", "")).endswith(".q16.u64.npy")
]
if len(entries) != 77:
    errors.append(f"N100 Q16 count={len(entries)}")
for row in entries:
    path = assets / "payload" / row["file"]
    if not path.is_file():
        errors.append(f"N100 missing {row['file']}")
    elif sha256(path) != row["sha256"]:
        errors.append(f"N100 SHA mismatch {row['file']}")

# Balanced N=100 reference identity and accuracy.
ref_dir = assets / "reference"
expected_hashes = {
    "qahl_ref_input_n10.npy": "024b8715883ebb2328c88199028f4b7f145f2ab3469e2a43b24a70581d8727bd",
    "qahl_ref_labels_n10.npy": "2ba053670e556154b05e2705f0284b6ce7f71813296eaf684739ea082e362645",
    "checkpoint013_balanced_n100_logits_q16_i64.npy": "ae2cd654ddce520317bcd3b4d0c78c7d3279c81ed46643533617d67ee2665fe7",
    "checkpoint013_balanced_n100_top5_i64.npy": "455c33ee98d1fdc97c99fdee253bf62222bbfca6acc7765bdcd0044d9c602ada",
    "balanced_n100_reference.json": "4dad044b31930810d52d2676895b36010f4798de279ed5d17dc8f9e9bb7f9d2b",
}
for name, expected in expected_hashes.items():
    path = ref_dir / name
    if not path.is_file():
        errors.append(f"N100 reference missing {name}")
    elif sha256(path) != expected:
        errors.append(f"N100 reference SHA mismatch {name}")

inputs = np.load(ref_dir / "qahl_ref_input_n10.npy", allow_pickle=False)
labels = np.load(ref_dir / "qahl_ref_labels_n10.npy", allow_pickle=False).astype(np.int64)
logits = np.load(ref_dir / "checkpoint013_balanced_n100_logits_q16_i64.npy", allow_pickle=False)
top5_file = np.load(ref_dir / "checkpoint013_balanced_n100_top5_i64.npy", allow_pickle=False)
top5 = np.argsort(-logits, axis=1, kind="stable")[:, :5]
top1_correct = int((top5[:, 0] == labels).sum())
top5_correct = sum(int(labels[i] in top5[i]) for i in range(100))
if inputs.shape != (100, 3, 32, 32):
    errors.append(f"N100 input shape={inputs.shape}")
if labels.shape != (100,) or labels.tolist() != list(range(100)):
    errors.append("N100 labels are not class order 0..99")
if logits.shape != (100, 100) or (top1_correct, top5_correct) != (72, 91):
    errors.append(f"N100 oracle contract shape={logits.shape} accuracy={top1_correct}/{top5_correct}")
if not np.array_equal(top5, top5_file):
    errors.append("N100 Top-5 reference file does not match oracle logits")

# Frozen Stage-S table and original checkpoint provenance.
stage_table = assets / "stage_s/stage_s_gelu_q16_i64.bin"
if (
    stage_table.stat().st_size != 8_388_616
    or sha256(stage_table) != "ad6d5faad10f0df1d06f6b0131aec7f6a2f2a1ff4e427456a819b98a930a6f49"
):
    errors.append("Stage-S table contract mismatch")

checkpoint = ROOT / "checkpoints/dazg_orbit_checkpoint013_weights.pt"
if sha256(checkpoint) != "f7db0ff0bae94d806275c23ff46ded8fbdebc8a72cbf193004e915d293c28c3e":
    errors.append("checkpoint 013 weights SHA mismatch")

if errors:
    print("[ASSET VERIFY FAIL]")
    print("\n".join(errors))
    sys.exit(2)

print(
    f"[ASSET VERIFY PASS] n10_payloads={len(n10_manifest['payloads'])} "
    f"n100_q16={len(entries)} n100_top1={top1_correct}/100 n100_top5={top5_correct}/100"
)
