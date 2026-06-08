// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_export.hpp"

#include "metric_producer.hpp"

#include <algorithm>
#include <sstream>

namespace scratchbird::core::metrics {
namespace {

std::string EscapeLabelValue(const std::string& value) {
  std::string out;
  for (char c : value) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

std::string FormatLabels(const MetricLabelSet& labels, const std::string& extra_key = {}, const std::string& extra_value = {}) {
  std::ostringstream out;
  bool first = true;
  auto emit = [&](const std::string& key, const std::string& value) {
    out << (first ? "{" : ",") << key << "=\"" << EscapeLabelValue(value) << "\"";
    first = false;
  };
  for (const auto& label : labels) {
    emit(label.key, label.value);
  }
  if (!extra_key.empty()) {
    emit(extra_key, extra_value);
  }
  if (!first) {
    out << "}";
  }
  return out.str();
}

}  // namespace

std::string MetricFamilyToOpenMetricsName(std::string family) {
  std::replace(family.begin(), family.end(), '.', '_');
  return family;
}

std::string ExportOpenMetrics(const MetricRegistry& registry, bool include_cluster) {
  (void)SetGauge("sb_export_adapter_queue_depth",
                 Labels({{"component", "core.metrics.export"}, {"operation", "openmetrics_export"}}),
                 0.0,
                 "metrics_exporter");
  std::ostringstream out;
  const auto descriptors = registry.Descriptors(include_cluster);
  const auto values = registry.SnapshotCurrent(include_cluster);
  for (const auto& descriptor : descriptors) {
    const auto name = MetricFamilyToOpenMetricsName(descriptor.family);
    out << "# HELP " << name << " " << descriptor.help << "\n";
    out << "# TYPE " << name << " " << MetricTypeName(descriptor.type) << "\n";
    for (const auto& value : values) {
      if (value.family != descriptor.family) {
        continue;
      }
      const auto redacted_labels = RedactSensitiveLabels(descriptor, value.labels, false);
      if (descriptor.type == MetricType::histogram) {
        for (const auto& bucket : value.buckets) {
          out << name << "_bucket" << FormatLabels(redacted_labels, "le", std::to_string(bucket.first)) << " " << bucket.second << "\n";
        }
        out << name << "_sum" << FormatLabels(redacted_labels) << " " << value.sum << "\n";
        out << name << "_count" << FormatLabels(redacted_labels) << " " << value.count << "\n";
      } else {
        out << name << FormatLabels(redacted_labels) << " " << value.value << "\n";
      }
    }
  }
  out << "# EOF\n";
  return out.str();
}

}  // namespace scratchbird::core::metrics
