// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sharded_memory_accounting_ledger.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::StatusCode;

constexpr const char* kLedgerAuthorityScope =
    "memory_accounting_event_only_not_transaction_finality_visibility_recovery_parser_reference_benchmark_or_support_bundle_authority";

Status LedgerStatus(StatusCode code, Severity severity) {
  return {code, severity, Subsystem::memory};
}

Status OkStatus() {
  return LedgerStatus(StatusCode::ok, Severity::info);
}

ShardedMemoryScopeKey TextScopeKey(const std::string& text) {
  // Finish fallible payload construction before starting variant lifetime.
  // Map insertion then moves a fully formed key without copying its payload.
  std::string prepared = text;
  static_assert(std::is_nothrow_move_constructible_v<ShardedMemoryScopeKey>);
  return ShardedMemoryScopeKey{std::move(prepared)};
}

ShardedMemoryScopeKey CopyScopeKey(const ShardedMemoryScopeKey& key) {
  if (const auto* text = std::get_if<std::string>(&key)) return TextScopeKey(*text);
  return std::get<MemoryBinaryScopeKey>(key);
}

template <class Map>
typename Map::mapped_type& PrepareScope(Map& map, const ShardedMemoryScopeKey& key) {
  const auto found = map.find(key);
  if (found != map.end()) return found->second;
  return map.try_emplace(CopyScopeKey(key)).first->second;
}

u64 StableHashString(u64 hash, const std::string& value) {
  constexpr u64 kFnvPrime = 1099511628211ull;
  for (unsigned char ch : value) {
    hash ^= static_cast<u64>(ch);
    hash *= kFnvPrime;
  }
  return hash;
}

u64 StableHashEvent(const ShardedMemoryAccountingEvent& event) {
  constexpr u64 kFnvOffset = 1469598103934665603ull;
  constexpr u64 kFnvPrime = 1099511628211ull;
  u64 hash = kFnvOffset;
  if (!event.tag.binary_ownership.empty()) {
    for (usize i = 0; i < event.tag.binary_ownership.scopes.size(); ++i)
      if (MemoryUuidPresent(event.tag.binary_ownership.scopes[i]))
        return ShardedMemoryScopeHash{}(MemoryBinaryScopeKey{
            static_cast<MemoryBinaryScopeKind>(i), event.tag.binary_ownership.scopes[i]});
  }
  if (!event.tag.context_id.empty()) {
    hash = StableHashString(hash, event.tag.context_id);
    return hash;
  }
  if (!event.scope_ids.empty()) {
    hash = StableHashString(hash, event.scope_ids.front());
    return hash;
  }
  if (!event.tag.owner.empty()) {
    hash = StableHashString(hash, event.tag.owner);
    return hash;
  }
  hash ^= static_cast<u64>(event.tag.category);
  hash *= kFnvPrime;
  hash = StableHashString(hash, event.tag.database_id);
  hash = StableHashString(hash, event.tag.session_id);
  hash ^= event.page_buffer_bytes ? 0xa5a5a5a5a5a5a5a5ull : 0x5a5a5a5a5a5a5a5aull;
  hash *= kFnvPrime;
  return hash;
}

DiagnosticRecord MakeLedgerDiagnostic(Status status,
                                      std::string diagnostic_code,
                                      std::string message_key,
                                      std::vector<DiagnosticArgument> arguments = {}) {
  arguments.push_back({"authority_scope", kLedgerAuthorityScope});
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.memory.sharded_accounting_ledger",
                        "Use the returned reservation token exactly once through reserve, commit, and release.");
}

bool IsPageBufferAccounting(const ShardedMemoryAccountingEvent& event) {
  return event.page_buffer_bytes ||
         event.tag.category == MemoryCategory::page_buffer ||
         event.tag.lifetime == MemoryLifetime::page_buffer;
}

bool IsPageBufferAccounting(const ShardedMemoryAccountingLedger::TokenRecord& record) {
  return record.page_buffer_bytes ||
         record.tag.category == MemoryCategory::page_buffer ||
         record.tag.lifetime == MemoryLifetime::page_buffer;
}

