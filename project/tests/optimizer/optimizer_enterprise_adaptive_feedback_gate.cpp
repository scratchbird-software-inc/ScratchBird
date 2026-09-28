// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_adaptive_feedback_enterprise.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <type_traits>

#include <cstdlib>
#include <iostream>
#include <string>

namespace opt = scratchbird::engine::optimizer;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OEIC adaptive feedback gate failure: " << message << '\n';
    return false;
  }
  return true;
}

opt::AdaptiveCardinalityFeedbackRequest AdaptiveRequest() {
  // SEARCH_KEY: OEIC_ADAPTIVE_CARDINALITY_FEEDBACK_ENTERPRISE
  opt::AdaptiveCardinalityFeedbackRequest request;
  request.feedback.operator_family = "hash_join";
  request.feedback.plan_shape = "join:customer_orders";
  request.feedback.cost_profile_id = "enterprise-feedback";
  request.feedback.estimated_rows = 100;
  request.feedback.actual_rows = 5000;
  request.feedback.actual_rows_examined = 12000;
  request.feedback.actual_rows_filtered = 7000;
  request.feedback.loop_count = 3;
  request.feedback.estimated_pages = 8;
  request.feedback.actual_pages = 96;
  request.feedback.estimated_io_operations = 10;
  request.feedback.actual_io_operations = 140;
  request.feedback.estimated_visibility_recheck_rows = 50;
  request.feedback.actual_visibility_recheck_rows = 4000;
  request.feedback.estimated_spill_bytes = 0;
  request.feedback.actual_spill_bytes = 4 * 1024 * 1024;
  request.feedback.memory_grant_bytes = 512 * 1024;
  request.feedback.peak_memory_bytes = 2 * 1024 * 1024;
  request.feedback.estimated_latency_microseconds = 1000;
  request.feedback.actual_latency_microseconds = 50000;
  request.feedback.estimated_resource_units = 100;
  request.feedback.actual_resource_units = 5000;
  request.feedback.freshness_microseconds = 1000;
  request.feedback.policy_allowed = true;
  request.feedback.advisory_only = true;
  request.feedback.mga_visibility_recheck_preserved = true;
  request.feedback.transaction_finality_authority = "engine_transaction_inventory";

  request.baseline_cost.startup_cost = 100;
  request.baseline_cost.row_cost = 100;
  request.baseline_cost.io_cost = 100;
  request.baseline_cost.memory_cost = 100;
  request.baseline_cost.total_cost = 400;
  request.baseline_cost.confidence = opt::CostConfidence::kMedium;
  request.baseline_cost.selectable = true;
  request.baseline_cost.reason = "baseline";

  request.authority.engine_mga_snapshot_bound = true;
  request.authority.transaction_inventory_authoritative = true;
  request.authority.security_recheck_required = true;
  request.authority.exact_recheck_required = true;
  request.epochs.feedback_generation = 10;
  request.epochs.expected_feedback_generation = 10;
  request.epochs.feedback_epoch = 11;
  request.epochs.catalog_epoch = 20;
  request.epochs.expected_catalog_epoch = 20;
  request.epochs.security_epoch = 30;
  request.epochs.expected_security_epoch = 30;
  request.plan.route_label = "embedded:select:join";
  request.plan.baseline_plan_hash = "plan:baseline";
  request.plan.variant_plan_hash = "plan:variant";
  request.plan.fallback_plan_hash = "plan:fallback";
  request.plan.result_hash = "result:hash";
  request.plan.fallback_result_hash = "result:hash";
  request.plan.runtime_consumed = true;
  request.plan.exact_fallback_available = true;
  request.bind_sensitive_variant_requested = true;
  request.misestimate_quarantine_requested = true;
  request.extended_stat_request_requested = true;
  request.extended_stat_source_authoritative = true;
  return request;
}

opt::EnterpriseAdaptiveFeedbackApplyRequest ApplyRequest(scratchbird::core::platform::Uuid uuid) {
  opt::EnterpriseAdaptiveFeedbackApplyRequest request;
  request.feedback_uuid = uuid;
  request.scope_uuid = scratchbird::tests::FixtureUuid(0x030, 10);
  request.bind_profile_digest = "bind-profile:customer-orders";
  request.predicate_digest = "predicate:customer-orders";
  request.metric_snapshot_digest = "metric-snapshot:adaptive:1";
  request.feedback_generation = 10;
  request.policy_generation = 20;
  request.catalog_epoch = 30;
  request.security_epoch = 40;
  request.created_microseconds = 1000000;
  request.expires_after_microseconds = 5000000;
  request.adaptive_request = AdaptiveRequest();
  return request;
}

