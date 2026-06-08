// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "runtime_filter_pushdown.hpp"

#include <sstream>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

std::uint64_t SafeSub(std::uint64_t lhs, std::uint64_t rhs) {
  return lhs > rhs ? lhs - rhs : 0;
}

RuntimeFilterPushdownDecision Refuse(std::string code, std::string evidence) {
  RuntimeFilterPushdownDecision result;
  result.ok = false;
  result.fail_closed = true;
  result.diagnostic_code = std::move(code);
  Add(&result.evidence, std::move(evidence));
  Add(&result.evidence, "runtime_filter.candidate_rows_only=true");
  Add(&result.evidence, "runtime_filter.exact_recheck_required=true");
  Add(&result.evidence, "runtime_filter.mga_visibility_recheck_required=true");
  Add(&result.evidence, "runtime_filter.security_recheck_required=true");
  Add(&result.evidence, "parser_or_donor_finality_or_visibility_authority=false");
  Add(&result.evidence, "client_finality_or_visibility_authority=false");
  Add(&result.evidence, "provider_finality_or_visibility_authority=false");
  Add(&result.evidence,
      "write_ahead_log_finality_or_visibility_authority=false");
  return result;
}

RuntimeFilterCandidateDecision RefuseCandidate(
    RuntimeFilterCandidateDecision decision,
    std::string code,
    std::string evidence) {
  decision.selected = false;
  decision.fallback_selected = false;
  decision.refused = true;
  decision.diagnostic_code = std::move(code);
  Add(&decision.evidence, std::move(evidence));
  return decision;
}

