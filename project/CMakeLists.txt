# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

cmake_minimum_required(VERSION 3.25)

project(ScratchBird
  VERSION 0.1.0
  LANGUAGES C CXX
)

enable_testing()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PublicBranding.cmake")

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(
  NAME public_header_gate
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/../tools/release/public_header_gate.py"
          "--root=${CMAKE_CURRENT_SOURCE_DIR}/.."
)
set_tests_properties(public_header_gate PROPERTIES
  LABELS "public_release;license_header;MPL-2.0"
)
add_test(
  NAME public_private_material_reference_gate
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/../tools/release/public_private_material_reference_gate.py"
)
set_tests_properties(public_private_material_reference_gate PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
  LABELS "public_release;private_material_reference"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# DPC_BENCHMARK_CLEAN_FLAGS: project-owned compile-time controls for
# benchmark-clean and diagnostic instrumentation builds.
set(_SCRATCHBIRD_ENABLE_DEBUG_LOGS_DEFAULT OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(_SCRATCHBIRD_ENABLE_DEBUG_LOGS_DEFAULT ON)
endif()
set(SCRATCHBIRD_ENABLE_DEBUG_LOGS "${_SCRATCHBIRD_ENABLE_DEBUG_LOGS_DEFAULT}" CACHE BOOL
  "Compile ScratchBird DEBUG_LOG instrumentation into runtime targets")
set(SCRATCHBIRD_ENABLE_HOTPATH_TRACE OFF CACHE BOOL
  "Compile ScratchBird hot-path executor/storage trace instrumentation into runtime targets")
set(SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE OFF CACHE BOOL
  "Compile ScratchBird executor profile trace instrumentation into runtime targets")
set(SCRATCHBIRD_ENABLE_PREPARED_TRACE OFF CACHE BOOL
  "Compile ScratchBird prepared-statement trace instrumentation into runtime targets")
unset(_SCRATCHBIRD_ENABLE_DEBUG_LOGS_DEFAULT)

foreach(_scratchbird_instrumentation_flag IN ITEMS
    SCRATCHBIRD_ENABLE_DEBUG_LOGS
    SCRATCHBIRD_ENABLE_HOTPATH_TRACE
    SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE
    SCRATCHBIRD_ENABLE_PREPARED_TRACE)
  if(${_scratchbird_instrumentation_flag})
    add_compile_definitions(${_scratchbird_instrumentation_flag}=1)
  endif()
endforeach()
unset(_scratchbird_instrumentation_flag)

add_test(
  NAME benchmark_clean_cmake_instrumentation_flags_gate
  COMMAND "${CMAKE_COMMAND}"
          "-DSB_CACHE_FILE=${CMAKE_BINARY_DIR}/CMakeCache.txt"
          "-DSB_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
          "-DSCRATCHBIRD_ENABLE_DEBUG_LOGS=${SCRATCHBIRD_ENABLE_DEBUG_LOGS}"
          "-DSCRATCHBIRD_ENABLE_HOTPATH_TRACE=${SCRATCHBIRD_ENABLE_HOTPATH_TRACE}"
          "-DSCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=${SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE}"
          "-DSCRATCHBIRD_ENABLE_PREPARED_TRACE=${SCRATCHBIRD_ENABLE_PREPARED_TRACE}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BenchmarkCleanInstrumentationFlagsGate.cmake"
)
set_tests_properties(benchmark_clean_cmake_instrumentation_flags_gate PROPERTIES
  LABELS "benchmark_clean_cmake_instrumentation_flags_gate;DPC-001;benchmark_clean;cmake_config"
)

# Implementation root scaffold only. Runtime targets are intentionally added by
# bounded implementation packets after their controlling contracts are ready.

set(SB_NONCLUSTER_ENGINE_PROFILE "release-complete" CACHE STRING
  "Non-cluster engine build profile: release-complete, bootstrap, or emergency")
set_property(CACHE SB_NONCLUSTER_ENGINE_PROFILE PROPERTY STRINGS
  release-complete bootstrap emergency)
set(SB_NONCLUSTER_ENGINE_PROFILE_VALID_VALUES release-complete bootstrap emergency)
if(NOT SB_NONCLUSTER_ENGINE_PROFILE IN_LIST SB_NONCLUSTER_ENGINE_PROFILE_VALID_VALUES)
  message(FATAL_ERROR
    "SB_NONCLUSTER_ENGINE_PROFILE must be one of: release-complete, bootstrap, emergency")
endif()
if(SB_NONCLUSTER_ENGINE_PROFILE STREQUAL "release-complete")
  set(SB_NONCLUSTER_ENGINE_RELEASE_COMPLETE ON CACHE BOOL
    "Release-complete non-cluster engine profile is active" FORCE)
  set(SB_NONCLUSTER_ENGINE_DEGRADED_PROFILE OFF CACHE BOOL
    "Bootstrap/emergency degraded non-cluster engine profile is active" FORCE)
elseif(SB_NONCLUSTER_ENGINE_PROFILE STREQUAL "bootstrap")
  set(SB_NONCLUSTER_ENGINE_RELEASE_COMPLETE OFF CACHE BOOL
    "Release-complete non-cluster engine profile is active" FORCE)
  set(SB_NONCLUSTER_ENGINE_DEGRADED_PROFILE ON CACHE BOOL
    "Bootstrap/emergency degraded non-cluster engine profile is active" FORCE)
elseif(SB_NONCLUSTER_ENGINE_PROFILE STREQUAL "emergency")
  set(SB_NONCLUSTER_ENGINE_RELEASE_COMPLETE OFF CACHE BOOL
    "Release-complete non-cluster engine profile is active" FORCE)
  set(SB_NONCLUSTER_ENGINE_DEGRADED_PROFILE ON CACHE BOOL
    "Bootstrap/emergency degraded non-cluster engine profile is active" FORCE)
endif()
set(SCRATCHBIRD_NONCLUSTER_ENGINE_RELEASE_COMPLETE
  ${SB_NONCLUSTER_ENGINE_RELEASE_COMPLETE})
set(SCRATCHBIRD_NONCLUSTER_ENGINE_DEGRADED_PROFILE
  ${SB_NONCLUSTER_ENGINE_DEGRADED_PROFILE})

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/platform/noncluster_engine_profile.hpp.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/scratchbird/core/platform/noncluster_engine_profile.hpp
  @ONLY
)

option(SB_BUILD_REGISTRY_LINTER "Build the parser/command registry linter vertical slice" OFF)
option(SB_BUILD_PARSER_GENERATOR "Build the parser generator skeleton vertical slice" OFF)
option(SB_BUILD_FIXTURE_LINTER "Build the parser golden fixture linter vertical slice" OFF)
option(SB_BUILD_PARSER_AST_MODEL "Build the parser AST model vertical slice" OFF)
option(SB_BUILD_NATIVE_V3_MINIMAL_PARSER "Build the native V3 minimal parser vertical slice" OFF)
option(SB_BUILD_MINIMAL_BINDER "Build the minimal bound-AST binder vertical slice" OFF)
option(SB_BUILD_MINIMAL_LOWERER "Build the minimal SBLR lowerer vertical slice" OFF)
option(SB_BUILD_PARSE_PROBE "Build the parser vertical-slice parse-bind-lower probe" OFF)
option(SB_BUILD_PACKAGE_GATE "Build the public/private package boundary gate" OFF)
option(SB_BUILD_PARSER_REGISTRY_SNAPSHOT "Build the parser shared registry snapshot library" OFF)
option(SB_BUILD_REGISTRY_SNAPSHOT_PROBE "Build the parser registry snapshot probe" OFF)
option(SB_BUILD_COMMAND_SURFACE_PROBE "Build the command surface lookup probe" OFF)
option(SB_BUILD_DIAGNOSTIC_CATALOG_PROBE "Build the diagnostic catalog lookup probe" OFF)
option(SB_BUILD_CORE_DIAGNOSTIC_PROBE "Build the core diagnostic record validation probe" OFF)
option(SB_BUILD_RESULT_SHAPE_PROBE "Build the result shape catalog lookup probe" OFF)
option(SB_BUILD_AUTHORITY_FAMILY_PROBE "Build the authority family catalog lookup probe" OFF)
option(SB_BUILD_SBLR_OPERATION_PROBE "Build the SBLR operation catalog lookup probe" OFF)
option(SB_BUILD_FIXTURE_MANIFEST_PROBE "Build the fixture manifest inventory probe" OFF)
option(SB_BUILD_TRACE_CONTRACT_PROBE "Build the trace contract evidence probe" OFF)
option(SB_BUILD_CONFORMANCE_PLAN_PROBE "Build the parser conformance plan probe" OFF)
option(SB_BUILD_PUBLIC_SPEC_ZERO_GREY_GATE "Build the public spec implementation zero-grey release gate" OFF)
option(SB_BUILD_DATABASE_PROBE "Build the database create/open vertical-slice probe" OFF)
option(SB_BUILD_MEMORY_PROBE "Build the local engine memory manager validation probe" OFF)
option(SB_BUILD_TRANSACTION_PROBE "Build the local transaction system validation probe" OFF)
option(SB_BUILD_SINGLE_NODE_TRANSACTION_PROBE "Build the single-node transaction completion validation probe" OFF)
option(SB_BUILD_TRANSACTION_RECOVERY_PROBE "Build the transaction recovery validation probe" OFF)
option(SB_BUILD_DIRTY_MANIFEST_RECOVERY_PROBE "Build the dirty-object manifest recovery validation probe" OFF)
option(SB_BUILD_AST_CONTRACT_PROBE "Build the AST to SBLR contract probe" OFF)
option(SB_BUILD_SBLR_ENVELOPE_PROBE "Build the engine SBLR envelope validation probe" OFF)
option(SB_BUILD_SBLR_DISPATCH_PROBE "Build the engine SBLR dispatch probe" OFF)
option(SB_BUILD_SBLR_API_EQUIVALENCE_PROBE "Build the SBLR to Engine API equivalence probe" OFF)
option(SB_BUILD_LOGICAL_PLAN_PROBE "Build the deterministic logical plan probe" OFF)
option(SB_BUILD_PLANNER_EXECUTION_PROBE "Build the planner to SBLR execution probe" OFF)
option(SB_BUILD_OPTIMIZER_CONTRACT_PROBE "Build the optimizer contract probe" OFF)
option(SB_BUILD_OPTIMIZER_COST_PROBE "Build the optimizer cost model probe" OFF)
option(SB_BUILD_OPTIMIZER_EXECUTION_PLAN_PROBES "Build the optimizer legacy-reference/local-costing execution_plan probes" OFF)
option(SB_BUILD_CAPABILITY_PROBE "Build the runtime capability report probe" OFF)
option(SB_BUILD_UUID_IDENTITY_PROBE "Build the UUID compatibility and engine identity probe" OFF)
option(SB_BUILD_TIME_AUTHORITY_PROBE "Build the single-node time authority probe" OFF)
option(SB_BUILD_SBSQL_V3_NOSQL_DOCUMENT_KV_PROBE "Build the SBSQL V3 document/key-value native surface probe" OFF)
option(SB_BUILD_SBSQL_V3_SEARCH_VECTOR_PROBE "Build the SBSQL V3 search/vector native surface probe" OFF)
option(SB_BUILD_SBSQL_V3_GRAPH_TIMESERIES_PROBE "Build the SBSQL V3 graph/time-series native surface probe" OFF)
option(SB_BUILD_SBSQL_V3_LLVM_GPU_PROBE "Build the SBSQL V3 LLVM/GPU extension command probe" OFF)
option(SB_BUILD_SBSQL_V3_UDR_PARSER_PACKAGE_PROBE "Build the SBSQL V3 UDR/parser package extension command probe" OFF)
option(SB_BUILD_UDR_LIFECYCLE_PROBE "Build the UDR lifecycle authority probe" OFF)
option(SB_BUILD_LLVM_RUNTIME_PROBE "Build the LLVM JIT/AOT runtime probe" OFF)
option(SB_BUILD_GPU_PROFILE_PROBE "Build the optional GPU profile probe" OFF)
option(SB_BUILD_ENGINE_LIFECYCLE_PROBE "Build the engine lifecycle state-machine probe" OFF)
option(SB_BUILD_SBSQL_V3_CLUSTER_FAIL_CLOSED_PROBE "Build the SBSQL V3 cluster placeholder fail-closed probe" OFF)
option(SB_ENABLE_CLUSTER_PROVIDER "Build the engine with a compile-time cluster provider boundary" OFF)
option(SB_CLUSTER_PROVIDER_STUB "Link the public cluster stub provider instead of the no-cluster provider" OFF)
set(SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY "" CACHE FILEPATH
  "Path to a closed-source cluster provider library that implements the sb_cluster_provider target ABI")
set(SB_CLUSTER_PROVIDER_EXTERNAL_INCLUDE_DIR "" CACHE PATH
  "Optional include directory for a closed-source cluster provider")
option(SB_AGENT_ENFORCE_PRODUCTION_BUILD_GATE
  "Fail release-complete production builds when agent fixture/debug/simulated paths are enabled"
  ON)
option(SB_AGENT_ALLOW_FIXTURE_AUTH_IN_PRODUCTION
  "Test-only: allow fixture authentication on agent production-live paths"
  OFF)
option(SB_AGENT_ALLOW_FIXTURE_POLICY_IN_PRODUCTION
  "Test-only: allow fixture policies on agent production-live paths"
  OFF)
option(SB_AGENT_ALLOW_RELAXED_METRICS_IN_PRODUCTION
  "Test-only: allow relaxed/registry-only agent metric evidence in production"
  OFF)
option(SB_AGENT_ALLOW_TEST_SEEDS_IN_PRODUCTION
  "Test-only: allow deterministic test seeds in agent production paths"
  OFF)
option(SB_AGENT_ALLOW_FORCED_COLLISION_HOOKS_IN_PRODUCTION
  "Test-only: allow forced collision/test hooks in agent production paths"
  OFF)
option(SB_AGENT_ALLOW_SIDECAR_STATE_IN_PRODUCTION
  "Test-only: allow sidecar-only agent state/evidence in production"
  OFF)
option(SB_AGENT_ALLOW_SIMULATED_ACTUATORS_IN_PRODUCTION
  "Test-only: allow simulated/default-only agent actuator execution in production"
  OFF)
option(SB_AGENT_ALLOW_DEBUG_ONLY_PATHS_IN_PRODUCTION
  "Test-only: allow debug-only agent paths in production"
  OFF)
option(SB_AGENT_CLUSTER_STUB_LIVE_CLAIMS
  "Test-only: allow cluster stub provider paths to claim live production cluster authority"
  OFF)
option(SB_OPTIMIZER_ENFORCE_PRODUCTION_BUILD_GATE
  "Fail release-complete production builds when optimizer fixture/default/placeholder/debug paths are enabled"
  ON)
option(SB_OPTIMIZER_ALLOW_FIXTURE_STATS_IN_PRODUCTION
  "Test-only: allow fixture optimizer statistics in production profiles"
  OFF)
option(SB_OPTIMIZER_ALLOW_LOCAL_DEFAULT_STATS_IN_PRODUCTION
  "Test-only: allow local default optimizer statistics in production profiles"
  OFF)
option(SB_OPTIMIZER_ALLOW_POLICY_DEFAULT_STATS_IN_PRODUCTION
  "Test-only: allow policy default optimizer statistics in production profiles"
  OFF)
option(SB_OPTIMIZER_ALLOW_RELAXED_METRICS_IN_PRODUCTION
  "Test-only: allow relaxed optimizer metric evidence in production profiles"
  OFF)
option(SB_OPTIMIZER_ALLOW_PLACEHOLDER_RUNTIME_EVIDENCE_IN_PRODUCTION
  "Test-only: allow placeholder runtime evidence in production optimizer routes"
  OFF)
option(SB_OPTIMIZER_ALLOW_COMPAT_AUTHORITY_IN_PRODUCTION
  "Test-only: allow compatibility optimizer evidence authority in production profiles"
  OFF)
option(SB_OPTIMIZER_ALLOW_PARSER_SHORTCUTS_IN_PRODUCTION
  "Test-only: allow parser execution shortcuts in production optimizer routes"
  OFF)
option(SB_OPTIMIZER_ALLOW_CLUSTER_STUB_LIVE_CLAIMS
  "Test-only: allow cluster stub provider paths to claim live optimizer authority"
  OFF)
option(SB_OPTIMIZER_ALLOW_DEBUG_ONLY_PATHS_IN_PRODUCTION
  "Test-only: allow debug-only optimizer paths in production profiles"
  OFF)
option(SB_OPTIMIZER_ALLOW_SYNTHETIC_FEEDBACK_IN_PRODUCTION
  "Test-only: allow synthetic optimizer feedback in production profiles"
  OFF)
option(SB_COMMERCIAL_ENFORCE_PRODUCTION_BUILD_GATE
  "Fail release-complete builds when generic commercial-readiness fixture/debug/stub paths are enabled"
  ON)
option(SB_COMMERCIAL_ALLOW_FIXTURE_AUTH_IN_PRODUCTION
  "Test-only: allow fixture authentication in commercial production profiles"
  OFF)
option(SB_COMMERCIAL_ALLOW_TEST_SEEDS_IN_PRODUCTION
  "Test-only: allow deterministic test seeds in commercial production profiles"
  OFF)
option(SB_COMMERCIAL_ALLOW_FORCED_COLLISION_HOOKS_IN_PRODUCTION
  "Test-only: allow forced collision hooks in commercial production profiles"
  OFF)
option(SB_COMMERCIAL_ALLOW_STUB_PROVIDERS_IN_PRODUCTION
  "Test-only: allow stub providers in commercial production profiles"
  OFF)
option(SB_COMMERCIAL_ALLOW_DEBUG_CREDENTIALS_IN_PRODUCTION
  "Test-only: allow debug-only credentials in commercial production profiles"
  OFF)
option(SB_COMMERCIAL_ALLOW_DEBUG_ONLY_AUTHORITY_PATHS_IN_PRODUCTION
  "Test-only: allow debug-only authority paths in commercial production profiles"
  OFF)
option(SB_COMMERCIAL_ALLOW_NO_CLUSTER_PRODUCTION_CLAIMS
  "Test-only: allow no_cluster to satisfy production cluster claims"
  OFF)
option(SB_COMMERCIAL_CLUSTER_PRODUCTION_CLAIMS
  "Enable production cluster readiness claims; requires external non-stub cluster provider"
  OFF)
option(SB_BUILD_SBSQL_V3_DIAGNOSTIC_RENDERING_PROBE "Build the SBSQL V3 diagnostic rendering/result-shape probe" OFF)
option(SB_BUILD_NATIVE_V3_PARSER_ISOLATION_PROBE "Build the native V3 parser package isolation/integration probe" OFF)
option(SB_BUILD_SBSQL_V3_CONFORMANCE_GATE "Build the SBSQL V3 full conformance execution_plan gate" OFF)
option(SB_BUILD_SBSQL_V3_FULL_GATE "Build the SBSQL V3 final execution_plan closure gate" OFF)
option(SB_BUILD_DATATYPE_CAST_MATRIX_PROBE "Build the datatype cast matrix probe" OFF)
option(SB_BUILD_DATATYPE_EXTRACT_PROBE "Build the datatype extract operation probe" OFF)
option(SB_BUILD_DATATYPE_SET_PROBE "Build the datatype set/collection operation probe" OFF)
option(SB_BUILD_DOMAIN_LIFECYCLE_PROBE "Build the domain lifecycle probe" OFF)
option(SB_BUILD_DOMAIN_ENFORCEMENT_PROBE "Build the domain enforcement probe" OFF)
option(SB_BUILD_SBLR_DATATYPE_DOMAIN_PROBE "Build the SBLR datatype/domain dispatch probe" OFF)
option(SB_BUILD_SBLR_DATATYPE_FULL_SURFACE_PROBE "Build the full SBLR datatype/domain surface probe" OFF)
option(SB_BUILD_AGENT_RUNTIME_PROBES "Build the operational agent runtime implementation probes" OFF)
option(SB_BUILD_TESTS "Build ScratchBird private tests" OFF)
option(SB_BUILD_PUBLIC_RELEASE_CORRECTNESS
  "Build public release correctness CTest gates that run from the public project tree"
  OFF)
option(SB_BUILD_PUBLIC_P1_CLOUD_OPS_TESTS
  "Build public P1 protected-material/cloud-ops CTest gates"
  OFF)
option(SB_INSTALL_NON_ENGINE_COMPONENTS
  "Install non-engine listener parser manager driver UDR example and auxiliary artifacts"
  OFF)
set(SB_PUBLIC_RELEASE_SANITIZER_PROFILE "none" CACHE STRING
  "Public release sanitizer profile: none, asan-ubsan, or tsan")
set_property(CACHE SB_PUBLIC_RELEASE_SANITIZER_PROFILE PROPERTY STRINGS
  none asan-ubsan tsan)
option(SB_PUBLIC_RELEASE_WARNINGS_AS_ERRORS
  "Build public release proof targets with compiler warnings promoted to errors"
  OFF)

# PUBLIC_RELEASE_SANITIZER_PROFILES
set(SB_PUBLIC_RELEASE_SANITIZER_PROFILE_VALID_VALUES none asan-ubsan tsan)
if(NOT SB_PUBLIC_RELEASE_SANITIZER_PROFILE IN_LIST SB_PUBLIC_RELEASE_SANITIZER_PROFILE_VALID_VALUES)
  message(FATAL_ERROR
    "SB_PUBLIC_RELEASE_SANITIZER_PROFILE must be one of: none, asan-ubsan, tsan")
endif()
add_compile_definitions(
  SB_PUBLIC_RELEASE_SANITIZER_PROFILE_TEXT="${SB_PUBLIC_RELEASE_SANITIZER_PROFILE}"
)
if(SB_PUBLIC_RELEASE_WARNINGS_AS_ERRORS)
  add_compile_definitions(SB_PUBLIC_RELEASE_WARNINGS_AS_ERRORS_ACTIVE=1)
  if(MSVC)
    add_compile_options(/W4 /WX)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
  else()
    message(FATAL_ERROR
      "SB_DIAG_WARNINGS_AS_ERRORS_UNSUPPORTED: compiler family does not have a public release warnings-as-errors profile")
  endif()
endif()
if(NOT SB_PUBLIC_RELEASE_SANITIZER_PROFILE STREQUAL "none")
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR
      "SB_DIAG_SANITIZER_PROFILE_UNSUPPORTED: public release sanitizer profiles require GCC or Clang")
  endif()
  add_compile_options(-fno-omit-frame-pointer)
  if(SB_PUBLIC_RELEASE_SANITIZER_PROFILE STREQUAL "asan-ubsan")
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
    add_compile_definitions(SB_PUBLIC_RELEASE_ASAN_UBSAN_PROFILE=1)
  elseif(SB_PUBLIC_RELEASE_SANITIZER_PROFILE STREQUAL "tsan")
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
    add_compile_definitions(SB_PUBLIC_RELEASE_TSAN_PROFILE=1)
  endif()
endif()

if(SB_CLUSTER_PROVIDER_STUB AND NOT SB_ENABLE_CLUSTER_PROVIDER)
  message(FATAL_ERROR "SB_CLUSTER_PROVIDER_STUB requires SB_ENABLE_CLUSTER_PROVIDER=ON")
endif()
if(SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY AND NOT SB_ENABLE_CLUSTER_PROVIDER)
  message(FATAL_ERROR "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY requires SB_ENABLE_CLUSTER_PROVIDER=ON")
endif()
if(SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY AND SB_CLUSTER_PROVIDER_STUB)
  message(FATAL_ERROR "Choose either SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY or SB_CLUSTER_PROVIDER_STUB, not both")
endif()

set(SB_AGENT_PRODUCTION_BUILD OFF)
if(SB_NONCLUSTER_ENGINE_RELEASE_COMPLETE AND NOT SB_AGENT_ENFORCE_PRODUCTION_BUILD_GATE)
  message(FATAL_ERROR
    "SB_AGENT_ENFORCE_PRODUCTION_BUILD_GATE cannot be disabled for release-complete builds")
endif()
if(SB_AGENT_ENFORCE_PRODUCTION_BUILD_GATE
   AND SB_NONCLUSTER_ENGINE_RELEASE_COMPLETE)
  set(SB_AGENT_PRODUCTION_BUILD ON)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/AgentProductionBuildGate.cmake")
if(SB_AGENT_PRODUCTION_BUILD)
  add_compile_definitions(SCRATCHBIRD_AGENT_PRODUCTION_BUILD=1)
