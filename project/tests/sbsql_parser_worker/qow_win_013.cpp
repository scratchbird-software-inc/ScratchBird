// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define QOW_WIN_003_FIXTURE_ONLY
#include "qow_win_003.cpp"

namespace {

bool ValidateEveryExclusionForm() {
  bool passed = true;
  const auto execute = [](const exec::CanonicalWindowFrameExclusion exclusion) {
    return ExecuteFrame(
        Window401Request(),
        ExplicitFrame(
            exec::CanonicalWindowFrameUnit::rows,
            FrameBound(
                exec::CanonicalWindowFrameBoundKind::unbounded_preceding),
            FrameBound(
                exec::CanonicalWindowFrameBoundKind::unbounded_following),
            exclusion));
  };

  const auto no_others =
      execute(exec::CanonicalWindowFrameExclusion::no_others);
  const auto current =
      execute(exec::CanonicalWindowFrameExclusion::current_row);
  const auto group = execute(exec::CanonicalWindowFrameExclusion::group);
  const auto ties = execute(exec::CanonicalWindowFrameExclusion::ties);
  passed &= Require401(
      no_others.diagnostic.ok && current.diagnostic.ok &&
          group.diagnostic.ok && ties.diagnostic.ok,
      "one or more canonical exclusion forms refused");
  if (!passed) return false;
  passed &= Require401(
      no_others.effective_frames[0].effective_row_indices ==
              std::vector<std::size_t>({0, 1, 2, 3, 4}) &&
          current.effective_frames[0].effective_row_indices ==
              std::vector<std::size_t>({1, 2, 3, 4}) &&
          group.effective_frames[0].effective_row_indices ==
              std::vector<std::size_t>({2, 3, 4}) &&
          ties.effective_frames[0].effective_row_indices ==
              std::vector<std::size_t>({0, 2, 3, 4}),
      "EXCLUDE operand did not alter the effective frame exactly");
  passed &= Require401(
      !no_others.effective_frames[0].exclusion_applied &&
          current.effective_frames[0].exclusion_applied &&
          group.effective_frames[0].exclusion_applied &&
          ties.effective_frames[0].exclusion_applied &&
          no_others.every_frame_operand_consumed &&
          current.every_frame_operand_consumed &&
          group.every_frame_operand_consumed &&
          ties.every_frame_operand_consumed,
      "exclusion consumption evidence was not retained");
  return passed;
}

}  // namespace

// QOW-TEST-WIN-013-V1
int main() {
  return ValidateEveryExclusionForm() ? EXIT_SUCCESS : EXIT_FAILURE;
}
