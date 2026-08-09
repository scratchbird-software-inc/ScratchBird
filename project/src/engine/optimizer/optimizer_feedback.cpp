// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_feedback.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <ranges>
#include <sstream>
#include <tuple>
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

namespace {

constexpr std::uint32_t kBasisPoints = 10000;
constexpr std::uint32_t kMaximumPolicySamples = 64;
constexpr std::uint32_t kMaximumPolicyIdentities = 1024;
constexpr std::uint32_t kMaximumPolicyQuarantine = 4096;
constexpr std::uint32_t kMaximumPolicyBasisPoints = 1000000;
constexpr std::size_t kMaximumIdentityTokenBytes = 256;

void AppendGovernedEvidence(OptimizerGovernedFeedbackPublicationResult* result,
                            std::string evidence) {
  if (result != nullptr) result->evidence.push_back(std::move(evidence));
}

void AppendGovernedEvidence(OptimizerGovernedFeedbackConsumptionResult* result,
                            std::string evidence) {
  if (result != nullptr) result->evidence.push_back(std::move(evidence));
}

bool IsCanonicalUuid(const std::string& value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) return false;
  }
  return value != "00000000-0000-0000-0000-000000000000";
}

bool IsIdentityToken(const std::string& value) {
  if (value.empty() || value.size() > kMaximumIdentityTokenBytes) return false;
  return std::none_of(value.begin(), value.end(), [](const unsigned char ch) {
    return std::iscntrl(ch) != 0;
  });
}

void AppendIdentityToken(std::ostringstream* out, const std::string& value) {
  *out << value.size() << ':' << value << '|';
}

std::string GovernedIdentityKey(
    const OptimizerGovernedFeedbackIdentity& identity) {
  if (identity.selected_plan_uuid.size() != 36 ||
      !IsIdentityToken(identity.selected_plan_signature) ||
      !IsIdentityToken(identity.implementation_id) ||
      !IsIdentityToken(identity.selected_alternative_uuid) ||
      !IsIdentityToken(identity.output_descriptor_digest) ||
      !IsIdentityToken(identity.result_identity_digest) ||
      !IsIdentityToken(identity.dependency_signature)) {
    return "invalid_identity";
  }
  std::ostringstream out;
  AppendIdentityToken(&out, identity.selected_plan_uuid);
  AppendIdentityToken(&out, identity.selected_plan_signature);
  out << identity.physical_node_id << '|' << identity.logical_node_id << '|'
      << identity.causal_counter_id << '|';
  AppendIdentityToken(&out, identity.implementation_id);
  AppendIdentityToken(&out, identity.selected_alternative_uuid);
  AppendIdentityToken(&out, identity.output_descriptor_digest);
  AppendIdentityToken(&out, identity.result_identity_digest);
  AppendIdentityToken(&out, identity.dependency_signature);
  return out.str();
}

bool GovernedIdentityValid(
    const OptimizerGovernedFeedbackIdentity& identity) {
  return identity.abi_version == 1 &&
         IsCanonicalUuid(identity.observation_uuid) &&
         IsCanonicalUuid(identity.selected_plan_uuid) &&
         identity.physical_node_id != 0 && identity.logical_node_id != 0 &&
         identity.causal_counter_id != 0 &&
         IsIdentityToken(identity.selected_plan_signature) &&
         IsIdentityToken(identity.implementation_id) &&
         IsCanonicalUuid(identity.selected_alternative_uuid) &&
         IsIdentityToken(identity.output_descriptor_digest) &&
         IsIdentityToken(identity.result_identity_digest) &&
         IsIdentityToken(identity.dependency_signature);
}

bool GovernedIdentityCompatible(
    const OptimizerGovernedFeedbackIdentity& left,
    const OptimizerGovernedFeedbackIdentity& right) {
  return GovernedIdentityKey(left) == GovernedIdentityKey(right);
}

bool GenerationsValid(const OptimizerGovernedFeedbackGenerations& value) {
  return value.catalog != 0 && value.security != 0 && value.policy != 0 &&
         value.statistics != 0 && value.capability != 0 && value.route != 0 &&
         value.resource != 0 && value.feedback != 0;
}

