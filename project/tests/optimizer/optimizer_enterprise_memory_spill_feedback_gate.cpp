// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_memory_spill_feedback_enterprise.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace opt = scratchbird::engine::optimizer;

namespace {

unsigned checks = 0;

bool Require(bool condition, const std::string& message) {
  ++checks;
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

opt::EnterpriseMemorySpillFeedbackApplyRequest ApplyRequest(scratchbird::core::platform::Uuid uuid) {
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
      ApplyRequest(scratchbird::tests::FixtureUuid(2211, 1)), &store);
  const auto found = store.Find(scratchbird::tests::FixtureUuid(2211, 1));
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
           ApplyRequest(scratchbird::tests::FixtureUuid(2211, 2)), &store).accepted) {
    return Require(false, "setup feedback failed");
  }
  const auto expired = store.Expire(7000000);
  const auto after_expire = store.Find(scratchbird::tests::FixtureUuid(2211, 2));
  if (!Require(expired == 1, "memory feedback did not expire") ||
      !Require(after_expire.has_value() && !after_expire->valid &&
                   after_expire->invalidation_reason == "memory_feedback_age_expired",
               "memory feedback expiry evidence missing")) {
    return false;
  }

  if (!opt::ApplyEnterpriseMemorySpillFeedback(
           ApplyRequest(scratchbird::tests::FixtureUuid(2211, 3)), &store).accepted) {
    return Require(false, "second setup feedback failed");
  }
  opt::EnterpriseMemorySpillFeedbackInvalidation event;
  event.scope_uuid = "scope.memory.feedback";
  event.security_epoch = 41;
  event.reason = "security_epoch_changed";
  const auto invalidated = store.Invalidate(event);
  const auto after_invalidate = store.Find(scratchbird::tests::FixtureUuid(2211, 3));
  return Require(invalidated == 1, "memory feedback invalidation did not match") &&
         Require(after_invalidate.has_value() && !after_invalidate->valid &&
                     after_invalidate->invalidation_reason == "security_epoch_changed",
                 "memory feedback invalidation evidence missing");
}

