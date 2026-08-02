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
#include <vector>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-004-ACCESS-V1: " << detail << '\n';
  }
  return condition;
}

std::string Uuid(const std::uint64_t suffix) {
  auto text = std::string("019f0000-0000-7400-8000-000000000000");
  const auto digits = std::to_string(suffix);
  text.replace(text.size() - digits.size(), digits.size(), digits);
  return text;
}

exec::CanonicalScanAccessRequest Request(const bool index_scan = true) {
  exec::CanonicalScanAccessRequest request;
  request.physical_dag.abi_version = 2;
  request.physical_dag.selected_plan_uuid = Uuid(401);
  request.physical_dag.root_physical_node_id = 41;
  request.physical_dag.local_transaction_id = 401;
  request.physical_dag.statement_snapshot_id = 402;
  request.physical_dag.mga_statement_context = {
      Uuid(440), Uuid(441), Uuid(442), Uuid(443), 401, 402, 401, 401,
      401, 401, {401}, {}, "statement_stable", 403, true, true, true};
  request.physical_dag.bound_sblr_tree_uuid = Uuid(451);
  request.physical_dag.catalog_epoch_uuid = Uuid(452);
  request.physical_dag.security_context_uuid = Uuid(453);
  request.physical_dag.capability_snapshot_uuid = Uuid(455);
  request.physical_dag.resource_snapshot_uuid = Uuid(456);
  request.physical_dag.statistics_snapshot_uuid = Uuid(457);
  request.physical_dag.route_snapshot_uuid = Uuid(458);
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       request.physical_dag.bound_sblr_tree_uuid},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       request.physical_dag.catalog_epoch_uuid},
      {exec::PhysicalAdmissionStage::kSecurity,
       request.physical_dag.security_context_uuid},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       request.physical_dag.mga_statement_context.statement_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       request.physical_dag.capability_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kResource,
       request.physical_dag.resource_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       request.physical_dag.statistics_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       request.physical_dag.route_snapshot_uuid},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 41,
       .relational_node_id = 41,
       .node_kind = exec::PhysicalNodeKind::kScan,
       .implementation_id = index_scan ? "scan.index.v1" : "scan.heap.v1",
       .output_descriptor_ids = {411},
       .causal_counter_id = 4101,
       .selected_alternative_uuid = Uuid(461),
       .executor_capability_uuid = Uuid(462),
       .executor_capability_abi_version = 1,
       .cost_vector_uuid = Uuid(463),
       .memory_bytes_required = 1024,
       .engine_capability_validated = true,
       .mga_statement_context = request.physical_dag.mga_statement_context},
  };
  request.physical_dag.catalog_generation = 1;
  request.physical_dag.security_epoch = 1;
  request.physical_dag.policy_epoch = 1;
  request.physical_dag.resource_epoch = 1;
  request.physical_dag.statistics_generation = 1;
  request.physical_dag.route_epoch = 1;
  request.physical_dag.route_generation = 1;
  request.physical_dag.memory_budget_bytes = 1 << 20;
  request.physical_dag.optimizer_published = true;
  request.physical_dag.immutable_node_identity_validated = true;
  request.physical_dag.capability_validated_before_access = true;
  request.selected_physical_node_id = 41;
  request.available_implementation_id =
      request.physical_dag.nodes.front().implementation_id;
  request.relation_uuid = Uuid(430);
  request.inventory_local_transaction_id = 401;
  request.inventory_statement_snapshot_id = 402;
  request.selected_descriptor_generation = 7;
  request.current_descriptor_generation = 7;
  return request;
}

