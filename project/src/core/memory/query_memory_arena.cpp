// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query_memory_arena.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace scratchbird::core::memory {
namespace {

static_assert(std::is_nothrow_move_constructible_v<QueryMemoryArenaResult>);
static_assert(std::is_nothrow_move_assignable_v<QueryMemoryArenaCounters>);

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
  tag.binary_ownership[MemoryBinaryScopeKind::owner] = context.query_id.bytes;
  tag.binary_ownership[MemoryBinaryScopeKind::context] = context.statement_id.bytes;
  tag.binary_ownership[MemoryBinaryScopeKind::database] = context.database_id.bytes;
  tag.binary_ownership[MemoryBinaryScopeKind::session] = context.session_id.bytes;
  tag.binary_ownership[MemoryBinaryScopeKind::transaction] = context.transaction_id.bytes;
  tag.binary_ownership[MemoryBinaryScopeKind::statement] = context.statement_id.bytes;
  tag.binary_ownership[MemoryBinaryScopeKind::query] = context.query_id.bytes;
  (void)family;
  return tag;
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

std::vector<HierarchicalMemoryScopeRef> ScopeChainForContext(
    const QueryMemoryContext& context) {
  std::vector<HierarchicalMemoryScopeRef> chain;
  chain.push_back({HierarchicalMemoryScopeKind::process, {}, context.engine_id.bytes});
  chain.push_back({HierarchicalMemoryScopeKind::database, {}, context.database_id.bytes});
  chain.push_back({HierarchicalMemoryScopeKind::session, {}, context.session_id.bytes});
  chain.push_back({HierarchicalMemoryScopeKind::transaction, {}, context.transaction_id.bytes});
  chain.push_back({HierarchicalMemoryScopeKind::statement, {}, context.statement_id.bytes});
  chain.push_back({HierarchicalMemoryScopeKind::query, {}, context.query_id.bytes});
  chain.push_back({HierarchicalMemoryScopeKind::operator_scope, {}, context.operation_id.bytes});
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
  // Heap destruction is a final owning teardown, not the allocating public
  // cleanup/report API. Object destruction requires callers to have quiesced.
  // Erase payloads, retire actual chunks, then release their retained leases.
  for (const auto& [id, grant] : active_) {
    if (grant.arena_owned && grant.pointer != nullptr)
      SecureZeroMemory(grant.pointer, static_cast<usize>(grant.grant.bytes));
  }
  heap_arena_.reset();
  heap_capacity_.clear();
  counters_.retained_heap_bytes = 0;
  counters_.consumed_heap_bytes = 0;
  counters_.heap_chunk_count = 0;
  for (auto it = active_.begin(); it != active_.end();) {
    if (!it->second.arena_owned) { ++it; continue; }
    const auto& grant = it->second.grant;
    counters_.current_bytes -= grant.bytes;
    const auto family = counters_.current_family_bytes.find(grant.family);
    if (family != counters_.current_family_bytes.end()) {
      family->second -= grant.bytes;
      if (family->second == 0) counters_.current_family_bytes.erase(family);
    }
    --counters_.active_grant_count;
    ++counters_.release_count;
    it = active_.erase(it);
  }
  counters_.leak_count = counters_.active_grant_count;
  if (!active_.empty()) (void)Reset();
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

  const auto grant_identity = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  if (!grant_identity || active_.contains(*grant_identity))
    return Refuse(request, "SB_QUERY_MEMORY_ARENA.CONTEXT_REQUIRED",
                  "query_memory_arena.context_required", "grant identity issuance failed",
                  StatusCode::memory_allocation_failed);

  QueryMemoryArenaCounters projected = counters_;
  projected.current_bytes += request.bytes;
  projected.peak_bytes = std::max(projected.peak_bytes, projected.current_bytes);
  ++projected.grant_count;
  ++projected.active_grant_count;
  projected.current_family_bytes[request.family] += request.bytes;
  projected.peak_family_bytes[request.family] =
      std::max(projected.peak_family_bytes[request.family], projected.current_family_bytes[request.family]);
  projected.leak_count = projected.active_grant_count;

  // Preallocate the owning map node. Transferring this node into active_ after
  // allocation does not allocate; UUID comparison and ownership moves are noexcept.
  std::map<QueryMemoryUuid, ActiveGrant> staged;
  auto& active = staged[*grant_identity];
  active.grant.grant_id = *grant_identity;
  active.grant.family = request.family;
  active.grant.bytes = request.bytes;
  active.tag = ArenaTag(context_, request.family, request.purpose);
  active.arena_owned = true;

  if (!heap_arena_) {
    MemoryTag heap_tag = ArenaTag(context_, request.family, "query_memory_bump_region");
    heap_tag.callsite = "core.memory.query_memory_arena.bump_region";
    heap_arena_.emplace(allocator_, std::move(heap_tag));
  }
  const auto capacity = heap_arena_->CapacitySnapshot();
  const auto physical = allocator_->AvailableCapacity(active.tag);
  if (!physical.ok())
    return Refuse(request, "SB_QUERY_MEMORY_ARENA.ALLOCATOR_REQUIRED",
                  "query_memory_arena.allocator_required",
                  "physical allocator refused capacity planning", physical.status.code);
  u64 growth_limit = physical.available_bytes;
  const auto constrain = [&](u64 limit, u64 owned) {
    if (limit != 0) growth_limit = std::min(growth_limit, owned < limit ? limit - owned : 0);
  };
  constrain(limits_.hard_limit_bytes, capacity.retained_bytes);
  constrain(limits_.query_limit_bytes, capacity.retained_bytes);
  constrain(limits_.soft_limit_bytes, capacity.retained_bytes);
  if (unified_budget_) {
    const auto budget = unified_budget_->Snapshot();
    constrain(budget.limit_bytes, budget.total_bytes);
  }
  if (reservation_ledger_) {
    const auto budget = reservation_ledger_->Snapshot();
    const auto chain = ScopeChainForContext(context_);
    for (const auto& scope : budget.scopes) {
      const auto selected = std::find_if(chain.begin(), chain.end(), [&](const auto& owner) {
        return owner.kind == scope.kind && owner.binary_scope_uuid == scope.binary_scope_uuid &&
               owner.scope_id == scope.scope_id;
      });
      if (selected != chain.end()) constrain(scope.hard_limit_bytes, scope.current_bytes);
    }
  }
  const auto plan = heap_arena_->PlanAllocation(static_cast<usize>(request.bytes), 0,
                                               static_cast<usize>(growth_limit));
  if (!plan.ok()) {
    if (request.spillable && limits_.allow_spill)
      return SpillInsteadOfGrant(std::move(request), "retained heap capacity pressure");
    return Refuse(request, "SB_QUERY_MEMORY_ARENA.HARD_LIMIT_EXCEEDED",
                  "query_memory_arena.hard_limit_exceeded",
                  "retained heap capacity cannot admit exact growth", plan.status.code);
  }
  // A capacity owner is independent of any logical grant. The reservation
  // authorities below recheck the availability hints before physical growth.
  if (plan.growth_bytes != 0) heap_capacity_.reserve(heap_capacity_.size() + 1);
  HeapCapacityOwner capacity_owner;
  auto capacity_request = request;
  capacity_request.bytes = plan.growth_bytes;

  struct BudgetRollback {
    HierarchicalMemoryBudgetLedger* hierarchy;
    UnifiedMemorySpillBudgetLedger* unified;
    HierarchicalMemoryReservationToken token{};
    QueryMemoryUuid unified_id{};
    bool published = false;
    ~BudgetRollback() noexcept {
      if (published) return;
      try {
        if (unified != nullptr && !unified_id.is_nil()) (void)unified->ReleaseNoAlloc(unified_id);
        if (hierarchy != nullptr && token.valid()) (void)hierarchy->ReleaseNoAlloc(token);
      } catch (...) { }
    }
  } rollback{reservation_ledger_, unified_budget_};

  HierarchicalReservationResult hierarchical;
  hierarchical.status = OkStatus();
  if (plan.growth_bytes != 0)
    hierarchical = ReserveHierarchicalBudget(capacity_request, "query_heap_capacity");
  if (!hierarchical.ok()) {
    QueryMemoryArenaResult result;
    result.status = hierarchical.status;
    result.fail_closed = true;
    result.diagnostic = std::move(hierarchical.diagnostic);
    result.counters = counters_;
    result.evidence = std::move(hierarchical.evidence);
    return result;
  }
  if (hierarchical.token) rollback.token = *hierarchical.token;

  UnifiedMemorySpillBudgetResult unified;
  unified.status = OkStatus();
  if (plan.growth_bytes != 0)
    unified = ReserveUnifiedBudget(capacity_request, UnifiedMemorySpillBudgetKind::heap);
  if (!unified.ok()) {
    QueryMemoryArenaResult result;
    result.status = unified.status;
    result.fail_closed = true;
    result.diagnostic = std::move(unified.diagnostic);
    result.counters = counters_;
    result.evidence = std::move(unified.evidence);
    return result;
  }
  if (unified.reservation) {
    rollback.unified_id = unified.reservation->reservation_id;
  }

  // Stage the entire response before changing any bump cursor or allocating
  // physical backing. A failed staging allocation releases both budgets.
  QueryMemoryArenaResult result;
  result.status = OkStatus();
  result.grant = active.grant;
  result.counters = projected;
  AppendBaseEvidence(&result.evidence, request.family, &projected);
  result.evidence.push_back("query_memory_arena.granted_bytes=" + std::to_string(request.bytes));
  result.evidence.push_back("query_memory_arena.spilled=false");
  result.evidence.push_back("query_memory_arena.heap_backing=bump_region");
  result.evidence.push_back("query_memory_arena.heap_reset_scope=query_arena");
  result.evidence.insert(result.evidence.end(), hierarchical.evidence.begin(), hierarchical.evidence.end());
  result.evidence.insert(result.evidence.end(), unified.evidence.begin(), unified.evidence.end());
  if (rollback.token.valid()) {
    const auto committed = reservation_ledger_->Commit(rollback.token);
    if (!committed.ok()) {
      result.status = committed.status;
      result.fail_closed = true;
      result.grant.reset();
      result.diagnostic = committed.diagnostic;
      result.counters = counters_;
      return result;
    }
    auto retained = reservation_ledger_->Retain(rollback.token);
    if (!retained.ok()) {
      result.status = retained.status;
      result.fail_closed = true;
      result.grant.reset();
      result.counters = counters_;
      return result;
    }
    capacity_owner.hierarchy = std::move(retained.lease);
    rollback.token = {};
  }
  if (!rollback.unified_id.is_nil()) {
    auto retained = unified_budget_->Retain(rollback.unified_id);
    if (!retained.ok()) {
      result.status = retained.status;
      result.fail_closed = true;
      result.grant.reset();
      result.counters = counters_;
      return result;
    }
    capacity_owner.unified = std::move(retained.lease);
    rollback.unified_id = {};
  }

  // No ledger operations are allowed after these guards are acquired. Owner
  // cleanup may be holding the ledger lock while waiting for a use to finish.
  // Destruction releases guards before any unpublished reservation rollback.
  std::vector<HierarchicalMemoryReservationLease::UseGuard> hierarchy_uses;
  std::vector<UnifiedMemorySpillBudgetLease::UseGuard> unified_uses;
  hierarchy_uses.reserve(heap_capacity_.size() + 1);
  unified_uses.reserve(heap_capacity_.size() + 1);
  const auto protect = [&](const HeapCapacityOwner& owner) {
    bool live = true;
    if (owner.hierarchy.valid()) {
      hierarchy_uses.push_back(owner.hierarchy.Use());
      live = hierarchy_uses.back().live();
    }
    if (owner.unified.valid()) {
      unified_uses.push_back(owner.unified.Use());
      live = unified_uses.back().live() && live;
    }
    return live;
  };
  bool live = protect(capacity_owner);
  for (const auto& owner : heap_capacity_) live = protect(owner) && live;
  if (!live) {
    result.status = ErrorStatus(StatusCode::memory_invalid_request);
    result.fail_closed = true;
    result.grant.reset();
    result.counters = counters_;
    return result;
  }

  auto node = staged.extract(*grant_identity);
  const auto allocated = heap_arena_->AllocateWithinCapacity(
      static_cast<usize>(request.bytes), 0, plan.growth_bytes);
  if (!allocated.ok()) {
    result.status = allocated.status;
    result.fail_closed = true;
    result.grant.reset();
    result.evidence.clear();
    result.diagnostic = allocated.diagnostic;
    result.counters = counters_;
    return result;
  }
  node.mapped().pointer = allocated.pointer;
  node.mapped().alignment = allocated.alignment;
  if (plan.growth_bytes != 0) heap_capacity_.push_back(std::move(capacity_owner));
  const auto actual_capacity = heap_arena_->CapacitySnapshot();
  projected.retained_heap_bytes = result.counters.retained_heap_bytes = actual_capacity.retained_bytes;
  projected.consumed_heap_bytes = result.counters.consumed_heap_bytes = actual_capacity.consumed_bytes;
  projected.heap_chunk_count = result.counters.heap_chunk_count = actual_capacity.chunk_count;
  active_.insert(std::move(node));
  counters_ = std::move(projected);
  rollback.published = true;
  return result;
}

QueryMemoryArenaReleaseResult QueryMemoryArena::Release(const QueryMemoryUuid& grant_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  return ReleaseLocked(grant_id);
}

QueryMemoryArenaReleaseResult QueryMemoryArena::ReleaseLocked(const QueryMemoryUuid& grant_id) {
  const auto it = active_.find(grant_id);
  if (it == active_.end())
    return RefuseRelease("SB_QUERY_MEMORY_ARENA.UNKNOWN_GRANT",
                         "query_memory_arena.unknown_grant", "release requested for unknown grant",
                         StatusCode::memory_unknown_pointer);

  ActiveGrant& active = it->second;
  QueryMemoryArenaReleaseResult result;
  result.status = OkStatus();
  // Complete the successful counter response before irreversible cleanup.
  // In particular, a surviving family's map must not allocate after the
  // released grant has been erased and is no longer available for retry.
  auto projected = counters_;
  projected.current_bytes -= active.grant.bytes;
  auto projected_family = projected.current_family_bytes.find(active.grant.family);
  if (projected_family != projected.current_family_bytes.end()) {
    projected_family->second -= active.grant.bytes;
    if (projected_family->second == 0) projected.current_family_bytes.erase(projected_family);
  }
  if (active.grant.spilled) projected.spilled_bytes -= active.grant.spill_reserved_bytes;
  --projected.active_grant_count;
  ++projected.release_count;
  projected.leak_count = projected.active_grant_count;
  result.counters = projected;
  AppendBaseEvidence(&result.evidence, active.grant.family);
  auto failed = [&] {
    result.fail_closed = true;
    if (result.status.ok()) result.status = ErrorStatus(StatusCode::memory_allocation_failed);
    result.counters = counters_;
    return result;
  };
  // Keep the owning record and both reservations until the actual spill is
  // removed. A refused unlink must be retryable by this exact grant.
  if (active.grant.spilled) {
    if (temp_workspace_ == nullptr) return failed();
    const auto cleanup = ReleaseSpill(active);
    if (!cleanup.ok()) {
      result.status = cleanup.status;
      result.diagnostic = cleanup.diagnostic;
      return failed();
    }
    counters_.spilled_bytes -= active.grant.spill_reserved_bytes;
    active.grant.spill_reserved_bytes = 0;
    active.grant.spilled = false;
  }
  if (active.pointer != nullptr) {
    if (active.arena_owned) {
      SecureZeroMemory(active.pointer, static_cast<usize>(active.grant.bytes));
      result.evidence.push_back(
          "query_memory_arena.release.heap_backing_retained_until_reset=true");
    } else {
      const auto deallocated = allocator_->Deallocate(active.pointer, active.tag);
      if (!deallocated.ok()) {
        result.status = deallocated.status;
        result.diagnostic = deallocated.diagnostic;
        return failed();
      }
    }
    active.pointer = nullptr;
  }
  if (!HasActiveHeapGrantLocked()) {
    const auto reset = ResetHeapArenaLocked(&result.evidence);
    if (!reset.ok()) {
      result.status = reset.status;
      result.diagnostic = reset.diagnostic;
      return failed();
    }
  }
  if (!ReleaseUnifiedBudget(active, &result.evidence, &result) ||
      !ReleaseHierarchicalBudget(active, &result.evidence, &result)) return failed();

  projected.retained_heap_bytes = counters_.retained_heap_bytes;
  projected.consumed_heap_bytes = counters_.consumed_heap_bytes;
  projected.heap_chunk_count = counters_.heap_chunk_count;
  result.counters.retained_heap_bytes = projected.retained_heap_bytes;
  result.counters.consumed_heap_bytes = projected.consumed_heap_bytes;
  result.counters.heap_chunk_count = projected.heap_chunk_count;
  counters_ = std::move(projected);
  active_.erase(it);
  return result;
}

QueryMemoryArenaReleaseResult QueryMemoryArena::CleanupAllLocked() {
  QueryMemoryArenaReleaseResult result;
  result.status = OkStatus();
  // Stop new grants even if cleanup fails; Release/Reset can retry retained
  // owners. Iteration never discards records whose release did not complete.
  released_ = true;
  for (auto it = active_.begin(); it != active_.end();) {
    const auto identity = (it++)->first;
    auto released = ReleaseLocked(identity);
    if (!released.ok()) {
      result.status = released.status;
      result.diagnostic = std::move(released.diagnostic);
      result.fail_closed = true;
    }
  }
  const auto reset = ResetHeapArenaLocked(&result.evidence);
  if (!reset.ok()) {
    result.status = reset.status;
    result.diagnostic = reset.diagnostic;
    result.fail_closed = true;
  }
  result.counters = counters_;
  return result;
}

QueryMemoryArenaReleaseResult QueryMemoryArena::Cancel(std::string reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (released_ && active_.empty())
    return RefuseRelease("SB_QUERY_MEMORY_ARENA.CANCEL_AFTER_RELEASE",
                         "query_memory_arena.cancel_after_release",
                         "cancel requested after arena release");
  ++counters_.cancelled_count;
  auto result = CleanupAllLocked();
  result.evidence.push_back("query_memory_arena.cancelled=true");
  result.evidence.push_back("query_memory_arena.cancel_reason=" + std::move(reason));
  return result;
}

QueryMemoryArenaReleaseResult QueryMemoryArena::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  return CleanupAllLocked();
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
  return !QueryMemoryContextIdentitiesValid(context_);
}

