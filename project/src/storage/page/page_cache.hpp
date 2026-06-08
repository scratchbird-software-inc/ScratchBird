// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-PAGE-CACHE-ANCHOR
#include "memory.hpp"
#include "page_header.hpp"
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::storage::disk::PageType;

enum class PageCacheIoContext : u32 {
  normal,
  bulk_read,
  bulk_write,
  vacuum_cleanup,
  index_build,
  strict_bulk_load
};

inline constexpr std::size_t kPageCacheIoContextCount = 6;
inline constexpr std::size_t kPageCacheFrameShardCount = 8;

struct PageCacheContextCounters {
  u64 admissions = 0;
  u64 reuses = 0;
  u64 evictions = 0;
  u64 protected_normal_hot_skips = 0;
  u64 refusals = 0;
};

struct PageCachePolicy {
  // MMCH_MEMORY_METADATA_OPEN_UPGRADE_COMPATIBILITY
  // Page-cache metadata versioning is compatibility evidence only; cache
  // metadata is not finality, visibility, catalog, or recovery authority.
  u64 metadata_format_version = 2;
  u64 max_resident_pages = 64;
  u64 max_resident_bytes = 64ull * 1024ull * 1024ull;
  bool allow_dirty_eviction = false;
  bool require_memory_manager_frames = true;
  u64 bulk_read_ring_pages = 4;
  u64 bulk_write_ring_pages = 4;
  u64 vacuum_cleanup_ring_pages = 4;
  u64 index_build_ring_pages = 8;
  u64 strict_bulk_load_ring_pages = 8;
};

enum class PageCacheLifecycleState : u32 {
  not_started,
  starting,
  active,
  memory_pressure,
  checkpointing,
  clean,
  draining,
  stopped,
  failed,
  quarantined
};

enum class PageCacheCheckpointMode : u32 {
  try_checkpoint,
  wait_checkpoint,
  force_checkpoint,
  clean_close,
  shutdown_flush
};

struct PageCacheEntry {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid page_uuid;
  PageType page_type = PageType::unknown;
  u64 page_number = 0;
  u64 page_generation = 0;
  u32 page_size = 0;
  u64 resident_bytes = 0;
  u64 pin_count = 0;
  bool dirty = false;
  bool resident = false;
  bool writeback_in_progress = false;
  u64 last_access_tick = 0;
  u64 dirty_epoch = 0;
  u64 last_writeback_checkpoint_generation = 0;
  PageCacheIoContext io_context = PageCacheIoContext::normal;
  bool cache_hot = false;
  u64 frame_shard_id = 0;
  std::string frame_scope_id;
};

struct PageCacheContextSnapshot {
  PageCacheIoContext context = PageCacheIoContext::normal;
  std::string context_name;
  u64 resident_pages = 0;
  u64 resident_bytes = 0;
  u64 pinned_pages = 0;
  u64 dirty_pages = 0;
  u64 admissions = 0;
  u64 reuses = 0;
  u64 evictions = 0;
  u64 protected_normal_hot_skips = 0;
  u64 refusals = 0;
};

struct PageCacheFrameShardSnapshot {
  u64 shard_id = 0;
  u64 resident_frames = 0;
  u64 resident_bytes = 0;
  std::array<u64, kPageCacheIoContextCount> context_frames{};
  std::array<u64, kPageCacheIoContextCount> context_bytes{};
  u64 allocation_count = 0;
  u64 release_count = 0;
  u64 allocation_failure_count = 0;
};

struct PageCacheSnapshot {
  u64 resident_pages = 0;
  u64 resident_bytes = 0;
  u64 pinned_pages = 0;
  u64 dirty_pages = 0;
  u64 writeback_pages = 0;
  u64 memory_manager_frame_bytes = 0;
  u64 memory_manager_frame_count = 0;
  u64 memory_manager_frame_allocation_count = 0;
  u64 memory_manager_frame_release_count = 0;
  u64 memory_manager_frame_allocation_failure_count = 0;
  u64 frame_shard_count = kPageCacheFrameShardCount;
  u64 active_frame_shard_count = 0;
  bool memory_manager_frames_bound = false;
  bool sharded_frame_table_bound = false;
  std::vector<PageCacheContextSnapshot> contexts;
  std::vector<PageCacheFrameShardSnapshot> frame_shards;
};

