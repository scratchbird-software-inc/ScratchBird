// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "candidate_set.hpp"
#include "compression_policy.hpp"
#include "direct_binary_result_frame.hpp"
#include "nosql/nosql_physical_provider_contract.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

using scratchbird::core::index::CandidateSetCompressionPolicyFamilyName;
using scratchbird::core::index::CompressionEligibility;
using scratchbird::core::index::CompressionFamily;
using scratchbird::core::index::CompressionFamilyName;
using scratchbird::core::index::CompressionMethod;
using scratchbird::core::index::CompressionPolicyDecision;
using scratchbird::core::index::CompressionPolicyRequest;
using scratchbird::core::index::DefaultCompressionPolicyRequest;
using scratchbird::core::index::EvaluateCompressionPolicy;
using scratchbird::core::index::RequiredCompressionFamilies;
using scratchbird::core::index::kCompressionPolicyByFamilySearchKey;
namespace nosql = scratchbird::engine::internal_api;
namespace wire = scratchbird::wire;

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ODFR-030 gate failure: " << message << '\n';
    std::exit(1);
  }
}

bool HasEvidence(const CompressionPolicyDecision& decision,
                 const std::string& value) {
  return std::find(decision.evidence.begin(), decision.evidence.end(), value) !=
         decision.evidence.end();
}

bool HasEvidencePrefix(const CompressionPolicyDecision& decision,
                       const std::string& prefix) {
  return std::any_of(decision.evidence.begin(), decision.evidence.end(),
                     [&](const std::string& item) {
                       return item.rfind(prefix, 0) == 0;
                     });
}

bool HasDiagnostic(const CompressionPolicyDecision& decision,
                   const std::string& diagnostic) {
  return std::find(decision.diagnostics.begin(), decision.diagnostics.end(),
                   diagnostic) != decision.diagnostics.end();
}

bool HasStringEvidence(const std::vector<std::string>& evidence,
                       const std::string& value) {
  return std::find(evidence.begin(), evidence.end(), value) != evidence.end();
}

CompressionPolicyRequest BeneficialRequest(CompressionFamily family) {
  auto request = DefaultCompressionPolicyRequest(family);
  request.cost.cpu_cost = 1;
  request.cost.io_savings = 18;
  request.cost.cache_density_gain = 9;
  request.cost.update_frequency_penalty = 1;
  request.cost.read_hotness = 8;
  request.cost.write_hotness = 1;
  request.uncompressed_bytes = 8192;
  request.estimated_compressed_bytes = 2048;
  return request;
}

void RequireBenchmarkEvidence(const CompressionPolicyDecision& decision) {
  Require(HasEvidence(decision, kCompressionPolicyByFamilySearchKey),
          "missing search-key evidence");
  Require(HasEvidencePrefix(decision, "compression_family="),
          "missing family evidence");
  Require(HasEvidencePrefix(decision, "compression_method="),
          "missing method evidence");
  Require(HasEvidencePrefix(decision, "compression_eligibility="),
          "missing eligibility evidence");
  Require(HasEvidencePrefix(decision, "compression_cost_cpu="),
          "missing CPU cost evidence");
  Require(HasEvidencePrefix(decision, "compression_cost_io_savings="),
          "missing IO savings evidence");
  Require(HasEvidencePrefix(decision, "compression_cost_cache_density_gain="),
          "missing cache-density evidence");
  Require(HasEvidencePrefix(decision,
                            "compression_cost_update_frequency_penalty="),
          "missing update-frequency penalty evidence");
  Require(HasEvidencePrefix(decision, "compression_cost_read_hotness="),
          "missing read hotness evidence");
  Require(HasEvidencePrefix(decision, "compression_cost_write_hotness="),
          "missing write hotness evidence");
  Require(HasEvidencePrefix(decision, "compression_dictionary_required="),
          "missing dictionary required evidence");
  Require(HasEvidencePrefix(decision, "compression_dictionary_generation="),
          "missing dictionary generation evidence");
  Require(HasEvidencePrefix(decision, "compression_training_reason="),
          "missing training reason evidence");
  Require(HasEvidencePrefix(decision, "compression_retraining_reason="),
          "missing retraining reason evidence");
  Require(HasEvidencePrefix(decision, "compression_stale_dictionary_fallback="),
          "missing stale dictionary fallback evidence");
  Require(HasEvidence(decision, "compression_exact_uncompressed_fallback=true"),
          "missing exact uncompressed fallback evidence");
  Require(HasEvidence(decision, "compression_exact_semantic_equivalence=true"),
          "missing exact semantic equivalence evidence");
  Require(HasEvidence(decision, "compression_exact_binary_equivalence=true"),
          "missing exact binary equivalence evidence");
  Require(HasEvidencePrefix(decision, "compression_byte_delta="),
          "missing byte delta evidence");
  Require(HasEvidence(decision, "parser_or_donor_authority=false"),
          "missing parser/donor non-authority evidence");
  Require(HasEvidence(decision, "wal_or_finality_authority=false"),
          "missing WAL/finality non-authority evidence");
  Require(HasEvidence(decision, "benchmark_clean=true"),
          "missing benchmark-clean evidence");
}

