// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_statistics_lifecycle.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

void AddEvidence(OptimizerStatisticsLifecycleResult* result,
                 std::string evidence) {
  if (result == nullptr) return;
  result->evidence.push_back(std::move(evidence));
}

OptimizerStatisticsLifecycleResult Refuse(const OptimizerStatisticsLifecycleRequest& request,
                                          std::string code,
                                          std::string evidence) {
  OptimizerStatisticsLifecycleResult result;
  result.decision = code == "SB_OPT_STATS_LIFECYCLE.NO_REFRESH_NEEDED"
                        ? OptimizerStatisticsLifecycleDecision::kNoRefreshNeeded
                        : OptimizerStatisticsLifecycleDecision::kRefused;
  result.accepted = false;
  result.refresh_needed = false;
  result.next_stats_epoch = request.current_stats_epoch;
  result.next_catalog_epoch = request.catalog_epoch;
  result.next_stats_visibility_epoch = request.stats_visibility_epoch;
  result.diagnostic_code = std::move(code);
  AddEvidence(&result, std::move(evidence));
  AddEvidence(&result, "trigger=" +
                           std::string(OptimizerStatisticsLifecycleTriggerName(
                               request.trigger)));
  return result;
}

bool CurrentStatsAreStale(const OptimizerStatisticsLifecycleRequest& request) {
  if (request.current_freshness != OptimizerStatsFreshnessState::kFresh) {
    return true;
  }
  return request.rows_modified_since_stats >=
         std::max<std::uint64_t>(1, request.stale_row_threshold);
}

std::uint64_t NextEpoch(std::uint64_t left, std::uint64_t right) {
  return std::max(left, right) + 1;
}

void AddCommonAuthorityEvidence(OptimizerStatisticsLifecycleResult* result) {
  AddEvidence(result, "lifecycle_metadata_only=true");
  AddEvidence(result, "advisor_refresh_metadata_only=true");
  AddEvidence(result, "mga_visibility_authority=engine_recheck_required");
  AddEvidence(result, "mga_finality_authority=engine_transaction_inventory");
  AddEvidence(result, "security_recheck=required");
  AddEvidence(result, "parser_or_donor_authority=false");
  AddEvidence(result, "result_semantics_changed=false");
}

std::vector<OptimizerStatisticsLifecycleRebuildPlan> BuildRebuildPlans(
    const std::vector<std::string>& column_uuids,
    const std::string& relation_uuid,
    const char* family,
    std::uint64_t target_count) {
  std::vector<OptimizerStatisticsLifecycleRebuildPlan> plans;
  if (target_count == 0) return plans;
  for (const auto& column_uuid : column_uuids) {
    if (column_uuid.empty()) continue;
    OptimizerStatisticsLifecycleRebuildPlan plan;
    plan.column_uuid = column_uuid;
    plan.statistic_uuid = relation_uuid + ":" + column_uuid + ":" + family;
    plan.target_entry_count = target_count;
    plans.push_back(std::move(plan));
  }
  return plans;
}

bool NeedsRefresh(const OptimizerStatisticsLifecycleRequest& request) {
  switch (request.trigger) {
    case OptimizerStatisticsLifecycleTrigger::kManualAnalyze:
      return true;
    case OptimizerStatisticsLifecycleTrigger::kSampledRefresh:
      return request.sampled_rows != 0 && request.total_rows_estimate != 0;
    case OptimizerStatisticsLifecycleTrigger::kStaleDetection:
      return CurrentStatsAreStale(request);
    case OptimizerStatisticsLifecycleTrigger::kPostBulkRefresh:
      return request.bulk_rows_written != 0;
    case OptimizerStatisticsLifecycleTrigger::kHistogramRebuild:
      return request.histogram_bucket_target != 0 && !request.column_uuids.empty();
    case OptimizerStatisticsLifecycleTrigger::kMcvRebuild:
      return request.mcv_entry_target != 0 && !request.column_uuids.empty();
    case OptimizerStatisticsLifecycleTrigger::kAdvisorSafeRefresh:
      return CurrentStatsAreStale(request) || request.rows_modified_since_stats != 0;
    case OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance:
      return request.agent_policy_safe &&
             (CurrentStatsAreStale(request) ||
              request.bulk_rows_written != 0 ||
              request.histogram_bucket_target != 0 ||
              request.mcv_entry_target != 0);
  }
  return false;
}

bool TriggerRequiresAgentSchedule(OptimizerStatisticsLifecycleTrigger trigger) {
  return trigger == OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance;
}

