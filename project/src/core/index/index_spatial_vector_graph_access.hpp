// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-SPATIAL-VECTOR-GRAPH-CLOSURE-ANCHOR

#include "index_family_registry.hpp"

namespace scratchbird::core::index {

using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class IndexSpatialPredicate : u32 {
  intersects = 1,
  contains = 2,
  within = 3,
  overlaps = 4,
  nearest = 5,
  distance_within = 6
};

enum class IndexVectorMetric : u32 {
  l2 = 1,
  inner_product = 2,
  cosine = 3,
  hamming = 4,
  jaccard = 5
};

enum class IndexVectorAlgorithm : u32 {
  flat = 1,
  binary_flat = 2,
  hnsw = 3,
  ivf_flat = 4,
  ivf_pq = 5,
  ivf_sq8 = 6,
  rhnsw_quantized = 7,
  annoy = 8,
  nsg = 9,
  diskann = 10,
  scann = 11,
  gpu_cagra = 12
};

enum class IndexGraphProfile : u32 {
  vertex_lookup = 1,
  edge_lookup = 2,
  label_property = 3,
  path_topology = 4,
  neo4j_lookup = 5,
  redis_structure = 6,
  cassandra_sai = 7,
  clickhouse_sparse = 8,
  mongodb_wildcard = 9
};

struct IndexSpatialResourceDescriptor {
  TypedUuid resource_uuid;
  u32 srid = 0;
  u32 dimensions = 2;
  u64 resource_epoch = 0;
  u64 tolerance_units = 0;
  std::string crs_name;
  std::string coordinate_order = "xy";
  bool deterministic = true;
};

struct IndexSpatialAccessRequest {
  IndexFamily family = IndexFamily::spatial;
  IndexSpatialPredicate predicate = IndexSpatialPredicate::intersects;
  IndexSpatialResourceDescriptor resource;
  std::vector<byte> query_shape_token;
  bool request_distance_order = false;
  bool reference_requires_exact_recheck = true;
};

struct IndexSpatialAccessPlan {
  Status status;
  IndexFamily family = IndexFamily::spatial;
  bool admitted = false;
  bool requires_exact_recheck = true;
  bool can_order_by_distance = false;
  bool resource_epoch_required = true;
  std::string semantic_profile_id;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && admitted; }
};

struct IndexVectorDescriptor {
  TypedUuid metric_resource_uuid;
  u32 dimensions = 0;
  u32 element_bytes = 4;
  IndexVectorMetric metric = IndexVectorMetric::l2;
  u64 metric_resource_epoch = 0;
  bool normalized = false;
  bool binary = false;
  bool deterministic_metric = true;
};

struct IndexVectorAdmissionRequest {
  IndexVectorAlgorithm algorithm = IndexVectorAlgorithm::flat;
  IndexVectorDescriptor descriptor;
  u64 training_row_count = 0;
  u32 requested_lists = 0;
  u32 requested_neighbors = 0;
  bool policy_allows_approximate = false;
  bool policy_allows_advanced_alpha = false;
  bool exact_fallback_allowed = true;
};

struct IndexVectorAdmissionDecision {
  Status status;
  IndexFamily family = IndexFamily::vector_exact;
  bool admitted = false;
  bool policy_blocked = false;
  bool requested_algorithm_policy_blocked = false;
  bool exact_fallback = false;
  bool requires_rerank = false;
  std::string semantic_profile_id;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && admitted && !policy_blocked; }
};

struct IndexGraphProfileRequest {
  IndexGraphProfile profile = IndexGraphProfile::vertex_lookup;
  std::string reference_name;
  std::string reference_surface;
  bool requires_range_order = false;
  bool requires_text_analysis = false;
  bool requires_vector_similarity = false;
  bool policy_allows_emulation = true;
};

struct IndexGraphProfileDecision {
  Status status;
  IndexFamily family = IndexFamily::graph;
  bool admitted = false;
  bool emulated = false;
  bool requires_recheck = true;
  std::string semantic_profile_id;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && admitted; }
};

IndexSpatialAccessPlan PlanSpatialIndexAccess(const IndexSpatialAccessRequest& request);
IndexVectorAdmissionDecision AdmitVectorIndex(const IndexVectorAdmissionRequest& request);
IndexGraphProfileDecision PlanGraphOrReferenceStructureIndex(const IndexGraphProfileRequest& request);
const char* IndexVectorAlgorithmName(IndexVectorAlgorithm algorithm);
DiagnosticRecord MakeIndexSpatialVectorGraphDiagnostic(Status status,
                                                       std::string diagnostic_code,
                                                       std::string message_key,
                                                       std::string detail = {});

}  // namespace scratchbird::core::index
