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

namespace opt = scratchbird::engine::optimizer;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OEIC stats invalidation gate failure: " << message << '\n';
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

opt::OptimizerStatsSnapshot StatsSnapshot(const std::string& relation_uuid,
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
  store.UpsertTable(opt::BuildTableStatsFromAnalyzeSample(input));
  return store.Snapshot(relation_uuid + ":snapshot");
}

opt::OptimizerPinnedStatsDescriptorSnapshot PinnedSnapshot(
    const std::string& relation_uuid,
    const std::string& index_uuid,
    std::uint64_t stats_epoch = 20,
    std::uint64_t catalog_epoch = 30) {
  opt::OptimizerPinnedStatsDescriptorSnapshot snapshot;
  snapshot.key.catalog_epoch = catalog_epoch;
  snapshot.key.security_epoch = 40;
  snapshot.key.resource_policy_epoch = 50;
  snapshot.key.name_resolution_epoch = 60;
  snapshot.key.stats_epoch = stats_epoch;
  snapshot.key.descriptor_set_digest = "descriptor:" + relation_uuid;
  snapshot.key.object_uuids = {relation_uuid};
  snapshot.key.index_uuids = {index_uuid};
  snapshot.key.security_policy_identity = "security.policy";
  snapshot.key.redaction_policy_identity = "redaction.policy";
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
  const auto rel_a = PinnedSnapshot("rel.invalidate.a", "idx.invalidate.a");
  const auto rel_b = PinnedSnapshot("rel.invalidate.b", "idx.invalidate.b");
  if (!Require(cache.Put(rel_a).ok, "failed to pin rel A stats") ||
      !Require(cache.Put(rel_b).ok, "failed to pin rel B stats")) {
    return false;
  }

  opt::OptimizerStatisticsInvalidationRequest analyze;
  analyze.kind = opt::OptimizerStatisticsInvalidationKind::kAnalyzeGeneration;
  analyze.authority = Authority();
  analyze.object_uuid = "rel.invalidate.a";
  analyze.evidence_digest = "evidence:analyze";
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
  metric.evidence_digest = "evidence:storage-metric";
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
  request.evidence_digest = "evidence:unsafe";
  request.stats_epoch = 20;
  const auto result = opt::DispatchOptimizerStatisticsInvalidation(request, &cache);
  return Require(!result.accepted, "unsafe authority was accepted") &&
         Require(result.diagnostic_code == "SB_OPT_STATS_INVALIDATION_UNSAFE_AUTHORITY",
                 "unsafe authority diagnostic mismatch");
}

bool BenchmarkCleanPinnedStatsRejectsStaleAndUnsafeSnapshots() {
  auto snapshot = PinnedSnapshot("rel.benchmark.clean", "idx.benchmark.clean");
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

}  // namespace

int main() {
  if (!InvalidationDispatchesAnalyzeAndMetricGenerations()) return EXIT_FAILURE;
  if (!InvalidationRejectsUnsafeAuthority()) return EXIT_FAILURE;
  if (!BenchmarkCleanPinnedStatsRejectsStaleAndUnsafeSnapshots()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
