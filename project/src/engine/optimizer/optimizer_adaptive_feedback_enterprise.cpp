// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_adaptive_feedback_enterprise.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

void AddEvidence(AdaptiveCardinalityFeedbackResult* result,
                 std::string evidence) {
  if (result == nullptr) return;
  result->evidence.push_back(std::move(evidence));
}

AdaptiveCardinalityFeedbackResult Refuse(std::string code, std::string evidence) {
  AdaptiveCardinalityFeedbackResult result;
  result.ok = false;
  result.benchmark_clean = false;
  result.fail_closed = true;
  result.fallback_used = false;
  result.diagnostic_code = std::move(code);
  result.fallback_reason = "enterprise_adaptive_feedback_refused";
  result.evidence.push_back(std::move(evidence));
  result.evidence.push_back("enterprise_adaptive_feedback.recorded=false");
  result.evidence.push_back("enterprise_adaptive_feedback.authority=advisory_only");
  return result;
}

bool MissingScope(const EnterpriseAdaptiveFeedbackApplyRequest& request) {
  return request.feedback_uuid.empty() ||
         request.scope_uuid.empty() ||
         request.metric_snapshot_digest.empty() ||
         request.feedback_generation == 0 ||
         request.policy_generation == 0 ||
         request.catalog_epoch == 0 ||
         request.security_epoch == 0 ||
         request.created_microseconds == 0 ||
         request.expires_after_microseconds == 0;
}

bool RecordExpired(const EnterpriseAdaptiveFeedbackRecord& record,
                   std::uint64_t now_microseconds) {
  return record.expires_after_microseconds != 0 &&
         now_microseconds >= record.created_microseconds &&
         now_microseconds - record.created_microseconds >=
             record.expires_after_microseconds;
}

bool InvalidationMatches(const EnterpriseAdaptiveFeedbackRecord& record,
                         const EnterpriseAdaptiveFeedbackInvalidation& event) {
  const bool scope_matches = event.scope_uuid.empty() ||
                             event.scope_uuid == record.scope_uuid;
  const bool policy_changed = event.policy_generation != 0 &&
                              event.policy_generation != record.policy_generation;
  const bool catalog_changed = event.catalog_epoch != 0 &&
                               event.catalog_epoch != record.catalog_epoch;
  const bool security_changed = event.security_epoch != 0 &&
                                event.security_epoch != record.security_epoch;
  return scope_matches && (policy_changed || catalog_changed || security_changed);
}

}  // namespace

AdaptiveCardinalityFeedbackResult EnterpriseAdaptiveFeedbackStore::Record(
    EnterpriseAdaptiveFeedbackRecord record) {
  if (record.feedback_uuid.empty() || record.scope_uuid.empty() ||
      record.route_label.empty() || record.metric_snapshot_digest.empty() ||
      record.feedback_generation == 0 || record.policy_generation == 0 ||
      record.catalog_epoch == 0 || record.security_epoch == 0) {
    return Refuse("SB_OPT_ENTERPRISE_ADAPTIVE_FEEDBACK_SCOPE_REQUIRED",
                  "scope_uuid_route_label_metric_digest_and_epochs_required");
  }
  if (!record.adaptive_result.ok || !record.adaptive_result.benchmark_clean) {
    return Refuse("SB_OPT_ENTERPRISE_ADAPTIVE_FEEDBACK_RESULT_NOT_APPLIED",
                  "only benchmark-clean applied feedback can be recorded");
  }
  record.valid = true;
  record.evidence.push_back("enterprise_adaptive_feedback.recorded=true");
  record.evidence.push_back("enterprise_adaptive_feedback.scope_uuid=" +
                            record.scope_uuid);
  record.evidence.push_back("enterprise_adaptive_feedback.metric_snapshot_digest=" +
                            record.metric_snapshot_digest);
  record.evidence.push_back("enterprise_adaptive_feedback.bind_profile_digest=" +
                            record.bind_profile_digest);
  record.evidence.push_back("enterprise_adaptive_feedback.misestimate_quarantine=" +
                            std::string(record.misestimate_quarantined ? "true" : "false"));
  auto result = record.adaptive_result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(records_.begin(), records_.end(), [&](const auto& existing) {
      return existing.feedback_uuid == record.feedback_uuid;
    });
    if (it == records_.end()) {
      records_.push_back(std::move(record));
    } else {
      *it = std::move(record);
    }
  }
  AddEvidence(&result, "enterprise_adaptive_feedback.recorded=true");
  AddEvidence(&result, "enterprise_adaptive_feedback.invalidatable=true");
  AddEvidence(&result, "enterprise_adaptive_feedback.ageable=true");
  AddEvidence(&result, "enterprise_adaptive_feedback.authority=advisory_only");
  return result;
}

