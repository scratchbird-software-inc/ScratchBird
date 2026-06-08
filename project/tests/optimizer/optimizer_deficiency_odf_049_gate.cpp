// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_contracts.hpp"
#include "metric_registry.hpp"
#include "page_cache.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace metrics = scratchbird::core::metrics;
namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u64;
using scratchbird::core::platform::UuidKind;
using scratchbird::storage::disk::PageType;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

u64 NowMillis() {
  return static_cast<u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

u64 UniqueSeed() {
  static u64 counter = 0;
  return NowMillis() + (++counter * 1000);
}

TypedUuid MakeUuid(UuidKind kind, u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, UniqueSeed() + salt);
  Require(generated.ok(), "ODF-049 UUID generation failed");
  return generated.value;
}

struct Ids {
  TypedUuid database_uuid = MakeUuid(UuidKind::database, 49);
  TypedUuid filespace_uuid = MakeUuid(UuidKind::filespace, 50);
};

page::PageCachePolicy Policy(u64 max_pages = 64) {
  page::PageCachePolicy policy;
  policy.max_resident_pages = max_pages;
  policy.max_resident_bytes = max_pages * 16384ull;
  policy.allow_dirty_eviction = false;
  policy.bulk_read_ring_pages = 2;
  policy.bulk_write_ring_pages = 2;
  policy.vacuum_cleanup_ring_pages = 2;
  policy.index_build_ring_pages = 2;
  policy.strict_bulk_load_ring_pages = 2;
  return policy;
}

page::PageCacheEntry Entry(const Ids& ids,
                           u64 page_number,
                           bool hot = false,
                           bool dirty = false) {
  page::PageCacheEntry entry;
  entry.database_uuid = ids.database_uuid;
  entry.filespace_uuid = ids.filespace_uuid;
  entry.page_uuid = MakeUuid(UuidKind::page, 1000 + page_number);
  entry.page_type = PageType::row_data;
  entry.page_number = page_number;
  entry.page_generation = 1;
  entry.page_size = 16384;
  entry.cache_hot = hot;
  entry.dirty = dirty;
  return entry;
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.value == right.value;
}

bool Resident(const page::PageCacheLedger& ledger, const TypedUuid& page_uuid) {
  for (const auto& entry : ledger.entries) {
    if (SameUuid(entry.page_uuid, page_uuid)) {
      return entry.resident;
    }
  }
  return false;
}

bool MetricHasLabel(const metrics::MetricValue& value,
                    std::string_view key,
                    std::string_view expected) {
  for (const auto& label : value.labels) {
    if (label.key == key && label.value == expected) {
      return true;
    }
  }
  return false;
}

double MetricValueFor(std::string_view family,
                      std::string_view context,
                      std::string_view result,
                      std::string_view reason) {
  for (const auto& value : metrics::DefaultMetricRegistry().SnapshotCurrent(true)) {
    if (value.family == family &&
        MetricHasLabel(value, "context", context) &&
        MetricHasLabel(value, "result", result) &&
        MetricHasLabel(value, "reason", reason)) {
      return value.value;
    }
  }
  return -1.0;
}

void TestContextNames() {
  const std::vector<std::pair<std::string_view, page::PageCacheIoContext>> expected = {
      {"normal", page::PageCacheIoContext::normal},
      {"bulk_read", page::PageCacheIoContext::bulk_read},
      {"bulk_write", page::PageCacheIoContext::bulk_write},
      {"vacuum_cleanup", page::PageCacheIoContext::vacuum_cleanup},
      {"index_build", page::PageCacheIoContext::index_build},
      {"strict_bulk_load", page::PageCacheIoContext::strict_bulk_load}};
  for (const auto& [name, context] : expected) {
    page::PageCacheIoContext parsed = page::PageCacheIoContext::normal;
    Require(page::PageCacheIoContextFromName(name, &parsed),
            "ODF-049 context parse failed");
    Require(parsed == context, "ODF-049 context parse mismatch");
    Require(name == page::PageCacheIoContextName(context),
            "ODF-049 context name mismatch");
  }
  page::PageCacheIoContext parsed = page::PageCacheIoContext::normal;
  Require(!page::PageCacheIoContextFromName("docs" "/execution-plans", &parsed),
          "ODF-049 accepted non-runtime context name");
  Require(!page::PageCacheIoContextUsesBoundedRing(page::PageCacheIoContext::normal),
          "ODF-049 normal context unexpectedly used a bounded ring");
  Require(page::PageCacheIoContextUsesBoundedRing(page::PageCacheIoContext::bulk_read),
          "ODF-049 bulk_read did not use a bounded ring");
  Require(page::PageCacheIoContextUsesBoundedRing(page::PageCacheIoContext::bulk_write),
          "ODF-049 bulk_write did not use a bounded ring");
  Require(page::PageCacheIoContextUsesBoundedRing(page::PageCacheIoContext::strict_bulk_load),
          "ODF-049 strict_bulk_load did not use a bounded ring");
}

