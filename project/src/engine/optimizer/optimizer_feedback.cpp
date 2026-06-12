// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_feedback.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

constexpr double kMinimumMultiplier = 0.25;
constexpr double kMaximumMultiplier = 4.0;
constexpr double kHighMisestimateRatio = 10.0;
constexpr double kIoPageDriftRatio = 2.0;
constexpr std::uint64_t kMinimumMemoryGrantBytes = 64 * 1024;

std::uint64_t NonZero(std::uint64_t value) {
  return std::max<std::uint64_t>(1, value);
}

double ErrorRatio(std::uint64_t estimated, std::uint64_t actual) {
  const auto high = static_cast<double>(std::max(NonZero(estimated), NonZero(actual)));
  const auto low = static_cast<double>(std::min(NonZero(estimated), NonZero(actual)));
  return high / low;
}

double DirectionalMultiplier(std::uint64_t estimated, std::uint64_t actual) {
  const double ratio = static_cast<double>(NonZero(actual)) / static_cast<double>(NonZero(estimated));
  return std::clamp(ratio, kMinimumMultiplier, kMaximumMultiplier);
}

std::uint64_t SaturatingAdd(std::uint64_t left, std::uint64_t right) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left + right;
}

std::uint64_t SaturatingMul(std::uint64_t left, std::uint64_t right) {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left * right;
}

void AddEvidence(OptimizerFeedbackStatus* status, std::string evidence) {
  if (status == nullptr) return;
  status->evidence.push_back(std::move(evidence));
}

bool HasEvidence(const OptimizerFeedbackStatus& status, const std::string& evidence) {
  return std::find(status.evidence.begin(), status.evidence.end(), evidence) != status.evidence.end();
}

std::string ProfileIdFor(const OptimizerRuntimeFeedback& feedback) {
  std::ostringstream out;
  out << "calibrated:" << feedback.cost_profile_id << ':' << feedback.operator_family << ':' << feedback.plan_shape;
  return out.str();
}

bool UnsafeAuthority(const OptimizerRuntimeFeedback& feedback) {
  return !feedback.advisory_only ||
         !feedback.mga_visibility_recheck_preserved ||
         feedback.parser_or_reference_authority ||
         feedback.transaction_finality_authority != "engine_transaction_inventory";
}

OptimizerMemoryGrantFeedback BuildMemoryGrantFeedback(const OptimizerRuntimeFeedback& feedback) {
  OptimizerMemoryGrantFeedback memory;
  memory.observed_grant_bytes = feedback.memory_grant_bytes;
  memory.observed_peak_bytes = feedback.peak_memory_bytes;
  memory.recommended_grant_bytes = feedback.memory_grant_bytes;

  if (feedback.memory_grant_bytes == 0 && feedback.peak_memory_bytes == 0) {
    return memory;
  }

  if (feedback.memory_grant_bytes == 0 && feedback.peak_memory_bytes != 0) {
    memory.apply = true;
    memory.recommended_grant_bytes = std::max(SaturatingMul(feedback.peak_memory_bytes, 2),
                                             kMinimumMemoryGrantBytes);
    memory.grant_multiplier = kMaximumMultiplier;
    memory.diagnostic_code = "SB_OPTIMIZER_FEEDBACK.MEMORY_UNDERGRANT";
    return memory;
  }

  if (feedback.peak_memory_bytes > feedback.memory_grant_bytes ||
      feedback.actual_spill_bytes > feedback.estimated_spill_bytes) {
    memory.apply = true;
    memory.recommended_grant_bytes = std::max(SaturatingAdd(feedback.peak_memory_bytes,
                                                            feedback.peak_memory_bytes / 4),
                                             feedback.memory_grant_bytes);
    memory.grant_multiplier = std::clamp(static_cast<double>(NonZero(memory.recommended_grant_bytes)) /
                                             static_cast<double>(NonZero(feedback.memory_grant_bytes)),
                                         1.0,
                                         kMaximumMultiplier);
    memory.diagnostic_code = "SB_OPTIMIZER_FEEDBACK.MEMORY_UNDERGRANT";
    return memory;
  }

  if (feedback.actual_spill_bytes == 0 &&
      feedback.peak_memory_bytes != 0 &&
      feedback.memory_grant_bytes >= SaturatingMul(feedback.peak_memory_bytes, 4)) {
    memory.apply = true;
    memory.recommended_grant_bytes = std::max(SaturatingAdd(feedback.peak_memory_bytes,
                                                            feedback.peak_memory_bytes / 2),
                                             kMinimumMemoryGrantBytes);
    memory.grant_multiplier = std::clamp(static_cast<double>(NonZero(memory.recommended_grant_bytes)) /
                                             static_cast<double>(NonZero(feedback.memory_grant_bytes)),
                                         kMinimumMultiplier,
                                         1.0);
    memory.diagnostic_code = "SB_OPTIMIZER_FEEDBACK.MEMORY_OVERGRANT";
  }

  return memory;
}

}  // namespace

