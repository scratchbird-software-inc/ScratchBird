// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/core/memory/memory_support_bundle.cpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace m = scratchbird::core::metrics;
namespace mem = scratchbird::core::memory;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
m::MetricUuid Id(unsigned n) {
  return m::MetricUuid{{1,144,0,0,0,0,112,0,128,0,0,0,0,0,255,static_cast<std::uint8_t>(n)}};
}
int main() {
  m::MetricDescriptor descriptor;
  descriptor.family = "fixture.memory"; descriptor.type = m::MetricType::gauge;
  descriptor.value_type = m::MetricScalarType::uuid;
  descriptor.labels = {{"owner", true, false, m::MetricLabelType::system_uuid},
                       {"private", true, true, m::MetricLabelType::system_uuid}};
  m::MetricValue value; value.family = descriptor.family; value.type = descriptor.type;
  value.value = Id(1); value.labels = {{"owner", Id(2)}, {"private", Id(3)}};
  mem::MemorySupportBundleMetricRecord record;
  Check(mem::ProjectMemoryMetric(descriptor, value, false, &record));
  Check(!record.value_redacted && record.metric.omitted_sensitive_labels == std::vector<std::string>{"private"});
  auto projected = descriptor; projected.labels.pop_back();
  auto decoded = m::DecodeMetricValue(projected, record.metric.encoded_value);
  Check(decoded.ok() && std::get<m::MetricUuid>(decoded.value->value) == Id(1));
  Check(std::get<m::MetricUuid>(decoded.value->labels.front().value) == Id(2));
  const auto saved = record.metric.encoded_value;
  value.labels[1].value = std::string("01900000-0000-7000-8000-00000000ff03");
  Check(!mem::ProjectMemoryMetric(descriptor, value, false, &record));
  Check(record.metric.encoded_value == saved);
  value.labels[1].value = Id(3);
  Check(mem::ProjectMemoryMetric(descriptor, value, true, &record));
  decoded = m::DecodeMetricValue(descriptor, record.metric.encoded_value);
  Check(decoded.ok() && decoded.value->labels.size() == 2);
  descriptor.value_type = m::MetricScalarType::text; value.value = std::string("secret-password");
  Check(mem::ProjectMemoryMetric(descriptor, value, false, &record));
  Check(record.value_redacted && record.metric.encoded_value.empty());
  Check(mem::ProjectMemoryMetric(descriptor, value, true, &record));
  Check(!record.value_redacted && !record.metric.encoded_value.empty());
  mem::MemorySupportBundleLimits limits; limits.max_rows = 1; limits.max_value_bytes = 1048576;
  mem::MemorySupportBundleResult result;
  Check(mem::BoundedAppendMetric(&result, record, limits));
  Check(!mem::BoundedAppendRow(&result, {}, limits));
  Check(result.metric_count == 1 && result.dropped_row_count == 1);
  mem::MemorySupportBundleResult bounded;
  limits.max_output_bytes = result.output_bytes - 1;
  Check(!mem::BoundedAppendMetric(&bounded, record, limits));
  Check(bounded.output_bytes == 0 && bounded.metric_records.empty());
}
