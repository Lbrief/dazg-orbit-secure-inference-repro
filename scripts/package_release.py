#!/usr/bin/env python3
# DAZG-Orbit Project Source File
# Component: scripts/package_release.py
# Purpose: Fail-closed experiment, verification, or Q16 oracle component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import stat
import tarfile
import tempfile
from pathlib import Path

EXCLUDED_DIRS = {'.git', 'build', 'runs', 'dist', '.venv', '__pycache__', '.idea', '.vscode'}
EXCLUDED_SUFFIXES = {'.pyc', '.pyo', '.log', '.pdf', '.core', '.tmp', '.tar', '.tgz', '.zip'}
EXCLUDED_NAMES = {'MANIFEST.sha256'}


def excluded(path: Path, root: Path) -> bool:
    rel = path.relative_to(root)
    if any(part in EXCLUDED_DIRS for part in rel.parts):
        return True
    if path.name in EXCLUDED_NAMES:
        return True
    lower = path.name.lower()
    if lower.endswith('.tar.gz'):
        return True
    return path.suffix.lower() in EXCLUDED_SUFFIXES


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def copy_tree(src: Path, dst: Path) -> None:
    for path in sorted(src.rglob('*')):
        if excluded(path, src):
            continue
        rel = path.relative_to(src)
        target = dst / rel
        if path.is_symlink():
            link = os.readlink(path)
            if os.path.isabs(link):
                raise RuntimeError(f'absolute symlink is not releasable: {rel} -> {link}')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.symlink_to(link)
        elif path.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        elif path.is_file():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
            # Normalize Git executable bits: only runnable entry points are 0755.
            executable = path.name == "reproduce.sh" or path.suffix == ".sh"
            if path.suffix == ".py":
                try:
                    executable = path.read_bytes().startswith(b"#!")
                except OSError:
                    executable = False
            target.chmod(0o755 if executable else 0o644)


def write_manifest(root: Path) -> None:
    rows = []
    for path in sorted(root.rglob('*')):
        if path.is_file() and path.name != 'MANIFEST.sha256':
            rows.append(f'{sha256(path)}  {path.relative_to(root).as_posix()}')
    (root / 'MANIFEST.sha256').write_text('\n'.join(rows) + '\n', encoding='utf-8')


def verify_tree(root: Path) -> None:
    forbidden = []
    for path in root.rglob('*'):
        if path.is_symlink() and os.path.isabs(os.readlink(path)):
            forbidden.append(f'absolute symlink: {path}')
        if path.is_file():
            lower = path.name.lower()
            if lower.endswith(('.pdf', '.log', '.core', '.pyc', '.pyo', '.tar', '.tar.gz', '.tgz', '.zip')):
                forbidden.append(f'forbidden file: {path}')
            if path.stat().st_size >= 100_000_000:
                forbidden.append(f'file >=100MB: {path}')
            mode = stat.S_IMODE(path.stat().st_mode)
            executable_expected = path.name == "reproduce.sh" or path.suffix == ".sh"
            if path.suffix == ".py":
                try:
                    executable_expected = path.read_bytes().startswith(b"#!")
                except OSError:
                    executable_expected = False
            if mode & 0o111 and not executable_expected:
                forbidden.append(f'unexpected executable bit: {path}')
    if forbidden:
        raise RuntimeError('\n'.join(forbidden))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', default=str(Path(__file__).resolve().parents[1]))
    parser.add_argument('--output', default='')
    args = parser.parse_args()
    src = Path(args.root).resolve()
    out = Path(args.output).resolve() if args.output else src / 'dist' / 'dazg-orbit-secure-inference-repro-github-ready.tar.gz'
    out.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix='dazg-release-') as tmp:
        stage = Path(tmp) / 'dazg-orbit-secure-inference-repro'
        stage.mkdir()
        copy_tree(src, stage)
        write_manifest(stage)
        verify_tree(stage)
        with tarfile.open(out, 'w:gz', format=tarfile.PAX_FORMAT) as tf:
            tf.add(stage, arcname=stage.name, recursive=True)

    with tarfile.open(out, 'r:gz') as tf:
        members = tf.getmembers()
        if not members:
            raise RuntimeError('empty release archive')
        if any(m.name.endswith(('.pdf', '.log', '.pyc', '.tar.gz', '.zip')) for m in members):
            raise RuntimeError('release archive contains a forbidden member')
    digest = sha256(out)
    print(f'[RELEASE PACKAGE] {out}')
    print(f'[SIZE BYTES] {out.stat().st_size}')
    print(f'[SHA256] {digest}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
