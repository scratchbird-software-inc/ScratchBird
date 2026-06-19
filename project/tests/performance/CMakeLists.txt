# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(NOT TARGET sb_engine_executor)
  add_subdirectory("${PROJECT_SOURCE_DIR}/src/engine/executor" "${CMAKE_CURRENT_BINARY_DIR}/engine_executor")
endif()

set(SB_OPTIMIZER_DEFICIENCY_BATCH_A_FIXTURE
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/optimizer_deficiency_batch_a/optimizer_deficiency_batch_a_fixture.json"
)

add_test(
  NAME optimizer_deficiency_batch_a_gate
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/optimizer_deficiency_batch_a_gate.py"
          --fixture "${SB_OPTIMIZER_DEFICIENCY_BATCH_A_FIXTURE}"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --build-root "${CMAKE_BINARY_DIR}"
          --output-dir "${CMAKE_CURRENT_BINARY_DIR}/optimizer_deficiency_batch_a_gate"
)
set_tests_properties(optimizer_deficiency_batch_a_gate PROPERTIES
  LABELS "optimizer_deficiency_batch_a_gate;ODF-000;ODF-001;ODF-002;ODF-003;ODF-004;ODF-005;ODF-006;ODF-007;ODF-008;ODF-009;optimizer_deficiency;performance;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odfr_001_gate
  optimizer_deficiency_odfr_001_gate.cpp
)
target_compile_features(optimizer_deficiency_odfr_001_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odfr_001_gate
  PRIVATE
    sb_engine_internal_api
)
add_test(
  NAME optimizer_deficiency_odfr_001_gate
  COMMAND optimizer_deficiency_odfr_001_gate
)
set_tests_properties(optimizer_deficiency_odfr_001_gate PROPERTIES
  LABELS "optimizer_deficiency_odfr_001_gate;ODFR-001;optimizer_deficiency;performance;benchmark_clean;profiler_attribution;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odfr_002_gate
  optimizer_deficiency_odfr_002_gate.cpp
)
target_compile_features(optimizer_deficiency_odfr_002_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odfr_002_gate
  PRIVATE
    sb_core_metrics
)
add_test(
  NAME optimizer_deficiency_odfr_002_gate
  COMMAND optimizer_deficiency_odfr_002_gate
)
set_tests_properties(optimizer_deficiency_odfr_002_gate PROPERTIES
  LABELS "optimizer_deficiency_odfr_002_gate;ODFR-002;optimizer_deficiency;performance;contention_telemetry;support_bundle;mga_transaction_regression"
)

add_test(
  NAME optimizer_deficiency_odf_119_gate
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/optimizer_deficiency_odf_119_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --output-dir "${CMAKE_CURRENT_BINARY_DIR}/optimizer_deficiency_odf_119_gate_evidence"
)
set_tests_properties(optimizer_deficiency_odf_119_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_119_gate;ODF-119;optimizer_deficiency;performance;benchmark;benchmark_clean;variance;reproducibility;mga_transaction_regression"
)

add_executable(
  memory_live_route_arena_gate
  memory_live_route_arena_gate.cpp
)
target_compile_features(memory_live_route_arena_gate PRIVATE cxx_std_20)
target_link_libraries(
  memory_live_route_arena_gate
  PRIVATE
    sb_core_memory
    sb_engine_executor
)
add_test(
  NAME memory_live_route_arena_gate
  COMMAND memory_live_route_arena_gate
)
set_tests_properties(memory_live_route_arena_gate PROPERTIES
  LABELS "memory_live_route_arena_gate;MMCH-021;memory;executor;route_evidence;performance;benchmark_clean;mga_transaction_regression"
)

add_executable(
  memory_locality_hugepage_policy_gate
  memory_locality_hugepage_policy_gate.cpp
)
target_compile_features(memory_locality_hugepage_policy_gate PRIVATE cxx_std_20)
target_link_libraries(
  memory_locality_hugepage_policy_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME memory_locality_hugepage_policy_gate
  COMMAND memory_locality_hugepage_policy_gate
)
set_tests_properties(memory_locality_hugepage_policy_gate PROPERTIES
  LABELS "memory_locality_hugepage_policy_gate;MMCH-062;MMCH_NUMA_HUGEPAGE_LOCALITY_POLICY;memory;numa;huge_pages;performance;benchmark_clean;mga_transaction_regression"
)

add_executable(
  memory_observability_overhead_budget_gate
  memory_observability_overhead_budget_gate.cpp
)
target_compile_features(memory_observability_overhead_budget_gate PRIVATE cxx_std_20)
target_link_libraries(
  memory_observability_overhead_budget_gate
  PRIVATE
    sb_core_memory
    sb_core_platform
)
add_test(
  NAME memory_observability_overhead_budget_gate
  COMMAND memory_observability_overhead_budget_gate
)
set_tests_properties(memory_observability_overhead_budget_gate PROPERTIES
  LABELS "memory_observability_overhead_budget_gate;MMCH-083;MMCH_MEMORY_OBSERVABILITY_OVERHEAD_BUDGET;ELER-105;ELER-GATE-105;memory;observability;performance;performance_scalability_baseline;resource_high_water;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_memory_feedback_evidence_bridge_gate
  optimizer_memory_feedback_evidence_bridge_gate.cpp
)
target_compile_features(optimizer_memory_feedback_evidence_bridge_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_memory_feedback_evidence_bridge_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_memory_feedback_evidence_bridge_gate
  COMMAND optimizer_memory_feedback_evidence_bridge_gate
)
set_tests_properties(optimizer_memory_feedback_evidence_bridge_gate PROPERTIES
  LABELS "optimizer_memory_feedback_evidence_bridge_gate;MMCH-091;MMCH_OPTIMIZER_MEMORY_FEEDBACK_EVIDENCE_BRIDGE;memory;optimizer;feedback;performance;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_batch_b_part1_gate
  optimizer_deficiency_batch_b_part1_gate.cpp
)
target_compile_features(optimizer_deficiency_batch_b_part1_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_batch_b_part1_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_deficiency_batch_b_part1_gate
  COMMAND optimizer_deficiency_batch_b_part1_gate
)
set_tests_properties(optimizer_deficiency_batch_b_part1_gate PROPERTIES
  LABELS "optimizer_deficiency_batch_b_part1_gate;ODF-010;ODF-011;ODF-012;ODF-013;ODF-014;optimizer_deficiency;performance;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odfr_010_gate
  optimizer_deficiency_odfr_010_gate.cpp
)
target_compile_features(optimizer_deficiency_odfr_010_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odfr_010_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_deficiency_odfr_010_gate
  COMMAND optimizer_deficiency_odfr_010_gate
)
set_tests_properties(optimizer_deficiency_odfr_010_gate PROPERTIES
  LABELS "optimizer_deficiency_odfr_010_gate;ODFR-010;optimizer_deficiency;performance;extended_statistics;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odfr_030_gate
  optimizer_deficiency_odfr_030_gate.cpp
)
target_compile_features(optimizer_deficiency_odfr_030_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odfr_030_gate
  PRIVATE
    sb_core_index
    sb_engine_optimizer_contract_only
    sb_wire_result_batch
)
add_test(
  NAME optimizer_deficiency_odfr_030_gate
  COMMAND optimizer_deficiency_odfr_030_gate
)
set_tests_properties(optimizer_deficiency_odfr_030_gate PROPERTIES
  LABELS "optimizer_deficiency_odfr_030_gate;ODFR-030;optimizer_deficiency;performance;compression_policy;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odfr_040_gate
  optimizer_deficiency_odfr_040_gate.cpp
)
target_compile_features(optimizer_deficiency_odfr_040_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odfr_040_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME optimizer_deficiency_odfr_040_gate
  COMMAND optimizer_deficiency_odfr_040_gate
)
set_tests_properties(optimizer_deficiency_odfr_040_gate PROPERTIES
  LABELS "optimizer_deficiency_odfr_040_gate;ODFR-040;optimizer_deficiency;performance;vector_recall_lifecycle;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odfr_050_gate
  optimizer_deficiency_odfr_050_gate.cpp
)
target_compile_features(optimizer_deficiency_odfr_050_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odfr_050_gate
  PRIVATE
    sb_wire_result_batch
)
add_test(
  NAME optimizer_deficiency_odfr_050_gate
  COMMAND optimizer_deficiency_odfr_050_gate
)
set_tests_properties(optimizer_deficiency_odfr_050_gate PROPERTIES
  LABELS "optimizer_deficiency_odfr_050_gate;ODFR-050;optimizer_deficiency;performance;streaming_result_window;result_frame;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odfr_051_gate
  optimizer_deficiency_odfr_051_gate.cpp
)
target_compile_features(optimizer_deficiency_odfr_051_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odfr_051_gate
  PRIVATE
    sb_engine_executor
)
add_test(
  NAME optimizer_deficiency_odfr_051_gate
  COMMAND optimizer_deficiency_odfr_051_gate
)
set_tests_properties(optimizer_deficiency_odfr_051_gate PROPERTIES
  LABELS "optimizer_deficiency_odfr_051_gate;ODFR-051;optimizer_deficiency;performance;snapshot_result_cache;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odfr_090_gate
  optimizer_deficiency_odfr_090_gate.cpp
)
target_compile_features(optimizer_deficiency_odfr_090_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odfr_090_gate
  PRIVATE
    sb_engine_internal_api
    sb_core_metrics
    sb_engine_optimizer
    sb_core_index
    sb_wire_result_batch
    sb_engine_executor
)
add_test(
  NAME optimizer_deficiency_odfr_090_gate
  COMMAND optimizer_deficiency_odfr_090_gate
)
set_tests_properties(optimizer_deficiency_odfr_090_gate PROPERTIES
  LABELS "optimizer_deficiency_odfr_090_gate;ODFR-090;optimizer_deficiency;performance;route_closure;support_bundle;benchmark_clean;mga_transaction_regression"
)

