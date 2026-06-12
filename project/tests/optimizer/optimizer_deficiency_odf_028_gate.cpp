// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_statistics_lifecycle.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool Has(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool ContainsNoRuntimeDocDependencyTokens(const std::string& evidence) {
  const std::vector<std::string> forbidden = {
      "docs/", "execution-plans", "findings", "audit", "contracts", "references"};
  for (const auto& token : forbidden) {
    if (!Require(evidence.find(token) == std::string::npos,
                 "serialized lifecycle evidence leaked forbidden token: " +
                     token)) {
      return false;
    }
  }
  return true;
}

opt::OptimizerStatisticsLifecycleRequest BaseRequest(
    opt::OptimizerStatisticsLifecycleTrigger trigger) {
  opt::OptimizerStatisticsLifecycleRequest request;
  request.trigger = trigger;
  request.relation_uuid = "rel.customer";
  request.column_uuids = {"col.customer.id", "col.customer.region"};
  request.current_stats_epoch = 10;
  request.request_stats_epoch = 10;
  request.catalog_epoch = 20;
  request.security_epoch = 30;
  request.policy_epoch = 40;
  request.stats_visibility_epoch = 50;
  request.current_freshness = opt::OptimizerStatsFreshnessState::kFresh;
  request.sampled_rows = 512;
  request.total_rows_estimate = 4096;
  request.page_count = 64;
  request.average_row_bytes = 128;
  request.rows_modified_since_stats = 256;
  request.bulk_rows_written = 2048;
  request.stale_row_threshold = 128;
  request.histogram_bucket_target = 8;
  request.mcv_entry_target = 4;
  return request;
}

bool AdmittedWithCommonEvidence(
    const opt::OptimizerStatisticsLifecycleResult& result,
    const std::string& diagnostic_code) {
  const auto evidence = opt::SerializeOptimizerStatisticsLifecycleEvidence(result);
  return Require(result.accepted, diagnostic_code + " was refused") &&
         Require(result.decision ==
                     opt::OptimizerStatisticsLifecycleDecision::kAdmitted,
                 diagnostic_code + " decision was not admitted") &&
         Require(result.refresh_needed, diagnostic_code + " did not refresh") &&
         Require(result.diagnostic_code == diagnostic_code,
                 "diagnostic mismatch: " + result.diagnostic_code) &&
         Require(result.next_stats_epoch == 11,
                 diagnostic_code + " did not advance stats epoch") &&
         Require(result.next_catalog_epoch == 20,
                 diagnostic_code + " catalog epoch drifted") &&
         Require(result.next_stats_visibility_epoch == 51,
                 diagnostic_code + " did not advance metadata visibility epoch") &&
         Require(!result.row_visibility_semantics_changed,
                 diagnostic_code + " changed row visibility semantics") &&
         Require(!result.transaction_finality_semantics_changed,
                 diagnostic_code + " changed finality semantics") &&
         Require(Has(result.evidence, "lifecycle_metadata_only=true"),
                 diagnostic_code + " missing metadata-only evidence") &&
         Require(Has(result.evidence,
                     "mga_visibility_authority=engine_recheck_required"),
                 diagnostic_code + " missing MGA recheck evidence") &&
         Require(Has(result.evidence,
                     "mga_finality_authority=engine_transaction_inventory"),
                 diagnostic_code + " missing MGA finality evidence") &&
         Require(Has(result.evidence, "security_recheck=required"),
                 diagnostic_code + " missing security recheck evidence") &&
         Require(Has(result.evidence, "parser_or_reference_authority=false"),
                 diagnostic_code + " missing parser/reference refusal evidence") &&
         Require(Has(result.evidence,
                     "stats_visibility_epoch_advanced=metadata_only"),
                 diagnostic_code + " missing visibility epoch evidence") &&
         Require(result.catalog_update_planned,
                 diagnostic_code + " did not plan catalog update") &&
         Require(Has(result.evidence,
                     "catalog_stats_descriptor_update=planned"),
                 diagnostic_code + " missing catalog descriptor evidence") &&
         Require(Has(result.evidence, "catalog_stats_epoch_persist=planned"),
                 diagnostic_code + " missing catalog epoch persist evidence") &&
         Require(Has(result.evidence,
                     "catalog_stats_visibility_epoch_persist=planned"),
                 diagnostic_code + " missing visibility persist evidence") &&
         ContainsNoRuntimeDocDependencyTokens(evidence);
}

