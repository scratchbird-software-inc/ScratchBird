// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by sblr_aggregate_window_runtime.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
  stream << "[";
  const std::size_t limit = static_cast<std::size_t>(std::min<std::uint64_t>(state.top_k_limit, order.size()));
  for (std::size_t rank = 0; rank < limit; ++rank) {
    if (rank != 0) stream << ",";
    const std::size_t index = order[rank];
    stream << "{\"value\":" << JsonEscape(state.sketch_values[index])
           << ",\"count\":" << state.sketch_counts[index] << "}";
  }
  stream << "]";
  return TextResultValue(state, stream.str(), "json");
}

SblrValue FinalNumericValue(const SblrAggregateWindowState& state, long double value) {
  const std::string descriptor = state.result_descriptor_id.empty() ? "real64" : state.result_descriptor_id;
  if (descriptor == "real64" || descriptor == "double" || descriptor == "float64") {
    return Real64Value(static_cast<double>(value));
  }
  std::ostringstream stream;
  stream << std::setprecision(36) << value;
  SblrValue out;
  out.descriptor_id = descriptor;
  out.is_null = false;
  out.payload_kind = SblrValuePayloadKind::high_precision_numeric_text;
  out.encoded_value = stream.str();
  out.text_value = out.encoded_value;
  return out;
}

bool IdContains(std::string_view function_id, std::string_view needle) {
  return Lower(function_id).find(std::string(needle)) != std::string::npos;
}

bool IsModeAggregateId(std::string_view function_id) {
  const std::string id = Lower(function_id);
  return id == "mode" || id == "sb.aggregate.mode" || id == "data.aggregate.mode" ||
         id == "mode()withingroup(orderbyexpr)" || id == "mode_within_group" ||
         id == "ordered_set.mode";
}

bool IsContinuousPercentileId(std::string_view function_id) {
  return IdContains(function_id, "percentile_cont") || IdContains(function_id, "approx_median") ||
         IdContains(function_id, "median");
}

bool IsDiscretePercentileId(std::string_view function_id) {
  return IdContains(function_id, "percentile_disc");
}

bool UsesDirectPercentileFraction(std::string_view function_id) {
  return IdContains(function_id, "withingroup") || IdContains(function_id, "within_group");
}

SblrResult CaptureQuantileOption(SblrAggregateWindowState* state,
                                 const SblrAggregateUpdateRequest& request,
                                 std::size_t option_index) {
  if (option_index >= request.values.size() || request.values[option_index].is_null) {
    return MakeSblrSuccess(AggregateOperationId(*state));
  }
  long double quantile = 0.5L;
  if (!ParseLongDouble(request.values[option_index], &quantile) ||
      quantile < 0.0L || quantile > 1.0L) {
    return AggregateFailure(SblrStatusCode::execution_failed,
                            AggregateOperationId(*state),
                            AggregateDiagnostic("SB_DIAG_AGGREGATE_PERCENTILE_FRACTION_INVALID",
                                                request.context,
                                                state,
                                                "percentile fraction must be finite and between 0 and 1"));
  }
  state->quantile = quantile;
  return MakeSblrSuccess(AggregateOperationId(*state));
}

SblrResult CaptureTopKLimitOption(SblrAggregateWindowState* state,
                                  const SblrAggregateUpdateRequest& request,
                                  std::size_t option_index) {
  if (option_index >= request.values.size() || request.values[option_index].is_null) {
    return MakeSblrSuccess(AggregateOperationId(*state));
  }
  const SblrValue& option = request.values[option_index];
  std::uint64_t limit = 0;
  if ((option.has_int64_value && option.int64_value <= 0) ||
      !ParseUint64(option, &limit) || limit == 0) {
    return AggregateFailure(SblrStatusCode::execution_failed,
                            AggregateOperationId(*state),
                            AggregateDiagnostic("SB_DIAG_AGGREGATE_TOP_K_LIMIT_INVALID",
                                                request.context,
                                                state,
                                                "top-k limit must be a positive integer"));
  }
  state->top_k_limit = limit;
  return MakeSblrSuccess(AggregateOperationId(*state));
}

SblrValue PercentileResultValue(const SblrAggregateWindowState& state, long double value) {
  return FinalNumericValue(state, value);
}

SblrValue DiscretePercentileResultValue(const SblrAggregateWindowState& state, long double value) {
  return FinalNumericValue(state, value);
}

