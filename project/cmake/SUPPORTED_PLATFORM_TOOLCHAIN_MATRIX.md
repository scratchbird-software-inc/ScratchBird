# Supported Platform And Toolchain Matrix

This source-level matrix is the durable platform/toolchain policy consumed by
CTest. It is intentionally under `project/cmake/` so validation does not depend
on active execution_plan artifacts.

<!-- SB_PLATFORM_TOOLCHAIN_MATRIX_V1_START -->
matrix_version: 1
cmake_minimum_required: "3.25"
tested_cmake_family: "3.x"
cxx_standard: "C++23"
cxx_extensions: "off"

beta_supported_targets:
  - target_id: "linux-x86_64-glibc-core"
    os: "Linux"
    architecture: "x86_64"
    runtime: "glibc"
    native_runner_required: "Ubuntu 24.04 LTS"
    release_layout: "release/linux/ENGINE_BINARY_LAYOUT.json"
    build_requirements: "docs/build_requirements/linux/README.md"
    build_modes:
      - "noncluster_release_complete"
      - "noncluster_bootstrap"
      - "noncluster_emergency"
    compilers:
      - family: "GCC"
        minimum: "13"
      - family: "Clang"
        minimum: "18"
  - target_id: "windows-x86_64-msvc-core"
    os: "Windows"
    architecture: "x86_64"
    unsupported_architecture_aliases:
      - "Win32"
      - "x86"
      - "i386"
      - "i686"
    unsupported_architecture_behavior: "windows_x64_only_no_win32_release_target"
    runtime: "msvc"
    native_runner_required: "Windows 11 or Windows Server 2022/2025"
    release_layout: "release/windows/ENGINE_BINARY_LAYOUT.json"
    build_requirements: "docs/build_requirements/windows/README.md"
    build_modes:
      - "noncluster_release_complete"
      - "noncluster_bootstrap"
      - "noncluster_emergency"
    compilers:
      - family: "MSVC"
        minimum: "19.40"
  - target_id: "windows-x86_64-gnu-core"
    os: "Windows"
    architecture: "x86_64"
    unsupported_architecture_aliases:
      - "Win32"
      - "x86"
      - "i386"
      - "i686"
    unsupported_architecture_behavior: "windows_x64_only_no_win32_release_target"
    runtime: "ucrt64"
    native_runner_required: "Windows 11 or Windows Server 2022/2025 with MSYS2 UCRT64"
    release_layout: "release/windows/ENGINE_BINARY_LAYOUT.json"
    build_requirements: "docs/build_requirements/windows/README.md"
    build_modes:
      - "noncluster_release_complete"
      - "noncluster_bootstrap"
      - "noncluster_emergency"
    compilers:
      - family: "GCC"
        minimum: "15"
  - target_id: "freebsd-x86_64-elf-core"
    os: "FreeBSD"
    architecture: "x86_64"
    runtime: "libc++"
    native_runner_required: "FreeBSD 14.x"
    release_layout: "release/freebsd/ENGINE_BINARY_LAYOUT.json"
    build_requirements: "docs/build_requirements/freebsd/README.md"
    build_modes:
      - "noncluster_release_complete"
      - "noncluster_bootstrap"
      - "noncluster_emergency"
    compilers:
      - family: "Clang"
        minimum: "18"

explicit_waivers_and_skips:
  - waiver_id: "macos-first-release-out-of-scope"
    applies_to: "macOS/AppleClang"
    behavior: "unsupported_platform_fail_closed_before_configure_or_release_claim"
  - waiver_id: "non_linux_architecture_not_validated"
    applies_to: "non-x86_64 architecture"
    behavior: "unsupported_platform_fail_closed_before_configure_or_release_claim"
  - waiver_id: "win32-first-release-out-of-scope"
    applies_to: "Windows 32-bit / Win32 / x86"
    behavior: "windows_x64_only_no_win32_release_target"

unsupported_platform_diagnostics:
  - diagnostic_id: "SB_DIAG_PLATFORM_UNSUPPORTED_FIRST_RELEASE"
    applies_to:
      - "macOS"
      - "Darwin"
      - "non-x86_64"
      - "Windows 32-bit"
      - "Win32"
      - "x86 Windows"
      - "platform_not_listed_in_beta_supported_targets"
    behavior: "fail_closed_without_support_claim"

driver_adaptor_toolchain_waiver_policy:
  missing_optional_toolchain: "deterministic_skip_with_reason"
  required_core_driver_toolchain: "must_build_or_fail_gate"
  generated_caches: "build_or_tmp_only"
  execution_plan_artifacts: "not_ctest_inputs"

cluster_compile_modes:
  - mode: "noncluster"
    cmake_options:
      - "SB_ENABLE_CLUSTER_PROVIDER=OFF"
      - "SB_CLUSTER_PROVIDER_STUB=OFF"
    behavior: "core_build_uses_no_cluster_provider_and_exact_refusal_vectors"
  - mode: "cluster_stub_boundary"
    cmake_options:
      - "SB_ENABLE_CLUSTER_PROVIDER=ON"
      - "SB_CLUSTER_PROVIDER_STUB=ON"
    behavior: "routes_to_public_stub_provider_boundary_only"
  - mode: "cluster_external_boundary"
    cmake_options:
      - "SB_ENABLE_CLUSTER_PROVIDER=ON"
      - "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY=<path>"
    behavior: "routes_to_external_provider_abi_without_claiming_closed_source_cluster_implementation_in_core"

noncluster_engine_profile_values:
  - "release-complete"
  - "bootstrap"
  - "emergency"

required_policy_tokens:
  - "optional_toolchain_missing_is_skip"
  - "cluster_positive_implementation_outside_core"
  - "no_execution_plan_ctest_dependency"
<!-- SB_PLATFORM_TOOLCHAIN_MATRIX_V1_END -->

## Validation Contract

`database_lifecycle_platform_toolchain_matrix_conformance` checks this file
against `project/CMakeLists.txt` and the configured build metadata generated by
CMake. The gate verifies the minimum CMake version, C++ standard requirement,
current compiler/system/architecture metadata, explicit waiver policy, and the
non-cluster versus cluster-provider compile boundary.
