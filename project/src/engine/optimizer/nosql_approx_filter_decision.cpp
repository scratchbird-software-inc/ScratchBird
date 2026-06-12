// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql_approx_filter_decision.hpp"

#include "index_filter_access.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

namespace api = scratchbird::engine::internal_api;
namespace idx = scratchbird::core::index;

using Family = api::EngineNoSqlProviderFamily;

void AddEvidence(NoSqlApproxFilterDecisionResult* result,
                 std::string evidence) {
  if (result != nullptr) {
    result->evidence.push_back(std::move(evidence));
  }
}

void AddEvidence(NoSqlApproxFilterCandidateDecision* decision,
                 std::string evidence) {
  if (decision != nullptr) {
    decision->evidence.push_back(std::move(evidence));
  }
}

NoSqlApproxFilterDecisionResult Refuse(std::string code,
                                       std::string evidence) {
  NoSqlApproxFilterDecisionResult result;
  result.ok = false;
  result.fail_closed = true;
  result.diagnostic_code = std::move(code);
  AddEvidence(&result, std::move(evidence));
  AddEvidence(&result, "approx_filter_metadata_only=true");
  AddEvidence(&result, "selected_filters_require_exact_fallback=true");
  AddEvidence(&result, "mga_visibility_authority=engine_recheck_required");
  AddEvidence(&result, "mga_finality_authority=engine_transaction_inventory");
  AddEvidence(&result, "security_recheck=required");
  AddEvidence(&result, "parser_or_reference_authority=false");
  AddEvidence(&result, "provider_transaction_finality_authority=false");
  AddEvidence(&result, "provider_visibility_authority=false");
  AddEvidence(&result, "client_visibility_or_finality_authority=false");
  AddEvidence(&result, "write_ahead_log_finality_authority=false");
  return result;
}

bool IsCoveredFamily(Family family) {
  return family == Family::kKeyValue ||
         family == Family::kDocument ||
         family == Family::kSearch ||
         family == Family::kVector ||
         family == Family::kGraph ||
         family == Family::kTimeSeries;
}

std::uint64_t SafeSub(std::uint64_t lhs, std::uint64_t rhs) {
  return lhs > rhs ? lhs - rhs : 0;
}

bool HasKind(const std::set<NoSqlApproxFilterKind>& kinds,
             NoSqlApproxFilterKind kind) {
  return kinds.find(kind) != kinds.end();
}

bool CandidateHasFreshBenchmark(const NoSqlApproxFilterBenchmarkInput& input) {
  return input.benchmark_epoch != 0 &&
         input.required_benchmark_epoch != 0 &&
         input.benchmark_epoch >= input.required_benchmark_epoch;
}

bool FilterKindUsesRangeBounds(NoSqlApproxFilterKind kind) {
  return kind == NoSqlApproxFilterKind::kMinMaxSet ||
         kind == NoSqlApproxFilterKind::kRangeFilter;
}

idx::RangePredicateProbe RangeProbe(
    const NoSqlApproxFilterBenchmarkInput& input,
    bool exact_recheck_available) {
  idx::RangePredicateProbe probe;
  probe.encoded_low = input.predicate_low.empty() ? input.encoded_min
                                                  : input.predicate_low;
  probe.encoded_high = input.predicate_high.empty() ? input.encoded_max
                                                    : input.predicate_high;
  probe.exact_recheck_available = exact_recheck_available;
  return probe;
}

idx::RangeSummaryBounds RangeSummary(
    const NoSqlApproxFilterBenchmarkInput& input) {
  idx::RangeSummaryBounds summary;
  summary.encoded_min = input.encoded_min;
  summary.encoded_max = input.encoded_max;
  summary.min_max_valid = input.minmax_bounds_valid &&
                          !input.encoded_min.empty() &&
                          !input.encoded_max.empty();
  summary.stale = !input.range_summary_valid;
  summary.rows_summarized = input.input_rows;
  return summary;
}

