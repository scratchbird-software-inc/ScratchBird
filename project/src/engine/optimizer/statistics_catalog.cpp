// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "statistics_catalog.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

constexpr std::uint64_t kBenchmarkCleanMaxFreshnessMicros = 60000000;

bool IsUsableConfidence(CostConfidence confidence) {
  return confidence == CostConfidence::kExact || confidence == CostConfidence::kHigh ||
         confidence == CostConfidence::kMedium || confidence == CostConfidence::kLow;
}

}  // namespace

void OptimizerStatisticsCatalog::Add(OptimizerStatistic statistic) {
  statistics_.push_back(std::move(statistic));
}

std::optional<OptimizerStatistic> OptimizerStatisticsCatalog::Find(const std::string& statistic_name,
                                                                   const std::string& object_uuid) const {
  auto it = std::find_if(statistics_.begin(), statistics_.end(), [&](const OptimizerStatistic& statistic) {
    return statistic.statistic_name == statistic_name && (object_uuid.empty() || statistic.object_uuid == object_uuid);
  });
  if (it == statistics_.end()) return std::nullopt;
  return *it;
}

std::vector<StatisticsContractStatus> OptimizerStatisticsCatalog::ValidateAll(std::uint64_t max_freshness_microseconds) const {
  std::vector<StatisticsContractStatus> statuses;
  for (const auto& statistic : statistics_) {
    statuses.push_back(ValidateStatistic(statistic, max_freshness_microseconds));
  }
  return statuses;
}

std::uint64_t OptimizerStatisticsCatalog::EstimateUnsigned(const std::string& statistic_name,
                                                          const std::string& object_uuid,
                                                          std::uint64_t fallback) const {
  const auto statistic = Find(statistic_name, object_uuid);
  if (!statistic || !statistic->available || statistic->value < 0.0) return fallback;
  return static_cast<std::uint64_t>(std::llround(statistic->value));
}

CostConfidence OptimizerStatisticsCatalog::ConfidenceFor(const std::string& statistic_name,
                                                        const std::string& object_uuid) const {
  const auto statistic = Find(statistic_name, object_uuid);
  if (!statistic || !statistic->available) return CostConfidence::kUnknown;
  return statistic->confidence;
}

bool OptimizerStatisticsCatalog::UsesPolicyDefault(const std::string& statistic_name,
                                                   const std::string& object_uuid) const {
  const auto statistic = Find(statistic_name, object_uuid);
  return statistic && statistic->available && statistic->source == StatisticSource::kPolicyDefault;
}

std::vector<StatisticsContractStatus> OptimizerStatisticsCatalog::ValidateBenchmarkCleanInputs(
    const std::vector<std::string>& statistic_names,
    const std::string& object_uuid) const {
  std::vector<StatisticsContractStatus> statuses;
  for (const auto& statistic_name : statistic_names) {
    const auto exact = Find(statistic_name, object_uuid);
    if (exact && exact->available && exact->object_uuid == "local.default") {
      statuses.push_back({false, "SB_OPTIMIZER_BENCHMARK_CLEAN.LOCAL_DEFAULT_STATS", statistic_name});
      continue;
    }
    if (exact && exact->available && exact->source == StatisticSource::kPolicyDefault) {
      statuses.push_back({false, "SB_OPTIMIZER_BENCHMARK_CLEAN.POLICY_DEFAULT_STATS", statistic_name});
      continue;
    }
    if (exact && exact->cluster_only) {
      statuses.push_back({false, "SB_OPTIMIZER_BENCHMARK_CLEAN.CLUSTER_ONLY_STATS", statistic_name});
      continue;
    }
    if (exact && (!exact->available || exact->source == StatisticSource::kUnavailable ||
                  !IsUsableConfidence(exact->confidence) || exact->stats_epoch == 0)) {
      statuses.push_back({false, "SB_OPTIMIZER_BENCHMARK_CLEAN.UNSUPPORTED_STATS", statistic_name});
      continue;
    }
    if (exact && exact->freshness_microseconds > kBenchmarkCleanMaxFreshnessMicros) {
      statuses.push_back({false, "SB_OPTIMIZER_BENCHMARK_CLEAN.STALE_STATS", statistic_name});
      continue;
    }
    if (exact && exact->available) {
      continue;
    }
    const auto local_default = Find(statistic_name, "local.default");
    if (local_default && local_default->available) {
      statuses.push_back({false, "SB_OPTIMIZER_BENCHMARK_CLEAN.LOCAL_DEFAULT_STATS", statistic_name});
      continue;
    }
    statuses.push_back({false, "SB_OPTIMIZER_BENCHMARK_CLEAN.STATS_MISSING", statistic_name});
  }
  if (statuses.empty()) statuses.push_back({true, "SB_OPTIMIZER_BENCHMARK_CLEAN.OK", object_uuid});
  return statuses;
}

