// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB_VECTOR_HNSW_PHYSICAL_PROVIDER
#include "vector_exact_physical_provider.hpp"

#include <functional>
#include <string>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kVectorHnswPhysicalProviderSearchKey =
    "SB_VECTOR_HNSW_PHYSICAL_PROVIDER";
inline constexpr const char* kVectorHnswPhysicalProviderArtifactKind =
    "vector_hnsw_physical_provider";
inline constexpr u32 kVectorHnswPhysicalProviderCurrentMajor = 1;
inline constexpr u32 kVectorHnswPhysicalProviderCurrentMinor = 0;

enum class VectorHnswOpenClass : u32 {
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
  invalid_graph = 14,
  duplicate_row_locator = 15,
  refused = 16
};

enum class VectorHnswMutationKind : u32 {
  insert_row = 1,
  delete_row = 2,
  update_row = 3
};

using VectorHnswDescriptor = VectorExactDescriptor;
using VectorHnswMetricResource = VectorExactMetricResource;
using VectorHnswRecheckProof = VectorExactRecheckProof;
using VectorHnswSourceRow = VectorExactSourceRow;

struct VectorHnswBuildProfile {
  u32 m = 8;
  u32 ef_construction = 32;
  u32 ef_search = 24;
  u32 max_level = 6;
  double compaction_tombstone_ratio = 0.25;
  bool deterministic_level_assignment = true;
  bool scalar_kernel_present = true;
  bool simd_kernel_present = false;
};

struct VectorHnswGraphNode {
  u32 node_id = 0;
  TextInvertedRowLocator locator;
  std::vector<byte> encoded_payload;
  u32 level = 0;
  bool tombstoned = false;
  u64 insert_generation = 0;
  u64 delete_generation = 0;
  std::vector<std::vector<u32>> layer_neighbors;
};

struct VectorHnswPhysicalProvider {
  std::string artifact_kind = kVectorHnswPhysicalProviderArtifactKind;
  PageExtentSummaryFormatVersion format_version{
      kVectorHnswPhysicalProviderCurrentMajor,
      kVectorHnswPhysicalProviderCurrentMinor};
  std::string relation_uuid;
  std::string index_uuid;
  std::string provider_uuid;
  u64 base_generation = 0;
  u64 provider_generation = 0;
  u64 training_generation = 0;
  VectorHnswDescriptor descriptor;
  VectorHnswMetricResource metric;
  VectorHnswBuildProfile profile;
  bool graph_storage_present = true;
  bool layers_present = true;
  bool entry_point_present = true;
  bool neighbor_lists_present = true;
  bool row_locators_present = true;
  bool encoded_payloads_present = true;
  bool tombstones_present = true;
  bool generation_evidence_present = true;
  bool deterministic_ordering_present = true;
  bool ef_construction_graph_build_present = true;
  bool ef_search_traversal_present = true;
  bool exact_rerank_present = true;
  bool metadata_prefilter_present = true;
  bool candidate_set_input_present = true;
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
  bool donor_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool index_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
  u32 entry_point_node_id = 0;
  u32 max_observed_level = 0;
  bool empty_graph = true;
  u64 live_node_count = 0;
  u64 tombstone_count = 0;
  double tombstone_ratio = 0.0;
  bool compaction_rebuild_required = false;
  u64 mutation_generation_evidence = 0;
  u64 graph_rebuild_generation_evidence = 0;
  u64 last_query_visited_nodes = 0;
  u64 last_query_candidate_count = 0;
  u64 last_query_exact_rerank_count = 0;
  u64 last_query_latency_units = 0;
  double last_query_recall_floor = 1.0;
  std::vector<VectorHnswGraphNode> nodes;
  std::vector<std::string> evidence;
};

struct VectorHnswBuildRequest {
  std::string relation_uuid;
  std::string index_uuid;
  std::string provider_uuid;
  u64 base_generation = 0;
  u64 provider_generation = 0;
  u64 training_generation = 0;
  VectorHnswDescriptor descriptor;
  VectorHnswMetricResource metric;
  VectorHnswBuildProfile profile;
  VectorHnswRecheckProof recheck_proof;
  std::vector<VectorHnswSourceRow> rows;
};

struct VectorHnswBuildResult {
  Status status;
  DiagnosticRecord diagnostic;
  VectorHnswPhysicalProvider provider;
  bool built = false;
  bool fail_closed = true;

  bool ok() const { return status.ok() && built && !fail_closed; }
};

struct VectorHnswSerializeResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<byte> bytes;
  u64 checksum = 0;

  bool ok() const { return status.ok() && !bytes.empty(); }
};

