// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_METRICS_RETENTION_POLICY_MODEL
// Engine-owned metric retention policy model. Policies decide whether metric
// values remain current-only, persist raw samples, and/or persist rollups.

#include "metric_registry.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::metrics {

using scratchbird::core::platform::u64;

enum class MetricRetentionMode {
  current_only,
  raw_and_rollup,
  rollup_only,
  invalid = 255
};

enum class MetricRollupGrain {
  one_minute,
  one_hour,
  one_day,
  long_summary,
  invalid = 255
};

// Identity-free bootstrap/input definition. Never a live policy or lookup fallback.
struct MetricRetentionPolicyDefinition {
  std::string policy_name;
  std::string scope = "local";
  MetricRetentionMode mode = MetricRetentionMode::current_only;
  u64 raw_retention_seconds = 0;
  u64 rollup_retention_seconds = 0;
  std::vector<MetricRollupGrain> rollup_grains;
  u64 purge_batch_limit = 1024;
  u64 max_cardinality = 4096;
  std::string overflow_behavior = "reject_and_evidence";
  std::string edit_right = "OBS_METRICS_RETENTION_CONTROL";
  std::string default_admin_group = "OPS";
  bool evidence_required = true;
};

// Only catalog-owned runtime records carry identity and generation. Validation
// checks shape/semantics, not live catalog ownership or transaction publication.
struct MetricRetentionPolicy : MetricRetentionPolicyDefinition {
  MetricUuid policy_uuid;
  u64 generation = 0;
};

MetricValidationResult ValidateMetricRetentionPolicyDefinition(
    const MetricRetentionPolicyDefinition& definition);
bool MetricRetentionTimeExpired(u64 observation_microseconds,
                                u64 now_microseconds,
                                u64 retention_seconds) noexcept;

const char* MetricRetentionModeName(MetricRetentionMode mode);
const char* MetricRollupGrainName(MetricRollupGrain grain);
MetricRetentionMode MetricRetentionModeFromName(const std::string& value);
MetricRollupGrain MetricRollupGrainFromName(const std::string& value);
u64 MetricRollupGrainWindowSeconds(MetricRollupGrain grain);
MetricValidationResult ValidateMetricRetentionPolicy(const MetricRetentionPolicy& policy);
std::vector<MetricRetentionPolicyDefinition> MetricRetentionPolicyDefinitions();
const MetricRetentionPolicyDefinition* FindMetricRetentionPolicyDefinition(
    const std::string& name);

}  // namespace scratchbird::core::metrics
