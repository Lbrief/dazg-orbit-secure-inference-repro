# Third-party dependencies

The repository vendors pruned source snapshots to avoid network-dependent builds:

| Component | Version / identity | License |
|---|---|---|
| Microsoft SEAL | 4.1.2 | MIT |
| Intel HEXL | 1.2.6 | Apache-2.0 |
| emp-tool | pinned commit in `THIRD_PARTY_LOCK.json` | MIT |
| emp-ot | pinned commit in `THIRD_PARTY_LOCK.json` | MIT |

Pinned identities are recorded in `THIRD_PARTY_LOCK.json`. Benchmark outputs, caches, large unused tests, and unrelated historical files were removed. Minimal SEAL CMake templates required for configuration remain present.

Third-party license and notice files are retained in the corresponding `Extern/` subdirectories.
