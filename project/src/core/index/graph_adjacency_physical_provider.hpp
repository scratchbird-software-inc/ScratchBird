// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB_GRAPH_ADJACENCY_PHYSICAL_PROVIDER
#include "candidate_set.hpp"
#include "text_inverted_segment.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kGraphAdjacencyPhysicalProviderSearchKey =
    "SB_GRAPH_ADJACENCY_PHYSICAL_PROVIDER";
inline constexpr const char* kGraphAdjacencyPhysicalProviderArtifactKind =
    "graph_adjacency_physical_provider";
inline constexpr u32 kGraphAdjacencyPhysicalProviderCurrentMajor = 1;
inline constexpr u32 kGraphAdjacencyPhysicalProviderCurrentMinor = 0;

enum class GraphAdjacencyOpenClass : u32 {
  current = 1,
  stale_format = 2,
  stale_generation = 3,
  bad_checksum = 4,
  corrupt_payload = 5,
  identity_mismatch = 6,
  stale_descriptor_epoch = 7,
  missing_recheck_proof = 8,
  authority_claim_refused = 9,
  stale_runtime_epoch = 10,
  refused = 11
};

enum class GraphEntityKind : u32 {
  vertex = 1,
  edge = 2
};

enum class GraphAdjacencyDirection : u32 {
  outgoing = 1,
  incoming = 2,
  both = 3
};

enum class GraphMutationKind : u32 {
  insert_vertex = 1,
  update_vertex = 2,
  delete_vertex = 3,
  insert_edge = 4,
  update_edge = 5,
  delete_edge = 6
};

struct GraphRecheckProof {
  bool proof_supplied = false;
  bool exact_source_recheck_required = true;
  bool exact_source_available = false;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  std::string evidence_ref;
  bool parser_finality_authority_claimed = false;
  bool donor_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool index_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
  bool visibility_authority_claimed = false;
  bool security_authority_claimed = false;
  bool transaction_finality_authority_claimed = false;
};

struct GraphDescriptor {
  u64 descriptor_epoch = 0;
  bool deterministic = false;
  bool descriptor_safe = false;
  bool vertex_id_index = true;
  bool edge_source_adjacency = true;
  bool edge_target_adjacency = true;
  bool label_index = true;
  bool property_index = true;
  bool typed_edge_label_adjacency = true;
  bool frontier_batch_expansion = true;
  bool visited_compressed_bitmap = true;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  bool contract_only_fallback = false;
  bool provider_only_fallback = false;
  bool parser_finality_authority_claimed = false;
  bool donor_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool index_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
};

struct GraphPropertyValue {
  std::string key;
  std::string type_tag;
  std::string encoded_value;
};

struct GraphVertexInput {
  std::string vertex_id;
  TextInvertedRowLocator locator;
  std::vector<std::string> labels;
  std::vector<GraphPropertyValue> properties;
  std::string exact_source_recheck_evidence_ref;
};

struct GraphEdgeInput {
  std::string edge_id;
  std::string source_vertex_id;
  std::string target_vertex_id;
  std::string label;
  TextInvertedRowLocator locator;
  std::vector<GraphPropertyValue> properties;
  std::string exact_source_recheck_evidence_ref;
};

struct GraphVertexRecord {
  GraphVertexInput row;
  bool tombstoned = false;
};

struct GraphEdgeRecord {
  GraphEdgeInput row;
  bool tombstoned = false;
};

struct GraphVertexIdIndexEntry {
  std::string vertex_id;
  TextInvertedRowLocator locator;
};

struct GraphAdjacencyIndexEntry {
  std::string vertex_id;
  std::string edge_label;
  std::string edge_id;
  std::string other_vertex_id;
  TextInvertedRowLocator edge_locator;
};

struct GraphLabelIndexEntry {
  std::string label;
  GraphEntityKind entity_kind = GraphEntityKind::vertex;
  std::string entity_id;
  TextInvertedRowLocator locator;
};

