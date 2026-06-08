// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "compression_policy.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace scratchbird::core::index {
namespace {

bool IsDictionaryFamily(CompressionFamily family) {
  return family == CompressionFamily::kBlobPayload ||
         family == CompressionFamily::kDocumentShape ||
         family == CompressionFamily::kVectorCode ||
         family == CompressionFamily::kBinaryResultFrame;
}

CompressionMethod PreferredMethod(CompressionFamily family) {
  switch (family) {
    case CompressionFamily::kRowPage:
      return CompressionMethod::kDelta;
    case CompressionFamily::kExactIndexPage:
      return CompressionMethod::kUuidV7Prefix;
    case CompressionFamily::kPostingList:
      return CompressionMethod::kRunLength;
    case CompressionFamily::kCandidateSet:
      return CompressionMethod::kBitmap;
    case CompressionFamily::kBlobPayload:
      return CompressionMethod::kDictionary;
    case CompressionFamily::kDocumentShape:
      return CompressionMethod::kDictionary;
    case CompressionFamily::kTimeSeriesMetricPage:
      return CompressionMethod::kDelta;
    case CompressionFamily::kSearchPosting:
      return CompressionMethod::kRunLength;
    case CompressionFamily::kVectorCode:
      return CompressionMethod::kVectorQuantizedCodes;
    case CompressionFamily::kBinaryResultFrame:
      return CompressionMethod::kFrameDictionary;
  }
  return CompressionMethod::kNone;
}

bool DictionaryUsable(const CompressionDictionaryLifecycle& dictionary) {
  if (!dictionary.required) {
    return true;
  }
  return dictionary.present &&
         dictionary.observed_generation == dictionary.current_generation;
}

bool ExactFallbackAndEquivalenceProven(const CompressionPolicyRequest& request) {
  return request.exact_uncompressed_fallback_available &&
         request.exact_semantic_equivalence_proven &&
         request.exact_binary_equivalence_proven;
}

int BenefitScore(const CompressionCostDimensions& cost) {
  return cost.io_savings + cost.cache_density_gain + cost.read_hotness;
}

int PenaltyScore(const CompressionCostDimensions& cost) {
  return cost.cpu_cost + cost.update_frequency_penalty + cost.write_hotness;
}

bool MeasuredFeedbackQualitySufficient(
    const CompressionMeasuredRuntimeFeedback& feedback) {
  return feedback.present && feedback.sample_count >= 128 &&
         feedback.age_ms <= 300000 &&
         feedback.compress_ns_per_byte > 0.0 &&
         feedback.decompress_ns_per_byte > 0.0 &&
         feedback.observed_compression_ratio > 0.0 &&
         feedback.observed_compression_ratio <= 1.0 &&
         feedback.dictionary_miss_rate >= 0.0 &&
         feedback.dictionary_miss_rate <= 1.0 &&
         feedback.fallback_rate >= 0.0 && feedback.fallback_rate <= 1.0;
}

double StaticCompressionRatio(const CompressionPolicyRequest& request) {
  if (request.uncompressed_bytes == 0) {
    return 1.0;
  }
  return static_cast<double>(request.estimated_compressed_bytes) /
         static_cast<double>(request.uncompressed_bytes);
}

double MeasuredBenefitScore(const CompressionMeasuredRuntimeFeedback& feedback) {
  const double saved_byte_fraction =
      std::max(0.0, 1.0 - feedback.observed_compression_ratio);
  const double cache_gain = std::max(0.0, feedback.cache_hit_improvement) * 120.0;
  const double write_gain =
      std::max(0.0, -feedback.write_amplification_change) * 80.0;
  return saved_byte_fraction * 100.0 + cache_gain + write_gain;
}

double MeasuredPenaltyScore(const CompressionMeasuredRuntimeFeedback& feedback,
                            bool update_hot) {
  double score = feedback.compress_ns_per_byte +
                 feedback.decompress_ns_per_byte +
                 std::max(0.0, feedback.write_amplification_change) * 80.0 +
                 feedback.dictionary_miss_rate * 80.0 +
                 feedback.fallback_rate * 100.0;
  if (update_hot) {
    score += feedback.update_rewrite_cost * 1.5;
  }
  return score;
}

bool DecompressionDominates(const CompressionPolicyDecision& decision) {
  if (decision.cost_source == CompressionCostSource::kMeasuredRuntimeFeedback) {
    return decision.measured_feedback.decompress_ns_per_byte >
           decision.cost_benefit_score / 10.0;
  }
  return decision.cost.cpu_cost > decision.cost_benefit_score / 2.0;
}

void AddEvidence(CompressionPolicyDecision* decision, std::string value) {
  decision->evidence.push_back(std::move(value));
}

void AddDiagnostic(CompressionPolicyDecision* decision, std::string value) {
  decision->diagnostics.push_back(std::move(value));
  AddEvidence(decision, "compression_diagnostic=" + decision->diagnostics.back());
}

CompressionPolicyDecision BaseDecision(const CompressionPolicyRequest& request) {
  CompressionPolicyDecision decision;
  decision.family = request.family;
  decision.cost = request.cost;
  decision.measured_feedback = request.measured_feedback;
  decision.measured_feedback_quality_sufficient =
      MeasuredFeedbackQualitySufficient(request.measured_feedback);
  decision.cost_source = decision.measured_feedback_quality_sufficient
                             ? CompressionCostSource::kMeasuredRuntimeFeedback
                             : CompressionCostSource::kStaticEstimate;
  decision.threshold = ThresholdForCompressionFamily(request.family);
  decision.dictionary = request.dictionary;
  decision.exact_uncompressed_fallback_available =
      request.exact_uncompressed_fallback_available;
  decision.exact_semantic_equivalence_proven =
      request.exact_semantic_equivalence_proven;
  decision.exact_binary_equivalence_proven =
      request.exact_binary_equivalence_proven;
  decision.parser_or_donor_authority = request.parser_or_donor_authority;
  decision.wal_or_finality_authority = request.wal_or_finality_authority;
  decision.update_hot = request.update_hot;
  decision.random_access = request.random_access;
  decision.vector_rerank = request.vector_rerank;
  decision.runtime_index_compression_requested =
      request.runtime_index_compression_requested;
  decision.index_runtime_correctness_proven =
      request.index_runtime_correctness_proven;
  decision.index_runtime_closure_claimed =
      request.runtime_index_compression_requested &&
      request.index_runtime_correctness_proven;
  decision.benchmark_clean = request.benchmark_clean;
  decision.uncompressed_bytes = request.uncompressed_bytes;
  decision.estimated_compressed_bytes = request.estimated_compressed_bytes;
  decision.byte_delta =
      static_cast<std::int64_t>(request.uncompressed_bytes) -
      static_cast<std::int64_t>(request.estimated_compressed_bytes);
  if (decision.cost_source == CompressionCostSource::kMeasuredRuntimeFeedback) {
    decision.effective_compression_ratio =
        request.measured_feedback.observed_compression_ratio;
    decision.cost_benefit_score =
        MeasuredBenefitScore(request.measured_feedback);
    decision.cost_penalty_score =
        MeasuredPenaltyScore(request.measured_feedback, request.update_hot);
  } else {
    decision.effective_compression_ratio = StaticCompressionRatio(request);
    decision.cost_benefit_score = BenefitScore(request.cost);
    decision.cost_penalty_score = PenaltyScore(request.cost);
  }
  decision.required_margin =
      decision.threshold.minimum_margin +
      (request.update_hot ? decision.threshold.hot_update_extra_margin : 0.0);
  AddEvidence(&decision, kCompressionPolicyByFamilySearchKey);
  AddEvidence(&decision, "ORH_COMPRESSION_MEASURED_COST_FEEDBACK");
  AddEvidence(&decision, "ORH_COMPRESSION_FAMILY_THRESHOLDS");
  AddEvidence(&decision, "compression_family=" +
                             std::string(CompressionFamilyName(request.family)));
  AddEvidence(&decision, "compression_cost_source=" +
                             std::string(CompressionCostSourceName(
                                 decision.cost_source)));
  AddEvidence(&decision, "compression_measured_quality_sufficient=" +
                             std::string(
                                 decision.measured_feedback_quality_sufficient
                                     ? "true"
                                     : "false"));
  AddEvidence(&decision, "compression_measured_sample_count=" +
                             std::to_string(
                                 request.measured_feedback.sample_count));
  AddEvidence(&decision, "compression_measured_age_ms=" +
                             std::to_string(request.measured_feedback.age_ms));
  AddEvidence(&decision, "compression_measured_compress_ns_per_byte=" +
                             std::to_string(
                                 request.measured_feedback.compress_ns_per_byte));
  AddEvidence(&decision, "compression_measured_decompress_ns_per_byte=" +
                             std::to_string(request.measured_feedback
                                                .decompress_ns_per_byte));
  AddEvidence(&decision, "compression_measured_observed_ratio=" +
                             std::to_string(request.measured_feedback
                                                .observed_compression_ratio));
  AddEvidence(&decision, "compression_measured_cache_hit_improvement=" +
                             std::to_string(request.measured_feedback
                                                .cache_hit_improvement));
  AddEvidence(&decision, "compression_measured_write_amplification_change=" +
                             std::to_string(request.measured_feedback
                                                .write_amplification_change));
  AddEvidence(&decision, "compression_measured_update_rewrite_cost=" +
                             std::to_string(request.measured_feedback
                                                .update_rewrite_cost));
  AddEvidence(&decision, "compression_measured_dictionary_miss_rate=" +
                             std::to_string(request.measured_feedback
                                                .dictionary_miss_rate));
  AddEvidence(&decision, "compression_measured_fallback_rate=" +
                             std::to_string(
                                 request.measured_feedback.fallback_rate));
  AddEvidence(&decision, "compression_effective_ratio=" +
                             std::to_string(
                                 decision.effective_compression_ratio));
  AddEvidence(&decision, "compression_threshold_minimum_payload_bytes=" +
                             std::to_string(
                                 decision.threshold.minimum_payload_bytes));
  AddEvidence(&decision, "compression_threshold_minimum_margin=" +
                             std::to_string(decision.threshold.minimum_margin));
  AddEvidence(&decision, "compression_threshold_hot_update_extra_margin=" +
                             std::to_string(
                                 decision.threshold.hot_update_extra_margin));
  AddEvidence(&decision, "parser_or_donor_authority=" +
                             std::string(request.parser_or_donor_authority
                                             ? "true"
                                             : "false"));
  AddEvidence(&decision, "wal_or_finality_authority=" +
                             std::string(request.wal_or_finality_authority
                                             ? "true"
                                             : "false"));
  AddEvidence(&decision,
              "compression_cost_cpu=" + std::to_string(request.cost.cpu_cost));
  AddEvidence(&decision, "compression_cost_io_savings=" +
                             std::to_string(request.cost.io_savings));
  AddEvidence(&decision, "compression_cost_cache_density_gain=" +
                             std::to_string(request.cost.cache_density_gain));
  AddEvidence(&decision, "compression_cost_update_frequency_penalty=" +
                             std::to_string(
                                 request.cost.update_frequency_penalty));
  AddEvidence(&decision, "compression_cost_read_hotness=" +
                             std::to_string(request.cost.read_hotness));
  AddEvidence(&decision, "compression_cost_write_hotness=" +
                             std::to_string(request.cost.write_hotness));
  AddEvidence(&decision, "compression_dictionary_required=" +
                             std::string(request.dictionary.required ? "true"
                                                                     : "false"));
  AddEvidence(&decision, "compression_dictionary_present=" +
                             std::string(request.dictionary.present ? "true"
                                                                    : "false"));
  AddEvidence(&decision, "compression_dictionary_reuse_observed=" +
                             std::string(request.dictionary.reuse_observed
                                             ? "true"
                                             : "false"));
  AddEvidence(&decision, "compression_dictionary_reuse_required=" +
                             std::string(
                                 decision.threshold.dictionary_reuse_required
                                     ? "true"
                                     : "false"));
  AddEvidence(&decision, "compression_dictionary_generation=" +
                             std::to_string(
                                 request.dictionary.observed_generation));
  AddEvidence(&decision, "compression_dictionary_current_generation=" +
                             std::to_string(request.dictionary.current_generation));
  AddEvidence(&decision, "compression_training_reason=" +
                             request.dictionary.training_reason);
  AddEvidence(&decision, "compression_retraining_reason=" +
                             request.dictionary.retraining_reason);
  AddEvidence(&decision, "compression_stale_dictionary_fallback=" +
                             std::string(
                                 request.dictionary.stale_dictionary_fallback
                                     ? "true"
                                     : "false"));
  AddEvidence(&decision, "compression_exact_uncompressed_fallback=" +
                             std::string(
                                 request.exact_uncompressed_fallback_available
                                     ? "true"
                                     : "false"));
  AddEvidence(&decision, "compression_exact_semantic_equivalence=" +
                             std::string(
                                 request.exact_semantic_equivalence_proven
                                     ? "true"
                                     : "false"));
  AddEvidence(&decision, "compression_exact_binary_equivalence=" +
                             std::string(
                                 request.exact_binary_equivalence_proven
                                     ? "true"
                                     : "false"));
  AddEvidence(&decision, "compression_uncompressed_bytes=" +
                             std::to_string(request.uncompressed_bytes));
  AddEvidence(&decision, "compression_estimated_compressed_bytes=" +
                             std::to_string(
                                 request.estimated_compressed_bytes));
  AddEvidence(&decision,
              "compression_byte_delta=" + std::to_string(decision.byte_delta));
  AddEvidence(&decision, "compression_random_access=" +
                             std::string(request.random_access ? "true"
                                                               : "false"));
  AddEvidence(&decision, "compression_vector_rerank=" +
                             std::string(request.vector_rerank ? "true"
                                                               : "false"));
  AddEvidence(&decision, "compression_runtime_index_requested=" +
                             std::string(request.runtime_index_compression_requested
                                             ? "true"
                                             : "false"));
  AddEvidence(&decision, "compression_index_runtime_correctness_proven=" +
                             std::string(request.index_runtime_correctness_proven
                                             ? "true"
                                             : "false"));
  AddEvidence(&decision, "compression_index_runtime_unproven=" +
                             std::string(decision.threshold.index_runtime_unproven
                                             ? "true"
                                             : "false"));
  AddEvidence(&decision, "compression_index_runtime_closure_claimed=" +
                             std::string(decision.index_runtime_closure_claimed
                                             ? "true"
                                             : "false"));
  AddEvidence(&decision, "benchmark_clean=" +
                             std::string(request.benchmark_clean ? "true"
                                                                 : "false"));
  return decision;
}

CompressionPolicyDecision Finalize(CompressionPolicyDecision decision) {
  AddEvidence(&decision, "compression_eligibility=" +
                             std::string(CompressionEligibilityName(
                                 decision.eligibility)));
  AddEvidence(&decision, "compression_method=" +
                             std::string(CompressionMethodName(decision.method)));
  AddEvidence(&decision, "compression_accepted=" +
                             std::string(decision.accepted ? "true" : "false"));
  AddEvidence(&decision, "compression_fallback=" +
                             std::string(decision.fallback ? "true" : "false"));
  AddEvidence(&decision, "compression_cost_benefit_score=" +
                             std::to_string(decision.cost_benefit_score));
  AddEvidence(&decision, "compression_cost_penalty_score=" +
                             std::to_string(decision.cost_penalty_score));
  AddEvidence(&decision, "compression_required_margin=" +
                             std::to_string(decision.required_margin));
  return decision;
}

CompressionPolicyDecision Fallback(CompressionPolicyDecision decision,
                                   std::string diagnostic) {
  decision.eligibility = CompressionEligibility::kMustFallback;
  decision.method = CompressionMethod::kNone;
  decision.accepted = false;
  decision.fallback = true;
  AddDiagnostic(&decision, std::move(diagnostic));
  return Finalize(std::move(decision));
}

CompressionPolicyDecision Refuse(CompressionPolicyDecision decision,
                                 std::string diagnostic) {
  decision.eligibility = CompressionEligibility::kRefused;
  decision.method = CompressionMethod::kNone;
  decision.accepted = false;
  decision.fallback = false;
  AddDiagnostic(&decision, std::move(diagnostic));
  return Finalize(std::move(decision));
}

}  // namespace

