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

enum class OptimizerStatisticTargetKind : std::uint8_t {
  kObject = 1,
  kLocalDefault,
  kClusterUnavailable,
};

// Default/policy inventory is a tagged source selection, not a fake object ID.
// There is intentionally no string constructor or wildcard target.
struct OptimizerStatisticTarget {
  OptimizerStatisticTargetKind kind{OptimizerStatisticTargetKind::kObject};
  planner::CanonicalPlannerUuid object_uuid;
  static OptimizerStatisticTarget Object(const planner::CanonicalPlannerUuid& uuid) {
    return {OptimizerStatisticTargetKind::kObject, uuid};
  }
  static OptimizerStatisticTarget LocalDefault() {
    return {OptimizerStatisticTargetKind::kLocalDefault, {}};
  }
  static OptimizerStatisticTarget ClusterUnavailable() {
    return {OptimizerStatisticTargetKind::kClusterUnavailable, {}};
  }
  bool Valid() const noexcept {
    if (kind == OptimizerStatisticTargetKind::kObject)
      return scratchbird::core::uuid::IsEngineIdentityUuid(object_uuid);
    return (kind == OptimizerStatisticTargetKind::kLocalDefault ||
            kind == OptimizerStatisticTargetKind::kClusterUnavailable) && object_uuid.is_nil();
  }
  bool operator==(const OptimizerStatisticTarget&) const = default;
};

enum class OptimizerStatisticValueDomain : std::uint8_t {
  kNonNegative = 1,
  kSignedCorrelation,
};

struct OptimizerStatistic {
  std::string statistic_name;
  std::string scope;
  OptimizerStatisticTarget target;
  double value = 0.0;
  StatisticSource source = StatisticSource::kUnavailable;
  std::uint64_t stats_epoch = 0;
  std::uint64_t freshness_microseconds = 0;
  CostConfidence confidence = CostConfidence::kUnknown;
  bool available = false;
  bool cluster_only = false;
  OptimizerStatisticValueDomain value_domain{OptimizerStatisticValueDomain::kNonNegative};
  std::optional<std::uint64_t> exact_unsigned_value;
};

enum class StatisticsContractReason : std::uint8_t {
  kNone = 0, kNameRequired, kScopeRequired, kTargetInvalid, kUnavailable,
  kSourceInvalid, kConfidenceUnusable, kStale, kValueInvalid, kEpochInvalid,
  kLocalDefault, kPolicyDefault, kClusterOnly, kMissing,
};

struct StatisticsContractStatus {
  bool ok = true;
  std::string diagnostic_code;
  std::string detail;
  StatisticsContractReason reason{StatisticsContractReason::kNone};
  planner::CanonicalPlannerUuid object_uuid;
};

class OptimizerStatisticsCatalog {
 public:
  [[nodiscard]] bool Add(const OptimizerStatistic& statistic) noexcept;
  std::optional<OptimizerStatistic> Find(const std::string& statistic_name,
                                         const OptimizerStatisticTarget& target) const;
  std::vector<StatisticsContractStatus> ValidateAll(std::uint64_t max_freshness_microseconds) const;
  std::uint64_t EstimateUnsigned(const std::string& statistic_name,
                                 const OptimizerStatisticTarget& target,
                                 std::uint64_t fallback) const;
  std::optional<std::uint64_t> TryEstimateUnsigned(
      const std::string& statistic_name, const OptimizerStatisticTarget& target) const;
  CostConfidence ConfidenceFor(const std::string& statistic_name,
                               const OptimizerStatisticTarget& target) const;
  bool UsesPolicyDefault(const std::string& statistic_name,
                         const OptimizerStatisticTarget& target) const;
  std::vector<StatisticsContractStatus> ValidateBenchmarkCleanInputs(
      const std::vector<std::string>& statistic_names,
      const OptimizerStatisticTarget& target) const;
  const std::vector<OptimizerStatistic>& Statistics() const { return statistics_; }

 private:
  std::vector<OptimizerStatistic> statistics_;
};

OptimizerStatistic MakeStatistic(std::string statistic_name,
                                 std::string scope,
                                 OptimizerStatisticTarget target,
                                 double value,
                                 StatisticSource source,
                                 std::uint64_t stats_epoch,
                                 std::uint64_t freshness_microseconds,
                                 CostConfidence confidence,
                                 bool available = true,
                                 bool cluster_only = false,
                                 OptimizerStatisticValueDomain value_domain = OptimizerStatisticValueDomain::kNonNegative);
OptimizerStatistic MakeUnsignedStatistic(std::string statistic_name,
                                 std::string scope, OptimizerStatisticTarget target,
                                 std::uint64_t value, StatisticSource source,
                                 std::uint64_t stats_epoch, std::uint64_t freshness_microseconds,
                                 CostConfidence confidence, bool available = true,
                                 bool cluster_only = false);
StatisticsContractStatus ValidateStatistic(const OptimizerStatistic& statistic,
                                           std::uint64_t max_freshness_microseconds);
std::optional<std::uint64_t> CheckedOptimizerStatisticUnsigned(
    const OptimizerStatistic& statistic) noexcept;
const char* StatisticSourceName(StatisticSource source);
OptimizerStatisticsCatalog DefaultLocalStatisticsCatalog();
[[nodiscard]] bool AddClusterUnavailableStatistics(OptimizerStatisticsCatalog* catalog) noexcept;

}  // namespace scratchbird::engine::optimizer
