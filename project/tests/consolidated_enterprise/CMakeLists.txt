# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(SB_PRIVATE_REPO_ROOT "${PROJECT_SOURCE_DIR}/..")
set(SB_CONSOLIDATED_ENTERPRISE_PROOF_GATE
  "${SB_PRIVATE_REPO_ROOT}/project/tools/consolidated_enterprise_proof_gate.py"
)

if(NOT TARGET sb_core_memory)
  add_subdirectory(
    ${PROJECT_SOURCE_DIR}/src/core/memory
    ${CMAKE_BINARY_DIR}/core_memory_for_consolidated_enterprise_tests
  )
endif()
if(NOT TARGET sb_udr_runtime)
  add_subdirectory(
    ${PROJECT_SOURCE_DIR}/src/udr/runtime
    ${CMAKE_BINARY_DIR}/udr_runtime_for_consolidated_enterprise_tests
  )
endif()
if(NOT TARGET sb_storage_page)
  add_subdirectory(
    ${PROJECT_SOURCE_DIR}/src/storage/page
    ${CMAKE_BINARY_DIR}/storage_page_for_consolidated_enterprise_tests
  )
endif()
if(NOT TARGET sb_core_index)
  add_subdirectory(
    ${PROJECT_SOURCE_DIR}/src/core/index
    ${CMAKE_BINARY_DIR}/core_index_for_consolidated_enterprise_tests
  )
endif()
if(NOT TARGET sb_engine_optimizer_contract_only)
  add_subdirectory(
    ${PROJECT_SOURCE_DIR}/src/engine/optimizer
    ${CMAKE_BINARY_DIR}/engine_optimizer_for_consolidated_enterprise_tests
  )
endif()
if(NOT TARGET sb_engine_native_compile)
  add_subdirectory(
    ${PROJECT_SOURCE_DIR}/src/engine/native_compile
    ${CMAKE_BINARY_DIR}/engine_native_compile_for_consolidated_enterprise_tests
  )
endif()
if(NOT TARGET sb_engine_executor)
  add_subdirectory(
    ${PROJECT_SOURCE_DIR}/src/engine/executor
    ${CMAKE_BINARY_DIR}/engine_executor_for_consolidated_enterprise_tests
  )
endif()
if(NOT TARGET sb_wire_result_batch)
  add_subdirectory(
    ${PROJECT_SOURCE_DIR}/src/wire
    ${CMAKE_BINARY_DIR}/wire_result_batch_for_consolidated_enterprise_tests
  )
endif()

add_custom_target(consolidated_enterprise_proof_control_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_CONSOLIDATED_ENTERPRISE_PROOF_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
  COMMENT "Validating consolidated enterprise proof-control traceability"
)