exec::CanonicalScanCandidateEvidence Candidate(
    const std::uint64_t ordinal,
    const exec::CanonicalMgaVisibilityDecision visibility,
    const exec::CanonicalMgaSecurityDecision security,
    const api::EngineSqlTruthValue residual,
    const bool locator_matches = true) {
  exec::CanonicalScanCandidateEvidence candidate;
  candidate.candidate_uuid = Uuid(500 + ordinal);
  candidate.record_uuid = Uuid(600 + ordinal);
  candidate.relation_uuid = Uuid(430);
  candidate.visibility_decision_uuid = Uuid(700 + ordinal);
  candidate.row_version_id = 800 + ordinal;
  candidate.candidate_generation = 9;
  candidate.observed_generation = locator_matches ? 9 : 10;
  candidate.source = exec::CanonicalScanCandidateSource::kIndexEntry;
  candidate.visibility = visibility;
  candidate.security_decision = security;
  candidate.residual_truth = residual;
  candidate.locator_identity_matches = locator_matches;
  return candidate;
}

// QOW-TEST-QRY-004-ACCESS-V1
bool ValidateIndexAccessRechecks() {
  auto request = Request();
  request.candidates = {
      Candidate(1, exec::CanonicalMgaVisibilityDecision::kVisible,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::true_value),
      Candidate(2, exec::CanonicalMgaVisibilityDecision::kInvisible,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::true_value),
      Candidate(3, exec::CanonicalMgaVisibilityDecision::kVisible,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::true_value, false),
      Candidate(4, exec::CanonicalMgaVisibilityDecision::kVisible,
                exec::CanonicalMgaSecurityDecision::kDenied,
                api::EngineSqlTruthValue::true_value),
      Candidate(5, exec::CanonicalMgaVisibilityDecision::kVisible,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::false_value),
      Candidate(6, exec::CanonicalMgaVisibilityDecision::kVisible,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::unknown),
  };

  const auto result = exec::ExecuteCanonicalSelectedScanAccess(request);
  bool passed = true;
  passed &= Require(result.diagnostic.ok && !result.replan_required,
                    "valid selected index access was refused");
  passed &= Require(result.accepted_record_uuids ==
                            std::vector<std::string>{Uuid(601)} &&
                        result.accepted_row_version_ids ==
                            std::vector<std::uint64_t>{801},
                    "index access published a candidate before all rechecks");
  passed &= Require(result.counters.candidate_count == 6 &&
                        result.counters.visibility_recheck_count == 6 &&
                        result.counters.invisible_filtered_count == 1 &&
                        result.counters.stale_index_filtered_count == 1 &&
                        result.counters.security_filtered_count == 1 &&
                        result.counters.residual_filtered_count == 2 &&
                        result.counters.emitted_count == 1,
                    "scan causal counters do not match recheck outcomes");
  passed &= Require(result.selected_plan_uuid == Uuid(401) &&
                        result.executed_physical_node_id == 41 &&
                        result.causal_counter_id == 4101,
                    "selected plan/node/causal identity was not retained");
  passed &= Require(result.authority.engine_mga_snapshot_bound &&
                        result.authority.visibility_rechecks_complete &&
                        !result.authority.owns_transaction_finality &&
                        !result.authority.owns_recovery &&
                        !result.authority.owns_parser_execution &&
                        !result.authority.index_or_cache_is_visibility_authority &&
                        !result.authority.wal_is_visibility_or_recovery_authority,
                    "scan access acquired forbidden authority");
  return passed;
}

bool ValidateRelationAccessOrder() {
  auto request = Request(false);
  auto first = Candidate(
      11, exec::CanonicalMgaVisibilityDecision::kVisible,
      exec::CanonicalMgaSecurityDecision::kAllowed,
      api::EngineSqlTruthValue::true_value);
  auto second = Candidate(
      12, exec::CanonicalMgaVisibilityDecision::kVisible,
      exec::CanonicalMgaSecurityDecision::kAllowed,
      api::EngineSqlTruthValue::true_value);
  first.source = exec::CanonicalScanCandidateSource::kRelationPage;
  second.source = exec::CanonicalScanCandidateSource::kRelationPage;
  request.candidates = {first, second};

  const auto result = exec::ExecuteCanonicalSelectedScanAccess(request);
  return Require(result.diagnostic.ok &&
                     result.accepted_row_version_ids ==
                         std::vector<std::uint64_t>{811, 812} &&
                     result.counters.emitted_count == 2,
                 "relation scan did not preserve candidate order");
}

