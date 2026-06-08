// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_aggregate_window_runtime.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using scratchbird::engine::sblr::FinalizeSblrAggregateState;
using scratchbird::engine::sblr::InitializeSblrAggregateState;
using scratchbird::engine::sblr::EvaluateSblrWindowFunction;
using scratchbird::engine::sblr::MergeSblrAggregateState;
using scratchbird::engine::sblr::SblrAggregateFinalizeRequest;
using scratchbird::engine::sblr::SblrAggregateOptions;
using scratchbird::engine::sblr::SblrAggregateUpdateRequest;
using scratchbird::engine::sblr::SblrAggregateWindowState;
using scratchbird::engine::sblr::SblrExecutionContext;
using scratchbird::engine::sblr::SblrListAggOverflowMode;
using scratchbird::engine::sblr::SblrStatusCode;
using scratchbird::engine::sblr::SblrValue;
using scratchbird::engine::sblr::SblrValuePayloadKind;
using scratchbird::engine::sblr::SblrWindowFrameRequest;
using scratchbird::engine::sblr::SblrWindowRow;
using scratchbird::engine::sblr::UpdateSblrAggregateState;

SblrValue NullValue(std::string descriptor = {}) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.is_null = true;
  return value;
}

SblrValue Int64Value(std::int64_t input) {
  SblrValue value;
  value.descriptor_id = "int64";
  value.payload_kind = SblrValuePayloadKind::signed_integer;
  value.is_null = false;
  value.has_int64_value = true;
  value.int64_value = input;
  value.encoded_value = std::to_string(input);
  value.text_value = value.encoded_value;
  return value;
}

SblrValue Real64Value(double input) {
  SblrValue value;
  value.descriptor_id = "real64";
  value.payload_kind = SblrValuePayloadKind::real64;
  value.is_null = false;
  value.has_real64_value = true;
  value.real64_value = input;
  value.encoded_value = std::to_string(input);
  value.text_value = value.encoded_value;
  return value;
}

SblrValue TextValue(std::string input) {
  SblrValue value;
  value.descriptor_id = "text";
  value.payload_kind = SblrValuePayloadKind::text;
  value.is_null = false;
  value.encoded_value = std::move(input);
  value.text_value = value.encoded_value;
  return value;
}

bool HasDiagnostic(const scratchbird::engine::sblr::SblrResult& result, std::string_view id) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.diagnostic_id == id) return true;
  }
  return false;
}

scratchbird::engine::sblr::SblrResult RunAggregate(std::string_view function_id,
                                                   std::string result_descriptor,
                                                   const std::vector<SblrValue>& values,
                                                   const SblrValue* option = nullptr,
                                                   const SblrAggregateOptions* aggregate_options = nullptr) {
  SblrAggregateWindowState state;
  SblrExecutionContext context;
  context.database_uuid = "SBSFC-015-runtime-db";
  context.transaction_uuid = "SBSFC-015-runtime-tx";
  context.transaction_context_present = true;

  auto init = InitializeSblrAggregateState(function_id, "SBSFC-015-runtime-function", std::move(result_descriptor), context, &state);
  if (!init.ok()) return init;

  for (const auto& value : values) {
    SblrAggregateUpdateRequest update;
    update.context = context;
    if (aggregate_options != nullptr) update.options = *aggregate_options;
    update.values.push_back(value);
    if (option != nullptr) update.values.push_back(*option);
    auto update_result = UpdateSblrAggregateState(&state, update);
    if (!update_result.ok()) return update_result;
  }

  SblrAggregateFinalizeRequest finalize;
  finalize.context = context;
  if (aggregate_options != nullptr) finalize.options = *aggregate_options;
  return FinalizeSblrAggregateState(state, finalize);
}

scratchbird::engine::sblr::SblrResult RunAggregateRows(std::string_view function_id,
                                                       std::string result_descriptor,
                                                       const std::vector<std::vector<SblrValue>>& rows) {
  SblrAggregateWindowState state;
  SblrExecutionContext context;
  context.database_uuid = "SBSFC-015-runtime-db";
  context.transaction_uuid = "SBSFC-015-runtime-tx";
  context.transaction_context_present = true;

  auto init = InitializeSblrAggregateState(function_id, "SBSFC-015-runtime-function", std::move(result_descriptor), context, &state);
  if (!init.ok()) return init;

  for (const auto& row : rows) {
    SblrAggregateUpdateRequest update;
    update.context = context;
    update.values = row;
    auto update_result = UpdateSblrAggregateState(&state, update);
    if (!update_result.ok()) return update_result;
  }

  SblrAggregateFinalizeRequest finalize;
  finalize.context = context;
  return FinalizeSblrAggregateState(state, finalize);
}

scratchbird::engine::sblr::SblrResult RunMergedAggregateRows(std::string_view function_id,
                                                             std::string result_descriptor,
                                                             const std::vector<std::vector<SblrValue>>& left_rows,
                                                             const std::vector<std::vector<SblrValue>>& right_rows) {
  SblrAggregateWindowState left_state;
  SblrAggregateWindowState right_state;
  SblrExecutionContext context;
  context.database_uuid = "SBSFC-015-runtime-db";
  context.transaction_uuid = "SBSFC-015-runtime-tx";
  context.transaction_context_present = true;

  auto left_init = InitializeSblrAggregateState(function_id, "SBSFC-015-runtime-function", result_descriptor, context, &left_state);
  if (!left_init.ok()) return left_init;
  auto right_init = InitializeSblrAggregateState(function_id, "SBSFC-015-runtime-function", std::move(result_descriptor), context, &right_state);
  if (!right_init.ok()) return right_init;

  for (const auto& row : left_rows) {
    SblrAggregateUpdateRequest update;
    update.context = context;
    update.values = row;
    auto update_result = UpdateSblrAggregateState(&left_state, update);
    if (!update_result.ok()) return update_result;
  }
  for (const auto& row : right_rows) {
    SblrAggregateUpdateRequest update;
    update.context = context;
    update.values = row;
    auto update_result = UpdateSblrAggregateState(&right_state, update);
    if (!update_result.ok()) return update_result;
  }

  auto merge_result = MergeSblrAggregateState(&left_state, right_state, context);
  if (!merge_result.ok()) return merge_result;

  SblrAggregateFinalizeRequest finalize;
  finalize.context = context;
  return FinalizeSblrAggregateState(left_state, finalize);
}

