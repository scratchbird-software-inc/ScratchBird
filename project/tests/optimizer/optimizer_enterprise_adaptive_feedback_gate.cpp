// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_adaptive_feedback_enterprise.hpp"

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

opt::EnterpriseAdaptiveFeedbackApplyRequest ApplyRequest(std::string uuid) {
  opt::EnterpriseAdaptiveFeedbackApplyRequest request;
  request.feedback_uuid = std::move(uuid);
  request.scope_uuid = "scope:optimizer:enterprise";
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
      ApplyRequest("feedback.enterprise.1"), &store);
  const auto found = store.Find("feedback.enterprise.1");
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
  const auto after_expire = store.Find("feedback.enterprise.1");
  return Require(expired == 1, "feedback record did not age out") &&
         Require(after_expire.has_value() && !after_expire->valid &&
                     after_expire->invalidation_reason == "feedback_age_expired",
                 "aged feedback record did not carry expiry evidence");
}

bool EnterpriseAdaptiveFeedbackInvalidatesByScopeAndEpoch() {
  opt::EnterpriseAdaptiveFeedbackStore store;
  if (!opt::ApplyEnterpriseAdaptiveFeedback(
           ApplyRequest("feedback.enterprise.2"), &store).ok) {
    return Require(false, "adaptive feedback setup failed");
  }
  opt::EnterpriseAdaptiveFeedbackInvalidation event;
  event.scope_uuid = "scope:optimizer:enterprise";
  event.policy_generation = 21;
  event.reason = "policy_epoch_changed";
  const auto invalidated = store.Invalidate(event);
  const auto found = store.Find("feedback.enterprise.2");
  return Require(invalidated == 1, "feedback invalidation did not match") &&
         Require(found.has_value() && !found->valid &&
                     found->invalidation_reason == "policy_epoch_changed",
                 "feedback invalidation evidence missing");
}

bool EnterpriseAdaptiveFeedbackRefusesAuthorityDrift() {
  opt::EnterpriseAdaptiveFeedbackStore store;
  auto request = ApplyRequest("feedback.enterprise.unsafe");
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
  return EXIT_SUCCESS;
}
