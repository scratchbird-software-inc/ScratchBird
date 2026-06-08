// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory_adaptive_batch_working_set.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::metrics::MetricLabel;
using scratchbird::core::metrics::MetricType;
using scratchbird::core::metrics::MetricUnit;
using scratchbird::core::metrics::MetricValue;
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr const char* kAnchor =
    "CEIC-029_ADAPTIVE_BATCH_WORKING_SET_LOCALITY";
constexpr const char* kBatchAnchor =
    "CEIC-029_PRESSURE_AWARE_ADAPTIVE_BATCH";
constexpr const char* kWorkingSetAnchor =
    "CEIC-029_WORKING_SET_LOCALITY_EVIDENCE";
constexpr const char* kAuthorityBoundary =
    "memory_adaptive_batch_working_set.authority_scope=evidence_only_not_transaction_finality_visibility_authorization_security_recovery_parser_donor_wal_benchmark_optimizer_plan_index_finality_cluster_or_agent_action_authority";

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::memory};
}

Status ErrorStatus(StatusCode code = StatusCode::memory_invalid_request) {
  return {code, Severity::error, Subsystem::memory};
}

std::string BoolString(bool value) {
  return value ? "true" : "false";
}

u64 PercentOf(u64 numerator, u64 denominator) {
  if (denominator == 0) {
    return 0;
  }
  if (numerator >= denominator) {
    return 100;
  }
  return (numerator * 100) / denominator;
}

u64 ClampRows(u64 value, u64 minimum, u64 maximum) {
  if (maximum != 0) {
    value = std::min(value, maximum);
  }
  return std::max(value, minimum == 0 ? 1 : minimum);
}

u64 ClampBytes(u64 value, u64 minimum, u64 maximum) {
  if (maximum != 0) {
    value = std::min(value, maximum);
  }
  return std::max(value, minimum == 0 ? 1 : minimum);
}

u64 ApplyPercent(u64 value, u64 percent) {
  if (value == 0) {
    return 0;
  }
  return std::max<u64>(1, (value * percent) / 100);
}

std::string EmptyAsDash(const std::string& value) {
  return value.empty() ? "-" : value;
}

DiagnosticRecord Diagnostic(Status status,
                            std::string code,
                            std::string message,
                            std::vector<DiagnosticArgument> args = {}) {
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(code),
                        std::move(message),
                        std::move(args),
                        {},
                        "core.memory.adaptive_batch_working_set",
                        "provide CEIC-017 pressure evidence, CEIC-019 page-cache facts, deterministic result-hash boundaries, and locality evidence");
}

void AddCommonEvidence(AdaptiveBatchSizingDecision* decision,
                       const AdaptiveBatchSizingRequest& request) {
  decision->evidence.push_back(kAnchor);
  decision->evidence.push_back(kBatchAnchor);
  decision->evidence.push_back(kWorkingSetAnchor);
  decision->evidence.push_back(kAuthorityBoundary);
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.operation=" +
      std::string(AdaptiveBatchOperationKindName(request.operation)));
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.pressure_state=" +
      std::string(MemoryPressureStateName(request.pressure_decision.new_state)));
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.action=" +
      std::string(AdaptiveBatchAdmissionActionName(decision->action)));
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.ceic017_pressure_evidence=true");
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.ceic019_page_cache_frame_pool_evidence=" +
      BoolString(request.working_set.ceic019_page_cache_frame_pool_evidence));
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.page_cache_snapshot_deterministic=" +
      BoolString(request.working_set.page_cache_snapshot_deterministic));
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.result_hash_stability_required=" +
      BoolString(decision->deterministic_boundaries_required));
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.deterministic_boundaries_preserved=" +
      BoolString(decision->deterministic_boundaries_preserved));
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.result_hash_stability_preserved=" +
      BoolString(decision->result_hash_stability_preserved));
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.working_set_temperature=" +
      std::string(WorkingSetTemperatureName(decision->working_set_temperature)));
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.tenant_quota_limited=" +
      BoolString(decision->tenant_quota_limited));
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.scan_resistance_applied=" +
      BoolString(decision->scan_resistance_applied));
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.replacement_policy_evaluation_present=" +
      BoolString(decision->replacement_policy_evaluation_present));
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.selected_replacement_policy=" +
      request.working_set.selected_replacement_policy);
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.dirty_page_policy_applied=" +
      BoolString(decision->dirty_page_policy_applied));
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.prefetch_budget_applied=" +
      BoolString(decision->prefetch_budget_applied));
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.locality_fallback_used=" +
      BoolString(decision->locality_fallback_used));
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.huge_page_fallback_used=" +
      BoolString(decision->huge_page_fallback_used));
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.no_authority.transaction_finality=true");
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.no_authority.visibility=true");
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.no_authority.authorization_security=true");
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.no_authority.recovery=true");
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.no_authority.parser_donor_wal=true");
  decision->evidence.push_back(
      "memory_adaptive_batch_working_set.no_authority.benchmark_optimizer_index_cluster_agent=true");
}

