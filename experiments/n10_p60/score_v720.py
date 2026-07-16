#!/usr/bin/env python3
# DAZG-Orbit Project Source File
# Component: experiments/n10_p60/score_v720.py
# Purpose: Fail-closed experiment, verification, or Q16 oracle component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
"""V724 fullgraph DAZG shadow scorer with stage-by-stage exact audits."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

import numpy as np
from numpy.lib.stride_tricks import sliding_window_view

# ===== DAZG_ORBIT_V740_2_SCORE_MODCENTER_EXACT_FIX BEGIN =====
# Exact, integer-only residue/center helpers for the active 50-bit plaintext modulus.
# Floating modulo for this modulus is rejected because it loses low bits.
V740_2_PLAIN_MODULUS = 1125899906826241

def v740_2_residue_u64(values: np.ndarray, p: int) -> np.ndarray:
    arr = np.ascontiguousarray(values)
    pp = int(p)
    if pp <= 0 or pp >= (1 << 63):
        raise ValueError(f"bad plaintext modulus for residue: {p}")
    if np.issubdtype(arr.dtype, np.floating):
        if arr.size and np.nanmax(np.abs(arr)) > (1 << 24):
            raise ValueError(
                "V740.2 refuses float modulo for 50-bit plaintext values; "
                "use signed integer tensors before mod-center"
            )
        arr = np.rint(arr).astype(np.int64)
    if arr.dtype == np.uint64:
        return np.remainder(arr, np.uint64(pp)).astype(np.uint64, copy=False)
    return np.remainder(arr.astype(np.int64, copy=False), np.int64(pp)).astype(np.uint64)

def v740_2_reconstruct_u64(server: np.ndarray, client: np.ndarray, p: int) -> np.ndarray:
    s = v740_2_residue_u64(server, p)
    c = v740_2_residue_u64(client, p)
    return np.remainder(s + c, np.uint64(int(p))).astype(np.uint64, copy=False)

def v740_2_centered_i64(residue: np.ndarray, p: int) -> np.ndarray:
    r = v740_2_residue_u64(residue, p).astype(np.int64, copy=False)
    return np.where(r > int(p) // 2, r - int(p), r).astype(np.int64, copy=False)

def v740_2_remainder_i64(values: np.ndarray, p: int) -> np.ndarray:
    return v740_2_residue_u64(values, p).astype(np.int64, copy=False)

def v740_2_guard_float_modcenter_contract() -> None:
    probe = np.array([-123456789.0], dtype=np.float32)
    try:
        v740_2_residue_u64(probe, V740_2_PLAIN_MODULUS)
    except ValueError:
        return
    raise AssertionError("V740.2 float-modcenter guard did not fire")
# ===== DAZG_ORBIT_V740_2_SCORE_MODCENTER_EXACT_FIX END =====
ENTRY_RE = re.compile(r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]*)"\s*\}')
KV_RE = re.compile(r'([A-Za-z0-9_]+)=([^\s,]+)')
PACK_RE = re.compile(r'PackWeight time\(us\)=([0-9]+)')

V723_DAZG_REFERENCE_MARKER = "DAZG_ORBIT_V723_DAZG_CHANNEL_ORBIT_REFERENCE_20260623_001"
V723_DAZG_CHANNEL_FORMULA = (
    "W[bo*B+i,bi*B+j,r,s]="
    "W[bo*B+((i-j) mod B),bi*B,r,s]"
)

V724_FULLGRAPH_REFERENCE_MARKER = (
    "DAZG_ORBIT_V724_FULLGRAPH_DAZG_SHADOW_REFERENCE_20260623_001"
)
V724_EXECUTOR_MARKER = (
    "DAZG_ORBIT_V724_FULLGRAPH_DAZG_SHADOW_AUDIT_ACTIVE_20260623_001"
)
V724_LINEAR_FORMULA = (
    "W_eff[bi*B+j,bo*B+i]="
    "W_raw[bi*B,bo*B+((i-j) mod B)]"
)
V724_CHANNEL_BLOCKS: dict[str, int] = {
    "stem.0": 1,
    "stem.2.conv": 8,
    "stem.3.net.0.conv": 4,
    "stem.3.net.3.conv": 4,
    "h32.0.body.0": 2,
    "h32.0.body.3.conv": 8,
    "h32.0.anchor.net.0.conv": 4,
    "h32.0.anchor.net.3.conv": 4,
    "h32.1.net.0.conv": 4,
    "h32.1.net.3.conv": 4,
    "to_h16.main.0": 8,
    "to_h16.main.3.conv": 32,
    "to_h16.skip": 16,
    "to_h16.tail.net.0.conv": 8,
    "to_h16.tail.net.3.conv": 8,
    "h16.0.body.0": 8,
    "h16.0.body.3.conv": 32,
    "h16.0.anchor.net.0.conv": 8,
    "h16.0.anchor.net.3.conv": 8,
    "h16.1.net.0.conv": 8,
    "h16.1.net.3.conv": 8,
    "to_h8.main.0": 32,
    "to_h8.main.3.conv": 64,
    "to_h8.skip": 32,
    "to_h8.tail.net.0.conv": 16,
    "to_h8.tail.net.3.conv": 16,
    "h8.0.body.0": 32,
    "h8.0.body.3.conv": 64,
    "h8.0.anchor.net.0.conv": 16,
    "h8.0.anchor.net.3.conv": 16,
    "h8.1.local.0.conv": 16,
    "h8.1.local.1.conv": 16,
    "h8.1.local.2.conv": 16,
    "h8.1.local.3.conv": 16,
    "h8.1.mix.conv": 64,
    "h8.2.net.0.conv": 16,
    "h8.2.net.3.conv": 16,
}


def atomic_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + f".tmp.{os.getpid()}")
    tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")
    os.replace(tmp, path)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def array_sha256(a: np.ndarray) -> str:
    c = np.ascontiguousarray(a)
    h = hashlib.sha256()
    h.update(str(c.dtype).encode())
    h.update(json.dumps(list(c.shape)).encode())
    h.update(c.tobytes(order="C"))
    return h.hexdigest()


def file_sha256(path: Path) -> str | None:
    if not path.is_file():
        return None
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def load_json(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {}
    try:
        value = json.loads(path.read_text())
        return value if isinstance(value, dict) else {}
    except Exception:
        return {}


def parse_inventory(path: Path) -> dict[str, Path]:
    text = path.read_text()
    entries: dict[str, Path] = {}
    for key, payload, _shape in ENTRY_RE.findall(text):
        entries[key] = Path(payload)
    return entries


def load_npy(path: Path) -> np.ndarray:
    if not path.is_file():
        raise FileNotFoundError(path)
    return np.load(path, allow_pickle=False)


def as_raw_i64(a: np.ndarray) -> np.ndarray:
    c = np.ascontiguousarray(a)
    if c.dtype == np.uint64:
        return c.view(np.int64)
    return c.astype(np.int64, copy=False)


def as_residue_u64(a: np.ndarray, p: int) -> np.ndarray:
    return v740_2_residue_u64(a, p)
def reconstruct(server: np.ndarray, client: np.ndarray, p: int) -> np.ndarray:
    if server.shape != client.shape:
        raise ValueError(f"share shape mismatch: {server.shape} vs {client.shape}")
    return v740_2_reconstruct_u64(server, client, p)
def centered(residue: np.ndarray, p: int) -> np.ndarray:
    r = as_residue_u64(residue, p).astype(np.int64)
    return np.where(r > p // 2, r - p, r).astype(np.int64)


def expected_to_residue(expected_i64: np.ndarray, p: int) -> np.ndarray:
    return v740_2_residue_u64(expected_i64, p)
def compare_mod(got_residue: np.ndarray, expected_i64: np.ndarray, p: int) -> dict[str, Any]:
    got = as_residue_u64(got_residue, p)
    expected = expected_to_residue(expected_i64, p)
    if got.shape != expected.shape:
        return {
            "exact": False,
            "shape_match": False,
            "got_shape": list(got.shape),
            "expected_shape": list(expected.shape),
            "neq": max(int(got.size), int(expected.size)),
            "total": int(expected.size),
            "max_abs_delta": None,
            "l1": None,
            "first_mismatch": None,
        }
    d = got.astype(np.int64) - expected.astype(np.int64)
    d = np.where(d > p // 2, d - p, d)
    d = np.where(d < -(p // 2), d + p, d).astype(np.int64)
    mask = d != 0
    neq = int(np.count_nonzero(mask))
    if neq:
        flat = np.flatnonzero(mask)
        idx = int(flat[0])
        first = {
            "flat_index": idx,
            "got_residue": int(got.reshape(-1)[idx]),
            "expected_residue": int(expected.reshape(-1)[idx]),
            "centered_delta": int(d.reshape(-1)[idx]),
        }
        max_abs = max(abs(int(x)) for x in d[mask].reshape(-1))
        l1 = sum(abs(int(x)) for x in d[mask].reshape(-1))
    else:
        first = None
        max_abs = 0
        l1 = 0
    return {
        "exact": neq == 0,
        "shape_match": True,
        "got_shape": list(got.shape),
        "expected_shape": list(expected.shape),
        "neq": neq,
        "total": int(expected.size),
        "max_abs_delta": int(max_abs),
        "l1": int(l1),
        "first_mismatch": first,
        "got_sha256": array_sha256(got),
        "expected_residue_sha256": array_sha256(expected),
    }


def conv_q16_postaccum_floor(
    x_i64: np.ndarray,
    w_i64: np.ndarray,
    b_i64: np.ndarray,
    stride: int,
    padding: int,
) -> np.ndarray:
    if x_i64.ndim != 3 or w_i64.ndim != 4 or b_i64.ndim != 1:
        raise ValueError(
            f"bad conv rank x={x_i64.shape} w={w_i64.shape} b={b_i64.shape}"
        )
    co, ci, kh, kw = w_i64.shape
    if x_i64.shape[0] != ci or b_i64.shape[0] != co:
        raise ValueError(
            f"bad conv channels x={x_i64.shape} w={w_i64.shape} b={b_i64.shape}"
        )
    xpad = np.pad(
        x_i64.astype(np.int64, copy=False),
        ((0, 0), (padding, padding), (padding, padding)),
        mode="constant",
    )
    windows = sliding_window_view(xpad, (kh, kw), axis=(1, 2))
    windows = windows[:, ::stride, ::stride, :, :]
    oh, ow = windows.shape[1], windows.shape[2]
    out_q32 = np.empty((co, oh, ow), dtype=np.int64)
    for o in range(co):
        if o % 32 == 0:
            print(f"[score-conv] output_channel={o}/{co}", flush=True)
        out_q32[o] = np.einsum(
            "cab,cijab->ij",
            w_i64[o].astype(np.int64, copy=False),
            windows,
            dtype=np.int64,
            optimize=True,
        )
    out_q32 += b_i64.astype(np.int64)[:, None, None] * np.int64(65536)
    return np.floor_divide(out_q32, np.int64(65536)).astype(np.int64)



def expand_dazg_block_circulant_weight(
    w_i64: np.ndarray,
    block_size: int,
) -> np.ndarray:
    """Expand CirConv's learned first-column generators into its effective W."""
    w = np.ascontiguousarray(w_i64).astype(np.int64, copy=False)
    if w.ndim != 4:
        raise ValueError(f"DAZG weight must be rank-4, got {w.shape}")
    co, ci, _kh, _kw = w.shape
    b = int(block_size)
    if b <= 0 or co % b != 0 or ci % b != 0:
        raise ValueError(
            f"bad DAZG channel block: weight={w.shape} block_size={b}"
        )

    effective = np.empty_like(w)
    in_offsets = np.arange(b, dtype=np.int64)
    for out_base in range(0, co, b):
        for in_base in range(0, ci, b):
            # CirConv packs only the first input-column generator of each block.
            generator = w[out_base:out_base + b, in_base, :, :]
            for out_off in range(b):
                generator_index = np.mod(out_off - in_offsets, b)
                effective[
                    out_base + out_off,
                    in_base:in_base + b,
                    :,
                    :,
                ] = generator[generator_index, :, :]
    return effective