OptimizerStatistic MakeStatistic(std::string statistic_name,
                                 std::string scope,
                                 std::string object_uuid,
                                 double value,
                                 StatisticSource source,
                                 std::uint64_t stats_epoch,
                                 std::uint64_t freshness_microseconds,
                                 CostConfidence confidence,
                                 bool available,
                                 bool cluster_only) {
  OptimizerStatistic statistic;
  statistic.statistic_name = std::move(statistic_name);
  statistic.scope = std::move(scope);
  statistic.object_uuid = std::move(object_uuid);
  statistic.value = value;
  statistic.source = source;
  statistic.stats_epoch = stats_epoch;
  statistic.freshness_microseconds = freshness_microseconds;
  statistic.confidence = confidence;
  statistic.available = available;
  statistic.cluster_only = cluster_only;
  return statistic;
}

StatisticsContractStatus ValidateStatistic(const OptimizerStatistic& statistic,
                                           std::uint64_t max_freshness_microseconds) {
  if (statistic.statistic_name.empty()) return {false, "SB_OPTIMIZER_STATS.NAME_REQUIRED", "statistic_name"};
  if (statistic.scope.empty()) return {false, "SB_OPTIMIZER_STATS.SCOPE_REQUIRED", statistic.statistic_name};
  if (!statistic.available) return {false, "SB_OPTIMIZER_STATS.UNAVAILABLE", statistic.statistic_name};
  if (statistic.source == StatisticSource::kUnavailable) return {false, "SB_OPTIMIZER_STATS.SOURCE_UNAVAILABLE", statistic.statistic_name};
  if (!IsUsableConfidence(statistic.confidence)) return {false, "SB_OPTIMIZER_STATS.CONFIDENCE_UNUSABLE", statistic.statistic_name};
  if (statistic.freshness_microseconds > max_freshness_microseconds) return {false, "SB_OPTIMIZER_STATS.STALE", statistic.statistic_name};
  return {true, "", statistic.statistic_name};
}

const char* StatisticSourceName(StatisticSource source) {
  switch (source) {
    case StatisticSource::kCatalogExact: return "catalog_exact";
    case StatisticSource::kCatalogSample: return "catalog_sample";
    case StatisticSource::kRuntimeMetric: return "runtime_metric";
    case StatisticSource::kPolicyDefault: return "policy_default";
    case StatisticSource::kClusterMetric: return "cluster_metric";
    case StatisticSource::kUnavailable: return "unavailable";
  }
  return "unavailable";
}

OptimizerStatisticsCatalog DefaultLocalStatisticsCatalog() {
  OptimizerStatisticsCatalog catalog;
  catalog.Add(MakeStatistic("row_count", "relation", "local.default", 1000.0, StatisticSource::kPolicyDefault, 1, 0, CostConfidence::kLow));
  catalog.Add(MakeStatistic("page_count", "relation", "local.default", 64.0, StatisticSource::kPolicyDefault, 1, 0, CostConfidence::kLow));
  catalog.Add(MakeStatistic("visible_fraction", "relation", "local.default", 1.0, StatisticSource::kRuntimeMetric, 1, 0, CostConfidence::kMedium));
  catalog.Add(MakeStatistic("index_depth", "index", "local.default", 3.0, StatisticSource::kPolicyDefault, 1, 0, CostConfidence::kLow));
  catalog.Add(MakeStatistic("memory_budget", "session", "local.default", 1048576.0, StatisticSource::kPolicyDefault, 1, 0, CostConfidence::kLow));
  catalog.Add(MakeStatistic("memory_grant_available_bytes", "session", "local.default", 1048576.0, StatisticSource::kPolicyDefault, 1, 0, CostConfidence::kLow));
  return catalog;
}

void AddClusterUnavailableStatistics(OptimizerStatisticsCatalog* catalog) {
  if (catalog == nullptr) {
    return;
  }
  catalog->Add(MakeStatistic("cluster_remote_stats_unavailable", "cluster", "cluster.unavailable", 0.0, StatisticSource::kUnavailable, 0, 0, CostConfidence::kRejected, false, true));
  catalog->Add(MakeStatistic("cluster_authority_unavailable", "cluster", "cluster.unavailable", 0.0, StatisticSource::kUnavailable, 0, 0, CostConfidence::kRejected, false, true));
  catalog->Add(MakeStatistic("cluster_route_generation_unavailable", "cluster", "cluster.unavailable", 0.0, StatisticSource::kUnavailable, 0, 0, CostConfidence::kRejected, false, true));
  catalog->Add(MakeStatistic("cluster_safe_execution_fence_unavailable", "cluster", "cluster.unavailable", 0.0, StatisticSource::kUnavailable, 0, 0, CostConfidence::kRejected, false, true));
  catalog->Add(MakeStatistic("cluster_remote_execution_unavailable", "cluster", "cluster.unavailable", 0.0, StatisticSource::kUnavailable, 0, 0, CostConfidence::kRejected, false, true));
}

}  // namespace scratchbird::engine::optimizer
