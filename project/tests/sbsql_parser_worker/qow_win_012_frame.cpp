// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define QOW_WIN_012_STATE_FIXTURE_ONLY
#include "qow_win_012_state.cpp"

namespace {

bool ValidateAggregateWindowFrame() {
  auto request = AggregateWindowRequest(
      PrefixFrame(exec::CanonicalWindowFrameExclusion::current_row));
  auto result = exec::ExecuteCanonicalWindowAggregate(request);
  bool passed = Require401(
      result.diagnostic.ok &&
          AggregateTexts(result) ==
              std::vector<std::string>({"<NULL>", "101", "206", "206", "306",
                                        "<NULL>", "<NULL>", "103", "<NULL>"}) &&
          result.transition_count == 11 && result.effective_frame_recomputed,
      "aggregate window did not recompute the post-exclusion frame");

  request = AggregateWindowRequest(ExplicitFrame(
      exec::CanonicalWindowFrameUnit::rows,
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row),
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row),
      exec::CanonicalWindowFrameExclusion::current_row));
  result = exec::ExecuteCanonicalWindowAggregate(request);
  passed &= Require401(
      result.diagnostic.ok &&
          AggregateTexts(result) ==
              std::vector<std::string>(9, "<NULL>") &&
          result.transition_count == 0 &&
          std::ranges::all_of(
              request.frames.effective_frames, [](const auto& frame) {
                return frame.base_state ==
                           exec::CanonicalWindowFrameState::nonempty &&
                       frame.effective_state ==
                           exec::CanonicalWindowFrameState::empty &&
                       frame.excluded_row_count == 1;
              }),
      "empty effective aggregate frames did not finalize to typed NULL");
  return passed;
}

bool ValidateAggregateWindowFrameRefusals() {
  auto request = AggregateWindowRequest();
  request.frames.effective_frames[0].effective_row_indices = {1, 0};
  return Require401(
      AggregateRefused(exec::ExecuteCanonicalWindowAggregate(request),
                       {"QOW-DIAG-WINDOW-AGGREGATE-FRAME"}),
      "aggregate window accepted forged effective-frame ordering");
}

}  // namespace

// QOW-TEST-WIN-012-FRAME-V1
int main() {
  return ValidateAggregateWindowFrame() &&
                 ValidateAggregateWindowFrameRefusals()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
