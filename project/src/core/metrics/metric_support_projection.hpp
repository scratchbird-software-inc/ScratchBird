// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "metric_value_codec.hpp"
#include <algorithm>

namespace scratchbird::core::metrics {
struct MetricSupportProjection {
  MetricDescriptorBinding binding;
  std::vector<std::string> omitted_sensitive_labels;
  std::vector<platform::byte> encoded_value;
};

// Validate the original observation before removing sensitive labels. UUID
// labels remain native values; omission is explicit metadata, never text in a
// UUID-typed field. The original binding identifies the unredacted schema.
inline bool ProjectMetricForSupport(const MetricDescriptor& descriptor,
    const MetricValue& value, bool allow_sensitive,
    MetricSupportProjection* output, MetricValue* visible = nullptr) {
  if (!output || !EncodeMetricValue(descriptor, value).ok()) return false;
  auto definition = static_cast<const MetricDescriptorDefinition&>(descriptor);
  auto projected = value;
  MetricSupportProjection staged;
  staged.binding = descriptor;
  if (!allow_sensitive) {
    for (const auto& label : descriptor.labels)
      if (label.sensitive) staged.omitted_sensitive_labels.push_back(label.key);
    std::sort(staged.omitted_sensitive_labels.begin(), staged.omitted_sensitive_labels.end());
    std::erase_if(definition.labels, [](const auto& label) { return label.sensitive; });
    std::erase_if(projected.labels, [&](const auto& label) {
      return std::binary_search(staged.omitted_sensitive_labels.begin(),
                               staged.omitted_sensitive_labels.end(), label.key);
    });
  }
  auto encoded = EncodeMetricValue(definition, projected);
  if (!encoded.ok()) return false;
  staged.encoded_value = std::move(encoded.bytes);
  *output = std::move(staged);
  if (visible) *visible = std::move(projected);
  return true;
}
}  // namespace scratchbird::core::metrics
