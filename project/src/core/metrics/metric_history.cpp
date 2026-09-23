// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_history.hpp"
#include "metric_history_store_codec.hpp"
#include "metric_label_key.hpp"
#include "uuid.hpp"
#include <cmath>
#include <filesystem>

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

const MetricRetentionPolicy* EffectivePolicyForDescriptor(const MetricDescriptorBinding& binding,
    const std::vector<MetricRetentionPolicy>& configured) {
  const MetricRetentionPolicy* found = nullptr;
  for (const auto& policy : configured) {
    if (policy.policy_uuid != binding.retention_policy_uuid ||
        policy.generation != binding.retention_policy_generation) continue;
    if (found || !ValidateMetricRetentionPolicy(policy).ok) return nullptr;
    found = &policy;
  }
  return found;
}
MetricValidationResult AddEvidence(MetricHistoryStore& store, std::string operation,
    const MetricUuid& policy, const MetricUuid& series, std::string family,
    u64 cutoff, u64 affected, const MetricUuid& actor, const MetricUuid& transaction,
    std::string detail) {
  MetricRetentionEvidenceRecord request;
  request.operation = std::move(operation); request.policy_uuid = policy;
  request.series_uuid = series; request.metric_family = std::move(family);
  request.cutoff_time_microseconds = cutoff; request.rows_affected = affected;
  request.actor_uuid = actor; request.transaction_uuid = transaction;
  request.decision = "allowed"; request.detail = std::move(detail);
  auto evidence = MakeMetricRetentionEvidenceRecord(std::move(request));
  if (!evidence.ok()) return MetricError("SB-METRICS-HISTORY-EVIDENCE-BINDING-INVALID", "native policy, actor and transaction bindings required");
  store.evidence.push_back(std::move(*evidence.record)); return MetricOk();
}
// Existing rollup fields are binary64 summaries. Reject values that cannot be
// represented exactly instead of silently narrowing an exact observation.
std::optional<double> ExactRollupScalar(const MetricScalar& value) {
  if (const auto* v = std::get_if<double>(&value))
    return std::isfinite(*v) ? std::optional<double>(*v) : std::nullopt;
  if (const auto* v = std::get_if<std::uint64_t>(&value)) {
    if (*v <= (UINT64_C(1) << 53)) return static_cast<double>(*v);
  }
  if (const auto* v = std::get_if<std::int64_t>(&value)) {
    if (*v >= -(INT64_C(1) << 53) && *v <= (INT64_C(1) << 53)) return static_cast<double>(*v);
  }
  if (const auto* v = std::get_if<bool>(&value)) return *v ? 1.0 : 0.0;
  if (const auto* v = std::get_if<MetricEnumValue>(&value)) {
    if (v->code <= (UINT64_C(1) << 53)) return static_cast<double>(v->code);
  }
  return std::nullopt;
}

MetricHistoryStore LoadOrSeedStore(const std::string& path, const std::vector<MetricRetentionPolicy>& policies) {
  MetricHistoryStore store = LoadMetricHistoryStore(path);
  if (!store.load_status.ok) return store;
  if (store.policies.empty()) {
    store.policies = policies;
  }
  return store;
}

}  // namespace

u64 MetricHistoryNowMicroseconds() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

