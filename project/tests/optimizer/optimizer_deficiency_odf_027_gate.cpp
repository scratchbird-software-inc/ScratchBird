// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_differential_fuzz.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool HasClass(const std::map<opt::OptimizerDifferentialCaseClass, std::size_t>& counts,
              opt::OptimizerDifferentialCaseClass case_class) {
  const auto found = counts.find(case_class);
  return found != counts.end() && found->second != 0;
}

bool CorpusCoverageIsBoundedAndComplete(const std::vector<opt::OptimizerDifferentialCase>& corpus) {
  std::map<opt::OptimizerDifferentialCaseClass, std::size_t> counts;
  for (const auto& test_case : corpus) {
    ++counts[test_case.case_class];
  }
  return Require(corpus.size() == 25, "ODF-027 corpus size drifted") &&
         Require(HasClass(counts, opt::OptimizerDifferentialCaseClass::kPredicateEquivalence),
                 "predicate equivalence cases missing") &&
         Require(HasClass(counts, opt::OptimizerDifferentialCaseClass::kJoinOrderBarrier),
                 "join reorder/barrier cases missing") &&
         Require(HasClass(counts, opt::OptimizerDifferentialCaseClass::kRewriteProof),
                 "rewrite proof/refusal cases missing") &&
         Require(HasClass(counts, opt::OptimizerDifferentialCaseClass::kPlanCacheShape),
                 "plan-cache shape cases missing") &&
         Require(HasClass(counts, opt::OptimizerDifferentialCaseClass::kAccessPathMetadata),
                 "access-path metadata cases missing");
}

bool ReportHasOnlyAcceptedOrExactRefusalEquivalence(
    const opt::OptimizerDifferentialFuzzReport& report) {
  if (!Require(report.mismatch_count == 0, "differential mismatch found")) {
    for (const auto& result : report.results) {
      if (result.outcome == opt::OptimizerDifferentialOutcome::kMismatch) {
        std::cerr << result.test_case.case_id << ": " << result.mismatch_reason << '\n';
      }
    }
    return false;
  }
  return Require(report.accepted_equivalent_count != 0,
                 "accepted-equivalent path was not exercised") &&
         Require(report.exact_refusal_equivalent_count != 0,
                 "exact-refusal-equivalent path was not exercised");
}

bool EvidenceCarriesSemanticDigestsOrExactRefusals(
    const opt::OptimizerDifferentialFuzzReport& report) {
  for (const auto& result : report.results) {
    if (result.outcome == opt::OptimizerDifferentialOutcome::kAcceptedEquivalent) {
      if (!Require(!result.baseline.canonical_semantic_digest.empty(),
                   result.test_case.case_id + " missing baseline semantic digest") ||
          !Require(!result.optimized.canonical_semantic_digest.empty(),
                   result.test_case.case_id + " missing optimized semantic digest") ||
          !Require(result.baseline.canonical_semantic_digest ==
                       result.optimized.canonical_semantic_digest,
                   result.test_case.case_id + " semantic digests diverged") ||
          !Require(result.baseline.result_class == result.optimized.result_class,
                   result.test_case.case_id + " result classes diverged")) {
        return false;
      }
    } else if (result.outcome == opt::OptimizerDifferentialOutcome::kExactRefusalEquivalent) {
      if (!Require(!result.baseline.exact_refusal_diagnostic.empty(),
                   result.test_case.case_id + " missing baseline refusal") ||
          !Require(!result.optimized.exact_refusal_diagnostic.empty(),
                   result.test_case.case_id + " missing optimized refusal") ||
          !Require(result.baseline.exact_refusal_diagnostic ==
                       result.optimized.exact_refusal_diagnostic,
                   result.test_case.case_id + " refusal diagnostics diverged")) {
        return false;
      }
    }
  }
  return true;
}

bool SerializedEvidenceHasNoRuntimeDocDependencyTokens(const std::string& evidence) {
  const std::vector<std::string> forbidden = {
      "docs/", "execution-plans", "findings", "audit", "contracts", "references"};
  for (const auto& token : forbidden) {
    if (!Require(evidence.find(token) == std::string::npos,
                 "serialized evidence leaked forbidden runtime dependency token: " +
                     token)) {
      return false;
    }
  }
  return true;
}

bool MetadataOnlyMGAEvidenceIsPresent(const std::string& evidence) {
  return Require(evidence.find("route_kind=metadata_only") != std::string::npos,
                 "metadata-only route evidence missing") &&
         Require(evidence.find("no_sql_backend_execution=true") != std::string::npos,
                 "SQL backend absence evidence missing") &&
         Require(evidence.find("mga_visibility_authority=engine_recheck_required") !=
                     std::string::npos,
                 "MGA recheck authority evidence missing") &&
         Require(evidence.find("mga_finality_authority=engine_transaction_inventory") !=
                     std::string::npos,
                 "MGA finality authority evidence missing") &&
         Require(evidence.find("parser_or_donor_finality_authority=false") !=
                     std::string::npos,
                 "parser/donor finality rejection evidence missing") &&
         Require(evidence.find("pruning_metadata_not_visibility_or_finality_authority") !=
                     std::string::npos,
                 "pruning metadata advisory evidence missing");
}

}  // namespace

int main() {
  const auto corpus = opt::GenerateOptimizerDifferentialFuzzCorpus();
  if (!CorpusCoverageIsBoundedAndComplete(corpus)) return 1;

  const auto report = opt::RunOptimizerDifferentialFuzzCorpus(corpus);
  if (!ReportHasOnlyAcceptedOrExactRefusalEquivalence(report)) return 1;
  if (!EvidenceCarriesSemanticDigestsOrExactRefusals(report)) return 1;

  const auto evidence = opt::SerializeOptimizerDifferentialEvidence(report);
  if (!SerializedEvidenceHasNoRuntimeDocDependencyTokens(evidence)) return 1;
  if (!MetadataOnlyMGAEvidenceIsPresent(evidence)) return 1;
  return 0;
}
