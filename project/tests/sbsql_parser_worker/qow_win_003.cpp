// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define QOW_WIN_004_FIXTURE_ONLY
#include "qow_win_004.cpp"

#include <optional>

namespace {

exec::CanonicalWindowFrameBound FrameBound(
    const exec::CanonicalWindowFrameBoundKind kind,
    std::optional<api::EngineTypedValue> offset = std::nullopt) {
  exec::CanonicalWindowFrameBound bound;
  bound.kind = kind;
  bound.offset = std::move(offset);
  return bound;
}

exec::CanonicalWindowFrameDescriptor ExplicitFrame(
    const exec::CanonicalWindowFrameUnit unit,
    exec::CanonicalWindowFrameBound start,
    exec::CanonicalWindowFrameBound end,
    const exec::CanonicalWindowFrameExclusion exclusion =
        exec::CanonicalWindowFrameExclusion::no_others) {
  exec::CanonicalWindowFrameDescriptor frame;
  frame.frame_descriptor_uuid = WindowUuid(4901);
  frame.frame_specified = true;
  frame.unit = unit;
  frame.start = std::move(start);
  frame.end = std::move(end);
  frame.exclusion = exclusion;
  return frame;
}

exec::CanonicalWindowFrameResult ExecuteFrame(
    const exec::CanonicalWindowPartitionOrderRequest& partition_request,
    exec::CanonicalWindowFrameDescriptor frame,
    const std::size_t maximum_references = 1048576) {
  exec::CanonicalWindowFrameRequest request;
  request.partition_order =
      exec::ExecuteCanonicalWindowPartitionOrder(partition_request);
  request.mga_authority = partition_request.mga_authority;
  request.frame = std::move(frame);
  request.frame_property_binding_evidence_uuid = WindowUuid(4903);
  request.maximum_effective_row_references = maximum_references;
  return exec::ExecuteCanonicalWindowFrames(request);
}

std::size_t EffectiveReferenceCount(
    const exec::CanonicalWindowFrameResult& result) {
  std::size_t count = 0;
  for (const auto& frame : result.effective_frames) {
    count += frame.effective_row_indices.size();
  }
  return count;
}

api::EngineTypedValue TypedOffset(const std::string& type,
                                  const std::string& encoded,
                                  const unsigned uuid = 4910) {
  const auto descriptor = WindowDescriptor(
      uuid, type,
      "type_uuid=" + WindowUuid(uuid + 100) + ";nullability=non_null");
  return WindowValue(descriptor, encoded);
}

bool ValidateRowsGroupsRangeAndDefaults() {
  bool passed = true;
  const auto int64_one = exec::EncodeInt64Value(1);
  const auto int64_zero = exec::EncodeInt64Value(0);

  const auto rows = ExecuteFrame(
      Window401Request(),
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::rows,
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding,
                     int64_one),
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row)));
  passed &= Require401(
      rows.diagnostic.ok && rows.effective_frames.size() == 9 &&
          rows.effective_frames[0].effective_row_indices ==
              std::vector<std::size_t>({0}) &&
          rows.effective_frames[1].effective_row_indices ==
              std::vector<std::size_t>({0, 1}) &&
          rows.effective_frames[2].effective_row_indices ==
              std::vector<std::size_t>({1, 2}) &&
          rows.every_frame_operand_consumed &&
          rows.empty_state_uses_optional_bounds &&
          rows.partition_property_uuid ==
              Window401Request().partition_property_uuid &&
          rows.term_binding_evidence_uuid ==
              Window401Request().term_binding_evidence_uuid &&
          rows.deterministic_tie_evidence_uuid ==
              Window401Request().deterministic_tie_evidence_uuid &&
          rows.frame_property_binding_evidence_uuid == WindowUuid(4903),
      "ROWS offset bounds did not use physical row ordinals");

  const auto rows_zero = ExecuteFrame(
      Window401Request(),
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::rows,
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding,
                     int64_zero),
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row)));
  passed &= Require401(
      rows_zero.diagnostic.ok &&
          rows_zero.effective_frames[2].effective_row_indices ==
              std::vector<std::size_t>({2}) &&
          rows_zero.effective_frames[2].effective_row_indices !=
              rows.effective_frames[2].effective_row_indices,
      "ROWS start-offset mutation was not semantically visible");

  const auto groups = ExecuteFrame(
      Window401Request(),
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::groups,
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding,
                     int64_one),
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row)));
  passed &= Require401(
      groups.diagnostic.ok &&
          groups.effective_frames[0].effective_row_indices ==
              std::vector<std::size_t>({0, 1}) &&
          groups.effective_frames[2].effective_row_indices ==
              std::vector<std::size_t>({0, 1, 2}) &&
          groups.effective_frames[3].effective_row_indices ==
              std::vector<std::size_t>({2, 3}),
      "GROUPS offset bounds did not use explicit typed peer groups");

  const auto rows_following = ExecuteFrame(
      Window401Request(),
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::rows,
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row),
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_following,
                     int64_one)));
  passed &= Require401(
      rows_following.diagnostic.ok &&
          rows_following.effective_frames[2].effective_row_indices ==
              std::vector<std::size_t>({2, 3}),
      "ROWS FOLLOWING did not use the next physical row within the partition");

  const auto groups_following = ExecuteFrame(
      Window401Request(),
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::groups,
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row),
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_following,
                     int64_one)));
  passed &= Require401(
      groups_following.diagnostic.ok &&
          groups_following.effective_frames[0].effective_row_indices ==
              std::vector<std::size_t>({0, 1, 2}) &&
          groups_following.effective_frames[2].effective_row_indices ==
              std::vector<std::size_t>({2, 3}),
      "GROUPS FOLLOWING did not advance by explicit peer-group ranges");

  auto one_order = Window401Request();
  one_order.order_terms.pop_back();
  const auto range = ExecuteFrame(
      one_order,
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::range,
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding,
                     TypedOffset("int32", "1")),
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row)));
  passed &= Require401(
      range.diagnostic.ok &&
          range.effective_frames[3].effective_row_indices ==
              std::vector<std::size_t>({0, 1, 2, 3}) &&
          range.effective_frames[4].effective_row_indices ==
              std::vector<std::size_t>({4}),
      "numeric RANGE did not consume a compatible typed offset");

  const auto range_following = ExecuteFrame(
      one_order,
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::range,
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row),
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_following,
                     TypedOffset("int32", "1", 4970))));
  passed &= Require401(
      range_following.diagnostic.ok &&
          range_following.effective_frames[0].effective_row_indices ==
              std::vector<std::size_t>({0, 1, 2, 3}),
      "numeric RANGE FOLLOWING did not include the threshold value");

  auto descending_order = one_order;
  descending_order.order_terms[0].direction =
      exec::CanonicalDescriptorOrderDirection::descending;
  const auto descending_range = ExecuteFrame(
      descending_order,
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::range,
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row),
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_following,
                     TypedOffset("int32", "1", 4971))));
  passed &= Require401(
      descending_range.diagnostic.ok &&
          descending_range.effective_frames[0].effective_row_indices ==
              std::vector<std::size_t>({0, 1, 2, 3}),
      "descending numeric RANGE FOLLOWING applied ascending threshold direction");

  auto hidden_range_request = Window401Request();
  const auto hidden_order_descriptor = WindowDescriptor(
      4980, "int64",
      "type_uuid=" + WindowUuid(4981) + ";nullability=nullable");
  exec::DescriptorBatch hidden_keys;
  hidden_keys.columns = {hidden_range_request.input_batch.columns[0],
                         hidden_range_request.input_batch.columns[1],
                         {"derived_order", hidden_order_descriptor, true,
                          4982}};
  for (const auto& row : hidden_range_request.input_batch.rows) {
    auto derived_order = row.values[2].isSqlNull()
                             ? WindowNull(hidden_order_descriptor)
                             : WindowValue(hidden_order_descriptor,
                                           row.values[2].encoded_value);
    hidden_keys.rows.push_back(
        {{row.values[0], row.values[1], std::move(derived_order)}});
  }
  hidden_range_request.key_batch = std::move(hidden_keys);
  hidden_range_request.order_terms.resize(1);
  hidden_range_request.order_terms[0].column = 2;
  hidden_range_request.order_terms[0].expression_descriptor_id = 4982;
  const auto hidden_range = ExecuteFrame(
      hidden_range_request,
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::range,
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding,
                     TypedOffset("int32", "1", 4983)),
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row)));
  passed &= Require401(
      hidden_range.diagnostic.ok &&
          hidden_range.effective_frames[3].effective_row_indices ==
              std::vector<std::size_t>({0, 1, 2, 3}),
      "RANGE did not consume the ordered hidden expression-key batch");

  exec::CanonicalWindowFrameDescriptor omitted;
  omitted.frame_descriptor_uuid = WindowUuid(4902);
  auto default_with_order = ExecuteFrame(one_order, omitted);
  passed &= Require401(
      default_with_order.diagnostic.ok &&
          default_with_order.defaulted_with_order &&
          !default_with_order.defaulted_without_order &&
          default_with_order.resolved_frame.unit ==
              exec::CanonicalWindowFrameUnit::range &&
          default_with_order.effective_frames[0].effective_row_indices ==
              std::vector<std::size_t>({0, 1, 2}),
      "ordered default was not RANGE unbounded preceding through peers");

  auto no_order = Window401Request();
  no_order.order_terms.clear();
  no_order.ordering_property_uuid.clear();
  no_order.physical_dag.nodes[1].required_property_uuids.pop_back();
  auto default_without_order = ExecuteFrame(no_order, omitted);
  passed &= Require401(
      default_without_order.diagnostic.ok &&
          default_without_order.defaulted_without_order &&
          !default_without_order.defaulted_with_order &&
          default_without_order.effective_frames[0].effective_row_indices ==
              std::vector<std::size_t>({0, 1, 2, 3, 4}),
      "unordered default did not cover the entire typed partition");

  const auto reversed = ExecuteFrame(
      Window401Request(),
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::rows,
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_following,
                     int64_one),
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding,
                     int64_one)));
  passed &= Require401(
      reversed.diagnostic.ok &&
          reversed.effective_frames[2].base_state ==
              exec::CanonicalWindowFrameState::reversed_to_empty &&
          !reversed.effective_frames[2].base_begin.has_value() &&
          !reversed.effective_frames[2].base_end_exclusive.has_value() &&
          reversed.effective_frames[2].effective_row_indices.empty(),
      "reversed frame was not represented as an explicit non-sentinel state");

  const auto adjacent_reversed = ExecuteFrame(
      Window401Request(),
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::rows,
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_following,
                     int64_one),
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row)));
  passed &= Require401(
      adjacent_reversed.diagnostic.ok &&
          adjacent_reversed.effective_frames[2].base_state ==
              exec::CanonicalWindowFrameState::reversed_to_empty &&
          !adjacent_reversed.effective_frames[2].base_begin.has_value() &&
          adjacent_reversed.effective_frames[2]
              .effective_row_indices.empty(),
      "adjacent reversed ROWS bounds collapsed into an ambiguous empty state");

  const auto edge_empty = ExecuteFrame(
      Window401Request(),
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::rows,
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_following,
                     exec::EncodeInt64Value(100)),
          FrameBound(exec::CanonicalWindowFrameBoundKind::unbounded_following)));
  passed &= Require401(
      edge_empty.diagnostic.ok &&
          edge_empty.effective_frames[0].base_state ==
              exec::CanonicalWindowFrameState::empty &&
          !edge_empty.effective_frames[0].base_begin.has_value(),
      "clamped empty frame became row-zero sentinel state");

  const std::vector<exec::CanonicalWindowFrameBoundKind> start_kinds = {
      exec::CanonicalWindowFrameBoundKind::unbounded_preceding,
      exec::CanonicalWindowFrameBoundKind::offset_preceding,
      exec::CanonicalWindowFrameBoundKind::current_row,
      exec::CanonicalWindowFrameBoundKind::offset_following};
  const std::vector<exec::CanonicalWindowFrameBoundKind> end_kinds = {
      exec::CanonicalWindowFrameBoundKind::offset_preceding,
      exec::CanonicalWindowFrameBoundKind::current_row,
      exec::CanonicalWindowFrameBoundKind::offset_following,
      exec::CanonicalWindowFrameBoundKind::unbounded_following};
  for (const auto unit : {exec::CanonicalWindowFrameUnit::rows,
                          exec::CanonicalWindowFrameUnit::groups,
                          exec::CanonicalWindowFrameUnit::range}) {
    for (const auto start_kind : start_kinds) {
      for (const auto end_kind : end_kinds) {
        auto start = FrameBound(start_kind);
        auto end = FrameBound(end_kind);
        const auto offset = unit == exec::CanonicalWindowFrameUnit::range
                                ? TypedOffset("int32", "0", 4940)
                                : int64_zero;
        if (start_kind ==
                exec::CanonicalWindowFrameBoundKind::offset_preceding ||
            start_kind ==
                exec::CanonicalWindowFrameBoundKind::offset_following) {
          start.offset = offset;
        }
        if (end_kind ==
                exec::CanonicalWindowFrameBoundKind::offset_preceding ||
            end_kind ==
                exec::CanonicalWindowFrameBoundKind::offset_following) {
          end.offset = offset;
        }
        const auto accepted = ExecuteFrame(
            unit == exec::CanonicalWindowFrameUnit::range
                ? one_order
                : Window401Request(),
            ExplicitFrame(unit, std::move(start), std::move(end)));
        passed &= Require401(accepted.diagnostic.ok,
                             "an accepted frame-bound combination refused");
      }
    }
  }
  return passed;
}

