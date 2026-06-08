// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dynamic_cleanup_debt_scheduler.hpp"

#include "metric_producer.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace scratchbird::core::agents {
namespace {

namespace idx = scratchbird::core::index;
namespace metrics = scratchbird::core::metrics;

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status SchedulerOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status SchedulerErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

u64 SaturatingAdd(u64 left, u64 right) {
  if (right > std::numeric_limits<u64>::max() - left) {
    return std::numeric_limits<u64>::max();
  }
  return left + right;
}

u64 SaturatingMul(u64 left, u64 right) {
  if (left == 0 || right == 0) {
    return 0;
  }
  if (left > std::numeric_limits<u64>::max() / right) {
    return std::numeric_limits<u64>::max();
  }
  return left * right;
}

void AddEvidence(std::vector<DynamicCleanupDebtEvidenceField>* evidence,
                 std::string key,
                 std::string value) {
  evidence->push_back({std::move(key), std::move(value)});
}

void AddSourceEvidence(DynamicCleanupDebtSource* source,
                       std::string key,
                       std::string value) {
  if (source != nullptr) {
    AddEvidence(&source->evidence, std::move(key), std::move(value));
  }
}

void AddDecisionEvidence(DynamicCleanupDebtAssignment* assignment,
                         std::string key,
                         std::string value) {
  if (assignment != nullptr) {
    AddEvidence(&assignment->evidence, std::move(key), std::move(value));
  }
}

std::vector<DynamicCleanupDebtFamily> AllFamilies() {
  return {
      DynamicCleanupDebtFamily::version_chain,
      DynamicCleanupDebtFamily::exact_index_leaf,
      DynamicCleanupDebtFamily::secondary_delta_ledger,
      DynamicCleanupDebtFamily::summary_page_range,
      DynamicCleanupDebtFamily::large_value,
      DynamicCleanupDebtFamily::hot_leaf,
      DynamicCleanupDebtFamily::nosql_key_value,
      DynamicCleanupDebtFamily::nosql_document,
      DynamicCleanupDebtFamily::nosql_search,
      DynamicCleanupDebtFamily::nosql_vector,
      DynamicCleanupDebtFamily::nosql_graph,
      DynamicCleanupDebtFamily::nosql_time_series,
  };
}

DynamicCleanupDebtFailureMode DefaultFailureModeForFamily(
    DynamicCleanupDebtFamily family) {
  switch (family) {
    case DynamicCleanupDebtFamily::exact_index_leaf:
    case DynamicCleanupDebtFamily::summary_page_range:
    case DynamicCleanupDebtFamily::hot_leaf:
      return DynamicCleanupDebtFailureMode::fail_open_to_foreground;
    case DynamicCleanupDebtFamily::version_chain:
    case DynamicCleanupDebtFamily::secondary_delta_ledger:
    case DynamicCleanupDebtFamily::large_value:
    case DynamicCleanupDebtFamily::nosql_key_value:
    case DynamicCleanupDebtFamily::nosql_document:
    case DynamicCleanupDebtFamily::nosql_search:
    case DynamicCleanupDebtFamily::nosql_vector:
    case DynamicCleanupDebtFamily::nosql_graph:
    case DynamicCleanupDebtFamily::nosql_time_series:
      return DynamicCleanupDebtFailureMode::fail_closed_retain_debt;
  }
  return DynamicCleanupDebtFailureMode::fail_closed_retain_debt;
}

DynamicCleanupDebtFamilyCap EffectiveCap(
    const DynamicCleanupDebtSchedulerPolicy& policy,
    DynamicCleanupDebtFamily family) {
  DynamicCleanupDebtFamilyCap cap;
  cap.family = family;
  cap.max_work_units = policy.default_max_family_work_units;
  cap.max_items = policy.default_max_family_items;
  for (const auto& override_cap : policy.family_caps) {
    if (override_cap.family != family) {
      continue;
    }
    if (override_cap.max_work_units != 0) {
      cap.max_work_units = override_cap.max_work_units;
    }
    if (override_cap.max_items != 0) {
      cap.max_items = override_cap.max_items;
    }
  }
  if (cap.max_work_units == 0) {
    cap.max_work_units = policy.max_total_work_units;
  }
  if (cap.max_items == 0) {
    cap.max_items = policy.max_scheduled_items;
  }
  return cap;
}

u64 DebtUnits(const DynamicCleanupDebtSource& source) {
  u64 units = source.debt_units;
  if (source.debt_bytes != 0) {
    units = std::max<u64>(units, (source.debt_bytes + 4095u) / 4096u);
  }
  if (source.blocker_count != 0) {
    units = std::max<u64>(units, source.blocker_count);
  }
  return units;
}

bool HasDebt(const DynamicCleanupDebtSource& source) {
  return DebtUnits(source) != 0;
}

u64 ScoreForSource(const DynamicCleanupDebtSource& source) {
  u64 score = SaturatingMul(DebtUnits(source), 100);
  score = SaturatingAdd(score, (source.debt_bytes + 4095u) / 4096u);
  score = SaturatingAdd(score, SaturatingMul(source.blocker_count, 50));
  score = SaturatingAdd(score, source.age_microseconds / 1000000u);
  score = SaturatingAdd(score, source.priority_boost);
  switch (source.family) {
    case DynamicCleanupDebtFamily::version_chain:
      score = SaturatingAdd(score, 80);
      break;
    case DynamicCleanupDebtFamily::exact_index_leaf:
      score = SaturatingAdd(score, 90);
      break;
    case DynamicCleanupDebtFamily::secondary_delta_ledger:
      score = SaturatingAdd(score, 85);
      break;
    case DynamicCleanupDebtFamily::summary_page_range:
      score = SaturatingAdd(score, 30);
      break;
    case DynamicCleanupDebtFamily::large_value:
      score = SaturatingAdd(score, 70);
      break;
    case DynamicCleanupDebtFamily::hot_leaf:
      score = SaturatingAdd(score, 75);
      break;
    case DynamicCleanupDebtFamily::nosql_key_value:
      score = SaturatingAdd(score, 88);
      break;
    case DynamicCleanupDebtFamily::nosql_document:
      score = SaturatingAdd(score, 82);
      break;
    case DynamicCleanupDebtFamily::nosql_search:
      score = SaturatingAdd(score, 86);
      break;
    case DynamicCleanupDebtFamily::nosql_vector:
      score = SaturatingAdd(score, 84);
      break;
    case DynamicCleanupDebtFamily::nosql_graph:
      score = SaturatingAdd(score, 83);
      break;
    case DynamicCleanupDebtFamily::nosql_time_series:
      score = SaturatingAdd(score, 81);
      break;
  }
  return score;
}

u64 RequestedWorkUnits(const DynamicCleanupDebtSource& source,
                       const DynamicCleanupDebtSchedulerPolicy& policy) {
  u64 units = source.estimated_work_units != 0 ? source.estimated_work_units
                                               : DebtUnits(source);
  if (units == 0 && HasDebt(source)) {
    units = 1;
  }
  if (source.max_work_units != 0) {
    units = std::min(units, source.max_work_units);
  }
  if (policy.max_work_units_per_item != 0) {
    units = std::min(units, policy.max_work_units_per_item);
  }
  return units;
}

u64 BackoffForFailureCount(const DynamicCleanupDebtSchedulerPolicy& policy,
                           u64 failure_count) {
  u64 backoff = policy.min_retry_backoff_microseconds;
  const u64 shifts = std::min<u64>(failure_count, 20);
  for (u64 i = 0; i < shifts; ++i) {
    if (backoff >= policy.max_retry_backoff_microseconds / 2u) {
      backoff = policy.max_retry_backoff_microseconds;
      break;
    }
    backoff *= 2u;
  }
  if (policy.max_retry_backoff_microseconds != 0) {
    backoff = std::min(backoff, policy.max_retry_backoff_microseconds);
  }
  return backoff;
}

u64 EffectiveTotalBudget(const DynamicCleanupDebtSchedulerRequest& request) {
  u64 budget = request.policy.max_total_work_units;
  if (request.foreground_work_active && request.policy.protect_foreground_work &&
      budget > 1) {
    budget = std::max<u64>(1, budget / 4u);
  }
  return budget;
}

bool AuthorityAvailableForSource(
    const DynamicCleanupDebtSchedulerRequest& request,
    const DynamicCleanupDebtSource& source) {
  if (!source.requires_mga_cleanup_horizon && !source.destructive_cleanup) {
    return true;
  }
  return request.engine_mga_authoritative &&
         request.cleanup_horizon.ok() &&
         request.cleanup_horizon.cleanup_horizon.valid();
}

DynamicCleanupDebtAssignment MakeAssignment(
    const DynamicCleanupDebtSource& source,
    DynamicCleanupDebtDecisionKind decision,
    u64 score,
    std::string diagnostic_code,
    std::string detail,
    u64 next_eligible_microseconds = 0) {
  DynamicCleanupDebtAssignment assignment;
  assignment.source = source;
  assignment.decision = decision;
  assignment.failure_mode = source.failure_mode;
  assignment.score = score;
  assignment.diagnostic_code = std::move(diagnostic_code);
  assignment.detail = std::move(detail);
  assignment.next_eligible_microseconds = next_eligible_microseconds;
  AddDecisionEvidence(&assignment, "cleanup_debt_family",
                      DynamicCleanupDebtFamilyName(source.family));
  AddDecisionEvidence(&assignment, "cleanup_debt_work_kind",
                      DynamicCleanupDebtWorkKindName(source.work_kind));
  if (decision != DynamicCleanupDebtDecisionKind::no_op) {
    AddDecisionEvidence(&assignment, "cleanup_debt_decision",
                        DynamicCleanupDebtDecisionKindName(decision));
  }
  AddDecisionEvidence(&assignment, "cleanup_debt_failure_mode",
                      DynamicCleanupDebtFailureModeName(source.failure_mode));
  AddDecisionEvidence(&assignment, "cleanup_debt_score",
                      std::to_string(score));
  AddDecisionEvidence(&assignment, "cleanup_debt_units",
                      std::to_string(source.debt_units));
  AddDecisionEvidence(&assignment, "cleanup_debt_bytes",
                      std::to_string(source.debt_bytes));
  AddDecisionEvidence(&assignment, "requires_mga_cleanup_horizon",
                      BoolText(source.requires_mga_cleanup_horizon));
  AddDecisionEvidence(&assignment, "destructive_cleanup",
                      BoolText(source.destructive_cleanup));
  AddDecisionEvidence(&assignment, "source_authoritative",
                      BoolText(source.source_authoritative));
  AddDecisionEvidence(&assignment, "recovery_proof_available",
                      BoolText(source.recovery_proof_available));
  AddDecisionEvidence(&assignment, "bounded_cleanup_available",
                      BoolText(source.bounded_cleanup_available));
  AddDecisionEvidence(&assignment, "diagnostic_code",
                      assignment.diagnostic_code);
  if (next_eligible_microseconds != 0) {
    AddDecisionEvidence(&assignment, "next_eligible_microseconds",
                        std::to_string(next_eligible_microseconds));
  }
  return assignment;
}

struct Candidate {
  DynamicCleanupDebtAssignment assignment;
  u64 requested_work_units = 0;
  bool consumed = false;
};

DynamicCleanupDebtFamilySummary* FindOrAddSummary(
    std::vector<DynamicCleanupDebtFamilySummary>* summaries,
    DynamicCleanupDebtFamily family) {
  for (auto& summary : *summaries) {
    if (summary.family == family) {
      return &summary;
    }
  }
  DynamicCleanupDebtFamilySummary summary;
  summary.family = family;
  summaries->push_back(summary);
  return &summaries->back();
}

u64 ScheduledFamilyUnits(const std::vector<DynamicCleanupDebtAssignment>& out,
                         DynamicCleanupDebtFamily family) {
  u64 units = 0;
  for (const auto& decision : out) {
    if (decision.scheduled() && decision.source.family == family) {
      units = SaturatingAdd(units, decision.scheduled_work_units);
    }
  }
  return units;
}

u64 ScheduledFamilyItems(const std::vector<DynamicCleanupDebtAssignment>& out,
                         DynamicCleanupDebtFamily family) {
  u64 items = 0;
  for (const auto& decision : out) {
    if (decision.scheduled() && decision.source.family == family) {
      ++items;
    }
  }
  return items;
}

bool ScheduleCandidate(Candidate* candidate,
                       const DynamicCleanupDebtSchedulerRequest& request,
                       u64 effective_total_budget,
                       u64* scheduled_units,
                       u64* scheduled_items,
                       std::vector<DynamicCleanupDebtAssignment>* out) {
  if (candidate == nullptr || candidate->consumed ||
      *scheduled_units >= effective_total_budget ||
      *scheduled_items >= request.policy.max_scheduled_items) {
    return false;
  }
  const auto family = candidate->assignment.source.family;
  const auto cap = EffectiveCap(request.policy, family);
  const u64 family_units = ScheduledFamilyUnits(*out, family);
  const u64 family_items = ScheduledFamilyItems(*out, family);
  if (family_units >= cap.max_work_units || family_items >= cap.max_items) {
    return false;
  }
  const u64 remaining_total = effective_total_budget - *scheduled_units;
  const u64 remaining_family = cap.max_work_units - family_units;
  u64 units = std::min(candidate->requested_work_units,
                       std::min(remaining_total, remaining_family));
  if (units == 0) {
    return false;
  }
  auto scheduled = candidate->assignment;
  scheduled.decision = DynamicCleanupDebtDecisionKind::scheduled;
  scheduled.scheduled_work_units = units;
  scheduled.priority_rank = *scheduled_items + 1;
  scheduled.next_eligible_microseconds =
      request.now_microseconds + request.policy.lease_duration_microseconds;
  scheduled.lease_token = scheduled.source.stable_work_key + ":lease:" +
                          std::to_string(request.now_microseconds);
  scheduled.diagnostic_code = "DYNAMIC_CLEANUP_DEBT.SCHEDULED";
  scheduled.detail = "bounded_cleanup_work_scheduled";
  AddDecisionEvidence(&scheduled, "cleanup_debt_decision", "scheduled");
  AddDecisionEvidence(&scheduled, "scheduled_work_units",
                      std::to_string(units));
  AddDecisionEvidence(&scheduled, "priority_rank",
                      std::to_string(scheduled.priority_rank));
  AddDecisionEvidence(&scheduled, "lease_until_microseconds",
                      std::to_string(scheduled.next_eligible_microseconds));
  AddDecisionEvidence(&scheduled, "lease_token", scheduled.lease_token);
  out->push_back(std::move(scheduled));
  candidate->consumed = true;
  *scheduled_units = SaturatingAdd(*scheduled_units, units);
  ++(*scheduled_items);
  return true;
}

void PublishSchedulerMetrics(const DynamicCleanupDebtSchedulerResult& result) {
  (void)metrics::SetGauge(
      "sb_dynamic_cleanup_debt_scheduled_work_units",
      metrics::Labels({{"component", "core.agents.cleanup_debt_scheduler"}}),
      static_cast<double>(result.scheduled_work_units),
      "dynamic_cleanup_debt_scheduler");
  (void)metrics::SetGauge(
      "sb_dynamic_cleanup_debt_scheduled_items",
      metrics::Labels({{"component", "core.agents.cleanup_debt_scheduler"}}),
      static_cast<double>(result.scheduled_count),
      "dynamic_cleanup_debt_scheduler");
  for (const auto& summary : result.family_summaries) {
    (void)metrics::SetGauge(
        "sb_dynamic_cleanup_debt_family_scheduled_work_units",
        metrics::Labels({{"component", "core.agents.cleanup_debt_scheduler"},
                         {"family", DynamicCleanupDebtFamilyName(summary.family)}}),
        static_cast<double>(summary.scheduled_work_units),
        "dynamic_cleanup_debt_scheduler");
  }
}

void FinishResultEvidence(DynamicCleanupDebtSchedulerResult* result,
                          const DynamicCleanupDebtSchedulerRequest& request) {
  AddEvidence(&result->evidence, "dynamic_cleanup_debt_scheduler",
              "dynamic_cleanup_debt_scheduler_v1");
  AddEvidence(&result->evidence, "authority_source",
              "durable_mga_transaction_inventory");
  AddEvidence(&result->evidence, "engine_mga_authoritative",
              BoolText(result->engine_mga_authoritative));
  AddEvidence(&result->evidence, "cleanup_horizon_authoritative",
              BoolText(result->cleanup_horizon_authoritative));
  AddEvidence(&result->evidence, "scheduler_enabled",
              BoolText(result->scheduler_enabled));
  AddEvidence(&result->evidence, "foreground_protected",
              BoolText(result->foreground_protected));
  AddEvidence(&result->evidence, "effective_total_work_units",
              std::to_string(result->effective_total_work_units));
  AddEvidence(&result->evidence, "scheduled_count",
              std::to_string(result->scheduled_count));
  AddEvidence(&result->evidence, "scheduled_work_units",
              std::to_string(result->scheduled_work_units));
  AddEvidence(&result->evidence, "fail_open_deferred_count",
              std::to_string(result->fail_open_deferred_count));
  AddEvidence(&result->evidence, "fail_closed_refusal_count",
              std::to_string(result->fail_closed_refusal_count));
  AddEvidence(&result->evidence, "parser_finality_authority", "false");
  AddEvidence(&result->evidence, "client_state_authority", "false");
  AddEvidence(&result->evidence, "timestamp_ordering_authority", "false");
  AddEvidence(&result->evidence, "uuid_ordering_authority", "false");
  AddEvidence(&result->evidence, "crud_event_stream_authority", "false");
  if (request.cleanup_horizon.cleanup_horizon.valid()) {
    AddEvidence(&result->evidence, "cleanup_horizon_local_transaction_id",
                std::to_string(request.cleanup_horizon.cleanup_horizon.value));
  }
  AddEvidence(&result->evidence, "diagnostic_code",
              result->diagnostic.diagnostic_code);
}

bool CandidateScoreHigher(const Candidate& left, const Candidate& right) {
  if (left.assignment.score != right.assignment.score) {
    return left.assignment.score > right.assignment.score;
  }
  return left.assignment.source.stable_work_key <
         right.assignment.source.stable_work_key;
}

}  // namespace

