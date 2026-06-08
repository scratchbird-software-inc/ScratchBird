# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if(NOT TARGET sb_core_platform)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/platform
                   ${CMAKE_CURRENT_BINARY_DIR}/src_core_platform)
endif()
if(NOT TARGET sb_core_metrics)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/metrics
                   ${CMAKE_CURRENT_BINARY_DIR}/src_core_metrics)
endif()
if(NOT TARGET sb_core_memory)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/memory
                   ${CMAKE_CURRENT_BINARY_DIR}/src_core_memory)
endif()
if(NOT TARGET sb_core_uuid)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/uuid
                   ${CMAKE_CURRENT_BINARY_DIR}/src_core_uuid)
endif()
if(NOT TARGET sb_core_datatypes)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/datatypes
                   ${CMAKE_CURRENT_BINARY_DIR}/src_core_datatypes)
endif()
if(NOT TARGET sb_core_resources)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/resources
                   ${CMAKE_CURRENT_BINARY_DIR}/src_core_resources)
endif()
if(NOT TARGET sb_storage_disk)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/storage/disk
                   ${CMAKE_CURRENT_BINARY_DIR}/src_storage_disk)
endif()
if(NOT TARGET sb_transaction_mga)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/transaction/mga
                   ${CMAKE_CURRENT_BINARY_DIR}/src_transaction_mga)
endif()
if(NOT TARGET sb_storage_page)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/storage/page
                   ${CMAKE_CURRENT_BINARY_DIR}/src_storage_page)
endif()
if(NOT TARGET sb_core_catalog)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/catalog
                   ${CMAKE_CURRENT_BINARY_DIR}/src_core_catalog)
endif()
if(NOT TARGET sb_core_index)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/index
                   ${CMAKE_CURRENT_BINARY_DIR}/src_core_index)
endif()
if(NOT TARGET sb_core_agents)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/agents
                   ${CMAKE_CURRENT_BINARY_DIR}/src_core_agents)
endif()
if(NOT TARGET sb_storage_database)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/storage/database
                   ${CMAKE_CURRENT_BINARY_DIR}/src_storage_database)
endif()
if(NOT TARGET sb_storage_filespace)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/storage/filespace
                   ${CMAKE_CURRENT_BINARY_DIR}/src_storage_filespace)
endif()
if(NOT TARGET sb_engine_internal_api)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/engine/internal_api
                   ${CMAKE_CURRENT_BINARY_DIR}/src_engine_internal_api)
endif()
if(NOT TARGET sb_engine_sblr)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/engine/sblr
                   ${CMAKE_CURRENT_BINARY_DIR}/src_engine_sblr)
endif()
if(NOT TARGET sb_core_agents_cluster_boundary)
  add_library(sb_core_agents_cluster_boundary
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/agents/agent_cluster_boundary.cpp
  )
  target_compile_features(sb_core_agents_cluster_boundary PUBLIC cxx_std_17)
  target_include_directories(sb_core_agents_cluster_boundary PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/agents
  )
  target_link_libraries(sb_core_agents_cluster_boundary PUBLIC
    sb_core_agents
  )
  target_link_libraries(sb_core_agents_cluster_boundary PRIVATE
    sb_cluster_provider
  )
endif()

option(SB_PUBLIC_RELEASE_EXPORT_NESTED
  "Suppress recursive public export CTest registration while proving a staged public tree"
  OFF)
option(SB_PUBLIC_RELEASE_HARDENING_NESTED
  "Suppress recursive public hardening CTest registration while proving sanitizer/static-analysis profiles"
  OFF)

set(_sb_release_nested_configure_env)
foreach(_sb_forwarded_var
        SB_LLVM_PROJECT_ROOT
        SB_LLVM_TOOLS_ROOT
        SB_LLVM_LIBRARY
        SB_LLVM_LINK_MODE)
  if(DEFINED ${_sb_forwarded_var} AND NOT "${${_sb_forwarded_var}}" STREQUAL "")
    list(APPEND _sb_release_nested_configure_env
         "${_sb_forwarded_var}=${${_sb_forwarded_var}}")
  endif()
endforeach()

add_library(public_page_type_truth_runtime
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/storage/page/page_layout.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/storage/page/page_registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/storage/page/page_skeleton.cpp
)
target_compile_features(public_page_type_truth_runtime PUBLIC cxx_std_23)
target_include_directories(public_page_type_truth_runtime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/storage/page
)
target_link_libraries(public_page_type_truth_runtime PUBLIC
  sb_storage_disk
)

add_library(public_row_data_page_runtime
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/storage/page/row_data_page.cpp
)
target_compile_features(public_row_data_page_runtime PUBLIC cxx_std_23)
target_include_directories(public_row_data_page_runtime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/storage/page
)
target_link_libraries(public_row_data_page_runtime PUBLIC
  sb_core_datatypes
  sb_core_uuid
  sb_storage_disk
)

add_executable(public_memory_reallocate_gate
  public_memory_reallocate_gate.cpp
)
target_compile_features(public_memory_reallocate_gate PRIVATE cxx_std_23)
target_compile_definitions(public_memory_reallocate_gate PRIVATE
  SCRATCHBIRD_MEMORY_FAILURE_INJECTION_TEST_GUARD=1
)
target_link_libraries(public_memory_reallocate_gate PRIVATE
  sb_core_memory
)
add_test(
  NAME public_memory_reallocate_gate
  COMMAND public_memory_reallocate_gate
)
set_tests_properties(public_memory_reallocate_gate PROPERTIES
  LABELS "public_release_correctness;public_memory_reallocate_gate;PCR-GATE-010;PCR-010;memory;allocator;fault_injection"
)

add_executable(public_default_memory_manager_gate
  public_default_memory_manager_gate.cpp
)
target_compile_features(public_default_memory_manager_gate PRIVATE cxx_std_23)
target_link_libraries(public_default_memory_manager_gate PRIVATE
  sb_core_memory
)
foreach(_default_memory_manager_mode IN ITEMS unconfigured production fixture)
  add_test(
    NAME public_default_memory_manager_${_default_memory_manager_mode}_gate
    COMMAND public_default_memory_manager_gate ${_default_memory_manager_mode}
  )
  set_tests_properties(public_default_memory_manager_${_default_memory_manager_mode}_gate PROPERTIES
    LABELS "public_release_correctness;public_default_memory_manager_gate;PCR-GATE-011;PCR-011;memory;default_manager;production_fixture_separation"
  )
endforeach()
unset(_default_memory_manager_mode)

add_executable(public_query_memory_reservation_gate
  public_query_memory_reservation_gate.cpp
)
target_compile_features(public_query_memory_reservation_gate PRIVATE cxx_std_23)
target_link_libraries(public_query_memory_reservation_gate PRIVATE
  sb_core_memory
)
add_test(
  NAME public_query_memory_reservation_gate
  COMMAND public_query_memory_reservation_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_query_memory_reservation_gate_tmp
)
set_tests_properties(public_query_memory_reservation_gate PROPERTIES
  LABELS "public_release_correctness;public_query_memory_reservation_gate;PCR-GATE-012;PCR-012;memory;query_memory;hierarchical_reservation;spill;cancel"
)

add_executable(public_memory_pressure_executor_gate
  public_memory_pressure_executor_gate.cpp
)
target_compile_features(public_memory_pressure_executor_gate PRIVATE cxx_std_23)
target_link_libraries(public_memory_pressure_executor_gate PRIVATE
  sb_core_memory
  sb_core_agents
  sb_storage_page
  sb_core_uuid
)
add_test(
  NAME public_memory_pressure_executor_gate
  COMMAND public_memory_pressure_executor_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_memory_pressure_executor_gate_tmp
)
set_tests_properties(public_memory_pressure_executor_gate PROPERTIES
  LABELS "public_release_correctness;public_memory_pressure_executor_gate;PCR-GATE-013;PCR-013;memory;pressure;executor_consumption;fail_closed"
)

add_executable(public_query_bump_region_arena_gate
  public_query_bump_region_arena_gate.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/engine/executor/operator_typed_arena_work_area.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/engine/optimizer/optimizer_typed_arena_work_area.cpp
)
target_compile_features(public_query_bump_region_arena_gate PRIVATE cxx_std_23)
target_include_directories(public_query_bump_region_arena_gate PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/engine/executor
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/engine/optimizer
)
target_link_libraries(public_query_bump_region_arena_gate PRIVATE
  sb_core_memory
)
add_test(
  NAME public_query_bump_region_arena_gate
  COMMAND public_query_bump_region_arena_gate
)
set_tests_properties(public_query_bump_region_arena_gate PROPERTIES
  LABELS "public_release_correctness;public_query_bump_region_arena_gate;PCR-GATE-014;PCR-014;memory;bump_region;query_memory;optimizer;executor;arena"
)

add_executable(public_allocator_sharded_overhead_gate
  public_allocator_sharded_overhead_gate.cpp
)
target_compile_features(public_allocator_sharded_overhead_gate PRIVATE cxx_std_23)
target_compile_definitions(public_allocator_sharded_overhead_gate PRIVATE
  SCRATCHBIRD_MEMORY_FAILURE_INJECTION_TEST_GUARD=1
)
target_link_libraries(public_allocator_sharded_overhead_gate PRIVATE
  sb_core_memory
)
add_test(
  NAME public_allocator_sharded_overhead_gate
  COMMAND public_allocator_sharded_overhead_gate
)
set_tests_properties(public_allocator_sharded_overhead_gate PROPERTIES
  LABELS "public_release_correctness;public_allocator_sharded_overhead_gate;PCR-GATE-015;PCR-015;memory;allocator;sharded_accounting;overhead;fragmentation;active_map"
)

add_executable(public_memory_pmr_call_path_gate
  public_memory_pmr_call_path_gate.cpp
)
target_compile_features(public_memory_pmr_call_path_gate PRIVATE cxx_std_23)
target_link_libraries(public_memory_pmr_call_path_gate PRIVATE
  sb_core_memory
)
add_test(
  NAME public_memory_pmr_call_path_gate
  COMMAND public_memory_pmr_call_path_gate
)
set_tests_properties(public_memory_pmr_call_path_gate PROPERTIES
  LABELS "public_release_correctness;public_memory_pmr_call_path_gate;PCR-GATE-016;PCR-016;memory;pmr;call_path;reservation_backed;planner;executor"
)

add_test(
  NAME public_memory_call_path_matrix_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_memory_call_path_matrix.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_memory_call_path_matrix_gate/public_memory_call_path_matrix.csv
)
set_tests_properties(public_memory_call_path_matrix_gate PROPERTIES
  LABELS "public_release_correctness;public_memory_call_path_matrix_gate;PCR-GATE-016;PCR-016;memory;pmr;call_path_matrix;reservation_backed;approved_exemption"
)

add_executable(public_disk_file_device_gate
  public_disk_file_device_gate.cpp
)
target_compile_features(public_disk_file_device_gate PRIVATE cxx_std_23)
target_link_libraries(public_disk_file_device_gate PRIVATE
  sb_storage_disk
)
add_test(
  NAME public_disk_file_device_gate
  COMMAND public_disk_file_device_gate
)
set_tests_properties(public_disk_file_device_gate PROPERTIES
  LABELS "public_release_correctness;public_disk_file_device_gate;PCR-GATE-020;PCR-GATE-021;PCR-020;PCR-021;storage;disk;durable_sync;atomic_create"
)

add_executable(public_database_ownership_gate
  public_database_ownership_gate.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/server/database_ownership.cpp
)
target_compile_features(public_database_ownership_gate PRIVATE cxx_std_23)
target_include_directories(public_database_ownership_gate PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/server
)
target_link_libraries(public_database_ownership_gate PRIVATE
  sb_storage_disk
)
add_test(
  NAME public_database_ownership_gate
  COMMAND public_database_ownership_gate
)
set_tests_properties(public_database_ownership_gate PROPERTIES
  LABELS "public_release_correctness;public_database_ownership_gate;PCR-GATE-022;PCR-022;storage;server;database_ownership;fail_closed"
)

add_executable(public_page_size_profile_gate
  public_page_size_profile_gate.cpp
)
target_compile_features(public_page_size_profile_gate PRIVATE cxx_std_23)
target_link_libraries(public_page_size_profile_gate PRIVATE
  sb_storage_disk
)
add_test(
  NAME public_page_size_profile_gate
  COMMAND public_page_size_profile_gate
)
set_tests_properties(public_page_size_profile_gate PROPERTIES
  LABELS "public_release_correctness;public_page_size_profile_gate;PCR-GATE-023;PCR-023;storage;page_size;database_format"
)

add_executable(public_filespace_header_capacity_gate
  public_filespace_header_capacity_gate.cpp
)
target_compile_features(public_filespace_header_capacity_gate PRIVATE cxx_std_23)
target_link_libraries(public_filespace_header_capacity_gate PRIVATE
  sb_storage_filespace
)
add_test(
  NAME public_filespace_header_capacity_gate
  COMMAND public_filespace_header_capacity_gate
)
set_tests_properties(public_filespace_header_capacity_gate PROPERTIES
  LABELS "public_release_correctness;public_filespace_header_capacity_gate;PCR-GATE-024;PCR-024;storage;filespace;header;capacity;durable_metadata"
)

add_executable(public_disk_resource_bundle_tool
  ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_disk_resource_bundle.cpp
)
target_compile_features(public_disk_resource_bundle_tool PRIVATE cxx_std_23)
target_link_libraries(public_disk_resource_bundle_tool PRIVATE
  sb_storage_database
  sb_storage_filespace
)
add_test(
  NAME public_disk_resource_bundle_gate
  COMMAND public_disk_resource_bundle_tool
          --self-test
          --work-dir ${CMAKE_CURRENT_BINARY_DIR}/public_disk_resource_bundle_gate_tmp
          --out ${CMAKE_CURRENT_BINARY_DIR}/public_disk_resource_bundle_gate_tmp/disk-resource.bundle
)
set_tests_properties(public_disk_resource_bundle_gate PROPERTIES
  LABELS "public_release_correctness;public_disk_resource_bundle_gate;PCR-GATE-025;PCR-025;storage;disk;filespace;support_bundle;offline_verify;release_hygiene"
)

