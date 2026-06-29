# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

cmake_minimum_required(VERSION 3.25)

# Current-repo driver/adaptor/tool integration gates.
#
# The gates are intentionally CTest-first and mostly script-backed. Native
# language build wrappers are added incrementally, but every wrapper must route
# outputs through ${PROJECT_BINARY_DIR}/drivers.

set(SB_DRIVER_FIXTURE_DIR
  "${PROJECT_SOURCE_DIR}/drivers/fixtures/driver_adaptor_current_repo_integration"
)
set(SB_DRIVER_SERVER_RECONCILIATION_FIXTURE_DIR
  "${PROJECT_SOURCE_DIR}/drivers/fixtures/driver_server_reconciliation"
)
set(SB_DRIVER_GATE_SCRIPT
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/driver_execution_plan_gate.py"
)
set(SB_DRIVER_COMPONENT_RUNNER
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/driver_component_runner.py"
)
set(SB_DRIVER_STATUS_PACKAGE_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/driver_status_package_gate.py"
)
set(SB_DRIVER_P4_ROUTE_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/driver_p4_route_gate.py"
)
set(SB_DRIVER_P4_DOCS_REFERENCE_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/driver_p4_docs_reference_gate.py"
)
set(SB_DRIVER_RELEASE_DECLARATION_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/driver_release_declaration_gate.py"
)
set(SB_DRIVER_STATIC_REQUIREMENT_GATE
  "${PROJECT_SOURCE_DIR}/tests/conformance/drivers/driver_static_requirement_gate.py"
)
set(SB_DRIVER_NATIVE_FULL_SURFACE_GATE
  "${PROJECT_SOURCE_DIR}/tests/conformance/drivers/driver_native_full_surface_gate.py"
)
set(SB_DRIVER_NATIVE_MATRIX_PLAN_GATE
  "${PROJECT_SOURCE_DIR}/tests/conformance/drivers/driver_native_matrix_plan_gate.py"
)
set(SB_DRIVER_NATIVE_WORKPLAN_COVERAGE_GATE
  "${PROJECT_SOURCE_DIR}/tests/conformance/drivers/driver_native_workplan_coverage_gate.py"
)
set(SB_DRIVER_NATIVE_FULL_SURFACE_MATRIX_RUNNER
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_driver_native_full_surface_matrix.py"
)
set(SB_DRIVER_NATIVE_FULL_SURFACE_MATRIX_FINALIZER
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/finalize_driver_native_full_surface_matrix.py"
)
set(SB_DRIVER_NATIVE_MATRIX_LANE_PROVISIONER
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/provision_driver_native_matrix_lanes.py"
)
set(SB_DRIVER_NATIVE_TOOL_BUILDER
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_native_driver_tools.py"
)
set(SB_DRIVER_SCRIPT_DIFF_RUNNER
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_driver_script_diff.py"
)
set(SB_DRIVER_FULL_SURFACE_SCRIPT_SUITE_GATE
  "${PROJECT_SOURCE_DIR}/tests/conformance/drivers/full_surface_scripts/validate_full_surface_script_suite.py"
)
set(SB_DRIVER_IPAR_PERFORMANCE_PROOF_GATE
  "${PROJECT_SOURCE_DIR}/tests/conformance/drivers/full_surface_scripts/ipar_performance_proof_gate.py"
)
set(SB_DRIVER_IPAR_METRICS_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/driver_ipar_metrics_gate.py"
)
set(SB_DRIVER_DBEAVER_MANAGEMENT_GATE
  "${PROJECT_SOURCE_DIR}/tests/conformance/drivers/validate_dbeaver_management_platform_corpus.py"
)
set(SB_DRIVER_BUILD_ROOT
  "${PROJECT_BINARY_DIR}/drivers"
)

option(SB_DRIVER_ALLOW_TOOLCHAIN_WAIVERS
  "Allow missing non-critical driver toolchains to be reported as CTest skips"
  ON
)
option(SB_DRIVER_REQUIRE_ALL_TOOLCHAINS
  "Require every imported driver/adaptor/tool toolchain to be present"
  OFF
)
option(SB_BUILD_DRIVER_NATIVE_CMAKE_TARGETS
  "Build native CMake driver/tool targets as part of the driver gate subtree"
  ON
)
option(SB_DRIVER_STAGE_COMPILED_TOOLS
  "Compile supported per-language sb_isql driver tools during the native tool staging gate"
  ON
)
option(SB_DRIVER_STRICT_STAGED_RUNTIMES
  "Fail the staged driver tool gate when a runtime-only launcher dependency is missing"
  OFF
)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

