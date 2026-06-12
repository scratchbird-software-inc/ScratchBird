// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// ODFR_COMPRESSION_POLICY_BY_FAMILY
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::core::index {

constexpr const char* kCompressionPolicyByFamilySearchKey =
    "ODFR_COMPRESSION_POLICY_BY_FAMILY";

enum class CompressionFamily {
  kRowPage,
  kExactIndexPage,
  kPostingList,
  kCandidateSet,
  kBlobPayload,
  kDocumentShape,
  kTimeSeriesMetricPage,
  kSearchPosting,
  kVectorCode,
  kBinaryResultFrame
};

enum class CompressionEligibility {
  kEligible,
  kIneligible,
  kMustFallback,
  kRefused
};

enum class CompressionMethod {
  kNone,
  kDictionary,
  kDelta,
  kRunLength,
  kBitmap,
  kUuidV7Prefix,
  kFrameDictionary,
  kVectorQuantizedCodes
};

enum class CompressionCostSource {
  kStaticEstimate,
  kMeasuredRuntimeFeedback
};

struct CompressionCostDimensions {
  int cpu_cost = 0;
  int io_savings = 0;
  int cache_density_gain = 0;
  int update_frequency_penalty = 0;
  int read_hotness = 0;
  int write_hotness = 0;
};

struct CompressionMeasuredRuntimeFeedback {
  bool present = false;
  double compress_ns_per_byte = 0.0;
  double decompress_ns_per_byte = 0.0;
  double observed_compression_ratio = 1.0;
  double cache_hit_improvement = 0.0;
  double write_amplification_change = 0.0;
  double update_rewrite_cost = 0.0;
  double dictionary_miss_rate = 0.0;
  double fallback_rate = 0.0;
  std::uint64_t sample_count = 0;
  std::uint64_t age_ms = 0;
};

struct CompressionDictionaryLifecycle {
  bool required = false;
  bool present = false;
  bool reuse_observed = false;
  std::uint64_t observed_generation = 0;
  std::uint64_t current_generation = 0;
  std::string training_reason = "not_required";
  std::string retraining_reason = "not_required";
  bool stale_dictionary_fallback = true;
};

struct CompressionFamilyThreshold {
  CompressionFamily family = CompressionFamily::kRowPage;
  std::uint64_t minimum_payload_bytes = 0;
  double minimum_margin = 0.0;
  double hot_update_extra_margin = 0.0;
  bool refuse_hot_updates = false;
  bool refuse_random_access_when_decompress_dominates = false;
  bool refuse_vector_rerank_when_decompress_dominates = false;
  bool dictionary_required = false;
  bool dictionary_reuse_required = false;
  bool index_runtime_unproven = false;
};

struct CompressionPolicyRequest {
  CompressionFamily family = CompressionFamily::kRowPage;
  CompressionCostDimensions cost;
  CompressionMeasuredRuntimeFeedback measured_feedback;
  CompressionDictionaryLifecycle dictionary;
  bool exact_uncompressed_fallback_available = true;
  bool exact_semantic_equivalence_proven = true;
  bool exact_binary_equivalence_proven = true;
  bool parser_or_reference_authority = false;
  bool wal_or_finality_authority = false;
  bool update_hot = false;
  bool random_access = false;
  bool vector_rerank = false;
  bool runtime_index_compression_requested = false;
  bool index_runtime_correctness_proven = false;
  bool benchmark_clean = true;
  std::uint64_t uncompressed_bytes = 0;
  std::uint64_t estimated_compressed_bytes = 0;
};

struct CompressionPolicyDecision {
  CompressionFamily family = CompressionFamily::kRowPage;
  CompressionEligibility eligibility = CompressionEligibility::kRefused;
  CompressionMethod method = CompressionMethod::kNone;
  CompressionCostDimensions cost;
  CompressionCostSource cost_source = CompressionCostSource::kStaticEstimate;
  CompressionMeasuredRuntimeFeedback measured_feedback;
  bool measured_feedback_quality_sufficient = false;
  CompressionFamilyThreshold threshold;
  CompressionDictionaryLifecycle dictionary;
  bool accepted = false;
  bool fallback = false;
  bool index_runtime_closure_claimed = false;
  bool exact_uncompressed_fallback_available = false;
  bool exact_semantic_equivalence_proven = false;
  bool exact_binary_equivalence_proven = false;
  bool parser_or_reference_authority = false;
  bool wal_or_finality_authority = false;
  bool update_hot = false;
  bool random_access = false;
  bool vector_rerank = false;
  bool runtime_index_compression_requested = false;
  bool index_runtime_correctness_proven = false;
  bool benchmark_clean = true;
  std::uint64_t uncompressed_bytes = 0;
  std::uint64_t estimated_compressed_bytes = 0;
  std::int64_t byte_delta = 0;
  double effective_compression_ratio = 1.0;
  double cost_benefit_score = 0.0;
  double cost_penalty_score = 0.0;
  double required_margin = 0.0;
  std::vector<std::string> diagnostics;
  std::vector<std::string> evidence;
};

const std::vector<CompressionFamily>& RequiredCompressionFamilies();
const char* CompressionFamilyName(CompressionFamily family);
const char* CompressionEligibilityName(CompressionEligibility eligibility);
const char* CompressionMethodName(CompressionMethod method);
const char* CompressionCostSourceName(CompressionCostSource source);
CompressionFamilyThreshold ThresholdForCompressionFamily(
    CompressionFamily family);

CompressionPolicyRequest DefaultCompressionPolicyRequest(
    CompressionFamily family);
CompressionPolicyDecision EvaluateCompressionPolicy(
    const CompressionPolicyRequest& request);

}  // namespace scratchbird::core::index
