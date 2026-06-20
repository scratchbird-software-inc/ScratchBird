# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_executable(
  optimizer_enterprise_inventory_gate
  optimizer_enterprise_inventory_gate.cpp
)
target_compile_features(optimizer_enterprise_inventory_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_inventory_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_inventory_gate
  COMMAND optimizer_enterprise_inventory_gate
)
set_tests_properties(optimizer_enterprise_inventory_gate PROPERTIES
  LABELS "optimizer_enterprise_inventory_gate;OEIC-001;OEIC-002;OEIC-003;optimizer;enterprise;cluster_boundary;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_metric_manifest_gate
  optimizer_enterprise_metric_manifest_gate.cpp
)
target_compile_features(optimizer_enterprise_metric_manifest_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_metric_manifest_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_metric_manifest_gate
  COMMAND optimizer_enterprise_metric_manifest_gate
)
set_tests_properties(optimizer_enterprise_metric_manifest_gate PROPERTIES
  LABELS "optimizer_enterprise_metric_manifest_gate;OEIC-010;optimizer;enterprise;metrics;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_027_gate
  optimizer_deficiency_odf_027_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_027_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_027_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_deficiency_odf_027_gate
  COMMAND optimizer_deficiency_odf_027_gate
)
set_tests_properties(optimizer_deficiency_odf_027_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_027_gate;ODF-027;optimizer_deficiency;optimizer;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_028_gate
  optimizer_deficiency_odf_028_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_028_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_028_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_deficiency_odf_028_gate
  COMMAND optimizer_deficiency_odf_028_gate
)
set_tests_properties(optimizer_deficiency_odf_028_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_028_gate;ODF-028;optimizer_deficiency;optimizer;benchmark_clean;mga_transaction_regression"
)

if(NOT TARGET sb_engine_internal_api)
  add_subdirectory(
    ${PROJECT_SOURCE_DIR}/src/engine/internal_api
    ${CMAKE_BINARY_DIR}/engine_internal_api_for_optimizer_tests
  )
endif()
if(NOT TARGET sb_engine_native_compile)
  add_subdirectory(
    ${PROJECT_SOURCE_DIR}/src/engine/native_compile
    ${CMAKE_BINARY_DIR}/engine_native_compile_for_optimizer_tests
  )
endif()
if(NOT TARGET sb_engine_executor)
  add_subdirectory(
    ${PROJECT_SOURCE_DIR}/src/engine/executor
    ${CMAKE_BINARY_DIR}/engine_executor_for_optimizer_tests
  )
endif()
if(NOT TARGET sb_core_index)
  add_subdirectory(
    ${PROJECT_SOURCE_DIR}/src/core/index
    ${CMAKE_BINARY_DIR}/core_index_for_optimizer_tests
  )
endif()

add_executable(
  optimizer_enterprise_llvm_boundary_gate
  optimizer_enterprise_llvm_boundary_gate.cpp
)
target_compile_features(optimizer_enterprise_llvm_boundary_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_llvm_boundary_gate
  PRIVATE
    sb_engine_native_compile
)
add_test(
  NAME optimizer_enterprise_llvm_boundary_gate
  COMMAND optimizer_enterprise_llvm_boundary_gate
)
set_tests_properties(optimizer_enterprise_llvm_boundary_gate PROPERTIES
  LABELS "optimizer_enterprise_llvm_boundary_gate;OEIC-004;OEIC-070;OEIC-072;optimizer;enterprise;llvm;mga_transaction_regression"
)

add_test(
  NAME optimizer_enterprise_llvm_configure_gate
  COMMAND "${CMAKE_COMMAND}"
          "-DSB_PROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
          "-DSB_CONFIGURE_GATE_BINARY_ROOT=${CMAKE_BINARY_DIR}/optimizer_enterprise_llvm_configure_gate"
          "-DSB_LLVM_TEST_LIBRARY=${SB_LLVM_LIBRARY}"
          -P "${PROJECT_SOURCE_DIR}/cmake/OptimizerLlvmConfigureGate.cmake"
)
set_tests_properties(optimizer_enterprise_llvm_configure_gate PROPERTIES
  LABELS "optimizer_enterprise_llvm_configure_gate;OEIC-070;OEIC-071;optimizer;enterprise;llvm;cmake_config"
)

