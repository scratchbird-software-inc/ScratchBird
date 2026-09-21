// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_retention_policy.hpp"
#include "metric_label_key.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::metrics {
namespace {

MetricRetentionPolicyDefinition Policy(std::string name,
                             std::string scope,
                             MetricRetentionMode mode,
                             u64 raw_seconds,
                             std::vector<MetricRollupGrain> grains,
                             u64 rollup_seconds,
                             u64 cardinality,
                             std::string overflow,
                             std::string edit_right,
                             std::string admin_group) {
  MetricRetentionPolicyDefinition policy;
  policy.policy_name = std::move(name);
  policy.scope = std::move(scope);
  policy.mode = mode;
  policy.raw_retention_seconds = raw_seconds;
  policy.rollup_grains = std::move(grains);
  policy.rollup_retention_seconds = rollup_seconds;
  policy.purge_batch_limit = 1024;
  policy.max_cardinality = cardinality;
  policy.overflow_behavior = std::move(overflow);
  policy.edit_right = std::move(edit_right);
  policy.default_admin_group = std::move(admin_group);
  policy.evidence_required = true;
  return policy;
}

const std::vector<MetricRetentionPolicyDefinition>& Baselines() {
  static const std::vector<MetricRetentionPolicyDefinition> policies = {
      Policy("metrics_current_only",
             "local",
             MetricRetentionMode::current_only,
             0,
             {},
             0,
             4096,
             "reject_and_evidence",
             "OBS_METRICS_RETENTION_CONTROL",
             "OPS"),
      Policy("metrics_short_raw_long_rollup",
             "local",
             MetricRetentionMode::raw_and_rollup,
             7ull * 24ull * 60ull * 60ull,
             {MetricRollupGrain::one_minute, MetricRollupGrain::one_hour, MetricRollupGrain::one_day},
             400ull * 24ull * 60ull * 60ull,
             4096,
             "reject_and_evidence",
             "OBS_METRICS_RETENTION_CONTROL",
             "OPS"),
      Policy("metrics_security_audit_long",
             "local",
             MetricRetentionMode::raw_and_rollup,
             400ull * 24ull * 60ull * 60ull,
             {MetricRollupGrain::one_hour, MetricRollupGrain::one_day},
             100ull * 365ull * 24ull * 60ull * 60ull,
             4096,
             "reject_and_evidence",
             "OBS_METRICS_RETENTION_CONTROL;SEC_GRANT_ADMIN",
             "SEC"),
      Policy("metrics_operational_400d_rollup",
             "local",
             MetricRetentionMode::raw_and_rollup,
             14ull * 24ull * 60ull * 60ull,
             {MetricRollupGrain::one_minute, MetricRollupGrain::one_hour, MetricRollupGrain::one_day},
             400ull * 24ull * 60ull * 60ull,
             4096,
             "overflow_only_if_not_automation",
             "OBS_METRICS_RETENTION_CONTROL",
             "OPS"),
      Policy("metrics_debug_ephemeral",
             "local",
             MetricRetentionMode::raw_and_rollup,
             24ull * 60ull * 60ull,
             {},
             7ull * 24ull * 60ull * 60ull,
             256,
             "reject_and_evidence",
             "OBS_METRICS_RETENTION_CONTROL",
             "DBA"),
      Policy("metrics_cluster_shared_history",
             "cluster",
             MetricRetentionMode::raw_and_rollup,
             7ull * 24ull * 60ull * 60ull,
             {MetricRollupGrain::one_minute, MetricRollupGrain::one_hour, MetricRollupGrain::one_day},
             400ull * 24ull * 60ull * 60ull,
             4096,
             "reject_and_evidence",
             "OBS_METRICS_RETENTION_CONTROL;OBS_CLUSTER_CONTROL",
             "OPS"),
  };
  return policies;
}

}  // namespace

const char* MetricRetentionModeName(MetricRetentionMode mode) {
  switch (mode) {
    case MetricRetentionMode::current_only: return "current_only";
    case MetricRetentionMode::raw_and_rollup: return "raw_and_rollup";
    case MetricRetentionMode::rollup_only: return "rollup_only";
    case MetricRetentionMode::invalid: return "invalid";
  }
  return "invalid";
}

const char* MetricRollupGrainName(MetricRollupGrain grain) {
  switch (grain) {
    case MetricRollupGrain::one_minute: return "1m";
    case MetricRollupGrain::one_hour: return "1h";
    case MetricRollupGrain::one_day: return "1d";
    case MetricRollupGrain::long_summary: return "long_summary";
    case MetricRollupGrain::invalid: return "invalid";
  }
  return "invalid";
}

