// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_memory_spill_feedback_enterprise.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

EnterpriseMemorySpillFeedbackApplyResult Refuse(std::string code,
                                                std::string evidence) {
  EnterpriseMemorySpillFeedbackApplyResult result;
  result.accepted = false;
  result.benchmark_clean = false;
  result.fail_closed = true;
  result.diagnostic_code = std::move(code);
  result.evidence.push_back(std::move(evidence));
  result.evidence.push_back("enterprise_memory_spill_feedback.recorded=false");
  return result;
}

bool MissingScope(const EnterpriseMemorySpillFeedbackApplyRequest& request) {
  return request.feedback_uuid.empty() ||
         request.reservation_id.empty() ||
         request.memory_snapshot_digest.empty() ||
         request.route_label.empty() ||
         request.plan_node_id.empty() ||
         request.policy_generation == 0 ||
         request.feedback_generation == 0 ||
         request.catalog_epoch == 0 ||
         request.security_epoch == 0 ||
         request.created_microseconds == 0 ||
         request.expires_after_microseconds == 0;
}

bool MissingRecordEvidence(const EnterpriseMemorySpillFeedbackRecord& record) {
  return record.source_kind.empty() ||
         record.provenance_digest.empty() ||
         record.redaction_class.empty() ||
         record.redaction_digest.empty() ||
         record.metric_snapshot_digest.empty() ||
         record.reservation_token.empty() ||
         record.reservation_generation == 0 ||
         record.redaction_epoch == 0 ||
         record.statistics_epoch == 0;
}

bool Expired(const EnterpriseMemorySpillFeedbackRecord& record,
             std::uint64_t now_microseconds) {
  return record.expires_after_microseconds != 0 &&
         now_microseconds >= record.created_microseconds &&
         now_microseconds - record.created_microseconds >=
             record.expires_after_microseconds;
}

bool InvalidationMatches(const EnterpriseMemorySpillFeedbackRecord& record,
                         const EnterpriseMemorySpillFeedbackInvalidation& event) {
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

void AddResultEvidence(EnterpriseMemorySpillFeedbackApplyResult* result,
                       std::string evidence) {
  if (result == nullptr) return;
  result->evidence.push_back(std::move(evidence));
}

}  // namespace

EnterpriseMemorySpillFeedbackApplyResult EnterpriseMemorySpillFeedbackStore::Record(
    EnterpriseMemorySpillFeedbackRecord record) {
  if (record.feedback_uuid.empty() || record.reservation_id.empty() ||
      record.memory_snapshot_digest.empty() || record.route_label.empty() ||
      record.plan_node_id.empty() || record.query_uuid.empty() ||
      record.scope_uuid.empty() || record.policy_generation == 0 ||
      record.feedback_generation == 0 || record.catalog_epoch == 0 ||
      record.security_epoch == 0 || MissingRecordEvidence(record)) {
    return Refuse("SB_OPT_ENTERPRISE_MEMORY_SPILL_FEEDBACK_SCOPE_REQUIRED",
                  "scope_reservation_metric_digest_route_plan_redaction_provenance_and_epochs_required");
  }
  if (!record.bridge_result.ok() ||
      !record.bridge_result.ceic_059_contract_accepted ||
      !record.bridge_result.authority_boundaries_clean ||
      !record.feedback_status.ok ||
      !record.feedback_status.applied) {
    return Refuse("SB_OPT_ENTERPRISE_MEMORY_SPILL_FEEDBACK_NOT_APPLIED",
                  "ceic_059_bridge_and_runtime_feedback_must_apply_cleanly");
  }
  record.valid = true;
  record.evidence.push_back("enterprise_memory_spill_feedback.recorded=true");
  record.evidence.push_back("enterprise_memory_spill_feedback.reservation_id=" +
                            record.reservation_id);
  record.evidence.push_back("enterprise_memory_spill_feedback.reservation_generation=" +
                            std::to_string(record.reservation_generation));
  record.evidence.push_back("enterprise_memory_spill_feedback.source_kind=" +
                            record.source_kind);
  record.evidence.push_back("enterprise_memory_spill_feedback.provenance_digest=" +
                            record.provenance_digest);
  record.evidence.push_back("enterprise_memory_spill_feedback.redaction_digest=" +
                            record.redaction_digest);
  record.evidence.push_back("enterprise_memory_spill_feedback.snapshot_digest=" +
                            record.memory_snapshot_digest);
  record.evidence.push_back("enterprise_memory_spill_feedback.metric_snapshot_digest=" +
                            record.metric_snapshot_digest);
  if (!record.support_snapshot_digest.empty()) {
    record.evidence.push_back(
        "enterprise_memory_spill_feedback.support_snapshot_digest=" +
        record.support_snapshot_digest);
  }
  EnterpriseMemorySpillFeedbackApplyResult result;
  result.accepted = true;
  result.benchmark_clean = record.bridge_result.ceic_059_contract_accepted &&
                           record.bridge_result.authority_boundaries_clean;
  result.fail_closed = false;
  result.diagnostic_code = "SB_OPT_ENTERPRISE_MEMORY_SPILL_FEEDBACK_ACCEPTED";
  result.bridge_result = record.bridge_result;
  result.feedback_status = record.feedback_status;
  result.adjusted_cost = record.adjusted_cost;
  result.evidence = record.evidence;
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

  AddResultEvidence(&result, "enterprise_memory_spill_feedback.invalidatable=true");
  AddResultEvidence(&result, "enterprise_memory_spill_feedback.ageable=true");
  AddResultEvidence(&result, "enterprise_memory_spill_feedback.authority=advisory_only");
  return result;
}