add_executable(
  optimizer_enterprise_operator_actuals_gate
  optimizer_enterprise_operator_actuals_gate.cpp
)
target_compile_features(optimizer_enterprise_operator_actuals_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_operator_actuals_gate
  PRIVATE
    sb_engine_executor
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_operator_actuals_gate
  COMMAND optimizer_enterprise_operator_actuals_gate
)
set_tests_properties(optimizer_enterprise_operator_actuals_gate PROPERTIES
  LABELS "optimizer_enterprise_operator_actuals_gate;OEIC-011;optimizer;enterprise;executor;metrics;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_storage_metrics_gate
  optimizer_enterprise_storage_metrics_gate.cpp
)
target_compile_features(optimizer_enterprise_storage_metrics_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_storage_metrics_gate
  PRIVATE
    sb_storage_page
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_storage_metrics_gate
  COMMAND optimizer_enterprise_storage_metrics_gate
)
set_tests_properties(optimizer_enterprise_storage_metrics_gate PROPERTIES
  LABELS "optimizer_enterprise_storage_metrics_gate;OEIC-012;optimizer;enterprise;storage;metrics;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_mga_pressure_metrics_gate
  optimizer_enterprise_mga_pressure_metrics_gate.cpp
)
target_compile_features(optimizer_enterprise_mga_pressure_metrics_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_mga_pressure_metrics_gate
  PRIVATE
    sb_transaction_mga
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_mga_pressure_metrics_gate
  COMMAND optimizer_enterprise_mga_pressure_metrics_gate
)
set_tests_properties(optimizer_enterprise_mga_pressure_metrics_gate PROPERTIES
  LABELS "optimizer_enterprise_mga_pressure_metrics_gate;OEIC-013;optimizer;enterprise;mga;metrics;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_index_metrics_gate
  optimizer_enterprise_index_metrics_gate.cpp
)
target_compile_features(optimizer_enterprise_index_metrics_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_index_metrics_gate
  PRIVATE
    sb_core_index
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_index_metrics_gate
  COMMAND optimizer_enterprise_index_metrics_gate
)
set_tests_properties(optimizer_enterprise_index_metrics_gate PROPERTIES
  LABELS "optimizer_enterprise_index_metrics_gate;OEIC-014;optimizer;enterprise;index;metrics;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_specialized_metrics_gate
  optimizer_enterprise_specialized_metrics_gate.cpp
)
target_compile_features(optimizer_enterprise_specialized_metrics_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_specialized_metrics_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_specialized_metrics_gate
  COMMAND optimizer_enterprise_specialized_metrics_gate
)
set_tests_properties(optimizer_enterprise_specialized_metrics_gate PROPERTIES
  LABELS "optimizer_enterprise_specialized_metrics_gate;OEIC-015;optimizer;enterprise;specialized;metrics;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_route_metrics_gate
  optimizer_enterprise_route_metrics_gate.cpp
)
target_compile_features(optimizer_enterprise_route_metrics_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_route_metrics_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_route_metrics_gate
  COMMAND optimizer_enterprise_route_metrics_gate
)
set_tests_properties(optimizer_enterprise_route_metrics_gate PROPERTIES
  LABELS "optimizer_enterprise_route_metrics_gate;OEIC-016;optimizer;enterprise;route;metrics;driver_visible;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_metric_bundle_gate
  optimizer_enterprise_metric_bundle_gate.cpp
)
target_compile_features(optimizer_enterprise_metric_bundle_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_metric_bundle_gate
  PRIVATE
    sb_engine_internal_api
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_metric_bundle_gate
  COMMAND optimizer_enterprise_metric_bundle_gate
)
set_tests_properties(optimizer_enterprise_metric_bundle_gate PROPERTIES
  LABELS "optimizer_enterprise_metric_bundle_gate;OEIC-017;optimizer;enterprise;metrics;support_bundle;redaction;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_analyze_statistics_gate
  optimizer_enterprise_analyze_statistics_gate.cpp
)
target_compile_features(optimizer_enterprise_analyze_statistics_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_analyze_statistics_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_analyze_statistics_gate
  COMMAND optimizer_enterprise_analyze_statistics_gate
)
set_tests_properties(optimizer_enterprise_analyze_statistics_gate PROPERTIES
  LABELS "optimizer_enterprise_analyze_statistics_gate;OEIC-020;optimizer;enterprise;analyze;statistics;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_stats_invalidation_gate
  optimizer_enterprise_stats_invalidation_gate.cpp
)
target_compile_features(optimizer_enterprise_stats_invalidation_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_stats_invalidation_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_stats_invalidation_gate
  COMMAND optimizer_enterprise_stats_invalidation_gate
)
set_tests_properties(optimizer_enterprise_stats_invalidation_gate PROPERTIES
  LABELS "optimizer_enterprise_stats_invalidation_gate;OEIC-021;optimizer;enterprise;statistics;invalidation;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_index_costing_gate
  optimizer_enterprise_index_costing_gate.cpp
)
target_compile_features(optimizer_enterprise_index_costing_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_index_costing_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_index_costing_gate
  COMMAND optimizer_enterprise_index_costing_gate
)
set_tests_properties(optimizer_enterprise_index_costing_gate PROPERTIES
  LABELS "optimizer_enterprise_index_costing_gate;OEIC-022;optimizer;enterprise;index;costing;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_adaptive_feedback_gate
  optimizer_enterprise_adaptive_feedback_gate.cpp
)
target_compile_features(optimizer_enterprise_adaptive_feedback_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_adaptive_feedback_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_adaptive_feedback_gate
  COMMAND optimizer_enterprise_adaptive_feedback_gate
)
set_tests_properties(optimizer_enterprise_adaptive_feedback_gate PROPERTIES
  LABELS "optimizer_enterprise_adaptive_feedback_gate;OEIC-030;optimizer;enterprise;feedback;adaptive;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_memory_spill_feedback_gate
  optimizer_enterprise_memory_spill_feedback_gate.cpp
)
target_compile_features(optimizer_enterprise_memory_spill_feedback_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_memory_spill_feedback_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_memory_spill_feedback_gate
  COMMAND optimizer_enterprise_memory_spill_feedback_gate
)
set_tests_properties(optimizer_enterprise_memory_spill_feedback_gate PROPERTIES
  LABELS "optimizer_enterprise_memory_spill_feedback_gate;OEIC-031;optimizer;enterprise;feedback;memory;spill;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_feedback_stability_gate
  optimizer_enterprise_feedback_stability_gate.cpp
)
target_compile_features(optimizer_enterprise_feedback_stability_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_feedback_stability_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_feedback_stability_gate
  COMMAND optimizer_enterprise_feedback_stability_gate
)
set_tests_properties(optimizer_enterprise_feedback_stability_gate PROPERTIES
  LABELS "optimizer_enterprise_feedback_stability_gate;OEIC-032;optimizer;enterprise;feedback;stability;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_join_memo_gate
  optimizer_enterprise_join_memo_gate.cpp
)
target_compile_features(optimizer_enterprise_join_memo_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_join_memo_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_join_memo_gate
  COMMAND optimizer_enterprise_join_memo_gate
)
set_tests_properties(optimizer_enterprise_join_memo_gate PROPERTIES
  LABELS "optimizer_enterprise_join_memo_gate;OEIC-040;optimizer;enterprise;join;memo;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_join_controls_gate
  optimizer_enterprise_join_controls_gate.cpp
)
target_compile_features(optimizer_enterprise_join_controls_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_join_controls_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_join_controls_gate
  COMMAND optimizer_enterprise_join_controls_gate
)
set_tests_properties(optimizer_enterprise_join_controls_gate PROPERTIES
  LABELS "optimizer_enterprise_join_controls_gate;OEIC-041;optimizer;enterprise;join;controls;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_ordering_window_gate
  optimizer_enterprise_ordering_window_gate.cpp
)
target_compile_features(optimizer_enterprise_ordering_window_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_ordering_window_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_ordering_window_gate
  COMMAND optimizer_enterprise_ordering_window_gate
)
set_tests_properties(optimizer_enterprise_ordering_window_gate PROPERTIES
  LABELS "optimizer_enterprise_ordering_window_gate;OEIC-042;optimizer;enterprise;ordering;window;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_relational_operator_gate
  optimizer_enterprise_relational_operator_gate.cpp
)
target_compile_features(optimizer_enterprise_relational_operator_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_relational_operator_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_relational_operator_gate
  COMMAND optimizer_enterprise_relational_operator_gate
)
set_tests_properties(optimizer_enterprise_relational_operator_gate PROPERTIES
  LABELS "optimizer_enterprise_relational_operator_gate;OEIC-043;optimizer;enterprise;relational;operators;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_runtime_payload_gate
  optimizer_enterprise_runtime_payload_gate.cpp
)
target_compile_features(optimizer_enterprise_runtime_payload_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_runtime_payload_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_runtime_payload_gate
  COMMAND optimizer_enterprise_runtime_payload_gate
)
set_tests_properties(optimizer_enterprise_runtime_payload_gate PROPERTIES
  LABELS "optimizer_enterprise_runtime_payload_gate;OEIC-050;optimizer;enterprise;explain;runtime_payload;driver_visible;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_plan_cache_gate
  optimizer_enterprise_plan_cache_gate.cpp
)
target_compile_features(optimizer_enterprise_plan_cache_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_plan_cache_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_plan_cache_gate
  COMMAND optimizer_enterprise_plan_cache_gate
)
set_tests_properties(optimizer_enterprise_plan_cache_gate PROPERTIES
  LABELS "optimizer_enterprise_plan_cache_gate;OEIC-051;ELER-106;ELER-GATE-106;optimizer;enterprise;plan_cache;compatibility_epoch;format_compatibility_epoch;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_runtime_filter_prefetch_gate
  optimizer_enterprise_runtime_filter_prefetch_gate.cpp
)
target_compile_features(optimizer_enterprise_runtime_filter_prefetch_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_runtime_filter_prefetch_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_runtime_filter_prefetch_gate
  COMMAND optimizer_enterprise_runtime_filter_prefetch_gate
)
set_tests_properties(optimizer_enterprise_runtime_filter_prefetch_gate PROPERTIES
  LABELS "optimizer_enterprise_runtime_filter_prefetch_gate;OEIC-052;optimizer;enterprise;runtime_filter;prefetch;late_materialization;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_route_validation_gate
  optimizer_enterprise_route_validation_gate.cpp
)
target_compile_features(optimizer_enterprise_route_validation_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_route_validation_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_route_validation_gate
  COMMAND optimizer_enterprise_route_validation_gate
)
set_tests_properties(optimizer_enterprise_route_validation_gate PROPERTIES
  LABELS "optimizer_enterprise_route_validation_gate;OEIC-080;OEIC-081;OEIC-082;OEIC-083;OEIC-084;OEIC-085;optimizer;enterprise;benchmark_clean;live_route;scale;reference;crash_restart;security;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_maintainability_gate
  optimizer_enterprise_maintainability_gate.cpp
)
target_compile_features(optimizer_enterprise_maintainability_gate PRIVATE cxx_std_20)
target_compile_definitions(
  optimizer_enterprise_maintainability_gate
  PRIVATE
    SCRATCHBIRD_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)
