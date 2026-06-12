// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB_VECTOR_EXACT_PHYSICAL_PROVIDER
#include "text_inverted_segment.hpp"

#include <functional>
#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::i32;
using scratchbird::core::platform::u16;

inline constexpr const char* kVectorExactPhysicalProviderSearchKey =
    "SB_VECTOR_EXACT_PHYSICAL_PROVIDER";
inline constexpr const char* kVectorExactPhysicalProviderArtifactKind =
    "vector_exact_physical_provider";
inline constexpr u32 kVectorExactPhysicalProviderCurrentMajor = 1;
inline constexpr u32 kVectorExactPhysicalProviderCurrentMinor = 0;

enum class VectorExactOpenClass : u32 {
  current = 1,
  stale_format = 2,
  stale_generation = 3,
  bad_checksum = 4,
  corrupt_payload = 5,
  identity_mismatch = 6,
  stale_descriptor_epoch = 7,
  stale_metric_epoch = 8,
  dimension_mismatch = 9,
  unsupported_element_profile = 10,
  unsafe_metric_resource = 11,
  missing_exact_recheck_proof = 12,
  authority_claim_refused = 13,
  refused = 14
};

enum class VectorExactElementProfile : u32 {
  fp32 = 1,
  fp16 = 2,
  int8 = 3
};

enum class VectorExactMetricKind : u32 {
  l2 = 1,
  cosine = 2,
  inner_product = 3
};

enum class VectorExactMutationKind : u32 {
  insert_row = 1,
  delete_row = 2,
  update_row = 3
};

struct VectorExactRecheckProof {
  bool proof_supplied = false;
  bool exact_source_vector_available = false;
  bool exact_rerank_proof_supplied = false;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  std::string evidence_ref;
  bool parser_finality_authority_claimed = false;
  bool reference_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool index_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
  bool visibility_authority_claimed = false;
  bool security_authority_claimed = false;
  bool transaction_finality_authority_claimed = false;
};

struct VectorExactDescriptor {
  u32 dimensions = 0;
  VectorExactElementProfile element_profile = VectorExactElementProfile::fp32;
  u64 descriptor_epoch = 0;
  bool deterministic = false;
  bool descriptor_safe = false;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  double int8_scale = 0.0;
  i32 int8_zero_point = 0;
  bool parser_finality_authority_claimed = false;
  bool reference_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool index_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
};

struct VectorExactMetricResource {
  std::string metric_resource_uuid;
  u64 metric_resource_epoch = 0;
  VectorExactMetricKind metric_kind = VectorExactMetricKind::l2;
  bool deterministic = false;
  bool safe = false;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  bool contract_only_fallback = false;
  bool provider_only_fallback = false;
  bool parser_finality_authority_claimed = false;
  bool reference_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool index_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
};

struct VectorExactSourceRow {
  TextInvertedRowLocator locator;
  std::vector<float> vector;
};

struct VectorExactStoredRow {
  TextInvertedRowLocator locator;
  std::vector<byte> encoded_payload;
};

struct VectorExactPhysicalProvider {
  std::string artifact_kind = kVectorExactPhysicalProviderArtifactKind;
  PageExtentSummaryFormatVersion format_version{
      kVectorExactPhysicalProviderCurrentMajor,
      kVectorExactPhysicalProviderCurrentMinor};
  std::string relation_uuid;
  std::string index_uuid;
  std::string provider_uuid;
  u64 base_generation = 0;
  u64 provider_generation = 0;
  VectorExactDescriptor descriptor;
  VectorExactMetricResource metric;
  bool encoded_payloads_present = true;
  bool fp32_payloads_supported = true;
  bool fp16_payloads_supported = true;
  bool int8_payloads_supported = true;
  bool exact_decode_scoring_present = true;
  bool batched_query_present = true;
  bool metadata_prefilter_present = true;
  bool candidate_set_input_present = true;
  bool top_k_heap_present = true;
  bool exact_rerank_present = true;
  bool scalar_kernel_present = true;
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
  u64 mutation_generation_evidence = 0;
  std::vector<VectorExactStoredRow> rows;
  std::vector<std::string> evidence;
};

struct VectorExactBuildRequest {
  std::string relation_uuid;
  std::string index_uuid;
  std::string provider_uuid;
  u64 base_generation = 0;
  u64 provider_generation = 0;
  VectorExactDescriptor descriptor;
  VectorExactMetricResource metric;
  VectorExactRecheckProof recheck_proof;
  std::vector<VectorExactSourceRow> rows;
};

struct VectorExactBuildResult {
  Status status;
  DiagnosticRecord diagnostic;
  VectorExactPhysicalProvider provider;
  bool built = false;
  bool fail_closed = true;

  bool ok() const { return status.ok() && built && !fail_closed; }
};

