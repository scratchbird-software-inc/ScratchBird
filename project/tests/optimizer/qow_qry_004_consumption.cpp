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
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

constexpr std::uint64_t kOwner = 0xffff'ffff'ffff'ff00ULL;
constexpr std::uint64_t kOldestActive = 0xffff'ffff'ffff'fee8ULL;
constexpr std::uint64_t kHorizon = 0xffff'ffff'ffff'fed0ULL;
constexpr std::uint64_t kInDoubt = 0xffff'ffff'ffff'fef0ULL;
constexpr std::uint64_t kInventoryNext = 0xffff'ffff'ffff'fff0ULL;

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

exec::CanonicalExecutionMgaAuthority ClosureAuthority(
    const exec::TypedPhysicalNodeDag& dag) {
  exec::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = dag.mga_statement_context;
  authority.origin = exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  const auto current = authority.statement_context;
  authority.resolve_current = [current] {
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = current;
    return resolution;
  };
  return authority;
}

exec::TypedPhysicalNodeDag OperatorLocalDag(
    const exec::TypedPhysicalNodeDag& dag,
    const std::uint64_t root_physical_node_id) {
  auto local = dag;
  local.root_physical_node_id = root_physical_node_id;
  std::vector<std::uint64_t> retained{root_physical_node_id};
  for (std::size_t index = 0; index < retained.size(); ++index) {
    const auto node = std::ranges::find_if(
        dag.nodes, [&](const auto& candidate) {
          return candidate.physical_node_id == retained[index];
        });
    if (node == dag.nodes.end()) continue;
    for (const auto input_id : node->input_physical_node_ids) {
      if (std::ranges::find(retained, input_id) == retained.end()) {
        retained.push_back(input_id);
      }
    }
  }
  std::erase_if(local.nodes, [&](const auto& node) {
    return std::ranges::find(retained, node.physical_node_id) ==
           retained.end();
  });
  return local;
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
  dag.local_transaction_id = kOwner;
  dag.statement_snapshot_id = 0;
  dag.mga_statement_context = {
      Uuid(540), Uuid(541), Uuid(542), Uuid(543), kOwner, 0,
      kOldestActive, kHorizon, kHorizon, kHorizon,
      {kOldestActive, kOwner}, {kInDoubt}, "statement_stable",
      kInventoryNext, true, true, true};
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
       .implementation_id = "project.typed.row.v1",
       .input_physical_node_ids = {42},
       .output_descriptor_ids = {412},
       .causal_counter_id = 4301},
      {.physical_node_id = 41,
       .relational_node_id = 41,
       .node_kind = exec::PhysicalNodeKind::kScan,
       .implementation_id = "scan.index.btree.v1",
       .output_descriptor_ids = {411, 412},
       .causal_counter_id = 4101},
      {.physical_node_id = 42,
       .relational_node_id = 42,
       .node_kind = exec::PhysicalNodeKind::kFilter,
       .implementation_id = "filter.3vl.row.v1",
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
  result.mga_statement_context = dag.mga_statement_context;
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
  request.available_implementation_id = "scan.index.btree.v1";
  request.relation_uuid = Uuid(601);
  request.mga_authority = ClosureAuthority(dag);
  request.selected_descriptor_generation = 9;
  request.current_descriptor_generation = 9;
  const auto candidate = [&](const std::uint64_t seed) {
    exec::CanonicalScanCandidateEvidence value;
    value.candidate_uuid = Uuid(seed);
    value.record_uuid = Uuid(seed + 1);
    value.relation_uuid = request.relation_uuid;
    value.visibility_decision_uuid = Uuid(seed + 2);
    value.creator_local_transaction_id = kOwner;
    value.row_version_id = seed + 3;
    value.candidate_generation = 3;
    value.observed_generation = 3;
    value.source = exec::CanonicalScanCandidateSource::kIndexEntry;
    value.visibility = exec::CanonicalMgaVisibilityDecision::kVisible;
    value.security_decision = exec::CanonicalMgaSecurityDecision::kAllowed;
    value.residual_truth = api::EngineSqlTruthValue::true_value;
    value.locator_identity_matches = true;
    return value;
  };
  request.candidates = {candidate(602), candidate(612), candidate(622)};
  return request;
}

