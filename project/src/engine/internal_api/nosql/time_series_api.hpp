// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "nosql/nosql_physical_provider_contract.hpp"

#include <string>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

inline constexpr const char* kTimeSeriesPhysicalProofMissing =
    "SB_TIME_SERIES_PHYSICAL_PROOF_MISSING";
inline constexpr const char* kTimeSeriesBucketStoreProofMissing =
    "SB_TIME_SERIES_BUCKET_STORE_PROOF_MISSING";
inline constexpr const char* kTimeSeriesColumnarMetricPageProofMissing =
    "SB_TIME_SERIES_COLUMNAR_METRIC_PAGE_PROOF_MISSING";
inline constexpr const char* kTimeSeriesSummaryProofMissing =
    "SB_TIME_SERIES_SUMMARY_PROOF_MISSING";
inline constexpr const char* kTimeSeriesRollupProofMissing =
    "SB_TIME_SERIES_ROLLUP_PROOF_MISSING";
inline constexpr const char* kTimeSeriesLateArrivalPolicyProofMissing =
    "SB_TIME_SERIES_LATE_ARRIVAL_POLICY_PROOF_MISSING";
inline constexpr const char* kTimeSeriesPointBatchRequired =
    "SB_TIME_SERIES_POINT_BATCH_REQUIRED";
inline constexpr const char* kTimeSeriesBucketDurationRequired =
    "SB_TIME_SERIES_BUCKET_DURATION_REQUIRED";
inline constexpr const char* kTimeSeriesLateArrivalRejected =
    "SB_TIME_SERIES_LATE_ARRIVAL_REJECTED";

enum class EngineTimeSeriesLateArrivalPolicy {
  kReject,
  kDeltaMergeReopen,
};

struct EngineTimeSeriesPointTag {
  std::string key;
  std::string value;
};

struct EngineTimeSeriesPoint {
  EngineApiI64 timestamp_ns = 0;
  std::string metric_name;
  double numeric_value = 0.0;
  std::vector<EngineTimeSeriesPointTag> metadata_tags;
};

struct EngineTimeSeriesPhysicalProof {
  EngineNoSqlPhysicalProviderContract provider_contract;
  bool proof_supplied = false;
  bool time_meta_bucket_store_proof = false;
  bool columnar_metric_page_proof = false;
  bool summary_min_max_count_sum_proof = false;
  bool rollup_materialization_proof = false;
  bool late_arrival_delta_merge_proof = false;
};

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_TIME_SERIES_API
struct EngineTimeSeriesAppendRequest : EngineApiRequest {
  bool physical_append = false;
  std::vector<EngineTimeSeriesPoint> points;
  EngineApiI64 bucket_duration_ns = 0;
  std::vector<EngineApiI64> rollup_intervals_ns;
  EngineApiI64 late_arrival_watermark_ns = 0;
  EngineTimeSeriesLateArrivalPolicy late_arrival_policy =
      EngineTimeSeriesLateArrivalPolicy::kDeltaMergeReopen;
  EngineTimeSeriesPhysicalProof physical_proof;
};
struct EngineTimeSeriesAppendResult : EngineApiResult {};
EngineTimeSeriesAppendResult EngineTimeSeriesAppend(const EngineTimeSeriesAppendRequest& request);

// RCP-076 engine-bound, read-only time-series model-source contract. This
// surface is intentionally distinct from EngineTimeSeriesAppend: query
// operands and immutable authority cross this boundary, never stored rows.
enum class EngineBoundTimeSeriesReadOperationV1 : std::uint8_t {
  kRangeRead = 1,
  kBucketDownsample = 2,
};

enum class EngineBoundTimeSeriesAggregateV1 : std::uint8_t {
  kNone = 0,
  kCount,
  kSum,
  kMin,
  kMax,
  kAvg,
};

struct EngineBoundTimeSeriesReadRequestV1 : EngineApiRequest {
  std::uint16_t abi_version{1};
  EngineBoundTimeSeriesReadOperationV1 operation{
      EngineBoundTimeSeriesReadOperationV1::kRangeRead};
  EngineBoundTimeSeriesAggregateV1 aggregate{
      EngineBoundTimeSeriesAggregateV1::kNone};
  std::string object_uuid;
  EngineTypedValue range_start;
  EngineTypedValue range_end;
  EngineApiI64 bucket_interval_ns{0};
  std::string expected_descriptor_uuid;
  std::uint64_t expected_descriptor_generation{0};
  std::string selected_alternative_uuid;
  std::string capability_uuid;
  std::string provider_uuid;
  std::uint64_t provider_generation{0};
  std::uint64_t rollup_generation{0};
  std::uint64_t visible_late_arrival_generation{0};
  std::uint64_t maximum_scanned_row_versions{0};
  std::uint64_t maximum_decoded_bytes{0};
  std::size_t maximum_output_rows{0};
  std::size_t maximum_groups{0};
  std::uint64_t maximum_tag_bytes{0};
  std::uint64_t maximum_result_bytes{0};
  std::uint64_t maximum_memory_bytes{0};
  bool rollup_candidate_selected{false};
  bool exact_fallback_selected{false};
  std::function<bool()> cancellation_requested;
};

struct EngineBoundTimeSeriesPointRowV1 {
  std::string row_uuid;
  std::string series_uuid;
  std::string metric_uuid;
  EngineApiI64 point_timestamp_ns{0};
  std::string point_timestamp;
  std::string tags;
  double value{0.0};
};

struct EngineBoundTimeSeriesDownsampleRowV1 {
  std::string series_uuid;
  std::string metric_uuid;
  EngineApiI64 bucket_start_ns{0};
  EngineApiI64 bucket_end_ns{0};
  std::string bucket_start;
  std::string bucket_end;
  std::string tags;
  EngineApiI64 sample_count{0};
  EngineApiI64 aggregate_count{0};
  double aggregate_value{0.0};
};

struct EngineBoundTimeSeriesReadResultV1 : EngineApiResult {
  bool data_access_observed{false};
  bool exact_fallback_observed{false};
  bool rollup_observed{false};
  bool rollup_equivalence_recheck_complete{false};
  std::uint64_t preferred_access_invocation_count{0};
  std::uint64_t exact_fallback_access_invocation_count{0};
  std::string selected_access_path_id;
  bool residual_recheck_complete{false};
  bool base_row_mga_recheck_complete{false};
  bool security_recheck_complete{false};
  std::uint64_t scanned_row_version_count{0};
  std::uint64_t selected_visible_row_count{0};
  std::uint64_t result_byte_count{0};
  std::string ordering_id;
  std::string descriptor_uuid;
  std::uint64_t descriptor_generation{0};
  std::string selected_alternative_uuid;
  std::string capability_uuid;
  std::string provider_uuid;
  std::uint64_t provider_generation{0};
  std::vector<EngineBoundTimeSeriesPointRowV1> rows;
  std::vector<EngineBoundTimeSeriesDownsampleRowV1> downsample_rows;
};

bool EngineExactTimeSeriesBucketStartV1(EngineApiI64 timestamp_ns,
                                        EngineApiI64 interval_ns,
                                        EngineApiI64* bucket_start_ns,
                                        std::string* bucket_start);

EngineBoundTimeSeriesReadResultV1 EngineBoundTimeSeriesReadV1(
    const EngineBoundTimeSeriesReadRequestV1& request);

}  // namespace scratchbird::engine::internal_api