add_test(
  NAME optimizer_enterprise_maintainability_gate
  COMMAND optimizer_enterprise_maintainability_gate
)
set_tests_properties(optimizer_enterprise_maintainability_gate PROPERTIES
  LABELS "optimizer_enterprise_maintainability_gate;OEIC-090;optimizer;enterprise;maintainability;drift"
)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(
  NAME optimizer_enterprise_manifest_drift_gate
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tools/optimizer_enterprise_manifest_drift_gate.py
    --repo ${CMAKE_SOURCE_DIR}/..
)
set_tests_properties(optimizer_enterprise_manifest_drift_gate PROPERTIES
  LABELS "optimizer_enterprise_manifest_drift_gate;OEIC-091;optimizer;enterprise;manifest;drift"
)

add_executable(
  optimizer_enterprise_specialized_fusion_gate
  optimizer_enterprise_specialized_fusion_gate.cpp
)
target_compile_features(optimizer_enterprise_specialized_fusion_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_specialized_fusion_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_specialized_fusion_gate
  COMMAND optimizer_enterprise_specialized_fusion_gate
)
set_tests_properties(optimizer_enterprise_specialized_fusion_gate PROPERTIES
  LABELS "optimizer_enterprise_specialized_fusion_gate;OEIC-060;optimizer;enterprise;specialized;fusion;nosql;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_vector_ann_gate
  optimizer_enterprise_vector_ann_gate.cpp
)
target_compile_features(optimizer_enterprise_vector_ann_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_vector_ann_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_vector_ann_gate
  COMMAND optimizer_enterprise_vector_ann_gate
)
set_tests_properties(optimizer_enterprise_vector_ann_gate PROPERTIES
  LABELS "optimizer_enterprise_vector_ann_gate;OEIC-061;optimizer;enterprise;vector;ann;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_text_wand_gate
  optimizer_enterprise_text_wand_gate.cpp
)
target_compile_features(optimizer_enterprise_text_wand_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_text_wand_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_text_wand_gate
  COMMAND optimizer_enterprise_text_wand_gate
)
set_tests_properties(optimizer_enterprise_text_wand_gate PROPERTIES
  LABELS "optimizer_enterprise_text_wand_gate;OEIC-062;optimizer;enterprise;text;wand;search;mga_transaction_regression"
)

