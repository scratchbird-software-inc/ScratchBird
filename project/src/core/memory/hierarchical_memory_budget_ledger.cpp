// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "hierarchical_memory_budget_ledger.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <set>
#include <type_traits>
#include <utility>

namespace scratchbird::core::memory {

MemoryBinaryScopeKind HierarchicalMemoryBinaryScopeKind(HierarchicalMemoryScopeKind kind) {
  switch (kind) {
    case HierarchicalMemoryScopeKind::process: return MemoryBinaryScopeKind::process;
    case HierarchicalMemoryScopeKind::database: return MemoryBinaryScopeKind::database;
    case HierarchicalMemoryScopeKind::tenant: return MemoryBinaryScopeKind::tenant;
    case HierarchicalMemoryScopeKind::user: return MemoryBinaryScopeKind::user;
    case HierarchicalMemoryScopeKind::role: return MemoryBinaryScopeKind::role;
    case HierarchicalMemoryScopeKind::session: return MemoryBinaryScopeKind::session;
    case HierarchicalMemoryScopeKind::transaction: return MemoryBinaryScopeKind::transaction;
    case HierarchicalMemoryScopeKind::statement: return MemoryBinaryScopeKind::statement;
    case HierarchicalMemoryScopeKind::query: return MemoryBinaryScopeKind::query;
    case HierarchicalMemoryScopeKind::operator_scope: return MemoryBinaryScopeKind::operator_scope;
    case HierarchicalMemoryScopeKind::page_cache: return MemoryBinaryScopeKind::page_cache;
    case HierarchicalMemoryScopeKind::background: return MemoryBinaryScopeKind::background;
    case HierarchicalMemoryScopeKind::plugin: return MemoryBinaryScopeKind::plugin;
    case HierarchicalMemoryScopeKind::connection: return MemoryBinaryScopeKind::connection;
    case HierarchicalMemoryScopeKind::cursor: return MemoryBinaryScopeKind::cursor;
    case HierarchicalMemoryScopeKind::plan_cache_entry: return MemoryBinaryScopeKind::plan_cache_entry;
    case HierarchicalMemoryScopeKind::prepared_statement: return MemoryBinaryScopeKind::prepared_statement;
    case HierarchicalMemoryScopeKind::descriptor_snapshot: return MemoryBinaryScopeKind::descriptor_snapshot;
  }
  return static_cast<MemoryBinaryScopeKind>(255);
}

namespace {

using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::StatusCode;

constexpr const char* kBudgetAuthorityScope =
    "memory_budget_evidence_only_not_transaction_finality_visibility_recovery_parser_reference_benchmark_cluster_authorization_optimizer_plan_index_finality_or_agent_action_authority";

Status BudgetStatus(StatusCode code, Severity severity) {
  return {code, severity, Subsystem::memory};
}

Status OkStatus() {
  return BudgetStatus(StatusCode::ok, Severity::info);
}

u64 StableHashString(u64 hash, const std::string& value) {
  constexpr u64 kFnvPrime = 1099511628211ull;
  for (unsigned char ch : value) {
    hash ^= static_cast<u64>(ch);
    hash *= kFnvPrime;
  }
  return hash;
}

u64 StableHashScope(const HierarchicalMemoryScopeRef& scope) {
  constexpr u64 kFnvOffset = 1469598103934665603ull;
  constexpr u64 kFnvPrime = 1099511628211ull;
  u64 hash = kFnvOffset;
  hash ^= static_cast<u64>(scope.kind);
  hash *= kFnvPrime;
  if (MemoryUuidPresent(scope.binary_scope_uuid)) {
    for (auto byte : scope.binary_scope_uuid) hash = (hash ^ byte) * kFnvPrime;
    return hash;
  }
  return StableHashString(hash, scope.scope_id);
}

ShardedMemoryScopeKey ScopeKey(const HierarchicalMemoryScopeRef& scope) {
  if (MemoryUuidPresent(scope.binary_scope_uuid))
    return MemoryBinaryScopeKey{HierarchicalMemoryBinaryScopeKind(scope.kind), scope.binary_scope_uuid};
  // Finish fallible text construction before starting variant lifetime.
  std::string prepared = std::string(HierarchicalMemoryScopeKindName(scope.kind)) + ":" + scope.scope_id;
  return ShardedMemoryScopeKey{std::move(prepared)};
}

std::string ClassKey(MemoryCategory category, const std::string& memory_class) {
  return std::string(MemoryCategoryName(category)) + ":" + memory_class;
}

DiagnosticRecord MakeBudgetDiagnostic(Status status,
                                      std::string diagnostic_code,
                                      std::string message_key,
                                      std::vector<DiagnosticArgument> arguments = {}) {
  arguments.push_back({"authority_scope", kBudgetAuthorityScope});
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.memory.hierarchical_budget_ledger",
                        "Use reservation tokens for memory evidence only; do not treat memory evidence as transaction, parser, reference, optimizer, index, or agent authority.");
}

bool ValidScope(const HierarchicalMemoryScopeRef& scope) {
  if (static_cast<unsigned>(scope.kind) > static_cast<unsigned>(HierarchicalMemoryScopeKind::descriptor_snapshot))
    return false;
  return MemoryUuidPresent(scope.binary_scope_uuid)
      ? scope.scope_id.empty() && MemorySystemUuidValid(scope.binary_scope_uuid)
      : !scope.scope_id.empty();
}

bool ValidateScopeChain(const std::vector<HierarchicalMemoryScopeRef>& chain,
                        std::string* reason,
                        std::string* duplicate_scope_key) {
  if (chain.empty()) {
    *reason = "scope_chain_empty";
    return false;
  }
  std::set<ShardedMemoryScopeKey> seen;
  const bool binary = MemoryUuidPresent(chain.front().binary_scope_uuid);
  for (const auto& scope : chain) {
    if (!ValidScope(scope) || MemoryUuidPresent(scope.binary_scope_uuid) != binary) {
      *reason = "scope_id_empty";
      return false;
    }
    auto key = ScopeKey(scope);
    if (!seen.insert(std::move(key)).second) {
      *reason = "duplicate_scope";
      *duplicate_scope_key = binary ? "binary_scope" : std::get<std::string>(ScopeKey(scope));
      return false;
    }
  }
  return true;
}

HierarchicalMemoryReservationRecommendation SoftRecommendation(
    const HierarchicalMemoryReservationRequest& request) {
  if (request.spillable) {
    return HierarchicalMemoryReservationRecommendation::spill;
  }
  if (request.cancelable) {
    return HierarchicalMemoryReservationRecommendation::cancel;
  }
  return HierarchicalMemoryReservationRecommendation::degrade;
}

bool RuntimeProvenanceSource(HierarchicalMemoryBudgetProvenanceSource source) {
  switch (source) {
    case HierarchicalMemoryBudgetProvenanceSource::runtime_policy:
    case HierarchicalMemoryBudgetProvenanceSource::server_runtime_api:
    case HierarchicalMemoryBudgetProvenanceSource::agent_runtime:
      return true;
    case HierarchicalMemoryBudgetProvenanceSource::unknown:
    case HierarchicalMemoryBudgetProvenanceSource::execution_plan_evidence:
    case HierarchicalMemoryBudgetProvenanceSource::test_fixture:
    case HierarchicalMemoryBudgetProvenanceSource::synthetic_evidence:
      break;
  }
  return false;
}

bool SafeProvenance(const HierarchicalMemoryBudgetProvenance& provenance,
                    std::string* reason) {
  if (!RuntimeProvenanceSource(provenance.source)) {
    *reason = "runtime_policy_server_api_or_agent_runtime_source_required";
    return false;
  }
  if (provenance.source_label.empty()) {
    *reason = "non_empty_runtime_source_label_required";
    return false;
  }
  if (!provenance.engine_mga_authoritative || !provenance.memory_evidence_only) {
    *reason = "engine_mga_and_memory_evidence_only_provenance_required";
    return false;
  }
  if (provenance.parser_authority || provenance.reference_authority ||
      provenance.transaction_finality_authority || provenance.visibility_authority ||
      provenance.recovery_authority || provenance.authorization_authority ||
      provenance.benchmark_authority || provenance.support_bundle_authority ||
      provenance.cluster_authority || provenance.debug_or_relaxed_path) {
    *reason = "unsafe_authority_or_relaxed_provenance_refused";
    return false;
  }
  return true;
}

ShardedMemoryAccountingEvent AccountingEventForReservation(
    const HierarchicalMemoryReservationRequest& request) {
  ShardedMemoryAccountingEvent event;
  event.bytes = request.requested_bytes;
  event.tag.purpose = "ceic_011_hierarchical_memory_budget_ledger";
  event.tag.category = request.category;
  event.tag.lifetime = MemoryLifetime::statement;
  event.tag.owner = request.owner_id;
  event.page_buffer_bytes = false;
  if (MemoryUuidPresent(request.binary_owner_uuid)) {
    event.tag.owner.clear();
    event.tag.binary_ownership[MemoryBinaryScopeKind::owner] = request.binary_owner_uuid;
    event.tag.binary_ownership[MemoryBinaryScopeKind::context] = request.scope_chain.front().binary_scope_uuid;
    event.binary_scope_ids.reserve(request.scope_chain.size());
    for (const auto& scope : request.scope_chain) {
      event.binary_scope_ids.push_back({HierarchicalMemoryBinaryScopeKind(scope.kind), scope.binary_scope_uuid});
      if (scope.kind == HierarchicalMemoryScopeKind::page_cache)
        event.page_buffer_bytes = true;
    }
    if (request.category == MemoryCategory::page_buffer) event.page_buffer_bytes = true;
    if (event.page_buffer_bytes) event.tag.lifetime = MemoryLifetime::page_buffer;
    return event;
  }
  event.scope_ids.reserve(request.scope_chain.size());
  for (const auto& scope : request.scope_chain) {
    const auto key = std::get<std::string>(ScopeKey(scope));
    if (event.tag.context_id.empty()) {
      event.tag.context_id = key;
    }
    if (scope.kind == HierarchicalMemoryScopeKind::page_cache) {
      event.page_buffer_bytes = true;
      event.tag.lifetime = MemoryLifetime::page_buffer;
    }
    if (scope.kind == HierarchicalMemoryScopeKind::database) {
      event.tag.database_id = scope.scope_id;
    } else if (scope.kind == HierarchicalMemoryScopeKind::session) {
      event.tag.session_id = scope.scope_id;
    } else if (scope.kind == HierarchicalMemoryScopeKind::transaction) {
      event.tag.transaction_id = scope.scope_id;
    } else if (scope.kind == HierarchicalMemoryScopeKind::statement) {
      event.tag.statement_id = scope.scope_id;
    } else if (scope.kind == HierarchicalMemoryScopeKind::query) {
      event.tag.query_id = scope.scope_id;
    }
    event.scope_ids.push_back(key);
  }
  if (request.category == MemoryCategory::page_buffer) {
    event.page_buffer_bytes = true;
    event.tag.lifetime = MemoryLifetime::page_buffer;
  }
  return event;
}

void AddActiveBytes(u64 bytes, HierarchicalMemoryBudgetLedger::ScopeAccounting* scope) {
  scope->active_bytes += bytes;
  scope->peak_bytes = std::max(scope->peak_bytes, scope->active_bytes);
}

void SubtractReservedBytes(u64 bytes, HierarchicalMemoryBudgetLedger::ScopeAccounting* scope) {
  scope->reserved_bytes = scope->reserved_bytes >= bytes ? scope->reserved_bytes - bytes : 0;
  if (scope->active_reservation_count != 0) {
    --scope->active_reservation_count;
  }
}

void SubtractActiveBytes(u64 bytes, HierarchicalMemoryBudgetLedger::ScopeAccounting* scope) {
  scope->active_bytes = scope->active_bytes >= bytes ? scope->active_bytes - bytes : 0;
  if (scope->active_allocation_count != 0) {
    --scope->active_allocation_count;
  }
}

u64 PriorityWeight(int priority, u64 weight) {
  return static_cast<u64>(std::max(priority, 0)) + std::max<u64>(weight, 1);
}

void SubtractPriorityWeight(u64 priority_weight,
                            HierarchicalMemoryBudgetLedger::ScopeAccounting* scope) {
  scope->priority_weight_total =
      scope->priority_weight_total >= priority_weight
          ? scope->priority_weight_total - priority_weight
          : 0;
}

}  // namespace

