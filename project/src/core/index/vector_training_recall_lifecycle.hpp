// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// ODFR_VECTOR_TRAINING_RECALL_LIFECYCLE
#include "index_spatial_vector_graph_access.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::core::index {

constexpr const char* kVectorTrainingRecallLifecycleSearchKey =
    "ODFR_VECTOR_TRAINING_RECALL_LIFECYCLE";

enum class VectorTrainingRecallLifecycleAction {
  kKeepCurrentGeneration,
  kScheduleAdaptiveTuning,
  kScheduleRetrain,
  kScheduleRebuild,
  kExactFallback,
  kRefuse
};

struct VectorTrainingSetSelectionProfile {
  std::uint64_t training_generation_id = 0;
  std::uint64_t sample_rows = 0;
  std::uint64_t corpus_rows = 0;
  std::uint64_t deterministic_seed = 0;
  std::string sample_fingerprint;
  bool deterministic = true;
};

struct VectorIvfTrainingProfile {
  std::uint32_t centroid_count = 0;
  std::uint32_t list_count = 0;
  double list_imbalance_ratio = 1.0;
  double max_list_imbalance_ratio = 2.0;
};

struct VectorPqTrainingProfile {
  std::uint32_t codebook_count = 0;
  std::uint32_t subvector_count = 0;
  std::uint32_t bits_per_code = 0;
  double quantization_error = 0.0;
  double max_quantization_error = 0.10;
};

struct VectorHnswTrainingProfile {
  std::uint32_t m = 0;
  std::uint32_t ef_construction = 0;
  std::uint32_t ef_search = 0;
};

struct VectorRecallProbeProfile {
  std::uint32_t probe_set_size = 0;
  std::uint32_t top_k = 0;
  double required_recall = 1.0;
  double observed_ann_recall = 1.0;
  bool exact_fallback_sample_comparison_present = true;
  bool exact_rerank_required = true;
  bool exact_rerank_final_scoring_authority = true;
};

struct VectorRecallDriftInputs {
  std::uint64_t deleted_vector_count = 0;
  std::uint64_t live_vector_count = 0;
  double tombstone_ratio = 0.0;
  double max_tombstone_ratio = 0.20;
  std::uint64_t hnsw_graph_age_generations = 0;
  std::uint64_t max_hnsw_graph_age_generations = 8;
  double hnsw_degree_imbalance_ratio = 1.0;
  double max_hnsw_degree_imbalance_ratio = 1.35;
  std::uint64_t p95_latency_microseconds = 0;
  std::uint64_t policy_p95_latency_microseconds = 0;
  double centroid_list_imbalance_drift = 0.0;
  double max_centroid_list_imbalance_drift = 0.25;
  double quantization_error_drift = 0.0;
  double max_quantization_error_drift = 0.05;
  double observed_recall_drift = 0.0;
  double max_observed_recall_drift = 0.03;
  std::uint64_t corpus_generation_drift = 0;
  std::uint64_t max_corpus_generation_drift = 0;
  bool adaptive_tuning_allowed = true;
  bool adaptive_tuning_attempted_before_rebuild = true;
  bool adaptive_tuning_expected_sufficient = false;
  std::uint32_t current_ef_search = 0;
  std::uint32_t tuned_ef_search = 0;
  std::uint32_t max_ef_search = 0;
  std::uint32_t current_nprobe = 0;
  std::uint32_t tuned_nprobe = 0;
  std::uint32_t max_nprobe = 0;
};

struct VectorTrainingRecallLifecycleProfile {
  IndexVectorAlgorithm algorithm = IndexVectorAlgorithm::hnsw;
  bool hnsw_profile_present = false;
  bool ivf_profile_present = false;
  bool pq_profile_present = false;
  bool exact_fallback_available = true;
  bool base_row_mga_recheck_required = true;
  bool base_row_security_recheck_required = true;
  bool parser_or_donor_authority = false;
  bool write_ahead_or_finality_authority = false;
  bool vector_metadata_visibility_authority = false;
  bool vector_metadata_finality_authority = false;
  bool benchmark_clean = true;
  VectorTrainingSetSelectionProfile training_set;
  VectorIvfTrainingProfile ivf;
  VectorPqTrainingProfile pq;
  VectorHnswTrainingProfile hnsw;
  VectorRecallProbeProfile recall_probe;
  VectorRecallDriftInputs drift;
};

struct VectorTrainingRecallLifecycleDecision {
  bool accepted = false;
  bool fail_closed = true;
  VectorTrainingRecallLifecycleAction action =
      VectorTrainingRecallLifecycleAction::kRefuse;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::vector<std::string> evidence;
};

const char* VectorTrainingRecallLifecycleActionName(
    VectorTrainingRecallLifecycleAction action);

VectorTrainingRecallLifecycleProfile DefaultVectorTrainingRecallLifecycleProfile(
    IndexVectorAlgorithm algorithm);
VectorTrainingRecallLifecycleDecision EvaluateVectorTrainingRecallLifecycle(
    const VectorTrainingRecallLifecycleProfile& profile);
std::vector<std::string> VectorTrainingRecallLifecycleEvidence(
    const VectorTrainingRecallLifecycleDecision& decision);

}  // namespace scratchbird::core::index
