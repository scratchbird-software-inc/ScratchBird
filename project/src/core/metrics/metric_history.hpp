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
#include <vector>

namespace scratchbird::core::metrics {

using scratchbird::core::platform::u64;

struct MetricSeriesIdentity {
  std::string series_uuid;
  std::string series_key;
  std::string metric_family;
  std::string namespace_path;
  std::string producer_owner;
  std::string scope_class = "local";
  std::string database_uuid;
  std::string node_uuid;
  std::string cluster_uuid;
  MetricLabelSet labels;
  std::string label_hash;
  std::string redaction_class = "none";
  std::string retention_policy_uuid;
};

struct MetricRawSampleRecord {
  std::string sample_uuid;
  std::string series_uuid;
  std::string metric_family;
  MetricLabelSet labels;
  u64 observation_time_microseconds = 0;
  u64 collection_time_microseconds = 0;
  u64 publish_time_microseconds = 0;
  u64 source_sequence = 0;
  std::string clock_quality = "local_monotonic";
  std::string freshness_class = "current";
  MetricValue value;
  std::string evidence_uuid;
};

struct MetricRollupRecord {
  std::string rollup_uuid;
  std::string series_uuid;
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
  std::string evidence_uuid;
};

struct MetricRetentionEvidenceRecord {
  std::string evidence_uuid;
  std::string operation;
  std::string policy_uuid;
  std::string series_uuid;
  std::string metric_family;
  u64 cutoff_time_microseconds = 0;
  u64 rows_affected = 0;
  std::string actor_uuid;
  std::string transaction_uuid;
  std::string decision = "allowed";
  std::string detail;
};

struct MetricHistoryStore {
  std::vector<MetricRetentionPolicy> policies;
  std::vector<MetricSeriesIdentity> series;
  std::vector<MetricRawSampleRecord> raw_samples;
  std::vector<MetricRollupRecord> rollups;
  std::vector<MetricRetentionEvidenceRecord> evidence;
};

MetricSeriesIdentity MakeMetricSeriesIdentity(const MetricDescriptor& descriptor,
                                              MetricLabelSet labels,
                                              const MetricRetentionPolicy& policy,
                                              std::string database_uuid = {},
                                              std::string node_uuid = {},
                                              std::string cluster_uuid = {});
MetricRawSampleRecord MakeMetricRawSampleRecord(const MetricSeriesIdentity& series,
                                                const MetricValue& value,
                                                u64 observation_time_microseconds = 0);
MetricRetentionEvidenceRecord MakeMetricRetentionEvidenceRecord(std::string operation,
                                                                std::string policy_uuid,
                                                                std::string series_uuid,
                                                                std::string metric_family,
                                                                u64 cutoff_time_microseconds,
                                                                u64 rows_affected,
                                                                std::string actor_uuid,
                                                                std::string transaction_uuid,
                                                                std::string decision,
                                                                std::string detail);

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

std::string CanonicalMetricLabels(const MetricLabelSet& labels);
std::string StableMetricSeriesKey(const MetricDescriptor& descriptor,
                                  const MetricLabelSet& labels,
                                  const std::string& database_uuid,
                                  const std::string& node_uuid,
                                  const std::string& cluster_uuid);
std::string StableV7LikeMetricUuid(const std::string& key);
u64 MetricHistoryNowMicroseconds();

}  // namespace scratchbird::core::metrics