std::uint64_t EnterpriseMemorySpillFeedbackStore::Invalidate(
    const EnterpriseMemorySpillFeedbackInvalidation& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::uint64_t invalidated = 0;
  for (auto& record : records_) {
    if (!record.valid || !InvalidationMatches(record, event)) continue;
    record.valid = false;
    record.invalidation_reason =
        event.reason.empty() ? "epoch_or_scope_invalidation" : event.reason;
    record.evidence.push_back("enterprise_memory_spill_feedback.invalidated=true");
    record.evidence.push_back("enterprise_memory_spill_feedback.invalidation_reason=" +
                              record.invalidation_reason);
    ++invalidated;
  }
  return invalidated;
}

std::uint64_t EnterpriseMemorySpillFeedbackStore::Expire(
    std::uint64_t now_microseconds) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::uint64_t expired = 0;
  for (auto& record : records_) {
    if (!record.valid || !Expired(record, now_microseconds)) continue;
    record.valid = false;
    record.invalidation_reason = "memory_feedback_age_expired";
    record.evidence.push_back("enterprise_memory_spill_feedback.expired=true");
    ++expired;
  }
  return expired;
}

EnterpriseMemorySpillFeedbackSnapshot EnterpriseMemorySpillFeedbackStore::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  EnterpriseMemorySpillFeedbackSnapshot snapshot;
  snapshot.records = records_;
  snapshot.total_records = records_.size();
  for (const auto& record : records_) {
    if (record.valid) ++snapshot.valid_records;
    else ++snapshot.invalidated_records;
    if (record.bridge_result.runtime_feedback.actual_spill_bytes != 0) {
      ++snapshot.spill_records;
    }
  }
  return snapshot;
}

std::optional<EnterpriseMemorySpillFeedbackRecord> EnterpriseMemorySpillFeedbackStore::Find(
    const std::string& feedback_uuid) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(records_.begin(), records_.end(), [&](const auto& record) {
    return record.feedback_uuid == feedback_uuid;
  });
  if (it == records_.end()) return std::nullopt;
  return *it;
}