std::string CoreDecisionEvidence(const NoSqlApproxFilterBenchmarkInput& input,
                                 bool exact_recheck_available,
                                 bool* accepted) {
  *accepted = false;
  switch (input.kind) {
    case NoSqlApproxFilterKind::kBloom: {
      idx::BloomFilterProbe probe;
      probe.filter_may_contain = true;
      probe.exact_recheck_available = exact_recheck_available;
      probe.false_positive_disclosed = input.false_positive_disclosed;
      probe.policy_accepts_lossy_filter = true;
      const auto decision = idx::DecideBloomFilterProbe(probe);
      *accepted = decision.decision ==
                  idx::FilterRecheckDecision::candidate_requires_exact_recheck;
      return std::string("core_filter_decision=") + decision.reason_code;
    }
    case NoSqlApproxFilterKind::kMinMaxSet: {
      idx::ClickHouseSkipIndexRequest skip;
      skip.variant = idx::ClickHouseSkipVariant::set;
      skip.exact_recheck_available = exact_recheck_available;
      const auto skip_plan = idx::ResolveClickHouseSkipIndex(skip);
      const auto range = idx::DecideRangeSummaryPrune(
          RangeSummary(input), RangeProbe(input, exact_recheck_available));
      *accepted = skip_plan.accepted &&
                  range.decision != idx::RangePruneDecision::policy_blocked;
      return std::string("core_filter_decision=") + skip_plan.reason_code +
             ";range_decision=" + range.reason_code;
    }
    case NoSqlApproxFilterKind::kRangeFilter: {
      const auto range = idx::DecideRangeSummaryPrune(
          RangeSummary(input), RangeProbe(input, exact_recheck_available));
      *accepted = range.decision != idx::RangePruneDecision::policy_blocked;
      return std::string("core_filter_decision=") + range.reason_code;
    }
    case NoSqlApproxFilterKind::kUnknown:
      break;
  }
  return "core_filter_decision=unsupported";
}

