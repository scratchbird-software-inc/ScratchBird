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
  dag.selected_plan_uuid = "019f0000-0000-7000-8000-000000000401";
  dag.root_physical_node_id = 4;
  dag.local_transaction_id = 41;
  dag.statement_snapshot_id = 43;
  for (std::uint8_t stage = 1; stage <= 8; ++stage) {
    std::string uuid = "019f0000-0000-7100-8000-00000000040";
    uuid.push_back(static_cast<char>('1' + stage));
    dag.admission_evidence.push_back(
        {static_cast<exec::PhysicalAdmissionStage>(stage), std::move(uuid)});
  }
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
  orphan.nodes.push_back(
      {5, 5, exec::PhysicalNodeKind::kScan, "scan.heap.v1", {}, {4}, false,
       105});
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
  passed &= ValidateHandleAndImplementationRefusal();
  passed &= ValidateGraphRefusal();
  passed &= ValidateResourceRefusal();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