const char* DynamicCleanupDebtFamilyName(DynamicCleanupDebtFamily family) {
  switch (family) {
    case DynamicCleanupDebtFamily::version_chain:
      return "version_chain";
    case DynamicCleanupDebtFamily::exact_index_leaf:
      return "exact_index_leaf";
    case DynamicCleanupDebtFamily::secondary_delta_ledger:
      return "secondary_delta_ledger";
    case DynamicCleanupDebtFamily::summary_page_range:
      return "summary_page_range";
    case DynamicCleanupDebtFamily::large_value:
      return "large_value";
    case DynamicCleanupDebtFamily::hot_leaf:
      return "hot_leaf";
    case DynamicCleanupDebtFamily::nosql_key_value:
      return "nosql_key_value";
    case DynamicCleanupDebtFamily::nosql_document:
      return "nosql_document";
    case DynamicCleanupDebtFamily::nosql_search:
      return "nosql_search";
    case DynamicCleanupDebtFamily::nosql_vector:
      return "nosql_vector";
    case DynamicCleanupDebtFamily::nosql_graph:
      return "nosql_graph";
    case DynamicCleanupDebtFamily::nosql_time_series:
      return "nosql_time_series";
  }
  return "unknown";
}

const char* DynamicCleanupDebtWorkKindName(DynamicCleanupDebtWorkKind kind) {
  switch (kind) {
    case DynamicCleanupDebtWorkKind::storage_version_sweep:
      return "storage_version_sweep";
    case DynamicCleanupDebtWorkKind::exact_index_leaf_cleanup:
      return "exact_index_leaf_cleanup";
    case DynamicCleanupDebtWorkKind::secondary_delta_merge_or_cleanup:
      return "secondary_delta_merge_or_cleanup";
    case DynamicCleanupDebtWorkKind::summary_refresh_or_rebuild:
      return "summary_refresh_or_rebuild";
    case DynamicCleanupDebtWorkKind::large_value_reclaim:
      return "large_value_reclaim";
    case DynamicCleanupDebtWorkKind::hot_leaf_pressure_relief:
      return "hot_leaf_pressure_relief";
    case DynamicCleanupDebtWorkKind::nosql_key_value_ttl_retirement:
      return "nosql_key_value_ttl_retirement";
    case DynamicCleanupDebtWorkKind::nosql_key_value_generation_compaction:
      return "nosql_key_value_generation_compaction";
    case DynamicCleanupDebtWorkKind::nosql_document_generation_merge:
      return "nosql_document_generation_merge";
    case DynamicCleanupDebtWorkKind::nosql_search_segment_merge:
      return "nosql_search_segment_merge";
    case DynamicCleanupDebtWorkKind::nosql_vector_generation_retirement:
      return "nosql_vector_generation_retirement";
    case DynamicCleanupDebtWorkKind::nosql_graph_adjacency_compaction:
      return "nosql_graph_adjacency_compaction";
    case DynamicCleanupDebtWorkKind::nosql_time_series_bucket_retirement:
      return "nosql_time_series_bucket_retirement";
  }
  return "unknown";
}

