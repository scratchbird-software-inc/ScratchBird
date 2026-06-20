// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// IPAR-P6-11/IPAR-P6-14/IPAR-P6-19/IPAR-P6-25 support services.
#include "memory.hpp"
#include "memory_pressure_response.hpp"
#include "query_memory_arena.hpp"
#include "typed_slab_pool.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::memory {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

enum class IparResidencyKind {
  table_descriptor,
  index_root,
  index_upper_page,
  row_encoder,
  prepared_descriptor,
  page_buffer,
  batch_arena,
  security_mask,
  resource_pack
};

enum class IparResidencyAction {
  keep_hot,
  keep_warm,
  evict_by_policy,
  refuse
};

enum class IparGovernedWorkKind {
  dml,
  ddl,
  support_agent
};

enum class IparGovernanceAction {
  admit,
  throttle,
  trim_then_admit,
  refuse
};

struct IparMemoryAuthorityScope {
  bool engine_mga_authoritative = true;
  bool uuid_resolved_operation = true;
  bool security_recheck_preserved = true;
  bool visibility_recheck_preserved = true;
  bool support_service_cache_only = true;
  bool memory_finality_authority = false;
  bool parser_execution_authority = false;
  bool external_recovery_authority = false;
  bool benchmark_authority = false;
  bool agent_action_authority = false;
};

struct IparResidencyEpoch {
  u64 catalog_epoch = 0;
  u64 descriptor_epoch = 0;
  u64 security_epoch = 0;
  u64 policy_epoch = 0;
  u64 resource_epoch = 0;
};

struct IparResidencyEntry {
  std::string entry_id;
  std::string object_uuid;
  IparResidencyKind kind = IparResidencyKind::table_descriptor;
  IparResidencyEpoch epoch;
  u64 bytes = 0;
  u64 last_access_tick = 0;
  u64 hit_count = 0;
  u64 rebuild_cost_units = 1;
  u64 priority = 1;
  bool pinned = false;
  bool pressure_evictable = true;
};

struct IparHotResidencyPolicy {
  u64 max_resident_bytes = 64ull * 1024ull * 1024ull;
  u64 pressure_target_bytes = 48ull * 1024ull * 1024ull;
  u64 max_entry_bytes = 8ull * 1024ull * 1024ull;
  u64 hot_score_threshold = 128;
  bool trim_to_target_under_pressure = true;
};

struct IparResidencyDecisionEntry {
  IparResidencyEntry entry;
  IparResidencyAction action = IparResidencyAction::refuse;
  u64 score = 0;
  std::string reason;
};

struct IparHotResidencyDecision {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = false;
  u64 admitted_bytes = 0;
  u64 evicted_bytes = 0;
  std::vector<IparResidencyDecisionEntry> entries;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct IparQuotaScope {
  std::string scope_kind;
  std::string scope_id;
  u64 memory_limit_bytes = 0;
  u64 memory_used_bytes = 0;
  u64 dirty_page_limit = 0;
  u64 dirty_pages = 0;
  u64 queue_depth_limit = 0;
  u64 queue_depth = 0;
};

struct IparResourceGovernanceRequest {
  IparGovernedWorkKind work_kind = IparGovernedWorkKind::dml;
  std::string operation_id;
  std::string database_id;
  std::string session_id;
  std::string filespace_id;
  u64 requested_memory_bytes = 0;
  u64 requested_dirty_pages = 0;
  u64 requested_queue_slots = 0;
  u64 total_governed_limit_bytes = 0;
  u64 total_governed_used_bytes = 0;
  u64 support_agent_reserved_bytes = 0;
  u64 support_agent_used_bytes = 0;
  MemoryPressureState pressure_state = MemoryPressureState::normal;
  IparMemoryAuthorityScope authority;
  std::vector<IparQuotaScope> scopes;
};

struct IparResourceGovernanceDecision {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = false;
  IparGovernanceAction action = IparGovernanceAction::refuse;
  u64 admitted_memory_bytes = 0;
  u64 admitted_dirty_pages = 0;
  u64 admitted_queue_slots = 0;
  std::vector<std::string> throttle_reasons;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct IparPrewarmCandidate {
  std::string candidate_id;
  std::string object_uuid;
  IparResidencyKind kind = IparResidencyKind::table_descriptor;
  IparResidencyEpoch required_epoch;
  IparResidencyEpoch observed_epoch;
  u64 bytes = 0;
  u64 recent_hits = 0;
  u64 last_access_tick = 0;
  u64 cold_start_cost_units = 1;
  bool authorization_checked = false;
  bool policy_checked = false;
  bool security_mask_checked = false;
};

struct IparPrewarmRequest {
  std::string database_id;
  std::string session_id;
  u64 budget_bytes = 0;
  u64 now_tick = 0;
  IparMemoryAuthorityScope authority;
  std::vector<IparPrewarmCandidate> candidates;
};

struct IparPrewarmDecision {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = false;
  u64 selected_bytes = 0;
  u64 cold_start_cost_before = 0;
  u64 cold_start_cost_after = 0;
  std::vector<IparPrewarmCandidate> selected;
  std::vector<std::string> skipped;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct IparStatementPoolRequest {
  QueryMemoryContext context;
  u64 statement_limit_bytes = 0;
  u64 batch_limit_bytes = 0;
  u64 row_version_bytes = 0;
  u64 key_buffer_bytes = 0;
  u64 diagnostic_bytes = 0;
  u64 coercion_temp_bytes = 0;
  u64 scratch_bytes = 0;
  u64 max_batch_rows = 0;
  u64 preferred_slots_per_slab = 0;
  IparMemoryAuthorityScope authority;
};

struct IparStatementPoolPlan {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = false;
  QueryMemoryArenaLimits arena_limits;
  QueryMemoryGrantRequest arena_grant;
  std::vector<SizeClassConfig> slab_size_classes;
  u64 batch_reset_bytes = 0;
  u64 row_version_pool_bytes = 0;
  u64 key_buffer_pool_bytes = 0;
  u64 diagnostic_pool_bytes = 0;
  u64 coercion_pool_bytes = 0;
  std::vector<std::string> reset_boundaries;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* IparResidencyKindName(IparResidencyKind kind);
const char* IparResidencyActionName(IparResidencyAction action);
const char* IparGovernedWorkKindName(IparGovernedWorkKind kind);
const char* IparGovernanceActionName(IparGovernanceAction action);

IparHotResidencyDecision PlanIparHotResidency(
    const std::vector<IparResidencyEntry>& entries,
    IparHotResidencyPolicy policy,
    MemoryPressureState pressure_state,
    IparMemoryAuthorityScope authority = {});
IparResourceGovernanceDecision PlanIparResourceGovernance(
    IparResourceGovernanceRequest request);
IparPrewarmDecision PlanIparWorkingSetPrewarm(IparPrewarmRequest request);
IparStatementPoolPlan PlanIparStatementMemoryPools(
    IparStatementPoolRequest request);

}  // namespace scratchbird::core::memory
