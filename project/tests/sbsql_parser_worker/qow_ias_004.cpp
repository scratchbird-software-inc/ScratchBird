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

bool RefusedWithoutRows(const exec::CanonicalWindowFrameResult& result) {
  return !result.diagnostic.ok &&
         result.diagnostic.diagnostic_code == "QOW-DIAG-WINDOW-FRAME" &&
         result.ordered_batch.rows.empty() && result.effective_frames.empty();
}

bool ValidateFrameRefusalsAndAuthority() {
  bool passed = true;
  auto frame = ExplicitFrame(
      exec::CanonicalWindowFrameUnit::rows,
      FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding),
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row));
  passed &= Require401(RefusedWithoutRows(ExecuteFrame(Window401Request(), frame)),
                       "missing ROWS offset was defaulted");

  frame = ExplicitFrame(
      exec::CanonicalWindowFrameUnit::rows,
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row,
                 exec::EncodeInt64Value(1)),
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row));
  passed &= Require401(RefusedWithoutRows(ExecuteFrame(Window401Request(), frame)),
                       "offset on CURRENT ROW was ignored");

  frame = ExplicitFrame(
      exec::CanonicalWindowFrameUnit::groups,
      FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding,
                 exec::EncodeInt64Value(-1)),
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row));
  passed &= Require401(RefusedWithoutRows(ExecuteFrame(Window401Request(), frame)),
                       "negative GROUPS offset entered execution");

  frame = ExplicitFrame(
      exec::CanonicalWindowFrameUnit::rows,
      FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding,
                 TypedOffset("int64", "9223372036854775808", 4950)),
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row));
  passed &= Require401(RefusedWithoutRows(ExecuteFrame(Window401Request(), frame)),
                       "overflowing ROWS offset entered execution");

  auto null_groups_offset = TypedOffset("int64", "1", 4951);
  null_groups_offset.setState(api::EngineValueState::sql_null);
  null_groups_offset.encoded_value.clear();
  frame = ExplicitFrame(
      exec::CanonicalWindowFrameUnit::groups,
      FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding,
                 null_groups_offset),
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row));
  passed &= Require401(RefusedWithoutRows(ExecuteFrame(Window401Request(), frame)),
                       "NULL GROUPS offset entered execution");

  frame = ExplicitFrame(
      exec::CanonicalWindowFrameUnit::rows,
      FrameBound(static_cast<exec::CanonicalWindowFrameBoundKind>(0)),
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row));
  passed &= Require401(RefusedWithoutRows(ExecuteFrame(Window401Request(), frame)),
                       "unknown frame-bound kind was treated as a default");

  frame = ExplicitFrame(
      exec::CanonicalWindowFrameUnit::rows,
      FrameBound(exec::CanonicalWindowFrameBoundKind::unbounded_following),
      FrameBound(exec::CanonicalWindowFrameBoundKind::unbounded_following));
  passed &= Require401(RefusedWithoutRows(ExecuteFrame(Window401Request(), frame)),
                       "UNBOUNDED FOLLOWING was accepted as a start bound");

  frame = ExplicitFrame(
      exec::CanonicalWindowFrameUnit::rows,
      FrameBound(exec::CanonicalWindowFrameBoundKind::unbounded_preceding),
      FrameBound(exec::CanonicalWindowFrameBoundKind::unbounded_preceding));
  passed &= Require401(RefusedWithoutRows(ExecuteFrame(Window401Request(), frame)),
                       "UNBOUNDED PRECEDING was accepted as an end bound");

  frame = ExplicitFrame(
      static_cast<exec::CanonicalWindowFrameUnit>(0),
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row),
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row));
  passed &= Require401(RefusedWithoutRows(ExecuteFrame(Window401Request(), frame)),
                       "unknown frame unit entered execution");

  frame = ExplicitFrame(
      exec::CanonicalWindowFrameUnit::rows,
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row),
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row),
      static_cast<exec::CanonicalWindowFrameExclusion>(0));
  passed &= Require401(RefusedWithoutRows(ExecuteFrame(Window401Request(), frame)),
                       "unknown frame exclusion entered execution");

  exec::CanonicalWindowFrameDescriptor contradictory_omission;
  contradictory_omission.frame_descriptor_uuid = WindowUuid(4957);
  contradictory_omission.start =
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row);
  passed &= Require401(
      RefusedWithoutRows(
          ExecuteFrame(Window401Request(), contradictory_omission)),
      "omitted frame with an explicit bound was silently defaulted");

  frame = ExplicitFrame(
      exec::CanonicalWindowFrameUnit::range,
      FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding,
                 TypedOffset("int32", "1")),
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row));
  passed &= Require401(RefusedWithoutRows(ExecuteFrame(Window401Request(), frame)),
                       "multi-term RANGE offset was accepted");

  auto one_order = Window401Request();
  one_order.order_terms.pop_back();
  frame = ExplicitFrame(
      exec::CanonicalWindowFrameUnit::range,
      FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding,
                 TypedOffset("int32", "-1", 4952)),
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row));
  passed &= Require401(RefusedWithoutRows(ExecuteFrame(one_order, frame)),
      "negative numeric RANGE offset entered execution");

  auto no_order = Window401Request();
  no_order.order_terms.clear();
  no_order.ordering_property_uuid.clear();
  no_order.physical_dag.nodes[1].required_property_uuids.pop_back();
  frame = ExplicitFrame(
      exec::CanonicalWindowFrameUnit::range,
      FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding,
                 TypedOffset("int32", "1", 4958)),
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row));
  passed &= Require401(RefusedWithoutRows(ExecuteFrame(no_order, frame)),
                       "RANGE offset without an order term entered execution");

  auto text_order = Window401Request();
  text_order.order_terms = {text_order.order_terms.back()};
  frame = ExplicitFrame(
      exec::CanonicalWindowFrameUnit::range,
      FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding,
                 TypedOffset("int32", "1", 4953)),
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row));
  passed &= Require401(RefusedWithoutRows(ExecuteFrame(text_order, frame)),
                       "non-numeric non-temporal RANGE order entered execution");

  auto overflow_request = Window401Request();
  overflow_request.order_terms.pop_back();
  for (auto& row : overflow_request.input_batch.rows) {
    row.values[2].encoded_value = std::to_string(
        std::numeric_limits<std::int64_t>::max());
  }
  frame = ExplicitFrame(
      exec::CanonicalWindowFrameUnit::range,
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row),
      FrameBound(exec::CanonicalWindowFrameBoundKind::offset_following,
                 TypedOffset("int32", "1")));
  passed &= Require401(RefusedWithoutRows(ExecuteFrame(overflow_request, frame)),
                       "overflowing RANGE threshold produced a frame");

  auto temporal_overflow = Window401Request();
  temporal_overflow.order_terms.pop_back();
  const auto timestamp_descriptor = WindowDescriptor(
      4954, "timestamp",
      "type_uuid=" + WindowUuid(4955) + ";nullability=non_null");
  temporal_overflow.input_batch.columns[2].descriptor = timestamp_descriptor;
  for (auto& row : temporal_overflow.input_batch.rows) {
    row.values[2] =
        WindowValue(timestamp_descriptor, "9999-12-31T23:59:59Z");
  }
  frame = ExplicitFrame(
      exec::CanonicalWindowFrameUnit::range,
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row),
      FrameBound(exec::CanonicalWindowFrameBoundKind::offset_following,
                 TypedOffset("interval", "P1D", 4956)));
  passed &= Require401(
      RefusedWithoutRows(ExecuteFrame(temporal_overflow, frame)),
      "overflowing temporal RANGE threshold produced a frame");

  exec::CanonicalWindowFrameRequest authority;
  authority.partition_order =
      exec::ExecuteCanonicalWindowPartitionOrder(Window401Request());
  authority.mga_authority = Window401Request().mga_authority;
  authority.frame = ExplicitFrame(
      exec::CanonicalWindowFrameUnit::rows,
      FrameBound(exec::CanonicalWindowFrameBoundKind::unbounded_preceding),
      FrameBound(exec::CanonicalWindowFrameBoundKind::unbounded_following));
  authority.frame_property_binding_evidence_uuid = WindowUuid(4903);
  authority.parser_execution_authority_claimed = true;
  passed &= Require401(
      RefusedWithoutRows(exec::ExecuteCanonicalWindowFrames(authority)),
      "parser execution authority entered canonical frame construction");

  authority.parser_execution_authority_claimed = false;
  authority.partition_order.row_metadata[0].peer_begin = 1;
  passed &= Require401(
      RefusedWithoutRows(exec::ExecuteCanonicalWindowFrames(authority)),
      "forged peer metadata entered canonical frame construction");

  authority.partition_order =
      exec::ExecuteCanonicalWindowPartitionOrder(Window401Request());
  authority.partition_order.row_metadata[1].source_row_index =
      authority.partition_order.row_metadata[0].source_row_index;
  passed &= Require401(
      RefusedWithoutRows(exec::ExecuteCanonicalWindowFrames(authority)),
      "duplicate stable source ordinal entered frame construction");

  authority.partition_order =
      exec::ExecuteCanonicalWindowPartitionOrder(Window401Request());
  authority.partition_order.row_metadata[2].peer_group_id = 7;
  passed &= Require401(
      RefusedWithoutRows(exec::ExecuteCanonicalWindowFrames(authority)),
      "discontinuous peer identity entered frame construction");

  authority.partition_order =
      exec::ExecuteCanonicalWindowPartitionOrder(Window401Request());
  authority.partition_order.ordered_key_batch =
      authority.partition_order.ordered_batch;
  passed &= Require401(
      RefusedWithoutRows(exec::ExecuteCanonicalWindowFrames(authority)),
      "unconsumed forged comparison-key column entered frame construction");

  authority.partition_order =
      exec::ExecuteCanonicalWindowPartitionOrder(Window401Request());
  authority.frame_property_binding_evidence_uuid = WindowUuid(4903);
  authority.maximum_effective_row_references = 1;
  passed &= Require401(
      RefusedWithoutRows(exec::ExecuteCanonicalWindowFrames(authority)),
      "effective-frame resource overflow returned partial rows");

  exec::CanonicalWindowFrameRequest unbound;
  unbound.partition_order =
      exec::ExecuteCanonicalWindowPartitionOrder(Window401Request());
  unbound.mga_authority = Window401Request().mga_authority;
  unbound.frame = ExplicitFrame(
      exec::CanonicalWindowFrameUnit::rows,
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row),
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row));
  passed &= Require401(
      RefusedWithoutRows(exec::ExecuteCanonicalWindowFrames(unbound)),
      "unbound frame descriptor entered execution");

  unbound.frame_property_binding_evidence_uuid =
      unbound.frame.frame_descriptor_uuid;
  passed &= Require401(
      RefusedWithoutRows(exec::ExecuteCanonicalWindowFrames(unbound)),
      "frame descriptor identity substituted for its binding receipt");
  return passed;
}

}  // namespace

// QOW-TEST-IAS-004-V1
int main() {
  return ValidateFrameRefusalsAndAuthority() ? EXIT_SUCCESS : EXIT_FAILURE;
}
