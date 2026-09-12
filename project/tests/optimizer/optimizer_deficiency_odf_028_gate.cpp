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
#include <limits>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace lifecycle_fault {
thread_local long remaining = -1;
thread_local bool hit = false;
}
void* operator new(std::size_t size) {
  if (lifecycle_fault::remaining >= 0 && lifecycle_fault::remaining-- == 0) {
    lifecycle_fault::remaining = 0;
    lifecycle_fault::hit = true;
    throw std::bad_alloc();
  }
  if (void* value = std::malloc(size ? size : 1)) return value;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace opt = scratchbird::engine::optimizer;

namespace {
using Id = scratchbird::engine::planner::CanonicalPlannerUuid;
unsigned checks = 0, allocation_faults = 0;
Id Identity(unsigned n) {
  Id id{};
  id.bytes[0] = 1; id.bytes[6] = 0x70; id.bytes[8] = 0x80;
  for (unsigned i = 0; i != 4; ++i) { id.bytes[15-i] = n & 255; n >>= 8; }
  return id;
}

bool Require(bool condition, const std::string& message) {
  ++checks;
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
  request.relation_uuid = Identity(1);
  request.column_uuids = {Identity(2), Identity(3)};
  request.current_stats_epoch = 10;
  request.request_stats_epoch = 10;
  request.catalog_epoch = 20;
  request.security_epoch = 30;
  request.policy_epoch = 40;
  request.stats_visibility_epoch = 50;
  request.current_freshness = opt::OptimizerStatsFreshnessState::kFresh;
  opt::TableCardinalityStats observed;
  observed.identity = {request.relation_uuid, Identity(4), 10, 20, 50,
      opt::OptimizerStatsFreshnessState::kFresh,
      opt::StatisticSource::kCatalogSample, opt::CostConfidence::kHigh};
  observed.row_count = 4096;
  observed.visible_row_count = 4000;
  observed.page_count = 64;
  observed.average_row_bytes = 128;
  request.observed_table_stats = observed;
  request.policy_enabled = request.security_context_present = request.grants_proven =
      request.mga_visibility_recheck_present = request.security_recheck_present =
      request.epoch_evidence_present = request.agent_policy_safe =
      request.catalog_descriptor_present = request.catalog_write_admitted =
      request.agent_runtime_registered = request.agent_schedule_admitted = true;
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
         Require(result.diagnostic_code.empty() && result.reason == diagnostic_code,
                 "diagnostic mismatch: " + std::string(result.diagnostic_code) + " reason=" + std::string(result.reason)) &&
         Require(result.next_stats_epoch == 11,
                 diagnostic_code + " did not advance stats epoch") &&
         Require(result.next_catalog_epoch == 20,
                 diagnostic_code + " catalog epoch drifted") &&
         Require(result.next_stats_visibility_epoch == 50,
                 diagnostic_code + " changed the captured visibility epoch") &&
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
                     "stats_visibility_epoch=captured_unchanged"),
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
        !Require(Has(result.evidence, "manual_analyze_plan=true"),
                 "manual analyze evidence missing") ||
        !Require(result.observed_table_stats.has_value(),
                 "manual analyze did not retain the supplied table observation") ||
        !Require(result.observed_table_stats->identity.source ==
                     opt::StatisticSource::kCatalogSample,
                 "manual analyze rewrote the observed source")) {
      return false;
    }
  }
  {
    const auto result = opt::EvaluateOptimizerStatisticsLifecycle(
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kSampledRefresh));
    if (!AdmittedWithCommonEvidence(
            result, "SB_OPT_STATS_LIFECYCLE.SAMPLED_REFRESH_ADMITTED") ||
        !Require(Has(result.evidence, "sampled_refresh_plan=true"),
                 "sampled refresh evidence missing") ||
        !Require(result.observed_table_stats.has_value(),
                 "sampled refresh did not retain the supplied table observation")) {
      return false;
    }
  }
  {
    auto request =
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kStaleDetection);
    request.observed_table_stats.reset();
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
        !Require(Has(result.evidence, "post_bulk_refresh_plan=true"),
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
        !Require(Has(result.evidence, "advisor_refresh_plan=true"),
                 "advisor-safe evidence missing")) {
      return false;
    }
  }
  {
    const auto result = opt::EvaluateOptimizerStatisticsLifecycle(
        BaseRequest(opt::OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance));
    if (!AdmittedWithCommonEvidence(
            result, "SB_OPT_STATS_LIFECYCLE.AGENT_AUTO_MAINTENANCE_ADMITTED") ||
        !Require(result.advisor_metadata_only,
                 "agent auto-maintenance plan was not metadata-only") ||
        !Require(result.agent_schedule_planned,
                 "agent auto-maintenance did not plan agent schedule") ||
        !Require(Has(result.evidence,
                     "agent_refresh_plan=true"),
                 "agent auto-maintenance evidence missing") ||
        !Require(Has(result.evidence,
                     "agent_statistics_lifecycle_registration=requires_runtime_recheck"),
                 "agent lifecycle registration evidence missing") ||
        !Require(Has(result.evidence,
                     "agent_statistics_refresh_schedule=planned"),
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
         Require(result.reason == expected_code && result.diagnostic_code ==
                     (expected_code == "SB_OPT_STATS_LIFECYCLE.NO_REFRESH_NEEDED" ? "" : "SB-STAT-0001"),
                 "refusal mismatch: expected " + expected_code + " got " +
                     std::string(result.diagnostic_code)) &&
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
    request.observed_table_stats.reset();
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
    request.observed_table_stats->identity.freshness = opt::OptimizerStatsFreshnessState::kStale;
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

bool BinaryPlanningAndFailureAtomicity() {
  using Trigger = opt::OptimizerStatisticsLifecycleTrigger;
  using Failure = opt::OptimizerStatisticsLifecycleFailure;
  static_assert(sizeof(Id) == 16);
  static_assert(std::is_same_v<decltype(opt::OptimizerStatisticsLifecycleRequest{}.relation_uuid), Id>);
  const auto base = BaseRequest(Trigger::kAgentAutoMaintenance);
  const auto admitted = opt::EvaluateOptimizerStatisticsLifecycle(base);
  if (!Require(admitted.accepted, "base plan rejected")) return false;
  if (!Require(admitted.observed_table_stats->identity.stats_epoch == 10 &&
               admitted.observed_table_stats->identity.transaction_visibility_epoch == 50 &&
               admitted.observed_table_stats->row_count == 4096 &&
               admitted.histogram_plans[0].column_uuid == base.column_uuids[0] &&
               admitted.histogram_plans[1].column_uuid == base.column_uuids[1],
               "observation or ordered binary targets were rewritten")) return false;
  const auto binding = opt::SerializeOptimizerStatisticsLifecycleEvidence(admitted);
  // Independently decode the complete wire order, including raw UUID bytes.
  struct Reader {
    const std::string& bytes;
    std::size_t offset = 0;
    std::uint64_t Number() {
      if (bytes.size() - offset < 8) throw std::runtime_error("short number");
      std::uint64_t value = 0;
      for (unsigned i = 0; i != 8; ++i)
        value |= std::uint64_t(static_cast<unsigned char>(bytes[offset++])) << (i*8);
      return value;
    }
    std::string Text() {
      const auto count = Number();
      if (count > bytes.size() - offset) throw std::runtime_error("short text");
      const auto value = bytes.substr(offset, count); offset += count; return value;
    }
    bool Uuid(const Id& id) {
      if (bytes.size() - offset < 16) return false;
      for (auto b : id.bytes)
        if (static_cast<unsigned char>(bytes[offset++]) != b) return false;
      return true;
    }
  } reader{binding};
  if (!Require(reader.Text() == "optimizer-statistics-lifecycle-v1" &&
       reader.Number() == 0 && reader.Number() == 0 && reader.Number() == 7 &&
       reader.Uuid(base.relation_uuid) && reader.Number() == 2 &&
       reader.Uuid(base.column_uuids[0]) && reader.Uuid(base.column_uuids[1]) &&
       reader.Number() == 30 && reader.Number() == 40,
       "binary lifecycle header/order mismatch")) return false;
  for (auto expected : {1, 1, 1, 1, 1, 1, 1, 0, 0})
    if (!Require(reader.Number() == unsigned(expected), "binary plan flag mismatch")) return false;
  if (!Require(reader.Number() == 11 && reader.Number() == 20 && reader.Number() == 50 &&
       reader.Text().empty() && reader.Text() == "SB_OPT_STATS_LIFECYCLE.AGENT_AUTO_MAINTENANCE_ADMITTED" &&
       reader.Number() == 1 && reader.Uuid(base.relation_uuid) && reader.Uuid(Identity(4)),
       "generation/observation identity binding mismatch")) return false;
  for (auto expected : {std::uint64_t(10), std::uint64_t(20), std::uint64_t(50),
       std::uint64_t(opt::OptimizerStatsFreshnessState::kFresh),
       std::uint64_t(opt::StatisticSource::kCatalogSample),
       std::uint64_t(opt::CostConfidence::kHigh),
       std::uint64_t(4096), std::uint64_t(4000), std::uint64_t(64), std::uint64_t(128)})
    if (!Require(reader.Number() == expected, "observation fields not exactly retained")) return false;
  for (auto target : {8, 4}) {
    if (!Require(reader.Number() == 2, "rebuild plan count mismatch")) return false;
    for (const auto& column : base.column_uuids)
      if (!Require(reader.Uuid(column) && reader.Number() == unsigned(target),
                   "rebuild target binding mismatch")) return false;
  }
  if (!Require(reader.Number() == admitted.evidence.size(), "evidence count missing")) return false;
  for (const auto& item : admitted.evidence)
    if (!Require(reader.Text() == item, "evidence frame mismatch")) return false;
  if (!Require(reader.offset == binding.size(), "trailing unbound fields")) return false;
  for (const auto& text : admitted.evidence) {
    if (!Require(text.find("scheduled=true") == std::string::npos &&
                 text.find("registered=true") == std::string::npos &&
                 text.find("epoch_advanced=true") == std::string::npos,
                 "unexecuted side effect reported")) return false;
  }

  const auto refuses = [](const auto& request) {
    const auto r = opt::EvaluateOptimizerStatisticsLifecycle(request);
    return Require(!r.accepted && r.failure == Failure::kInvalidRequest &&
        r.diagnostic_code == "SB-STAT-0001" && !r.catalog_update_planned &&
        !r.agent_schedule_planned && !r.observed_table_stats &&
        r.histogram_plans.empty() && r.mcv_plans.empty(),
        "invalid request retained a success payload");
  };
  if (!refuses(opt::OptimizerStatisticsLifecycleRequest{})) return false;
  for (int ordinal : {-1, 8, 255}) {
    auto request = base; request.trigger = static_cast<Trigger>(ordinal);
    if (!refuses(request) || !Require(
          std::string(opt::OptimizerStatisticsLifecycleTriggerName(request.trigger)) == "unknown",
          "unknown trigger became manual analyze")) return false;
  }
  for (int ordinal : {-1, 4, 255}) {
    auto request = base;
    request.current_freshness = static_cast<opt::OptimizerStatsFreshnessState>(ordinal);
    if (!refuses(request)) return false;
  }
  for (bool opt::OptimizerStatisticsLifecycleRequest::* flag : {
       &opt::OptimizerStatisticsLifecycleRequest::policy_enabled,
       &opt::OptimizerStatisticsLifecycleRequest::security_context_present,
       &opt::OptimizerStatisticsLifecycleRequest::grants_proven,
       &opt::OptimizerStatisticsLifecycleRequest::mga_visibility_recheck_present,
       &opt::OptimizerStatisticsLifecycleRequest::security_recheck_present,
       &opt::OptimizerStatisticsLifecycleRequest::epoch_evidence_present,
       &opt::OptimizerStatisticsLifecycleRequest::agent_policy_safe,
       &opt::OptimizerStatisticsLifecycleRequest::catalog_descriptor_present,
       &opt::OptimizerStatisticsLifecycleRequest::catalog_write_admitted,
       &opt::OptimizerStatisticsLifecycleRequest::agent_runtime_registered,
       &opt::OptimizerStatisticsLifecycleRequest::agent_schedule_admitted}) {
    auto request = base; request.*flag = false;
    if (!Require(!(opt::OptimizerStatisticsLifecycleRequest{}.*flag),
                 "planning authority defaulted true") || !refuses(request)) return false;
  }
  for (unsigned variant = 0; variant != 14; ++variant) {
    auto request = base;
    switch (variant) {
      case 0: request.relation_uuid = {}; break;
      case 1: request.relation_uuid.bytes[6] = 0x40; break;
      case 2: request.relation_uuid.bytes[8] = 0xc0; break;
      case 3: request.column_uuids[1] = {}; break;
      case 4: request.column_uuids[1] = request.column_uuids[0]; break;
      case 5: request.column_uuids[0].bytes[6] = 0x10; break;
      case 6: request.column_uuids.clear(); break;
      case 7: request.observed_table_stats->identity.object_uuid = Identity(5); break;
      case 8: request.observed_table_stats->identity.statistic_uuid = {}; break;
      case 9: request.observed_table_stats->identity.catalog_epoch = 21; break;
      case 10: request.observed_table_stats->identity.transaction_visibility_epoch = 51; break;
      case 11: request.observed_table_stats->identity.source = opt::StatisticSource::kRuntimeMetric; break;
      case 12: request.observed_table_stats->visible_row_count = 4097; break;
      case 13: request.observed_table_stats->page_count = 0; break;
    }
    if (!refuses(request)) return false;
  }
  for (const auto trigger : {Trigger::kManualAnalyze, Trigger::kSampledRefresh}) {
    auto request = base; request.trigger = trigger;
    request.agent_policy_safe = request.agent_runtime_registered =
        request.agent_schedule_admitted = false;
    request.observed_table_stats.reset();
    auto r = opt::EvaluateOptimizerStatisticsLifecycle(request);
    if (!Require(r.accepted && !r.observed_table_stats && !r.agent_schedule_planned,
                 "refresh requires an invented prior sample or agent context")) return false;
    auto empty = *base.observed_table_stats;
    empty.row_count = empty.visible_row_count = empty.page_count = empty.average_row_bytes = 0;
    request.observed_table_stats = empty;
    r = opt::EvaluateOptimizerStatisticsLifecycle(request);
    if (!Require(r.accepted && r.observed_table_stats &&
                 r.observed_table_stats->row_count == 0 &&
                 r.observed_table_stats->page_count == 0 &&
                 r.observed_table_stats->identity.statistic_uuid == empty.identity.statistic_uuid,
                 "empty observation dropped or rewritten")) return false;
  }
  for (const auto trigger : {Trigger::kHistogramRebuild, Trigger::kMcvRebuild}) {
    auto request = base; request.trigger = trigger;
    request.histogram_bucket_target = request.mcv_entry_target = 0;
    if (!refuses(request)) return false;
  }
  auto max = base;
  max.request_stats_epoch = std::numeric_limits<std::uint64_t>::max();
  if (!refuses(max)) return false;
  max.trigger = Trigger::kStaleDetection; max.rows_modified_since_stats = 0;
  const auto none = opt::EvaluateOptimizerStatisticsLifecycle(max);
  if (!Require(!none.accepted && none.decision ==
      opt::OptimizerStatisticsLifecycleDecision::kNoRefreshNeeded &&
      none.failure == Failure::kNone && none.diagnostic_code.empty() &&
      none.next_stats_epoch == max.current_stats_epoch,
      "no-refresh unexpectedly overflowed or reported an error")) return false;
  max = base; max.stats_visibility_epoch = std::numeric_limits<std::uint64_t>::max();
  const auto unchanged = opt::EvaluateOptimizerStatisticsLifecycle(max);
  if (!Require(unchanged.accepted && unchanged.next_stats_visibility_epoch ==
      max.stats_visibility_epoch && unchanged.observed_table_stats->identity.transaction_visibility_epoch == 50,
      "visibility horizon incremented or prior observation rewritten")) return false;

  // Every raw UUID byte participates in matching and binding; no textual aliases.
  for (unsigned byte = 0; byte != 16; ++byte) {
    auto request = base;
    request.relation_uuid.bytes[byte] ^= 1;
    request.observed_table_stats->identity.object_uuid = request.relation_uuid;
    const auto result = opt::EvaluateOptimizerStatisticsLifecycle(request);
    if (!Require(result.accepted &&
        opt::SerializeOptimizerStatisticsLifecycleEvidence(result) != binding,
        "relation identity byte lost")) return false;
    request = base; request.column_uuids[0].bytes[byte] ^= 4;
    const auto column = opt::EvaluateOptimizerStatisticsLifecycle(request);
    if (!Require(column.accepted &&
        column.histogram_plans[0].column_uuid == request.column_uuids[0] &&
        opt::SerializeOptimizerStatisticsLifecycleEvidence(column) != binding,
        "column identity byte lost")) return false;
  }
  auto alternate = admitted;
  alternate.evidence = {"a|b", "c"};
  auto alternate2 = admitted;
  alternate2.evidence = {"a", "b|c"};
  if (!Require(opt::SerializeOptimizerStatisticsLifecycleEvidence(alternate) !=
               opt::SerializeOptimizerStatisticsLifecycleEvidence(alternate2),
               "delimiter evidence collision")) return false;
  bool evaluated = false;
  for (long fault = 0; fault != 512; ++fault) {
    lifecycle_fault::remaining = fault; lifecycle_fault::hit = false;
    const auto result = opt::EvaluateOptimizerStatisticsLifecycle(base);
    const bool hit = lifecycle_fault::hit;
    lifecycle_fault::remaining = -1;
    if (!hit) { evaluated = result.accepted; break; }
    ++allocation_faults;
    if (!Require(result.failure == Failure::kAllocationFailure &&
         result.diagnostic_code == "SB-STAT-0001" && !result.accepted &&
         result.histogram_plans.empty() && result.mcv_plans.empty() &&
         !result.catalog_update_planned && !result.agent_schedule_planned &&
         !result.observed_table_stats && result.next_stats_epoch == 0 &&
         result.column_uuids.empty(), "allocation failure published a partial recipe")) return false;
  }
  if (!Require(evaluated, "allocation sweep never reached success")) return false;
  bool encoded = false;
  for (long fault = 0; fault != 64; ++fault) {
    lifecycle_fault::remaining = fault; lifecycle_fault::hit = false;
    bool threw = false; std::string encoded_result;
    try { encoded_result = opt::SerializeOptimizerStatisticsLifecycleEvidence(admitted); }
    catch (const std::bad_alloc&) { threw = true; }
    const bool hit = lifecycle_fault::hit;
    lifecycle_fault::remaining = -1;
    if (!hit) { encoded = encoded_result == binding; break; }
    ++allocation_faults;
    if (!Require(threw && encoded_result.empty(), "serialization returned truncated success")) return false;
  }
  return Require(encoded, "encoding allocation sweep never reached success");
}

}  // namespace

int main() {
  try {
    if (!TriggerAdmissionsCoverLifecycle()) return 1;
    if (!ExactRefusalsCoverPolicySecurityMgaAndEpochs()) return 1;
    if (!BinaryPlanningAndFailureAtomicity()) return 1;
    std::cout << "statistics lifecycle checks=" << checks
              << " allocation_faults=" << allocation_faults << '\n';
    return 0;
  } catch (const std::exception& error) {
    lifecycle_fault::remaining = -1;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