def dazg_payload_contract(
    raw_weight: np.ndarray,
    effective_weight: np.ndarray,
    block_size: int,
) -> dict[str, Any]:
    raw = np.ascontiguousarray(raw_weight).astype(np.int64, copy=False)
    effective = np.ascontiguousarray(effective_weight).astype(np.int64, copy=False)
    if raw.shape != effective.shape:
        raise ValueError(f"weight audit shape mismatch: {raw.shape} vs {effective.shape}")
    mismatch = raw != effective
    neq = int(np.count_nonzero(mismatch))
    return {
        "reference_marker": V723_DAZG_REFERENCE_MARKER,
        "reference_semantics": "dazg_block_circulant_first_column",
        "channel_formula": V723_DAZG_CHANNEL_FORMULA,
        "block_size": int(block_size),
        "raw_payload_shape": list(raw.shape),
        "raw_payload_sha256": array_sha256(raw),
        "effective_weight_sha256": array_sha256(effective),
        "payload_already_fully_expanded": neq == 0,
        "payload_vs_effective_neq": neq,
        "payload_vs_effective_total": int(raw.size),
    }


def manual_dazg_conv_q16_postaccum_floor(
    x_i64: np.ndarray,
    w_i64: np.ndarray,
    b_i64: np.ndarray,
    block_size: int,
    stride: int,
    padding: int,
) -> np.ndarray:
    """Small independent loop oracle used only by scorer self-test."""
    x = np.ascontiguousarray(x_i64).astype(np.int64, copy=False)
    w = np.ascontiguousarray(w_i64).astype(np.int64, copy=False)
    bias = np.ascontiguousarray(b_i64).astype(np.int64, copy=False)
    co, ci, kh, kw = w.shape
    b = int(block_size)
    if co % b != 0 or ci % b != 0:
        raise ValueError("manual DAZG oracle requires complete channel blocks")
    xpad = np.pad(x, ((0, 0), (padding, padding), (padding, padding)))
    oh = (x.shape[1] + 2 * padding - kh) // stride + 1
    ow = (x.shape[2] + 2 * padding - kw) // stride + 1
    out = np.empty((co, oh, ow), dtype=np.int64)
    for out_ch in range(co):
        out_base = (out_ch // b) * b
        out_off = out_ch % b
        for y in range(oh):
            for z in range(ow):
                acc = 0
                for in_ch in range(ci):
                    in_base = (in_ch // b) * b
                    in_off = in_ch % b
                    weight_ch = out_base + ((out_off - in_off) % b)
                    for r in range(kh):
                        for s in range(kw):
                            acc += int(xpad[in_ch, y * stride + r, z * stride + s]) * int(
                                w[weight_ch, in_base, r, s]
                            )
                out[out_ch, y, z] = acc // 65536 + int(bias[out_ch])
    return out


def parse_keyvals(line: str) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for key, raw in KV_RE.findall(line):
        if re.fullmatch(r"-?[0-9]+", raw):
            out[key] = int(raw)
        elif re.fullmatch(r"-?[0-9]+\.[0-9]+", raw):
            out[key] = float(raw)
        else:
            out[key] = raw
    return out


def parse_runtime(log_path: Path) -> dict[str, Any]:
    text = log_path.read_text(errors="replace") if log_path.is_file() else ""
    runtime: list[dict[str, Any]] = []
    routes: list[dict[str, Any]] = []
    pack_weight_us: list[int] = []
    for line in text.splitlines():
        if "[DAZG_ORBIT_RUNTIME]" in line:
            runtime.append(parse_keyvals(line))
        if "[DAZG_ORBIT_V719_TO_H8_ROUTE]" in line:
            routes.append(parse_keyvals(line))
        m = PACK_RE.search(line)
        if m:
            pack_weight_us.append(int(m.group(1)))

    def total(field: str) -> int:
        return sum(int(r.get(field, 0)) for r in runtime)

    layouts: dict[str, int] = {}
    schedules: dict[str, int] = {}
    for r in runtime:
        layout = str(r.get("layout_mode", "unknown"))
        layouts[layout] = layouts.get(layout, 0) + 1
        schedule = str(r.get("schedule", "unknown"))
        schedules[schedule] = schedules.get(schedule, 0) + 1

    main0_runtime = next(
        (
            r for r in reversed(runtime)
            if r.get("H") == 16 and r.get("Cin") == 96 and
            r.get("Cout") == 192 and r.get("K") == 3 and r.get("S") == 2
        ),
        None,
    )
    skip_runtime = next(
        (
            r for r in reversed(runtime)
            if r.get("H") == 16 and r.get("Cin") == 96 and
            r.get("Cout") == 192 and r.get("K") == 1 and r.get("S") == 2
        ),
        None,
    )
    target_route = next(
        (
            r for r in reversed(routes)
            if r.get("H") == 16 and r.get("Cin") == 96 and
            r.get("Cout") == 192 and r.get("K") == 3 and r.get("S") == 2
        ),
        None,
    )
    return {
        "runtime_record_count": len(runtime),
        "hecompute_us": total("hecompute_us"),
        "mul_plain": total("mul_plain"),
        "rotate_rows": total("rotate_rows"),
        "add_inplace": total("add_inplace"),
        "pack_weight_us": sum(pack_weight_us),
        "pack_weight_record_count": len(pack_weight_us),
        "layout_histogram": layouts,
        "schedule_histogram": schedules,
        "to_h8_main0_runtime": main0_runtime,
        "to_h8_skip_runtime": skip_runtime,
        "to_h8_route": target_route,
        "server_log_sha256": file_sha256(log_path),
    }


def _int_or_none(value: Any) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, np.integer)):
        return int(value)
    return None


def report_contract(pair_dir: Path, pair_report: dict[str, Any]) -> dict[str, Any]:
    server = load_json(pair_dir / "server_report.json")
    client = load_json(pair_dir / "client_report.json")
    server_bytes = _int_or_none(server.get("communication_bytes_sent"))
    client_bytes = _int_or_none(client.get("communication_bytes_sent"))
    server_rounds = _int_or_none(server.get("network_rounds_observed"))
    client_rounds = _int_or_none(client.get("network_rounds_observed"))
    server_fallback = _int_or_none(server.get("algorithm_fallback_count"))
    client_fallback = _int_or_none(client.get("algorithm_fallback_count"))
    communication_total = (
        server_bytes + client_bytes
        if server_bytes is not None and client_bytes is not None
        else None
    )
    rounds_per_pair = (
        max(server_rounds, client_rounds)
        if server_rounds is not None and client_rounds is not None
        else None
    )
    fallback_total = (
        server_fallback + client_fallback
        if server_fallback is not None and client_fallback is not None
        else None
    )
    return {
        "pair_status": pair_report.get("status"),
        "pair_server_rc": pair_report.get("server_rc"),
        "pair_client_rc": pair_report.get("client_rc"),
        "server_status": server.get("status"),
        "client_status": client.get("status"),
        "server_party": server.get("party"),
        "client_party": client.get("party"),
        "server_party_contract_ok": server.get("party_contract_ok"),
        "client_party_contract_ok": client.get("party_contract_ok"),
        "server_input_owner_ok": server.get("input_owner_ok"),
        "client_input_owner_ok": client.get("input_owner_ok"),
        "server_output_ready": server.get("output_ready"),
        "client_output_ready": client.get("output_ready"),
        "server_netio_ok": server.get("netio_ok"),
        "client_netio_ok": client.get("netio_ok"),
        "server_context_ok": server.get("context_ok"),
        "client_context_ok": client.get("context_ok"),
        "server_keygen_ok": server.get("keygen_ok"),
        "client_keygen_ok": client.get("keygen_ok"),
        "server_input_ok": server.get("input_ok"),
        "client_input_ok": client.get("input_ok"),
        "server_source_marker": server.get("source_marker"),
        "client_source_marker": client.get("source_marker"),
        "server_bias_closure_marker": server.get("bias_closure_marker"),
        "client_bias_closure_marker": client.get("bias_closure_marker"),
        "server_fullgraph_shadow_marker": server.get("fullgraph_shadow_marker"),
        "client_fullgraph_shadow_marker": client.get("fullgraph_shadow_marker"),
        "server_v724_stage_dump_contract": server.get("v724_stage_dump_contract"),
        "client_v724_stage_dump_contract": client.get("v724_stage_dump_contract"),
        "server_v724_stage_dump_enabled": server.get("v724_stage_dump_enabled"),
        "client_v724_stage_dump_enabled": client.get("v724_stage_dump_enabled"),
        "server_v724_stage_dump_count": _int_or_none(server.get("v724_stage_dump_count")),
        "client_v724_stage_dump_count": _int_or_none(client.get("v724_stage_dump_count")),
        "server_operator_bias_zeroed": server.get("v721_operator_bias_zeroed"),
        "client_operator_bias_zeroed": client.get("v721_operator_bias_zeroed"),
        "server_postbridge_q16_bias_once": server.get("v721_postbridge_q16_bias_once"),
        "client_postbridge_q16_bias_once": client.get("v721_postbridge_q16_bias_once"),
        "server_bias_closure_calls": _int_or_none(server.get("v721_bias_closure_calls")),
        "client_bias_closure_calls": _int_or_none(client.get("v721_bias_closure_calls")),
        "server_bias_closure_applied_calls": _int_or_none(server.get("v721_bias_closure_applied_calls")),
        "client_bias_closure_applied_calls": _int_or_none(client.get("v721_bias_closure_applied_calls")),
        "server_bias_closure_applied_elements": _int_or_none(server.get("v721_bias_closure_applied_elements")),
        "client_bias_closure_applied_elements": _int_or_none(client.get("v721_bias_closure_applied_elements")),
        "server_route_marker": server.get("route_marker"),
        "client_route_marker": client.get("route_marker"),
        "server_route_candidate": server.get("route_candidate"),
        "client_route_candidate": client.get("route_candidate"),
        "pair_route_candidate": pair_report.get("candidate"),
        "server_plain_mod": server.get("plain_mod"),
        "client_plain_mod": client.get("plain_mod"),
        "plain_mod": server.get("plain_mod") or client.get("plain_mod"),
        "diagnostic_reveal": bool(
            server.get("diagnostic_reveal") or client.get("diagnostic_reveal")
        ),
        "server_diagnostic_reveal_count": server.get("diagnostic_reveal_count"),
        "client_diagnostic_reveal_count": client.get("diagnostic_reveal_count"),
        # DAZG_ORBIT_V743R8P80_SCORE_BRIDGE_BLOCKERS_BEGIN
        "server_p73_q32_pair_carry_exchange_count": _int_or_none(server.get("v743r8p73_q32_pair_carry_exchange_count")),
        "client_p73_q32_pair_carry_exchange_count": _int_or_none(client.get("v743r8p73_q32_pair_carry_exchange_count")),
        "server_p74_nonlinear_pair_bridge_count": _int_or_none(server.get("v743r8p74_nonlinear_pair_bridge_count")),
        "client_p74_nonlinear_pair_bridge_count": _int_or_none(client.get("v743r8p74_nonlinear_pair_bridge_count")),
        "server_p77_q32_low16_carry_exchange_count": _int_or_none(server.get("v743r8p77_q32_low16_carry_exchange_count")),
        "client_p77_q32_low16_carry_exchange_count": _int_or_none(client.get("v743r8p77_q32_low16_carry_exchange_count")),
        "server_p80_q32_wrap_exchange_count": _int_or_none(server.get("v743r8p80_q32_wrap_exchange_count")),
        "client_p80_q32_wrap_exchange_count": _int_or_none(client.get("v743r8p80_q32_wrap_exchange_count")),
        # DAZG_ORBIT_V743R8P80_SCORE_BRIDGE_BLOCKERS_END
        "server_algorithm_fallback_count": server_fallback,
        "client_algorithm_fallback_count": client_fallback,
        "algorithm_fallback_count": fallback_total,
        "server_communication_bytes_sent": server_bytes,
        "client_communication_bytes_sent": client_bytes,
        "communication_bytes_total": communication_total,
        "server_network_rounds_observed": server_rounds,
        "client_network_rounds_observed": client_rounds,
        "network_rounds_per_pair": rounds_per_pair,
        "server_wall_ms": server.get("wall_ms"),
        "client_wall_ms": client.get("wall_ms"),
        "pair_wall_s": pair_report.get("wall_s"),
    }


