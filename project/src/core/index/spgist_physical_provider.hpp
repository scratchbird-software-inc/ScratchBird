// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB_SPGIST_PHYSICAL_PROVIDER
#include "spatial_rtree_physical_provider.hpp"

#include <functional>
#include <string>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kSpGistPhysicalProviderSearchKey =
    "SB_SPGIST_PHYSICAL_PROVIDER";
inline constexpr const char* kSpGistPhysicalProviderArtifactKind =
    "spgist_physical_provider";
inline constexpr const char* kSpGistSpatialQuadMbrOpclassName =
    "spgist_spatial_quad_mbr_ops";
inline constexpr u32 kSpGistPhysicalProviderCurrentMajor = 1;
inline constexpr u32 kSpGistPhysicalProviderCurrentMinor = 0;
inline constexpr u32 kSpGistDefaultLeafCapacity = 2;
inline constexpr u32 kSpGistDefaultMaxDepth = 16;

enum class SpGistOpenClass : u32 {
  current = 1,
  stale_format = 2,
  stale_generation = 3,
  bad_checksum = 4,
  corrupt_payload = 5,
  identity_mismatch = 6,
  stale_opclass_epoch = 7,
  stale_resource_epoch = 8,
  stale_descriptor_epoch = 9,
  stale_srid_resource_epoch = 10,
  unsafe_opclass = 11,
  invalid_compressed_key = 12,
  invalid_partition_key = 13,
  incompatible_spatial_profile = 14,
  authority_claim_refused = 15,
  missing_recheck_proof = 16,
  refused = 17
};

enum class SpGistPredicateStrategy : u32 {
  point = 1,
  intersects = 2,
  contains = 3,
  within = 4,
  range = 5,
  nearest = 6
};

enum class SpGistMutationKind : u32 {
  insert_row = 1,
  delete_row = 2,
  update_row = 3
};