bool PolicyValid(const OptimizerGovernedFeedbackPolicy& policy) {
  return policy.deviation_threshold_basis_points != 0 &&
         policy.deviation_threshold_basis_points <=
             kMaximumPolicyBasisPoints &&
         policy.required_compatible_samples >= 2 &&
         policy.required_compatible_samples <=
             policy.maximum_samples_per_identity &&
         policy.maximum_samples_per_identity <= kMaximumPolicySamples &&
         policy.maximum_retained_identities != 0 &&
         policy.maximum_retained_identities <= kMaximumPolicyIdentities &&
         policy.maximum_quarantine_records != 0 &&
         policy.maximum_quarantine_records <= kMaximumPolicyQuarantine &&
         policy.minimum_multiplier_basis_points != 0 &&
         policy.minimum_multiplier_basis_points <= kBasisPoints &&
         policy.maximum_multiplier_basis_points >= kBasisPoints &&
         policy.maximum_multiplier_basis_points <=
             kMaximumPolicyBasisPoints &&
         policy.minimum_multiplier_basis_points <=
             policy.maximum_multiplier_basis_points &&
         policy.maximum_advisory_rows != 0 &&
         policy.maximum_advisory_memory_bytes != 0;
}

bool HasForbiddenAuthority(
    const OptimizerGovernedFeedbackAuthorityReceipt& authority) {
  return authority.owns_execution || authority.owns_mga_visibility ||
         authority.owns_transaction_finality || authority.owns_security ||
         authority.owns_parser || authority.owns_reference ||
         authority.owns_donor || authority.owns_recovery ||
         authority.owns_wal || authority.owns_benchmark ||
         authority.owns_release || authority.owns_cluster;
}

bool AuthorityValid(
    const OptimizerGovernedFeedbackAuthorityReceipt& authority) {
  return authority.engine_execution_observation &&
         authority.producer_receipt_complete &&
         authority.counters_frozen_after_finish &&
         authority.estimates_frozen_before_access &&
         authority.plan_identity_verified &&
         authority.node_causal_identity_verified &&
         authority.implementation_alternative_identity_verified &&
         authority.descriptor_identity_verified &&
         authority.result_identity_verified &&
         authority.dependency_identity_verified &&
         authority.mga_statement_context_verified &&
         authority.security_recheck_preserved &&
         authority.donor_semantics_preserved &&
         !HasForbiddenAuthority(authority);
}

bool MetricValid(const OptimizerGovernedFeedbackMetric& metric) {
  if (metric.state == OptimizerGovernedFeedbackMetricState::kUnavailable) {
    return false;
  }
  return metric.state == OptimizerGovernedFeedbackMetricState::kObserved ||
         (metric.state ==
              OptimizerGovernedFeedbackMetricState::kNotApplicable &&
          metric.value == 0);
}

bool CheckedAdd(const std::uint64_t left, const std::uint64_t right,
                std::uint64_t* result) {
  if (result == nullptr || right > std::numeric_limits<std::uint64_t>::max() -
                                      left) {
    return false;
  }
  *result = left + right;
  return true;
}

std::uint64_t SaturatingSum(const std::uint64_t left,
                            const std::uint64_t right) {
  std::uint64_t result = 0;
  return CheckedAdd(left, right, &result)
             ? result
             : std::numeric_limits<std::uint64_t>::max();
}

std::uint64_t ScaledRatio(const std::uint64_t numerator,
                          const std::uint64_t denominator,
                          const std::uint64_t scale) {
  const auto divisor = std::max<std::uint64_t>(1, denominator);
  const auto quotient = numerator / divisor;
  const auto remainder = numerator % divisor;
  const auto scaled_quotient = SaturatingMul(quotient, scale);
  const auto scaled_remainder = SaturatingMul(remainder, scale) / divisor;
  return SaturatingSum(scaled_quotient, scaled_remainder);
}

std::uint64_t DeviationBasisPoints(const std::uint64_t estimated,
                                   const std::uint64_t actual) {
  const auto difference = estimated >= actual ? estimated - actual
                                               : actual - estimated;
  return ScaledRatio(difference, std::max<std::uint64_t>(1, estimated),
                     kBasisPoints);
}

std::uint32_t Direction(const std::uint64_t estimated,
                        const std::uint64_t actual) {
  if (actual > estimated) return 1;
  if (actual < estimated) return 2;
  return 0;
}

