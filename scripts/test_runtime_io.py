#!/usr/bin/env python3
# DAZG-Orbit Project Source File
# Component: scripts/test_runtime_io.py
# Purpose: Verify portable JSON publication and WSL DrvFS recovery behavior.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
from __future__ import annotations

import importlib.util
import json
import os
import tempfile
import sys

sys.dont_write_bytecode = True
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def exercise_atomic(module, label: str) -> None:
    with tempfile.TemporaryDirectory(prefix=f"dazg-{label}-") as td:
        out = Path(td) / "report.json"
        original_replace = os.replace
        original_sleep = module.time.sleep

        def blocked_replace(_src, _dst):
            raise PermissionError("simulated WSL DrvFS/indexer lock")

        try:
            os.replace = blocked_replace
            module.time.sleep = lambda _seconds: None
            module.atomic_json(out, {"status": "success", "value": 27})
        finally:
            os.replace = original_replace
            module.time.sleep = original_sleep

        data = json.loads(out.read_text(encoding="utf-8"))
        if data != {"status": "success", "value": 27}:
            raise RuntimeError(f"{label}: fallback JSON mismatch")
        if list(out.parent.glob("report.json.tmp.*")):
            raise RuntimeError(f"{label}: temporary JSON was not cleaned")


def exercise_recovery(module) -> None:
    with tempfile.TemporaryDirectory(prefix="dazg-recovery-") as td:
        root = Path(td)
        final = root / "pair_report.json"
        tmp = root / "pair_report.json.tmp.13552"
        expected = {
            "status": "success",
            "server_rc": 0,
            "client_rc": 0,
            "sample_index": 27,
        }
        tmp.write_text(json.dumps(expected), encoding="utf-8")
        recovered = module.recover_atomic_json(final)
        if recovered != expected:
            raise RuntimeError("temporary pair report recovery returned wrong data")
        if json.loads(final.read_text(encoding="utf-8")) != expected:
            raise RuntimeError("temporary pair report was not promoted")


def main() -> int:
    pair = load(
        "dazg_pair_runner",
        ROOT / "experiments/n100_checkpoint013/tools/pair_runner.py",
    )
    n100 = load(
        "dazg_run_n100",
        ROOT / "experiments/n100_checkpoint013/tools/run_n100.py",
    )
    exercise_atomic(pair, "pair")
    exercise_atomic(n100, "n100")
    exercise_recovery(n100)
    print("[RUNTIME IO TEST PASS] DrvFS fallback and temporary-report recovery")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
