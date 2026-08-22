// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define QOW_WIN_012_STATE_FIXTURE_ONLY
#include "qow_win_012_state.cpp"

namespace {

exec::CanonicalWindowAggregateRequest DuplicateAggregateWindowRequest() {
  auto request = AggregateWindowRequest();
  request.frames.ordered_batch.rows[1].values[4] =
      request.frames.ordered_batch.rows[0].values[4];
  request.frames.ordered_batch.rows[7].values[4] =
      request.frames.ordered_batch.rows[6].values[4];
  request.distinct = true;
  return request;
}

bool ValidateAggregateWindowDistinct() {
  auto request = DuplicateAggregateWindowRequest();
  const auto result = exec::ExecuteCanonicalWindowAggregate(request);
  bool passed = Require401(
      result.diagnostic.ok &&
          AggregateTexts(result) ==
              std::vector<std::string>({"308", "308", "308", "308", "308",
                                        "102", "103", "103", "106"}) &&
          result.transition_count == 19 && result.distinct_value_count == 19 &&
          result.transition_row_indices[0] ==
              std::vector<std::size_t>({0, 3, 4}) &&
          result.distinct_applied_before_transition,
      "aggregate-window DISTINCT did not use typed frame-local equality: " +
          result.diagnostic.diagnostic_code + ":" + result.diagnostic.detail);

  request.distinct = false;
  const auto ordinary = exec::ExecuteCanonicalWindowAggregate(request);
  passed &= Require401(
      ordinary.diagnostic.ok && AggregateTexts(ordinary).front() == "409" &&
          ordinary.transition_row_indices.front().size() == 5,
      "DISTINCT mutation did not alter aggregate transition state");
  return passed;
}

bool ValidateAggregateWindowDistinctRefusals() {
  auto request = DuplicateAggregateWindowRequest();
  request.maximum_distinct_value_count = 2;
  return Require401(
      AggregateRefused(exec::ExecuteCanonicalWindowAggregate(request),
                       {"QOW-DIAG-WINDOW-AGGREGATE-DISTINCT"}),
      "aggregate-window DISTINCT exceeded its state resource bound");
}

}  // namespace

// QOW-TEST-WIN-012-DISTINCT-V1
int main() {
  return ValidateAggregateWindowDistinct() &&
                 ValidateAggregateWindowDistinctRefusals()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