exec::DescriptorBatch ScanBatch() {
  auto key = exec::MakeExecutorDescriptor(
      "int64", "type_uuid=" + Uuid(641) + ";nullability=non_null");
  key.descriptor_uuid.canonical = Uuid(642);
  key.descriptor_kind = "scalar";
  auto label = exec::MakeExecutorDescriptor(
      "text", "type_uuid=" + Uuid(643) + ";nullability=non_null");
  label.descriptor_uuid.canonical = Uuid(644);
  label.descriptor_kind = "scalar";
  return exec::MakeDescriptorBatch(
      {{"key", key, false, 411}, {"label", label, false, 412}},
      {{{exec::MakeExecutorValue(key, "1"),
         exec::MakeExecutorValue(label, "discard")}},
       {{exec::MakeExecutorValue(key, "2"),
         exec::MakeExecutorValue(label, "keep")}},
       {{exec::MakeExecutorValue(key, "3"),
         exec::MakeExecutorValue(label, "unknown")}}});
}

exec::DescriptorBatch BatchForNode(const exec::PhysicalNodeRecord& node,
                                   const std::size_t row_count) {
  std::vector<exec::ExecutorColumnDescriptor> columns;
  columns.reserve(node.output_descriptor_ids.size());
  for (const auto descriptor_id : node.output_descriptor_ids) {
    auto descriptor = exec::MakeExecutorDescriptor(
        "int64", "type_uuid=" + Uuid(10000 + descriptor_id) +
                     ";nullability=non_null");
    descriptor.descriptor_uuid.canonical = Uuid(20000 + descriptor_id);
    descriptor.descriptor_kind = "scalar";
    columns.push_back({"value_" + std::to_string(descriptor_id), descriptor,
                       false, descriptor_id});
  }
  std::vector<exec::DescriptorTuple> rows;
  rows.reserve(row_count);
  for (std::size_t row = 0; row < row_count; ++row) {
    exec::DescriptorTuple tuple;
    tuple.values.reserve(columns.size());
    for (const auto& column : columns) {
      tuple.values.push_back(
          exec::MakeExecutorValue(column.descriptor, std::to_string(row + 1)));
    }
    rows.push_back(std::move(tuple));
  }
  return exec::MakeDescriptorBatch(std::move(columns), std::move(rows));
}