endif()
# PUBLIC_PRODUCTION_FEATURE_AUDIT
add_test(
  NAME agent_production_build_cmake_gate
  COMMAND "${CMAKE_COMMAND}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AgentProductionBuildGateMatrix.cmake"
)
set_tests_properties(agent_production_build_cmake_gate PROPERTIES
  LABELS "agent_production_build_cmake_gate;AEIC-017;agents;production_fixture_separation;cmake_config"
)
add_test(
  NAME agent_production_build_configure_gate
  COMMAND "${CMAKE_COMMAND}"
          "-DSB_PROJECT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
          "-DSB_CONFIGURE_GATE_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/agent_production_configure_gate"
          "-DSB_CONFIGURE_GATE_GENERATOR=${CMAKE_GENERATOR}"
          "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
          "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
          "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
          "-DSB_LLVM_PROJECT_ROOT=${SB_LLVM_PROJECT_ROOT}"
          "-DSB_LLVM_TOOLS_ROOT=${SB_LLVM_TOOLS_ROOT}"
          "-DSB_LLVM_LIBRARY=${SB_LLVM_LIBRARY}"
          "-DSB_LLVM_LINK_MODE=${SB_LLVM_LINK_MODE}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AgentProductionConfigureGateMatrix.cmake"
)
set_tests_properties(agent_production_build_configure_gate PROPERTIES
  LABELS "agent_production_build_configure_gate;AEIC-017;agents;production_fixture_separation;cmake_config"
)

set(SB_OPTIMIZER_PRODUCTION_BUILD OFF)
if(SB_NONCLUSTER_ENGINE_RELEASE_COMPLETE AND NOT SB_OPTIMIZER_ENFORCE_PRODUCTION_BUILD_GATE)
  message(FATAL_ERROR
    "SB_OPTIMIZER_ENFORCE_PRODUCTION_BUILD_GATE cannot be disabled for release-complete builds")
endif()
if(SB_OPTIMIZER_ENFORCE_PRODUCTION_BUILD_GATE
   AND SB_NONCLUSTER_ENGINE_RELEASE_COMPLETE)
  set(SB_OPTIMIZER_PRODUCTION_BUILD ON)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/OptimizerProductionBuildGate.cmake")
if(SB_OPTIMIZER_PRODUCTION_BUILD)
  add_compile_definitions(SCRATCHBIRD_OPTIMIZER_PRODUCTION_BUILD=1)
endif()
add_test(
  NAME optimizer_production_build_cmake_gate
  COMMAND "${CMAKE_COMMAND}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/OptimizerProductionBuildGateMatrix.cmake"
)
set_tests_properties(optimizer_production_build_cmake_gate PROPERTIES
  LABELS "optimizer_production_build_cmake_gate;OEIC-080;optimizer;production_build_separation;cmake_config"
)
add_test(
  NAME optimizer_production_build_configure_gate
  COMMAND "${CMAKE_COMMAND}"
          "-DSB_PROJECT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
          "-DSB_CONFIGURE_GATE_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/optimizer_production_configure_gate"
          "-DSB_CONFIGURE_GATE_GENERATOR=${CMAKE_GENERATOR}"
          "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
          "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
          "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
          "-DSB_LLVM_PROJECT_ROOT=${SB_LLVM_PROJECT_ROOT}"
          "-DSB_LLVM_TOOLS_ROOT=${SB_LLVM_TOOLS_ROOT}"
          "-DSB_LLVM_LIBRARY=${SB_LLVM_LIBRARY}"
          "-DSB_LLVM_LINK_MODE=${SB_LLVM_LINK_MODE}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/OptimizerProductionConfigureGateMatrix.cmake"
)
set_tests_properties(optimizer_production_build_configure_gate PROPERTIES
  LABELS "optimizer_production_build_configure_gate;OEIC-080;optimizer;production_build_separation;cmake_config"
)

set(SB_COMMERCIAL_READINESS_PRODUCTION_BUILD OFF)
if(SB_NONCLUSTER_ENGINE_RELEASE_COMPLETE AND NOT SB_COMMERCIAL_ENFORCE_PRODUCTION_BUILD_GATE)
  message(FATAL_ERROR
    "SB_COMMERCIAL_ENFORCE_PRODUCTION_BUILD_GATE cannot be disabled for release-complete builds")
endif()
if(SB_COMMERCIAL_ENFORCE_PRODUCTION_BUILD_GATE
   AND SB_NONCLUSTER_ENGINE_RELEASE_COMPLETE)
  set(SB_COMMERCIAL_READINESS_PRODUCTION_BUILD ON)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CommercialReadinessProductionBuildGate.cmake")
if(SB_COMMERCIAL_READINESS_PRODUCTION_BUILD)
  add_compile_definitions(SCRATCHBIRD_COMMERCIAL_READINESS_PRODUCTION_BUILD=1)
endif()
add_test(
  NAME commercial_readiness_production_build_cmake_gate
  COMMAND "${CMAKE_COMMAND}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CommercialReadinessProductionBuildGateMatrix.cmake"
)
set_tests_properties(commercial_readiness_production_build_cmake_gate PROPERTIES
  LABELS "commercial_readiness_production_build_cmake_gate;CRP-GATE-010;commercial_readiness;production_build;cmake_config"
)

if(SB_BUILD_REGISTRY_LINTER)
  add_subdirectory(tools/sb_registry_lint)
endif()

if(SB_BUILD_PARSER_GENERATOR)
  add_subdirectory(tools/sb_parser_gen)
endif()

if(SB_BUILD_FIXTURE_LINTER)
  add_subdirectory(tools/sb_fixture_lint)
endif()

if(SB_BUILD_PARSER_AST_MODEL OR SB_BUILD_NATIVE_V3_MINIMAL_PARSER OR SB_BUILD_MINIMAL_BINDER OR SB_BUILD_MINIMAL_LOWERER OR SB_BUILD_PARSE_PROBE)
  add_subdirectory(src/parsers/shared/ast)
endif()

if(SB_BUILD_NATIVE_V3_MINIMAL_PARSER OR SB_BUILD_PARSE_PROBE)
  add_subdirectory(src/parsers/native/v3/minimal)
endif()

if(SB_BUILD_MINIMAL_BINDER OR SB_BUILD_MINIMAL_LOWERER OR SB_BUILD_PARSE_PROBE)
  add_subdirectory(src/parsers/shared/bound_ast)
endif()

