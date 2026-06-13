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
- unixodbc-dev
- LLVM 23+
- clang-tidy-18
- cppcheck
- clang-tidy-18 cppcheck
- ASan and UBSan
- TSan where platform support is available
- SB_PUBLIC_RELEASE_WARNINGS_AS_ERRORS=ON
- SB_PUBLIC_RELEASE_SANITIZER_PROFILE=asan-ubsan

Native proof contract:

```sh
cmake -S project -B build-linux-public-release-proof -G Ninja -DCMAKE_BUILD_TYPE=Release -DSB_BUILD_TESTS=ON -DSB_BUILD_PUBLIC_RELEASE_CORRECTNESS=ON -DSB_NONCLUSTER_ENGINE_PROFILE=release-complete -DSB_ENABLE_CLUSTER_PROVIDER=OFF -DSCRATCHBIRD_ENABLE_DEBUG_LOGS=OFF -DSCRATCHBIRD_ENABLE_HOTPATH_TRACE=OFF -DSCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=OFF -DSCRATCHBIRD_ENABLE_PREPARED_TRACE=OFF -DSB_LLVM_LINK_MODE=dynamic
cmake --build build-linux-public-release-proof -j2
ctest --test-dir build-linux-public-release-proof -L public_release_correctness --output-on-failure
ctest --test-dir build-linux-public-release-proof -L engine_listener_enterprise --output-on-failure
```

cluster execution succeeds without the external cluster provider only for the
public noncluster release-complete profile.
