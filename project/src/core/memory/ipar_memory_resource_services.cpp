// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ipar_memory_resource_services.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr const char* kResidencyAnchor = "IPAR-P6-11_HOT_DESCRIPTOR_INDEX_MEMORY_RESIDENCY";
constexpr const char* kGovernanceAnchor = "IPAR-P6-14_DML_DDL_RESOURCE_GOVERNANCE";
constexpr const char* kPrewarmAnchor = "IPAR-P6-19_HOT_OBJECT_WORKING_SET_PREWARMER";
constexpr const char* kStatementPoolAnchor = "IPAR-P6-25_PER_STATEMENT_ARENA_SLAB_POOLS";
constexpr const char* kAuthorityScope =
    "ipar_memory_support.cache_only_no_transaction_visibility_security_recovery_or_parser_authority";

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::memory};
}

Status ErrorStatus(StatusCode code = StatusCode::memory_invalid_request) {
  return {code, Severity::error, Subsystem::memory};
}

DiagnosticRecord Diagnostic(Status status,
                            std::string code,
                            std::string message,
                            std::vector<DiagnosticArgument> arguments = {}) {
  arguments.push_back({"authority_scope", kAuthorityScope});
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(code),
                        std::move(message),
                        std::move(arguments),
                        {},
                        "core.memory.ipar_memory_resource_services",
                        "preserve MGA and UUID authority; use these services only for bounded support-state admission");
}

bool UnsafeAuthority(const IparMemoryAuthorityScope& authority,
                     std::string* reason) {
  if (!authority.engine_mga_authoritative) {
    *reason = "engine_mga_authority_required";
    return true;
  }
  if (!authority.uuid_resolved_operation) {
    *reason = "uuid_resolved_operation_required";
    return true;
  }
  if (!authority.security_recheck_preserved) {
    *reason = "security_recheck_required";
    return true;
  }
  if (!authority.visibility_recheck_preserved) {
    *reason = "visibility_recheck_required";
    return true;
  }
  if (!authority.support_service_cache_only) {
    *reason = "support_service_must_be_cache_only";
    return true;
  }
  if (authority.memory_finality_authority ||
      authority.parser_execution_authority ||
      authority.external_recovery_authority ||
      authority.benchmark_authority ||
      authority.agent_action_authority) {
    *reason = "unsafe_authority_claim_refused";
    return true;
  }
  return false;
}

bool EpochComplete(const IparResidencyEpoch& epoch) {
  return epoch.catalog_epoch != 0 &&
         epoch.descriptor_epoch != 0 &&
         epoch.security_epoch != 0 &&
         epoch.policy_epoch != 0 &&
         epoch.resource_epoch != 0;
}

bool SameEpoch(const IparResidencyEpoch& left,
               const IparResidencyEpoch& right) {
  return left.catalog_epoch == right.catalog_epoch &&
         left.descriptor_epoch == right.descriptor_epoch &&
         left.security_epoch == right.security_epoch &&
         left.policy_epoch == right.policy_epoch &&
         left.resource_epoch == right.resource_epoch;
}

u64 KindBaseScore(IparResidencyKind kind) {
  switch (kind) {
    case IparResidencyKind::table_descriptor: return 160;
    case IparResidencyKind::index_root: return 150;
    case IparResidencyKind::index_upper_page: return 120;
    case IparResidencyKind::row_encoder: return 145;
    case IparResidencyKind::prepared_descriptor: return 155;
    case IparResidencyKind::page_buffer: return 90;
    case IparResidencyKind::batch_arena: return 110;
    case IparResidencyKind::security_mask: return 130;
    case IparResidencyKind::resource_pack: return 125;
  }
  return 1;
}

u64 SaturatingAdd(u64 left, u64 right) {
  if (right > std::numeric_limits<u64>::max() - left) {
    return std::numeric_limits<u64>::max();
  }
  return left + right;
}

u64 ResidencyScore(const IparResidencyEntry& entry) {
  u64 score = KindBaseScore(entry.kind);
  score = SaturatingAdd(score, entry.priority * 16);
  score = SaturatingAdd(score, entry.rebuild_cost_units * 4);
  score = SaturatingAdd(score, std::min<u64>(entry.hit_count, 1000));
  score = SaturatingAdd(score, entry.last_access_tick / 16);
  if (entry.pinned) {
    score = SaturatingAdd(score, 10000);
  }
  return score;
}