SblrValue ModeResultValue(const SblrAggregateWindowState& state) {
  std::vector<std::size_t> order(state.sketch_values.size());
  for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
  std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
    if (state.sketch_counts[left] != state.sketch_counts[right]) return state.sketch_counts[left] > state.sketch_counts[right];
    return state.sketch_values[left] < state.sketch_values[right];
  });
  if (order.empty()) return NullValue(state.result_descriptor_id.empty() ? "text" : state.result_descriptor_id);
  const std::string& value = state.sketch_values[order.front()];
  char* end = nullptr;
  const long double numeric = std::strtold(value.c_str(), &end);
  if (end != value.c_str() && *end == '\0' && std::isfinite(static_cast<double>(numeric))) {
    return FinalNumericValue(state, numeric);
  }
  return TextResultValue(state, value, "text");
}

SblrResult ScalarResult(const SblrAggregateWindowState& state, SblrValue value) {
  SblrResult result = MakeSblrSuccess(AggregateOperationId(state));
  result.scalar_values.push_back(std::move(value));
  return result;
}

}  // namespace

SblrAggregateFunctionKind ResolveSblrAggregateFunctionKind(std::string_view function_id) {
  const std::string id = Lower(function_id);
  if (id == "count" || id == "count_star" || id == "count(*)|count([distinct]expr)" ||
      id == "data.aggregate.count" ||
      id == "data.aggregate.count_star" || id == "aggregate.count" ||
      id == "sb.aggregate.count") {
    return SblrAggregateFunctionKind::count;
  }
  if (id == "sum" || id == "data.aggregate.sum" || id == "aggregate.sum") {
    return SblrAggregateFunctionKind::sum;
  }
  if (id == "avg" || id == "avg([distinct]expr)" || id == "average" ||
      id == "data.aggregate.avg" ||
      id == "data.aggregate.average" || id == "aggregate.avg" ||
      id == "sb.aggregate.avg") {
    return SblrAggregateFunctionKind::avg;
  }
  if (id == "min" || id == "min(expr)" || id == "data.aggregate.min" || id == "aggregate.min" ||
      id == "sb.aggregate.min") {
    return SblrAggregateFunctionKind::min;
  }
  if (id == "max" || id == "max(expr)" || id == "data.aggregate.max" || id == "aggregate.max" ||
      id == "sb.aggregate.max") {
    return SblrAggregateFunctionKind::max;
  }
  if (id == "every" || id == "every(boolean)" || id == "bool_and" ||
      id == "bool_and(boolean)" || id == "boolean_and" ||
      id == "data.aggregate.every" || id == "data.aggregate.bool_and" ||
      id == "sb.aggregate.every" || id == "sb.aggregate.bool_and") {
    return SblrAggregateFunctionKind::every;
  }
  if (id == "any" || id == "some" || id == "bool_or" ||
      id == "bool_or(boolean)" || id == "boolean_or" ||
      id == "data.aggregate.any" || id == "data.aggregate.some" ||
      id == "data.aggregate.bool_or" || id == "sb.aggregate.bool_or") {
    return SblrAggregateFunctionKind::any;
  }
  if (id == "variance" || id == "variance(numeric)" || id == "var_samp" || id == "variance_samp" ||
      id == "variance_samp(numeric)" ||
      id == "data.aggregate.variance" || id == "data.aggregate.var_samp" ||
      id == "data.aggregate.variance_samp" || id == "sb.aggregate.variance" ||
      id == "sb.aggregate.variance_samp") {
    return SblrAggregateFunctionKind::variance_sample;
  }
  if (id == "var_pop" || id == "variance_pop" || id == "variance_pop(numeric)" ||
      id == "data.aggregate.var_pop" || id == "data.aggregate.variance_pop" ||
      id == "sb.aggregate.variance_pop") {
    return SblrAggregateFunctionKind::variance_population;
  }
  if (id == "stddev" || id == "stddev(numeric)" || id == "stddev_samp" ||
      id == "stddev_samp(numeric)" ||
      id == "data.aggregate.stddev" || id == "data.aggregate.stddev_samp" ||
      id == "sb.aggregate.stddev" || id == "sb.aggregate.stddev_samp") {
    return SblrAggregateFunctionKind::stddev_sample;
  }
  if (id == "stddev_pop" || id == "stddev_pop(numeric)" ||
      id == "data.aggregate.stddev_pop" || id == "sb.aggregate.stddev_pop") {
    return SblrAggregateFunctionKind::stddev_population;
  }
  if (id == "corr" || id == "corr(y,x)" || id == "correlation" ||
      id == "data.aggregate.corr" || id == "data.aggregate.correlation" ||
      id == "sb.aggregate.corr") {
    return SblrAggregateFunctionKind::corr;
  }
  if (id == "covar_pop" || id == "covar_pop(y,x)" ||
      id == "data.aggregate.covar_pop" || id == "sb.aggregate.covar_pop") {
    return SblrAggregateFunctionKind::covariance_population;
  }
  if (id == "covar_samp" || id == "covar_samp(y,x)" ||
      id == "data.aggregate.covar_samp" || id == "sb.aggregate.covar_samp") {
    return SblrAggregateFunctionKind::covariance_sample;
  }
  if (id == "regr_avgx" || id == "regr_avgx(y,x)" ||
      id == "data.aggregate.regr_avgx" || id == "sb.aggregate.regr_avgx") {
    return SblrAggregateFunctionKind::regr_avgx;
  }
  if (id == "regr_avgy" || id == "regr_avgy(y,x)" ||
      id == "data.aggregate.regr_avgy" || id == "sb.aggregate.regr_avgy") {
    return SblrAggregateFunctionKind::regr_avgy;
  }
  if (id == "regr_count" || id == "regr_count(y,x)" ||
      id == "data.aggregate.regr_count" || id == "sb.aggregate.regr_count") {
    return SblrAggregateFunctionKind::regr_count;
  }
  if (id == "regr_intercept" || id == "regr_intercept(y,x)" ||
      id == "data.aggregate.regr_intercept" || id == "sb.aggregate.regr_intercept") {
    return SblrAggregateFunctionKind::regr_intercept;
  }
  if (id == "regr_r2" || id == "regr_r2(y,x)" ||
      id == "data.aggregate.regr_r2" || id == "sb.aggregate.regr_r2") {
    return SblrAggregateFunctionKind::regr_r2;
  }
  if (id == "regr_slope" || id == "regr_slope(y,x)" ||
      id == "data.aggregate.regr_slope" || id == "sb.aggregate.regr_slope") {
    return SblrAggregateFunctionKind::regr_slope;
  }
  if (id == "regr_sxx" || id == "regr_sxx(y,x)" ||
      id == "data.aggregate.regr_sxx" || id == "sb.aggregate.regr_sxx") {
    return SblrAggregateFunctionKind::regr_sxx;
  }
  if (id == "regr_sxy" || id == "regr_sxy(y,x)" ||
      id == "data.aggregate.regr_sxy" || id == "sb.aggregate.regr_sxy") {
    return SblrAggregateFunctionKind::regr_sxy;
  }
  if (id == "regr_syy" || id == "regr_syy(y,x)" ||
      id == "data.aggregate.regr_syy" || id == "sb.aggregate.regr_syy") {
    return SblrAggregateFunctionKind::regr_syy;
  }
  if (id == "bit_and" || id == "data.aggregate.bit_and") {
    return SblrAggregateFunctionKind::bit_and;
  }
  if (id == "bit_or" || id == "data.aggregate.bit_or") {
    return SblrAggregateFunctionKind::bit_or;
  }
  if (id == "bit_xor" || id == "data.aggregate.bit_xor") {
    return SblrAggregateFunctionKind::bit_xor;
  }
  if (id == "string_agg" || id == "group_concat" || id == "listagg" ||
      id == "listagg(expr[,sep][onoverflow...])withingroup(orderby...)" ||
      id == "data.aggregate.string_agg" || id == "data.aggregate.group_concat" ||
      id == "data.aggregate.listagg" || id == "sb.aggregate.listagg") {
    return SblrAggregateFunctionKind::string_agg;
  }
  if (id == "array_agg" || id == "array_agg(expr[orderby...])" || id == "list_agg" ||
      id == "data.aggregate.array_agg" || id == "data.aggregate.list_agg" ||
      id == "sb.aggregate.array_agg") {
    return SblrAggregateFunctionKind::list_agg;
  }
  if (id == "binary_agg" || id == "bytea_agg" || id == "data.aggregate.binary_agg") {
    return SblrAggregateFunctionKind::binary_agg;
  }
  if (id == "json_agg" || id == "json_agg(expr[orderby...])" ||
      id == "json_arrayagg" || id == "json_array_agg" ||
      id == "data.aggregate.json_agg" || id == "data.aggregate.json_array_agg" ||
      id == "sb.aggregate.json_agg") {
    return SblrAggregateFunctionKind::json_array_agg;
  }
  if (id == "json_object_agg" || id == "json_object_agg(key,value)" ||
      id == "json_objectagg" ||
      id == "data.aggregate.json_object_agg" ||
      id == "sb.aggregate.json_object_agg") {
    return SblrAggregateFunctionKind::json_object_agg;
  }
  if (id == "approx_count_distinct" || id == "approx_count_distinct(expr)" ||
      id == "count_distinct_approx" || id == "data.aggregate.approx_count_distinct" ||
      id == "sb.aggregate.approx_count_distinct") {
    return SblrAggregateFunctionKind::approx_count_distinct;
  }
  if (id == "approx_quantile" || id == "quantile" || id == "percentile_approx" ||
      id == "approx_median" || id == "approx_median(expr)" ||
      id == "approx_percentile_cont" || id == "approx_percentile_cont(0.5)" ||
      id == "approx_percentile_cont(fraction)withingroup(orderbyexpr)" ||
      id == "approx_percentile_disc" ||
      id == "approx_percentile_disc(fraction)withingroup(orderbyexpr)" ||
      id == "percentile_cont" || id == "percentile_cont(fraction)withingroup(orderbyexpr)" ||
      id == "percentile_disc" || id == "percentile_disc(fraction)withingroup(orderbyexpr)" ||
      id == "data.aggregate.approx_quantile" || id == "data.aggregate.approx_median" ||
      id == "data.aggregate.approx_percentile_cont" ||
      id == "data.aggregate.approx_percentile_disc" ||
      id == "data.aggregate.percentile_cont" || id == "data.aggregate.percentile_disc" ||
      id == "sb.aggregate.approx_median" || id == "sb.aggregate.approx_percentile_cont" ||
      id == "sb.aggregate.approx_percentile_disc" || id == "sb.aggregate.percentile_cont" ||
      id == "sb.aggregate.percentile_disc") {
    return SblrAggregateFunctionKind::approx_quantile;
  }
  if (id == "top_k" || id == "approx_top_k" || id == "approx_top_k(expr,k)" ||
      id == "data.aggregate.top_k" || id == "data.aggregate.approx_top_k" ||
      id == "sb.aggregate.approx_top_k" || IsModeAggregateId(id)) {
    return SblrAggregateFunctionKind::top_k;
  }
  return SblrAggregateFunctionKind::unknown;
}

