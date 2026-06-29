# ScratchBird Packaging Staging

This tree is a temporary private pre-release distribution area.

It is intentionally trackable while the ScratchBird repository remains private
so non-coding testers and platform teams can exchange candidate installers,
drivers, reference parsers, UDR packages, documentation bundles, source
archives, and public proof bundles from one dated location.

This tree is not source code and is not an official release channel. Before the
first official public source release, `packaging/` must be removed from public
source history or moved to release hosting/package repositories.

Rules:

- `build/` remains disposable and must never be committed.
- Files enter `packaging/<date>/` only through an explicit promotion command or
  an intentional platform-team artifact drop.
- Every promoted bundle must contain `PRE_RELEASE_NOT_FINAL.txt`,
  `RELEASE_MANIFEST.json`, and `SHA256SUMS`.
- Bundled proofs must be public-release proofs. Do not copy private workplans,
  private audits, private notes, or ScratchBird-Private paths into this tree.
- Each artifact must have a checksum and an entry in the release manifest.

Promotion command:

```bash
python3 project/tools/release/promote_prerelease_bundle.py \
  --release-date 2026.06.13 \
  --copy build/output/linux-x86_64/scratchbird.deb=installers/linux-x86_64/deb/ \
  --copy build/docs/scratchbird-docs.pdf=docs/pdf/
```

Verification command:

```bash
python3 project/tools/release/verify_prerelease_packaging_bundle.py \
  packaging/2026.06.13
```

Driver promotion command:

```bash
export SB_DRIVER_BETA_MATRIX=/path/to/DRIVER_COMPLETE_COVERAGE_CHECKLIST_MATRIX.csv
python3 packaging/scripts/promote_driver_release_artifacts.py \
  --matrix "$SB_DRIVER_BETA_MATRIX"
```

Driver promotion verification:

```bash
export SB_DRIVER_BETA_MATRIX=/path/to/DRIVER_COMPLETE_COVERAGE_CHECKLIST_MATRIX.csv
python3 packaging/scripts/promote_driver_release_artifacts.py \
  --verify-only \
  --matrix "$SB_DRIVER_BETA_MATRIX"
```