void AddSupportRow(std::vector<MemorySupportBundleRow>* rows,
                   std::string key,
                   std::string value,
                   std::string redaction_class = "public") {
  MemorySupportBundleRow row;
  row.key = std::move(key);
  row.value = std::move(value);
  row.redaction_class = std::move(redaction_class);
  rows->push_back(std::move(row));
}

MetricValue GaugeMetric(const std::string& family,
                        double value,
                        const AdaptiveBatchSizingRequest& request,
                        MetricUnit unit = MetricUnit::count) {
  MetricValue metric;
  metric.family = family;
  metric.type = MetricType::gauge;
  metric.value = value;
  metric.labels = {
      MetricLabel{"database", EmptyAsDash(request.working_set.database_id)},
      MetricLabel{"tenant", EmptyAsDash(request.working_set.tenant_id)},
      MetricLabel{"user", EmptyAsDash(request.working_set.user_id)},
      MetricLabel{"session", EmptyAsDash(request.working_set.session_id)},
      MetricLabel{"query", EmptyAsDash(request.working_set.query_id)},
      MetricLabel{"operator", EmptyAsDash(request.working_set.operator_id)},
      MetricLabel{"operation", AdaptiveBatchOperationKindName(request.operation)},
      MetricLabel{"pressure_state",
                  MemoryPressureStateName(request.pressure_decision.new_state)},
      MetricLabel{"unit", scratchbird::core::metrics::MetricUnitName(unit)}};
  return metric;
}

AdaptiveBatchSizingDecision FailClosed(AdaptiveBatchSizingRequest request,
                                       std::string code,
                                       std::string message,
                                       std::string reason) {
  AdaptiveBatchSizingDecision decision;
  decision.status = ErrorStatus();
  decision.fail_closed = true;
  decision.admission_allowed = false;
  decision.operation = request.operation;
  decision.pressure_state = request.pressure_decision.new_state;
  decision.action = AdaptiveBatchAdmissionAction::refuse;
  decision.deterministic_boundaries_required =
      request.route_requires_stable_result_hash ||
      request.result_hash_stability_required;
  decision.diagnostic = Diagnostic(decision.status,
                                   std::move(code),
                                   std::move(message),
                                   {{"reason", std::move(reason)}});
  AddCommonEvidence(&decision, request);
  decision.evidence.push_back("memory_adaptive_batch_working_set.fail_closed=true");
  return decision;
}

WorkingSetTemperature ClassifyWorkingSet(
    const WorkingSetLocalityObservation& observation) {
  if (observation.page_cache_hot_pages != 0 &&
      observation.page_cache_hot_pages >= observation.page_cache_warm_pages &&
      observation.page_cache_hot_pages >= observation.page_cache_cold_pages) {
    return WorkingSetTemperature::hot;
  }
  const u64 hit_percent = PercentOf(observation.page_cache_recent_hits,
                                    observation.page_cache_recent_accesses);
  const u64 reuse_percent = PercentOf(observation.page_cache_reuse_count,
                                      observation.page_cache_reuse_count +
                                          observation.page_cache_allocate_count);
  if (hit_percent >= 80 || reuse_percent >= 75) {
    return WorkingSetTemperature::hot;
  }
  if (observation.page_cache_warm_pages != 0 ||
      hit_percent >= 35 ||
      reuse_percent >= 30) {
    return WorkingSetTemperature::warm;
  }
  return WorkingSetTemperature::cold;
}