OptimizerFeedbackStatus EvaluateOptimizerRuntimeFeedback(const OptimizerRuntimeFeedback& feedback) {
  OptimizerFeedbackStatus status;
  status.memory_grant = BuildMemoryGrantFeedback(feedback);

  if (feedback.operator_family.empty() || feedback.plan_shape.empty()) {
    status.ok = false;
    status.applied = false;
    status.diagnostic_code = "SB_OPTIMIZER_FEEDBACK.MISSING_LABELS";
    AddEvidence(&status, "missing_labels");
    return status;
  }

  if (!feedback.policy_allowed) {
    status.ok = false;
    status.applied = false;
    status.diagnostic_code = "SB_OPTIMIZER_FEEDBACK.POLICY_DISABLED";
    AddEvidence(&status, "policy_disabled_feedback");
    return status;
  }

  if (feedback.freshness_microseconds > feedback.max_freshness_microseconds) {
    status.ok = false;
    status.applied = false;
    status.diagnostic_code = "SB_OPTIMIZER_FEEDBACK.STALE";
    AddEvidence(&status, "stale_feedback");
    return status;
  }

  if (UnsafeAuthority(feedback)) {
    status.ok = false;
    status.applied = false;
    status.diagnostic_code = "SB_OPTIMIZER_FEEDBACK.REJECTED_UNSAFE";
    AddEvidence(&status, "rejected_unsafe_feedback");
    AddEvidence(&status, "mga_finality_authority_required=engine_transaction_inventory");
    AddEvidence(&status, "feedback_advisory_only_required=true");
    return status;
  }

  status.estimate_error_ratio = ErrorRatio(feedback.estimated_rows, feedback.actual_rows);
  status.page_error_ratio = ErrorRatio(feedback.estimated_pages, feedback.actual_pages);
  status.io_error_ratio = ErrorRatio(feedback.estimated_io_operations, feedback.actual_io_operations);
  status.ok = true;
  status.applied = true;
  status.diagnostic_code = "SB_OPTIMIZER_FEEDBACK.OK";
  AddEvidence(&status, "ok_feedback");
  AddEvidence(&status, "feedback_advisory_only=true");
  AddEvidence(&status, "mga_visibility_recheck=preserved");
  AddEvidence(&status, "mga_finality_authority=engine_transaction_inventory");

  if (status.estimate_error_ratio > kHighMisestimateRatio) {
    status.diagnostic_code = "SB_OPTIMIZER_FEEDBACK.HIGH_MISESTIMATE";
    AddEvidence(&status, "high_misestimate");
  }
  if (status.page_error_ratio > kIoPageDriftRatio || status.io_error_ratio > kIoPageDriftRatio) {
    if (status.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.OK") {
      status.diagnostic_code = "SB_OPTIMIZER_FEEDBACK.IO_PAGE_DRIFT";
    }
    AddEvidence(&status, "io_page_drift");
  }
  if (feedback.actual_spill_bytes > 0) {
    if (status.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.OK" ||
        status.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.IO_PAGE_DRIFT") {
      status.diagnostic_code = "SB_OPTIMIZER_FEEDBACK.SPILL_OBSERVED";
    }
    AddEvidence(&status, "spill_observed");
  }
  if (status.memory_grant.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.MEMORY_UNDERGRANT") {
    if (status.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.OK" ||
        status.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.IO_PAGE_DRIFT") {
      status.diagnostic_code = "SB_OPTIMIZER_FEEDBACK.MEMORY_UNDERGRANT";
    }
    AddEvidence(&status, "memory_undergrant");
  } else if (status.memory_grant.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.MEMORY_OVERGRANT") {
    if (status.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.OK") {
      status.diagnostic_code = "SB_OPTIMIZER_FEEDBACK.MEMORY_OVERGRANT";
    }
    AddEvidence(&status, "memory_overgrant");
  }

  status.cost_profile = BuildOptimizerCalibratedCostProfile(feedback, status);
  return status;
}

