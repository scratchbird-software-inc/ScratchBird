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
    std::cerr << "QOW-TEST-QRY-007-SCAN-V1: " << detail << '\n';
  }
  return condition;
}

std::string Uuid(const std::uint64_t suffix) {
  auto text = std::string("019f0000-0000-7710-8000-000000000000");
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

exec::TypedPhysicalNodeDag Dag() {
  exec::TypedPhysicalNodeDag dag;
  dag.abi_version = 2;
  dag.selected_plan_uuid = Uuid(1);
  dag.root_physical_node_id = 72;
  dag.local_transaction_id = kOwner;
  dag.statement_snapshot_id = 0;
  dag.mga_statement_context = {
      Uuid(2), Uuid(3), Uuid(4), Uuid(5), kOwner, 0,
      kOldestActive, kHorizon, kHorizon, kHorizon,
      {kOldestActive, kOwner}, {kInDoubt}, "statement_stable",
      kInventoryNext, true, true, true};
  dag.bound_sblr_tree_uuid = Uuid(6);
  dag.catalog_epoch_uuid = Uuid(7);
  dag.security_context_uuid = Uuid(8);
  dag.capability_snapshot_uuid = Uuid(9);
  dag.resource_snapshot_uuid = Uuid(10);
  dag.statistics_snapshot_uuid = Uuid(11);
  dag.route_snapshot_uuid = Uuid(12);
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
  dag.nodes = {
      {.physical_node_id = 71,
       .relational_node_id = 71,
       .node_kind = exec::PhysicalNodeKind::kScan,
       .implementation_id = "scan.heap.v1",
       .output_descriptor_ids = {711},
       .causal_counter_id = 7101},
      {.physical_node_id = 72,
       .relational_node_id = 72,
       .node_kind = exec::PhysicalNodeKind::kProject,
       .implementation_id = "project.typed.row.v1",
       .input_physical_node_ids = {71},
       .output_descriptor_ids = {711},
       .causal_counter_id = 7201},
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
  for (std::size_t index = 0; index < dag.nodes.size(); ++index) {
    auto& node = dag.nodes[index];
    node.selected_alternative_uuid = Uuid(20 + index * 3);
    node.executor_capability_uuid = Uuid(21 + index * 3);
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid = Uuid(22 + index * 3);
    node.memory_bytes_required = 1024;
    node.engine_capability_validated = true;
    node.mga_statement_context = dag.mga_statement_context;
  }
  return dag;
}

exec::CanonicalScanCandidateEvidence Candidate(
    const std::uint64_t seed,
    const std::string& relation_uuid,
    const exec::CanonicalMgaVisibilityDecision visibility) {
  exec::CanonicalScanCandidateEvidence candidate;
  candidate.candidate_uuid = Uuid(seed);
  candidate.record_uuid = Uuid(seed + 1);
  candidate.relation_uuid = relation_uuid;
  candidate.visibility_decision_uuid = Uuid(seed + 2);
  candidate.creator_local_transaction_id = kOwner;
  candidate.row_version_id = seed + 3;
  candidate.candidate_generation = 4;
  candidate.observed_generation = 4;
  candidate.source = exec::CanonicalScanCandidateSource::kRelationPage;
  candidate.visibility = visibility;
  candidate.security_decision = exec::CanonicalMgaSecurityDecision::kAllowed;
  candidate.residual_truth = api::EngineSqlTruthValue::true_value;
  candidate.locator_identity_matches = true;
  return candidate;
}

exec::CanonicalScanAccessRequest Request() {
  exec::CanonicalScanAccessRequest request;
  request.physical_dag = Dag();
  request.selected_physical_node_id = 71;
  request.available_implementation_id = "scan.heap.v1";
  request.relation_uuid = Uuid(40);
  request.mga_authority = ClosureAuthority(request.physical_dag);
  request.selected_descriptor_generation = 9;
  request.current_descriptor_generation = 9;
  request.maximum_candidate_count = 2;
  request.candidates = {
      Candidate(50, request.relation_uuid,
                exec::CanonicalMgaVisibilityDecision::kVisible),
      Candidate(60, request.relation_uuid,
                exec::CanonicalMgaVisibilityDecision::kInvisible),
  };
  return request;
}

// QOW-TEST-QRY-007-SCAN-V1
bool ValidateReachableTypedScan() {
  bool passed = true;
  const auto request = Request();
  const auto result = exec::ExecuteCanonicalSelectedScanAccess(request);
  passed &= Require(result.diagnostic.ok && !result.replan_required,
                    "reachable non-root scan node was refused");
  passed &= Require(
      result.executed_physical_node_id == 71 &&
          result.causal_counter_id == 7101 &&
          result.selected_plan_uuid == request.physical_dag.selected_plan_uuid &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              request.physical_dag.mga_statement_context),
      "scan lost selected plan, node, counter, or MGA identity");
  passed &= Require(
      result.accepted_record_uuids == std::vector<std::string>{Uuid(51)} &&
          result.accepted_row_version_ids == std::vector<std::uint64_t>{53} &&
          result.counters.candidate_count == 2 &&
          result.counters.visibility_recheck_count == 2 &&
          result.counters.invisible_filtered_count == 1 &&
          result.counters.emitted_count == 1,
      "scan did not preserve candidate visibility and emission counters");

  auto stale = Request();
  stale.current_descriptor_generation = 10;
  const auto stale_result =
      exec::ExecuteCanonicalSelectedScanAccess(stale);
  passed &= Require(
      !stale_result.diagnostic.ok && stale_result.replan_required &&
          stale_result.accepted_record_uuids.empty() &&
          stale_result.accepted_row_version_ids.empty(),
      "descriptor drift did not atomically request replanning");

  auto input_scan = Request();
  input_scan.physical_dag.nodes.front().input_physical_node_ids = {72};
  const auto input_result =
      exec::ExecuteCanonicalSelectedScanAccess(input_scan);
  passed &= Require(!input_result.diagnostic.ok &&
                        input_result.accepted_record_uuids.empty(),
                    "non-leaf scan produced rows");
  return passed;
}

}  // namespace

int main() {
  if (!ValidateReachableTypedScan()) return 1;
  std::cout << "QOW-TEST-QRY-007-SCAN-V1: PASS\n";
  return 0;
}
