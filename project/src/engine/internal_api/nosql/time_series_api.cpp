// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/time_series_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "nosql/nosql_batch_point_lookup_support.hpp"
#include "nosql/nosql_surface_support.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

struct MetricSummary {
  EngineApiU64 count = 0;
  double min = std::numeric_limits<double>::infinity();
  double max = -std::numeric_limits<double>::infinity();
  double sum = 0.0;
};

struct BucketKey {
  std::string meta_key;
  EngineApiU64 meta_hash = 0;
  EngineApiI64 bucket_start_ns = 0;

  bool operator<(const BucketKey& other) const {
    if (meta_key != other.meta_key) { return meta_key < other.meta_key; }
    if (meta_hash != other.meta_hash) { return meta_hash < other.meta_hash; }
    return bucket_start_ns < other.bucket_start_ns;
  }
};

struct BucketState {
  BucketKey key;
  EngineApiI64 bucket_end_ns = 0;
  std::map<std::string, std::vector<double>> metric_columns;
  std::map<std::string, MetricSummary> summaries;
  EngineApiU64 late_arrival_count = 0;
};

struct RollupKey {
  EngineApiI64 interval_ns = 0;
  std::string meta_key;
  EngineApiU64 meta_hash = 0;
  std::string metric_name;
  EngineApiI64 rollup_start_ns = 0;

  bool operator<(const RollupKey& other) const {
    if (interval_ns != other.interval_ns) { return interval_ns < other.interval_ns; }
    if (meta_key != other.meta_key) { return meta_key < other.meta_key; }
    if (meta_hash != other.meta_hash) { return meta_hash < other.meta_hash; }
    if (metric_name != other.metric_name) { return metric_name < other.metric_name; }
    return rollup_start_ns < other.rollup_start_ns;
  }
};

template <typename TResult>
TResult DiagnosticResult(const EngineRequestContext& context,
                         const std::string& operation_id,
                         const char* diagnostic_code) {
  return MakeApiBehaviorDiagnostic<TResult>(
      context,
      operation_id,
      MakeInvalidRequestDiagnostic(operation_id, diagnostic_code));
}

void AddSelectionEvidence(const EngineNoSqlPhysicalProviderSelection& selection,
                          EngineApiResult* result) {
  for (const auto& item : selection.evidence) {
    AddApiBehaviorEvidence(result, "time_series_physical_provider", item);
  }
}

bool IsPhysicalTimeSeriesAppendRequest(
    const EngineTimeSeriesAppendRequest& request) {
  return request.physical_append || !request.points.empty() ||
         request.bucket_duration_ns != 0 || !request.rollup_intervals_ns.empty() ||
         request.physical_proof.proof_supplied;
}

bool IsLateArrival(const EngineTimeSeriesAppendRequest& request,
                   const EngineTimeSeriesPoint& point) {
  return request.late_arrival_watermark_ns != 0 &&
         point.timestamp_ns < request.late_arrival_watermark_ns;
}

std::string FormatDouble(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(6) << value;
  return out.str();
}

std::string JoinValues(const std::vector<double>& values) {
  std::string joined;
  for (const auto value : values) {
    if (!joined.empty()) { joined += ','; }
    joined += FormatDouble(value);
  }
  return joined;
}

EngineApiU64 StableHash(const std::string& value) {
  EngineApiU64 hash = 1469598103934665603ULL;
  for (const unsigned char ch : value) {
    hash ^= static_cast<EngineApiU64>(ch);
    hash *= 1099511628211ULL;
  }
  return hash == 0 ? 1 : hash;
}

std::string MetaKey(const std::vector<EngineTimeSeriesPointTag>& tags) {
  if (tags.empty()) { return "meta:empty"; }
  std::vector<std::pair<std::string, std::string>> sorted;
  sorted.reserve(tags.size());
  for (const auto& tag : tags) {
    sorted.push_back({tag.key, tag.value});
  }
  std::sort(sorted.begin(), sorted.end());
  std::string key;
  for (const auto& [name, value] : sorted) {
    if (!key.empty()) { key += ';'; }
    key += name + '=' + value;
  }
  return key;
}

