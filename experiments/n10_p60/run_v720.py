#!/usr/bin/env python3
# DAZG-Orbit Project Source File
# Component: experiments/n10_p60/run_v720.py
# Purpose: Fail-closed experiment, verification, or Q16 oracle component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.

"""V721 bias-closed exact-chain orchestrator with visible progress and safe stops.\nDAZG_ORBIT_V743R8P72_DYNAMIC_SECURITY_REPORT"""
from __future__ import annotations

# [DAZG_ORBIT_V743R8P76_RUN_DEFAULTS_ACTIVE_20260630]
# P77 repair: this block must stay AFTER all __future__ imports.
# It only supplies lane defaults when the outer runner explicitly enables FORCE_DEFAULTS.
import os as _dazg_orbit_p76_os
if _dazg_orbit_p76_os.environ.get("DAZG_ORBIT_V743R8P76_FORCE_DEFAULTS", "0") == "1":
    _dazg_orbit_p76_os.environ.setdefault("DAZG_ORBIT_V743R8P70_ADAPTER_POLICY", "secure_shadow")
    _dazg_orbit_p76_os.environ.setdefault("DAZG_ORBIT_V743R8P69_ADAPTER_POLICY", "secure_shadow")
    _dazg_orbit_p76_os.environ.setdefault("DAZG_ORBIT_V743R8P76_Q32_CARRY_ONLY_SECURE_CANDIDATE", "1")
    _dazg_orbit_p76_os.environ.setdefault("DAZG_ORBIT_V743R8P77_Q32_LOW16_CARRY_CANDIDATE", "1")
    _dazg_orbit_p76_os.environ.setdefault("DAZG_ORBIT_V743R8P76_DISABLE_SECURITY_CLAIM", "1")
# [/DAZG_ORBIT_V743R8P76_RUN_DEFAULTS_ACTIVE_20260630]


import argparse
import json
import os
import re
import shutil
import socket
import statistics
import subprocess
import sys
import time
import traceback
from datetime import datetime
from pathlib import Path
from typing import Any

REQUIRED_PAYLOAD_KEYS = [
    "qahl_ref_input_n10.npy",
    "qahl_ref_logits_n10.npy",
    "qahl_ref_labels_n10.npy",
    "stem.0.weight",
    "stem.0.bias",
    "to_h8.main.0.weight",
    "to_h8.main.0.bias",
]
ENTRY_RE = re.compile(r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]*)"\s*\}')


def atomic_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + f".tmp.{os.getpid()}")
    tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")
    os.replace(tmp, path)


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text())
        return value if isinstance(value, dict) else {}
    except Exception:
        return {}


def mean_or_none(values: list[float | int | None]) -> float | None:
    clean = [float(v) for v in values if isinstance(v, (int, float))]
    return statistics.fmean(clean) if clean else None


def sum_or_none(values: list[float | int | None]) -> float | None:
    clean = [float(v) for v in values if isinstance(v, (int, float))]
    return sum(clean) if clean else None


def max_or_none(values: list[float | int | None]) -> float | None:
    clean = [float(v) for v in values if isinstance(v, (int, float))]
    return max(clean) if clean else None


def display_value(value: Any, unavailable: str = "not_measured_due_to_gate") -> str:
    if value is None:
        return unavailable
    if isinstance(value, float):
        return f"{value:.6f}"
    return str(value)


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def parse_inventory(path: Path) -> dict[str, Path]:
    text = path.read_text()
    return {key: Path(payload) for key, payload, _ in ENTRY_RE.findall(text)}


