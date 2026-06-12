// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB_VECTOR_IVF_PQ_PHYSICAL_PROVIDER
#include "vector_exact_physical_provider.hpp"

#include <functional>
#include <string>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kVectorIvfPqPhysicalProviderSearchKey =
    "SB_VECTOR_IVF_PQ_PHYSICAL_PROVIDER";
inline constexpr const char* kVectorIvfPqPhysicalProviderArtifactKind =
    "vector_ivf_pq_physical_provider";
inline constexpr u32 kVectorIvfPqPhysicalProviderCurrentMajor = 1;
inline constexpr u32 kVectorIvfPqPhysicalProviderCurrentMinor = 0;

enum class VectorIvfPqOpenClass : u32 {
  current = 1,
  stale_format = 2,
  stale_generation = 3,
  bad_checksum = 4,
  corrupt_payload = 5,
  identity_mismatch = 6,
  stale_descriptor_epoch = 7,
  stale_metric_epoch = 8,
  dimension_mismatch = 9,
  unsupported_profile = 10,
  unsafe_metric_resource = 11,
  missing_exact_recheck_proof = 12,
  authority_claim_refused = 13,
  invalid_centroid = 14,
  invalid_list = 15,
  invalid_codebook = 16,
  invalid_code = 17,
  duplicate_row_locator = 18,
  refused = 19
};

enum class VectorIvfPqCompression : u32 {
  ivf_flat = 1,
  sq8 = 2,
  pq = 3
};

enum class VectorIvfPqMutationKind : u32 {
  insert_row = 1,
  delete_row = 2,
  update_row = 3
};

using VectorIvfPqDescriptor = VectorExactDescriptor;
using VectorIvfPqMetricResource = VectorExactMetricResource;
using VectorIvfPqRecheckProof = VectorExactRecheckProof;
using VectorIvfPqSourceRow = VectorExactSourceRow;

struct VectorIvfPqBuildProfile {
  VectorIvfPqCompression compression = VectorIvfPqCompression::ivf_flat;
  u32 centroid_count = 4;
  u32 nprobe = 2;
  u32 training_iterations = 6;
  u32 max_training_rows = 4096;
  u32 pq_subspaces = 2;
  u32 pq_codewords = 4;
  double retrain_imbalance_ratio = 2.0;
  double rebuild_tombstone_ratio = 0.25;
  bool deterministic_training = true;
  bool scalar_kernel_present = true;
};

struct VectorIvfPqCentroid {
  u32 centroid_id = 0;
  std::vector<float> vector;
  u64 assigned_count = 0;
  double residual_error_sum = 0.0;
};

struct VectorIvfPqSq8Axis {
  float min_value = 0.0F;
  float scale = 1.0F;
};

struct VectorIvfPqCodebook {
  u32 subspace_id = 0;
  u32 offset = 0;
  u32 width = 0;
  std::vector<std::vector<float>> centroids;
};

struct VectorIvfPqStoredVector {
  TextInvertedRowLocator locator;
  u32 list_id = 0;
  std::vector<byte> exact_payload;
  std::vector<byte> compressed_code;
  bool tombstoned = false;
  u64 insert_generation = 0;
  u64 delete_generation = 0;
};

struct VectorIvfPqList {
  u32 list_id = 0;
  u32 centroid_id = 0;
  std::vector<VectorIvfPqStoredVector> entries;
  u64 live_count = 0;
  u64 tombstone_count = 0;
  double residual_error_sum = 0.0;
};

