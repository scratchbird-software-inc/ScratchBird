// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/core/index/index_metrics.cpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace i = scratchbird::core::index;
namespace m = scratchbird::core::metrics;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
m::MetricUuid Id(unsigned n) {
  return m::MetricUuid{{1,144,0,0,0,0,112,0,128,0,0,0,0,0,255,static_cast<std::uint8_t>(n)}};
}
int main() {
  i::IndexMetricIdentity id; id.index_uuid = Id(1);
  auto labels = i::Labels(id);
  Check(i::UuidLabelValue(labels, "index_uuid") == Id(1));
  Check(i::UuidLabelValue(labels, "filespace_uuid").is_nil());
  Check(std::none_of(labels.begin(), labels.end(), [](const auto& label) {return label.key == "filespace_uuid";}));
  id.filespace_uuid = Id(2); labels = i::Labels(id);
  Check(i::UuidLabelValue(labels, "filespace_uuid") == Id(2));
  Check(i::LabelValue(labels, "index_uuid").empty());
  Check(i::UuidLabelValue({{"index_uuid", "01900000-0000-7000-8000-00000000ff01"}}, "index_uuid").is_nil());
  bool ok = false;
  Check(i::ParseU64("18446744073709551615", &ok) == UINT64_MAX && ok);
  Check(i::ParseU64("18446744073709551616", &ok) == 0 && !ok);
  Check(i::ParseU64("-1", &ok) == 0 && !ok);
  auto descriptor = i::OperationDescriptor("fixture", "fixture");
  m::MetricValue metric; metric.family = descriptor.family;
  metric.type = m::MetricType::counter; metric.value = UINT64_MAX;
  i::IndexOperationMetricSample sample; sample.identity = id;
  sample.identity.index_family = "hash";
  sample.identity.index_generation = "1";
  sample.identity.evidence_digest = "fixture-evidence";
  metric.labels = i::OperationLabels(sample, i::IndexOperationCounterKind::probe);
  m::MetricSupportProjection projected;
  Check(m::ProjectMetricForSupport(descriptor, metric, false, &projected));
  Check(projected.omitted_sensitive_labels == std::vector<std::string>{"evidence_digest"});
  std::erase_if(descriptor.labels, [](const auto& l) { return l.sensitive; });
  const auto decoded = m::DecodeMetricValue(descriptor, projected.encoded_value);
  Check(decoded.ok() && std::get<std::uint64_t>(decoded.value->value) == UINT64_MAX);
  Check(i::UuidLabelValue(decoded.value->labels, "index_uuid") == Id(1));
  auto reordered = metric; std::reverse(reordered.labels.begin(), reordered.labels.end());
  Check(i::LabelSortKey(metric) == i::LabelSortKey(reordered));
  i::IndexOperationMetricSupportBundleRow row; row.key = "fixture";
  row.value = metric.value; row.metric = projected; row.labels = decoded.value->labels;
  auto limits = i::IndexOperationMetricSupportBundleLimits{};
  limits.max_output_bytes = i::RowSizeEstimate(row);
  i::IndexOperationMetricSupportBundleResult result;
  Check(i::BoundedAppend(&result, row, limits));
  Check(!i::BoundedAppend(&result, row, limits));
  Check(result.output_bytes == limits.max_output_bytes && result.dropped_row_count == 1);
  const auto refused = i::RefuseOperationMetric(sample, "test", "test");
  Check(refused.index_uuid == Id(1));
}