add_executable(
  optimizer_enterprise_graph_document_gate
  optimizer_enterprise_graph_document_gate.cpp
)
target_compile_features(optimizer_enterprise_graph_document_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_enterprise_graph_document_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_enterprise_graph_document_gate
  COMMAND optimizer_enterprise_graph_document_gate
)
set_tests_properties(optimizer_enterprise_graph_document_gate PROPERTIES
  LABELS "optimizer_enterprise_graph_document_gate;OEIC-063;optimizer;enterprise;graph;document;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_030_gate
  optimizer_deficiency_odf_030_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_030_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_030_gate
  PRIVATE
    sb_engine_internal_api
)
add_test(
  NAME optimizer_deficiency_odf_030_gate
  COMMAND optimizer_deficiency_odf_030_gate
)
set_tests_properties(optimizer_deficiency_odf_030_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_030_gate;ODF-030;optimizer_deficiency;optimizer;dml;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_031_gate
  optimizer_deficiency_odf_031_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_031_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_031_gate
  PRIVATE
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_database
)
add_test(
  NAME optimizer_deficiency_odf_031_gate
  COMMAND optimizer_deficiency_odf_031_gate
)
set_tests_properties(optimizer_deficiency_odf_031_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_031_gate;ODF-031;optimizer_deficiency;optimizer;dml;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_032_gate
  optimizer_deficiency_odf_032_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_032_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_032_gate
  PRIVATE
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_database
)
add_test(
  NAME optimizer_deficiency_odf_032_gate
  COMMAND optimizer_deficiency_odf_032_gate
)
set_tests_properties(optimizer_deficiency_odf_032_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_032_gate;ODF-032;optimizer_deficiency;optimizer;dml;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_033_gate
  optimizer_deficiency_odf_033_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_033_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_033_gate
  PRIVATE
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_database
)
add_test(
  NAME optimizer_deficiency_odf_033_gate
  COMMAND optimizer_deficiency_odf_033_gate
)
set_tests_properties(optimizer_deficiency_odf_033_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_033_gate;ODF-033;optimizer_deficiency;optimizer;dml;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_034_gate
  optimizer_deficiency_odf_034_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_034_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_034_gate
  PRIVATE
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_database
)
add_test(
  NAME optimizer_deficiency_odf_034_gate
  COMMAND optimizer_deficiency_odf_034_gate
)
set_tests_properties(optimizer_deficiency_odf_034_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_034_gate;ODF-034;optimizer_deficiency;optimizer;dml;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_035_gate
  optimizer_deficiency_odf_035_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_035_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_035_gate
  PRIVATE
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_database
)
add_test(
  NAME optimizer_deficiency_odf_035_gate
  COMMAND optimizer_deficiency_odf_035_gate
)
set_tests_properties(optimizer_deficiency_odf_035_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_035_gate;ODF-035;optimizer_deficiency;optimizer;dml;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_036_gate
  optimizer_deficiency_odf_036_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_036_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_036_gate
  PRIVATE
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_database
)
add_test(
  NAME optimizer_deficiency_odf_036_gate
  COMMAND optimizer_deficiency_odf_036_gate
)
set_tests_properties(optimizer_deficiency_odf_036_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_036_gate;ODF-036;optimizer_deficiency;optimizer;dml;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_037_gate
  optimizer_deficiency_odf_037_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_037_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_037_gate
  PRIVATE
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_database
)
add_test(
  NAME optimizer_deficiency_odf_037_gate
  COMMAND optimizer_deficiency_odf_037_gate
)
set_tests_properties(optimizer_deficiency_odf_037_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_037_gate;ODF-037;optimizer_deficiency;optimizer;dml;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_038_gate
  optimizer_deficiency_odf_038_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_038_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_038_gate
  PRIVATE
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_database
)
add_test(
  NAME optimizer_deficiency_odf_038_gate
  COMMAND optimizer_deficiency_odf_038_gate
)
set_tests_properties(optimizer_deficiency_odf_038_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_038_gate;ODF-038;optimizer_deficiency;optimizer;dml;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_039_gate
  optimizer_deficiency_odf_039_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_039_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_039_gate
  PRIVATE
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_database
)
add_test(
  NAME optimizer_deficiency_odf_039_gate
  COMMAND optimizer_deficiency_odf_039_gate
)
set_tests_properties(optimizer_deficiency_odf_039_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_039_gate;ODF-039;optimizer_deficiency;optimizer;dml;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_040_gate
  optimizer_deficiency_odf_040_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_040_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_040_gate
  PRIVATE
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_database
)
add_test(
  NAME optimizer_deficiency_odf_040_gate
  COMMAND optimizer_deficiency_odf_040_gate
)
set_tests_properties(optimizer_deficiency_odf_040_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_040_gate;ODF-040;optimizer_deficiency;bulk;dml;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_041_gate
  optimizer_deficiency_odf_041_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_041_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_041_gate
  PRIVATE
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_database
)
add_test(
  NAME optimizer_deficiency_odf_041_gate
  COMMAND optimizer_deficiency_odf_041_gate
)
set_tests_properties(optimizer_deficiency_odf_041_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_041_gate;ODF-041;optimizer_deficiency;bulk;dml;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_042_gate
  optimizer_deficiency_odf_042_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_042_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_042_gate
  PRIVATE
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_database
)
add_test(
  NAME optimizer_deficiency_odf_042_gate
  COMMAND optimizer_deficiency_odf_042_gate
)
set_tests_properties(optimizer_deficiency_odf_042_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_042_gate;ODF-042;optimizer_deficiency;bulk;dml;storage;agents;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_043_gate
  optimizer_deficiency_odf_043_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_043_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_043_gate
  PRIVATE
    sb_core_index
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_database
)
add_test(
  NAME optimizer_deficiency_odf_043_gate
  COMMAND optimizer_deficiency_odf_043_gate
)
set_tests_properties(optimizer_deficiency_odf_043_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_043_gate;ODF-043;optimizer_deficiency;index;dml;bulk;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_044_gate
  optimizer_deficiency_odf_044_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_044_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_044_gate
  PRIVATE
    sb_core_index
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_database
)
add_test(
  NAME optimizer_deficiency_odf_044_gate
  COMMAND optimizer_deficiency_odf_044_gate
)
set_tests_properties(optimizer_deficiency_odf_044_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_044_gate;ODF-044;optimizer_deficiency;index;bulk;dml;ddl;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_045_gate
  optimizer_deficiency_odf_045_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_045_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_045_gate
  PRIVATE
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_database
)
add_test(
  NAME optimizer_deficiency_odf_045_gate
  COMMAND optimizer_deficiency_odf_045_gate
)
set_tests_properties(optimizer_deficiency_odf_045_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_045_gate;ODF-045;optimizer_deficiency;index;nosql;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_046_gate
  optimizer_deficiency_odf_046_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_046_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_046_gate
  PRIVATE
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_database
)
add_test(
  NAME optimizer_deficiency_odf_046_gate
  COMMAND optimizer_deficiency_odf_046_gate
)
set_tests_properties(optimizer_deficiency_odf_046_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_046_gate;ODF-046;IPAR-P4-05;optimizer_deficiency;bulk;constraints;dml;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_047_gate
  optimizer_deficiency_odf_047_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_047_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_047_gate
  PRIVATE
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_database
)
add_test(
  NAME optimizer_deficiency_odf_047_gate
  COMMAND optimizer_deficiency_odf_047_gate
)
set_tests_properties(optimizer_deficiency_odf_047_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_047_gate;ODF-047;optimizer_deficiency;bulk;storage;optimizer;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_048_gate
  optimizer_deficiency_odf_048_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_048_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_048_gate
  PRIVATE
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_database
)
add_test(
  NAME optimizer_deficiency_odf_048_gate
  COMMAND optimizer_deficiency_odf_048_gate
)
set_tests_properties(optimizer_deficiency_odf_048_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_048_gate;ODF-048;optimizer_deficiency;dml;nosql;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_049_gate
  optimizer_deficiency_odf_049_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_049_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_049_gate
  PRIVATE
    sb_core_metrics
    sb_core_uuid
    sb_storage_page
)
add_test(
  NAME optimizer_deficiency_odf_049_gate
  COMMAND optimizer_deficiency_odf_049_gate
)
set_tests_properties(optimizer_deficiency_odf_049_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_049_gate;ODF-049;optimizer_deficiency;storage;metrics;bulk;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_055_gate
  optimizer_deficiency_odf_055_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_055_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_055_gate
  PRIVATE
    sb_core_metrics
    sb_core_uuid
    sb_storage_page
)
add_test(
  NAME optimizer_deficiency_odf_055_gate
  COMMAND optimizer_deficiency_odf_055_gate
)
set_tests_properties(optimizer_deficiency_odf_055_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_055_gate;ODF-055;optimizer_deficiency;storage;metrics;scan_resistant;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_050_gate
  optimizer_deficiency_odf_050_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_050_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_050_gate
  PRIVATE
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_database
    sb_transaction_mga
)
add_test(
  NAME optimizer_deficiency_odf_050_gate
  COMMAND optimizer_deficiency_odf_050_gate
)
set_tests_properties(optimizer_deficiency_odf_050_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_050_gate;ODF-050;optimizer_deficiency;optimizer;dml;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_051_gate
  optimizer_deficiency_odf_051_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_051_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_051_gate
  PRIVATE
    sb_core_uuid
    sb_engine_optimizer
    sb_storage_page
    sb_transaction_mga
)
add_test(
  NAME optimizer_deficiency_odf_051_gate
  COMMAND optimizer_deficiency_odf_051_gate
)
set_tests_properties(optimizer_deficiency_odf_051_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_051_gate;ODF-051;optimizer_deficiency;optimizer;storage;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_052_gate
  optimizer_deficiency_odf_052_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_052_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_052_gate
  PRIVATE
    sb_core_index
    sb_core_uuid
    sb_transaction_mga
)
add_test(
  NAME optimizer_deficiency_odf_052_gate
  COMMAND optimizer_deficiency_odf_052_gate
)
set_tests_properties(optimizer_deficiency_odf_052_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_052_gate;ODF-052;optimizer_deficiency;optimizer;index;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_053_gate
  optimizer_deficiency_odf_053_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_053_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_053_gate
  PRIVATE
    sb_core_index
)
add_test(
  NAME optimizer_deficiency_odf_053_gate
  COMMAND optimizer_deficiency_odf_053_gate
)
set_tests_properties(optimizer_deficiency_odf_053_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_053_gate;ODF-053;optimizer_deficiency;optimizer;index;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_054_gate
  optimizer_deficiency_odf_054_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_054_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_054_gate
  PRIVATE
    sb_core_index
    sb_transaction_mga
)
add_test(
  NAME optimizer_deficiency_odf_054_gate
  COMMAND optimizer_deficiency_odf_054_gate
)
set_tests_properties(optimizer_deficiency_odf_054_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_054_gate;ODF-054;optimizer_deficiency;optimizer;index;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_056_gate
  optimizer_deficiency_odf_056_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_056_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_056_gate
  PRIVATE
    sb_engine_internal_api
    sb_engine_optimizer
)
add_test(
  NAME optimizer_deficiency_odf_056_gate
  COMMAND optimizer_deficiency_odf_056_gate
)
set_tests_properties(optimizer_deficiency_odf_056_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_056_gate;ODF-056;optimizer_deficiency;optimizer;index;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_057_gate
  optimizer_deficiency_odf_057_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_057_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_057_gate
  PRIVATE
    sb_core_agents
    sb_core_index
    sb_core_uuid
    sb_transaction_mga
)
add_test(
  NAME optimizer_deficiency_odf_057_gate
  COMMAND optimizer_deficiency_odf_057_gate
)
set_tests_properties(optimizer_deficiency_odf_057_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_057_gate;ODF-057;optimizer_deficiency;optimizer;agents;index;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_058_gate
  optimizer_deficiency_odf_058_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_058_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_058_gate
  PRIVATE
    sb_core_uuid
    sb_engine_optimizer
)
add_test(
  NAME optimizer_deficiency_odf_058_gate
  COMMAND optimizer_deficiency_odf_058_gate
)
set_tests_properties(optimizer_deficiency_odf_058_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_058_gate;ODF-058;optimizer_deficiency;optimizer;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_059_gate
  optimizer_deficiency_odf_059_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_059_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_059_gate
  PRIVATE
    sb_core_uuid
    sb_engine_executor
    sb_storage_page
)
add_test(
  NAME optimizer_deficiency_odf_059_gate
  COMMAND optimizer_deficiency_odf_059_gate
)
set_tests_properties(optimizer_deficiency_odf_059_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_059_gate;ODF-059;optimizer_deficiency;executor;storage;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_060_gate
  optimizer_deficiency_odf_060_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_060_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_060_gate
  PRIVATE
    sb_core_index
    sb_core_uuid
)
add_test(
  NAME optimizer_deficiency_odf_060_gate
  COMMAND optimizer_deficiency_odf_060_gate
)
set_tests_properties(optimizer_deficiency_odf_060_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_060_gate;ODF-060;optimizer_deficiency;uuid;index;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_061_gate
  optimizer_deficiency_odf_061_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_061_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_061_gate
  PRIVATE
    sb_core_uuid
    sb_storage_filespace
    sb_storage_page
)
add_test(
  NAME optimizer_deficiency_odf_061_gate
  COMMAND optimizer_deficiency_odf_061_gate
)
set_tests_properties(optimizer_deficiency_odf_061_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_061_gate;ODF-061;optimizer_deficiency;storage;filespace;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_062_gate
  optimizer_deficiency_odf_062_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_062_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_062_gate
  PRIVATE
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_page
)
add_test(
  NAME optimizer_deficiency_odf_062_gate
  COMMAND optimizer_deficiency_odf_062_gate
)
set_tests_properties(optimizer_deficiency_odf_062_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_062_gate;ODF-062;optimizer_deficiency;storage;nosql;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_063_gate
  optimizer_deficiency_odf_063_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_063_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_063_gate
  PRIVATE
    sb_core_uuid
    sb_engine_internal_api
    sb_storage_page
)
add_test(
  NAME optimizer_deficiency_odf_063_gate
  COMMAND optimizer_deficiency_odf_063_gate
)
set_tests_properties(optimizer_deficiency_odf_063_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_063_gate;ODF-063;optimizer_deficiency;storage;dml;nosql;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_070_gate
  optimizer_deficiency_odf_070_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_070_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_070_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_deficiency_odf_070_gate
  COMMAND optimizer_deficiency_odf_070_gate
)
set_tests_properties(optimizer_deficiency_odf_070_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_070_gate;ODF-070;optimizer_deficiency;optimizer;nosql;mga;benchmark_clean;mga_transaction_regression"
)