file(MAKE_DIRECTORY "${SB_DRIVER_BUILD_ROOT}")

function(sb_add_driver_gate test_name mode)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_GATE_SCRIPT}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--project-root" "${PROJECT_SOURCE_DIR}"
      "--build-root" "${SB_DRIVER_BUILD_ROOT}"
      "--fixture-root" "${SB_DRIVER_FIXTURE_DIR}"
      "${mode}"
      ${ARGN}
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "driver;driver_lane_ctest_gate;${test_name}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
    SKIP_RETURN_CODE 77
  )
endfunction()

function(sb_add_driver_component_gate test_name component)
  set(toolchain_waiver_args)
  if(SB_DRIVER_ALLOW_TOOLCHAIN_WAIVERS)
    list(APPEND toolchain_waiver_args "--allow-toolchain-waivers")
  endif()
  if(SB_DRIVER_REQUIRE_ALL_TOOLCHAINS)
    list(APPEND toolchain_waiver_args "--require-all-toolchains")
  endif()
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_COMPONENT_RUNNER}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--project-root" "${PROJECT_SOURCE_DIR}"
      "--build-root" "${SB_DRIVER_BUILD_ROOT}"
      "--component" "${component}"
      ${toolchain_waiver_args}
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "driver;driver_lane_ctest_gate;driver_tool_adaptor_gate;${test_name}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
    SKIP_RETURN_CODE 77
  )
endfunction()

function(sb_add_driver_static_requirement_gate test_name mode)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_STATIC_REQUIREMENT_GATE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--mode" "${mode}"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "driver;driver_static_requirement_gate;${test_name}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
  )
endfunction()

function(sb_add_driver_dbeaver_management_gate test_name)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_DBEAVER_MANAGEMENT_GATE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "driver;driver_static_requirement_gate;dbeaver_management_platform_gate;${test_name}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
  )
endfunction()

function(sb_add_driver_native_full_surface_gate test_name mode)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_NATIVE_FULL_SURFACE_GATE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--mode" "${mode}"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "driver;driver_native_full_surface_gate;${test_name}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
  )
endfunction()

function(sb_add_driver_native_full_surface_artifact_schema_gate test_name)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_NATIVE_FULL_SURFACE_GATE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--mode" "artifact-schema"
      "--artifact-root" "${SB_DRIVER_BUILD_ROOT}/native-artifact-schema-fixture"
      "--generate-deterministic-fixture"
      "--output" "${SB_DRIVER_BUILD_ROOT}/reports/${test_name}.json"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "driver;driver_native_full_surface_gate;artifact_schema;${test_name}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
  )
endfunction()

function(sb_add_driver_native_matrix_plan_gate test_name)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_NATIVE_MATRIX_PLAN_GATE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--output" "${SB_DRIVER_BUILD_ROOT}/reports/${test_name}.json"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "driver;driver_native_full_surface_gate;matrix_plan;${test_name}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
  )
endfunction()

function(sb_add_driver_native_workplan_coverage_gate test_name)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_NATIVE_WORKPLAN_COVERAGE_GATE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--output" "${SB_DRIVER_BUILD_ROOT}/reports/${test_name}.json"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "driver;driver_native_full_surface_gate;workplan_coverage;${test_name}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
  )
endfunction()

function(sb_add_driver_native_full_surface_matrix_runner_gate test_name)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_NATIVE_FULL_SURFACE_MATRIX_RUNNER}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--plan-only"
      "--require-staged-tools"
      "--output" "${SB_DRIVER_BUILD_ROOT}/reports/${test_name}.json"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "driver;driver_native_full_surface_gate;matrix_orchestrator;${test_name}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
    FIXTURES_REQUIRED "driver_native_core_tools_staged"
  )
endfunction()

function(sb_add_driver_native_full_surface_matrix_shard_runner_gate test_name)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_NATIVE_FULL_SURFACE_MATRIX_RUNNER}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--plan-only"
      "--require-staged-tools"
      "--shard-count" "16"
      "--shard-index" "0"
      "--output" "${SB_DRIVER_BUILD_ROOT}/reports/${test_name}.json"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "driver;driver_native_full_surface_gate;matrix_orchestrator;matrix_shard_runner;${test_name}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
    FIXTURES_REQUIRED "driver_native_core_tools_staged"
  )
endfunction()

function(sb_add_driver_native_full_surface_matrix_finalizer_gate test_name)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_NATIVE_FULL_SURFACE_MATRIX_FINALIZER}"
      "--generate-deterministic-fixture"
      "--fixture-root" "${SB_DRIVER_BUILD_ROOT}/matrix-finalizer-fixture"
      "--output" "${SB_DRIVER_BUILD_ROOT}/reports/${test_name}.json"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "driver;driver_native_full_surface_gate;matrix_orchestrator;matrix_finalizer;${test_name}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
  )
endfunction()

function(sb_add_driver_native_matrix_lane_plan_gate test_name)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_NATIVE_MATRIX_LANE_PROVISIONER}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--runtime-root" "${SB_DRIVER_BUILD_ROOT}/native-matrix-lanes-plan"
      "--run-id" "ctest-plan"
      "plan"
      "--output" "${SB_DRIVER_BUILD_ROOT}/reports/${test_name}.json"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "driver;driver_native_full_surface_gate;matrix_orchestrator;matrix_lane_provisioner;${test_name}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
  )
endfunction()

function(sb_add_driver_native_tool_stage_gate test_name)
  set(stage_args)
  if(SB_DRIVER_STAGE_COMPILED_TOOLS)
    list(APPEND stage_args "--build-compiled")
  endif()
  if(SB_DRIVER_STRICT_STAGED_RUNTIMES)
    list(APPEND stage_args "--strict-runtimes")
  endif()
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_NATIVE_TOOL_BUILDER}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--build-root" "${PROJECT_BINARY_DIR}"
      "--manifest-out" "${SB_DRIVER_BUILD_ROOT}/reports/${test_name}.json"
      ${stage_args}
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "driver;driver_native_full_surface_gate;driver_native_tool_binaries;${test_name}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
    FIXTURES_SETUP "driver_native_core_tools_staged"
  )
endfunction()

function(sb_add_driver_script_diff_contract_gate test_name)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_SCRIPT_DIFF_RUNNER}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--bin-root" "${PROJECT_BINARY_DIR}/output/linux/bin"
      "--report" "${SB_DRIVER_BUILD_ROOT}/reports/${test_name}.json"
      "--contract-check"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "driver;driver_native_full_surface_gate;driver_script_diff_contract;${test_name}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
    FIXTURES_REQUIRED "driver_native_core_tools_staged"
  )
endfunction()

function(sb_add_driver_full_surface_script_suite_gate test_name)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_FULL_SURFACE_SCRIPT_SUITE_GATE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--output-root" "${SB_DRIVER_BUILD_ROOT}/full_surface_script_suite_sample"
      "--report" "${SB_DRIVER_BUILD_ROOT}/full_surface_script_suite_sample/report.json"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "driver;driver_native_full_surface_gate;driver_full_surface_script_suite;${test_name}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
  )
endfunction()

function(sb_add_driver_ipar_performance_proof_gate test_name mode)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_IPAR_PERFORMANCE_PROOF_GATE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--mode" "${mode}"
      "--output-root" "${SB_DRIVER_BUILD_ROOT}/ipar-performance-proof/${test_name}"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "driver;driver_native_full_surface_gate;driver_ipar_metrics_gate;ipar_performance_proof;${test_name}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
  )
endfunction()

function(sb_add_driver_ipar_artifact_gate test_name)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_IPAR_METRICS_GATE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--artifact-root" "${SB_DRIVER_BUILD_ROOT}/ipar-metrics-proof"
      "--shape" "all"
      "--output-root" "${SB_DRIVER_BUILD_ROOT}/ipar-performance-proof/${test_name}"
      "--report" "${SB_DRIVER_BUILD_ROOT}/reports/${test_name}.json"
      "--require-tls-route-proof"
      "--require-non-tls-route-proof"
      "--generate-deterministic-fixture"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "driver;driver_native_full_surface_gate;driver_ipar_metrics_gate;ipar_artifact_contract;ipar_deterministic_artifact_fixture;IPAR-P5-03-contract;IPAR-P7-03-contract;${test_name}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
  )
endfunction()

function(sb_add_driver_status_package_gate test_name mode)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_STATUS_PACKAGE_GATE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--project-root" "${PROJECT_SOURCE_DIR}"
      "--fixture-root" "${SB_DRIVER_SERVER_RECONCILIATION_FIXTURE_DIR}"
      "${mode}"
  )
  set(label_string "driver;driver_lane_ctest_gate;driver_server_reconciliation;${test_name}")
  foreach(extra_label IN LISTS ARGN)
    string(APPEND label_string ";${extra_label}")
  endforeach()
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "${label_string}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
  )
endfunction()

function(sb_add_driver_p4_route_gate test_name mode)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_P4_ROUTE_GATE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--project-root" "${PROJECT_SOURCE_DIR}"
      "--fixture-root" "${SB_DRIVER_SERVER_RECONCILIATION_FIXTURE_DIR}"
      "${mode}"
  )
  set(label_string "driver;driver_lane_ctest_gate;driver_server_reconciliation;${test_name}")
  foreach(extra_label IN LISTS ARGN)
    string(APPEND label_string ";${extra_label}")
  endforeach()
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "${label_string}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
  )
