// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define QOW_WIN_003_FIXTURE_ONLY
#include "qow_win_003.cpp"

#include "executor_foundation.hpp"

#include <initializer_list>

namespace {

std::string ValueFunctionUuid(
    const exec::CanonicalWindowValueFunction function) {
  switch (function) {
    case exec::CanonicalWindowValueFunction::lag:
      return "019de5fc-2400-782c-8436-9ac310301738";
    case exec::CanonicalWindowValueFunction::lead:
      return "019de5fc-2400-7a06-bc3c-6747cf5be66f";
    case exec::CanonicalWindowValueFunction::first_value:
      return "019de5fc-2400-7264-90fb-d25bd0f806f2";
    case exec::CanonicalWindowValueFunction::last_value:
      return "019de5fc-2400-7d23-a5be-7ed3f1a5c3ec";
    case exec::CanonicalWindowValueFunction::nth_value:
      return "019de5fc-2400-7dc9-80e6-9f2ccf08076f";
  }
  return {};
}

api::EngineTypedValue ValueOperand(const std::string& type,
                                   const std::string& encoded,
                                   const unsigned uuid) {
  const auto descriptor = WindowDescriptor(
      uuid, type,
      "type_uuid=" + WindowUuid(uuid + 100) + ";nullability=non_null");
  return WindowValue(descriptor, encoded);
}

std::vector<api::EngineTypedValue> RepeatedOperand(
    const std::size_t count, const std::string& type,
    const std::string& encoded, const unsigned uuid) {
  std::vector<api::EngineTypedValue> values;
  values.reserve(count);
  for (std::size_t row = 0; row < count; ++row) {
    values.push_back(ValueOperand(type, encoded, uuid));
  }
  return values;
}

exec::CanonicalWindowFrameResult ValueFrames(
    exec::CanonicalWindowFrameDescriptor frame) {
  auto partition_request = Window401Request();
  const auto payload_descriptor = WindowDescriptor(
      4105, "int64",
      "type_uuid=" + WindowUuid(4205) + ";nullability=nullable");
  partition_request.input_batch.columns[4].descriptor = payload_descriptor;
  partition_request.input_batch.columns[4].nullable = true;
  for (auto& row : partition_request.input_batch.rows) {
    if (row.values[4].encoded_value == "108") {
      row.values[4] = WindowNull(payload_descriptor);
    } else {
      row.values[4].descriptor = payload_descriptor;
    }
  }
  return ExecuteFrame(partition_request, std::move(frame));
}

exec::CanonicalWindowFrameDescriptor WholePartitionFrame(
    const exec::CanonicalWindowFrameExclusion exclusion =
        exec::CanonicalWindowFrameExclusion::no_others) {
  return ExplicitFrame(
      exec::CanonicalWindowFrameUnit::rows,
      FrameBound(exec::CanonicalWindowFrameBoundKind::unbounded_preceding),
      FrameBound(exec::CanonicalWindowFrameBoundKind::unbounded_following),
      exclusion);
}

exec::CanonicalWindowFrameDescriptor PrefixFrame(
    const exec::CanonicalWindowFrameExclusion exclusion =
        exec::CanonicalWindowFrameExclusion::no_others) {
  return ExplicitFrame(
      exec::CanonicalWindowFrameUnit::rows,
      FrameBound(exec::CanonicalWindowFrameBoundKind::unbounded_preceding),
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row), exclusion);
}

exec::CanonicalWindowValueRequest ValueRequest(
    const exec::CanonicalWindowValueFunction function,
    exec::CanonicalWindowFrameDescriptor frame = WholePartitionFrame()) {
  exec::CanonicalWindowValueRequest request;
  request.frames = ValueFrames(std::move(frame));
  request.function = function;
  request.function_uuid = ValueFunctionUuid(function);
  request.value_expression_descriptor_id = 4005;
  request.result_column = request.frames.ordered_batch.columns[4];
  request.result_column.stable_name = "window_value";
  request.result_column.descriptor_id = 4999;
  return request;
}

std::vector<std::string> ValueTexts(
    const exec::CanonicalWindowValueResult& result) {
  std::vector<std::string> values;
  values.reserve(result.values.size());
  for (const auto& value : result.values) {
    if (value.state == api::EngineValueState::sql_null && value.is_null &&
        value.encoded_value.empty() && value.binary_value.empty()) {
      values.emplace_back("<NULL>");
    } else {
      values.push_back(value.encoded_value);
    }
  }
  return values;
}

bool ValueRefused(const exec::CanonicalWindowValueResult& result,
                  const std::initializer_list<std::string_view> codes) {
  if (result.diagnostic.ok || !result.values.empty()) return false;
  for (const auto code : codes) {
    if (result.diagnostic.diagnostic_code == code) return true;
  }
  return false;
}