EngineApiI64 BucketStart(EngineApiI64 timestamp_ns,
                         EngineApiI64 bucket_duration_ns) {
  if (timestamp_ns >= 0) {
    return (timestamp_ns / bucket_duration_ns) * bucket_duration_ns;
  }
  const EngineApiI64 adjusted =
      ((-timestamp_ns + bucket_duration_ns - 1) / bucket_duration_ns) *
      bucket_duration_ns;
  return -adjusted;
}

std::string MetricName(const EngineTimeSeriesPoint& point) {
  return point.metric_name.empty() ? "metric" : point.metric_name;
}

void AddToSummary(double value, MetricSummary* summary) {
  ++summary->count;
  summary->min = std::min(summary->min, value);
  summary->max = std::max(summary->max, value);
  summary->sum += value;
}

template <typename TResult>
std::optional<TResult> ValidatePhysicalProof(
    const EngineTimeSeriesAppendRequest& request,
    const std::string& operation_id,
    const EngineTimeSeriesPhysicalProof& proof) {
  if (!proof.proof_supplied) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kTimeSeriesPhysicalProofMissing);
  }
  if (proof.provider_contract.family != EngineNoSqlProviderFamily::kTimeSeries) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kNoSqlProviderFamilyUnsupported);
  }
  const auto selection = SelectLocalNoSqlPhysicalProvider(proof.provider_contract);
  if (!selection.selected) {
    auto failure = MakeApiBehaviorDiagnostic<TResult>(
        request.context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id,
                                     selection.missing_diagnostics.empty()
                                         ? selection.refusal_diagnostics.front()
                                         : selection.missing_diagnostics.front()));
    AddSelectionEvidence(selection, &failure);
    return failure;
  }
  if (!proof.time_meta_bucket_store_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kTimeSeriesBucketStoreProofMissing);
  }
  if (!proof.columnar_metric_page_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kTimeSeriesColumnarMetricPageProofMissing);
  }
  if (!proof.summary_min_max_count_sum_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kTimeSeriesSummaryProofMissing);
  }
  if (!proof.rollup_materialization_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kTimeSeriesRollupProofMissing);
  }
  if (!proof.late_arrival_delta_merge_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kTimeSeriesLateArrivalPolicyProofMissing);
  }
  return std::nullopt;
}

const char* LateArrivalPolicyName(EngineTimeSeriesLateArrivalPolicy policy) {
  switch (policy) {
    case EngineTimeSeriesLateArrivalPolicy::kReject: return "reject";
    case EngineTimeSeriesLateArrivalPolicy::kDeltaMergeReopen:
      return "delta_merge_reopen";
  }
  return "unknown";
}

void AddTimeSeriesEvidence(EngineApiResult* result,
                           const EngineNoSqlPhysicalProviderSelection& selection,
                           const EngineTimeSeriesAppendRequest& request,
                           EngineApiU64 bucket_count,
                           EngineApiU64 column_page_count,
                           EngineApiU64 summary_count,
                           EngineApiU64 rollup_count,
                           EngineApiU64 late_arrival_count) {
  AddEngineNoSqlSurfaceEvidence(
      result, "time_series", "physical_bucketed_columnar_append");
  AddSelectionEvidence(selection, result);
  AddApiBehaviorEvidence(result,
                         "time_series_physical_access",
                         "local_time_meta_bucket_provider");
  AddApiBehaviorEvidence(result,
                         "time_series_bucket_selection",
                         "time_bucket_duration_ns=" +
                             std::to_string(request.bucket_duration_ns) +
                             ";meta_key_hash=stable_fnv1a");
  AddApiBehaviorEvidence(result,
                         "time_series_columnar_metric_pages",
                         "metric_pages=" + std::to_string(column_page_count));
  AddApiBehaviorEvidence(result,
                         "time_series_summary_maintenance",
                         "min_max_count_sum;summaries=" +
                             std::to_string(summary_count));
  AddApiBehaviorEvidence(result,
                         "time_series_rollup_materialization",
                         "rows=" + std::to_string(rollup_count));
  AddApiBehaviorEvidence(result,
                         "time_series_late_arrival_policy",
                         std::string(LateArrivalPolicyName(
                             request.late_arrival_policy)) +
                             ";sealed_columnar_rewrite=false;delta_rows=" +
                             std::to_string(late_arrival_count));
  AddApiBehaviorEvidence(result,
                         "time_series_bucket_count",
                         std::to_string(bucket_count));
  AddApiBehaviorEvidence(result, "behavior_store_scan_selected", "false");
  AddApiBehaviorEvidence(result, "descriptor_scan_selected", "false");
  AddApiBehaviorEvidence(result, "row_mga_recheck_evidence", "required");
  AddApiBehaviorEvidence(result, "row_security_recheck_evidence", "required");
  AddApiBehaviorEvidence(result,
                         "mga_finality_authority",
                         "engine_transaction_inventory");
  AddApiBehaviorEvidence(result,
                         "provider_transaction_finality_authority",
                         "false");
  AddApiBehaviorEvidence(result, "provider_visibility_authority", "false");
  AddApiBehaviorEvidence(result,
                         "parser_transaction_finality_authority",
                         "false");
  AddApiBehaviorEvidence(result, "client_autocommit_authority", "false");
}

