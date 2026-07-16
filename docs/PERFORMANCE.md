# Performance

Timing is platform-dependent. Accuracy and exactness gates are deterministic;
wall time is not. All figures below use the reveal correctness backend and
must not be presented as no-reveal deployment performance.

## Latest DAZG-Orbit v3 validation

| Lane | Pair wall total | Mean | Min / median / max |
|---|---:|---:|---:|
| P60 N=10 | 70.740 s | 7.074 s/sample | 6.884 / 7.065 / 7.253 s |
| Checkpoint-013 N=100 | 199.043 s | 1.990 s/sample | 1.922 / 1.968 / 2.280 s |

The complete validation sessions are longer because they include preflight,
Stage-S, scorer self-tests, route selection, sample-0, and first-10 gates.

## N=10 exposed HE and protocol counters

Per sample:

- HE compute: 3169880.3 us;
- rotations: 125;
- `mul_plain`: 3463;
- `add_inplace`: 3165;
- communication: 183332572 bytes;
- protocol rounds: 75.

## N=100 counter boundary

The checkpoint-013 N=100 executable records pair wall time and exactness but
does not expose a trustworthy isolated rotation count or HE-only timer. Those
values must not be inferred from pair wall time. Server and client execute
concurrently, so their process times must not be added to obtain pair time.

Compact per-sample tables are stored under `results/`. Raw logs are regenerated
locally under `runs/` and are not included in the release archive.
