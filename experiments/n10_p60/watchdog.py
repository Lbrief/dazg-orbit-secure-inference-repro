#!/usr/bin/env python3
# DAZG-Orbit Project Source File
# Component: experiments/n10_p60/watchdog.py
# Purpose: Fail-closed experiment, verification, or Q16 oracle component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
"""Run one command with visible heartbeats and safe child-process-group timeouts."""
from __future__ import annotations

import argparse
import codecs
import json
import os
import queue
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any


def atomic_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + f".tmp.{os.getpid()}")
    tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")
    os.replace(tmp, path)


def shorten(line: str, limit: int = 280) -> str:
    line = " ".join(line.strip().split())
    if len(line) <= limit:
        return line
    return line[: limit - 3] + "..."


def kill_process_group(proc: subprocess.Popen[bytes], grace: float = 8.0) -> None:
    if proc.poll() is not None:
        return
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    deadline = time.monotonic() + grace
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return
        time.sleep(0.1)
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def run(args: argparse.Namespace) -> int:
    command = list(args.command)
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        raise SystemExit("watchdog: missing command after --")

    log_path = Path(args.log).resolve()
    heartbeat_path = Path(args.heartbeat_json).resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    heartbeat_path.parent.mkdir(parents=True, exist_ok=True)

    start = time.monotonic()
    last_output = start
    last_heartbeat = 0.0
    last_line = "command started"
    output_bytes = 0
    timed_out: str | None = None

    print(
        f"[watchdog-start] stage={args.stage} total_timeout_s={args.total} "
        f"idle_timeout_s={args.idle} heartbeat_s={args.heartbeat}",
        flush=True,
    )
    print("[watchdog-command] " + " ".join(command), flush=True)

    with log_path.open("wb", buffering=0) as log:
        log.write(("[watchdog-command] " + " ".join(command) + "\n").encode("utf-8"))
        proc = subprocess.Popen(
            command,
            cwd=args.cwd,
            env=os.environ.copy(),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=False,
            bufsize=0,
            start_new_session=True,
        )

        q: queue.Queue[bytes | None] = queue.Queue()

        def reader() -> None:
            assert proc.stdout is not None
            fd = proc.stdout.fileno()
            try:
                while True:
                    chunk = os.read(fd, 4096)
                    if not chunk:
                        break
                    q.put(chunk)
            finally:
                q.put(None)

        thread = threading.Thread(target=reader, name="watchdog-reader", daemon=True)
        thread.start()
        reader_done = False
        decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
        partial_line = ""

        def consume_chunk(chunk: bytes, *, final: bool = False) -> None:
            nonlocal last_output, last_line, output_bytes, partial_line
            now_local = time.monotonic()
            last_output = now_local
            output_bytes += len(chunk)
            log.write(chunk)
            log.flush()
            text = decoder.decode(chunk, final=final)
            if text:
                sys.stdout.write(text)
                sys.stdout.flush()
                combined = partial_line + text
                parts = combined.split("\n")
                if combined.endswith("\n"):
                    partial_line = ""
                    candidates = parts[:-1]
                else:
                    partial_line = parts[-1]
                    candidates = parts[:-1]
                for candidate in candidates:
                    if candidate.strip():
                        last_line = shorten(candidate)
                if partial_line.strip():
                    last_line = shorten(partial_line)

        try:
            while True:
                now = time.monotonic()
                drained = False
                while True:
                    try:
                        item = q.get_nowait()
                    except queue.Empty:
                        break
                    drained = True
                    if item is None:
                        reader_done = True
                        continue
                    consume_chunk(item)

                now = time.monotonic()
                elapsed = now - start
                idle = max(0.0, now - last_output)
                if elapsed >= args.total:
                    timed_out = "total_timeout"
                    print(
                        f"[watchdog-timeout] stage={args.stage} reason=total "
                        f"elapsed_s={elapsed:.1f}",
                        flush=True,
                    )
                    kill_process_group(proc)
                    break
                if idle >= args.idle:
                    timed_out = "idle_timeout"
                    print(
                        f"[watchdog-timeout] stage={args.stage} reason=idle "
                        f"idle_s={idle:.1f} last={last_line}",
                        flush=True,
                    )
                    kill_process_group(proc)
                    break

                if now - last_heartbeat >= args.heartbeat:
                    last_heartbeat = now
                    state = {
                        "stage": args.stage,
                        "status": "running",
                        "pid": proc.pid,
                        "elapsed_s": round(elapsed, 3),
                        "idle_s": round(idle, 3),
                        "output_bytes": output_bytes,
                        "last_line": last_line,
                        "log": str(log_path),
                        "updated_unix": time.time(),
                    }
                    atomic_json(heartbeat_path, state)
                    print(
                        f"[watchdog-heartbeat] stage={args.stage} "
                        f"elapsed_s={elapsed:.1f} idle_s={idle:.1f} "
                        f"bytes={output_bytes} last={last_line}",
                        flush=True,
                    )

                rc = proc.poll()
                if rc is not None and reader_done and q.empty():
                    break
                if not drained:
                    time.sleep(0.1)
        except KeyboardInterrupt:
            timed_out = "interrupted"
            print(f"[watchdog-interrupt] stage={args.stage}", flush=True)
            kill_process_group(proc)

        # Drain any final lines after termination.
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            try:
                item = q.get(timeout=0.1)
            except queue.Empty:
                if proc.poll() is not None:
                    break
                continue
            if item is None:
                break
            consume_chunk(item)

        try:
            rc = proc.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            kill_process_group(proc, grace=1.0)
            rc = proc.wait(timeout=2.0)

    elapsed = time.monotonic() - start
    if timed_out == "total_timeout":
        final_rc = 124
    elif timed_out == "idle_timeout":
        final_rc = 125
    elif timed_out == "interrupted":
        final_rc = 130
    else:
        final_rc = rc

    final_state = {
        "stage": args.stage,
        "status": "timeout" if timed_out else ("success" if final_rc == 0 else "failed"),
        "timeout_reason": timed_out,
        "exit_code": final_rc,
        "elapsed_s": round(elapsed, 3),
        "output_bytes": output_bytes,
        "last_line": last_line,
        "log": str(log_path),
        "updated_unix": time.time(),
    }
    atomic_json(heartbeat_path, final_state)
    print(
        f"[watchdog-end] stage={args.stage} status={final_state['status']} "
        f"exit_code={final_rc} elapsed_s={elapsed:.1f} log={log_path}",
        flush=True,
    )
    return final_rc


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--heartbeat-json", required=True)
    parser.add_argument("--total", type=float, required=True)
    parser.add_argument("--idle", type=float, required=True)
    parser.add_argument("--heartbeat", type=float, default=10.0)
    parser.add_argument("--cwd")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    return run(parser.parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
