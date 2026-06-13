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
#include <utility>
#include <vector>

namespace {

namespace metrics = scratchbird::core::metrics;
namespace memory = scratchbird::core::memory;
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
  Require(generated.ok(), "ODF-055 UUID generation failed");
  return generated.value;
}

struct Ids {
  TypedUuid database_uuid = MakeUuid(UuidKind::database, 55);
  TypedUuid filespace_uuid = MakeUuid(UuidKind::filespace, 56);
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

memory::MemoryManager Manager(u64 pages) {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "odf055_page_cache_scan_resistant_fixture";
  policy.hard_limit_bytes = pages * 16384ull;
  policy.soft_limit_bytes = pages * 16384ull;
  policy.per_context_limit_bytes = pages * 16384ull;
  policy.page_buffer_pool_limit_bytes = pages * 16384ull;
  policy.reject_over_soft_limit = false;
  return memory::MemoryManager(policy);
}

struct CacheFixture {
  memory::MemoryManager manager;
  page::PageCacheLedger ledger;

  explicit CacheFixture(u64 pages) : manager(Manager(pages)) {
    page::BindPageCacheMemoryManager(&ledger, &manager);
  }
};

page::PageCacheEntry Entry(const Ids& ids,
                           u64 page_number,
                           bool hot = false,
                           bool dirty = false) {
  page::PageCacheEntry entry;
  entry.database_uuid = ids.database_uuid;
  entry.filespace_uuid = ids.filespace_uuid;
  entry.page_uuid = MakeUuid(UuidKind::page, 5000 + page_number);
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

const page::PageCacheEntry* FindEntry(const page::PageCacheLedger& ledger,
                                      const TypedUuid& page_uuid) {
  for (const auto& entry : ledger.entries) {
    if (SameUuid(entry.page_uuid, page_uuid)) {
      return &entry;
    }
  }
  return nullptr;
}

bool Resident(const page::PageCacheLedger& ledger, const TypedUuid& page_uuid) {
  const auto* entry = FindEntry(ledger, page_uuid);
  return entry != nullptr && entry->resident;
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

bool ContainsForbiddenRuntimeToken(std::string_view value) {
  constexpr std::string_view tokens[] = {
      "docs" "/execution-plans",
      "public_release_evidence",
      "docs" "/findings",
      "docs/references",
      "execution_plan",
      "contract",
      "finding",
      "reference"};
  for (const auto token : tokens) {
    if (value.find(token) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

void RequireNoForbiddenDiagnosticToken(const page::PageCacheResult& result,
                                       std::string_view label) {
  Require(!ContainsForbiddenRuntimeToken(result.diagnostic.diagnostic_code), label);
  Require(!ContainsForbiddenRuntimeToken(result.diagnostic.message_key), label);
  Require(!ContainsForbiddenRuntimeToken(result.diagnostic.source_component), label);
  for (const auto& argument : result.diagnostic.arguments) {
    Require(!ContainsForbiddenRuntimeToken(argument.key), label);
    Require(!ContainsForbiddenRuntimeToken(argument.value), label);
  }
}

void TestBulkReadChurnUsesOwnRingAndKeepsNormalHot() {
  const Ids ids;
  CacheFixture cache(64);
  auto& ledger = cache.ledger;
  const auto policy = Policy(4);
  const auto normal_a = Entry(ids, 10, true);
  const auto normal_b = Entry(ids, 11, true);

  const auto admitted_a = page::AdmitPageCacheEntry(&ledger, policy, normal_a);
  Require(admitted_a.ok(),
          "ODF-055 normal hot admission A failed: " +
              admitted_a.diagnostic.diagnostic_code);
  const auto admitted_b = page::AdmitPageCacheEntry(&ledger, policy, normal_b);
  Require(admitted_b.ok(),
          "ODF-055 normal hot admission B failed: " +
              admitted_b.diagnostic.diagnostic_code);
  for (u64 index = 0; index < 10; ++index) {
    const auto admitted = page::AdmitPageCacheEntryForContext(
        &ledger, policy, Entry(ids, 100 + index), page::PageCacheIoContext::bulk_read);
    Require(admitted.ok(), "ODF-055 bulk_read churn admission failed");
  }

  Require(Resident(ledger, normal_a.page_uuid), "ODF-055 bulk_read evicted normal hot page A");
  Require(Resident(ledger, normal_b.page_uuid), "ODF-055 bulk_read evicted normal hot page B");
  const auto bulk = page::SnapshotPageCacheContext(ledger, page::PageCacheIoContext::bulk_read);
  Require(bulk.resident_pages == 2, "ODF-055 bulk_read ring resident count mismatch");
  Require(bulk.admissions == 10, "ODF-055 bulk_read admissions not counted");
  Require(bulk.reuses >= 8, "ODF-055 bulk_read ring reuse not counted");
  Require(bulk.evictions >= 8, "ODF-055 bulk_read ring evictions not counted");
}

void TestProtectedNormalHotBudgetRefusal() {
  const Ids ids;
  CacheFixture cache(64);
  auto& ledger = cache.ledger;
  const auto policy = Policy(2);
  const auto normal_a = Entry(ids, 20, true);
  const auto normal_b = Entry(ids, 21, true);

  Require(page::AdmitPageCacheEntry(&ledger, policy, normal_a).ok(),
          "ODF-055 protected normal admission A failed");
  Require(page::AdmitPageCacheEntry(&ledger, policy, normal_b).ok(),
          "ODF-055 protected normal admission B failed");
  const auto refused = page::AdmitPageCacheEntryForContext(
      &ledger, policy, Entry(ids, 22), page::PageCacheIoContext::bulk_read);

  Require(!refused.ok(), "ODF-055 bulk_read evicted protected normal pages");
  Require(refused.diagnostic.diagnostic_code == "page_cache_budget_exhausted_pinned_or_dirty",
          "ODF-055 protected refusal diagnostic mismatch");
  Require(Resident(ledger, normal_a.page_uuid), "ODF-055 protected normal A not resident");
  Require(Resident(ledger, normal_b.page_uuid), "ODF-055 protected normal B not resident");
  const auto bulk = page::SnapshotPageCacheContext(ledger, page::PageCacheIoContext::bulk_read);
  Require(bulk.protected_normal_hot_skips >= 2,
          "ODF-055 protected normal skip counter missing");
  Require(bulk.refusals >= 1, "ODF-055 protected refusal counter missing");
  RequireNoForbiddenDiagnosticToken(refused, "ODF-055 protected refusal leaked docs token");
}

void TestScanLaneRings() {
  const Ids ids;
  const std::vector<std::pair<page::PageCacheIoContext, std::string_view>> contexts = {
      {page::PageCacheIoContext::bulk_write, "bulk_write"},
      {page::PageCacheIoContext::vacuum_cleanup, "vacuum_cleanup"},
      {page::PageCacheIoContext::index_build, "index_build"},
      {page::PageCacheIoContext::strict_bulk_load, "strict_bulk_load"}};

  for (const auto& [context, name] : contexts) {
    CacheFixture cache(64);
    auto& ledger = cache.ledger;
    const auto policy = Policy(64);
    for (u64 index = 0; index < 5; ++index) {
      const auto admitted = page::AdmitPageCacheEntryForContext(
          &ledger, policy, Entry(ids, 1000 + static_cast<u64>(context) * 100 + index), context);
      Require(admitted.ok(), "ODF-055 scan-lane ring admission failed");
    }
    const auto snapshot = page::SnapshotPageCacheContext(ledger, context);
    Require(snapshot.context_name == name, "ODF-055 scan-lane context name mismatch");
    Require(snapshot.resident_pages == 2, "ODF-055 scan-lane ring resident count mismatch");
    Require(snapshot.resident_bytes == 2 * 16384ull, "ODF-055 scan-lane resident bytes mismatch");
    Require(snapshot.admissions == 5, "ODF-055 scan-lane admissions not counted");
    Require(snapshot.reuses == 3, "ODF-055 scan-lane reuses not counted");
    Require(snapshot.evictions == 3, "ODF-055 scan-lane evictions not counted");
  }
}

void TestNormalContextStillUsesOrdinaryAdmission() {
  const Ids ids;
  CacheFixture cache(64);
  auto& ledger = cache.ledger;
  const auto policy = Policy(3);
  const auto normal_a = Entry(ids, 2000);
  const auto normal_b = Entry(ids, 2001);
  const auto normal_c = Entry(ids, 2002);
  const auto normal_d = Entry(ids, 2003);

  Require(page::AdmitPageCacheEntry(&ledger, policy, normal_a).ok(),
          "ODF-055 normal admission A failed");
  Require(page::AdmitPageCacheEntry(&ledger, policy, normal_b).ok(),
          "ODF-055 normal admission B failed");
  Require(page::AdmitPageCacheEntry(&ledger, policy, normal_c).ok(),
          "ODF-055 normal admission C failed");
  Require(page::PinPageCacheEntry(&ledger, normal_a.page_uuid).ok(),
          "ODF-055 normal pin failed");
  Require(page::UnpinPageCacheEntry(&ledger, normal_a.page_uuid).ok(),
          "ODF-055 normal unpin failed");
  Require(page::AdmitPageCacheEntry(&ledger, policy, normal_d).ok(),
          "ODF-055 normal admission D failed");
  Require(!Resident(ledger, normal_b.page_uuid), "ODF-055 normal LRU did not evict older page");

  Require(page::AdmitPageCacheEntry(&ledger, policy, normal_b).ok(),
          "ODF-055 normal nonresident slot reuse failed");
  Require(Resident(ledger, normal_b.page_uuid), "ODF-055 normal reused page not resident");
  const auto normal = page::SnapshotPageCacheContext(ledger, page::PageCacheIoContext::normal);
  Require(normal.resident_pages == 3, "ODF-055 normal resident count mismatch");
  Require(normal.admissions == 5, "ODF-055 normal admission count mismatch");
  Require(normal.evictions >= 2, "ODF-055 normal eviction count mismatch");
  Require(normal.reuses >= 1, "ODF-055 normal nonresident slot reuse not counted");
}

void TestPinnedAndDirtyRefusalsRemainClosed() {
  const Ids ids;
  {
    CacheFixture cache(64);
    auto& ledger = cache.ledger;
    auto policy = Policy(64);
    policy.strict_bulk_load_ring_pages = 1;
    const auto pinned = Entry(ids, 3000);
    Require(page::AdmitPageCacheEntryForContext(&ledger,
                                                policy,
                                                pinned,
                                                page::PageCacheIoContext::strict_bulk_load).ok(),
            "ODF-055 strict bulk initial admission failed");
    Require(page::PinPageCacheEntry(&ledger, pinned.page_uuid).ok(),
            "ODF-055 strict bulk pin failed");
    const auto refused = page::AdmitPageCacheEntryForContext(
        &ledger, policy, Entry(ids, 3001), page::PageCacheIoContext::strict_bulk_load);
    Require(!refused.ok(), "ODF-055 strict bulk evicted pinned ring page");
    Require(refused.diagnostic.diagnostic_code == "page_cache_context_ring_exhausted_pinned_or_dirty",
            "ODF-055 pinned refusal diagnostic mismatch");
    Require(Resident(ledger, pinned.page_uuid), "ODF-055 pinned page not resident after refusal");
    const auto strict = page::SnapshotPageCacheContext(ledger, page::PageCacheIoContext::strict_bulk_load);
    Require(strict.refusals >= 1, "ODF-055 strict bulk refusal not counted");
    RequireNoForbiddenDiagnosticToken(refused, "ODF-055 pinned refusal leaked docs token");
  }

  {
    CacheFixture cache(64);
    auto& ledger = cache.ledger;
    auto policy = Policy(64);
    policy.bulk_write_ring_pages = 1;
    const auto dirty = Entry(ids, 3100, false, true);
    Require(page::AdmitPageCacheEntryForContext(&ledger,
                                                policy,
                                                dirty,
                                                page::PageCacheIoContext::bulk_write).ok(),
            "ODF-055 bulk_write dirty admission failed");
    const auto refused = page::AdmitPageCacheEntryForContext(
        &ledger, policy, Entry(ids, 3101), page::PageCacheIoContext::bulk_write);
    Require(!refused.ok(), "ODF-055 bulk_write evicted dirty ring page");
    Require(Resident(ledger, dirty.page_uuid), "ODF-055 dirty page not resident after refusal");
    const auto bulk_write = page::SnapshotPageCacheContext(ledger, page::PageCacheIoContext::bulk_write);
    Require(bulk_write.refusals >= 1, "ODF-055 dirty ring refusal not counted");
    RequireNoForbiddenDiagnosticToken(refused, "ODF-055 dirty refusal leaked docs token");
  }
}

void TestMetricsAndNoRuntimeDocTokenLeak() {
  const auto contracts = metrics::MetricProducerContractsForOwner("storage_page");
  bool saw_refusal_counter = false;
  bool saw_resident_gauge = false;
  for (const auto& contract : contracts) {
    if (contract.family == "sb_page_cache_context_refusals_total") {
      saw_refusal_counter = true;
    }
    if (contract.family == "sb_page_cache_context_resident_bytes") {
      saw_resident_gauge = true;
    }
  }
  Require(saw_refusal_counter, "ODF-055 page-cache refusal metric contract missing");
  Require(saw_resident_gauge, "ODF-055 page-cache resident bytes metric contract missing");

  Require(MetricValueFor("sb_page_cache_context_admissions_total",
                         "bulk_read",
                         "ok",
                         "admit") >= 10.0,
          "ODF-055 admission metric missing");
  Require(MetricValueFor("sb_page_cache_context_reuses_total",
                         "bulk_read",
                         "ok",
                         "ring_or_slot_reuse") >= 8.0,
          "ODF-055 reuse metric missing");
  Require(MetricValueFor("sb_page_cache_context_evictions_total",
                         "bulk_read",
                         "evicted",
                         "budget_or_ring") >= 8.0,
          "ODF-055 eviction metric missing");
  Require(MetricValueFor("sb_page_cache_context_resident_pages",
                         "bulk_write",
                         "current",
                         "snapshot") >= 1.0,
          "ODF-055 lane resident metric missing");
  Require(MetricValueFor("sb_page_cache_context_protected_normal_hot_skips_total",
                         "bulk_read",
                         "protected",
                         "bulk_context_budget") >= 2.0,
          "ODF-055 protected normal skip metric missing");
  Require(MetricValueFor("sb_page_cache_context_refusals_total",
                         "bulk_read",
                         "refused",
                         "normal_hot_protected") >= 1.0,
          "ODF-055 protected refusal metric missing");
  Require(MetricValueFor("sb_page_cache_context_refusals_total",
                         "strict_bulk_load",
                         "refused",
                         "ring_pinned_or_dirty") >= 1.0,
          "ODF-055 ring refusal metric missing");

  for (const auto& value : metrics::DefaultMetricRegistry().SnapshotCurrent(true)) {
    Require(!ContainsForbiddenRuntimeToken(value.family), "ODF-055 metric family leaked docs token");
    for (const auto& label : value.labels) {
      Require(!ContainsForbiddenRuntimeToken(label.key), "ODF-055 metric label key leaked docs token");
      Require(!ContainsForbiddenRuntimeToken(label.value), "ODF-055 metric label value leaked docs token");
    }
  }
  for (const auto& code : page::PageCacheDiagnosticCodes()) {
    Require(!ContainsForbiddenRuntimeToken(code), "ODF-055 diagnostic code leaked docs token");
  }
}

}  // namespace

int main() {
  TestBulkReadChurnUsesOwnRingAndKeepsNormalHot();
  TestProtectedNormalHotBudgetRefusal();
  TestScanLaneRings();
  TestNormalContextStillUsesOrdinaryAdmission();
  TestPinnedAndDirtyRefusalsRemainClosed();
  TestMetricsAndNoRuntimeDocTokenLeak();
  return EXIT_SUCCESS;
}
