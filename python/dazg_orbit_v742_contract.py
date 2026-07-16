# DAZG-Orbit Project Source File
# Component: python/dazg_orbit_v742_contract.py
# Purpose: Fail-closed experiment, verification, or Q16 oracle component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
# -*- coding: utf-8 -*-
from __future__ import annotations
import json, time, hashlib
from collections import OrderedDict
from pathlib import Path
from typing import Any, Mapping

try:
    import torch
except Exception:
    torch = None

V742_VERSION = "DAZG_ORBIT_V742_DAZG_QAT_EXPORT_CONTRACT_20260624_001"

DEFAULT_QAHL_DAZG_BLOCKS = OrderedDict([
    ("stem.0", 1),
    ("stem.2.conv", 8),
    ("stem.3.net.0.conv", 4),
    ("stem.3.net.3.conv", 4),
    ("h32.0.body.0", 2),
    ("h32.0.body.3.conv", 8),
    ("h32.0.anchor.net.0.conv", 4),
    ("h32.0.anchor.net.3.conv", 4),
    ("h32.1.net.0.conv", 4),
    ("h32.1.net.3.conv", 4),
    ("to_h16.main.0", 8),
    ("to_h16.main.3.conv", 32),
    ("to_h16.skip", 16),
    ("to_h16.tail.net.0.conv", 8),
    ("to_h16.tail.net.3.conv", 8),
    ("h16.0.body.0", 8),
    ("h16.0.body.3.conv", 32),
    ("h16.0.anchor.net.0.conv", 8),
    ("h16.0.anchor.net.3.conv", 8),
    ("h16.1.net.0.conv", 8),
    ("h16.1.net.3.conv", 8),
    ("to_h8.main.0", 32),
    ("to_h8.main.3.conv", 64),
    ("to_h8.skip", 32),
    ("to_h8.tail.net.0.conv", 16),
    ("to_h8.tail.net.3.conv", 16),
    ("h8.0.body.0", 32),
    ("h8.0.body.3.conv", 64),
    ("h8.0.anchor.net.0.conv", 16),
    ("h8.0.anchor.net.3.conv", 16),
    ("h8.1.local.0.conv", 16),
    ("h8.1.local.1.conv", 16),
    ("h8.1.local.2.conv", 16),
    ("h8.1.local.3.conv", 16),
    ("h8.1.mix.conv", 64),
    ("h8.2.net.0.conv", 16),
    ("h8.2.net.3.conv", 16),
    ("head.2", 4),
])

NOTES = [
    "Dense checkpoints must not be silently interpreted as DAZG first-column generators.",
    "Use DAZG-aware fine-tuning or export with --dazg_contract_mode=require will fail.",
    "Train activation must match deployment lowering; use bfe_poly2/dazg_bfe for the deployment-aligned branch.",
]

def _need_torch():
    if torch is None:
        raise RuntimeError("torch is required for V742 DAZG contract utilities")
    return torch

def block_policy_sha256(policy=None):
    policy = policy or DEFAULT_QAHL_DAZG_BLOCKS
    payload = json.dumps(list(policy.items()), separators=(",", ":")).encode()
    return hashlib.sha256(payload).hexdigest()

def project_conv_weight_to_dazg(weight, block: int):
    t = _need_torch()
    b = int(block)
    if b <= 1:
        return weight
    if weight.dim() != 4:
        raise ValueError(f"expected rank-4 conv weight, got {tuple(weight.shape)}")
    oc, ic, kh, kw = weight.shape
    if oc % b or ic % b:
        raise ValueError(f"conv weight shape {tuple(weight.shape)} not divisible by block {b}")
    q, p = oc // b, ic // b
    w = weight.reshape(q, b, p, b, kh, kw)
    gen = []
    for g in range(b):
        vals = []
        for i in range(b):
            j = (i - g) % b
            vals.append(w[:, i, :, j, :, :])
        gen.append(t.stack(vals, dim=0).mean(dim=0))
    gen = t.stack(gen, dim=1)
    rows = []
    for i in range(b):
        cols = []
        for j in range(b):
            cols.append(gen[:, (i - j) % b, :, :, :])
        rows.append(t.stack(cols, dim=2))
    return t.stack(rows, dim=1).reshape(oc, ic, kh, kw)

def project_linear_weight_to_dazg(weight, block: int):
    t = _need_torch()
    b = int(block)
    if b <= 1:
        return weight
    if weight.dim() != 2:
        raise ValueError(f"expected rank-2 linear weight, got {tuple(weight.shape)}")
    of, inf = weight.shape
    if of % b or inf % b:
        raise ValueError(f"linear weight shape {tuple(weight.shape)} not divisible by block {b}")
    q, p = of // b, inf // b
    w = weight.reshape(q, b, p, b)
    gen = []
    for g in range(b):
        vals = []
        for i in range(b):
            vals.append(w[:, i, :, (i - g) % b])
        gen.append(t.stack(vals, dim=0).mean(dim=0))
    gen = t.stack(gen, dim=1)
    rows = []
    for i in range(b):
        cols = []
        for j in range(b):
            cols.append(gen[:, (i - j) % b, :])
        rows.append(t.stack(cols, dim=2))
    return t.stack(rows, dim=1).reshape(of, inf)

def project_weight_to_dazg(name: str, weight, policy=None):
    policy = policy or DEFAULT_QAHL_DAZG_BLOCKS
    layer = name[:-7] if name.endswith(".weight") else name
    block = int(policy.get(layer, 1))
    if block <= 1:
        return weight
    if weight.dim() == 4:
        return project_conv_weight_to_dazg(weight, block)
    if weight.dim() == 2:
        return project_linear_weight_to_dazg(weight, block)
    return weight