RuntimeFilterCandidateDecision EvaluateCandidate(
    const RuntimeFilterPushdownRequest& request,
    const RuntimeFilterDescriptor& descriptor) {
  RuntimeFilterCandidateDecision decision;
  decision.descriptor = descriptor;
  decision.diagnostic_code = "SB_RUNTIME_FILTER.NOT_SELECTED";

  const auto filtered_cost =
      descriptor.filter_cost_units + descriptor.exact_recheck_cost_units;
  decision.net_benefit_units =
      SafeSub(descriptor.baseline_cost_units, filtered_cost);

  Add(&decision.evidence,
      std::string("runtime_filter.family=") +
          RuntimeFilterFamilyName(descriptor.family));
  Add(&decision.evidence,
      std::string("runtime_filter.route=") +
          RuntimeFilterRouteName(descriptor.route));
  Add(&decision.evidence,
      "runtime_filter.filter_id=" + descriptor.filter_id);
  Add(&decision.evidence,
      "runtime_filter.input_rows=" + std::to_string(descriptor.input_rows));
  Add(&decision.evidence,
      "runtime_filter.candidate_rows=" +
          std::to_string(descriptor.estimated_candidate_rows));
  Add(&decision.evidence,
      "runtime_filter.pruned_rows=" +
          std::to_string(descriptor.estimated_pruned_rows));
  Add(&decision.evidence,
      "runtime_filter.net_benefit_units=" +
          std::to_string(decision.net_benefit_units));
  Add(&decision.evidence, "runtime_filter.candidate_rows_only=true");

  if (request.plan_id.empty() || descriptor.plan_node_id.empty() ||
      descriptor.filter_id.empty() || descriptor.predicate_digest.empty()) {
    return RefuseCandidate(
        std::move(decision), "SB_RUNTIME_FILTER.DESCRIPTOR_REQUIRED",
        "descriptor_plan_filter_or_predicate_digest_required");
  }
  if (!RuntimeFilterFamilySupported(descriptor.family)) {
    return RefuseCandidate(std::move(decision),
                           "SB_RUNTIME_FILTER.UNSUPPORTED_FAMILY",
                           "unsupported_family");
  }
  if (!RuntimeFilterRouteSupported(descriptor.route)) {
    return RefuseCandidate(std::move(decision),
                           "SB_RUNTIME_FILTER.UNSUPPORTED_ROUTE",
                           "unsupported_route");
  }
  if (!descriptor.plan_shape_supported) {
    return RefuseCandidate(std::move(decision),
                           "SB_RUNTIME_FILTER.PLAN_SHAPE_REFUSED",
                           "plan_shape_not_runtime_filter_safe");
  }
  if (!descriptor.candidate_set_available) {
    return RefuseCandidate(std::move(decision),
                           "SB_RUNTIME_FILTER.CANDIDATE_SET_REQUIRED",
                           "candidate_set_required");
  }
  if (!descriptor.security_context_present ||
      !descriptor.security_snapshot_bound ||
      !descriptor.grants_proven) {
    return RefuseCandidate(std::move(decision),
                           "SB_RUNTIME_FILTER.SECURITY_CONTEXT_REQUIRED",
                           "security_context_snapshot_and_grants_required");
  }
  if (!descriptor.engine_mga_authoritative ||
      !descriptor.mga_visibility_recheck_required) {
    return RefuseCandidate(std::move(decision),
                           "SB_RUNTIME_FILTER.MGA_RECHECK_REQUIRED",
                           "engine_mga_visibility_recheck_required");
  }
  if (!descriptor.exact_recheck_available ||
      !descriptor.security_authorization_recheck_required) {
    return RefuseCandidate(std::move(decision),
                           "SB_RUNTIME_FILTER.EXACT_RECHECK_REQUIRED",
                           "exact_and_security_recheck_required");
  }
  if (descriptor.parser_or_donor_finality_or_visibility_authority ||
      descriptor.client_finality_or_visibility_authority ||
      descriptor.provider_finality_or_visibility_authority ||
      descriptor.write_ahead_log_finality_or_visibility_authority) {
    return RefuseCandidate(std::move(decision),
                           "SB_RUNTIME_FILTER.UNSAFE_AUTHORITY",
                           "unsafe_runtime_filter_authority_claim");
  }
  if (descriptor.descriptor_scan_selected ||
      descriptor.behavior_store_scan_selected) {
    return RefuseCandidate(std::move(decision),
                           "SB_RUNTIME_FILTER.PHYSICAL_ROUTE_REQUIRED",
                           "descriptor_or_behavior_store_scan_refused");
  }
  if (descriptor.stale ||
      descriptor.descriptor_generation < descriptor.required_descriptor_generation) {
    return RefuseCandidate(std::move(decision),
                           "SB_RUNTIME_FILTER.STALE_DESCRIPTOR",
                           "stale_runtime_filter_descriptor");
  }
  if (descriptor.estimated_candidate_rows > descriptor.input_rows ||
      descriptor.estimated_pruned_rows > descriptor.input_rows) {
    return RefuseCandidate(std::move(decision),
                           "SB_RUNTIME_FILTER.COUNTERS_INVALID",
                           "candidate_or_pruned_rows_exceed_input_rows");
  }
  if (descriptor.lossy_or_false_negative_possible &&
      !descriptor.exact_fallback_available) {
    return RefuseCandidate(std::move(decision),
                           "SB_RUNTIME_FILTER.EXACT_FALLBACK_REQUIRED",
                           "lossy_or_false_negative_filter_requires_exact_fallback");
  }
  if (descriptor.route == RuntimeFilterRoute::kProvider &&
      !descriptor.provider_supports_runtime_filters) {
    if (!descriptor.exact_fallback_available) {
      return RefuseCandidate(std::move(decision),
                             "SB_RUNTIME_FILTER.PROVIDER_UNSUPPORTED",
                             "provider_runtime_filter_unsupported_no_fallback");
    }
    decision.fallback_selected = true;
    decision.diagnostic_code = "SB_RUNTIME_FILTER.EXACT_FALLBACK_SELECTED";
    Add(&decision.evidence, "runtime_filter.provider_unsupported=true");
    Add(&decision.evidence, "runtime_filter.exact_fallback_selected=true");
    return decision;
  }
  if (decision.net_benefit_units < request.min_net_benefit_units ||
      descriptor.estimated_pruned_rows == 0) {
    decision.diagnostic_code = "SB_RUNTIME_FILTER.INSUFFICIENT_BENEFIT";
    Add(&decision.evidence, "runtime_filter.benefit_threshold_met=false");
    return decision;
  }

  decision.selected = true;
  decision.diagnostic_code = "SB_RUNTIME_FILTER.SELECTED_WITH_RECHECK";
  Add(&decision.evidence, "runtime_filter.benefit_threshold_met=true");
  Add(&decision.evidence, "runtime_filter.exact_recheck_required=true");
  Add(&decision.evidence, "runtime_filter.mga_visibility_recheck_required=true");
  Add(&decision.evidence, "runtime_filter.security_recheck_required=true");
  Add(&decision.evidence, "runtime_filter.returns_final_rows=false");
  Add(&decision.evidence, "mga_finality_authority=engine_transaction_inventory");
  return decision;
}

void AddCommonEvidence(RuntimeFilterPushdownDecision* result) {
  Add(&result->evidence, "runtime_filter.contract=optimizer_pushdown_v1");
  Add(&result->evidence, "runtime_filter.candidate_rows_only=true");
  Add(&result->evidence, "runtime_filter.exact_recheck_required=true");
  Add(&result->evidence, "runtime_filter.mga_visibility_recheck_required=true");
  Add(&result->evidence, "runtime_filter.security_recheck_required=true");
  Add(&result->evidence, "parser_or_donor_finality_or_visibility_authority=false");
  Add(&result->evidence, "client_finality_or_visibility_authority=false");
  Add(&result->evidence, "provider_finality_or_visibility_authority=false");
  Add(&result->evidence,
      "write_ahead_log_finality_or_visibility_authority=false");
}

}  // namespace