MetricValidationResult ConfigureMetricHistoryPersistence(std::string history_path,
                                                         std::vector<MetricRetentionPolicy> policies) {
  if (history_path.empty()) {
    return MetricError("SB-METRICS-HISTORY-PERSISTENCE-DISABLED", "empty history path");
  }
  for (const auto& policy : policies) {
    const auto valid = ValidateMetricRetentionPolicy(policy);
    if (!valid.ok) {
      return valid;
    }
  }
  MetricHistoryStore store = LoadOrSeedStore(history_path, policies);
  if (!store.load_status.ok) return store.load_status;
  if (store.policies.empty()) return MetricError("SB-METRICS-HISTORY-POLICY-BINDING-REQUIRED", history_path);
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

MetricValidationResult RegisterMetricHistorySeries(const std::string& path,
    const MetricDescriptor& descriptor, const MetricSeriesIdentity& series,
    const MetricRetentionPolicy& policy) {
  const auto checked = MakeMetricSeriesIdentity(descriptor, series.labels, policy, series,
      series.series_uuid, series.series_definition_generation);
  if (!checked.ok()) return MetricError("SB-METRICS-HISTORY-SERIES-BINDING-INVALID", descriptor.family);
  std::lock_guard<std::mutex> lock(ConfigMutex());
  auto store = LoadMetricHistoryStore(path);
  if (!store.load_status.ok) return store.load_status;
  if (!EffectivePolicyForDescriptor(descriptor, store.policies))
    return MetricError("SB-METRICS-HISTORY-POLICY-BINDING-REQUIRED", descriptor.family);
  std::size_t family_count = 0;
  for (const auto& existing : store.series) {
    if (existing.database_uuid != series.database_uuid)
      return MetricError("SB-METRICS-HISTORY-DATABASE-MISMATCH", descriptor.family);
    if (existing.series_uuid == series.series_uuid || existing.series_key == checked.record->series_key) {
      if (existing.series_uuid == series.series_uuid &&
          existing.series_key == checked.record->series_key &&
          existing.series_definition_generation == series.series_definition_generation &&
          static_cast<const MetricHistoryBinding&>(existing) == static_cast<const MetricHistoryBinding&>(series))
        return MetricOk();
      return MetricError("SB-METRICS-HISTORY-SERIES-BINDING-CONFLICT", descriptor.family);
    }
    if (existing.metric_uuid == series.metric_uuid) ++family_count;
  }
  if (family_count >= policy.max_cardinality)
    return MetricError("SB-METRICS-HISTORY-SERIES-CARDINALITY-EXCEEDED", descriptor.family);
  store.series.push_back(*checked.record);
  return WriteMetricHistoryStore(path, store);
}

MetricValidationResult AppendMetricRawSample(const std::string& path,
    const MetricDescriptor& descriptor, const MetricValue& value,
    u64 observation_time_microseconds) {
  if (descriptor.readiness != MetricReadiness::implemented && descriptor.readiness != MetricReadiness::derived)
    return MetricError("SB-METRICS-HISTORY-NO-FAKE-SAMPLES", descriptor.family);
  if (path.empty()) return MetricError("SB-METRICS-HISTORY-PERSISTENCE-DISABLED", descriptor.family);
  std::lock_guard<std::mutex> lock(ConfigMutex());
  auto store = LoadMetricHistoryStore(path);
  if (!store.load_status.ok) return store.load_status;
  const auto* policy = EffectivePolicyForDescriptor(descriptor, store.policies);
  if (!policy) return MetricError("SB-METRICS-HISTORY-POLICY-BINDING-REQUIRED", descriptor.family);
  if (policy->mode == MetricRetentionMode::current_only || policy->mode == MetricRetentionMode::rollup_only)
    return MetricOk();
  const MetricSeriesIdentity* selected = nullptr;
  for (const auto& series : store.series) {
    if (static_cast<const MetricDescriptorBinding&>(series) != static_cast<const MetricDescriptorBinding&>(descriptor) ||
        MakeMetricSeriesKey(series.metric_family, series.labels) != MakeMetricSeriesKey(value.family, value.labels)) continue;
    if (selected) return MetricError("SB-METRICS-HISTORY-SERIES-BINDING-AMBIGUOUS", descriptor.family);
    selected = &series;
  }
  if (!selected) return MetricError("SB-METRICS-HISTORY-SERIES-BINDING-REQUIRED", descriptor.family);
  u64 sequence = 0;
  for (const auto& sample : store.raw_samples) sequence = std::max(sequence, sample.source_sequence);
  if (sequence == UINT64_MAX) return MetricError("SB-METRICS-HISTORY-SEQUENCE-EXHAUSTED", descriptor.family);
  const auto collected = MetricHistoryNowMicroseconds();
  const auto observed = observation_time_microseconds ? observation_time_microseconds : collected;
  if (observed > UINT64_MAX / 1000 || collected > UINT64_MAX / 1000)
    return MetricError("SB-METRICS-HISTORY-TIME-OVERFLOW", descriptor.family);
  auto sample = MakeMetricRawSampleRecord(descriptor, *selected, value,
      observed * 1000, collected * 1000, sequence + 1);
  if (!sample.ok()) return MetricError("SB-METRICS-HISTORY-OBSERVATION-INVALID", descriptor.family);
  store.raw_samples.push_back(std::move(*sample.record));
  return WriteMetricHistoryStore(path, store);
}

MetricHistoryStore LoadMetricHistoryStore(const std::string& path) {
  MetricHistoryStore store;
  const auto invalid = [&] {
    store = {};
    store.load_status = MetricError("SB-METRICS-HISTORY-READ-FAILED", path);
    return store;
  };
  if (path.empty()) return invalid();
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error) return invalid();
  if (!exists) return store;
  try {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return invalid();
    const auto size = input.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > history_codec::kMaximumBytes)
      return invalid();
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!input || input.peek() != std::char_traits<char>::eof() ||
        !DecodeMetricHistoryStoreBinary(bytes, &store)) return invalid();
    return store;
  } catch (...) { return invalid(); }
}

