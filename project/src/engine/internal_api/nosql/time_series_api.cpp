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
#include "datatype_catalog_manifest.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "nosql/nosql_batch_point_lookup_support.hpp"
#include "nosql/nosql_surface_support.hpp"
#include "query/expression_api.hpp"
#include "security/security_model.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cfenv>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
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

namespace {

constexpr const char* kBoundTimeSeriesOperation = "model.time_series.read.v1";

EngineBoundTimeSeriesReadResultV1 BoundTimeSeriesRefusal(
    const EngineBoundTimeSeriesReadRequestV1& request,
    const char* diagnostic_id,
    std::string detail) {
  auto result = MakeApiBehaviorDiagnostic<EngineBoundTimeSeriesReadResultV1>(
      request.context, kBoundTimeSeriesOperation,
      MakeEngineApiDiagnostic(diagnostic_id,
                              "engine.model.time_series.read.refused",
                              std::move(detail)));
  result.rows.clear();
  result.downsample_rows.clear();
  result.result_shape = {};
  return result;
}

bool CheckedTimeSeriesAdd(const std::uint64_t value, std::uint64_t* total) {
  if (total == nullptr ||
      value > std::numeric_limits<std::uint64_t>::max() - *total) {
    return false;
  }
  *total += value;
  return true;
}

bool CheckedTimeSeriesMultiply(const std::uint64_t left,
                               const std::uint64_t right,
                               std::uint64_t* product) {
  if (product == nullptr ||
      (right != 0 && left > std::numeric_limits<std::uint64_t>::max() / right)) {
    return false;
  }
  *product = left * right;
  return true;
}

bool AccountTimeSeriesMemory(const std::uint64_t bytes,
                             const std::uint64_t limit,
                             std::uint64_t* retained) {
  return CheckedTimeSeriesAdd(bytes, retained) && *retained <= limit;
}

bool CheckedTimeSeriesOwnedStringBytes(const std::string& value,
                                       std::uint64_t* total) {
  return value.capacity() < std::numeric_limits<std::uint64_t>::max() &&
         CheckedTimeSeriesAdd(
             static_cast<std::uint64_t>(value.capacity()) + 1, total);
}

bool AccountTimeSeriesDiagnosticMemoryV1(
    const EngineApiDiagnostic& diagnostic, std::uint64_t* total) {
  std::uint64_t field_bytes = 0;
  if (!CheckedTimeSeriesMultiply(
          static_cast<std::uint64_t>(diagnostic.fields.capacity()),
          sizeof(EngineApiDiagnosticField), &field_bytes) ||
      !CheckedTimeSeriesAdd(field_bytes, total) ||
      !CheckedTimeSeriesOwnedStringBytes(diagnostic.code, total) ||
      !CheckedTimeSeriesOwnedStringBytes(diagnostic.message_key, total) ||
      !CheckedTimeSeriesOwnedStringBytes(diagnostic.detail, total)) {
    return false;
  }
  for (const auto& field : diagnostic.fields) {
    if (!CheckedTimeSeriesOwnedStringBytes(field.key, total) ||
        !CheckedTimeSeriesOwnedStringBytes(field.value, total)) {
      return false;
    }
  }
  return true;
}

bool AccountTimeSeriesEngineDescriptorMemoryV1(
    const EngineDescriptor& descriptor, std::uint64_t* total) {
  return CheckedTimeSeriesOwnedStringBytes(
             descriptor.descriptor_uuid.canonical, total) &&
         CheckedTimeSeriesOwnedStringBytes(descriptor.descriptor_kind,
                                           total) &&
         CheckedTimeSeriesOwnedStringBytes(descriptor.canonical_type_name,
                                           total) &&
         CheckedTimeSeriesOwnedStringBytes(descriptor.encoded_descriptor,
                                           total);
}

bool AccountTimeSeriesStorageDescriptorMemoryV1(
    const MgaRelationStorageDescriptor& descriptor, std::uint64_t* total) {
  const auto account_uuid = [&](const EngineUuid& uuid) {
    return CheckedTimeSeriesOwnedStringBytes(uuid.canonical, total);
  };
  if (!account_uuid(descriptor.descriptor_uuid) ||
      !account_uuid(descriptor.database_uuid) ||
      !account_uuid(descriptor.schema_uuid) ||
      !account_uuid(descriptor.relation_uuid) ||
      !account_uuid(descriptor.primary_filespace_uuid) ||
      !CheckedTimeSeriesOwnedStringBytes(descriptor.relation_kind, total) ||
      !CheckedTimeSeriesOwnedStringBytes(descriptor.storage_profile, total) ||
      !CheckedTimeSeriesOwnedStringBytes(descriptor.row_identity_rule, total) ||
      !CheckedTimeSeriesOwnedStringBytes(descriptor.version_identity_rule,
                                         total) ||
      !CheckedTimeSeriesOwnedStringBytes(descriptor.mutation_rule, total) ||
      !CheckedTimeSeriesOwnedStringBytes(descriptor.visibility_rule, total) ||
      !CheckedTimeSeriesOwnedStringBytes(descriptor.cleanup_rule, total) ||
      !CheckedTimeSeriesOwnedStringBytes(descriptor.recovery_rule, total) ||
      !CheckedTimeSeriesOwnedStringBytes(descriptor.descriptor_status, total)) {
    return false;
  }
  std::uint64_t allocation_bytes = 0;
  if (!CheckedTimeSeriesMultiply(
          static_cast<std::uint64_t>(descriptor.columns.capacity()),
          sizeof(MgaRelationColumnStorageDescriptor), &allocation_bytes) ||
      !CheckedTimeSeriesAdd(allocation_bytes, total) ||
      !CheckedTimeSeriesMultiply(
          static_cast<std::uint64_t>(descriptor.indexes.capacity()),
          sizeof(MgaRelationIndexStorageDescriptor), &allocation_bytes) ||
      !CheckedTimeSeriesAdd(allocation_bytes, total) ||
      !CheckedTimeSeriesMultiply(
          static_cast<std::uint64_t>(
              descriptor.required_evidence_kinds.capacity()),
          sizeof(std::string), &allocation_bytes) ||
      !CheckedTimeSeriesAdd(allocation_bytes, total)) {
    return false;
  }
  for (const auto& column : descriptor.columns) {
    if (!account_uuid(column.column_uuid) ||
        !CheckedTimeSeriesOwnedStringBytes(column.canonical_name_key, total) ||
        !AccountTimeSeriesEngineDescriptorMemoryV1(column.value_descriptor,
                                                   total) ||
        !CheckedTimeSeriesOwnedStringBytes(column.storage_class, total) ||
        !CheckedTimeSeriesOwnedStringBytes(column.charset_uuid, total) ||
        !CheckedTimeSeriesOwnedStringBytes(column.collation_uuid, total) ||
        !CheckedTimeSeriesOwnedStringBytes(column.overflow_policy, total)) {
      return false;
    }
  }
  for (const auto& index : descriptor.indexes) {
    if (!account_uuid(index.index_uuid) ||
        !CheckedTimeSeriesOwnedStringBytes(index.family, total) ||
        !CheckedTimeSeriesOwnedStringBytes(index.profile, total) ||
        !CheckedTimeSeriesOwnedStringBytes(index.predicate_kind, total) ||
        !CheckedTimeSeriesOwnedStringBytes(index.predicate_column, total) ||
        !CheckedTimeSeriesOwnedStringBytes(index.predicate_value, total) ||
        !CheckedTimeSeriesOwnedStringBytes(index.residency_policy, total) ||
        !CheckedTimeSeriesMultiply(
            static_cast<std::uint64_t>(index.key_envelopes.capacity()),
            sizeof(std::string), &allocation_bytes) ||
        !CheckedTimeSeriesAdd(allocation_bytes, total) ||
        !CheckedTimeSeriesMultiply(
            static_cast<std::uint64_t>(index.include_columns.capacity()),
            sizeof(std::string), &allocation_bytes) ||
        !CheckedTimeSeriesAdd(allocation_bytes, total)) {
      return false;
    }
    for (const auto& value : index.key_envelopes) {
      if (!CheckedTimeSeriesOwnedStringBytes(value, total)) return false;
    }
    for (const auto& value : index.include_columns) {
      if (!CheckedTimeSeriesOwnedStringBytes(value, total)) return false;
    }
  }
  for (const auto& evidence : descriptor.required_evidence_kinds) {
    if (!CheckedTimeSeriesOwnedStringBytes(evidence, total)) return false;
  }
  return true;
}

bool AccountTimeSeriesCrudRowMemoryV1(const CrudRowVersionRecord& row,
                                     std::uint64_t* total) {
  std::uint64_t value_bytes = 0;
  if (!CheckedTimeSeriesMultiply(
          static_cast<std::uint64_t>(row.values.capacity()),
          sizeof(std::pair<std::string, std::string>), &value_bytes) ||
      !CheckedTimeSeriesAdd(value_bytes, total) ||
      !CheckedTimeSeriesOwnedStringBytes(row.table_uuid, total) ||
      !CheckedTimeSeriesOwnedStringBytes(row.row_uuid, total) ||
      !CheckedTimeSeriesOwnedStringBytes(row.version_uuid, total) ||
      !CheckedTimeSeriesOwnedStringBytes(row.temporary_session_uuid, total) ||
      !CheckedTimeSeriesOwnedStringBytes(row.previous_version_uuid, total)) {
    return false;
  }
  for (const auto& [key, value] : row.values) {
    if (!CheckedTimeSeriesOwnedStringBytes(key, total) ||
        !CheckedTimeSeriesOwnedStringBytes(value, total)) {
      return false;
    }
  }
  return true;
}

std::optional<std::uint64_t> TimeSeriesMgaReadCarrierMemoryBytesV1(
    const MgaVisibleHeapRelationReadResult& read) {
  std::uint64_t bytes = sizeof(read);
  std::uint64_t allocation_bytes = 0;
  if (!AccountTimeSeriesDiagnosticMemoryV1(read.diagnostic, &bytes) ||
      !AccountTimeSeriesStorageDescriptorMemoryV1(read.descriptor, &bytes) ||
      !CheckedTimeSeriesMultiply(
          static_cast<std::uint64_t>(read.visible_rows.capacity()),
          sizeof(CrudRowVersionRecord), &allocation_bytes) ||
      !CheckedTimeSeriesAdd(allocation_bytes, &bytes) ||
      !CheckedTimeSeriesMultiply(
          static_cast<std::uint64_t>(read.evidence.capacity()),
          sizeof(EngineEvidenceReference), &allocation_bytes) ||
      !CheckedTimeSeriesAdd(allocation_bytes, &bytes)) {
    return std::nullopt;
  }
  for (const auto& row : read.visible_rows) {
    if (!AccountTimeSeriesCrudRowMemoryV1(row, &bytes)) return std::nullopt;
  }
  for (const auto& evidence : read.evidence) {
    if (!CheckedTimeSeriesOwnedStringBytes(evidence.evidence_kind, &bytes) ||
        !CheckedTimeSeriesOwnedStringBytes(evidence.evidence_id, &bytes)) {
      return std::nullopt;
    }
  }
  return bytes;
}

class ScopedTimeSeriesNearestRounding {
 public:
  explicit ScopedTimeSeriesNearestRounding(const bool enabled) {
    if (!enabled) return;
    active_ = std::feholdexcept(&original_) == 0;
    if (active_ && std::fesetround(FE_TONEAREST) != 0) {
      (void)std::fesetenv(&original_);
      active_ = false;
    }
  }

