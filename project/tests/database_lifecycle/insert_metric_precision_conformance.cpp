// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/metric_projection_fixture.hpp"
#include "engine/internal_api/dml/insert_batch.hpp"
#include "metric_contracts.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace m = scratchbird::core::metrics;
namespace api = scratchbird::engine::internal_api;
namespace {
template <typename T>
concept CountOperand = requires(const api::InsertBatchContext& context, T value) {
  api::RecordInsertBatchMetric(context, "count", value, "ok");
};
static_assert(CountOperand<std::uint64_t> && !CountOperand<double> && !CountOperand<float>);
template <typename T> concept RowCountOperand = requires(T v, m::MetricUuid id) {
  m::RecordInsertRowsInserted(v, id, "singleton");
};
template <typename T> concept HistogramCountOperand = requires(T v, m::MetricUuid id) {
  m::ObserveInsertRowsPerBatch(v, id, "singleton");
};
template <typename T> concept PageCountOperand = requires(T v, m::MetricUuid id) {
  m::RecordInsertPreallocatedPages(v, id, "singleton", "row", "ok", "none");
};
template <typename R, typename A, typename B>
concept AdaptiveCountOperands = requires(R r, A a, B b, m::MetricUuid id) {
  m::PublishInsertAdaptiveBatchPlan(id, "singleton", r, a, b, "none");
};
template <typename T> concept SlowCountOperand = requires(T v, m::MetricUuid id) {
  m::RecordInsertSlowPath(id, "singleton", "scan_fallback", "none", v);
};
static_assert(RowCountOperand<std::uint64_t> && !RowCountOperand<double>);
static_assert(HistogramCountOperand<std::uint64_t> && !HistogramCountOperand<double>);
static_assert(PageCountOperand<std::uint64_t> && !PageCountOperand<double>);
static_assert(SlowCountOperand<std::uint64_t> && !SlowCountOperand<double>);
static_assert(AdaptiveCountOperands<std::uint64_t, std::uint64_t, std::uint64_t>);
static_assert(!AdaptiveCountOperands<double, std::uint64_t, std::uint64_t>);
static_assert(!AdaptiveCountOperands<std::uint64_t, double, std::uint64_t>);
static_assert(!AdaptiveCountOperands<std::uint64_t, std::uint64_t, double>);
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
m::MetricLabelSet Labels(m::MetricUuid object, std::string result,
                         std::string reason = {}) {
  m::MetricLabelSet labels{{"component", "engine.insert"},
      {"object_uuid", object}, {"operation", "singleton"},
      {"result", std::move(result)}};
  if (!reason.empty()) labels.push_back({"reason", std::move(reason)});
  return labels;
}
m::MetricValue Value(const std::string& family, const m::MetricLabelSet& labels) {
  const auto values = m::DefaultMetricRegistry().SnapshotCurrent(false);
  const auto key = m::MakeMetricSeriesKey(family, labels);
  const auto value = std::find_if(values.begin(), values.end(), [&](const auto& row) {
    return m::MakeMetricSeriesKey(row.family, row.labels) == key;
  });
  Check(value != values.end(), "insert metric observation missing");
  return *value;
}
void Exact(const std::string& family, const m::MetricLabelSet& labels,
           std::uint64_t expected) {
  const auto value = Value(family, labels);
  const auto* integer = std::get_if<std::uint64_t>(&value.value);
  Check(integer && *integer == expected && !value.arithmetic_inexact,
        "insert count scalar type or precision changed");
  if (value.type == m::MetricType::histogram) {
    const auto* sum = std::get_if<std::uint64_t>(&value.sum);
    Check(sum && *sum == expected && value.count == 1 && value.buckets.back() == 1,
          "insert count histogram lost its exact sample or sum");
  }
}
}  // namespace