const char* DynamicCleanupDebtDecisionKindName(
    DynamicCleanupDebtDecisionKind decision) {
  switch (decision) {
    case DynamicCleanupDebtDecisionKind::scheduled:
      return "scheduled";
    case DynamicCleanupDebtDecisionKind::no_op:
      return "no_op";
    case DynamicCleanupDebtDecisionKind::skipped_no_debt:
      return "skipped_no_debt";
    case DynamicCleanupDebtDecisionKind::deferred_cooldown:
      return "deferred_cooldown";
    case DynamicCleanupDebtDecisionKind::deferred_lease:
      return "deferred_lease";
    case DynamicCleanupDebtDecisionKind::deferred_contention:
      return "deferred_contention";
    case DynamicCleanupDebtDecisionKind::deferred_family_cap:
      return "deferred_family_cap";
    case DynamicCleanupDebtDecisionKind::refused_authority:
      return "refused_authority";
    case DynamicCleanupDebtDecisionKind::refused_budget:
      return "refused_budget";
    case DynamicCleanupDebtDecisionKind::refused_source:
      return "refused_source";
  }
  return "no_op";
}

const char* DynamicCleanupDebtFailureModeName(
    DynamicCleanupDebtFailureMode mode) {
  switch (mode) {
    case DynamicCleanupDebtFailureMode::fail_open_to_foreground:
      return "fail_open_to_foreground";
    case DynamicCleanupDebtFailureMode::fail_closed_retain_debt:
      return "fail_closed_retain_debt";
  }
  return "fail_closed_retain_debt";
}