bool PressureActive(MemoryPressureState state) {
  return state == MemoryPressureState::soft_pressure ||
         state == MemoryPressureState::high_pressure ||
         state == MemoryPressureState::emergency_pressure ||
         state == MemoryPressureState::recovery;
}

IparHotResidencyDecision MemoryFail(std::string anchor,
                                    std::string code,
                                    std::string message,
                                    std::string reason) {
  IparHotResidencyDecision result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = Diagnostic(result.status,
                                 std::move(code),
                                 std::move(message),
                                 {{"slice", std::move(anchor)},
                                  {"reason", std::move(reason)}});
  result.evidence.push_back(kAuthorityScope);
  result.evidence.push_back("ipar_memory_support.fail_closed=true");
  return result;
}

void AddBaseEvidence(std::vector<std::string>* evidence, const char* anchor) {
  evidence->push_back(anchor);
  evidence->push_back(kAuthorityScope);
  evidence->push_back("ipar_memory_support.sblr_uuid_only=true");
  evidence->push_back("ipar_memory_support.cache_recheck_required=true");
}

u64 ScopeOverage(u64 used, u64 request, u64 limit) {
  if (limit == 0 || used + request <= limit || request > std::numeric_limits<u64>::max() - used) {
    return request > std::numeric_limits<u64>::max() - used
               ? std::numeric_limits<u64>::max()
               : 0;
  }
  return (used + request) - limit;
}

bool Blank(const std::string& value) {
  return value.find_first_not_of(" \t\r\n") == std::string::npos;
}

void AddUniqueClass(std::set<u64>* sizes, u64 bytes) {
  if (bytes == 0) {
    return;
  }
  u64 klass = 64;
  while (klass < bytes && klass < (1ull << 20)) {
    klass *= 2;
  }
  sizes->insert(klass);
}

}  // namespace

const char* IparResidencyKindName(IparResidencyKind kind) {
  switch (kind) {
    case IparResidencyKind::table_descriptor: return "table_descriptor";
    case IparResidencyKind::index_root: return "index_root";
    case IparResidencyKind::index_upper_page: return "index_upper_page";
    case IparResidencyKind::row_encoder: return "row_encoder";
    case IparResidencyKind::prepared_descriptor: return "prepared_descriptor";
    case IparResidencyKind::page_buffer: return "page_buffer";
    case IparResidencyKind::batch_arena: return "batch_arena";
    case IparResidencyKind::security_mask: return "security_mask";
    case IparResidencyKind::resource_pack: return "resource_pack";
  }
  return "unknown";
}

const char* IparResidencyActionName(IparResidencyAction action) {
  switch (action) {
    case IparResidencyAction::keep_hot: return "keep_hot";
    case IparResidencyAction::keep_warm: return "keep_warm";
    case IparResidencyAction::evict_by_policy: return "evict_by_policy";
    case IparResidencyAction::refuse: return "refuse";
  }
  return "refuse";
}

const char* IparGovernedWorkKindName(IparGovernedWorkKind kind) {
  switch (kind) {
    case IparGovernedWorkKind::dml: return "dml";
    case IparGovernedWorkKind::ddl: return "ddl";
    case IparGovernedWorkKind::support_agent: return "support_agent";
  }
  return "dml";
}

const char* IparGovernanceActionName(IparGovernanceAction action) {
  switch (action) {
    case IparGovernanceAction::admit: return "admit";
    case IparGovernanceAction::throttle: return "throttle";
    case IparGovernanceAction::trim_then_admit: return "trim_then_admit";
    case IparGovernanceAction::refuse: return "refuse";
  }
  return "refuse";
}