add_executable(
  index_runtime_capability_truthfulness_gate
  index_runtime_capability_truthfulness_gate.cpp
)
target_compile_features(index_runtime_capability_truthfulness_gate PRIVATE cxx_std_20)
target_link_libraries(
  index_runtime_capability_truthfulness_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME index_runtime_capability_truthfulness_gate
  COMMAND index_runtime_capability_truthfulness_gate
)
set_tests_properties(index_runtime_capability_truthfulness_gate PROPERTIES
  LABELS "index_runtime_capability_truthfulness_gate;IRC-000;IRC-001;IRC-002;IRC-003;ELER-105;ELER-GATE-105;index_runtime_correctness;index_lookup_runtime_truthfulness;performance_scalability_baseline;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  index_route_capability_gate
  index_route_capability_gate.cpp
)
target_compile_features(index_route_capability_gate PRIVATE cxx_std_20)
target_link_libraries(
  index_route_capability_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME index_route_capability_gate
  COMMAND index_route_capability_gate
)
set_tests_properties(index_route_capability_gate PROPERTIES
  LABELS "index_route_capability_gate;IRC-190;index_runtime_correctness;route_capability;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  index_catalog_management_metrics_surface_gate
  index_catalog_management_metrics_surface_gate.cpp
)
target_compile_features(index_catalog_management_metrics_surface_gate PRIVATE cxx_std_20)
target_link_libraries(
  index_catalog_management_metrics_surface_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME index_catalog_management_metrics_surface_gate
  COMMAND index_catalog_management_metrics_surface_gate
)
set_tests_properties(index_catalog_management_metrics_surface_gate PROPERTIES
  LABELS "index_catalog_management_metrics_surface_gate;IRC-170;index_runtime_correctness;catalog_management_metrics;support_bundle;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  index_family_validation_repair_framework_gate
  index_family_validation_repair_framework_gate.cpp
)
target_compile_features(index_family_validation_repair_framework_gate PRIVATE cxx_std_20)
target_link_libraries(
  index_family_validation_repair_framework_gate
  PRIVATE
    sb_core_index
    sb_core_uuid
)
add_test(
  NAME index_family_validation_repair_framework_gate
  COMMAND index_family_validation_repair_framework_gate
)
set_tests_properties(index_family_validation_repair_framework_gate PROPERTIES
  LABELS "index_family_validation_repair_framework_gate;IRC-180;index_runtime_correctness;family_validation_repair;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  indexed_physical_operator_integration_gate
  indexed_physical_operator_integration_gate.cpp
)
target_compile_features(indexed_physical_operator_integration_gate PRIVATE cxx_std_20)
target_link_libraries(
  indexed_physical_operator_integration_gate
  PRIVATE
    sb_engine_executor
    sb_core_index
    sb_storage_page
    sb_core_uuid
)
add_test(
  NAME indexed_physical_operator_integration_gate
  COMMAND indexed_physical_operator_integration_gate
)
set_tests_properties(indexed_physical_operator_integration_gate PROPERTIES
  LABELS "indexed_physical_operator_integration_gate;IRC-060;index_runtime_correctness;physical_index_operator;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  late_materialization_covering_scan_gate
  late_materialization_covering_scan_gate.cpp
)
target_compile_features(late_materialization_covering_scan_gate PRIVATE cxx_std_20)
target_link_libraries(
  late_materialization_covering_scan_gate
  PRIVATE
    sb_engine_executor
    sb_core_index
    sb_storage_page
    sb_core_uuid
)
add_test(
  NAME late_materialization_covering_scan_gate
  COMMAND late_materialization_covering_scan_gate
)
set_tests_properties(late_materialization_covering_scan_gate PROPERTIES
  LABELS "late_materialization_covering_scan_gate;IRC-061;index_runtime_correctness;late_materialization;covering_index_scan;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  index_plan_shape_regression_guard_gate
  index_plan_shape_regression_guard_gate.cpp
)
target_compile_features(index_plan_shape_regression_guard_gate PRIVATE cxx_std_20)
target_link_libraries(
  index_plan_shape_regression_guard_gate
  PRIVATE
    sb_engine_executor
    sb_core_index
    sb_storage_page
    sb_core_uuid
)
add_test(
  NAME index_plan_shape_regression_guard_gate
  COMMAND index_plan_shape_regression_guard_gate
)
set_tests_properties(index_plan_shape_regression_guard_gate PROPERTIES
  LABELS "index_plan_shape_regression_guard_gate;IRC-062;index_runtime_correctness;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  index_key_encoding_order_property_gate
  index_key_encoding_order_property_gate.cpp
)
target_compile_features(index_key_encoding_order_property_gate PRIVATE cxx_std_20)
target_link_libraries(
  index_key_encoding_order_property_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME index_key_encoding_order_property_gate
  COMMAND index_key_encoding_order_property_gate
)
set_tests_properties(index_key_encoding_order_property_gate PROPERTIES
  LABELS "index_key_encoding_order_property_gate;IRC-004;IRC-005;IRC-006;index_runtime_correctness;property_fuzz;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  expression_index_extractor_gate
  expression_index_extractor_gate.cpp
)
target_compile_features(expression_index_extractor_gate PRIVATE cxx_std_20)
target_link_libraries(
  expression_index_extractor_gate
  PRIVATE
    sb_core_index
    sb_core_uuid
)
add_test(
  NAME expression_index_extractor_gate
  COMMAND expression_index_extractor_gate
)
set_tests_properties(expression_index_extractor_gate PROPERTIES
  LABELS "expression_index_extractor_gate;IRC-030;index_runtime_correctness;expression_index;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  partial_index_implication_gate
  partial_index_implication_gate.cpp
)
target_compile_features(partial_index_implication_gate PRIVATE cxx_std_20)
target_link_libraries(
  partial_index_implication_gate
  PRIVATE
    sb_core_index
    sb_engine_optimizer
)
add_test(
  NAME partial_index_implication_gate
  COMMAND partial_index_implication_gate
)
set_tests_properties(partial_index_implication_gate PROPERTIES
  LABELS "partial_index_implication_gate;IRC-031;index_runtime_correctness;partial_index;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  covering_index_payload_gate
  covering_index_payload_gate.cpp
)
target_compile_features(covering_index_payload_gate PRIVATE cxx_std_20)
target_link_libraries(
  covering_index_payload_gate
  PRIVATE
    sb_core_index
    sb_engine_optimizer
)
add_test(
  NAME covering_index_payload_gate
  COMMAND covering_index_payload_gate
)
set_tests_properties(covering_index_payload_gate PROPERTIES
  LABELS "covering_index_payload_gate;IRC-032;index_runtime_correctness;covering_index_payload;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  index_btree_physical_page_gate
  index_btree_physical_page_gate.cpp
)
target_compile_features(index_btree_physical_page_gate PRIVATE cxx_std_20)
target_link_libraries(
  index_btree_physical_page_gate
  PRIVATE
    sb_storage_page
    sb_core_index
    sb_core_uuid
)
add_test(
  NAME index_btree_physical_page_gate
  COMMAND index_btree_physical_page_gate
)
set_tests_properties(index_btree_physical_page_gate PROPERTIES
  LABELS "index_btree_physical_page_gate;IRC-010;IRC-011;IRC-012;IRC-013;IRC-014;IRC-015;IRC-020;index_runtime_correctness;physical_btree;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  index_hash_physical_page_gate
  index_hash_physical_page_gate.cpp
)
target_compile_features(index_hash_physical_page_gate PRIVATE cxx_std_20)
target_link_libraries(
  index_hash_physical_page_gate
  PRIVATE
    sb_storage_page
    sb_core_uuid
)
add_test(
  NAME index_hash_physical_page_gate
  COMMAND index_hash_physical_page_gate
)
set_tests_properties(index_hash_physical_page_gate PROPERTIES
  LABELS "index_hash_physical_page_gate;IRC-080;index_runtime_correctness;physical_hash;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  index_hash_split_merge_compaction_gate
  index_hash_split_merge_compaction_gate.cpp
)
target_compile_features(index_hash_split_merge_compaction_gate PRIVATE cxx_std_20)
target_link_libraries(
  index_hash_split_merge_compaction_gate
  PRIVATE
    sb_storage_page
    sb_core_uuid
)
add_test(
  NAME index_hash_split_merge_compaction_gate
  COMMAND index_hash_split_merge_compaction_gate
)
set_tests_properties(index_hash_split_merge_compaction_gate PROPERTIES
  LABELS "index_hash_split_merge_compaction_gate;IRC-081;index_runtime_correctness;physical_hash;hash_split_merge_compaction;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  physical_zone_summary_gate
  physical_zone_summary_gate.cpp
)
target_compile_features(physical_zone_summary_gate PRIVATE cxx_std_20)
target_link_libraries(
  physical_zone_summary_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME physical_zone_summary_gate
  COMMAND physical_zone_summary_gate
)
set_tests_properties(physical_zone_summary_gate PROPERTIES
  LABELS "physical_zone_summary_gate;IRC-090;index_runtime_correctness;physical_zone_summary;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  physical_bloom_filter_gate
  physical_bloom_filter_gate.cpp
)
target_compile_features(physical_bloom_filter_gate PRIVATE cxx_std_20)
target_link_libraries(
  physical_bloom_filter_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME physical_bloom_filter_gate
  COMMAND physical_bloom_filter_gate
)
set_tests_properties(physical_bloom_filter_gate PROPERTIES
  LABELS "physical_bloom_filter_gate;IRC-091;index_runtime_correctness;physical_bloom_filter;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  physical_columnar_zone_gate
  physical_columnar_zone_gate.cpp
)
target_compile_features(physical_columnar_zone_gate PRIVATE cxx_std_20)
target_link_libraries(
  physical_columnar_zone_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME physical_columnar_zone_gate
  COMMAND physical_columnar_zone_gate
)
set_tests_properties(physical_columnar_zone_gate PROPERTIES
  LABELS "physical_columnar_zone_gate;IRC-092;index_runtime_correctness;physical_columnar_zone;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  document_path_physical_provider_gate
  document_path_physical_provider_gate.cpp
)
target_compile_features(document_path_physical_provider_gate PRIVATE cxx_std_20)
target_link_libraries(
  document_path_physical_provider_gate
  PRIVATE
    sb_engine_internal_api
)
add_test(
  NAME document_path_physical_provider_gate
  COMMAND document_path_physical_provider_gate
)
set_tests_properties(document_path_physical_provider_gate PROPERTIES
  LABELS "document_path_physical_provider_gate;IRC-100;index_runtime_correctness;document_path_physical_provider;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  document_provider_generation_lifecycle_gate
  document_provider_generation_lifecycle_gate.cpp
)
target_compile_features(document_provider_generation_lifecycle_gate PRIVATE cxx_std_20)
target_link_libraries(
  document_provider_generation_lifecycle_gate
  PRIVATE
    sb_engine_internal_api
)
add_test(
  NAME document_provider_generation_lifecycle_gate
  COMMAND document_provider_generation_lifecycle_gate
)
set_tests_properties(document_provider_generation_lifecycle_gate PROPERTIES
  LABELS "document_provider_generation_lifecycle_gate;IRC-101;index_runtime_correctness;document_provider_generation_lifecycle;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  text_inverted_segment_engine_gate
  text_inverted_segment_engine_gate.cpp
)
target_compile_features(text_inverted_segment_engine_gate PRIVATE cxx_std_20)
target_link_libraries(
  text_inverted_segment_engine_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME text_inverted_segment_engine_gate
  COMMAND text_inverted_segment_engine_gate
)
set_tests_properties(text_inverted_segment_engine_gate PROPERTIES
  LABELS "text_inverted_segment_engine_gate;IRC-110;index_runtime_correctness;text_inverted_segment;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  full_text_runtime_gate
  full_text_runtime_gate.cpp
)
target_compile_features(full_text_runtime_gate PRIVATE cxx_std_20)
target_link_libraries(
  full_text_runtime_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME full_text_runtime_gate
  COMMAND full_text_runtime_gate
)
set_tests_properties(full_text_runtime_gate PROPERTIES
  LABELS "full_text_runtime_gate;IRC-111;index_runtime_correctness;full_text_runtime;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  gin_ngram_physical_provider_gate
  gin_ngram_physical_provider_gate.cpp
)
target_compile_features(gin_ngram_physical_provider_gate PRIVATE cxx_std_20)
target_link_libraries(
  gin_ngram_physical_provider_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME gin_ngram_physical_provider_gate
  COMMAND gin_ngram_physical_provider_gate
)
set_tests_properties(gin_ngram_physical_provider_gate PROPERTIES
  LABELS "gin_ngram_physical_provider_gate;IRC-112;index_runtime_correctness;gin_physical_provider;ngram_physical_provider;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  spatial_rtree_physical_provider_gate
  spatial_rtree_physical_provider_gate.cpp
)
target_compile_features(spatial_rtree_physical_provider_gate PRIVATE cxx_std_20)
target_link_libraries(
  spatial_rtree_physical_provider_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME spatial_rtree_physical_provider_gate
  COMMAND spatial_rtree_physical_provider_gate
)
set_tests_properties(spatial_rtree_physical_provider_gate PROPERTIES
  LABELS "spatial_rtree_physical_provider_gate;IRC-130;index_runtime_correctness;spatial_rtree_physical_provider;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  gist_physical_provider_gate
  gist_physical_provider_gate.cpp
)
target_compile_features(gist_physical_provider_gate PRIVATE cxx_std_20)
target_link_libraries(
  gist_physical_provider_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME gist_physical_provider_gate
  COMMAND gist_physical_provider_gate
)
set_tests_properties(gist_physical_provider_gate PROPERTIES
  LABELS "gist_physical_provider_gate;IRC-131;index_runtime_correctness;gist_physical_provider;spatial_rtree_physical_provider;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  spgist_physical_provider_gate
  spgist_physical_provider_gate.cpp
)
target_compile_features(spgist_physical_provider_gate PRIVATE cxx_std_20)
target_link_libraries(
  spgist_physical_provider_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME spgist_physical_provider_gate
  COMMAND spgist_physical_provider_gate
)
set_tests_properties(spgist_physical_provider_gate PROPERTIES
  LABELS "spgist_physical_provider_gate;IRC-132;index_runtime_correctness;spgist_physical_provider;spatial_prefix_quadtree;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  graph_adjacency_physical_provider_gate
  graph_adjacency_physical_provider_gate.cpp
)
target_compile_features(graph_adjacency_physical_provider_gate PRIVATE cxx_std_20)
target_link_libraries(
  graph_adjacency_physical_provider_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME graph_adjacency_physical_provider_gate
  COMMAND graph_adjacency_physical_provider_gate
)
set_tests_properties(graph_adjacency_physical_provider_gate PROPERTIES
  LABELS "graph_adjacency_physical_provider_gate;IRC-140;index_runtime_correctness;graph_adjacency_physical_provider;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  temporary_work_index_runtime_gate
  temporary_work_index_runtime_gate.cpp
)
target_compile_features(temporary_work_index_runtime_gate PRIVATE cxx_std_20)
target_link_libraries(
  temporary_work_index_runtime_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME temporary_work_index_runtime_gate
  COMMAND temporary_work_index_runtime_gate
)
set_tests_properties(temporary_work_index_runtime_gate PROPERTIES
  LABELS "temporary_work_index_runtime_gate;IRC-150;index_runtime_correctness;temporary_work_index_runtime;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  in_memory_index_runtime_gate
  in_memory_index_runtime_gate.cpp
)
target_compile_features(in_memory_index_runtime_gate PRIVATE cxx_std_20)
target_link_libraries(
  in_memory_index_runtime_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME in_memory_index_runtime_gate
  COMMAND in_memory_index_runtime_gate
)
set_tests_properties(in_memory_index_runtime_gate PROPERTIES
  LABELS "in_memory_index_runtime_gate;IRC-151;index_runtime_correctness;in_memory_index_runtime;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  reference_emulated_index_mapping_gate
  reference_emulated_index_mapping_gate.cpp
)
target_compile_features(reference_emulated_index_mapping_gate PRIVATE cxx_std_20)
target_link_libraries(
  reference_emulated_index_mapping_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME reference_emulated_index_mapping_gate
  COMMAND reference_emulated_index_mapping_gate
)
set_tests_properties(reference_emulated_index_mapping_gate PROPERTIES
  LABELS "reference_emulated_index_mapping_gate;IRC-152;index_runtime_correctness;reference_emulated_index_mapping;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  policy_blocked_index_diagnostics_gate
  policy_blocked_index_diagnostics_gate.cpp
)
target_compile_features(policy_blocked_index_diagnostics_gate PRIVATE cxx_std_20)
target_link_libraries(
  policy_blocked_index_diagnostics_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME policy_blocked_index_diagnostics_gate
  COMMAND policy_blocked_index_diagnostics_gate
)
set_tests_properties(policy_blocked_index_diagnostics_gate PROPERTIES
  LABELS "policy_blocked_index_diagnostics_gate;IRC-153;index_runtime_correctness;policy_blocked;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  vector_exact_physical_provider_gate
  vector_exact_physical_provider_gate.cpp
)
target_compile_features(vector_exact_physical_provider_gate PRIVATE cxx_std_20)
target_link_libraries(
  vector_exact_physical_provider_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME vector_exact_physical_provider_gate
  COMMAND vector_exact_physical_provider_gate
)
set_tests_properties(vector_exact_physical_provider_gate PROPERTIES
  LABELS "vector_exact_physical_provider_gate;IRC-120;index_runtime_correctness;vector_exact_physical_provider;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  vector_hnsw_physical_provider_gate
  vector_hnsw_physical_provider_gate.cpp
)
target_compile_features(vector_hnsw_physical_provider_gate PRIVATE cxx_std_20)
target_link_libraries(
  vector_hnsw_physical_provider_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME vector_hnsw_physical_provider_gate
  COMMAND vector_hnsw_physical_provider_gate
)
set_tests_properties(vector_hnsw_physical_provider_gate PROPERTIES
  LABELS "vector_hnsw_physical_provider_gate;IRC-121;index_runtime_correctness;vector_hnsw_physical_provider;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  vector_ivf_pq_physical_provider_gate
  vector_ivf_pq_physical_provider_gate.cpp
)
target_compile_features(vector_ivf_pq_physical_provider_gate PRIVATE cxx_std_20)
target_link_libraries(
  vector_ivf_pq_physical_provider_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME vector_ivf_pq_physical_provider_gate
  COMMAND vector_ivf_pq_physical_provider_gate
)
set_tests_properties(vector_ivf_pq_physical_provider_gate PROPERTIES
  LABELS "vector_ivf_pq_physical_provider_gate;IRC-122;index_runtime_correctness;vector_ivf_pq_physical_provider;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  vector_provider_maintenance_gate
  vector_provider_maintenance_gate.cpp
)
target_compile_features(vector_provider_maintenance_gate PRIVATE cxx_std_20)
target_link_libraries(
  vector_provider_maintenance_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME vector_provider_maintenance_gate
  COMMAND vector_provider_maintenance_gate
)
set_tests_properties(vector_provider_maintenance_gate PROPERTIES
  LABELS "vector_provider_maintenance_gate;IRC-123;index_runtime_correctness;vector_provider_maintenance;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  unique_index_reservation_ledger_gate
  unique_index_reservation_ledger_gate.cpp
)
target_compile_features(unique_index_reservation_ledger_gate PRIVATE cxx_std_20)
target_link_libraries(
  unique_index_reservation_ledger_gate
  PRIVATE
    sb_core_index
    sb_core_uuid
)
add_test(
  NAME unique_index_reservation_ledger_gate
  COMMAND unique_index_reservation_ledger_gate
)
set_tests_properties(unique_index_reservation_ledger_gate PROPERTIES
  LABELS "unique_index_reservation_ledger_gate;IRC-021;index_runtime_correctness;unique_reservation_ledger;no_execution_plan_runtime;mga_transaction_regression"
)

