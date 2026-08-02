// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <cstdint>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-004-CONSUMPTION-V1: " << detail << '\n';
  }
  return condition;
}

std::string Uuid(const std::uint64_t suffix) {
  auto text = std::string("019f0000-0000-7500-8000-000000000000");
  const auto digits = std::to_string(suffix);
  text.replace(text.size() - digits.size(), digits.size(), digits);
  return text;
}

void BindPublishedNodeContexts(exec::TypedPhysicalNodeDag* dag) {
  for (auto& node : dag->nodes) {
    node.selected_alternative_uuid = Uuid(7000 + node.physical_node_id);
    node.executor_capability_uuid = Uuid(8000 + node.physical_node_id);
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid = Uuid(9000 + node.physical_node_id);
    node.memory_bytes_required = 1024;
    node.engine_capability_validated = true;
    node.mga_statement_context = dag->mga_statement_context;
  }
}

exec::TypedPhysicalNodeDag Dag() {
  exec::TypedPhysicalNodeDag dag;
  dag.abi_version = 2;
  dag.selected_plan_uuid = Uuid(401);
  dag.root_physical_node_id = 43;
  dag.local_transaction_id = 501;
  dag.statement_snapshot_id = 502;
  dag.mga_statement_context = {
      Uuid(540), Uuid(541), Uuid(542), Uuid(543), 501, 502, 501, 501,
      501, 501, {501}, {}, "statement_stable", 503, true, true, true};
  dag.bound_sblr_tree_uuid = Uuid(551);
  dag.catalog_epoch_uuid = Uuid(552);
  dag.security_context_uuid = Uuid(553);
  dag.capability_snapshot_uuid = Uuid(555);
  dag.resource_snapshot_uuid = Uuid(556);
  dag.statistics_snapshot_uuid = Uuid(557);
  dag.route_snapshot_uuid = Uuid(558);
  dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest, dag.bound_sblr_tree_uuid},
      {exec::PhysicalAdmissionStage::kCatalogEpoch, dag.catalog_epoch_uuid},
      {exec::PhysicalAdmissionStage::kSecurity, dag.security_context_uuid},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       dag.mga_statement_context.statement_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       dag.capability_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kResource, dag.resource_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       dag.statistics_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       dag.route_snapshot_uuid},
  };
  // Deliberately not topological: dispatch must follow typed input edges.
  dag.nodes = {
      {.physical_node_id = 43,
       .relational_node_id = 43,
       .node_kind = exec::PhysicalNodeKind::kProject,
       .implementation_id = "project.typed.v1",
       .input_physical_node_ids = {42},
       .output_descriptor_ids = {433},
       .causal_counter_id = 4301},
      {.physical_node_id = 41,
       .relational_node_id = 41,
       .node_kind = exec::PhysicalNodeKind::kScan,
       .implementation_id = "scan.index.v1",
       .output_descriptor_ids = {411, 412},
       .causal_counter_id = 4101},
      {.physical_node_id = 42,
       .relational_node_id = 42,
       .node_kind = exec::PhysicalNodeKind::kFilter,
       .implementation_id = "filter.3vl.v1",
       .input_physical_node_ids = {41},
       .output_descriptor_ids = {411, 412},
       .causal_counter_id = 4201},
  };
  dag.catalog_generation = 1;
  dag.security_epoch = 1;
  dag.policy_epoch = 1;
  dag.resource_epoch = 1;
  dag.statistics_generation = 1;
  dag.route_epoch = 1;
  dag.route_generation = 1;
  dag.memory_budget_bytes = 1 << 20;
  dag.optimizer_published = true;
  dag.immutable_node_identity_validated = true;
  dag.capability_validated_before_access = true;
  BindPublishedNodeContexts(&dag);
  return dag;
}

exec::CanonicalPhysicalDispatchStepResult Step(
    const exec::TypedPhysicalNodeDag& dag,
    const exec::PhysicalNodeRecord& node,
    const std::uint64_t handle) {
  exec::CanonicalPhysicalDispatchStepResult result;
  result.selected_plan_uuid = dag.selected_plan_uuid;
  result.executed_physical_node_id = node.physical_node_id;
  result.causal_counter_id = node.causal_counter_id;
  result.result_handle_id = handle;
  result.output_descriptor_ids = node.output_descriptor_ids;
  result.authority.engine_mga_snapshot_bound = true;
  return result;
}