u64 PressurePercent(const AdaptiveBatchSizingPolicy& policy,
                    MemoryPressureState state) {
  switch (state) {
    case MemoryPressureState::normal:
      return policy.normal_percent;
    case MemoryPressureState::recovery:
      return policy.recovery_percent;
    case MemoryPressureState::soft_pressure:
      return policy.soft_pressure_percent;
    case MemoryPressureState::high_pressure:
      return policy.high_pressure_percent;
    case MemoryPressureState::emergency_pressure:
      return policy.emergency_pressure_percent;
  }
  return policy.high_pressure_percent;
}

u64 WorkingSetPercent(const AdaptiveBatchSizingPolicy& policy,
                      WorkingSetTemperature temperature) {
  switch (temperature) {
    case WorkingSetTemperature::hot:
      return policy.hot_working_set_bonus_percent;
    case WorkingSetTemperature::warm:
      return policy.warm_working_set_percent;
    case WorkingSetTemperature::cold:
      return policy.cold_working_set_percent;
  }
  return policy.cold_working_set_percent;
}

bool ScanResistant(const WorkingSetLocalityObservation& observation) {
  return observation.scan_resistance_evidence &&
         observation.sequential_scan_pages > 0 &&
         observation.sequential_scan_pages >
             std::max<u64>(observation.random_reuse_pages * 4, 16);
}

bool ReplacementPolicyEvaluated(
    const WorkingSetLocalityObservation& observation) {
  return observation.clock_replacement_policy_evaluated &&
         observation.lru2_replacement_policy_evaluated &&
         observation.arc_replacement_policy_evaluated &&
         !observation.selected_replacement_policy.empty();
}

bool TenantQuotaPressed(const WorkingSetLocalityObservation& observation) {
  return observation.tenant_quota_evidence &&
         observation.tenant_quota_bytes != 0 &&
         observation.tenant_active_bytes + observation.projected_bytes >
             observation.tenant_quota_bytes;
}

bool DirtyPolicyPressed(const WorkingSetLocalityObservation& observation) {
  return observation.dirty_page_policy_evidence &&
         observation.dirty_page_limit != 0 &&
         observation.dirty_pages >= observation.dirty_page_limit;
}

void AttachRowsAndMetrics(AdaptiveBatchSizingDecision* decision,
                          const AdaptiveBatchSizingRequest& request) {
  AddSupportRow(&decision->support_bundle_rows,
                "memory_working_set_locality.operation",
                AdaptiveBatchOperationKindName(request.operation));
  AddSupportRow(&decision->support_bundle_rows,
                "memory_working_set_locality.pressure_state",
                MemoryPressureStateName(decision->pressure_state));
  AddSupportRow(&decision->support_bundle_rows,
                "memory_working_set_locality.action",
                AdaptiveBatchAdmissionActionName(decision->action));
  AddSupportRow(&decision->support_bundle_rows,
                "memory_working_set_locality.temperature",
                WorkingSetTemperatureName(decision->working_set_temperature));
  AddSupportRow(&decision->support_bundle_rows,
                "memory_working_set_locality.admitted_batch_rows",
                std::to_string(decision->admitted_batch_rows));
  AddSupportRow(&decision->support_bundle_rows,
                "memory_working_set_locality.admitted_batch_bytes",
                std::to_string(decision->admitted_batch_bytes));
  AddSupportRow(&decision->support_bundle_rows,
                "memory_working_set_locality.prefetch_admitted_pages",
                std::to_string(decision->prefetch_admitted_pages));
  AddSupportRow(&decision->support_bundle_rows,
                "memory_working_set_locality.cleanup_admitted_pages",
                std::to_string(decision->cleanup_admitted_pages));
  AddSupportRow(&decision->support_bundle_rows,
                "memory_working_set_locality.deterministic_boundaries_preserved",
                BoolString(decision->deterministic_boundaries_preserved));
  AddSupportRow(&decision->support_bundle_rows,
                "memory_working_set_locality.replacement_policy_evaluation",
                decision->replacement_policy_evaluation_present
                    ? request.working_set.selected_replacement_policy
                    : "missing");
  AddSupportRow(&decision->support_bundle_rows,
                "memory_working_set_locality.no_authority",
                "observability_control_only");

  decision->metrics.push_back(GaugeMetric("memory_adaptive_batch_size_rows",
                                          static_cast<double>(decision->admitted_batch_rows),
                                          request));
  decision->metrics.push_back(GaugeMetric("memory_adaptive_batch_size_bytes",
                                          static_cast<double>(decision->admitted_batch_bytes),
                                          request,
                                          MetricUnit::bytes));
  decision->metrics.push_back(GaugeMetric("memory_working_set_locality_pages",
                                          static_cast<double>(
                                              request.working_set.page_cache_resident_pages),
                                          request));
  decision->metrics.push_back(GaugeMetric("memory_working_set_prefetch_budget_pages",
                                          static_cast<double>(
                                              decision->prefetch_admitted_pages),
                                          request));
  decision->metrics.push_back(GaugeMetric(
      "memory_page_cache_replacement_policy_evaluated",
      decision->replacement_policy_evaluation_present ? 1.0 : 0.0,
      request));
  decision->support_bundle_ready = !decision->support_bundle_rows.empty();
  decision->metrics_ready = !decision->metrics.empty();
}

}  // namespace