if(NOT TARGET sb_core_bulk_load)
  add_subdirectory("${PROJECT_SOURCE_DIR}/src/core/bulk_load" "${CMAKE_CURRENT_BINARY_DIR}/core_bulk_load")
endif()

add_executable(
  sorted_bulk_unique_proof_gate
  sorted_bulk_unique_proof_gate.cpp
)
target_compile_features(sorted_bulk_unique_proof_gate PRIVATE cxx_std_20)
target_link_libraries(
  sorted_bulk_unique_proof_gate
  PRIVATE
    sb_core_index
    sb_core_bulk_load
    sb_core_uuid
)
add_test(
  NAME sorted_bulk_unique_proof_gate
  COMMAND sorted_bulk_unique_proof_gate
)
set_tests_properties(sorted_bulk_unique_proof_gate PROPERTIES
  LABELS "sorted_bulk_unique_proof_gate;IRC-022;IPAR-P4-05;index_runtime_correctness;sorted_bulk_unique_proof;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  sorted_bulk_leaf_branch_builder_gate
  sorted_bulk_leaf_branch_builder_gate.cpp
)
target_compile_features(sorted_bulk_leaf_branch_builder_gate PRIVATE cxx_std_20)
target_link_libraries(
  sorted_bulk_leaf_branch_builder_gate
  PRIVATE
    sb_core_index
    sb_storage_page
    sb_core_uuid
)
add_test(
  NAME sorted_bulk_leaf_branch_builder_gate
  COMMAND sorted_bulk_leaf_branch_builder_gate
)
set_tests_properties(sorted_bulk_leaf_branch_builder_gate PROPERTIES
  LABELS "sorted_bulk_leaf_branch_builder_gate;IRC-040;IPAR-P4-05;index_runtime_correctness;sorted_bulk_physical_bulk_build;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  index_root_generation_publish_gate
  index_root_generation_publish_gate.cpp
)
target_compile_features(index_root_generation_publish_gate PRIVATE cxx_std_20)
target_link_libraries(
  index_root_generation_publish_gate
  PRIVATE
    sb_core_index
    sb_storage_page
    sb_core_uuid
)
add_test(
  NAME index_root_generation_publish_gate
  COMMAND index_root_generation_publish_gate
)
set_tests_properties(index_root_generation_publish_gate PROPERTIES
  LABELS "index_root_generation_publish_gate;IRC-041;index_runtime_correctness;root_generation_publish;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  compressed_bitmap_container_format_gate
  compressed_bitmap_container_format_gate.cpp
)
target_compile_features(compressed_bitmap_container_format_gate PRIVATE cxx_std_20)
target_link_libraries(
  compressed_bitmap_container_format_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME compressed_bitmap_container_format_gate
  COMMAND compressed_bitmap_container_format_gate
)
set_tests_properties(compressed_bitmap_container_format_gate PROPERTIES
  LABELS "compressed_bitmap_container_format_gate;IRC-070;index_runtime_correctness;compressed_bitmap;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  compressed_bitmap_algebra_gate
  compressed_bitmap_algebra_gate.cpp
)
target_compile_features(compressed_bitmap_algebra_gate PRIVATE cxx_std_20)
target_link_libraries(
  compressed_bitmap_algebra_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME compressed_bitmap_algebra_gate
  COMMAND compressed_bitmap_algebra_gate
)
set_tests_properties(compressed_bitmap_algebra_gate PROPERTIES
  LABELS "compressed_bitmap_algebra_gate;IRC-071;index_runtime_correctness;compressed_bitmap;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  compressed_bitmap_spill_repair_simd_gate
  compressed_bitmap_spill_repair_simd_gate.cpp
)
target_compile_features(compressed_bitmap_spill_repair_simd_gate PRIVATE cxx_std_20)
target_link_libraries(
  compressed_bitmap_spill_repair_simd_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME compressed_bitmap_spill_repair_simd_gate
  COMMAND compressed_bitmap_spill_repair_simd_gate
)
set_tests_properties(compressed_bitmap_spill_repair_simd_gate PROPERTIES
  LABELS "compressed_bitmap_spill_repair_simd_gate;IRC-072;index_runtime_correctness;compressed_bitmap;spill_repair;simd_popcount;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  index_fault_injection_crash_matrix_gate
  index_fault_injection_crash_matrix_gate.cpp
)
target_compile_features(index_fault_injection_crash_matrix_gate PRIVATE cxx_std_20)
target_link_libraries(
  index_fault_injection_crash_matrix_gate
  PRIVATE
    sb_engine_internal_api
)
add_test(
  NAME index_fault_injection_crash_matrix_gate
  COMMAND index_fault_injection_crash_matrix_gate
)
set_tests_properties(index_fault_injection_crash_matrix_gate PROPERTIES
  LABELS "index_fault_injection_crash_matrix_gate;IRC-181;index_runtime_correctness;failure_injection;crash_reopen;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  index_family_benchmark_gate
  index_family_benchmark_gate.cpp
)
target_compile_features(index_family_benchmark_gate PRIVATE cxx_std_20)
target_link_libraries(
  index_family_benchmark_gate
  PRIVATE
    sb_core_index
    sb_core_uuid
)
add_test(
  NAME index_family_benchmark_gate
  COMMAND index_family_benchmark_gate
)
set_tests_properties(index_family_benchmark_gate PROPERTIES
  LABELS "index_family_benchmark_gate;IRC-182;index_runtime_correctness;family_benchmark;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  index_compact_encoding_gate
  index_compact_encoding_gate.cpp
)
target_compile_features(index_compact_encoding_gate PRIVATE cxx_std_20)
target_link_libraries(
  index_compact_encoding_gate
  PRIVATE
    sb_core_index
    sb_core_uuid
)
add_test(
  NAME index_compact_encoding_gate
  COMMAND index_compact_encoding_gate
)
set_tests_properties(index_compact_encoding_gate PROPERTIES
  LABELS "index_compact_encoding_gate;IRC-160;index_runtime_correctness;compact_encoding;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_001_002_gate
  optimizer_runtime_hot_path_orh_001_002_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_001_002_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_001_002_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_runtime_hot_path_orh_001_002_gate
  COMMAND optimizer_runtime_hot_path_orh_001_002_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_001_002_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_001_002_gate;ORH-001;ORH-002;optimizer_runtime_hot_path;performance;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_004_gate
  optimizer_runtime_hot_path_orh_004_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_004_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_004_gate
  PRIVATE
    sb_core_agents
    sb_core_index
    sb_engine_optimizer
)
add_test(
  NAME optimizer_runtime_hot_path_orh_004_gate
  COMMAND optimizer_runtime_hot_path_orh_004_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_004_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_004_gate;ORH-004;ORH-GATE-004;optimizer_runtime_hot_path;performance;index_rebase;route_capability;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_010_011_gate
  optimizer_runtime_hot_path_orh_010_011_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_010_011_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_010_011_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_runtime_hot_path_orh_010_011_gate
  COMMAND optimizer_runtime_hot_path_orh_010_011_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_010_011_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_010_011_gate;ORH-010;ORH-011;optimizer_runtime_hot_path;performance;benchmark_clean;catalog_backed_access_path;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_020_021_gate
  optimizer_runtime_hot_path_orh_020_021_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_020_021_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_020_021_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_runtime_hot_path_orh_020_021_gate
  COMMAND optimizer_runtime_hot_path_orh_020_021_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_020_021_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_020_021_gate;ORH-020;ORH-021;optimizer_runtime_hot_path;performance;extended_statistics;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_050_cache_gate
  optimizer_runtime_hot_path_orh_050_cache_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_050_cache_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_050_cache_gate
  PRIVATE
    sb_core_index
    sb_core_uuid
)
add_test(
  NAME optimizer_runtime_hot_path_orh_050_cache_gate
  COMMAND optimizer_runtime_hot_path_orh_050_cache_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_050_cache_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_050_cache_gate;ORH-050;optimizer_runtime_hot_path;performance;dml_hot_point_cache;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_060_061_062_gate
  optimizer_runtime_hot_path_orh_060_061_062_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_060_061_062_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_060_061_062_gate
  PRIVATE
    sb_wire_result_batch
)
add_test(
  NAME optimizer_runtime_hot_path_orh_060_061_062_gate
  COMMAND optimizer_runtime_hot_path_orh_060_061_062_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_060_061_062_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_060_061_062_gate;ORH-060;ORH-061;ORH-062;optimizer_runtime_hot_path;performance;streaming_cursor;continuation_token;binary_result_frame;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_070_071_gate
  optimizer_runtime_hot_path_orh_070_071_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_070_071_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_070_071_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME optimizer_runtime_hot_path_orh_070_071_gate
  COMMAND optimizer_runtime_hot_path_orh_070_071_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_070_071_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_070_071_gate;ORH-070;ORH-071;optimizer_runtime_hot_path;performance;compression_policy;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_080_081_gate
  optimizer_runtime_hot_path_orh_080_081_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_080_081_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_080_081_gate
  PRIVATE
    sb_core_agents
    sb_core_index
    sb_core_metrics
)
add_test(
  NAME optimizer_runtime_hot_path_orh_080_081_gate
  COMMAND optimizer_runtime_hot_path_orh_080_081_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_080_081_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_080_081_gate;ORH-080;ORH-081;optimizer_runtime_hot_path;performance;vector_maintenance;vector_lifecycle;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_090_091_092_gate
  optimizer_runtime_hot_path_orh_090_091_092_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_090_091_092_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_090_091_092_gate
  PRIVATE
    sb_engine_internal_api
    sb_storage_database
)
add_test(
  NAME optimizer_runtime_hot_path_orh_090_091_092_gate
  COMMAND optimizer_runtime_hot_path_orh_090_091_092_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_090_091_092_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_090_091_092_gate;ORH-090;ORH-091;ORH-092;optimizer_runtime_hot_path;performance;nosql_provider_generation;document_path_index_runtime;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_100_110_111_gate
  optimizer_runtime_hot_path_orh_100_110_111_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_100_110_111_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_100_110_111_gate
  PRIVATE
    sb_engine_internal_api
    sb_engine_executor
)
add_test(
  NAME optimizer_runtime_hot_path_orh_100_110_111_gate
  COMMAND optimizer_runtime_hot_path_orh_100_110_111_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_100_110_111_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_100_110_111_gate;ORH-100;ORH-110;ORH-111;optimizer_runtime_hot_path;performance;metric_provenance;snapshot_result_cache;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_120_gate
  optimizer_runtime_hot_path_orh_120_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_120_gate PRIVATE cxx_std_23)
