# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if(NOT TARGET sb_core_metrics)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../metrics ${CMAKE_CURRENT_BINARY_DIR}/core_metrics)
endif()

add_library(sb_core_memory
  background_memory_reclamation.cpp
  foreign_memory_reservation.cpp
  hierarchical_memory_budget_ledger.cpp
  llvm_memory_accounting.cpp
  memory.cpp
  memory_adaptive_batch_working_set.cpp
  memory_class_policy_lease.cpp
  memory_metadata_compatibility.cpp
  memory_fairness_scheduler.cpp
  memory_fragmentation_profiler.cpp
  memory_locality_policy.cpp
  memory_observability_overhead.cpp
  memory_policy_config.cpp
  memory_reserved_category_claims.cpp
  memory_support_bundle.cpp
  memory_pressure_response.cpp
  plugin_native_memory_sandbox.cpp
  query_memory_arena.cpp
  reservation_backed_memory_resource.cpp
  result_cursor_plan_memory_governance.cpp
  sharded_memory_accounting_ledger.cpp
  temp_workspace_lifecycle.cpp
  thread_local_memory_cache.cpp
  typed_slab_pool.cpp
)

target_compile_features(sb_core_memory PUBLIC cxx_std_17)
target_include_directories(sb_core_memory PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sb_core_memory PUBLIC sb_core_platform sb_core_metrics)
if(WIN32)
  target_link_libraries(sb_core_memory PUBLIC bcrypt)
endif()