add_executable(public_page_type_truth_table_tool
  ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_page_type_truth_table.cpp
)
target_compile_features(public_page_type_truth_table_tool PRIVATE cxx_std_23)
target_link_libraries(public_page_type_truth_table_tool PRIVATE
  public_page_type_truth_runtime
)
add_test(
  NAME public_page_type_truth_table_gate
  COMMAND public_page_type_truth_table_tool
          --verify
          --out ${CMAKE_CURRENT_BINARY_DIR}/public_page_type_truth_table.csv
)
set_tests_properties(public_page_type_truth_table_gate PROPERTIES
  LABELS "public_release_correctness;public_page_type_truth_table_gate;PCR-GATE-030;PCR-030;storage;page_type_truth;cluster_boundary"
)

add_executable(public_page_body_production_refusal_gate
  public_page_body_production_refusal_gate.cpp
)
target_compile_features(public_page_body_production_refusal_gate PRIVATE cxx_std_23)
target_link_libraries(public_page_body_production_refusal_gate PRIVATE
  public_page_type_truth_runtime
)
add_test(
  NAME public_page_body_production_refusal_gate
  COMMAND public_page_body_production_refusal_gate
)
set_tests_properties(public_page_body_production_refusal_gate PROPERTIES
  LABELS "public_release_correctness;public_page_body_production_refusal_gate;PCR-GATE-031;PCR-031;storage;page_body;production_refusal;cluster_boundary"
)

add_executable(public_toast_overflow_binary_descriptor_gate
  public_toast_overflow_binary_descriptor_gate.cpp
)
target_compile_features(public_toast_overflow_binary_descriptor_gate PRIVATE cxx_std_23)
target_link_libraries(public_toast_overflow_binary_descriptor_gate PRIVATE
  sb_storage_page
)
add_test(
  NAME public_toast_overflow_binary_descriptor_gate
  COMMAND public_toast_overflow_binary_descriptor_gate
)
set_tests_properties(public_toast_overflow_binary_descriptor_gate PROPERTIES
  LABELS "public_release_correctness;public_toast_overflow_binary_descriptor_gate;PCR-GATE-032;PCR-032;ELER-104;ELER-GATE-104;ELER-106;ELER-GATE-106;storage;page_body;toast;overflow;binary_descriptor;mga;toast_format_compatibility;toast_reachability_property"
)

add_executable(public_row_data_slotted_layout_gate
  public_row_data_slotted_layout_gate.cpp
)
target_compile_features(public_row_data_slotted_layout_gate PRIVATE cxx_std_23)
target_link_libraries(public_row_data_slotted_layout_gate PRIVATE
  public_row_data_page_runtime
)
add_test(
  NAME public_row_data_slotted_layout_gate
  COMMAND public_row_data_slotted_layout_gate
)
set_tests_properties(public_row_data_slotted_layout_gate PROPERTIES
  LABELS "public_release_correctness;public_row_data_slotted_layout_gate;PCR-GATE-033;PCR-033;storage;row_data;slotted_page;mga"
)

add_executable(public_page_body_checksum_agreement_gate
  public_page_body_checksum_agreement_gate.cpp
)
target_compile_features(public_page_body_checksum_agreement_gate PRIVATE cxx_std_23)
target_link_libraries(public_page_body_checksum_agreement_gate PRIVATE
  sb_storage_page
)
add_test(
  NAME public_page_body_checksum_agreement_gate
  COMMAND public_page_body_checksum_agreement_gate
)
set_tests_properties(public_page_body_checksum_agreement_gate PROPERTIES
  LABELS "public_release_correctness;public_page_body_checksum_agreement_gate;PCR-GATE-034;PCR-034;ELER-103;ELER-GATE-103;ELER-104;ELER-GATE-104;storage;page_body;checksum;body_family;index;malformed_database_open;page_catalog_property"
)

add_executable(public_transaction_mga_cow_gate
  public_transaction_mga_cow_gate.cpp
)
target_compile_features(public_transaction_mga_cow_gate PRIVATE cxx_std_23)
target_link_libraries(public_transaction_mga_cow_gate PRIVATE
  public_row_data_page_runtime
  sb_transaction_mga
)
add_test(
  NAME public_transaction_mga_cow_gate
  COMMAND public_transaction_mga_cow_gate
)
set_tests_properties(public_transaction_mga_cow_gate PROPERTIES
  LABELS "public_release_correctness;public_transaction_mga_cow_gate;PCR-GATE-070;PCR-070;ELER-104;ELER-GATE-104;transaction;mga;copy_on_write;row_data_page;mga_visibility_property"
)

add_executable(public_transaction_inventory_lock_table_gate
  public_transaction_inventory_lock_table_gate.cpp
)
target_compile_features(public_transaction_inventory_lock_table_gate PRIVATE cxx_std_23)
target_link_libraries(public_transaction_inventory_lock_table_gate PRIVATE
  sb_storage_database
  sb_transaction_mga
  sb_core_memory
  sb_core_uuid
)
add_test(
  NAME public_transaction_inventory_lock_table_gate
  COMMAND public_transaction_inventory_lock_table_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_transaction_inventory_lock_table_gate_tmp
)
set_tests_properties(public_transaction_inventory_lock_table_gate PROPERTIES
  LABELS "public_release_correctness;public_transaction_inventory_lock_table_gate;PCR-GATE-071A;PCR-071;ELER-104;ELER-GATE-104;ELER-106;ELER-GATE-106;transaction;mga;inventory;lock_table;durable_metadata;transaction_inventory_format_policy;transaction_inventory_property"
)

add_executable(public_transaction_savepoint_limbo_cleanup_gate
  public_transaction_savepoint_limbo_cleanup_gate.cpp
)
target_compile_features(public_transaction_savepoint_limbo_cleanup_gate PRIVATE cxx_std_23)
target_link_libraries(public_transaction_savepoint_limbo_cleanup_gate PRIVATE
  sb_engine_internal_api
  sb_storage_database
  sb_transaction_mga
  sb_core_memory
  sb_core_uuid
)
add_test(
  NAME public_transaction_savepoint_limbo_cleanup_gate
  COMMAND public_transaction_savepoint_limbo_cleanup_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_transaction_savepoint_limbo_cleanup_gate_tmp
)
set_tests_properties(public_transaction_savepoint_limbo_cleanup_gate PROPERTIES
  LABELS "public_release_correctness;public_transaction_savepoint_limbo_cleanup_gate;PCR-GATE-072A;PCR-072;transaction;mga;savepoint;limbo;cleanup;durable_metadata"
)

add_executable(public_transaction_support_bundle_gate
  public_transaction_support_bundle_gate.cpp
)
target_compile_features(public_transaction_support_bundle_gate PRIVATE cxx_std_23)
target_link_libraries(public_transaction_support_bundle_gate PRIVATE
  sb_engine_internal_api
  sb_transaction_mga
  sb_core_uuid
)
add_test(
  NAME public_transaction_support_bundle_gate
  COMMAND public_transaction_support_bundle_gate
)
set_tests_properties(public_transaction_support_bundle_gate PROPERTIES
  LABELS "public_release_correctness;public_transaction_support_bundle_gate;PCR-GATE-073A;PCR-073;ELER-103;ELER-GATE-103;transaction;mga;support_bundle;observability;non_authority;support_bundle_leakage"
)

add_executable(public_mga_audit_transaction_location_gate
  public_mga_audit_transaction_location_gate.cpp
)
target_compile_features(public_mga_audit_transaction_location_gate PRIVATE cxx_std_23)
target_link_libraries(public_mga_audit_transaction_location_gate PRIVATE
  sb_engine_internal_api
  sb_storage_database
  sb_transaction_mga
  sb_core_memory
  sb_core_uuid
)
add_test(
  NAME public_mga_audit_transaction_location_gate
  COMMAND public_mga_audit_transaction_location_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_mga_audit_transaction_location_gate_tmp
)
set_tests_properties(public_mga_audit_transaction_location_gate PROPERTIES
  LABELS "public_release_correctness;public_mga_audit_transaction_location_gate;PCR-GATE-008A;PCR-078;transaction;mga;audit_as_of;location;cluster_fail_closed"
)

add_executable(public_mga_historical_snapshot_locator_gate
  public_mga_historical_snapshot_locator_gate.cpp
)
target_compile_features(public_mga_historical_snapshot_locator_gate PRIVATE cxx_std_23)
target_link_libraries(public_mga_historical_snapshot_locator_gate PRIVATE
  sb_storage_database
  sb_transaction_mga
  sb_core_memory
  sb_core_uuid
)
add_test(
  NAME public_mga_historical_snapshot_locator_gate
  COMMAND public_mga_historical_snapshot_locator_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_mga_historical_snapshot_locator_gate_tmp
)
set_tests_properties(public_mga_historical_snapshot_locator_gate PROPERTIES
  LABELS "public_release_correctness;public_mga_historical_snapshot_locator_gate;PCR-GATE-008;PCR-079;transaction;mga;historical_snapshot;retired;cluster_fail_closed"
)

add_executable(public_repair_event_ledger_quarantine_gate
  public_repair_event_ledger_quarantine_gate.cpp
)
target_compile_features(public_repair_event_ledger_quarantine_gate PRIVATE cxx_std_23)
target_link_libraries(public_repair_event_ledger_quarantine_gate PRIVATE
  sb_storage_database
  sb_storage_page
  sb_core_uuid
)
add_test(
  NAME public_repair_event_ledger_quarantine_gate
  COMMAND public_repair_event_ledger_quarantine_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_repair_event_ledger_quarantine_gate_tmp
)
set_tests_properties(public_repair_event_ledger_quarantine_gate PROPERTIES
  LABELS "public_release_correctness;public_repair_event_ledger_quarantine_gate;PCR-GATE-007A;PCR-074;repair;storage;page_quarantine;mga;non_authority"
)

add_executable(public_repair_history_inspection_api_gate
  public_repair_history_inspection_api_gate.cpp
)
target_compile_features(public_repair_history_inspection_api_gate PRIVATE cxx_std_23)
target_link_libraries(public_repair_history_inspection_api_gate PRIVATE
  sb_engine_internal_api
  sb_storage_database
  sb_transaction_mga
  sb_core_uuid
)
add_test(
  NAME public_repair_history_inspection_api_gate
  COMMAND public_repair_history_inspection_api_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_repair_history_inspection_api_gate_tmp
)
set_tests_properties(public_repair_history_inspection_api_gate PROPERTIES
  LABELS "public_release_correctness;public_repair_history_inspection_api_gate;PCR-GATE-007B;PCR-075;repair;inspection_api;mga;non_authority"
)

add_executable(public_repair_identity_rules_gate
  public_repair_identity_rules_gate.cpp
)
target_compile_features(public_repair_identity_rules_gate PRIVATE cxx_std_23)
target_link_libraries(public_repair_identity_rules_gate PRIVATE
  sb_storage_page
  sb_transaction_mga
  sb_core_uuid
)
add_test(
  NAME public_repair_identity_rules_gate
  COMMAND public_repair_identity_rules_gate
)
set_tests_properties(public_repair_identity_rules_gate PROPERTIES
  LABELS "public_release_correctness;public_repair_identity_rules_gate;PCR-GATE-007C;PCR-076;repair;identity;mga;salvage;non_authority"
)

add_executable(public_repair_tamper_retention_crash_resume_gate
  public_repair_tamper_retention_crash_resume_gate.cpp
)
target_compile_features(public_repair_tamper_retention_crash_resume_gate PRIVATE cxx_std_23)
target_link_libraries(public_repair_tamper_retention_crash_resume_gate PRIVATE
  sb_storage_database
  sb_storage_page
  sb_core_uuid
)
add_test(
  NAME public_repair_tamper_retention_crash_resume_gate
  COMMAND public_repair_tamper_retention_crash_resume_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_repair_tamper_retention_crash_resume_gate_tmp
)
set_tests_properties(public_repair_tamper_retention_crash_resume_gate PROPERTIES
  LABELS "public_release_correctness;public_repair_tamper_retention_crash_resume_gate;PCR-GATE-007D;PCR-077;repair;tamper_evidence;retention;legal_hold;crash_resume;mga;non_authority"
)

add_executable(public_mga_physical_sweep_gate
  public_mga_physical_sweep_gate.cpp
)
target_compile_features(public_mga_physical_sweep_gate PRIVATE cxx_std_23)
target_link_libraries(public_mga_physical_sweep_gate PRIVATE
  sb_engine_internal_api
  sb_storage_page
  sb_transaction_mga
  sb_core_uuid
)
add_test(
  NAME public_mga_physical_sweep_gate
  COMMAND public_mga_physical_sweep_gate
)
set_tests_properties(public_mga_physical_sweep_gate PROPERTIES
  LABELS "public_release_correctness;public_mga_physical_sweep_gate;PCR-GATE-071;PCR-083;transaction;mga;physical_sweep;free_space;index_cleanup"
)

add_executable(public_archive_before_reclaim_gate
  public_archive_before_reclaim_gate.cpp
)
target_compile_features(public_archive_before_reclaim_gate PRIVATE cxx_std_23)
target_link_libraries(public_archive_before_reclaim_gate PRIVATE
  sb_engine_internal_api
  sb_storage_filespace
  sb_transaction_mga
  sb_core_agents
  sb_core_uuid
)
add_test(
  NAME public_archive_before_reclaim_gate
  COMMAND public_archive_before_reclaim_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_archive_before_reclaim_gate_tmp
)
set_tests_properties(public_archive_before_reclaim_gate PROPERTIES
  LABELS "public_release_correctness;public_archive_before_reclaim_gate;PCR-GATE-072;PCR-084;transaction;mga;archive;filespace;archive_before_reclaim;retention;cluster_boundary"
)

