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

// QOW-RCP-077-BOUND-VECTOR-READ-V1
// Canonical query execution uses this engine-bound, read-only contract.  It is
// deliberately distinct from the legacy caller-corpus EngineVectorSearch API:
// no corpus rows, provider proof booleans, requested tier, donor text, opaque
// options, or request-created storage/generation identities are admitted.
enum class EngineBoundVectorReadOperationV1 : std::uint8_t {
  kUnknown = 0,
  kExactSearch,
  kAnnSearch,
  kFilteredSearch,
};

enum class EngineBoundVectorMetricV1 : std::uint8_t {
  kUnknown = 0,
  kL2Squared,
  kCosine,
  kInnerProduct,
};

enum class EngineBoundVectorPhysicalRouteV1 : std::uint8_t {
  kUnknown = 0,
  kExactScan,
  kAnnWithExactFallback,
};

struct EngineBoundVectorFilterV1 {
  bool present = false;
  std::string canonical_metadata_json;
};

struct EngineBoundVectorReadRequestV1 {
  EngineRequestContext context;
  std::string collection_uuid;
  std::string expected_descriptor_uuid;
  std::uint64_t expected_descriptor_generation = 0;
  std::string selected_alternative_uuid;
  std::string selected_provider_uuid;
  std::string selected_capability_uuid;
  std::string selected_implementation_id;
  EngineBoundVectorReadOperationV1 operation =
      EngineBoundVectorReadOperationV1::kUnknown;
  EngineBoundVectorMetricV1 metric = EngineBoundVectorMetricV1::kUnknown;
  EngineBoundVectorPhysicalRouteV1 physical_route =
      EngineBoundVectorPhysicalRouteV1::kUnknown;
  // Exact canonical finite binary32 literal payload: "[e0,e1,e2]".
  std::string bound_query_vector_literal;
  std::uint32_t top_k = 0;
  EngineBoundVectorFilterV1 filter;
  // The three engine-bound public descriptor identities in row_uuid,
  // distance, score order.  The engine revalidates their exact type cohort.
  std::vector<EngineDescriptor> output_descriptors;
  std::uint64_t maximum_scanned_row_versions = 0;
  std::uint64_t maximum_decoded_bytes = 0;
  std::uint64_t maximum_output_rows = 0;
  std::uint64_t maximum_memory_bytes = 0;
  std::function<bool()> cancellation_requested;
};

struct EngineBoundVectorRowV1 {
  std::string row_uuid;
  double distance = 0.0;
  double score = 0.0;
  std::string encoded_distance;
  std::string encoded_score;
};

struct EngineBoundVectorReadResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  MgaRelationStorageDescriptor relation_descriptor;
  std::vector<EngineDescriptor> output_descriptors;
  std::vector<EngineBoundVectorRowV1> rows;
  std::uint64_t current_relation_base_generation = 0;
  std::uint64_t scanned_row_version_count = 0;
  std::uint64_t decoded_byte_count = 0;
  std::uint64_t visible_base_row_count = 0;
  std::uint64_t filtered_base_row_count = 0;
  std::uint64_t scored_base_row_count = 0;
  std::uint64_t result_byte_count = 0;
  bool ann_carrier_loaded = false;
  bool ann_candidate_hint_selected = false;
  bool exact_fallback_selected = false;
  bool full_base_exact_recheck_complete = false;
  bool base_row_mga_recheck_complete = false;
  bool security_recheck_complete = false;
  bool execution_resource_acquired = false;
  std::uint64_t cleanup_count = 0;
  std::vector<EngineEvidenceReference> evidence;
};

// Exact persisted vector-storage admission shared by canonical planning and
// the provider boundary. It is catalog-only and owns no MGA row visibility or
// transaction-finality decisions.
bool ExactBoundVectorStorageDescriptorV1(
    const MgaRelationStorageDescriptor& descriptor,
    std::string_view collection_uuid);

EngineBoundVectorReadResultV1 EngineBoundVectorReadV1(
    const EngineBoundVectorReadRequestV1& request);

struct EngineVectorCollectionOperationRequest : EngineApiRequest {};
struct EngineVectorCollectionOperationResult : EngineApiResult {};
EngineVectorCollectionOperationResult EngineVectorCollectionOperation(
    const EngineVectorCollectionOperationRequest& request);

struct EngineVectorWriteRequest : EngineApiRequest {};
struct EngineVectorWriteResult : EngineApiResult {};
EngineVectorWriteResult EngineVectorWrite(const EngineVectorWriteRequest& request);

}  // namespace scratchbird::engine::internal_api
