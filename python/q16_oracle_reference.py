#!/usr/bin/env python3
# DAZG-Orbit Project Source File
# Component: python/q16_oracle_reference.py
# Purpose: Fail-closed experiment, verification, or Q16 oracle component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
"""Regenerate the frozen checkpoint-013 balanced-N=100 Q16 logits.

This is an inference/oracle exporter, not a training script.  It requires
NumPy and CPU PyTorch.  The WSL one-click runner consumes the pre-generated
logits and does not require PyTorch.
"""
from __future__ import annotations
import argparse, hashlib, json
from pathlib import Path
import numpy as np

Q=65536
NEG_CLIP=-8*Q
POS_CLIP=8*Q

def sha256(path:Path)->str:
    h=hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda:f.read(1<<20),b''): h.update(b)
    return h.hexdigest()

def main()->int:
    ap=argparse.ArgumentParser()
    ap.add_argument('--assets',required=True)
    ap.add_argument('--out',required=True)
    a=ap.parse_args()
    try:
        import torch
        import torch.nn.functional as F
        torch.set_num_threads(max(1, min(8, int(__import__('os').environ.get('DAZG_ORACLE_THREADS','8')))))
    except Exception as e:
        raise SystemExit(f'PyTorch is required only to regenerate the oracle: {e}')
    assets=Path(a.assets).resolve(); out=Path(a.out).resolve()
    payload=assets/'payload'
    manifest=json.loads((payload/'payload_manifest.json').read_text())
    W={}
    for e in manifest['files']:
        if e.get('dtype')=='uint64' or str(e.get('file','')).endswith('.q16.u64.npy'):
            arr=np.load(payload/e['file'],allow_pickle=False)
            W[e['state_key']]=torch.from_numpy(arr.view(np.int64).copy())
    if len(W)!=77: raise SystemExit(f'expected 77 Q16 tensors, found {len(W)}')
    table_np=np.fromfile(assets/'stage_s/stage_s_gelu_q16_i64.bin',dtype='<i8')
    if table_np.shape!=(1048577,): raise SystemExit(f'bad Stage-S table shape: {table_np.shape}')
    T=torch.from_numpy(table_np.copy())
    x_np=np.load(assets/'reference/qahl_ref_input_n10.npy',allow_pickle=False)
    labels=np.load(assets/'reference/qahl_ref_labels_n10.npy',allow_pickle=False).astype(np.int64)
    ref=json.loads((assets/'reference/balanced_n100_reference.json').read_text())
    x=torch.from_numpy(x_np.view(np.int64).copy())

    def qconv(z,base,stride=1,padding=0):
        acc=F.conv2d(z,W[base+'.weight'],None,stride=stride,padding=padding)
        return torch.div(acc,Q,rounding_mode='floor')+W[base+'.bias'].view(1,-1,1,1)
    def qlinear(z,base):
        acc=z@W[base+'.weight'].t()
        return torch.div(acc,Q,rounding_mode='floor')+W[base+'.bias'].view(1,-1)
    def gelu_s(z):
        y=torch.empty_like(z); neg=z<=NEG_CLIP; pos=z>=POS_CLIP; mid=~(neg|pos)
        y[neg]=0; y[pos]=z[pos]; y[mid]=T[(z[mid]-NEG_CLIP).long()]
        return y
    def unit(z,c0,c1,p=0):
        return z+qconv(gelu_s(qconv(z,c0,padding=p)),c1)
    def transition(z,prefix):
        main=qconv(z,prefix+'.main.0',stride=2,padding=1)
        main=qconv(gelu_s(main),prefix+'.main.3.conv')
        skip=qconv(z,prefix+'.skip',stride=2)
        return unit(main+skip,prefix+'.tail.net.0.conv',prefix+'.tail.net.3.conv')

    x=qconv(x,'stem.0',padding=1)
    x=gelu_s(x)
    x=qconv(x,'stem.2.conv')
    x=unit(x,'stem.3.net.0.conv','stem.3.net.3.conv')
    x=unit(x,'h32.0.body.0','h32.0.body.3.conv',p=1)
    x=unit(x,'h32.0.anchor.net.0.conv','h32.0.anchor.net.3.conv')
    x=unit(x,'h32.1.net.0.conv','h32.1.net.3.conv')
    x=transition(x,'to_h16')
    x=unit(x,'h16.0.body.0','h16.0.body.3.conv',p=1)
    x=unit(x,'h16.0.anchor.net.0.conv','h16.0.anchor.net.3.conv')
    x=unit(x,'h16.1.net.0.conv','h16.1.net.3.conv')
    x=transition(x,'to_h8')
    x=unit(x,'h8.0.body.0','h8.0.body.3.conv',p=1)
    x=unit(x,'h8.0.anchor.net.0.conv','h8.0.anchor.net.3.conv')
    chunks=torch.chunk(x,4,dim=1); outs=[]; scale=W['h8.1.bucket_scale']
    for i,c in enumerate(chunks):
        y=qconv(c,f'h8.1.local.{i}.conv')
        y=torch.bitwise_right_shift(y*scale[i].view(1,-1,1,1),16)
        outs.append(y)
    x=gelu_s(torch.cat(outs,dim=1))
    x=qconv(x,'h8.1.mix.conv')
    x=unit(x,'h8.2.net.0.conv','h8.2.net.3.conv')
    x=torch.div(x.sum(dim=(2,3)),64,rounding_mode='trunc')
    logits=qlinear(x,'head.2').cpu().numpy().astype(np.int64)
    top5=np.argsort(-logits,axis=1,kind='stable')[:,:5]
    ref_top5=np.asarray(ref['q16_top5_predictions'],dtype=np.int64)
    top1=int(np.sum(top5[:,0]==labels))
    top5_correct=int(sum(int(labels[i] in top5[i]) for i in range(100)))
    if not np.array_equal(top5,ref_top5) or (top1,top5_correct)!=(72,91):
        raise SystemExit(f'oracle gate failed: rows_exact={np.array_equal(top5,ref_top5)} top1={top1} top5={top5_correct}')
    out.parent.mkdir(parents=True,exist_ok=True)
    np.save(out,logits,allow_pickle=False)
    report={'status':'PASS','shape':list(logits.shape),'top1':top1,'top5':top5_correct,
            'reference_top5_rows_exact':100,'output':str(out),'sha256':sha256(out),
            'retraining_required':False}
    print(json.dumps(report,indent=2))
    return 0
if __name__=='__main__': raise SystemExit(main())