bool ValidateAuthorityAndReplanRefusals() {
  bool passed = true;
  auto request = Request();
  request.candidates = {
      Candidate(21, exec::CanonicalMgaVisibilityDecision::kVisible,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::true_value),
  };
  request.inventory_statement_snapshot_id = 999;
  auto result = exec::ExecuteCanonicalSelectedScanAccess(request);
  passed &= Require(!result.diagnostic.ok &&
                        result.diagnostic.diagnostic_code ==
                            "SB_DIAG_MGA_READ_SNAPSHOT_MISSING" &&
                        result.accepted_record_uuids.empty() &&
                        result.executed_physical_node_id == 0,
                    "mismatched MGA snapshot produced scan output");

  request = Request();
  request.current_descriptor_generation = 8;
  result = exec::ExecuteCanonicalSelectedScanAccess(request);
  passed &= Require(!result.diagnostic.ok && result.replan_required &&
                        result.diagnostic.diagnostic_code ==
                            "SB_DIAG_MGA_READ_INDEX_DESCRIPTOR_INVALID",
                    "stale selected index generation did not require replan");

  request = Request();
  request.available_implementation_id = "scan.heap.v1";
  result = exec::ExecuteCanonicalSelectedScanAccess(request);
  passed &= Require(!result.diagnostic.ok && result.replan_required &&
                        result.diagnostic.diagnostic_code ==
                            "QOW-DIAG-QRY-004-SCAN-IMPLEMENTATION-UNAVAILABLE-V1",
                    "unavailable selected implementation silently defaulted");

  request = Request();
  request.physical_dag.mga_statement_context.current = false;
  result = exec::ExecuteCanonicalSelectedScanAccess(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-PHYSICAL-NODE-ABI-PUBLICATION",
      "non-current physical MGA vector reached selected scan access");
  return passed;
}

bool ValidateAllOrNothingRefusal() {
  auto request = Request();
  request.candidates = {
      Candidate(31, exec::CanonicalMgaVisibilityDecision::kVisible,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::true_value),
      Candidate(32, exec::CanonicalMgaVisibilityDecision::kIndeterminate,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::true_value),
  };
  auto result = exec::ExecuteCanonicalSelectedScanAccess(request);
  bool passed = true;
  passed &= Require(!result.diagnostic.ok &&
                        result.diagnostic.diagnostic_code ==
                            "SB_DIAG_MGA_READ_VISIBILITY_DECISION_INVALID" &&
                        result.accepted_record_uuids.empty() &&
                        result.accepted_row_version_ids.empty() &&
                        result.counters.emitted_count == 0,
                    "indeterminate visibility leaked partial scan output");

  request = Request();
  request.maximum_candidate_count = 1;
  request.candidates = {
      Candidate(33, exec::CanonicalMgaVisibilityDecision::kVisible,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::true_value),
      Candidate(34, exec::CanonicalMgaVisibilityDecision::kVisible,
                exec::CanonicalMgaSecurityDecision::kAllowed,
                api::EngineSqlTruthValue::true_value),
  };
  result = exec::ExecuteCanonicalSelectedScanAccess(request);
  passed &= Require(!result.diagnostic.ok &&
                        result.diagnostic.diagnostic_code ==
                            "SBLR.PLAN_TREE.RESOURCE_LIMIT" &&
                        result.accepted_record_uuids.empty(),
                    "scan candidate resource bound was ignored");
  return passed;
}

}  // namespace

int main() {
  if (!ValidateIndexAccessRechecks() || !ValidateRelationAccessOrder() ||
      !ValidateAuthorityAndReplanRefusals() ||
      !ValidateAllOrNothingRefusal()) {
    return 1;
  }
  std::cout << "QOW-TEST-QRY-004-ACCESS-V1: PASS\n";
  return 0;
}