target_link_libraries(
  optimizer_runtime_hot_path_orh_120_gate
  PRIVATE
    sb_engine_optimizer
    sb_engine_internal_api
    sb_engine_executor
    sb_core_index
    sb_core_agents
    sb_core_metrics
    sb_wire_result_batch
    sbl_sbsql_parser_pipeline
    sb_core_uuid
)
add_test(
  NAME optimizer_runtime_hot_path_orh_120_gate
  COMMAND optimizer_runtime_hot_path_orh_120_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_120_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_120_gate;ORH-120;optimizer_runtime_hot_path;performance;semantic_equivalence;benchmark_clean;index_runtime_blockers;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_121_gate
  optimizer_runtime_hot_path_orh_121_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_121_gate PRIVATE cxx_std_23)
target_compile_definitions(
  optimizer_runtime_hot_path_orh_121_gate
  PRIVATE
    SB_ORH121_SEED_PACK_ROOT="${CMAKE_SOURCE_DIR}/resources/seed-packs/initial-resource-pack"
)
target_link_libraries(
  optimizer_runtime_hot_path_orh_121_gate
  PRIVATE
    sb_engine_internal_api
    sb_engine_executor
    sb_core_index
    sb_core_agents
    sb_wire_result_batch
    sb_storage_database
)
add_test(
  NAME optimizer_runtime_hot_path_orh_121_gate
  COMMAND optimizer_runtime_hot_path_orh_121_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_121_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_121_gate;ORH-121;ORH-GATE-121;optimizer_runtime_hot_path;performance;fault_injection;crash_reopen;recovery;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_122_gate
  optimizer_runtime_hot_path_orh_122_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_122_gate PRIVATE cxx_std_23)