DynamicCleanupDebtSchedulerResult PlanDynamicCleanupDebt(
    const DynamicCleanupDebtSchedulerRequest& request) {
  DynamicCleanupDebtSchedulerResult result;
  result.scheduler_enabled = request.policy.enabled;
  result.engine_mga_authoritative = request.engine_mga_authoritative;
  result.cleanup_horizon_authoritative = request.cleanup_horizon.ok();
  result.effective_total_work_units = EffectiveTotalBudget(request);
  result.foreground_protected =
      request.foreground_work_active && request.policy.protect_foreground_work;

  if (!request.policy.enabled) {
    result.status = SchedulerOkStatus();
    result.diagnostic = MakeDynamicCleanupDebtSchedulerDiagnostic(
        result.status,
        "DYNAMIC_CLEANUP_DEBT.DISABLED",
        "agents.dynamic_cleanup_debt.disabled");
    FinishResultEvidence(&result, request);
    return result;
  }
  if (request.policy.max_total_work_units == 0 ||
      request.policy.max_scheduled_items == 0 ||
      request.policy.max_work_units_per_item == 0) {
    result.status = SchedulerErrorStatus();
    result.fail_closed = true;
    result.diagnostic = MakeDynamicCleanupDebtSchedulerDiagnostic(
        result.status,
        "DYNAMIC_CLEANUP_DEBT.BUDGET_REQUIRED",
        "agents.dynamic_cleanup_debt.budget_required",
        "nonzero total item and per-item budgets are required");
    FinishResultEvidence(&result, request);
    return result;
  }

  std::vector<Candidate> candidates;
  std::vector<DynamicCleanupDebtAssignment> deferred;
  for (const auto& source : request.sources) {
    auto* summary = FindOrAddSummary(&result.family_summaries, source.family);
    ++summary->candidate_count;
    const u64 score = ScoreForSource(source);
    const auto failure_mode = source.failure_mode;

    if (!HasDebt(source)) {
      ++summary->skipped_count;
      deferred.push_back(MakeAssignment(
          source,
          DynamicCleanupDebtDecisionKind::skipped_no_debt,
          score,
          "DYNAMIC_CLEANUP_DEBT.NO_DEBT",
          "source reported no cleanup debt"));
      continue;
    }
    if (!source.source_authoritative ||
        !source.recovery_proof_available ||
        !source.bounded_cleanup_available) {
      ++summary->refused_count;
      if (failure_mode ==
          DynamicCleanupDebtFailureMode::fail_open_to_foreground) {
        ++result.fail_open_deferred_count;
      } else {
        ++result.fail_closed_refusal_count;
      }
      deferred.push_back(MakeAssignment(
          source,
          DynamicCleanupDebtDecisionKind::refused_source,
          score,
          "DYNAMIC_CLEANUP_DEBT.SOURCE_REFUSED",
          "source authority recovery proof and bounded cleanup evidence are required"));
      continue;
    }
    if (!AuthorityAvailableForSource(request, source)) {
      ++summary->refused_count;
      if (failure_mode ==
          DynamicCleanupDebtFailureMode::fail_open_to_foreground) {
        ++result.fail_open_deferred_count;
      } else {
        ++result.fail_closed_refusal_count;
      }
      deferred.push_back(MakeAssignment(
          source,
          DynamicCleanupDebtDecisionKind::refused_authority,
          score,
          "DYNAMIC_CLEANUP_DEBT.AUTHORITY_REFUSED",
          "authoritative MGA cleanup horizon is required"));
      continue;
    }
    if (source.lease_active &&
        source.lease_until_microseconds > request.now_microseconds) {
      ++summary->skipped_count;
      deferred.push_back(MakeAssignment(
          source,
          DynamicCleanupDebtDecisionKind::deferred_lease,
          score,
          "DYNAMIC_CLEANUP_DEBT.LEASE_HELD",
          "cleanup work already has an active lease",
          source.lease_until_microseconds));
      continue;
    }
    if (source.worker_contention_observed) {
      ++summary->skipped_count;
      deferred.push_back(MakeAssignment(
          source,
          DynamicCleanupDebtDecisionKind::deferred_contention,
          score,
          "DYNAMIC_CLEANUP_DEBT.WORKER_CONTENTION_BACKOFF",
          "worker contention observed; cleanup source is backed off",
          request.now_microseconds +
              request.policy.contention_backoff_microseconds));
      continue;
    }
    u64 next_eligible = source.next_eligible_microseconds;
    if (source.failure_count != 0 && source.last_attempt_microseconds != 0) {
      next_eligible = std::max(
          next_eligible,
          source.last_attempt_microseconds +
              BackoffForFailureCount(request.policy, source.failure_count));
    }
    if (next_eligible > request.now_microseconds) {
      ++summary->skipped_count;
      deferred.push_back(MakeAssignment(
          source,
          DynamicCleanupDebtDecisionKind::deferred_cooldown,
          score,
          "DYNAMIC_CLEANUP_DEBT.COOLDOWN",
          "cleanup source is in cooldown backoff",
          next_eligible));
      continue;
    }

    Candidate candidate;
    candidate.assignment = MakeAssignment(
        source,
        DynamicCleanupDebtDecisionKind::no_op,
        score,
        "DYNAMIC_CLEANUP_DEBT.CANDIDATE",
        "cleanup source admitted for scheduling");
    candidate.requested_work_units = RequestedWorkUnits(source, request.policy);
    candidates.push_back(std::move(candidate));
  }

  std::sort(candidates.begin(), candidates.end(), CandidateScoreHigher);
  u64 scheduled_units = 0;
  u64 scheduled_items = 0;

  for (const auto family : AllFamilies()) {
    auto best = candidates.end();
    for (auto it = candidates.begin(); it != candidates.end(); ++it) {
      if (!it->consumed && it->assignment.source.family == family &&
          (best == candidates.end() || CandidateScoreHigher(*it, *best))) {
        best = it;
      }
    }
    if (best != candidates.end()) {
      (void)ScheduleCandidate(&(*best),
                              request,
                              result.effective_total_work_units,
                              &scheduled_units,
                              &scheduled_items,
                              &result.decisions);
    }
  }

  bool progressed = true;
  while (progressed &&
         scheduled_units < result.effective_total_work_units &&
         scheduled_items < request.policy.max_scheduled_items) {
    progressed = false;
    for (auto& candidate : candidates) {
      if (candidate.consumed) {
        continue;
      }
      if (ScheduleCandidate(&candidate,
                            request,
                            result.effective_total_work_units,
                            &scheduled_units,
                            &scheduled_items,
                            &result.decisions)) {
        progressed = true;
      }
      if (scheduled_units >= result.effective_total_work_units ||
          scheduled_items >= request.policy.max_scheduled_items) {
        break;
      }
    }
  }

  for (auto& candidate : candidates) {
    if (candidate.consumed) {
      continue;
    }
    const auto family = candidate.assignment.source.family;
    const auto cap = EffectiveCap(request.policy, family);
    const u64 family_units = ScheduledFamilyUnits(result.decisions, family);
    const u64 family_items = ScheduledFamilyItems(result.decisions, family);
    if (family_units >= cap.max_work_units || family_items >= cap.max_items) {
      candidate.assignment.decision =
          DynamicCleanupDebtDecisionKind::deferred_family_cap;
      candidate.assignment.diagnostic_code =
          "DYNAMIC_CLEANUP_DEBT.FAMILY_CAP";
      candidate.assignment.detail =
          "per-family cleanup cap deferred remaining debt";
    } else {
      candidate.assignment.decision =
          DynamicCleanupDebtDecisionKind::refused_budget;
      candidate.assignment.diagnostic_code =
          "DYNAMIC_CLEANUP_DEBT.TOTAL_BUDGET";
      candidate.assignment.detail =
          "total cleanup budget deferred remaining debt";
    }
    AddDecisionEvidence(&candidate.assignment,
                        "cleanup_debt_decision",
                        DynamicCleanupDebtDecisionKindName(
                            candidate.assignment.decision));
    deferred.push_back(std::move(candidate.assignment));
    auto* summary = FindOrAddSummary(&result.family_summaries, family);
    ++summary->skipped_count;
  }

  result.decisions.insert(result.decisions.end(),
                          std::make_move_iterator(deferred.begin()),
                          std::make_move_iterator(deferred.end()));
  result.scheduled_count = scheduled_items;
  result.scheduled_work_units = scheduled_units;
  for (const auto& decision : result.decisions) {
    auto* summary =
        FindOrAddSummary(&result.family_summaries, decision.source.family);
    if (decision.scheduled()) {
      ++summary->scheduled_count;
      summary->scheduled_work_units = SaturatingAdd(
          summary->scheduled_work_units, decision.scheduled_work_units);
    }
  }

  result.fail_closed = result.scheduled_count == 0 &&
                       result.fail_closed_refusal_count != 0;
  result.status = result.fail_closed ? SchedulerErrorStatus()
                                     : SchedulerOkStatus();
  result.diagnostic = MakeDynamicCleanupDebtSchedulerDiagnostic(
      result.status,
      result.scheduled_count == 0
          ? "DYNAMIC_CLEANUP_DEBT.NO_WORK_SCHEDULED"
          : "DYNAMIC_CLEANUP_DEBT.SCHEDULED",
      result.scheduled_count == 0
          ? "agents.dynamic_cleanup_debt.no_work_scheduled"
          : "agents.dynamic_cleanup_debt.scheduled",
      std::to_string(result.scheduled_count));
  FinishResultEvidence(&result, request);
  PublishSchedulerMetrics(result);
  return result;
}