const std::vector<CompressionFamily>& RequiredCompressionFamilies() {
  static const std::vector<CompressionFamily> families = {
      CompressionFamily::kRowPage,
      CompressionFamily::kExactIndexPage,
      CompressionFamily::kPostingList,
      CompressionFamily::kCandidateSet,
      CompressionFamily::kBlobPayload,
      CompressionFamily::kDocumentShape,
      CompressionFamily::kTimeSeriesMetricPage,
      CompressionFamily::kSearchPosting,
      CompressionFamily::kVectorCode,
      CompressionFamily::kBinaryResultFrame};
  return families;
}

const char* CompressionFamilyName(CompressionFamily family) {
  switch (family) {
    case CompressionFamily::kRowPage:
      return "row_page";
    case CompressionFamily::kExactIndexPage:
      return "exact_index_page";
    case CompressionFamily::kPostingList:
      return "posting_list";
    case CompressionFamily::kCandidateSet:
      return "candidate_set";
    case CompressionFamily::kBlobPayload:
      return "blob_payload";
    case CompressionFamily::kDocumentShape:
      return "document_shape";
    case CompressionFamily::kTimeSeriesMetricPage:
      return "time_series_metric_page";
    case CompressionFamily::kSearchPosting:
      return "search_posting";
    case CompressionFamily::kVectorCode:
      return "vector_code";
    case CompressionFamily::kBinaryResultFrame:
      return "binary_result_frame";
  }
  return "unknown";
}

