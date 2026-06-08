// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_retention_policy.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::metrics {
namespace {

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool Contains(const std::string& value, const std::string& needle) {
  return value.find(needle) != std::string::npos;
}

MetricRetentionPolicy Policy(std::string uuid,
                             std::string name,
                             std::string scope,
                             MetricRetentionMode mode,
                             u64 raw_seconds,
                             std::vector<MetricRollupGrain> grains,
                             u64 rollup_seconds,
                             u64 cardinality,
                             std::string overflow,
                             std::string edit_right,
                             std::string admin_group) {
  MetricRetentionPolicy policy;
  policy.policy_uuid = std::move(uuid);
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

const std::vector<MetricRetentionPolicy>& Baselines() {
  static const std::vector<MetricRetentionPolicy> policies = {
      Policy("seed-metrics-current-only",
             "metrics_current_only",
             "local",
             MetricRetentionMode::current_only,
             0,
             {},
             0,
             4096,
             "reject_and_evidence",
             "OBS_METRICS_RETENTION_CONTROL",
             "OPS"),
      Policy("seed-metrics-short-raw-long-rollup",
             "metrics_short_raw_long_rollup",
             "local",
             MetricRetentionMode::raw_and_rollup,
             7ull * 24ull * 60ull * 60ull,
             {MetricRollupGrain::one_minute, MetricRollupGrain::one_hour, MetricRollupGrain::one_day},
             400ull * 24ull * 60ull * 60ull,
             4096,
             "reject_and_evidence",
             "OBS_METRICS_RETENTION_CONTROL",
             "OPS"),
      Policy("seed-metrics-security-audit-long",
             "metrics_security_audit_long",
             "local",
             MetricRetentionMode::raw_and_rollup,
             400ull * 24ull * 60ull * 60ull,
             {MetricRollupGrain::one_hour, MetricRollupGrain::one_day},
             100ull * 365ull * 24ull * 60ull * 60ull,
             4096,
             "reject_and_evidence",
             "OBS_METRICS_RETENTION_CONTROL;SEC_GRANT_ADMIN",
             "SEC"),
      Policy("seed-metrics-operational-400d-rollup",
             "metrics_operational_400d_rollup",
             "local",
             MetricRetentionMode::raw_and_rollup,
             14ull * 24ull * 60ull * 60ull,
             {MetricRollupGrain::one_minute, MetricRollupGrain::one_hour, MetricRollupGrain::one_day},
             400ull * 24ull * 60ull * 60ull,
             4096,
             "overflow_only_if_not_automation",
             "OBS_METRICS_RETENTION_CONTROL",
             "OPS"),
      Policy("seed-metrics-debug-ephemeral",
             "metrics_debug_ephemeral",
             "local",
             MetricRetentionMode::raw_and_rollup,
             24ull * 60ull * 60ull,
             {},
             7ull * 24ull * 60ull * 60ull,
             256,
             "reject_and_evidence",
             "OBS_METRICS_RETENTION_CONTROL",
             "DBA"),
      Policy("seed-metrics-cluster-shared-history",
             "metrics_cluster_shared_history",
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
  }
  return "current_only";
}

const char* MetricRollupGrainName(MetricRollupGrain grain) {
  switch (grain) {
    case MetricRollupGrain::one_minute: return "1m";
    case MetricRollupGrain::one_hour: return "1h";
    case MetricRollupGrain::one_day: return "1d";
    case MetricRollupGrain::long_summary: return "long_summary";
  }
  return "1m";
}

MetricRetentionMode MetricRetentionModeFromName(const std::string& value) {
  if (value == "raw_and_rollup") {
    return MetricRetentionMode::raw_and_rollup;
  }
  if (value == "rollup_only") {
    return MetricRetentionMode::rollup_only;
  }
  return MetricRetentionMode::current_only;
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
  return MetricRollupGrain::one_minute;
}

u64 MetricRollupGrainWindowSeconds(MetricRollupGrain grain) {
  switch (grain) {
    case MetricRollupGrain::one_minute: return 60;
    case MetricRollupGrain::one_hour: return 60 * 60;
    case MetricRollupGrain::one_day: return 24 * 60 * 60;
    case MetricRollupGrain::long_summary: return 30ull * 24ull * 60ull * 60ull;
  }
  return 60;
}

MetricValidationResult ValidateMetricRetentionPolicy(const MetricRetentionPolicy& policy) {
  if (policy.policy_uuid.empty() || policy.policy_name.empty()) {
    return MetricError("SB-METRICS-HISTORY-POLICY-INVALID", "missing policy uuid or name");
  }
  if (policy.scope != "local" && policy.scope != "database" && policy.scope != "node" && policy.scope != "cluster") {
    return MetricError("SB-METRICS-HISTORY-POLICY-INVALID", policy.policy_name + ":invalid_scope");
  }
  if (policy.purge_batch_limit == 0 || policy.max_cardinality == 0) {
    return MetricError("SB-METRICS-HISTORY-POLICY-INVALID", policy.policy_name + ":invalid_limits");
  }
  if (!policy.evidence_required) {
    return MetricError("SB-METRICS-HISTORY-POLICY-INVALID", policy.policy_name + ":evidence_required_false");
  }
  if (policy.mode == MetricRetentionMode::current_only) {
    if (policy.raw_retention_seconds != 0 || policy.rollup_retention_seconds != 0 || !policy.rollup_grains.empty()) {
      return MetricError("SB-METRICS-HISTORY-POLICY-INVALID", policy.policy_name + ":current_only_has_history");
    }
  }
  if (policy.mode == MetricRetentionMode::rollup_only && policy.rollup_retention_seconds == 0) {
    return MetricError("SB-METRICS-HISTORY-POLICY-INVALID", policy.policy_name + ":rollup_only_without_rollup_retention");
  }
  if (policy.mode == MetricRetentionMode::raw_and_rollup && policy.raw_retention_seconds == 0) {
    return MetricError("SB-METRICS-HISTORY-POLICY-INVALID", policy.policy_name + ":raw_and_rollup_without_raw_retention");
  }
  if (policy.overflow_behavior != "reject_and_evidence" &&
      policy.overflow_behavior != "quarantine_new_series" &&
      policy.overflow_behavior != "overflow_only_if_not_automation") {
    return MetricError("SB-METRICS-HISTORY-POLICY-INVALID", policy.policy_name + ":invalid_overflow_behavior");
  }
  return MetricOk();
}

std::vector<MetricRetentionPolicy> BaselineMetricRetentionPolicies() {
  return Baselines();
}

const MetricRetentionPolicy* FindBaselineMetricRetentionPolicy(const std::string& policy_name_or_uuid) {
  for (const auto& policy : Baselines()) {
    if (policy.policy_name == policy_name_or_uuid || policy.policy_uuid == policy_name_or_uuid) {
      return &policy;
    }
  }
  return nullptr;
}

const MetricRetentionPolicy& DefaultMetricRetentionPolicyForDescriptor(const MetricDescriptor& descriptor) {
  const auto& policies = Baselines();
  if (descriptor.readiness == MetricReadiness::contract_ready_unwired) {
    return policies[0];
  }
  if (descriptor.cluster_only || StartsWith(descriptor.namespace_path, "cluster.sys.metrics")) {
    return policies[5];
  }
  if (StartsWith(descriptor.namespace_path, "sys.metrics.security")) {
    return policies[2];
  }
  if (Contains(descriptor.namespace_path, ".debug") || Contains(descriptor.producer_owner, "probe")) {
    return policies[4];
  }
  if (StartsWith(descriptor.namespace_path, "sys.metrics.archive") ||
      StartsWith(descriptor.namespace_path, "sys.metrics.backup") ||
      StartsWith(descriptor.namespace_path, "sys.metrics.scheduler") ||
      StartsWith(descriptor.namespace_path, "sys.metrics.jobs") ||
      StartsWith(descriptor.namespace_path, "sys.metrics.storage") ||
      StartsWith(descriptor.namespace_path, "sys.metrics.memory") ||
      StartsWith(descriptor.namespace_path, "sys.metrics.transactions") ||
      StartsWith(descriptor.namespace_path, "sys.metrics.optimizer") ||
      StartsWith(descriptor.namespace_path, "sys.metrics.indexes") ||
      StartsWith(descriptor.namespace_path, "sys.metrics.agents") ||
      StartsWith(descriptor.namespace_path, "sys.metrics.alerts")) {
    return policies[3];
  }
  return policies[1];
}

}  // namespace scratchbird::core::metrics