NoSqlApproxFilterCandidateDecision EvaluateCandidate(
    const NoSqlApproxFilterDecisionRequest& request,
    const NoSqlApproxFilterBenchmarkInput& input) {
  NoSqlApproxFilterCandidateDecision decision;
  decision.family = input.family;
  decision.kind = input.kind;
  decision.candidate_id = input.candidate_id;
  decision.observed_false_positive_ppm = input.observed_false_positive_ppm;
  decision.exact_fallback_required = true;
  decision.row_mga_recheck_required = request.row_mga_recheck_required;
  decision.row_security_recheck_required = request.row_security_recheck_required;
  decision.returns_final_rows = false;
  decision.diagnostic_code = "SB_NOSQL_APPROX_FILTER.NOT_SELECTED";

  AddEvidence(&decision,
              std::string("family=") +
                  api::EngineNoSqlProviderFamilyName(input.family));
  AddEvidence(&decision, std::string("filter_kind=") +
                             NoSqlApproxFilterKindName(input.kind));
  AddEvidence(&decision, "candidate_only=true");
  AddEvidence(&decision, "approx_filter_metadata_only=true");

  const auto filtered_cost =
      input.filter_cost_units + input.exact_fallback_cost_units;
  decision.net_benefit_units = SafeSub(input.baseline_cost_units,
                                       filtered_cost);
  AddEvidence(&decision,
              "baseline_cost_units=" +
                  std::to_string(input.baseline_cost_units));
  AddEvidence(&decision,
              "filtered_cost_units=" + std::to_string(filtered_cost));
  AddEvidence(&decision,
              "net_benefit_units=" +
                  std::to_string(decision.net_benefit_units));
  AddEvidence(&decision,
              "observed_false_positive_ppm=" +
                  std::to_string(input.observed_false_positive_ppm));

  auto refuse = [&decision](std::string code, std::string evidence) {
    decision.selected = false;
    decision.safe = false;
    decision.refused = true;
    decision.diagnostic_code = std::move(code);
    AddEvidence(&decision, std::move(evidence));
    return decision;
  };

  if (!IsCoveredFamily(input.family)) {
    return refuse("SB_NOSQL_APPROX_FILTER.UNSUPPORTED_FAMILY",
                  "unsupported_family");
  }
  if (input.kind == NoSqlApproxFilterKind::kUnknown) {
    return refuse("SB_NOSQL_APPROX_FILTER.UNSUPPORTED_FILTER_KIND",
                  "unsupported_filter_kind");
  }
  if (input.candidate_id.empty()) {
    return refuse("SB_NOSQL_APPROX_FILTER.CANDIDATE_ID_REQUIRED",
                  "candidate_id_required");
  }
  if (!input.benchmark_authoritative ||
      !input.physical_provider_backed ||
      input.descriptor_scan_selected ||
      input.behavior_store_scan_selected) {
    return refuse("SB_NOSQL_APPROX_FILTER.BENCHMARK_NOT_AUTHORITATIVE",
                  "benchmark_not_authoritative_or_not_physical");
  }
  if (!CandidateHasFreshBenchmark(input)) {
    return refuse("SB_NOSQL_APPROX_FILTER.BENCHMARK_STALE",
                  "benchmark_stale_or_missing_epoch");
  }
  if (!request.exact_fallback_available || !input.exact_fallback_available) {
    return refuse("SB_NOSQL_APPROX_FILTER.EXACT_FALLBACK_REQUIRED",
                  "exact_fallback_required");
  }
  if (!request.row_mga_recheck_required ||
      !request.row_security_recheck_required ||
      !request.engine_mga_authoritative) {
    return refuse("SB_NOSQL_APPROX_FILTER.MGA_SECURITY_RECHECK_REQUIRED",
                  "missing_engine_mga_or_security_recheck");
  }
  if (!input.candidate_only ||
      input.candidate_claims_visibility_or_finality_authority) {
    return refuse("SB_NOSQL_APPROX_FILTER.FINAL_RESULT_AUTHORITY_REFUSED",
                  "candidate_claimed_visibility_or_finality_authority");
  }
  if (input.observed_false_negative_ppm != 0) {
    return refuse("SB_NOSQL_APPROX_FILTER.FALSE_NEGATIVE_REFUSED",
                  "observed_false_negative_refused");
  }
  if (input.observed_authorization_leak_count != 0) {
    return refuse("SB_NOSQL_APPROX_FILTER.AUTHORIZATION_LEAK_REFUSED",
                  "observed_authorization_leak_refused");
  }
  if (FilterKindUsesRangeBounds(input.kind) && !input.range_summary_valid) {
    return refuse("SB_NOSQL_APPROX_FILTER.RANGE_SUMMARY_STALE",
                  "range_summary_stale_resummarize_before_selection");
  }
  if (FilterKindUsesRangeBounds(input.kind) &&
      (!input.minmax_bounds_valid ||
       input.encoded_min.empty() ||
       input.encoded_max.empty())) {
    return refuse("SB_NOSQL_APPROX_FILTER.RANGE_BOUNDS_INVALID",
                  "range_bounds_invalid_or_missing");
  }
  if (input.candidate_rows > input.input_rows ||
      input.pruned_rows > input.input_rows) {
    return refuse("SB_NOSQL_APPROX_FILTER.BENCHMARK_SHAPE_INVALID",
                  "candidate_or_pruned_rows_exceed_input_rows");
  }

  bool core_accepted = false;
  AddEvidence(&decision,
              CoreDecisionEvidence(input, request.exact_fallback_available,
                                   &core_accepted));
  if (!core_accepted) {
    return refuse("SB_NOSQL_APPROX_FILTER.CORE_FILTER_REFUSED",
                  "core_filter_refused");
  }

  decision.safe = true;
  AddEvidence(&decision, "exact_fallback_required=true");
  AddEvidence(&decision, "row_mga_recheck_required=true");
  AddEvidence(&decision, "row_security_recheck_required=true");
  AddEvidence(&decision, "returns_final_rows=false");
  AddEvidence(&decision, "mga_finality_authority=engine_transaction_inventory");

  if (decision.net_benefit_units < request.min_net_benefit_units) {
    decision.diagnostic_code =
        "SB_NOSQL_APPROX_FILTER.INSUFFICIENT_BENEFIT";
    AddEvidence(&decision, "benefit_threshold_met=false");
    return decision;
  }

  decision.selected = true;
  decision.diagnostic_code = "SB_NOSQL_APPROX_FILTER.SELECTED_WITH_RECHECK";
  AddEvidence(&decision, "benefit_threshold_met=true");
  AddEvidence(&decision, "selected_filter_requires_exact_fallback=true");
  return decision;
}

