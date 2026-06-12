// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "adaptive_cardinality_feedback.hpp"

#include "optimizer_cost_full.hpp"

#include <string>

namespace scratchbird::engine::optimizer {
namespace {

void AddEvidence(AdaptiveCardinalityFeedbackResult* result,
                 std::string evidence) {
  result->evidence.push_back(std::move(evidence));
}

AdaptiveCardinalityFeedbackResult Refuse(
    const AdaptiveCardinalityFeedbackRequest& request,
    std::string diagnostic,
    std::string fallback_reason) {
  AdaptiveCardinalityFeedbackResult result;
  result.ok = false;
  result.benchmark_clean = false;
  result.fail_closed = !request.plan.exact_fallback_available;
  result.fallback_used = request.plan.exact_fallback_available;
  result.diagnostic_code = std::move(diagnostic);
  result.fallback_reason = std::move(fallback_reason);
  result.adjusted_cost = request.baseline_cost;
  AddEvidence(&result, "adaptive_feedback.route_label=" + request.plan.route_label);
  AddEvidence(&result, "adaptive_feedback.refused=" + result.diagnostic_code);
  AddEvidence(&result, "adaptive_feedback.fallback_reason=" + result.fallback_reason);
  if (result.fallback_used) {
    AddEvidence(&result, "adaptive_feedback.exact_fallback_used=true");
  } else {
    AddEvidence(&result, "adaptive_feedback.exact_fallback_used=false");
  }
  AddEvidence(&result, "adaptive_feedback.benchmark_clean=false");
  AddEvidence(&result, "adaptive_feedback.parser_authority=false");
  AddEvidence(&result, "adaptive_feedback.reference_authority=false");
  AddEvidence(&result, "adaptive_feedback.client_authority=false");
  AddEvidence(&result, "adaptive_feedback.visibility_authority=false");
  AddEvidence(&result, "adaptive_feedback.finality_authority=false");
  AddEvidence(&result, "adaptive_feedback.recovery_authority=false");
  AddEvidence(&result,
              "adaptive_feedback.mga_finality_authority=engine_transaction_inventory");
  return result;
}

bool MissingHash(const AdaptiveFeedbackPlanCapture& plan) {
  return plan.baseline_plan_hash.empty() || plan.variant_plan_hash.empty() ||
         plan.fallback_plan_hash.empty() || plan.result_hash.empty() ||
         plan.fallback_result_hash.empty();
}

}  // namespace

AdaptiveCardinalityFeedbackResult EvaluateAdaptiveCardinalityFeedback(
    const AdaptiveCardinalityFeedbackRequest& request) {
  if (request.plan.route_label.empty()) {
    return Refuse(request,
                  "ORH_ADAPTIVE_FEEDBACK_MISSING_ROUTE_LABEL",
                  "missing_route_label");
  }
  if (!request.plan.runtime_consumed) {
    return Refuse(request,
                  "ORH_ADAPTIVE_FEEDBACK_NO_RUNTIME",
                  "runtime_consumption_missing");
  }
  if (MissingHash(request.plan)) {
    return Refuse(request,
                  "ORH_ADAPTIVE_FEEDBACK_MISSING_RESULT_CONTRACT",
                  "result_contract_missing");
  }
  if (request.plan.result_hash != request.plan.fallback_result_hash) {
    return Refuse(request,
                  "ORH_ADAPTIVE_FEEDBACK_RESULT_MISMATCH",
                  "result_equivalence_mismatch");
  }
  if (!request.plan.exact_fallback_available &&
      request.misestimate_quarantine_requested) {
    return Refuse(request,
                  "ORH_ADAPTIVE_FEEDBACK_EXACT_FALLBACK_UNAVAILABLE",
                  "misestimate_quarantine_requires_exact_fallback");
  }
  if (request.plan.benchmark_or_reference_dominance_claim) {
    return Refuse(request,
                  "ORH_ADAPTIVE_FEEDBACK_DOMINANCE_OVERCLAIM",
                  "adaptive_feedback_is_not_reference_dominance");
  }
  if (request.authority.parser_client_or_reference_feedback_authority ||
      request.feedback.parser_or_reference_authority ||
      request.authority.feedback_visibility_or_finality_authority ||
      request.authority.feedback_recovery_authority ||
      !request.feedback.advisory_only) {
    return Refuse(request,
                  "ORH_ADAPTIVE_FEEDBACK_UNSAFE_AUTHORITY",
                  "feedback_must_be_optimizer_advisory_only");
  }
  if (!request.authority.engine_mga_snapshot_bound ||
      !request.authority.transaction_inventory_authoritative ||
      request.feedback.transaction_finality_authority !=
          "engine_transaction_inventory") {
    return Refuse(request,
                  "ORH_ADAPTIVE_FEEDBACK_MGA_UNPROVEN",
                  "engine_mga_transaction_inventory_required");
  }
  if (!request.authority.security_recheck_required ||
      !request.authority.exact_recheck_required ||
      !request.feedback.mga_visibility_recheck_preserved) {
    return Refuse(request,
                  "ORH_ADAPTIVE_FEEDBACK_SECURITY_UNPROVEN",
                  "security_and_exact_recheck_required");
  }
  if (request.epochs.feedback_generation !=
          request.epochs.expected_feedback_generation ||
      request.epochs.catalog_epoch != request.epochs.expected_catalog_epoch ||
      request.epochs.security_epoch != request.epochs.expected_security_epoch) {
    return Refuse(request,
                  "ORH_ADAPTIVE_FEEDBACK_EPOCH_MISMATCH",
                  "feedback_catalog_or_security_epoch_mismatch");
  }
  if (request.extended_stat_request_requested &&
      !request.extended_stat_source_authoritative) {
    return Refuse(request,
                  "ORH_ADAPTIVE_FEEDBACK_EXTENDED_STAT_SOURCE_UNSAFE",
                  "extended_stat_request_requires_optimizer_authority");
  }

  AdaptiveCardinalityFeedbackResult result;
  result.feedback_status = EvaluateOptimizerRuntimeFeedback(request.feedback);
  result.adjusted_cost = request.baseline_cost;
  AddEvidence(&result, "adaptive_feedback.route_label=" + request.plan.route_label);
  AddEvidence(&result,
              "adaptive_feedback.feedback_generation=" +
                  std::to_string(request.epochs.feedback_generation));
  AddEvidence(&result,
              "adaptive_feedback.feedback_epoch=" +
                  std::to_string(request.epochs.feedback_epoch));
  AddEvidence(&result,
              "adaptive_feedback.catalog_epoch=" +
                  std::to_string(request.epochs.catalog_epoch));
  AddEvidence(&result,
              "adaptive_feedback.security_epoch=" +
                  std::to_string(request.epochs.security_epoch));
  AddEvidence(&result,
              "adaptive_feedback.plan_hash=" + request.plan.variant_plan_hash);
  AddEvidence(&result,
              "adaptive_feedback.fallback_plan_hash=" +
                  request.plan.fallback_plan_hash);
  AddEvidence(&result,
              "adaptive_feedback.result_hash=" + request.plan.result_hash);
  AddEvidence(&result, "adaptive_feedback.runtime_consumed=true");
  AddEvidence(&result, "adaptive_feedback.result_equivalence=true");
  AddEvidence(&result, "adaptive_feedback.exact_fallback_available=true");
  AddEvidence(&result, "adaptive_feedback.parser_authority=false");
  AddEvidence(&result, "adaptive_feedback.reference_authority=false");
  AddEvidence(&result, "adaptive_feedback.client_authority=false");
  AddEvidence(&result, "adaptive_feedback.visibility_authority=false");
  AddEvidence(&result, "adaptive_feedback.finality_authority=false");
  AddEvidence(&result, "adaptive_feedback.recovery_authority=false");
  AddEvidence(&result,
              "adaptive_feedback.mga_finality_authority=engine_transaction_inventory");
  AddEvidence(&result,
              "adaptive_feedback.security_recheck_required=true");
  AddEvidence(&result, "adaptive_feedback.exact_recheck_required=true");

  for (const auto& evidence : result.feedback_status.evidence) {
    AddEvidence(&result, "optimizer_feedback." + evidence);
  }

  if (!result.feedback_status.ok || !result.feedback_status.applied) {
    result.ok = false;
    result.benchmark_clean = false;
    result.fallback_used = request.plan.exact_fallback_available;
    result.fail_closed = !request.plan.exact_fallback_available;
    result.diagnostic_code =
        result.feedback_status.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.STALE"
            ? "ORH_ADAPTIVE_FEEDBACK_STALE"
            : result.feedback_status.diagnostic_code;
    result.fallback_reason = "runtime_feedback_rejected";
    AddEvidence(&result, "adaptive_feedback.exact_fallback_used=true");
    AddEvidence(&result, "adaptive_feedback.benchmark_clean=false");
    return result;
  }

  const bool high_misestimate =
      result.feedback_status.diagnostic_code ==
      "SB_OPTIMIZER_FEEDBACK.HIGH_MISESTIMATE";
  result.bind_sensitive_variant_created =
      request.bind_sensitive_variant_requested && high_misestimate;
  result.misestimate_quarantined =
      request.misestimate_quarantine_requested && high_misestimate;
  result.extended_stat_requested =
      request.extended_stat_request_requested && high_misestimate;
  if (!result.bind_sensitive_variant_created ||
      !result.misestimate_quarantined ||
      !result.extended_stat_requested) {
    result.ok = false;
    result.benchmark_clean = false;
    result.fallback_used = true;
    result.diagnostic_code =
        "ORH_ADAPTIVE_FEEDBACK_NO_ACTIONABLE_MISESTIMATE";
    result.fallback_reason = "feedback_did_not_drive_required_actions";
    AddEvidence(&result, "adaptive_feedback.exact_fallback_used=true");
    AddEvidence(&result, "adaptive_feedback.benchmark_clean=false");
    return result;
  }

  result.adjusted_cost =
      ApplyOptimizerRuntimeFeedbackCost(request.baseline_cost, request.feedback);
  result.ok = true;
  result.benchmark_clean = true;
  result.fallback_used = false;
  result.fail_closed = false;
  result.diagnostic_code = "ORH_ADAPTIVE_CARDINALITY_PLAN_FEEDBACK.OK";
  AddEvidence(&result, "adaptive_feedback.bind_sensitive_variant=true");
  AddEvidence(&result, "adaptive_feedback.misestimate_quarantine=true");
  AddEvidence(&result, "adaptive_feedback.extended_stat_request=true");
  AddEvidence(&result, "adaptive_feedback.cost_profile_applied=true");
  AddEvidence(&result, "adaptive_feedback.exact_fallback_used=false");
  AddEvidence(&result, "adaptive_feedback.benchmark_clean=true");
  return result;
}

}  // namespace scratchbird::engine::optimizer