MetricValidationResult WriteMetricHistoryStore(const std::string& path, const MetricHistoryStore& store) {
  if (path.empty()) {
    return MetricError("SB-METRICS-HISTORY-PERSISTENCE-DISABLED", "empty history path");
  }
  if (!store.load_status.ok) return store.load_status;
  const auto encoded = EncodeMetricHistoryStoreBinary(store);
  if (!encoded) return MetricError("SB-METRICS-HISTORY-ENCODE-FAILED", path);
  const std::string tmp = path + ".tmp";
  std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
  if (!output.good()) return MetricError("SB-METRICS-HISTORY-WRITE-FAILED", path);
  output.write(reinterpret_cast<const char*>(encoded->data()), encoded->size());
  output.close();
  if (!output.good()) {
    return MetricError("SB-METRICS-HISTORY-WRITE-FAILED", path);
  }
#if defined(_WIN32)
  (void)std::remove(path.c_str());
#endif
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    return MetricError("SB-METRICS-HISTORY-WRITE-FAILED", path + ":rename");
  }
  return MetricOk();
}

MetricValidationResult GenerateMetricRollups(const std::string& path, MetricRollupGrain grain,
    const MetricUuid& actor, const MetricUuid& transaction) {
  const u64 seconds = MetricRollupGrainWindowSeconds(grain);
  if (!seconds || seconds > UINT64_MAX / 1000000 ||
      !MetricSystemUuidValid(actor) || !MetricSystemUuidValid(transaction))
    return MetricError("METRIC.RETENTION_POLICY_INVALID", "rollup grain and native actor/transaction bindings required");
  std::lock_guard<std::mutex> lock(ConfigMutex());
  auto store = LoadMetricHistoryStore(path);
  if (!store.load_status.ok) return store.load_status;
  const u64 width = seconds * 1000000;
  std::map<std::pair<MetricUuid, u64>, std::vector<const MetricRawSampleRecord*>> groups;
  for (const auto& sample : store.raw_samples) {
    const auto observed = sample.sample_time_utc_ns / 1000;
    groups[{sample.series_uuid, (observed / width) * width}].push_back(&sample);
  }
  for (auto& [key, samples] : groups) {
    if (samples.empty() || std::any_of(store.rollups.begin(), store.rollups.end(), [&](const auto& r) {
          return r.series_uuid == key.first && r.grain == grain && r.window_start_microseconds == key.second;
        })) continue;
    const auto* policy = EffectivePolicyForDescriptor(*samples.front(), store.policies);
    if (!policy) return MetricError("SB-METRICS-HISTORY-POLICY-BINDING-REQUIRED", path);
    if (policy->mode == MetricRetentionMode::current_only ||
        std::find(policy->rollup_grains.begin(), policy->rollup_grains.end(), grain) == policy->rollup_grains.end()) continue;
    std::sort(samples.begin(), samples.end(), [](const auto* l, const auto* r) {
      return std::pair{l->sample_time_utc_ns, l->source_sequence} < std::pair{r->sample_time_utc_ns, r->source_sequence};
    });
    MetricRollupRecord rollup;
    rollup.series_uuid = key.first; rollup.metric_family = samples.front()->metric_family;
    rollup.grain = grain; rollup.window_start_microseconds = key.second;
    if (key.second > UINT64_MAX - width) return MetricError("SB-METRICS-HISTORY-TIME-OVERFLOW", path);
    rollup.window_end_microseconds = key.second + width; rollup.sample_count = samples.size();
    rollup.min_value = std::numeric_limits<double>::max();
    rollup.max_value = std::numeric_limits<double>::lowest();
    for (const auto* sample : samples) {
      if (static_cast<const MetricHistoryBinding&>(*sample) != static_cast<const MetricHistoryBinding&>(*samples.front()))
        return MetricError("SB-METRICS-HISTORY-ROLLUP-BINDING-MISMATCH", path);
      const auto number = ExactRollupScalar(sample->value.value);
      if (!number) return MetricError("SB-METRICS-HISTORY-ROLLUP-SCALAR-NOT-REPRESENTABLE", sample->metric_family);
      rollup.min_value = std::min(rollup.min_value, *number); rollup.max_value = std::max(rollup.max_value, *number);
      rollup.sum_value += *number; rollup.last_value = *number;
      if (!std::isfinite(rollup.sum_value) || sample->value.count > UINT64_MAX - rollup.histogram_count)
        return MetricError("SB-METRICS-HISTORY-ROLLUP-OVERFLOW", path);
      rollup.histogram_count += sample->value.count;
      if (sample->value.type == MetricType::histogram) {
        const auto sum = ExactRollupScalar(sample->value.sum);
        if (!sum) return MetricError("SB-METRICS-HISTORY-ROLLUP-SCALAR-NOT-REPRESENTABLE", sample->metric_family);
        rollup.histogram_sum += *sum;
        if (!std::isfinite(rollup.histogram_sum)) return MetricError("SB-METRICS-HISTORY-ROLLUP-OVERFLOW", path);
      }
      if (!sample->value.state_text.empty() && sample->value.state_text != rollup.last_state_text) {
        ++rollup.state_transition_count; rollup.last_state_text = sample->value.state_text;
      }
    }
    rollup.avg_value = rollup.sum_value / static_cast<double>(rollup.sample_count);
    const auto identity = uuid::IssueRuntimeIdentityV7();
    if (!identity) return MetricError("SB-METRICS-HISTORY-IDENTITY-ISSUANCE-FAILED", path);
    rollup.rollup_uuid = *identity;
    const auto evidence = AddEvidence(store, "rollup_generate", policy->policy_uuid,
        rollup.series_uuid, rollup.metric_family, key.second, samples.size(), actor, transaction,
        std::string("grain=") + MetricRollupGrainName(grain));
    if (!evidence.ok) return evidence;
    rollup.evidence_uuid = store.evidence.back().evidence_uuid;
    store.rollups.push_back(std::move(rollup));
  }
  return WriteMetricHistoryStore(path, store);
}