bool IsSblrAggregateFunctionSupported(std::string_view function_id) {
  return ResolveSblrAggregateFunctionKind(function_id) != SblrAggregateFunctionKind::unknown;
}

SblrWindowFunctionKind ResolveSblrWindowFunctionKind(std::string_view function_id) {
  const std::string id = Lower(function_id);
  if (id == "row_number" || id == "data.window.row_number" || id == "window.row_number" ||
      id == "sb.window.row_number") {
    return SblrWindowFunctionKind::row_number;
  }
  if (id == "rank" || id == "rank(expr)withingroup(orderbyexpr)" ||
      id == "data.window.rank" || id == "window.rank" || id == "sb.window.rank" ||
      id == "data.aggregate.rank" || id == "aggregate.rank" || id == "sb.aggregate.rank" ||
      id == "rank_within_group" || id == "data.aggregate.rank_within_group" ||
      id == "ordered_set.rank" || id == "hypothetical.rank") {
    return SblrWindowFunctionKind::rank;
  }
  if (id == "dense_rank" || id == "dense_rank(expr)withingroup(orderbyexpr)" ||
      id == "data.window.dense_rank" || id == "window.dense_rank" ||
      id == "sb.window.dense_rank" || id == "data.aggregate.dense_rank" ||
      id == "aggregate.dense_rank" || id == "sb.aggregate.dense_rank" ||
      id == "dense_rank_within_group" || id == "data.aggregate.dense_rank_within_group" ||
      id == "ordered_set.dense_rank" || id == "hypothetical.dense_rank") {
    return SblrWindowFunctionKind::dense_rank;
  }
  if (id == "percent_rank" || id == "percent_rank(expr)withingroup(orderbyexpr)" ||
      id == "data.window.percent_rank" || id == "window.percent_rank" ||
      id == "sb.window.percent_rank" || id == "data.aggregate.percent_rank" ||
      id == "aggregate.percent_rank" || id == "sb.aggregate.percent_rank" ||
      id == "percent_rank_within_group" || id == "data.aggregate.percent_rank_within_group" ||
      id == "ordered_set.percent_rank" || id == "hypothetical.percent_rank") {
    return SblrWindowFunctionKind::percent_rank;
  }
  if (id == "cume_dist" || id == "cumulative_distribution" ||
      id == "cume_dist(expr)withingroup(orderbyexpr)" ||
      id == "data.window.cume_dist" || id == "window.cume_dist" ||
      id == "sb.window.cume_dist" || id == "data.aggregate.cume_dist" ||
      id == "aggregate.cume_dist" || id == "sb.aggregate.cume_dist" ||
      id == "cume_dist_within_group" || id == "data.aggregate.cume_dist_within_group" ||
      id == "ordered_set.cume_dist" || id == "hypothetical.cume_dist") {
    return SblrWindowFunctionKind::cume_dist;
  }
  if (id == "ntile" || id == "data.window.ntile" || id == "window.ntile" ||
      id == "sb.window.ntile") {
    return SblrWindowFunctionKind::ntile;
  }
  if (id == "lag" || id == "data.window.lag" || id == "window.lag" ||
      id == "sb.window.lag") {
    return SblrWindowFunctionKind::lag;
  }
  if (id == "lead" || id == "data.window.lead" || id == "window.lead" ||
      id == "sb.window.lead") {
    return SblrWindowFunctionKind::lead;
  }
  if (id == "first_value" || id == "data.window.first_value" || id == "window.first_value" ||
      id == "sb.window.first_value") {
    return SblrWindowFunctionKind::first_value;
  }
  if (id == "last_value" || id == "data.window.last_value" || id == "window.last_value" ||
      id == "sb.window.last_value") {
    return SblrWindowFunctionKind::last_value;
  }
  if (id == "nth_value" || id == "data.window.nth_value" || id == "window.nth_value" ||
      id == "sb.window.nth_value") {
    return SblrWindowFunctionKind::nth_value;
  }
  if (id == "aggregate_as_window" || id == "data.window.aggregate" || id == "window.aggregate") {
    return SblrWindowFunctionKind::aggregate_as_window;
  }
  return SblrWindowFunctionKind::unknown;
}