std::vector<ShardedMemoryScopeKey> AccountingScopeIds(const ShardedMemoryAccountingEvent& event) {
  std::vector<ShardedMemoryScopeKey> scopes;
  if (!event.tag.binary_ownership.empty()) {
    for (usize i = 0; i < event.tag.binary_ownership.scopes.size(); ++i) {
      const auto& uuid = event.tag.binary_ownership.scopes[i];
      if (MemoryUuidPresent(uuid))
        scopes.emplace_back(MemoryBinaryScopeKey{static_cast<MemoryBinaryScopeKind>(i), uuid});
    }
    for (const auto& binary : event.binary_scope_ids) {
      ShardedMemoryScopeKey key = binary;
      if (std::find(scopes.begin(), scopes.end(), key) == scopes.end())
        scopes.push_back(std::move(key));
    }
    return scopes;
  }
  if (!event.tag.context_id.empty()) scopes.push_back(TextScopeKey(event.tag.context_id));
  for (const auto& id : event.scope_ids) {
    auto key = TextScopeKey(id);
    if (!id.empty() && std::find(scopes.begin(), scopes.end(), key) == scopes.end())
      scopes.push_back(std::move(key));
  }
  return scopes;
}

void AddBytes(u64 bytes, u64* current, u64* peak) {
  *current += bytes;
  *peak = std::max(*peak, *current);
}

void UpdateAtomicPeak(std::atomic<u64>* peak, u64 value) {
  u64 observed = peak->load(std::memory_order_relaxed);
  while (observed < value &&
         !peak->compare_exchange_weak(observed, value, std::memory_order_relaxed)) {
  }
}

void CommitCategory(u64 bytes, ShardedMemoryAccountingLedger::CategoryAccounting* category) {
  AddBytes(bytes, &category->current_bytes, &category->peak_bytes);
  ++category->allocation_count;
  ++category->active_allocation_count;
}

void ReleaseCategory(u64 bytes, ShardedMemoryAccountingLedger::CategoryAccounting* category) {
  if (category->current_bytes >= bytes) {
    category->current_bytes -= bytes;
  } else {
    category->current_bytes = 0;
  }
  if (category->active_allocation_count != 0) {
    --category->active_allocation_count;
  }
  ++category->release_count;
}

void CommitScope(u64 bytes, ShardedMemoryAccountingLedger::ScopeAccounting* scope) {
  AddBytes(bytes, &scope->current_bytes, &scope->peak_bytes);
  ++scope->allocation_count;
  ++scope->active_allocation_count;
}

void ReleaseScope(u64 bytes, ShardedMemoryAccountingLedger::ScopeAccounting* scope) {
  if (scope->current_bytes >= bytes) {
    scope->current_bytes -= bytes;
  } else {
    scope->current_bytes = 0;
  }
  if (scope->active_allocation_count != 0) {
    --scope->active_allocation_count;
  }
  ++scope->release_count;
}

void MergeScope(const ShardedMemoryScopeKey& scope_id,
                const ShardedMemoryAccountingLedger::ScopeAccounting& source,
                std::map<ShardedMemoryScopeKey, ShardedMemoryAccountingLedger::ScopeAccounting>* target) {
  auto& merged = PrepareScope(*target, scope_id);
  merged.current_bytes += source.current_bytes;
  merged.peak_bytes += source.peak_bytes;
  merged.allocation_count += source.allocation_count;
  merged.release_count += source.release_count;
  merged.active_allocation_count += source.active_allocation_count;
}

void MergeCategory(MemoryCategory category,
                   const ShardedMemoryAccountingLedger::CategoryAccounting& source,
                   std::map<MemoryCategory, ShardedMemoryAccountingLedger::CategoryAccounting>* target) {
  auto& merged = (*target)[category];
  merged.current_bytes += source.current_bytes;
  merged.peak_bytes += source.peak_bytes;
  merged.allocation_count += source.allocation_count;
  merged.release_count += source.release_count;
  merged.active_allocation_count += source.active_allocation_count;
}

