# ScratchBird Installer Builders

This directory contains source-tracked installer recipes and verification tools.
Generated binaries, installers, logs, SBOM files, and proof bundles are build
artifacts. They belong in GitHub Actions artifacts or release assets, not in the
source tree.

Nightly tester packages use the `native-sbsql-only` distribution profile. The
proof build may compile broader conformance targets, but installer input is a
separate fail-closed allowlisted tree containing only the branded ScratchBird
server, listener, manager, native SBSQL parser, native command-line tools, and
their native runtime libraries. Compatibility parser workers, parser-support
UDRs, emulation listeners, and test/probe binaries are excluded. Core catalog
charset and collation seed data remains part of the native engine resource pack;
that data does not activate an emulation parser or network protocol.

The native gate verifies every artifact indexed by the initial resource pack:
charset mappings, collations, timezone sources, and the signed/hash-indexed
SBSQL language resource pack. It also verifies every content hash and the
aggregate hash in the default local-password policy pack. Tester-specific TLS
and DBBT keys are never packaged. After extraction or installation, use
`share/scratchbird/examples/native_release_qa/prepare_native_qa_instance.py`
to create a private, TLS-required QA instance with locally generated or
operator-supplied key material.

The native share tree is also allowlisted: complete `resources`,
`docs/public_api`, `docs/release`, `examples/core_beta_qa`, and
`examples/native_release_qa` only. Broad proof-build examples and test harnesses
are not installer inputs and are rejected if they reappear in a native payload.

LLVM remains mandatory in the native profile. Linux binaries load the
versioned system SONAME and package metadata requires `libllvm23` or
`llvm-libs >= 23`. Windows packages bundle the selected LLVM DLL plus its
recursively inspected non-system imports because the engine loads LLVM
explicitly rather than through a PE import. Current macOS QA packages retain
Homebrew LLVM as an external prerequisite: run `brew install llvm` before
testing. The macOS support matrix and audit record that limitation explicitly.

The builders consume the public standalone output tree created by the release
build:

```bash
cmake --preset public-release-linux
cmake --build --preset public-release-linux
python3 project/tools/release/verify_public_release_bundle.py \
  build/public-release-linux/output/linux
python3 project/tools/release/stage_native_release_bundle.py \
  --source-root build/public-release-linux/output/linux \
  --output-root build/native-release-linux/output/linux \
  --platform linux
python3 project/tools/release/verify_native_release_bundle.py \
  build/native-release-linux/output/linux --platform linux
```

Then build installers:

```bash
python3 project/tools/installers/build_installers.py \
  --platform linux \
  --artifact-root build/native-release-linux/output/linux \
  --output-root build/installers/linux \
  --version 0.0.0-nightly \
  --require-native-only
```

The Linux portable tarball remains an extraction-only payload: it does not
create users, groups, system directories, service definitions, databases, or
security sidecars. DEB, RPM, and AUR packages derive from a separate
system-install payload and install `scratchbird-sbsrv.service` disabled and
not started. The unit runs only `SBsrv`; SBsrv owns the shared SBgate listener,
and SBgate owns the standalone native SBParser. The native listener default is
TCP port 3092.

Linux system packages create or verify the dedicated, non-login `scratchbird`
service user and group. They prepare `/var/lib/scratchbird/data`,
`/var/log/scratchbird`, and the server/listener/manager control and runtime
directories below `/run/scratchbird`, all with mode `0750` and explicit
ownership. Default configuration remains under `/etc/scratchbird` with
`root:scratchbird` ownership and mode `0640` for files. Package install does
not create a database and does not use `--create-if-missing`.

An existing same-name identity is accepted only when local passwd, group, and
shadow records are unique and consistent; UID/GID are nonzero, unique, and in
the system-account range; the home and primary group are exact; and the account
is locked behind an approved non-login shell. Its complete effective numeric
group set must contain only the local `scratchbird` GID. A remote, ambiguous,
UID-0, interactive, unlocked, wrong-home, shared-numeric, or supplementary-
authority identity blocks install.

The package lifecycle never adds a human account to the `scratchbird` service
group. Root is the sole create-time OS authorization gate for explicit
`SBsec bootstrap`; after authorization SBsec permanently drops to the locked
service identity so database files are created with the correct owner. Neither
the service user nor its primary group names or grant a database principal,
role, authentication right, or security authority.

The non-privileged Linux system-package smoke extracts and inspects every
lifecycle surface and runs the lifecycle helper against an isolated fixture:

```bash
python3 project/tools/installers/smoke_install_linux_system.py \
  --artifact-root build/installers/linux \
  --work-root build/install-smoke/linux-system
```