void TestBoundedRing(page::PageCacheIoContext context) {
  const Ids ids;
  page::PageCacheLedger ledger;
  const auto policy = Policy(64);

  for (u64 index = 0; index < 4; ++index) {
    const auto admitted = page::AdmitPageCacheEntryForContext(
        &ledger, policy, Entry(ids, 10 + index), context);
    Require(admitted.ok(), "ODF-049 bounded ring admission failed");
  }

  const auto snapshot = page::SnapshotPageCacheContext(ledger, context);
  Require(snapshot.resident_pages == 2, "ODF-049 bounded ring resident count mismatch");
  Require(snapshot.admissions == 4, "ODF-049 bounded ring admission count mismatch");
  Require(snapshot.reuses == 2, "ODF-049 bounded ring reuse count mismatch");
  Require(snapshot.evictions == 2, "ODF-049 bounded ring eviction count mismatch");
}

void TestNormalHotProtection() {
  const Ids ids;
  page::PageCacheLedger ledger;
  const auto policy = Policy(3);
  const auto normal_a = Entry(ids, 100, true);
  const auto normal_b = Entry(ids, 101, true);
  const auto bulk_a = Entry(ids, 102);
  const auto bulk_b = Entry(ids, 103);

  Require(page::AdmitPageCacheEntry(&ledger, policy, normal_a).ok(),
          "ODF-049 normal hot admission A failed");
  Require(page::AdmitPageCacheEntry(&ledger, policy, normal_b).ok(),
          "ODF-049 normal hot admission B failed");
  Require(page::AdmitPageCacheEntryForContext(&ledger,
                                              policy,
                                              bulk_a,
                                              page::PageCacheIoContext::bulk_read).ok(),
          "ODF-049 initial bulk admission failed");
  Require(page::AdmitPageCacheEntryForContext(&ledger,
                                              policy,
                                              bulk_b,
                                              page::PageCacheIoContext::bulk_read).ok(),
          "ODF-049 churn bulk admission failed");

  Require(Resident(ledger, normal_a.page_uuid), "ODF-049 evicted normal hot page A");
  Require(Resident(ledger, normal_b.page_uuid), "ODF-049 evicted normal hot page B");
  Require(!Resident(ledger, bulk_a.page_uuid), "ODF-049 did not reuse bulk-owned page");
  Require(Resident(ledger, bulk_b.page_uuid), "ODF-049 did not admit replacement bulk page");
  const auto bulk = page::SnapshotPageCacheContext(ledger, page::PageCacheIoContext::bulk_read);
  Require(bulk.protected_normal_hot_skips >= 2,
          "ODF-049 did not count protected normal hot skips");
  Require(bulk.evictions >= 1, "ODF-049 did not count bulk-owned eviction");
}

