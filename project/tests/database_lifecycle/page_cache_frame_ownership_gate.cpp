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
          "MMCH-050 page-cache frame ownership evidence missing");
  Require(EvidenceHas(
              evidence,
              "page_cache.frame_authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority"),
          "MMCH-050 page-cache authority boundary evidence missing");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

TypedUuid MakeUuid(UuidKind kind, std::uint64_t offset) {
  auto generated = uuid::GenerateEngineIdentityV7(kind, CurrentUnixMillis() + offset);
  Require(generated.ok(), "MMCH-050 UUID generation failed");
  return generated.value;
}

mem::AllocationPolicy MemoryPolicy(std::uint64_t page_budget_bytes) {
  auto policy = mem::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "mmch050_page_cache_frames";
  policy.hard_limit_bytes = page_budget_bytes;
  policy.soft_limit_bytes = page_budget_bytes;
  policy.per_context_limit_bytes = page_budget_bytes;
  policy.page_buffer_pool_limit_bytes = page_budget_bytes;
  policy.reject_over_soft_limit = false;
  return policy;
}

page::PageCachePolicy CachePolicy(std::uint64_t page_count) {
  page::PageCachePolicy policy;
  policy.max_resident_pages = page_count;
  policy.max_resident_bytes = page_count * kPageSize;
  policy.allow_dirty_eviction = false;
  policy.require_memory_manager_frames = true;
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

void AdmissionAndEvictionOwnFrames() {
  mem::MemoryManager manager(MemoryPolicy(4ull * kPageSize));
  page::PageCacheLedger ledger;
  page::BindPageCacheMemoryManager(&ledger, &manager);
  const auto policy = CachePolicy(2);
  const auto database_uuid = MakeUuid(UuidKind::database, 1);
  const auto filespace_uuid = MakeUuid(UuidKind::filespace, 2);

  auto first = page::AdmitPageCacheEntry(&ledger, policy, Entry(database_uuid, filespace_uuid, 1));
  Require(first.ok() && first.snapshot.memory_manager_frame_count == 1,
          "MMCH-050 first page did not allocate a memory-manager frame");
  RequireFrameEvidence(first.evidence);
  auto second = page::AdmitPageCacheEntry(&ledger, policy, Entry(database_uuid, filespace_uuid, 2));
  Require(second.ok() && second.snapshot.memory_manager_frame_count == 2,
          "MMCH-050 second page did not allocate a memory-manager frame");
  Require(manager.Snapshot().page_buffer_current_bytes == 2ull * kPageSize,
          "MMCH-050 page-buffer bytes were not charged to MemoryManager");

  auto third = page::AdmitPageCacheEntry(&ledger, policy, Entry(database_uuid, filespace_uuid, 3));
  Require(third.ok(), "MMCH-050 budget eviction admission failed");
  Require(third.snapshot.resident_pages == 2 &&
              third.snapshot.memory_manager_frame_count == 2 &&
              third.snapshot.memory_manager_frame_release_count == 1,
          "MMCH-050 eviction did not release exactly one frame");
  Require(manager.Snapshot().page_buffer_current_bytes == 2ull * kPageSize,
          "MMCH-050 eviction leaked or over-released page-buffer bytes");

  auto evicted = page::EvictOnePageCacheEntry(&ledger, policy);
  Require(evicted.ok() && evicted.evicted,
          "MMCH-050 explicit eviction failed");
  Require(evicted.snapshot.memory_manager_frame_count == 1 &&
              evicted.snapshot.memory_manager_frame_bytes == kPageSize,
          "MMCH-050 explicit eviction did not release frame ownership");
  Require(manager.Snapshot().page_buffer_current_bytes == kPageSize,
          "MMCH-050 explicit eviction did not release MemoryManager bytes");
  RequireFrameEvidence(evicted.evidence);
}

void PinnedDirtyPagesRemainProtected() {
  mem::MemoryManager manager(MemoryPolicy(3ull * kPageSize));
  page::PageCacheLedger ledger;
  page::BindPageCacheMemoryManager(&ledger, &manager);
  auto policy = CachePolicy(1);
  const auto database_uuid = MakeUuid(UuidKind::database, 11);
  const auto filespace_uuid = MakeUuid(UuidKind::filespace, 12);

  auto first = page::AdmitPageCacheEntry(&ledger, policy, Entry(database_uuid, filespace_uuid, 1));
  Require(first.ok(), "MMCH-050 pinned setup admission failed");
  Require(page::PinPageCacheEntry(&ledger, first.entry.page_uuid).ok(),
          "MMCH-050 pin failed");
  Require(page::MarkPageCacheEntryDirty(&ledger, first.entry.page_uuid, true).ok(),
          "MMCH-050 dirty mark failed");

  auto refused = page::AdmitPageCacheEntry(&ledger, policy, Entry(database_uuid, filespace_uuid, 2));
  Require(!refused.ok(),
          "MMCH-050 admission evicted a pinned dirty page");
  Require(refused.diagnostic.diagnostic_code ==
              "page_cache_budget_exhausted_pinned_or_dirty",
          "MMCH-050 pinned dirty diagnostic changed");
  Require(manager.Snapshot().page_buffer_current_bytes == kPageSize,
          "MMCH-050 refused admission leaked temporary page frame");
  Require(page::SnapshotPageCache(ledger).memory_manager_frame_count == 1,
          "MMCH-050 pinned dirty refusal changed frame residency");
  RequireFrameEvidence(refused.evidence);
}

void FrameAllocationFailureFailsClosed() {
  mem::MemoryManager manager(MemoryPolicy(kPageSize));
  page::PageCacheLedger ledger;
  page::BindPageCacheMemoryManager(&ledger, &manager);
  const auto policy = CachePolicy(2);
  const auto database_uuid = MakeUuid(UuidKind::database, 21);
  const auto filespace_uuid = MakeUuid(UuidKind::filespace, 22);

  auto first = page::AdmitPageCacheEntry(&ledger, policy, Entry(database_uuid, filespace_uuid, 1));
  Require(first.ok(), "MMCH-050 setup admission failed");
  auto second = page::AdmitPageCacheEntry(&ledger, policy, Entry(database_uuid, filespace_uuid, 2));
  Require(!second.ok(),
          "MMCH-050 admitted page without MemoryManager frame capacity");
  Require(page::SnapshotPageCache(ledger).memory_manager_frame_allocation_failure_count == 1,
          "MMCH-050 frame allocation failure count missing");
  Require(page::SnapshotPageCache(ledger).memory_manager_frame_count == 1,
          "MMCH-050 failed frame admission changed residency");
  Require(manager.Snapshot().page_buffer_current_bytes == kPageSize,
          "MMCH-050 failed frame admission leaked page-buffer bytes");
  RequireFrameEvidence(second.evidence);
}

}  // namespace

int main() {
  std::cout << "MMCH-050 authority_note=page_cache_frame_ownership_evidence_only;"
               "not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority"
            << '\n';
  AdmissionAndEvictionOwnFrames();
  PinnedDirtyPagesRemainProtected();
  FrameAllocationFailureFailsClosed();
  return EXIT_SUCCESS;
}