struct HierarchicalMemoryReservationLease::State {
  mutable std::mutex mutex;
  bool live = true;
};

HierarchicalMemoryReservationLease::UseGuard::UseGuard(std::shared_ptr<State> state)
    : state_(std::move(state)),
      lock_(state_ ? std::unique_lock<std::mutex>(state_->mutex) : std::unique_lock<std::mutex>()) {}

bool HierarchicalMemoryReservationLease::UseGuard::live() const {
  return state_ && lock_.owns_lock() && state_->live;
}

HierarchicalMemoryReservationLease::HierarchicalMemoryReservationLease(
    HierarchicalMemoryReservationLease&& other) noexcept {
  std::lock_guard lock(other.mutex_);
  ledger_ = std::exchange(other.ledger_, nullptr);
  token_ = std::exchange(other.token_, {});
  state_ = std::move(other.state_);
}

HierarchicalMemoryReservationLease& HierarchicalMemoryReservationLease::operator=(
    HierarchicalMemoryReservationLease&& other) noexcept {
  if (this != &other) {
    (void)Reset();
    std::scoped_lock lock(mutex_, other.mutex_);
    ledger_ = std::exchange(other.ledger_, nullptr);
    token_ = std::exchange(other.token_, {});
    state_ = std::move(other.state_);
  }
  return *this;
}

HierarchicalMemoryReservationLease::~HierarchicalMemoryReservationLease() {
  (void)Reset();
}

bool HierarchicalMemoryReservationLease::valid() const {
  std::lock_guard lock(mutex_);
  return ledger_ && token_.valid() && state_;
}

HierarchicalMemoryReservationLease::UseGuard HierarchicalMemoryReservationLease::Use() const {
  std::shared_ptr<State> state;
  {
    std::lock_guard lock(mutex_);
    state = state_;
  }
  return UseGuard(std::move(state));
}

bool HierarchicalMemoryReservationLease::live() const { return Use().live(); }

Status HierarchicalMemoryReservationLease::Reset() {
  std::lock_guard lock(mutex_);
  if (!ledger_) return OkStatus();
  const auto result = ledger_->ReleaseImpl(token_, false, state_.get());
  if (result.ok() || result.status.code == StatusCode::memory_unknown_pointer) {
    ledger_ = nullptr;
    token_ = {};
    state_.reset();
  }
  return result.status;
}