if(NOT TARGET sb_engine_sblr)
  add_subdirectory(
    ${PROJECT_SOURCE_DIR}/src/engine/sblr
    ${CMAKE_BINARY_DIR}/engine_sblr_for_optimizer_tests
  )
endif()

add_executable(
  optimizer_deficiency_odf_071_gate
  optimizer_deficiency_odf_071_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_071_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_071_gate
  PRIVATE
    sb_engine_sblr
)
add_test(
  NAME optimizer_deficiency_odf_071_gate
  COMMAND optimizer_deficiency_odf_071_gate
)
set_tests_properties(optimizer_deficiency_odf_071_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_071_gate;ODF-071;optimizer_deficiency;optimizer;nosql;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_072_gate
  optimizer_deficiency_odf_072_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_072_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_072_gate
  PRIVATE
    sb_engine_internal_api
)
add_test(
  NAME optimizer_deficiency_odf_072_gate
  COMMAND optimizer_deficiency_odf_072_gate
)
set_tests_properties(optimizer_deficiency_odf_072_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_072_gate;ODF-072;optimizer_deficiency;optimizer;nosql;index;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_073_gate
  optimizer_deficiency_odf_073_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_073_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_073_gate
  PRIVATE
    sb_engine_internal_api
)
add_test(
  NAME optimizer_deficiency_odf_073_gate
  COMMAND optimizer_deficiency_odf_073_gate
)
set_tests_properties(optimizer_deficiency_odf_073_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_073_gate;ODF-073;optimizer_deficiency;optimizer;nosql;index;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_074_gate
  optimizer_deficiency_odf_074_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_074_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_074_gate
  PRIVATE
    sb_engine_internal_api
)
add_test(
  NAME optimizer_deficiency_odf_074_gate
  COMMAND optimizer_deficiency_odf_074_gate
)
set_tests_properties(optimizer_deficiency_odf_074_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_074_gate;ODF-074;optimizer_deficiency;optimizer;nosql;index;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_075_gate
  optimizer_deficiency_odf_075_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_075_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_075_gate
  PRIVATE
    sb_engine_internal_api
)
add_test(
  NAME optimizer_deficiency_odf_075_gate
  COMMAND optimizer_deficiency_odf_075_gate
)
set_tests_properties(optimizer_deficiency_odf_075_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_075_gate;ODF-075;optimizer_deficiency;optimizer;nosql;executor;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_076_gate
  optimizer_deficiency_odf_076_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_076_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_076_gate
  PRIVATE
    sb_engine_internal_api
)
add_test(
  NAME optimizer_deficiency_odf_076_gate
  COMMAND optimizer_deficiency_odf_076_gate
)
set_tests_properties(optimizer_deficiency_odf_076_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_076_gate;ODF-076;optimizer_deficiency;optimizer;nosql;storage;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_077_gate
  optimizer_deficiency_odf_077_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_077_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_077_gate
  PRIVATE
    sb_engine_internal_api
)
add_test(
  NAME optimizer_deficiency_odf_077_gate
  COMMAND optimizer_deficiency_odf_077_gate
)
set_tests_properties(optimizer_deficiency_odf_077_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_077_gate;ODF-077;optimizer_deficiency;optimizer;nosql;agents;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_078_gate
  optimizer_deficiency_odf_078_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_078_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_078_gate
  PRIVATE
    sb_engine_internal_api
)
add_test(
  NAME optimizer_deficiency_odf_078_gate
  COMMAND optimizer_deficiency_odf_078_gate
)
set_tests_properties(optimizer_deficiency_odf_078_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_078_gate;ODF-078;optimizer_deficiency;optimizer;nosql;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_079_gate
  optimizer_deficiency_odf_079_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_079_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_079_gate
  PRIVATE
    sb_engine_sblr
)
add_test(
  NAME optimizer_deficiency_odf_079_gate
  COMMAND optimizer_deficiency_odf_079_gate
)
set_tests_properties(optimizer_deficiency_odf_079_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_079_gate;ODF-079;optimizer_deficiency;optimizer;nosql;agents;observability;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_100_gate
  optimizer_deficiency_odf_100_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_100_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_100_gate
  PRIVATE
    sb_core_agents
    sb_engine_optimizer_contract_only
)
add_test(
  NAME optimizer_deficiency_odf_100_gate
  COMMAND optimizer_deficiency_odf_100_gate
)
set_tests_properties(optimizer_deficiency_odf_100_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_100_gate;ODF-100;optimizer_deficiency;optimizer;agents;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_080_gate
  optimizer_deficiency_odf_080_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_080_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_080_gate
  PRIVATE
    sb_engine_optimizer_contract_only
)
add_test(
  NAME optimizer_deficiency_odf_080_gate
  COMMAND optimizer_deficiency_odf_080_gate
)
set_tests_properties(optimizer_deficiency_odf_080_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_080_gate;ODF-080;optimizer_deficiency;optimizer;nosql;index;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_090_gate
  optimizer_deficiency_odf_090_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_090_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_090_gate
  PRIVATE
    sb_core_index
    sb_engine_executor
)
add_test(
  NAME optimizer_deficiency_odf_090_gate
  COMMAND optimizer_deficiency_odf_090_gate
)
set_tests_properties(optimizer_deficiency_odf_090_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_090_gate;ODF-090;optimizer_deficiency;optimizer;executor;index;mga;benchmark_clean;mga_transaction_regression"
)

if(NOT TARGET sb_wire_result_batch)
  add_subdirectory(
    ${PROJECT_SOURCE_DIR}/src/wire
    ${CMAKE_BINARY_DIR}/wire_result_batch_for_optimizer_tests
  )
endif()
if(NOT TARGET sb_local_ipc)
  add_subdirectory(
    ${PROJECT_SOURCE_DIR}/src/ipc
    ${CMAKE_BINARY_DIR}/local_ipc_for_optimizer_tests
  )