IparHotResidencyDecision PlanIparHotResidency(
    const std::vector<IparResidencyEntry>& entries,
    IparHotResidencyPolicy policy,
    MemoryPressureState pressure_state,
    IparMemoryAuthorityScope authority) {
  std::string unsafe_reason;
  if (UnsafeAuthority(authority, &unsafe_reason)) {
    return MemoryFail(kResidencyAnchor,
                      "SB_IPAR_MEMORY.UNSAFE_AUTHORITY",
                      "ipar.memory.unsafe_authority",
                      std::move(unsafe_reason));
  }
  if (policy.max_resident_bytes == 0 || policy.pressure_target_bytes == 0 ||
      policy.pressure_target_bytes > policy.max_resident_bytes) {
    return MemoryFail(kResidencyAnchor,
                      "SB_IPAR_MEMORY.RESIDENCY_POLICY_INVALID",
                      "ipar.memory.residency_policy_invalid",
                      "resident limits must be nonzero and ordered");
  }

  std::vector<IparResidencyDecisionEntry> scored;
  scored.reserve(entries.size());
  for (const auto& entry : entries) {
    IparResidencyDecisionEntry decision_entry;
    decision_entry.entry = entry;
    decision_entry.score = ResidencyScore(entry);
    if (entry.entry_id.empty() || entry.object_uuid.empty()) {
      decision_entry.action = IparResidencyAction::refuse;
      decision_entry.reason = "identity_required";
    } else if (!EpochComplete(entry.epoch)) {
      decision_entry.action = IparResidencyAction::refuse;
      decision_entry.reason = "complete_epoch_required";
    } else if (entry.bytes == 0 || entry.bytes > policy.max_entry_bytes) {
      decision_entry.action = IparResidencyAction::refuse;
      decision_entry.reason = "entry_size_out_of_policy";
    } else {
      decision_entry.action = IparResidencyAction::keep_warm;
      decision_entry.reason = "candidate";
    }
    scored.push_back(std::move(decision_entry));
  }

  std::sort(scored.begin(), scored.end(),
            [](const IparResidencyDecisionEntry& left,
               const IparResidencyDecisionEntry& right) {
              if (left.entry.pinned != right.entry.pinned) {
                return left.entry.pinned && !right.entry.pinned;
              }
              if (left.score != right.score) {
                return left.score > right.score;
              }
              return left.entry.last_access_tick > right.entry.last_access_tick;
            });

  const bool trim = policy.trim_to_target_under_pressure &&
                    PressureActive(pressure_state);
  const u64 target = trim ? policy.pressure_target_bytes
                          : policy.max_resident_bytes;
  u64 admitted = 0;
  u64 evicted = 0;
  for (auto& entry : scored) {
    if (entry.action == IparResidencyAction::refuse) {
      evicted = SaturatingAdd(evicted, entry.entry.bytes);
      continue;
    }
    const bool over_target =
        entry.entry.bytes > std::numeric_limits<u64>::max() - admitted ||
        admitted + entry.entry.bytes > target;
    if (over_target && !entry.entry.pinned) {
      entry.action = IparResidencyAction::evict_by_policy;
      entry.reason = trim ? "pressure_target_exceeded" : "resident_limit_exceeded";
      evicted = SaturatingAdd(evicted, entry.entry.bytes);
      continue;
    }
    if (over_target && entry.entry.pinned) {
      entry.action = IparResidencyAction::keep_hot;
      entry.reason = "pinned_entry_preserved";
    } else if (entry.score >= policy.hot_score_threshold || entry.entry.pinned) {
      entry.action = IparResidencyAction::keep_hot;
      entry.reason = "hot_score_admitted";
    } else {
      entry.action = IparResidencyAction::keep_warm;
      entry.reason = "warm_score_admitted";
    }
    admitted = SaturatingAdd(admitted, entry.entry.bytes);
  }

  IparHotResidencyDecision result;
  result.status = OkStatus();
  result.admitted_bytes = admitted;
  result.evicted_bytes = evicted;
  result.entries = std::move(scored);
  result.diagnostic = Diagnostic(result.status,
                                 "SB_IPAR_MEMORY.RESIDENCY_PLANNED",
                                 "ipar.memory.residency_planned",
                                 {{"pressure_state", MemoryPressureStateName(pressure_state)},
                                  {"admitted_bytes", std::to_string(admitted)},
                                  {"evicted_bytes", std::to_string(evicted)}});
  AddBaseEvidence(&result.evidence, kResidencyAnchor);
  result.evidence.push_back("ipar.residency.target_bytes=" + std::to_string(target));
  result.evidence.push_back("ipar.residency.per_row_allocation_avoided=true");
  result.evidence.push_back("ipar.residency.pressure_eviction_policy=true");
  return result;
}