class Runner:
    def __init__(self, args: argparse.Namespace) -> None:
        self.root = Path(args.root).resolve()
        self.test_dir = Path(__file__).resolve().parent
        self.watchdog = self.test_dir / "watchdog.py"
        self.pair_runner = self.test_dir / "pair_runner.py"
        self.scorer = self.test_dir / "score_v720.py"
        self.inventory = self.test_dir / "src" / "qahl_v645_src" / "qahl_v645_inventory.hpp"
        self.manifest_path = self.test_dir / "install_manifest.json"
        self.manifest = load_json(self.manifest_path)
        self.binary = Path(self.manifest.get("binary", self.root / "build/v720_bin/qahl_v720_to_h8_exact_executor")).resolve()
        self.marker = str(self.manifest.get("source_marker", ""))
        self.bias_marker = str(self.manifest.get("bias_closure_marker", ""))
        self.idle_timeout = int(os.getenv("DAZG_ORBIT_V720_IDLE_TIMEOUT", str(args.idle_timeout)))
        self.protocol_timeout = int(os.getenv("DAZG_ORBIT_V720_PROTOCOL_TIMEOUT", str(args.protocol_timeout)))
        self.score_timeout = int(os.getenv("DAZG_ORBIT_V720_SCORE_TIMEOUT", str(args.score_timeout)))
        self.overall_timeout = int(os.getenv("DAZG_ORBIT_V720_TOTAL_TIMEOUT", str(args.overall_timeout)))
        self.started_monotonic = time.monotonic()
        self.run_id = datetime.now().strftime("%Y%m%d_%H%M%S_%f")

        requested_base = os.getenv("DAZG_ORBIT_V720_OUTPUT_BASE")
        if requested_base:
            self.output_base = Path(requested_base).expanduser().resolve()
        else:
            self.output_base = self.root / "runs" / "n10"
        self.output_base.mkdir(parents=True, exist_ok=True)
        self.out_dir = self.output_base / f"v721_bias_closed_exact_chain_{self.run_id}"
        self.out_dir.mkdir(parents=True, exist_ok=False)
        self.progress_path = self.out_dir / "v721_progress.json"
        self.latest_progress = self.output_base / "v721_latest_progress.json"
        self.latest_run = self.output_base / "v721_latest_run.json"
        self.final_report_path = self.out_dir / "final_report.json"
        self.events: list[dict[str, Any]] = []
        self.results: dict[str, Any] = {}
        self.stage_counter = 0
        self.current_stage = "startup"
        self.failure_reason: str | None = None
        # DAZG_ORBIT_V7403_ACCURACY_GATE_BEGIN
        self.accuracy_floor_top1 = int(os.getenv("DAZG_ORBIT_V7403_ACCURACY_FLOOR_TOP1", "6"))
        self.accuracy_floor_top5 = int(os.getenv("DAZG_ORBIT_V7403_ACCURACY_FLOOR_TOP5", "8"))
        self.accuracy_total_samples = int(os.getenv("DAZG_ORBIT_V7403_ACCURACY_TOTAL_SAMPLES", "10"))
        self.accuracy_gate_enabled = os.getenv("DAZG_ORBIT_V7403_ACCURACY_GATE", "1").lower() not in ("0", "false", "no")
        self.accuracy_gate_stop: dict[str, Any] | None = None
        # DAZG_ORBIT_V7403_ACCURACY_GATE_END

        atomic_json(
            self.latest_run,
            {
                "run_id": self.run_id,
                "out_dir": str(self.out_dir),
                "progress": str(self.progress_path),
                "final_report": str(self.final_report_path),
                "status": "running",
            },
        )

    def elapsed(self) -> float:
        return time.monotonic() - self.started_monotonic

    def check_overall_deadline(self) -> None:
        if self.elapsed() >= self.overall_timeout:
            raise TimeoutError(
                f"overall timeout reached: {self.elapsed():.1f}s >= {self.overall_timeout}s"
            )

    def progress(self, stage: str, message: str, status: str = "running", **extra: Any) -> None:
        self.current_stage = stage
        event = {
            "time_unix": time.time(),
            "elapsed_s": round(self.elapsed(), 3),
            "stage": stage,
            "status": status,
            "message": message,
        }
        event.update(extra)
        self.events.append(event)
        data = {
            "schema": "dazg_orbit.qahl.v721.progress",
            "run_id": self.run_id,
            "status": status,
            "stage": stage,
            "message": message,
            "elapsed_s": round(self.elapsed(), 3),
            "source_marker": self.marker,
            "binary": str(self.binary),
            "out_dir": str(self.out_dir),
            "timeouts": {
                "idle_s": self.idle_timeout,
                "protocol_s": self.protocol_timeout,
                "score_s": self.score_timeout,
                "overall_s": self.overall_timeout,
            },
            "events": self.events[-40:],
        }
        atomic_json(self.progress_path, data)
        atomic_json(self.latest_progress, data)
        print(
            f"[v721-progress] stage={stage} status={status} "
            f"elapsed_s={self.elapsed():.1f} message={message}",
            flush=True,
        )

    def run_watchdog(
        self,
        stage: str,
        command: list[str],
        out_dir: Path,
        total: int,
        idle: int | None = None,
    ) -> int:
        self.check_overall_deadline()
        remaining = max(1, int(self.overall_timeout - self.elapsed()))
        effective_total = min(int(total), remaining)
        out_dir.mkdir(parents=True, exist_ok=True)
        log = out_dir / f"{stage}.log"
        heartbeat = out_dir / f"{stage}_heartbeat.json"
        cmd = [
            sys.executable,
            str(self.watchdog),
            "--stage", stage,
            "--log", str(log),
            "--heartbeat-json", str(heartbeat),
            "--total", str(effective_total),
            "--idle", str(idle if idle is not None else self.idle_timeout),
            "--heartbeat", "10",
            "--cwd", str(self.root),
            "--",
            *command,
        ]
        self.progress(stage, f"starting: {' '.join(command[:4])}")
        rc = subprocess.run(cmd, cwd=self.root).returncode
        hb = load_json(heartbeat)
        self.progress(
            stage,
            f"finished rc={rc}; last={hb.get('last_line')}",
            status="completed" if rc == 0 else "controlled_stop",
            exit_code=rc,
            heartbeat=hb,
        )
        return rc

    def preflight(self) -> None:
        self.progress("preflight", "checking fresh binary, marker, payloads, and Python runtime")
        for path in (self.watchdog, self.pair_runner, self.scorer, self.inventory):
            if not path.is_file():
                raise FileNotFoundError(path)
        if not self.binary.is_file() or not os.access(self.binary, os.X_OK):
            raise FileNotFoundError(f"active v720 binary missing: {self.binary}")
        if not self.marker.startswith("DAZG_ORBIT_V720_FRESH_"):
            raise RuntimeError(f"invalid or missing source marker: {self.marker!r}")
        if not self.bias_marker.startswith("DAZG_ORBIT_V721_BIAS_CLOSURE_ACTIVE_"):
            raise RuntimeError(
                f"invalid or missing v721 bias-closure marker: {self.bias_marker!r}"
            )
        strings = subprocess.check_output(["strings", str(self.binary)], text=True, errors="replace")
        if self.marker not in strings:
            raise RuntimeError("fresh marker is absent from active binary")
        if self.bias_marker not in strings:
            raise RuntimeError("v721 bias-closure marker is absent from active binary")
        if "DAZG_ORBIT_V720_FRESH_MARKER_PLACEHOLDER" in strings:
            raise RuntimeError("placeholder marker remains in active binary")
        if "DAZG_ORBIT_V711_ACTIVE_BINARY" in strings:
            raise RuntimeError("stale v711 executor marker found in v720 binary")
        entries = parse_inventory(self.inventory)
        missing = [
            f"{key}: {entries.get(key)}"
            for key in REQUIRED_PAYLOAD_KEYS
            if key not in entries or not entries[key].is_file()
        ]
        if missing:
            raise FileNotFoundError("missing required payloads:\n" + "\n".join(missing))
        import numpy  # noqa: F401
        self.results["fresh_binary"] = {
            "source_marker": self.marker,
            "bias_closure_marker": self.bias_marker,
            "binary_marker_match": True,
            "bias_closure_marker_match": True,
            "placeholder_absent": True,
            "stale_v711_absent": True,
            "binary": str(self.binary),
            "binary_sha256": self.manifest.get("binary_sha256"),
            "link_line": self.manifest.get("link_line"),
            "build_log": self.manifest.get("build_log"),
        }
        self.progress("preflight", "fresh binary and all required payloads verified", status="completed")

    def scorer_selftest(self) -> None:
        stage_dir = self.out_dir / "00_scorer_selftest"
        out = stage_dir / "scorer_selftest.json"
        rc = self.run_watchdog(
            "scorer_selftest",
            [sys.executable, str(self.scorer), "selftest", "--out", str(out)],
            stage_dir,
            total=120,
            idle=60,
        )
        report = load_json(out)
        self.results["scorer_selftest"] = report
        if rc != 0 or report.get("status") != "pass":
            raise RuntimeError("scorer selftest failed")

    def run_pair_and_score(
        self,
        label: str,
        mode: str,
        sample: int,
        candidate: str,
        kind: str,
    ) -> dict[str, Any]:
        self.stage_counter += 1
        stage_dir = self.out_dir / f"{self.stage_counter:02d}_{label}"
        pair_dir = stage_dir / "pair"
        score_path = stage_dir / "score.json"
        port = free_port()
        pair_cmd = [
            sys.executable,
            str(self.pair_runner),
            "--binary", str(self.binary),
            "--root", str(self.root),
            "--mode", mode,
            "--sample", str(sample),
            "--candidate", candidate,
            "--out-dir", str(pair_dir),
            "--port", str(port),
        ]
        pair_rc = self.run_watchdog(
            f"{label}_pair",
            pair_cmd,
            stage_dir,
            total=self.protocol_timeout,
        )
        if pair_rc != 0:
            heartbeat = load_json(stage_dir / f"{label}_pair_heartbeat.json")
            return {
                "status": "runtime_failed",
                "pair_exit_code": pair_rc,
                "heartbeat": heartbeat,
                "pair_dir": str(pair_dir),
                "strict_exact": False,
            }

        score_cmd = [
            sys.executable,
            str(self.scorer),
            "score",
            "--kind", kind,
            "--pair-dir", str(pair_dir),
            "--inventory", str(self.inventory),
            "--sample", str(sample),
            "--out", str(score_path),
        ]
        score_rc = self.run_watchdog(
            f"{label}_score",
            score_cmd,
            stage_dir,
            total=self.score_timeout,
        )
        report = load_json(score_path)
        report["pair_exit_code"] = pair_rc
        report["score_exit_code"] = score_rc
        report["stage_dir"] = str(stage_dir)
        if score_rc != 0 or not report:
            report.setdefault("status", "scorer_failed")
            report.setdefault("correctness", {"strict_exact": False})
        return report

    @staticmethod
    def strict(report: dict[str, Any]) -> bool:
        return bool(report.get("correctness", {}).get("strict_exact"))

    # DAZG_ORBIT_V7403_ACCURACY_GATE_BEGIN
    def accuracy_gate_state(self, reports: list[dict[str, Any]]) -> dict[str, Any]:
        total = max(1, int(self.accuracy_total_samples))
        floor_top1 = max(0, int(self.accuracy_floor_top1))
        floor_top5 = max(0, int(self.accuracy_floor_top5))
        accuracy_reports = [
            r for r in reports
            if isinstance(r, dict) and r.get("kind") == "full" and isinstance(r.get("correctness"), dict)
        ]
        evaluated = min(len(accuracy_reports), total)
        items = [r.get("correctness", {}) for r in accuracy_reports[:total]]
        top1 = sum(1 for x in items if x.get("top1_correct"))
        top5 = sum(1 for x in items if x.get("top5_correct"))
        remaining = max(0, total - evaluated)
        top1_possible = top1 + remaining
        top5_possible = top5 + remaining
        floor_ok = evaluated == total and top1 >= floor_top1 and top5 >= floor_top5
        impossible = top1_possible < floor_top1 or top5_possible < floor_top5
        return {
            "enabled": bool(self.accuracy_gate_enabled),
            "evaluated_samples": evaluated,
            "total_samples": total,
            "top1_correct": top1,
            "top5_correct": top5,
            "top1_floor": floor_top1,
            "top5_floor": floor_top5,
            "top1_possible": top1_possible,
            "top5_possible": top5_possible,
            "floor_ok": floor_ok,
            "impossible": impossible,
            "sample_top1": [x.get("got_top1") for x in items],
            "sample_top5": [x.get("got_top5") for x in items],
            "labels": [x.get("label") for x in items],
            "legacy_dense_top1": [x.get("legacy_dense_top1") for x in items],
            "note": "V740.3 treats exactness and trained accuracy as separate gates; DAZG/BFE shadow exactness alone is not an accuracy proof.",
        }

    def record_accuracy_gate_progress(self, reports: list[dict[str, Any]]) -> dict[str, Any]:
        state = self.accuracy_gate_state(reports)
        self.results["accuracy_gate_state"] = state
        self.progress(
            "accuracy_gate",
            f"samples={state['evaluated_samples']}/{state['total_samples']} "
            f"top1={state['top1_correct']}/{state['top1_floor']} "
            f"top5={state['top5_correct']}/{state['top5_floor']} "
            f"possible_top1={state['top1_possible']} "
            f"possible_top5={state['top5_possible']} "
            f"floor_ok={state['floor_ok']} impossible={state['impossible']}",
            status="completed" if state["floor_ok"] else "running",
            accuracy_gate=state,
        )
        return state
    # DAZG_ORBIT_V7403_ACCURACY_GATE_END

    @staticmethod
    def numeric_to_h8(report: dict[str, Any]) -> bool:
        """Baseline route still must pass protocol and reference-safety gates."""
        return bool(
            report.get("correctness", {}).get("numeric_exact") and
            report.get("protocol_gate", {}).get("ok") is True and
            report.get("reference_safety", {}).get("reference_echo_safe") is True
        )

    @staticmethod
    def route_cost_key(candidate: str, record: dict[str, Any]) -> tuple[Any, ...]:
        cost = record.get("full_sample0", {}).get("cost", {})
        def number(key: str) -> float:
            value = cost.get(key)
            return float(value) if isinstance(value, (int, float)) else float("inf")
        return (
            number("rotate_rows"),
            number("hecompute_us"),
            number("communication_bytes_total"),
            number("pair_wall_s"),
            candidate,
        )

    def run_chain(self) -> None:
        self.preflight()
        self.scorer_selftest()

        first = self.run_pair_and_score(
            "firstconv_s0", "first-conv", 0, "off", "firstconv"
        )
        self.results["firstconv"] = first
        if not self.strict(first):
            self.failure_reason = "firstconv_not_exact_after_bias_closure"
            return

        route_reports: dict[str, Any] = {}
        candidates = ("off", "main0_pcoi", "main0_pcoi_skip_colayout")
        for candidate in candidates:
            to_h8 = self.run_pair_and_score(
                f"{candidate}_to_h8_s0",
                "to-h8-substage",
                0,
                candidate,
                "to_h8",
            )
            record: dict[str, Any] = {"to_h8_sample0": to_h8}
            if candidate == "off":
                to_h8_gate = self.numeric_to_h8(to_h8)
                record["to_h8_gate_kind"] = "numeric_exact_baseline"
                record["to_h8_contract_required"] = False
            else:
                to_h8_gate = self.strict(to_h8)
                record["to_h8_gate_kind"] = "numeric_and_pcoi_contract_exact"
                record["to_h8_contract_required"] = True
            record["to_h8_gate_pass"] = to_h8_gate

            if to_h8_gate:
                full0 = self.run_pair_and_score(
                    f"{candidate}_full_s0",
                    "full-sample",
                    0,
                    candidate,
                    "full",
                )
                record["full_sample0"] = full0
                record["route_exact"] = self.strict(full0)
            else:
                record["route_exact"] = False
            route_reports[candidate] = record

        self.results["routes"] = route_reports
        eligible = [
            candidate
            for candidate, record in route_reports.items()
            if record.get("route_exact") is True
        ]
        chosen = min(
            eligible,
            key=lambda name: self.route_cost_key(name, route_reports[name]),
        ) if eligible else None
        self.results["chosen_candidate"] = chosen
        self.results["selection_objective"] = [
            "strict exactness required",
            "minimum rotations",
            "minimum HE compute us",
            "minimum communication bytes",
            "minimum wall seconds",
        ]
        if chosen is None:
            self.failure_reason = "no_route_reached_exact_fullgraph_sample0"
            return

        # DAZG_ORBIT_V7403_ACCURACY_GATE_BEGIN
        full_scores: list[dict[str, Any]] = [
            route_reports[chosen]["full_sample0"]
        ]
        self.results["full_n10_scores"] = full_scores
        gate_state = self.record_accuracy_gate_progress(full_scores)
        if self.accuracy_gate_enabled and gate_state.get("impossible"):
            self.failure_reason = "trained_accuracy_floor_impossible_after_sample_0"
            self.accuracy_gate_stop = gate_state
            return

        for sample in range(1, self.accuracy_total_samples):
            report = self.run_pair_and_score(
                f"{chosen}_full_s{sample}",
                "full-sample",
                sample,
                chosen,
                "full",
            )
            full_scores.append(report)
            self.results["full_n10_scores"] = full_scores
            if not self.strict(report):
                self.failure_reason = f"full_n10_not_exact_sample_{sample}"
                break
            gate_state = self.record_accuracy_gate_progress(full_scores)
            if self.accuracy_gate_enabled and gate_state.get("impossible"):
                self.failure_reason = f"trained_accuracy_floor_impossible_after_sample_{sample}"
                self.accuracy_gate_stop = gate_state
                break

        final_gate_state = self.accuracy_gate_state(full_scores)
        self.results["accuracy_gate_state"] = final_gate_state
        if (
            len(full_scores) == self.accuracy_total_samples and
            all(self.strict(x) for x in full_scores)
        ):
            if (not self.accuracy_gate_enabled) or final_gate_state.get("floor_ok"):
                self.failure_reason = None
            else:
                self.failure_reason = "trained_accuracy_floor_not_preserved"
                self.accuracy_gate_stop = final_gate_state
        elif self.failure_reason is None:
            self.failure_reason = "full_n10_incomplete"
        # DAZG_ORBIT_V7403_ACCURACY_GATE_END

    def build_final_report(self, infrastructure_error: str | None = None) -> dict[str, Any]:
        first = self.results.get("firstconv", {})
        routes = self.results.get("routes", {})
        chosen = self.results.get("chosen_candidate")

        def depth(record: dict[str, Any]) -> tuple[int, int, int]:
            return (
                1 if record.get("full_sample0") else 0,
                1 if record.get("to_h8_sample0") else 0,
                1 if record.get("route_exact") else 0,
            )

        diagnostic_candidate = chosen
        if diagnostic_candidate is None and isinstance(routes, dict) and routes:
            diagnostic_candidate = max(routes, key=lambda name: depth(routes[name]))
        diagnostic_record = (
            routes.get(diagnostic_candidate, {})
            if diagnostic_candidate and isinstance(routes, dict)
            else {}
        )
        diagnostic_to_h8 = diagnostic_record.get("to_h8_sample0", {})
        diagnostic_full0 = diagnostic_record.get("full_sample0", {})
        n10 = [x for x in self.results.get("full_n10_scores", []) if isinstance(x, dict)]
        n10_started = bool(n10)
        n10_exact = len(n10) == 10 and all(self.strict(x) for x in n10)

        if n10:
            metric_reports = n10
            measurement_stage = "full_n10"
        elif diagnostic_full0:
            metric_reports = [diagnostic_full0]
            measurement_stage = "full_sample0"
        elif diagnostic_to_h8:
            metric_reports = [diagnostic_to_h8]
            measurement_stage = "to_h8_sample0"
        elif first:
            metric_reports = [first]
            measurement_stage = "firstconv_sample0"
        else:
            metric_reports = []
            measurement_stage = "not_reached"
        metric_reports = [r for r in metric_reports if isinstance(r.get("cost"), dict)]
        costs = [r.get("cost", {}) for r in metric_reports]

        if n10:
            accuracy_reports = [
                r for r in n10
                if r.get("kind") == "full" and isinstance(r.get("correctness"), dict)
            ]
        elif diagnostic_full0 and diagnostic_full0.get("kind") == "full":
            accuracy_reports = [diagnostic_full0]
        else:
            accuracy_reports = []
        accuracy_items = [r.get("correctness", {}) for r in accuracy_reports]
        top1_count = sum(1 for x in accuracy_items if x.get("top1_correct"))
        top5_count = sum(1 for x in accuracy_items if x.get("top5_correct"))
        accuracy_floor_ok = (
            len(accuracy_items) == self.accuracy_total_samples and
            top1_count >= self.accuracy_floor_top1 and
            top5_count >= self.accuracy_floor_top5
        )
        accuracy_gate_state = self.results.get("accuracy_gate_state")
        if not isinstance(accuracy_gate_state, dict):
            accuracy_gate_state = self.accuracy_gate_state(accuracy_reports)

        audit_reports: list[dict[str, Any]] = []
        seen: set[str] = set()
        candidates_to_audit: list[dict[str, Any]] = [first]
        for record in routes.values() if isinstance(routes, dict) else []:
            candidates_to_audit.extend([
                record.get("to_h8_sample0", {}),
                record.get("full_sample0", {}),
            ])
        candidates_to_audit.extend(n10)
        for report_item in candidates_to_audit:
            if not isinstance(report_item, dict) or not report_item:
                continue
            key = str(report_item.get("stage_dir") or report_item.get("pair_dir") or id(report_item))
            if key in seen:
                continue
            seen.add(key)
            audit_reports.append(report_item)

        reference_flags = [
            r.get("reference_safety", {}).get("reference_echo_safe")
            for r in audit_reports
            if isinstance(r.get("reference_safety"), dict)
        ]
        protocol_flags = [
            r.get("protocol_gate", {}).get("ok")
            for r in audit_reports
            if isinstance(r.get("protocol_gate"), dict)
        ]
        fallback_values = [
            r.get("protocol", {}).get("algorithm_fallback_count")
            for r in audit_reports
            if isinstance(r.get("protocol"), dict)
        ]
        reference_echo_safe = bool(reference_flags) and all(x is True for x in reference_flags)
        all_protocol_gates_ok = bool(protocol_flags) and all(x is True for x in protocol_flags)
        fallback_count = max_or_none(fallback_values)
        reveal_oracle_used = any(
            bool(r.get("security", {}).get("reveal_oracle_used"))
            for r in audit_reports
            if isinstance(r.get("security"), dict)
        )
        # DAZG_ORBIT_V743R8P72_DYNAMIC_SECURITY_REPORT_BEGIN
        # Audit reports are evidence, not reveal-oracle usage by themselves.
        # Claim is allowed only after N=10 exactness + accuracy floor + all protocol/reference/fallback gates.
        # DAZG_ORBIT_V743R8P73_SECURITY_GUARD_BEGIN
        # P73 pair-carry exact bridge is diagnostic. It closes the Q32 truncation
        # carry/wrap blocker, but it is not a paper-grade secure trunc primitive.
        p73_q32_pair_carry_diagnostic = (
            os.environ.get("DAZG_ORBIT_V743R8P73_Q32_CARRY_MODE", "")
            in {"1", "pair_exact", "pair_exact_diagnostic"}
        )
        # DAZG_ORBIT_V743R8P73_SECURITY_GUARD_END
        # DAZG_ORBIT_V743R8P74_SECURITY_GUARD_BEGIN
        # P74 nonlinear pair-exact bridge is diagnostic. It reconstructs
        # nonlinear adapter inputs to close exactness blockers quickly, but it
        # is not a paper-grade secure GeLU/bucket/avgpool primitive.
        p74_nonlinear_pair_diagnostic = (
            os.environ.get("DAZG_ORBIT_V743R8P74_NONLINEAR_BRIDGE_MODE", "")
            in {"1", "pair_exact", "pair_exact_diagnostic"}
        )
        # DAZG_ORBIT_V743R8P74_SECURITY_GUARD_END
        # DAZG_ORBIT_V743R8P77_SECURITY_GUARD_BEGIN
        # P77 low16 carry candidate exchanges only low16 remainders, not full q32,
        # but it is still a candidate adapter, not a paper-grade secure trunc primitive.
        p77_low16_carry_candidate = (
            os.environ.get("DAZG_ORBIT_V743R8P77_Q32_LOW16_CARRY_CANDIDATE", "")
            in {"1", "true", "TRUE", "yes", "on"} or
            os.environ.get("DAZG_ORBIT_V743R8P76_Q32_CARRY_ONLY_SECURE_CANDIDATE", "")
            in {"1", "true", "TRUE", "yes", "on"}
        )
        # DAZG_ORBIT_V743R8P77_SECURITY_GUARD_END
        # DAZG_ORBIT_V743R8P80_SECURITY_GUARD_BEGIN
        p80_q32_wrap_q16_candidate = (
            os.environ.get("DAZG_ORBIT_V743R8P80_Q32_WRAP_Q16_CANDIDATE", "")
            in {"1", "true", "TRUE", "yes", "YES", "on", "ON"}
        )
        # DAZG_ORBIT_V743R8P80_SECURITY_GUARD_END
        no_reveal_or_fallback = (
            reveal_oracle_used is False and
            fallback_count == 0 and
            all_protocol_gates_ok and
            reference_echo_safe and
            not p73_q32_pair_carry_diagnostic and
            not p74_nonlinear_pair_diagnostic and
            not p77_low16_carry_candidate and
            not p80_q32_wrap_q16_candidate
        )
        # DAZG_ORBIT_V743R8P72_DYNAMIC_SECURITY_REPORT_END

        if infrastructure_error:
            status = "infrastructure_error"
        elif self.failure_reason:
            status = "controlled_stop_" + self.failure_reason
        elif n10_exact and accuracy_floor_ok:
            status = "diagnostic_n10_exact_accuracy_floor_ok"
        elif n10_exact:
            status = "controlled_stop_trained_accuracy_floor_not_preserved"
        else:
            status = "controlled_stop_incomplete"

        first_correct = first.get("correctness", {})
        to_h8_correct = diagnostic_to_h8.get("correctness", {})
        full0_correct = diagnostic_full0.get("correctness", {})
        route_layout = to_h8_correct.get("packing_layout", {})
        if not route_layout and diagnostic_to_h8:
            dcost = diagnostic_to_h8.get("cost", {})
            route_layout = {
                "main0_route": dcost.get("to_h8_route"),
                "main0_runtime": dcost.get("to_h8_main0_runtime"),
                "skip_runtime": dcost.get("to_h8_skip_runtime"),
            }
        protocol_source = first.get("protocol", {})
        if not protocol_source:
            protocol_source = diagnostic_to_h8.get("protocol", {}) or diagnostic_full0.get("protocol", {})

        route_summary: dict[str, Any] = {}
        if isinstance(routes, dict):
            for name, record in routes.items():
                th = record.get("to_h8_sample0", {}).get("correctness", {})
                f0 = record.get("full_sample0", {}).get("correctness", {})
                route_summary[name] = {
                    "to_h8_gate_kind": record.get("to_h8_gate_kind"),
                    "to_h8_numeric_exact": (
                        bool(th.get("numeric_exact")) if th else None
                    ),
                    "to_h8_contract_exact": (
                        bool(th.get("strict_exact"))
                        if th and record.get("to_h8_contract_required")
                        else None
                    ),
                    "full_sample0_exact": bool(f0.get("strict_exact")) if f0 else None,
                    "route_exact": bool(record.get("route_exact")),
                    "cost": record.get("full_sample0", {}).get("cost"),
                }

        report = {
            "schema": "dazg_orbit.qahl.v721.final_report",
            "status": status,
            "evidence_grade": "C-infrastructure" if infrastructure_error else "A-runtime-diagnostic",
            "run_id": self.run_id,
            "started_unix": self.events[0]["time_unix"] if self.events else None,
            "finished_unix": time.time(),
            "wall_s": round(self.elapsed(), 6),
            "out_dir": str(self.out_dir),
            "failure_reason": self.failure_reason,
            "infrastructure_error": infrastructure_error,
            "fresh_binary": self.results.get("fresh_binary", {}),
            "scorer_selftest": self.results.get("scorer_selftest", {}),
            "bias_closure": {
                "marker": protocol_source.get("server_bias_closure_marker") or self.bias_marker,
                "contract_gate_ok": (
                    first.get("protocol_gate", {}).get("checks", {}).get(
                        "v721_bias_closure_contract"
                    ) if first else None
                ),
                "policy": "zero operator bias; Q32 weight-only floor; add raw Q16 bias exactly once on server share",
                "server_calls_firstconv": protocol_source.get("server_bias_closure_calls"),
                "client_calls_firstconv": protocol_source.get("client_bias_closure_calls"),
                "server_applied_calls_firstconv": protocol_source.get("server_bias_closure_applied_calls"),
                "client_applied_calls_firstconv": protocol_source.get("client_bias_closure_applied_calls"),
                "server_applied_elements_firstconv": protocol_source.get("server_bias_closure_applied_elements"),
            },
            "protocol": {
                "server_party": protocol_source.get("server_party"),
                "client_party": protocol_source.get("client_party"),
                "server_only_real_input": (
                    protocol_source.get("server_input_owner_ok") is True and
                    protocol_source.get("client_input_owner_ok") is True
                ),
                "client_input_zero": protocol_source.get("client_input_owner_ok") is True,
                "plain_mod": first.get("plain_mod") or protocol_source.get("plain_mod"),
                "all_runtime_protocol_gates_ok": all_protocol_gates_ok,
            },
            # DAZG_ORBIT_V741_DAZG_PAYLOAD_CONTRACT_REPORT_BEGIN
            "dazg_payload_contract": {
                "to_h8_main0": to_h8_correct.get("dazg_channel_orbit"),
                "paper_grade_payload_contract_ok": to_h8_correct.get("paper_grade_payload_contract_ok"),
                "projected_dazg_allowed_by_env": to_h8_correct.get("projected_dazg_allowed_by_env"),
                "gate_failures": to_h8_correct.get("gate_failures", []),
                "core_diagnosis": (
                    "raw trained payload is being reinterpreted as a DAZG first-column generator; "
                    "accuracy cannot be claimed until the checkpoint is trained/exported in this algebra "
                    "or the executor evaluates the dense weights"
                ),
            },
            # DAZG_ORBIT_V741_DAZG_PAYLOAD_CONTRACT_REPORT_END
            "correctness": {
                "reference_echo_safe": reference_echo_safe,
                "fallback_count": int(fallback_count) if fallback_count is not None else None,
                "firstconv_strict_exact": (
                    bool(first_correct.get("strict_exact")) if first else None
                ),
                "firstconv_neq": first_correct.get("metric", {}).get("neq") if first else None,
                "to_h8_numeric_exact": (
                    bool(to_h8_correct.get("numeric_exact")) if diagnostic_to_h8 else None
                ),
                "to_h8_contract_required": (
                    diagnostic_candidate != "off" if diagnostic_candidate is not None else None
                ),
                "to_h8_contract_exact": (
                    bool(to_h8_correct.get("strict_exact"))
                    if diagnostic_to_h8 and diagnostic_candidate != "off"
                    else None
                ),
                "to_h8_exact_contracts": to_h8_correct.get("exact_contracts", []) if diagnostic_to_h8 else [],
                "to_h8_neq": to_h8_correct.get("metric", {}).get("neq") if diagnostic_to_h8 else None,
                "to_h8_max_delta": to_h8_correct.get("metric", {}).get("max_abs_delta") if diagnostic_to_h8 else None,
                "to_h8_l1": to_h8_correct.get("metric", {}).get("l1") if diagnostic_to_h8 else None,
                "sample0_strict_exact": (
                    bool(full0_correct.get("strict_exact")) if diagnostic_full0 else None
                ),
                "full10_strict_exact": n10_exact if n10_started else None,
                "full10_samples_completed": len(n10),
                "chosen_candidate": chosen,
                "diagnostic_candidate": diagnostic_candidate,
                "attempted_candidates": list(routes.keys()) if isinstance(routes, dict) else [],
                "route_summary": route_summary,
            },
            "selection": {
                "chosen_candidate": chosen,
                "objective": self.results.get("selection_objective", []),
                "exactness_is_mandatory": True,
            },
            "security": {
                "reveal_oracle_used": reveal_oracle_used,
                "security_claim": 1 if (n10_exact and accuracy_floor_ok and no_reveal_or_fallback) else 0,
                "security_claim_reason": (
                    "P72_N10_exact_accuracy_floor_no_reveal_no_fallback"
                    if (n10_exact and accuracy_floor_ok and no_reveal_or_fallback)
                    else "needs_N10_exact_accuracy_floor_no_reveal_no_fallback_no_diagnostic_bridges"
                ),
                "note": "P73/P74 diagnostic bridges plus P77/P80 q32 trunc candidates are claim-blocked. Claim requires N=10 exactness, trained accuracy floor, protocol gates, reference safety, fallback_count=0, and no diagnostic/candidate carry/wrap or nonlinear bridge.",
            },
            "cost": {
                "measurement_stage": measurement_stage,
                "sample_count": len(costs),
                "wall_s_per_sample": mean_or_none([x.get("pair_wall_s") for x in costs]),
                "hecompute_us_per_sample": mean_or_none([x.get("hecompute_us") for x in costs]),
                "rotations_per_sample": mean_or_none([x.get("rotate_rows") for x in costs]),
                "mul_plain_per_sample": mean_or_none([x.get("mul_plain") for x in costs]),
                "add_inplace_per_sample": mean_or_none([x.get("add_inplace") for x in costs]),
                "pack_weight_us_per_sample": mean_or_none([x.get("pack_weight_us") for x in costs]),
                "communication_bytes_per_sample": mean_or_none([
                    x.get("communication_bytes_total") for x in costs
                ]),
                "rounds_per_sample": mean_or_none([
                    x.get("network_rounds_per_pair") for x in costs
                ]),
                "rotations_total": sum_or_none([x.get("rotate_rows") for x in costs]),
                "hecompute_us_total": sum_or_none([x.get("hecompute_us") for x in costs]),
                "communication_bytes_total": sum_or_none([
                    x.get("communication_bytes_total") for x in costs
                ]),
                "rounds_total": sum_or_none([
                    x.get("network_rounds_per_pair") for x in costs
                ]),
            },
            "packing": {
                "to_h8_main0": route_layout.get("main0_route"),
                "to_h8_main0_runtime": route_layout.get("main0_runtime"),
                "to_h8_skip_runtime": route_layout.get("skip_runtime"),
            },
            "accuracy": {
                "evaluated_samples": len(accuracy_items),
                "top1_correct": top1_count,
                "top5_correct": top5_count,
                "top1": (top1_count / len(accuracy_items)) if accuracy_items else None,
                "top5": (top5_count / len(accuracy_items)) if accuracy_items else None,
                "floor_top1_correct_n10": self.accuracy_floor_top1,
                "floor_top5_correct_n10": self.accuracy_floor_top5,
                "floor_ok": accuracy_floor_ok if n10_started else None,
                "reference_exact_preserves_accuracy": accuracy_floor_ok if n10_started else None,
            },
            "accuracy_gate": {
                "enabled": self.accuracy_gate_enabled,
                "state": accuracy_gate_state,
                "stop": self.accuracy_gate_stop,
                "contract_note": "The current optimized DAZG/BFE shadow path can be exact while failing the trained dense/GELU accuracy floor; V740.3 stops instead of reporting a false accuracy pass.",
            },
            "timeouts": {
                "idle_s": self.idle_timeout,
                "protocol_s": self.protocol_timeout,
                "score_s": self.score_timeout,
                "overall_s": self.overall_timeout,
                "kills_only_child_process_groups": True,
                "wsl_shell_is_never_targeted": True,
            },
            "artifacts": {
                "progress": str(self.progress_path),
                "final_report": str(self.final_report_path),
                "summary": str(self.out_dir / "final_summary.txt"),
            },
            "raw_results": self.results,
        }
        return report

    @staticmethod
    def gate_text(value: Any) -> str:
        if value is None:
            return "NOT RUN"
        return "PASS" if value is True else "FAIL"

    def print_summary(self, report: dict[str, Any]) -> None:
        c = report["correctness"]
        cost = report["cost"]
        packing = report["packing"]
        acc = report["accuracy"]
        bias = report["bias_closure"]
        dazg_contract = report.get("dazg_payload_contract", {}) or {}
        dazg_main0 = dazg_contract.get("to_h8_main0", {}) or {}
        main0 = packing.get("to_h8_main0") or {}
        main0_rt = packing.get("to_h8_main0_runtime") or {}
        skip_rt = packing.get("to_h8_skip_runtime") or {}
        contract_text = (
            "N/A (baseline route off)"
            if c.get("to_h8_contract_required") is False
            else self.gate_text(c.get("to_h8_contract_exact"))
        )
        lines = [
            "",
            "==================== V721 FINAL RESULTS ====================",
            f"status                     : {report['status']}",
            f"failure reason             : {report.get('failure_reason') or 'none'}",
            f"fresh binary marker        : {report['fresh_binary'].get('source_marker')}",
            f"bias closure marker        : {bias.get('marker')}",
            f"bias closure contract      : {self.gate_text(bias.get('contract_gate_ok'))}",
            f"bias closure firstconv     : server_calls={display_value(bias.get('server_calls_firstconv'))} server_applied={display_value(bias.get('server_applied_calls_firstconv'))} elements={display_value(bias.get('server_applied_elements_firstconv'))} client_applied={display_value(bias.get('client_applied_calls_firstconv'))}",
            f"protocol gates             : {self.gate_text(report['protocol'].get('all_runtime_protocol_gates_ok'))}",
            f"reference echo safe        : {self.gate_text(c.get('reference_echo_safe'))}",
            f"fallback count             : {display_value(c.get('fallback_count'))}",
            f"firstconv strict exact     : {self.gate_text(c.get('firstconv_strict_exact'))}  neq={display_value(c.get('firstconv_neq'))}",
            f"to_h8 numeric exact        : {self.gate_text(c.get('to_h8_numeric_exact'))}  neq={display_value(c.get('to_h8_neq'))} max={display_value(c.get('to_h8_max_delta'))} L1={display_value(c.get('to_h8_l1'))}",
            f"to_h8 optimized contract   : {contract_text}  contracts={c.get('to_h8_exact_contracts')}",
            f"DAZG payload contract      : {self.gate_text(dazg_contract.get('paper_grade_payload_contract_ok'))}  raw_vs_effective_neq={display_value(dazg_main0.get('payload_vs_effective_neq'))}/{display_value(dazg_main0.get('payload_vs_effective_total'))}",
            f"DAZG payload core issue    : {', '.join(dazg_contract.get('gate_failures') or []) or 'none'}",
            f"fullgraph sample0 exact    : {self.gate_text(c.get('sample0_strict_exact'))}",
            f"fullgraph N=10 exact       : {self.gate_text(c.get('full10_strict_exact'))}  completed={c.get('full10_samples_completed')}/10",
            f"chosen route candidate     : {c.get('chosen_candidate') or 'none'}",
            f"diagnostic candidate       : {c.get('diagnostic_candidate') or 'none'}",
            f"measurement stage          : {cost.get('measurement_stage')}",
            f"wall seconds / sample      : {display_value(cost.get('wall_s_per_sample'))}",
            f"rotations / sample         : {display_value(cost.get('rotations_per_sample'))}",
            f"HE compute us / sample     : {display_value(cost.get('hecompute_us_per_sample'))}",
            f"mul_plain / sample         : {display_value(cost.get('mul_plain_per_sample'))}",
            f"add_inplace / sample       : {display_value(cost.get('add_inplace_per_sample'))}",
            f"PackWeight us / sample     : {display_value(cost.get('pack_weight_us_per_sample'))}",
            f"communication bytes/sample : {display_value(cost.get('communication_bytes_per_sample'))}",
            f"network rounds / sample    : {display_value(cost.get('rounds_per_sample'))}",
            f"main0 packing              : layout={display_value(main0.get('layout_mode'))} pcoi={display_value(main0.get('pcoi_k3s2'))} compact={display_value(main0.get('compact_k3s2'))} block={display_value(main0.get('block_size'))} ntt={display_value(main0.get('ntt_size'))} capacity_ok={display_value(main0.get('capacity_ok'))}",
            f"main0 HE schedule          : {display_value(main0_rt.get('schedule'))}",
            f"skip packing               : layout={display_value(skip_rt.get('layout_mode'))} schedule={display_value(skip_rt.get('schedule'))}",
            f"accuracy top1 / top5       : {display_value(acc.get('top1'))} / {display_value(acc.get('top5'))}  samples={acc.get('evaluated_samples')}",
            f"trained accuracy floor     : {self.gate_text(acc.get('floor_ok'))}  top1>={acc.get('floor_top1_correct_n10')}/10 top5>={acc.get('floor_top5_correct_n10')}/10",
            "contract guard             : V741 refuses projected DAZG weights as paper-grade accuracy evidence unless explicitly allowed",
            f"security claim             : {report['security'].get('security_claim')} ({report['security'].get('security_claim_reason')})",
            f"idle / total timeout       : {report['timeouts'].get('idle_s')}s / {report['timeouts'].get('overall_s')}s; child process groups only",
            f"final report               : {self.final_report_path}",
            f"all logs                   : {self.out_dir}",
            "============================================================",
            "",
        ]
        text = "\n".join(lines)
        print(text, flush=True)
        (self.out_dir / "final_summary.txt").write_text(text)

    def finish(self, infrastructure_error: str | None = None) -> None:
        report = self.build_final_report(infrastructure_error)
        atomic_json(self.final_report_path, report)
        status = "completed" if infrastructure_error is None else "controlled_stop"
        self.progress("final", report["status"], status=status, final_report=str(self.final_report_path))
        latest = load_json(self.latest_run)
        latest.update({"status": report["status"], "finished_unix": time.time()})
        atomic_json(self.latest_run, latest)
        self.print_summary(report)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--idle-timeout", type=int, default=120)
    parser.add_argument("--protocol-timeout", type=int, default=900)
    parser.add_argument("--score-timeout", type=int, default=600)
    parser.add_argument("--overall-timeout", type=int, default=10800)
    args = parser.parse_args()
    runner = Runner(args)
    print(f"[v721-run] output={runner.out_dir}", flush=True)
    print(f"[v721-run] live_progress={runner.latest_progress}", flush=True)
    try:
        runner.run_chain()
        runner.finish()
        # Controlled scientific stops still return zero because final_report records the gate failure.
        return 0
    except KeyboardInterrupt:
        runner.failure_reason = "interrupted"
        runner.finish("keyboard_interrupt")
        return 130
    except Exception as exc:
        error = f"{type(exc).__name__}: {exc}"
        runner.failure_reason = runner.failure_reason or "infrastructure_error"
        (runner.out_dir / "orchestrator_error.txt").write_text(
            error + "\n\n" + traceback.format_exc()
        )
        print(f"[v721-controlled-error] {error}", file=sys.stderr, flush=True)
        runner.finish(error)
        # The run completed with an auditable report; do not terminate the WSL shell.
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
