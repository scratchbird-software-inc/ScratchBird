// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "cost_model.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_STATISTICS_CATALOG
// Statistics are optimizer inputs, not optimizer authority. Every statistic used
// by costing carries a source, freshness, and confidence. Missing or stale
// statistics force conservative costs or explicit rejection.
enum class StatisticSource {
  kCatalogExact,
  kCatalogSample,
  kRuntimeMetric,
  kPolicyDefault,
  kClusterMetric,
  kUnavailable,
};

struct OptimizerStatistic {
  std::string statistic_name;
  std::string scope;
  std::string object_uuid;
  double value = 0.0;
  StatisticSource source = StatisticSource::kUnavailable;
  std::uint64_t stats_epoch = 0;
  std::uint64_t freshness_microseconds = 0;
  CostConfidence confidence = CostConfidence::kUnknown;
  bool available = false;
  bool cluster_only = false;
};

struct StatisticsContractStatus {
  bool ok = true;
  std::string diagnostic_code;
  std::string detail;
};

class OptimizerStatisticsCatalog {
 public:
  void Add(OptimizerStatistic statistic);
  std::optional<OptimizerStatistic> Find(const std::string& statistic_name,
                                         const std::string& object_uuid = "") const;
  std::vector<StatisticsContractStatus> ValidateAll(std::uint64_t max_freshness_microseconds) const;
  std::uint64_t EstimateUnsigned(const std::string& statistic_name,
                                 const std::string& object_uuid,
                                 std::uint64_t fallback) const;
  CostConfidence ConfidenceFor(const std::string& statistic_name,
                               const std::string& object_uuid = "") const;
  bool UsesPolicyDefault(const std::string& statistic_name,
                         const std::string& object_uuid = "") const;
  std::vector<StatisticsContractStatus> ValidateBenchmarkCleanInputs(
      const std::vector<std::string>& statistic_names,
      const std::string& object_uuid) const;
  const std::vector<OptimizerStatistic>& Statistics() const { return statistics_; }

 private:
  std::vector<OptimizerStatistic> statistics_;
};

OptimizerStatistic MakeStatistic(std::string statistic_name,
                                 std::string scope,
                                 std::string object_uuid,
                                 double value,
                                 StatisticSource source,
                                 std::uint64_t stats_epoch,
                                 std::uint64_t freshness_microseconds,
                                 CostConfidence confidence,
                                 bool available = true,
                                 bool cluster_only = false);
StatisticsContractStatus ValidateStatistic(const OptimizerStatistic& statistic,
                                           std::uint64_t max_freshness_microseconds);
const char* StatisticSourceName(StatisticSource source);
OptimizerStatisticsCatalog DefaultLocalStatisticsCatalog();
void AddClusterUnavailableStatistics(OptimizerStatisticsCatalog* catalog);

}  // namespace scratchbird::engine::optimizer