DiagnosticRecord MakeDynamicCleanupDebtSchedulerDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.agents.dynamic_cleanup_debt_scheduler",
                        status.ok()
                            ? ""
                            : "retain cleanup debt until authoritative local MGA evidence and bounded worker budget are available");
}

DynamicCleanupDebtSource DynamicCleanupDebtSourceFromVersionChainMetrics(
    std::string stable_work_key,
    const implemented_agents::StorageVersionCleanupPressureMetrics& metrics) {
  DynamicCleanupDebtSource source;
  source.family = DynamicCleanupDebtFamily::version_chain;
  source.work_kind = DynamicCleanupDebtWorkKind::storage_version_sweep;
  source.failure_mode =
      DynamicCleanupDebtFailureMode::fail_closed_retain_debt;
  source.stable_work_key = std::move(stable_work_key);
  source.debt_units = SaturatingAdd(metrics.cleanup_candidate_row_versions,
                                    metrics.blocked_row_versions);
  source.debt_bytes = SaturatingMul(source.debt_units, 128);
  source.estimated_work_units = metrics.cleanup_candidate_row_versions;
  source.max_work_units = metrics.cleanup_candidate_row_versions;
  source.blocker_count = metrics.active_cleanup_blockers;
  source.source_detail = "version_chain_cleanup_debt";
  AddSourceEvidence(&source, "source", "storage_version_cleanup_pressure");
  AddSourceEvidence(&source, "cleanup_candidate_row_versions",
                    std::to_string(metrics.cleanup_candidate_row_versions));
  AddSourceEvidence(&source, "blocked_row_versions",
                    std::to_string(metrics.blocked_row_versions));
  AddSourceEvidence(&source, "active_cleanup_blockers",
                    std::to_string(metrics.active_cleanup_blockers));
  return source;
}

