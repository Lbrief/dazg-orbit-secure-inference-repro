# Security boundary

This package closes the checkpoint-013 **balanced N=100 correctness and accuracy** gate.
All convolution operators use an exact Q16 diagnostic reveal/re-share backend because the
optimized CirConv layouts were observed to produce sample- and binary-layout-dependent
corruption. The package therefore reports `security_claim=0` and does not claim a no-reveal
secure deployment.

The verified result is:

- two-process reconstructed logits equal the frozen Python Q16 oracle for 100/100 samples;
- Top-1 is 72/100;
- Top-5 is 91/100;
- no N=1000 runner is present.

A later no-reveal package must repair the optimized encrypted convolution kernels and pass
these same exact-logit gates before it may inherit any security claim.