struct GraphPropertyIndexEntry {
  std::string key;
  std::string type_tag;
  std::string encoded_value;
  GraphEntityKind entity_kind = GraphEntityKind::vertex;
  std::string entity_id;
  TextInvertedRowLocator locator;
};

struct GraphAdjacencyPhysicalProvider {
  std::string artifact_kind = kGraphAdjacencyPhysicalProviderArtifactKind;
  PageExtentSummaryFormatVersion format_version{
      kGraphAdjacencyPhysicalProviderCurrentMajor,
      kGraphAdjacencyPhysicalProviderCurrentMinor};
  std::string relation_uuid;
  std::string index_uuid;
  std::string provider_uuid;
  u64 base_generation = 0;
  u64 provider_generation = 0;
  GraphDescriptor descriptor;
  bool vertex_id_index_present = true;
  bool edge_source_adjacency_present = true;
  bool edge_target_adjacency_present = true;
  bool label_index_present = true;
  bool property_index_present = true;
  bool typed_edge_label_adjacency_present = true;
  bool frontier_batch_expansion_present = true;
  bool visited_compressed_bitmap_present = true;
  bool candidate_evidence_only = true;
  bool exact_source_recheck_required = true;
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
  std::vector<GraphVertexRecord> vertices;
  std::vector<GraphEdgeRecord> edges;
  std::vector<GraphVertexIdIndexEntry> vertex_id_index;
  std::vector<GraphAdjacencyIndexEntry> edge_source_adjacency;
  std::vector<GraphAdjacencyIndexEntry> edge_target_adjacency;
  std::vector<GraphLabelIndexEntry> label_index;
  std::vector<GraphPropertyIndexEntry> property_index;
  std::vector<GraphAdjacencyIndexEntry> typed_edge_label_adjacency;
  std::vector<std::string> evidence;
};

struct GraphBuildRequest {
  std::string relation_uuid;
  std::string index_uuid;
  std::string provider_uuid;
  u64 base_generation = 0;
  u64 provider_generation = 0;
  GraphDescriptor descriptor;
  GraphRecheckProof recheck_proof;
  std::vector<GraphVertexInput> vertices;
  std::vector<GraphEdgeInput> edges;
};

struct GraphBuildResult {
  Status status;
  DiagnosticRecord diagnostic;
  GraphAdjacencyPhysicalProvider provider;
  bool built = false;
  bool fail_closed = true;

  bool ok() const { return status.ok() && built && !fail_closed; }
};

struct GraphSerializeResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<byte> bytes;
  u64 checksum = 0;

  bool ok() const { return status.ok() && !bytes.empty(); }
};

struct GraphOpenRequest {
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
  GraphRecheckProof recheck_proof;
};

struct GraphOpenResult {
  Status status;
  DiagnosticRecord diagnostic;
  GraphAdjacencyOpenClass open_class = GraphAdjacencyOpenClass::refused;
  GraphAdjacencyPhysicalProvider provider;
  bool fail_closed = true;
  bool restricted_repair_required = false;
  std::vector<std::string> actions;

  bool ok() const {
    return status.ok() && open_class == GraphAdjacencyOpenClass::current &&
           !fail_closed;
  }
};

struct GraphQueryRequest {
  GraphAdjacencyPhysicalProvider provider;
  GraphRecheckProof recheck_proof;
  bool descriptor_epoch_current = true;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  bool contract_only_fallback = false;
  bool provider_only_fallback = false;
};

struct GraphVertexLookupRequest : GraphQueryRequest {
  std::string vertex_id;
};

struct GraphAdjacencyLookupRequest : GraphQueryRequest {
  std::string vertex_id;
  bool label_filter_present = false;
  std::string edge_label;
  GraphAdjacencyDirection direction = GraphAdjacencyDirection::outgoing;
};

struct GraphLabelLookupRequest : GraphQueryRequest {
  std::string label;
  bool include_vertices = true;
  bool include_edges = true;
};

struct GraphPropertyLookupRequest : GraphQueryRequest {
  std::string key;
  std::string type_tag;
  std::string encoded_value;
  bool include_vertices = true;
  bool include_edges = true;
};

