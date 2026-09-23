// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/binary_uuid_fixture.hpp"
#include "core/metrics/metric_history_store_codec.hpp"
#include <iostream>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <chrono>

namespace m = scratchbird::core::metrics;
namespace {
unsigned checks = 0;
void Check(bool value) {
  ++checks;
  if (!value) { std::cerr << "metric history codec check failed: " << checks << '\n'; std::exit(1); }
}
auto Id(unsigned n) { return scratchbird::tests::FixtureUuid(1421, n); }
void StorageChecks() {
  const auto root = std::filesystem::temp_directory_path() /
      ("sb_metric_history_native_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Check(std::filesystem::create_directory(root));
  const auto path = (root / "history.bin").string();
  m::MetricRetentionPolicy policy;
  policy.policy_uuid = Id(501); policy.generation = 1; policy.policy_name = "history_native";
  policy.mode = m::MetricRetentionMode::raw_and_rollup;
  policy.raw_retention_seconds = 1; policy.rollup_retention_seconds = 3600;
  policy.rollup_grains = {m::MetricRollupGrain::one_minute};
  Check(!m::ConfigureMetricHistoryPersistence(path).ok);
  Check(m::ConfigureMetricHistoryPersistence(path, {policy}).ok);
  m::MetricDescriptor descriptor;
  descriptor.metric_uuid = Id(502); descriptor.descriptor_generation = 1;
  descriptor.label_schema_uuid = Id(503); descriptor.label_schema_generation = 1;
  descriptor.retention_policy_uuid = policy.policy_uuid; descriptor.retention_policy_generation = 1;
  descriptor.visibility_policy_uuid = Id(504); descriptor.visibility_policy_generation = 1;
  descriptor.family = "history_native"; descriptor.namespace_path = "sys.metrics.test";
  descriptor.producer_owner = "history_fixture"; descriptor.type = m::MetricType::gauge;
  descriptor.value_type = m::MetricScalarType::float64; descriptor.readiness = m::MetricReadiness::implemented;
  descriptor.labels = {{"object", true, false, m::MetricLabelType::system_uuid}};
  m::MetricHistoryBinding binding;
  static_cast<m::MetricDescriptorBinding&>(binding) = descriptor;
  binding.database_uuid = Id(505); binding.node_uuid = Id(506);
  const m::MetricLabelSet labels = {{"object", Id(507)}};
  const auto series = m::MakeMetricSeriesIdentity(descriptor, labels, policy, binding, Id(508), 1);
  Check(series.ok());
  m::MetricValue value;
  value.family = descriptor.family; value.labels = labels; value.type = descriptor.type; value.value = 1.0;
  Check(!m::AppendMetricRawSample(path, descriptor, value, 1000000).ok);
  Check(m::RegisterMetricHistorySeries(path, descriptor, *series.record, policy).ok);
  Check(m::RegisterMetricHistorySeries(path, descriptor, *series.record, policy).ok);
  Check(m::AppendMetricRawSample(path, descriptor, value, 1000000).ok);
  value.value = 3.0;
  Check(m::AppendMetricRawSample(path, descriptor, value, 2000000).ok);
  auto loaded = m::LoadMetricHistoryStore(path);
  Check(loaded.load_status.ok && loaded.series.size() == 1 && loaded.raw_samples.size() == 2);
  Check(loaded.raw_samples[0].sample_uuid != loaded.raw_samples[1].sample_uuid);
  Check(loaded.raw_samples[0].sample_time_utc_ns == 1000000000 && loaded.raw_samples[1].source_sequence == 2);
  Check(!m::GenerateMetricRollups(path, m::MetricRollupGrain::one_minute, {}, {}).ok);
  Check(m::GenerateMetricRollups(path, m::MetricRollupGrain::one_minute, Id(509), Id(510)).ok);
  loaded = m::LoadMetricHistoryStore(path);
  Check(loaded.load_status.ok && loaded.rollups.size() == 1 && loaded.evidence.size() == 1);
  Check(loaded.rollups[0].avg_value == 2 && loaded.rollups[0].sum_value == 4 && loaded.rollups[0].last_value == 3);
  Check(loaded.evidence[0].actor_uuid == Id(509) && loaded.evidence[0].transaction_uuid == Id(510));
  const auto rollup_id = loaded.rollups[0].rollup_uuid;
  Check(m::GenerateMetricRollups(path, m::MetricRollupGrain::one_minute, Id(509), Id(510)).ok);
  Check(m::LoadMetricHistoryStore(path).rollups[0].rollup_uuid == rollup_id);
  Check(m::ApplyMetricRetentionCleanup(path, 4000000, Id(509), Id(510)).ok);
  loaded = m::LoadMetricHistoryStore(path);
  Check(loaded.raw_samples.empty() && loaded.evidence.size() == 2 && loaded.evidence.back().rows_affected == 2);
  auto bad_policy = policy; bad_policy.policy_uuid = {};
  Check(!m::UpsertMetricRetentionPolicy(path, bad_policy, Id(509), Id(510)).ok);
  // Explicit corruption is never mistaken for a missing file and reseeded.
  { std::ofstream out(path, std::ios::binary | std::ios::trunc); out << "SBMTRH1 legacy or corrupt"; }
  loaded = m::LoadMetricHistoryStore(path); Check(!loaded.load_status.ok);
  Check(!m::WriteMetricHistoryStore(path, loaded).ok);
  Check(!m::ConfigureMetricHistoryPersistence(path, {policy}).ok);
  Check(!m::AppendMetricRawSample(path, descriptor, value, 3000000).ok);
  Check(!m::GenerateMetricRollups(path, m::MetricRollupGrain::one_minute, Id(509), Id(510)).ok);
  { std::ifstream in(path, std::ios::binary); const std::string bytes((std::istreambuf_iterator<char>(in)), {});
    Check(bytes == "SBMTRH1 legacy or corrupt"); }
  m::DisableMetricHistoryPersistence();
  std::filesystem::remove_all(root);
}

}
int main() {
  StorageChecks();
  m::MetricHistoryStore store;
  m::MetricRetentionPolicy policy;
  policy.policy_uuid = Id(1); policy.generation = 42; policy.policy_name = "retention";
  policy.mode = m::MetricRetentionMode::raw_and_rollup;
  policy.rollup_grains = {m::MetricRollupGrain::one_minute, m::MetricRollupGrain::one_day};
  store.policies.push_back(policy);
  m::MetricSeriesIdentity series;
  series.series_uuid = Id(2); series.series_definition_generation = 7;
  series.database_uuid = Id(3); series.node_uuid = Id(4); series.metric_uuid = Id(5);
  series.descriptor_generation = 2; series.label_schema_uuid = Id(6); series.label_schema_generation = 3;
  series.retention_policy_uuid = policy.policy_uuid; series.retention_policy_generation = policy.generation;
  series.visibility_policy_uuid = Id(7); series.visibility_policy_generation = 8;
  series.metric_family = "test_metric"; series.namespace_path = "sys.metrics";
  series.labels = {{"subject", Id(8)}, {"text", std::string("a\0b|;", 6)}};
  store.series.push_back(series);
  m::MetricFloat128 f128; f128.bytes[0] = 0xa5;
  m::MetricDecimal128 d128; d128.bytes[15] = 0x5a;
  const std::vector<m::MetricScalar> variants = {std::monostate{}, UINT64_MAX, INT64_MIN,
      -0.0, f128, d128, true, std::string("binary\0text", 11), Id(9), m::MetricEnumValue{99}};
  for (std::size_t i = 0; i < variants.size(); ++i) {
    m::MetricRawSampleRecord sample;
    static_cast<m::MetricHistoryBinding&>(sample) = series;
    sample.sample_uuid = Id(100 + i); sample.series_uuid = series.series_uuid;
    sample.series_definition_generation = series.series_definition_generation;
    sample.metric_family = series.metric_family; sample.labels = series.labels;
    sample.sample_time_utc_ns = 123456789; sample.collection_time_utc_ns = 123456790;
    sample.publication_time_utc_ns = 123456800; sample.source_sequence = i + 1; sample.revision = 2;
    sample.clock_quality = "test"; sample.freshness_class = "fresh";
    sample.value.family = series.metric_family; sample.value.labels = series.labels;
    sample.value.value = variants[i]; sample.value.sum = variants[i];
    sample.value.bucket_bounds = variants; sample.value.buckets = {0, UINT64_MAX};
    sample.value.count = UINT64_MAX; sample.value.arithmetic_inexact = true;
    sample.evidence_uuid = Id(200 + i);
    store.raw_samples.push_back(sample);
  }
  m::MetricRollupRecord rollup;
  rollup.rollup_uuid = Id(300); rollup.series_uuid = series.series_uuid;
  rollup.min_value = -1; rollup.max_value = 100; rollup.avg_value = 33.5;
  rollup.evidence_uuid = Id(301); store.rollups.push_back(rollup);
  m::MetricRetentionEvidenceRecord evidence;
  evidence.evidence_uuid = Id(400); evidence.policy_uuid = policy.policy_uuid;
  evidence.series_uuid = series.series_uuid; evidence.actor_uuid = Id(401);
  evidence.transaction_uuid = Id(402); evidence.operation = "cleanup";
  evidence.decision = "allowed"; store.evidence.push_back(evidence);
  const auto encoded = m::EncodeMetricHistoryStoreBinary(store); Check(encoded.has_value());
  Check(std::search(encoded->begin(), encoded->end(), series.series_uuid.bytes.begin(),
                    series.series_uuid.bytes.end()) != encoded->end());
  m::MetricHistoryStore decoded; Check(m::DecodeMetricHistoryStoreBinary(*encoded, &decoded));
  const auto reencoded = m::EncodeMetricHistoryStoreBinary(decoded); Check(reencoded == encoded);
  Check(decoded.series[0].series_uuid == series.series_uuid);
  Check(decoded.raw_samples.size() == variants.size());
  Check(std::get<m::MetricUuid>(decoded.series[0].labels[0].value) == Id(8));
  Check(std::get<std::string>(decoded.series[0].labels[1].value) == std::string("a\0b|;", 6));
  Check(std::get<0>(decoded.series[0].series_key) == series.database_uuid);
  Check(std::get<5>(decoded.series[0].series_key).size() == 2);
  for (std::size_t i = 0; i < variants.size(); ++i) Check(decoded.raw_samples[i].value.value == variants[i]);
  // Neither truncation nor corruption may publish partial decoded records.
  m::MetricHistoryStore sentinel; sentinel.policies.push_back(policy);
  for (std::size_t size = 0; size < encoded->size(); ++size) {
    Check(!m::DecodeMetricHistoryStoreBinary(std::span(*encoded).first(size), &sentinel));
    Check(sentinel.policies.size() == 1 && sentinel.policies[0].policy_uuid == policy.policy_uuid);
  }
  for (std::size_t i = 0; i < encoded->size(); ++i) {
    auto bad = *encoded; bad[i] ^= 0x80;
    Check(!m::DecodeMetricHistoryStoreBinary(bad, &sentinel));
  }
  auto extra = *encoded; extra.push_back(0); Check(!m::DecodeMetricHistoryStoreBinary(extra, &sentinel));
  m::MetricHistoryStore empty; auto empty_bytes = m::EncodeMetricHistoryStoreBinary(empty);
  Check(empty_bytes && m::DecodeMetricHistoryStoreBinary(*empty_bytes, &empty));
  auto too_many = *empty_bytes;
  too_many[8] = 1; too_many[9] = 0; too_many[10] = 1; too_many[11] = 0;
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(too_many.data(), too_many.size() - 32);
  Check(digest.ok()); std::copy(digest.digest.begin(), digest.digest.end(), too_many.end() - 32);
  Check(!m::DecodeMetricHistoryStoreBinary(too_many, &sentinel));
  for (const auto tag : {std::uint8_t(10), std::uint8_t(255)}) {
    m::history_codec::Reader reader{std::span(&tag, 1)}; m::MetricScalar value;
    bool refused = false; try { reader.One(value); } catch (const m::history_codec::Invalid&) { refused = true; }
    Check(refused);
  }
  std::cout << "metric_history_codec_checks=" << checks << " failures=0\n";
}