DynamicCleanupDebtSource DynamicCleanupDebtSourceFromExactIndexLeafPressure(
    std::string stable_work_key,
    const ExactIndexLeafPressureDecision& decision) {
  DynamicCleanupDebtSource source;
  source.family = DynamicCleanupDebtFamily::exact_index_leaf;
  source.work_kind = DynamicCleanupDebtWorkKind::exact_index_leaf_cleanup;
  source.failure_mode =
      DynamicCleanupDebtFailureMode::fail_open_to_foreground;
  source.stable_work_key = std::move(stable_work_key);
  source.debt_units = std::max<u64>(
      decision.required_reclaim_count,
      SaturatingAdd(decision.counters.retained_count,
                    decision.counters.cleaned_count));
  if (source.debt_units == 0 && decision.leaf_pressure_detected) {
    source.debt_units = 1;
  }
  source.debt_bytes = SaturatingMul(source.debt_units, 96);
  source.estimated_work_units = source.debt_units;
  source.max_work_units = source.debt_units;
  source.priority_boost = decision.split_selected ? 100 : 25;
  source.source_authoritative =
      decision.mga_authority_source == "durable_mga_transaction_inventory";
  source.recovery_proof_available = !decision.cleanup_result.fail_closed ||
                                    !decision.cleanup_attempted;
  source.bounded_cleanup_available = decision.cleanup_attempted ||
                                     decision.leaf_pressure_detected;
  source.source_detail = "exact_index_leaf_cleanup_debt";
  AddSourceEvidence(&source, "source", "exact_index_leaf_pressure");
  AddSourceEvidence(&source, "leaf_pressure_detected",
                    BoolText(decision.leaf_pressure_detected));
  AddSourceEvidence(&source, "required_reclaim_count",
                    std::to_string(decision.required_reclaim_count));
  AddSourceEvidence(&source, "split_selected",
                    BoolText(decision.split_selected));
  AddSourceEvidence(&source, "mga_authority_source",
                    decision.mga_authority_source);
  return source;
}