int main() {
  try {
    using scratchbird::tests::FixtureUuid;
    constexpr std::uint64_t large = (std::uint64_t{1} << 53) + 1;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto object = FixtureUuid(1500, 1);
    Check(!m::RecordInsertRowsInserted(large, object, "singleton").ok,
          "unbound INSERT producer accepted");
    Check(m::DefaultMetricRegistry().SnapshotCurrent(false).empty(),
          "missing INSERT binding fabricated observations");

    // Explicit producer-boundary measurements, not a claim that this test
    // inserted UINT64_MAX rows or activated runtime catalog descriptors.
    scratchbird::tests::MetricProjectionFixture fixture(
        FixtureUuid(1500, 2), FixtureUuid(1500, 3), 1501);
    const auto ok = Labels(object, "ok");
    const auto reduced = Labels(object, "reduced", "precision_component");
    const auto cache = Labels(object, "miss", "epoch_key_match");
    for (const auto family : {"sb_dml_insert_batch_started_total",
                             "sb_dml_insert_rows_inserted_total",
                             "sb_dml_insert_rows_per_batch"})
      fixture.Admit(family, ok);
    fixture.Admit("sb_dml_insert_prepared_descriptor_cache_total", cache);
    for (const auto family : {"sb_dml_insert_adaptive_batch_requested_rows",
                             "sb_dml_insert_adaptive_batch_admitted_rows",
                             "sb_dml_insert_adaptive_batch_admitted_bytes",
                             "sb_dml_insert_adaptive_batch_resize_total"})
      fixture.Admit(family, reduced);

    api::InsertBatchContext context;
    context.target_object_uuid = object;
    context.actual_row_count = large;
    context.adaptive_batch_plan.requested_rows = maximum;
    context.adaptive_batch_plan.admitted_rows = large;
    context.adaptive_batch_plan.admitted_bytes = maximum;
    context.adaptive_batch_plan.reason = "precision_component";
    api::RecordInsertBatchMetric(context, "sb_dml_insert_batch_started_total", 1, "ok");
    fixture.ExpectProduced(6);
    Exact("sb_dml_insert_batch_started_total", ok, 1);
    Exact("sb_dml_insert_prepared_descriptor_cache_total", cache, 1);
    Exact("sb_dml_insert_adaptive_batch_requested_rows", reduced, maximum);
    Exact("sb_dml_insert_adaptive_batch_admitted_rows", reduced, large);
    Exact("sb_dml_insert_adaptive_batch_admitted_bytes", reduced, maximum);
    Exact("sb_dml_insert_adaptive_batch_resize_total", reduced, 1);
    api::RecordInsertBatchMetric(context, "sb_dml_insert_rows_inserted_total", maximum, "ok");
    fixture.ExpectProduced(2);
    Exact("sb_dml_insert_rows_inserted_total", ok, maximum);
    Exact("sb_dml_insert_rows_per_batch", ok, large);
    Check(!m::RecordInsertRowsInserted(1, object, "singleton").ok,
          "INSERT counter overflow accepted");
    Exact("sb_dml_insert_rows_inserted_total", ok, maximum);
    // Dispatch publishes independent observations, not an atomic batch: a
    // refused counter must not falsify the separately accepted histogram.
    api::RecordInsertBatchMetric(context, "sb_dml_insert_rows_inserted_total", 1, "ok");
    fixture.ExpectProduced(1);
    Exact("sb_dml_insert_rows_inserted_total", ok, maximum);
    const auto repeated = Value("sb_dml_insert_rows_per_batch", ok);
    Check(repeated.count == 2 && repeated.sum == m::MetricScalar{large * 2},
          "partial INSERT observation outcome lost exact histogram accounting");

    auto pages = Labels(object, "reserved", "precision_component");
    pages.push_back({"page_family", "row"});
    fixture.Admit("sb_page_insert_preallocated_pages_total", pages);
    fixture.Produced(m::RecordInsertPreallocatedPages(maximum, object, "singleton",
                                                     "row", "reserved", "precision_component"));
    Exact("sb_page_insert_preallocated_pages_total", pages, maximum);
    Check(!m::RecordInsertPreallocatedPages(1, object, "singleton", "row",
                                           "reserved", "precision_component").ok,
          "INSERT preallocation overflow accepted");

    const auto probe = Labels(object, "physical_probe", "precision_component");
    fixture.Admit("sb_index_insert_unique_physical_probe_total", probe);
    api::RecordInsertBatchMetric(context, "sb_index_insert_unique_physical_probe_total",
                                large, "physical_probe", "precision_component");
    fixture.ExpectProduced(1);
    Exact("sb_index_insert_unique_physical_probe_total", probe, large);
    const auto direct_probe = Labels(object, "physical_probe", "direct_component");
    fixture.Admit("sb_index_insert_unique_physical_probe_total", direct_probe);
    fixture.Produced(m::RecordInsertUniquePhysicalProbe(object, "singleton",
        "physical_probe", "direct_component"));
    Exact("sb_index_insert_unique_physical_probe_total", direct_probe, 1);

    const auto fallback = Labels(object, "fallback", "precision_component");
    for (const auto family : {"sb_dml_insert_batch_fallback_total",
                             "sb_dml_insert_batch_fallback_reason_total"})
      fixture.Admit(family, fallback);
    const auto slow = Labels(object, "refused", "precision_component");
    fixture.Admit("sb_dml_insert_slow_path_total", slow);
    api::RecordInsertBatchMetric(context, "sb_dml_insert_batch_fallback_total", 1,
                                "fallback", "precision_component");
    fixture.ExpectProduced(3);
    Exact("sb_dml_insert_batch_fallback_total", fallback, 1);
    Exact("sb_dml_insert_batch_fallback_reason_total", fallback, 1);
    Exact("sb_dml_insert_slow_path_total", slow, 1);
    const auto accumulated_slow = Labels(object, "scan_fallback", "precision_component");
    fixture.Admit("sb_dml_insert_slow_path_total", accumulated_slow);
    api::RecordInsertBatchMetric(context, "sb_dml_insert_slow_path_total", large,
                                "scan_fallback", "precision_component");
    fixture.ExpectProduced(1);
    Exact("sb_dml_insert_slow_path_total", accumulated_slow, large);

    const auto trace = Labels(object, "trace", "precision_component");
    fixture.Admit("sb_dml_insert_trace_event_total", trace);
    fixture.Produced(m::RecordInsertTraceEvent(object, "singleton", "precision_component"));
    Exact("sb_dml_insert_trace_event_total", trace, 1);
    const auto cancel = Labels(object, "cancelled", "precision_component");
    fixture.Admit("sb_dml_insert_cancel_total", cancel);
    fixture.Produced(m::RecordInsertCancel(object, "singleton", "precision_component"));
    Exact("sb_dml_insert_cancel_total", cancel, 1);
    const auto full = Labels(object, "full", "precision_component");
    const auto scoped = Labels(object, "scoped", "precision_component");
    fixture.Admit("sb_dml_insert_relation_state_full_load_total", full);
    fixture.Admit("sb_dml_insert_relation_state_scoped_load_total", scoped);
    Check(m::RecordInsertRelationStateLoad(object, "singleton", true, true,
                                           "precision_component").ok,
          "INSERT relation-state counts refused");
    fixture.ExpectProduced(2);
    Exact("sb_dml_insert_relation_state_full_load_total", full, 1);
    Exact("sb_dml_insert_relation_state_scoped_load_total", scoped, 1);

    const auto growth = Labels(object, "requested", "precision_component");
    fixture.Admit("sb_filespace_insert_growth_request_total", growth);
    api::RecordInsertBatchMetric(context, "sb_filespace_insert_growth_request_total", 1,
                                "requested", "precision_component");
    fixture.ExpectProduced(1);
    Exact("sb_filespace_insert_growth_request_total", growth, 1);

    const auto zero_object = FixtureUuid(1500, 4);
    const auto zero = Labels(zero_object, "ok");
    fixture.Admit("sb_dml_insert_rows_inserted_total", zero);
    fixture.Admit("sb_dml_insert_rows_per_batch", zero);
    fixture.Produced(m::RecordInsertRowsInserted(0, zero_object, "singleton"));
    fixture.Produced(m::ObserveInsertRowsPerBatch(0, zero_object, "singleton"));
    Exact("sb_dml_insert_rows_inserted_total", zero, 0);
    Exact("sb_dml_insert_rows_per_batch", zero, 0);

    auto allocation = Labels(object, "ok");
    allocation.push_back({"wait_class", "precision_component"});
    const auto wait = Labels(object, "ok", "precision_component");
    fixture.Admit("sb_dml_insert_allocation_stall_microseconds", allocation);
    fixture.Admit("sb_filespace_insert_growth_wait_microseconds", wait);
    api::RecordInsertBatchDurationMetric(context, "sb_dml_insert_allocation_stall_microseconds",
                                        1.25, "ok", "precision_component");
    api::RecordInsertBatchDurationMetric(context, "sb_filespace_insert_growth_wait_microseconds",
                                        2.5, "ok", "precision_component");
    fixture.ExpectProduced(2);
    Check(std::get<double>(Value("sb_dml_insert_allocation_stall_microseconds", allocation).value) == 1.25 &&
          std::get<double>(Value("sb_filespace_insert_growth_wait_microseconds", wait).value) == 2.5,
          "duration observations were truncated to counts");

    fixture.Seal();
    // Use a non-saturated counter so overflow cannot mask a broken authority
    // or scalar-type check in either of these rejection tests.
    fixture.VerifyAdmissionRefusals("sb_dml_insert_batch_started_total", ok, std::uint64_t{1});
    Check(!m::DefaultMetricRegistry().IncrementCounter("sb_dml_insert_batch_started_total",
        ok, 1.0, "engine_insert").ok, "REAL64 count coercion reopened");
    Check(!m::DefaultMetricRegistry().ObserveHistogram("sb_dml_insert_allocation_stall_microseconds",
        allocation, std::uint64_t{1}, "engine_insert").ok, "UINT64 duration coercion opened");
    fixture.VerifyAndDrain();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