OptimizerCalibratedCostProfile BuildOptimizerCalibratedCostProfile(const OptimizerRuntimeFeedback& feedback,
                                                                    const OptimizerFeedbackStatus& status) {
  OptimizerCalibratedCostProfile profile;
  if (!status.ok || !status.applied) return profile;

  profile.apply = true;
  profile.profile_id = ProfileIdFor(feedback);
  profile.row_cost_multiplier = DirectionalMultiplier(feedback.estimated_rows, feedback.actual_rows);
  profile.page_cost_multiplier = DirectionalMultiplier(feedback.estimated_pages, feedback.actual_pages);
  profile.io_cost_multiplier = DirectionalMultiplier(feedback.estimated_io_operations,
                                                      feedback.actual_io_operations);
  profile.visibility_cost_multiplier = DirectionalMultiplier(feedback.estimated_visibility_recheck_rows,
                                                             feedback.actual_visibility_recheck_rows);
  profile.memory_cost_multiplier = status.memory_grant.apply ? status.memory_grant.grant_multiplier : 1.0;
  profile.latency_cost_multiplier = DirectionalMultiplier(feedback.estimated_latency_microseconds,
                                                          feedback.actual_latency_microseconds);

  const double resource_multiplier = DirectionalMultiplier(feedback.estimated_resource_units,
                                                           feedback.actual_resource_units);
  profile.row_cost_multiplier = std::clamp((profile.row_cost_multiplier + resource_multiplier) / 2.0,
                                           kMinimumMultiplier,
                                           kMaximumMultiplier);

  if (HasEvidence(status, "spill_observed")) {
    profile.spill_penalty_pages = (feedback.actual_spill_bytes + 4095) / 4096;
  }
  if (HasEvidence(status, "high_misestimate")) {
    profile.uncertainty_penalty = SaturatingAdd(profile.uncertainty_penalty, 1000);
  }
  if (HasEvidence(status, "io_page_drift")) {
    profile.uncertainty_penalty = SaturatingAdd(profile.uncertainty_penalty, 500);
  }
  if (HasEvidence(status, "memory_undergrant") || HasEvidence(status, "spill_observed")) {
    profile.uncertainty_penalty = SaturatingAdd(profile.uncertainty_penalty, 250);
  }
  return profile;
}

