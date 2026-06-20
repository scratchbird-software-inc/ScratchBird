# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(SB_PRIVATE_REPO_ROOT "${PROJECT_SOURCE_DIR}/..")

set(SB_DATABASE_LIFECYCLE_POLICY_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/default_policy_conformance.py"
)

set(SB_DATABASE_LIFECYCLE_FIXTURE_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/full_database_lifecycle_closure"
)

set(SB_DATABASE_LIFECYCLE_RECONCILIATION_AUDIT
  "${SB_PRIVATE_REPO_ROOT}/project/tools/database_lifecycle/lifecycle_reconciliation_audit.py"
)

set(SB_DATABASE_LIFECYCLE_TRACEABILITY_AUDIT
  "${SB_PRIVATE_REPO_ROOT}/project/tools/database_lifecycle/lifecycle_traceability_audit.py"
)

set(SB_DATABASE_LIFECYCLE_FINAL_ZERO_OPEN_AUDIT
  "${SB_DATABASE_LIFECYCLE_FIXTURE_ROOT}/artifacts/final_database_lifecycle_zero_open_audit.py"
)

set(SB_DATABASE_LIFECYCLE_PLATFORM_TOOLCHAIN_METADATA
  "${CMAKE_CURRENT_BINARY_DIR}/platform_toolchain_configured_build.txt"
)
file(WRITE "${SB_DATABASE_LIFECYCLE_PLATFORM_TOOLCHAIN_METADATA}"
  "CMAKE_VERSION=${CMAKE_VERSION}\n"
  "CMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}\n"
  "CMAKE_CXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}\n"
  "CMAKE_CXX_COMPILER_VERSION=${CMAKE_CXX_COMPILER_VERSION}\n"
  "CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}\n"
  "CMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}\n"
  "CMAKE_HOST_SYSTEM_NAME=${CMAKE_HOST_SYSTEM_NAME}\n"
  "CMAKE_HOST_SYSTEM_PROCESSOR=${CMAKE_HOST_SYSTEM_PROCESSOR}\n"
  "CMAKE_CXX_STANDARD=${CMAKE_CXX_STANDARD}\n"
  "SB_NONCLUSTER_ENGINE_PROFILE=${SB_NONCLUSTER_ENGINE_PROFILE}\n"
  "SB_ENABLE_CLUSTER_PROVIDER=${SB_ENABLE_CLUSTER_PROVIDER}\n"
  "SB_CLUSTER_PROVIDER_STUB=${SB_CLUSTER_PROVIDER_STUB}\n"
  "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY=${SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY}\n"
)

add_test(
  NAME database_lifecycle_platform_toolchain_matrix_conformance
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/platform_toolchain_matrix_static.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --build-root "${CMAKE_BINARY_DIR}"
          --build-metadata "${SB_DATABASE_LIFECYCLE_PLATFORM_TOOLCHAIN_METADATA}"
)
set_tests_properties(database_lifecycle_platform_toolchain_matrix_conformance PROPERTIES
  LABELS "database_lifecycle_platform_toolchain_matrix_conformance;CBQ-036;CBQ_GATE_PLATFORM_TOOLCHAIN_MATRIX;platform_toolchain_matrix;database_lifecycle"
)

# DPC_CROSS_BUILD_MATRIX: standalone DPC-069 evidence for required build modes.
set(SB_DPC_CROSS_BUILD_MATRIX_METADATA
  "${CMAKE_CURRENT_BINARY_DIR}/dpc_cross_build_matrix_configured_build.txt"
)
file(WRITE "${SB_DPC_CROSS_BUILD_MATRIX_METADATA}"
  "CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}\n"
  "SB_ENABLE_CLUSTER_PROVIDER=${SB_ENABLE_CLUSTER_PROVIDER}\n"
  "SB_CLUSTER_PROVIDER_STUB=${SB_CLUSTER_PROVIDER_STUB}\n"
  "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY=${SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY}\n"
  "SB_CLUSTER_PROVIDER_EXTERNAL_INCLUDE_DIR=${SB_CLUSTER_PROVIDER_EXTERNAL_INCLUDE_DIR}\n"
  "SCRATCHBIRD_ENABLE_DEBUG_LOGS=${SCRATCHBIRD_ENABLE_DEBUG_LOGS}\n"
  "SCRATCHBIRD_ENABLE_HOTPATH_TRACE=${SCRATCHBIRD_ENABLE_HOTPATH_TRACE}\n"
  "SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE=${SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE}\n"
  "SCRATCHBIRD_ENABLE_PREPARED_TRACE=${SCRATCHBIRD_ENABLE_PREPARED_TRACE}\n"
)

set(SB_DPC_CROSS_BUILD_MATRIX_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/dpc_cross_build_matrix_gate.py"
)

add_custom_target(dpc_cross_build_matrix_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_DPC_CROSS_BUILD_MATRIX_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --build-root "${CMAKE_BINARY_DIR}"
          --build-metadata "${SB_DPC_CROSS_BUILD_MATRIX_METADATA}"
  COMMENT "Validating DPC-069 cross-build matrix source and CTest surfaces"
)

add_test(
  NAME dpc_cross_build_matrix_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_DPC_CROSS_BUILD_MATRIX_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --build-root "${CMAKE_BINARY_DIR}"
          --build-metadata "${SB_DPC_CROSS_BUILD_MATRIX_METADATA}"
)
set_tests_properties(dpc_cross_build_matrix_gate PROPERTIES
  LABELS "dpc_cross_build_matrix_gate;DPC-069;DPC_P6_CROSS_BUILD_READY;DPC_AUDIT_CROSS_BUILD;DPC_DEP_CROSS_BUILD;cross_build_matrix;database_lifecycle;mga_transaction_regression"
)

set(SB_DPC_OPERATOR_UI_DOCUMENTATION_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/dpc_operator_ui_documentation_gate.py"
)

add_custom_target(dpc_operator_ui_documentation_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_DPC_OPERATOR_UI_DOCUMENTATION_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
  COMMENT "Validating DPC-071 operator and UI product documentation"
)