std::string AdmissionDiagnostic(OptimizerStatisticsLifecycleTrigger trigger) {
  switch (trigger) {
    case OptimizerStatisticsLifecycleTrigger::kManualAnalyze:
      return "SB_OPT_STATS_LIFECYCLE.MANUAL_ANALYZE_ADMITTED";
    case OptimizerStatisticsLifecycleTrigger::kSampledRefresh:
      return "SB_OPT_STATS_LIFECYCLE.SAMPLED_REFRESH_ADMITTED";
    case OptimizerStatisticsLifecycleTrigger::kStaleDetection:
      return "SB_OPT_STATS_LIFECYCLE.STALE_REFRESH_ADMITTED";
    case OptimizerStatisticsLifecycleTrigger::kPostBulkRefresh:
      return "SB_OPT_STATS_LIFECYCLE.POST_BULK_REFRESH_ADMITTED";
    case OptimizerStatisticsLifecycleTrigger::kHistogramRebuild:
      return "SB_OPT_STATS_LIFECYCLE.HISTOGRAM_REBUILD_ADMITTED";
    case OptimizerStatisticsLifecycleTrigger::kMcvRebuild:
      return "SB_OPT_STATS_LIFECYCLE.MCV_REBUILD_ADMITTED";
    case OptimizerStatisticsLifecycleTrigger::kAdvisorSafeRefresh:
      return "SB_OPT_STATS_LIFECYCLE.ADVISOR_SAFE_REFRESH_ADMITTED";
    case OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance:
      return "SB_OPT_STATS_LIFECYCLE.AGENT_AUTO_MAINTENANCE_ADMITTED";
  }
  return "SB_OPT_STATS_LIFECYCLE.ADMITTED";
}

void AddTriggerEvidence(const OptimizerStatisticsLifecycleRequest& request,
                        OptimizerStatisticsLifecycleResult* result) {
  AddEvidence(result, "trigger=" +
                          std::string(OptimizerStatisticsLifecycleTriggerName(
                              request.trigger)));
  switch (request.trigger) {
    case OptimizerStatisticsLifecycleTrigger::kManualAnalyze:
      AddEvidence(result, "manual_analyze_admitted=true");
      break;
    case OptimizerStatisticsLifecycleTrigger::kSampledRefresh:
      AddEvidence(result, "sampled_refresh_admitted=true");
      break;
    case OptimizerStatisticsLifecycleTrigger::kStaleDetection:
      AddEvidence(result, "stale_stat_detected=true");
      break;
    case OptimizerStatisticsLifecycleTrigger::kPostBulkRefresh:
      AddEvidence(result, "post_bulk_refresh_admitted=true");
      break;
    case OptimizerStatisticsLifecycleTrigger::kHistogramRebuild:
      AddEvidence(result, "histogram_rebuild=true");
      break;
    case OptimizerStatisticsLifecycleTrigger::kMcvRebuild:
      AddEvidence(result, "mcv_rebuild=true");
      break;
    case OptimizerStatisticsLifecycleTrigger::kAdvisorSafeRefresh:
      AddEvidence(result, "advisor_safe_refresh=true");
      break;
    case OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance:
      AddEvidence(result, "agent_auto_maintenance_policy_safe=true");
      break;
  }
}

void MaybeBuildPlannedTableStats(const OptimizerStatisticsLifecycleRequest& request,
                                 OptimizerStatisticsLifecycleResult* result) {
  if (request.total_rows_estimate == 0 || result == nullptr) return;

  AnalyzeSampleInput input;
  input.relation_uuid = request.relation_uuid;
  input.sampled_rows = request.sampled_rows == 0 ? request.total_rows_estimate
                                                 : request.sampled_rows;
  input.total_rows_estimate = request.total_rows_estimate;
  input.page_count = request.page_count;
  input.average_row_bytes = request.average_row_bytes;
  input.stats_epoch = result->next_stats_epoch;
  input.catalog_epoch = result->next_catalog_epoch;
  result->planned_table_stats = BuildTableStatsFromAnalyzeSample(input);
  result->planned_table_stats.identity.transaction_visibility_epoch =
      result->next_stats_visibility_epoch;
  result->has_planned_table_stats = true;
}

}  // namespace

