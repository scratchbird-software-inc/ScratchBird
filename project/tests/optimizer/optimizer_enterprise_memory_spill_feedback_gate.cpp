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
  evidence.query_uuid = scratchbird::tests::FixtureUuid(2212, 1);
  evidence.scope_uuid = scratchbird::tests::FixtureUuid(2212, 2);
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
  event.scope_uuid = scratchbird::tests::FixtureUuid(2212, 2);
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

bool NativeScopeAndInvalidation() {
  using scratchbird::tests::FixtureUuid;
  const auto key = FixtureUuid(2212, 90);
  auto request = ApplyRequest(key);
  opt::EnterpriseMemorySpillFeedbackStore seed;
  if (!Require(opt::ApplyEnterpriseMemorySpillFeedback(request, &seed).accepted,
               "native scope fixture admission failed")) return false;
  const auto original = *seed.Find(key);
  for (const bool query : {false, true}) {
    for (unsigned position = 0; position < 16; ++position) {
      for (unsigned octet = 0; octet < 256; ++octet) {
        auto changed = request;
        auto& id = query ? changed.evidence.query_uuid : changed.evidence.scope_uuid;
        id.bytes[position] = static_cast<unsigned char>(octet);
        const bool valid = (id.bytes[6] & 0xf0) == 0x70 && (id.bytes[8] & 0xc0) == 0x80;
        const auto bridge = opt::BuildOptimizerMemoryFeedbackForPlanner(changed.evidence);
        if (!Require(bridge.ok() == valid && bridge.fail_closed == !valid &&
                     bridge.diagnostic.arguments.size() >= 2 &&
                     bridge.diagnostic.arguments[0].uuid() &&
                     *bridge.diagnostic.arguments[0].uuid() == changed.evidence.query_uuid &&
                     bridge.diagnostic.arguments[1].uuid() &&
                     *bridge.diagnostic.arguments[1].uuid() == changed.evidence.scope_uuid,
                     "bridge dropped UUID bytes or confused diagnostic data with authority")) return false;
        if (!Require(valid ? bridge.query_uuid == changed.evidence.query_uuid &&
                              bridge.scope_uuid == changed.evidence.scope_uuid :
                              bridge.query_uuid.is_nil() && bridge.scope_uuid.is_nil(),
                     "refused bridge retained admitted native scope")) return false;
        opt::EnterpriseMemorySpillFeedbackStore applied_store;
        const auto applied = opt::ApplyEnterpriseMemorySpillFeedback(changed, &applied_store);
        if (!Require(applied.accepted == valid && applied.fail_closed == !valid &&
                     applied_store.Snapshot().total_records == (valid ? 1u : 0u),
                     "apply path bypassed native query or scope admission")) return false;
        if (valid && !Require(applied_store.Find(key)->query_uuid == changed.evidence.query_uuid &&
                              applied_store.Find(key)->scope_uuid == changed.evidence.scope_uuid,
                              "applied feedback changed a native scope byte")) return false;

        opt::EnterpriseMemorySpillFeedbackStore direct;
        auto record = original;
        record.query_uuid = changed.evidence.query_uuid;
        record.scope_uuid = changed.evidence.scope_uuid;
        const auto detached = direct.Record(record);
        const bool same = record.query_uuid == original.query_uuid &&
                          record.scope_uuid == original.scope_uuid;
        if (!Require(detached.accepted == same &&
                     direct.Snapshot().total_records == (same ? 1u : 0u),
                     "direct store accepted identity different from the admitted bridge")) return false;
        record.bridge_result = bridge;
        const auto matched = direct.Record(record);
        if (!Require(matched.accepted == valid &&
                     direct.Snapshot().total_records == (valid ? 1u : 0u),
                     "direct store failed exact native bridge binding")) return false;
        if (!query) {
          opt::EnterpriseMemorySpillFeedbackStore invalidation_store;
          if (!Require(invalidation_store.Record(original).accepted, "invalidation fixture refused")) return false;
          opt::EnterpriseMemorySpillFeedbackInvalidation event;
          event.scope_uuid = id;
          event.security_epoch = original.security_epoch + 1;
          const bool exact = valid && id == original.scope_uuid;
          if (!Require(invalidation_store.Invalidate(event) == (exact ? 1u : 0u) &&
                       invalidation_store.Find(key)->valid == !exact,
                       "invalidation ignored a selector byte or treated invalid input as wildcard")) return false;
        }
      }
    }
  }
  for (const bool query : {false, true}) {
    auto nil_request = request;
    (query ? nil_request.evidence.query_uuid : nil_request.evidence.scope_uuid) = {};
    const auto nil = opt::ApplyEnterpriseMemorySpillFeedback(nil_request, &seed);
    if (!Require(!nil.accepted && nil.feedback_uuid.is_nil() && seed.Find(key)->valid &&
                 seed.Snapshot().total_records == 1, "nil query/scope overwrote admitted record")) return false;
  }
  auto second = request;
  second.feedback_uuid = FixtureUuid(2212, 91);
  second.evidence.scope_uuid = FixtureUuid(2212, 92);
  if (!Require(opt::ApplyEnterpriseMemorySpillFeedback(second, &seed).accepted,
               "distinct native scope could not retain its own record")) return false;
  opt::EnterpriseMemorySpillFeedbackInvalidation event;
  event.scope_uuid = scratchbird::core::platform::Uuid{};
  event.security_epoch = original.security_epoch + 1;
  if (!Require(seed.Invalidate(event) == 0 && seed.Snapshot().valid_records == 2,
               "explicit nil selector became all-scope invalidation")) return false;
  event.scope_uuid.reset();
  if (!Require(seed.Invalidate(event) == 2 && seed.Snapshot().valid_records == 0,
               "explicitly absent selector did not invalidate all matching generations")) return false;
  for (unsigned slot = 0; slot < 6; ++slot) {
    auto mismatch = request;
    switch (slot) {
      case 0: ++mismatch.policy_generation; break;
      case 1: ++mismatch.feedback_generation; break;
      case 2: ++mismatch.catalog_epoch; break;
      case 3: ++mismatch.security_epoch; break;
      case 4: mismatch.route_label += ".different"; break;
      case 5: mismatch.plan_node_id += ".different"; break;
    }
    opt::EnterpriseMemorySpillFeedbackStore store;
    const auto refused = opt::ApplyEnterpriseMemorySpillFeedback(mismatch, &store);
    if (!Require(!refused.accepted && refused.fail_closed && refused.feedback_uuid.is_nil() &&
                 refused.diagnostic_code == "SB-OPT-0001" && store.Snapshot().total_records == 0,
                 "request metadata was detached from admitted memory evidence")) return false;
  }
  return true;
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
      case 0: changed.query_uuid = FixtureUuid(2212, 11); break;
      case 1: changed.scope_uuid = FixtureUuid(2212, 12); break;
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
  if (!NativeScopeAndInvalidation()) return EXIT_FAILURE;
  if (!NativeFeedbackKeyAdmissionAndIsolation()) return EXIT_FAILURE;
  if (!MemorySpillFeedbackRecordsAndAdjustsCost()) return EXIT_FAILURE;
  if (!MemorySpillFeedbackExpiresAndInvalidates()) return EXIT_FAILURE;
  if (!MemorySpillFeedbackRejectsUngovernedAndStaleEvidence()) {
    return EXIT_FAILURE;
  }
  std::cout << "memory feedback native key checks=" << checks << '\n';
  return EXIT_SUCCESS;
}
