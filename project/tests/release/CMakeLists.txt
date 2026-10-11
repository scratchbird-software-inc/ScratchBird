# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_test(NAME public_runtime_stage_conformance
  COMMAND ${Python3_EXECUTABLE} -B
    "${CMAKE_CURRENT_SOURCE_DIR}/public_runtime_stage_conformance.py"
    --cmake "${CMAKE_COMMAND}"
    --module "${PROJECT_SOURCE_DIR}/cmake/PublicBranding.cmake")
set_tests_properties(public_runtime_stage_conformance PROPERTIES TIMEOUT 60)

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

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(crypto_memory_adapter_gate crypto_memory_adapter_gate.cpp)
  target_compile_features(crypto_memory_adapter_gate PRIVATE cxx_std_20)
  target_link_libraries(crypto_memory_adapter_gate PRIVATE sb_core_hash sb_core_memory)
  target_link_options(crypto_memory_adapter_gate PRIVATE
    "LINKER:--wrap=pthread_mutex_lock"
    "LINKER:--wrap=EVP_MD_CTX_new" "LINKER:--wrap=EVP_DigestInit_ex"
    "LINKER:--wrap=EVP_DigestUpdate" "LINKER:--wrap=EVP_DigestFinal_ex")
  foreach(mode bounded late custom)
    add_test(NAME crypto_memory_adapter_${mode}_gate COMMAND crypto_memory_adapter_gate ${mode})
    set_tests_properties(crypto_memory_adapter_${mode}_gate PROPERTIES
      TIMEOUT 60 LABELS "storage;crypto;actual_memory;binary_uuid;not_runtime_acceptance")
  endforeach()
endif()

option(SB_PUBLIC_RELEASE_EXPORT_NESTED
  "Suppress recursive public export CTest registration while proving a staged public tree"
  OFF)
option(SB_PUBLIC_RELEASE_HARDENING_NESTED
  "Suppress recursive public hardening CTest registration while proving sanitizer/static-analysis profiles"
  OFF)
set(SB_PUBLIC_RELEASE_HARDENING_C_COMPILER "${CMAKE_C_COMPILER}" CACHE FILEPATH
  "C compiler for all required nested sanitizer profiles (defaults to the main compiler)")
set(SB_PUBLIC_RELEASE_HARDENING_CXX_COMPILER "${CMAKE_CXX_COMPILER}" CACHE FILEPATH
  "C++ compiler for all required nested sanitizer profiles (defaults to the main compiler)")
set(SB_PUBLIC_RELEASE_EXPORT_BUILD_PARALLELISM "4" CACHE STRING
  "Maximum parallel compile jobs used by the clean public-project export proof")
if(NOT SB_PUBLIC_RELEASE_EXPORT_BUILD_PARALLELISM MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR
    "SB_PUBLIC_RELEASE_EXPORT_BUILD_PARALLELISM must be a positive integer")
endif()

set(_sb_release_nested_configure_env)
foreach(_sb_forwarded_var
        SB_LLVM_PROJECT_ROOT
        SB_LLVM_TOOLS_ROOT
        SB_LLVM_LIBRARY
        SB_LLVM_RUNTIME_LIBRARY
        SB_LLVM_LINK_MODE
        SB_LLVM_MIN_MAJOR)
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
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/storage/filespace
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
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/storage/filespace
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

