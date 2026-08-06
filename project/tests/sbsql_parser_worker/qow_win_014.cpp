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

bool ValidateOptimizerPeerIdentityCarryThrough() {
  const auto request = Window401Request();
  const auto result = exec::ExecuteCanonicalWindowPartitionOrder(request);
  bool passed = true;
  passed &= Require401(
      result.diagnostic.ok &&
          result.window_property_uuid == request.window_property_uuid &&
          result.partition_property_uuid == request.partition_property_uuid &&
          result.ordering_property_uuid == request.ordering_property_uuid &&
          result.term_binding_evidence_uuid ==
              request.term_binding_evidence_uuid &&
          result.deterministic_tie_evidence_uuid ==
              request.deterministic_tie_evidence_uuid &&
          result.selected_plan_uuid == request.physical_dag.selected_plan_uuid &&
          result.executed_physical_node_id == 2 &&
          result.causal_counter_id == 40102 &&
          result.weaker_peer_recomputation_forbidden,
      "optimizer property or selected-node peer identity did not reach execution");
  if (!result.diagnostic.ok) {
    std::cerr << "QOW-TEST-WIN-401-V1: initial WIN-014 refusal "
              << result.diagnostic.diagnostic_code << ": "
              << result.diagnostic.detail << '\n';
    return false;
  }

  auto mutated = Window401Request();
  mutated.physical_dag.nodes[1].required_property_uuids.erase(
      mutated.physical_dag.nodes[1].required_property_uuids.begin() + 1);
  auto refused = exec::ExecuteCanonicalWindowPartitionOrder(mutated);
  passed &= Require401(
      !refused.diagnostic.ok &&
          refused.diagnostic.diagnostic_code == "QOW-DIAG-WINDOW-ORDER" &&
          refused.row_metadata.empty(),
      "dropped optimizer ordering property was recomputed in execution");

  mutated = Window401Request();
  mutated.physical_dag.nodes[1].delivered_property_uuids.front() =
      WindowUuid(4999);
  refused = exec::ExecuteCanonicalWindowPartitionOrder(mutated);
  passed &= Require401(
      !refused.diagnostic.ok &&
          refused.diagnostic.diagnostic_code == "QOW-DIAG-WINDOW-PEER" &&
          refused.row_metadata.empty(),
      "drifted optimizer window property was accepted as peer identity");

  mutated = Window401Request();
  mutated.physical_dag.nodes[1].required_property_uuids.push_back(
      WindowUuid(4997));
  refused = exec::ExecuteCanonicalWindowPartitionOrder(mutated);
  passed &= Require401(
      !refused.diagnostic.ok && refused.diagnostic.diagnostic_code ==
                                    "QOW-DIAG-WINDOW-PROPERTY-BINDING" &&
          refused.row_metadata.empty(),
      "unbound optimizer property entered Window term execution");

  mutated = Window401Request();
  mutated.deterministic_tie_evidence_uuid =
      mutated.ordering_property_uuid;
  refused = exec::ExecuteCanonicalWindowPartitionOrder(mutated);
  passed &= Require401(
      !refused.diagnostic.ok &&
          refused.diagnostic.diagnostic_code == "QOW-DIAG-WINDOW-TIE" &&
          refused.row_metadata.empty(),
      "ordering property identity was substituted for stable tie evidence");

  mutated = Window401Request();
  mutated.physical_dag.abi_version = 1;
  mutated.physical_dag.statement_snapshot_id = 1;
  mutated.physical_dag.admission_evidence[3].evidence_uuid = WindowUuid(4998);
  refused = exec::ExecuteCanonicalWindowPartitionOrder(mutated);
  passed &= Require401(
      !refused.diagnostic.ok &&
          refused.diagnostic.diagnostic_code == "QOW-DIAG-WINDOW-PEER" &&
          refused.row_metadata.empty(),
      "legacy physical evidence root entered typed peer execution");
  return passed;
}

}  // namespace

// QOW-TEST-WIN-014-V1
int main() {
  return ValidateOptimizerPeerIdentityCarryThrough() ? EXIT_SUCCESS
                                                     : EXIT_FAILURE;
}