HierarchicalMemoryRetainResult HierarchicalMemoryBudgetLedger::Retain(
    HierarchicalMemoryReservationToken token) try {
  HierarchicalMemoryRetainResult result;
  result.status = BudgetStatus(StatusCode::memory_unknown_pointer, Severity::error);
  if (!token.valid()) return result;
  auto& shard = TokenShardForIndex(TokenShardIndex(token.token_id));
  std::lock_guard lock(shard.mutex);
  auto it = shard.tokens.find(token.token_id);
  if (it == shard.tokens.end() || it->second.token.bytes != token.bytes) return result;
  auto& record = it->second;
  if (record.state != HierarchicalMemoryReservationState::active || record.retained_owner) {
    result.status = BudgetStatus(StatusCode::memory_invalid_request, Severity::error);
    return result;
  }
  auto state = std::make_shared<HierarchicalMemoryReservationLease::State>();
  result.lease.ledger_ = this;
  result.lease.token_ = token;
  result.lease.state_ = state;
  record.retained_owner = std::move(state);
  result.status = OkStatus();
  return result;
} catch (const std::bad_alloc&) {
  HierarchicalMemoryRetainResult result;
  result.status = BudgetStatus(StatusCode::memory_allocation_failed, Severity::error);
  return result;
}


const char* HierarchicalMemoryScopeKindName(HierarchicalMemoryScopeKind kind) {
  switch (kind) {
    case HierarchicalMemoryScopeKind::process:
      return "process";
    case HierarchicalMemoryScopeKind::database:
      return "database";
    case HierarchicalMemoryScopeKind::tenant:
      return "tenant";
    case HierarchicalMemoryScopeKind::user:
      return "user";
    case HierarchicalMemoryScopeKind::role:
      return "role";
    case HierarchicalMemoryScopeKind::session:
      return "session";
    case HierarchicalMemoryScopeKind::transaction:
      return "transaction";
    case HierarchicalMemoryScopeKind::statement:
      return "statement";
    case HierarchicalMemoryScopeKind::query:
      return "query";
    case HierarchicalMemoryScopeKind::operator_scope:
      return "operator";
    case HierarchicalMemoryScopeKind::page_cache:
      return "page_cache";
    case HierarchicalMemoryScopeKind::background:
      return "background";
    case HierarchicalMemoryScopeKind::plugin:
      return "plugin";
    case HierarchicalMemoryScopeKind::connection: return "connection";
    case HierarchicalMemoryScopeKind::cursor: return "cursor";
    case HierarchicalMemoryScopeKind::plan_cache_entry: return "plan_cache_entry";
    case HierarchicalMemoryScopeKind::prepared_statement: return "prepared_statement";
    case HierarchicalMemoryScopeKind::descriptor_snapshot: return "descriptor_snapshot";
  }
  return "unknown";
}

const char* HierarchicalMemoryReservationRecommendationName(
    HierarchicalMemoryReservationRecommendation recommendation) {
  switch (recommendation) {
    case HierarchicalMemoryReservationRecommendation::granted:
      return "granted";
    case HierarchicalMemoryReservationRecommendation::deny:
      return "deny";
    case HierarchicalMemoryReservationRecommendation::spill:
      return "spill";
    case HierarchicalMemoryReservationRecommendation::cancel:
      return "cancel";
    case HierarchicalMemoryReservationRecommendation::degrade:
      return "degrade";
  }
  return "unknown";
}

const char* HierarchicalMemoryBudgetProvenanceSourceName(
    HierarchicalMemoryBudgetProvenanceSource source) {
  switch (source) {
    case HierarchicalMemoryBudgetProvenanceSource::runtime_policy:
      return "runtime_policy";
    case HierarchicalMemoryBudgetProvenanceSource::server_runtime_api:
      return "server_runtime_api";
    case HierarchicalMemoryBudgetProvenanceSource::agent_runtime:
      return "agent_runtime";
    case HierarchicalMemoryBudgetProvenanceSource::execution_plan_evidence:
      return "execution_plan_evidence";
    case HierarchicalMemoryBudgetProvenanceSource::test_fixture:
      return "test_fixture";
    case HierarchicalMemoryBudgetProvenanceSource::synthetic_evidence:
      return "synthetic_evidence";
    case HierarchicalMemoryBudgetProvenanceSource::unknown:
      break;
  }
  return "unknown";
}

HierarchicalMemoryBudgetLedger::HierarchicalMemoryBudgetLedger(usize scope_shard_count,
                                                               usize token_shard_count)
    : accounting_(token_shard_count) {
  if (scope_shard_count == 0) {
    scope_shard_count = 1;
  }
  if (token_shard_count == 0) {
    token_shard_count = 1;
  }
  scope_shards_.reserve(scope_shard_count);
  for (usize index = 0; index < scope_shard_count; ++index) {
    scope_shards_.push_back(std::make_unique<ScopeShard>());
  }
  token_shards_.reserve(token_shard_count);
  for (usize index = 0; index < token_shard_count; ++index) {
    token_shards_.push_back(std::make_unique<TokenShard>());
  }
}

HierarchicalMemoryBudgetLedger::~HierarchicalMemoryBudgetLedger() = default;

usize HierarchicalMemoryBudgetLedger::scope_shard_count() const {
  return scope_shards_.size();
}

usize HierarchicalMemoryBudgetLedger::token_shard_count() const {
  return token_shards_.size();
}

HierarchicalMemoryBudgetOperationResult HierarchicalMemoryBudgetLedger::SetBudget(
    HierarchicalMemoryBudget budget) try {
  HierarchicalMemoryBudgetOperationResult result;
  std::string provenance_reason;
  if (!SafeProvenance(budget.provenance, &provenance_reason)) {
    result.status = BudgetStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeBudgetDiagnostic(
        result.status,
        "SB-MEMORY-BUDGET-PROVENANCE-REFUSED",
        "memory.budget.provenance.refused",
        {{"scope_kind", HierarchicalMemoryScopeKindName(budget.scope.kind)},
         {"scope_id", budget.scope.scope_id},
         {"provenance_source",
          HierarchicalMemoryBudgetProvenanceSourceName(budget.provenance.source)},
         {"reason", provenance_reason}});
    return result;
  }
  if (!ValidScope(budget.scope) ||
      static_cast<unsigned>(budget.scope.kind) >
          static_cast<unsigned>(HierarchicalMemoryScopeKind::descriptor_snapshot)) {
    result.status = BudgetStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeBudgetDiagnostic(
        result.status,
        "SB-MEMORY-BUDGET-SCOPE-INVALID",
        "memory.budget.scope.invalid",
        {{"scope_kind", HierarchicalMemoryScopeKindName(budget.scope.kind)},
         {"reason", "scope_id_empty_or_kind_invalid"}});
    return result;
  }
  if (budget.hard_limit_bytes != 0 && budget.soft_limit_bytes > budget.hard_limit_bytes) {
    result.status = BudgetStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeBudgetDiagnostic(
        result.status,
        "SB-MEMORY-BUDGET-LIMITS-INVALID",
        "memory.budget.limits.invalid",
        {{"scope_kind", HierarchicalMemoryScopeKindName(budget.scope.kind)},
         {"scope_id", budget.scope.scope_id},
         {"hard_limit_bytes", std::to_string(budget.hard_limit_bytes)},
         {"soft_limit_bytes", std::to_string(budget.soft_limit_bytes)}});
    return result;
  }

  // Prepare owning metadata before inserting a scope node or changing limits.
  // Reuse existing map nodes: live reservations hold stable pointers to them.
  auto key = ScopeKey(budget.scope);
  static_assert(std::is_nothrow_move_assignable_v<std::string>);
  static_assert(std::is_nothrow_move_constructible_v<HierarchicalMemoryBudgetOperationResult>);
  const auto shard_index = ScopeShardIndex(budget.scope);
  ScopeAccounting prepared;
  prepared.kind = budget.scope.kind;
  prepared.binary_scope_uuid = budget.scope.binary_scope_uuid;
  prepared.scope_id = std::move(budget.scope.scope_id);
  prepared.hard_limit_bytes = budget.hard_limit_bytes;
  prepared.soft_limit_bytes = budget.soft_limit_bytes;
  ScopeShard& shard = ScopeShardForIndex(shard_index);
  std::lock_guard<std::mutex> lock(shard.mutex);
  const auto existing = shard.scopes.find(key);
  if (existing != shard.scopes.end()) {
    auto& scope = existing->second;
    const auto hard = prepared.hard_limit_bytes;
    if (hard != 0 && (scope.active_bytes > hard ||
                     scope.reserved_bytes > hard - scope.active_bytes)) {
      result.status = BudgetStatus(StatusCode::memory_limit_exceeded, Severity::error);
      result.diagnostic = MakeBudgetDiagnostic(result.status,
          "SB-MEMORY-BUDGET-LIVE-SHRINK-REFUSED",
          "memory.budget.live_shrink.refused",
          {{"scope_kind", HierarchicalMemoryScopeKindName(prepared.kind)},
           {"scope_id", prepared.scope_id},
           {"existing_work_rule", "reject_change"},
           {"active_bytes", std::to_string(scope.active_bytes)},
           {"reserved_bytes", std::to_string(scope.reserved_bytes)},
           {"requested_hard_limit_bytes", std::to_string(hard)}});
      return result;
    }
    // All remaining work is non-allocating. Preserve every live counter and
    // token pointer; do not replace the accounting object on policy reload.
    scope.kind = prepared.kind;
    scope.scope_id = std::move(prepared.scope_id);
    scope.binary_scope_uuid = prepared.binary_scope_uuid;
    scope.hard_limit_bytes = prepared.hard_limit_bytes;
    scope.soft_limit_bytes = prepared.soft_limit_bytes;
  } else {
    shard.scopes.emplace(std::move(key), std::move(prepared));
  }
  result.status = OkStatus();
  return result;
} catch (const std::bad_alloc&) {
  HierarchicalMemoryBudgetOperationResult result;
  result.status = BudgetStatus(StatusCode::memory_allocation_failed, Severity::error);
  result.diagnostic.status = result.status;
  return result;
}