EnterpriseMemorySpillFeedbackApplyResult ApplyEnterpriseMemorySpillFeedback(
    const EnterpriseMemorySpillFeedbackApplyRequest& request,
    EnterpriseMemorySpillFeedbackStore* store) {
  if (store == nullptr) {
    return Refuse("SB_OPT_ENTERPRISE_MEMORY_SPILL_FEEDBACK_STORE_REQUIRED",
                  "store_required");
  }
  if (MissingScope(request)) {
    return Refuse("SB_OPT_ENTERPRISE_MEMORY_SPILL_FEEDBACK_SCOPE_REQUIRED",
                  "feedback_reservation_snapshot_route_plan_and_epochs_required");
  }
  auto bridge = BuildOptimizerMemoryFeedbackForPlanner(request.evidence);
  if (!bridge.ok()) {
    EnterpriseMemorySpillFeedbackApplyResult result;
    result.accepted = false;
    result.benchmark_clean = false;
    result.fail_closed = true;
    result.diagnostic_code = bridge.diagnostic_code;
    result.bridge_result = bridge;
    result.evidence = bridge.evidence;
    AddResultEvidence(&result, "enterprise_memory_spill_feedback.recorded=false");
    return result;
  }
  if (request.memory_snapshot_digest != request.evidence.metric_snapshot_digest) {
    return Refuse("SB_OPT_ENTERPRISE_MEMORY_SPILL_FEEDBACK_SNAPSHOT_MISMATCH",
                  "request_snapshot_digest_must_match_accepted_ceic_059_metric_snapshot");
  }
  if (!request.evidence.reservation_id.empty() &&
      request.reservation_id != request.evidence.reservation_id) {
    return Refuse("SB_OPT_ENTERPRISE_MEMORY_SPILL_FEEDBACK_RESERVATION_MISMATCH",
                  "request_reservation_must_match_accepted_ceic_059_reservation");
  }

  auto feedback_status = EvaluateOptimizerRuntimeFeedback(bridge.runtime_feedback);
  if (!feedback_status.ok || !feedback_status.applied) {
    return Refuse(feedback_status.diagnostic_code,
                  "runtime_memory_feedback_rejected");
  }

  EnterpriseMemorySpillFeedbackRecord record;
  record.feedback_uuid = request.feedback_uuid;
  record.reservation_id = request.reservation_id;
  record.memory_snapshot_digest = request.memory_snapshot_digest;
  record.source_kind = request.evidence.source_kind;
  record.provenance_digest = request.evidence.provenance_digest;
  record.redaction_class = request.evidence.redaction_class;
  record.redaction_digest = request.evidence.redaction_digest;
  record.metric_snapshot_digest = request.evidence.metric_snapshot_digest;
  record.support_snapshot_digest = request.evidence.support_snapshot_digest;
  record.reservation_token = request.evidence.reservation_token;
  record.reservation_generation = request.evidence.reservation_generation;
  record.route_label = request.route_label;
  record.plan_node_id = request.plan_node_id;
  record.query_uuid = request.evidence.query_uuid;
  record.scope_uuid = request.evidence.scope_uuid;
  record.policy_generation = request.policy_generation;
  record.feedback_generation = request.feedback_generation;
  record.catalog_epoch = request.catalog_epoch;
  record.security_epoch = request.security_epoch;
  record.redaction_epoch = request.evidence.redaction_epoch;
  record.statistics_epoch = request.evidence.statistics_epoch;
  record.created_microseconds = request.created_microseconds;
  record.expires_after_microseconds = request.expires_after_microseconds;
  record.bridge_result = bridge;
  record.feedback_status = feedback_status;
  record.adjusted_cost =
      ApplyOptimizerRuntimeFeedbackCost(request.baseline_cost,
                                        bridge.runtime_feedback);
  record.evidence = bridge.evidence;
  record.evidence.push_back("enterprise_memory_spill_feedback.spill_passes=" +
                            std::to_string(request.evidence.spill_passes));
  record.evidence.push_back("enterprise_memory_spill_feedback.adjusted_cost=true");
  return store->Record(std::move(record));
}

}  // namespace scratchbird::engine::optimizer