def protocol_gate(contract: dict[str, Any]) -> dict[str, Any]:
    markers = [
        contract.get("server_source_marker"),
        contract.get("client_source_marker"),
        contract.get("server_route_marker"),
        contract.get("client_route_marker"),
    ]
    marker_strings = [x for x in markers if isinstance(x, str) and x]
    server_candidate = contract.get("server_route_candidate")
    client_candidate = contract.get("client_route_candidate")
    pair_candidate = contract.get("pair_route_candidate")
    checks = {
        "pair_success": (
            contract.get("pair_status") == "success" and
            contract.get("pair_server_rc") == 0 and
            contract.get("pair_client_rc") == 0
        ),
        "executor_status_ready": (
            contract.get("server_status") not in (None, "blocked") and
            contract.get("client_status") not in (None, "blocked")
        ),
        "party_contract": (
            contract.get("server_party") == 1 and
            contract.get("client_party") == 2 and
            contract.get("server_party_contract_ok") is True and
            contract.get("client_party_contract_ok") is True
        ),
        "server_owned_input_contract": (
            contract.get("server_input_owner_ok") is True and
            contract.get("client_input_owner_ok") is True
        ),
        "executor_output_ready": (
            contract.get("server_output_ready") is True and
            contract.get("client_output_ready") is True
        ),
        "he_context_ready": all(
            contract.get(key) is True
            for key in (
                "server_netio_ok", "client_netio_ok",
                "server_context_ok", "client_context_ok",
                "server_keygen_ok", "client_keygen_ok",
                "server_input_ok", "client_input_ok",
            )
        ),
        "fresh_markers_consistent": (
            len(marker_strings) == 4 and
            len(set(marker_strings)) == 1 and
            marker_strings[0].startswith("DAZG_ORBIT_V720_FRESH_")
        ),
        "v724_fullgraph_shadow_marker": (
            contract.get("server_fullgraph_shadow_marker") == V724_EXECUTOR_MARKER and
            contract.get("client_fullgraph_shadow_marker") == V724_EXECUTOR_MARKER and
            contract.get("server_v724_stage_dump_contract") ==
                "input_to_logits_dazg_shadow_stages" and
            contract.get("client_v724_stage_dump_contract") ==
                "input_to_logits_dazg_shadow_stages"
        ),
        "v721_bias_closure_contract": (
            isinstance(contract.get("server_bias_closure_marker"), str) and
            contract.get("server_bias_closure_marker") ==
                contract.get("client_bias_closure_marker") and
            contract.get("server_bias_closure_marker", "").startswith(
                "DAZG_ORBIT_V721_BIAS_CLOSURE_ACTIVE_") and
            contract.get("server_operator_bias_zeroed") is True and
            contract.get("client_operator_bias_zeroed") is True and
            contract.get("server_postbridge_q16_bias_once") is True and
            contract.get("client_postbridge_q16_bias_once") is True and
            isinstance(contract.get("server_bias_closure_calls"), int) and
            contract.get("server_bias_closure_calls") >= 1 and
            contract.get("server_bias_closure_calls") ==
                contract.get("client_bias_closure_calls") and
            isinstance(contract.get("server_bias_closure_applied_calls"), int) and
            contract.get("server_bias_closure_applied_calls") >= 1 and
            contract.get("client_bias_closure_applied_calls") == 0 and
            isinstance(contract.get("server_bias_closure_applied_elements"), int) and
            contract.get("server_bias_closure_applied_elements") >= 1 and
            contract.get("client_bias_closure_applied_elements") == 0
        ),
        "route_candidate_consistent": (
            isinstance(server_candidate, str) and bool(server_candidate) and
            server_candidate == client_candidate == pair_candidate
        ),
        "plain_mod_consistent": (
            isinstance(contract.get("server_plain_mod"), int) and
            contract.get("server_plain_mod") == contract.get("client_plain_mod") and
            int(contract.get("server_plain_mod")) > 0
        ),
        "algorithm_fallback_zero": (
            contract.get("server_algorithm_fallback_count") == 0 and
            contract.get("client_algorithm_fallback_count") == 0
        ),
        "cost_counters_present": (
            contract.get("communication_bytes_total") is not None and
            contract.get("network_rounds_per_pair") is not None
        ),
    }
    failures = [name for name, ok in checks.items() if not ok]
    return {
        "ok": not failures,
        "checks": checks,
        "failures": failures,
    }


def reference_safety(
    pair_dir: Path, entries: dict[str, Path], kind: str
) -> dict[str, Any]:
    input_path = required_payload(entries, "qahl_ref_input_n10.npy")
    reference_path = required_payload(entries, "qahl_ref_logits_n10.npy")
    if kind == "firstconv":
        share_paths = [
            pair_dir / "server_tensor.npy",
            pair_dir / "client_tensor.npy",
        ]
    elif kind == "to_h8":
        share_paths = [
            pair_dir / "server_dump" / "v720_before_to_h8_transition.npy",
            pair_dir / "client_dump" / "v720_before_to_h8_transition.npy",
            pair_dir / "server_dump" / "v720_to_h8_main0.npy",
            pair_dir / "client_dump" / "v720_to_h8_main0.npy",
        ]
    elif kind == "full":
        share_paths = [
            pair_dir / "server_logits.npy",
            pair_dir / "client_logits.npy",
        ]
    else:
        raise ValueError(kind)

    input_hash = file_sha256(input_path)
    reference_hash = file_sha256(reference_path)
    share_hashes = {str(path): file_sha256(path) for path in share_paths}
    checks = {
        "input_reference_paths_differ": input_path.resolve() != reference_path.resolve(),
        "input_reference_hashes_differ": (
            input_hash is not None and reference_hash is not None and
            input_hash != reference_hash
        ),
        "all_expected_share_files_present": all(path.is_file() for path in share_paths),
        "share_paths_do_not_point_to_reference": all(
            path.resolve() != reference_path.resolve() for path in share_paths
        ),
        "share_files_are_not_reference_file_copies": all(
            digest is not None and digest != reference_hash
            for digest in share_hashes.values()
        ),
    }
    failures = [name for name, ok in checks.items() if not ok]
    return {
        "reference_echo_safe": not failures,
        "checks": checks,
        "failures": failures,
        "input_path": str(input_path),
        "reference_path": str(reference_path),
        "input_file_sha256": input_hash,
        "reference_file_sha256": reference_hash,
        "share_files": [str(path) for path in share_paths],
        "share_file_sha256": share_hashes,
    }


def required_payload(entries: dict[str, Path], key: str) -> Path:
    path = entries.get(key)
    if path is None:
        raise KeyError(f"inventory missing {key}")
    if not path.is_file():
        raise FileNotFoundError(f"payload missing for {key}: {path}")
    return path


def score_firstconv(
    pair_dir: Path, entries: dict[str, Path], sample: int, p: int
) -> dict[str, Any]:
    print("[score] reconstructing firstconv shares", flush=True)
    got = reconstruct(
        load_npy(pair_dir / "server_tensor.npy"),
        load_npy(pair_dir / "client_tensor.npy"),
        p,
    )
    x_all = as_raw_i64(load_npy(required_payload(entries, "qahl_ref_input_n10.npy")))
    if x_all.ndim != 4 or sample >= x_all.shape[0]:
        raise ValueError(f"bad input shape/sample: {x_all.shape}, sample={sample}")
    x = x_all[sample]
    w = as_raw_i64(load_npy(required_payload(entries, "stem.0.weight")))
    b = as_raw_i64(load_npy(required_payload(entries, "stem.0.bias")))
    print("[score] computing independent clear firstconv reference", flush=True)
    expected = conv_q16_postaccum_floor(x, w, b, stride=1, padding=1)
    metric = compare_mod(got, expected, p)
    return {
        "kind": "firstconv",
        "strict_exact": bool(metric["exact"]),
        "metric": metric,
        "input_sha256": array_sha256(x_all[sample]),
        "weight_sha256": array_sha256(w),
        "bias_sha256": array_sha256(b),
        "reconstructed_sha256": array_sha256(got),
        "expected_signed_sha256": array_sha256(expected),
        "expected_shape": list(expected.shape),
    }