exec::CanonicalPhysicalDagDispatchRequest Request(
    std::vector<std::uint64_t>* invocation_order) {
  exec::CanonicalPhysicalDagDispatchRequest request;
  request.physical_dag = Dag();
  request.mga_authority = ClosureAuthority(request.physical_dag);
  const auto scan_request = ScanRequest(request.physical_dag);
  const auto scan_batch = ScanBatch();
  request.runtime_limits.maximum_rows_per_batch = 16;
  request.runtime_limits.maximum_columns_per_batch = 8;
  request.runtime_limits.maximum_cells_per_batch = 64;
  request.runtime_limits.maximum_total_materialized_rows = 32;
  request.runtime_limits.maximum_total_materialized_cells = 128;
  request.cancellation_requested = [] { return false; };

  request.available_executors.push_back(Registration(
      request.physical_dag, 41,
       [invocation_order, scan_request, scan_batch](const auto& dag,
                                                    const auto& node,
                                                    const auto& inputs) {
         invocation_order->push_back(node.physical_node_id);
         if (!inputs.empty()) {
           auto result = Step(dag, node, 0);
           result.diagnostic.ok = false;
           result.diagnostic.diagnostic_code =
               "TEST_SCAN_INPUT_CARDINALITY";
           return result;
         }
         auto local_scan_request = scan_request;
         local_scan_request.physical_dag =
             OperatorLocalDag(dag, node.physical_node_id);
         const auto scan =
             exec::ExecuteCanonicalSelectedScanAccess(local_scan_request);
         if (!scan.diagnostic.ok || scan.accepted_row_version_ids.size() != 3) {
           auto result = Step(dag, node, 0);
           result.diagnostic = scan.diagnostic;
           return result;
         }
         auto result = Step(dag, node, 9001);
         result.output_row_count = scan_batch.rows.size();
         result.rows_examined = scan.counters.candidate_count;
         result.materialized_output_batch = scan_batch;
         result.data_access_observation_known = true;
         result.data_access_observed = true;
         return result;
       }));
  request.available_executors.push_back(Registration(
      request.physical_dag, 42,
       [invocation_order](const auto& dag, const auto& node,
                          const auto& inputs) {
         invocation_order->push_back(node.physical_node_id);
         if (inputs.size() != 1 || inputs[0].physical_node_id != 41 ||
             inputs[0].result_handle_id != 9001 ||
             !inputs[0].materialized_output_batch.has_value()) {
           auto result = Step(dag, node, 0);
           result.diagnostic.ok = false;
           result.diagnostic.diagnostic_code =
               "TEST_FILTER_INPUT_IDENTITY";
           return result;
         }
         const auto& input_batch = *inputs[0].materialized_output_batch;
         auto filtered_batch = input_batch;
         filtered_batch.rows = {input_batch.rows[1]};
         auto result = Step(dag, node, 9002);
         result.input_row_count = input_batch.rows.size();
         result.output_row_count = filtered_batch.rows.size();
         result.rows_examined = input_batch.rows.size();
         result.materialized_output_batch = std::move(filtered_batch);
         return result;
       }));
  request.available_executors.push_back(Registration(
      request.physical_dag, 43,
       [invocation_order](const auto& dag, const auto& node,
                          const auto& inputs) {
         invocation_order->push_back(node.physical_node_id);
         if (inputs.size() != 1 || inputs[0].physical_node_id != 42 ||
             inputs[0].result_handle_id != 9002 ||
             !inputs[0].materialized_output_batch.has_value()) {
           auto result = Step(dag, node, 0);
           result.diagnostic.ok = false;
           result.diagnostic.diagnostic_code =
               "TEST_PROJECT_INPUT_IDENTITY";
           return result;
         }
         exec::CanonicalDescriptorProjectionRequest project_request;
         project_request.physical_dag =
             OperatorLocalDag(dag, node.physical_node_id);
         project_request.selected_physical_node_id = node.physical_node_id;
         project_request.input_batch = *inputs[0].materialized_output_batch;
         project_request.projected_columns = {1};
         project_request.mga_authority = ClosureAuthority(dag);
         const auto project =
             exec::ExecuteCanonicalDescriptorProjection(project_request);
         if (!project.diagnostic.ok) {
           auto result = Step(dag, node, 0);
           result.diagnostic = project.diagnostic;
           return result;
         }
         auto result = Step(dag, node, 9003);
         result.input_row_count = project_request.input_batch.rows.size();
         result.output_row_count = project.output_batch.rows.size();
         result.rows_examined = project_request.input_batch.rows.size();
         result.materialized_output_batch = project.output_batch;
         return result;
       }));
  return request;
}

