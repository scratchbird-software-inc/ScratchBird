// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_execute.hpp"
#include "canonical_query_aggregate_composition.hpp"
#include "canonical_query_current_heap_composition.hpp"
#include "canonical_query_current_heap_join_composition.hpp"
#include "canonical_query_document_composition.hpp"
#include "canonical_query_filter_project_composition.hpp"
#include "canonical_query_graph_composition.hpp"
#include "canonical_query_join_composition.hpp"
#include "canonical_query_join_pipeline_composition.hpp"
#include "canonical_query_key_value_composition.hpp"
#include "canonical_query_literal_values_composition.hpp"
#include "canonical_query_multileg_composition.hpp"
#include "canonical_query_node_composition.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_order_limit_composition.hpp"
#include "canonical_query_physical_registration.hpp"
#include "canonical_query_pivot_composition.hpp"
#include "canonical_query_runtime_observation_support.hpp"
#include "canonical_query_search_composition.hpp"
#include "canonical_query_set_composition.hpp"
#include "canonical_query_spatial_columnar_composition.hpp"
#include "canonical_query_table_function_composition.hpp"
#include "canonical_query_time_series_composition.hpp"
#include "canonical_query_vector_composition.hpp"

#if !defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
#include "sblr_dispatch.hpp"
#include "transaction/transaction_api.hpp"
#endif

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <ranges>
#include <string>
#include <utility>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace {

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_EXECUTE_COORDINATOR_AUTHORITY
// Coordinates admitted canonical query planning and execution. It consumes
// engine-owned MGA snapshots and typed results but does not own transaction
// visibility, finality, parser lowering, or durable storage publication.

#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
thread_local bool g_contract_security_boundary_drift_armed = false;
thread_local bool g_contract_resource_boundary_drift_armed = false;
#endif

