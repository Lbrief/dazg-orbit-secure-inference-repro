# Publishing DAZG-Orbit on GitHub

## 1. Validate locally

```bash
./reproduce.sh verify-manifest   # release archive only
./reproduce.sh verify
DAZG_JOBS=8 ./reproduce.sh build
./reproduce.sh stage-s
./reproduce.sh n10
./reproduce.sh n100
```

## 2. Review legal items

Choose a first-party license and confirm redistribution rights for the weights
and fixed evaluation tensors. Preserve `NOTICE_UPSTREAM.md`,
`THIRD_PARTY_LOCK.json`, `LICENSES/`, and vendored dependency notices.

## 3. Create an empty repository

On GitHub, create a repository without adding another README, `.gitignore`, or
license. Start private until the legal review is complete.

## 4. Push

```bash
git init -b main
git add .
git status
git commit -m "Initial DAZG-Orbit reproducibility release"
git remote add origin https://github.com/<OWNER>/<REPOSITORY>.git
git push -u origin main
```

With GitHub CLI:

```bash
gh auth login
gh repo create <OWNER>/<REPOSITORY> --private --source=. --remote=origin --push
```

Do not commit generated `build/`, `runs/`, `dist/`, or release archives.