// QOW-TEST-QRY-004-CONSUMPTION-V1
bool ValidateCausalDagConsumption() {
  std::vector<std::uint64_t> invocation_order;
  const auto result =
      exec::ExecuteCanonicalPhysicalDag(Request(&invocation_order));
  if (!result.diagnostic.ok || result.replan_required) {
    return Require(
        false,
        std::string("valid selected physical DAG was refused: ") +
            result.diagnostic.diagnostic_code + "/" +
            result.diagnostic.detail);
  }
  bool passed = true;
  passed &= Require(invocation_order ==
                            std::vector<std::uint64_t>{41, 42, 43} &&
                        result.executed_steps.size() == 3,
                    "physical nodes did not execute in dependency order");
  passed &= Require(result.root_result_handle_id == 9003 &&
                        result.root_output_descriptor_ids ==
                            std::vector<std::uint32_t>{412} &&
                        result.selected_plan_uuid == Uuid(401) &&
                        result.executed_root_physical_node_id == 43 &&
                        result.root_causal_counter_id == 4301 &&
                        result.mga_statement_context
                                .owning_local_transaction_id == kOwner &&
                        result.mga_statement_context
                                .visible_committed_high_watermark == 0 &&
                        exec::PhysicalMgaStatementContextEqual(
                            result.mga_statement_context,
                            Dag().mga_statement_context) &&
                        std::ranges::all_of(
                            result.executed_steps, [&](const auto& step) {
                              return exec::PhysicalMgaStatementContextEqual(
                                  step.mga_statement_context,
                                  result.mga_statement_context);
                            }),
                    "dispatch root lost result or causal evidence");
  passed &= Require(
      result.data_access_observed && !result.cancellation_observed &&
          result.executed_steps[0].output_row_count == 3 &&
          result.executed_steps[1].input_row_count == 3 &&
          result.executed_steps[1].output_row_count == 1 &&
          result.executed_steps[2].input_row_count == 1 &&
          result.executed_steps[2].output_row_count == 1 &&
          result.executed_steps[2].materialized_output_batch.has_value() &&
          result.executed_steps[2].materialized_output_batch->columns.size() ==
              1 &&
          result.executed_steps[2].materialized_output_batch->rows.size() == 1 &&
          result.executed_steps[2]
                  .materialized_output_batch->rows[0]
                  .values[0]
                  .encoded_value == "keep",
      "typed SCAN/FILTER/PROJECT batches or causal counters were not consumed");
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
  request.mga_authority = ClosureAuthority(dag);
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
                             const auto& inputs) {
           invocation_order.push_back(node.physical_node_id);
           std::uint64_t input_rows = 0;
           for (const auto& input : inputs) {
             if (!input.materialized_output_batch.has_value()) {
               auto result = Step(selected_dag, node, 0);
               result.diagnostic.ok = false;
               result.diagnostic.diagnostic_code =
                   "TEST_SHARED_TYPED_INPUT";
               return result;
             }
             input_rows += input.materialized_output_batch->rows.size();
           }
           auto result =
               Step(selected_dag, node, 9100 + node.physical_node_id);
           result.input_row_count = input_rows;
           result.output_row_count = 1;
           result.rows_examined = inputs.empty() ? 1 : input_rows;
           result.materialized_output_batch = BatchForNode(node, 1);
           return result;
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
  auto drifted_current = request.mga_authority.statement_context;
  drifted_current.statement_snapshot_uuid = Uuid(999);
  request.mga_authority.resolve_current = [drifted_current] {
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = drifted_current;
    return resolution;
  };
  result = exec::ExecuteCanonicalPhysicalDag(request);
  passed &= Require(!result.diagnostic.ok &&
                        result.diagnostic.diagnostic_code ==
                            "QOW-DIAG-MGA-RUNTIME-CURRENT-V1" &&
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

bool ValidateCompletePreflightMgaRefusalMatrix() {
  const auto expect_refusal = [](auto mutation,
                                 const std::string_view diagnostic,
                                 const std::string_view detail) {
    std::vector<std::uint64_t> invocation_order;
    auto request = Request(&invocation_order);
    mutation(request);
    const auto result = exec::ExecuteCanonicalPhysicalDag(request);
    return Require(!result.diagnostic.ok &&
                       result.diagnostic.diagnostic_code == diagnostic &&
                       !result.replan_required &&
                       !result.data_access_observed &&
                       invocation_order.empty() &&
                       result.executed_steps.empty() &&
                       result.root_result_handle_id == 0 &&
                       result.root_output_descriptor_ids.empty() &&
                       result.selected_plan_uuid.empty() &&
                       result.executed_root_physical_node_id == 0 &&
                       result.root_causal_counter_id == 0 &&
                       !exec::PhysicalMgaStatementContextValid(
                           result.mga_statement_context),
                   detail);
  };
  bool passed = true;
  passed &= expect_refusal(
      [](auto& request) {
        request.mga_authority.origin =
            exec::CanonicalMgaAuthorityOrigin::kMissing;
      },
      "QOW-DIAG-MGA-RUNTIME-AUTHORITY-V1",
      "missing MGA authority reached a physical executor");
  passed &= expect_refusal(
      [](auto& request) { request.mga_authority.resolve_current = {}; },
      "QOW-DIAG-MGA-RUNTIME-AUTHORITY-V1",
      "missing current resolver reached a physical executor");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga_authority.statement_context.statement_uuid.clear();
      },
      "QOW-DIAG-MGA-RUNTIME-AUTHORITY-V1",
      "malformed carried context reached a physical executor");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga_authority.statement_context.owning_local_transaction_id =
            static_cast<std::uint32_t>(kOwner);
      },
      "QOW-DIAG-MGA-RUNTIME-AUTHORITY-V1",
      "narrowed carried transaction identity reached a physical executor");
  const auto expect_current_refusal = [&](auto mutation,
                                          const std::string_view detail) {
    return expect_refusal(
        [mutation](auto& request) {
          auto current = request.mga_authority.statement_context;
          mutation(current);
          request.mga_authority.resolve_current = [current] {
            exec::CanonicalMgaCurrentResolution resolution;
            resolution.statement_context = current;
            return resolution;
          };
        },
        "QOW-DIAG-MGA-RUNTIME-CURRENT-V1", detail);
  };
  passed &= expect_current_refusal(
      [](auto& current) { current.statement_snapshot_uuid = Uuid(999); },
      "swapped resolved snapshot reached a physical executor");
  passed &= expect_current_refusal(
      [](auto& current) {
        current.in_doubt_excluded_local_transaction_ids =
            {kInDoubt, kInDoubt};
      },
      "duplicate resolved exclusion reached a physical executor");
  passed &= expect_current_refusal(
      [](auto& current) {
        current.publication_inventory_next_local_transaction_id =
            static_cast<std::uint32_t>(kInventoryNext);
      },
      "truncated resolved inventory reached a physical executor");
  passed &= expect_current_refusal(
      [](auto& current) { current.current = false; },
      "stale resolved vector reached a physical executor");
  passed &= expect_refusal(
      [](auto& request) {
        request.physical_dag.nodes.front().mga_statement_context.current =
            false;
      },
      "QOW-DIAG-PHYSICAL-NODE-ABI-CAPABILITY",
      "node-swapped statement context reached a physical executor");
  passed &= expect_refusal(
      [](auto& request) {
        request.physical_dag.catalog_epoch_uuid =
            request.physical_dag.mga_statement_context
                .statement_metadata_snapshot_uuid;
        request.physical_dag.admission_evidence[1].evidence_uuid =
            request.physical_dag.catalog_epoch_uuid;
      },
      "QOW-DIAG-PHYSICAL-NODE-ABI-PUBLICATION",
      "metadata/catalog conflation reached a physical executor");
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

