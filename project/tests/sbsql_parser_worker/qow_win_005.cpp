// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define QOW_WIN_004_FIXTURE_ONLY
#include "qow_win_004.cpp"

namespace {

bool ValidateTypedOrderAndExplicitPeers() {
  const auto result =
      exec::ExecuteCanonicalWindowPartitionOrder(Window401Request());
  bool passed = true;
  passed &= Require401(
      result.diagnostic.ok && result.row_metadata.size() == 9,
      "typed window order and peer stage was not accepted");
  if (!result.diagnostic.ok || result.row_metadata.size() != 9) {
    return false;
  }
  passed &= Require401(
      result.row_metadata[0].peer_group_id.has_value() &&
          *result.row_metadata[0].peer_group_id == 0 &&
          result.row_metadata[0].peer_begin == 0 &&
          result.row_metadata[0].peer_end_exclusive == 2 &&
          result.row_metadata[1].peer_group_id == 0 &&
          result.row_metadata[2].peer_group_id == 1 &&
          result.row_metadata[4].peer_group_id == 3,
      "peer group zero or explicit peer ranges became sentinel state");
  passed &= Require401(
      result.row_metadata[6].peer_group_id == 0 &&
          result.row_metadata[6].peer_begin == 6 &&
          result.row_metadata[6].peer_end_exclusive == 8 &&
          result.row_metadata[7].peer_group_id == 0,
      "typed NULL/collation peers were not retained within their partition");

  auto request = Window401Request();
  request.order_terms[1].direction =
      exec::CanonicalDescriptorOrderDirection::ascending;
  auto mutated = exec::ExecuteCanonicalWindowPartitionOrder(request);
  passed &= Require401(
      mutated.diagnostic.ok &&
          WindowPayloads(mutated.ordered_batch) ==
              std::vector<std::string>({"108", "101", "105", "100", "107",
                                        "102", "103", "104", "106"}),
      "second typed order term or direction was ignored");

  request = Window401Request();
  request.order_terms[0].null_placement =
      exec::CanonicalDescriptorNullPlacement::first;
  mutated = exec::ExecuteCanonicalWindowPartitionOrder(request);
  passed &= Require401(
      mutated.diagnostic.ok &&
          WindowPayloads(mutated.ordered_batch).front() == "107",
      "explicit NULL placement was ignored");

  request = Window401Request();
  request.order_terms[1].collation_epoch = 0;
  mutated = exec::ExecuteCanonicalWindowPartitionOrder(request);
  passed &= Require401(
      !mutated.diagnostic.ok &&
          mutated.diagnostic.diagnostic_code == "QOW-DIAG-WINDOW-ORDER" &&
          mutated.row_metadata.empty(),
      "unbound order collation authority produced peer metadata");

  request = Window401Request();
  request.maximum_pair_comparisons = 255;
  mutated = exec::ExecuteCanonicalWindowPartitionOrder(request);
  passed &= Require401(
      !mutated.diagnostic.ok &&
          mutated.diagnostic.diagnostic_code == "QOW-DIAG-WINDOW-PEER" &&
          mutated.ordered_batch.rows.empty(),
      "window comparison resource bound was exceeded");
  return passed;
}

}  // namespace

// QOW-TEST-WIN-005-V1
int main() {
  return ValidateTypedOrderAndExplicitPeers() ? EXIT_SUCCESS : EXIT_FAILURE;
}
