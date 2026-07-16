#!/usr/bin/env python3
# DAZG-Orbit Project Source File
# Component: experiments/n100_checkpoint013/tools/preflight.py
# Purpose: Fail-closed experiment, verification, or Q16 oracle component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
from __future__ import annotations
import argparse, hashlib, json
from pathlib import Path
import numpy as np

EXPECTED={
 'checkpoint_sha256':'5a103795fff72d3bbc0bcffac4839f49afe4eb1bd97541f9bab8bcc75eb98569',
 'policy_sha256':'90c4a90671f81cb88fdaf373693bacf9146897db494d709d10cac7582f42adaf',
 'stage_s_table_sha256':'ad6d5faad10f0df1d06f6b0131aec7f6a2f2a1ff4e427456a819b98a930a6f49',
 'oracle_logits_sha256':'ae2cd654ddce520317bcd3b4d0c78c7d3279c81ed46643533617d67ee2665fe7',
 'top1':72,'top5':91,
}

def sha256(path:Path)->str:
 h=hashlib.sha256()
 with path.open('rb') as f:
  for b in iter(lambda:f.read(1<<20),b''): h.update(b)
 return h.hexdigest()

def main()->int:
 ap=argparse.ArgumentParser(); ap.add_argument('--assets',required=True); ap.add_argument('--out',required=True); a=ap.parse_args()
 root=Path(a.assets).resolve(); package=root.parent; payload=root/'payload'; refdir=root/'reference'; errors=[]
 table_path=root/'stage_s/stage_s_gelu_q16_i64.bin'; table_manifest=root/'stage_s/stage_s_contract.json'
 if not table_path.is_file(): errors.append('frozen Stage-S table missing')
 else:
  if table_path.stat().st_size!=8388616: errors.append(f'Stage-S table size={table_path.stat().st_size}')
  if sha256(table_path)!=EXPECTED['stage_s_table_sha256']: errors.append('Stage-S table SHA mismatch')
  table=np.fromfile(table_path,dtype='<i8')
  if table.shape!=(1048577,): errors.append(f'Stage-S table shape={table.shape}')
  elif [int(table[524287]),int(table[524288]),int(table[524289]),int(table[589824])] != [-1,0,1,55139]:
   errors.append('Stage-S central probe mismatch')
 if not table_manifest.is_file(): errors.append('Stage-S contract manifest missing')

 manifest_path=payload/'payload_manifest.json'
 if not manifest_path.is_file(): errors.append('payload_manifest.json missing'); manifest={'files':[]}
 else: manifest=json.loads(manifest_path.read_text())
 if manifest.get('checkpoint_state_keys')!=77: errors.append(f"checkpoint_state_keys={manifest.get('checkpoint_state_keys')}")
 if manifest.get('policy_sha256')!=EXPECTED['policy_sha256']: errors.append('policy SHA mismatch')
 if manifest.get('stage_s_table_sha256')!=EXPECTED['stage_s_table_sha256']: errors.append('manifest Stage-S SHA mismatch')
 entries=[e for e in manifest.get('files',[]) if e.get('dtype')=='uint64' or str(e.get('file','')).endswith('.q16.u64.npy')]
 if len(entries)!=77: errors.append(f'Q16 entry count={len(entries)}')
 seen=set()
 for e in entries:
  key=e.get('state_key')
  if key in seen: errors.append(f'duplicate key {key}')
  seen.add(key); p=payload/e['file']
  if not p.is_file(): errors.append(f'missing payload {p.name}'); continue
  if sha256(p)!=e['sha256']: errors.append(f'payload SHA mismatch {p.name}')
  arr=np.load(p,allow_pickle=False)
  if arr.dtype!=np.uint64 or list(arr.shape)!=list(e['shape']): errors.append(f'payload contract mismatch {p.name}')

 required=['balanced_n100_reference.json','qahl_ref_input_n10.npy','qahl_ref_labels_n10.npy',
           'checkpoint013_balanced_n100_logits_q16_i64.npy','checkpoint013_balanced_n100_top5_i64.npy']
 for name in required:
  if not (refdir/name).is_file(): errors.append(f'missing reference {name}')
 ref=json.loads((refdir/'balanced_n100_reference.json').read_text())
 x=np.load(refdir/'qahl_ref_input_n10.npy',allow_pickle=False); y=np.load(refdir/'qahl_ref_labels_n10.npy',allow_pickle=False)
 oracle=np.load(refdir/'checkpoint013_balanced_n100_logits_q16_i64.npy',allow_pickle=False)
 frozen_top5=np.load(refdir/'checkpoint013_balanced_n100_top5_i64.npy',allow_pickle=False)
 calc_top5=np.argsort(-oracle,axis=1,kind='stable')[:,:5]
 json_top5=np.asarray(ref.get('q16_top5_predictions',[]),dtype=np.int64)
 if x.shape!=(100,3,32,32) or x.dtype!=np.uint64: errors.append(f'input contract {x.shape} {x.dtype}')
 if y.shape!=(100,) or y.dtype!=np.uint64: errors.append(f'label contract {y.shape} {y.dtype}')
 if oracle.shape!=(100,100) or oracle.dtype!=np.int64: errors.append(f'oracle contract {oracle.shape} {oracle.dtype}')
 if frozen_top5.shape!=(100,5) or frozen_top5.dtype!=np.int64: errors.append(f'oracle Top-5 contract {frozen_top5.shape} {frozen_top5.dtype}')
 if sha256(refdir/'checkpoint013_balanced_n100_logits_q16_i64.npy')!=EXPECTED['oracle_logits_sha256']: errors.append('oracle logits SHA mismatch')
 if not np.array_equal(calc_top5,frozen_top5): errors.append('oracle logits/Top-5 NPY mismatch')
 if not np.array_equal(calc_top5,json_top5): errors.append('oracle/reference Top-5 rows mismatch')
 labels=y.astype(np.int64); top1=int(np.sum(calc_top5[:,0]==labels)); top5=int(sum(int(labels[i] in calc_top5[i]) for i in range(100)))
 if (top1,top5)!=(72,91): errors.append(f'oracle metrics {top1}/{top5}, expected 72/91')
 if ref.get('q16_top1_correct')!=72 or ref.get('q16_top5_correct')!=91: errors.append('reference metadata is not 72/91')
 if ref.get('labels')!=list(range(100)) or labels.tolist()!=list(range(100)): errors.append('balanced class order must be 0..99')
 if len(ref.get('official_test_indices',[]))!=100 or len(set(ref.get('official_test_indices',[])))!=100: errors.append('balanced indices must be 100 unique rows')
 if ref.get('input_sha256') and sha256(refdir/'qahl_ref_input_n10.npy')!=ref['input_sha256']: errors.append('balanced input SHA mismatch')

 source_candidates=[package/'src/dazg_checkpoint013_n100_executor.cpp', package/'source/src/dazg_checkpoint013_n100_executor.cpp']
 source=next((p for p in source_candidates if p.is_file()), source_candidates[0])
 marker='DAZG_CHECKPOINT013_N100_FULLGRAPH_REVEAL_CONV_EXACT_R3_20260715'
 if not source.is_file() or marker not in source.read_text(errors='replace'): errors.append('R3 exact reveal convolution marker missing from source')
 for p in list((package/'tools').glob('*.py'))+[package/'install_and_run.sh']:
  if p.resolve()==Path(__file__).resolve():
   continue
  if p.is_file():
   text=p.read_text(errors='replace')
   for forbidden in ('--target 1000','range(1000)','05_n1000'):
    if forbidden in text: errors.append(f'N=1000 execution token {forbidden!r} found in {p.name}')

 report={
  'schema':'dazg.checkpoint013.n100.preflight.v3','status':'PASS' if not errors else 'FAIL','errors':errors,
  'q16_weight_count':len(entries),'balanced_input_shape':list(x.shape),'balanced_input_sha256':sha256(refdir/'qahl_ref_input_n10.npy'),
  'oracle_logits_sha256':sha256(refdir/'checkpoint013_balanced_n100_logits_q16_i64.npy'),
  'oracle_reference_rows_exact':int(np.sum(np.all(calc_top5==json_top5,axis=1))) if json_top5.shape==(100,5) else 0,
  'reference_top1':top1,'reference_top5':top5,'retraining_required':False,'n1000_present':False,
  'security_boundary':{'adapter_policy':'reveal','security_claim':0},
 }
 Path(a.out).resolve().parent.mkdir(parents=True,exist_ok=True); Path(a.out).resolve().write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps(report,indent=2),flush=True)
 return 0 if not errors else 21
if __name__=='__main__': raise SystemExit(main())
