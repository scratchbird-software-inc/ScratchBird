// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_statistics_invalidation.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <atomic>
#include <barrier>
#include <thread>
#include <limits>
#include <new>
#include <stdexcept>

namespace invalidation_fault {
thread_local long remaining = -1;
thread_local bool hit = false;
}
void* operator new(std::size_t size) {
  if (invalidation_fault::remaining >= 0 && invalidation_fault::remaining-- == 0) {
    invalidation_fault::remaining = 0;
    invalidation_fault::hit = true;
    throw std::bad_alloc();
  }
  if (void* value = std::malloc(size ? size : 1)) return value;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }


namespace opt = scratchbird::engine::optimizer;

namespace {

using Id = scratchbird::engine::planner::CanonicalPlannerUuid;
using Kind = opt::OptimizerStatisticsInvalidationKind;
using Scope = opt::OptimizerStatsGenerationScope;
unsigned checks = 0;
Id Identity(unsigned n) {
  Id id{}; id.bytes[0] = 1; id.bytes[6] = 0x70; id.bytes[8] = 0x80;
  for (unsigned i = 0; i != 4; ++i) { id.bytes[15-i] = n & 255; n >>= 8; }
  return id;
}

bool Require(bool condition, const std::string& message) {
  ++checks;
  if (!condition) {
    std::cerr << "OEIC stats invalidation gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool HasStatus(const std::vector<opt::StatisticsContractStatus>& statuses,
               const std::string& code) {
  return std::any_of(statuses.begin(), statuses.end(), [&](const auto& status) {
    return status.detail.starts_with(code + ":");
  });
}

opt::OptimizerStatsSnapshot StatsSnapshot(const Id& relation_uuid,
                                          std::uint64_t stats_epoch,
                                          std::uint64_t catalog_epoch) {
  opt::OptimizerStatisticsStore store;
  opt::AnalyzeSampleInput input;
  input.relation_uuid = relation_uuid;
  input.sampled_rows = 100;
  input.total_rows_estimate = 100;
  input.page_count = 4;
  input.average_row_bytes = 128;
  input.stats_epoch = stats_epoch;
  input.catalog_epoch = catalog_epoch;
  auto table = opt::BuildTableStatsFromAnalyzeSample(input);
  if (!table) throw std::runtime_error("table fixture rejected");
  store.UpsertTable(*table);
  auto snapshot_id = relation_uuid;
  snapshot_id.bytes[5] = 1;
  return store.Snapshot(snapshot_id);
}

opt::OptimizerPinnedStatsDescriptorSnapshot PinnedSnapshot(
    const Id& relation_uuid,
    const Id& index_uuid,
    std::uint64_t stats_epoch = 20,
    std::uint64_t catalog_epoch = 30) {
  opt::OptimizerPinnedStatsDescriptorSnapshot snapshot;
  snapshot.key.catalog_epoch = catalog_epoch;
  snapshot.key.security_epoch = 40;
  snapshot.key.resource_policy_epoch = 50;
  snapshot.key.name_resolution_epoch = 60;
  snapshot.key.stats_epoch = stats_epoch;
  snapshot.key.descriptor_set_digest = "descriptor.fixture";
  snapshot.key.object_uuids = {relation_uuid};
  snapshot.key.index_uuids = {index_uuid};
  snapshot.key.security_policy_identity = Identity(3);
  snapshot.key.redaction_policy_identity = Identity(4);
  snapshot.stats_snapshot = StatsSnapshot(relation_uuid, stats_epoch, catalog_epoch);
  snapshot.read_only_snapshot = true;
  snapshot.mga_visibility_recheck_required = true;
  snapshot.security_recheck_required = true;
  snapshot.finality_authority_cached = false;
  return snapshot;
}

opt::OptimizerStatisticsInvalidationAuthority Authority() {
  opt::OptimizerStatisticsInvalidationAuthority authority;
  authority.engine_runtime_scope = true;
  authority.optimizer_cache_owner = true;
  authority.catalog_generation_authority = true;
  authority.security_generation_authority = true;
  authority.redaction_generation_authority = true;
  authority.metric_generation_authority = true;
  authority.analyze_generation_authority = true;
  authority.index_generation_authority = true;
  return authority;
}

bool InvalidationDispatchesAnalyzeAndMetricGenerations() {
  // SEARCH_KEY: OEIC_STATISTICS_INVALIDATION_ENTERPRISE
  opt::OptimizerPinnedStatsDescriptorCache cache;
  const auto rel_a = PinnedSnapshot(Identity(10), Identity(11));
  const auto rel_b = PinnedSnapshot(Identity(20), Identity(21));
  if (!Require(cache.Put(rel_a).ok, "failed to pin rel A stats") ||
      !Require(cache.Put(rel_b).ok, "failed to pin rel B stats")) {
    return false;
  }

  opt::OptimizerStatisticsInvalidationRequest analyze;
  analyze.kind = opt::OptimizerStatisticsInvalidationKind::kAnalyzeGeneration;
  analyze.authority = Authority();
  analyze.object_uuid = Identity(10);
  analyze.evidence_digest = std::string(64, 'a');
  analyze.stats_epoch = 21;
  analyze.analyze_generation = 22;
  analyze.reason = "analyze refresh";
  const auto analyze_result = opt::DispatchOptimizerStatisticsInvalidation(analyze, &cache);
  if (!Require(analyze_result.accepted, "analyze invalidation refused") ||
      !Require(analyze_result.invalidated_entries.size() == 1,
               "analyze invalidation did not target one snapshot")) {
    return false;
  }
  const auto miss_a = cache.Lookup(rel_a.key);
  const auto hit_b = cache.Lookup(rel_b.key);
  if (!Require(!miss_a.ok &&
                   miss_a.diagnostic_code == "SB_OPT_PINNED_STATS_CACHE_MISS",
               "rel A pinned stats survived analyze invalidation") ||
      !Require(hit_b.ok, "rel B pinned stats was invalidated too early")) {
    return false;
  }

  opt::OptimizerStatisticsInvalidationRequest metric;
  metric.kind = opt::OptimizerStatisticsInvalidationKind::kStorageMetricGeneration;
  metric.authority = Authority();
  metric.evidence_digest = std::string(64, 'b');
  metric.metric_generation = 23;
  const auto metric_result = opt::DispatchOptimizerStatisticsInvalidation(metric, &cache);
  const auto miss_b = cache.Lookup(rel_b.key);
  return Require(metric_result.accepted, "storage metric invalidation refused") &&
         Require(metric_result.invalidated_entries.size() == 1,
                 "storage metric invalidation did not clear remaining snapshot") &&
         Require(!miss_b.ok &&
                     miss_b.diagnostic_code == "SB_OPT_PINNED_STATS_CACHE_MISS",
                 "rel B pinned stats survived storage metric invalidation");
}

bool InvalidationRejectsUnsafeAuthority() {
  opt::OptimizerPinnedStatsDescriptorCache cache;
  opt::OptimizerStatisticsInvalidationRequest request;
  request.kind = opt::OptimizerStatisticsInvalidationKind::kStatsRefresh;
  request.authority = Authority();
  request.authority.parser_or_reference_authority = true;
  request.evidence_digest = std::string(64, 'c');
  request.stats_epoch = 20;
  const auto result = opt::DispatchOptimizerStatisticsInvalidation(request, &cache);
  return Require(!result.accepted, "unsafe authority was accepted") &&
         Require(result.diagnostic_code == "SB-STAT-0001" &&
                 result.failure == opt::OptimizerStatisticsInvalidationFailure::kInvalidRequest,
                 "unsafe authority diagnostic mismatch");
}

bool BenchmarkCleanPinnedStatsRejectsStaleAndUnsafeSnapshots() {
  auto snapshot = PinnedSnapshot(Identity(30), Identity(31));
  opt::OptimizerPinnedStatsBenchmarkCleanRequest request;
  request.snapshot = snapshot;
  request.required_catalog_epoch = snapshot.key.catalog_epoch;
  request.required_security_epoch = snapshot.key.security_epoch;
  request.required_resource_policy_epoch = snapshot.key.resource_policy_epoch;
  request.required_name_resolution_epoch = snapshot.key.name_resolution_epoch;
  request.required_stats_epoch = snapshot.key.stats_epoch;
  request.required_descriptor_set_digest = snapshot.key.descriptor_set_digest;
  request.required_security_policy_identity = snapshot.key.security_policy_identity;
  request.required_redaction_policy_identity = snapshot.key.redaction_policy_identity;
  request.required_object_uuids = snapshot.key.object_uuids;
  request.required_index_uuids = snapshot.key.index_uuids;
  const auto ok = opt::ValidatePinnedStatsForBenchmarkCleanAdmission(request);

  auto stale = request;
  stale.required_stats_epoch = snapshot.key.stats_epoch + 1;
  const auto stale_status = opt::ValidatePinnedStatsForBenchmarkCleanAdmission(stale);

  auto unsafe = request;
  unsafe.snapshot.finality_authority_cached = true;
  const auto unsafe_status = opt::ValidatePinnedStatsForBenchmarkCleanAdmission(unsafe);

  return Require(HasStatus(ok, "SB_OPT_STATS_BENCHMARK_CLEAN_PINNED_OK"),
                 "valid pinned stats snapshot was not accepted") &&
         Require(HasStatus(stale_status, "SB_OPT_STATS_BENCHMARK_CLEAN_STALE_EPOCH"),
                 "stale pinned stats snapshot was accepted") &&
         Require(HasStatus(unsafe_status, "SB_OPT_STATS_BENCHMARK_CLEAN_UNSAFE_SNAPSHOT"),
                 "unsafe pinned stats snapshot was accepted");
}

opt::OptimizerStatisticsInvalidationRequest GenerationRequest(Kind kind) {
  opt::OptimizerStatisticsInvalidationRequest r;
  r.kind = kind; r.authority = Authority();
  r.evidence_digest = std::string(64, 'a');
  switch (kind) {
    case Kind::kCatalogGeneration: r.catalog_epoch = 100; break;
    case Kind::kSecurityGeneration: r.security_epoch = 100; break;
    case Kind::kRedactionGeneration: r.redaction_epoch = 100; break;
    case Kind::kPolicyGeneration: r.policy_epoch = 100; break;
    case Kind::kResourceGeneration: r.resource_epoch = 100; break;
    case Kind::kNameResolutionGeneration: r.name_resolution_epoch = 100; break;
    case Kind::kAnalyzeGeneration: r.analyze_generation = r.stats_epoch = 100; break;
    case Kind::kStatsRefresh: r.stats_epoch = 100; break;
    case Kind::kStorageMetricGeneration:
    case Kind::kRuntimeMetricGeneration: r.metric_generation = 100; break;
    case Kind::kIndexGeneration: r.index_generation = 100; r.index_uuid = Identity(11); break;
  }
  return r;
}

void SetGeneration(opt::OptimizerPinnedStatsDescriptorSnapshot& pin,
                   Kind kind, Scope scope, Id id, std::uint64_t generation) {
  opt::OptimizerStatsGenerationKey key{kind, scope, id};
  auto found = std::ranges::find(pin.key.dependency_generations, key,
      &opt::OptimizerStatsGenerationDependency::key);
  if (found == pin.key.dependency_generations.end())
    pin.key.dependency_generations.push_back({key, generation});
  else found->generation = generation;
  if (kind == Kind::kCatalogGeneration) {
    pin.key.catalog_epoch = pin.stats_snapshot.catalog_epoch = generation;
    for (auto& record : pin.stats_snapshot.tables) record.identity.catalog_epoch = generation;
  }
  if (kind == Kind::kStatsRefresh || kind == Kind::kAnalyzeGeneration) {
    pin.key.stats_epoch = pin.stats_snapshot.stats_epoch = generation;
    for (auto& record : pin.stats_snapshot.tables) record.identity.stats_epoch = generation;
  }
  if (kind == Kind::kSecurityGeneration) pin.key.security_epoch = generation;
  if (kind == Kind::kNameResolutionGeneration) pin.key.name_resolution_epoch = generation;
}

bool EveryGenerationIsFenced() {
  bool passed = true;
  for (unsigned ordinal = 0; ordinal != 11; ++ordinal) {
    for (bool targeted : {false, true}) {
      const auto kind = static_cast<Kind>(ordinal);
      auto request = GenerationRequest(kind);
      Scope scope = Scope::kNode; Id scope_id{};
      if (kind == Kind::kIndexGeneration) { scope = Scope::kIndex; scope_id = Identity(11); }
      else if (targeted) { request.object_uuid = Identity(10); scope = Scope::kObject; scope_id = Identity(10); }
      auto a = PinnedSnapshot(Identity(10), Identity(11));
      const auto b = PinnedSnapshot(Identity(20), Identity(21));
      opt::OptimizerPinnedStatsDescriptorCache cache;
      passed &= Require(cache.Put(a).ok && cache.Put(b).ok, "generation baseline");
      const auto retained = cache.Lookup(a.key).snapshot;
      const auto result = opt::DispatchOptimizerStatisticsInvalidation(request, &cache);
      const bool global = scope == Scope::kNode;
      passed &= Require(result.accepted && result.diagnostic_code.empty() &&
          result.invalidated_entries.size() == (global ? 2 : 1),
          "generation or target scope was dropped");
      passed &= Require(!cache.Lookup(a.key).cache_hit &&
          (global ? !cache.Lookup(b.key).cache_hit : cache.Lookup(b.key).cache_hit),
          "wrong generation dependents survived or unrelated pin was erased");
      passed &= Require(!cache.Put(a).ok, "old generation reinserted after invalidation");
      if (kind == Kind::kCatalogGeneration || kind == Kind::kStatsRefresh) {
        auto forged = a;
        if (kind == Kind::kCatalogGeneration) forged.key.catalog_epoch = 100;
        else forged.key.stats_epoch = 100;
        forged.key.dependency_generations.push_back({{kind, scope, scope_id}, 100});
        passed &= Require(!cache.Put(forged).ok, "new key disguised stale payload records");
      }
      SetGeneration(a, kind, scope, scope_id, 100);
      passed &= Require(cache.Put(a).ok, "current scoped generation refused");
      passed &= Require(retained && retained->stats_snapshot.tables.front().row_count == 100,
                        "invalidation mutated a retained prior owner");
      cache.Clear();
      const auto old = PinnedSnapshot(Identity(10), Identity(11));
      passed &= Require(!cache.Put(old).ok, "Clear erased the generation fence");
    }
  }
  return passed;
}

bool ScopedPolicyAndFilespaceEvents() {
  bool passed = true;
  for (auto scope : {Scope::kSecurityPolicy, Scope::kRedactionPolicy, Scope::kFilespace}) {
    auto a = PinnedSnapshot(Identity(10), Identity(11));
    auto b = PinnedSnapshot(Identity(20), Identity(21));
    auto c = PinnedSnapshot(Identity(30), Identity(31));
    const Kind kind = scope == Scope::kSecurityPolicy ? Kind::kSecurityGeneration :
        scope == Scope::kRedactionPolicy ? Kind::kRedactionGeneration : Kind::kStorageMetricGeneration;
    const Id target = Identity(90);
    if (scope == Scope::kSecurityPolicy) a.key.security_policy_identity = b.key.security_policy_identity = target;
    if (scope == Scope::kRedactionPolicy) a.key.redaction_policy_identity = b.key.redaction_policy_identity = target;
    if (scope == Scope::kFilespace) {
      a.key.dependency_generations.push_back({{kind, scope, target}, 1});
      b.key.dependency_generations.push_back({{kind, scope, target}, 1});
    }
    opt::OptimizerPinnedStatsDescriptorCache cache;
    passed &= Require(cache.Put(a).ok && cache.Put(b).ok && cache.Put(c).ok, "scope baseline");
    auto r = GenerationRequest(kind);
    if (scope == Scope::kSecurityPolicy) r.security_policy_identity = target;
    if (scope == Scope::kRedactionPolicy) r.redaction_policy_identity = target;
    if (scope == Scope::kFilespace) r.filespace_uuid = target;
    const auto result = opt::DispatchOptimizerStatisticsInvalidation(r, &cache);
    passed &= Require(result.accepted && result.invalidated_entries.size() == 2 &&
                      cache.Lookup(c.key).cache_hit, "shared scoped dependents not selected exactly");
    passed &= Require(!cache.Put(a).ok && !cache.Put(b).ok, "scoped delayed producer reinserted");
    SetGeneration(a, kind, scope, target, 100);
    passed &= Require(cache.Put(a).ok, "current policy/filespace generation refused");
  }
  return passed;
}

bool RefusalsAndKeyBinding() {
  bool passed = true;
  for (unsigned scenario = 0; scenario != 9; ++scenario) {
    const auto pin = PinnedSnapshot(Identity(10), Identity(11));
    opt::OptimizerPinnedStatsDescriptorCache cache;
    passed &= Require(cache.Put(pin).ok, "refusal baseline");
    auto r = GenerationRequest(Kind::kStatsRefresh);
    if (scenario == 0) r.kind = static_cast<Kind>(255);
    if (scenario == 1) { r.object_uuid = Identity(10); r.object_uuid.bytes[6] = 0x40; }
    if (scenario == 2) { r.filespace_uuid = Identity(12); r.filespace_uuid.bytes[8] = 0; }
    if (scenario == 3) r.evidence_digest = "not a digest";
    if (scenario == 4) r.stats_epoch = 0;
    if (scenario == 5) { r.kind = Kind::kPolicyGeneration; r.resource_epoch = 100; }
    if (scenario == 6) { r.kind = Kind::kNameResolutionGeneration; r.catalog_epoch = 100; }
    if (scenario == 7) r.authority.metric_finality_authority = true;
    if (scenario == 8) r.authority.optimizer_cache_owner = false;
    const auto result = opt::DispatchOptimizerStatisticsInvalidation(r, &cache);
    passed &= Require(!result.accepted && result.invalidated_entries.empty() &&
                      cache.Lookup(pin.key).cache_hit && cache.Put(pin).ok,
                      "invalid request changed cache or epoch fences");
  }
  auto pin = PinnedSnapshot(Identity(10), Identity(11));
  pin.key.dependency_generations = {
      {{Kind::kRuntimeMetricGeneration, Scope::kObject, Identity(10)}, 8},
      {{Kind::kRuntimeMetricGeneration, Scope::kFilespace, Identity(10)}, 9}};
  const auto original = opt::OptimizerPinnedStatsDescriptorCacheKeyText(pin.key);
  auto reverse = pin.key;
  std::ranges::reverse(reverse.dependency_generations);
  passed &= Require(opt::OptimizerPinnedStatsDescriptorCacheKeyText(reverse) == original,
                    "generation order changed canonical key");
  ++reverse.dependency_generations[0].generation;
  passed &= Require(opt::OptimizerPinnedStatsDescriptorCacheKeyText(reverse) != original,
                    "generation missing from key");
  auto duplicate = pin.key;
  duplicate.dependency_generations.push_back(duplicate.dependency_generations.front());
  passed &= Require(!opt::ValidateOptimizerPinnedStatsDescriptorKey(duplicate).ok,
                    "duplicate scoped generation accepted");
  opt::StatsInvalidationEvent event;
  event.event_kind = "unknown";
  opt::OptimizerPinnedStatsDescriptorCache cache;
  passed &= Require(cache.Put(pin).ok && !cache.Invalidate(event).accepted &&
      cache.Lookup(pin.key).cache_hit, "unknown low-level event was acknowledged");
  event.event_kind = "stats_refresh";
  event.generations = {{{Kind::kStatsRefresh, Scope::kObject, Identity(999)}, 100}};
  passed &= Require(!cache.Invalidate(event).accepted, "generation scope absent from event accepted");
  return passed;
}

bool BenchmarkRejectsAdvisoryAndForgedEpochs() {
  opt::OptimizerPinnedStatsBenchmarkCleanRequest r;
  r.snapshot = PinnedSnapshot(Identity(10), Identity(11));
  r.required_catalog_epoch = 30; r.required_stats_epoch = 20;
  r.required_security_epoch = 40; r.required_resource_policy_epoch = 50;
  r.required_name_resolution_epoch = 60;
  r.required_descriptor_set_digest = r.snapshot.key.descriptor_set_digest;
  r.required_security_policy_identity = r.snapshot.key.security_policy_identity;
  r.required_redaction_policy_identity = r.snapshot.key.redaction_policy_identity;
  r.required_object_uuids = r.snapshot.key.object_uuids;
  r.required_index_uuids = r.snapshot.key.index_uuids;
  bool passed = true;
  for (unsigned mode = 0; mode != 6; ++mode) {
    auto invalid = r;
    if (mode == 0) invalid.snapshot.stats_snapshot.tables[0].identity.source = opt::StatisticSource::kRuntimeMetric;
    if (mode == 1) { invalid.snapshot.key.stats_epoch = 100; invalid.required_stats_epoch = 100; }
    if (mode == 2) invalid.required_object_uuids.push_back(Identity(10));
    if (mode == 3) invalid.required_security_policy_identity = {};
    if (mode == 4) invalid.snapshot.stats_snapshot.tables.clear();
    if (mode == 5) invalid.snapshot.stats_snapshot.tables[0].visible_row_count = 200;
    const auto statuses = opt::ValidatePinnedStatsForBenchmarkCleanAdmission(invalid);
    passed &= Require(std::ranges::any_of(statuses, [](const auto& value) {
      return !value.ok && value.diagnostic_code == "SB-STAT-0001";
    }) && std::ranges::all_of(statuses, [](const auto& value) {
      return value.ok || value.diagnostic_code == "SB-STAT-0001";
    }), "advisory/stale/incomplete benchmark context qualified or escaped registered diagnostics");
  }
  return passed;
}

bool FailureAtomicDispatch() {
  bool passed = true, finished = false; unsigned faults = 0;
  for (long allocation = 0; allocation != 2000; ++allocation) {
    opt::OptimizerPinnedStatsDescriptorCache cache;
    const auto a = PinnedSnapshot(Identity(10), Identity(11));
    const auto b = PinnedSnapshot(Identity(20), Identity(21));
    passed &= Require(cache.Put(a).ok && cache.Put(b).ok, "fault baseline");
    auto r = GenerationRequest(Kind::kStatsRefresh);
    r.reason = std::string(200, 'r');
    invalidation_fault::hit = false; invalidation_fault::remaining = allocation;
    const auto result = opt::DispatchOptimizerStatisticsInvalidation(r, &cache);
    invalidation_fault::remaining = -1;
    if (!invalidation_fault::hit) {
      passed &= Require(result.accepted && result.invalidated_entries.size() == 2 &&
                        !cache.Put(a).ok, "allocation sweep did not reach atomic commit");
      finished = true; break;
    }
    ++faults;
    passed &= Require(!result.accepted && result.invalidated_entries.empty() &&
        cache.Lookup(a.key).cache_hit && cache.Lookup(b.key).cache_hit &&
        cache.Put(a).ok && cache.Put(b).ok, "allocation failure published a partial invalidation");
  }
  std::cout << "statistics invalidation allocation_faults=" << faults << '\n';
  return Require(finished && faults > 10, "allocation sweep incomplete") && passed;
}

bool ConcurrentDelayedProducer() {
  bool passed = true;
  for (unsigned iteration = 0; iteration != 50; ++iteration) {
    opt::OptimizerPinnedStatsDescriptorCache cache;
    const auto pin = PinnedSnapshot(Identity(10), Identity(11));
    auto r = GenerationRequest(Kind::kSecurityGeneration);
    std::barrier start(2);
    std::jthread producer([&] { start.arrive_and_wait(); cache.Put(pin); });
    start.arrive_and_wait();
    const auto result = opt::DispatchOptimizerStatisticsInvalidation(r, &cache);
    producer.join();
    passed &= Require(result.accepted && !cache.Lookup(pin.key).cache_hit &&
                      !cache.Put(pin).ok, "delayed producer crossed committed fence");
  }
  return passed;
}

bool GlobalFencesAlsoRejectStaleAnalyzePublication() {
  auto& cache = opt::GlobalOptimizerPinnedStatsDescriptorCache();
  opt::OptimizerStatisticsStore store;
  auto snapshot = StatsSnapshot(Identity(1000), 20, 30);
  const auto first = store.PublishRelationSnapshot(snapshot);
  bool passed = Require(first.status == opt::OptimizerStatsPublicationStatus::kPublished,
                         "global fence publication baseline");
  auto event = GenerationRequest(Kind::kStatsRefresh);
  passed &= Require(opt::DispatchOptimizerStatisticsInvalidation(event, &cache).accepted,
                    "global statistics generation not committed");
  snapshot.snapshot_id = Identity(1001);
  snapshot.stats_epoch = snapshot.tables[0].identity.stats_epoch = 21;
  passed &= Require(store.PublishRelationSnapshot(snapshot).status ==
      opt::OptimizerStatsPublicationStatus::kStaleEpoch &&
      store.PublishedRelationSnapshot(Identity(1000)) == first.snapshot,
      "old ANALYZE generation crossed global statistics fence");
  snapshot.stats_epoch = snapshot.tables[0].identity.stats_epoch = 101;
  passed &= Require(store.PublishRelationSnapshot(snapshot).status ==
      opt::OptimizerStatsPublicationStatus::kPublished, "new statistics generation refused");
  event = GenerationRequest(Kind::kCatalogGeneration);
  passed &= Require(opt::DispatchOptimizerStatisticsInvalidation(event, &cache).accepted,
                    "global catalog generation not committed");
  snapshot.snapshot_id = Identity(1002);
  snapshot.stats_epoch = snapshot.tables[0].identity.stats_epoch = 102;
  passed &= Require(store.PublishRelationSnapshot(snapshot).status ==
      opt::OptimizerStatsPublicationStatus::kStaleEpoch, "old catalog crossed global fence");
  snapshot.catalog_epoch = snapshot.tables[0].identity.catalog_epoch = 101;
  passed &= Require(store.PublishRelationSnapshot(snapshot).status ==
      opt::OptimizerStatsPublicationStatus::kPublished, "current catalog/statistics batch refused");
  return passed;
}

}  // namespace

int RunTests() {
  if (!InvalidationDispatchesAnalyzeAndMetricGenerations()) return EXIT_FAILURE;
  if (!InvalidationRejectsUnsafeAuthority()) return EXIT_FAILURE;
  if (!BenchmarkCleanPinnedStatsRejectsStaleAndUnsafeSnapshots()) return EXIT_FAILURE;
  if (!EveryGenerationIsFenced()) return EXIT_FAILURE;
  if (!ScopedPolicyAndFilespaceEvents()) return EXIT_FAILURE;
  if (!RefusalsAndKeyBinding()) return EXIT_FAILURE;
  if (!BenchmarkRejectsAdvisoryAndForgedEpochs()) return EXIT_FAILURE;
  if (!FailureAtomicDispatch()) return EXIT_FAILURE;
  if (!ConcurrentDelayedProducer()) return EXIT_FAILURE;
  if (!GlobalFencesAlsoRejectStaleAnalyzePublication()) return EXIT_FAILURE;
  std::cout << "statistics invalidation checks=" << checks << '\n';
  return EXIT_SUCCESS;
}
int main() {
  try { return RunTests(); }
  catch (const std::exception& e) {
    invalidation_fault::remaining = -1;
    std::cerr << e.what() << '\n'; return EXIT_FAILURE;
  }
}
