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
  passed &= Require401(
      result.row_metadata[0].source_row_index == 1 &&
          result.row_metadata[1].source_row_index == 5 &&
          result.row_metadata[6].source_row_index == 3 &&
          result.row_metadata[7].source_row_index == 4 &&
          result.stable_ties_preserved &&
          result.deterministic_tie_evidence_uuid ==
              Window401Request().deterministic_tie_evidence_uuid,
      "semantic peers did not preserve the evidenced stable tie order");

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

  request = Window401Request();
  request.deterministic_tie_evidence_uuid.clear();
  mutated = exec::ExecuteCanonicalWindowPartitionOrder(request);
  passed &= Require401(
      !mutated.diagnostic.ok &&
          mutated.diagnostic.diagnostic_code == "QOW-DIAG-WINDOW-TIE" &&
          mutated.row_metadata.empty(),
      "missing deterministic tie evidence reached window ordering");

  request = Window401Request();
  request.term_binding_evidence_uuid.clear();
  mutated = exec::ExecuteCanonicalWindowPartitionOrder(request);
  passed &= Require401(
      !mutated.diagnostic.ok && mutated.diagnostic.diagnostic_code ==
                                     "QOW-DIAG-WINDOW-PROPERTY-BINDING" &&
          mutated.row_metadata.empty(),
      "unbound runtime terms reached window comparisons");

  request = Window401Request();
  request.partition_terms.clear();
  request.order_terms.clear();
  request.partition_property_uuid.clear();
  request.ordering_property_uuid.clear();
  request.term_binding_evidence_uuid.clear();
  request.physical_dag.nodes[1].required_property_uuids.clear();
  mutated = exec::ExecuteCanonicalWindowPartitionOrder(request);
  passed &= Require401(
      mutated.diagnostic.ok && mutated.partition_count == 1 &&
          mutated.peer_group_count == 1 &&
          WindowPayloads(mutated.ordered_batch) ==
              WindowPayloads(request.input_batch) &&
          mutated.stable_ties_preserved,
      "global unordered Window did not retain one explicit peer and stable input order");

  request = Window401Request();
  exec::DescriptorBatch hidden_keys;
  hidden_keys.columns = {request.input_batch.columns[1],
                         request.input_batch.columns[2]};
  for (const auto& row : request.input_batch.rows) {
    hidden_keys.rows.push_back({{row.values[1], row.values[2]}});
  }
  request.key_batch = hidden_keys;
  request.partition_terms = {{.column = 0,
                              .expression_descriptor_id = 4002}};
  exec::CanonicalDescriptorOrderTerm hidden_order;
  hidden_order.column = 1;
  hidden_order.expression_descriptor_id = 4003;
  hidden_order.direction = exec::CanonicalDescriptorOrderDirection::ascending;
  hidden_order.null_placement = exec::CanonicalDescriptorNullPlacement::last;
  request.order_terms = {hidden_order};
  mutated = exec::ExecuteCanonicalWindowPartitionOrder(request);
  passed &= Require401(
      mutated.diagnostic.ok && mutated.partition_count == 2 &&
          mutated.ordered_batch.columns.size() == 5 &&
          mutated.ordered_batch.rows.front().values[4].encoded_value == "101",
      "engine-materialized hidden keys changed the payload schema or order");

  request.key_batch->rows.pop_back();
  mutated = exec::ExecuteCanonicalWindowPartitionOrder(request);
  passed &= Require401(
      !mutated.diagnostic.ok && mutated.diagnostic.diagnostic_code ==
                                     "QOW-DIAG-WINDOW-PROPERTY-BINDING" &&
          mutated.ordered_batch.rows.empty(),
      "misaligned materialized key cardinality produced partial output");
  return passed;
}

}  // namespace

// QOW-TEST-WIN-005-V1
int main() {
  return ValidateTypedOrderAndExplicitPeers() ? EXIT_SUCCESS : EXIT_FAILURE;
}
