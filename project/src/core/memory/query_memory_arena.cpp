// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query_memory_arena.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::memory};
}

Status ErrorStatus(StatusCode code) {
  return {code, Severity::error, Subsystem::memory};
}

MemoryTag ArenaTag(const QueryMemoryContext& context,
                   QueryMemoryFamily family,
                   const std::string& purpose) {
  MemoryTag tag;
  tag.subsystem = Subsystem::engine;
  tag.purpose = purpose.empty() ? "query_memory_grant" : purpose;
  tag.category = MemoryCategory::executor_query_reserved;
  tag.lifetime = MemoryLifetime::arena;
  tag.owner = context.query_id;
  tag.context_id = context.statement_id;
  tag.database_id = context.database_id;
  tag.session_id = context.session_id;
  tag.transaction_id = context.transaction_id;
  tag.statement_id = context.statement_id;
  tag.query_id = context.query_id;
  (void)family;
  return tag;
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

std::vector<HierarchicalMemoryScopeRef> ScopeChainForContext(
    const QueryMemoryContext& context) {
  std::vector<HierarchicalMemoryScopeRef> chain;
  const std::string process_scope =
      context.engine_id.empty() ? std::string("scratchbird-engine") : context.engine_id;
  chain.push_back({HierarchicalMemoryScopeKind::process, process_scope});
  if (!context.database_id.empty()) {
    chain.push_back({HierarchicalMemoryScopeKind::database, context.database_id});
  }
  chain.push_back({HierarchicalMemoryScopeKind::session, context.session_id});
  chain.push_back({HierarchicalMemoryScopeKind::transaction, context.transaction_id});
  chain.push_back({HierarchicalMemoryScopeKind::statement, context.statement_id});
  chain.push_back({HierarchicalMemoryScopeKind::query, context.query_id});
  if (!context.operation_id.empty()) {
    chain.push_back({HierarchicalMemoryScopeKind::operator_scope, context.operation_id});
  }
  return chain;
}

HierarchicalMemoryBudgetProvenance QueryArenaReservationProvenance() {
  HierarchicalMemoryBudgetProvenance provenance;
  provenance.source = HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  provenance.source_label = "query_memory_arena";
  provenance.engine_mga_authoritative = true;
  provenance.memory_evidence_only = true;
  return provenance;
}

}  // namespace

const char* QueryMemoryFamilyName(QueryMemoryFamily family) {
  switch (family) {
    case QueryMemoryFamily::unknown: return "unknown";
    case QueryMemoryFamily::relational: return "relational";
    case QueryMemoryFamily::search: return "search";
    case QueryMemoryFamily::vector: return "vector";
    case QueryMemoryFamily::graph: return "graph";
    case QueryMemoryFamily::document: return "document";
    case QueryMemoryFamily::time_series: return "time_series";
    case QueryMemoryFamily::dml: return "dml";
    case QueryMemoryFamily::candidate_set: return "candidate_set";
  }
  return "unknown";
}

bool QueryMemoryFamilySupported(QueryMemoryFamily family) {
  switch (family) {
    case QueryMemoryFamily::relational:
    case QueryMemoryFamily::search:
    case QueryMemoryFamily::vector:
    case QueryMemoryFamily::graph:
    case QueryMemoryFamily::document:
    case QueryMemoryFamily::time_series:
    case QueryMemoryFamily::dml:
    case QueryMemoryFamily::candidate_set:
      return true;
    case QueryMemoryFamily::unknown:
      return false;
  }
  return false;
}

const char* UnifiedMemorySpillBudgetKindName(UnifiedMemorySpillBudgetKind kind) {
  switch (kind) {
    case UnifiedMemorySpillBudgetKind::heap:
      return "heap";
    case UnifiedMemorySpillBudgetKind::spill:
      return "spill";
  }
  return "heap";
}

QueryMemoryArena::QueryMemoryArena(QueryMemoryContext context,
                                   QueryMemoryArenaLimits limits,
                                   BoundedAllocator* allocator,
                                   TempWorkspaceLifecycleManager* temp_workspace,
                                   UnifiedMemorySpillBudgetLedger* unified_budget,
                                   HierarchicalMemoryBudgetLedger* reservation_ledger)
    : context_(std::move(context)),
      limits_(limits),
      allocator_(allocator),
      temp_workspace_(temp_workspace),
      unified_budget_(unified_budget),
      reservation_ledger_(reservation_ledger) {}

QueryMemoryArena::~QueryMemoryArena() {
  (void)Reset();
}

