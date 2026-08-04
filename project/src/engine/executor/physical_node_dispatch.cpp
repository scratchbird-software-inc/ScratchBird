// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <algorithm>
#include <exception>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::executor {
namespace {

DescriptorRuntimeDiagnostic Refusal(std::string code,
                                    std::string detail = {}) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code = std::move(code);
  diagnostic.detail = std::move(detail);
  return diagnostic;
}

bool HasForbiddenAuthority(
    const CanonicalPhysicalDispatchAuthorityEvidence& authority) {
  return authority.owns_transaction_finality || authority.owns_recovery ||
         authority.owns_parser_execution ||
         authority.owns_visibility_outside_engine_mga ||
         authority.wal_is_transaction_or_recovery_authority;
}

bool CheckedAdd(const std::size_t left,
                const std::size_t right,
                std::size_t* output) {
  if (output == nullptr ||
      right > std::numeric_limits<std::size_t>::max() - left) {
    return false;
  }
  *output = left + right;
  return true;
}

DescriptorRuntimeDiagnostic ValidateRuntimeLimits(
    const CanonicalPhysicalDagRuntimeLimits& limits) {
  if (limits.maximum_rows_per_batch == 0 ||
      limits.maximum_columns_per_batch == 0 ||
      limits.maximum_cells_per_batch == 0 ||
      limits.maximum_total_materialized_rows == 0 ||
      limits.maximum_total_materialized_cells == 0) {
    return Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "physical typed-batch runtime limits must be nonzero");
  }
  return {};
}

DescriptorRuntimeDiagnostic ValidateMaterializedBatch(
    const DescriptorBatch& batch,
    const std::vector<std::uint32_t>& descriptor_ids,
    const CanonicalPhysicalDagRuntimeLimits& limits,
    const std::size_t total_rows,
    const std::size_t total_cells,
    std::size_t* next_total_rows,
    std::size_t* next_total_cells) {
  const auto row_count = batch.rows.size();
  const auto column_count = batch.columns.size();
  if (row_count > limits.maximum_rows_per_batch ||
      column_count > limits.maximum_columns_per_batch ||
      (column_count != 0 &&
       row_count > limits.maximum_cells_per_batch / column_count)) {
    return Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "physical typed batch exceeds its row, column, or cell "
                   "bound");
  }
  const auto batch_cells = row_count * column_count;
  if (!CheckedAdd(total_rows, row_count, next_total_rows) ||
      !CheckedAdd(total_cells, batch_cells, next_total_cells) ||
      *next_total_rows > limits.maximum_total_materialized_rows ||
      *next_total_cells > limits.maximum_total_materialized_cells) {
    return Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "physical typed DAG exceeds its cumulative batch bound");
  }
  return ValidateCanonicalDescriptorBatch(batch, descriptor_ids);
}

}  // namespace

DescriptorRuntimeDiagnostic RevalidateCanonicalExecutionMgaAuthority(
    const CanonicalExecutionMgaAuthority& authority,
    const TypedPhysicalNodeDag& physical_dag,
    const PhysicalNodeAbiLimits& limits) {
  const auto dag_validation = ValidateTypedPhysicalNodeDag(physical_dag, limits);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return Refusal(issue.diagnostic_id, issue.field_id);
  }
  if (physical_dag.abi_version != 2) {
    // ABI v1 predates the full immutable statement-context carrier, but its
    // scalar snapshot admission contract still fails closed on zero.  Keep
    // that legacy boundary intact while ABI v2 uses only the full carrier
    // and accepts a valid zero committed high-watermark.
    if (physical_dag.statement_snapshot_id == 0) {
      return Refusal("QOW-DIAG-PHYSICAL-NODE-ABI-ADMISSION",
                     "statement_snapshot_id");
    }
    return {};
  }
  if (authority.origin == CanonicalMgaAuthorityOrigin::kMissing ||
      !authority.resolve_current ||
      !PhysicalMgaStatementContextValid(authority.statement_context) ||
      !PhysicalMgaStatementContextEqual(authority.statement_context,
                                        physical_dag.mga_statement_context)) {
    return Refusal(
        "QOW-DIAG-MGA-RUNTIME-AUTHORITY-V1",
        "missing, malformed, or DAG-swapped MGA runtime authority");
  }
  auto current = authority.resolve_current();
  if (!current.diagnostic.ok) {
    return current.diagnostic;
  }
  if (!PhysicalMgaStatementContextValid(current.statement_context) ||
      !PhysicalMgaStatementContextEqual(current.statement_context,
                                        authority.statement_context)) {
    return Refusal(
        "QOW-DIAG-MGA-RUNTIME-CURRENT-V1",
        "resolved current MGA statement context differs from the selected "
        "execution authority");
  }
  return {};
}

