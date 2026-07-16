# Public release checklist

Before publishing on GitHub:

- [ ] Select and add a first-party `LICENSE`.
- [ ] Confirm checkpoint-013 redistribution rights.
- [ ] Confirm redistribution rights for fixed CIFAR-100-derived tensors.
- [ ] Replace the generic author entry in `CITATION.cff`.
- [ ] Run `./reproduce.sh release`.
- [ ] Verify the generated archive SHA-256 and `MANIFEST.sha256`.
- [ ] Confirm that no `build/`, `runs/`, PDF, log, cache, nested archive, or credential is present.
- [ ] Keep the reveal/`security_claim=0` disclosure in the README and paper artifact statement.
- [ ] Do not claim N=1000 or no-reveal deployment from this release.