HierarchicalMemoryReservationResult HierarchicalMemoryBudgetLedger::Reserve(
    HierarchicalMemoryReservationRequest request) try {
  HierarchicalMemoryReservationResult result;
  std::string invalid_reason;
  std::string duplicate_scope_key;
  if (request.requested_bytes == 0 ||
      (MemoryUuidPresent(request.binary_owner_uuid) &&
       (!request.owner_id.empty() || !MemorySystemUuidValid(request.binary_owner_uuid))) ||
      (!request.scope_chain.empty() &&
       MemoryUuidPresent(request.scope_chain.front().binary_scope_uuid) !=
           MemoryUuidPresent(request.binary_owner_uuid)) ||
      !ValidateScopeChain(request.scope_chain, &invalid_reason, &duplicate_scope_key)) {
    result.status = BudgetStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeBudgetDiagnostic(
        result.status,
        "SB-MEMORY-BUDGET-RESERVE-INVALID",
        "memory.budget.reserve.invalid",
        {{"requested_bytes", std::to_string(request.requested_bytes)},
         {"reason", request.requested_bytes == 0 ? "requested_bytes_zero" : invalid_reason},
         {"duplicate_scope", duplicate_scope_key}});
    return result;
  }
  std::string provenance_reason;
  if (!SafeProvenance(request.provenance, &provenance_reason)) {
    result.status = BudgetStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeBudgetDiagnostic(
        result.status,
        "SB-MEMORY-BUDGET-PROVENANCE-REFUSED",
        "memory.budget.provenance.refused",
        {{"requested_bytes", std::to_string(request.requested_bytes)},
         {"provenance_source",
          HierarchicalMemoryBudgetProvenanceSourceName(request.provenance.source)},
         {"reason", provenance_reason}});
    return result;
  }

  if (request.memory_class.empty()) {
    request.memory_class = "unclassified";
  }
  if (request.weight == 0) {
    request.weight = 1;
  }

  const auto priority = static_cast<u64>(std::max(request.priority, 0));
  if (request.weight > std::numeric_limits<u64>::max() - priority) {
    result.status = BudgetStatus(StatusCode::memory_limit_exceeded, Severity::error);
    result.diagnostic.status = result.status;
    return result;
  }
  const u64 priority_weight = priority + request.weight;
  u64 token_id = next_token_id_.load(std::memory_order_relaxed);
  while (token_id != 0 &&
         !next_token_id_.compare_exchange_weak(
             token_id, token_id == std::numeric_limits<u64>::max() ? 0 : token_id + 1,
             std::memory_order_relaxed)) {}
  if (token_id == 0) {
    result.status = BudgetStatus(StatusCode::memory_limit_exceeded, Severity::error);
    result.diagnostic.status = result.status;
    return result;
  }
  TokenShard& token_shard = TokenShardForIndex(TokenShardIndex(token_id));
  std::lock_guard<std::mutex> token_lock(token_shard.mutex);
  auto scope_locks = LockScopeShardsForChain(request.scope_chain);
  ReservationRecord record;
  record.scope_shard_indexes = ScopeShardIndexesForChain(request.scope_chain);
  record.scopes.reserve(request.scope_chain.size());

  for (const auto& scope_ref : request.scope_chain) {
    ScopeShard& scope_shard = ScopeShardForIndex(ScopeShardIndex(scope_ref));
    auto& scope = scope_shard.scopes[ScopeKey(scope_ref)];
    scope.kind = scope_ref.kind;
    scope.scope_id = scope_ref.scope_id;
    scope.binary_scope_uuid = scope_ref.binary_scope_uuid;
    record.scopes.push_back(&scope);
    const auto max = std::numeric_limits<u64>::max();
    if (scope.reserved_bytes > max - scope.active_bytes ||
        request.requested_bytes > max - scope.active_bytes - scope.reserved_bytes ||
        priority_weight > max - scope.priority_weight_total) {
      hard_limit_refusal_count_.fetch_add(1, std::memory_order_relaxed);
      result.status = BudgetStatus(StatusCode::memory_limit_exceeded, Severity::error);
      result.diagnostic.status = result.status;
      return result;
    }
    const u64 projected = scope.active_bytes + scope.reserved_bytes + request.requested_bytes;
    if (scope.hard_limit_bytes != 0 && projected > scope.hard_limit_bytes) {
      hard_limit_refusal_count_.fetch_add(1, std::memory_order_relaxed);
      result.status = BudgetStatus(StatusCode::memory_limit_exceeded, Severity::error);
      result.recommendation = HierarchicalMemoryReservationRecommendation::deny;
      result.diagnostic = MakeBudgetDiagnostic(
          result.status,
          "SB-MEMORY-BUDGET-HARD-LIMIT-REFUSED",
          "memory.budget.hard_limit.refused",
          {{"scope_kind", HierarchicalMemoryScopeKindName(scope_ref.kind)},
           {"scope_id", scope_ref.scope_id},
           {"reason", "hard_limit_exceeded"},
           {"requested_bytes", std::to_string(request.requested_bytes)},
           {"projected_bytes", std::to_string(projected)},
           {"hard_limit_bytes", std::to_string(scope.hard_limit_bytes)}});
      return result;
    }
    if (scope.soft_limit_bytes != 0 && projected > scope.soft_limit_bytes) {
      soft_limit_recommendation_count_.fetch_add(1, std::memory_order_relaxed);
      result.status = BudgetStatus(StatusCode::memory_limit_exceeded, Severity::warning);
      result.recommendation = SoftRecommendation(request);
      result.diagnostic = MakeBudgetDiagnostic(
          result.status,
          "SB-MEMORY-BUDGET-SOFT-LIMIT-RECOMMENDATION",
          "memory.budget.soft_limit.recommendation",
          {{"scope_kind", HierarchicalMemoryScopeKindName(scope_ref.kind)},
           {"scope_id", scope_ref.scope_id},
           {"reason", "soft_limit_exceeded"},
           {"recommendation", HierarchicalMemoryReservationRecommendationName(result.recommendation)},
           {"requested_bytes", std::to_string(request.requested_bytes)},
           {"projected_bytes", std::to_string(projected)},
           {"soft_limit_bytes", std::to_string(scope.soft_limit_bytes)},
           {"spillable", request.spillable ? "true" : "false"},
           {"cancelable", request.cancelable ? "true" : "false"}});
      return result;
    }
  }

  ScopeShard& class_shard = ScopeShardForIndex(ScopeShardIndex(request.scope_chain.front()));
  auto& class_accounting = class_shard.classes[ClassKey(request.category, request.memory_class)];
  class_accounting.category = request.category;
  class_accounting.memory_class = request.memory_class;
  record.class_accounting = &class_accounting;
  auto accounting_event = AccountingEventForReservation(request);
  record.token = {token_id, request.requested_bytes};
  record.scope_chain = std::move(request.scope_chain);
  record.category = request.category;
  record.memory_class = std::move(request.memory_class);
  record.owner_id = std::move(request.owner_id);
  record.binary_owner_uuid = request.binary_owner_uuid;
  record.priority = request.priority;
  record.weight = request.weight;
  record.lease_expires_at_ms = request.lease_expires_at_ms;
  auto inserted = token_shard.tokens.emplace(token_id, std::move(record));
  auto accounting_reservation = accounting_.Reserve(std::move(accounting_event));
  if (!accounting_reservation.ok()) {
    token_shard.tokens.erase(inserted.first);
    result.status = accounting_reservation.status;
    result.recommendation = HierarchicalMemoryReservationRecommendation::deny;
    result.diagnostic = std::move(accounting_reservation.diagnostic);
    return result;
  }

  // All fallible preparation completed before the actual accounting reserve.
  // The token shard remains locked until both ledgers publish the same owner.
  auto& published = inserted.first->second;
  published.accounting_token = accounting_reservation.token;
  for (auto* scope_ptr : published.scopes) {
    auto& scope = *scope_ptr;
    scope.reserved_bytes += request.requested_bytes;
    ++scope.reservation_count;
    ++scope.active_reservation_count;
    scope.priority_weight_total += priority_weight;
  }
  class_accounting.reserved_bytes += request.requested_bytes;
  ++class_accounting.reservation_count;

  global_reservation_count_.fetch_add(1, std::memory_order_relaxed);
  global_active_reservation_count_.fetch_add(1, std::memory_order_relaxed);

  result.status = OkStatus();
  result.recommendation = HierarchicalMemoryReservationRecommendation::granted;
  result.token = {token_id, request.requested_bytes};
  return result;
} catch (const std::bad_alloc&) {
  HierarchicalMemoryReservationResult result;
  result.status = BudgetStatus(StatusCode::memory_allocation_failed, Severity::error);
  result.diagnostic.status = result.status;
  return result;
}

