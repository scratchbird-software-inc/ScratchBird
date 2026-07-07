# ScratchBird Pre-Release Bundle 2026.07.03

This directory is the private pre-release bundle for the 2026.07.03 test
cycle. It is not an official release.

Use this tree for finalized candidate artifacts that need to be visible to
testers and platform teams:

- `installers/` - MSI, EXE, DEB, RPM, tar.gz, AUR source package, PKG, DMG.
- `drivers/` - driver package downloads and driver proof sidecars.
- `adapters/` - application adapter packages and generated adapter payloads.
- `reference-parsers/` - compatibility parser package downloads.
- `udr/` - optional parser-support and non-parser UDR packages.
- `docs/` - generated PDF, HTML, examples, and documentation bundles.
- `proofs/` - public build, CTest, compatibility, security, SBOM, signing, and
  release-verifier proof bundles.
- `source/` - source tarballs generated from exact commits or tags.
- `server/` - engine runtime, IPC server/listener/manager, SBParser, configs,
  server-side SBParser UDR support, and source/support material.
- `tools/` - command-line and administration tool bundles. `SBParser` is staged
  under `server/sbparser/`, not under `tools/`.
- `FILE_LOCATION_MANIFEST.json` - installer-builder map for every staged file,
  including package path, role, checksum, and destination hint.

Promotion is explicit. Do not copy from `build/` manually unless a platform
owner is intentionally dropping a final candidate and then regenerating
`FILE_LOCATION_MANIFEST.json`, `RELEASE_MANIFEST.json`, and `SHA256SUMS`.