add_executable(public_backup_forward_session_gate
  public_backup_forward_session_gate.cpp
)
target_compile_features(public_backup_forward_session_gate PRIVATE cxx_std_23)
target_link_libraries(public_backup_forward_session_gate PRIVATE
  sb_engine_internal_api
  sb_storage_database
  sb_transaction_mga
  sb_core_memory
  sb_core_uuid
)
add_test(
  NAME public_backup_forward_session_gate
  COMMAND public_backup_forward_session_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_backup_forward_session_gate_tmp
)
set_tests_properties(public_backup_forward_session_gate PROPERTIES
  LABELS "public_release_correctness;public_backup_forward_session_gate;PCR-GATE-073;PCR-085;backup_forward;write_after;transaction;mga;archive;restore;cluster_boundary;non_authority"
)

add_executable(public_multi_horizon_disposal_gate
  public_multi_horizon_disposal_gate.cpp
)
target_compile_features(public_multi_horizon_disposal_gate PRIVATE cxx_std_23)
target_link_libraries(public_multi_horizon_disposal_gate PRIVATE
  sb_engine_internal_api
  sb_core_agents
  sb_core_uuid
)
add_test(
  NAME public_multi_horizon_disposal_gate
  COMMAND public_multi_horizon_disposal_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_multi_horizon_disposal_gate_tmp
)
set_tests_properties(public_multi_horizon_disposal_gate PROPERTIES
  LABELS "public_release_correctness;public_multi_horizon_disposal_gate;PCR-GATE-074;PCR-086;transaction;mga;archive;backup_forward;write_after;multi_horizon;fail_closed;cluster_boundary;non_authority"
)

add_executable(public_backup_update_coverage_gate
  public_backup_update_coverage_gate.cpp
)
target_compile_features(public_backup_update_coverage_gate PRIVATE cxx_std_23)
target_link_libraries(public_backup_update_coverage_gate PRIVATE
  sb_engine_internal_api
  sb_storage_database
  sb_core_memory
  sb_core_uuid
)
add_test(
  NAME public_backup_update_coverage_gate
  COMMAND public_backup_update_coverage_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_backup_update_coverage_gate_tmp
)
set_tests_properties(public_backup_update_coverage_gate PROPERTIES
  LABELS "public_release_correctness;public_backup_update_coverage_gate;PCR-GATE-075;PCR-087;backup;archive;restore;write_after;coverage_reuse;idempotency;pitr;mga;cluster_boundary;non_authority"
)

add_executable(public_authorization_durable_flow_gate
  public_authorization_durable_flow_gate.cpp
)
target_compile_features(public_authorization_durable_flow_gate PRIVATE cxx_std_23)
target_link_libraries(public_authorization_durable_flow_gate PRIVATE
  sb_engine_internal_api
  sb_core_uuid
)
add_test(
  NAME public_authorization_durable_flow_gate
  COMMAND public_authorization_durable_flow_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_authorization_durable_flow_gate_tmp
)
set_tests_properties(public_authorization_durable_flow_gate PROPERTIES
  LABELS "public_release_correctness;public_authorization_durable_flow_gate;PCR-GATE-090;PCR-090;ELER-103;ELER-GATE-103;ELER-104;ELER-GATE-104;security;authorization;durable_principal;role;group;nested_group;grant;deny;row_policy;domain_policy;epoch;fail_closed;auth_bypass;privilege_escalation;authorization_property"
)

add_executable(public_security_durable_crypto_hardening_gate
  public_security_durable_crypto_hardening_gate.cpp
)
target_compile_features(public_security_durable_crypto_hardening_gate PRIVATE cxx_std_23)
target_link_libraries(public_security_durable_crypto_hardening_gate PRIVATE
  sb_engine_internal_api
  sb_core_uuid
)
add_test(
  NAME public_security_durable_crypto_hardening_gate
  COMMAND public_security_durable_crypto_hardening_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_security_durable_crypto_hardening_gate_tmp
)
set_tests_properties(public_security_durable_crypto_hardening_gate PROPERTIES
  LABELS "public_release_correctness;public_security_durable_crypto_hardening_gate;PCR-GATE-091;PCR-091;ELER-103;ELER-GATE-103;security;authentication;durable_state;crypto;constant_time;local_password;temporary_token;no_hardcoded_admin;no_sidecar;auth_bypass"
)

add_executable(public_security_provider_contract_protected_material_gate
  public_security_provider_contract_protected_material_gate.cpp
)
target_compile_features(public_security_provider_contract_protected_material_gate PRIVATE cxx_std_23)
target_link_libraries(public_security_provider_contract_protected_material_gate PRIVATE
  sb_engine_internal_api
  sb_core_uuid
)
add_test(
  NAME public_security_provider_contract_protected_material_gate
  COMMAND public_security_provider_contract_protected_material_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_security_provider_contract_protected_material_gate_tmp
)
set_tests_properties(public_security_provider_contract_protected_material_gate PROPERTIES
  LABELS "public_release_correctness;public_security_provider_contract_protected_material_gate;PCR-GATE-092;PCR-092;ELER-103;ELER-GATE-103;security;auth_provider;external_provider;row_policy;protected_material;crypto;key_rotation;stale_rejection;group_materialization;cluster_boundary;privilege_escalation;catalog_corruption"
)

add_executable(public_datatype_truth_matrix
  ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_datatype_truth_matrix.cpp
)
target_compile_features(public_datatype_truth_matrix PRIVATE cxx_std_23)
target_link_libraries(public_datatype_truth_matrix PRIVATE
  sb_engine_internal_api
)
add_test(
  NAME public_datatype_truth_matrix_gate
  COMMAND public_datatype_truth_matrix
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_datatype_truth_matrix_gate/public_datatype_truth_matrix.csv
)
set_tests_properties(public_datatype_truth_matrix_gate PROPERTIES
  LABELS "public_release_correctness;public_datatype_truth_matrix_gate;PCR-GATE-040;PCR-040;datatypes;truth_matrix;release_hygiene"
)

add_executable(public_domain_psql_exact_numeric_gate
  public_domain_psql_exact_numeric_gate.cpp
)
target_compile_features(public_domain_psql_exact_numeric_gate PRIVATE cxx_std_23)
target_link_libraries(public_domain_psql_exact_numeric_gate PRIVATE
  sb_engine_sblr
  sb_engine_internal_api
  sb_storage_database
  sb_core_memory
  sb_core_uuid
)
add_test(
  NAME public_domain_psql_exact_numeric_gate
  COMMAND public_domain_psql_exact_numeric_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_domain_psql_exact_numeric_gate_tmp
)
set_tests_properties(public_domain_psql_exact_numeric_gate PROPERTIES
  LABELS "public_release_correctness;public_domain_psql_exact_numeric_gate;PCR-GATE-041;PCR-041;datatypes;domain;sblr;exact_numeric;sbl_numeric;mga;fail_closed"
)

add_executable(public_datatype_binary_descriptor_integrity_gate
  public_datatype_binary_descriptor_integrity_gate.cpp
)
target_compile_features(public_datatype_binary_descriptor_integrity_gate PRIVATE cxx_std_23)
target_link_libraries(public_datatype_binary_descriptor_integrity_gate PRIVATE
  sb_engine_internal_api
)
add_test(
  NAME public_datatype_binary_descriptor_integrity_gate
  COMMAND public_datatype_binary_descriptor_integrity_gate
)
set_tests_properties(public_datatype_binary_descriptor_integrity_gate PROPERTIES
  LABELS "public_release_correctness;public_datatype_binary_descriptor_integrity_gate;PCR-GATE-042;PCR-042;ELER-104;ELER-GATE-104;ELER-106;ELER-GATE-106;datatypes;binary_descriptor;integrity;transport;domain;document;structured;advanced_family;descriptor_format_policy;fail_closed;durable_codec_property"
)

add_executable(public_advanced_datatype_family_gate
  public_advanced_datatype_family_gate.cpp
)
target_compile_features(public_advanced_datatype_family_gate PRIVATE cxx_std_23)
target_link_libraries(public_advanced_datatype_family_gate PRIVATE
  sb_engine_internal_api
)
add_test(
  NAME public_advanced_datatype_family_gate
  COMMAND public_advanced_datatype_family_gate
)
set_tests_properties(public_advanced_datatype_family_gate PROPERTIES
  LABELS "public_release_correctness;public_advanced_datatype_family_gate;PCR-GATE-043;PCR-043;datatypes;advanced_family;descriptor_schema;index_admission;optimizer;compare_hash;fail_closed"
)

# PUBLIC_SBLR_UUID_INTEGRATION_GATE
add_executable(public_sblr_uuid_mga_route_integration_gate
  public_sblr_uuid_mga_route_integration_gate.cpp
)
target_compile_features(public_sblr_uuid_mga_route_integration_gate PRIVATE cxx_std_23)
target_link_libraries(public_sblr_uuid_mga_route_integration_gate PRIVATE
  sb_engine_sblr
  sb_core_memory
  sb_core_index
  sb_core_agents
  sb_storage_database
)
add_test(
  NAME public_sblr_uuid_mga_route_integration_gate
  COMMAND public_sblr_uuid_mga_route_integration_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_sblr_uuid_mga_route_integration_gate_tmp
)
set_tests_properties(public_sblr_uuid_mga_route_integration_gate PROPERTIES
  LABELS "public_release_correctness;public_sblr_uuid_mga_route_integration_gate;PCR-GATE-004;PCR-006;ELER-100;ELER-GATE-100;ELER-103;ELER-GATE-103;ELER-104;ELER-GATE-104;sblr;uuid;mga;integration;route;security;optimizer;index;agents;diagnostics;cluster_boundary;sblr_tampering;parser_boundary_abuse;uuid_spoofing;sblr_parser_authority_property"
)

add_executable(public_example_database_seed
  ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_example_database_seed.cpp
)
target_compile_features(public_example_database_seed PRIVATE cxx_std_23)
target_link_libraries(public_example_database_seed PRIVATE
  sb_storage_database
  sb_core_memory
  sb_core_uuid
)
add_test(
  NAME public_example_database_seed_gate
  COMMAND public_example_database_seed
          --output ${CMAKE_CURRENT_BINARY_DIR}/data/example/scratchbird-example.sbdb
          --manifest ${CMAKE_CURRENT_BINARY_DIR}/data/example/scratchbird-example.manifest.json
          --resource-seed-pack-root ${CMAKE_CURRENT_SOURCE_DIR}/../../resources/seed-packs/initial-resource-pack
          --overwrite
)
set_tests_properties(public_example_database_seed_gate PROPERTIES
  LABELS "public_release_correctness;public_example_database_seed_gate;PCR-GATE-001;PCR-002;data;example_database;storage_database;public_fixture"
)

add_test(
  NAME public_private_reference_scan_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_private_reference_scan.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
)
set_tests_properties(public_private_reference_scan_gate PROPERTIES
  LABELS "public_release_correctness;public_private_reference_scan_gate;PCR-GATE-002;PCR-003;release_hygiene"
)

# PUBLIC_PROOF_MIGRATION_GATE
add_test(
  NAME public_proof_migration_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_proof_migration_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --build-root ${CMAKE_BINARY_DIR}
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_proof_migration_gate/public_proof_migration.json
)
set_tests_properties(public_proof_migration_gate PROPERTIES
  LABELS "public_release_correctness;public_proof_migration_gate;PCR-GATE-003;PCR-004;release_hygiene;proof_migration;project_local_proof"
)

# PUBLIC_UUID_IDENTITY_RESOLUTION_GATE
add_test(
  NAME public_uuid_literal_classifier_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_uuid_literal_classifier.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --self-test
          --summary-json ${CMAKE_CURRENT_BINARY_DIR}/public_uuid_literal_classifier_gate/public_uuid_literal_inventory.json
          project/src
          project/tests
          project/tools
)
set_tests_properties(public_uuid_literal_classifier_gate PROPERTIES
  LABELS "public_release_correctness;public_uuid_literal_classifier_gate;PCR-GATE-006;PCR-007;uuid;identity;literal_inventory;allowlist"
)

add_test(
  NAME public_generated_sql_object_uuid_binding_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_uuid_literal_classifier.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --self-test
          --enforce-generated-sql-object-bindings
          --summary-json ${CMAKE_CURRENT_BINARY_DIR}/public_generated_sql_object_uuid_binding_gate/public_generated_sql_object_uuid_bindings.json
          project/src
          project/tools
          project/tests/database_lifecycle
          project/tests/release
)
set_tests_properties(public_generated_sql_object_uuid_binding_gate PROPERTIES
  LABELS "public_release_correctness;public_generated_sql_object_uuid_binding_gate;PCR-GATE-006;PCR-008;PCR-009;uuid;identity;generated_sql_object;literal_binding;resolver_conversion"
)

add_executable(public_uuid_identity_resolution_gate
  public_uuid_identity_resolution_gate.cpp
)
target_compile_features(public_uuid_identity_resolution_gate PRIVATE cxx_std_23)
target_link_libraries(public_uuid_identity_resolution_gate PRIVATE
  sb_engine_internal_api
  sb_storage_database
  sb_core_memory
  sb_core_uuid
)
add_test(
  NAME public_uuid_identity_resolution_gate
  COMMAND public_uuid_identity_resolution_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_uuid_identity_resolution_gate_tmp
)
set_tests_properties(public_uuid_identity_resolution_gate PROPERTIES
  LABELS "public_release_correctness;public_uuid_identity_resolution_gate;PCR-GATE-006;PCR-008;PCR-009;uuid;identity;generated_sql_object;catalog_resolver;recreate_reopen_restore;mga"
)

