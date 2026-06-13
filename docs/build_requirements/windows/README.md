# Windows Build Requirements

Targets: Windows 11 x64 and Windows Server 2022/2025 x64. Win32 is not a supported release target. Win32 is not a
supported release target; use x64 tools, x64 dependencies, x64 Python.

Use MSYS2 UCRT64 with GNU C++ and gcc 15. Install or provide:

- CMake 3.29
- Ninja 1.11
- Python 3.12
- mingw-w64-ucrt-x86_64-openssl
- mingw-w64-ucrt-x86_64-icu
- mingw-w64-ucrt-x86_64-libxml2
- mingw-w64-ucrt-x86_64-zlib
- mingw-w64-ucrt-x86_64-lz4
- mingw-w64-ucrt-x86_64-zstd
- mingw-w64-ucrt-x86_64-geos
- mingw-w64-ucrt-x86_64-proj
- mingw-w64-ucrt-x86_64-gtest
- odbc32
- LLVM 23+
- clang-tidy
- cppcheck
- MSVC warnings-as-errors
- SB_PUBLIC_RELEASE_WARNINGS_AS_ERRORS=ON

Native proof contract:

```powershell
cmake -S project -B build-windows-public-release-proof -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake -DSB_BUILD_TESTS=ON -DSB_BUILD_PUBLIC_RELEASE_CORRECTNESS=ON -DSB_NONCLUSTER_ENGINE_PROFILE=release-complete -DSB_ENABLE_CLUSTER_PROVIDER=OFF -DSCRATCHBIRD_ENABLE_DEBUG_LOGS=OFF -DSCRATCHBIRD_ENABLE_HOTPATH_TRACE=OFF -DSCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=OFF -DSCRATCHBIRD_ENABLE_PREPARED_TRACE=OFF -DSB_LLVM_LINK_MODE=dynamic
cmake --build build-windows-public-release-proof -j 2
ctest --test-dir build-windows-public-release-proof -L public_release_correctness --output-on-failure
ctest --test-dir build-windows-public-release-proof -L engine_listener_enterprise --output-on-failure
```

cluster execution succeeds without the external cluster provider only after
native public release evidence passes.
