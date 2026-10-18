# Release Checklist

Use this checklist for stable repository releases.

## Before Tagging

- [ ] Worktree is clean and `main` is synchronized with `origin/main`.
- [ ] Pull request for the release changes is merged.
- [ ] GitHub Actions CI is green for the exact commit that will be tagged.
- [ ] `git diff --check` passes.
- [ ] No local `.env`, `secrets.h`, Keil GUI-state files, or generated build directories are tracked.
- [ ] README, documentation, and `CHANGELOG.md` describe the release accurately.
- [ ] Known limitations are documented.

## Create the Tag

Create an annotated tag from the verified `main` commit:

```bash
git tag -a v1.0.0 -m "IoT System v1.0.0"
git push origin v1.0.0
```

Do not move or rewrite a published stable tag. If a released version needs a correction, create a new patch release.

## Package Source

Create a release archive from the tag instead of the working tree:

```bash
mkdir -p dist

git archive \
  --format=tar.gz \
  --prefix=iot-system-v1.0.0/ \
  -o dist/iot-system-v1.0.0-source.tar.gz \
  v1.0.0

(
  cd dist
  sha256sum iot-system-v1.0.0-source.tar.gz > SHA256SUMS
)
```

Verify the checksum before upload:

```bash
cd dist
sha256sum -c SHA256SUMS
```

## GitHub Release

- [ ] Title: `IoT System v1.0.0`.
- [ ] Use `docs/releases/v1.0.0.md` as the release-note basis.
- [ ] Attach `iot-system-v1.0.0-source.tar.gz`.
- [ ] Attach `SHA256SUMS`.
- [ ] Mark the release as the latest stable release.
- [ ] Confirm README links and the release badge render correctly after publication.

## After Release

- [ ] Confirm `git status -sb` is clean.
- [ ] Confirm `git tag --list` contains the published tag.
- [ ] Confirm `gh release view v1.0.0` shows the expected assets.
- [ ] Treat the released tag as immutable.
