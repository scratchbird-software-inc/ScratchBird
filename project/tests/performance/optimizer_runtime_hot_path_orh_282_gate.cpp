// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "adaptive_cardinality_feedback.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace opt = scratchbird::engine::optimizer;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "ORH-282 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 std::string_view needle) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) {
    return item.find(needle) != std::string::npos;
  });
}

std::string StableHash(std::vector<std::string> rows) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const auto& row : rows) {
    for (const unsigned char ch : row) {
      hash ^= ch;
      hash *= 1099511628211ull;
    }
    hash ^= 0xffu;
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << "fnv1a64:" << std::hex << hash;
  return out.str();
}

opt::CostVector BaselineCost() {
  opt::CostVector cost;
  cost.startup_cost = 20;
  cost.row_cost = 100;
  cost.io_cost = 40;
  cost.memory_cost = 10;
  cost.total_cost = 170;
  cost.reason = "orh282.baseline_cost";
  cost.confidence = opt::CostConfidence::kMedium;
  cost.selectable = true;
  return cost;
}

opt::OptimizerRuntimeFeedback RuntimeFeedback() {
  opt::OptimizerRuntimeFeedback feedback;
  feedback.operator_family = "join_predicate";
  feedback.plan_shape = "hash_join_with_bind_filter";
  feedback.cost_profile_id = "orh282_feedback_v1";
  feedback.estimated_rows = 8;
  feedback.actual_rows = 512;
  feedback.estimated_pages = 4;
  feedback.actual_pages = 16;
  feedback.estimated_io_operations = 4;
  feedback.actual_io_operations = 12;
  feedback.estimated_visibility_recheck_rows = 8;
  feedback.actual_visibility_recheck_rows = 512;
  feedback.memory_grant_bytes = 64 * 1024;
  feedback.peak_memory_bytes = 80 * 1024;
  feedback.estimated_latency_microseconds = 100;
  feedback.actual_latency_microseconds = 900;
  feedback.estimated_resource_units = 12;
  feedback.actual_resource_units = 512;
  feedback.freshness_microseconds = 1000;
  feedback.max_freshness_microseconds = 60000000;
  feedback.policy_allowed = true;
  feedback.advisory_only = true;
  feedback.mga_visibility_recheck_preserved = true;
  feedback.parser_or_donor_authority = false;
  feedback.transaction_finality_authority = "engine_transaction_inventory";
  return feedback;
}

opt::AdaptiveCardinalityFeedbackRequest Request() {
  opt::AdaptiveCardinalityFeedbackRequest request;
  request.feedback = RuntimeFeedback();
  request.baseline_cost = BaselineCost();
  request.authority.engine_mga_snapshot_bound = true;
  request.authority.transaction_inventory_authoritative = true;
  request.authority.security_recheck_required = true;
  request.authority.exact_recheck_required = true;
  request.epochs.feedback_generation = 282;
  request.epochs.expected_feedback_generation = 282;
  request.epochs.feedback_epoch = 2820;
  request.epochs.catalog_epoch = 28200;
  request.epochs.expected_catalog_epoch = 28200;
  request.epochs.security_epoch = 282000;
  request.epochs.expected_security_epoch = 282000;
  request.plan.route_label = "orh282.adaptive.feedback.join";
  request.plan.baseline_plan_hash =
      StableHash({"scan customers", "join orders", "estimate rows=8"});
  request.plan.variant_plan_hash =
      StableHash({"scan customers", "join orders", "bind-sensitive rows=512"});
  request.plan.fallback_plan_hash = request.plan.baseline_plan_hash;
  request.plan.result_hash =
      StableHash({"alice-visible row 1", "alice-visible row 2"});
  request.plan.fallback_result_hash = request.plan.result_hash;
  request.plan.runtime_consumed = true;
  request.plan.exact_fallback_available = true;
  request.bind_sensitive_variant_requested = true;
  request.misestimate_quarantine_requested = true;
  request.extended_stat_request_requested = true;
  request.extended_stat_source_authoritative = true;
  return request;
}