bool MetricsValid(const OptimizerGovernedFeedbackMetrics& metrics,
                  std::string* failure) {
  const std::vector<const OptimizerGovernedFeedbackMetric*> all_metrics = {
      &metrics.estimated_input_rows,
      &metrics.estimated_output_rows,
      &metrics.actual_input_rows,
      &metrics.actual_output_rows,
      &metrics.actual_rows_examined,
      &metrics.elapsed_ns,
      &metrics.operator_wait_ns,
      &metrics.current_memory_bytes,
      &metrics.peak_memory_bytes,
      &metrics.estimated_pages,
      &metrics.decoded_bytes,
      &metrics.bytes_read,
      &metrics.bytes_written,
      &metrics.pages_read,
      &metrics.pages_written,
      &metrics.spill_bytes_read,
      &metrics.spill_bytes_written,
      &metrics.visibility_rechecks,
      &metrics.security_rechecks,
      &metrics.storage_rechecks,
      &metrics.index_rechecks,
      &metrics.residual_rechecks,
      &metrics.compatibility_rechecks,
      &metrics.archive_bytes_read,
      &metrics.cluster_bytes_sent,
      &metrics.cluster_bytes_received,
      &metrics.estimated_resource_units,
      &metrics.actual_resource_units,
  };
  if (!std::all_of(all_metrics.begin(), all_metrics.end(), [](const auto* metric) {
        return MetricValid(*metric);
      })) {
    if (failure != nullptr) *failure = "metric_state";
    return false;
  }
  const std::vector<const OptimizerGovernedFeedbackMetric*> required_observed = {
      &metrics.estimated_input_rows,
      &metrics.estimated_output_rows,
      &metrics.actual_input_rows,
      &metrics.actual_output_rows,
      &metrics.actual_rows_examined,
      &metrics.elapsed_ns,
      &metrics.operator_wait_ns,
      &metrics.current_memory_bytes,
      &metrics.peak_memory_bytes,
      &metrics.estimated_resource_units,
      &metrics.actual_resource_units,
  };
  if (!std::all_of(required_observed.begin(), required_observed.end(),
                   [](const auto* metric) {
                     return metric->state ==
                            OptimizerGovernedFeedbackMetricState::kObserved;
                   })) {
    if (failure != nullptr) *failure = "metric_state";
    return false;
  }
  if (metrics.operator_wait_ns.value > metrics.elapsed_ns.value ||
      metrics.current_memory_bytes.value > metrics.peak_memory_bytes.value) {
    if (failure != nullptr) *failure = "metric_relationship";
    return false;
  }
  std::uint64_t ignored = 0;
  if (!CheckedAdd(metrics.decoded_bytes.value, metrics.bytes_read.value,
                  &ignored) ||
      !CheckedAdd(ignored, metrics.bytes_written.value, &ignored) ||
      !CheckedAdd(metrics.pages_read.value, metrics.pages_written.value,
                  &ignored) ||
      !CheckedAdd(metrics.spill_bytes_read.value,
                  metrics.spill_bytes_written.value, &ignored) ||
      !CheckedAdd(metrics.cluster_bytes_sent.value,
                  metrics.cluster_bytes_received.value, &ignored)) {
    if (failure != nullptr) *failure = "metric_overflow";
    return false;
  }
  return true;
}

std::string OutcomeDiagnostic(const OptimizerGovernedFeedbackOutcome outcome) {
  switch (outcome) {
    case OptimizerGovernedFeedbackOutcome::kSucceeded:
      return {};
    case OptimizerGovernedFeedbackOutcome::kFailed:
      return "QOW-DIAG-OPT-013-EXECUTION-FAILED-V1";
    case OptimizerGovernedFeedbackOutcome::kCrashed:
      return "QOW-DIAG-OPT-013-EXECUTION-CRASHED-V1";
    case OptimizerGovernedFeedbackOutcome::kCancelled:
      return "QOW-DIAG-OPT-013-EXECUTION-CANCELLED-V1";
    case OptimizerGovernedFeedbackOutcome::kRolledBack:
      return "QOW-DIAG-OPT-013-EXECUTION-ROLLED-BACK-V1";
    case OptimizerGovernedFeedbackOutcome::kUnauthorized:
      return "QOW-DIAG-OPT-013-UNAUTHORIZED-V1";
    case OptimizerGovernedFeedbackOutcome::kRedacted:
      return "QOW-DIAG-OPT-013-REDACTED-V1";
    case OptimizerGovernedFeedbackOutcome::kCorrupt:
      return "QOW-DIAG-OPT-013-CORRUPT-V1";
    case OptimizerGovernedFeedbackOutcome::kPoisoned:
      return "QOW-DIAG-OPT-013-POISONED-V1";
    case OptimizerGovernedFeedbackOutcome::kPolicyQuarantined:
      return "QOW-DIAG-OPT-013-POLICY-QUARANTINED-V1";
  }
  return "QOW-DIAG-OPT-013-OUTCOME-UNKNOWN-V1";
}

OptimizerGovernedFeedbackDisposition OutcomeDisposition(
    const OptimizerGovernedFeedbackOutcome outcome) {
  if (outcome == OptimizerGovernedFeedbackOutcome::kUnauthorized ||
      outcome == OptimizerGovernedFeedbackOutcome::kRedacted) {
    return OptimizerGovernedFeedbackDisposition::kDiagnosticOnly;
  }
  return OptimizerGovernedFeedbackDisposition::kQuarantined;
}

