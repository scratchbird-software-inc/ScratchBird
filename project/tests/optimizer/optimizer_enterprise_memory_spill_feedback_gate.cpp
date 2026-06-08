// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_memory_spill_feedback_enterprise.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace opt = scratchbird::engine::optimizer;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OEIC memory spill feedback gate failure: " << message << '\n';
    return false;
  }
  return true;
}

opt::OptimizerMemoryFeedbackEvidence MemoryEvidence() {
  // SEARCH_KEY: OEIC_MEMORY_SPILL_FEEDBACK_ENTERPRISE
  opt::OptimizerMemoryFeedbackEvidence evidence;
  evidence.query_uuid = "query.memory.feedback";
  evidence.scope_uuid = "scope.memory.feedback";
  evidence.route_kind = "sql_select";
  evidence.route_label = "embedded:select:aggregate";
  evidence.operator_family = "hash_aggregate";
  evidence.plan_shape = "aggregate:grouped";
  evidence.plan_node_id = "plan-node:aggregate";
  evidence.source_quality = "observed_runtime";
  evidence.source_kind = "resource_governance_reservation_ledger";
  evidence.trust_provenance = "resource_governance_reservation_ledger";
  evidence.trusted_provenance = true;
  evidence.provenance_digest = "sha256:memory-feedback-provenance";
  evidence.redaction_class = "operational";
  evidence.redaction_digest = "sha256:memory-feedback-redaction";
  evidence.metric_snapshot_digest = "sha256:memory-feedback-metric-snapshot";
  evidence.reservation_id = "reservation.memory.feedback";
  evidence.reservation_token = "reservation-token.memory.feedback";
  evidence.reservation_generation = 12;
  evidence.policy_generation = 10;
  evidence.feedback_generation = 20;
  evidence.catalog_epoch = 30;
  evidence.security_epoch = 40;
  evidence.redaction_epoch = 50;
  evidence.statistics_epoch = 60;
  evidence.observed_timestamp_ticks = 1000;
  evidence.received_timestamp_ticks = 1500;
  evidence.max_age_ticks = 1000000;
  evidence.memory_grant_bytes = 512 * 1024;
  evidence.peak_memory_bytes = 2 * 1024 * 1024;
  evidence.spill_bytes = 8 * 1024 * 1024;
  evidence.spill_passes = 3;
  evidence.allocation_failure_count = 1;
  evidence.governed_reservation = true;
  evidence.reservation_token_bound = true;
  evidence.resource_governance_ledger_recorded = true;
  evidence.protected_material_redacted = true;
  evidence.advisory_only = true;
  evidence.mga_visibility_recheck_preserved = true;
  evidence.security_recheck_preserved = true;
  return evidence;
}

opt::EnterpriseMemorySpillFeedbackApplyRequest ApplyRequest(std::string uuid) {
  opt::EnterpriseMemorySpillFeedbackApplyRequest request;
  request.evidence = MemoryEvidence();
  request.feedback_uuid = std::move(uuid);
  request.reservation_id = request.evidence.reservation_id;
  request.memory_snapshot_digest = request.evidence.metric_snapshot_digest;
  request.route_label = "embedded:select:aggregate";
  request.plan_node_id = "plan-node:aggregate";
  request.policy_generation = 10;
  request.feedback_generation = 20;
  request.catalog_epoch = 30;
  request.security_epoch = 40;
  request.created_microseconds = 1000000;
  request.expires_after_microseconds = 5000000;
  request.baseline_cost.startup_cost = 100;
  request.baseline_cost.row_cost = 100;
  request.baseline_cost.io_cost = 100;
  request.baseline_cost.memory_cost = 100;
  request.baseline_cost.total_cost = 400;
  request.baseline_cost.selectable = true;
  request.baseline_cost.confidence = opt::CostConfidence::kMedium;
  return request;
}