add_test(
  NAME consolidated_enterprise_proof_control_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_CONSOLIDATED_ENTERPRISE_PROOF_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(consolidated_enterprise_proof_control_gate PROPERTIES
  LABELS "consolidated_enterprise_proof_control_gate;CEIC-001;CEIC-002;CEIC-GATE-001;CEIC-GATE-002;proof_control;traceability"
)

set(SB_CEIC_003_MGA_AUTHORITY_BOUNDARY_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/ceic_003_mga_authority_boundary_gate.py"
)

add_custom_target(ceic_003_mga_authority_boundary_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_003_MGA_AUTHORITY_BOUNDARY_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
  COMMENT "Validating CEIC-003 MGA authority boundaries"
)

add_test(
  NAME ceic_003_mga_authority_boundary_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_003_MGA_AUTHORITY_BOUNDARY_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(ceic_003_mga_authority_boundary_gate PROPERTIES
  LABELS "ceic_003_mga_authority_boundary_gate;CEIC-003;CEIC-GATE-003;MGA;authority_boundary;no_wal_finality"
)

set(SB_CEIC_004_PRODUCTION_TEST_SEPARATION_GATE
  "${PROJECT_SOURCE_DIR}/cmake/CommercialReadinessProductionBuildGateMatrix.cmake"
)

add_custom_target(ceic_004_production_test_separation_gate_check
  COMMAND "${CMAKE_COMMAND}" -P "${SB_CEIC_004_PRODUCTION_TEST_SEPARATION_GATE}"
  COMMENT "Validating CEIC-004 production/test separation with the production build gate matrix"
)

add_test(
  NAME ceic_004_production_test_separation_gate
  COMMAND "${CMAKE_COMMAND}" -P "${SB_CEIC_004_PRODUCTION_TEST_SEPARATION_GATE}"
)
set_tests_properties(ceic_004_production_test_separation_gate PROPERTIES
  LABELS "ceic_004_production_test_separation_gate;CEIC-004;CEIC-GATE-004;production_build;test_separation;cluster_boundary"
)

add_executable(
  ceic_010_sharded_memory_accounting_ledger_gate
  ceic_010_sharded_memory_accounting_ledger_gate.cpp
)
target_compile_features(ceic_010_sharded_memory_accounting_ledger_gate PRIVATE cxx_std_20)
target_link_libraries(
  ceic_010_sharded_memory_accounting_ledger_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME ceic_010_sharded_memory_accounting_ledger_gate
  COMMAND ceic_010_sharded_memory_accounting_ledger_gate
)
set_tests_properties(ceic_010_sharded_memory_accounting_ledger_gate PROPERTIES
  LABELS "ceic_010_sharded_memory_accounting_ledger_gate;CEIC-010;CEIC-GATE-010;memory;sharded_accounting"
)

add_executable(
  ceic_011_hierarchical_memory_budget_ledger_gate
  ceic_011_hierarchical_memory_budget_ledger_gate.cpp
)
target_compile_features(ceic_011_hierarchical_memory_budget_ledger_gate PRIVATE cxx_std_20)
target_link_libraries(
  ceic_011_hierarchical_memory_budget_ledger_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME ceic_011_hierarchical_memory_budget_ledger_gate
  COMMAND ceic_011_hierarchical_memory_budget_ledger_gate
)
set_tests_properties(ceic_011_hierarchical_memory_budget_ledger_gate PROPERTIES
  LABELS "ceic_011_hierarchical_memory_budget_ledger_gate;CEIC-011;CEIC-GATE-011;memory;hierarchical_budget;reservation_lifecycle"
)

add_executable(
  ceic_012_query_operator_planner_parser_arenas_gate
  ceic_012_query_operator_planner_parser_arenas_gate.cpp
  ${PROJECT_SOURCE_DIR}/src/engine/executor/reservation_backed_executor_memory_bridge.cpp
  ${PROJECT_SOURCE_DIR}/src/engine/executor/vectorized_result_batch.cpp
  ${PROJECT_SOURCE_DIR}/src/engine/optimizer/reservation_backed_optimizer_memory_bridge.cpp
  ${PROJECT_SOURCE_DIR}/src/engine/planner/reservation_backed_planner_memory_bridge.cpp
  ${PROJECT_SOURCE_DIR}/src/engine/sblr/sblr_parser_memory_handoff.cpp
)
target_compile_features(
  ceic_012_query_operator_planner_parser_arenas_gate
  PRIVATE
    cxx_std_20
)
target_include_directories(
  ceic_012_query_operator_planner_parser_arenas_gate
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core/agents
    ${PROJECT_SOURCE_DIR}/src/core/memory
    ${PROJECT_SOURCE_DIR}/src/core/platform
    ${PROJECT_SOURCE_DIR}/src/engine/executor
    ${PROJECT_SOURCE_DIR}/src/engine/optimizer
    ${PROJECT_SOURCE_DIR}/src/engine/planner
    ${PROJECT_SOURCE_DIR}/src/engine/sblr
    ${PROJECT_SOURCE_DIR}/src/udr/runtime
)
target_link_libraries(
  ceic_012_query_operator_planner_parser_arenas_gate
  PRIVATE
    sb_core_memory
    sb_udr_runtime
)
add_test(
  NAME ceic_012_query_operator_planner_parser_arenas_gate
  COMMAND ceic_012_query_operator_planner_parser_arenas_gate
)
set_tests_properties(ceic_012_query_operator_planner_parser_arenas_gate PROPERTIES
  LABELS "ceic_012_query_operator_planner_parser_arenas_gate;CEIC-012;CEIC-GATE-010;CEIC-GATE-011;memory;executor;planner;optimizer;parser;udr;background;result_frame;reservation_backed_resource"
)

add_executable(
  ceic_013_typed_slab_pool_gate
  ceic_013_typed_slab_pool_gate.cpp
  ${PROJECT_SOURCE_DIR}/src/engine/executor/typed_slab_executor_work_area.cpp
  ${PROJECT_SOURCE_DIR}/src/engine/optimizer/typed_slab_planner_work_area.cpp
  ${PROJECT_SOURCE_DIR}/src/core/index/typed_slab_index_work_area.cpp
)
target_compile_features(
  ceic_013_typed_slab_pool_gate
  PRIVATE
    cxx_std_20
)
target_include_directories(
  ceic_013_typed_slab_pool_gate
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core/index
    ${PROJECT_SOURCE_DIR}/src/core/memory
    ${PROJECT_SOURCE_DIR}/src/core/platform
    ${PROJECT_SOURCE_DIR}/src/engine/executor
    ${PROJECT_SOURCE_DIR}/src/engine/optimizer
)
target_link_libraries(
  ceic_013_typed_slab_pool_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME ceic_013_typed_slab_pool_gate
  COMMAND ceic_013_typed_slab_pool_gate
)
set_tests_properties(ceic_013_typed_slab_pool_gate PROPERTIES
  LABELS "ceic_013_typed_slab_pool_gate;CEIC-013;CEIC-GATE-010;CEIC-GATE-012;memory;typed_slab;size_class;executor;planner;index;diagnostics;reservation_backed_resource"
)

add_executable(
  ceic_014_thread_local_per_core_cache_gate
  ceic_014_thread_local_per_core_cache_gate.cpp
)
target_compile_features(
  ceic_014_thread_local_per_core_cache_gate
  PRIVATE
    cxx_std_20
)
target_include_directories(
  ceic_014_thread_local_per_core_cache_gate
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core/memory
    ${PROJECT_SOURCE_DIR}/src/core/platform
)
target_link_libraries(
  ceic_014_thread_local_per_core_cache_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME ceic_014_thread_local_per_core_cache_gate
  COMMAND ceic_014_thread_local_per_core_cache_gate
)
set_tests_properties(ceic_014_thread_local_per_core_cache_gate PROPERTIES
  LABELS "ceic_014_thread_local_per_core_cache_gate;CEIC-014;CEIC-GATE-010;CEIC-GATE-012;memory;thread_local_cache;per_core;numa_fallback;reservation_backed_resource;typed_slab"
)

add_executable(
  ceic_015_remote_free_queue_gate
  ceic_015_remote_free_queue_gate.cpp
)
target_compile_features(
  ceic_015_remote_free_queue_gate
  PRIVATE
    cxx_std_20
)
target_include_directories(
  ceic_015_remote_free_queue_gate
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core/memory
    ${PROJECT_SOURCE_DIR}/src/core/platform
)
target_link_libraries(
  ceic_015_remote_free_queue_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME ceic_015_remote_free_queue_gate
  COMMAND ceic_015_remote_free_queue_gate
)
set_tests_properties(ceic_015_remote_free_queue_gate PROPERTIES
  LABELS "ceic_015_remote_free_queue_gate;CEIC-015;CEIC-GATE-010;CEIC-GATE-012;memory;remote_free_queue;ownership_reconciliation;reservation_backed_resource;typed_slab"
)

add_executable(
  ceic_016_foreign_memory_reservation_gate
  ceic_016_foreign_memory_reservation_gate.cpp
)
target_compile_features(
  ceic_016_foreign_memory_reservation_gate
  PRIVATE
    cxx_std_20
)
target_include_directories(
  ceic_016_foreign_memory_reservation_gate
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core/memory
    ${PROJECT_SOURCE_DIR}/src/core/platform
)
target_link_libraries(
  ceic_016_foreign_memory_reservation_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME ceic_016_foreign_memory_reservation_gate
  COMMAND ceic_016_foreign_memory_reservation_gate
)
set_tests_properties(ceic_016_foreign_memory_reservation_gate PROPERTIES
  LABELS "ceic_016_foreign_memory_reservation_gate;CEIC-016;CEIC-GATE-011;CEIC-GATE-015;memory;foreign_memory;native_memory;llvm;plugin_udr;driver_native"
)

add_executable(
  ceic_061_llvm_memory_accounting_gate
  ceic_061_llvm_memory_accounting_gate.cpp
)
target_compile_features(
  ceic_061_llvm_memory_accounting_gate
  PRIVATE
    cxx_std_20
)
target_include_directories(
  ceic_061_llvm_memory_accounting_gate
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core/memory
    ${PROJECT_SOURCE_DIR}/src/core/platform
    ${PROJECT_SOURCE_DIR}/src/engine/native_compile
)
target_link_libraries(
  ceic_061_llvm_memory_accounting_gate
  PRIVATE
    sb_core_memory
    sb_engine_native_compile
)
add_test(
  NAME ceic_061_llvm_memory_accounting_gate
  COMMAND ceic_061_llvm_memory_accounting_gate
)
set_tests_properties(ceic_061_llvm_memory_accounting_gate PROPERTIES
  LABELS "ceic_061_llvm_memory_accounting_gate;CEIC-061;CEIC-GATE-015;optimizer;build;memory;foreign_memory;llvm;dynamic_linkage;static_linkage;native_compile;support_bundle;no_authority_drift"
)

add_executable(
  ceic_017_memory_pressure_state_machine_gate
  ceic_017_memory_pressure_state_machine_gate.cpp
)
target_compile_features(
  ceic_017_memory_pressure_state_machine_gate
  PRIVATE
    cxx_std_20
)
target_include_directories(
  ceic_017_memory_pressure_state_machine_gate
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core/memory
    ${PROJECT_SOURCE_DIR}/src/core/platform
)
target_link_libraries(
  ceic_017_memory_pressure_state_machine_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME ceic_017_memory_pressure_state_machine_gate
  COMMAND ceic_017_memory_pressure_state_machine_gate
)
set_tests_properties(ceic_017_memory_pressure_state_machine_gate PROPERTIES
  LABELS "ceic_017_memory_pressure_state_machine_gate;CEIC-017;CEIC-GATE-013;memory;pressure_state_machine;hard_oom_survival;emergency_reserve;low_memory_diagnostics;authority_boundary"
)

add_executable(
  ceic_018_secure_temp_spill_reservation_gate
  ceic_018_secure_temp_spill_reservation_gate.cpp
)
target_compile_features(
  ceic_018_secure_temp_spill_reservation_gate
  PRIVATE
    cxx_std_20
)
target_include_directories(
  ceic_018_secure_temp_spill_reservation_gate
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core/memory
    ${PROJECT_SOURCE_DIR}/src/core/platform
)
target_link_libraries(
  ceic_018_secure_temp_spill_reservation_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME ceic_018_secure_temp_spill_reservation_gate
  COMMAND ceic_018_secure_temp_spill_reservation_gate
)
set_tests_properties(ceic_018_secure_temp_spill_reservation_gate PROPERTIES
  LABELS "ceic_018_secure_temp_spill_reservation_gate;CEIC-018;CEIC-GATE-011;CEIC-GATE-013;memory;temp_workspace;secure_temp_spill;reservation_lifecycle;authority_boundary"
)

add_executable(
  ceic_019_page_cache_sharded_frame_pool_gate
  ceic_019_page_cache_sharded_frame_pool_gate.cpp
)
target_compile_features(
  ceic_019_page_cache_sharded_frame_pool_gate
  PRIVATE
    cxx_std_20
)
target_include_directories(
  ceic_019_page_cache_sharded_frame_pool_gate
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core/memory
    ${PROJECT_SOURCE_DIR}/src/core/platform
    ${PROJECT_SOURCE_DIR}/src/storage/page
)
target_link_libraries(
  ceic_019_page_cache_sharded_frame_pool_gate
  PRIVATE
    sb_core_memory
    sb_storage_page
)
add_test(
  NAME ceic_019_page_cache_sharded_frame_pool_gate
  COMMAND ceic_019_page_cache_sharded_frame_pool_gate
)
set_tests_properties(ceic_019_page_cache_sharded_frame_pool_gate PROPERTIES
  LABELS "ceic_019_page_cache_sharded_frame_pool_gate;CEIC-019;CEIC-GATE-014;memory;storage;page_cache;sharded_frame_pool;frame_ownership;authority_boundary"
)

add_executable(
  ceic_020_result_cursor_plan_memory_governance_gate
  ceic_020_result_cursor_plan_memory_governance_gate.cpp
)
target_compile_features(
  ceic_020_result_cursor_plan_memory_governance_gate
  PRIVATE
    cxx_std_20
)
target_include_directories(
  ceic_020_result_cursor_plan_memory_governance_gate
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core/memory
    ${PROJECT_SOURCE_DIR}/src/core/platform
    ${PROJECT_SOURCE_DIR}/src/engine/executor
    ${PROJECT_SOURCE_DIR}/src/engine/internal_api
    ${PROJECT_SOURCE_DIR}/src/engine/optimizer
    ${PROJECT_SOURCE_DIR}/src/wire
)
target_link_libraries(
  ceic_020_result_cursor_plan_memory_governance_gate
  PRIVATE
    sb_core_memory
    sb_engine_executor
    sb_engine_optimizer_contract_only
    sb_wire_result_batch
)
add_test(
  NAME ceic_020_result_cursor_plan_memory_governance_gate
  COMMAND ceic_020_result_cursor_plan_memory_governance_gate
)
set_tests_properties(ceic_020_result_cursor_plan_memory_governance_gate PROPERTIES
  LABELS "ceic_020_result_cursor_plan_memory_governance_gate;CEIC-020;CEIC-GATE-011;memory;wire;optimizer;executor;result_cursor;plan_cache;prepared_template;reservation_lifecycle;authority_boundary"
)

set(SB_CEIC_021_MEMORY_SOURCE_GATE_TEST
  "${CMAKE_CURRENT_SOURCE_DIR}/ceic_021_memory_source_gate_test.py"
)

add_custom_target(ceic_021_memory_source_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_021_MEMORY_SOURCE_GATE_TEST}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
  COMMENT "Validating CEIC-021 raw hot-path memory source gate"
)

add_test(
  NAME ceic_021_memory_source_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_021_MEMORY_SOURCE_GATE_TEST}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(ceic_021_memory_source_gate PROPERTIES
  LABELS "ceic_021_memory_source_gate;CEIC-021;CEIC-GATE-010;memory;source_gate;raw_allocation;hot_path;allowlist;authority_boundary"
)

set(SB_CEIC_022_MEMORY_INVARIANT_GATE_TEST
  "${CMAKE_CURRENT_SOURCE_DIR}/ceic_022_memory_invariant_gate_test.py"
)

add_custom_target(ceic_022_memory_invariant_static_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_022_MEMORY_INVARIANT_GATE_TEST}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
  COMMENT "Validating CEIC-022 formal memory invariant and verification-lane definitions"
)