HierarchicalMemoryBudgetOperationResult HierarchicalMemoryBudgetLedger::Commit(
    HierarchicalMemoryReservationToken token) {
  if (!token.valid()) {
    failed_commit_count_.fetch_add(1, std::memory_order_relaxed);
    return TokenFailure(StatusCode::memory_unknown_pointer,
                        "SB-MEMORY-BUDGET-COMMIT-UNKNOWN-RESERVATION",
                        "memory.budget.commit.unknown_reservation",
                        token,
                        {{"reason", "token_invalid"}});
  }

  TokenShard& token_shard = TokenShardForIndex(TokenShardIndex(token.token_id));
  std::lock_guard<std::mutex> token_lock(token_shard.mutex);
  auto it = token_shard.tokens.find(token.token_id);
  if (it == token_shard.tokens.end()) {
    failed_commit_count_.fetch_add(1, std::memory_order_relaxed);
    return TokenFailure(StatusCode::memory_unknown_pointer,
                        "SB-MEMORY-BUDGET-COMMIT-UNKNOWN-RESERVATION",
                        "memory.budget.commit.unknown_reservation",
                        token,
                        {{"reason", "token_not_found"}});
  }
  ReservationRecord& record = it->second;
  if (record.token.bytes != token.bytes || record.state != HierarchicalMemoryReservationState::reserved) {
    failed_commit_count_.fetch_add(1, std::memory_order_relaxed);
    return TokenFailure(StatusCode::memory_unknown_pointer,
                        "SB-MEMORY-BUDGET-COMMIT-STATE-INVALID",
                        "memory.budget.commit.state_invalid",
                        token,
                        {{"reason", record.token.bytes != token.bytes ? "token_bytes_mismatch" : "not_reserved"},
                         {"recorded_bytes", std::to_string(record.token.bytes)}});
  }

  ScopeLocks scope_locks(*this, record.scope_shard_indexes);
  const auto accounting_commit = accounting_.Commit(record.accounting_token);
  if (!accounting_commit.ok()) {
    failed_commit_count_.fetch_add(1, std::memory_order_relaxed);
    HierarchicalMemoryBudgetOperationResult result;
    result.status = accounting_commit.status;
    result.diagnostic = accounting_commit.diagnostic;
    return result;
  }

  for (auto* scope_ptr : record.scopes) {
    auto& scope = *scope_ptr;
    SubtractReservedBytes(record.token.bytes, &scope);
    AddActiveBytes(record.token.bytes, &scope);
    ++scope.commit_count;
    ++scope.active_allocation_count;
  }
  auto& class_accounting = *record.class_accounting;
  class_accounting.reserved_bytes =
      class_accounting.reserved_bytes >= record.token.bytes
          ? class_accounting.reserved_bytes - record.token.bytes
          : 0;
  class_accounting.active_bytes += record.token.bytes;
  class_accounting.peak_bytes = std::max(class_accounting.peak_bytes,
                                         class_accounting.active_bytes);
  ++class_accounting.commit_count;
  record.state = HierarchicalMemoryReservationState::active;
  global_active_reservation_count_.fetch_sub(1, std::memory_order_relaxed);
  global_commit_count_.fetch_add(1, std::memory_order_relaxed);
  global_active_allocation_count_.fetch_add(1, std::memory_order_relaxed);

  HierarchicalMemoryBudgetOperationResult result;
  result.status = OkStatus();
  return result;
}

HierarchicalMemoryBudgetOperationResult HierarchicalMemoryBudgetLedger::Release(
    HierarchicalMemoryReservationToken token) {
  return ReleaseImpl(token, true);
}

Status HierarchicalMemoryBudgetLedger::ReleaseNoAlloc(HierarchicalMemoryReservationToken token) {
  return ReleaseImpl(token, false).status;
}