  ScopedTimeSeriesNearestRounding(const ScopedTimeSeriesNearestRounding&) =
      delete;
  ScopedTimeSeriesNearestRounding& operator=(
      const ScopedTimeSeriesNearestRounding&) = delete;

  ~ScopedTimeSeriesNearestRounding() {
    if (active_) (void)std::fesetenv(&original_);
  }

  bool active() const noexcept { return active_; }

 private:
  std::fenv_t original_{};
  bool active_{false};
};

bool CanonicalTimeSeriesUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-' ||
      value == "00000000-0000-0000-0000-000000000000") {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto byte = static_cast<unsigned char>(value[index]);
    if (!((byte >= '0' && byte <= '9') ||
          (byte >= 'a' && byte <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool WellFormedTimeSeriesUtf8(const std::string_view value) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto first = static_cast<unsigned char>(value[offset]);
    std::uint32_t code_point = 0;
    std::size_t continuation_count = 0;
    if (first <= 0x7f) {
      code_point = first;
    } else if (first >= 0xc2 && first <= 0xdf) {
      code_point = first & 0x1f;
      continuation_count = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
      code_point = first & 0x0f;
      continuation_count = 2;
    } else if (first >= 0xf0 && first <= 0xf4) {
      code_point = first & 0x07;
      continuation_count = 3;
    } else {
      return false;
    }
    if (continuation_count > value.size() - offset - 1) return false;
    for (std::size_t index = 1; index <= continuation_count; ++index) {
      const auto next = static_cast<unsigned char>(value[offset + index]);
      if ((next & 0xc0) != 0x80) return false;
      code_point = (code_point << 6) | (next & 0x3f);
    }
    if ((continuation_count == 2 && code_point < 0x800) ||
        (continuation_count == 3 && code_point < 0x10000) ||
        (code_point >= 0xd800 && code_point <= 0xdfff) ||
        code_point > 0x10ffff) {
      return false;
    }
    offset += continuation_count + 1;
  }
  return true;
}

constexpr std::int64_t DaysFromCivil(const int year,
                                     const unsigned month,
                                     const unsigned day) {
  const int adjusted_year = year - (month <= 2 ? 1 : 0);
  const int era = (adjusted_year >= 0 ? adjusted_year
                                      : adjusted_year - 399) /
                  400;
  const unsigned year_of_era =
      static_cast<unsigned>(adjusted_year - era * 400);
  const unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
  const unsigned day_of_year =
      (153 * adjusted_month + 2) / 5 + day - 1;
  const unsigned day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 +
      day_of_year;
  return static_cast<std::int64_t>(era) * 146097 +
         static_cast<std::int64_t>(day_of_era) - 719468;
}

struct CivilDate {
  int year{1970};
  unsigned month{1};
  unsigned day{1};
};

CivilDate CivilFromDays(std::int64_t days) {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned day_of_era =
      static_cast<unsigned>(days - era * 146097);
  const unsigned year_of_era =
      (day_of_era - day_of_era / 1460 + day_of_era / 36524 -
       day_of_era / 146096) /
      365;
  int year = static_cast<int>(year_of_era) + static_cast<int>(era) * 400;
  const unsigned day_of_year =
      day_of_era - (365 * year_of_era + year_of_era / 4 -
                    year_of_era / 100);
  const unsigned month_prime = (5 * day_of_year + 2) / 153;
  const unsigned day = day_of_year - (153 * month_prime + 2) / 5 + 1;
  const unsigned month = month_prime < 10 ? month_prime + 3
                                           : month_prime - 9;
  year += month <= 2 ? 1 : 0;
  return {year, month, day};
}

bool LeapYear(const unsigned year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

bool ParseUnsignedField(const std::string_view value,
                        const std::size_t offset,
                        const std::size_t count,
                        unsigned* out) {
  if (out == nullptr || offset + count > value.size()) return false;
  unsigned parsed = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const char ch = value[offset + index];
    if (ch < '0' || ch > '9') return false;
    parsed = parsed * 10 + static_cast<unsigned>(ch - '0');
  }
  *out = parsed;
  return true;
}

bool ParseTimeSeriesTimestamp(const std::string_view value,
                              EngineApiI64* timestamp_ns) {
  if (timestamp_ns == nullptr || value.size() < 20 || value[4] != '-' ||
      value[7] != '-' || value[10] != 'T' || value[13] != ':' ||
      value[16] != ':') {
    return false;
  }
  unsigned year = 0;
  unsigned month = 0;
  unsigned day = 0;
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
  if (!ParseUnsignedField(value, 0, 4, &year) ||
      !ParseUnsignedField(value, 5, 2, &month) ||
      !ParseUnsignedField(value, 8, 2, &day) ||
      !ParseUnsignedField(value, 11, 2, &hour) ||
      !ParseUnsignedField(value, 14, 2, &minute) ||
      !ParseUnsignedField(value, 17, 2, &second) || year == 0 ||
      month == 0 || month > 12 || hour > 23 || minute > 59 ||
      second > 59) {
    return false;
  }
  constexpr std::array<unsigned, 12> kMonthDays{
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  unsigned maximum_day = kMonthDays[month - 1];
  if (month == 2 && LeapYear(year)) ++maximum_day;
  if (day == 0 || day > maximum_day) return false;

  std::size_t offset = 19;
  std::uint64_t fraction_ns = 0;
  if (offset < value.size() && value[offset] == '.') {
    ++offset;
    const auto fraction_begin = offset;
    while (offset < value.size() && value[offset] >= '0' &&
           value[offset] <= '9') {
      if (offset - fraction_begin >= 9) return false;
      fraction_ns = fraction_ns * 10 +
                    static_cast<std::uint64_t>(value[offset] - '0');
      ++offset;
    }
    if (offset == fraction_begin) return false;
    for (std::size_t index = offset - fraction_begin; index < 9; ++index) {
      fraction_ns *= 10;
    }
  }

  int offset_sign = 0;
  unsigned offset_hour = 0;
  unsigned offset_minute = 0;
  if (offset < value.size() && value[offset] == 'Z') {
    ++offset;
  } else if (offset + 6 == value.size() &&
             (value[offset] == '+' || value[offset] == '-') &&
             value[offset + 3] == ':' &&
             ParseUnsignedField(value, offset + 1, 2, &offset_hour) &&
             ParseUnsignedField(value, offset + 4, 2, &offset_minute)) {
    offset_sign = value[offset] == '+' ? 1 : -1;
    offset += 6;
    if (offset_hour > 14 || offset_minute > 59 ||
        (offset_hour == 14 && offset_minute != 0)) {
      return false;
    }
  } else {
    return false;
  }
  if (offset != value.size()) return false;

  constexpr __int128 kNsPerSecond = 1'000'000'000;
  constexpr __int128 kSecondsPerDay = 86'400;
  const auto days = DaysFromCivil(static_cast<int>(year), month, day);
  __int128 seconds = static_cast<__int128>(days) * kSecondsPerDay +
                     static_cast<__int128>(hour) * 3600 +
                     static_cast<__int128>(minute) * 60 + second;
  seconds -= static_cast<__int128>(offset_sign) *
             (static_cast<__int128>(offset_hour) * 3600 +
              static_cast<__int128>(offset_minute) * 60);
  const __int128 encoded = seconds * kNsPerSecond + fraction_ns;
  if (encoded < std::numeric_limits<EngineApiI64>::min() ||
      encoded > std::numeric_limits<EngineApiI64>::max()) {
    return false;
  }
  *timestamp_ns = static_cast<EngineApiI64>(encoded);
  return true;
}

std::string FormatTimeSeriesTimestamp(const EngineApiI64 timestamp_ns) {
  constexpr EngineApiI64 kNsPerSecond = 1'000'000'000;
  constexpr EngineApiI64 kSecondsPerDay = 86'400;
  EngineApiI64 seconds = timestamp_ns / kNsPerSecond;
  EngineApiI64 fraction = timestamp_ns % kNsPerSecond;
  if (fraction < 0) {
    fraction += kNsPerSecond;
    --seconds;
  }
  EngineApiI64 days = seconds / kSecondsPerDay;
  EngineApiI64 day_seconds = seconds % kSecondsPerDay;
  if (day_seconds < 0) {
    day_seconds += kSecondsPerDay;
    --days;
  }
  const auto civil = CivilFromDays(days);
  const auto hour = static_cast<unsigned>(day_seconds / 3600);
  const auto minute = static_cast<unsigned>((day_seconds % 3600) / 60);
  const auto second = static_cast<unsigned>(day_seconds % 60);
  std::string out;
  out.reserve(30);
  const auto append_fixed = [&](std::uint64_t value,
                                const unsigned width) {
    const auto start = out.size();
    out.resize(start + width, '0');
    for (unsigned offset = 0; offset < width; ++offset) {
      out[start + width - offset - 1] =
          static_cast<char>('0' + value % 10);
      value /= 10;
    }
  };
  append_fixed(static_cast<std::uint64_t>(civil.year), 4);
  out.push_back('-');
  append_fixed(civil.month, 2);
  out.push_back('-');
  append_fixed(civil.day, 2);
  out.push_back('T');
  append_fixed(hour, 2);
  out.push_back(':');
  append_fixed(minute, 2);
  out.push_back(':');
  append_fixed(second, 2);
  out.push_back('.');
  append_fixed(static_cast<std::uint64_t>(fraction), 9);
  out.push_back('Z');
  return out;
}

bool AppendUtf8CodePoint(const std::uint32_t code_point, std::string* out) {
  if (out == nullptr || (code_point >= 0xd800 && code_point <= 0xdfff) ||
      code_point > 0x10ffff) {
    return false;
  }
  if (code_point <= 0x7f) {
    out->push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7ff) {
    out->push_back(static_cast<char>(0xc0 | (code_point >> 6)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else if (code_point <= 0xffff) {
    out->push_back(static_cast<char>(0xe0 | (code_point >> 12)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else {
    out->push_back(static_cast<char>(0xf0 | (code_point >> 18)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  }
  return true;
}

bool ParseJsonString(const std::string_view input,
                     std::size_t* offset,
                     const std::function<bool()>& cancellation_requested,
                     bool* cancellation_observed,
                     std::string* output) {
  if (offset == nullptr || output == nullptr || *offset >= input.size() ||
      input[*offset] != '"') {
    return false;
  }
  ++*offset;
  output->clear();
  while (*offset < input.size()) {
    try {
      if (cancellation_requested && cancellation_requested()) {
        if (cancellation_observed != nullptr) *cancellation_observed = true;
        return false;
      }
    } catch (...) {
      if (cancellation_observed != nullptr) *cancellation_observed = true;
      return false;
    }
    const auto byte = static_cast<unsigned char>(input[*offset]);
    ++*offset;
    if (byte == '"') return WellFormedTimeSeriesUtf8(*output);
    if (byte < 0x20) return false;
    if (byte != '\\') {
      output->push_back(static_cast<char>(byte));
      continue;
    }
    if (*offset >= input.size()) return false;
    const char escaped = input[(*offset)++];
    switch (escaped) {
      case '"': output->push_back('"'); break;
      case '\\': output->push_back('\\'); break;
      case '/': output->push_back('/'); break;
      case 'b': output->push_back('\b'); break;
      case 'f': output->push_back('\f'); break;
      case 'n': output->push_back('\n'); break;
      case 'r': output->push_back('\r'); break;
      case 't': output->push_back('\t'); break;
      case 'u': {
        if (*offset + 4 > input.size()) return false;
        const auto parse_hex_quad = [&](std::uint32_t* code_unit) {
          if (code_unit == nullptr || *offset + 4 > input.size()) return false;
          *code_unit = 0;
          for (std::size_t index = 0; index < 4; ++index) {
            const char digit = input[*offset + index];
            *code_unit <<= 4;
            if (digit >= '0' && digit <= '9') *code_unit |= digit - '0';
            else if (digit >= 'a' && digit <= 'f')
              *code_unit |= digit - 'a' + 10;
            else if (digit >= 'A' && digit <= 'F')
              *code_unit |= digit - 'A' + 10;
            else
              return false;
          }
          *offset += 4;
          return true;
        };
        std::uint32_t code_point = 0;
        if (!parse_hex_quad(&code_point)) return false;
        if (code_point >= 0xd800 && code_point <= 0xdbff) {
          if (*offset + 2 > input.size() || input[*offset] != '\\' ||
              input[*offset + 1] != 'u') {
            return false;
          }
          *offset += 2;
          std::uint32_t low_surrogate = 0;
          if (!parse_hex_quad(&low_surrogate) || low_surrogate < 0xdc00 ||
              low_surrogate > 0xdfff) {
            return false;
          }
          code_point = 0x10000 + ((code_point - 0xd800) << 10) +
                       (low_surrogate - 0xdc00);
        } else if (code_point >= 0xdc00 && code_point <= 0xdfff) {
          return false;
        }
        if (!AppendUtf8CodePoint(code_point, output)) return false;
        break;
      }
      default: return false;
    }
  }
  return false;
}

bool UnsignedTextLess(const std::string& left, const std::string& right) {
  return std::lexicographical_compare(
      left.begin(), left.end(), right.begin(), right.end(),
      [](const char l, const char r) {
        return static_cast<unsigned char>(l) <
               static_cast<unsigned char>(r);
      });
}

bool AppendJsonEscape(const std::string_view value,
                      const std::function<bool()>& cancellation_requested,
                      std::string* out) {
  static constexpr char kHex[] = "0123456789abcdef";
  out->push_back('"');
  for (const auto raw : value) {
    try {
      if (cancellation_requested && cancellation_requested()) return false;
    } catch (...) {
      return false;
    }
    const auto byte = static_cast<unsigned char>(raw);
    switch (byte) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\b': *out += "\\b"; break;
      case '\f': *out += "\\f"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      default:
        if (byte < 0x20) {
          *out += "\\u00";
          out->push_back(kHex[(byte >> 4) & 0x0f]);
          out->push_back(kHex[byte & 0x0f]);
        } else {
          out->push_back(static_cast<char>(byte));
        }
    }
  }
  out->push_back('"');
  return true;
}

enum class TagParseStatus {
  kOk,
  kInvalid,
  kDuplicate,
  kResourceRefused,
  kCancelled,
};

TagParseStatus CanonicalizeTimeSeriesTags(const std::string_view input,
                                          const std::uint64_t maximum_tag_bytes,
                                          const std::uint64_t maximum_memory_bytes,
                                          const std::function<bool()>&
                                              cancellation_requested,
                                          std::string* canonical,
                                          std::uint64_t* working_memory_bytes) {
  if (canonical == nullptr || working_memory_bytes == nullptr ||
      input.empty() || input.front() != '{') {
    return TagParseStatus::kInvalid;
  }
  *working_memory_bytes = 0;
  if (input.size() > maximum_tag_bytes) {
    return TagParseStatus::kResourceRefused;
  }
  const std::uint64_t maximum_tag_count =
      static_cast<std::uint64_t>(input.size() / 4 + 1);
  std::uint64_t vector_bytes = 0;
  std::uint64_t working_bytes = sizeof(
      std::vector<std::pair<std::string, std::string>>);
  if (!CheckedTimeSeriesMultiply(
          maximum_tag_count,
          sizeof(std::pair<std::string, std::string>), &vector_bytes) ||
      !CheckedTimeSeriesAdd(vector_bytes, &working_bytes) ||
      !CheckedTimeSeriesAdd(static_cast<std::uint64_t>(input.size()),
                            &working_bytes) ||
      !CheckedTimeSeriesAdd(static_cast<std::uint64_t>(input.size()) + 1,
                            &working_bytes) ||
      working_bytes > maximum_memory_bytes) {
    return TagParseStatus::kResourceRefused;
  }
  *working_memory_bytes = working_bytes;
  std::size_t offset = 1;
  std::vector<std::pair<std::string, std::string>> tags;
  tags.reserve(static_cast<std::size_t>(maximum_tag_count));
  bool cancellation_observed = false;
  if (offset < input.size() && input[offset] == '}') {
    ++offset;
  } else {
    while (offset < input.size()) {
      std::string key;
      std::string value;
      if (!ParseJsonString(input, &offset, cancellation_requested,
                           &cancellation_observed, &key) ||
          cancellation_observed || key.empty() ||
          offset >= input.size() || input[offset++] != ':' ||
          !ParseJsonString(input, &offset, cancellation_requested,
                           &cancellation_observed, &value)) {
        if (cancellation_observed) return TagParseStatus::kCancelled;
        return TagParseStatus::kInvalid;
      }
      tags.push_back({std::move(key), std::move(value)});
      if (offset >= input.size()) return TagParseStatus::kInvalid;
      if (input[offset] == '}') {
        ++offset;
        break;
      }
      if (input[offset++] != ',') return TagParseStatus::kInvalid;
    }
  }
  if (offset != input.size()) return TagParseStatus::kInvalid;
  bool sort_cancelled = false;
  std::ranges::sort(tags, [&](const auto& left, const auto& right) {
    try {
      if (!sort_cancelled && cancellation_requested &&
          cancellation_requested()) {
        sort_cancelled = true;
      }
    } catch (...) {
      sort_cancelled = true;
    }
    return UnsignedTextLess(left.first, right.first);
  });
  if (sort_cancelled) return TagParseStatus::kCancelled;
  for (std::size_t index = 1; index < tags.size(); ++index) {
    try {
      if (cancellation_requested && cancellation_requested()) {
        return TagParseStatus::kCancelled;
      }
    } catch (...) {
      return TagParseStatus::kCancelled;
    }
    if (tags[index - 1].first == tags[index].first) {
      return TagParseStatus::kDuplicate;
    }
  }
  canonical->clear();
  canonical->reserve(input.size());
  canonical->push_back('{');
  for (std::size_t index = 0; index < tags.size(); ++index) {
    try {
      if (cancellation_requested && cancellation_requested()) {
        canonical->clear();
        return TagParseStatus::kCancelled;
      }
    } catch (...) {
      canonical->clear();
      return TagParseStatus::kCancelled;
    }
    if (index != 0) canonical->push_back(',');
    if (!AppendJsonEscape(tags[index].first, cancellation_requested,
                          canonical)) {
      canonical->clear();
      return TagParseStatus::kCancelled;
    }
    canonical->push_back(':');
    if (!AppendJsonEscape(tags[index].second, cancellation_requested,
                          canonical)) {
      canonical->clear();
      return TagParseStatus::kCancelled;
    }
  }
  canonical->push_back('}');
  if (canonical->size() > maximum_tag_bytes) {
    canonical->clear();
    return TagParseStatus::kResourceRefused;
  }
  return TagParseStatus::kOk;
}

bool ExactTimeSeriesValueDescriptor(
    const EngineDescriptor& descriptor, const std::string_view expected_type,
    const std::string_view expected_type_uuid,
    const std::string_view expected_column_uuid,
    const scratchbird::core::datatypes::DatatypeTypeCodecIdentityRowV1*
        expected_registry_identity) {
  if (!QowCanonicalDescriptorIdentityV1(descriptor) ||
      descriptor.descriptor_kind != "canonical_type_descriptor" ||
      descriptor.canonical_type_name != expected_type) {
    return false;
  }
  std::map<std::string_view, std::string_view> fields;
  const auto encoded = std::string_view(descriptor.encoded_descriptor);
  std::size_t offset = 0;
  while (offset <= encoded.size()) {
    const auto end = encoded.find(';', offset);
    const auto field = encoded.substr(
        offset, end == std::string_view::npos ? std::string_view::npos
                                              : end - offset);
    const auto equal = field.find('=');
    if (field.empty() || equal == std::string_view::npos || equal == 0 ||
        equal + 1 == field.size() ||
        !fields.emplace(field.substr(0, equal), field.substr(equal + 1)).second) {
      return false;
    }
    if (end == std::string_view::npos) break;
    offset = end + 1;
  }
  const bool contextual_text = expected_type == "text";
  if (fields.size() != (contextual_text ? 12U :
                       expected_type == "timestamp_tz" ? 3U : 3U) &&
      !(expected_type == "timestamp_tz" && fields.size() == 4U)) {
    return false;
  }
  if (
      !fields.contains("canonical") || !fields.contains("type_uuid") ||
      !fields.contains("nullable") ||
      fields.at("canonical") != expected_type ||
      fields.at("type_uuid") != expected_type_uuid ||
      fields.at("nullable") != "false") {
    return false;
  }
  const auto timezone = fields.find("timezone_profile_id");
  const auto column_uuid = fields.find("column_uuid");
  if (expected_type == "timestamp_tz") {
    return column_uuid == fields.end() && (fields.size() == 3 ||
           (fields.size() == 4 && timezone != fields.end() &&
            timezone->second == "UTC"));
  }
  if (expected_type == "text") {
    return expected_registry_identity != nullptr && timezone == fields.end() &&
           column_uuid != fields.end() &&
           column_uuid->second == expected_column_uuid &&
           descriptor.descriptor_uuid.canonical ==
               expected_column_uuid &&
           fields.contains("datatype_descriptor_uuid") &&
           fields.at("datatype_descriptor_uuid") ==
               expected_registry_identity->descriptor_uuid &&
           fields.contains("datatype_descriptor_generation") &&
           fields.at("datatype_descriptor_generation") ==
               std::to_string(
                   expected_registry_identity->descriptor_generation) &&
           fields.contains("type_generation") &&
           fields.at("type_generation") ==
               std::to_string(expected_registry_identity->type_generation) &&
           fields.contains("codec_uuid") &&
           fields.at("codec_uuid") == expected_registry_identity->codec_uuid &&
           fields.contains("codec_id") &&
           fields.at("codec_id") == expected_registry_identity->codec_id &&
           fields.contains("codec_version") &&
           fields.at("codec_version") ==
               std::to_string(expected_registry_identity->codec_version) &&
           fields.contains("codec_generation") &&
           fields.at("codec_generation") ==
               std::to_string(expected_registry_identity->codec_generation) &&
           fields.contains("null_encoding") &&
           fields.at("null_encoding") ==
               std::to_string(expected_registry_identity->null_encoding_code);
  }
  return fields.size() == 3 && timezone == fields.end() &&
         column_uuid == fields.end();
}

bool ExactTimeSeriesStorageDescriptorImpl(
    const MgaRelationStorageDescriptor& descriptor) {
  static constexpr std::array<std::string_view, 4> kNames{
      "metric_uuid", "point_timestamp", "tags", "value"};
  static constexpr std::array<std::string_view, 4> kTypes{
      "uuid", "timestamp_tz", "text", "real64"};
  if (descriptor.columns.size() != kNames.size()) return false;
  const auto manifest =
      scratchbird::core::datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return false;
  std::unordered_set<std::string> column_uuids;
  std::unordered_set<std::string> descriptor_uuids;
  for (std::size_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
    const auto& column = descriptor.columns[ordinal];
    const auto type_id =
        kTypes[ordinal] == "timestamp_tz"
            ? scratchbird::core::datatypes::CanonicalTypeId::timestamp
            : scratchbird::core::datatypes::CanonicalTypeIdFromStableName(
                  std::string(kTypes[ordinal]));
    const auto type_row = scratchbird::core::datatypes::LookupDatatypeCatalogRow(
        manifest.manifest, type_id);
    if (!type_row.ok() || type_row.manifest.descriptor_rows.size() != 1 ||
        !type_row.manifest.descriptor_rows.front().descriptor_uuid.valid()) {
      return false;
    }
    const auto& descriptor_row =
        type_row.manifest.descriptor_rows.front();
    const auto descriptor_uuid = scratchbird::core::uuid::UuidToString(
        descriptor_row.descriptor_uuid.value);
    const auto codec_identity =
        scratchbird::core::datatypes::LookupDatatypeTypeCodecIdentityV1(
            "019d0000-0000-7000-8000-00000000d701",
            manifest.manifest.catalog_epoch, 1, descriptor_uuid,
            descriptor_row.descriptor_epoch);
    const auto expected_type_uuid =
        codec_identity.ok ? codec_identity.row.type_uuid : descriptor_uuid;
    const auto* expected_registry_identity =
        codec_identity.ok ? &codec_identity.row : nullptr;
    if (kTypes[ordinal] == "text" && expected_registry_identity == nullptr) {
      return false;
    }
    if (column.ordinal != ordinal ||
        column.canonical_name_key != kNames[ordinal] || column.nullable ||
        column.generated || column.identity_column ||
        column.storage_class != "inline_row_value" ||
        column.max_inline_bytes != 4096 ||
        column.overflow_policy != "mga_large_value_locator" ||
        column.value_descriptor.canonical_type_name != kTypes[ordinal] ||
        !CanonicalTimeSeriesUuid(column.column_uuid.canonical) ||
        !CanonicalTimeSeriesUuid(
            column.value_descriptor.descriptor_uuid.canonical) ||
        !column_uuids.insert(column.column_uuid.canonical).second ||
        !descriptor_uuids
             .insert(column.value_descriptor.descriptor_uuid.canonical)
             .second ||
        !column.charset_uuid.empty() || !column.collation_uuid.empty() ||
        column.character_length != 0 ||
        !ExactTimeSeriesValueDescriptor(column.value_descriptor,
                                        kTypes[ordinal],
                                        expected_type_uuid,
                                        column.column_uuid.canonical,
                                        expected_registry_identity)) {
      return false;
    }
  }
  return true;
}

bool ParseFiniteReal64(const std::string_view encoded, double* out) {
  if (out == nullptr || encoded.empty()) return false;
  double value = 0.0;
  const auto parsed = std::from_chars(encoded.data(),
                                      encoded.data() + encoded.size(), value,
                                      std::chars_format::general);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != encoded.data() + encoded.size() || !std::isfinite(value)) {
    return false;
  }
  *out = value == 0.0 ? 0.0 : value;
  return true;
}

bool ExactBucketStart(const EngineApiI64 timestamp_ns,
                      const EngineApiI64 interval_ns,
                      EngineApiI64* start_ns,
                      EngineApiI64* end_ns) {
  if (start_ns == nullptr || end_ns == nullptr || interval_ns <= 0) {
    return false;
  }
  EngineApiI64 quotient = timestamp_ns / interval_ns;
  const EngineApiI64 remainder = timestamp_ns % interval_ns;
  if (remainder < 0) --quotient;
  const __int128 start =
      static_cast<__int128>(quotient) * interval_ns;
  const __int128 end = start + interval_ns;
  if (start < std::numeric_limits<EngineApiI64>::min() ||
      start > std::numeric_limits<EngineApiI64>::max() ||
      end < std::numeric_limits<EngineApiI64>::min() ||
      end > std::numeric_limits<EngineApiI64>::max()) {
    return false;
  }
  *start_ns = static_cast<EngineApiI64>(start);
  *end_ns = static_cast<EngineApiI64>(end);
  return true;
}

MgaVisibleHeapRelationReadResult ReadPreferredTimeSeriesRawStoreV1(
    const EngineRequestContext& context,
    const MgaVisibleHeapRelationReadRequest& request) {
  return ReadVisibleMgaHeapRelation(context, request);
}

MgaVisibleHeapRelationReadResult ReadExactTimeSeriesBucketStoreFallbackV1(
    const EngineRequestContext& context,
    const MgaVisibleHeapRelationReadRequest& request) {
  // The exact bucket-store fallback owns a separate provider branch and
  // receipt, but it deliberately reuses the engine MGA heap reader so the
  // same statement snapshot, security cohort, and bounded raw-row authority
  // govern reconstruction of raw and aggregate outputs.
  return ReadVisibleMgaHeapRelation(context, request);
}

std::optional<std::uint64_t> BoundTimeSeriesResultLogicalMemoryBytesV1(
    const EngineBoundTimeSeriesReadResultV1& result) {
  std::uint64_t bytes = sizeof(result);
  const auto account = [&](const std::uint64_t amount) {
    return CheckedTimeSeriesAdd(amount, &bytes);
  };
  const auto account_string = [&](const std::string& value) {
    return CheckedTimeSeriesOwnedStringBytes(value, &bytes);
  };
  const auto account_array = [&](const std::size_t count,
                                 const std::uint64_t width) {
    std::uint64_t amount = 0;
    return CheckedTimeSeriesMultiply(count, width, &amount) &&
           account(amount);
  };
  if (!account_string(result.operation_id) ||
      !account_string(result.selected_access_path_id) ||
      !account_string(result.ordering_id) ||
      !account_string(result.descriptor_uuid) ||
      !account_string(result.selected_alternative_uuid) ||
      !account_string(result.capability_uuid) ||
      !account_string(result.provider_uuid) ||
      !account_array(result.diagnostics.capacity(),
                     sizeof(EngineApiDiagnostic)) ||
      !account_array(result.evidence.capacity(),
                     sizeof(EngineEvidenceReference)) ||
      !account_array(result.rows.capacity(),
                     sizeof(EngineBoundTimeSeriesPointRowV1)) ||
      !account_array(result.downsample_rows.capacity(),
                     sizeof(EngineBoundTimeSeriesDownsampleRowV1))) {
    return std::nullopt;
  }
  for (const auto& diagnostic : result.diagnostics) {
    if (!AccountTimeSeriesDiagnosticMemoryV1(diagnostic, &bytes)) {
      return std::nullopt;
    }
  }
  for (const auto& evidence : result.evidence) {
    if (!account_string(evidence.evidence_kind) ||
        !account_string(evidence.evidence_id)) {
      return std::nullopt;
    }
  }
  for (const auto& row : result.rows) {
    if (!account_string(row.row_uuid) || !account_string(row.series_uuid) ||
        !account_string(row.metric_uuid) ||
        !account_string(row.point_timestamp) || !account_string(row.tags)) {
      return std::nullopt;
    }
  }
  for (const auto& row : result.downsample_rows) {
    if (!account_string(row.series_uuid) ||
        !account_string(row.metric_uuid) ||
        !account_string(row.bucket_start) ||
        !account_string(row.bucket_end) || !account_string(row.tags)) {
      return std::nullopt;
    }
  }
  return bytes;
}

}  // namespace

bool ExactTimeSeriesStorageDescriptorV1(
    const MgaRelationStorageDescriptor& descriptor) {
  return ExactTimeSeriesStorageDescriptorImpl(descriptor);
}

bool EngineExactTimeSeriesBucketStartV1(
    const EngineApiI64 timestamp_ns, const EngineApiI64 interval_ns,
    EngineApiI64* bucket_start_ns, std::string* bucket_start) {
  if (bucket_start_ns == nullptr || bucket_start == nullptr) return false;
  EngineApiI64 bucket_end_ns = 0;
  if (!ExactBucketStart(timestamp_ns, interval_ns, bucket_start_ns,
                        &bucket_end_ns)) {
    return false;
  }
  *bucket_start = FormatTimeSeriesTimestamp(*bucket_start_ns);
  return !bucket_start->empty();
}

static EngineBoundTimeSeriesReadResultV1 EngineBoundTimeSeriesReadV1Impl(
    const EngineBoundTimeSeriesReadRequestV1& request) {
  bool data_access_observed = false;
  bool cancellation_observed = false;
  bool cancellation_probe_failed = false;
  std::uint64_t api_peak_live_memory_bytes = 0;
  std::uint64_t preferred_access_invocation_count = 0;
  std::uint64_t exact_fallback_access_invocation_count = 0;
  std::string selected_access_path_id;
  const auto refuse = [&](const char* diagnostic, std::string detail) {
    const bool probe_failure = cancellation_probe_failed;
    auto result = BoundTimeSeriesRefusal(
        request,
        probe_failure &&
                std::string_view(diagnostic) ==
                    "SB_MODEL_EXECUTION_CANCELLED_V1"
            ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
            : diagnostic,
        probe_failure &&
                std::string_view(diagnostic) ==
                    "SB_MODEL_EXECUTION_CANCELLED_V1"
            ? "time-series cancellation probe failed"
            : std::move(detail));
    result.data_access_observed = data_access_observed;
    result.cancellation_observed = cancellation_observed;
    result.cancellation_probe_failed = cancellation_probe_failed;
    result.peak_live_memory_bytes = api_peak_live_memory_bytes;
    result.memory_grant_bytes = request.maximum_memory_bytes;
    result.preferred_access_invocation_count =
        preferred_access_invocation_count;
    result.exact_fallback_access_invocation_count =
        exact_fallback_access_invocation_count;
    result.selected_access_path_id = selected_access_path_id;
    return result;
  };
  const auto cancelled = [&]() noexcept {
    if (!request.cancellation_requested) return false;
    try {
      if (!request.cancellation_requested()) return false;
      cancellation_observed = true;
      return true;
    } catch (...) {
      cancellation_probe_failed = true;
      return true;
    }
  };
  try {
  const bool range_read =
      request.operation == EngineBoundTimeSeriesReadOperationV1::kRangeRead;
  const bool downsample = request.operation ==
                          EngineBoundTimeSeriesReadOperationV1::kBucketDownsample;
  // Establish the aggregate floating-point environment before decoding any
  // stored REAL64 payload. Restoring only around the grouping loop would
  // preserve exceptions raised by pre-group decoding instead of restoring the
  // caller's exact environment.
  ScopedTimeSeriesNearestRounding rounding(downsample);
  if (downsample && !rounding.active()) {
    return refuse("SB_MODEL_TIME_SERIES_VALUE_INVALID_V1",
                  "time-series aggregate could not establish round-to-nearest-ties-to-even");
  }
  if (request.abi_version != 1 || (!range_read && !downsample) ||
      !CanonicalTimeSeriesUuid(request.object_uuid) ||
      !CanonicalTimeSeriesUuid(request.expected_descriptor_uuid) ||
      request.expected_descriptor_generation == 0 ||
      !CanonicalTimeSeriesUuid(request.selected_alternative_uuid) ||
      !CanonicalTimeSeriesUuid(request.capability_uuid) ||
      !CanonicalTimeSeriesUuid(request.provider_uuid) ||
      request.provider_generation == 0 || !request.cancellation_requested) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "engine-bound time-series request identity is incomplete");
  }
  if (request.range_start.state != EngineValueState::value ||
      request.range_start.is_null || request.range_end.state != EngineValueState::value ||
      request.range_end.is_null) {
    return refuse("SB_MODEL_TIME_SERIES_RANGE_INVALID_V1",
                  "time-series range endpoints are absent or null");
  }
  if (!request.range_start.binary_value.empty() ||
      request.range_start.descriptor.canonical_type_name != "timestamp_tz" ||
      !request.range_end.binary_value.empty() ||
      request.range_end.descriptor.canonical_type_name != "timestamp_tz") {
    return refuse("SB_MODEL_TIME_SERIES_TIMESTAMP_INVALID_V1",
                  "time-series range endpoints are not non-null TIMESTAMP_TZ");
  }
  EngineApiI64 range_start_ns = 0;
  EngineApiI64 range_end_ns = 0;
  if (!ParseTimeSeriesTimestamp(request.range_start.encoded_value,
                                &range_start_ns) ||
      !ParseTimeSeriesTimestamp(request.range_end.encoded_value,
                                &range_end_ns)) {
    return refuse("SB_MODEL_TIME_SERIES_TIMESTAMP_INVALID_V1",
                  "time-series range endpoint is malformed or out of range");
  }
  if (range_start_ns > range_end_ns) {
    return refuse("SB_MODEL_TIME_SERIES_RANGE_INVALID_V1",
                  "time-series range start exceeds its end");
  }
  const bool valid_aggregate =
      request.aggregate >= EngineBoundTimeSeriesAggregateV1::kCount &&
      request.aggregate <= EngineBoundTimeSeriesAggregateV1::kAvg;
  if (range_read && (request.aggregate != EngineBoundTimeSeriesAggregateV1::kNone ||
                     request.bucket_interval_ns != 0)) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "time-series range operation has aggregate operands");
  }
  if (downsample && request.bucket_interval_ns <= 0) {
    return refuse("SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1",
                  "time-series fixed bucket interval is nonpositive");
  }
  if (downsample && !valid_aggregate) {
    return refuse("SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1",
                  "time-series aggregate identity is outside the closed set");
  }
  if (request.maximum_scanned_row_versions == 0 ||
      request.maximum_decoded_bytes == 0 ||
      request.maximum_output_rows == 0 || request.maximum_groups == 0 ||
      request.maximum_tag_bytes == 0 || request.maximum_result_bytes == 0 ||
      request.maximum_memory_bytes == 0) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "time-series resource contract is incomplete");
  }
  if (request.rollup_candidate_selected &&
      !request.exact_fallback_selected) {
    return refuse("SB_MODEL_TIME_SERIES_ROLLUP_EQUIVALENCE_REFUSED_V1",
                  "time-series rollup store equivalence is not available");
  }
  if (cancelled()) {
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "time-series execution was cancelled before data access");
  }
  const auto authorization = EvaluateMaterializedAuthorization(
      request.context, request.context.authorization_context, "SELECT",
      request.object_uuid);
  if (!authorization.authorized || authorization.denied ||
      authorization.policy_recheck_required ||
      !authorization.diagnostics.empty()) {
    return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                  "time-series SELECT authorization was refused");
  }

  // Descriptor generation and the exact storage shape are catalog authority,
  // not row-access observations. Refuse a stale/substituted binding before the
  // provider is allowed to inspect an MGA heap version.
  {
    const auto current_descriptor =
        LoadMgaRelationStorageDescriptor(request.context,
                                         request.object_uuid);
    if (!current_descriptor.ok ||
        current_descriptor.descriptor.descriptor_uuid.canonical !=
            request.expected_descriptor_uuid ||
        current_descriptor.descriptor.descriptor_generation !=
            request.expected_descriptor_generation) {
      return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                    "time-series relation descriptor generation changed");
    }
    if (!ExactTimeSeriesStorageDescriptorV1(current_descriptor.descriptor)) {
      return refuse(
          "SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
          "time-series storage descriptor is not the exact four-field raw layout");
    }
  }

  MgaVisibleHeapRelationReadRequest read_request;
  read_request.relation_uuid = request.object_uuid;
  read_request.maximum_scanned_row_versions =
      request.maximum_scanned_row_versions;
  read_request.maximum_decoded_bytes = request.maximum_decoded_bytes;
  // The MGA reader's output is the total visible raw relation, before the
  // time-range predicate. Bound that carrier by the scanned-version cohort;
  // the requested output-row bound is enforced only after [start,end)
  // selection and grouping below.
  read_request.maximum_output_rows = request.maximum_scanned_row_versions;
  read_request.maximum_memory_bytes = request.maximum_memory_bytes;
  read_request.cancellation_requested = cancelled;
  data_access_observed = true;
  MgaVisibleHeapRelationReadResult read;
  if (request.exact_fallback_selected) {
    selected_access_path_id = "TIME_SERIES_BUCKET_STORE_SCAN_EXACT_V1";
    ++exact_fallback_access_invocation_count;
    read = ReadExactTimeSeriesBucketStoreFallbackV1(request.context,
                                                    read_request);
  } else {
    selected_access_path_id = "time_series.local.v1";
    ++preferred_access_invocation_count;
    read = ReadPreferredTimeSeriesRawStoreV1(request.context, read_request);
  }
  api_peak_live_memory_bytes = read.peak_live_memory_bytes;
  if (!read.ok) {
    if (read.cancellation_observed || cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "time-series MGA-visible row read was cancelled");
    }
    const auto detail = read.diagnostic.detail.empty()
                            ? std::string("bounded time-series MGA row read failed")
                            : read.diagnostic.detail;
    const char* diagnostic = "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
    switch (read.failure_category) {
      case MgaHeapReadFailureCategoryV1::kResource:
        diagnostic = "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
        break;
      case MgaHeapReadFailureCategoryV1::kCancellation:
        diagnostic = "SB_MODEL_EXECUTION_CANCELLED_V1";
        break;
      case MgaHeapReadFailureCategoryV1::kMgaContext:
        diagnostic = "SB_MODEL_MGA_CONTEXT_MISMATCH_V1";
        break;
      case MgaHeapReadFailureCategoryV1::kCatalog:
        diagnostic = "SB_MODEL_CATALOG_GENERATION_STALE_V1";
        break;
      case MgaHeapReadFailureCategoryV1::kCorruptStorage:
        diagnostic = "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
        break;
      case MgaHeapReadFailureCategoryV1::kInvalidRequest:
        diagnostic = "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1";
        break;
      case MgaHeapReadFailureCategoryV1::kStorage:
      case MgaHeapReadFailureCategoryV1::kNone:
        break;
    }
    return refuse(diagnostic, detail);
  }
  if (!read.memory_receipt_complete ||
      read.memory_grant_bytes != request.maximum_memory_bytes ||
      read.current_live_memory_bytes > read.peak_live_memory_bytes ||
      read.peak_live_memory_bytes > read.memory_grant_bytes) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "time-series MGA read memory receipt is incomplete");
  }

  const auto read_carrier_memory =
      TimeSeriesMgaReadCarrierMemoryBytesV1(read);
  std::uint64_t working_memory_bytes =
      sizeof(EngineBoundTimeSeriesReadResultV1);
  api_peak_live_memory_bytes =
      std::max(api_peak_live_memory_bytes, working_memory_bytes);
  std::uint64_t retained_vector_bytes = 0;
  const auto account_working = [&](const std::uint64_t bytes) {
    const bool accounted =
        AccountTimeSeriesMemory(bytes, request.maximum_memory_bytes,
                                &working_memory_bytes);
    if (accounted) {
      api_peak_live_memory_bytes =
          std::max(api_peak_live_memory_bytes, working_memory_bytes);
    }
    return accounted;
  };
  const auto account_working_string = [&](const std::string& value) {
    std::uint64_t bytes = 0;
    return CheckedTimeSeriesOwnedStringBytes(value, &bytes) &&
           account_working(bytes);
  };
  if (!read_carrier_memory.has_value() ||
      !account_working(*read_carrier_memory) ||
      !account_working_string(selected_access_path_id) ||
      !account_working_string(read.descriptor.descriptor_uuid.canonical) ||
      !account_working_string(request.selected_alternative_uuid) ||
      !account_working_string(request.capability_uuid) ||
      !account_working_string(request.provider_uuid) ||
      !account_working(4096)) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "time-series retained read/result preflight exceeded its memory bound");
  }

  EngineBoundTimeSeriesReadResultV1 result =
      MakeApiBehaviorSuccess<EngineBoundTimeSeriesReadResultV1>(
          request.context, kBoundTimeSeriesOperation);
  result.data_access_observed = true;
  result.exact_fallback_observed =
      exact_fallback_access_invocation_count == 1;
  result.rollup_observed = false;
  result.rollup_equivalence_recheck_complete =
      !request.rollup_candidate_selected || result.exact_fallback_observed;
  result.preferred_access_invocation_count =
      preferred_access_invocation_count;
  result.exact_fallback_access_invocation_count =
      exact_fallback_access_invocation_count;
  result.selected_access_path_id = selected_access_path_id;
  result.scanned_row_version_count = read.scanned_row_version_count;
  result.descriptor_uuid = read.descriptor.descriptor_uuid.canonical;
  result.descriptor_generation = read.descriptor.descriptor_generation;
  result.selected_alternative_uuid = request.selected_alternative_uuid;
  result.capability_uuid = request.capability_uuid;
  result.provider_uuid = request.provider_uuid;
  result.provider_generation = request.provider_generation;
  if (result.descriptor_uuid != request.expected_descriptor_uuid ||
      result.descriptor_generation !=
          request.expected_descriptor_generation) {
    return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                  "time-series relation descriptor generation changed");
  }

  std::vector<EngineBoundTimeSeriesPointRowV1> selected;
  const std::size_t selected_capacity =
      downsample
          ? read.visible_rows.size()
          : std::min(read.visible_rows.size(), request.maximum_output_rows);
  if (!CheckedTimeSeriesMultiply(
          static_cast<std::uint64_t>(selected_capacity),
          sizeof(EngineBoundTimeSeriesPointRowV1), &retained_vector_bytes) ||
      !account_working(retained_vector_bytes)) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "time-series selected-row vector exceeded its memory bound");
  }
  selected.reserve(selected_capacity);
  std::uint64_t tag_bytes = 0;
  for (const auto& row : read.visible_rows) {
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "time-series visible-row validation was cancelled");
    }
    if (!CanonicalTimeSeriesUuid(row.row_uuid) ||
        row.values.size() != read.descriptor.columns.size()) {
      return refuse("SB_MODEL_TIME_SERIES_IDENTITY_INVALID_V1",
                    "time-series row identity or width is invalid");
    }
    const std::string* metric = nullptr;
    const std::string* timestamp = nullptr;
    const std::string* tags = nullptr;
    const std::string* stored_value = nullptr;
    for (const auto& [name, field_value] : row.values) {
      std::string const** destination = nullptr;
      if (name == "metric_uuid") destination = &metric;
      else if (name == "point_timestamp") destination = &timestamp;
      else if (name == "tags") destination = &tags;
      else if (name == "value") destination = &stored_value;
      if (destination == nullptr || *destination != nullptr) {
        return refuse("SB_MODEL_TIME_SERIES_VALUE_INVALID_V1",
                      "time-series stored row has an unknown or duplicated field");
      }
      *destination = &field_value;
    }
    if (metric == nullptr || timestamp == nullptr || tags == nullptr ||
        stored_value == nullptr || *metric == "<NULL>" ||
        *timestamp == "<NULL>" || *tags == "<NULL>" ||
        *stored_value == "<NULL>" ||
        !CanonicalTimeSeriesUuid(*metric)) {
      return refuse("SB_MODEL_TIME_SERIES_IDENTITY_INVALID_V1",
                    "time-series selected row has null or invalid identity fields");
    }
    EngineApiI64 point_ns = 0;
    if (!ParseTimeSeriesTimestamp(*timestamp, &point_ns)) {
      return refuse("SB_MODEL_TIME_SERIES_TIMESTAMP_INVALID_V1",
                    "time-series selected point timestamp is invalid");
    }
    std::string canonical_tags;
    const auto remaining_tag_bytes =
        tag_bytes <= request.maximum_tag_bytes
            ? request.maximum_tag_bytes - tag_bytes
            : 0;
    const auto remaining_memory_bytes =
        working_memory_bytes <= request.maximum_memory_bytes
            ? request.maximum_memory_bytes - working_memory_bytes
            : 0;
    std::uint64_t tag_working_memory_bytes = 0;
    const auto tag_status =
        CanonicalizeTimeSeriesTags(*tags, remaining_tag_bytes,
                                   remaining_memory_bytes, cancelled,
                                   &canonical_tags,
                                   &tag_working_memory_bytes);
    if (tag_status == TagParseStatus::kCancelled) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "time-series tag canonicalization was cancelled");
    }
    if (tag_status == TagParseStatus::kResourceRefused) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "time-series tag canonicalization exceeded its memory bound");
    }
    if (tag_status == TagParseStatus::kDuplicate) {
      return refuse("SB_MODEL_TIME_SERIES_DUPLICATE_TAG_REFUSED_V1",
                    "time-series selected point contains a duplicate tag key");
    }
    if (tag_status != TagParseStatus::kOk) {
      return refuse("SB_MODEL_TIME_SERIES_TAG_INVALID_V1",
                    "time-series selected point tags are invalid");
    }
    std::uint64_t tag_phase_memory_bytes = working_memory_bytes;
    if (!CheckedTimeSeriesAdd(tag_working_memory_bytes,
                              &tag_phase_memory_bytes)) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "time-series tag working-memory receipt overflowed");
    }
    api_peak_live_memory_bytes =
        std::max(api_peak_live_memory_bytes, tag_phase_memory_bytes);
    double numeric_value = 0.0;
    if (!ParseFiniteReal64(*stored_value, &numeric_value)) {
      return refuse("SB_MODEL_TIME_SERIES_VALUE_INVALID_V1",
                    "time-series selected point value is not finite REAL64");
    }
    if (!CheckedTimeSeriesAdd(canonical_tags.size(), &tag_bytes) ||
        tag_bytes > request.maximum_tag_bytes) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "time-series tag bytes exceeded their bound");
    }
    if (point_ns >= range_start_ns && point_ns < range_end_ns) {
      if (range_read && selected.size() >= request.maximum_output_rows) {
        return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "time-series raw output row count exceeded its bound");
      }
      std::uint64_t retained_strings = 0;
      if (!CheckedTimeSeriesOwnedStringBytes(row.row_uuid,
                                             &retained_strings) ||
          !CheckedTimeSeriesOwnedStringBytes(request.object_uuid,
                                             &retained_strings) ||
          !CheckedTimeSeriesOwnedStringBytes(*metric, &retained_strings) ||
          !CheckedTimeSeriesAdd(31, &retained_strings) ||
          !CheckedTimeSeriesOwnedStringBytes(canonical_tags,
                                             &retained_strings) ||
          !account_working(retained_strings)) {
        return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "time-series selected-row retained memory exceeded its bound");
      }
      selected.push_back({row.row_uuid,
                          request.object_uuid,
                          *metric,
                          point_ns,
                          FormatTimeSeriesTimestamp(point_ns),
                          std::move(canonical_tags),
                          numeric_value});
    }
  }
  result.selected_visible_row_count = selected.size();
  bool row_sort_cancelled = false;
  std::ranges::sort(selected, [&](const auto& left, const auto& right) {
    if (!row_sort_cancelled && cancelled()) row_sort_cancelled = true;
    if (left.series_uuid != right.series_uuid) {
      return UnsignedTextLess(left.series_uuid, right.series_uuid);
    }
    if (left.metric_uuid != right.metric_uuid) {
      return UnsignedTextLess(left.metric_uuid, right.metric_uuid);
    }
    if (left.point_timestamp_ns != right.point_timestamp_ns) {
      return left.point_timestamp_ns < right.point_timestamp_ns;
    }
    if (left.tags != right.tags) {
      return UnsignedTextLess(left.tags, right.tags);
    }
    return UnsignedTextLess(left.row_uuid, right.row_uuid);
  });
  if (row_sort_cancelled) {
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "time-series row ordering sort was cancelled");
  }

  std::uint64_t result_bytes = 0;
  if (range_read) {
    result.ordering_id = "series_metric_timestamp_tags_row_ascending_v1";
    if (selected.size() > request.maximum_output_rows) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "time-series raw output row count exceeded its bound");
    }
    for (const auto& row : selected) {
      const std::uint64_t bytes = row.row_uuid.size() + row.series_uuid.size() +
                                  row.metric_uuid.size() +
                                  row.point_timestamp.size() + row.tags.size() +
                                  sizeof(row.value) + 6;
      if (!CheckedTimeSeriesAdd(bytes, &result_bytes) ||
          result_bytes > request.maximum_result_bytes) {
        return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "time-series raw result preflight exceeded its bound");
      }
    }
    result.rows = std::move(selected);
  } else {
    struct GroupState {
      std::string series_uuid;
      std::string metric_uuid;
      EngineApiI64 bucket_start_ns{0};
      EngineApiI64 bucket_end_ns{0};
      std::string tags;
      EngineApiI64 count{0};
      double sum{0.0};
      double minimum{0.0};
      double maximum{0.0};
      bool initialized{false};
    };
    using GroupKey =
        std::tuple<std::string, std::string, std::string, EngineApiI64>;
    struct GroupKeyLess {
      bool operator()(const GroupKey& left, const GroupKey& right) const {
        if (std::get<0>(left) != std::get<0>(right)) {
          return UnsignedTextLess(std::get<0>(left), std::get<0>(right));
        }
        if (std::get<1>(left) != std::get<1>(right)) {
          return UnsignedTextLess(std::get<1>(left), std::get<1>(right));
        }
        if (std::get<2>(left) != std::get<2>(right)) {
          return UnsignedTextLess(std::get<2>(left), std::get<2>(right));
        }
        return std::get<3>(left) < std::get<3>(right);
      }
    };
    std::map<GroupKey, GroupState, GroupKeyLess> groups;
    for (const auto& row : selected) {
      if (cancelled()) {
        return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                      "time-series downsample was cancelled");
      }
      EngineApiI64 bucket_start_ns = 0;
      EngineApiI64 bucket_end_ns = 0;
      if (!ExactBucketStart(row.point_timestamp_ns,
                            request.bucket_interval_ns, &bucket_start_ns,
                            &bucket_end_ns)) {
        return refuse("SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1",
                      "time-series bucket boundary overflowed");
      }
      std::uint64_t group_lookup_projection = working_memory_bytes;
      if (!CheckedTimeSeriesAdd(sizeof(GroupKey),
                                &group_lookup_projection) ||
          !CheckedTimeSeriesOwnedStringBytes(row.series_uuid,
                                             &group_lookup_projection) ||
          !CheckedTimeSeriesOwnedStringBytes(row.metric_uuid,
                                             &group_lookup_projection) ||
          !CheckedTimeSeriesOwnedStringBytes(row.tags,
                                             &group_lookup_projection) ||
          group_lookup_projection > request.maximum_memory_bytes) {
        return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "time-series downsample lookup key exceeded its memory bound");
      }
      api_peak_live_memory_bytes =
          std::max(api_peak_live_memory_bytes, group_lookup_projection);
      GroupKey group_key{row.series_uuid, row.metric_uuid, row.tags,
                         bucket_start_ns};
      auto group_entry = groups.find(group_key);
      if (cancelled()) {
        return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                      "time-series downsample group search was cancelled");
      }
      if (group_entry == groups.end()) {
        if (groups.size() >= request.maximum_groups ||
            groups.size() >= request.maximum_output_rows) {
          return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                        "time-series group count exceeded its bound");
        }
        std::uint64_t group_memory =
            sizeof(GroupKey) + sizeof(GroupState) + 5 * sizeof(void*);
        const auto duplicated_string_bytes = [&]() {
          std::uint64_t bytes = 0;
          return CheckedTimeSeriesOwnedStringBytes(row.series_uuid, &bytes) &&
                         CheckedTimeSeriesOwnedStringBytes(row.metric_uuid,
                                                           &bytes) &&
                         CheckedTimeSeriesOwnedStringBytes(row.tags, &bytes)
                     ? std::optional<std::uint64_t>(bytes)
                     : std::nullopt;
        }();
        std::uint64_t dynamic_bytes = 0;
        if (!duplicated_string_bytes.has_value() ||
            !CheckedTimeSeriesMultiply(2, *duplicated_string_bytes,
                                       &dynamic_bytes) ||
            !CheckedTimeSeriesAdd(dynamic_bytes, &group_memory) ||
            !account_working(group_memory)) {
          return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                        "time-series downsample working group memory exceeded its bound");
        }
        group_entry =
            groups.emplace(std::move(group_key), GroupState{}).first;
      }
      auto& group = group_entry->second;
      if (!group.initialized) {
        group.series_uuid = row.series_uuid;
        group.metric_uuid = row.metric_uuid;
        group.bucket_start_ns = bucket_start_ns;
        group.bucket_end_ns = bucket_end_ns;
        group.tags = row.tags;
        group.minimum = row.value;
        group.maximum = row.value;
        group.initialized = true;
      }
      if (group.count == std::numeric_limits<EngineApiI64>::max()) {
        return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "time-series sample count overflowed");
      }
      ++group.count;
      group.minimum = std::min(group.minimum, row.value);
      group.maximum = std::max(group.maximum, row.value);
      group.sum += row.value;
      if (!std::isfinite(group.sum)) {
        return refuse("SB_MODEL_TIME_SERIES_VALUE_INVALID_V1",
                      "time-series aggregate became non-finite");
      }
    }
    if (groups.size() > request.maximum_groups ||
        groups.size() > request.maximum_output_rows) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "time-series group count exceeded its bound");
    }
    result.ordering_id =
        "series_metric_tags_bucket_start_ascending_v1";
    if (!CheckedTimeSeriesMultiply(
            static_cast<std::uint64_t>(groups.size()),
            sizeof(EngineBoundTimeSeriesDownsampleRowV1),
            &retained_vector_bytes) ||
        !account_working(retained_vector_bytes)) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "time-series downsample output vector exceeded its memory bound");
    }
    result.downsample_rows.reserve(groups.size());
    for (const auto& [key, group] : groups) {
      (void)key;
      std::uint64_t output_dynamic_bytes = 0;
      if (!CheckedTimeSeriesOwnedStringBytes(group.series_uuid,
                                             &output_dynamic_bytes) ||
          !CheckedTimeSeriesOwnedStringBytes(group.metric_uuid,
                                             &output_dynamic_bytes) ||
          !CheckedTimeSeriesOwnedStringBytes(group.tags,
                                             &output_dynamic_bytes) ||
          !CheckedTimeSeriesAdd(62, &output_dynamic_bytes) ||
          !account_working(output_dynamic_bytes)) {
        return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "time-series downsample retained output memory exceeded its bound");
      }
      EngineBoundTimeSeriesDownsampleRowV1 output;
      output.series_uuid = group.series_uuid;
      output.metric_uuid = group.metric_uuid;
      output.bucket_start_ns = group.bucket_start_ns;
      output.bucket_end_ns = group.bucket_end_ns;
      output.bucket_start = FormatTimeSeriesTimestamp(group.bucket_start_ns);
      output.bucket_end = FormatTimeSeriesTimestamp(group.bucket_end_ns);
      output.tags = group.tags;
      output.sample_count = group.count;
      switch (request.aggregate) {
        case EngineBoundTimeSeriesAggregateV1::kCount:
          output.aggregate_count = group.count;
          break;
        case EngineBoundTimeSeriesAggregateV1::kSum:
          output.aggregate_value = group.sum;
          break;
        case EngineBoundTimeSeriesAggregateV1::kMin:
          output.aggregate_value = group.minimum;
          break;
        case EngineBoundTimeSeriesAggregateV1::kMax:
          output.aggregate_value = group.maximum;
          break;
        case EngineBoundTimeSeriesAggregateV1::kAvg:
          output.aggregate_value = group.sum / static_cast<double>(group.count);
          break;
        case EngineBoundTimeSeriesAggregateV1::kNone:
          return refuse("SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1",
                        "time-series downsample aggregate is absent");
      }
      if (request.aggregate != EngineBoundTimeSeriesAggregateV1::kCount &&
          !std::isfinite(output.aggregate_value)) {
        return refuse("SB_MODEL_TIME_SERIES_VALUE_INVALID_V1",
                      "time-series aggregate result is non-finite");
      }
      const std::uint64_t bytes =
          output.series_uuid.size() + output.metric_uuid.size() +
          output.bucket_start.size() + output.bucket_end.size() +
          output.tags.size() + sizeof(output.sample_count) +
          sizeof(output.aggregate_count) + sizeof(output.aggregate_value) + 7;
      if (!CheckedTimeSeriesAdd(bytes, &result_bytes) ||
          result_bytes > request.maximum_result_bytes) {
        return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "time-series downsample result preflight exceeded its bound");
      }
      result.downsample_rows.push_back(std::move(output));
    }
  }
  if (cancelled()) {
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "time-series execution was cancelled before publication");
  }
  result.result_byte_count = result_bytes;
  result.residual_recheck_complete = true;
  result.base_row_mga_recheck_complete = true;
  result.security_recheck_complete = true;
  constexpr std::uint64_t kPublicationEvidenceCarrierBytes =
      4 * sizeof(EngineEvidenceReference) + 1024;
  if (!account_working(kPublicationEvidenceCarrierBytes)) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "time-series evidence publication exceeded its memory grant");
  }
  result.evidence.reserve(result.evidence.size() + 4);
  AddApiBehaviorEvidence(&result, "time_series_visibility_authority",
                         "engine_mga_statement_snapshot_first");
  AddApiBehaviorEvidence(&result, "time_series_range",
                         FormatTimeSeriesTimestamp(range_start_ns) + "/" +
                             FormatTimeSeriesTimestamp(range_end_ns));
  AddApiBehaviorEvidence(
      &result, "time_series_access_route",
      result.selected_access_path_id);
  AddApiBehaviorEvidence(&result, "time_series_ordering", result.ordering_id);
  std::uint64_t evidence_memory_bytes = 0;
  if (!CheckedTimeSeriesOwnedStringBytes(result.operation_id,
                                         &evidence_memory_bytes)) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "time-series result metadata memory receipt overflowed");
  }
  if (!CheckedTimeSeriesMultiply(
          result.evidence.capacity(), sizeof(EngineEvidenceReference),
          &retained_vector_bytes) ||
      !CheckedTimeSeriesAdd(retained_vector_bytes,
                            &evidence_memory_bytes)) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "time-series evidence memory receipt overflowed");
  }
  for (const auto& evidence : result.evidence) {
    std::uint64_t item_bytes = 0;
    if (!CheckedTimeSeriesOwnedStringBytes(evidence.evidence_kind,
                                           &item_bytes) ||
        !CheckedTimeSeriesOwnedStringBytes(evidence.evidence_id,
                                           &item_bytes) ||
        !CheckedTimeSeriesAdd(item_bytes, &evidence_memory_bytes)) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "time-series evidence memory receipt overflowed");
    }
  }
  if (!account_working(evidence_memory_bytes)) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "time-series evidence exceeded its memory grant");
  }
  if (cancelled()) {
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "time-series execution was cancelled at final publication");
  }
  const auto current_memory =
      BoundTimeSeriesResultLogicalMemoryBytesV1(result);
  if (!current_memory.has_value()) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "time-series result memory receipt overflowed");
  }
  result.current_live_memory_bytes = *current_memory;
  result.peak_live_memory_bytes =
      std::max(api_peak_live_memory_bytes, *current_memory);
  result.memory_grant_bytes = request.maximum_memory_bytes;
  result.memory_receipt_complete =
      result.current_live_memory_bytes <= result.peak_live_memory_bytes &&
      result.peak_live_memory_bytes <= result.memory_grant_bytes;
  result.cancellation_observed = cancellation_observed;
  result.cancellation_probe_failed = cancellation_probe_failed;
  if (!result.memory_receipt_complete) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "time-series API live-memory receipt exceeded its grant");
  }
  return result;
  } catch (const std::bad_alloc&) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "time-series execution allocation was refused");
  } catch (const std::length_error&) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "time-series execution allocation length was refused");
  } catch (const std::exception& exception) {
    return refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
                  std::string("time-series execution threw: ") +
                      exception.what());
  } catch (...) {
    return refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
                  "time-series execution threw a non-standard exception");
  }
}

