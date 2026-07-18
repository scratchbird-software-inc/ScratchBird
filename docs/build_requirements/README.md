# Supported Platform Matrix

This public source-review release separates support-eligible platform requirements
from production support claims.

No support claim is made for a platform until that platform's native proof lane
has passed and the release evidence names it as supported.

## Common Requirements

| Platform | Status | Requirement boundary |
| --- | --- | --- |
| Linux x86_64, Ubuntu 24.04 LTS | Fully proven first target | Native configure, build, CTest, install, public export, and release evidence are required before release. |
| Windows x64, Windows 11 or Windows Server 2022/2025 | Target platform pending native CI/runtime proof | Native runner evidence must pass before support is claimed. |
| FreeBSD x86_64, FreeBSD 14.x | Target platform pending native runner proof | Native runner evidence must pass before support is claimed. |
| macOS 14+ x86_64 and arm64 | Target platform pending native GitHub CI/runtime proof | Native macos-15-intel and macos-15 runner evidence must pass before support is claimed. |

All support-eligible platforms must provide before support is claimed:

- CMake 3.25 minimum
- Ninja 1.11 or newer
- C++23 compiler
- Python 3.11 or newer
- OpenSSL 3.x development headers and libraries
- ICU development headers and libraries
- LibXML2 development headers and libraries
- zlib development headers and libraries
- LZ4 development headers and libraries
- Zstd development headers and libraries
- GEOS and PROJ
- GoogleTest
- ODBC SDK or driver manager
- LLVM 23 or newer
- clang-tidy 18 or newer
- cppcheck
- ASan
- UBSan
- TSan

macOS GitHub-hosted proof runners currently use Homebrew LLVM 22, and the macOS
public-release preset records that exception with `SB_LLVM_MIN_MAJOR=22`.
Windows GitHub-hosted proof workflows use the current MSYS2 UCRT64 LLVM 22
package and pass the same bounded exception explicitly; other Windows release
proofs retain the common LLVM 23 floor.
LLVM is mandatory at runtime as well as at build time. Release binaries do not
embed a Linux or Windows build-machine path: Linux loads the versioned LLVM
SONAME supplied by `libllvm23`/`llvm-libs >= 23`, while the Windows bundle
ships the configured LLVM DLL and its import closure beside the executables.
macOS QA packages deliberately do not bundle Homebrew LLVM; testers must run
`brew install llvm`, and the build records the stable
`$(brew --prefix llvm)/lib/libLLVM.dylib` runtime path. This external macOS
prerequisite is part of the package support metadata and dynamic-library audit.
Release-complete binaries redact the producer's LLVM source, tools, and staging
directories; a post-build binary gate rejects those configured paths if they
survive in `SBsrv` or `SBcore`.

Every support-eligible platform must prove before support is claimed:

- `SB_BUILD_PUBLIC_RELEASE_CORRECTNESS=ON`
- `SB_ENABLE_CLUSTER_PROVIDER=OFF`
- `SB_CLUSTER_PROVIDER_STUB=ON`
- `SB_LLVM_LINK_MODE=dynamic`
- `SB_LLVM_RUNTIME_LIBRARY` is either a relocatable SONAME/DLL basename or the
  declared stable Homebrew `opt` path; an absolute Linux/Windows build path is
  rejected for release-complete builds.
- `ctest --test-dir` release gates for `public_release_correctness`
- `ctest --test-dir` release gates for `engine_listener_enterprise`
- macOS artifacts must include launchd payload proof, dynamic-library path audit, and signing state proof.

External cluster-provider proof only when the closed cluster library is supplied
outside the public core tree.
