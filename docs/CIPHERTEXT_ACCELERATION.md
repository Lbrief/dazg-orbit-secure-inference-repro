# Ciphertext acceleration and rotation reduction

DAZG-Orbit keeps optimization claims tied to exact arithmetic gates. Every
accelerated route must reproduce the frozen Q16 logits before it is accepted.

## Block-circulant packing and cyclic NTT

Block-circulant weight structure reduces independent packed diagonals. The
cyclic NTT preprocessing path converts each active block in `O(n log n)` time
and produces plaintext polynomials consumed by homomorphic multiplication.

## Sparse BSGS rotation scheduling

The baby-step/giant-step scheduler enumerates only active diagonals. Rotated
ciphertexts are cached by packed offset and reused across output blocks, which
avoids repeated Galois/key-switch operations when several terms request the
same rotation. Inactive diagonals are skipped instead of materialising zero
rotations.

## K3/S2 phase packing

The PCOI K3/S2 route separates the four stride-2 spatial phases, accumulates
phase contributions in the HE domain, and performs a single terminal
conversion. The PD-GIANT schedule defers the final giant-step fold so it can be
shared. A correctness-first generic route remains available and is selected
whenever an optimized route fails an exactness gate.

## Conversion batching

Secret-share/ciphertext conversion batches independent polynomials with a
bounded worker count. This changes scheduling only; tensor order, ring
semantics, and output ownership are preserved.

## Counters and interpretation

Where exposed, runners report ciphertext rotations, `mul_plain`,
`add_inplace`, communication bytes, and protocol rounds. The N=10 lane exposes
these counters. N=100 does not expose a trustworthy isolated rotation or
HE-only timer, so those values must not be inferred from pair wall time.

All currently validated lanes use the reveal correctness backend and retain
`security_claim=0`.
