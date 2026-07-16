#!/usr/bin/env python3
# DAZG-Orbit Project Source File
# Component: experiments/n100_checkpoint013/tools/run_n100.py
# Purpose: Fail-closed experiment, verification, or Q16 oracle component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
from __future__ import annotations
import argparse, datetime as dt, hashlib, json, os, shutil, signal, socket, subprocess, sys, tarfile, tempfile, time
from pathlib import Path
from typing import Any
import numpy as np

P=1125899906826241

def sha256(path:Path)->str:
    h=hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda:f.read(1<<20),b''): h.update(b)
    return h.hexdigest()

def atomic_json(path:Path, data:dict[str,Any]) -> None:
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

def free_port()->int:
    with socket.socket(socket.AF_INET,socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1',0)); return int(s.getsockname()[1])

def recover_atomic_json(path: Path) -> dict[str, Any] | None:
    """Recover a complete report left as *.tmp.* after a DrvFS rename failure."""
    if path.is_file():
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except Exception:
            pass
    candidates = sorted(
        path.parent.glob(path.name + ".tmp.*"),
        key=lambda p: p.stat().st_mtime_ns,
        reverse=True,
    )
    for tmp in candidates:
        try:
            data = json.loads(tmp.read_text(encoding="utf-8"))
        except Exception:
            continue
        atomic_json(path, data)
        print(f"[n100-recover] promoted complete temporary report {tmp.name}", flush=True)
        return data
    return None

def reconstruct(s:np.ndarray,c:np.ndarray)->np.ndarray:
    if s.shape!=c.shape: raise ValueError(f'share shape mismatch {s.shape} vs {c.shape}')
    sr=np.remainder(np.ascontiguousarray(s,dtype=np.uint64),np.uint64(P))
    cr=np.remainder(np.ascontiguousarray(c,dtype=np.uint64),np.uint64(P))
    return np.remainder(sr+cr,np.uint64(P)).astype(np.uint64,copy=False)

def center(r:np.ndarray)->np.ndarray:
    x=np.asarray(r,dtype=np.uint64).astype(np.int64)
    return np.where(x>P//2,x-P,x).astype(np.int64)

class Runner:
    def __init__(self,a:argparse.Namespace):
        self.a=a; self.binary=Path(a.binary).resolve(); self.repo=Path(a.repo).resolve()
        self.assets=Path(a.assets).resolve(); self.package=Path(a.package_root).resolve()
        self.downloads=Path(a.downloads).resolve(); self.run_base=Path(a.run_base).resolve()
        self.session_dir=Path(a.session_dir).resolve() if a.session_dir else None
        self.ref=json.loads((self.assets/'reference/balanced_n100_reference.json').read_text())
        self.labels=np.load(self.assets/'reference/qahl_ref_labels_n10.npy',allow_pickle=False).astype(np.int64)
        self.reference_top5=np.asarray(self.ref['q16_top5_predictions'],dtype=np.int64)
        self.oracle_path=self.assets/'reference/checkpoint013_balanced_n100_logits_q16_i64.npy'
        self.oracle=np.load(self.oracle_path,allow_pickle=False).astype(np.int64,copy=False)
        if self.oracle.shape!=(100,100): raise RuntimeError(f'oracle shape={self.oracle.shape}, expected (100,100)')
        calc=np.argsort(-self.oracle,axis=1,kind='stable')[:,:5]
        if self.reference_top5.shape!=(100,5) or not np.array_equal(calc,self.reference_top5):
            raise RuntimeError('frozen logits do not reproduce all 100 checkpoint013 Top-5 rows')
        t1=int(np.sum(calc[:,0]==self.labels)); t5=int(sum(int(self.labels[i] in calc[i]) for i in range(100)))
        if (t1,t5)!=(72,91): raise RuntimeError(f'frozen oracle accuracy={t1}/{t5}, expected 72/91')
        self.binary_sha=sha256(self.binary); self.reference_sha=sha256(self.assets/'reference/balanced_n100_reference.json')
        self.input_sha=sha256(self.assets/'reference/qahl_ref_input_n10.npy'); self.oracle_sha=sha256(self.oracle_path)
        ident=hashlib.sha256((self.binary_sha+self.reference_sha+self.input_sha+self.oracle_sha).encode()).hexdigest()[:20]
        self.run=self.run_base/'runs'/ident; self.samples=self.run/'samples'; self.reports=self.run/'reports'
        self.samples.mkdir(parents=True,exist_ok=True); self.reports.mkdir(parents=True,exist_ok=True)
        self.stop=False; self.failure_sample:int|None=None; self.failure_reason=''; self.started=time.time()
        signal.signal(signal.SIGINT,self.on_signal); signal.signal(signal.SIGTERM,self.on_signal)
        atomic_json(self.run/'run_identity.json',{
            'schema':'dazg.checkpoint013.n100.run_identity.v3','binary_sha256':self.binary_sha,
            'reference_sha256':self.reference_sha,'input_sha256':self.input_sha,'oracle_sha256':self.oracle_sha,
            'oracle_top1':72,'oracle_top5':91,'n1000_present':False,
            'security_boundary':{'adapter_policy':'reveal','security_claim':0},
        })
    def on_signal(self,signum:int,_frame:object)->None:
        self.stop=True; self.failure_reason=f'interrupted by signal {signum}'
        print(f'[n100-signal] {self.failure_reason}',flush=True)

    def valid_cached(self,row:int)->dict[str,Any]|None:
        p=self.samples/f'sample_{row:04d}'/'score.json'
        if not p.is_file(): return None
        try: d=json.loads(p.read_text())
        except Exception: return None
        if (d.get('status')=='PASS' and d.get('binary_sha256')==self.binary_sha
            and d.get('reference_sha256')==self.reference_sha and d.get('input_sha256')==self.input_sha
            and d.get('oracle_sha256')==self.oracle_sha and d.get('logits_exact') is True
            and d.get('logits_neq')==0 and d.get('max_abs_delta')==0
            and d.get('reference_top5_exact') is True): return d
        return None

    def score(self,row:int,out:Path)->dict[str,Any]:
        pair_path=out/'pair_report.json'
        pair=recover_atomic_json(pair_path)
        if pair is None: raise RuntimeError(f'missing pair report in {out}')
        if pair.get('status')!='success': raise RuntimeError(f'pair failed: {pair}')
        for role in ('server','client'):
            rp=out/f'{role}_report.json'
            if not rp.is_file(): raise RuntimeError(f'missing {rp.name}')
            r=json.loads(rp.read_text())
            if not r.get('output_ready',False) or not r.get('wrote_logits',False):
                raise RuntimeError(f'{role} report output not ready')
        s=np.load(out/'server_logits.npy',allow_pickle=False).reshape(-1)
        c=np.load(out/'client_logits.npy',allow_pickle=False).reshape(-1)
        if s.size!=100 or c.size!=100: raise RuntimeError(f'logit size mismatch server={s.size} client={c.size}')
        logits=center(reconstruct(s,c)); expected_logits=self.oracle[row]
        delta=logits-expected_logits; neq=int(np.count_nonzero(delta)); max_abs=int(np.max(np.abs(delta)))
        logits_exact=bool(neq==0)
        pred=np.argsort(-logits,kind='stable')[:5].astype(int)
        expected_top5=self.reference_top5[row]
        top5_exact=bool(np.array_equal(pred,expected_top5)); label=int(self.labels[row])
        passed=logits_exact and top5_exact
        result={
            'schema':'dazg.checkpoint013.n100.sample.v3','status':'PASS' if passed else 'FAIL',
            'balanced_row':row,'official_test_index':int(self.ref['official_test_indices'][row]),'label':label,
            'predicted_top5':pred.tolist(),'reference_top5':expected_top5.astype(int).tolist(),
            'reference_top5_exact':top5_exact,'logits_exact':logits_exact,'logits_neq':neq,'max_abs_delta':max_abs,
            'top1_correct':bool(pred[0]==label),'top5_correct':bool(label in pred),
            'binary_sha256':self.binary_sha,'reference_sha256':self.reference_sha,
            'input_sha256':self.input_sha,'oracle_sha256':self.oracle_sha,
            'pair_wall_s':pair.get('wall_s'),'server_rc':pair.get('server_rc'),'client_rc':pair.get('client_rc'),
            'security_boundary':{'adapter_policy':'reveal','security_claim':0,
                'reason':'all convolutions use exact Q16 reveal/re-share correctness backend'},
        }
        atomic_json(out/'score.json',result); return result

    def pair(self,row:int,out:Path,enable_dump:bool=False)->int:
        cmd=[sys.executable,str(self.package/'tools/pair_runner.py'),'--binary',str(self.binary),'--root',str(self.repo),
             '--sample',str(row),'--out-dir',str(out),'--port',str(free_port()),'--timeout',str(self.a.sample_timeout),
             '--server-lead',str(self.a.server_lead)]
        if enable_dump: cmd.append('--enable-dump')
        return subprocess.call(cmd,cwd=self.repo)

    def diagnostic_rerun(self,row:int,parent:Path)->None:
        diag=parent/'failure_diagnostic_with_stage_dumps'
        if diag.exists(): shutil.rmtree(diag)
        diag.mkdir(parents=True)
        print(f'[n100-diagnostic] rerun row={row} with complete stage dumps',flush=True)
        rc=self.pair(row,diag,True)
        try:
            if rc==0: self.score(row,diag)
        except Exception as e:
            atomic_json(diag/'diagnostic_score_error.json',{'error':f'{type(e).__name__}: {e}'})

    def run_one(self,row:int)->dict[str,Any]:
        cached=self.valid_cached(row)
        if cached:
            print(f"[n100-resume] row={row} official_index={self.ref['official_test_indices'][row]} cached=PASS logits_exact=True",flush=True)
            return cached
        if self.stop: raise KeyboardInterrupt
        out=self.samples/f'sample_{row:04d}'
        if out.exists(): shutil.rmtree(out)
        out.mkdir(parents=True)
        print(f"[n100-run] row={row} official_index={self.ref['official_test_indices'][row]} label={self.labels[row]}",flush=True)
        enable_sample0_dump = (row == 0 and os.getenv("DAZG_ENABLE_SAMPLE0_STAGE_DUMP", "0").lower() in ("1", "true", "yes"))
        rc=self.pair(row,out,enable_sample0_dump)
        if rc!=0:
            recovered=recover_atomic_json(out/'pair_report.json')
            recovered_ok=bool(
                recovered
                and recovered.get('status')=='success'
                and recovered.get('server_rc')==0
                and recovered.get('client_rc')==0
            )
            if recovered_ok:
                print(
                    f"[n100-recover] row={row} pair_runner_rc={rc} "
                    "but complete successful pair report was recovered",
                    flush=True,
                )
                rc=0
            else:
                self.failure_sample=row; self.failure_reason=f'pair runner rc={rc}'; self.diagnostic_rerun(row,out)
                raise RuntimeError(self.failure_reason)
        result=self.score(row,out)
        print(f"[n100-score] row={row} logits_exact={result['logits_exact']} neq={result['logits_neq']} max_abs_delta={result['max_abs_delta']} pred={result['predicted_top5']} ref={result['reference_top5']} top1={result['top1_correct']} top5={result['top5_correct']}",flush=True)
        if result['status']!='PASS':
            self.failure_sample=row; self.failure_reason=(f"checkpoint013 logits mismatch neq={result['logits_neq']} max_abs_delta={result['max_abs_delta']}")
            self.diagnostic_rerun(row,out); raise RuntimeError(self.failure_reason)
        return result

    def aggregate(self,name:str,rows:list[int],expected_top1:int|None=None,expected_top5:int|None=None)->dict[str,Any]:
        scores=[]
        for row in rows:
            d=self.valid_cached(row)
            if not d: raise RuntimeError(f'missing verified score for row {row}')
            scores.append(d)
        exact=sum(bool(x['logits_exact']) for x in scores); ref_exact=sum(bool(x['reference_top5_exact']) for x in scores)
        top1=sum(bool(x['top1_correct']) for x in scores); top5=sum(bool(x['top5_correct']) for x in scores); errors=[]
        if exact!=len(rows): errors.append(f'logits_exact={exact}/{len(rows)}')
        if ref_exact!=len(rows): errors.append(f'reference_top5_exact={ref_exact}/{len(rows)}')
        if expected_top1 is not None and top1!=expected_top1: errors.append(f'Top-1={top1}/{len(rows)} expected {expected_top1}')
        if expected_top5 is not None and top5!=expected_top5: errors.append(f'Top-5={top5}/{len(rows)} expected {expected_top5}')
        report={
            'schema':'dazg.checkpoint013.n100.gate.v3','gate':name,'status':'PASS' if not errors else 'FAIL','errors':errors,
            'requested_count':len(rows),'completed_count':len(scores),'logits_exact_count':exact,
            'reference_top5_exact_count':ref_exact,'top1_correct':top1,'top5_correct':top5,
            'expected_top1':expected_top1,'expected_top5':expected_top5,'binary_sha256':self.binary_sha,
            'reference_sha256':self.reference_sha,'oracle_sha256':self.oracle_sha,
            'security_boundary':{'adapter_policy':'reveal','security_claim':0},
        }
        atomic_json(self.reports/f'{name}.json',report)
        print(f"[n100-gate] gate={name} status={report['status']} logits_exact={exact}/{len(rows)} reference_rows={ref_exact}/{len(rows)} top1={top1}/{len(rows)} top5={top5}/{len(rows)}",flush=True)
        if errors: raise RuntimeError('; '.join(errors))
        return report

    def package_result(self,status:str)->Path:
        timestamp=dt.datetime.now().strftime('%Y%m%d_%H%M%S')
        result=self.downloads/f'dazg-stage4-checkpoint013-n100-result-{timestamp}.tar.gz'
        summary={
            'schema':'dazg.checkpoint013.n100.final.v3','status':status,'failure_sample':self.failure_sample,
            'failure_reason':self.failure_reason,'binary':str(self.binary),'binary_sha256':self.binary_sha,
            'reference_sha256':self.reference_sha,'input_sha256':self.input_sha,'oracle_sha256':self.oracle_sha,
            'run_dir':str(self.run),'elapsed_s':round(time.time()-self.started,3),'n1000_executed':False,
            'retraining_required':False,'required_pass':{'logits_exact':'100/100','reference_rows':'100/100','top1':'72/100','top5':'91/100'},
            'security_boundary':{'adapter_policy':'reveal','security_claim':0,
                'reason':'checkpoint013 correctness baseline; no no-reveal deployment claim'},
        }
        atomic_json(self.run/'final_report.json',summary)
        with tempfile.TemporaryDirectory(prefix='dazg-n100-result-') as td:
            stage=Path(td)/'dazg-stage4-checkpoint013-n100-result'; stage.mkdir()
            shutil.copy2(self.run/'final_report.json',stage/'final_report.json')
            shutil.copy2(self.run/'run_identity.json',stage/'run_identity.json')
            for p in sorted(self.reports.glob('*.json')):
                (stage/'reports').mkdir(exist_ok=True); shutil.copy2(p,stage/'reports'/p.name)
            scores_dir=stage/'scores'; scores_dir.mkdir()
            for p in sorted(self.samples.glob('sample_*/score.json')): shutil.copy2(p,scores_dir/f'{p.parent.name}.json')
            evidence_rows={0}
            if self.failure_sample is not None: evidence_rows.add(self.failure_sample)
            for row in evidence_rows:
                src=self.samples/f'sample_{row:04d}'
                if not src.is_dir(): continue
                dst=stage/'sample_evidence'/src.name; dst.parent.mkdir(exist_ok=True)
                if row==self.failure_sample:
                    shutil.copytree(src,dst,ignore=shutil.ignore_patterns('server_tensor.npy','client_tensor.npy','server_feature.npy','client_feature.npy'))
                else:
                    shutil.copytree(src,dst,ignore=shutil.ignore_patterns('server_tensor.npy','client_tensor.npy','server_feature.npy','client_feature.npy','server_dump','client_dump'))
            for name in ['PACKAGE_INFO.json','README.md','MANIFEST.sha256','VALIDATION_REPORT.json','SECURITY_BOUNDARY.md']:
                p=self.package/name
                if p.is_file(): (stage/'package_identity').mkdir(exist_ok=True); shutil.copy2(p,stage/'package_identity'/name)
            if self.session_dir is not None and self.session_dir.is_dir():
                shutil.copytree(self.session_dir,stage/'build_and_install_evidence',ignore=shutil.ignore_patterns('*.o','*.a','CMakeFiles','__pycache__'))
            patch_candidates=[self.package/'src/dazg_checkpoint013_n100_executor.cpp', self.package/'source/src/dazg_checkpoint013_n100_executor.cpp']
            patch_src=next((p for p in patch_candidates if p.is_file()), patch_candidates[0])
            if patch_src.is_file(): (stage/'source_identity').mkdir(exist_ok=True); shutil.copy2(patch_src,stage/'source_identity'/patch_src.name)
            with tarfile.open(result,'w:gz',compresslevel=6) as tf: tf.add(stage,arcname=stage.name)
        (self.run_base/'latest_result_path.txt').write_text(str(result)+'\n')
        print(f'[RESULT PACKAGE] {result}',flush=True); print('[UPLOAD ONLY THIS ONE FILE]',flush=True); return result

    def execute(self)->int:
        status='FAIL'
        try:
            self.run_one(0); self.aggregate('01_sample0',[0])
            for row in range(10): self.run_one(row)
            self.aggregate('02_first10',list(range(10)))
            for row in range(100):
                self.run_one(row); done=sum(self.valid_cached(i) is not None for i in range(100))
                print(f'[n100-progress] completed={done}/100 current_row={row}',flush=True)
            self.aggregate('03_balanced_n100',list(range(100)),72,91); status='PASS'; return 0
        except KeyboardInterrupt:
            self.failure_reason=self.failure_reason or 'interrupted'; return 130
        except Exception as e:
            self.failure_reason=self.failure_reason or f'{type(e).__name__}: {e}'
            print(f'[n100-fail] {self.failure_reason}',file=sys.stderr,flush=True); return 31
        finally: self.package_result(status)

def main()->int:
    ap=argparse.ArgumentParser(); ap.add_argument('--binary',required=True); ap.add_argument('--repo',required=True)
    ap.add_argument('--assets',required=True); ap.add_argument('--package-root',required=True)
    ap.add_argument('--run-base',required=True); ap.add_argument('--downloads',required=True)
    ap.add_argument('--sample-timeout',type=float,default=10800); ap.add_argument('--server-lead',type=float,default=1.0)
    ap.add_argument('--session-dir',default=''); return Runner(ap.parse_args()).execute()
if __name__=='__main__': raise SystemExit(main())