scratchbird::engine::sblr::SblrResult RunMergedAggregate(std::string_view function_id,
                                                         std::string result_descriptor,
                                                         const std::vector<SblrValue>& left_values,
                                                         const std::vector<SblrValue>& right_values,
                                                         const SblrValue* option = nullptr,
                                                         const SblrAggregateOptions* aggregate_options = nullptr) {
  SblrAggregateWindowState left_state;
  SblrAggregateWindowState right_state;
  SblrExecutionContext context;
  context.database_uuid = "SBSFC-015-runtime-db";
  context.transaction_uuid = "SBSFC-015-runtime-tx";
  context.transaction_context_present = true;

  auto left_init = InitializeSblrAggregateState(function_id, "SBSFC-015-runtime-function", result_descriptor, context, &left_state);
  if (!left_init.ok()) return left_init;
  auto right_init = InitializeSblrAggregateState(function_id, "SBSFC-015-runtime-function", std::move(result_descriptor), context, &right_state);
  if (!right_init.ok()) return right_init;

  auto apply_value = [&](SblrAggregateWindowState* state, const SblrValue& value) {
    SblrAggregateUpdateRequest update;
    update.context = context;
    if (aggregate_options != nullptr) update.options = *aggregate_options;
    update.values.push_back(value);
    if (option != nullptr) update.values.push_back(*option);
    return UpdateSblrAggregateState(state, update);
  };

  for (const auto& value : left_values) {
    auto update_result = apply_value(&left_state, value);
    if (!update_result.ok()) return update_result;
  }
  for (const auto& value : right_values) {
    auto update_result = apply_value(&right_state, value);
    if (!update_result.ok()) return update_result;
  }

  auto merge_result = MergeSblrAggregateState(&left_state, right_state, context);
  if (!merge_result.ok()) return merge_result;

  SblrAggregateFinalizeRequest finalize;
  finalize.context = context;
  if (aggregate_options != nullptr) finalize.options = *aggregate_options;
  return FinalizeSblrAggregateState(left_state, finalize);
}

scratchbird::engine::sblr::SblrResult RunWindow(std::string function_id,
                                                const std::vector<SblrValue>& values,
                                                std::size_t current_row_index,
                                                std::size_t frame_start_index,
                                                std::size_t frame_end_exclusive,
                                                std::int64_t offset = 1,
                                                std::uint64_t ntile_bucket_count = 1,
                                                std::uint64_t nth = 1,
                                                const SblrValue* default_value = nullptr,
                                                const std::vector<std::uint64_t>& peer_groups = {}) {
  SblrWindowFrameRequest request;
  request.context.database_uuid = "SBSFC-015-runtime-db";
  request.context.transaction_uuid = "SBSFC-015-runtime-tx";
  request.context.transaction_context_present = true;
  request.function_id = std::move(function_id);
  request.function_uuid = "SBSFC-015-runtime-window-function";
  request.current_row_index = current_row_index;
  request.frame_start_index = frame_start_index;
  request.frame_end_exclusive = frame_end_exclusive;
  request.offset = offset;
  request.ntile_bucket_count = ntile_bucket_count;
  request.nth = nth;
  if (default_value != nullptr) {
    request.default_value = *default_value;
    request.default_value_present = true;
  }
  for (std::size_t index = 0; index < values.size(); ++index) {
    SblrWindowRow row;
    row.values.push_back(values[index]);
    if (index < peer_groups.size()) row.peer_group = peer_groups[index];
    request.rows.push_back(std::move(row));
  }
  return EvaluateSblrWindowFunction(request);
}

bool ExpectOkScalar(const scratchbird::engine::sblr::SblrResult& result, std::string_view case_id) {
  if (!result.ok() || result.scalar_values.size() != 1) {
    std::cerr << case_id << ": expected one successful scalar result\n";
    return false;
  }
  return true;
}

