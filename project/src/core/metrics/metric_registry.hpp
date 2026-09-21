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
#include "metric_scalar.hpp"

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace scratchbird::core::metrics {

using scratchbird::core::platform::u64;

enum class MetricType {
  counter,
  gauge,
  histogram,
  state,
  rate,
  sample,
  derived = 255  // Retired prototype spelling, never a canonical class.
};

enum class MetricUnit {
  bytes,
  pages,
  records,
  transactions,
  operations,
  seconds,
  milliseconds,
  microseconds,
  nanoseconds,
  percent,
  ratio,
  revisions,
  events,
  errors,
  conflicts,
  none,
  count = 254,
  state = 255
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
  derived,
  unvalidated
};

enum class MetricLabelType { text, system_uuid, uuid_value };

struct MetricLabelDescriptor {
  std::string key;
  bool required = false;
  bool sensitive = false;
  MetricLabelType value_type = MetricLabelType::text;
};

// Serializable definition/template. It does not claim identity or readiness.
struct MetricDescriptorDefinition {
  std::string family;
  MetricType type = MetricType::counter;
  MetricUnit unit = MetricUnit::none;
  std::string namespace_path;
  std::string help;
  std::string producer_owner;
  std::string security_family;
  MetricVisibilityScope visibility = MetricVisibilityScope::family;
  bool cluster_only = false;
  std::vector<MetricLabelDescriptor> labels;
  std::vector<std::string> aliases;
  std::vector<MetricScalar> histogram_buckets;
  bool histogram_cumulative = true;
  MetricScalarType value_type = MetricScalarType::invalid;
  std::optional<MetricScalar> min_value;
  std::optional<MetricScalar> max_value;
  std::vector<std::uint64_t> enum_values;
  u64 rate_window_nanoseconds = 0;
};
struct MetricDescriptorBinding {
  MetricUuid metric_uuid;
  u64 descriptor_generation = 0;
  MetricUuid label_schema_uuid;
  u64 label_schema_generation = 0;
  MetricUuid retention_policy_uuid;
  u64 retention_policy_generation = 0;
  MetricUuid visibility_policy_uuid;
  u64 visibility_policy_generation = 0;
  MetricUuid rate_source_counter_uuid;
  u64 rate_source_counter_generation = 0;
  bool operator==(const MetricDescriptorBinding&) const = default;
};
struct MetricDescriptor : MetricDescriptorDefinition, MetricDescriptorBinding {
  MetricReadiness readiness = MetricReadiness::unvalidated;
};

struct MetricLabelValue : std::variant<std::string, MetricUuid> {
  using Base=std::variant<std::string,MetricUuid>;
  using Base::Base;
  using Base::operator=;
  MetricLabelValue()=default;
  MetricLabelValue(const MetricLabelValue& other):Base(Clone(other)){}
  MetricLabelValue(MetricLabelValue&&) noexcept=default;
  MetricLabelValue& operator=(const MetricLabelValue& other) {
    if(this!=&other){MetricLabelValue copy(other);*this=std::move(copy);}return *this;
  }
  MetricLabelValue& operator=(MetricLabelValue&&) noexcept=default;
 private:
  static Base Clone(const MetricLabelValue& other) {
    return std::visit([](const auto& value)->Base {
      return Base(std::in_place_type<std::decay_t<decltype(value)>>,value);
    },static_cast<const Base&>(other));
  }
};

struct MetricLabel {
  std::string key;
  MetricLabelValue value;
};

using MetricLabelSet = std::vector<MetricLabel>;
// Exact typed series identity. No delimiter concatenation or UUID rendering.
using MetricSeriesKey =
    std::pair<std::string, std::vector<std::pair<std::string, MetricLabelValue>>>;

struct MetricValue {
  std::string family;
  MetricLabelSet labels;
  MetricType type = MetricType::counter;
  MetricScalar value;
  u64 count = 0;
  MetricScalar sum;
  // Finite descriptor bounds followed by the terminal unbounded bucket.
  std::vector<u64> buckets;
  std::vector<MetricScalar> bucket_bounds;
  bool buckets_cumulative = true;
  bool arithmetic_inexact = false;
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
                                          MetricScalar delta,
                                          const std::string& producer_owner);
  MetricValidationResult SetGauge(const std::string& family,
                                  MetricLabelSet labels,
                                  MetricScalar value,
                                  const std::string& producer_owner);
  MetricValidationResult ObserveHistogram(const std::string& family,
                                          MetricLabelSet labels,
                                          MetricScalar value,
                                          const std::string& producer_owner);
  MetricValidationResult SetState(const std::string& family,
                                  MetricLabelSet labels,
                                  MetricScalar value,
                                  std::string state_text,
                                  const std::string& producer_owner);

  std::vector<MetricValue> SnapshotCurrent(bool include_cluster = true) const;
  std::vector<MetricValue> SnapshotHistory(bool include_cluster = true, u64 max_rows = 1024) const;

 private:
  MetricValidationResult UpdateValue(const MetricDescriptor& descriptor,
                                     MetricLabelSet labels,
                                     MetricScalar value,
                                     std::string state_text,
                                     const std::string& producer_owner,
                                     MetricType operation_type);
  MetricSeriesKey NormalizeKey(const std::string& family, const MetricLabelSet& labels) const;
  void LoadBuiltinDescriptors();

  mutable std::mutex mutex_;
  std::map<std::string, MetricDescriptor> descriptors_;
  std::map<std::string, std::string> aliases_;
  std::map<MetricSeriesKey, MetricValue> current_values_;
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
