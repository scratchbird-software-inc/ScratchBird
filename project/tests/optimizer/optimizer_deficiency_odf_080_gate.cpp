// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql_approx_filter_decision.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
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

std::vector<api::EngineNoSqlProviderFamily> Families() {
  return {api::EngineNoSqlProviderFamily::kKeyValue,
          api::EngineNoSqlProviderFamily::kDocument,
          api::EngineNoSqlProviderFamily::kSearch,
          api::EngineNoSqlProviderFamily::kVector,
          api::EngineNoSqlProviderFamily::kGraph,
          api::EngineNoSqlProviderFamily::kTimeSeries};
}

std::vector<opt::NoSqlApproxFilterKind> Kinds() {
  return {opt::NoSqlApproxFilterKind::kMinMaxSet,
          opt::NoSqlApproxFilterKind::kBloom,
          opt::NoSqlApproxFilterKind::kRangeFilter};
}

opt::NoSqlApproxFilterKind SelectedKindForFamily(
    api::EngineNoSqlProviderFamily family) {
  switch (family) {
    case api::EngineNoSqlProviderFamily::kKeyValue:
    case api::EngineNoSqlProviderFamily::kTimeSeries:
      return opt::NoSqlApproxFilterKind::kMinMaxSet;
    case api::EngineNoSqlProviderFamily::kDocument:
    case api::EngineNoSqlProviderFamily::kVector:
      return opt::NoSqlApproxFilterKind::kBloom;
    case api::EngineNoSqlProviderFamily::kSearch:
    case api::EngineNoSqlProviderFamily::kGraph:
      return opt::NoSqlApproxFilterKind::kRangeFilter;
    case api::EngineNoSqlProviderFamily::kUnknown:
    case api::EngineNoSqlProviderFamily::kSpatial:
    case api::EngineNoSqlProviderFamily::kColumnar:
      break;
  }
  return opt::NoSqlApproxFilterKind::kUnknown;
}

std::string CandidateId(api::EngineNoSqlProviderFamily family,
                        opt::NoSqlApproxFilterKind kind) {
  return std::string(api::EngineNoSqlProviderFamilyName(family)) + "." +
         opt::NoSqlApproxFilterKindName(kind);
}

opt::NoSqlApproxFilterBenchmarkInput Candidate(
    api::EngineNoSqlProviderFamily family,
    opt::NoSqlApproxFilterKind kind) {
  opt::NoSqlApproxFilterBenchmarkInput candidate;
  candidate.family = family;
  candidate.kind = kind;
  candidate.candidate_id = CandidateId(family, kind);
  candidate.benchmark_epoch = 800;
  candidate.required_benchmark_epoch = 800;
  candidate.input_rows = 10000;
  candidate.candidate_rows = 2400;
  candidate.pruned_rows = 7600;
  candidate.baseline_cost_units = 100;
  candidate.filter_cost_units = 55;
  candidate.exact_fallback_cost_units = 50;
  candidate.observed_false_positive_ppm = 2000;
  candidate.encoded_min = "000100";
  candidate.encoded_max = "999900";
  candidate.predicate_low = "010000";
  candidate.predicate_high = "020000";
  candidate.benchmark_authoritative = true;
  candidate.physical_provider_backed = true;
  candidate.exact_fallback_available = true;
  candidate.candidate_only = true;
  candidate.false_positive_disclosed = true;
  candidate.range_summary_valid = true;
  candidate.minmax_bounds_valid = true;
  return candidate;
}

std::vector<opt::NoSqlApproxFilterBenchmarkInput> AllCandidates() {
  std::vector<opt::NoSqlApproxFilterBenchmarkInput> candidates;
  for (const auto family : Families()) {
    const auto selected_kind = SelectedKindForFamily(family);
    bool unsafe_added = false;
    for (const auto kind : Kinds()) {
      auto candidate = Candidate(family, kind);
      if (kind == selected_kind) {
        candidate.baseline_cost_units = 1200;
        candidate.filter_cost_units = 120;
        candidate.exact_fallback_cost_units = 280;
        candidate.candidate_rows = 1900;
        candidate.pruned_rows = 8100;
        candidate.observed_false_positive_ppm = 1500;
      } else if (!unsafe_added) {
        candidate.observed_false_negative_ppm = 1;
        unsafe_added = true;
      }
      candidates.push_back(std::move(candidate));
    }
  }
  return candidates;
}