add_executable(public_index_readiness_matrix_gate
  public_index_readiness_matrix_gate.cpp
)
target_compile_features(public_index_readiness_matrix_gate PRIVATE cxx_std_23)
target_link_libraries(public_index_readiness_matrix_gate PRIVATE
  sb_core_index
  sb_core_uuid
)
add_test(
  NAME public_index_readiness_matrix_gate
  COMMAND public_index_readiness_matrix_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_index_readiness_matrix_gate_artifacts/public_index_readiness_matrix.csv
)
set_tests_properties(public_index_readiness_matrix_gate PROPERTIES
  LABELS "public_release_correctness;public_index_readiness_matrix_gate;PCR-GATE-050;PCR-050;PCR-051;index;readiness_matrix;provider_evidence;route_capability;exact_recheck"
)

add_executable(public_agent_readiness_matrix_gate
  public_agent_readiness_matrix_gate.cpp
)
target_compile_features(public_agent_readiness_matrix_gate PRIVATE cxx_std_23)
target_link_libraries(public_agent_readiness_matrix_gate PRIVATE
  sb_core_agents
)
add_test(
  NAME public_agent_readiness_matrix_gate
  COMMAND public_agent_readiness_matrix_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_agent_readiness_matrix_gate_artifacts/public_agent_readiness_matrix.csv
)
set_tests_properties(public_agent_readiness_matrix_gate PROPERTIES
  LABELS "public_release_correctness;public_agent_readiness_matrix_gate;PCR-GATE-080;PCR-080;agents;readiness_matrix;status_labels;cluster_provider_stub;dry_run;recommendation;workflow;actuator_required"
)

function(sb_public_agent_durable_gate target source extra_labels)
  add_executable(${target}
    ${CMAKE_CURRENT_SOURCE_DIR}/../agents/${source}
  )
  target_compile_features(${target} PRIVATE cxx_std_23)
  target_link_libraries(${target} PRIVATE
    sb_core_agents
    sb_storage_page
    sb_core_uuid
  )
  add_test(
    NAME ${target}
    COMMAND ${target}
  )
  set_tests_properties(${target} PROPERTIES
    LABELS "public_release_correctness;${target};PCR-GATE-081;PCR-081;ELER-104;ELER-GATE-104;agents;durable_catalog;crash_replay;actuator_proof;agent_record_property;${extra_labels}"
  )
endfunction()

sb_public_agent_durable_gate(
  public_agent_durable_runtime_catalog_gate
  agent_durable_runtime_catalog_gate.cpp
  "serialization;leases;approvals;overrides;migration"
)
sb_public_agent_durable_gate(
  public_agent_action_dispatch_idempotency_gate
  agent_action_dispatch_idempotency_gate.cpp
  "action_dispatch;idempotency;provider_proof;compensation"
)
sb_public_agent_durable_gate(
  public_agent_replay_compensation_quarantine_gate
  agent_replay_compensation_quarantine_gate.cpp
  "replay;compensation;quarantine;reservation_release"
)
sb_public_agent_durable_gate(
  public_page_allocation_manager_direct_preallocation_gate
  page_allocation_manager_direct_preallocation_gate.cpp
  "page_preallocation;real_subsystem_handler;fail_closed_no_mutation"
)

add_executable(public_agent_operator_explain_cluster_boundary_gate
  public_agent_operator_explain_cluster_boundary_gate.cpp
)
target_compile_features(public_agent_operator_explain_cluster_boundary_gate PRIVATE cxx_std_23)
target_link_libraries(public_agent_operator_explain_cluster_boundary_gate PRIVATE
  sb_core_agents
  sb_core_agents_cluster_boundary
  sb_cluster_provider
)
add_test(
  NAME public_agent_operator_explain_cluster_boundary_gate
  COMMAND public_agent_operator_explain_cluster_boundary_gate
)
set_tests_properties(public_agent_operator_explain_cluster_boundary_gate PROPERTIES
  LABELS "public_release_correctness;public_agent_operator_explain_cluster_boundary_gate;PCR-GATE-082;PCR-082;agents;operator_explain;cluster_boundary;no_cluster;external_provider_required;fail_closed"
)

add_executable(public_cluster_catalog_manifest_gate
  public_cluster_catalog_manifest_gate.cpp
)
target_compile_features(public_cluster_catalog_manifest_gate PRIVATE cxx_std_23)
target_link_libraries(public_cluster_catalog_manifest_gate PRIVATE
  sb_core_catalog
)
add_test(
  NAME public_cluster_catalog_manifest_gate
  COMMAND public_cluster_catalog_manifest_gate
)
set_tests_properties(public_cluster_catalog_manifest_gate PROPERTIES
  LABELS "public_release_correctness;public_cluster_catalog_manifest_gate;PCR-GATE-093;PCR-093;cluster_catalog;cluster_boundary;manifest;role_profile;external_provider_required;no_local_execution"
)

add_executable(public_cluster_schema_projection_gate
  public_cluster_schema_projection_gate.cpp
)
target_compile_features(public_cluster_schema_projection_gate PRIVATE cxx_std_23)
target_link_libraries(public_cluster_schema_projection_gate PRIVATE
  sb_core_catalog
)
add_test(
  NAME public_cluster_schema_projection_gate
  COMMAND public_cluster_schema_projection_gate
)
set_tests_properties(public_cluster_schema_projection_gate PROPERTIES
  LABELS "public_release_correctness;public_cluster_schema_projection_gate;PCR-GATE-094;PCR-094;cluster_catalog;cluster_boundary;schema_root;cluster_cache;projection_only;external_provider_required;no_local_execution"
)

add_executable(public_cluster_catalog_codec_resolver_gate
  public_cluster_catalog_codec_resolver_gate.cpp
)
target_compile_features(public_cluster_catalog_codec_resolver_gate PRIVATE cxx_std_23)
target_link_libraries(public_cluster_catalog_codec_resolver_gate PRIVATE
  sb_core_catalog
  sb_core_uuid
  sb_storage_page
)
add_test(
  NAME public_cluster_catalog_codec_resolver_gate
  COMMAND public_cluster_catalog_codec_resolver_gate
)
set_tests_properties(public_cluster_catalog_codec_resolver_gate PROPERTIES
  LABELS "public_release_correctness;public_cluster_catalog_codec_resolver_gate;PCR-GATE-095;PCR-095;cluster_catalog;codec;uuid_resolver;comment_resolver;checksum;no_embedded_names;no_property_bag"
)

add_executable(public_cluster_descriptor_manifest_gate
  public_cluster_descriptor_manifest_gate.cpp
)
target_compile_features(public_cluster_descriptor_manifest_gate PRIVATE cxx_std_23)
target_link_libraries(public_cluster_descriptor_manifest_gate PRIVATE
  sb_core_catalog
  sb_core_metrics
)
add_test(
  NAME public_cluster_descriptor_manifest_gate
  COMMAND public_cluster_descriptor_manifest_gate
)
set_tests_properties(public_cluster_descriptor_manifest_gate PROPERTIES
  LABELS "public_release_correctness;public_cluster_descriptor_manifest_gate;PCR-GATE-096;PCR-096;cluster_catalog;cluster_boundary;descriptor;decision;route;fence;topology;cleanup;security;metrics;authority_provenance;external_provider_required;no_local_execution"
)

add_executable(public_cluster_provider_boundary_cleanup_gate
  public_cluster_provider_boundary_cleanup_gate.cpp
)
target_compile_features(public_cluster_provider_boundary_cleanup_gate PRIVATE cxx_std_23)
target_link_libraries(public_cluster_provider_boundary_cleanup_gate PRIVATE
  sb_cluster_provider
  sb_core_agents
  sb_core_agents_cluster_boundary
)
add_test(
  NAME public_cluster_provider_boundary_cleanup_gate
  COMMAND public_cluster_provider_boundary_cleanup_gate
)
set_tests_properties(public_cluster_provider_boundary_cleanup_gate PROPERTIES
  LABELS "public_release_correctness;public_cluster_provider_boundary_cleanup_gate;PCR-GATE-097;PCR-097;ELER-108;ELER-GATE-108;cluster_boundary;external_provider;no_cluster;compile_link_stub;non_mutating;fail_closed;release_artifact_trust;linux_proof"
)

add_test(
  NAME public_cluster_boundary_cleanup_audit_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_cluster_boundary_cleanup_audit.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --build-root ${CMAKE_BINARY_DIR}
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_cluster_boundary_cleanup_audit_gate/public_cluster_boundary_cleanup_audit.json
)
set_tests_properties(public_cluster_boundary_cleanup_audit_gate PROPERTIES
  LABELS "public_release_correctness;public_cluster_boundary_cleanup_audit_gate;PCR-GATE-097;PCR-097;ELER-108;ELER-GATE-108;cluster_boundary;external_provider;static_audit;provider_modes;compile_link_stub;fail_closed;release_artifact_trust;linux_proof"
)

add_executable(public_cluster_catalog_readiness_suite
  public_cluster_catalog_readiness_suite.cpp
)
target_compile_features(public_cluster_catalog_readiness_suite PRIVATE cxx_std_23)
target_link_libraries(public_cluster_catalog_readiness_suite PRIVATE
  sb_cluster_provider
  sb_core_agents
  sb_core_agents_cluster_boundary
  sb_core_catalog
  sb_core_metrics
  sb_core_uuid
  sb_storage_page
)
add_test(
  NAME public_cluster_catalog_readiness_suite
  COMMAND public_cluster_catalog_readiness_suite
)
set_tests_properties(public_cluster_catalog_readiness_suite PROPERTIES
  LABELS "public_release_correctness;public_cluster_catalog_readiness_suite;PCR-GATE-098;PCR-098;cluster_catalog;cluster_boundary;readiness;conformance;schema_gating;projection_only;codec;resolver;comment_resolver;descriptor;metrics;cleanup_audit;external_provider_required;no_local_execution"
)

add_test(
  NAME public_cluster_catalog_manifest_check_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_cluster_catalog_manifest_check.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --build-root ${CMAKE_BINARY_DIR}
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_cluster_catalog_manifest_check_gate/public_cluster_catalog_manifest_check.json
)
set_tests_properties(public_cluster_catalog_manifest_check_gate PROPERTIES
  LABELS "public_release_correctness;public_cluster_catalog_manifest_check_gate;PCR-GATE-098;PCR-098;cluster_catalog;cluster_boundary;readiness;static_audit;public_proof;cleanup_audit"
)

add_test(
  NAME public_cluster_manifest_source_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/public_cluster_manifest_source_gate.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --build-root ${CMAKE_BINARY_DIR}
          --output-dir ${CMAKE_CURRENT_BINARY_DIR}/public_cluster_manifest_source_gate
)
set_tests_properties(public_cluster_manifest_source_gate PROPERTIES
  LABELS "public_release_correctness;public_cluster_manifest_source_gate;PCR-GATE-099;PCR-099;cluster_catalog;manifest_source;source_of_truth;generated_matrix;readable_proof;external_provider_required;no_local_execution"
)

add_executable(public_cluster_access_profile_gate
  public_cluster_access_profile_gate.cpp
)
target_compile_features(public_cluster_access_profile_gate PRIVATE cxx_std_23)
target_link_libraries(public_cluster_access_profile_gate PRIVATE
  sb_core_catalog
  sb_core_index
)
add_test(
  NAME public_cluster_access_profile_gate
  COMMAND public_cluster_access_profile_gate
)
set_tests_properties(public_cluster_access_profile_gate PROPERTIES
  LABELS "public_release_correctness;public_cluster_access_profile_gate;PCR-GATE-100;PCR-100;cluster_catalog;access_profile;index_profile;uuid_exact_lookup;node_role;route;decision_proof;shard_placement;epoch_generation;projection_invalidation;external_provider_required;no_local_execution"
)

add_executable(public_cluster_schema_version_gate
  public_cluster_schema_version_gate.cpp
)
target_compile_features(public_cluster_schema_version_gate PRIVATE cxx_std_23)
target_link_libraries(public_cluster_schema_version_gate PRIVATE
  sb_core_catalog
  sb_storage_database
  sb_core_memory
  sb_core_uuid
)
add_test(
  NAME public_cluster_schema_version_gate
  COMMAND public_cluster_schema_version_gate
)
set_tests_properties(public_cluster_schema_version_gate PROPERTIES
  LABELS "public_release_correctness;public_cluster_schema_version_gate;PCR-GATE-101;PCR-101;ELER-106;ELER-GATE-106;cluster_catalog;schema_version;codec_version;migration_refusal;compatibility;external_provider_required;cluster_stub_boundary;no_local_execution;fail_closed"
)

add_executable(public_cluster_provider_handshake_gate
  public_cluster_provider_handshake_gate.cpp
)
target_compile_features(public_cluster_provider_handshake_gate PRIVATE cxx_std_23)
target_link_libraries(public_cluster_provider_handshake_gate PRIVATE
  sb_cluster_provider
)
add_test(
  NAME public_cluster_provider_handshake_gate
  COMMAND public_cluster_provider_handshake_gate
)
set_tests_properties(public_cluster_provider_handshake_gate PROPERTIES
  LABELS "public_release_correctness;public_cluster_provider_handshake_gate;PCR-GATE-102;PCR-102;ELER-103;ELER-GATE-103;cluster_boundary;provider_abi;catalog_manifest;operation_set;feature_flags;authority_domains;digest_compatibility;route_admission;external_provider_required;fail_closed;cluster_boundary_bypass"
)

