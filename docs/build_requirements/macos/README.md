# macOS Build Requirements

Target: macOS 14 or newer on native x86_64 and arm64 runners.

GitHub-hosted proof runners:

- x86_64: macos-15-intel
- arm64: macos-15

Install or provide:

- AppleClang 16 or Clang 18+
- Xcode command-line tools
- cmake
- ninja
- python3
- pkg-config
- openssl@3
- icu4c
- libxml2
- zlib
- lz4
- zstd
- geos
- proj
- googletest
- unixodbc
- LLVM 22+ on GitHub-hosted macOS runners
- clang-tidy
- cppcheck
- ASan and UBSan where supported by the runner
- TSan where platform support is available
- SB_PUBLIC_RELEASE_WARNINGS_AS_ERRORS=ON
- SB_PUBLIC_RELEASE_SANITIZER_PROFILE=asan-ubsan

Native proof contract:

```sh
cmake -S project -B build-macos-public-release-proof -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DSB_BUILD_TESTS=ON -DSB_BUILD_PUBLIC_RELEASE_CORRECTNESS=ON -DSB_NONCLUSTER_ENGINE_PROFILE=release-complete -DSB_ENABLE_CLUSTER_PROVIDER=OFF -DSCRATCHBIRD_ENABLE_DEBUG_LOGS=OFF -DSCRATCHBIRD_ENABLE_HOTPATH_TRACE=OFF -DSCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=OFF -DSCRATCHBIRD_ENABLE_PREPARED_TRACE=OFF -DSB_LLVM_LINK_MODE=dynamic -DSB_LLVM_MIN_MAJOR=22 -DSB_PUBLIC_TARGET_PLATFORM=macos
cmake --build build-macos-public-release-proof -j2
ctest --test-dir build-macos-public-release-proof -L public_release_correctness --output-on-failure
ctest --test-dir build-macos-public-release-proof -L engine_listener_enterprise --output-on-failure
```

Installer proof contract:

```sh
python3 project/tools/installers/build_installers.py --platform macos --artifact-root build/public-release-macos/output/macos --output-root build/installers/macos --version 0.0.0-nightly
python3 project/tools/installers/verify_installer_artifacts.py --platform macos --artifact-root build/installers/macos
project/tools/installers/smoke_install_macos.sh build/installers/macos/scratchbird-macos-0.0.0-nightly.tar.gz build/install-smoke/macos-tar
project/tools/installers/smoke_install_macos.sh build/installers/macos/scratchbird-macos-0.0.0-nightly.pkg build/install-smoke/macos-pkg
```

macOS package signing is explicit. QA builds are unsigned and marked by
`MACOS_SIGNING_STATE.json` as not public signed release artifacts. A signed
release requires `SB_MACOS_RELEASE_SIGNING_ENABLED=true`,
`SB_MACOS_DEVELOPER_ID_APPLICATION`, and `SB_MACOS_DEVELOPER_ID_INSTALLER`.
Unsigned QA artifacts must not be promoted as final signed installers.

cluster execution succeeds without the external cluster provider only for the
public noncluster release-complete profile.