HierarchicalMemoryBudgetOperationResult HierarchicalMemoryBudgetLedger::ReleaseImpl(
    HierarchicalMemoryReservationToken token, bool materialize_diagnostic,
    const HierarchicalMemoryReservationLease::State* retained_owner) {
  const auto no_alloc_failure = [] {
    HierarchicalMemoryBudgetOperationResult failure;
    failure.status = BudgetStatus(StatusCode::memory_unknown_pointer, Severity::error);
    return failure;
  };
  if (!token.valid()) {
    failed_release_count_.fetch_add(1, std::memory_order_relaxed);
    if (!materialize_diagnostic) return no_alloc_failure();
    return TokenFailure(StatusCode::memory_unknown_pointer,
                        "SB-MEMORY-BUDGET-RELEASE-UNKNOWN-RESERVATION",
                        "memory.budget.release.unknown_reservation",
                        token,
                        {{"reason", "token_invalid"}});
  }

  TokenShard& token_shard = TokenShardForIndex(TokenShardIndex(token.token_id));
  std::lock_guard<std::mutex> token_lock(token_shard.mutex);
  auto it = token_shard.tokens.find(token.token_id);
  if (it == token_shard.tokens.end()) {
    failed_release_count_.fetch_add(1, std::memory_order_relaxed);
    if (!materialize_diagnostic) return no_alloc_failure();
    return TokenFailure(StatusCode::memory_unknown_pointer,
                        "SB-MEMORY-BUDGET-RELEASE-UNKNOWN-RESERVATION",
                        "memory.budget.release.unknown_reservation",
                        token,
                        {{"reason", "token_not_found"}});
  }
  ReservationRecord& record = it->second;
  if (record.token.bytes != token.bytes) {
    failed_release_count_.fetch_add(1, std::memory_order_relaxed);
    if (!materialize_diagnostic) return no_alloc_failure();
    return TokenFailure(StatusCode::memory_unknown_pointer,
                        "SB-MEMORY-BUDGET-RELEASE-UNDERFLOW-REFUSED",
                        "memory.budget.release.underflow_refused",
                        token,
                        {{"reason", "token_bytes_mismatch"},
                         {"recorded_bytes", std::to_string(record.token.bytes)}});
  }

  std::unique_lock<std::mutex> owner_lock;
  if (record.retained_owner) {
    owner_lock = std::unique_lock<std::mutex>(record.retained_owner->mutex);
    if (retained_owner != record.retained_owner.get()) {
      HierarchicalMemoryBudgetOperationResult result;
      result.status = BudgetStatus(StatusCode::memory_invalid_request, Severity::error);
      result.retained = true;
      result.retained_bytes = token.bytes;
      result.newly_revoked = !record.revocation_pending;
      record.retained_owner->live = false;
      if (!record.revocation_pending) {
        record.revocation_pending = true;
        record.pending_reason = CleanupReason::release;
        pending_revocation_count_.fetch_add(1, std::memory_order_relaxed);
        retained_revoked_bytes_.fetch_add(token.bytes, std::memory_order_relaxed);
      }
      failed_release_count_.fetch_add(1, std::memory_order_relaxed);
      return result;
    }
    record.retained_owner->live = false;
  } else if (retained_owner) {
    return no_alloc_failure();
  }
  ScopeLocks scope_locks(*this, record.scope_shard_indexes);
  const auto accounting_release = materialize_diagnostic
      ? accounting_.Release(record.accounting_token)
      : ShardedMemoryAccountingOperationResult{accounting_.ReleaseNoAlloc(record.accounting_token), {}};
  if (!accounting_release.ok()) {
    failed_release_count_.fetch_add(1, std::memory_order_relaxed);
    HierarchicalMemoryBudgetOperationResult result;
    result.status = accounting_release.status;
    result.diagnostic = accounting_release.diagnostic;
    return result;
  }

  for (auto* scope_ptr : record.scopes) {
    auto& scope = *scope_ptr;
    SubtractPriorityWeight(PriorityWeight(record.priority, record.weight), &scope);
    if (record.state == HierarchicalMemoryReservationState::reserved) {
      SubtractReservedBytes(record.token.bytes, &scope);
    } else {
      SubtractActiveBytes(record.token.bytes, &scope);
    }
    switch (record.revocation_pending ? record.pending_reason : CleanupReason::release) {
      case CleanupReason::release: ++scope.release_count; break;
      case CleanupReason::cancel: ++scope.cancel_cleanup_count; break;
      case CleanupReason::owner: ++scope.owner_cleanup_count; break;
      case CleanupReason::lease_expiry: ++scope.lease_expiry_cleanup_count; break;
    }
  }
  auto& class_accounting = *record.class_accounting;
  if (record.state == HierarchicalMemoryReservationState::reserved) {
    class_accounting.reserved_bytes =
        class_accounting.reserved_bytes >= record.token.bytes
            ? class_accounting.reserved_bytes - record.token.bytes
            : 0;
  } else {
    class_accounting.active_bytes =
        class_accounting.active_bytes >= record.token.bytes
            ? class_accounting.active_bytes - record.token.bytes
            : 0;
  }
  ++class_accounting.release_count;
  if (record.state == HierarchicalMemoryReservationState::active) {
    global_active_allocation_count_.fetch_sub(1, std::memory_order_relaxed);
  } else {
    global_active_reservation_count_.fetch_sub(1, std::memory_order_relaxed);
  }
  switch (record.revocation_pending ? record.pending_reason : CleanupReason::release) {
    case CleanupReason::release: global_release_count_.fetch_add(1, std::memory_order_relaxed); break;
    case CleanupReason::cancel: global_cancel_cleanup_count_.fetch_add(1, std::memory_order_relaxed); break;
    case CleanupReason::owner: global_owner_cleanup_count_.fetch_add(1, std::memory_order_relaxed); break;
    case CleanupReason::lease_expiry: global_lease_expiry_cleanup_count_.fetch_add(1, std::memory_order_relaxed); break;
  }
  if (record.revocation_pending) {
    pending_revocation_count_.fetch_sub(1, std::memory_order_relaxed);
    retained_revoked_bytes_.fetch_sub(token.bytes, std::memory_order_relaxed);
  }
  scope_locks.Unlock();  // The index vector belongs to the token being erased.
  token_shard.tokens.erase(it);

  HierarchicalMemoryBudgetOperationResult result;
  result.status = OkStatus();
  return result;
}

HierarchicalMemoryBudgetOperationResult HierarchicalMemoryBudgetLedger::Cancel(
    HierarchicalMemoryReservationToken token) {
  if (!token.valid()) {
    failed_release_count_.fetch_add(1, std::memory_order_relaxed);
    return TokenFailure(StatusCode::memory_unknown_pointer,
                        "SB-MEMORY-BUDGET-CANCEL-UNKNOWN-RESERVATION",
                        "memory.budget.cancel.unknown_reservation",
                        token,
                        {{"reason", "token_invalid"}});
  }
  TokenShard& token_shard = TokenShardForIndex(TokenShardIndex(token.token_id));
  std::lock_guard<std::mutex> token_lock(token_shard.mutex);
  auto it = token_shard.tokens.find(token.token_id);
  if (it == token_shard.tokens.end() || it->second.token.bytes != token.bytes) {
    failed_release_count_.fetch_add(1, std::memory_order_relaxed);
    return TokenFailure(StatusCode::memory_unknown_pointer,
                        "SB-MEMORY-BUDGET-CANCEL-UNKNOWN-RESERVATION",
                        "memory.budget.cancel.unknown_reservation",
                        token,
                        {{"reason", it == token_shard.tokens.end() ? "token_not_found" : "token_bytes_mismatch"}});
  }
  return CleanupLocked(token_shard, token.token_id, CleanupReason::cancel);
}

HierarchicalMemoryCleanupResult HierarchicalMemoryBudgetLedger::CleanupOwner(std::string owner_id) {
  return CleanupOwnerImpl(owner_id, {});
}

HierarchicalMemoryCleanupResult HierarchicalMemoryBudgetLedger::CleanupOwner(const MemoryBinaryUuid& owner_uuid) {
  return CleanupOwnerImpl({}, owner_uuid);
}