std::vector<ShardedMemoryAccountingScopeSnapshot> ScopeSnapshots(
    const std::map<ShardedMemoryScopeKey, ShardedMemoryAccountingLedger::ScopeAccounting>& scopes) {
  std::vector<ShardedMemoryAccountingScopeSnapshot> snapshots;
  snapshots.reserve(scopes.size());
  for (const auto& entry : scopes) {
    ShardedMemoryAccountingScopeSnapshot snapshot;
    if (const auto* binary = std::get_if<MemoryBinaryScopeKey>(&entry.first))
      snapshot.binary_scope = *binary;
    else snapshot.scope_id = std::get<std::string>(entry.first);
    snapshot.current_bytes = entry.second.current_bytes;
    snapshot.peak_bytes = entry.second.peak_bytes;
    snapshot.allocation_count = entry.second.allocation_count;
    snapshot.release_count = entry.second.release_count;
    snapshot.active_allocation_count = entry.second.active_allocation_count;
    snapshots.push_back(std::move(snapshot));
  }
  return snapshots;
}

std::vector<ShardedMemoryAccountingCategorySnapshot> CategorySnapshots(
    const std::map<MemoryCategory, ShardedMemoryAccountingLedger::CategoryAccounting>& categories) {
  std::vector<ShardedMemoryAccountingCategorySnapshot> snapshots;
  snapshots.reserve(categories.size());
  for (const auto& entry : categories) {
    ShardedMemoryAccountingCategorySnapshot snapshot;
    snapshot.category = entry.first;
    snapshot.current_bytes = entry.second.current_bytes;
    snapshot.peak_bytes = entry.second.peak_bytes;
    snapshot.allocation_count = entry.second.allocation_count;
    snapshot.release_count = entry.second.release_count;
    snapshot.active_allocation_count = entry.second.active_allocation_count;
    snapshots.push_back(snapshot);
  }
  return snapshots;
}

}  // namespace

ShardedMemoryAccountingLedger::ShardedMemoryAccountingLedger(usize shard_count) {
  if (shard_count == 0) {
    shard_count = 1;
  }
  shards_.reserve(shard_count);
  for (usize index = 0; index < shard_count; ++index) {
    shards_.push_back(std::make_unique<Shard>());
  }
}

ShardedMemoryAccountingLedger::~ShardedMemoryAccountingLedger() = default;

usize ShardedMemoryAccountingLedger::shard_count() const {
  return shards_.size();
}

usize ShardedMemoryAccountingLedger::ShardIndexForEvent(const ShardedMemoryAccountingEvent& event) const {
  if (shards_.empty()) {
    return 0;
  }
  return static_cast<usize>(StableHashEvent(event) % static_cast<u64>(shards_.size()));
}