exec::CanonicalPhysicalExecutorRegistration Registration(
    const exec::TypedPhysicalNodeDag& dag,
    const std::uint64_t physical_node_id,
    exec::CanonicalPhysicalNodeExecutor execute) {
  const auto node = std::ranges::find_if(dag.nodes, [&](const auto& candidate) {
    return candidate.physical_node_id == physical_node_id;
  });
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = node->node_kind;
  registration.implementation_id = node->implementation_id;
  registration.execute = std::move(execute);
  registration.executor_capability_uuid = node->executor_capability_uuid;
  registration.executor_capability_abi_version =
      node->executor_capability_abi_version;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  return registration;
}

exec::CanonicalScanAccessRequest ScanRequest(
    const exec::TypedPhysicalNodeDag& dag) {
  exec::CanonicalScanAccessRequest request;
  request.physical_dag = dag;
  request.selected_physical_node_id = 41;
  request.available_implementation_id = "scan.index.v1";
  request.relation_uuid = Uuid(601);
  request.inventory_local_transaction_id = dag.local_transaction_id;
  request.inventory_statement_snapshot_id = dag.statement_snapshot_id;
  request.selected_descriptor_generation = 9;
  request.current_descriptor_generation = 9;
  exec::CanonicalScanCandidateEvidence candidate;
  candidate.candidate_uuid = Uuid(602);
  candidate.record_uuid = Uuid(603);
  candidate.relation_uuid = request.relation_uuid;
  candidate.visibility_decision_uuid = Uuid(604);
  candidate.row_version_id = 605;
  candidate.candidate_generation = 3;
  candidate.observed_generation = 3;
  candidate.source = exec::CanonicalScanCandidateSource::kIndexEntry;
  candidate.visibility = exec::CanonicalMgaVisibilityDecision::kVisible;
  candidate.security_decision = exec::CanonicalMgaSecurityDecision::kAllowed;
  candidate.residual_truth = api::EngineSqlTruthValue::true_value;
  candidate.locator_identity_matches = true;
  request.candidates = {std::move(candidate)};
  return request;
}

exec::CanonicalPhysicalDagDispatchRequest Request(
    std::vector<std::uint64_t>* invocation_order) {
  exec::CanonicalPhysicalDagDispatchRequest request;
  request.physical_dag = Dag();
  request.inventory_local_transaction_id = 501;
  request.inventory_statement_snapshot_id = 502;
  const auto scan_request = ScanRequest(request.physical_dag);

  request.available_executors.push_back(Registration(
      request.physical_dag, 41,
       [invocation_order, scan_request](const auto& dag, const auto& node,
                                        const auto& inputs) {
         invocation_order->push_back(node.physical_node_id);
         if (!inputs.empty()) {
           auto result = Step(dag, node, 0);
           result.diagnostic.ok = false;
           result.diagnostic.diagnostic_code =
               "TEST_SCAN_INPUT_CARDINALITY";
           return result;
         }
         const auto scan =
             exec::ExecuteCanonicalSelectedScanAccess(scan_request);
         if (!scan.diagnostic.ok || scan.accepted_row_version_ids.size() != 1) {
           auto result = Step(dag, node, 0);
           result.diagnostic = scan.diagnostic;
           return result;
         }
         return Step(dag, node, 9001);
       }));
  request.available_executors.push_back(Registration(
      request.physical_dag, 42,
       [invocation_order](const auto& dag, const auto& node,
                          const auto& inputs) {
         invocation_order->push_back(node.physical_node_id);
         if (inputs.size() != 1 || inputs[0].physical_node_id != 41 ||
             inputs[0].result_handle_id != 9001) {
           auto result = Step(dag, node, 0);
           result.diagnostic.ok = false;
           result.diagnostic.diagnostic_code =
               "TEST_FILTER_INPUT_IDENTITY";
           return result;
         }
         return Step(dag, node, 9002);
       }));
  request.available_executors.push_back(Registration(
      request.physical_dag, 43,
       [invocation_order](const auto& dag, const auto& node,
                          const auto& inputs) {
         invocation_order->push_back(node.physical_node_id);
         if (inputs.size() != 1 || inputs[0].physical_node_id != 42 ||
             inputs[0].result_handle_id != 9002) {
           auto result = Step(dag, node, 0);
           result.diagnostic.ok = false;
           result.diagnostic.diagnostic_code =
               "TEST_PROJECT_INPUT_IDENTITY";
           return result;
         }
         return Step(dag, node, 9003);
       }));
  return request;
}