QueryMemoryArenaResult QueryMemoryArena::Grant(QueryMemoryGrantRequest request) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (released_) {
    return Refuse(request,
                  "SB_QUERY_MEMORY_ARENA.RELEASED",
                  "query_memory_arena.released",
                  "arena already released");
  }
  if (allocator_ == nullptr) {
    return Refuse(request,
                  "SB_QUERY_MEMORY_ARENA.ALLOCATOR_REQUIRED",
                  "query_memory_arena.allocator_required",
                  "bounded allocator is required");
  }
  if (ContextMissing()) {
    return Refuse(request,
                  "SB_QUERY_MEMORY_ARENA.CONTEXT_REQUIRED",
                  "query_memory_arena.context_required",
                  "query statement session and transaction context are required");
  }
  if (UnsafeAuthority()) {
    return Refuse(request,
                  "SB_QUERY_MEMORY_ARENA.UNSAFE_AUTHORITY",
                  "query_memory_arena.unsafe_authority",
                  "memory accounting cannot own finality or visibility authority");
  }
  if (!SupportedFamily(request.family)) {
    return Refuse(request,
                  "SB_QUERY_MEMORY_ARENA.UNSUPPORTED_FAMILY",
                  "query_memory_arena.unsupported_family",
                  "unsupported query memory family");
  }
  if (request.bytes == 0) {
    return Refuse(request,
                  "SB_QUERY_MEMORY_ARENA.ZERO_SIZE_GRANT",
                  "query_memory_arena.zero_size_grant",
                  "zero-sized memory grants are refused");
  }
  if (request.bytes > std::numeric_limits<usize>::max()) {
    return Refuse(request,
                  "SB_QUERY_MEMORY_ARENA.GRANT_OVERFLOW",
                  "query_memory_arena.grant_overflow",
                  "requested grant does not fit allocator size",
                  StatusCode::memory_limit_exceeded);
  }
  if (request.bytes > std::numeric_limits<u64>::max() - counters_.current_bytes ||
      request.bytes > std::numeric_limits<u64>::max() - FamilyCurrent(request.family)) {
    return Refuse(request,
                  "SB_QUERY_MEMORY_ARENA.GRANT_OVERFLOW",
                  "query_memory_arena.grant_overflow",
                  "requested grant would overflow arena accounting",
                  StatusCode::memory_limit_exceeded);
  }
  if (AddWouldExceed(counters_.current_bytes, request.bytes, limits_.hard_limit_bytes)) {
    return Refuse(request,
                  "SB_QUERY_MEMORY_ARENA.HARD_LIMIT_EXCEEDED",
                  "query_memory_arena.hard_limit_exceeded",
                  "hard memory limit exceeded",
                  StatusCode::memory_limit_exceeded);
  }
  if (AddWouldExceed(counters_.current_bytes, request.bytes, limits_.query_limit_bytes)) {
    return Refuse(request,
                  "SB_QUERY_MEMORY_ARENA.QUERY_LIMIT_EXCEEDED",
                  "query_memory_arena.query_limit_exceeded",
                  "query memory limit exceeded",
                  StatusCode::memory_limit_exceeded);
  }
  if (AddWouldExceed(FamilyCurrent(request.family), request.bytes, limits_.family_limit_bytes)) {
    return Refuse(request,
                  "SB_QUERY_MEMORY_ARENA.FAMILY_LIMIT_EXCEEDED",
                  "query_memory_arena.family_limit_exceeded",
                  "family memory limit exceeded",
                  StatusCode::memory_limit_exceeded);
  }
  if (AddWouldExceed(counters_.current_bytes, request.bytes, limits_.soft_limit_bytes)) {
    if (request.spillable && limits_.allow_spill) {
      return SpillInsteadOfGrant(std::move(request), "soft memory pressure");
    }
    return Refuse(request,
                  "SB_QUERY_MEMORY_ARENA.SOFT_LIMIT_EXCEEDED",
                  "query_memory_arena.soft_limit_exceeded",
                  "soft memory limit exceeded and spill is unavailable",
                  StatusCode::memory_limit_exceeded);
  }

  auto hierarchical = ReserveHierarchicalBudget(request, "query_heap_grant");
  if (!hierarchical.ok()) {
    ++counters_.denied_count;
    QueryMemoryArenaResult result;
    result.status = hierarchical.status;
    result.fail_closed = true;
    result.diagnostic = hierarchical.diagnostic;
    result.counters = counters_;
    result.evidence = std::move(hierarchical.evidence);
    return result;
  }

  auto unified = ReserveUnifiedBudget(request, UnifiedMemorySpillBudgetKind::heap);
  if (!unified.ok()) {
    if (hierarchical.token.has_value()) {
      (void)reservation_ledger_->Release(*hierarchical.token);
    }
    auto refused = Refuse(request,
                         "SB_QUERY_MEMORY_ARENA.UNIFIED_BUDGET_DENIED",
                         "query_memory_arena.unified_budget_denied",
                         unified.diagnostic.diagnostic_code.empty()
                             ? "unified heap and spill budget denied heap grant"
                             : unified.diagnostic.diagnostic_code,
                         StatusCode::memory_limit_exceeded);
    refused.evidence.insert(refused.evidence.end(),
                            unified.evidence.begin(),
                            unified.evidence.end());
    return refused;
  }

  QueryMemoryArenaReleaseResult commit_failure;
  if (hierarchical.token.has_value() &&
      !CommitHierarchicalBudget(*hierarchical.token, &commit_failure)) {
    if (unified.reservation.has_value()) {
      (void)unified_budget_->Release(unified.reservation->reservation_id);
    }
    QueryMemoryArenaResult result;
    result.status = commit_failure.status;
    result.fail_closed = true;
    result.diagnostic = commit_failure.diagnostic;
    result.counters = counters_;
    AppendBaseEvidence(&result.evidence, request.family);
    result.evidence.insert(result.evidence.end(),
                           commit_failure.evidence.begin(),
                           commit_failure.evidence.end());
    return result;
  }

  MemoryTag tag = ArenaTag(context_, request.family, request.purpose);
  if (!heap_arena_.has_value()) {
    MemoryTag heap_tag = ArenaTag(context_, request.family, "query_memory_bump_region");
    heap_tag.callsite = "core.memory.query_memory_arena.bump_region";
    heap_arena_.emplace(allocator_, std::move(heap_tag));
  }
  AllocationResult allocated =
      heap_arena_->Allocate(static_cast<usize>(request.bytes), 0);
  if (!allocated.ok()) {
    if (hierarchical.token.has_value()) {
      ActiveGrant rollback_grant;
      rollback_grant.reservation_token = *hierarchical.token;
      ReleaseHierarchicalBudget(rollback_grant, nullptr);
    }
    if (unified.reservation.has_value()) {
      (void)unified_budget_->Release(unified.reservation->reservation_id);
    }
    ++counters_.denied_count;
    QueryMemoryArenaResult result;
    result.status = allocated.status;
    result.fail_closed = true;
    result.diagnostic = allocated.diagnostic;
    result.counters = counters_;
    AppendBaseEvidence(&result.evidence, request.family);
    result.evidence.push_back("query_memory_arena.fail_closed=true");
    result.evidence.push_back("query_memory_arena.refused=allocator_refused");
    return result;
  }

  QueryMemoryGrant grant;
  grant.grant_id = context_.query_id + ".grant." + std::to_string(next_grant_++);
  grant.family = request.family;
  grant.bytes = request.bytes;
  if (unified.reservation.has_value()) {
    grant.unified_budget_reservation_id = unified.reservation->reservation_id;
  }

  ActiveGrant active;
  active.grant = grant;
  active.pointer = allocated.pointer;
  active.alignment = allocated.alignment;
  active.tag = std::move(tag);
  active.arena_owned = true;
  if (hierarchical.token.has_value()) {
    active.reservation_token = *hierarchical.token;
  }
  active_.emplace(grant.grant_id, active);

  counters_.current_bytes += request.bytes;
  counters_.peak_bytes = std::max(counters_.peak_bytes, counters_.current_bytes);
  ++counters_.grant_count;
  ++counters_.active_grant_count;
  counters_.current_family_bytes[request.family] += request.bytes;
  counters_.peak_family_bytes[request.family] =
      std::max(counters_.peak_family_bytes[request.family],
               counters_.current_family_bytes[request.family]);
  counters_.leak_count = counters_.active_grant_count;

  QueryMemoryArenaResult result;
  result.status = OkStatus();
  result.grant = grant;
  result.counters = counters_;
  AppendBaseEvidence(&result.evidence, request.family);
  result.evidence.push_back("query_memory_arena.grant_id=" + grant.grant_id);
  result.evidence.push_back("query_memory_arena.granted_bytes=" +
                            std::to_string(grant.bytes));
  result.evidence.push_back("query_memory_arena.spilled=false");
  result.evidence.push_back("query_memory_arena.heap_backing=bump_region");
  result.evidence.push_back("query_memory_arena.heap_reset_scope=query_arena");
  result.evidence.insert(result.evidence.end(),
                         hierarchical.evidence.begin(),
                         hierarchical.evidence.end());
  result.evidence.insert(result.evidence.end(),
                         unified.evidence.begin(),
                         unified.evidence.end());
  return result;
}