MetricRetentionMode MetricRetentionModeFromName(const std::string& value) {
  if (value == "raw_and_rollup") {
    return MetricRetentionMode::raw_and_rollup;
  }
  if (value == "rollup_only") {
    return MetricRetentionMode::rollup_only;
  }
  if (value == "current_only") return MetricRetentionMode::current_only;
  return MetricRetentionMode::invalid;
}

MetricRollupGrain MetricRollupGrainFromName(const std::string& value) {
  if (value == "1h") {
    return MetricRollupGrain::one_hour;
  }
  if (value == "1d") {
    return MetricRollupGrain::one_day;
  }
  if (value == "long_summary") {
    return MetricRollupGrain::long_summary;
  }
  if (value == "1m") return MetricRollupGrain::one_minute;
  return MetricRollupGrain::invalid;
}

u64 MetricRollupGrainWindowSeconds(MetricRollupGrain grain) {
  switch (grain) {
    case MetricRollupGrain::one_minute: return 60;
    case MetricRollupGrain::one_hour: return 60 * 60;
    case MetricRollupGrain::one_day: return 24 * 60 * 60;
    case MetricRollupGrain::long_summary: return 30ull * 24ull * 60ull * 60ull;
    case MetricRollupGrain::invalid: return 0;
  }
  return 0;
}

MetricValidationResult ValidateMetricRetentionPolicyDefinition(
    const MetricRetentionPolicyDefinition& policy) {
  const auto invalid = [](std::string detail) {
    return MetricValidationResult{false, "METRIC.RETENTION_POLICY_INVALID", std::move(detail)};
  };
  const auto text = [](const std::string& value) {
    return !value.empty() && value.find('\0') == std::string::npos;
  };
  if (!text(policy.policy_name) || !text(policy.edit_right) ||
      !text(policy.default_admin_group)) return invalid("invalid_policy_annotation");
  if (policy.scope != "local" && policy.scope != "database" &&
      policy.scope != "node" && policy.scope != "cluster") return invalid("invalid_scope");
  if (!policy.purge_batch_limit || !policy.max_cardinality || !policy.evidence_required)
    return invalid("invalid_limits_or_missing_evidence");
  if (policy.overflow_behavior != "reject_and_evidence" &&
      policy.overflow_behavior != "quarantine_new_series" &&
      policy.overflow_behavior != "overflow_only_if_not_automation")
    return invalid("invalid_overflow_behavior");
  for (std::size_t i = 0; i < policy.rollup_grains.size(); ++i) {
    if (!MetricRollupGrainWindowSeconds(policy.rollup_grains[i]))
      return invalid("invalid_rollup_grain");
    if (std::find(policy.rollup_grains.begin(), policy.rollup_grains.begin() + i,
                  policy.rollup_grains[i]) != policy.rollup_grains.begin() + i)
      return invalid("duplicate_rollup_grain");
  }
  switch (policy.mode) {
    case MetricRetentionMode::current_only:
      if (policy.raw_retention_seconds || policy.rollup_retention_seconds ||
          !policy.rollup_grains.empty()) return invalid("current_only_has_history");
      break;
    case MetricRetentionMode::rollup_only:
      if (policy.raw_retention_seconds || !policy.rollup_retention_seconds ||
          policy.rollup_grains.empty()) return invalid("invalid_rollup_only_retention");
      break;
    case MetricRetentionMode::raw_and_rollup:
      if (!policy.raw_retention_seconds ||
          (!policy.rollup_grains.empty() && !policy.rollup_retention_seconds))
        return invalid("invalid_raw_and_rollup_retention");
      break;
    default:
      return invalid("invalid_retention_mode");
  }
  return {true, {}, {}};
}

MetricValidationResult ValidateMetricRetentionPolicy(const MetricRetentionPolicy& policy) {
  if (!MetricSystemUuidValid(policy.policy_uuid))
    return {false, "UUID.ENGINE_IDENTITY_NOT_V7", "metric_retention_policy_uuid"};
  if (!policy.generation)
    return {false, "METRIC.RETENTION_POLICY_INVALID", "zero_catalog_definition_generation"};
  return ValidateMetricRetentionPolicyDefinition(policy);
}

bool MetricRetentionTimeExpired(u64 observation, u64 now, u64 seconds) noexcept {
  if (!seconds || observation >= now) return false;
  const u64 age = now - observation;
  // Compare the exact age in microseconds without multiplying a uint64 duration.
  return age / 1000000 > seconds ||
         (age / 1000000 == seconds && age % 1000000 != 0);
}

std::vector<MetricRetentionPolicyDefinition> MetricRetentionPolicyDefinitions() {
  return Baselines();
}

const MetricRetentionPolicyDefinition* FindMetricRetentionPolicyDefinition(
    const std::string& name) {
  for (const auto& definition : Baselines())
    if (definition.policy_name == name) return &definition;
  return nullptr;
}

}  // namespace scratchbird::core::metrics