add_test(
  NAME public_cluster_build_matrix_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_cluster_build_matrix_gate.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --build-root ${CMAKE_BINARY_DIR}
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_cluster_build_matrix_gate/public_cluster_build_matrix.json
)
set_tests_properties(public_cluster_build_matrix_gate PROPERTIES
  LABELS "public_release_correctness;public_cluster_build_matrix_gate;PCR-GATE-103;PCR-103;cluster_boundary;build_matrix;no_cluster;compile_link_stub;external_provider_mode;non_mutating;fail_closed"
)

add_executable(public_cluster_catalog_backup_export_gate
  public_cluster_catalog_backup_export_gate.cpp
)
target_compile_features(public_cluster_catalog_backup_export_gate PRIVATE cxx_std_23)
target_link_libraries(public_cluster_catalog_backup_export_gate PRIVATE
  sb_engine_internal_api
  sb_core_catalog
  sb_core_uuid
)
add_test(
  NAME public_cluster_catalog_backup_export_gate
  COMMAND public_cluster_catalog_backup_export_gate
)
set_tests_properties(public_cluster_catalog_backup_export_gate PROPERTIES
  LABELS "public_release_correctness;public_cluster_catalog_backup_export_gate;PCR-GATE-104;PCR-104;cluster_catalog;backup_restore;import_export;identity_preserve;identity_remap;identity_reject;identity_quarantine;resolver_comment_security_projection;external_provider_required;fail_closed"
)

add_executable(public_cluster_readable_view_gate
  public_cluster_readable_view_gate.cpp
)
target_compile_features(public_cluster_readable_view_gate PRIVATE cxx_std_23)
target_link_libraries(public_cluster_readable_view_gate PRIVATE
  sb_core_catalog
)
add_test(
  NAME public_cluster_readable_view_gate
  COMMAND public_cluster_readable_view_gate
)
set_tests_properties(public_cluster_readable_view_gate PROPERTIES
  LABELS "public_release_correctness;public_cluster_readable_view_gate;PCR-GATE-105;PCR-105;cluster_catalog;readable_views;resolver_join;comment_join;joined_cluster_authority;external_provider_required;fail_closed"
)

add_executable(public_cluster_projection_redaction_gate
  public_cluster_projection_redaction_gate.cpp
)
target_compile_features(public_cluster_projection_redaction_gate PRIVATE cxx_std_23)
target_link_libraries(public_cluster_projection_redaction_gate PRIVATE
  sb_engine_internal_api
)
add_test(
  NAME public_cluster_projection_redaction_gate
  COMMAND public_cluster_projection_redaction_gate
)
set_tests_properties(public_cluster_projection_redaction_gate PROPERTIES
  LABELS "public_release_correctness;public_cluster_projection_redaction_gate;PCR-GATE-106;PCR-106;cluster_catalog;projection_redaction;support_bundle;security;retention;fail_closed"
)

add_executable(public_cluster_projection_authority_guard_gate
  public_cluster_projection_authority_guard_gate.cpp
)
target_compile_features(public_cluster_projection_authority_guard_gate PRIVATE cxx_std_23)
target_link_libraries(public_cluster_projection_authority_guard_gate PRIVATE
  sb_engine_optimizer_contract_only
  sb_core_agents
)
add_test(
  NAME public_cluster_projection_authority_guard_gate
  COMMAND public_cluster_projection_authority_guard_gate
)
set_tests_properties(public_cluster_projection_authority_guard_gate PROPERTIES
  LABELS "public_release_correctness;public_cluster_projection_authority_guard_gate;PCR-GATE-107;PCR-107;cluster_catalog;optimizer;agents;projection_authority;external_provider_authority;evidence_only;fail_closed"
)

add_executable(public_cluster_catalog_crypto_evidence_gate
  public_cluster_catalog_crypto_evidence_gate.cpp
)
target_compile_features(public_cluster_catalog_crypto_evidence_gate PRIVATE cxx_std_23)
target_link_libraries(public_cluster_catalog_crypto_evidence_gate PRIVATE
  sb_engine_internal_api
  sb_core_catalog
)
add_test(
  NAME public_cluster_catalog_crypto_evidence_gate
  COMMAND public_cluster_catalog_crypto_evidence_gate
)
set_tests_properties(public_cluster_catalog_crypto_evidence_gate PROPERTIES
  LABELS "public_release_correctness;public_cluster_catalog_crypto_evidence_gate;PCR-GATE-108;PCR-108;cluster_catalog;crypto_evidence;canonical_serialization;sha256;hmac;signature_ready;weak_evidence_rejected;fail_closed"
)

add_executable(public_codec_property_gate
  public_codec_property_gate.cpp
)
target_compile_features(public_codec_property_gate PRIVATE cxx_std_23)
target_link_libraries(public_codec_property_gate PRIVATE
  sb_core_catalog
  sb_storage_page
  sb_core_uuid
)
add_test(
  NAME public_codec_property_gate
  COMMAND public_codec_property_gate
)
set_tests_properties(public_codec_property_gate PROPERTIES
  LABELS "public_release_correctness;public_codec_property_gate;PCR-GATE-114;PCR-114;ELER-104;ELER-GATE-104;durable_codec;property;bounds;checksum;version;authority_refusal;fail_closed"
)

add_test(
  NAME public_durable_codec_fuzz
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../fuzz/public_durable_codec_fuzz.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_durable_codec_fuzz/public_durable_codec_fuzz.json
)
set_tests_properties(public_durable_codec_fuzz PROPERTIES
  LABELS "public_release_correctness;public_durable_codec_fuzz;PCR-GATE-114;PCR-114;ELER-104;ELER-GATE-104;durable_codec;fuzz;property;bounds;checksum;version;authority_refusal;fail_closed"
)

add_test(
  NAME public_crash_fault_matrix
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../fault_injection/public_crash_fault_matrix.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_crash_fault_matrix/public_crash_fault_matrix.json
)
set_tests_properties(public_crash_fault_matrix PROPERTIES
  LABELS "public_release_correctness;public_crash_fault_matrix;PCR-GATE-115;PCR-115;ELER-080;ELER-GATE-080;crash_reopen;fault_injection;mga_authority;fail_closed"
)

add_test(
  NAME public_crash_fault_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/public_crash_fault_gate.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_crash_fault_gate/public_crash_fault_gate.json
)
set_tests_properties(public_crash_fault_gate PROPERTIES
  LABELS "public_release_correctness;public_crash_fault_gate;PCR-GATE-115;PCR-115;ELER-080;ELER-GATE-080;crash_reopen;fault_injection;mga_authority;fail_closed"
)

add_test(
  NAME public_release_soak_lane
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../soak/public_release_soak_lane.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_release_soak_lane/public_release_soak_lane.json
)
set_tests_properties(public_release_soak_lane PROPERTIES
  LABELS "public_release_correctness;public_release_soak_lane;PCR-GATE-116;PCR-116;ELER-081;ELER-GATE-081;ELER-102;ELER-GATE-102;soak;bounded;concurrency;diagnostics;fail_closed;linux_proof"
)

add_test(
  NAME public_release_soak_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/public_release_soak_gate.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_release_soak_gate/public_release_soak_gate.json
)
set_tests_properties(public_release_soak_gate PROPERTIES
  LABELS "public_release_correctness;public_release_soak_gate;PCR-GATE-116;PCR-116;ELER-081;ELER-GATE-081;ELER-102;ELER-GATE-102;soak;bounded;concurrency;diagnostics;fail_closed;linux_proof"
)

add_executable(public_index_provider_evidence_split_gate
  public_index_provider_evidence_split_gate.cpp
)
target_compile_features(public_index_provider_evidence_split_gate PRIVATE cxx_std_23)
target_link_libraries(public_index_provider_evidence_split_gate PRIVATE
  sb_core_index
)
add_test(
  NAME public_index_provider_evidence_split_gate
  COMMAND public_index_provider_evidence_split_gate
)
set_tests_properties(public_index_provider_evidence_split_gate PROPERTIES
  LABELS "public_release_correctness;public_index_provider_evidence_split_gate;PCR-GATE-051;PCR-051;index;provider_evidence;static_contract;durable_closure;fail_closed"
)

add_executable(public_index_durable_metadata_validator_gate
  public_index_durable_metadata_validator_gate.cpp
)
target_compile_features(public_index_durable_metadata_validator_gate PRIVATE cxx_std_23)
target_link_libraries(public_index_durable_metadata_validator_gate PRIVATE
  sb_core_index
  sb_core_uuid
)
add_test(
  NAME public_index_durable_metadata_validator_gate
  COMMAND public_index_durable_metadata_validator_gate
)
set_tests_properties(public_index_durable_metadata_validator_gate PROPERTIES
  LABELS "public_release_correctness;public_index_durable_metadata_validator_gate;PCR-GATE-052;PCR-052;ELER-104;ELER-GATE-104;index;durable_metadata;metapage;provider_artifact;checksum_profile;family_validator;index_authority_property"
)

add_executable(public_index_dml_maintenance_strategy_gate
  public_index_dml_maintenance_strategy_gate.cpp
)
target_compile_features(public_index_dml_maintenance_strategy_gate PRIVATE cxx_std_23)
target_link_libraries(public_index_dml_maintenance_strategy_gate PRIVATE
  sb_engine_internal_api
  sb_core_index
)
add_test(
  NAME public_index_dml_maintenance_strategy_gate
  COMMAND public_index_dml_maintenance_strategy_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_index_dml_maintenance_strategy_gate_artifacts/public_index_dml_maintenance_strategy_matrix.csv
)
set_tests_properties(public_index_dml_maintenance_strategy_gate PROPERTIES
  LABELS "public_release_correctness;public_index_dml_maintenance_strategy_gate;PCR-GATE-053;PCR-053;ELER-104;ELER-GATE-104;index;dml;maintenance_strategy;exact_recheck;deferred_delta;change_buffer;index_authority_property"
)

add_executable(public_optimizer_implementation_matrix_gate
  public_optimizer_implementation_matrix_gate.cpp
)
target_compile_features(public_optimizer_implementation_matrix_gate PRIVATE cxx_std_23)
target_link_libraries(public_optimizer_implementation_matrix_gate PRIVATE
  sb_engine_optimizer_contract_only
)
add_test(
  NAME public_optimizer_implementation_matrix_gate
  COMMAND public_optimizer_implementation_matrix_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_optimizer_implementation_matrix_gate_artifacts/public_optimizer_implementation_matrix.csv
)
set_tests_properties(public_optimizer_implementation_matrix_gate PROPERTIES
  LABELS "public_release_correctness;public_optimizer_implementation_matrix_gate;PCR-GATE-060;PCR-060;optimizer;implementation_matrix;plan_cache;explain;benchmark_clean;runtime_evidence"
)

add_executable(public_optimizer_catalog_backed_planning_gate
  public_optimizer_catalog_backed_planning_gate.cpp
)
target_compile_features(public_optimizer_catalog_backed_planning_gate PRIVATE cxx_std_23)
target_link_libraries(public_optimizer_catalog_backed_planning_gate PRIVATE
  sb_engine_optimizer_contract_only
)
add_test(
  NAME public_optimizer_catalog_backed_planning_gate
  COMMAND public_optimizer_catalog_backed_planning_gate
)
set_tests_properties(public_optimizer_catalog_backed_planning_gate PROPERTIES
  LABELS "public_release_correctness;public_optimizer_catalog_backed_planning_gate;PCR-GATE-061;PCR-061;ELER-104;ELER-GATE-104;optimizer;catalog_backed;production_planning;benchmark_clean;local_default_refusal;optimizer_property"
)

add_executable(public_optimizer_hints_calibration_feedback_gate
  public_optimizer_hints_calibration_feedback_gate.cpp
)
target_compile_features(public_optimizer_hints_calibration_feedback_gate PRIVATE cxx_std_23)
target_link_libraries(public_optimizer_hints_calibration_feedback_gate PRIVATE
  sb_engine_optimizer_contract_only
)
add_test(
  NAME public_optimizer_hints_calibration_feedback_gate
  COMMAND public_optimizer_hints_calibration_feedback_gate
)
set_tests_properties(public_optimizer_hints_calibration_feedback_gate PROPERTIES
  LABELS "public_release_correctness;public_optimizer_hints_calibration_feedback_gate;PCR-GATE-062;PCR-062;optimizer;hints;calibration;statistics_lifecycle;feedback;explain_lineage"
)

add_executable(public_optimizer_expression_plan_cache_validation_gate
  public_optimizer_expression_plan_cache_validation_gate.cpp
)
target_compile_features(public_optimizer_expression_plan_cache_validation_gate PRIVATE cxx_std_23)
target_link_libraries(public_optimizer_expression_plan_cache_validation_gate PRIVATE
  sb_engine_optimizer_contract_only
)
add_test(
  NAME public_optimizer_expression_plan_cache_validation_gate
  COMMAND public_optimizer_expression_plan_cache_validation_gate
)
set_tests_properties(public_optimizer_expression_plan_cache_validation_gate PROPERTIES
  LABELS "public_release_correctness;public_optimizer_expression_plan_cache_validation_gate;PCR-GATE-063;PCR-063;ELER-104;ELER-GATE-104;ELER-106;ELER-GATE-106;optimizer;expression_normalization;plan_cache;compatibility_epoch;validation_hardening;sblr_uuid_mga;optimizer_property"
)

add_test(
  NAME public_dependency_sbom_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_dependency_sbom.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --build-root ${CMAKE_BINARY_DIR}
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_dependency_sbom_gate/public_dependency_sbom.json
)
set_tests_properties(public_dependency_sbom_gate PROPERTIES
  LABELS "public_release_correctness;public_dependency_sbom_gate;PCR-GATE-111;PCR-111;ELER-108;ELER-GATE-108;release_hygiene;dependency;license;sbom;vulnerability_scan;provenance;generated_artifacts;llvm_policy;release_artifact_trust;linux_proof"
)