struct SpGistExactRecheckProof {
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

struct SpGistOpclassDescriptor {
  std::string opclass_name;
  u64 opclass_epoch = 0;
  u64 resource_epoch = 0;
  bool registered = false;
  bool deterministic = false;
  bool immutable = false;
  bool safe = false;
  u32 dimensions = 0;
  u32 srid = 0;
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

struct SpGistCompressedKey {
  std::vector<byte> bytes;
  u32 dimensions = 0;
  u32 srid = 0;
  bool deterministic = false;
  bool valid = false;
};

struct SpGistPartitionKey {
  u32 depth = 0;
  u32 quadrant = 0;
  bool valid = false;
};

struct SpGistChooseResult {
  SpGistPartitionKey partition;
  bool valid = false;
  bool deterministic = false;
};

struct SpGistOpclassMethods {
  std::function<SpGistCompressedKey(const SpatialRTreeMbr&)> compress;
  std::function<SpatialRTreeMbr(const SpGistCompressedKey&)> decompress;
  std::function<SpGistChooseResult(const SpatialRTreeMbr&,
                                   const SpGistCompressedKey&,
                                   u32)>
      choose;
  std::function<std::vector<SpGistPartitionKey>(const SpatialRTreeMbr&,
                                                const SpatialRTreeMbr&,
                                                SpGistPredicateStrategy,
                                                u32)>
      inner_consistent;
  std::function<bool(const SpGistCompressedKey&,
                     const SpatialRTreeMbr&,
                     SpGistPredicateStrategy)>
      leaf_consistent;
  std::function<double(const SpGistCompressedKey&, const std::vector<double>&)>
      distance;
};

struct SpGistOpclassRuntime {
  SpGistOpclassDescriptor descriptor;
  SpGistOpclassMethods methods;
};

struct SpGistSourceRow {
  TextInvertedRowLocator locator;
  SpatialRTreeMbr key;
  std::string exact_source_recheck_evidence_ref;
};

struct SpGistStoredRow {
  TextInvertedRowLocator locator;
  SpatialRTreeMbr key;
  SpGistCompressedKey compressed;
  std::string exact_source_recheck_evidence_ref;
  bool tombstoned = false;
};

struct SpGistLeafTuple {
  TextInvertedRowLocator locator;
  SpGistCompressedKey compressed;
  SpatialRTreeMbr key;
  std::string exact_source_recheck_evidence_ref;
  bool tombstoned = false;
};

struct SpGistInnerTuple {
  u32 child_node = 0;
  SpGistPartitionKey partition;
  SpatialRTreeMbr prefix;
};

struct SpGistNode {
  u32 node_id = 0;
  bool leaf = true;
  u32 depth = 0;
  SpatialRTreeMbr prefix;
  std::vector<SpGistInnerTuple> inner_tuples;
  std::vector<SpGistLeafTuple> leaf_tuples;
};

struct SpGistPhysicalProvider {
  std::string artifact_kind = kSpGistPhysicalProviderArtifactKind;
  PageExtentSummaryFormatVersion format_version{
      kSpGistPhysicalProviderCurrentMajor, kSpGistPhysicalProviderCurrentMinor};
  std::string relation_uuid;
  std::string index_uuid;
  std::string provider_uuid;
  u64 base_generation = 0;
  u64 provider_generation = 0;
  SpatialRTreeDescriptor spatial_descriptor;
  SpatialRTreeSridResource srid_resource;
  SpGistOpclassDescriptor opclass;
  u32 root_node_id = 0;
  u32 tree_height = 0;
  u32 leaf_capacity = kSpGistDefaultLeafCapacity;
  u32 max_depth = kSpGistDefaultMaxDepth;
  u64 split_partition_count = 0;
  u64 merge_count = 0;
  u64 choose_call_count = 0;
  u64 inner_consistent_call_count = 0;
  u64 leaf_consistent_call_count = 0;
  u64 compress_call_count = 0;
  u64 decompress_call_count = 0;
  u64 distance_call_count = 0;
  bool physical_inner_tuple_layout_present = true;
  bool physical_leaf_tuple_layout_present = true;
  bool partitioned_search_tree_present = true;
  bool choose_present = true;
  bool inner_consistent_present = true;
  bool leaf_consistent_present = true;
  bool compress_present = true;
  bool decompress_present = true;
  bool bulk_spatial_prefix_build_present = true;
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
  std::vector<SpGistStoredRow> rows;
  std::vector<SpGistNode> nodes;
  std::vector<std::string> evidence;
};

struct SpGistBuildRequest {
  std::string relation_uuid;
  std::string index_uuid;
  std::string provider_uuid;
  u64 base_generation = 0;
  u64 provider_generation = 0;
  SpatialRTreeDescriptor spatial_descriptor;
  SpatialRTreeSridResource srid_resource;
  SpGistOpclassRuntime opclass;
  SpGistExactRecheckProof recheck_proof;
  u32 leaf_capacity = kSpGistDefaultLeafCapacity;
  u32 max_depth = kSpGistDefaultMaxDepth;
  std::vector<SpGistSourceRow> rows;
};

struct SpGistBuildResult {
  Status status;
  DiagnosticRecord diagnostic;
  SpGistPhysicalProvider provider;
  bool built = false;
  bool fail_closed = true;
  bool used_bulk_spatial_prefix_build = false;
  bool all_methods_exercised = false;

  bool ok() const { return status.ok() && built && !fail_closed; }
};

struct SpGistSerializeResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<byte> bytes;
  u64 checksum = 0;

  bool ok() const { return status.ok() && !bytes.empty(); }
};

struct SpGistOpenRequest {
  std::vector<byte> bytes;
  SpGistOpclassRuntime opclass;
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
  bool expected_opclass_epoch_present = false;
  u64 expected_opclass_epoch = 0;
  bool expected_resource_epoch_present = false;
  u64 expected_resource_epoch = 0;
  bool expected_descriptor_epoch_present = false;
  u64 expected_descriptor_epoch = 0;
  bool expected_srid_resource_epoch_present = false;
  u64 expected_srid_resource_epoch = 0;
  bool expected_srid_present = false;
  u32 expected_srid = 0;
  SpGistExactRecheckProof recheck_proof;
};

struct SpGistOpenResult {
  Status status;
  DiagnosticRecord diagnostic;
  SpGistOpenClass open_class = SpGistOpenClass::refused;
  SpGistPhysicalProvider provider;
  bool fail_closed = true;
  bool restricted_repair_required = false;
  std::vector<std::string> actions;