bool MemorySpillFeedbackRejectsUngovernedAndStaleEvidence() {
  opt::EnterpriseMemorySpillFeedbackStore store;
  auto ungoverned = ApplyRequest(scratchbird::tests::FixtureUuid(2211, 4));
  ungoverned.evidence.governed_reservation = false;
  const auto ungoverned_result =
      opt::ApplyEnterpriseMemorySpillFeedback(ungoverned, &store);

  auto stale = ApplyRequest(scratchbird::tests::FixtureUuid(2211, 5));
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

bool NativeFeedbackKeyAdmissionAndIsolation() {
  using scratchbird::tests::FixtureUuid;
  const auto key = FixtureUuid(2211, 90);
  opt::EnterpriseMemorySpillFeedbackStore seed;
  const auto admitted = opt::ApplyEnterpriseMemorySpillFeedback(ApplyRequest(key), &seed);
  const auto retained = seed.Find(key);
  if (!Require(admitted.accepted && admitted.feedback_uuid == key && retained &&
               retained->feedback_uuid == key, "native record key was not published exactly")) return false;

  // Exercise both planner admission and the public direct-store entry point.
  // The validity oracle is independent of the production UUID helper.
  for (const bool direct : {false, true}) {
    for (unsigned position = 0; position < 16; ++position) {
      for (unsigned octet = 0; octet < 256; ++octet) {
        auto changed = key;
        changed.bytes[position] = static_cast<unsigned char>(octet);
        const bool valid = (changed.bytes[6] & 0xf0) == 0x70 &&
                           (changed.bytes[8] & 0xc0) == 0x80;
        opt::EnterpriseMemorySpillFeedbackStore store;
        if (!Require(store.Record(*retained).accepted, "valid seed record rejected")) return false;
        auto record = *retained;
        record.feedback_uuid = changed;
        const auto result = direct ? store.Record(record) :
            opt::ApplyEnterpriseMemorySpillFeedback(ApplyRequest(changed), &store);
        if (!Require(result.accepted == valid && result.fail_closed == !valid,
                     "feedback key admission dropped a byte or accepted a non-v7 identity")) return false;
        const auto found = store.Find(changed);
        const auto original = store.Find(key);
        const auto snapshot = store.Snapshot();
        if (!Require(original && original->feedback_uuid == key && original->valid &&
                     original->query_uuid == retained->query_uuid &&
                     original->scope_uuid == retained->scope_uuid &&
                     original->adjusted_cost.total_cost == retained->adjusted_cost.total_cost,
                     "admission altered the pre-existing feedback owner")) return false;
        if (valid) {
          if (!Require(found && found->feedback_uuid == changed &&
                       result.feedback_uuid == changed && result.benchmark_clean &&
                       snapshot.total_records == (changed == key ? 1u : 2u) &&
                       snapshot.valid_records == snapshot.total_records,
                       "distinct binary keys aliased or valid key was not retained")) return false;
          if (!Require(store.Expire(retained->created_microseconds +
                                   retained->expires_after_microseconds - 1) == 0 &&
                       store.Expire(retained->created_microseconds +
                                   retained->expires_after_microseconds) == snapshot.total_records &&
                       store.Find(changed) && !store.Find(changed)->valid,
                       "native keys lost exact expiry boundary semantics")) return false;
        } else {
          if (!Require(!found && result.feedback_uuid.is_nil() && !result.benchmark_clean &&
                       result.diagnostic_code == "SB-OPT-0001" && snapshot.total_records == 1,
                       "invalid key published identity or mutated storage")) return false;
        }
      }
    }
  }

  auto missing = *retained;
  missing.feedback_uuid = {};
  const auto direct_nil = seed.Record(missing);
  const auto apply_nil = opt::ApplyEnterpriseMemorySpillFeedback(ApplyRequest({}), &seed);
  if (!Require(!direct_nil.accepted && !apply_nil.accepted &&
               direct_nil.feedback_uuid.is_nil() && apply_nil.feedback_uuid.is_nil() &&
               !seed.Find({}) && seed.Snapshot().total_records == 1,
               "nil feedback identity was stored or resolved to an existing record")) return false;

  // A record identity cannot be reassigned to a different query, scope or node.
  for (unsigned binding = 0; binding < 4; ++binding) {
    auto changed = *retained;
    switch (binding) {
      case 0: changed.query_uuid += ".different"; break;
      case 1: changed.scope_uuid += ".different"; break;
      case 2: changed.route_label += ".different"; break;
      case 3: changed.plan_node_id += ".different"; break;
    }
    const auto refused = seed.Record(changed);
    const auto original = seed.Find(key);
    if (!Require(!refused.accepted && refused.fail_closed && refused.feedback_uuid.is_nil() &&
                 refused.diagnostic_code == "SB-OPT-0001" && original &&
                 original->query_uuid == retained->query_uuid &&
                 original->scope_uuid == retained->scope_uuid &&
                 original->route_label == retained->route_label &&
                 original->plan_node_id == retained->plan_node_id &&
                 seed.Snapshot().total_records == 1,
                 "duplicate native key was rebound to another feedback owner")) return false;
    auto request = ApplyRequest(key);
    request.evidence.query_uuid = changed.query_uuid;
    request.evidence.scope_uuid = changed.scope_uuid;
    request.route_label = changed.route_label;
    request.plan_node_id = changed.plan_node_id;
    const auto applied = opt::ApplyEnterpriseMemorySpillFeedback(request, &seed);
    if (!Require(!applied.accepted && applied.fail_closed && applied.feedback_uuid.is_nil() &&
                 seed.Find(key)->query_uuid == retained->query_uuid &&
                 seed.Find(key)->scope_uuid == retained->scope_uuid &&
                 seed.Find(key)->route_label == retained->route_label &&
                 seed.Find(key)->plan_node_id == retained->plan_node_id,
                 "apply path bypassed retained key ownership")) return false;
  }
  auto same_owner = *retained;
  same_owner.adjusted_cost.total_cost += 1;
  const auto updated = seed.Record(same_owner);
  if (!Require(updated.accepted && updated.feedback_uuid == key &&
               seed.Find(key)->adjusted_cost.total_cost == same_owner.adjusted_cost.total_cost &&
               seed.Snapshot().total_records == 1,
               "same-owner update lost exact key or created a duplicate")) return false;
  for (const bool missing_creation : {false, true}) {
    auto unageable = *retained;
    if (missing_creation) unageable.created_microseconds = 0;
    else unageable.expires_after_microseconds = 0;
    if (!Require(!seed.Record(unageable).accepted && seed.Snapshot().total_records == 1 &&
                 seed.Find(key)->adjusted_cost.total_cost == same_owner.adjusted_cost.total_cost,
                 "direct record bypassed required aging metadata")) return false;
  }
  for (const unsigned lifetime : {5u, 10u}) {
    opt::EnterpriseMemorySpillFeedbackStore boundary;
    auto edge = *retained;
    edge.created_microseconds = std::numeric_limits<std::uint64_t>::max() - 5;
    edge.expires_after_microseconds = lifetime;
    if (!Require(boundary.Record(edge).accepted && boundary.Expire(0) == 0 &&
                 boundary.Expire(std::numeric_limits<std::uint64_t>::max() - 1) == 0 &&
                 boundary.Expire(std::numeric_limits<std::uint64_t>::max()) ==
                     (lifetime == 5 ? 1u : 0u) &&
                 boundary.Find(key)->feedback_uuid == key,
                 "expiry wrapped an unsigned timestamp or lost native key")) return false;
  }
  return true;
}

}  // namespace

int main() {
  if (!NativeFeedbackKeyAdmissionAndIsolation()) return EXIT_FAILURE;
  if (!MemorySpillFeedbackRecordsAndAdjustsCost()) return EXIT_FAILURE;
  if (!MemorySpillFeedbackExpiresAndInvalidates()) return EXIT_FAILURE;
  if (!MemorySpillFeedbackRejectsUngovernedAndStaleEvidence()) {
    return EXIT_FAILURE;
  }
  std::cout << "memory feedback native key checks=" << checks << '\n';
  return EXIT_SUCCESS;
}