struct VectorExactSerializeResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<byte> bytes;
  u64 checksum = 0;

  bool ok() const { return status.ok() && !bytes.empty(); }
};

struct VectorExactOpenRequest {
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
  bool expected_descriptor_epoch_present = false;
  u64 expected_descriptor_epoch = 0;
  bool expected_metric_resource_epoch_present = false;
  u64 expected_metric_resource_epoch = 0;
  bool expected_dimensions_present = false;
  u32 expected_dimensions = 0;
  VectorExactRecheckProof recheck_proof;
};

struct VectorExactOpenResult {
  Status status;
  DiagnosticRecord diagnostic;
  VectorExactOpenClass open_class = VectorExactOpenClass::refused;
  VectorExactPhysicalProvider provider;
  bool fail_closed = true;
  bool restricted_repair_required = false;
  std::vector<std::string> actions;

  bool ok() const {
    return status.ok() && open_class == VectorExactOpenClass::current &&
           !fail_closed;
  }
};

struct VectorExactQuery {
  std::vector<float> vector;
  u32 top_k = 0;
  std::vector<TextInvertedRowLocator> candidate_set;
  std::function<bool(const TextInvertedRowLocator&)> metadata_prefilter;
};

struct VectorExactQueryRequest {
  VectorExactPhysicalProvider provider;
  std::vector<VectorExactQuery> queries;
  VectorExactRecheckProof recheck_proof;
  bool descriptor_epoch_current = true;
  bool metric_resource_epoch_current = true;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  bool contract_only_fallback = false;
  bool provider_only_fallback = false;
};

struct VectorExactCandidate {
  TextInvertedRowLocator locator;
  double score = 0.0;
  bool lower_score_better = true;
  bool decoded_from_physical_payload = false;
  bool metadata_prefilter_passed = true;
  bool candidate_set_member = true;
  bool exact_rerank_proof_verified = false;
  bool exact_source_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool final_row_admitted = false;
  std::string source_recheck_evidence_ref;
};

struct VectorExactSingleQueryResult {
  bool ok = false;
  std::vector<VectorExactCandidate> candidates;
  u64 candidates_considered = 0;
  u64 candidates_filtered = 0;
  u64 decoded_vector_count = 0;
  u64 scalar_kernel_consumed_count = 0;
  bool top_k_heap_used = false;
  bool exact_rerank_performed = false;
  bool candidate_rows_only = true;
  bool final_rows_authorized = false;
};

struct VectorExactQueryResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = true;
  std::vector<VectorExactSingleQueryResult> batch_results;
  bool batched_query_evaluation = false;
  bool metadata_prefilter_consumed = false;
  bool candidate_set_consumed = false;
  bool scalar_kernel_consumed = false;
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
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct VectorExactMutation {
  VectorExactMutationKind kind = VectorExactMutationKind::insert_row;
  bool expected_provider_generation_present = false;
  u64 expected_provider_generation = 0;
  bool expected_descriptor_epoch_present = false;
  u64 expected_descriptor_epoch = 0;
  bool expected_metric_resource_epoch_present = false;
  u64 expected_metric_resource_epoch = 0;
  bool before_row_present = false;
  VectorExactSourceRow before_row;
  bool after_row_present = false;
  VectorExactSourceRow after_row;
  VectorExactRecheckProof recheck_proof;
};

struct VectorExactMutationResult {
  Status status;
  DiagnosticRecord diagnostic;
  VectorExactPhysicalProvider provider;
  bool applied = false;
  bool fail_closed = true;
  std::vector<std::string> actions;

  bool ok() const { return status.ok() && applied && !fail_closed; }
};

VectorExactBuildResult BuildVectorExactPhysicalProvider(
    const VectorExactBuildRequest& request);
VectorExactSerializeResult SerializeVectorExactPhysicalProvider(
    const VectorExactPhysicalProvider& provider);
VectorExactOpenResult OpenVectorExactPhysicalProvider(
    const VectorExactOpenRequest& request);
VectorExactQueryResult QueryVectorExactPhysicalProvider(
    const VectorExactQueryRequest& request);
VectorExactMutationResult ApplyVectorExactPhysicalMutation(
    const VectorExactPhysicalProvider& provider,
    const VectorExactMutation& mutation);

std::vector<byte> EncodeVectorExactPayload(
    const std::vector<float>& values,
    const VectorExactDescriptor& descriptor);
std::vector<float> DecodeVectorExactPayload(
    const std::vector<byte>& encoded_payload,
    const VectorExactDescriptor& descriptor);
u16 VectorExactEncodeFloat16(float value);
float VectorExactDecodeFloat16(u16 value);

const char* VectorExactOpenClassName(VectorExactOpenClass open_class);
const char* VectorExactElementProfileName(VectorExactElementProfile profile);
const char* VectorExactMetricKindName(VectorExactMetricKind metric);
DiagnosticRecord MakeVectorExactPhysicalProviderDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