endif()
if(NOT TARGET sbl_parser_server_ipc_schema)
  add_subdirectory(
    ${PROJECT_SOURCE_DIR}/src/wire/parser_server_ipc
    ${CMAKE_BINARY_DIR}/parser_server_ipc_for_optimizer_tests
  )
endif()
if(NOT TARGET sb_local_parser_shared_memory_transport)
  add_subdirectory(
    ${PROJECT_SOURCE_DIR}/src/server
    ${CMAKE_BINARY_DIR}/server_for_optimizer_odf_099_tests
  )
endif()

add_executable(
  optimizer_deficiency_odf_091_gate
  optimizer_deficiency_odf_091_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_091_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_091_gate
  PRIVATE
    sb_engine_executor
    sb_wire_result_batch
)
add_test(
  NAME optimizer_deficiency_odf_091_gate
  COMMAND optimizer_deficiency_odf_091_gate
)
set_tests_properties(optimizer_deficiency_odf_091_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_091_gate;ODF-091;optimizer_deficiency;optimizer;executor;wire;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_098_gate
  optimizer_deficiency_odf_098_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_098_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_098_gate
  PRIVATE
    sb_engine_executor
    sb_wire_result_batch
)
add_test(
  NAME optimizer_deficiency_odf_098_gate
  COMMAND optimizer_deficiency_odf_098_gate
)
set_tests_properties(optimizer_deficiency_odf_098_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_098_gate;ODF-098;optimizer_deficiency;wire;executor;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_099_gate
  optimizer_deficiency_odf_099_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_099_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_099_gate
  PRIVATE
    sb_engine_executor
    sb_local_ipc
    sb_local_parser_shared_memory_transport
    sb_wire_result_batch
    sbl_parser_server_ipc_schema
)
add_test(
  NAME optimizer_deficiency_odf_099_gate
  COMMAND optimizer_deficiency_odf_099_gate
)
set_tests_properties(optimizer_deficiency_odf_099_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_099_gate;ODF-099;optimizer_deficiency;ipc;wire;server;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_092_gate
  optimizer_deficiency_odf_092_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_092_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_092_gate
  PRIVATE
    sb_core_index
    sb_engine_executor
    sb_engine_internal_api
)
add_test(
  NAME optimizer_deficiency_odf_092_gate
  COMMAND optimizer_deficiency_odf_092_gate
)
set_tests_properties(optimizer_deficiency_odf_092_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_092_gate;ODF-092;optimizer_deficiency;optimizer;executor;index;nosql;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_093_gate
  optimizer_deficiency_odf_093_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_093_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_093_gate
  PRIVATE
    sb_core_index
    sb_engine_executor
    sb_storage_page
)
add_test(
  NAME optimizer_deficiency_odf_093_gate
  COMMAND optimizer_deficiency_odf_093_gate
)
set_tests_properties(optimizer_deficiency_odf_093_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_093_gate;ODF-093;optimizer_deficiency;optimizer;executor;storage;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_094_gate
  optimizer_deficiency_odf_094_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_094_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_094_gate
  PRIVATE
    sb_core_memory
    sb_engine_executor
)
add_test(
  NAME optimizer_deficiency_odf_094_gate
  COMMAND optimizer_deficiency_odf_094_gate
)
set_tests_properties(optimizer_deficiency_odf_094_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_094_gate;ODF-094;optimizer_deficiency;optimizer;executor;memory;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  memory_live_operator_grants_gate
  memory_live_operator_grants_gate.cpp
)
target_compile_features(memory_live_operator_grants_gate PRIVATE cxx_std_20)
target_link_libraries(
  memory_live_operator_grants_gate
  PRIVATE
    sb_core_memory
    sb_engine_executor
)
add_test(
  NAME memory_live_operator_grants_gate
  COMMAND memory_live_operator_grants_gate
)
set_tests_properties(memory_live_operator_grants_gate PROPERTIES
  LABELS "memory_live_operator_grants_gate;MMCH-020;memory;executor;operator_memory;optimizer;benchmark_clean;mga_transaction_regression"
)

add_executable(
  memory_cancel_rollback_cleanup_gate
  memory_cancel_rollback_cleanup_gate.cpp
)
target_compile_features(memory_cancel_rollback_cleanup_gate PRIVATE cxx_std_20)
target_link_libraries(
  memory_cancel_rollback_cleanup_gate
  PRIVATE
    sb_core_memory
    sb_engine_executor
)
add_test(
  NAME memory_cancel_rollback_cleanup_gate
  COMMAND memory_cancel_rollback_cleanup_gate
)
set_tests_properties(memory_cancel_rollback_cleanup_gate PROPERTIES
  LABELS "memory_cancel_rollback_cleanup_gate;MMCH-022;memory;executor;rollback;cancel;cleanup;optimizer;mga_transaction_regression"
)

add_executable(
  memory_cumulative_context_accounting_gate
  memory_cumulative_context_accounting_gate.cpp
)
target_compile_features(memory_cumulative_context_accounting_gate PRIVATE cxx_std_20)
target_link_libraries(
  memory_cumulative_context_accounting_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME memory_cumulative_context_accounting_gate
  COMMAND memory_cumulative_context_accounting_gate
)
set_tests_properties(memory_cumulative_context_accounting_gate PROPERTIES
  LABELS "memory_cumulative_context_accounting_gate;MMCH-010;memory;allocator;context_accounting;mga_transaction_regression"
)

add_executable(
  memory_allocator_stress_fragmentation_contention_gate
  memory_allocator_stress_fragmentation_contention_gate.cpp
)
target_compile_features(memory_allocator_stress_fragmentation_contention_gate PRIVATE cxx_std_20)
target_link_libraries(
  memory_allocator_stress_fragmentation_contention_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME memory_allocator_stress_fragmentation_contention_gate
  COMMAND memory_allocator_stress_fragmentation_contention_gate
)
set_tests_properties(memory_allocator_stress_fragmentation_contention_gate PROPERTIES
  LABELS "memory_allocator_stress_fragmentation_contention_gate;MMCH-012;memory;allocator;fragmentation;contention;benchmark_clean;mga_transaction_regression"
)

add_executable(
  memory_allocation_failure_injection_gate
  memory_allocation_failure_injection_gate.cpp
)
target_compile_features(memory_allocation_failure_injection_gate PRIVATE cxx_std_20)
target_compile_definitions(memory_allocation_failure_injection_gate PRIVATE
  SCRATCHBIRD_MEMORY_FAILURE_INJECTION_TEST_GUARD=1
)
target_link_libraries(
  memory_allocation_failure_injection_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME memory_allocation_failure_injection_gate
  COMMAND memory_allocation_failure_injection_gate
)
set_tests_properties(memory_allocation_failure_injection_gate PROPERTIES
  LABELS "memory_allocation_failure_injection_gate;MMCH-014;memory;allocator;failure_injection;fault_injection;mga_transaction_regression"
)

add_executable(
  memory_protected_material_lifecycle_gate
  memory_protected_material_lifecycle_gate.cpp
)
target_compile_features(memory_protected_material_lifecycle_gate PRIVATE cxx_std_20)
target_link_libraries(
  memory_protected_material_lifecycle_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME memory_protected_material_lifecycle_gate
  COMMAND memory_protected_material_lifecycle_gate
)
set_tests_properties(memory_protected_material_lifecycle_gate PROPERTIES
  LABELS "memory_protected_material_lifecycle_gate;MMCH-015;memory;security;protected_material;zeroization;mga_transaction_regression"
)

