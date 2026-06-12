// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "hierarchical_memory_budget_ledger.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace scratchbird::core::memory {
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
  return StableHashString(hash, scope.scope_id);
}

std::string ScopeKey(const HierarchicalMemoryScopeRef& scope) {
  return std::string(HierarchicalMemoryScopeKindName(scope.kind)) + ":" + scope.scope_id;
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
  return !scope.scope_id.empty();
}

bool ValidateScopeChain(const std::vector<HierarchicalMemoryScopeRef>& chain,
                        std::string* reason,
                        std::string* duplicate_scope_key) {
  if (chain.empty()) {
    *reason = "scope_chain_empty";
    return false;
  }
  std::set<std::string> seen;
  for (const auto& scope : chain) {
    if (!ValidScope(scope)) {
      *reason = "scope_id_empty";
      return false;
    }
    const auto key = ScopeKey(scope);
    if (!seen.insert(key).second) {
      *reason = "duplicate_scope";
      *duplicate_scope_key = key;
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
  event.scope_ids.reserve(request.scope_chain.size());
  for (const auto& scope : request.scope_chain) {
    const auto key = ScopeKey(scope);
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

void UpdateAtomicPeak(std::atomic<u64>* peak, u64 value) {
  u64 observed = peak->load(std::memory_order_relaxed);
  while (observed < value &&
         !peak->compare_exchange_weak(observed, value, std::memory_order_relaxed)) {
  }
}

}  // namespace

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
    HierarchicalMemoryBudget budget) {
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
  if (!ValidScope(budget.scope)) {
    result.status = BudgetStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeBudgetDiagnostic(
        result.status,
        "SB-MEMORY-BUDGET-SCOPE-INVALID",
        "memory.budget.scope.invalid",
        {{"scope_kind", HierarchicalMemoryScopeKindName(budget.scope.kind)},
         {"reason", "scope_id_empty"}});
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

  ScopeShard& shard = ScopeShardForIndex(ScopeShardIndex(budget.scope));
  std::lock_guard<std::mutex> lock(shard.mutex);
  auto& scope = shard.scopes[ScopeKey(budget.scope)];
  scope.kind = budget.scope.kind;
  scope.scope_id = budget.scope.scope_id;
  scope.hard_limit_bytes = budget.hard_limit_bytes;
  scope.soft_limit_bytes = budget.soft_limit_bytes;
  result.status = OkStatus();
  return result;
}

HierarchicalMemoryReservationResult HierarchicalMemoryBudgetLedger::Reserve(
    HierarchicalMemoryReservationRequest request) {
  HierarchicalMemoryReservationResult result;
  std::string invalid_reason;
  std::string duplicate_scope_key;
  if (request.requested_bytes == 0 ||
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

  const u64 token_id = next_token_id_.fetch_add(1, std::memory_order_relaxed);
  TokenShard& token_shard = TokenShardForIndex(TokenShardIndex(token_id));
  std::lock_guard<std::mutex> token_lock(token_shard.mutex);
  auto scope_locks = LockScopeShardsForChain(request.scope_chain);

  for (const auto& scope_ref : request.scope_chain) {
    ScopeShard& scope_shard = ScopeShardForIndex(ScopeShardIndex(scope_ref));
    auto& scope = scope_shard.scopes[ScopeKey(scope_ref)];
    scope.kind = scope_ref.kind;
    scope.scope_id = scope_ref.scope_id;
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

  auto accounting_reservation =
      accounting_.Reserve(AccountingEventForReservation(request));
  if (!accounting_reservation.ok()) {
    result.status = accounting_reservation.status;
    result.recommendation = HierarchicalMemoryReservationRecommendation::deny;
    result.diagnostic = accounting_reservation.diagnostic;
    return result;
  }

  const u64 priority_weight = static_cast<u64>(std::max(request.priority, 0)) + request.weight;
  for (const auto& scope_ref : request.scope_chain) {
    ScopeShard& scope_shard = ScopeShardForIndex(ScopeShardIndex(scope_ref));
    auto& scope = scope_shard.scopes[ScopeKey(scope_ref)];
    scope.reserved_bytes += request.requested_bytes;
    ++scope.reservation_count;
    ++scope.active_reservation_count;
    scope.priority_weight_total += priority_weight;
  }
  ScopeShard& class_shard = ScopeShardForIndex(ScopeShardIndex(request.scope_chain.front()));
  auto& class_accounting = class_shard.classes[ClassKey(request.category, request.memory_class)];
  class_accounting.category = request.category;
  class_accounting.memory_class = request.memory_class;
  class_accounting.reserved_bytes += request.requested_bytes;
  ++class_accounting.reservation_count;

  ReservationRecord record;
  record.token = {token_id, request.requested_bytes};
  record.scope_chain = std::move(request.scope_chain);
  record.category = request.category;
  record.memory_class = std::move(request.memory_class);
  record.owner_id = std::move(request.owner_id);
  record.accounting_token = accounting_reservation.token;
  record.priority = request.priority;
  record.weight = request.weight;
  record.lease_expires_at_ms = request.lease_expires_at_ms;
  token_shard.tokens[token_id] = std::move(record);
  global_reserved_bytes_.fetch_add(request.requested_bytes, std::memory_order_relaxed);
  global_reservation_count_.fetch_add(1, std::memory_order_relaxed);
  global_active_reservation_count_.fetch_add(1, std::memory_order_relaxed);

  result.status = OkStatus();
  result.recommendation = HierarchicalMemoryReservationRecommendation::granted;
  result.token = {token_id, request.requested_bytes};
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

  auto scope_locks = LockScopeShardsForChain(record.scope_chain);
  const auto accounting_commit = accounting_.Commit(record.accounting_token);
  if (!accounting_commit.ok()) {
    failed_commit_count_.fetch_add(1, std::memory_order_relaxed);
    HierarchicalMemoryBudgetOperationResult result;
    result.status = accounting_commit.status;
    result.diagnostic = accounting_commit.diagnostic;
    return result;
  }

  for (const auto& scope_ref : record.scope_chain) {
    ScopeShard& scope_shard = ScopeShardForIndex(ScopeShardIndex(scope_ref));
    auto& scope = scope_shard.scopes[ScopeKey(scope_ref)];
    SubtractReservedBytes(record.token.bytes, &scope);
    AddActiveBytes(record.token.bytes, &scope);
    ++scope.commit_count;
    ++scope.active_allocation_count;
  }
  ScopeShard& class_shard = ScopeShardForIndex(ScopeShardIndex(record.scope_chain.front()));
  auto& class_accounting = class_shard.classes[ClassKey(record.category, record.memory_class)];
  class_accounting.reserved_bytes =
      class_accounting.reserved_bytes >= record.token.bytes
          ? class_accounting.reserved_bytes - record.token.bytes
          : 0;
  class_accounting.active_bytes += record.token.bytes;
  class_accounting.peak_bytes = std::max(class_accounting.peak_bytes,
                                         class_accounting.active_bytes);
  ++class_accounting.commit_count;
  record.state = HierarchicalMemoryReservationState::active;
  global_reserved_bytes_.fetch_sub(record.token.bytes, std::memory_order_relaxed);
  global_active_reservation_count_.fetch_sub(1, std::memory_order_relaxed);
  global_commit_count_.fetch_add(1, std::memory_order_relaxed);
  global_active_allocation_count_.fetch_add(1, std::memory_order_relaxed);
  const u64 global_current =
      global_current_bytes_.fetch_add(record.token.bytes, std::memory_order_relaxed) +
      record.token.bytes;
  UpdateAtomicPeak(&global_peak_bytes_, global_current);

  HierarchicalMemoryBudgetOperationResult result;
  result.status = OkStatus();
  return result;
}

HierarchicalMemoryBudgetOperationResult HierarchicalMemoryBudgetLedger::Release(
    HierarchicalMemoryReservationToken token) {
  if (!token.valid()) {
    failed_release_count_.fetch_add(1, std::memory_order_relaxed);
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
    return TokenFailure(StatusCode::memory_unknown_pointer,
                        "SB-MEMORY-BUDGET-RELEASE-UNKNOWN-RESERVATION",
                        "memory.budget.release.unknown_reservation",
                        token,
                        {{"reason", "token_not_found"}});
  }
  ReservationRecord& record = it->second;
  if (record.token.bytes != token.bytes) {
    failed_release_count_.fetch_add(1, std::memory_order_relaxed);
    return TokenFailure(StatusCode::memory_unknown_pointer,
                        "SB-MEMORY-BUDGET-RELEASE-UNDERFLOW-REFUSED",
                        "memory.budget.release.underflow_refused",
                        token,
                        {{"reason", "token_bytes_mismatch"},
                         {"recorded_bytes", std::to_string(record.token.bytes)}});
  }

  auto scope_locks = LockScopeShardsForChain(record.scope_chain);
  const auto accounting_release = accounting_.Release(record.accounting_token);
  if (!accounting_release.ok()) {
    failed_release_count_.fetch_add(1, std::memory_order_relaxed);
    HierarchicalMemoryBudgetOperationResult result;
    result.status = accounting_release.status;
    result.diagnostic = accounting_release.diagnostic;
    return result;
  }

  for (const auto& scope_ref : record.scope_chain) {
    ScopeShard& scope_shard = ScopeShardForIndex(ScopeShardIndex(scope_ref));
    auto& scope = scope_shard.scopes[ScopeKey(scope_ref)];
    SubtractPriorityWeight(PriorityWeight(record.priority, record.weight), &scope);
    if (record.state == HierarchicalMemoryReservationState::reserved) {
      SubtractReservedBytes(record.token.bytes, &scope);
    } else {
      SubtractActiveBytes(record.token.bytes, &scope);
    }
    ++scope.release_count;
  }
  ScopeShard& class_shard = ScopeShardForIndex(ScopeShardIndex(record.scope_chain.front()));
  auto& class_accounting = class_shard.classes[ClassKey(record.category, record.memory_class)];
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
    global_current_bytes_.fetch_sub(record.token.bytes, std::memory_order_relaxed);
    global_active_allocation_count_.fetch_sub(1, std::memory_order_relaxed);
  } else {
    global_reserved_bytes_.fetch_sub(record.token.bytes, std::memory_order_relaxed);
    global_active_reservation_count_.fetch_sub(1, std::memory_order_relaxed);
  }
  global_release_count_.fetch_add(1, std::memory_order_relaxed);
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
  HierarchicalMemoryCleanupResult cleanup;
  cleanup.status = OkStatus();
  if (owner_id.empty()) {
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
    std::vector<u64> token_ids;
    for (const auto& entry : token_shard.tokens) {
      if (entry.second.owner_id == owner_id) {
        token_ids.push_back(entry.first);
      }
    }
    std::sort(token_ids.begin(), token_ids.end());
    for (u64 token_id : token_ids) {
      const auto bytes = token_shard.tokens[token_id].token.bytes;
      const auto result = CleanupLocked(token_shard, token_id, CleanupReason::owner);
      if (result.ok()) {
        ++cleanup.cleaned_reservation_count;
        cleanup.cleaned_bytes += bytes;
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
    std::vector<u64> token_ids;
    for (const auto& entry : token_shard.tokens) {
      const u64 lease = entry.second.lease_expires_at_ms;
      if (lease != 0 && lease <= now_ms) {
        token_ids.push_back(entry.first);
      }
    }
    std::sort(token_ids.begin(), token_ids.end());
    for (u64 token_id : token_ids) {
      const auto bytes = token_shard.tokens[token_id].token.bytes;
      const auto result = CleanupLocked(token_shard, token_id, CleanupReason::lease_expiry);
      if (result.ok()) {
        ++cleanup.cleaned_reservation_count;
        cleanup.cleaned_bytes += bytes;
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
  snapshot.hard_limit_refusal_count = hard_limit_refusal_count_.load(std::memory_order_relaxed);
  snapshot.soft_limit_recommendation_count =
      soft_limit_recommendation_count_.load(std::memory_order_relaxed);
  snapshot.failed_commit_count = failed_commit_count_.load(std::memory_order_relaxed);
  snapshot.failed_release_count = failed_release_count_.load(std::memory_order_relaxed);

  std::map<std::string, HierarchicalMemoryScopeSnapshot> scopes;
  std::map<std::string, HierarchicalMemoryClassSnapshot> classes;
  for (const auto& shard_ptr : scope_shards_) {
    const ScopeShard& shard = *shard_ptr;
    std::lock_guard<std::mutex> lock(shard.mutex);
    for (const auto& entry : shard.scopes) {
      const auto& source = entry.second;
      const auto scope_accounting = accounting_.SnapshotForContext(entry.first);
      HierarchicalMemoryScopeSnapshot scope;
      scope.kind = source.kind;
      scope.scope_id = source.scope_id;
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
      scopes[entry.first] = std::move(scope);

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
  ReservationRecord record = it->second;
  auto scope_locks = LockScopeShardsForChain(record.scope_chain);
  const auto accounting_release = accounting_.Release(record.accounting_token);
  if (!accounting_release.ok()) {
    HierarchicalMemoryBudgetOperationResult result;
    result.status = accounting_release.status;
    result.diagnostic = accounting_release.diagnostic;
    return result;
  }
  for (const auto& scope_ref : record.scope_chain) {
    ScopeShard& scope_shard = ScopeShardForIndex(ScopeShardIndex(scope_ref));
    auto& scope = scope_shard.scopes[ScopeKey(scope_ref)];
    SubtractPriorityWeight(PriorityWeight(record.priority, record.weight), &scope);
    if (record.state == HierarchicalMemoryReservationState::reserved) {
      SubtractReservedBytes(record.token.bytes, &scope);
    } else {
      SubtractActiveBytes(record.token.bytes, &scope);
    }
    switch (reason) {
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
  ScopeShard& class_shard = ScopeShardForIndex(ScopeShardIndex(record.scope_chain.front()));
  auto& class_accounting = class_shard.classes[ClassKey(record.category, record.memory_class)];
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
    global_current_bytes_.fetch_sub(record.token.bytes, std::memory_order_relaxed);
    global_active_allocation_count_.fetch_sub(1, std::memory_order_relaxed);
  } else {
    global_reserved_bytes_.fetch_sub(record.token.bytes, std::memory_order_relaxed);
    global_active_reservation_count_.fetch_sub(1, std::memory_order_relaxed);
  }
  switch (reason) {
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