HierarchicalMemoryCleanupResult HierarchicalMemoryBudgetLedger::CleanupOwnerImpl(
    std::string_view owner_id, const MemoryBinaryUuid& owner_uuid) {
  HierarchicalMemoryCleanupResult cleanup;
  cleanup.status = OkStatus();
  if (MemoryUuidPresent(owner_uuid) ? !MemorySystemUuidValid(owner_uuid) : owner_id.empty()) {
    cleanup.status = BudgetStatus(StatusCode::memory_invalid_request, Severity::error);
    cleanup.diagnostic = MakeBudgetDiagnostic(cleanup.status,
                                              "SB-MEMORY-BUDGET-OWNER-CLEANUP-INVALID",
                                              "memory.budget.owner_cleanup.invalid",
                                              {{"reason", "owner_id_empty"}});
    return cleanup;
  }

  for (auto& token_shard_ptr : token_shards_) {
    TokenShard& token_shard = *token_shard_ptr;
    std::lock_guard<std::mutex> token_lock(token_shard.mutex);
    for (auto it = token_shard.tokens.begin(); it != token_shard.tokens.end();) {
      auto current = it++;
      if (current->second.owner_id != owner_id || current->second.binary_owner_uuid != owner_uuid) continue;
      const auto bytes = current->second.token.bytes;
      auto result = CleanupLocked(token_shard, current->first, CleanupReason::owner);
      if (result.retained || result.ok()) {
        if (result.retained) {
          cleanup.status = result.status;
          cleanup.revoked_reservation_count += result.newly_revoked ? 1 : 0;
          cleanup.retained_bytes += result.retained_bytes;
        } else {
          ++cleanup.cleaned_reservation_count;
          cleanup.cleaned_bytes += bytes;
        }
      } else {
        cleanup.status = result.status;
        cleanup.diagnostic = std::move(result.diagnostic);
        return cleanup;
      }
    }
  }
  return cleanup;
}

HierarchicalMemoryCleanupResult HierarchicalMemoryBudgetLedger::CleanupExpiredLeases(u64 now_ms) {
  HierarchicalMemoryCleanupResult cleanup;
  cleanup.status = OkStatus();
  for (auto& token_shard_ptr : token_shards_) {
    TokenShard& token_shard = *token_shard_ptr;
    std::lock_guard<std::mutex> token_lock(token_shard.mutex);
    for (auto it = token_shard.tokens.begin(); it != token_shard.tokens.end();) {
      auto current = it++;
      const auto lease = current->second.lease_expires_at_ms;
      if (lease == 0 || lease > now_ms) continue;
      const auto bytes = current->second.token.bytes;
      auto result = CleanupLocked(token_shard, current->first, CleanupReason::lease_expiry);
      if (result.retained || result.ok()) {
        if (result.retained) {
          cleanup.status = result.status;
          cleanup.revoked_reservation_count += result.newly_revoked ? 1 : 0;
          cleanup.retained_bytes += result.retained_bytes;
        } else {
          ++cleanup.cleaned_reservation_count;
          cleanup.cleaned_bytes += bytes;
        }
      } else {
        cleanup.status = result.status;
        cleanup.diagnostic = std::move(result.diagnostic);
        return cleanup;
      }
    }
  }
  return cleanup;
}

HierarchicalMemoryBudgetSnapshot HierarchicalMemoryBudgetLedger::Snapshot() const {
  HierarchicalMemoryBudgetSnapshot snapshot;
  snapshot.shard_count = static_cast<u64>(scope_shards_.size());
  snapshot.token_shard_count = static_cast<u64>(token_shards_.size());
  const auto accounting_snapshot = accounting_.Snapshot();
  snapshot.reserved_bytes = accounting_snapshot.reserved_bytes;
  snapshot.current_bytes = accounting_snapshot.current_bytes;
  snapshot.active_bytes = snapshot.current_bytes;
  snapshot.peak_bytes = accounting_snapshot.peak_bytes;
  snapshot.reservation_count = global_reservation_count_.load(std::memory_order_relaxed);
  snapshot.commit_count = global_commit_count_.load(std::memory_order_relaxed);
  snapshot.release_count = global_release_count_.load(std::memory_order_relaxed);
  snapshot.cancel_cleanup_count = global_cancel_cleanup_count_.load(std::memory_order_relaxed);
  snapshot.owner_cleanup_count = global_owner_cleanup_count_.load(std::memory_order_relaxed);
  snapshot.lease_expiry_cleanup_count =
      global_lease_expiry_cleanup_count_.load(std::memory_order_relaxed);
  snapshot.active_reservation_count =
      global_active_reservation_count_.load(std::memory_order_relaxed);
  snapshot.active_allocation_count =
      global_active_allocation_count_.load(std::memory_order_relaxed);
  snapshot.pending_revocation_count = pending_revocation_count_.load(std::memory_order_relaxed);
  snapshot.retained_revoked_bytes = retained_revoked_bytes_.load(std::memory_order_relaxed);
  snapshot.hard_limit_refusal_count = hard_limit_refusal_count_.load(std::memory_order_relaxed);
  snapshot.soft_limit_recommendation_count =
      soft_limit_recommendation_count_.load(std::memory_order_relaxed);
  snapshot.failed_commit_count = failed_commit_count_.load(std::memory_order_relaxed);
  snapshot.failed_release_count = failed_release_count_.load(std::memory_order_relaxed);

  std::map<ShardedMemoryScopeKey, HierarchicalMemoryScopeSnapshot> scopes;
  std::map<std::string, HierarchicalMemoryClassSnapshot> classes;
  for (const auto& shard_ptr : scope_shards_) {
    const ScopeShard& shard = *shard_ptr;
    std::lock_guard<std::mutex> lock(shard.mutex);
    for (const auto& entry : shard.scopes) {
      const auto& source = entry.second;
      const auto scope_accounting = std::holds_alternative<std::string>(entry.first)
          ? accounting_.SnapshotForContext(std::get<std::string>(entry.first))
          : accounting_.SnapshotForContext(std::get<MemoryBinaryScopeKey>(entry.first));
      HierarchicalMemoryScopeSnapshot scope;
      scope.kind = source.kind;
      scope.scope_id = source.scope_id;
      scope.binary_scope_uuid = source.binary_scope_uuid;
      scope.hard_limit_bytes = source.hard_limit_bytes;
      scope.soft_limit_bytes = source.soft_limit_bytes;
      scope.reserved_bytes = source.reserved_bytes;
      scope.active_bytes = source.active_bytes;
      scope.current_bytes = scope_accounting.current_bytes;
      scope.peak_bytes = scope_accounting.peak_bytes;
      scope.reservation_count = source.reservation_count;
      scope.commit_count = source.commit_count;
      scope.release_count = source.release_count;
      scope.cancel_cleanup_count = source.cancel_cleanup_count;
      scope.owner_cleanup_count = source.owner_cleanup_count;
      scope.lease_expiry_cleanup_count = source.lease_expiry_cleanup_count;
      scope.active_reservation_count = source.active_reservation_count;
      scope.active_allocation_count = source.active_allocation_count;
      scope.priority_weight_total = source.priority_weight_total;
      // Avoid copying a fallible string inside a variant constructor.
      scopes.emplace(ScopeKey({source.kind, source.scope_id, source.binary_scope_uuid}), std::move(scope));

    }
    for (const auto& entry : shard.classes) {
      auto& target = classes[entry.first];
      target.category = entry.second.category;
      target.memory_class = entry.second.memory_class;
      target.reserved_bytes += entry.second.reserved_bytes;
      target.active_bytes += entry.second.active_bytes;
      target.current_bytes += entry.second.active_bytes;
      target.peak_bytes += entry.second.peak_bytes;
      target.reservation_count += entry.second.reservation_count;
      target.commit_count += entry.second.commit_count;
      target.release_count += entry.second.release_count;
    }
  }
  snapshot.scopes.reserve(scopes.size());
  for (auto& entry : scopes) {
    snapshot.scopes.push_back(std::move(entry.second));
  }
  snapshot.classes.reserve(classes.size());
  for (auto& entry : classes) {
    snapshot.classes.push_back(std::move(entry.second));
  }
  return snapshot;
}

