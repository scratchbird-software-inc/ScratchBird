# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if(NOT TARGET sb_core_memory)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/memory ${CMAKE_CURRENT_BINARY_DIR}/core_memory)
endif()
if(NOT TARGET sb_core_metrics)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/metrics ${CMAKE_CURRENT_BINARY_DIR}/core_metrics)
endif()

add_library(sb_transaction_mga
  cluster_transaction_fail_closed.cpp
  copy_on_write.cpp
  current_row_map.cpp
  historical_snapshot_locator.cpp
  isolation.cpp
  lock_wait_lifecycle.cpp
  optimizer_mga_pressure_metrics.cpp
  page_finality_evidence.cpp
  row_version.cpp
  savepoint.cpp
  table_content_generation.cpp
  transaction_cleanup.cpp
  transaction_cleanup_horizon_service.cpp
  transaction_evidence.cpp
  transaction_horizon.cpp
  transaction_memory_governance.cpp
  transaction_memory_hooks.cpp
  transaction_manager.cpp
  transaction_metrics.cpp
  transaction_policy.cpp
  transaction_prepare.cpp
  transaction_recovery.cpp
  transaction_lock.cpp
  transaction_snapshot.cpp
  transaction_support_services.cpp
  transaction_inventory.cpp
  transaction_state.cpp
  visibility_status_cache.cpp
)

target_compile_features(sb_transaction_mga PUBLIC cxx_std_17)
target_include_directories(sb_transaction_mga PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sb_transaction_mga PUBLIC sb_core_platform sb_core_uuid sb_core_memory sb_core_metrics)

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(Threads REQUIRED)
  add_executable(sbsql_sblr_alignment_snapshot_retention
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/sbsql_sblr_alignment/snapshot_retention_test.cpp)
  target_link_libraries(sbsql_sblr_alignment_snapshot_retention PRIVATE sb_transaction_mga Threads::Threads)
  add_test(NAME sbsql_sblr_alignment_snapshot_retention
    COMMAND sbsql_sblr_alignment_snapshot_retention)
  set_tests_properties(sbsql_sblr_alignment_snapshot_retention PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-06;MGA;binary_uuid;retained_snapshot;component_regression;not_runtime_acceptance"
    TIMEOUT 60)
endif()
