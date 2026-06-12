// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sharded_memory_accounting_ledger.hpp"

#include <algorithm>
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

std::vector<std::string> AccountingScopeIds(
    const ShardedMemoryAccountingLedger::TokenRecord& record) {
  std::vector<std::string> scope_ids;
  if (!record.tag.context_id.empty()) {
    scope_ids.push_back(record.tag.context_id);
  }
  for (const auto& scope_id : record.scope_ids) {
    if (scope_id.empty() ||
        std::find(scope_ids.begin(), scope_ids.end(), scope_id) != scope_ids.end()) {
      continue;
    }
    scope_ids.push_back(scope_id);
  }
  return scope_ids;
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

void MergeScope(const std::string& scope_id,
                const ShardedMemoryAccountingLedger::ScopeAccounting& source,
                std::map<std::string, ShardedMemoryAccountingLedger::ScopeAccounting>* target) {
  auto& merged = (*target)[scope_id];
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
    const std::map<std::string, ShardedMemoryAccountingLedger::ScopeAccounting>& scopes) {
  std::vector<ShardedMemoryAccountingScopeSnapshot> snapshots;
  snapshots.reserve(scopes.size());
  for (const auto& entry : scopes) {
    ShardedMemoryAccountingScopeSnapshot snapshot;
    snapshot.scope_id = entry.first;
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

ShardedMemoryAccountingResult ShardedMemoryAccountingLedger::Reserve(ShardedMemoryAccountingEvent event) {
  ShardedMemoryAccountingResult result;
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
  const u64 token_id = next_token_id_.fetch_add(1, std::memory_order_relaxed);

  std::lock_guard<std::mutex> lock(shard.mutex);
  shard.reserved_bytes += event.bytes;
  ++shard.reservation_count;
  ++shard.active_reservation_count;
  shard.active_tokens[token_id] = {event.bytes,
                                  std::move(event.tag),
                                  std::move(event.scope_ids),
                                  IsPageBufferAccounting(event),
                                  ShardedMemoryAccountingTokenState::reserved};

  result.status = OkStatus();
  result.token = {token_id, event.bytes, shard_index};
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

  CommitCategory(record.bytes, &shard.categories[record.tag.category]);
  const auto scope_ids = AccountingScopeIds(record);
  for (const auto& scope_id : scope_ids) {
    CommitScope(record.bytes, &shard.contexts[scope_id]);
    CommitCategory(record.bytes, &shard.context_categories[scope_id][record.tag.category]);
    if (!record.tag.owner.empty()) {
      CommitScope(record.bytes, &shard.context_owners[scope_id][record.tag.owner]);
    }
  }
  if (!record.tag.owner.empty()) {
    CommitScope(record.bytes, &shard.owners[record.tag.owner]);
  }
  if (IsPageBufferAccounting(record)) {
    AddBytes(record.bytes, &shard.page_buffer_current_bytes, &shard.page_buffer_peak_bytes);
    for (const auto& scope_id : scope_ids) {
      CommitScope(record.bytes, &shard.context_page_buffers[scope_id]);
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
  ShardedMemoryAccountingOperationResult result;
  result.status = OkStatus();
  if (!token.valid() || token.shard_index >= shards_.size()) {
    result.status = LedgerStatus(StatusCode::memory_unknown_pointer, Severity::error);
    global_failed_release_count_.fetch_add(1, std::memory_order_relaxed);
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
  auto it = shard.active_tokens.find(token.token_id);
  if (it == shard.active_tokens.end()) {
    return TokenFailure(shard,
                        token,
                        true,
                        "SB-MEMORY-LEDGER-RELEASE-UNKNOWN-RESERVATION",
                        "memory.ledger.release.unknown_reservation",
                        {{"shard_index", std::to_string(token.shard_index)}});
  }
  TokenRecord record = it->second;
  if (record.bytes != token.bytes) {
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
    shard.active_tokens.erase(it);
    return result;
  }

  if (record.state != ShardedMemoryAccountingTokenState::committed) {
    return TokenFailure(shard,
                        token,
                        true,
                        "SB-MEMORY-LEDGER-RELEASE-STATE-INVALID",
                        "memory.ledger.release.state_invalid",
                        {{"token_state", "released"}});
  }

  if (shard.current_bytes < record.bytes || shard.active_allocation_count == 0) {
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
  ReleaseCategory(record.bytes, &shard.categories[record.tag.category]);
  const auto scope_ids = AccountingScopeIds(record);
  for (const auto& scope_id : scope_ids) {
    ReleaseScope(record.bytes, &shard.contexts[scope_id]);
    ReleaseCategory(record.bytes, &shard.context_categories[scope_id][record.tag.category]);
    if (!record.tag.owner.empty()) {
      ReleaseScope(record.bytes, &shard.context_owners[scope_id][record.tag.owner]);
    }
  }
  if (!record.tag.owner.empty()) {
    ReleaseScope(record.bytes, &shard.owners[record.tag.owner]);
  }
  if (IsPageBufferAccounting(record)) {
    if (shard.page_buffer_current_bytes >= record.bytes) {
      shard.page_buffer_current_bytes -= record.bytes;
    } else {
      shard.page_buffer_current_bytes = 0;
    }
    for (const auto& scope_id : scope_ids) {
      ReleaseScope(record.bytes, &shard.context_page_buffers[scope_id]);
    }
    global_page_buffer_current_bytes_.fetch_sub(record.bytes, std::memory_order_relaxed);
  }
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
  std::map<std::string, ScopeAccounting> contexts;
  std::map<std::string, ScopeAccounting> owners;

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
  ShardedMemoryAccountingSnapshot snapshot;
  snapshot.context_filter = context_id;
  snapshot.shard_count = static_cast<u64>(shards_.size());

  std::map<MemoryCategory, CategoryAccounting> categories;
  std::map<std::string, ScopeAccounting> contexts;
  std::map<std::string, ScopeAccounting> owners;

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