void AddCommonAuthorityEvidence(NoSqlApproxFilterDecisionResult* result) {
  AddEvidence(result, "approx_filter_metadata_only=true");
  AddEvidence(result, "benchmark_decision_surface=family_filter_candidates");
  AddEvidence(result, "selected_filters_require_exact_fallback=true");
  AddEvidence(result, "mga_visibility_authority=engine_recheck_required");
  AddEvidence(result, "mga_finality_authority=engine_transaction_inventory");
  AddEvidence(result, "row_mga_recheck_evidence=required");
  AddEvidence(result, "row_security_recheck_evidence=required");
  AddEvidence(result, "security_context=present");
  AddEvidence(result, "security_snapshot_bound=true");
  AddEvidence(result, "grants_proven=true");
  AddEvidence(result, "parser_or_reference_authority=false");
  AddEvidence(result, "provider_transaction_finality_authority=false");
  AddEvidence(result, "provider_visibility_authority=false");
  AddEvidence(result, "client_visibility_or_finality_authority=false");
  AddEvidence(result, "write_ahead_log_finality_authority=false");
}

NoSqlApproxFilterDecisionResult FailClosedWithEvidence(
    NoSqlApproxFilterDecisionResult result,
    std::string code,
    std::string evidence) {
  result.ok = false;
  result.fail_closed = true;
  result.diagnostic_code = std::move(code);
  AddEvidence(&result, std::move(evidence));
  return result;
}

}  // namespace

const char* NoSqlApproxFilterKindName(NoSqlApproxFilterKind kind) {
  switch (kind) {
    case NoSqlApproxFilterKind::kMinMaxSet:
      return "minmax_set";
    case NoSqlApproxFilterKind::kBloom:
      return "bloom";
    case NoSqlApproxFilterKind::kRangeFilter:
      return "range_filter";
    case NoSqlApproxFilterKind::kUnknown:
      break;
  }
  return "unknown";
}

std::vector<api::EngineNoSqlProviderFamily>
NoSqlApproxFilterCoveredFamilies() {
  return {Family::kKeyValue,
          Family::kDocument,
          Family::kSearch,
          Family::kVector,
          Family::kGraph,
          Family::kTimeSeries};
}

std::vector<NoSqlApproxFilterKind> NoSqlApproxFilterCandidateKinds() {
  return {NoSqlApproxFilterKind::kMinMaxSet,
          NoSqlApproxFilterKind::kBloom,
          NoSqlApproxFilterKind::kRangeFilter};
}

