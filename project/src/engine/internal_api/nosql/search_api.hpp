// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "mga_relation_store/mga_relation_descriptor.hpp"
#include "nosql/nosql_physical_provider_contract.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
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

// QOW-RCP-078-BOUND-SEARCH-READ-V1
// Canonical query execution uses this engine-bound, read-only contract. It is
// deliberately distinct from the legacy caller-corpus EngineSearchQuery API:
// no corpus rows, raw donor/provider query, proof booleans, or caller-created
// storage/generation identities are admitted.
enum class EngineBoundSearchOperationV1 : std::uint8_t {
  kUnknown = 0,
  kTerms,
  kPhrase,
  kFuzzy,
};

enum class EngineBoundSearchPhysicalRouteV1 : std::uint8_t {
  kUnknown = 0,
  kExactCorpusScan,
  kSegmentWithExactFallback,
};

struct EngineBoundSearchFilterV1 {
  bool present = false;
  std::string category_text;
};

struct EngineBoundSearchReadRequestV1 {
  EngineRequestContext context;
  std::string collection_uuid;
  std::string expected_descriptor_uuid;
  std::uint64_t expected_descriptor_generation = 0;
  std::string selected_alternative_uuid;
  std::string selected_provider_uuid;
  std::string selected_capability_uuid;
  std::string selected_implementation_id;
  EngineBoundSearchOperationV1 operation =
      EngineBoundSearchOperationV1::kUnknown;
  EngineBoundSearchPhysicalRouteV1 physical_route =
      EngineBoundSearchPhysicalRouteV1::kUnknown;
  std::string bound_query_text;
  std::uint32_t fuzzy_maximum_edits = 0;
  std::uint32_t top_k = 0;
  EngineBoundSearchFilterV1 filter;
  std::string analyzer_uuid;
  std::uint64_t analyzer_generation = 0;
  std::string analyzer_pipeline_sha256;
  // document_uuid, analyzer_uuid, analyzer_generation, score, rank.
  std::vector<EngineDescriptor> output_descriptors;
  std::uint64_t maximum_scanned_row_versions = 0;
  std::uint64_t maximum_decoded_bytes = 0;
  std::uint64_t maximum_tokens = 0;
  std::uint64_t maximum_positions = 0;
  std::uint64_t maximum_candidates = 0;
  std::uint64_t maximum_scored_rows = 0;
  std::uint64_t maximum_output_rows = 0;
  std::uint64_t maximum_memory_bytes = 0;
  std::function<bool()> cancellation_requested;
};

struct EngineBoundSearchRowV1 {
  std::string document_uuid;
  std::string analyzer_uuid;
  std::uint64_t analyzer_generation = 0;
  double score = 0.0;
  std::uint64_t rank = 0;
  std::string encoded_score;
};

struct EngineBoundSearchReadResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  MgaRelationStorageDescriptor relation_descriptor;
  std::vector<EngineDescriptor> output_descriptors;
  std::vector<EngineBoundSearchRowV1> rows;
  std::uint64_t current_relation_base_generation = 0;
  std::uint64_t scanned_row_version_count = 0;
  std::uint64_t decoded_byte_count = 0;
  std::uint64_t visible_base_row_count = 0;
  std::uint64_t filtered_base_row_count = 0;
  std::uint64_t analyzed_token_count = 0;
  std::uint64_t analyzed_position_count = 0;
  std::uint64_t scored_base_row_count = 0;
  std::uint64_t result_byte_count = 0;
  bool segment_carrier_loaded = false;
  bool segment_candidate_hint_selected = false;
  bool exact_fallback_selected = false;
  bool full_corpus_exact_recheck_complete = false;
  bool base_row_mga_recheck_complete = false;
  bool security_recheck_complete = false;
  bool execution_resource_acquired = false;
  std::uint64_t cleanup_count = 0;
  std::vector<EngineEvidenceReference> evidence;
};

// Exact persisted search-storage admission shared by canonical planning and
// the provider boundary. It is catalog-only and owns no MGA row visibility or
// transaction-finality decisions.
bool ExactBoundSearchStorageDescriptorV1(
    const MgaRelationStorageDescriptor& descriptor,
    std::string_view collection_uuid);

EngineBoundSearchReadResultV1 EngineBoundSearchReadV1(
    const EngineBoundSearchReadRequestV1& request);

}  // namespace scratchbird::engine::internal_api