const char* AdaptiveBatchOperationKindName(AdaptiveBatchOperationKind kind) {
  switch (kind) {
    case AdaptiveBatchOperationKind::copy:
      return "copy";
    case AdaptiveBatchOperationKind::result_frame:
      return "result_frame";
    case AdaptiveBatchOperationKind::vector:
      return "vector";
    case AdaptiveBatchOperationKind::hash_join:
      return "hash_join";
    case AdaptiveBatchOperationKind::sort:
      return "sort";
    case AdaptiveBatchOperationKind::index_build:
      return "index_build";
    case AdaptiveBatchOperationKind::prefetch:
      return "prefetch";
    case AdaptiveBatchOperationKind::cleanup_page_cache:
      return "cleanup_page_cache";
  }
  return "unknown";
}

const char* WorkingSetTemperatureName(WorkingSetTemperature temperature) {
  switch (temperature) {
    case WorkingSetTemperature::hot:
      return "hot";
    case WorkingSetTemperature::warm:
      return "warm";
    case WorkingSetTemperature::cold:
      return "cold";
  }
  return "unknown";
}

const char* AdaptiveBatchAdmissionActionName(
    AdaptiveBatchAdmissionAction action) {
  switch (action) {
    case AdaptiveBatchAdmissionAction::admit:
      return "admit";
    case AdaptiveBatchAdmissionAction::reduce:
      return "reduce";
    case AdaptiveBatchAdmissionAction::throttle:
      return "throttle";
    case AdaptiveBatchAdmissionAction::spill:
      return "spill";
    case AdaptiveBatchAdmissionAction::cancel:
      return "cancel";
    case AdaptiveBatchAdmissionAction::refuse:
      return "refuse";
  }
  return "unknown";
}