IparResourceGovernanceDecision PlanIparResourceGovernance(
    IparResourceGovernanceRequest request) {
  IparResourceGovernanceDecision result;
  std::string unsafe_reason;
  if (UnsafeAuthority(request.authority, &unsafe_reason)) {
    result.status = ErrorStatus();
    result.fail_closed = true;
    result.action = IparGovernanceAction::refuse;
    result.diagnostic = Diagnostic(result.status,
                                   "SB_IPAR_GOVERNANCE.UNSAFE_AUTHORITY",
                                   "ipar.governance.unsafe_authority",
                                   {{"slice", kGovernanceAnchor},
                                    {"reason", std::move(unsafe_reason)}});
    AddBaseEvidence(&result.evidence, kGovernanceAnchor);
    result.evidence.push_back("ipar.governance.fail_closed=true");
    return result;
  }
  if (request.operation_id.empty() || request.database_id.empty()) {
    result.status = ErrorStatus();
    result.fail_closed = true;
    result.action = IparGovernanceAction::refuse;
    result.diagnostic = Diagnostic(result.status,
                                   "SB_IPAR_GOVERNANCE.IDENTITY_REQUIRED",
                                   "ipar.governance.identity_required",
                                   {{"slice", kGovernanceAnchor}});
    AddBaseEvidence(&result.evidence, kGovernanceAnchor);
    return result;
  }

  bool refuse = false;
  bool throttle = PressureActive(request.pressure_state);
  for (const auto& scope : request.scopes) {
    const u64 memory_over =
        ScopeOverage(scope.memory_used_bytes,
                     request.requested_memory_bytes,
                     scope.memory_limit_bytes);
    const u64 dirty_over =
        ScopeOverage(scope.dirty_pages,
                     request.requested_dirty_pages,
                     scope.dirty_page_limit);
    const u64 queue_over =
        ScopeOverage(scope.queue_depth,
                     request.requested_queue_slots,
                     scope.queue_depth_limit);
    if (memory_over != 0) {
      refuse = true;
      result.throttle_reasons.push_back(scope.scope_kind + ":" +
                                        scope.scope_id + ":memory_limit");
    }
    if (dirty_over != 0 || queue_over != 0) {
      throttle = true;
      result.throttle_reasons.push_back(scope.scope_kind + ":" +
                                        scope.scope_id + ":dirty_or_queue_depth");
    }
  }

  if (request.work_kind != IparGovernedWorkKind::support_agent &&
      request.total_governed_limit_bytes != 0 &&
      request.support_agent_reserved_bytes > request.support_agent_used_bytes) {
    const u64 reserve_gap =
        request.support_agent_reserved_bytes - request.support_agent_used_bytes;
    const bool reserve_would_be_crossed =
        request.total_governed_used_bytes + request.requested_memory_bytes >
        request.total_governed_limit_bytes - reserve_gap;
    if (reserve_would_be_crossed) {
      throttle = true;
      result.throttle_reasons.push_back("support_agent_min_share_reserved");
    }
  }

  result.status = refuse ? ErrorStatus(StatusCode::memory_limit_exceeded)
                         : OkStatus();
  result.fail_closed = refuse;
  if (refuse) {
    result.action = IparGovernanceAction::refuse;
  } else if (request.pressure_state == MemoryPressureState::high_pressure ||
             request.pressure_state == MemoryPressureState::emergency_pressure) {
    result.action = IparGovernanceAction::trim_then_admit;
  } else if (throttle) {
    result.action = IparGovernanceAction::throttle;
  } else {
    result.action = IparGovernanceAction::admit;
  }

  const bool full_admit = result.action == IparGovernanceAction::admit ||
                          result.action == IparGovernanceAction::trim_then_admit;
  result.admitted_memory_bytes = full_admit ? request.requested_memory_bytes
                                            : request.requested_memory_bytes / 2;
  result.admitted_dirty_pages = full_admit ? request.requested_dirty_pages
                                           : request.requested_dirty_pages / 2;
  result.admitted_queue_slots = full_admit ? request.requested_queue_slots
                                           : std::min<u64>(1, request.requested_queue_slots);
  if (refuse) {
    result.admitted_memory_bytes = 0;
    result.admitted_dirty_pages = 0;
    result.admitted_queue_slots = 0;
  }

  result.diagnostic = Diagnostic(result.status,
                                 refuse ? "SB_IPAR_GOVERNANCE.REFUSED"
                                        : "SB_IPAR_GOVERNANCE.PLANNED",
                                 refuse ? "ipar.governance.refused"
                                        : "ipar.governance.planned",
                                 {{"slice", kGovernanceAnchor},
                                  {"action", IparGovernanceActionName(result.action)},
                                  {"work_kind", IparGovernedWorkKindName(request.work_kind)}});
  AddBaseEvidence(&result.evidence, kGovernanceAnchor);
  result.evidence.push_back("ipar.governance.per_session_database_filespace_scopes=true");
  result.evidence.push_back("ipar.governance.dirty_page_queue_depth_accounted=true");
  result.evidence.push_back("ipar.governance.support_agent_min_share_preserved=true");
  return result;
}

