// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_history.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

namespace scratchbird::core::metrics {
namespace {

constexpr const char* kMagic = "SBMTRH1";

struct MetricHistoryConfig {
  bool enabled = false;
  std::string path;
  std::vector<MetricRetentionPolicy> policies;
  u64 source_sequence = 0;
};

std::mutex& ConfigMutex() {
  static std::mutex mutex;
  return mutex;
}

MetricHistoryConfig& Config() {
  static MetricHistoryConfig config;
  return config;
}

u64 Fnva64(const std::string& value) {
  u64 hash = 1469598103934665603ull;
  for (unsigned char c : value) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string HexEncode(const std::string& value) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (unsigned char c : value) {
    out << std::setw(2) << static_cast<unsigned int>(c);
  }
  return out.str();
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return 0;
}

std::string HexDecode(const std::string& value) {
  std::string out;
  for (std::size_t i = 0; i + 1 < value.size(); i += 2) {
    out.push_back(static_cast<char>((HexValue(value[i]) << 4) | HexValue(value[i + 1])));
  }
  return out;
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> out;
  std::string current;
  std::istringstream input(value);
  while (std::getline(input, current, delimiter)) {
    out.push_back(current);
  }
  return out;
}

u64 ParseU64(const std::string& value) {
  if (value.empty()) {
    return 0;
  }
  try {
    return static_cast<u64>(std::stoull(value));
  } catch (...) {
    return 0;
  }
}

double ParseDouble(const std::string& value) {
  if (value.empty()) {
    return 0.0;
  }
  try {
    return std::stod(value);
  } catch (...) {
    return 0.0;
  }
}

std::string JoinRollupGrains(const std::vector<MetricRollupGrain>& grains) {
  std::ostringstream out;
  bool first = true;
  for (const auto grain : grains) {
    if (!first) {
      out << ',';
    }
    out << MetricRollupGrainName(grain);
    first = false;
  }
  return out.str();
}

std::vector<MetricRollupGrain> ParseRollupGrains(const std::string& value) {
  std::vector<MetricRollupGrain> grains;
  if (value.empty()) {
    return grains;
  }
  for (const auto& grain : Split(value, ',')) {
    if (!grain.empty()) {
      grains.push_back(MetricRollupGrainFromName(grain));
    }
  }
  return grains;
}

MetricLabelSet ParseLabels(const std::string& encoded) {
  MetricLabelSet labels;
  const auto decoded = HexDecode(encoded);
  for (const auto& item : Split(decoded, ';')) {
    if (item.empty()) {
      continue;
    }
    const auto pos = item.find('=');
    if (pos == std::string::npos) {
      continue;
    }
    labels.push_back({item.substr(0, pos), item.substr(pos + 1)});
  }
  return labels;
}

std::string EncodeLabels(const MetricLabelSet& labels) {
  return HexEncode(CanonicalMetricLabels(labels));
}

std::string EncodeBuckets(const std::map<double, u64>& buckets) {
  std::ostringstream out;
  bool first = true;
  for (const auto& [bucket, count] : buckets) {
    if (!first) {
      out << ',';
    }
    out << bucket << ':' << count;
    first = false;
  }
  return out.str();
}

std::map<double, u64> DecodeBuckets(const std::string& encoded) {
  std::map<double, u64> buckets;
  for (const auto& item : Split(encoded, ',')) {
    const auto pos = item.find(':');
    if (pos == std::string::npos) {
      continue;
    }
    buckets[ParseDouble(item.substr(0, pos))] = ParseU64(item.substr(pos + 1));
  }
  return buckets;
}

const MetricRetentionPolicy& EffectivePolicyForDescriptor(const MetricDescriptor& descriptor,
                                                          const std::vector<MetricRetentionPolicy>& configured) {
  const auto& fallback = DefaultMetricRetentionPolicyForDescriptor(descriptor);
  for (const auto& policy : configured) {
    if (policy.policy_name == fallback.policy_name || policy.policy_uuid == fallback.policy_uuid) {
      return policy;
    }
  }
  return fallback;
}

MetricHistoryStore LoadOrSeedStore(const std::string& path, const std::vector<MetricRetentionPolicy>& policies) {
  MetricHistoryStore store = LoadMetricHistoryStore(path);
  if (store.policies.empty()) {
    store.policies = policies.empty() ? BaselineMetricRetentionPolicies() : policies;
  }
  return store;
}

void WritePolicy(std::ostream& out, const MetricRetentionPolicy& policy) {
  out << kMagic << "\tPOLICY\t"
      << HexEncode(policy.policy_uuid) << '\t'
      << HexEncode(policy.policy_name) << '\t'
      << HexEncode(policy.scope) << '\t'
      << MetricRetentionModeName(policy.mode) << '\t'
      << policy.raw_retention_seconds << '\t'
      << policy.rollup_retention_seconds << '\t'
      << HexEncode(JoinRollupGrains(policy.rollup_grains)) << '\t'
      << policy.purge_batch_limit << '\t'
      << policy.max_cardinality << '\t'
      << HexEncode(policy.overflow_behavior) << '\t'
      << HexEncode(policy.edit_right) << '\t'
      << HexEncode(policy.default_admin_group) << '\t'
      << (policy.evidence_required ? "1" : "0") << '\n';
}

void WriteSeries(std::ostream& out, const MetricSeriesIdentity& series) {
  out << kMagic << "\tSERIES\t"
      << HexEncode(series.series_uuid) << '\t'
      << HexEncode(series.series_key) << '\t'
      << HexEncode(series.metric_family) << '\t'
      << HexEncode(series.namespace_path) << '\t'
      << HexEncode(series.producer_owner) << '\t'
      << HexEncode(series.scope_class) << '\t'
      << HexEncode(series.database_uuid) << '\t'
      << HexEncode(series.node_uuid) << '\t'
      << HexEncode(series.cluster_uuid) << '\t'
      << EncodeLabels(series.labels) << '\t'
      << HexEncode(series.label_hash) << '\t'
      << HexEncode(series.redaction_class) << '\t'
      << HexEncode(series.retention_policy_uuid) << '\n';
}

void WriteSample(std::ostream& out, const MetricRawSampleRecord& sample) {
  out << kMagic << "\tSAMPLE\t"
      << HexEncode(sample.sample_uuid) << '\t'
      << HexEncode(sample.series_uuid) << '\t'
      << HexEncode(sample.metric_family) << '\t'
      << EncodeLabels(sample.labels) << '\t'
      << sample.observation_time_microseconds << '\t'
      << sample.collection_time_microseconds << '\t'
      << sample.publish_time_microseconds << '\t'
      << sample.source_sequence << '\t'
      << HexEncode(sample.clock_quality) << '\t'
      << HexEncode(sample.freshness_class) << '\t'
      << MetricTypeName(sample.value.type) << '\t'
      << std::setprecision(17) << sample.value.value << '\t'
      << sample.value.count << '\t'
      << std::setprecision(17) << sample.value.sum << '\t'
      << HexEncode(EncodeBuckets(sample.value.buckets)) << '\t'
      << HexEncode(sample.value.state_text) << '\t'
      << HexEncode(sample.evidence_uuid) << '\n';
}

void WriteRollup(std::ostream& out, const MetricRollupRecord& rollup) {
  out << kMagic << "\tROLLUP\t"
      << HexEncode(rollup.rollup_uuid) << '\t'
      << HexEncode(rollup.series_uuid) << '\t'
      << HexEncode(rollup.metric_family) << '\t'
      << MetricRollupGrainName(rollup.grain) << '\t'
      << rollup.window_start_microseconds << '\t'
      << rollup.window_end_microseconds << '\t'
      << rollup.sample_count << '\t'
      << std::setprecision(17) << rollup.min_value << '\t'
      << std::setprecision(17) << rollup.max_value << '\t'
      << std::setprecision(17) << rollup.avg_value << '\t'
      << std::setprecision(17) << rollup.last_value << '\t'
      << std::setprecision(17) << rollup.sum_value << '\t'
      << rollup.histogram_count << '\t'
      << std::setprecision(17) << rollup.histogram_sum << '\t'
      << HexEncode(rollup.histogram_buckets) << '\t'
      << rollup.state_transition_count << '\t'
      << HexEncode(rollup.last_state_text) << '\t'
      << HexEncode(rollup.evidence_uuid) << '\n';
}

void WriteEvidence(std::ostream& out, const MetricRetentionEvidenceRecord& evidence) {
  out << kMagic << "\tEVIDENCE\t"
      << HexEncode(evidence.evidence_uuid) << '\t'
      << HexEncode(evidence.operation) << '\t'
      << HexEncode(evidence.policy_uuid) << '\t'
      << HexEncode(evidence.series_uuid) << '\t'
      << HexEncode(evidence.metric_family) << '\t'
      << evidence.cutoff_time_microseconds << '\t'
      << evidence.rows_affected << '\t'
      << HexEncode(evidence.actor_uuid) << '\t'
      << HexEncode(evidence.transaction_uuid) << '\t'
      << HexEncode(evidence.decision) << '\t'
      << HexEncode(evidence.detail) << '\n';
}

}  // namespace

u64 MetricHistoryNowMicroseconds() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

std::string CanonicalMetricLabels(const MetricLabelSet& labels) {
  std::vector<std::pair<std::string, std::string>> sorted;
  for (const auto& label : labels) {
    sorted.push_back({label.key, label.value});
  }
  std::sort(sorted.begin(), sorted.end());
  std::ostringstream out;
  for (const auto& label : sorted) {
    out << label.first << '=' << label.second << ';';
  }
  return out.str();
}

std::string StableV7LikeMetricUuid(const std::string& key) {
  const u64 h1 = Fnva64(key);
  const u64 h2 = Fnva64("scratchbird.metric.history:" + key);
  std::ostringstream out;
  out << std::hex << std::setfill('0')
      << std::setw(8) << static_cast<unsigned int>((h1 >> 32) & 0xffffffffu) << '-'
      << std::setw(4) << static_cast<unsigned int>((h1 >> 16) & 0xffffu) << '-'
      << std::setw(4) << static_cast<unsigned int>(0x7000u | (h1 & 0x0fffu)) << '-'
      << std::setw(4) << static_cast<unsigned int>(0x8000u | ((h2 >> 48) & 0x3fffu)) << '-'
      << std::setw(12) << (h2 & 0x0000ffffffffffffull);
  return out.str();
}

std::string StableMetricSeriesKey(const MetricDescriptor& descriptor,
                                  const MetricLabelSet& labels,
                                  const std::string& database_uuid,
                                  const std::string& node_uuid,
                                  const std::string& cluster_uuid) {
  std::ostringstream out;
  out << descriptor.family << '|'
      << descriptor.namespace_path << '|'
      << descriptor.producer_owner << '|'
      << (descriptor.cluster_only ? "cluster" : "local") << '|'
      << database_uuid << '|'
      << node_uuid << '|'
      << cluster_uuid << '|'
      << CanonicalMetricLabels(labels);
  return out.str();
}

MetricSeriesIdentity MakeMetricSeriesIdentity(const MetricDescriptor& descriptor,
                                              MetricLabelSet labels,
                                              const MetricRetentionPolicy& policy,
                                              std::string database_uuid,
                                              std::string node_uuid,
                                              std::string cluster_uuid) {
  MetricSeriesIdentity series;
  series.metric_family = descriptor.family;
  series.namespace_path = descriptor.namespace_path;
  series.producer_owner = descriptor.producer_owner;
  series.scope_class = descriptor.cluster_only ? "cluster" : "local";
  series.database_uuid = std::move(database_uuid);
  series.node_uuid = std::move(node_uuid);
  series.cluster_uuid = std::move(cluster_uuid);
  series.labels = std::move(labels);
  series.label_hash = StableV7LikeMetricUuid(CanonicalMetricLabels(series.labels));
  bool sensitive = false;
  for (const auto& label : descriptor.labels) {
    sensitive = sensitive || label.sensitive;
  }
  series.redaction_class = sensitive ? "contains_sensitive_labels" : "none";
  series.retention_policy_uuid = policy.policy_uuid;
  series.series_key = StableMetricSeriesKey(descriptor, series.labels, series.database_uuid, series.node_uuid, series.cluster_uuid);
  series.series_uuid = StableV7LikeMetricUuid(series.series_key);
  return series;
}

MetricRawSampleRecord MakeMetricRawSampleRecord(const MetricSeriesIdentity& series,
                                                const MetricValue& value,
                                                u64 observation_time_microseconds) {
  const u64 now = MetricHistoryNowMicroseconds();
  MetricRawSampleRecord sample;
  sample.series_uuid = series.series_uuid;
  sample.metric_family = series.metric_family;
  sample.labels = series.labels;
  sample.observation_time_microseconds = observation_time_microseconds == 0 ? now : observation_time_microseconds;
  sample.collection_time_microseconds = now;
  sample.publish_time_microseconds = now;
  sample.clock_quality = "local_monotonic";
  sample.freshness_class = "current";
  sample.value = value;
  sample.sample_uuid = StableV7LikeMetricUuid(series.series_uuid + ":" + std::to_string(sample.observation_time_microseconds) + ":" + std::to_string(sample.value.value) + ":" + sample.value.state_text);
  return sample;
}

MetricRetentionEvidenceRecord MakeMetricRetentionEvidenceRecord(std::string operation,
                                                                std::string policy_uuid,
                                                                std::string series_uuid,
                                                                std::string metric_family,
                                                                u64 cutoff_time_microseconds,
                                                                u64 rows_affected,
                                                                std::string actor_uuid,
                                                                std::string transaction_uuid,
                                                                std::string decision,
                                                                std::string detail) {
  MetricRetentionEvidenceRecord evidence;
  evidence.operation = std::move(operation);
  evidence.policy_uuid = std::move(policy_uuid);
  evidence.series_uuid = std::move(series_uuid);
  evidence.metric_family = std::move(metric_family);
  evidence.cutoff_time_microseconds = cutoff_time_microseconds;
  evidence.rows_affected = rows_affected;
  evidence.actor_uuid = std::move(actor_uuid);
  evidence.transaction_uuid = std::move(transaction_uuid);
  evidence.decision = std::move(decision);
  evidence.detail = std::move(detail);
  evidence.evidence_uuid = StableV7LikeMetricUuid(evidence.operation + ":" + evidence.policy_uuid + ":" + std::to_string(MetricHistoryNowMicroseconds()) + ":" + evidence.detail);
  return evidence;
}

MetricValidationResult ConfigureMetricHistoryPersistence(std::string history_path,
                                                         std::vector<MetricRetentionPolicy> policies) {
  if (history_path.empty()) {
    return MetricError("SB-METRICS-HISTORY-PERSISTENCE-DISABLED", "empty history path");
  }
  if (policies.empty()) {
    policies = BaselineMetricRetentionPolicies();
  }
  for (const auto& policy : policies) {
    const auto valid = ValidateMetricRetentionPolicy(policy);
    if (!valid.ok) {
      return valid;
    }
  }
  MetricHistoryStore store = LoadOrSeedStore(history_path, policies);
  if (store.policies.empty()) {
    store.policies = policies;
  }
  const auto written = WriteMetricHistoryStore(history_path, store);
  if (!written.ok) {
    return written;
  }
  std::lock_guard<std::mutex> lock(ConfigMutex());
  Config().enabled = true;
  Config().path = std::move(history_path);
  Config().policies = std::move(policies);
  Config().source_sequence = 0;
  return MetricOk();
}

void DisableMetricHistoryPersistence() {
  std::lock_guard<std::mutex> lock(ConfigMutex());
  Config() = MetricHistoryConfig{};
}

bool MetricHistoryPersistenceEnabled() {
  std::lock_guard<std::mutex> lock(ConfigMutex());
  return Config().enabled;
}

std::string ConfiguredMetricHistoryPath() {
  std::lock_guard<std::mutex> lock(ConfigMutex());
  return Config().path;
}

MetricValidationResult PersistMetricValueForHistory(const MetricDescriptor& descriptor, const MetricValue& value) {
  std::string path;
  {
    std::lock_guard<std::mutex> lock(ConfigMutex());
    if (!Config().enabled || Config().path.empty()) {
      return MetricOk();
    }
    path = Config().path;
  }
  return AppendMetricRawSample(path, descriptor, value, 0);
}

MetricValidationResult AppendMetricRawSample(const std::string& path,
                                             const MetricDescriptor& descriptor,
                                             const MetricValue& value,
                                             u64 observation_time_microseconds) {
  if (descriptor.readiness == MetricReadiness::contract_ready_unwired) {
    return MetricError("SB-METRICS-HISTORY-NO-FAKE-SAMPLES", descriptor.family);
  }
  if (path.empty()) {
    return MetricError("SB-METRICS-HISTORY-PERSISTENCE-DISABLED", descriptor.family);
  }
  MetricHistoryStore store = LoadOrSeedStore(path, BaselineMetricRetentionPolicies());
  const auto& policy = EffectivePolicyForDescriptor(descriptor, store.policies);
  const auto policy_valid = ValidateMetricRetentionPolicy(policy);
  if (!policy_valid.ok) {
    return policy_valid;
  }
  if (policy.mode == MetricRetentionMode::current_only || policy.mode == MetricRetentionMode::rollup_only) {
    return MetricOk();
  }
  MetricSeriesIdentity series = MakeMetricSeriesIdentity(descriptor, value.labels, policy);
  auto existing = std::find_if(store.series.begin(), store.series.end(), [&](const MetricSeriesIdentity& item) {
    return item.series_uuid == series.series_uuid;
  });
  if (existing == store.series.end()) {
    u64 family_count = 0;
    for (const auto& item : store.series) {
      if (item.metric_family == descriptor.family) {
        ++family_count;
      }
    }
    if (family_count >= policy.max_cardinality) {
      store.evidence.push_back(MakeMetricRetentionEvidenceRecord("series_overflow",
                                                                 policy.policy_uuid,
                                                                 {},
                                                                 descriptor.family,
                                                                 0,
                                                                 0,
                                                                 "system.metrics_runtime",
                                                                 {},
                                                                 "rejected",
                                                                 "series cardinality exceeded"));
      (void)WriteMetricHistoryStore(path, store);
      return MetricError("SB-METRICS-HISTORY-SERIES-CARDINALITY-EXCEEDED", descriptor.family);
    }
    store.series.push_back(series);
  } else {
    series = *existing;
  }
  MetricRawSampleRecord sample = MakeMetricRawSampleRecord(series, value, observation_time_microseconds);
  {
    std::lock_guard<std::mutex> lock(ConfigMutex());
    sample.source_sequence = ++Config().source_sequence;
    sample.sample_uuid = StableV7LikeMetricUuid(sample.series_uuid + ":" + std::to_string(sample.observation_time_microseconds) + ":" + std::to_string(sample.source_sequence));
  }
  store.raw_samples.push_back(sample);
  return WriteMetricHistoryStore(path, store);
}

MetricHistoryStore LoadMetricHistoryStore(const std::string& path) {
  MetricHistoryStore store;
  if (path.empty()) {
    return store;
  }
  std::ifstream input(path);
  if (!input.good()) {
    return store;
  }
  std::string line;
  while (std::getline(input, line)) {
    const auto fields = Split(line, '\t');
    if (fields.size() < 2 || fields[0] != kMagic) {
      continue;
    }
    if (fields[1] == "POLICY" && fields.size() >= 15) {
      MetricRetentionPolicy policy;
      policy.policy_uuid = HexDecode(fields[2]);
      policy.policy_name = HexDecode(fields[3]);
      policy.scope = HexDecode(fields[4]);
      policy.mode = MetricRetentionModeFromName(fields[5]);
      policy.raw_retention_seconds = ParseU64(fields[6]);
      policy.rollup_retention_seconds = ParseU64(fields[7]);
      policy.rollup_grains = ParseRollupGrains(HexDecode(fields[8]));
      policy.purge_batch_limit = ParseU64(fields[9]);
      policy.max_cardinality = ParseU64(fields[10]);
      policy.overflow_behavior = HexDecode(fields[11]);
      policy.edit_right = HexDecode(fields[12]);
      policy.default_admin_group = HexDecode(fields[13]);
      policy.evidence_required = fields[14] == "1";
      store.policies.push_back(policy);
    } else if (fields[1] == "SERIES" && fields.size() >= 15) {
      MetricSeriesIdentity series;
      series.series_uuid = HexDecode(fields[2]);
      series.series_key = HexDecode(fields[3]);
      series.metric_family = HexDecode(fields[4]);
      series.namespace_path = HexDecode(fields[5]);
      series.producer_owner = HexDecode(fields[6]);
      series.scope_class = HexDecode(fields[7]);
      series.database_uuid = HexDecode(fields[8]);
      series.node_uuid = HexDecode(fields[9]);
      series.cluster_uuid = HexDecode(fields[10]);
      series.labels = ParseLabels(fields[11]);
      series.label_hash = HexDecode(fields[12]);
      series.redaction_class = HexDecode(fields[13]);
      series.retention_policy_uuid = HexDecode(fields[14]);
      store.series.push_back(series);
    } else if (fields[1] == "SAMPLE" && fields.size() >= 18) {
      MetricRawSampleRecord sample;
      sample.sample_uuid = HexDecode(fields[2]);
      sample.series_uuid = HexDecode(fields[3]);
      sample.metric_family = HexDecode(fields[4]);
      sample.labels = ParseLabels(fields[5]);
      sample.observation_time_microseconds = ParseU64(fields[6]);
      sample.collection_time_microseconds = ParseU64(fields[7]);
      sample.publish_time_microseconds = ParseU64(fields[8]);
      sample.source_sequence = ParseU64(fields[9]);
      sample.clock_quality = HexDecode(fields[10]);
      sample.freshness_class = HexDecode(fields[11]);
      sample.value.family = sample.metric_family;
      sample.value.labels = sample.labels;
      sample.value.type = fields[12] == std::string("histogram") ? MetricType::histogram :
                          fields[12] == std::string("gauge") ? MetricType::gauge :
                          fields[12] == std::string("state") ? MetricType::state :
                          fields[12] == std::string("derived") ? MetricType::derived : MetricType::counter;
      sample.value.value = ParseDouble(fields[13]);
      sample.value.count = ParseU64(fields[14]);
      sample.value.sum = ParseDouble(fields[15]);
      sample.value.buckets = DecodeBuckets(HexDecode(fields[16]));
      sample.value.state_text = HexDecode(fields[17]);
      sample.evidence_uuid = fields.size() > 18 ? HexDecode(fields[18]) : "";
      store.raw_samples.push_back(sample);
    } else if (fields[1] == "ROLLUP" && fields.size() >= 20) {
      MetricRollupRecord rollup;
      rollup.rollup_uuid = HexDecode(fields[2]);
      rollup.series_uuid = HexDecode(fields[3]);
      rollup.metric_family = HexDecode(fields[4]);
      rollup.grain = MetricRollupGrainFromName(fields[5]);
      rollup.window_start_microseconds = ParseU64(fields[6]);
      rollup.window_end_microseconds = ParseU64(fields[7]);
      rollup.sample_count = ParseU64(fields[8]);
      rollup.min_value = ParseDouble(fields[9]);
      rollup.max_value = ParseDouble(fields[10]);
      rollup.avg_value = ParseDouble(fields[11]);
      rollup.last_value = ParseDouble(fields[12]);
      rollup.sum_value = ParseDouble(fields[13]);
      rollup.histogram_count = ParseU64(fields[14]);
      rollup.histogram_sum = ParseDouble(fields[15]);
      rollup.histogram_buckets = HexDecode(fields[16]);
      rollup.state_transition_count = ParseU64(fields[17]);
      rollup.last_state_text = HexDecode(fields[18]);
      rollup.evidence_uuid = HexDecode(fields[19]);
      store.rollups.push_back(rollup);
    } else if (fields[1] == "EVIDENCE" && fields.size() >= 13) {
      MetricRetentionEvidenceRecord evidence;
      evidence.evidence_uuid = HexDecode(fields[2]);
      evidence.operation = HexDecode(fields[3]);
      evidence.policy_uuid = HexDecode(fields[4]);
      evidence.series_uuid = HexDecode(fields[5]);
      evidence.metric_family = HexDecode(fields[6]);
      evidence.cutoff_time_microseconds = ParseU64(fields[7]);
      evidence.rows_affected = ParseU64(fields[8]);
      evidence.actor_uuid = HexDecode(fields[9]);
      evidence.transaction_uuid = HexDecode(fields[10]);
      evidence.decision = HexDecode(fields[11]);
      evidence.detail = HexDecode(fields[12]);
      store.evidence.push_back(evidence);
    }
  }
  return store;
}

MetricValidationResult WriteMetricHistoryStore(const std::string& path, const MetricHistoryStore& store) {
  if (path.empty()) {
    return MetricError("SB-METRICS-HISTORY-PERSISTENCE-DISABLED", "empty history path");
  }
  const std::string tmp = path + ".tmp";
  std::ofstream output(tmp, std::ios::trunc);
  if (!output.good()) {
    return MetricError("SB-METRICS-HISTORY-WRITE-FAILED", path);
  }
  for (const auto& policy : store.policies) {
    WritePolicy(output, policy);
  }
  for (const auto& series : store.series) {
    WriteSeries(output, series);
  }
  for (const auto& sample : store.raw_samples) {
    WriteSample(output, sample);
  }
  for (const auto& rollup : store.rollups) {
    WriteRollup(output, rollup);
  }
  for (const auto& evidence : store.evidence) {
    WriteEvidence(output, evidence);
  }
  output.close();
  if (!output.good()) {
    return MetricError("SB-METRICS-HISTORY-WRITE-FAILED", path);
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    return MetricError("SB-METRICS-HISTORY-WRITE-FAILED", path + ":rename");
  }
  return MetricOk();
}

MetricValidationResult GenerateMetricRollups(const std::string& path, MetricRollupGrain grain) {
  MetricHistoryStore store = LoadOrSeedStore(path, BaselineMetricRetentionPolicies());
  const u64 window_microseconds = MetricRollupGrainWindowSeconds(grain) * 1000000ull;
  std::map<std::pair<std::string, u64>, std::vector<MetricRawSampleRecord>> groups;
  for (const auto& sample : store.raw_samples) {
    const u64 window = (sample.observation_time_microseconds / window_microseconds) * window_microseconds;
    groups[{sample.series_uuid, window}].push_back(sample);
  }
  u64 created = 0;
  for (const auto& [key, samples] : groups) {
    const bool exists = std::any_of(store.rollups.begin(), store.rollups.end(), [&](const MetricRollupRecord& rollup) {
      return rollup.series_uuid == key.first && rollup.grain == grain && rollup.window_start_microseconds == key.second;
    });
    if (exists || samples.empty()) {
      continue;
    }
    MetricRollupRecord rollup;
    rollup.series_uuid = key.first;
    rollup.metric_family = samples.front().metric_family;
    rollup.grain = grain;
    rollup.window_start_microseconds = key.second;
    rollup.window_end_microseconds = key.second + window_microseconds;
    rollup.sample_count = static_cast<u64>(samples.size());
    rollup.min_value = std::numeric_limits<double>::max();
    rollup.max_value = std::numeric_limits<double>::lowest();
    for (const auto& sample : samples) {
      rollup.min_value = std::min(rollup.min_value, sample.value.value);
      rollup.max_value = std::max(rollup.max_value, sample.value.value);
      rollup.sum_value += sample.value.value;
      rollup.last_value = sample.value.value;
      rollup.histogram_count += sample.value.count;
      rollup.histogram_sum += sample.value.sum;
      if (!sample.value.state_text.empty() && sample.value.state_text != rollup.last_state_text) {
        ++rollup.state_transition_count;
        rollup.last_state_text = sample.value.state_text;
      }
    }
    rollup.avg_value = rollup.sample_count == 0 ? 0.0 : rollup.sum_value / static_cast<double>(rollup.sample_count);
    rollup.evidence_uuid = StableV7LikeMetricUuid("rollup-evidence:" + rollup.series_uuid + ":" + std::to_string(rollup.window_start_microseconds));
    rollup.rollup_uuid = StableV7LikeMetricUuid("rollup:" + rollup.series_uuid + ":" + MetricRollupGrainName(grain) + ":" + std::to_string(rollup.window_start_microseconds));
    store.rollups.push_back(rollup);
    ++created;
  }
  store.evidence.push_back(MakeMetricRetentionEvidenceRecord("rollup_generate",
                                                             {},
                                                             {},
                                                             {},
                                                             0,
                                                             created,
                                                             "system.metrics_rollup",
                                                             {},
                                                             "allowed",
                                                             std::string("grain=") + MetricRollupGrainName(grain)));
  return WriteMetricHistoryStore(path, store);
}

MetricValidationResult ApplyMetricRetentionCleanup(const std::string& path,
                                                   u64 now_microseconds,
                                                   std::string actor_uuid,
                                                   std::string transaction_uuid) {
  MetricHistoryStore store = LoadOrSeedStore(path, BaselineMetricRetentionPolicies());
  if (now_microseconds == 0) {
    now_microseconds = MetricHistoryNowMicroseconds();
  }
  u64 removed = 0;
  std::vector<MetricRawSampleRecord> kept_samples;
  kept_samples.reserve(store.raw_samples.size());
  for (const auto& sample : store.raw_samples) {
    const MetricDescriptor* descriptor = DefaultMetricRegistry().FindDescriptorOrAlias(sample.metric_family);
    if (descriptor == nullptr) {
      kept_samples.push_back(sample);
      continue;
    }
    const auto& policy = EffectivePolicyForDescriptor(*descriptor, store.policies);
    if (policy.raw_retention_seconds == 0) {
      kept_samples.push_back(sample);
      continue;
    }
    const u64 retention_microseconds = policy.raw_retention_seconds * static_cast<u64>(1000000);
    const u64 cutoff = now_microseconds - std::min<u64>(now_microseconds, retention_microseconds);
    if (sample.observation_time_microseconds < cutoff && removed < policy.purge_batch_limit) {
      ++removed;
      continue;
    }
    kept_samples.push_back(sample);
  }
  store.evidence.push_back(MakeMetricRetentionEvidenceRecord("raw_cleanup",
                                                             {},
                                                             {},
                                                             {},
                                                             now_microseconds,
                                                             removed,
                                                             std::move(actor_uuid),
                                                             std::move(transaction_uuid),
                                                             "allowed",
                                                             "policy retention cleanup"));
  store.raw_samples = std::move(kept_samples);
  return WriteMetricHistoryStore(path, store);
}

MetricValidationResult UpsertMetricRetentionPolicy(const std::string& path,
                                                   MetricRetentionPolicy policy,
                                                   std::string actor_uuid,
                                                   std::string transaction_uuid) {
  const auto valid = ValidateMetricRetentionPolicy(policy);
  if (!valid.ok) {
    return valid;
  }
  MetricHistoryStore store = LoadOrSeedStore(path, BaselineMetricRetentionPolicies());
  auto existing = std::find_if(store.policies.begin(), store.policies.end(), [&](const MetricRetentionPolicy& item) {
    return item.policy_uuid == policy.policy_uuid || item.policy_name == policy.policy_name;
  });
  const std::string operation = existing == store.policies.end() ? "policy_create" : "policy_alter";
  if (existing == store.policies.end()) {
    store.policies.push_back(policy);
  } else {
    *existing = policy;
  }
  store.evidence.push_back(MakeMetricRetentionEvidenceRecord(operation,
                                                             policy.policy_uuid,
                                                             {},
                                                             {},
                                                             0,
                                                             1,
                                                             std::move(actor_uuid),
                                                             std::move(transaction_uuid),
                                                             "allowed",
                                                             policy.policy_name));
  return WriteMetricHistoryStore(path, store);
}

}  // namespace scratchbird::core::metrics