add_test(
  NAME ceic_022_memory_invariant_static_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_022_MEMORY_INVARIANT_GATE_TEST}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(ceic_022_memory_invariant_static_gate PROPERTIES
  LABELS "ceic_022_memory_invariant_static_gate;CEIC-022;CEIC-GATE-013;CEIC-GATE-051;CEIC-GATE-052;memory;invariants;static_analysis;sanitizer;soak_definition;authority_boundary"
)

add_executable(
  ceic_022_memory_reservation_model_gate
  ceic_022_memory_reservation_model_gate.cpp
)
target_compile_features(
  ceic_022_memory_reservation_model_gate
  PRIVATE
    cxx_std_20
)
target_include_directories(
  ceic_022_memory_reservation_model_gate
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core/memory
    ${PROJECT_SOURCE_DIR}/src/core/platform
)
target_link_libraries(
  ceic_022_memory_reservation_model_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME ceic_022_memory_reservation_model_gate
  COMMAND ceic_022_memory_reservation_model_gate
)
set_tests_properties(ceic_022_memory_reservation_model_gate PROPERTIES
  LABELS "ceic_022_memory_reservation_model_gate;CEIC-022;CEIC-GATE-011;CEIC-GATE-013;CEIC-GATE-051;memory;reservation_model;concurrency;high_churn;authority_boundary"
)

