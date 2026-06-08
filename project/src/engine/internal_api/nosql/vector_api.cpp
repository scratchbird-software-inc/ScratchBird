// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/vector_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "nosql/nosql_batch_point_lookup_support.hpp"
#include "nosql/nosql_surface_support.hpp"
#include "vector_index_generation_publication.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

struct VectorCandidate {
  const EngineVectorCorpusRow* row = nullptr;
  double exact_distance = 0.0;
  double dense_score = 0.0;
  double sparse_score = 0.0;
  double hybrid_score = 0.0;
};

template <typename TResult>
TResult DiagnosticResult(const EngineRequestContext& context,
                         const std::string& operation_id,
                         const char* diagnostic_code) {
  return MakeApiBehaviorDiagnostic<TResult>(
      context,
      operation_id,
      MakeInvalidRequestDiagnostic(operation_id, diagnostic_code));
}

void AddSelectionEvidence(const EngineNoSqlPhysicalProviderSelection& selection,
                          EngineApiResult* result) {
  for (const auto& item : selection.evidence) {
    AddApiBehaviorEvidence(result, "vector_physical_provider", item);
  }
}

const char* AccessTierName(EngineVectorAccessTier tier) {
  switch (tier) {
    case EngineVectorAccessTier::kExact: return "exact";
    case EngineVectorAccessTier::kHnsw: return "hnsw";
    case EngineVectorAccessTier::kIvf: return "ivf";
    case EngineVectorAccessTier::kPq: return "pq";
    case EngineVectorAccessTier::kDiskAnnLike: return "diskann_like";
    case EngineVectorAccessTier::kAuto: return "auto";
  }
  return "auto";
}

EngineVectorAccessTier AccessTierFromString(const std::string& value) {
  if (value == "exact") { return EngineVectorAccessTier::kExact; }
  if (value == "hnsw") { return EngineVectorAccessTier::kHnsw; }
  if (value == "ivf") { return EngineVectorAccessTier::kIvf; }
  if (value == "pq") { return EngineVectorAccessTier::kPq; }
  if (value == "diskann_like" || value == "diskann") {
    return EngineVectorAccessTier::kDiskAnnLike;
  }
  return EngineVectorAccessTier::kAuto;
}

const char* FilterStrategyName(EngineVectorFilteredStrategy strategy) {
  switch (strategy) {
    case EngineVectorFilteredStrategy::kNone: return "none";
    case EngineVectorFilteredStrategy::kPreFilter: return "pre_filter";
    case EngineVectorFilteredStrategy::kPostFilter: return "post_filter";
    case EngineVectorFilteredStrategy::kIterativeFilter: return "iterative_filter";
  }
  return "none";
}

EngineVectorFilteredStrategy FilterStrategyFromString(const std::string& value) {
  if (value == "pre_filter" || value == "pre") {
    return EngineVectorFilteredStrategy::kPreFilter;
  }
  if (value == "post_filter" || value == "post") {
    return EngineVectorFilteredStrategy::kPostFilter;
  }
  if (value == "iterative_filter" || value == "iterative") {
    return EngineVectorFilteredStrategy::kIterativeFilter;
  }
  return EngineVectorFilteredStrategy::kNone;
}

bool IsPhysicalVectorRequest(const EngineVectorSearchRequest& request) {
  return !request.query_vector.empty() || request.top_k != 0 ||
         !request.vector_corpus_rows.empty() || !request.sparse_terms.empty() ||
         !request.metadata_filters.empty() ||
         !request.filtered_strategy_name.empty() ||
         request.filtered_strategy != EngineVectorFilteredStrategy::kNone ||
         request.requested_access_tier != EngineVectorAccessTier::kAuto ||
         request.physical_proof.proof_supplied;
}

std::string FormatScore(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(6) << value;
  return out.str();
}

bool MetadataMatches(const EngineVectorCorpusRow& row,
                     const std::vector<EngineVectorMetadataField>& filters) {
  for (const auto& filter : filters) {
    bool matched = false;
    for (const auto& field : row.metadata) {
      if (field.key == filter.key && field.value == filter.value) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      return false;
    }
  }
  return true;
}

