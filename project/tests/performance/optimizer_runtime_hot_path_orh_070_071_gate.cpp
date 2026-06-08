// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "compression_policy.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using scratchbird::core::index::CompressionCostSource;
using scratchbird::core::index::CompressionFamily;
using scratchbird::core::index::CompressionMethod;
using scratchbird::core::index::CompressionPolicyDecision;
using scratchbird::core::index::CompressionPolicyRequest;
using scratchbird::core::index::DefaultCompressionPolicyRequest;
using scratchbird::core::index::EvaluateCompressionPolicy;

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ORH-070/071 gate failure: " << message << '\n';
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

CompressionPolicyRequest CostedRequest(CompressionFamily family) {
  auto request = DefaultCompressionPolicyRequest(family);
  request.uncompressed_bytes = 16 * 1024;
  request.estimated_compressed_bytes = 15 * 1024;
  request.cost.cpu_cost = 30;
  request.cost.io_savings = 5;
  request.cost.cache_density_gain = 1;
  request.cost.update_frequency_penalty = 20;
  request.cost.read_hotness = 1;
  request.cost.write_hotness = 10;
  request.measured_feedback.present = true;
  request.measured_feedback.compress_ns_per_byte = 2.0;
  request.measured_feedback.decompress_ns_per_byte = 2.5;
  request.measured_feedback.observed_compression_ratio = 0.42;
  request.measured_feedback.cache_hit_improvement = 0.14;
  request.measured_feedback.write_amplification_change = -0.02;
  request.measured_feedback.update_rewrite_cost = 3.0;
  request.measured_feedback.dictionary_miss_rate = 0.01;
  request.measured_feedback.fallback_rate = 0.0;
  request.measured_feedback.sample_count = 512;
  request.measured_feedback.age_ms = 1000;
  return request;
}

void RequireMeasuredEvidence(const CompressionPolicyDecision& decision) {
  Require(HasEvidence(decision, "ORH_COMPRESSION_MEASURED_COST_FEEDBACK"),
          "missing measured-cost search key");
  Require(HasEvidence(decision, "ORH_COMPRESSION_FAMILY_THRESHOLDS"),
          "missing threshold search key");
  Require(HasEvidencePrefix(decision, "compression_measured_compress_ns_per_byte="),
          "missing compress ns/byte evidence");
  Require(HasEvidencePrefix(decision, "compression_measured_decompress_ns_per_byte="),
          "missing decompress ns/byte evidence");
  Require(HasEvidencePrefix(decision, "compression_measured_observed_ratio="),
          "missing observed ratio evidence");
  Require(HasEvidencePrefix(decision, "compression_measured_cache_hit_improvement="),
          "missing cache-hit improvement evidence");
  Require(HasEvidencePrefix(decision,
                            "compression_measured_write_amplification_change="),
          "missing write-amplification evidence");
  Require(HasEvidencePrefix(decision, "compression_measured_update_rewrite_cost="),
          "missing update rewrite evidence");
  Require(HasEvidencePrefix(decision, "compression_measured_dictionary_miss_rate="),
          "missing dictionary miss evidence");
  Require(HasEvidencePrefix(decision, "compression_measured_fallback_rate="),
          "missing fallback-rate evidence");
  Require(HasEvidence(decision, "parser_or_donor_authority=false"),
          "compression policy drifted to parser/donor authority");
  Require(HasEvidence(decision, "wal_or_finality_authority=false"),
          "compression policy drifted to finality authority");
  Require(HasEvidence(decision, "compression_exact_semantic_equivalence=true"),
          "missing exact semantic fallback evidence");
  Require(HasEvidence(decision, "compression_exact_binary_equivalence=true"),
          "missing exact binary fallback evidence");
}

