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
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

constexpr std::uint64_t kBenchmarkCleanMaxFreshnessMicros = 60000000;
using Reason = StatisticsContractReason;

bool IsUsableConfidence(CostConfidence confidence) {
  return confidence == CostConfidence::kExact || confidence == CostConfidence::kHigh ||
         confidence == CostConfidence::kMedium || confidence == CostConfidence::kLow;
}

Reason StructuralReason(const OptimizerStatistic& statistic) noexcept {
  if (statistic.statistic_name.empty()) return Reason::kNameRequired;
  if (statistic.scope.empty()) return Reason::kScopeRequired;
  if (!statistic.target.Valid()) return Reason::kTargetInvalid;
  if (statistic.source < StatisticSource::kCatalogExact ||
      statistic.source > StatisticSource::kUnavailable) return Reason::kSourceInvalid;
  if (!std::isfinite(statistic.value)) return Reason::kValueInvalid;
  if (statistic.value_domain == OptimizerStatisticValueDomain::kNonNegative) {
    if (statistic.value < 0 ||
        (statistic.exact_unsigned_value &&
         statistic.value != static_cast<double>(*statistic.exact_unsigned_value)))
      return Reason::kValueInvalid;
  } else if (statistic.value_domain == OptimizerStatisticValueDomain::kSignedCorrelation) {
    if (statistic.value < -1 || statistic.value > 1 || statistic.exact_unsigned_value)
      return Reason::kValueInvalid;
  } else return Reason::kValueInvalid;
  if (statistic.target.kind == OptimizerStatisticTargetKind::kLocalDefault &&
      statistic.source != StatisticSource::kPolicyDefault &&
      statistic.source != StatisticSource::kUnavailable) return Reason::kSourceInvalid;
  if (statistic.target.kind == OptimizerStatisticTargetKind::kClusterUnavailable &&
      (statistic.available || !statistic.cluster_only ||
       statistic.source != StatisticSource::kUnavailable)) return Reason::kSourceInvalid;
  if (statistic.source == StatisticSource::kClusterMetric && !statistic.cluster_only)
    return Reason::kSourceInvalid;
  if (statistic.available) {
    if (statistic.source == StatisticSource::kUnavailable) return Reason::kSourceInvalid;
    if (!IsUsableConfidence(statistic.confidence)) return Reason::kConfidenceUnusable;
    if (statistic.stats_epoch == 0) return Reason::kEpochInvalid;
  }
  return Reason::kNone;
}

Reason UsabilityReason(const OptimizerStatistic& statistic,
                       std::uint64_t maximum_age) noexcept {
  const auto structural = StructuralReason(statistic);
  if (structural != Reason::kNone) return structural;
  if (!statistic.available) return Reason::kUnavailable;
  if (statistic.freshness_microseconds > maximum_age) return Reason::kStale;
  return Reason::kNone;
}

StatisticsContractStatus Status(Reason reason, const std::string& name,
                                const OptimizerStatisticTarget& target) {
  // Core's registered statistics diagnostic; the typed reason carries detail.
  return {reason == Reason::kNone, reason == Reason::kNone ? "" : "SB-STAT-0001",
          name, reason, target.object_uuid};
}
}  // namespace

bool OptimizerStatisticsCatalog::Add(const OptimizerStatistic& statistic) noexcept {
  if (StructuralReason(statistic) != Reason::kNone) return false;
  if (std::ranges::any_of(statistics_, [&](const auto& current) {
        return current.statistic_name == statistic.statistic_name &&
               current.target == statistic.target;
      })) return false;
  try {
    statistics_.push_back(statistic);
    return true;
  } catch (const std::bad_alloc&) {
    return false;
  } catch (const std::length_error&) {
    return false;
  }
}