add_executable(
  memory_resource_governance_reservation_lifecycle_gate
  memory_resource_governance_reservation_lifecycle_gate.cpp
)
target_compile_features(memory_resource_governance_reservation_lifecycle_gate PRIVATE cxx_std_20)
target_link_libraries(
  memory_resource_governance_reservation_lifecycle_gate
  PRIVATE
    sb_core_agents
)
add_test(
  NAME memory_resource_governance_reservation_lifecycle_gate
  COMMAND memory_resource_governance_reservation_lifecycle_gate
)
set_tests_properties(memory_resource_governance_reservation_lifecycle_gate PROPERTIES
  LABELS "memory_resource_governance_reservation_lifecycle_gate;MMCH-030;memory;resource_governance;reservation_lifecycle;mga_transaction_regression"
)

add_executable(
  memory_hierarchical_budget_gate
  memory_hierarchical_budget_gate.cpp
)
target_compile_features(memory_hierarchical_budget_gate PRIVATE cxx_std_20)
target_link_libraries(
  memory_hierarchical_budget_gate
  PRIVATE
    sb_core_agents
)
add_test(
  NAME memory_hierarchical_budget_gate
  COMMAND memory_hierarchical_budget_gate
)
set_tests_properties(memory_hierarchical_budget_gate PROPERTIES
  LABELS "memory_hierarchical_budget_gate;MMCH-017;memory;resource_governance;hierarchical_budget;mga_transaction_regression"
)

add_executable(
  memory_unified_heap_spill_budget_gate
  memory_unified_heap_spill_budget_gate.cpp
)
target_compile_features(memory_unified_heap_spill_budget_gate PRIVATE cxx_std_20)
target_link_libraries(
  memory_unified_heap_spill_budget_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME memory_unified_heap_spill_budget_gate
  COMMAND memory_unified_heap_spill_budget_gate
)
set_tests_properties(memory_unified_heap_spill_budget_gate PROPERTIES
  LABELS "memory_unified_heap_spill_budget_gate;MMCH-031;memory;resource_governance;unified_heap_spill_budget;mga_transaction_regression"
)

add_executable(
  memory_typed_arena_adapters_gate
  memory_typed_arena_adapters_gate.cpp
)
target_compile_features(memory_typed_arena_adapters_gate PRIVATE cxx_std_20)
target_link_libraries(
  memory_typed_arena_adapters_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME memory_typed_arena_adapters_gate
  COMMAND memory_typed_arena_adapters_gate
)
set_tests_properties(memory_typed_arena_adapters_gate PROPERTIES
  LABELS "memory_typed_arena_adapters_gate;MMCH-060;memory;typed_arena;optimizer;executor;mga_transaction_regression"
)

add_executable(
  memory_executor_planner_typed_arena_migration_gate
  memory_executor_planner_typed_arena_migration_gate.cpp
)
target_compile_features(memory_executor_planner_typed_arena_migration_gate PRIVATE cxx_std_20)
target_link_libraries(
  memory_executor_planner_typed_arena_migration_gate
  PRIVATE
    sb_core_memory
    sb_engine_executor
    sb_engine_optimizer_contract_only
)
add_test(
  NAME memory_executor_planner_typed_arena_migration_gate
  COMMAND memory_executor_planner_typed_arena_migration_gate
)
set_tests_properties(memory_executor_planner_typed_arena_migration_gate PROPERTIES
  LABELS "memory_executor_planner_typed_arena_migration_gate;MMCH-061;memory;typed_arena;optimizer;executor;mga_transaction_regression"
)

add_executable(
  memory_reserved_category_claims_gate
  memory_reserved_category_claims_gate.cpp
)
target_compile_features(memory_reserved_category_claims_gate PRIVATE cxx_std_20)
target_link_libraries(
  memory_reserved_category_claims_gate
  PRIVATE
    sb_core_memory
)
add_test(
  NAME memory_reserved_category_claims_gate
  COMMAND memory_reserved_category_claims_gate
)
set_tests_properties(memory_reserved_category_claims_gate PROPERTIES
  LABELS "memory_reserved_category_claims_gate;MMCH-082;MMCH_RESERVED_MEMORY_CATEGORY_CLAIM_GATES;memory;release_claims;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_095_gate
  optimizer_deficiency_odf_095_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_095_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_095_gate
  PRIVATE
    sb_core_index
    sb_engine_executor
    sb_engine_optimizer_contract_only
)
add_test(
  NAME optimizer_deficiency_odf_095_gate
  COMMAND optimizer_deficiency_odf_095_gate
)
set_tests_properties(optimizer_deficiency_odf_095_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_095_gate;ODF-095;optimizer_deficiency;optimizer;executor;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_096_gate
  optimizer_deficiency_odf_096_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_096_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_096_gate
  PRIVATE
    sb_core_index
    sb_engine_executor
    sb_engine_optimizer_contract_only
)
add_test(
  NAME optimizer_deficiency_odf_096_gate
  COMMAND optimizer_deficiency_odf_096_gate
)
set_tests_properties(optimizer_deficiency_odf_096_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_096_gate;ODF-096;optimizer_deficiency;optimizer;executor;sblr;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_104_gate
  optimizer_deficiency_odf_104_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_104_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_104_gate
  PRIVATE
    sb_engine_sblr
)
add_test(
  NAME optimizer_deficiency_odf_104_gate
  COMMAND optimizer_deficiency_odf_104_gate
)
set_tests_properties(optimizer_deficiency_odf_104_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_104_gate;ODF-104;optimizer_deficiency;sblr;native_compile;executor;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_105_gate
  optimizer_deficiency_odf_105_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_105_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_105_gate
  PRIVATE
    sb_engine_executor
)
add_test(
  NAME optimizer_deficiency_odf_105_gate
  COMMAND optimizer_deficiency_odf_105_gate
)
set_tests_properties(optimizer_deficiency_odf_105_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_105_gate;ODF-105;optimizer_deficiency;gpu;simd;executor;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_097_gate
  optimizer_deficiency_odf_097_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_097_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_097_gate
  PRIVATE
    sb_engine_optimizer_contract_only
    sb_storage_page
)
add_test(
  NAME optimizer_deficiency_odf_097_gate
  COMMAND optimizer_deficiency_odf_097_gate
)
set_tests_properties(optimizer_deficiency_odf_097_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_097_gate;ODF-097;optimizer_deficiency;optimizer;storage;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_102_gate
  optimizer_deficiency_odf_102_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_102_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_102_gate
  PRIVATE
    sb_storage_page
)
add_test(
  NAME optimizer_deficiency_odf_102_gate
  COMMAND optimizer_deficiency_odf_102_gate
)
set_tests_properties(optimizer_deficiency_odf_102_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_102_gate;ODF-102;IPAR-P3-11;IPAR-P7-13;optimizer_deficiency;storage;performance;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_103_gate
  optimizer_deficiency_odf_103_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_103_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_103_gate
  PRIVATE
    sb_engine_executor
    sb_storage_page
    sb_core_bulk_load
)
add_test(
  NAME optimizer_deficiency_odf_103_gate
  COMMAND optimizer_deficiency_odf_103_gate
)
set_tests_properties(optimizer_deficiency_odf_103_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_103_gate;ODF-103;optimizer_deficiency;executor;storage;bulk;performance;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_101_gate
  optimizer_deficiency_odf_101_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_101_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_101_gate
  PRIVATE
    sb_core_agents
    sb_core_metrics
)
add_test(
  NAME optimizer_deficiency_odf_101_gate
  COMMAND optimizer_deficiency_odf_101_gate
)
set_tests_properties(optimizer_deficiency_odf_101_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_101_gate;ODF-101;optimizer_deficiency;agents;metrics;observability;mga;benchmark_clean;mga_transaction_regression"
)

if(NOT TARGET sb_server_resource_governance)
  add_subdirectory(
    ${PROJECT_SOURCE_DIR}/src/server
    ${CMAKE_BINARY_DIR}/server_for_optimizer_odf106_tests
  )
endif()