QueryMemoryArenaReleaseResult QueryMemoryArena::Release(const std::string& grant_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = active_.find(grant_id);
  if (it == active_.end()) {
    return RefuseRelease("SB_QUERY_MEMORY_ARENA.UNKNOWN_GRANT",
                         "query_memory_arena.unknown_grant",
                         "release requested for unknown grant",
                         StatusCode::memory_unknown_pointer);
  }

  ActiveGrant active = it->second;
  active_.erase(it);

  QueryMemoryArenaReleaseResult result;
  result.status = OkStatus();
  AppendBaseEvidence(&result.evidence, active.grant.family);
  result.evidence.push_back("query_memory_arena.release.grant_id=" + grant_id);

  if (active.pointer != nullptr && active.arena_owned) {
    SecureZeroMemory(active.pointer, static_cast<usize>(active.grant.bytes));
    result.evidence.push_back(
        "query_memory_arena.release.heap_backing_retained_until_reset=true");
  } else if (active.pointer != nullptr) {
    DeallocationResult released = allocator_->Deallocate(active.pointer, active.tag);
    if (!released.ok()) {
      result.status = released.status;
      result.fail_closed = true;
      result.diagnostic = released.diagnostic;
    }
  }
  if (active.grant.spilled && temp_workspace_ != nullptr) {
    TempWorkspaceCleanupResult spill = ReleaseSpill(active);
    if (!spill.ok()) {
      result.status = spill.status;
      result.fail_closed = true;
      result.diagnostic = spill.diagnostic;
    }
  }
  ReleaseUnifiedBudget(active, &result.evidence);
  ReleaseHierarchicalBudget(active, &result.evidence);
  if (!HasActiveHeapGrantLocked()) {
    const auto reset = ResetHeapArenaLocked(&result.evidence);
    if (!reset.ok()) {
      result.status = reset.status;
      result.fail_closed = true;
      result.diagnostic = reset.diagnostic;
    }
  }

  counters_.current_bytes =
      active.grant.bytes >= counters_.current_bytes ? 0 : counters_.current_bytes - active.grant.bytes;
  auto family_it = counters_.current_family_bytes.find(active.grant.family);
  if (family_it != counters_.current_family_bytes.end()) {
    family_it->second =
        active.grant.bytes >= family_it->second ? 0 : family_it->second - active.grant.bytes;
    if (family_it->second == 0) {
      counters_.current_family_bytes.erase(family_it);
    }
  }
  if (counters_.active_grant_count != 0) {
    --counters_.active_grant_count;
  }
  ++counters_.release_count;
  counters_.leak_count = counters_.active_grant_count;
  result.counters = counters_;
  result.evidence.push_back("query_memory_arena.current_bytes=" +
                            std::to_string(counters_.current_bytes));
  result.evidence.push_back("query_memory_arena.leak_count=" +
                            std::to_string(counters_.leak_count));
  return result;
}

QueryMemoryArenaReleaseResult QueryMemoryArena::Cancel(std::string reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (released_ && active_.empty()) {
    return RefuseRelease("SB_QUERY_MEMORY_ARENA.CANCEL_AFTER_RELEASE",
                         "query_memory_arena.cancel_after_release",
                         "cancel requested after arena release");
  }

  QueryMemoryArenaReleaseResult result;
  result.status = OkStatus();
  AppendBaseEvidence(&result.evidence, QueryMemoryFamily::unknown);
  result.evidence.push_back("query_memory_arena.cancelled=true");
  result.evidence.push_back("query_memory_arena.cancel_reason=" + std::move(reason));
  result.evidence.push_back("query_memory_arena.transaction_finality_authority=false");
  result.evidence.push_back("query_memory_arena.visibility_authority=false");

  for (auto& entry : active_) {
    ActiveGrant& active = entry.second;
    if (active.pointer != nullptr && active.arena_owned) {
      SecureZeroMemory(active.pointer, static_cast<usize>(active.grant.bytes));
      result.evidence.push_back(
          "query_memory_arena.cancel.heap_backing_retained_until_reset=true");
      active.pointer = nullptr;
    } else if (active.pointer != nullptr) {
      DeallocationResult released = allocator_->Deallocate(active.pointer, active.tag);
      if (!released.ok()) {
        result.status = released.status;
        result.fail_closed = true;
        result.diagnostic = released.diagnostic;
      }
      active.pointer = nullptr;
    }
    if (active.grant.spilled && temp_workspace_ != nullptr) {
      TempWorkspaceCleanupResult spill = ReleaseSpill(active);
      if (!spill.ok()) {
        result.status = spill.status;
        result.fail_closed = true;
        result.diagnostic = spill.diagnostic;
      }
    }
    ReleaseUnifiedBudget(active, &result.evidence);
    ReleaseHierarchicalBudget(active, &result.evidence);
  }
  const auto reset = ResetHeapArenaLocked(&result.evidence);
  if (!reset.ok()) {
    result.status = reset.status;
    result.fail_closed = true;
    result.diagnostic = reset.diagnostic;
  }

  active_.clear();
  counters_.current_bytes = 0;
  counters_.current_family_bytes.clear();
  counters_.active_grant_count = 0;
  counters_.leak_count = 0;
  ++counters_.cancelled_count;
  released_ = true;
  result.counters = counters_;
  result.evidence.push_back("query_memory_arena.leak_count=0");
  return result;
}

