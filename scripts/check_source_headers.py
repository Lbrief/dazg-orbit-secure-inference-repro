#!/usr/bin/env python3
# DAZG-Orbit Project Source File
# Component: scripts/check_source_headers.py
# Purpose: Require an English DAZG-Orbit header on maintained source files.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.

from pathlib import Path
import sys
ROOT=Path(__file__).resolve().parents[1]
SKIP={'Extern','LICENSES','checkpoints','results','.git','build','runs','dist','__pycache__'}
files=[]; errors=[]
for p in ROOT.rglob('*'):
    if not p.is_file(): continue
    rel=p.relative_to(ROOT)
    if any(part in SKIP for part in rel.parts): continue
    ext=p.suffix.lower()
    if not (ext in {'.cpp','.cc','.cxx','.h','.hpp','.py','.sh','.cmake','.yml','.yaml'} or p.name in {'CMakeLists.txt','Makefile','reproduce.sh'}): continue
    files.append(p)
    try: head=p.read_text('utf-8',errors='replace')[:1400]
    except OSError: continue
    if 'DAZG-Orbit Project Source File' not in head: errors.append(str(rel))
if errors:
    print('[SOURCE HEADER CHECK FAIL]'); print('\n'.join(errors[:200])); sys.exit(2)
print(f'[SOURCE HEADER CHECK PASS] files={len(files)}')