bool IsSblrWindowFunctionSupported(std::string_view function_id) {
  return ResolveSblrWindowFunctionKind(function_id) != SblrWindowFunctionKind::unknown;
}

SblrResult InitializeSblrAggregateState(std::string_view function_id,
                                        std::string function_uuid,
                                        std::string result_descriptor_id,
                                        const SblrExecutionContext& context,
                                        SblrAggregateWindowState* state) {
  if (state == nullptr) {
    auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_AGGREGATE_STATE_REQUIRED",
                                                context,
                                                "aggregate state pointer is required");
    return MakeSblrFailure(SblrStatusCode::execution_failed, std::string(function_id), std::move(diagnostic));
  }
  const auto kind = ResolveSblrAggregateFunctionKind(function_id);
  if (kind == SblrAggregateFunctionKind::unknown) {
    SblrAggregateWindowState diagnostic_state;
    diagnostic_state.function_id = std::string(function_id);
    diagnostic_state.function_uuid = function_uuid;
    return AggregateFailure(SblrStatusCode::unsupported_feature,
                            std::string(function_id),
                            AggregateDiagnostic("SB_DIAG_AGGREGATE_KIND_UNSUPPORTED",
                                                context,
                                                &diagnostic_state,
                                                "aggregate function id is not supported by the aggregate ABI"));
  }
  *state = SblrAggregateWindowState{};
  state->function_id = std::string(function_id);
  state->function_uuid = std::move(function_uuid);
  state->result_descriptor_id = std::move(result_descriptor_id);
  state->aggregate_kind = kind;
  state->initialized = true;
  state->inverse_supported = false;
  state->recompute_required = true;
  state->memory_bytes = EstimateStateBytes(*state);
  return MakeSblrSuccess(AggregateOperationId(*state));
}