endfunction()

function(sb_add_driver_p4_docs_reference_gate test_name mode)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_P4_DOCS_REFERENCE_GATE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--project-root" "${PROJECT_SOURCE_DIR}"
      "--fixture-root" "${SB_DRIVER_SERVER_RECONCILIATION_FIXTURE_DIR}"
      "${mode}"
  )
  set(label_string "driver;driver_lane_ctest_gate;driver_server_reconciliation;${test_name}")
  foreach(extra_label IN LISTS ARGN)
    string(APPEND label_string ";${extra_label}")
  endforeach()
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "${label_string}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
  )
endfunction()

function(sb_add_driver_release_declaration_gate test_name mode)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_DRIVER_RELEASE_DECLARATION_GATE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--project-root" "${PROJECT_SOURCE_DIR}"
      "--fixture-root" "${SB_DRIVER_SERVER_RECONCILIATION_FIXTURE_DIR}"
      "${mode}"
  )
  set(label_string "driver;driver_lane_ctest_gate;driver_server_reconciliation;${test_name}")
  foreach(extra_label IN LISTS ARGN)
    string(APPEND label_string ";${extra_label}")
  endforeach()
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "${label_string}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPYCACHEPREFIX=${SB_DRIVER_BUILD_ROOT}/pycache"
  )
endfunction()

sb_add_driver_gate(driver_source_inventory_gate inventory)
sb_add_driver_gate(driver_old_path_gate old-paths)
sb_add_driver_gate(driver_build_artifact_isolation_gate artifact-isolation)
sb_add_driver_gate(driver_source_tree_write_guard source-tree-write)
sb_add_driver_gate(driver_toolchain_detection_gate toolchains)
sb_add_driver_gate(driver_dependency_policy_gate dependency-policy)
sb_add_driver_gate(driver_common_conformance_gate conformance-matrix)
sb_add_driver_gate(driver_package_manifest_gate package-manifest)
sb_add_driver_gate(driver_server_fixture_gate server-fixture)
sb_add_driver_gate(driver_packaging_install_gate packaging-policy)
sb_add_driver_gate(driver_benchmark_readiness_gate benchmark-policy)
sb_add_driver_gate(driver_execution_plan10_runner_gate execution_plan10)
sb_add_driver_gate(driver_static_hygiene_gate static-hygiene)
sb_add_driver_gate(driver_release_claim_gate release-claims)
sb_add_driver_gate(drivers_final_zero_drift_audit final-zero-drift)

sb_add_driver_static_requirement_gate(driver_sblr_uuid_bundle sblr_uuid_bundle)
sb_add_driver_static_requirement_gate(driver_schema_path_resolution schema_path_resolution)
sb_add_driver_static_requirement_gate(sbsql_language_resource sbsql_language_resource)
sb_add_driver_static_requirement_gate(driver_sblr_uuid_authorization sblr_uuid_authorization)
sb_add_driver_static_requirement_gate(driver_cache_lifecycle cache_lifecycle)
sb_add_driver_dbeaver_management_gate(dbeaver_management_platform_corpus_gate)
sb_add_driver_native_full_surface_gate(driver_native_full_surface_gate_input gate-input)
sb_add_driver_native_full_surface_gate(driver_native_full_surface_tool_inventory tool-inventory)
sb_add_driver_native_full_surface_artifact_schema_gate(driver_native_full_surface_artifact_schema)
sb_add_driver_native_matrix_plan_gate(driver_native_full_surface_matrix_plan)
sb_add_driver_native_workplan_coverage_gate(driver_native_workplan_coverage_gate)
sb_add_driver_native_tool_stage_gate(driver_native_core_tool_binaries)
sb_add_driver_script_diff_contract_gate(driver_native_script_diff_contract)
sb_add_driver_native_full_surface_matrix_runner_gate(driver_native_full_surface_matrix_runner_plan)
sb_add_driver_native_full_surface_matrix_shard_runner_gate(driver_native_full_surface_matrix_shard_runner_plan)
sb_add_driver_native_full_surface_matrix_finalizer_gate(driver_native_full_surface_matrix_finalizer_gate)
sb_add_driver_native_matrix_lane_plan_gate(driver_native_matrix_lane_provisioner_plan)
sb_add_driver_full_surface_script_suite_gate(driver_full_surface_script_suite_gate)
sb_add_driver_ipar_performance_proof_gate(driver_ipar_performance_schema_gate schema)
sb_add_driver_ipar_performance_proof_gate(driver_ipar_performance_plan_gate plan)
sb_add_driver_ipar_artifact_gate(driver_ipar_metrics_artifact_proof_gate)