QueryMemoryArenaReleaseResult QueryMemoryArena::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  QueryMemoryArenaReleaseResult result;
  result.status = OkStatus();
  AppendBaseEvidence(&result.evidence, QueryMemoryFamily::unknown);
  result.evidence.push_back("query_memory_arena.reset=true");

  for (auto& entry : active_) {
    ActiveGrant& active = entry.second;
    if (active.pointer != nullptr && active.arena_owned) {
      SecureZeroMemory(active.pointer, static_cast<usize>(active.grant.bytes));
      result.evidence.push_back(
          "query_memory_arena.reset.heap_backing_retained_until_reset=true");
      active.pointer = nullptr;
    } else if (active.pointer != nullptr) {
      DeallocationResult released = allocator_->Deallocate(active.pointer, active.tag);
      if (!released.ok()) {
        result.status = released.status;
        result.fail_closed = true;
        result.diagnostic = released.diagnostic;
      }
      active.pointer = nullptr;
    }
    if (active.grant.spilled && temp_workspace_ != nullptr) {
      TempWorkspaceCleanupResult spill = ReleaseSpill(active);
      if (!spill.ok()) {
        result.status = spill.status;
        result.fail_closed = true;
        result.diagnostic = spill.diagnostic;
      }
    }
    ReleaseUnifiedBudget(active, &result.evidence);
    ReleaseHierarchicalBudget(active, &result.evidence);
  }
  const auto reset = ResetHeapArenaLocked(&result.evidence);
  if (!reset.ok()) {
    result.status = reset.status;
    result.fail_closed = true;
    result.diagnostic = reset.diagnostic;
  }

  active_.clear();
  counters_.current_bytes = 0;
  counters_.current_family_bytes.clear();
  counters_.active_grant_count = 0;
  counters_.leak_count = 0;
  released_ = true;
  result.counters = counters_;
  result.evidence.push_back("query_memory_arena.current_bytes=0");
  result.evidence.push_back("query_memory_arena.leak_count=0");
  return result;
}

QueryMemoryArenaCounters QueryMemoryArena::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  QueryMemoryArenaCounters snapshot = counters_;
  snapshot.leak_count = snapshot.active_grant_count;
  return snapshot;
}

QueryMemoryArenaResult QueryMemoryArena::Refuse(const QueryMemoryGrantRequest& request,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                std::string reason,
                                                StatusCode code) {
  ++counters_.denied_count;
  QueryMemoryArenaResult result;
  result.status = ErrorStatus(code);
  result.fail_closed = true;
  result.counters = counters_;
  result.diagnostic = MakeArenaDiagnostic(result.status,
                                          std::move(diagnostic_code),
                                          std::move(message_key),
                                          reason,
                                          request.family,
                                          request.bytes);
  AppendBaseEvidence(&result.evidence, request.family);
  result.evidence.push_back("query_memory_arena.fail_closed=true");
  result.evidence.push_back("query_memory_arena.refused=" + std::move(reason));
  return result;
}

QueryMemoryArenaReleaseResult QueryMemoryArena::RefuseRelease(std::string diagnostic_code,
                                                              std::string message_key,
                                                              std::string reason,
                                                              StatusCode code) const {
  QueryMemoryArenaReleaseResult result;
  result.status = ErrorStatus(code);
  result.fail_closed = true;
  result.counters = counters_;
  result.diagnostic = MakeArenaDiagnostic(result.status,
                                          std::move(diagnostic_code),
                                          std::move(message_key),
                                          reason,
                                          QueryMemoryFamily::unknown,
                                          0);
  AppendBaseEvidence(&result.evidence, QueryMemoryFamily::unknown);
  result.evidence.push_back("query_memory_arena.fail_closed=true");
  result.evidence.push_back("query_memory_arena.refused=" + std::move(reason));
  return result;
}

bool QueryMemoryArena::ContextMissing() const {
  return context_.query_id.empty() || context_.statement_id.empty() ||
         context_.session_id.empty() || context_.transaction_id.empty();
}

bool QueryMemoryArena::UnsafeAuthority() const {
  return !context_.engine_mga_authoritative ||
         context_.parser_or_reference_finality_or_visibility_authority ||
         context_.client_finality_or_visibility_authority ||
         context_.provider_finality_or_visibility_authority ||
         context_.wal_recovery_or_finality_authority;
}

bool QueryMemoryArena::SupportedFamily(QueryMemoryFamily family) const {
  return QueryMemoryFamilySupported(family);
}

bool QueryMemoryArena::AddWouldExceed(u64 current, u64 add, u64 limit) const {
  if (limit == 0) {
    return false;
  }
  return add > limit || current > limit - add;
}

u64 QueryMemoryArena::FamilyCurrent(QueryMemoryFamily family) const {
  const auto it = counters_.current_family_bytes.find(family);
  if (it == counters_.current_family_bytes.end()) {
    return 0;
  }
  return it->second;
}