struct PageCacheResidentFrame {
  scratchbird::core::memory::ScopedPageBuffer buffer;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid page_uuid;
  PageCacheIoContext io_context = PageCacheIoContext::normal;
  u64 page_number = 0;
  u64 page_generation = 0;
  u64 page_size = 0;
  std::string scope_id;
};

struct PageCacheFrameShard {
  mutable std::mutex mutex;
  std::map<std::string, PageCacheResidentFrame> frames;
  u64 allocation_count = 0;
  u64 release_count = 0;
  u64 allocation_failure_count = 0;
};

struct PageCacheLedger {
  mutable std::mutex mutex;
  std::vector<PageCacheEntry> entries;
  scratchbird::core::memory::MemoryManager* memory_manager = nullptr;
  std::array<PageCacheFrameShard, kPageCacheFrameShardCount> frame_shards;
  // Legacy mirror retained for ABI/source compatibility. Resident frame
  // authority lives in frame_shards.
  std::map<std::string, scratchbird::core::memory::ScopedPageBuffer> resident_frames;
  std::array<PageCacheContextCounters, kPageCacheIoContextCount> context_counters{};
  std::array<u64, kPageCacheIoContextCount> ring_reuse_cursors{};
  u64 frame_allocation_count = 0;
  u64 frame_release_count = 0;
  u64 frame_allocation_failure_count = 0;
  u64 next_access_tick = 1;
  u64 dirty_epoch = 0;
  u64 last_checkpoint_generation = 0;
  u64 writeback_count = 0;
  u64 checkpoint_count = 0;
  u64 shutdown_flush_count = 0;
};

struct PageCacheAuthorityBoundary {
  bool transaction_finality_authority = false;
  bool transaction_visibility_authority = false;
  bool recovery_finality_authority = false;
  bool catalog_truth_authority = false;
  bool filespace_identity_authority = false;
  bool parser_authority = false;
  bool wal_or_redo_authority = false;
  bool checkpoint_is_clean_close_evidence_only = true;
  bool cache_flush_requires_mga_finality_evidence = true;
};

struct PageCacheLifecycleInput {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  std::string database_lifecycle_state = "opened";
  u64 policy_generation = 1;
  u64 checkpoint_generation = 1;
  u64 dirty_epoch = 0;
  u64 target_resident_pages = 0;
  bool tx2_activation_committed = false;
  bool cache_runtime_started = false;
  bool engine_agent_active = false;
  bool writeback_allowed = true;
  bool checkpoint_allowed = true;
  bool clean_close_requested = false;
  bool shutdown_requested = false;
  bool force = false;
  bool standalone_mode = true;
  bool cluster_authority_available = false;
};

struct PageCacheCheckpointPublication {
  std::string database_uuid;
  std::string filespace_uuid;
  std::string database_lifecycle_state;
  PageCacheLifecycleState lifecycle_state = PageCacheLifecycleState::not_started;
  PageCacheCheckpointMode checkpoint_mode = PageCacheCheckpointMode::try_checkpoint;
  u64 policy_generation = 0;
  u64 checkpoint_generation = 0;
  u64 dirty_epoch = 0;
  u64 resident_pages = 0;
  u64 dirty_pages_before = 0;
  u64 dirty_pages_after = 0;
  u64 flushed_pages = 0;
  u64 evicted_pages = 0;
  bool preload_complete = false;
  bool writeback_complete = false;
  bool checkpoint_complete = false;
  bool clean_close_evidence = false;
  bool shutdown_flush_complete = false;
  bool memory_pressure_handled = false;
  bool cluster_paths_failed_closed = true;
  bool ordinary_admission_allowed = false;
  std::vector<std::string> diagnostics;
  PageCacheAuthorityBoundary authority_boundary;
};

