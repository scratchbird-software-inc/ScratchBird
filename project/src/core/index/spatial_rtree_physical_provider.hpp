// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB_SPATIAL_RTREE_PHYSICAL_PROVIDER
#include "text_inverted_segment.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kSpatialRTreePhysicalProviderSearchKey =
    "SB_SPATIAL_RTREE_PHYSICAL_PROVIDER";
inline constexpr const char* kSpatialRTreePhysicalProviderArtifactKind =
    "spatial_rtree_physical_provider";
inline constexpr u32 kSpatialRTreePhysicalProviderCurrentMajor = 1;
inline constexpr u32 kSpatialRTreePhysicalProviderCurrentMinor = 0;
inline constexpr u32 kSpatialRTreeDefaultMaxEntries = 4;
inline constexpr u32 kSpatialRTreeDefaultMinEntries = 2;

enum class SpatialRTreeOpenClass : u32 {
  current = 1,
  stale_format = 2,
  stale_generation = 3,
  bad_checksum = 4,
  corrupt_payload = 5,
  identity_mismatch = 6,
  stale_descriptor_epoch = 7,
  stale_srid_resource_epoch = 8,
  invalid_mbr = 9,
  unsupported_geometry_profile = 10,
  missing_exact_recheck_proof = 11,
  authority_claim_refused = 12,
  refused = 13
};

enum class SpatialRTreeBuildMode : u32 {
  incremental_insert = 1,
  str_bulk = 2
};

enum class SpatialRTreeQueryKind : u32 {
  point = 1,
  intersects = 2,
  contains = 3,
  within = 4,
  range = 5,
  nearest = 6
};

enum class SpatialRTreeMutationKind : u32 {
  insert_row = 1,
  delete_row = 2,
  update_row = 3
};

struct SpatialRTreeMbr {
  u32 dimensions = 0;
  u32 srid = 0;
  std::vector<double> min;
  std::vector<double> max;
};

struct SpatialRTreeRecheckProof {
  bool proof_supplied = false;
  bool exact_source_geometry_available = false;
  bool exact_predicate_recheck_required = true;
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

struct SpatialRTreeDescriptor {
  u32 dimensions = 2;
  u64 descriptor_epoch = 0;
  bool deterministic = false;
  bool descriptor_safe = false;
  bool supports_point = true;
  bool supports_mbr = true;
  bool supports_z = false;
  bool supports_m = false;
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

struct SpatialRTreeSridResource {
  std::string resource_uuid;
  u32 srid = 0;
  u64 resource_epoch = 0;
  std::string coordinate_order = "xy";
  bool deterministic = false;
  bool safe = false;
  bool cache_present = true;
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

struct SpatialRTreeSourceRow {
  TextInvertedRowLocator locator;
  SpatialRTreeMbr mbr;
  std::string exact_source_recheck_evidence_ref;
};

struct SpatialRTreeStoredRow {
  TextInvertedRowLocator locator;
  SpatialRTreeMbr mbr;
  std::string exact_source_recheck_evidence_ref;
  bool tombstoned = false;
};

struct SpatialRTreeNodeEntry {
  SpatialRTreeMbr mbr;
  bool child = false;
  u32 child_node = 0;
  TextInvertedRowLocator locator;
  bool tombstoned = false;
};

struct SpatialRTreeNode {
  u32 node_id = 0;
  bool leaf = true;
  u32 level = 0;
  SpatialRTreeMbr cover;
  std::vector<SpatialRTreeNodeEntry> entries;
};

struct SpatialRTreePhysicalProvider {
  std::string artifact_kind = kSpatialRTreePhysicalProviderArtifactKind;
  PageExtentSummaryFormatVersion format_version{
      kSpatialRTreePhysicalProviderCurrentMajor,
      kSpatialRTreePhysicalProviderCurrentMinor};
  std::string relation_uuid;
  std::string index_uuid;
  std::string provider_uuid;
  u64 base_generation = 0;
  u64 provider_generation = 0;
  SpatialRTreeDescriptor descriptor;
  SpatialRTreeSridResource srid_resource;
  u32 max_entries = kSpatialRTreeDefaultMaxEntries;
  u32 min_entries = kSpatialRTreeDefaultMinEntries;
  u32 root_node_id = 0;
  u32 tree_height = 0;
  u64 split_count = 0;
  u64 merge_count = 0;
  bool mbr_encoding_present = true;
  bool insert_search_split_merge_present = true;
  bool nearest_neighbor_priority_queue_present = true;
  bool srid_epoch_cache_present = true;
  bool str_bulk_build_present = true;
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
  bool reference_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool index_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
  std::vector<SpatialRTreeStoredRow> rows;
  std::vector<SpatialRTreeNode> nodes;
  std::vector<std::string> evidence;
};

struct SpatialRTreeBuildRequest {
  std::string relation_uuid;
  std::string index_uuid;
  std::string provider_uuid;
  u64 base_generation = 0;
  u64 provider_generation = 0;
  SpatialRTreeDescriptor descriptor;
  SpatialRTreeSridResource srid_resource;
  SpatialRTreeRecheckProof recheck_proof;
  SpatialRTreeBuildMode build_mode = SpatialRTreeBuildMode::incremental_insert;
  u32 max_entries = kSpatialRTreeDefaultMaxEntries;
  u32 min_entries = kSpatialRTreeDefaultMinEntries;
  std::vector<SpatialRTreeSourceRow> rows;
};

struct SpatialRTreeBuildResult {
  Status status;
  DiagnosticRecord diagnostic;
  SpatialRTreePhysicalProvider provider;
  bool built = false;
  bool fail_closed = true;
  bool used_str_bulk_build = false;