void ProveMeasuredFeedbackChangesDecision() {
  auto request = CostedRequest(CompressionFamily::kBlobPayload);
  const auto measured = EvaluateCompressionPolicy(request);
  Require(measured.accepted, "quality measured feedback did not accept safe case");
  Require(measured.cost_source ==
              CompressionCostSource::kMeasuredRuntimeFeedback,
          "quality measured feedback was not preferred");
  Require(HasEvidence(measured,
                      "compression_cost_source=measured_runtime_feedback"),
          "missing measured source evidence");
  RequireMeasuredEvidence(measured);

  request.measured_feedback.present = false;
  const auto static_only = EvaluateCompressionPolicy(request);
  Require(!static_only.accepted,
          "static estimate unexpectedly accepted measured-only win");
  Require(static_only.cost_source == CompressionCostSource::kStaticEstimate,
          "static fallback source not recorded");
  Require(HasDiagnostic(static_only,
                        "compression_policy.cost_model_not_beneficial_fallback"),
          "static fallback diagnostic missing");
  Require(HasEvidence(static_only, "compression_cost_source=static_estimate"),
          "missing static source evidence");
  RequireMeasuredEvidence(static_only);
}

void ProveLowQualityFeedbackFallsBackToStatic() {
  auto stale = CostedRequest(CompressionFamily::kCandidateSet);
  stale.cost.cpu_cost = 1;
  stale.cost.io_savings = 22;
  stale.cost.cache_density_gain = 8;
  stale.cost.update_frequency_penalty = 1;
  stale.cost.read_hotness = 6;
  stale.cost.write_hotness = 1;
  stale.estimated_compressed_bytes = 4096;
  stale.measured_feedback.age_ms = 20 * 60 * 1000;
  const auto stale_decision = EvaluateCompressionPolicy(stale);
  Require(stale_decision.accepted, "beneficial static estimate was refused");
  Require(stale_decision.cost_source == CompressionCostSource::kStaticEstimate,
          "stale feedback did not fall back to static estimate");
  Require(HasEvidence(stale_decision,
                      "compression_measured_quality_sufficient=false"),
          "missing stale measured quality evidence");

  auto low_sample = stale;
  low_sample.measured_feedback.age_ms = 1000;
  low_sample.measured_feedback.sample_count = 3;
  const auto low_sample_decision = EvaluateCompressionPolicy(low_sample);
  Require(low_sample_decision.cost_source == CompressionCostSource::kStaticEstimate,
          "low-sample feedback did not fall back to static estimate");
  Require(HasEvidence(low_sample_decision,
                      "compression_measured_sample_count=3"),
          "missing low-sample evidence");
}

void ProveFamilyThresholdRefusals() {
  auto small = CostedRequest(CompressionFamily::kCandidateSet);
  small.uncompressed_bytes = 128;
  const auto small_decision = EvaluateCompressionPolicy(small);
  Require(!small_decision.accepted, "small candidate set was accepted");
  Require(HasDiagnostic(
              small_decision,
              "SB_ORH_COMPRESSION_FAMILY_THRESHOLD.MIN_PAYLOAD_REFUSED"),
          "missing minimum payload refusal");

  auto document = CostedRequest(CompressionFamily::kDocumentShape);
  document.dictionary.present = true;
  document.dictionary.reuse_observed = false;
  const auto document_decision = EvaluateCompressionPolicy(document);
  Require(!document_decision.accepted,
          "document shape without dictionary reuse was accepted");
  Require(HasDiagnostic(
              document_decision,
              "SB_ORH_COMPRESSION_FAMILY_THRESHOLD.DICTIONARY_REUSE_REQUIRED"),
          "missing document dictionary reuse refusal");
}

void ProveBinaryFrameDictionaryReuseRequired() {
  auto frame = CostedRequest(CompressionFamily::kBinaryResultFrame);
  frame.dictionary.present = true;
  frame.dictionary.reuse_observed = false;
  const auto decision = EvaluateCompressionPolicy(frame);
  Require(!decision.accepted,
          "binary result frame without dictionary reuse was accepted");
  Require(HasDiagnostic(
              decision,
              "SB_ORH_COMPRESSION_FAMILY_THRESHOLD.DICTIONARY_REUSE_REQUIRED"),
          "binary result frame did not require dictionary reuse");
  Require(HasEvidence(decision, "compression_family=binary_result_frame"),
          "missing binary result frame family evidence");
}