QueryMemoryArenaResult QueryMemoryArena::SpillInsteadOfGrant(QueryMemoryGrantRequest request,
                                                             std::string reason) {
  if (temp_workspace_ == nullptr) {
    return Refuse(request,
                  "SB_QUERY_MEMORY_ARENA.SPILL_WORKSPACE_REQUIRED",
                  "query_memory_arena.spill_workspace_required",
                  "spill workspace is required",
                  StatusCode::memory_limit_exceeded);
  }
  if (AddWouldExceed(counters_.spilled_bytes, request.bytes, limits_.spill_limit_bytes)) {
    return Refuse(request,
                  "SB_QUERY_MEMORY_ARENA.SPILL_QUOTA_DENIED",
                  "query_memory_arena.spill_quota_denied",
                  "spill quota denied",
                  StatusCode::memory_limit_exceeded);
  }
  if (request.bytes > std::numeric_limits<u64>::max() - counters_.spilled_bytes) {
    return Refuse(request,
                  "SB_QUERY_MEMORY_ARENA.GRANT_OVERFLOW",
                  "query_memory_arena.grant_overflow",
                  "requested spill would overflow arena accounting",
                  StatusCode::memory_limit_exceeded);
  }

  auto hierarchical = ReserveHierarchicalBudget(request, "query_spill_grant");
  if (!hierarchical.ok()) {
    ++counters_.denied_count;
    QueryMemoryArenaResult result;
    result.status = hierarchical.status;
    result.fail_closed = true;
    result.diagnostic = hierarchical.diagnostic;
    result.counters = counters_;
    result.evidence = std::move(hierarchical.evidence);
    return result;
  }

  auto unified = ReserveUnifiedBudget(request, UnifiedMemorySpillBudgetKind::spill);
  if (!unified.ok()) {
    if (hierarchical.token.has_value()) {
      (void)reservation_ledger_->Release(*hierarchical.token);
    }
    auto refused = Refuse(request,
                         "SB_QUERY_MEMORY_ARENA.UNIFIED_BUDGET_DENIED",
                         "query_memory_arena.unified_budget_denied",
                         unified.diagnostic.diagnostic_code.empty()
                             ? "unified heap and spill budget denied spill reservation"
                             : unified.diagnostic.diagnostic_code,
                         StatusCode::memory_limit_exceeded);
    refused.evidence.insert(refused.evidence.end(),
                            unified.evidence.begin(),
                            unified.evidence.end());
    return refused;
  }

  QueryMemoryGrant grant;
  grant.grant_id = context_.query_id + ".grant." + std::to_string(next_grant_++);
  grant.family = request.family;
  grant.bytes = 0;
  grant.spilled = true;
  grant.spill_reserved_bytes = request.bytes;
  if (unified.reservation.has_value()) {
    grant.unified_budget_reservation_id = unified.reservation->reservation_id;
  }
  grant.spill_operation_id = context_.operation_id.empty()
                                 ? grant.grant_id + ".spill"
                                 : context_.operation_id + "." + grant.grant_id + ".spill";

  TempWorkspaceAllocationRequest spill;
  spill.storage_class = TempStorageClass::spill_file;
  spill.lifetime = TempWorkspaceLifetime::operation_lifetime;
  spill.owner.temp_object_uuid = grant.grant_id;
  spill.owner.database_id = context_.database_id;
  spill.owner.engine_id = context_.engine_id;
  spill.owner.session_id = context_.session_id;
  spill.owner.transaction_id = context_.transaction_id;
  spill.owner.statement_id = context_.statement_id;
  spill.owner.operation_id = grant.spill_operation_id;
  spill.owner.resource_budget_reference = context_.query_id;
  spill.bytes = request.bytes;
  spill.purpose = request.purpose.empty() ? "query_memory_spill" : request.purpose;

  TempWorkspaceResult reserved = temp_workspace_->AllocateSpillFile(spill);
  if (!reserved.ok() || !reserved.record.has_value()) {
    if (hierarchical.token.has_value()) {
      (void)reservation_ledger_->Release(*hierarchical.token);
    }
    if (unified.reservation.has_value()) {
      (void)unified_budget_->Release(unified.reservation->reservation_id);
    }
    ++counters_.denied_count;
    QueryMemoryArenaResult result;
    result.status = ErrorStatus(StatusCode::memory_limit_exceeded);
    result.fail_closed = true;
    result.diagnostic = MakeArenaDiagnostic(
        result.status,
        "SB_QUERY_MEMORY_ARENA.SPILL_QUOTA_DENIED",
        "query_memory_arena.spill_quota_denied",
        reserved.diagnostic.diagnostic_code.empty()
            ? "spill workspace reservation was denied"
            : reserved.diagnostic.diagnostic_code,
        request.family,
        request.bytes);
    result.counters = counters_;
    AppendBaseEvidence(&result.evidence, request.family);
    result.evidence.push_back("query_memory_arena.fail_closed=true");
    result.evidence.push_back("query_memory_arena.refused=spill_quota_denied");
    if (!reserved.diagnostic.diagnostic_code.empty()) {
      result.evidence.push_back("query_memory_arena.spill_workspace_refused=" +
                                reserved.diagnostic.diagnostic_code);
    }
    return result;
  }

  QueryMemoryArenaReleaseResult commit_failure;
  if (hierarchical.token.has_value() &&
      !CommitHierarchicalBudget(*hierarchical.token, &commit_failure)) {
    (void)temp_workspace_->CleanupOperation(grant.spill_operation_id);
    if (unified.reservation.has_value()) {
      (void)unified_budget_->Release(unified.reservation->reservation_id);
    }
    QueryMemoryArenaResult result;
    result.status = commit_failure.status;
    result.fail_closed = true;
    result.diagnostic = commit_failure.diagnostic;
    result.counters = counters_;
    AppendBaseEvidence(&result.evidence, request.family);
    result.evidence.insert(result.evidence.end(),
                           commit_failure.evidence.begin(),
                           commit_failure.evidence.end());
    return result;
  }

  grant.spill_allocation_id = reserved.record->allocation_id;
  ActiveGrant active;
  active.grant = grant;
  active.tag = ArenaTag(context_, request.family, request.purpose);
  if (hierarchical.token.has_value()) {
    active.reservation_token = *hierarchical.token;
  }
  active_.emplace(grant.grant_id, active);

  ++counters_.spilled_count;
  counters_.spilled_bytes += request.bytes;
  ++counters_.grant_count;
  ++counters_.active_grant_count;
  counters_.leak_count = counters_.active_grant_count;

  QueryMemoryArenaResult result;
  result.status = OkStatus();
  result.grant = grant;
  result.counters = counters_;
  AppendBaseEvidence(&result.evidence, request.family);
  result.evidence.push_back("query_memory_arena.spilled=true");
  result.evidence.push_back("query_memory_arena.spill_reserved_bytes=" +
                            std::to_string(request.bytes));
  result.evidence.push_back("query_memory_arena.spill_reason=" + std::move(reason));
  result.evidence.push_back("query_memory_arena.spill_allocation_id=" +
                            grant.spill_allocation_id);
  result.evidence.insert(result.evidence.end(),
                         hierarchical.evidence.begin(),
                         hierarchical.evidence.end());
  result.evidence.insert(result.evidence.end(),
                         unified.evidence.begin(),
                         unified.evidence.end());
  return result;
}

