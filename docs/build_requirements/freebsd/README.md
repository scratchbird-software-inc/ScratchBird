# FreeBSD Build Requirements

Target: FreeBSD 14.x x86_64. no native FreeBSD runner produced passing artifacts
until this platform proof is regenerated.

Install or provide:

- cmake
- ninja
- python311
- llvm18
- openssl
- icu
- libxml2
- zlib
- lz4
- zstd
- geos
- proj
- googletest
- unixODBC
- cppcheck
- ASan and UBSan
- TSan where platform support is available
- SB_PUBLIC_RELEASE_WARNINGS_AS_ERRORS=ON
- SB_PUBLIC_RELEASE_SANITIZER_PROFILE=asan-ubsan

Native proof contract:

```sh
cmake -S project -B build-freebsd-public-release-proof -G Ninja -DCMAKE_BUILD_TYPE=Release -DSB_BUILD_TESTS=ON -DSB_BUILD_PUBLIC_RELEASE_CORRECTNESS=ON -DSB_NONCLUSTER_ENGINE_PROFILE=release-complete -DSB_ENABLE_CLUSTER_PROVIDER=OFF -DSCRATCHBIRD_ENABLE_DEBUG_LOGS=OFF -DSCRATCHBIRD_ENABLE_HOTPATH_TRACE=OFF -DSCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=OFF -DSCRATCHBIRD_ENABLE_PREPARED_TRACE=OFF -DSB_LLVM_LINK_MODE=dynamic
cmake --build build-freebsd-public-release-proof -j2
ctest --test-dir build-freebsd-public-release-proof -L public_release_correctness --output-on-failure
ctest --test-dir build-freebsd-public-release-proof -L engine_listener_enterprise --output-on-failure
```

cluster execution succeeds without the external cluster provider only after
native public release evidence passes.
