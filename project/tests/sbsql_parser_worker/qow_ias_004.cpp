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
      FrameBound(static_cast<exec::CanonicalWindowFrameBoundKind>(0)),
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row));
  passed &= Require401(RefusedWithoutRows(ExecuteFrame(Window401Request(), frame)),
                       "unknown frame-bound kind was treated as a default");

  frame = ExplicitFrame(
      exec::CanonicalWindowFrameUnit::range,
      FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding,
                 TypedOffset("int32", "1")),
      FrameBound(exec::CanonicalWindowFrameBoundKind::current_row));
  passed &= Require401(RefusedWithoutRows(ExecuteFrame(Window401Request(), frame)),
                       "multi-term RANGE offset was accepted");

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

  exec::CanonicalWindowFrameRequest authority;
  authority.partition_order =
      exec::ExecuteCanonicalWindowPartitionOrder(Window401Request());
  authority.frame = ExplicitFrame(
      exec::CanonicalWindowFrameUnit::rows,
      FrameBound(exec::CanonicalWindowFrameBoundKind::unbounded_preceding),
      FrameBound(exec::CanonicalWindowFrameBoundKind::unbounded_following));
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
  authority.maximum_effective_row_references = 1;
  passed &= Require401(
      RefusedWithoutRows(exec::ExecuteCanonicalWindowFrames(authority)),
      "effective-frame resource overflow returned partial rows");
  return passed;
}

}  // namespace

// QOW-TEST-IAS-004-V1
int main() {
  return ValidateFrameRefusalsAndAuthority() ? EXIT_SUCCESS : EXIT_FAILURE;
}