void ProveHotUpdateRowPageMargin() {
  auto row = CostedRequest(CompressionFamily::kRowPage);
  row.update_hot = true;
  row.measured_feedback.observed_compression_ratio = 0.60;
  row.measured_feedback.cache_hit_improvement = 0.02;
  row.measured_feedback.update_rewrite_cost = 20.0;
  const auto refused = EvaluateCompressionPolicy(row);
  Require(!refused.accepted,
          "hot-update row page accepted without higher margin");
  Require(HasDiagnostic(refused, "compression_policy.update_hot_cost_fallback"),
          "missing hot-update margin fallback");
  Require(HasEvidence(refused, "compression_threshold_hot_update_extra_margin=38.000000"),
          "missing hot-update threshold evidence");

  row.measured_feedback.observed_compression_ratio = 0.20;
  row.measured_feedback.cache_hit_improvement = 0.25;
  row.measured_feedback.update_rewrite_cost = 1.0;
  const auto accepted = EvaluateCompressionPolicy(row);
  Require(accepted.accepted,
          "hot-update row page with sufficient measured margin was refused");
}

void ProveVectorRandomAccessAndRerankRefusals() {
  auto vector_random = CostedRequest(CompressionFamily::kVectorCode);
  vector_random.random_access = true;
  vector_random.measured_feedback.decompress_ns_per_byte = 25.0;
  vector_random.measured_feedback.observed_compression_ratio = 0.55;
  vector_random.measured_feedback.cache_hit_improvement = 0.01;
  const auto random_decision = EvaluateCompressionPolicy(vector_random);
  Require(!random_decision.accepted,
          "vector random-access accepted decompression-dominated cost");
  Require(HasDiagnostic(
              random_decision,
              "SB_ORH_COMPRESSION_FAMILY_THRESHOLD.RANDOM_ACCESS_DECOMPRESS_DOMINATES"),
          "missing vector random-access refusal");

  auto vector_rerank = vector_random;
  vector_rerank.random_access = false;
  vector_rerank.vector_rerank = true;
  const auto rerank_decision = EvaluateCompressionPolicy(vector_rerank);
  Require(!rerank_decision.accepted,
          "vector rerank accepted decompression-dominated cost");
  Require(HasDiagnostic(
              rerank_decision,
              "SB_ORH_COMPRESSION_FAMILY_THRESHOLD.VECTOR_RERANK_DECOMPRESS_DOMINATES"),
          "missing vector rerank refusal");
}

void ProveIndexFamiliesDoNotClaimRuntimeClosure() {
  for (const auto family :
       {CompressionFamily::kExactIndexPage, CompressionFamily::kPostingList}) {
    auto request = CostedRequest(family);
    request.runtime_index_compression_requested = true;
    request.index_runtime_correctness_proven = false;
    const auto decision = EvaluateCompressionPolicy(request);
    Require(!decision.accepted,
            "index-family runtime compression was accepted");
    Require(HasDiagnostic(
                decision,
                "SB_ORH_COMPRESSION_FAMILY_THRESHOLD.INDEX_RUNTIME_UNPROVEN"),
            "missing INDEX_RUNTIME_UNPROVEN blocker");
    Require(!decision.index_runtime_closure_claimed,
            "index runtime closure was claimed");
    Require(HasEvidence(decision, "compression_index_runtime_unproven=true"),
            "missing index-runtime-unproven evidence");
    Require(HasEvidence(decision,
                        "compression_index_runtime_closure_claimed=false"),
            "missing no-index-runtime-closure evidence");
  }
}

}  // namespace

int main() {
  ProveMeasuredFeedbackChangesDecision();
  ProveLowQualityFeedbackFallsBackToStatic();
  ProveFamilyThresholdRefusals();
  ProveBinaryFrameDictionaryReuseRequired();
  ProveHotUpdateRowPageMargin();
  ProveVectorRandomAccessAndRerankRefusals();
  ProveIndexFamiliesDoNotClaimRuntimeClosure();
  std::cout << "optimizer_runtime_hot_path_orh_070_071_gate=passed\n";
  return 0;
}