EngineTimeSeriesAppendResult PhysicalTimeSeriesAppend(
    const EngineTimeSeriesAppendRequest& request,
    const std::string& operation_id) {
  if (request.points.empty()) {
    return DiagnosticResult<EngineTimeSeriesAppendResult>(
        request.context, operation_id, kTimeSeriesPointBatchRequired);
  }
  if (request.bucket_duration_ns <= 0) {
    return DiagnosticResult<EngineTimeSeriesAppendResult>(
        request.context, operation_id, kTimeSeriesBucketDurationRequired);
  }
  if (auto failure = ValidatePhysicalProof<EngineTimeSeriesAppendResult>(
          request, operation_id, request.physical_proof)) {
    return *failure;
  }
  if (request.late_arrival_policy == EngineTimeSeriesLateArrivalPolicy::kReject) {
    for (const auto& point : request.points) {
      if (IsLateArrival(request, point)) {
        return DiagnosticResult<EngineTimeSeriesAppendResult>(
            request.context, operation_id, kTimeSeriesLateArrivalRejected);
      }
    }
  }

  const auto selection =
      SelectLocalNoSqlPhysicalProvider(request.physical_proof.provider_contract);
  std::map<BucketKey, BucketState> buckets;
  std::map<RollupKey, MetricSummary> rollups;
  struct LatePoint {
    EngineTimeSeriesPoint point;
    std::string meta_key;
    EngineApiU64 meta_hash = 0;
    EngineApiI64 bucket_start_ns = 0;
  };
  std::vector<LatePoint> late_points;

  for (const auto& point : request.points) {
    const std::string meta_key = MetaKey(point.metadata_tags);
    const EngineApiU64 meta_hash = StableHash(meta_key);
    const EngineApiI64 bucket_start =
        BucketStart(point.timestamp_ns, request.bucket_duration_ns);
    const BucketKey bucket_key{meta_key, meta_hash, bucket_start};
    auto& bucket = buckets[bucket_key];
    bucket.key = bucket_key;
    bucket.bucket_end_ns = bucket_start + request.bucket_duration_ns;
    const std::string metric = MetricName(point);
    bucket.metric_columns[metric].push_back(point.numeric_value);
    AddToSummary(point.numeric_value, &bucket.summaries[metric]);

    const bool late_arrival = IsLateArrival(request, point);
    if (late_arrival) {
      ++bucket.late_arrival_count;
      late_points.push_back({point, meta_key, meta_hash, bucket_start});
    }

    for (const auto interval : request.rollup_intervals_ns) {
      if (interval <= 0) { continue; }
      const RollupKey rollup_key{
          interval,
          meta_key,
          meta_hash,
          metric,
          BucketStart(point.timestamp_ns, interval)};
      AddToSummary(point.numeric_value, &rollups[rollup_key]);
    }
  }

  auto result =
      MakeApiBehaviorSuccess<EngineTimeSeriesAppendResult>(request.context,
                                                           operation_id);
  std::vector<EngineNoSqlBatchPointLookupItem> lookup_items;

  EngineApiU64 column_page_count = 0;
  EngineApiU64 summary_count = 0;
  for (const auto& [key, bucket] : buckets) {
    const auto bucket_lookup_key =
        key.meta_key + "|" + std::to_string(key.meta_hash) + "|" +
        std::to_string(key.bucket_start_ns);
    lookup_items.push_back(
        {bucket_lookup_key,
         {},
         0.0,
         "time_series_bucket",
         {{"row_kind", "bucket"},
          {"bucket_start_ns", std::to_string(key.bucket_start_ns)}}});
    AddApiBehaviorRow(
        &result,
        {{"surface", "time_series"},
         {"row_kind", "bucket"},
         {"meta_key", key.meta_key},
         {"meta_hash", std::to_string(key.meta_hash)},
         {"bucket_start_ns", std::to_string(key.bucket_start_ns)},
         {"bucket_end_ns", std::to_string(bucket.bucket_end_ns)},
         {"bucket_duration_ns", std::to_string(request.bucket_duration_ns)},
         {"late_arrival_count", std::to_string(bucket.late_arrival_count)},
         {"merge_policy", LateArrivalPolicyName(request.late_arrival_policy)},
         {"row_mga_recheck_required", "true"},
         {"row_security_recheck_required", "true"}});

    for (const auto& [metric, values] : bucket.metric_columns) {
      ++column_page_count;
      lookup_items.push_back(
          {bucket_lookup_key + "|column|" + metric,
           {},
           0.0,
           JoinValues(values),
           {{"row_kind", "column_page"}, {"metric_name", metric}}});
      AddApiBehaviorRow(
          &result,
          {{"surface", "time_series"},
           {"row_kind", "column_page"},
           {"meta_key", key.meta_key},
           {"meta_hash", std::to_string(key.meta_hash)},
           {"bucket_start_ns", std::to_string(key.bucket_start_ns)},
           {"bucket_end_ns", std::to_string(bucket.bucket_end_ns)},
           {"metric_name", metric},
           {"column_layout", "metric_value_columnar_page"},
           {"values", JoinValues(values)},
           {"value_count", std::to_string(values.size())},
           {"row_mga_recheck_required", "true"},
           {"row_security_recheck_required", "true"}});
    }
    for (const auto& [metric, summary] : bucket.summaries) {
      ++summary_count;
      lookup_items.push_back(
          {bucket_lookup_key + "|summary|" + metric,
           {},
           0.0,
           FormatDouble(summary.sum),
           {{"row_kind", "summary"}, {"metric_name", metric}}});
      AddApiBehaviorRow(
          &result,
          {{"surface", "time_series"},
           {"row_kind", "summary"},
           {"meta_key", key.meta_key},
           {"meta_hash", std::to_string(key.meta_hash)},
           {"bucket_start_ns", std::to_string(key.bucket_start_ns)},
           {"metric_name", metric},
           {"min", FormatDouble(summary.min)},
           {"max", FormatDouble(summary.max)},
           {"count", std::to_string(summary.count)},
           {"sum", FormatDouble(summary.sum)},
           {"row_mga_recheck_required", "true"},
           {"row_security_recheck_required", "true"}});
    }
  }

  for (const auto& late : late_points) {
    lookup_items.push_back(
        {late.meta_key + "|" + std::to_string(late.meta_hash) + "|" +
             std::to_string(late.bucket_start_ns) + "|late|" +
             std::to_string(late.point.timestamp_ns),
         {},
         0.0,
         FormatDouble(late.point.numeric_value),
         {{"row_kind", "late_arrival_delta"},
          {"metric_name", MetricName(late.point)}}});
    AddApiBehaviorRow(
        &result,
        {{"surface", "time_series"},
         {"row_kind", "late_arrival_delta"},
         {"meta_key", late.meta_key},
         {"meta_hash", std::to_string(late.meta_hash)},
         {"bucket_start_ns", std::to_string(late.bucket_start_ns)},
         {"timestamp_ns", std::to_string(late.point.timestamp_ns)},
         {"metric_name", MetricName(late.point)},
         {"value", FormatDouble(late.point.numeric_value)},
         {"late_path", "delta_page_merge_reopen_bucket"},
         {"merge_policy", LateArrivalPolicyName(request.late_arrival_policy)},
         {"row_mga_recheck_required", "true"},
         {"row_security_recheck_required", "true"}});
  }

  EngineApiU64 rollup_count = 0;
  for (const auto& [key, summary] : rollups) {
    ++rollup_count;
    lookup_items.push_back(
        {std::to_string(key.interval_ns) + "|" + key.meta_key + "|" +
             std::to_string(key.meta_hash) + "|" + key.metric_name + "|" +
             std::to_string(key.rollup_start_ns),
         {},
         0.0,
         FormatDouble(summary.sum),
         {{"row_kind", "rollup"}, {"metric_name", key.metric_name}}});
    AddApiBehaviorRow(
        &result,
        {{"surface", "time_series"},
         {"row_kind", "rollup"},
         {"rollup_interval_ns", std::to_string(key.interval_ns)},
         {"rollup_start_ns", std::to_string(key.rollup_start_ns)},
         {"rollup_end_ns", std::to_string(key.rollup_start_ns + key.interval_ns)},
         {"meta_key", key.meta_key},
         {"meta_hash", std::to_string(key.meta_hash)},
         {"metric_name", key.metric_name},
         {"aggregate", "min_max_count_sum"},
         {"min", FormatDouble(summary.min)},
         {"max", FormatDouble(summary.max)},
         {"count", std::to_string(summary.count)},
         {"sum", FormatDouble(summary.sum)},
         {"row_mga_recheck_required", "true"},
         {"row_security_recheck_required", "true"}});
  }

  if (auto failure = AddEngineNoSqlOrderedBatchLookupEvidence<
          EngineTimeSeriesAppendResult>(
          request.context,
          operation_id,
          "time_series",
          scratchbird::core::index::BatchPointLookupPurpose::
              time_series_bucket,
          selection,
          lookup_items,
          &result)) {
    return *failure;
  }

  AddTimeSeriesEvidence(&result,
                        selection,
                        request,
                        static_cast<EngineApiU64>(buckets.size()),
                        column_page_count,
                        summary_count,
                        rollup_count,
                        static_cast<EngineApiU64>(late_points.size()));
  result.dml_summary.rows_changed =
      static_cast<EngineApiU64>(request.points.size());
  result.dml_summary.append_calls = 1;
  result.dml_summary.index_probes =
      static_cast<EngineApiU64>(buckets.size() + column_page_count);
  result.dml_summary.visible_rows_scanned = 0;
  return result;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_TIME_SERIES_API_BEHAVIOR
EngineTimeSeriesAppendResult EngineTimeSeriesAppend(
    const EngineTimeSeriesAppendRequest& request) {
  constexpr const char* kOperation = "nosql.time_series_append";
  if (!request.context.cluster_authority_available &&
      EngineNoSqlRequiresClusterAuthority(request)) {
    return EngineNoSqlClusterAuthorityUnavailable<EngineTimeSeriesAppendResult>(
        request, kOperation);
  }
  if (IsPhysicalTimeSeriesAppendRequest(request)) {
    return PhysicalTimeSeriesAppend(request, kOperation);
  }
  if (EngineNoSqlRequestsHeavyImmutableGeneration(request)) {
    return EngineNoSqlPublishHeavyImmutableGeneration<EngineTimeSeriesAppendResult>(
        request,
        kOperation,
        "time_series",
        "columnar_summary",
        "time_series_columnar_summary_generation_v1",
        "time_series_append");
  }
  auto result = EngineNoSqlPersistedWriteResult<EngineTimeSeriesAppendResult>(
      request, kOperation, "time_series_point", true, "appended");
  if (result.ok) {
    AddEngineNoSqlSurfaceEvidence(
        &result, "time_series", "persisted_time_series_append");
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