bool ValidateTypedBatchCancellationAndResourceAtomicity() {
  bool passed = true;

  std::vector<std::uint64_t> invocation_order;
  auto request = Request(&invocation_order);
  request.cancellation_requested = [] { return true; };
  auto result = exec::ExecuteCanonicalPhysicalDag(request);
  passed &= Require(
      !result.diagnostic.ok && result.cancellation_observed &&
          !result.execution_started && !result.data_access_observed &&
          invocation_order.empty() && result.executed_steps.empty() &&
          result.root_result_handle_id == 0,
      "pre-dispatch cancellation exposed execution or typed output");

  invocation_order.clear();
  request = Request(&invocation_order);
  request.cancellation_requested = [&invocation_order] {
    return !invocation_order.empty();
  };
  result = exec::ExecuteCanonicalPhysicalDag(request);
  passed &= Require(
      !result.diagnostic.ok && result.cancellation_observed &&
          result.execution_started && result.data_access_observed &&
          invocation_order == std::vector<std::uint64_t>{41} &&
          result.executed_steps.empty() && result.root_result_handle_id == 0 &&
          result.root_output_descriptor_ids.empty(),
      "mid-DAG cancellation published the completed scan batch");

  invocation_order.clear();
  request = Request(&invocation_order);
  request.runtime_limits.maximum_rows_per_batch = 2;
  result = exec::ExecuteCanonicalPhysicalDag(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "SBLR.PLAN_TREE.RESOURCE_LIMIT" &&
          result.execution_started && result.data_access_observed &&
          !result.cancellation_observed &&
          invocation_order == std::vector<std::uint64_t>{41} &&
          result.executed_steps.empty(),
      "post-read typed-batch resource overflow exposed partial output");

  invocation_order.clear();
  request = Request(&invocation_order);
  auto scan_executor = request.available_executors.front().execute;
  request.available_executors.front().execute =
      [scan_executor](const auto& dag, const auto& node, const auto& inputs) {
        auto step = scan_executor(dag, node, inputs);
        step.materialized_output_batch.reset();
        return step;
      };
  result = exec::ExecuteCanonicalPhysicalDag(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-004-PHYSICAL-TYPED-BATCH-REQUIRED-V1" &&
          result.execution_started && result.data_access_observed &&
          invocation_order == std::vector<std::uint64_t>{41} &&
          result.executed_steps.empty(),
      "opaque-handle-only scan bypassed the typed batch route");

  invocation_order.clear();
  request = Request(&invocation_order);
  scan_executor = request.available_executors.front().execute;
  request.available_executors.front().execute =
      [scan_executor](const auto& dag, const auto& node, const auto& inputs) {
        auto step = scan_executor(dag, node, inputs);
        ++step.output_row_count;
        return step;
      };
  result = exec::ExecuteCanonicalPhysicalDag(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-004-PHYSICAL-CAUSAL-COUNTER-V1" &&
          result.execution_started && result.data_access_observed &&
          invocation_order == std::vector<std::uint64_t>{41} &&
          result.executed_steps.empty(),
      "typed batch and output counter mismatch reached a consumer");

  invocation_order.clear();
  request = Request(&invocation_order);
  request.runtime_limits.maximum_total_materialized_cells = 0;
  result = exec::ExecuteCanonicalPhysicalDag(request);
  passed &= Require(
      !result.diagnostic.ok && !result.execution_started &&
          !result.data_access_observed && invocation_order.empty(),
      "zero cumulative resource bound reached physical execution");
  return passed;
}