api::CanonicalOptimizerSelectedExecutionResult ExecuteSelectedWithMgaGuard(
    const api::EngineRequestContext& context,
    const api::CanonicalOptimizerSelectedExecutionRequest& request,
    const OrdinaryRuntimeMemoryReceipts& memory_receipts = {}) {
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
  auto bounded_request = request;
  // Deterministic closure seams model a selected plan becoming stale between
  // publication and execution. Production has no mutable selected-plan seam.
  if (g_contract_security_boundary_drift_armed) {
    ++bounded_request.selected_physical_dag.security_epoch;
    g_contract_security_boundary_drift_armed = false;
  }
  if (g_contract_resource_boundary_drift_armed) {
    ++bounded_request.selected_physical_dag.resource_epoch;
    g_contract_resource_boundary_drift_armed = false;
  }
#else
  api::CanonicalOptimizerSelectedExecutionRequest bounded_request;
  bounded_request.abi_version = request.abi_version;
  bounded_request.pre_access_statistics_snapshot_uuid =
      request.pre_access_statistics_snapshot_uuid;
  bounded_request.limits = request.limits;
  bounded_request.runtime_limits = request.runtime_limits;
  bounded_request.executor_registration_live_memory_bytes =
      request.executor_registration_live_memory_bytes;
  bounded_request.engine_execution_authorized =
      request.engine_execution_authorized;
  bounded_request.parser_execution_authority_claimed =
      request.parser_execution_authority_claimed;
  bounded_request.transaction_finality_claimed =
      request.transaction_finality_claimed;
  bounded_request.recovery_authority_claimed =
      request.recovery_authority_claimed;
  bounded_request.borrowed_selected_physical_dag =
      request.borrowed_selected_physical_dag == nullptr
          ? &request.selected_physical_dag
          : request.borrowed_selected_physical_dag;
  bounded_request.borrowed_mga_authority =
      request.borrowed_mga_authority == nullptr
          ? &request.mga_authority
          : request.borrowed_mga_authority;
  bounded_request.borrowed_result_publication_request =
      request.borrowed_result_publication_request == nullptr
          ? &request.result_publication_request
          : request.borrowed_result_publication_request;
  const auto& source_executors =
      request.borrowed_available_executors == nullptr
          ? request.available_executors
          : *request.borrowed_available_executors;
  if (!HasOrdinaryRuntimeObservationWrapperTarget(source_executors,
                                                  memory_receipts)) {
    bounded_request.borrowed_available_executors =
        &source_executors;
  } else {
    bounded_request.available_executors = source_executors;
  }
#endif

  const auto refuse_boundary = [](std::string field_id) {
    api::CanonicalOptimizerSelectedExecutionResult result;
    result.issues.push_back(
        {"QOW-DIAG-QRY-004-SELECT-EXECUTION-BOUNDARY-V1", 0,
         std::move(field_id)});
    return result;
  };
  const auto& dag = bounded_request.borrowed_selected_physical_dag == nullptr
                        ? bounded_request.selected_physical_dag
                        : *bounded_request.borrowed_selected_physical_dag;
  const auto& authorization = context.authorization_context;
  if (!context.security_context_present || !authorization.present ||
      authorization.authority_uuid.canonical.empty() ||
      dag.security_context_uuid != authorization.authority_uuid.canonical ||
      context.principal_uuid.canonical !=
          authorization.principal_uuid.canonical ||
      context.security_epoch == 0 ||
      dag.security_epoch != context.security_epoch ||
      authorization.security_epoch != context.security_epoch ||
      authorization.policy_epoch == 0 ||
      dag.policy_epoch != authorization.policy_epoch ||
      context.catalog_generation_id == 0 ||
      dag.catalog_generation != context.catalog_generation_id ||
      authorization.catalog_generation_id != context.catalog_generation_id) {
    return refuse_boundary("security_authorization_binding");
  }
  if (context.resource_epoch == 0 ||
      dag.resource_epoch != context.resource_epoch ||
      context.optimizer_resource_snapshot_uuid.canonical.empty() ||
      dag.resource_snapshot_uuid !=
          context.optimizer_resource_snapshot_uuid.canonical ||
      context.optimizer_memory_budget_bytes == 0 ||
      dag.memory_budget_bytes != context.optimizer_memory_budget_bytes) {
    return refuse_boundary("resource_budget_binding");
  }

  std::size_t maximum_output_width = 0;
  for (const auto& node : dag.nodes) {
    maximum_output_width =
        std::max(maximum_output_width, node.output_descriptor_ids.size());
  }
  const auto bounded_budget = static_cast<std::size_t>(
      std::min<std::uint64_t>(context.optimizer_memory_budget_bytes,
                              std::numeric_limits<std::size_t>::max()));
  if (maximum_output_width == 0 || bounded_budget < maximum_output_width) {
    return refuse_boundary("runtime_materialization_budget");
  }
  auto& runtime_limits = bounded_request.runtime_limits;
  runtime_limits.maximum_columns_per_batch =
      std::min(runtime_limits.maximum_columns_per_batch, maximum_output_width);
  runtime_limits.maximum_cells_per_batch =
      std::min(runtime_limits.maximum_cells_per_batch, bounded_budget);
  runtime_limits.maximum_rows_per_batch = std::min(
      runtime_limits.maximum_rows_per_batch,
      runtime_limits.maximum_cells_per_batch / maximum_output_width);
  runtime_limits.maximum_total_materialized_cells = std::min(
      runtime_limits.maximum_total_materialized_cells, bounded_budget);
  runtime_limits.maximum_total_materialized_rows = std::min(
      runtime_limits.maximum_total_materialized_rows, bounded_budget);
  if (runtime_limits.maximum_columns_per_batch == 0 ||
      runtime_limits.maximum_cells_per_batch == 0 ||
      runtime_limits.maximum_rows_per_batch == 0 ||
      runtime_limits.maximum_total_materialized_cells == 0 ||
      runtime_limits.maximum_total_materialized_rows == 0) {
    return refuse_boundary("runtime_materialization_budget");
  }
  bounded_request.cancellation_requested =
      context.query_cancellation_requested
          ? context.query_cancellation_requested
          : std::function<bool()>([] { return false; });
  if (!memory_receipts.empty()) {
    PublishOrdinaryRuntimeObservations(&bounded_request.available_executors,
                                       memory_receipts);
  }
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
  return api::ExecuteCanonicalOptimizerSelectedDag(bounded_request);
#else
  const auto inventory_guard =
      api::AcquireTransactionInventoryGuard(context.database_path);
  return api::ExecuteCanonicalOptimizerSelectedDag(bounded_request);
#endif
}

}  // namespace

