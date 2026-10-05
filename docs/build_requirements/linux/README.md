# Linux Build Requirements

Target: Ubuntu 24.04 LTS x86_64.

Install or provide:

- gcc-13
- g++-13
- clang-18
- cmake
- ninja-build
- python3
- libssl-dev
- libicu-dev
- libxml2-dev
- zlib1g-dev
- liblz4-dev
- libzstd-dev
- libgeos-dev
- libproj-dev
- libgtest-dev
- libboost-dev
- libgmp-dev
- libmpfr-dev
- unixodbc-dev
- LLVM 23+
- libllvm23 (runtime; provides the versioned `libLLVM.so.23.*` SONAME)
- clang-tidy-18
- cppcheck
- clang-tidy-18 cppcheck
- ASan and UBSan
- TSan where platform support is available
- SB_PUBLIC_RELEASE_WARNINGS_AS_ERRORS=ON
- SB_PUBLIC_RELEASE_SANITIZER_PROFILE=asan-ubsan

Native proof contract:

GMP and MPFR development headers and libraries are required for the numeric
reference backend. A runtime-only `libmpfr.so` installation is insufficient.
Use the development packages above, or explicitly provide
`SBL_NUMERIC_GMP_INCLUDE_DIR`, `SBL_NUMERIC_GMP_LIBRARY`,
`SBL_NUMERIC_MPFR_INCLUDE_DIR`, and `SBL_NUMERIC_MPFR_LIBRARY` for a non-system
installation. Do not depend on another developer's build directory for headers.

Nested export, install and hardening checks must receive those configured
dependency locations and repeat the reference backend's version, link and
thread-local-runtime checks; cached successes are not transferable proof.

The required hardening gate builds and runs all three profiles (`none`,
`asan-ubsan`, `tsan`). A missing or unloadable sanitizer runtime is a failed
qualification, not a passing waiver. The default nested compiler is the main
build compiler. To select a separately installed, host-compatible sanitizer
toolchain without changing the main compiler, configure both options, for example:

```sh
cmake -S project -B build-linux-public-release-proof \
  -DSB_PUBLIC_RELEASE_HARDENING_C_COMPILER=/usr/bin/clang-21 \
  -DSB_PUBLIC_RELEASE_HARDENING_CXX_COMPILER=/usr/bin/clang++-21
```

This is an explicit toolchain choice, not an automatic fallback after a failed
test. All profiles still require successful configure, build and execution,
with warnings treated as errors. Toolchain smoke probes do not qualify the
entire engine under sanitizers; retain the broader component/regression evidence.

```sh
cmake -S project -B build-linux-public-release-proof -G Ninja -DCMAKE_BUILD_TYPE=Release -DSB_BUILD_TESTS=ON -DSB_BUILD_COMPATIBILITY_PARSERS=OFF -DSB_BUILD_PUBLIC_RELEASE_CORRECTNESS=ON -DSB_NONCLUSTER_ENGINE_PROFILE=release-complete -DSB_ENABLE_CLUSTER_PROVIDER=OFF -DSCRATCHBIRD_ENABLE_DEBUG_LOGS=OFF -DSCRATCHBIRD_ENABLE_HOTPATH_TRACE=OFF -DSCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=OFF -DSCRATCHBIRD_ENABLE_PREPARED_TRACE=OFF -DSB_LLVM_LINK_MODE=dynamic
cmake --build build-linux-public-release-proof -j2
ctest --test-dir build-linux-public-release-proof -L public_release_correctness --output-on-failure
ctest --test-dir build-linux-public-release-proof -L engine_listener_enterprise --output-on-failure
```

cluster execution succeeds without the external cluster provider only for the
public noncluster release-complete profile.

DEB metadata declares `libllvm23`; RPM and AUR recipes declare
`llvm-libs >= 23`. Portable tarball testers must install the equivalent system
runtime package before starting ScratchBird. The binaries load the versioned
SONAME through the system loader and never retain the CI/build-tree path.