const char* CompressionEligibilityName(CompressionEligibility eligibility) {
  switch (eligibility) {
    case CompressionEligibility::kEligible:
      return "eligible";
    case CompressionEligibility::kIneligible:
      return "ineligible";
    case CompressionEligibility::kMustFallback:
      return "must_fallback";
    case CompressionEligibility::kRefused:
      return "refused";
  }
  return "unknown";
}

const char* CompressionMethodName(CompressionMethod method) {
  switch (method) {
    case CompressionMethod::kNone:
      return "none";
    case CompressionMethod::kDictionary:
      return "dictionary";
    case CompressionMethod::kDelta:
      return "delta";
    case CompressionMethod::kRunLength:
      return "run_length";
    case CompressionMethod::kBitmap:
      return "bitmap";
    case CompressionMethod::kUuidV7Prefix:
      return "uuidv7_prefix";
    case CompressionMethod::kFrameDictionary:
      return "frame_dictionary";
    case CompressionMethod::kVectorQuantizedCodes:
      return "vector_quantized_codes";
  }
  return "unknown";
}

const char* CompressionCostSourceName(CompressionCostSource source) {
  switch (source) {
    case CompressionCostSource::kStaticEstimate:
      return "static_estimate";
    case CompressionCostSource::kMeasuredRuntimeFeedback:
      return "measured_runtime_feedback";
  }
  return "unknown";
}

