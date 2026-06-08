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
          "MMCH-052 frame ownership marker missing");
  Require(EvidenceHas(
              evidence,
              "page_cache.frame_authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority"),
          "MMCH-052 authority evidence missing");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

TypedUuid MakeUuid(UuidKind kind, std::uint64_t offset) {
  auto generated = uuid::GenerateEngineIdentityV7(kind, CurrentUnixMillis() + offset);
  Require(generated.ok(), "MMCH-052 UUID generation failed");
  return generated.value;
}

mem::MemoryManager Manager(std::uint64_t pages) {
  auto policy = mem::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "mmch052_page_cache_pressure";
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
  entry.page_uuid = MakeUuid(UuidKind::page, 100 + index);
  entry.page_type = PageType::row_data;
  entry.page_number = index;
  entry.page_generation = 1;
  entry.page_size = kPageSize;
  return entry;
}

page::PageCacheLifecycleInput LifecycleInput(const TypedUuid& database_uuid,
                                             const TypedUuid& filespace_uuid,
                                             std::uint64_t target_pages) {
  page::PageCacheLifecycleInput input;
  input.database_uuid = database_uuid;
  input.filespace_uuid = filespace_uuid;
  input.database_lifecycle_state = "opened";
  input.policy_generation = 52;
  input.checkpoint_generation = 520;
  input.target_resident_pages = target_pages;
  input.tx2_activation_committed = true;
  input.cache_runtime_started = true;
  input.engine_agent_active = true;
  input.writeback_allowed = true;
  input.checkpoint_allowed = true;
  input.standalone_mode = true;
  input.cluster_authority_available = false;
  return input;
}

void ResidentBudgetEvictsAndReleasesFrames() {
  auto manager = Manager(4);
  page::PageCacheLedger ledger;
  page::BindPageCacheMemoryManager(&ledger, &manager);
  const auto policy = Policy(2);
  const auto database_uuid = MakeUuid(UuidKind::database, 1);
  const auto filespace_uuid = MakeUuid(UuidKind::filespace, 2);

  Require(page::AdmitPageCacheEntry(&ledger, policy, Entry(database_uuid, filespace_uuid, 1)).ok(),
          "MMCH-052 setup page 1 failed");
  Require(page::AdmitPageCacheEntry(&ledger, policy, Entry(database_uuid, filespace_uuid, 2)).ok(),
          "MMCH-052 setup page 2 failed");
  auto third = page::AdmitPageCacheEntry(&ledger, policy, Entry(database_uuid, filespace_uuid, 3));
  Require(third.ok(), "MMCH-052 third admission failed");
  Require(third.snapshot.resident_pages == 2 &&
              third.snapshot.resident_bytes == 2ull * kPageSize &&
              third.snapshot.memory_manager_frame_release_count == 1,
          "MMCH-052 resident budget did not evict/release deterministically");
  Require(manager.Snapshot().page_buffer_current_bytes == 2ull * kPageSize,
          "MMCH-052 resident budget leaked page-buffer bytes");
  RequireFrameEvidence(third.evidence);
}

void MemoryPressureShrinksCleanFrames() {
  auto manager = Manager(4);
  page::PageCacheLedger ledger;
  page::BindPageCacheMemoryManager(&ledger, &manager);
  auto admit_policy = Policy(3);
  const auto pressure_policy = Policy(1);
  const auto database_uuid = MakeUuid(UuidKind::database, 11);
  const auto filespace_uuid = MakeUuid(UuidKind::filespace, 12);

  Require(page::AdmitPageCacheEntry(&ledger, admit_policy, Entry(database_uuid, filespace_uuid, 1)).ok(),
          "MMCH-052 pressure page 1 failed");
  Require(page::AdmitPageCacheEntry(&ledger, admit_policy, Entry(database_uuid, filespace_uuid, 2)).ok(),
          "MMCH-052 pressure page 2 failed");
  Require(page::AdmitPageCacheEntry(&ledger, admit_policy, Entry(database_uuid, filespace_uuid, 3)).ok(),
          "MMCH-052 pressure page 3 failed");
  auto pressure = page::ApplyPageCacheMemoryPressure(
      &ledger,
      pressure_policy,
      LifecycleInput(database_uuid, filespace_uuid, 1));
  Require(pressure.ok() && pressure.publication.memory_pressure_handled,
          "MMCH-052 memory pressure was not handled");
  Require(pressure.snapshot.resident_pages == 1 &&
              pressure.snapshot.memory_manager_frame_count == 1 &&
              pressure.evicted_pages == 2,
          "MMCH-052 memory pressure did not shrink clean frames");
  Require(manager.Snapshot().page_buffer_current_bytes == kPageSize,
          "MMCH-052 memory pressure did not release page-buffer bytes");
  RequireFrameEvidence(pressure.evidence);
}