def score_to_h8(
    pair_dir: Path, entries: dict[str, Path], p: int, runtime: dict[str, Any]
) -> dict[str, Any]:
    print("[score] reconstructing true pre-H8 and to_h8.main0 dumps", flush=True)
    pre = reconstruct(
        load_npy(pair_dir / "server_dump" / "v720_before_to_h8_transition.npy"),
        load_npy(pair_dir / "client_dump" / "v720_before_to_h8_transition.npy"),
        p,
    )
    got = reconstruct(
        load_npy(pair_dir / "server_dump" / "v720_to_h8_main0.npy"),
        load_npy(pair_dir / "client_dump" / "v720_to_h8_main0.npy"),
        p,
    )
    pre_signed = centered(pre, p)
    w = as_raw_i64(load_npy(required_payload(entries, "to_h8.main.0.weight")))
    b = as_raw_i64(load_npy(required_payload(entries, "to_h8.main.0.bias")))
    route = runtime.get("to_h8_route") or {}
    block_size = int(route.get("block_size") or 0)
    block_lock_ok = (
        block_size == 32 and
        list(pre.shape) == [96, 16, 16] and
        list(got.shape) == [192, 8, 8] and
        list(w.shape) == [192, 96, 3, 3]
    )
    if not block_lock_ok:
        raise RuntimeError(
            "V723 DAZG contract requires the PDF-locked to_h8 tuple "
            f"H16/Cin96/Cout192/K3/S2/P1 with channel block 32; route={route}"
        )

    print(
        "[score] computing dense audit reference (not the DAZG model contract)",
        flush=True,
    )
    expected_dense = conv_q16_postaccum_floor(
        pre_signed, w, b, stride=2, padding=1
    )
    dense_metric = compare_mod(got, expected_dense, p)

    print(
        "[score] computing independent DAZG block-circulant channel-orbit reference",
        flush=True,
    )
    effective_w = expand_dazg_block_circulant_weight(w, block_size)
    expected = conv_q16_postaccum_floor(
        pre_signed, effective_w, b, stride=2, padding=1
    )
    metric = compare_mod(got, expected, p)
    payload_contract = dazg_payload_contract(w, effective_w, block_size)

    route_active = (
        route.get("layout_mode") == 7 and
        route.get("pcoi_k3s2") == 1 and
        route.get("capacity_ok") == 1
    )
    # ===== DAZG_ORBIT_V741_DAZG_PAYLOAD_CONTRACT_GUARD BEGIN =====
    # Arithmetic exactness against a DAZG first-column projection is not enough
    # for paper-grade accuracy.  The trained payload must itself satisfy the
    # DAZG orbit, or the executor must evaluate the dense weights.  Otherwise
    # the chain can be perfectly exact while predicting the wrong labels.
    allow_projected = os.getenv(
        "DAZG_ORBIT_V741_ALLOW_PROJECTED_DAZG", "0"
    ).lower() in ("1", "true", "yes", "on")
    payload_contract_ok = bool(
        payload_contract.get("payload_already_fully_expanded") or allow_projected
    )
    gate_failures = []
    if not payload_contract_ok:
        gate_failures.append(
            "dazg_payload_not_trained_or_exported_for_block_cyclic_contract"
        )
    if not route_active:
        gate_failures.append("to_h8_pcoi_route_not_active")
    if not block_lock_ok:
        gate_failures.append("to_h8_pdf_locked_tuple_mismatch")
    exact_contracts = (
        [
            "to_h8.main0_raw_payload_satisfies_dazg_channel_orbit",
            "to_h8.main0_k3s2_pcoi_spatial_orbit",
        ]
        if metric["exact"] and route_active and block_lock_ok and payload_contract_ok
        else []
    )
    # ===== DAZG_ORBIT_V741_DAZG_PAYLOAD_CONTRACT_GUARD END =====
    return {
        "kind": "to_h8",
        "reference_marker": V723_DAZG_REFERENCE_MARKER,
        "reference_semantics": "dazg_block_circulant_first_column",
        "pre_h8_shape": list(pre.shape),
        "main0_shape": list(got.shape),
        "expected_shape": list(expected.shape),
        "pre_h8_contract_ok": list(pre.shape) == [96, 16, 16],
        "main0_contract_ok": list(got.shape) == [192, 8, 8],
        "block_lock_ok": block_lock_ok,
        "strict_exact": bool(metric["exact"] and exact_contracts),
        "numeric_exact": bool(metric["exact"] and block_lock_ok),
        "exact_contracts": exact_contracts,
        "route_active": route_active,
        "pcoi_route_active": route_active,
        "p24_route_contract_key_bridge": True,
        # DAZG_ORBIT_V741_DAZG_PAYLOAD_CONTRACT_GUARD_BEGIN
        "paper_grade_payload_contract_ok": payload_contract_ok,
        "projected_dazg_allowed_by_env": allow_projected,
        "gate_failures": gate_failures,
        "metric": metric,
        "dense_audit_metric": dense_metric,
        "dazg_channel_orbit": payload_contract,
        # DAZG_ORBIT_V741_DAZG_PAYLOAD_CONTRACT_GUARD_END
        "pre_h8_sha256": array_sha256(pre),
        "main0_reconstructed_sha256": array_sha256(got),
        "expected_signed_sha256": array_sha256(expected),
        "dense_expected_signed_sha256": array_sha256(expected_dense),
        "packing_layout": {
            "main0_route": runtime.get("to_h8_route"),
            "main0_runtime": runtime.get("to_h8_main0_runtime"),
            "skip_runtime": runtime.get("to_h8_skip_runtime"),
        },
    }



def v724_safe_tag(tag: str) -> str:
    return re.sub(r"[^A-Za-z0-9_-]", "_", tag)


def v724_canonical_signed(values: np.ndarray, p: int) -> np.ndarray:
    return centered(expected_to_residue(np.asarray(values, dtype=np.int64), p), p)


def v724_modular_add_signed(a: np.ndarray, b: np.ndarray, p: int) -> np.ndarray:
    aa = np.asarray(a, dtype=np.int64)
    bb = np.asarray(b, dtype=np.int64)
    if aa.shape != bb.shape:
        raise ValueError(f"residual shape mismatch: {aa.shape} vs {bb.shape}")
    residue = v740_2_reconstruct_u64(aa, bb, p)
    return centered(residue, p)
def v724_modular_matmul_residue(
    left_residue: np.ndarray,
    right_residue: np.ndarray,
    p: int,
) -> np.ndarray:
    """Exact (left @ right) mod p without int64 product overflow.

    p is below 2^50 in the active HE context.  Base-2^13 limbs make each
    limb dot product and every Horner multiplication fit signed int64.
    This path is used only when a conservative bound says the fast int64
    dot product could overflow.
    """
    left = np.asarray(left_residue, dtype=np.int64)
    right = np.asarray(right_residue, dtype=np.int64)
    if left.ndim != 2 or right.ndim != 2 or left.shape[1] != right.shape[0]:
        raise ValueError(f"bad modular matmul shapes: {left.shape} @ {right.shape}")
    if p <= 0 or p >= (1 << 50):
        raise ValueError(f"V724 modular matmul requires 0 < p < 2^50, got {p}")

    base_bits = 13
    base = 1 << base_bits
    mask = base - 1
    limb_count = (int(p).bit_length() + base_bits - 1) // base_bits
    left_limbs = [((left >> (i * base_bits)) & mask).astype(np.int64, copy=False)
                  for i in range(limb_count)]
    right_limbs = [((right >> (i * base_bits)) & mask).astype(np.int64, copy=False)
                   for i in range(limb_count)]

    degree_coefficients: list[np.ndarray] = []
    for degree in range(2 * limb_count - 1):
        coefficient = None
        for i in range(limb_count):
            j = degree - i
            if j < 0 or j >= limb_count:
                continue
            term = left_limbs[i] @ right_limbs[j]
            coefficient = term if coefficient is None else coefficient + term
        if coefficient is None:
            coefficient = np.zeros((left.shape[0], right.shape[1]), dtype=np.int64)
        degree_coefficients.append(coefficient)

    result = np.zeros_like(degree_coefficients[-1], dtype=np.int64)
    for coefficient in reversed(degree_coefficients):
        # (p - 1) * 2^13 still fits int64 for the active p.  Reduce before add.
        result = np.remainder(result * np.int64(base), np.int64(p))
        result = np.remainder(result + np.remainder(coefficient, np.int64(p)), np.int64(p))
    return result


def v724_exact_dot_residue(
    left_signed: np.ndarray,
    right_signed: np.ndarray,
    p: int,
    label: str,
) -> tuple[np.ndarray, str, int]:
    """Return exact modular dot, selecting a fast safe path by bound."""
    left = np.asarray(left_signed, dtype=np.int64)
    right = np.asarray(right_signed, dtype=np.int64)
    if left.ndim != 2 or right.ndim != 2 or left.shape[1] != right.shape[0]:
        raise ValueError(f"bad dot shapes for {label}: {left.shape} @ {right.shape}")
    k = int(left.shape[1])
    max_left = max((abs(int(left.min(initial=0))), abs(int(left.max(initial=0)))))
    max_right = max((abs(int(right.min(initial=0))), abs(int(right.max(initial=0)))))
    conservative_bound = int(k) * int(max_left) * int(max_right)
    if conservative_bound <= np.iinfo(np.int64).max:
        raw = left @ right
        residue = v740_2_remainder_i64(raw, p)
        mode = "int64_exact_then_modp"
    else:
        left_residue = v740_2_remainder_i64(left, p)
        right_residue = v740_2_remainder_i64(right, p)
        residue = v724_modular_matmul_residue(left_residue, right_residue, p)
        mode = "base8192_limb_modp"
    print(
        f"[v724-modp-dot] label={label} mode={mode} k={k} "
        f"bound={conservative_bound} p={p}",
        flush=True,
    )
    return residue, mode, conservative_bound


def v724_q32_residue_to_q16_signed(
    q32_residue: np.ndarray,
    p: int,
) -> np.ndarray:
    q32_signed = centered(np.asarray(q32_residue, dtype=np.int64), p)
    return np.floor_divide(q32_signed, np.int64(65536)).astype(np.int64)