bool OutcomePoisonsCompatibleRecord(
    const OptimizerGovernedFeedbackOutcome outcome) {
  return outcome == OptimizerGovernedFeedbackOutcome::kCorrupt ||
         outcome == OptimizerGovernedFeedbackOutcome::kPoisoned ||
         outcome == OptimizerGovernedFeedbackOutcome::kPolicyQuarantined;
}

void BoundQuarantine(
    std::vector<OptimizerGovernedFeedbackQuarantineRecord>* quarantine,
    const std::uint32_t maximum) {
  std::ranges::sort(*quarantine, {}, [](const auto& record) {
    return std::tuple(record.observation_sequence, record.identity_key,
                      record.reason,
                      static_cast<std::uint8_t>(record.disposition));
  });
  if (quarantine->size() > maximum) {
    quarantine->erase(quarantine->begin(),
                      quarantine->begin() +
                          static_cast<std::ptrdiff_t>(quarantine->size() -
                                                      maximum));
  }
}

void RecomputeRecord(OptimizerGovernedFeedbackRecord* record,
                     const OptimizerGovernedFeedbackPolicy& policy) {
  std::ranges::sort(record->retained_samples, {},
                    &OptimizerGovernedFeedbackSample::observation_sequence);
  if (record->retained_samples.size() > policy.maximum_samples_per_identity) {
    record->retained_samples.erase(
        record->retained_samples.begin(),
        record->retained_samples.begin() +
            static_cast<std::ptrdiff_t>(record->retained_samples.size() -
                                        policy.maximum_samples_per_identity));
  }
  record->compatible_sample_count =
      static_cast<std::uint32_t>(record->retained_samples.size());
  record->first_observation_sequence =
      record->retained_samples.front().observation_sequence;
  record->last_observation_sequence =
      record->retained_samples.back().observation_sequence;
  record->identity.observation_uuid =
      record->retained_samples.back().observation_uuid;
  record->accumulated_actual_output_rows = 0;
  record->accumulated_peak_memory_bytes = 0;
  for (const auto& sample : record->retained_samples) {
    record->accumulated_actual_output_rows = SaturatingSum(
        record->accumulated_actual_output_rows, sample.actual_output_rows);
    record->accumulated_peak_memory_bytes = SaturatingSum(
        record->accumulated_peak_memory_bytes, sample.peak_memory_bytes);
  }
  const auto divisor = std::max<std::uint64_t>(1, record->compatible_sample_count);
  record->advisory_output_rows = std::min(
      record->accumulated_actual_output_rows / divisor,
      policy.maximum_advisory_rows);
  const auto average_peak = record->accumulated_peak_memory_bytes / divisor;
  record->advisory_memory_bytes = std::min(
      SaturatingSum(average_peak, average_peak / 4),
      policy.maximum_advisory_memory_bytes);
  const auto multiplier = ScaledRatio(record->advisory_output_rows,
                                      record->frozen_estimated_output_rows,
                                      kBasisPoints);
  record->cardinality_multiplier_basis_points =
      static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
          multiplier, policy.minimum_multiplier_basis_points,
          policy.maximum_multiplier_basis_points));
  record->disposition =
      record->compatible_sample_count >= policy.required_compatible_samples
          ? OptimizerGovernedFeedbackDisposition::kAdmitted
          : OptimizerGovernedFeedbackDisposition::kPending;
}

void BoundRecords(std::vector<OptimizerGovernedFeedbackRecord>* records,
                  const std::uint32_t maximum) {
  if (records->size() <= maximum) return;
  std::ranges::sort(*records, {}, [](const auto& record) {
    return std::tuple(record.last_observation_sequence,
                      GovernedIdentityKey(record.identity));
  });
  records->erase(records->begin(),
                 records->begin() +
                     static_cast<std::ptrdiff_t>(records->size() - maximum));
}

bool AnyGenerationDiffers(
    const OptimizerGovernedFeedbackGenerations& stored,
    const OptimizerGovernedFeedbackGenerations& current) {
  return (current.catalog != 0 && current.catalog != stored.catalog) ||
         (current.security != 0 && current.security != stored.security) ||
         (current.policy != 0 && current.policy != stored.policy) ||
         (current.statistics != 0 &&
          current.statistics != stored.statistics) ||
         (current.capability != 0 &&
          current.capability != stored.capability) ||
         (current.route != 0 && current.route != stored.route) ||
         (current.resource != 0 && current.resource != stored.resource) ||
         (current.feedback != 0 && current.feedback != stored.feedback);
}