std::uint64_t EnterpriseAdaptiveFeedbackStore::Invalidate(
    const EnterpriseAdaptiveFeedbackInvalidation& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::uint64_t invalidated = 0;
  for (auto& record : records_) {
    if (!record.valid || !InvalidationMatches(record, event)) continue;
    record.valid = false;
    record.invalidation_reason =
        event.reason.empty() ? "epoch_or_scope_invalidation" : event.reason;
    record.evidence.push_back("enterprise_adaptive_feedback.invalidated=true");
    record.evidence.push_back("enterprise_adaptive_feedback.invalidation_reason=" +
                              record.invalidation_reason);
    ++invalidated;
  }
  return invalidated;
}

std::uint64_t EnterpriseAdaptiveFeedbackStore::Expire(
    std::uint64_t now_microseconds) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::uint64_t expired = 0;
  for (auto& record : records_) {
    if (!record.valid || !RecordExpired(record, now_microseconds)) continue;
    record.valid = false;
    record.invalidation_reason = "feedback_age_expired";
    record.evidence.push_back("enterprise_adaptive_feedback.expired=true");
    ++expired;
  }
  return expired;
}

EnterpriseAdaptiveFeedbackSnapshot EnterpriseAdaptiveFeedbackStore::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  EnterpriseAdaptiveFeedbackSnapshot snapshot;
  snapshot.records = records_;
  snapshot.total_records = records_.size();
  for (const auto& record : records_) {
    if (record.valid) ++snapshot.valid_records;
    else ++snapshot.invalidated_records;
    if (record.misestimate_quarantined) ++snapshot.quarantined_records;
  }
  return snapshot;
}

std::optional<EnterpriseAdaptiveFeedbackRecord> EnterpriseAdaptiveFeedbackStore::Find(
    const std::string& feedback_uuid) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(records_.begin(), records_.end(), [&](const auto& record) {
    return record.feedback_uuid == feedback_uuid;
  });
  if (it == records_.end()) return std::nullopt;
  return *it;
}

AdaptiveCardinalityFeedbackResult ApplyEnterpriseAdaptiveFeedback(
    const EnterpriseAdaptiveFeedbackApplyRequest& request,
    EnterpriseAdaptiveFeedbackStore* store) {
  if (store == nullptr) {
    return Refuse("SB_OPT_ENTERPRISE_ADAPTIVE_FEEDBACK_STORE_REQUIRED",
                  "store_required");
  }
  if (MissingScope(request)) {
    return Refuse("SB_OPT_ENTERPRISE_ADAPTIVE_FEEDBACK_SCOPE_REQUIRED",
                  "feedback_uuid_scope_metric_digest_and_epochs_required");
  }
  if (request.adaptive_request.bind_sensitive_variant_requested &&
      request.bind_profile_digest.empty()) {
    return Refuse("SB_OPT_ENTERPRISE_ADAPTIVE_FEEDBACK_BIND_PROFILE_REQUIRED",
                  "bind_profile_digest_required");
  }
  if (request.adaptive_request.extended_stat_request_requested &&
      request.predicate_digest.empty()) {
    return Refuse("SB_OPT_ENTERPRISE_ADAPTIVE_FEEDBACK_PREDICATE_DIGEST_REQUIRED",
                  "predicate_digest_required");
  }

  auto adaptive = EvaluateAdaptiveCardinalityFeedback(request.adaptive_request);
  if (!adaptive.ok || !adaptive.benchmark_clean) {
    AddEvidence(&adaptive, "enterprise_adaptive_feedback.recorded=false");
    return adaptive;
  }

  EnterpriseAdaptiveFeedbackRecord record;
  record.feedback_uuid = request.feedback_uuid;
  record.scope_uuid = request.scope_uuid;
  record.bind_profile_digest = request.bind_profile_digest;
  record.predicate_digest = request.predicate_digest;
  record.metric_snapshot_digest = request.metric_snapshot_digest;
  record.route_label = request.adaptive_request.plan.route_label;
  record.baseline_plan_hash = request.adaptive_request.plan.baseline_plan_hash;
  record.variant_plan_hash = request.adaptive_request.plan.variant_plan_hash;
  record.feedback_generation = request.feedback_generation;
  record.policy_generation = request.policy_generation;
  record.catalog_epoch = request.catalog_epoch;
  record.security_epoch = request.security_epoch;
  record.created_microseconds = request.created_microseconds;
  record.expires_after_microseconds = request.expires_after_microseconds;
  record.bind_sensitive_variant_created = adaptive.bind_sensitive_variant_created;
  record.misestimate_quarantined = adaptive.misestimate_quarantined;
  record.extended_stat_requested = adaptive.extended_stat_requested;
  record.adaptive_result = adaptive;
  return store->Record(std::move(record));
}

}  // namespace scratchbird::engine::optimizer
