// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "physical_node_abi.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

namespace exec = scratchbird::engine::executor;

namespace {

bool Require(const bool condition, const std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

bool HasIssue(const exec::PhysicalNodeAbiValidationResult& result,
              const std::string_view diagnostic_id,
              const std::string_view field_id) {
  for (const auto& issue : result.issues) {
    if (issue.diagnostic_id == diagnostic_id &&
        issue.field_id == field_id) {
      return true;
    }
  }
  return false;
}

exec::TypedPhysicalNodeDag SharedDag() {
  exec::TypedPhysicalNodeDag dag;
  dag.abi_version = 2;
  dag.selected_plan_uuid = "019f0000-0000-7000-8000-000000000401";
  dag.root_physical_node_id = 4;
  dag.local_transaction_id = 41;
  dag.statement_snapshot_id = 43;
  dag.mga_statement_context = {
      "019f0000-0000-7100-8000-000000000411",
      "019f0000-0000-7100-8000-000000000412",
      "019f0000-0000-7100-8000-000000000413",
      "019f0000-0000-7100-8000-000000000414",
      41,
      43,
      41,
      41,
      41,
      41,
      {41},
      {42},
      "statement_stable",
      44,
      true,
      true,
      true,
  };
  dag.bound_sblr_tree_uuid = "019f0000-0000-7100-8000-000000000421";
  dag.catalog_epoch_uuid = "019f0000-0000-7100-8000-000000000422";
  dag.security_context_uuid = "019f0000-0000-7100-8000-000000000423";
  dag.capability_snapshot_uuid = "019f0000-0000-7100-8000-000000000425";
  dag.resource_snapshot_uuid = "019f0000-0000-7100-8000-000000000426";
  dag.statistics_snapshot_uuid = "019f0000-0000-7100-8000-000000000427";
  dag.route_snapshot_uuid = "019f0000-0000-7100-8000-000000000428";
  dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       dag.bound_sblr_tree_uuid},
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
  dag.nodes = {
      {1, 1, exec::PhysicalNodeKind::kValues, "values.literal.v1", {}, {1},
       true, 101},
      {2, 2, exec::PhysicalNodeKind::kFilter, "filter.3vl.v1", {1}, {1},
       false, 102},
      {3, 3, exec::PhysicalNodeKind::kProject, "project.typed.v1", {1}, {2},
       false, 103},
      {4, 4, exec::PhysicalNodeKind::kSetOperation, "set.union-all.v1", {2, 3},
       {3}, false, 104},
  };
  dag.catalog_generation = 1;
  dag.security_epoch = 1;
  dag.policy_epoch = 1;
  dag.resource_epoch = 1;
  dag.statistics_generation = 1;
  dag.route_epoch = 1;
  dag.route_generation = 1;
  dag.memory_budget_bytes = 1 << 20;
  dag.spill_allowed = false;
  dag.optimizer_published = true;
  dag.immutable_node_identity_validated = true;
  dag.capability_validated_before_access = true;
  for (std::size_t index = 0; index < dag.nodes.size(); ++index) {
    auto& node = dag.nodes[index];
    node.selected_alternative_uuid =
        "019f0000-0000-7200-8000-00000000043" +
        std::to_string(index + 1);
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000044" +
        std::to_string(index + 1);
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000045" +
        std::to_string(index + 1);
    node.memory_bytes_required = 1024;
    node.engine_capability_validated = true;
    node.mga_statement_context = dag.mga_statement_context;
  }
  return dag;
}

bool ValidateAcceptedAbi() {
  const auto result = exec::ValidateTypedPhysicalNodeDag(SharedDag());
  bool passed = true;
  passed &= Require(result.accepted, "valid typed physical DAG was refused");
  passed &= Require(result.issues.empty(),
                    "valid typed physical DAG emitted issues");
  passed &= Require(result.validated_node_count == 4 &&
                        result.maximum_observed_depth == 3,
                    "typed physical DAG validation counters differ");
  return passed;
}

bool ValidateAdmissionRefusal() {
  auto order = SharedDag();
  std::swap(order.admission_evidence[2], order.admission_evidence[3]);
  const auto order_result = exec::ValidateTypedPhysicalNodeDag(order);

  auto no_mga = SharedDag();
  no_mga.local_transaction_id = 0;
  const auto mga_result = exec::ValidateTypedPhysicalNodeDag(no_mga);

  bool passed = true;
  passed &= Require(
      !order_result.accepted &&
          HasIssue(order_result, "QOW-DIAG-PHYSICAL-NODE-ABI-ADMISSION",
                   "admission_order_or_evidence"),
      "out-of-order physical admission evidence was accepted");
  passed &= Require(
      !mga_result.accepted &&
          HasIssue(mga_result, "QOW-DIAG-PHYSICAL-NODE-ABI-ADMISSION",
                   "mga_statement_context"),
      "physical DAG without engine MGA statement context was accepted");
  return passed;
}

bool ValidateSnapshotVectorAbi() {
  auto zero_highwater = SharedDag();
  zero_highwater.statement_snapshot_id = 0;
  zero_highwater.mga_statement_context.visible_committed_high_watermark = 0;
  for (auto& node : zero_highwater.nodes) {
    node.mga_statement_context = zero_highwater.mga_statement_context;
  }
  const auto zero_result =
      exec::ValidateTypedPhysicalNodeDag(zero_highwater);

  auto dag_current_mismatch = SharedDag();
  dag_current_mismatch.mga_statement_context.current = false;
  const auto dag_current_result =
      exec::ValidateTypedPhysicalNodeDag(dag_current_mismatch);

  auto node_current_mismatch = SharedDag();
  node_current_mismatch.nodes[0].mga_statement_context.current = false;
  const auto node_current_result =
      exec::ValidateTypedPhysicalNodeDag(node_current_mismatch);

  auto conflated_catalog = SharedDag();
  conflated_catalog.catalog_epoch_uuid =
      conflated_catalog.mga_statement_context.statement_metadata_snapshot_uuid;
  conflated_catalog.admission_evidence[1].evidence_uuid =
      conflated_catalog.catalog_epoch_uuid;
  const auto conflated_catalog_result =
      exec::ValidateTypedPhysicalNodeDag(conflated_catalog);

  auto nil_catalog = SharedDag();
  nil_catalog.catalog_epoch_uuid =
      "00000000-0000-0000-0000-000000000000";
  nil_catalog.admission_evidence[1].evidence_uuid =
      nil_catalog.catalog_epoch_uuid;
  const auto nil_catalog_result =
      exec::ValidateTypedPhysicalNodeDag(nil_catalog);

  bool passed = true;
  passed &= Require(
      zero_result.accepted,
      "zero visible-committed boundary was treated as missing MGA authority");
  passed &= Require(
      !dag_current_result.accepted &&
          HasIssue(dag_current_result,
                   "QOW-DIAG-PHYSICAL-NODE-ABI-PUBLICATION",
                   "optimizer_publication_scope"),
      "non-current DAG MGA snapshot vector was accepted");
  passed &= Require(
      !node_current_result.accepted &&
          HasIssue(node_current_result,
                   "QOW-DIAG-PHYSICAL-NODE-ABI-CAPABILITY",
                   "selected_node_capability_contract"),
      "node/DAG MGA snapshot-vector mismatch was accepted");
  passed &= Require(
      !conflated_catalog_result.accepted &&
          HasIssue(conflated_catalog_result,
                   "QOW-DIAG-PHYSICAL-NODE-ABI-PUBLICATION",
                   "optimizer_publication_scope"),
      "metadata snapshot was accepted as the physical catalog epoch");
  passed &= Require(
      !nil_catalog_result.accepted &&
          HasIssue(nil_catalog_result,
                   "QOW-DIAG-PHYSICAL-NODE-ABI-PUBLICATION",
                   "optimizer_publication_scope"),
      "nil physical catalog epoch was accepted");
  return passed;
}

bool ValidateHandleAndImplementationRefusal() {
  auto dangling = SharedDag();
  dangling.nodes.back().input_physical_node_ids[1] = 99;
  const auto dangling_result = exec::ValidateTypedPhysicalNodeDag(dangling);

  auto implementation = SharedDag();
  implementation.nodes[0].implementation_id = "silent default";
  const auto implementation_result =
      exec::ValidateTypedPhysicalNodeDag(implementation);

  auto counter = SharedDag();
  counter.nodes[1].causal_counter_id = counter.nodes[0].causal_counter_id;
  const auto counter_result = exec::ValidateTypedPhysicalNodeDag(counter);

  bool passed = true;
  passed &= Require(
      !dangling_result.accepted &&
          HasIssue(dangling_result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                   "input_physical_node_ids"),
      "dangling physical input handle was accepted");
  passed &= Require(
      !implementation_result.accepted &&
          HasIssue(implementation_result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                   "physical_node_record"),
      "untyped implementation identity was accepted");
  passed &= Require(
      !counter_result.accepted &&
          HasIssue(counter_result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                   "physical_node_record"),
      "duplicate physical causal counter identity was accepted");
  return passed;
}

bool ValidateGraphRefusal() {
  auto sharing = SharedDag();
  sharing.nodes[0].shareable = false;
  const auto sharing_result = exec::ValidateTypedPhysicalNodeDag(sharing);

  auto cycle = SharedDag();
  cycle.nodes[0].input_physical_node_ids = {4};
  const auto cycle_result = exec::ValidateTypedPhysicalNodeDag(cycle);

  auto orphan = SharedDag();
  auto orphan_node = orphan.nodes.front();
  orphan_node.physical_node_id = 5;
  orphan_node.relational_node_id = 5;
  orphan_node.node_kind = exec::PhysicalNodeKind::kScan;
  orphan_node.implementation_id = "scan.heap.v1";
  orphan_node.input_physical_node_ids.clear();
  orphan_node.output_descriptor_ids = {4};
  orphan_node.shareable = false;
  orphan_node.causal_counter_id = 105;
  orphan_node.selected_alternative_uuid =
      "019f0000-0000-7200-8000-000000000435";
  orphan_node.executor_capability_uuid =
      "019f0000-0000-7200-8000-000000000445";
  orphan_node.cost_vector_uuid =
      "019f0000-0000-7200-8000-000000000455";
  orphan.nodes.push_back(std::move(orphan_node));
  const auto orphan_result = exec::ValidateTypedPhysicalNodeDag(orphan);

  bool passed = true;
  passed &= Require(
      !sharing_result.accepted &&
          HasIssue(sharing_result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                   "shareable"),
      "undeclared shared physical node was accepted");
  passed &= Require(
      !cycle_result.accepted &&
          HasIssue(cycle_result, "SBLR.PLAN_TREE.INVALID_HANDLE", "cycle"),
      "cyclic physical DAG was accepted");
  passed &= Require(
      !orphan_result.accepted &&
          HasIssue(orphan_result, "SBLR.PLAN_TREE.INVALID_HANDLE",
                   "orphan_physical_node"),
      "orphan physical node was accepted");
  return passed;
}

bool ValidateResourceRefusal() {
  auto depth = SharedDag();
  exec::PhysicalNodeAbiLimits shallow;
  shallow.maximum_depth = 2;
  const auto depth_result =
      exec::ValidateTypedPhysicalNodeDag(depth, shallow);

  auto fanout = SharedDag();
  exec::PhysicalNodeAbiLimits narrow;
  narrow.maximum_fanout = 1;
  const auto fanout_result =
      exec::ValidateTypedPhysicalNodeDag(fanout, narrow);

  bool passed = true;
  passed &= Require(
      !depth_result.accepted &&
          HasIssue(depth_result, "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "maximum_depth"),
      "physical DAG depth limit was ignored");
  passed &= Require(
      !fanout_result.accepted &&
          HasIssue(fanout_result, "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "input_physical_node_ids"),
      "physical DAG fanout limit was ignored");
  return passed;
}

}  // namespace

// QOW-TEST-QRY-004-NODE-ABI-V1
int main() {
  bool passed = true;
  passed &= ValidateAcceptedAbi();
  passed &= ValidateAdmissionRefusal();
  passed &= ValidateSnapshotVectorAbi();
  passed &= ValidateHandleAndImplementationRefusal();
  passed &= ValidateGraphRefusal();
  passed &= ValidateResourceRefusal();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