void BulkRingCannotGrowUnbounded() {
  auto manager = Manager(4);
  page::PageCacheLedger ledger;
  page::BindPageCacheMemoryManager(&ledger, &manager);
  auto policy = Policy(4);
  policy.bulk_read_ring_pages = 2;
  const auto database_uuid = MakeUuid(UuidKind::database, 21);
  const auto filespace_uuid = MakeUuid(UuidKind::filespace, 22);

  Require(page::AdmitPageCacheEntryForContext(&ledger, policy, Entry(database_uuid, filespace_uuid, 1), page::PageCacheIoContext::bulk_read).ok(),
          "MMCH-052 bulk page 1 failed");
  Require(page::AdmitPageCacheEntryForContext(&ledger, policy, Entry(database_uuid, filespace_uuid, 2), page::PageCacheIoContext::bulk_read).ok(),
          "MMCH-052 bulk page 2 failed");
  auto third = page::AdmitPageCacheEntryForContext(
      &ledger,
      policy,
      Entry(database_uuid, filespace_uuid, 3),
      page::PageCacheIoContext::bulk_read);
  Require(third.ok(), "MMCH-052 bulk ring replacement failed");
  const auto bulk = page::SnapshotPageCacheContext(ledger, page::PageCacheIoContext::bulk_read);
  Require(bulk.resident_pages == 2 &&
              bulk.reuses == 1 &&
              third.snapshot.memory_manager_frame_count == 2,
          "MMCH-052 bulk ring did not stay bounded");
  Require(manager.Snapshot().page_buffer_current_bytes == 2ull * kPageSize,
          "MMCH-052 bulk ring leaked page-buffer bytes");
}

void HotNormalAndPinnedDirtyProtection() {
  auto manager = Manager(3);
  page::PageCacheLedger ledger;
  page::BindPageCacheMemoryManager(&ledger, &manager);
  const auto policy = Policy(1);
  const auto database_uuid = MakeUuid(UuidKind::database, 31);
  const auto filespace_uuid = MakeUuid(UuidKind::filespace, 32);

  auto normal = page::AdmitPageCacheEntry(&ledger, policy, Entry(database_uuid, filespace_uuid, 1));
  Require(normal.ok(), "MMCH-052 normal hot setup failed");
  Require(page::PinPageCacheEntry(&ledger, normal.entry.page_uuid).ok(),
          "MMCH-052 pin normal failed");
  Require(page::UnpinPageCacheEntry(&ledger, normal.entry.page_uuid).ok(),
          "MMCH-052 unpin normal failed");
  auto bulk_refused = page::AdmitPageCacheEntryForContext(
      &ledger,
      policy,
      Entry(database_uuid, filespace_uuid, 2),
      page::PageCacheIoContext::bulk_read);
  Require(!bulk_refused.ok(),
          "MMCH-052 bulk admission evicted protected hot normal page");
  Require(page::SnapshotPageCache(ledger).resident_pages == 1 &&
              manager.Snapshot().page_buffer_current_bytes == kPageSize,
          "MMCH-052 protected hot refusal changed residency or leaked memory");
  RequireFrameEvidence(bulk_refused.evidence);

  page::PageCacheLedger dirty_ledger;
  page::BindPageCacheMemoryManager(&dirty_ledger, &manager);
  const auto dirty_admit_policy = Policy(2);
  const auto dirty_pressure_policy = Policy(1);
  auto dirty = page::AdmitPageCacheEntry(
      &dirty_ledger,
      dirty_admit_policy,
      Entry(database_uuid, filespace_uuid, 3));
  Require(dirty.ok(), "MMCH-052 dirty setup failed");
  auto dirty_second = page::AdmitPageCacheEntry(
      &dirty_ledger,
      dirty_admit_policy,
      Entry(database_uuid, filespace_uuid, 4));
  Require(dirty_second.ok(), "MMCH-052 second dirty setup failed");
  Require(page::PinPageCacheEntry(&dirty_ledger, dirty.entry.page_uuid).ok(),
          "MMCH-052 dirty pin failed");
  Require(page::PinPageCacheEntry(&dirty_ledger, dirty_second.entry.page_uuid).ok(),
          "MMCH-052 second dirty pin failed");
  Require(page::MarkPageCacheEntryDirty(&dirty_ledger, dirty.entry.page_uuid, true).ok(),
          "MMCH-052 dirty mark failed");
  Require(page::MarkPageCacheEntryDirty(&dirty_ledger, dirty_second.entry.page_uuid, true).ok(),
          "MMCH-052 second dirty mark failed");
  auto dirty_refused = page::ApplyPageCacheMemoryPressure(
      &dirty_ledger,
      dirty_pressure_policy,
      LifecycleInput(database_uuid, filespace_uuid, 1));
  Require(!dirty_refused.ok() &&
              dirty_refused.diagnostic.diagnostic_code ==
                  "CACHE.CHECKPOINT_MEMORY_PRESSURE_PINNED",
          "MMCH-052 pinned dirty pressure was not refused");
  Require(page::SnapshotPageCache(dirty_ledger).pinned_pages == 2 &&
              page::SnapshotPageCache(dirty_ledger).dirty_pages == 0,
          "MMCH-052 pressure did not flush but retain pinned dirty page");
}

}  // namespace

int main() {
  std::cout << "MMCH-052 authority_note=page_cache_pressure_evidence_only;"
               "not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority"
            << '\n';
  ResidentBudgetEvictsAndReleasesFrames();
  MemoryPressureShrinksCleanFrames();
  BulkRingCannotGrowUnbounded();
  HotNormalAndPinnedDirtyProtection();
  return EXIT_SUCCESS;
}
