// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/metric_projection_fixture.hpp"
#include "metric_contracts.hpp"

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace m = scratchbird::core::metrics;
namespace {
constexpr std::array families{
    "sb_mga_relation_state_load_total",
    "sb_mga_relation_state_rows_materialized_total",
    "sb_mga_relation_state_bytes_materialized_total",
    "sb_mga_relation_state_allocation_units_materialized_total"};
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
m::MetricLabelSet Labels(m::MetricUuid object) {
  m::MetricLabelSet labels{{"component", "engine.mga_relation_store"},
      {"operation", "select"}, {"result", "scoped"}, {"reason", "precision_component"}};
  if (!object.is_nil()) labels.push_back({"object_uuid", object});
  return labels;
}
void CheckValues(const m::MetricLabelSet& labels, const std::array<std::uint64_t, 4>& expected) {
  const auto values = m::DefaultMetricRegistry().SnapshotCurrent(false);
  for (std::size_t i = 0; i < families.size(); ++i) {
    const auto key = m::MakeMetricSeriesKey(families[i], labels);
    const auto value = std::find_if(values.begin(), values.end(), [&](const auto& row) {
      return m::MakeMetricSeriesKey(row.family, row.labels) == key;
    });
    Check(value != values.end(), "relation metric sample missing");
    const auto* integer = std::get_if<std::uint64_t>(&value->value);
    Check(integer && *integer == expected[i], "relation metric scalar type or precision changed");
  }
}
}  // namespace

int main() {
  try {
    using scratchbird::tests::FixtureUuid;
    const auto object = FixtureUuid(1492, 1);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    constexpr std::uint64_t large_bytes = (std::uint64_t{1} << 53) + 1;
    constexpr std::uint64_t large_allocations = (std::uint64_t{1} << 53) + 3;
    Check(!m::RecordMgaRelationStateLoad(object, "select", "scoped", "precision_component",
          maximum, large_bytes, large_allocations).ok, "unbound relation metrics accepted");
    Check(m::DefaultMetricRegistry().SnapshotCurrent().empty(), "missing binding fabricated observations");

    // Explicit component measurements; these large values are not attributed
    // to a real relation scan. The route gate independently checks real scans.
    scratchbird::tests::MetricProjectionFixture fixture(FixtureUuid(1492, 2), FixtureUuid(1492, 3), 1493);
    const auto labels = Labels(object);
    const auto aggregate_labels = Labels({});
    for (const auto family : families) {
      fixture.Admit(family, labels);
      fixture.Admit(family, aggregate_labels);
    }
    Check(m::RecordMgaRelationStateLoad(object, "select", "scoped", "precision_component",
          maximum, large_bytes, large_allocations).ok, "native relation measurements refused");
    fixture.ExpectProduced(4);
    CheckValues(labels, {1, maximum, large_bytes, large_allocations});

    // The load observation precedes the overflowing row counter. Later byte
    // and allocation counters must not change after the first refused field.
    Check(!m::RecordMgaRelationStateLoad(object, "select", "scoped", "precision_component",
          1, 99, 99).ok, "relation row-count overflow accepted");
    fixture.ExpectProduced(1);
    CheckValues(labels, {2, maximum, large_bytes, large_allocations});

    Check(m::RecordMgaRelationStateLoad({}, "select", "scoped", "precision_component", 0, 0, 0).ok,
          "explicit empty aggregate measurement refused");
    fixture.ExpectProduced(4);
    CheckValues(aggregate_labels, {1, 0, 0, 0});
    Check(std::none_of(aggregate_labels.begin(), aggregate_labels.end(),
          [](const auto& label) { return label.key == "object_uuid"; }),
          "aggregate measurement invented an object identity");
    fixture.Seal();
    fixture.VerifyAdmissionRefusals(families.front(), labels, std::uint64_t{1});
    fixture.VerifyAndDrain();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
