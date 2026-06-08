// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "vector_training_recall_lifecycle.hpp"

#include <string>
#include <utility>

namespace scratchbird::core::index {
namespace {

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

void AddBool(std::vector<std::string>* evidence,
             const std::string& key,
             bool value) {
  Add(evidence, key + "=" + (value ? "true" : "false"));
}

bool RequiresIvf(IndexVectorAlgorithm algorithm) {
  return algorithm == IndexVectorAlgorithm::ivf_flat ||
         algorithm == IndexVectorAlgorithm::ivf_pq ||
         algorithm == IndexVectorAlgorithm::ivf_sq8;
}

bool RequiresPq(IndexVectorAlgorithm algorithm) {
  return algorithm == IndexVectorAlgorithm::ivf_pq ||
         algorithm == IndexVectorAlgorithm::rhnsw_quantized;
}

bool RequiresHnsw(IndexVectorAlgorithm algorithm) {
  return algorithm == IndexVectorAlgorithm::hnsw ||
         algorithm == IndexVectorAlgorithm::rhnsw_quantized;
}

double EffectiveTombstoneRatio(const VectorRecallDriftInputs& drift) {
  if (drift.tombstone_ratio > 0.0) {
    return drift.tombstone_ratio;
  }
  const std::uint64_t total_vectors =
      drift.deleted_vector_count + drift.live_vector_count;
  if (total_vectors == 0) {
    return 0.0;
  }
  return static_cast<double>(drift.deleted_vector_count) /
         static_cast<double>(total_vectors);
}

VectorTrainingRecallLifecycleDecision BaseDecision(
    const VectorTrainingRecallLifecycleProfile& profile) {
  VectorTrainingRecallLifecycleDecision decision;
  const double effective_tombstone_ratio =
      EffectiveTombstoneRatio(profile.drift);
  Add(&decision.evidence, kVectorTrainingRecallLifecycleSearchKey);
  Add(&decision.evidence, std::string("vector_algorithm=") +
                              IndexVectorAlgorithmName(profile.algorithm));
  Add(&decision.evidence, "training_generation_id=" +
                              std::to_string(
                                  profile.training_set.training_generation_id));
  Add(&decision.evidence,
      "training_sample_rows=" +
          std::to_string(profile.training_set.sample_rows));
  Add(&decision.evidence,
      "training_corpus_rows=" +
          std::to_string(profile.training_set.corpus_rows));
  Add(&decision.evidence,
      "training_seed=" +
          std::to_string(profile.training_set.deterministic_seed));
  Add(&decision.evidence,
      "training_sample_fingerprint=" +
          profile.training_set.sample_fingerprint);
  AddBool(&decision.evidence,
          "training_set_selection_deterministic",
          profile.training_set.deterministic);
  AddBool(&decision.evidence, "hnsw_profile_present",
          profile.hnsw_profile_present);
  Add(&decision.evidence, "hnsw_m=" + std::to_string(profile.hnsw.m));
  Add(&decision.evidence, "hnsw_ef_construction=" +
                              std::to_string(profile.hnsw.ef_construction));
  Add(&decision.evidence,
      "hnsw_ef_search=" + std::to_string(profile.hnsw.ef_search));
  AddBool(&decision.evidence, "ivf_profile_present",
          profile.ivf_profile_present);
  Add(&decision.evidence,
      "ivf_centroid_count=" + std::to_string(profile.ivf.centroid_count));
  Add(&decision.evidence,
      "ivf_list_count=" + std::to_string(profile.ivf.list_count));
  Add(&decision.evidence, "ivf_list_imbalance_ratio=" +
                              std::to_string(profile.ivf.list_imbalance_ratio));
  AddBool(&decision.evidence, "pq_profile_present",
          profile.pq_profile_present);
  Add(&decision.evidence,
      "pq_codebook_count=" + std::to_string(profile.pq.codebook_count));
  Add(&decision.evidence,
      "pq_subvector_count=" + std::to_string(profile.pq.subvector_count));
  Add(&decision.evidence,
      "pq_bits_per_code=" + std::to_string(profile.pq.bits_per_code));
  Add(&decision.evidence, "pq_quantization_error=" +
                              std::to_string(profile.pq.quantization_error));
  Add(&decision.evidence, "recall_probe_set_size=" +
                              std::to_string(profile.recall_probe.probe_set_size));
  Add(&decision.evidence,
      "recall_top_k=" + std::to_string(profile.recall_probe.top_k));
  Add(&decision.evidence, "recall_required=" +
                              std::to_string(profile.recall_probe.required_recall));
  Add(&decision.evidence,
      "recall_observed_ann=" +
          std::to_string(profile.recall_probe.observed_ann_recall));
  AddBool(&decision.evidence,
          "exact_fallback_sample_comparison_present",
          profile.recall_probe.exact_fallback_sample_comparison_present);
  AddBool(&decision.evidence,
          "exact_rerank_required",
          profile.recall_probe.exact_rerank_required);
  AddBool(&decision.evidence,
          "exact_rerank_final_scoring_authority",
          profile.recall_probe.exact_rerank_final_scoring_authority);
  AddBool(&decision.evidence,
          "exact_fallback_available",
          profile.exact_fallback_available);
  Add(&decision.evidence,
      "deleted_vector_count=" +
          std::to_string(profile.drift.deleted_vector_count));
  Add(&decision.evidence,
      "live_vector_count=" +
          std::to_string(profile.drift.live_vector_count));
  Add(&decision.evidence,
      "tombstone_ratio=" + std::to_string(profile.drift.tombstone_ratio));
  Add(&decision.evidence,
      "effective_tombstone_ratio=" +
          std::to_string(effective_tombstone_ratio));
  Add(&decision.evidence,
      "tombstone_ratio_threshold=" +
          std::to_string(profile.drift.max_tombstone_ratio));
  Add(&decision.evidence,
      "hnsw_graph_age_generations=" +
          std::to_string(profile.drift.hnsw_graph_age_generations));
  Add(&decision.evidence,
      "hnsw_graph_age_threshold_generations=" +
          std::to_string(profile.drift.max_hnsw_graph_age_generations));
  Add(&decision.evidence,
      "hnsw_degree_imbalance_ratio=" +
          std::to_string(profile.drift.hnsw_degree_imbalance_ratio));
  Add(&decision.evidence,
      "hnsw_degree_imbalance_threshold=" +
          std::to_string(profile.drift.max_hnsw_degree_imbalance_ratio));
  Add(&decision.evidence,
      "vector_p95_latency_microseconds=" +
          std::to_string(profile.drift.p95_latency_microseconds));
  Add(&decision.evidence,
      "vector_policy_p95_latency_microseconds=" +
          std::to_string(profile.drift.policy_p95_latency_microseconds));
  Add(&decision.evidence, "drift_centroid_list_imbalance=" +
                              std::to_string(
                                  profile.drift.centroid_list_imbalance_drift));
  Add(&decision.evidence, "drift_quantization_error=" +
                              std::to_string(
                                  profile.drift.quantization_error_drift));
  Add(&decision.evidence, "drift_observed_recall=" +
                              std::to_string(
                                  profile.drift.observed_recall_drift));
  Add(&decision.evidence,
      "drift_corpus_generation=" +
          std::to_string(profile.drift.corpus_generation_drift));
  AddBool(&decision.evidence,
          "adaptive_tuning_allowed",
          profile.drift.adaptive_tuning_allowed);
  AddBool(&decision.evidence,
          "adaptive_tuning_attempted_before_rebuild",
          profile.drift.adaptive_tuning_attempted_before_rebuild);
  AddBool(&decision.evidence,
          "adaptive_tuning_expected_sufficient",
          profile.drift.adaptive_tuning_expected_sufficient);
  Add(&decision.evidence,
      "adaptive_ef_search_current=" +
          std::to_string(profile.drift.current_ef_search));
  Add(&decision.evidence,
      "adaptive_ef_search_tuned=" +
          std::to_string(profile.drift.tuned_ef_search));
  Add(&decision.evidence,
      "adaptive_ef_search_max=" +
          std::to_string(profile.drift.max_ef_search));
  Add(&decision.evidence,
      "adaptive_nprobe_current=" +
          std::to_string(profile.drift.current_nprobe));
  Add(&decision.evidence,
      "adaptive_nprobe_tuned=" +
          std::to_string(profile.drift.tuned_nprobe));
  Add(&decision.evidence,
      "adaptive_nprobe_max=" +
          std::to_string(profile.drift.max_nprobe));
  AddBool(&decision.evidence,
          "base_row_mga_recheck_required",
          profile.base_row_mga_recheck_required);
  AddBool(&decision.evidence,
          "base_row_security_recheck_required",
          profile.base_row_security_recheck_required);
  AddBool(&decision.evidence,
          "vector_metadata_visibility_authority",
          profile.vector_metadata_visibility_authority);
  AddBool(&decision.evidence,
          "vector_metadata_finality_authority",
          profile.vector_metadata_finality_authority);
  AddBool(&decision.evidence,
          "parser_or_donor_authority",
          profile.parser_or_donor_authority);
  AddBool(&decision.evidence,
          "write_ahead_or_finality_authority",
          profile.write_ahead_or_finality_authority);
  AddBool(&decision.evidence, "benchmark_clean", profile.benchmark_clean);
  Add(&decision.evidence,
      "vector_runtime_correctness_blocker="
      "SB_ORH_VECTOR_INDEX_RUNTIME_CORRECTNESS_UNPROVEN");
  Add(&decision.evidence, "vector_maintenance_records_only=true");
  Add(&decision.evidence, "ann_state_visibility_authority=false");
  Add(&decision.evidence, "ann_state_finality_authority=false");
  Add(&decision.evidence, "support_bundle_ready=true");
  return decision;
}

VectorTrainingRecallLifecycleDecision Finish(
    VectorTrainingRecallLifecycleDecision decision,
    VectorTrainingRecallLifecycleAction action,
    std::string diagnostic_code,
    std::string diagnostic_detail,
    bool accepted,
    bool fail_closed) {
  decision.action = action;
  decision.diagnostic_code = std::move(diagnostic_code);
  decision.diagnostic_detail = std::move(diagnostic_detail);
  decision.accepted = accepted;
  decision.fail_closed = fail_closed;
  Add(&decision.evidence,
      "vector_lifecycle_action=" +
          std::string(VectorTrainingRecallLifecycleActionName(action)));
  Add(&decision.evidence,
      "vector_lifecycle_diagnostic=" + decision.diagnostic_code);
  if (!decision.diagnostic_detail.empty()) {
    Add(&decision.evidence,
        "vector_lifecycle_detail=" + decision.diagnostic_detail);
  }
  return decision;
}

VectorTrainingRecallLifecycleDecision ExactFallback(
    VectorTrainingRecallLifecycleDecision decision,
    std::string diagnostic_code,
    std::string diagnostic_detail) {
  Add(&decision.evidence, "exact_fallback_selected=true");
  Add(&decision.evidence, "ann_generation_not_used_as_authority=true");
  return Finish(std::move(decision),
                VectorTrainingRecallLifecycleAction::kExactFallback,
                std::move(diagnostic_code),
                std::move(diagnostic_detail),
                true,
                false);
}

VectorTrainingRecallLifecycleDecision Refuse(
    VectorTrainingRecallLifecycleDecision decision,
    std::string diagnostic_code,
    std::string diagnostic_detail) {
  return Finish(std::move(decision),
                VectorTrainingRecallLifecycleAction::kRefuse,
                std::move(diagnostic_code),
                std::move(diagnostic_detail),
                false,
                true);
}

}  // namespace

const char* VectorTrainingRecallLifecycleActionName(
    VectorTrainingRecallLifecycleAction action) {
  switch (action) {
    case VectorTrainingRecallLifecycleAction::kKeepCurrentGeneration:
      return "keep_current_generation";
    case VectorTrainingRecallLifecycleAction::kScheduleAdaptiveTuning:
      return "schedule_adaptive_tuning";
    case VectorTrainingRecallLifecycleAction::kScheduleRetrain:
      return "schedule_retrain";
    case VectorTrainingRecallLifecycleAction::kScheduleRebuild:
      return "schedule_rebuild";
    case VectorTrainingRecallLifecycleAction::kExactFallback:
      return "exact_fallback";
    case VectorTrainingRecallLifecycleAction::kRefuse:
      return "refuse";
  }
  return "refuse";
}

VectorTrainingRecallLifecycleProfile DefaultVectorTrainingRecallLifecycleProfile(
    IndexVectorAlgorithm algorithm) {
  VectorTrainingRecallLifecycleProfile profile;
  profile.algorithm = algorithm;
  profile.hnsw_profile_present = RequiresHnsw(algorithm);
  profile.ivf_profile_present = RequiresIvf(algorithm);
  profile.pq_profile_present = RequiresPq(algorithm);
  profile.exact_fallback_available = true;
  profile.base_row_mga_recheck_required = true;
  profile.base_row_security_recheck_required = true;
  profile.benchmark_clean = true;
  profile.training_set.training_generation_id = 40;
  profile.training_set.sample_rows = 2048;
  profile.training_set.corpus_rows = 8192;
  profile.training_set.deterministic_seed = 40040;
  profile.training_set.sample_fingerprint = "odfr040-deterministic-sample";
  profile.training_set.deterministic = true;
  profile.ivf.centroid_count = RequiresIvf(algorithm) ? 128 : 0;
  profile.ivf.list_count = RequiresIvf(algorithm) ? 128 : 0;
  profile.ivf.list_imbalance_ratio = 1.08;
  profile.ivf.max_list_imbalance_ratio = 2.0;
  profile.pq.codebook_count = RequiresPq(algorithm) ? 16 : 0;
  profile.pq.subvector_count = RequiresPq(algorithm) ? 8 : 0;
  profile.pq.bits_per_code = RequiresPq(algorithm) ? 8 : 0;
  profile.pq.quantization_error = 0.035;
  profile.pq.max_quantization_error = 0.10;
  profile.hnsw.m = RequiresHnsw(algorithm) ? 16 : 0;
  profile.hnsw.ef_construction = RequiresHnsw(algorithm) ? 200 : 0;
  profile.hnsw.ef_search = RequiresHnsw(algorithm) ? 80 : 0;
  profile.drift.p95_latency_microseconds = 1200;
  profile.drift.policy_p95_latency_microseconds = 2000;
  profile.drift.current_ef_search = profile.hnsw.ef_search;
  profile.drift.tuned_ef_search = profile.hnsw.ef_search;
  profile.drift.max_ef_search = RequiresHnsw(algorithm) ? 256 : 0;
  profile.drift.current_nprobe = RequiresIvf(algorithm) ? 8 : 0;
  profile.drift.tuned_nprobe = RequiresIvf(algorithm) ? 8 : 0;
  profile.drift.max_nprobe = RequiresIvf(algorithm) ? 64 : 0;
  profile.recall_probe.probe_set_size = 256;
  profile.recall_probe.top_k = 10;
  profile.recall_probe.required_recall = 0.95;
  profile.recall_probe.observed_ann_recall = 0.982;
  profile.recall_probe.exact_fallback_sample_comparison_present = true;
  profile.recall_probe.exact_rerank_required = true;
  profile.recall_probe.exact_rerank_final_scoring_authority = true;
  return profile;
}

VectorTrainingRecallLifecycleDecision EvaluateVectorTrainingRecallLifecycle(
    const VectorTrainingRecallLifecycleProfile& profile) {
  auto decision = BaseDecision(profile);
  if (profile.parser_or_donor_authority) {
    return Refuse(std::move(decision),
                  "INDEX.VECTOR_RECALL_LIFECYCLE.UNSAFE_PARSER_OR_DONOR_AUTHORITY",
                  "parser_or_donor_authority_forbidden");
  }
  if (profile.write_ahead_or_finality_authority) {
    return Refuse(std::move(decision),
                  "INDEX.VECTOR_RECALL_LIFECYCLE.UNSAFE_FINALITY_AUTHORITY",
                  "write_ahead_or_finality_authority_forbidden");
  }
  if (profile.vector_metadata_visibility_authority ||
      profile.vector_metadata_finality_authority) {
    return Refuse(std::move(decision),
                  "INDEX.VECTOR_RECALL_LIFECYCLE.METADATA_AUTHORITY_REFUSED",
                  "vector_metadata_cannot_be_visibility_or_finality_authority");
  }
  if (!profile.base_row_mga_recheck_required ||
      !profile.base_row_security_recheck_required) {
    return Refuse(std::move(decision),
                  "INDEX.VECTOR_RECALL_LIFECYCLE.BASE_ROW_RECHECK_REQUIRED",
                  "base_row_mga_and_security_rechecks_required");
  }
  if (!profile.exact_fallback_available ||
      !profile.recall_probe.exact_fallback_sample_comparison_present) {
    return Refuse(std::move(decision),
                  "INDEX.VECTOR_RECALL_LIFECYCLE.EXACT_FALLBACK_REQUIRED",
                  "exact_fallback_sample_comparison_required");
  }
  if (profile.recall_probe.exact_rerank_required &&
      !profile.recall_probe.exact_rerank_final_scoring_authority) {
    return Refuse(std::move(decision),
                  "INDEX.VECTOR_RECALL_LIFECYCLE.EXACT_RERANK_REQUIRED",
                  "exact_rerank_final_scoring_authority_required");
  }
  if (!profile.training_set.deterministic ||
      profile.training_set.training_generation_id == 0 ||
      profile.training_set.sample_rows == 0 ||
      profile.training_set.corpus_rows == 0 ||
      profile.training_set.sample_fingerprint.empty()) {
    return Refuse(std::move(decision),
                  "INDEX.VECTOR_RECALL_LIFECYCLE.TRAINING_SET_REFUSED",
                  "deterministic_training_set_selection_required");
  }
  if (RequiresHnsw(profile.algorithm) &&
      (!profile.hnsw_profile_present || profile.hnsw.m == 0 ||
       profile.hnsw.ef_construction == 0 || profile.hnsw.ef_search == 0)) {
    return ExactFallback(
        std::move(decision),
        "INDEX.VECTOR_RECALL_LIFECYCLE.HNSW_PROFILE_EXACT_FALLBACK",
        "hnsw_profile_incomplete_exact_fallback_selected");
  }
  if (RequiresIvf(profile.algorithm) &&
      (!profile.ivf_profile_present || profile.ivf.centroid_count == 0 ||
       profile.ivf.list_count == 0)) {
    return ExactFallback(
        std::move(decision),
        "INDEX.VECTOR_RECALL_LIFECYCLE.IVF_PROFILE_EXACT_FALLBACK",
        "ivf_centroid_or_list_profile_incomplete_exact_fallback_selected");
  }
  if (RequiresPq(profile.algorithm) &&
      (!profile.pq_profile_present || profile.pq.codebook_count == 0 ||
       profile.pq.subvector_count == 0 || profile.pq.bits_per_code == 0)) {
    return ExactFallback(
        std::move(decision),
        "INDEX.VECTOR_RECALL_LIFECYCLE.PQ_PROFILE_EXACT_FALLBACK",
        "pq_codebook_subvector_or_bits_incomplete_exact_fallback_selected");
  }
  if (profile.recall_probe.probe_set_size == 0 ||
      profile.recall_probe.top_k == 0 ||
      profile.recall_probe.required_recall <= 0.0 ||
      profile.recall_probe.required_recall > 1.0 ||
      profile.recall_probe.observed_ann_recall < 0.0 ||
      profile.recall_probe.observed_ann_recall > 1.0) {
    return Refuse(std::move(decision),
                  "INDEX.VECTOR_RECALL_LIFECYCLE.RECALL_PROBE_REFUSED",
                  "valid_recall_probe_set_top_k_and_recall_bounds_required");
  }
  const bool latency_over_policy =
      profile.drift.policy_p95_latency_microseconds != 0 &&
      profile.drift.p95_latency_microseconds >
          profile.drift.policy_p95_latency_microseconds;
  const bool tombstone_over_threshold =
      EffectiveTombstoneRatio(profile.drift) >
      profile.drift.max_tombstone_ratio;
  const bool graph_age_over_threshold =
      profile.drift.hnsw_graph_age_generations >
      profile.drift.max_hnsw_graph_age_generations;
  const bool degree_over_threshold =
      profile.drift.hnsw_degree_imbalance_ratio >
      profile.drift.max_hnsw_degree_imbalance_ratio;
  const bool recall_drift_retrain =
      profile.recall_probe.observed_ann_recall <
          profile.recall_probe.required_recall ||
      profile.drift.observed_recall_drift >
          profile.drift.max_observed_recall_drift;
  const bool quantization_drift_retrain =
      profile.pq.quantization_error > profile.pq.max_quantization_error ||
      profile.drift.quantization_error_drift >
          profile.drift.max_quantization_error_drift;
  const bool adaptive_knob_has_room =
      (profile.drift.tuned_ef_search > profile.drift.current_ef_search &&
       profile.drift.tuned_ef_search <= profile.drift.max_ef_search) ||
      (profile.drift.tuned_nprobe > profile.drift.current_nprobe &&
       profile.drift.tuned_nprobe <= profile.drift.max_nprobe);
  if (recall_drift_retrain) {
    return Finish(std::move(decision),
                  VectorTrainingRecallLifecycleAction::kScheduleRetrain,
                  "INDEX.VECTOR_RECALL_LIFECYCLE.RECALL_DRIFT_RETRAIN",
                  "observed_recall_below_required_or_drift_threshold",
                  true,
                  false);
  }
  if (quantization_drift_retrain) {
    return Finish(std::move(decision),
                  VectorTrainingRecallLifecycleAction::kScheduleRetrain,
                  "INDEX.VECTOR_RECALL_LIFECYCLE.QUANTIZATION_DRIFT_RETRAIN",
                  "quantization_error_or_drift_threshold_exceeded",
                  true,
                  false);
  }
  if ((latency_over_policy || tombstone_over_threshold ||
       degree_over_threshold) &&
      profile.drift.adaptive_tuning_allowed &&
      profile.drift.adaptive_tuning_attempted_before_rebuild &&
      profile.drift.adaptive_tuning_expected_sufficient &&
      adaptive_knob_has_room) {
    return Finish(std::move(decision),
                  VectorTrainingRecallLifecycleAction::
                      kScheduleAdaptiveTuning,
                  "INDEX.VECTOR_RECALL_LIFECYCLE.ADAPTIVE_TUNING",
                  "adaptive_ef_search_or_nprobe_before_rebuild",
                  true,
                  false);
  }
  if (profile.ivf.list_imbalance_ratio > profile.ivf.max_list_imbalance_ratio ||
      tombstone_over_threshold ||
      graph_age_over_threshold ||
      degree_over_threshold ||
      latency_over_policy ||
      profile.drift.centroid_list_imbalance_drift >
          profile.drift.max_centroid_list_imbalance_drift ||
      profile.drift.corpus_generation_drift >
          profile.drift.max_corpus_generation_drift) {
    return Finish(std::move(decision),
                  VectorTrainingRecallLifecycleAction::kScheduleRebuild,
                  "INDEX.VECTOR_RECALL_LIFECYCLE.STRUCTURE_DRIFT_REBUILD",
                  "centroid_list_imbalance_or_corpus_generation_drift",
                  true,
                  false);
  }
  return Finish(std::move(decision),
                VectorTrainingRecallLifecycleAction::kKeepCurrentGeneration,
                "INDEX.VECTOR_RECALL_LIFECYCLE.KEEP_CURRENT_GENERATION",
                "recall_profile_within_thresholds",
                true,
                false);
}

std::vector<std::string> VectorTrainingRecallLifecycleEvidence(
    const VectorTrainingRecallLifecycleDecision& decision) {
  return decision.evidence;
}

}  // namespace scratchbird::core::index
