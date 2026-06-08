// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "adaptive_batch_controller.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

constexpr std::uint64_t kPressurePpm = 900;
constexpr std::uint64_t kQuotaOverrunPpm = 1000;

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

void AddEvidence(AdaptiveBatchControllerResult* result, std::string evidence) {
  result->evidence.push_back(std::move(evidence));
}

void AddDiagnostic(AdaptiveBatchControllerResult* result,
                   std::string diagnostic) {
  result->diagnostics.push_back(std::move(diagnostic));
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

bool LessOrEqualRatio(std::uint64_t observed,
                      std::uint64_t budget,
                      std::uint64_t numerator,
                      std::uint64_t denominator) {
  if (budget == 0 || denominator == 0) {
    return false;
  }
  return SaturatingMul(observed, denominator) <= SaturatingMul(budget, numerator);
}

AdaptiveBatchControllerResult Refuse(const AdaptiveBatchControllerRequest& request,
                                     std::string code,
                                     std::string detail) {
  AdaptiveBatchControllerResult result;
  result.ok = false;
  result.fail_closed = true;
  result.benchmark_clean_evidence = request.benchmark_clean_input_evidence;
  result.selected_batch_size = request.lower_bound;
  result.action = AdaptiveBatchActionClass::kRefuse;
  result.diagnostic_code = std::move(code);
  AddDiagnostic(&result, result.diagnostic_code + ":" + detail);
  AddEvidence(&result, "adaptive_batch_controller=odf100_v1");
  AddEvidence(&result, "decision=refuse");
  AddEvidence(&result, "fail_closed=true");
  AddEvidence(&result, "advisory_only=true");
  AddEvidence(&result, "resource_governance_only=true");
  AddEvidence(&result, "mga_finality_authority=engine_transaction_inventory");
  AddEvidence(&result, "parser_executes_sql=false");
  AddEvidence(&result, "provider_transaction_finality_authority=false");
  AddEvidence(&result, "provider_visibility_authority=false");
  AddEvidence(&result, "client_autocommit_authority=false");
  AddEvidence(&result, "wal_recovery_authority=false");
  AddEvidence(&result,
              std::string("benchmark_clean=") +
                  BoolText(result.benchmark_clean_evidence));
  AddEvidence(&result, std::move(detail));
  return result;
}

bool IsKnownFamily(AdaptiveBatchFamily family) {
  switch (family) {
    case AdaptiveBatchFamily::kInsert:
    case AdaptiveBatchFamily::kCopy:
    case AdaptiveBatchFamily::kSegmentVector:
    case AdaptiveBatchFamily::kBucketTimeSeries:
    case AdaptiveBatchFamily::kGraphFrontier:
    case AdaptiveBatchFamily::kIndexMerge:
      return true;
    case AdaptiveBatchFamily::kUnknown:
      return false;
  }
  return false;
}

bool FamilyLabelMatches(AdaptiveBatchFamily family, const std::string& label) {
  return label == AdaptiveBatchFamilyName(family);
}

bool AuthoritySafe(
    const scratchbird::core::agents::AdaptiveBatchPolicyEvidence& evidence) {
  return evidence.engine_mga_authoritative &&
         evidence.security_snapshot_bound &&
         evidence.grants_proven &&
         evidence.mga_recheck_required &&
         evidence.security_recheck_required &&
         !evidence.parser_or_donor_authority &&
         !evidence.provider_transaction_finality_authority &&
         !evidence.provider_visibility_authority &&
         !evidence.client_autocommit_authority &&
         !evidence.wal_recovery_authority;
}

bool EvidenceFresh(
    const scratchbird::core::agents::AdaptiveBatchPolicyEvidence& evidence) {
  if (evidence.required_epoch == 0 || evidence.evidence_epoch == 0 ||
      evidence.evidence_epoch < evidence.required_epoch) {
    return false;
  }
  return evidence.max_evidence_age_microseconds == 0 ||
         evidence.evidence_age_microseconds <=
             evidence.max_evidence_age_microseconds;
}

AdaptiveBatchBudgetEvidence BuildBudget(
    const AdaptiveBatchControllerRequest& request) {
  AdaptiveBatchBudgetEvidence budget;
  budget.latency_budget_microseconds = request.latency_budget_microseconds;
  budget.observed_latency_microseconds = request.observed_latency_microseconds;
  budget.memory_budget_bytes = request.memory_budget_bytes;
  budget.observed_memory_bytes = request.observed_memory_bytes;
  budget.backlog_units = request.agent_evidence.backlog_units;
  budget.backlog_budget_units = request.agent_evidence.backlog_budget_units;
  budget.worker_pressure_ppm = request.agent_evidence.worker_pressure_ppm;
  budget.quota_pressure_ppm = request.agent_evidence.quota_pressure_ppm;
  budget.latency_over_budget =
      request.observed_latency_microseconds >
      request.latency_budget_microseconds;
  budget.memory_over_budget =
      request.observed_memory_bytes > request.memory_budget_bytes;
  budget.backlog_over_budget =
      request.agent_evidence.backlog_budget_units != 0 &&
      request.agent_evidence.backlog_units >
          request.agent_evidence.backlog_budget_units;
  budget.worker_pressure_high =
      request.agent_evidence.worker_pressure_ppm >= kPressurePpm;
  budget.quota_pressure_high =
      request.agent_evidence.quota_pressure_ppm >= kPressurePpm;
  return budget;
}

void AddBudgetEvidence(AdaptiveBatchControllerResult* result) {
  AddEvidence(result,
              "latency_budget_microseconds=" +
                  std::to_string(result->budget.latency_budget_microseconds));
  AddEvidence(result,
              "observed_latency_microseconds=" +
                  std::to_string(result->budget.observed_latency_microseconds));
  AddEvidence(result,
              "memory_budget_bytes=" +
                  std::to_string(result->budget.memory_budget_bytes));
  AddEvidence(result,
              "observed_memory_bytes=" +
                  std::to_string(result->budget.observed_memory_bytes));
  AddEvidence(result,
              "backlog_units=" + std::to_string(result->budget.backlog_units));
  AddEvidence(result,
              "backlog_budget_units=" +
                  std::to_string(result->budget.backlog_budget_units));
  AddEvidence(result,
              "worker_pressure_ppm=" +
                  std::to_string(result->budget.worker_pressure_ppm));
  AddEvidence(result,
              "quota_pressure_ppm=" +
                  std::to_string(result->budget.quota_pressure_ppm));
  AddEvidence(result,
              "latency_over_budget=" +
                  BoolText(result->budget.latency_over_budget));
  AddEvidence(result,
              "memory_over_budget=" +
                  BoolText(result->budget.memory_over_budget));
  AddEvidence(result,
              "backlog_over_budget=" +
                  BoolText(result->budget.backlog_over_budget));
}

std::uint64_t DecreasedBatch(const AdaptiveBatchControllerRequest& request) {
  const std::uint64_t halved = std::max<std::uint64_t>(1, request.current_batch_size / 2);
  return std::clamp(halved, request.lower_bound, request.upper_bound);
}

std::uint64_t IncreasedBatch(const AdaptiveBatchControllerRequest& request) {
  const std::uint64_t step =
      std::max<std::uint64_t>(1, request.current_batch_size / 4);
  return std::clamp(SaturatingAdd(request.current_batch_size, step),
                    request.lower_bound,
                    request.upper_bound);
}

}  // namespace