api::CanonicalOptimizerSelectedExecutionResult
ExecuteSelectedCanonicalObjectFreeDag(
    const api::EngineRequestContext& context,
    const api::CanonicalOptimizerSelectedExecutionRequest& request,
    const OrdinaryRuntimeMemoryReceipts& memory_receipts) {
  return ExecuteSelectedWithMgaGuard(context, request, memory_receipts);
}

#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
void ArmCanonicalQueryPreResultRevocationForContractTest() {
  ArmCanonicalPhysicalRegistrationPreResultRevocationForContractTest();
}

void ArmCanonicalQuerySecurityBoundaryDriftForContractTest() {
  g_contract_security_boundary_drift_armed = true;
}

void ArmCanonicalQueryResourceBoundaryDriftForContractTest() {
  g_contract_resource_boundary_drift_armed = true;
}

std::size_t CanonicalQueryContractRevalidationCountForTest() {
  return CanonicalPhysicalRegistrationRevalidationCountForTest();
}
#endif

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeValuesQuery(
    const CanonicalObjectFreeValuesExecutionRequest& input_request) {
  auto request = input_request;
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
  BindCanonicalPersistedRowDescriptorAuthorityForComposition(
      request.context, &request.expression_services);
  // The textual closure seam does not own a durable transaction inventory.
  // Complete its already-bound statement vector here so the same ABI-v2
  // publication and executor revalidation used by production is exercised.
  auto closure_mga = request.optimizer_request.logical_graph
                         .mga_statement_context;
  closure_mga.oldest_active_transaction_id =
      closure_mga.owning_local_transaction_id;
  closure_mga.oldest_interesting_transaction_id =
      closure_mga.owning_local_transaction_id;
  closure_mga.oldest_snapshot_transaction_id =
      closure_mga.owning_local_transaction_id;
  closure_mga.retention_horizon_transaction_id =
      closure_mga.owning_local_transaction_id;
  closure_mga.active_excluded_local_transaction_ids = {
      closure_mga.owning_local_transaction_id};
  closure_mga.in_doubt_excluded_local_transaction_ids.clear();
  closure_mga.snapshot_kind = "statement_stable";
  closure_mga.publication_inventory_next_local_transaction_id =
      std::max(closure_mga.owning_local_transaction_id,
               closure_mga.visible_committed_high_watermark) +
      1;
  closure_mga.inventory_authoritative = true;
  closure_mga.complete = true;
  closure_mga.current = true;
  request.optimizer_request.logical_graph.mga_statement_context = closure_mga;
  request.optimizer_request.logical_properties.mga_statement_context =
      closure_mga;
  request.optimizer_request.mga.statement_context = closure_mga;
  request.optimizer_admission.mga_statement_context = closure_mga;
#endif
  auto pivot = ExecuteCanonicalObjectFreePivotQuery(request);
  if (pivot.profile_matched) return pivot;
  auto unpivot = ExecuteCanonicalObjectFreeUnpivotQuery(request);
  if (unpivot.profile_matched) return unpivot;
  auto grouped_aggregate =
      ExecuteCanonicalObjectFreeGroupedCountSumQuery(request);
  if (grouped_aggregate.profile_matched) return grouped_aggregate;
  auto aggregate = ExecuteCanonicalObjectFreeGlobalAggregateQuery(request);
  if (aggregate.profile_matched) return aggregate;
  auto composition =
      ExecuteCanonicalObjectFreeDistinctSortLimitQuery(request);
  if (composition.profile_matched) return composition;
  auto full_filtered_tail =
      ExecuteCanonicalObjectFreeFilterProjectDistinctSortLimitQuery(request);
  if (full_filtered_tail.profile_matched) return full_filtered_tail;
  auto filter_project_sort_limit =
      ExecuteCanonicalObjectFreeFilterProjectSortLimitQuery(request);
  if (filter_project_sort_limit.profile_matched) {
    return filter_project_sort_limit;
  }
  auto filter_project_sort =
      ExecuteCanonicalObjectFreeFilterProjectSortQuery(request);
  if (filter_project_sort.profile_matched) return filter_project_sort;
  auto filter_project = ExecuteCanonicalObjectFreeFilterProjectQuery(request);
  if (filter_project.profile_matched) return filter_project;
  auto project_sort = ExecuteCanonicalObjectFreeProjectSortQuery(request);
  if (project_sort.profile_matched) return project_sort;
  auto sort = ExecuteCanonicalObjectFreeSortQuery(request);
  if (sort.profile_matched) return sort;
  auto limit = ExecuteCanonicalObjectFreeLimitQuery(request);
  if (limit.profile_matched) return limit;
  auto project = ExecuteCanonicalObjectFreeProjectQuery(request);
  if (project.profile_matched) return project;
  auto filter = ExecuteCanonicalObjectFreeFilterQuery(request);
  if (filter.profile_matched) return filter;
  auto join_filter_project =
      ExecuteCanonicalObjectFreeInnerJoinFilterProjectQuery(request);
  if (join_filter_project.profile_matched) return join_filter_project;
  auto join = ExecuteCanonicalObjectFreeJoinQuery(request);
  if (join.profile_matched) return join;
  auto nested_set_operation =
      ExecuteCanonicalObjectFreeNestedSetOperationQuery(request);
  if (nested_set_operation.profile_matched) return nested_set_operation;
  auto set_operation = ExecuteCanonicalObjectFreeSetOperationQuery(request);
  if (set_operation.profile_matched) return set_operation;
  auto node_composition =
      ExecuteCanonicalObjectFreeNodeDrivenCompositionQuery(request);
  if (node_composition.profile_matched) return node_composition;
  return ExecuteCanonicalObjectFreeLiteralValuesQuery(request);
}

