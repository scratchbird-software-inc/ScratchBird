// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory.hpp"
#include "page_cache.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

namespace mem = scratchbird::core::memory;
namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::storage::disk::PageType;

constexpr std::uint32_t kPageSize = 16384;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool EvidenceHas(const std::vector<std::string>& evidence,
                 std::string_view token) {
  for (const auto& row : evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireFrameEvidence(const std::vector<std::string>& evidence) {
  Require(EvidenceHas(evidence, "MMCH_PAGE_CACHE_FRAME_OWNERSHIP"),
          "CEIC-019 frame ownership marker missing");
  Require(EvidenceHas(evidence, "MMCH_PAGE_CACHE_SHARDED_FRAME_TABLE"),
          "CEIC-019 sharded frame-table marker missing");
  Require(EvidenceHas(evidence, "page_cache.sharded_snapshot_order=shard_id_ascending"),
          "CEIC-019 deterministic shard merge evidence missing");
  Require(EvidenceHas(
              evidence,
              "page_cache.enterprise_frame_authority_scope=evidence_only_not_transaction_finality_row_visibility_security_authorization_recovery_parser_donor_wal_benchmark_optimizer_plan_index_finality_or_agent_action_authority"),
          "CEIC-019 enterprise authority-boundary evidence missing");
  Require(EvidenceHas(evidence,
                      "page_cache.cluster_scope=external_cluster_provider_only_no_local_cluster_page_cache_authority"),
          "CEIC-019 cluster external-provider boundary evidence missing");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

TypedUuid MakeUuid(UuidKind kind, std::uint64_t offset) {
  auto generated = uuid::GenerateEngineIdentityV7(kind, CurrentUnixMillis() + offset);
  Require(generated.ok(), "CEIC-019 UUID generation failed");
  return generated.value;
}

mem::MemoryManager Manager(std::uint64_t pages) {
  auto policy = mem::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "ceic019_page_cache_sharded_frames";
  policy.hard_limit_bytes = pages * kPageSize;
  policy.soft_limit_bytes = pages * kPageSize;
  policy.per_context_limit_bytes = pages * kPageSize;
  policy.page_buffer_pool_limit_bytes = pages * kPageSize;
  policy.reject_over_soft_limit = false;
  return mem::MemoryManager(policy);
}

page::PageCachePolicy Policy(std::uint64_t pages) {
  page::PageCachePolicy policy;
  policy.max_resident_pages = pages;
  policy.max_resident_bytes = pages * kPageSize;
  policy.allow_dirty_eviction = false;
  policy.require_memory_manager_frames = true;
  policy.bulk_read_ring_pages = 2;
  policy.bulk_write_ring_pages = 2;
  policy.vacuum_cleanup_ring_pages = 2;
  policy.index_build_ring_pages = 2;
  policy.strict_bulk_load_ring_pages = 2;
  return policy;
}

page::PageCacheEntry Entry(const TypedUuid& database_uuid,
                           const TypedUuid& filespace_uuid,
                           std::uint64_t index) {
  page::PageCacheEntry entry;
  entry.database_uuid = database_uuid;
  entry.filespace_uuid = filespace_uuid;
  entry.page_uuid = MakeUuid(UuidKind::page, 1000 + index);
  entry.page_type = PageType::row_data;
  entry.page_number = index;
  entry.page_generation = 1;
  entry.page_size = kPageSize;
  return entry;
}

page::PageCacheLifecycleInput LifecycleInput(const TypedUuid& database_uuid,
                                             const TypedUuid& filespace_uuid) {
  page::PageCacheLifecycleInput input;
  input.database_uuid = database_uuid;
  input.filespace_uuid = filespace_uuid;
  input.database_lifecycle_state = "opened";
  input.policy_generation = 19;
  input.checkpoint_generation = 190;
  input.target_resident_pages = 1;
  input.tx2_activation_committed = true;
  input.cache_runtime_started = true;
  input.engine_agent_active = true;
  input.writeback_allowed = true;
  input.checkpoint_allowed = true;
  input.standalone_mode = true;
  input.cluster_authority_available = false;
  return input;
}

std::uint64_t SumShardFrames(const page::PageCacheSnapshot& snapshot) {
  std::uint64_t total = 0;
  for (const auto& shard : snapshot.frame_shards) {
    total += shard.resident_frames;
  }
  return total;
}

std::uint64_t SumShardBytes(const page::PageCacheSnapshot& snapshot) {
  std::uint64_t total = 0;
  for (const auto& shard : snapshot.frame_shards) {
    total += shard.resident_bytes;
  }
  return total;
}

std::uint64_t SumShardReleases(const page::PageCacheSnapshot& snapshot) {
  std::uint64_t total = 0;
  for (const auto& shard : snapshot.frame_shards) {
    total += shard.release_count;
  }
  return total;
}

std::uint64_t SumShardContextFrames(const page::PageCacheSnapshot& snapshot,
                                    page::PageCacheIoContext context) {
  std::uint64_t total = 0;
  const auto context_index = static_cast<std::size_t>(context);
  for (const auto& shard : snapshot.frame_shards) {
    total += shard.context_frames[context_index];
  }
  return total;
}

void ShardedFrameSnapshotMergesDeterministically() {
  auto manager = Manager(8);
  page::PageCacheLedger ledger;
  page::BindPageCacheMemoryManager(&ledger, &manager);
  const auto policy = Policy(6);
  const auto database_uuid = MakeUuid(UuidKind::database, 1);
  const auto filespace_uuid = MakeUuid(UuidKind::filespace, 2);

  auto normal = page::AdmitPageCacheEntryForContext(
      &ledger, policy, Entry(database_uuid, filespace_uuid, 1), page::PageCacheIoContext::normal);
  auto bulk = page::AdmitPageCacheEntryForContext(
      &ledger, policy, Entry(database_uuid, filespace_uuid, 2), page::PageCacheIoContext::bulk_read);
  auto index = page::AdmitPageCacheEntryForContext(
      &ledger, policy, Entry(database_uuid, filespace_uuid, 3), page::PageCacheIoContext::index_build);
  Require(normal.ok() && bulk.ok() && index.ok(), "CEIC-019 setup admission failed");

  const auto snapshot = page::SnapshotPageCache(ledger);
  Require(snapshot.sharded_frame_table_bound,
          "CEIC-019 sharded frame table not marked bound");
  Require(snapshot.frame_shard_count == page::kPageCacheFrameShardCount &&
              snapshot.frame_shards.size() == page::kPageCacheFrameShardCount,
          "CEIC-019 shard count mismatch");
  for (std::size_t i = 0; i < snapshot.frame_shards.size(); ++i) {
    Require(snapshot.frame_shards[i].shard_id == i,
            "CEIC-019 shard snapshots are not deterministic ascending order");
  }
  Require(SumShardFrames(snapshot) == snapshot.memory_manager_frame_count &&
              SumShardBytes(snapshot) == snapshot.memory_manager_frame_bytes,
          "CEIC-019 merged shard totals do not match frame totals");
  Require(SumShardContextFrames(snapshot, page::PageCacheIoContext::normal) ==
              page::SnapshotPageCacheContext(ledger, page::PageCacheIoContext::normal).resident_pages,
          "CEIC-019 normal context shard totals mismatch");
  Require(SumShardContextFrames(snapshot, page::PageCacheIoContext::bulk_read) ==
              page::SnapshotPageCacheContext(ledger, page::PageCacheIoContext::bulk_read).resident_pages,
          "CEIC-019 bulk context shard totals mismatch");
  Require(normal.entry.frame_scope_id.find("context=normal") != std::string::npos &&
              bulk.entry.frame_scope_id.find("context=bulk_read") != std::string::npos &&
              index.entry.frame_scope_id.find("context=index_build") != std::string::npos,
          "CEIC-019 page entries did not retain per-context frame scope evidence");
  Require(manager.Snapshot().page_buffer_current_bytes == snapshot.memory_manager_frame_bytes,
          "CEIC-019 memory-manager bytes do not match page-cache frame bytes");
  RequireFrameEvidence(normal.evidence);
}

void EvictionReleasesShardedFrames() {
  auto manager = Manager(4);
  page::PageCacheLedger ledger;
  page::BindPageCacheMemoryManager(&ledger, &manager);
  const auto policy = Policy(2);
  const auto database_uuid = MakeUuid(UuidKind::database, 21);
  const auto filespace_uuid = MakeUuid(UuidKind::filespace, 22);

  Require(page::AdmitPageCacheEntry(&ledger, policy, Entry(database_uuid, filespace_uuid, 1)).ok(),
          "CEIC-019 page 1 admission failed");
  Require(page::AdmitPageCacheEntry(&ledger, policy, Entry(database_uuid, filespace_uuid, 2)).ok(),
          "CEIC-019 page 2 admission failed");
  auto third = page::AdmitPageCacheEntry(&ledger, policy, Entry(database_uuid, filespace_uuid, 3));
  Require(third.ok(), "CEIC-019 eviction admission failed");
  Require(third.snapshot.memory_manager_frame_count == 2 &&
              third.snapshot.memory_manager_frame_release_count == 1 &&
              SumShardReleases(third.snapshot) == third.snapshot.memory_manager_frame_release_count,
          "CEIC-019 eviction did not release exactly one sharded frame");
  Require(manager.Snapshot().page_buffer_current_bytes == 2ull * kPageSize,
          "CEIC-019 eviction leaked page-buffer bytes");
  RequireFrameEvidence(third.evidence);
}

void PinnedDirtyAndHotNormalFramesAreProtected() {
  auto manager = Manager(4);
  const auto database_uuid = MakeUuid(UuidKind::database, 31);
  const auto filespace_uuid = MakeUuid(UuidKind::filespace, 32);

  page::PageCacheLedger hot_ledger;
  page::BindPageCacheMemoryManager(&hot_ledger, &manager);
  const auto hot_policy = Policy(1);
  auto normal = page::AdmitPageCacheEntry(&hot_ledger, hot_policy,
                                          Entry(database_uuid, filespace_uuid, 1));
  Require(normal.ok(), "CEIC-019 hot normal setup failed");
  Require(page::PinPageCacheEntry(&hot_ledger, normal.entry.page_uuid).ok(),
          "CEIC-019 pin hot normal failed");
  Require(page::UnpinPageCacheEntry(&hot_ledger, normal.entry.page_uuid).ok(),
          "CEIC-019 unpin hot normal failed");
  auto bulk_refused = page::AdmitPageCacheEntryForContext(
      &hot_ledger, hot_policy, Entry(database_uuid, filespace_uuid, 2),
      page::PageCacheIoContext::bulk_read);
  Require(!bulk_refused.ok(), "CEIC-019 bulk evicted protected hot normal frame");
  Require(page::SnapshotPageCache(hot_ledger).memory_manager_frame_count == 1 &&
              page::SnapshotPageCache(hot_ledger).memory_manager_frame_release_count == 0,
          "CEIC-019 protected hot normal frame was released");
  Require(page::SnapshotPageCacheContext(hot_ledger,
                                         page::PageCacheIoContext::normal).resident_pages == 1,
          "CEIC-019 normal hot page lost residency");
  RequireFrameEvidence(bulk_refused.evidence);

  page::PageCacheLedger dirty_ledger;
  page::BindPageCacheMemoryManager(&dirty_ledger, &manager);
  auto dirty = page::AdmitPageCacheEntry(&dirty_ledger, hot_policy,
                                         Entry(database_uuid, filespace_uuid, 3));
  Require(dirty.ok(), "CEIC-019 dirty setup failed");
  Require(page::PinPageCacheEntry(&dirty_ledger, dirty.entry.page_uuid).ok(),
          "CEIC-019 pin dirty failed");
  Require(page::MarkPageCacheEntryDirty(&dirty_ledger, dirty.entry.page_uuid, true).ok(),
          "CEIC-019 mark dirty failed");
  auto dirty_refused = page::AdmitPageCacheEntry(&dirty_ledger, hot_policy,
                                                 Entry(database_uuid, filespace_uuid, 4));
  Require(!dirty_refused.ok(), "CEIC-019 evicted pinned dirty frame");
  Require(page::SnapshotPageCache(dirty_ledger).memory_manager_frame_count == 1 &&
              page::SnapshotPageCache(dirty_ledger).memory_manager_frame_release_count == 0,
          "CEIC-019 pinned dirty frame was released");
  RequireFrameEvidence(dirty_refused.evidence);
}

void ClusterPageCacheFailsClosedLocally() {
  auto manager = Manager(2);
  page::PageCacheLedger ledger;
  page::BindPageCacheMemoryManager(&ledger, &manager);
  const auto database_uuid = MakeUuid(UuidKind::database, 41);
  const auto filespace_uuid = MakeUuid(UuidKind::filespace, 42);
  auto input = LifecycleInput(database_uuid, filespace_uuid);
  input.standalone_mode = false;
  input.cluster_authority_available = true;

  auto result = page::StartPageCacheLifecycle(&ledger, Policy(1), input, {});
  Require(!result.ok() &&
              result.diagnostic.diagnostic_code ==
                  "CACHE.CHECKPOINT_CLUSTER_EXTERNAL_PROVIDER_REQUIRED",
          "CEIC-019 local page cache did not fail closed for cluster request");
  Require(result.publication.cluster_paths_failed_closed &&
              !result.publication.ordinary_admission_allowed,
          "CEIC-019 cluster failure did not publish fail-closed evidence");
}

}  // namespace

int main() {
  std::cout << "CEIC-019 authority_note=page_cache_frame_ownership_and_sharded_buffer_pool;"
               "not_transaction_finality_row_visibility_security_authorization_recovery_parser_donor_wal_benchmark_optimizer_plan_index_finality_or_agent_action_authority"
            << '\n';
  ShardedFrameSnapshotMergesDeterministically();
  EvictionReleasesShardedFrames();
  PinnedDirtyAndHotNormalFramesAreProtected();
  ClusterPageCacheFailsClosedLocally();
  return EXIT_SUCCESS;
}
