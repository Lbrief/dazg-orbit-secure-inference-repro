# DAZG-Orbit Project Source File
# Component: experiments/n10_p60/pair_runner.py
# Purpose: Fail-closed experiment, verification, or Q16 oracle component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
# DAZG_ORBIT_V743R8P74_RUNNER_POLICY_ENV: adapter policy may be overridden by DAZG_ORBIT_V743R8P74_ADAPTER_POLICY, then P72/P70/P69/P68, default reveal.
# DAZG_ORBIT_V743R8P72_RUNNER_POLICY_ENV: adapter policy may be overridden by DAZG_ORBIT_V743R8P72_ADAPTER_POLICY, then P70/P69/P68, default reveal.
# DAZG_ORBIT_V743R8P70_RUNNER_POLICY_ENV: adapter policy may be overridden by DAZG_ORBIT_V743R8P70_ADAPTER_POLICY, then P69, then P68, default reveal.
# DAZG_ORBIT_V743R8P69_RUNNER_POLICY_ENV: adapter policy may be overridden by DAZG_ORBIT_V743R8P69_ADAPTER_POLICY, then P68, default reveal.

# DAZG_ORBIT_V743R8P68_RUNNER_POLICY_ENV: adapter policy may be overridden by DAZG_ORBIT_V743R8P68_ADAPTER_POLICY.
#!/usr/bin/env python3
"""Launch the canonical two-party executor and stream both logs visibly."""
from __future__ import annotations

import argparse
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
    tmp = path.with_name(path.name + f".tmp.{os.getpid()}")
    tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")
    os.replace(tmp, path)


def terminate(proc: subprocess.Popen[bytes] | None, grace: float = 5.0) -> None:
    if proc is None or proc.poll() is not None:
        return
    try:
        proc.terminate()
    except ProcessLookupError:
        return
    deadline = time.monotonic() + grace
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return
        time.sleep(0.1)
    try:
        proc.kill()
    except ProcessLookupError:
        pass


def make_command(
    binary: Path,
    role: str,
    party: int,
    address: str,
    port: int,
    mode: str,
    sample: int,
    out_dir: Path,
    dump_dir: Path,
) -> list[str]:
    return [
        str(binary),
        "--role", role,
        "--party", str(party),
        "--address", address,
        "--port", str(port),
        "--mode", mode,
        "--sample-index", str(sample),
        "--input-owner", "server",
        "--adapter-policy", os.environ.get("DAZG_ORBIT_V743R8P74_ADAPTER_POLICY", os.environ.get("DAZG_ORBIT_V743R8P72_ADAPTER_POLICY", os.environ.get("DAZG_ORBIT_V743R8P70_ADAPTER_POLICY", os.environ.get("DAZG_ORBIT_V743R8P69_ADAPTER_POLICY", os.environ.get("DAZG_ORBIT_V743R8P68_ADAPTER_POLICY", "reveal"))))),
        "--dump-dir", str(dump_dir),
        "--out-report", str(out_dir / f"{role}_report.json"),
        "--out-tensor", str(out_dir / f"{role}_tensor.npy"),
        "--out-logits", str(out_dir / f"{role}_logits.npy"),
        "--out-feature", str(out_dir / f"{role}_feature.npy"),
    ]


