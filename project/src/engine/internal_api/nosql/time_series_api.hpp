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

}  // namespace scratchbird::engine::internal_api
