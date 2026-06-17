# Packaging and Build Targets

Status: public specification baseline
Search key: `INSTALLER-BUILD`

This document defines the target matrix, the native packaging technology per
platform, and the build/signing strategy. The strategy is **cross-compile first**:
build and sign as much as possible from a single Linux host to keep cost low, and
treat rented native CI as a test/QA and fallback resource rather than a build
dependency.

## Target matrix

| Platform | Arch | Artifact | State |
| --- | --- | --- | --- |
| Linux | x86_64, arm64 | native `deb` + `rpm`, element packages + persona metapackages; tarball; (Flatpak/Snap/AppImage for portable) | supported |
| Windows | x64, arm64 | WiX **Burn** bundle over per-element MSIs / merge modules | supported |
| macOS | universal (x86_64 + arm64) | distribution `.pkg` over component `.pkg`s | supported |
| Android | arm64 (+ others) | AAR / Maven (driver); engine `.so` via NDK | planned |
| iOS | arm64 | Swift Package / CocoaPods / XCFramework | planned |

`state` matches the element catalog's `platforms[].state` (02). `planned` entries
remain first-class in the catalog and documentation so no platform is locked out;
they flip to `supported` as each pipeline begins producing signed artifacts.

## Native packaging technology

| Platform | Coordinator | Service mechanism | Conventions |
| --- | --- | --- | --- |
| Windows | WiX Burn bundle (feature tree, repair/update, Add/Remove) | Windows Service via SCM | `%ProgramFiles%\ScratchBird`, `%ProgramData%\ScratchBird` |
| macOS | `productbuild` distribution `.pkg` with `choices` | launchd LaunchDaemon | `/usr/local/{bin,lib}`, `/Library/Application Support/ScratchBird` |
| Linux | coordinator over native packages + metapackages; apt/dnf repo as later wrapper | systemd unit; postinst creates service user + enables unit | `/var/lib/scratchbird`, `/etc/scratchbird`, journald |

## Cross-compile-first build strategy

| Target | Build from Linux | Sign from Linux | Residual native need |
| --- | --- | --- | --- |
| Linux x86_64/arm64 | native + aarch64 toolchain; distro matrix in containers | GPG repo signing | none |
| Android | NDK on Linux (fully native) | jarsigner / Play signing | device farm for QA |
| Windows x64/arm64 | `clang-cl` + `xwin` (MSVC CRT/SDK) for MSVC ABI, or MinGW-w64; MSI via `msitools`/`wixl` | `osslsigncode` (Authenticode) | a Windows VM/CI occasionally for QA and to verify MSI service/custom-action authoring |
| macOS universal | `osxcross` + macOS SDK → Mach-O, `lipo` for universal | `rcodesign` (sign + notarize + staple via App Store Connect API) | macOS SDK licensing is a gray area; native tests need a Mac → rent minimal macOS CI for QA/fallback |
| iOS | `osxcross`/cctools build the library (XCFramework) | `rcodesign` | app-level packaging realistically wants Xcode/macOS |

Net: Linux, Windows, and Android can be built and signed entirely from the Linux
host; macOS/iOS can be cross-built and signed/notarized from Linux, with a small
rented macOS CI reserved for native test/QA and as a safety net. Cost is metered CI
minutes, not capital, and is incurred mainly for verification rather than building.

## Signing and notarization

| Platform | Signing | Notarization |
| --- | --- | --- |
| Windows | Authenticode (`osslsigncode` from Linux; cloud HSM/cert) | n/a |
| macOS / iOS | `rcodesign` from Linux with an Apple Developer cert | Apple notary submission via `rcodesign` (App Store Connect API key) |
| Linux | GPG-signed packages/repo | n/a |

Caveats to track: `wixl`/`msitools` covers most MSI authoring — verify it handles
the Windows-service `ServiceInstall` and any custom actions for the engine service;
and the `osxcross` SDK path is legally gray and subject to Apple changes, so the
rented macOS CI is the fallback if cross-signing breaks.

## Distribution channels

Native installers are primary. Package-manager channels — winget (Windows),
Homebrew tap (macOS), apt/dnf repositories (Linux) — and the mobile registries
(Maven, Swift Package Index, CocoaPods) are later thin wrappers over the same
payloads and element definitions (02).
