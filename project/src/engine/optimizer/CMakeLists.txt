# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if(NOT TARGET sb_engine_plan_shapes)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../planner ${CMAKE_CURRENT_BINARY_DIR}/engine_planner)
endif()
if(NOT TARGET sb_core_index)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/index ${CMAKE_CURRENT_BINARY_DIR}/core_index)
endif()
if(NOT TARGET sb_core_agents)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/agents ${CMAKE_CURRENT_BINARY_DIR}/core_agents)
endif()
if(NOT TARGET sb_core_memory)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/memory ${CMAKE_CURRENT_BINARY_DIR}/core_memory)
endif()
if(NOT TARGET sb_storage_page)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../storage/page ${CMAKE_CURRENT_BINARY_DIR}/storage_page)
endif()
if(NOT TARGET sb_engine_nosql_provider_contract)
  add_library(sb_engine_nosql_provider_contract
    ${CMAKE_CURRENT_SOURCE_DIR}/../internal_api/nosql/nosql_physical_provider_contract.cpp
  )
  target_include_directories(sb_engine_nosql_provider_contract PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../internal_api
  )
  target_compile_features(sb_engine_nosql_provider_contract PUBLIC cxx_std_20)
  target_link_libraries(sb_engine_nosql_provider_contract PUBLIC sb_core_index)
endif()

if(NOT TARGET sb_engine_optimizer_contract_only)
  add_library(sb_engine_optimizer_contract_only
    adaptive_cardinality_feedback.cpp
    optimizer_adaptive_feedback_enterprise.cpp
    agent_optimizer_recommendation_bridge.cpp
    adaptive_batch_controller.cpp
    access_path.cpp
    access_path_full.cpp
    cluster_candidate.cpp
    cluster_projection_authority_guard.cpp
    cluster_refusal_path.cpp
    cost_model.cpp
    join_planner.cpp
    join_planner_full.cpp
    mga_page_finality_evidence.cpp
    mga_visibility_status_cache_evidence.cpp
    optimizer_barrier.cpp
    optimizer_benchmark_evidence_schema.cpp
    optimizer_correctness_oracle.cpp
    optimizer_crash_reopen_persistence.cpp
    optimizer_catalog_backed_planning.cpp
    optimizer_contract.cpp
    optimizer_cost_full.cpp
    optimizer_differential_fuzz.cpp
    optimizer_driver_explain_compatibility.cpp
    optimizer_explain.cpp
    optimizer_feedback.cpp
    optimizer_enterprise_manifest.cpp
    optimizer_feedback_stability.cpp
    optimizer_graph_document_costing_enterprise.cpp
    optimizer_hint_policy.cpp
    bulk_placement_order.cpp
    optimizer_hot_point_lookup.cpp
    optimizer_index_costing.cpp
    optimizer_join_controls_enterprise.cpp
    optimizer_live_donor_comparison_artifacts.cpp
    optimizer_memory_feedback_bridge.cpp
    optimizer_memory_spill_feedback_enterprise.cpp
    optimizer_metric_manifest.cpp
    nosql_approx_filter_decision.cpp
    nosql_statistics_advisor.cpp
    optimizer_plan_cache.cpp
    optimizer_production_analyze.cpp
    optimizer_relational_operator_enterprise.cpp
    predicate_normalization.cpp
    optimizer_request.cpp
    optimizer_rewrite.cpp
    reservation_backed_optimizer_memory_bridge.cpp
    optimizer_route_metrics.cpp
    optimizer_runtime_filter_prefetch_enterprise.cpp
    optimizer_safety_gates.cpp
    optimizer_selectivity_drift_observability.cpp
    optimizer_specialized_fusion_enterprise.cpp
    optimizer_statistics_full.cpp
    optimizer_statistics_invalidation.cpp
    optimizer_statistics_lifecycle.cpp
    optimizer_text_wand_costing_enterprise.cpp
    optimizer_transformation_memo_coverage.cpp
    optimizer_typed_arena_work_area.cpp
    optimizer_vector_ann_costing_enterprise.cpp
    optimizer_workload_regression_budget.cpp
    partition_segment_pruning.cpp
    physical_plan.cpp
    physical_plan_prefetch.cpp
    relational_planner.cpp
    runtime_consumption_benchmark_evidence.cpp
    runtime_consumption_evidence.cpp
    runtime_filter_pushdown.cpp
    selectivity_model.cpp
    specialized_planner.cpp
    specialized_workload_metrics.cpp
    statistics_catalog.cpp
    typed_slab_planner_work_area.cpp
  )

  target_include_directories(sb_engine_optimizer_contract_only PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../internal_api
  )

  target_compile_features(sb_engine_optimizer_contract_only PUBLIC cxx_std_20)

  target_link_libraries(sb_engine_optimizer_contract_only PUBLIC
    sb_core_agents
    sb_core_index
    sb_core_memory
    sb_engine_plan_shapes
    sb_engine_nosql_provider_contract
    sb_storage_page
  )
endif()

if(NOT TARGET sb_engine_optimizer)
  add_library(sb_engine_optimizer INTERFACE)

  target_link_libraries(sb_engine_optimizer INTERFACE
    sb_engine_optimizer_contract_only
  )
  if(TARGET sb_engine_planner)
    target_link_libraries(sb_engine_optimizer INTERFACE
      sb_engine_planner
    )
  endif()
endif()