IparPrewarmDecision PlanIparWorkingSetPrewarm(IparPrewarmRequest request) {
  IparPrewarmDecision result;
  std::string unsafe_reason;
  if (UnsafeAuthority(request.authority, &unsafe_reason)) {
    result.status = ErrorStatus();
    result.fail_closed = true;
    result.diagnostic = Diagnostic(result.status,
                                   "SB_IPAR_PREWARM.UNSAFE_AUTHORITY",
                                   "ipar.prewarm.unsafe_authority",
                                   {{"slice", kPrewarmAnchor},
                                    {"reason", std::move(unsafe_reason)}});
    AddBaseEvidence(&result.evidence, kPrewarmAnchor);
    return result;
  }
  if (request.database_id.empty() || request.budget_bytes == 0) {
    result.status = ErrorStatus();
    result.fail_closed = true;
    result.diagnostic = Diagnostic(result.status,
                                   "SB_IPAR_PREWARM.REQUEST_INVALID",
                                   "ipar.prewarm.request_invalid",
                                   {{"slice", kPrewarmAnchor}});
    AddBaseEvidence(&result.evidence, kPrewarmAnchor);
    return result;
  }

  std::sort(request.candidates.begin(),
            request.candidates.end(),
            [](const IparPrewarmCandidate& left,
               const IparPrewarmCandidate& right) {
              const u64 left_score =
                  left.recent_hits * 8 + left.cold_start_cost_units * 4 +
                  left.last_access_tick;
              const u64 right_score =
                  right.recent_hits * 8 + right.cold_start_cost_units * 4 +
                  right.last_access_tick;
              return left_score > right_score;
            });

  for (const auto& candidate : request.candidates) {
    result.cold_start_cost_before =
        SaturatingAdd(result.cold_start_cost_before,
                      candidate.cold_start_cost_units);
    if (candidate.candidate_id.empty() || candidate.object_uuid.empty()) {
      result.skipped.push_back("identity_required");
      continue;
    }
    if (!candidate.authorization_checked || !candidate.policy_checked ||
        !candidate.security_mask_checked) {
      result.skipped.push_back(candidate.candidate_id + ":security_or_policy_check_missing");
      continue;
    }
    if (!SameEpoch(candidate.required_epoch, candidate.observed_epoch)) {
      result.skipped.push_back(candidate.candidate_id + ":epoch_mismatch");
      continue;
    }
    if (candidate.bytes == 0 ||
        candidate.bytes > request.budget_bytes ||
        result.selected_bytes + candidate.bytes > request.budget_bytes) {
      result.skipped.push_back(candidate.candidate_id + ":budget");
      continue;
    }
    result.selected.push_back(candidate);
    result.selected_bytes += candidate.bytes;
    result.cold_start_cost_after =
        SaturatingAdd(result.cold_start_cost_after,
                      candidate.cold_start_cost_units / 8);
  }

  result.status = OkStatus();
  result.diagnostic = Diagnostic(result.status,
                                 "SB_IPAR_PREWARM.PLANNED",
                                 "ipar.prewarm.planned",
                                 {{"slice", kPrewarmAnchor},
                                  {"selected", std::to_string(result.selected.size())}});
  AddBaseEvidence(&result.evidence, kPrewarmAnchor);
  result.evidence.push_back("ipar.prewarm.descriptor_index_security_page_resource_candidates=true");
  result.evidence.push_back("ipar.prewarm.cold_start_cost_bounded=true");
  return result;
}

