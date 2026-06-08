// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_METRICS_DESCRIPTOR_REGISTRY
// SEARCH_KEY: ENTERPRISE_METRIC_SCHEMA
// Engine-owned metric descriptor registry and current-value store. Descriptor
// schema rows are redacted observability evidence and are not transaction,
// parser, recovery, or cluster authority.

#include "runtime_platform.hpp"

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::core::metrics {

using scratchbird::core::platform::u64;

enum class MetricType {
  counter,
  gauge,
  histogram,
  state,
  derived
};

enum class MetricUnit {
  count,
  bytes,
  microseconds,
  seconds,
  percent,
  ratio,
  state,
  none
};

enum class MetricVisibilityScope {
  baseline,
  self,
  family,
  all,
  cluster
};

enum class MetricReadiness {
  implemented,
  contract_ready_unwired,
  derived
};

struct MetricLabelDescriptor {
  std::string key;
  bool required = false;
  bool sensitive = false;
};

struct MetricDescriptor {
  std::string family;
  MetricType type = MetricType::counter;
  MetricUnit unit = MetricUnit::count;
  std::string namespace_path;
  std::string help;
  std::string producer_owner;
  std::string security_family;
  MetricVisibilityScope visibility = MetricVisibilityScope::family;
  MetricReadiness readiness = MetricReadiness::implemented;
  bool cluster_only = false;
  std::vector<MetricLabelDescriptor> labels;
  std::vector<std::string> aliases;
  std::vector<double> histogram_buckets;
};

struct MetricLabel {
  std::string key;
  std::string value;
};

using MetricLabelSet = std::vector<MetricLabel>;

struct MetricValue {
  std::string family;
  MetricLabelSet labels;
  MetricType type = MetricType::counter;
  double value = 0.0;
  u64 count = 0;
  double sum = 0.0;
  std::map<double, u64> buckets;
  std::string state_text;
};

struct MetricValidationResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
};

class MetricRegistry {
 public:
  MetricRegistry();

  MetricValidationResult RegisterDescriptor(MetricDescriptor descriptor);
  const MetricDescriptor* FindDescriptor(const std::string& family) const;
  const MetricDescriptor* FindDescriptorOrAlias(const std::string& family_or_alias) const;
  std::vector<MetricDescriptor> Descriptors(bool include_cluster = true) const;
  MetricValidationResult ValidateDescriptor(const MetricDescriptor& descriptor) const;
  MetricValidationResult ValidateLabels(const MetricDescriptor& descriptor, const MetricLabelSet& labels) const;

  MetricValidationResult IncrementCounter(const std::string& family,
                                          MetricLabelSet labels,
                                          double delta,
                                          const std::string& producer_owner);
  MetricValidationResult SetGauge(const std::string& family,
                                  MetricLabelSet labels,
                                  double value,
                                  const std::string& producer_owner);
  MetricValidationResult ObserveHistogram(const std::string& family,
                                          MetricLabelSet labels,
                                          double value,
                                          const std::string& producer_owner);
  MetricValidationResult SetState(const std::string& family,
                                  MetricLabelSet labels,
                                  double value,
                                  std::string state_text,
                                  const std::string& producer_owner);

  std::vector<MetricValue> SnapshotCurrent(bool include_cluster = true) const;
  std::vector<MetricValue> SnapshotHistory(bool include_cluster = true, u64 max_rows = 1024) const;

 private:
  MetricValidationResult UpdateValue(const MetricDescriptor& descriptor,
                                     MetricLabelSet labels,
                                     double value,
                                     std::string state_text,
                                     const std::string& producer_owner,
                                     MetricType operation_type);
  std::string NormalizeKey(const std::string& family, const MetricLabelSet& labels) const;
  void LoadBuiltinDescriptors();

  mutable std::mutex mutex_;
  std::map<std::string, MetricDescriptor> descriptors_;
  std::map<std::string, std::string> aliases_;
  std::map<std::string, MetricValue> current_values_;
  std::vector<MetricValue> history_values_;
};

MetricRegistry& DefaultMetricRegistry();

const char* MetricTypeName(MetricType type);
const char* MetricUnitName(MetricUnit unit);
const char* MetricVisibilityScopeName(MetricVisibilityScope scope);
const char* MetricReadinessName(MetricReadiness readiness);

MetricValidationResult MetricOk();
MetricValidationResult MetricError(std::string code, std::string detail);
MetricLabelSet RedactSensitiveLabels(const MetricDescriptor& descriptor,
                                      const MetricLabelSet& labels,
                                      bool allow_sensitive_labels);
MetricValue RedactSensitiveMetricValue(const MetricDescriptor& descriptor,
                                       MetricValue value,
                                       bool allow_sensitive_labels);

}  // namespace scratchbird::core::metrics
