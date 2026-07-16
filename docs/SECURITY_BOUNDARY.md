# Security boundary

The included experiments use `adapter_policy=reveal` and declare `security_claim=0`.

They demonstrate:

- exact agreement between two-process execution and the frozen Q16 reference;
- preservation of checkpoint-013 accuracy on the balanced N=100 set;
- protocol orchestration, communication accounting, and deterministic fail-closed gates.

They do not demonstrate that every intermediate activation and adapter remains secret throughout inference. Results must not be described as a complete no-reveal private-inference deployment.

A future secure lane must independently replace reveal/re-share adapters, preserve every exactness and accuracy gate, and include a separate protocol and leakage analysis. N=1000 is also outside this release.