An actual DEB install/remove smoke is available for disposable CI hosts but is
opt-in and skips cleanly when noninteractive root is unavailable. Set `CI=true`
or `SB_ALLOW_HOST_PACKAGE_MUTATION=1`, then add
`--run-privileged-deb-install`. It verifies that the unit remains disabled and
inactive and that removal preserves the data root.

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
python3 project/tools/release/stage_native_release_bundle.py \
  --source-root build/public-release-macos/output/macos \
  --output-root build/native-release-macos/output/macos \
  --platform macos
python3 project/tools/release/verify_native_release_bundle.py \
  build/native-release-macos/output/macos --platform macos
python3 project/tools/installers/build_installers.py \
  --platform macos \
  --artifact-root build/native-release-macos/output/macos \
  --output-root build/installers/macos-arm64 \
  --version 0.0.0-nightly \
  --require-native-only
python3 project/tools/installers/verify_installer_artifacts.py \
  --platform macos \
  --artifact-root build/installers/macos-arm64
project/tools/installers/smoke_install_macos.sh \
  build/installers/macos-arm64/scratchbird-macos-0.0.0-nightly.tar.gz \
  build/install-smoke/macos-arm64-tar
```

The macOS portable tarball keeps its extraction-only `etc/scratchbird` layout
and performs no host identity or directory mutation. It contains no launchd
plist or launchd manifest and is foreground-only. The `.pkg` is built from a
separate system payload. Only that system payload carries launchd definitions.
It stores pristine copies of all five native
configuration inputs (`SBsrv.conf`, `SBgate.conf`, `SBmgr.conf`,
`SBParser.conf`, and `SBbootstrap.profile`) below
`/opt/ScratchBird/share/scratchbird/config-defaults`. Its lifecycle helper
copies a default into the canonical live root
`/Library/Application Support/ScratchBird` only when that file is missing, so
an upgrade cannot overwrite an operator configuration. The system payload does
not install a duplicate live configuration under `/etc` and does not create a
compatibility symlink.

The package creates or verifies the local `scratchbird` group and the hidden,
non-login `scratchbird` service identity. It prepares data under
`/var/lib/scratchbird`, logs under `/var/log/scratchbird`, and server,
listener, and manager runtime/control paths under `/var/run/scratchbird`, with
mode `0750` and explicit ownership. It creates no database or security sidecar.
The launchd definitions specify `UserName=scratchbird` and
`GroupName=scratchbird`; only `SBsrv` and optional `SBmgr` are top-level jobs.
Both jobs remain disabled, unloaded, and `RunAtLoad=false` after installation.
`SBsrv` owns the shared `SBgate`, which owns the standalone native `SBParser`.
The sole native default listener port is 3092.

Apple reserves UIDs 0 through 500 for the operating system and recommends a
locally unique UID for each daemon. The lifecycle helper therefore allocates
the first unused local UID in 501 through 59999, marks the account hidden,
locks its password, assigns `/usr/bin/false` as its shell, and rejects an
existing `scratchbird` user record unless its nonzero UID is unique and remains in
that range. Explicit membership of the service user in any other local group,
and nesting of the `scratchbird` group inside any other local group, are
forbidden. The transitive `admin` membership check remains an additional
fail-closed guard. The resolved numeric group inventory must contain only the
primary `scratchbird` GID plus Apple's unavoidable computed `everyone` (12) and
`localaccounts` (61) baselines. The package never adds a human account to the
service group. Root alone authorizes explicit create-time bootstrap; the
numeric service identity is used only for ownership and process execution.

The packaged launchd jobs pass the canonical configuration paths explicitly,
so service operation does not depend on implicit discovery. Interactive
SBsrv/SBmgr invocation without `--config` also requires the binaries' macOS
default-discovery list to include `/Library/Application Support/ScratchBird`;
do not work around an older binary by creating a second live `/etc` tree or a
symlink.

Run the Linux-compatible static and fixture-root lifecycle smoke before hosted
macOS package construction:

```bash
python3 project/tools/installers/smoke_install_macos_system.py \
  --work-root build/install-smoke/macos-system
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
  installer recipes only, but the broad proof output must first pass through
  `stage_native_release_bundle.py`; installer builders use only the resulting
  `build/native-release-*/output/*` tree with `--require-native-only`.
- Generated installers must carry manifests, checksums, and proof sidecars.
- macOS portable tarballs must be foreground-only and contain no launchd plist
  or launchd manifest. macOS system PKGs carry the launchd plists and
  system-install profile; the installer artifact set also carries the
  dynamic-library audit, system-package evidence, support-matrix metadata, and
  signing-state metadata.
- macOS launchd installs only the top-level `SBsrv` and optional `SBmgr`
  services. `SBsrv` owns `SBgate`, and `SBgate` owns `SBParser`; neither child
  component is installed as an independent service.
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