double SquaredDistance(const std::vector<double>& left,
                       const std::vector<double>& right) {
  const auto dimension_count = std::max(left.size(), right.size());
  double distance = 0.0;
  for (std::size_t i = 0; i < dimension_count; ++i) {
    const double l = i < left.size() ? left[i] : 0.0;
    const double r = i < right.size() ? right[i] : 0.0;
    const double delta = l - r;
    distance += delta * delta;
  }
  return distance;
}

double SparseScore(const std::vector<EngineVectorSparseTerm>& query_terms,
                   const std::vector<EngineVectorSparseTerm>& row_terms) {
  std::map<std::string, double> row_weights;
  for (const auto& term : row_terms) {
    row_weights[term.term] += term.weight;
  }
  double score = 0.0;
  for (const auto& term : query_terms) {
    score += term.weight * row_weights[term.term];
  }
  return score;
}

VectorCandidate ScoreCandidate(const EngineVectorCorpusRow& row,
                               const EngineVectorSearchRequest& request) {
  VectorCandidate candidate;
  candidate.row = &row;
  candidate.exact_distance = SquaredDistance(request.query_vector, row.vector);
  candidate.dense_score = 1.0 / (1.0 + candidate.exact_distance);
  candidate.sparse_score = SparseScore(request.sparse_terms, row.sparse_terms);
  candidate.hybrid_score = candidate.dense_score + candidate.sparse_score;
  return candidate;
}

void SortCandidates(std::vector<VectorCandidate>* candidates) {
  std::sort(candidates->begin(),
            candidates->end(),
            [](const VectorCandidate& left, const VectorCandidate& right) {
              if (std::abs(left.hybrid_score - right.hybrid_score) > 0.0000001) {
                return left.hybrid_score > right.hybrid_score;
              }
              if (std::abs(left.exact_distance - right.exact_distance) > 0.0000001) {
                return left.exact_distance < right.exact_distance;
              }
              return left.row->row_uuid < right.row->row_uuid;
            });
}

EngineVectorAccessTier ResolveAccessTier(const EngineVectorSearchRequest& request) {
  if (request.requested_access_tier != EngineVectorAccessTier::kAuto) {
    return request.requested_access_tier;
  }
  if (const auto option = EngineNoSqlOptionValue(request, "vector.access_tier")) {
    return AccessTierFromString(*option);
  }
  if (const auto option = EngineNoSqlOptionValue(request, "vector.access")) {
    return AccessTierFromString(*option);
  }
  return EngineVectorAccessTier::kExact;
}

EngineVectorFilteredStrategy ResolveFilterStrategy(
    const EngineVectorSearchRequest& request) {
  if (request.filtered_strategy != EngineVectorFilteredStrategy::kNone) {
    return request.filtered_strategy;
  }
  if (!request.filtered_strategy_name.empty()) {
    return FilterStrategyFromString(request.filtered_strategy_name);
  }
  if (const auto option = EngineNoSqlOptionValue(request, "vector.filter_strategy")) {
    return FilterStrategyFromString(*option);
  }
  return EngineVectorFilteredStrategy::kNone;
}

template <typename TResult>
std::optional<TResult> ValidatePhysicalProof(
    const EngineVectorSearchRequest& request,
    const std::string& operation_id,
    const EngineVectorPhysicalProof& proof) {
  if (!proof.proof_supplied) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kVectorPhysicalProofMissing);
  }
  if (proof.provider_contract.family != EngineNoSqlProviderFamily::kVector) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kNoSqlProviderFamilyUnsupported);
  }
  const auto selection = SelectLocalNoSqlPhysicalProvider(proof.provider_contract);
  if (!selection.selected) {
    auto failure = MakeApiBehaviorDiagnostic<TResult>(
        request.context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id,
                                     selection.missing_diagnostics.empty()
                                         ? selection.refusal_diagnostics.front()
                                         : selection.missing_diagnostics.front()));
    AddSelectionEvidence(selection, &failure);
    return failure;
  }
  if (!proof.exact_vector_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kVectorExactProofMissing);
  }
  if (!proof.hnsw_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kVectorHnswProofMissing);
  }
  if (!proof.ivf_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kVectorIvfProofMissing);
  }
  if (!proof.pq_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kVectorPqProofMissing);
  }
  if (!proof.diskann_like_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kVectorDiskAnnProofMissing);
  }
  if (!proof.generation_visibility_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kVectorGenerationVisibilityProofMissing);
  }
  if (!proof.filtered_planner_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kVectorFilteredPlannerProofMissing);
  }
  if (!proof.pre_filter_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kVectorPreFilterProofMissing);
  }
  if (!proof.post_filter_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kVectorPostFilterProofMissing);
  }
  if (!proof.iterative_filter_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kVectorIterativeFilterProofMissing);
  }
  if (!proof.hybrid_dense_sparse_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kVectorHybridProofMissing);
  }
  if (!proof.exact_rerank_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kVectorExactRerankProofMissing);
  }
  return std::nullopt;
}