if((SB_BUILD_MINIMAL_LOWERER OR SB_BUILD_PARSE_PROBE) AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()

if((SB_BUILD_MINIMAL_LOWERER OR SB_BUILD_PARSE_PROBE) AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()

if((SB_BUILD_MINIMAL_LOWERER OR SB_BUILD_PARSE_PROBE) AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()

if((SB_BUILD_MINIMAL_LOWERER OR SB_BUILD_PARSE_PROBE) AND NOT TARGET sbl_numeric)
  add_subdirectory(libraries/sbl_numeric)
endif()

if((SB_BUILD_MINIMAL_LOWERER OR SB_BUILD_PARSE_PROBE) AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()

if((SB_BUILD_MINIMAL_LOWERER OR SB_BUILD_PARSE_PROBE) AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()

if((SB_BUILD_MINIMAL_LOWERER OR SB_BUILD_PARSE_PROBE) AND NOT TARGET sb_storage_page)
  add_subdirectory(src/storage/page)
endif()

if((SB_BUILD_MINIMAL_LOWERER OR SB_BUILD_PARSE_PROBE) AND NOT TARGET sb_core_catalog)
  add_subdirectory(src/core/catalog)
endif()

if((SB_BUILD_MINIMAL_LOWERER OR SB_BUILD_PARSE_PROBE) AND NOT TARGET sb_storage_database)
  add_subdirectory(src/storage/database)
endif()

if((SB_BUILD_MINIMAL_LOWERER OR SB_BUILD_PARSE_PROBE) AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()

if(SB_BUILD_MINIMAL_LOWERER OR SB_BUILD_PARSE_PROBE)
  add_subdirectory(src/parsers/shared/lowering)
endif()

if(SB_BUILD_PARSE_PROBE)
  add_subdirectory(tools/sb_parse_probe)
endif()

if(SB_BUILD_PACKAGE_GATE AND NOT TARGET sb_package_gate)
  add_subdirectory(tools/sb_package_gate)
endif()

if(SB_BUILD_PARSER_REGISTRY_SNAPSHOT AND NOT TARGET sb_parser_registry_snapshot)
  add_subdirectory(src/parsers/shared/registry)
endif()

if(SB_BUILD_REGISTRY_SNAPSHOT_PROBE AND NOT TARGET sb_parser_registry_snapshot)
  add_subdirectory(src/parsers/shared/registry)
endif()

if(SB_BUILD_REGISTRY_SNAPSHOT_PROBE)
  add_subdirectory(tools/sb_registry_snapshot_probe)
endif()

if(SB_BUILD_COMMAND_SURFACE_PROBE AND NOT TARGET sb_parser_registry_snapshot)
  add_subdirectory(src/parsers/shared/registry)
endif()

if(SB_BUILD_COMMAND_SURFACE_PROBE)
  add_subdirectory(tools/sb_command_surface_probe)
endif()

if(SB_BUILD_DIAGNOSTIC_CATALOG_PROBE)
  add_subdirectory(tools/sb_diagnostic_catalog_probe)
endif()

if(SB_BUILD_CORE_DIAGNOSTIC_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_CORE_DIAGNOSTIC_PROBE)
  add_subdirectory(tools/sb_core_diagnostic_probe)
endif()

if(SB_BUILD_RESULT_SHAPE_PROBE)
  add_subdirectory(tools/sb_result_shape_probe)
endif()

if(SB_BUILD_AUTHORITY_FAMILY_PROBE)
  add_subdirectory(tools/sb_authority_family_probe)
endif()

if(SB_BUILD_SBLR_OPERATION_PROBE)
  add_subdirectory(tools/sb_sblr_operation_probe)
endif()

if(SB_BUILD_FIXTURE_MANIFEST_PROBE)
  add_subdirectory(tools/sb_fixture_manifest_probe)
endif()

if(SB_BUILD_TRACE_CONTRACT_PROBE)
  add_subdirectory(tools/sb_trace_contract_probe)
endif()

if(SB_BUILD_CONFORMANCE_PLAN_PROBE)
  add_subdirectory(tools/sb_conformance_plan_probe)
endif()

if(SB_BUILD_PUBLIC_SPEC_ZERO_GREY_GATE)
  add_subdirectory(tools/sb_public_spec_zero_grey_gate)
endif()

if(SB_BUILD_MEMORY_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_MEMORY_PROBE AND NOT TARGET sb_core_memory)
  add_subdirectory(src/core/memory)
endif()
if(SB_BUILD_MEMORY_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_MEMORY_PROBE AND NOT TARGET sb_transaction_mga)
  add_subdirectory(src/transaction/mga)
endif()
if(SB_BUILD_MEMORY_PROBE)
  add_subdirectory(tools/sb_memory_probe)
endif()

if(SB_BUILD_TRANSACTION_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_TRANSACTION_PROBE AND NOT TARGET sb_core_memory)
  add_subdirectory(src/core/memory)
endif()
if(SB_BUILD_TRANSACTION_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_TRANSACTION_PROBE AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_TRANSACTION_PROBE AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if(SB_BUILD_TRANSACTION_PROBE AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_TRANSACTION_PROBE AND NOT TARGET sb_transaction_mga)
  add_subdirectory(src/transaction/mga)
endif()
if(SB_BUILD_TRANSACTION_PROBE AND NOT TARGET sb_storage_page)
  add_subdirectory(src/storage/page)
endif()
if(SB_BUILD_TRANSACTION_PROBE AND NOT TARGET sb_core_catalog)
  add_subdirectory(src/core/catalog)
endif()
if(SB_BUILD_TRANSACTION_PROBE AND NOT TARGET sb_storage_database)
  add_subdirectory(src/storage/database)
endif()
if(SB_BUILD_TRANSACTION_PROBE)
  add_subdirectory(tools/sb_transaction_probe)
endif()

if((SB_BUILD_SINGLE_NODE_TRANSACTION_PROBE OR SB_BUILD_TRANSACTION_RECOVERY_PROBE OR SB_BUILD_DIRTY_MANIFEST_RECOVERY_PROBE) AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if((SB_BUILD_SINGLE_NODE_TRANSACTION_PROBE OR SB_BUILD_DIRTY_MANIFEST_RECOVERY_PROBE) AND NOT TARGET sb_core_memory)
  add_subdirectory(src/core/memory)
endif()
if((SB_BUILD_SINGLE_NODE_TRANSACTION_PROBE OR SB_BUILD_TRANSACTION_RECOVERY_PROBE OR SB_BUILD_DIRTY_MANIFEST_RECOVERY_PROBE) AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if((SB_BUILD_SINGLE_NODE_TRANSACTION_PROBE OR SB_BUILD_DIRTY_MANIFEST_RECOVERY_PROBE) AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if((SB_BUILD_SINGLE_NODE_TRANSACTION_PROBE OR SB_BUILD_DIRTY_MANIFEST_RECOVERY_PROBE) AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if((SB_BUILD_SINGLE_NODE_TRANSACTION_PROBE OR SB_BUILD_DIRTY_MANIFEST_RECOVERY_PROBE) AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if((SB_BUILD_SINGLE_NODE_TRANSACTION_PROBE OR SB_BUILD_TRANSACTION_RECOVERY_PROBE) AND NOT TARGET sb_transaction_mga)
  add_subdirectory(src/transaction/mga)
endif()
if((SB_BUILD_SINGLE_NODE_TRANSACTION_PROBE OR SB_BUILD_DIRTY_MANIFEST_RECOVERY_PROBE) AND NOT TARGET sb_storage_page)
  add_subdirectory(src/storage/page)
endif()
if((SB_BUILD_SINGLE_NODE_TRANSACTION_PROBE OR SB_BUILD_DIRTY_MANIFEST_RECOVERY_PROBE) AND NOT TARGET sb_core_catalog)
  add_subdirectory(src/core/catalog)
endif()
if((SB_BUILD_SINGLE_NODE_TRANSACTION_PROBE OR SB_BUILD_DIRTY_MANIFEST_RECOVERY_PROBE) AND NOT TARGET sb_storage_database)
  add_subdirectory(src/storage/database)
endif()
if(SB_BUILD_SINGLE_NODE_TRANSACTION_PROBE)
  add_subdirectory(tools/sb_single_node_transaction_probe)
endif()
if(SB_BUILD_TRANSACTION_RECOVERY_PROBE)
  add_subdirectory(tools/sb_transaction_recovery_probe)
endif()
if(SB_BUILD_DIRTY_MANIFEST_RECOVERY_PROBE)
  add_subdirectory(tools/sb_dirty_manifest_recovery_probe)
endif()

if(SB_BUILD_PACKAGE_GATE AND NOT TARGET sb_package_gate)
  add_subdirectory(tools/sb_package_gate)
endif()

option(SB_BUILD_CORE_PLATFORM "Build the ScratchBird core platform runtime foundation" OFF)
if(SB_BUILD_CORE_PLATFORM AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()

option(SB_BUILD_CORE_UUID "Build the ScratchBird core UUID runtime foundation" OFF)
if(SB_BUILD_CORE_UUID AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_CORE_UUID AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()

option(SB_BUILD_CORE_TIME "Build the ScratchBird core time runtime foundation" OFF)
if(SB_BUILD_CORE_TIME AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_CORE_TIME AND NOT TARGET sb_core_time)
  add_subdirectory(src/core/time)
endif()

option(SB_BUILD_CORE_MEMORY "Build the ScratchBird core memory runtime foundation" OFF)
if(SB_BUILD_CORE_MEMORY AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_CORE_MEMORY AND NOT TARGET sb_core_memory)
  add_subdirectory(src/core/memory)
endif()

option(SB_BUILD_CORE_DATATYPES "Build the ScratchBird core datatypes runtime foundation" OFF)
if(SB_BUILD_CORE_DATATYPES AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_CORE_DATATYPES AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()

option(SB_BUILD_CORE_RESOURCES "Build the ScratchBird core resource seed-pack runtime foundation" OFF)
if(SB_BUILD_CORE_RESOURCES AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_CORE_RESOURCES AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()

option(SB_BUILD_CORE_CATALOG "Build the ScratchBird core catalog record foundation" OFF)
if(SB_BUILD_CORE_CATALOG AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_CORE_CATALOG AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_CORE_CATALOG AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_CORE_CATALOG AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_CORE_CATALOG AND NOT TARGET sb_storage_page)
  add_subdirectory(src/storage/page)
endif()
if(SB_BUILD_CORE_CATALOG AND NOT TARGET sb_core_catalog)
  add_subdirectory(src/core/catalog)
endif()

option(SB_BUILD_STORAGE_DISK "Build the ScratchBird storage disk runtime foundation" OFF)
option(SB_BUILD_DISK_MANAGER_COMPLETION_PROBE "Build the disk/device manager completion probe" OFF)
if(SB_BUILD_STORAGE_DISK AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_STORAGE_DISK AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_DISK_MANAGER_COMPLETION_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_DISK_MANAGER_COMPLETION_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_DISK_MANAGER_COMPLETION_PROBE AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_DISK_MANAGER_COMPLETION_PROBE)
  add_subdirectory(tools/sb_disk_manager_completion_probe)
endif()

option(SB_BUILD_STORAGE_PAGE "Build the ScratchBird storage page runtime foundation" OFF)
if(SB_BUILD_STORAGE_PAGE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_STORAGE_PAGE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_STORAGE_PAGE AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_STORAGE_PAGE AND NOT TARGET sb_storage_page)
  add_subdirectory(src/storage/page)
endif()

option(SB_BUILD_STORAGE_FILESPACE "Build the ScratchBird storage filespace lifecycle runtime foundation" OFF)
option(SB_BUILD_FILESPACE_LIFECYCLE_PROBE "Build the storage filespace lifecycle completion probe" OFF)
if(SB_BUILD_STORAGE_FILESPACE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_STORAGE_FILESPACE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_STORAGE_FILESPACE AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_STORAGE_FILESPACE AND NOT TARGET sb_storage_filespace)
  add_subdirectory(src/storage/filespace)
endif()
if(SB_BUILD_FILESPACE_LIFECYCLE_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_FILESPACE_LIFECYCLE_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_FILESPACE_LIFECYCLE_PROBE AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_FILESPACE_LIFECYCLE_PROBE AND NOT TARGET sb_storage_filespace)
  add_subdirectory(src/storage/filespace)
endif()
if(SB_BUILD_FILESPACE_LIFECYCLE_PROBE)
  add_subdirectory(tools/sb_filespace_lifecycle_probe)
endif()

option(SB_BUILD_STORAGE_DATABASE "Build the ScratchBird storage database lifecycle vertical slice" OFF)
if(SB_BUILD_STORAGE_DATABASE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_STORAGE_DATABASE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_STORAGE_DATABASE AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_STORAGE_DATABASE AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if(SB_BUILD_STORAGE_DATABASE AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_STORAGE_DATABASE AND NOT TARGET sb_storage_page)
  add_subdirectory(src/storage/page)
endif()
if(SB_BUILD_STORAGE_DATABASE AND NOT TARGET sb_core_catalog)
  add_subdirectory(src/core/catalog)
endif()
if(SB_BUILD_STORAGE_DATABASE AND NOT TARGET sb_storage_database)
  add_subdirectory(src/storage/database)
endif()

option(SB_BUILD_TRANSACTION_MGA "Build the ScratchBird transaction MGA runtime foundation" OFF)
if(SB_BUILD_TRANSACTION_MGA AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_TRANSACTION_MGA AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_TRANSACTION_MGA AND NOT TARGET sb_transaction_mga)
  add_subdirectory(src/transaction/mga)
endif()

option(SB_BUILD_MGA_TRANSACTION_REGRESSION_TESTS
  "Build the MGA ACID/stress/transaction regression CTest suite"
  OFF)
if(SB_BUILD_MGA_TRANSACTION_REGRESSION_TESTS)
  set(SB_BUILD_SERVER ON)
  set(SB_BUILD_SB_LISTENER ON)
  set(SB_BUILD_SBSQL_PARSER_WORKER ON)
endif()

option(SB_BUILD_CATALOG_BOOTSTRAP "Build the ScratchBird catalog bootstrap runtime foundation" OFF)
if(SB_BUILD_CATALOG_BOOTSTRAP AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_CATALOG_BOOTSTRAP AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_CATALOG_BOOTSTRAP AND NOT TARGET sb_catalog_bootstrap)
  add_subdirectory(src/catalog/bootstrap)
endif()

option(SB_BUILD_ENGINE_INTERNAL_API "Build the ScratchBird engine internal API runtime foundation" OFF)
if(SB_BUILD_ENGINE_INTERNAL_API AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_ENGINE_INTERNAL_API AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_ENGINE_INTERNAL_API AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_ENGINE_INTERNAL_API AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if(SB_BUILD_ENGINE_INTERNAL_API AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_ENGINE_INTERNAL_API AND NOT TARGET sb_storage_page)
  add_subdirectory(src/storage/page)
endif()
if(SB_BUILD_ENGINE_INTERNAL_API AND NOT TARGET sb_core_catalog)
  add_subdirectory(src/core/catalog)
endif()
if(SB_BUILD_ENGINE_INTERNAL_API AND NOT TARGET sb_storage_database)
  add_subdirectory(src/storage/database)
endif()
if(SB_BUILD_ENGINE_INTERNAL_API AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()

if(SB_BUILD_DATABASE_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_DATABASE_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_DATABASE_PROBE AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_DATABASE_PROBE AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if(SB_BUILD_DATABASE_PROBE AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_DATABASE_PROBE AND NOT TARGET sb_storage_page)
  add_subdirectory(src/storage/page)
endif()
if(SB_BUILD_DATABASE_PROBE AND NOT TARGET sb_core_catalog)
  add_subdirectory(src/core/catalog)
endif()
if(SB_BUILD_DATABASE_PROBE AND NOT TARGET sb_storage_database)
  add_subdirectory(src/storage/database)
endif()
if(SB_BUILD_DATABASE_PROBE AND NOT TARGET sb_catalog_bootstrap)
  add_subdirectory(src/catalog/bootstrap)
endif()
if(SB_BUILD_DATABASE_PROBE AND NOT TARGET sb_transaction_mga)
  add_subdirectory(src/transaction/mga)
endif()
if(SB_BUILD_DATABASE_PROBE AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if(SB_BUILD_DATABASE_PROBE)
  add_subdirectory(tools/sb_database_probe)
endif()

if(SB_BUILD_CAPABILITY_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_CAPABILITY_PROBE)
  add_subdirectory(tools/sb_capability_probe)
endif()

if(SB_BUILD_UUID_IDENTITY_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_UUID_IDENTITY_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_UUID_IDENTITY_PROBE)
  add_subdirectory(tools/sb_uuid_identity_probe)
endif()

if(SB_BUILD_TIME_AUTHORITY_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_TIME_AUTHORITY_PROBE AND NOT TARGET sb_core_time)
  add_subdirectory(src/core/time)
endif()
if(SB_BUILD_TIME_AUTHORITY_PROBE)
  add_subdirectory(tools/sb_time_authority_probe)
endif()

option(SB_BUILD_STORAGE_FOUNDATION_PROBE "Build the storage foundation datatype/page/catalog persistence probe" OFF)
if(SB_BUILD_STORAGE_FOUNDATION_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_STORAGE_FOUNDATION_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_STORAGE_FOUNDATION_PROBE AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_STORAGE_FOUNDATION_PROBE AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if(SB_BUILD_STORAGE_FOUNDATION_PROBE AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_STORAGE_FOUNDATION_PROBE AND NOT TARGET sb_storage_page)
  add_subdirectory(src/storage/page)
endif()
if(SB_BUILD_STORAGE_FOUNDATION_PROBE AND NOT TARGET sb_core_catalog)
  add_subdirectory(src/core/catalog)
endif()
if(SB_BUILD_STORAGE_FOUNDATION_PROBE AND NOT TARGET sb_storage_database)
  add_subdirectory(src/storage/database)
endif()
if(SB_BUILD_STORAGE_FOUNDATION_PROBE)
  add_subdirectory(tools/sb_storage_foundation_probe)
endif()

option(SB_BUILD_INDEX_BTREE_PAGE_PROBE "Build the index B-tree page body probe" OFF)
if(SB_BUILD_INDEX_BTREE_PAGE_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_INDEX_BTREE_PAGE_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_INDEX_BTREE_PAGE_PROBE AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_INDEX_BTREE_PAGE_PROBE AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_INDEX_BTREE_PAGE_PROBE AND NOT TARGET sb_storage_page)
  add_subdirectory(src/storage/page)
endif()
if(SB_BUILD_INDEX_BTREE_PAGE_PROBE)
  add_subdirectory(tools/sb_index_btree_page_probe)
endif()

option(SB_BUILD_INSERT_PAGE_RESERVATION_PROBE "Build the insert page reservation probe" OFF)
if(SB_BUILD_INSERT_PAGE_RESERVATION_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_INSERT_PAGE_RESERVATION_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_INSERT_PAGE_RESERVATION_PROBE AND NOT TARGET sb_core_metrics)
  add_subdirectory(src/core/metrics)
endif()
if(SB_BUILD_INSERT_PAGE_RESERVATION_PROBE AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_INSERT_PAGE_RESERVATION_PROBE AND NOT TARGET sb_storage_page)
  add_subdirectory(src/storage/page)
endif()
if(SB_BUILD_INSERT_PAGE_RESERVATION_PROBE)
  add_subdirectory(tools/sb_insert_page_reservation_probe)
endif()

option(SB_BUILD_INSERT_PAGE_SELECTION_PROBE "Build the insert page selection probe" OFF)
if(SB_BUILD_INSERT_PAGE_SELECTION_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_INSERT_PAGE_SELECTION_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_INSERT_PAGE_SELECTION_PROBE AND NOT TARGET sb_core_metrics)
  add_subdirectory(src/core/metrics)
endif()
if(SB_BUILD_INSERT_PAGE_SELECTION_PROBE AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_INSERT_PAGE_SELECTION_PROBE AND NOT TARGET sb_storage_page)
  add_subdirectory(src/storage/page)
endif()
if(SB_BUILD_INSERT_PAGE_SELECTION_PROBE)
  add_subdirectory(tools/sb_insert_page_selection_probe)
endif()

option(SB_BUILD_INSERT_FILESPACE_GROWTH_PROBE "Build the insert filespace growth probe" OFF)
if(SB_BUILD_INSERT_FILESPACE_GROWTH_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_INSERT_FILESPACE_GROWTH_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_INSERT_FILESPACE_GROWTH_PROBE AND NOT TARGET sb_core_metrics)
  add_subdirectory(src/core/metrics)
endif()
if(SB_BUILD_INSERT_FILESPACE_GROWTH_PROBE AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_INSERT_FILESPACE_GROWTH_PROBE AND NOT TARGET sb_storage_filespace)
  add_subdirectory(src/storage/filespace)
endif()
if(SB_BUILD_INSERT_FILESPACE_GROWTH_PROBE)
  add_subdirectory(tools/sb_insert_filespace_growth_probe)
endif()

option(SB_BUILD_PAGE_FILESPACE_HANDOFF_PROBE "Build the page/filespace handoff probe" OFF)
if(SB_BUILD_PAGE_FILESPACE_HANDOFF_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_PAGE_FILESPACE_HANDOFF_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_PAGE_FILESPACE_HANDOFF_PROBE AND NOT TARGET sb_core_metrics)
  add_subdirectory(src/core/metrics)
endif()
if(SB_BUILD_PAGE_FILESPACE_HANDOFF_PROBE AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_PAGE_FILESPACE_HANDOFF_PROBE AND NOT TARGET sb_storage_page)
  add_subdirectory(src/storage/page)
endif()
if(SB_BUILD_PAGE_FILESPACE_HANDOFF_PROBE)
  add_subdirectory(tools/sb_page_filespace_handoff_probe)
endif()

option(SB_BUILD_PAGE_CACHE_PROBE "Build the page cache and quarantine probe" OFF)
if(SB_BUILD_PAGE_CACHE_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_PAGE_CACHE_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_PAGE_CACHE_PROBE AND NOT TARGET sb_core_metrics)
  add_subdirectory(src/core/metrics)
endif()
if(SB_BUILD_PAGE_CACHE_PROBE AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_PAGE_CACHE_PROBE AND NOT TARGET sb_storage_page)
  add_subdirectory(src/storage/page)
endif()
if(SB_BUILD_PAGE_CACHE_PROBE)
  add_subdirectory(tools/sb_page_cache_probe)
endif()

option(SB_BUILD_SECONDARY_INDEX_DELTA_OVERLAY_PROBE "Build the secondary index delta overlay probe" OFF)
if(SB_BUILD_SECONDARY_INDEX_DELTA_OVERLAY_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_SECONDARY_INDEX_DELTA_OVERLAY_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_SECONDARY_INDEX_DELTA_OVERLAY_PROBE AND NOT TARGET sb_core_index)
  add_subdirectory(src/core/index)
endif()
if(SB_BUILD_SECONDARY_INDEX_DELTA_OVERLAY_PROBE)
  add_subdirectory(tools/sb_secondary_index_delta_overlay_probe)
endif()

option(SB_BUILD_SECONDARY_INDEX_DELTA_MERGE_PROBE "Build the secondary index delta merge probe" OFF)
if(SB_BUILD_SECONDARY_INDEX_DELTA_MERGE_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_SECONDARY_INDEX_DELTA_MERGE_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_SECONDARY_INDEX_DELTA_MERGE_PROBE AND NOT TARGET sb_core_index)
  add_subdirectory(src/core/index)
endif()
if(SB_BUILD_SECONDARY_INDEX_DELTA_MERGE_PROBE)
  add_subdirectory(tools/sb_secondary_index_delta_merge_probe)
endif()

option(SB_BUILD_OVERFLOW_INSERT_PERSISTENCE_PROBE "Build the overflow insert persistence probe" OFF)
if(SB_BUILD_OVERFLOW_INSERT_PERSISTENCE_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_OVERFLOW_INSERT_PERSISTENCE_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_OVERFLOW_INSERT_PERSISTENCE_PROBE AND NOT TARGET sb_core_metrics)
  add_subdirectory(src/core/metrics)
endif()
if(SB_BUILD_OVERFLOW_INSERT_PERSISTENCE_PROBE AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_OVERFLOW_INSERT_PERSISTENCE_PROBE AND NOT TARGET sb_storage_page)
  add_subdirectory(src/storage/page)
endif()
if(SB_BUILD_OVERFLOW_INSERT_PERSISTENCE_PROBE)
  add_subdirectory(tools/sb_overflow_insert_persistence_probe)
endif()

option(SB_BUILD_STRICT_BULK_LOAD_LIFECYCLE_PROBE "Build the strict bulk-load lifecycle probe" OFF)
if(SB_BUILD_STRICT_BULK_LOAD_LIFECYCLE_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_STRICT_BULK_LOAD_LIFECYCLE_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_STRICT_BULK_LOAD_LIFECYCLE_PROBE AND NOT TARGET sb_core_bulk_load)
  add_subdirectory(src/core/bulk_load)
endif()
if(SB_BUILD_STRICT_BULK_LOAD_LIFECYCLE_PROBE)
  add_subdirectory(tools/sb_strict_bulk_load_lifecycle_probe)
endif()

option(SB_BUILD_INSERT_PHYSICAL_INTEGRATION_PROBE "Build the insert physical integration probe" OFF)
if(SB_BUILD_INSERT_PHYSICAL_INTEGRATION_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_INSERT_PHYSICAL_INTEGRATION_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_INSERT_PHYSICAL_INTEGRATION_PROBE AND NOT TARGET sb_core_metrics)
  add_subdirectory(src/core/metrics)
endif()
if(SB_BUILD_INSERT_PHYSICAL_INTEGRATION_PROBE AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_INSERT_PHYSICAL_INTEGRATION_PROBE AND NOT TARGET sb_transaction_mga)
  add_subdirectory(src/transaction/mga)
endif()
if(SB_BUILD_INSERT_PHYSICAL_INTEGRATION_PROBE AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_INSERT_PHYSICAL_INTEGRATION_PROBE AND NOT TARGET sb_core_memory)
  add_subdirectory(src/core/memory)
endif()
if(SB_BUILD_INSERT_PHYSICAL_INTEGRATION_PROBE AND NOT TARGET sb_storage_page)
  add_subdirectory(src/storage/page)
endif()
if(SB_BUILD_INSERT_PHYSICAL_INTEGRATION_PROBE AND NOT TARGET sb_storage_filespace)
  add_subdirectory(src/storage/filespace)
endif()
if(SB_BUILD_INSERT_PHYSICAL_INTEGRATION_PROBE AND NOT TARGET sb_core_index)
  add_subdirectory(src/core/index)
endif()
if(SB_BUILD_INSERT_PHYSICAL_INTEGRATION_PROBE AND NOT TARGET sb_core_bulk_load)
  add_subdirectory(src/core/bulk_load)
endif()
if(SB_BUILD_INSERT_PHYSICAL_INTEGRATION_PROBE)
  add_subdirectory(tools/sb_insert_physical_integration_probe)
endif()


# SEARCH_KEY: SB_ENGINE_INTERNAL_API_SURFACE_SKELETON
if(NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()

option(SB_BUILD_ENGINE_INTERNAL_API_PROBES "Build engine internal API registry and smoke probes" OFF)
option(SB_BUILD_ENGINE_API_REGISTRY_PROBE "Build the engine API registry probe" OFF)
if((SB_BUILD_ENGINE_INTERNAL_API_PROBES OR SB_BUILD_ENGINE_API_REGISTRY_PROBE) AND NOT TARGET sb_engine_api_registry_probe)
  add_subdirectory(tools/sb_engine_api_registry_probe)
endif()
if(SB_BUILD_ENGINE_INTERNAL_API_PROBES AND NOT TARGET sb_engine_api_smoke_probe)
  add_subdirectory(tools/sb_engine_api_smoke_probe)
endif()

# SEARCH_KEY: SB_ENGINE_API_CRUD_VERTICAL_SLICE_PROBE
option(SB_BUILD_ENGINE_API_CRUD_PROBE "Build engine API CRUD vertical-slice probe" OFF)
if(SB_BUILD_ENGINE_API_CRUD_PROBE)
  add_subdirectory(tools/sb_engine_api_crud_probe)
endif()

# SEARCH_KEY: SB_ENGINE_API_TRANSACTION_SEMANTICS_PROBE
option(SB_BUILD_ENGINE_API_TRANSACTION_SEMANTICS_PROBE "Build engine API transaction semantics probe" OFF)
if(SB_BUILD_ENGINE_API_TRANSACTION_SEMANTICS_PROBE)
  add_subdirectory(tools/sb_engine_api_transaction_semantics_probe)
endif()

# SEARCH_KEY: SB_ENGINE_API_INDEX_VERTICAL_SLICE_PROBE
option(SB_BUILD_ENGINE_API_INDEX_PROBE "Build engine API index vertical-slice probe" OFF)
if(SB_BUILD_ENGINE_API_INDEX_PROBE)
  add_subdirectory(tools/sb_engine_api_index_probe)
endif()

option(SB_BUILD_ENGINE_API_REMAINING_INDEXES_PROBE "Build the engine API remaining index families probe" OFF)
if(SB_BUILD_ENGINE_API_REMAINING_INDEXES_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_ENGINE_API_REMAINING_INDEXES_PROBE AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if(SB_BUILD_ENGINE_API_REMAINING_INDEXES_PROBE)
  add_subdirectory(tools/sb_engine_api_remaining_indexes_probe)
endif()

option(SB_BUILD_ENGINE_API_COMPLETION_PROBE "Build the engine API completion readiness probe" OFF)
option(SB_BUILD_ENGINE_API_DOMAIN_PROBE "Build the engine API domain foundation probe" OFF)
option(SB_BUILD_WIRE_DRIVER_DATATYPE_METADATA_PROBE "Build the wire/driver datatype metadata probe" OFF)
option(SB_BUILD_DATATYPE_STORAGE_BACKUP_REPLICATION_PROBE "Build the datatype storage/backup/replication transport probe" OFF)
option(SB_BUILD_COMPAT_DATATYPE_COVERAGE_PROBE "Build the compatibility datatype coverage/conformance probe" OFF)
option(SB_BUILD_COMPAT_EXACT_REGISTRY_PROBE "Build the compatibility exact datatype registry probe" OFF)
option(SB_BUILD_CANONICAL_DATATYPE_CLOSURE_PROBE "Build the canonical datatype closure/conformance probe" OFF)
option(SB_BUILD_DATATYPE_INDEX_OPTIMIZER_ADMISSION_PROBE "Build the datatype index/statistics/optimizer admission probe" OFF)
option(SB_BUILD_DOMAIN_FULL_FUNCTIONALITY_PROBE "Build the domain full functionality conformance probe" OFF)
option(SB_BUILD_SBLR_API_MATRIX_PROBE "Build the SBLR/API operation matrix probe" OFF)
option(SB_BUILD_DOMAIN_PATH_MASKING_PROBE "Build the domain path masking/unmask validation probe" OFF)
option(SB_BUILD_DOMAIN_GRANT_ENFORCEMENT_PROBE "Build the domain grant enforcement validation probe" OFF)
option(SB_BUILD_DOMAIN_ENCRYPTION_KEY_POLICY_PROBE "Build the domain encryption key-policy validation probe" OFF)
option(SB_BUILD_DOMAIN_METHOD_DISPATCH_PROBE "Build the domain method dispatch validation probe" OFF)
option(SB_BUILD_DOMAIN_SBLR_PREDICATE_PROBE "Build the domain SBLR predicate validation probe" OFF)
option(SB_BUILD_DECIMAL_DECFLOAT_CLOSURE_PROBE "Build the decimal/decimal-float closure validation probe" OFF)
option(SB_BUILD_REAL128_SPECIAL_VALUE_PROBE "Build the real128 special-value validation probe" OFF)
option(SB_BUILD_COMPAT_TIMEZONE_WIRE_PROFILE_PROBE "Build the compatibility timezone wire-profile validation probe" OFF)
option(SB_BUILD_DOCUMENT_CANONICALIZATION_PROBE "Build the document canonicalization validation probe" OFF)
option(SB_BUILD_ADVANCED_DATATYPE_FAMILY_PROBE "Build the advanced datatype family validation probe" OFF)
if((SB_BUILD_ENGINE_API_COMPLETION_PROBE OR SB_BUILD_ENGINE_API_DOMAIN_PROBE OR SB_BUILD_WIRE_DRIVER_DATATYPE_METADATA_PROBE OR SB_BUILD_DATATYPE_STORAGE_BACKUP_REPLICATION_PROBE OR SB_BUILD_COMPAT_DATATYPE_COVERAGE_PROBE OR SB_BUILD_CANONICAL_DATATYPE_CLOSURE_PROBE OR SB_BUILD_DATATYPE_INDEX_OPTIMIZER_ADMISSION_PROBE OR SB_BUILD_DOMAIN_FULL_FUNCTIONALITY_PROBE OR SB_BUILD_SBLR_API_MATRIX_PROBE OR SB_BUILD_DOMAIN_PATH_MASKING_PROBE OR SB_BUILD_DOMAIN_GRANT_ENFORCEMENT_PROBE OR SB_BUILD_DOMAIN_ENCRYPTION_KEY_POLICY_PROBE OR SB_BUILD_DOMAIN_METHOD_DISPATCH_PROBE OR SB_BUILD_DOMAIN_SBLR_PREDICATE_PROBE) AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if((SB_BUILD_ENGINE_API_DOMAIN_PROBE OR SB_BUILD_WIRE_DRIVER_DATATYPE_METADATA_PROBE OR SB_BUILD_DATATYPE_STORAGE_BACKUP_REPLICATION_PROBE OR SB_BUILD_COMPAT_DATATYPE_COVERAGE_PROBE OR SB_BUILD_CANONICAL_DATATYPE_CLOSURE_PROBE OR SB_BUILD_DATATYPE_INDEX_OPTIMIZER_ADMISSION_PROBE OR SB_BUILD_DOMAIN_FULL_FUNCTIONALITY_PROBE OR SB_BUILD_DOMAIN_PATH_MASKING_PROBE OR SB_BUILD_DOMAIN_GRANT_ENFORCEMENT_PROBE OR SB_BUILD_DOMAIN_ENCRYPTION_KEY_POLICY_PROBE OR SB_BUILD_DOMAIN_METHOD_DISPATCH_PROBE OR SB_BUILD_DOMAIN_SBLR_PREDICATE_PROBE) AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if(SB_BUILD_ENGINE_API_COMPLETION_PROBE)
  add_subdirectory(tools/sb_engine_api_completion_probe)
endif()
if(SB_BUILD_ENGINE_API_DOMAIN_PROBE)
  add_subdirectory(tools/sb_engine_api_domain_probe)
endif()
if(SB_BUILD_WIRE_DRIVER_DATATYPE_METADATA_PROBE)
  add_subdirectory(tools/sb_wire_driver_datatype_metadata_probe)
endif()
if(SB_BUILD_DATATYPE_STORAGE_BACKUP_REPLICATION_PROBE)
  add_subdirectory(tools/sb_datatype_storage_backup_replication_probe)
endif()
if(SB_BUILD_COMPAT_DATATYPE_COVERAGE_PROBE)
  add_subdirectory(tools/sb_reference_datatype_coverage_probe)
endif()
if(SB_BUILD_COMPAT_EXACT_REGISTRY_PROBE)
  add_subdirectory(tools/sb_reference_exact_registry_probe)
endif()
if(SB_BUILD_CANONICAL_DATATYPE_CLOSURE_PROBE)
  add_subdirectory(tools/sb_canonical_datatype_closure_probe)
endif()
if(SB_BUILD_DATATYPE_INDEX_OPTIMIZER_ADMISSION_PROBE)
  add_subdirectory(tools/sb_datatype_index_optimizer_admission_probe)
endif()
if(SB_BUILD_DOMAIN_FULL_FUNCTIONALITY_PROBE)
  add_subdirectory(tools/sb_domain_full_functionality_probe)
endif()
if(SB_BUILD_SBLR_API_MATRIX_PROBE)
  add_subdirectory(tools/sb_sblr_api_matrix_probe)
endif()
if(SB_BUILD_DOMAIN_PATH_MASKING_PROBE)
  add_subdirectory(tools/sb_domain_path_masking_probe)
endif()
if(SB_BUILD_DOMAIN_GRANT_ENFORCEMENT_PROBE)
  add_subdirectory(tools/sb_domain_grant_enforcement_probe)
endif()
if(SB_BUILD_DOMAIN_ENCRYPTION_KEY_POLICY_PROBE)
  add_subdirectory(tools/sb_domain_encryption_key_policy_probe)
endif()
if(SB_BUILD_DOMAIN_METHOD_DISPATCH_PROBE)
  add_subdirectory(tools/sb_domain_method_dispatch_probe)
endif()
if(SB_BUILD_DOMAIN_SBLR_PREDICATE_PROBE)
  add_subdirectory(tools/sb_domain_sblr_predicate_probe)
endif()
if((SB_BUILD_DECIMAL_DECFLOAT_CLOSURE_PROBE OR SB_BUILD_REAL128_SPECIAL_VALUE_PROBE) AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_DECIMAL_DECFLOAT_CLOSURE_PROBE)
  add_subdirectory(tools/sb_decimal_decfloat_closure_probe)
endif()
if(SB_BUILD_REAL128_SPECIAL_VALUE_PROBE)
  add_subdirectory(tools/sb_real128_special_value_probe)
endif()
if(SB_BUILD_COMPAT_TIMEZONE_WIRE_PROFILE_PROBE AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if(SB_BUILD_COMPAT_TIMEZONE_WIRE_PROFILE_PROBE AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_COMPAT_TIMEZONE_WIRE_PROFILE_PROBE)
  add_subdirectory(tools/sb_reference_timezone_wire_profile_probe)
endif()
if(SB_BUILD_DOCUMENT_CANONICALIZATION_PROBE AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_DOCUMENT_CANONICALIZATION_PROBE)
  add_subdirectory(tools/sb_document_canonicalization_probe)
endif()
if(SB_BUILD_ADVANCED_DATATYPE_FAMILY_PROBE AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_ADVANCED_DATATYPE_FAMILY_PROBE)
  add_subdirectory(tools/sb_advanced_datatype_family_probe)
endif()

option(SB_BUILD_ENGINE_API_NONCLUSTER_BEHAVIOR_PROBE "Build the non-cluster engine API behavior closure probe" OFF)
option(SB_BUILD_ENGINE_API_QUERY_EXECUTOR_PROBE "Build the local non-cluster query executor probe" OFF)
if(SB_BUILD_ENGINE_API_NONCLUSTER_BEHAVIOR_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_ENGINE_API_QUERY_EXECUTOR_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_ENGINE_API_NONCLUSTER_BEHAVIOR_PROBE AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if(SB_BUILD_ENGINE_API_QUERY_EXECUTOR_PROBE AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if(SB_BUILD_ENGINE_API_NONCLUSTER_BEHAVIOR_PROBE)
  add_subdirectory(tools/sb_engine_api_noncluster_behavior_probe)
endif()
if(SB_BUILD_ENGINE_API_QUERY_EXECUTOR_PROBE)
  add_subdirectory(tools/sb_engine_api_query_executor_probe)
endif()

option(SB_BUILD_SECURITY_AUTHORITY_PROBE "Build the security authority conformance probe" OFF)
option(SB_BUILD_SECURITY_AUTHENTICATION_PROBE "Build the security authentication conformance probe" OFF)
option(SB_BUILD_SECURITY_AUTHORIZATION_PROBE "Build the security authorization/grant/visibility conformance probe" OFF)
option(SB_BUILD_SECURITY_EXTERNAL_GROUP_SYNC_PROBE "Build the external group sync and explainability probe" OFF)
option(SB_BUILD_SECURITY_POLICY_PROBE "Build the security policy fail-closed probe" OFF)
option(SB_BUILD_SECURITY_AUDIT_PROBE "Build the security audit evidence probe" OFF)
option(SB_BUILD_SECURITY_PROTECTED_MATERIAL_PROBE "Build the protected material release probe" OFF)
option(SB_BUILD_SECURITY_PLUGIN_TRUST_PROBE "Build the UDR and manager trust probe" OFF)
option(SB_BUILD_SECURITY_STANDARD_BUNDLE_PROBE "Build the standard security bundle seed probe" OFF)
option(SB_BUILD_SECURITY_SBLR_API_PROBE "Build the security SBLR/API dispatch probe" OFF)
option(SB_BUILD_SECURITY_DEEP_ENFORCEMENT_PROBE "Build the deep security enforcement conformance probe" OFF)
option(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES "Build every authentication provider plugin completion probe" OFF)
option(SB_BUILD_AUTH_PLUGIN_REGISTRY_PROBE "Build the authentication provider registry probe" OFF)
option(SB_BUILD_AUTH_PLUGIN_MANIFEST_POLICY_PROBE "Build the authentication provider manifest policy probe" OFF)
option(SB_BUILD_AUTH_PLUGIN_SECRET_PROVIDER_PROBE "Build the authentication provider protected-secret probe" OFF)
option(SB_BUILD_AUTH_PLUGIN_CHALLENGE_STATE_PROBE "Build the authentication provider challenge-state probe" OFF)
option(SB_BUILD_AUTH_PLUGIN_SCRAM_PASSWORD_PROBE "Build the SCRAM/password authentication provider probe" OFF)
option(SB_BUILD_AUTH_PLUGIN_TOKEN_AUTHKEY_PROBE "Build the token/API-key authentication provider probe" OFF)
option(SB_BUILD_AUTH_PLUGIN_PEER_IDENT_PROBE "Build the peer/ident authentication provider probe" OFF)
option(SB_BUILD_AUTH_PLUGIN_CERTIFICATE_MTLS_PROBE "Build the certificate/mTLS authentication provider probe" OFF)
option(SB_BUILD_AUTH_PLUGIN_PROXY_ASSERTION_PROBE "Build the proxy assertion authentication provider probe" OFF)
option(SB_BUILD_AUTH_PLUGIN_LDAP_AD_PROBE "Build the LDAP/AD authentication provider probe" OFF)
option(SB_BUILD_AUTH_PLUGIN_KERBEROS_PAC_PROBE "Build the Kerberos/PAC authentication provider probe" OFF)
option(SB_BUILD_AUTH_PLUGIN_PAM_PROBE "Build the PAM authentication provider probe" OFF)
option(SB_BUILD_AUTH_PLUGIN_RADIUS_PROBE "Build the RADIUS authentication provider probe" OFF)
option(SB_BUILD_AUTH_PLUGIN_OIDC_JWT_PROBE "Build the OIDC/JWT authentication provider probe" OFF)
option(SB_BUILD_AUTH_PLUGIN_SAML_PROBE "Build the SAML authentication provider probe" OFF)
option(SB_BUILD_AUTH_PLUGIN_WEBAUTHN_MFA_PROBE "Build the WebAuthn/MFA authentication provider probe" OFF)
option(SB_BUILD_AUTH_PLUGIN_WORKLOAD_IDENTITY_PROBE "Build the workload identity authentication provider probe" OFF)
option(SB_BUILD_AUTH_PROVIDER_GROUP_SYNC_PROBE "Build the authentication provider group sync probe" OFF)
option(SB_BUILD_AUTH_PROVIDER_POLICY_ENFORCEMENT_PROBE "Build the authentication provider policy enforcement probe" OFF)
option(SB_BUILD_AUTH_PROVIDER_AUDIT_METRICS_PROBE "Build the authentication provider audit/metrics probe" OFF)
option(SB_BUILD_AUTH_PLUGIN_SBLR_API_PROBE "Build the authentication provider SBLR/API dispatch probe" OFF)
option(SB_BUILD_AUTH_PROVIDER_LIVE_ADAPTER_PROBE "Build the authentication provider live adapter probe" OFF)

set(SB_BUILD_SECURITY_SYSTEM_PROBES OFF)
if(SB_BUILD_SECURITY_AUTHORITY_PROBE
   OR SB_BUILD_SECURITY_AUTHENTICATION_PROBE
   OR SB_BUILD_SECURITY_AUTHORIZATION_PROBE
   OR SB_BUILD_SECURITY_EXTERNAL_GROUP_SYNC_PROBE
   OR SB_BUILD_SECURITY_POLICY_PROBE
   OR SB_BUILD_SECURITY_AUDIT_PROBE
   OR SB_BUILD_SECURITY_PROTECTED_MATERIAL_PROBE
   OR SB_BUILD_SECURITY_PLUGIN_TRUST_PROBE
   OR SB_BUILD_SECURITY_STANDARD_BUNDLE_PROBE
   OR SB_BUILD_SECURITY_SBLR_API_PROBE
   OR SB_BUILD_SECURITY_DEEP_ENFORCEMENT_PROBE)
  set(SB_BUILD_SECURITY_SYSTEM_PROBES ON)
endif()

if(SB_BUILD_SECURITY_SYSTEM_PROBES AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_SECURITY_SYSTEM_PROBES AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_SECURITY_SYSTEM_PROBES AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_SECURITY_SYSTEM_PROBES AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if(SB_BUILD_SECURITY_SYSTEM_PROBES AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if(SB_BUILD_SECURITY_SBLR_API_PROBE AND NOT TARGET sb_engine_sblr)
  add_subdirectory(src/engine/sblr)
endif()

if(SB_BUILD_SECURITY_AUTHORITY_PROBE)
  add_subdirectory(tools/sb_security_authority_probe)
endif()
if(SB_BUILD_SECURITY_AUTHENTICATION_PROBE)
  add_subdirectory(tools/sb_security_authentication_probe)
endif()
if(SB_BUILD_SECURITY_AUTHORIZATION_PROBE)
  add_subdirectory(tools/sb_security_authorization_probe)
endif()
if(SB_BUILD_SECURITY_EXTERNAL_GROUP_SYNC_PROBE)
  add_subdirectory(tools/sb_security_external_group_sync_probe)
endif()
if(SB_BUILD_SECURITY_POLICY_PROBE)
  add_subdirectory(tools/sb_security_policy_probe)
endif()
if(SB_BUILD_SECURITY_AUDIT_PROBE)
  add_subdirectory(tools/sb_security_audit_probe)
endif()
if(SB_BUILD_SECURITY_PROTECTED_MATERIAL_PROBE)
  add_subdirectory(tools/sb_security_protected_material_probe)
endif()
if(SB_BUILD_SECURITY_PLUGIN_TRUST_PROBE)
  add_subdirectory(tools/sb_security_plugin_trust_probe)
endif()
if(SB_BUILD_SECURITY_STANDARD_BUNDLE_PROBE)
  add_subdirectory(tools/sb_security_standard_bundle_probe)
endif()
if(SB_BUILD_SECURITY_SBLR_API_PROBE)
  add_subdirectory(tools/sb_security_sblr_api_probe)
endif()
if(SB_BUILD_SECURITY_DEEP_ENFORCEMENT_PROBE)
  add_subdirectory(tools/sb_security_deep_enforcement_probe)
endif()

set(SB_BUILD_AUTH_PROVIDER_PLUGIN_PROBES OFF)
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES
   OR SB_BUILD_AUTH_PLUGIN_REGISTRY_PROBE
   OR SB_BUILD_AUTH_PLUGIN_MANIFEST_POLICY_PROBE
   OR SB_BUILD_AUTH_PLUGIN_SECRET_PROVIDER_PROBE
   OR SB_BUILD_AUTH_PLUGIN_CHALLENGE_STATE_PROBE
   OR SB_BUILD_AUTH_PLUGIN_SCRAM_PASSWORD_PROBE
   OR SB_BUILD_AUTH_PLUGIN_TOKEN_AUTHKEY_PROBE
   OR SB_BUILD_AUTH_PLUGIN_PEER_IDENT_PROBE
   OR SB_BUILD_AUTH_PLUGIN_CERTIFICATE_MTLS_PROBE
   OR SB_BUILD_AUTH_PLUGIN_PROXY_ASSERTION_PROBE
   OR SB_BUILD_AUTH_PLUGIN_LDAP_AD_PROBE
   OR SB_BUILD_AUTH_PLUGIN_KERBEROS_PAC_PROBE
   OR SB_BUILD_AUTH_PLUGIN_PAM_PROBE
   OR SB_BUILD_AUTH_PLUGIN_RADIUS_PROBE
   OR SB_BUILD_AUTH_PLUGIN_OIDC_JWT_PROBE
   OR SB_BUILD_AUTH_PLUGIN_SAML_PROBE
   OR SB_BUILD_AUTH_PLUGIN_WEBAUTHN_MFA_PROBE
   OR SB_BUILD_AUTH_PLUGIN_WORKLOAD_IDENTITY_PROBE
   OR SB_BUILD_AUTH_PROVIDER_GROUP_SYNC_PROBE
   OR SB_BUILD_AUTH_PROVIDER_POLICY_ENFORCEMENT_PROBE
   OR SB_BUILD_AUTH_PROVIDER_AUDIT_METRICS_PROBE
   OR SB_BUILD_AUTH_PLUGIN_SBLR_API_PROBE
   OR SB_BUILD_AUTH_PROVIDER_LIVE_ADAPTER_PROBE)
  set(SB_BUILD_AUTH_PROVIDER_PLUGIN_PROBES ON)
endif()

if(SB_BUILD_AUTH_PROVIDER_PLUGIN_PROBES AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_PROBES AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_PROBES AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_PROBES AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_PROBES AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if((SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PLUGIN_SBLR_API_PROBE) AND NOT TARGET sb_engine_sblr)
  add_subdirectory(src/engine/sblr)
endif()

if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PLUGIN_REGISTRY_PROBE)
  add_subdirectory(tools/sb_auth_plugin_registry_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PLUGIN_MANIFEST_POLICY_PROBE)
  add_subdirectory(tools/sb_auth_plugin_manifest_policy_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PLUGIN_SECRET_PROVIDER_PROBE)
  add_subdirectory(tools/sb_auth_plugin_secret_provider_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PLUGIN_CHALLENGE_STATE_PROBE)
  add_subdirectory(tools/sb_auth_plugin_challenge_state_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PLUGIN_SCRAM_PASSWORD_PROBE)
  add_subdirectory(tools/sb_auth_plugin_scram_password_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PLUGIN_TOKEN_AUTHKEY_PROBE)
  add_subdirectory(tools/sb_auth_plugin_token_authkey_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PLUGIN_PEER_IDENT_PROBE)
  add_subdirectory(tools/sb_auth_plugin_peer_ident_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PLUGIN_CERTIFICATE_MTLS_PROBE)
  add_subdirectory(tools/sb_auth_plugin_certificate_mtls_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PLUGIN_PROXY_ASSERTION_PROBE)
  add_subdirectory(tools/sb_auth_plugin_proxy_assertion_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PLUGIN_LDAP_AD_PROBE)
  add_subdirectory(tools/sb_auth_plugin_ldap_ad_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PLUGIN_KERBEROS_PAC_PROBE)
  add_subdirectory(tools/sb_auth_plugin_kerberos_pac_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PLUGIN_PAM_PROBE)
  add_subdirectory(tools/sb_auth_plugin_pam_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PLUGIN_RADIUS_PROBE)
  add_subdirectory(tools/sb_auth_plugin_radius_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PLUGIN_OIDC_JWT_PROBE)
  add_subdirectory(tools/sb_auth_plugin_oidc_jwt_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PLUGIN_SAML_PROBE)
  add_subdirectory(tools/sb_auth_plugin_saml_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PLUGIN_WEBAUTHN_MFA_PROBE)
  add_subdirectory(tools/sb_auth_plugin_webauthn_mfa_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PLUGIN_WORKLOAD_IDENTITY_PROBE)
  add_subdirectory(tools/sb_auth_plugin_workload_identity_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PROVIDER_GROUP_SYNC_PROBE)
  add_subdirectory(tools/sb_auth_provider_group_sync_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PROVIDER_POLICY_ENFORCEMENT_PROBE)
  add_subdirectory(tools/sb_auth_provider_policy_enforcement_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PROVIDER_AUDIT_METRICS_PROBE)
  add_subdirectory(tools/sb_auth_provider_audit_metrics_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PLUGIN_SBLR_API_PROBE)
  add_subdirectory(tools/sb_auth_plugin_sblr_api_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PROVIDER_LIVE_ADAPTER_PROBE)
  add_subdirectory(tools/sb_auth_provider_live_adapter_probe)
endif()
if(SB_BUILD_AUTH_PROVIDER_PLUGIN_COMPLETION_PROBES OR SB_BUILD_AUTH_PROVIDER_DEPENDENCY_INVENTORY_PROBE)
  add_subdirectory(tools/sb_auth_provider_dependency_inventory_probe)
endif()

set(SB_BUILD_SBLR_AST_EXECUTOR_PLANNER_OPTIMIZER_FOUNDATION OFF)
if(SB_BUILD_AST_CONTRACT_PROBE
   OR SB_BUILD_SBLR_ENVELOPE_PROBE
   OR SB_BUILD_SBLR_DISPATCH_PROBE
   OR SB_BUILD_SBLR_API_EQUIVALENCE_PROBE
   OR SB_BUILD_LOGICAL_PLAN_PROBE
   OR SB_BUILD_PLANNER_EXECUTION_PROBE
   OR SB_BUILD_OPTIMIZER_CONTRACT_PROBE
   OR SB_BUILD_OPTIMIZER_COST_PROBE
   OR SB_BUILD_OPTIMIZER_EXECUTION_PLAN_PROBES)
  set(SB_BUILD_SBLR_AST_EXECUTOR_PLANNER_OPTIMIZER_FOUNDATION ON)
endif()

if(SB_BUILD_SBLR_AST_EXECUTOR_PLANNER_OPTIMIZER_FOUNDATION AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_SBLR_AST_EXECUTOR_PLANNER_OPTIMIZER_FOUNDATION AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_SBLR_AST_EXECUTOR_PLANNER_OPTIMIZER_FOUNDATION AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if(SB_BUILD_SBLR_AST_EXECUTOR_PLANNER_OPTIMIZER_FOUNDATION AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_SBLR_AST_EXECUTOR_PLANNER_OPTIMIZER_FOUNDATION AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_SBLR_AST_EXECUTOR_PLANNER_OPTIMIZER_FOUNDATION AND NOT TARGET sb_storage_page)
  add_subdirectory(src/storage/page)
endif()
if(SB_BUILD_SBLR_AST_EXECUTOR_PLANNER_OPTIMIZER_FOUNDATION AND NOT TARGET sb_core_catalog)
  add_subdirectory(src/core/catalog)
endif()
if(SB_BUILD_SBLR_AST_EXECUTOR_PLANNER_OPTIMIZER_FOUNDATION AND NOT TARGET sb_storage_database)
  add_subdirectory(src/storage/database)
endif()
if(SB_BUILD_SBLR_AST_EXECUTOR_PLANNER_OPTIMIZER_FOUNDATION AND NOT TARGET sb_transaction_mga)
  add_subdirectory(src/transaction/mga)
endif()
if(SB_BUILD_SBLR_AST_EXECUTOR_PLANNER_OPTIMIZER_FOUNDATION AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if(SB_BUILD_SBLR_AST_EXECUTOR_PLANNER_OPTIMIZER_FOUNDATION AND NOT TARGET sb_engine_sblr)
  add_subdirectory(src/engine/sblr)
endif()
if((SB_BUILD_LOGICAL_PLAN_PROBE OR SB_BUILD_PLANNER_EXECUTION_PROBE OR SB_BUILD_OPTIMIZER_CONTRACT_PROBE OR SB_BUILD_OPTIMIZER_COST_PROBE OR SB_BUILD_OPTIMIZER_EXECUTION_PLAN_PROBES) AND NOT TARGET sb_engine_planner)
  add_subdirectory(src/engine/planner)
endif()
if((SB_BUILD_OPTIMIZER_CONTRACT_PROBE OR SB_BUILD_OPTIMIZER_COST_PROBE OR SB_BUILD_OPTIMIZER_EXECUTION_PLAN_PROBES) AND NOT TARGET sb_engine_optimizer)
  add_subdirectory(src/engine/optimizer)
endif()
if(SB_BUILD_AST_CONTRACT_PROBE AND NOT TARGET sb_parser_ast_model)
  add_subdirectory(src/parsers/shared/ast)
endif()
if(SB_BUILD_AST_CONTRACT_PROBE AND NOT TARGET sb_parser_bound_ast_model)
  add_subdirectory(src/parsers/shared/bound_ast)
endif()
if(SB_BUILD_AST_CONTRACT_PROBE AND NOT TARGET sb_parser_sblr_lowering)
  add_subdirectory(src/parsers/shared/lowering)
endif()
if(SB_BUILD_AST_CONTRACT_PROBE)
  add_subdirectory(tools/sb_ast_contract_probe)
endif()
if(SB_BUILD_SBLR_ENVELOPE_PROBE)
  add_subdirectory(tools/sb_sblr_envelope_probe)
endif()
if(SB_BUILD_SBLR_DISPATCH_PROBE)
  add_subdirectory(tools/sb_sblr_dispatch_probe)
endif()
if(SB_BUILD_SBLR_API_EQUIVALENCE_PROBE)
  add_subdirectory(tools/sb_sblr_api_equivalence_probe)
endif()
if(SB_BUILD_LOGICAL_PLAN_PROBE)
  add_subdirectory(tools/sb_logical_plan_probe)
endif()
if(SB_BUILD_PLANNER_EXECUTION_PROBE)
  add_subdirectory(tools/sb_planner_execution_probe)
endif()
if(SB_BUILD_OPTIMIZER_CONTRACT_PROBE)
  add_subdirectory(tools/sb_optimizer_contract_probe)
endif()
if(SB_BUILD_OPTIMIZER_COST_PROBE)
  add_subdirectory(tools/sb_optimizer_cost_probe)
endif()
if(SB_BUILD_OPTIMIZER_EXECUTION_PLAN_PROBES)
  add_subdirectory(tools/sb_optimizer_execution_plan_probes)
endif()

option(SB_BUILD_SBSQL_V3_SCOPE_PROBE "Build the SBSQL Native V3 scope matrix probe" OFF)
if(SB_BUILD_SBSQL_V3_SCOPE_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_scope_probe)
endif()

option(SB_BUILD_SBSQL_V3_LEXER_PROBE "Build the SBSQL Native V3 lexer contract probe" OFF)
option(SB_BUILD_SBSQL_V3_GRAMMAR_INVENTORY_PROBE "Build the SBSQL Native V3 grammar inventory probe" OFF)
if(SB_BUILD_SBSQL_V3_LEXER_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_lexer_probe)
endif()
if(SB_BUILD_SBSQL_V3_GRAMMAR_INVENTORY_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_grammar_inventory_probe)
endif()

option(SB_BUILD_SBSQL_V3_AST_INVENTORY_PROBE "Build the SBSQL Native V3 AST inventory probe" OFF)
option(SB_BUILD_SBSQL_V3_AST_ROUNDTRIP_PROBE "Build the SBSQL Native V3 AST roundtrip probe" OFF)
if((SB_BUILD_SBSQL_V3_AST_INVENTORY_PROBE OR SB_BUILD_SBSQL_V3_AST_ROUNDTRIP_PROBE) AND NOT TARGET sb_sbsql_v3_ast_catalog)
  add_subdirectory(src/parsers/shared/sbsql_v3_ast)
endif()
if(SB_BUILD_SBSQL_V3_AST_INVENTORY_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_ast_inventory_probe)
endif()
if(SB_BUILD_SBSQL_V3_AST_ROUNDTRIP_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_ast_roundtrip_probe)
endif()

option(SB_BUILD_SBSQL_V3_BINDING_PROBE "Build the SBSQL Native V3 binding matrix probe" OFF)
option(SB_BUILD_SBSQL_V3_NAME_RESOLUTION_PROBE "Build the SBSQL Native V3 name resolution probe" OFF)
option(SB_BUILD_SBSQL_V3_DESCRIPTOR_BINDING_PROBE "Build the SBSQL Native V3 descriptor binding probe" OFF)
if((SB_BUILD_SBSQL_V3_BINDING_PROBE OR SB_BUILD_SBSQL_V3_NAME_RESOLUTION_PROBE OR SB_BUILD_SBSQL_V3_DESCRIPTOR_BINDING_PROBE) AND NOT TARGET sb_sbsql_v3_binding_catalog)
  add_subdirectory(src/parsers/shared/sbsql_v3_binding)
endif()
if(SB_BUILD_SBSQL_V3_BINDING_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_binding_probe)
endif()
if(SB_BUILD_SBSQL_V3_NAME_RESOLUTION_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_name_resolution_probe)
endif()
if(SB_BUILD_SBSQL_V3_DESCRIPTOR_BINDING_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_descriptor_binding_probe)
endif()

option(SB_BUILD_SBSQL_V3_SBLR_OPCODE_REGISTRY_PROBE "Build the SBSQL Native V3 SBLR opcode registry probe" OFF)
option(SB_BUILD_SBSQL_V3_SBLR_ENCODE_DECODE_PROBE "Build the SBSQL Native V3 SBLR encode/decode probe" OFF)
option(SB_BUILD_SBLR_FUNCTION_EXECUTOR_PROBE "Build the SBLR function executor implementation probe" OFF)
option(SB_BUILD_SBLR_FUNCTION_COMPLETION_GATE "Build the SBLR function-family completion gate probe" OFF)
option(SB_BUILD_SBLR_FUNCTION_PROPERTY_PROBE "Build the SBLR function property/conformance probe" OFF)
option(SB_BUILD_SBLR_AGGREGATE_SPILL_PROBE "Build the SBLR aggregate spill/persistence probe" OFF)
option(SB_BUILD_SBLR_FUNCTION_FAMILY_PROBE "Build the SBLR function-family conformance probe" OFF)
option(SB_BUILD_SBLR_COMPAT_ALIAS_PROBE "Build the SBLR compatibility alias conformance probe" OFF)
option(SB_BUILD_SBLR_PLUGIN_ALIAS_PROBE "Build the SBLR plugin/extension alias conformance probe" OFF)
option(SB_BUILD_SBLR_EXPRESSION_RUNTIME_PROBE "Build the SBLR expression runtime conformance probe" OFF)
option(SB_BUILD_SBLR_PAYLOAD_PRESERVATION_PROBE "Build the SBLR value payload preservation probe" OFF)
option(SB_BUILD_SBLR_PROVIDER_SEAM_PROBE "Build the SBLR deterministic provider seam probe" OFF)
option(SB_BUILD_SBLR_THREAD_REENTRANCY_PROBE "Build the SBLR/function thread and reentrancy conformance probe" OFF)

if((SB_BUILD_SBSQL_V3_SBLR_OPCODE_REGISTRY_PROBE OR SB_BUILD_SBSQL_V3_SBLR_ENCODE_DECODE_PROBE) AND NOT TARGET sb_sbsql_v3_sblr_catalog)
  add_subdirectory(src/parsers/shared/sbsql_v3_sblr)
endif()

if(SB_BUILD_SBSQL_V3_SBLR_OPCODE_REGISTRY_PROBE)
  add_subdirectory(tools/sb_sblr_opcode_registry_probe)
endif()

if(SB_BUILD_SBSQL_V3_SBLR_ENCODE_DECODE_PROBE)
  add_subdirectory(tools/sb_sblr_encode_decode_probe)
endif()

if(SB_BUILD_SBLR_FUNCTION_EXECUTOR_PROBE)
  if(NOT TARGET sb_engine_internal_api)
    add_subdirectory(src/engine/internal_api)
  endif()
  if(NOT TARGET sb_engine_sblr)
    add_subdirectory(src/engine/sblr)
  endif()
  if(NOT TARGET sb_engine_functions)
    add_subdirectory(src/engine/functions)
  endif()
  add_subdirectory(tools/sb_sblr_function_executor_probe)
endif()

if(SB_BUILD_SBLR_FUNCTION_COMPLETION_GATE)
  if(NOT TARGET sb_engine_internal_api)
    add_subdirectory(src/engine/internal_api)
  endif()
  if(NOT TARGET sb_engine_sblr)
    add_subdirectory(src/engine/sblr)
  endif()
  if(NOT TARGET sb_engine_functions)
    add_subdirectory(src/engine/functions)
  endif()
  add_subdirectory(tools/sb_sblr_function_completion_gate)
endif()

if(SB_BUILD_SBLR_FUNCTION_PROPERTY_PROBE)
  if(NOT TARGET sb_engine_internal_api)
    add_subdirectory(src/engine/internal_api)
  endif()
  if(NOT TARGET sb_engine_sblr)
    add_subdirectory(src/engine/sblr)
  endif()
  if(NOT TARGET sb_engine_functions)
    add_subdirectory(src/engine/functions)
  endif()
  add_subdirectory(tools/sb_sblr_function_property_probe)
endif()

if(SB_BUILD_SBLR_AGGREGATE_SPILL_PROBE)
  if(NOT TARGET sb_engine_internal_api)
    add_subdirectory(src/engine/internal_api)
  endif()
  if(NOT TARGET sb_engine_sblr)
    add_subdirectory(src/engine/sblr)
  endif()
  if(NOT TARGET sb_engine_functions)
    add_subdirectory(src/engine/functions)
  endif()
  add_subdirectory(tools/sb_sblr_aggregate_spill_probe)
endif()

if(SB_BUILD_SBLR_FUNCTION_FAMILY_PROBE)
  if(NOT TARGET sb_engine_internal_api)
    add_subdirectory(src/engine/internal_api)
  endif()
  if(NOT TARGET sb_engine_sblr)
    add_subdirectory(src/engine/sblr)
  endif()
  if(NOT TARGET sb_engine_functions)
    add_subdirectory(src/engine/functions)
  endif()
  add_subdirectory(tools/sb_sblr_function_family_probe)
endif()

if(SB_BUILD_SBLR_COMPAT_ALIAS_PROBE)
  if(NOT TARGET sb_engine_internal_api)
    add_subdirectory(src/engine/internal_api)
  endif()
  if(NOT TARGET sb_engine_sblr)
    add_subdirectory(src/engine/sblr)
  endif()
  if(NOT TARGET sb_engine_functions)
    add_subdirectory(src/engine/functions)
  endif()
  add_subdirectory(tools/sb_sblr_reference_alias_probe)
endif()

if(SB_BUILD_SBLR_PLUGIN_ALIAS_PROBE)
  if(NOT TARGET sb_engine_internal_api)
    add_subdirectory(src/engine/internal_api)
  endif()
  if(NOT TARGET sb_engine_sblr)
    add_subdirectory(src/engine/sblr)
  endif()
  if(NOT TARGET sb_engine_functions)
    add_subdirectory(src/engine/functions)
  endif()
  add_subdirectory(tools/sb_sblr_plugin_alias_probe)
endif()

if(SB_BUILD_SBLR_EXPRESSION_RUNTIME_PROBE)
  if(NOT TARGET sb_engine_internal_api)
    add_subdirectory(src/engine/internal_api)
  endif()
  if(NOT TARGET sb_engine_sblr)
    add_subdirectory(src/engine/sblr)
  endif()
  if(NOT TARGET sb_engine_functions)
    add_subdirectory(src/engine/functions)
  endif()
  add_subdirectory(tools/sb_sblr_expression_runtime_probe)
endif()

if(SB_BUILD_SBLR_PAYLOAD_PRESERVATION_PROBE)
  if(NOT TARGET sb_engine_internal_api)
    add_subdirectory(src/engine/internal_api)
  endif()
  if(NOT TARGET sb_engine_sblr)
    add_subdirectory(src/engine/sblr)
  endif()
  add_subdirectory(tools/sb_sblr_payload_preservation_probe)
endif()

if(SB_BUILD_SBLR_PROVIDER_SEAM_PROBE)
  if(NOT TARGET sb_engine_internal_api)
    add_subdirectory(src/engine/internal_api)
  endif()
  if(NOT TARGET sb_engine_sblr)
    add_subdirectory(src/engine/sblr)
  endif()
  if(NOT TARGET sb_engine_functions)
    add_subdirectory(src/engine/functions)
  endif()
  add_subdirectory(tools/sb_sblr_provider_seam_probe)
endif()

if(SB_BUILD_SBLR_THREAD_REENTRANCY_PROBE)
  if(NOT TARGET sb_engine_internal_api)
    add_subdirectory(src/engine/internal_api)
  endif()
  if(NOT TARGET sb_engine_sblr)
    add_subdirectory(src/engine/sblr)
  endif()
  if(NOT TARGET sb_engine_functions)
    add_subdirectory(src/engine/functions)
  endif()
  add_subdirectory(tools/sb_sblr_thread_reentrancy_probe)
endif()

option(SB_BUILD_SBSQL_V3_SBLR_API_MAPPING_PROBE "Build the SBSQL Native V3 SBLR API mapping probe" OFF)
option(SB_BUILD_SBSQL_V3_SBLR_API_EQUIVALENCE_FULL_PROBE "Build the SBSQL Native V3 full SBLR/API equivalence probe" OFF)

if((SB_BUILD_SBSQL_V3_SBLR_API_MAPPING_PROBE OR SB_BUILD_SBSQL_V3_SBLR_API_EQUIVALENCE_FULL_PROBE) AND NOT TARGET sb_sbsql_v3_api_mapping_catalog)
  add_subdirectory(src/parsers/shared/sbsql_v3_api_mapping)
endif()

if(SB_BUILD_SBSQL_V3_SBLR_API_MAPPING_PROBE)
  add_subdirectory(tools/sb_sblr_api_mapping_probe)
endif()

if(SB_BUILD_SBSQL_V3_SBLR_API_EQUIVALENCE_FULL_PROBE)
  add_subdirectory(tools/sb_sblr_api_equivalence_full_probe)
endif()

option(SB_BUILD_SBSQL_V3_EXECUTOR_EXPRESSION_PROBE "Build the SBSQL Native V3 executor expression probe" OFF)
option(SB_BUILD_SBSQL_V3_EXECUTOR_FILTER_PROJECT_PROBE "Build the SBSQL Native V3 executor filter/project probe" OFF)
option(SB_BUILD_SBSQL_V3_EXECUTOR_SORT_LIMIT_PROBE "Build the SBSQL Native V3 executor sort/limit probe" OFF)
option(SB_BUILD_SBSQL_V3_EXECUTOR_AGGREGATE_PROBE "Build the SBSQL Native V3 executor aggregate probe" OFF)
option(SB_BUILD_SBSQL_V3_EXECUTOR_JOIN_PROBE "Build the SBSQL Native V3 executor join probe" OFF)
option(SB_BUILD_SBSQL_V3_EXECUTOR_WINDOW_PROBE "Build the SBSQL Native V3 executor window probe" OFF)
option(SB_BUILD_SBSQL_V3_EXECUTOR_CTE_SUBQUERY_PROBE "Build the SBSQL Native V3 executor CTE/subquery probe" OFF)
option(SB_BUILD_SBSQL_V3_EXECUTOR_SETOP_PROBE "Build the SBSQL Native V3 executor set-operation probe" OFF)
option(SB_BUILD_SBSQL_V3_EXECUTOR_DESCRIPTOR_RUNTIME_PROBE "Build the SBSQL Native V3 descriptor-bound executor runtime probe" OFF)

if((SB_BUILD_SBSQL_V3_EXECUTOR_EXPRESSION_PROBE OR SB_BUILD_SBSQL_V3_EXECUTOR_FILTER_PROJECT_PROBE OR SB_BUILD_SBSQL_V3_EXECUTOR_SORT_LIMIT_PROBE OR SB_BUILD_SBSQL_V3_EXECUTOR_AGGREGATE_PROBE OR SB_BUILD_SBSQL_V3_EXECUTOR_JOIN_PROBE OR SB_BUILD_SBSQL_V3_EXECUTOR_WINDOW_PROBE OR SB_BUILD_SBSQL_V3_EXECUTOR_CTE_SUBQUERY_PROBE OR SB_BUILD_SBSQL_V3_EXECUTOR_SETOP_PROBE OR SB_BUILD_SBSQL_V3_EXECUTOR_DESCRIPTOR_RUNTIME_PROBE) AND NOT TARGET sb_engine_executor)
  add_subdirectory(src/engine/executor)
endif()

if(SB_BUILD_SBSQL_V3_EXECUTOR_DESCRIPTOR_RUNTIME_PROBE)
  add_subdirectory(tools/sb_executor_descriptor_runtime_probe)
endif()
if(SB_BUILD_SBSQL_V3_EXECUTOR_EXPRESSION_PROBE)
  add_subdirectory(tools/sb_executor_expression_probe)
endif()
if(SB_BUILD_SBSQL_V3_EXECUTOR_FILTER_PROJECT_PROBE)
  add_subdirectory(tools/sb_executor_filter_project_probe)
endif()
if(SB_BUILD_SBSQL_V3_EXECUTOR_SORT_LIMIT_PROBE)
  add_subdirectory(tools/sb_executor_sort_limit_probe)
endif()
if(SB_BUILD_SBSQL_V3_EXECUTOR_AGGREGATE_PROBE)
  add_subdirectory(tools/sb_executor_aggregate_probe)
endif()
if(SB_BUILD_SBSQL_V3_EXECUTOR_JOIN_PROBE)
  add_subdirectory(tools/sb_executor_join_probe)
endif()
if(SB_BUILD_SBSQL_V3_EXECUTOR_WINDOW_PROBE)
  add_subdirectory(tools/sb_executor_window_probe)
endif()
if(SB_BUILD_SBSQL_V3_EXECUTOR_CTE_SUBQUERY_PROBE)
  add_subdirectory(tools/sb_executor_cte_subquery_probe)
endif()
if(SB_BUILD_SBSQL_V3_EXECUTOR_SETOP_PROBE)
  add_subdirectory(tools/sb_executor_setop_probe)
endif()

option(SB_BUILD_SBSQL_V3_PLANNER_QUERY_SHAPE_PROBE "Build the SBSQL Native V3 planner query-shape probe" OFF)
option(SB_BUILD_SBSQL_V3_OPTIMIZER_INDEX_CHOICE_PROBE "Build the SBSQL Native V3 optimizer index-choice probe" OFF)
option(SB_BUILD_SBSQL_V3_OPTIMIZER_JOIN_ORDER_PROBE "Build the SBSQL Native V3 optimizer join-order probe" OFF)
option(SB_BUILD_SBSQL_V3_OPTIMIZER_AGGREGATE_STRATEGY_PROBE "Build the SBSQL Native V3 optimizer aggregate-strategy probe" OFF)
option(SB_BUILD_SBSQL_V3_OPTIMIZER_SPECIALIZED_WORKLOAD_PROBE "Build the SBSQL Native V3 optimizer specialized-workload probe" OFF)

if((SB_BUILD_SBSQL_V3_PLANNER_QUERY_SHAPE_PROBE OR SB_BUILD_SBSQL_V3_OPTIMIZER_INDEX_CHOICE_PROBE OR SB_BUILD_SBSQL_V3_OPTIMIZER_JOIN_ORDER_PROBE OR SB_BUILD_SBSQL_V3_OPTIMIZER_AGGREGATE_STRATEGY_PROBE OR SB_BUILD_SBSQL_V3_OPTIMIZER_SPECIALIZED_WORKLOAD_PROBE) AND NOT TARGET sb_engine_plan_shapes)
  add_subdirectory(src/engine/planner)
endif()
if((SB_BUILD_SBSQL_V3_PLANNER_QUERY_SHAPE_PROBE OR SB_BUILD_SBSQL_V3_OPTIMIZER_INDEX_CHOICE_PROBE OR SB_BUILD_SBSQL_V3_OPTIMIZER_JOIN_ORDER_PROBE OR SB_BUILD_SBSQL_V3_OPTIMIZER_AGGREGATE_STRATEGY_PROBE OR SB_BUILD_SBSQL_V3_OPTIMIZER_SPECIALIZED_WORKLOAD_PROBE) AND NOT TARGET sb_engine_optimizer_contract_only)
  add_subdirectory(src/engine/optimizer)
endif()

if(SB_BUILD_SBSQL_V3_PLANNER_QUERY_SHAPE_PROBE)
  add_subdirectory(tools/sb_planner_query_shape_probe)
endif()
if(SB_BUILD_SBSQL_V3_OPTIMIZER_INDEX_CHOICE_PROBE AND NOT TARGET sb_optimizer_index_choice_probe)
  add_subdirectory(tools/sb_optimizer_index_choice_probe)
endif()
if(SB_BUILD_SBSQL_V3_OPTIMIZER_JOIN_ORDER_PROBE AND NOT TARGET sb_optimizer_join_order_probe)
  add_subdirectory(tools/sb_optimizer_join_order_probe)
endif()
if(SB_BUILD_SBSQL_V3_OPTIMIZER_AGGREGATE_STRATEGY_PROBE AND NOT TARGET sb_optimizer_aggregate_strategy_probe)
  add_subdirectory(tools/sb_optimizer_aggregate_strategy_probe)
endif()
if(SB_BUILD_SBSQL_V3_OPTIMIZER_SPECIALIZED_WORKLOAD_PROBE AND NOT TARGET sb_optimizer_specialized_workload_probe)
  add_subdirectory(tools/sb_optimizer_specialized_workload_probe)
endif()

option(SB_BUILD_SBSQL_V3_DDL_PROBE "Build the SBSQL Native V3 DDL/catalog command probe" OFF)
option(SB_BUILD_CATALOG_SCHEMA_TREE_COMMAND_PROBE "Build the recursive schema-tree command probe" OFF)
option(SB_BUILD_CATALOG_LOCALIZED_NAME_PROBE "Build the localized name/comment catalog probe" OFF)
option(SB_BUILD_CATALOG_DDL_TRANSACTION_PROBE "Build the catalog DDL transaction authority probe" OFF)
option(SB_BUILD_CATALOG_ARTIFACT_ROUNDTRIP_PROBE "Build the canonical catalog artifact round-trip probe" OFF)

if((SB_BUILD_SBSQL_V3_DDL_PROBE OR SB_BUILD_CATALOG_SCHEMA_TREE_COMMAND_PROBE OR SB_BUILD_CATALOG_LOCALIZED_NAME_PROBE OR SB_BUILD_CATALOG_DDL_TRANSACTION_PROBE OR SB_BUILD_CATALOG_ARTIFACT_ROUNDTRIP_PROBE) AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if((SB_BUILD_SBSQL_V3_DDL_PROBE OR SB_BUILD_CATALOG_SCHEMA_TREE_COMMAND_PROBE OR SB_BUILD_CATALOG_LOCALIZED_NAME_PROBE OR SB_BUILD_CATALOG_DDL_TRANSACTION_PROBE OR SB_BUILD_CATALOG_ARTIFACT_ROUNDTRIP_PROBE) AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if((SB_BUILD_SBSQL_V3_DDL_PROBE OR SB_BUILD_CATALOG_SCHEMA_TREE_COMMAND_PROBE OR SB_BUILD_CATALOG_LOCALIZED_NAME_PROBE OR SB_BUILD_CATALOG_DDL_TRANSACTION_PROBE OR SB_BUILD_CATALOG_ARTIFACT_ROUNDTRIP_PROBE) AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if((SB_BUILD_SBSQL_V3_DDL_PROBE OR SB_BUILD_CATALOG_SCHEMA_TREE_COMMAND_PROBE OR SB_BUILD_CATALOG_LOCALIZED_NAME_PROBE OR SB_BUILD_CATALOG_DDL_TRANSACTION_PROBE OR SB_BUILD_CATALOG_ARTIFACT_ROUNDTRIP_PROBE) AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if((SB_BUILD_SBSQL_V3_DDL_PROBE OR SB_BUILD_CATALOG_SCHEMA_TREE_COMMAND_PROBE OR SB_BUILD_CATALOG_LOCALIZED_NAME_PROBE OR SB_BUILD_CATALOG_DDL_TRANSACTION_PROBE OR SB_BUILD_CATALOG_ARTIFACT_ROUNDTRIP_PROBE) AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()

if(SB_BUILD_SBSQL_V3_DDL_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_ddl_probe)
endif()
if(SB_BUILD_CATALOG_SCHEMA_TREE_COMMAND_PROBE)
  add_subdirectory(tools/sb_catalog_schema_tree_command_probe)
endif()
if(SB_BUILD_CATALOG_LOCALIZED_NAME_PROBE)
  add_subdirectory(tools/sb_catalog_localized_name_probe)
endif()
if(SB_BUILD_CATALOG_DDL_TRANSACTION_PROBE)
  add_subdirectory(tools/sb_catalog_ddl_transaction_probe)
endif()
if(SB_BUILD_CATALOG_ARTIFACT_ROUNDTRIP_PROBE)
  add_subdirectory(tools/sb_catalog_artifact_roundtrip_probe)
endif()

option(SB_BUILD_SBSQL_V3_DML_PROBE "Build the SBSQL Native V3 DML command probe" OFF)
option(SB_BUILD_SBSQL_V3_TRANSACTION_COMMAND_PROBE "Build the SBSQL Native V3 transaction command probe" OFF)
option(SB_BUILD_SBSQL_V3_ROLLBACK_FAILURE_PROBE "Build the SBSQL Native V3 rollback failure probe" OFF)

if((SB_BUILD_SBSQL_V3_DML_PROBE OR SB_BUILD_SBSQL_V3_TRANSACTION_COMMAND_PROBE OR SB_BUILD_SBSQL_V3_ROLLBACK_FAILURE_PROBE) AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if((SB_BUILD_SBSQL_V3_DML_PROBE OR SB_BUILD_SBSQL_V3_TRANSACTION_COMMAND_PROBE OR SB_BUILD_SBSQL_V3_ROLLBACK_FAILURE_PROBE) AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if((SB_BUILD_SBSQL_V3_DML_PROBE OR SB_BUILD_SBSQL_V3_TRANSACTION_COMMAND_PROBE OR SB_BUILD_SBSQL_V3_ROLLBACK_FAILURE_PROBE) AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if((SB_BUILD_SBSQL_V3_DML_PROBE OR SB_BUILD_SBSQL_V3_TRANSACTION_COMMAND_PROBE OR SB_BUILD_SBSQL_V3_ROLLBACK_FAILURE_PROBE) AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if((SB_BUILD_SBSQL_V3_DML_PROBE OR SB_BUILD_SBSQL_V3_TRANSACTION_COMMAND_PROBE OR SB_BUILD_SBSQL_V3_ROLLBACK_FAILURE_PROBE) AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()

if(SB_BUILD_SBSQL_V3_DML_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_dml_probe)
endif()
if(SB_BUILD_SBSQL_V3_TRANSACTION_COMMAND_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_transaction_command_probe)
endif()
if(SB_BUILD_SBSQL_V3_ROLLBACK_FAILURE_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_rollback_failure_probe)
endif()

option(SB_BUILD_SBSQL_V3_SECURITY_POLICY_PROBE "Build the SBSQL Native V3 security/policy command probe" OFF)
option(SB_BUILD_SBSQL_V3_MANAGEMENT_OBSERVABILITY_PROBE "Build the SBSQL Native V3 management/config/support-bundle probe" OFF)
option(SB_BUILD_SBSQL_V3_METRICS_SHOW_PROBE "Build the SBSQL Native V3 metrics/show/explain probe" OFF)

if((SB_BUILD_SBSQL_V3_SECURITY_POLICY_PROBE OR SB_BUILD_SBSQL_V3_MANAGEMENT_OBSERVABILITY_PROBE OR SB_BUILD_SBSQL_V3_METRICS_SHOW_PROBE) AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if((SB_BUILD_SBSQL_V3_SECURITY_POLICY_PROBE OR SB_BUILD_SBSQL_V3_MANAGEMENT_OBSERVABILITY_PROBE OR SB_BUILD_SBSQL_V3_METRICS_SHOW_PROBE) AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if((SB_BUILD_SBSQL_V3_SECURITY_POLICY_PROBE OR SB_BUILD_SBSQL_V3_MANAGEMENT_OBSERVABILITY_PROBE OR SB_BUILD_SBSQL_V3_METRICS_SHOW_PROBE) AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if((SB_BUILD_SBSQL_V3_SECURITY_POLICY_PROBE OR SB_BUILD_SBSQL_V3_MANAGEMENT_OBSERVABILITY_PROBE OR SB_BUILD_SBSQL_V3_METRICS_SHOW_PROBE) AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if((SB_BUILD_SBSQL_V3_SECURITY_POLICY_PROBE OR SB_BUILD_SBSQL_V3_MANAGEMENT_OBSERVABILITY_PROBE OR SB_BUILD_SBSQL_V3_METRICS_SHOW_PROBE) AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()

if(SB_BUILD_SBSQL_V3_SECURITY_POLICY_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_security_policy_probe)
endif()
if(SB_BUILD_SBSQL_V3_MANAGEMENT_OBSERVABILITY_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_management_observability_probe)
endif()
if(SB_BUILD_SBSQL_V3_METRICS_SHOW_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_metrics_show_probe)
endif()

if((SB_BUILD_SBSQL_V3_NOSQL_DOCUMENT_KV_PROBE OR SB_BUILD_SBSQL_V3_SEARCH_VECTOR_PROBE OR SB_BUILD_SBSQL_V3_GRAPH_TIMESERIES_PROBE) AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if((SB_BUILD_SBSQL_V3_NOSQL_DOCUMENT_KV_PROBE OR SB_BUILD_SBSQL_V3_SEARCH_VECTOR_PROBE OR SB_BUILD_SBSQL_V3_GRAPH_TIMESERIES_PROBE) AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if((SB_BUILD_SBSQL_V3_NOSQL_DOCUMENT_KV_PROBE OR SB_BUILD_SBSQL_V3_SEARCH_VECTOR_PROBE OR SB_BUILD_SBSQL_V3_GRAPH_TIMESERIES_PROBE) AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if((SB_BUILD_SBSQL_V3_NOSQL_DOCUMENT_KV_PROBE OR SB_BUILD_SBSQL_V3_SEARCH_VECTOR_PROBE OR SB_BUILD_SBSQL_V3_GRAPH_TIMESERIES_PROBE) AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if((SB_BUILD_SBSQL_V3_NOSQL_DOCUMENT_KV_PROBE OR SB_BUILD_SBSQL_V3_SEARCH_VECTOR_PROBE OR SB_BUILD_SBSQL_V3_GRAPH_TIMESERIES_PROBE) AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if(SB_BUILD_SBSQL_V3_NOSQL_DOCUMENT_KV_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_nosql_document_kv_probe)
endif()
if(SB_BUILD_SBSQL_V3_SEARCH_VECTOR_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_search_vector_probe)
endif()
if(SB_BUILD_SBSQL_V3_GRAPH_TIMESERIES_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_graph_timeseries_probe)
endif()

if((SB_BUILD_SBSQL_V3_LLVM_GPU_PROBE OR SB_BUILD_SBSQL_V3_UDR_PARSER_PACKAGE_PROBE) AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if((SB_BUILD_SBSQL_V3_LLVM_GPU_PROBE OR SB_BUILD_SBSQL_V3_UDR_PARSER_PACKAGE_PROBE) AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if((SB_BUILD_SBSQL_V3_LLVM_GPU_PROBE OR SB_BUILD_SBSQL_V3_UDR_PARSER_PACKAGE_PROBE) AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if((SB_BUILD_SBSQL_V3_LLVM_GPU_PROBE OR SB_BUILD_SBSQL_V3_UDR_PARSER_PACKAGE_PROBE) AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if((SB_BUILD_SBSQL_V3_LLVM_GPU_PROBE OR SB_BUILD_SBSQL_V3_UDR_PARSER_PACKAGE_PROBE) AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if(SB_BUILD_SBSQL_V3_LLVM_GPU_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_llvm_gpu_probe)
endif()
if(SB_BUILD_SBSQL_V3_UDR_PARSER_PACKAGE_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_udr_parser_package_probe)
endif()
if(SB_BUILD_UDR_LIFECYCLE_PROBE)
  add_subdirectory(tools/sb_udr_lifecycle_probe)
endif()
if(SB_BUILD_LLVM_RUNTIME_PROBE)
  add_subdirectory(tools/sb_llvm_runtime_probe)
endif()
if(SB_BUILD_GPU_PROFILE_PROBE)
  add_subdirectory(tools/sb_gpu_profile_probe)
endif()
if(SB_BUILD_ENGINE_LIFECYCLE_PROBE)
  add_subdirectory(tools/sb_engine_lifecycle_probe)
endif()

if(SB_BUILD_SBSQL_V3_CLUSTER_FAIL_CLOSED_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_SBSQL_V3_CLUSTER_FAIL_CLOSED_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_SBSQL_V3_CLUSTER_FAIL_CLOSED_PROBE AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_SBSQL_V3_CLUSTER_FAIL_CLOSED_PROBE AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if(SB_BUILD_SBSQL_V3_CLUSTER_FAIL_CLOSED_PROBE AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if(SB_BUILD_SBSQL_V3_CLUSTER_FAIL_CLOSED_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_cluster_fail_closed_probe)
endif()

if(SB_BUILD_SBSQL_V3_DIAGNOSTIC_RENDERING_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_SBSQL_V3_DIAGNOSTIC_RENDERING_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_SBSQL_V3_DIAGNOSTIC_RENDERING_PROBE AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_SBSQL_V3_DIAGNOSTIC_RENDERING_PROBE AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if(SB_BUILD_SBSQL_V3_DIAGNOSTIC_RENDERING_PROBE AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if(SB_BUILD_SBSQL_V3_DIAGNOSTIC_RENDERING_PROBE)
  add_subdirectory(tools/sb_sbsql_v3_diagnostic_rendering_probe)
endif()

if(SB_BUILD_NATIVE_V3_PARSER_ISOLATION_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_NATIVE_V3_PARSER_ISOLATION_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_NATIVE_V3_PARSER_ISOLATION_PROBE AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_NATIVE_V3_PARSER_ISOLATION_PROBE AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if(SB_BUILD_NATIVE_V3_PARSER_ISOLATION_PROBE AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if(SB_BUILD_NATIVE_V3_PARSER_ISOLATION_PROBE AND NOT TARGET sb_engine_sblr)
  add_subdirectory(src/engine/sblr)
endif()
if(SB_BUILD_NATIVE_V3_PARSER_ISOLATION_PROBE AND NOT TARGET sb_parser_ast_model)
  add_subdirectory(src/parsers/shared/ast)
endif()
if(SB_BUILD_NATIVE_V3_PARSER_ISOLATION_PROBE AND NOT TARGET sb_parser_bound_ast_model)
  add_subdirectory(src/parsers/shared/bound_ast)
endif()
if(SB_BUILD_NATIVE_V3_PARSER_ISOLATION_PROBE AND NOT TARGET sb_native_v3_minimal_parser)
  add_subdirectory(src/parsers/native/v3/minimal)
endif()
if(SB_BUILD_NATIVE_V3_PARSER_ISOLATION_PROBE AND NOT TARGET sb_parser_sblr_lowering)
  add_subdirectory(src/parsers/shared/lowering)
endif()
if(SB_BUILD_NATIVE_V3_PARSER_ISOLATION_PROBE AND NOT TARGET sb_native_v3_parser_package)
  add_subdirectory(src/parsers/native/v3/package)
endif()
if(SB_BUILD_NATIVE_V3_PARSER_ISOLATION_PROBE)
  add_subdirectory(tools/sb_native_v3_parser_isolation_probe)
endif()

if(SB_BUILD_SBSQL_V3_CONFORMANCE_GATE)
  add_subdirectory(tools/sb_sbsql_v3_conformance_gate)
endif()

if(SB_BUILD_SBSQL_V3_FULL_GATE)
  add_subdirectory(tools/sb_sbsql_v3_full_gate)
endif()

set(SB_BUILD_DATATYPE_DOMAIN_EXECUTION_PLAN_PROBES OFF)
if(SB_BUILD_DATATYPE_CAST_MATRIX_PROBE OR SB_BUILD_DATATYPE_EXTRACT_PROBE OR SB_BUILD_DATATYPE_SET_PROBE OR
   SB_BUILD_DOMAIN_LIFECYCLE_PROBE OR SB_BUILD_DOMAIN_ENFORCEMENT_PROBE OR SB_BUILD_SBLR_DATATYPE_DOMAIN_PROBE OR
   SB_BUILD_SBLR_DATATYPE_FULL_SURFACE_PROBE)
  set(SB_BUILD_DATATYPE_DOMAIN_EXECUTION_PLAN_PROBES ON)
endif()

if(SB_BUILD_DATATYPE_DOMAIN_EXECUTION_PLAN_PROBES AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_DATATYPE_DOMAIN_EXECUTION_PLAN_PROBES AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if((SB_BUILD_DOMAIN_LIFECYCLE_PROBE OR SB_BUILD_DOMAIN_ENFORCEMENT_PROBE OR SB_BUILD_SBLR_DATATYPE_DOMAIN_PROBE OR SB_BUILD_SBLR_DATATYPE_FULL_SURFACE_PROBE) AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if((SB_BUILD_DOMAIN_LIFECYCLE_PROBE OR SB_BUILD_DOMAIN_ENFORCEMENT_PROBE OR SB_BUILD_SBLR_DATATYPE_DOMAIN_PROBE OR SB_BUILD_SBLR_DATATYPE_FULL_SURFACE_PROBE) AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if((SB_BUILD_DOMAIN_LIFECYCLE_PROBE OR SB_BUILD_DOMAIN_ENFORCEMENT_PROBE OR SB_BUILD_SBLR_DATATYPE_DOMAIN_PROBE OR SB_BUILD_SBLR_DATATYPE_FULL_SURFACE_PROBE) AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if((SB_BUILD_SBLR_DATATYPE_DOMAIN_PROBE OR SB_BUILD_SBLR_DATATYPE_FULL_SURFACE_PROBE) AND NOT TARGET sb_engine_sblr)
  add_subdirectory(src/engine/sblr)
endif()

if(SB_BUILD_DATATYPE_CAST_MATRIX_PROBE)
  add_subdirectory(tools/sb_datatype_cast_matrix_probe)
endif()
if(SB_BUILD_DATATYPE_EXTRACT_PROBE)
  add_subdirectory(tools/sb_datatype_extract_probe)
endif()
if(SB_BUILD_DATATYPE_SET_PROBE)
  add_subdirectory(tools/sb_datatype_set_probe)
endif()
if(SB_BUILD_DOMAIN_LIFECYCLE_PROBE)
  add_subdirectory(tools/sb_domain_lifecycle_probe)
endif()
if(SB_BUILD_DOMAIN_ENFORCEMENT_PROBE)
  add_subdirectory(tools/sb_domain_enforcement_probe)
endif()
if(SB_BUILD_SBLR_DATATYPE_DOMAIN_PROBE)
  add_subdirectory(tools/sb_sblr_datatype_domain_probe)
endif()
if(SB_BUILD_SBLR_DATATYPE_FULL_SURFACE_PROBE)
  add_subdirectory(tools/sb_sblr_datatype_full_surface_probe)
endif()

option(SB_BUILD_DOMAIN_READ_POLICY_PROBE "Build the domain read-policy probe" OFF)
if(SB_BUILD_DOMAIN_READ_POLICY_PROBE AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if((SB_BUILD_DOMAIN_READ_POLICY_PROBE OR TARGET sb_engine_internal_api) AND NOT TARGET sb_domain_read_policy_probe)
  add_subdirectory(tools/sb_domain_read_policy_probe)
endif()

option(SB_BUILD_CATALOG_NAME_REGISTRY_PROBE "Build the catalog name/schema resolution registry probe" OFF)
if(SB_BUILD_CATALOG_NAME_REGISTRY_PROBE AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if(SB_BUILD_CATALOG_NAME_REGISTRY_PROBE)
  add_subdirectory(tools/sb_catalog_name_registry_probe)
endif()

option(SB_BUILD_METRICS_REGISTRY_PROBE "Build the metrics registry and producer API probe" OFF)
if(SB_BUILD_METRICS_REGISTRY_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_METRICS_REGISTRY_PROBE AND NOT TARGET sb_core_metrics)
  add_subdirectory(src/core/metrics)
endif()
if(SB_BUILD_METRICS_REGISTRY_PROBE)
  add_subdirectory(tools/sb_metrics_registry_probe)
endif()

option(SB_BUILD_INSERT_OPTIMIZATION_METRICS_PROBE "Build the insert optimization metric descriptor probe" OFF)
if(SB_BUILD_INSERT_OPTIMIZATION_METRICS_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_INSERT_OPTIMIZATION_METRICS_PROBE AND NOT TARGET sb_core_metrics)
  add_subdirectory(src/core/metrics)
endif()
if(SB_BUILD_INSERT_OPTIMIZATION_METRICS_PROBE)
  add_subdirectory(tools/sb_insert_optimization_metrics_probe)
endif()

option(SB_BUILD_UPDATE_DELETE_OPTIMIZATION_PROBE "Build the update/delete optimization metric and evidence probe" OFF)
if(SB_BUILD_UPDATE_DELETE_OPTIMIZATION_PROBE AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if(SB_BUILD_UPDATE_DELETE_OPTIMIZATION_PROBE)
  add_subdirectory(tools/sb_update_delete_optimization_probe)
endif()

option(SB_BUILD_METRICS_RUNTIME_PROBE "Build the metrics runtime surface and contract probe" OFF)
if(SB_BUILD_METRICS_RUNTIME_PROBE AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if(SB_BUILD_METRICS_RUNTIME_PROBE)
  add_subdirectory(tools/sb_metrics_runtime_probe)
endif()

option(SB_BUILD_METRICS_HISTORY_STORAGE_PROBE "Build the persistent metric history storage probe" OFF)
option(SB_BUILD_METRICS_SERIES_IDENTITY_PROBE "Build the persistent metric series identity probe" OFF)
option(SB_BUILD_METRICS_RETENTION_POLICY_PROBE "Build the metric retention policy probe" OFF)
option(SB_BUILD_METRICS_ROLLUP_PROBE "Build the persistent metric rollup probe" OFF)
option(SB_BUILD_METRICS_RETENTION_CLEANUP_PROBE "Build the metric retention cleanup probe" OFF)
option(SB_BUILD_METRICS_HISTORY_REDACTION_PROBE "Build the metric history redaction probe" OFF)
option(SB_BUILD_METRICS_CLUSTER_HISTORY_PROBE "Build the cluster metric history fail-closed probe" OFF)
option(SB_BUILD_METRICS_NO_FAKE_HISTORY_PROBE "Build the no-fake metric history probe" OFF)
option(SB_BUILD_METRICS_SECURITY_PROBE "Build the metric security and rights probe" OFF)
option(SB_BUILD_LOGICAL_BACKUP_PROBE "Build the local logical backup/restore probe" OFF)
option(SB_BUILD_PHYSICAL_BACKUP_PROBE "Build the local physical backup/restore probe" OFF)
option(SB_BUILD_PIT_DELTA_PROBE "Build the local PIT write-after delta probe" OFF)
if((SB_BUILD_METRICS_HISTORY_STORAGE_PROBE
    OR SB_BUILD_METRICS_SERIES_IDENTITY_PROBE
    OR SB_BUILD_METRICS_RETENTION_POLICY_PROBE
    OR SB_BUILD_METRICS_ROLLUP_PROBE
    OR SB_BUILD_METRICS_RETENTION_CLEANUP_PROBE
    OR SB_BUILD_METRICS_HISTORY_REDACTION_PROBE
    OR SB_BUILD_METRICS_CLUSTER_HISTORY_PROBE
    OR SB_BUILD_METRICS_NO_FAKE_HISTORY_PROBE
    OR SB_BUILD_METRICS_SECURITY_PROBE
    OR SB_BUILD_LOGICAL_BACKUP_PROBE
    OR SB_BUILD_PHYSICAL_BACKUP_PROBE
    OR SB_BUILD_PIT_DELTA_PROBE) AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if(SB_BUILD_METRICS_HISTORY_STORAGE_PROBE)
  add_subdirectory(tools/sb_metrics_history_storage_probe)
endif()
if(SB_BUILD_METRICS_SERIES_IDENTITY_PROBE)
  add_subdirectory(tools/sb_metrics_series_identity_probe)
endif()
if(SB_BUILD_METRICS_RETENTION_POLICY_PROBE)
  add_subdirectory(tools/sb_metrics_retention_policy_probe)
endif()
if(SB_BUILD_METRICS_ROLLUP_PROBE)
  add_subdirectory(tools/sb_metrics_rollup_probe)
endif()
if(SB_BUILD_METRICS_RETENTION_CLEANUP_PROBE)
  add_subdirectory(tools/sb_metrics_retention_cleanup_probe)
endif()
if(SB_BUILD_METRICS_HISTORY_REDACTION_PROBE)
  add_subdirectory(tools/sb_metrics_history_redaction_probe)
endif()
if(SB_BUILD_METRICS_CLUSTER_HISTORY_PROBE)
  add_subdirectory(tools/sb_metrics_cluster_history_probe)
endif()
if(SB_BUILD_METRICS_NO_FAKE_HISTORY_PROBE)
  add_subdirectory(tools/sb_metrics_no_fake_history_probe)
endif()
if(SB_BUILD_METRICS_SECURITY_PROBE)
  add_subdirectory(tools/sb_metrics_security_probe)
endif()
if(SB_BUILD_LOGICAL_BACKUP_PROBE)
  add_subdirectory(tools/sb_logical_backup_probe)
endif()
if(SB_BUILD_PHYSICAL_BACKUP_PROBE)
  add_subdirectory(tools/sb_physical_backup_probe)
endif()
if(SB_BUILD_PIT_DELTA_PROBE)
  add_subdirectory(tools/sb_pit_delta_probe)
endif()

if(SB_BUILD_AGENT_RUNTIME_PROBES AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_AGENT_RUNTIME_PROBES AND NOT TARGET sb_core_metrics)
  add_subdirectory(src/core/metrics)
endif()
if(SB_BUILD_AGENT_RUNTIME_PROBES AND NOT TARGET sb_core_agents)
  add_subdirectory(src/core/agents)
endif()
if(SB_BUILD_AGENT_RUNTIME_PROBES AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if(SB_BUILD_AGENT_RUNTIME_PROBES AND NOT TARGET sb_engine_sblr)
  add_subdirectory(src/engine/sblr)
endif()
if(SB_BUILD_AGENT_RUNTIME_PROBES AND NOT TARGET sb_core_agents_cluster_boundary)
  add_library(sb_core_agents_cluster_boundary
    src/core/agents/agent_cluster_boundary.cpp
  )
  target_compile_features(sb_core_agents_cluster_boundary PUBLIC cxx_std_17)
  target_include_directories(sb_core_agents_cluster_boundary PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/agents
  )
  target_link_libraries(sb_core_agents_cluster_boundary PUBLIC sb_core_agents)
  target_link_libraries(sb_core_agents_cluster_boundary PRIVATE sb_cluster_provider)
endif()
if(SB_BUILD_AGENT_RUNTIME_PROBES)
  add_subdirectory(tools/sb_agent_runtime_probes)
endif()

option(SB_BUILD_STARTUP_OPEN_FILESPACE_CLOSURE_PROBE "Build the startup/open/filespace closure conformance probe" OFF)

if(SB_BUILD_STARTUP_OPEN_FILESPACE_CLOSURE_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_STARTUP_OPEN_FILESPACE_CLOSURE_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_STARTUP_OPEN_FILESPACE_CLOSURE_PROBE AND NOT TARGET sb_core_memory)
  add_subdirectory(src/core/memory)
endif()
if(SB_BUILD_STARTUP_OPEN_FILESPACE_CLOSURE_PROBE AND NOT TARGET sb_core_metrics)
  add_subdirectory(src/core/metrics)
endif()
if(SB_BUILD_STARTUP_OPEN_FILESPACE_CLOSURE_PROBE AND NOT TARGET sb_core_agents)
  add_subdirectory(src/core/agents)
endif()
if(SB_BUILD_STARTUP_OPEN_FILESPACE_CLOSURE_PROBE AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if(SB_BUILD_STARTUP_OPEN_FILESPACE_CLOSURE_PROBE AND NOT TARGET sb_core_catalog)
  add_subdirectory(src/core/catalog)
endif()
if(SB_BUILD_STARTUP_OPEN_FILESPACE_CLOSURE_PROBE AND NOT TARGET sbl_numeric)
  add_subdirectory(libraries/sbl_numeric)
endif()
if(SB_BUILD_STARTUP_OPEN_FILESPACE_CLOSURE_PROBE AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_STARTUP_OPEN_FILESPACE_CLOSURE_PROBE AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_STARTUP_OPEN_FILESPACE_CLOSURE_PROBE AND NOT TARGET sb_storage_page)
  add_subdirectory(src/storage/page)
endif()
if(SB_BUILD_STARTUP_OPEN_FILESPACE_CLOSURE_PROBE AND NOT TARGET sb_storage_filespace)
  add_subdirectory(src/storage/filespace)
endif()
if(SB_BUILD_STARTUP_OPEN_FILESPACE_CLOSURE_PROBE AND NOT TARGET sb_storage_database)
  add_subdirectory(src/storage/database)
endif()
if(SB_BUILD_STARTUP_OPEN_FILESPACE_CLOSURE_PROBE)
  add_subdirectory(tools/sb_startup_open_filespace_closure_probe)
  add_subdirectory(tools/sb_startup_open_filespace_expansion_probe)
endif()

option(SB_BUILD_SBLR_EXECUTOR_FUNCTIONS "Build the SBLR executor/function implementation slice" OFF)
if(SB_BUILD_SBLR_EXECUTOR_FUNCTIONS AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_SBLR_EXECUTOR_FUNCTIONS AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_SBLR_EXECUTOR_FUNCTIONS AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_SBLR_EXECUTOR_FUNCTIONS AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if(SB_BUILD_SBLR_EXECUTOR_FUNCTIONS AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_SBLR_EXECUTOR_FUNCTIONS AND NOT TARGET sb_storage_page)
  add_subdirectory(src/storage/page)
endif()
if(SB_BUILD_SBLR_EXECUTOR_FUNCTIONS AND NOT TARGET sb_core_catalog)
  add_subdirectory(src/core/catalog)
endif()
if(SB_BUILD_SBLR_EXECUTOR_FUNCTIONS AND NOT TARGET sb_storage_database)
  add_subdirectory(src/storage/database)
endif()
if(SB_BUILD_SBLR_EXECUTOR_FUNCTIONS AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if(SB_BUILD_SBLR_EXECUTOR_FUNCTIONS AND NOT TARGET sb_engine_executor)
  add_subdirectory(src/engine/executor)
endif()
if(SB_BUILD_SBLR_EXECUTOR_FUNCTIONS AND NOT TARGET sb_engine_sblr)
  add_subdirectory(src/engine/sblr)
endif()
if(SB_BUILD_SBLR_EXECUTOR_FUNCTIONS AND NOT TARGET sb_engine_functions)
  add_subdirectory(src/engine/functions)
endif()

option(SB_BUILD_OPTIMIZER_FULL_IMPLEMENTATION_PROBES "Build the full optimizer implementation probe set" OFF)
option(SB_BUILD_OPTIMIZER_FULL_CONTRACT_PROBE "Build the full optimizer contract probe" OFF)
option(SB_BUILD_OPTIMIZER_STATISTICS_FULL_PROBE "Build the full optimizer statistics probe" OFF)
option(SB_BUILD_OPTIMIZER_REWRITE_FULL_PROBE "Build the full optimizer rewrite probe" OFF)
option(SB_BUILD_OPTIMIZER_ACCESS_PATH_FULL_PROBE "Build the full optimizer access-path probe" OFF)
option(SB_BUILD_OPTIMIZER_JOIN_FULL_PROBE "Build the full optimizer join probe" OFF)
option(SB_BUILD_OPTIMIZER_AGGREGATE_WINDOW_FULL_PROBE "Build the full optimizer aggregate/window probe" OFF)
option(SB_BUILD_OPTIMIZER_COST_FULL_PROBE "Build the full optimizer cost probe" OFF)
option(SB_BUILD_OPTIMIZER_SPECIALIZED_FULL_PROBE "Build the full optimizer specialized-family probe" OFF)
option(SB_BUILD_OPTIMIZER_CACHE_INVALIDATION_PROBE "Build the full optimizer cache/invalidation probe" OFF)
option(SB_BUILD_OPTIMIZER_SECURITY_BARRIER_PROBE "Build the full optimizer security-barrier probe" OFF)

set(SB_BUILD_OPTIMIZER_FULL_ANY_PROBE OFF)
if(SB_BUILD_OPTIMIZER_FULL_IMPLEMENTATION_PROBES
   OR SB_BUILD_OPTIMIZER_FULL_CONTRACT_PROBE
   OR SB_BUILD_OPTIMIZER_STATISTICS_FULL_PROBE
   OR SB_BUILD_OPTIMIZER_REWRITE_FULL_PROBE
   OR SB_BUILD_OPTIMIZER_ACCESS_PATH_FULL_PROBE
   OR SB_BUILD_OPTIMIZER_JOIN_FULL_PROBE
   OR SB_BUILD_OPTIMIZER_AGGREGATE_WINDOW_FULL_PROBE
   OR SB_BUILD_OPTIMIZER_COST_FULL_PROBE
   OR SB_BUILD_OPTIMIZER_SPECIALIZED_FULL_PROBE
   OR SB_BUILD_OPTIMIZER_CACHE_INVALIDATION_PROBE
   OR SB_BUILD_OPTIMIZER_SECURITY_BARRIER_PROBE)
  set(SB_BUILD_OPTIMIZER_FULL_ANY_PROBE ON)
endif()

if(SB_BUILD_OPTIMIZER_FULL_ANY_PROBE AND NOT TARGET sb_core_platform)
  add_subdirectory(src/core/platform)
endif()
if(SB_BUILD_OPTIMIZER_FULL_ANY_PROBE AND NOT TARGET sb_core_uuid)
  add_subdirectory(src/core/uuid)
endif()
if(SB_BUILD_OPTIMIZER_FULL_ANY_PROBE AND NOT TARGET sbl_numeric)
  add_subdirectory(libraries/sbl_numeric)
endif()
if(SB_BUILD_OPTIMIZER_FULL_ANY_PROBE AND NOT TARGET sb_core_datatypes)
  add_subdirectory(src/core/datatypes)
endif()
if(SB_BUILD_OPTIMIZER_FULL_ANY_PROBE AND NOT TARGET sb_core_resources)
  add_subdirectory(src/core/resources)
endif()
if(SB_BUILD_OPTIMIZER_FULL_ANY_PROBE AND NOT TARGET sb_storage_disk)
  add_subdirectory(src/storage/disk)
endif()
if(SB_BUILD_OPTIMIZER_FULL_ANY_PROBE AND NOT TARGET sb_transaction_mga)
  add_subdirectory(src/transaction/mga)
endif()
if(SB_BUILD_OPTIMIZER_FULL_ANY_PROBE AND NOT TARGET sb_storage_page)
  add_subdirectory(src/storage/page)
endif()
if(SB_BUILD_OPTIMIZER_FULL_ANY_PROBE AND NOT TARGET sb_core_catalog)
  add_subdirectory(src/core/catalog)
endif()
if(SB_BUILD_OPTIMIZER_FULL_ANY_PROBE AND NOT TARGET sb_storage_database)
  add_subdirectory(src/storage/database)
endif()
if(SB_BUILD_OPTIMIZER_FULL_ANY_PROBE AND NOT TARGET sb_engine_internal_api)
  add_subdirectory(src/engine/internal_api)
endif()
if(SB_BUILD_OPTIMIZER_FULL_ANY_PROBE AND NOT TARGET sb_engine_sblr)
  add_subdirectory(src/engine/sblr)
endif()

if(SB_BUILD_OPTIMIZER_FULL_ANY_PROBE AND NOT TARGET sb_engine_plan_shapes)
  add_subdirectory(src/engine/planner)
endif()
if(SB_BUILD_OPTIMIZER_FULL_ANY_PROBE AND NOT TARGET sb_engine_optimizer_contract_only)
  add_subdirectory(src/engine/optimizer)
endif()

if(SB_BUILD_OPTIMIZER_FULL_IMPLEMENTATION_PROBES OR SB_BUILD_OPTIMIZER_FULL_CONTRACT_PROBE)
  add_subdirectory(tools/sb_optimizer_full_contract_probe)
endif()
if(SB_BUILD_OPTIMIZER_FULL_IMPLEMENTATION_PROBES OR SB_BUILD_OPTIMIZER_STATISTICS_FULL_PROBE)
  add_subdirectory(tools/sb_optimizer_statistics_full_probe)
endif()
if(SB_BUILD_OPTIMIZER_FULL_IMPLEMENTATION_PROBES OR SB_BUILD_OPTIMIZER_REWRITE_FULL_PROBE)
  add_subdirectory(tools/sb_optimizer_rewrite_full_probe)
endif()
if(SB_BUILD_OPTIMIZER_FULL_IMPLEMENTATION_PROBES OR SB_BUILD_OPTIMIZER_ACCESS_PATH_FULL_PROBE)
  add_subdirectory(tools/sb_optimizer_access_path_full_probe)
endif()
if(SB_BUILD_OPTIMIZER_FULL_IMPLEMENTATION_PROBES OR SB_BUILD_OPTIMIZER_JOIN_FULL_PROBE)
  add_subdirectory(tools/sb_optimizer_join_full_probe)
endif()
if(SB_BUILD_OPTIMIZER_FULL_IMPLEMENTATION_PROBES OR SB_BUILD_OPTIMIZER_AGGREGATE_WINDOW_FULL_PROBE)
  add_subdirectory(tools/sb_optimizer_aggregate_window_full_probe)
endif()
if(SB_BUILD_OPTIMIZER_FULL_IMPLEMENTATION_PROBES OR SB_BUILD_OPTIMIZER_COST_FULL_PROBE)
  add_subdirectory(tools/sb_optimizer_cost_full_probe)
endif()
if(SB_BUILD_OPTIMIZER_FULL_IMPLEMENTATION_PROBES OR SB_BUILD_OPTIMIZER_SPECIALIZED_FULL_PROBE)
  add_subdirectory(tools/sb_optimizer_specialized_full_probe)
endif()
if(SB_BUILD_OPTIMIZER_FULL_IMPLEMENTATION_PROBES OR SB_BUILD_OPTIMIZER_CACHE_INVALIDATION_PROBE)
  add_subdirectory(tools/sb_optimizer_cache_invalidation_probe)
endif()
if(SB_BUILD_OPTIMIZER_FULL_IMPLEMENTATION_PROBES OR SB_BUILD_OPTIMIZER_SECURITY_BARRIER_PROBE)
  add_subdirectory(tools/sb_optimizer_security_barrier_probe)
endif()

# Product-boundary aggregate target.
#

# SEARCH_KEY: EVN_IMPL_006_SERVER_SUBSCRIPTION_REGISTRY_CMAKE
option(SB_BUILD_SBMN_MANAGER "Build the standalone sbmn_manager executable product" OFF)
option(SB_BUILD_SB_LISTENER "Build the standalone sb_listener executable product" OFF)
option(SB_BUILD_SERVER "Build the standalone sb_server executable product skeleton" OFF)
option(SB_BUILD_SERVER_BOUNDARY_GUARD "Build the sb_server forbidden dependency boundary guard" OFF)
option(SB_BUILD_SERVER_EVENT_NOTIFICATIONS "Build sb_server event notification routing support" OFF)
option(SB_BUILD_DATABASE_LIFECYCLE_TESTS "Build database lifecycle contract/registry CTest gates" OFF)
if(SB_BUILD_PUBLIC_RELEASE_CORRECTNESS)
  set(SB_BUILD_SERVER ON CACHE BOOL
    "Build server core for public release correctness startup gates" FORCE)
  set(SB_BUILD_SB_LISTENER ON CACHE BOOL
    "Build listener runtime for public release correctness gates" FORCE)
  set(SB_BUILD_SBMN_MANAGER ON CACHE BOOL
    "Build node manager for public release correctness gates" FORCE)
endif()
if(SB_BUILD_DATABASE_LIFECYCLE_TESTS)
  set(SB_BUILD_SBMN_MANAGER ON)
  set(SB_BUILD_SERVER ON)
  set(SB_BUILD_SB_LISTENER ON)
  set(SB_BUILD_SBSQL_PARSER_WORKER ON)
  set(SB_BUILD_SBSQL_PARSER_WORKER_TESTS ON)
endif()
if((SB_BUILD_SBMN_MANAGER OR SB_BUILD_SB_LISTENER OR SB_BUILD_SERVER) AND NOT TARGET sbl_manager_protocol)
  add_subdirectory(src/manager/protocol)
endif()
if((SB_BUILD_SBMN_MANAGER OR SB_BUILD_SB_LISTENER OR SB_BUILD_SERVER) AND NOT TARGET sbl_listener_control_plane)
  add_subdirectory(src/listener)
endif()
if(SB_BUILD_SBMN_MANAGER)
  add_subdirectory(src/manager/node)
endif()
if(SB_BUILD_SB_LISTENER AND NOT TARGET sbl_listener_runtime)
  add_subdirectory(src/listener)
endif()

if(NOT TARGET sb_engine_sblr)
  add_subdirectory(src/engine/sblr)
endif()

if((SB_BUILD_DATABASE_LIFECYCLE_TESTS OR SB_BUILD_SERVER OR SB_BUILD_SB_LISTENER OR SB_BUILD_TESTS)
   AND NOT TARGET sb_local_ipc)
  add_subdirectory(src/ipc)
endif()

# PUBLIC_TARGET_EXPORTS
if(NOT TARGET sb_engine)
  add_library(sb_engine STATIC
    src/engine/engine_boundary.cpp
    src/engine/public_abi.cpp
  )
  target_compile_features(sb_engine PUBLIC cxx_std_20)
  target_include_directories(sb_engine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  )
endif()

if(NOT TARGET sb_engine_shared)
  add_library(sb_engine_shared SHARED
    src/engine/engine_boundary.cpp
    src/engine/public_abi.cpp
  )
  target_compile_features(sb_engine_shared PUBLIC cxx_std_20)
  target_include_directories(sb_engine_shared PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  )
  target_compile_definitions(sb_engine_shared PRIVATE SCRATCHBIRD_ENGINE_BUILDING_SHARED=1)
  set_target_properties(sb_engine_shared PROPERTIES
    OUTPUT_NAME SBcore
    EXPORT_NAME sb_engine
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
  )
endif()

if(SB_BUILD_SERVER OR SB_BUILD_SERVER_EVENT_NOTIFICATIONS)
  add_subdirectory(src/server)
endif()
if(SB_BUILD_SERVER_BOUNDARY_GUARD)
  add_subdirectory(tools/sb_server_boundary_guard)
endif()

# `sb_engine` is the engine component/product boundary. Internal module
# libraries such as SBLR, optimizer, planner, executor, functions, and internal
# API may remain separate CMake targets for build hygiene, but they are not
# deployable product binaries. `sb_server` hosts this engine target and owns
# IPC/listener/manager/session orchestration. Private `sb_cluster` extends the
# server product boundary in the cluster workstream.
if(NOT TARGET sb_engine)
  add_library(sb_engine STATIC
    src/engine/engine_boundary.cpp
    src/engine/public_abi.cpp
  )
  target_compile_features(sb_engine PUBLIC cxx_std_20)
  target_include_directories(sb_engine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  )
endif()

if(NOT TARGET sb_engine_shared)
  add_library(sb_engine_shared SHARED
    src/engine/engine_boundary.cpp
    src/engine/public_abi.cpp
  )
  target_compile_features(sb_engine_shared PUBLIC cxx_std_20)
  target_include_directories(sb_engine_shared PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  )
  target_compile_definitions(sb_engine_shared PRIVATE SCRATCHBIRD_ENGINE_BUILDING_SHARED=1)
  set_target_properties(sb_engine_shared PROPERTIES
    OUTPUT_NAME SBcore
    EXPORT_NAME sb_engine
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
  )
endif()

include(CMakePackageConfigHelpers)
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ScratchBirdEngineConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/ScratchBirdEngineConfig.cmake
  INSTALL_DESTINATION lib/cmake/ScratchBirdEngine
)

set(sb_engine_runtime_dependency_set_args)
if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(sb_engine_runtime_dependency_set_args
    RUNTIME_DEPENDENCY_SET ScratchBirdEngineRuntimeDeps
  )
endif()

install(TARGETS sb_engine_shared
  EXPORT ScratchBirdEngineTargets
  ${sb_engine_runtime_dependency_set_args}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)
if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
  endif()
  get_filename_component(SB_GNU_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
  install(RUNTIME_DEPENDENCY_SET ScratchBirdEngineRuntimeDeps
    DESTINATION bin
    DIRECTORIES
      "${SB_GNU_RUNTIME_DIR}"
    PRE_EXCLUDE_REGEXES
      "api-ms-"
      "ext-ms-"
    POST_EXCLUDE_REGEXES
      "[/\\\\][Ss]ystem32[/\\\\]"
      "[/\\\\][Ss]ysWOW64[/\\\\]"
      "[/\\\\][Ww]indows[/\\\\]"
  )
endif()
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/scratchbird/engine
  DESTINATION include/scratchbird
)
install(EXPORT ScratchBirdEngineTargets
  NAMESPACE ScratchBird::
  DESTINATION lib/cmake/ScratchBirdEngine
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ScratchBirdEngineConfig.cmake
  DESTINATION lib/cmake/ScratchBirdEngine
)

if(SB_INSTALL_NON_ENGINE_COMPONENTS)
  foreach(sb_installed_runtime_target IN ITEMS
      sb_server
      sb_listener
      sbmn_manager
      sbp_sbsql
      sbp_firebird)
    if(TARGET ${sb_installed_runtime_target})
      install(TARGETS ${sb_installed_runtime_target}
        RUNTIME DESTINATION bin
        COMPONENT non_engine_runtime
      )
    endif()
  endforeach()

  if(TARGET sb_ipc_tester)
    install(PROGRAMS $<TARGET_FILE:sb_ipc_tester>
      DESTINATION bin
      COMPONENT non_engine_runtime
    )
  endif()

  if(TARGET sbu_sbsql_parser_support)
    install(TARGETS sbu_sbsql_parser_support
      ARCHIVE DESTINATION lib COMPONENT non_engine_runtime
      LIBRARY DESTINATION lib COMPONENT non_engine_runtime
    )
  endif()

  if(TARGET sbu_firebird_parser_support)
    install(TARGETS sbu_firebird_parser_support
      ARCHIVE DESTINATION lib COMPONENT non_engine_runtime
      LIBRARY DESTINATION lib COMPONENT non_engine_runtime
    )
  endif()

  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/drivers/DriverPackageManifest.csv)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/drivers/DriverPackageManifest.csv
      DESTINATION share/scratchbird/drivers
      COMPONENT non_engine_runtime
    )
  endif()

  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/core_beta_qa)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/examples/core_beta_qa
      DESTINATION share/scratchbird/examples
      COMPONENT non_engine_runtime
    )
  endif()
endif()

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/resources/
  DESTINATION share/scratchbird/resources
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/config/templates/
  DESTINATION etc/scratchbird
)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/docs/public_api/CORE_BETA_PUBLIC_API_ABI_MANIFEST.json)
  install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/docs/public_api/CORE_BETA_PUBLIC_API_ABI_MANIFEST.json
    ${CMAKE_CURRENT_SOURCE_DIR}/docs/public_api/CORE_BETA_PUBLIC_API_ABI.md
    DESTINATION share/scratchbird/docs/public_api
  )
endif()

foreach(sb_engine_internal_module_target IN ITEMS
    sb_engine_internal_api
    sb_engine_sblr
    sb_engine_plan_shapes
    sb_engine_planner
    sb_engine_optimizer
    sb_engine_executor
    sb_engine_functions)
  if(TARGET ${sb_engine_internal_module_target})
    target_link_libraries(sb_engine PRIVATE ${sb_engine_internal_module_target})
    if(TARGET sb_engine_shared)
      target_link_libraries(sb_engine_shared PRIVATE ${sb_engine_internal_module_target})
    endif()
  endif()
endforeach()

if(TARGET sb_engine_shared AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
  target_link_options(sb_engine_shared PRIVATE "-Wl,--exclude-libs,ALL")
endif()

option(SB_BUILD_SB_LISTENER "Build the standalone sb_listener executable product" OFF)
if(SB_BUILD_SB_LISTENER AND NOT TARGET sbl_manager_protocol)
  add_subdirectory(src/manager/protocol)
endif()
if(SB_BUILD_SB_LISTENER AND NOT TARGET sbl_listener_runtime)
  add_subdirectory(src/listener)
endif()
option(SB_BUILD_PUBLIC_STANDALONE_OUTPUT
  "Build branded standalone public output artifacts under output/<platform>"
  ON)
option(SB_BUILD_SBSQL_PARSER_WORKER "Build the native SBSQL parser-worker product vertical slice" OFF)
option(SB_BUILD_SBSQL_PARSER_WORKER_TESTS "Build the SBSQL parser-worker vertical-slice conformance probes" OFF)
option(SB_BUILD_SBU_SBSQL_PARSER_SUPPORT "Build the trusted SBSQL parser-support UDR package" OFF)
option(SB_BUILD_NATIVE_PARSER_WORKER "Build the native SBWP parser-worker product vertical slice" OFF)
option(SB_BUILD_FIREBIRD_PARSER_WORKER "Build the Firebird compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_FIREBIRD_PARSER_WORKER_TESTS "Build the Firebird parser-worker vertical-slice conformance probes" OFF)
option(SB_BUILD_SBU_FIREBIRD_PARSER_SUPPORT "Build the trusted Firebird parser-support UDR package" OFF)
option(SB_BUILD_MYSQL_PARSER_WORKER "Build the MySQL compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_MYSQL_PARSER_WORKER_TESTS "Build the MySQL parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_MYSQL_PARSER_SUPPORT "Build the trusted MySQL parser-support UDR package" OFF)
option(SB_BUILD_POSTGRESQL_PARSER_WORKER "Build the PostgreSQL compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_POSTGRESQL_PARSER_WORKER_TESTS "Build the PostgreSQL parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_POSTGRESQL_PARSER_SUPPORT "Build the trusted PostgreSQL parser-support UDR package" OFF)
option(SB_BUILD_SQLITE_PARSER_WORKER "Build the SQLite compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_SQLITE_PARSER_WORKER_TESTS "Build the SQLite parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_SQLITE_PARSER_SUPPORT "Build the trusted SQLite parser-support UDR package" OFF)
option(SB_BUILD_MARIADB_PARSER_WORKER "Build the MariaDB compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_MARIADB_PARSER_WORKER_TESTS "Build the MariaDB parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_MARIADB_PARSER_SUPPORT "Build the trusted MariaDB parser-support UDR package" OFF)
option(SB_BUILD_DUCKDB_PARSER_WORKER "Build the DuckDB compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_DUCKDB_PARSER_WORKER_TESTS "Build the DuckDB parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_DUCKDB_PARSER_SUPPORT "Build the trusted DuckDB parser-support UDR package" OFF)
option(SB_BUILD_CLICKHOUSE_PARSER_WORKER "Build the ClickHouse compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_CLICKHOUSE_PARSER_WORKER_TESTS "Build the ClickHouse parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_CLICKHOUSE_PARSER_SUPPORT "Build the trusted ClickHouse parser-support UDR package" OFF)
option(SB_BUILD_TIDB_PARSER_WORKER "Build the TiDB compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_TIDB_PARSER_WORKER_TESTS "Build the TiDB parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_TIDB_PARSER_SUPPORT "Build the trusted TiDB parser-support UDR package" OFF)
option(SB_BUILD_VITESS_PARSER_WORKER "Build the Vitess compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_VITESS_PARSER_WORKER_TESTS "Build the Vitess parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_VITESS_PARSER_SUPPORT "Build the trusted Vitess parser-support UDR package" OFF)
option(SB_BUILD_COCKROACHDB_PARSER_WORKER "Build the CockroachDB compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_COCKROACHDB_PARSER_WORKER_TESTS "Build the CockroachDB parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_COCKROACHDB_PARSER_SUPPORT "Build the trusted CockroachDB parser-support UDR package" OFF)
option(SB_BUILD_YUGABYTEDB_PARSER_WORKER "Build the YugabyteDB compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_YUGABYTEDB_PARSER_WORKER_TESTS "Build the YugabyteDB parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_YUGABYTEDB_PARSER_SUPPORT "Build the trusted YugabyteDB parser-support UDR package" OFF)
option(SB_BUILD_CASSANDRA_PARSER_WORKER "Build the Cassandra compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_CASSANDRA_PARSER_WORKER_TESTS "Build the Cassandra parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_CASSANDRA_PARSER_SUPPORT "Build the trusted Cassandra parser-support UDR package" OFF)
option(SB_BUILD_MONGODB_PARSER_WORKER "Build the MongoDB compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_MONGODB_PARSER_WORKER_TESTS "Build the MongoDB parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_MONGODB_PARSER_SUPPORT "Build the trusted MongoDB parser-support UDR package" OFF)
option(SB_BUILD_REDIS_PARSER_WORKER "Build the Redis compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_REDIS_PARSER_WORKER_TESTS "Build the Redis parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_REDIS_PARSER_SUPPORT "Build the trusted Redis parser-support UDR package" OFF)
option(SB_BUILD_OPENSEARCH_SQL_PPL_PARSER_WORKER "Build the OpenSearch SQL/PPL compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_OPENSEARCH_SQL_PPL_PARSER_WORKER_TESTS "Build the OpenSearch SQL/PPL parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_OPENSEARCH_SQL_PPL_PARSER_SUPPORT "Build the trusted OpenSearch SQL/PPL parser-support UDR package" OFF)
option(SB_BUILD_OPENSEARCH_PARSER_WORKER "Build the OpenSearch REST compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_OPENSEARCH_PARSER_WORKER_TESTS "Build the OpenSearch REST parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_OPENSEARCH_PARSER_SUPPORT "Build the trusted OpenSearch REST parser-support UDR package" OFF)
option(SB_BUILD_NEO4J_PARSER_WORKER "Build the Neo4j compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_NEO4J_PARSER_WORKER_TESTS "Build the Neo4j parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_NEO4J_PARSER_SUPPORT "Build the trusted Neo4j parser-support UDR package" OFF)
option(SB_BUILD_INFLUXDB_PARSER_WORKER "Build the InfluxDB compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_INFLUXDB_PARSER_WORKER_TESTS "Build the InfluxDB parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_INFLUXDB_PARSER_SUPPORT "Build the trusted InfluxDB parser-support UDR package" OFF)
option(SB_BUILD_MILVUS_PARSER_WORKER "Build the Milvus compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_MILVUS_PARSER_WORKER_TESTS "Build the Milvus parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_MILVUS_PARSER_SUPPORT "Build the trusted Milvus parser-support UDR package" OFF)
option(SB_BUILD_DOLT_PARSER_WORKER "Build the Dolt compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_DOLT_PARSER_WORKER_TESTS "Build the Dolt parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_DOLT_PARSER_SUPPORT "Build the trusted Dolt parser-support UDR package" OFF)
option(SB_BUILD_APACHE_IGNITE_PARSER_WORKER "Build the Apache Ignite compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_APACHE_IGNITE_PARSER_WORKER_TESTS "Build the Apache Ignite parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_APACHE_IGNITE_PARSER_SUPPORT "Build the trusted Apache Ignite parser-support UDR package" OFF)
option(SB_BUILD_TIKV_PARSER_WORKER "Build the TiKV compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_TIKV_PARSER_WORKER_TESTS "Build the TiKV parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_TIKV_PARSER_SUPPORT "Build the trusted TiKV parser-support UDR package" OFF)
option(SB_BUILD_FOUNDATIONDB_PARSER_WORKER "Build the FoundationDB compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_FOUNDATIONDB_PARSER_WORKER_TESTS "Build the FoundationDB parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_FOUNDATIONDB_PARSER_SUPPORT "Build the trusted FoundationDB parser-support UDR package" OFF)
option(SB_BUILD_IMMUDB_PARSER_WORKER "Build the immudb compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_IMMUDB_PARSER_WORKER_TESTS "Build the immudb parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_IMMUDB_PARSER_SUPPORT "Build the trusted immudb parser-support UDR package" OFF)
option(SB_BUILD_XTDB_PARSER_WORKER "Build the XTDB compatibility parser-worker product vertical slice" OFF)
option(SB_BUILD_XTDB_PARSER_WORKER_TESTS "Build the XTDB parser-worker conformance probes" OFF)
option(SB_BUILD_SBU_XTDB_PARSER_SUPPORT "Build the trusted XTDB parser-support UDR package" OFF)
option(SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS
  "Build compatibility SQL parser and parser-support UDR proof gates"
  OFF)
if(SB_BUILD_PUBLIC_STANDALONE_OUTPUT)
  set(SB_BUILD_SBSQL_PARSER_WORKER ON CACHE BOOL
    "Build the native SBSQL parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_SBU_SBSQL_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted SBSQL parser-support UDR package"
    FORCE)
endif()
if(SB_BUILD_TESTS)
  set(SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS ON CACHE BOOL
    "Build compatibility SQL parser and parser-support UDR proof gates"
    FORCE)
  set(SB_BUILD_FIREBIRD_PARSER_WORKER ON CACHE BOOL
    "Build the Firebird compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_FIREBIRD_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the Firebird parser-worker vertical-slice conformance probes"
    FORCE)
  set(SB_BUILD_SBU_FIREBIRD_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted Firebird parser-support UDR package"
    FORCE)
  set(SB_BUILD_MYSQL_PARSER_WORKER ON CACHE BOOL
    "Build the MySQL compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_MYSQL_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the MySQL parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_MYSQL_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted MySQL parser-support UDR package"
    FORCE)
  set(SB_BUILD_POSTGRESQL_PARSER_WORKER ON CACHE BOOL
    "Build the PostgreSQL compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_POSTGRESQL_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the PostgreSQL parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_POSTGRESQL_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted PostgreSQL parser-support UDR package"
    FORCE)
  set(SB_BUILD_SQLITE_PARSER_WORKER ON CACHE BOOL
    "Build the SQLite compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_SQLITE_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the SQLite parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_SQLITE_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted SQLite parser-support UDR package"
    FORCE)
  set(SB_BUILD_MARIADB_PARSER_WORKER ON CACHE BOOL
    "Build the MariaDB compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_MARIADB_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the MariaDB parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_MARIADB_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted MariaDB parser-support UDR package"
    FORCE)
  set(SB_BUILD_DUCKDB_PARSER_WORKER ON CACHE BOOL
    "Build the DuckDB compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_DUCKDB_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the DuckDB parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_DUCKDB_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted DuckDB parser-support UDR package"
    FORCE)
  set(SB_BUILD_CLICKHOUSE_PARSER_WORKER ON CACHE BOOL
    "Build the ClickHouse compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_CLICKHOUSE_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the ClickHouse parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_CLICKHOUSE_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted ClickHouse parser-support UDR package"
    FORCE)
  set(SB_BUILD_TIDB_PARSER_WORKER ON CACHE BOOL
    "Build the TiDB compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_TIDB_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the TiDB parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_TIDB_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted TiDB parser-support UDR package"
    FORCE)
  set(SB_BUILD_VITESS_PARSER_WORKER ON CACHE BOOL
    "Build the Vitess compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_VITESS_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the Vitess parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_VITESS_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted Vitess parser-support UDR package"
    FORCE)
  set(SB_BUILD_COCKROACHDB_PARSER_WORKER ON CACHE BOOL
    "Build the CockroachDB compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_COCKROACHDB_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the CockroachDB parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_COCKROACHDB_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted CockroachDB parser-support UDR package"
    FORCE)
  set(SB_BUILD_YUGABYTEDB_PARSER_WORKER ON CACHE BOOL
    "Build the YugabyteDB compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_YUGABYTEDB_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the YugabyteDB parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_YUGABYTEDB_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted YugabyteDB parser-support UDR package"
    FORCE)
  set(SB_BUILD_CASSANDRA_PARSER_WORKER ON CACHE BOOL
    "Build the Cassandra compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_CASSANDRA_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the Cassandra parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_CASSANDRA_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted Cassandra parser-support UDR package"
    FORCE)
  set(SB_BUILD_MONGODB_PARSER_WORKER ON CACHE BOOL
    "Build the MongoDB compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_MONGODB_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the MongoDB parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_MONGODB_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted MongoDB parser-support UDR package"
    FORCE)
  set(SB_BUILD_REDIS_PARSER_WORKER ON CACHE BOOL
    "Build the Redis compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_REDIS_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the Redis parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_REDIS_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted Redis parser-support UDR package"
    FORCE)
  set(SB_BUILD_OPENSEARCH_SQL_PPL_PARSER_WORKER ON CACHE BOOL
    "Build the OpenSearch SQL/PPL compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_OPENSEARCH_SQL_PPL_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the OpenSearch SQL/PPL parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_OPENSEARCH_SQL_PPL_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted OpenSearch SQL/PPL parser-support UDR package"
    FORCE)
  set(SB_BUILD_OPENSEARCH_PARSER_WORKER ON CACHE BOOL
    "Build the OpenSearch REST compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_OPENSEARCH_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the OpenSearch REST parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_OPENSEARCH_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted OpenSearch REST parser-support UDR package"
    FORCE)
  set(SB_BUILD_NEO4J_PARSER_WORKER ON CACHE BOOL
    "Build the Neo4j compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_NEO4J_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the Neo4j parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_NEO4J_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted Neo4j parser-support UDR package"
    FORCE)
  set(SB_BUILD_INFLUXDB_PARSER_WORKER ON CACHE BOOL
    "Build the InfluxDB compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_INFLUXDB_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the InfluxDB parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_INFLUXDB_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted InfluxDB parser-support UDR package"
    FORCE)
  set(SB_BUILD_MILVUS_PARSER_WORKER ON CACHE BOOL
    "Build the Milvus compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_MILVUS_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the Milvus parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_MILVUS_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted Milvus parser-support UDR package"
    FORCE)
  set(SB_BUILD_DOLT_PARSER_WORKER ON CACHE BOOL
    "Build the Dolt compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_DOLT_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the Dolt parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_DOLT_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted Dolt parser-support UDR package"
    FORCE)
  set(SB_BUILD_APACHE_IGNITE_PARSER_WORKER ON CACHE BOOL
    "Build the Apache Ignite compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_APACHE_IGNITE_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the Apache Ignite parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_APACHE_IGNITE_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted Apache Ignite parser-support UDR package"
    FORCE)
  set(SB_BUILD_TIKV_PARSER_WORKER ON CACHE BOOL
    "Build the TiKV compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_TIKV_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the TiKV parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_TIKV_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted TiKV parser-support UDR package"
    FORCE)
  set(SB_BUILD_FOUNDATIONDB_PARSER_WORKER ON CACHE BOOL
    "Build the FoundationDB compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_FOUNDATIONDB_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the FoundationDB parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_FOUNDATIONDB_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted FoundationDB parser-support UDR package"
    FORCE)
  set(SB_BUILD_IMMUDB_PARSER_WORKER ON CACHE BOOL
    "Build the immudb compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_IMMUDB_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the immudb parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_IMMUDB_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted immudb parser-support UDR package"
    FORCE)
  set(SB_BUILD_XTDB_PARSER_WORKER ON CACHE BOOL
    "Build the XTDB compatibility parser-worker product vertical slice"
    FORCE)
  set(SB_BUILD_XTDB_PARSER_WORKER_TESTS ON CACHE BOOL
    "Build the XTDB parser-worker conformance probes"
    FORCE)
  set(SB_BUILD_SBU_XTDB_PARSER_SUPPORT ON CACHE BOOL
    "Build the trusted XTDB parser-support UDR package"
    FORCE)
endif()

if(SB_BUILD_SBSQL_PARSER_WORKER OR SB_BUILD_SBSQL_PARSER_WORKER_TESTS OR SB_BUILD_SBU_SBSQL_PARSER_SUPPORT)
  if(NOT TARGET sbl_manager_protocol)
    add_subdirectory(src/manager/protocol)
  endif()
  if(NOT TARGET sbl_listener_control_plane)
    add_subdirectory(src/listener)
  endif()
  if(NOT TARGET sbl_sbsql_parser_worker_core)
    add_subdirectory(src/parsers/sbsql_worker)
  endif()
endif()

if(SB_BUILD_NATIVE_PARSER_WORKER)
  if(NOT TARGET sbl_manager_protocol)
    add_subdirectory(src/manager/protocol)
  endif()
  if(NOT TARGET sbl_listener_control_plane)
    add_subdirectory(src/listener)
  endif()
  add_subdirectory(src/parsers/native/v3/worker)
endif()

if(WIN32 AND (SB_BUILD_TESTS OR SB_BUILD_DATABASE_LIFECYCLE_TESTS OR SB_BUILD_SB_LISTENER OR
              SB_BUILD_SBSQL_PARSER_WORKER_TESTS OR SB_BUILD_ENGINE_LISTENER_ENTERPRISE_TESTS))
  add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:-include>
    $<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/tests/support/sb_test_temp_compat.hpp>
  )
endif()

if(SB_BUILD_SB_LISTENER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/listener/CMakeLists.txt")
  add_subdirectory(tests/listener)
endif()

if(SB_BUILD_SBU_SBSQL_PARSER_SUPPORT AND NOT TARGET sbu_sbsql_parser_support)
  add_subdirectory(src/udr/sbu_sbsql_parser_support)
endif()

if(SB_BUILD_SBSQL_PARSER_WORKER_TESTS AND NOT TARGET sbu_sbsql_parser_support)
  add_subdirectory(src/udr/sbu_sbsql_parser_support)
endif()

if(SB_BUILD_SBSQL_PARSER_WORKER_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_parser_worker/CMakeLists.txt")
  add_subdirectory(tests/sbsql_parser_worker)
endif()

if(SB_BUILD_FIREBIRD_PARSER_WORKER OR SB_BUILD_FIREBIRD_PARSER_WORKER_TESTS OR SB_BUILD_SBU_FIREBIRD_PARSER_SUPPORT)
  if(NOT TARGET sbl_firebird_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/firebird)
  endif()
endif()

if(SB_BUILD_MYSQL_PARSER_WORKER OR SB_BUILD_MYSQL_PARSER_WORKER_TESTS OR SB_BUILD_SBU_MYSQL_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_mysql_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/mysql)
  endif()
endif()

if(SB_BUILD_POSTGRESQL_PARSER_WORKER OR SB_BUILD_POSTGRESQL_PARSER_WORKER_TESTS OR SB_BUILD_SBU_POSTGRESQL_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_postgresql_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/postgresql)
  endif()
endif()

if(SB_BUILD_SQLITE_PARSER_WORKER OR SB_BUILD_SQLITE_PARSER_WORKER_TESTS OR SB_BUILD_SBU_SQLITE_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_sqlite_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/sqlite)
  endif()
endif()

if(SB_BUILD_MARIADB_PARSER_WORKER OR SB_BUILD_MARIADB_PARSER_WORKER_TESTS OR SB_BUILD_SBU_MARIADB_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_mariadb_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/mariadb)
  endif()
endif()

if(SB_BUILD_DUCKDB_PARSER_WORKER OR SB_BUILD_DUCKDB_PARSER_WORKER_TESTS OR SB_BUILD_SBU_DUCKDB_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_duckdb_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/duckdb)
  endif()
endif()

if(SB_BUILD_CLICKHOUSE_PARSER_WORKER OR SB_BUILD_CLICKHOUSE_PARSER_WORKER_TESTS OR SB_BUILD_SBU_CLICKHOUSE_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_clickhouse_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/clickhouse)
  endif()
endif()

if(SB_BUILD_TIDB_PARSER_WORKER OR SB_BUILD_TIDB_PARSER_WORKER_TESTS OR SB_BUILD_SBU_TIDB_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_tidb_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/tidb)
  endif()
endif()

if(SB_BUILD_VITESS_PARSER_WORKER OR SB_BUILD_VITESS_PARSER_WORKER_TESTS OR SB_BUILD_SBU_VITESS_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_vitess_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/vitess)
  endif()
endif()

if(SB_BUILD_COCKROACHDB_PARSER_WORKER OR SB_BUILD_COCKROACHDB_PARSER_WORKER_TESTS OR SB_BUILD_SBU_COCKROACHDB_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_cockroachdb_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/cockroachdb)
  endif()
endif()

if(SB_BUILD_YUGABYTEDB_PARSER_WORKER OR SB_BUILD_YUGABYTEDB_PARSER_WORKER_TESTS OR SB_BUILD_SBU_YUGABYTEDB_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_yugabytedb_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/yugabytedb)
  endif()
endif()

if(SB_BUILD_CASSANDRA_PARSER_WORKER OR SB_BUILD_CASSANDRA_PARSER_WORKER_TESTS OR SB_BUILD_SBU_CASSANDRA_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_cassandra_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/cassandra)
  endif()
endif()

if(SB_BUILD_MONGODB_PARSER_WORKER OR SB_BUILD_MONGODB_PARSER_WORKER_TESTS OR SB_BUILD_SBU_MONGODB_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_mongodb_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/mongodb)
  endif()
endif()

if(SB_BUILD_REDIS_PARSER_WORKER OR SB_BUILD_REDIS_PARSER_WORKER_TESTS OR SB_BUILD_SBU_REDIS_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_redis_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/redis)
  endif()
endif()

if(SB_BUILD_OPENSEARCH_SQL_PPL_PARSER_WORKER OR SB_BUILD_OPENSEARCH_SQL_PPL_PARSER_WORKER_TESTS OR SB_BUILD_SBU_OPENSEARCH_SQL_PPL_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_opensearch_sql_ppl_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/opensearch_sql_ppl)
  endif()
endif()

if(SB_BUILD_OPENSEARCH_PARSER_WORKER OR SB_BUILD_OPENSEARCH_PARSER_WORKER_TESTS OR SB_BUILD_SBU_OPENSEARCH_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_opensearch_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/opensearch)
  endif()
endif()

if(SB_BUILD_NEO4J_PARSER_WORKER OR SB_BUILD_NEO4J_PARSER_WORKER_TESTS OR SB_BUILD_SBU_NEO4J_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_neo4j_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/neo4j)
  endif()
endif()

if(SB_BUILD_INFLUXDB_PARSER_WORKER OR SB_BUILD_INFLUXDB_PARSER_WORKER_TESTS OR SB_BUILD_SBU_INFLUXDB_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_influxdb_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/influxdb)
  endif()
endif()

if(SB_BUILD_MILVUS_PARSER_WORKER OR SB_BUILD_MILVUS_PARSER_WORKER_TESTS OR SB_BUILD_SBU_MILVUS_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_milvus_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/milvus)
  endif()
endif()

if(SB_BUILD_DOLT_PARSER_WORKER OR SB_BUILD_DOLT_PARSER_WORKER_TESTS OR SB_BUILD_SBU_DOLT_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_dolt_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/dolt)
  endif()
endif()

if(SB_BUILD_APACHE_IGNITE_PARSER_WORKER OR SB_BUILD_APACHE_IGNITE_PARSER_WORKER_TESTS OR SB_BUILD_SBU_APACHE_IGNITE_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_apache_ignite_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/apache_ignite)
  endif()
endif()

if(SB_BUILD_TIKV_PARSER_WORKER OR SB_BUILD_TIKV_PARSER_WORKER_TESTS OR SB_BUILD_SBU_TIKV_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_tikv_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/tikv)
  endif()
endif()

if(SB_BUILD_FOUNDATIONDB_PARSER_WORKER OR SB_BUILD_FOUNDATIONDB_PARSER_WORKER_TESTS OR SB_BUILD_SBU_FOUNDATIONDB_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_foundationdb_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/foundationdb)
  endif()
endif()

if(SB_BUILD_IMMUDB_PARSER_WORKER OR SB_BUILD_IMMUDB_PARSER_WORKER_TESTS OR SB_BUILD_SBU_IMMUDB_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_immudb_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/immudb)
  endif()
endif()

if(SB_BUILD_XTDB_PARSER_WORKER OR SB_BUILD_XTDB_PARSER_WORKER_TESTS OR SB_BUILD_SBU_XTDB_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS)
  if(NOT TARGET sbl_xtdb_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/xtdb)
  endif()
endif()

if(SB_BUILD_SBU_FIREBIRD_PARSER_SUPPORT AND NOT TARGET sbu_firebird_parser_support)
  add_subdirectory(src/udr/sbu_firebird_parser_support)
endif()

if((SB_BUILD_SBU_MYSQL_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_mysql_parser_support)
  add_subdirectory(src/udr/sbu_mysql_parser_support)
endif()

if((SB_BUILD_SBU_POSTGRESQL_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_postgresql_parser_support)
  add_subdirectory(src/udr/sbu_postgresql_parser_support)
endif()

if((SB_BUILD_SBU_SQLITE_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_sqlite_parser_support)
  add_subdirectory(src/udr/sbu_sqlite_parser_support)
endif()

if((SB_BUILD_SBU_MARIADB_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_mariadb_parser_support)
  add_subdirectory(src/udr/sbu_mariadb_parser_support)
endif()

if((SB_BUILD_SBU_DUCKDB_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_duckdb_parser_support)
  add_subdirectory(src/udr/sbu_duckdb_parser_support)
endif()

if((SB_BUILD_SBU_CLICKHOUSE_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_clickhouse_parser_support)
  add_subdirectory(src/udr/sbu_clickhouse_parser_support)
endif()

if((SB_BUILD_SBU_TIDB_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_tidb_parser_support)
  add_subdirectory(src/udr/sbu_tidb_parser_support)
endif()

if((SB_BUILD_SBU_VITESS_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_vitess_parser_support)
  add_subdirectory(src/udr/sbu_vitess_parser_support)
endif()

if((SB_BUILD_SBU_COCKROACHDB_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_cockroachdb_parser_support)
  add_subdirectory(src/udr/sbu_cockroachdb_parser_support)
endif()

if((SB_BUILD_SBU_YUGABYTEDB_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_yugabytedb_parser_support)
  add_subdirectory(src/udr/sbu_yugabytedb_parser_support)
endif()

if((SB_BUILD_SBU_CASSANDRA_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_cassandra_parser_support)
  add_subdirectory(src/udr/sbu_cassandra_parser_support)
endif()

if((SB_BUILD_SBU_MONGODB_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_mongodb_parser_support)
  add_subdirectory(src/udr/sbu_mongodb_parser_support)
endif()

if((SB_BUILD_SBU_REDIS_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_redis_parser_support)
  add_subdirectory(src/udr/sbu_redis_parser_support)
endif()

if((SB_BUILD_SBU_OPENSEARCH_SQL_PPL_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_opensearch_sql_ppl_parser_support)
  add_subdirectory(src/udr/sbu_opensearch_sql_ppl_parser_support)
endif()

if((SB_BUILD_SBU_OPENSEARCH_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_opensearch_parser_support)
  add_subdirectory(src/udr/sbu_opensearch_parser_support)
endif()

if((SB_BUILD_SBU_NEO4J_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_neo4j_parser_support)
  add_subdirectory(src/udr/sbu_neo4j_parser_support)
endif()

if((SB_BUILD_SBU_INFLUXDB_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_influxdb_parser_support)
  add_subdirectory(src/udr/sbu_influxdb_parser_support)
endif()

if((SB_BUILD_SBU_MILVUS_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_milvus_parser_support)
  add_subdirectory(src/udr/sbu_milvus_parser_support)
endif()

if((SB_BUILD_SBU_DOLT_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_dolt_parser_support)
  add_subdirectory(src/udr/sbu_dolt_parser_support)
endif()

if((SB_BUILD_SBU_APACHE_IGNITE_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_apache_ignite_parser_support)
  add_subdirectory(src/udr/sbu_apache_ignite_parser_support)
endif()

if((SB_BUILD_SBU_TIKV_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_tikv_parser_support)
  add_subdirectory(src/udr/sbu_tikv_parser_support)
endif()

if((SB_BUILD_SBU_FOUNDATIONDB_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_foundationdb_parser_support)
  add_subdirectory(src/udr/sbu_foundationdb_parser_support)
endif()

if((SB_BUILD_SBU_IMMUDB_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_immudb_parser_support)
  add_subdirectory(src/udr/sbu_immudb_parser_support)
endif()

if((SB_BUILD_SBU_XTDB_PARSER_SUPPORT OR SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS) AND NOT TARGET sbu_xtdb_parser_support)
  add_subdirectory(src/udr/sbu_xtdb_parser_support)
endif()

if(SB_BUILD_FIREBIRD_PARSER_WORKER_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/firebird_parser_worker/CMakeLists.txt")
  add_subdirectory(tests/firebird_parser_worker)
endif()

if(SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/compatibility_sql_parser_first_tranche/CMakeLists.txt")
  if(NOT TARGET sbl_firebird_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/firebird)
  endif()
  if(NOT TARGET sbu_firebird_parser_support)
    add_subdirectory(src/udr/sbu_firebird_parser_support)
  endif()
  foreach(sb_compatibility_sql_parser_target
      sqlite mariadb duckdb clickhouse
      tidb vitess cockroachdb yugabytedb
      cassandra mongodb redis opensearch_sql_ppl
      opensearch neo4j influxdb milvus
      dolt apache_ignite tikv foundationdb
      immudb xtdb)
    if(NOT TARGET sbl_${sb_compatibility_sql_parser_target}_parser_pipeline)
      add_subdirectory(src/parsers/compatibility/${sb_compatibility_sql_parser_target})
    endif()
    if(NOT TARGET sbu_${sb_compatibility_sql_parser_target}_parser_support)
      add_subdirectory(src/udr/sbu_${sb_compatibility_sql_parser_target}_parser_support)
    endif()
  endforeach()
  add_subdirectory(tests/compatibility_sql_parser_first_tranche)
endif()

if(SB_BUILD_DATABASE_LIFECYCLE_TESTS)
  if(NOT TARGET sb_udr_runtime)
    add_subdirectory(src/udr/runtime)
  endif()
  if(NOT TARGET sbl_sbsql_parser_worker_core)
    add_subdirectory(src/parsers/sbsql_worker)
  endif()
  if(NOT TARGET sbu_sbsql_parser_support)
    add_subdirectory(src/udr/sbu_sbsql_parser_support)
  endif()
  if(NOT TARGET sbl_firebird_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/firebird)
  endif()
  if(NOT TARGET sbu_firebird_parser_support)
    add_subdirectory(src/udr/sbu_firebird_parser_support)
  endif()
  if(NOT TARGET sbl_mysql_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/mysql)
  endif()
  if(NOT TARGET sbu_mysql_parser_support)
    add_subdirectory(src/udr/sbu_mysql_parser_support)
  endif()
  if(NOT TARGET sbl_postgresql_parser_pipeline)
    add_subdirectory(src/parsers/compatibility/postgresql)
  endif()
  if(NOT TARGET sbu_postgresql_parser_support)
    add_subdirectory(src/udr/sbu_postgresql_parser_support)
  endif()
endif()

if(SB_BUILD_DATABASE_LIFECYCLE_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/database_lifecycle/CMakeLists.txt")
  add_subdirectory(tests/database_lifecycle)
endif()

if((SB_BUILD_AGENT_RUNTIME_PROBES OR SB_BUILD_DATABASE_LIFECYCLE_TESTS OR SB_BUILD_TESTS)
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/agents/CMakeLists.txt")
  add_subdirectory(tests/agents)
endif()

if((SB_BUILD_DATABASE_LIFECYCLE_TESTS OR SB_BUILD_TESTS)
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/commercial_readiness/CMakeLists.txt")
  add_subdirectory(tests/commercial_readiness)
endif()

option(SB_BUILD_CONSOLIDATED_ENTERPRISE_PROOF_GATE
  "Build the CEIC consolidated enterprise proof-control CTest gate"
  OFF)
if((SB_BUILD_CONSOLIDATED_ENTERPRISE_PROOF_GATE OR SB_BUILD_TESTS)
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/consolidated_enterprise/CMakeLists.txt")
  add_subdirectory(tests/consolidated_enterprise)
endif()

if(SB_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/performance/CMakeLists.txt")
  add_subdirectory(tests/performance)
endif()

if(SB_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency/CMakeLists.txt")
  add_subdirectory(tests/concurrency)
endif()

if(SB_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/optimizer/CMakeLists.txt")
  add_subdirectory(tests/optimizer)
endif()

option(SB_BUILD_DOCUMENTATION_BASELINE_GATES
  "Build generated documentation baseline CTest gates"
  OFF)
set(_sb_documentation_baseline_registry
  "${PROJECT_SOURCE_DIR}/../docs/execution-plans/end-user-documentation-baseline-generation/DOCUMENTATION_BOOK_REGISTRY.csv")
if(SB_BUILD_TESTS AND EXISTS "${_sb_documentation_baseline_registry}")
  set(SB_BUILD_DOCUMENTATION_BASELINE_GATES ON CACHE BOOL
    "Build generated documentation baseline CTest gates" FORCE)
endif()
if(SB_BUILD_DOCUMENTATION_BASELINE_GATES
   AND EXISTS "${_sb_documentation_baseline_registry}"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/documentation/generation/CMakeLists.txt")
  add_subdirectory(tests/documentation/generation)
endif()
unset(_sb_documentation_baseline_registry)

if(SB_BUILD_PUBLIC_STANDALONE_OUTPUT
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/drivers/tool/cli/CMakeLists.txt"
   AND NOT TARGET sb_isql)
  add_subdirectory(drivers/tool/cli "${CMAKE_BINARY_DIR}/drivers_public_cli")
endif()

option(SB_BUILD_DRIVER_GATES "Build current-repo driver/adaptor/tool CTest gates" OFF)
if(SB_BUILD_TESTS)
  set(SB_BUILD_DRIVER_GATES ON CACHE BOOL
    "Build current-repo driver/adaptor/tool CTest gates" FORCE)
endif()
if(SB_BUILD_DRIVER_GATES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/drivers/CMakeLists.txt")
  add_subdirectory(drivers)
endif()

option(SB_BUILD_AI_GATES
  "Build ScratchBird AI current-repo integration CTest gates"
  OFF)
if(SB_BUILD_TESTS)
  set(SB_BUILD_AI_GATES ON CACHE BOOL
    "Build ScratchBird AI current-repo integration CTest gates" FORCE)
endif()
if(SB_BUILD_AI_GATES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ai/CMakeLists.txt")
  add_subdirectory(ai)
endif()

option(SB_BUILD_COMPAT_REGRESSION_GATES
  "Build compatibility compatibility profile and regression-import CTest gates"
  OFF)
if(SB_BUILD_TESTS)
  set(SB_BUILD_COMPAT_REGRESSION_GATES ON CACHE BOOL
    "Build compatibility compatibility profile and regression-import CTest gates" FORCE)
endif()
if(SB_BUILD_COMPAT_REGRESSION_GATES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/reference_regression/CMakeLists.txt")
  add_subdirectory(tests/reference_regression)
endif()

option(SB_BUILD_SBLR_SURFACE_GUARDRAIL_GATES
  "Build compatibility/SBLR surface public guardrail inventory CTest gates"
  OFF)
if(SB_BUILD_TESTS OR SB_BUILD_PUBLIC_RELEASE_CORRECTNESS)
  set(SB_BUILD_SBLR_SURFACE_GUARDRAIL_GATES ON CACHE BOOL
    "Build compatibility/SBLR surface public guardrail inventory CTest gates" FORCE)
endif()
if(SB_BUILD_SBLR_SURFACE_GUARDRAIL_GATES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/sblr_surface/CMakeLists.txt")
  add_subdirectory(tests/sblr_surface)
endif()

option(SB_BUILD_ENGINE_LISTENER_ENTERPRISE_GATES
  "Build engine/listener enterprise closure foundation CTest gates"
  OFF)
if(SB_BUILD_TESTS OR SB_BUILD_PUBLIC_RELEASE_CORRECTNESS)
  set(SB_BUILD_ENGINE_LISTENER_ENTERPRISE_GATES ON CACHE BOOL
    "Build engine/listener enterprise closure foundation CTest gates" FORCE)
endif()
if(SB_BUILD_ENGINE_LISTENER_ENTERPRISE_GATES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/engine_listener_enterprise/CMakeLists.txt")
  add_subdirectory(tests/engine_listener_enterprise)
endif()

if(SB_BUILD_ENGINE_LISTENER_ENTERPRISE_GATES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fault_injection/CMakeLists.txt")
  add_subdirectory(tests/fault_injection)
endif()

set(SB_ENABLE_MGA_TRANSACTION_REGRESSION_TESTS OFF)
if(SB_BUILD_MGA_TRANSACTION_REGRESSION_TESTS OR SB_BUILD_TESTS)
  set(SB_ENABLE_MGA_TRANSACTION_REGRESSION_TESTS ON)
endif()

if(SB_ENABLE_MGA_TRANSACTION_REGRESSION_TESTS)
  if(NOT TARGET sb_core_platform)
    add_subdirectory(src/core/platform)
  endif()
  if(NOT TARGET sb_core_uuid)
    add_subdirectory(src/core/uuid)
  endif()
  if(NOT TARGET sb_core_memory)
    add_subdirectory(src/core/memory)
  endif()
  if(NOT TARGET sb_core_metrics)
    add_subdirectory(src/core/metrics)
  endif()
  if(NOT TARGET sb_transaction_mga)
    add_subdirectory(src/transaction/mga)
  endif()
  if(NOT TARGET sb_core_datatypes)
    add_subdirectory(src/core/datatypes)
  endif()
  if(NOT TARGET sb_core_resources)
    add_subdirectory(src/core/resources)
  endif()
  if(NOT TARGET sb_storage_disk)
    add_subdirectory(src/storage/disk)
  endif()
  if(NOT TARGET sb_storage_page)
    add_subdirectory(src/storage/page)
  endif()
  if(NOT TARGET sb_core_catalog)
    add_subdirectory(src/core/catalog)
  endif()
  if(NOT TARGET sb_storage_database)
    add_subdirectory(src/storage/database)
  endif()
  if(NOT TARGET sb_engine_internal_api)
    add_subdirectory(src/engine/internal_api)
  endif()
  if(NOT TARGET sb_engine_api_transaction_semantics_probe)
    add_subdirectory(tools/sb_engine_api_transaction_semantics_probe)
  endif()
  if(NOT TARGET sb_single_node_transaction_probe)
    add_subdirectory(tools/sb_single_node_transaction_probe)
  endif()
  if(NOT TARGET sb_transaction_recovery_probe)
    add_subdirectory(tools/sb_transaction_recovery_probe)
  endif()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/mga_transaction_regression/CMakeLists.txt")
    add_subdirectory(tests/mga_transaction_regression)
  endif()
endif()

if((SB_BUILD_TESTS OR SB_BUILD_DATABASE_LIFECYCLE_TESTS) AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/engine_public_abi/CMakeLists.txt")
  add_subdirectory(tests/engine_public_abi)
endif()

if(SB_BUILD_TESTS)
  set(SB_BUILD_PUBLIC_P1_CLOUD_OPS_TESTS ON CACHE BOOL
    "Build public P1 protected-material/cloud-ops CTest gates" FORCE)
endif()
if(SB_BUILD_PUBLIC_P1_CLOUD_OPS_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/cloud_ops/CMakeLists.txt")
  add_subdirectory(tests/cloud_ops)
endif()

if(SB_BUILD_PUBLIC_RELEASE_CORRECTNESS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/release/CMakeLists.txt")
  # PUBLIC_RELEASE_PLATFORM_MATRIX
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_subdirectory(tests/release)
endif()

sb_public_brand_target(sb_engine "SBcore_static" "ScratchBird Engine Static Library" "runtime")
sb_public_brand_target(sb_engine_shared "SBcore" "ScratchBird Engine" "runtime")
sb_public_brand_target(sb_listener "SBgate" "ScratchBird Listener" "runtime")
sb_public_brand_target(sb_server "SBsrv" "ScratchBird IPC Server" "runtime")
sb_public_brand_target(sbmn_manager "SBmgr" "ScratchBird Single Node Manager" "runtime")
sb_public_brand_target(sbp_sbsql "SBParser" "ScratchBird Core Parser" "runtime")
sb_public_brand_target(sbl_sbsql_parser_pipeline "SBParser_pipeline" "ScratchBird Core Parser Pipeline" "runtime")
sb_public_brand_target(sbl_sbsql_parser_worker_core "SBParser_core" "ScratchBird Core Parser Worker Core" "runtime")
sb_public_brand_target(sbu_sbsql_parser_support "SBParser_udr" "ScratchBird Core Parser UDR" "runtime")

sb_public_brand_target(sb_isql "SBsql" "ScratchBird SQL" "cli")
sb_public_brand_target(sb_admin "SBadm" "ScratchBird Administrator" "cli")
sb_public_brand_target(sb_backup "SBbak" "ScratchBird Backup Manager" "cli")
sb_public_brand_target(sb_security "SBsec" "ScratchBird Security" "cli")
sb_public_brand_target(sb_verify "SBdoc" "ScratchBird Doctor" "cli")
sb_public_brand_target(sbdriver_conformance "SBcop" "ScratchBird Conformance Officer" "cli")

sb_public_write_branding_manifest("${CMAKE_BINARY_DIR}/public_branding_manifest.tsv")
if(SB_BUILD_PUBLIC_STANDALONE_OUTPUT)
  sb_public_configure_output_stage("${CMAKE_CURRENT_SOURCE_DIR}" "${Python3_EXECUTABLE}")
endif()
add_test(
  NAME public_branding_gate
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/../tools/release/public_branding_gate.py"
          "--manifest=${CMAKE_BINARY_DIR}/public_branding_manifest.tsv"
)
set_tests_properties(public_branding_gate PROPERTIES
  LABELS "public_release;branding"
)
if(SB_BUILD_PUBLIC_STANDALONE_OUTPUT)
  add_test(
    NAME public_output_stage_gate
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target scratchbird_public_output_stage
  )
  set_tests_properties(public_output_stage_gate PROPERTIES
    LABELS "public_release;standalone_output"
  )
endif()