def v724_conv_q16_postaccum_floor_fast(
    x_i64: np.ndarray,
    w_i64: np.ndarray,
    b_i64: np.ndarray,
    stride: int,
    padding: int,
    layer: str,
    p: int,
) -> np.ndarray:
    """Match V721: HE sum mod p -> centered Q32 floor -> Q16 bias once."""
    x = np.asarray(x_i64, dtype=np.int64)
    w = np.asarray(w_i64, dtype=np.int64)
    b = np.asarray(b_i64, dtype=np.int64)
    if x.ndim != 3 or w.ndim != 4 or b.ndim != 1:
        raise ValueError(f"bad conv rank layer={layer} x={x.shape} w={w.shape} b={b.shape}")
    co, ci, kh, kw = w.shape
    if x.shape[0] != ci or b.shape[0] != co:
        raise ValueError(f"bad conv channels layer={layer} x={x.shape} w={w.shape} b={b.shape}")
    xpad = np.pad(x, ((0, 0), (padding, padding), (padding, padding)), mode="constant")
    windows = sliding_window_view(xpad, (kh, kw), axis=(1, 2))
    windows = windows[:, ::stride, ::stride, :, :]
    oh, ow = windows.shape[1], windows.shape[2]
    right = np.transpose(windows, (0, 3, 4, 1, 2)).reshape(ci * kh * kw, oh * ow)
    out = np.empty((co, oh * ow), dtype=np.int64)
    chunk = 32
    for start in range(0, co, chunk):
        stop = min(co, start + chunk)
        print(
            f"[v724-shadow-conv] layer={layer} outputs={start}:{stop}/{co} "
            f"input={list(x.shape)} kernel={kh} stride={stride} padding={padding}",
            flush=True,
        )
        left = w[start:stop].reshape(stop - start, ci * kh * kw)
        q32_residue, _mode, _bound = v724_exact_dot_residue(
            left, right, p, f"conv:{layer}:{start}:{stop}"
        )
        q16_weight_only = v724_q32_residue_to_q16_signed(q32_residue, p)
        out[start:stop] = q16_weight_only + b[start:stop, None]
    return out.reshape(co, oh, ow)


def v724_expand_dazg_linear_weight(
    raw_input_output: np.ndarray,
    block_size: int,
) -> np.ndarray:
    """Expand CirLinear's learned first-column block generators."""
    raw = np.asarray(raw_input_output, dtype=np.int64)
    if raw.ndim != 2:
        raise ValueError(f"DAZG linear weight must be rank-2, got {raw.shape}")
    dim_in, dim_out = raw.shape
    b = int(block_size)
    if b <= 0 or dim_in % b != 0 or dim_out % b != 0:
        raise ValueError(f"bad DAZG linear block weight={raw.shape} block={b}")
    effective = np.empty_like(raw)
    in_offsets = np.arange(b, dtype=np.int64)
    for in_base in range(0, dim_in, b):
        for out_base in range(0, dim_out, b):
            generator = raw[in_base, out_base:out_base + b]
            for out_off in range(b):
                effective[in_base:in_base + b, out_base + out_off] = (
                    generator[np.mod(out_off - in_offsets, b)]
                )
    return effective


def v724_linear_q16_postaccum_floor(
    x_i64: np.ndarray,
    raw_weight_input_output: np.ndarray,
    bias_i64: np.ndarray,
    block_size: int,
    p: int,
) -> np.ndarray:
    """Match V721 CirLinear: modular Q32 sum, floor bridge, bias once."""
    x = np.asarray(x_i64, dtype=np.int64)
    if x.ndim == 1:
        x = x.reshape(1, -1)
    raw = np.asarray(raw_weight_input_output, dtype=np.int64)
    bias = np.asarray(bias_i64, dtype=np.int64).reshape(-1)
    effective = v724_expand_dazg_linear_weight(raw, block_size)
    if x.ndim != 2 or x.shape[1] != effective.shape[0] or bias.size != effective.shape[1]:
        raise ValueError(
            f"bad linear shapes x={x.shape} w={effective.shape} bias={bias.shape}"
        )
    q32_residue, _mode, _bound = v724_exact_dot_residue(
        x, effective, p, "linear:head.2"
    )
    q16 = v724_q32_residue_to_q16_signed(q32_residue, p)
    return q16 + bias.reshape(1, -1)


def v724_bucket_scale_q16(
    x_i64: np.ndarray,
    scale_i64: np.ndarray,
) -> np.ndarray:
    x = np.asarray(x_i64, dtype=np.int64)
    scale = np.asarray(scale_i64, dtype=np.int64).reshape(-1)
    if x.ndim != 3 or x.shape[0] != scale.size:
        raise ValueError(f"bad bucket scale shapes x={x.shape} scale={scale.shape}")
    product = x * scale[:, None, None]
    return np.floor_divide(product, np.int64(65536)).astype(np.int64)


def v724_avgpool_trunc_zero(x_i64: np.ndarray) -> np.ndarray:
    x = np.asarray(x_i64, dtype=np.int64)
    if x.ndim != 3:
        raise ValueError(f"avgpool expects CHW, got {x.shape}")
    denom = int(x.shape[1] * x.shape[2])
    sums = x.sum(axis=(1, 2), dtype=np.int64)
    # C++ signed integer division truncates toward zero.
    return np.where(
        sums >= 0,
        np.floor_divide(sums, denom),
        -np.floor_divide(-sums, denom),
    ).astype(np.int64)


def v724_binary_from_pair_report(pair_report: dict[str, Any]) -> Path:
    command = pair_report.get("server_command")
    if not isinstance(command, list) or not command or not isinstance(command[0], str):
        raise RuntimeError("pair_report.server_command does not identify the active binary")
    binary = Path(command[0]).resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise FileNotFoundError(f"active executor missing: {binary}")
    return binary