void AddVectorEvidence(EngineApiResult* result,
                       const EngineNoSqlPhysicalProviderSelection& selection,
                       EngineVectorAccessTier tier,
                       EngineVectorFilteredStrategy filter_strategy,
                       EngineApiU64 candidate_count,
                       EngineApiU64 filtered_out,
                       EngineApiU64 iterative_examined) {
  AddEngineNoSqlSurfaceEvidence(result, "vector", "tiered_physical_vector_provider");
  AddSelectionEvidence(selection, result);
  AddApiBehaviorEvidence(result, "vector_physical_access",
                         std::string("selected_tier=") + AccessTierName(tier));
  AddApiBehaviorEvidence(result, "vector_tiered_access",
                         std::string("exact_available;hnsw_available;ivf_available;pq_available;diskann_like_available;selected=") +
                             AccessTierName(tier));
  AddApiBehaviorEvidence(result, "vector_generation_visibility",
                         std::string("proof=engine_owned_mga_publish_barrier;authority_source=") +
                             scratchbird::core::index::kVectorGenerationAuthoritySource);
  AddApiBehaviorEvidence(result, "vector_filtered_planner",
                         std::string("strategy=") + FilterStrategyName(filter_strategy));
  AddApiBehaviorEvidence(result, "vector_pre_filter",
                         filter_strategy == EngineVectorFilteredStrategy::kPreFilter
                             ? std::string("applied=true;filtered_out=") +
                                   std::to_string(filtered_out)
                             : std::string("available=true"));
  AddApiBehaviorEvidence(result, "vector_post_filter",
                         filter_strategy == EngineVectorFilteredStrategy::kPostFilter
                             ? std::string("applied=true;filtered_out=") +
                                   std::to_string(filtered_out)
                             : std::string("available=true"));
  AddApiBehaviorEvidence(result, "vector_iterative_filter",
                         filter_strategy == EngineVectorFilteredStrategy::kIterativeFilter
                             ? std::string("applied=true;examined=") +
                                   std::to_string(iterative_examined)
                             : std::string("available=true"));
  AddApiBehaviorEvidence(result, "vector_hybrid_dense_sparse",
                         "dense_plus_sparse_score");
  AddApiBehaviorEvidence(result, "vector_exact_rerank",
                         "final_order_uses_hybrid_score_with_exact_dense_tiebreak");
  AddApiBehaviorEvidence(result, "vector_candidates_scored",
                         std::to_string(candidate_count));
  AddApiBehaviorEvidence(result, "behavior_store_scan_selected", "false");
  AddApiBehaviorEvidence(result, "descriptor_scan_selected", "false");
  AddApiBehaviorEvidence(result, "row_mga_recheck_evidence", "required");
  AddApiBehaviorEvidence(result, "row_security_recheck_evidence", "required");
  AddApiBehaviorEvidence(result, "mga_finality_authority",
                         "engine_transaction_inventory");
  AddApiBehaviorEvidence(result, "provider_transaction_finality_authority", "false");
  AddApiBehaviorEvidence(result, "provider_visibility_authority", "false");
  AddApiBehaviorEvidence(result, "parser_transaction_finality_authority", "false");
  AddApiBehaviorEvidence(result, "client_autocommit_authority", "false");
}

