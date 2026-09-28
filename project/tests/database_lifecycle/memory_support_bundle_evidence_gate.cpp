#include "../support/binary_uuid_fixture.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/diagnostic_value_fixture.hpp"

#include "memory_support_bundle.hpp"
#include "metric_producer.hpp"
#include "metric_observation_queue.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace mem = scratchbird::core::memory;
namespace metrics = scratchbird::core::metrics;
namespace platform = scratchbird::core::platform;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool EvidenceHas(const std::vector<std::string>& evidence, std::string_view token) {
  for (const auto& row : evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool RowValueContains(const mem::MemorySupportBundleResult& result,
                      std::string_view needle) {
  for (const auto& row : result.rows) {
    if (scratchbird::tests::DiagnosticValueBytes(row.value).find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool HasRowKey(const mem::MemorySupportBundleResult& result,
               std::string_view key) {
  for (const auto& row : result.rows) {
    if (row.key == key) {
      return true;
    }
  }
  return false;
}

mem::MemoryAccountingSnapshot Snapshot() {
  mem::MemoryAccountingSnapshot snapshot;
  snapshot.current_bytes = 100;
  snapshot.peak_bytes = 200;
  snapshot.allocation_count = 4;
  snapshot.deallocation_count = 2;
  snapshot.failure_count = 1;
  snapshot.leak_candidate_count = 2;
  snapshot.page_buffer_current_bytes = 16;
  snapshot.arena_current_bytes = 32;
  snapshot.contexts.push_back({"query", "query-secret-token-raw", std::nullopt, 90, 120, 3, 1, 0, 2});
  snapshot.contexts.push_back({"session", "session-visible", std::nullopt, 10, 20, 1, 1, 0, 0});
  snapshot.categories.push_back({mem::MemoryCategory::executor_query_reserved,
                                 80, 100, 2, 1, 0, 1});
  snapshot.categories.push_back({mem::MemoryCategory::diagnostics,
                                 20, 40, 2, 1, 1, 1});
  return snapshot;
}

platform::DiagnosticRecord Diagnostic() {
  return platform::MakeDiagnostic(platform::StatusCode::memory_limit_exceeded,
                                  platform::Severity::error,
                                  platform::Subsystem::memory,
                                  "memory_test_failure",
                                  "core.memory.test_failure",
                                  {{"reason", "query memory limit exceeded"},
                                   {"password", "cleartext-secret-password"},
                                   {"seed", "test-seed-should-redact"}},
                                  {},
                                  "memory_support_bundle_gate",
                                  {});
}

void SupportBundleRedactsAndReportsMemoryEvidence() {
  using scratchbird::tests::FixtureUuid;
  // Explicit component fixture bindings, not fabricated production catalog
  // ownership. Publish a real observation through a node-owned queue and check
  // every admission result instead of ignoring an unbound global producer.
  metrics::MetricDescriptor descriptor;
  descriptor.family = "sb_memory_allocated_bytes";
  descriptor.type = metrics::MetricType::gauge;
  descriptor.unit = metrics::MetricUnit::bytes;
  descriptor.namespace_path = "sys.metrics.memory";
  descriptor.producer_owner = "core_memory";
  descriptor.help = "memory support bundle native observation fixture";
  descriptor.value_type = metrics::MetricScalarType::uint64;
  descriptor.readiness = metrics::MetricReadiness::implemented;
  descriptor.metric_uuid = FixtureUuid(2212, 20);
  descriptor.descriptor_generation = 1;
  descriptor.labels = {{"component", true, false}, {"operation", true, false}};
  descriptor.label_schema_uuid = FixtureUuid(2212, 21);
  descriptor.label_schema_generation = 1;
  descriptor.retention_policy_uuid = FixtureUuid(2212, 22);
  descriptor.retention_policy_generation = 1;
  descriptor.visibility_policy_uuid = FixtureUuid(2212, 25);
  descriptor.visibility_policy_generation = 1;
  auto made_queue = metrics::MetricObservationQueue::Create(
      {FixtureUuid(2212, 23), FixtureUuid(2212, 24), {}},
      {2, 8 * metrics::kMetricSampleMaxBytes});
  Require(made_queue.ok(), "memory observation queue fixture refused");
  std::shared_ptr<metrics::MetricObservationQueue> queue(std::move(made_queue.queue));
  metrics::MetricRegistry registry(queue);
  metrics::MetricRetentionPolicy policy;
  policy.policy_uuid = descriptor.retention_policy_uuid;
  policy.generation = 1;
  policy.policy_name = "memory support fixture retention";
  metrics::MetricHistoryBinding binding;
  static_cast<metrics::MetricDescriptorBinding&>(binding) = descriptor;
  binding.database_uuid = queue->binding().database_uuid;
  binding.node_uuid = queue->binding().node_uuid;
  const auto labels = metrics::Labels({{"component", "core.memory"}, {"operation", "snapshot"}});
  const auto series = metrics::MakeMetricSeriesIdentity(
      descriptor, labels, policy, binding, FixtureUuid(2212, 26), 1);
  Require(series.ok() && registry.RegisterDescriptor(descriptor).ok &&
              registry.RegisterSeries(*series.record, policy).ok,
          "memory metric native descriptor/series binding refused");
  Require(metrics::DefaultMetricRegistry().RegisterDescriptor(descriptor).ok,
          "support projection descriptor registration refused");
  Require(registry.SetGauge(descriptor.family, labels, metrics::u64{100}, "core_memory").ok,
          "memory metric observation was not published");
  auto observation = queue->TryAcquire();
  Require(observation.ok(), "memory metric actual queue observation missing");
  const auto sample = metrics::DecodeMetricRawSample(
      descriptor, *series.record, observation.lease.observation->bytes);
  Require(sample.ok() && sample.record->metric_uuid == descriptor.metric_uuid &&
              sample.record->series_uuid == series.record->series_uuid &&
              std::get<metrics::u64>(sample.record->value.value) == 100,
          "memory metric queue changed native ownership or exact value");

  mem::MemorySupportBundleRequest request;
  request.snapshot = Snapshot();
  request.diagnostics.push_back(Diagnostic());
  request.metrics = registry.SnapshotCurrent();
  request.max_top_contexts = 2;
  request.max_top_categories = 2;
  request.allow_protected_material = false;

  const auto result = mem::BuildMemorySupportBundleEvidence(std::move(request));
  Require(result.ok(), "MMCH-080 memory support bundle failed");
  Require(EvidenceHas(result.evidence, "MMCH_MEMORY_SUPPORT_BUNDLE_EVIDENCE"),
          "MMCH-080 evidence marker missing");
  Require(EvidenceHas(
              result.evidence,
              "memory_support_bundle.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_wal_or_benchmark_authority"),
          "MMCH-080 authority boundary evidence missing");
  Require(result.top_context_count == 2 && result.top_category_count == 2,
          "MMCH-080 top context/category counts missing");
  Require(result.failure_reason_count == 1 && result.metric_count > 0,
          "MMCH-080 failure or metric rows missing");
  Require(result.redacted_row_count >= 3,
          "MMCH-080 protected material was not redacted");
  Require(HasRowKey(result, "snapshot.leak_candidate_count"),
          "MMCH-080 leak candidate row missing");
  Require(HasRowKey(result, "snapshot.peak_bytes"),
          "MMCH-080 high-water row missing");
  Require(!RowValueContains(result, "cleartext-secret-password") &&
              !RowValueContains(result, "test-seed-should-redact") &&
              !RowValueContains(result, "query-secret-token-raw"),
          "MMCH-080 protected material leaked into support bundle rows");
  for (const auto& row : result.rows) {
    Require(!row.tamper_evidence_digest.empty(),
            "MMCH-080 row missing tamper-evidence digest");
  }
  Require(result.metric_records.size() == 1,
          "observed memory metric disappeared during support projection");
  const auto projected = metrics::DecodeMetricValue(
      descriptor, result.metric_records[0].metric.encoded_value);
  Require(projected.ok() && std::get<metrics::u64>(projected.value->value) == 100,
          "support bundle changed the actual observed metric");
  Require(queue->TryRemove(observation.lease) == metrics::MetricQueueError::none,
          "verified observation fixture could not be released");
}

void NativeDiagnosticValuesAreBoundedAndRedacted() {
  const std::string key = "failure_reason.0.argument.visible_uuid";
  const auto find = [](const mem::MemorySupportBundleResult& result,
                       const std::string& name) -> const mem::MemorySupportBundleRow* {
    for (const auto& row : result.rows) if (row.key == name) return &row;
    return nullptr;
  };
  for (unsigned position = 0; position < 16; ++position) {
    for (unsigned octet = 0; octet < 256; ++octet) {
      auto id = scratchbird::tests::FixtureUuid(2212, 40);
      id.bytes[position] = static_cast<unsigned char>(octet);
      auto diagnostic = Diagnostic();
      diagnostic.arguments = {{"visible_uuid", id}, {"secret_uuid", id}};
      mem::MemorySupportBundleRequest request;
      request.include_metrics = false;
      request.diagnostics = {diagnostic};
      const auto result = mem::BuildMemorySupportBundleEvidence(request);
      const auto* visible = find(result, key);
      const auto* hidden = find(result, "failure_reason.0.argument.secret_uuid");
      Require(result.ok() && visible && std::get_if<platform::Uuid>(&visible->value) &&
                  std::get<platform::Uuid>(visible->value) == id && !visible->redacted,
              "support bundle formatted/truncated/rejected diagnostic UUID data");
      Require(hidden && hidden->redacted && scratchbird::tests::DiagnosticTextEquals(
                  hidden->value, "<protected-material-excluded>"),
              "protected UUID value was buffered without redaction");
      const auto digest = visible->tamper_evidence_digest;
      request.diagnostics[0].arguments[0].value =
          std::string(reinterpret_cast<const char*>(id.bytes.data()), id.bytes.size());
      const auto text = mem::BuildMemorySupportBundleEvidence(request);
      const auto* text_row = find(text, key);
      Require(text_row && std::get_if<std::string>(&text_row->value) &&
                  text_row->tamper_evidence_digest != digest,
              "support bundle confused text bytes with UUID type");
      request.diagnostics[0].arguments[0].value = id;
      request.limits.max_value_bytes = 15;
      const auto bounded = mem::BuildMemorySupportBundleEvidence(request);
      Require(!find(bounded, key) && bounded.dropped_row_count > 0 &&
                  bounded.output_bytes <= bounded.output_byte_limit,
              "bounded support output retained a partial UUID");
      for (const auto& row : bounded.rows)
        Require(scratchbird::tests::DiagnosticValueBytes(row.value).size() <= 15,
                "bounded support value or redaction marker exceeded its limit");
    }
  }
  auto diagnostic = Diagnostic();
  diagnostic.arguments = {{"visible_uuid", platform::Uuid{}},
                          {"long_name_with_secret_at_the_end", platform::Uuid{}}};
  mem::MemorySupportBundleRequest request;
  request.include_metrics = false;
  request.diagnostics = {diagnostic};
  const auto nil = mem::BuildMemorySupportBundleEvidence(request);
  const auto* nil_row = find(nil, key);
  Require(nil_row && std::get_if<platform::Uuid>(&nil_row->value) &&
              std::get<platform::Uuid>(nil_row->value).is_nil(),
          "nil diagnostic data lost its explicit UUID type");
  request.limits.max_key_bytes = 4;
  const auto truncated_keys = mem::BuildMemorySupportBundleEvidence(request);
  Require(truncated_keys.redacted_row_count == 1,
          "truncating a key removed its protected classification");
  mem::MemorySupportBundleRow classified;
  classified.key = "neutral_identity";
  classified.value = scratchbird::tests::FixtureUuid(2212, 41);
  classified.redaction_class = "protected_material";
  request.limits.max_key_bytes = 128;
  request.diagnostics.clear();
  request.memory_fragmentation_rows = {classified};
  const auto protected_class = mem::BuildMemorySupportBundleEvidence(request);
  Require(protected_class.redacted_row_count == 1,
          "explicit protected UUID classification was ignored");
}

}  // namespace

int main() {
  NativeDiagnosticValuesAreBoundedAndRedacted();
  SupportBundleRedactsAndReportsMemoryEvidence();
  std::cout << "MMCH-080 authority_note=memory_support_bundle_evidence_only;"
            << " support_bundle_rows_are_not_finality_visibility_authorization_recovery_or_benchmark_authority\n";
  return EXIT_SUCCESS;
}
