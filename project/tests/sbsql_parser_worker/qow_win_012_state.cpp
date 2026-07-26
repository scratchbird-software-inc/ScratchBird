// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define QOW_WIN_007_FIXTURE_ONLY
#include "qow_win_007.cpp"

namespace {

constexpr std::string_view kInt64SumUuid =
    "019de5fc-2400-72e4-8549-82b2eef5a777";

exec::CanonicalWindowAggregateRequest AggregateWindowRequest(
    exec::CanonicalWindowFrameDescriptor frame = WholePartitionFrame()) {
  exec::CanonicalWindowAggregateRequest request;
  request.frames = ValueFrames(std::move(frame));
  request.function = exec::CanonicalWindowAggregateFunction::int64_sum;
  request.function_uuid = kInt64SumUuid;
  request.value_expression_descriptor_id = 4005;
  request.result_column = request.frames.ordered_batch.columns[4];
  request.result_column.stable_name = "window_sum";
  request.result_column.descriptor_id = 5999;
  return request;
}

std::vector<std::string> AggregateTexts(
    const exec::CanonicalWindowAggregateResult& result) {
  std::vector<std::string> values;
  values.reserve(result.values.size());
  for (const auto& value : result.values) {
    values.push_back(value.state == api::EngineValueState::sql_null
                         ? "<NULL>"
                         : value.encoded_value);
  }
  return values;
}

bool AggregateRefused(
    const exec::CanonicalWindowAggregateResult& result,
    const std::initializer_list<std::string_view> codes) {
  if (result.diagnostic.ok || !result.values.empty() ||
      !result.transition_row_indices.empty()) {
    return false;
  }
  for (const auto code : codes) {
    if (result.diagnostic.diagnostic_code == code) return true;
  }
  return false;
}

bool ValidateAggregateWindowState() {
  auto request = AggregateWindowRequest();
  auto result = exec::ExecuteCanonicalWindowAggregate(request);
  bool passed = Require401(
      result.diagnostic.ok &&
          AggregateTexts(result) ==
              std::vector<std::string>({"413", "413", "413", "413", "413",
                                        "102", "207", "207", "106"}) &&
          result.transition_count == 31 &&
          result.transition_row_indices.front() ==
              std::vector<std::size_t>({0, 1, 2, 3, 4}) &&
          result.shared_aggregate_state_authority_used &&
          result.effective_frame_recomputed &&
          result.authority.engine_mga_snapshot_bound &&
          result.selected_plan_uuid == WindowUuid(4301) &&
          result.causal_counter_id == 40102,
      "SUM window did not reuse canonical state/finalization authority");

  request = AggregateWindowRequest(PrefixFrame());
  result = exec::ExecuteCanonicalWindowAggregate(request);
  passed &= Require401(
      result.diagnostic.ok &&
          AggregateTexts(result) ==
              std::vector<std::string>({"101", "206", "206", "306", "413",
                                        "102", "103", "207", "106"}) &&
          result.transition_count == 20,
      "SUM window did not recompute the exact moving frame");
  return passed;
}

bool ValidateAggregateWindowStateRefusals() {
  bool passed = true;
  auto request = AggregateWindowRequest();
  request.function_uuid =
      "019de5fc-2400-784a-9aec-371f8b95b7ea";
  passed &= Require401(
      AggregateRefused(exec::ExecuteCanonicalWindowAggregate(request),
                       {"QOW-DIAG-WINDOW-AGGREGATE"}),
      "COUNT registry UUID selected the SUM window state");

  request = AggregateWindowRequest();
  request.result_column.nullable = false;
  passed &= Require401(
      AggregateRefused(exec::ExecuteCanonicalWindowAggregate(request),
                       {"QOW-DIAG-WINDOW-AGGREGATE"}),
      "SUM window admitted a result unable to represent empty-frame NULL");

  request = AggregateWindowRequest();
  request.maximum_transition_count = 30;
  passed &= Require401(
      AggregateRefused(exec::ExecuteCanonicalWindowAggregate(request),
                       {"QOW-DIAG-WINDOW-AGGREGATE-FRAME"}),
      "SUM window published partial output after transition exhaustion");

  request = AggregateWindowRequest();
  request.transaction_finality_claimed = true;
  passed &= Require401(
      AggregateRefused(exec::ExecuteCanonicalWindowAggregate(request),
                       {"QOW-DIAG-WINDOW-AUTHORITY"}),
      "SUM window claimed engine transaction finality");
  return passed;
}

}  // namespace

#ifndef QOW_WIN_012_STATE_FIXTURE_ONLY
// QOW-TEST-WIN-012-STATE-V1
int main() {
  return ValidateAggregateWindowState() &&
                 ValidateAggregateWindowStateRefusals()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
#endif