bool GenerationsStrictlyAdvance(
    const OptimizerGovernedFeedbackGenerations& stored,
    const OptimizerGovernedFeedbackGenerations& current) {
  const bool never_moves_backward =
      current.catalog >= stored.catalog &&
      current.security >= stored.security &&
      current.policy >= stored.policy &&
      current.statistics >= stored.statistics &&
      current.capability >= stored.capability && current.route >= stored.route &&
      current.resource >= stored.resource &&
      current.feedback >= stored.feedback;
  return never_moves_backward && current != stored;
}

}  // namespace

// QOW-SOURCE-OPT-013-V1
OptimizerGovernedFeedbackPublicationResult
OptimizerGovernedFeedbackStore::Publish(
    const OptimizerGovernedFeedbackObservation& observation,
    const OptimizerGovernedFeedbackPolicy& policy) {
  OptimizerGovernedFeedbackPublicationResult result;
  result.identity_key = GovernedIdentityKey(observation.identity);
  result.feedback_generation = observation.generations.feedback;
  const auto refuse = [&](std::string diagnostic,
                          const OptimizerGovernedFeedbackDisposition disposition,
                          const bool poison_compatible) {
    result.accepted = false;
    result.disposition = disposition;
    result.diagnostic_code = std::move(diagnostic);
    governed_quarantine_.push_back(
        {result.identity_key, observation.observation_sequence,
         result.diagnostic_code, disposition});
    const auto quarantine_bound =
        governed_policy_.has_value()
            ? governed_policy_->maximum_quarantine_records
            : (PolicyValid(policy) ? policy.maximum_quarantine_records
                                   : kMaximumPolicyQuarantine);
    BoundQuarantine(&governed_quarantine_, quarantine_bound);
    if (poison_compatible) {
      auto found = std::ranges::find_if(governed_records_, [&](const auto& record) {
        return GovernedIdentityCompatible(record.identity, observation.identity) &&
               record.generations == observation.generations;
      });
      if (found != governed_records_.end()) {
        found->valid = false;
        found->disposition = OptimizerGovernedFeedbackDisposition::kQuarantined;
        found->invalidation_reason = result.diagnostic_code;
      }
    }
    AppendGovernedEvidence(&result, "feedback.publication=refused");
    AppendGovernedEvidence(&result, "feedback.authority=advisory_only");
    return result;
  };

  std::lock_guard<std::mutex> lock(governed_mutex_);
  if (!PolicyValid(policy)) {
    return refuse("QOW-DIAG-OPT-013-POLICY-BOUNDS-V1",
                  OptimizerGovernedFeedbackDisposition::kQuarantined, false);
  }
  if (!GovernedIdentityValid(observation.identity) ||
      observation.observation_sequence == 0) {
    return refuse("QOW-DIAG-OPT-013-IDENTITY-V1",
                  OptimizerGovernedFeedbackDisposition::kQuarantined, false);
  }
  if (!GenerationsValid(observation.generations)) {
    return refuse("QOW-DIAG-OPT-013-GENERATION-V1",
                  OptimizerGovernedFeedbackDisposition::kQuarantined, false);
  }
  if (!governed_policy_.has_value()) {
    governed_policy_ = policy;
  } else if (*governed_policy_ != policy) {
    const auto every_prior_record_terminal =
        std::ranges::all_of(governed_records_, [](const auto& record) {
          return !record.valid &&
                 (record.disposition ==
                      OptimizerGovernedFeedbackDisposition::kInvalidated ||
                  record.disposition ==
                      OptimizerGovernedFeedbackDisposition::kQuarantined);
        });
    std::uint64_t newest_prior_policy_generation = 0;
    for (const auto& record : governed_records_) {
      newest_prior_policy_generation =
          std::max(newest_prior_policy_generation, record.generations.policy);
    }
    if (!every_prior_record_terminal ||
        observation.generations.policy <= newest_prior_policy_generation) {
      return refuse("QOW-DIAG-OPT-013-POLICY-GENERATION-CONFLICT-V1",
                    OptimizerGovernedFeedbackDisposition::kQuarantined,
                    false);
    }
    governed_records_.clear();
    governed_policy_ = policy;
    BoundQuarantine(&governed_quarantine_,
                    policy.maximum_quarantine_records);
  }
  if (observation.outcome != OptimizerGovernedFeedbackOutcome::kSucceeded) {
    return refuse(OutcomeDiagnostic(observation.outcome),
                  OutcomeDisposition(observation.outcome),
                  OutcomePoisonsCompatibleRecord(observation.outcome));
  }
  if (!observation.policy_allowed) {
    return refuse("QOW-DIAG-OPT-013-POLICY-DISABLED-V1",
                  OptimizerGovernedFeedbackDisposition::kDiagnosticOnly,
                  false);
  }
  if (!AuthorityValid(observation.authority)) {
    return refuse(HasForbiddenAuthority(observation.authority)
                      ? "QOW-DIAG-OPT-013-FORBIDDEN-AUTHORITY-V1"
                      : "QOW-DIAG-OPT-013-UNFROZEN-OBSERVATION-V1",
                  OptimizerGovernedFeedbackDisposition::kQuarantined, true);
  }
  std::string metric_failure;
  if (!MetricsValid(observation.metrics, &metric_failure)) {
    return refuse(metric_failure == "metric_overflow"
                      ? "QOW-DIAG-OPT-013-METRIC-OVERFLOW-V1"
                      : metric_failure == "metric_relationship"
                            ? "QOW-DIAG-OPT-013-METRIC-CONTRADICTION-V1"
                            : "QOW-DIAG-OPT-013-METRIC-STATE-V1",
                  OptimizerGovernedFeedbackDisposition::kQuarantined, true);
  }

  auto found = std::ranges::find_if(governed_records_, [&](const auto& record) {
    return GovernedIdentityCompatible(record.identity, observation.identity);
  });
  if (found != governed_records_.end() &&
      found->generations != observation.generations) {
    const bool terminal =
        !found->valid &&
        (found->disposition ==
             OptimizerGovernedFeedbackDisposition::kInvalidated ||
         found->disposition ==
             OptimizerGovernedFeedbackDisposition::kQuarantined);
    if (!terminal ||
        !GenerationsStrictlyAdvance(found->generations,
                                    observation.generations)) {
      return refuse("QOW-DIAG-OPT-013-STALE-GENERATION-V1",
                    OptimizerGovernedFeedbackDisposition::kQuarantined,
                    false);
    }
    governed_records_.erase(found);
    found = governed_records_.end();
  }
  if (observation.regression_observed ||
      observation.correctness_constraint_conflict) {
    return refuse(observation.regression_observed
                      ? "QOW-DIAG-OPT-013-REGRESSION-QUARANTINED-V1"
                      : "QOW-DIAG-OPT-013-CORRECTNESS-CONFLICT-V1",
                  OptimizerGovernedFeedbackDisposition::kQuarantined, true);
  }

  const auto estimated = observation.metrics.estimated_output_rows.value;
  const auto actual = observation.metrics.actual_output_rows.value;
  const auto deviation = DeviationBasisPoints(estimated, actual);
  if (deviation < policy.deviation_threshold_basis_points ||
      !observation.quality_improvement_observed) {
    result.accepted = true;
    result.disposition = OptimizerGovernedFeedbackDisposition::kIgnored;
    result.diagnostic_code =
        deviation < policy.deviation_threshold_basis_points
            ? "QOW-DIAG-OPT-013-BELOW-THRESHOLD-V1"
            : "QOW-DIAG-OPT-013-NO-IMPROVEMENT-EVIDENCE-V1";
    AppendGovernedEvidence(&result, "feedback.publication=ignored");
    AppendGovernedEvidence(&result, "feedback.side_effect=false");
    return result;
  }

  const auto direction = Direction(estimated, actual);
  if (found == governed_records_.end()) {
    OptimizerGovernedFeedbackRecord record;
    record.identity = observation.identity;
    record.generations = observation.generations;
    record.direction = direction;
    record.frozen_estimated_output_rows = estimated;
    record.retained_samples.push_back(
        {observation.identity.observation_uuid,
         observation.observation_sequence, actual,
         observation.metrics.peak_memory_bytes.value});
    RecomputeRecord(&record, policy);
    governed_records_.push_back(std::move(record));
    BoundRecords(&governed_records_, policy.maximum_retained_identities);
    found = std::ranges::find_if(governed_records_, [&](const auto& record) {
      return GovernedIdentityCompatible(record.identity, observation.identity);
    });
    if (found == governed_records_.end()) {
      result.accepted = true;
      result.disposition = OptimizerGovernedFeedbackDisposition::kIgnored;
      result.diagnostic_code = "QOW-DIAG-OPT-013-RETENTION-EVICTED-V1";
      AppendGovernedEvidence(&result, "feedback.retention=deterministic_eviction");
      return result;
    }
  } else {
    if (!found->valid) {
      return refuse("QOW-DIAG-OPT-013-IDENTITY-QUARANTINED-V1",
                    OptimizerGovernedFeedbackDisposition::kQuarantined, false);
    }
    if (observation.observation_sequence <=
        found->last_observation_sequence) {
      return refuse("QOW-DIAG-OPT-013-NON-MONOTONIC-OBSERVATION-V1",
                    OptimizerGovernedFeedbackDisposition::kQuarantined,
                    false);
    }
    const auto duplicate = std::ranges::find_if(
        found->retained_samples, [&](const auto& sample) {
          return sample.observation_uuid == observation.identity.observation_uuid ||
                 sample.observation_sequence == observation.observation_sequence;
        });
    if (duplicate != found->retained_samples.end()) {
      return refuse("QOW-DIAG-OPT-013-DUPLICATE-OBSERVATION-V1",
                    OptimizerGovernedFeedbackDisposition::kQuarantined, false);
    }
    if (found->frozen_estimated_output_rows != estimated ||
        found->direction != direction) {
      return refuse("QOW-DIAG-OPT-013-INCOMPATIBLE-SAMPLE-V1",
                    OptimizerGovernedFeedbackDisposition::kQuarantined, true);
    }
    found->retained_samples.push_back(
        {observation.identity.observation_uuid,
         observation.observation_sequence, actual,
         observation.metrics.peak_memory_bytes.value});
    RecomputeRecord(&*found, policy);
  }

  result.accepted = true;
  result.disposition = found->disposition;
  result.diagnostic_code =
      found->disposition == OptimizerGovernedFeedbackDisposition::kAdmitted
          ? "QOW-OPT-013-GOVERNED-FEEDBACK-ADMITTED-V1"
          : "QOW-OPT-013-GOVERNED-FEEDBACK-PENDING-V1";
  result.compatible_sample_count = found->compatible_sample_count;
  result.cardinality_multiplier_basis_points =
      found->cardinality_multiplier_basis_points;
  result.advisory_output_rows = found->advisory_output_rows;
  result.advisory_memory_bytes = found->advisory_memory_bytes;
  AppendGovernedEvidence(&result, "feedback.publication=accepted");
  AppendGovernedEvidence(&result, "feedback.current_plan_mutation=false");
  AppendGovernedEvidence(&result, "feedback.current_result_mutation=false");
  AppendGovernedEvidence(&result, "feedback.current_actual_mutation=false");
  AppendGovernedEvidence(&result, "feedback.transaction_mutation=false");
  AppendGovernedEvidence(&result, "feedback.authority=advisory_only");
  return result;
}