SblrResult UpdateSblrAggregateState(SblrAggregateWindowState* state,
                                    const SblrAggregateUpdateRequest& request) {
  if (state == nullptr || !state->initialized) {
    auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_AGGREGATE_STATE_NOT_INITIALIZED",
                                                request.context,
                                                "aggregate state must be initialized before update");
    return MakeSblrFailure(SblrStatusCode::execution_failed, "sblr.aggregate.update", std::move(diagnostic));
  }
  if (state->finalized) {
    return AggregateFailure(SblrStatusCode::execution_failed,
                            AggregateOperationId(*state),
                            AggregateDiagnostic("SB_DIAG_AGGREGATE_STATE_ALREADY_FINALIZED",
                                                request.context,
                                                state,
                                                "aggregate state cannot be updated after finalize"));
  }
  state->input_count += 1;
  const SblrValue null_value = NullValue({});
  const SblrValue& value = request.values.empty() ? null_value : request.values.front();
  SblrResult status = MakeSblrSuccess(AggregateOperationId(*state));
  switch (state->aggregate_kind) {
    case SblrAggregateFunctionKind::count:
      if (CountIncludesValue(*state, request)) state->non_null_count += 1;
      break;
    case SblrAggregateFunctionKind::sum:
    case SblrAggregateFunctionKind::avg:
    case SblrAggregateFunctionKind::variance_sample:
