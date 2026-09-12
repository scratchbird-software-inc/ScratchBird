// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_statistics_lifecycle.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

void AddEvidence(OptimizerStatisticsLifecycleResult* result,
                 std::string evidence) {
  if (result == nullptr) return;
  result->evidence.push_back(std::move(evidence));
}

OptimizerStatisticsLifecycleResult Refuse(const OptimizerStatisticsLifecycleRequest& request,
                                          std::string_view code,
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
  result.failure = result.decision == OptimizerStatisticsLifecycleDecision::kNoRefreshNeeded
      ? OptimizerStatisticsLifecycleFailure::kNone
      : OptimizerStatisticsLifecycleFailure::kInvalidRequest;
  result.diagnostic_code = result.failure == OptimizerStatisticsLifecycleFailure::kNone
      ? "" : "SB-STAT-0001";
  result.reason = code;
  result.trigger = request.trigger;
  result.relation_uuid = request.relation_uuid;
  result.column_uuids = request.column_uuids;
  result.security_epoch = request.security_epoch;
  result.policy_epoch = request.policy_epoch;
  if (result.decision == OptimizerStatisticsLifecycleDecision::kNoRefreshNeeded)
    result.observed_table_stats = request.observed_table_stats;
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

void AddCommonAuthorityEvidence(OptimizerStatisticsLifecycleResult* result) {
  AddEvidence(result, "lifecycle_metadata_only=true");
  AddEvidence(result, "advisor_refresh_metadata_only=true");
  AddEvidence(result, "mga_visibility_authority=engine_recheck_required");
  AddEvidence(result, "mga_finality_authority=engine_transaction_inventory");
  AddEvidence(result, "security_recheck=required");
  AddEvidence(result, "parser_or_reference_authority=false");
  AddEvidence(result, "result_semantics_changed=false");
}

std::vector<OptimizerStatisticsLifecycleRebuildPlan> BuildRebuildPlans(
    const std::vector<planner::CanonicalPlannerUuid>& column_uuids,
    std::uint64_t target_count) {
  std::vector<OptimizerStatisticsLifecycleRebuildPlan> plans;
  if (target_count == 0) return plans;
  plans.reserve(column_uuids.size());
  for (const auto& column_uuid : column_uuids) {
    plans.push_back({column_uuid, target_count});
  }
  return plans;
}

bool NeedsRefresh(const OptimizerStatisticsLifecycleRequest& request) {
  switch (request.trigger) {
    case OptimizerStatisticsLifecycleTrigger::kManualAnalyze:
      return true;
    case OptimizerStatisticsLifecycleTrigger::kSampledRefresh:
      return true;  // A request to collect a sample is not an existing sample.
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

std::string_view AdmissionDiagnostic(OptimizerStatisticsLifecycleTrigger trigger) {
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
      AddEvidence(result, "manual_analyze_plan=true");
      break;
    case OptimizerStatisticsLifecycleTrigger::kSampledRefresh:
      AddEvidence(result, "sampled_refresh_plan=true");
      break;
    case OptimizerStatisticsLifecycleTrigger::kStaleDetection:
      AddEvidence(result, "stale_stat_detected=true");
      break;
    case OptimizerStatisticsLifecycleTrigger::kPostBulkRefresh:
      AddEvidence(result, "post_bulk_refresh_plan=true");
      break;
    case OptimizerStatisticsLifecycleTrigger::kHistogramRebuild:
      AddEvidence(result, "histogram_rebuild=true");
      break;
    case OptimizerStatisticsLifecycleTrigger::kMcvRebuild:
      AddEvidence(result, "mcv_rebuild=true");
      break;
    case OptimizerStatisticsLifecycleTrigger::kAdvisorSafeRefresh:
      AddEvidence(result, "advisor_refresh_plan=true");
      break;
    case OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance:
      AddEvidence(result, "agent_refresh_plan=true");
      break;
  }
}

bool ObservationMatches(const OptimizerStatisticsLifecycleRequest& request) {
  if (!request.observed_table_stats) return true;
  const auto& table = *request.observed_table_stats;
  const auto& id = table.identity;
  return scratchbird::core::uuid::IsEngineIdentityUuid(id.statistic_uuid) &&
      id.object_uuid == request.relation_uuid &&
      id.stats_epoch != 0 && id.stats_epoch == request.current_stats_epoch &&
      id.catalog_epoch != 0 && id.catalog_epoch <= request.catalog_epoch &&
      id.transaction_visibility_epoch != 0 &&
      id.transaction_visibility_epoch <= request.stats_visibility_epoch &&
      id.freshness == request.current_freshness &&
      id.freshness != OptimizerStatsFreshnessState::kMissing &&
      (id.source == StatisticSource::kCatalogExact ||
       id.source == StatisticSource::kCatalogSample) &&
      id.confidence >= CostConfidence::kExact &&
      id.confidence <= CostConfidence::kLow &&
      table.visible_row_count <= table.row_count &&
      (table.row_count == 0 ||
       (table.page_count != 0 && table.average_row_bytes != 0));
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
  return "unknown";
}

static OptimizerStatisticsLifecycleResult EvaluateImpl(
    const OptimizerStatisticsLifecycleRequest& request) {
  if (request.trigger < OptimizerStatisticsLifecycleTrigger::kManualAnalyze ||
      request.trigger > OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance ||
      request.current_freshness < OptimizerStatsFreshnessState::kFresh ||
      request.current_freshness > OptimizerStatsFreshnessState::kMissing) {
    return Refuse(request, "SB_OPT_STATS_LIFECYCLE.INVALID_ENUM", "invalid_enum");
  }
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(request.relation_uuid)) {
    return Refuse(request,
                  "SB_OPT_STATS_LIFECYCLE.OBJECT_REQUIRED",
                  "object_required");
  }
  std::set<planner::CanonicalPlannerUuid> columns;
  for (const auto& column : request.column_uuids) {
    if (!scratchbird::core::uuid::IsEngineIdentityUuid(column) ||
        !columns.insert(column).second) {
      return Refuse(request, "SB_OPT_STATS_LIFECYCLE.INVALID_COLUMN", "invalid_column");
    }
  }
  if (((request.histogram_bucket_target != 0 || request.mcv_entry_target != 0) &&
       columns.empty()) ||
      (request.trigger == OptimizerStatisticsLifecycleTrigger::kHistogramRebuild &&
       request.histogram_bucket_target == 0) ||
      (request.trigger == OptimizerStatisticsLifecycleTrigger::kMcvRebuild &&
       request.mcv_entry_target == 0)) {
    return Refuse(request, "SB_OPT_STATS_LIFECYCLE.INVALID_REBUILD_TARGET",
                  "invalid_rebuild_target");
  }
  if (!ObservationMatches(request)) {
    return Refuse(request, "SB_OPT_STATS_LIFECYCLE.OBSERVATION_MISMATCH",
                  "observation_mismatch");
  }
  if (!request.policy_enabled ||
      (TriggerRequiresAgentSchedule(request.trigger) && !request.agent_policy_safe)) {
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
  if (!request.advisory_only || request.parser_or_reference_authority) {
    return Refuse(request,
                  "SB_OPT_STATS_LIFECYCLE.UNSAFE_PARSER_REFERENCE_AUTHORITY",
                  "unsafe_parser_or_reference_authority");
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
  if (!NeedsRefresh(request)) {
    return Refuse(request,
                  "SB_OPT_STATS_LIFECYCLE.NO_REFRESH_NEEDED",
                  "no_refresh_need");
  }

  const auto generation = std::max(request.current_stats_epoch, request.request_stats_epoch);
  if (generation == std::numeric_limits<std::uint64_t>::max()) {
    return Refuse(request, "SB_OPT_STATS_LIFECYCLE.EPOCH_EXHAUSTED", "epoch_exhausted");
  }

  OptimizerStatisticsLifecycleResult result;
  result.trigger = request.trigger;
  result.relation_uuid = request.relation_uuid;
  result.column_uuids = request.column_uuids;
  result.security_epoch = request.security_epoch;
  result.policy_epoch = request.policy_epoch;
  result.decision = OptimizerStatisticsLifecycleDecision::kAdmitted;
  result.accepted = true;
  result.refresh_needed = true;
  result.advisor_metadata_only = true;
  result.catalog_update_planned = true;
  result.agent_schedule_planned = TriggerRequiresAgentSchedule(request.trigger);
  result.histogram_rebuild =
      request.trigger == OptimizerStatisticsLifecycleTrigger::kHistogramRebuild;
  result.mcv_rebuild =
      request.trigger == OptimizerStatisticsLifecycleTrigger::kMcvRebuild;
  result.next_stats_epoch =
      generation + 1;
  result.next_catalog_epoch = request.catalog_epoch;
  result.next_stats_visibility_epoch = request.stats_visibility_epoch;
  result.reason = AdmissionDiagnostic(request.trigger);

  AddTriggerEvidence(request, &result);
  AddCommonAuthorityEvidence(&result);
  AddEvidence(&result, "stats_epoch_advance=planned");
  AddEvidence(&result, "stats_visibility_epoch=captured_unchanged");
  AddEvidence(&result, "catalog_stats_descriptor_update=planned");
  AddEvidence(&result, "catalog_stats_epoch_persist=planned");
  AddEvidence(&result, "catalog_stats_visibility_epoch_persist=planned");
  if (result.agent_schedule_planned) {
    AddEvidence(&result, "agent_statistics_lifecycle_registration=requires_runtime_recheck");
    AddEvidence(&result, "agent_statistics_refresh_schedule=planned");
  }

  result.histogram_plans = BuildRebuildPlans(request.column_uuids,
                                             request.histogram_bucket_target);
  result.mcv_plans = BuildRebuildPlans(request.column_uuids,
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
  result.observed_table_stats = request.observed_table_stats;
  return result;
}

OptimizerStatisticsLifecycleResult EvaluateOptimizerStatisticsLifecycle(
    const OptimizerStatisticsLifecycleRequest& request) noexcept {
  try {
    return EvaluateImpl(request);
  } catch (const std::bad_alloc&) {
    OptimizerStatisticsLifecycleResult result;
    result.failure = OptimizerStatisticsLifecycleFailure::kAllocationFailure;
    result.diagnostic_code = "SB-STAT-0001";
    result.reason = "allocation_failure";
    return result;
  } catch (...) {
    OptimizerStatisticsLifecycleResult result;
    result.failure = OptimizerStatisticsLifecycleFailure::kInternalFailure;
    result.diagnostic_code = "SB-STAT-0001";
    result.reason = "internal_failure";
    return result;
  }
}

std::string SerializeOptimizerStatisticsLifecycleEvidence(
    const OptimizerStatisticsLifecycleResult& result) {
  // Binary metadata binding, not a transport command or proof of execution.
  // No stream can silently truncate a successful result on allocation failure.
  std::string out;
  const auto number = [&](std::uint64_t value) {
    for (unsigned i = 0; i != 8; ++i) out.push_back(static_cast<char>(value >> (8 * i)));
  };
  const auto text = [&](std::string_view value) {
    number(value.size());
    out.append(value);
  };
  const auto uuid = [&](const planner::CanonicalPlannerUuid& value) {
    out.append(reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size());
  };
  text("optimizer-statistics-lifecycle-v1");
  number(static_cast<std::uint64_t>(result.decision));
  number(static_cast<std::uint64_t>(result.failure));
  number(static_cast<std::uint64_t>(result.trigger));
  uuid(result.relation_uuid);
  number(result.column_uuids.size());
  for (const auto& column : result.column_uuids) uuid(column);
  number(result.security_epoch);
  number(result.policy_epoch);
  for (bool flag : {result.accepted, result.refresh_needed, result.histogram_rebuild,
       result.mcv_rebuild, result.advisor_metadata_only, result.catalog_update_planned,
       result.agent_schedule_planned, result.row_visibility_semantics_changed,
       result.transaction_finality_semantics_changed}) number(flag);
  number(result.next_stats_epoch);
  number(result.next_catalog_epoch);
  number(result.next_stats_visibility_epoch);
  text(result.diagnostic_code);
  text(result.reason);
  number(result.observed_table_stats.has_value());
  if (result.observed_table_stats) {
    const auto& table = *result.observed_table_stats;
    const auto& id = table.identity;
    uuid(id.object_uuid);
    uuid(id.statistic_uuid);
    number(id.stats_epoch);
    number(id.catalog_epoch);
    number(id.transaction_visibility_epoch);
    number(static_cast<std::uint64_t>(id.freshness));
    number(static_cast<std::uint64_t>(id.source));
    number(static_cast<std::uint64_t>(id.confidence));
    number(table.row_count);
    number(table.visible_row_count);
    number(table.page_count);
    number(table.average_row_bytes);
  }
  for (const auto* plans : {&result.histogram_plans, &result.mcv_plans}) {
    number(plans->size());
    for (const auto& plan : *plans) {
      uuid(plan.column_uuid);
      number(plan.target_entry_count);
    }
  }
  number(result.evidence.size());
  for (const auto& evidence : result.evidence) text(evidence);
  return out;
}

}  // namespace scratchbird::engine::optimizer