target_link_libraries(
  optimizer_runtime_hot_path_orh_122_gate
  PRIVATE
    sb_engine_optimizer
    sb_engine_internal_api
    sb_engine_executor
    sb_wire_result_batch
    sbl_sbsql_parser_pipeline
)
add_test(
  NAME optimizer_runtime_hot_path_orh_122_gate
  COMMAND optimizer_runtime_hot_path_orh_122_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_122_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_122_gate;ORH-122;optimizer_runtime_hot_path;performance;epoch_churn;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_123_gate
  optimizer_runtime_hot_path_orh_123_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_123_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_123_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_runtime_hot_path_orh_123_gate
  COMMAND optimizer_runtime_hot_path_orh_123_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_123_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_123_gate;ORH-123;ORH-GATE-123;ELER-105;ELER-GATE-105;optimizer_runtime_hot_path;performance;benchmark_methodology;noise_control;performance_scalability_baseline;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_124_gate
  optimizer_runtime_hot_path_orh_124_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_124_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_124_gate
  PRIVATE
    sb_core_agents
    sb_core_index
    sb_engine_internal_api
    sb_wire_result_batch
)
add_test(
  NAME optimizer_runtime_hot_path_orh_124_gate
  COMMAND optimizer_runtime_hot_path_orh_124_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_124_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_124_gate;ORH-124;optimizer_runtime_hot_path;performance;resource_governance;starvation_gate;benchmark_clean;mga_transaction_regression;no_execution_plan_runtime"
)

