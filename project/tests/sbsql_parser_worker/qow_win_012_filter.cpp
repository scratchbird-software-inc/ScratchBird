// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define QOW_WIN_012_STATE_FIXTURE_ONLY
#include "qow_win_012_state.cpp"

namespace {

bool ValidateAggregateWindowFilter() {
  namespace api = scratchbird::engine::internal_api;
  auto request = AggregateWindowRequest();
  request.filter_truth_values =
      std::vector<api::EngineSqlTruthValue>{
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::false_value,
          api::EngineSqlTruthValue::unknown,
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::false_value,
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::unknown,
          api::EngineSqlTruthValue::true_value};
  const auto result = exec::ExecuteCanonicalWindowAggregate(request);
  return Require401(
      result.diagnostic.ok &&
          AggregateTexts(result) ==
              std::vector<std::string>({"308", "308", "308", "308", "308",
                                        "<NULL>", "103", "103", "106"}) &&
          result.transition_count == 18 &&
          result.transition_row_indices[0] ==
              std::vector<std::size_t>({0, 3, 4}) &&
          result.filter_applied_before_transition,
      "aggregate FILTER did not retain only canonical TRUE frame rows");
}

bool ValidateAggregateWindowFilterRefusals() {
  namespace api = scratchbird::engine::internal_api;
  bool passed = true;
  auto request = AggregateWindowRequest();
  request.filter_truth_values = std::vector<api::EngineSqlTruthValue>(
      request.frames.ordered_batch.rows.size() - 1,
      api::EngineSqlTruthValue::true_value);
  passed &= Require401(
      AggregateRefused(exec::ExecuteCanonicalWindowAggregate(request),
                       {"QOW-DIAG-WINDOW-AGGREGATE-FILTER"}),
      "aggregate FILTER accepted mismatched predicate cardinality");

  request = AggregateWindowRequest();
  request.filter_truth_values = std::vector<api::EngineSqlTruthValue>(
      request.frames.ordered_batch.rows.size(),
      api::EngineSqlTruthValue::true_value);
  (*request.filter_truth_values)[2] =
      api::EngineSqlTruthValue::unspecified;
  passed &= Require401(
      AggregateRefused(exec::ExecuteCanonicalWindowAggregate(request),
                       {"QOW-DIAG-WINDOW-AGGREGATE-FILTER"}),
      "aggregate FILTER treated an unspecified truth value as FALSE");
  return passed;
}

}  // namespace

// QOW-TEST-WIN-012-FILTER-V1
int main() {
  return ValidateAggregateWindowFilter() &&
                 ValidateAggregateWindowFilterRefusals()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
