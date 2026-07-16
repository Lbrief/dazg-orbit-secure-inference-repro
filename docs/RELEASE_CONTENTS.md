# Release contents and pruning policy

The GitHub-ready archive retains only material needed to build, execute, verify, or understand the two reproduced experiments:

- first-party C++ runtime and CMake files;
- P60 N=10 source, fixed-point weights, inputs, labels, references, and gates;
- checkpoint-013 N=100 source, 77 Q16 tensors, balanced inputs, labels, oracle, Stage-S table, and gates;
- original checkpoint 013 and minimal Python oracle code;
- pinned pruned third-party source and licenses;
- build/run scripts, GitHub workflows, documentation, and compact result summaries.

The archive excludes:

- PDFs and screenshots;
- raw terminal, server, client, and build logs;
- failed historical patches and one-click packages;
- `build/`, `runs/`, object files, binaries, and caches;
- `__pycache__` and bytecode;
- nested `.tar.gz`/`.zip` files;
- absolute symlinks and local WSL paths;
- unrelated training histories and full datasets.

The full CIFAR-100 dataset is not required because both lanes include their fixed evaluation tensors. Raw run evidence is regenerated locally and remains outside Git.
