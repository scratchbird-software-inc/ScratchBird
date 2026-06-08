// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "adaptive_tuning_controller.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <utility>

namespace scratchbird::core::agents {
namespace {

constexpr u64 kPressurePpm = 900;
constexpr u64 kQuotaOverrunPpm = 1000;

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

void AddEvidence(AdaptiveTuningControllerResult* result,
                 std::string evidence) {
  result->evidence.push_back(std::move(evidence));
}

void AddDiagnostic(AdaptiveTuningControllerResult* result,
                   std::string diagnostic) {
  result->diagnostics.push_back(std::move(diagnostic));
}

u64 SaturatingAdd(u64 left, u64 right) {
  if (right > std::numeric_limits<u64>::max() - left) {
    return std::numeric_limits<u64>::max();
  }
  return left + right;
}

u64 SaturatingMul(u64 left, u64 right) {
  if (left != 0 && right > std::numeric_limits<u64>::max() / left) {
    return std::numeric_limits<u64>::max();
  }
  return left * right;
}

bool LessOrEqualRatio(u64 observed, u64 budget, u64 numerator, u64 denominator) {
  if (budget == 0 || denominator == 0) {
    return false;
  }
  return SaturatingMul(observed, denominator) <=
         SaturatingMul(budget, numerator);
}

bool IsKnownKnob(AdaptiveTuningKnob knob) {
  switch (knob) {
    case AdaptiveTuningKnob::kPrefetchDepth:
    case AdaptiveTuningKnob::kMergeWorkers:
    case AdaptiveTuningKnob::kRefreshInterval:
    case AdaptiveTuningKnob::kCandidateBudget:
    case AdaptiveTuningKnob::kCachePartition:
    case AdaptiveTuningKnob::kEvidenceSampleRate:
    case AdaptiveTuningKnob::kVectorEfSearch:
    case AdaptiveTuningKnob::kVectorNprobe:
      return true;
    case AdaptiveTuningKnob::kUnknown:
      return false;
  }
  return false;
}

bool AuthoritySafe(const AdaptiveTuningSafetyPolicy& safety,
                   const scratchbird::core::metrics::AdaptiveTuningMetricEvidence&
                       metrics) {
  return safety.engine_mga_authoritative &&
         safety.security_snapshot_bound &&
         safety.grants_proven &&
         safety.mga_recheck_required &&
         safety.security_recheck_required &&
         !safety.parser_or_donor_authority &&
         !safety.provider_transaction_finality_authority &&
         !safety.provider_visibility_authority &&
         !safety.client_autocommit_authority &&
         !safety.wal_recovery_authority &&
         !metrics.parser_or_donor_authority &&
         !metrics.provider_transaction_finality_authority &&
         !metrics.provider_visibility_authority &&
         !metrics.client_autocommit_authority &&
         !metrics.wal_recovery_authority;
}

u64 SafeRefusalValue(const AdaptiveTuningControllerRequest& request) {
  if (request.min_value == 0 || request.max_value == 0 ||
      request.min_value > request.max_value) {
    return 0;
  }
  if (request.default_value >= request.min_value &&
      request.default_value <= request.max_value) {
    return request.default_value;
  }
  if (request.current_value >= request.min_value &&
      request.current_value <= request.max_value) {
    return request.current_value;
  }
  return request.min_value;
}

AdaptiveTuningControllerResult Refuse(
    const AdaptiveTuningControllerRequest& request,
    std::string code,
    std::string detail) {
  AdaptiveTuningControllerResult result;
  result.ok = false;
  result.fail_closed = true;
  result.benchmark_clean_evidence = request.metrics.benchmark_clean;
  result.semantics_neutral = false;
  result.selected_value = SafeRefusalValue(request);
  result.action = AdaptiveTuningActionClass::kRefuse;
  result.diagnostic_code = std::move(code);
  AddDiagnostic(&result, result.diagnostic_code + ":" + detail);
  AddEvidence(&result, "adaptive_tuning_controller=odf101_v1");
  AddEvidence(&result, "decision=refuse");
  AddEvidence(&result, "fail_closed=true");
  AddEvidence(&result, "advisory_only=true");
  AddEvidence(&result, "resource_governance_only=true");
  AddEvidence(&result, "semantics_neutral=false");
  AddEvidence(&result, "mga_finality_authority=engine_transaction_inventory");
  AddEvidence(&result, "parser_executes_sql=false");
  AddEvidence(&result, "provider_transaction_finality_authority=false");
  AddEvidence(&result, "provider_visibility_authority=false");
  AddEvidence(&result, "client_autocommit_authority=false");
  AddEvidence(&result, "wal_recovery_authority=false");
  AddEvidence(&result,
              "benchmark_clean=" + BoolText(result.benchmark_clean_evidence));
  AddEvidence(&result, std::move(detail));
  for (const auto& field :
       scratchbird::core::metrics::SerializeAdaptiveTuningMetricEvidence(
           request.metrics)) {
    result.metrics_evidence.push_back(field.first + "=" + field.second);
  }
  return result;
}

AdaptiveTuningBudgetEvidence BuildBudget(
    const AdaptiveTuningControllerRequest& request) {
  AdaptiveTuningBudgetEvidence budget;
  budget.observed_latency_microseconds =
      request.metrics.observed_latency_microseconds;
  budget.latency_budget_microseconds = request.metrics.latency_budget_microseconds;
  budget.observed_memory_bytes = request.metrics.observed_memory_bytes;
  budget.memory_budget_bytes = request.metrics.memory_budget_bytes;
  budget.backlog_units = request.metrics.backlog_units;
  budget.backlog_budget_units = request.metrics.backlog_budget_units;
  budget.error_count = request.metrics.error_count;
  budget.throughput_units_per_second =
      request.metrics.throughput_units_per_second;
  budget.quota_pressure_ppm = request.metrics.quota_pressure_ppm;
  budget.latency_over_budget =
      request.metrics.observed_latency_microseconds >
      request.metrics.latency_budget_microseconds;
  budget.memory_over_budget =
      request.metrics.observed_memory_bytes > request.metrics.memory_budget_bytes;
  budget.backlog_over_budget =
      request.metrics.backlog_budget_units != 0 &&
      request.metrics.backlog_units > request.metrics.backlog_budget_units;
  budget.quota_pressure_high =
      request.metrics.quota_pressure_ppm >= kPressurePpm;
  budget.errors_observed = request.metrics.error_count != 0;
  return budget;
}

void AddBudgetEvidence(AdaptiveTuningControllerResult* result) {
  AddEvidence(result,
              "observed_latency_microseconds=" +
                  std::to_string(result->budget.observed_latency_microseconds));
  AddEvidence(result,
              "latency_budget_microseconds=" +
                  std::to_string(result->budget.latency_budget_microseconds));
  AddEvidence(result,
              "observed_memory_bytes=" +
                  std::to_string(result->budget.observed_memory_bytes));
  AddEvidence(result,
              "memory_budget_bytes=" +
                  std::to_string(result->budget.memory_budget_bytes));
  AddEvidence(result,
              "backlog_units=" + std::to_string(result->budget.backlog_units));
  AddEvidence(result,
              "backlog_budget_units=" +
                  std::to_string(result->budget.backlog_budget_units));
  AddEvidence(result,
              "error_count=" + std::to_string(result->budget.error_count));
  AddEvidence(result,
              "throughput_units_per_second=" +
                  std::to_string(result->budget.throughput_units_per_second));
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

u64 IncreasedValue(const AdaptiveTuningControllerRequest& request) {
  const u64 step = std::max<u64>(1, request.current_value / 4);
  return std::clamp(SaturatingAdd(request.current_value, step),
                    request.min_value,
                    request.max_value);
}

u64 DecreasedValue(const AdaptiveTuningControllerRequest& request) {
  const u64 step = std::max<u64>(1, request.current_value / 4);
  const u64 selected =
      request.current_value > step ? request.current_value - step : 0;
  return std::clamp(selected, request.min_value, request.max_value);
}

bool KnobConsumesMoreWhenIncreased(AdaptiveTuningKnob knob) {
  return knob != AdaptiveTuningKnob::kRefreshInterval;
}

}  // namespace

const char* AdaptiveTuningKnobName(AdaptiveTuningKnob knob) {
  switch (knob) {
    case AdaptiveTuningKnob::kPrefetchDepth:
      return "prefetch_depth";
    case AdaptiveTuningKnob::kMergeWorkers:
      return "merge_workers";
    case AdaptiveTuningKnob::kRefreshInterval:
      return "refresh_interval";
    case AdaptiveTuningKnob::kCandidateBudget:
      return "candidate_budget";
    case AdaptiveTuningKnob::kCachePartition:
      return "cache_partition";
    case AdaptiveTuningKnob::kEvidenceSampleRate:
      return "evidence_sample_rate";
    case AdaptiveTuningKnob::kVectorEfSearch:
      return "vector_ef_search";
    case AdaptiveTuningKnob::kVectorNprobe:
      return "vector_nprobe";
    case AdaptiveTuningKnob::kUnknown:
      return "unknown";
  }
  return "unknown";
}

const char* AdaptiveTuningActionClassName(AdaptiveTuningActionClass action) {
  switch (action) {
    case AdaptiveTuningActionClass::kRefuse:
      return "refuse";
    case AdaptiveTuningActionClass::kHold:
      return "hold";
    case AdaptiveTuningActionClass::kIncrease:
      return "increase";
    case AdaptiveTuningActionClass::kDecrease:
      return "decrease";
    case AdaptiveTuningActionClass::kReset:
      return "reset";
    case AdaptiveTuningActionClass::kDefault:
      return "default";
  }
  return "refuse";
}

AdaptiveTuningControllerRequest BuildAdaptiveTuningAgentRequest(
    AdaptiveTuningKnob knob,
    u64 min_value,
    u64 max_value,
    u64 default_value,
    u64 current_value,
    AdaptiveTuningSafetyPolicy safety,
    scratchbird::core::metrics::AdaptiveTuningMetricEvidence metrics) {
  AdaptiveTuningControllerRequest request;
  request.knob = knob;
  request.knob_label = AdaptiveTuningKnobName(knob);
  request.min_value = min_value;
  request.max_value = max_value;
  request.default_value = default_value;
  request.current_value = current_value;
  request.safety = std::move(safety);
  request.metrics = std::move(metrics);
  return request;
}

AdaptiveTuningControllerResult EvaluateAdaptiveTuningController(
    const AdaptiveTuningControllerRequest& request) {
  if (!IsKnownKnob(request.knob)) {
    return Refuse(request,
                  "SB_AGENT_ADAPTIVE_TUNING.UNKNOWN_KNOB",
                  "unknown_knob");
  }
  if (request.knob_label.empty() ||
      request.knob_label != AdaptiveTuningKnobName(request.knob) ||
      request.metrics.knob_label != request.knob_label ||
      !request.metrics.labels_present) {
    return Refuse(request,
                  "SB_AGENT_ADAPTIVE_TUNING.MISSING_LABELS",
                  "missing_or_mismatched_knob_label");
  }
  if (request.min_value == 0 || request.max_value == 0 ||
      request.min_value > request.max_value) {
    return Refuse(request,
                  "SB_AGENT_ADAPTIVE_TUNING.INVALID_BOUNDS",
                  "invalid_bounds");
  }
  if (request.default_value < request.min_value ||
      request.default_value > request.max_value) {
    return Refuse(request,
                  "SB_AGENT_ADAPTIVE_TUNING.INVALID_DEFAULT",
                  "default_outside_bounds");
  }
  if (request.current_value < request.min_value ||
      request.current_value > request.max_value) {
    return Refuse(request,
                  "SB_AGENT_ADAPTIVE_TUNING.CURRENT_OUTSIDE_BOUNDS",
                  "current_outside_bounds");
  }
  if (!request.metrics.benchmark_clean) {
    return Refuse(request,
                  "SB_AGENT_ADAPTIVE_TUNING.BENCHMARK_DIRTY",
                  "benchmark_clean_evidence_required");
  }
  if (!request.metrics.evidence_present ||
      !request.metrics.evidence_authoritative ||
      !scratchbird::core::metrics::AdaptiveTuningMetricEvidenceFresh(
          request.metrics)) {
    return Refuse(request,
                  "SB_AGENT_ADAPTIVE_TUNING.STALE_METRICS_EVIDENCE",
                  "metrics_evidence_missing_stale_or_unauthoritative");
  }
  if (request.metrics.latency_budget_microseconds == 0 ||
      request.metrics.memory_budget_bytes == 0) {
    return Refuse(request,
                  "SB_AGENT_ADAPTIVE_TUNING.INVALID_METRIC_BUDGET",
                  "latency_and_memory_budget_required");
  }
  if (!request.safety.policy_allowed ||
      !request.safety.semantics_neutral_proof ||
      !request.safety.advisory_resource_governance_only) {
    return Refuse(request,
                  "SB_AGENT_ADAPTIVE_TUNING.UNSAFE_POLICY",
                  "policy_semantics_neutral_proof_required");
  }
  if (!AuthoritySafe(request.safety, request.metrics)) {
    return Refuse(request,
                  "SB_AGENT_ADAPTIVE_TUNING.UNSAFE_AUTHORITY",
                  "mga_security_recheck_and_engine_authority_required");
  }
  if (request.metrics.quota_pressure_ppm > kQuotaOverrunPpm) {
    return Refuse(request,
                  "SB_AGENT_ADAPTIVE_TUNING.QUOTA_OVERRUN",
                  "quota_pressure_overrun");
  }
  if (request.metrics.hard_backlog_refusal) {
    return Refuse(request,
                  "SB_AGENT_ADAPTIVE_TUNING.HARD_BACKLOG_PRESSURE",
                  "hard_backlog_refusal");
  }
  auto governance_request = request.resource_governance;
  governance_request.expected_family =
      ResourceGovernanceFamily::kAdaptiveTuningKnob;
  const auto governance = AdmitResourceGovernance(governance_request);
  if (governance.action == ResourceGovernanceAction::kFailClosed) {
    auto result = Refuse(request,
                         "SB_AGENT_ADAPTIVE_TUNING.ODF106_QUOTA_REFUSED",
                         governance.diagnostic_code);
    result.evidence.insert(result.evidence.end(), governance.evidence.begin(),
                           governance.evidence.end());
    return result;
  }
  if (governance.action == ResourceGovernanceAction::kCancel) {
    auto result = Refuse(request,
                         "SB_AGENT_ADAPTIVE_TUNING.ODF106_CANCELLED",
                         governance.diagnostic_code);
    result.evidence.insert(result.evidence.end(), governance.evidence.begin(),
                           governance.evidence.end());
    return result;
  }

  AdaptiveTuningControllerResult result;
  result.ok = true;
  result.fail_closed = false;
  result.benchmark_clean_evidence = true;
  result.semantics_neutral = true;
  result.selected_value = request.current_value;
  result.action = AdaptiveTuningActionClass::kHold;
  result.diagnostic_code = "SB_AGENT_ADAPTIVE_TUNING.HOLD";
  result.budget = BuildBudget(request);

  if (request.safety.reset_to_default_requested && request.safety.allow_reset) {
    result.selected_value = request.default_value;
    result.action = AdaptiveTuningActionClass::kReset;
    result.diagnostic_code = "SB_AGENT_ADAPTIVE_TUNING.RESET";
  } else if (request.safety.use_default_requested &&
             request.safety.allow_default) {
    result.selected_value = request.default_value;
    result.action = AdaptiveTuningActionClass::kDefault;
    result.diagnostic_code = "SB_AGENT_ADAPTIVE_TUNING.DEFAULT";
  } else {
    const bool resource_pressure =
        result.budget.latency_over_budget ||
        result.budget.memory_over_budget ||
        result.budget.quota_pressure_high ||
        result.budget.errors_observed;
    const bool healthy =
        LessOrEqualRatio(request.metrics.observed_latency_microseconds,
                         request.metrics.latency_budget_microseconds,
                         7,
                         10) &&
        LessOrEqualRatio(request.metrics.observed_memory_bytes,
                         request.metrics.memory_budget_bytes,
                         7,
                         10) &&
        request.metrics.quota_pressure_ppm < kPressurePpm &&
        request.metrics.error_count == 0;
    const bool work_to_absorb =
        result.budget.backlog_over_budget ||
        request.metrics.throughput_units_per_second != 0;

    if (resource_pressure) {
      const bool increase = !KnobConsumesMoreWhenIncreased(request.knob);
      if (increase && request.safety.allow_increase) {
        result.selected_value = IncreasedValue(request);
        result.action = AdaptiveTuningActionClass::kIncrease;
        result.diagnostic_code = "SB_AGENT_ADAPTIVE_TUNING.INCREASE";
      } else if (!increase && request.safety.allow_decrease) {
        result.selected_value = DecreasedValue(request);
        result.action = AdaptiveTuningActionClass::kDecrease;
        result.diagnostic_code = "SB_AGENT_ADAPTIVE_TUNING.DECREASE";
      }
    } else if (healthy && work_to_absorb) {
      const bool increase = KnobConsumesMoreWhenIncreased(request.knob);
      if (increase && request.safety.allow_increase) {
        result.selected_value = IncreasedValue(request);
        result.action = AdaptiveTuningActionClass::kIncrease;
        result.diagnostic_code = "SB_AGENT_ADAPTIVE_TUNING.INCREASE";
      } else if (!increase && request.safety.allow_decrease) {
        result.selected_value = DecreasedValue(request);
        result.action = AdaptiveTuningActionClass::kDecrease;
        result.diagnostic_code = "SB_AGENT_ADAPTIVE_TUNING.DECREASE";
      }
    }
  }
  if (governance.action == ResourceGovernanceAction::kSlowdownDegrade ||
      governance.action == ResourceGovernanceAction::kExactScalarFallback) {
    result.selected_value = DecreasedValue(request);
    result.action = AdaptiveTuningActionClass::kDecrease;
    result.diagnostic_code = "SB_AGENT_ADAPTIVE_TUNING.ODF106_SLOWDOWN_DEGRADE";
  }

  AddEvidence(&result, "adaptive_tuning_controller=odf101_v1");
  AddEvidence(&result,
              std::string("knob=") + AdaptiveTuningKnobName(request.knob));
  AddEvidence(&result,
              std::string("action=") +
                  AdaptiveTuningActionClassName(result.action));
  AddEvidence(&result, "selected_value=" + std::to_string(result.selected_value));
  AddEvidence(&result, "min_value=" + std::to_string(request.min_value));
  AddEvidence(&result, "max_value=" + std::to_string(request.max_value));
  AddEvidence(&result, "default_value=" + std::to_string(request.default_value));
  AddEvidence(&result, "fail_closed=false");
  AddEvidence(&result, "advisory_only=true");
  AddEvidence(&result, "resource_governance_only=true");
  AddEvidence(&result, "semantics_neutral=true");
  AddEvidence(&result, "benchmark_clean=true");
  AddEvidence(&result, "mga_recheck_evidence=required");
  AddEvidence(&result, "security_recheck_evidence=required");
  AddEvidence(&result, "mga_finality_authority=engine_transaction_inventory");
  AddEvidence(&result, "parser_executes_sql=false");
  AddEvidence(&result, "provider_transaction_finality_authority=false");
  AddEvidence(&result, "provider_visibility_authority=false");
  AddEvidence(&result, "client_autocommit_authority=false");
  AddEvidence(&result, "wal_recovery_authority=false");
  AddBudgetEvidence(&result);
  result.evidence.insert(result.evidence.end(), governance.evidence.begin(),
                         governance.evidence.end());
  AddEvidence(&result,
              "adaptive_tuning.resource_governance_action=" +
                  std::string(ResourceGovernanceActionName(governance.action)));
  for (const auto& field :
       scratchbird::core::metrics::SerializeAdaptiveTuningMetricEvidence(
           request.metrics)) {
    result.metrics_evidence.push_back(field.first + "=" + field.second);
  }
  return result;
}

std::string SerializeAdaptiveTuningControllerEvidence(
    const AdaptiveTuningControllerResult& result) {
  std::ostringstream out;
  out << "ok=" << BoolText(result.ok) << '\n';
  out << "fail_closed=" << BoolText(result.fail_closed) << '\n';
  out << "benchmark_clean=" << BoolText(result.benchmark_clean_evidence)
      << '\n';
  out << "semantics_neutral=" << BoolText(result.semantics_neutral) << '\n';
  out << "advisory_only=" << BoolText(result.advisory_only) << '\n';
  out << "action=" << AdaptiveTuningActionClassName(result.action) << '\n';
  out << "selected_value=" << result.selected_value << '\n';
  out << "diagnostic_code=" << result.diagnostic_code << '\n';
  for (const auto& evidence : result.evidence) {
    out << "evidence=" << evidence << '\n';
  }
  for (const auto& evidence : result.metrics_evidence) {
    out << "metrics_evidence=" << evidence << '\n';
  }
  for (const auto& diagnostic : result.diagnostics) {
    out << "diagnostic=" << diagnostic << '\n';
  }
  return out.str();
}

}  // namespace scratchbird::core::agents
