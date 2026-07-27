// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <exception>
#include <functional>
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

}  // namespace

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
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic,
                          const bool replan = false) {
    result = {};
    result.diagnostic = std::move(diagnostic);
    result.replan_required = replan;
    result.execution_started = execution_started;
    result.data_access_observed = execution_started;
    return result;
  };

  const auto dag_validation =
      ValidateTypedPhysicalNodeDag(request.physical_dag, request.limits);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(Refusal(issue.diagnostic_id, issue.field_id));
  }
  if (request.inventory_local_transaction_id == 0 ||
      request.inventory_local_transaction_id !=
          request.physical_dag.local_transaction_id) {
    return refuse(Refusal("SB_DIAG_MGA_READ_TRANSACTION_MISSING",
                          "physical dispatch transaction is not bound"));
  }
  if (request.inventory_statement_snapshot_id == 0 ||
      request.inventory_statement_snapshot_id !=
          request.physical_dag.statement_snapshot_id) {
    return refuse(Refusal("SB_DIAG_MGA_READ_SNAPSHOT_MISSING",
                          "physical dispatch statement snapshot is not bound"));
  }

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
  result.executed_steps.reserve(dispatch_order.size());
  for (const auto node_id : dispatch_order) {
    const auto& node = *nodes_by_id.at(node_id);
    std::vector<CanonicalPhysicalDispatchInput> inputs;
    inputs.reserve(node.input_physical_node_ids.size());
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
                        input_result.materialized_output_batch});
    }

    CanonicalPhysicalDispatchStepResult step;
    execution_started = true;
    try {
      step = executors_by_implementation.at(node.implementation_id)
                 ->execute(request.physical_dag, node, inputs);
    } catch (const std::exception& exception) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-EXECUTOR-FAILURE-V1",
          std::string("physical executor threw: ") + exception.what()));
    } catch (...) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-EXECUTOR-FAILURE-V1",
          "physical executor threw a non-standard exception"));
    }

    if (!step.diagnostic.ok) {
      return refuse(std::move(step.diagnostic));
    }
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
        HasForbiddenAuthority(step.authority)) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-EXECUTION-EVIDENCE-V1",
          "physical executor result does not match admitted node evidence"));
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
  const auto& root = result.executed_steps[root_result->second];
  result.diagnostic = {};
  result.root_result_handle_id = root.result_handle_id;
  result.root_output_descriptor_ids = root.output_descriptor_ids;
  result.authority.engine_mga_snapshot_bound = true;
  result.execution_started = true;
  result.data_access_observed = true;
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_root_physical_node_id = root.executed_physical_node_id;
  result.root_causal_counter_id = root.causal_counter_id;
  return result;
}

}  // namespace scratchbird::engine::executor