struct PageCacheResult {
  Status status;
  bool changed = false;
  bool evicted = false;
  PageCacheEntry entry;
  PageCacheSnapshot snapshot;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok(); }
};

struct PageCacheLifecycleResult {
  Status status;
  PageCacheLifecycleState state = PageCacheLifecycleState::failed;
  PageCacheSnapshot snapshot;
  PageCacheCheckpointPublication publication;
  u64 flushed_pages = 0;
  u64 evicted_pages = 0;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok(); }
};

const char* PageCacheIoContextName(PageCacheIoContext context);
bool PageCacheIoContextFromName(std::string_view name, PageCacheIoContext* context);
bool PageCacheIoContextUsesBoundedRing(PageCacheIoContext context);
u64 PageCacheIoContextRingLimit(const PageCachePolicy& policy, PageCacheIoContext context);
const char* PageCacheLifecycleStateName(PageCacheLifecycleState state);
const char* PageCacheCheckpointModeName(PageCacheCheckpointMode mode);
bool PageCacheAuthorityBoundaryValid(const PageCacheAuthorityBoundary& boundary);
PageCacheLifecycleResult ValidatePageCacheAuthorityBoundary(const PageCacheAuthorityBoundary& boundary);
void BindPageCacheMemoryManager(PageCacheLedger* ledger,
                                scratchbird::core::memory::MemoryManager* manager);
PageCacheSnapshot SnapshotPageCache(const PageCacheLedger& ledger);
PageCacheContextSnapshot SnapshotPageCacheContext(const PageCacheLedger& ledger,
                                                  PageCacheIoContext context);
PageCacheResult AdmitPageCacheEntry(PageCacheLedger* ledger,
                                    const PageCachePolicy& policy,
                                    const PageCacheEntry& entry);
PageCacheResult AdmitPageCacheEntryForContext(PageCacheLedger* ledger,
                                              const PageCachePolicy& policy,
                                              const PageCacheEntry& entry,
                                              PageCacheIoContext context);
PageCacheResult PinPageCacheEntry(PageCacheLedger* ledger, const TypedUuid& page_uuid);
PageCacheResult UnpinPageCacheEntry(PageCacheLedger* ledger, const TypedUuid& page_uuid);
PageCacheResult MarkPageCacheEntryDirty(PageCacheLedger* ledger, const TypedUuid& page_uuid, bool dirty);
PageCacheResult EvictOnePageCacheEntry(PageCacheLedger* ledger, const PageCachePolicy& policy);
PageCacheResult EvictOnePageCacheEntryForContext(PageCacheLedger* ledger,
                                                 const PageCachePolicy& policy,
                                                 PageCacheIoContext context);
PageCacheLifecycleResult StartPageCacheLifecycle(PageCacheLedger* ledger,
                                                 const PageCachePolicy& policy,
                                                 const PageCacheLifecycleInput& input,
                                                 const std::vector<PageCacheEntry>& preload_entries);
PageCacheLifecycleResult WritebackDirtyPageCacheEntries(PageCacheLedger* ledger,
                                                        const PageCacheLifecycleInput& input);
PageCacheLifecycleResult CheckpointPageCacheLifecycle(PageCacheLedger* ledger,
                                                      const PageCacheLifecycleInput& input,
                                                      PageCacheCheckpointMode mode);
PageCacheLifecycleResult ApplyPageCacheMemoryPressure(PageCacheLedger* ledger,
                                                      const PageCachePolicy& policy,
                                                      const PageCacheLifecycleInput& input);
PageCacheLifecycleResult ShutdownFlushPageCacheLifecycle(PageCacheLedger* ledger,
                                                         const PageCacheLifecycleInput& input);
std::string SerializePageCacheCheckpointJson(const PageCacheCheckpointPublication& publication,
                                             bool diagnostic_role);
std::vector<std::string> PageCacheDiagnosticCodes();
DiagnosticRecord MakePageCacheDiagnostic(Status status,
                                         std::string diagnostic_code,
                                         std::string message_key,
                                         std::string detail = {});

}  // namespace scratchbird::storage::page