struct GraphFrontierExpandRequest : GraphQueryRequest {
  std::vector<std::string> frontier_vertex_ids;
  std::vector<std::string> visited_vertex_ids;
  bool label_filter_present = false;
  std::string edge_label;
  GraphAdjacencyDirection direction = GraphAdjacencyDirection::outgoing;
  u32 max_output_vertices = 0;
};

struct GraphCandidate {
  GraphEntityKind entity_kind = GraphEntityKind::vertex;
  std::string entity_id;
  std::string vertex_id;
  std::string edge_id;
  std::string source_vertex_id;
  std::string target_vertex_id;
  std::string edge_label;
  TextInvertedRowLocator locator;
  bool from_physical_index = true;
  bool exact_source_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool final_row_admitted = false;
  std::string source_recheck_evidence_ref;
};

struct GraphQueryResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = true;
  std::vector<GraphCandidate> candidates;
  bool candidate_rows_only = true;
  bool final_rows_authorized = false;
  bool exact_source_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool vertex_id_index_used = false;
  bool edge_source_adjacency_used = false;
  bool edge_target_adjacency_used = false;
  bool label_index_used = false;
  bool property_index_used = false;
  bool typed_edge_label_adjacency_used = false;
  u64 index_entries_examined = 0;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct GraphFrontierExpandResult : GraphQueryResult {
  CandidateSet visited_candidate_set;
  bool compressed_bitmap_visited_set_used = false;
  u64 visited_cardinality = 0;
};

struct GraphMutation {
  GraphMutationKind kind = GraphMutationKind::insert_vertex;
  bool expected_provider_generation_present = false;
  u64 expected_provider_generation = 0;
  bool expected_descriptor_epoch_present = false;
  u64 expected_descriptor_epoch = 0;
  bool before_vertex_present = false;
  GraphVertexInput before_vertex;
  bool after_vertex_present = false;
  GraphVertexInput after_vertex;
  bool before_edge_present = false;
  GraphEdgeInput before_edge;
  bool after_edge_present = false;
  GraphEdgeInput after_edge;
  GraphRecheckProof recheck_proof;
};

struct GraphMutationResult {
  Status status;
  DiagnosticRecord diagnostic;
  GraphAdjacencyPhysicalProvider provider;
  bool applied = false;
  bool fail_closed = true;
  bool tombstone_written = false;
  bool compaction_performed = false;
  std::vector<std::string> actions;

  bool ok() const { return status.ok() && applied && !fail_closed; }
};

GraphBuildResult BuildGraphAdjacencyPhysicalProvider(
    const GraphBuildRequest& request);
GraphSerializeResult SerializeGraphAdjacencyPhysicalProvider(
    const GraphAdjacencyPhysicalProvider& provider);
GraphOpenResult OpenGraphAdjacencyPhysicalProvider(
    const GraphOpenRequest& request);
GraphQueryResult QueryGraphVertexIdIndex(
    const GraphVertexLookupRequest& request);
GraphQueryResult QueryGraphAdjacencyIndex(
    const GraphAdjacencyLookupRequest& request);
GraphQueryResult QueryGraphLabelIndex(const GraphLabelLookupRequest& request);
GraphQueryResult QueryGraphPropertyIndex(
    const GraphPropertyLookupRequest& request);
GraphFrontierExpandResult ExpandGraphFrontierBatch(
    const GraphFrontierExpandRequest& request);
GraphMutationResult ApplyGraphAdjacencyPhysicalMutation(
    const GraphAdjacencyPhysicalProvider& provider,
    const GraphMutation& mutation);
GraphMutationResult CompactGraphAdjacencyPhysicalProvider(
    const GraphAdjacencyPhysicalProvider& provider,
    const GraphRecheckProof& recheck_proof);

const char* GraphAdjacencyOpenClassName(GraphAdjacencyOpenClass open_class);
DiagnosticRecord MakeGraphAdjacencyPhysicalProviderDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