ShardedMemoryAccountingResult ShardedMemoryAccountingLedger::Reserve(ShardedMemoryAccountingEvent event) try {
  ShardedMemoryAccountingResult result;
  if (!MemoryBinaryOwnershipValid(event.tag) ||
      (!event.tag.binary_ownership.empty() && !event.scope_ids.empty()) ||
      (!event.binary_scope_ids.empty() && event.tag.binary_ownership.empty()) ||
      std::any_of(event.binary_scope_ids.begin(), event.binary_scope_ids.end(),
          [](const auto& key) { return !MemorySystemUuidValid(key.uuid) ||
              static_cast<unsigned>(key.kind) > static_cast<unsigned>(MemoryBinaryScopeKind::descriptor_snapshot); })) {
    result.status = LedgerStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic.status = result.status;
    return result;
  }
  if (event.bytes == 0) {
    result.status = LedgerStatus(StatusCode::memory_invalid_request, Severity::error);
    result.diagnostic = MakeLedgerDiagnostic(
        result.status,
        "SB-MEMORY-LEDGER-RESERVE-ZERO-BYTES",
        "memory.ledger.reserve.zero_bytes",
        {{"context_id", event.tag.context_id},
         {"owner", event.tag.owner},
         {"category", MemoryCategoryName(event.tag.category)}});
    return result;
  }

  const usize shard_index = ShardIndexForEvent(event);
  Shard& shard = ShardForIndex(shard_index);
  u64 token_id = next_token_id_.load(std::memory_order_relaxed);
  while (token_id != 0 &&
         !next_token_id_.compare_exchange_weak(
             token_id, token_id == std::numeric_limits<u64>::max() ? 0 : token_id + 1,
             std::memory_order_relaxed)) {}
  if (token_id == 0) {
    result.status = LedgerStatus(StatusCode::memory_limit_exceeded, Severity::error);
    result.diagnostic.status = result.status;
    return result;
  }

  std::lock_guard<std::mutex> lock(shard.mutex);
  // Allocate every token and reporting node before publishing any charge.
  // Nodes are retained for historical counters; zero-valued nodes left by a
  // failed preparation own no reservation or allocation.
  const bool page_buffer = IsPageBufferAccounting(event);
  TokenRecord record;
  record.bytes = event.bytes;
  record.page_buffer_bytes = page_buffer;
  record.scope_ids = AccountingScopeIds(event);
  record.tag = std::move(event.tag);
  if (!record.tag.binary_ownership.empty())
    record.owner_key = MemoryBinaryScopeKey{MemoryBinaryScopeKind::owner,
        record.tag.binary_ownership[MemoryBinaryScopeKind::owner]};
  else if (!record.tag.owner.empty()) record.owner_key = TextScopeKey(record.tag.owner);
  shard.categories.try_emplace(record.tag.category);
  for (const auto& scope_id : record.scope_ids) {
    (void)PrepareScope(shard.contexts, scope_id);
    PrepareScope(shard.context_categories, scope_id).try_emplace(record.tag.category);
    if (record.owner_key) (void)PrepareScope(PrepareScope(shard.context_owners, scope_id), *record.owner_key);
    if (page_buffer) (void)PrepareScope(shard.context_page_buffers, scope_id);
  }
  if (record.owner_key) (void)PrepareScope(shard.owners, *record.owner_key);
  const auto inserted = shard.active_tokens.emplace(token_id, std::move(record));
  u64 outstanding = global_outstanding_bytes_.load(std::memory_order_relaxed);
  for (;;) {
    if (event.bytes > std::numeric_limits<u64>::max() - outstanding) {
      shard.active_tokens.erase(inserted.first);
      result.status = LedgerStatus(StatusCode::memory_limit_exceeded, Severity::error);
      result.diagnostic.status = result.status;
      return result;
    }
    if (global_outstanding_bytes_.compare_exchange_weak(
            outstanding, outstanding + event.bytes, std::memory_order_relaxed)) break;
  }
  // From this point through token publication nothing can allocate or throw.
  shard.reserved_bytes += event.bytes;
  ++shard.reservation_count;
  ++shard.active_reservation_count;

  result.status = OkStatus();
  result.token = {token_id, event.bytes, shard_index};
  return result;
} catch (const std::bad_alloc&) {
  ShardedMemoryAccountingResult result;
  result.status = LedgerStatus(StatusCode::memory_allocation_failed, Severity::error);
  result.diagnostic.status = result.status;
  return result;
}

