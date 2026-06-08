// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_DYNAMIC_CLEANUP_DEBT_SCHEDULER
// Database-local cleanup debt scheduler. This is a prioritization and bounded
// work-selection surface only; durable MGA transaction inventory and the
// cleanup horizon service remain finality and visibility authority.

#include "agent_runtime.hpp"
#include "agents/storage_version_cleanup_agent.hpp"
#include "deferred_secondary_index_runtime_policy.hpp"
#include "exact_index_leaf_cleanup.hpp"
#include "page_extent_summary.hpp"
#include "time_range_summary_pruning.hpp"
#include "transaction_cleanup_horizon_service.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents {

using scratchbird::core::index::ExactIndexLeafPressureDecision;
using scratchbird::core::index::PageAwareSecondaryChangeBufferDecision;
using scratchbird::core::index::PageAwareSecondaryChangeBufferRequest;
using scratchbird::core::index::PageExtentSummaryMetadata;
using scratchbird::core::index::PersistentSecondaryIndexDeltaLedger;
using scratchbird::core::index::TimeRangeSummaryPrunePlan;
using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::transaction::mga::AuthoritativeCleanupHorizonResult;

enum class DynamicCleanupDebtFamily : u32 {
  version_chain = 1,
  exact_index_leaf = 2,
  secondary_delta_ledger = 3,
  summary_page_range = 4,
  large_value = 5,
  hot_leaf = 6,
  nosql_key_value = 7,
  nosql_document = 8,
  nosql_search = 9,
  nosql_vector = 10,
  nosql_graph = 11,
  nosql_time_series = 12
};

enum class DynamicCleanupDebtWorkKind : u32 {
  storage_version_sweep = 1,
  exact_index_leaf_cleanup = 2,
  secondary_delta_merge_or_cleanup = 3,
  summary_refresh_or_rebuild = 4,
  large_value_reclaim = 5,
  hot_leaf_pressure_relief = 6,
  nosql_key_value_ttl_retirement = 7,
  nosql_key_value_generation_compaction = 8,
  nosql_document_generation_merge = 9,
  nosql_search_segment_merge = 10,
  nosql_vector_generation_retirement = 11,
  nosql_graph_adjacency_compaction = 12,
  nosql_time_series_bucket_retirement = 13
};

enum class DynamicCleanupDebtDecisionKind : u32 {
  scheduled = 1,
  no_op = 2,
  skipped_no_debt = 3,
  deferred_cooldown = 4,
  deferred_lease = 5,
  deferred_contention = 6,
  deferred_family_cap = 7,
  refused_authority = 8,
  refused_budget = 9,
  refused_source = 10
};

enum class DynamicCleanupDebtFailureMode : u32 {
  fail_open_to_foreground = 1,
  fail_closed_retain_debt = 2
};

struct DynamicCleanupDebtEvidenceField {
  std::string key;
  std::string value;
};

struct DynamicCleanupDebtSource {
  DynamicCleanupDebtFamily family = DynamicCleanupDebtFamily::version_chain;
  DynamicCleanupDebtWorkKind work_kind =
      DynamicCleanupDebtWorkKind::storage_version_sweep;
  DynamicCleanupDebtFailureMode failure_mode =
      DynamicCleanupDebtFailureMode::fail_closed_retain_debt;

  std::string stable_work_key;
  std::string database_uuid;
  std::string relation_uuid;
  std::string index_uuid;
  std::string object_uuid;
  std::string source_detail;

  u64 debt_units = 0;
  u64 debt_bytes = 0;
  u64 estimated_work_units = 0;
  u64 max_work_units = 0;
  u64 age_microseconds = 0;
  u64 priority_boost = 0;
  u64 blocker_count = 0;
  u64 cleanup_horizon_local_transaction_id = 0;

  bool source_authoritative = true;
  bool requires_mga_cleanup_horizon = true;
  bool destructive_cleanup = true;
  bool recovery_proof_available = true;
  bool bounded_cleanup_available = true;
  bool lease_active = false;
  u64 lease_until_microseconds = 0;
  bool worker_contention_observed = false;
  u64 last_attempt_microseconds = 0;
  u64 next_eligible_microseconds = 0;
  u64 failure_count = 0;

  std::vector<DynamicCleanupDebtEvidenceField> evidence;
};

struct DynamicCleanupDebtFamilyCap {
  DynamicCleanupDebtFamily family = DynamicCleanupDebtFamily::version_chain;
  u64 max_work_units = 0;
  u64 max_items = 0;
};

struct DynamicCleanupDebtSchedulerPolicy {
  bool enabled = true;
  bool protect_foreground_work = true;
  u64 max_total_work_units = 64;
  u64 max_scheduled_items = 8;
  u64 default_max_family_work_units = 16;
  u64 default_max_family_items = 2;
  u64 max_work_units_per_item = 16;
  u64 min_retry_backoff_microseconds = 1000000;
  u64 max_retry_backoff_microseconds = 60000000;
  u64 contention_backoff_microseconds = 5000000;
  u64 lease_duration_microseconds = 5000000;
  std::vector<DynamicCleanupDebtFamilyCap> family_caps;
};