void RequireAccepted(const opt::AdaptiveCardinalityFeedbackResult& result) {
  Require(result.ok && result.benchmark_clean,
          "adaptive feedback was not benchmark-clean");
  Require(result.diagnostic_code ==
              "ORH_ADAPTIVE_CARDINALITY_PLAN_FEEDBACK.OK",
          "unexpected accepted diagnostic");
  Require(result.bind_sensitive_variant_created,
          "bind-sensitive variant was not created");
  Require(result.misestimate_quarantined,
          "misestimate quarantine was not created");
  Require(result.extended_stat_requested,
          "extended-stat request was not created");
  Require(result.feedback_status.applied,
          "runtime feedback was not consumed by optimizer evaluator");
  Require(result.adjusted_cost.reason.find("feedback=") != std::string::npos,
          "calibrated feedback cost was not applied");
  Require(HasEvidence(result.evidence,
                      "adaptive_feedback.route_label=orh282.adaptive.feedback.join"),
          "route label evidence missing");
  Require(HasEvidence(result.evidence,
                      "adaptive_feedback.feedback_generation=282"),
          "feedback generation evidence missing");
  Require(HasEvidence(result.evidence,
                      "adaptive_feedback.catalog_epoch=28200"),
          "catalog epoch evidence missing");
  Require(HasEvidence(result.evidence,
                      "adaptive_feedback.security_epoch=282000"),
          "security epoch evidence missing");
  Require(HasEvidence(result.evidence,
                      "adaptive_feedback.result_equivalence=true"),
          "result-equivalence evidence missing");
  Require(HasEvidence(result.evidence,
                      "adaptive_feedback.bind_sensitive_variant=true"),
          "bind-sensitive evidence missing");
  Require(HasEvidence(result.evidence,
                      "adaptive_feedback.misestimate_quarantine=true"),
          "quarantine evidence missing");
  Require(HasEvidence(result.evidence,
                      "adaptive_feedback.extended_stat_request=true"),
          "extended-stat evidence missing");
  Require(HasEvidence(result.evidence,
                      "adaptive_feedback.mga_finality_authority=engine_transaction_inventory"),
          "MGA authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "adaptive_feedback.security_recheck_required=true"),
          "security recheck evidence missing");
  Require(HasEvidence(result.evidence,
                      "adaptive_feedback.exact_recheck_required=true"),
          "exact recheck evidence missing");
  Require(HasEvidence(result.evidence,
                      "adaptive_feedback.parser_authority=false"),
          "parser non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "adaptive_feedback.visibility_authority=false"),
          "visibility non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "adaptive_feedback.finality_authority=false"),
          "finality non-authority evidence missing");
}

void RequireRejected(const opt::AdaptiveCardinalityFeedbackRequest& request,
                     std::string_view expected_diagnostic,
                     std::string_view expected_evidence) {
  const auto result = opt::EvaluateAdaptiveCardinalityFeedback(request);
  Require(!result.benchmark_clean,
          "negative adaptive feedback case was benchmark-clean");
  Require(result.diagnostic_code == expected_diagnostic,
          "negative adaptive feedback diagnostic mismatch");
  Require(HasEvidence(result.evidence, expected_evidence),
          "negative adaptive feedback evidence missing");
}

}  // namespace

