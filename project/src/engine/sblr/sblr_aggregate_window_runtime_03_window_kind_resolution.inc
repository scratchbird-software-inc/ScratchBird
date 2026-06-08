// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by sblr_aggregate_window_runtime.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
    case SblrAggregateFunctionKind::variance_population:
    case SblrAggregateFunctionKind::stddev_sample:
    case SblrAggregateFunctionKind::stddev_population:
      status = ApplyNumericInput(state, request, value);
      break;
    case SblrAggregateFunctionKind::corr:
    case SblrAggregateFunctionKind::covariance_population:
    case SblrAggregateFunctionKind::covariance_sample:
    case SblrAggregateFunctionKind::regr_avgx:
    case SblrAggregateFunctionKind::regr_avgy:
    case SblrAggregateFunctionKind::regr_count:
    case SblrAggregateFunctionKind::regr_intercept:
    case SblrAggregateFunctionKind::regr_r2:
    case SblrAggregateFunctionKind::regr_slope:
    case SblrAggregateFunctionKind::regr_sxx:
    case SblrAggregateFunctionKind::regr_sxy:
    case SblrAggregateFunctionKind::regr_syy:
      status = ApplyCorrelationInput(state, request);
      break;
    case SblrAggregateFunctionKind::min:
    case SblrAggregateFunctionKind::max:
      ApplyMinMaxInput(state, value);
      break;
    case SblrAggregateFunctionKind::every:
    case SblrAggregateFunctionKind::any:
      status = ApplyBoolInput(state, request, value);
      break;
    case SblrAggregateFunctionKind::bit_and:
    case SblrAggregateFunctionKind::bit_or:
    case SblrAggregateFunctionKind::bit_xor:
      status = ApplyBitInput(state, request, value);
      break;
    case SblrAggregateFunctionKind::string_agg:
      status = ApplyStringAggregateInput(state, request, value);
      break;
    case SblrAggregateFunctionKind::list_agg:
      status = ApplyListAggregateInput(state, value);
      break;
    case SblrAggregateFunctionKind::binary_agg:
      status = ApplyBinaryAggregateInput(state, value);
      break;
    case SblrAggregateFunctionKind::json_array_agg:
      status = ApplyJsonArrayInput(state, value);
      break;
    case SblrAggregateFunctionKind::json_object_agg:
      status = request.values.size() < 2
                   ? AggregateFailure(SblrStatusCode::execution_failed,
                                      AggregateOperationId(*state),
                                      AggregateDiagnostic("SB_DIAG_AGGREGATE_JSON_OBJECT_VALUE_REQUIRED",
                                                          request.context,
                                                          state,
                                                          "JSON object aggregate requires key and value inputs"))
                   : ApplyJsonObjectInput(state, request, request.values[0], request.values[1]);
      break;
    case SblrAggregateFunctionKind::approx_count_distinct:
      if (!value.is_null) {
        AddDistinctValue(state, ValueText(value));
        state->non_null_count += 1;
      }
      break;
    case SblrAggregateFunctionKind::approx_quantile: {
      status = CaptureQuantileOption(state, request, 1);
      if (!status.ok()) break;
      if (!value.is_null) {
        long double numeric = 0.0L;
        if (!ParseLongDouble(value, &numeric)) {
          status = AggregateFailure(SblrStatusCode::execution_failed,
                                    AggregateOperationId(*state),
                                    AggregateDiagnostic("SB_DIAG_AGGREGATE_NUMERIC_INPUT_REQUIRED",
                                                        request.context,
                                                        state,
                                                        "approximate quantile requires numeric input"));
        } else {
          state->sketch_numeric_values.push_back(numeric);
          state->non_null_count += 1;
        }
      }
      break;
    }
    case SblrAggregateFunctionKind::top_k:
      if (!IsModeAggregateId(state->function_id)) {
        status = CaptureTopKLimitOption(state, request, 1);
        if (!status.ok()) break;
      }
      if (!value.is_null) {
        AddFrequencyValue(state, ValueText(value));
        state->non_null_count += 1;
      }
      break;
    case SblrAggregateFunctionKind::unknown:
      status = AggregateFailure(SblrStatusCode::unsupported_feature,
                                AggregateOperationId(*state),
                                AggregateDiagnostic("SB_DIAG_AGGREGATE_KIND_UNSUPPORTED",
                                                    request.context,
                                                    state,
                                                    "aggregate function id is not supported by the aggregate ABI"));
      break;
  }
  if (!status.ok()) return status;
  return CheckMemoryBudget(state, request);
}