bool EnterpriseAdaptiveFeedbackRecordsAndAgesScopedFeedback() {
  opt::EnterpriseAdaptiveFeedbackStore store;
  const auto result = opt::ApplyEnterpriseAdaptiveFeedback(
      ApplyRequest(scratchbird::tests::FixtureUuid(0x030, 1)), &store);
  const auto found = store.Find(scratchbird::tests::FixtureUuid(0x030, 1));
  const auto snapshot = store.Snapshot();

  if (!Require(result.ok && result.benchmark_clean,
               "adaptive feedback was not applied: " + result.diagnostic_code) ||
      !Require(found.has_value(), "feedback record not stored") ||
      !Require(found->bind_sensitive_variant_created &&
                   found->misestimate_quarantined &&
                   found->extended_stat_requested,
               "feedback actions were not recorded") ||
      !Require(snapshot.valid_records == 1 &&
                   snapshot.quarantined_records == 1,
               "feedback snapshot counters mismatch")) {
    return false;
  }

  const auto expired = store.Expire(7000000);
  const auto after_expire = store.Find(scratchbird::tests::FixtureUuid(0x030, 1));
  return Require(expired == 1, "feedback record did not age out") &&
         Require(after_expire.has_value() && !after_expire->valid &&
                     after_expire->invalidation_reason == "feedback_age_expired",
                 "aged feedback record did not carry expiry evidence");
}

bool EnterpriseAdaptiveFeedbackInvalidatesByScopeAndEpoch() {
  opt::EnterpriseAdaptiveFeedbackStore store;
  if (!opt::ApplyEnterpriseAdaptiveFeedback(
           ApplyRequest(scratchbird::tests::FixtureUuid(0x030, 2)), &store).ok) {
    return Require(false, "adaptive feedback setup failed");
  }
  opt::EnterpriseAdaptiveFeedbackInvalidation event;
  event.scope_uuid = scratchbird::tests::FixtureUuid(0x030, 10);
  event.policy_generation = 21;
  event.reason = "policy_epoch_changed";
  const auto invalidated = store.Invalidate(event);
  const auto found = store.Find(scratchbird::tests::FixtureUuid(0x030, 2));
  return Require(invalidated == 1, "feedback invalidation did not match") &&
         Require(found.has_value() && !found->valid &&
                     found->invalidation_reason == "policy_epoch_changed",
                 "feedback invalidation evidence missing");
}