// QOW-TEST-QRY-004-CONSUMPTION-V1
bool ValidateCausalDagConsumption() {
  std::vector<std::uint64_t> invocation_order;
  const auto result =
      exec::ExecuteCanonicalPhysicalDag(Request(&invocation_order));
  bool passed = true;
  passed &= Require(result.diagnostic.ok && !result.replan_required,
                    "valid selected physical DAG was refused");
  passed &= Require(invocation_order ==
                            std::vector<std::uint64_t>{41, 42, 43} &&
                        result.executed_steps.size() == 3,
                    "physical nodes did not execute in dependency order");
  passed &= Require(result.root_result_handle_id == 9003 &&
                        result.root_output_descriptor_ids ==
                            std::vector<std::uint32_t>{433} &&
                        result.selected_plan_uuid == Uuid(401) &&
                        result.executed_root_physical_node_id == 43 &&
                        result.root_causal_counter_id == 4301,
                    "dispatch root lost result or causal evidence");
  passed &= Require(result.authority.engine_mga_snapshot_bound &&
                        !result.authority.owns_transaction_finality &&
                        !result.authority.owns_recovery &&
                        !result.authority.owns_parser_execution &&
                        !result.authority.owns_visibility_outside_engine_mga &&
                        !result.authority.wal_is_transaction_or_recovery_authority,
                    "physical dispatcher acquired forbidden authority");
  return passed;
}

bool ValidateSharedNodeExecutesOnce() {
  auto dag = Dag();
  dag.root_physical_node_id = 54;
  dag.nodes = {
      {.physical_node_id = 54,
       .relational_node_id = 54,
       .node_kind = exec::PhysicalNodeKind::kSetOperation,
       .implementation_id = "set.union-all.v1",
       .input_physical_node_ids = {52, 53},
       .output_descriptor_ids = {541},
       .causal_counter_id = 5401},
      {.physical_node_id = 51,
       .relational_node_id = 51,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.literal.v1",
       .output_descriptor_ids = {511},
       .shareable = true,
       .causal_counter_id = 5101},
      {.physical_node_id = 52,
       .relational_node_id = 52,
       .node_kind = exec::PhysicalNodeKind::kFilter,
       .implementation_id = "filter.shared.v1",
       .input_physical_node_ids = {51},
       .output_descriptor_ids = {511},
       .causal_counter_id = 5201},
      {.physical_node_id = 53,
       .relational_node_id = 53,
       .node_kind = exec::PhysicalNodeKind::kProject,
       .implementation_id = "project.shared.v1",
       .input_physical_node_ids = {51},
       .output_descriptor_ids = {531},
       .causal_counter_id = 5301},
  };
  BindPublishedNodeContexts(&dag);

  exec::CanonicalPhysicalDagDispatchRequest request;
  request.physical_dag = dag;
  request.inventory_local_transaction_id = dag.local_transaction_id;
  request.inventory_statement_snapshot_id = dag.statement_snapshot_id;
  std::vector<std::uint64_t> invocation_order;
  const auto register_executor = [&](const exec::PhysicalNodeKind kind,
                                     const std::string& implementation) {
    const auto node = std::ranges::find_if(
        dag.nodes, [&](const auto& candidate) {
          return candidate.node_kind == kind &&
                 candidate.implementation_id == implementation;
        });
    request.available_executors.push_back(Registration(
        dag, node->physical_node_id,
         [&invocation_order](const auto& selected_dag, const auto& node,
                             const auto&) {
           invocation_order.push_back(node.physical_node_id);
           return Step(selected_dag, node, 9100 + node.physical_node_id);
         }));
  };
  register_executor(exec::PhysicalNodeKind::kValues, "values.literal.v1");
  register_executor(exec::PhysicalNodeKind::kFilter, "filter.shared.v1");
  register_executor(exec::PhysicalNodeKind::kProject, "project.shared.v1");
  register_executor(exec::PhysicalNodeKind::kSetOperation,
                    "set.union-all.v1");

  const auto result = exec::ExecuteCanonicalPhysicalDag(request);
  return Require(result.diagnostic.ok &&
                     invocation_order ==
                         std::vector<std::uint64_t>{51, 52, 53, 54} &&
                     result.executed_steps.size() == 4 &&
                     result.root_result_handle_id == 9154,
                 "shareable physical input executed more than once");
}