ShardedMemoryAccountingOperationResult ShardedMemoryAccountingLedger::Commit(
    ShardedMemoryAccountingToken token) {
  ShardedMemoryAccountingOperationResult result;
  result.status = OkStatus();
  if (!token.valid() || token.shard_index >= shards_.size()) {
    result.status = LedgerStatus(StatusCode::memory_unknown_pointer, Severity::error);
    result.diagnostic = MakeLedgerDiagnostic(
        result.status,
        "SB-MEMORY-LEDGER-COMMIT-UNKNOWN-RESERVATION",
        "memory.ledger.commit.unknown_reservation",
        {{"token_id", std::to_string(token.token_id)},
         {"token_bytes", std::to_string(token.bytes)},
         {"shard_index", std::to_string(token.shard_index)}});
    return result;
  }

  Shard& shard = ShardForIndex(token.shard_index);
  std::lock_guard<std::mutex> lock(shard.mutex);
  auto it = shard.active_tokens.find(token.token_id);
  if (it == shard.active_tokens.end()) {
    return TokenFailure(shard,
                        token,
                        false,
                        "SB-MEMORY-LEDGER-COMMIT-UNKNOWN-RESERVATION",
                        "memory.ledger.commit.unknown_reservation",
                        {{"shard_index", std::to_string(token.shard_index)}});
  }
  TokenRecord& record = it->second;
  if (record.bytes != token.bytes) {
    return TokenFailure(shard,
                        token,
                        false,
                        "SB-MEMORY-LEDGER-COMMIT-TOKEN-BYTES-MISMATCH",
                        "memory.ledger.commit.token_bytes_mismatch",
                        {{"recorded_bytes", std::to_string(record.bytes)},
                         {"token_bytes", std::to_string(token.bytes)}});
  }
  if (record.state != ShardedMemoryAccountingTokenState::reserved) {
    return TokenFailure(shard,
                        token,
                        false,
                        "SB-MEMORY-LEDGER-COMMIT-STATE-INVALID",
                        "memory.ledger.commit.state_invalid",
                        {{"token_state", record.state == ShardedMemoryAccountingTokenState::committed ? "committed" : "released"}});
  }

  record.state = ShardedMemoryAccountingTokenState::committed;
  shard.reserved_bytes -= record.bytes;
  --shard.active_reservation_count;
  AddBytes(record.bytes, &shard.current_bytes, &shard.peak_bytes);
  ++shard.commit_count;
  ++shard.active_allocation_count;
  const u64 global_current =
      global_current_bytes_.fetch_add(record.bytes, std::memory_order_relaxed) + record.bytes;
  UpdateAtomicPeak(&global_peak_bytes_, global_current);

  CommitCategory(record.bytes, &shard.categories.at(record.tag.category));
  const auto& scope_ids = record.scope_ids;
  for (const auto& scope_id : scope_ids) {
    CommitScope(record.bytes, &shard.contexts.at(scope_id));
    CommitCategory(record.bytes, &shard.context_categories.at(scope_id).at(record.tag.category));
    if (record.owner_key) {
      CommitScope(record.bytes, &shard.context_owners.at(scope_id).at(*record.owner_key));
    }
  }
  if (record.owner_key) {
    CommitScope(record.bytes, &shard.owners.at(*record.owner_key));
  }
  if (IsPageBufferAccounting(record)) {
    AddBytes(record.bytes, &shard.page_buffer_current_bytes, &shard.page_buffer_peak_bytes);
    for (const auto& scope_id : scope_ids) {
      CommitScope(record.bytes, &shard.context_page_buffers.at(scope_id));
    }
    const u64 page_current =
        global_page_buffer_current_bytes_.fetch_add(record.bytes, std::memory_order_relaxed) +
        record.bytes;
    UpdateAtomicPeak(&global_page_buffer_peak_bytes_, page_current);
  }

  return result;
}

ShardedMemoryAccountingOperationResult ShardedMemoryAccountingLedger::Release(
    ShardedMemoryAccountingToken token) {
  return ReleaseImpl(token, true);
}

Status ShardedMemoryAccountingLedger::ReleaseNoAlloc(ShardedMemoryAccountingToken token) {
  return ReleaseImpl(token, false).status;
}