TempWorkspaceCleanupResult QueryMemoryArena::ReleaseSpill(const ActiveGrant& grant) {
  return temp_workspace_->CleanupOperation(grant.grant.spill_operation_id);
}

QueryMemoryArena::HierarchicalReservationResult
QueryMemoryArena::ReserveHierarchicalBudget(QueryMemoryGrantRequest request,
                                            const char* memory_class) {
  HierarchicalReservationResult result;
  result.status = OkStatus();
  result.evidence.push_back("query_memory_arena.hierarchical_reservation_required=" +
                            BoolText(limits_.require_hierarchical_reservation));
  result.evidence.push_back("query_memory_arena.hierarchical_reservation_bound=" +
                            BoolText(reservation_ledger_ != nullptr));
  if (reservation_ledger_ == nullptr) {
    if (!limits_.require_hierarchical_reservation) {
      return result;
    }
    result.status = ErrorStatus(StatusCode::memory_invalid_request);
    result.fail_closed = true;
    result.diagnostic = MakeArenaDiagnostic(
        result.status,
        "SB_QUERY_MEMORY_ARENA.HIERARCHICAL_RESERVATION_REQUIRED",
        "query_memory_arena.hierarchical_reservation_required",
        "hierarchical memory budget ledger is required",
        request.family,
        request.bytes);
    result.evidence.push_back("query_memory_arena.fail_closed=true");
    return result;
  }

  HierarchicalMemoryReservationRequest reservation;
  reservation.scope_chain = ScopeChainForContext(context_);
  reservation.category = MemoryCategory::executor_query_reserved;
  reservation.memory_class = memory_class == nullptr ? "query_memory_grant" : memory_class;
  reservation.requested_bytes = request.bytes;
  reservation.owner_id = context_.query_id;
  reservation.spillable = request.spillable;
  reservation.cancelable = true;
  reservation.priority = 1;
  reservation.weight = 1;
  reservation.provenance = QueryArenaReservationProvenance();

  auto reserved = reservation_ledger_->Reserve(std::move(reservation));
  if (!reserved.ok()) {
    result.status = reserved.status;
    result.fail_closed = true;
    result.diagnostic = reserved.diagnostic;
    result.evidence.push_back("query_memory_arena.hierarchical_reservation_granted=false");
    return result;
  }
  result.token = reserved.token;
  result.evidence.push_back("query_memory_arena.hierarchical_reservation_granted=true");
  result.evidence.push_back("query_memory_arena.hierarchical_reservation_token=" +
                            std::to_string(reserved.token.token_id));
  result.evidence.push_back("query_memory_arena.hierarchical_reservation_bytes=" +
                            std::to_string(reserved.token.bytes));
  return result;
}

bool QueryMemoryArena::CommitHierarchicalBudget(
    const HierarchicalMemoryReservationToken& token,
    QueryMemoryArenaReleaseResult* rollback_result) {
  if (reservation_ledger_ == nullptr || !token.valid()) {
    return true;
  }
  auto committed = reservation_ledger_->Commit(token);
  if (committed.ok()) {
    if (rollback_result != nullptr) {
      rollback_result->evidence.push_back(
          "query_memory_arena.hierarchical_reservation_committed=true");
    }
    return true;
  }
  if (rollback_result != nullptr) {
    rollback_result->status = committed.status;
    rollback_result->fail_closed = true;
    rollback_result->diagnostic = committed.diagnostic;
    rollback_result->evidence.push_back(
        "query_memory_arena.hierarchical_reservation_committed=false");
  }
  (void)reservation_ledger_->Release(token);
  return false;
}

void QueryMemoryArena::ReleaseHierarchicalBudget(
    const ActiveGrant& grant,
    std::vector<std::string>* evidence) {
  if (reservation_ledger_ == nullptr || !grant.reservation_token.valid()) {
    return;
  }
  auto released = reservation_ledger_->Release(grant.reservation_token);
  if (evidence != nullptr) {
    evidence->push_back("query_memory_arena.hierarchical_reservation_released=" +
                        BoolText(released.ok()));
    evidence->push_back("query_memory_arena.hierarchical_reservation_token=" +
                        std::to_string(grant.reservation_token.token_id));
  }
}

UnifiedMemorySpillBudgetResult QueryMemoryArena::ReserveUnifiedBudget(
    QueryMemoryGrantRequest request,
    UnifiedMemorySpillBudgetKind kind) {
  UnifiedMemorySpillBudgetResult result;
  result.status = OkStatus();
  result.evidence.push_back("MMCH_UNIFIED_MEMORY_SPILL_BUDGET");
  result.evidence.push_back("unified_memory_spill.bound=" +
                            BoolText(unified_budget_ != nullptr));
  result.evidence.push_back(
      "unified_memory_spill.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority");
  if (unified_budget_ == nullptr) {
    return result;
  }
  UnifiedMemorySpillBudgetRequest budget;
  budget.operation_id = context_.operation_id.empty()
                            ? context_.query_id + "." + request.purpose
                            : context_.operation_id + "." + request.purpose;
  budget.owner_scope = context_.query_id;
  budget.kind = kind;
  budget.bytes = request.bytes;
  result = unified_budget_->Reserve(std::move(budget));
  return result;
}

void QueryMemoryArena::ReleaseUnifiedBudget(const ActiveGrant& grant,
                                            std::vector<std::string>* evidence) {
  if (unified_budget_ == nullptr ||
      grant.grant.unified_budget_reservation_id.empty()) {
    return;
  }
  auto released = unified_budget_->Release(
      grant.grant.unified_budget_reservation_id);
  evidence->insert(evidence->end(), released.evidence.begin(),
                   released.evidence.end());
}

bool QueryMemoryArena::HasActiveHeapGrantLocked() const {
  for (const auto& entry : active_) {
    const ActiveGrant& active = entry.second;
    if (active.arena_owned && active.pointer != nullptr) {
      return true;
    }
  }
  return false;
}