OptimizerGovernedFeedbackConsumptionResult
OptimizerGovernedFeedbackStore::Consume(
    const OptimizerGovernedFeedbackConsumptionRequest& request) const {
  OptimizerGovernedFeedbackConsumptionResult result;
  const auto refuse = [&](std::string diagnostic) {
    result.consumed = false;
    result.conservative_fallback_required = true;
    result.diagnostic_code = std::move(diagnostic);
    AppendGovernedEvidence(&result, "feedback.consume=refused");
    AppendGovernedEvidence(&result, "feedback.fallback=conservative");
    return result;
  };

  std::lock_guard<std::mutex> lock(governed_mutex_);
  if (request.abi_version != 1 ||
      !GovernedIdentityValid(request.source_identity) ||
      !GenerationsValid(request.current_generations)) {
    return refuse("QOW-DIAG-OPT-013-CONSUME-IDENTITY-V1");
  }
  if (!request.later_planning_attempt || !request.policy_allowed ||
      request.planning_attempt_sequence == 0) {
    return refuse("QOW-DIAG-OPT-013-CONSUME-NOT-LATER-PLANNING-V1");
  }
  if (request.mutate_executed_plan_requested ||
      request.mutate_runtime_actual_requested ||
      request.mutate_result_requested ||
      request.mutate_transaction_requested ||
      request.force_execution_requested || request.retry_requested ||
      request.reprepare_requested || request.statistics_refresh_requested ||
      request.operator_replacement_requested ||
      request.feedback_authority_claimed) {
    return refuse("QOW-DIAG-OPT-013-CONSUME-FORBIDDEN-SIDE-EFFECT-V1");
  }
  const auto& proof = request.alternative;
  if (!IsCanonicalUuid(proof.candidate_alternative_uuid) ||
      !IsCanonicalUuid(proof.exact_fallback_alternative_uuid) ||
      !proof.identical_logical_semantics ||
      !proof.identical_output_descriptors ||
      !proof.identical_required_properties ||
      !proof.identical_delivered_properties ||
      !proof.identical_security_behavior || !proof.identical_mga_behavior ||
      !proof.identical_donor_compatibility || !proof.compatible_capability ||
      !proof.exact_fallback_available ||
      proof.exact_fallback_alternative_uuid !=
          request.source_identity.selected_alternative_uuid) {
    return refuse("QOW-DIAG-OPT-013-ALTERNATIVE-INCOMPATIBLE-V1");
  }

  const auto found = std::ranges::find_if(governed_records_, [&](const auto& record) {
    return GovernedIdentityCompatible(record.identity, request.source_identity);
  });
  if (found == governed_records_.end()) {
    return refuse("QOW-DIAG-OPT-013-CONSUME-NOT-FOUND-V1");
  }
  if (!found->valid ||
      found->disposition != OptimizerGovernedFeedbackDisposition::kAdmitted) {
    return refuse(found->disposition ==
                          OptimizerGovernedFeedbackDisposition::kInvalidated
                      ? "QOW-DIAG-OPT-013-CONSUME-INVALIDATED-V1"
                      : "QOW-DIAG-OPT-013-CONSUME-NOT-ADMITTED-V1");
  }
  if (found->generations != request.current_generations) {
    return refuse("QOW-DIAG-OPT-013-CONSUME-STALE-GENERATION-V1");
  }
  if (request.planning_attempt_sequence <= found->last_observation_sequence) {
    return refuse("QOW-DIAG-OPT-013-CONSUME-CURRENT-EXECUTION-V1");
  }

  result.consumed = true;
  result.conservative_fallback_required = false;
  result.diagnostic_code = "QOW-OPT-013-GOVERNED-FEEDBACK-CONSUMED-V1";
  result.feedback_generation = found->generations.feedback;
  result.cardinality_multiplier_basis_points =
      found->cardinality_multiplier_basis_points;
  result.advisory_output_rows = found->advisory_output_rows;
  result.advisory_memory_bytes = found->advisory_memory_bytes;
  AppendGovernedEvidence(&result, "feedback.consume=later_planning_only");
  AppendGovernedEvidence(&result, "feedback.alternative=prevalidated_equivalent");
  AppendGovernedEvidence(&result, "feedback.exact_fallback=available");
  AppendGovernedEvidence(&result, "feedback.authority=advisory_only");
  AppendGovernedEvidence(&result, "feedback.side_effect=false");
  return result;
}

