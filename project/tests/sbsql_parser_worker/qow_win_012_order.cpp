// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define QOW_WIN_012_STATE_FIXTURE_ONLY
#include "qow_win_012_state.cpp"

namespace {

exec::CanonicalWindowAggregateRequest OrderedAggregateWindowRequest() {
  auto request = AggregateWindowRequest();
  exec::CanonicalDescriptorOrderTerm term;
  term.column = 4;
  term.expression_descriptor_id = 4005;
  term.direction = exec::CanonicalDescriptorOrderDirection::descending;
  term.null_placement = exec::CanonicalDescriptorNullPlacement::last;
  request.aggregate_order_terms = {term};
  request.deterministic_tie_evidence_uuid = WindowUuid(5901);
  return request;
}

bool ValidateAggregateWindowOrder() {
  const auto result =
      exec::ExecuteCanonicalWindowAggregate(OrderedAggregateWindowRequest());
  return Require401(
      result.diagnostic.ok &&
          AggregateTexts(result) ==
              std::vector<std::string>({"413", "413", "413", "413", "413",
                                        "102", "207", "207", "106"}) &&
          result.transition_row_indices[0] ==
              std::vector<std::size_t>({4, 1, 0, 3, 2}) &&
          result.transition_row_indices[6] ==
              std::vector<std::size_t>({7, 6}) &&
          result.pair_comparison_count == 135 &&
          result.aggregate_order_independent_of_window_order,
      "aggregate argument ORDER BY reused window order or lost typed NULL rules");
}

bool ValidateAggregateWindowOrderRefusals() {
  bool passed = true;
  auto request = OrderedAggregateWindowRequest();
  request.deterministic_tie_evidence_uuid.clear();
  passed &= Require401(
      AggregateRefused(exec::ExecuteCanonicalWindowAggregate(request),
                       {"QOW-DIAG-WINDOW-AGGREGATE-ORDER"}),
      "aggregate argument ORDER BY omitted deterministic tie evidence");

  request = OrderedAggregateWindowRequest();
  request.aggregate_order_terms[0].expression_descriptor_id = 4004;
  passed &= Require401(
      AggregateRefused(exec::ExecuteCanonicalWindowAggregate(request),
                       {"QOW-DIAG-WINDOW-AGGREGATE-ORDER"}),
      "aggregate argument ORDER BY accepted an unresolved descriptor handle");

  request = OrderedAggregateWindowRequest();
  request.maximum_pair_comparisons = 134;
  passed &= Require401(
      AggregateRefused(exec::ExecuteCanonicalWindowAggregate(request),
                       {"QOW-DIAG-WINDOW-AGGREGATE-ORDER"}),
      "aggregate argument ORDER BY published partial comparison output");
  return passed;
}

}  // namespace

// QOW-TEST-WIN-012-ORDER-V1
int main() {
  return ValidateAggregateWindowOrder() &&
                 ValidateAggregateWindowOrderRefusals()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