DeallocationResult QueryMemoryArena::ResetHeapArenaLocked(
    std::vector<std::string>* evidence) {
  DeallocationResult result;
  result.status = OkStatus();
  if (!heap_arena_.has_value()) {
    return result;
  }
  result = heap_arena_->Reset();
  if (evidence != nullptr) {
    evidence->push_back("query_memory_arena.heap_bump_region_reset=" +
                        BoolText(result.ok()));
  }
  return result;
}

void QueryMemoryArena::AppendBaseEvidence(std::vector<std::string>* evidence,
                                          QueryMemoryFamily family) const {
  evidence->push_back("query_memory_arena.family=" +
                      std::string(QueryMemoryFamilyName(family)));
  evidence->push_back("query_memory_arena.query_id=" + context_.query_id);
  evidence->push_back("query_memory_arena.statement_id=" + context_.statement_id);
  evidence->push_back("query_memory_arena.session_id=" + context_.session_id);
  evidence->push_back("query_memory_arena.transaction_context_bound=" +
                      BoolText(!context_.transaction_id.empty()));
  evidence->push_back("query_memory_arena.current_bytes=" +
                      std::to_string(counters_.current_bytes));
  evidence->push_back("query_memory_arena.peak_bytes=" +
                      std::to_string(counters_.peak_bytes));
  evidence->push_back("query_memory_arena.denied_count=" +
                      std::to_string(counters_.denied_count));
  evidence->push_back("query_memory_arena.spilled_count=" +
                      std::to_string(counters_.spilled_count));
  evidence->push_back("query_memory_arena.cancelled_count=" +
                      std::to_string(counters_.cancelled_count));
  evidence->push_back("query_memory_arena.transaction_finality_authority=false");
  evidence->push_back("query_memory_arena.visibility_authority=false");
  evidence->push_back("query_memory_arena.parser_execution_authority=false");
  evidence->push_back("query_memory_arena.recovery_authority=false");
}

DiagnosticRecord QueryMemoryArena::MakeArenaDiagnostic(Status status,
                                                       std::string diagnostic_code,
                                                       std::string message_key,
                                                       std::string reason,
                                                       QueryMemoryFamily family,
                                                       u64 requested_bytes) const {
  std::vector<DiagnosticArgument> arguments;
  arguments.push_back({"reason", std::move(reason)});
  arguments.push_back({"family", QueryMemoryFamilyName(family)});
  arguments.push_back({"query_id", context_.query_id});
  arguments.push_back({"statement_id", context_.statement_id});
  arguments.push_back({"session_id", context_.session_id});
  arguments.push_back({"transaction_context_bound", BoolText(!context_.transaction_id.empty())});
  arguments.push_back({"requested_bytes", std::to_string(requested_bytes)});
  arguments.push_back({"current_bytes", std::to_string(counters_.current_bytes)});
  arguments.push_back({"hard_limit_bytes", std::to_string(limits_.hard_limit_bytes)});
  arguments.push_back({"soft_limit_bytes", std::to_string(limits_.soft_limit_bytes)});
  arguments.push_back({"family_limit_bytes", std::to_string(limits_.family_limit_bytes)});
  arguments.push_back({"query_limit_bytes", std::to_string(limits_.query_limit_bytes)});
  arguments.push_back({"spill_limit_bytes", std::to_string(limits_.spill_limit_bytes)});
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.memory.query_memory_arena",
                        "Use bounded query memory grants, bounded spill reservations, or cancel the statement.");
}

UnifiedMemorySpillBudgetLedger::UnifiedMemorySpillBudgetLedger(
    std::string ledger_id,
    u64 limit_bytes)
    : ledger_id_(std::move(ledger_id)), limit_bytes_(limit_bytes) {}

UnifiedMemorySpillBudgetSnapshot
UnifiedMemorySpillBudgetLedger::SnapshotLocked() const {
  UnifiedMemorySpillBudgetSnapshot snapshot;
  snapshot.ledger_id = ledger_id_;
  snapshot.limit_bytes = limit_bytes_;
  snapshot.heap_bytes = heap_bytes_;
  snapshot.spill_bytes = spill_bytes_;
  snapshot.total_bytes = heap_bytes_ + spill_bytes_;
  snapshot.active_reservation_count = active_.size();
  snapshot.peak_total_bytes = peak_total_bytes_;
  snapshot.denial_count = denial_count_;
  return snapshot;
}

UnifiedMemorySpillBudgetSnapshot
UnifiedMemorySpillBudgetLedger::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return SnapshotLocked();
}

UnifiedMemorySpillBudgetResult UnifiedMemorySpillBudgetLedger::Reserve(
    UnifiedMemorySpillBudgetRequest request) {
  UnifiedMemorySpillBudgetResult result;
  result.status = OkStatus();
  result.evidence.push_back("MMCH_UNIFIED_MEMORY_SPILL_BUDGET");
  result.evidence.push_back("unified_memory_spill.ledger_id=" + ledger_id_);
  result.evidence.push_back("unified_memory_spill.operation_id=" + request.operation_id);
  result.evidence.push_back("unified_memory_spill.owner_scope=" + request.owner_scope);
  result.evidence.push_back("unified_memory_spill.kind=" +
                            std::string(UnifiedMemorySpillBudgetKindName(request.kind)));
  result.evidence.push_back("unified_memory_spill.requested_bytes=" +
                            std::to_string(request.bytes));
  result.evidence.push_back(
      "unified_memory_spill.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority");

  std::lock_guard<std::mutex> lock(mutex_);
  if (limit_bytes_ == 0 || request.operation_id.empty() ||
      request.owner_scope.empty() || request.bytes == 0) {
    ++denial_count_;
    result.status = ErrorStatus(StatusCode::memory_invalid_request);
    result.fail_closed = true;
    result.snapshot = SnapshotLocked();
    result.diagnostic = MakeDiagnostic(
        result.status,
        "SB_UNIFIED_MEMORY_SPILL_BUDGET.REQUEST_INVALID",
        "unified_memory_spill.request_invalid",
        "limit operation owner and positive bytes are required",
        request);
    result.evidence.push_back("unified_memory_spill.reservation_created=false");
    result.evidence.push_back("unified_memory_spill.fail_closed=true");
    return result;
  }
  const u64 current = heap_bytes_ + spill_bytes_;
  if (request.bytes > limit_bytes_ || current > limit_bytes_ - request.bytes) {
    ++denial_count_;
    result.status = ErrorStatus(StatusCode::memory_limit_exceeded);
    result.fail_closed = true;
    result.snapshot = SnapshotLocked();
    result.diagnostic = MakeDiagnostic(
        result.status,
        "SB_UNIFIED_MEMORY_SPILL_BUDGET.LIMIT_EXCEEDED",
        "unified_memory_spill.limit_exceeded",
        "combined heap and spill budget exceeded",
        request);
    result.evidence.push_back("unified_memory_spill.reservation_created=false");
    result.evidence.push_back("unified_memory_spill.fail_closed=true");
    result.evidence.push_back("unified_memory_spill.current_total_bytes=" +
                              std::to_string(current));
    return result;
  }

  UnifiedMemorySpillBudgetReservation reservation;
  reservation.reservation_id =
      ledger_id_ + ":" + request.operation_id + ":" +
      std::to_string(next_reservation_++);
  reservation.operation_id = std::move(request.operation_id);
  reservation.owner_scope = std::move(request.owner_scope);
  reservation.kind = request.kind;
  reservation.bytes = request.bytes;
  if (reservation.kind == UnifiedMemorySpillBudgetKind::heap) {
    heap_bytes_ += reservation.bytes;
  } else {
    spill_bytes_ += reservation.bytes;
  }
  peak_total_bytes_ = std::max(peak_total_bytes_, heap_bytes_ + spill_bytes_);
  active_[reservation.reservation_id] = ActiveReservation{reservation};
  result.reservation = reservation;
  result.reservation_created = true;
  result.snapshot = SnapshotLocked();
  result.evidence.push_back("unified_memory_spill.reservation_created=true");
  result.evidence.push_back("unified_memory_spill.reservation_id=" +
                            reservation.reservation_id);
  result.evidence.push_back("unified_memory_spill.total_bytes=" +
                            std::to_string(result.snapshot.total_bytes));
  return result;
}