NoSqlApproxFilterDecisionResult EvaluateNoSqlApproxFilterDecision(
    const NoSqlApproxFilterDecisionRequest& request) {
  if (request.object_uuid.empty()) {
    return Refuse("SB_NOSQL_APPROX_FILTER.OBJECT_REQUIRED",
                  "object_required");
  }
  if (!request.security_context_present) {
    return Refuse("SB_NOSQL_APPROX_FILTER.SECURITY_CONTEXT_REQUIRED",
                  "security_context_required");
  }
  if (request.cluster_scope_requested && !request.cluster_authority_present) {
    return Refuse("SB_NOSQL_APPROX_FILTER.CLUSTER_AUTHORITY_REQUIRED",
                  "cluster_authority_required");
  }
  if (!request.security_snapshot_bound || !request.grants_proven) {
    return Refuse("SB_NOSQL_APPROX_FILTER.SECURITY_PROOF_REQUIRED",
                  "security_snapshot_or_grants_proof_required");
  }
  if (request.parser_or_reference_authority ||
      request.provider_claims_transaction_finality_authority ||
      request.provider_claims_visibility_authority ||
      request.client_claims_visibility_or_finality_authority ||
      request.write_ahead_log_claims_finality_authority) {
    return Refuse("SB_NOSQL_APPROX_FILTER.UNSAFE_AUTHORITY",
                  "unsafe_authority_claim_refused");
  }
  if (!request.engine_mga_authoritative ||
      !request.exact_fallback_available ||
      !request.row_mga_recheck_required ||
      !request.row_security_recheck_required) {
    return Refuse("SB_NOSQL_APPROX_FILTER.RECHECK_AUTHORITY_REQUIRED",
                  "engine_mga_exact_fallback_and_security_recheck_required");
  }
  if (request.candidates.empty()) {
    return Refuse("SB_NOSQL_APPROX_FILTER.CANDIDATES_REQUIRED",
                  "candidates_required");
  }

  NoSqlApproxFilterDecisionResult result;
  result.ok = true;
  result.fail_closed = false;
  result.diagnostic_code = "SB_NOSQL_APPROX_FILTER.SELECTED";
  AddCommonAuthorityEvidence(&result);

  std::map<Family, std::set<NoSqlApproxFilterKind>> family_kinds;
  std::map<Family, bool> selected_by_family;
  for (const auto family : NoSqlApproxFilterCoveredFamilies()) {
    selected_by_family[family] = false;
  }

  for (const auto& candidate : request.candidates) {
    auto decision = EvaluateCandidate(request, candidate);
    family_kinds[candidate.family].insert(candidate.kind);
    if (decision.selected) {
      selected_by_family[candidate.family] = true;
      NoSqlApproxSelectedFilter selected;
      selected.family = decision.family;
      selected.kind = decision.kind;
      selected.candidate_id = decision.candidate_id;
      selected.net_benefit_units = decision.net_benefit_units;
      selected.exact_fallback_required = true;
      selected.row_mga_recheck_required = true;
      selected.row_security_recheck_required = true;
      selected.candidate_only = true;
      result.selected_filters.push_back(std::move(selected));
    }
    result.candidate_decisions.push_back(std::move(decision));
  }

  for (const auto family : NoSqlApproxFilterCoveredFamilies()) {
    const auto found = family_kinds.find(family);
    for (const auto kind : NoSqlApproxFilterCandidateKinds()) {
      if (found == family_kinds.end() || !HasKind(found->second, kind)) {
        return FailClosedWithEvidence(
            std::move(result),
            "SB_NOSQL_APPROX_FILTER.FAMILY_CANDIDATES_INCOMPLETE",
            std::string("missing_family_filter=") +
                api::EngineNoSqlProviderFamilyName(family) + "." +
                NoSqlApproxFilterKindName(kind));
      }
    }
    AddEvidence(&result,
                std::string("family_candidates.") +
                    api::EngineNoSqlProviderFamilyName(family) + "=3");
    if (!selected_by_family[family]) {
      return FailClosedWithEvidence(
          std::move(result),
          "SB_NOSQL_APPROX_FILTER.NO_SAFE_FILTER_FOR_FAMILY",
          std::string("no_safe_filter_for_family=") +
              api::EngineNoSqlProviderFamilyName(family));
    }
  }

  AddEvidence(&result,
              "candidate_benchmark_rows=" +
                  std::to_string(result.candidate_decisions.size()));
  AddEvidence(&result,
              "selected_filter_count=" +
                  std::to_string(result.selected_filters.size()));
  AddEvidence(&result, "all_selected_filters_candidate_only=true");
  AddEvidence(&result, "all_selected_filters_return_final_rows=false");
  return result;
}

std::string SerializeNoSqlApproxFilterDecisionEvidence(
    const NoSqlApproxFilterDecisionResult& result) {
  std::ostringstream out;
  out << "diagnostic_code=" << result.diagnostic_code
      << "|ok=" << (result.ok ? "true" : "false")
      << "|fail_closed=" << (result.fail_closed ? "true" : "false")
      << "|candidate_decisions=" << result.candidate_decisions.size()
      << "|selected_filters=" << result.selected_filters.size();
  for (const auto& evidence : result.evidence) {
    out << '|' << evidence;
  }
  for (const auto& decision : result.candidate_decisions) {
    out << "|candidate=" << decision.candidate_id
        << ",family=" << api::EngineNoSqlProviderFamilyName(decision.family)
        << ",kind=" << NoSqlApproxFilterKindName(decision.kind)
        << ",diagnostic=" << decision.diagnostic_code
        << ",selected=" << (decision.selected ? "true" : "false");
  }
  return out.str();
}

}  // namespace scratchbird::engine::optimizer