add_executable(
  ceic_023_memory_support_bundle_protected_gate
  ceic_023_memory_support_bundle_protected_gate.cpp
)
target_compile_features(
  ceic_023_memory_support_bundle_protected_gate
  PRIVATE
    cxx_std_20
)
target_include_directories(
  ceic_023_memory_support_bundle_protected_gate
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core/memory
    ${PROJECT_SOURCE_DIR}/src/core/metrics
    ${PROJECT_SOURCE_DIR}/src/core/platform
)
target_link_libraries(
  ceic_023_memory_support_bundle_protected_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME ceic_023_memory_support_bundle_protected_gate
  COMMAND ceic_023_memory_support_bundle_protected_gate
)
set_tests_properties(ceic_023_memory_support_bundle_protected_gate PROPERTIES
  LABELS "ceic_023_memory_support_bundle_protected_gate;CEIC-023;CEIC-GATE-013;CEIC-GATE-050;memory;support_bundle;low_memory;protected_memory;redaction;authority_boundary"
)

set(SB_CEIC_024_MEMORY_READINESS_MANIFEST_GATE_TEST
  "${CMAKE_CURRENT_SOURCE_DIR}/ceic_024_memory_readiness_manifest_gate_test.py"
)

add_custom_target(ceic_024_memory_readiness_manifest_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_024_MEMORY_READINESS_MANIFEST_GATE_TEST}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
  COMMENT "Validating CEIC-024 generated memory readiness manifest"
)

add_test(
  NAME ceic_024_memory_readiness_manifest_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_024_MEMORY_READINESS_MANIFEST_GATE_TEST}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(ceic_024_memory_readiness_manifest_gate PROPERTIES
  LABELS "ceic_024_memory_readiness_manifest_gate;CEIC-024;CEIC-GATE-012;memory;readiness_manifest;source_evidence;support_bundle;metrics;platform;production_gate;authority_boundary"
)

add_executable(
  ceic_025_multi_tenant_memory_fairness_gate
  ceic_025_multi_tenant_memory_fairness_gate.cpp
)
target_compile_features(
  ceic_025_multi_tenant_memory_fairness_gate
  PRIVATE
    cxx_std_20
)
target_include_directories(
  ceic_025_multi_tenant_memory_fairness_gate
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core/memory
    ${PROJECT_SOURCE_DIR}/src/core/platform
)
target_link_libraries(
  ceic_025_multi_tenant_memory_fairness_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME ceic_025_multi_tenant_memory_fairness_gate
  COMMAND ceic_025_multi_tenant_memory_fairness_gate
)
set_tests_properties(ceic_025_multi_tenant_memory_fairness_gate PROPERTIES
  LABELS "ceic_025_multi_tenant_memory_fairness_gate;CEIC-025;CEIC-GATE-011;memory;resource_governance;tenant_fairness;foreground_protection;burst;spill_throttle_cancel;authority_boundary"
)

add_executable(
  ceic_026_memory_class_policy_lease_gate
  ceic_026_memory_class_policy_lease_gate.cpp
)
target_compile_features(
  ceic_026_memory_class_policy_lease_gate
  PRIVATE
    cxx_std_20
)
target_include_directories(
  ceic_026_memory_class_policy_lease_gate
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core/memory
    ${PROJECT_SOURCE_DIR}/src/core/platform
)
target_link_libraries(
  ceic_026_memory_class_policy_lease_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME ceic_026_memory_class_policy_lease_gate
  COMMAND ceic_026_memory_class_policy_lease_gate
)
set_tests_properties(ceic_026_memory_class_policy_lease_gate PROPERTIES
  LABELS "ceic_026_memory_class_policy_lease_gate;CEIC-026;CEIC-GATE-011;CEIC-GATE-013;memory;resource_governance;memory_classes;budget_leases;pressure_actions;authority_boundary"
)

add_executable(
  ceic_027_plugin_native_memory_sandbox_gate
  ceic_027_plugin_native_memory_sandbox_gate.cpp
)
target_compile_features(
  ceic_027_plugin_native_memory_sandbox_gate
  PRIVATE
    cxx_std_20
)
target_include_directories(
  ceic_027_plugin_native_memory_sandbox_gate
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core/memory
    ${PROJECT_SOURCE_DIR}/src/core/platform
)
target_link_libraries(
  ceic_027_plugin_native_memory_sandbox_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME ceic_027_plugin_native_memory_sandbox_gate
  COMMAND ceic_027_plugin_native_memory_sandbox_gate
)
set_tests_properties(ceic_027_plugin_native_memory_sandbox_gate PROPERTIES
  LABELS "ceic_027_plugin_native_memory_sandbox_gate;CEIC-027;CEIC-GATE-011;CEIC-GATE-015;memory;plugin_udr;native_memory;sandbox;allocator_abi;plugin_context;invocation_budget;pressure_cancel;unload_cleanup;authority_boundary"
)

add_executable(
  ceic_028_fragmentation_profiler_diff_gate
  ceic_028_fragmentation_profiler_diff_gate.cpp
)
target_compile_features(
  ceic_028_fragmentation_profiler_diff_gate
  PRIVATE
    cxx_std_20
)
target_include_directories(
  ceic_028_fragmentation_profiler_diff_gate
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core/memory
    ${PROJECT_SOURCE_DIR}/src/core/metrics
    ${PROJECT_SOURCE_DIR}/src/core/platform
)
target_link_libraries(
  ceic_028_fragmentation_profiler_diff_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME ceic_028_fragmentation_profiler_diff_gate
  COMMAND ceic_028_fragmentation_profiler_diff_gate
)
set_tests_properties(ceic_028_fragmentation_profiler_diff_gate PROPERTIES
  LABELS "ceic_028_fragmentation_profiler_diff_gate;CEIC-028;CEIC-GATE-010;CEIC-GATE-012;CEIC-GATE-013;memory;typed_arena;typed_slab;fragmentation;profiler_diff;support_bundle;authority_boundary"
)

add_executable(
  ceic_029_adaptive_batch_working_set_locality_gate
  ceic_029_adaptive_batch_working_set_locality_gate.cpp
)
target_compile_features(
  ceic_029_adaptive_batch_working_set_locality_gate
  PRIVATE
    cxx_std_20
)
target_include_directories(
  ceic_029_adaptive_batch_working_set_locality_gate
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core/memory
    ${PROJECT_SOURCE_DIR}/src/core/metrics
    ${PROJECT_SOURCE_DIR}/src/core/platform
    ${PROJECT_SOURCE_DIR}/src/storage/page
)
target_link_libraries(
  ceic_029_adaptive_batch_working_set_locality_gate
  PRIVATE
    sb_core_memory
    sb_storage_page
)
add_test(
  NAME ceic_029_adaptive_batch_working_set_locality_gate
  COMMAND ceic_029_adaptive_batch_working_set_locality_gate
)
set_tests_properties(ceic_029_adaptive_batch_working_set_locality_gate PROPERTIES
  LABELS "ceic_029_adaptive_batch_working_set_locality_gate;CEIC-029;CEIC-GATE-013;CEIC-GATE-014;memory;adaptive_batch;working_set;page_cache;locality;support_bundle;metrics;authority_boundary"
)