add_test(
  NAME dpc_operator_ui_documentation_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_DPC_OPERATOR_UI_DOCUMENTATION_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(dpc_operator_ui_documentation_gate PROPERTIES
  LABELS "dpc_operator_ui_documentation_gate;DPC-071;DPC_OPERATOR_UI_DOCUMENTATION_GATE;DPC_P6_OPERATOR_UI_DOC_READY;DPC_AUDIT_OPERATOR_UI_DOC;DPC_DEP_OPERATOR_UI_DOCS;operator_ui_docs;documentation;management_observability;support_bundle;message_vectors;database_lifecycle;mga_transaction_regression"
)

set(SB_DATABASE_LIFECYCLE_EXAMPLES_SMOKE_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/examples_smoke_scripts_gate.py"
)

if(NOT SB_BUILD_DRIVER_GATES)
  set(SB_DATABASE_LIFECYCLE_DRIVER_BUILD_ROOT
    "${CMAKE_BINARY_DIR}/drivers"
  )
  file(MAKE_DIRECTORY "${SB_DATABASE_LIFECYCLE_DRIVER_BUILD_ROOT}")

  add_test(
    NAME driver_package_manifest_gate
    COMMAND
      "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/drivers/scripts/driver_execution_plan_gate.py"
      "--repo-root" "${SB_PRIVATE_REPO_ROOT}"
      "--project-root" "${PROJECT_SOURCE_DIR}"
      "--build-root" "${SB_DATABASE_LIFECYCLE_DRIVER_BUILD_ROOT}"
      "--fixture-root" "${PROJECT_SOURCE_DIR}/drivers/fixtures/driver_adaptor_current_repo_integration"
      "package-manifest"
  )
  set_tests_properties(driver_package_manifest_gate PROPERTIES
    LABELS "driver;driver_lane_ctest_gate;driver_package_manifest_gate;database_lifecycle_examples_smoke_dependency"
    SKIP_RETURN_CODE 77
  )

  foreach(component IN ITEMS python cpp)
    add_test(
      NAME "driver_${component}_gate"
      COMMAND
        "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/drivers/scripts/driver_component_runner.py"
        "--repo-root" "${SB_PRIVATE_REPO_ROOT}"
        "--project-root" "${PROJECT_SOURCE_DIR}"
        "--build-root" "${SB_DATABASE_LIFECYCLE_DRIVER_BUILD_ROOT}"
        "--component" "driver:${component}"
        "--allow-toolchain-waivers"
    )
    set_tests_properties("driver_${component}_gate" PROPERTIES
      LABELS "driver;driver_lane_ctest_gate;driver_tool_adaptor_gate;driver_${component}_gate;database_lifecycle_examples_smoke_dependency"
      SKIP_RETURN_CODE 77
    )
  endforeach()
endif()

add_custom_target(database_lifecycle_examples_smoke_scripts_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_DATABASE_LIFECYCLE_EXAMPLES_SMOKE_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --build-root "${CMAKE_BINARY_DIR}"
  COMMENT "Validating core beta QA examples smoke scripts"
)

add_test(
  NAME database_lifecycle_examples_smoke_scripts_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_DATABASE_LIFECYCLE_EXAMPLES_SMOKE_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --build-root "${CMAKE_BINARY_DIR}"
)
set_tests_properties(database_lifecycle_examples_smoke_scripts_gate PROPERTIES
  LABELS "database_lifecycle_examples_smoke_scripts_gate;CBQ-034;CBQ_GATE_EXAMPLES_SMOKE_SCRIPTS;example;smoke;documentation;sample;cli;embedded;driver;database_lifecycle"
)

include("${CMAKE_CURRENT_SOURCE_DIR}/ReleaseProvenanceSbomLicenseGate.cmake")
sb_add_database_lifecycle_release_provenance_sbom_license_gate()

set(SB_DATABASE_LIFECYCLE_BETA_PACKAGE_SMOKE_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/beta_package_smoke_gate.py"
)
set(SB_DATABASE_LIFECYCLE_BETA_PACKAGE_SMOKE_DEPENDS sb_engine_shared)
foreach(sb_package_smoke_dependency IN ITEMS
    sb_server
    sb_ipc_tester
    sb_listener
    sbp_sbsql
    sbu_sbsql_parser_support)
  if(TARGET ${sb_package_smoke_dependency})
    list(APPEND SB_DATABASE_LIFECYCLE_BETA_PACKAGE_SMOKE_DEPENDS
      ${sb_package_smoke_dependency})
  endif()
endforeach()

add_custom_target(database_lifecycle_beta_package_smoke_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_DATABASE_LIFECYCLE_BETA_PACKAGE_SMOKE_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --build-root "${CMAKE_BINARY_DIR}"
  DEPENDS ${SB_DATABASE_LIFECYCLE_BETA_PACKAGE_SMOKE_DEPENDS}
  COMMENT "Validating beta package smoke from staged install artifacts"
)

add_test(
  NAME database_lifecycle_beta_package_smoke_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_DATABASE_LIFECYCLE_BETA_PACKAGE_SMOKE_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --build-root "${CMAKE_BINARY_DIR}"
)
set_tests_properties(database_lifecycle_beta_package_smoke_gate PROPERTIES
  LABELS "database_lifecycle_beta_package_smoke_gate;CBQ-016;CBQ_GATE_BETA_PACKAGE_SMOKE;package;packaging;install;smoke;artifact;release;driver;database_lifecycle"
)

set(SB_DPC_FAILURE_INJECTION_CRASH_MATRIX_FIXTURE
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dpc_failure_injection_crash_matrix.json"
)

add_test(
  NAME dpc_failure_injection_crash_matrix_gate
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/dpc_failure_injection_crash_matrix_gate.py"
          --fixture "${SB_DPC_FAILURE_INJECTION_CRASH_MATRIX_FIXTURE}"
)
set_tests_properties(dpc_failure_injection_crash_matrix_gate PROPERTIES
  LABELS "dpc_failure_injection_crash_matrix_gate;DPC-008;DPC_TEST_FAILURE_INJECTION;failure_injection;crash_recovery;mga_transaction_regression;database_lifecycle;database_lifecycle_fault_injection"
)

if(NOT TARGET sb_storage_filespace)
  add_subdirectory("${PROJECT_SOURCE_DIR}/src/storage/filespace" "${CMAKE_CURRENT_BINARY_DIR}/storage_filespace")
endif()

if(NOT TARGET sb_core_bulk_load)
  add_subdirectory("${PROJECT_SOURCE_DIR}/src/core/bulk_load" "${CMAKE_CURRENT_BINARY_DIR}/core_bulk_load")
endif()

if(NOT TARGET sb_core_index)
  add_subdirectory("${PROJECT_SOURCE_DIR}/src/core/index" "${CMAKE_CURRENT_BINARY_DIR}/core_index")
endif()

if(NOT TARGET sb_engine_executor)
  add_subdirectory("${PROJECT_SOURCE_DIR}/src/engine/executor" "${CMAKE_CURRENT_BINARY_DIR}/engine_executor")
endif()

add_executable(index_bulk_publish_recovery_gate
  index_bulk_publish_recovery_gate.cpp
)
target_compile_features(index_bulk_publish_recovery_gate PRIVATE cxx_std_20)
target_link_libraries(index_bulk_publish_recovery_gate
  PRIVATE
    sb_core_index
    sb_storage_page
    sb_core_uuid
)

add_test(
  NAME index_bulk_publish_recovery_gate
  COMMAND index_bulk_publish_recovery_gate
)
set_tests_properties(index_bulk_publish_recovery_gate PROPERTIES
  LABELS "index_bulk_publish_recovery_gate;IRC-042;IPAR-P4-05;index_runtime_correctness;bulk_publish_recovery;crash_recovery;database_lifecycle;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(dml_index_write_path_gate
  dml_index_write_path_gate.cpp
)
target_compile_features(dml_index_write_path_gate PRIVATE cxx_std_20)
target_link_libraries(dml_index_write_path_gate
  PRIVATE
    sb_engine_internal_api
    sb_core_index
    sb_storage_page
    sb_core_uuid
)

add_test(
  NAME dml_index_write_path_gate
  COMMAND dml_index_write_path_gate
)
set_tests_properties(dml_index_write_path_gate PROPERTIES
  LABELS "dml_index_write_path_gate;IRC-050;IPAR-P4-01;physical_unique_index_probe;index_cursor_probe;index_runtime_correctness;dml_index_write_path;database_lifecycle;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(dml_hot_delta_ledger_gate
  dml_hot_delta_ledger_gate.cpp
)
target_compile_features(dml_hot_delta_ledger_gate PRIVATE cxx_std_20)
target_link_libraries(dml_hot_delta_ledger_gate
  PRIVATE
    sb_engine_internal_api
    sb_core_index
    sb_storage_page
    sb_core_uuid
)

add_test(
  NAME dml_hot_delta_ledger_gate
  COMMAND dml_hot_delta_ledger_gate
)
set_tests_properties(dml_hot_delta_ledger_gate PROPERTIES
  LABELS "dml_hot_delta_ledger_gate;IRC-051;index_runtime_correctness;dml_hot_delta_ledger;database_lifecycle;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(dml_conflict_merge_locator_stream_gate
  dml_conflict_merge_locator_stream_gate.cpp
)
target_compile_features(dml_conflict_merge_locator_stream_gate PRIVATE cxx_std_20)
target_link_libraries(dml_conflict_merge_locator_stream_gate
  PRIVATE
    sb_engine_internal_api
    sb_core_index
    sb_storage_page
    sb_core_uuid
)

add_test(
  NAME dml_conflict_merge_locator_stream_gate
  COMMAND dml_conflict_merge_locator_stream_gate
)
set_tests_properties(dml_conflict_merge_locator_stream_gate PROPERTIES
  LABELS "dml_conflict_merge_locator_stream_gate;IRC-052;index_runtime_correctness;dml_row_locator_stream;database_lifecycle;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(dpc_executor_batch_primitive_gate
  dpc_executor_batch_primitive_gate.cpp
)
target_compile_features(dpc_executor_batch_primitive_gate PRIVATE cxx_std_20)
target_link_libraries(dpc_executor_batch_primitive_gate
  PRIVATE
    sb_engine_executor
)

add_test(
  NAME dpc_executor_batch_primitive_gate
  COMMAND dpc_executor_batch_primitive_gate
)
set_tests_properties(dpc_executor_batch_primitive_gate PROPERTIES
  LABELS "dpc_executor_batch_primitive_gate;DPC-050;DPC_EXECUTOR_BATCH;DPC_TEST_EXECUTOR_BATCH;executor_batch;database_lifecycle;mga_transaction_regression"
)

add_executable(dpc_executor_batch_scan_filter_projection_aggregate_gate
  dpc_executor_batch_scan_filter_projection_aggregate_gate.cpp
)
target_compile_features(dpc_executor_batch_scan_filter_projection_aggregate_gate
  PRIVATE cxx_std_20)
target_link_libraries(dpc_executor_batch_scan_filter_projection_aggregate_gate
  PRIVATE
    sb_engine_executor
)

add_test(
  NAME dpc_executor_batch_scan_filter_projection_aggregate_gate
  COMMAND dpc_executor_batch_scan_filter_projection_aggregate_gate
)
set_tests_properties(dpc_executor_batch_scan_filter_projection_aggregate_gate PROPERTIES
  LABELS "dpc_executor_batch_scan_filter_projection_aggregate_gate;DPC-051;DPC_EXECUTOR_BATCH_RELATIONAL;DPC_TEST_EXECUTOR_BATCH;executor_batch;scan;filter;projection;aggregate;database_lifecycle;mga_transaction_regression"
)

add_executable(dpc_executor_batch_join_dml_gate
  dpc_executor_batch_join_dml_gate.cpp
)
target_compile_features(dpc_executor_batch_join_dml_gate PRIVATE cxx_std_20)
target_link_libraries(dpc_executor_batch_join_dml_gate
  PRIVATE
    sb_engine_executor
)

add_test(
  NAME dpc_executor_batch_join_dml_gate
  COMMAND dpc_executor_batch_join_dml_gate
)
set_tests_properties(dpc_executor_batch_join_dml_gate PROPERTIES
  LABELS "dpc_executor_batch_join_dml_gate;DPC-052;DPC_EXECUTOR_BATCH_JOIN_DML;DPC_TEST_EXECUTOR_BATCH;executor_batch;join;dml;database_lifecycle;mga_transaction_regression"
)

add_executable(dpc_executor_batch_failure_fallback_gate
  dpc_executor_batch_failure_fallback_gate.cpp
)
target_compile_features(dpc_executor_batch_failure_fallback_gate PRIVATE cxx_std_20)
target_link_libraries(dpc_executor_batch_failure_fallback_gate
  PRIVATE
    sb_engine_executor
)

add_test(
  NAME dpc_executor_batch_failure_fallback_gate
  COMMAND dpc_executor_batch_failure_fallback_gate
)
set_tests_properties(dpc_executor_batch_failure_fallback_gate PROPERTIES
  LABELS "dpc_executor_batch_failure_fallback_gate;DPC-053;DPC_EXECUTOR_BATCH_FAILURE_POLICY;DPC_TEST_EXECUTOR_BATCH;executor_batch;failure_fallback;database_lifecycle;mga_transaction_regression"
)

add_executable(dpc_executor_batch_benchmark_gate
  dpc_executor_batch_benchmark_gate.cpp
)
target_compile_features(dpc_executor_batch_benchmark_gate PRIVATE cxx_std_20)
target_link_libraries(dpc_executor_batch_benchmark_gate
  PRIVATE
    sb_engine_executor
)

add_test(
  NAME dpc_executor_batch_benchmark_gate
  COMMAND dpc_executor_batch_benchmark_gate
)
set_tests_properties(dpc_executor_batch_benchmark_gate PROPERTIES
  LABELS "dpc_executor_batch_benchmark_gate;DPC-054;DPC_EXECUTOR_BATCH_BENCHMARK_GATE;DPC_TEST_EXECUTOR_BATCH;executor_batch;benchmark;database_lifecycle;mga_transaction_regression"
)

add_executable(cdp_copy_batch_metric_hint_gate
  cdp_copy_batch_metric_hint_gate.cpp
)
target_compile_features(cdp_copy_batch_metric_hint_gate PRIVATE cxx_std_23)
target_link_libraries(cdp_copy_batch_metric_hint_gate
  PRIVATE
    sb_core_bulk_load
    sb_core_platform
    sb_core_uuid
)

add_test(
  NAME cdp_copy_batch_metric_hint_gate
  COMMAND cdp_copy_batch_metric_hint_gate
)
set_tests_properties(cdp_copy_batch_metric_hint_gate PROPERTIES
  LABELS "cdp_copy_batch_metric_hint_gate;CDP-010;CDP-013;database_lifecycle_storage;mga_transaction_regression"
)

if(TARGET sb_engine_internal_api AND TARGET sb_storage_database)
  if(NOT TARGET sb_engine_sblr)
    add_subdirectory("${PROJECT_SOURCE_DIR}/src/engine/sblr" "${CMAKE_CURRENT_BINARY_DIR}/engine_sblr_cdp040")
  endif()

  add_executable(cdp_copy_append_batching_gate
    cdp_copy_append_batching_gate.cpp
  )
  target_compile_features(cdp_copy_append_batching_gate PRIVATE cxx_std_20)
  target_link_libraries(cdp_copy_append_batching_gate
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sb_engine_sblr
      sb_storage_database
  )

  add_test(
    NAME cdp_copy_append_batching_gate
    COMMAND cdp_copy_append_batching_gate
  )
  set_tests_properties(cdp_copy_append_batching_gate PROPERTIES
    LABELS "cdp_copy_append_batching_gate;CDP-011;PID-007;PID-010;IPAR-P5-02;CDP_P1_COPY_BATCHED;database_lifecycle_storage;sblr_opcode_registry_conformance;mga_transaction_regression"
  )

  add_executable(cdp_native_bulk_ingest_api_gate
    cdp_native_bulk_ingest_api_gate.cpp
  )
  target_compile_features(cdp_native_bulk_ingest_api_gate PRIVATE cxx_std_20)
  target_link_libraries(cdp_native_bulk_ingest_api_gate
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sb_engine_sblr
      sb_server_core
      sb_storage_database
  )

  add_test(
    NAME cdp_native_bulk_ingest_api_gate
    COMMAND cdp_native_bulk_ingest_api_gate
  )
  set_tests_properties(cdp_native_bulk_ingest_api_gate PROPERTIES
    LABELS "cdp_native_bulk_ingest_api_gate;CDP-040;CDP_P4_NATIVE_INGEST_READY;database_lifecycle_storage;sblr_opcode_registry_conformance;mga_transaction_regression"
  )

  add_executable(cdp_page_filespace_demand_gate
    cdp_page_filespace_demand_gate.cpp
  )
  target_compile_features(cdp_page_filespace_demand_gate PRIVATE cxx_std_20)
  target_link_libraries(cdp_page_filespace_demand_gate
    PRIVATE
      sb_core_bulk_load
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sb_engine_sblr
      sb_storage_database
  )

  add_test(
    NAME cdp_page_filespace_demand_gate
    COMMAND cdp_page_filespace_demand_gate
  )
  set_tests_properties(cdp_page_filespace_demand_gate PROPERTIES
    LABELS "cdp_page_filespace_demand_gate;CDP-013;CDP_P1_PAGE_PREALLOC_READY;database_lifecycle_storage;mga_transaction_regression"
  )

  add_executable(ipar_page_size_preallocation_gate
    ipar_page_size_preallocation_gate.cpp
  )
  target_compile_features(ipar_page_size_preallocation_gate PRIVATE cxx_std_23)
  target_link_libraries(ipar_page_size_preallocation_gate
    PRIVATE
      sb_core_bulk_load
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sb_engine_sblr
      sb_storage_database
  )

  add_test(
    NAME ipar_page_size_preallocation_gate
    COMMAND ipar_page_size_preallocation_gate
  )
  set_tests_properties(ipar_page_size_preallocation_gate PROPERTIES
    LABELS "ipar_page_size_preallocation_gate;IPAR-P3-02;IPAR-P3-03;IPAR-P3-10;IPAR-P7-02;IPAR-P7-12;page_preallocation;database_lifecycle_storage;mga_transaction_regression"
  )

  add_executable(ipar_index_family_preallocation_gate
    ipar_index_family_preallocation_gate.cpp
  )
  target_compile_features(ipar_index_family_preallocation_gate PRIVATE cxx_std_23)
  target_link_libraries(ipar_index_family_preallocation_gate
    PRIVATE
      sb_core_bulk_load
      sb_core_index
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sb_engine_sblr
      sb_storage_database
  )

  add_test(
    NAME ipar_index_family_preallocation_gate
    COMMAND ipar_index_family_preallocation_gate
  )
  set_tests_properties(ipar_index_family_preallocation_gate PROPERTIES
    LABELS "ipar_index_family_preallocation_gate;IPAR-P3-10;IPAR-P4-02;IPAR-P7-12;index_page_preallocation;database_lifecycle_storage;mga_transaction_regression"
  )

  add_executable(cdp_statistics_access_path_gate
    cdp_statistics_access_path_gate.cpp
  )
  target_compile_features(cdp_statistics_access_path_gate PRIVATE cxx_std_20)
  target_link_libraries(cdp_statistics_access_path_gate
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sb_storage_database
  )

  add_test(
    NAME cdp_statistics_access_path_gate
    COMMAND cdp_statistics_access_path_gate
  )
  set_tests_properties(cdp_statistics_access_path_gate PROPERTIES
    LABELS "cdp_statistics_access_path_gate;CDP-022;CDP_P2_STATS_ACCESS_PATH_READY;database_lifecycle_optimizer;mga_transaction_regression"
  )

  add_executable(dpc_plan_stability_statistics_lifecycle_gate
    dpc_plan_stability_statistics_lifecycle_gate.cpp
  )
  target_compile_features(dpc_plan_stability_statistics_lifecycle_gate PRIVATE cxx_std_20)
  target_link_libraries(dpc_plan_stability_statistics_lifecycle_gate
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sb_storage_database
  )

  add_test(
    NAME dpc_plan_stability_statistics_lifecycle_gate
    COMMAND dpc_plan_stability_statistics_lifecycle_gate
  )
  set_tests_properties(dpc_plan_stability_statistics_lifecycle_gate PROPERTIES
    LABELS "dpc_plan_stability_statistics_lifecycle_gate;DPC-010;DPC_TEST_PLAN_STABILITY;ELER-107;ELER-GATE-107;plan_stability;optimizer_statistics;explain;support_bundle;operational_readiness;mga_transaction_regression;database_lifecycle"
  )

  add_executable(cdp_join_costing_gate
    cdp_join_costing_gate.cpp
  )
  target_compile_features(cdp_join_costing_gate PRIVATE cxx_std_20)
  target_link_libraries(cdp_join_costing_gate
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sb_storage_database
  )

  add_test(
    NAME cdp_join_costing_gate
    COMMAND cdp_join_costing_gate
  )
  set_tests_properties(cdp_join_costing_gate PROPERTIES
    LABELS "cdp_join_costing_gate;CDP-023;CDP_P2_JOIN_COSTING_READY;database_lifecycle_optimizer;mga_transaction_regression"
  )

  add_executable(dpc_query_crud_relation_materialization_gate
    dpc_query_crud_relation_materialization_gate.cpp
  )
  target_compile_features(dpc_query_crud_relation_materialization_gate PRIVATE cxx_std_20)
  target_link_libraries(dpc_query_crud_relation_materialization_gate
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sb_storage_database
  )

  add_test(
    NAME dpc_query_crud_relation_materialization_gate
    COMMAND dpc_query_crud_relation_materialization_gate
  )
  set_tests_properties(dpc_query_crud_relation_materialization_gate PROPERTIES
    LABELS "dpc_query_crud_relation_materialization_gate;DPC-065;DPC_QUERY_CRUD_RELATION_MATERIALIZATION;database_lifecycle_optimizer;mga_transaction_regression"
  )
endif()

add_executable(database_lifecycle_existing_reconciliation_conformance
  existing_lifecycle_reconciliation_conformance.cpp
)
target_compile_features(database_lifecycle_existing_reconciliation_conformance PRIVATE cxx_std_23)
target_compile_definitions(
  database_lifecycle_existing_reconciliation_conformance
  PRIVATE
    SB_DBLC_RECONCILIATION_AUDIT="${SB_DATABASE_LIFECYCLE_RECONCILIATION_AUDIT}"
    SB_DBLC_RECONCILIATION_REPORT="${CMAKE_CURRENT_BINARY_DIR}/DATABASE_LIFECYCLE_RECONCILIATION_REPORT.md"
    SB_DBLC_REPO_ROOT="${SB_PRIVATE_REPO_ROOT}"
    SB_DBLC_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
)

add_test(
  NAME database_lifecycle_existing_reconciliation_conformance
  COMMAND database_lifecycle_existing_reconciliation_conformance
)
set_tests_properties(database_lifecycle_existing_reconciliation_conformance PROPERTIES
  LABELS "database_lifecycle_existing_reconciliation_conformance;database_lifecycle_existing_reconciliation;DBLC_STATIC_NO_LEGACY_LIFECYCLE_DRIFT;database_lifecycle;mga_transaction_regression"
)

add_executable(database_lifecycle_traceability_conformance
  traceability_conformance.cpp
)
target_compile_features(database_lifecycle_traceability_conformance PRIVATE cxx_std_23)
target_compile_definitions(
  database_lifecycle_traceability_conformance
  PRIVATE
    SB_DBLC_TRACEABILITY_AUDIT="${SB_DATABASE_LIFECYCLE_TRACEABILITY_AUDIT}"
    SB_DBLC_TRACEABILITY_REPORT="${CMAKE_CURRENT_BINARY_DIR}/DATABASE_LIFECYCLE_TRACEABILITY_REPORT.md"
    SB_DBLC_REPO_ROOT="${SB_PRIVATE_REPO_ROOT}"
    SB_DBLC_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
)

add_test(
  NAME database_lifecycle_traceability_conformance
  COMMAND database_lifecycle_traceability_conformance
)
set_tests_properties(database_lifecycle_traceability_conformance PROPERTIES
  LABELS "database_lifecycle_traceability;DBLC_P13R_TRACEABILITY_COMPLETE;database_lifecycle"
)

add_test(
  NAME database_lifecycle_traceability_static
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/traceability_static.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(database_lifecycle_traceability_static PROPERTIES
  LABELS "database_lifecycle_traceability;DBLC_STATIC_TRACEABILITY_COVERAGE;DBLC_P13R_TRACEABILITY_COMPLETE;database_lifecycle"
)

add_executable(database_lifecycle_exhaustive_regression_conformance
  exhaustive_lifecycle_regression_conformance.cpp
)
target_compile_features(database_lifecycle_exhaustive_regression_conformance PRIVATE cxx_std_23)
target_compile_definitions(
  database_lifecycle_exhaustive_regression_conformance
  PRIVATE
    SB_DBLC_REPO_ROOT="${SB_PRIVATE_REPO_ROOT}"
    SB_DBLC_BUILD_ROOT="${CMAKE_BINARY_DIR}"
    SB_DBLC_VALIDATION_PLAN="${SB_DATABASE_LIFECYCLE_FIXTURE_ROOT}/VALIDATION_PLAN.md"
    SB_DBLC_REGRESSION_REPORT="${SB_DATABASE_LIFECYCLE_FIXTURE_ROOT}/artifacts/DATABASE_LIFECYCLE_REGRESSION_REPORT.md"
    SB_DBLC_REGRESSION_MATRIX="${SB_DATABASE_LIFECYCLE_FIXTURE_ROOT}/artifacts/DATABASE_LIFECYCLE_REGRESSION_MATRIX.csv"
)

add_test(
  NAME database_lifecycle_exhaustive_regression_conformance
  COMMAND database_lifecycle_exhaustive_regression_conformance
)
set_tests_properties(database_lifecycle_exhaustive_regression_conformance PROPERTIES
  LABELS "database_lifecycle_exhaustive;database_lifecycle_fault_injection;database_lifecycle_release;DBLC_P16_REGRESSION_COMPLETE;database_lifecycle"
)

add_test(
  NAME database_lifecycle_exhaustive_static
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/exhaustive_lifecycle_static.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(database_lifecycle_exhaustive_static PROPERTIES
  LABELS "database_lifecycle_exhaustive;database_lifecycle_fault_injection;database_lifecycle_release;DBLC_STATIC_REGRESSION_REPORT_ARTIFACT;DBLC_P16_REGRESSION_COMPLETE;database_lifecycle"
)

if(TARGET sb_server_core AND TARGET sb_storage_database AND TARGET sb_storage_disk AND
   TARGET sb_storage_page AND TARGET sb_transaction_mga AND TARGET sb_engine_internal_api AND
   TARGET sbl_firebird_parser_pipeline)
  add_executable(database_lifecycle_fault_injection_conformance
    fault_injection_conformance.cpp
  )
  target_compile_features(database_lifecycle_fault_injection_conformance PRIVATE cxx_std_23)
  target_compile_definitions(
    database_lifecycle_fault_injection_conformance
    PRIVATE
      SB_BOOTSTRAP_SEED_PACK_ROOT="${CMAKE_SOURCE_DIR}/resources/seed-packs/initial-resource-pack"
  )
  target_link_libraries(database_lifecycle_fault_injection_conformance
    PRIVATE
      sb_core_memory
      sb_core_platform
      sb_core_uuid
      sb_server_core
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
      sbl_firebird_parser_pipeline
  )

  add_test(
    NAME database_lifecycle_fault_injection_conformance
    COMMAND database_lifecycle_fault_injection_conformance
  )
  set_tests_properties(database_lifecycle_fault_injection_conformance PROPERTIES
    LABELS "database_lifecycle_fault_injection;DBLC_P17_HARDENED;database_lifecycle;mga_transaction_regression"
  )
endif()

add_test(
  NAME database_lifecycle_authority_drift_static
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/authority_drift_static.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(database_lifecycle_authority_drift_static PROPERTIES
  LABELS "database_lifecycle_fault_injection;DBLC_STATIC_AUTHORITY_DRIFT_GATES;DBLC_P17_HARDENED;database_lifecycle"
)

add_test(
  NAME database_lifecycle_no_lifecycle_placeholders_static
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/no_lifecycle_placeholders_static.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(database_lifecycle_no_lifecycle_placeholders_static PROPERTIES
  LABELS "database_lifecycle_fault_injection;DBLC_STATIC_NO_LIFECYCLE_PLACEHOLDERS;DBLC_P17_HARDENED;database_lifecycle"
)

add_test(
  NAME database_lifecycle_release_zero_open_audit
  COMMAND "${Python3_EXECUTABLE}" "${SB_DATABASE_LIFECYCLE_FINAL_ZERO_OPEN_AUDIT}"
          --fixture-root "${SB_DATABASE_LIFECYCLE_FIXTURE_ROOT}"
)
set_tests_properties(database_lifecycle_release_zero_open_audit PROPERTIES
  LABELS "database_lifecycle_release;DBLC_STATIC_FINAL_ZERO_OPEN_AUDIT;DBLC_P18_FINAL_CLEAN;database_lifecycle"
)

add_test(
  NAME database_lifecycle_default_policy_catalog
  COMMAND "${Python3_EXECUTABLE}" "${SB_DATABASE_LIFECYCLE_POLICY_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --mode catalog
)
set_tests_properties(database_lifecycle_default_policy_catalog PROPERTIES
  LABELS "database_lifecycle_default_policy_catalog;database_lifecycle"
)

add_test(
  NAME database_lifecycle_default_policy_registry
  COMMAND "${Python3_EXECUTABLE}" "${SB_DATABASE_LIFECYCLE_POLICY_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --mode registry
)
set_tests_properties(database_lifecycle_default_policy_registry PROPERTIES
  LABELS "database_lifecycle_default_policy_registry;database_lifecycle"
)

add_test(
  NAME database_lifecycle_policy_diagnostic_registry
  COMMAND "${Python3_EXECUTABLE}" "${SB_DATABASE_LIFECYCLE_POLICY_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --mode diagnostics
)
set_tests_properties(database_lifecycle_policy_diagnostic_registry PROPERTIES
  LABELS "database_lifecycle_policy_diagnostic_registry;database_lifecycle"
)

add_test(
  NAME database_lifecycle_policy_override_fixtures
  COMMAND "${Python3_EXECUTABLE}" "${SB_DATABASE_LIFECYCLE_POLICY_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --mode overrides
)
set_tests_properties(database_lifecycle_policy_override_fixtures PROPERTIES
  LABELS "database_lifecycle_policy_override_fixtures;database_lifecycle"
)

add_test(
  NAME database_lifecycle_canonical_spec_closure
  COMMAND "${Python3_EXECUTABLE}" "${SB_DATABASE_LIFECYCLE_POLICY_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --mode specs
)
set_tests_properties(database_lifecycle_canonical_spec_closure PROPERTIES
  LABELS "database_lifecycle_canonical_spec_closure;database_lifecycle"
)

set(SB_DATABASE_LIFECYCLE_REGISTRY_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/lifecycle_registry_conformance.py"
)

add_test(
  NAME database_lifecycle_engine_api
  COMMAND "${Python3_EXECUTABLE}" "${SB_DATABASE_LIFECYCLE_REGISTRY_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(database_lifecycle_engine_api PROPERTIES
  LABELS "database_lifecycle_engine_api;database_lifecycle"
)

if(TARGET sb_storage_database)
  add_executable(database_lifecycle_durable_state_conformance
    lifecycle_durable_state_conformance.cpp
  )
  target_compile_features(database_lifecycle_durable_state_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_durable_state_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_storage_database
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_durable_state_conformance
    COMMAND database_lifecycle_durable_state_conformance
  )
  set_tests_properties(database_lifecycle_durable_state_conformance PROPERTIES
    LABELS "database_lifecycle_durable_state_conformance;database_lifecycle;database_lifecycle_unit;database_lifecycle_storage;mga_transaction_regression"
  )

  add_executable(database_lifecycle_create_bootstrap_conformance
    create_bootstrap_conformance.cpp
  )
  target_compile_features(database_lifecycle_create_bootstrap_conformance PRIVATE cxx_std_23)
  target_compile_definitions(
    database_lifecycle_create_bootstrap_conformance
    PRIVATE
      SB_BOOTSTRAP_SEED_PACK_ROOT="${CMAKE_SOURCE_DIR}/resources/seed-packs/initial-resource-pack"
  )
  target_link_libraries(database_lifecycle_create_bootstrap_conformance
    PRIVATE
      sb_core_catalog
      sb_core_platform
      sb_core_uuid
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
      sb_engine_sblr
  )

  add_test(
    NAME database_lifecycle_create_bootstrap_conformance
    COMMAND database_lifecycle_create_bootstrap_conformance
  )
  set_tests_properties(database_lifecycle_create_bootstrap_conformance PROPERTIES
    LABELS "database_lifecycle_create_bootstrap_conformance;database_lifecycle;database_lifecycle_storage;database_lifecycle_default_policy_catalog;database_lifecycle_tx_gate"
  )

  add_executable(database_lifecycle_first_open_activation_conformance
    first_open_activation_conformance.cpp
  )
  target_compile_features(database_lifecycle_first_open_activation_conformance PRIVATE cxx_std_23)
  target_compile_definitions(
    database_lifecycle_first_open_activation_conformance
    PRIVATE
      SB_BOOTSTRAP_SEED_PACK_ROOT="${CMAKE_SOURCE_DIR}/resources/seed-packs/initial-resource-pack"
  )
  target_link_libraries(database_lifecycle_first_open_activation_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_first_open_activation_conformance
    COMMAND database_lifecycle_first_open_activation_conformance
  )
  set_tests_properties(database_lifecycle_first_open_activation_conformance PROPERTIES
    LABELS "database_lifecycle_first_open_activation_conformance;database_lifecycle;database_lifecycle_storage;mga_transaction_regression;database_lifecycle_tx_gate"
  )

  add_executable(database_lifecycle_filespace_conformance
    filespace_lifecycle_conformance.cpp
  )
  target_compile_features(database_lifecycle_filespace_conformance PRIVATE cxx_std_23)
  target_compile_definitions(
    database_lifecycle_filespace_conformance
    PRIVATE
      SB_BOOTSTRAP_SEED_PACK_ROOT="${CMAKE_SOURCE_DIR}/resources/seed-packs/initial-resource-pack"
  )
  target_link_libraries(database_lifecycle_filespace_conformance
    PRIVATE
      sb_core_catalog
      sb_core_platform
      sb_core_uuid
      sb_storage_database
      sb_storage_disk
      sb_storage_page
  )

  add_test(
    NAME database_lifecycle_filespace_conformance
    COMMAND database_lifecycle_filespace_conformance
  )
  set_tests_properties(database_lifecycle_filespace_conformance PROPERTIES
    LABELS "database_lifecycle_filespace;database_lifecycle;database_lifecycle_storage;filespace_lifecycle_gate;database_lifecycle_tx_gate"
  )

  if(TARGET sb_storage_filespace)
    add_executable(database_lifecycle_storage_page_filespace_p2_conformance
      storage_page_filespace_p2_conformance.cpp
    )
    target_compile_features(database_lifecycle_storage_page_filespace_p2_conformance PRIVATE cxx_std_23)
    target_link_libraries(database_lifecycle_storage_page_filespace_p2_conformance
      PRIVATE
        sb_core_metrics
        sb_core_platform
        sb_core_uuid
        sb_storage_disk
        sb_storage_filespace
        sb_storage_page
    )

    add_test(
      NAME database_lifecycle_storage_page_filespace_p2_conformance
      COMMAND database_lifecycle_storage_page_filespace_p2_conformance
    )
    set_tests_properties(database_lifecycle_storage_page_filespace_p2_conformance PROPERTIES
      LABELS "storage_page_layout_gate;filespace_lifecycle_gate;foreign_filespace_quarantine_gate;storage_metrics_gate;database_lifecycle;database_lifecycle_storage;mga_transaction_regression"
    )

    add_executable(database_lifecycle_final_deferred_storage_workflows_gate
      final_deferred_storage_workflows_gate.cpp
    )
    target_compile_features(database_lifecycle_final_deferred_storage_workflows_gate PRIVATE cxx_std_20)
    target_link_libraries(database_lifecycle_final_deferred_storage_workflows_gate
      PRIVATE
        sb_storage_filespace
        sb_storage_page
    )

    add_test(
      NAME database_lifecycle_final_deferred_storage_workflows_gate
      COMMAND database_lifecycle_final_deferred_storage_workflows_gate
    )
    set_tests_properties(database_lifecycle_final_deferred_storage_workflows_gate PROPERTIES
      LABELS "database_lifecycle_final_deferred_storage_workflows_gate;FINAL-DEFERRED-IMPLEMENTATION-TRACKER;MDF-001;MDF-002;MDF-003;MDF-004;MDF-005;MDF-006;MDF-007;MDF-008;MDF-009;MDF-010;FILES-COMMAND-CLOSURE-MAINTENANCE-SUPPORTED-SUBSET-IMPLEMENTED;ACTIVE-PRIMARY-REPLACEMENT-PHYSICAL-HEADER-RELOCATION-PROOF-IMPLEMENTED;ACTIVE-PRIMARY-REPLACEMENT-AUTHORITY-SWITCH-IMPLEMENTED;SNAPSHOT-SHADOW-LOCAL-LIFECYCLE-COMMAND-SUBSET-IMPLEMENTED;ENCRYPTION-KEY-LIFECYCLE-ENGINE-BOUNDARY-IMPLEMENTED;ENCRYPTION-MAINTENANCE-SBSQL-ROUTE-IMPLEMENTED;REPAIR-REBUILD-SALVAGE-DATABASE-EVIDENCE-BOUNDARY-IMPLEMENTED;FILESPACE-REPAIR-REBUILD-SALVAGE-SBSQL-ROUTE-IMPLEMENTED;FILESPACE-ORPHAN-STALE-DISCOVERY-CLASSIFIER-IMPLEMENTED;FILESPACE-DISCOVERY-PHYSICAL-CLEANUP-DELETE-IMPLEMENTED;FILESPACE-PACKAGE-STORAGE-WORKFLOW-IMPLEMENTED;FILESPACE-PACKAGE-PHYSICAL-MEMBER-TRANSFER-IMPLEMENTED;SHARD-PLACEMENT-DESCRIPTOR-PLANNER-IMPLEMENTED;SHARD-PLACEMENT-SBSQL-ROUTE-IMPLEMENTED;PAGE-REGISTRY-STATUS-MATRIX-IMPLEMENTED;STORAGE-TIER-DESCRIPTOR-PLANNER-IMPLEMENTED;STORAGE-TIER-BOUNDED-PHYSICAL-RELOCATION-IMPLEMENTED;database_lifecycle;database_lifecycle_storage;mga_transaction_regression"
    )

    if(TARGET sb_engine_internal_api)
      add_executable(database_lifecycle_storage_tier_migration_conformance
        storage_tier_migration_conformance.cpp
      )
      target_compile_features(database_lifecycle_storage_tier_migration_conformance PRIVATE cxx_std_20)
      target_include_directories(database_lifecycle_storage_tier_migration_conformance
        PRIVATE
          "${PROJECT_SOURCE_DIR}/include"
      )
      target_link_libraries(database_lifecycle_storage_tier_migration_conformance
        PRIVATE
          sb_engine_internal_api
          sb_storage_page
      )

      add_test(
        NAME database_lifecycle_storage_tier_migration_conformance
        COMMAND database_lifecycle_storage_tier_migration_conformance
      )
      set_tests_properties(database_lifecycle_storage_tier_migration_conformance PROPERTIES
        LABELS "storage_tier_migration;storage_management;database_lifecycle;database_lifecycle_storage;mga_transaction_regression"
      )
    endif()

    add_executable(filespace_preallocation_runtime_gate
      filespace_preallocation_runtime_gate.cpp
    )
    target_compile_features(filespace_preallocation_runtime_gate PRIVATE cxx_std_23)
    target_link_libraries(filespace_preallocation_runtime_gate
      PRIVATE
        sb_core_metrics
        sb_core_platform
        sb_core_uuid
        sb_storage_filespace
    )

    add_test(
      NAME filespace_preallocation_runtime_gate
      COMMAND filespace_preallocation_runtime_gate
    )
    set_tests_properties(filespace_preallocation_runtime_gate PROPERTIES
      LABELS "PFAR-009;filespace_preallocation_runtime_gate;database_lifecycle_storage;mga_transaction_regression"
    )

    add_executable(filespace_growth_runtime_gate
      filespace_growth_runtime_gate.cpp
    )
    target_compile_features(filespace_growth_runtime_gate PRIVATE cxx_std_23)
    target_link_libraries(filespace_growth_runtime_gate
      PRIVATE
        sb_core_metrics
        sb_core_platform
        sb_core_uuid
        sb_storage_filespace
    )

    add_test(
      NAME filespace_growth_runtime_gate
      COMMAND filespace_growth_runtime_gate
    )
    set_tests_properties(filespace_growth_runtime_gate PROPERTIES
      LABELS "PFAR-010;IPAR-P3-02;filespace_growth_runtime_gate;database_lifecycle_storage;mga_transaction_regression"
    )

    add_executable(ipar_platform_storage_preallocation_gate
      ipar_platform_storage_preallocation_gate.cpp
    )
    target_compile_features(ipar_platform_storage_preallocation_gate PRIVATE cxx_std_23)
    target_link_libraries(ipar_platform_storage_preallocation_gate
      PRIVATE
        sb_core_platform
        sb_storage_disk
    )

    add_test(
      NAME ipar_platform_storage_preallocation_gate
      COMMAND ipar_platform_storage_preallocation_gate
    )
    set_tests_properties(ipar_platform_storage_preallocation_gate PROPERTIES
      LABELS "ipar_platform_storage_preallocation_gate;IPAR-P0-06;IPAR-P7-08;platform_storage_preallocation;database_lifecycle_storage;mga_transaction_regression"
    )

    add_executable(database_lifecycle_cloud_storage_emulator_conformance
      cloud_storage_emulator_conformance.cpp
    )
    target_compile_features(database_lifecycle_cloud_storage_emulator_conformance PRIVATE cxx_std_23)
    target_link_libraries(database_lifecycle_cloud_storage_emulator_conformance
      PRIVATE
        sb_core_metrics
        sb_core_platform
        sb_core_uuid
        sb_storage_filespace
    )

    add_test(
      NAME database_lifecycle_cloud_storage_emulator_conformance
      COMMAND database_lifecycle_cloud_storage_emulator_conformance
    )
    set_tests_properties(database_lifecycle_cloud_storage_emulator_conformance PROPERTIES
      LABELS "cloud_storage_emulator_gate;storage_metrics_gate;database_lifecycle;database_lifecycle_storage;mga_transaction_regression"
    )
  endif()

  add_executable(database_lifecycle_open_recovery_conformance
    open_recovery_conformance.cpp
  )
  target_compile_features(database_lifecycle_open_recovery_conformance PRIVATE cxx_std_23)
  target_compile_definitions(
    database_lifecycle_open_recovery_conformance
    PRIVATE
      SB_BOOTSTRAP_SEED_PACK_ROOT="${CMAKE_SOURCE_DIR}/resources/seed-packs/initial-resource-pack"
  )
  target_link_libraries(database_lifecycle_open_recovery_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_open_recovery_conformance
    COMMAND database_lifecycle_open_recovery_conformance
  )
  set_tests_properties(database_lifecycle_open_recovery_conformance PROPERTIES
    LABELS "database_lifecycle_open_recovery_conformance;database_lifecycle;database_lifecycle_storage;mga_transaction_regression;mga_recovery_proof_model_gate;mga_checkpoint_generation_gate;mga_dirty_manifest_emission_gate;mga_recovery_idempotency_gate;PRF_G07_MGA_RECOVERY_READY"
  )

  add_executable(database_lifecycle_cache_checkpoint_conformance
    cache_checkpoint_conformance.cpp
  )
  target_compile_features(database_lifecycle_cache_checkpoint_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_cache_checkpoint_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_cache_checkpoint_conformance
    COMMAND database_lifecycle_cache_checkpoint_conformance
  )
  set_tests_properties(database_lifecycle_cache_checkpoint_conformance PROPERTIES
    LABELS "database_lifecycle_cache_checkpoint_conformance;database_lifecycle_cache_checkpoint;database_lifecycle;database_lifecycle_storage;database_lifecycle_engine_agent;database_lifecycle_shutdown_notification;mga_transaction_regression"
  )

  add_executable(page_cache_frame_ownership_gate
    page_cache_frame_ownership_gate.cpp
  )
  target_compile_features(page_cache_frame_ownership_gate PRIVATE cxx_std_23)
  target_link_libraries(page_cache_frame_ownership_gate
    PRIVATE
      sb_core_memory
      sb_core_platform
      sb_core_uuid
      sb_storage_page
  )
  add_test(
    NAME page_cache_frame_ownership_gate
    COMMAND page_cache_frame_ownership_gate
  )
  set_tests_properties(page_cache_frame_ownership_gate PROPERTIES
    LABELS "page_cache_frame_ownership_gate;MMCH-050;memory;page_cache;frame_ownership;database_lifecycle;mga_transaction_regression"
  )

  add_executable(page_cache_pressure_protection_gate
    page_cache_pressure_protection_gate.cpp
  )
  target_compile_features(page_cache_pressure_protection_gate PRIVATE cxx_std_23)
  target_link_libraries(page_cache_pressure_protection_gate
    PRIVATE
      sb_core_memory
      sb_core_platform
      sb_core_uuid
      sb_storage_page
  )
  add_test(
    NAME page_cache_pressure_protection_gate
    COMMAND page_cache_pressure_protection_gate
  )
  set_tests_properties(page_cache_pressure_protection_gate PROPERTIES
    LABELS "page_cache_pressure_protection_gate;MMCH-052;memory;page_cache;pressure;database_lifecycle;mga_transaction_regression"
  )

  add_executable(memory_adaptive_pressure_response_gate
    memory_adaptive_pressure_response_gate.cpp
  )
  target_compile_features(memory_adaptive_pressure_response_gate PRIVATE cxx_std_23)
  target_link_libraries(memory_adaptive_pressure_response_gate
    PRIVATE
      sb_core_memory
      sb_core_platform
      sb_core_uuid
      sb_storage_page
  )
  add_test(
    NAME memory_adaptive_pressure_response_gate
    COMMAND memory_adaptive_pressure_response_gate
  )
  set_tests_properties(memory_adaptive_pressure_response_gate PROPERTIES
    LABELS "memory_adaptive_pressure_response_gate;MMCH-018;memory;pressure_response;page_cache;database_lifecycle;mga_transaction_regression"
  )

  add_executable(memory_background_reclamation_gate
    memory_background_reclamation_gate.cpp
  )
  target_compile_features(memory_background_reclamation_gate PRIVATE cxx_std_23)
  target_link_libraries(memory_background_reclamation_gate
    PRIVATE
      sb_core_memory
      sb_core_platform
      sb_core_uuid
      sb_storage_page
  )
  add_test(
    NAME memory_background_reclamation_gate
    COMMAND memory_background_reclamation_gate
  )
  set_tests_properties(memory_background_reclamation_gate PROPERTIES
    LABELS "memory_background_reclamation_gate;MMCH-019;memory;background_reclamation;page_cache;temp_workspace;database_lifecycle;mga_transaction_regression"
  )

  add_test(
    NAME page_buffer_source_audit_gate
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/page_buffer_source_audit_gate.py"
            --repo-root "${SB_PRIVATE_REPO_ROOT}"
  )
  set_tests_properties(page_buffer_source_audit_gate PROPERTIES
    LABELS "page_buffer_source_audit_gate;MMCH-051;memory;page_cache;source_audit;database_lifecycle;mga_transaction_regression"
  )
endif()

if(TARGET sb_engine_internal_api AND TARGET sb_storage_database)
  add_executable(database_lifecycle_catalog_object_conformance
    catalog_object_conformance.cpp
  )
  target_compile_features(database_lifecycle_catalog_object_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_catalog_object_conformance
    PRIVATE
      sb_core_uuid
      sb_engine_internal_api
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_catalog_object_conformance
    COMMAND database_lifecycle_catalog_object_conformance
  )
  set_tests_properties(database_lifecycle_catalog_object_conformance PROPERTIES
    LABELS "database_lifecycle_catalog_object_conformance;database_lifecycle_catalog_object;database_lifecycle;database_lifecycle_catalog;catalog_common_descriptor_gate;catalog_generation_mga_gate;catalog_name_comment_resolver_gate;catalog_dependency_api_gate;catalog_diagnostic_vector_gate;catalog_wire_metadata_gate;constraint_catalog_descriptor_gate;PRF_G06_CONSTRAINTS_READY;mga_transaction_regression"
  )

  add_executable(database_lifecycle_sbsql_multilingual_name_registry_conformance
    sbsql_multilingual_name_registry_conformance.cpp
  )
  target_compile_features(database_lifecycle_sbsql_multilingual_name_registry_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_sbsql_multilingual_name_registry_conformance
    PRIVATE
      sb_core_uuid
      sb_engine_internal_api
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_sbsql_multilingual_name_registry_conformance
    COMMAND database_lifecycle_sbsql_multilingual_name_registry_conformance
  )
  set_tests_properties(database_lifecycle_sbsql_multilingual_name_registry_conformance PROPERTIES
    LABELS "database_lifecycle_sbsql_multilingual_name_registry_conformance;SML-004;SML-GATE-002;sbsql_multilingual;database_lifecycle_catalog;catalog_name_comment_resolver_gate;mga_transaction_regression"
  )

  add_executable(cdp_uuid_descriptor_cache_gate
    cdp_uuid_descriptor_cache_gate.cpp
  )
  target_compile_features(cdp_uuid_descriptor_cache_gate PRIVATE cxx_std_23)
  target_link_libraries(cdp_uuid_descriptor_cache_gate
    PRIVATE
      sb_core_uuid
      sb_engine_internal_api
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME cdp_uuid_descriptor_cache_gate
    COMMAND cdp_uuid_descriptor_cache_gate
  )
  set_tests_properties(cdp_uuid_descriptor_cache_gate PROPERTIES
    LABELS "cdp_uuid_descriptor_cache_gate;CDP-021;database_lifecycle_catalog;mga_transaction_regression"
  )

  add_executable(cdp_plan_cache_live_gate
    cdp_plan_cache_live_gate.cpp
  )
  target_compile_features(cdp_plan_cache_live_gate PRIVATE cxx_std_23)
  target_link_libraries(cdp_plan_cache_live_gate
    PRIVATE
      sb_core_uuid
      sb_engine_internal_api
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME cdp_plan_cache_live_gate
    COMMAND cdp_plan_cache_live_gate
  )
  set_tests_properties(cdp_plan_cache_live_gate PROPERTIES
    LABELS "cdp_plan_cache_live_gate;CDP-020;database_lifecycle_catalog;mga_transaction_regression"
  )

  add_executable(cdp_persisted_format_transient_rebuild_gate
    cdp_persisted_format_transient_rebuild_gate.cpp
  )
  target_compile_features(cdp_persisted_format_transient_rebuild_gate PRIVATE cxx_std_23)
  target_link_libraries(cdp_persisted_format_transient_rebuild_gate
    PRIVATE
      sb_core_uuid
      sb_engine_internal_api
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME cdp_persisted_format_transient_rebuild_gate
    COMMAND cdp_persisted_format_transient_rebuild_gate
  )
  set_tests_properties(cdp_persisted_format_transient_rebuild_gate PROPERTIES
    LABELS "cdp_persisted_format_transient_rebuild_gate;CDP-024;CDP_P2_PERSISTED_FORMAT_READY;database_lifecycle_optimizer;mga_transaction_regression"
  )

  add_executable(dpc_persisted_format_restricted_open_gate
    dpc_persisted_format_restricted_open_gate.cpp
  )
  target_compile_features(dpc_persisted_format_restricted_open_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_persisted_format_restricted_open_gate
    PRIVATE
      sb_core_uuid
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME dpc_persisted_format_restricted_open_gate
    COMMAND dpc_persisted_format_restricted_open_gate
  )
  set_tests_properties(dpc_persisted_format_restricted_open_gate PROPERTIES
    LABELS "dpc_persisted_format_restricted_open_gate;DPC-009;DPC_TEST_PERSISTED_FORMAT;persisted_format;restricted_open;repair;mga_transaction_regression;database_lifecycle"
  )

  add_executable(optimizer_deficiency_odf_118_gate
    optimizer_deficiency_odf_118_gate.cpp
  )
  target_compile_features(optimizer_deficiency_odf_118_gate PRIVATE cxx_std_23)
  target_link_libraries(optimizer_deficiency_odf_118_gate
    PRIVATE
      sb_core_index
      sb_core_uuid
      sb_engine_internal_api
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME optimizer_deficiency_odf_118_gate
    COMMAND optimizer_deficiency_odf_118_gate
  )
  set_tests_properties(optimizer_deficiency_odf_118_gate PROPERTIES
    LABELS "optimizer_deficiency_odf_118_gate;ODF-118;ODF_P7_BACKFILL_BOOTSTRAP_READY;backfill_bootstrap;persisted_format;restricted_open;support_bundle;uuid_resolution;mga_transaction_regression;database_lifecycle"
  )

  add_executable(optimizer_deficiency_odf_120_gate
    optimizer_deficiency_odf_120_gate.cpp
  )
  target_compile_features(optimizer_deficiency_odf_120_gate PRIVATE cxx_std_23)
  target_link_libraries(optimizer_deficiency_odf_120_gate
    PRIVATE
      sb_core_index
      sb_core_uuid
      sb_engine_internal_api
  )

  add_test(
    NAME optimizer_deficiency_odf_120_gate
    COMMAND optimizer_deficiency_odf_120_gate
  )
  set_tests_properties(optimizer_deficiency_odf_120_gate PROPERTIES
    LABELS "optimizer_deficiency_odf_120_gate;ODF-120;ODF_P7_BACKUP_REPAIR_READY;backup_restore;validate_repair;support_bundle;mga_transaction_regression;database_lifecycle"
  )

  add_executable(optimizer_deficiency_odf_121_gate
    optimizer_deficiency_odf_121_gate.cpp
  )
  target_compile_features(optimizer_deficiency_odf_121_gate PRIVATE cxx_std_23)
  target_link_libraries(optimizer_deficiency_odf_121_gate
    PRIVATE
      sb_core_agents
      sb_core_index
      sb_core_uuid
      sb_engine_optimizer
      sb_transaction_mga
  )

  add_test(
    NAME optimizer_deficiency_odf_121_gate
    COMMAND optimizer_deficiency_odf_121_gate
  )
  set_tests_properties(optimizer_deficiency_odf_121_gate PROPERTIES
    LABELS "optimizer_deficiency_odf_121_gate;ODF-121;ODF_P7_ONLINE_MAINT_READY;online_maintenance;progress;cancel;resume;restart;support_bundle;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_page_extent_summary_metadata_gate
    dpc_page_extent_summary_metadata_gate.cpp
  )
  target_compile_features(dpc_page_extent_summary_metadata_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_page_extent_summary_metadata_gate
    PRIVATE
      sb_core_index
      sb_core_uuid
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME dpc_page_extent_summary_metadata_gate
    COMMAND dpc_page_extent_summary_metadata_gate
  )
  set_tests_properties(dpc_page_extent_summary_metadata_gate PROPERTIES
    LABELS "dpc_page_extent_summary_metadata_gate;DPC-011;DPC_TEST_PAGE_SUMMARY;page_summary_metadata;persisted_format;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_page_extent_summary_dml_maintenance_gate
    dpc_page_extent_summary_dml_maintenance_gate.cpp
  )
  target_compile_features(dpc_page_extent_summary_dml_maintenance_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_page_extent_summary_dml_maintenance_gate
    PRIVATE
      sb_core_index
      sb_core_uuid
  )

  add_test(
    NAME dpc_page_extent_summary_dml_maintenance_gate
    COMMAND dpc_page_extent_summary_dml_maintenance_gate
  )
  set_tests_properties(dpc_page_extent_summary_dml_maintenance_gate PROPERTIES
    LABELS "dpc_page_extent_summary_dml_maintenance_gate;DPC-012;DPC_TEST_PAGE_SUMMARY;page_summary_dml_maintenance;page_summary_rebuild;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_page_extent_summary_prune_observability_gate
    dpc_page_extent_summary_prune_observability_gate.cpp
  )
  target_compile_features(dpc_page_extent_summary_prune_observability_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_page_extent_summary_prune_observability_gate
    PRIVATE
      sb_core_index
      sb_core_uuid
      sb_engine_internal_api
  )

  add_test(
    NAME dpc_page_extent_summary_prune_observability_gate
    COMMAND dpc_page_extent_summary_prune_observability_gate
  )
  set_tests_properties(dpc_page_extent_summary_prune_observability_gate PROPERTIES
    LABELS "dpc_page_extent_summary_prune_observability_gate;DPC-013;DPC_TEST_PAGE_SUMMARY;ELER-107;ELER-GATE-107;summary_prune;explain;support_bundle;operational_readiness;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_page_extent_summary_benchmark_gate
    dpc_page_extent_summary_benchmark_gate.cpp
  )
  target_compile_features(dpc_page_extent_summary_benchmark_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_page_extent_summary_benchmark_gate
    PRIVATE
      sb_core_index
      sb_core_uuid
  )

  add_test(
    NAME dpc_page_extent_summary_benchmark_gate
    COMMAND dpc_page_extent_summary_benchmark_gate
  )
  set_tests_properties(dpc_page_extent_summary_benchmark_gate PROPERTIES
    LABELS "dpc_page_extent_summary_benchmark_gate;DPC-014;DPC_TEST_PAGE_SUMMARY;page_summary_benchmark;benchmark;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_time_range_pruning_gate
    dpc_time_range_pruning_gate.cpp
  )
  target_compile_features(dpc_time_range_pruning_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_time_range_pruning_gate
    PRIVATE
      sb_core_index
      sb_core_uuid
      sb_engine_internal_api
  )

  add_test(
    NAME dpc_time_range_pruning_gate
    COMMAND dpc_time_range_pruning_gate
  )
  set_tests_properties(dpc_time_range_pruning_gate PROPERTIES
    LABELS "dpc_time_range_pruning_gate;DPC-042;DPC_TEST_TIME_PRUNE;time_range_pruning;summary_prune;explain;support_bundle;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_inverted_search_segment_publication_gate
    dpc_inverted_search_segment_publication_gate.cpp
  )
  target_compile_features(dpc_inverted_search_segment_publication_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_inverted_search_segment_publication_gate
    PRIVATE
      sb_core_index
      sb_core_uuid
  )

  add_test(
    NAME dpc_inverted_search_segment_publication_gate
    COMMAND dpc_inverted_search_segment_publication_gate
  )
  set_tests_properties(dpc_inverted_search_segment_publication_gate PROPERTIES
    LABELS "dpc_inverted_search_segment_publication_gate;DPC-043;DPC_TEST_SEARCH_SEGMENT;inverted_search_segment;search_segment_merge;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_vector_generation_publication_gate
    dpc_vector_generation_publication_gate.cpp
  )
  target_compile_features(dpc_vector_generation_publication_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_vector_generation_publication_gate
    PRIVATE
      sb_core_index
      sb_core_uuid
  )

  add_test(
    NAME dpc_vector_generation_publication_gate
    COMMAND dpc_vector_generation_publication_gate
  )
  set_tests_properties(dpc_vector_generation_publication_gate PROPERTIES
    LABELS "dpc_vector_generation_publication_gate;DPC-044;DPC_TEST_VECTOR;vector_generation;sealed_ann_publication;recall_contract;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_specialized_index_benchmark_gate
    dpc_specialized_index_benchmark_gate.cpp
  )
  target_compile_features(dpc_specialized_index_benchmark_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_specialized_index_benchmark_gate
    PRIVATE
      sb_core_index
      sb_core_uuid
  )

  add_test(
    NAME dpc_specialized_index_benchmark_gate
    COMMAND dpc_specialized_index_benchmark_gate
  )
  set_tests_properties(dpc_specialized_index_benchmark_gate PROPERTIES
    LABELS "dpc_specialized_index_benchmark_gate;DPC-045;DPC_TEST_SPECIALIZED_INDEX_BENCHMARK;time_range_pruning;inverted_search_segment;search_segment_merge;vector_generation;benchmark;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_index_validation_repair_tooling_gate
    dpc_index_validation_repair_tooling_gate.cpp
  )
  target_compile_features(dpc_index_validation_repair_tooling_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_index_validation_repair_tooling_gate
    PRIVATE
      sb_core_index
      sb_core_uuid
  )

  add_test(
    NAME dpc_index_validation_repair_tooling_gate
    COMMAND dpc_index_validation_repair_tooling_gate
  )
  set_tests_properties(dpc_index_validation_repair_tooling_gate PROPERTIES
    LABELS "dpc_index_validation_repair_tooling_gate;FINAL-DEFERRED-IMPLEMENTATION-TRACKER;MDF-023;MDF-025;DEFER-IMO-MAINTENANCE;DEFER-IDX-MAINTENANCE;DEFER-IDX-METRICS;DEFER-ICD-DIAGNOSTICS;DEFER-IDX-CONFORMANCE;DEFER-IPL-IFM-DIO-CONFORMANCE;DPC-046;DPC_TEST_INDEX_REPAIR;ELER-107;ELER-GATE-107;index_validation_repair;management_route;sblr_route;backup_restore;operational_readiness;mga_transaction_regression;database_lifecycle"
  )

  if(TARGET sb_engine_sblr AND TARGET sb_engine_internal_api AND TARGET sb_server_core)
    add_executable(dpc_route_surface_completion_gate
      dpc_route_surface_completion_gate.cpp
    )
    target_compile_features(dpc_route_surface_completion_gate PRIVATE cxx_std_23)
    target_link_libraries(dpc_route_surface_completion_gate
      PRIVATE
        sb_core_index
        sb_core_uuid
        sb_engine
        sb_engine_internal_api
        sb_engine_sblr
        sb_server_core
    )

    add_test(
      NAME dpc_route_surface_completion_gate
      COMMAND dpc_route_surface_completion_gate
    )
    set_tests_properties(dpc_route_surface_completion_gate PROPERTIES
      LABELS "dpc_route_surface_completion_gate;DPC-060;DPC_P6_ROUTE_SURFACE_READY;DPC_TEST_ROUTE_SURFACE;index_validation_repair;sblr_route;server_admission;public_abi;driver_capability;executor_batch_internal_only;mga_transaction_regression;database_lifecycle"
    )

    add_executable(dpc_sblr_api_abi_compatibility_gate
      dpc_sblr_api_abi_compatibility_gate.cpp
    )
    target_compile_features(dpc_sblr_api_abi_compatibility_gate PRIVATE cxx_std_23)
    target_link_libraries(dpc_sblr_api_abi_compatibility_gate
      PRIVATE
        sb_core_uuid
        sb_engine
        sb_engine_internal_api
        sb_engine_sblr
        sb_server_core
    )

    add_test(
      NAME dpc_sblr_api_abi_compatibility_gate
      COMMAND dpc_sblr_api_abi_compatibility_gate "${SB_PRIVATE_REPO_ROOT}"
    )
    set_tests_properties(dpc_sblr_api_abi_compatibility_gate PROPERTIES
      LABELS "dpc_sblr_api_abi_compatibility_gate;DPC-074;DPC_SBLR_API_ABI_COMPATIBILITY_GATE;DPC_P6_SBLR_API_ABI_READY;DPC_AUDIT_SBLR_API_ABI;DPC_TEST_SBLR_API_ABI;sblr_api_abi;sblr_route;server_admission;public_abi;driver_capability;local_ipc_gate;sbwp_wire_gate;mga_transaction_regression;database_lifecycle"
    )
  endif()

  add_executable(dpc_unique_index_deferral_policy_gate
    dpc_unique_index_deferral_policy_gate.cpp
  )
  target_compile_features(dpc_unique_index_deferral_policy_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_unique_index_deferral_policy_gate
    PRIVATE
      sb_core_index
      sb_core_uuid
      sb_engine_internal_api
  )

  add_test(
    NAME dpc_unique_index_deferral_policy_gate
    COMMAND dpc_unique_index_deferral_policy_gate
  )
  set_tests_properties(dpc_unique_index_deferral_policy_gate PROPERTIES
    LABELS "dpc_unique_index_deferral_policy_gate;FINAL-DEFERRED-IMPLEMENTATION-TRACKER;MDF-024;DEFER-IDX-MGA-EXPANSION;DEFER-IMT-INDEX-MGA;DEFER-IMT-TRANSACTION-EXPANSION;DEFER-ISC-TRANSACTION-CLOSURE;DPC-019;DPC_TEST_UNIQUE_INDEX_POLICY;unique_index_policy;deferred_index;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_deferred_index_feature_flag_fallback_gate
    dpc_deferred_index_feature_flag_fallback_gate.cpp
  )
  target_compile_features(dpc_deferred_index_feature_flag_fallback_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_deferred_index_feature_flag_fallback_gate
    PRIVATE
      sb_core_index
      sb_engine_internal_api
  )

  add_test(
    NAME dpc_deferred_index_feature_flag_fallback_gate
    COMMAND dpc_deferred_index_feature_flag_fallback_gate
  )
  set_tests_properties(dpc_deferred_index_feature_flag_fallback_gate PROPERTIES
    LABELS "dpc_deferred_index_feature_flag_fallback_gate;DPC-020;DPC_TEST_INDEX_DEFER_DISABLED;deferred_index;feature_flag;synchronous_fallback;mga_transaction_regression;database_lifecycle"
  )

  add_executable(ipar_prepared_cache_lifecycle_gate
    ipar_prepared_cache_lifecycle_gate.cpp
  )
  target_compile_features(ipar_prepared_cache_lifecycle_gate PRIVATE cxx_std_23)
  target_link_libraries(ipar_prepared_cache_lifecycle_gate
    PRIVATE
      sb_core_platform
      sb_core_index
      sb_core_uuid
      sb_engine_internal_api
      sb_engine_sblr
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME ipar_prepared_cache_lifecycle_gate
    COMMAND ipar_prepared_cache_lifecycle_gate
  )
  set_tests_properties(ipar_prepared_cache_lifecycle_gate PROPERTIES
    LABELS "ipar_prepared_cache_lifecycle_gate;IPAR-P1-02;IPAR-P1-04;IPAR-P5-04;IPAR-P6-01;prepared_cache;row_encoder_validator_execution;security_fast_path;memory_arena_reuse;database_lifecycle;mga_transaction_regression"
  )

  add_executable(ipar_ddl_publication_result_gate
    ipar_ddl_publication_result_gate.cpp
  )
  target_compile_features(ipar_ddl_publication_result_gate PRIVATE cxx_std_23)
  target_link_libraries(ipar_ddl_publication_result_gate
    PRIVATE
      sb_core_platform
      sb_core_index
      sb_core_uuid
      sb_engine_internal_api
      sb_engine_sblr
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME ipar_ddl_publication_result_gate
    COMMAND ipar_ddl_publication_result_gate
  )
  set_tests_properties(ipar_ddl_publication_result_gate PROPERTIES
    LABELS "ipar_ddl_publication_result_gate;IPAR-P2-10;ddl_publish_packet;ddl_uuid_return;database_lifecycle;mga_transaction_regression"
  )

  add_executable(ipar_memory_pressure_trim_gate
    ipar_memory_pressure_trim_gate.cpp
  )
  target_compile_features(ipar_memory_pressure_trim_gate PRIVATE cxx_std_23)
  target_link_libraries(ipar_memory_pressure_trim_gate
    PRIVATE
      sb_core_index
      sb_engine_internal_api
  )

  add_test(
    NAME ipar_memory_pressure_trim_gate
    COMMAND ipar_memory_pressure_trim_gate
  )
  set_tests_properties(ipar_memory_pressure_trim_gate PROPERTIES
    LABELS "ipar_memory_pressure_trim_gate;IPAR-P6-02;prepared_cache;memory_pressure;trim_backoff;database_lifecycle;mga_transaction_regression"
  )

  add_executable(dpc_secondary_index_delta_ledger_persistence_gate
    dpc_secondary_index_delta_ledger_persistence_gate.cpp
  )
  target_compile_features(dpc_secondary_index_delta_ledger_persistence_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_secondary_index_delta_ledger_persistence_gate
    PRIVATE
      sb_core_index
      sb_core_uuid
  )

  add_test(
    NAME dpc_secondary_index_delta_ledger_persistence_gate
    COMMAND dpc_secondary_index_delta_ledger_persistence_gate
  )
  set_tests_properties(dpc_secondary_index_delta_ledger_persistence_gate PROPERTIES
    LABELS "dpc_secondary_index_delta_ledger_persistence_gate;DPC-021;DPC_TEST_INDEX_DEFER_LEDGER;secondary_index_delta_ledger;persisted_format;recovery;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_secondary_index_delta_write_path_gate
    dpc_secondary_index_delta_write_path_gate.cpp
  )
  target_compile_features(dpc_secondary_index_delta_write_path_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_secondary_index_delta_write_path_gate
    PRIVATE
      sb_core_index
      sb_core_uuid
      sb_engine_internal_api
      sb_engine_sblr
      sb_storage_database
  )

  add_test(
    NAME dpc_secondary_index_delta_write_path_gate
    COMMAND dpc_secondary_index_delta_write_path_gate
  )
  set_tests_properties(dpc_secondary_index_delta_write_path_gate PROPERTIES
    LABELS "dpc_secondary_index_delta_write_path_gate;DPC-022;DPC_TEST_INDEX_DEFER_LEDGER;secondary_index_delta_ledger;dml_write_path;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_secondary_index_delta_overlay_lookup_gate
    dpc_secondary_index_delta_overlay_lookup_gate.cpp
  )
  target_compile_features(dpc_secondary_index_delta_overlay_lookup_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_secondary_index_delta_overlay_lookup_gate
    PRIVATE
      sb_core_index
      sb_core_uuid
      sb_engine_internal_api
      sb_storage_database
  )

  add_test(
    NAME dpc_secondary_index_delta_overlay_lookup_gate
    COMMAND dpc_secondary_index_delta_overlay_lookup_gate
  )
  set_tests_properties(dpc_secondary_index_delta_overlay_lookup_gate PROPERTIES
    LABELS "dpc_secondary_index_delta_overlay_lookup_gate;DPC-023;DPC_TEST_INDEX_OVERLAY;secondary_index_delta_ledger;reader_overlay;index_lookup;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_secondary_index_delta_merge_agent_gate
    dpc_secondary_index_delta_merge_agent_gate.cpp
  )
  target_compile_features(dpc_secondary_index_delta_merge_agent_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_secondary_index_delta_merge_agent_gate
    PRIVATE
      sb_core_index
      sb_core_uuid
      sb_engine_internal_api
      sb_storage_database
  )

  add_test(
    NAME dpc_secondary_index_delta_merge_agent_gate
    COMMAND dpc_secondary_index_delta_merge_agent_gate
  )
  set_tests_properties(dpc_secondary_index_delta_merge_agent_gate PROPERTIES
    LABELS "dpc_secondary_index_delta_merge_agent_gate;DPC-024;DPC_TEST_INDEX_DELTA_MERGE;secondary_index_delta_ledger;delta_merge;resource_governor;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_secondary_index_delta_recovery_repair_gate
    dpc_secondary_index_delta_recovery_repair_gate.cpp
  )
  target_compile_features(dpc_secondary_index_delta_recovery_repair_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_secondary_index_delta_recovery_repair_gate
    PRIVATE
      sb_core_index
      sb_core_uuid
      sb_engine_internal_api
      sb_storage_database
  )

  add_test(
    NAME dpc_secondary_index_delta_recovery_repair_gate
    COMMAND dpc_secondary_index_delta_recovery_repair_gate
  )
  set_tests_properties(dpc_secondary_index_delta_recovery_repair_gate PROPERTIES
    LABELS "dpc_secondary_index_delta_recovery_repair_gate;FINAL-DEFERRED-IMPLEMENTATION-TRACKER;MDF-024;DEFER-IDX-MGA-EXPANSION;DEFER-IMT-INDEX-MGA;DEFER-IMT-TRANSACTION-EXPANSION;DEFER-ISC-TRANSACTION-CLOSURE;DPC-025;DPC_TEST_INDEX_DELTA_RECOVERY_REPAIR;secondary_index_delta_ledger;delta_recovery;delta_repair;backup_restore;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_secondary_index_garbage_cleanup_agent_gate
    dpc_secondary_index_garbage_cleanup_agent_gate.cpp
  )
  target_compile_features(dpc_secondary_index_garbage_cleanup_agent_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_secondary_index_garbage_cleanup_agent_gate
    PRIVATE
      sb_core_index
      sb_core_uuid
      sb_engine_internal_api
      sb_storage_database
  )

  add_test(
    NAME dpc_secondary_index_garbage_cleanup_agent_gate
    COMMAND dpc_secondary_index_garbage_cleanup_agent_gate
  )
  set_tests_properties(dpc_secondary_index_garbage_cleanup_agent_gate PROPERTIES
    LABELS "dpc_secondary_index_garbage_cleanup_agent_gate;DPC-033;DPC_TEST_INDEX_CLEANUP;secondary_index_garbage_cleanup;cleanup_horizon;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_shadow_index_build_lifecycle_gate
    dpc_shadow_index_build_lifecycle_gate.cpp
  )
  target_compile_features(dpc_shadow_index_build_lifecycle_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_shadow_index_build_lifecycle_gate
    PRIVATE
      sb_core_agents
      sb_core_index
      sb_core_uuid
  )

  add_test(
    NAME dpc_shadow_index_build_lifecycle_gate
    COMMAND dpc_shadow_index_build_lifecycle_gate
  )
  set_tests_properties(dpc_shadow_index_build_lifecycle_gate PROPERTIES
    LABELS "dpc_shadow_index_build_lifecycle_gate;DPC-040;IPAR-P4-05;DPC_TEST_SHADOW_INDEX;shadow_index_build_lifecycle;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_shadow_index_delta_drain_recovery_gate
    dpc_shadow_index_delta_drain_recovery_gate.cpp
  )
  target_compile_features(dpc_shadow_index_delta_drain_recovery_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_shadow_index_delta_drain_recovery_gate
    PRIVATE
      sb_core_index
      sb_core_uuid
  )

  add_test(
    NAME dpc_shadow_index_delta_drain_recovery_gate
    COMMAND dpc_shadow_index_delta_drain_recovery_gate
  )
  set_tests_properties(dpc_shadow_index_delta_drain_recovery_gate PROPERTIES
    LABELS "dpc_shadow_index_delta_drain_recovery_gate;DPC-041;IPAR-P4-05;DPC_TEST_SHADOW_INDEX;shadow_index_delta_drain;shadow_index_recovery;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_hot_update_version_chain_gate
    dpc_hot_update_version_chain_gate.cpp
  )
  target_compile_features(dpc_hot_update_version_chain_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_hot_update_version_chain_gate
    PRIVATE
      sb_core_index
      sb_core_uuid
      sb_engine_internal_api
      sb_storage_database
  )

  add_test(
    NAME dpc_hot_update_version_chain_gate
    COMMAND dpc_hot_update_version_chain_gate
  )
  set_tests_properties(dpc_hot_update_version_chain_gate PROPERTIES
    LABELS "dpc_hot_update_version_chain_gate;DPC-026;DPC_TEST_HOT_UPDATE;DPC_HOT_UPDATE_VERSION_CHAIN_GATE;hot_update;version_chain;secondary_index_delta_ledger;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_deferred_index_hot_update_benchmark_gate
    dpc_deferred_index_hot_update_benchmark_gate.cpp
  )
  target_compile_features(dpc_deferred_index_hot_update_benchmark_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_deferred_index_hot_update_benchmark_gate
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sb_storage_database
  )

  add_test(
    NAME dpc_deferred_index_hot_update_benchmark_gate
    COMMAND dpc_deferred_index_hot_update_benchmark_gate
  )
  set_tests_properties(dpc_deferred_index_hot_update_benchmark_gate PROPERTIES
    LABELS "dpc_deferred_index_hot_update_benchmark_gate;DPC-027;DPC_TEST_DEFERRED_INDEX_HOT_UPDATE_BENCHMARK;deferred_index;hot_update;benchmark;secondary_index_delta_ledger;mga_transaction_regression;database_lifecycle"
  )

  add_executable(database_lifecycle_sys_information_projection_conformance
    sys_information_projection_conformance.cpp
  )
  target_compile_features(database_lifecycle_sys_information_projection_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_sys_information_projection_conformance
    PRIVATE
      sb_engine_internal_api
  )

  add_test(
    NAME database_lifecycle_sys_information_projection_conformance
    COMMAND database_lifecycle_sys_information_projection_conformance
  )
  set_tests_properties(database_lifecycle_sys_information_projection_conformance PROPERTIES
    LABELS "database_lifecycle_sys_information_projection_conformance;database_lifecycle_sys_information_projection;database_lifecycle_catalog_index_profile;database_lifecycle;database_lifecycle_catalog;sys_catalog_readable_projection_gate;sys_information_projection_gate;information_projection_naming_gate;catalog_wire_metadata_gate;mga_transaction_regression"
  )

  add_executable(sys_agent_storage_views_gate
    sys_agent_storage_views_gate.cpp
  )
  target_compile_features(sys_agent_storage_views_gate PRIVATE cxx_std_23)
  target_link_libraries(sys_agent_storage_views_gate
    PRIVATE
      sb_engine_internal_api
  )

  add_test(
    NAME sys_agent_storage_views_gate
    COMMAND sys_agent_storage_views_gate
  )
  set_tests_properties(sys_agent_storage_views_gate PROPERTIES
    LABELS "PFAR-015;sys_agent_storage_views_gate;database_lifecycle_sys_information_projection;database_lifecycle_catalog;agent_runtime;storage_agents;mga_transaction_regression"
  )

  add_executable(ipar_observability_projection_gate
    ipar_observability_projection_gate.cpp
  )
  target_compile_features(ipar_observability_projection_gate PRIVATE cxx_std_23)
  target_link_libraries(ipar_observability_projection_gate
    PRIVATE
      sb_core_memory
      sb_engine_internal_api
      sb_engine_sblr
  )

  add_test(
    NAME ipar_observability_projection_gate
    COMMAND ipar_observability_projection_gate
  )
  set_tests_properties(ipar_observability_projection_gate PROPERTIES
    LABELS "IPAR-P6-03;IPAR-P6-06;IPAR-P6-07;IPAR-P6-08;IPAR-P6-13;ipar_observability_projection_gate;database_lifecycle_sys_information_projection;database_lifecycle_catalog;observability;agent_runtime;mga_transaction_regression"
  )

  add_executable(sys_agent_view_contract_documentation_gate
    sys_agent_view_contract_documentation_gate.cpp
  )
  target_compile_features(sys_agent_view_contract_documentation_gate PRIVATE cxx_std_23)
  target_compile_definitions(sys_agent_view_contract_documentation_gate
    PRIVATE
      SB_AGENT_SYS_VIEW_CONTRACT_DOC="${SB_PRIVATE_REPO_ROOT}/docs/reference/management-ui/agent-storage-sys-view-contract.md"
  )
  target_link_libraries(sys_agent_view_contract_documentation_gate
    PRIVATE
      sb_engine_internal_api
  )

  add_test(
    NAME sys_agent_view_contract_documentation_gate
    COMMAND sys_agent_view_contract_documentation_gate
  )
  set_tests_properties(sys_agent_view_contract_documentation_gate PROPERTIES
    LABELS "PFAR-015A;sys_agent_view_contract_documentation_gate;database_lifecycle_sys_information_projection;database_lifecycle_catalog;management_ui_docs;mga_transaction_regression"
  )

  add_executable(agent_catalog_runtime_schema_versioning_gate
    agent_catalog_runtime_schema_versioning_gate.cpp
  )
  target_compile_features(agent_catalog_runtime_schema_versioning_gate PRIVATE cxx_std_23)
  target_link_libraries(agent_catalog_runtime_schema_versioning_gate
    PRIVATE
      sb_engine_internal_api
  )

  add_test(
    NAME agent_catalog_runtime_schema_versioning_gate
    COMMAND agent_catalog_runtime_schema_versioning_gate
  )
  set_tests_properties(agent_catalog_runtime_schema_versioning_gate PROPERTIES
    LABELS "PFAR-015B;agent_catalog_runtime_schema_versioning_gate;database_lifecycle_catalog;mga_transaction_regression"
  )

  add_executable(agent_show_sys_surface_parity_gate
    agent_show_sys_surface_parity_gate.cpp
  )
  target_compile_features(agent_show_sys_surface_parity_gate PRIVATE cxx_std_23)
  target_link_libraries(agent_show_sys_surface_parity_gate
    PRIVATE
      sb_engine_internal_api
  )

  add_test(
    NAME agent_show_sys_surface_parity_gate
    COMMAND agent_show_sys_surface_parity_gate
  )
  set_tests_properties(agent_show_sys_surface_parity_gate PROPERTIES
    LABELS "PFAR-015C;agent_show_sys_surface_parity_gate;database_lifecycle_catalog;management_ui_docs;mga_transaction_regression"
  )

  add_executable(agent_metrics_audit_support_bundle_gate
    agent_metrics_audit_support_bundle_gate.cpp
  )
  target_compile_features(agent_metrics_audit_support_bundle_gate PRIVATE cxx_std_23)
  target_link_libraries(agent_metrics_audit_support_bundle_gate
    PRIVATE
      sb_core_uuid
      sb_engine_internal_api
      sbl_listener_runtime
      sbl_manager_node_runtime
  )

  add_test(
    NAME agent_metrics_audit_support_bundle_gate
    COMMAND agent_metrics_audit_support_bundle_gate
  )
  set_tests_properties(agent_metrics_audit_support_bundle_gate PROPERTIES
    LABELS "PFAR-016;AEIC-042;agent_metrics_audit_support_bundle_gate;database_lifecycle_observability;database_lifecycle_supportability_evidence;support_bundle_redaction;security_negative;management_ui_docs;mga_transaction_regression"
  )

  add_executable(agent_evidence_audit_redaction_retention_gate
    agent_evidence_audit_redaction_retention_gate.cpp
  )
  target_compile_features(agent_evidence_audit_redaction_retention_gate PRIVATE cxx_std_23)
  target_link_libraries(agent_evidence_audit_redaction_retention_gate
    PRIVATE
      sb_core_uuid
      sb_engine_internal_api
  )

  add_test(
    NAME agent_evidence_audit_redaction_retention_gate
    COMMAND agent_evidence_audit_redaction_retention_gate
  )
  set_tests_properties(agent_evidence_audit_redaction_retention_gate PROPERTIES
    LABELS "PFAR-016A;AEIC-042;agent_evidence_audit_redaction_retention_gate;database_lifecycle_observability;database_lifecycle_security;database_lifecycle_supportability_evidence;redaction_retention_tamper;security_negative;management_ui_docs;mga_transaction_regression"
  )

  add_executable(database_lifecycle_catalog_synonym_conformance
    ../conformance/catalog/catalog_synonym_conformance.cpp
  )
  target_compile_features(database_lifecycle_catalog_synonym_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_catalog_synonym_conformance
    PRIVATE
      sb_core_uuid
      sb_engine_internal_api
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_catalog_synonym_conformance
    COMMAND database_lifecycle_catalog_synonym_conformance
  )
  set_tests_properties(database_lifecycle_catalog_synonym_conformance PROPERTIES
    LABELS "database_lifecycle_catalog_synonym_conformance;catalog_sql_object_synonym_semantics_gate;catalog_synonym_object_gate;catalog_synonym_resolution_gate;catalog_synonym_parent_remap_gate;catalog_synonym_depth_cycle_gate;sys_information_schema_synonym_gate;catalog_dependency_api_gate;catalog_diagnostic_vector_gate;database_lifecycle;database_lifecycle_catalog;mga_transaction_regression;public_release_foundation"
  )

  add_executable(database_lifecycle_encryption_key_conformance
    encryption_key_conformance.cpp
  )
  target_compile_features(database_lifecycle_encryption_key_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_encryption_key_conformance
    PRIVATE
      sb_engine_internal_api
  )

  add_test(
    NAME database_lifecycle_encryption_key_conformance
    COMMAND database_lifecycle_encryption_key_conformance
  )
  set_tests_properties(database_lifecycle_encryption_key_conformance PROPERTIES
    LABELS "database_lifecycle_encryption_key_conformance;database_lifecycle_encryption_key;database_lifecycle;database_lifecycle_security;database_lifecycle_filespace;database_lifecycle_shutdown_notification;encryption_at_rest_gate;mga_transaction_regression"
  )

  add_executable(database_lifecycle_executable_object_conformance
    executable_object_conformance.cpp
  )
  target_compile_features(database_lifecycle_executable_object_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_executable_object_conformance
    PRIVATE
      sb_engine_internal_api
  )

  add_test(
    NAME database_lifecycle_executable_object_conformance
    COMMAND database_lifecycle_executable_object_conformance
  )
  set_tests_properties(database_lifecycle_executable_object_conformance PROPERTIES
    LABELS "database_lifecycle_executable_object_conformance;database_lifecycle_executable_object;database_lifecycle;database_lifecycle_security;database_lifecycle_catalog;mga_transaction_regression"
  )

  add_executable(database_lifecycle_security_principal_conformance
    security_principal_conformance.cpp
  )
  target_compile_features(database_lifecycle_security_principal_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_security_principal_conformance
    PRIVATE
      sb_engine_internal_api
  )

  add_test(
    NAME database_lifecycle_security_principal_conformance
    COMMAND database_lifecycle_security_principal_conformance
  )
  set_tests_properties(database_lifecycle_security_principal_conformance PROPERTIES
    LABELS "database_lifecycle_security_principal_conformance;database_lifecycle_security_principal;database_lifecycle;database_lifecycle_security;authorization_privilege_gate;audit_event_gate;security_policy_gate;mga_transaction_regression"
  )

  add_executable(database_lifecycle_security_auth_audit_p4_conformance
    security_auth_audit_p4_conformance.cpp
  )
  target_compile_features(database_lifecycle_security_auth_audit_p4_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_security_auth_audit_p4_conformance
    PRIVATE
      sb_engine_internal_api
  )

  add_test(
    NAME database_lifecycle_security_auth_audit_p4_conformance
    COMMAND database_lifecycle_security_auth_audit_p4_conformance
  )
  set_tests_properties(database_lifecycle_security_auth_audit_p4_conformance PROPERTIES
    LABELS "database_lifecycle_security_auth_audit_p4_conformance;database_lifecycle;database_lifecycle_security;security_auth_gate;audit_event_gate;encryption_at_rest_gate;security_policy_gate;auth_plugin_manifest_gate;mga_transaction_regression"
  )

  add_executable(database_lifecycle_sequence_generator_conformance
    sequence_generator_conformance.cpp
  )
  target_compile_features(database_lifecycle_sequence_generator_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_sequence_generator_conformance
    PRIVATE
      sb_engine_internal_api
  )

  add_test(
    NAME database_lifecycle_sequence_generator_conformance
    COMMAND database_lifecycle_sequence_generator_conformance
  )
  set_tests_properties(database_lifecycle_sequence_generator_conformance PROPERTIES
    LABELS "database_lifecycle_sequence_generator_conformance;database_lifecycle_sequence_generator;database_lifecycle;mga_transaction_regression"
  )

  add_executable(database_lifecycle_index_statistics_plan_conformance
    index_statistics_plan_conformance.cpp
  )
  target_compile_features(database_lifecycle_index_statistics_plan_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_index_statistics_plan_conformance
    PRIVATE
      sb_engine_internal_api
  )

  add_test(
    NAME database_lifecycle_index_statistics_plan_conformance
    COMMAND database_lifecycle_index_statistics_plan_conformance
  )
  set_tests_properties(database_lifecycle_index_statistics_plan_conformance PROPERTIES
    LABELS "database_lifecycle_index_statistics_plan_conformance;FINAL-DEFERRED-IMPLEMENTATION-TRACKER;MDF-023;DEFER-ISO-STATS-OPTIMIZER;DEFER-IMO-MAINTENANCE;DEFER-IDX-MAINTENANCE;DEFER-IDX-METRICS;database_lifecycle_index_statistics_plan_conformance;database_lifecycle_index_statistics_plan;database_lifecycle;database_lifecycle_catalog;mga_transaction_regression"
  )

  add_executable(database_lifecycle_backup_archive_restore_conformance
    backup_archive_restore_conformance.cpp
  )
  target_compile_features(database_lifecycle_backup_archive_restore_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_backup_archive_restore_conformance
    PRIVATE
      sb_engine_internal_api
  )

  add_test(
    NAME database_lifecycle_backup_archive_restore_conformance
    COMMAND database_lifecycle_backup_archive_restore_conformance
  )
  set_tests_properties(database_lifecycle_backup_archive_restore_conformance PROPERTIES
    LABELS "database_lifecycle_backup_archive_restore_conformance;database_lifecycle_backup_archive_restore;database_lifecycle;database_lifecycle_storage;database_lifecycle_filespace;database_lifecycle_shutdown_notification;mga_transaction_regression;backup_restore_manifest_proof_gate;mga_delta_coverage_gate;pitr_rollforward_gate;PRF_G08_BACKUP_DELTA_PITR_READY"
  )
endif()

add_executable(database_lifecycle_plain_value_payload_conformance
  plain_value_payload_conformance.cpp
)
target_compile_features(database_lifecycle_plain_value_payload_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_plain_value_payload_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_plain_value_payload_conformance
  COMMAND database_lifecycle_plain_value_payload_conformance
)
set_tests_properties(database_lifecycle_plain_value_payload_conformance PROPERTIES
  LABELS "EDR-003;EDR-GATE-003;plain_value_payload;database_lifecycle;datatype_commercial_closure_gate"
)

add_executable(database_lifecycle_execution_data_packet_conformance
  execution_data_packet_conformance.cpp
)
target_compile_features(database_lifecycle_execution_data_packet_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_data_packet_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_data_packet_conformance
  COMMAND database_lifecycle_execution_data_packet_conformance
)
set_tests_properties(database_lifecycle_execution_data_packet_conformance PROPERTIES
  LABELS "EDR-004;EDR-GATE-004;execution_data_packet;database_lifecycle;datatype_commercial_closure_gate"
)

add_executable(database_lifecycle_execution_row_batch_conformance
  execution_row_batch_conformance.cpp
)
target_compile_features(database_lifecycle_execution_row_batch_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_row_batch_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_row_batch_conformance
  COMMAND database_lifecycle_execution_row_batch_conformance
)
set_tests_properties(database_lifecycle_execution_row_batch_conformance PROPERTIES
  LABELS "EDR-005;EDR-GATE-005;execution_row_batch;database_lifecycle;datatype_commercial_closure_gate"
)

add_executable(database_lifecycle_execution_result_envelope_conformance
  execution_result_envelope_conformance.cpp
)
target_compile_features(database_lifecycle_execution_result_envelope_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_result_envelope_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_result_envelope_conformance
  COMMAND database_lifecycle_execution_result_envelope_conformance
)
set_tests_properties(database_lifecycle_execution_result_envelope_conformance PROPERTIES
  LABELS "EDR-006;EDR-GATE-006;execution_result_envelope;database_lifecycle;datatype_commercial_closure_gate"
)

add_executable(database_lifecycle_execution_coercion_context_conformance
  execution_coercion_context_conformance.cpp
)
target_compile_features(database_lifecycle_execution_coercion_context_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_coercion_context_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_coercion_context_conformance
  COMMAND database_lifecycle_execution_coercion_context_conformance
)
set_tests_properties(database_lifecycle_execution_coercion_context_conformance PROPERTIES
  LABELS "EDR-007;EDR-GATE-007;execution_coercion_context;database_lifecycle;datatype_commercial_closure_gate"
)

add_executable(database_lifecycle_execution_text_temporal_semantics_conformance
  execution_text_temporal_semantics_conformance.cpp
)
target_compile_features(database_lifecycle_execution_text_temporal_semantics_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_text_temporal_semantics_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_text_temporal_semantics_conformance
  COMMAND database_lifecycle_execution_text_temporal_semantics_conformance
)
set_tests_properties(database_lifecycle_execution_text_temporal_semantics_conformance PROPERTIES
  LABELS "EDR-008;EDR-GATE-008;execution_text_temporal_semantics;database_lifecycle;datatype_commercial_closure_gate"
)

add_executable(database_lifecycle_execution_large_value_handle_conformance
  execution_large_value_handle_conformance.cpp
)
target_compile_features(database_lifecycle_execution_large_value_handle_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_large_value_handle_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_large_value_handle_conformance
  COMMAND database_lifecycle_execution_large_value_handle_conformance
)
set_tests_properties(database_lifecycle_execution_large_value_handle_conformance PROPERTIES
  LABELS "EDR-009;EDR-GATE-009;execution_large_value_handle;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_protected_value_conformance
  execution_protected_value_conformance.cpp
)
target_compile_features(database_lifecycle_execution_protected_value_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_protected_value_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_protected_value_conformance
  COMMAND database_lifecycle_execution_protected_value_conformance
)
set_tests_properties(database_lifecycle_execution_protected_value_conformance PROPERTIES
  LABELS "EDR-010;EDR-GATE-010;execution_protected_value;database_lifecycle;datatype_commercial_closure_gate;security_negative"
)

add_executable(database_lifecycle_execution_udr_bridge_value_conformance
  execution_udr_bridge_value_conformance.cpp
)
target_compile_features(database_lifecycle_execution_udr_bridge_value_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_udr_bridge_value_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_udr_bridge_value_conformance
  COMMAND database_lifecycle_execution_udr_bridge_value_conformance
)
set_tests_properties(database_lifecycle_execution_udr_bridge_value_conformance PROPERTIES
  LABELS "EDR-011;EDR-GATE-011;execution_udr_bridge_value;database_lifecycle;datatype_commercial_closure_gate"
)

add_executable(database_lifecycle_execution_index_cache_identity_conformance
  execution_index_cache_identity_conformance.cpp
)
target_compile_features(database_lifecycle_execution_index_cache_identity_conformance PRIVATE cxx_std_20)
target_link_libraries(database_lifecycle_execution_index_cache_identity_conformance
  PRIVATE
    sb_core_index
    sb_engine_executor
)

add_test(
  NAME database_lifecycle_execution_index_cache_identity_conformance
  COMMAND database_lifecycle_execution_index_cache_identity_conformance
)
set_tests_properties(database_lifecycle_execution_index_cache_identity_conformance PROPERTIES
  LABELS "EDR-012;EDR-GATE-012;execution_index_cache_identity;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_terminology_conformance
  execution_terminology_conformance.cpp
  "${PROJECT_SOURCE_DIR}/src/server/product_identity.cpp"
)
target_compile_features(database_lifecycle_execution_terminology_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_terminology_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src/server"
)

add_test(
  NAME database_lifecycle_execution_terminology_conformance
  COMMAND database_lifecycle_execution_terminology_conformance
)
set_tests_properties(database_lifecycle_execution_terminology_conformance PROPERTIES
  LABELS "EDR-013;EDR-GATE-013;execution_terminology;database_lifecycle;datatype_commercial_closure_gate"
)

add_test(
  NAME database_lifecycle_execution_data_gate_matrix_conformance
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/execution_data_gate_matrix_conformance.py"
          --source-root "${PROJECT_SOURCE_DIR}"
          --build-root "${CMAKE_BINARY_DIR}"
)
set_tests_properties(database_lifecycle_execution_data_gate_matrix_conformance PROPERTIES
  LABELS "EDR-014;EDR-GATE-014;execution_data_gate_matrix;database_lifecycle;datatype_commercial_closure_gate"
)

add_executable(database_lifecycle_execution_relation_descriptor_conformance
  execution_relation_descriptor_conformance.cpp
)
target_compile_features(database_lifecycle_execution_relation_descriptor_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_relation_descriptor_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_relation_descriptor_conformance
  COMMAND database_lifecycle_execution_relation_descriptor_conformance
)
set_tests_properties(database_lifecycle_execution_relation_descriptor_conformance PROPERTIES
  LABELS "EDR-015;EDR-GATE-015;execution_relation_descriptor;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_cursor_handle_conformance
  execution_cursor_handle_conformance.cpp
)
target_compile_features(database_lifecycle_execution_cursor_handle_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_cursor_handle_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_cursor_handle_conformance
  COMMAND database_lifecycle_execution_cursor_handle_conformance
)
set_tests_properties(database_lifecycle_execution_cursor_handle_conformance PROPERTIES
  LABELS "EDR-016;EDR-GATE-016;execution_cursor_handle;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_rowset_value_conformance
  execution_rowset_value_conformance.cpp
)
target_compile_features(database_lifecycle_execution_rowset_value_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_rowset_value_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_rowset_value_conformance
  COMMAND database_lifecycle_execution_rowset_value_conformance
)
set_tests_properties(database_lifecycle_execution_rowset_value_conformance PROPERTIES
  LABELS "EDR-017;EDR-GATE-017;execution_rowset_value;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_table_value_conformance
  execution_table_value_conformance.cpp
)
target_compile_features(database_lifecycle_execution_table_value_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_table_value_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_table_value_conformance
  COMMAND database_lifecycle_execution_table_value_conformance
)
set_tests_properties(database_lifecycle_execution_table_value_conformance PROPERTIES
  LABELS "EDR-018;EDR-GATE-018;execution_table_value;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_routine_signature_descriptor_conformance
  execution_routine_signature_descriptor_conformance.cpp
)
target_compile_features(database_lifecycle_execution_routine_signature_descriptor_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_routine_signature_descriptor_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_routine_signature_descriptor_conformance
  COMMAND database_lifecycle_execution_routine_signature_descriptor_conformance
)
set_tests_properties(database_lifecycle_execution_routine_signature_descriptor_conformance PROPERTIES
  LABELS "EDR-019;EDR-GATE-019;execution_routine_signature_descriptor;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_ownership_transfer_conformance
  execution_ownership_transfer_conformance.cpp
)
target_compile_features(database_lifecycle_execution_ownership_transfer_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_ownership_transfer_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_ownership_transfer_conformance
  COMMAND database_lifecycle_execution_ownership_transfer_conformance
)
set_tests_properties(database_lifecycle_execution_ownership_transfer_conformance PROPERTIES
  LABELS "EDR-020;EDR-GATE-020;execution_ownership_transfer;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_result_channel_descriptor_conformance
  execution_result_channel_descriptor_conformance.cpp
)
target_compile_features(database_lifecycle_execution_result_channel_descriptor_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_result_channel_descriptor_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_result_channel_descriptor_conformance
  COMMAND database_lifecycle_execution_result_channel_descriptor_conformance
)
set_tests_properties(database_lifecycle_execution_result_channel_descriptor_conformance PROPERTIES
  LABELS "EDR-021;EDR-GATE-021;execution_result_channel_descriptor;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_optimizer_safety_conformance
  execution_optimizer_safety_conformance.cpp
)
target_compile_features(database_lifecycle_execution_optimizer_safety_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_optimizer_safety_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_optimizer_safety_conformance
  COMMAND database_lifecycle_execution_optimizer_safety_conformance
)
set_tests_properties(database_lifecycle_execution_optimizer_safety_conformance PROPERTIES
  LABELS "EDR-022;EDR-GATE-022;execution_optimizer_safety;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_distributed_handle_conformance
  execution_distributed_handle_conformance.cpp
)
target_compile_features(database_lifecycle_execution_distributed_handle_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_distributed_handle_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_distributed_handle_conformance
  COMMAND database_lifecycle_execution_distributed_handle_conformance
)
set_tests_properties(database_lifecycle_execution_distributed_handle_conformance PROPERTIES
  LABELS "EDR-023;EDR-GATE-023;execution_distributed_handle;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_domain_descriptor_conformance
  execution_domain_descriptor_conformance.cpp
)
target_compile_features(database_lifecycle_execution_domain_descriptor_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_domain_descriptor_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_domain_descriptor_conformance
  COMMAND database_lifecycle_execution_domain_descriptor_conformance
)
set_tests_properties(database_lifecycle_execution_domain_descriptor_conformance PROPERTIES
  LABELS "EDR-024;EDR-GATE-024;execution_domain_descriptor;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_domain_element_path_conformance
  execution_domain_element_path_conformance.cpp
)
target_compile_features(database_lifecycle_execution_domain_element_path_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_domain_element_path_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_domain_element_path_conformance
  COMMAND database_lifecycle_execution_domain_element_path_conformance
)
set_tests_properties(database_lifecycle_execution_domain_element_path_conformance PROPERTIES
  LABELS "EDR-025;EDR-GATE-025;execution_domain_element_path;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_domain_cast_operation_conformance
  execution_domain_cast_operation_conformance.cpp
)
target_compile_features(database_lifecycle_execution_domain_cast_operation_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_domain_cast_operation_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_domain_cast_operation_conformance
  COMMAND database_lifecycle_execution_domain_cast_operation_conformance
)
set_tests_properties(database_lifecycle_execution_domain_cast_operation_conformance PROPERTIES
  LABELS "EDR-026;EDR-GATE-026;execution_domain_cast_operation;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_type_modifier_canonicalization_conformance
  execution_type_modifier_canonicalization_conformance.cpp
)
target_compile_features(database_lifecycle_execution_type_modifier_canonicalization_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_type_modifier_canonicalization_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_type_modifier_canonicalization_conformance
  COMMAND database_lifecycle_execution_type_modifier_canonicalization_conformance
)
set_tests_properties(database_lifecycle_execution_type_modifier_canonicalization_conformance PROPERTIES
  LABELS "EDR-027;EDR-GATE-027;execution_type_modifier_canonicalization;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_enum_set_representation_conformance
  execution_enum_set_representation_conformance.cpp
)
target_compile_features(database_lifecycle_execution_enum_set_representation_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_enum_set_representation_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_enum_set_representation_conformance
  COMMAND database_lifecycle_execution_enum_set_representation_conformance
)
set_tests_properties(database_lifecycle_execution_enum_set_representation_conformance PROPERTIES
  LABELS "EDR-028;EDR-GATE-028;execution_enum_set_representation;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_range_representation_conformance
  execution_range_representation_conformance.cpp
)
target_compile_features(database_lifecycle_execution_range_representation_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_range_representation_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_range_representation_conformance
  COMMAND database_lifecycle_execution_range_representation_conformance
)
set_tests_properties(database_lifecycle_execution_range_representation_conformance PROPERTIES
  LABELS "EDR-029;EDR-GATE-029;execution_range_representation;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_container_representation_conformance
  execution_container_representation_conformance.cpp
)
target_compile_features(database_lifecycle_execution_container_representation_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_container_representation_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_container_representation_conformance
  COMMAND database_lifecycle_execution_container_representation_conformance
)
set_tests_properties(database_lifecycle_execution_container_representation_conformance PROPERTIES
  LABELS "EDR-030;EDR-GATE-030;execution_container_representation;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_variant_representation_conformance
  execution_variant_representation_conformance.cpp
)
target_compile_features(database_lifecycle_execution_variant_representation_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_variant_representation_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_variant_representation_conformance
  COMMAND database_lifecycle_execution_variant_representation_conformance
)
set_tests_properties(database_lifecycle_execution_variant_representation_conformance PROPERTIES
  LABELS "EDR-031;EDR-GATE-031;execution_variant_representation;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_polymorphic_routine_binding_conformance
  execution_polymorphic_routine_binding_conformance.cpp
)
target_compile_features(database_lifecycle_execution_polymorphic_routine_binding_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_polymorphic_routine_binding_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_polymorphic_routine_binding_conformance
  COMMAND database_lifecycle_execution_polymorphic_routine_binding_conformance
)
set_tests_properties(database_lifecycle_execution_polymorphic_routine_binding_conformance PROPERTIES
  LABELS "EDR-032;EDR-GATE-032;execution_polymorphic_routine_binding;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_comparison_key_representation_conformance
  execution_comparison_key_representation_conformance.cpp
)
target_compile_features(database_lifecycle_execution_comparison_key_representation_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_comparison_key_representation_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_comparison_key_representation_conformance
  COMMAND database_lifecycle_execution_comparison_key_representation_conformance
)
set_tests_properties(database_lifecycle_execution_comparison_key_representation_conformance PROPERTIES
  LABELS "EDR-033;EDR-GATE-033;execution_comparison_key_representation;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_numeric_edge_policy_conformance
  execution_numeric_edge_policy_conformance.cpp
)
target_compile_features(database_lifecycle_execution_numeric_edge_policy_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_numeric_edge_policy_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_numeric_edge_policy_conformance
  COMMAND database_lifecycle_execution_numeric_edge_policy_conformance
)
set_tests_properties(database_lifecycle_execution_numeric_edge_policy_conformance PROPERTIES
  LABELS "EDR-034;EDR-GATE-034;execution_numeric_edge_policy;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_descriptor_migration_conformance
  execution_descriptor_migration_conformance.cpp
)
target_compile_features(database_lifecycle_execution_descriptor_migration_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_descriptor_migration_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_descriptor_migration_conformance
  COMMAND database_lifecycle_execution_descriptor_migration_conformance
)
set_tests_properties(database_lifecycle_execution_descriptor_migration_conformance PROPERTIES
  LABELS "EDR-035;EDR-GATE-035;execution_descriptor_migration;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_reference_type_capability_conformance
  execution_reference_type_capability_conformance.cpp
)
target_compile_features(database_lifecycle_execution_reference_type_capability_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_reference_type_capability_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_reference_type_capability_conformance
  COMMAND database_lifecycle_execution_reference_type_capability_conformance
)
set_tests_properties(database_lifecycle_execution_reference_type_capability_conformance PROPERTIES
  LABELS "EDR-037;EDR-GATE-037;DTC-REFERENCE-TYPE-COVERAGE;DTC-REFERENCE-UDR-DOMAIN-INSTALL;DTC-LLVM-DOMAIN-ACCELERATION;DTC-LITERAL-BIND-TYPING;DTC-TYPE-INFERENCE-OVERLOAD;DTC-LOCATOR-PSEUDOTYPE-SAFETY;DTC-OPAQUE-TYPE-LIFECYCLE;DTC-TYPE-MIGRATION;DTC-REFERENCE-SUPERIORITY;DTC-GATE-001;DTC-GATE-002;DTC-GATE-003;DTC-GATE-004;DTC-GATE-005;DTC-GATE-006;DTC-GATE-007;DTC-GATE-008;DTC-GATE-009;DTC-GATE-010;DTC-GATE-011;DTC-GATE-012;execution_reference_type_capability;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_type_operation_registry_conformance
  type_operation_registry_conformance.cpp
)
target_compile_features(database_lifecycle_type_operation_registry_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_type_operation_registry_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_type_operation_registry_conformance
  COMMAND database_lifecycle_type_operation_registry_conformance
)
set_tests_properties(database_lifecycle_type_operation_registry_conformance PROPERTIES
  LABELS "TOR-TYPE-OPERATION-REGISTRY;TOR-OPERATION-DESCRIPTOR-FIELDS;TOR-OPERATION-REGISTRATION;TOR-OVERLOAD-RESOLUTION;TOR-CAST-REGISTRY;TOR-AGGREGATE-WINDOW-STATE;TOR-SBLR-OPERATION-BINDING;TOR-GATE-001;TOR-GATE-002;TOR-GATE-003;TOR-GATE-004;TOR-GATE-005;TOR-GATE-006;TOR-GATE-007;type_operation_registry;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_type_index_stats_optimizer_conformance
  type_index_stats_optimizer_conformance.cpp
)
target_compile_features(database_lifecycle_type_index_stats_optimizer_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_type_index_stats_optimizer_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_type_index_stats_optimizer_conformance
  COMMAND database_lifecycle_type_index_stats_optimizer_conformance
)
set_tests_properties(database_lifecycle_type_index_stats_optimizer_conformance PROPERTIES
  LABELS "TIO-TYPE-INDEX-STATS-OPTIMIZER;TIO-CANONICAL-COMPARISON-CONTRACT;TIO-CANONICALIZATION-PROFILE;TIO-INDEX-COMPATIBILITY-DESCRIPTOR;TIO-INDEX-FAMILY-MATRIX;TIO-TYPE-STATISTICS-DESCRIPTOR;TIO-SELECTIVITY-MODEL;TIO-OPTIMIZER-ADMISSION;TIO-GATE-001;TIO-GATE-002;TIO-GATE-003;TIO-GATE-004;TIO-GATE-005;TIO-GATE-006;TIO-GATE-007;type_index_stats_optimizer;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_type_metadata_diagnostics_driver_conformance
  type_metadata_diagnostics_driver_conformance.cpp
)
target_compile_features(database_lifecycle_type_metadata_diagnostics_driver_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_type_metadata_diagnostics_driver_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_type_metadata_diagnostics_driver_conformance
  COMMAND database_lifecycle_type_metadata_diagnostics_driver_conformance
)
set_tests_properties(database_lifecycle_type_metadata_diagnostics_driver_conformance PROPERTIES
  LABELS "TMD-TYPE-METADATA-DIAGNOSTICS-DRIVER;TMD-CATALOG-EXPOSURE-MODEL;TMD-DRIVER-TYPE-METADATA-DESCRIPTOR;TMD-DIAGNOSTIC-COMPATIBILITY-MODEL;TMD-BACKUP-RESTORE-TYPE-PROFILE;TMD-REPLICATION-TYPE-PROFILE;TMD-CLUSTER-TYPE-TRANSPORT-PROFILE;TMD-UNSUPPORTED-DEGRADED-CONTRACT;TMD-TYPE-TESTING-CORPUS;TMD-GATE-001;TMD-GATE-002;TMD-GATE-003;TMD-GATE-004;TMD-GATE-005;TMD-GATE-006;TMD-GATE-007;TMD-GATE-008;type_metadata_diagnostics_driver;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_canonical_scalar_datatypes_conformance
  canonical_scalar_datatypes_conformance.cpp
)
target_compile_features(database_lifecycle_canonical_scalar_datatypes_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_canonical_scalar_datatypes_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_canonical_scalar_datatypes_conformance
  COMMAND database_lifecycle_canonical_scalar_datatypes_conformance
)
set_tests_properties(database_lifecycle_canonical_scalar_datatypes_conformance PROPERTIES
  LABELS "CSD-CANONICAL-SCALAR-DATATYPES;CSD-SCALAR-AUTHORITY-RULE;CSD-SCALAR-DESCRIPTOR-FIELDS;CSD-BOOLEAN;CSD-SIGNED-INTEGER;CSD-UNSIGNED-INTEGER;CSD-EXACT-NUMERIC;CSD-DECIMAL-FLOATING;CSD-APPROXIMATE-NUMERIC;CSD-FLOAT128-BINARY128;CSD-MONEY-CURRENCY;CSD-SCALAR-LITERAL-BIND-RESOLUTION;CSD-SCALAR-CAST-RANKING;CSD-SCALAR-OPERATION-FAMILIES;CSD-SCALAR-BACKUP-REPLICATION-TRANSPORT;CSD-SCALAR-DIAGNOSTICS;CSD-GATE-001;CSD-GATE-002;CSD-GATE-003;CSD-GATE-004;CSD-GATE-005;CSD-GATE-006;CSD-GATE-007;CSD-GATE-008;CSD-GATE-009;CSD-GATE-010;CSD-GATE-011;SCALAR-CONF-001;SCALAR-CONF-002;SCALAR-CONF-003;SCALAR-CONF-004;SCALAR-CONF-005;SCALAR-CONF-006;SCALAR-CONF-007;SCALAR-CONF-008;SCALAR-CONF-009;SCALAR-CONF-010;float128_backend_unavailable;float128_raw_host_encoding_refused;canonical_scalar_datatypes;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_native_vs_domain_datatype_decisions_conformance
  native_vs_domain_datatype_decisions_conformance.cpp
)
target_compile_features(database_lifecycle_native_vs_domain_datatype_decisions_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_native_vs_domain_datatype_decisions_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_native_vs_domain_datatype_decisions_conformance
  COMMAND database_lifecycle_native_vs_domain_datatype_decisions_conformance
)
set_tests_properties(database_lifecycle_native_vs_domain_datatype_decisions_conformance PROPERTIES
  LABELS "NVD-NATIVE-VS-DOMAIN-DATATYPE-DECISIONS;NVD-CORE-DECISION;NVD-NATIVE-PROMOTION-CRITERIA;NVD-DOMAIN-ONLY-CRITERIA;NVD-REQUIRED-NATIVE-SUBSTRATE-FAMILIES;NVD-DOMAIN-ONLY-FAMILIES;NVD-OPAQUE-UDR-BRIDGE-FAMILIES;NVD-REFERENCE-FAMILY-DECISIONS;NVD-FULL-SUPPORT-RULE;NVD-GATE-001;NVD-GATE-002;NVD-GATE-003;NVD-GATE-004;NVD-CONF-001;NVD-CONF-002;NVD-CONF-003;NVD-CONF-004;NVD-CONF-005;NVD-CONF-006;NVD-CONF-007;NVD-CONF-008;NVD-CONF-009;NVD-CONF-010;NVD-CONF-011;NVD-CONF-012;NVD-CONF-013;NVD-CONF-014;NVD-CONF-015;native_vs_domain_datatype_decisions;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_canonical_datatype_families_conformance
  canonical_datatype_families_conformance.cpp
)
target_compile_features(database_lifecycle_canonical_datatype_families_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_canonical_datatype_families_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_canonical_datatype_families_conformance
  COMMAND database_lifecycle_canonical_datatype_families_conformance
)
set_tests_properties(database_lifecycle_canonical_datatype_families_conformance PROPERTIES
  LABELS "CDT-CANONICAL-DATATYPE-FAMILY-IMPLEMENTATION;CTB-CANONICAL-TEXT-BINARY-BIT-DATATYPES;CTI-CANONICAL-TEMPORAL-INTERVAL-DATATYPES;CINL-CANONICAL-IDENTITY-NETWORK-LOB-DATATYPES;CSCR-CANONICAL-STRUCTURED-CONTAINER-RANGE-DATATYPES;CDSSV-CANONICAL-DOCUMENT-SEARCH-SPATIAL-VECTOR-DATATYPES;CGTC-CANONICAL-GRAPH-TIMESERIES-COLUMNAR-DATATYPES;CASLO-CANONICAL-AGGREGATE-SKETCH-LOCATOR-OPAQUE-DATATYPES;CTB-GATE-001;CTB-GATE-002;CTB-GATE-003;CTB-GATE-004;CTI-GATE-001;CTI-GATE-002;CTI-GATE-003;CTI-GATE-004;CINL-GATE-001;CINL-GATE-002;CINL-GATE-003;CINL-GATE-004;CSCR-GATE-001;CSCR-GATE-002;CSCR-GATE-003;CSCR-GATE-004;CDSSV-GATE-001;CDSSV-GATE-002;CDSSV-GATE-003;CDSSV-GATE-004;CGTC-GATE-001;CGTC-GATE-002;CGTC-GATE-003;CGTC-GATE-004;CASLO-GATE-001;CASLO-GATE-002;CASLO-GATE-003;CASLO-GATE-004;CTB-CONF-001;CTB-CONF-009;CTI-CONF-001;CTI-CONF-011;CINL-CONF-001;CINL-CONF-008;CSCR-CONF-001;CSCR-CONF-013;CDSSV-CONF-001;CDSSV-CONF-011;CGTC-CONF-001;CGTC-CONF-010;CASLO-CONF-001;CASLO-CONF-011;canonical_datatype_families;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_execution_detail_closure_matrix_conformance
  execution_detail_closure_matrix_conformance.cpp
)
target_compile_features(database_lifecycle_execution_detail_closure_matrix_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_execution_detail_closure_matrix_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_execution_detail_closure_matrix_conformance
  COMMAND database_lifecycle_execution_detail_closure_matrix_conformance
)
set_tests_properties(database_lifecycle_execution_detail_closure_matrix_conformance PROPERTIES
  LABELS "EDC-DETAIL-CLOSURE-MATRICES;EDC-CANONICAL-TYPE-FAMILY-INVENTORY;EDC-REFERENCE-TYPE-COVERAGE-MATRIX;EDC-OPERATION-REGISTRY-SEED-MATRIX;EDC-CAST-REGISTRY-SEED-MATRIX;EDC-AGGREGATE-WINDOW-SEED-MATRIX;EDC-INDEX-FAMILY-COMPATIBILITY-MATRIX;EDC-STATISTICS-SELECTIVITY-SEED-MATRIX;EDC-DRIVER-METADATA-SEED-MATRIX;EDC-BACKUP-REPLICATION-TRANSPORT-MATRIX;EDC-SYSTEM-CATALOG-SEED-CONTRACTS;EDC-CONFORMANCE-CORPUS-SEED-MATRIX;EDC-GATE-001;EDC-GATE-002;EDC-GATE-003;EDC-GATE-004;EDC-GATE-005;EDC-GATE-006;EDC-GATE-007;EDC-GATE-008;EDC-GATE-009;EDC-GATE-010;EDC-GATE-011;EDC-GATE-012;EDC-GATE-013;EDC-GATE-014;EDC-GATE-015;EDC-GATE-016;EDC-GATE-017;execution_detail_closure_matrix;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_remaining_detail_closure_controls_conformance
  remaining_detail_closure_controls_conformance.cpp
)
target_compile_features(database_lifecycle_remaining_detail_closure_controls_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_remaining_detail_closure_controls_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_remaining_detail_closure_controls_conformance
  COMMAND database_lifecycle_remaining_detail_closure_controls_conformance
)
set_tests_properties(database_lifecycle_remaining_detail_closure_controls_conformance PROPERTIES
  LABELS "RDC-REMAINING-DETAIL-CLOSURE-CONTROLS;RDC-001;RDC-002;RDC-003;RDC-004;RDC-005;RDC-006;RDC-007;RDC-008;RDC-009;RDC-010;RDC-011;RDC-012;RDC-013;RDC-014;RDC-015;RDC-016;SCS-SYSTEM-CATALOG-SCHEMA;TSP-TYPE-SECURITY-PRIVILEGE-MATRIX;DER-DIAGNOSTIC-ERROR-CODE-REGISTRY;RRV-RESOURCE-REGISTRY-VERSIONING;PSG-PARSER-SBLR-GRAMMAR-EXPANSION;DSM-REFERENCE-SPECIFIC-TYPE-MATRICES;CEE-CANONICAL-ENCODING-EXAMPLES;CMM-COMPATIBILITY-MODE-MATRIX;CTM-CONFORMANCE-TEST-MANIFEST-FORMAT;DEP-DOCUMENTATION-EXAMPLES-POLICY;SCT-SYSTEM-CATALOG-TABLE-DEFINITIONS;PSF-PARSER-SBLR-FORMAL-GRAMMAR;NEE-NORMATIVE-ENCODING-EXAMPLES;CMI-CONFORMANCE-MANIFEST-INVENTORY;DVP-REFERENCE-VERSION-PROFILE-CLOSURE;CGC-COMMERCIAL-GRADE-COMPLETION-GATES;SCS-GATE-001;SCS-GATE-005;TSP-GATE-001;TSP-GATE-004;DER-GATE-001;DER-GATE-013;RRV-GATE-001;RRV-GATE-004;RRV-CONF-001;RRV-CONF-012;PSG-GATE-001;PSG-GATE-003;DSM-GATE-001;DSM-GATE-013;CEE-GATE-001;CEE-CONF-001;CEE-CONF-007;CMM-GATE-001;CMM-GATE-013;CTM-GATE-001;CTM-GATE-013;DEP-GATE-001;DEP-GATE-008;SCT-GATE-001;SCT-GATE-004;PSF-GATE-001;PSF-GATE-004;PSF-CONF-001;PSF-CONF-012;NEE-GATE-001;NEE-GATE-005;CMI-GATE-001;CMI-GATE-012;DVP-GATE-001;DVP-GATE-010;CGC-GATE-001;CGC-GATE-025;remaining_detail_closure_controls;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

add_executable(database_lifecycle_commercial_grade_completion_evidence_conformance
  commercial_grade_completion_evidence_conformance.cpp
)
target_compile_features(database_lifecycle_commercial_grade_completion_evidence_conformance PRIVATE cxx_std_20)
target_include_directories(database_lifecycle_commercial_grade_completion_evidence_conformance
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

add_test(
  NAME database_lifecycle_commercial_grade_completion_evidence_conformance
  COMMAND database_lifecycle_commercial_grade_completion_evidence_conformance
)
set_tests_properties(database_lifecycle_commercial_grade_completion_evidence_conformance PROPERTIES
  LABELS "CGC-COMMERCIAL-GRADE-COMPLETION-GATES;CGC-SPECIFICATION-COMPLETE-GATES;CGC-IMPLEMENTATION-MAPPED-GATES;CGC-FINAL-CONTRADICTION-CHECKLIST;CGC-AUDIT-MATRIX;CGC-GATE-001;CGC-GATE-002;CGC-GATE-003;CGC-GATE-004;CGC-GATE-005;CGC-GATE-006;CGC-GATE-007;CGC-GATE-008;CGC-GATE-009;CGC-GATE-010;CGC-GATE-011;CGC-GATE-012;CGC-GATE-013;CGC-GATE-014;CGC-GATE-015;CGC-GATE-016;CGC-GATE-017;CGC-GATE-018;CGC-GATE-019;CGC-GATE-020;CGC-GATE-021;CGC-GATE-022;CGC-GATE-023;CGC-GATE-024;CGC-GATE-025;commercial_grade_completion_evidence;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
)

if(TARGET sb_engine_internal_api)
  add_executable(database_lifecycle_execution_value_state_conformance
    execution_value_state_conformance.cpp
  )
  target_compile_features(database_lifecycle_execution_value_state_conformance PRIVATE cxx_std_20)
  target_include_directories(database_lifecycle_execution_value_state_conformance
    PRIVATE
      "${PROJECT_SOURCE_DIR}/include"
  )
  target_link_libraries(database_lifecycle_execution_value_state_conformance
    PRIVATE
      sb_engine_internal_api
  )

  add_test(
    NAME database_lifecycle_execution_value_state_conformance
    COMMAND database_lifecycle_execution_value_state_conformance
  )
  set_tests_properties(database_lifecycle_execution_value_state_conformance PROPERTIES
    LABELS "EDR-002;EDR-GATE-002;execution_value_state;database_lifecycle;datatype_commercial_closure_gate"
  )
endif()

if(TARGET sb_core_datatypes)
  add_executable(database_lifecycle_execution_type_descriptor_conformance
    execution_type_descriptor_conformance.cpp
  )
  target_compile_features(database_lifecycle_execution_type_descriptor_conformance PRIVATE cxx_std_20)
  target_link_libraries(database_lifecycle_execution_type_descriptor_conformance
    PRIVATE
      sb_core_datatypes
      sb_core_platform
  )

  add_test(
    NAME database_lifecycle_execution_type_descriptor_conformance
    COMMAND database_lifecycle_execution_type_descriptor_conformance
  )
  set_tests_properties(database_lifecycle_execution_type_descriptor_conformance PROPERTIES
    LABELS "EDR-001;EDR-GATE-001;execution_type_descriptor;database_lifecycle;datatype_commercial_closure_gate"
  )

  add_executable(database_lifecycle_current_core_reference_neutral_metadata_conformance
    current_core_reference_neutral_metadata_conformance.cpp
  )
  target_compile_features(database_lifecycle_current_core_reference_neutral_metadata_conformance PRIVATE cxx_std_17)
  target_link_libraries(database_lifecycle_current_core_reference_neutral_metadata_conformance
    PRIVATE
      sb_core_datatypes
      sb_core_platform
  )

  add_test(
    NAME database_lifecycle_current_core_reference_neutral_metadata_conformance
    COMMAND database_lifecycle_current_core_reference_neutral_metadata_conformance
  )
  set_tests_properties(database_lifecycle_current_core_reference_neutral_metadata_conformance PROPERTIES
    LABELS "database_lifecycle_current_core_reference_neutral_metadata_conformance;FINAL-DEFERRED-IMPLEMENTATION-TRACKER;MDF-026;DEFER-DTYPE-REFERENCE-GATE-IMPLEMENTATION;DEFER-DPE-REFERENCE-MAPPING;DEFER-DTM-REFERENCE-METRICS;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
  )

  add_executable(database_lifecycle_current_core_datatype_catalog_manifest_gate
    current_core_datatype_catalog_manifest_gate.cpp
  )
  target_compile_features(database_lifecycle_current_core_datatype_catalog_manifest_gate PRIVATE cxx_std_17)
  target_link_libraries(database_lifecycle_current_core_datatype_catalog_manifest_gate
    PRIVATE
      sb_core_datatypes
      sb_core_platform
  )

  add_test(
    NAME database_lifecycle_current_core_datatype_catalog_manifest_gate
    COMMAND database_lifecycle_current_core_datatype_catalog_manifest_gate
  )
  set_tests_properties(database_lifecycle_current_core_datatype_catalog_manifest_gate PROPERTIES
    LABELS "database_lifecycle_current_core_datatype_catalog_manifest_gate;FINAL-DEFERRED-IMPLEMENTATION-TRACKER;MDF-012;MDF-012-CURRENT-CORE-DATATYPE-CATALOG-MANIFEST;DEFER-DTYPE-DESCRIPTOR-IMPLEMENTATION;DEFER-DTYPE-CATALOG-DDL;DEFER-DTYPE-CLOSURE-MATRIX-TRACE;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
  )

  add_executable(database_lifecycle_current_core_datatype_physical_encoding_gate
    current_core_datatype_physical_encoding_gate.cpp
  )
  target_compile_features(database_lifecycle_current_core_datatype_physical_encoding_gate PRIVATE cxx_std_17)
  target_link_libraries(database_lifecycle_current_core_datatype_physical_encoding_gate
    PRIVATE
      sb_core_datatypes
      sb_core_platform
  )

  add_test(
    NAME database_lifecycle_current_core_datatype_physical_encoding_gate
    COMMAND database_lifecycle_current_core_datatype_physical_encoding_gate
  )
  set_tests_properties(database_lifecycle_current_core_datatype_physical_encoding_gate PROPERTIES
    LABELS "database_lifecycle_current_core_datatype_physical_encoding_gate;FINAL-DEFERRED-IMPLEMENTATION-TRACKER;MDF-013;MDF-013-CURRENT-CORE-DATATYPE-PHYSICAL-ENCODING;DEFER-DPE-ENCODER-DECODER;DEFER-DPE-IN-PAGE-LAYOUT;DEFER-DPE-OVERFLOW-LAYOUT;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
  )

  add_executable(database_lifecycle_current_core_datatype_comparison_cast_gate
    current_core_datatype_comparison_cast_gate.cpp
  )
  target_compile_features(database_lifecycle_current_core_datatype_comparison_cast_gate PRIVATE cxx_std_17)
  target_link_libraries(database_lifecycle_current_core_datatype_comparison_cast_gate
    PRIVATE
      sb_core_datatypes
      sb_core_platform
  )

  add_test(
    NAME database_lifecycle_current_core_datatype_comparison_cast_gate
    COMMAND database_lifecycle_current_core_datatype_comparison_cast_gate
  )
  set_tests_properties(database_lifecycle_current_core_datatype_comparison_cast_gate PROPERTIES
    LABELS "database_lifecycle_current_core_datatype_comparison_cast_gate;FINAL-DEFERRED-IMPLEMENTATION-TRACKER;MDF-014;MDF-014-CURRENT-CORE-DATATYPE-COMPARISON-CASTS;DEFER-DPE-COMPARISON-KEYS;DEFER-DPE-CAST-STORAGE;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
  )

  add_executable(database_lifecycle_current_core_datatype_metrics_redaction_gate
    current_core_datatype_metrics_redaction_gate.cpp
  )
  target_compile_features(database_lifecycle_current_core_datatype_metrics_redaction_gate PRIVATE cxx_std_17)
  target_link_libraries(database_lifecycle_current_core_datatype_metrics_redaction_gate
    PRIVATE
      sb_core_datatypes
      sb_core_metrics
      sb_core_platform
  )

  add_test(
    NAME database_lifecycle_current_core_datatype_metrics_redaction_gate
    COMMAND database_lifecycle_current_core_datatype_metrics_redaction_gate
  )
  set_tests_properties(database_lifecycle_current_core_datatype_metrics_redaction_gate PROPERTIES
    LABELS "database_lifecycle_current_core_datatype_metrics_redaction_gate;FINAL-DEFERRED-IMPLEMENTATION-TRACKER;MDF-016;MDF-016-CURRENT-CORE-DATATYPE-METRICS-REDACTION;DEFER-DTM-*;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
  )

  add_executable(database_lifecycle_current_core_storage_metrics_management_gate
    current_core_storage_metrics_management_gate.cpp
  )
  target_compile_features(database_lifecycle_current_core_storage_metrics_management_gate PRIVATE cxx_std_17)
  target_link_libraries(database_lifecycle_current_core_storage_metrics_management_gate
    PRIVATE
      sb_core_metrics
      sb_core_platform
  )

  add_test(
    NAME database_lifecycle_current_core_storage_metrics_management_gate
    COMMAND database_lifecycle_current_core_storage_metrics_management_gate
  )
  set_tests_properties(database_lifecycle_current_core_storage_metrics_management_gate PROPERTIES
    LABELS "database_lifecycle_current_core_storage_metrics_management_gate;FINAL-DEFERRED-IMPLEMENTATION-TRACKER;MDF-018;MDF-018-CURRENT-CORE-STORAGE-METRICS-MANAGEMENT;DEFER-SPM-*;database_lifecycle;mga_transaction_regression"
  )

  add_executable(database_lifecycle_current_core_datatype_conformance_manifest_gate
    current_core_datatype_conformance_manifest_gate.cpp
  )
  target_compile_features(database_lifecycle_current_core_datatype_conformance_manifest_gate PRIVATE cxx_std_17)
  target_link_libraries(database_lifecycle_current_core_datatype_conformance_manifest_gate
    PRIVATE
      sb_core_datatypes
      sb_core_platform
  )

  add_test(
    NAME database_lifecycle_current_core_datatype_conformance_manifest_gate
    COMMAND database_lifecycle_current_core_datatype_conformance_manifest_gate
  )
  set_tests_properties(database_lifecycle_current_core_datatype_conformance_manifest_gate PROPERTIES
    LABELS "database_lifecycle_current_core_datatype_conformance_manifest_gate;FINAL-DEFERRED-IMPLEMENTATION-TRACKER;MDF-015;MDF-015-CURRENT-CORE-DATATYPE-CONFORMANCE-MANIFEST;DEFER-DPE-EXAMPLE-CORPUS;DEFER-DTYPE-CONFORMANCE-MANIFESTS;database_lifecycle;datatype_commercial_closure_gate;mga_transaction_regression"
  )
endif()

if(TARGET sb_engine_internal_api AND TARGET sb_core_datatypes AND TARGET sb_core_index AND TARGET sb_core_metrics)
  add_executable(database_lifecycle_datatype_numeric_p3_conformance
    datatype_numeric_p3_conformance.cpp
  )
  target_compile_features(database_lifecycle_datatype_numeric_p3_conformance PRIVATE cxx_std_20)
  target_link_libraries(database_lifecycle_datatype_numeric_p3_conformance
    PRIVATE
      sb_core_datatypes
      sb_core_metrics
      sb_core_platform
      sb_engine_internal_api
      sbl_numeric
  )

  add_test(
    NAME database_lifecycle_datatype_numeric_p3_conformance
    COMMAND database_lifecycle_datatype_numeric_p3_conformance
  )
  set_tests_properties(database_lifecycle_datatype_numeric_p3_conformance PROPERTIES
    LABELS "datatype_commercial_closure_gate;numeric_128_backend_gate;cast_comparison_gate;domain_method_binding_gate;database_lifecycle;mga_transaction_regression"
  )

  add_executable(database_lifecycle_index_write_profile_p3_conformance
    index_write_profile_p3_conformance.cpp
  )
  target_compile_features(database_lifecycle_index_write_profile_p3_conformance PRIVATE cxx_std_20)
  target_link_libraries(database_lifecycle_index_write_profile_p3_conformance
    PRIVATE
      sb_core_index
      sb_core_metrics
      sb_core_platform
      sb_engine_internal_api
  )

  add_test(
    NAME database_lifecycle_index_write_profile_p3_conformance
    COMMAND database_lifecycle_index_write_profile_p3_conformance
  )
  set_tests_properties(database_lifecycle_index_write_profile_p3_conformance PROPERTIES
    LABELS "database_lifecycle_index_write_profile_p3_conformance;FINAL-DEFERRED-IMPLEMENTATION-TRACKER;MDF-021;MDF-022;DEFER-IDX-CATALOGS;DEFER-IDX-BTREE;DEFER-IDX-HASH;DEFER-IDX-BITMAP-BRIN;DEFER-IDX-DOCUMENT-SEARCH;DEFER-IDX-SPATIAL;DEFER-IDX-VECTOR;DEFER-IDX-RANGE-GRAPH;DEFER-IDX-COLUMNAR-SKETCH;DEFER-IPL-PAGE-LAYOUTS;DEFER-IPL-ORDERED-KEYS;DEFER-IPL-BTREE-HASH;DEFER-IPL-DOC-SEARCH-SPATIAL-VECTOR;index_catalog_ddl_gate;index_family_matrix_gate;insert_update_write_profile_gate;index_metrics_gate;database_lifecycle;mga_transaction_regression"
  )

  add_executable(dml_page_allocation_runtime_gate
    dml_page_allocation_runtime_gate.cpp
  )
  target_compile_features(dml_page_allocation_runtime_gate PRIVATE cxx_std_20)
  target_link_libraries(dml_page_allocation_runtime_gate
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sb_engine_sblr
      sb_storage_database
  )

  add_test(
    NAME dml_page_allocation_runtime_gate
    COMMAND dml_page_allocation_runtime_gate
  )
  set_tests_properties(dml_page_allocation_runtime_gate PROPERTIES
    LABELS "PFAR-012;IPAR-P3-10;IPAR-P7-12;dml_page_allocation_runtime_gate;database_lifecycle;mga_transaction_regression"
  )

  if(NOT TARGET sb_engine_sblr)
    add_subdirectory("${PROJECT_SOURCE_DIR}/src/engine/sblr" "${CMAKE_CURRENT_BINARY_DIR}/engine_sblr")
  endif()

  if(TARGET sb_engine_sblr)
    add_executable(sblr_agent_management_route_gate
      sblr_agent_management_route_gate.cpp
    )
    target_compile_features(sblr_agent_management_route_gate PRIVATE cxx_std_20)
    target_link_libraries(sblr_agent_management_route_gate
      PRIVATE
        sb_core_platform
        sb_core_uuid
        sb_engine_internal_api
        sb_engine_sblr
    )

    add_test(
      NAME sblr_agent_management_route_gate
      COMMAND sblr_agent_management_route_gate
    )
    set_tests_properties(sblr_agent_management_route_gate PROPERTIES
      LABELS "PFAR-013;sblr_agent_management_route_gate;database_lifecycle;mga_transaction_regression"
    )
  endif()
endif()

if(TARGET sb_core_agents)
  add_executable(database_lifecycle_jobs_scheduler_conformance
    jobs_scheduler_conformance.cpp
  )
  target_compile_features(database_lifecycle_jobs_scheduler_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_jobs_scheduler_conformance
    PRIVATE
      sb_core_agents
  )

  add_test(
    NAME database_lifecycle_jobs_scheduler_conformance
    COMMAND database_lifecycle_jobs_scheduler_conformance
  )
  set_tests_properties(database_lifecycle_jobs_scheduler_conformance PROPERTIES
    LABELS "database_lifecycle_jobs_scheduler_conformance;database_lifecycle_jobs_scheduler;database_lifecycle;database_lifecycle_shutdown_notification;database_lifecycle_cluster_boundary;mga_transaction_regression"
  )

  add_executable(database_lifecycle_workload_resource_conformance
    workload_resource_conformance.cpp
  )
  target_compile_features(database_lifecycle_workload_resource_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_workload_resource_conformance
    PRIVATE
      sb_core_agents
  )

  add_test(
    NAME database_lifecycle_workload_resource_conformance
    COMMAND database_lifecycle_workload_resource_conformance
  )
  set_tests_properties(database_lifecycle_workload_resource_conformance PROPERTIES
    LABELS "database_lifecycle_workload_resource;database_lifecycle;database_lifecycle_security;database_lifecycle_shutdown_notification;database_lifecycle_cluster_boundary;mga_transaction_regression"
  )

  add_executable(cdp_failure_backpressure_gate
    cdp_failure_backpressure_gate.cpp
  )
  target_compile_features(cdp_failure_backpressure_gate PRIVATE cxx_std_23)
  target_link_libraries(cdp_failure_backpressure_gate
    PRIVATE
      sb_core_agents
      sb_core_bulk_load
      sb_storage_page
  )

  add_test(
    NAME cdp_failure_backpressure_gate
    COMMAND cdp_failure_backpressure_gate
  )
  set_tests_properties(cdp_failure_backpressure_gate PROPERTIES
    LABELS "cdp_failure_backpressure_gate;CDP-014;CDP-015;database_lifecycle_storage;mga_transaction_regression"
  )
endif()

if(TARGET sb_server_core AND TARGET sb_server_event_notifications AND TARGET sb_engine_internal_api AND TARGET sb_storage_database)
  add_executable(database_lifecycle_event_notification_conformance
    event_notification_conformance.cpp
  )
  target_compile_features(database_lifecycle_event_notification_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_event_notification_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sb_server_core
      sb_server_event_notifications
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_event_notification_conformance
    COMMAND database_lifecycle_event_notification_conformance
  )
  set_tests_properties(database_lifecycle_event_notification_conformance PROPERTIES
    LABELS "database_lifecycle_event_notification_conformance;database_lifecycle_event_notification;database_lifecycle;database_lifecycle_security;database_lifecycle_ipc;database_lifecycle_shutdown_notification;mga_transaction_regression"
  )
endif()

if(TARGET sb_transaction_mga)
  add_executable(database_lifecycle_lock_wait_deadlock_conformance
    lock_wait_deadlock_conformance.cpp
  )
  target_compile_features(database_lifecycle_lock_wait_deadlock_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_lock_wait_deadlock_conformance
    PRIVATE
      sb_core_platform
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_lock_wait_deadlock_conformance
    COMMAND database_lifecycle_lock_wait_deadlock_conformance
  )
  set_tests_properties(database_lifecycle_lock_wait_deadlock_conformance PROPERTIES
    LABELS "database_lifecycle_lock_wait_deadlock_conformance;database_lifecycle_lock_wait_deadlock;database_lifecycle;mga_transaction_regression"
  )

  add_executable(database_lifecycle_mga_gc_retention_conformance
    mga_gc_retention_conformance.cpp
  )
  target_compile_features(database_lifecycle_mga_gc_retention_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_mga_gc_retention_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_mga_gc_retention_conformance
    COMMAND database_lifecycle_mga_gc_retention_conformance
  )
  set_tests_properties(database_lifecycle_mga_gc_retention_conformance PROPERTIES
    LABELS "database_lifecycle_mga_gc_retention_conformance;database_lifecycle_mga_gc_retention;database_lifecycle;mga_transaction_regression"
  )

  add_executable(memory_mga_transaction_governance_gate
    memory_mga_transaction_governance_gate.cpp
  )
  target_compile_features(memory_mga_transaction_governance_gate PRIVATE cxx_std_23)
  target_link_libraries(memory_mga_transaction_governance_gate
    PRIVATE
      sb_core_memory
      sb_core_platform
      sb_transaction_mga
  )
  add_test(
    NAME memory_mga_transaction_governance_gate
    COMMAND memory_mga_transaction_governance_gate
  )
  set_tests_properties(memory_mga_transaction_governance_gate PROPERTIES
    LABELS "memory_mga_transaction_governance_gate;MMCH-070;MMCH_MGA_TRANSACTION_MEMORY_GOVERNANCE;database_lifecycle;database_lifecycle_unit;mga_transaction_regression"
  )

  add_executable(dpc_authoritative_cleanup_horizon_service_gate
    dpc_authoritative_cleanup_horizon_service_gate.cpp
  )
  target_compile_features(dpc_authoritative_cleanup_horizon_service_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_authoritative_cleanup_horizon_service_gate
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_transaction_mga
  )

  add_test(
    NAME dpc_authoritative_cleanup_horizon_service_gate
    COMMAND dpc_authoritative_cleanup_horizon_service_gate
  )
  set_tests_properties(dpc_authoritative_cleanup_horizon_service_gate PROPERTIES
    LABELS "dpc_authoritative_cleanup_horizon_service_gate;DPC-030;DPC_TEST_AUTHORITATIVE_CLEANUP_HORIZON;ELER-107;ELER-GATE-107;cleanup_horizon;operational_readiness;mga_transaction_regression;database_lifecycle"
  )

  if(TARGET sb_core_agents AND TARGET sb_server_core)
    add_executable(dpc_transaction_pressure_manager_long_idle_gate
      dpc_transaction_pressure_manager_long_idle_gate.cpp
    )
    target_compile_features(dpc_transaction_pressure_manager_long_idle_gate PRIVATE cxx_std_23)
    target_link_libraries(dpc_transaction_pressure_manager_long_idle_gate
      PRIVATE
        sb_core_agents
        sb_core_uuid
        sb_server_core
        sb_transaction_mga
    )

    add_test(
      NAME dpc_transaction_pressure_manager_long_idle_gate
      COMMAND dpc_transaction_pressure_manager_long_idle_gate
    )
    set_tests_properties(dpc_transaction_pressure_manager_long_idle_gate PROPERTIES
      LABELS "dpc_transaction_pressure_manager_long_idle_gate;DPC-031;DPC_TEST_TRANSACTION_PRESSURE_MANAGER;long_idle_policy;always_active_transaction;mga_transaction_regression;database_lifecycle"
    )

    add_executable(dpc_storage_version_cleanup_agent_gate
      dpc_storage_version_cleanup_agent_gate.cpp
    )
    target_compile_features(dpc_storage_version_cleanup_agent_gate PRIVATE cxx_std_23)
    target_link_libraries(dpc_storage_version_cleanup_agent_gate
      PRIVATE
        sb_core_agents
        sb_core_uuid
        sb_transaction_mga
    )

    add_test(
      NAME dpc_storage_version_cleanup_agent_gate
      COMMAND dpc_storage_version_cleanup_agent_gate
    )
    set_tests_properties(dpc_storage_version_cleanup_agent_gate PROPERTIES
      LABELS "dpc_storage_version_cleanup_agent_gate;DPC-032;DPC_TEST_STORAGE_VERSION_CLEANUP_AGENT;storage_version_cleanup;cleanup_horizon;mga_transaction_regression;database_lifecycle"
    )

    if(TARGET sb_engine_internal_api AND TARGET sb_core_index)
      add_executable(dpc_cleanup_backup_restore_repair_diagnostics_gate
        dpc_cleanup_backup_restore_repair_diagnostics_gate.cpp
      )
      target_compile_features(dpc_cleanup_backup_restore_repair_diagnostics_gate PRIVATE cxx_std_23)
      target_link_libraries(dpc_cleanup_backup_restore_repair_diagnostics_gate
        PRIVATE
          sb_core_agents
          sb_core_index
          sb_core_uuid
          sb_engine_internal_api
          sb_transaction_mga
      )

      add_test(
        NAME dpc_cleanup_backup_restore_repair_diagnostics_gate
        COMMAND dpc_cleanup_backup_restore_repair_diagnostics_gate
      )
      set_tests_properties(dpc_cleanup_backup_restore_repair_diagnostics_gate PROPERTIES
        LABELS "dpc_cleanup_backup_restore_repair_diagnostics_gate;DPC-034;DPC_TEST_CLEANUP_DIAGNOSTICS;ELER-107;ELER-GATE-107;cleanup_horizon;storage_version_cleanup;secondary_index_garbage_cleanup;backup_restore;restricted_open;repair;operational_readiness;mga_transaction_regression;database_lifecycle"
      )

      add_executable(dpc_mga_index_cleanup_benchmark_gate
        dpc_mga_index_cleanup_benchmark_gate.cpp
      )
      target_compile_features(dpc_mga_index_cleanup_benchmark_gate PRIVATE cxx_std_23)
      target_link_libraries(dpc_mga_index_cleanup_benchmark_gate
        PRIVATE
          sb_core_agents
          sb_core_index
          sb_core_uuid
          sb_engine_internal_api
          sb_transaction_mga
      )

      add_test(
        NAME dpc_mga_index_cleanup_benchmark_gate
        COMMAND dpc_mga_index_cleanup_benchmark_gate
      )
      set_tests_properties(dpc_mga_index_cleanup_benchmark_gate PROPERTIES
        LABELS "dpc_mga_index_cleanup_benchmark_gate;DPC-035;DPC_TEST_MGA_INDEX_CLEANUP_BENCHMARK;cleanup_horizon;storage_version_cleanup;secondary_index_garbage_cleanup;cleanup_diagnostics;benchmark;mga_transaction_regression;database_lifecycle"
      )
    endif()
  endif()
endif()

if(TARGET sb_core_resources AND TARGET sb_storage_database AND TARGET sb_transaction_mga)
  add_executable(database_lifecycle_resource_seed_conformance
    resource_seed_conformance.cpp
  )
  target_compile_features(database_lifecycle_resource_seed_conformance PRIVATE cxx_std_23)
  target_compile_definitions(database_lifecycle_resource_seed_conformance PRIVATE
    SB_BOOTSTRAP_SEED_PACK_ROOT="${CMAKE_SOURCE_DIR}/resources/seed-packs/initial-resource-pack"
  )
  target_link_libraries(database_lifecycle_resource_seed_conformance
    PRIVATE
      sb_core_catalog
      sb_core_platform
      sb_core_resources
      sb_core_uuid
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_resource_seed_conformance
    COMMAND database_lifecycle_resource_seed_conformance
  )
  set_tests_properties(database_lifecycle_resource_seed_conformance PROPERTIES
    LABELS "database_lifecycle_resource_seed_conformance;database_lifecycle_resource_seed;database_lifecycle;database_lifecycle_storage;mga_transaction_regression"
  )
endif()

if(TARGET sb_core_memory)
  add_executable(database_lifecycle_temp_workspace_conformance
    temp_workspace_conformance.cpp
  )
  target_compile_features(database_lifecycle_temp_workspace_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_temp_workspace_conformance
    PRIVATE
      sb_core_memory
  )

  add_test(
    NAME database_lifecycle_temp_workspace_conformance
    COMMAND database_lifecycle_temp_workspace_conformance
  )
  set_tests_properties(database_lifecycle_temp_workspace_conformance PROPERTIES
    LABELS "database_lifecycle_temp_workspace_conformance;database_lifecycle_temp_workspace;database_lifecycle;database_lifecycle_unit;mga_transaction_regression"
  )

  add_executable(memory_secure_temp_workspace_gate
    memory_secure_temp_workspace_gate.cpp
  )
  target_compile_features(memory_secure_temp_workspace_gate PRIVATE cxx_std_23)
  target_link_libraries(memory_secure_temp_workspace_gate
    PRIVATE
      sb_core_memory
  )

  add_test(
    NAME memory_secure_temp_workspace_gate
    COMMAND memory_secure_temp_workspace_gate
  )
  set_tests_properties(memory_secure_temp_workspace_gate PROPERTIES
    LABELS "memory_secure_temp_workspace_gate;MMCH-040;MMCH_SECURE_TEMP_WORKSPACE;database_lifecycle_temp_workspace;database_lifecycle;database_lifecycle_unit;mga_transaction_regression"
  )

  add_executable(memory_temp_disk_reservation_semantics_gate
    memory_temp_disk_reservation_semantics_gate.cpp
  )
  target_compile_features(memory_temp_disk_reservation_semantics_gate PRIVATE cxx_std_23)
  target_link_libraries(memory_temp_disk_reservation_semantics_gate
    PRIVATE
      sb_core_memory
  )

  add_test(
    NAME memory_temp_disk_reservation_semantics_gate
    COMMAND memory_temp_disk_reservation_semantics_gate
  )
  set_tests_properties(memory_temp_disk_reservation_semantics_gate PROPERTIES
    LABELS "memory_temp_disk_reservation_semantics_gate;MMCH-041;MMCH_TEMP_DISK_RESERVATION_SEMANTICS;database_lifecycle_temp_workspace;database_lifecycle;database_lifecycle_unit;mga_transaction_regression"
  )

  add_executable(memory_temp_workspace_crash_recovery_gate
    memory_temp_workspace_crash_recovery_gate.cpp
  )
  target_compile_features(memory_temp_workspace_crash_recovery_gate PRIVATE cxx_std_23)
  target_link_libraries(memory_temp_workspace_crash_recovery_gate
    PRIVATE
      sb_core_memory
  )

  add_test(
    NAME memory_temp_workspace_crash_recovery_gate
    COMMAND memory_temp_workspace_crash_recovery_gate
  )
  set_tests_properties(memory_temp_workspace_crash_recovery_gate PROPERTIES
    LABELS "memory_temp_workspace_crash_recovery_gate;MMCH-042;MMCH_TEMP_WORKSPACE_CRASH_RECOVERY;database_lifecycle_temp_workspace;database_lifecycle;database_lifecycle_unit;mga_transaction_regression"
  )

  add_executable(memory_temp_workspace_cross_platform_gate
    memory_temp_workspace_cross_platform_gate.cpp
  )
  target_compile_features(memory_temp_workspace_cross_platform_gate PRIVATE cxx_std_23)
  target_link_libraries(memory_temp_workspace_cross_platform_gate
    PRIVATE
      sb_core_memory
      sb_core_platform
  )
  add_test(
    NAME memory_temp_workspace_cross_platform_gate
    COMMAND memory_temp_workspace_cross_platform_gate
  )
  set_tests_properties(memory_temp_workspace_cross_platform_gate PROPERTIES
    LABELS "memory_temp_workspace_cross_platform_gate;MMCH-043;MMCH_TEMP_WORKSPACE_CROSS_PLATFORM;database_lifecycle_temp_workspace;database_lifecycle;database_lifecycle_unit;mga_transaction_regression"
  )

  add_test(
    NAME memory_temp_workspace_cross_platform_source_gate
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/memory_temp_workspace_cross_platform_source_gate.py"
            --repo-root "${SB_PRIVATE_REPO_ROOT}"
  )
  set_tests_properties(memory_temp_workspace_cross_platform_source_gate PROPERTIES
    LABELS "memory_temp_workspace_cross_platform_source_gate;MMCH-043;MMCH_TEMP_WORKSPACE_CROSS_PLATFORM;database_lifecycle_temp_workspace;database_lifecycle;database_lifecycle_unit;mga_transaction_regression"
  )

  if(TARGET sb_storage_page)
    add_executable(memory_metadata_compatibility_gate
      memory_metadata_compatibility_gate.cpp
    )
    target_compile_features(memory_metadata_compatibility_gate PRIVATE cxx_std_23)
    target_link_libraries(memory_metadata_compatibility_gate
      PRIVATE
        sb_core_memory
        sb_storage_page
    )
    add_test(
      NAME memory_metadata_compatibility_gate
      COMMAND memory_metadata_compatibility_gate
    )
    set_tests_properties(memory_metadata_compatibility_gate PROPERTIES
      LABELS "memory_metadata_compatibility_gate;MMCH-044;MMCH_MEMORY_METADATA_OPEN_UPGRADE_COMPATIBILITY;database_lifecycle_temp_workspace;database_lifecycle;database_lifecycle_unit;mga_transaction_regression"
    )
  endif()

  add_executable(memory_support_bundle_evidence_gate
    memory_support_bundle_evidence_gate.cpp
  )
  target_compile_features(memory_support_bundle_evidence_gate PRIVATE cxx_std_23)
  target_link_libraries(memory_support_bundle_evidence_gate
    PRIVATE
      sb_core_memory
      sb_core_metrics
      sb_core_platform
  )
  add_test(
    NAME memory_support_bundle_evidence_gate
    COMMAND memory_support_bundle_evidence_gate
  )
  set_tests_properties(memory_support_bundle_evidence_gate PROPERTIES
    LABELS "memory_support_bundle_evidence_gate;MMCH-080;MMCH_MEMORY_SUPPORT_BUNDLE_EVIDENCE;database_lifecycle;database_lifecycle_unit;support_bundle;memory_observability;mga_transaction_regression"
  )

  if(TARGET sb_engine_internal_api)
    add_executable(database_lifecycle_memory_management_descriptor_conformance
      memory_management_descriptor_conformance.cpp
    )
    target_compile_features(database_lifecycle_memory_management_descriptor_conformance PRIVATE cxx_std_23)
    target_link_libraries(database_lifecycle_memory_management_descriptor_conformance
      PRIVATE
        sb_engine_internal_api
    )

    add_test(
      NAME database_lifecycle_memory_management_descriptor_conformance
      COMMAND database_lifecycle_memory_management_descriptor_conformance
    )
    set_tests_properties(database_lifecycle_memory_management_descriptor_conformance PROPERTIES
      LABELS "database_lifecycle_memory_management_descriptor_conformance;FINAL-DEFERRED-IMPLEMENTATION-TRACKER;MDF-011;MEMORY-MANAGEMENT-DESCRIPTOR-PLANNER-IMPLEMENTED;MEMORY-GOVERNANCE-DIRECT-EXECUTOR-IMPLEMENTED;MEMORY-AUTOMATION-REPORT-CATALOG-IMPLEMENTED;MEMORY-AUTOMATION-SAFE-EXECUTOR-IMPLEMENTED;MEMORY-OBJECT-RESIDENCY-DURABLE-CATALOG-IMPLEMENTED;MEMORY-RATE-LIMIT-DURABLE-CATALOG-IMPLEMENTED;MEMORY-RATE-LIMIT-LIVE-EXECUTOR-IMPLEMENTED;MEMORY-POLICY-MIGRATION-DURABLE-CATALOG-IMPLEMENTED;MM-MEMORY-MANAGEMENT-CORRECTIVE-CLOSURE;MM-GATE-001;MM-GATE-002;MM-GATE-003;MM-GATE-004;MM-GATE-005;MM-GATE-006;MM-GATE-007;MM-GATE-008;MM-GATE-009;MM-GATE-010;MM-GATE-011;MM-GATE-012;MM-GATE-013;MM-GATE-014;MM-GATE-015;MM-GATE-016;MM-GATE-017;MM-GATE-018;MM-GATE-019;MM-GATE-020;MM-GATE-021;MM-GATE-022;MM-GATE-023;MM-GATE-024;memory_governance;memory_automation;memory_object_residency;memory_rate_limit;memory_policy_migration;database_lifecycle;memory;mga_transaction_regression"
    )
  endif()

  add_test(
    NAME memory_production_test_separation_gate
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/memory_production_test_separation_gate.py"
            --repo-root "${SB_PRIVATE_REPO_ROOT}"
            --build-metadata "${SB_DPC_CROSS_BUILD_MATRIX_METADATA}"
  )
  set_tests_properties(memory_production_test_separation_gate PROPERTIES
    LABELS "memory_production_test_separation_gate;MMCH-081;MMCH_MEMORY_PRODUCTION_TEST_SEPARATION;database_lifecycle;memory;build_separation;mga_transaction_regression"
  )

  add_test(
    NAME memory_cross_platform_behavior_gate
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/memory_cross_platform_behavior_gate.py"
            --repo-root "${SB_PRIVATE_REPO_ROOT}"
  )
  set_tests_properties(memory_cross_platform_behavior_gate PROPERTIES
    LABELS "memory_cross_platform_behavior_gate;MMCH-110;MMCH_CROSS_PLATFORM_MEMORY_BEHAVIOR;database_lifecycle;memory;cross_platform;mga_transaction_regression"
  )
endif()

if(TARGET sb_core_catalog)
  add_executable(database_lifecycle_catalog_index_profile_conformance
    catalog_index_profile_conformance.cpp
  )
  target_compile_features(database_lifecycle_catalog_index_profile_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_catalog_index_profile_conformance
    PRIVATE
      sb_core_catalog
  )

  add_test(
    NAME database_lifecycle_catalog_index_profile_conformance
    COMMAND database_lifecycle_catalog_index_profile_conformance
  )
  set_tests_properties(database_lifecycle_catalog_index_profile_conformance PROPERTIES
    LABELS "database_lifecycle_catalog_index_profile_conformance;FINAL-DEFERRED-IMPLEMENTATION-TRACKER;MDF-025;DEFER-IFM-FAMILY-ROWS;DEFER-ICD-CATALOGS;DEFER-ICD-DIAGNOSTICS;DEFER-ICD-DRIVER-METADATA;DEFER-ICD-EXPLAIN-SUPPORT;DEFER-ICI-MANIFESTS;DEFER-IDX-CONFORMANCE;DEFER-IPL-IFM-DIO-CONFORMANCE;database_lifecycle_catalog_index_profile_conformance;database_lifecycle_catalog_index_profile;database_lifecycle;database_lifecycle_catalog;catalog_physical_index_profile_gate;catalog_common_descriptor_gate;mga_transaction_regression"
  )
endif()

add_test(
  NAME database_lifecycle_catalog_identity_boundary_static
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/catalog_identity_boundary_static.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(database_lifecycle_catalog_identity_boundary_static PROPERTIES
  LABELS "database_lifecycle_catalog_identity_boundary_static;DBLC_STATIC_CATALOG_IDENTITY_BOUNDARY;database_lifecycle_catalog_index_profile;database_lifecycle;database_lifecycle_catalog"
)

add_test(
  NAME database_lifecycle_catalog_index_profile_boundary_static
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/catalog_index_profile_boundary_static.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(database_lifecycle_catalog_index_profile_boundary_static PROPERTIES
  LABELS "database_lifecycle_catalog_index_profile_boundary_static;DBLC_STATIC_CATALOG_INDEX_PROFILE_BOUNDARY;database_lifecycle_catalog_index_profile;database_lifecycle;database_lifecycle_catalog"
)

add_test(
  NAME database_lifecycle_protocol_versioning_static
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/protocol_versioning_static.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(database_lifecycle_protocol_versioning_static PROPERTIES
  LABELS "database_lifecycle_protocol_versioning;DBLC_STATIC_PROTOCOL_VERSIONING;database_lifecycle"
)

add_test(
  NAME database_lifecycle_observability_static
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/observability_static.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(database_lifecycle_observability_static PROPERTIES
  LABELS "database_lifecycle_observability;database_lifecycle_supportability_evidence;DBLC_STATIC_DIAGNOSTIC_MESSAGE_VECTOR_AUDIT;DBLC_P15_OBSERVABILITY_COMPLETE;database_lifecycle"
)

if(TARGET sb_engine_internal_api)
  add_executable(cdp_performance_metric_schema_gate
    performance_metric_schema_gate.cpp
  )
  target_compile_features(cdp_performance_metric_schema_gate PRIVATE cxx_std_23)
  target_link_libraries(cdp_performance_metric_schema_gate
    PRIVATE
      sb_engine_internal_api
  )

  add_test(
    NAME cdp_performance_metric_schema_gate
    COMMAND cdp_performance_metric_schema_gate
  )
  set_tests_properties(cdp_performance_metric_schema_gate PROPERTIES
    LABELS "CDP-002;CDP-006;cdp_performance_metric_schema_gate;cdp_instrumentation_overhead_gate;database_lifecycle_observability;database_lifecycle"
  )

  add_executable(cdp_user_observability_surface_gate
    cdp_user_observability_surface_gate.cpp
  )
  target_compile_features(cdp_user_observability_surface_gate PRIVATE cxx_std_23)
  target_link_libraries(cdp_user_observability_surface_gate
    PRIVATE
      sb_engine_internal_api
  )

  add_test(
    NAME cdp_user_observability_surface_gate
    COMMAND cdp_user_observability_surface_gate
  )
  set_tests_properties(cdp_user_observability_surface_gate PROPERTIES
    LABELS "cdp_user_observability_surface_gate;CDP-033;CDP_P3_USER_OBSERVABILITY_READY;ELER-107;ELER-GATE-107;database_lifecycle_observability;management_ui_docs;operational_readiness;mga_transaction_regression"
  )

  add_executable(dpc_management_observability_support_bundle_gate
    dpc_management_observability_support_bundle_gate.cpp
  )
  target_compile_features(dpc_management_observability_support_bundle_gate
    PRIVATE cxx_std_23)
  target_link_libraries(dpc_management_observability_support_bundle_gate
    PRIVATE
      sb_core_uuid
      sb_engine_internal_api
  )

  add_test(
    NAME dpc_management_observability_support_bundle_gate
    COMMAND dpc_management_observability_support_bundle_gate
  )
  set_tests_properties(dpc_management_observability_support_bundle_gate PROPERTIES
    LABELS "dpc_management_observability_support_bundle_gate;DPC-061;DPC_P6_OBSERVABILITY_READY;DPC_TEST_OBSERVABILITY;ELER-107;ELER-GATE-107;management_observability;support_bundle;metrics;audit;message_vectors;operational_readiness;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_config_defaults_packaging_gate
    dpc_config_defaults_packaging_gate.cpp
  )
  target_compile_features(dpc_config_defaults_packaging_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_config_defaults_packaging_gate
    PRIVATE
      sb_engine_internal_api
  )

  add_test(
    NAME dpc_config_defaults_packaging_gate
    COMMAND dpc_config_defaults_packaging_gate
  )
  set_tests_properties(dpc_config_defaults_packaging_gate PROPERTIES
    LABELS "dpc_config_defaults_packaging_gate;DPC-068;DPC_P6_CONFIG_PACKAGING_READY;DPC_AUDIT_CONFIG_PACKAGING;DPC_DEP_CONFIG_PACKAGING;config_defaults;packaging;support_bundle;management_observability;mga_transaction_regression;database_lifecycle"
  )

  add_executable(dpc_security_privilege_gate
    dpc_security_privilege_gate.cpp
  )
  target_compile_features(dpc_security_privilege_gate PRIVATE cxx_std_23)
  target_link_libraries(dpc_security_privilege_gate
    PRIVATE
      sb_core_uuid
      sb_engine_internal_api
      sb_engine_sblr
  )

  add_test(
    NAME dpc_security_privilege_gate
    COMMAND dpc_security_privilege_gate
  )
  set_tests_properties(dpc_security_privilege_gate PROPERTIES
    LABELS "dpc_security_privilege_gate;DPC-066;DPC_P6_SECURITY_PRIVILEGE_READY;DPC_TEST_SECURITY_PRIVILEGE;management_authorization;exact_denial;message_vectors;mga_transaction_regression;database_lifecycle"
  )
endif()

set(SB_DPC_STANDALONE_CTEST_INDEPENDENCE_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/dpc_standalone_ctest_independence_gate.py"
)

add_custom_target(dpc_standalone_ctest_independence_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_DPC_STANDALONE_CTEST_INDEPENDENCE_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --build-root "${CMAKE_BINARY_DIR}"
  COMMENT "Validating DPC standalone CTest independence and no-execution_plan runtime dependencies"
)

add_test(
  NAME dpc_standalone_ctest_independence_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_DPC_STANDALONE_CTEST_INDEPENDENCE_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --build-root "${CMAKE_BINARY_DIR}"
)
set_tests_properties(dpc_standalone_ctest_independence_gate PROPERTIES
  LABELS "dpc_standalone_ctest_independence_gate;DPC-062;DPC_P6_STANDALONE_CTEST_READY;DPC_AUDIT_STANDALONE_CTEST;DPC_TEST_NO_EXECUTION_PLAN_RUNTIME;DPC_DEP_STANDALONE_CTEST;standalone_ctest;no_execution_plan_runtime;database_lifecycle;mga_transaction_regression"
)

set(SB_DPC_NEGATIVE_REJECTED_TECHNIQUE_DRIFT_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/dpc_negative_rejected_technique_drift_gate.py"
)

add_custom_target(dpc_negative_rejected_technique_drift_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_DPC_NEGATIVE_REJECTED_TECHNIQUE_DRIFT_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --build-root "${CMAKE_BINARY_DIR}"
  COMMENT "Validating DPC rejected-technique negative drift gate"
)

add_test(
  NAME dpc_negative_rejected_technique_drift_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_DPC_NEGATIVE_REJECTED_TECHNIQUE_DRIFT_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --build-root "${CMAKE_BINARY_DIR}"
)
set_tests_properties(dpc_negative_rejected_technique_drift_gate PROPERTIES
  LABELS "dpc_negative_rejected_technique_drift_gate;DPC-067;DPC_P6_NEGATIVE_DRIFT_READY;DPC_AUDIT_NEGATIVE_DRIFT;DPC_NEGATIVE_DRIFT;DPC_DEP_NEGATIVE_DRIFT;rejected_technique_drift;database_lifecycle;mga_transaction_regression"
)

set(SB_DPC_EVIDENCE_PROVENANCE_RETENTION_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/dpc_evidence_provenance_retention_gate.py"
)

set(SB_MIGRATION_DEFERRED_OPERATIONS_CLOSURE_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/migration_deferred_operations_closure_gate.py"
)

set(SB_FINAL_DEFERRED_IMPLEMENTATION_TRACKER_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/final_deferred_implementation_tracker_gate.py"
)

add_custom_target(migration_deferred_operations_closure_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_MIGRATION_DEFERRED_OPERATIONS_CLOSURE_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --build-root "${CMAKE_BINARY_DIR}"
  COMMENT "Validating migration deferred operations implementation closure"
)

add_test(
  NAME migration_deferred_operations_closure_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_MIGRATION_DEFERRED_OPERATIONS_CLOSURE_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --build-root "${CMAKE_BINARY_DIR}"
)
set_tests_properties(migration_deferred_operations_closure_gate PROPERTIES
  LABELS "migration_deferred_operations_closure_gate;MIGRATION-DEFERRED-OPERATIONS-CLOSURE;MIGRATION-DIRECTORY-IMPLEMENTATION-CLOSURE;DEFERRED-MIGRATION-IMPLEMENTATION-CLOSURE;TEMP-TABLE-GATE-017;database_lifecycle;mga_transaction_regression"
)

add_custom_target(final_deferred_implementation_tracker_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_FINAL_DEFERRED_IMPLEMENTATION_TRACKER_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --build-root "${CMAKE_BINARY_DIR}"
  COMMENT "Validating final deferred implementation tracker inventory"
)

add_custom_target(final_deferred_implementation_tracker_release_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_FINAL_DEFERRED_IMPLEMENTATION_TRACKER_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --build-root "${CMAKE_BINARY_DIR}"
          --mode release
  COMMENT "Validating final deferred implementation release closure"
)

add_test(
  NAME final_deferred_implementation_tracker_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_FINAL_DEFERRED_IMPLEMENTATION_TRACKER_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --build-root "${CMAKE_BINARY_DIR}"
)
set_tests_properties(final_deferred_implementation_tracker_gate PROPERTIES
  LABELS "final_deferred_implementation_tracker_gate;FINAL-DEFERRED-IMPLEMENTATION-TRACKER;MDF-000;DEFERRED-MIGRATION-IMPLEMENTATION-CLOSURE;migration_deferred_operations_closure;database_lifecycle;mga_transaction_regression"
)

set(SB_DPC_EVIDENCE_PROVENANCE_RETENTION_MANIFEST
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dpc_evidence_provenance_retention_manifest.json"
)

add_custom_target(dpc_evidence_provenance_retention_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_DPC_EVIDENCE_PROVENANCE_RETENTION_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --build-root "${CMAKE_BINARY_DIR}"
          --manifest "${SB_DPC_EVIDENCE_PROVENANCE_RETENTION_MANIFEST}"
  COMMENT "Validating DPC-072 retained evidence provenance and retention manifest"
)

add_test(
  NAME dpc_evidence_provenance_retention_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_DPC_EVIDENCE_PROVENANCE_RETENTION_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --build-root "${CMAKE_BINARY_DIR}"
          --manifest "${SB_DPC_EVIDENCE_PROVENANCE_RETENTION_MANIFEST}"
)
set_tests_properties(dpc_evidence_provenance_retention_gate PROPERTIES
  LABELS "dpc_evidence_provenance_retention_gate;DPC-072;DPC_EVIDENCE_PROVENANCE_RETENTION_GATE;DPC_P6_EVIDENCE_RETENTION_READY;DPC_AUDIT_EVIDENCE_RETENTION;DPC_DEP_EVIDENCE_RETENTION;evidence_retention;database_lifecycle;mga_transaction_regression"
)

set(SB_DPC_PROFILER_HOT_PATH_ATTRIBUTION_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/dpc_profiler_hot_path_attribution_gate.py"
)

set(SB_DPC_PROFILER_HOT_PATH_ATTRIBUTION_MANIFEST
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dpc_profiler_hot_path_attribution_manifest.json"
)

add_custom_target(dpc_profiler_hot_path_attribution_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_DPC_PROFILER_HOT_PATH_ATTRIBUTION_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --build-root "${CMAKE_BINARY_DIR}"
          --manifest "${SB_DPC_PROFILER_HOT_PATH_ATTRIBUTION_MANIFEST}"
  COMMENT "Validating DPC-073 profiler and hot-path attribution fixture"
)

add_test(
  NAME dpc_profiler_hot_path_attribution_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_DPC_PROFILER_HOT_PATH_ATTRIBUTION_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --build-root "${CMAKE_BINARY_DIR}"
          --manifest "${SB_DPC_PROFILER_HOT_PATH_ATTRIBUTION_MANIFEST}"
)
set_tests_properties(dpc_profiler_hot_path_attribution_gate PROPERTIES
  LABELS "dpc_profiler_hot_path_attribution_gate;DPC-073;DPC_PROFILER_HOT_PATH_ATTRIBUTION_GATE;DPC_P6_PROFILER_ATTRIBUTION_READY;DPC_AUDIT_PROFILER_ATTRIBUTION;DPC_DEP_PROFILER_ATTRIBUTION;profiler_attribution;database_lifecycle;mga_transaction_regression"
)

add_test(
  NAME database_lifecycle_admin_cli_static
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/admin_cli_static.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(database_lifecycle_admin_cli_static PROPERTIES
  LABELS "database_lifecycle_admin_cli;DBLC_STATIC_ADMIN_AUTH_AUDIT_ROUTE;DBLC_P13P_ADMIN_CLI_COMPLETE;database_lifecycle"
)

add_test(
  NAME database_lifecycle_packaging_service_static
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/packaging_service_static.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(database_lifecycle_packaging_service_static PROPERTIES
  LABELS "database_lifecycle_packaging_service;DBLC_STATIC_RUNTIME_CLEANUP;DBLC_P13Q_PACKAGING_SERVICE_COMPLETE;database_lifecycle"
)

add_test(
  NAME database_lifecycle_upgrade_migration_static
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/upgrade_migration_static.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(database_lifecycle_upgrade_migration_static PROPERTIES
  LABELS "database_lifecycle_upgrade_migration;DBLC_STATIC_MIGRATION_NO_GUESSING;database_lifecycle"
)

add_test(
  NAME database_lifecycle_threat_model_static
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/threat_model_static.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(database_lifecycle_threat_model_static PROPERTIES
  LABELS "database_lifecycle_threat_model;DBLC_STATIC_THREAT_MODEL_ABUSE_CASES;database_lifecycle"
)

add_test(
  NAME database_lifecycle_reference_mapping_static
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/reference_mapping_static.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(database_lifecycle_reference_mapping_static PROPERTIES
  LABELS "database_lifecycle_reference_mapping;DBLC_STATIC_NO_REFERENCE_ENGINE_SQL;database_lifecycle"
)

if(TARGET sb_storage_page)
  add_executable(database_lifecycle_storage_allocation_conformance
    storage_allocation_conformance.cpp
  )
  target_compile_features(database_lifecycle_storage_allocation_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_storage_allocation_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_storage_page
  )

  add_test(
    NAME database_lifecycle_storage_allocation_conformance
    COMMAND database_lifecycle_storage_allocation_conformance
  )
  set_tests_properties(database_lifecycle_storage_allocation_conformance PROPERTIES
    LABELS "database_lifecycle_storage_allocation_conformance;database_lifecycle_storage_allocation;database_lifecycle;database_lifecycle_storage;mga_transaction_regression;page_durability_ordering_gate;PRF_G07_MGA_RECOVERY_READY"
  )
endif()

if(TARGET sb_engine_internal_api)
  add_executable(database_lifecycle_replication_boundary_conformance
    replication_boundary_conformance.cpp
  )
  target_compile_features(database_lifecycle_replication_boundary_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_replication_boundary_conformance
    PRIVATE
      sb_engine_internal_api
  )

  add_test(
    NAME database_lifecycle_replication_boundary_conformance
    COMMAND database_lifecycle_replication_boundary_conformance
  )
  set_tests_properties(database_lifecycle_replication_boundary_conformance PROPERTIES
    LABELS "database_lifecycle_replication_boundary_conformance;database_lifecycle_replication_boundary;database_lifecycle;database_lifecycle_cluster_boundary;database_lifecycle_security;mga_transaction_regression"
  )
endif()

if(TARGET sb_engine_internal_api AND TARGET sb_storage_database)
  add_executable(database_lifecycle_constraint_dml_enforcement_conformance
    constraint_dml_enforcement_conformance.cpp
  )
  target_compile_features(database_lifecycle_constraint_dml_enforcement_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_constraint_dml_enforcement_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_constraint_dml_enforcement_conformance
    COMMAND database_lifecycle_constraint_dml_enforcement_conformance
  )
	  set_tests_properties(database_lifecycle_constraint_dml_enforcement_conformance PROPERTIES
	    LABELS "database_lifecycle_constraint_dml_enforcement_conformance;FINAL-DEFERRED-IMPLEMENTATION-TRACKER;MDF-019;MDF-020;DEFER-CLI-CATALOGS;DEFER-CLI-ENFORCEMENT;DEFER-CLI-VALIDATION-STATES;DEFER-CLI-DEFERRAL;DEFER-CLI-MAINTENANCE;DEFER-CLI-DURABILITY-RESIDENCY;DEFER-CLI-MEMORY-OPTIMIZATION;DEFER-CLI-METRICS;DEFER-CLI-CONFORMANCE;constraint_dml_enforcement_gate;constraint_transaction_visibility_gate;constraint_diagnostic_gate;constraint_index_dependency_gate;PRF_G06A_CONSTRAINT_INDEX_DEPENDENCY_READY;PRF_G06_CONSTRAINTS_READY;database_lifecycle;mga_transaction_regression"
	  )

	  add_executable(ipar_insert_runtime_security_recheck_gate
	    ipar_insert_runtime_security_recheck_gate.cpp
	  )
	  target_compile_features(ipar_insert_runtime_security_recheck_gate PRIVATE cxx_std_23)
	  target_link_libraries(ipar_insert_runtime_security_recheck_gate
	    PRIVATE
	      sb_core_platform
	      sb_core_uuid
	      sb_engine_internal_api
	      sb_engine_sblr
	      sb_storage_database
	      sb_storage_disk
	      sb_storage_page
	      sb_transaction_mga
	  )

	  add_test(
	    NAME ipar_insert_runtime_security_recheck_gate
	    COMMAND ipar_insert_runtime_security_recheck_gate
	  )
		  set_tests_properties(ipar_insert_runtime_security_recheck_gate PROPERTIES
		    LABELS "ipar_insert_runtime_security_recheck_gate;IPAR-P1-04;security_fast_path;prepared_cache;database_lifecycle;mga_transaction_regression"
		  )

		  add_executable(ipar_relation_state_load_route_gate
		    ipar_relation_state_load_route_gate.cpp
		  )
		  target_compile_features(ipar_relation_state_load_route_gate PRIVATE cxx_std_23)
		  target_link_libraries(ipar_relation_state_load_route_gate
		    PRIVATE
		      sb_core_platform
		      sb_core_uuid
		      sb_engine_internal_api
		      sb_engine_sblr
		      sb_storage_database
		      sb_storage_disk
		      sb_storage_page
		      sb_transaction_mga
		  )

		  add_test(
		    NAME ipar_relation_state_load_route_gate
		    COMMAND ipar_relation_state_load_route_gate
		  )
		  set_tests_properties(ipar_relation_state_load_route_gate PROPERTIES
		    LABELS "ipar_relation_state_load_route_gate;IPAR-P2-01;IPAR-P2-02;IPAR-P2-03;relation_state_load;database_lifecycle;mga_transaction_regression"
		  )

		  add_executable(ipar_long_snapshot_version_chain_gate
		    ipar_long_snapshot_version_chain_gate.cpp
		  )
		  target_compile_features(ipar_long_snapshot_version_chain_gate PRIVATE cxx_std_23)
		  target_link_libraries(ipar_long_snapshot_version_chain_gate
		    PRIVATE
		      sb_core_platform
		      sb_core_uuid
		      sb_engine_internal_api
		      sb_engine_sblr
		      sb_storage_database
		      sb_storage_disk
		      sb_storage_page
		      sb_transaction_mga
		  )

		  add_test(
		    NAME ipar_long_snapshot_version_chain_gate
		    COMMAND ipar_long_snapshot_version_chain_gate
		  )
		  set_tests_properties(ipar_long_snapshot_version_chain_gate PROPERTIES
		    LABELS "ipar_long_snapshot_version_chain_gate;IPAR-P2-04;long_snapshot;version_chain;insert_performance;database_lifecycle;mga_transaction_regression"
		  )

		  add_executable(ipar_adaptive_batch_sizing_gate
		    ipar_adaptive_batch_sizing_gate.cpp
		  )
		  target_compile_features(ipar_adaptive_batch_sizing_gate PRIVATE cxx_std_23)
		  target_link_libraries(ipar_adaptive_batch_sizing_gate
		    PRIVATE
		      sb_core_platform
		      sb_core_uuid
		      sb_engine_internal_api
		      sb_engine_sblr
		      sb_storage_database
		      sb_storage_disk
		      sb_storage_page
		      sb_transaction_mga
		  )

		  add_test(
		    NAME ipar_adaptive_batch_sizing_gate
		    COMMAND ipar_adaptive_batch_sizing_gate
		  )
		  set_tests_properties(ipar_adaptive_batch_sizing_gate PROPERTIES
		    LABELS "ipar_adaptive_batch_sizing_gate;IPAR-P1-05;adaptive_batch;insert_performance;database_lifecycle;mga_transaction_regression"
		  )

		  add_executable(ipar_large_value_batch_allocation_gate
		    ipar_large_value_batch_allocation_gate.cpp
		  )
		  target_compile_features(ipar_large_value_batch_allocation_gate PRIVATE cxx_std_23)
		  target_link_libraries(ipar_large_value_batch_allocation_gate
		    PRIVATE
		      sb_core_platform
		      sb_core_uuid
		      sb_engine_internal_api
		      sb_engine_sblr
		      sb_storage_database
		      sb_storage_disk
		      sb_storage_page
		      sb_transaction_mga
		  )

		  add_test(
		    NAME ipar_large_value_batch_allocation_gate
		    COMMAND ipar_large_value_batch_allocation_gate
		  )
		  set_tests_properties(ipar_large_value_batch_allocation_gate PROPERTIES
		    LABELS "ipar_large_value_batch_allocation_gate;IPAR-P3-04;large_value_batch_allocation;insert_performance;database_lifecycle;mga_transaction_regression"
		  )

		  add_executable(ipar_multi_filespace_placement_gate
		    ipar_multi_filespace_placement_gate.cpp
		  )
		  target_compile_features(ipar_multi_filespace_placement_gate PRIVATE cxx_std_23)
		  target_link_libraries(ipar_multi_filespace_placement_gate
		    PRIVATE
		      sb_core_platform
		      sb_core_uuid
		      sb_engine_internal_api
		      sb_engine_sblr
		      sb_storage_database
		      sb_storage_disk
		      sb_storage_page
		      sb_transaction_mga
		  )

		  add_test(
		    NAME ipar_multi_filespace_placement_gate
		    COMMAND ipar_multi_filespace_placement_gate
		  )
		  set_tests_properties(ipar_multi_filespace_placement_gate PROPERTIES
		    LABELS "ipar_multi_filespace_placement_gate;IPAR-P3-06;multi_filespace_placement;insert_performance;database_lifecycle;mga_transaction_regression"
		  )

		  add_executable(ipar_resource_quota_backpressure_gate
		    ipar_resource_quota_backpressure_gate.cpp
		  )
		  target_compile_features(ipar_resource_quota_backpressure_gate PRIVATE cxx_std_23)
		  target_link_libraries(ipar_resource_quota_backpressure_gate
		    PRIVATE
		      sb_core_platform
		      sb_core_uuid
		      sb_engine_internal_api
		      sb_engine_sblr
		      sb_storage_database
		      sb_storage_disk
		      sb_storage_page
		      sb_transaction_mga
		  )

		  add_test(
		    NAME ipar_resource_quota_backpressure_gate
		    COMMAND ipar_resource_quota_backpressure_gate
		  )
		  set_tests_properties(ipar_resource_quota_backpressure_gate PROPERTIES
		    LABELS "ipar_resource_quota_backpressure_gate;IPAR-P3-07;resource_quota;backpressure;insert_performance;database_lifecycle;mga_transaction_regression"
		  )

		  add_executable(ipar_commit_durability_batching_gate
		    ipar_commit_durability_batching_gate.cpp
		  )
		  target_compile_features(ipar_commit_durability_batching_gate PRIVATE cxx_std_23)
		  target_link_libraries(ipar_commit_durability_batching_gate
		    PRIVATE
		      sb_core_platform
		      sb_core_uuid
		      sb_engine_internal_api
		      sb_engine_sblr
		      sb_storage_database
		      sb_storage_disk
		      sb_storage_page
		      sb_transaction_mga
		  )

		  add_test(
		    NAME ipar_commit_durability_batching_gate
		    COMMAND ipar_commit_durability_batching_gate
		  )
		  set_tests_properties(ipar_commit_durability_batching_gate PROPERTIES
		    LABELS "ipar_commit_durability_batching_gate;IPAR-P3-05;IPAR-P3-08;commit_fence;durability_batching;insert_performance;database_lifecycle;mga_transaction_regression"
		  )

		  add_executable(ipar_physical_row_page_append_gate
		    ipar_physical_row_page_append_gate.cpp
		  )
		  target_compile_features(ipar_physical_row_page_append_gate PRIVATE cxx_std_23)
		  target_link_libraries(ipar_physical_row_page_append_gate
		    PRIVATE
		      sb_core_platform
		      sb_core_uuid
		      sb_engine_internal_api
		      sb_engine_sblr
		      sb_storage_database
		      sb_storage_disk
		      sb_storage_page
		      sb_transaction_mga
		  )

		  add_test(
		    NAME ipar_physical_row_page_append_gate
		    COMMAND ipar_physical_row_page_append_gate
		  )
		  set_tests_properties(ipar_physical_row_page_append_gate PROPERTIES
		    LABELS "ipar_physical_row_page_append_gate;IPAR-P3-01;page_native_row_append;direct_physical_bulk;insert_performance;database_lifecycle;mga_transaction_regression"
		  )

		  add_executable(ipar_storage_format_migration_safety_gate
		    ipar_storage_format_migration_safety_gate.cpp
		  )
		  target_compile_features(ipar_storage_format_migration_safety_gate PRIVATE cxx_std_23)
		  target_link_libraries(ipar_storage_format_migration_safety_gate
		    PRIVATE
		      sb_core_platform
		      sb_core_uuid
		      sb_storage_database
		      sb_storage_disk
		      sb_storage_page
		  )

		  add_test(
		    NAME ipar_storage_format_migration_safety_gate
		    COMMAND ipar_storage_format_migration_safety_gate
		  )
		  set_tests_properties(ipar_storage_format_migration_safety_gate PROPERTIES
		    LABELS "ipar_storage_format_migration_safety_gate;IPAR-P3-09;IPAR-P7-11;storage_format;migration_refusal;row_page;index_page;allocation_map;database_open_format_refusal;database_lifecycle;mga_transaction_regression"
		  )

		  add_executable(ipar_concurrency_insert_proof_gate
		    ipar_concurrency_insert_proof_gate.cpp
		  )
		  target_compile_features(ipar_concurrency_insert_proof_gate PRIVATE cxx_std_23)
		  target_link_libraries(ipar_concurrency_insert_proof_gate
		    PRIVATE
		      sb_core_platform
		      sb_core_uuid
		      sb_engine_internal_api
		      sb_engine_sblr
		      sb_storage_database
		      sb_storage_disk
		      sb_storage_page
		      sb_transaction_mga
		  )

		  add_test(
		    NAME ipar_concurrency_insert_proof_gate
		    COMMAND ipar_concurrency_insert_proof_gate
		  )
		  set_tests_properties(ipar_concurrency_insert_proof_gate PROPERTIES
		    LABELS "ipar_concurrency_insert_proof_gate;IPAR-P7-04;concurrency;insert;copy_import;rollback;unique_conflict;restart_persistence;database_lifecycle;mga_transaction_regression"
		  )

		  add_executable(ipar_insert_cancel_timeout_gate
		    ipar_insert_cancel_timeout_gate.cpp
		  )
		  target_compile_features(ipar_insert_cancel_timeout_gate PRIVATE cxx_std_23)
		  target_link_libraries(ipar_insert_cancel_timeout_gate
		    PRIVATE
		      sb_core_platform
		      sb_core_uuid
		      sb_engine_internal_api
		      sb_engine_sblr
		      sb_storage_database
		      sb_storage_disk
		      sb_storage_page
		      sb_transaction_mga
		  )

		  add_test(
		    NAME ipar_insert_cancel_timeout_gate
		    COMMAND ipar_insert_cancel_timeout_gate
		  )
		  set_tests_properties(ipar_insert_cancel_timeout_gate PROPERTIES
		    LABELS "ipar_insert_cancel_timeout_gate;IPAR-P5-05;cancel_timeout;copy_import;insert_performance;database_lifecycle;mga_transaction_regression"
		  )

		  add_executable(ipar_restart_persistence_gate
		    ipar_restart_persistence_gate.cpp
		  )
		  target_compile_features(ipar_restart_persistence_gate PRIVATE cxx_std_23)
		  target_link_libraries(ipar_restart_persistence_gate
		    PRIVATE
		      sb_core_platform
		      sb_core_uuid
		      sb_engine_internal_api
		      sb_engine_sblr
		      sb_storage_database
		      sb_storage_disk
		      sb_storage_page
		      sb_transaction_mga
		  )

		  add_test(
		    NAME ipar_restart_persistence_gate
		    COMMAND ipar_restart_persistence_gate
		  )
		  set_tests_properties(ipar_restart_persistence_gate PROPERTIES
		    LABELS "ipar_restart_persistence_gate;IPAR-P7-05;restart_persistence;insert_performance;database_lifecycle;mga_transaction_regression"
		  )

		  add_executable(ipar_fault_injection_recovery_gate
		    ipar_fault_injection_recovery_gate.cpp
		  )
		  target_compile_features(ipar_fault_injection_recovery_gate PRIVATE cxx_std_23)
		  target_link_libraries(ipar_fault_injection_recovery_gate
		    PRIVATE
		      sb_core_index
		      sb_core_platform
		      sb_core_uuid
		      sb_engine_internal_api
		      sb_engine_sblr
		      sb_storage_database
		      sb_storage_disk
		      sb_storage_page
		      sb_transaction_mga
		  )

		  add_test(
		    NAME ipar_fault_injection_recovery_gate
		    COMMAND ipar_fault_injection_recovery_gate
		  )
		  set_tests_properties(ipar_fault_injection_recovery_gate PROPERTIES
		    LABELS "ipar_fault_injection_recovery_gate;IPAR-P7-06;fault_injection;recovery;insert_performance;database_lifecycle;mga_transaction_regression"
		  )

		  add_executable(ipar_data_shape_stress_matrix_gate
		    ipar_data_shape_stress_matrix_gate.cpp
		  )
		  target_compile_features(ipar_data_shape_stress_matrix_gate PRIVATE cxx_std_23)
		  target_link_libraries(ipar_data_shape_stress_matrix_gate
		    PRIVATE
		      sb_core_bulk_load
		      sb_core_platform
		      sb_core_uuid
		      sb_engine_internal_api
		      sb_engine_sblr
		      sb_storage_database
		      sb_storage_filespace
		      sb_storage_page
		      sb_transaction_mga
		  )

		  add_test(
		    NAME ipar_data_shape_stress_matrix_gate
		    COMMAND ipar_data_shape_stress_matrix_gate
		  )
		  set_tests_properties(ipar_data_shape_stress_matrix_gate PROPERTIES
		    LABELS "ipar_data_shape_stress_matrix_gate;IPAR-P7-09;data_shape_stress;insert_performance;copy_import;index_probe;multi_filespace_placement;database_lifecycle;mga_transaction_regression"
		  )

		  add_executable(ipar_mixed_reader_writer_workload_gate
		    ipar_mixed_reader_writer_workload_gate.cpp
		  )
		  target_compile_features(ipar_mixed_reader_writer_workload_gate PRIVATE cxx_std_23)
		  target_link_libraries(ipar_mixed_reader_writer_workload_gate
		    PRIVATE
		      sb_core_platform
		      sb_core_uuid
		      sb_engine_internal_api
		      sb_engine_sblr
		      sb_storage_database
		      sb_storage_disk
		      sb_storage_page
		      sb_transaction_mga
		  )

		  add_test(
		    NAME ipar_mixed_reader_writer_workload_gate
		    COMMAND ipar_mixed_reader_writer_workload_gate
		  )
		  set_tests_properties(ipar_mixed_reader_writer_workload_gate PROPERTIES
		    LABELS "ipar_mixed_reader_writer_workload_gate;IPAR-P7-10;mixed_reader_writer;insert_performance;select;index_lookup;metadata_read;navigator_tree;database_lifecycle;mga_transaction_regression"
		  )

		  add_executable(ipar_unique_index_probe_gate
		    ipar_unique_index_probe_gate.cpp
		  )
		  target_compile_features(ipar_unique_index_probe_gate PRIVATE cxx_std_23)
		  target_link_libraries(ipar_unique_index_probe_gate
		    PRIVATE
		      sb_core_platform
		      sb_core_uuid
		      sb_engine_internal_api
		      sb_engine_sblr
		      sb_storage_database
		      sb_storage_disk
		      sb_storage_page
		      sb_transaction_mga
		  )

		  add_test(
		    NAME ipar_unique_index_probe_gate
		    COMMAND ipar_unique_index_probe_gate
		  )
		  set_tests_properties(ipar_unique_index_probe_gate PROPERTIES
		    LABELS "ipar_unique_index_probe_gate;IPAR-P4-01;IPAR-P4-03;synchronous_exact_unique_maintenance;unique_index_probe;physical_unique_index_probe;database_lifecycle;mga_transaction_regression"
		  )
		endif()

if(TARGET sb_server_core)
  add_executable(ipar_prepared_execution_cache_authority_gate
    ipar_prepared_execution_cache_authority_gate.cpp
  )
  target_compile_features(ipar_prepared_execution_cache_authority_gate PRIVATE cxx_std_23)
  target_link_libraries(ipar_prepared_execution_cache_authority_gate
    PRIVATE
      sb_server_core
  )

  add_test(
    NAME ipar_prepared_execution_cache_authority_gate
    COMMAND ipar_prepared_execution_cache_authority_gate
  )
  set_tests_properties(ipar_prepared_execution_cache_authority_gate PROPERTIES
    LABELS "ipar_prepared_execution_cache_authority_gate;IPAR-P1-12;IPAR-P1-13;IPAR-P1-15;IPAR-P1-18;prepared_execution_context;negative_authorization_cache;capability_cache;statement_preflight_cache;database_lifecycle;mga_transaction_regression"
  )

  add_executable(ipar_prepared_handle_authority_gate
    ipar_prepared_handle_authority_gate.cpp
  )
  target_compile_features(ipar_prepared_handle_authority_gate PRIVATE cxx_std_23)
  target_link_libraries(ipar_prepared_handle_authority_gate
    PRIVATE
      sb_server_core
  )

  add_test(
    NAME ipar_prepared_handle_authority_gate
    COMMAND ipar_prepared_handle_authority_gate
  )
  set_tests_properties(ipar_prepared_handle_authority_gate PROPERTIES
    LABELS "ipar_prepared_handle_authority_gate;IPAR-P1-03;IPAR-P1-14;IPAR-P5-04;IPAR-G037;IPAR-G079;prepared_sblr;prepared_handle_authority;session_object_handle;database_lifecycle;mga_transaction_regression"
  )

  add_executable(ipar_server_observability_projection_gate
    ipar_server_observability_projection_gate.cpp
  )
  target_compile_features(ipar_server_observability_projection_gate PRIVATE cxx_std_23)
  target_link_libraries(ipar_server_observability_projection_gate
    PRIVATE
      sb_server_core
      sb_engine_internal_api
  )

  add_test(
    NAME ipar_server_observability_projection_gate
    COMMAND ipar_server_observability_projection_gate
  )
  set_tests_properties(ipar_server_observability_projection_gate PROPERTIES
    LABELS "IPAR-P6-06;IPAR-P6-07;IPAR-P6-08;IPAR-P6-13;ipar_server_observability_projection_gate;server_observability;slow_path;database_lifecycle;mga_transaction_regression"
  )

  add_executable(ipar_contention_quota_projection_gate
    ipar_contention_quota_projection_gate.cpp
  )
  target_compile_features(ipar_contention_quota_projection_gate PRIVATE cxx_std_23)
  target_link_libraries(ipar_contention_quota_projection_gate
    PRIVATE
      sb_server_core
      sb_engine_internal_api
  )

  add_test(
    NAME ipar_contention_quota_projection_gate
    COMMAND ipar_contention_quota_projection_gate
  )
  set_tests_properties(ipar_contention_quota_projection_gate PROPERTIES
    LABELS "IPAR-P4-06;IPAR-P6-06;ipar_contention_quota_projection_gate;server_observability;sys_ipar;contention;quota;prepared_cache;page_allocation;agent_queue;database_lifecycle;mga_transaction_regression"
  )

  add_executable(ipar_agent_runtime_idle_gate
    ipar_agent_runtime_idle_gate.cpp
  )
  target_compile_features(ipar_agent_runtime_idle_gate PRIVATE cxx_std_23)
  set(IPAR_AGENT_RUNTIME_IDLE_GATE_LIBS
    sb_core_platform
    sb_core_uuid
    sb_engine_internal_api
    sb_engine_sblr
    sb_server_core
    sb_storage_database
    sb_storage_disk
    sb_storage_page
    sb_transaction_mga
  )
  if(CMAKE_CXX_LINK_GROUP_USING_RESCAN_SUPPORTED)
    string(JOIN "," IPAR_AGENT_RUNTIME_IDLE_GATE_LIBS_CSV
      ${IPAR_AGENT_RUNTIME_IDLE_GATE_LIBS})
    target_link_libraries(ipar_agent_runtime_idle_gate
      PRIVATE
        "$<LINK_GROUP:RESCAN,${IPAR_AGENT_RUNTIME_IDLE_GATE_LIBS_CSV}>"
    )
  elseif(UNIX AND NOT APPLE)
    target_link_libraries(ipar_agent_runtime_idle_gate
      PRIVATE
        -Wl,--start-group
        ${IPAR_AGENT_RUNTIME_IDLE_GATE_LIBS}
        -Wl,--end-group
    )
  else()
    target_link_libraries(ipar_agent_runtime_idle_gate
      PRIVATE
        ${IPAR_AGENT_RUNTIME_IDLE_GATE_LIBS}
    )
  endif()

  add_test(
    NAME ipar_agent_runtime_idle_gate
    COMMAND ipar_agent_runtime_idle_gate
  )
  set_tests_properties(ipar_agent_runtime_idle_gate PROPERTIES
    LABELS "IPAR-P6-03;IPAR-P6-04;IPAR-P6-05;IPAR-P6-06;IPAR-P7-12;agent_runtime;agent_idle;agent_failure_diagnostics;agent_fairness;database_lifecycle;mga_transaction_regression"
  )

  add_executable(database_lifecycle_memory_policy_config_conformance
    memory_policy_config_conformance.cpp
  )
  target_compile_features(database_lifecycle_memory_policy_config_conformance PRIVATE cxx_std_23)
  target_compile_definitions(database_lifecycle_memory_policy_config_conformance PRIVATE
    SB_DEFAULT_POLICY_PACK_ROOT="${PROJECT_SOURCE_DIR}/resources/policy-packs/default-local-password"
  )
  target_link_libraries(database_lifecycle_memory_policy_config_conformance
    PRIVATE
      sb_server_core
  )

  add_test(
    NAME database_lifecycle_memory_policy_config_conformance
    COMMAND database_lifecycle_memory_policy_config_conformance
  )
  set_tests_properties(database_lifecycle_memory_policy_config_conformance PROPERTIES
    LABELS "database_lifecycle_memory_policy_config_conformance;MMCH-011;MMCH-013;memory_policy;database_lifecycle;mga_transaction_regression"
  )

  add_executable(database_lifecycle_attach_auth_conformance
    attach_auth_conformance.cpp
  )
  target_compile_features(database_lifecycle_attach_auth_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_attach_auth_conformance
    PRIVATE
      sb_server_core
  )

  add_test(
    NAME database_lifecycle_attach_auth_conformance
    COMMAND database_lifecycle_attach_auth_conformance
  )
  set_tests_properties(database_lifecycle_attach_auth_conformance PROPERTIES
    LABELS "database_lifecycle_attach_auth_conformance;database_lifecycle;database_lifecycle_security;sbsql_engine_auth_authority;security_auth_gate;sb_server_sbsql_admission_conformance;mga_transaction_regression"
  )

  add_executable(database_lifecycle_process_association_conformance
    process_association_conformance.cpp
  )
  target_compile_features(database_lifecycle_process_association_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_process_association_conformance
    PRIVATE
      sb_server_core
  )

  add_test(
    NAME database_lifecycle_process_association_conformance
    COMMAND database_lifecycle_process_association_conformance
  )
  set_tests_properties(database_lifecycle_process_association_conformance PROPERTIES
    LABELS "database_lifecycle_process_association_conformance;database_lifecycle;database_lifecycle_process_association;database_lifecycle_listener;database_lifecycle_parser;database_lifecycle_server_route;database_lifecycle_session_request_cursor;server_lifecycle_gate;routing_reconnect_finality_gate;mga_transaction_regression"
  )

  add_executable(database_lifecycle_capability_profile_conformance
    capability_profile_conformance.cpp
  )
  target_compile_features(database_lifecycle_capability_profile_conformance PRIVATE cxx_std_23)
  target_compile_definitions(
    database_lifecycle_capability_profile_conformance
    PRIVATE
      SB_SPEC_MANIFEST="${SB_PRIVATE_REPO_ROOT}/public_contract_snapshot"
  )
  target_link_libraries(database_lifecycle_capability_profile_conformance
    PRIVATE
      sb_core_agents
      sb_server_core
  )

  add_test(
    NAME database_lifecycle_capability_profile_conformance
    COMMAND database_lifecycle_capability_profile_conformance
  )
  set_tests_properties(database_lifecycle_capability_profile_conformance PROPERTIES
    LABELS "database_lifecycle_capability_profile_conformance;database_lifecycle_capability_profile;database_lifecycle;database_lifecycle_config_policy_security_provider;database_lifecycle_cluster_boundary;mga_transaction_regression"
  )

  if(TARGET sb_engine_internal_api)
    add_executable(database_lifecycle_udr_extension_conformance
      udr_extension_conformance.cpp
    )
    target_compile_features(database_lifecycle_udr_extension_conformance PRIVATE cxx_std_23)
    target_link_libraries(database_lifecycle_udr_extension_conformance
      PRIVATE
        sb_engine_internal_api
        sb_server_core
        sb_udr_runtime
        sbu_firebird_parser_support
        sbu_sbsql_parser_support
    )

    add_test(
      NAME database_lifecycle_udr_extension_conformance
      COMMAND database_lifecycle_udr_extension_conformance
    )
    set_tests_properties(database_lifecycle_udr_extension_conformance PROPERTIES
      LABELS "database_lifecycle_udr_extension_conformance;database_lifecycle_udr_extension;FINAL-DEFERRED-IMPLEMENTATION-TRACKER;MDF-017;DEFER-CXX-ONLY-UDR-ENFORCEMENT;DEFER-DTM-UDR-BRIDGE-METRICS;database_lifecycle;database_lifecycle_parser;database_lifecycle_security;database_lifecycle_shutdown_notification;mga_transaction_regression"
    )
  endif()

  add_executable(database_lifecycle_server_daemon_conformance
    server_daemon_lifecycle_conformance.cpp
  )
  target_compile_features(database_lifecycle_server_daemon_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_server_daemon_conformance
    PRIVATE
      sb_core_uuid
      sb_server_core
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_server_daemon_conformance
    COMMAND database_lifecycle_server_daemon_conformance
  )
  set_tests_properties(database_lifecycle_server_daemon_conformance PROPERTIES
    LABELS "database_lifecycle_server_daemon_conformance;database_lifecycle;database_lifecycle_server_daemon;database_lifecycle_server_route;database_lifecycle_process_association;database_lifecycle_listener;database_lifecycle_session_request_cursor;server_lifecycle_gate;mga_transaction_regression"
  )

  if(TARGET sb_local_ipc AND TARGET sbl_listener_runtime)
    add_executable(database_lifecycle_wire_driver_p5_conformance
      wire_driver_p5_conformance.cpp
    )
    target_compile_features(database_lifecycle_wire_driver_p5_conformance PRIVATE cxx_std_23)
    target_link_libraries(database_lifecycle_wire_driver_p5_conformance
      PRIVATE
        sb_local_ipc
        sb_server_core
        sbl_listener_runtime
    )

    add_test(
      NAME database_lifecycle_wire_driver_p5_conformance
      COMMAND database_lifecycle_wire_driver_p5_conformance
    )
    set_tests_properties(database_lifecycle_wire_driver_p5_conformance PROPERTIES
      LABELS "database_lifecycle_wire_driver_p5_conformance;database_lifecycle;server_lifecycle_gate;listener_sbct_pool_gate;local_ipc_gate;sbwp_wire_gate;mga_transaction_regression"
    )
  endif()

  if(TARGET sbl_parser_server_ipc_schema)
    add_executable(database_lifecycle_ipc_conformance
      ipc_lifecycle_conformance.cpp
    )
    target_compile_features(database_lifecycle_ipc_conformance PRIVATE cxx_std_23)
    target_link_libraries(database_lifecycle_ipc_conformance
      PRIVATE
        sb_server_event_notifications
        sb_server_core
        sbl_parser_server_ipc_schema
    )

    add_test(
      NAME database_lifecycle_ipc_conformance
      COMMAND database_lifecycle_ipc_conformance
    )
    set_tests_properties(database_lifecycle_ipc_conformance PROPERTIES
      LABELS "database_lifecycle_ipc_conformance;database_lifecycle;database_lifecycle_ipc;database_lifecycle_server_route;database_lifecycle_process_association;database_lifecycle_listener;database_lifecycle_parser;database_lifecycle_session_request_cursor;local_ipc_gate;sbwp_wire_gate;mga_transaction_regression"
    )
  endif()
endif()

if(TARGET sb_server_core AND TARGET sb_storage_database AND TARGET sb_storage_disk AND
   TARGET sb_storage_page AND TARGET sb_transaction_mga AND TARGET sbl_parser_server_ipc_schema)
  add_executable(database_lifecycle_protocol_versioning_conformance
    protocol_versioning_conformance.cpp
  )
  target_compile_features(database_lifecycle_protocol_versioning_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_protocol_versioning_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sb_server_core
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
      sbl_parser_server_ipc_schema
  )

  add_test(
    NAME database_lifecycle_protocol_versioning_conformance
    COMMAND database_lifecycle_protocol_versioning_conformance
  )
  set_tests_properties(database_lifecycle_protocol_versioning_conformance PROPERTIES
    LABELS "database_lifecycle_protocol_versioning;ELER-106;ELER-GATE-106;database_lifecycle;database_lifecycle_ipc;database_lifecycle_storage;protocol_version_policy;local_ipc_gate;sbwp_wire_gate;mga_transaction_regression"
  )
endif()

if(TARGET sb_server_core AND TARGET sb_storage_database AND TARGET sb_storage_disk AND TARGET sb_storage_page AND TARGET sb_transaction_mga)
  if(TARGET sbl_sbsql_parser_worker_core AND TARGET sbl_firebird_parser_pipeline)
    add_executable(database_lifecycle_reference_mapping_conformance
      reference_mapping_conformance.cpp
    )
    target_compile_features(database_lifecycle_reference_mapping_conformance PRIVATE cxx_std_23)
    target_link_libraries(database_lifecycle_reference_mapping_conformance
      PRIVATE
        sbl_sbsql_parser_worker_core
        sbl_firebird_parser_pipeline
    )

    add_test(
      NAME database_lifecycle_reference_mapping_conformance
      COMMAND database_lifecycle_reference_mapping_conformance
    )
    set_tests_properties(database_lifecycle_reference_mapping_conformance PROPERTIES
      LABELS "database_lifecycle_reference_mapping;DBLC_P14_REFERENCE_MAPPING_COMPLETE;database_lifecycle"
    )
  endif()

  add_executable(database_lifecycle_upgrade_migration_conformance
    upgrade_migration_conformance.cpp
  )
  target_compile_features(database_lifecycle_upgrade_migration_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_upgrade_migration_conformance
    PRIVATE
      sb_core_platform
      sb_core_memory
      sb_core_uuid
      sb_server_core
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_upgrade_migration_conformance
    COMMAND database_lifecycle_upgrade_migration_conformance
  )
  set_tests_properties(database_lifecycle_upgrade_migration_conformance PROPERTIES
    LABELS "database_lifecycle_upgrade_migration;DBLC_P13S_UPGRADE_MIGRATION_COMPLETE;ELER-106;ELER-GATE-106;database_lifecycle;database_lifecycle_storage;format_version_policy;upgrade_downgrade_refusal;mga_transaction_regression"
  )

  add_executable(database_lifecycle_threat_model_conformance
    threat_model_conformance.cpp
  )
  target_compile_features(database_lifecycle_threat_model_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_threat_model_conformance
    PRIVATE
      sb_core_agents
      sb_engine_internal_api
      sb_server_core
  )

  add_test(
    NAME database_lifecycle_threat_model_conformance
    COMMAND database_lifecycle_threat_model_conformance
  )
  set_tests_properties(database_lifecycle_threat_model_conformance PROPERTIES
    LABELS "database_lifecycle_threat_model;DBLC_P13T_THREAT_MODEL_COMPLETE;database_lifecycle;mga_transaction_regression"
  )

  if(TARGET sbl_manager_node_runtime)
    add_executable(database_lifecycle_packaging_service_conformance
      packaging_service_conformance.cpp
    )
    target_compile_features(database_lifecycle_packaging_service_conformance PRIVATE cxx_std_23)
    target_link_libraries(database_lifecycle_packaging_service_conformance
      PRIVATE
        sb_server_core
        sbl_manager_node_runtime
    )

    add_test(
      NAME database_lifecycle_packaging_service_conformance
      COMMAND database_lifecycle_packaging_service_conformance
    )
    set_tests_properties(database_lifecycle_packaging_service_conformance PROPERTIES
      LABELS "database_lifecycle_packaging_service;DBLC_P13Q_PACKAGING_SERVICE_COMPLETE;database_lifecycle;mga_transaction_regression"
    )
  endif()

  add_executable(database_lifecycle_admin_cli_conformance
    admin_cli_conformance.cpp
    ../../drivers/tool/cli/res_lifecycle_parity.cpp
  )
  target_compile_features(database_lifecycle_admin_cli_conformance PRIVATE cxx_std_23)
  target_include_directories(database_lifecycle_admin_cli_conformance
    PRIVATE
      "${PROJECT_SOURCE_DIR}/drivers/tool/cli"
      "${PROJECT_SOURCE_DIR}/drivers/tool/cli/include"
      "${PROJECT_SOURCE_DIR}/drivers/driver/cpp/include"
  )
  target_link_libraries(database_lifecycle_admin_cli_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sb_server_core
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_admin_cli_conformance
    COMMAND database_lifecycle_admin_cli_conformance
  )
  set_tests_properties(database_lifecycle_admin_cli_conformance PROPERTIES
    LABELS "database_lifecycle_admin_cli;DBLC_P13P_ADMIN_CLI_COMPLETE;ELER-107;ELER-GATE-107;database_lifecycle;database_lifecycle_server_route;database_lifecycle_security;operational_readiness;mga_transaction_regression"
  )

  add_executable(database_lifecycle_transaction_admission_conformance
    transaction_admission_conformance.cpp
  )
  target_compile_features(database_lifecycle_transaction_admission_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_transaction_admission_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_server_core
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_transaction_admission_conformance
    COMMAND database_lifecycle_transaction_admission_conformance
  )
  set_tests_properties(database_lifecycle_transaction_admission_conformance PROPERTIES
    LABELS "database_lifecycle_transaction_admission_conformance;database_lifecycle;database_lifecycle_storage;database_lifecycle_security;database_lifecycle_cluster_boundary;mga_transaction_regression"
  )

  add_executable(database_lifecycle_detach_cleanup_conformance
    detach_cleanup_conformance.cpp
  )
  target_compile_features(database_lifecycle_detach_cleanup_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_detach_cleanup_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_server_core
      sb_server_event_notifications
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_detach_cleanup_conformance
    COMMAND database_lifecycle_detach_cleanup_conformance
  )
  set_tests_properties(database_lifecycle_detach_cleanup_conformance PROPERTIES
    LABELS "database_lifecycle_detach_cleanup_conformance;database_lifecycle;database_lifecycle_server_route;database_lifecycle_ipc;database_lifecycle_session_request_cursor;database_lifecycle_parser;routing_reconnect_finality_gate;mga_transaction_regression"
  )

  add_executable(database_lifecycle_session_request_cursor_conformance
    session_request_cursor_conformance.cpp
  )
  target_compile_features(database_lifecycle_session_request_cursor_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_session_request_cursor_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_server_core
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_session_request_cursor_conformance
    COMMAND database_lifecycle_session_request_cursor_conformance
  )
  set_tests_properties(database_lifecycle_session_request_cursor_conformance PROPERTIES
    LABELS "database_lifecycle_session_request_cursor_conformance;database_lifecycle;database_lifecycle_session_request_cursor;database_lifecycle_server_route;database_lifecycle_ipc;database_lifecycle_parser;database_lifecycle_security;routing_reconnect_finality_gate;mga_transaction_regression"
  )

  add_executable(database_lifecycle_bounded_stress_resource_leak_conformance
    bounded_stress_resource_leak_conformance.cpp
  )
  target_compile_features(database_lifecycle_bounded_stress_resource_leak_conformance PRIVATE cxx_std_23)
  target_include_directories(database_lifecycle_bounded_stress_resource_leak_conformance
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src/engine/sblr"
  )
  target_link_libraries(database_lifecycle_bounded_stress_resource_leak_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sb_server_core
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_bounded_stress_resource_leak_conformance
    COMMAND database_lifecycle_bounded_stress_resource_leak_conformance
  )
  set_tests_properties(database_lifecycle_bounded_stress_resource_leak_conformance PROPERTIES
    LABELS "database_lifecycle_bounded_stress_resource_leak_conformance;CBQ_GATE_STRESS_SOAK_RESOURCE_LEAK;ELER-107;ELER-GATE-107;database_lifecycle;database_lifecycle_server_route;database_lifecycle_resource;database_lifecycle_shutdown;operational_readiness;mga_transaction_regression"
  )

  add_executable(dpc_soak_leak_resource_stability_gate
    dpc_soak_leak_resource_stability_gate.cpp
  )
  target_compile_features(dpc_soak_leak_resource_stability_gate PRIVATE cxx_std_23)
  target_include_directories(dpc_soak_leak_resource_stability_gate
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src/engine/sblr"
  )
  target_link_libraries(dpc_soak_leak_resource_stability_gate
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sb_server_core
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME dpc_soak_leak_resource_stability_gate
    COMMAND dpc_soak_leak_resource_stability_gate
  )
  set_tests_properties(dpc_soak_leak_resource_stability_gate PROPERTIES
    LABELS "dpc_soak_leak_resource_stability_gate;DPC-070;DPC_SOAK_LEAK_RESOURCE_STABILITY_GATE;DPC_P6_SOAK_LEAK_READY;DPC_P6_SOAK_LEAK_RESOURCE_STABILITY_READY;ELER-081;ELER-GATE-081;ELER-102;ELER-GATE-102;ELER-107;ELER-GATE-107;soak;resource_stability;support_bundle;management_observability;database_lifecycle;database_lifecycle_server_route;database_lifecycle_resource;database_lifecycle_shutdown;operational_readiness;mga_transaction_regression;linux_proof"
  )

  add_executable(database_lifecycle_engine_agent_conformance
    engine_agent_conformance.cpp
  )
  target_compile_features(database_lifecycle_engine_agent_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_engine_agent_conformance
    PRIVATE
      sb_core_agents
      sb_core_platform
      sb_core_uuid
      sb_server_core
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_engine_agent_conformance
    COMMAND database_lifecycle_engine_agent_conformance
  )
  set_tests_properties(database_lifecycle_engine_agent_conformance PROPERTIES
    LABELS "database_lifecycle_engine_agent_conformance;database_lifecycle;database_lifecycle_engine_agent;database_lifecycle_server_route;database_lifecycle_session_request_cursor;database_lifecycle_storage;database_lifecycle_security;database_lifecycle_cluster_boundary;mga_transaction_regression"
  )

  add_executable(database_lifecycle_cluster_boundary_conformance
    cluster_boundary_conformance.cpp
  )
  target_compile_features(database_lifecycle_cluster_boundary_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_cluster_boundary_conformance
    PRIVATE
      sb_core_agents
      sb_core_index
      sb_core_metrics
      sb_engine_internal_api
      sb_server_core
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_cluster_boundary_conformance
    COMMAND database_lifecycle_cluster_boundary_conformance
  )
  set_tests_properties(database_lifecycle_cluster_boundary_conformance PROPERTIES
    LABELS "database_lifecycle_cluster_boundary_conformance;database_lifecycle;database_lifecycle_cluster_boundary;database_lifecycle_engine_agent;database_lifecycle_server_route;database_lifecycle_storage;database_lifecycle_security;mga_transaction_regression"
  )

  add_executable(database_lifecycle_supportability_evidence_conformance
    supportability_evidence_conformance.cpp
  )
  target_compile_features(database_lifecycle_supportability_evidence_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_supportability_evidence_conformance
    PRIVATE
      sb_engine_internal_api
      sb_server_core
  )

  add_test(
    NAME database_lifecycle_supportability_evidence_conformance
    COMMAND database_lifecycle_supportability_evidence_conformance
  )
  set_tests_properties(database_lifecycle_supportability_evidence_conformance PROPERTIES
    LABELS "database_lifecycle_supportability_evidence_conformance;database_lifecycle_supportability_evidence;ELER-107;ELER-GATE-107;database_lifecycle;database_lifecycle_server_route;database_lifecycle_security;database_lifecycle_shutdown_notification;operational_readiness;mga_transaction_regression"
  )

  add_executable(database_lifecycle_backup_restore_export_admin_gate_conformance
    backup_restore_export_admin_gate_conformance.cpp
  )
  target_compile_features(database_lifecycle_backup_restore_export_admin_gate_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_backup_restore_export_admin_gate_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sb_engine_sblr
      sb_server_core
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_backup_restore_export_admin_gate_conformance
    COMMAND database_lifecycle_backup_restore_export_admin_gate_conformance
  )
  set_tests_properties(database_lifecycle_backup_restore_export_admin_gate_conformance PROPERTIES
    LABELS "database_lifecycle_backup_restore_export_admin_gate_conformance;CBQ-039;CBQ_GATE_BACKUP_RESTORE_EXPORT_ADMIN;database_lifecycle;database_lifecycle_server_route;database_lifecycle_storage;database_lifecycle_admin_cli;database_lifecycle_supportability_evidence;mga_transaction_regression"
  )

  add_executable(database_lifecycle_observability_conformance
    observability_conformance.cpp
  )
  target_compile_features(database_lifecycle_observability_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_observability_conformance
    PRIVATE
      sb_engine_internal_api
      sb_server_core
  )

  add_test(
    NAME database_lifecycle_observability_conformance
    COMMAND database_lifecycle_observability_conformance
  )
  set_tests_properties(database_lifecycle_observability_conformance PROPERTIES
    LABELS "database_lifecycle_observability;database_lifecycle_supportability_evidence;DBLC_P15_OBSERVABILITY_COMPLETE;ELER-107;ELER-GATE-107;database_lifecycle;operational_readiness;mga_transaction_regression"
  )

  add_executable(database_lifecycle_config_policy_security_provider_conformance
    config_policy_security_provider_conformance.cpp
  )
  target_compile_features(database_lifecycle_config_policy_security_provider_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_config_policy_security_provider_conformance
    PRIVATE
      sb_engine_internal_api
      sb_server_core
  )

  add_test(
    NAME database_lifecycle_config_policy_security_provider_conformance
    COMMAND database_lifecycle_config_policy_security_provider_conformance
  )
  set_tests_properties(database_lifecycle_config_policy_security_provider_conformance PROPERTIES
    LABELS "database_lifecycle_config_policy_security_provider_conformance;database_lifecycle_config_policy_security_provider;ELER-107;ELER-GATE-107;database_lifecycle;database_lifecycle_security;database_lifecycle_server_route;database_lifecycle_session_request_cursor;database_lifecycle_default_policy_catalog;security_auth_gate;security_policy_gate;auth_plugin_manifest_gate;operational_readiness;mga_transaction_regression"
  )

  add_executable(database_lifecycle_maintenance_repair_conformance
    maintenance_repair_conformance.cpp
  )
  target_compile_features(database_lifecycle_maintenance_repair_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_maintenance_repair_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sb_engine_sblr
      sb_server_core
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_maintenance_repair_conformance
    COMMAND database_lifecycle_maintenance_repair_conformance
  )
  set_tests_properties(database_lifecycle_maintenance_repair_conformance PROPERTIES
    LABELS "database_lifecycle_maintenance_repair_conformance;ELER-107;ELER-GATE-107;database_lifecycle;database_lifecycle_storage;database_lifecycle_admin_cli;database_lifecycle_cluster_boundary;operational_readiness;mga_transaction_regression"
  )

  add_executable(database_lifecycle_shutdown_conformance
    shutdown_conformance.cpp
  )
  target_compile_features(database_lifecycle_shutdown_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_shutdown_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_server_core
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_shutdown_conformance
    COMMAND database_lifecycle_shutdown_conformance
  )
  set_tests_properties(database_lifecycle_shutdown_conformance PROPERTIES
    LABELS "database_lifecycle_shutdown_conformance;ELER-107;ELER-GATE-107;database_lifecycle;database_lifecycle_storage;database_lifecycle_server_route;server_lifecycle_gate;routing_reconnect_finality_gate;operational_readiness;mga_transaction_regression;database_lifecycle_tx_gate"
  )

  add_executable(database_lifecycle_shutdown_notification_conformance
    shutdown_notification_conformance.cpp
  )
  target_compile_features(database_lifecycle_shutdown_notification_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_shutdown_notification_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_server_core
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_shutdown_notification_conformance
    COMMAND database_lifecycle_shutdown_notification_conformance
  )
  set_tests_properties(database_lifecycle_shutdown_notification_conformance PROPERTIES
    LABELS "database_lifecycle_shutdown_notification_conformance;database_lifecycle;database_lifecycle_server_route;database_lifecycle_shutdown_notification;database_lifecycle_process_association;database_lifecycle_storage;server_lifecycle_gate;routing_reconnect_finality_gate;mga_transaction_regression"
  )

  add_executable(database_lifecycle_drop_conformance
    drop_conformance.cpp
  )
  target_compile_features(database_lifecycle_drop_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_drop_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sb_server_core
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
  )

  add_test(
    NAME database_lifecycle_drop_conformance
    COMMAND database_lifecycle_drop_conformance
  )
  set_tests_properties(database_lifecycle_drop_conformance PROPERTIES
    LABELS "database_lifecycle_drop_conformance;database_lifecycle;database_lifecycle_storage;database_lifecycle_filespace;mga_transaction_regression"
  )
endif()