SblrResult MergeSblrAggregateState(SblrAggregateWindowState* target,
                                   const SblrAggregateWindowState& source,
                                   const SblrExecutionContext& context) {
  if (target == nullptr || !target->initialized || !source.initialized) {
    auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_AGGREGATE_MERGE_STATE_REQUIRED",
                                                context,
                                                "aggregate merge requires initialized source and target states");
    return MakeSblrFailure(SblrStatusCode::execution_failed, "sblr.aggregate.merge", std::move(diagnostic));
  }
  if (target->aggregate_kind != source.aggregate_kind || target->function_id != source.function_id) {
    return AggregateFailure(SblrStatusCode::execution_failed,
                            AggregateOperationId(*target),
                            AggregateDiagnostic("SB_DIAG_AGGREGATE_MERGE_KIND_MISMATCH",
                                                context,
                                                target,
                                                "aggregate merge source and target must use the same function"));
  }
  const bool target_was_empty = target->input_count == 0 && target->non_null_count == 0;
  target->input_count += source.input_count;
  switch (target->aggregate_kind) {
    case SblrAggregateFunctionKind::count:
    case SblrAggregateFunctionKind::bit_and:
    case SblrAggregateFunctionKind::bit_or:
    case SblrAggregateFunctionKind::bit_xor:
      if (target->aggregate_kind == SblrAggregateFunctionKind::count) {
        target->non_null_count += source.non_null_count;
      } else if (source.bit_initialized) {
        SblrAggregateUpdateRequest request;
        request.context = context;
        request.values.push_back(Int64Value(static_cast<std::int64_t>(source.bit_accumulator)));
        const auto update = ApplyBitInput(target, request, request.values.front());
        if (!update.ok()) return update;
      }
      break;
    case SblrAggregateFunctionKind::sum:
      target->non_null_count += source.non_null_count;
      target->numeric_sum += source.numeric_sum;
      break;
    case SblrAggregateFunctionKind::avg:
    case SblrAggregateFunctionKind::variance_sample:
    case SblrAggregateFunctionKind::variance_population:
    case SblrAggregateFunctionKind::stddev_sample:
    case SblrAggregateFunctionKind::stddev_population: {
      const std::uint64_t left_count = target->non_null_count;
      const std::uint64_t right_count = source.non_null_count;
      if (right_count == 0) break;
      if (left_count == 0) {
        target->non_null_count = source.non_null_count;
        target->numeric_sum = source.numeric_sum;
        target->numeric_mean = source.numeric_mean;
        target->numeric_m2 = source.numeric_m2;
        break;
      }
      const long double total = static_cast<long double>(left_count + right_count);
      const long double delta = source.numeric_mean - target->numeric_mean;
      target->numeric_m2 += source.numeric_m2 +
                            delta * delta * static_cast<long double>(left_count) *
                                static_cast<long double>(right_count) / total;
      target->numeric_mean += delta * static_cast<long double>(right_count) / total;
      target->numeric_sum += source.numeric_sum;
      target->non_null_count += source.non_null_count;
      break;
    }
    case SblrAggregateFunctionKind::corr:
    case SblrAggregateFunctionKind::covariance_population:
    case SblrAggregateFunctionKind::covariance_sample:
    case SblrAggregateFunctionKind::regr_avgx:
    case SblrAggregateFunctionKind::regr_avgy:
    case SblrAggregateFunctionKind::regr_intercept:
    case SblrAggregateFunctionKind::regr_r2:
    case SblrAggregateFunctionKind::regr_slope:
    case SblrAggregateFunctionKind::regr_sxx:
    case SblrAggregateFunctionKind::regr_sxy:
    case SblrAggregateFunctionKind::regr_syy: {
      const std::uint64_t left_count = target->non_null_count;
      const std::uint64_t right_count = source.non_null_count;
      if (right_count == 0) break;
      if (left_count == 0) {
        target->non_null_count = source.non_null_count;
        target->corr_mean_x = source.corr_mean_x;
        target->corr_mean_y = source.corr_mean_y;
        target->corr_m2_x = source.corr_m2_x;
        target->corr_m2_y = source.corr_m2_y;
        target->corr_comoment = source.corr_comoment;
        break;
      }
      const long double total = static_cast<long double>(left_count + right_count);
      const long double left = static_cast<long double>(left_count);
      const long double right = static_cast<long double>(right_count);
      const long double delta_x = source.corr_mean_x - target->corr_mean_x;
      const long double delta_y = source.corr_mean_y - target->corr_mean_y;
      target->corr_comoment += source.corr_comoment + delta_x * delta_y * left * right / total;
      target->corr_m2_x += source.corr_m2_x + delta_x * delta_x * left * right / total;
      target->corr_m2_y += source.corr_m2_y + delta_y * delta_y * left * right / total;
      target->corr_mean_x += delta_x * right / total;
      target->corr_mean_y += delta_y * right / total;
      target->non_null_count += source.non_null_count;
      break;
    }
    case SblrAggregateFunctionKind::regr_count:
      target->non_null_count += source.non_null_count;
      break;
    case SblrAggregateFunctionKind::min:
    case SblrAggregateFunctionKind::max:
      if (source.non_null_count > 0) {
        ApplyMinMaxInput(target, source.min_value);
        if (source.non_null_count > 1) ApplyMinMaxInput(target, source.max_value);
      }
      break;
    case SblrAggregateFunctionKind::every:
    case SblrAggregateFunctionKind::any:
      target->non_null_count += source.non_null_count;
      target->saw_true = target->saw_true || source.saw_true;
      target->saw_false = target->saw_false || source.saw_false;
      break;
    case SblrAggregateFunctionKind::string_agg:
    case SblrAggregateFunctionKind::list_agg:
      if (target->separator.empty() && !source.separator.empty()) target->separator = source.separator;
      if (!source.text_accumulator.empty()) {
        if (!target->text_accumulator.empty()) target->text_accumulator += target->separator.empty() ? "," : target->separator;
        target->text_accumulator += source.text_accumulator;
      }
      target->sketch_values.insert(target->sketch_values.end(),
                                   source.sketch_values.begin(),
                                   source.sketch_values.end());
      target->listagg_retained_count += source.listagg_retained_count;
      target->non_null_count += source.non_null_count;
      break;
    case SblrAggregateFunctionKind::binary_agg:
      target->binary_accumulator.insert(target->binary_accumulator.end(),
                                        source.binary_accumulator.begin(),
                                        source.binary_accumulator.end());
      target->non_null_count += source.non_null_count;
      break;
    case SblrAggregateFunctionKind::json_array_agg:
    case SblrAggregateFunctionKind::json_object_agg:
      if (!source.text_accumulator.empty()) {
        if (!target->text_accumulator.empty()) target->text_accumulator += ",";
        target->text_accumulator += source.text_accumulator;
      }
      target->non_null_count += source.non_null_count;
      break;
    case SblrAggregateFunctionKind::approx_count_distinct:
      for (const auto& value : source.distinct_values) AddDistinctValue(target, value);
      target->non_null_count += source.non_null_count;
      break;
    case SblrAggregateFunctionKind::approx_quantile:
      if (target_was_empty) target->quantile = source.quantile;
      target->sketch_numeric_values.insert(target->sketch_numeric_values.end(),
                                           source.sketch_numeric_values.begin(),
                                           source.sketch_numeric_values.end());
      target->non_null_count += source.non_null_count;
      break;
    case SblrAggregateFunctionKind::top_k:
      if (target_was_empty) target->top_k_limit = source.top_k_limit;
      for (std::size_t index = 0; index < source.sketch_values.size(); ++index) {
        for (std::uint64_t count = 0; count < source.sketch_counts[index]; ++count) {
          AddFrequencyValue(target, source.sketch_values[index]);
        }
      }
      target->non_null_count += source.non_null_count;
      break;
    case SblrAggregateFunctionKind::unknown:
      break;
  }
  target->memory_bytes = EstimateStateBytes(*target);
  return MakeSblrSuccess(AggregateOperationId(*target));
}

