# Changelog

## 1.1.1 - 2026-07-16

- Hardened JSON report publication for WSL repositories extracted under `/mnt/c`.
- Added retry, verified-copy fallback, and recovery of complete `pair_report.json.tmp.*` files.
- Prevented a successful two-process sample from being misclassified when DrvFS blocks `os.replace()`.
- Added an automated runtime-I/O compatibility test.

## 1.1.0 - 2026-07-16

- Standardized active first-party branding as DAZG-Orbit.
- Added English maintained-source headers and ciphertext-acceleration comments.
- Replaced the private training checkpoint with a 77-tensor weights-only artifact.
- Added branding, source-header, release, and GitHub publication checks.

## 1.0.1 - 2026-07-16

- Revalidated offline build, Stage-S, N=10, and N=100 from a clean extraction.
- Added independent validation evidence.
- Normalized release file modes so only runnable scripts are executable.

## 1.0.0 — 2026-07-16

- Added isolated, offline-buildable P60 N=10 and checkpoint-013 N=100 lanes.
- Preserved lane-specific Stage-S semantics in separate CMake build trees.
- Added frozen assets, Q16 oracle regeneration, fail-closed runners, and compact result summaries.
- Removed PDFs, raw logs, caches, failed historical packages, and non-portable absolute links.
- Added GitHub Actions, release checks, and publication/security documentation.
