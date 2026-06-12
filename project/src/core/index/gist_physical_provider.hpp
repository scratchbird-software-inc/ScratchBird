// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB_GIST_PHYSICAL_PROVIDER
#include "spatial_rtree_physical_provider.hpp"

#include <functional>
#include <string>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kGistPhysicalProviderSearchKey =
    "SB_GIST_PHYSICAL_PROVIDER";
inline constexpr const char* kGistPhysicalProviderArtifactKind =
    "gist_physical_provider";
inline constexpr const char* kGistSpatialMbrOpclassName =
    "gist_spatial_mbr_ops";
inline constexpr u32 kGistPhysicalProviderCurrentMajor = 1;
inline constexpr u32 kGistPhysicalProviderCurrentMinor = 0;

enum class GistOpenClass : u32 {
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
  incompatible_spatial_profile = 13,
  authority_claim_refused = 14,
  missing_recheck_proof = 15,
  refused = 16
};

enum class GistPredicateStrategy : u32 {
  point = 1,
  intersects = 2,
  contains = 3,
  within = 4,
  range = 5,
  nearest = 6
};

enum class GistMutationKind : u32 {
  insert_row = 1,
  delete_row = 2,
  update_row = 3
};

struct GistExactRecheckProof {
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

struct GistOpclassDescriptor {
  std::string opclass_name;
  u64 opclass_epoch = 0;
  u64 resource_epoch = 0;
  bool registered = false;
  bool deterministic = false;
  bool immutable = false;
  bool safe = false;
  bool supports_nearest = false;
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

struct GistCompressedKey {
  std::vector<byte> bytes;
  u32 dimensions = 0;
  u32 srid = 0;
  bool deterministic = false;
  bool valid = false;
};

struct GistPickSplitResult {
  std::vector<GistCompressedKey> left;
  std::vector<GistCompressedKey> right;
  bool deterministic = false;
  bool valid = false;
};

struct GistOpclassMethods {
  std::function<bool(const GistCompressedKey&,
                     const SpatialRTreeMbr&,
                     GistPredicateStrategy)>
      consistent;
  std::function<GistCompressedKey(const std::vector<GistCompressedKey>&)> union_key;
  std::function<GistCompressedKey(const SpatialRTreeMbr&)> compress;
  std::function<SpatialRTreeMbr(const GistCompressedKey&)> decompress;
  std::function<double(const GistCompressedKey&, const GistCompressedKey&)>
      penalty;
  std::function<GistPickSplitResult(const std::vector<GistCompressedKey>&,
                                    u32)>
      picksplit;
  std::function<bool(const GistCompressedKey&, const GistCompressedKey&)> same;
  std::function<double(const GistCompressedKey&, const std::vector<double>&)>
      distance;
};

struct GistOpclassRuntime {
  GistOpclassDescriptor descriptor;
  GistOpclassMethods methods;
};

struct GistSourceRow {
  TextInvertedRowLocator locator;
  SpatialRTreeMbr key;
  std::string exact_source_recheck_evidence_ref;
};

struct GistPhysicalProvider {
  std::string artifact_kind = kGistPhysicalProviderArtifactKind;
  PageExtentSummaryFormatVersion format_version{
      kGistPhysicalProviderCurrentMajor, kGistPhysicalProviderCurrentMinor};
  std::string relation_uuid;
  std::string index_uuid;
  std::string provider_uuid;
  u64 base_generation = 0;
  u64 provider_generation = 0;
  GistOpclassDescriptor opclass;
  SpatialRTreePhysicalProvider physical_tree;
  bool opclass_required_for_runtime = true;
  bool spatial_rtree_provider_consumed = true;
  bool consistent_present = true;
  bool union_present = true;
  bool compress_present = true;
  bool decompress_present = true;
  bool penalty_present = true;
  bool picksplit_present = true;
  bool same_present = true;
  bool distance_present = true;
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
  u64 consistent_call_count = 0;
  u64 union_call_count = 0;
  u64 compress_call_count = 0;
  u64 decompress_call_count = 0;
  u64 penalty_call_count = 0;
  u64 picksplit_call_count = 0;
  u64 same_call_count = 0;
  u64 distance_call_count = 0;
  std::vector<std::string> evidence;
};

struct GistBuildRequest {
  std::string relation_uuid;
  std::string index_uuid;
  std::string provider_uuid;
  u64 base_generation = 0;
  u64 provider_generation = 0;
  SpatialRTreeDescriptor spatial_descriptor;
  SpatialRTreeSridResource srid_resource;
  GistOpclassRuntime opclass;
  GistExactRecheckProof recheck_proof;
  SpatialRTreeBuildMode build_mode = SpatialRTreeBuildMode::incremental_insert;
  u32 max_entries = kSpatialRTreeDefaultMaxEntries;
  u32 min_entries = kSpatialRTreeDefaultMinEntries;
  std::vector<GistSourceRow> rows;
};

struct GistBuildResult {
  Status status;
  DiagnosticRecord diagnostic;
  GistPhysicalProvider provider;
  bool built = false;
  bool fail_closed = true;
  bool used_spatial_rtree_provider = false;
  bool all_methods_exercised = false;