// QOW-SOURCE-PACKET7-OBJECT-BACKED-HEAP-ROUTE-V1
CanonicalObjectFreeValuesExecutionResult ExecuteCanonicalCurrentHeapQuery(
    const CanonicalCurrentHeapExecutionRequest& input) {
  CanonicalObjectFreeValuesExecutionResult result;
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
  (void)input;
  return result;
#else
  const auto& dag = input.relational_dag;
  // Contextual TEXT authority is intentionally admitted only by the exact
  // synchronous RCP079 COLUMNAR_FILTER route.  Route it there before any
  // other current-heap/model-family selector can perform work or publish a
  // result.  Non-candidates remain unmatched so dispatch can publish the
  // canonical ROUTE_MISMATCH without executing or consuming authority.
  if (input.contextual_text_activation) {
    if (!IsCanonicalSpatialColumnarContextualRouteCandidate(dag)) {
      return result;
    }
    return ExecuteCanonicalSpatialColumnarFamilyQuery(input);
  }
  if (std::ranges::any_of(dag.nodes, [](const auto& node) {
        return node.node_kind ==
               api::RelationalDagNodeKind::kMatchRecognize;
      })) {
    return ExecuteCanonicalGenerateSeriesMatchRecognizeQuery(input);
  }
  if (dag.nodes.size() == 1 &&
      dag.nodes.front().node_kind ==
          api::RelationalDagNodeKind::kTableFunctionInvoke) {
    return ExecuteCanonicalGenerateSeriesTableFunctionQuery(input);
  }
  if (std::ranges::any_of(dag.nodes, [](const auto& node) {
        return node.semantic_variant_id == "SBLR_MODEL_SOURCE_V1" ||
               node.semantic_variant_id == "SBLR_MODEL_EXPAND_V1" ||
               node.semantic_variant_id == "SBLR_MODEL_AGGREGATE_V1";
      })) {
    const auto composition_source_count =
        std::ranges::count_if(dag.nodes, [](const auto& node) {
          return (node.node_kind == api::RelationalDagNodeKind::kScan ||
                  node.node_kind == api::RelationalDagNodeKind::kAggregate) &&
                 (node.semantic_variant_id == "SBLR_MODEL_SOURCE_V1" ||
                  node.semantic_variant_id == "SBLR_MODEL_AGGREGATE_V1" ||
                  node.semantic_variant_id == "relation.source.v1");
        });
    const auto model_composition_source_count =
        std::ranges::count_if(dag.nodes, [](const auto& node) {
          return node.semantic_variant_id == "SBLR_MODEL_SOURCE_V1" ||
                 node.semantic_variant_id == "SBLR_MODEL_AGGREGATE_V1";
        });
    const bool time_series_relational_pair =
        std::ranges::any_of(dag.expressions, [](const auto& expression) {
          return expression.operator_name == "TIME_RANGE";
        }) &&
        std::ranges::any_of(dag.nodes, [](const auto& node) {
          return node.semantic_variant_id == "relation.source.v1";
        });
    const bool lateral_or_apply_pair =
        std::ranges::any_of(dag.nodes, [](const auto& node) {
          return node.node_kind == api::RelationalDagNodeKind::kJoin &&
                 MatchLiveLateralSubqueryProfileForComposition(
                     node.semantic_variant_id).matched;
        });
    if (composition_source_count >= 3 && composition_source_count <= 9 &&
        model_composition_source_count >= 2) {
      const auto composition =
          ExecuteCanonicalBoundedModelFamilyCompositionQuery(input);
      if (composition.profile_matched) return composition;
    }
    if (composition_source_count == 2 &&
        model_composition_source_count >= 1 &&
        (!time_series_relational_pair || lateral_or_apply_pair)) {
      const auto captured = ExecuteCanonicalCapturedModelFamilyJoinQuery(input);
      if (captured.profile_matched) return captured;
      const auto composition = ExecuteCanonicalColumnarFamilyJoinQuery(input);
      if (composition.profile_matched) return composition;
    }
    if (std::ranges::any_of(dag.expressions, [](const auto& expression) {
          return expression.operator_name == "SPATIAL_SOURCE" ||
                 expression.operator_name == "COLUMNAR_SOURCE";
        })) {
      return ExecuteCanonicalSpatialColumnarFamilyQuery(input);
    }
    if (std::ranges::any_of(dag.expressions, [](const auto& expression) {
          return expression.operator_name == "SEARCH_MATCH" ||
                 expression.operator_name == "SEARCH_TERMS" ||
                 expression.operator_name == "SEARCH_PHRASE" ||
                 expression.operator_name == "SEARCH_FUZZY" ||
                 expression.operator_name == "SEARCH_FILTER" ||
                 expression.operator_name == "SEARCH_ANALYZER_BINDING";
        })) {
      return ExecuteCanonicalSearchFamilyQuery(input);
    }
    if (std::ranges::any_of(dag.expressions, [](const auto& expression) {
          return expression.operator_name == "VECTOR_NEAREST" ||
                 expression.operator_name == "VECTOR_FILTER";
        })) {
      return ExecuteCanonicalVectorFamilyQuery(input);
    }
    if (std::ranges::any_of(dag.expressions, [](const auto& expression) {
          return expression.operator_name == "TIME_RANGE" ||
                 expression.operator_name == "TIME_BUCKET" ||
                 expression.operator_name == "TIME_DOWNSAMPLE";
        })) {
      return ExecuteCanonicalTimeSeriesFamilyQuery(input);
    }
    if (std::ranges::any_of(dag.expressions, [](const auto& expression) {
          return expression.operator_name == "KV_KEY" ||
                 expression.operator_name == "KV_MULTI_GET" ||
                 expression.operator_name == "KV_PREFIX";
        })) {
      return ExecuteCanonicalKeyValueFamilyQuery(input);
    }
    if (std::ranges::any_of(dag.expressions, [](const auto& expression) {
          return expression.operator_name == "GRAPH_MATCH" ||
                 expression.operator_name == "GRAPH_EXPAND";
        })) {
      return ExecuteCanonicalGraphFamilyQuery(input);
    }
    return ExecuteCanonicalDocumentFamilyQuery(input);
  }
  if (std::ranges::count_if(dag.nodes, [](const auto& node) {
        return node.node_kind == api::RelationalDagNodeKind::kScan;
      }) >= 2) {
    return ExecuteCanonicalCurrentHeapJoin(input);
  }
  return ExecuteCanonicalCurrentHeapSingleSourceQuery(input);
#endif
}

#if !defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
std::uint32_t CanonicalContextualTextRcp079RuntimeProofMaskForTest() {
  std::uint32_t mask =
      CanonicalSpatialColumnarContextualInternalProofMaskForTest();
  auto activation = std::make_shared<ContextualTextDispatchActivationV2>();
  CanonicalCurrentHeapExecutionRequest unsupported;
  unsupported.contextual_text_activation = activation;
  const auto routed = ExecuteCanonicalCurrentHeapQuery(unsupported);
  if (!routed.profile_matched && !activation->joint_consumed &&
      !activation->lease.valid()) {
    mask |= 1U << 0;
  }
  return mask;
}
#endif

}  // namespace scratchbird::engine::sblr
