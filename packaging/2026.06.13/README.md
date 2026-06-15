# ScratchBird Pre-Release Bundle 2026.06.13

This directory is the private pre-release bundle for the 2026.06.13 test
cycle. It is not an official release.

Use this tree for finalized candidate artifacts that need to be visible to
testers and platform teams:

- `installers/` - MSI, EXE, DEB, RPM, tar.gz, AUR source package, PKG, DMG.
- `drivers/` - driver package downloads and driver proof sidecars.
- `reference-parsers/` - compatibility parser package downloads.
- `udr/` - parser-support and non-parser UDR packages.
- `docs/` - generated PDF, HTML, examples, and documentation bundles.
- `proofs/` - public build, CTest, compatibility, security, SBOM, signing, and
  release-verifier proof bundles.
- `source/` - source tarballs generated from exact commits or tags.
- `server/` - standalone server/runtime bundles when not packaged as
  installers.
- `tools/` - command-line and administration tool bundles.

Promotion is explicit. Do not copy from `build/` manually unless a platform
owner is intentionally dropping a final candidate and then regenerating
`RELEASE_MANIFEST.json` and `SHA256SUMS`.

Required after every artifact change:

```bash
python3 project/tools/release/promote_prerelease_bundle.py \
  --release-date 2026.06.13

python3 project/tools/release/verify_prerelease_packaging_bundle.py \
  packaging/2026.06.13
```

While this bundle is still an empty seeded layout, use:

```bash
python3 project/tools/release/verify_prerelease_packaging_bundle.py \
  packaging/2026.06.13 --allow-empty
```

After any real installer, driver, parser, UDR, documentation, source, or proof
artifact is promoted, omit `--allow-empty` so the verifier rejects an empty
bundle.