struct VectorHnswOpenRequest {
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
  VectorHnswRecheckProof recheck_proof;
};

struct VectorHnswOpenResult {
  Status status;
  DiagnosticRecord diagnostic;
  VectorHnswOpenClass open_class = VectorHnswOpenClass::refused;
  VectorHnswPhysicalProvider provider;
  bool fail_closed = true;
  bool restricted_repair_required = false;
  std::vector<std::string> actions;

  bool ok() const {
    return status.ok() && open_class == VectorHnswOpenClass::current &&
           !fail_closed;
  }
};

struct VectorHnswQuery {
  std::vector<float> vector;
  u32 top_k = 0;
  u32 ef_search = 0;
  std::vector<TextInvertedRowLocator> candidate_set;
  std::function<bool(const TextInvertedRowLocator&)> metadata_prefilter;
};

struct VectorHnswQueryRequest {
  VectorHnswPhysicalProvider provider;
  std::vector<VectorHnswQuery> queries;
  VectorHnswRecheckProof recheck_proof;
  bool descriptor_epoch_current = true;
  bool metric_resource_epoch_current = true;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  bool contract_only_fallback = false;
  bool provider_only_fallback = false;
};

struct VectorHnswCandidate {
  TextInvertedRowLocator locator;
  u32 node_id = 0;
  double approximate_score = 0.0;
  double exact_score = 0.0;
  bool lower_score_better = true;
  bool reached_by_ef_search = false;
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

struct VectorHnswSingleQueryResult {
  bool ok = false;
  std::vector<VectorHnswCandidate> candidates;
  u64 graph_nodes_visited = 0;
  u64 graph_edges_considered = 0;
  u64 approximate_candidate_count = 0;
  u64 exact_rerank_count = 0;
  u64 scalar_kernel_consumed_count = 0;
  bool ef_search_used = false;
  bool exact_rerank_performed = false;
  bool candidate_rows_only = true;
  bool final_rows_authorized = false;
};

struct VectorHnswQueryResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = true;
  std::vector<VectorHnswSingleQueryResult> batch_results;
  bool ef_search_traversal = false;
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
  VectorHnswPhysicalProvider provider_after_telemetry;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct VectorHnswMutation {
  VectorHnswMutationKind kind = VectorHnswMutationKind::insert_row;
  bool expected_provider_generation_present = false;
  u64 expected_provider_generation = 0;
  bool expected_descriptor_epoch_present = false;
  u64 expected_descriptor_epoch = 0;
  bool expected_metric_resource_epoch_present = false;
  u64 expected_metric_resource_epoch = 0;
  bool before_row_present = false;
  VectorHnswSourceRow before_row;
  bool after_row_present = false;
  VectorHnswSourceRow after_row;
  VectorHnswRecheckProof recheck_proof;
};

struct VectorHnswMutationResult {
  Status status;
  DiagnosticRecord diagnostic;
  VectorHnswPhysicalProvider provider;
  bool applied = false;
  bool fail_closed = true;
  bool graph_repaired = false;
  bool tombstone_recorded = false;
  bool compaction_rebuild_required = false;
  std::vector<std::string> actions;

  bool ok() const { return status.ok() && applied && !fail_closed; }
};

struct VectorHnswCompactionResult {
  Status status;
  DiagnosticRecord diagnostic;
  VectorHnswPhysicalProvider provider;
  bool compacted = false;
  bool fail_closed = true;
  u64 removed_tombstones = 0;
  std::vector<std::string> actions;

  bool ok() const { return status.ok() && compacted && !fail_closed; }
};

VectorHnswBuildResult BuildVectorHnswPhysicalProvider(
    const VectorHnswBuildRequest& request);
VectorHnswSerializeResult SerializeVectorHnswPhysicalProvider(
    const VectorHnswPhysicalProvider& provider);
VectorHnswOpenResult OpenVectorHnswPhysicalProvider(
    const VectorHnswOpenRequest& request);
VectorHnswQueryResult QueryVectorHnswPhysicalProvider(
    const VectorHnswQueryRequest& request);
VectorHnswMutationResult ApplyVectorHnswPhysicalMutation(
    const VectorHnswPhysicalProvider& provider,
    const VectorHnswMutation& mutation);
VectorHnswCompactionResult CompactVectorHnswPhysicalProvider(
    const VectorHnswPhysicalProvider& provider,
    const VectorHnswRecheckProof& proof);

const char* VectorHnswOpenClassName(VectorHnswOpenClass open_class);
DiagnosticRecord MakeVectorHnswPhysicalProviderDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