ShardedMemoryAccountingOperationResult ShardedMemoryAccountingLedger::ReleaseImpl(
    ShardedMemoryAccountingToken token, bool materialize_diagnostic) {
  ShardedMemoryAccountingOperationResult result;
  result.status = OkStatus();
  if (!token.valid() || token.shard_index >= shards_.size()) {
    result.status = LedgerStatus(StatusCode::memory_unknown_pointer, Severity::error);
    global_failed_release_count_.fetch_add(1, std::memory_order_relaxed);
    if (!materialize_diagnostic) return result;
    result.diagnostic = MakeLedgerDiagnostic(
        result.status,
        "SB-MEMORY-LEDGER-RELEASE-UNKNOWN-RESERVATION",
        "memory.ledger.release.unknown_reservation",
        {{"token_id", std::to_string(token.token_id)},
         {"token_bytes", std::to_string(token.bytes)},
         {"shard_index", std::to_string(token.shard_index)}});
    return result;
  }

  Shard& shard = ShardForIndex(token.shard_index);
  std::lock_guard<std::mutex> lock(shard.mutex);
  const auto no_alloc_failure = [&] {
    ShardedMemoryAccountingOperationResult failure;
    failure.status = LedgerStatus(StatusCode::memory_unknown_pointer, Severity::error);
    ++shard.failed_release_count;
    global_failed_release_count_.fetch_add(1, std::memory_order_relaxed);
    return failure;
  };
  auto it = shard.active_tokens.find(token.token_id);
  if (it == shard.active_tokens.end()) {
    if (!materialize_diagnostic) return no_alloc_failure();
    return TokenFailure(shard,
                        token,
                        true,
                        "SB-MEMORY-LEDGER-RELEASE-UNKNOWN-RESERVATION",
                        "memory.ledger.release.unknown_reservation",
                        {{"shard_index", std::to_string(token.shard_index)}});
  }
  const TokenRecord& record = it->second;
  if (record.bytes != token.bytes) {
    if (!materialize_diagnostic) return no_alloc_failure();
    return TokenFailure(shard,
                        token,
                        true,
                        "SB-MEMORY-LEDGER-RELEASE-UNDERFLOW-REFUSED",
                        "memory.ledger.release.underflow_refused",
                        {{"recorded_bytes", std::to_string(record.bytes)},
                         {"token_bytes", std::to_string(token.bytes)}});
  }

  if (record.state == ShardedMemoryAccountingTokenState::reserved) {
    shard.reserved_bytes -= record.bytes;
    --shard.active_reservation_count;
    ++shard.release_count;
    global_outstanding_bytes_.fetch_sub(record.bytes, std::memory_order_relaxed);
    shard.active_tokens.erase(it);
    return result;
  }

  if (record.state != ShardedMemoryAccountingTokenState::committed) {
    if (!materialize_diagnostic) return no_alloc_failure();
    return TokenFailure(shard,
                        token,
                        true,
                        "SB-MEMORY-LEDGER-RELEASE-STATE-INVALID",
                        "memory.ledger.release.state_invalid",
                        {{"token_state", "released"}});
  }

  if (shard.current_bytes < record.bytes || shard.active_allocation_count == 0) {
    if (!materialize_diagnostic) return no_alloc_failure();
    return TokenFailure(shard,
                        token,
                        true,
                        "SB-MEMORY-LEDGER-RELEASE-UNDERFLOW-REFUSED",
                        "memory.ledger.release.underflow_refused",
                        {{"recorded_bytes", std::to_string(record.bytes)},
                         {"current_bytes", std::to_string(shard.current_bytes)}});
  }

  shard.current_bytes -= record.bytes;
  --shard.active_allocation_count;
  ++shard.release_count;
  global_current_bytes_.fetch_sub(record.bytes, std::memory_order_relaxed);
  ReleaseCategory(record.bytes, &shard.categories.at(record.tag.category));
  const auto& scope_ids = record.scope_ids;
  for (const auto& scope_id : scope_ids) {
    ReleaseScope(record.bytes, &shard.contexts.at(scope_id));
    ReleaseCategory(record.bytes, &shard.context_categories.at(scope_id).at(record.tag.category));
    if (record.owner_key) {
      ReleaseScope(record.bytes, &shard.context_owners.at(scope_id).at(*record.owner_key));
    }
  }
  if (record.owner_key) {
    ReleaseScope(record.bytes, &shard.owners.at(*record.owner_key));
  }
  if (IsPageBufferAccounting(record)) {
    if (shard.page_buffer_current_bytes >= record.bytes) {
      shard.page_buffer_current_bytes -= record.bytes;
    } else {
      shard.page_buffer_current_bytes = 0;
    }
    for (const auto& scope_id : scope_ids) {
      ReleaseScope(record.bytes, &shard.context_page_buffers.at(scope_id));
    }
    global_page_buffer_current_bytes_.fetch_sub(record.bytes, std::memory_order_relaxed);
  }
  global_outstanding_bytes_.fetch_sub(record.bytes, std::memory_order_relaxed);
  shard.active_tokens.erase(it);
  return result;
}

