// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by sblr_aggregate_window_runtime.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
      return ScalarResult(state, BoolValue(state.saw_true));
    case SblrAggregateFunctionKind::variance_sample:
      if (state.non_null_count < 2) return ScalarResult(state, NullValue("real64"));
      return ScalarResult(state, FinalNumericValue(state, state.numeric_m2 / static_cast<long double>(state.non_null_count - 1)));
    case SblrAggregateFunctionKind::variance_population:
      if (state.non_null_count == 0) return ScalarResult(state, NullValue("real64"));
      return ScalarResult(state, FinalNumericValue(state, state.numeric_m2 / static_cast<long double>(state.non_null_count)));
    case SblrAggregateFunctionKind::stddev_sample:
      if (state.non_null_count < 2) return ScalarResult(state, NullValue("real64"));
      return ScalarResult(state, Real64Value(std::sqrt(static_cast<double>(state.numeric_m2 / static_cast<long double>(state.non_null_count - 1)))));
    case SblrAggregateFunctionKind::stddev_population:
      if (state.non_null_count == 0) return ScalarResult(state, NullValue("real64"));
      return ScalarResult(state, Real64Value(std::sqrt(static_cast<double>(state.numeric_m2 / static_cast<long double>(state.non_null_count)))));
    case SblrAggregateFunctionKind::corr:
      if (state.non_null_count < 2 || state.corr_m2_x <= 0.0L || state.corr_m2_y <= 0.0L) {
        return ScalarResult(state, NullValue("real64"));
      }
      return ScalarResult(state,
                          Real64Value(static_cast<double>(
                              state.corr_comoment / std::sqrt(state.corr_m2_x * state.corr_m2_y))));
    case SblrAggregateFunctionKind::covariance_population:
      if (state.non_null_count == 0) return ScalarResult(state, NullValue("real64"));
      return ScalarResult(state,
                          Real64Value(static_cast<double>(
                              state.corr_comoment / static_cast<long double>(state.non_null_count))));
    case SblrAggregateFunctionKind::covariance_sample:
      if (state.non_null_count < 2) return ScalarResult(state, NullValue("real64"));
      return ScalarResult(state,
                          Real64Value(static_cast<double>(
                              state.corr_comoment / static_cast<long double>(state.non_null_count - 1))));
    case SblrAggregateFunctionKind::regr_avgx:
      if (state.non_null_count == 0) return ScalarResult(state, NullValue("real64"));
      return ScalarResult(state, Real64Value(static_cast<double>(state.corr_mean_x)));
    case SblrAggregateFunctionKind::regr_avgy:
      if (state.non_null_count == 0) return ScalarResult(state, NullValue("real64"));
      return ScalarResult(state, Real64Value(static_cast<double>(state.corr_mean_y)));
    case SblrAggregateFunctionKind::regr_count:
      return ScalarResult(state, Int64Value(static_cast<std::int64_t>(state.non_null_count)));
    case SblrAggregateFunctionKind::regr_intercept:
      if (state.non_null_count == 0 || state.corr_m2_x == 0.0L) {
        return ScalarResult(state, NullValue("real64"));
      }
      return ScalarResult(state,
                          Real64Value(static_cast<double>(
                              state.corr_mean_y -
                              (state.corr_mean_x * state.corr_comoment / state.corr_m2_x))));
    case SblrAggregateFunctionKind::regr_r2:
      if (state.non_null_count == 0 || state.corr_m2_x == 0.0L) {
        return ScalarResult(state, NullValue("real64"));
      }
      if (state.corr_m2_y == 0.0L) {
        return ScalarResult(state, Real64Value(1.0));
      }
      return ScalarResult(state,
                          Real64Value(static_cast<double>(
                              (state.corr_comoment * state.corr_comoment) /
                              (state.corr_m2_x * state.corr_m2_y))));
    case SblrAggregateFunctionKind::regr_slope:
      if (state.non_null_count == 0 || state.corr_m2_x == 0.0L) {
        return ScalarResult(state, NullValue("real64"));
      }
      return ScalarResult(state,
                          Real64Value(static_cast<double>(
                              state.corr_comoment / state.corr_m2_x)));
    case SblrAggregateFunctionKind::regr_sxx:
      if (state.non_null_count == 0) return ScalarResult(state, NullValue("real64"));
      return ScalarResult(state, Real64Value(static_cast<double>(state.corr_m2_x)));
    case SblrAggregateFunctionKind::regr_sxy:
      if (state.non_null_count == 0) return ScalarResult(state, NullValue("real64"));
      return ScalarResult(state, Real64Value(static_cast<double>(state.corr_comoment)));
    case SblrAggregateFunctionKind::regr_syy:
      if (state.non_null_count == 0) return ScalarResult(state, NullValue("real64"));
      return ScalarResult(state, Real64Value(static_cast<double>(state.corr_m2_y)));
    case SblrAggregateFunctionKind::bit_and:
    case SblrAggregateFunctionKind::bit_or:
    case SblrAggregateFunctionKind::bit_xor:
      if (!state.bit_initialized) return ScalarResult(state, NullValue("uint64"));
      return ScalarResult(state, Int64Value(static_cast<std::int64_t>(state.bit_accumulator)));
    case SblrAggregateFunctionKind::string_agg:
      if (state.non_null_count == 0) return ScalarResult(state, NullValue(state.result_descriptor_id.empty() ? "text" : state.result_descriptor_id));
      if (IsListAggFunctionId(state.function_id)) return FinalizeListAggText(state, request);
      return ScalarResult(state, TextResultValue(state, state.text_accumulator, "text"));
    case SblrAggregateFunctionKind::list_agg:
      if (state.input_count == 0 || state.listagg_retained_count == 0) {
        return ScalarResult(state, NullValue(ListDescriptorForState(state)));
      }
      return ScalarResult(state, ListResultValue(state));
    case SblrAggregateFunctionKind::binary_agg:
      if (state.non_null_count == 0) return ScalarResult(state, NullValue(state.result_descriptor_id.empty() ? "binary" : state.result_descriptor_id));
      return ScalarResult(state, BinaryResultValue(state));
    case SblrAggregateFunctionKind::json_array_agg:
      if (state.non_null_count == 0) return ScalarResult(state, NullValue("json"));
      return ScalarResult(state, JsonArrayResult(state));
    case SblrAggregateFunctionKind::json_object_agg:
      if (state.non_null_count == 0) return ScalarResult(state, NullValue("json"));
      return ScalarResult(state, JsonObjectResult(state));
    case SblrAggregateFunctionKind::approx_count_distinct:
      return ScalarResult(state, Int64Value(static_cast<std::int64_t>(state.distinct_values.size())));
    case SblrAggregateFunctionKind::approx_quantile: {
      if (state.sketch_numeric_values.empty()) return ScalarResult(state, NullValue("real64"));
      auto values = state.sketch_numeric_values;
      std::sort(values.begin(), values.end());
      if (IsDiscretePercentileId(state.function_id)) {
        const long double scaled = state.quantile * static_cast<long double>(values.size());
        const std::size_t index = state.quantile <= 0.0L
                                      ? 0
                                      : static_cast<std::size_t>(std::ceil(scaled) - 1.0L);
        return ScalarResult(state, DiscretePercentileResultValue(
                                       state,
                                       values[std::min(index, values.size() - 1)]));
      }
      const long double scaled = state.quantile * static_cast<long double>(values.size() - 1);
      const std::size_t lower_index = static_cast<std::size_t>(std::floor(scaled));
      const std::size_t upper_index = static_cast<std::size_t>(std::ceil(scaled));
      if (lower_index == upper_index) {
        return ScalarResult(state, PercentileResultValue(state, values[lower_index]));
      }
      const long double weight = scaled - static_cast<long double>(lower_index);
      const long double interpolated = values[lower_index] +
                                       ((values[upper_index] - values[lower_index]) * weight);
      return ScalarResult(state, PercentileResultValue(state, interpolated));
    }
    case SblrAggregateFunctionKind::top_k:
      if (state.sketch_values.empty()) return ScalarResult(state, NullValue("json"));
      if (IsModeAggregateId(state.function_id)) return ScalarResult(state, ModeResultValue(state));
      return ScalarResult(state, TopKResult(state));
    case SblrAggregateFunctionKind::unknown:
      return AggregateFailure(SblrStatusCode::unsupported_feature,
                              AggregateOperationId(state),
                              AggregateDiagnostic("SB_DIAG_AGGREGATE_KIND_UNSUPPORTED",
                                                  request.context,
                                                  &state,
                                                  "aggregate function id is not supported by the aggregate ABI"));
  }
  return AggregateFailure(SblrStatusCode::internal_error,
                          AggregateOperationId(state),
                          AggregateDiagnostic("SB_DIAG_AGGREGATE_FINALIZE_UNREACHABLE",
                                              request.context,
                                              &state,
                                              "aggregate finalize reached an impossible state"));
}