add_executable(
  optimizer_deficiency_odf_106_gate
  optimizer_deficiency_odf_106_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_106_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_106_gate
  PRIVATE
    sb_core_resource_governance
    sb_engine_executor
    sb_engine_sblr
    sb_server_resource_governance
    sb_storage_page
)
add_test(
  NAME optimizer_deficiency_odf_106_gate
  COMMAND optimizer_deficiency_odf_106_gate
)
set_tests_properties(optimizer_deficiency_odf_106_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_106_gate;ODF-106;optimizer_deficiency;runtime;agents;server;executor;quotas;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_107_gate
  optimizer_deficiency_odf_107_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_107_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_107_gate
  PRIVATE
    sb_core_index
    sb_core_platform
    sb_engine_executor
    sb_engine_gpu_acceleration
    sb_engine_native_compile
    sb_local_ipc
    sb_local_parser_shared_memory_transport
    sb_storage_page
    sb_wire_result_batch
)
add_test(
  NAME optimizer_deficiency_odf_107_gate
  COMMAND optimizer_deficiency_odf_107_gate
)
set_tests_properties(optimizer_deficiency_odf_107_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_107_gate;ODF-107;optimizer_deficiency;platform;runtime;wire;ipc;uuid;storage;native_compile;gpu;simd;mga;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_deficiency_odf_108_gate
  optimizer_deficiency_odf_108_gate.cpp
)
target_compile_features(optimizer_deficiency_odf_108_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_deficiency_odf_108_gate
  PRIVATE
    sb_core_uuid
    sb_engine_internal_api
)
add_test(
  NAME optimizer_deficiency_odf_108_gate
  COMMAND optimizer_deficiency_odf_108_gate
)
set_tests_properties(optimizer_deficiency_odf_108_gate PROPERTIES
  LABELS "optimizer_deficiency_odf_108_gate;ODF-108;optimizer_deficiency;catalog;management;observability;explain;support;runtime;mga;benchmark_clean;mga_transaction_regression;security"
)

add_executable(
  optimizer_planner_opch_011_gate
  optimizer_planner_opch_011_gate.cpp
)
target_compile_features(optimizer_planner_opch_011_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_planner_opch_011_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_planner_opch_011_gate
  COMMAND optimizer_planner_opch_011_gate
)
set_tests_properties(optimizer_planner_opch_011_gate PROPERTIES
  LABELS "optimizer_planner_opch_011_gate;OPCH-011;optimizer;planner;engine_boundary;mga_transaction_regression"
)

add_executable(
  optimizer_planner_opch_join_gate
  optimizer_planner_opch_join_gate.cpp
)
target_compile_features(optimizer_planner_opch_join_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_planner_opch_join_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_planner_opch_join_gate
  COMMAND optimizer_planner_opch_join_gate
)
set_tests_properties(optimizer_planner_opch_join_gate PROPERTIES
  LABELS "optimizer_planner_opch_join_gate;OPCH-020;OPCH-021;OPCH-022;optimizer;join;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_planner_opch_030_031_gate
  optimizer_planner_opch_030_031_gate.cpp
)
target_compile_features(optimizer_planner_opch_030_031_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_planner_opch_030_031_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_planner_opch_030_031_gate
  COMMAND optimizer_planner_opch_030_031_gate
)
set_tests_properties(optimizer_planner_opch_030_031_gate PROPERTIES
  LABELS "optimizer_planner_opch_030_031_gate;OPCH-030;OPCH-031;optimizer;plan_cache;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_planner_opch_runtime_payload_feedback_gate
  optimizer_planner_opch_runtime_payload_feedback_gate.cpp
)
target_compile_features(optimizer_planner_opch_runtime_payload_feedback_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_planner_opch_runtime_payload_feedback_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_planner_opch_runtime_payload_feedback_gate
  COMMAND optimizer_planner_opch_runtime_payload_feedback_gate
)
set_tests_properties(optimizer_planner_opch_runtime_payload_feedback_gate PROPERTIES
  LABELS "optimizer_planner_opch_runtime_payload_feedback_gate;OPCH-040;OPCH-041;optimizer;explain;adaptive_feedback;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_planner_opch_statistics_index_gate
  optimizer_planner_opch_statistics_index_gate.cpp
)
target_compile_features(optimizer_planner_opch_statistics_index_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_planner_opch_statistics_index_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_planner_opch_statistics_index_gate
  COMMAND optimizer_planner_opch_statistics_index_gate
)
set_tests_properties(optimizer_planner_opch_statistics_index_gate PROPERTIES
  LABELS "optimizer_planner_opch_statistics_index_gate;OPCH-050;OPCH-051;OPCH-052;optimizer;statistics;index;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_planner_opch_pressure_inputs_gate
  optimizer_planner_opch_pressure_inputs_gate.cpp
)
target_compile_features(optimizer_planner_opch_pressure_inputs_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_planner_opch_pressure_inputs_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_planner_opch_pressure_inputs_gate
  COMMAND optimizer_planner_opch_pressure_inputs_gate
)
set_tests_properties(optimizer_planner_opch_pressure_inputs_gate PROPERTIES
  LABELS "optimizer_planner_opch_pressure_inputs_gate;OPCH-060;OPCH-061;OPCH-062;optimizer;mga;memory;agents;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_planner_opch_070_071_gate
  optimizer_planner_opch_070_071_gate.cpp
)
target_compile_features(optimizer_planner_opch_070_071_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_planner_opch_070_071_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_planner_opch_070_071_gate
  COMMAND optimizer_planner_opch_070_071_gate
)
set_tests_properties(optimizer_planner_opch_070_071_gate PROPERTIES
  LABELS "optimizer_planner_opch_070_071_gate;OPCH-070;OPCH-071;optimizer;planner;ordering;window;aggregate;mga;memory;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_planner_opch_specialized_fusion_gate
  optimizer_planner_opch_specialized_fusion_gate.cpp
)
target_compile_features(optimizer_planner_opch_specialized_fusion_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_planner_opch_specialized_fusion_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_planner_opch_specialized_fusion_gate
  COMMAND optimizer_planner_opch_specialized_fusion_gate
)
set_tests_properties(optimizer_planner_opch_specialized_fusion_gate PROPERTIES
  LABELS "optimizer_planner_opch_specialized_fusion_gate;OPCH-080;OPCH-081;optimizer;nosql;specialized;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_planner_opch_route_benchmark_explain_gate
  optimizer_planner_opch_route_benchmark_explain_gate.cpp
)
target_compile_features(optimizer_planner_opch_route_benchmark_explain_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_planner_opch_route_benchmark_explain_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_planner_opch_route_benchmark_explain_gate
  COMMAND optimizer_planner_opch_route_benchmark_explain_gate
)
set_tests_properties(optimizer_planner_opch_route_benchmark_explain_gate PROPERTIES
  LABELS "optimizer_planner_opch_route_benchmark_explain_gate;OPCH-090;OPCH-091;optimizer;route;benchmark;explain;driver;benchmark_clean;mga_transaction_regression"
)

add_executable(
  optimizer_planner_opch_validation_production_gate
  optimizer_planner_opch_validation_production_gate.cpp
)
target_compile_features(optimizer_planner_opch_validation_production_gate PRIVATE cxx_std_20)
target_link_libraries(
  optimizer_planner_opch_validation_production_gate
  PRIVATE
    sb_engine_optimizer
)
add_test(
  NAME optimizer_planner_opch_validation_production_gate
  COMMAND optimizer_planner_opch_validation_production_gate
)
set_tests_properties(optimizer_planner_opch_validation_production_gate PROPERTIES
  LABELS "optimizer_planner_opch_validation_production_gate;OPCH-092;OPCH-093;optimizer;physical_plan;production_gate;benchmark_clean;mga_transaction_regression"
)