ShardedMemoryAccountingSnapshot ShardedMemoryAccountingLedger::Snapshot() const {
  ShardedMemoryAccountingSnapshot snapshot;
  snapshot.shard_count = static_cast<u64>(shards_.size());
  snapshot.current_bytes = global_current_bytes_.load(std::memory_order_relaxed);
  snapshot.peak_bytes = global_peak_bytes_.load(std::memory_order_relaxed);
  snapshot.failed_release_count = global_failed_release_count_.load(std::memory_order_relaxed);
  snapshot.page_buffer_current_bytes =
      global_page_buffer_current_bytes_.load(std::memory_order_relaxed);
  snapshot.page_buffer_peak_bytes =
      global_page_buffer_peak_bytes_.load(std::memory_order_relaxed);

  std::map<MemoryCategory, CategoryAccounting> categories;
  std::map<ShardedMemoryScopeKey, ScopeAccounting> contexts;
  std::map<ShardedMemoryScopeKey, ScopeAccounting> owners;

  snapshot.shards.reserve(shards_.size());
  for (usize index = 0; index < shards_.size(); ++index) {
    const Shard& shard = ShardForIndex(index);
    std::lock_guard<std::mutex> lock(shard.mutex);
    snapshot.reserved_bytes += shard.reserved_bytes;
    snapshot.reservation_count += shard.reservation_count;
    snapshot.commit_count += shard.commit_count;
    snapshot.release_count += shard.release_count;
    snapshot.active_reservation_count += shard.active_reservation_count;
    snapshot.active_allocation_count += shard.active_allocation_count;

    ShardedMemoryAccountingShardSnapshot shard_snapshot;
    shard_snapshot.shard_index = index;
    shard_snapshot.reserved_bytes = shard.reserved_bytes;
    shard_snapshot.current_bytes = shard.current_bytes;
    shard_snapshot.peak_bytes = shard.peak_bytes;
    shard_snapshot.reservation_count = shard.reservation_count;
    shard_snapshot.commit_count = shard.commit_count;
    shard_snapshot.release_count = shard.release_count;
    shard_snapshot.active_reservation_count = shard.active_reservation_count;
    shard_snapshot.active_allocation_count = shard.active_allocation_count;
    shard_snapshot.page_buffer_current_bytes = shard.page_buffer_current_bytes;
    shard_snapshot.page_buffer_peak_bytes = shard.page_buffer_peak_bytes;
    snapshot.shards.push_back(shard_snapshot);

    for (const auto& entry : shard.categories) {
      MergeCategory(entry.first, entry.second, &categories);
    }
    for (const auto& entry : shard.contexts) {
      MergeScope(entry.first, entry.second, &contexts);
    }
    for (const auto& entry : shard.owners) {
      MergeScope(entry.first, entry.second, &owners);
    }
  }

  snapshot.categories = CategorySnapshots(categories);
  snapshot.contexts = ScopeSnapshots(contexts);
  snapshot.owners = ScopeSnapshots(owners);
  return snapshot;
}

ShardedMemoryAccountingSnapshot ShardedMemoryAccountingLedger::SnapshotForContext(
    std::string context_id) const {
  return SnapshotForScope(ShardedMemoryScopeKey{std::move(context_id)});
}

ShardedMemoryAccountingSnapshot ShardedMemoryAccountingLedger::SnapshotForContext(
    MemoryBinaryScopeKey context) const {
  return SnapshotForScope(ShardedMemoryScopeKey{context});
}