DynamicCleanupDebtSource DynamicCleanupDebtSourceFromSecondaryDeltaLedger(
    std::string stable_work_key,
    const PersistentSecondaryIndexDeltaLedger& ledger) {
  DynamicCleanupDebtSource source;
  source.family = DynamicCleanupDebtFamily::secondary_delta_ledger;
  source.work_kind =
      DynamicCleanupDebtWorkKind::secondary_delta_merge_or_cleanup;
  source.failure_mode =
      DynamicCleanupDebtFailureMode::fail_closed_retain_debt;
  source.stable_work_key = std::move(stable_work_key);
  source.debt_units = ledger.records.size();
  source.debt_bytes =
      ledger.encoded_bytes == 0 ? SaturatingMul(source.debt_units, 128)
                                : ledger.encoded_bytes;
  source.estimated_work_units = source.debt_units;
  source.max_work_units = source.debt_units;
  const auto recovery = idx::ClassifySecondaryIndexDeltaLedgerForRecovery(ledger);
  source.recovery_proof_available = recovery.status.ok() && !recovery.fail_closed;
  source.source_detail = "secondary_delta_ledger_debt";
  AddSourceEvidence(&source, "source", "secondary_delta_ledger");
  AddSourceEvidence(&source, "delta_ledger_records",
                    std::to_string(ledger.records.size()));
  AddSourceEvidence(&source, "delta_ledger_recovery_class",
                    idx::SecondaryIndexDeltaLedgerRecoveryClassName(
                        recovery.recovery_class));
  AddSourceEvidence(&source, "delta_ledger_recovery_action",
                    idx::SecondaryIndexDeltaLedgerRecoveryActionName(
                        recovery.action));
  return source;
}

DynamicCleanupDebtSource DynamicCleanupDebtSourceFromSecondaryChangeBuffer(
    std::string stable_work_key,
    const PageAwareSecondaryChangeBufferRequest& request,
    const PageAwareSecondaryChangeBufferDecision& decision) {
  DynamicCleanupDebtSource source;
  source.family = DynamicCleanupDebtFamily::secondary_delta_ledger;
  source.work_kind =
      DynamicCleanupDebtWorkKind::secondary_delta_merge_or_cleanup;
  source.failure_mode =
      DynamicCleanupDebtFailureMode::fail_closed_retain_debt;
  source.stable_work_key = std::move(stable_work_key);
  source.debt_units =
      SaturatingAdd(request.pending_delta_count, request.incoming_delta_count);
  source.debt_bytes =
      SaturatingAdd(request.pending_delta_bytes, request.incoming_delta_bytes);
  source.estimated_work_units = source.debt_units;
  source.max_work_units = source.debt_units;
  source.priority_boost =
      decision.reason == "secondary_change_buffer_backlog_cap_exceeded" ? 120
                                                                        : 30;
  source.source_authoritative =
      decision.durable_mga_inventory_remains_authority &&
      !decision.finality_map_transaction_authority;
  source.recovery_proof_available =
      request.persisted_recovery_proof_available &&
      decision.recovery_action != "fail_closed";
  source.bounded_cleanup_available =
      request.max_pending_delta_count != 0 &&
      request.max_pending_delta_bytes != 0;
  source.source_detail = "page_aware_secondary_change_buffer_debt";
  AddSourceEvidence(&source, "source", "secondary_change_buffer");
  AddSourceEvidence(&source, "pending_delta_count",
                    std::to_string(request.pending_delta_count));
  AddSourceEvidence(&source, "incoming_delta_count",
                    std::to_string(request.incoming_delta_count));
  AddSourceEvidence(&source, "change_buffer_selected",
                    BoolText(decision.selected));
  AddSourceEvidence(&source, "change_buffer_reason", decision.reason);
  return source;
}