def dazg_contract_loss(model, policy=None, device=None):
    t = _need_torch()
    policy = policy or DEFAULT_QAHL_DAZG_BLOCKS
    losses, layers = [], []
    for name, param in model.named_parameters():
        if not name.endswith(".weight"):
            continue
        layer = name[:-7]
        block = int(policy.get(layer, 1))
        if block <= 1 or param.dim() not in (2, 4):
            continue
        projected = project_weight_to_dazg(name, param, policy)
        mse = (param - projected).pow(2).mean()
        denom = param.detach().pow(2).mean().clamp_min(1e-12)
        losses.append(mse)
        layers.append({"layer": layer, "block": block, "mse": float(mse.detach().cpu()), "relative_mse": float((mse.detach() / denom).cpu())})
    total = t.stack(losses).mean() if losses else t.zeros((), device=device if device is not None else None)
    return {"loss": total, "num_layers": len(layers), "layers": layers}

def project_model_to_dazg_(model, policy=None):
    policy = policy or DEFAULT_QAHL_DAZG_BLOCKS
    changed = []
    with _need_torch().no_grad():
        for name, param in model.named_parameters():
            if not name.endswith(".weight"):
                continue
            layer = name[:-7]
            block = int(policy.get(layer, 1))
            if block <= 1 or param.dim() not in (2, 4):
                continue
            projected = project_weight_to_dazg(name, param, policy)
            delta = float((param - projected).abs().max().item()) if param.numel() else 0.0
            param.copy_(projected)
            changed.append({"layer": layer, "block": block, "max_abs_delta_before_project": delta})
    return {"version": V742_VERSION, "num_projected_layers": len(changed), "projected_layers": changed}

def project_state_dict_to_dazg(state, policy=None):
    policy = policy or DEFAULT_QAHL_DAZG_BLOCKS
    out = OrderedDict()
    for name, value in state.items():
        if torch is not None and torch.is_tensor(value) and name.endswith(".weight"):
            layer = name[:-7]
            block = int(policy.get(layer, 1))
            if block > 1 and value.dim() in (2, 4):
                out[name] = project_weight_to_dazg(name, value, policy).detach().clone()
                continue
        out[name] = value.detach().clone() if torch is not None and torch.is_tensor(value) else value
    return out

def dazg_contract_report_state_dict(state, policy=None, atol=0.0):
    policy = policy or DEFAULT_QAHL_DAZG_BLOCKS
    layers, violations = [], []
    total_numel, total_neq, max_abs = 0, 0, 0.0
    for name, value in state.items():
        if torch is None or not torch.is_tensor(value) or not name.endswith(".weight"):
            continue
        layer = name[:-7]
        block = int(policy.get(layer, 1))
        if block <= 1 or value.dim() not in (2, 4):
            continue
        try:
            projected = project_weight_to_dazg(name, value, policy)
            diff = (value - projected).detach()
            absdiff = diff.abs()
            neq = int((absdiff > float(atol)).sum().item())
            numel = int(value.numel())
            mx = float(absdiff.max().item()) if numel else 0.0
            mse = float(diff.pow(2).mean().item()) if numel else 0.0
            denom = float(value.detach().pow(2).mean().clamp_min(1e-12).item()) if numel else 1.0
            rec = {"layer": layer, "weight": name, "shape": list(value.shape), "block": block, "neq": neq, "numel": numel, "match_ratio": 1.0 - neq / max(1, numel), "max_abs_delta": mx, "mse": mse, "relative_mse": mse / denom}
            layers.append(rec)
            total_numel += numel
            total_neq += neq
            max_abs = max(max_abs, mx)
            if neq:
                violations.append(rec)
        except Exception as exc:
            violations.append({"layer": layer, "weight": name, "block": block, "error": repr(exc)})
    return {"version": V742_VERSION, "policy_sha256": block_policy_sha256(policy), "contract_semantics": "dazg_block_circulant_first_column_generator", "contract_ok": not violations, "total_numel": total_numel, "total_neq": total_neq, "match_ratio": 1.0 - total_neq / max(1, total_numel), "max_abs_delta": max_abs, "num_checked_layers": len(layers), "num_violating_layers": len(violations), "layers": layers, "violations_first20": violations[:20], "notes": NOTES}

def dazg_contract_report_model(model, policy=None):
    return dazg_contract_report_state_dict(model.state_dict(), policy=policy)

def save_json(path, obj):
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(obj, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")

def checkpoint_contract_metadata(mode: str, activation: str, projected: bool = False):
    return {"version": V742_VERSION, "dazg_contract_mode": mode, "activation_contract": activation, "projected_before_save": bool(projected), "block_policy": dict(DEFAULT_QAHL_DAZG_BLOCKS), "block_policy_sha256": block_policy_sha256(), "notes": NOTES, "created_at": time.strftime("%Y-%m-%d %H:%M:%S")}

def selftest():
    t = _need_torch()
    conv = t.randn(8, 8, 3, 3)
    proj = project_conv_weight_to_dazg(conv, 4)
    assert dazg_contract_report_state_dict({"x.weight": proj}, {"x": 4})["contract_ok"]
    assert not dazg_contract_report_state_dict({"x.weight": conv}, {"x": 4})["contract_ok"]
    lin = t.randn(12, 8)
    lproj = project_linear_weight_to_dazg(lin, 4)
    assert dazg_contract_report_state_dict({"head.weight": lproj}, {"head": 4})["contract_ok"]
    return {"status": "pass", "version": V742_VERSION, "policy_sha256": block_policy_sha256()}

if __name__ == "__main__":
    print(json.dumps(selftest(), indent=2))