ShardedMemoryAccountingSnapshot ShardedMemoryAccountingLedger::SnapshotForScope(
    ShardedMemoryScopeKey context_id) const {
  ShardedMemoryAccountingSnapshot snapshot;
  if (const auto* binary = std::get_if<MemoryBinaryScopeKey>(&context_id))
    snapshot.binary_context_filter = *binary;
  else snapshot.context_filter = std::get<std::string>(context_id);
  snapshot.shard_count = static_cast<u64>(shards_.size());

  std::map<MemoryCategory, CategoryAccounting> categories;
  std::map<ShardedMemoryScopeKey, ScopeAccounting> contexts;
  std::map<ShardedMemoryScopeKey, ScopeAccounting> owners;

  snapshot.shards.reserve(shards_.size());
  for (usize index = 0; index < shards_.size(); ++index) {
    const Shard& shard = ShardForIndex(index);
    std::lock_guard<std::mutex> lock(shard.mutex);
    const auto context_it = shard.contexts.find(context_id);

    ShardedMemoryAccountingShardSnapshot shard_snapshot;
    shard_snapshot.shard_index = index;
    if (context_it != shard.contexts.end()) {
      shard_snapshot.current_bytes = context_it->second.current_bytes;
      shard_snapshot.peak_bytes = context_it->second.peak_bytes;
      shard_snapshot.commit_count = context_it->second.allocation_count;
      shard_snapshot.release_count = context_it->second.release_count;
      shard_snapshot.active_allocation_count = context_it->second.active_allocation_count;

      snapshot.current_bytes += context_it->second.current_bytes;
      snapshot.peak_bytes += context_it->second.peak_bytes;
      snapshot.commit_count += context_it->second.allocation_count;
      snapshot.release_count += context_it->second.release_count;
      snapshot.active_allocation_count += context_it->second.active_allocation_count;
      MergeScope(context_id, context_it->second, &contexts);
    }
    snapshot.shards.push_back(shard_snapshot);

    const auto categories_it = shard.context_categories.find(context_id);
    if (categories_it != shard.context_categories.end()) {
      for (const auto& entry : categories_it->second) {
        MergeCategory(entry.first, entry.second, &categories);
      }
    }
    const auto owners_it = shard.context_owners.find(context_id);
    if (owners_it != shard.context_owners.end()) {
      for (const auto& entry : owners_it->second) {
        MergeScope(entry.first, entry.second, &owners);
      }
    }
    const auto page_it = shard.context_page_buffers.find(context_id);
    if (page_it != shard.context_page_buffers.end()) {
      snapshot.page_buffer_current_bytes += page_it->second.current_bytes;
      snapshot.page_buffer_peak_bytes += page_it->second.peak_bytes;
    }
  }

  snapshot.categories = CategorySnapshots(categories);
  snapshot.contexts = ScopeSnapshots(contexts);
  snapshot.owners = ScopeSnapshots(owners);
  return snapshot;
}

ShardedMemoryAccountingLedger::Shard& ShardedMemoryAccountingLedger::ShardForIndex(usize shard_index) {
  return *shards_[shard_index];
}

const ShardedMemoryAccountingLedger::Shard& ShardedMemoryAccountingLedger::ShardForIndex(
    usize shard_index) const {
  return *shards_[shard_index];
}

ShardedMemoryAccountingOperationResult ShardedMemoryAccountingLedger::TokenFailure(
    Shard& shard,
    const ShardedMemoryAccountingToken& token,
    bool release_failure,
    std::string diagnostic_code,
    std::string message_key,
    std::vector<DiagnosticArgument> arguments) {
  if (release_failure) {
    ++shard.failed_release_count;
    global_failed_release_count_.fetch_add(1, std::memory_order_relaxed);
  }
  ShardedMemoryAccountingOperationResult result;
  result.status = LedgerStatus(StatusCode::memory_unknown_pointer, Severity::error);
  arguments.push_back({"token_id", std::to_string(token.token_id)});
  arguments.push_back({"token_bytes", std::to_string(token.bytes)});
  arguments.push_back({"active_reservation_count", std::to_string(shard.active_reservation_count)});
  arguments.push_back({"active_allocation_count", std::to_string(shard.active_allocation_count)});
  arguments.push_back({"current_bytes", std::to_string(shard.current_bytes)});
  result.diagnostic = MakeLedgerDiagnostic(result.status,
                                           std::move(diagnostic_code),
                                           std::move(message_key),
                                           std::move(arguments));
  return result;
}

}  // namespace scratchbird::core::memory