bool ValidateTemporalRangeAndCausalCarryThrough() {
  auto temporal = Window401Request();
  temporal.order_terms.pop_back();
  const auto descriptor = WindowDescriptor(
      4920, "timestamp",
      "type_uuid=" + WindowUuid(4921) + ";nullability=non_null");
  for (std::size_t row = 0; row < temporal.input_batch.rows.size(); ++row) {
    const auto day = static_cast<unsigned>((row % 4) + 1);
    temporal.input_batch.rows[row].values[2] = WindowValue(
        descriptor, "2026-07-0" + std::to_string(day) + "T12:00:00Z");
  }
  temporal.input_batch.columns[2].descriptor = descriptor;
  temporal.input_batch.columns[2].nullable = false;
  const auto interval = TypedOffset("interval", "P1D", 4930);
  const auto result = ExecuteFrame(
      temporal,
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::range,
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding,
                     interval),
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row)));
  bool passed = Require401(
      result.diagnostic.ok && !result.effective_frames.empty() &&
          result.selected_plan_uuid == temporal.physical_dag.selected_plan_uuid &&
          result.executed_physical_node_id == 2 &&
          result.causal_counter_id == 40102 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              temporal.physical_dag.mga_statement_context) &&
          result.mga_statement_context.visible_committed_high_watermark == 0 &&
          result.authority.engine_mga_snapshot_bound,
      "temporal RANGE or selected-plan/MGA evidence did not carry through: " +
          result.diagnostic.diagnostic_code + ":" + result.diagnostic.detail);
  const auto wider = ExecuteFrame(
      temporal,
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::range,
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding,
                     TypedOffset("interval", "P2D", 4931)),
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row)));
  passed &= Require401(
      wider.diagnostic.ok &&
          EffectiveReferenceCount(wider) > EffectiveReferenceCount(result),
      "temporal RANGE offset mutation did not change effective frames");

  auto invalid = temporal;
  invalid.mga_authority.resolve_current = {};
  const auto refused = ExecuteFrame(
      invalid,
      ExplicitFrame(
          exec::CanonicalWindowFrameUnit::range,
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding,
                     interval),
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row)));
  passed &= Require401(
      !refused.diagnostic.ok && refused.ordered_batch.rows.empty() &&
          refused.effective_frames.empty() &&
          refused.selected_plan_uuid.empty() &&
          refused.executed_physical_node_id == 0 &&
          !exec::PhysicalMgaStatementContextValid(
              refused.mga_statement_context),
      "missing current MGA resolver reached window frame access");
  return passed;
}

}  // namespace

#ifndef QOW_WIN_003_FIXTURE_ONLY
// QOW-TEST-WIN-003-V1
int main() {
  return ValidateRowsGroupsRangeAndDefaults() &&
                 ValidateTemporalRangeAndCausalCarryThrough()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
#endif
