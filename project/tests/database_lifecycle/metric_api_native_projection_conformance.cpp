// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "observability/metrics_api.hpp"
#include "observability/lifecycle_metric_definitions.hpp"
#include "metric_history_store_codec.hpp"
#include "../support/component_authorization_fixture.hpp"
#include "../support/metric_projection_fixture.hpp"
#include <chrono>
#include <bit>
#include <limits>
#include <filesystem>
#include <iostream>
#include <source_location>

namespace a = scratchbird::engine::internal_api;
namespace m = scratchbird::core::metrics;
namespace t = scratchbird::tests;
namespace {
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) throw std::runtime_error("metric API projection check at " + std::to_string(at.line()));
}
auto Id(unsigned n) { return t::FixtureUuid(2290, n); }
a::EngineRequestContext Context(bool sensitive = true) {
  a::EngineRequestContext context;
  context.security_context_present = true;
  context.database_uuid = Id(1); context.node_uuid = Id(2); context.principal_uuid = Id(7);
  if (sensitive) t::MaterializeComponentAuthorization(context, {"OBS_METRICS_READ_ALL"});
  else t::MaterializeComponentAuthorization(context, {"OBS_METRICS_READ_DATABASE"});
  return context;
}
struct Temp {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("sb_metric_api_projection_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Temp() { Check(std::filesystem::create_directory(path)); }
  ~Temp() {
    m::DisableMetricHistoryPersistence();
    std::error_code error;
    std::filesystem::remove_all(path, error);
    if (error) std::cerr << "metric fixture cleanup failed: " << error.message() << '\n';
  }
};
const a::EngineTypedValue& Field(const a::EngineRowValue& row, const std::string& name) {
  for (const auto& field : row.fields) if (field.first == name) return field.second;
  throw std::runtime_error("missing metric projection field " + name);
}
template<class T> T Decode(const a::EngineTypedValue& value, std::string_view tag) {
  Check(value.encoded_value.empty() && value.descriptor.canonical_type_name == "binary");
  Check(value.binary_value.size() >= 8 &&
        std::equal(tag.begin(), tag.end(), value.binary_value.begin()));
  m::history_codec::Reader reader{value.binary_value, 8};
  T decoded;
  reader.One(decoded);
  Check(reader.offset == reader.bytes.size());
  return decoded;
}
void Verify(const a::EngineApiResult& result, bool sensitive, bool series = false) {
  Check(result.ok && result.result_shape.rows.size() == 1);
  const auto& row = result.result_shape.rows.front();
  const auto labels = Decode<m::MetricLabelSet>(Field(row, "labels"), "SBMLB001");
  const auto omitted = Decode<std::vector<std::string>>(Field(row, "omitted_sensitive_labels"), "SBMLO001");
  Check(labels.size() == (sensitive ? 2 : 1));
  bool public_seen = false, private_seen = false;
  for (const auto& label : labels) {
    Check(std::holds_alternative<m::MetricUuid>(label.value));
    if (label.key == "public") { Check(std::get<m::MetricUuid>(label.value) == Id(3)); public_seen = true; }
    else { Check(label.key == "private" && std::get<m::MetricUuid>(label.value) == Id(4)); private_seen = true; }
  }
  Check(public_seen && private_seen == sensitive);
  Check(omitted == (sensitive ? std::vector<std::string>{} : std::vector<std::string>{"private"}));
  if (series) {
    Check(Field(row, "series_key").isSqlNull() != sensitive);
    if (sensitive) Check(!Field(row, "series_key").binary_value.empty());
    Check(Field(row, "label_hash").encoded_value == "<redacted>" || sensitive);
  } else {
    const auto& scalar = Field(row, "value");
    const auto expected = Id(5);
    Check(scalar.encoded_value.empty() && scalar.descriptor.canonical_type_name == "uuid" &&
          scalar.binary_value == std::vector<std::uint8_t>(expected.bytes.begin(), expected.bytes.end()));
  }
}
void Lifecycle(scratchbird::tests::MetricProjectionFixture& fixture) {
  a::EngineRecordLifecycleMetricRequest request;
  request.context = Context(); request.context.session_uuid = Id(8);
  request.operation_key = "repair_database"; request.outcome = "repaired";
  request.route_family = "engine_internal"; request.diagnostic_code = "component_diagnostic";
  request.cache_invalidation_required = true;
  request.cache_family = "lifecycle_metadata"; request.cache_reason = "repair";
  const auto absent = a::EngineRecordLifecycleMetric(request);
  Check(!absent.ok && !absent.metric_recorded && !absent.cache_invalidation_recorded);
  fixture.VerifyReadOnly();
  m::MetricLabelSet operation{{"operation", request.operation_key}, {"result", "success"},
      {"route_class", request.route_family}, {"database_uuid", Id(1)}, {"session_uuid", Id(8)}};
  auto audit = operation; audit.push_back({"diagnostic_code", request.diagnostic_code});
  for (const auto& definition : a::LifecycleMetricDefinitions()) {
    if (definition.family == "sb_lifecycle_operation_total") {
      fixture.AdmitDefinition(definition, operation);
      auto partial = operation; partial.front().value = "partial";
      fixture.AdmitDefinition(definition, partial);
    } else if (definition.family == "sb_lifecycle_audit_event_total") fixture.AdmitDefinition(definition, audit);
    else if (definition.family == "sb_lifecycle_diagnostic_total") fixture.AdmitDefinition(definition,
        {{"operation", request.operation_key}, {"diagnostic_code", request.diagnostic_code}, {"result", "success"}});
    else fixture.AdmitDefinition(definition,
        {{"operation", request.operation_key}, {"cache_family", request.cache_family}, {"reason", request.cache_reason}});
  }
  const auto admitted = a::EngineRecordLifecycleMetric(request);
  Check(admitted.ok && admitted.metric_recorded && admitted.cache_invalidation_recorded);
  fixture.ExpectProduced(4); fixture.Seal();
  for (const auto& value : m::DefaultMetricRegistry().SnapshotCurrent()) {
    if (value.family.starts_with("sb_lifecycle_"))
      Check(std::holds_alternative<std::uint64_t>(value.value) && std::get<std::uint64_t>(value.value) == 1);
  }
  auto wrong = request; wrong.context.node_uuid = Id(9);
  Check(!a::EngineRecordLifecycleMetric(wrong).ok);
  wrong = request; wrong.context.session_uuid = {};
  Check(!a::EngineRecordLifecycleMetric(wrong).ok);
  fixture.VerifyReadOnly();
  auto partial = request; partial.operation_key = "partial"; partial.diagnostic_code.clear();
  const auto retained = a::EngineRecordLifecycleMetric(partial);
  Check(!retained.ok && retained.metric_recorded && !retained.cache_invalidation_recorded &&
        !retained.diagnostics.empty());
  Check(std::count_if(retained.evidence.begin(), retained.evidence.end(), [](const auto& evidence) {
    return evidence.evidence_kind == "metric_observation_admitted";
  }) == 1);
  fixture.ExpectProduced(1); fixture.Seal();
}
void NumericProjection(t::MetricProjectionFixture& fixture) {
  const std::vector<m::MetricScalar> values{
      std::uint64_t{9007199254740993ULL}, std::numeric_limits<std::uint64_t>::max(),
      std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max(),
      -0.0, 0.0, std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::max()};
  unsigned ordinal = 0;
  for (const auto& scalar : values) {
    m::MetricDescriptorDefinition definition;
    definition.family = "sb_numeric_api_" + std::to_string(++ordinal);
    definition.namespace_path = "sys.metrics.test";
    definition.producer_owner = "native_projection_component";
    definition.type = m::MetricType::gauge;
    definition.value_type = m::MetricScalarTypeOf(scalar);
    fixture.AdmitDefinition(definition, {});
    fixture.Emit(definition.family, {}, scalar);
    a::EngineSysMetricsCurrentRequest request;
    request.context = Context();
    request.option_envelopes = {"family:" + definition.family};
    const auto result = a::EngineSysMetricsCurrent(request);
    Check(result.ok && result.result_shape.rows.size() == 1);
    const auto& field = Field(result.result_shape.rows.front(), "value");
    Check(field.encoded_value.empty() && field.binary_value.size() == 8);
    std::uint64_t bits = 0;
    for (unsigned i = 0; i < 8; ++i) bits |= std::uint64_t(field.binary_value[i]) << (i * 8);
    if (const auto* v = std::get_if<std::uint64_t>(&scalar)) {
      Check(bits == *v && field.descriptor.canonical_type_name == "uint64");
    } else if (const auto* v = std::get_if<std::int64_t>(&scalar)) {
      Check(bits == std::bit_cast<std::uint64_t>(*v) && field.descriptor.canonical_type_name == "int64");
    } else {
      Check(bits == std::bit_cast<std::uint64_t>(std::get<double>(scalar)) &&
            field.descriptor.canonical_type_name == "real64");
    }
    const auto& count = Field(result.result_shape.rows.front(), "count");
    Check(count.encoded_value.empty() && count.descriptor.canonical_type_name == "uint64" &&
          count.binary_value == std::vector<std::uint8_t>(8, 0));
  }
  m::MetricDescriptorDefinition absent;
  absent.family = "sb_unobserved_api"; absent.namespace_path = "sys.metrics.test";
  absent.producer_owner = "native_projection_component";
  absent.type = m::MetricType::gauge; absent.value_type = m::MetricScalarType::uint64;
  fixture.AdmitDefinition(absent, {});
  a::EngineShowMetricsRequest show;
  show.context = Context();
  show.option_envelopes = {"family:" + absent.family};
  const auto unobserved = a::EngineShowMetrics(show);
  Check(unobserved.ok && unobserved.result_shape.rows.size() == 1);
  for (const auto* name : {"value", "count", "sum"})
    Check(Field(unobserved.result_shape.rows.front(), name).isSqlNull());
  fixture.Seal();
}
}
int main() try {
  Temp temp;
  t::MetricProjectionFixture fixture(Id(1), Id(2), 2291);
  m::MetricRetentionPolicyDefinition retention;
  retention.policy_name = "component history retention";
  retention.mode = m::MetricRetentionMode::raw_and_rollup;
  retention.raw_retention_seconds = 3600;
  fixture.ConfigureRetention(retention);
  m::MetricDescriptorDefinition definition;
  definition.family = "sb_native_api_projection";
  definition.namespace_path = "sys.metrics.test";
  definition.producer_owner = "native_projection_component";
  definition.type = m::MetricType::gauge;
  definition.value_type = m::MetricScalarType::uuid;
  definition.labels = {{"public", true, false, m::MetricLabelType::system_uuid},
                       {"private", true, true, m::MetricLabelType::system_uuid}};
  const m::MetricLabelSet labels{{"public", Id(3)}, {"private", Id(4)}};
  fixture.AdmitDefinition(definition, labels);
  fixture.Emit(definition.family, labels, Id(5));
  fixture.Seal();
  const auto descriptor = *m::DefaultMetricRegistry().FindDescriptor(definition.family);
  m::MetricRetentionPolicy policy;
  static_cast<m::MetricRetentionPolicyDefinition&>(policy) = retention;
  policy.policy_uuid = descriptor.retention_policy_uuid;
  policy.generation = descriptor.retention_policy_generation;
  const auto& series = fixture.RetainedSeries(definition.family, labels);
  const auto history_path = (temp.path / "history.bin").string();
  Check(m::ConfigureMetricHistoryPersistence(history_path, {policy}).ok);
  Check(m::RegisterMetricHistorySeries(history_path, descriptor, series, policy).ok);
  auto staged_store = m::LoadMetricHistoryStore(history_path);
  Check(staged_store.load_status.ok && staged_store.series.size() == 1);
  // Stored presentation metadata cannot override the schema's sensitive UUID.
  staged_store.series.front().redaction_class = "none";
  Check(m::WriteMetricHistoryStore(history_path, staged_store).ok);
  const auto original = m::DefaultMetricRegistry().SnapshotCurrent().front();
  Check(m::AppendMetricRawSample(history_path, descriptor, original, 1000).ok);
  m::DisableMetricHistoryPersistence();
  for (bool sensitive : {false, true}) {
    const auto context = Context(sensitive);
    a::EngineSysMetricsCurrentRequest current; current.context = context;
    Verify(a::EngineSysMetricsCurrent(current), sensitive);
    a::EngineShowMetricsRequest show; show.context = context;
    Verify(a::EngineShowMetrics(show), sensitive);
    a::EngineSysMetricsHistoryRequest history; history.context = context;
    Verify(a::EngineSysMetricsHistory(history), sensitive);
    history.option_envelopes = {"history_path:" + history_path};
    Verify(a::EngineSysMetricsHistory(history), sensitive);
    a::EngineSysMetricsPersistentHistoryRequest persisted; persisted.context = context;
    persisted.option_envelopes = history.option_envelopes;
    Verify(a::EngineSysMetricsPersistentHistory(persisted), sensitive);
    a::EngineSysMetricsSeriesRequest request; request.context = context;
    request.option_envelopes = history.option_envelopes;
    Verify(a::EngineSysMetricsSeries(request), sensitive, true);
  }
  a::EngineSysMetricsCurrentRequest denied;
  denied.context.security_context_present = true;
  denied.context.database_uuid = Id(1); denied.context.principal_uuid = Id(7);
  denied.context.trace_tags = {"right:OBS_METRICS_READ_ALL"};
  Check(!a::EngineSysMetricsCurrent(denied).ok);
  auto invalid = labels;
  invalid.back().value = "01900000-0000-7000-8000-000000000004";
  Check(!m::DefaultMetricRegistry().SetGauge(definition.family, invalid, Id(5), definition.producer_owner).ok);
  Lifecycle(fixture);
  NumericProjection(fixture);
  fixture.VerifyAndDrain();
  const auto reopened = m::LoadMetricHistoryStore(history_path);
  Check(reopened.load_status.ok && reopened.raw_samples.size() == 1 && reopened.series.size() == 1);
  Check(reopened.raw_samples.front().value.value == original.value &&
        std::equal(reopened.raw_samples.front().labels.begin(), reopened.raw_samples.front().labels.end(),
            original.labels.begin(), original.labels.end(),
            [](const auto& a, const auto& b) { return a.key == b.key && a.value == b.value; }));
  // The file codec verifies framing, not descriptor semantics. A malformed
  // protected field must be rejected before redaction can hide the defect.
  auto malformed = reopened;
  malformed.raw_samples.push_back(malformed.raw_samples.front());
  malformed.series.push_back(malformed.series.front());
  malformed.series.back().labels = invalid;
  for (unsigned corruption = 0; corruption < 3; ++corruption) {
    malformed.raw_samples.back().labels = corruption == 1 ? original.labels : invalid;
    malformed.raw_samples.back().value.labels = corruption == 2 ? original.labels : invalid;
    Check(m::WriteMetricHistoryStore(history_path, malformed).ok);
    for (bool sensitive : {false, true}) {
      const auto context = Context(sensitive);
      a::EngineSysMetricsHistoryRequest history;
      history.context = context; history.option_envelopes = {"history_path:" + history_path};
      const auto refused = a::EngineSysMetricsHistory(history);
      Check(!refused.ok && refused.result_shape.rows.empty() && !refused.diagnostics.empty());
      a::EngineSysMetricsSeriesRequest series_request;
      series_request.context = context; series_request.option_envelopes = history.option_envelopes;
      const auto series_refused = a::EngineSysMetricsSeries(series_request);
      Check(!series_refused.ok && series_refused.result_shape.rows.empty() && !series_refused.diagnostics.empty());
    }
  }
  std::cout << "native metric API projections passed (component authorization; not login/IPC qualification)\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n'; return 1;
} catch (...) {
  std::cerr << "unexpected metric projection exception\n"; return 1;
}
