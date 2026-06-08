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

inline constexpr const char* kVectorPhysicalProofMissing =
    "SB_VECTOR_PHYSICAL_PROOF_MISSING";
inline constexpr const char* kVectorQueryVectorRequired =
    "SB_VECTOR_QUERY_VECTOR_REQUIRED";
inline constexpr const char* kVectorCorpusRequired =
    "SB_VECTOR_CORPUS_REQUIRED";
inline constexpr const char* kVectorExactProofMissing =
    "SB_VECTOR_EXACT_PROOF_MISSING";
inline constexpr const char* kVectorHnswProofMissing =
    "SB_VECTOR_HNSW_PROOF_MISSING";
inline constexpr const char* kVectorIvfProofMissing =
    "SB_VECTOR_IVF_PROOF_MISSING";
inline constexpr const char* kVectorPqProofMissing =
    "SB_VECTOR_PQ_PROOF_MISSING";
inline constexpr const char* kVectorDiskAnnProofMissing =
    "SB_VECTOR_DISKANN_LIKE_PROOF_MISSING";
inline constexpr const char* kVectorGenerationVisibilityProofMissing =
    "SB_VECTOR_GENERATION_VISIBILITY_PROOF_MISSING";
inline constexpr const char* kVectorFilteredPlannerProofMissing =
    "SB_VECTOR_FILTERED_PLANNER_PROOF_MISSING";
inline constexpr const char* kVectorPreFilterProofMissing =
    "SB_VECTOR_PRE_FILTER_PROOF_MISSING";
inline constexpr const char* kVectorPostFilterProofMissing =
    "SB_VECTOR_POST_FILTER_PROOF_MISSING";
inline constexpr const char* kVectorIterativeFilterProofMissing =
    "SB_VECTOR_ITERATIVE_FILTER_PROOF_MISSING";
inline constexpr const char* kVectorHybridProofMissing =
    "SB_VECTOR_HYBRID_DENSE_SPARSE_PROOF_MISSING";
inline constexpr const char* kVectorExactRerankProofMissing =
    "SB_VECTOR_EXACT_RERANK_PROOF_MISSING";

enum class EngineVectorAccessTier {
  kAuto,
  kExact,
  kHnsw,
  kIvf,
  kPq,
  kDiskAnnLike,
};

enum class EngineVectorFilteredStrategy {
  kNone,
  kPreFilter,
  kPostFilter,
  kIterativeFilter,
};

struct EngineVectorSparseTerm {
  std::string term;
  double weight = 1.0;
};

struct EngineVectorMetadataField {
  std::string key;
  std::string value;
};

struct EngineVectorCorpusRow {
  std::string row_uuid;
  std::vector<double> vector;
  std::vector<EngineVectorSparseTerm> sparse_terms;
  std::vector<EngineVectorMetadataField> metadata;
};

struct EngineVectorPhysicalProof {
  EngineNoSqlPhysicalProviderContract provider_contract;
  bool proof_supplied = false;
  bool exact_vector_proof = false;
  bool hnsw_proof = false;
  bool ivf_proof = false;
  bool pq_proof = false;
  bool diskann_like_proof = false;
  bool generation_visibility_proof = false;
  bool filtered_planner_proof = false;
  bool pre_filter_proof = false;
  bool post_filter_proof = false;
  bool iterative_filter_proof = false;
  bool hybrid_dense_sparse_proof = false;
  bool exact_rerank_proof = false;
};

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_VECTOR_API
struct EngineVectorSearchRequest : EngineApiRequest {
  std::vector<double> query_vector;
  EngineApiU64 top_k = 0;
  std::vector<EngineVectorCorpusRow> vector_corpus_rows;
  std::vector<EngineVectorSparseTerm> sparse_terms;
  std::vector<EngineVectorMetadataField> metadata_filters;
  EngineVectorFilteredStrategy filtered_strategy =
      EngineVectorFilteredStrategy::kNone;
  std::string filtered_strategy_name;
  EngineVectorAccessTier requested_access_tier = EngineVectorAccessTier::kAuto;
  EngineVectorPhysicalProof physical_proof;
};
struct EngineVectorSearchResult : EngineApiResult {};
EngineVectorSearchResult EngineVectorSearch(const EngineVectorSearchRequest& request);

struct EngineVectorCollectionOperationRequest : EngineApiRequest {};
struct EngineVectorCollectionOperationResult : EngineApiResult {};
EngineVectorCollectionOperationResult EngineVectorCollectionOperation(
    const EngineVectorCollectionOperationRequest& request);

struct EngineVectorWriteRequest : EngineApiRequest {};
struct EngineVectorWriteResult : EngineApiResult {};
EngineVectorWriteResult EngineVectorWrite(const EngineVectorWriteRequest& request);

}  // namespace scratchbird::engine::internal_api