void ProveRequiredFamilies() {
  const std::set<std::string> expected = {
      "row_page",
      "exact_index_page",
      "posting_list",
      "candidate_set",
      "blob_payload",
      "document_shape",
      "time_series_metric_page",
      "search_posting",
      "vector_code",
      "binary_result_frame"};
  std::set<std::string> observed;
  for (const auto family : RequiredCompressionFamilies()) {
    observed.insert(CompressionFamilyName(family));
  }
  Require(observed == expected, "required compression family set mismatch");
  Require(std::string(CandidateSetCompressionPolicyFamilyName()) ==
              "candidate_set",
          "candidate-set adapter does not use central compression family name");
}

void ProveCostedSelectionAndFallback() {
  for (const auto family : RequiredCompressionFamilies()) {
    auto accepted = EvaluateCompressionPolicy(BeneficialRequest(family));
    Require(accepted.accepted, std::string("beneficial family not accepted: ") +
                                   CompressionFamilyName(family));
    Require(!accepted.fallback, "accepted decision unexpectedly fell back");
    Require(accepted.eligibility == CompressionEligibility::kEligible,
            "accepted decision eligibility mismatch");
    Require(accepted.method != CompressionMethod::kNone,
            "accepted decision selected no method");
    RequireBenchmarkEvidence(accepted);
  }

  auto row_hot = BeneficialRequest(CompressionFamily::kRowPage);
  row_hot.update_hot = true;
  row_hot.cost.cpu_cost = 10;
  row_hot.cost.update_frequency_penalty = 10;
  row_hot.cost.write_hotness = 8;
  row_hot.cost.io_savings = 8;
  row_hot.cost.cache_density_gain = 3;
  row_hot.cost.read_hotness = 3;
  const auto row_hot_decision = EvaluateCompressionPolicy(row_hot);
  Require(row_hot_decision.fallback, "update-hot row page did not fallback");
  Require(row_hot_decision.method == CompressionMethod::kNone,
          "update-hot row page selected compression");
  Require(HasDiagnostic(row_hot_decision,
                        "compression_policy.update_hot_cost_fallback"),
          "missing update-hot fallback diagnostic");
  RequireBenchmarkEvidence(row_hot_decision);
}

void ProveDictionaryLifecycleFallbacks() {
  auto missing = BeneficialRequest(CompressionFamily::kDocumentShape);
  missing.dictionary.present = false;
  const auto missing_decision = EvaluateCompressionPolicy(missing);
  Require(missing_decision.fallback, "missing dictionary did not fallback");
  Require(HasDiagnostic(missing_decision,
                        "compression_policy.dictionary_missing_fallback"),
          "missing dictionary fallback diagnostic absent");
  RequireBenchmarkEvidence(missing_decision);

  auto stale = BeneficialRequest(CompressionFamily::kBinaryResultFrame);
  stale.dictionary.present = true;
  stale.dictionary.observed_generation = 3;
  stale.dictionary.current_generation = 4;
  stale.dictionary.retraining_reason = "generation_advanced";
  const auto stale_decision = EvaluateCompressionPolicy(stale);
  Require(stale_decision.fallback, "stale dictionary did not fallback");
  Require(HasDiagnostic(stale_decision,
                        "compression_policy.dictionary_stale_fallback"),
          "stale dictionary fallback diagnostic absent");
  RequireBenchmarkEvidence(stale_decision);
}

void ProveExplicitProviderFamilies() {
  const auto candidate =
      EvaluateCompressionPolicy(BeneficialRequest(CompressionFamily::kCandidateSet));
  Require(candidate.accepted, "candidate_set compression policy not accepted");
  Require(HasEvidence(candidate, "compression_family=candidate_set"),
          "candidate_set family not explicit");
  Require(HasEvidence(candidate, "compression_method=bitmap"),
          "candidate_set did not select bitmap method");

  const auto frame = EvaluateCompressionPolicy(
      BeneficialRequest(CompressionFamily::kBinaryResultFrame));
  Require(frame.accepted, "binary_result_frame policy not accepted");
  Require(HasEvidence(frame, "compression_family=binary_result_frame"),
          "binary_result_frame family not explicit");
  Require(HasEvidence(frame, "compression_method=frame_dictionary"),
          "binary_result_frame did not select frame dictionary method");
}

