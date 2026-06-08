// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC-010: foundational sharded memory accounting primitive.
#include "memory.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace scratchbird::core::memory {

enum class ShardedMemoryAccountingTokenState {
  reserved,
  committed,
  released
};

struct ShardedMemoryAccountingEvent {
  u64 bytes = 0;
  MemoryTag tag;
  std::vector<std::string> scope_ids;
  bool page_buffer_bytes = false;
};

struct ShardedMemoryAccountingToken {
  u64 token_id = 0;
  u64 bytes = 0;
  usize shard_index = 0;

  bool valid() const {
    return token_id != 0 && bytes != 0;
  }
};

struct ShardedMemoryAccountingResult {
  Status status;
  DiagnosticRecord diagnostic;
  ShardedMemoryAccountingToken token;

  bool ok() const {
    return status.ok() && token.valid();
  }
};

struct ShardedMemoryAccountingOperationResult {
  Status status;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct ShardedMemoryAccountingScopeSnapshot {
  std::string scope_id;
  u64 current_bytes = 0;
  u64 peak_bytes = 0;
  u64 allocation_count = 0;
  u64 release_count = 0;
  u64 active_allocation_count = 0;
};

struct ShardedMemoryAccountingCategorySnapshot {
  MemoryCategory category = MemoryCategory::unknown;
  u64 current_bytes = 0;
  u64 peak_bytes = 0;
  u64 allocation_count = 0;
  u64 release_count = 0;
  u64 active_allocation_count = 0;
};

struct ShardedMemoryAccountingShardSnapshot {
  usize shard_index = 0;
  u64 reserved_bytes = 0;
  u64 current_bytes = 0;
  u64 peak_bytes = 0;
  u64 reservation_count = 0;
  u64 commit_count = 0;
  u64 release_count = 0;
  u64 active_reservation_count = 0;
  u64 active_allocation_count = 0;
  u64 page_buffer_current_bytes = 0;
  u64 page_buffer_peak_bytes = 0;
};

struct ShardedMemoryAccountingSnapshot {
  std::string context_filter;
  u64 shard_count = 0;
  u64 reserved_bytes = 0;
  u64 current_bytes = 0;
  u64 peak_bytes = 0;
  u64 reservation_count = 0;
  u64 commit_count = 0;
  u64 release_count = 0;
  u64 failed_release_count = 0;
  u64 active_reservation_count = 0;
  u64 active_allocation_count = 0;
  u64 page_buffer_current_bytes = 0;
  u64 page_buffer_peak_bytes = 0;
  std::vector<ShardedMemoryAccountingShardSnapshot> shards;
  std::vector<ShardedMemoryAccountingCategorySnapshot> categories;
  std::vector<ShardedMemoryAccountingScopeSnapshot> contexts;
  std::vector<ShardedMemoryAccountingScopeSnapshot> owners;
};

class ShardedMemoryAccountingLedger {
 public:
  explicit ShardedMemoryAccountingLedger(usize shard_count = 64);
  ShardedMemoryAccountingLedger(const ShardedMemoryAccountingLedger&) = delete;
  ShardedMemoryAccountingLedger& operator=(const ShardedMemoryAccountingLedger&) = delete;
  ~ShardedMemoryAccountingLedger();

  usize shard_count() const;
  usize ShardIndexForEvent(const ShardedMemoryAccountingEvent& event) const;

  ShardedMemoryAccountingResult Reserve(ShardedMemoryAccountingEvent event);
  ShardedMemoryAccountingOperationResult Commit(ShardedMemoryAccountingToken token);
  ShardedMemoryAccountingOperationResult Release(ShardedMemoryAccountingToken token);

  ShardedMemoryAccountingSnapshot Snapshot() const;
  ShardedMemoryAccountingSnapshot SnapshotForContext(std::string context_id) const;

  struct ScopeAccounting {
    u64 current_bytes = 0;
    u64 peak_bytes = 0;
    u64 allocation_count = 0;
    u64 release_count = 0;
    u64 active_allocation_count = 0;
  };

  struct CategoryAccounting {
    u64 current_bytes = 0;
    u64 peak_bytes = 0;
    u64 allocation_count = 0;
    u64 release_count = 0;
    u64 active_allocation_count = 0;
  };

  struct TokenRecord {
    u64 bytes = 0;
    MemoryTag tag;
    std::vector<std::string> scope_ids;
    bool page_buffer_bytes = false;
    ShardedMemoryAccountingTokenState state = ShardedMemoryAccountingTokenState::reserved;
  };

  struct Shard {
    mutable std::mutex mutex;
    u64 reserved_bytes = 0;
    u64 current_bytes = 0;
    u64 peak_bytes = 0;
    u64 reservation_count = 0;
    u64 commit_count = 0;
    u64 release_count = 0;
    u64 failed_release_count = 0;
    u64 active_reservation_count = 0;
    u64 active_allocation_count = 0;
    u64 page_buffer_current_bytes = 0;
    u64 page_buffer_peak_bytes = 0;
    std::unordered_map<u64, TokenRecord> active_tokens;
    std::map<MemoryCategory, CategoryAccounting> categories;
    std::unordered_map<std::string, ScopeAccounting> contexts;
    std::unordered_map<std::string, ScopeAccounting> owners;
    std::unordered_map<std::string, std::map<MemoryCategory, CategoryAccounting>> context_categories;
    std::unordered_map<std::string, std::unordered_map<std::string, ScopeAccounting>> context_owners;
    std::unordered_map<std::string, ScopeAccounting> context_page_buffers;
  };

 private:
  Shard& ShardForIndex(usize shard_index);
  const Shard& ShardForIndex(usize shard_index) const;
  ShardedMemoryAccountingOperationResult TokenFailure(Shard& shard,
                                                     const ShardedMemoryAccountingToken& token,
                                                     bool release_failure,
                                                     std::string diagnostic_code,
                                                     std::string message_key,
                                                     std::vector<DiagnosticArgument> arguments);

  std::vector<std::unique_ptr<Shard>> shards_;
  std::atomic<u64> next_token_id_{1};
  std::atomic<u64> global_current_bytes_{0};
  std::atomic<u64> global_peak_bytes_{0};
  std::atomic<u64> global_page_buffer_current_bytes_{0};
  std::atomic<u64> global_page_buffer_peak_bytes_{0};
  std::atomic<u64> global_failed_release_count_{0};
};

}  // namespace scratchbird::core::memory