SblrResult FinalizeSblrAggregateState(const SblrAggregateWindowState& state,
                                      const SblrAggregateFinalizeRequest& request) {
  if (!state.initialized) {
    auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_AGGREGATE_STATE_NOT_INITIALIZED",
                                                request.context,
                                                "aggregate state must be initialized before finalize");
    return MakeSblrFailure(SblrStatusCode::execution_failed, AggregateOperationId(state), std::move(diagnostic));
  }
  switch (state.aggregate_kind) {
    case SblrAggregateFunctionKind::count:
      return ScalarResult(state, Int64Value(static_cast<std::int64_t>(state.non_null_count)));
    case SblrAggregateFunctionKind::sum:
      if (state.non_null_count == 0) return ScalarResult(state, NullValue(state.result_descriptor_id.empty() ? "real64" : state.result_descriptor_id));
      return ScalarResult(state, FinalNumericValue(state, state.numeric_sum));
    case SblrAggregateFunctionKind::avg:
      if (state.non_null_count == 0) return ScalarResult(state, NullValue(state.result_descriptor_id.empty() ? "real64" : state.result_descriptor_id));
      return ScalarResult(state, FinalNumericValue(state, state.numeric_sum / static_cast<long double>(state.non_null_count)));
    case SblrAggregateFunctionKind::min:
      if (state.non_null_count == 0) return ScalarResult(state, NullValue(state.result_descriptor_id));
      return ScalarResult(state, state.min_value);
    case SblrAggregateFunctionKind::max:
      if (state.non_null_count == 0) return ScalarResult(state, NullValue(state.result_descriptor_id));
      return ScalarResult(state, state.max_value);
    case SblrAggregateFunctionKind::every:
      if (state.non_null_count == 0) return ScalarResult(state, NullValue("boolean"));
      return ScalarResult(state, BoolValue(!state.saw_false));
    case SblrAggregateFunctionKind::any:
      if (state.non_null_count == 0) return ScalarResult(state, NullValue("boolean"));
