// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/observability/optimizer_metric_support_bundle.cpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a = scratchbird::engine::internal_api;
namespace o = a::observability;
namespace m = scratchbird::core::metrics;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
m::MetricUuid Id(unsigned n) {
  return m::MetricUuid{{1,144,0,0,0,0,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(n)}};
}
int main() {
  m::MetricDescriptor descriptor;
  descriptor.family = "test.native";
  descriptor.type = m::MetricType::gauge;
  descriptor.value_type = m::MetricScalarType::uuid;
  descriptor.metric_uuid = Id(1);
  descriptor.descriptor_generation = 1;
  descriptor.labels = {{"public", true, false, m::MetricLabelType::system_uuid},
                       {"private", true, true, m::MetricLabelType::system_uuid}};
  m::MetricValue value;
  value.family = descriptor.family;
  value.type = descriptor.type;
  value.value = Id(2);
  value.labels = {{"public", Id(3)}, {"private", Id(4)}};
  o::OptimizerMetricSupportBundleRow row;
  Check(o::EncodeRedactedMetric(descriptor, value, false, &row));
  Check(row.omitted_sensitive_labels == std::vector<std::string>{"private"});
  auto projected = descriptor;
  projected.labels.pop_back();
  auto decoded = m::DecodeMetricValue(projected, row.encoded_redacted_value);
  Check(decoded.ok() && std::get<m::MetricUuid>(decoded.value->value) == Id(2));
  Check(decoded.value->labels.size() == 1 &&
        std::get<m::MetricUuid>(decoded.value->labels[0].value) == Id(3));
  const auto saved = row.encoded_redacted_value;
  value.labels[1].value = std::string("01900000-0000-7000-8000-000000000004");
  Check(!o::EncodeRedactedMetric(descriptor, value, false, &row) && row.encoded_redacted_value == saved);
  value.labels[1].value = Id(4);
  Check(o::EncodeRedactedMetric(descriptor, value, true, &row));
  decoded = m::DecodeMetricValue(descriptor, row.encoded_redacted_value);
  Check(decoded.ok() && decoded.value->labels.size() == 2 && row.omitted_sensitive_labels.empty());
  o::OptimizerMetricSupportBundleRequest request;
  request.scope_uuid = Id(5); request.support_bundle_id = "fixture";
  o::OptimizerMetricSupportBundleResult result;
  result.rows.push_back(row);
  const auto bundle = o::EncodeBundle(request, result);
  std::vector<std::string> fields;
  Check(a::DecodeMgaMetadataFields(bundle, &fields) && fields[1] == a::MetadataUuidBytes(Id(5)));
  std::vector<std::string> row_fields;
  Check(a::DecodeMgaMetadataFields(fields[7], &row_fields) && row_fields[7] == a::MetadataUuidBytes(Id(1)));
  Check(row_fields.back() == std::string(row.encoded_redacted_value.begin(), row.encoded_redacted_value.end()));
  auto corrupt = bundle; corrupt[22] ^= 1;
  Check(!a::DecodeMgaMetadataFields(corrupt, &fields));
}