std::uint64_t OptimizerGovernedFeedbackStore::Invalidate(
    const OptimizerGovernedFeedbackInvalidation& event) {
  std::lock_guard<std::mutex> lock(governed_mutex_);
  if (!GenerationsValid(event.current_generations) ||
      !IsCanonicalUuid(event.selected_plan_uuid) ||
      (!event.reason.empty() && !IsIdentityToken(event.reason))) {
    return 0;
  }
  std::uint64_t count = 0;
  for (auto& record : governed_records_) {
    if (!record.valid ||
        (!event.selected_plan_uuid.empty() &&
         event.selected_plan_uuid != record.identity.selected_plan_uuid) ||
        !AnyGenerationDiffers(record.generations,
                              event.current_generations)) {
      continue;
    }
    record.valid = false;
    record.disposition = OptimizerGovernedFeedbackDisposition::kInvalidated;
    record.invalidation_reason = event.reason.empty()
                                     ? "generation_changed"
                                     : event.reason;
    ++count;
  }
  return count;
}

OptimizerGovernedFeedbackSnapshot OptimizerGovernedFeedbackStore::Snapshot()
    const {
  std::lock_guard<std::mutex> lock(governed_mutex_);
  OptimizerGovernedFeedbackSnapshot snapshot;
  snapshot.records = governed_records_;
  snapshot.quarantine = governed_quarantine_;
  std::ranges::sort(snapshot.records, {}, [](const auto& record) {
    return GovernedIdentityKey(record.identity);
  });
  std::ranges::sort(snapshot.quarantine, {}, [](const auto& record) {
    return std::tuple(record.observation_sequence, record.identity_key,
                      record.reason,
                      static_cast<std::uint8_t>(record.disposition));
  });
  snapshot.retained_identity_count = snapshot.records.size();
  snapshot.quarantine_record_count = snapshot.quarantine.size();
  for (const auto& record : snapshot.records) {
    if (record.valid &&
        record.disposition == OptimizerGovernedFeedbackDisposition::kAdmitted) {
      ++snapshot.admitted_identity_count;
    }
    if (record.disposition ==
        OptimizerGovernedFeedbackDisposition::kInvalidated) {
      ++snapshot.invalidated_identity_count;
    }
    if (record.disposition ==
        OptimizerGovernedFeedbackDisposition::kQuarantined) {
      ++snapshot.quarantined_identity_count;
    }
  }
  return snapshot;
}

}  // namespace scratchbird::engine::optimizer