EngineBoundTimeSeriesReadResultV1 EngineBoundTimeSeriesReadV1(
    const EngineBoundTimeSeriesReadRequestV1& request) {
  try {
    return EngineBoundTimeSeriesReadV1Impl(request);
  } catch (const std::bad_alloc&) {
    auto result = BoundTimeSeriesRefusal(
        request, "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
        "time-series execution allocation was refused");
    result.memory_grant_bytes = request.maximum_memory_bytes;
    return result;
  } catch (const std::length_error&) {
    auto result = BoundTimeSeriesRefusal(
        request, "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
        "time-series execution allocation length was refused");
    result.memory_grant_bytes = request.maximum_memory_bytes;
    return result;
  } catch (const std::exception& exception) {
    auto result = BoundTimeSeriesRefusal(
        request, "SB_MODEL_COORDINATOR_LEG_FAILED_V1",
        std::string("time-series execution threw: ") + exception.what());
    result.memory_grant_bytes = request.maximum_memory_bytes;
    return result;
  } catch (...) {
    auto result = BoundTimeSeriesRefusal(
        request, "SB_MODEL_COORDINATOR_LEG_FAILED_V1",
        "time-series execution threw a non-standard exception");
    result.memory_grant_bytes = request.maximum_memory_bytes;
    return result;
  }
}

}  // namespace scratchbird::engine::internal_api