bool QueryMemoryContextIdentitiesValid(const QueryMemoryContext& context) noexcept {
  for (const auto& id : {context.query_id, context.statement_id, context.session_id,
       context.transaction_id, context.database_id, context.engine_id,
       context.operation_id, context.snapshot_boundary, context.metadata_boundary,
       context.resource_budget_reference})
    if (!scratchbird::core::uuid::IsEngineIdentityUuid(id)) return false;
  return true;
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

  const auto grant_identity = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  const auto spill_identity = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  const auto spill_operation = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  if (!grant_identity || !spill_identity || !spill_operation ||
      active_.contains(*grant_identity) || *grant_identity == *spill_identity ||
      *spill_identity == *spill_operation || *grant_identity == *spill_operation)
    return Refuse(request, "SB_QUERY_MEMORY_ARENA.CONTEXT_REQUIRED",
                  "query_memory_arena.context_required", "spill identity issuance failed",
                  StatusCode::memory_allocation_failed);

  QueryMemoryArenaCounters projected = counters_;
  ++projected.spilled_count;
  projected.spilled_bytes += request.bytes;
  ++projected.grant_count;
  ++projected.active_grant_count;
  projected.leak_count = projected.active_grant_count;

  std::map<QueryMemoryUuid, ActiveGrant> staged;
  auto& active = staged[*grant_identity];
  auto& grant = active.grant;
  grant.grant_id = *grant_identity;
  grant.family = request.family;
  grant.spilled = true;
  grant.spill_reserved_bytes = request.bytes;
  grant.spill_object_id = *spill_identity;
  grant.spill_operation_id = *spill_operation;
  active.tag = ArenaTag(context_, request.family, request.purpose);

  TempWorkspaceAllocationRequest spill;
  spill.storage_class = TempStorageClass::spill_file;
  spill.lifetime = TempWorkspaceLifetime::operation_lifetime;
  spill.owner.temp_object_uuid = grant.spill_object_id;
  spill.owner.database_id = context_.database_id;
  spill.owner.engine_id = context_.engine_id;
  spill.owner.session_id = context_.session_id;
  spill.owner.transaction_id = context_.transaction_id;
  spill.owner.statement_id = context_.statement_id;
  spill.owner.operation_id = grant.spill_operation_id;
  spill.owner.resource_budget_reference = context_.resource_budget_reference;
  spill.owner.snapshot_boundary = context_.snapshot_boundary;
  spill.owner.metadata_boundary = context_.metadata_boundary;
  spill.owner.policy_generation = context_.policy_generation;
  spill.owner.security_generation = context_.security_generation;
  spill.bytes = request.bytes;
  spill.purpose = request.purpose.empty() ? "query_memory_spill" : request.purpose;

  struct BudgetRollback {
    HierarchicalMemoryBudgetLedger* hierarchy;
    UnifiedMemorySpillBudgetLedger* unified;
    HierarchicalMemoryReservationToken token{};
    QueryMemoryUuid unified_id{};
    bool published = false;
    ~BudgetRollback() {
      if (published) return;
      if (unified != nullptr && !unified_id.is_nil()) (void)unified->ReleaseNoAlloc(unified_id);
      if (hierarchy != nullptr && token.valid()) (void)hierarchy->ReleaseNoAlloc(token);
    }
  } rollback{reservation_ledger_, unified_budget_};

  auto hierarchical = ReserveHierarchicalBudget(request, "query_spill_grant");
  if (!hierarchical.ok()) {
    QueryMemoryArenaResult result;
    result.status = hierarchical.status;
    result.fail_closed = true;
    result.diagnostic = std::move(hierarchical.diagnostic);
    result.counters = counters_;
    result.evidence = std::move(hierarchical.evidence);
    return result;
  }
  if (hierarchical.token) rollback.token = active.reservation_token = *hierarchical.token;

  auto unified = ReserveUnifiedBudget(request, UnifiedMemorySpillBudgetKind::spill);
  if (!unified.ok()) {
    QueryMemoryArenaResult result;
    result.status = unified.status;
    result.fail_closed = true;
    result.diagnostic = std::move(unified.diagnostic);
    result.counters = counters_;
    result.evidence = std::move(unified.evidence);
    return result;
  }
  if (unified.reservation) {
    rollback.unified_id = unified.reservation->reservation_id;
    grant.unified_budget_reservation_id = rollback.unified_id;
  }

  // Query ownership is the binary temp-object identity, not its OS filename.
  // All caller metadata and the map node are complete before file publication.
  QueryMemoryArenaResult result;
  result.status = OkStatus();
  result.grant = grant;
  result.counters = projected;
  AppendBaseEvidence(&result.evidence, request.family, &projected);
  result.evidence.push_back("query_memory_arena.spilled=true");
  result.evidence.push_back("query_memory_arena.spill_reserved_bytes=" +
                            std::to_string(request.bytes));
  result.evidence.push_back("query_memory_arena.spill_reason=" + std::move(reason));
  result.evidence.insert(result.evidence.end(), hierarchical.evidence.begin(), hierarchical.evidence.end());
  result.evidence.insert(result.evidence.end(), unified.evidence.begin(), unified.evidence.end());

  if (rollback.token.valid()) {
    const auto committed = reservation_ledger_->Commit(rollback.token);
    if (!committed.ok()) {
      result.status = committed.status;
      result.fail_closed = true;
      result.grant.reset();
      result.diagnostic = committed.diagnostic;
      result.counters = counters_;
      return result;
    }
  }
  auto node = staged.extract(*grant_identity);
  auto reserved = temp_workspace_->AllocateSpillFile(std::move(spill));
  if (!reserved.ok() || !reserved.record) {
    result.status = reserved.ok() ? ErrorStatus(StatusCode::memory_allocation_failed) : reserved.status;
    result.fail_closed = true;
    result.diagnostic = std::move(reserved.diagnostic);
    result.evidence.clear();
    if (!reserved.record) {
      result.grant.reset();
      result.counters = counters_;
      return result;
    }
    // A post-rename OS sync failure can leave a real retained temp owner.
    // Publish its exact query cleanup owner too, but never report success.
  }
  active_.insert(std::move(node));
  counters_ = std::move(projected);
  rollback.published = true;
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
  reservation.binary_owner_uuid = context_.query_id.bytes;
  reservation.spillable = request.spillable;
  reservation.cancelable = true;
  reservation.priority = 1;
  reservation.weight = 1;
  reservation.provenance = QueryArenaReservationProvenance();

  result.evidence.reserve(result.evidence.size() + 1);
  std::string granted_evidence = "query_memory_arena.hierarchical_reservation_granted=true";
  auto reserved = reservation_ledger_->Reserve(std::move(reservation));
  if (!reserved.ok()) {
    result.status = reserved.status;
    result.fail_closed = true;
    result.diagnostic = reserved.diagnostic;
    result.evidence.push_back("query_memory_arena.hierarchical_reservation_granted=false");
    return result;
  }
  result.token = reserved.token;
  result.evidence.push_back(std::move(granted_evidence));
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

bool QueryMemoryArena::ReleaseHierarchicalBudget(
    ActiveGrant& grant, std::vector<std::string>* evidence,
    QueryMemoryArenaReleaseResult* failure) {
  if (!grant.reservation_token.valid()) return true;
  if (reservation_ledger_ == nullptr) return false;
  const auto released = reservation_ledger_->Release(grant.reservation_token);
  // Remember completed cleanup before optional evidence allocation can fail.
  if (released.ok()) grant.reservation_token = {};
  if (evidence != nullptr)
    evidence->push_back("query_memory_arena.hierarchical_reservation_released=" +
                        BoolText(released.ok()));
  if (!released.ok()) {
    if (failure != nullptr) {
      failure->status = released.status;
      failure->diagnostic = released.diagnostic;
      failure->fail_closed = true;
    }
    return false;
  }
  return true;
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
  budget.operation_id = context_.operation_id;
  budget.owner_scope = context_.query_id;
  budget.kind = kind;
  budget.bytes = request.bytes;
  result = unified_budget_->Reserve(std::move(budget));
  return result;
}

bool QueryMemoryArena::ReleaseUnifiedBudget(
    ActiveGrant& grant, std::vector<std::string>* evidence,
    QueryMemoryArenaReleaseResult* failure) {
  if (grant.grant.unified_budget_reservation_id.is_nil()) return true;
  if (unified_budget_ == nullptr) return false;
  const auto released = unified_budget_->Release(grant.grant.unified_budget_reservation_id);
  if (released.ok()) grant.grant.unified_budget_reservation_id = {};
  if (evidence != nullptr)
    evidence->insert(evidence->end(), released.evidence.begin(), released.evidence.end());
  if (!released.ok()) {
    if (failure != nullptr) {
      failure->status = released.status;
      failure->diagnostic = released.diagnostic;
      failure->fail_closed = true;
    }
    return false;
  }
  return true;
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
  if (!heap_arena_.has_value() || HasActiveHeapGrantLocked()) {
    return result;
  }
  result = heap_arena_->Reset();
  const auto capacity = heap_arena_->CapacitySnapshot();
  counters_.retained_heap_bytes = capacity.retained_bytes;
  counters_.consumed_heap_bytes = capacity.consumed_bytes;
  counters_.heap_chunk_count = capacity.chunk_count;
  // Arena reset retires chunks from the back. Keep each still-owned chunk's
  // leases on a partial failure and release only physically retired owners.
  while (heap_capacity_.size() > capacity.chunk_count) heap_capacity_.pop_back();
  if (evidence != nullptr) {
    evidence->push_back("query_memory_arena.heap_bump_region_reset=" +
                        BoolText(result.ok()));
  }
  return result;
}

void QueryMemoryArena::AppendBaseEvidence(std::vector<std::string>* evidence,
                                          QueryMemoryFamily family,
                                          const QueryMemoryArenaCounters* snapshot) const {
  const auto& counters = snapshot == nullptr ? counters_ : *snapshot;
  evidence->push_back("query_memory_arena.family=" +
                      std::string(QueryMemoryFamilyName(family)));
  evidence->push_back("query_memory_arena.transaction_context_bound=" +
                      BoolText(!context_.transaction_id.is_nil()));
  evidence->push_back("query_memory_arena.current_bytes=" +
                      std::to_string(counters.current_bytes));
  evidence->push_back("query_memory_arena.peak_bytes=" +
                      std::to_string(counters.peak_bytes));
  evidence->push_back("query_memory_arena.denied_count=" +
                      std::to_string(counters.denied_count));
  evidence->push_back("query_memory_arena.spilled_count=" +
                      std::to_string(counters.spilled_count));
  evidence->push_back("query_memory_arena.cancelled_count=" +
                      std::to_string(counters.cancelled_count));
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
  arguments.push_back({"query_id_present", BoolText(!context_.query_id.is_nil())});
  arguments.push_back({"statement_id_present", BoolText(!context_.statement_id.is_nil())});
  arguments.push_back({"session_id_present", BoolText(!context_.session_id.is_nil())});
  arguments.push_back({"transaction_context_bound", BoolText(!context_.transaction_id.is_nil())});
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


}  // namespace scratchbird::core::memory