bool ValidateLag() {
  bool passed = true;
  auto request = ValueRequest(exec::CanonicalWindowValueFunction::lag);
  auto result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"<NULL>", "101", "105", "<NULL>",
                                        "100", "<NULL>", "<NULL>", "103",
                                        "<NULL>"}) &&
          result.frame_and_exclusion_validated &&
          result.frame_and_exclusion_ignored_for_navigation &&
          result.selected_plan_uuid == WindowUuid(4301) &&
          result.causal_counter_id == 40102 &&
          result.authority.engine_mga_snapshot_bound,
      "LAG default offset or partition/MGA evidence drifted");

  request = ValueRequest(
      exec::CanonicalWindowValueFunction::lag,
      WholePartitionFrame(exec::CanonicalWindowFrameExclusion::group));
  const auto excluded = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      excluded.diagnostic.ok && ValueTexts(excluded) == ValueTexts(result) &&
          request.frames.effective_frames[0].effective_row_indices.size() == 3,
      "LAG used frame exclusion after validating it");

  request = ValueRequest(exec::CanonicalWindowValueFunction::lag);
  request.offset_values =
      RepeatedOperand(request.frames.ordered_batch.rows.size(), "int64", "0",
                      5001);
  result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"101", "105", "<NULL>", "100", "107",
                                        "102", "103", "104", "106"}),
      "LAG offset zero did not select the current row");

  const std::vector<std::string> per_row_offsets = {
      "0", "1", "2", "3", "4", "0", "0", "1", "0"};
  std::vector<api::EngineTypedValue> offsets;
  for (std::size_t row = 0; row < per_row_offsets.size(); ++row) {
    offsets.push_back(
        ValueOperand("int64", per_row_offsets[row], 5003 + row));
  }
  request.offset_values = offsets;
  result = exec::ExecuteCanonicalWindowValue(request);
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"101", "101", "101", "101", "101",
                                        "102", "103", "103", "106"}),
      "LAG did not evaluate its offset independently per row");

  request.offset_values =
      RepeatedOperand(request.frames.ordered_batch.rows.size(), "int64", "2",
                      5002);
  std::vector<api::EngineTypedValue> defaults;
  for (std::size_t row = 0; row < request.frames.ordered_batch.rows.size();
       ++row) {
    defaults.push_back(
        ValueOperand("int32",
                     std::to_string(-10 - static_cast<std::int64_t>(row)),
                     5010 + row));
  }
  defaults[0].setState(api::EngineValueState::sql_null);
  defaults[0].encoded_value.clear();
  request.default_values = defaults;
  result = exec::ExecuteCanonicalWindowValue(request);
  if (!result.diagnostic.ok) {
    std::cerr << "QOW-TEST-WIN-007-V1: "
              << result.diagnostic.diagnostic_code << ": "
              << result.diagnostic.detail << '\n';
  }
  passed &= Require401(
      result.diagnostic.ok &&
          ValueTexts(result) ==
              std::vector<std::string>({"<NULL>", "-11", "101", "105", "<NULL>",
                                        "-15", "-16", "-17", "-18"}),
      "LAG did not evaluate typed offset/default operands per current row");
  return passed;
}

bool ValidateLagRefusals() {
  bool passed = true;
  auto request = ValueRequest(exec::CanonicalWindowValueFunction::lag);
  const auto rows = request.frames.ordered_batch.rows.size();
  request.offset_values = RepeatedOperand(rows, "int64", "-1", 5030);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-OFFSET"}),
      "negative LAG offset entered execution");

  auto null_offset = ValueOperand("int64", "1", 5031);
  null_offset.setState(api::EngineValueState::sql_null);
  null_offset.encoded_value.clear();
  request.offset_values = std::vector<api::EngineTypedValue>(rows, null_offset);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-OFFSET"}),
      "NULL LAG offset entered execution");

  request.offset_values = RepeatedOperand(
      rows, "int64", "9223372036854775808", 5032);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-OFFSET"}),
      "overflowed LAG offset entered execution");

  request = ValueRequest(exec::CanonicalWindowValueFunction::lag);
  request.default_values = RepeatedOperand(rows, "int64", "7", 5033);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-OFFSET"}),
      "LAG default silently supplied an omitted offset state");

  request.offset_values = RepeatedOperand(rows, "int64", "1", 5034);
  request.default_values = RepeatedOperand(rows, "real64", "1.5", 5035);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-DEFAULT-TYPE"}),
      "lossy LAG default conversion entered execution");

  request = ValueRequest(exec::CanonicalWindowValueFunction::lag);
  request.result_column.nullable = false;
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-FUNCTION-DESCRIPTOR"}),
      "non-nullable LAG result descriptor admitted boundary NULL");

  request = ValueRequest(exec::CanonicalWindowValueFunction::lag);
  request.function_uuid =
      ValueFunctionUuid(exec::CanonicalWindowValueFunction::lead);
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-FUNCTION-DESCRIPTOR"}),
      "LAG function UUID drift selected LEAD");

  request = ValueRequest(exec::CanonicalWindowValueFunction::lag);
  request.parser_execution_authority_claimed = true;
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-AUTHORITY"}),
      "parser authority claim entered LAG execution");

  request = ValueRequest(exec::CanonicalWindowValueFunction::lag);
  request.maximum_output_rows = 1;
  passed &= Require401(
      ValueRefused(exec::ExecuteCanonicalWindowValue(request),
                   {"QOW-DIAG-WINDOW-FRAME"}),
      "LAG resource overflow returned partial values");
  return passed;
}

}  // namespace

#ifndef QOW_WIN_007_FIXTURE_ONLY
// QOW-TEST-WIN-007-V1
int main() {
  return ValidateLag() && ValidateLagRefusals() ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif
