// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_label_key.hpp"
#include "metric_producer.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <type_traits>

namespace m = scratchbird::core::metrics;
namespace {
std::size_t checks = 0;
void Require(bool good, const char* detail) {
  ++checks;
  if (!good) { std::cerr << detail << '\n'; std::exit(1); }
}
m::MetricUuid Id() {
  m::MetricUuid value{};
  value.bytes = {1,159,0,0,0,0,112,0,128,0,0,0,0,0,0,1};
  return value;
}
}  // namespace

int main() {
  static_assert(sizeof(m::MetricUuid) == 16);
  static_assert(std::is_same_v<std::variant_alternative_t<1, m::MetricLabelValue>, m::MetricUuid>);
  m::MetricDescriptor descriptor;
  descriptor.family = "sb_binary_label_test";
  descriptor.labels = {{"object_uuid", true, false, m::MetricLabelType::system_uuid},
                        {"mode", true, false, m::MetricLabelType::text},
                        {"secret", false, true, m::MetricLabelType::system_uuid}};
  const m::MetricLabelSet labels{{"object_uuid", Id()}, {"mode", "insert"}};
  const auto produced = m::Labels({{"object_uuid", Id()}, {"mode", "insert"}});
  Require(m::MakeMetricSeriesKey(descriptor.family, produced) ==
          m::MakeMetricSeriesKey(descriptor.family, labels), "producer flattened binary label");
  const auto invalid_produced = m::Labels({{"object_uuid", m::MetricUuid{}}, {"mode", ""}});
  Require(invalid_produced.size() == 2 && !m::ValidateMetricLabelSet(descriptor, invalid_produced).ok,
          "producer silently discarded invalid labels");
  auto confidential = labels; confidential.push_back({"secret", Id()});
  const auto redacted = m::RedactSensitiveLabels(descriptor, confidential, false);
  Require(std::get<m::MetricUuid>(redacted[0].value) == Id(), "redaction changed public binary identity");
  Require(std::get<std::string>(redacted.back().value) == "<redacted>", "redaction leaked sensitive identity");
  Require(std::get<m::MetricUuid>(m::RedactSensitiveLabels(descriptor, confidential, true).back().value) == Id(),
          "authorized binary label lost");
  Require(std::get<m::MetricUuid>(confidential.back().value) == Id(), "redaction mutated source identity");
  Require(!m::ValidateMetricLabelSet(descriptor, redacted).ok, "presentation redaction became identity authority");
  Require(m::ValidateMetricLabelSet(descriptor, labels).ok, "valid typed labels refused");
  const auto original = m::MakeMetricSeriesKey(descriptor.family, labels);
  Require(original.first == descriptor.family && original.second.size() == 2, "series key shape");
  Require(std::get<m::MetricUuid>(original.second[1].second) == Id(), "UUID was not retained as raw16");
  auto reversed = labels; std::reverse(reversed.begin(), reversed.end());
  Require(m::MakeMetricSeriesKey(descriptor.family, reversed) == original, "label order changed series");
  for (std::size_t n = 0; n < 16; ++n) for (unsigned bit = 0; bit < 8; ++bit) {
    auto changed = labels;
    std::get<m::MetricUuid>(changed[0].value).bytes[n] ^= 1u << bit;
    Require(m::MakeMetricSeriesKey(descriptor.family, changed) != original, "UUID bit omitted from metric series key");
  }
  for (unsigned version = 0; version < 256; ++version) {
    auto changed = labels;
    std::get<m::MetricUuid>(changed[0].value).bytes[6] = version;
    Require(m::ValidateMetricLabelSet(descriptor, changed).ok == ((version & 0xf0) == 0x70), "system UUID version admission");
  }
  for (unsigned variant = 0; variant < 256; ++variant) {
    auto changed = labels;
    std::get<m::MetricUuid>(changed[0].value).bytes[8] = variant;
    Require(m::ValidateMetricLabelSet(descriptor, changed).ok == ((variant & 0xc0) == 0x80), "system UUID variant admission");
  }
  auto data_descriptor = descriptor;
  data_descriptor.labels[0].value_type = m::MetricLabelType::uuid_value;
  for (unsigned version = 0; version < 256; ++version) {
    auto data = labels;
    std::get<m::MetricUuid>(data[0].value).bytes[6] = version;
    Require(m::ValidateMetricLabelSet(data_descriptor, data).ok ==
        ((version >> 4) >= 1 && (version >> 4) <= 7), "user UUID version policy");
    Require(std::get<m::MetricUuid>(m::MakeMetricSeriesKey(descriptor.family, data).second[1].second).bytes[6] == version,
            "user UUID was rewritten to a system identity");
  }
  auto bad = labels; bad[0].value = m::MetricUuid{};
  Require(!m::ValidateMetricLabelSet(descriptor, bad).ok, "nil UUID admitted");
  bad[0].value = std::string("019f0000-0000-7000-8000-000000000001");
  Require(!m::ValidateMetricLabelSet(descriptor, bad).ok, "text UUID admitted");
  Require(m::MakeMetricSeriesKey(descriptor.family, bad) != original, "text UUID aliases binary identity");
  bad = labels; bad[1].value = Id();
  Require(!m::ValidateMetricLabelSet(descriptor, bad).ok, "UUID bypassed text label schema");
  bad = labels; bad.push_back(labels[0]);
  Require(!m::ValidateMetricLabelSet(descriptor, bad).ok, "duplicate label admitted");
  bad = labels; bad.pop_back();
  Require(!m::ValidateMetricLabelSet(descriptor, bad).ok, "required label absent");
  bad = labels; bad[1].key = "unknown";
  Require(!m::ValidateMetricLabelSet(descriptor, bad).ok, "unknown label admitted");
  bad = labels; bad[1].value = std::string{};
  Require(!m::ValidateMetricLabelSet(descriptor, bad).ok, "empty text label admitted");
  m::MetricLabelSet left{{"a", "b|c=d"}}, right{{"a", "b"}, {"c", "d"}};
  Require(m::MakeMetricSeriesKey("sb_x", left) != m::MakeMetricSeriesKey("sb_x", right), "delimiter series collision");
  left = {{"b", "c"}}; right = {{"a|b", "c"}};
  Require(m::MakeMetricSeriesKey("sb_x|a", left) != m::MakeMetricSeriesKey("sb_x", right), "family label delimiter collision");
  left = {{"a", std::string("b\0c", 3)}}; right = {{"a", "b"}};
  Require(m::MakeMetricSeriesKey("sb_x", left) != m::MakeMetricSeriesKey("sb_x", right), "embedded NUL truncated");
  std::map<m::MetricSeriesKey, unsigned> series;
  for (unsigned n = 0; n != 256; ++n) {
    auto modified = labels;
    std::get<m::MetricUuid>(modified[0].value).bytes[15] = n;
    Require(series.emplace(m::MakeMetricSeriesKey(descriptor.family, modified), n).second, "binary series aliased in map");
  }
  for (unsigned n = 0; n != 256; ++n) {
    auto modified = labels;
    std::get<m::MetricUuid>(modified[0].value).bytes[15] = n;
    Require(series.at(m::MakeMetricSeriesKey(descriptor.family, modified)) == n, "binary map lookup changed series");
  }
  std::cout << "metric_binary_label_key=passed checks=" << checks << '\n';
}
