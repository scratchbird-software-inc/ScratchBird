# Supported Platform Matrix

This public source-review release separates support-eligible platform requirements
from production support claims.

## Common Requirements

| Platform | Status | Requirement boundary |
| --- | --- | --- |
| Linux x86_64, Ubuntu 24.04 LTS | Fully proven first target | Native configure, build, CTest, install, public export, and release evidence are required before release. |
| Windows x64, Windows 11 or Windows Server 2022/2025 | Target platform pending native CI/runtime proof | Native runner evidence must pass before support is claimed. |
| FreeBSD x86_64, FreeBSD 14.x | Target platform pending native runner proof | Native runner evidence must pass before support is claimed. |
| macOS | Out of scope for first public release | No support claim. |

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

Every support-eligible platform must prove before support is claimed:

- `SB_BUILD_PUBLIC_RELEASE_CORRECTNESS=ON`
- `SB_ENABLE_CLUSTER_PROVIDER=OFF`
- `SB_CLUSTER_PROVIDER_STUB=ON`
- `SB_LLVM_LINK_MODE=dynamic`
- `ctest --test-dir` release gates for `public_release_correctness`
- `ctest --test-dir` release gates for `engine_listener_enterprise`

External cluster-provider proof only when the closed cluster library is supplied
outside the public core tree.
