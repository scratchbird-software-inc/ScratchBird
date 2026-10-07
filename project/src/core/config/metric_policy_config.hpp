// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../metrics/metric_retention_policy.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string_view>

namespace scratchbird::core::config {

// Create/import inputs only. No UUIDs, grants, readiness or runtime fallbacks.
struct MetricPolicyDefinition {
  metrics::MetricRetentionPolicyDefinition retention;
  std::string read_right;
  bool redact_sensitive_labels = true;
};

struct MetricPolicyConfig {
  MetricPolicyDefinition operational_defaults;
  std::map<std::string, MetricPolicyDefinition, std::less<>> families;
};

struct MetricPolicyConfigResult {
  std::optional<MetricPolicyConfig> config;
  metrics::MetricValidationResult validation;
  bool ok() const { return config.has_value() && validation.ok; }
};

// Bounded, strict, all-or-nothing parsing. A file error never selects compiled
// defaults. Both files are required, with no environment expansion/includes.
MetricPolicyConfigResult ParseMetricPolicyConfig(std::string_view defaults,
                                                std::string_view families);
MetricPolicyConfigResult LoadMetricPolicyConfig(const std::filesystem::path& defaults,
                                               const std::filesystem::path& families);

// Exact opted-in family only; no prefix matching. An existing governing family
// definition takes precedence as a whole, including its security requirements.
// The catalog writer must still validate authority, assign binary identities,
// publish native records and bind generations. This API performs none of those.
struct MetricPolicySelection {
  std::optional<MetricPolicyDefinition> definition;
  metrics::MetricValidationResult validation;
  bool ok() const { return definition.has_value() && validation.ok; }
};
MetricPolicySelection SelectMetricPolicyDefinition(
    const MetricPolicyConfig& config, std::string_view family,
    const MetricPolicyDefinition* governing_definition = nullptr);

}  // namespace scratchbird::core::config
