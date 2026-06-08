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

inline constexpr const char* kSearchPhysicalProofMissing =
    "SB_SEARCH_PHYSICAL_PROOF_MISSING";
inline constexpr const char* kSearchMutableBufferProofMissing =
    "SB_SEARCH_MUTABLE_BUFFER_PROOF_MISSING";
inline constexpr const char* kSearchSealedInvertedSegmentProofMissing =
    "SB_SEARCH_SEALED_INVERTED_SEGMENT_PROOF_MISSING";
inline constexpr const char* kSearchBm25StatisticsProofMissing =
    "SB_SEARCH_BM25_STATISTICS_PROOF_MISSING";
inline constexpr const char* kSearchSparseVectorScoreProofMissing =
    "SB_SEARCH_SPARSE_VECTOR_SCORE_PROOF_MISSING";
inline constexpr const char* kSearchMaxScoreWandTopKProofMissing =
    "SB_SEARCH_MAXSCORE_WAND_TOPK_PROOF_MISSING";
inline constexpr const char* kSearchBloomNegativePruningProofMissing =
    "SB_SEARCH_BLOOM_NEGATIVE_PRUNING_PROOF_MISSING";
inline constexpr const char* kSearchQueryTextRequired =
    "SB_SEARCH_QUERY_TEXT_REQUIRED";

struct EngineSearchDocumentInput {
  std::string document_uuid;
  std::string text;
  bool sealed_segment = true;
};

struct EngineSearchPhysicalProof {
  EngineNoSqlPhysicalProviderContract provider_contract;
  bool proof_supplied = false;
  bool mutable_buffer_proof = false;
  bool sealed_inverted_segment_proof = false;
  bool bm25_statistics_proof = false;
  bool sparse_vector_score_proof = false;
  bool maxscore_wand_topk_proof = false;
  bool bloom_negative_pruning_proof = false;
};

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_SEARCH_API
struct EngineSearchQueryRequest : EngineApiRequest {
  std::string query_text;
  EngineApiU64 top_k = 0;
  std::vector<EngineSearchDocumentInput> document_corpus;
  EngineSearchPhysicalProof physical_proof;
};
struct EngineSearchQueryResult : EngineApiResult {};
EngineSearchQueryResult EngineSearchQuery(const EngineSearchQueryRequest& request);

}  // namespace scratchbird::engine::internal_api