def run(args: argparse.Namespace) -> int:
    binary = Path(args.binary).resolve()
    out_dir = Path(args.out_dir).resolve()
    root = Path(args.root).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    server_dump = out_dir / "server_dump"
    client_dump = out_dir / "client_dump"
    server_dump.mkdir(exist_ok=True)
    client_dump.mkdir(exist_ok=True)

    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise SystemExit(f"executor is missing or not executable: {binary}")

    env = os.environ.copy()
    env.update(
        {
            "DAZG_ORBIT_V675_FORCE_PLAIN_BITS": "50",
            "DAZG_ORBIT_V671_ENABLE_Q32_TRUNC_BRIDGE": "1",
            "DAZG_ORBIT_V671_TRUNC_MODE": "floor",
            "DAZG_ORBIT_V720_TO_H8_ROUTE": args.candidate,
            "DAZG_ORBIT_PROFILER": "1",
            "PYTHONUNBUFFERED": "1",
        }
    )
    env.pop("DAZG_ORBIT_V677_DISABLE_WEIGHT_MODP", None)
    env.pop("DAZG_ORBIT_V678_DISABLE_BIAS_Q32_PROMOTION", None)

    server_cmd = make_command(
        binary, "server", 1, args.address, args.port,
        args.mode, args.sample, out_dir, server_dump,
    )
    client_cmd = make_command(
        binary, "client", 2, args.address, args.port,
        args.mode, args.sample, out_dir, client_dump,
    )

    print(
        f"[pair-start] mode={args.mode} sample={args.sample} "
        f"candidate={args.candidate} port={args.port}",
        flush=True,
    )
    print("[pair-server-command] " + " ".join(server_cmd), flush=True)
    print("[pair-client-command] " + " ".join(client_cmd), flush=True)

    server_log = (out_dir / "server.log").open("wb", buffering=0)
    client_log = (out_dir / "client.log").open("wb", buffering=0)
    processes: dict[str, subprocess.Popen[bytes]] = {}
    q: queue.Queue[tuple[str, bytes] | tuple[str, None]] = queue.Queue()
    stop_requested = False
    start = time.monotonic()

    def on_signal(signum: int, _frame: object) -> None:
        nonlocal stop_requested
        stop_requested = True
        print(f"[pair-signal] signal={signum}; terminating both parties", flush=True)
        terminate(processes.get("server"), grace=1.0)
        terminate(processes.get("client"), grace=1.0)

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    def reader(role: str, proc: subprocess.Popen[bytes]) -> None:
        assert proc.stdout is not None
        buf = b""
        try:
            while True:
                chunk = os.read(proc.stdout.fileno(), 4096)
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    q.put((role, line + b"\n"))
            if buf:
                q.put((role, buf))
        finally:
            q.put((role, None))

    try:
        processes["server"] = subprocess.Popen(
            server_cmd,
            cwd=root,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0,
        )
        threading.Thread(
            target=reader,
            args=("server", processes["server"]),
            daemon=True,
            name="v720-server-reader",
        ).start()

        time.sleep(args.server_lead)
        if processes["server"].poll() is not None:
            print(
                f"[pair-error] server exited before client start rc={processes['server'].returncode}",
                flush=True,
            )
        else:
            processes["client"] = subprocess.Popen(
                client_cmd,
                cwd=root,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                bufsize=0,
            )
            threading.Thread(
                target=reader,
                args=("client", processes["client"]),
                daemon=True,
                name="v720-client-reader",
            ).start()

        done_readers: set[str] = set()
        while True:
            try:
                role, payload = q.get(timeout=0.2)
            except queue.Empty:
                role = ""
                payload = b""

            if role:
                if payload is None:
                    done_readers.add(role)
                else:
                    target = server_log if role == "server" else client_log
                    target.write(payload)
                    text = payload.decode("utf-8", errors="replace")
                    prefix = f"[{role}] "
                    if text.endswith("\n"):
                        print(prefix + text[:-1], flush=True)
                    else:
                        print(prefix + text, flush=True)

            server_rc = processes.get("server").poll() if processes.get("server") else 127
            client_rc = processes.get("client").poll() if processes.get("client") else 127

            if server_rc is not None and server_rc != 0 and client_rc is None:
                print(f"[pair-peer-stop] server rc={server_rc}; stopping client", flush=True)
                terminate(processes.get("client"))
            if client_rc is not None and client_rc != 0 and server_rc is None:
                print(f"[pair-peer-stop] client rc={client_rc}; stopping server", flush=True)
                terminate(processes.get("server"))

            both_exited = server_rc is not None and client_rc is not None
            readers_done = (
                "server" in done_readers and
                ("client" in done_readers or "client" not in processes)
            )
            if both_exited and readers_done and q.empty():
                break
            if stop_requested and both_exited:
                break

        server_rc = processes.get("server").wait() if processes.get("server") else 127
        client_rc = processes.get("client").wait() if processes.get("client") else 127
    finally:
        terminate(processes.get("server"), grace=1.0)
        terminate(processes.get("client"), grace=1.0)
        server_log.close()
        client_log.close()

    elapsed = time.monotonic() - start
    status = "success" if server_rc == 0 and client_rc == 0 else "failed"
    report = {
        "schema": "dazg_orbit.qahl.v720.pair_report",
        "status": status,
        "mode": args.mode,
        "sample_index": args.sample,
        "candidate": args.candidate,
        "port": args.port,
        "server_rc": server_rc,
        "client_rc": client_rc,
        "wall_s": round(elapsed, 6),
        "server_command": server_cmd,
        "client_command": client_cmd,
        "out_dir": str(out_dir),
    }
    atomic_json(out_dir / "pair_report.json", report)
    print(
        f"[pair-end] status={status} server_rc={server_rc} client_rc={client_rc} "
        f"wall_s={elapsed:.3f} out={out_dir}",
        flush=True,
    )
    return 0 if status == "success" else 20


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--root", required=True)
    parser.add_argument("--mode", required=True)
    parser.add_argument("--sample", type=int, required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--address", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--server-lead", type=float, default=1.0)
    return run(parser.parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