# PUBLIC_PLATFORM_MATRIX_GATE
add_test(
  NAME public_platform_matrix_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_platform_matrix_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --build-root ${CMAKE_BINARY_DIR}
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_platform_matrix_gate/public_platform_matrix.json
          --configured-system-name ${CMAKE_SYSTEM_NAME}
          --configured-system-processor ${CMAKE_SYSTEM_PROCESSOR}
          --configured-cxx-compiler-id ${CMAKE_CXX_COMPILER_ID}
          --configured-cxx-compiler-version ${CMAKE_CXX_COMPILER_VERSION}
          --configured-cmake-version ${CMAKE_VERSION}
          --configured-cxx-standard ${CMAKE_CXX_STANDARD}
          --cluster-provider-enabled ${SB_ENABLE_CLUSTER_PROVIDER}
          --cluster-provider-stub ${SB_CLUSTER_PROVIDER_STUB}
          --cluster-provider-external-library=${SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY}
)
set_tests_properties(public_platform_matrix_gate PROPERTIES
  LABELS "public_release_correctness;public_platform_matrix_gate;PCR-GATE-109;PCR-109;ELER-005;ELER-GATE-004;ELER-108;ELER-GATE-108;release_hygiene;platform;linux;windows;freebsd;debug_symbols;cluster_boundary;fail_closed;release_artifact_trust;linux_proof"
)

add_test(
  NAME public_no_skip_waiver_xfail_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../engine_listener_enterprise/engine_listener_enterprise_gate.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --build-root ${CMAKE_BINARY_DIR}
          --mode no-skip-waiver-xfail-release-proof
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_no_skip_waiver_xfail_gate/public_no_skip_waiver_xfail.json
)
set_tests_properties(public_no_skip_waiver_xfail_gate PROPERTIES
  LABELS "public_release_correctness;public_no_skip_waiver_xfail_gate;ELER-083;ELER-GATE-083;release_hygiene;declared_feature_result_classifier;no_skip_waiver_xfail_completion;linux_proof"
)

add_test(
  NAME public_row_level_traceability_manifest_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../engine_listener_enterprise/engine_listener_enterprise_gate.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --build-root ${CMAKE_BINARY_DIR}
          --mode row-level-traceability-manifest
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_row_level_traceability_manifest_gate/public_row_level_traceability_manifest.json
)
set_tests_properties(public_row_level_traceability_manifest_gate PROPERTIES
  LABELS "public_release_correctness;public_row_level_traceability_manifest_gate;ELER-084;ELER-GATE-084;release_hygiene;row_level_traceability_manifest;declared_feature_traceability;donor_sblr_traceability;linux_proof"
)

add_test(
  NAME public_parser_contract_freeze_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../engine_listener_enterprise/engine_listener_enterprise_gate.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --build-root ${CMAKE_BINARY_DIR}
          --mode parser-facing-contract-freeze
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_parser_contract_freeze_gate/public_parser_contract_freeze.json
)
set_tests_properties(public_parser_contract_freeze_gate PROPERTIES
  LABELS "public_release_correctness;public_parser_contract_freeze_gate;ELER-085;ELER-GATE-085;ELER-106;ELER-GATE-106;release_hygiene;parser_facing_contract_freeze;versioned_parser_contract;compatibility_version_policy;linux_proof"
)

add_test(
  NAME public_generated_resource_determinism_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../engine_listener_enterprise/engine_listener_enterprise_gate.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --build-root ${CMAKE_BINARY_DIR}
          --mode generated-resource-determinism
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_generated_resource_determinism_gate/public_generated_resource_determinism.json
)
set_tests_properties(public_generated_resource_determinism_gate PROPERTIES
  LABELS "public_release_correctness;public_generated_resource_determinism_gate;ELER-086;ELER-GATE-086;release_hygiene;generated_resource_determinism;fixture_hash_manifest;two_pass_generation;linux_proof"
  RESOURCE_LOCK generated_resource_determinism_workdir
)

add_test(
  NAME public_sbsql_parser_sync_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../engine_listener_enterprise/engine_listener_enterprise_gate.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --build-root ${CMAKE_BINARY_DIR}
          --mode sbsql-parser-spec-execution_plan-sync
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_sbsql_parser_sync_gate/public_sbsql_parser_sync.json
)
set_tests_properties(public_sbsql_parser_sync_gate PROPERTIES
  LABELS "public_release_correctness;public_sbsql_parser_sync_gate;ELER-089;ELER-GATE-089;release_hygiene;sbsql_parser_spec_execution_plan_sync;row_expanded_sync;donor_dialect_paste_through_rejection;linux_proof"
)

add_executable(public_sanitizer_warning_probe
  public_sanitizer_warning_probe.cpp
)
target_compile_features(public_sanitizer_warning_probe PRIVATE cxx_std_23)

if(NOT SB_PUBLIC_RELEASE_HARDENING_NESTED)
  add_test(
    NAME public_sanitizer_static_analysis_gate
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_sanitizer_static_analysis_gate.py
            --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
            --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
            --build-root ${CMAKE_BINARY_DIR}
            --work-root ${CMAKE_CURRENT_BINARY_DIR}/public_sanitizer_static_analysis_gate
            --output ${CMAKE_CURRENT_BINARY_DIR}/public_sanitizer_static_analysis_gate/public_sanitizer_static_analysis.json
            --cmake ${CMAKE_COMMAND}
            --generator Ninja
            --c-compiler ${CMAKE_C_COMPILER}
            --cxx-compiler ${CMAKE_CXX_COMPILER}
  )
  set_tests_properties(public_sanitizer_static_analysis_gate PROPERTIES
    LABELS "public_release_correctness;public_sanitizer_static_analysis_gate;PCR-GATE-110;PCR-110;ELER-108;ELER-GATE-108;release_hygiene;sanitizer;static_analysis;warnings_as_errors;source_hygiene;compiler_hardening;release_artifact_trust;linux_proof"
  )
  if(_sb_release_nested_configure_env)
    set_tests_properties(public_sanitizer_static_analysis_gate PROPERTIES
      ENVIRONMENT "${_sb_release_nested_configure_env}"
    )
  endif()
endif()

# PUBLIC_PRODUCTION_BUILD_GATE
add_test(
  NAME public_production_feature_audit_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_production_feature_audit_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --build-root ${CMAKE_BINARY_DIR}
          --work-root ${CMAKE_CURRENT_BINARY_DIR}/public_production_feature_audit_gate
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_production_feature_audit_gate/public_production_feature_audit.json
          --cmake ${CMAKE_COMMAND}
          --c-compiler ${CMAKE_C_COMPILER}
          --cxx-compiler ${CMAKE_CXX_COMPILER}
)
set_tests_properties(public_production_feature_audit_gate PROPERTIES
  LABELS "public_release_correctness;public_production_feature_audit_gate;PCR-GATE-113;PCR-113;release_hygiene;production_build;fixture_refusal;cluster_boundary;fail_closed"
)
if(_sb_release_nested_configure_env)
  set_tests_properties(public_production_feature_audit_gate PROPERTIES
    ENVIRONMENT "${_sb_release_nested_configure_env}"
  )
endif()

# PUBLIC_DOC_CONSISTENCY_GATE
add_test(
  NAME public_doc_consistency_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_doc_consistency_check.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_doc_consistency_gate/public_doc_consistency.json
)
set_tests_properties(public_doc_consistency_gate PROPERTIES
  LABELS "public_release_correctness;public_doc_consistency_gate;PCR-GATE-117;PCR-117;ELER-109;ELER-GATE-109;release_hygiene;documentation;readiness_matrix;private_reference_scan;enterprise_documentation;linux_proof"
)

# PUBLIC_ENTERPRISE_DOCUMENTATION_GATE
add_test(
  NAME public_enterprise_documentation_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_enterprise_documentation_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --csv-output ${CMAKE_CURRENT_BINARY_DIR}/public_enterprise_documentation_gate/public_enterprise_documentation_matrix.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_enterprise_documentation_gate/public_enterprise_documentation_gate.json
)
set_tests_properties(public_enterprise_documentation_gate PROPERTIES
  LABELS "public_release_correctness;public_enterprise_documentation_gate;ELER-109;ELER-GATE-109;enterprise_documentation;architecture;trust_boundaries;lifecycle;file_format;mga;security;authorization;toast;optimizer;memory;runbooks;support_bundle;upgrade;backup_restore;limitations;support_states;linux_proof"
)

# PUBLIC_API_BOUNDARY_GATE
add_test(
  NAME public_api_boundary_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_api_boundary_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --build-root ${CMAKE_BINARY_DIR}
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_api_boundary_gate/public_api_boundary.json
)
set_tests_properties(public_api_boundary_gate PROPERTIES
  LABELS "public_release_correctness;public_api_boundary_gate;PCR-GATE-119;PCR-119;release_hygiene;public_api;include_boundary;engine_only;cluster_boundary"
)

# PUBLIC_INSTALL_CONSUMER_GATE
add_test(
  NAME public_install_consumer_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_install_consumer_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --build-root ${CMAKE_BINARY_DIR}
          --work-root ${CMAKE_CURRENT_BINARY_DIR}/public_install_consumer_gate
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_install_consumer_gate/public_install_consumer.json
          --cmake ${CMAKE_COMMAND}
)
set_tests_properties(public_install_consumer_gate PROPERTIES
  LABELS "public_release_correctness;public_install_consumer_gate;PCR-GATE-120;PCR-120;release_hygiene;install;external_consumer;engine_only;public_api"
)

# PUBLIC_NON_ENGINE_INSTALL_GATE
add_test(
  NAME public_non_engine_install_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_non_engine_install_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --build-root ${CMAKE_BINARY_DIR}
          --work-root ${CMAKE_CURRENT_BINARY_DIR}/public_non_engine_install_gate
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_non_engine_install_gate/public_non_engine_install.json
          --cmake ${CMAKE_COMMAND}
)
set_tests_properties(public_non_engine_install_gate PROPERTIES
  LABELS "public_release_correctness;public_non_engine_install_gate;PCR-GATE-140;PCR-140;release_hygiene;install;non_engine_install;manager;sbmn_manager;engine_only_export;linux_proof"
)
if(_sb_release_nested_configure_env)
  set_tests_properties(public_non_engine_install_gate PROPERTIES
    ENVIRONMENT "${_sb_release_nested_configure_env}"
  )
endif()