class V724FullgraphShadow:
    EXPECTED_STAGE_COUNT = 83

    def __init__(
        self,
        pair_dir: Path,
        entries: dict[str, Path],
        sample: int,
        p: int,
        pair_report: dict[str, Any],
        contract: dict[str, Any],
    ) -> None:
        self.pair_dir = pair_dir
        self.entries = entries
        self.sample = int(sample)
        self.p = int(p)
        self.pair_report = pair_report
        self.contract = contract
        self.binary = v724_binary_from_pair_report(pair_report)
        self.weight_cache: dict[str, np.ndarray] = {}
        self.bias_cache: dict[str, np.ndarray] = {}
        self.effective_conv_cache: dict[str, np.ndarray] = {}
        self.stage_order: list[str] = []
        self.stage_metrics: list[dict[str, Any]] = []
        self.first_mismatch: dict[str, Any] | None = None
        self.full_run_dir = pair_dir.parent.parent
        self.cache_dir = self.full_run_dir / "v724_shadow_cache" / f"sample_{sample}"
        self.cache_manifest = self.cache_dir / "complete.json"
        self.cache_dir.mkdir(parents=True, exist_ok=True)
        self.gelu_tmp = tempfile.TemporaryDirectory(prefix="dazg_orbit_v724_gelu.", dir="/tmp")
        self.gelu_counter = 0

    def close(self) -> None:
        self.gelu_tmp.cleanup()

    def payload(self, key: str) -> np.ndarray:
        return as_raw_i64(load_npy(required_payload(self.entries, key)))

    def conv_weight(self, name: str) -> np.ndarray:
        cached = self.effective_conv_cache.get(name)
        if cached is not None:
            return cached
        raw = self.weight_cache.get(name)
        if raw is None:
            raw = self.payload(name + ".weight")
            self.weight_cache[name] = raw
        block = int(V724_CHANNEL_BLOCKS[name])
        effective = expand_dazg_block_circulant_weight(raw, block)
        self.effective_conv_cache[name] = effective
        return effective

    def bias(self, name: str) -> np.ndarray:
        cached = self.bias_cache.get(name)
        if cached is None:
            cached = self.payload(name + ".bias")
            self.bias_cache[name] = cached
        return cached

    def cache_stage_path(self, tag: str) -> Path:
        return self.cache_dir / f"stage_{len(self.stage_order):03d}_{v724_safe_tag(tag)}.npy"

    def physical_stage(self, tag: str) -> np.ndarray:
        filename = f"v720_v724_{v724_safe_tag(tag)}.npy"
        return reconstruct(
            load_npy(self.pair_dir / "server_dump" / filename),
            load_npy(self.pair_dir / "client_dump" / filename),
            self.p,
        )

    def emit(self, tag: str, expected_i64: np.ndarray) -> np.ndarray:
        expected = v724_canonical_signed(expected_i64, self.p)
        self.stage_order.append(tag)
        if self.sample == 0:
            cache_path = self.cache_stage_path(tag)
            tmp = cache_path.with_name(cache_path.name + f".tmp.{os.getpid()}")
            with tmp.open("wb") as handle:
                np.save(handle, expected, allow_pickle=False)
            os.replace(tmp, cache_path)

        if self.sample == 0:
            try:
                got = self.physical_stage(tag)
                metric = compare_mod(got, expected, self.p)
                record: dict[str, Any] = {
                    "index": len(self.stage_order) - 1,
                    "stage": tag,
                    "exact": bool(metric.get("exact")),
                    "metric": metric,
                    "expected_signed_sha256": array_sha256(expected),
                    "physical_residue_sha256": array_sha256(got),
                }
            except Exception as exc:
                record = {
                    "index": len(self.stage_order) - 1,
                    "stage": tag,
                    "exact": False,
                    "error": f"{type(exc).__name__}: {exc}",
                }
            self.stage_metrics.append(record)
            print(
                f"[v724-stage-audit] index={record['index']}/{self.EXPECTED_STAGE_COUNT} "
                f"stage={tag} exact={record.get('exact')} "
                f"neq={(record.get('metric') or {}).get('neq')} "
                f"max={(record.get('metric') or {}).get('max_abs_delta')}",
                flush=True,
            )
            if not record.get("exact") and self.first_mismatch is None:
                self.first_mismatch = record
        return expected

    def conv(
        self,
        x: np.ndarray,
        name: str,
        stride: int,
        padding: int,
    ) -> np.ndarray:
        print(
            f"[v724-shadow] op=conv layer={name} block={V724_CHANNEL_BLOCKS[name]} "
            f"input_shape={list(x.shape)}",
            flush=True,
        )
        y = v724_conv_q16_postaccum_floor_fast(
            x,
            self.conv_weight(name),
            self.bias(name),
            stride,
            padding,
            name,
            self.p,
        )
        return v724_canonical_signed(y, self.p)

    def gelu(self, x: np.ndarray, stage: str) -> np.ndarray:
        self.gelu_counter += 1
        base = Path(self.gelu_tmp.name) / f"gelu_{self.gelu_counter:03d}_{v724_safe_tag(stage)}"
        input_path = base.with_suffix(".input.npy")
        output_path = base.with_suffix(".output.npy")
        report_path = base.with_suffix(".report.json")
        raw_u64 = np.ascontiguousarray(np.asarray(x, dtype=np.int64)).view(np.uint64)
        np.save(input_path, raw_u64, allow_pickle=False)
        command = [
            str(self.binary),
            "--mode", "plain-gelu-file",
            "--input-tensor", str(input_path),
            "--out-tensor", str(output_path),
            "--out-report", str(report_path),
        ]
        print(
            f"[v724-shadow] op=gelu stage={stage} index={self.gelu_counter} "
            f"shape={list(x.shape)}",
            flush=True,
        )
        result = subprocess.run(
            command,
            cwd=self.binary.parent,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=600,
            check=False,
        )
        report = load_json(report_path)
        if result.returncode != 0 or report.get("status") != "plain_gelu_ready":
            tail = "\\n".join(result.stdout.splitlines()[-20:])
            raise RuntimeError(
                f"plain GeLU failed stage={stage} rc={result.returncode} "
                f"status={report.get('status')} tail={tail}"
            )
        y = as_raw_i64(load_npy(output_path))
        if y.shape != x.shape:
            raise RuntimeError(f"plain GeLU shape mismatch stage={stage}: {y.shape} vs {x.shape}")
        return v724_canonical_signed(y, self.p)

    def unit(
        self,
        x: np.ndarray,
        conv0: str,
        conv1: str,
        stride0: int,
        padding0: int,
        stage: str,
    ) -> np.ndarray:
        residual = x.copy()
        branch = self.emit(stage + "_conv0", self.conv(x, conv0, stride0, padding0))
        branch = self.emit(stage + "_gelu", self.gelu(branch, stage + ".gelu"))
        branch = self.emit(stage + "_conv1", self.conv(branch, conv1, 1, 0))
        return self.emit(stage + "_out", v724_modular_add_signed(branch, residual, self.p))

    def transition(
        self,
        x: np.ndarray,
        main0: str,
        main3: str,
        skip: str,
        tail0: str,
        tail3: str,
        stage: str,
    ) -> np.ndarray:
        base = self.emit(stage + "_input", x.copy())
        main = self.emit(stage + "_main0", self.conv(base, main0, 2, 1))
        main = self.emit(stage + "_main_gelu", self.gelu(main, stage + ".main_gelu"))
        main = self.emit(stage + "_main3", self.conv(main, main3, 1, 0))
        skip_y = self.emit(stage + "_skip", self.conv(base, skip, 2, 0))
        current = self.emit(
            stage + "_main_skip_add",
            v724_modular_add_signed(main, skip_y, self.p),
        )
        current = self.unit(current, tail0, tail3, 1, 0, stage + ".tail")
        return self.emit(stage + "_output", current)

    def qchar(self, x: np.ndarray) -> np.ndarray:
        self.emit("h8_1_input", x)
        buckets = [self.emit(f"h8_1_bucket{i}_slice", chunk.copy())
                   for i, chunk in enumerate(np.split(x, 4, axis=0))]
        scale = self.payload("h8.1.bucket_scale")
        if scale.shape != (4, 48, 1, 1):
            raise ValueError(f"bad h8.1.bucket_scale shape: {scale.shape}")
        scaled: list[np.ndarray] = []
        for i, bucket in enumerate(buckets):
            conv_name = f"h8.1.local.{i}.conv"
            y = self.emit(f"h8_1_bucket{i}_conv", self.conv(bucket, conv_name, 1, 0))
            y = self.emit(
                f"h8_1_bucket{i}_scaled",
                v724_bucket_scale_q16(y, scale[i, :, 0, 0]),
            )
            scaled.append(v724_canonical_signed(y, self.p))
        cat = self.emit("h8_1_concat", np.concatenate(scaled, axis=0))
        cat = self.emit("h8_1_gelu", self.gelu(cat, "h8.1.qchar.gelu"))
        return self.emit("h8_1_mix", self.conv(cat, "h8.1.mix.conv", 1, 0))

    def head(self, feature: np.ndarray) -> np.ndarray:
        pooled = self.emit("head_avgpool", v724_avgpool_trunc_zero(feature))
        raw_head = self.payload("head.2.weight")
        if raw_head.shape != (100, 192):
            raise ValueError(f"bad head weight shape: {raw_head.shape}")
        transposed = raw_head.T.copy()
        bias = self.payload("head.2.bias")
        logits = v724_linear_q16_postaccum_floor(
            pooled.reshape(1, -1), transposed, bias, 4, self.p
        )
        return self.emit("head_logits", v724_canonical_signed(logits, self.p))

    def build(self) -> np.ndarray:
        x_all = self.payload("qahl_ref_input_n10.npy")
        if x_all.ndim != 4 or self.sample < 0 or self.sample >= x_all.shape[0]:
            raise ValueError(f"bad input shape/sample: {x_all.shape}, sample={self.sample}")
        x = v724_canonical_signed(x_all[self.sample], self.p)
        current = self.emit("stem0", self.conv(x, "stem.0", 1, 1))
        current = self.emit("stem1_gelu", self.gelu(current, "stem.1.gelu"))
        current = self.emit("stem2", self.conv(current, "stem.2.conv", 1, 0))
        current = self.unit(current, "stem.3.net.0.conv", "stem.3.net.3.conv", 1, 0, "stem.3")
        current = self.unit(current, "h32.0.body.0", "h32.0.body.3.conv", 1, 1, "h32.0.body")
        current = self.unit(current, "h32.0.anchor.net.0.conv", "h32.0.anchor.net.3.conv", 1, 0, "h32.0.anchor")
        current = self.unit(current, "h32.1.net.0.conv", "h32.1.net.3.conv", 1, 0, "h32.1")
        current = self.transition(
            current,
            "to_h16.main.0", "to_h16.main.3.conv", "to_h16.skip",
            "to_h16.tail.net.0.conv", "to_h16.tail.net.3.conv", "to_h16",
        )
        current = self.unit(current, "h16.0.body.0", "h16.0.body.3.conv", 1, 1, "h16.0.body")
        current = self.unit(current, "h16.0.anchor.net.0.conv", "h16.0.anchor.net.3.conv", 1, 0, "h16.0.anchor")
        current = self.unit(current, "h16.1.net.0.conv", "h16.1.net.3.conv", 1, 0, "h16.1")
        current = self.transition(
            current,
            "to_h8.main.0", "to_h8.main.3.conv", "to_h8.skip",
            "to_h8.tail.net.0.conv", "to_h8.tail.net.3.conv", "to_h8",
        )
        current = self.unit(current, "h8.0.body.0", "h8.0.body.3.conv", 1, 1, "h8.0.body")
        current = self.unit(current, "h8.0.anchor.net.0.conv", "h8.0.anchor.net.3.conv", 1, 0, "h8.0.anchor")
        current = self.qchar(current)
        current = self.unit(current, "h8.2.net.0.conv", "h8.2.net.3.conv", 1, 0, "h8.2")
        logits = self.head(current)
        if len(self.stage_order) != self.EXPECTED_STAGE_COUNT:
            raise RuntimeError(
                f"V724 stage contract count mismatch: {len(self.stage_order)} "
                f"!= {self.EXPECTED_STAGE_COUNT}"
            )
        return logits

    def cache_valid(self) -> bool:
        manifest = load_json(self.cache_manifest)
        if not (
            manifest.get("reference_marker") == V724_FULLGRAPH_REFERENCE_MARKER and
            manifest.get("sample") == self.sample and
            manifest.get("plain_mod") == self.p and
            manifest.get("stage_count") == self.EXPECTED_STAGE_COUNT and
            (self.cache_dir / "expected_logits.npy").is_file()
        ):
            return False
        if self.sample == 0:
            files = manifest.get("stage_files")
            return isinstance(files, list) and len(files) == self.EXPECTED_STAGE_COUNT and all(
                (self.cache_dir / str(name)).is_file() for name in files
            )
        return True

    def audit_cached_sample0(self, manifest: dict[str, Any]) -> None:
        self.stage_order = list(manifest.get("stage_order") or [])
        files = list(manifest.get("stage_files") or [])
        if len(self.stage_order) != self.EXPECTED_STAGE_COUNT or len(files) != self.EXPECTED_STAGE_COUNT:
            raise RuntimeError("cached V724 stage manifest is incomplete")
        for index, (tag, name) in enumerate(zip(self.stage_order, files)):
            expected = as_raw_i64(load_npy(self.cache_dir / name))
            try:
                got = self.physical_stage(tag)
                metric = compare_mod(got, expected, self.p)
                record: dict[str, Any] = {
                    "index": index,
                    "stage": tag,
                    "exact": bool(metric.get("exact")),
                    "metric": metric,
                    "expected_signed_sha256": array_sha256(expected),
                    "physical_residue_sha256": array_sha256(got),
                }
            except Exception as exc:
                record = {
                    "index": index,
                    "stage": tag,
                    "exact": False,
                    "error": f"{type(exc).__name__}: {exc}",
                }
            self.stage_metrics.append(record)
            print(
                f"[v724-stage-audit-cache] index={index}/{self.EXPECTED_STAGE_COUNT} "
                f"stage={tag} exact={record.get('exact')} "
                f"neq={(record.get('metric') or {}).get('neq')}",
                flush=True,
            )
            if not record.get("exact") and self.first_mismatch is None:
                self.first_mismatch = record

    def run(self) -> tuple[np.ndarray, dict[str, Any]]:
        generated = False
        if self.cache_valid():
            manifest = load_json(self.cache_manifest)
            print(
                f"[v724-shadow-cache] hit sample={self.sample} dir={self.cache_dir}",
                flush=True,
            )
            logits = as_raw_i64(load_npy(self.cache_dir / "expected_logits.npy"))
            if self.sample == 0:
                self.audit_cached_sample0(manifest)
        else:
            print(
                f"[v724-shadow-cache] miss sample={self.sample}; computing full DAZG shadow",
                flush=True,
            )
            for old in self.cache_dir.glob("stage_*.npy"):
                old.unlink()
            logits = self.build()
            generated = True
            expected_logits_path = self.cache_dir / "expected_logits.npy"
            tmp = expected_logits_path.with_name(expected_logits_path.name + f".tmp.{os.getpid()}")
            with tmp.open("wb") as handle:
                np.save(handle, logits, allow_pickle=False)
            os.replace(tmp, expected_logits_path)
            stage_files = [p.name for p in sorted(self.cache_dir.glob("stage_*.npy"))]
            manifest = {
                "schema": "dazg_orbit.qahl.v724.shadow_cache",
                "reference_marker": V724_FULLGRAPH_REFERENCE_MARKER,
                "sample": self.sample,
                "plain_mod": self.p,
                "stage_count": self.EXPECTED_STAGE_COUNT,
                "stage_order": self.stage_order,
                "stage_files": stage_files,
                "expected_logits_sha256": array_sha256(logits),
                "created_unix": time.time(),
            }
            atomic_json(self.cache_manifest, manifest)
        stage_exact = (
            len(self.stage_metrics) == self.EXPECTED_STAGE_COUNT and
            all(record.get("exact") is True for record in self.stage_metrics)
            if self.sample == 0 else None
        )
        audit = {
            "reference_marker": V724_FULLGRAPH_REFERENCE_MARKER,
            "executor_marker": V724_EXECUTOR_MARKER,
            "generated_now": generated,
            "cache_dir": str(self.cache_dir),
            "stage_dump_expected_count": self.EXPECTED_STAGE_COUNT,
            "stage_dump_server_count": self.contract.get("server_v724_stage_dump_count"),
            "stage_dump_client_count": self.contract.get("client_v724_stage_dump_count"),
            "stage_dump_server_enabled": self.contract.get("server_v724_stage_dump_enabled"),
            "stage_dump_client_enabled": self.contract.get("client_v724_stage_dump_enabled"),
            "stage_order": self.stage_order if self.sample == 0 else [],
            "stage_exact": stage_exact,
            "stage_metrics": self.stage_metrics if self.sample == 0 else [],
            "first_mismatch": self.first_mismatch,
            "gelu_reference_calls": self.gelu_counter,
        }
        return logits, audit