struct DynamicCleanupDebtSchedulerRequest {
  DynamicCleanupDebtSchedulerPolicy policy;
  AuthoritativeCleanupHorizonResult cleanup_horizon;
  std::vector<DynamicCleanupDebtSource> sources;
  u64 now_microseconds = 0;
  bool engine_mga_authoritative = false;
  bool foreground_work_active = false;
};

struct DynamicCleanupDebtAssignment {
  DynamicCleanupDebtSource source;
  DynamicCleanupDebtDecisionKind decision =
      DynamicCleanupDebtDecisionKind::no_op;
  DynamicCleanupDebtFailureMode failure_mode =
      DynamicCleanupDebtFailureMode::fail_closed_retain_debt;
  u64 score = 0;
  u64 scheduled_work_units = 0;
  u64 priority_rank = 0;
  u64 next_eligible_microseconds = 0;
  std::string lease_token;
  std::string diagnostic_code;
  std::string detail;
  std::vector<DynamicCleanupDebtEvidenceField> evidence;

  bool scheduled() const {
    return decision == DynamicCleanupDebtDecisionKind::scheduled &&
           scheduled_work_units != 0;
  }
};

struct DynamicCleanupDebtFamilySummary {
  DynamicCleanupDebtFamily family = DynamicCleanupDebtFamily::version_chain;
  u64 candidate_count = 0;
  u64 scheduled_count = 0;
  u64 scheduled_work_units = 0;
  u64 skipped_count = 0;
  u64 refused_count = 0;
};

struct DynamicCleanupDebtSchedulerResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool scheduler_enabled = true;
  bool cleanup_horizon_authoritative = false;
  bool engine_mga_authoritative = false;
  bool fail_closed = false;
  bool foreground_protected = false;
  u64 effective_total_work_units = 0;
  u64 scheduled_count = 0;
  u64 scheduled_work_units = 0;
  u64 fail_open_deferred_count = 0;
  u64 fail_closed_refusal_count = 0;
  std::vector<DynamicCleanupDebtAssignment> decisions;
  std::vector<DynamicCleanupDebtFamilySummary> family_summaries;
  std::vector<DynamicCleanupDebtEvidenceField> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* DynamicCleanupDebtFamilyName(DynamicCleanupDebtFamily family);
const char* DynamicCleanupDebtWorkKindName(DynamicCleanupDebtWorkKind kind);
const char* DynamicCleanupDebtDecisionKindName(
    DynamicCleanupDebtDecisionKind decision);
const char* DynamicCleanupDebtFailureModeName(
    DynamicCleanupDebtFailureMode mode);

DynamicCleanupDebtSchedulerResult PlanDynamicCleanupDebt(
    const DynamicCleanupDebtSchedulerRequest& request);

DiagnosticRecord MakeDynamicCleanupDebtSchedulerDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

DynamicCleanupDebtSource DynamicCleanupDebtSourceFromVersionChainMetrics(
    std::string stable_work_key,
    const implemented_agents::StorageVersionCleanupPressureMetrics& metrics);
DynamicCleanupDebtSource DynamicCleanupDebtSourceFromExactIndexLeafPressure(
    std::string stable_work_key,
    const ExactIndexLeafPressureDecision& decision);
DynamicCleanupDebtSource DynamicCleanupDebtSourceFromSecondaryDeltaLedger(
    std::string stable_work_key,
    const PersistentSecondaryIndexDeltaLedger& ledger);
DynamicCleanupDebtSource DynamicCleanupDebtSourceFromSecondaryChangeBuffer(
    std::string stable_work_key,
    const PageAwareSecondaryChangeBufferRequest& request,
    const PageAwareSecondaryChangeBufferDecision& decision);
DynamicCleanupDebtSource DynamicCleanupDebtSourceFromPageRangeSummaries(
    std::string stable_work_key,
    const std::vector<PageExtentSummaryMetadata>& summaries);
DynamicCleanupDebtSource DynamicCleanupDebtSourceFromTimeRangePrunePlan(
    std::string stable_work_key,
    const TimeRangeSummaryPrunePlan& plan);
DynamicCleanupDebtSource DynamicCleanupDebtSourceFromLargeValueDebt(
    std::string stable_work_key,
    u64 orphan_value_count,
    u64 orphan_bytes,
    u64 pinned_value_count);
DynamicCleanupDebtSource DynamicCleanupDebtSourceFromHotLeafPressure(
    std::string stable_work_key,
    const PageAwareSecondaryChangeBufferRequest& request,
    const PageAwareSecondaryChangeBufferDecision& decision);

}  // namespace scratchbird::core::agents