int main() {
  const auto accepted = opt::EvaluateAdaptiveCardinalityFeedback(Request());
  RequireAccepted(accepted);

  auto parser_authority = Request();
  parser_authority.authority.parser_client_or_donor_feedback_authority = true;
  RequireRejected(parser_authority,
                  "ORH_ADAPTIVE_FEEDBACK_UNSAFE_AUTHORITY",
                  "refused=ORH_ADAPTIVE_FEEDBACK_UNSAFE_AUTHORITY");

  auto donor_authority = Request();
  donor_authority.feedback.parser_or_donor_authority = true;
  RequireRejected(donor_authority,
                  "ORH_ADAPTIVE_FEEDBACK_UNSAFE_AUTHORITY",
                  "feedback_must_be_optimizer_advisory_only");

  auto stale_feedback = Request();
  stale_feedback.feedback.freshness_microseconds =
      stale_feedback.feedback.max_freshness_microseconds + 1;
  const auto stale = opt::EvaluateAdaptiveCardinalityFeedback(stale_feedback);
  Require(!stale.benchmark_clean && stale.fallback_used,
          "stale feedback did not refuse benchmark-clean with exact fallback");
  Require(stale.diagnostic_code == "ORH_ADAPTIVE_FEEDBACK_STALE",
          "stale feedback diagnostic mismatch");

  auto stale_generation = Request();
  stale_generation.epochs.feedback_generation = 281;
  RequireRejected(stale_generation,
                  "ORH_ADAPTIVE_FEEDBACK_EPOCH_MISMATCH",
                  "feedback_catalog_or_security_epoch_mismatch");

  auto result_mismatch = Request();
  result_mismatch.plan.fallback_result_hash = StableHash({"different row"});
  RequireRejected(result_mismatch,
                  "ORH_ADAPTIVE_FEEDBACK_RESULT_MISMATCH",
                  "result_equivalence_mismatch");

  auto no_runtime = Request();
  no_runtime.plan.runtime_consumed = false;
  RequireRejected(no_runtime,
                  "ORH_ADAPTIVE_FEEDBACK_NO_RUNTIME",
                  "runtime_consumption_missing");

  auto no_mga = Request();
  no_mga.authority.transaction_inventory_authoritative = false;
  RequireRejected(no_mga,
                  "ORH_ADAPTIVE_FEEDBACK_MGA_UNPROVEN",
                  "engine_mga_transaction_inventory_required");

  auto no_security = Request();
  no_security.authority.security_recheck_required = false;
  RequireRejected(no_security,
                  "ORH_ADAPTIVE_FEEDBACK_SECURITY_UNPROVEN",
                  "security_and_exact_recheck_required");

  auto feedback_as_finality = Request();
  feedback_as_finality.authority.feedback_visibility_or_finality_authority =
      true;
  RequireRejected(feedback_as_finality,
                  "ORH_ADAPTIVE_FEEDBACK_UNSAFE_AUTHORITY",
                  "feedback_must_be_optimizer_advisory_only");

  auto unsafe_security_epoch_reuse = Request();
  unsafe_security_epoch_reuse.epochs.security_epoch = 1;
  RequireRejected(unsafe_security_epoch_reuse,
                  "ORH_ADAPTIVE_FEEDBACK_EPOCH_MISMATCH",
                  "feedback_catalog_or_security_epoch_mismatch");

  auto unsafe_catalog_epoch_reuse = Request();
  unsafe_catalog_epoch_reuse.epochs.catalog_epoch = 1;
  RequireRejected(unsafe_catalog_epoch_reuse,
                  "ORH_ADAPTIVE_FEEDBACK_EPOCH_MISMATCH",
                  "feedback_catalog_or_security_epoch_mismatch");

  auto no_fallback = Request();
  no_fallback.plan.exact_fallback_available = false;
  RequireRejected(no_fallback,
                  "ORH_ADAPTIVE_FEEDBACK_EXACT_FALLBACK_UNAVAILABLE",
                  "misestimate_quarantine_requires_exact_fallback");

  auto unsafe_extended_stat = Request();
  unsafe_extended_stat.extended_stat_source_authoritative = false;
  RequireRejected(unsafe_extended_stat,
                  "ORH_ADAPTIVE_FEEDBACK_EXTENDED_STAT_SOURCE_UNSAFE",
                  "extended_stat_request_requires_optimizer_authority");

  auto dominance_overclaim = Request();
  dominance_overclaim.plan.benchmark_or_donor_dominance_claim = true;
  RequireRejected(dominance_overclaim,
                  "ORH_ADAPTIVE_FEEDBACK_DOMINANCE_OVERCLAIM",
                  "adaptive_feedback_is_not_donor_dominance");

  std::cout << "ORH-282 adaptive cardinality feedback gate passed\n";
  return EXIT_SUCCESS;
}