const char* AdaptiveBatchFamilyName(AdaptiveBatchFamily family) {
  switch (family) {
    case AdaptiveBatchFamily::kInsert:
      return "insert";
    case AdaptiveBatchFamily::kCopy:
      return "copy";
    case AdaptiveBatchFamily::kSegmentVector:
      return "segment_vector";
    case AdaptiveBatchFamily::kBucketTimeSeries:
      return "bucket_time_series";
    case AdaptiveBatchFamily::kGraphFrontier:
      return "graph_frontier";
    case AdaptiveBatchFamily::kIndexMerge:
      return "index_merge";
    case AdaptiveBatchFamily::kUnknown:
      return "unknown";
  }
  return "unknown";
}

const char* AdaptiveBatchActionClassName(AdaptiveBatchActionClass action) {
  switch (action) {
    case AdaptiveBatchActionClass::kRefuse:
      return "refuse";
    case AdaptiveBatchActionClass::kHold:
      return "hold";
    case AdaptiveBatchActionClass::kIncrease:
      return "increase";
    case AdaptiveBatchActionClass::kDecrease:
      return "decrease";
  }
  return "refuse";
}

AdaptiveBatchControllerResult EvaluateAdaptiveBatchController(
    const AdaptiveBatchControllerRequest& request) {
  if (!IsKnownFamily(request.family)) {
    return Refuse(request,
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.UNKNOWN_FAMILY",
                  "unknown_batch_family");
  }
  if (request.family_label.empty() ||
      !FamilyLabelMatches(request.family, request.family_label) ||
      request.agent_evidence.family_label != request.family_label) {
    return Refuse(request,
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.MISSING_LABELS",
                  "missing_or_mismatched_family_label");
  }
  if (request.lower_bound == 0 || request.upper_bound == 0 ||
      request.lower_bound > request.upper_bound) {
    return Refuse(request,
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.INVALID_BOUNDS",
                  "invalid_bounds");
  }
  if (request.current_batch_size < request.lower_bound ||
      request.current_batch_size > request.upper_bound) {
    return Refuse(request,
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.CURRENT_OUTSIDE_BOUNDS",
                  "current_outside_bounds");
  }
  if (request.latency_budget_microseconds == 0 ||
      request.memory_budget_bytes == 0) {
    return Refuse(request,
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.INVALID_BUDGET",
                  "latency_and_memory_budgets_required");
  }
  if (!request.benchmark_clean_input_evidence) {
    return Refuse(request,
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.BENCHMARK_DIRTY",
                  "benchmark_clean_evidence_required");
  }
  if (!request.agent_evidence.policy_allowed ||
      !request.agent_evidence.evidence_present) {
    return Refuse(request,
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.POLICY_REFUSED",
                  "policy_or_agent_evidence_missing");
  }
  if (!request.agent_evidence.evidence_authoritative ||
      !EvidenceFresh(request.agent_evidence)) {
    return Refuse(request,
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.STALE_AGENT_EVIDENCE",
                  "agent_evidence_not_authoritative_or_fresh");
  }
  if (!AuthoritySafe(request.agent_evidence)) {
    return Refuse(request,
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.UNSAFE_AUTHORITY",
                  "mga_security_recheck_and_engine_authority_required");
  }
  if (request.agent_evidence.quota_pressure_ppm > kQuotaOverrunPpm) {
    return Refuse(request,
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.QUOTA_OVERRUN",
                  "quota_pressure_overrun");
  }
  if (request.agent_evidence.hard_backlog_pressure) {
    return Refuse(request,
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.HARD_BACKLOG_PRESSURE",
                  "hard_backlog_pressure");
  }

  AdaptiveBatchControllerResult result;
  result.ok = true;
  result.fail_closed = false;
  result.benchmark_clean_evidence = true;
  result.budget = BuildBudget(request);
  result.selected_batch_size = request.current_batch_size;
  result.action = AdaptiveBatchActionClass::kHold;
  result.diagnostic_code = "SB_OPTIMIZER_ADAPTIVE_BATCH.HOLD";

  const bool error_pressure =
      request.history.consecutive_error_count > 0 ||
      request.history.error_count > request.history.success_count;
  const bool resource_pressure =
      result.budget.latency_over_budget ||
      result.budget.memory_over_budget ||
      result.budget.worker_pressure_high ||
      result.budget.quota_pressure_high ||
      error_pressure;
  const bool healthy_for_growth =
      LessOrEqualRatio(request.observed_latency_microseconds,
                       request.latency_budget_microseconds,
                       7,
                       10) &&
      LessOrEqualRatio(request.observed_memory_bytes,
                       request.memory_budget_bytes,
                       7,
                       10) &&
      request.agent_evidence.worker_pressure_ppm < kPressurePpm &&
      request.agent_evidence.quota_pressure_ppm < kPressurePpm &&
      request.history.consecutive_error_count == 0;

  result.throttle_recommended = resource_pressure;
  if (resource_pressure && request.agent_evidence.throttle_allowed) {
    result.selected_batch_size = DecreasedBatch(request);
    result.action = AdaptiveBatchActionClass::kDecrease;
    result.diagnostic_code = "SB_OPTIMIZER_ADAPTIVE_BATCH.DECREASE";
  } else if (healthy_for_growth &&
             (result.budget.backlog_over_budget ||
              request.history.consecutive_success_count >= 3)) {
    result.selected_batch_size = IncreasedBatch(request);
    result.action = AdaptiveBatchActionClass::kIncrease;
    result.diagnostic_code = "SB_OPTIMIZER_ADAPTIVE_BATCH.INCREASE";
  }

  AddEvidence(&result, "adaptive_batch_controller=odf100_v1");
  AddEvidence(&result,
              std::string("family=") + AdaptiveBatchFamilyName(request.family));
  AddEvidence(&result,
              std::string("action=") +
                  AdaptiveBatchActionClassName(result.action));
  AddEvidence(&result,
              "selected_batch_size=" +
                  std::to_string(result.selected_batch_size));
  AddEvidence(&result, "fail_closed=false");
  AddEvidence(&result, "advisory_only=true");
  AddEvidence(&result, "resource_governance_only=true");
  AddEvidence(&result, "benchmark_clean=true");
  AddEvidence(&result, "odf038_summary_counters_compatible=true");
  AddEvidence(&result, "odf079_backpressure_debt_compatible=true");
  AddEvidence(&result, "mga_recheck_evidence=required");
  AddEvidence(&result, "security_recheck_evidence=required");
  AddEvidence(&result, "mga_finality_authority=engine_transaction_inventory");
  AddEvidence(&result, "parser_executes_sql=false");
  AddEvidence(&result, "provider_transaction_finality_authority=false");
  AddEvidence(&result, "provider_visibility_authority=false");
  AddEvidence(&result, "client_autocommit_authority=false");
  AddEvidence(&result, "wal_recovery_authority=false");
  AddBudgetEvidence(&result);
  return result;
}

std::string SerializeAdaptiveBatchControllerEvidence(
    const AdaptiveBatchControllerResult& result) {
  std::ostringstream out;
  out << "ok=" << BoolText(result.ok) << '\n';
  out << "fail_closed=" << BoolText(result.fail_closed) << '\n';
  out << "benchmark_clean=" << BoolText(result.benchmark_clean_evidence)
      << '\n';
  out << "advisory_only=" << BoolText(result.advisory_only) << '\n';
  out << "action=" << AdaptiveBatchActionClassName(result.action) << '\n';
  out << "selected_batch_size=" << result.selected_batch_size << '\n';
  out << "diagnostic_code=" << result.diagnostic_code << '\n';
  for (const auto& evidence : result.evidence) {
    out << "evidence=" << evidence << '\n';
  }
  for (const auto& diagnostic : result.diagnostics) {
    out << "diagnostic=" << diagnostic << '\n';
  }
  return out.str();
}

}  // namespace scratchbird::engine::optimizer