bool ValidatePreflightAndSnapshotRefusal() {
  bool passed = true;
  std::vector<std::uint64_t> invocation_order;
  auto request = Request(&invocation_order);
  request.available_executors.pop_back();
  auto result = exec::ExecuteCanonicalPhysicalDag(request);
  passed &= Require(!result.diagnostic.ok && result.replan_required &&
                        result.diagnostic.diagnostic_code ==
                            "QOW-DIAG-QRY-004-PHYSICAL-IMPLEMENTATION-UNAVAILABLE-V1" &&
                        invocation_order.empty() &&
                        result.executed_steps.empty(),
                    "missing implementation executed a partial DAG");

  invocation_order.clear();
  request = Request(&invocation_order);
  request.inventory_statement_snapshot_id = 999;
  result = exec::ExecuteCanonicalPhysicalDag(request);
  passed &= Require(!result.diagnostic.ok &&
                        result.diagnostic.diagnostic_code ==
                            "SB_DIAG_MGA_READ_SNAPSHOT_MISSING" &&
                        invocation_order.empty(),
                    "snapshot mismatch reached a physical executor");

  invocation_order.clear();
  request = Request(&invocation_order);
  request.physical_dag.nodes.front().mga_statement_context.current = false;
  result = exec::ExecuteCanonicalPhysicalDag(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-PHYSICAL-NODE-ABI-CAPABILITY" &&
          invocation_order.empty(),
      "node-level MGA vector drift reached physical dispatch");
  return passed;
}

bool ValidateEvidenceAndRegistryRefusal() {
  bool passed = true;
  std::vector<std::uint64_t> invocation_order;
  auto request = Request(&invocation_order);
  request.available_executors.back().execute =
      [&invocation_order](const auto& dag, const auto& node,
                          const auto&) {
        invocation_order.push_back(node.physical_node_id);
        auto result = Step(dag, node, 9003);
        result.causal_counter_id = 999;
        result.authority.owns_transaction_finality = true;
        return result;
      };
  auto result = exec::ExecuteCanonicalPhysicalDag(request);
  passed &= Require(!result.diagnostic.ok &&
                        result.diagnostic.diagnostic_code ==
                            "QOW-DIAG-QRY-004-PHYSICAL-EXECUTION-EVIDENCE-V1" &&
                        result.executed_steps.empty() &&
                        result.root_result_handle_id == 0,
                    "mismatched or forbidden executor evidence was published");

  invocation_order.clear();
  request = Request(&invocation_order);
  request.available_executors.push_back(request.available_executors.front());
  result = exec::ExecuteCanonicalPhysicalDag(request);
  passed &= Require(!result.diagnostic.ok &&
                        result.diagnostic.diagnostic_code ==
                            "QOW-DIAG-QRY-004-PHYSICAL-EXECUTOR-REGISTRY-V1" &&
                        invocation_order.empty(),
                    "duplicate implementation registration was accepted");

  invocation_order.clear();
  request = Request(&invocation_order);
  request.available_executors.front().execute =
      [](const auto&, const auto&, const auto&)
          -> exec::CanonicalPhysicalDispatchStepResult {
        throw std::runtime_error("bounded test fault");
      };
  result = exec::ExecuteCanonicalPhysicalDag(request);
  passed &= Require(!result.diagnostic.ok &&
                        result.diagnostic.diagnostic_code ==
                            "QOW-DIAG-QRY-004-PHYSICAL-EXECUTOR-FAILURE-V1" &&
                        result.executed_steps.empty(),
                    "executor exception escaped the dispatch boundary");
  return passed;
}

}  // namespace

int main() {
  if (!ValidateCausalDagConsumption() || !ValidateSharedNodeExecutesOnce() ||
      !ValidatePreflightAndSnapshotRefusal() ||
      !ValidateEvidenceAndRegistryRefusal()) {
    return 1;
  }
  std::cout << "QOW-TEST-QRY-004-CONSUMPTION-V1: PASS\n";
  return 0;
}