  bool ok() const { return status.ok() && built && !fail_closed; }
};

struct GistSerializeResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<byte> bytes;
  u64 checksum = 0;

  bool ok() const { return status.ok() && !bytes.empty(); }
};

struct GistOpenRequest {
  std::vector<byte> bytes;
  GistOpclassRuntime opclass;
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
  GistExactRecheckProof recheck_proof;
};

struct GistOpenResult {
  Status status;
  DiagnosticRecord diagnostic;
  GistOpenClass open_class = GistOpenClass::refused;
  GistPhysicalProvider provider;
  bool fail_closed = true;
  bool restricted_repair_required = false;
  std::vector<std::string> actions;

  bool ok() const {
    return status.ok() && open_class == GistOpenClass::current &&
           !fail_closed;
  }
};

struct GistQueryRequest {
  GistPhysicalProvider provider;
  GistOpclassRuntime opclass;
  GistPredicateStrategy strategy = GistPredicateStrategy::intersects;
  SpatialRTreeMbr query_mbr;
  std::vector<double> query_point;
  u32 top_k = 0;
  GistExactRecheckProof recheck_proof;
  bool opclass_epoch_current = true;
  bool resource_epoch_current = true;
  bool descriptor_epoch_current = true;
  bool srid_resource_epoch_current = true;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  bool contract_only_fallback = false;
  bool provider_only_fallback = false;
};

struct GistCandidate {
  TextInvertedRowLocator locator;
  SpatialRTreeMbr key;
  double distance = 0.0;
  bool from_spatial_rtree_provider = true;
  bool opclass_consistent = false;
  bool exact_source_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool final_row_admitted = false;
  std::string source_recheck_evidence_ref;
};

struct GistQueryResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = true;
  std::vector<GistCandidate> candidates;
  u64 nodes_visited = 0;
  u64 entries_examined = 0;
  bool spatial_rtree_provider_used = false;
  bool priority_queue_used = false;
  bool opclass_consistent_used = false;
  bool opclass_distance_used = false;
  bool candidate_rows_only = true;
  bool final_rows_authorized = false;
  bool exact_source_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct GistMutation {
  GistMutationKind kind = GistMutationKind::insert_row;
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
  GistSourceRow before_row;
  bool after_row_present = false;
  GistSourceRow after_row;
  GistExactRecheckProof recheck_proof;
};

struct GistMutationResult {
  Status status;
  DiagnosticRecord diagnostic;
  GistPhysicalProvider provider;
  bool applied = false;
  bool fail_closed = true;
  bool spatial_rtree_provider_used = false;
  bool split_performed = false;
  bool merge_performed = false;
  bool tombstone_written = false;
  std::vector<std::string> actions;

  bool ok() const { return status.ok() && applied && !fail_closed; }
};

GistOpclassRuntime MakeSpatialMbrGistOpclass(u64 opclass_epoch,
                                             u64 resource_epoch,
                                             u32 dimensions,
                                             u32 srid);
GistBuildResult BuildGistPhysicalProvider(const GistBuildRequest& request);
GistSerializeResult SerializeGistPhysicalProvider(
    const GistPhysicalProvider& provider);
GistOpenResult OpenGistPhysicalProvider(const GistOpenRequest& request);
GistQueryResult QueryGistPhysicalProvider(const GistQueryRequest& request);
GistMutationResult ApplyGistPhysicalMutation(
    const GistPhysicalProvider& provider,
    const GistOpclassRuntime& opclass,
    const GistMutation& mutation);

bool GistCompressedKeyValid(const GistCompressedKey& key,
                            const GistOpclassDescriptor& opclass);
SpatialRTreeQueryKind GistStrategyToSpatialRTreeQueryKind(
    GistPredicateStrategy strategy);
const char* GistOpenClassName(GistOpenClass open_class);
DiagnosticRecord MakeGistPhysicalProviderDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
