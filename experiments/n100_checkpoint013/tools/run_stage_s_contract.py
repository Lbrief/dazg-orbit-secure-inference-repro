#!/usr/bin/env python3
# DAZG-Orbit Project Source File
# Component: experiments/n100_checkpoint013/tools/run_stage_s_contract.py
# Purpose: Fail-closed experiment, verification, or Q16 oracle component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
from __future__ import annotations
import argparse, hashlib, json, os, subprocess, sys
from pathlib import Path

EXPECTED_SHA256 = "ad6d5faad10f0df1d06f6b0131aec7f6a2f2a1ff4e427456a819b98a930a6f49"
EXPECTED_BYTES = 8_388_616

def sha256(path: Path) -> str:
    h=hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()

def main() -> int:
    ap=argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--table", required=True)
    ap.add_argument("--out", required=True)
    a=ap.parse_args()
    binary=Path(a.binary).resolve()
    table=Path(a.table).resolve()
    out=Path(a.out).resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        print(f"[ERROR] Stage-S contract binary is not executable: {binary}", file=sys.stderr)
        return 22
    if not table.is_file():
        print(f"[ERROR] frozen Stage-S table is missing: {table}", file=sys.stderr)
        return 22
    if table.stat().st_size != EXPECTED_BYTES:
        print(f"[ERROR] frozen Stage-S table size mismatch: {table.stat().st_size} != {EXPECTED_BYTES}", file=sys.stderr)
        return 22
    actual=sha256(table)
    if actual != EXPECTED_SHA256:
        print(f"[ERROR] frozen Stage-S table SHA mismatch: {actual}", file=sys.stderr)
        return 22
    env=os.environ.copy()
    env["DAZG_ORBIT_STAGE_S_Q16_TABLE"] = str(table)
    print(f"[STAGE-S TABLE] path={table} bytes={EXPECTED_BYTES} sha256={actual}", flush=True)
    proc=subprocess.run([str(binary)], env=env, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True, check=False)
    output=proc.stdout or ""
    print(output, end="" if output.endswith("\n") or not output else "\n", flush=True)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(output, encoding="utf-8")
    if proc.returncode != 0:
        print(f"[ERROR] Stage-S contract process returned {proc.returncode}", file=sys.stderr)
        return 22
    compact="".join(output.split())
    if '"status":"PASS"' not in compact:
        print("[ERROR] Stage-S contract output does not contain PASS", file=sys.stderr)
        return 22
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
