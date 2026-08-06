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

  const auto peer_execute = [](const auto exclusion) {
    return ExecuteFrame(
        Window401Request(),
        ExplicitFrame(
            exec::CanonicalWindowFrameUnit::groups,
            FrameBound(exec::CanonicalWindowFrameBoundKind::current_row),
            FrameBound(exec::CanonicalWindowFrameBoundKind::current_row),
            exclusion));
  };
  const auto peer_no_others =
      peer_execute(exec::CanonicalWindowFrameExclusion::no_others);
  const auto peer_current =
      peer_execute(exec::CanonicalWindowFrameExclusion::current_row);
  const auto peer_group =
      peer_execute(exec::CanonicalWindowFrameExclusion::group);
  const auto peer_ties =
      peer_execute(exec::CanonicalWindowFrameExclusion::ties);
  passed &= Require401(
      peer_no_others.effective_frames[0].effective_row_indices ==
              std::vector<std::size_t>({0, 1}) &&
          peer_current.effective_frames[0].effective_row_indices ==
              std::vector<std::size_t>({1}) &&
          peer_current.effective_frames[1].effective_row_indices ==
              std::vector<std::size_t>({0}) &&
          peer_group.effective_frames[0].effective_row_indices.empty() &&
          peer_ties.effective_frames[0].effective_row_indices ==
              std::vector<std::size_t>({0}) &&
          peer_ties.effective_frames[1].effective_row_indices ==
              std::vector<std::size_t>({1}),
      "peer-local exclusion semantics did not distinguish row, group, and ties");
  passed &= Require401(
      peer_group.effective_frames[0].base_state ==
              exec::CanonicalWindowFrameState::nonempty &&
          peer_group.effective_frames[0].effective_state ==
              exec::CanonicalWindowFrameState::empty &&
          peer_group.effective_frames[0].excluded_row_count == 2 &&
          peer_group.effective_frames[0].exclusion_operand_consumed &&
          peer_group.base_frame_constructed_before_exclusion &&
          peer_group.exactly_one_exclusion_consumed &&
          peer_ties.effective_frames[0].excluded_row_count == 1 &&
          peer_no_others.effective_frames[0].excluded_row_count == 0 &&
          peer_no_others.effective_frames[0].exclusion_operand_consumed,
      "post-exclusion empty state or exclusion counts were ambiguous");

  const auto rows_current_group = ExecuteFrame(
      Window401Request(),
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::rows,
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row),
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row),
          exec::CanonicalWindowFrameExclusion::group));
  passed &= Require401(
      rows_current_group.diagnostic.ok &&
          rows_current_group.effective_frames[0].excluded_row_count == 1 &&
          rows_current_group.effective_frames[0].effective_row_indices.empty(),
      "EXCLUDE GROUP removed a peer outside the clamped ROWS base frame");

  auto one_order = Window401Request();
  one_order.order_terms.pop_back();
  exec::CanonicalWindowFrameDescriptor omitted;
  omitted.frame_descriptor_uuid = WindowUuid(4902);
  const auto ordered_default = ExecuteFrame(one_order, omitted);
  passed &= Require401(
      ordered_default.diagnostic.ok && ordered_default.defaulted_with_order &&
          ordered_default.resolved_frame.unit ==
              exec::CanonicalWindowFrameUnit::range &&
          ordered_default.effective_frames[0].effective_row_indices ==
              std::vector<std::size_t>({0, 1, 2}) &&
          ordered_default.effective_frames[1].effective_row_indices ==
              std::vector<std::size_t>({0, 1, 2}) &&
          ordered_default.effective_frames[2].effective_row_indices ==
              std::vector<std::size_t>({0, 1, 2}) &&
          ordered_default.effective_frames[3].effective_row_indices ==
              std::vector<std::size_t>({0, 1, 2, 3}),
      "ordered default frame did not expand CURRENT ROW to every typed peer");

  auto no_order = Window401Request();
  no_order.order_terms.clear();
  no_order.ordering_property_uuid.clear();
  no_order.physical_dag.nodes[1].required_property_uuids.pop_back();
  const auto unordered_default = ExecuteFrame(no_order, omitted);
  passed &= Require401(
      unordered_default.diagnostic.ok &&
          unordered_default.defaulted_without_order &&
          unordered_default.resolved_frame.unit ==
              exec::CanonicalWindowFrameUnit::rows &&
          unordered_default.effective_frames[0].effective_row_indices ==
              std::vector<std::size_t>({0, 1, 2, 3, 4}) &&
          unordered_default.effective_frames[4].effective_row_indices ==
              std::vector<std::size_t>({0, 1, 2, 3, 4}),
      "unordered default did not expose the whole current partition per row");

  const auto unordered_current_range = ExecuteFrame(
      no_order,
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::range,
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row),
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row)));
  passed &= Require401(
      unordered_current_range.diagnostic.ok &&
          unordered_current_range.effective_frames[0].effective_row_indices ==
              std::vector<std::size_t>({0, 1, 2, 3, 4}),
      "unordered RANGE CURRENT ROW did not use the all-row peer group");

  auto empty = Window401Request();
  empty.input_batch.rows.clear();
  const auto empty_default = ExecuteFrame(empty, omitted);
  passed &= Require401(
      empty_default.diagnostic.ok && empty_default.defaulted_with_order &&
          empty_default.effective_frames.empty() &&
          empty_default.base_frame_constructed_before_exclusion &&
          empty_default.exactly_one_exclusion_consumed,
      "empty input did not preserve canonical default/exclusion evidence");

  const auto empty_base = ExecuteFrame(
      Window401Request(),
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::rows,
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_following,
                     exec::EncodeInt64Value(100)),
          FrameBound(
              exec::CanonicalWindowFrameBoundKind::unbounded_following),
          exec::CanonicalWindowFrameExclusion::group));
  passed &= Require401(
      empty_base.diagnostic.ok &&
          empty_base.effective_frames[0].base_state ==
              exec::CanonicalWindowFrameState::empty &&
          empty_base.effective_frames[0].effective_state ==
              exec::CanonicalWindowFrameState::empty &&
          empty_base.effective_frames[0].excluded_row_count == 0 &&
          !empty_base.effective_frames[0].base_begin.has_value(),
      "exclusion changed a clamped empty base frame or introduced a sentinel");

  const auto reversed_range = ExecuteFrame(
      one_order,
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::range,
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_following,
                     TypedOffset("int32", "1", 4990)),
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row)));
  passed &= Require401(
      reversed_range.diagnostic.ok &&
          reversed_range.effective_frames[0].base_state ==
              exec::CanonicalWindowFrameState::reversed_to_empty &&
          reversed_range.effective_frames[0].effective_state ==
              exec::CanonicalWindowFrameState::reversed_to_empty &&
          !reversed_range.effective_frames[0].base_begin.has_value(),
      "RANGE reversal at an equal physical boundary lost semantic direction");

  const auto reversed_preceding_range = ExecuteFrame(
      one_order,
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::range,
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding,
                     TypedOffset("int32", "1", 4991)),
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding,
                     TypedOffset("int32", "2", 4992))));
  const auto reversed_following_range = ExecuteFrame(
      one_order,
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::range,
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_following,
                     TypedOffset("int32", "2", 4993)),
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_following,
                     TypedOffset("int32", "1", 4994))));
  const auto zero_following_to_current = ExecuteFrame(
      one_order,
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::range,
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_following,
                     TypedOffset("int32", "0", 4995)),
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row)));
  passed &= Require401(
      reversed_preceding_range.diagnostic.ok &&
          reversed_following_range.diagnostic.ok &&
          reversed_preceding_range.effective_frames[0].base_state ==
              exec::CanonicalWindowFrameState::reversed_to_empty &&
          reversed_following_range.effective_frames[0].base_state ==
              exec::CanonicalWindowFrameState::reversed_to_empty &&
          zero_following_to_current.diagnostic.ok &&
          zero_following_to_current.effective_frames[0].base_state ==
              exec::CanonicalWindowFrameState::nonempty &&
          zero_following_to_current.effective_frames[0]
                  .effective_row_indices ==
              std::vector<std::size_t>({0, 1, 2}),
      "RANGE same-direction offsets or zero-offset CURRENT ROW ordering drifted");
  return passed;
}

}  // namespace

// QOW-TEST-WIN-013-V1
int main() {
  return ValidateEveryExclusionForm() ? EXIT_SUCCESS : EXIT_FAILURE;
}
