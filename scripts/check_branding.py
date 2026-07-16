#!/usr/bin/env python3
# DAZG-Orbit Project Source File
# Component: scripts/check_branding.py
# Purpose: Enforce active DAZG-Orbit naming while preserving legal provenance.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.

from pathlib import Path
import re, sys
ROOT=Path(__file__).resolve().parents[1]
EXCLUDED_TOP={'Extern','LICENSES','.git','build','runs','dist','__pycache__'}
ALLOWED={Path('NOTICE_UPSTREAM.md'),Path('THIRD_PARTY_LOCK.json'),Path('docs/BRANDING_AND_ATTRIBUTION.md'),Path('scripts/check_branding.py')}
PATTERN=re.compile(r'PrivCirNet|SEC-PPDL|SEC_PPDL|sec_ppdl|secppdl|AegisPI|AEGISPI|AEGIS_PI|aegispi',re.I)
errors=[]; scanned=0
for path in ROOT.rglob('*'):
    rel=path.relative_to(ROOT)
    if any(part in EXCLUDED_TOP for part in rel.parts): continue
    if rel in ALLOWED: continue
    if PATTERN.search(rel.as_posix()): errors.append(f'legacy active path: {rel}')
    if not path.is_file(): continue
    data=path.read_bytes()
    if b'\0' in data: continue
    try: text=data.decode('utf-8')
    except UnicodeDecodeError: continue
    scanned+=1
    m=PATTERN.search(text)
    if m: errors.append(f'legacy active token: {rel}:{text.count(chr(10),0,m.start())+1}: {m.group(0)}')
if errors:
    print('[BRANDING CHECK FAIL]'); print('\n'.join(errors[:200])); sys.exit(2)
print(f'[BRANDING CHECK PASS] active_name=DAZG-Orbit scanned_text_files={scanned} legacy_hits=0')