const char* OptimizerStatisticsLifecycleTriggerName(
    OptimizerStatisticsLifecycleTrigger trigger) {
  switch (trigger) {
    case OptimizerStatisticsLifecycleTrigger::kManualAnalyze:
      return "manual_analyze";
    case OptimizerStatisticsLifecycleTrigger::kSampledRefresh:
      return "sampled_refresh";
    case OptimizerStatisticsLifecycleTrigger::kStaleDetection:
      return "stale_detection";
    case OptimizerStatisticsLifecycleTrigger::kPostBulkRefresh:
      return "post_bulk_refresh";
    case OptimizerStatisticsLifecycleTrigger::kHistogramRebuild:
      return "histogram_rebuild";
    case OptimizerStatisticsLifecycleTrigger::kMcvRebuild:
      return "mcv_rebuild";
    case OptimizerStatisticsLifecycleTrigger::kAdvisorSafeRefresh:
      return "advisor_safe_refresh";
    case OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance:
      return "agent_auto_maintenance";
  }
  return "manual_analyze";
}

OptimizerStatisticsLifecycleResult EvaluateOptimizerStatisticsLifecycle(
    const OptimizerStatisticsLifecycleRequest& request) {
  if (request.relation_uuid.empty()) {
    return Refuse(request,
                  "SB_OPT_STATS_LIFECYCLE.OBJECT_REQUIRED",
                  "object_required");
  }
  if (!request.policy_enabled || !request.agent_policy_safe) {
    return Refuse(request,
                  "SB_OPT_STATS_LIFECYCLE.POLICY_DISABLED",
                  "policy_disabled");
  }
  if (!request.security_context_present) {
    return Refuse(request,
                  "SB_OPT_STATS_LIFECYCLE.MISSING_SECURITY_CONTEXT",
                  "missing_security_context");
  }
  if (!request.grants_proven) {
    return Refuse(request,
                  "SB_OPT_STATS_LIFECYCLE.MISSING_GRANTS",
                  "missing_grants");
  }
  if (!request.mga_visibility_recheck_present) {
    return Refuse(request,
                  "SB_OPT_STATS_LIFECYCLE.MISSING_MGA_RECHECK",
                  "missing_mga_recheck");
  }
  if (!request.security_recheck_present) {
    return Refuse(request,
                  "SB_OPT_STATS_LIFECYCLE.MISSING_SECURITY_RECHECK",
                  "missing_security_recheck");
  }
  if (!request.advisory_only || request.parser_or_donor_authority) {
    return Refuse(request,
                  "SB_OPT_STATS_LIFECYCLE.UNSAFE_PARSER_DONOR_AUTHORITY",
                  "unsafe_parser_or_donor_authority");
  }
  if (!request.epoch_evidence_present ||
      request.request_stats_epoch == 0 ||
      request.catalog_epoch == 0 ||
      request.security_epoch == 0 ||
      request.policy_epoch == 0 ||
      request.stats_visibility_epoch == 0) {
    return Refuse(request,
                  "SB_OPT_STATS_LIFECYCLE.MISSING_EPOCH_EVIDENCE",
                  "missing_epoch_evidence");
  }
  if (request.current_stats_epoch != 0 &&
      request.request_stats_epoch < request.current_stats_epoch) {
    return Refuse(request,
                  "SB_OPT_STATS_LIFECYCLE.STALE_EPOCH",
                  "stale_epoch");
  }
  if (!request.stats_compatible) {
    return Refuse(request,
                  "SB_OPT_STATS_LIFECYCLE.STATS_INCOMPATIBLE",
                  "stats_incompatible");
  }
  if (!request.catalog_descriptor_present) {
    return Refuse(request,
                  "SB_OPT_STATS_LIFECYCLE.MISSING_CATALOG_DESCRIPTOR",
                  "missing_catalog_descriptor");
  }
  if (!request.catalog_write_admitted) {
    return Refuse(request,
                  "SB_OPT_STATS_LIFECYCLE.CATALOG_WRITE_NOT_ADMITTED",
                  "catalog_write_not_admitted");
  }
  if (TriggerRequiresAgentSchedule(request.trigger) &&
      !request.agent_runtime_registered) {
    return Refuse(request,
                  "SB_OPT_STATS_LIFECYCLE.AGENT_RUNTIME_NOT_REGISTERED",
                  "agent_runtime_not_registered");
  }
  if (TriggerRequiresAgentSchedule(request.trigger) &&
      !request.agent_schedule_admitted) {
    return Refuse(request,
                  "SB_OPT_STATS_LIFECYCLE.AGENT_SCHEDULE_NOT_ADMITTED",
                  "agent_schedule_not_admitted");
  }
  if (request.require_fresh_current_stats &&
      request.current_freshness != OptimizerStatsFreshnessState::kFresh) {
    return Refuse(request,
                  "SB_OPT_STATS_LIFECYCLE.STATS_STALE",
                  "stats_stale");
  }
  if (request.trigger == OptimizerStatisticsLifecycleTrigger::kSampledRefresh &&
      (request.sampled_rows == 0 || request.total_rows_estimate == 0)) {
    return Refuse(request,
                  "SB_OPT_STATS_LIFECYCLE.SAMPLE_REQUIRED",
                  "sample_required");
  }
  if (!NeedsRefresh(request)) {
    return Refuse(request,
                  "SB_OPT_STATS_LIFECYCLE.NO_REFRESH_NEEDED",
                  "no_refresh_need");
  }

  OptimizerStatisticsLifecycleResult result;
  result.decision = OptimizerStatisticsLifecycleDecision::kAdmitted;
  result.accepted = true;
  result.refresh_needed = true;
  result.advisor_metadata_only = true;
  result.agent_action_safe =
      request.trigger == OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance;
  result.catalog_update_planned = true;
  result.agent_schedule_planned = result.agent_action_safe;
  result.histogram_rebuild =
      request.trigger == OptimizerStatisticsLifecycleTrigger::kHistogramRebuild;
  result.mcv_rebuild =
      request.trigger == OptimizerStatisticsLifecycleTrigger::kMcvRebuild;
  result.next_stats_epoch =
      NextEpoch(request.current_stats_epoch, request.request_stats_epoch);
  result.next_catalog_epoch = request.catalog_epoch;
  result.next_stats_visibility_epoch = request.stats_visibility_epoch + 1;
  result.diagnostic_code = AdmissionDiagnostic(request.trigger);

  AddTriggerEvidence(request, &result);
  AddCommonAuthorityEvidence(&result);
  AddEvidence(&result, "catalog_epoch_compatible=true");
  AddEvidence(&result, "security_epoch_compatible=true");
  AddEvidence(&result, "policy_epoch_compatible=true");
  AddEvidence(&result, "stats_epoch_advanced=true");
  AddEvidence(&result, "stats_visibility_epoch_advanced=metadata_only");
  AddEvidence(&result, "catalog_stats_descriptor_update=planned");
  AddEvidence(&result, "catalog_stats_epoch_persist=planned");
  AddEvidence(&result, "catalog_stats_visibility_epoch_persist=planned");
  if (result.agent_schedule_planned) {
    AddEvidence(&result, "agent_statistics_lifecycle_registered=true");
    AddEvidence(&result, "agent_statistics_refresh_scheduled=true");
  }

  result.histogram_plans = BuildRebuildPlans(request.column_uuids,
                                             request.relation_uuid,
                                             "histogram",
                                             request.histogram_bucket_target);
  result.mcv_plans = BuildRebuildPlans(request.column_uuids,
                                       request.relation_uuid,
                                       "mcv",
                                       request.mcv_entry_target);
  if (!result.histogram_plans.empty()) {
    result.histogram_rebuild = true;
    AddEvidence(&result, "histogram_plan_count=" +
                             std::to_string(result.histogram_plans.size()));
  }
  if (!result.mcv_plans.empty()) {
    result.mcv_rebuild = true;
    AddEvidence(&result, "mcv_plan_count=" +
                             std::to_string(result.mcv_plans.size()));
  }
  MaybeBuildPlannedTableStats(request, &result);
  return result;
}

std::string SerializeOptimizerStatisticsLifecycleEvidence(
    const OptimizerStatisticsLifecycleResult& result) {
  std::ostringstream out;
  out << "diagnostic_code=" << result.diagnostic_code
      << "|accepted=" << (result.accepted ? "true" : "false")
      << "|refresh_needed=" << (result.refresh_needed ? "true" : "false")
      << "|next_stats_epoch=" << result.next_stats_epoch
      << "|next_catalog_epoch=" << result.next_catalog_epoch
      << "|next_stats_visibility_epoch=" << result.next_stats_visibility_epoch
      << "|catalog_update_planned="
      << (result.catalog_update_planned ? "true" : "false")
      << "|agent_schedule_planned="
      << (result.agent_schedule_planned ? "true" : "false")
      << "|row_visibility_semantics_changed="
      << (result.row_visibility_semantics_changed ? "true" : "false")
      << "|transaction_finality_semantics_changed="
      << (result.transaction_finality_semantics_changed ? "true" : "false");
  for (const auto& evidence : result.evidence) {
    out << '|' << evidence;
  }
  return out.str();
}

}  // namespace scratchbird::engine::optimizer
