#!/usr/bin/env python3
# DAZG-Orbit Project Source File
# Component: experiments/n100_checkpoint013/tools/pair_runner.py
# Purpose: Fail-closed experiment, verification, or Q16 oracle component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
from __future__ import annotations
import argparse, json, os, queue, shutil, signal, subprocess, sys, threading, time
from pathlib import Path
from typing import Any

def atomic_json(path: Path, data: dict[str, Any]) -> None:
    """Write JSON safely on Linux filesystems and WSL DrvFS mounts.

    WSL users often unpack the repository under /mnt/c.  Antivirus/indexing can
    temporarily hold a newly written file and make os.replace() fail even though
    the JSON payload is complete.  Retry the atomic promotion, then fall back to
    a verified direct copy.  The caller never treats a complete temporary report
    as a computation failure.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + f".tmp.{os.getpid()}.{time.time_ns()}")
    payload = json.dumps(data, ensure_ascii=False, indent=2) + "\n"
    with tmp.open("w", encoding="utf-8", newline="\n") as fh:
        fh.write(payload)
        fh.flush()
        os.fsync(fh.fileno())

    last_error: OSError | None = None
    for attempt in range(20):
        try:
            os.replace(tmp, path)
            return
        except OSError as exc:
            last_error = exc
            time.sleep(min(0.05 * (attempt + 1), 0.5))

    # DrvFS compatibility fallback.  A single runner owns this path, and the
    # parent process reads it only after this function returns.
    fallback = path.with_name(path.name + f".copy.{os.getpid()}.{time.time_ns()}")
    try:
        shutil.copyfile(tmp, fallback)
        with fallback.open("rb") as fh:
            os.fsync(fh.fileno())
        shutil.copyfile(fallback, path)
        # Verify that the final file is complete JSON before removing evidence.
        json.loads(path.read_text(encoding="utf-8"))
        tmp.unlink(missing_ok=True)
        fallback.unlink(missing_ok=True)
        print(
            f"[atomic-json-fallback] path={path} replace_error={last_error}",
            file=sys.stderr,
            flush=True,
        )
    except Exception:
        # Preserve tmp/fallback for post-mortem evidence.
        raise RuntimeError(
            f"unable to publish JSON report {path}; temporary payload preserved at {tmp}"
        ) from last_error

def terminate(proc: subprocess.Popen[bytes] | None, grace: float=5.0) -> None:
    if proc is None or proc.poll() is not None: return
    try: proc.terminate()
    except ProcessLookupError: return
    deadline=time.monotonic()+grace
    while time.monotonic()<deadline:
        if proc.poll() is not None: return
        time.sleep(.1)
    try: proc.kill()
    except ProcessLookupError: pass

def command(binary:Path, role:str, party:int, address:str, port:int,
            sample:int, out:Path, dump:str) -> list[str]:
    return [
        str(binary),"--role",role,"--party",str(party),
        "--address",address,"--port",str(port),
        "--mode","full-sample","--sample-index",str(sample),
        "--input-owner","server","--adapter-policy","reveal",
        "--dump-dir",dump,
        "--out-report",str(out/f"{role}_report.json"),
        "--out-tensor",str(out/f"{role}_tensor.npy"),
        "--out-logits",str(out/f"{role}_logits.npy"),
        "--out-feature",str(out/f"{role}_feature.npy"),
    ]

def run(a:argparse.Namespace)->int:
    binary=Path(a.binary).resolve()
    root=Path(a.root).resolve()
    out=Path(a.out_dir).resolve()
    out.mkdir(parents=True,exist_ok=True)
    if not binary.is_file() or not os.access(binary,os.X_OK):
        raise SystemExit(f"missing executable: {binary}")
    dump_s=str(out/"server_dump") if a.enable_dump else ""
    dump_c=str(out/"client_dump") if a.enable_dump else ""
    if a.enable_dump:
        Path(dump_s).mkdir(exist_ok=True); Path(dump_c).mkdir(exist_ok=True)
    server_cmd=command(binary,"server",1,a.address,a.port,a.sample,out,dump_s)
    client_cmd=command(binary,"client",2,a.address,a.port,a.sample,out,dump_c)
    env=os.environ.copy()
    stage_s_table=Path(__file__).resolve().parents[1]/"assets/stage_s/stage_s_gelu_q16_i64.bin"
    if not stage_s_table.is_file():
        raise SystemExit(f"missing frozen Stage-S table: {stage_s_table}")
    env["DAZG_ORBIT_STAGE_S_Q16_TABLE"]=str(stage_s_table)
    env.update({
        "DAZG_ORBIT_V675_FORCE_PLAIN_BITS":"50",
        "DAZG_ORBIT_V671_ENABLE_Q32_TRUNC_BRIDGE":"1",
        "DAZG_ORBIT_V671_TRUNC_MODE":"floor",
        "DAZG_ORBIT_V720_TO_H8_ROUTE":"off",
        "DAZG_ORBIT_V743R8P74_ADAPTER_POLICY":"reveal",
        "DAZG_ORBIT_V724_DUMP_ALL_SAMPLES":"0",
        "DAZG_ORBIT_PROFILER":"1",
        "PYTHONUNBUFFERED":"1",
    })
    for k in ["DAZG_ORBIT_V677_DISABLE_WEIGHT_MODP","DAZG_ORBIT_V678_DISABLE_BIAS_Q32_PROMOTION"]:
        env.pop(k,None)
    print(f"[pair-start] sample={a.sample} port={a.port} timeout={a.timeout}",flush=True)
    sl=(out/"server.log").open("wb",buffering=0)
    cl=(out/"client.log").open("wb",buffering=0)
    q:queue.Queue[tuple[str,bytes|None]]=queue.Queue()
    procs:dict[str,subprocess.Popen[bytes]]={}
    stop=False; timed_out=False; start=time.monotonic(); last_beat=start

    def sig(signum:int,_frame:object)->None:
        nonlocal stop
        stop=True
        print(f"[pair-signal] signal={signum}",flush=True)
        terminate(procs.get("server"),1); terminate(procs.get("client"),1)
    signal.signal(signal.SIGINT,sig); signal.signal(signal.SIGTERM,sig)

    def reader(role:str,p:subprocess.Popen[bytes])->None:
        assert p.stdout is not None
        try:
            for line in iter(p.stdout.readline,b""):
                q.put((role,line))
        finally:
            q.put((role,None))

    try:
        procs["server"]=subprocess.Popen(server_cmd,cwd=root,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,bufsize=0)
        threading.Thread(target=reader,args=("server",procs["server"]),daemon=True).start()
        time.sleep(a.server_lead)
        if procs["server"].poll() is None:
            procs["client"]=subprocess.Popen(client_cmd,cwd=root,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,bufsize=0)
            threading.Thread(target=reader,args=("client",procs["client"]),daemon=True).start()
        done=set()
        while True:
            now=time.monotonic()
            if a.timeout>0 and now-start>a.timeout:
                timed_out=True; stop=True
                print(f"[pair-timeout] sample={a.sample} elapsed={now-start:.1f}",flush=True)
                terminate(procs.get("server"),1); terminate(procs.get("client"),1)
            if now-last_beat>=60:
                last_beat=now
                print(f"[pair-heartbeat] sample={a.sample} elapsed={now-start:.1f} server_rc={procs.get('server').poll() if procs.get('server') else None} client_rc={procs.get('client').poll() if procs.get('client') else None}",flush=True)
            try: role,payload=q.get(timeout=.2)
            except queue.Empty: role=""; payload=b""
            if role:
                if payload is None: done.add(role)
                else:
                    (sl if role=="server" else cl).write(payload)
                    txt=payload.decode("utf-8",errors="replace").rstrip("\n")
                    print(f"[{role}] {txt}",flush=True)
            sr=procs.get("server").poll() if procs.get("server") else 127
            cr=procs.get("client").poll() if procs.get("client") else 127
            if sr is not None and sr!=0 and cr is None: terminate(procs.get("client"))
            if cr is not None and cr!=0 and sr is None: terminate(procs.get("server"))
            both=sr is not None and cr is not None
            readers=("server" in done and ("client" in done or "client" not in procs))
            if both and readers and q.empty(): break
            if stop and both: break
        sr=procs.get("server").wait() if procs.get("server") else 127
        cr=procs.get("client").wait() if procs.get("client") else 127
    finally:
        terminate(procs.get("server"),1); terminate(procs.get("client"),1)
        sl.close(); cl.close()
    elapsed=time.monotonic()-start
    status="success" if sr==0 and cr==0 and not timed_out and not stop else "failed"
    report={
        "schema":"dazg.checkpoint013.n100.pair.v1","status":status,
        "sample_index":a.sample,"server_rc":sr,"client_rc":cr,
        "timed_out":timed_out,"interrupted":stop and not timed_out,
        "wall_s":round(elapsed,6),"port":a.port,
        "binary":str(binary),"server_command":server_cmd,"client_command":client_cmd,
    }
    atomic_json(out/"pair_report.json",report)
    print(f"[pair-end] sample={a.sample} status={status} server_rc={sr} client_rc={cr} elapsed={elapsed:.1f}",flush=True)
    return 0 if status=="success" else 20

def main()->int:
    ap=argparse.ArgumentParser()
    ap.add_argument("--binary",required=True); ap.add_argument("--root",required=True)
    ap.add_argument("--sample",required=True,type=int); ap.add_argument("--out-dir",required=True)
    ap.add_argument("--port",required=True,type=int); ap.add_argument("--address",default="127.0.0.1")
    ap.add_argument("--server-lead",type=float,default=1.0); ap.add_argument("--timeout",type=float,default=10800)
    ap.add_argument("--enable-dump",action="store_true")
    return run(ap.parse_args())
if __name__=="__main__": raise SystemExit(main())