opt::NoSqlApproxFilterDecisionRequest BaseRequest() {
  opt::NoSqlApproxFilterDecisionRequest request;
  request.object_uuid = "019df080-0000-7000-8000-000000000080";
  request.candidates = AllCandidates();
  request.min_net_benefit_units = 100;
  request.security_context_present = true;
  request.security_snapshot_bound = true;
  request.grants_proven = true;
  request.engine_mga_authoritative = true;
  request.exact_fallback_available = true;
  request.row_mga_recheck_required = true;
  request.row_security_recheck_required = true;
  return request;
}

bool EvidenceContains(const std::vector<std::string>& evidence,
                      std::string_view token) {
  for (const auto& item : evidence) {
    if (item.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool HasCandidateDecision(const opt::NoSqlApproxFilterDecisionResult& result,
                          api::EngineNoSqlProviderFamily family,
                          opt::NoSqlApproxFilterKind kind,
                          std::string_view diagnostic) {
  for (const auto& decision : result.candidate_decisions) {
    if (decision.family == family &&
        decision.kind == kind &&
        decision.diagnostic_code.find(diagnostic) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireEvidenceHygiene(
    const opt::NoSqlApproxFilterDecisionResult& result) {
  std::vector<std::string> values = result.evidence;
  values.push_back(result.diagnostic_code);
  for (const auto& decision : result.candidate_decisions) {
    values.push_back(decision.candidate_id);
    values.push_back(decision.diagnostic_code);
    values.insert(values.end(), decision.evidence.begin(),
                  decision.evidence.end());
  }
  for (const auto& value : values) {
    for (const auto forbidden : {"docs/", "execution-plans", "findings",
                                 "contracts", "references",
                                 "provider_transaction_finality_authority=true",
                                 "provider_visibility_authority=true",
                                 "parser_executes_sql=true",
                                 "client_visibility_or_finality_authority=true",
                                 "write_ahead_log_finality_authority=true"}) {
      Require(value.find(forbidden) == std::string::npos,
              "ODF-080 evidence leaked forbidden documentation or authority token");
    }
  }
}

void AllSixFamiliesBenchmarkAndSelectSafeFilters() {
  const auto result = opt::EvaluateNoSqlApproxFilterDecision(BaseRequest());
  Require(result.ok, "ODF-080 authoritative approximate filter plan failed");
  Require(!result.fail_closed, "ODF-080 successful plan remained fail closed");
  Require(result.candidate_decisions.size() == 18,
          "ODF-080 did not benchmark three candidates for all six families");
  Require(result.selected_filters.size() == 6,
          "ODF-080 did not select one safe filter per family");
  Require(EvidenceContains(result.evidence,
                           "selected_filters_require_exact_fallback=true"),
          "ODF-080 missing exact fallback evidence");
  Require(EvidenceContains(result.evidence,
                           "mga_finality_authority=engine_transaction_inventory"),
          "ODF-080 missing MGA finality authority evidence");
  Require(EvidenceContains(result.evidence,
                           "row_mga_recheck_evidence=required"),
          "ODF-080 missing row MGA recheck evidence");
  Require(EvidenceContains(result.evidence,
                           "row_security_recheck_evidence=required"),
          "ODF-080 missing row security recheck evidence");

  std::set<std::string> selected_families;
  std::set<std::string> selected_kinds;
  for (const auto& selected : result.selected_filters) {
    selected_families.insert(api::EngineNoSqlProviderFamilyName(selected.family));
    selected_kinds.insert(opt::NoSqlApproxFilterKindName(selected.kind));
    Require(selected.exact_fallback_required,
            "ODF-080 selected filter lacked exact fallback requirement");
    Require(selected.row_mga_recheck_required,
            "ODF-080 selected filter lacked MGA recheck requirement");
    Require(selected.row_security_recheck_required,
            "ODF-080 selected filter lacked security recheck requirement");
    Require(selected.candidate_only,
            "ODF-080 selected filter was allowed to return final rows");
  }
  Require(selected_families.size() == 6,
          "ODF-080 selected filters did not cover all six families");
  Require(selected_kinds.count("minmax_set") != 0,
          "ODF-080 did not select any min/max set filter");
  Require(selected_kinds.count("bloom") != 0,
          "ODF-080 did not select any Bloom filter");
  Require(selected_kinds.count("range_filter") != 0,
          "ODF-080 did not select any range filter");

  for (const auto family : Families()) {
    const auto family_name =
        std::string(api::EngineNoSqlProviderFamilyName(family));
    Require(EvidenceContains(result.evidence,
                             "family_candidates." + family_name + "=3"),
            "ODF-080 missing per-family benchmark evidence");
    for (const auto kind : Kinds()) {
      Require(HasCandidateDecision(result, family, kind, "SB_NOSQL_APPROX_FILTER"),
              "ODF-080 missing candidate decision row");
    }
  }

  const auto refused = std::count_if(
      result.candidate_decisions.begin(),
      result.candidate_decisions.end(),
      [](const opt::NoSqlApproxFilterCandidateDecision& decision) {
        return decision.refused &&
               decision.diagnostic_code ==
                   "SB_NOSQL_APPROX_FILTER.FALSE_NEGATIVE_REFUSED";
      });
  Require(refused == 6,
          "ODF-080 did not explicitly refuse unsafe false-negative candidates");
  RequireEvidenceHygiene(result);
}

void UnsafeAuthorityAndMissingRecheckFailClosed() {
  auto request = BaseRequest();
  request.parser_or_reference_authority = true;
  auto result = opt::EvaluateNoSqlApproxFilterDecision(request);
  Require(!result.ok, "ODF-080 parser/reference authority was accepted");
  Require(result.fail_closed, "ODF-080 parser/reference refusal did not fail closed");
  Require(result.diagnostic_code == "SB_NOSQL_APPROX_FILTER.UNSAFE_AUTHORITY",
          "ODF-080 parser/reference authority diagnostic changed");

  request = BaseRequest();
  request.provider_claims_transaction_finality_authority = true;
  result = opt::EvaluateNoSqlApproxFilterDecision(request);
  Require(!result.ok, "ODF-080 provider finality authority was accepted");
  Require(result.diagnostic_code == "SB_NOSQL_APPROX_FILTER.UNSAFE_AUTHORITY",
          "ODF-080 provider authority diagnostic changed");

  request = BaseRequest();
  request.exact_fallback_available = false;
  result = opt::EvaluateNoSqlApproxFilterDecision(request);
  Require(!result.ok, "ODF-080 missing exact fallback was accepted");
  Require(result.diagnostic_code ==
              "SB_NOSQL_APPROX_FILTER.RECHECK_AUTHORITY_REQUIRED",
          "ODF-080 exact fallback diagnostic changed");

  request = BaseRequest();
  request.engine_mga_authoritative = false;
  result = opt::EvaluateNoSqlApproxFilterDecision(request);
  Require(!result.ok, "ODF-080 missing MGA authority was accepted");
  Require(result.diagnostic_code ==
              "SB_NOSQL_APPROX_FILTER.RECHECK_AUTHORITY_REQUIRED",
          "ODF-080 MGA authority diagnostic changed");
}

void SecurityAndClusterGuardsFailClosed() {
  auto request = BaseRequest();
  request.security_context_present = false;
  auto result = opt::EvaluateNoSqlApproxFilterDecision(request);
  Require(!result.ok, "ODF-080 missing security context was accepted");
  Require(result.diagnostic_code ==
              "SB_NOSQL_APPROX_FILTER.SECURITY_CONTEXT_REQUIRED",
          "ODF-080 security context diagnostic changed");

  request = BaseRequest();
  request.security_snapshot_bound = false;
  result = opt::EvaluateNoSqlApproxFilterDecision(request);
  Require(!result.ok, "ODF-080 missing security proof was accepted");
  Require(result.diagnostic_code ==
              "SB_NOSQL_APPROX_FILTER.SECURITY_PROOF_REQUIRED",
          "ODF-080 security proof diagnostic changed");

  request = BaseRequest();
  request.cluster_scope_requested = true;
  result = opt::EvaluateNoSqlApproxFilterDecision(request);
  Require(!result.ok, "ODF-080 missing cluster authority was accepted");
  Require(result.diagnostic_code ==
              "SB_NOSQL_APPROX_FILTER.CLUSTER_AUTHORITY_REQUIRED",
          "ODF-080 cluster authority diagnostic changed");
}

void FamilyCompletenessAndBenchmarkAuthorityFailClosed() {
  auto request = BaseRequest();
  request.candidates.pop_back();
  auto result = opt::EvaluateNoSqlApproxFilterDecision(request);
  Require(!result.ok, "ODF-080 incomplete family candidate matrix was accepted");
  Require(result.diagnostic_code ==
              "SB_NOSQL_APPROX_FILTER.FAMILY_CANDIDATES_INCOMPLETE",
          "ODF-080 incomplete candidate diagnostic changed");

  request = BaseRequest();
  request.candidates.at(2).benchmark_authoritative = false;
  result = opt::EvaluateNoSqlApproxFilterDecision(request);
  Require(result.ok,
          "ODF-080 one refused candidate should not block another safe family filter");
  Require(HasCandidateDecision(
              result, api::EngineNoSqlProviderFamily::kKeyValue,
              opt::NoSqlApproxFilterKind::kRangeFilter,
              "SB_NOSQL_APPROX_FILTER.BENCHMARK_NOT_AUTHORITATIVE"),
          "ODF-080 non-authoritative benchmark was not refused");

  request = BaseRequest();
  request.candidates.at(0).minmax_bounds_valid = false;
  result = opt::EvaluateNoSqlApproxFilterDecision(request);
  Require(!result.ok,
          "ODF-080 selected min/max filter with invalid bounds was accepted");
  Require(result.diagnostic_code ==
              "SB_NOSQL_APPROX_FILTER.NO_SAFE_FILTER_FOR_FAMILY",
          "ODF-080 invalid range-bound family diagnostic changed");
  Require(HasCandidateDecision(
              result, api::EngineNoSqlProviderFamily::kKeyValue,
              opt::NoSqlApproxFilterKind::kMinMaxSet,
              "SB_NOSQL_APPROX_FILTER.RANGE_BOUNDS_INVALID"),
          "ODF-080 invalid min/max bounds were not refused before selection");

  request = BaseRequest();
  request.candidates.at(8).range_summary_valid = false;
  result = opt::EvaluateNoSqlApproxFilterDecision(request);
  Require(!result.ok,
          "ODF-080 selected range filter with stale summary was accepted");
  Require(result.diagnostic_code ==
              "SB_NOSQL_APPROX_FILTER.NO_SAFE_FILTER_FOR_FAMILY",
          "ODF-080 stale range-summary family diagnostic changed");
  Require(HasCandidateDecision(
              result, api::EngineNoSqlProviderFamily::kSearch,
              opt::NoSqlApproxFilterKind::kRangeFilter,
              "SB_NOSQL_APPROX_FILTER.RANGE_SUMMARY_STALE"),
          "ODF-080 stale range summary was not refused before selection");

  request = BaseRequest();
  request.candidates.clear();
  for (const auto family : Families()) {
    for (const auto kind : Kinds()) {
      auto candidate = Candidate(family, kind);
      candidate.baseline_cost_units = 100;
      candidate.filter_cost_units = 70;
      candidate.exact_fallback_cost_units = 40;
      request.candidates.push_back(std::move(candidate));
    }
  }
  result = opt::EvaluateNoSqlApproxFilterDecision(request);
  Require(!result.ok, "ODF-080 all-low-benefit matrix was accepted");
  Require(result.diagnostic_code ==
              "SB_NOSQL_APPROX_FILTER.NO_SAFE_FILTER_FOR_FAMILY",
          "ODF-080 low-benefit diagnostic changed");
}

}  // namespace

int main() {
  AllSixFamiliesBenchmarkAndSelectSafeFilters();
  UnsafeAuthorityAndMissingRecheckFailClosed();
  SecurityAndClusterGuardsFailClosed();
  FamilyCompletenessAndBenchmarkAuthorityFailClosed();
  return EXIT_SUCCESS;
}