DynamicCleanupDebtSource DynamicCleanupDebtSourceFromPageRangeSummaries(
    std::string stable_work_key,
    const std::vector<PageExtentSummaryMetadata>& summaries) {
  DynamicCleanupDebtSource source;
  source.family = DynamicCleanupDebtFamily::summary_page_range;
  source.work_kind = DynamicCleanupDebtWorkKind::summary_refresh_or_rebuild;
  source.failure_mode =
      DynamicCleanupDebtFailureMode::fail_open_to_foreground;
  source.stable_work_key = std::move(stable_work_key);
  source.requires_mga_cleanup_horizon = false;
  source.destructive_cleanup = false;
  source.source_detail = "summary_page_range_refresh_debt";
  bool authority_clean = true;
  for (const auto& summary : summaries) {
    authority_clean = authority_clean &&
                      idx::PageExtentSummaryAuthorityFlagsClean(summary);
    switch (summary.status) {
      case idx::PageExtentSummaryStatus::missing:
      case idx::PageExtentSummaryStatus::stale:
      case idx::PageExtentSummaryStatus::corrupt:
      case idx::PageExtentSummaryStatus::incompatible_format:
        ++source.debt_units;
        source.debt_bytes = SaturatingAdd(source.debt_bytes,
                                          SaturatingMul(summary.row_count, 32));
        break;
      case idx::PageExtentSummaryStatus::current:
        break;
    }
  }
  source.estimated_work_units = source.debt_units;
  source.max_work_units = source.debt_units;
  source.source_authoritative = authority_clean;
  AddSourceEvidence(&source, "source", "page_extent_summary_metadata");
  AddSourceEvidence(&source, "summary_count", std::to_string(summaries.size()));
  AddSourceEvidence(&source, "summary_debt_ranges",
                    std::to_string(source.debt_units));
  AddSourceEvidence(&source, "summary_authority_clean",
                    BoolText(authority_clean));
  return source;
}

DynamicCleanupDebtSource DynamicCleanupDebtSourceFromTimeRangePrunePlan(
    std::string stable_work_key,
    const TimeRangeSummaryPrunePlan& plan) {
  DynamicCleanupDebtSource source;
  source.family = DynamicCleanupDebtFamily::summary_page_range;
  source.work_kind = DynamicCleanupDebtWorkKind::summary_refresh_or_rebuild;
  source.failure_mode =
      DynamicCleanupDebtFailureMode::fail_open_to_foreground;
  source.stable_work_key = std::move(stable_work_key);
  source.requires_mga_cleanup_horizon = false;
  source.destructive_cleanup = false;
  source.source_authoritative = !plan.summary_metadata_finality_authority &&
                                !plan.summary_metadata_visibility_authority;
  source.debt_units =
      plan.exact_fallback_required ? std::max<u64>(1, plan.counters.ranges_scanned)
                                   : 0;
  source.debt_bytes = SaturatingMul(plan.counters.pages_scanned, 4096);
  source.estimated_work_units = source.debt_units;
  source.max_work_units = source.debt_units;
  source.source_detail = "time_range_summary_refresh_debt";
  AddSourceEvidence(&source, "source", "time_range_summary_prune_plan");
  AddSourceEvidence(&source, "exact_fallback_required",
                    BoolText(plan.exact_fallback_required));
  AddSourceEvidence(&source, "fallback_reason", plan.fallback_reason);
  AddSourceEvidence(&source, "ranges_scanned",
                    std::to_string(plan.counters.ranges_scanned));
  return source;
}

DynamicCleanupDebtSource DynamicCleanupDebtSourceFromLargeValueDebt(
    std::string stable_work_key,
    u64 orphan_value_count,
    u64 orphan_bytes,
    u64 pinned_value_count) {
  DynamicCleanupDebtSource source;
  source.family = DynamicCleanupDebtFamily::large_value;
  source.work_kind = DynamicCleanupDebtWorkKind::large_value_reclaim;
  source.failure_mode =
      DynamicCleanupDebtFailureMode::fail_closed_retain_debt;
  source.stable_work_key = std::move(stable_work_key);
  source.debt_units = SaturatingAdd(orphan_value_count, pinned_value_count);
  source.debt_bytes = orphan_bytes;
  source.estimated_work_units = orphan_value_count;
  source.max_work_units = orphan_value_count;
  source.blocker_count = pinned_value_count;
  source.source_detail = "large_value_cleanup_debt";
  AddSourceEvidence(&source, "source", "large_value_cleanup_debt");
  AddSourceEvidence(&source, "orphan_value_count",
                    std::to_string(orphan_value_count));
  AddSourceEvidence(&source, "orphan_bytes", std::to_string(orphan_bytes));
  AddSourceEvidence(&source, "pinned_value_count",
                    std::to_string(pinned_value_count));
  return source;
}

DynamicCleanupDebtSource DynamicCleanupDebtSourceFromHotLeafPressure(
    std::string stable_work_key,
    const PageAwareSecondaryChangeBufferRequest& request,
    const PageAwareSecondaryChangeBufferDecision& decision) {
  DynamicCleanupDebtSource source;
  source.family = DynamicCleanupDebtFamily::hot_leaf;
  source.work_kind = DynamicCleanupDebtWorkKind::hot_leaf_pressure_relief;
  source.failure_mode =
      DynamicCleanupDebtFailureMode::fail_open_to_foreground;
  source.stable_work_key = std::move(stable_work_key);
  source.debt_units =
      SaturatingAdd(request.pending_delta_count, request.incoming_delta_count);
  if (source.debt_units == 0 &&
      (!request.target_page_cold || decision.counters.hot_page_refusals != 0)) {
    source.debt_units = 1;
  }
  source.debt_bytes =
      SaturatingAdd(request.pending_delta_bytes, request.incoming_delta_bytes);
  source.estimated_work_units = source.debt_units;
  source.max_work_units = source.debt_units;
  source.priority_boost =
      decision.counters.hot_page_refusals != 0 ? 110 : 40;
  source.source_authoritative =
      decision.durable_mga_inventory_remains_authority &&
      !decision.finality_map_transaction_authority;
  source.source_detail = "hot_leaf_cleanup_debt";
  AddSourceEvidence(&source, "source", "hot_leaf_pressure");
  AddSourceEvidence(&source, "target_page_cold",
                    BoolText(request.target_page_cold));
  AddSourceEvidence(&source, "target_page_random_io_score",
                    std::to_string(request.target_page_random_io_score));
  AddSourceEvidence(&source, "hot_page_refusals",
                    std::to_string(decision.counters.hot_page_refusals));
  AddSourceEvidence(&source, "change_buffer_reason", decision.reason);
  return source;
}

}  // namespace scratchbird::core::agents
