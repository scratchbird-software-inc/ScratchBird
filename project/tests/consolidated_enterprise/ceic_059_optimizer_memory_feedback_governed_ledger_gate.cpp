// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-059 focused validation for governed optimizer memory feedback.
#include "optimizer_memory_feedback_bridge.hpp"
#include "optimizer_memory_spill_feedback_enterprise.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace opt = scratchbird::engine::optimizer;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 std::string_view token) {
  for (const auto& row : evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

opt::OptimizerMemoryFeedbackEvidence BaseEvidence(std::string source_kind) {
  opt::OptimizerMemoryFeedbackEvidence evidence;
  evidence.query_uuid = "query-ceic059-governed-ledger";
  evidence.scope_uuid = "scope-ceic059-governed-ledger";
  evidence.route_kind = "sql_select";
  evidence.route_label = "embedded/ceic059/hash-aggregate";
  evidence.operator_family = "hash_aggregate";
  evidence.plan_shape = "aggregate(hash)->spill";
  evidence.plan_node_id = "plan-node-ceic059-hash-aggregate";
  evidence.source_kind = std::move(source_kind);
  evidence.trust_provenance = evidence.source_kind;
  evidence.trusted_provenance = true;
  evidence.provenance_digest = "sha256:ceic059-provenance-" + evidence.source_kind;
  evidence.redaction_class = "operational_redacted";
  evidence.redaction_digest = "sha256:ceic059-redaction-digest";
  evidence.metric_snapshot_digest = "sha256:ceic059-memory-metric-snapshot";
  evidence.support_snapshot_digest = "sha256:ceic059-support-snapshot";
  evidence.reservation_id = "reservation-ceic059-operator";
  evidence.reservation_token = "reservation-token-ceic059-operator";
  evidence.reservation_generation = 5901;
  evidence.policy_generation = 5902;
  evidence.feedback_generation = 5903;
  evidence.catalog_epoch = 5904;
  evidence.security_epoch = 5905;
  evidence.redaction_epoch = 5906;
  evidence.statistics_epoch = 5907;
  evidence.observed_timestamp_ticks = 1000000;
  evidence.received_timestamp_ticks = 1000500;
  evidence.max_age_ticks = 60000000;
  evidence.memory_grant_bytes = 512 * 1024;
  evidence.peak_memory_bytes = 2 * 1024 * 1024;
  evidence.spill_bytes = 8 * 1024 * 1024;
  evidence.spill_passes = 2;
  evidence.allocation_failure_count = 1;
  evidence.governed_reservation = true;
  evidence.reservation_token_bound = true;
  evidence.resource_governance_ledger_recorded =
      evidence.source_kind == "resource_governance_reservation_ledger";
  evidence.bounded_support_bundle =
      evidence.source_kind == "memory_support_bundle_metric_snapshot";
  evidence.support_bundle_redacted =
      evidence.source_kind == "memory_support_bundle_metric_snapshot";
  evidence.support_bundle_fresh =
      evidence.source_kind == "memory_support_bundle_metric_snapshot";
  evidence.real_operation_metric =
      evidence.source_kind == "real_operation_memory_metrics";
  evidence.operation_metric_runtime_path =
      evidence.source_kind == "real_operation_memory_metrics";
  return evidence;
}

opt::EnterpriseMemorySpillFeedbackApplyRequest SpillRequest(
    std::string uuid,
    opt::OptimizerMemoryFeedbackEvidence evidence = BaseEvidence(
        "resource_governance_reservation_ledger")) {
  opt::EnterpriseMemorySpillFeedbackApplyRequest request;
  request.feedback_uuid = std::move(uuid);
  request.reservation_id = evidence.reservation_id;
  request.memory_snapshot_digest = evidence.metric_snapshot_digest;
  request.route_label = evidence.route_label;
  request.plan_node_id = evidence.plan_node_id;
  request.policy_generation = evidence.policy_generation;
  request.feedback_generation = evidence.feedback_generation;
  request.catalog_epoch = evidence.catalog_epoch;
  request.security_epoch = evidence.security_epoch;
  request.created_microseconds = 2000000;
  request.expires_after_microseconds = 1000000;
  request.baseline_cost.startup_cost = 100;
  request.baseline_cost.row_cost = 100;
  request.baseline_cost.io_cost = 100;
  request.baseline_cost.memory_cost = 100;
  request.baseline_cost.total_cost = 400;
  request.baseline_cost.selectable = true;
  request.baseline_cost.confidence = opt::CostConfidence::kMedium;
  request.evidence = std::move(evidence);
  return request;
}

void ExpectAccepted(const opt::OptimizerMemoryFeedbackEvidence& evidence,
                    std::string_view message) {
  const auto result = opt::BuildOptimizerMemoryFeedbackForPlanner(evidence);
  Require(result.ok(), message);
  Require(result.ceic_059_contract_accepted,
          "CEIC-059 accepted result did not mark contract accepted");
  Require(result.authority_boundaries_clean,
          "CEIC-059 accepted result did not mark authority boundaries clean");
  Require(HasEvidence(result.evidence, "CEIC_059_OPTIMIZER_MEMORY_FEEDBACK"),
          "CEIC-059 accepted evidence missing anchor");
  Require(result.runtime_feedback.advisory_only,
          "CEIC-059 feedback must remain advisory");
  Require(result.runtime_feedback.transaction_finality_authority ==
              "engine_transaction_inventory",
          "CEIC-059 feedback drifted transaction finality authority");
}

void ExpectRejected(opt::OptimizerMemoryFeedbackEvidence evidence,
                    std::string_view diagnostic_code,
                    std::string_view message) {
  const auto result = opt::BuildOptimizerMemoryFeedbackForPlanner(evidence);
  Require(!result.ok(), message);
  Require(result.fail_closed, "CEIC-059 rejection did not fail closed");
  Require(result.diagnostic_code == diagnostic_code,
          "CEIC-059 rejection diagnostic mismatch");
}

void AcceptedSourceKinds() {
  ExpectAccepted(BaseEvidence("resource_governance_reservation_ledger"),
                 "CEIC-059 rejected governed reservation ledger feedback");
  ExpectAccepted(BaseEvidence("memory_support_bundle_metric_snapshot"),
                 "CEIC-059 rejected redacted support-bundle snapshot feedback");
  ExpectAccepted(BaseEvidence("real_operation_memory_metrics"),
                 "CEIC-059 rejected real operation metrics with reservation proof");
}

void SourceFreshnessDigestAndGenerationRefusals() {
  auto generic = BaseEvidence("resource_governance_reservation_ledger");
  generic.source_kind = "observed_runtime";
  generic.trust_provenance = "observed_runtime";
  ExpectRejected(generic, "SB_OPTIMIZER_MEMORY_FEEDBACK.UNTRUSTED_SOURCE",
                 "CEIC-059 accepted generic observed_runtime evidence");

  auto stale = BaseEvidence("resource_governance_reservation_ledger");
  stale.received_timestamp_ticks =
      stale.observed_timestamp_ticks + stale.max_age_ticks + 1;
  ExpectRejected(stale, "SB_OPTIMIZER_MEMORY_FEEDBACK.STALE",
                 "CEIC-059 accepted stale memory feedback");

  auto future = BaseEvidence("resource_governance_reservation_ledger");
  future.received_timestamp_ticks = future.observed_timestamp_ticks - 1;
  ExpectRejected(future, "SB_OPTIMIZER_MEMORY_FEEDBACK.FUTURE",
                 "CEIC-059 accepted future-dated memory feedback");

  auto unbounded = BaseEvidence("resource_governance_reservation_ledger");
  unbounded.max_age_ticks = 0;
  ExpectRejected(unbounded, "SB_OPTIMIZER_MEMORY_FEEDBACK.UNBOUNDED_FRESHNESS",
                 "CEIC-059 accepted unbounded freshness");

  auto placeholder = BaseEvidence("resource_governance_reservation_ledger");
  placeholder.redaction_digest = "placeholder";
  ExpectRejected(placeholder, "SB_OPTIMIZER_MEMORY_FEEDBACK.PLACEHOLDER_DIGEST",
                 "CEIC-059 accepted placeholder redaction digest");

  auto missing_generation = BaseEvidence("resource_governance_reservation_ledger");
  missing_generation.catalog_epoch = 0;
  ExpectRejected(missing_generation,
                 "SB_OPTIMIZER_MEMORY_FEEDBACK.MISSING_GENERATION",
                 "CEIC-059 accepted missing catalog epoch");
}

void SyntheticRedactionReservationAndSupportBundleRefusals() {
  auto synthetic = BaseEvidence("resource_governance_reservation_ledger");
  synthetic.synthetic = true;
  ExpectRejected(synthetic, "SB_OPTIMIZER_MEMORY_FEEDBACK.SYNTHETIC",
                 "CEIC-059 accepted synthetic evidence");

  auto local_default = BaseEvidence("resource_governance_reservation_ledger");
  local_default.local_default_evidence = true;
  ExpectRejected(local_default, "SB_OPTIMIZER_MEMORY_FEEDBACK.SYNTHETIC",
                 "CEIC-059 accepted local-default evidence");

  auto unredacted = BaseEvidence("resource_governance_reservation_ledger");
  unredacted.protected_material_redacted = false;
  ExpectRejected(unredacted, "SB_OPTIMIZER_MEMORY_FEEDBACK.REDACTION_REQUIRED",
                 "CEIC-059 accepted unredacted protected material");

  auto exposed = BaseEvidence("resource_governance_reservation_ledger");
  exposed.protected_material_exposed = true;
  ExpectRejected(exposed, "SB_OPTIMIZER_MEMORY_FEEDBACK.REDACTION_REQUIRED",
                 "CEIC-059 accepted protected material exposure");

  auto missing_reservation = BaseEvidence("resource_governance_reservation_ledger");
  missing_reservation.reservation_id.clear();
  ExpectRejected(missing_reservation, "SB_OPTIMIZER_MEMORY_FEEDBACK.UNGOVERNED",
                 "CEIC-059 accepted missing reservation id");

  auto support = BaseEvidence("memory_support_bundle_metric_snapshot");
  support.bounded_support_bundle = false;
  ExpectRejected(
      support, "SB_OPTIMIZER_MEMORY_FEEDBACK.SUPPORT_BUNDLE_PROOF_REQUIRED",
      "CEIC-059 accepted support-bundle evidence without bounded proof");
}

using Mutator = void (*)(opt::OptimizerMemoryFeedbackEvidence*);

void ExpectAuthorityRejected(std::string_view label, Mutator mutate) {
  auto evidence = BaseEvidence("resource_governance_reservation_ledger");
  mutate(&evidence);
  const auto result = opt::BuildOptimizerMemoryFeedbackForPlanner(evidence);
  Require(!result.ok() &&
              result.diagnostic_code ==
                  "SB_OPTIMIZER_MEMORY_FEEDBACK.UNSAFE_AUTHORITY",
          std::string("CEIC-059 accepted authority drift: ") +
              std::string(label));
}

void AuthorityDriftRefusals() {
  const std::vector<std::pair<std::string_view, Mutator>> cases = {
      {"benchmark", [](auto* e) { e->benchmark_authority = true; }},
      {"parser", [](auto* e) { e->parser_authority = true; }},
      {"client", [](auto* e) { e->client_authority = true; }},
      {"reference", [](auto* e) { e->reference_authority = true; }},
      {"combined_parser_client_reference",
       [](auto* e) { e->parser_client_or_reference_authority = true; }},
      {"wal", [](auto* e) { e->wal_authority = true; }},
      {"recovery", [](auto* e) { e->recovery_authority = true; }},
      {"combined_recovery_wal",
       [](auto* e) { e->recovery_or_wal_authority = true; }},
      {"transaction_finality",
       [](auto* e) { e->transaction_finality_authority = true; }},
      {"visibility", [](auto* e) { e->visibility_authority = true; }},
      {"security",
       [](auto* e) { e->authorization_security_authority = true; }},
      {"optimizer_plan",
       [](auto* e) { e->optimizer_plan_authority = true; }},
      {"index_finality",
       [](auto* e) { e->index_finality_authority = true; }},
      {"provider_finality",
       [](auto* e) { e->provider_finality_authority = true; }},
      {"local_cluster", [](auto* e) { e->local_cluster_authority = true; }},
      {"cluster", [](auto* e) { e->cluster_authority = true; }},
      {"agent", [](auto* e) { e->agent_action_authority = true; }},
      {"not_advisory", [](auto* e) { e->advisory_only = false; }},
      {"mga_recheck_missing",
       [](auto* e) { e->mga_visibility_recheck_preserved = false; }},
      {"security_recheck_missing",
       [](auto* e) { e->security_recheck_preserved = false; }},
  };
  for (const auto& [label, mutate] : cases) {
    ExpectAuthorityRejected(label, mutate);
  }
}

void SpillStoreInvalidatesAndExpires() {
  opt::EnterpriseMemorySpillFeedbackStore store;
  const auto accepted =
      opt::ApplyEnterpriseMemorySpillFeedback(SpillRequest("ceic059-spill-1"),
                                              &store);
  Require(accepted.accepted, "CEIC-059 spill feedback was rejected");
  Require(accepted.benchmark_clean,
          "CEIC-059 spill feedback did not mark benchmark clean after clean bridge");
  auto found = store.Find("ceic059-spill-1");
  Require(found.has_value() && found->valid,
          "CEIC-059 spill feedback record was not persisted");
  Require(HasEvidence(found->evidence, "enterprise_memory_spill_feedback.redaction_digest="),
          "CEIC-059 spill record missing redaction evidence");
  Require(HasEvidence(found->evidence, "enterprise_memory_spill_feedback.provenance_digest="),
          "CEIC-059 spill record missing provenance evidence");
  Require(HasEvidence(found->evidence, "enterprise_memory_spill_feedback.reservation_generation=5901"),
          "CEIC-059 spill record missing reservation generation evidence");

  const auto expired = store.Expire(3000000);
  found = store.Find("ceic059-spill-1");
  Require(expired == 1 && found.has_value() && !found->valid,
          "CEIC-059 spill feedback did not expire");

  const auto accepted_again =
      opt::ApplyEnterpriseMemorySpillFeedback(SpillRequest("ceic059-spill-2"),
                                              &store);
  Require(accepted_again.accepted, "CEIC-059 second spill feedback setup failed");
  opt::EnterpriseMemorySpillFeedbackInvalidation event;
  event.scope_uuid = "scope-ceic059-governed-ledger";
  event.security_epoch = 9900;
  event.reason = "security_epoch_changed";
  const auto invalidated = store.Invalidate(event);
  found = store.Find("ceic059-spill-2");
  Require(invalidated == 1 && found.has_value() && !found->valid &&
              found->invalidation_reason == "security_epoch_changed",
          "CEIC-059 spill feedback did not invalidate on epoch change");

  auto rejected_request = SpillRequest("ceic059-spill-rejected");
  rejected_request.evidence.benchmark_authority = true;
  const auto rejected =
      opt::ApplyEnterpriseMemorySpillFeedback(rejected_request, &store);
  Require(!rejected.accepted && !rejected.benchmark_clean && rejected.fail_closed,
          "CEIC-059 unsafe spill feedback was not fail-closed");
  Require(store.Find("ceic059-spill-rejected") == std::nullopt,
          "CEIC-059 rejected spill feedback was persisted");
}

}  // namespace

int main() {
  AcceptedSourceKinds();
  SourceFreshnessDigestAndGenerationRefusals();
  SyntheticRedactionReservationAndSupportBundleRefusals();
  AuthorityDriftRefusals();
  SpillStoreInvalidatesAndExpires();
  return EXIT_SUCCESS;
}
