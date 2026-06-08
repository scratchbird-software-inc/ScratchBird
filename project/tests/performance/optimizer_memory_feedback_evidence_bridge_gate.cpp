// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_memory_feedback_bridge.hpp"
#include "optimizer_feedback.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace opt = scratchbird::engine::optimizer;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

opt::OptimizerMemoryFeedbackEvidence ValidFeedback() {
  opt::OptimizerMemoryFeedbackEvidence feedback;
  feedback.query_uuid = "018f4f4c-1111-7111-8111-111111111111";
  feedback.scope_uuid = "018f4f4c-2222-7222-8222-222222222222";
  feedback.route_kind = "sql_select";
  feedback.route_label = "performance/hash_join";
  feedback.operator_family = "hash_join";
  feedback.plan_shape = "join(hash)->aggregate";
  feedback.plan_node_id = "performance-plan-node-hash-join";
  feedback.provenance_digest = "sha256:performance-memory-feedback-provenance";
  feedback.redaction_digest = "sha256:performance-memory-feedback-redaction";
  feedback.metric_snapshot_digest =
      "sha256:performance-memory-feedback-metrics";
  feedback.reservation_id = "reservation-performance-hash-join";
  feedback.reservation_token = "reservation-token-performance-hash-join";
  feedback.reservation_generation = 11;
  feedback.policy_generation = 9;
  feedback.feedback_generation = 4;
  feedback.catalog_epoch = 13;
  feedback.security_epoch = 17;
  feedback.redaction_epoch = 19;
  feedback.statistics_epoch = 23;
  feedback.observed_timestamp_ticks = 1000;
  feedback.received_timestamp_ticks = 1200;
  feedback.memory_grant_bytes = 128 * 1024;
  feedback.peak_memory_bytes = 192 * 1024;
  feedback.spill_bytes = 4096;
  feedback.governed_reservation = true;
  feedback.reservation_token_bound = true;
  feedback.resource_governance_ledger_recorded = true;
  return feedback;
}

}  // namespace

int main() {
  // MMCH_OPTIMIZER_MEMORY_FEEDBACK_EVIDENCE_BRIDGE
  auto accepted = opt::BuildOptimizerMemoryFeedbackForPlanner(ValidFeedback());
  Require(accepted.ok(), "valid governed memory feedback rejected");
  Require(accepted.runtime_feedback.advisory_only,
          "optimizer memory feedback must remain advisory");
  Require(!accepted.runtime_feedback.parser_or_donor_authority,
          "optimizer memory feedback accepted parser/donor authority");
  Require(accepted.runtime_feedback.transaction_finality_authority ==
              "engine_transaction_inventory",
          "optimizer memory feedback finality authority drifted");

  auto evaluated = opt::EvaluateOptimizerRuntimeFeedback(accepted.runtime_feedback);
  Require(evaluated.ok, "bridged memory feedback failed runtime evaluation");
  Require(evaluated.memory_grant.apply, "undergrant feedback should recommend grant change");

  auto stale = ValidFeedback();
  stale.received_timestamp_ticks = stale.max_age_ticks + 2000;
  auto stale_result = opt::BuildOptimizerMemoryFeedbackForPlanner(stale);
  Require(!stale_result.ok(), "stale memory feedback accepted");

  auto synthetic = ValidFeedback();
  synthetic.synthetic = true;
  auto synthetic_result = opt::BuildOptimizerMemoryFeedbackForPlanner(synthetic);
  Require(!synthetic_result.ok(), "synthetic memory feedback accepted");

  auto unsafe = ValidFeedback();
  unsafe.parser_client_or_donor_authority = true;
  auto unsafe_result = opt::BuildOptimizerMemoryFeedbackForPlanner(unsafe);
  Require(!unsafe_result.ok(), "parser/client/donor memory feedback accepted");

  auto ungoverned = ValidFeedback();
  ungoverned.governed_reservation = false;
  auto ungoverned_result = opt::BuildOptimizerMemoryFeedbackForPlanner(ungoverned);
  Require(!ungoverned_result.ok(), "ungoverned memory feedback accepted");

  std::cout << "MMCH_OPTIMIZER_MEMORY_FEEDBACK_EVIDENCE_BRIDGE: PASS\n";
  return EXIT_SUCCESS;
}