MetricValidationResult ApplyMetricRetentionCleanup(const std::string& path, u64 now,
    MetricUuid actor, MetricUuid transaction) {
  if (!MetricSystemUuidValid(actor) || !MetricSystemUuidValid(transaction))
    return MetricError("SB-METRICS-HISTORY-EVIDENCE-BINDING-INVALID", path);
  std::lock_guard<std::mutex> lock(ConfigMutex());
  auto store = LoadMetricHistoryStore(path);
  if (!store.load_status.ok) return store.load_status;
  if (!now) now = MetricHistoryNowMicroseconds();
  std::map<MetricUuid, u64> removed;
  std::vector<MetricRawSampleRecord> kept;
  for (const auto& sample : store.raw_samples) {
    const auto* policy = EffectivePolicyForDescriptor(sample, store.policies);
    if (!policy) return MetricError("SB-METRICS-HISTORY-POLICY-BINDING-REQUIRED", sample.metric_family);
    if (policy->raw_retention_seconds &&
        MetricRetentionTimeExpired(sample.sample_time_utc_ns / 1000, now, policy->raw_retention_seconds) &&
        removed[policy->policy_uuid] < policy->purge_batch_limit) ++removed[policy->policy_uuid];
    else kept.push_back(sample);
  }
  for (const auto& [policy, count] : removed) {
    const auto result = AddEvidence(store, "raw_cleanup", policy, {}, {}, now, count,
        actor, transaction, "policy retention cleanup");
    if (!result.ok) return result;
  }
  store.raw_samples = std::move(kept);
  return WriteMetricHistoryStore(path, store);
}

MetricValidationResult UpsertMetricRetentionPolicy(const std::string& path,
    MetricRetentionPolicy policy, MetricUuid actor, MetricUuid transaction) {
  const auto valid = ValidateMetricRetentionPolicy(policy);
  if (!valid.ok) return valid;
  std::lock_guard<std::mutex> lock(ConfigMutex());
  auto store = LoadMetricHistoryStore(path);
  if (!store.load_status.ok) return store.load_status;
  auto existing = std::find_if(store.policies.begin(), store.policies.end(),
      [&](const auto& item) { return item.policy_uuid == policy.policy_uuid; });
  const auto operation = existing == store.policies.end() ? "policy_create" : "policy_alter";
  const auto evidence = AddEvidence(store, operation, policy.policy_uuid, {}, {}, 0, 1,
      actor, transaction, policy.policy_name);
  if (!evidence.ok) return evidence;
  if (existing == store.policies.end()) store.policies.push_back(std::move(policy));
  else *existing = std::move(policy);
  return WriteMetricHistoryStore(path, store);
}

} // namespace scratchbird::core::metrics