AdaptiveBatchSizingDecision PlanAdaptiveBatchWorkingSetLocality(
    AdaptiveBatchSizingRequest request) {
  if (request.policy.require_ceic017_pressure_evidence &&
      request.pressure_decision.evidence.empty()) {
    return FailClosed(std::move(request),
                      "memory_adaptive_batch_missing_pressure_evidence",
                      "core.memory.adaptive_batch.pressure_evidence_missing",
                      "ceic017_pressure_decision_required");
  }
  if (!request.production_route) {
    return FailClosed(std::move(request),
                      "memory_adaptive_batch_nonproduction_route",
                      "core.memory.adaptive_batch.nonproduction_route_refused",
                      "production_route_required");
  }
  if (request.cluster_route && !request.external_cluster_provider) {
    return FailClosed(std::move(request),
                      "memory_adaptive_batch_cluster_provider_required",
                      "core.memory.adaptive_batch.cluster_provider_required",
                      "cluster_production_external_provider_only");
  }
  if (request.policy.refuse_without_locality_evidence &&
      (!request.working_set.ceic019_page_cache_frame_pool_evidence ||
       !request.working_set.page_cache_snapshot_deterministic ||
       !ReplacementPolicyEvaluated(request.working_set) ||
       !request.working_set.numa_locality_evidence ||
       !request.working_set.huge_page_evidence)) {
    return FailClosed(std::move(request),
                      "memory_adaptive_batch_locality_evidence_missing",
                      "core.memory.adaptive_batch.locality_evidence_missing",
                      "page_cache_locality_replacement_policy_huge_page_evidence_required");
  }

  AdaptiveBatchSizingDecision decision;
  decision.status = OkStatus();
  decision.operation = request.operation;
  decision.pressure_state = request.pressure_decision.new_state;
  decision.requested_batch_rows = request.requested_batch_rows;
  decision.requested_batch_bytes = request.requested_batch_bytes;
  decision.working_set_temperature = ClassifyWorkingSet(request.working_set);
  decision.deterministic_boundaries_required =
      request.route_requires_stable_result_hash ||
      request.result_hash_stability_required;
  if (decision.deterministic_boundaries_required) {
    decision.deterministic_boundaries_preserved =
        request.deterministic_boundary_evidence &&
        request.deterministic_route_evidence &&
        request.stable_result_hash_evidence;
    decision.result_hash_stability_preserved =
        decision.deterministic_boundaries_preserved;
    if (request.policy.require_deterministic_boundaries_for_result_hash &&
        !decision.deterministic_boundaries_preserved) {
      return FailClosed(std::move(request),
                        "memory_adaptive_batch_deterministic_boundary_required",
                        "core.memory.adaptive_batch.result_hash_boundary_missing",
                        "stable_result_hash_routes_require_deterministic_batch_boundaries");
    }
  } else {
    decision.deterministic_boundaries_preserved =
        request.deterministic_boundary_evidence;
    decision.result_hash_stability_preserved =
        request.stable_result_hash_evidence;
  }

  u64 percent = PressurePercent(request.policy, decision.pressure_state);
  percent = (percent * WorkingSetPercent(request.policy,
                                         decision.working_set_temperature)) / 100;
  decision.scan_resistance_applied = ScanResistant(request.working_set);
  if (decision.scan_resistance_applied) {
    percent = std::min(percent, request.policy.scan_resistant_percent);
  }
  decision.replacement_policy_evaluation_present =
      ReplacementPolicyEvaluated(request.working_set);
  decision.tenant_quota_limited = TenantQuotaPressed(request.working_set);
  if (decision.tenant_quota_limited) {
    percent = std::min(percent, request.policy.tenant_quota_pressure_percent);
  }
  decision.dirty_page_policy_applied = DirtyPolicyPressed(request.working_set);
  if (decision.dirty_page_policy_applied) {
    percent = std::min(percent, request.policy.dirty_pressure_percent);
  }
  decision.reduction_percent = std::max<u64>(1, std::min<u64>(percent, 100));

  decision.admitted_batch_rows =
      ClampRows(ApplyPercent(request.requested_batch_rows,
                             decision.reduction_percent),
                request.policy.min_batch_rows,
                request.max_batch_rows);
  decision.admitted_batch_bytes =
      ClampBytes(ApplyPercent(request.requested_batch_bytes,
                              decision.reduction_percent),
                 request.policy.min_batch_bytes,
                 request.max_batch_bytes);

  decision.prefetch_admitted_pages =
      std::min({request.working_set.prefetch_requested_pages,
                request.working_set.prefetch_budget_pages == 0
                    ? request.policy.max_prefetch_pages
                    : request.working_set.prefetch_budget_pages,
                request.policy.max_prefetch_pages});
  decision.prefetch_budget_applied =
      request.working_set.prefetch_budget_evidence &&
      decision.prefetch_admitted_pages <
          request.working_set.prefetch_requested_pages;
  decision.cleanup_admitted_pages =
      std::min(request.policy.max_cleanup_pages,
               request.operation == AdaptiveBatchOperationKind::cleanup_page_cache
                   ? decision.admitted_batch_rows
                   : 0);

  const auto locality = EvaluateMemoryLocalityPolicy(request.locality_policy);
  if (!locality.ok()) {
    return FailClosed(std::move(request),
                      "memory_adaptive_batch_locality_policy_refused",
                      "core.memory.adaptive_batch.locality_policy_refused",
                      locality.diagnostic.diagnostic_code.empty()
                          ? "locality_policy_refused"
                          : locality.diagnostic.diagnostic_code);
  }
  decision.locality_fallback_used =
      locality.portable_fallback_used ||
      request.thread_local_cache_snapshot.locality_portable_fallback_used;
  decision.huge_page_fallback_used =
      request.locality_policy.huge_page_mode ==
          MemoryHugePagePolicyMode::prefer_huge_pages &&
      !locality.huge_page_hint_applied;
  decision.evidence.insert(decision.evidence.end(),
                           locality.evidence.begin(),
                           locality.evidence.end());
  decision.evidence.insert(decision.evidence.end(),
                           request.thread_local_cache_snapshot.evidence.begin(),
                           request.thread_local_cache_snapshot.evidence.end());

  if (!request.pressure_decision.ordinary_admission_allowed ||
      decision.pressure_state == MemoryPressureState::emergency_pressure) {
    if (request.cancel_supported) {
      decision.action = AdaptiveBatchAdmissionAction::cancel;
    } else if (request.spill_supported) {
      decision.action = AdaptiveBatchAdmissionAction::spill;
    } else {
      decision.action = AdaptiveBatchAdmissionAction::throttle;
    }
  } else if (decision.reduction_percent < 100 ||
             decision.admitted_batch_rows < request.requested_batch_rows ||
             decision.admitted_batch_bytes < request.requested_batch_bytes) {
    decision.action = request.throttle_supported
                          ? AdaptiveBatchAdmissionAction::reduce
                          : AdaptiveBatchAdmissionAction::throttle;
  } else {
    decision.action = AdaptiveBatchAdmissionAction::admit;
  }
  decision.admission_allowed =
      decision.action != AdaptiveBatchAdmissionAction::refuse &&
      decision.action != AdaptiveBatchAdmissionAction::cancel;
  decision.diagnostic = Diagnostic(decision.status,
                                   "memory_adaptive_batch_decision",
                                   "core.memory.adaptive_batch.decision");
  AddCommonEvidence(&decision, request);
  decision.evidence.push_back("memory_adaptive_batch_working_set.fail_closed=false");
  decision.evidence.push_back("memory_adaptive_batch_working_set.requested_batch_rows=" +
                              std::to_string(decision.requested_batch_rows));
  decision.evidence.push_back("memory_adaptive_batch_working_set.admitted_batch_rows=" +
                              std::to_string(decision.admitted_batch_rows));
  decision.evidence.push_back("memory_adaptive_batch_working_set.requested_batch_bytes=" +
                              std::to_string(decision.requested_batch_bytes));
  decision.evidence.push_back("memory_adaptive_batch_working_set.admitted_batch_bytes=" +
                              std::to_string(decision.admitted_batch_bytes));
  decision.evidence.push_back("memory_adaptive_batch_working_set.prefetch_admitted_pages=" +
                              std::to_string(decision.prefetch_admitted_pages));
  decision.evidence.push_back("memory_adaptive_batch_working_set.cleanup_admitted_pages=" +
                              std::to_string(decision.cleanup_admitted_pages));
  decision.evidence.push_back(
      "memory_adaptive_batch_working_set.deterministic_boundary_order=route_id_page_key_batch_ordinal_ascending");
  AttachRowsAndMetrics(&decision, request);
  decision.evidence.push_back("memory_adaptive_batch_working_set.support_bundle_ready=" +
                              BoolString(decision.support_bundle_ready));
  decision.evidence.push_back("memory_adaptive_batch_working_set.metrics_ready=" +
                              BoolString(decision.metrics_ready));
  return decision;
}

}  // namespace scratchbird::core::memory
