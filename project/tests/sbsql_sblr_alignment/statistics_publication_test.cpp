// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/optimizer/optimizer_statistics_full.hpp"
#include "binary_uuid_fixture.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <thread>

namespace fault {
thread_local long remaining = -1;
thread_local bool hit = false;
}
void* operator new(std::size_t n) {
  if (fault::remaining >= 0 && fault::remaining-- == 0) {
    fault::remaining = 0; fault::hit = true; throw std::bad_alloc();
  }
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace o = scratchbird::engine::optimizer;
namespace p = scratchbird::engine::planner;
using Uuid = p::CanonicalPlannerUuid;
using Status = o::OptimizerStatsPublicationStatus;
namespace {
unsigned checks = 0, faults = 0;
void Check(bool ok, const char* why) {
  ++checks;
  if (!ok) { std::cerr << "FAIL " << why << '\n'; std::abort(); }
}
// Independent literal test identities; no text UUID enters production.
Uuid Id(unsigned id) {
  auto value = scratchbird::tests::BinaryUuid("01900000-0000-7000-8000-000000000000");
  for (unsigned i = 0; i != 4; ++i) value.bytes[15-i] = (id >> (8*i)) & 255;
  return value;
}
o::OptimizerStatsIdentity Identity(Uuid relation, unsigned statistic, unsigned epoch) {
  o::OptimizerStatsIdentity id;
  id.object_uuid = relation;
  id.statistic_uuid = Id(statistic);
  id.catalog_epoch = 2;
  id.stats_epoch = epoch;
  id.transaction_visibility_epoch = 3;
  id.freshness = o::OptimizerStatsFreshnessState::kFresh;
  id.source = o::StatisticSource::kCatalogExact;
  id.confidence = o::CostConfidence::kExact;
  return id;
}
o::OptimizerStatsSnapshot Snapshot(unsigned base, unsigned epoch, bool full = true) {
  const auto relation = Id(base);
  o::OptimizerStatsSnapshot s;
  s.snapshot_id = Id(base + epoch*100 + 1);
  s.catalog_epoch = 2;
  s.stats_epoch = epoch;
  o::TableCardinalityStats table;
  table.identity = Identity(relation, base + epoch*100 + 2, epoch);
  table.row_count = epoch;
  table.visible_row_count = epoch;
  table.page_count = 1;
  table.average_row_bytes = 8;
  s.tables.push_back(table);
  if (!full) return s;
  o::ColumnStats column;
  column.identity = Identity(relation, base + epoch*100 + 3, epoch);
  column.column_uuid = Id(base+10);
  column.descriptor_digest = std::string(64, 'c');
  column.distinct_count = epoch;
  s.columns.push_back(column);
  o::HistogramStats histogram;
  histogram.identity = Identity(relation, base + epoch*100 + 4, epoch);
  histogram.column_uuid = column.column_uuid;
  histogram.buckets = {{"a", "z", 1., epoch}};
  s.histograms.push_back(histogram);
  o::MostCommonValueStats mcv;
  mcv.identity = Identity(relation, base + epoch*100 + 5, epoch);
  mcv.column_uuid = column.column_uuid;
  mcv.value_encoded = std::string(80, 'm');
  mcv.frequency = .25;
  s.mcv.push_back(mcv);
  mcv.identity.statistic_uuid = Id(base + epoch*100 + 6);
  mcv.value_encoded = std::string(80, 'n');
  s.mcv.push_back(mcv);
  o::ExpressionStats expression;
  expression.identity = Identity(relation, base + epoch*100 + 7, epoch);
  expression.expression_digest = std::string(64, 'e');
  expression.distinct_count = epoch;
  s.expressions.push_back(expression);
  o::ExtendedOptimizerStatistic extended;
  extended.identity = Identity(relation, base + epoch*100 + 8, epoch);
  extended.relation_uuid = relation;
  extended.column_uuids = {column.column_uuid};
  extended.multi_column_distinct_count = epoch;
  s.extended_stats.push_back(extended);
  o::IndexStats index;
  index.identity = Identity(relation, base + epoch*100 + 9, epoch);
  index.relation_uuid = relation;
  index.index_uuid = Id(base+11);
  index.key_column_uuids = {column.column_uuid};
  index.equality_lookup_supported = true;
  s.indexes.push_back(index);
  o::PageFilespaceStats filespace;
  filespace.filespace_uuid = Id(base+12);
  filespace.identity = Identity(filespace.filespace_uuid, base + epoch*100 + 10, epoch);
  filespace.page_family = "relation";
  filespace.page_size_bytes = 8192;
  s.page_filespaces.push_back(filespace);
  return s;
}
o::OptimizerPinnedStatsDescriptorSnapshot Pin(const o::OptimizerStatsSnapshot& s) {
  o::OptimizerPinnedStatsDescriptorSnapshot pin;
  pin.stats_snapshot = s;
  pin.key.catalog_epoch = s.catalog_epoch;
  pin.key.stats_epoch = s.stats_epoch;
  pin.key.security_epoch = 4;
  pin.key.resource_policy_epoch = 5;
  pin.key.name_resolution_epoch = 6;
  pin.key.descriptor_set_digest = std::string(80, 'd');
  pin.key.object_uuids = {s.tables.front().identity.object_uuid};
  for (const auto& filespace : s.page_filespaces) pin.key.object_uuids.push_back(filespace.filespace_uuid);
  for (const auto& index : s.indexes) pin.key.index_uuids.push_back(index.index_uuid);
  pin.key.security_policy_identity = Id(50);
  pin.key.redaction_policy_identity = Id(51);
  return pin;
}
void IndividualUpdates() {
  o::OptimizerStatisticsStore store;
  auto records = Snapshot(9000000, 1);
  const auto update = [&] {
    store.UpsertTable(records.tables[0]);
    store.UpsertColumn(records.columns[0]);
    store.UpsertHistogram(records.histograms[0]);
    store.UpsertMcv(records.mcv[0]);
    store.UpsertExtendedStatistic(records.extended_stats[0]);
    store.UpsertIndex(records.indexes[0]);
    store.UpsertExpression(records.expressions[0]);
    store.UpsertPageFilespace(records.page_filespaces[0]);
  };
  update();
  records.tables[0].row_count = 17;
  records.columns[0].distinct_count = 18;
  records.histograms[0].buckets[0].row_count = 19;
  records.mcv[0].frequency = .5;
  records.extended_stats[0].multi_column_distinct_count = 20;
  records.indexes[0].height = 21;
  records.expressions[0].distinct_count = 22;
  records.page_filespaces[0].free_pages = 23;
  update();
  const auto state = store.Snapshot(Id(999));
  Check(state.tables.size() == 1 && state.tables[0].row_count == 17 &&
        state.columns.size() == 1 && state.columns[0].distinct_count == 18 &&
        state.histograms.size() == 1 && state.histograms[0].buckets[0].row_count == 19 &&
        state.mcv.size() == 1 && state.mcv[0].frequency == .5 &&
        state.extended_stats.size() == 1 && state.extended_stats[0].multi_column_distinct_count == 20 &&
        state.indexes.size() == 1 && state.indexes[0].height == 21 &&
        state.expressions.size() == 1 && state.expressions[0].distinct_count == 22 &&
        state.page_filespaces.size() == 1 && state.page_filespaces[0].free_pages == 23,
        "all eight individual upserts actually insert and replace values");
  store.MarkStaleByObject(Id(9000000), 3);
  Check(store.FindTable(Id(9000000))->identity.freshness == o::OptimizerStatsFreshnessState::kStale &&
        store.FindIndex(Id(9000011))->identity.freshness == o::OptimizerStatsFreshnessState::kStale,
        "stale mutation reaches real stored records");
}
void PublicationAndRemoval() {
  auto& cache = o::GlobalOptimizerPinnedStatsDescriptorCache();
  cache.Clear();
  o::OptimizerStatisticsStore store;
  const auto first = Snapshot(10000, 1);
  auto initial = store.PublishRelationSnapshot(first);
  Check(initial.status == Status::kPublished, "all eight families publish");
  Check(initial.snapshot == store.PublishedRelationSnapshot(Id(10000)), "actual snapshot owner retained");
  const auto pin = Pin(first);
  auto held_pin = cache.Put(pin);
  Check(held_pin.ok, "old pin inserted");
  const auto other = Snapshot(20000, 1);
  Check(store.PublishRelationSnapshot(other).status == Status::kPublished, "independent relation publishes");
  auto other_pin = cache.Put(Pin(other));
  Check(other_pin.ok && cache.Lookup(pin.key).cache_hit, "unrelated publication preserves pin");
  auto second = Snapshot(10000, 2, false);
  second.tables[0].row_count = 0;
  second.tables[0].visible_row_count = 0;
  second.tables[0].page_count = 0;
  second.tables[0].average_row_bytes = 0;
  const auto published = store.PublishRelationSnapshot(second);
  Check(published.status == Status::kPublished && published.invalidated_snapshots == 1, "empty relation replaces whole prior snapshot");
  Check(!cache.Lookup(pin.key).cache_hit && cache.Lookup(other_pin.snapshot->key).cache_hit, "only affected pin removed");
  Check(held_pin.snapshot->stats_snapshot.mcv.size() == 2, "reader retains old immutable snapshot");
  Check(initial.snapshot->stats_epoch == 1, "old publication owner survives replacement");
  Check(!cache.Put(pin).ok, "delayed old pin refused");
  auto forged = pin;
  forged.key.stats_epoch = 2;
  Check(!cache.Put(forged).ok, "relabelled old contents do not evade epoch fence");
  Check(cache.Put(Pin(second)).ok, "new pin accepted");
  const auto state = store.Snapshot(Id(999));
  Check(state.tables.size() == 2 && state.columns.size() == 1 && state.histograms.size() == 1 &&
        state.mcv.size() == 2 && state.extended_stats.size() == 1 && state.expressions.size() == 1 &&
        state.indexes.size() == 1 && state.page_filespaces.size() == 2, "absent relation families removed; shared filespaces and unrelated families intact");
  Check(store.FindTable(Id(10000))->row_count == 0, "known zero not floored");
  Check(store.PublishRelationSnapshot(first).status == Status::kStaleEpoch, "stale publication refused");
}
void ZeroDistinctPublication() {
  o::OptimizerStatisticsStore store;
  auto all_null = Snapshot(12000000, 1);
  auto& column = all_null.columns[0];
  column.distinct_count = 0;
  column.hyperloglog_register_count = 4096;
  column.hyperloglog_estimated_distinct = 0;
  column.null_fraction = 1.;
  all_null.histograms.clear();
  all_null.mcv.clear();
  auto result = store.PublishRelationSnapshot(all_null);
  Check(result.status == Status::kPublished && result.snapshot->columns[0].distinct_count == 0 &&
        result.snapshot->columns[0].hyperloglog_estimated_distinct == 0,
        "all-null column HLL observation preserves known zero distinct count");
}
void SharedFilespacePublication() {
  auto& cache = o::GlobalOptimizerPinnedStatsDescriptorCache();
  cache.Clear();
  o::OptimizerStatisticsStore store;
  const auto a = Snapshot(10000000, 1);
  Check(store.PublishRelationSnapshot(a).status == Status::kPublished, "shared filespace first relation");
  const auto old_pin = Pin(a);
  Check(cache.Put(old_pin).ok, "shared filespace first pin");
  auto b = Snapshot(11000000, 2);
  b.page_filespaces[0].filespace_uuid = a.page_filespaces[0].filespace_uuid;
  b.page_filespaces[0].identity.object_uuid = b.page_filespaces[0].filespace_uuid;
  b.page_filespaces[0].free_pages = 42;
  auto result = store.PublishRelationSnapshot(b);
  Check(result.status == Status::kPublished && result.invalidated_snapshots == 1,
        "second relation can refresh shared filespace atomically");
  const auto state = store.Snapshot(Id(999));
  Check(state.tables.size() == 2 && state.page_filespaces.size() == 1 &&
        state.page_filespaces[0].free_pages == 42 && store.FindTable(Id(10000000))->row_count == 1,
        "shared filespace refresh preserves both relation records");
  Check(!cache.Put(old_pin).ok, "shared filespace update fences dependent pin from other relation");
  const auto current_pin = Pin(b);
  Check(cache.Put(current_pin).ok, "updated shared filespace pin");
  Check(store.PublishRelationSnapshot(Snapshot(10000000, 3, false)).status == Status::kPublished,
        "relation-only replacement does not require a shared filespace refresh");
  Check(store.Snapshot(Id(999)).page_filespaces.size() == 1 && cache.Lookup(current_pin.key).cache_hit,
        "omitted shared filespace survives and other relation pin remains valid");
}
void InvalidSnapshots() {
  o::OptimizerStatisticsStore store;
  auto good = Snapshot(30000, 1);
  Check(store.PublishRelationSnapshot(good).status == Status::kPublished, "negative baseline");
  auto pin = Pin(good);
  auto& cache = o::GlobalOptimizerPinnedStatsDescriptorCache();
  Check(cache.Put(pin).ok, "negative pin baseline");
  for (unsigned test = 0; test != 21; ++test) {
    auto bad = Snapshot(30000, 2);
    switch (test) {
      case 0: bad.snapshot_id = {}; break;
      case 1: bad.tables.push_back(bad.tables.front()); break;
      case 2: bad.columns[0].identity.object_uuid = Id(30001); break;
      case 3: bad.columns[0].identity.statistic_uuid = bad.tables[0].identity.statistic_uuid; break;
      case 4: bad.columns[0].identity.stats_epoch = 1; break;
      case 5: bad.columns.push_back(bad.columns.front()); bad.columns.back().identity.statistic_uuid = Id(39999); break;
      case 6: bad.histograms[0].column_uuid = Id(39998); break;
      case 7: bad.mcv[1].value_encoded = bad.mcv[0].value_encoded; break;
      case 8: bad.columns[0].null_fraction = std::numeric_limits<double>::quiet_NaN(); break;
      case 9: bad.histograms[0].buckets[0].fraction = std::numeric_limits<double>::infinity(); break;
      case 10: bad.mcv[0].frequency = std::numeric_limits<double>::quiet_NaN(); break;
      case 11: bad.expressions[0].null_fraction = std::numeric_limits<double>::quiet_NaN(); break;
      case 12: bad.indexes[0].relation_uuid = Id(39998); break;
      case 13: bad.columns[0].column_uuid.bytes[6] = 0x40; break;
      case 14: bad.tables[0].visible_row_count = 100; break;
      case 15: bad.indexes[0].key_column_uuids = {{}}; break;
      case 16: bad.extended_stats[0].column_uuids = {{}}; break;
      case 17: bad.page_filespaces[0].identity.object_uuid = Id(30000); break;
      case 18: bad.page_filespaces[0].sequential_latency_score = std::numeric_limits<double>::quiet_NaN(); break;
      case 19: bad.indexes[0].clustering_factor = std::numeric_limits<double>::infinity(); break;
      case 20: bad.indexes[0].contention_ratio = -1.; break;
    }
    const auto result = store.PublishRelationSnapshot(std::move(bad));
    Check(result.status == Status::kInvalidSnapshot && !result.snapshot && result.invalidated_snapshots == 0, "invalid snapshot cannot publish");
    Check(store.PublishedRelationSnapshot(Id(30000))->stats_epoch == 1 && cache.Lookup(pin.key).cache_hit,
          "invalid publication preserves store and cache");
  }
}
void FaultAtomicPublication() {
  bool completed = false;
  auto& cache = o::GlobalOptimizerPinnedStatsDescriptorCache();
  for (long allocation = 0; allocation != 2000; ++allocation) {
    const unsigned base = 100000;
    o::OptimizerStatisticsStore store;
    const auto old = Snapshot(base, 1);
    Check(store.PublishRelationSnapshot(old).status == Status::kPublished, "fault baseline");
    const auto pin = Pin(old);
    Check(cache.Put(pin).ok, "fault pin baseline");
    auto replacement = Snapshot(base, 2);
    fault::hit = false;
    fault::remaining = allocation;
    const auto result = store.PublishRelationSnapshot(std::move(replacement));
    fault::remaining = -1;
    if (!fault::hit) {
      Check(result.status == Status::kPublished, "fault sweep reaches actual successful publication");
      Check(!cache.Lookup(pin.key).cache_hit, "successful fault sweep invalidates old pin");
      completed = true;
      break;
    }
    ++faults;
    Check(result.status == Status::kResourceExhausted && !result.snapshot && result.invalidated_snapshots == 0,
          "allocation refusal has no success fields");
    const auto state = store.Snapshot(Id(999));
    Check(state.stats_epoch == 1 && state.tables.size() == 1 && state.columns.size() == 1 &&
          state.histograms.size() == 1 && state.mcv.size() == 2 && state.extended_stats.size() == 1 &&
          state.indexes.size() == 1 && state.expressions.size() == 1 && state.page_filespaces.size() == 1,
          "every allocation failure preserves all eight families");
    Check(store.PublishedRelationSnapshot(Id(base))->stats_epoch == 1 && cache.Lookup(pin.key).cache_hit,
          "every allocation failure preserves prior owner and cache pin");
    Check(cache.Put(pin).ok, "failed publication does not advance epoch floor");
    cache.Clear();
  }
  Check(completed && faults > 20, "all actual publication allocations swept");
}
void FaultAtomicInvalidation() {
  bool completed = false;
  for (long allocation = 0; allocation != 500; ++allocation) {
    o::OptimizerPinnedStatsDescriptorCache cache;
    const auto a = Pin(Snapshot(6000000, 1));
    const auto b = Pin(Snapshot(7000000, 1));
    Check(cache.Put(a).ok && cache.Put(b).ok, "invalidation baseline");
    o::StatsInvalidationEvent event;
    event.event_kind = "stats_refresh";
    event.reason = std::string(100, 'r');
    bool threw = false;
    o::OptimizerPinnedStatsInvalidationResult result;
    fault::hit = false;
    fault::remaining = allocation;
    try { result = cache.Invalidate(event); } catch (const std::bad_alloc&) { threw = true; }
    fault::remaining = -1;
    if (!fault::hit) {
      Check(!threw && result.invalidated_entries.size() == 2, "complete invalidation evidence");
      Check(!cache.Lookup(a.key).cache_hit && !cache.Lookup(b.key).cache_hit, "both matching pins erased");
      completed = true;
      break;
    }
    ++faults;
    Check(threw && cache.Lookup(a.key).cache_hit && cache.Lookup(b.key).cache_hit,
          "evidence allocation failure cannot erase a prefix of matching pins");
  }
  Check(completed, "invalidation allocation sweep completed");
}
void FaultAtomicUpsert() {
  bool completed = false;
  auto& cache = o::GlobalOptimizerPinnedStatsDescriptorCache();
  for (long allocation = 0; allocation != 500; ++allocation) {
    cache.Clear();
    o::OptimizerStatisticsStore store;
    const auto initial = Snapshot(8000000, 1);
    Check(store.PublishRelationSnapshot(initial).status == Status::kPublished, "upsert baseline");
    const auto pin = Pin(initial);
    Check(cache.Put(pin).ok, "upsert pin baseline");
    auto replacement = Snapshot(8000000, 2).tables.front();
    bool threw = false;
    fault::hit = false;
    fault::remaining = allocation;
    try { store.UpsertTable(std::move(replacement)); } catch (const std::bad_alloc&) { threw = true; }
    fault::remaining = -1;
    if (!fault::hit) {
      Check(!threw && store.FindTable(Id(8000000))->row_count == 2 && !cache.Lookup(pin.key).cache_hit,
            "single record update commits after invalidation preparation");
      Check(!store.PublishedRelationSnapshot(Id(8000000)), "single record update cannot retain obsolete batch receipt");
      Check(!cache.Put(pin).ok, "single record update fences delayed old pins");
      completed = true;
      break;
    }
    ++faults;
    Check(threw && store.FindTable(Id(8000000))->row_count == 1 &&
          store.PublishedRelationSnapshot(Id(8000000))->stats_epoch == 1 && cache.Lookup(pin.key).cache_hit,
          "single record update preserves prior table, owner and pin on allocation failure");
  }
  Check(completed, "upsert allocation sweep completed");
}
void ConcurrentSnapshots() {
  o::OptimizerStatisticsStore store;
  Check(store.PublishRelationSnapshot(Snapshot(5000000, 1)).status == Status::kPublished, "concurrent baseline");
  std::atomic<bool> stop{false}, consistent{true};
  std::atomic<unsigned> reads{0};
  std::vector<std::thread> readers;
  for (unsigned t = 0; t != 4; ++t) readers.emplace_back([&] {
    while (!stop.load()) {
      const auto s = store.Snapshot(Id(999));
      const auto owner = store.PublishedRelationSnapshot(Id(5000000));
      if (s.tables.size() != 1 || s.columns.size() != 1 || s.mcv.size() != 2 ||
          s.tables[0].row_count != s.stats_epoch || s.columns[0].distinct_count != s.stats_epoch ||
          s.expressions[0].distinct_count != s.stats_epoch || s.extended_stats[0].multi_column_distinct_count != s.stats_epoch ||
          owner->tables[0].row_count != owner->stats_epoch) consistent = false;
      ++reads;
    }
  });
  for (unsigned epoch = 2; epoch != 80; ++epoch)
    Check(store.PublishRelationSnapshot(Snapshot(5000000, epoch)).status == Status::kPublished,
          "concurrent complete replacement");
  stop = true;
  for (auto& reader : readers) reader.join();
  Check(consistent && reads > 0, "concurrent readers observe only complete immutable generations");
}
}  // namespace
int main() {
  IndividualUpdates();
  PublicationAndRemoval();
  ZeroDistinctPublication();
  SharedFilespacePublication();
  InvalidSnapshots();
  FaultAtomicPublication();
  FaultAtomicInvalidation();
  FaultAtomicUpsert();
  ConcurrentSnapshots();
  std::cout << "PASS statistics publication checks=" << checks << " allocation_faults=" << faults << '\n';
}