  bool ok() const {
    return status.ok() && open_class == SpGistOpenClass::current &&
           !fail_closed;
  }
};

struct SpGistQueryRequest {
  SpGistPhysicalProvider provider;
  SpGistOpclassRuntime opclass;
  SpGistPredicateStrategy strategy = SpGistPredicateStrategy::intersects;
  SpatialRTreeMbr query_mbr;
  std::vector<double> query_point;
  u32 top_k = 0;
  SpGistExactRecheckProof recheck_proof;
  bool opclass_epoch_current = true;
  bool resource_epoch_current = true;
  bool descriptor_epoch_current = true;
  bool srid_resource_epoch_current = true;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  bool contract_only_fallback = false;
  bool provider_only_fallback = false;
};

struct SpGistCandidate {
  TextInvertedRowLocator locator;
  SpatialRTreeMbr key;
  double distance = 0.0;
  bool from_spgist_leaf_tuple = true;
  bool opclass_leaf_consistent = false;
  bool exact_source_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool final_row_admitted = false;
  std::string source_recheck_evidence_ref;
};

struct SpGistQueryResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = true;
  std::vector<SpGistCandidate> candidates;
  u64 nodes_visited = 0;
  u64 inner_tuples_examined = 0;
  u64 leaf_tuples_examined = 0;
  bool partitioned_search_tree_used = false;
  bool inner_consistent_used = false;
  bool leaf_consistent_used = false;
  bool priority_queue_used = false;
  bool candidate_rows_only = true;
  bool final_rows_authorized = false;
  bool exact_source_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct SpGistMutation {
  SpGistMutationKind kind = SpGistMutationKind::insert_row;
  bool expected_provider_generation_present = false;
  u64 expected_provider_generation = 0;
  bool expected_opclass_epoch_present = false;
  u64 expected_opclass_epoch = 0;
  bool expected_resource_epoch_present = false;
  u64 expected_resource_epoch = 0;
  bool expected_descriptor_epoch_present = false;
  u64 expected_descriptor_epoch = 0;
  bool expected_srid_resource_epoch_present = false;
  u64 expected_srid_resource_epoch = 0;
  bool before_row_present = false;
  SpGistSourceRow before_row;
  bool after_row_present = false;
  SpGistSourceRow after_row;
  SpGistExactRecheckProof recheck_proof;
};

struct SpGistMutationResult {
  Status status;
  DiagnosticRecord diagnostic;
  SpGistPhysicalProvider provider;
  bool applied = false;
  bool fail_closed = true;
  bool split_partition_performed = false;
  bool merge_performed = false;
  bool tombstone_written = false;
  std::vector<std::string> actions;

  bool ok() const { return status.ok() && applied && !fail_closed; }
};

SpGistOpclassRuntime MakeSpatialQuadMbrSpGistOpclass(u64 opclass_epoch,
                                                     u64 resource_epoch,
                                                     u32 srid);
SpGistBuildResult BuildSpGistPhysicalProvider(
    const SpGistBuildRequest& request);
SpGistSerializeResult SerializeSpGistPhysicalProvider(
    const SpGistPhysicalProvider& provider);
SpGistOpenResult OpenSpGistPhysicalProvider(const SpGistOpenRequest& request);
SpGistQueryResult QuerySpGistPhysicalProvider(
    const SpGistQueryRequest& request);
SpGistMutationResult ApplySpGistPhysicalMutation(
    const SpGistPhysicalProvider& provider,
    const SpGistOpclassRuntime& opclass,
    const SpGistMutation& mutation);

bool SpGistCompressedKeyValid(const SpGistCompressedKey& key,
                              const SpGistOpclassDescriptor& opclass);
bool SpGistPartitionKeyValid(const SpGistPartitionKey& key, u32 max_depth);
const char* SpGistOpenClassName(SpGistOpenClass open_class);
DiagnosticRecord MakeSpGistPhysicalProviderDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