bool ValidateFetchFinalRevalidationScrubsOutput() {
  auto dag = Dag();
  dag.root_physical_node_id = 62;
  dag.nodes = {
      {.physical_node_id = 61,
       .relational_node_id = 61,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.fetch-input.v1",
       .output_descriptor_ids = {6101},
       .causal_counter_id = 61001},
      {.physical_node_id = 62,
       .relational_node_id = 62,
       .node_kind = exec::PhysicalNodeKind::kLimit,
       .implementation_id = "fetch.native.rows-only.v1",
       .input_physical_node_ids = {61},
       .output_descriptor_ids = {6101},
       .causal_counter_id = 61002},
  };
  BindPublishedNodeContexts(&dag);

  auto descriptor = exec::MakeExecutorDescriptor(
      "int64", "type_uuid=" + Uuid(6102) + ";nullability=non_null");
  descriptor.descriptor_uuid.canonical = Uuid(6101);
  descriptor.descriptor_kind = "scalar";

  exec::CanonicalDescriptorFetchProfileRequest request;
  request.physical_dag = dag;
  request.selected_physical_node_id = 62;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"value", descriptor, false, 6101}},
      {{{exec::MakeExecutorValue(descriptor, "1")}},
       {{exec::MakeExecutorValue(descriptor, "2")}}});
  request.form = exec::CanonicalFetchTopProfileForm::fetch_first_rows_only;
  request.row_count = 1;
  request.row_count_is_bound = true;
  request.mga_authority = ClosureAuthority(dag);

  const auto resolution_count = std::make_shared<std::size_t>(0);
  const auto current = request.mga_authority.statement_context;
  request.mga_authority.resolve_current = [resolution_count, current] {
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = current;
    ++*resolution_count;
    if (*resolution_count >= 4) resolution.statement_context.current = false;
    return resolution;
  };

  const auto result = exec::ExecuteCanonicalDescriptorFetchProfile(request);
  return Require(
      *resolution_count == 4 && !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-MGA-RUNTIME-CURRENT-V1" &&
          result.output_batch.columns.empty() && result.output_batch.rows.empty() &&
          result.selected_plan_uuid.empty() &&
          result.executed_physical_node_id == 0 &&
          result.causal_counter_id == 0 &&
          result.mga_statement_context.statement_uuid.empty(),
      "final FETCH MGA refusal exposed output or execution evidence");
}

}  // namespace

int main() {
  if (!ValidateCausalDagConsumption() || !ValidateSharedNodeExecutesOnce() ||
      !ValidatePreflightAndSnapshotRefusal() ||
      !ValidateCompletePreflightMgaRefusalMatrix() ||
      !ValidateEvidenceAndRegistryRefusal() ||
      !ValidateTypedBatchCancellationAndResourceAtomicity() ||
      !ValidateFetchFinalRevalidationScrubsOutput()) {
    return 1;
  }
  std::cout << "QOW-TEST-QRY-004-CONSUMPTION-V1: PASS\n";
  return 0;
}