struct VectorIvfPqPhysicalProvider {
  std::string artifact_kind = kVectorIvfPqPhysicalProviderArtifactKind;
  PageExtentSummaryFormatVersion format_version{
      kVectorIvfPqPhysicalProviderCurrentMajor,
      kVectorIvfPqPhysicalProviderCurrentMinor};
  std::string relation_uuid;
  std::string index_uuid;
  std::string provider_uuid;
  u64 base_generation = 0;
  u64 provider_generation = 0;
  u64 training_generation = 0;
  VectorIvfPqDescriptor descriptor;
  VectorIvfPqMetricResource metric;
  VectorIvfPqBuildProfile profile;
  bool centroid_training_present = true;
  bool list_assignment_present = true;
  bool nprobe_planner_present = true;
  bool ivf_list_storage_present = true;
  bool vector_payload_storage_present = true;
  bool compressed_code_storage_present = true;
  bool sq8_codec_present = true;
  bool pq_codec_present = true;
  bool exact_rerank_present = true;
  bool metadata_prefilter_present = true;
  bool candidate_set_input_present = true;
  bool tombstones_present = true;
  bool generation_evidence_present = true;
  bool telemetry_present = true;
  bool candidate_evidence_only = true;
  bool exact_source_recheck_required = true;
  bool exact_rerank_proof_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  bool contract_only_fallback = false;
  bool provider_only_fallback = false;
  bool visibility_authority_claimed = false;
  bool security_authority_claimed = false;
  bool transaction_finality_authority_claimed = false;
  bool parser_finality_authority_claimed = false;
  bool reference_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool index_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
  u64 live_vector_count = 0;
  u64 tombstone_count = 0;
  u64 mutation_generation_evidence = 0;
  u64 training_generation_evidence = 0;
  double list_imbalance_ratio = 0.0;
  double residual_error_mean = 0.0;
  double compression_error_mean = 0.0;
  bool retrain_recommended = false;
  bool rebuild_recommended = false;
  u64 last_query_selected_lists = 0;
  u64 last_query_candidate_count = 0;
  u64 last_query_exact_rerank_count = 0;
  u64 last_query_latency_units = 0;
  double last_query_recall_floor = 1.0;
  std::vector<VectorIvfPqCentroid> centroids;
  std::vector<VectorIvfPqSq8Axis> sq8_axes;
  std::vector<VectorIvfPqCodebook> pq_codebooks;
  std::vector<VectorIvfPqList> lists;
  std::vector<std::string> evidence;
};

struct VectorIvfPqBuildRequest {
  std::string relation_uuid;
  std::string index_uuid;
  std::string provider_uuid;
  u64 base_generation = 0;
  u64 provider_generation = 0;
  u64 training_generation = 0;
  VectorIvfPqDescriptor descriptor;
  VectorIvfPqMetricResource metric;
  VectorIvfPqBuildProfile profile;
  VectorIvfPqRecheckProof recheck_proof;
  std::vector<VectorIvfPqSourceRow> rows;
};

struct VectorIvfPqBuildResult {
  Status status;
  DiagnosticRecord diagnostic;
  VectorIvfPqPhysicalProvider provider;
  bool built = false;
  bool fail_closed = true;

  bool ok() const { return status.ok() && built && !fail_closed; }
};

struct VectorIvfPqSerializeResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<byte> bytes;
  u64 checksum = 0;

  bool ok() const { return status.ok() && !bytes.empty(); }
};

struct VectorIvfPqOpenRequest {
  std::vector<byte> bytes;
  bool expected_relation_uuid_present = false;
  std::string expected_relation_uuid;
  bool expected_index_uuid_present = false;
  std::string expected_index_uuid;
  bool expected_provider_uuid_present = false;
  std::string expected_provider_uuid;
  bool expected_base_generation_present = false;
  u64 expected_base_generation = 0;
  bool expected_provider_generation_present = false;
  u64 expected_provider_generation = 0;
  bool expected_training_generation_present = false;
  u64 expected_training_generation = 0;
  bool expected_descriptor_epoch_present = false;
  u64 expected_descriptor_epoch = 0;
  bool expected_metric_resource_epoch_present = false;
  u64 expected_metric_resource_epoch = 0;
  bool expected_dimensions_present = false;
  u32 expected_dimensions = 0;
  VectorIvfPqRecheckProof recheck_proof;
};

struct VectorIvfPqOpenResult {
  Status status;
  DiagnosticRecord diagnostic;
  VectorIvfPqOpenClass open_class = VectorIvfPqOpenClass::refused;
  VectorIvfPqPhysicalProvider provider;
  bool fail_closed = true;
  bool restricted_repair_required = false;
  std::vector<std::string> actions;

  bool ok() const {
    return status.ok() && open_class == VectorIvfPqOpenClass::current &&
           !fail_closed;
  }
};

struct VectorIvfPqQuery {
  std::vector<float> vector;
  u32 top_k = 0;
  u32 nprobe = 0;
  std::vector<TextInvertedRowLocator> candidate_set;
  std::function<bool(const TextInvertedRowLocator&)> metadata_prefilter;
};