add_executable(
  optimizer_runtime_hot_path_orh_125_gate
  optimizer_runtime_hot_path_orh_125_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_125_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_125_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_runtime_hot_path_orh_125_gate
  COMMAND optimizer_runtime_hot_path_orh_125_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_125_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_125_gate;ORH-125;ORH-GATE-125;optimizer_runtime_hot_path;performance;cross_route_equivalence;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

if(TARGET sb_server AND TARGET sb_listener AND TARGET sbp_sbsql AND TARGET sb_isql)
  add_custom_target(
    optimizer_runtime_hot_path_orh_125_live_route_gate_deps
    DEPENDS sb_server sb_listener sbp_sbsql sb_isql
  )
  add_test(
    NAME optimizer_runtime_hot_path_orh_125_live_route_gate
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/optimizer_runtime_hot_path_orh_125_live_route_gate.py"
            --server $<TARGET_FILE:sb_server>
            --listener $<TARGET_FILE:sb_listener>
            --parser-worker $<TARGET_FILE:sbp_sbsql>
            --sb-isql $<TARGET_FILE:sb_isql>
            --work-dir "${CMAKE_CURRENT_BINARY_DIR}/optimizer_runtime_hot_path_orh_125_live_route_gate"
            --build-mode "$<CONFIG>"
  )
  set_tests_properties(optimizer_runtime_hot_path_orh_125_live_route_gate PROPERTIES
    LABELS "optimizer_runtime_hot_path_orh_125_live_route_gate;ORH-125;ORH-GATE-125;optimizer_runtime_hot_path;performance;cross_route_equivalence;live_route_capture;no_execution_plan_runtime;mga_transaction_regression"
  )
endif()

add_executable(
  optimizer_runtime_hot_path_orh_126_gate
  optimizer_runtime_hot_path_orh_126_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_126_gate PRIVATE cxx_std_23)
target_link_libraries(
  optimizer_runtime_hot_path_orh_126_gate
  PRIVATE
    sb_engine_optimizer
    sb_engine_internal_api
    sb_engine_executor
    sb_wire_result_batch
    sbl_sbsql_parser_pipeline
)
add_test(
  NAME optimizer_runtime_hot_path_orh_126_gate
  COMMAND optimizer_runtime_hot_path_orh_126_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_126_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_126_gate;ORH-126;ORH-GATE-126;security_revocation_redaction_race;optimizer_runtime_hot_path;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_127_gate
  optimizer_runtime_hot_path_orh_127_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_127_gate PRIVATE cxx_std_23)
target_link_libraries(
  optimizer_runtime_hot_path_orh_127_gate
  PRIVATE
    sb_engine_internal_api
    sb_core_index
)
add_test(
  NAME optimizer_runtime_hot_path_orh_127_gate
  COMMAND optimizer_runtime_hot_path_orh_127_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_127_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_127_gate;ORH-127;ORH-GATE-127;persisted_metadata;upgrade_backfill_repair;optimizer_runtime_hot_path;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_128_gate
  optimizer_runtime_hot_path_orh_128_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_128_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_128_gate
  PRIVATE
    sb_engine_internal_api
    sb_engine_optimizer
)
add_test(
  NAME optimizer_runtime_hot_path_orh_128_gate
  COMMAND optimizer_runtime_hot_path_orh_128_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_128_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_128_gate;ORH-128;ORH-GATE-128;ELER-105;ELER-GATE-105;evidence_overhead;overhead_budget;optimizer_runtime_hot_path;performance_scalability_baseline;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_129_gate
  optimizer_runtime_hot_path_orh_129_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_129_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_129_gate
  PRIVATE
    sb_engine_internal_api
    sb_engine_optimizer
    sb_engine_executor
    sb_storage_database
    sb_wire_result_batch
)
add_test(
  NAME optimizer_runtime_hot_path_orh_129_gate
  COMMAND optimizer_runtime_hot_path_orh_129_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_129_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_129_gate;ORH-129;ORH-GATE-129;property_fuzz;optimizer_runtime_hot_path;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_200_201_260_gate
  optimizer_runtime_hot_path_orh_200_201_260_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_200_201_260_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_200_201_260_gate
  PRIVATE
    sb_engine_optimizer
    sb_engine_internal_api
)
add_test(
  NAME optimizer_runtime_hot_path_orh_200_201_260_gate
  COMMAND optimizer_runtime_hot_path_orh_200_201_260_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_200_201_260_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_200_201_260_gate;ORH-200;ORH-201;ORH-260;optimizer_runtime_hot_path;performance;benchmark_clean;profiler_attribution;benchmark_equivalence;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_202_203_gate
  optimizer_runtime_hot_path_orh_202_203_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_202_203_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_202_203_gate
  PRIVATE
    sb_engine_optimizer
    sb_engine_internal_api
    sb_engine_sblr
    sb_wire_result_batch
)
add_test(
  NAME optimizer_runtime_hot_path_orh_202_203_gate
  COMMAND optimizer_runtime_hot_path_orh_202_203_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_202_203_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_202_203_gate;ORH-202;ORH-203;optimizer_runtime_hot_path;performance;benchmark_clean;binary_result_frame;evidence_suppression;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_210_211_212_gate
  optimizer_runtime_hot_path_orh_210_211_212_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_210_211_212_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_210_211_212_gate
  PRIVATE
    sb_engine_internal_api
    sb_storage_database
)
add_test(
  NAME optimizer_runtime_hot_path_orh_210_211_212_gate
  COMMAND optimizer_runtime_hot_path_orh_210_211_212_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_210_211_212_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_210_211_212_gate;ORH-210;ORH-211;ORH-212;IPAR-P4-05;optimizer_runtime_hot_path;performance;native_bulk_ingest;deferred_index_publish;page_allocator_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_220_221_222_gate
  optimizer_runtime_hot_path_orh_220_221_222_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_220_221_222_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_220_221_222_gate
  PRIVATE
    sb_engine_executor
    sb_engine_optimizer
    sb_core_index
    sb_storage_page
)
add_test(
  NAME optimizer_runtime_hot_path_orh_220_221_222_gate
  COMMAND optimizer_runtime_hot_path_orh_220_221_222_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_220_221_222_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_220_221_222_gate;ORH-220;ORH-221;ORH-222;optimizer_runtime_hot_path;performance;physical_join_operator;vectorized_parallel_pipeline;late_materialization;covering_index_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_230_231_gate
  optimizer_runtime_hot_path_orh_230_231_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_230_231_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_230_231_gate
  PRIVATE
    sb_engine_internal_api
    sb_core_index
    sb_storage_page
)
add_test(
  NAME optimizer_runtime_hot_path_orh_230_231_gate
  COMMAND optimizer_runtime_hot_path_orh_230_231_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_230_231_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_230_231_gate;ORH-230;ORH-231;optimizer_runtime_hot_path;performance;row_locator_dml;hot_update_delta;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_240_gate
  optimizer_runtime_hot_path_orh_240_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_240_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_240_gate
  PRIVATE
    sb_transaction_mga
    sb_storage_page
    sb_engine_optimizer
)
add_test(
  NAME optimizer_runtime_hot_path_orh_240_gate
  COMMAND optimizer_runtime_hot_path_orh_240_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_240_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_240_gate;ORH-240;ORH-GATE-240;visibility_accelerator;current_row_map;page_finality_map;optimizer_runtime_hot_path;performance;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_241_gate
  optimizer_runtime_hot_path_orh_241_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_241_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_241_gate
  PRIVATE
    sb_engine_executor
)
add_test(
  NAME optimizer_runtime_hot_path_orh_241_gate
  COMMAND optimizer_runtime_hot_path_orh_241_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_241_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_241_gate;ORH-241;ORH-GATE-241;buffer_prefetch;read_ahead;hot_page_pinning;optimizer_runtime_hot_path;performance;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_242_gate
  optimizer_runtime_hot_path_orh_242_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_242_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_242_gate
  PRIVATE
    sb_core_index
    sb_storage_page
)
add_test(
  NAME optimizer_runtime_hot_path_orh_242_gate
  COMMAND optimizer_runtime_hot_path_orh_242_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_242_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_242_gate;ORH-242;ORH-GATE-242;compact_encoding;posting_list;uuidv7_keys;optimizer_runtime_hot_path;performance;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_250_gate
  optimizer_runtime_hot_path_orh_250_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_250_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_250_gate
  PRIVATE
    sb_engine_executor
    sb_core_index
)
add_test(
  NAME optimizer_runtime_hot_path_orh_250_gate
  COMMAND optimizer_runtime_hot_path_orh_250_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_250_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_250_gate;ORH-250;ORH-GATE-250;foreground_parallel_execution;multi_worker;resource_governance;optimizer_runtime_hot_path;performance;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_test(
  NAME optimizer_runtime_hot_path_orh_270_gate
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/optimizer_runtime_hot_path_orh_270_gate.py"
          --artifact "${PROJECT_SOURCE_DIR}/../project/tests/public_migrated_proof/completed/optimizer-runtime-hot-path-operationalization-closure/artifacts/ORH_270_REFERENCE_DOMINANCE_CLOSURE_BLOCKERS.json"
)
set_tests_properties(optimizer_runtime_hot_path_orh_270_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_270_gate;ORH-270;ORH-GATE-270;reference_dominance_closure;deprecated;optimizer_runtime_hot_path;performance;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_287_gate
  optimizer_runtime_hot_path_orh_287_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_287_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_287_gate
  PRIVATE
    sb_engine_optimizer
    sb_core_index
)
add_test(
  NAME optimizer_runtime_hot_path_orh_287_gate
  COMMAND optimizer_runtime_hot_path_orh_287_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_287_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_287_gate;ORH-287;ORH-GATE-287;optimization_ab_bisect;benchmark_harness;optimizer_runtime_hot_path;performance;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_288_gate
  optimizer_runtime_hot_path_orh_288_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_288_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_288_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_runtime_hot_path_orh_288_gate
  COMMAND optimizer_runtime_hot_path_orh_288_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_288_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_288_gate;ORH-288;ORH-GATE-288;large_scale_benchmark_tiers;benchmark_harness;optimizer_runtime_hot_path;performance;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_289_gate
  optimizer_runtime_hot_path_orh_289_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_289_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_289_gate
  PRIVATE
    sb_engine_executor
    sb_core_index
    sb_storage_page
)
add_test(
  NAME optimizer_runtime_hot_path_orh_289_gate
  COMMAND optimizer_runtime_hot_path_orh_289_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_289_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_289_gate;ORH-289;ORH-GATE-289;cpu_cache_layout;layout_observability;optimizer_runtime_hot_path;performance;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_test(
  NAME optimizer_runtime_hot_path_orh_900_gate
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/optimizer_runtime_hot_path_orh_900_gate.py"
          --artifact "${PROJECT_SOURCE_DIR}/../project/tests/public_migrated_proof/completed/optimizer-runtime-hot-path-operationalization-closure/artifacts/ORH_900_FOCUSED_LIVE_ROUTE_CLOSURE_BLOCKERS.json"
)
set_tests_properties(optimizer_runtime_hot_path_orh_900_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_900_gate;ORH-900;ORH-GATE-900;focused_live_route_closure;deprecated;optimizer_runtime_hot_path;performance;mga_transaction_regression"
)

add_test(
  NAME optimizer_runtime_hot_path_orh_901_gate
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/optimizer_runtime_hot_path_orh_901_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(optimizer_runtime_hot_path_orh_901_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_901_gate;ORH-901;ORH-GATE-901;final_audit;movement_readiness;completed;optimizer_runtime_hot_path;performance;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_280_gate
  optimizer_runtime_hot_path_orh_280_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_280_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_280_gate
  PRIVATE
    sb_engine_sblr
)
add_test(
  NAME optimizer_runtime_hot_path_orh_280_gate
  COMMAND optimizer_runtime_hot_path_orh_280_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_280_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_280_gate;ORH-280;ORH-GATE-280;sblr_execution_hot_path;prepared_sblr_template;opcode_specialization;superinstruction;batched_repeated_row;optimizer_runtime_hot_path;performance;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_281_gate
  optimizer_runtime_hot_path_orh_281_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_281_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_281_gate
  PRIVATE
    sb_engine_executor
)
add_test(
  NAME optimizer_runtime_hot_path_orh_281_gate
  COMMAND optimizer_runtime_hot_path_orh_281_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_281_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_281_gate;ORH-281;ORH-GATE-281;allocator_lifetime;query_memory_arena;object_lifetime;optimizer_runtime_hot_path;performance;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_282_gate
  optimizer_runtime_hot_path_orh_282_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_282_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_282_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_runtime_hot_path_orh_282_gate
  COMMAND optimizer_runtime_hot_path_orh_282_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_282_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_282_gate;ORH-282;ORH-GATE-282;adaptive_cardinality_feedback;bind_sensitive_variant;misestimate_quarantine;extended_stats_request;optimizer_runtime_hot_path;performance;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_283_gate
  optimizer_runtime_hot_path_orh_283_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_283_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_283_gate
  PRIVATE
    sb_engine_executor
)
add_test(
  NAME optimizer_runtime_hot_path_orh_283_gate
  COMMAND optimizer_runtime_hot_path_orh_283_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_283_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_283_gate;ORH-283;ORH-GATE-283;temp_spill;sort_topn_distinct_groupby_hashagg;optimizer_runtime_hot_path;performance;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_284_gate
  optimizer_runtime_hot_path_orh_284_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_284_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_284_gate
  PRIVATE
    sb_storage_database
)
add_test(
  NAME optimizer_runtime_hot_path_orh_284_gate
  COMMAND optimizer_runtime_hot_path_orh_284_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_284_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_284_gate;ORH-284;ORH-GATE-284;durability_write_batching;write_path_batching;dirty_page_tracking;transaction_inventory_recovery;optimizer_runtime_hot_path;performance;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_285_gate
  optimizer_runtime_hot_path_orh_285_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_285_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_285_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME optimizer_runtime_hot_path_orh_285_gate
  COMMAND optimizer_runtime_hot_path_orh_285_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_285_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_285_gate;ORH-285;ORH-GATE-285;parallel_index_constraint_validation;deferred_bulk_publish;optimizer_runtime_hot_path;performance;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_runtime_hot_path_orh_286_gate
  optimizer_runtime_hot_path_orh_286_gate.cpp
)
target_compile_features(optimizer_runtime_hot_path_orh_286_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_runtime_hot_path_orh_286_gate
  PRIVATE
    sb_engine_executor
    sb_core_index
    sb_storage_page
)
add_test(
  NAME optimizer_runtime_hot_path_orh_286_gate
  COMMAND optimizer_runtime_hot_path_orh_286_gate
)
set_tests_properties(optimizer_runtime_hot_path_orh_286_gate PROPERTIES
  LABELS "optimizer_runtime_hot_path_orh_286_gate;ORH-286;ORH-GATE-286;plan_shape_regression;route_capability;optimizer_runtime_hot_path;performance;benchmark_clean;no_execution_plan_runtime;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_batch_c_foundation_gate
  optimizer_deficiency_batch_c_foundation_gate.cpp
)
target_compile_features(optimizer_deficiency_batch_c_foundation_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_batch_c_foundation_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_deficiency_batch_c_foundation_gate
  COMMAND optimizer_deficiency_batch_c_foundation_gate
)
set_tests_properties(optimizer_deficiency_batch_c_foundation_gate PROPERTIES
  LABELS "optimizer_deficiency_batch_c_foundation_gate;ODF-015;ODF-016;optimizer_deficiency;performance;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_017_gate
  optimizer_deficiency_odf_017_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_017_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_017_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_deficiency_odf_017_gate
  COMMAND optimizer_deficiency_odf_017_gate
)
set_tests_properties(optimizer_deficiency_odf_017_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_017_gate;ODF-017;optimizer_deficiency;performance;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_018_gate
  optimizer_deficiency_odf_018_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_018_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_018_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_deficiency_odf_018_gate
  COMMAND optimizer_deficiency_odf_018_gate
)
set_tests_properties(optimizer_deficiency_odf_018_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_018_gate;ODF-018;optimizer_deficiency;performance;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_019_gate
  optimizer_deficiency_odf_019_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_019_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_019_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_deficiency_odf_019_gate
  COMMAND optimizer_deficiency_odf_019_gate
)
set_tests_properties(optimizer_deficiency_odf_019_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_019_gate;ODF-019;optimizer_deficiency;performance;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_020_gate
  optimizer_deficiency_odf_020_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_020_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_020_gate
  PRIVATE
    sb_engine_sblr
    sb_engine_executor
)
add_test(
  NAME optimizer_deficiency_odf_020_gate
  COMMAND optimizer_deficiency_odf_020_gate
)
set_tests_properties(optimizer_deficiency_odf_020_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_020_gate;ODF-020;optimizer_deficiency;performance;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_021_gate
  optimizer_deficiency_odf_021_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_021_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_021_gate
  PRIVATE
    sb_engine_internal_api
    sb_engine_optimizer
    sb_engine_executor
)
add_test(
  NAME optimizer_deficiency_odf_021_gate
  COMMAND optimizer_deficiency_odf_021_gate
)
set_tests_properties(optimizer_deficiency_odf_021_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_021_gate;ODF-021;optimizer_deficiency;performance;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_022_gate
  optimizer_deficiency_odf_022_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_022_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_022_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_deficiency_odf_022_gate
  COMMAND optimizer_deficiency_odf_022_gate
)
set_tests_properties(optimizer_deficiency_odf_022_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_022_gate;ODF-022;optimizer_deficiency;performance;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_023_gate
  optimizer_deficiency_odf_023_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_023_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_023_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_deficiency_odf_023_gate
  COMMAND optimizer_deficiency_odf_023_gate
)
set_tests_properties(optimizer_deficiency_odf_023_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_023_gate;ODF-023;optimizer_deficiency;performance;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_024_gate
  optimizer_deficiency_odf_024_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_024_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_024_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_deficiency_odf_024_gate
  COMMAND optimizer_deficiency_odf_024_gate
)
set_tests_properties(optimizer_deficiency_odf_024_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_024_gate;ODF-024;optimizer_deficiency;performance;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_025_gate
  optimizer_deficiency_odf_025_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_025_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_025_gate
  PRIVATE
    sb_engine_optimizer
    sb_core_metrics
)
add_test(
  NAME optimizer_deficiency_odf_025_gate
  COMMAND optimizer_deficiency_odf_025_gate
)
set_tests_properties(optimizer_deficiency_odf_025_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_025_gate;ODF-025;optimizer_deficiency;performance;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_026_gate
  optimizer_deficiency_odf_026_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_026_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_026_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_deficiency_odf_026_gate
  COMMAND optimizer_deficiency_odf_026_gate
)
set_tests_properties(optimizer_deficiency_odf_026_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_026_gate;ODF-026;optimizer_deficiency;performance;benchmark_clean;mga_transaction_regression"
)

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_engine_sblr AND TARGET sb_core_uuid)
  add_executable(
    optimizer_deficiency_odf_110_gate
    optimizer_deficiency_odf_110_gate.cpp
  )
  target_compile_features(optimizer_deficiency_odf_110_gate PRIVATE cxx_std_20)
  target_compile_definitions(
    optimizer_deficiency_odf_110_gate
    PRIVATE
      ODF110_OUTPUT_JSON="${CMAKE_CURRENT_BINARY_DIR}/optimizer_deficiency_odf_110_gate_evidence/optimizer_deficiency_odf_110_gate.json"
  )
  target_link_libraries(
    optimizer_deficiency_odf_110_gate
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_engine_sblr
      sb_core_uuid
  )
  add_test(
    NAME optimizer_deficiency_odf_110_gate
    COMMAND optimizer_deficiency_odf_110_gate
  )
  set_tests_properties(optimizer_deficiency_odf_110_gate PROPERTIES
    LABELS "optimizer_deficiency_odf_110_gate;ODF-110;optimizer_deficiency;performance;benchmark;sql;planner;sbsql;sblr;api;benchmark_clean;mga_transaction_regression"
  )
endif()

if(TARGET sb_core_uuid AND TARGET sb_engine_internal_api AND TARGET sb_storage_database)
  add_executable(
    optimizer_deficiency_odf_111_gate
    optimizer_deficiency_odf_111_gate.cpp
  )
  target_compile_features(optimizer_deficiency_odf_111_gate PRIVATE cxx_std_20)
  target_compile_definitions(
    optimizer_deficiency_odf_111_gate
    PRIVATE
      ODF111_OUTPUT_JSON="${CMAKE_CURRENT_BINARY_DIR}/optimizer_deficiency_odf_111_gate_evidence/optimizer_deficiency_odf_111_gate.json"
  )
  target_link_libraries(
    optimizer_deficiency_odf_111_gate
    PRIVATE
      sb_core_uuid
      sb_engine_internal_api
      sb_storage_database
  )
  add_test(
    NAME optimizer_deficiency_odf_111_gate
    COMMAND optimizer_deficiency_odf_111_gate
  )
  set_tests_properties(optimizer_deficiency_odf_111_gate PROPERTIES
    LABELS "optimizer_deficiency_odf_111_gate;ODF-111;optimizer_deficiency;performance;benchmark;dml;row_location;update;delete;unique_preflight;hot_update;benchmark_clean;mga_transaction_regression"
  )
endif()

if(TARGET sb_core_uuid AND TARGET sb_engine_internal_api AND TARGET sb_engine_sblr AND TARGET sb_storage_database)
  add_executable(
    optimizer_deficiency_odf_112_gate
    optimizer_deficiency_odf_112_gate.cpp
  )
  target_compile_features(optimizer_deficiency_odf_112_gate PRIVATE cxx_std_20)
  target_compile_definitions(
    optimizer_deficiency_odf_112_gate
    PRIVATE
      ODF112_OUTPUT_JSON="${CMAKE_CURRENT_BINARY_DIR}/optimizer_deficiency_odf_112_gate_evidence/optimizer_deficiency_odf_112_gate.json"
  )
  target_link_libraries(
    optimizer_deficiency_odf_112_gate
    PRIVATE
      sb_core_uuid
      sb_engine_internal_api
      sb_engine_sblr
      sb_storage_database
  )
  add_test(
    NAME optimizer_deficiency_odf_112_gate
    COMMAND optimizer_deficiency_odf_112_gate
  )
  set_tests_properties(optimizer_deficiency_odf_112_gate PROPERTIES
    LABELS "optimizer_deficiency_odf_112_gate;ODF-112;IPAR-P4-05;optimizer_deficiency;performance;benchmark;bulk;copy;native_bulk;sorted_index_build;proof_validation;reject_bisection;page_preallocation;benchmark_clean;mga_transaction_regression"
  )
endif()

if(TARGET sb_engine_internal_api)
  add_executable(
    optimizer_deficiency_odf_113_gate
    optimizer_deficiency_odf_113_gate.cpp
  )
  target_compile_features(optimizer_deficiency_odf_113_gate PRIVATE cxx_std_20)
  target_compile_definitions(
    optimizer_deficiency_odf_113_gate
    PRIVATE
      ODF113_OUTPUT_JSON="${CMAKE_CURRENT_BINARY_DIR}/optimizer_deficiency_odf_113_gate_evidence/optimizer_deficiency_odf_113_gate.json"
  )
  target_link_libraries(
    optimizer_deficiency_odf_113_gate
    PRIVATE
      sb_engine_internal_api
  )
  add_test(
    NAME optimizer_deficiency_odf_113_gate
    COMMAND optimizer_deficiency_odf_113_gate
  )
  set_tests_properties(optimizer_deficiency_odf_113_gate PROPERTIES
    LABELS "optimizer_deficiency_odf_113_gate;ODF-113;optimizer_deficiency;performance;benchmark;nosql;kv;document;search;vector;graph;time_series;physical_provider;benchmark_clean;mga_transaction_regression"
  )
endif()

if(TARGET sb_core_index AND TARGET sb_engine_executor AND TARGET sb_engine_optimizer_contract_only)
  add_executable(
    optimizer_deficiency_odf_114_gate
    optimizer_deficiency_odf_114_gate.cpp
  )
  target_compile_features(optimizer_deficiency_odf_114_gate PRIVATE cxx_std_20)
  target_compile_definitions(
    optimizer_deficiency_odf_114_gate
    PRIVATE
      ODF114_OUTPUT_JSON="${CMAKE_CURRENT_BINARY_DIR}/optimizer_deficiency_odf_114_gate_evidence/optimizer_deficiency_odf_114_gate.json"
  )
  target_link_libraries(
    optimizer_deficiency_odf_114_gate
    PRIVATE
      sb_core_index
      sb_engine_executor
      sb_engine_optimizer_contract_only
  )
  add_test(
    NAME optimizer_deficiency_odf_114_gate
    COMMAND optimizer_deficiency_odf_114_gate
  )
  set_tests_properties(optimizer_deficiency_odf_114_gate PROPERTIES
    LABELS "optimizer_deficiency_odf_114_gate;ODF-114;optimizer_deficiency;performance;benchmark;fusion;mixed_family;candidate_set;runtime_filter;operator_fusion;sblr;mga_transaction_regression;benchmark_clean"
  )
endif()

if(TARGET sb_engine_sblr AND TARGET sb_storage_database AND TARGET sb_transaction_mga)
  add_executable(
    scratchbird_beta_performance_baseline_conformance
    scratchbird_beta_performance_baseline_conformance.cpp
  )
  target_compile_features(scratchbird_beta_performance_baseline_conformance PRIVATE cxx_std_23)
  target_compile_definitions(
    scratchbird_beta_performance_baseline_conformance
    PRIVATE
      SB_PERF_THRESHOLD_JSON="${CMAKE_CURRENT_SOURCE_DIR}/BETA_PERFORMANCE_BASELINE_THRESHOLDS.json"
      SB_PERF_OUTPUT_JSON="${CMAKE_CURRENT_BINARY_DIR}/scratchbird_beta_performance_baseline_result.json"
      SB_PERF_SEED_PACK_ROOT="${PROJECT_SOURCE_DIR}/resources/seed-packs/initial-resource-pack"
  )
  target_link_libraries(
    scratchbird_beta_performance_baseline_conformance
    PRIVATE
      sb_engine_sblr
      sb_core_memory
      sb_storage_database
      sb_transaction_mga
  )
  if(WIN32)
    target_link_libraries(
      scratchbird_beta_performance_baseline_conformance
      PRIVATE
        psapi
    )
  endif()
  add_test(
    NAME scratchbird_beta_performance_baseline_conformance
    COMMAND scratchbird_beta_performance_baseline_conformance
  )
  set_tests_properties(scratchbird_beta_performance_baseline_conformance PROPERTIES
    LABELS "scratchbird_beta_performance_baseline_conformance;CBQ-040;CBQ_GATE_PERFORMANCE_BASELINE_THRESHOLDS;ELER-105;ELER-GATE-105;IPAR-P0-05;IPAR-P7-07;performance;baseline;throughput;latency;latency_percentiles;runtime_health_counters;resource_high_water;profiler_evidence;post_fix_profiler_comparison;memory;benchmark_clean;mga_transaction_regression"
  )
endif()