std::optional<OptimizerStatistic> OptimizerStatisticsCatalog::Find(
    const std::string& statistic_name, const OptimizerStatisticTarget& target) const {
  if (!target.Valid()) return std::nullopt;
  const auto it = std::ranges::find_if(statistics_, [&](const auto& statistic) {
    return statistic.statistic_name == statistic_name && statistic.target == target;
  });
  if (it == statistics_.end()) return std::nullopt;
  return *it;
}

std::vector<StatisticsContractStatus> OptimizerStatisticsCatalog::ValidateAll(
    std::uint64_t max_freshness_microseconds) const {
  std::vector<StatisticsContractStatus> statuses;
  for (const auto& statistic : statistics_)
    statuses.push_back(ValidateStatistic(statistic, max_freshness_microseconds));
  return statuses;
}

std::optional<std::uint64_t> CheckedOptimizerStatisticUnsigned(
    const OptimizerStatistic& statistic) noexcept {
  if (UsabilityReason(statistic, kBenchmarkCleanMaxFreshnessMicros) != Reason::kNone)
    return std::nullopt;
  if (statistic.value_domain != OptimizerStatisticValueDomain::kNonNegative) return std::nullopt;
  if (statistic.exact_unsigned_value) return statistic.exact_unsigned_value;
  const long double rounded = std::round(static_cast<long double>(statistic.value));
  // Exclusive 2^64 bound also works when long double has double precision.
  if (rounded < 0 || rounded >= std::ldexp(1.0L, 64)) return std::nullopt;
  return static_cast<std::uint64_t>(rounded);
}

std::optional<std::uint64_t> OptimizerStatisticsCatalog::TryEstimateUnsigned(
    const std::string& statistic_name, const OptimizerStatisticTarget& target) const {
  const auto statistic = Find(statistic_name, target);
  return statistic ? CheckedOptimizerStatisticUnsigned(*statistic) : std::nullopt;
}

std::uint64_t OptimizerStatisticsCatalog::EstimateUnsigned(
    const std::string& statistic_name, const OptimizerStatisticTarget& target,
    std::uint64_t fallback) const {
  return TryEstimateUnsigned(statistic_name, target).value_or(fallback);
}

CostConfidence OptimizerStatisticsCatalog::ConfidenceFor(
    const std::string& statistic_name, const OptimizerStatisticTarget& target) const {
  const auto statistic = Find(statistic_name, target);
  if (!statistic || UsabilityReason(*statistic, kBenchmarkCleanMaxFreshnessMicros) != Reason::kNone)
    return CostConfidence::kUnknown;
  return statistic->confidence;
}

bool OptimizerStatisticsCatalog::UsesPolicyDefault(
    const std::string& statistic_name, const OptimizerStatisticTarget& target) const {
  const auto statistic = Find(statistic_name, target);
  return statistic && UsabilityReason(*statistic, kBenchmarkCleanMaxFreshnessMicros) == Reason::kNone &&
         statistic->source == StatisticSource::kPolicyDefault;
}

std::vector<StatisticsContractStatus> OptimizerStatisticsCatalog::ValidateBenchmarkCleanInputs(
    const std::vector<std::string>& statistic_names,
    const OptimizerStatisticTarget& target) const {
  std::vector<StatisticsContractStatus> statuses;
  for (const auto& name : statistic_names) {
    const auto exact = Find(name, target);
    Reason reason = Reason::kNone;
    if (!target.Valid()) reason = Reason::kTargetInvalid;
    else if (exact) {
      reason = UsabilityReason(*exact, kBenchmarkCleanMaxFreshnessMicros);
      if (reason == Reason::kNone) {
        if (exact->target.kind == OptimizerStatisticTargetKind::kLocalDefault)
          reason = Reason::kLocalDefault;
        else if (exact->source == StatisticSource::kPolicyDefault)
          reason = Reason::kPolicyDefault;
        else if (exact->cluster_only) reason = Reason::kClusterOnly;
        else if (exact->source != StatisticSource::kCatalogExact &&
                 exact->source != StatisticSource::kCatalogSample)
          reason = Reason::kSourceInvalid;
      }
    } else {
      const auto local = Find(name, OptimizerStatisticTarget::LocalDefault());
      reason = local && UsabilityReason(*local, kBenchmarkCleanMaxFreshnessMicros) == Reason::kNone
          ? Reason::kLocalDefault : Reason::kMissing;
    }
    if (reason != Reason::kNone) statuses.push_back(Status(reason, name, target));
  }
  if (statuses.empty()) {
    statuses.push_back(Status(target.Valid() ? Reason::kNone : Reason::kTargetInvalid, "", target));
  }
  return statuses;
}