bool MemorySpillFeedbackRecordsAndAdjustsCost() {
  opt::EnterpriseMemorySpillFeedbackStore store;
  const auto result = opt::ApplyEnterpriseMemorySpillFeedback(
      ApplyRequest("memory.feedback.1"), &store);
  const auto found = store.Find("memory.feedback.1");
  const auto snapshot = store.Snapshot();
  return Require(result.accepted && result.benchmark_clean,
                 "memory spill feedback refused: " + result.diagnostic_code) &&
         Require(found.has_value() && found->valid,
                 "memory spill feedback record missing") &&
         Require(result.feedback_status.memory_grant.apply,
                 "memory grant recommendation was not applied") &&
         Require(result.bridge_result.runtime_feedback.actual_spill_bytes ==
                     8 * 1024 * 1024,
                 "spill bytes were not propagated") &&
         Require(result.adjusted_cost.total_cost != 400,
                 "feedback did not adjust cost") &&
         Require(snapshot.valid_records == 1 && snapshot.spill_records == 1,
                 "memory feedback snapshot counters mismatch");
}

bool MemorySpillFeedbackExpiresAndInvalidates() {
  opt::EnterpriseMemorySpillFeedbackStore store;
  if (!opt::ApplyEnterpriseMemorySpillFeedback(
           ApplyRequest("memory.feedback.2"), &store).accepted) {
    return Require(false, "setup feedback failed");
  }
  const auto expired = store.Expire(7000000);
  const auto after_expire = store.Find("memory.feedback.2");
  if (!Require(expired == 1, "memory feedback did not expire") ||
      !Require(after_expire.has_value() && !after_expire->valid &&
                   after_expire->invalidation_reason == "memory_feedback_age_expired",
               "memory feedback expiry evidence missing")) {
    return false;
  }

  if (!opt::ApplyEnterpriseMemorySpillFeedback(
           ApplyRequest("memory.feedback.3"), &store).accepted) {
    return Require(false, "second setup feedback failed");
  }
  opt::EnterpriseMemorySpillFeedbackInvalidation event;
  event.scope_uuid = "scope.memory.feedback";
  event.security_epoch = 41;
  event.reason = "security_epoch_changed";
  const auto invalidated = store.Invalidate(event);
  const auto after_invalidate = store.Find("memory.feedback.3");
  return Require(invalidated == 1, "memory feedback invalidation did not match") &&
         Require(after_invalidate.has_value() && !after_invalidate->valid &&
                     after_invalidate->invalidation_reason == "security_epoch_changed",
                 "memory feedback invalidation evidence missing");
}

bool MemorySpillFeedbackRejectsUngovernedAndStaleEvidence() {
  opt::EnterpriseMemorySpillFeedbackStore store;
  auto ungoverned = ApplyRequest("memory.feedback.unsafe");
  ungoverned.evidence.governed_reservation = false;
  const auto ungoverned_result =
      opt::ApplyEnterpriseMemorySpillFeedback(ungoverned, &store);

  auto stale = ApplyRequest("memory.feedback.stale");
  stale.evidence.received_timestamp_ticks =
      stale.evidence.observed_timestamp_ticks + stale.evidence.max_age_ticks + 1;
  const auto stale_result = opt::ApplyEnterpriseMemorySpillFeedback(stale, &store);

  return Require(!ungoverned_result.accepted &&
                     ungoverned_result.diagnostic_code ==
                         "SB_OPTIMIZER_MEMORY_FEEDBACK.UNGOVERNED",
                 "ungoverned memory feedback was accepted") &&
         Require(!stale_result.accepted &&
                     stale_result.diagnostic_code ==
                         "SB_OPTIMIZER_MEMORY_FEEDBACK.STALE",
                 "stale memory feedback was accepted") &&
         Require(store.Snapshot().total_records == 0,
                 "rejected memory feedback was recorded");
}

}  // namespace

int main() {
  if (!MemorySpillFeedbackRecordsAndAdjustsCost()) return EXIT_FAILURE;
  if (!MemorySpillFeedbackExpiresAndInvalidates()) return EXIT_FAILURE;
  if (!MemorySpillFeedbackRejectsUngovernedAndStaleEvidence()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
