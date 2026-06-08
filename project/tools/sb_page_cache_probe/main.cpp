// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_cache.hpp"
#include "page_manager.hpp"
#include "uuid.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
  }
  return true;
}

scratchbird::core::platform::TypedUuid Id(scratchbird::core::platform::UuidKind kind,
                                          scratchbird::core::platform::u64 seed) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, seed);
  return generated.ok() ? generated.value : scratchbird::core::platform::TypedUuid{};
}

scratchbird::storage::page::PageCacheEntry Entry(scratchbird::core::platform::u64 seed,
                                                 scratchbird::core::platform::u32 page_size = 16 * 1024) {
  scratchbird::storage::page::PageCacheEntry entry;
  entry.database_uuid = Id(scratchbird::core::platform::UuidKind::database, 30000);
  entry.filespace_uuid = Id(scratchbird::core::platform::UuidKind::filespace, 30001);
  entry.page_uuid = Id(scratchbird::core::platform::UuidKind::page, seed);
  entry.page_type = scratchbird::storage::disk::PageType::row_data;
  entry.page_number = seed;
  entry.page_generation = 1;
  entry.page_size = page_size;
  return entry;
}

}  // namespace

int main() {
  using namespace scratchbird::storage::page;

  bool ok = true;
  PageCacheLedger ledger;
  PageCachePolicy policy;
  policy.max_resident_pages = 2;
  policy.max_resident_bytes = 32 * 1024;
  policy.allow_dirty_eviction = false;

  const auto first = AdmitPageCacheEntry(&ledger, policy, Entry(30010));
  ok &= Require(first.ok(), "first page admitted");
  const auto second = AdmitPageCacheEntry(&ledger, policy, Entry(30011));
  ok &= Require(second.ok(), "second page admitted");
  ok &= Require(SnapshotPageCache(ledger).resident_pages == 2, "two pages resident");

  const auto pin_first = PinPageCacheEntry(&ledger, first.entry.page_uuid);
  ok &= Require(pin_first.ok() && pin_first.entry.pin_count == 1, "pin first page");
  const auto dirty_second = MarkPageCacheEntryDirty(&ledger, second.entry.page_uuid, true);
  ok &= Require(dirty_second.ok() && dirty_second.entry.dirty, "mark second dirty");

  const auto blocked = AdmitPageCacheEntry(&ledger, policy, Entry(30012));
  ok &= Require(!blocked.ok(), "admission blocked when all pages pinned or dirty");
  ok &= Require(blocked.diagnostic.diagnostic_code == "page_cache_budget_exhausted_pinned_or_dirty",
                "budget blocked diagnostic");

  const auto unpin_first = UnpinPageCacheEntry(&ledger, first.entry.page_uuid);
  ok &= Require(unpin_first.ok(), "unpin first page");
  const auto third = AdmitPageCacheEntry(&ledger, policy, Entry(30012));
  ok &= Require(third.ok(), "third page admitted after eviction");
  ok &= Require(SnapshotPageCache(ledger).resident_pages == 2, "resident count remains bounded");

  PageCachePolicy dirty_policy = policy;
  dirty_policy.allow_dirty_eviction = true;
  const auto explicit_evict = EvictOnePageCacheEntry(&ledger, dirty_policy);
  ok &= Require(explicit_evict.ok() && explicit_evict.evicted, "explicit eviction succeeds with dirty policy");
  const auto readmit = AdmitPageCacheEntry(&ledger, dirty_policy, Entry(explicit_evict.entry.page_number));
  ok &= Require(readmit.ok(), "nonresident evicted page can be readmitted");

  const std::vector<scratchbird::core::platform::u32> supported_page_sizes = {
      1024, 2048, 4096, 8192, 16 * 1024, 32 * 1024, 64 * 1024, 128 * 1024,
      256 * 1024, 512 * 1024, 1024 * 1024};
  PageCacheLedger page_size_ledger;
  PageCachePolicy page_size_policy;
  page_size_policy.max_resident_pages = supported_page_sizes.size();
  page_size_policy.max_resident_bytes = 2 * 1024 * 1024;
  for (std::size_t i = 0; i < supported_page_sizes.size(); ++i) {
    const auto admitted = AdmitPageCacheEntry(&page_size_ledger,
                                              page_size_policy,
                                              Entry(30100 + i, supported_page_sizes[i]));
    ok &= Require(admitted.ok(), "supported page size admitted to page cache");
  }

  ManagedPageQuarantineLedger quarantine_ledger;
  scratchbird::storage::disk::SerializedPageHeader first_header;
  PageManagerContext first_context;
  for (std::size_t i = 0; i < supported_page_sizes.size(); ++i) {
    PageManagerContext context;
    context.page_size = supported_page_sizes[i];
    context.database_uuid = Id(scratchbird::core::platform::UuidKind::database, 31000 + i);
    context.filespace_uuid = Id(scratchbird::core::platform::UuidKind::filespace, 32000 + i);
    ManagedPageHeaderRequest header_request;
    header_request.context = context;
    header_request.page_type = scratchbird::storage::disk::PageType::row_data;
    header_request.page_uuid = Id(scratchbird::core::platform::UuidKind::page, 33000 + i);
    header_request.page_number = 1 + i;
    const auto header = BuildManagedPageHeader(header_request);
    ok &= Require(header.ok(), "managed page header builds for supported page size");
    const auto safe = QuarantineManagedPageIfUnsafe(&quarantine_ledger, context, header.serialized);
    ok &= Require(safe.ok() && !safe.quarantined, "valid page is not quarantined for supported page size");
    if (i == 0) {
      first_header = header.serialized;
      first_context = context;
    }
  }

  auto corrupt = first_header;
  corrupt[0] = 0;
  const auto quarantined = QuarantineManagedPageIfUnsafe(&quarantine_ledger, first_context, corrupt);
  ok &= Require(!quarantined.ok() && quarantined.quarantined, "corrupt page quarantined");
  ok &= Require(quarantined.evidence.reason == ManagedPageQuarantineReason::invalid_magic,
                "invalid magic quarantine reason");
  ok &= Require(quarantine_ledger.evidence.size() == supported_page_sizes.size() + 1, "quarantine evidence recorded");

  return ok ? 0 : 1;
}