bool ExpectInt64(std::string_view case_id,
                 const scratchbird::engine::sblr::SblrResult& result,
                 std::int64_t expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || !value.has_int64_value || value.int64_value != expected) {
    std::cerr << case_id << ": expected int64 " << expected << ", got " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectReal64(std::string_view case_id,
                  const scratchbird::engine::sblr::SblrResult& result,
                  double expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || !value.has_real64_value || std::fabs(value.real64_value - expected) > 1e-12) {
    std::cerr << case_id << ": expected real64 " << expected << ", got " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectBool(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                bool expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  const std::int64_t expected_int = expected ? 1 : 0;
  if (value.is_null || value.payload_kind != SblrValuePayloadKind::boolean ||
      !value.has_int64_value || value.int64_value != expected_int) {
    std::cerr << case_id << ": expected boolean " << (expected ? "TRUE" : "FALSE")
              << ", got " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectNull(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                std::string_view expected_descriptor) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (!value.is_null || value.descriptor_id != expected_descriptor) {
    std::cerr << case_id << ": expected NULL " << expected_descriptor
              << ", got " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectText(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                std::string_view expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.text_value != expected) {
    std::cerr << case_id << ": expected text " << expected << ", got " << value.text_value << "\n";
    return false;
  }
  return true;
}

bool ExpectList(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                std::string_view expected_descriptor,
                std::string_view expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != expected_descriptor ||
      value.payload_kind != SblrValuePayloadKind::descriptor_payload ||
      value.text_value != expected) {
    std::cerr << case_id << ": expected list " << expected_descriptor << " "
              << expected << ", got " << value.descriptor_id << " "
              << value.text_value << "\n";
    return false;
  }
  return true;
}

bool ExpectFailure(std::string_view case_id,
                   const scratchbird::engine::sblr::SblrResult& result,
                   std::string_view diagnostic_id) {
  if (result.status == SblrStatusCode::ok || !HasDiagnostic(result, diagnostic_id)) {
    std::cerr << case_id << ": expected diagnostic " << diagnostic_id << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  bool ok = true;
  const SblrValue top_two = Int64Value(2);
  const SblrValue top_zero = Int64Value(0);
  const SblrValue separator_pipe = TextValue("|");
  const SblrValue fraction_quarter = Real64Value(0.25);
  const SblrValue fraction_three_quarters = Real64Value(0.75);
  const SblrValue invalid_fraction = Real64Value(1.5);
  SblrAggregateOptions listagg_overflow_error;
  listagg_overflow_error.listagg_overflow_mode = SblrListAggOverflowMode::error;
  listagg_overflow_error.listagg_max_output_bytes = 12;
  SblrAggregateOptions listagg_truncate_with_count;
  listagg_truncate_with_count.listagg_overflow_mode = SblrListAggOverflowMode::truncate;
  listagg_truncate_with_count.listagg_max_output_bytes = 12;
  listagg_truncate_with_count.listagg_truncation_indicator = "...";
  listagg_truncate_with_count.listagg_with_count = true;
  SblrAggregateOptions listagg_truncate_without_count;
  listagg_truncate_without_count.listagg_overflow_mode = SblrListAggOverflowMode::truncate;
  listagg_truncate_without_count.listagg_max_output_bytes = 14;
  listagg_truncate_without_count.listagg_truncation_indicator = "...";
  listagg_truncate_without_count.listagg_with_count = false;
  SblrAggregateOptions count_star_options;
  count_star_options.count_nulls = true;

  ok = ExpectInt64("count_star_surface",
                   RunAggregate("count(*)|count([DISTINCT]expr)", "int64",
                                {Int64Value(10), Int64Value(20), NullValue("int64")},
                                nullptr,
                                &count_star_options),
                   3) && ok;

  ok = ExpectInt64("count_name_non_null",
                   RunAggregate("count", "int64",
                                {Int64Value(10), NullValue("int64"), Int64Value(30)}),
                   2) && ok;

  ok = ExpectInt64("count_canonical",
                   RunAggregate("sb.aggregate.count", "int64",
                                {TextValue("a"), TextValue("b")}),
                   2) && ok;

  ok = ExpectReal64("avg_canonical",
                    RunAggregate("sb.aggregate.avg", "real64",
                                 {Int64Value(10), Int64Value(20), NullValue("real64")}),
                    15.0) && ok;

  ok = ExpectReal64("avg_name",
                    RunAggregate("avg", "real64",
                                 {Int64Value(7), Int64Value(13)}),
                    10.0) && ok;

  ok = ExpectInt64("min_expr",
                   RunAggregate("min(expr)", "int64",
                                {Int64Value(12), Int64Value(7), NullValue("int64")}),
                   7) && ok;

  ok = ExpectInt64("min_canonical",
                   RunAggregate("sb.aggregate.min", "int64",
                                {Int64Value(12), Int64Value(13)}),
                   12) && ok;

  ok = ExpectInt64("min_name",
                   RunAggregate("min", "int64",
                                {Int64Value(5), Int64Value(9)}),
                   5) && ok;

  ok = ExpectInt64("max_name",
                   RunAggregate("max", "int64",
                                {Int64Value(5), Int64Value(9)}),
                   9) && ok;

  ok = ExpectInt64("max_expr",
                   RunAggregate("max(expr)", "int64",
                                {Int64Value(12), Int64Value(7), NullValue("int64")}),
                   12) && ok;

  ok = ExpectInt64("max_canonical",
                   RunAggregate("sb.aggregate.max", "int64",
                                {Int64Value(12), Int64Value(13)}),
                   13) && ok;

  ok = ExpectBool("bool_or_signature",
                  RunAggregate("bool_or(boolean)", "boolean",
                               {TextValue("false"), NullValue("boolean"), TextValue("true")}),
                  true) && ok;

  ok = ExpectBool("bool_or_canonical",
                  RunAggregate("sb.aggregate.bool_or", "boolean",
                               {TextValue("false"), Int64Value(0)}),
                  false) && ok;

  ok = ExpectNull("bool_or_name_empty",
                  RunAggregate("bool_or", "boolean", {NullValue("boolean")}),
                  "boolean") && ok;

  ok = ExpectBool("bool_and_name",
                  RunAggregate("bool_and", "boolean",
                               {TextValue("true"), TextValue("true"), NullValue("boolean")}),
                  true) && ok;

  ok = ExpectBool("bool_and_signature",
                  RunAggregate("bool_and(boolean)", "boolean",
                               {TextValue("true"), TextValue("false"), NullValue("boolean")}),
                  false) && ok;

  ok = ExpectNull("bool_and_canonical_empty",
                  RunAggregate("sb.aggregate.bool_and", "boolean", {NullValue("boolean")}),
                  "boolean") && ok;

  ok = ExpectInt64("approx_count_distinct",
                   RunAggregate("approx_count_distinct", "int64",
                                {TextValue("alpha"), TextValue("beta"), TextValue("alpha"), NullValue("text")}),
                   2) && ok;

  ok = ExpectText("approx_top_k",
                  RunAggregate("approx_top_k", "json",
                               {TextValue("b"), TextValue("a"), TextValue("b"), TextValue("c"), TextValue("a"), TextValue("b")},
                               &top_two),
                  R"([{"value":"b","count":3},{"value":"a","count":2}])") && ok;

  ok = ExpectText("top_k_merge_preserves_source_limit",
                  RunMergedAggregate("approx_top_k", "json",
                                     {},
                                     {TextValue("b"), TextValue("a"), TextValue("b"),
                                      TextValue("c"), TextValue("a"), TextValue("b")},
                                     &top_two),
                  R"([{"value":"b","count":3},{"value":"a","count":2}])") && ok;

  ok = ExpectReal64("approx_median_even",
                    RunAggregate("approx_median", "real64",
                                 {Int64Value(10), Int64Value(20), Int64Value(30), Int64Value(40)}),
                    25.0) && ok;

  ok = ExpectReal64("approx_percentile_cont",
                    RunAggregate("approx_percentile_cont", "real64",
                                 {Int64Value(10), Int64Value(20), Int64Value(30), Int64Value(40)},
                                 &fraction_three_quarters),
                    32.5) && ok;

  ok = ExpectReal64("approx_percentile_disc",
                    RunAggregate("approx_percentile_disc", "real64",
                                 {Int64Value(10), Int64Value(20), Int64Value(30), Int64Value(40)},
                                 &fraction_three_quarters),
                    30.0) && ok;

  ok = ExpectReal64("percentile_merge_preserves_source_fraction",
                    RunMergedAggregate("percentile_cont", "real64",
                                       {},
                                       {Int64Value(10), Int64Value(20), Int64Value(30), Int64Value(40)},
                                       &fraction_three_quarters),
                    32.5) && ok;

  ok = ExpectReal64("percentile_cont_exact",
                    RunAggregate("percentile_cont", "real64",
                                 {Int64Value(10), Int64Value(20), Int64Value(30), Int64Value(40)},
                                 &fraction_quarter),
                    17.5) && ok;

  ok = ExpectReal64("percentile_disc_exact",
                    RunAggregate("percentile_disc", "real64",
                                 {Int64Value(10), Int64Value(20), Int64Value(30), Int64Value(40)},
                                 &fraction_quarter),
                    10.0) && ok;

  ok = ExpectReal64("mode_frequency",
                    RunAggregate("mode", "real64",
                                 {Int64Value(5), Int64Value(4), Int64Value(5), Int64Value(4)}),
                    4.0) && ok;

  ok = ExpectText("listagg_separator",
                  RunAggregate("LISTAGG", "text",
                               {TextValue("a"), NullValue("text"), TextValue("b"), TextValue("c")},
                               &separator_pipe),
                  "a|b|c") && ok;

  ok = ExpectFailure("listagg_on_overflow_error",
                     RunAggregate("LISTAGG(expr[,sep][ONOVERFLOW...])WITHINGROUP(ORDERBY...)", "text",
                                  {TextValue("north"), TextValue("east"), TextValue("south")},
                                  &separator_pipe,
                                  &listagg_overflow_error),
                     "SB_DIAG_AGGREGATE_LISTAGG_OVERFLOW") && ok;

  ok = ExpectText("listagg_on_overflow_truncate_with_count",
                  RunAggregate("LISTAGG(expr[,sep][ONOVERFLOW...])WITHINGROUP(ORDERBY...)", "text",
                               {TextValue("north"), TextValue("east"), TextValue("south")},
                               &separator_pipe,
                               &listagg_truncate_with_count),
                  "north|...(2)") && ok;

  ok = ExpectText("listagg_on_overflow_truncate_without_count",
                  RunAggregate("LISTAGG(expr[,sep][ONOVERFLOW...])WITHINGROUP(ORDERBY...)", "text",
                               {TextValue("north"), TextValue("east"), TextValue("south")},
                               &separator_pipe,
                               &listagg_truncate_without_count),
                  "north|east|...") && ok;

  ok = ExpectText("listagg_merge_retains_element_boundaries",
                  RunMergedAggregate("LISTAGG(expr[,sep][ONOVERFLOW...])WITHINGROUP(ORDERBY...)", "text",
                                     {TextValue("north"), TextValue("east")},
                                     {TextValue("south")},
                                     &separator_pipe,
                                     &listagg_truncate_with_count),
                  "north|...(2)") && ok;

  ok = ExpectBool("every_all_true",
                  RunAggregate("sb.aggregate.every", "boolean",
                               {TextValue("true"), NullValue("boolean"), TextValue("yes"), Int64Value(1)}),
                  true) && ok;

  ok = ExpectBool("every_false_present",
                  RunAggregate("every(boolean)", "boolean",
                               {TextValue("true"), TextValue("false"), NullValue("boolean")}),
                  false) && ok;

  ok = ExpectNull("every_empty",
                  RunAggregate("every", "boolean", {}),
                  "boolean") && ok;

  ok = ExpectReal64("stddev_sample",
                    RunAggregate("sb.aggregate.stddev", "real64",
                                 {Int64Value(2), Int64Value(4), Int64Value(4), Int64Value(4),
                                  Int64Value(5), Int64Value(5), Int64Value(7), Int64Value(9),
                                  NullValue("real64")}),
                    2.138089935299395) && ok;

  ok = ExpectReal64("stddev_numeric_signature",
                    RunAggregate("stddev(numeric)", "real64",
                                 {Int64Value(1), Int64Value(2), Int64Value(3)}),
                    1.0) && ok;

  ok = ExpectNull("stddev_singleton",
                  RunAggregate("stddev", "real64", {Int64Value(1), NullValue("real64")}),
                  "real64") && ok;

  ok = ExpectReal64("variance_sample",
                    RunAggregate("sb.aggregate.variance", "real64",
                                 {Int64Value(2), Int64Value(4), Int64Value(4), Int64Value(4),
                                  Int64Value(5), Int64Value(5), Int64Value(7), Int64Value(9),
                                  NullValue("real64")}),
                    4.571428571428571) && ok;

  ok = ExpectReal64("variance_numeric_signature",
                    RunAggregate("variance(numeric)", "real64",
                                 {Int64Value(1), Int64Value(2), Int64Value(3)}),
                    1.0) && ok;

  ok = ExpectNull("variance_empty",
                  RunAggregate("variance", "real64", {}),
                  "real64") && ok;

  ok = ExpectReal64("SBSQL-D4A54D6879E1-stddev_pop",
                    RunAggregate("stddev_pop", "real64",
                                 {Int64Value(2), Int64Value(4), Int64Value(4), Int64Value(4),
                                  Int64Value(5), Int64Value(5), Int64Value(7), Int64Value(9),
                                  NullValue("real64")}),
                    2.0) && ok;

  ok = ExpectReal64("SBSQL-46D54006C21A-stddev_pop_numeric",
                    RunAggregate("stddev_pop(numeric)", "real64",
                                 {Int64Value(1), Int64Value(2), Int64Value(3)}),
                    0.81649658092772603) && ok;

  ok = ExpectReal64("SBSQL-1B1392E72628-stddev_pop_canonical_singleton",
                    RunAggregate("sb.aggregate.stddev_pop", "real64",
                                 {Int64Value(1), NullValue("real64")}),
                    0.0) && ok;

  ok = ExpectNull("stddev_pop_empty",
                  RunAggregate("sb.aggregate.stddev_pop", "real64", {}),
                  "real64") && ok;

  ok = ExpectReal64("SBSQL-1926F7E782F3-variance_pop",
                    RunAggregate("variance_pop", "real64",
                                 {Int64Value(2), Int64Value(4), Int64Value(4), Int64Value(4),
                                  Int64Value(5), Int64Value(5), Int64Value(7), Int64Value(9),
                                  NullValue("real64")}),
                    4.0) && ok;

  ok = ExpectReal64("SBSQL-7CBEA5B27835-variance_pop_numeric",
                    RunAggregate("variance_pop(numeric)", "real64",
                                 {Int64Value(1), Int64Value(2), Int64Value(3)}),
                    2.0 / 3.0) && ok;

  ok = ExpectReal64("SBSQL-F89AE449F324-variance_pop_canonical_singleton",
                    RunAggregate("sb.aggregate.variance_pop", "real64",
                                 {Int64Value(1), NullValue("real64")}),
                    0.0) && ok;

  ok = ExpectNull("variance_pop_empty",
                  RunAggregate("sb.aggregate.variance_pop", "real64", {}),
                  "real64") && ok;

  ok = ExpectReal64("corr_positive",
                    RunAggregateRows("sb.aggregate.corr", "real64",
                                     {{Int64Value(1), Int64Value(1)},
                                      {Int64Value(2), Int64Value(2)},
                                      {Int64Value(3), Int64Value(3)},
                                      {NullValue("real64"), Int64Value(4)},
                                      {Int64Value(5), NullValue("real64")}}),
                    1.0) && ok;

  ok = ExpectReal64("corr_y_x_signature",
                    RunAggregateRows("corr(y,x)", "real64",
                                     {{Int64Value(1), Int64Value(3)},
                                      {Int64Value(2), Int64Value(2)},
                                      {Int64Value(3), Int64Value(1)},
                                      {Int64Value(4), Int64Value(0)}}),
                    -1.0) && ok;

  ok = ExpectReal64("corr_merge_state",
                    RunMergedAggregateRows("corr", "real64",
                                           {{Int64Value(1), Int64Value(1)},
                                            {Int64Value(2), Int64Value(2)}},
                                           {{Int64Value(3), Int64Value(3)},
                                            {Int64Value(4), Int64Value(4)}}),
                    1.0) && ok;

  ok = ExpectNull("corr_zero_variance",
                  RunAggregateRows("corr", "real64",
                                   {{Int64Value(1), Int64Value(5)},
                                    {Int64Value(2), Int64Value(5)},
                                    {Int64Value(3), Int64Value(5)}}),
                  "real64") && ok;

  ok = ExpectFailure("every_invalid_input",
                     RunAggregate("every", "boolean", {TextValue("maybe")}),
                     "SB_DIAG_AGGREGATE_BOOLEAN_INPUT_REQUIRED") && ok;

  ok = ExpectFailure("stddev_invalid_input",
                     RunAggregate("stddev", "real64", {TextValue("not-a-number")}),
                     "SB_DIAG_AGGREGATE_NUMERIC_INPUT_REQUIRED") && ok;

  ok = ExpectFailure("variance_invalid_input",
                     RunAggregate("variance", "real64", {TextValue("not-a-number")}),
                     "SB_DIAG_AGGREGATE_NUMERIC_INPUT_REQUIRED") && ok;

  ok = ExpectFailure("corr_missing_pair",
                     RunAggregateRows("corr", "real64", {{Int64Value(1)}}),
                     "SB_DIAG_AGGREGATE_CORR_PAIR_REQUIRED") && ok;

  ok = ExpectFailure("corr_invalid_input",
                     RunAggregateRows("corr", "real64", {{TextValue("not-a-number"), Int64Value(1)}}),
                     "SB_DIAG_AGGREGATE_NUMERIC_INPUT_REQUIRED") && ok;

  ok = ExpectReal64("SBSQL-53E3A168AD26-stddev_samp",
                    RunAggregate("stddev_samp", "real64",
                                 {Int64Value(2), Int64Value(4), Int64Value(4), Int64Value(4),
                                  Int64Value(5), Int64Value(5), Int64Value(7), Int64Value(9),
                                  NullValue("real64")}),
                    2.138089935299395) && ok;

  ok = ExpectReal64("SBSQL-D155F7EC1FE1-stddev_samp_numeric",
                    RunAggregate("stddev_samp(numeric)", "real64",
                                 {Int64Value(1), Int64Value(2), Int64Value(3)}),
                    1.0) && ok;

  ok = ExpectNull("stddev_samp_singleton",
                  RunAggregate("sb.aggregate.stddev_samp", "real64", {Int64Value(1), NullValue("real64")}),
                  "real64") && ok;

  ok = ExpectReal64("SBSQL-4AF99A06B193-variance_samp",
                    RunAggregate("variance_samp", "real64",
                                 {Int64Value(2), Int64Value(4), Int64Value(4), Int64Value(4),
                                  Int64Value(5), Int64Value(5), Int64Value(7), Int64Value(9),
                                  NullValue("real64")}),
                    4.571428571428571) && ok;

  ok = ExpectReal64("SBSQL-482B2C54BAF1-variance_samp_numeric",
                    RunAggregate("variance_samp(numeric)", "real64",
                                 {Int64Value(1), Int64Value(2), Int64Value(3)}),
                    1.0) && ok;

  ok = ExpectNull("variance_samp_empty",
                  RunAggregate("sb.aggregate.variance_samp", "real64", {}),
                  "real64") && ok;

  ok = ExpectReal64("SBSQL-7D77C331D16C-covar_pop",
                    RunAggregateRows("covar_pop", "real64",
                                     {{Int64Value(1), Int64Value(1)},
                                      {Int64Value(2), Int64Value(2)},
                                      {Int64Value(3), Int64Value(3)},
                                      {NullValue("real64"), Int64Value(4)},
                                      {Int64Value(5), NullValue("real64")}}),
                    2.0 / 3.0) && ok;

  ok = ExpectReal64("SBSQL-E662CB944FC2-covar_pop_y_x",
                    RunAggregateRows("covar_pop(y,x)", "real64",
                                     {{Int64Value(1), Int64Value(3)},
                                      {Int64Value(2), Int64Value(2)},
                                      {Int64Value(3), Int64Value(1)},
                                      {Int64Value(4), Int64Value(0)}}),
                    -1.25) && ok;

  ok = ExpectReal64("covar_pop_singleton",
                    RunAggregateRows("sb.aggregate.covar_pop", "real64",
                                     {{Int64Value(42), Int64Value(7)},
                                      {NullValue("real64"), Int64Value(8)}}),
                    0.0) && ok;

  ok = ExpectReal64("SBSQL-5B5757128C3F-covar_samp",
                    RunAggregateRows("covar_samp", "real64",
                                     {{Int64Value(1), Int64Value(1)},
                                      {Int64Value(2), Int64Value(2)},
                                      {Int64Value(3), Int64Value(3)},
                                      {NullValue("real64"), Int64Value(4)},
                                      {Int64Value(5), NullValue("real64")}}),
                    1.0) && ok;

  ok = ExpectReal64("SBSQL-FC78A3D1CF86-covar_samp_y_x",
                    RunAggregateRows("covar_samp(y,x)", "real64",
                                     {{Int64Value(1), Int64Value(3)},
                                      {Int64Value(2), Int64Value(2)},
                                      {Int64Value(3), Int64Value(1)},
                                      {Int64Value(4), Int64Value(0)}}),
                    -5.0 / 3.0) && ok;

  ok = ExpectNull("covar_samp_singleton",
                  RunAggregateRows("sb.aggregate.covar_samp", "real64",
                                   {{Int64Value(42), Int64Value(7)},
                                    {NullValue("real64"), Int64Value(8)}}),
                  "real64") && ok;

  ok = ExpectReal64("covar_samp_merge_state",
                    RunMergedAggregateRows("covar_samp", "real64",
                                           {{Int64Value(1), Int64Value(1)},
                                            {Int64Value(2), Int64Value(2)}},
                                           {{Int64Value(3), Int64Value(3)},
                                            {Int64Value(4), Int64Value(4)}}),
                    5.0 / 3.0) && ok;

  ok = ExpectInt64("SBSQL-1BBEB1E43F45-regr_count",
                   RunAggregateRows("regr_count", "int64",
                                    {{Int64Value(1), Int64Value(1)},
                                     {Int64Value(2), Int64Value(2)},
                                     {Int64Value(3), Int64Value(3)},
                                     {NullValue("real64"), Int64Value(4)},
                                     {Int64Value(5), NullValue("real64")}}),
                   3) && ok;

  ok = ExpectInt64("SBSQL-3C0839E8B792-regr_count_y_x",
                   RunAggregateRows("regr_count(y,x)", "int64",
                                    {{Int64Value(1), Int64Value(3)},
                                     {Int64Value(2), Int64Value(2)},
                                     {Int64Value(3), Int64Value(1)},
                                     {Int64Value(4), Int64Value(0)}}),
                   4) && ok;

  ok = ExpectInt64("regr_count_empty",
                   RunAggregateRows("sb.aggregate.regr_count", "int64",
                                    {{NullValue("real64"), Int64Value(7)},
                                     {Int64Value(8), NullValue("real64")}}),
                   0) && ok;

  const std::vector<std::vector<SblrValue>> regr_positive_rows = {
      {Int64Value(3), Int64Value(1)},
      {Int64Value(5), Int64Value(2)},
      {Int64Value(7), Int64Value(3)},
      {NullValue("real64"), Int64Value(4)},
      {Int64Value(9), NullValue("real64")},
  };
  const std::vector<std::vector<SblrValue>> regr_negative_rows = {
      {Int64Value(8), Int64Value(1)},
      {Int64Value(6), Int64Value(2)},
      {Int64Value(4), Int64Value(3)},
      {Int64Value(2), Int64Value(4)},
  };
  const std::vector<std::vector<SblrValue>> regr_singleton_rows = {
      {Int64Value(42), Int64Value(7)},
      {NullValue("real64"), Int64Value(8)},
  };
  const std::vector<std::vector<SblrValue>> regr_constant_y_rows = {
      {Int64Value(5), Int64Value(1)},
      {Int64Value(5), Int64Value(2)},
      {Int64Value(5), Int64Value(3)},
  };

  ok = ExpectReal64("SBSQL-7102C019D2CF-regr_avgx",
                    RunAggregateRows("regr_avgx", "real64", regr_positive_rows),
                    2.0) && ok;

  ok = ExpectReal64("SBSQL-54324247868A-regr_avgx_y_x",
                    RunAggregateRows("regr_avgx(y,x)", "real64", regr_negative_rows),
                    2.5) && ok;

  ok = ExpectReal64("SBSQL-DF6313DE4B56-regr_avgy",
                    RunAggregateRows("regr_avgy", "real64", regr_positive_rows),
                    5.0) && ok;

  ok = ExpectReal64("SBSQL-189983EF2867-regr_avgy_y_x",
                    RunAggregateRows("regr_avgy(y,x)", "real64", regr_negative_rows),
                    5.0) && ok;

  ok = ExpectReal64("SBSQL-431925B5EC67-regr_intercept",
                    RunAggregateRows("regr_intercept", "real64", regr_positive_rows),
                    1.0) && ok;

  ok = ExpectReal64("SBSQL-8F9FD6E0E1B0-regr_intercept_y_x",
                    RunAggregateRows("regr_intercept(y,x)", "real64", regr_negative_rows),
                    10.0) && ok;

  ok = ExpectReal64("SBSQL-794AAFE26F38-regr_r2",
                    RunAggregateRows("regr_r2", "real64", regr_positive_rows),
                    1.0) && ok;

  ok = ExpectReal64("SBSQL-BE43021856AE-regr_r2_y_x",
                    RunAggregateRows("regr_r2(y,x)", "real64", regr_negative_rows),
                    1.0) && ok;

  ok = ExpectReal64("regr_r2_constant_y",
                    RunAggregateRows("sb.aggregate.regr_r2", "real64", regr_constant_y_rows),
                    1.0) && ok;

  ok = ExpectReal64("SBSQL-559DFA580089-regr_slope",
                    RunAggregateRows("regr_slope", "real64", regr_positive_rows),
                    2.0) && ok;

  ok = ExpectReal64("SBSQL-BB7BA14B2666-regr_slope_y_x",
                    RunAggregateRows("regr_slope(y,x)", "real64", regr_negative_rows),
                    -2.0) && ok;

  ok = ExpectReal64("SBSQL-C77EA68C577B-regr_sxx",
                    RunAggregateRows("regr_sxx", "real64", regr_positive_rows),
                    2.0) && ok;

  ok = ExpectReal64("SBSQL-D291129F3FD3-regr_sxx_y_x",
                    RunAggregateRows("regr_sxx(y,x)", "real64", regr_negative_rows),
                    5.0) && ok;

  ok = ExpectReal64("regr_sxx_singleton",
                    RunAggregateRows("sb.aggregate.regr_sxx", "real64", regr_singleton_rows),
                    0.0) && ok;

  ok = ExpectReal64("SBSQL-61641209CF6B-regr_sxy",
                    RunAggregateRows("regr_sxy", "real64", regr_positive_rows),
                    4.0) && ok;

  ok = ExpectReal64("SBSQL-1F514A240E49-regr_sxy_y_x",
                    RunAggregateRows("regr_sxy(y,x)", "real64", regr_negative_rows),
                    -10.0) && ok;

  ok = ExpectReal64("SBSQL-1D81FEFFF22A-regr_syy",
                    RunAggregateRows("regr_syy", "real64", regr_positive_rows),
                    8.0) && ok;

  ok = ExpectReal64("SBSQL-9C9BD835BEAF-regr_syy_y_x",
                    RunAggregateRows("regr_syy(y,x)", "real64", regr_negative_rows),
                    20.0) && ok;

  ok = ExpectNull("regr_slope_singleton",
                  RunAggregateRows("sb.aggregate.regr_slope", "real64", regr_singleton_rows),
                  "real64") && ok;

  ok = ExpectFailure("covar_pop_missing_pair",
                     RunAggregateRows("covar_pop", "real64", {{Int64Value(1)}}),
                     "SB_DIAG_AGGREGATE_CORR_PAIR_REQUIRED") && ok;

  ok = ExpectFailure("regr_count_invalid_input",
                     RunAggregateRows("regr_count", "int64", {{TextValue("not-a-number"), Int64Value(1)}}),
                     "SB_DIAG_AGGREGATE_NUMERIC_INPUT_REQUIRED") && ok;

  ok = ExpectText("json_agg_values",
                  RunAggregate("json_agg", "json",
                               {Int64Value(1), TextValue("two"), NullValue("text")}),
                  R"([1,"two",null])") && ok;

  ok = ExpectText("json_agg_orderby_signature",
                  RunAggregate("json_agg(expr[ORDERBY...])", "json",
                               {TextValue("north"), TextValue("east")}),
                  R"(["north","east"])") && ok;

  ok = ExpectText("json_agg_canonical",
                  RunAggregate("sb.aggregate.json_agg", "json",
                               {TextValue("alpha"), NullValue("text")}),
                  R"(["alpha",null])") && ok;

  ok = ExpectNull("json_agg_empty",
                  RunAggregate("sb.aggregate.json_agg", "json", {}),
                  "json") && ok;

  ok = ExpectList("array_agg_values_include_null",
                  RunAggregate("array_agg", "list<text nullable>",
                               {TextValue("north"), NullValue("text"), TextValue("east")}),
                  "list<text nullable>",
                  "list[text:north;NULL;text:east]") && ok;

  ok = ExpectList("array_agg_orderby_signature",
                  RunAggregate("array_agg(expr[ORDERBY...])", "list<text nullable>",
                               {TextValue("north"), TextValue("east")}),
                  "list<text nullable>",
                  "list[text:north;text:east]") && ok;

  ok = ExpectList("array_agg_canonical",
                  RunAggregate("sb.aggregate.array_agg", "list<int64 nullable>",
                               {Int64Value(7), NullValue("int64"), Int64Value(9)}),
                  "list<int64 nullable>",
                  "list[int64:7;NULL;int64:9]") && ok;

  ok = ExpectNull("array_agg_empty",
                  RunAggregate("sb.aggregate.array_agg", "list<any nullable>", {}),
                  "list<any nullable>") && ok;

  ok = ExpectText("json_object_agg_pairs",
                  RunAggregateRows("json_object_agg(key,value)", "json",
                                   {{TextValue("first"), Int64Value(1)},
                                    {TextValue("second"), TextValue("two")}}),
                  R"({"first":1,"second":"two"})") && ok;

  ok = ExpectText("json_object_agg_duplicate_last_key_wins",
                  RunAggregateRows("sb.aggregate.json_object_agg", "json",
                                   {{TextValue("dup"), Int64Value(1)},
                                    {TextValue("other"), NullValue("int64")},
                                    {TextValue("dup"), Int64Value(2)}}),
                  R"({"other":null,"dup":2})") && ok;

  ok = ExpectFailure("json_object_agg_null_key",
                     RunAggregateRows("json_object_agg", "json",
                                      {{NullValue("text"), TextValue("value")}}),
                     "SB_DIAG_AGGREGATE_JSON_OBJECT_KEY_REQUIRED") && ok;

  const std::vector<SblrValue> window_values = {
      TextValue("north"),
      TextValue("east"),
      TextValue("south"),
      TextValue("west"),
      TextValue("zenith"),
  };
  const std::vector<SblrValue> peer_rank_values = {
      Int64Value(10),
      Int64Value(20),
      Int64Value(20),
      Int64Value(30),
      Int64Value(40),
  };
  const std::vector<std::uint64_t> peer_groups = {1, 2, 2, 3, 4};
  const SblrValue hypothetical_25 = Int64Value(25);
  const SblrValue navigation_default = TextValue("fallback");

  ok = ExpectInt64("SBSQL-E33776097240-window-rank-peer",
                   RunWindow("sb.window.rank", peer_rank_values, 2, 0, peer_rank_values.size(),
                             1, 1, 1, nullptr, peer_groups),
                   2) && ok;
  ok = ExpectInt64("SBSQL-73159B932B38-window-rank-canonical-alias",
                   RunWindow("sb.window.rank", peer_rank_values, 2, 0, peer_rank_values.size(),
                             1, 1, 1, nullptr, peer_groups),
                   2) && ok;

  ok = ExpectInt64("SBSQL-E1BCEE3D98B7-window-dense-rank-peer",
                   RunWindow("sb.window.dense_rank", peer_rank_values, 2, 0, peer_rank_values.size(),
                             1, 1, 1, nullptr, peer_groups),
                   2) && ok;
  ok = ExpectInt64("SBSQL-E7B5D653D886-window-dense-rank-canonical-alias",
                   RunWindow("sb.window.dense_rank", peer_rank_values, 2, 0, peer_rank_values.size(),
                             1, 1, 1, nullptr, peer_groups),
                   2) && ok;

  ok = ExpectReal64("SBSQL-513700E3598C-window-percent-rank-peer",
                    RunWindow("sb.window.percent_rank", peer_rank_values, 2, 0, peer_rank_values.size(),
                              1, 1, 1, nullptr, peer_groups),
                    0.25) && ok;
  ok = ExpectReal64("SBSQL-8F46078CCAA2-window-percent-rank-canonical-alias",
                    RunWindow("sb.window.percent_rank", peer_rank_values, 2, 0, peer_rank_values.size(),
                              1, 1, 1, nullptr, peer_groups),
                    0.25) && ok;

  ok = ExpectReal64("SBSQL-F959FD740DD3-window-cume-dist-peer",
                    RunWindow("sb.window.cume_dist", peer_rank_values, 2, 0, peer_rank_values.size(),
                              1, 1, 1, nullptr, peer_groups),
                    0.6) && ok;
  ok = ExpectReal64("SBSQL-3A4D165FF59E-window-cume-dist-canonical-alias",
                    RunWindow("sb.window.cume_dist", peer_rank_values, 2, 0, peer_rank_values.size(),
                              1, 1, 1, nullptr, peer_groups),
                    0.6) && ok;

  ok = ExpectInt64("SBSQL-405DA76744CA-ordered-rank",
                   RunWindow("sb.aggregate.rank", peer_rank_values, 0, 0, peer_rank_values.size(),
                             1, 1, 1, &hypothetical_25),
                   4) && ok;

  ok = ExpectInt64("SBSQL-D9E3FA320510-ordered-dense-rank",
                   RunWindow("sb.aggregate.dense_rank", peer_rank_values, 0, 0, peer_rank_values.size(),
                             1, 1, 1, &hypothetical_25),
                   3) && ok;

  ok = ExpectReal64("SBSQL-443C68E68D9F-ordered-percent-rank",
                    RunWindow("sb.aggregate.percent_rank", peer_rank_values, 0, 0, peer_rank_values.size(),
                              1, 1, 1, &hypothetical_25),
                    0.6) && ok;

  ok = ExpectReal64("SBSQL-63BB74EAD479-ordered-cume-dist",
                    RunWindow("sb.aggregate.cume_dist", peer_rank_values, 0, 0, peer_rank_values.size(),
                              1, 1, 1, &hypothetical_25),
                    2.0 / 3.0) && ok;

  ok = ExpectInt64("SBSQL-6F988BD1E2E0-ordered-rank-within-group",
                   RunWindow("rank(expr)WITHINGROUP(ORDERBYexpr)", peer_rank_values, 0, 0, peer_rank_values.size(),
                             1, 1, 1, &hypothetical_25),
                   4) && ok;

  ok = ExpectInt64("SBSQL-7B0D1EA07215-ordered-dense-rank-within-group",
                   RunWindow("dense_rank(expr)WITHINGROUP(ORDERBYexpr)", peer_rank_values, 0, 0, peer_rank_values.size(),
                             1, 1, 1, &hypothetical_25),
                   3) && ok;

  ok = ExpectReal64("SBSQL-374E6DE31900-ordered-percent-rank-within-group",
                    RunWindow("percent_rank(expr)WITHINGROUP(ORDERBYexpr)", peer_rank_values, 0, 0, peer_rank_values.size(),
                              1, 1, 1, &hypothetical_25),
                    0.6) && ok;

  ok = ExpectReal64("SBSQL-70B39E494FED-ordered-cume-dist-within-group",
                    RunWindow("cume_dist(expr)WITHINGROUP(ORDERBYexpr)", peer_rank_values, 0, 0, peer_rank_values.size(),
                              1, 1, 1, &hypothetical_25),
                    2.0 / 3.0) && ok;

  ok = ExpectInt64("SBSQL-28B6483D8641-row_number",
                   RunWindow("row_number", window_values, 2, 0, window_values.size()),
                   3) && ok;

  ok = ExpectInt64("SBSQL-7A6AFA548A76-row_number-over-lowered",
                   RunWindow("sb.window.row_number", window_values, 3, 0, window_values.size()),
                   4) && ok;

  ok = ExpectInt64("SBSQL-BAF3A91528AA-sb-window-row-number",
                   RunWindow("sb.window.row_number", window_values, 4, 0, window_values.size()),
                   5) && ok;

  ok = ExpectText("SBSQL-C02257DB2BE3-lag",
                  RunWindow("lag", window_values, 3, 0, window_values.size(), 2),
                  "east") && ok;

  ok = ExpectText("SBSQL-35A1ECA35D13-sb-window-lag",
                  RunWindow("sb.window.lag", window_values, 0, 0, window_values.size(), 1, 1, 1, &navigation_default),
                  "fallback") && ok;

  ok = ExpectText("SBSQL-0F7E089AB839-lag-over-lowered",
                  RunWindow("sb.window.lag", window_values, 2, 0, window_values.size(), 1),
                  "east") && ok;

  ok = ExpectText("SBSQL-CD90EEAF7468-lead",
                  RunWindow("lead", window_values, 1, 0, window_values.size(), 2),
                  "west") && ok;

  ok = ExpectText("SBSQL-F14938CD9CF3-sb-window-lead",
                  RunWindow("sb.window.lead", window_values, 4, 0, window_values.size(), 1, 1, 1, &navigation_default),
                  "fallback") && ok;

  ok = ExpectText("SBSQL-F7B4F498213C-lead-over-lowered",
                  RunWindow("sb.window.lead", window_values, 0, 0, window_values.size(), 1),
                  "east") && ok;

  ok = ExpectText("SBSQL-842F61769B34-first-value",
                  RunWindow("first_value", window_values, 3, 1, 4),
                  "east") && ok;

  ok = ExpectText("SBSQL-BDDEB821D132-sb-window-first-value",
                  RunWindow("sb.window.first_value", window_values, 2, 0, window_values.size()),
                  "north") && ok;

  ok = ExpectText("SBSQL-AA6AE730A722-first-value-over-lowered",
                  RunWindow("sb.window.first_value", window_values, 4, 2, window_values.size()),
                  "south") && ok;

  ok = ExpectInt64("SBSQL-E52C3FB97F6C-ntile",
                   RunWindow("ntile",
                             {TextValue("r1"), TextValue("r2"), TextValue("r3"), TextValue("r4"), TextValue("r5"),
                              TextValue("r6"), TextValue("r7"), TextValue("r8"), TextValue("r9"), TextValue("r10")},
                             5, 0, 10, 1, 4),
                   2) && ok;

  ok = ExpectInt64("SBSQL-6412E60ED18E-sb-window-ntile",
                   RunWindow("sb.window.ntile", {TextValue("solo")}, 0, 0, 1, 1, 4),
                   1) && ok;

  ok = ExpectInt64("SBSQL-1EF274EAE8DC-ntile-over-lowered",
                   RunWindow("sb.window.ntile", window_values, 4, 0, window_values.size(), 1, 2),
                   2) && ok;

  ok = ExpectFailure("window_ntile_zero_bucket",
                     RunWindow("ntile", window_values, 0, 0, window_values.size(), 1, 0),
                     "SB_DIAG_WINDOW_NTILE_BUCKET_INVALID") && ok;

  ok = ExpectText("SBSQL-2D40C15A4E0A-last-value",
                  RunWindow("last_value", window_values, 2, 1, 4),
                  "west") && ok;

  ok = ExpectNull("SBSQL-23AF50D41FEC-sb-window-last-value-all-null",
                  RunWindow("sb.window.last_value", {NullValue("text"), NullValue("text")}, 1, 0, 2),
                  "text") && ok;

  ok = ExpectText("SBSQL-804D99407A3B-last-value-over-lowered",
                  RunWindow("sb.window.last_value", {TextValue("solo")}, 0, 0, 1),
                  "solo") && ok;

  ok = ExpectNull("window_last_value_empty_frame",
                  RunWindow("last_value", window_values, 1, 1, 1),
                  "text") && ok;

  ok = ExpectText("SBSQL-ED86D05F9232-nth-value",
                  RunWindow("nth_value", window_values, 2, 1, window_values.size(), 1, 1, 3),
                  "west") && ok;

  ok = ExpectText("SBSQL-4BC628E8AD6C-sb-window-nth-value",
                  RunWindow("sb.window.nth_value", {TextValue("solo")}, 0, 0, 1, 1, 1, 1),
                  "solo") && ok;

  ok = ExpectText("SBSQL-C97299B0256C-nth-value-over-lowered",
                  RunWindow("sb.window.nth_value", window_values, 3, 0, window_values.size(), 1, 1, 2),
                  "east") && ok;

  ok = ExpectNull("window_nth_value_out_of_frame",
                  RunWindow("nth_value", {TextValue("north"), TextValue("east"), TextValue("south")}, 1, 1, 3, 1, 1, 5),
                  "text") && ok;

  ok = ExpectNull("window_nth_value_all_null",
                  RunWindow("sb.window.nth_value", {NullValue("text"), NullValue("text")}, 1, 0, 2, 1, 1, 2),
                  "text") && ok;

  ok = ExpectInt64("window_row_number",
                   RunWindow("sb.window.row_number", window_values, 2, 0, window_values.size()),
                   3) && ok;

  ok = ExpectInt64("window_ntile",
                   RunWindow("sb.window.ntile", window_values, 3, 0, window_values.size(), 1, 3),
                   2) && ok;

  ok = ExpectText("window_lag_offset",
                  RunWindow("sb.window.lag", window_values, 3, 0, window_values.size(), 2),
                  "east") && ok;

  ok = ExpectText("window_lag_default",
                  RunWindow("sb.window.lag", window_values, 0, 0, window_values.size(), 1, 1, 1, &navigation_default),
                  "fallback") && ok;

  ok = ExpectText("window_lead_offset",
                  RunWindow("sb.window.lead", window_values, 1, 0, window_values.size(), 2),
                  "west") && ok;

  ok = ExpectText("window_lead_default",
                  RunWindow("sb.window.lead", window_values, 4, 0, window_values.size(), 1, 1, 1, &navigation_default),
                  "fallback") && ok;

  ok = ExpectText("window_first_value_frame",
                  RunWindow("sb.window.first_value", window_values, 3, 1, 4),
                  "east") && ok;

  ok = ExpectText("window_last_value_frame",
                  RunWindow("sb.window.last_value", window_values, 2, 1, 4),
                  "west") && ok;

  ok = ExpectText("window_nth_value_frame",
                  RunWindow("sb.window.nth_value", window_values, 2, 1, 5, 1, 1, 3),
                  "west") && ok;

  ok = ExpectFailure("window_nth_value_invalid",
                     RunWindow("sb.window.nth_value", window_values, 2, 0, window_values.size(), 1, 1, 0),
                     "SB_DIAG_WINDOW_NTH_VALUE_INVALID") && ok;

  ok = ExpectFailure("percentile_fraction_invalid",
                     RunAggregate("percentile_cont", "real64",
                                  {Int64Value(10)},
                                  &invalid_fraction),
                     "SB_DIAG_AGGREGATE_PERCENTILE_FRACTION_INVALID") && ok;

  ok = ExpectFailure("top_k_limit_invalid",
                     RunAggregate("approx_top_k", "json",
                                  {TextValue("a")},
                                  &top_zero),
                     "SB_DIAG_AGGREGATE_TOP_K_LIMIT_INVALID") && ok;

  if (!ok) return 1;
  std::cout << "sbsql_sbsfc_015_aggregate_window_runtime_conformance=passed\n";
  return 0;
}