OptimizerStatistic MakeStatistic(std::string statistic_name, std::string scope,
                                 OptimizerStatisticTarget target, double value,
                                 StatisticSource source, std::uint64_t stats_epoch,
                                 std::uint64_t freshness_microseconds,
                                 CostConfidence confidence, bool available,
                                 bool cluster_only, OptimizerStatisticValueDomain value_domain) {
  OptimizerStatistic statistic;
  statistic.statistic_name = std::move(statistic_name);
  statistic.scope = std::move(scope);
  statistic.target = target;
  statistic.value = value;
  statistic.source = source;
  statistic.stats_epoch = stats_epoch;
  statistic.freshness_microseconds = freshness_microseconds;
  statistic.confidence = confidence;
  statistic.available = available;
  statistic.cluster_only = cluster_only;
  statistic.value_domain = value_domain;
  return statistic;
}

OptimizerStatistic MakeUnsignedStatistic(std::string statistic_name,
                                 std::string scope, OptimizerStatisticTarget target,
                                 std::uint64_t value, StatisticSource source,
                                 std::uint64_t stats_epoch, std::uint64_t freshness_microseconds,
                                 CostConfidence confidence, bool available, bool cluster_only) {
  auto statistic = MakeStatistic(std::move(statistic_name), std::move(scope), target,
      static_cast<double>(value), source, stats_epoch, freshness_microseconds,
      confidence, available, cluster_only);
  statistic.exact_unsigned_value = value;
  return statistic;
}

StatisticsContractStatus ValidateStatistic(const OptimizerStatistic& statistic,
                                           std::uint64_t max_freshness_microseconds) {
  return Status(UsabilityReason(statistic, max_freshness_microseconds),
                statistic.statistic_name, statistic.target);
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
  const auto add = [&](const char* name, const char* scope, double value) {
    if (!catalog.Add(MakeStatistic(name, scope, OptimizerStatisticTarget::LocalDefault(),
                                  value, StatisticSource::kPolicyDefault, 1, 0, CostConfidence::kLow)))
      throw std::bad_alloc();
  };
  add("row_count", "relation", 1000);
  add("page_count", "relation", 64);
  add("visible_fraction", "relation", 1);
  add("index_depth", "index", 3);
  add("memory_budget", "session", 1048576);
  add("memory_grant_available_bytes", "session", 1048576);
  return catalog;
}

bool AddClusterUnavailableStatistics(OptimizerStatisticsCatalog* catalog) noexcept {
  if (catalog == nullptr) return false;
  try {
    auto staged = *catalog;
    for (const char* name : {"cluster_remote_stats_unavailable",
                            "cluster_authority_unavailable",
                            "cluster_route_generation_unavailable",
                            "cluster_safe_execution_fence_unavailable",
                            "cluster_remote_execution_unavailable"}) {
      const auto statistic = MakeStatistic(name, "cluster",
          OptimizerStatisticTarget::ClusterUnavailable(), 0, StatisticSource::kUnavailable,
          0, 0, CostConfidence::kRejected, false, true);
      if (!staged.Add(statistic)) return false;
    }
    *catalog = std::move(staged);
    return true;
  } catch (const std::bad_alloc&) {
    return false;
  } catch (const std::length_error&) {
    return false;
  }
}

}  // namespace scratchbird::engine::optimizer
