#!/usr/bin/env python3
# DAZG-Orbit Project Source File
# Component: scripts/verify_results.py
# Purpose: Fail-closed experiment, verification, or Q16 oracle component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
from __future__ import annotations

import csv
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"
errors: list[str] = []


def load_json(name: str) -> dict:
    path = RESULTS / name
    if not path.is_file():
        errors.append(f"missing result summary: {name}")
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        errors.append(f"invalid JSON {name}: {exc}")
        return {}


def load_csv(name: str) -> list[dict[str, str]]:
    path = RESULTS / name
    if not path.is_file():
        errors.append(f"missing result table: {name}")
        return []
    with path.open(newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


n10_ref = load_json("n10_reference.json")
n100_ref = load_json("n100_reference.json")
n10_live = load_json("n10_clean_reproduction.json")
n100_live = load_json("n100_clean_reproduction.json")
validation = load_json("clean_repository_validation.json")

if not (
    n10_ref.get("status") == "PASS"
    and n10_ref.get("strict_exact_count") == 10
    and n10_ref.get("top1_correct") == 10
    and n10_ref.get("top5_correct") == 10
):
    errors.append("N=10 frozen reference summary mismatch")

if not (
    n100_ref.get("status") == "PASS"
    and n100_ref.get("logits_exact_count") == 100
    and n100_ref.get("logits_mismatch_total") == 0
    and n100_ref.get("max_abs_logit_delta") == 0
    and n100_ref.get("top1_correct") == 72
    and n100_ref.get("top5_correct") == 91
):
    errors.append("N=100 frozen reference summary mismatch")

if not (
    n10_live.get("status") == "PASS"
    and n10_live.get("strict_exact_count") == 10
    and n10_live.get("top1_correct") == 10
    and n10_live.get("top5_correct") == 10
):
    errors.append("N=10 clean-reproduction summary mismatch")

if not (
    n100_live.get("status") == "PASS"
    and n100_live.get("logits_exact_count") == 100
    and n100_live.get("logits_mismatch_total") == 0
    and n100_live.get("max_abs_logit_delta") == 0
    and n100_live.get("reference_top5_exact_count") == 100
    and n100_live.get("top1_correct") == 72
    and n100_live.get("top5_correct") == 91
):
    errors.append("N=100 clean-reproduction summary mismatch")

if not (
    validation.get("status") == "PASS"
    and validation.get("offline_dependency_build") == "PASS"
    and validation.get("n1000_executed") is False
    and validation.get("security_claim") == 0
):
    errors.append("clean repository validation summary mismatch")

n10_rows = load_csv("n10_clean_per_sample.csv")
if len(n10_rows) != 10:
    errors.append(f"N=10 clean per-sample row count={len(n10_rows)}")
elif any(
    row.get("strict_exact") != "True"
    or row.get("top1_correct") != "True"
    or row.get("top5_correct") != "True"
    for row in n10_rows
):
    errors.append("N=10 clean per-sample failure present")

for name in ("n100_reference_per_sample.csv", "n100_clean_per_sample.csv"):
    rows = load_csv(name)
    if len(rows) != 100:
        errors.append(f"{name} row count={len(rows)}")
        continue
    if name.startswith("n100_clean") and any(
        row.get("logits_exact") != "True"
        or row.get("reference_top5_exact") != "True"
        for row in rows
    ):
        errors.append("N=100 clean per-sample exactness failure present")

if errors:
    print("[RESULT VERIFY FAIL]")
    print("\n".join(errors))
    sys.exit(2)

print(
    "[RESULT VERIFY PASS] "
    "N10_ref=10/10 N10_clean=10/10 "
    "N100_ref_logits=100/100 N100_clean_logits=100/100 "
    "N100_top1=72/100 N100_top5=91/100"
)
