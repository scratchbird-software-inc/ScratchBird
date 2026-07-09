# ScratchBird Installer Builders

This directory contains source-tracked installer recipes and verification tools.
Generated binaries, installers, logs, SBOM files, and proof bundles are build
artifacts. They belong in GitHub Actions artifacts or release assets, not in the
source tree.

The builders consume the public standalone output tree created by the release
build:

```bash
cmake --preset public-release-linux
cmake --build --preset public-release-linux
python3 project/tools/release/verify_public_release_bundle.py \
  build/public-release-linux/output/linux
```

Then build installers:

```bash
python3 project/tools/installers/build_installers.py \
  --platform linux \
  --artifact-root build/public-release-linux/output/linux \
  --output-root build/installers/linux \
  --version 0.0.0-nightly
```

macOS uses the same public output contract, with native x86_64 and arm64
builds produced on GitHub-hosted macOS runners:

```bash
cmake --preset public-release-macos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
cmake --build --preset public-release-macos
ctest --preset public-release-macos
python3 project/tools/release/verify_public_release_bundle.py \
  build/public-release-macos/output/macos
python3 project/tools/installers/build_installers.py \
  --platform macos \
  --artifact-root build/public-release-macos/output/macos \
  --output-root build/installers/macos-arm64 \
  --version 0.0.0-nightly
python3 project/tools/installers/verify_installer_artifacts.py \
  --platform macos \
  --artifact-root build/installers/macos-arm64
project/tools/installers/smoke_install_macos.sh \
  build/installers/macos-arm64/scratchbird-macos-0.0.0-nightly.tar.gz \
  build/install-smoke/macos-arm64-tar
```

When both per-architecture macOS tarballs have passed verification, a universal
QA tarball can be assembled on a macOS runner:

```bash
python3 project/tools/installers/make_macos_universal.py \
  --x86-tar build/installers/macos-x86_64/scratchbird-macos-0.0.0-nightly.tar.gz \
  --arm-tar build/installers/macos-arm64/scratchbird-macos-0.0.0-nightly.tar.gz \
  --output-root build/installers/macos-universal \
  --version 0.0.0-nightly-universal
```

Rules:

- `packaging/` is not an installer input authority.
- Installer inputs come from the clean public output tree and source-tracked
  installer recipes only.
- Generated installers must carry manifests, checksums, and proof sidecars.
- macOS generated installers must carry launchd plists, a dynamic-library audit,
  support-matrix metadata, and signing-state metadata.
- macOS smoke tests record fresh install, upgrade overlay, uninstall/removal,
  and config-preservation proof for extracted QA payloads.
- macOS universal artifacts are optional QA artifacts assembled only after both
  per-architecture artifacts verify.
- macOS QA packages are unsigned unless release signing variables are provided;
  unsigned QA artifacts must not be promoted as final signed installers.
- Private paths, private workplans, acquired reference payloads, and secrets are
  forbidden in installer artifacts.

## Webserver package export

The manual GitHub workflow `.github/workflows/webserver-package-export.yml`
builds and verifies packages, then assembles a webserver upload bundle as a
GitHub Actions artifact. It does not create a GitHub release, tag, or
pre-release.

Run it from GitHub Actions with:

- `platform`: `all`, `linux`, `windows`, or `macos`
- `version`: the package version string
- `channel`: `beta`, `pre-release`, `release-candidate`, `nightly`, or `qa`
- `base-url`: the public download host URL to record in the manifest
- `require-msi`: whether Windows MSI generation is mandatory
- `retention-days`: temporary GitHub artifact retention window

The workflow output artifact is named `scratchbird-webserver-package-export`.
It contains:

- `WEB_DISTRIBUTION_MANIFEST.json`
- `SHA256SUMS`
- `UPLOAD_LAYOUT.txt`
- platform package files under `<channel>/<version>/<platform>/<arch>/`

The upload bundle is intended to be copied to the ScratchBird webserver after
independent approval. GitHub Actions artifact storage is only a staging point.