SblrResult EvaluateSblrWindowFunction(const SblrWindowFrameRequest& request) {
  const auto kind = ResolveSblrWindowFunctionKind(request.function_id);
  if (kind == SblrWindowFunctionKind::unknown) {
    auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_WINDOW_FUNCTION_UNSUPPORTED",
                                                request.context,
                                                "window function id is not supported by the window ABI");
    diagnostic.fields.push_back({"function_id", request.function_id});
    diagnostic.fields.push_back({"function_uuid", request.function_uuid});
    return MakeSblrFailure(SblrStatusCode::unsupported_feature, request.function_id, std::move(diagnostic));
  }

  auto is_hypothetical_set_request = [&]() -> bool {
    if (!request.default_value_present) return false;
    const std::string id = Lower(request.function_id);
    if (id.find("withingroup") != std::string::npos ||
        id.find("within_group") != std::string::npos ||
        id.find("ordered_set.") != std::string::npos ||
        id.find("hypothetical.") != std::string::npos) {
      return kind == SblrWindowFunctionKind::rank ||
             kind == SblrWindowFunctionKind::dense_rank ||
             kind == SblrWindowFunctionKind::percent_rank ||
             kind == SblrWindowFunctionKind::cume_dist;
    }
    return id == "data.aggregate.rank" || id == "aggregate.rank" || id == "sb.aggregate.rank" ||
           id == "data.aggregate.dense_rank" || id == "aggregate.dense_rank" ||
           id == "sb.aggregate.dense_rank" ||
           id == "data.aggregate.percent_rank" || id == "aggregate.percent_rank" ||
           id == "sb.aggregate.percent_rank" ||
           id == "data.aggregate.cume_dist" || id == "aggregate.cume_dist" ||
           id == "sb.aggregate.cume_dist";
  };

  if (is_hypothetical_set_request()) {
    if (request.default_value.is_null) {
      auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_ORDERED_SET_HYPOTHETICAL_VALUE_REQUIRED",
                                                  request.context,
                                                  "hypothetical-set ranking requires a non-NULL hypothetical value in this ABI slice");
      diagnostic.fields.push_back({"function_id", request.function_id});
      return MakeSblrFailure(SblrStatusCode::execution_failed, request.function_id, std::move(diagnostic));
    }

    std::uint64_t less_count = 0;
    std::uint64_t less_or_equal_count = 0;
    std::vector<SblrValue> distinct_less_values;
    for (const auto& row : request.rows) {
      if (row.values.empty() || row.values.front().is_null) {
        auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_ORDERED_SET_INPUT_VALUE_REQUIRED",
                                                    request.context,
                                                    "hypothetical-set ranking requires non-NULL ordered input values in this ABI slice");
        diagnostic.fields.push_back({"function_id", request.function_id});
        return MakeSblrFailure(SblrStatusCode::execution_failed, request.function_id, std::move(diagnostic));
      }
      const SblrValue& value = row.values.front();
      const int comparison = CompareValues(value, request.default_value);
      if (comparison < 0) {
        less_count += 1;
        const auto already_seen = std::find_if(distinct_less_values.begin(),
                                               distinct_less_values.end(),
                                               [&](const SblrValue& seen) {
                                                 return CompareValues(seen, value) == 0;
                                               });
        if (already_seen == distinct_less_values.end()) distinct_less_values.push_back(value);
      }
      if (comparison <= 0) less_or_equal_count += 1;
    }

    SblrAggregateWindowState operation_state;
    operation_state.function_id = request.function_id;
    operation_state.function_uuid = request.function_uuid;

    const std::uint64_t group_rows = static_cast<std::uint64_t>(request.rows.size());
    const std::uint64_t rank = less_count + 1;
    switch (kind) {
      case SblrWindowFunctionKind::rank:
        return ScalarResult(operation_state, Int64Value(static_cast<std::int64_t>(rank)));
      case SblrWindowFunctionKind::dense_rank:
        return ScalarResult(operation_state,
                            Int64Value(static_cast<std::int64_t>(distinct_less_values.size() + 1)));
      case SblrWindowFunctionKind::percent_rank:
        if (group_rows == 0) return ScalarResult(operation_state, Real64Value(0.0));
        return ScalarResult(operation_state,
                            Real64Value(static_cast<double>(rank - 1) /
                                        static_cast<double>(group_rows)));
      case SblrWindowFunctionKind::cume_dist:
        return ScalarResult(operation_state,
                            Real64Value(static_cast<double>(less_or_equal_count + 1) /
                                        static_cast<double>(group_rows + 1)));
      default:
        break;
    }
  }

  if (request.rows.empty() || request.current_row_index >= request.rows.size()) {
    auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_WINDOW_FRAME_INVALID",
                                                request.context,
                                                "window frame requires at least one row and a valid current row index");
    diagnostic.fields.push_back({"function_id", request.function_id});
    return MakeSblrFailure(SblrStatusCode::execution_failed, request.function_id, std::move(diagnostic));
  }

  const std::size_t total_rows = request.rows.size();
  const std::size_t frame_start = std::min(request.frame_start_index, total_rows);
  const std::size_t frame_end = request.frame_end_exclusive == 0
                                    ? total_rows
                                    : std::min(request.frame_end_exclusive, total_rows);
  auto null_value_for_window = [&]() -> SblrValue {
    for (const auto& row : request.rows) {
      if (!row.values.empty()) return NullValue(row.values.front().descriptor_id);
    }
    return NullValue({});
  };
  auto default_or_null = [&]() -> SblrValue {
    return request.default_value_present ? request.default_value : null_value_for_window();
  };
  if (frame_start >= frame_end) {
    SblrAggregateWindowState empty_frame_state;
    empty_frame_state.function_id = request.function_id;
    return ScalarResult(empty_frame_state, default_or_null());
  }

  auto value_at = [&](std::size_t index) -> SblrValue {
    if (index >= request.rows.size() || request.rows[index].values.empty()) {
      return default_or_null();
    }
    return request.rows[index].values.front();
  };

  auto rank_for_current = [&]() -> std::uint64_t {
    const std::uint64_t current_peer = request.rows[request.current_row_index].peer_group;
    if (current_peer == 0) return static_cast<std::uint64_t>(request.current_row_index + 1);
    for (std::size_t index = 0; index < total_rows; ++index) {
      if (request.rows[index].peer_group == current_peer) return static_cast<std::uint64_t>(index + 1);
    }
    return static_cast<std::uint64_t>(request.current_row_index + 1);
  };

  auto dense_rank_for_current = [&]() -> std::uint64_t {
    const std::uint64_t current_peer = request.rows[request.current_row_index].peer_group;
    if (current_peer == 0) return static_cast<std::uint64_t>(request.current_row_index + 1);
    std::vector<std::uint64_t> seen;
    for (std::size_t index = 0; index <= request.current_row_index && index < total_rows; ++index) {
      const std::uint64_t peer = request.rows[index].peer_group;
      if (std::find(seen.begin(), seen.end(), peer) == seen.end()) seen.push_back(peer);
    }
    return static_cast<std::uint64_t>(seen.size());
  };

  auto cume_dist_for_current = [&]() -> double {
    const std::uint64_t current_peer = request.rows[request.current_row_index].peer_group;
    std::size_t last_index = request.current_row_index;
    if (current_peer != 0) {
      for (std::size_t index = request.current_row_index; index < total_rows; ++index) {
        if (request.rows[index].peer_group == current_peer) last_index = index;
      }
    }
    return static_cast<double>(last_index + 1) / static_cast<double>(total_rows);
  };

  SblrAggregateWindowState operation_state;
  operation_state.function_id = request.function_id;
  operation_state.function_uuid = request.function_uuid;

  switch (kind) {
    case SblrWindowFunctionKind::row_number:
      return ScalarResult(operation_state, Int64Value(static_cast<std::int64_t>(request.current_row_index + 1)));
    case SblrWindowFunctionKind::rank:
      return ScalarResult(operation_state, Int64Value(static_cast<std::int64_t>(rank_for_current())));
    case SblrWindowFunctionKind::dense_rank:
      return ScalarResult(operation_state, Int64Value(static_cast<std::int64_t>(dense_rank_for_current())));
    case SblrWindowFunctionKind::percent_rank:
      if (total_rows <= 1) return ScalarResult(operation_state, Real64Value(0.0));
      return ScalarResult(operation_state,
                          Real64Value(static_cast<double>(rank_for_current() - 1) /
                                      static_cast<double>(total_rows - 1)));
    case SblrWindowFunctionKind::cume_dist:
      return ScalarResult(operation_state, Real64Value(cume_dist_for_current()));
    case SblrWindowFunctionKind::ntile: {
      const std::uint64_t buckets = request.ntile_bucket_count;
      if (buckets == 0) {
        auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_WINDOW_NTILE_BUCKET_INVALID",
                                                    request.context,
                                                    "NTILE requires a positive bucket count");
        diagnostic.fields.push_back({"function_id", request.function_id});
        return MakeSblrFailure(SblrStatusCode::execution_failed, request.function_id, std::move(diagnostic));
      }
      const std::uint64_t row_number = static_cast<std::uint64_t>(request.current_row_index) + 1;
      const std::uint64_t base_size = static_cast<std::uint64_t>(total_rows) / buckets;
      const std::uint64_t larger_bucket_count = static_cast<std::uint64_t>(total_rows) % buckets;
      const std::uint64_t larger_bucket_size = base_size + 1;
      std::uint64_t bucket = 1;
      if (row_number <= larger_bucket_count * larger_bucket_size) {
        bucket = (row_number + larger_bucket_size - 1) / larger_bucket_size;
      } else {
        const std::uint64_t smaller_offset = row_number - (larger_bucket_count * larger_bucket_size);
        bucket = larger_bucket_count + ((smaller_offset + base_size - 1) / base_size);
      }
      return ScalarResult(operation_state, Int64Value(static_cast<std::int64_t>(bucket)));
    }
    case SblrWindowFunctionKind::lag: {
      const std::int64_t target = static_cast<std::int64_t>(request.current_row_index) -
                                  (request.offset <= 0 ? 1 : request.offset);
      if (target < 0) return ScalarResult(operation_state, default_or_null());
      return ScalarResult(operation_state, value_at(static_cast<std::size_t>(target)));
    }
    case SblrWindowFunctionKind::lead: {
      const std::int64_t target = static_cast<std::int64_t>(request.current_row_index) +
                                  (request.offset <= 0 ? 1 : request.offset);
      if (target < 0 || static_cast<std::size_t>(target) >= total_rows) {
        return ScalarResult(operation_state, default_or_null());
      }
      return ScalarResult(operation_state, value_at(static_cast<std::size_t>(target)));
    }
    case SblrWindowFunctionKind::first_value:
      return ScalarResult(operation_state, value_at(frame_start));
    case SblrWindowFunctionKind::last_value:
      return ScalarResult(operation_state, value_at(frame_end - 1));
    case SblrWindowFunctionKind::nth_value: {
      if (request.nth == 0) {
        auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_WINDOW_NTH_VALUE_INVALID",
                                                    request.context,
                                                    "NTH_VALUE requires a 1-based positive row number");
        diagnostic.fields.push_back({"function_id", request.function_id});
        return MakeSblrFailure(SblrStatusCode::execution_failed, request.function_id, std::move(diagnostic));
      }
      const std::size_t index = frame_start + static_cast<std::size_t>(request.nth - 1);
      if (index >= frame_end) return ScalarResult(operation_state, default_or_null());
      return ScalarResult(operation_state, value_at(index));
    }
    case SblrWindowFunctionKind::aggregate_as_window: {
      if (request.aggregate_function_id.empty()) {
        auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_WINDOW_AGGREGATE_FUNCTION_REQUIRED",
                                                    request.context,
                                                    "aggregate-as-window evaluation requires an aggregate function id");
        diagnostic.fields.push_back({"function_id", request.function_id});
        return MakeSblrFailure(SblrStatusCode::execution_failed, request.function_id, std::move(diagnostic));
      }
      SblrAggregateWindowState aggregate_state;
      auto init = InitializeSblrAggregateState(request.aggregate_function_id,
                                              request.aggregate_function_uuid,
                                              request.aggregate_result_descriptor_id,
                                              request.context,
                                              &aggregate_state);
      if (!init.ok()) return init;
      for (std::size_t index = frame_start; index < frame_end; ++index) {
        SblrAggregateUpdateRequest update;
        update.context = request.context;
        update.options = request.aggregate_options;
        update.values = request.rows[index].values;
        auto update_result = UpdateSblrAggregateState(&aggregate_state, update);
        if (!update_result.ok()) return update_result;
      }
      SblrAggregateFinalizeRequest finalize;
      finalize.context = request.context;
      finalize.options = request.aggregate_options;
      return FinalizeSblrAggregateState(aggregate_state, finalize);
    }
    case SblrWindowFunctionKind::unknown:
      break;
  }
  auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_WINDOW_FUNCTION_UNREACHABLE",
                                              request.context,
                                              "window function evaluation reached an impossible state");
  diagnostic.fields.push_back({"function_id", request.function_id});
  return MakeSblrFailure(SblrStatusCode::internal_error, request.function_id, std::move(diagnostic));
}

SblrResult RefuseSblrAggregateWindowRuntime(const SblrExecutionContext& context,
                                            const SblrAggregateWindowState& state) {
  auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_AGGREGATE_WINDOW_RUNTIME_NOT_ACTIVE", context,
                                              "aggregate/window runtime ABI is specified but not active in this slice");
  diagnostic.fields.push_back({"state_uuid", state.state_uuid});
  diagnostic.fields.push_back({"function_uuid", state.function_uuid});
  return MakeSblrFailure(SblrStatusCode::unsupported_feature, "sblr.aggregate_window", std::move(diagnostic));
}

}  // namespace scratchbird::engine::sblr
