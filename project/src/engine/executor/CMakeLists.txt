# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if(NOT TARGET sb_storage_page)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../storage/page ${CMAKE_CURRENT_BINARY_DIR}/storage_page)
endif()
if(NOT TARGET sb_core_index)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/index ${CMAKE_CURRENT_BINARY_DIR}/core_index)
endif()
if(NOT TARGET sb_core_memory)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/memory ${CMAKE_CURRENT_BINARY_DIR}/core_memory)
endif()
if(NOT TARGET sb_core_bulk_load)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/bulk_load ${CMAKE_CURRENT_BINARY_DIR}/core_bulk_load)
endif()
if(NOT TARGET sb_core_resource_governance)
  add_library(sb_core_resource_governance
    ${CMAKE_CURRENT_SOURCE_DIR}/../../core/agents/resource_governance_admission.cpp
  )
  target_compile_features(sb_core_resource_governance PUBLIC cxx_std_17)
  target_include_directories(sb_core_resource_governance PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../core/agents
  )
  target_link_libraries(sb_core_resource_governance PUBLIC sb_core_platform sb_core_metrics)
endif()
if(NOT TARGET sb_engine_gpu_acceleration)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../gpu_acceleration ${CMAKE_CURRENT_BINARY_DIR}/engine_gpu_acceleration)
endif()

add_library(sb_engine_executor STATIC
  aggregate_executor.cpp
  allocator_lifetime_hot_path.cpp
  batch_point_lookup_executor.cpp
  buffer_prefetch_route_executor.cpp
  bulk_executor.cpp
  candidate_set_executor.cpp
  cpu_cache_layout_observability.cpp
  cte_executor.cpp
  descriptor_value_runtime.cpp
  dml_executor.cpp
  explain_executor.cpp
  executor_batching.cpp
  executor_batch_failure_policy.cpp
  executor_batch_join_dml.cpp
  executor_batch_relational.cpp
  executor_foundation.cpp
  executor_operator_metrics.cpp
  filter_executor.cpp
  indexed_physical_operator.cpp
  join_executor.cpp
  late_materialization_covering_scan_runtime.cpp
  late_materialization_executor.cpp
  operator_memory_grant.cpp
  operator_typed_arena_work_area.cpp
  operator_fusion_executor.cpp
  parallel_physical_pipeline.cpp
  prepared_execution_template.cpp
  projection_executor.cpp
  query_memory_arena_executor.cpp
  reservation_backed_executor_memory_bridge.cpp
  row_ordinal_locator.cpp
  runtime_filter_executor.cpp
  scan_executor.cpp
  scoring_kernel_executor.cpp
  security_barrier_executor.cpp
  snapshot_safe_result_cache.cpp
  sort_limit_executor.cpp
  subquery_executor.cpp
  table_function_executor.cpp
  temp_spill_executor.cpp
  typed_slab_executor_work_area.cpp
  vectorized_result_batch.cpp
  window_executor.cpp
)

target_compile_features(sb_engine_executor PUBLIC cxx_std_20)

target_include_directories(sb_engine_executor PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../internal_api
  ${CMAKE_CURRENT_SOURCE_DIR}/../optimizer
)

target_link_libraries(sb_engine_executor PUBLIC sb_storage_page)
target_link_libraries(sb_engine_executor PUBLIC sb_core_index)
target_link_libraries(sb_engine_executor PUBLIC sb_core_memory)
target_link_libraries(sb_engine_executor PUBLIC sb_core_bulk_load)
target_link_libraries(sb_engine_executor PUBLIC sb_core_resource_governance)
target_link_libraries(sb_engine_executor PUBLIC sb_engine_gpu_acceleration)