def score_full(
    pair_dir: Path,
    entries: dict[str, Path],
    sample: int,
    p: int,
    runtime: dict[str, Any],
    contract: dict[str, Any],
    pair_report: dict[str, Any],
) -> dict[str, Any]:
    print(f"[score] reconstructing fullgraph logits sample={sample}", flush=True)
    got = reconstruct(
        load_npy(pair_dir / "server_logits.npy"),
        load_npy(pair_dir / "client_logits.npy"),
        p,
    )
    got_signed = centered(got, p).reshape(-1)

    shadow = V724FullgraphShadow(
        pair_dir, entries, sample, p, pair_report, contract
    )
    try:
        expected_shadow_2d, stage_audit = shadow.run()
    finally:
        shadow.close()
    expected_shadow = np.asarray(expected_shadow_2d, dtype=np.int64).reshape(-1)
    shadow_metric = compare_mod(got.reshape(-1), expected_shadow, p)

    refs = as_raw_i64(load_npy(required_payload(entries, "qahl_ref_logits_n10.npy")))
    if refs.ndim != 2 or sample >= refs.shape[0]:
        raise ValueError(f"bad legacy reference logits shape/sample: {refs.shape}, sample={sample}")
    legacy_expected = refs[sample].reshape(-1)
    legacy_metric = compare_mod(got.reshape(-1), legacy_expected, p)

    route = runtime.get("to_h8_route") or {}
    pcoi_route_active = (
        route.get("layout_mode") == 7 and
        route.get("pcoi_k3s2") == 1 and
        route.get("capacity_ok") == 1
    )
    if sample == 0:
        stage_contract_ok = (
            contract.get("server_v724_stage_dump_enabled") is True and
            contract.get("client_v724_stage_dump_enabled") is True and
            contract.get("server_v724_stage_dump_count") == V724FullgraphShadow.EXPECTED_STAGE_COUNT and
            contract.get("client_v724_stage_dump_count") == V724FullgraphShadow.EXPECTED_STAGE_COUNT and
            stage_audit.get("stage_exact") is True
        )
    else:
        stage_contract_ok = (
            contract.get("server_v724_stage_dump_enabled") is False and
            contract.get("client_v724_stage_dump_enabled") is False and
            contract.get("server_v724_stage_dump_count") == 0 and
            contract.get("client_v724_stage_dump_count") == 0
        )

    labels = as_raw_i64(load_npy(required_payload(entries, "qahl_ref_labels_n10.npy"))).reshape(-1)
    label = int(labels[sample]) if sample < labels.size else None
    got_top1 = int(np.argmax(got_signed))
    shadow_top1 = int(np.argmax(expected_shadow))
    legacy_top1 = int(np.argmax(legacy_expected))
    got_top5 = [int(x) for x in np.argsort(got_signed)[-5:][::-1]]
    shadow_top5 = [int(x) for x in np.argsort(expected_shadow)[-5:][::-1]]
    legacy_top5 = [int(x) for x in np.argsort(legacy_expected)[-5:][::-1]]
    top1_correct = label is not None and got_top1 == label
    top5_correct = label is not None and label in got_top5

    strict = bool(shadow_metric["exact"] and pcoi_route_active and stage_contract_ok)
    exact_contracts = []
    if shadow_metric["exact"]:
        exact_contracts.append("fullgraph_dazg_shadow_numeric")
    if pcoi_route_active:
        exact_contracts.append("to_h8_k3s2_pcoi_route_active")
    if stage_contract_ok:
        exact_contracts.append(
            "sample0_all_83_stages_exact" if sample == 0
            else "nonzero_sample_stage_dump_disabled_by_contract"
        )

    return {
        "kind": "full",
        "reference_marker": V724_FULLGRAPH_REFERENCE_MARKER,
        "reference_semantics": "input_to_logits_dazg_block_circulant_plus_pcoi",
        "channel_formula": V723_DAZG_CHANNEL_FORMULA,
        "linear_formula": V724_LINEAR_FORMULA,
        "strict_exact": strict,
        "numeric_exact": bool(shadow_metric["exact"]),
        "metric": shadow_metric,
        "exact_contracts": exact_contracts,
        "pcoi_route_active": pcoi_route_active,
        "stage_contract_ok": stage_contract_ok,
        "stage_audit": stage_audit,
        "legacy_dense_reference_metric": legacy_metric,
        "legacy_dense_reference_role": "audit_only_not_model_contract",
        "logits_shape": list(got.shape),
        "label": label,
        "got_top1": got_top1,
        "reference_top1": shadow_top1,
        "shadow_top1": shadow_top1,
        "legacy_dense_top1": legacy_top1,
        "got_top5": got_top5,
        "reference_top5": shadow_top5,
        "shadow_top5": shadow_top5,
        "legacy_dense_top5": legacy_top5,
        # DAZG_ORBIT_V7403_TRAINED_REFERENCE_FIELDS_BEGIN
        "trained_dense_top1_agreement": got_top1 == legacy_top1,
        "trained_dense_top5_agreement": got_top5 == legacy_top5,
        "trained_dense_prediction_contract": "audit_only_dense_gelu_accuracy_floor",
        # DAZG_ORBIT_V7403_TRAINED_REFERENCE_FIELDS_END
        "top1_correct": top1_correct,
        "top5_correct": top5_correct,
        "accuracy_preserved": bool(
            shadow_metric["exact"] and got_top1 == shadow_top1 and got_top5 == shadow_top5
        ),
        "reference_logits_sha256": array_sha256(expected_shadow),
        "shadow_logits_sha256": array_sha256(expected_shadow),
        "legacy_dense_logits_sha256": array_sha256(legacy_expected),
        "reconstructed_logits_sha256": array_sha256(got_signed),
    }