CompressionFamilyThreshold ThresholdForCompressionFamily(
    CompressionFamily family) {
  switch (family) {
    case CompressionFamily::kRowPage:
      return {.family = family,
              .minimum_payload_bytes = 2048,
              .minimum_margin = 18.0,
              .hot_update_extra_margin = 38.0};
    case CompressionFamily::kExactIndexPage:
      return {.family = family,
              .minimum_payload_bytes = 4096,
              .minimum_margin = 24.0,
              .index_runtime_unproven = true};
    case CompressionFamily::kPostingList:
      return {.family = family,
              .minimum_payload_bytes = 4096,
              .minimum_margin = 22.0,
              .index_runtime_unproven = true};
    case CompressionFamily::kCandidateSet:
      return {.family = family,
              .minimum_payload_bytes = 1024,
              .minimum_margin = 12.0};
    case CompressionFamily::kBlobPayload:
      return {.family = family,
              .minimum_payload_bytes = 8192,
              .minimum_margin = 16.0};
    case CompressionFamily::kDocumentShape:
      return {.family = family,
              .minimum_payload_bytes = 2048,
              .minimum_margin = 20.0,
              .dictionary_required = true,
              .dictionary_reuse_required = true};
    case CompressionFamily::kTimeSeriesMetricPage:
      return {.family = family,
              .minimum_payload_bytes = 4096,
              .minimum_margin = 14.0};
    case CompressionFamily::kSearchPosting:
      return {.family = family,
              .minimum_payload_bytes = 4096,
              .minimum_margin = 20.0,
              .dictionary_required = true,
              .dictionary_reuse_required = true};
    case CompressionFamily::kVectorCode:
      return {.family = family,
              .minimum_payload_bytes = 8192,
              .minimum_margin = 28.0,
              .refuse_random_access_when_decompress_dominates = true,
              .refuse_vector_rerank_when_decompress_dominates = true};
    case CompressionFamily::kBinaryResultFrame:
      return {.family = family,
              .minimum_payload_bytes = 1024,
              .minimum_margin = 10.0,
              .dictionary_required = true,
              .dictionary_reuse_required = true};
  }
  return {};
}