bool TriggerAdmissionsCoverLifecycle() {
  {
    const auto result = opt::EvaluateOptimizerStatisticsLifecycle(
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kManualAnalyze));
    if (!AdmittedWithCommonEvidence(
            result, "SB_OPT_STATS_LIFECYCLE.MANUAL_ANALYZE_ADMITTED") ||
        !Require(Has(result.evidence, "manual_analyze_admitted=true"),
                 "manual analyze evidence missing") ||
        !Require(result.has_planned_table_stats,
                 "manual analyze did not plan table stats") ||
        !Require(result.planned_table_stats.identity.source ==
                     opt::StatisticSource::kCatalogSample,
                 "manual analyze planned source mismatch")) {
      return false;
    }
  }
  {
    const auto result = opt::EvaluateOptimizerStatisticsLifecycle(
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kSampledRefresh));
    if (!AdmittedWithCommonEvidence(
            result, "SB_OPT_STATS_LIFECYCLE.SAMPLED_REFRESH_ADMITTED") ||
        !Require(Has(result.evidence, "sampled_refresh_admitted=true"),
                 "sampled refresh evidence missing") ||
        !Require(result.has_planned_table_stats,
                 "sampled refresh did not plan table stats")) {
      return false;
    }
  }
  {
    auto request =
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kStaleDetection);
    request.current_freshness = opt::OptimizerStatsFreshnessState::kStale;
    const auto result = opt::EvaluateOptimizerStatisticsLifecycle(request);
    if (!AdmittedWithCommonEvidence(
            result, "SB_OPT_STATS_LIFECYCLE.STALE_REFRESH_ADMITTED") ||
        !Require(Has(result.evidence, "stale_stat_detected=true"),
                 "stale detection evidence missing")) {
      return false;
    }
  }
  {
    const auto result = opt::EvaluateOptimizerStatisticsLifecycle(
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kPostBulkRefresh));
    if (!AdmittedWithCommonEvidence(
            result, "SB_OPT_STATS_LIFECYCLE.POST_BULK_REFRESH_ADMITTED") ||
        !Require(Has(result.evidence, "post_bulk_refresh_admitted=true"),
                 "post-bulk evidence missing")) {
      return false;
    }
  }
  {
    const auto result = opt::EvaluateOptimizerStatisticsLifecycle(
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kHistogramRebuild));
    if (!AdmittedWithCommonEvidence(
            result, "SB_OPT_STATS_LIFECYCLE.HISTOGRAM_REBUILD_ADMITTED") ||
        !Require(result.histogram_rebuild, "histogram rebuild flag missing") ||
        !Require(result.histogram_plans.size() == 2,
                 "histogram rebuild plan count mismatch") ||
        !Require(result.histogram_plans.front().target_entry_count == 8,
                 "histogram target count mismatch")) {
      return false;
    }
  }
  {
    const auto result = opt::EvaluateOptimizerStatisticsLifecycle(
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kMcvRebuild));
    if (!AdmittedWithCommonEvidence(
            result, "SB_OPT_STATS_LIFECYCLE.MCV_REBUILD_ADMITTED") ||
        !Require(result.mcv_rebuild, "MCV rebuild flag missing") ||
        !Require(result.mcv_plans.size() == 2, "MCV rebuild plan count mismatch") ||
        !Require(result.mcv_plans.front().target_entry_count == 4,
                 "MCV target count mismatch")) {
      return false;
    }
  }
  {
    const auto result = opt::EvaluateOptimizerStatisticsLifecycle(
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kAdvisorSafeRefresh));
    if (!AdmittedWithCommonEvidence(
            result, "SB_OPT_STATS_LIFECYCLE.ADVISOR_SAFE_REFRESH_ADMITTED") ||
        !Require(result.advisor_metadata_only,
                 "advisor refresh was not metadata-only") ||
        !Require(Has(result.evidence, "advisor_safe_refresh=true"),
                 "advisor-safe evidence missing")) {
      return false;
    }
  }
  {
    const auto result = opt::EvaluateOptimizerStatisticsLifecycle(
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance));
    if (!AdmittedWithCommonEvidence(
            result, "SB_OPT_STATS_LIFECYCLE.AGENT_AUTO_MAINTENANCE_ADMITTED") ||
        !Require(result.agent_action_safe,
                 "agent auto-maintenance was not marked safe") ||
        !Require(result.agent_schedule_planned,
                 "agent auto-maintenance did not plan agent schedule") ||
        !Require(Has(result.evidence,
                     "agent_auto_maintenance_policy_safe=true"),
                 "agent auto-maintenance evidence missing") ||
        !Require(Has(result.evidence,
                     "agent_statistics_lifecycle_registered=true"),
                 "agent lifecycle registration evidence missing") ||
        !Require(Has(result.evidence,
                     "agent_statistics_refresh_scheduled=true"),
                 "agent refresh schedule evidence missing") ||
        !Require(result.histogram_rebuild && result.mcv_rebuild,
                 "agent auto-maintenance did not plan histogram and MCV rebuilds")) {
      return false;
    }
  }
  return true;
}

bool RefusesWith(opt::OptimizerStatisticsLifecycleRequest request,
                 const std::string& expected_code,
                 const std::string& expected_evidence) {
  const auto result = opt::EvaluateOptimizerStatisticsLifecycle(request);
  return Require(!result.accepted, expected_code + " was accepted") &&
         Require(result.diagnostic_code == expected_code,
                 "refusal mismatch: expected " + expected_code + " got " +
                     result.diagnostic_code) &&
         Require(Has(result.evidence, expected_evidence),
                 expected_code + " missing refusal evidence") &&
         ContainsNoRuntimeDocDependencyTokens(
             opt::SerializeOptimizerStatisticsLifecycleEvidence(result));
}

