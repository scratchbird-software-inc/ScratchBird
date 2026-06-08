// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

enum class SblrAggregateFunctionKind {
  unknown,
  count,
  sum,
  avg,
  min,
  max,
  every,
  any,
  variance_sample,
  variance_population,
  stddev_sample,
  stddev_population,
  corr,
  covariance_population,
  covariance_sample,
  regr_avgx,
  regr_avgy,
  regr_count,
  regr_intercept,
  regr_r2,
  regr_slope,
  regr_sxx,
  regr_sxy,
  regr_syy,
  bit_and,
  bit_or,
  bit_xor,
  string_agg,
  list_agg,
  binary_agg,
  json_array_agg,
  json_object_agg,
  approx_count_distinct,
  approx_quantile,
  top_k,
};

struct SblrAggregateWindowState {
  std::string state_uuid;
  std::string function_uuid;
  std::string function_id;
  std::string result_descriptor_id;
  SblrAggregateFunctionKind aggregate_kind = SblrAggregateFunctionKind::unknown;
  bool inverse_supported = false;
  bool recompute_required = true;
  bool initialized = false;
  bool finalized = false;
  std::uint64_t input_count = 0;
  std::uint64_t non_null_count = 0;
  std::uint64_t memory_bytes = 0;
  long double numeric_sum = 0.0L;
  long double numeric_mean = 0.0L;
  long double numeric_m2 = 0.0L;
  long double corr_mean_x = 0.0L;
  long double corr_mean_y = 0.0L;
  long double corr_m2_x = 0.0L;
  long double corr_m2_y = 0.0L;
  long double corr_comoment = 0.0L;
  bool saw_true = false;
  bool saw_false = false;
  bool bit_initialized = false;
  std::uint64_t bit_accumulator = 0;
  SblrValue min_value;
  SblrValue max_value;
  std::string text_accumulator;
  std::string separator;
  std::uint64_t listagg_retained_count = 0;
  std::vector<std::uint8_t> binary_accumulator;
  std::vector<std::string> distinct_values;
  std::vector<std::string> sketch_values;
  std::vector<std::uint64_t> sketch_counts;
  std::vector<long double> sketch_numeric_values;
  long double quantile = 0.5L;
  std::uint64_t top_k_limit = 10;
};

enum class SblrListAggOverflowMode {
  none,
  error,
  truncate,
};

struct SblrAggregateOptions {
  std::uint64_t max_state_bytes = 0;
  bool count_nulls = false;
  SblrListAggOverflowMode listagg_overflow_mode = SblrListAggOverflowMode::none;
  std::uint64_t listagg_max_output_bytes = 0;
  std::string listagg_truncation_indicator = "...";
  bool listagg_with_count = true;
};

struct SblrAggregateUpdateRequest {
  SblrExecutionContext context;
  SblrAggregateOptions options;
  std::vector<SblrValue> values;
};

struct SblrAggregateFinalizeRequest {
  SblrExecutionContext context;
  SblrAggregateOptions options;
};

enum class SblrWindowFunctionKind {
  unknown,
  row_number,
  rank,
  dense_rank,
  percent_rank,
  cume_dist,
  ntile,
  lag,
  lead,
  first_value,
  last_value,
  nth_value,
  aggregate_as_window,
};

struct SblrWindowRow {
  std::vector<SblrValue> values;
  std::uint64_t peer_group = 0;
};

struct SblrWindowFrameRequest {
  SblrExecutionContext context;
  std::string function_id;
  std::string function_uuid;
  std::vector<SblrWindowRow> rows;
  std::size_t current_row_index = 0;
  std::size_t frame_start_index = 0;
  std::size_t frame_end_exclusive = 0;
  std::int64_t offset = 1;
  std::uint64_t ntile_bucket_count = 1;
  std::uint64_t nth = 1;
  SblrValue default_value;
  bool default_value_present = false;
  std::string aggregate_function_id;
  std::string aggregate_function_uuid;
  std::string aggregate_result_descriptor_id;
  SblrAggregateOptions aggregate_options;
};

SblrAggregateFunctionKind ResolveSblrAggregateFunctionKind(std::string_view function_id);
bool IsSblrAggregateFunctionSupported(std::string_view function_id);
SblrWindowFunctionKind ResolveSblrWindowFunctionKind(std::string_view function_id);
bool IsSblrWindowFunctionSupported(std::string_view function_id);
SblrResult InitializeSblrAggregateState(std::string_view function_id,
                                        std::string function_uuid,
                                        std::string result_descriptor_id,
                                        const SblrExecutionContext& context,
                                        SblrAggregateWindowState* state);
SblrResult UpdateSblrAggregateState(SblrAggregateWindowState* state,
                                    const SblrAggregateUpdateRequest& request);
SblrResult MergeSblrAggregateState(SblrAggregateWindowState* target,
                                   const SblrAggregateWindowState& source,
                                   const SblrExecutionContext& context);
SblrResult FinalizeSblrAggregateState(const SblrAggregateWindowState& state,
                                      const SblrAggregateFinalizeRequest& request);
SblrResult EvaluateSblrWindowFunction(const SblrWindowFrameRequest& request);
SblrResult RefuseSblrAggregateWindowRuntime(const SblrExecutionContext& context,
                                            const SblrAggregateWindowState& state);

}  // namespace scratchbird::engine::sblr