struct VectorIvfPqQueryRequest {
  VectorIvfPqPhysicalProvider provider;
  std::vector<VectorIvfPqQuery> queries;
  VectorIvfPqRecheckProof recheck_proof;
  bool descriptor_epoch_current = true;
  bool metric_resource_epoch_current = true;
  bool training_generation_current = true;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  bool contract_only_fallback = false;
  bool provider_only_fallback = false;
};

struct VectorIvfPqCandidate {
  TextInvertedRowLocator locator;
  u32 list_id = 0;
  double approximate_score = 0.0;
  double exact_score = 0.0;
  bool lower_score_better = true;
  bool reached_by_nprobe = false;
  bool compressed_code_scored = false;
  bool decoded_from_physical_payload = false;
  bool exact_payload_reranked = false;
  bool metadata_prefilter_passed = true;
  bool candidate_set_member = true;
  bool exact_rerank_proof_verified = false;
  bool exact_source_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool final_row_admitted = false;
  std::string source_recheck_evidence_ref;
};

struct VectorIvfPqSingleQueryResult {
  bool ok = false;
  std::vector<u32> selected_list_ids;
  std::vector<VectorIvfPqCandidate> candidates;
  u64 candidates_considered = 0;
  u64 candidates_filtered = 0;
  u64 compressed_decode_count = 0;
  u64 exact_rerank_count = 0;
  u64 scalar_kernel_consumed_count = 0;
  bool nprobe_planner_used = false;
  bool exact_rerank_performed = false;
  bool candidate_rows_only = true;
  bool final_rows_authorized = false;
};

struct VectorIvfPqQueryResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = true;
  std::vector<VectorIvfPqSingleQueryResult> batch_results;
  bool nprobe_planner_used = false;
  bool compressed_code_search_used = false;
  bool metadata_prefilter_consumed = false;
  bool candidate_set_consumed = false;
  bool scalar_kernel_consumed = false;
  bool exact_rerank_performed = false;
  bool candidate_rows_only = true;
  bool final_rows_authorized = false;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  bool contract_only_fallback = false;
  bool provider_only_fallback = false;
  bool exact_source_recheck_required = true;
  bool exact_rerank_proof_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  VectorIvfPqPhysicalProvider provider_after_telemetry;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct VectorIvfPqMutation {
  VectorIvfPqMutationKind kind = VectorIvfPqMutationKind::insert_row;
  bool expected_provider_generation_present = false;
  u64 expected_provider_generation = 0;
  bool expected_training_generation_present = false;
  u64 expected_training_generation = 0;
  bool expected_descriptor_epoch_present = false;
  u64 expected_descriptor_epoch = 0;
  bool expected_metric_resource_epoch_present = false;
  u64 expected_metric_resource_epoch = 0;
  bool before_row_present = false;
  VectorIvfPqSourceRow before_row;
  bool after_row_present = false;
  VectorIvfPqSourceRow after_row;
  VectorIvfPqRecheckProof recheck_proof;
};

struct VectorIvfPqMutationResult {
  Status status;
  DiagnosticRecord diagnostic;
  VectorIvfPqPhysicalProvider provider;
  bool applied = false;
  bool fail_closed = true;
  bool tombstone_recorded = false;
  bool list_assignment_recomputed = false;
  bool retrain_recommended = false;
  bool rebuild_recommended = false;
  std::vector<std::string> actions;

  bool ok() const { return status.ok() && applied && !fail_closed; }
};

VectorIvfPqBuildResult BuildVectorIvfPqPhysicalProvider(
    const VectorIvfPqBuildRequest& request);
VectorIvfPqSerializeResult SerializeVectorIvfPqPhysicalProvider(
    const VectorIvfPqPhysicalProvider& provider);
VectorIvfPqOpenResult OpenVectorIvfPqPhysicalProvider(
    const VectorIvfPqOpenRequest& request);
VectorIvfPqQueryResult QueryVectorIvfPqPhysicalProvider(
    const VectorIvfPqQueryRequest& request);
VectorIvfPqMutationResult ApplyVectorIvfPqPhysicalMutation(
    const VectorIvfPqPhysicalProvider& provider,
    const VectorIvfPqMutation& mutation);

const char* VectorIvfPqOpenClassName(VectorIvfPqOpenClass open_class);
const char* VectorIvfPqCompressionName(VectorIvfPqCompression compression);
DiagnosticRecord MakeVectorIvfPqPhysicalProviderDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
