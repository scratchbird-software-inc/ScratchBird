// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_METRICS_HISTORY_MODEL
// Persistent metric history model: series identity, raw samples, rollups, and
// evidence records. This layer is ScratchBird-owned history, not WAL.

#include "metric_retention_policy.hpp"

#include <string>
#include <optional>
#include <tuple>
#include <vector>

namespace scratchbird::core::metrics {

using scratchbird::core::platform::u64;

struct MetricHistoryBinding : MetricDescriptorBinding {
  MetricUuid database_uuid;
  MetricUuid node_uuid;
  MetricUuid cluster_uuid;
  bool operator==(const MetricHistoryBinding&) const = default;
};
using MetricHistorySeriesKey = std::tuple<MetricUuid, MetricUuid, MetricUuid,
    MetricUuid, MetricUuid, std::vector<std::pair<std::string, MetricLabelValue>>>;

struct MetricSeriesIdentity : MetricHistoryBinding {
  MetricUuid series_uuid;
  MetricHistorySeriesKey series_key;
  std::string metric_family;
  std::string namespace_path;
  std::string producer_owner;
  std::string scope_class = "local";
  MetricLabelSet labels;
  std::string redaction_class = "none";
};

struct MetricRawSampleRecord : MetricHistoryBinding {
  MetricUuid sample_uuid;
  MetricUuid series_uuid;
  std::string metric_family;
  MetricLabelSet labels;
  u64 sample_time_utc_ns = 0;
  u64 collection_time_utc_ns = 0;
  u64 publication_time_utc_ns = 0;
  u64 source_sequence = 0;
  u64 revision = 0;
  std::string clock_quality;
  std::string freshness_class;
  MetricValue value;
  MetricUuid evidence_uuid;
};

struct MetricRollupRecord {
  MetricUuid rollup_uuid;
  MetricUuid series_uuid;
  std::string metric_family;
  MetricRollupGrain grain = MetricRollupGrain::one_minute;
  u64 window_start_microseconds = 0;
  u64 window_end_microseconds = 0;
  u64 sample_count = 0;
  double min_value = 0.0;
  double max_value = 0.0;
  double avg_value = 0.0;
  double last_value = 0.0;
  double sum_value = 0.0;
  u64 histogram_count = 0;
  double histogram_sum = 0.0;
  std::string histogram_buckets;
  u64 state_transition_count = 0;
  std::string last_state_text;
  MetricUuid evidence_uuid;
};

struct MetricRetentionEvidenceRecord {
  MetricUuid evidence_uuid;
  std::string operation;
  MetricUuid policy_uuid;
  MetricUuid series_uuid;
  std::string metric_family;
  u64 cutoff_time_microseconds = 0;
  u64 rows_affected = 0;
  MetricUuid actor_uuid;
  MetricUuid transaction_uuid;
  std::string decision;
  std::string detail;
};

struct MetricHistoryStore {
  std::vector<MetricRetentionPolicy> policies;
  std::vector<MetricSeriesIdentity> series;
  std::vector<MetricRawSampleRecord> raw_samples;
  std::vector<MetricRollupRecord> rollups;
  std::vector<MetricRetentionEvidenceRecord> evidence;
};

enum class MetricHistoryRecordError {
  none, invalid_identity, invalid_binding, invalid_labels, invalid_observation,
  identity_issuance_failed
};
template<class Record> struct MetricHistoryRecordResult {
  MetricHistoryRecordError error = MetricHistoryRecordError::invalid_binding;
  std::optional<Record> record;
  bool ok() const { return error == MetricHistoryRecordError::none && record.has_value(); }
};
// Construction/shape and exact scalar admission only. Caller retains live
// catalog, clock, aggregate and policy authority. These functions do not look
// up names, write storage or publish data.
MetricHistoryRecordResult<MetricSeriesIdentity> MakeMetricSeriesIdentity(
    const MetricDescriptor&, MetricLabelSet, const MetricRetentionPolicy&,
    const MetricHistoryBinding&, const MetricUuid& catalog_series_uuid);
MetricHistoryRecordResult<MetricRawSampleRecord> MakeMetricRawSampleRecord(
    const MetricDescriptor&, const MetricSeriesIdentity&, const MetricValue&,
    u64 sample_time_utc_ns, u64 collection_time_utc_ns, u64 source_sequence);
MetricHistoryRecordResult<MetricRetentionEvidenceRecord> MakeMetricRetentionEvidenceRecord(
    MetricRetentionEvidenceRecord requested);

MetricValidationResult ConfigureMetricHistoryPersistence(std::string history_path,
                                                         std::vector<MetricRetentionPolicy> policies = {});
void DisableMetricHistoryPersistence();
bool MetricHistoryPersistenceEnabled();
std::string ConfiguredMetricHistoryPath();
MetricValidationResult PersistMetricValueForHistory(const MetricDescriptor& descriptor, const MetricValue& value);
MetricValidationResult AppendMetricRawSample(const std::string& path,
                                             const MetricDescriptor& descriptor,
                                             const MetricValue& value,
                                             u64 observation_time_microseconds = 0);
MetricHistoryStore LoadMetricHistoryStore(const std::string& path);
MetricValidationResult WriteMetricHistoryStore(const std::string& path, const MetricHistoryStore& store);
MetricValidationResult GenerateMetricRollups(const std::string& path, MetricRollupGrain grain);
MetricValidationResult ApplyMetricRetentionCleanup(const std::string& path,
                                                   u64 now_microseconds,
                                                   std::string actor_uuid,
                                                   std::string transaction_uuid);
MetricValidationResult UpsertMetricRetentionPolicy(const std::string& path,
                                                   MetricRetentionPolicy policy,
                                                   std::string actor_uuid,
                                                   std::string transaction_uuid);

u64 MetricHistoryNowMicroseconds();

}  // namespace scratchbird::core::metrics