def selftest(out_path: Path) -> int:
    print("[selftest] merger 100/100", flush=True)
    rng = np.random.default_rng(720)
    p = 1125899906826241
    passed = 0
    for trial in range(100):
        n = int(rng.integers(1, 129))
        signed = rng.integers(-1000000, 1000001, size=n, dtype=np.int64)
        target = expected_to_residue(signed, p)
        s = rng.integers(0, p, size=n, dtype=np.uint64)
        c = v740_2_reconstruct_u64(target + np.uint64(p), -s.astype(np.int64), p)
        got = reconstruct(s, c, p)
        if np.array_equal(got, target):
            passed += 1
    baseline = np.array([1, -2, 3, -4], dtype=np.int64)
    fault = baseline.copy()
    fault[2] += 1
    intentional_fault_failed = not compare_mod(
        expected_to_residue(fault, p), baseline, p
    )["exact"]
    input_hash = array_sha256(np.arange(32, dtype=np.uint64))
    reference_hash = array_sha256(np.arange(100, dtype=np.uint64) + 1000)
    hashes_independent = input_hash != reference_hash

    # Independent algebra test for the learned DAZG channel orbit.
    bsz = 4
    x_small = rng.integers(-128, 129, size=(8, 5, 5), dtype=np.int64)
    w_small = np.zeros((8, 8, 3, 3), dtype=np.int64)
    for ob in range(0, 8, bsz):
        for ib in range(0, 8, bsz):
            w_small[ob:ob + bsz, ib, :, :] = rng.integers(
                -64, 65, size=(bsz, 3, 3), dtype=np.int64
            )
    bias_small = rng.integers(-16, 17, size=8, dtype=np.int64)
    expanded_small = expand_dazg_block_circulant_weight(w_small, bsz)
    dense_small = conv_q16_postaccum_floor(
        x_small, expanded_small, bias_small, stride=2, padding=1
    )
    manual_small = manual_dazg_conv_q16_postaccum_floor(
        x_small, w_small, bias_small, bsz, stride=2, padding=1
    )
    dazg_formula_exact = np.array_equal(dense_small, manual_small)

    wrong = expanded_small.copy()
    wrong[[0, 1]] = wrong[[1, 0]]
    orientation_fault_rejected = not np.array_equal(
        conv_q16_postaccum_floor(
            x_small, wrong, bias_small, stride=2, padding=1
        ),
        manual_small,
    )

    # CirLinear first-column channel-orbit orientation test.
    raw_linear = np.zeros((8, 8), dtype=np.int64)
    for ib in range(0, 8, bsz):
        for ob in range(0, 8, bsz):
            raw_linear[ib, ob:ob + bsz] = rng.integers(
                -32, 33, size=bsz, dtype=np.int64
            )
    expanded_linear = v724_expand_dazg_linear_weight(raw_linear, bsz)
    x_linear = rng.integers(-128, 129, size=(3, 8), dtype=np.int64)
    manual_linear = np.zeros((3, 8), dtype=np.int64)
    for n in range(3):
        for ob in range(0, 8, bsz):
            for oo in range(bsz):
                for ib in range(0, 8, bsz):
                    for io in range(bsz):
                        manual_linear[n, ob + oo] += (
                            x_linear[n, ib + io] *
                            raw_linear[ib, ob + ((oo - io) % bsz)]
                        )
    dazg_linear_formula_exact = np.array_equal(
        x_linear @ expanded_linear, manual_linear
    )
    wrong_linear = expanded_linear.copy()
    wrong_linear[:, [0, 1]] = wrong_linear[:, [1, 0]]
    dazg_linear_orientation_fault_rejected = not np.array_equal(
        x_linear @ wrong_linear, manual_linear
    )

    bucket_x = np.array([[[-3, 3]], [[-5, 5]]], dtype=np.int64)
    bucket_s = np.array([32768, 98304], dtype=np.int64)
    bucket_got = v724_bucket_scale_q16(bucket_x, bucket_s)
    bucket_expected = np.array([[[-2, 1]], [[-8, 7]]], dtype=np.int64)
    bucket_negative_floor_exact = np.array_equal(bucket_got, bucket_expected)

    pool_x = np.array([[[ -3, -2], [1, 1]], [[3, 2], [-1, -1]]], dtype=np.int64)
    pool_got = v724_avgpool_trunc_zero(pool_x)
    avgpool_trunc_zero_exact = np.array_equal(pool_got, np.array([0, 0], dtype=np.int64))

    # Exact modular Q32 bridge tests, including the overflow-safe limb path.
    p_small = 1048583
    left_small = np.array([[700000, -800000, 900000]], dtype=np.int64)
    right_small = np.array([[900000], [800000], [-700000]], dtype=np.int64)
    got_small, mode_small, _ = v724_exact_dot_residue(
        left_small, right_small, p_small, "selftest-small-modp"
    )
    expected_small = sum(
        int(left_small[0, k]) * int(right_small[k, 0])
        for k in range(left_small.shape[1])
    ) % p_small
    modular_q32_exact = int(got_small[0, 0]) == expected_small

    # Force conservative-bound overflow while keeping the true answer auditable.
    left_big = np.array([[p - 2, p - 3]], dtype=np.int64)
    right_big = np.array([[p - 5], [p - 7]], dtype=np.int64)
    got_big, mode_big, _ = v724_exact_dot_residue(
        left_big, right_big, p, "selftest-limb-modp"
    )
    expected_big = (
        int(left_big[0, 0]) * int(right_big[0, 0]) +
        int(left_big[0, 1]) * int(right_big[1, 0])
    ) % p
    limb_modular_q32_exact = (
        mode_big == "base8192_limb_modp" and
        int(got_big[0, 0]) == expected_big
    )

    q32_wrap = np.array([p - 32768], dtype=np.int64)
    q16_wrap = v724_q32_residue_to_q16_signed(q32_wrap, p)
    centered_floor_exact = int(q16_wrap[0]) == -1

    all_ok = (
        passed == 100 and intentional_fault_failed and hashes_independent and
        dazg_formula_exact and orientation_fault_rejected and
        dazg_linear_formula_exact and dazg_linear_orientation_fault_rejected and
        bucket_negative_floor_exact and avgpool_trunc_zero_exact and
        modular_q32_exact and limb_modular_q32_exact and centered_floor_exact
    )
    report = {
        "schema": "dazg_orbit.qahl.v724.scorer_selftest",
        "status": "pass" if all_ok else "fail",
        "reference_marker": V723_DAZG_REFERENCE_MARKER,
        "channel_formula": V723_DAZG_CHANNEL_FORMULA,
        "merger_passed": passed,
        "merger_total": 100,
        "intentional_fault_failed": intentional_fault_failed,
        "input_reference_hash_independent": hashes_independent,
        "dazg_formula_exact": dazg_formula_exact,
        "orientation_fault_rejected": orientation_fault_rejected,
        "dazg_linear_formula_exact": dazg_linear_formula_exact,
        "dazg_linear_orientation_fault_rejected": dazg_linear_orientation_fault_rejected,
        "bucket_negative_floor_exact": bucket_negative_floor_exact,
        "avgpool_trunc_zero_exact": avgpool_trunc_zero_exact,
        "modular_q32_exact": modular_q32_exact,
        "limb_modular_q32_exact": limb_modular_q32_exact,
        "centered_q32_floor_exact": centered_floor_exact,
        "v724_expected_stage_count": V724FullgraphShadow.EXPECTED_STAGE_COUNT,
        "input_hash": input_hash,
        "reference_hash": reference_hash,
    }
    atomic_json(out_path, report)
    print(
        f"[selftest-end] status={report['status']} merger={passed}/100 "
        f"intentional_fault_failed={intentional_fault_failed} "
        f"dazg_formula_exact={dazg_formula_exact} "
        f"orientation_fault_rejected={orientation_fault_rejected} "
        f"dazg_linear_formula_exact={dazg_linear_formula_exact} "
        f"bucket_floor={bucket_negative_floor_exact} "
        f"avgpool_trunc_zero={avgpool_trunc_zero_exact} "
        f"modular_q32={modular_q32_exact} "
        f"limb_modp={limb_modular_q32_exact} "
        f"centered_floor={centered_floor_exact}",
        flush=True,
    )
    return 0 if report["status"] == "pass" else 2


def score(args: argparse.Namespace) -> int:
    pair_dir = Path(args.pair_dir).resolve()
    out_path = Path(args.out).resolve()
    entries = parse_inventory(Path(args.inventory).resolve())
    pair_report = load_json(pair_dir / "pair_report.json")
    contract = report_contract(pair_dir, pair_report)
    p = int(contract.get("plain_mod") or 0)
    if p <= 0:
        raise RuntimeError("plain_mod is missing from executor reports")
    runtime = parse_runtime(pair_dir / "server.log")

    if args.kind == "firstconv":
        core = score_firstconv(pair_dir, entries, args.sample, p)
    elif args.kind == "to_h8":
        core = score_to_h8(pair_dir, entries, p, runtime)
    elif args.kind == "full":
        core = score_full(pair_dir, entries, args.sample, p, runtime, contract, pair_report)
    else:
        raise ValueError(args.kind)

    protocol = protocol_gate(contract)
    safety = reference_safety(pair_dir, entries, args.kind)
    scientific_exact = bool(core.get("strict_exact"))
    outer_failures = (
        [f"protocol:{name}" for name in protocol["failures"]] +
        [f"reference_safety:{name}" for name in safety["failures"]]
    )
    # DAZG_ORBIT_V743R8P24_SCORE_GATE_PATCH_BEGIN
    inner_gate_failures = list(core.get("gate_failures") or [])
    core["inner_gate_failures_before_outer"] = inner_gate_failures
    core["outer_gate_failures"] = outer_failures
    core["scientific_exact_before_outer_gates"] = scientific_exact
    core["strict_exact"] = bool(
        scientific_exact and protocol["ok"] and safety["reference_echo_safe"]
    )
    core["gate_failures"] = inner_gate_failures + outer_failures
    # DAZG_ORBIT_V743R8P24_SCORE_GATE_PATCH_END

    cost = dict(runtime)
    cost.update(
        {
            "pair_wall_s": pair_report.get("wall_s"),
            "server_wall_ms": contract.get("server_wall_ms"),
            "client_wall_ms": contract.get("client_wall_ms"),
            "server_communication_bytes_sent": contract.get(
                "server_communication_bytes_sent"
            ),
            "client_communication_bytes_sent": contract.get(
                "client_communication_bytes_sent"
            ),
            "communication_bytes_total": contract.get(
                "communication_bytes_total"
            ),
            "server_network_rounds_observed": contract.get(
                "server_network_rounds_observed"
            ),
            "client_network_rounds_observed": contract.get(
                "client_network_rounds_observed"
            ),
            "network_rounds_per_pair": contract.get(
                "network_rounds_per_pair"
            ),
        }
    )

    if core["strict_exact"]:
        status = "pass"
    elif scientific_exact:
        status = "gate_failed"
    else:
        status = "not_exact"

    # DAZG_ORBIT_V743R8P80_SCORE_SECURITY_GUARD_BEGIN
    reveal_count_total = int(contract.get("server_diagnostic_reveal_count") or 0) + int(contract.get("client_diagnostic_reveal_count") or 0)
    bridge_blocker_keys = [
        "server_p73_q32_pair_carry_exchange_count", "client_p73_q32_pair_carry_exchange_count",
        "server_p74_nonlinear_pair_bridge_count", "client_p74_nonlinear_pair_bridge_count",
        "server_p77_q32_low16_carry_exchange_count", "client_p77_q32_low16_carry_exchange_count",
        "server_p80_q32_wrap_exchange_count", "client_p80_q32_wrap_exchange_count",
    ]
    bridge_blocker_count = sum(int(contract.get(k) or 0) for k in bridge_blocker_keys)
    security_blocked = bool(contract.get("diagnostic_reveal")) or reveal_count_total != 0 or bridge_blocker_count != 0
    # DAZG_ORBIT_V743R8P80_SCORE_SECURITY_GUARD_END

    report = {
        "schema": "dazg_orbit.qahl.v724.score",
        "status": status,
        "kind": args.kind,
        "sample_index": args.sample,
        "pair_dir": str(pair_dir),
        "plain_mod": p,
        "protocol": contract,
        "protocol_gate": protocol,
        "pair": pair_report,
        "correctness": core,
        "cost": cost,
        "reference_safety": safety,
        "security": {
            "reveal_oracle_used": bool(contract.get("diagnostic_reveal")),
            "diagnostic_or_candidate_bridge_count": bridge_blocker_count,
            "security_claim": 0 if security_blocked else 1,
            "security_claim_reason": "blocked_by_diagnostic_reveal_or_candidate_bridge" if security_blocked else "no_reveal_flags_or_counts",
        },
        "scored_unix": time.time(),
    }
    atomic_json(out_path, report)
    metric = core.get("metric", {})
    print(
        f"[score-end] kind={args.kind} sample={args.sample} "
        f"strict_exact={core.get('strict_exact')} "
        f"scientific_exact={scientific_exact} "
        f"neq={metric.get('neq')} max={metric.get('max_abs_delta')} "
        f"l1={metric.get('l1')} rotations={cost.get('rotate_rows')} "
        f"hecompute_us={cost.get('hecompute_us')} "
        f"communication_bytes={cost.get('communication_bytes_total')} "
        f"rounds={cost.get('network_rounds_per_pair')} "
        f"pcoi_route={core.get('pcoi_route_active')} "
        f"stage_contract={core.get('stage_contract_ok')} "
        f"first_stage_mismatch={((core.get('stage_audit') or {}).get('first_mismatch') or {}).get('stage')} "
        f"legacy_dense_neq={(core.get('legacy_dense_reference_metric') or {}).get('neq')} "
        f"gate_failures={core.get('gate_failures')} out={out_path}",
        flush=True,
    )
    # Numeric non-exactness is a scientific result, not a scorer crash.
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    p_self = sub.add_parser("selftest")
    p_self.add_argument("--out", required=True)
    p_score = sub.add_parser("score")
    p_score.add_argument("--kind", choices=["firstconv", "to_h8", "full"], required=True)
    p_score.add_argument("--pair-dir", required=True)
    p_score.add_argument("--inventory", required=True)
    p_score.add_argument("--sample", type=int, required=True)
    p_score.add_argument("--out", required=True)
    args = parser.parse_args()
    if args.command == "selftest":
        return selftest(Path(args.out).resolve())
    return score(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"[score-error] {type(exc).__name__}: {exc}", file=sys.stderr, flush=True)
        raise