CompressionPolicyRequest DefaultCompressionPolicyRequest(
    CompressionFamily family) {
  CompressionPolicyRequest request;
  request.family = family;
  request.cost.cpu_cost = 2;
  request.cost.io_savings = 8;
  request.cost.cache_density_gain = 5;
  request.cost.update_frequency_penalty = 1;
  request.cost.read_hotness = 4;
  request.cost.write_hotness = 1;
  const auto threshold = ThresholdForCompressionFamily(family);
  request.dictionary.required =
      IsDictionaryFamily(family) || threshold.dictionary_required;
  request.dictionary.present = request.dictionary.required;
  request.dictionary.reuse_observed = request.dictionary.present;
  request.dictionary.observed_generation = request.dictionary.required ? 1 : 0;
  request.dictionary.current_generation = request.dictionary.required ? 1 : 0;
  request.dictionary.training_reason =
      request.dictionary.required ? "family_training_available" : "not_required";
  request.dictionary.retraining_reason =
      request.dictionary.required ? "current_generation" : "not_required";
  request.dictionary.stale_dictionary_fallback = true;
  request.exact_uncompressed_fallback_available = true;
  request.exact_semantic_equivalence_proven = true;
  request.exact_binary_equivalence_proven = true;
  request.parser_or_donor_authority = false;
  request.wal_or_finality_authority = false;
  request.update_hot = false;
  request.random_access = false;
  request.vector_rerank = false;
  request.runtime_index_compression_requested = false;
  request.index_runtime_correctness_proven = false;
  request.benchmark_clean = true;
  request.uncompressed_bytes = 4096;
  request.estimated_compressed_bytes = 1536;
  return request;
}