OptimizerFeedbackStatus OptimizerRuntimeFeedbackStore::Record(
    OptimizerRuntimeFeedbackRecord record) {
  OptimizerFeedbackStatus status;
  if (record.feedback_uuid.empty() || record.scope_uuid.empty() ||
      record.route_label.empty() || record.feedback_generation == 0 ||
      record.policy_generation == 0 || record.catalog_epoch == 0 ||
      record.security_epoch == 0) {
    status.ok = false;
    status.applied = false;
    status.diagnostic_code = "SB_OPTIMIZER_FEEDBACK_PERSISTENCE.MISSING_SCOPE";
    AddEvidence(&status, "runtime_feedback_persistence.refused=missing_scope_or_epoch");
    return status;
  }
  status = EvaluateOptimizerRuntimeFeedback(record.feedback);
  if (!status.ok || !status.applied) {
    status.evidence.push_back("runtime_feedback_persistence.refused=feedback_not_accepted");
    return status;
  }
  record.status = status;
  record.valid = true;
  record.evidence.push_back("runtime_feedback_persistence.recorded=true");
  record.evidence.push_back("runtime_feedback_persistence.scope_uuid=" +
                            record.scope_uuid);
  record.evidence.push_back("runtime_feedback_persistence.route_label=" +
                            record.route_label);
  record.evidence.push_back("runtime_feedback_persistence.actual_rows=" +
                            std::to_string(record.feedback.actual_rows));
  record.evidence.push_back("runtime_feedback_persistence.actual_rows_examined=" +
                            std::to_string(record.feedback.actual_rows_examined));
  record.evidence.push_back("runtime_feedback_persistence.actual_rows_filtered=" +
                            std::to_string(record.feedback.actual_rows_filtered));
  record.evidence.push_back("runtime_feedback_persistence.loop_count=" +
                            std::to_string(record.feedback.loop_count));
  record.evidence.push_back("runtime_feedback_persistence.actual_spill_bytes=" +
                            std::to_string(record.feedback.actual_spill_bytes));
  record.evidence.push_back("runtime_feedback_persistence.memory_grant_bytes=" +
                            std::to_string(record.feedback.memory_grant_bytes));
  record.evidence.push_back("runtime_feedback_persistence.peak_memory_bytes=" +
                            std::to_string(record.feedback.peak_memory_bytes));
  record.evidence.push_back(
      "runtime_feedback_persistence.authority_scope=advisory_only_not_transaction_finality_visibility_security_recovery_parser_or_reference_authority");
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
  status.evidence.push_back("runtime_feedback_persistence.recorded=true");
  status.evidence.push_back("runtime_feedback_persistence.invalidatable=true");
  status.evidence.push_back(
      "runtime_feedback_persistence.authority_scope=advisory_only_not_transaction_finality_visibility_security_recovery_parser_or_reference_authority");
  return status;
}

std::uint64_t OptimizerRuntimeFeedbackStore::Invalidate(
    const OptimizerRuntimeFeedbackInvalidation& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::uint64_t invalidated = 0;
  for (auto& record : records_) {
    const bool scope_matches = event.scope_uuid.empty() ||
                               event.scope_uuid == record.scope_uuid;
    const bool policy_newer = event.policy_generation != 0 &&
                              event.policy_generation != record.policy_generation;
    const bool catalog_newer = event.catalog_epoch != 0 &&
                               event.catalog_epoch != record.catalog_epoch;
    const bool security_newer = event.security_epoch != 0 &&
                                event.security_epoch != record.security_epoch;
    if (!record.valid || !scope_matches ||
        (!policy_newer && !catalog_newer && !security_newer)) {
      continue;
    }
    record.valid = false;
    record.invalidation_reason =
        event.reason.empty() ? "epoch_or_scope_invalidation" : event.reason;
    record.evidence.push_back("runtime_feedback_persistence.invalidated=true");
    record.evidence.push_back("runtime_feedback_persistence.invalidation_reason=" +
                              record.invalidation_reason);
    ++invalidated;
  }
  return invalidated;
}

OptimizerRuntimeFeedbackSnapshot OptimizerRuntimeFeedbackStore::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  OptimizerRuntimeFeedbackSnapshot snapshot;
  snapshot.records = records_;
  snapshot.total_records = records_.size();
  for (const auto& record : records_) {
    if (record.valid) {
      ++snapshot.valid_records;
    } else {
      ++snapshot.invalidated_records;
    }
  }
  return snapshot;
}

std::optional<OptimizerRuntimeFeedbackRecord> OptimizerRuntimeFeedbackStore::Find(
    const std::string& feedback_uuid) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(records_.begin(), records_.end(), [&](const auto& record) {
    return record.feedback_uuid == feedback_uuid;
  });
  if (it == records_.end()) return std::nullopt;
  return *it;
}

}  // namespace scratchbird::engine::optimizer