bool ExactRefusalsCoverPolicySecurityMgaAndEpochs() {
  {
    auto request =
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance);
    request.policy_enabled = false;
    if (!RefusesWith(request, "SB_OPT_STATS_LIFECYCLE.POLICY_DISABLED",
                     "policy_disabled")) {
      return false;
    }
  }
  {
    auto request =
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance);
    request.security_context_present = false;
    if (!RefusesWith(request,
                     "SB_OPT_STATS_LIFECYCLE.MISSING_SECURITY_CONTEXT",
                     "missing_security_context")) {
      return false;
    }
  }
  {
    auto request =
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance);
    request.grants_proven = false;
    if (!RefusesWith(request, "SB_OPT_STATS_LIFECYCLE.MISSING_GRANTS",
                     "missing_grants")) {
      return false;
    }
  }
  {
    auto request =
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance);
    request.mga_visibility_recheck_present = false;
    if (!RefusesWith(request, "SB_OPT_STATS_LIFECYCLE.MISSING_MGA_RECHECK",
                     "missing_mga_recheck")) {
      return false;
    }
  }
  {
    auto request =
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance);
    request.security_recheck_present = false;
    if (!RefusesWith(request,
                     "SB_OPT_STATS_LIFECYCLE.MISSING_SECURITY_RECHECK",
                     "missing_security_recheck")) {
      return false;
    }
  }
  {
    auto request =
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance);
    request.parser_or_reference_authority = true;
    if (!RefusesWith(request,
                     "SB_OPT_STATS_LIFECYCLE.UNSAFE_PARSER_REFERENCE_AUTHORITY",
                     "unsafe_parser_or_reference_authority")) {
      return false;
    }
  }
  {
    auto request =
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance);
    request.epoch_evidence_present = false;
    if (!RefusesWith(request,
                     "SB_OPT_STATS_LIFECYCLE.MISSING_EPOCH_EVIDENCE",
                     "missing_epoch_evidence")) {
      return false;
    }
  }
  {
    auto request =
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance);
    request.current_stats_epoch = 12;
    request.request_stats_epoch = 10;
    if (!RefusesWith(request, "SB_OPT_STATS_LIFECYCLE.STALE_EPOCH",
                     "stale_epoch")) {
      return false;
    }
  }
  {
    auto request =
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance);
    request.stats_compatible = false;
    if (!RefusesWith(request,
                     "SB_OPT_STATS_LIFECYCLE.STATS_INCOMPATIBLE",
                     "stats_incompatible")) {
      return false;
    }
  }
  {
    auto request =
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance);
    request.catalog_descriptor_present = false;
    if (!RefusesWith(request,
                     "SB_OPT_STATS_LIFECYCLE.MISSING_CATALOG_DESCRIPTOR",
                     "missing_catalog_descriptor")) {
      return false;
    }
  }
  {
    auto request =
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance);
    request.catalog_write_admitted = false;
    if (!RefusesWith(request,
                     "SB_OPT_STATS_LIFECYCLE.CATALOG_WRITE_NOT_ADMITTED",
                     "catalog_write_not_admitted")) {
      return false;
    }
  }
  {
    auto request =
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance);
    request.agent_runtime_registered = false;
    if (!RefusesWith(request,
                     "SB_OPT_STATS_LIFECYCLE.AGENT_RUNTIME_NOT_REGISTERED",
                     "agent_runtime_not_registered")) {
      return false;
    }
  }
  {
    auto request =
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance);
    request.agent_schedule_admitted = false;
    if (!RefusesWith(request,
                     "SB_OPT_STATS_LIFECYCLE.AGENT_SCHEDULE_NOT_ADMITTED",
                     "agent_schedule_not_admitted")) {
      return false;
    }
  }
  {
    auto request =
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kAdvisorSafeRefresh);
    request.current_freshness = opt::OptimizerStatsFreshnessState::kStale;
    request.require_fresh_current_stats = true;
    if (!RefusesWith(request, "SB_OPT_STATS_LIFECYCLE.STATS_STALE",
                     "stats_stale")) {
      return false;
    }
  }
  {
    auto request =
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kAdvisorSafeRefresh);
    request.current_freshness = opt::OptimizerStatsFreshnessState::kFresh;
    request.rows_modified_since_stats = 0;
    if (!RefusesWith(request, "SB_OPT_STATS_LIFECYCLE.NO_REFRESH_NEEDED",
                     "no_refresh_need")) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  if (!TriggerAdmissionsCoverLifecycle()) return 1;
  if (!ExactRefusalsCoverPolicySecurityMgaAndEpochs()) return 1;
  return 0;
}