CompressionPolicyDecision EvaluateCompressionPolicy(
    const CompressionPolicyRequest& request) {
  auto decision = BaseDecision(request);
  if (request.parser_or_donor_authority) {
    return Refuse(std::move(decision),
                  "compression_policy.unsafe_parser_or_donor_authority");
  }
  if (request.wal_or_finality_authority) {
    return Refuse(std::move(decision),
                  "compression_policy.unsafe_wal_or_finality_authority");
  }
  if (!ExactFallbackAndEquivalenceProven(request)) {
    return Refuse(std::move(decision),
                  "compression_policy.exact_fallback_or_equivalence_missing");
  }
  if (decision.threshold.index_runtime_unproven &&
      request.runtime_index_compression_requested &&
      !request.index_runtime_correctness_proven) {
    return Fallback(
        std::move(decision),
        "SB_ORH_COMPRESSION_FAMILY_THRESHOLD.INDEX_RUNTIME_UNPROVEN");
  }
  if (request.uncompressed_bytes < decision.threshold.minimum_payload_bytes) {
    return Fallback(
        std::move(decision),
        "SB_ORH_COMPRESSION_FAMILY_THRESHOLD.MIN_PAYLOAD_REFUSED");
  }
  if (decision.threshold.dictionary_reuse_required &&
      request.dictionary.present &&
      !request.dictionary.reuse_observed) {
    return Fallback(
        std::move(decision),
        "SB_ORH_COMPRESSION_FAMILY_THRESHOLD.DICTIONARY_REUSE_REQUIRED");
  }
  if (decision.threshold.refuse_random_access_when_decompress_dominates &&
      request.random_access && DecompressionDominates(decision)) {
    return Fallback(
        std::move(decision),
        "SB_ORH_COMPRESSION_FAMILY_THRESHOLD.RANDOM_ACCESS_DECOMPRESS_DOMINATES");
  }
  if (decision.threshold.refuse_vector_rerank_when_decompress_dominates &&
      request.vector_rerank && DecompressionDominates(decision)) {
    return Fallback(
        std::move(decision),
        "SB_ORH_COMPRESSION_FAMILY_THRESHOLD.VECTOR_RERANK_DECOMPRESS_DOMINATES");
  }
  if (request.update_hot && decision.threshold.refuse_hot_updates) {
    return Fallback(
        std::move(decision),
        "SB_ORH_COMPRESSION_FAMILY_THRESHOLD.HOT_UPDATE_REFUSED");
  }
  if (request.dictionary.required && !DictionaryUsable(request.dictionary)) {
    const bool missing = !request.dictionary.present;
    return Fallback(std::move(decision),
                    missing ? "compression_policy.dictionary_missing_fallback"
                            : "compression_policy.dictionary_stale_fallback");
  }
  if (request.update_hot &&
      (request.family == CompressionFamily::kRowPage ||
       request.family == CompressionFamily::kExactIndexPage) &&
      decision.cost_benefit_score <=
          decision.cost_penalty_score + decision.required_margin) {
    return Fallback(std::move(decision),
                    "compression_policy.update_hot_cost_fallback");
  }
  if (decision.effective_compression_ratio >= 1.0 ||
      decision.cost_benefit_score <=
          decision.cost_penalty_score + decision.required_margin) {
    return Fallback(std::move(decision),
                    "compression_policy.cost_model_not_beneficial_fallback");
  }

  decision.eligibility = CompressionEligibility::kEligible;
  decision.method = PreferredMethod(request.family);
  decision.accepted = true;
  decision.fallback = false;
  return Finalize(std::move(decision));
}

}  // namespace scratchbird::core::index
