// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_production_analyze.hpp"
#include "../sbsql_sblr_alignment/binary_uuid_fixture.hpp"
#include <limits>
#include <new>
#include <set>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;

namespace fault {
thread_local long remaining = -1;
thread_local bool hit = false;
}
void* operator new(std::size_t n) {
  if (fault::remaining >= 0 && fault::remaining-- == 0) {
    fault::remaining = 0; fault::hit = true; throw std::bad_alloc();
  }
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using Uuid = scratchbird::engine::planner::CanonicalPlannerUuid;
using Target = opt::OptimizerStatisticTarget;
Uuid Id(unsigned id) {
  auto value = scratchbird::tests::BinaryUuid("01900000-0000-7000-8000-000000000000");
  for (unsigned i = 0; i != 4; ++i) value.bytes[15-i] = (id >> (i*8)) & 255;
  return value;
}
unsigned checks = 0, faults = 0;

bool Require(bool condition, const std::string& message) {
  ++checks;
  if (!condition) {
    std::cerr << "OEIC analyze statistics gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool HasStatus(const std::vector<opt::StatisticsContractStatus>& statuses,
               const std::string& code) {
  return std::any_of(statuses.begin(), statuses.end(), [&](const auto& status) {
    return status.diagnostic_code == code;
  });
}

opt::OptimizerStatsIdentity Identity(Uuid object,
                                     Uuid statistic,
                                     std::uint64_t stats_epoch = 20) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = std::move(object);
  identity.statistic_uuid = std::move(statistic);
  identity.stats_epoch = stats_epoch;
  identity.catalog_epoch = 30;
  identity.transaction_visibility_epoch = 40;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogSample;
  identity.confidence = opt::CostConfidence::kHigh;
  return identity;
}

opt::OptimizerProductionAnalyzeRequest BaseRequest(unsigned base = 0) {
  // SEARCH_KEY: OEIC_PRODUCTION_ANALYZE_STATISTICS
  opt::OptimizerProductionAnalyzeRequest request;
  request.analyze_run_uuid = Id(base + 1);
  request.relation_uuid = Id(base + 2);
  request.descriptor_set_digest = "descriptor-set:enterprise-analyze";
  request.storage_scan_evidence_digest = "storage-scan:evidence:1";
  request.sample_provenance_digest = "sample:provenance:1";
  request.result_contract_hash = "result-contract:analyze:1";
  request.sample_method = opt::OptimizerProductionAnalyzeSampleMethod::kStratifiedSample;
  request.sampled_rows = 5000;
  request.row_count = 10000;
  request.visible_row_count = 9600;
  request.page_count = 128;
  request.average_row_bytes = 192;
  request.catalog_epoch = 30;
  request.stats_epoch = 20;
  request.transaction_visibility_epoch = 40;
  request.security_epoch = 50;
  request.redaction_epoch = 60;
  request.resource_epoch = 70;
  request.source_generation = 80;
  request.benchmark_clean_profile = true;

  request.authority.engine_runtime_scope = true;
  request.authority.catalog_descriptor_authority = true;
  request.authority.catalog_stats_write_authority = true;
  request.authority.storage_scan_authority = true;
  request.authority.page_filespace_authority = true;
  request.authority.index_generation_authority = true;
  request.authority.metric_generation_authority = true;
  request.authority.mga_snapshot_authority = true;
  request.authority.transaction_inventory_authority = true;
  request.authority.security_context_authority = true;
  request.authority.grants_proven = true;
  request.authority.redaction_policy_bound = true;
  request.authority.stats_epoch_authority = true;

  opt::OptimizerProductionAnalyzeColumnSample column;
  column.column_uuid = Id(base + 3);
  column.descriptor_digest = "descriptor:column.enterprise.key";
  column.sample_rows = 5000;
  column.null_count = 48;
  column.distinct_count = 4200;
  column.hyperloglog_register_count = 4096;
  column.hyperloglog_estimated_distinct = 4210;
  column.hyperloglog_relative_error = 0.018;
  column.null_fraction = 0.005;
  column.correlation = 0.62;
  column.average_width_bytes = 16;
  column.min_encoded = "0001";
  column.max_encoded = "9999";
  column.histogram_buckets.push_back({"0001", "3000", 0.30, 2880});
  column.histogram_buckets.push_back({"3001", "7000", 0.40, 3840});
  column.histogram_buckets.push_back({"7001", "9999", 0.295, 2832});
  column.mcv_values.push_back({"0042", 0.08, 768});
  column.mcv_values.push_back({"0100", 0.04, 384});
  request.columns.push_back(column);

  opt::OptimizerProductionAnalyzeExpressionSample expression;
  expression.expression_digest = "expr:lower(column.enterprise.key)";
  expression.descriptor_digest = "descriptor:expression";
  expression.distinct_count = 3900;
  expression.null_fraction = 0.003;
  request.expressions.push_back(expression);

  opt::OptimizerProductionAnalyzeExtendedSample extended;
  extended.catalog_descriptor_proven = true;
  extended.stats.identity = Identity(request.relation_uuid,
                                     Id(base + 5));
  extended.stats.kind = opt::ExtendedOptimizerStatisticKind::kMultiColumnNdv;
  extended.stats.relation_uuid = request.relation_uuid;
  extended.stats.column_uuids = {Id(base + 3), Id(base + 4)};
  extended.stats.multi_column_distinct_count = 6400;
  extended.stats.functional_dependency_strength = 0.35;
  extended.stats.correlation_coefficient = 0.47;
  extended.stats.histogram_selectivity = 0.28;
  extended.stats.sampled_dependency_selectivity = 0.31;
  extended.stats.observed_selectivity_error = 0.06;
  extended.stats.mga_visibility_recheck_required = true;
  extended.stats.security_recheck_required = true;
  request.extended_stats.push_back(extended);

  opt::OptimizerProductionAnalyzeIndexSample index;
  index.captured_from_index_provider = true;
  index.route_capability_proven = true;
  index.stats.identity = Identity(request.relation_uuid, Id(base + 6));
  index.stats.index_uuid = Id(base + 7);
  index.stats.relation_uuid = request.relation_uuid;
  index.stats.index_family = "btree";
  index.stats.descriptor_digest = "descriptor:index.enterprise.key";
  index.stats.key_column_uuids = {Id(base + 3)};
  index.stats.height = 3;
  index.stats.leaf_pages = 48;
  index.stats.distinct_keys = 4200;
  index.stats.clustering_factor = 0.72;
  index.stats.fragmentation_ratio = 0.03;
  index.stats.visibility_coverage = 1.0;
  index.stats.predicate_coverage = 1.0;
  index.stats.equality_lookup_supported = true;
  index.stats.ordered_range_supported = true;
  index.stats.exact_recheck_required = true;
  index.stats.mga_recheck_required = true;
  index.stats.security_recheck_required = true;
  index.stats.route_benchmark_clean = true;
  request.indexes.push_back(index);

  opt::OptimizerProductionAnalyzePageFilespaceSample filespace;
  filespace.captured_from_page_manager = true;
  filespace.stats.identity = Identity(Id(base + 9),
                                      Id(base + 8));
  filespace.stats.filespace_uuid = Id(base + 9);
  filespace.stats.page_family = "relation";
  filespace.stats.page_size_bytes = 8192;
  filespace.stats.free_pages = 250000;
  filespace.stats.sequential_latency_score = 0.80;
  filespace.stats.random_latency_score = 3.20;
  filespace.stats.health_score = 1.0;
  filespace.stats.degraded = false;
  request.page_filespaces.push_back(filespace);
  return request;
}

void SetEpoch(opt::OptimizerProductionAnalyzeRequest* request, std::uint64_t epoch) {
  request->stats_epoch = epoch;
  for (auto& x : request->extended_stats) x.stats.identity.stats_epoch = epoch;
  for (auto& x : request->indexes) x.stats.identity.stats_epoch = epoch;
  for (auto& x : request->page_filespaces) x.stats.identity.stats_epoch = epoch;
}

opt::OptimizerPinnedStatsDescriptorSnapshot Pin(const opt::OptimizerProductionAnalyzeResult& result) {
  opt::OptimizerPinnedStatsDescriptorSnapshot pin;
  pin.stats_snapshot = *result.snapshot;
  pin.key.catalog_epoch = result.snapshot->catalog_epoch;
  pin.key.stats_epoch = result.snapshot->stats_epoch;
  pin.key.security_epoch = 50;
  pin.key.resource_policy_epoch = 70;
  pin.key.name_resolution_epoch = 90;
  pin.key.descriptor_set_digest = result.snapshot->collection.descriptor_set_digest;
  pin.key.object_uuids = {result.snapshot->tables[0].identity.object_uuid};
  for (const auto& f : result.snapshot->page_filespaces) pin.key.object_uuids.push_back(f.filespace_uuid);
  for (const auto& i : result.snapshot->indexes) pin.key.index_uuids.push_back(i.index_uuid);
  pin.key.security_policy_identity = Id(9001);
  pin.key.redaction_policy_identity = Id(9002);
  return pin;
}

bool RefusalHasNoPublication(const opt::OptimizerProductionAnalyzeResult& result) {
  return !result.statistics_published && !result.snapshot && !result.catalog_view &&
      !result.snapshot_valid && !result.benchmark_clean_ready && !result.pinned_stats_invalidated &&
      result.table_stats_written == 0 && result.column_stats_written == 0 &&
      result.histogram_stats_written == 0 && result.mcv_stats_written == 0 &&
      result.expression_stats_written == 0 && result.extended_stats_written == 0 &&
      result.index_stats_written == 0 && result.page_filespace_stats_written == 0;
}

bool ProductionAnalyzePublishesAllStatsFamilies() {
  opt::OptimizerStatisticsStore store;
  const auto request = BaseRequest();
  const auto result = opt::RunOptimizerProductionAnalyze(request, &store);
  const auto published = store.Snapshot(Id(10));
  const auto catalog = store.ToLegacyCatalog();

  if (!Require(result.statistics_published && result.snapshot && result.catalog_view, "publication prerequisites")) return false;
  const auto& s = *result.snapshot;
  const auto& binding = s.collection;
  if (!Require(binding.run_uuid == request.analyze_run_uuid &&
          binding.descriptor_set_digest == request.descriptor_set_digest &&
          binding.storage_scan_evidence_digest == request.storage_scan_evidence_digest &&
          binding.sample_provenance_digest == request.sample_provenance_digest &&
          binding.result_contract_hash == request.result_contract_hash &&
          binding.sampled_rows == request.sampled_rows && binding.source_generation == request.source_generation &&
          binding.transaction_visibility_epoch == request.transaction_visibility_epoch &&
          binding.security_epoch == request.security_epoch && binding.redaction_epoch == request.redaction_epoch &&
          binding.resource_epoch == request.resource_epoch, "actual collection binding retained")) return false;
  std::set<Uuid> identities;
  const auto issued = [&](const Uuid& id) {
    return !id.is_nil() && (id.bytes[6] >> 4) == 7 && (id.bytes[8] & 0xc0) == 0x80 && identities.insert(id).second;
  };
  if (!Require(issued(s.snapshot_id) && issued(s.tables[0].identity.statistic_uuid) &&
          issued(s.columns[0].identity.statistic_uuid) && issued(s.histograms[0].identity.statistic_uuid) &&
          issued(s.mcv[0].identity.statistic_uuid) && issued(s.mcv[1].identity.statistic_uuid) &&
          issued(s.expressions[0].identity.statistic_uuid), "new binary v7 identities are distinct")) return false;
  for (unsigned i = 0; i != 2; ++i) {
    const auto value = result.catalog_view->Find("mcv_frequency", Target::Object(s.mcv[i].identity.statistic_uuid));
    if (!Require(value && value->value == request.columns[0].mcv_values[i].frequency &&
            s.mcv[i].row_count == request.columns[0].mcv_values[i].row_count &&
            s.mcv[i].value_encoded == request.columns[0].mcv_values[i].value_encoded,
            "distinct MCV value, count and frequency preserved")) return false;
  }
  if (!Require(!result.catalog_view->Find("extended_correlation_coefficient", Target::Object(Id(5))) &&
          std::find(result.evidence.begin(), result.evidence.end(), "durable_catalog_write_performed=false") != result.evidence.end(),
          "no unrelated extended metric or durable-write success invented")) return false;

  return Require(result.statistics_published, "production analyze was refused: " + result.diagnostic_code) &&
         Require(result.snapshot && store.PublishedRelationSnapshot(request.relation_uuid) == result.snapshot,
                 "actual published snapshot owner missing") &&
         Require(result.snapshot_valid, "snapshot did not validate") &&
         Require(result.benchmark_clean_ready, "benchmark-clean catalog view was not ready") &&
         Require(result.table_stats_written == 1, "table stats count mismatch") &&
         Require(result.column_stats_written == 1, "column stats count mismatch") &&
         Require(result.histogram_stats_written == 1, "histogram stats count mismatch") &&
         Require(result.mcv_stats_written == 2, "MCV stats count mismatch") &&
         Require(result.expression_stats_written == 1, "expression stats count mismatch") &&
         Require(result.extended_stats_written == 1, "extended stats count mismatch") &&
         Require(result.index_stats_written == 1, "index stats count mismatch") &&
         Require(result.page_filespace_stats_written == 1, "page/filespace stats count mismatch") &&
         Require(published.tables.size() == 1 && published.columns.size() == 1 &&
                     published.histograms.size() == 1 && published.mcv.size() == 2 &&
                     published.expressions.size() == 1 &&
                     published.extended_stats.size() == 1 &&
                     published.indexes.size() == 1 &&
                     published.page_filespaces.size() == 1,
                 "published snapshot does not contain every stats family") &&
         Require(catalog.has_value(), "scalar projection missing") &&
         Require(catalog->Find("column_hll_estimated_distinct",
                              Target::Object(Id(3))).has_value(),
                 "HLL distinct stat missing from catalog view") &&
         Require(catalog->Find("expression_distinct_count",
                              Target::Object(result.snapshot->expressions[0].identity.statistic_uuid)).has_value(),
                 "expression stat missing from catalog view") &&
         Require(catalog->Find("extended_multi_column_distinct_count",
                              Target::Object(Id(5))).has_value(),
                 "extended stat missing from catalog view") &&
         Require(catalog->Find("index_route_benchmark_clean",
                              Target::Object(Id(7))).has_value(),
                 "index benchmark-clean stat missing") &&
         Require(catalog->Find("filespace_available_pages",
                              Target::Object(Id(8))).has_value(),
                 "page/filespace stat missing");
}

bool ProductionAnalyzeInvalidatesPinnedStats() {
  opt::OptimizerStatisticsStore store;
  auto request = BaseRequest();
  if (!opt::RunOptimizerProductionAnalyze(request, &store).statistics_published) {
    return Require(false, "initial analyze failed");
  }

  opt::OptimizerPinnedStatsDescriptorSnapshot pinned;
  pinned.key.catalog_epoch = request.catalog_epoch;
  pinned.key.security_epoch = request.security_epoch;
  pinned.key.resource_policy_epoch = request.resource_epoch;
  pinned.key.name_resolution_epoch = 90;
  pinned.key.stats_epoch = request.stats_epoch;
  pinned.key.descriptor_set_digest = request.descriptor_set_digest;
  pinned.key.object_uuids = {request.relation_uuid};
  pinned.key.index_uuids = {Id(7)};
  pinned.key.security_policy_identity = Id(11);
  pinned.key.redaction_policy_identity = Id(12);
  pinned.stats_snapshot = store.Snapshot(Id(13));
  const auto put = opt::GlobalOptimizerPinnedStatsDescriptorCache().Put(pinned);
  if (!Require(put.ok, "pinned stats cache put failed: " + put.diagnostic_code)) {
    return false;
  }

  request.analyze_run_uuid = Id(14);
  request.stats_epoch = 21;
  for (auto& x : request.extended_stats) x.stats.identity.stats_epoch = 21;
  for (auto& x : request.indexes) x.stats.identity.stats_epoch = 21;
  for (auto& x : request.page_filespaces) x.stats.identity.stats_epoch = 21;
  request.source_generation = 81;
  if (!opt::RunOptimizerProductionAnalyze(request, &store).statistics_published) {
    return Require(false, "refresh analyze failed");
  }
  const auto lookup = opt::GlobalOptimizerPinnedStatsDescriptorCache().Lookup(pinned.key);
  return Require(!lookup.ok &&
                     lookup.diagnostic_code == "SB_OPT_PINNED_STATS_CACHE_MISS",
                 "pinned stats cache was not invalidated");
}

bool ZeroAndSparseCollections() {
  opt::OptimizerStatisticsStore store;
  auto request = BaseRequest(100);
  request.sample_method = opt::OptimizerProductionAnalyzeSampleMethod::kFullScan;
  request.row_count = request.visible_row_count = request.sampled_rows = 0;
  request.page_count = request.average_row_bytes = 0;
  request.expressions.clear(); request.extended_stats.clear(); request.indexes.clear(); request.page_filespaces.clear();
  request.authority.page_filespace_authority = false;
  request.authority.index_generation_authority = false;
  request.authority.metric_generation_authority = false;
  auto& column = request.columns[0];
  column.sample_rows = column.null_count = column.distinct_count = 0;
  column.hyperloglog_estimated_distinct = 0;
  column.hyperloglog_relative_error = 0;
  column.null_fraction = column.correlation = 0;
  column.average_width_bytes = 0;
  column.histogram_buckets.clear(); column.mcv_values.clear(); column.min_encoded.clear(); column.max_encoded.clear();
  auto empty = opt::RunOptimizerProductionAnalyze(request, &store);
  if (!Require(empty.statistics_published && empty.snapshot->tables[0].row_count == 0 &&
          empty.snapshot->columns[0].distinct_count == 0 && empty.histogram_stats_written == 0 &&
          empty.mcv_stats_written == 0 && empty.index_stats_written == 0 && empty.benchmark_clean_ready,
          "empty relation publishes actual zero statistics without invented families")) return false;
  SetEpoch(&request, 21);
  request.row_count = request.visible_row_count = request.sampled_rows = 40;
  column.sample_rows = column.null_count = 40;
  column.null_fraction = 1.;
  auto all_null = opt::RunOptimizerProductionAnalyze(request, &store);
  if (!Require(all_null.statistics_published && all_null.snapshot->columns[0].null_fraction == 1. &&
          all_null.snapshot->columns[0].hyperloglog_estimated_distinct == 0,
          "all-null column retains zero HLL NDV")) return false;
  SetEpoch(&request, 22);
  request.columns.clear();
  auto sparse = opt::RunOptimizerProductionAnalyze(request, &store);
  return Require(sparse.statistics_published && sparse.column_stats_written == 0 &&
      store.Snapshot(Id(190)).columns.empty(), "table-only collection replaces omitted relation statistics");
}

bool ProviderProvenanceAndMultipleFilespaceFamilies() {
  auto request = BaseRequest(200);
  request.benchmark_clean_profile = false;
  request.page_filespaces[0].stats.identity.source = opt::StatisticSource::kRuntimeMetric;
  request.page_filespaces[0].stats.identity.confidence = opt::CostConfidence::kLow;
  auto second = request.page_filespaces[0];
  second.stats.identity.statistic_uuid = Id(250);
  second.stats.page_family = "index";
  second.stats.sequential_latency_score = 2.5;
  request.page_filespaces.push_back(second);
  opt::OptimizerStatisticsStore store;
  auto result = opt::RunOptimizerProductionAnalyze(request, &store);
  if (!Require(result.statistics_published && !result.benchmark_clean_ready &&
          result.snapshot->page_filespaces.size() == 2, "runtime measurements publish without benchmark overclaim")) return false;
  for (unsigned i = 0; i != 2; ++i) {
    const auto& retained = result.snapshot->page_filespaces[i];
    const auto value = result.catalog_view->Find("page_family_read_latency_microseconds",
        Target::Object(retained.identity.statistic_uuid));
    if (!Require(retained.identity.source == opt::StatisticSource::kRuntimeMetric &&
            retained.identity.confidence == opt::CostConfidence::kLow && value &&
            value->value == request.page_filespaces[i].stats.sequential_latency_score * 1000.,
            "page-family-specific identity and measured provenance preserved")) return false;
  }
  return true;
}

bool InvalidCollectionPreservesLiveState() {
  auto request = BaseRequest(300);
  opt::OptimizerStatisticsStore store;
  const auto initial = opt::RunOptimizerProductionAnalyze(request, &store);
  if (!Require(initial.statistics_published, "invalid-case baseline")) return false;
  const auto pin = Pin(initial);
  auto& cache = opt::GlobalOptimizerPinnedStatsDescriptorCache();
  if (!Require(cache.Put(pin).ok, "invalid-case pin")) return false;
  for (unsigned test = 0; test != 23; ++test) {
    auto bad = request;
    SetEpoch(&bad, 21);
    switch (test) {
      case 0: bad.analyze_run_uuid = {}; break;
      case 1: bad.relation_uuid.bytes[6] = 0x40; break;
      case 2: bad.columns[0].column_uuid = {}; break;
      case 3: bad.columns[0].null_fraction = std::numeric_limits<double>::quiet_NaN(); break;
      case 4: bad.columns[0].hyperloglog_relative_error = std::numeric_limits<double>::infinity(); break;
      case 5: bad.columns[0].correlation = std::numeric_limits<double>::quiet_NaN(); break;
      case 6: bad.columns[0].histogram_buckets[0].row_count = std::numeric_limits<std::uint64_t>::max(); break;
      case 7: bad.columns[0].mcv_values[0].frequency = std::numeric_limits<double>::quiet_NaN(); break;
      case 8: bad.columns[0].mcv_values[0].row_count = 1; break;
      case 9: bad.indexes[0].stats.identity.stats_epoch = 20; break;
      case 10: bad.extended_stats[0].stats.identity.source = opt::StatisticSource::kPolicyDefault; break;
      case 11: bad.page_filespaces[0].stats.identity.object_uuid = bad.relation_uuid; break;
      case 12: bad.sample_method = static_cast<opt::OptimizerProductionAnalyzeSampleMethod>(255); break;
      case 13: bad.sample_method = opt::OptimizerProductionAnalyzeSampleMethod::kFullScan; break;
      case 14: bad.columns.push_back(bad.columns[0]); break;
      case 15: bad.expressions.push_back(bad.expressions[0]); break;
      case 16: bad.columns[0].mcv_values.push_back(bad.columns[0].mcv_values[0]); break;
      case 17: bad.indexes[0].stats.route_benchmark_clean = false; break;
      case 18: bad.extended_stats[0].stats.identity.transaction_visibility_epoch = 39; break;
      case 19: bad.columns[0].sample_rows = 0; break;
      case 20: bad.page_filespaces[0].stats.identity.source = opt::StatisticSource::kRuntimeMetric;
               bad.authority.metric_generation_authority = false; break;
      case 21: SetEpoch(&bad, 20); break;
      case 22: bad.extended_stats[0].stats.kind = static_cast<opt::ExtendedOptimizerStatisticKind>(255); break;
    }
    auto result = opt::RunOptimizerProductionAnalyze(bad, &store);
    if (!Require(RefusalHasNoPublication(result), "invalid request has no success counts, catalog or snapshot receipt") ||
        !Require(store.PublishedRelationSnapshot(request.relation_uuid) == initial.snapshot && cache.Lookup(pin.key).cache_hit,
            "request, snapshot and benchmark validation cannot invalidate live state")) return false;
  }
  return true;
}

bool ExtendedFamilyProjection() {
  const std::vector<std::string> names = {"extended_multi_column_distinct_count",
      "extended_functional_dependency_strength", "extended_correlation_coefficient",
      "extended_histogram_selectivity", "extended_sampled_dependency_selectivity"};
  // Independent expected scalar presence for each of the eight defined kinds.
  const unsigned expected[] = {1, 1, 2, 4, 8, 16, 0, 1|8|16};
  for (unsigned kind = 0; kind != 8; ++kind) {
    auto request = BaseRequest(1000 + kind*100);
    auto& record = request.extended_stats[0].stats;
    record.kind = static_cast<opt::ExtendedOptimizerStatisticKind>(kind);
    if (kind == 1) record.joint_mcv = {{{"a", "b"}, .2}, {{"c", "d"}, .1}};
    record.fk_pk_estimated_rows = 0;
    opt::OptimizerStatisticsStore store;
    const auto result = opt::RunOptimizerProductionAnalyze(request, &store);
    if (!Require(result.statistics_published && result.benchmark_clean_ready,
            "all extended families retain their defined snapshot payload")) return false;
    for (unsigned metric = 0; metric != names.size(); ++metric) {
      if (!Require(result.catalog_view->Find(names[metric], Target::Object(record.identity.statistic_uuid)).has_value() ==
              bool(expected[kind] & (1u << metric)), "only metrics belonging to the extended kind are projected")) return false;
    }
    const auto& published = result.snapshot->extended_stats[0];
    if (!Require(published.kind == record.kind && published.joint_mcv.size() == record.joint_mcv.size() &&
            published.fk_pk_estimated_rows == 0, "typed distributions and zero join estimate are not discarded")) return false;
  }
  return true;
}

bool AllocationFailurePreservesLiveState() {
  auto request = BaseRequest(400);
  auto& cache = opt::GlobalOptimizerPinnedStatsDescriptorCache();
  for (long allocation = 0; allocation != 4000; ++allocation) {
    cache.Clear();
    opt::OptimizerStatisticsStore store;
    const auto initial = opt::RunOptimizerProductionAnalyze(request, &store);
    if (!Require(initial.statistics_published, "allocation baseline publication")) return false;
    const auto pin = Pin(initial);
    if (!Require(cache.Put(pin).ok, "allocation baseline pin")) return false;
    auto next = request;
    SetEpoch(&next, 21);
    fault::hit = false;
    fault::remaining = allocation;
    const auto result = opt::RunOptimizerProductionAnalyze(next, &store);
    fault::remaining = -1;
    if (!fault::hit) {
      return Require(result.statistics_published && result.snapshot->stats_epoch == 21 &&
          result.snapshot == store.PublishedRelationSnapshot(request.relation_uuid) && !cache.Lookup(pin.key).cache_hit,
          "sweep reaches successful single publication") && Require(faults > 50, "actual producer allocations exercised");
    }
    ++faults;
    if (!Require(RefusalHasNoPublication(result), "allocation failure leaves no fake success receipt") ||
        !Require(initial.snapshot == store.PublishedRelationSnapshot(request.relation_uuid) &&
            cache.Lookup(pin.key).cache_hit && cache.Put(pin).ok,
            "allocation failure preserves prior snapshot, pin and insertion epoch fence")) return false;
  }
  return Require(false, "allocation sweep did not reach successful publication");
}

bool ProductionAnalyzeRejectsUnsafeAuthorityAndBenchmarkOverclaim() {
  opt::OptimizerStatisticsStore store;
  auto unsafe = BaseRequest();
  unsafe.authority.parser_or_reference_authority = true;
  const auto unsafe_result = opt::RunOptimizerProductionAnalyze(unsafe, &store);

  auto overclaim = BaseRequest();
  overclaim.indexes.front().stats.route_benchmark_clean = false;
  const auto overclaim_result = opt::RunOptimizerProductionAnalyze(overclaim, &store);

  return Require(!unsafe_result.statistics_published,
                 "parser/reference authority was accepted") &&
         Require(HasStatus(unsafe_result.validation_statuses,
                           "SB_OPT_ANALYZE_UNSAFE_AUTHORITY"),
                 "unsafe authority diagnostic missing") &&
         Require(!overclaim_result.statistics_published,
                 "benchmark-clean index overclaim was accepted") &&
         Require(HasStatus(overclaim_result.benchmark_clean_statuses,
                           "SB_OPT_ANALYZE_INDEX_ROUTE_NOT_BENCHMARK_CLEAN"),
                 "benchmark-clean overclaim diagnostic missing");
}

}  // namespace

int main() {
  if (!ProductionAnalyzePublishesAllStatsFamilies()) return EXIT_FAILURE;
  if (!ProductionAnalyzeInvalidatesPinnedStats()) return EXIT_FAILURE;
  if (!ProductionAnalyzeRejectsUnsafeAuthorityAndBenchmarkOverclaim()) {
    return EXIT_FAILURE;
  }
  if (!ZeroAndSparseCollections() || !ProviderProvenanceAndMultipleFilespaceFamilies() ||
      !InvalidCollectionPreservesLiveState() || !ExtendedFamilyProjection() ||
      !AllocationFailurePreservesLiveState()) return EXIT_FAILURE;
  std::cout << "PASS ANALYZE in-memory publication checks=" << checks << " allocation_faults=" << faults << '\n';
  return EXIT_SUCCESS;
}