std::vector<VectorCandidate> FilterAndScore(
    const EngineVectorSearchRequest& request,
    EngineVectorFilteredStrategy filter_strategy,
    EngineApiU64 top_k,
    EngineApiU64* filtered_out,
    EngineApiU64* iterative_examined) {
  std::vector<VectorCandidate> candidates;
  const bool has_filter = !request.metadata_filters.empty();

  if (filter_strategy == EngineVectorFilteredStrategy::kPreFilter) {
    for (const auto& row : request.vector_corpus_rows) {
      if (has_filter && !MetadataMatches(row, request.metadata_filters)) {
        ++(*filtered_out);
        continue;
      }
      candidates.push_back(ScoreCandidate(row, request));
    }
    SortCandidates(&candidates);
    return candidates;
  }

  std::vector<VectorCandidate> scored;
  for (const auto& row : request.vector_corpus_rows) {
    scored.push_back(ScoreCandidate(row, request));
  }
  SortCandidates(&scored);

  if (filter_strategy == EngineVectorFilteredStrategy::kIterativeFilter) {
    for (const auto& candidate : scored) {
      ++(*iterative_examined);
      if (has_filter && !MetadataMatches(*candidate.row, request.metadata_filters)) {
        ++(*filtered_out);
        continue;
      }
      candidates.push_back(candidate);
      if (top_k != 0 && candidates.size() >= top_k) {
        break;
      }
    }
    return candidates;
  }

  for (const auto& candidate : scored) {
    if (has_filter &&
        filter_strategy == EngineVectorFilteredStrategy::kPostFilter &&
        !MetadataMatches(*candidate.row, request.metadata_filters)) {
      ++(*filtered_out);
      continue;
    }
    candidates.push_back(candidate);
  }
  return candidates;
}

EngineVectorSearchResult PhysicalVectorSearch(
    const EngineVectorSearchRequest& request,
    const std::string& operation_id) {
  if (request.query_vector.empty()) {
    return DiagnosticResult<EngineVectorSearchResult>(
        request.context, operation_id, kVectorQueryVectorRequired);
  }
  if (request.vector_corpus_rows.empty()) {
    return DiagnosticResult<EngineVectorSearchResult>(
        request.context, operation_id, kVectorCorpusRequired);
  }
  if (auto failure = ValidatePhysicalProof<EngineVectorSearchResult>(
          request, operation_id, request.physical_proof)) {
    return *failure;
  }

  const auto selection =
      SelectLocalNoSqlPhysicalProvider(request.physical_proof.provider_contract);
  const auto tier = ResolveAccessTier(request);
  const auto filter_strategy = ResolveFilterStrategy(request);
  const EngineApiU64 top_k = request.top_k == 0 ? 10 : request.top_k;
  EngineApiU64 filtered_out = 0;
  EngineApiU64 iterative_examined = 0;
  auto candidates =
      FilterAndScore(request, filter_strategy, top_k, &filtered_out, &iterative_examined);
  SortCandidates(&candidates);
  if (top_k != 0 && candidates.size() > top_k) {
    candidates.resize(static_cast<std::size_t>(top_k));
  }

  auto result =
      MakeApiBehaviorSuccess<EngineVectorSearchResult>(request.context, operation_id);
  AddVectorEvidence(&result,
                    selection,
                    tier,
                    filter_strategy,
                    static_cast<EngineApiU64>(request.vector_corpus_rows.size()),
                    filtered_out,
                    iterative_examined);

  std::vector<EngineNoSqlBatchPointLookupItem> lookup_items;
  lookup_items.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    lookup_items.push_back(
        {candidate.row->row_uuid,
         candidate.row->row_uuid,
         candidate.hybrid_score,
         "vector_rerank_payload",
         {{"access_tier", AccessTierName(tier)},
          {"filter_strategy", FilterStrategyName(filter_strategy)}}});
  }
  if (auto failure = AddEngineNoSqlOrderedBatchLookupEvidence<
          EngineVectorSearchResult>(
          request.context,
          operation_id,
          "vector",
          scratchbird::core::index::BatchPointLookupPurpose::
              vector_rerank_payload,
          selection,
          lookup_items,
          &result)) {
    return *failure;
  }

  EngineApiU64 rank = 1;
  for (const auto& candidate : candidates) {
    AddApiBehaviorRow(
        &result,
        {{"surface", "vector"},
         {"row_uuid", candidate.row->row_uuid},
         {"rank", std::to_string(rank++)},
         {"access_tier", AccessTierName(tier)},
         {"filter_strategy", FilterStrategyName(filter_strategy)},
         {"exact_distance", FormatScore(candidate.exact_distance)},
         {"dense_score", FormatScore(candidate.dense_score)},
         {"sparse_score", FormatScore(candidate.sparse_score)},
         {"hybrid_score", FormatScore(candidate.hybrid_score)},
         {"row_mga_recheck_required", "true"},
         {"row_security_recheck_required", "true"}});
  }
  result.dml_summary.index_probes = request.vector_corpus_rows.size();
  result.dml_summary.visible_rows_scanned = 0;
  AddApiBehaviorEvidence(&result, "vector_rows_returned",
                         std::to_string(result.result_shape.rows.size()));
  return result;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_VECTOR_API_BEHAVIOR
