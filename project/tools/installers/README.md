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

Rules:

- `packaging/` is not an installer input authority.
- Installer inputs come from the clean public output tree and source-tracked
  installer recipes only.
- Generated installers must carry manifests, checksums, and proof sidecars.
- Private paths, private workplans, acquired reference payloads, and secrets are
  forbidden in installer artifacts.
