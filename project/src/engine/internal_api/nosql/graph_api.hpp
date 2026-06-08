// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "nosql/nosql_physical_provider_contract.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

inline constexpr const char* kGraphPhysicalProofMissing =
    "SB_GRAPH_PHYSICAL_PROOF_MISSING";
inline constexpr const char* kGraphVertexIndexProofMissing =
    "SB_GRAPH_VERTEX_INDEX_PROOF_MISSING";
inline constexpr const char* kGraphEdgeIndexProofMissing =
    "SB_GRAPH_EDGE_INDEX_PROOF_MISSING";
inline constexpr const char* kGraphAdjacencyStoreProofMissing =
    "SB_GRAPH_ADJACENCY_STORE_PROOF_MISSING";
inline constexpr const char* kGraphAdjacencyPageProofMissing =
    "SB_GRAPH_ADJACENCY_PAGE_PROOF_MISSING";
inline constexpr const char* kGraphFrontierBatchingProofMissing =
    "SB_GRAPH_FRONTIER_BATCHING_PROOF_MISSING";
inline constexpr const char* kGraphVisitedCyclePolicyProofMissing =
    "SB_GRAPH_VISITED_CYCLE_POLICY_PROOF_MISSING";
inline constexpr const char* kGraphBidirectionalSearchProofMissing =
    "SB_GRAPH_BIDIRECTIONAL_SEARCH_PROOF_MISSING";
inline constexpr const char* kGraphFusionSeedProofMissing =
    "SB_GRAPH_FUSION_SEED_PROOF_MISSING";
inline constexpr const char* kGraphSeedRequired =
    "SB_GRAPH_SEED_REQUIRED";
inline constexpr const char* kGraphVertexCorpusRequired =
    "SB_GRAPH_VERTEX_CORPUS_REQUIRED";

enum class EngineGraphTraversalDirection {
  kOutgoing,
  kIncoming,
  kBoth,
};

enum class EngineGraphCyclePolicy {
  kVisitedSet,
  kAllowCycles,
};

enum class EngineGraphFusionSourceKind {
  kNone,
  kVector,
  kSearch,
  kDocument,
  kSql,
};

struct EngineGraphProperty {
  std::string key;
  std::string value;
};

struct EngineGraphVertexInput {
  std::string vertex_id;
  std::vector<std::string> labels;
  std::vector<EngineGraphProperty> properties;
};

struct EngineGraphEdgeInput {
  std::string edge_id;
  std::string source_vertex_id;
  std::string target_vertex_id;
  std::string edge_type;
  std::vector<EngineGraphProperty> properties;
  double weight = 1.0;
};

struct EngineGraphPhysicalProof {
  EngineNoSqlPhysicalProviderContract provider_contract;
  bool proof_supplied = false;
  bool vertex_index_proof = false;
  bool edge_index_proof = false;
  bool adjacency_store_proof = false;
  bool adjacency_page_proof = false;
  bool frontier_batching_proof = false;
  bool visited_cycle_policy_proof = false;
  bool bidirectional_search_proof = false;
  bool fusion_seed_proof = false;
};

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_GRAPH_API
struct EngineGraphQueryRequest : EngineApiRequest {
  bool physical_query = false;
  std::vector<EngineGraphVertexInput> vertices;
  std::vector<EngineGraphEdgeInput> edges;
  std::vector<std::string> seed_vertex_ids;
  std::string seed_label;
  std::string seed_property_key;
  std::string seed_property_value;
  std::vector<std::string> fused_candidate_seed_vertex_ids;
  EngineGraphFusionSourceKind fusion_source_kind =
      EngineGraphFusionSourceKind::kNone;
  EngineGraphTraversalDirection direction =
      EngineGraphTraversalDirection::kOutgoing;
  std::string edge_type_filter;
  EngineApiU64 max_depth = 1;
  EngineGraphCyclePolicy cycle_policy = EngineGraphCyclePolicy::kVisitedSet;
  std::string bidirectional_start_vertex_id;
  std::string bidirectional_end_vertex_id;
  EngineGraphPhysicalProof physical_proof;
};
struct EngineGraphQueryResult : EngineApiResult {};
EngineGraphQueryResult EngineGraphQuery(const EngineGraphQueryRequest& request);

struct EngineGraphWriteRequest : EngineApiRequest {};
struct EngineGraphWriteResult : EngineApiResult {};
EngineGraphWriteResult EngineGraphWrite(const EngineGraphWriteRequest& request);

}  // namespace scratchbird::engine::internal_api