const char* RuntimeFilterFamilyName(RuntimeFilterFamily family) {
  switch (family) {
    case RuntimeFilterFamily::kJoin:
      return "join";
    case RuntimeFilterFamily::kGraph:
      return "graph";
    case RuntimeFilterFamily::kSearch:
      return "search";
    case RuntimeFilterFamily::kVector:
      return "vector";
    case RuntimeFilterFamily::kTimeSeries:
      return "time_series";
    case RuntimeFilterFamily::kCandidateSet:
      return "candidate_set";
    case RuntimeFilterFamily::kUnknown:
      break;
  }
  return "unknown";
}

const char* RuntimeFilterRouteName(RuntimeFilterRoute route) {
  switch (route) {
    case RuntimeFilterRoute::kScan:
      return "scan";
    case RuntimeFilterRoute::kProvider:
      return "provider";
    case RuntimeFilterRoute::kUnknown:
      break;
  }
  return "unknown";
}

bool RuntimeFilterFamilySupported(RuntimeFilterFamily family) {
  return family == RuntimeFilterFamily::kJoin ||
         family == RuntimeFilterFamily::kGraph ||
         family == RuntimeFilterFamily::kSearch ||
         family == RuntimeFilterFamily::kVector ||
         family == RuntimeFilterFamily::kTimeSeries ||
         family == RuntimeFilterFamily::kCandidateSet;
}

bool RuntimeFilterRouteSupported(RuntimeFilterRoute route) {
  return route == RuntimeFilterRoute::kScan ||
         route == RuntimeFilterRoute::kProvider;
}

RuntimeFilterPushdownDecision EvaluateRuntimeFilterPushdown(
    const RuntimeFilterPushdownRequest& request) {
  if (request.plan_id.empty()) {
    return Refuse("SB_RUNTIME_FILTER.PLAN_REQUIRED", "plan_id_required");
  }
  if (request.candidates.empty()) {
    return Refuse("SB_RUNTIME_FILTER.CANDIDATES_REQUIRED",
                  "runtime_filter_candidates_required");
  }

  RuntimeFilterPushdownDecision result;
  result.ok = true;
  result.fail_closed = false;
  result.diagnostic_code = "SB_RUNTIME_FILTER.SELECTED";
  AddCommonEvidence(&result);

  std::uint64_t fallback_count = 0;
  for (const auto& candidate : request.candidates) {
    auto decision = EvaluateCandidate(request, candidate);
    if (decision.refused) {
      result.ok = false;
      result.fail_closed = true;
      result.diagnostic_code = decision.diagnostic_code;
      result.selected_filters.clear();
    }
    if (decision.selected) {
      result.selected_filters.push_back(decision.descriptor);
    }
    if (decision.fallback_selected) {
      ++fallback_count;
    }
    result.candidate_decisions.push_back(std::move(decision));
    if (result.fail_closed) {
      break;
    }
  }

  Add(&result.evidence,
      "runtime_filter.candidate_count=" +
          std::to_string(result.candidate_decisions.size()));
  Add(&result.evidence,
      "runtime_filter.pushed_filter_count=" +
          std::to_string(result.selected_filters.size()));
  Add(&result.evidence,
      "runtime_filter.fallback_count=" + std::to_string(fallback_count));

  if (!result.fail_closed && result.selected_filters.empty() &&
      fallback_count == 0) {
    result.ok = false;
    result.fail_closed = true;
    result.diagnostic_code = "SB_RUNTIME_FILTER.NO_SAFE_FILTER";
    Add(&result.evidence, "runtime_filter.no_safe_filter_selected");
  }
  if (!result.fail_closed && result.selected_filters.empty() &&
      fallback_count != 0) {
    result.diagnostic_code = "SB_RUNTIME_FILTER.EXACT_FALLBACK_ONLY";
  }
  return result;
}

std::string SerializeRuntimeFilterPushdownEvidence(
    const RuntimeFilterPushdownDecision& decision) {
  std::ostringstream out;
  out << "diagnostic_code=" << decision.diagnostic_code
      << "|ok=" << (decision.ok ? "true" : "false")
      << "|fail_closed=" << (decision.fail_closed ? "true" : "false")
      << "|selected_filters=" << decision.selected_filters.size();
  for (const auto& evidence : decision.evidence) {
    out << '|' << evidence;
  }
  for (const auto& candidate : decision.candidate_decisions) {
    out << "|candidate=" << candidate.descriptor.filter_id
        << ",family=" << RuntimeFilterFamilyName(candidate.descriptor.family)
        << ",route=" << RuntimeFilterRouteName(candidate.descriptor.route)
        << ",diagnostic=" << candidate.diagnostic_code
        << ",selected=" << (candidate.selected ? "true" : "false")
        << ",fallback=" << (candidate.fallback_selected ? "true" : "false");
  }
  return out.str();
}

}  // namespace scratchbird::engine::optimizer