  bool ok() const { return status.ok() && built && !fail_closed; }
};

struct SpatialRTreeSerializeResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<byte> bytes;
  u64 checksum = 0;

  bool ok() const { return status.ok() && !bytes.empty(); }
};

struct SpatialRTreeOpenRequest {
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
  bool expected_srid_resource_epoch_present = false;
  u64 expected_srid_resource_epoch = 0;
  bool expected_srid_present = false;
  u32 expected_srid = 0;
  SpatialRTreeRecheckProof recheck_proof;
};

struct SpatialRTreeOpenResult {
  Status status;
  DiagnosticRecord diagnostic;
  SpatialRTreeOpenClass open_class = SpatialRTreeOpenClass::refused;
  SpatialRTreePhysicalProvider provider;
  bool fail_closed = true;
  bool restricted_repair_required = false;
  std::vector<std::string> actions;

  bool ok() const {
    return status.ok() && open_class == SpatialRTreeOpenClass::current &&
           !fail_closed;
  }
};

struct SpatialRTreeQueryRequest {
  SpatialRTreePhysicalProvider provider;
  SpatialRTreeQueryKind kind = SpatialRTreeQueryKind::intersects;
  SpatialRTreeMbr query_mbr;
  std::vector<double> query_point;
  u32 top_k = 0;
  SpatialRTreeRecheckProof recheck_proof;
  bool descriptor_epoch_current = true;
  bool srid_resource_epoch_current = true;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  bool contract_only_fallback = false;
  bool provider_only_fallback = false;
};

struct SpatialRTreeCandidate {
  TextInvertedRowLocator locator;
  SpatialRTreeMbr mbr;
  double distance = 0.0;
  bool from_rtree_node = true;
  bool exact_source_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool final_row_admitted = false;
  std::string source_recheck_evidence_ref;
};

struct SpatialRTreeQueryResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = true;
  std::vector<SpatialRTreeCandidate> candidates;
  u64 nodes_visited = 0;
  u64 entries_examined = 0;
  bool priority_queue_used = false;
  bool mbr_predicate_evaluated = false;
  bool candidate_rows_only = true;
  bool final_rows_authorized = false;
  bool exact_source_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct SpatialRTreeMutation {
  SpatialRTreeMutationKind kind = SpatialRTreeMutationKind::insert_row;
  bool expected_provider_generation_present = false;
  u64 expected_provider_generation = 0;
  bool expected_descriptor_epoch_present = false;
  u64 expected_descriptor_epoch = 0;
  bool expected_srid_resource_epoch_present = false;
  u64 expected_srid_resource_epoch = 0;
  bool before_row_present = false;
  SpatialRTreeSourceRow before_row;
  bool after_row_present = false;
  SpatialRTreeSourceRow after_row;
  SpatialRTreeRecheckProof recheck_proof;
};

struct SpatialRTreeMutationResult {
  Status status;
  DiagnosticRecord diagnostic;
  SpatialRTreePhysicalProvider provider;
  bool applied = false;
  bool fail_closed = true;
  bool split_performed = false;
  bool merge_performed = false;
  bool tombstone_written = false;
  std::vector<std::string> actions;

  bool ok() const { return status.ok() && applied && !fail_closed; }
};

SpatialRTreeBuildResult BuildSpatialRTreePhysicalProvider(
    const SpatialRTreeBuildRequest& request);
SpatialRTreeSerializeResult SerializeSpatialRTreePhysicalProvider(
    const SpatialRTreePhysicalProvider& provider);
SpatialRTreeOpenResult OpenSpatialRTreePhysicalProvider(
    const SpatialRTreeOpenRequest& request);
SpatialRTreeQueryResult QuerySpatialRTreePhysicalProvider(
    const SpatialRTreeQueryRequest& request);
SpatialRTreeMutationResult ApplySpatialRTreePhysicalMutation(
    const SpatialRTreePhysicalProvider& provider,
    const SpatialRTreeMutation& mutation);

std::vector<byte> EncodeSpatialRTreeMbr(const SpatialRTreeMbr& mbr);
SpatialRTreeMbr DecodeSpatialRTreeMbr(const std::vector<byte>& encoded,
                                      u32 expected_dimensions,
                                      u32 expected_srid);
bool SpatialRTreeMbrValid(const SpatialRTreeMbr& mbr,
                          const SpatialRTreeDescriptor& descriptor,
                          const SpatialRTreeSridResource& srid_resource);

const char* SpatialRTreeOpenClassName(SpatialRTreeOpenClass open_class);
const char* SpatialRTreeQueryKindName(SpatialRTreeQueryKind kind);
DiagnosticRecord MakeSpatialRTreePhysicalProviderDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