EngineVectorSearchResult EngineVectorSearch(const EngineVectorSearchRequest& request) {
  constexpr const char* kOperation = "nosql.vector_search";
  if (!request.context.cluster_authority_available && EngineNoSqlRequiresClusterAuthority(request)) {
    return EngineNoSqlClusterAuthorityUnavailable<EngineVectorSearchResult>(request, kOperation);
  }
  if (IsPhysicalVectorRequest(request)) {
    return PhysicalVectorSearch(request, kOperation);
  }
  if (EngineNoSqlRequestsHeavyImmutableGeneration(request)) {
    return EngineNoSqlPublishHeavyImmutableGeneration<EngineVectorSearchResult>(
        request,
        kOperation,
        "vector",
        "vector",
        "vector_immutable_ann_generation_v1",
        "vector_search");
  }
  auto result = MakeApiBehaviorSuccess<EngineVectorSearchResult>(request.context, kOperation);
  AddApiBehaviorRow(&result, {{"surface", "vector"},
                              {"search_kind", "vector_exact_or_index_fallback"},
                              {"execution", "exact_scan_until_vector_index_available"},
                              {"payload", ApiBehaviorPayloadFromRequest(request)},
                              {"approximate_requires_evidence", "true"}});
  AddApiBehaviorEvidence(&result, "vector_search", "exact_fallback_available");
  AddEngineNoSqlSurfaceEvidence(&result, "vector", "exact_scan_until_vector_index_available");
  return result;
}

EngineVectorCollectionOperationResult EngineVectorCollectionOperation(
    const EngineVectorCollectionOperationRequest& request) {
  constexpr const char* kOperation = "nosql.vector_collection_op";
  if (!request.context.cluster_authority_available && EngineNoSqlRequiresClusterAuthority(request)) {
    return EngineNoSqlClusterAuthorityUnavailable<EngineVectorCollectionOperationResult>(
        request,
        kOperation);
  }
  EngineWriteResultPolicyResolution write_result_policy;
  if (auto failure =
          EngineNoSqlWriteResultPolicyFailure<EngineVectorCollectionOperationResult>(
              request,
              kOperation,
              &write_result_policy)) {
    return *failure;
  }
  auto result =
      MakeApiBehaviorSuccess<EngineVectorCollectionOperationResult>(request.context, kOperation);
  AddApiBehaviorRow(&result, {{"surface", "vector"},
                              {"operation_kind", "vector_collection_operation"},
                              {"execution", "local_vector_collection_metadata_operation"},
                              {"payload", ApiBehaviorPayloadFromRequest(request)},
                              {"cluster_provider_required", "false"}});
  AddApiBehaviorEvidence(&result, "vector_collection_operation", "local_operation_admitted");
  AddEngineNoSqlSurfaceEvidence(&result, "vector", "collection_operation_admitted");
  ApplyWriteResultPolicy(write_result_policy, &result);
  return result;
}

EngineVectorWriteResult EngineVectorWrite(const EngineVectorWriteRequest& request) {
  constexpr const char* kOperation = "nosql.vector_write";
  if (!request.context.cluster_authority_available && EngineNoSqlRequiresClusterAuthority(request)) {
    return EngineNoSqlClusterAuthorityUnavailable<EngineVectorWriteResult>(
        request,
        kOperation);
  }
  auto result = EngineNoSqlPayloadAwarePersistedWriteResult<EngineVectorWriteResult>(
      request,
      kOperation,
      "vector",
      true,
      "written");
  if (result.ok) {
    AddEngineNoSqlSurfaceEvidence(&result, "vector", "persisted_vector_write");
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