void ProveNoSqlProviderCompressionAdapters() {
  using nosql::EngineNoSqlProviderCompressionPolicyEvidence;
  using nosql::EngineNoSqlProviderCompressionPolicyFamilyName;
  using nosql::EngineNoSqlProviderFamily;

  struct Mapping {
    EngineNoSqlProviderFamily provider_family;
    CompressionFamily compression_family;
  };
  const std::vector<Mapping> mappings = {
      {EngineNoSqlProviderFamily::kDocument, CompressionFamily::kDocumentShape},
      {EngineNoSqlProviderFamily::kSearch, CompressionFamily::kSearchPosting},
      {EngineNoSqlProviderFamily::kVector, CompressionFamily::kVectorCode},
      {EngineNoSqlProviderFamily::kTimeSeries,
       CompressionFamily::kTimeSeriesMetricPage},
  };
  for (const auto& mapping : mappings) {
    const auto* expected = CompressionFamilyName(mapping.compression_family);
    Require(std::string(EngineNoSqlProviderCompressionPolicyFamilyName(
                mapping.provider_family)) == expected,
            std::string("NoSQL compression family mapping mismatch for ") +
                expected);
    const auto evidence =
        EngineNoSqlProviderCompressionPolicyEvidence(mapping.provider_family);
    Require(HasStringEvidence(evidence, kCompressionPolicyByFamilySearchKey),
            "NoSQL adapter missing ODFR-030 search key");
    Require(HasStringEvidence(evidence,
                              std::string("compression_family=") + expected),
            "NoSQL adapter does not use central compression family name");
    Require(HasStringEvidence(evidence, "compression_metadata_only=true"),
            "NoSQL adapter must be metadata-only");
    Require(HasStringEvidence(evidence, "provider_finality_authority=false"),
            "NoSQL adapter must not claim provider finality authority");
    Require(HasStringEvidence(evidence, "parser_or_donor_authority=false"),
            "NoSQL adapter must not claim parser/donor authority");
  }
}

void ProveWireResultFrameCompressionAdapter() {
  const auto* expected =
      CompressionFamilyName(CompressionFamily::kBinaryResultFrame);
  Require(std::string(wire::DirectBinaryResultFrameCompressionPolicyFamilyName()) ==
              expected,
          "direct binary result frame adapter does not use central family name");
  const auto evidence = wire::DirectBinaryResultFrameCompressionPolicyEvidence();
  Require(HasStringEvidence(evidence, kCompressionPolicyByFamilySearchKey),
          "wire adapter missing ODFR-030 search key");
  Require(HasStringEvidence(evidence,
                            std::string("compression_family=") + expected),
          "wire adapter does not expose binary_result_frame family");
  Require(HasStringEvidence(evidence, "compression_metadata_only=true"),
          "wire adapter must be metadata-only");
  Require(HasStringEvidence(evidence,
                            "direct_binary_frame.finality_authority=false"),
          "wire adapter must not claim finality authority");
  Require(HasStringEvidence(evidence,
                            "direct_binary_frame.parser_authority=false"),
          "wire adapter must not claim parser authority");
}

void ProveUnsafeAuthorityRefused() {
  auto parser = BeneficialRequest(CompressionFamily::kPostingList);
  parser.parser_or_donor_authority = true;
  const auto parser_decision = EvaluateCompressionPolicy(parser);
  Require(!parser_decision.accepted && !parser_decision.fallback,
          "parser/donor authority was not refused");
  Require(HasDiagnostic(parser_decision,
                        "compression_policy.unsafe_parser_or_donor_authority"),
          "missing parser/donor authority diagnostic");

  auto wal = BeneficialRequest(CompressionFamily::kSearchPosting);
  wal.wal_or_finality_authority = true;
  const auto wal_decision = EvaluateCompressionPolicy(wal);
  Require(!wal_decision.accepted && !wal_decision.fallback,
          "WAL/finality authority was not refused");
  Require(HasDiagnostic(wal_decision,
                        "compression_policy.unsafe_wal_or_finality_authority"),
          "missing WAL/finality authority diagnostic");

  auto unsafe_exact = BeneficialRequest(CompressionFamily::kBlobPayload);
  unsafe_exact.exact_binary_equivalence_proven = false;
  const auto exact_decision = EvaluateCompressionPolicy(unsafe_exact);
  Require(!exact_decision.accepted && !exact_decision.fallback,
          "missing exact equivalence was not refused");
  Require(HasDiagnostic(
              exact_decision,
              "compression_policy.exact_fallback_or_equivalence_missing"),
          "missing exact fallback/equivalence diagnostic");
}

}  // namespace

int main() {
  ProveRequiredFamilies();
  ProveCostedSelectionAndFallback();
  ProveDictionaryLifecycleFallbacks();
  ProveExplicitProviderFamilies();
  ProveNoSqlProviderCompressionAdapters();
  ProveWireResultFrameCompressionAdapter();
  ProveUnsafeAuthorityRefused();
  return 0;
}