sb_add_driver_status_package_gate(driver_row_status_manifest_gate driver-row-status DSR-030 DSR-032)
sb_add_driver_status_package_gate(adapter_row_status_manifest_gate adapter-row-status DSR-031 DSR-032)
sb_add_driver_status_package_gate(tool_row_status_manifest_gate tool-row-status DSR-032)
sb_add_driver_status_package_gate(driver_status_packaging_evidence_gate packaging-evidence DSR-033 DSR-032)
sb_add_driver_p4_route_gate(
  driver_server_verification_packets_gate
  server-verification-packets
  DSR-040
)
sb_add_driver_p4_route_gate(
  driver_full_route_benchmark_evidence_gate
  full-route-benchmark-evidence
  DSR-041
)
sb_add_driver_p4_route_gate(
  driver_performance_budget_threshold_gate
  performance-budgets
  DSR-043
)
sb_add_driver_p4_docs_reference_gate(
  driver_documentation_sample_app_evidence_gate
  documentation-sample-evidence
  DSR-044
)
sb_add_driver_p4_docs_reference_gate(
  driver_reference_compatibility_route_evidence_gate
  reference-driver-route-evidence
  DSR-045
)
sb_add_driver_release_declaration_gate(
  driver_server_release_declaration_gate
  validate
  DSR-052
)

set(SB_DRIVER_GATE_NAMES
  adbc
  python
  go
  rust
  node
  jdbc
  odbc
  cpp
  dotnet
  dart
  elixir
  flightsql
  julia
  mojo
  pascal
  perl
  php
  r
  r2dbc
  ruby
  swift
)
foreach(driver_name IN LISTS SB_DRIVER_GATE_NAMES)
  sb_add_driver_component_gate("driver_${driver_name}_gate" "driver:${driver_name}")
endforeach()

set(SB_ADAPTOR_GATE_NAMES
  airbyte:scratchbird-airbyte
  dbeaver:scratchbird-dbeaver-driver
  dbt:scratchbird-dbt-adapter
  hibernate:scratchbird-hibernate-dialect
  looker:scratchbird-looker
  metabase:scratchbird-metabase-driver
  powerbi:scratchbird-powerbi
  prisma:scratchbird-prisma-adapter
  sqlalchemy:scratchbird-sqlalchemy-dialect
  superset:scratchbird-superset-driver
  tableau:scratchbird-tableau
  typeorm:scratchbird-typeorm-adapter
)
foreach(adaptor_pair IN LISTS SB_ADAPTOR_GATE_NAMES)
  string(REPLACE ":" ";" adaptor_parts "${adaptor_pair}")
  list(GET adaptor_parts 0 adaptor_label)
  list(GET adaptor_parts 1 adaptor_dir)
  sb_add_driver_component_gate("adaptor_${adaptor_label}_gate" "adaptor:${adaptor_dir}")
endforeach()

sb_add_driver_component_gate(tool_cli_gate "tool:cli")

add_test(
  NAME drivers_all
  COMMAND
    "${Python3_EXECUTABLE}" "${SB_DRIVER_GATE_SCRIPT}"
    "--repo-root" "${PROJECT_SOURCE_DIR}/.."
    "--project-root" "${PROJECT_SOURCE_DIR}"
    "--build-root" "${SB_DRIVER_BUILD_ROOT}"
    "--fixture-root" "${SB_DRIVER_FIXTURE_DIR}"
    drivers-all
)
set_tests_properties(drivers_all PROPERTIES
  LABELS "driver;driver_lane_ctest_gate;driver_tool_adaptor_gate;drivers_all"
  SKIP_RETURN_CODE 77
)

if(SB_BUILD_DRIVER_NATIVE_CMAKE_TARGETS)
  if(NOT TARGET scratchbird_client)
    add_subdirectory(driver/cpp "${SB_DRIVER_BUILD_ROOT}/driver/cpp")
  endif()
  if(NOT TARGET scratchbird_mojo_client_bridge)
    add_subdirectory(driver/mojo "${SB_DRIVER_BUILD_ROOT}/driver/mojo")
  endif()
  add_subdirectory(driver/odbc "${SB_DRIVER_BUILD_ROOT}/driver/odbc")
  if(NOT TARGET sb_isql)
    add_subdirectory(tool/cli "${SB_DRIVER_BUILD_ROOT}/tool/cli")
  endif()
endif()