usize HierarchicalMemoryBudgetLedger::ScopeShardIndex(const HierarchicalMemoryScopeRef& scope) const {
  if (scope_shards_.empty()) {
    return 0;
  }
  return static_cast<usize>(StableHashScope(scope) % static_cast<u64>(scope_shards_.size()));
}

usize HierarchicalMemoryBudgetLedger::TokenShardIndex(u64 token_id) const {
  if (token_shards_.empty()) {
    return 0;
  }
  return static_cast<usize>(token_id % static_cast<u64>(token_shards_.size()));
}

std::vector<usize> HierarchicalMemoryBudgetLedger::ScopeShardIndexesForChain(
    const std::vector<HierarchicalMemoryScopeRef>& chain) const {
  std::vector<usize> indexes;
  indexes.reserve(chain.size());
  for (const auto& scope : chain) {
    indexes.push_back(ScopeShardIndex(scope));
  }
  std::sort(indexes.begin(), indexes.end());
  indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
  return indexes;
}

HierarchicalMemoryBudgetLedger::ScopeLocks::ScopeLocks(
    HierarchicalMemoryBudgetLedger& ledger, const std::vector<usize>& indexes)
    : ledger_(ledger), indexes_(indexes) {
  try {
    for (; locked_ < indexes_.size(); ++locked_)
      ledger_.scope_shards_[indexes_[locked_]]->mutex.lock();
  } catch (...) {
    Unlock();
    throw;
  }
}

HierarchicalMemoryBudgetLedger::ScopeLocks::~ScopeLocks() { Unlock(); }

void HierarchicalMemoryBudgetLedger::ScopeLocks::Unlock() noexcept {
  while (locked_ != 0)
    ledger_.scope_shards_[indexes_[--locked_]]->mutex.unlock();
}

std::vector<std::unique_lock<std::mutex>> HierarchicalMemoryBudgetLedger::LockScopeShardsForChain(
    const std::vector<HierarchicalMemoryScopeRef>& chain) {
  const auto shard_indexes = ScopeShardIndexesForChain(chain);
  std::vector<std::unique_lock<std::mutex>> locks;
  locks.reserve(shard_indexes.size());
  for (usize index : shard_indexes) {
    locks.emplace_back(scope_shards_[index]->mutex);
  }
  return locks;
}

HierarchicalMemoryBudgetLedger::ScopeShard& HierarchicalMemoryBudgetLedger::ScopeShardForIndex(
    usize shard_index) {
  return *scope_shards_[shard_index];
}

const HierarchicalMemoryBudgetLedger::ScopeShard& HierarchicalMemoryBudgetLedger::ScopeShardForIndex(
    usize shard_index) const {
  return *scope_shards_[shard_index];
}

HierarchicalMemoryBudgetLedger::TokenShard& HierarchicalMemoryBudgetLedger::TokenShardForIndex(
    usize shard_index) {
  return *token_shards_[shard_index];
}

const HierarchicalMemoryBudgetLedger::TokenShard& HierarchicalMemoryBudgetLedger::TokenShardForIndex(
    usize shard_index) const {
  return *token_shards_[shard_index];
}

HierarchicalMemoryBudgetOperationResult HierarchicalMemoryBudgetLedger::CleanupLocked(
    TokenShard& token_shard,
    u64 token_id,
    CleanupReason reason) {
  auto it = token_shard.tokens.find(token_id);
  if (it == token_shard.tokens.end()) {
    return TokenFailure(StatusCode::memory_unknown_pointer,
                        "SB-MEMORY-BUDGET-CLEANUP-UNKNOWN-RESERVATION",
                        "memory.budget.cleanup.unknown_reservation",
                        {token_id, 0},
                        {{"reason", "token_not_found"}});
  }
  ReservationRecord& record = it->second;
  if (record.retained_owner) {
    std::lock_guard owner_lock(record.retained_owner->mutex);
    HierarchicalMemoryBudgetOperationResult result;
    // Revocation was requested, but cleanup is not complete until the owning
    // context has quiesced and released physical storage and this lease.
    result.status = BudgetStatus(StatusCode::memory_invalid_request, Severity::error);
    result.retained = true;
    result.retained_bytes = record.token.bytes;
    result.newly_revoked = !record.revocation_pending;
    record.retained_owner->live = false;
    if (!record.revocation_pending) {
      record.revocation_pending = true;
      record.pending_reason = reason;
      pending_revocation_count_.fetch_add(1, std::memory_order_relaxed);
      retained_revoked_bytes_.fetch_add(record.token.bytes, std::memory_order_relaxed);
    }
    return result;
  }
  ScopeLocks scope_locks(*this, record.scope_shard_indexes);
  const auto accounting_release = accounting_.Release(record.accounting_token);
  if (!accounting_release.ok()) {
    HierarchicalMemoryBudgetOperationResult result;
    result.status = accounting_release.status;
    result.diagnostic = accounting_release.diagnostic;
    return result;
  }
  for (auto* scope_ptr : record.scopes) {
    auto& scope = *scope_ptr;
    SubtractPriorityWeight(PriorityWeight(record.priority, record.weight), &scope);
    if (record.state == HierarchicalMemoryReservationState::reserved) {
      SubtractReservedBytes(record.token.bytes, &scope);
    } else {
      SubtractActiveBytes(record.token.bytes, &scope);
    }
    switch (reason) {
      case CleanupReason::release:
        ++scope.release_count;
        break;
      case CleanupReason::cancel:
        ++scope.cancel_cleanup_count;
        break;
      case CleanupReason::owner:
        ++scope.owner_cleanup_count;
        break;
      case CleanupReason::lease_expiry:
        ++scope.lease_expiry_cleanup_count;
        break;
    }

  }
  auto& class_accounting = *record.class_accounting;
  if (record.state == HierarchicalMemoryReservationState::reserved) {
    class_accounting.reserved_bytes =
        class_accounting.reserved_bytes >= record.token.bytes
            ? class_accounting.reserved_bytes - record.token.bytes
            : 0;
  } else {
    class_accounting.active_bytes =
        class_accounting.active_bytes >= record.token.bytes
            ? class_accounting.active_bytes - record.token.bytes
            : 0;
  }
  ++class_accounting.release_count;
  if (record.state == HierarchicalMemoryReservationState::active) {
    global_active_allocation_count_.fetch_sub(1, std::memory_order_relaxed);
  } else {
    global_active_reservation_count_.fetch_sub(1, std::memory_order_relaxed);
  }
  switch (reason) {
    case CleanupReason::release:
      global_release_count_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CleanupReason::cancel:
      global_cancel_cleanup_count_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CleanupReason::owner:
      global_owner_cleanup_count_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CleanupReason::lease_expiry:
      global_lease_expiry_cleanup_count_.fetch_add(1, std::memory_order_relaxed);
      break;
  }
  scope_locks.Unlock();
  token_shard.tokens.erase(it);

  HierarchicalMemoryBudgetOperationResult result;
  result.status = OkStatus();
  return result;
}

HierarchicalMemoryBudgetOperationResult HierarchicalMemoryBudgetLedger::TokenFailure(
    StatusCode code,
    std::string diagnostic_code,
    std::string message_key,
    const HierarchicalMemoryReservationToken& token,
    std::vector<DiagnosticArgument> arguments) {
  HierarchicalMemoryBudgetOperationResult result;
  result.status = BudgetStatus(code, Severity::error);
  arguments.push_back({"token_id", std::to_string(token.token_id)});
  arguments.push_back({"token_bytes", std::to_string(token.bytes)});
  result.diagnostic = MakeBudgetDiagnostic(result.status,
                                           std::move(diagnostic_code),
                                           std::move(message_key),
                                           std::move(arguments));
  return result;
}

}  // namespace scratchbird::core::memory
