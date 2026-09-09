// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_join_composition.hpp"

#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_join_registration.hpp"
#include "canonical_query_object_free_profile.hpp"
#include "canonical_query_physical_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace plan = scratchbird::engine::planner;

namespace {

constexpr std::string_view kValuesImplementationId =
    "values.materialize.canonical.v1";

}  // namespace

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_JOIN_COMPOSITION_AUTHORITY
// Coordinates an already-admitted object-free JOIN over typed VALUES inputs.
// It consumes engine-selected MGA statement context and cannot create a
// snapshot, access storage, or publish transaction finality.

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeJoinQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  std::optional<exec::CanonicalAcceptedJoinKind> join_kind;
  std::string join_component;
  std::string operation_name;
  if (root != graph.nodes.end()) {
    if (root->semantic_variant_id == "join.inner.v1") {
      join_kind = exec::CanonicalAcceptedJoinKind::kInner;
      join_component = "inner";
      operation_name = "INNER JOIN";
    } else if (root->semantic_variant_id == "join.cross.v1") {
      join_kind = exec::CanonicalAcceptedJoinKind::kCross;
      join_component = "cross";
      operation_name = "CROSS JOIN";
    } else if (root->semantic_variant_id == "join.left-outer.v1") {
      join_kind = exec::CanonicalAcceptedJoinKind::kLeftOuter;
      join_component = "left-outer";
      operation_name = "LEFT OUTER JOIN";
    } else if (root->semantic_variant_id == "join.right-outer.v1") {
      join_kind = exec::CanonicalAcceptedJoinKind::kRightOuter;
      join_component = "right-outer";
      operation_name = "RIGHT OUTER JOIN";
    } else if (root->semantic_variant_id == "join.full-outer.v1") {
      join_kind = exec::CanonicalAcceptedJoinKind::kFullOuter;
      join_component = "full-outer";
      operation_name = "FULL OUTER JOIN";
    } else if (root->semantic_variant_id == "join.left-semi.v1") {
      join_kind = exec::CanonicalAcceptedJoinKind::kLeftSemi;
      join_component = "left-semi";
      operation_name = "LEFT SEMI JOIN";
    } else if (root->semantic_variant_id == "join.left-anti.v1") {
      join_kind = exec::CanonicalAcceptedJoinKind::kLeftAnti;
      join_component = "left-anti";
      operation_name = "LEFT ANTI JOIN";
    }
  }
  const auto expected_expression_count =
      join_kind == exec::CanonicalAcceptedJoinKind::kCross ? 0U : 1U;
  if (graph.nodes.size() != 3 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kJoin ||
      !join_kind.has_value() ||
      root->input_logical_node_ids.size() != 2 ||
      root->input_logical_node_ids[0] == root->input_logical_node_ids[1] ||
      root->bound_expression_ids.size() != expected_expression_count ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const auto find_node = [&](const std::uint32_t node_id) {
    return std::ranges::find_if(graph.nodes, [&](const auto& node) {
      return node.logical_node_id == node_id;
    });
  };
  const auto left_node = find_node(root->input_logical_node_ids[0]);
  const auto right_node = find_node(root->input_logical_node_ids[1]);
  if (left_node == graph.nodes.end() || right_node == graph.nodes.end() ||
      left_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      right_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      left_node->semantic_variant_id != "values.literal-table.v1" ||
      right_node->semantic_variant_id != "values.literal-table.v1") {
    return result;
  }
  for (const auto& node : graph.nodes) {
    const bool values =
        node.node_kind == plan::CanonicalLogicalRelationalNodeKind::kValues;
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty() ||
        (values && !node.input_logical_node_ids.empty())) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-JOIN-ADMISSION-V1",
                  "live " + operation_name +
                      " execution lacks optimizer admission");
  }

  auto left = MaterializeValues(request.relational_dag, *left_node,
                                request.expression_services);
  if (!left.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-JOIN-PAYLOAD-V1",
                  "left VALUES: " + left.detail);
  }
  auto right = MaterializeValues(request.relational_dag, *right_node,
                                 request.expression_services);
  if (!right.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-JOIN-PAYLOAD-V1",
                  "right VALUES: " + right.detail);
  }
  auto prepared_root = PrepareJoinRootForComposition(request.relational_dag, *root,
                                       *left_node, *right_node, left, right,
                                       *join_kind);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-JOIN-PAYLOAD-V1",
                  prepared_root.detail);
  }

  const auto left_count = left.batch.rows.size();
  const auto right_count = right.batch.rows.size();
  if (left_count != 0 &&
      right_count > std::numeric_limits<std::size_t>::max() / left_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live " + operation_name +
                      " pair cardinality overflowed");
  }
  const auto pair_count = left_count * right_count;
  if (pair_count >
      request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live " + operation_name +
                      " pair evaluation exceeds the admitted candidate bound");
  }

  std::uint64_t left_memory = 1;
  std::uint64_t right_memory = 1;
  if (!AddBatchMemoryBytes(left.batch, &left_memory) ||
      !AddBatchMemoryBytes(right.batch, &right_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live " + operation_name + " input size overflowed");
  }
  std::uint64_t total_memory = 0;
  std::uint64_t join_retained_state_memory = 0;
  if (!CheckedAdd(left_memory, right_memory, &total_memory) ||
      !exec::BoundCanonicalJoinRetainedStateBytes(
          left_count, right_count, &join_retained_state_memory) ||
      !CheckedAdd(total_memory, join_retained_state_memory, &total_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live " + operation_name +
                      " retained join state size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live " + operation_name +
                      " predicate state exceeds the admitted memory budget");
  }

  std::vector<api::EngineSqlTruthValue> predicate_truth_values;
  predicate_truth_values.reserve(pair_count);
  std::vector<bool> matched_left(left_count, false);
  std::vector<bool> matched_right(right_count, false);
  std::size_t matched_pair_count = 0;
  if (*join_kind == exec::CanonicalAcceptedJoinKind::kCross) {
    predicate_truth_values.assign(
        pair_count, api::EngineSqlTruthValue::true_value);
    std::fill(matched_left.begin(), matched_left.end(), right_count != 0);
    std::fill(matched_right.begin(), matched_right.end(), left_count != 0);
    matched_pair_count = pair_count;
  } else {
    CanonicalRelationalExpressionRuntime expression_runtime(
        request.relational_dag, request.expression_services);
    std::vector<api::EngineTypedValue> predicate_row_values;
    predicate_row_values.reserve(left.batch.columns.size() +
                                 right.batch.columns.size());
    for (std::size_t left_ordinal = 0; left_ordinal < left_count;
         ++left_ordinal) {
      for (std::size_t right_ordinal = 0; right_ordinal < right_count;
           ++right_ordinal) {
        predicate_row_values.clear();
        const auto& left_values = left.batch.rows[left_ordinal].values;
        const auto& right_values = right.batch.rows[right_ordinal].values;
        predicate_row_values.insert(predicate_row_values.end(),
                                    left_values.begin(), left_values.end());
        predicate_row_values.insert(predicate_row_values.end(),
                                    right_values.begin(), right_values.end());
        api::EngineSqlTruthValue predicate_truth =
            api::EngineSqlTruthValue::unknown;
        std::string predicate_detail;
        if (!expression_runtime.EvaluatePredicateForConsumer(
                prepared_root.predicate_expression_id,
                prepared_root.predicate_row_binding, predicate_row_values,
                api::EngineCanonicalExpressionConsumer::join,
                &predicate_truth, &predicate_detail)) {
          return refuse("QOW-DIAG-RELATIONAL-LIVE-JOIN-PAYLOAD-V1",
                        operation_name + " predicate pair " +
                            std::to_string(predicate_truth_values.size()) +
                            ": " + predicate_detail);
        }
        predicate_truth_values.push_back(predicate_truth);
        if (predicate_truth == api::EngineSqlTruthValue::true_value) {
          matched_left[left_ordinal] = true;
          matched_right[right_ordinal] = true;
          ++matched_pair_count;
        }
      }
    }
  }
  const auto matched_left_count = static_cast<std::size_t>(
      std::ranges::count(matched_left, true));
  const auto matched_right_count = static_cast<std::size_t>(
      std::ranges::count(matched_right, true));
  const auto unmatched_left_count = left_count - matched_left_count;
  const auto unmatched_right_count = right_count - matched_right_count;
  std::size_t output_row_bound = matched_pair_count;
  const auto add_output_rows = [&](const std::size_t additional) {
    if (additional > std::numeric_limits<std::size_t>::max() -
                         output_row_bound) {
      return false;
    }
    output_row_bound += additional;
    return true;
  };
  switch (*join_kind) {
    case exec::CanonicalAcceptedJoinKind::kLeftOuter:
      if (!add_output_rows(unmatched_left_count)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "live LEFT OUTER JOIN output cardinality overflowed");
      }
      break;
    case exec::CanonicalAcceptedJoinKind::kRightOuter:
      if (!add_output_rows(unmatched_right_count)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "live RIGHT OUTER JOIN output cardinality overflowed");
      }
      break;
    case exec::CanonicalAcceptedJoinKind::kFullOuter:
      if (!add_output_rows(unmatched_left_count) ||
          !add_output_rows(unmatched_right_count)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "live FULL OUTER JOIN output cardinality overflowed");
      }
      break;
    case exec::CanonicalAcceptedJoinKind::kLeftSemi:
      output_row_bound = matched_left_count;
      break;
    case exec::CanonicalAcceptedJoinKind::kLeftAnti:
      output_row_bound = unmatched_left_count;
      break;
    case exec::CanonicalAcceptedJoinKind::kCross:
    case exec::CanonicalAcceptedJoinKind::kInner:
      break;
  }

  {
    // RuntimeMaterializedBatchMemoryBytes assigns every materialized batch one
    // logical base byte, including an empty result; rows themselves add only
    // their encoded and binary value payloads.
    std::uint64_t output_memory = 1;
    const auto add_tuple_memory = [&](const exec::DescriptorTuple& tuple) {
      for (const auto& value : tuple.values) {
        if (!CheckedAdd(output_memory, value.encoded_value.size(),
                        &output_memory) ||
            !CheckedAdd(output_memory, value.binary_value.size(),
                        &output_memory)) {
          return false;
        }
      }
      return true;
    };
    bool output_memory_valid = true;
    if (*join_kind == exec::CanonicalAcceptedJoinKind::kLeftSemi) {
      for (std::size_t left_ordinal = 0;
           output_memory_valid && left_ordinal < left_count;
           ++left_ordinal) {
        if (matched_left[left_ordinal]) {
          output_memory_valid =
              add_tuple_memory(left.batch.rows[left_ordinal]);
        }
      }
    } else if (*join_kind == exec::CanonicalAcceptedJoinKind::kLeftAnti) {
      for (std::size_t left_ordinal = 0;
           output_memory_valid && left_ordinal < left_count;
           ++left_ordinal) {
        if (!matched_left[left_ordinal]) {
          output_memory_valid =
              add_tuple_memory(left.batch.rows[left_ordinal]);
        }
      }
    } else {
      const bool emits_unmatched_left =
          *join_kind == exec::CanonicalAcceptedJoinKind::kLeftOuter ||
          *join_kind == exec::CanonicalAcceptedJoinKind::kFullOuter;
      const bool emits_unmatched_right =
          *join_kind == exec::CanonicalAcceptedJoinKind::kRightOuter ||
          *join_kind == exec::CanonicalAcceptedJoinKind::kFullOuter;
      for (std::size_t pair = 0;
           output_memory_valid && pair < predicate_truth_values.size();
           ++pair) {
        if (predicate_truth_values[pair] !=
            api::EngineSqlTruthValue::true_value) {
          continue;
        }
        const auto left_ordinal = pair / right_count;
        const auto right_ordinal = pair % right_count;
        output_memory_valid =
            add_tuple_memory(left.batch.rows[left_ordinal]) &&
            add_tuple_memory(right.batch.rows[right_ordinal]);
      }
      for (std::size_t left_ordinal = 0;
           output_memory_valid && emits_unmatched_left &&
           left_ordinal < left_count;
           ++left_ordinal) {
        if (!matched_left[left_ordinal]) {
          output_memory_valid =
              add_tuple_memory(left.batch.rows[left_ordinal]);
        }
      }
      for (std::size_t right_ordinal = 0;
           output_memory_valid && emits_unmatched_right &&
           right_ordinal < right_count;
           ++right_ordinal) {
        if (!matched_right[right_ordinal]) {
          output_memory_valid =
              add_tuple_memory(right.batch.rows[right_ordinal]);
        }
      }
    }
    if (!output_memory_valid ||
        !CheckedAdd(total_memory, output_memory, &total_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "live " + operation_name + " output size overflowed");
    }
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live " + operation_name +
                      " exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto join_capability_uuid =
      DerivedCanonicalUuid(identity_scope,
                           "join." + join_component + ".capability");
  const auto join_implementation_id =
      "join." + join_component + ".3vl.nested.v1";
  std::vector<LivePhysicalNodeProfile> profiles;
  for (const auto& node : graph.nodes) {
    const bool values =
        node.node_kind == plan::CanonicalLogicalRelationalNodeKind::kValues;
    std::uint64_t node_rows = output_row_bound;
    std::uint64_t node_memory = total_memory;
    if (node.logical_node_id == left_node->logical_node_id) {
      node_rows = left_count;
      node_memory = left_memory;
    } else if (node.logical_node_id == right_node->logical_node_id) {
      node_rows = right_count;
      node_memory = right_memory;
    }
    profiles.push_back(
        {node.logical_node_id,
         values ? std::string(kValuesImplementationId)
                : join_implementation_id,
         values ? values_capability_uuid : join_capability_uuid,
         node.node_kind,
         values ? exec::PhysicalNodeKind::kValues
                : exec::PhysicalNodeKind::kJoin,
         values ? "canonical.values.materialize.v1"
                : "canonical." + join_implementation_id,
         node_rows,
         values ? node_memory
                : request.optimizer_request.resource.memory_budget_bytes,
         values ? 0U : 2U,
         values ? 0U : 2U});
  }
  if (!CompleteLiveRuntimeMemoryReceipts(
          &profiles,
          {{root->logical_node_id, join_retained_state_memory}})) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "join runtime memory receipts are incomplete");
  }
  auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "join." + join_component + ".selected-plan",
      operation_name);
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(left_node->logical_node_id, std::move(left.batch));
  values_batches.emplace(right_node->logical_node_id, std::move(right.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-JOIN-VALUES-V1", operation_name);

  auto join_registration = MakeLiveJoinRegistration(
      join_implementation_id, join_capability_uuid,
      std::move(predicate_truth_values), pair_count, output_row_bound,
      *join_kind, operation_name, request.context);

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.selected_physical_dag =
      std::move(planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(std::move(join_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "join.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "join.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, output_row_bound);

  const auto execution =
      ExecuteSelectedCanonicalObjectFreeDag(
          request.context, execution_request,
          planning.ordinary_runtime_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-JOIN-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live INNER JOIN selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

}  // namespace scratchbird::engine::sblr