add_executable(public_release_version_metadata_tool
  ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_release_version_metadata.cpp
)
target_compile_features(public_release_version_metadata_tool PRIVATE cxx_std_23)
target_compile_definitions(public_release_version_metadata_tool PRIVATE
  SB_PUBLIC_RELEASE_PROJECT_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(public_release_version_metadata_tool PRIVATE
  sb_engine
  sb_storage_disk
  sb_storage_page
  sb_core_catalog
  sb_core_datatypes
  sb_core_uuid
)
add_test(
  NAME public_release_version_metadata_gate
  COMMAND public_release_version_metadata_tool
          --self-test
          --out ${CMAKE_CURRENT_BINARY_DIR}/public_release_version_metadata_gate/public_release_version_metadata.json
)
set_tests_properties(public_release_version_metadata_gate PROPERTIES
  LABELS "public_release_correctness;public_release_version_metadata_gate;PCR-GATE-112;PCR-112;ELER-108;ELER-GATE-108;release_hygiene;version_metadata;compatibility;database_format;catalog_schema;catalog_codec;datatype_wire;debug_symbols;fail_closed;release_artifact_trust;linux_proof"
)

# PUBLIC_ARTIFACT_REPRODUCIBILITY_GATE
add_test(
  NAME public_artifact_reproducibility_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_reproducible_export.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --build-root ${CMAKE_BINARY_DIR}
          --work-root ${CMAKE_CURRENT_BINARY_DIR}/public_artifact_reproducibility_gate/work
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_artifact_reproducibility_gate/public_artifact_reproducibility.json
          --python ${Python3_EXECUTABLE}
          --version-metadata-tool $<TARGET_FILE:public_release_version_metadata_tool>
          --configured-system-name ${CMAKE_SYSTEM_NAME}
          --configured-system-processor ${CMAKE_SYSTEM_PROCESSOR}
          --configured-cxx-compiler-id ${CMAKE_CXX_COMPILER_ID}
          --configured-cxx-compiler-version ${CMAKE_CXX_COMPILER_VERSION}
          --configured-cmake-version ${CMAKE_VERSION}
          --configured-cxx-standard ${CMAKE_CXX_STANDARD}
          --cluster-provider-enabled ${SB_ENABLE_CLUSTER_PROVIDER}
          --cluster-provider-stub ${SB_CLUSTER_PROVIDER_STUB}
          --cluster-provider-external-library=${SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY}
)
set_tests_properties(public_artifact_reproducibility_gate PROPERTIES
  DEPENDS public_release_version_metadata_gate
  LABELS "public_release_correctness;public_artifact_reproducibility_gate;PCR-GATE-118;PCR-118;ELER-108;ELER-GATE-108;release_hygiene;reproducibility;cleanup_manifest;generated_matrices;sbom;version_metadata;release_artifact_trust;linux_proof"
)

# PUBLIC_ARTIFACT_SIGNATURE_GATE
add_test(
  NAME public_artifact_signature_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_artifact_signature_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --build-root ${CMAKE_BINARY_DIR}
          --work-root ${CMAKE_CURRENT_BINARY_DIR}/public_artifact_signature_gate/work
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_artifact_signature_gate/public_artifact_signature.json
)
set_tests_properties(public_artifact_signature_gate PROPERTIES
  LABELS "public_release_correctness;public_artifact_signature_gate;PCR-GATE-127;PCR-127;ELER-108;ELER-GATE-108;release_hygiene;artifact_signing;checksum;signature_ready;sha256;hmac;fail_closed;public_export;release_artifact_trust;linux_proof"
)

# PUBLIC_DIAGNOSTIC_STABILITY_GATE
add_test(
  NAME public_diagnostic_stability_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_diagnostic_matrix_generator.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --matrix-output ${CMAKE_CURRENT_BINARY_DIR}/public_diagnostic_stability_gate/public_diagnostic_matrix.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_diagnostic_stability_gate/public_diagnostic_matrix.json
)
set_tests_properties(public_diagnostic_stability_gate PROPERTIES
  LABELS "public_release_correctness;public_diagnostic_stability_gate;PCR-GATE-121;PCR-121;diagnostics;stability_matrix;redaction;compatibility;unsupported_features"
)

# PUBLIC_CRYPTO_ENTROPY_GATE
add_test(
  NAME public_crypto_entropy_policy_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/public_crypto_entropy_policy_gate.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_crypto_entropy_policy_gate/public_crypto_entropy_policy.json
)
set_tests_properties(public_crypto_entropy_policy_gate PROPERTIES
  LABELS "public_release_correctness;public_crypto_entropy_policy_gate;PCR-GATE-122;PCR-122;ELER-108;ELER-GATE-108;security;crypto;entropy;hmac;signature_ready;protected_material;fail_closed;release_artifact_trust;linux_proof"
)

# PUBLIC_DEFAULT_CONFIG_CHECK
add_test(
  NAME public_default_config_check
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_default_config_check.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_default_config_check/public_default_config_check.json
)
set_tests_properties(public_default_config_check PROPERTIES
  LABELS "public_release_correctness;public_default_config_check;PCR-GATE-123;PCR-123;ELER-108;ELER-GATE-108;release_hygiene;secure_defaults;startup;config;security;memory;cluster_boundary;release_artifact_trust;linux_proof"
)

if(NOT TARGET sb_server_core)
  message(FATAL_ERROR "public_secure_defaults_gate requires sb_server_core")
endif()

# PUBLIC_SECURE_DEFAULTS_GATE
add_executable(public_secure_defaults_gate
  public_secure_defaults_gate.cpp
)
target_compile_features(public_secure_defaults_gate PRIVATE cxx_std_23)
target_link_libraries(public_secure_defaults_gate PRIVATE
  sb_server_core
)
add_test(
  NAME public_secure_defaults_gate
  COMMAND public_secure_defaults_gate
)
set_tests_properties(public_secure_defaults_gate PROPERTIES
  LABELS "public_release_correctness;public_secure_defaults_gate;PCR-GATE-123;PCR-123;ELER-108;ELER-GATE-108;release_hygiene;secure_defaults;startup;config;security;memory;permissions;cluster_boundary;fail_closed;release_artifact_trust;linux_proof"
)

# PUBLIC_UNSUPPORTED_FEATURE_GATE
add_test(
  NAME public_unsupported_feature_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_unsupported_feature_matrix.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --matrix-output ${CMAKE_CURRENT_BINARY_DIR}/public_unsupported_feature_gate/public_unsupported_feature_matrix.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_unsupported_feature_gate/public_unsupported_feature_matrix.json
)
set_tests_properties(public_unsupported_feature_gate PROPERTIES
  LABELS "public_release_correctness;public_unsupported_feature_gate;PCR-GATE-124;PCR-124;unsupported_features;release_hygiene;fail_closed;no_overclaim;cluster_boundary;external_provider_required;compile_time_disabled;policy_blocked"
)

# PUBLIC_GENERATED_PROVENANCE_GATE
add_test(
  NAME public_generated_provenance_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_generated_source_provenance.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --build-root ${CMAKE_BINARY_DIR}
          --work-root ${CMAKE_CURRENT_BINARY_DIR}/public_generated_provenance_gate/work
          --provenance-output ${CMAKE_CURRENT_BINARY_DIR}/public_generated_provenance_gate/public_generated_source_provenance.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_generated_provenance_gate/public_generated_source_provenance.json
)
set_tests_properties(public_generated_provenance_gate PROPERTIES
  LABELS "public_release_correctness;public_generated_provenance_gate;PCR-GATE-125;PCR-125;ELER-108;ELER-GATE-108;release_hygiene;generated_source;source_provenance;drift_detection;regeneration;checked_in_authority;release_artifact_trust;linux_proof"
)

if(NOT SB_PUBLIC_RELEASE_EXPORT_NESTED)
  # PUBLIC_EXAMPLE_SMOKE_GATE
  add_test(
    NAME public_example_smoke_gate
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_example_smoke_gate.py
            --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
            --build-root ${CMAKE_BINARY_DIR}
            --work-root ${CMAKE_CURRENT_BINARY_DIR}/public_example_smoke_gate/work
            --output ${CMAKE_CURRENT_BINARY_DIR}/public_example_smoke_gate/public_example_smoke.json
            --cmake ${CMAKE_COMMAND}
            --ctest ${CMAKE_CTEST_COMMAND}
            --c-compiler ${CMAKE_C_COMPILER}
            --cxx-compiler ${CMAKE_CXX_COMPILER}
            --parallel 1
  )
  set_tests_properties(public_example_smoke_gate PROPERTIES
    LABELS "public_release_correctness;public_example_smoke_gate;PCR-GATE-126;PCR-126;examples;smoke;public_export;create;open;schema;insert;select;transaction;rollback;backup;verify;diagnostics"
  )
endif()

add_test(
  NAME public_policy_pack_manifest_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_policy_pack_manifest_gate.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
)
set_tests_properties(public_policy_pack_manifest_gate PROPERTIES
  LABELS "public_release_correctness;public_policy_pack_manifest_gate;PCR-GATE-128;PCR-128;policy;security;database_lifecycle;default_policy_pack;local_password_only;catalog_materialization;release_hygiene"
)

add_test(
  NAME public_policy_coverage_matrix_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_policy_coverage_matrix.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_policy_coverage_matrix_gate/public_policy_coverage_matrix.csv
)
set_tests_properties(public_policy_coverage_matrix_gate PROPERTIES
  LABELS "public_release_correctness;public_policy_coverage_matrix_gate;PCR-GATE-130;PCR-130;policy;security;default_policy_pack;coverage_matrix;unsupported_fail_closed;cluster_boundary"
)

add_executable(public_policy_pack_catalog_import_gate
  public_policy_pack_catalog_import_gate.cpp
)
target_compile_features(public_policy_pack_catalog_import_gate PRIVATE cxx_std_23)
target_compile_definitions(public_policy_pack_catalog_import_gate PRIVATE
  SB_DEFAULT_POLICY_PACK_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../resources/policy-packs/default-local-password"
)
target_link_libraries(public_policy_pack_catalog_import_gate PRIVATE
  sb_storage_database
  sb_storage_page
  sb_storage_disk
  sb_core_catalog
  sb_core_uuid
)
add_test(
  NAME public_policy_pack_catalog_import_gate
  COMMAND public_policy_pack_catalog_import_gate
)
set_tests_properties(public_policy_pack_catalog_import_gate PROPERTIES
  LABELS "public_release_correctness;public_policy_pack_catalog_import_gate;PCR-GATE-129;PCR-129;policy;security;database_lifecycle;default_policy_pack;catalog_materialization;mga_catalog_commit;fail_closed"
)

add_executable(public_policy_mutation_boundary_gate
  public_policy_mutation_boundary_gate.cpp
)
target_compile_features(public_policy_mutation_boundary_gate PRIVATE cxx_std_23)
target_compile_definitions(public_policy_mutation_boundary_gate PRIVATE
  SB_DEFAULT_POLICY_PACK_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../resources/policy-packs/default-local-password"
)
target_link_libraries(public_policy_mutation_boundary_gate PRIVATE
  sb_engine_internal_api
  sb_storage_database
  sb_core_memory
  sb_core_uuid
)
add_test(
  NAME public_policy_mutation_boundary_gate
  COMMAND public_policy_mutation_boundary_gate
)
set_tests_properties(public_policy_mutation_boundary_gate PROPERTIES
  LABELS "public_release_correctness;public_policy_mutation_boundary_gate;PCR-GATE-131;PCR-131;policy;security;database_lifecycle;catalog_mutation;mga_catalog_commit;audit;generation_invalidation;fail_closed"
)

add_executable(public_custom_policy_pack_gate
  public_custom_policy_pack_gate.cpp
)
target_compile_features(public_custom_policy_pack_gate PRIVATE cxx_std_23)
target_compile_definitions(public_custom_policy_pack_gate PRIVATE
  SB_DEFAULT_POLICY_PACK_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../resources/policy-packs/default-local-password"
)
find_package(OpenSSL REQUIRED)
target_link_libraries(public_custom_policy_pack_gate PRIVATE
  sb_storage_database
  sb_engine_internal_api
  sb_core_memory
  sb_core_uuid
  OpenSSL::Crypto
)
add_test(
  NAME public_custom_policy_pack_gate
  COMMAND public_custom_policy_pack_gate
)
set_tests_properties(public_custom_policy_pack_gate PROPERTIES
  LABELS "public_release_correctness;public_custom_policy_pack_gate;PCR-GATE-132;PCR-132;policy;security;database_lifecycle;custom_policy_pack;provenance;fail_closed"
)

# PUBLIC_ENTERPRISE_THREAT_GATE
add_test(
  NAME public_enterprise_threat_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_enterprise_threat_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --csv-output ${CMAKE_CURRENT_BINARY_DIR}/public_enterprise_threat_gate/public_enterprise_threat_matrix.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_enterprise_threat_gate/public_enterprise_threat_gate.json
)
set_tests_properties(public_enterprise_threat_gate PROPERTIES
  LABELS "public_release_correctness;public_enterprise_threat_gate;PCR-GATE-134;PCR-134;ELER-103;ELER-GATE-103;security;enterprise;threat_model;abuse_case;fail_closed;stable_diagnostics"
)

add_executable(public_upgrade_migration_gate
  public_upgrade_migration_gate.cpp
)
target_compile_features(public_upgrade_migration_gate PRIVATE cxx_std_23)
target_link_libraries(public_upgrade_migration_gate PRIVATE
  sb_storage_database
  sb_storage_page
  sb_storage_disk
  sb_core_catalog
  sb_core_datatypes
  sb_core_index
  sb_core_uuid
)
add_test(
  NAME public_upgrade_migration_gate
  COMMAND public_upgrade_migration_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_upgrade_migration_gate_artifacts/public_upgrade_migration_matrix.csv
)
set_tests_properties(public_upgrade_migration_gate PROPERTIES
  LABELS "public_release_correctness;public_upgrade_migration_gate;PCR-GATE-136;PCR-136;ELER-103;ELER-GATE-103;ELER-106;ELER-GATE-106;upgrade;migration;rollback;downgrade_refusal;format_version_policy;odf;catalog_page;index;datatype;policy_pack;cluster_catalog;stable_diagnostics;fail_closed;downgrade_attack;catalog_corruption"
)

add_executable(public_observability_schema_gate
  public_observability_schema_gate.cpp
)
target_compile_features(public_observability_schema_gate PRIVATE cxx_std_23)
target_link_libraries(public_observability_schema_gate PRIVATE
  sb_engine_internal_api
  sb_core_metrics
  sb_core_uuid
)
add_test(
  NAME public_observability_schema_gate
  COMMAND public_observability_schema_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_observability_schema_gate_artifacts/public_observability_schema_matrix.csv
)
set_tests_properties(public_observability_schema_gate PROPERTIES
  LABELS "public_release_correctness;public_observability_schema_gate;PCR-GATE-137;PCR-137;observability;metrics;events;traces;health;redaction;stable_schema;memory;storage;transactions;index;optimizer;agents;backup;archive;security;repair;cluster_boundary;support_bundle;fail_closed"
)

# PUBLIC_PERFORMANCE_BASELINE_GATE
add_test(
  NAME public_performance_baseline_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_capacity_baseline_report.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --baseline ${CMAKE_CURRENT_SOURCE_DIR}/../performance/public_performance_baselines.json
          --csv-output ${CMAKE_CURRENT_BINARY_DIR}/public_performance_baseline_gate/public_performance_baselines.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_performance_baseline_gate/public_performance_baseline_gate.json
)
set_tests_properties(public_performance_baseline_gate PROPERTIES
  LABELS "public_release_correctness;public_performance_baseline_gate;PCR-GATE-138;PCR-138;performance;capacity;baseline;p50;p95;p99;benchmark_clean;no_donor_dominance;memory;transactions;backup;restore;archive;support_bundle"
)

# PUBLIC_INSTALL_SERVICE_GATE
add_test(
  NAME public_install_service_hardening_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_install_service_hardening_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --csv-output ${CMAKE_CURRENT_BINARY_DIR}/public_install_service_hardening_gate/public_install_service_hardening_matrix.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_install_service_hardening_gate/public_install_service_hardening_gate.json
)
set_tests_properties(public_install_service_hardening_gate PROPERTIES
  LABELS "public_release_correctness;public_install_service_hardening_gate;PCR-GATE-139;PCR-139;ELER-108;ELER-GATE-108;install;uninstall;service;service_identity;permissions;startup;shutdown;secure_defaults;support_bundle;no_world_writable;fail_closed;release_artifact_trust;linux_proof"
)

# PUBLIC_RUNBOOK_GATE
add_test(
  NAME public_admin_runbook_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_runbook_consistency_check.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --csv-output ${CMAKE_CURRENT_BINARY_DIR}/public_admin_runbook_gate/public_admin_runbook_matrix.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_admin_runbook_gate/public_admin_runbook_gate.json
)
set_tests_properties(public_admin_runbook_gate PROPERTIES
  LABELS "public_release_correctness;public_admin_runbook_gate;PCR-GATE-140;PCR-140;ELER-108;ELER-GATE-108;ELER-109;ELER-GATE-109;runbooks;admin_docs;operations;create;open;config;security;backup;restore;verify;repair;memory_pressure;sweep;archive;diagnostics;unsupported_features;upgrade;release_artifact_trust;enterprise_documentation;linux_proof"
)

# PUBLIC_API_ABI_COMPAT_GATE
add_test(
  NAME public_api_abi_compat_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_compatibility_policy_check.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --csv-output ${CMAKE_CURRENT_BINARY_DIR}/public_api_abi_compat_gate/public_api_abi_compat_matrix.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_api_abi_compat_gate/public_api_abi_compat_gate.json
)
set_tests_properties(public_api_abi_compat_gate PROPERTIES
  LABELS "public_release_correctness;public_api_abi_compat_gate;PCR-GATE-141;PCR-141;ELER-108;ELER-GATE-108;ELER-109;ELER-GATE-109;ELER-111;ELER-GATE-111;api;abi;compatibility;semantic_versioning;deprecation;removal;headers;c_api;provider_abi;file_format;diagnostics;config;policy_pack;extension_points;upgrade;fail_closed;release_artifact_trust;enterprise_documentation;support_policy;linux_proof"
)

# PUBLIC_SUPPORT_POLICY_GATE
add_test(
  NAME public_support_policy_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_support_policy_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --csv-output ${CMAKE_CURRENT_BINARY_DIR}/public_support_policy_gate/public_support_policy_matrix.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_support_policy_gate/public_support_policy_gate.json
)
set_tests_properties(public_support_policy_gate PROPERTIES
  LABELS "public_release_correctness;public_support_policy_gate;ELER-111;ELER-GATE-111;support_policy;support_lifecycle;security_update;cve;disclosure;patch_release;compatibility;data_loss_escalation;rollback;diagnostics;sla;community;commercial;linux_proof"
)

# PUBLIC_SUPPORT_BUNDLE_INCIDENT_GATE
add_test(
  NAME public_support_bundle_incident_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_support_bundle_incident_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --csv-output ${CMAKE_CURRENT_BINARY_DIR}/public_support_bundle_incident_gate/public_support_bundle_incident_matrix.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_support_bundle_incident_gate/public_support_bundle_incident_gate.json
)
set_tests_properties(public_support_bundle_incident_gate PROPERTIES
  LABELS "public_release_correctness;public_support_bundle_incident_gate;PCR-GATE-142;PCR-142;ELER-111;ELER-GATE-111;support_bundle;incident;redaction;triage;diagnostics;memory;storage;transactions;index;security;agents;repair;archive;backup;authority_boundary;fail_closed;support_policy;linux_proof"
)

# PUBLIC_DISASTER_RECOVERY_GATE
add_test(
  NAME public_disaster_recovery_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_disaster_recovery_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --csv-output ${CMAKE_CURRENT_BINARY_DIR}/public_disaster_recovery_gate/public_disaster_recovery_matrix.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_disaster_recovery_gate/public_disaster_recovery_gate.json
)
set_tests_properties(public_disaster_recovery_gate PROPERTIES
  LABELS "public_release_correctness;public_disaster_recovery_gate;PCR-GATE-143;PCR-143;ELER-111;ELER-GATE-111;backup;restore;disaster_recovery;interrupted_restore;corrupt_backup;missing_segment;pitr;write_after_replay;identity;security;authority_boundary;fail_closed;support_policy;linux_proof"
)

# PUBLIC_AUDIT_PRIVACY_GATE
add_test(
  NAME public_audit_privacy_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_audit_privacy_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --csv-output ${CMAKE_CURRENT_BINARY_DIR}/public_audit_privacy_gate/public_audit_privacy_matrix.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_audit_privacy_gate/public_audit_privacy_gate.json
)
set_tests_properties(public_audit_privacy_gate PROPERTIES
  LABELS "public_release_correctness;public_audit_privacy_gate;PCR-GATE-144;PCR-144;ELER-111;ELER-GATE-111;audit;retention;integrity;privacy;legal_hold;redaction;tamper_evidence;export;deletion_blocker;protected_fields;mga;non_authority;fail_closed;support_policy;linux_proof"
)

# PUBLIC_WORKLOAD_FAIRNESS_GATE
add_test(
  NAME public_workload_fairness_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_workload_fairness_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --csv-output ${CMAKE_CURRENT_BINARY_DIR}/public_workload_fairness_gate/public_workload_fairness_matrix.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_workload_fairness_gate/public_workload_fairness_gate.json
)
set_tests_properties(public_workload_fairness_gate PROPERTIES
  LABELS "public_release_correctness;public_workload_fairness_gate;PCR-GATE-145;PCR-145;workload;governance;fairness;quota;admission;cancellation;spill;agent_throttling;noisy_neighbor;starvation;memory_pressure;fail_closed"
)

# PUBLIC_RELEASE_ATTESTATION_GATE
add_test(
  NAME public_release_attestation_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_release_attestation_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --build-root ${CMAKE_BINARY_DIR}
          --csv-output ${CMAKE_CURRENT_BINARY_DIR}/public_release_attestation_gate/public_release_attestation_matrix.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_release_attestation_gate/public_release_attestation_gate.json
)
set_tests_properties(public_release_attestation_gate PROPERTIES
  LABELS "public_release_correctness;public_release_attestation_gate;PCR-GATE-146;PCR-146;ELER-108;ELER-GATE-108;release_hygiene;provenance;attestation;source_provenance;generated_provenance;toolchain;reproducible_archive;signature_ready;public_inputs;fail_closed;release_artifact_trust;linux_proof"
)

# PUBLIC_CONFIG_POLICY_MIGRATION_GATE
add_test(
  NAME public_config_policy_migration_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_config_policy_migration_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --csv-output ${CMAKE_CURRENT_BINARY_DIR}/public_config_policy_migration_gate/public_config_policy_migration_matrix.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_config_policy_migration_gate/public_config_policy_migration_gate.json
)
set_tests_properties(public_config_policy_migration_gate PROPERTIES
  LABELS "public_release_correctness;public_config_policy_migration_gate;PCR-GATE-147;PCR-147;config;policy;migration;policy_pack;versioning;diff;export;import;post_create_filesystem_refusal;admin_diagnostics;mga_catalog_commit;fail_closed"
)

# PUBLIC_LIFECYCLE_POLICY_GATE
add_test(
  NAME public_lifecycle_policy_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_lifecycle_policy_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --csv-output ${CMAKE_CURRENT_BINARY_DIR}/public_lifecycle_policy_gate/public_lifecycle_policy_matrix.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_lifecycle_policy_gate/public_lifecycle_policy_gate.json
)
set_tests_properties(public_lifecycle_policy_gate PROPERTIES
  LABELS "public_release_correctness;public_lifecycle_policy_gate;PCR-GATE-152;PCR-152;support;release_lifecycle;supported_platforms;file_format;upgrade;security_patch_window;preview_status;unsupported_features;first_release_boundaries;fail_closed"
)

# PUBLIC_SECRET_SCAN_GATE
add_test(
  NAME public_secret_scan_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_secret_credential_scan.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --csv-output ${CMAKE_CURRENT_BINARY_DIR}/public_secret_scan_gate/public_secret_scan_matrix.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_secret_scan_gate/public_secret_scan_gate.json
)
set_tests_properties(public_secret_scan_gate PROPERTIES
  LABELS "public_release_correctness;public_secret_scan_gate;PCR-GATE-153;PCR-153;secrets;credentials;local_paths;private_endpoints;private_keys;tokens;passwords;signing_keys;cloud_credentials;public_package;fail_closed"
)

# PUBLIC_PLATFORM_ENV_VERIFY_GATE
add_test(
  NAME public_platform_env_verify_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_platform_environment_verify.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --build-root ${CMAKE_BINARY_DIR}
          --csv-output ${CMAKE_CURRENT_BINARY_DIR}/public_platform_env_verify_gate/public_platform_env_verify_matrix.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_platform_env_verify_gate/public_platform_env_verify_gate.json
)
set_tests_properties(public_platform_env_verify_gate PROPERTIES
  LABELS "public_release_correctness;public_platform_env_verify_gate;PCR-GATE-156;PCR-156;platform;environment;compiler;cmake;ninja;python;llvm;openssl;icu;libxml2;zlib;lz4;zstd;geos;proj;gtest;odbc;fail_closed"
)

# PUBLIC_PROVENANCE_POLICY_GATE
add_test(
  NAME public_generated_ai_donor_provenance_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_generated_ai_donor_provenance_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --csv-output ${CMAKE_CURRENT_BINARY_DIR}/public_generated_ai_donor_provenance_gate/public_generated_ai_donor_provenance_matrix.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_generated_ai_donor_provenance_gate/public_generated_ai_donor_provenance_gate.json
)
set_tests_properties(public_generated_ai_donor_provenance_gate PROPERTIES
  LABELS "public_release_correctness;public_generated_ai_donor_provenance_gate;PCR-GATE-154;PCR-154;provenance;generated_code;ai_assisted;donor_derived;copied_reference;public_inputs;human_review;authority_boundary;fail_closed"
)

if(NOT SB_PUBLIC_RELEASE_EXPORT_NESTED)
  add_test(
    NAME public_project_export_gate
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_project_export_gate.py
            --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
            --staging-root ${CMAKE_CURRENT_BINARY_DIR}/public_project_export_gate
            --cmake ${CMAKE_COMMAND}
            --ctest ${CMAKE_CTEST_COMMAND}
            --c-compiler ${CMAKE_C_COMPILER}
            --cxx-compiler ${CMAKE_CXX_COMPILER}
            --parallel 4
  )
  # PUBLIC_RELEASE_CLEANUP_GATE
  set_tests_properties(public_project_export_gate PROPERTIES
    LABELS "public_release_correctness;public_project_export_gate;PCR-GATE-001;PCR-GATE-005;PCR-001;PCR-002;PCR-005;ELER-108;ELER-GATE-108;release_hygiene;project_only_export;cleanup_manifest;private_reference_scan;release_artifact_trust;linux_proof"
    TIMEOUT 2400
  )
  if(_sb_release_nested_configure_env)
    set_tests_properties(public_project_export_gate PROPERTIES
      ENVIRONMENT "${_sb_release_nested_configure_env}"
    )
  endif()
endif()

set(PUBLIC_RELEASE_CORRECTNESS_BUILD_TARGETS
  public_memory_reallocate_gate
  public_default_memory_manager_gate
  public_query_memory_reservation_gate
  public_memory_pressure_executor_gate
  public_query_bump_region_arena_gate
  public_allocator_sharded_overhead_gate
  public_memory_pmr_call_path_gate
  public_disk_file_device_gate
  public_database_ownership_gate
  public_page_size_profile_gate
  public_filespace_header_capacity_gate
  public_disk_resource_bundle_tool
  public_page_type_truth_table_tool
  public_page_body_production_refusal_gate
  public_toast_overflow_binary_descriptor_gate
  public_row_data_slotted_layout_gate
  public_page_body_checksum_agreement_gate
  public_transaction_mga_cow_gate
  public_transaction_inventory_lock_table_gate
  public_transaction_savepoint_limbo_cleanup_gate
  public_transaction_support_bundle_gate
  public_mga_audit_transaction_location_gate
  public_mga_historical_snapshot_locator_gate
  public_repair_event_ledger_quarantine_gate
  public_repair_history_inspection_api_gate
  public_repair_identity_rules_gate
  public_repair_tamper_retention_crash_resume_gate
  public_mga_physical_sweep_gate
  public_archive_before_reclaim_gate
  public_backup_forward_session_gate
  public_multi_horizon_disposal_gate
  public_backup_update_coverage_gate
  public_authorization_durable_flow_gate
  public_security_durable_crypto_hardening_gate
  public_security_provider_contract_protected_material_gate
  public_datatype_truth_matrix
  public_domain_psql_exact_numeric_gate
  public_datatype_binary_descriptor_integrity_gate
  public_advanced_datatype_family_gate
  public_sblr_uuid_mga_route_integration_gate
  public_uuid_identity_resolution_gate
  public_agent_readiness_matrix_gate
  public_agent_durable_runtime_catalog_gate
  public_agent_action_dispatch_idempotency_gate
  public_agent_replay_compensation_quarantine_gate
  public_page_allocation_manager_direct_preallocation_gate
  public_agent_operator_explain_cluster_boundary_gate
  public_cluster_catalog_manifest_gate
  public_cluster_schema_projection_gate
  public_cluster_catalog_codec_resolver_gate
  public_cluster_descriptor_manifest_gate
  public_cluster_provider_boundary_cleanup_gate
  public_cluster_catalog_readiness_suite
  public_cluster_access_profile_gate
  public_cluster_schema_version_gate
  public_cluster_provider_handshake_gate
  public_cluster_catalog_backup_export_gate
  public_cluster_readable_view_gate
  public_cluster_projection_redaction_gate
  public_cluster_projection_authority_guard_gate
  public_cluster_catalog_crypto_evidence_gate
  public_codec_property_gate
  public_example_database_seed
  public_sanitizer_warning_probe
  public_release_version_metadata_tool
  public_secure_defaults_gate
)

set(PUBLIC_RELEASE_ARTIFACT_TRUST_BUILD_TARGETS
  public_cluster_provider_boundary_cleanup_gate
  public_example_database_seed
  public_sanitizer_warning_probe
  public_release_version_metadata_tool
  public_secure_defaults_gate
)

add_custom_target(public_release_artifact_trust_build_prereqs
  DEPENDS ${PUBLIC_RELEASE_ARTIFACT_TRUST_BUILD_TARGETS}
)

add_custom_target(public_release_correctness_build_prereqs
  DEPENDS ${PUBLIC_RELEASE_CORRECTNESS_BUILD_TARGETS}
)

add_custom_target(public_release_correctness_gate_check
  DEPENDS public_release_correctness_build_prereqs
  COMMAND ${CMAKE_CTEST_COMMAND}
          --test-dir ${CMAKE_BINARY_DIR}
          -L public_release_correctness
          --output-on-failure
)