foreach(history_target sbsql_sblr_alignment_native_allocation_map sbsql_sblr_alignment_native_filespace_directory)
  if(TARGET ${history_target} AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(${history_target} PRIVATE NATIVE_HISTORICAL_IO_FAULTS=1)
    target_link_options(${history_target} PRIVATE -Wl,--wrap=fstat -Wl,--wrap=pwrite -Wl,--wrap=fsync)
  endif()
endforeach()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(native_page_zero_growth_body_gate native_page_zero_growth_body_gate.cpp)
  target_compile_features(native_page_zero_growth_body_gate PRIVATE cxx_std_23)
  target_link_libraries(native_page_zero_growth_body_gate PRIVATE sb_storage_disk)
  target_link_options(native_page_zero_growth_body_gate PRIVATE
    -Wl,--wrap=pread -Wl,--wrap=pwrite -Wl,--wrap=fsync -Wl,--wrap=fstat -Wl,--wrap=EVP_MD_CTX_new -Wl,--wrap=EVP_Digest)
  add_test(NAME native_page_zero_growth_body_gate COMMAND native_page_zero_growth_body_gate)
  set_tests_properties(native_page_zero_growth_body_gate PROPERTIES TIMEOUT 120
    LABELS "storage;binary_uuid;real_files;partial_effects;not_runtime_acceptance")
endif()
if(TARGET sbsql_sblr_alignment_native_storage_components)
  add_test(NAME native_btree_memory_shared_regression_gate
    COMMAND sbsql_sblr_alignment_native_storage_components --native-btree-memory-only)
  set_tests_properties(native_btree_memory_shared_regression_gate PROPERTIES
    TIMEOUT 600 LABELS "storage;memory;binary_uuid;real_files;fault_injection;not_runtime_acceptance")
endif()
if(TARGET sbsql_sblr_alignment_native_management_control_bundle)
  # New exhaustive encoding faults are independently scheduled by profile.
  # Preserve existing aggregate codec/read gates and their original deadlines.
  foreach(primary RANGE 0 4)
    add_test(NAME native_bundle_encoding_legacy_${primary}_gate
      COMMAND sbsql_sblr_alignment_native_management_control_bundle --bundle-encoding ${primary})
    set_tests_properties(native_bundle_encoding_legacy_${primary}_gate PROPERTIES
      LABELS "storage;binary_uuid;memory;bundle_encoding;fault_injection;real_files;not_publication_acceptance" TIMEOUT 120)
    foreach(secondary RANGE 0 4)
      add_test(NAME native_bundle_encoding_${primary}_${secondary}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --bundle-encoding ${primary} ${secondary})
      set_tests_properties(native_bundle_encoding_${primary}_${secondary}_gate PROPERTIES
        LABELS "storage;binary_uuid;memory;bundle_encoding;mixed_page_profiles;fault_injection;real_files;not_publication_acceptance" TIMEOUT 120)
    endforeach()
  endforeach()
  # Consume the unchanged 01f issuer; storage owns this integration fixture.
  target_sources(sbsql_sblr_alignment_native_management_control_bundle PRIVATE
    "${PROJECT_SOURCE_DIR}/src/server/database_ownership.cpp")
  target_compile_definitions(sbsql_sblr_alignment_native_management_control_bundle PRIVATE SB_NATIVE_ROUTE_SOURCE_TESTS=1)
  foreach(primary RANGE 0 4)
    foreach(secondary RANGE 0 4)
      add_test(NAME native_reservation_source_${primary}_${secondary}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --reservation-source ${primary} ${secondary})
      set_tests_properties(native_reservation_source_${primary}_${secondary}_gate PROPERTIES
        TIMEOUT 120 LABELS "storage;memory;binary_uuid;real_files;allocation_map;fault_injection;not_allocation_acceptance")
    endforeach()
  endforeach()
  foreach(scenario RANGE 0 3)
    foreach(primary RANGE 0 4)
      foreach(secondary RANGE 0 4)
        add_test(NAME native_allocation_lease_${scenario}_${primary}_${secondary}_gate
          COMMAND sbsql_sblr_alignment_native_management_control_bundle --guarded-allocation ${scenario} ${primary} ${secondary})
        set_tests_properties(native_allocation_lease_${scenario}_${primary}_${secondary}_gate PROPERTIES
          TIMEOUT 600 LABELS "storage;memory;binary_uuid;real_files;allocation_map;fault_injection;not_runtime_acceptance")
      endforeach()
    endforeach()
  endforeach()
  foreach(indexed RANGE 0 1)
    foreach(primary RANGE 0 4)
      foreach(secondary RANGE 0 4)
        add_test(NAME native_catalog_relation_lease_${indexed}_${primary}_${secondary}_gate
          COMMAND sbsql_sblr_alignment_native_management_control_bundle --guarded-catalog-relation ${indexed} ${primary} ${secondary})
        set_tests_properties(native_catalog_relation_lease_${indexed}_${primary}_${secondary}_gate PROPERTIES
          TIMEOUT 600 LABELS "storage;memory;binary_uuid;real_files;catalog_metadata;fault_injection;not_runtime_acceptance")
        add_test(NAME native_catalog_visibility_lease_${indexed}_${primary}_${secondary}_gate
          COMMAND sbsql_sblr_alignment_native_management_control_bundle --guarded-catalog-visibility ${indexed} ${primary} ${secondary})
        set_tests_properties(native_catalog_visibility_lease_${indexed}_${primary}_${secondary}_gate PROPERTIES
          TIMEOUT 600 LABELS "storage;memory;binary_uuid;real_files;catalog_metadata;mga_visibility;fault_injection;not_runtime_acceptance")
      endforeach()
    endforeach()
  endforeach()
  foreach(primary RANGE 0 4)
    add_test(NAME native_guarded_snapshot_observation_${primary}_gate
      COMMAND sbsql_sblr_alignment_native_management_control_bundle --guarded-snapshot-observation ${primary})
    set_tests_properties(native_guarded_snapshot_observation_${primary}_gate PROPERTIES
      TIMEOUT 600 LABELS "storage;memory;binary_uuid;real_files;snapshot;revocation;not_runtime_acceptance")
    add_test(NAME native_guarded_visibility_observation_${primary}_gate
      COMMAND sbsql_sblr_alignment_native_management_control_bundle --guarded-visibility-observation ${primary})
    set_tests_properties(native_guarded_visibility_observation_${primary}_gate PROPERTIES
      TIMEOUT 600 LABELS "storage;memory;binary_uuid;real_files;mga_visibility;not_runtime_acceptance")
    math(EXPR secondary "(${primary}+1)%5")
    foreach(scenario RANGE 7 9)
      if(scenario EQUAL 9)
        set(relation_indexed 0)
      else()
        set(relation_indexed 1)
      endif()
      add_test(NAME native_catalog_relation_lease_outcome_${primary}_${scenario}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --guarded-catalog-relation ${relation_indexed} ${primary} ${secondary} ${scenario})
      set_tests_properties(native_catalog_relation_lease_outcome_${primary}_${scenario}_gate PROPERTIES
        TIMEOUT 600 LABELS "storage;memory;binary_uuid;real_files;catalog_metadata;fault_injection;not_runtime_acceptance")
      add_test(NAME native_catalog_visibility_lease_outcome_${primary}_${scenario}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --guarded-catalog-visibility ${relation_indexed} ${primary} ${secondary} ${scenario})
      set_tests_properties(native_catalog_visibility_lease_outcome_${primary}_${scenario}_gate PROPERTIES
        TIMEOUT 600 LABELS "storage;memory;binary_uuid;real_files;catalog_metadata;mga_visibility;fault_injection;not_runtime_acceptance")
    endforeach()
    foreach(chain RANGE 10 24)
      add_test(NAME native_catalog_visibility_chain_${primary}_${chain}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --guarded-catalog-visibility 1 ${primary} ${secondary} ${chain})
      set_tests_properties(native_catalog_visibility_chain_${primary}_${chain}_gate PROPERTIES
        TIMEOUT 600 LABELS "storage;memory;binary_uuid;real_files;catalog_metadata;mga_visibility;version_chain;not_runtime_acceptance")
    endforeach()
    foreach(fault RANGE 1 6)
      add_test(NAME native_catalog_relation_lease_refusal_${primary}_${fault}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --guarded-catalog-relation 1 ${primary} ${secondary} ${fault})
      set_tests_properties(native_catalog_relation_lease_refusal_${primary}_${fault}_gate PROPERTIES
        TIMEOUT 600 LABELS "storage;memory;binary_uuid;real_files;catalog_metadata;fault_injection;not_runtime_acceptance")
      add_test(NAME native_catalog_visibility_lease_refusal_${primary}_${fault}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --guarded-catalog-visibility 1 ${primary} ${secondary} ${fault})
      set_tests_properties(native_catalog_visibility_lease_refusal_${primary}_${fault}_gate PROPERTIES
        TIMEOUT 600 LABELS "storage;memory;binary_uuid;real_files;catalog_metadata;mga_visibility;fault_injection;not_runtime_acceptance")
    endforeach()
  endforeach()
  foreach(primary RANGE 0 4)
    foreach(secondary RANGE 0 4)
      add_test(NAME native_guarded_btree_memory_${primary}_${secondary}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --guarded-btree-memory ${primary} ${secondary})
      set_tests_properties(native_guarded_btree_memory_${primary}_${secondary}_gate PROPERTIES
        TIMEOUT 600 LABELS "storage;memory;binary_uuid;real_files;catalog_metadata;fault_injection;not_runtime_acceptance")
    endforeach()
  endforeach()
  foreach(profile RANGE 0 4)
    math(EXPR secondary "(${profile}+1)%5")
    foreach(fault RANGE 1 4)
      add_test(NAME native_catalog_roots_creator_refusal_${profile}_${fault}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --catalog-distinct-roots-memory 0 ${profile} ${secondary} ${fault})
      set_tests_properties(native_catalog_roots_creator_refusal_${profile}_${fault}_gate PROPERTIES
        TIMEOUT 600 LABELS "storage;memory;binary_uuid;real_files;catalog_metadata;fault_injection;not_runtime_acceptance")
    endforeach()
  endforeach()
  foreach(via_route RANGE 0 1)
    foreach(primary RANGE 0 4)
      foreach(secondary RANGE 0 4)
        add_test(NAME native_catalog_distinct_roots_memory_${via_route}_${primary}_${secondary}_gate
          COMMAND sbsql_sblr_alignment_native_management_control_bundle --catalog-distinct-roots-memory ${via_route} ${primary} ${secondary})
        set_tests_properties(native_catalog_distinct_roots_memory_${via_route}_${primary}_${secondary}_gate PROPERTIES
          TIMEOUT 600 LABELS "storage;memory;binary_uuid;real_files;catalog_metadata;owned_device_lifetime;not_runtime_acceptance")
      endforeach()
    endforeach()
  endforeach()
  foreach(via_route RANGE 0 1)
    foreach(profile RANGE 0 4)
      add_test(NAME native_catalog_populated_owned_memory_${via_route}_${profile}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --catalog-populated-owned-source-memory ${via_route} ${profile})
      set_tests_properties(native_catalog_populated_owned_memory_${via_route}_${profile}_gate PROPERTIES
        TIMEOUT 600 LABELS "storage;memory;binary_uuid;real_files;catalog_metadata;owned_device_lifetime;not_runtime_acceptance")
    endforeach()
  endforeach()
  foreach(via_route RANGE 0 1)
    foreach(initial RANGE 0 1)
      foreach(profile RANGE 2 4)
        foreach(primary RANGE 0 4)
          foreach(secondary RANGE 0 4)
            add_test(NAME native_owned_source_memory_${via_route}_${initial}_${profile}_${primary}_${secondary}_gate
              COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-owned-source-memory
                ${via_route} ${initial} ${profile} ${primary} ${secondary})
            set_tests_properties(native_owned_source_memory_${via_route}_${initial}_${profile}_${primary}_${secondary}_gate
              PROPERTIES TIMEOUT 600 LABELS "storage;memory;binary_uuid;real_files;owned_device_lifetime;not_runtime_acceptance")
          endforeach()
        endforeach()
        foreach(route RANGE 0 8)
          set(last_shard 0)
          if(route LESS_EQUAL 6)
            set(last_shard 15)
          endif()
          foreach(shard RANGE 0 ${last_shard})
            add_test(NAME native_owned_source_memory_fault_${via_route}_${initial}_${profile}_${route}_${shard}_gate
              COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-owned-source-faults
                ${via_route} ${initial} ${profile} ${route} ${shard})
            set_tests_properties(native_owned_source_memory_fault_${via_route}_${initial}_${profile}_${route}_${shard}_gate
              PROPERTIES TIMEOUT 600 LABELS "storage;memory;binary_uuid;real_files;fault_injection;owned_device_lifetime;not_runtime_acceptance")
          endforeach()
        endforeach()
      endforeach()
    endforeach()
  endforeach()
  foreach(route RANGE 0 2)
    if(route EQUAL 2)
      set(owned_directory_last 31)
    elseif(route EQUAL 1)
      set(owned_directory_last 4)
    else()
      set(owned_directory_last 0)
    endif()
    foreach(shard RANGE 0 ${owned_directory_last})
      add_test(NAME native_directory_owned_faults_${route}_${shard}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --owned-directory-faults ${route} ${shard})
      set_tests_properties(native_directory_owned_faults_${route}_${shard}_gate PROPERTIES TIMEOUT 120
        LABELS "storage;binary_uuid;real_files;owned_directory_assembly;fault_injection;not_runtime_acceptance")
    endforeach()
  endforeach()
  foreach(route RANGE 0 2)
    if(route EQUAL 0)
      set(last_shard 0)
    elseif(route EQUAL 1)
      set(last_shard 19)
    else()
      set(last_shard 31)
    endif()
    foreach(shard RANGE 0 ${last_shard})
      add_test(NAME native_directory_growth_selection_faults_${route}_${shard}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-growth-selection-faults ${route} ${shard})
      set_tests_properties(native_directory_growth_selection_faults_${route}_${shard}_gate PROPERTIES TIMEOUT 300
        LABELS "storage;binary_uuid;real_files;growth_selector_recovery;fault_injection;not_runtime_acceptance")
    endforeach()
  endforeach()
  foreach(primary RANGE 0 4)
    add_test(NAME native_directory_owned_secondary_preallocation_${primary}_gate
      COMMAND sbsql_sblr_alignment_native_management_control_bundle --owned-secondary-preallocation ${primary})
    set_tests_properties(native_directory_owned_secondary_preallocation_${primary}_gate PROPERTIES TIMEOUT 120
      LABELS "storage;binary_uuid;real_files;secondary_preallocation;not_runtime_acceptance")
    add_test(NAME native_directory_owned_secondary_subdivision_${primary}_gate
      COMMAND sbsql_sblr_alignment_native_management_control_bundle --owned-secondary-subdivision ${primary})
    set_tests_properties(native_directory_owned_secondary_subdivision_${primary}_gate PROPERTIES TIMEOUT 120
      LABELS "storage;binary_uuid;real_files;secondary_preallocation;map_subdivision;not_runtime_acceptance")
    foreach(secondary RANGE 0 4)
      add_test(NAME native_directory_owned_append_admission_${primary}_${secondary}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --owned-append-admission ${primary} ${secondary})
      set_tests_properties(native_directory_owned_append_admission_${primary}_${secondary}_gate PROPERTIES TIMEOUT 300
        LABELS "storage;binary_uuid;real_files;history_append_admission;not_runtime_acceptance")
      foreach(mode growth growth-subdivision growth-admission growth-close growth-physical-recovery growth-recovery-refusals growth-crashes)
        add_test(NAME native_directory_owned_${mode}_${primary}_${secondary}_gate
          COMMAND sbsql_sblr_alignment_native_management_control_bundle --owned-${mode} ${primary} ${secondary})
        set_tests_properties(native_directory_owned_${mode}_${primary}_${secondary}_gate PROPERTIES TIMEOUT 600
          LABELS "storage;binary_uuid;real_files;owned_growth;original_operation_recovery;not_runtime_acceptance")
      endforeach()
      add_test(NAME native_directory_growth_staging_crashes_${primary}_${secondary}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-growth-staging-crashes ${primary} ${secondary})
      set_tests_properties(native_directory_growth_staging_crashes_${primary}_${secondary}_gate PROPERTIES TIMEOUT 600
        LABELS "storage;binary_uuid;real_files;growth_staging;crash_reopen;not_runtime_acceptance")
      foreach(mode selection selection-crashes)
        add_test(NAME native_directory_growth_${mode}_${primary}_${secondary}_gate
          COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-growth-${mode} ${primary} ${secondary})
        set_tests_properties(native_directory_growth_${mode}_${primary}_${secondary}_gate PROPERTIES TIMEOUT 300
          LABELS "storage;binary_uuid;real_files;growth_selector_recovery;not_runtime_acceptance")
      endforeach()
      add_test(NAME native_directory_owned_secondary_close_${primary}_${secondary}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --owned-secondary-close ${primary} ${secondary})
      set_tests_properties(native_directory_owned_secondary_close_${primary}_${secondary}_gate PROPERTIES TIMEOUT 120
        LABELS "storage;binary_uuid;real_files;secondary_preallocation;retained_device_concurrency;not_runtime_acceptance")
      foreach(reverse RANGE 0 1)
        add_test(NAME native_directory_owned_secondary_physical_faults_${primary}_${secondary}_${reverse}_gate
          COMMAND sbsql_sblr_alignment_native_management_control_bundle --owned-secondary-physical-faults ${primary} ${secondary} ${reverse})
        set_tests_properties(native_directory_owned_secondary_physical_faults_${primary}_${secondary}_${reverse}_gate PROPERTIES TIMEOUT 120
          LABELS "storage;binary_uuid;real_files;secondary_preallocation;fault_injection;not_runtime_acceptance")
        add_test(NAME native_directory_owned_secondary_crashes_${primary}_${secondary}_${reverse}_gate
          COMMAND sbsql_sblr_alignment_native_management_control_bundle --owned-secondary-crashes ${primary} ${secondary} ${reverse})
        set_tests_properties(native_directory_owned_secondary_crashes_${primary}_${secondary}_${reverse}_gate PROPERTIES TIMEOUT 300
          LABELS "storage;binary_uuid;real_files;secondary_preallocation;crash_reopen;not_runtime_acceptance")
      endforeach()
    endforeach()
    add_test(NAME native_directory_owned_preallocation_${primary}_gate
      COMMAND sbsql_sblr_alignment_native_management_control_bundle --owned-directory-preallocation ${primary})
    set_tests_properties(native_directory_owned_preallocation_${primary}_gate PROPERTIES TIMEOUT 120
      LABELS "storage;binary_uuid;real_files;owned_directory_assembly;actual_preallocation;not_runtime_acceptance")
    add_test(NAME native_directory_owned_repacking_${primary}_gate
      COMMAND sbsql_sblr_alignment_native_management_control_bundle --owned-directory-repacking ${primary})
    set_tests_properties(native_directory_owned_repacking_${primary}_gate PROPERTIES TIMEOUT 120
      LABELS "storage;binary_uuid;real_files;owned_directory_assembly;not_runtime_acceptance")
    add_test(NAME native_directory_owned_inventory_${primary}_gate
      COMMAND sbsql_sblr_alignment_native_management_control_bundle --owned-directory-inventory ${primary})
    set_tests_properties(native_directory_owned_inventory_${primary}_gate PROPERTIES TIMEOUT 120
      LABELS "storage;binary_uuid;real_files;owned_directory_assembly;not_runtime_acceptance")
  endforeach()
  foreach(route RANGE 0 2)
    if(route EQUAL 0)
      set(append_fault_last 0)
    elseif(route EQUAL 1)
      set(append_fault_last 19)
    else()
      set(append_fault_last 127)
    endif()
    foreach(shard RANGE 0 ${append_fault_last})
      add_test(NAME native_directory_owned_append_faults_${route}_${shard}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --owned-append-faults ${route} ${shard})
      set_tests_properties(native_directory_owned_append_faults_${route}_${shard}_gate PROPERTIES TIMEOUT 600
        LABELS "storage;binary_uuid;real_files;history_append_admission;fault_injection;not_runtime_acceptance")
    endforeach()
  endforeach()
  foreach(route RANGE 0 2)
    if(route EQUAL 0)
      set(growth_recovery_last 0)
    elseif(route EQUAL 1)
      set(growth_recovery_last 19)
    else()
      set(growth_recovery_last 31)
    endif()
    foreach(shard RANGE 0 ${growth_recovery_last})
      add_test(NAME native_directory_owned_growth_recovery_faults_${route}_${shard}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --owned-growth-recovery-faults ${route} ${shard})
      set_tests_properties(native_directory_owned_growth_recovery_faults_${route}_${shard}_gate PROPERTIES TIMEOUT 600
        LABELS "storage;binary_uuid;real_files;owned_growth;original_operation_recovery;fault_injection;not_runtime_acceptance")
    endforeach()
  endforeach()
  foreach(route RANGE 0 5)
    set(secondary_fault_last 0)
    if(route EQUAL 1)
      set(secondary_fault_last 4)
    elseif(route EQUAL 2)
      set(secondary_fault_last 31)
    endif()
    foreach(shard RANGE 0 ${secondary_fault_last})
      add_test(NAME native_directory_owned_secondary_sweep_${route}_${shard}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --owned-secondary-sweeps ${route} ${shard})
      set_tests_properties(native_directory_owned_secondary_sweep_${route}_${shard}_gate PROPERTIES TIMEOUT 180
        LABELS "storage;binary_uuid;real_files;secondary_preallocation;fault_injection;not_runtime_acceptance")
    endforeach()
  endforeach()
  foreach(profile RANGE 2 4)
    if(profile EQUAL 4)
      set(last_install_stage 1)
    else()
      set(last_install_stage 2)
    endif()
    foreach(stage RANGE 0 ${last_install_stage})
      foreach(route RANGE 0 2)
        if(route EQUAL 2)
          set(install_fault_last 15)
        elseif(route EQUAL 1)
          if(stage EQUAL 2)
            set(install_fault_last 19)
          else()
            set(install_fault_last 4)
          endif()
        else()
          set(install_fault_last 0)
        endif()
        foreach(shard RANGE 0 ${install_fault_last})
          add_test(NAME native_directory_install_faults_${profile}_${stage}_${route}_${shard}_gate
            COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-install-faults ${profile} ${stage} ${route} ${shard})
          set_tests_properties(native_directory_install_faults_${profile}_${stage}_${route}_${shard}_gate PROPERTIES TIMEOUT 120
            LABELS "storage;binary_uuid;real_files;actual_control_publication;fault_injection;not_runtime_acceptance")
        endforeach()
      endforeach()
    endforeach()
    foreach(primary RANGE 0 4)
      if(NOT profile EQUAL 4)
        foreach(secondary RANGE 0 4)
          add_test(NAME native_directory_publication_torn_payload_${profile}_${primary}_${secondary}_gate
            COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-publication-torn-payload ${profile} ${primary} ${secondary})
          set_tests_properties(native_directory_publication_torn_payload_${profile}_${primary}_${secondary}_gate PROPERTIES TIMEOUT 120
            LABELS "storage;binary_uuid;real_files;actual_control_publication;crash_reopen;torn_payload;not_runtime_acceptance")
          add_test(NAME native_directory_publication_crashes_${profile}_${primary}_${secondary}_gate
            COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-publication-crashes ${profile} ${primary} ${secondary})
          set_tests_properties(native_directory_publication_crashes_${profile}_${primary}_${secondary}_gate PROPERTIES TIMEOUT 120
            LABELS "storage;binary_uuid;real_files;actual_control_publication;crash_reopen;not_runtime_acceptance")
        endforeach()
      endif()
      add_test(NAME native_directory_install_${profile}_${primary}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-install ${profile} ${primary})
      set_tests_properties(native_directory_install_${profile}_${primary}_gate PROPERTIES TIMEOUT 120
        LABELS "storage;binary_uuid;real_files;actual_control_publication;not_runtime_acceptance")
      if(profile EQUAL 4)
        set_tests_properties(native_directory_install_${profile}_${primary}_gate PROPERTIES TIMEOUT 300)
      endif()
    endforeach()
  endforeach()
  foreach(profile RANGE 0 4)
    add_test(NAME native_directory_control_close_${profile}_gate
      COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-control-close ${profile})
    set_tests_properties(native_directory_control_close_${profile}_gate PROPERTIES TIMEOUT 120
      LABELS "storage;binary_uuid;real_files;actual_control_ancestry;retained_device_concurrency;not_runtime_acceptance")
  endforeach()

  # Full retained source lease, initial and later selections, every ordered
  # page-profile pair. Fault shards retain every actual provider/I/O site.
  # These are conformance sweeps, not latency thresholds: the largest full
  # profile takes over 100s alongside compilation before concurrent repetition.
  foreach(initial RANGE 0 1)
    foreach(profile RANGE 2 4)
      foreach(primary RANGE 0 4)
        foreach(secondary RANGE 0 4)
          add_test(NAME native_selected_lease_memory_${initial}_${profile}_${primary}_${secondary}_gate
            COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-selected-lease-memory
              ${initial} ${profile} ${primary} ${secondary})
          set_tests_properties(native_selected_lease_memory_${initial}_${profile}_${primary}_${secondary}_gate
            PROPERTIES TIMEOUT 600 LABELS "storage;memory;binary_uuid;real_files;retained_device_concurrency;not_runtime_acceptance")
        endforeach()
      endforeach()
      foreach(route RANGE 0 7)
        set(last_shard 0)
        if(route LESS_EQUAL 5)
          set(last_shard 15)
        endif()
        foreach(shard RANGE 0 ${last_shard})
          add_test(NAME native_selected_lease_memory_fault_${initial}_${profile}_${route}_${shard}_gate
            COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-selected-lease-faults
              ${initial} ${profile} ${route} ${shard})
          set_tests_properties(native_selected_lease_memory_fault_${initial}_${profile}_${route}_${shard}_gate
            PROPERTIES TIMEOUT 600 LABELS "storage;memory;binary_uuid;real_files;retained_device_concurrency;not_runtime_acceptance")
        endforeach()
      endforeach()
    endforeach()
  endforeach()
  foreach(profile RANGE 3 4)
    foreach(size RANGE 0 4)
      add_test(NAME native_selected_lease_memory_repeat_${profile}_${size}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-selected-lease-repeat ${profile} ${size})
      set_tests_properties(native_selected_lease_memory_repeat_${profile}_${size}_gate
        PROPERTIES TIMEOUT 600 LABELS "storage;memory;binary_uuid;real_files;retained_device_concurrency;not_runtime_acceptance")
    endforeach()
  endforeach()
  foreach(profile RANGE 2 4)
    foreach(allocation RANGE 0 1)
      foreach(primary RANGE 0 4)
        foreach(secondary RANGE 0 4)
          add_test(NAME native_current_source_memory_${allocation}_${profile}_${primary}_${secondary}_gate
            COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-current-source-memory ${allocation} ${profile} ${primary} ${secondary})
          set_tests_properties(native_current_source_memory_${allocation}_${profile}_${primary}_${secondary}_gate PROPERTIES TIMEOUT 120
            LABELS "storage;binary_uuid;real_files;current_source;memory;not_runtime_acceptance")
        endforeach()
        if(profile GREATER 2)
          add_test(NAME native_current_source_memory_repeat_${allocation}_${profile}_${primary}_gate
            COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-current-source-repeat-memory ${allocation} ${profile} ${primary})
          set_tests_properties(native_current_source_memory_repeat_${allocation}_${profile}_${primary}_gate PROPERTIES TIMEOUT 120
            LABELS "storage;binary_uuid;real_files;current_source;memory;not_runtime_acceptance")
        endif()
      endforeach()
      foreach(route RANGE 0 7)
        if(route LESS 6)
          set(current_source_shard_last 3)
        else()
          set(current_source_shard_last 0)
        endif()
        foreach(shard RANGE 0 ${current_source_shard_last})
          add_test(NAME native_current_source_memory_fault_${allocation}_${profile}_${route}_${shard}_gate
            COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-current-source-faults ${allocation} ${profile} ${route} ${shard})
          set_tests_properties(native_current_source_memory_fault_${allocation}_${profile}_${route}_${shard}_gate PROPERTIES TIMEOUT 120
            LABELS "storage;binary_uuid;real_files;current_source;memory;fault_injection;not_runtime_acceptance")
        endforeach()
      endforeach()
    endforeach()
    if(profile GREATER 2)
      set(selection_repeat_last 1)
    else()
      set(selection_repeat_last 0)
    endif()
    foreach(repeated RANGE 0 ${selection_repeat_last})
      foreach(route RANGE 0 7)
        if(route LESS 6)
          if(repeated)
            set(selection_shard_last 15)
          else()
            set(selection_shard_last 3)
          endif()
        else()
          set(selection_shard_last 0)
        endif()
        foreach(shard RANGE 0 ${selection_shard_last})
          add_test(NAME native_directory_selection_memory_fault_${profile}_${repeated}_${route}_${shard}_gate
            COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-selection-faults ${profile} ${repeated} ${route} ${shard})
          set_tests_properties(native_directory_selection_memory_fault_${profile}_${repeated}_${route}_${shard}_gate PROPERTIES TIMEOUT 120
            LABELS "storage;binary_uuid;real_files;selected_control_ancestry;memory;fault_injection;not_runtime_acceptance")
        endforeach()
      endforeach()
    endforeach()
    foreach(route RANGE 0 2)
      if(route EQUAL 2)
        set(control_fault_last 7)
      elseif(route EQUAL 1)
        set(control_fault_last 4)
      else()
        set(control_fault_last 0)
      endif()
      foreach(shard RANGE 0 ${control_fault_last})
        add_test(NAME native_directory_control_faults_${profile}_${route}_${shard}_gate
          COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-control-faults ${profile} ${route} ${shard})
        set_tests_properties(native_directory_control_faults_${profile}_${route}_${shard}_gate PROPERTIES TIMEOUT 120
          LABELS "storage;binary_uuid;real_files;actual_control_ancestry;fault_injection;not_runtime_acceptance")
      endforeach()
    endforeach()
    if(profile GREATER 2)
      foreach(size RANGE 0 4)
        add_test(NAME native_directory_selection_memory_repeat_${profile}_${size}_gate
          COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-selection-repeat-memory ${profile} ${size})
        set_tests_properties(native_directory_selection_memory_repeat_${profile}_${size}_gate PROPERTIES TIMEOUT 120
          LABELS "storage;binary_uuid;real_files;selected_control_ancestry;memory;not_runtime_acceptance")
        add_test(NAME native_directory_history_memory_repeat_${profile}_${size}_gate
          COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-history-repeat-memory ${profile} ${size})
        set_tests_properties(native_directory_history_memory_repeat_${profile}_${size}_gate PROPERTIES TIMEOUT 120
          LABELS "storage;binary_uuid;real_files;historical_provenance;memory;not_runtime_acceptance")
      endforeach()
      add_test(NAME native_directory_history_repeat_${profile}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-history-repeat ${profile})
      set_tests_properties(native_directory_history_repeat_${profile}_gate PROPERTIES TIMEOUT 120
        LABELS "storage;binary_uuid;real_files;historical_provenance;not_runtime_acceptance")
    endif()
    add_test(NAME native_directory_history_faults_${profile}_gate
      COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-history-faults ${profile})
    set_tests_properties(native_directory_history_faults_${profile}_gate PROPERTIES TIMEOUT 120
      LABELS "storage;binary_uuid;real_files;historical_provenance;fault_injection;not_runtime_acceptance")
    foreach(primary RANGE 0 4)
      # Additional grant/heap/retirement checks are independently sharded for
      # every ordered pair. Keep the original grouped tests unchanged.
      foreach(secondary RANGE 0 4)
        add_test(NAME native_directory_selection_memory_${profile}_${primary}_${secondary}_gate
          COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-selection-memory ${profile} ${primary} ${secondary})
        set_tests_properties(native_directory_selection_memory_${profile}_${primary}_${secondary}_gate PROPERTIES TIMEOUT 120
          LABELS "storage;binary_uuid;real_files;selected_control_ancestry;memory;not_runtime_acceptance")
        add_test(NAME native_directory_history_memory_${profile}_${primary}_${secondary}_gate
          COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-history-memory ${profile} ${primary} ${secondary})
        set_tests_properties(native_directory_history_memory_${profile}_${primary}_${secondary}_gate PROPERTIES TIMEOUT 120
          LABELS "storage;binary_uuid;real_files;historical_provenance;memory;not_runtime_acceptance")
      endforeach()
      add_test(NAME native_directory_history_${profile}_${primary}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-history ${profile} ${primary})
      set_tests_properties(native_directory_history_${profile}_${primary}_gate PROPERTIES TIMEOUT 120
        LABELS "storage;binary_uuid;real_files;historical_provenance;not_runtime_acceptance")
    endforeach()
    # Partition the complete matrix, not its assertions: profile4 has 200
    # real-file/mutation cases and exceeded the aggregate 120-second deadline.
    # All primary/secondary page sizes and ordering/target/state cases remain.
    foreach(primary RANGE 0 4)
      add_test(NAME native_directory_allocation_delta_${profile}_${primary}_gate
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-delta-shard ${profile} ${primary})
      set_tests_properties(native_directory_allocation_delta_${profile}_${primary}_gate PROPERTIES TIMEOUT 120
        LABELS "storage;binary_uuid;immutable_transition;not_runtime_acceptance")
    endforeach()
    add_test(NAME native_directory_allocation_delta_faults_${profile}_gate
      COMMAND sbsql_sblr_alignment_native_management_control_bundle --directory-delta-faults ${profile})
    set_tests_properties(native_directory_allocation_delta_faults_${profile}_gate PROPERTIES TIMEOUT 120
      LABELS "storage;binary_uuid;immutable_transition;fault_injection;not_runtime_acceptance")
  endforeach()
  add_test(NAME native_preallocation_repeat_gate
    COMMAND sbsql_sblr_alignment_native_management_control_bundle --native-preallocation-repeat)
  set_tests_properties(native_preallocation_repeat_gate PROPERTIES TIMEOUT 120
    LABELS "storage;binary_uuid;real_files;native_publication;not_runtime_acceptance")
  add_test(NAME native_preallocation_publication_gate
    COMMAND sbsql_sblr_alignment_native_management_control_bundle --native-preallocation)
  set_tests_properties(native_preallocation_publication_gate PROPERTIES
    TIMEOUT 120 LABELS "storage;binary_uuid;real_files;native_publication;not_runtime_acceptance")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(sbsql_sblr_alignment_native_management_control_bundle PRIVATE -Wl,--wrap=posix_fallocate)
    target_compile_definitions(sbsql_sblr_alignment_native_management_control_bundle PRIVATE NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS=1)
    target_link_options(sbsql_sblr_alignment_native_management_control_bundle PRIVATE -Wl,--wrap=fstat)
    foreach(stage RANGE 0 2)
      foreach(route RANGE 0 2)
        if(route EQUAL 0)
          set(reconcile_last_shard 0)
        elseif(route EQUAL 1)
          set(reconcile_last_shard 4)
        else()
          set(reconcile_last_shard 7)
        endif()
        foreach(shard RANGE 0 ${reconcile_last_shard})
          add_test(NAME native_preallocation_reconcile_${stage}_${route}_${shard}
            COMMAND sbsql_sblr_alignment_native_management_control_bundle --native-preallocation-reconcile-faults ${stage} ${route} ${shard})
          set_tests_properties(native_preallocation_reconcile_${stage}_${route}_${shard} PROPERTIES TIMEOUT 180
            LABELS "storage;binary_uuid;real_files;fault_injection;native_publication;not_runtime_acceptance")
        endforeach()
      endforeach()
    endforeach()
    add_test(NAME native_preallocation_publication_fault_gate
      COMMAND sbsql_sblr_alignment_native_management_control_bundle --native-preallocation-faults)
    set_tests_properties(native_preallocation_publication_fault_gate PROPERTIES
      TIMEOUT 120 LABELS "storage;binary_uuid;real_files;fault_injection;native_publication;not_runtime_acceptance")
    foreach(profile RANGE 0 4)
      add_test(NAME native_preallocation_crash_${profile}
        COMMAND sbsql_sblr_alignment_native_management_control_bundle --native-preallocation-crashes ${profile})
      set_tests_properties(native_preallocation_crash_${profile} PROPERTIES TIMEOUT 180
        LABELS "storage;binary_uuid;real_files;fault_injection;native_publication;not_runtime_acceptance")
    endforeach()
    foreach(route RANGE 0 2)
      if(route EQUAL 2)
        set(preallocation_last_shard 7)
      else()
        set(preallocation_last_shard 0)
      endif()
      foreach(shard RANGE 0 ${preallocation_last_shard})
        add_test(NAME native_preallocation_sweep_${route}_${shard}
          COMMAND sbsql_sblr_alignment_native_management_control_bundle --native-preallocation-sweep ${route} ${shard})
        set_tests_properties(native_preallocation_sweep_${route}_${shard} PROPERTIES
          TIMEOUT 180 LABELS "storage;binary_uuid;real_files;fault_injection;native_publication;not_runtime_acceptance")
      endforeach()
    endforeach()
  endif()
  add_test(NAME native_publication_effect_receipt_gate
    COMMAND sbsql_sblr_alignment_native_management_control_bundle --publication-effects)
  set_tests_properties(native_publication_effect_receipt_gate PROPERTIES
    LABELS "storage;actual_files;partial_effects;binary_uuid;not_runtime_acceptance" TIMEOUT 600)
  foreach(startup_profile RANGE 0 4)
    add_test(NAME native_startup_binding_${startup_profile}_gate
      COMMAND sbsql_sblr_alignment_native_management_control_bundle --startup-binding ${startup_profile})
    set_tests_properties(native_startup_binding_${startup_profile}_gate PROPERTIES
      LABELS "storage;startup;actual_files;partial_effects;binary_uuid;not_runtime_acceptance" TIMEOUT 600)
    foreach(startup_phase RANGE 0 1)
      foreach(startup_fault_route IN ITEMS 3 4)
        add_test(NAME native_startup_binding_faults_${startup_profile}_${startup_phase}_${startup_fault_route}
          COMMAND sbsql_sblr_alignment_native_management_control_bundle --startup-binding-faults ${startup_profile} ${startup_phase} ${startup_fault_route})
        set_tests_properties(native_startup_binding_faults_${startup_profile}_${startup_phase}_${startup_fault_route} PROPERTIES
          LABELS "storage;startup;actual_files;fault_injection;binary_uuid;not_runtime_acceptance" TIMEOUT 600)
      endforeach()
    endforeach()
  endforeach()
  foreach(startup_phase RANGE 0 1)
    foreach(startup_route RANGE 0 2)
      if(startup_route EQUAL 2)
        set(startup_last_shard 31)
      elseif(startup_route EQUAL 1)
        set(startup_last_shard 4)
      else()
        set(startup_last_shard 0)
      endif()
      foreach(startup_shard RANGE 0 ${startup_last_shard})
        add_test(NAME native_startup_binding_sweep_${startup_phase}_${startup_route}_${startup_shard}
          COMMAND sbsql_sblr_alignment_native_management_control_bundle --startup-binding-sweep ${startup_phase} ${startup_route} ${startup_shard})
        set_tests_properties(native_startup_binding_sweep_${startup_phase}_${startup_route}_${startup_shard} PROPERTIES
          LABELS "storage;startup;actual_files;allocation_faults;hash_faults;binary_uuid;not_runtime_acceptance" TIMEOUT 600)
      endforeach()
    endforeach()
  endforeach()
endif()

add_executable(catalog_storage_action_policy_gate catalog_storage_action_policy_gate.cpp)
if(TARGET sbsql_sblr_alignment_native_storage_components)
  add_test(NAME native_storage_governed_range_gate
    COMMAND sbsql_sblr_alignment_native_storage_components --catalog-storage-policy-stage-only)
  set_tests_properties(native_storage_governed_range_gate PROPERTIES
    TIMEOUT 600 LABELS "storage;memory;binary_uuid;actual_files;not_runtime_acceptance")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(native_checkpoint_selection_memory_gate
    ../sbsql_sblr_alignment/native_checkpoint_selection_test.cpp)
  target_compile_features(native_checkpoint_selection_memory_gate PRIVATE cxx_std_23)
  target_compile_definitions(native_checkpoint_selection_memory_gate PRIVATE SB_NATIVE_SELECTOR_MEMORY_TESTS=1)
  target_link_libraries(native_checkpoint_selection_memory_gate PRIVATE sb_storage_database)
  target_link_options(native_checkpoint_selection_memory_gate PRIVATE
    -Wl,--wrap=pread -Wl,--wrap=pwrite -Wl,--wrap=fsync -Wl,--wrap=EVP_MD_CTX_new -Wl,--wrap=EVP_DigestInit_ex
    -Wl,--wrap=EVP_DigestUpdate -Wl,--wrap=EVP_DigestFinal_ex)
  add_test(NAME native_checkpoint_selection_memory_gate COMMAND native_checkpoint_selection_memory_gate)
  set_tests_properties(native_checkpoint_selection_memory_gate PROPERTIES
    TIMEOUT 120 LABELS "storage;memory;binary_uuid;actual_files;fault_injection;not_runtime_acceptance")
  add_executable(bounded_device_io_gate bounded_device_io_gate.cpp)
  target_compile_features(bounded_device_io_gate PRIVATE cxx_std_23)
  target_link_libraries(bounded_device_io_gate PRIVATE sb_storage_disk)
  target_link_options(bounded_device_io_gate PRIVATE
    -Wl,--wrap=pread -Wl,--wrap=pwrite -Wl,--wrap=fsync -Wl,--wrap=fstat)
  add_test(NAME bounded_device_io_gate COMMAND bounded_device_io_gate)
  set_tests_properties(bounded_device_io_gate PROPERTIES
    TIMEOUT 120 LABELS "storage;memory;binary_uuid;actual_files;fault_injection;not_runtime_acceptance")
  add_executable(native_storage_prepared_hash_gate native_storage_prepared_hash_gate.cpp)
  target_compile_features(native_storage_prepared_hash_gate PRIVATE cxx_std_23)
  target_link_libraries(native_storage_prepared_hash_gate PRIVATE sb_storage_database sb_core_hash)
  target_link_options(native_storage_prepared_hash_gate PRIVATE
    -Wl,--wrap=EVP_Digest -Wl,--wrap=EVP_MD_CTX_new -Wl,--wrap=EVP_DigestInit_ex
    -Wl,--wrap=EVP_DigestUpdate -Wl,--wrap=EVP_DigestFinal_ex)
  add_test(NAME native_storage_prepared_hash_gate COMMAND native_storage_prepared_hash_gate)
  set_tests_properties(native_storage_prepared_hash_gate PROPERTIES
    TIMEOUT 120 LABELS "storage;memory;binary_uuid;actual_files;fault_injection;not_runtime_acceptance")
  add_executable(native_allocation_map_memory_gate ../sbsql_sblr_alignment/native_allocation_map_test.cpp)
  target_compile_features(native_allocation_map_memory_gate PRIVATE cxx_std_23)
  target_compile_definitions(native_allocation_map_memory_gate PRIVATE SB_NATIVE_ALLOCATION_MEMORY_TESTS=1 NATIVE_HISTORICAL_IO_FAULTS=1)
  target_include_directories(native_allocation_map_memory_gate PRIVATE ../../src/storage/database)
  target_link_libraries(native_allocation_map_memory_gate PRIVATE sb_storage_page)
  target_link_options(native_allocation_map_memory_gate PRIVATE
    -Wl,--wrap=pread -Wl,--wrap=pwrite -Wl,--wrap=fsync -Wl,--wrap=fstat
    -Wl,--wrap=EVP_MD_CTX_new -Wl,--wrap=EVP_Digest -Wl,--wrap=EVP_DigestInit_ex
    -Wl,--wrap=EVP_DigestUpdate -Wl,--wrap=EVP_DigestFinal_ex)
  add_test(NAME native_allocation_map_memory_gate COMMAND native_allocation_map_memory_gate)
  set_tests_properties(native_allocation_map_memory_gate PROPERTIES
    TIMEOUT 180 LABELS "storage;memory;binary_uuid;actual_files;fault_injection;not_runtime_acceptance")
  add_executable(native_checkpoint_decoded_memory_gate native_checkpoint_decoded_memory_gate.cpp)
  add_executable(native_inventory_memory_gate native_inventory_memory_gate.cpp)
  add_executable(native_retention_memory_gate native_retention_memory_gate.cpp)
  add_executable(native_horizon_memory_gate native_horizon_memory_gate.cpp)
  add_executable(native_catalog_root_memory_gate native_catalog_root_memory_gate.cpp)
  add_executable(native_catalog_value_memory_gate native_catalog_value_memory_gate.cpp)
  add_executable(native_catalog_fixed_family_memory_gate native_catalog_fixed_family_memory_gate.cpp)
  add_executable(native_catalog_bootstrap_family_memory_gate native_catalog_bootstrap_family_memory_gate.cpp)
  add_executable(native_catalog_label_schema_memory_gate native_catalog_label_schema_memory_gate.cpp)
  add_executable(native_catalog_series_memory_gate native_catalog_series_memory_gate.cpp)
  add_executable(native_catalog_descriptor_memory_gate native_catalog_descriptor_memory_gate.cpp)
  add_executable(native_catalog_current_value_memory_gate native_catalog_current_value_memory_gate.cpp)
  add_executable(native_catalog_typed_record_memory_gate native_catalog_typed_record_memory_gate.cpp)
  add_executable(native_catalog_metadata_memory_gate native_catalog_metadata_memory_gate.cpp)
  add_executable(native_catalog_name_memory_gate native_catalog_name_memory_gate.cpp)
  if(TARGET sbsql_sblr_alignment_native_storage_components)
    add_test(NAME native_catalog_binary_cell_gate
      COMMAND sbsql_sblr_alignment_native_storage_components --catalog-binary-cells-only)
    set_tests_properties(native_catalog_binary_cell_gate PROPERTIES
      TIMEOUT 180 LABELS "storage;binary_uuid;catalog;allocation_failure;real_file_io;not_runtime_acceptance")
  endif()
  add_executable(native_catalog_name_envelope_memory_gate native_catalog_name_envelope_memory_gate.cpp)
  target_compile_features(native_catalog_name_envelope_memory_gate PRIVATE cxx_std_23)
  target_link_libraries(native_catalog_name_envelope_memory_gate PRIVATE sb_core_catalog sb_storage_page)
  add_test(NAME native_catalog_name_envelope_memory_gate COMMAND native_catalog_name_envelope_memory_gate)
  set_tests_properties(native_catalog_name_envelope_memory_gate PROPERTIES
    TIMEOUT 180 LABELS "storage;memory;binary_uuid;catalog;not_runtime_acceptance")
  target_compile_features(native_catalog_name_memory_gate PRIVATE cxx_std_23)
  target_link_libraries(native_catalog_name_memory_gate PRIVATE sb_core_catalog)
  add_test(NAME native_catalog_name_memory_gate COMMAND native_catalog_name_memory_gate)
  set_tests_properties(native_catalog_name_memory_gate PROPERTIES
    TIMEOUT 180 LABELS "storage;memory;binary_uuid;catalog;not_runtime_acceptance")
  target_compile_features(native_catalog_metadata_memory_gate PRIVATE cxx_std_23)
  target_link_libraries(native_catalog_metadata_memory_gate PRIVATE sb_core_catalog)
  target_link_options(native_catalog_metadata_memory_gate PRIVATE
    -Wl,--wrap=EVP_MD_CTX_new -Wl,--wrap=EVP_MD_CTX_free -Wl,--wrap=EVP_DigestInit_ex
    -Wl,--wrap=EVP_DigestUpdate -Wl,--wrap=EVP_DigestFinal_ex)
  add_test(NAME native_catalog_metadata_memory_gate COMMAND native_catalog_metadata_memory_gate)
  set_tests_properties(native_catalog_metadata_memory_gate PROPERTIES
    TIMEOUT 180 LABELS "storage;memory;binary_uuid;catalog;hash_failure;not_runtime_acceptance")
  target_compile_features(native_catalog_typed_record_memory_gate PRIVATE cxx_std_23)
  target_link_libraries(native_catalog_typed_record_memory_gate PRIVATE sb_core_catalog)
  add_test(NAME native_catalog_typed_record_memory_gate COMMAND native_catalog_typed_record_memory_gate)
  set_tests_properties(native_catalog_typed_record_memory_gate PROPERTIES
    TIMEOUT 180 LABELS "storage;memory;binary_uuid;catalog;not_runtime_acceptance")
  target_compile_features(native_catalog_current_value_memory_gate PRIVATE cxx_std_23)
  target_link_libraries(native_catalog_current_value_memory_gate PRIVATE sb_core_catalog)
  add_test(NAME native_catalog_current_value_memory_gate COMMAND native_catalog_current_value_memory_gate)
  set_tests_properties(native_catalog_current_value_memory_gate PROPERTIES
    TIMEOUT 180 LABELS "storage;memory;binary_uuid;catalog;not_runtime_acceptance")
  target_compile_features(native_catalog_descriptor_memory_gate PRIVATE cxx_std_23)
  target_link_libraries(native_catalog_descriptor_memory_gate PRIVATE sb_core_catalog)
  add_test(NAME native_catalog_descriptor_memory_gate COMMAND native_catalog_descriptor_memory_gate)
  set_tests_properties(native_catalog_descriptor_memory_gate PROPERTIES
    TIMEOUT 180 LABELS "storage;memory;binary_uuid;catalog;not_runtime_acceptance")
  target_compile_features(native_catalog_series_memory_gate PRIVATE cxx_std_23)
  target_link_libraries(native_catalog_series_memory_gate PRIVATE sb_core_catalog)
  add_test(NAME native_catalog_series_memory_gate COMMAND native_catalog_series_memory_gate)
  set_tests_properties(native_catalog_series_memory_gate PROPERTIES
    TIMEOUT 180 LABELS "storage;memory;binary_uuid;catalog;not_runtime_acceptance")
  target_compile_features(native_catalog_label_schema_memory_gate PRIVATE cxx_std_23)
  target_link_libraries(native_catalog_label_schema_memory_gate PRIVATE sb_core_catalog)
  add_test(NAME native_catalog_label_schema_memory_gate COMMAND native_catalog_label_schema_memory_gate)
  set_tests_properties(native_catalog_label_schema_memory_gate PROPERTIES
    TIMEOUT 180 LABELS "storage;memory;binary_uuid;catalog;not_runtime_acceptance")
  target_compile_features(native_catalog_bootstrap_family_memory_gate PRIVATE cxx_std_23)
  target_link_libraries(native_catalog_bootstrap_family_memory_gate PRIVATE sb_core_catalog)
  add_test(NAME native_catalog_bootstrap_family_memory_gate COMMAND native_catalog_bootstrap_family_memory_gate)
  set_tests_properties(native_catalog_bootstrap_family_memory_gate PROPERTIES
    TIMEOUT 180 LABELS "storage;memory;binary_uuid;catalog;not_runtime_acceptance")
  add_executable(native_catalog_retention_family_memory_gate native_catalog_retention_family_memory_gate.cpp)
  target_compile_features(native_catalog_retention_family_memory_gate PRIVATE cxx_std_23)
  target_link_libraries(native_catalog_retention_family_memory_gate PRIVATE sb_core_catalog)
  add_test(NAME native_catalog_retention_family_memory_gate COMMAND native_catalog_retention_family_memory_gate)
  set_tests_properties(native_catalog_retention_family_memory_gate PROPERTIES
    TIMEOUT 180 LABELS "storage;memory;binary_uuid;catalog;not_runtime_acceptance")
  target_compile_features(native_catalog_fixed_family_memory_gate PRIVATE cxx_std_23)
  target_link_libraries(native_catalog_fixed_family_memory_gate PRIVATE sb_core_catalog)
  add_test(NAME native_catalog_fixed_family_memory_gate COMMAND native_catalog_fixed_family_memory_gate)
  set_tests_properties(native_catalog_fixed_family_memory_gate PROPERTIES
    TIMEOUT 180 LABELS "storage;memory;binary_uuid;catalog;not_runtime_acceptance")
  target_compile_features(native_catalog_value_memory_gate PRIVATE cxx_std_23)
  target_link_libraries(native_catalog_value_memory_gate PRIVATE sb_core_catalog)
  add_test(NAME native_catalog_value_memory_gate COMMAND native_catalog_value_memory_gate)
  set_tests_properties(native_catalog_value_memory_gate PROPERTIES
    TIMEOUT 180 LABELS "storage;memory;binary_uuid;catalog;not_runtime_acceptance")
  target_compile_features(native_catalog_root_memory_gate PRIVATE cxx_std_23)
  target_link_libraries(native_catalog_root_memory_gate PRIVATE sb_storage_database)
  target_link_options(native_catalog_root_memory_gate PRIVATE
    -Wl,--wrap=pread -Wl,--wrap=pwrite -Wl,--wrap=fsync -Wl,--wrap=EVP_Digest
    -Wl,--wrap=EVP_MD_CTX_new -Wl,--wrap=EVP_DigestInit_ex
    -Wl,--wrap=EVP_DigestUpdate -Wl,--wrap=EVP_DigestFinal_ex)
  add_test(NAME native_catalog_root_memory_gate COMMAND native_catalog_root_memory_gate)
  set_tests_properties(native_catalog_root_memory_gate PROPERTIES
    TIMEOUT 300 LABELS "storage;memory;binary_uuid;actual_files;fault_injection;not_runtime_acceptance")
  target_compile_features(native_horizon_memory_gate PRIVATE cxx_std_23)
  target_link_libraries(native_horizon_memory_gate PRIVATE sb_storage_database)
  target_link_options(native_horizon_memory_gate PRIVATE
    -Wl,--wrap=pread -Wl,--wrap=pwrite -Wl,--wrap=fsync -Wl,--wrap=EVP_Digest
    -Wl,--wrap=EVP_MD_CTX_new -Wl,--wrap=EVP_DigestInit_ex
    -Wl,--wrap=EVP_DigestUpdate -Wl,--wrap=EVP_DigestFinal_ex)
  add_test(NAME native_horizon_memory_gate COMMAND native_horizon_memory_gate)
  set_tests_properties(native_horizon_memory_gate PROPERTIES
    TIMEOUT 300 LABELS "storage;memory;binary_uuid;actual_files;fault_injection;not_runtime_acceptance")
  target_compile_features(native_retention_memory_gate PRIVATE cxx_std_23)
  target_link_libraries(native_retention_memory_gate PRIVATE sb_storage_database)
  target_link_options(native_retention_memory_gate PRIVATE
    -Wl,--wrap=pread -Wl,--wrap=pwrite -Wl,--wrap=fsync -Wl,--wrap=EVP_Digest
    -Wl,--wrap=EVP_MD_CTX_new -Wl,--wrap=EVP_DigestInit_ex
    -Wl,--wrap=EVP_DigestUpdate -Wl,--wrap=EVP_DigestFinal_ex)
  add_test(NAME native_retention_memory_gate COMMAND native_retention_memory_gate)
  set_tests_properties(native_retention_memory_gate PROPERTIES
    TIMEOUT 300 LABELS "storage;memory;binary_uuid;actual_files;fault_injection;not_runtime_acceptance")
  add_executable(native_system_state_memory_gate native_system_state_memory_gate.cpp)
  target_compile_features(native_system_state_memory_gate PRIVATE cxx_std_23)
  target_link_libraries(native_system_state_memory_gate PRIVATE sb_storage_database)
  target_link_options(native_system_state_memory_gate PRIVATE
    -Wl,--wrap=pread -Wl,--wrap=pwrite -Wl,--wrap=fsync -Wl,--wrap=EVP_Digest
    -Wl,--wrap=EVP_MD_CTX_new -Wl,--wrap=EVP_DigestInit_ex
    -Wl,--wrap=EVP_DigestUpdate -Wl,--wrap=EVP_DigestFinal_ex)
  add_test(NAME native_system_state_memory_gate COMMAND native_system_state_memory_gate)
  set_tests_properties(native_system_state_memory_gate PROPERTIES
    TIMEOUT 180 LABELS "storage;memory;binary_uuid;actual_files;fault_injection;not_runtime_acceptance")
  target_compile_features(native_inventory_memory_gate PRIVATE cxx_std_23)
  target_link_libraries(native_inventory_memory_gate PRIVATE sb_storage_database)
  target_link_options(native_inventory_memory_gate PRIVATE
    -Wl,--wrap=pread -Wl,--wrap=pwrite -Wl,--wrap=fsync -Wl,--wrap=fstat -Wl,--wrap=EVP_Digest
    -Wl,--wrap=EVP_MD_CTX_new -Wl,--wrap=EVP_DigestInit_ex
    -Wl,--wrap=EVP_DigestUpdate -Wl,--wrap=EVP_DigestFinal_ex)
  add_test(NAME native_inventory_memory_gate COMMAND native_inventory_memory_gate)
  set_tests_properties(native_inventory_memory_gate PROPERTIES
    TIMEOUT 180 LABELS "storage;memory;binary_uuid;actual_files;fault_injection;not_runtime_acceptance")
  add_executable(native_filespace_page_zero_memory_gate native_filespace_page_zero_memory_gate.cpp)
  target_compile_features(native_filespace_page_zero_memory_gate PRIVATE cxx_std_23)
  target_link_libraries(native_filespace_page_zero_memory_gate PRIVATE sb_storage_database)
  target_link_options(native_filespace_page_zero_memory_gate PRIVATE
    -Wl,--wrap=pread -Wl,--wrap=pwrite -Wl,--wrap=fsync -Wl,--wrap=fstat -Wl,--wrap=EVP_Digest
    -Wl,--wrap=EVP_MD_CTX_new -Wl,--wrap=EVP_DigestInit_ex
    -Wl,--wrap=EVP_DigestUpdate -Wl,--wrap=EVP_DigestFinal_ex)
  add_test(NAME native_filespace_page_zero_memory_gate COMMAND native_filespace_page_zero_memory_gate)
  set_tests_properties(native_filespace_page_zero_memory_gate PROPERTIES
    TIMEOUT 180 LABELS "storage;memory;binary_uuid;actual_files;fault_injection;not_runtime_acceptance")
  target_compile_features(native_checkpoint_decoded_memory_gate PRIVATE cxx_std_23)
  target_link_libraries(native_checkpoint_decoded_memory_gate PRIVATE sb_storage_database)
  target_link_options(native_checkpoint_decoded_memory_gate PRIVATE
    -Wl,--wrap=pread -Wl,--wrap=pwrite -Wl,--wrap=fsync -Wl,--wrap=EVP_Digest
    -Wl,--wrap=EVP_MD_CTX_new -Wl,--wrap=EVP_DigestInit_ex
    -Wl,--wrap=EVP_DigestUpdate -Wl,--wrap=EVP_DigestFinal_ex)
  add_test(NAME native_checkpoint_decoded_memory_gate COMMAND native_checkpoint_decoded_memory_gate)
  set_tests_properties(native_checkpoint_decoded_memory_gate PROPERTIES
    TIMEOUT 120 LABELS "storage;memory;binary_uuid;actual_files;fault_injection;not_runtime_acceptance")
  add_executable(native_filespace_directory_memory_gate ../sbsql_sblr_alignment/native_filespace_directory_test.cpp)
  target_compile_features(native_filespace_directory_memory_gate PRIVATE cxx_std_23)
  target_compile_definitions(native_filespace_directory_memory_gate PRIVATE SB_NATIVE_DIRECTORY_MEMORY_TESTS=1 NATIVE_HISTORICAL_IO_FAULTS=1)
  target_include_directories(native_filespace_directory_memory_gate PRIVATE ../../src/storage/database)
  target_link_libraries(native_filespace_directory_memory_gate PRIVATE sb_storage_page)
  target_link_options(native_filespace_directory_memory_gate PRIVATE
    -Wl,--wrap=pread -Wl,--wrap=pwrite -Wl,--wrap=fsync -Wl,--wrap=fstat
    -Wl,--wrap=EVP_MD_CTX_new -Wl,--wrap=EVP_Digest -Wl,--wrap=EVP_DigestInit_ex
    -Wl,--wrap=EVP_DigestUpdate -Wl,--wrap=EVP_DigestFinal_ex)
  add_test(NAME native_current_source_bootstrap_memory_gate
    COMMAND sbsql_sblr_alignment_filespace_page_zero --current-source-memory-only)
  set_tests_properties(native_current_source_bootstrap_memory_gate PROPERTIES TIMEOUT 300
    LABELS "storage;binary_uuid;real_files;current_source;memory;not_runtime_acceptance")
  add_test(NAME native_filespace_directory_memory_gate COMMAND native_filespace_directory_memory_gate)
  set_tests_properties(native_filespace_directory_memory_gate PROPERTIES
    TIMEOUT 180 LABELS "storage;memory;binary_uuid;actual_files;fault_injection;not_runtime_acceptance")
  add_executable(native_storage_memory_buffer_gate native_storage_memory_buffer_gate.cpp)
  add_executable(native_storage_memory_arena_gate native_storage_memory_arena_gate.cpp)
  target_compile_features(native_storage_memory_arena_gate PRIVATE cxx_std_23)
  target_include_directories(native_storage_memory_arena_gate PRIVATE ../../src/storage/database)
  target_link_libraries(native_storage_memory_arena_gate PRIVATE sb_storage_disk sb_core_memory)
  add_test(NAME native_storage_memory_arena_gate COMMAND native_storage_memory_arena_gate)
  set_tests_properties(native_storage_memory_arena_gate PROPERTIES
    TIMEOUT 120 LABELS "storage;memory;binary_uuid;actual_files;fault_injection;not_runtime_acceptance")
  target_compile_features(native_storage_memory_buffer_gate PRIVATE cxx_std_23)
  target_include_directories(native_storage_memory_buffer_gate PRIVATE ../../src/storage/database)
  target_link_libraries(native_storage_memory_buffer_gate PRIVATE sb_storage_disk sb_core_memory)
  add_test(NAME native_storage_memory_buffer_gate COMMAND native_storage_memory_buffer_gate)
  set_tests_properties(native_storage_memory_buffer_gate PROPERTIES
    TIMEOUT 120 LABELS "storage;memory;binary_uuid;actual_files;fault_injection;not_runtime_acceptance")
endif()
add_executable(native_storage_memory_reservation_gate native_storage_memory_reservation_gate.cpp)
target_compile_features(native_storage_memory_reservation_gate PRIVATE cxx_std_23)
target_link_libraries(native_storage_memory_reservation_gate PRIVATE sb_core_memory)
add_test(NAME native_storage_memory_reservation_gate COMMAND native_storage_memory_reservation_gate)
set_tests_properties(native_storage_memory_reservation_gate PROPERTIES
  TIMEOUT 120 LABELS "storage;memory;binary_uuid;reservation_adapter;not_runtime_acceptance")
target_compile_features(catalog_storage_action_policy_gate PRIVATE cxx_std_23)
target_link_libraries(catalog_storage_action_policy_gate PRIVATE sb_core_catalog)
add_test(NAME catalog_storage_action_policy_gate COMMAND catalog_storage_action_policy_gate)
set_tests_properties(catalog_storage_action_policy_gate PROPERTIES
  TIMEOUT 60 LABELS "storage;catalog;binary_uuid;policy_codec;not_runtime_acceptance")
if(TARGET sbsql_sblr_alignment_filespace_page_zero)
  add_test(NAME native_storage_policy_catalog_staging_gate
    COMMAND sbsql_sblr_alignment_filespace_page_zero --catalog-storage-policy-stage-only)
  set_tests_properties(native_storage_policy_catalog_staging_gate PROPERTIES
    TIMEOUT 120 LABELS "storage;catalog;binary_uuid;actual_files;not_runtime_acceptance")
endif()

add_executable(filespace_byte_observation_gate filespace_byte_observation_gate.cpp)
target_compile_features(filespace_byte_observation_gate PRIVATE cxx_std_23)
target_link_libraries(filespace_byte_observation_gate PRIVATE sb_core_metrics)
add_test(NAME filespace_byte_observation_gate COMMAND filespace_byte_observation_gate)
set_tests_properties(filespace_byte_observation_gate PROPERTIES
  TIMEOUT 60 LABELS "storage;metrics;binary_uuid;component_regression;not_runtime_acceptance")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(native_storage_action_intent_gate native_storage_action_intent_gate.cpp)
  target_compile_features(native_storage_action_intent_gate PRIVATE cxx_std_23)
  target_link_libraries(native_storage_action_intent_gate PRIVATE sb_storage_database OpenSSL::Crypto)
  target_link_options(native_storage_action_intent_gate PRIVATE
    "LINKER:--wrap=pread" "LINKER:--wrap=pwrite" "LINKER:--wrap=fsync"
    "LINKER:--wrap=EVP_MD_CTX_new" "LINKER:--wrap=EVP_DigestInit_ex"
    "LINKER:--wrap=EVP_DigestUpdate" "LINKER:--wrap=EVP_DigestFinal_ex" "LINKER:--wrap=EVP_Digest")
  add_test(NAME native_storage_action_intent_gate COMMAND native_storage_action_intent_gate)
  set_tests_properties(native_storage_action_intent_gate PROPERTIES
    TIMEOUT 60 LABELS "storage;binary_uuid;intent_codec;fault_injection;not_runtime_acceptance")
  add_executable(native_owned_source_route_gate native_owned_source_route_gate.cpp
    "${PROJECT_SOURCE_DIR}/src/server/database_ownership.cpp")
  target_compile_features(native_owned_source_route_gate PRIVATE cxx_std_23)
  target_include_directories(native_owned_source_route_gate PRIVATE "${PROJECT_SOURCE_DIR}/src/server")
  target_link_libraries(native_owned_source_route_gate PRIVATE sb_storage_database sb_core_uuid)
  target_link_options(native_owned_source_route_gate PRIVATE "LINKER:--wrap=pread"
    "LINKER:--wrap=pthread_cond_timedwait" "LINKER:--wrap=pthread_mutex_lock"
    "LINKER:--wrap=pthread_cond_destroy" "LINKER:--wrap=pthread_cond_init")
  add_test(NAME native_owned_source_route_gate COMMAND native_owned_source_route_gate)
  set_tests_properties(native_owned_source_route_gate PROPERTIES
    TIMEOUT 60 LABELS "storage;binary_uuid;route_ownership;not_runtime_acceptance")
  add_executable(filespace_retained_observation_gate filespace_retained_observation_gate.cpp)
  target_compile_features(filespace_retained_observation_gate PRIVATE cxx_std_23)
  target_link_libraries(filespace_retained_observation_gate PRIVATE sb_storage_disk)
  target_link_options(filespace_retained_observation_gate PRIVATE
    "LINKER:--wrap=pthread_mutex_unlock" "LINKER:--wrap=pread" "LINKER:--wrap=fstat")
  add_test(NAME filespace_retained_observation_gate COMMAND filespace_retained_observation_gate)
  set_tests_properties(filespace_retained_observation_gate PROPERTIES
    TIMEOUT 60 LABELS "storage;disk;binary_uuid;native_fault_injection;not_runtime_acceptance")
  add_executable(device_extent_preallocation_gate device_extent_preallocation_gate.cpp)
  target_compile_features(device_extent_preallocation_gate PRIVATE cxx_std_23)
  target_link_libraries(device_extent_preallocation_gate PRIVATE sb_storage_disk)
  target_link_options(device_extent_preallocation_gate PRIVATE
    "LINKER:--wrap=posix_fallocate" "LINKER:--wrap=fstat" "LINKER:--wrap=pwrite"
    "LINKER:--wrap=pread" "LINKER:--wrap=fsync")
  add_test(NAME device_extent_preallocation_gate COMMAND device_extent_preallocation_gate)
  set_tests_properties(device_extent_preallocation_gate PROPERTIES
    TIMEOUT 60 LABELS "storage;disk;preallocation;native_fault_injection;not_SQL_E2E")
endif()

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
  sb_storage_database
  sb_core_uuid
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
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(public_filespace_header_capacity_gate PRIVATE
    "LINKER:--wrap=pread" "LINKER:--wrap=fstat" "LINKER:--wrap=fsync"
    "LINKER:--wrap=pwrite")
endif()
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

add_executable(public_ipar_p5_10_backup_restore_archive_snapshot_gate
  public_ipar_p5_10_backup_restore_archive_snapshot_gate.cpp
)
target_compile_features(public_ipar_p5_10_backup_restore_archive_snapshot_gate PRIVATE cxx_std_23)
target_link_libraries(public_ipar_p5_10_backup_restore_archive_snapshot_gate PRIVATE
  sb_engine_internal_api
  sb_storage_database
  sb_storage_filespace
  sb_transaction_mga
  sb_core_memory
  sb_core_uuid
)
add_test(
  NAME public_ipar_p5_10_backup_restore_archive_snapshot_gate
  COMMAND public_ipar_p5_10_backup_restore_archive_snapshot_gate
          ${CMAKE_CURRENT_BINARY_DIR}/public_ipar_p5_10_backup_restore_archive_snapshot_gate_tmp
)
set_tests_properties(public_ipar_p5_10_backup_restore_archive_snapshot_gate PROPERTIES
  LABELS "public_release_correctness;public_ipar_p5_10_backup_restore_archive_snapshot_gate;IPAR-P5-10;backup;archive;restore;snapshot_coordination;mga_transaction_regression"
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

add_executable(public_sbsql_language_release_channel_gate
  public_sbsql_language_release_channel_gate.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/parsers/sbsql_worker/resources/language_resource_contract.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/wire/parser_server_ipc/parser_ipc_common.cpp
)
target_compile_features(public_sbsql_language_release_channel_gate PRIVATE cxx_std_23)
target_include_directories(public_sbsql_language_release_channel_gate PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/parsers/sbsql_worker
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/wire/parser_server_ipc
)
add_test(
  NAME public_sbsql_language_release_channel_gate
  COMMAND public_sbsql_language_release_channel_gate
)
set_tests_properties(public_sbsql_language_release_channel_gate PROPERTIES
  LABELS "public_release_correctness;public_sbsql_language_release_channel_gate;SML-092;sbsql_language_resource_contract;release_channel_lifecycle;fail_closed"
)

# PUBLIC_SBLR_UUID_INTEGRATION_GATE
add_executable(public_sblr_uuid_mga_route_integration_gate
  public_sblr_uuid_mga_route_integration_gate.cpp
)
target_compile_features(public_sblr_uuid_mga_route_integration_gate PRIVATE cxx_std_23)
target_link_libraries(public_sblr_uuid_mga_route_integration_gate PRIVATE
  sb_engine
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

if(NOT TARGET public_driver_test_database_seed)
  add_executable(public_driver_test_database_seed
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_driver_test_database_seed.cpp
  )
  target_compile_features(public_driver_test_database_seed PRIVATE cxx_std_23)
  target_link_libraries(public_driver_test_database_seed PRIVATE
    sb_engine sb_engine_internal_api
    sb_storage_database
    sb_core_memory
    sb_core_uuid
  )
endif()
add_test(
  NAME public_driver_test_database_seed_gate
  COMMAND public_driver_test_database_seed
          --output ${CMAKE_CURRENT_BINARY_DIR}/data/driver-test/scratchbird-driver-test.sbdb
          --manifest ${CMAKE_CURRENT_BINARY_DIR}/data/driver-test/scratchbird-driver-test.manifest.json
          --resource-seed-pack-root ${CMAKE_CURRENT_SOURCE_DIR}/../../resources/seed-packs/initial-resource-pack
          --overwrite
)
set_tests_properties(public_driver_test_database_seed_gate PROPERTIES
  LABELS "public_release_correctness;public_driver_test_database_seed_gate;BDRV-FIX-LIVE-001;driver_test_fixture;full_create_database;storage_database;mga;security"
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

add_test(NAME public_private_reference_scan_semantics
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/public_private_reference_scan_semantics_test.py)
set_tests_properties(public_private_reference_scan_semantics PROPERTIES
  LABELS "public_release_correctness;release_hygiene;unit;negative_controls")

# DBEAVER_MANAGEMENT_PLATFORM_GATE_TEST
add_test(
  NAME dbeaver_management_platform_gate_test
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/dbeaver_management_platform_gate_test.py
)
set_tests_properties(dbeaver_management_platform_gate_test PROPERTIES
  LABELS "public_release_correctness;dbeaver_management_platform_gate;DBEAVER-MGMT-003;DBEAVER-MGMT-006;DBEAVER-MGMT-023;DBEAVER-MGMT-026;DBEAVER-MGMT-027;DBEAVER-MGMT-028;DBEAVER-MGMT-029;DBEAVER-MGMT-034;DBEAVER-MGMT-043;DBEAVER-MGMT-044;DBEAVER-MGMT-046;dbeaver;release_controller;api_drift;secure_properties;license_ip;network_egress"
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

add_executable(public_external_git_versioning_gate
  public_external_git_versioning_gate.cpp
)
target_compile_features(public_external_git_versioning_gate PRIVATE cxx_std_23)
target_link_libraries(public_external_git_versioning_gate PRIVATE
  sb_engine_internal_api
  sb_engine_sblr
)
add_test(
  NAME public_external_git_versioning_gate
  COMMAND public_external_git_versioning_gate
)
set_tests_properties(public_external_git_versioning_gate PROPERTIES
  LABELS "public_release_correctness;public_external_git_versioning_gate;PCR-GATE-010;external_git;catalog_artifact;versioning;rollback_plan;mga_transaction_regression"
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

add_executable(public_optimizer_foundation_contract_gate
  public_optimizer_foundation_contract_gate.cpp
)
target_compile_features(public_optimizer_foundation_contract_gate PRIVATE
  cxx_std_23)
target_link_libraries(public_optimizer_foundation_contract_gate PRIVATE
  sb_engine_optimizer_contract_only
  sb_engine_internal_api
)
add_test(
  NAME public_optimizer_foundation_contract_gate
  COMMAND public_optimizer_foundation_contract_gate
)
set_tests_properties(public_optimizer_foundation_contract_gate PROPERTIES
  LABELS "public_release_correctness;optimizer;profile_factory;model_family;native_optimization;exact_fallback;physical_properties;cost_vector;cross_join;relational_dag;prepare_metrics;MGA;binary_uuid"
)

# Keep the production optimizer manifest/fallback classification proof in the
# default release profile as well as the opt-in broad optimizer suite.
add_executable(public_optimizer_enterprise_inventory_gate
  ../optimizer/optimizer_enterprise_inventory_gate.cpp
)
target_compile_features(public_optimizer_enterprise_inventory_gate PRIVATE
  cxx_std_23)
target_link_libraries(public_optimizer_enterprise_inventory_gate PRIVATE
  sb_engine_optimizer_contract_only
)
add_test(
  NAME public_optimizer_enterprise_inventory_gate
  COMMAND public_optimizer_enterprise_inventory_gate
)
set_tests_properties(public_optimizer_enterprise_inventory_gate PROPERTIES
  LABELS "public_release_correctness;optimizer;enterprise_manifest;model_family;native_optimization;exact_fallback;cluster_boundary;MGA"
)

# Independent production-front-door proof for optimizer-owned model-family
# planning.  This is deliberately enrolled in the release correctness profile
# instead of remaining reachable only through SB_BUILD_QOW_CLOSURE_TESTS.
add_executable(public_optimizer_multimodel_sbsql_e2e_gate
  ../optimizer/qow_ces05_multimodel_closure.cpp
)
target_compile_features(public_optimizer_multimodel_sbsql_e2e_gate PRIVATE
  cxx_std_23)
target_include_directories(public_optimizer_multimodel_sbsql_e2e_gate PRIVATE
  "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(public_optimizer_multimodel_sbsql_e2e_gate PRIVATE
  SB_RCP080_PRODUCTION_QUERY_ROUTE=1
  SB_BOOTSTRAP_SEED_PACK_ROOT="${CMAKE_SOURCE_DIR}/resources/seed-packs/initial-resource-pack")
target_link_libraries(public_optimizer_multimodel_sbsql_e2e_gate PRIVATE
  sb_engine
  sb_engine_sblr
  sb_core_hash
  sb_core_memory
  sb_core_uuid
  sb_storage_database
  sbl_sbsql_parser_worker_core)
add_test(
  NAME public_optimizer_multimodel_sbsql_e2e_gate
  COMMAND public_optimizer_multimodel_sbsql_e2e_gate
)
set_tests_properties(public_optimizer_multimodel_sbsql_e2e_gate PROPERTIES
  LABELS "public_release_correctness;optimizer;model_family;native_optimization;exact_fallback;SBSQL_frontdoor;SBLR_dispatch;physical_dag;executor;post_state;MGA"
)

add_executable(public_optimizer_prepare_execute_metric_gate
  ../optimizer/qow_opt_009.cpp
)
target_compile_features(public_optimizer_prepare_execute_metric_gate PRIVATE
  cxx_std_23)
target_compile_definitions(public_optimizer_prepare_execute_metric_gate PRIVATE
  SB_QOW_PLAN_API_SOURCE_FILE="${PROJECT_SOURCE_DIR}/src/engine/internal_api/query/plan_api.cpp"
)
target_link_libraries(public_optimizer_prepare_execute_metric_gate PRIVATE
  sb_engine_optimizer_contract_only
  sb_engine_internal_api
  sb_engine_executor
  sb_core_index
  sb_storage_page
)
add_test(
  NAME public_optimizer_prepare_execute_metric_gate
  COMMAND public_optimizer_prepare_execute_metric_gate
)
set_tests_properties(public_optimizer_prepare_execute_metric_gate PROPERTIES
  LABELS "public_release_correctness;optimizer;prepare;execute;prepare_metrics;dependency_chain;metric_receipts;MGA"
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
  NAME public_durable_codec_fuzz_coverage_inventory
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../fuzz/public_durable_codec_fuzz_coverage_inventory.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_durable_codec_fuzz_coverage_inventory/public_durable_codec_fuzz_coverage_inventory.json
)
set_tests_properties(public_durable_codec_fuzz_coverage_inventory PROPERTIES
  LABELS "public_release_correctness;PCR-GATE-114;PCR-114;ELER-104;ELER-GATE-104;durable_codec;evidence_inventory;source_token_check;non_behavioral;property;bounds;checksum;version;authority_refusal;fail_closed"
)

add_test(
  NAME public_crash_fault_source_contract_matrix
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../fault_injection/public_crash_fault_source_contract_matrix.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_crash_fault_source_contract_matrix/public_crash_fault_source_contract_matrix.json
)
set_tests_properties(public_crash_fault_source_contract_matrix PROPERTIES
  LABELS "public_release_correctness;public_crash_fault_source_contract_matrix;PCR-GATE-115;PCR-115;ELER-080;ELER-GATE-080;source_contract;source_token_check;non_behavioral;mga_authority;fail_closed"
)

add_test(
  NAME public_release_soak_coverage_inventory
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../soak/public_release_soak_coverage_inventory.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --output ${CMAKE_CURRENT_BINARY_DIR}/public_release_soak_coverage_inventory/public_release_soak_coverage_inventory.json
)
set_tests_properties(public_release_soak_coverage_inventory PROPERTIES
  LABELS "public_release_correctness;PCR-GATE-116;PCR-116;ELER-081;ELER-GATE-081;ELER-102;ELER-GATE-102;evidence_inventory;source_token_check;non_behavioral;concurrency;diagnostics;fail_closed"
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
  LABELS "public_release_correctness;public_optimizer_implementation_matrix_gate;PCR-GATE-060;PCR-060;optimizer;model_family;native_optimization;exact_fallback;implementation_matrix;plan_cache;explain;benchmark_clean;runtime_evidence;binary_uuid"
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
  LABELS "public_release_correctness;public_optimizer_hints_calibration_feedback_gate;PCR-GATE-062;PCR-062;optimizer;hints;calibration;statistics_lifecycle;feedback;explain_lineage;binary_uuid"
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
  LABELS "public_release_correctness;public_optimizer_expression_plan_cache_validation_gate;PCR-GATE-063;PCR-063;ELER-104;ELER-GATE-104;ELER-106;ELER-GATE-106;optimizer;expression_normalization;plan_cache;compatibility_epoch;validation_hardening;sblr_uuid_mga;optimizer_property;binary_uuid"
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
  LABELS "public_release_correctness;public_row_level_traceability_manifest_gate;ELER-084;ELER-GATE-084;release_hygiene;row_level_traceability_manifest;declared_feature_traceability;reference_sblr_traceability;linux_proof"
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
  LABELS "public_release_correctness;public_sbsql_parser_sync_gate;ELER-089;ELER-GATE-089;release_hygiene;sbsql_parser_spec_execution_plan_sync;row_expanded_sync;compatibility_dialect_paste_through_rejection;linux_proof"
)

add_test(
  NAME public_sanitizer_result_contract_test
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/public_sanitizer_result_contract_test.py
)
set_tests_properties(public_sanitizer_result_contract_test PROPERTIES
  LABELS "public_release_correctness;sanitizer;qualification_integrity;fail_closed;linux_proof"
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
            --c-compiler "${SB_PUBLIC_RELEASE_HARDENING_C_COMPILER}"
            --cxx-compiler "${SB_PUBLIC_RELEASE_HARDENING_CXX_COMPILER}"
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

# Nested configurations retain dependency locations, never cached probe results.
add_test(
  NAME public_nested_dependency_config_test
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/public_nested_dependency_config_test.py
)
set_tests_properties(public_nested_dependency_config_test PROPERTIES
  LABELS "public_release_correctness;release_hygiene;dependency_configuration;source_archive_safe;linux_proof"
  ENVIRONMENT "SB_TEST_CMAKE_COMMAND=${CMAKE_COMMAND}"
)

# PUBLIC_PROJECT_EXPORT_COPY_FILTER_TEST
add_test(
  NAME public_project_export_copy_filter_test
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/public_project_export_copy_filter_test.py
          --work-root ${CMAKE_CURRENT_BINARY_DIR}/public_project_export_copy_filter_test
)
set_tests_properties(public_project_export_copy_filter_test PROPERTIES
  LABELS "public_release_correctness;public_project_export_copy_filter_test;PCR-GATE-001;release_hygiene;project_only_export;reference_acquisition_boundary;source_archive_safe;linux_proof"
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

# CRUD_COMPATIBILITY_BOUNDARY_GATE
add_test(
  NAME crud_compatibility_boundary_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/crud_compatibility_boundary_gate.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
)
set_tests_properties(crud_compatibility_boundary_gate PROPERTIES
  LABELS "public_release_correctness;crud_compatibility_boundary_gate;dml;MGA;compatibility;source_policy"
)

# DML_FULL_STATE_LOAD_BOUNDARY_GATE
add_test(
  NAME dml_full_state_load_boundary_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/dml_full_state_load_boundary_gate.py
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
)
set_tests_properties(dml_full_state_load_boundary_gate PROPERTIES
  LABELS "public_release_correctness;dml_full_state_load_boundary_gate;dml;MGA;performance;source_policy"
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
          ${CMAKE_CURRENT_BINARY_DIR}/public_secure_defaults_gate/public_secure_defaults_effective_config.json
)
set_tests_properties(public_secure_defaults_gate PROPERTIES
  LABELS "public_release_correctness;public_secure_defaults_gate;PCR-GATE-123;PCR-123;ELER-108;ELER-GATE-108;release_hygiene;secure_defaults;startup;config;security;memory;permissions;cluster_boundary;fail_closed;release_artifact_trust;linux_proof"
)

# PUBLIC_UNSUPPORTED_FEATURE_GATE
add_test(NAME public_release_contract_oracle_regression
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/release_contract_oracle_regression.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set_tests_properties(public_release_contract_oracle_regression PROPERTIES
  LABELS "public_release_correctness;release_hygiene;source_oracle;negative_controls"
  TIMEOUT 60)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME public_short_socket_workspace_regression
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/short_socket_workspace_regression.py
            --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
  set_tests_properties(public_short_socket_workspace_regression PROPERTIES
    LABELS "public_release_correctness;linux_proof;unix_socket;test_harness"
    TIMEOUT 60)
endif()

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
  LABELS "public_release_correctness;public_observability_schema_gate;PCR-GATE-137;PCR-137;ELER-107;ELER-GATE-107;observability;metrics;events;traces;health;redaction;stable_schema;memory;storage;transactions;index;optimizer;agents;backup;archive;security;repair;cluster_boundary;support_bundle;fail_closed;operational_readiness"
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
  LABELS "public_release_correctness;public_performance_baseline_gate;PCR-GATE-138;PCR-138;performance;capacity;baseline;p50;p95;p99;benchmark_clean;no_reference_dominance;memory;transactions;backup;restore;archive;support_bundle"
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
  LABELS "public_release_correctness;public_admin_runbook_gate;PCR-GATE-140;PCR-140;ELER-107;ELER-GATE-107;ELER-108;ELER-GATE-108;ELER-109;ELER-GATE-109;runbooks;admin_docs;operations;create;open;config;security;backup;restore;verify;repair;memory_pressure;sweep;archive;diagnostics;unsupported_features;upgrade;release_artifact_trust;enterprise_documentation;operational_readiness;linux_proof"
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

# PUBLIC_REFERENCE_MATERIAL_BOUNDARY_GATE
add_test(
  NAME public_reference_material_boundary_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_reference_material_boundary_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
)
set_tests_properties(public_reference_material_boundary_gate PROPERTIES
  LABELS "public_release_correctness;public_reference_material_boundary_gate;PCR-GATE-157;PCR-157;reference_material;source_boundary;documentation_boundary;reference_source_absent;external_payloads_untracked;fail_closed"
)

# INVESTOR_READINESS_SURFACE_GATE
add_test(
  NAME investor_readiness_surface_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/investor_readiness_surface_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --project-root ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --output ${CMAKE_CURRENT_BINARY_DIR}/investor_readiness_surface_gate/investor_readiness_surface_gate.json
)
set_tests_properties(investor_readiness_surface_gate PROPERTIES
  LABELS "public_release_correctness;investor_readiness_surface_gate;PCR-GATE-158;PCR-158;investor_readiness;release_hygiene;claims_boundary;driver_release_bucket;benchmark_no_claim;reference_fixture_boundary;fail_closed"
)

# SPDX_LICENSE_SURFACE_GATE
add_test(
  NAME spdx_license_surface_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/spdx_license_surface_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --output ${CMAKE_CURRENT_BINARY_DIR}/spdx_license_surface_gate/spdx_license_surface_gate.json
)
set_tests_properties(spdx_license_surface_gate PROPERTIES
  LABELS "public_release_correctness;spdx_license_surface_gate;PCR-GATE-159;PCR-159;license;spdx;third_party_notices;sbom;release_hygiene;fail_closed"
)

# PUBLIC_WEBSERVER_PACKAGE_EXPORT_GATE
add_test(
  NAME web_distribution_bundle_test
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/web_distribution_bundle_test.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
)
set_tests_properties(web_distribution_bundle_test PROPERTIES
  LABELS "public_release_correctness;web_distribution_bundle_test;PCR-GATE-160;PCR-160;installers;webserver_export;artifact_manifest;checksums;native_server_only;no_client_payload;dbeaver_excluded;no_github_release;fail_closed"
)

add_test(
  NAME prerelease_bundle_client_boundary_test
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/prerelease_bundle_client_boundary_test.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
)
set_tests_properties(prerelease_bundle_client_boundary_test PROPERTIES
  LABELS "public_release_correctness;prerelease_bundle;manual_route;native_server_only;no_client_payload;dbeaver_excluded;fail_closed"
)

add_test(
  NAME llvm_runtime_release_contract_test
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/llvm_runtime_release_contract_test.py
)
set_tests_properties(llvm_runtime_release_contract_test PROPERTIES
  LABELS "public_release_correctness;llvm_runtime_library;installers;runtime_dependencies;fail_closed"
)

add_test(
  NAME macos_universal_llvm_profile_test
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/macos_universal_llvm_profile_test.py
)
set_tests_properties(macos_universal_llvm_profile_test PROPERTIES
  LABELS "public_release_correctness;llvm_runtime_library;installers;macos;universal;archive_safety;fail_closed"
)

if(NOT WIN32)
  add_test(
    NAME macos_system_installer_smoke
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/installers/smoke_install_macos_system.py
            --work-root ${CMAKE_CURRENT_BINARY_DIR}/macos_system_installer_smoke
  )
  set_tests_properties(macos_system_installer_smoke PROPERTIES
    LABELS "public_release_correctness;installers;macos;identity;fail_closed"
  )
endif()

add_test(
  NAME windows_portable_release_workflow_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/installers/windows/verify_windows_system_installer.py
)
set_tests_properties(windows_portable_release_workflow_gate PROPERTIES
  LABELS "public_release_correctness;installers;windows;portable_zip;manual_msi_tooling;workflow;fail_closed"
)

add_test(
  NAME nightly_release_bundle_test
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/nightly_release_bundle_test.py
)
set_tests_properties(nightly_release_bundle_test PROPERTIES
  LABELS "public_release_correctness;nightly_release;installers;native_sbsql;llvm_runtime_library;fail_closed"
)

add_test(
  NAME textual_failure_proof_test
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/textual_failure_proof_test.py
)
set_tests_properties(textual_failure_proof_test PROPERTIES
  LABELS "public_release_correctness;installers;github_actions;failure_proof;no_binary_payload;fail_closed"
)

add_test(
  NAME native_release_bundle_test
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/native_release_bundle_test.py
)
set_tests_properties(native_release_bundle_test PROPERTIES
  LABELS "public_release_correctness;native_release_bundle;installers;native_sbsql;llvm_runtime_library;fail_closed"
)

add_test(
  NAME standalone_sbparser_graph_contract_test
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/standalone_sbparser_graph_contract_test.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --cmake ${CMAKE_COMMAND}
)
set_tests_properties(standalone_sbparser_graph_contract_test PROPERTIES
  LABELS "public_release_correctness;native_sbsql;parser_boundary;standalone_parser;build_graph;fail_closed"
)

add_test(
  NAME rolling_nightly_publisher_test
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/rolling_nightly_publisher_test.py
)
set_tests_properties(rolling_nightly_publisher_test PROPERTIES
  LABELS "public_release_correctness;nightly_release;github_release;publication;fail_closed"
)

add_test(
  NAME stage_driver_beta_artifacts_test
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/stage_driver_beta_artifacts_test.py
)
set_tests_properties(stage_driver_beta_artifacts_test PROPERTIES
  LABELS "driver_release;metadata_snapshot;fail_closed"
)

# PUBLIC_CONFIG_POLICY_MIGRATION_GATE
add_test(
  NAME native_qa_bootstrap_test
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/native_qa_bootstrap_test.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
)
set_tests_properties(native_qa_bootstrap_test PROPERTIES
  LABELS "public_release_correctness;native_qa_bootstrap_test;installers;operational_readiness;native_sbsql;tls;dbbt;secure_bootstrap;fail_closed"
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
  LABELS "public_release_correctness;public_config_policy_migration_gate;PCR-GATE-147;PCR-147;ELER-107;ELER-GATE-107;config;policy;migration;policy_pack;versioning;diff;export;import;post_create_filesystem_refusal;admin_diagnostics;mga_catalog_commit;fail_closed;operational_readiness"
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
  NAME public_generated_ai_reference_provenance_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_generated_ai_reference_provenance_gate.py
          --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
          --csv-output ${CMAKE_CURRENT_BINARY_DIR}/public_generated_ai_reference_provenance_gate/public_generated_ai_reference_provenance_matrix.csv
          --evidence-output ${CMAKE_CURRENT_BINARY_DIR}/public_generated_ai_reference_provenance_gate/public_generated_ai_reference_provenance_gate.json
)
set_tests_properties(public_generated_ai_reference_provenance_gate PROPERTIES
  LABELS "public_release_correctness;public_generated_ai_reference_provenance_gate;PCR-GATE-154;PCR-154;provenance;generated_code;ai_assisted;reference_derived;copied_reference;public_inputs;human_review;authority_boundary;fail_closed"
)

if(NOT SB_PUBLIC_RELEASE_EXPORT_NESTED)
  add_test(
    NAME public_project_export_gate
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/release/public_project_export_gate.py
            --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/../../..
            --staging-root ${CMAKE_CURRENT_BINARY_DIR}/public_project_export_gate
            --dependency-cache "${CMAKE_BINARY_DIR}/CMakeCache.txt"
            --cmake ${CMAKE_COMMAND}
            --ctest ${CMAKE_CTEST_COMMAND}
            --c-compiler ${CMAKE_C_COMPILER}
            --cxx-compiler ${CMAKE_CXX_COMPILER}
            --hardening-c-compiler "${SB_PUBLIC_RELEASE_HARDENING_C_COMPILER}"
            --hardening-cxx-compiler "${SB_PUBLIC_RELEASE_HARDENING_CXX_COMPILER}"
            --parallel ${SB_PUBLIC_RELEASE_EXPORT_BUILD_PARALLELISM}
  )
  # PUBLIC_RELEASE_CLEANUP_GATE
  set_tests_properties(public_project_export_gate PROPERTIES
    LABELS "public_release_correctness;public_project_export_gate;PCR-GATE-001;PCR-GATE-005;PCR-001;PCR-002;PCR-005;ELER-108;ELER-GATE-108;release_hygiene;project_only_export;cleanup_manifest;private_reference_scan;release_artifact_trust;linux_proof"
    TIMEOUT 7200
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
  public_sbsql_language_release_channel_gate
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
  public_driver_test_database_seed
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