bool CanonicalMgaCreatorVisibleToStatement(
    const PhysicalMgaStatementContext& statement_context,
    const std::uint64_t creator_local_transaction_id) {
  if (!PhysicalMgaStatementContextValid(statement_context) ||
      creator_local_transaction_id == 0) {
    return false;
  }
  if (creator_local_transaction_id ==
      statement_context.owning_local_transaction_id) {
    return true;
  }
  if (creator_local_transaction_id >
      statement_context.visible_committed_high_watermark) {
    return false;
  }
  return !std::binary_search(
             statement_context.active_excluded_local_transaction_ids.begin(),
             statement_context.active_excluded_local_transaction_ids.end(),
             creator_local_transaction_id) &&
         !std::binary_search(
             statement_context.in_doubt_excluded_local_transaction_ids.begin(),
             statement_context.in_doubt_excluded_local_transaction_ids.end(),
             creator_local_transaction_id);
}

// QOW-SOURCE-QRY-004-CONSUMPTION-V1
// Validates and consumes one complete selected physical DAG.  Every exact
// implementation is resolved before execution, inputs run before consumers,
// shared nodes execute once, and every returned handle must retain the
// admitted plan/node/descriptor/causal identities.  The dispatcher coordinates
// engine executors but never acquires parser, MGA-finality, recovery, or WAL
// authority.
CanonicalPhysicalDagDispatchResult ExecuteCanonicalPhysicalDag(
    const CanonicalPhysicalDagDispatchRequest& request) {
  CanonicalPhysicalDagDispatchResult result;
  bool execution_started = false;
  bool data_access_observed = false;
  bool cancellation_observed = false;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic,
                          const bool replan = false) {
    result = {};
    result.diagnostic = std::move(diagnostic);
    result.replan_required = replan;
    result.execution_started = execution_started;
    result.data_access_observed = data_access_observed;
    result.cancellation_observed = cancellation_observed;
    return result;
  };

  const auto cancellation_diagnostic = [&](const char* phase) {
    DescriptorRuntimeDiagnostic diagnostic;
    if (!request.cancellation_requested) return diagnostic;
    try {
      if (!request.cancellation_requested()) return diagnostic;
      cancellation_observed = true;
      return Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-DISPATCH-CANCELLED-V1",
          std::string("physical DAG cancellation observed ") + phase);
    } catch (const std::exception& exception) {
      return Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1",
          std::string("physical cancellation probe threw: ") +
              exception.what());
    } catch (...) {
      return Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1",
          "physical cancellation probe threw a non-standard exception");
    }
  };

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag, request.limits);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }
  const auto runtime_limits_validation =
      ValidateRuntimeLimits(request.runtime_limits);
  if (!runtime_limits_validation.ok) {
    return refuse(runtime_limits_validation);
  }
  const auto entry_cancellation =
      cancellation_diagnostic("before selected-node dispatch");
  if (!entry_cancellation.ok) return refuse(entry_cancellation);

  std::unordered_map<std::string,
                     const CanonicalPhysicalExecutorRegistration*>
      executors_by_implementation;
  for (const auto& registration : request.available_executors) {
    if (registration.implementation_id.empty() || !registration.execute ||
        !executors_by_implementation
             .emplace(registration.implementation_id, &registration)
             .second) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-EXECUTOR-REGISTRY-V1",
          "physical executor registration is missing or duplicated"));
    }
  }

  std::unordered_map<std::uint64_t, const PhysicalNodeRecord*> nodes_by_id;
  for (const auto& node : request.physical_dag.nodes) {
    nodes_by_id.emplace(node.physical_node_id, &node);
    const auto registration =
        executors_by_implementation.find(node.implementation_id);
    if (registration == executors_by_implementation.end() ||
        registration->second->node_kind != node.node_kind ||
        (request.physical_dag.abi_version == 2 &&
         (registration->second->executor_capability_uuid !=
              node.executor_capability_uuid ||
          registration->second->executor_capability_abi_version !=
              node.executor_capability_abi_version ||
          !registration->second->engine_owned ||
          !registration->second->accepts_optimizer_publication_v2))) {
      return refuse(
          Refusal("QOW-DIAG-QRY-004-PHYSICAL-IMPLEMENTATION-UNAVAILABLE-V1",
                  "selected physical implementation is unavailable: " +
                      node.implementation_id),
          true);
    }
  }

  std::vector<std::uint64_t> dispatch_order;
  dispatch_order.reserve(request.physical_dag.nodes.size());
  std::unordered_set<std::uint64_t> scheduled;
  std::function<void(std::uint64_t)> schedule =
      [&](const std::uint64_t node_id) {
        if (!scheduled.insert(node_id).second) return;
        const auto* node = nodes_by_id.at(node_id);
        for (const auto input_id : node->input_physical_node_ids) {
          schedule(input_id);
        }
        dispatch_order.push_back(node_id);
      };
  schedule(request.physical_dag.root_physical_node_id);
  if (dispatch_order.size() != request.physical_dag.nodes.size()) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "physical dispatch order omitted a node"));
  }

  std::unordered_map<std::uint64_t, std::size_t> result_index_by_node;
  std::size_t total_materialized_rows = 0;
  std::size_t total_materialized_cells = 0;
  result.executed_steps.reserve(dispatch_order.size());
  for (const auto node_id : dispatch_order) {
    const auto node_cancellation =
        cancellation_diagnostic("before a selected node");
    if (!node_cancellation.ok) return refuse(node_cancellation);
    const auto& node = *nodes_by_id.at(node_id);
    std::vector<CanonicalPhysicalDispatchInput> inputs;
    inputs.reserve(node.input_physical_node_ids.size());
    std::size_t materialized_input_rows = 0;
    for (const auto input_id : node.input_physical_node_ids) {
      const auto prior = result_index_by_node.find(input_id);
      if (prior == result_index_by_node.end()) {
        return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                              "physical input result is unavailable"));
      }
      const auto& input_result = result.executed_steps[prior->second];
      inputs.push_back({input_result.executed_physical_node_id,
                        input_result.causal_counter_id,
                        input_result.result_handle_id,
                        input_result.output_descriptor_ids,
                        input_result.materialized_output_batch,
                        input_result.mga_statement_context});
      if (!input_result.materialized_output_batch.has_value()) {
        return refuse(Refusal(
            "QOW-DIAG-QRY-004-PHYSICAL-TYPED-BATCH-REQUIRED-V1",
            "selected physical input did not produce a typed batch"));
      }
      if (input_result.materialized_output_batch.has_value() &&
          !CheckedAdd(materialized_input_rows,
                      input_result.materialized_output_batch->rows.size(),
                      &materialized_input_rows)) {
        return refuse(Refusal(
            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
            "physical input row count overflowed"));
      }
      if (request.physical_dag.abi_version == 2 &&
          !PhysicalMgaStatementContextEqual(
              input_result.mga_statement_context,
              request.mga_authority.statement_context)) {
        return refuse(Refusal(
            "QOW-DIAG-MGA-DISPATCH-INPUT-CONTEXT-V1",
            "physical input batch is not bound to the selected MGA context"));
      }
    }

    CanonicalPhysicalDispatchStepResult step;
    const auto pre_callback_authority =
        RevalidateCanonicalExecutionMgaAuthority(
            request.mga_authority, request.physical_dag, request.limits);
    if (!pre_callback_authority.ok) {
      return refuse(pre_callback_authority);
    }
    execution_started = true;
    try {
      step = executors_by_implementation.at(node.implementation_id)
                 ->execute(request.physical_dag, node, inputs);
    } catch (const std::exception& exception) {
      data_access_observed = true;
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-EXECUTOR-FAILURE-V1",
          std::string("physical executor threw: ") + exception.what()));
    } catch (...) {
      data_access_observed = true;
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-EXECUTOR-FAILURE-V1",
          "physical executor threw a non-standard exception"));
    }

    // QOW-SOURCE-QRY-004-DATA-ACCESS-OBSERVATION-V1
    // Preserve an executor's explicit observation even when its diagnostic
    // refuses the step. Unknown legacy callbacks retain the conservative
    // callback-started fallback.
    data_access_observed =
        data_access_observed ||
        (step.data_access_observation_known ? step.data_access_observed
                                            : execution_started);

    const auto post_callback_authority =
        RevalidateCanonicalExecutionMgaAuthority(
            request.mga_authority, request.physical_dag, request.limits);
    if (!post_callback_authority.ok) {
      return refuse(post_callback_authority);
    }

    if (!step.diagnostic.ok) {
      return refuse(std::move(step.diagnostic));
    }
    const auto post_step_cancellation =
        cancellation_diagnostic("after a selected node");
    if (!post_step_cancellation.ok) return refuse(post_step_cancellation);
    step.execution_ordinal = result.executed_steps.size() + 1;
    step.execution_started = true;
    step.execution_finished = true;
    step.counters_captured_after_finish = true;
    if (step.selected_plan_uuid != request.physical_dag.selected_plan_uuid ||
        step.executed_physical_node_id != node.physical_node_id ||
        step.causal_counter_id != node.causal_counter_id ||
        step.result_handle_id == 0 ||
        step.output_descriptor_ids != node.output_descriptor_ids ||
        !step.authority.engine_mga_snapshot_bound ||
        (request.physical_dag.abi_version == 2 &&
         !PhysicalMgaStatementContextEqual(
             step.mga_statement_context,
             request.mga_authority.statement_context)) ||
        HasForbiddenAuthority(step.authority)) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-EXECUTION-EVIDENCE-V1",
          "physical executor result does not match admitted node evidence"));
    }
    if (!step.materialized_output_batch.has_value()) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-TYPED-BATCH-REQUIRED-V1",
          "selected physical node returned only an opaque result handle"));
    }
    if (step.materialized_output_batch.has_value()) {
      std::size_t next_total_rows = 0;
      std::size_t next_total_cells = 0;
      const auto batch_validation = ValidateMaterializedBatch(
          *step.materialized_output_batch, node.output_descriptor_ids,
          request.runtime_limits, total_materialized_rows,
          total_materialized_cells, &next_total_rows, &next_total_cells);
      if (!batch_validation.ok) return refuse(batch_validation);
      if (step.output_row_count !=
          step.materialized_output_batch->rows.size()) {
        return refuse(Refusal(
            "QOW-DIAG-QRY-004-PHYSICAL-CAUSAL-COUNTER-V1",
            "physical output-row counter differs from the typed batch"));
      }
      total_materialized_rows = next_total_rows;
      total_materialized_cells = next_total_cells;
    }
    if (step.input_row_count != materialized_input_rows) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-CAUSAL-COUNTER-V1",
          "physical input-row counter differs from causal input batches"));
    }
    if ((node.node_kind == PhysicalNodeKind::kFilter ||
         node.node_kind == PhysicalNodeKind::kProject) &&
        step.rows_examined != materialized_input_rows) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-CAUSAL-COUNTER-V1",
          "row operator examination counter differs from its typed input"));
    }
    if (node.node_kind == PhysicalNodeKind::kFilter &&
        step.output_row_count > materialized_input_rows) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-CAUSAL-COUNTER-V1",
          "filter output exceeds its causal typed input"));
    }
    if (node.node_kind == PhysicalNodeKind::kProject &&
        step.output_row_count != materialized_input_rows) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-CAUSAL-COUNTER-V1",
          "project output cardinality differs from its causal typed input"));
    }
    result_index_by_node.emplace(node_id, result.executed_steps.size());
    result.executed_steps.push_back(std::move(step));
  }

  const auto root_result =
      result_index_by_node.find(request.physical_dag.root_physical_node_id);
  if (root_result == result_index_by_node.end()) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "physical root result is unavailable"));
  }
  const auto publication_cancellation =
      cancellation_diagnostic("before root publication");
  if (!publication_cancellation.ok) return refuse(publication_cancellation);
  const auto root_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag, request.limits);
  if (!root_authority.ok) {
    return refuse(root_authority);
  }
  const auto& root = result.executed_steps[root_result->second];
  result.diagnostic = {};
  result.root_result_handle_id = root.result_handle_id;
  result.root_output_descriptor_ids = root.output_descriptor_ids;
  result.authority.engine_mga_snapshot_bound = true;
  result.execution_started = true;
  result.data_access_observed = data_access_observed;
  result.cancellation_observed = false;
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_root_physical_node_id = root.executed_physical_node_id;
  result.root_causal_counter_id = root.causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

}  // namespace scratchbird::engine::executor