void TestPinnedAndDirtyRefusals() {
  const Ids ids;
  {
    page::PageCacheLedger ledger;
    auto policy = Policy(64);
    policy.strict_bulk_load_ring_pages = 1;
    const auto first = Entry(ids, 200);
    const auto second = Entry(ids, 201);
    Require(page::AdmitPageCacheEntryForContext(&ledger,
                                                policy,
                                                first,
                                                page::PageCacheIoContext::strict_bulk_load).ok(),
            "ODF-049 strict bulk initial admission failed");
    Require(page::PinPageCacheEntry(&ledger, first.page_uuid).ok(),
            "ODF-049 pin failed");
    const auto refused = page::AdmitPageCacheEntryForContext(
        &ledger, policy, second, page::PageCacheIoContext::strict_bulk_load);
    Require(!refused.ok(), "ODF-049 strict bulk evicted a pinned ring page");
    Require(refused.diagnostic.diagnostic_code ==
                "page_cache_context_ring_exhausted_pinned_or_dirty",
            "ODF-049 pinned refusal diagnostic mismatch");
    Require(Resident(ledger, first.page_uuid),
            "ODF-049 pinned page was evicted after refusal");
    Require(!Resident(ledger, second.page_uuid),
            "ODF-049 refused pinned-ring admission still became resident");
  }

  {
    page::PageCacheLedger ledger;
    auto policy = Policy(64);
    policy.bulk_write_ring_pages = 1;
    const auto first = Entry(ids, 300, false, true);
    const auto second = Entry(ids, 301);
    Require(page::AdmitPageCacheEntryForContext(&ledger,
                                                policy,
                                                first,
                                                page::PageCacheIoContext::bulk_write).ok(),
            "ODF-049 dirty bulk initial admission failed");
    const auto refused = page::AdmitPageCacheEntryForContext(
        &ledger, policy, second, page::PageCacheIoContext::bulk_write);
    Require(!refused.ok(), "ODF-049 bulk write evicted a dirty ring page");
    Require(Resident(ledger, first.page_uuid),
            "ODF-049 dirty page was evicted after refusal");
    Require(!Resident(ledger, second.page_uuid),
            "ODF-049 refused dirty-ring admission still became resident");
  }
}

void TestMetrics() {
  const Ids ids;
  page::PageCacheLedger ledger;
  auto policy = Policy(64);
  policy.index_build_ring_pages = 1;
  Require(page::AdmitPageCacheEntryForContext(&ledger,
                                              policy,
                                              Entry(ids, 400),
                                              page::PageCacheIoContext::index_build).ok(),
          "ODF-049 index-build metric admission A failed");
  Require(page::AdmitPageCacheEntryForContext(&ledger,
                                              policy,
                                              Entry(ids, 401),
                                              page::PageCacheIoContext::index_build).ok(),
          "ODF-049 index-build metric admission B failed");

  const auto contracts = metrics::MetricProducerContractsForOwner("storage_page");
  bool saw_context_counter = false;
  for (const auto& contract : contracts) {
    if (contract.family == "sb_page_cache_context_admissions_total") {
      saw_context_counter = true;
      break;
    }
  }
  Require(saw_context_counter, "ODF-049 page-cache context metric contract missing");

  Require(MetricValueFor("sb_page_cache_context_admissions_total",
                         "index_build",
                         "ok",
                         "admit") >= 2.0,
          "ODF-049 admission metric missing");
  Require(MetricValueFor("sb_page_cache_context_reuses_total",
                         "index_build",
                         "ok",
                         "ring_or_slot_reuse") >= 1.0,
          "ODF-049 reuse metric missing");
  Require(MetricValueFor("sb_page_cache_context_evictions_total",
                         "index_build",
                         "evicted",
                         "budget_or_ring") >= 1.0,
          "ODF-049 eviction metric missing");
  Require(MetricValueFor("sb_page_cache_context_resident_pages",
                         "index_build",
                         "current",
                         "snapshot") >= 1.0,
          "ODF-049 context snapshot metric missing");
  Require(MetricValueFor("sb_page_cache_context_protected_normal_hot_skips_total",
                         "bulk_read",
                         "protected",
                         "bulk_context_budget") >= 2.0,
          "ODF-049 protected-skip metric missing");
}

}  // namespace

int main() {
  TestContextNames();
  TestBoundedRing(page::PageCacheIoContext::bulk_read);
  TestBoundedRing(page::PageCacheIoContext::bulk_write);
  TestBoundedRing(page::PageCacheIoContext::strict_bulk_load);
  TestNormalHotProtection();
  TestPinnedAndDirtyRefusals();
  TestMetrics();
  return EXIT_SUCCESS;
}