IparStatementPoolPlan PlanIparStatementMemoryPools(
    IparStatementPoolRequest request) {
  IparStatementPoolPlan result;
  std::string unsafe_reason;
  if (UnsafeAuthority(request.authority, &unsafe_reason) ||
      !request.context.engine_mga_authoritative ||
      request.context.parser_or_reference_finality_or_visibility_authority ||
      request.context.client_finality_or_visibility_authority ||
      request.context.provider_finality_or_visibility_authority ||
      request.context.wal_recovery_or_finality_authority) {
    result.status = ErrorStatus();
    result.fail_closed = true;
    result.diagnostic = Diagnostic(result.status,
                                   "SB_IPAR_STATEMENT_POOL.UNSAFE_AUTHORITY",
                                   "ipar.statement_pool.unsafe_authority",
                                   {{"slice", kStatementPoolAnchor},
                                    {"reason", unsafe_reason.empty()
                                                   ? "unsafe_query_memory_context"
                                                   : unsafe_reason}});
    AddBaseEvidence(&result.evidence, kStatementPoolAnchor);
    return result;
  }
  if (Blank(request.context.statement_id) ||
      Blank(request.context.session_id) ||
      Blank(request.context.transaction_id) ||
      Blank(request.context.database_id) ||
      request.statement_limit_bytes == 0 ||
      request.max_batch_rows == 0) {
    result.status = ErrorStatus();
    result.fail_closed = true;
    result.diagnostic = Diagnostic(result.status,
                                   "SB_IPAR_STATEMENT_POOL.REQUEST_INVALID",
                                   "ipar.statement_pool.request_invalid",
                                   {{"slice", kStatementPoolAnchor}});
    AddBaseEvidence(&result.evidence, kStatementPoolAnchor);
    return result;
  }

  const u64 row_pool = request.row_version_bytes * request.max_batch_rows;
  const u64 key_pool = request.key_buffer_bytes * request.max_batch_rows;
  const u64 diagnostic_pool = request.diagnostic_bytes * std::min<u64>(request.max_batch_rows, 16);
  const u64 coercion_pool = request.coercion_temp_bytes * request.max_batch_rows;
  const u64 scratch_pool = request.scratch_bytes * request.max_batch_rows;
  const u64 requested_total = row_pool + key_pool + diagnostic_pool +
                              coercion_pool + scratch_pool;
  if (requested_total == 0 || requested_total > request.statement_limit_bytes) {
    result.status = ErrorStatus(StatusCode::memory_limit_exceeded);
    result.fail_closed = true;
    result.diagnostic = Diagnostic(result.status,
                                   "SB_IPAR_STATEMENT_POOL.LIMIT_EXCEEDED",
                                   "ipar.statement_pool.limit_exceeded",
                                   {{"slice", kStatementPoolAnchor},
                                    {"requested_bytes", std::to_string(requested_total)}});
    AddBaseEvidence(&result.evidence, kStatementPoolAnchor);
    return result;
  }

  result.status = OkStatus();
  result.row_version_pool_bytes = row_pool;
  result.key_buffer_pool_bytes = key_pool;
  result.diagnostic_pool_bytes = diagnostic_pool;
  result.coercion_pool_bytes = coercion_pool;
  result.batch_reset_bytes = request.batch_limit_bytes == 0
                                 ? requested_total
                                 : std::min(request.batch_limit_bytes, requested_total);
  result.arena_limits.hard_limit_bytes = request.statement_limit_bytes;
  result.arena_limits.soft_limit_bytes = result.batch_reset_bytes;
  result.arena_limits.family_limit_bytes = request.statement_limit_bytes;
  result.arena_limits.query_limit_bytes = request.statement_limit_bytes;
  result.arena_limits.allow_spill = false;
  result.arena_limits.require_hierarchical_reservation = false;
  result.arena_grant.family = QueryMemoryFamily::dml;
  result.arena_grant.bytes = requested_total;
  result.arena_grant.spillable = false;
  result.arena_grant.purpose = "ipar_statement_pool_batch_region";

  std::set<u64> sizes;
  AddUniqueClass(&sizes, request.row_version_bytes);
  AddUniqueClass(&sizes, request.key_buffer_bytes);
  AddUniqueClass(&sizes, request.diagnostic_bytes);
  AddUniqueClass(&sizes, request.coercion_temp_bytes);
  AddUniqueClass(&sizes, request.scratch_bytes);
  const u64 slots = request.preferred_slots_per_slab == 0
                        ? std::min<u64>(64, std::max<u64>(8, request.max_batch_rows))
                        : request.preferred_slots_per_slab;
  for (const auto size : sizes) {
    result.slab_size_classes.push_back(
        {static_cast<usize>(size), static_cast<usize>(slots)});
  }

  result.reset_boundaries = {"statement_end", "batch_end", "rollback_to_savepoint"};
  result.diagnostic = Diagnostic(result.status,
                                 "SB_IPAR_STATEMENT_POOL.PLANNED",
                                 "ipar.statement_pool.planned",
                                 {{"slice", kStatementPoolAnchor},
                                  {"arena_bytes", std::to_string(requested_total)}});
  AddBaseEvidence(&result.evidence, kStatementPoolAnchor);
  result.evidence.push_back("ipar.statement_pool.per_statement_arena=true");
  result.evidence.push_back("ipar.statement_pool.typed_slab_classes=true");
  result.evidence.push_back("ipar.statement_pool.batch_reset_boundaries=true");
  return result;
}

}  // namespace scratchbird::core::memory