bool NativeAdaptiveIdentitiesAndInvalidationAreExact() {
  using NativeUuid = scratchbird::core::platform::Uuid;
  using Request = opt::EnterpriseAdaptiveFeedbackApplyRequest;
  using Record = opt::EnterpriseAdaptiveFeedbackRecord;
  using Invalidation = opt::EnterpriseAdaptiveFeedbackInvalidation;
  static_assert(std::is_same_v<decltype(Request::feedback_uuid), NativeUuid>);
  static_assert(std::is_same_v<decltype(Request::scope_uuid), NativeUuid>);
  static_assert(std::is_same_v<decltype(Record::feedback_uuid), NativeUuid>);
  static_assert(std::is_same_v<decltype(Record::scope_uuid), NativeUuid>);
  static_assert(std::is_same_v<decltype(Invalidation::scope_uuid),
                               std::optional<NativeUuid>>);
  const auto base = ApplyRequest(scratchbird::tests::FixtureUuid(0x030, 99));
  opt::EnterpriseAdaptiveFeedbackStore reference;
  if (!Require(opt::ApplyEnterpriseAdaptiveFeedback(base, &reference).ok,
               "native reference admission failed")) return false;
  const auto original = *reference.Find(base.feedback_uuid);
  for (const auto field : {&Request::feedback_uuid, &Request::scope_uuid}) {
    for (unsigned position = 0; position != 17; ++position) {
      for (unsigned octet = 0; octet != (position == 16 ? 1 : 256); ++octet) {
        auto changed = base;
        if (position == 16) changed.*field = {};
        else {
          if ((changed.*field).bytes[position] == octet) continue;
          (changed.*field).bytes[position] = static_cast<std::uint8_t>(octet);
        }
        const auto id = changed.*field;
        const bool valid = (id.bytes[6] & 0xf0) == 0x70 && (id.bytes[8] & 0xc0) == 0x80;
        opt::EnterpriseAdaptiveFeedbackStore apply_store, direct_store;
        if (!Require(apply_store.Record(original).ok && direct_store.Record(original).ok,
                     "baseline native records refused")) return false;
        auto direct_record = original;
        direct_record.feedback_uuid = changed.feedback_uuid;
        direct_record.scope_uuid = changed.scope_uuid;
        const auto applied = opt::ApplyEnterpriseAdaptiveFeedback(changed, &apply_store);
        const auto recorded = direct_store.Record(direct_record);
        if (!Require(applied.ok == valid && recorded.ok == valid,
                     "request and direct-store admission disagree with UUIDv7 oracle"))
          return false;
        for (const auto* store : {&apply_store, &direct_store}) {
          const auto snapshot = store->Snapshot();
          if (!Require(snapshot.total_records ==
                           (valid && field == &Request::feedback_uuid ? 2 : 1),
                       "binary UUID alias or malformed identity mutated store")) return false;
          const auto found = store->Find(valid ? changed.feedback_uuid : base.feedback_uuid);
          if (!Require(found.has_value() && found->valid &&
                           found->feedback_uuid == (valid ? changed.feedback_uuid : base.feedback_uuid) &&
                           found->scope_uuid == (valid ? changed.scope_uuid : base.scope_uuid),
                       "store lookup normalized or truncated identity bytes")) return false;
        }
        if (!valid &&
            !Require(applied.diagnostic_code ==
                         "SB_OPT_ENTERPRISE_ADAPTIVE_FEEDBACK_SCOPE_REQUIRED" &&
                     recorded.diagnostic_code == applied.diagnostic_code,
                     "malformed UUID bypassed admission")) return false;
        if (!Require(changed.*field == id, "request UUID was normalized")) return false;
      }
    }
  }
  Invalidation event;
  event.policy_generation = 21;
  event.scope_uuid = NativeUuid{};
  if (!Require(reference.Invalidate(event) == 0, "nil scope became wildcard")) return false;
  for (unsigned position = 0; position != 16; ++position) {
    for (unsigned octet = 0; octet != 256; ++octet) {
      auto id = base.scope_uuid;
      if (id.bytes[position] == octet) continue;
      id.bytes[position] = static_cast<std::uint8_t>(octet);
      event.scope_uuid = id;
      if (!Require(reference.Invalidate(event) == 0,
                   "different or malformed scope invalidated native record")) return false;
      auto lookup = base.feedback_uuid;
      lookup.bytes[position] = static_cast<std::uint8_t>(octet);
      if (lookup != base.feedback_uuid &&
          !Require(!reference.Find(lookup), "distinct native feedback lookup aliased"))
        return false;
    }
  }
  event.scope_uuid = base.scope_uuid;
  if (!Require(reference.Invalidate(event) == 1, "exact native scope did not match"))
    return false;
  if (!Require(reference.Record(original).ok, "native replacement failed")) return false;
  auto second = original;
  second.feedback_uuid = scratchbird::tests::FixtureUuid(0x030, 100);
  second.scope_uuid = scratchbird::tests::FixtureUuid(0x030, 101);
  if (!Require(reference.Record(second).ok, "second native scope refused")) return false;
  event.scope_uuid.reset();
  return Require(reference.Invalidate(event) == 2 &&
                     reference.Snapshot().valid_records == 0,
                 "explicit absent scope did not invalidate all scopes");
}

bool EnterpriseAdaptiveFeedbackRefusesAuthorityDrift() {
  opt::EnterpriseAdaptiveFeedbackStore store;
  auto request = ApplyRequest(scratchbird::tests::FixtureUuid(0x030, 3));
  request.adaptive_request.authority.parser_client_or_reference_feedback_authority = true;
  const auto result = opt::ApplyEnterpriseAdaptiveFeedback(request, &store);
  return Require(!result.ok, "unsafe adaptive feedback was accepted") &&
         Require(store.Snapshot().total_records == 0,
                 "unsafe adaptive feedback was recorded");
}

}  // namespace

int main() {
  if (!EnterpriseAdaptiveFeedbackRecordsAndAgesScopedFeedback()) return EXIT_FAILURE;
  if (!EnterpriseAdaptiveFeedbackInvalidatesByScopeAndEpoch()) return EXIT_FAILURE;
  if (!EnterpriseAdaptiveFeedbackRefusesAuthorityDrift()) return EXIT_FAILURE;
  if (!NativeAdaptiveIdentitiesAndInvalidationAreExact()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