UnifiedMemorySpillBudgetResult UnifiedMemorySpillBudgetLedger::Release(
    const std::string& reservation_id) {
  UnifiedMemorySpillBudgetResult result;
  result.status = OkStatus();
  result.evidence.push_back("MMCH_UNIFIED_MEMORY_SPILL_BUDGET");
  result.evidence.push_back("unified_memory_spill.ledger_id=" + ledger_id_);
  result.evidence.push_back("unified_memory_spill.reservation_id=" + reservation_id);
  result.evidence.push_back(
      "unified_memory_spill.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority");

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = active_.find(reservation_id);
  if (it == active_.end()) {
    result.status = ErrorStatus(StatusCode::memory_unknown_pointer);
    result.fail_closed = true;
    result.not_found = true;
    result.snapshot = SnapshotLocked();
    UnifiedMemorySpillBudgetRequest request;
    request.operation_id = reservation_id;
    result.diagnostic = MakeDiagnostic(result.status,
                                       "SB_UNIFIED_MEMORY_SPILL_BUDGET.NOT_FOUND",
                                       "unified_memory_spill.not_found",
                                       "reservation was not active",
                                       request);
    result.evidence.push_back("unified_memory_spill.released=false");
    return result;
  }
  result.reservation = it->second.reservation;
  if (it->second.reservation.kind == UnifiedMemorySpillBudgetKind::heap) {
    heap_bytes_ = it->second.reservation.bytes >= heap_bytes_
                      ? 0
                      : heap_bytes_ - it->second.reservation.bytes;
  } else {
    spill_bytes_ = it->second.reservation.bytes >= spill_bytes_
                       ? 0
                       : spill_bytes_ - it->second.reservation.bytes;
  }
  active_.erase(it);
  result.released = true;
  result.snapshot = SnapshotLocked();
  result.evidence.push_back("unified_memory_spill.released=true");
  result.evidence.push_back("unified_memory_spill.total_bytes=" +
                            std::to_string(result.snapshot.total_bytes));
  return result;
}

UnifiedMemorySpillBudgetResult
UnifiedMemorySpillBudgetLedger::ReleaseOwnerReservations(
    const std::string& owner_scope) {
  UnifiedMemorySpillBudgetResult result;
  result.status = OkStatus();
  result.evidence.push_back("MMCH_UNIFIED_MEMORY_SPILL_BUDGET");
  result.evidence.push_back("unified_memory_spill.ledger_id=" + ledger_id_);
  result.evidence.push_back("unified_memory_spill.owner_scope=" + owner_scope);
  result.evidence.push_back(
      "unified_memory_spill.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority");

  std::vector<std::string> reservations;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : active_) {
      if (entry.second.reservation.owner_scope == owner_scope) {
        reservations.push_back(entry.first);
      }
    }
  }
  u64 released_count = 0;
  for (const auto& reservation_id : reservations) {
    auto released = Release(reservation_id);
    if (released.ok()) {
      ++released_count;
    }
  }
  result.snapshot = Snapshot();
  result.released = released_count != 0;
  result.evidence.push_back("unified_memory_spill.owner_released_count=" +
                            std::to_string(released_count));
  return result;
}

DiagnosticRecord UnifiedMemorySpillBudgetLedger::MakeDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string reason,
    const UnifiedMemorySpillBudgetRequest& request) const {
  std::vector<DiagnosticArgument> arguments;
  arguments.push_back({"reason", std::move(reason)});
  arguments.push_back({"ledger_id", ledger_id_});
  arguments.push_back({"operation_id", request.operation_id});
  arguments.push_back({"owner_scope", request.owner_scope});
  arguments.push_back({"kind", UnifiedMemorySpillBudgetKindName(request.kind)});
  arguments.push_back({"requested_bytes", std::to_string(request.bytes)});
  arguments.push_back({"limit_bytes", std::to_string(limit_bytes_)});
  arguments.push_back({"heap_bytes", std::to_string(heap_bytes_)});
  arguments.push_back({"spill_bytes", std::to_string(spill_bytes_)});
  arguments.push_back({"authority_scope",
                       "evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority"});
  return scratchbird::core::platform::MakeDiagnostic(
      status.code,
      status.severity,
      status.subsystem,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(arguments),
      {},
      "core.memory.unified_memory_spill_budget",
      "Lower heap grants, spill less, or cancel the query.");
}

}  // namespace scratchbird::core::memory