set(SB_CEIC_030_INDEX_READINESS_MANIFEST_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/ceic_030_index_readiness_manifest_gate_test.py"
)

add_custom_target(ceic_030_index_readiness_manifest_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_030_INDEX_READINESS_MANIFEST_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
  COMMENT "Validating CEIC-030 generated index readiness manifest"
)

add_test(
  NAME ceic_030_index_readiness_manifest_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_030_INDEX_READINESS_MANIFEST_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(ceic_030_index_readiness_manifest_gate PROPERTIES
  LABELS "ceic_030_index_readiness_manifest_gate;CEIC-030;CEIC-GATE-020;indexes;readiness_manifest;provider_classification;route_capability;MGA;security;metrics;benchmark;crash;corruption;cleanup;authority_boundary"
)

add_executable(
  ceic_031_index_access_method_provider_gate
  ceic_031_index_access_method_provider_gate.cpp
)
target_compile_features(
  ceic_031_index_access_method_provider_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_031_index_access_method_provider_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME ceic_031_index_access_method_provider_gate
  COMMAND ceic_031_index_access_method_provider_gate
)
set_tests_properties(ceic_031_index_access_method_provider_gate PROPERTIES
  LABELS "ceic_031_index_access_method_provider_gate;CEIC-031;CEIC-GATE-020;indexes;access_method;provider_contract;route_capability;generation;recovery;cleanup;validation_repair;cluster_external_provider;authority_boundary"
)

add_executable(
  ceic_032_index_mga_recovery_contract_gate
  ceic_032_index_mga_recovery_contract_gate.cpp
)
target_compile_features(
  ceic_032_index_mga_recovery_contract_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_032_index_mga_recovery_contract_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME ceic_032_index_mga_recovery_contract_gate
  COMMAND ceic_032_index_mga_recovery_contract_gate
)
set_tests_properties(ceic_032_index_mga_recovery_contract_gate PROPERTIES
  LABELS "ceic_032_index_mga_recovery_contract_gate;CEIC-032;CEIC-GATE-021;indexes;MGA;COW;generation_publish;recovery_classification;cleanup_horizon;validation_rebuild_replay;cluster_external_provider;authority_boundary"
)

add_executable(
  ceic_033_btree_unique_durable_provider_closure_gate
  ceic_033_btree_unique_durable_provider_closure_gate.cpp
)
target_compile_features(
  ceic_033_btree_unique_durable_provider_closure_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_033_btree_unique_durable_provider_closure_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME ceic_033_btree_unique_durable_provider_closure_gate
  COMMAND ceic_033_btree_unique_durable_provider_closure_gate
)
set_tests_properties(ceic_033_btree_unique_durable_provider_closure_gate PROPERTIES
  LABELS "ceic_033_btree_unique_durable_provider_closure_gate;CEIC-033;CEIC-GATE-021;CEIC-GATE-024;indexes;btree;unique_btree;durable_provider;COW;root_identity;split_merge_scan;cleanup_horizon;crash_corruption;authority_boundary"
)

add_executable(
  ceic_034_unique_reservation_finality_protocol_gate
  ceic_034_unique_reservation_finality_protocol_gate.cpp
)
target_compile_features(
  ceic_034_unique_reservation_finality_protocol_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_034_unique_reservation_finality_protocol_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME ceic_034_unique_reservation_finality_protocol_gate
  COMMAND ceic_034_unique_reservation_finality_protocol_gate
)
set_tests_properties(ceic_034_unique_reservation_finality_protocol_gate PROPERTIES
  LABELS "ceic_034_unique_reservation_finality_protocol_gate;CEIC-034;CEIC-GATE-021;CEIC-GATE-024;indexes;unique_btree;reservation;finality_protocol;MGA;duplicate_preflight;deferred_unique;rollback_retry_cleanup;crash_window;authority_boundary"
)

add_executable(
  ceic_035_hash_durable_provider_closure_gate
  ceic_035_hash_durable_provider_closure_gate.cpp
)
target_compile_features(
  ceic_035_hash_durable_provider_closure_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_035_hash_durable_provider_closure_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME ceic_035_hash_durable_provider_closure_gate
  COMMAND ceic_035_hash_durable_provider_closure_gate
)
set_tests_properties(ceic_035_hash_durable_provider_closure_gate PROPERTIES
  LABELS "ceic_035_hash_durable_provider_closure_gate;CEIC-035;CEIC-GATE-021;CEIC-GATE-024;indexes;hash;durable_provider;COW;directory_bucket_overflow;collision_chain;full_key_recheck;cleanup_compaction;crash_corruption;authority_boundary"
)

add_executable(
  ceic_036_canonical_key_ordering_encoding_fuzz_gate
  ceic_036_canonical_key_ordering_encoding_fuzz_gate.cpp
)
target_compile_features(
  ceic_036_canonical_key_ordering_encoding_fuzz_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_036_canonical_key_ordering_encoding_fuzz_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME ceic_036_canonical_key_ordering_encoding_fuzz_gate
  COMMAND ceic_036_canonical_key_ordering_encoding_fuzz_gate
)
set_tests_properties(ceic_036_canonical_key_ordering_encoding_fuzz_gate PROPERTIES
  LABELS "ceic_036_canonical_key_ordering_encoding_fuzz_gate;CEIC-036;CEIC-GATE-020;indexes;canonical_key_ordering;SBKO;typed_comparable_payload;null_ordering;collation_key;numeric;text;binary;composite;expression;partial_predicate;prefix_bounds;authority_boundary"
)

add_executable(
  ceic_037_engine_owned_exact_recheck_gate
  ceic_037_engine_owned_exact_recheck_gate.cpp
)
target_compile_features(
  ceic_037_engine_owned_exact_recheck_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_037_engine_owned_exact_recheck_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME ceic_037_engine_owned_exact_recheck_gate
  COMMAND ceic_037_engine_owned_exact_recheck_gate
)
set_tests_properties(ceic_037_engine_owned_exact_recheck_gate PROPERTIES
  LABELS "ceic_037_engine_owned_exact_recheck_gate;CEIC-037;CEIC-GATE-022;indexes;exact_recheck;MGA;security;authorization;predicate;source_payload;rerank;authority_boundary"
)

add_executable(
  ceic_038_lossy_pruning_family_classification_gate
  ceic_038_lossy_pruning_family_classification_gate.cpp
)
target_compile_features(
  ceic_038_lossy_pruning_family_classification_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_038_lossy_pruning_family_classification_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME ceic_038_lossy_pruning_family_classification_gate
  COMMAND ceic_038_lossy_pruning_family_classification_gate
)
set_tests_properties(ceic_038_lossy_pruning_family_classification_gate PROPERTIES
  LABELS "ceic_038_lossy_pruning_family_classification_gate;CEIC-038;CEIC-GATE-022;indexes;lossy_candidate;pruning;Bloom;BRIN;bitmap;text;vector;document;graph;spatial;rerank;fallback;authority_boundary"
)

add_executable(
  ceic_039_specialized_persistent_provider_closure_gate
  ceic_039_specialized_persistent_provider_closure_gate.cpp
)
target_compile_features(
  ceic_039_specialized_persistent_provider_closure_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_039_specialized_persistent_provider_closure_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME ceic_039_specialized_persistent_provider_closure_gate
  COMMAND ceic_039_specialized_persistent_provider_closure_gate
)
set_tests_properties(ceic_039_specialized_persistent_provider_closure_gate PROPERTIES
  LABELS "ceic_039_specialized_persistent_provider_closure_gate;CEIC-039;CEIC-GATE-021;CEIC-GATE-022;CEIC-GATE-024;indexes;specialized_provider;full_text;GIN;ngram;vector;spatial;document;graph;columnar;MGA;exact_recheck;authority_boundary"
)

add_executable(
  ceic_040_index_operation_metrics_support_bundle_gate
  ceic_040_index_operation_metrics_support_bundle_gate.cpp
)
target_compile_features(
  ceic_040_index_operation_metrics_support_bundle_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_040_index_operation_metrics_support_bundle_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME ceic_040_index_operation_metrics_support_bundle_gate
  COMMAND ceic_040_index_operation_metrics_support_bundle_gate
)
set_tests_properties(ceic_040_index_operation_metrics_support_bundle_gate PROPERTIES
  LABELS "ceic_040_index_operation_metrics_support_bundle_gate;CEIC-040;CEIC-GATE-023;ELER-107;ELER-GATE-107;indexes;operation_metrics;support_bundle;redaction;runtime_producer;generation_freshness;operational_readiness;authority_boundary"
)

add_executable(
  ceic_041_index_crash_concurrency_cleanup_corruption_matrix_gate
  ceic_041_index_crash_concurrency_cleanup_corruption_matrix_gate.cpp
)
target_compile_features(
  ceic_041_index_crash_concurrency_cleanup_corruption_matrix_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_041_index_crash_concurrency_cleanup_corruption_matrix_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME ceic_041_index_crash_concurrency_cleanup_corruption_matrix_gate
  COMMAND ceic_041_index_crash_concurrency_cleanup_corruption_matrix_gate
)
set_tests_properties(ceic_041_index_crash_concurrency_cleanup_corruption_matrix_gate PROPERTIES
  LABELS "ceic_041_index_crash_concurrency_cleanup_corruption_matrix_gate;CEIC-041;CEIC-GATE-024;indexes;crash;concurrency;cleanup;corruption;backup_restore;authority_boundary"
)

set(SB_CEIC_042_INDEX_READINESS_MANIFEST_DRIFT_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/ceic_042_index_readiness_manifest_drift_gate_test.py"
)

add_custom_target(ceic_042_index_readiness_manifest_drift_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_042_INDEX_READINESS_MANIFEST_DRIFT_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
  COMMENT "Validating CEIC-042 index readiness manifest drift/freshness closure"
)

add_test(
  NAME ceic_042_index_readiness_manifest_drift_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_042_INDEX_READINESS_MANIFEST_DRIFT_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(ceic_042_index_readiness_manifest_drift_gate PROPERTIES
  LABELS "ceic_042_index_readiness_manifest_drift_gate;CEIC-042;CEIC-GATE-020;indexes;readiness_manifest;drift;freshness;route_capability;provider_evidence;metrics;crash;corruption;artifact_registration;authority_boundary"
)

set(SB_CEIC_050_LIVE_ROUTE_BENCHMARK_ARTIFACT_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/ceic_050_live_route_benchmark_artifact_gate_test.py"
)
# SEARCH_KEY: CEIC_050_LIVE_ROUTE_BENCHMARK_ARTIFACT_GATE_TEST

add_custom_target(ceic_050_live_route_benchmark_artifact_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_050_LIVE_ROUTE_BENCHMARK_ARTIFACT_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
  COMMENT "Validating CEIC-050 optimizer live-route benchmark artifact harness bridge"
)

add_test(
  NAME ceic_050_live_route_benchmark_artifact_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_050_LIVE_ROUTE_BENCHMARK_ARTIFACT_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(ceic_050_live_route_benchmark_artifact_gate PROPERTIES
  LABELS "ceic_050_live_route_benchmark_artifact_gate;CEIC-050;CEIC-GATE-030;optimizer;benchmark_clean;live_route;route_artifact;artifact_registration;authority_boundary"
)

add_executable(
  ceic_051_persisted_optimizer_benchmark_evidence_schema_gate
  ceic_051_persisted_optimizer_benchmark_evidence_schema_gate.cpp
)
target_compile_features(
  ceic_051_persisted_optimizer_benchmark_evidence_schema_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_051_persisted_optimizer_benchmark_evidence_schema_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME ceic_051_persisted_optimizer_benchmark_evidence_schema_gate
  COMMAND ceic_051_persisted_optimizer_benchmark_evidence_schema_gate
)
set_tests_properties(ceic_051_persisted_optimizer_benchmark_evidence_schema_gate PROPERTIES
  LABELS "ceic_051_persisted_optimizer_benchmark_evidence_schema_gate;CEIC-051;CEIC-GATE-030;optimizer;benchmark_schema;persisted_evidence;cold_warm;p50_p95_p99;donor_evidence_only;cluster_external_provider_only;authority_boundary"
)

add_executable(
  ceic_052_optimizer_correctness_oracle_suite_gate
  ceic_052_optimizer_correctness_oracle_suite_gate.cpp
)
target_compile_features(
  ceic_052_optimizer_correctness_oracle_suite_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_052_optimizer_correctness_oracle_suite_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME ceic_052_optimizer_correctness_oracle_suite_gate
  COMMAND ceic_052_optimizer_correctness_oracle_suite_gate
)
set_tests_properties(ceic_052_optimizer_correctness_oracle_suite_gate PROPERTIES
  LABELS "ceic_052_optimizer_correctness_oracle_suite_gate;CEIC-052;CEIC-GATE-031;optimizer;correctness_oracle;baseline_optimized_equivalence;joins;aggregation;windows;topn;dml_locator;document;vector;text;graph;mixed_fusion;donor_evidence_only;cluster_external_provider_only;authority_boundary"
)

add_executable(
  ceic_053_optimizer_crash_reopen_persistence_gate
  ceic_053_optimizer_crash_reopen_persistence_gate.cpp
)
target_compile_features(
  ceic_053_optimizer_crash_reopen_persistence_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_053_optimizer_crash_reopen_persistence_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME ceic_053_optimizer_crash_reopen_persistence_gate
  COMMAND ceic_053_optimizer_crash_reopen_persistence_gate
)
set_tests_properties(ceic_053_optimizer_crash_reopen_persistence_gate PROPERTIES
  LABELS "ceic_053_optimizer_crash_reopen_persistence_gate;CEIC-053;CEIC-GATE-030;optimizer;crash_reopen;persistence;MGA;plan_cache;statistics;feedback;benchmark_artifacts;cluster_external_provider_only;authority_boundary"
)

add_executable(
  ceic_054_selectivity_drift_plan_stability_observability_gate
  ceic_054_selectivity_drift_plan_stability_observability_gate.cpp
)
target_compile_features(
  ceic_054_selectivity_drift_plan_stability_observability_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_054_selectivity_drift_plan_stability_observability_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME ceic_054_selectivity_drift_plan_stability_observability_gate
  COMMAND ceic_054_selectivity_drift_plan_stability_observability_gate
)
set_tests_properties(ceic_054_selectivity_drift_plan_stability_observability_gate PROPERTIES
  LABELS "ceic_054_selectivity_drift_plan_stability_observability_gate;CEIC-054;CEIC-GATE-032;optimizer;observability;selectivity_drift;plan_stability;support_bundle;feedback;ann_recall;spill_prediction;cluster_external_provider_only;authority_boundary"
)

add_executable(
  ceic_055_optimizer_transformation_memo_coverage_gate
  ceic_055_optimizer_transformation_memo_coverage_gate.cpp
)
target_compile_features(
  ceic_055_optimizer_transformation_memo_coverage_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_055_optimizer_transformation_memo_coverage_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME ceic_055_optimizer_transformation_memo_coverage_gate
  COMMAND ceic_055_optimizer_transformation_memo_coverage_gate
)
set_tests_properties(ceic_055_optimizer_transformation_memo_coverage_gate PROPERTIES
  LABELS "ceic_055_optimizer_transformation_memo_coverage_gate;CEIC-055;CEIC-GATE-032;optimizer;transformation_memo;cascades_equivalent;predicate_pushdown;projection_pruning;subquery_decorrelation;cte_materialization;aggregate_pushdown;window_order_reuse;mv_rewrite;runtime_filters;sql_nosql_fusion;cluster_external_provider_only;authority_boundary"
)

add_executable(
  ceic_056_optimizer_workload_regression_budget_gate
  ceic_056_optimizer_workload_regression_budget_gate.cpp
)
target_compile_features(
  ceic_056_optimizer_workload_regression_budget_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_056_optimizer_workload_regression_budget_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME ceic_056_optimizer_workload_regression_budget_gate
  COMMAND ceic_056_optimizer_workload_regression_budget_gate
)
set_tests_properties(ceic_056_optimizer_workload_regression_budget_gate PROPERTIES
  LABELS "ceic_056_optimizer_workload_regression_budget_gate;CEIC-056;CEIC-GATE-032;optimizer;workload_regression_budget;oltp;olap;joins;dml;document;vector;text;graph;mixed_fusion;bulk;benchmark_schema;correctness_oracle;cluster_external_provider_only;authority_boundary"
)

add_executable(
  ceic_057_driver_visible_explain_compatibility_gate
  ceic_057_driver_visible_explain_compatibility_gate.cpp
)
target_compile_features(
  ceic_057_driver_visible_explain_compatibility_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_057_driver_visible_explain_compatibility_gate
  PRIVATE
    sb_engine_optimizer_contract_only
)
add_test(
  NAME ceic_057_driver_visible_explain_compatibility_gate
  COMMAND ceic_057_driver_visible_explain_compatibility_gate
)
set_tests_properties(ceic_057_driver_visible_explain_compatibility_gate PROPERTIES
  LABELS "ceic_057_driver_visible_explain_compatibility_gate;CEIC-057;CEIC-GATE-034;optimizer;driver_visible_explain;json_contract;plan_hash;route_equivalence;redaction;embedded;ipc;inet;cli;driver;benchmark_schema;cluster_external_provider_only;authority_boundary"
)

add_executable(
  ceic_058_live_donor_comparison_artifacts_gate
  ceic_058_live_donor_comparison_artifacts_gate.cpp
)
target_compile_features(
  ceic_058_live_donor_comparison_artifacts_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_058_live_donor_comparison_artifacts_gate
  PRIVATE
    sb_engine_optimizer_contract_only
)
add_test(
  NAME ceic_058_live_donor_comparison_artifacts_gate
  COMMAND ceic_058_live_donor_comparison_artifacts_gate
)
set_tests_properties(ceic_058_live_donor_comparison_artifacts_gate PROPERTIES
  LABELS "ceic_058_live_donor_comparison_artifacts_gate;CEIC-058;CEIC-GATE-035;optimizer;donor_comparison;donor_artifact;24_donors;route_equivalence;result_hash;benchmark_schema;non_authority;cluster_external_provider_only;authority_boundary"
)

add_executable(
  ceic_059_optimizer_memory_feedback_governed_ledger_gate
  ceic_059_optimizer_memory_feedback_governed_ledger_gate.cpp
)
target_compile_features(
  ceic_059_optimizer_memory_feedback_governed_ledger_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_059_optimizer_memory_feedback_governed_ledger_gate
  PRIVATE
    sb_engine_optimizer_contract_only
)
add_test(
  NAME ceic_059_optimizer_memory_feedback_governed_ledger_gate
  COMMAND ceic_059_optimizer_memory_feedback_governed_ledger_gate
)
set_tests_properties(ceic_059_optimizer_memory_feedback_governed_ledger_gate PROPERTIES
  LABELS "ceic_059_optimizer_memory_feedback_governed_ledger_gate;CEIC-059;CEIC-GATE-033;optimizer;memory_feedback;governed_reservation;support_bundle;real_operation_metrics;redaction;freshness;authority_boundary"
)

add_executable(
  ceic_060_index_readiness_plan_admission_gate
  ceic_060_index_readiness_plan_admission_gate.cpp
)
target_compile_features(
  ceic_060_index_readiness_plan_admission_gate
  PRIVATE
    cxx_std_20
)
target_link_libraries(
  ceic_060_index_readiness_plan_admission_gate
  PRIVATE
    sb_core_index
    sb_engine_optimizer_contract_only
)
add_test(
  NAME ceic_060_index_readiness_plan_admission_gate
  COMMAND ceic_060_index_readiness_plan_admission_gate
)
set_tests_properties(ceic_060_index_readiness_plan_admission_gate PROPERTIES
  LABELS "ceic_060_index_readiness_plan_admission_gate;CEIC-060;CEIC-GATE-033;optimizer;indexes;readiness_manifest;plan_admission;route_runtime_proof;metrics;crash_cleanup;storage;cluster_external_provider_only;authority_boundary"
)

set(SB_CEIC_062_OPTIMIZER_READINESS_MANIFEST_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/ceic_062_optimizer_readiness_manifest_gate_test.py"
)
# SEARCH_KEY: CEIC_062_OPTIMIZER_READINESS_MANIFEST_GATE_TEST

add_custom_target(ceic_062_optimizer_readiness_manifest_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_062_OPTIMIZER_READINESS_MANIFEST_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
  COMMENT "Validating CEIC-062 optimizer readiness manifest closure"
)

add_test(
  NAME ceic_062_optimizer_readiness_manifest_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_062_OPTIMIZER_READINESS_MANIFEST_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(ceic_062_optimizer_readiness_manifest_gate PROPERTIES
  LABELS "ceic_062_optimizer_readiness_manifest_gate;CEIC-062;CEIC-GATE-030;CEIC-GATE-033;optimizer;readiness_manifest;live_routes;correctness_oracle;crash_reopen;metrics_feedback;memory_feedback;index_readiness;driver_explain;donor_artifacts;llvm_memory;cluster_external_provider_only;authority_boundary"
)

set(SB_CEIC_085_AGENT_READINESS_MANIFEST_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/ceic_085_agent_readiness_manifest_gate_test.py"
)
# SEARCH_KEY: CEIC_085_AGENT_READINESS_MANIFEST_GATE_TEST

add_custom_target(ceic_085_agent_readiness_manifest_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_085_AGENT_READINESS_MANIFEST_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
  COMMENT "Validating CEIC-085 agent enterprise readiness manifest closure"
)

add_test(
  NAME ceic_085_agent_readiness_manifest_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_085_AGENT_READINESS_MANIFEST_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(ceic_085_agent_readiness_manifest_gate PROPERTIES
  LABELS "ceic_085_agent_readiness_manifest_gate;CEIC-085;CEIC-GATE-040;CEIC-GATE-045;agents;readiness_manifest;profiles;policies;lanes;metrics;actions;approval;evidence;plugin_provenance;memory_coupling;index_optimizer_boundary;support_bundle;cluster_external_provider_only;authority_boundary"
)

set(SB_CEIC_090_METRICS_PRODUCER_COVERAGE_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/ceic_090_metrics_producer_coverage_gate_test.py"
)
# SEARCH_KEY: CEIC_090_METRICS_PRODUCER_COVERAGE_GATE_TEST

add_custom_target(ceic_090_metrics_producer_coverage_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_090_METRICS_PRODUCER_COVERAGE_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
  COMMENT "Validating CEIC-090 metrics producer coverage closure"
)

add_test(
  NAME ceic_090_metrics_producer_coverage_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_090_METRICS_PRODUCER_COVERAGE_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(ceic_090_metrics_producer_coverage_gate PROPERTIES
  LABELS "ceic_090_metrics_producer_coverage_gate;CEIC-090;CEIC-GATE-049;metrics;producer_coverage;memory;indexes;optimizer;agents;support_bundle_inputs;production_gate_failures;authority_boundary"
)

set(SB_CEIC_091_INTEGRATED_SUPPORT_BUNDLE_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/ceic_091_integrated_support_bundle_gate_test.py"
)
# SEARCH_KEY: CEIC_091_INTEGRATED_SUPPORT_BUNDLE_GATE_TEST

add_custom_target(ceic_091_integrated_support_bundle_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_091_INTEGRATED_SUPPORT_BUNDLE_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
  COMMENT "Validating CEIC-091 integrated support-bundle closure"
)

add_test(
  NAME ceic_091_integrated_support_bundle_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_091_INTEGRATED_SUPPORT_BUNDLE_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(ceic_091_integrated_support_bundle_gate PROPERTIES
  LABELS "ceic_091_integrated_support_bundle_gate;CEIC-091;CEIC-GATE-050;support_bundle;memory;indexes;optimizer;agents;redaction;low_memory;streaming;tamper_chain;authority_boundary"
)

set(SB_CEIC_092_ROUTE_CHAIN_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/ceic_092_route_chain_gate_test.py"
)
# SEARCH_KEY: CEIC_092_ROUTE_CHAIN_GATE_TEST

add_custom_target(ceic_092_route_chain_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_092_ROUTE_CHAIN_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
  COMMENT "Validating CEIC-092 end-to-end route-chain closure"
)

add_test(
  NAME ceic_092_route_chain_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_092_ROUTE_CHAIN_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(ceic_092_route_chain_gate PROPERTIES
  LABELS "ceic_092_route_chain_gate;CEIC-092;CEIC-GATE-053;route_chain;memory;optimizer;indexes;agents;mga;security;result_equivalence;cluster_external_provider_only;authority_boundary"
)

set(SB_CEIC_093_RELIABILITY_SECURITY_SUITE_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/ceic_093_reliability_security_suite_gate_test.py"
)
# SEARCH_KEY: CEIC_093_RELIABILITY_SECURITY_SUITE_GATE_TEST

add_custom_target(ceic_093_reliability_security_suite_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_093_RELIABILITY_SECURITY_SUITE_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
  COMMENT "Validating CEIC-093 long-soak reliability and security-negative closure"
)

add_test(
  NAME ceic_093_reliability_security_suite_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_093_RELIABILITY_SECURITY_SUITE_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(ceic_093_reliability_security_suite_gate PROPERTIES
  LABELS "ceic_093_reliability_security_suite_gate;CEIC-093;CEIC-GATE-051;ELER-102;ELER-GATE-102;soak_24h;soak_72h;soak_7d;high_concurrency;crash_fault;sanitizer_static;memory_pressure;security_negative;artifact_retention;cluster_external_provider_only;authority_boundary;linux_proof"
)

set(SB_CEIC_094_CROSS_PLATFORM_RELEASE_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/ceic_094_cross_platform_release_gate_test.py"
)
# SEARCH_KEY: CEIC_094_CROSS_PLATFORM_RELEASE_GATE_TEST

add_custom_target(ceic_094_cross_platform_release_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_094_CROSS_PLATFORM_RELEASE_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
  COMMENT "Validating CEIC-094 cross-platform release proof closure"
)

add_test(
  NAME ceic_094_cross_platform_release_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_094_CROSS_PLATFORM_RELEASE_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(ceic_094_cross_platform_release_gate PROPERTIES
  LABELS "ceic_094_cross_platform_release_gate;CEIC-094;CEIC-GATE-052;cross_platform;linux;windows;macos;bsd;production_build;entropy;temp_spill;plugin_signing;sbom;llvm;cluster_external_provider_only;authority_boundary"
)

set(SB_CEIC_095_FINAL_ENTERPRISE_MANIFEST_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/ceic_095_final_enterprise_manifest_gate_test.py"
)
# SEARCH_KEY: CEIC_095_FINAL_ENTERPRISE_MANIFEST_GATE_TEST

add_custom_target(ceic_095_final_enterprise_manifest_gate_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_095_FINAL_ENTERPRISE_MANIFEST_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
  COMMENT "Validating CEIC-095 final enterprise proof manifest and movement gate"
)

add_test(
  NAME ceic_095_final_enterprise_manifest_gate
  COMMAND "${Python3_EXECUTABLE}" "${SB_CEIC_095_FINAL_ENTERPRISE_MANIFEST_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(ceic_095_final_enterprise_manifest_gate PROPERTIES
  LABELS "ceic_095_final_enterprise_manifest_gate;CEIC-095;CEIC-GATE-900;final_manifest;movement_gate;all_gates_complete;residual_risk;static_claim_refusal;cluster_external_provider_only;authority_boundary"
)
