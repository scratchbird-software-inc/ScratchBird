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
#include "datatype_catalog_manifest.hpp"
#include "datatype_document.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "nosql/nosql_batch_point_lookup_support.hpp"
#include "nosql/nosql_provider_generation_store.hpp"
#include "nosql/nosql_surface_support.hpp"
#include "security/security_model.hpp"
#include "uuid.hpp"
#include "vector_index_generation_publication.hpp"

#include <algorithm>
#include <array>
#include <cfenv>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
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

bool CanonicalBoundVectorUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-' ||
      value == "00000000-0000-0000-0000-000000000000") {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const char ch = value[index];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
      return false;
    }
  }
  return true;
}

std::string BoundVectorDescriptorField(const std::string_view descriptor,
                                       const std::string_view name) {
  std::size_t offset = 0;
  while (offset <= descriptor.size()) {
    const auto end = descriptor.find(';', offset);
    const auto field = descriptor.substr(
        offset, end == std::string_view::npos ? descriptor.size() - offset
                                              : end - offset);
    const auto equals = field.find('=');
    if (equals != std::string_view::npos && field.substr(0, equals) == name) {
      return std::string(field.substr(equals + 1));
    }
    if (end == std::string_view::npos) break;
    offset = end + 1;
  }
  return {};
}

std::string BoundVectorTypeUuid(const EngineDescriptor& descriptor) {
  return BoundVectorDescriptorField(descriptor.encoded_descriptor,
                                    "type_uuid");
}

std::string ExactBoundVectorCoreTypeUuid(const std::string_view stable_name) {
  static const auto manifest =
      scratchbird::core::datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto count = std::ranges::count_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  const auto found = std::ranges::find_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  if (count != 1 || found == manifest.manifest.descriptor_rows.end() ||
      !found->descriptor_uuid.valid()) {
    return {};
  }
  const auto descriptor_uuid = scratchbird::core::uuid::UuidToString(
      found->descriptor_uuid.value);
  const auto identity =
      scratchbird::core::datatypes::LookupDatatypeTypeCodecIdentityV1(
          "019d0000-0000-7000-8000-00000000d701",
          manifest.manifest.catalog_epoch, 1, descriptor_uuid,
          found->descriptor_epoch);
  return identity.ok ? identity.row.type_uuid : descriptor_uuid;
}

bool ExactBoundVectorDescriptorFields(
    const EngineDescriptor& descriptor,
    const std::initializer_list<std::pair<std::string_view, std::string_view>>&
        expected) {
  std::map<std::string_view, std::string_view> fields;
  const auto encoded = std::string_view(descriptor.encoded_descriptor);
  std::size_t offset = 0;
  while (offset <= encoded.size()) {
    const auto end = encoded.find(';', offset);
    const auto field = encoded.substr(
        offset, end == std::string_view::npos ? std::string_view::npos
                                              : end - offset);
    const auto equal = field.find('=');
    if (field.empty() || equal == std::string_view::npos || equal == 0 ||
        equal + 1 == field.size() ||
        !fields.emplace(field.substr(0, equal), field.substr(equal + 1)).second) {
      return false;
    }
    if (end == std::string_view::npos) break;
    offset = end + 1;
  }
  if (fields.size() != expected.size()) return false;
  return std::ranges::all_of(expected, [&](const auto& field) {
    const auto found = fields.find(field.first);
    return found != fields.end() && found->second == field.second;
  });
}

bool ExactBoundVectorStorageDescriptorImpl(
    const MgaRelationStorageDescriptor& descriptor,
    const std::string_view collection_uuid) {
  const auto vector_type_uuid = ExactBoundVectorCoreTypeUuid("dense_vector");
  const auto text_type_uuid = ExactBoundVectorCoreTypeUuid("character");
  if (descriptor.relation_uuid.canonical != collection_uuid ||
      vector_type_uuid.empty() || text_type_uuid.empty() ||
      !CanonicalBoundVectorUuid(descriptor.database_uuid.canonical) ||
      !CanonicalBoundVectorUuid(descriptor.schema_uuid.canonical) ||
      descriptor.relation_kind != "table" ||
      descriptor.storage_profile != "local_mga_rowstore_v1" ||
      descriptor.descriptor_generation == 0 ||
      !CanonicalBoundVectorUuid(descriptor.descriptor_uuid.canonical) ||
      descriptor.columns.size() != 2) {
    return false;
  }
  const auto& embedding = descriptor.columns[0];
  const auto& metadata = descriptor.columns[1];
  const bool exact = embedding.ordinal == 0 &&
         embedding.canonical_name_key == "embedding" && !embedding.nullable &&
         !embedding.generated && !embedding.identity_column &&
         embedding.storage_class == "inline_row_value" &&
         embedding.max_inline_bytes == 4096 &&
         embedding.overflow_policy == "mga_large_value_locator" &&
         embedding.charset_uuid.empty() && embedding.collation_uuid.empty() &&
         embedding.character_length == 0 &&
         embedding.value_descriptor.descriptor_kind ==
             "canonical_type_descriptor" &&
         embedding.value_descriptor.canonical_type_name == "dense_vector" &&
         ExactBoundVectorDescriptorFields(
             embedding.value_descriptor,
             {{"canonical", "dense_vector"},
              {"type_uuid", vector_type_uuid},
              {"nullable", "false"},
              {"dimension", "3"},
              {"element_type", "real32"}}) &&
         metadata.ordinal == 1 &&
         metadata.canonical_name_key == "metadata" && !metadata.nullable &&
         !metadata.generated && !metadata.identity_column &&
         metadata.storage_class == "inline_row_value" &&
         metadata.max_inline_bytes == 4096 &&
         metadata.overflow_policy == "mga_large_value_locator" &&
         metadata.charset_uuid.empty() && metadata.collation_uuid.empty() &&
         metadata.character_length == 0 &&
         metadata.value_descriptor.descriptor_kind ==
             "canonical_type_descriptor" &&
         metadata.value_descriptor.canonical_type_name == "text" &&
         CanonicalBoundVectorUuid(embedding.column_uuid.canonical) &&
         CanonicalBoundVectorUuid(metadata.column_uuid.canonical) &&
         CanonicalBoundVectorUuid(
             embedding.value_descriptor.descriptor_uuid.canonical) &&
         CanonicalBoundVectorUuid(
             metadata.value_descriptor.descriptor_uuid.canonical) &&
         metadata.value_descriptor.descriptor_uuid.canonical ==
             metadata.column_uuid.canonical &&
         embedding.column_uuid.canonical != metadata.column_uuid.canonical &&
         embedding.value_descriptor.descriptor_uuid.canonical !=
             metadata.value_descriptor.descriptor_uuid.canonical &&
         ExactBoundVectorDescriptorFields(
             metadata.value_descriptor,
             {{"canonical", "text"},
              {"type_uuid", text_type_uuid},
              {"nullable", "false"},
              {"column_uuid", metadata.column_uuid.canonical},
              {"datatype_descriptor_uuid",
               "019d0000-0000-7000-8000-00000000d718"},
              {"datatype_descriptor_generation", "1"},
              {"type_generation", "1"},
              {"codec_uuid",
               "019d0000-0000-7000-8000-00000000d71a"},
              {"codec_id", "datatype.text.utf8.v1"},
              {"codec_version", "1"},
              {"codec_generation", "1"},
              {"null_encoding", "1"}});
  return exact;
}

bool ExactBoundVectorOutputDescriptors(
    const std::vector<EngineDescriptor>& descriptors) {
  if (descriptors.size() != 3) return false;
  static constexpr std::array<std::string_view, 3> kTypes{
      "uuid", "real64", "real64"};
  std::unordered_set<std::string> descriptor_uuids;
  for (std::size_t index = 0; index < descriptors.size(); ++index) {
    const auto& descriptor = descriptors[index];
    const auto type_uuid = ExactBoundVectorCoreTypeUuid(kTypes[index]);
    if (!CanonicalBoundVectorUuid(descriptor.descriptor_uuid.canonical) ||
        !descriptor_uuids.insert(descriptor.descriptor_uuid.canonical).second ||
        descriptor.descriptor_kind != "scalar" || type_uuid.empty() ||
        descriptor.canonical_type_name != kTypes[index] ||
        !ExactBoundVectorDescriptorFields(
            descriptor,
            {{"type_uuid", type_uuid}, {"nullability", "non_null"}})) {
      return false;
    }
  }
  return true;
}

bool ParseCanonicalBoundVectorReal32(const std::string_view text,
                                     float* value) {
  if (value == nullptr || text.empty() || text.front() == '+' ||
      text.find('E') != std::string_view::npos) {
    return false;
  }
  float parsed = 0.0F;
  const auto conversion =
      std::from_chars(text.data(), text.data() + text.size(), parsed,
                      std::chars_format::general);
  if (conversion.ec != std::errc{} ||
      conversion.ptr != text.data() + text.size() || !std::isfinite(parsed)) {
    return false;
  }
  if (parsed == 0.0F) parsed = 0.0F;
  std::array<char, 64> canonical{};
  const auto rendered = std::to_chars(canonical.data(),
                                      canonical.data() + canonical.size(),
                                      parsed, std::chars_format::general);
  if (rendered.ec != std::errc{} ||
      text != std::string_view(canonical.data(),
                               static_cast<std::size_t>(rendered.ptr -
                                                        canonical.data()))) {
    return false;
  }
  *value = parsed;
  return true;
}

bool ParseCanonicalBoundVectorLiteral(
    const std::string_view literal, std::array<float, 3>* vector) {
  if (vector == nullptr || literal.size() < 7 || literal.front() != '[' ||
      literal.back() != ']' || literal.find_first_of(" \t\r\n") !=
                                     std::string_view::npos) {
    return false;
  }
  const auto payload = literal.substr(1, literal.size() - 2);
  std::size_t offset = 0;
  for (std::size_t ordinal = 0; ordinal < vector->size(); ++ordinal) {
    const auto comma = payload.find(',', offset);
    if ((ordinal + 1 < vector->size() && comma == std::string_view::npos) ||
        (ordinal + 1 == vector->size() && comma != std::string_view::npos)) {
      return false;
    }
    const auto end = comma == std::string_view::npos ? payload.size() : comma;
    if (!ParseCanonicalBoundVectorReal32(payload.substr(offset, end - offset),
                                         &(*vector)[ordinal])) {
      return false;
    }
    offset = end + 1;
  }
  return offset == payload.size() + 1;
}

bool CanonicalBoundVectorMetadata(const std::string& value) {
  scratchbird::core::datatypes::DocumentCanonicalizationRequest request;
  request.type_id =
      scratchbird::core::datatypes::CanonicalTypeId::json_document;
  request.encoded_value = value;
  const auto canonical =
      scratchbird::core::datatypes::CanonicalizeDocumentValue(request);
  return canonical.ok() && canonical.canonical_value == value &&
         !value.empty() && value.front() == '{' && value.back() == '}';
}

bool CheckedBoundVectorAdd(const std::uint64_t left,
                           const std::uint64_t right,
                           std::uint64_t* result) {
  if (result == nullptr ||
      right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  *result = left + right;
  return true;
}

bool CheckedBoundVectorMultiply(const std::uint64_t left,
                                const std::uint64_t right,
                                std::uint64_t* result) {
  if (result == nullptr ||
      (left != 0 &&
       right > std::numeric_limits<std::uint64_t>::max() / left)) {
    return false;
  }
  *result = left * right;
  return true;
}

class BoundVectorFloatingEnvironment final {
 public:
  BoundVectorFloatingEnvironment() {
    active_ = std::fegetenv(&saved_) == 0;
    if (active_) {
      std::feclearexcept(FE_ALL_EXCEPT);
      if (std::fesetround(FE_TONEAREST) != 0) active_ = false;
    }
  }

  ~BoundVectorFloatingEnvironment() {
    if (active_) std::fesetenv(&saved_);
  }

  bool active() const { return active_; }

 private:
  std::fenv_t saved_{};
  bool active_ = false;
};

std::string CanonicalBoundVectorReal64(double value) {
  if (!std::isfinite(value)) return {};
  if (value == 0.0) value = 0.0;
  std::array<char, 128> encoded{};
  const auto rendered = std::to_chars(encoded.data(),
                                      encoded.data() + encoded.size(), value,
                                      std::chars_format::general);
  if (rendered.ec != std::errc{}) return {};
  return std::string(encoded.data(), rendered.ptr);
}

const char* BoundVectorMetricName(const EngineBoundVectorMetricV1 metric) {
  switch (metric) {
    case EngineBoundVectorMetricV1::kL2Squared: return "L2_SQUARED";
    case EngineBoundVectorMetricV1::kCosine: return "COSINE";
    case EngineBoundVectorMetricV1::kInnerProduct: return "INNER_PRODUCT";
    case EngineBoundVectorMetricV1::kUnknown: break;
  }
  return "UNKNOWN";
}

struct BoundVectorScoredRow {
  std::string row_uuid;
  double distance = 0.0;
  double score = 0.0;
  std::string encoded_distance;
  std::string encoded_score;
};

bool ScoreBoundVectorRow(const std::array<float, 3>& query,
                         const std::array<float, 3>& stored,
                         const EngineBoundVectorMetricV1 metric,
                         BoundVectorScoredRow* row,
                         bool* cosine_zero_norm) {
  if (row == nullptr || cosine_zero_norm == nullptr) return false;
  *cosine_zero_norm = false;
  volatile double dot = 0.0;
  volatile double query_norm = 0.0;
  volatile double stored_norm = 0.0;
  volatile double l2 = 0.0;
  for (std::size_t ordinal = 0; ordinal < query.size(); ++ordinal) {
    volatile double q = static_cast<double>(query[ordinal]);
    volatile double x = static_cast<double>(stored[ordinal]);
    volatile double product = q * x;
    dot = dot + product;
    volatile double query_square = q * q;
    query_norm = query_norm + query_square;
    volatile double stored_square = x * x;
    stored_norm = stored_norm + stored_square;
    volatile double delta = q - x;
    volatile double square = delta * delta;
    l2 = l2 + square;
  }
  switch (metric) {
    case EngineBoundVectorMetricV1::kL2Squared:
      row->distance = l2;
      row->score = -row->distance;
      break;
    case EngineBoundVectorMetricV1::kInnerProduct:
      row->distance = -dot;
      row->score = dot;
      break;
    case EngineBoundVectorMetricV1::kCosine: {
      if (query_norm == 0.0 || stored_norm == 0.0) {
        *cosine_zero_norm = true;
        return false;
      }
      volatile double denominator =
          std::sqrt(static_cast<double>(query_norm)) *
          std::sqrt(static_cast<double>(stored_norm));
      volatile double similarity = dot / denominator;
      row->distance = 1.0 - similarity;
      row->score = 1.0 - row->distance;
      break;
    }
    case EngineBoundVectorMetricV1::kUnknown: return false;
  }
  row->encoded_distance = CanonicalBoundVectorReal64(row->distance);
  row->encoded_score = CanonicalBoundVectorReal64(row->score);
  return !row->encoded_distance.empty() && !row->encoded_score.empty() &&
         std::isfinite(row->distance) && std::isfinite(row->score);
}

bool BoundVectorCarrierContextMatches(
    const EngineBoundVectorReadRequestV1& request,
    const EngineNoSqlProviderGenerationMetadata& carrier) {
  const auto& context = request.context;
  return carrier.family == EngineNoSqlProviderFamily::kVector &&
         carrier.provider_id == request.selected_provider_uuid &&
         carrier.vector_ann_capability_uuid ==
             request.selected_capability_uuid &&
         carrier.database_identity ==
             EngineNoSqlProviderDatabaseIdentity(context) &&
         carrier.database_uuid == context.database_uuid.canonical &&
         carrier.collection_uuid == request.collection_uuid &&
         carrier.vector_ann_base_relation_uuid == request.collection_uuid &&
         carrier.vector_ann_metric_id == BoundVectorMetricName(request.metric) &&
         carrier.vector_ann_statement_uuid == context.statement_uuid.canonical &&
         carrier.vector_ann_statement_snapshot_uuid ==
             context.statement_snapshot_uuid.canonical &&
         carrier.vector_ann_statement_metadata_snapshot_uuid ==
             context.statement_metadata_snapshot_uuid.canonical &&
         carrier.vector_ann_owning_transaction_uuid ==
             context.transaction_uuid.canonical &&
         carrier.vector_ann_local_transaction_id ==
             context.local_transaction_id &&
         carrier.vector_ann_snapshot_visible_through_local_transaction_id ==
             context.snapshot_visible_through_local_transaction_id &&
         carrier.vector_ann_security_context_uuid ==
             context.authorization_context.authority_uuid.canonical &&
         carrier.vector_ann_catalog_epoch_uuid ==
             context.catalog_epoch_uuid.canonical &&
         carrier.security_epoch == context.security_epoch &&
         carrier.catalog_epoch == context.catalog_generation_id &&
         carrier.vector_ann_exact_fallback_available &&
         carrier.vector_ann_full_base_exact_recheck_required &&
         carrier.vector_ann_base_row_mga_recheck_required &&
         carrier.vector_ann_security_recheck_required &&
         !carrier.provider_claims_visibility_authority &&
         !carrier.provider_claims_transaction_finality_authority &&
         !carrier.vector_ann_index_claims_visibility_authority &&
         !carrier.vector_ann_index_claims_transaction_finality_authority &&
         !carrier.vector_ann_parser_claims_visibility_authority &&
         !carrier.vector_ann_parser_claims_transaction_finality_authority &&
         !carrier.vector_ann_client_claims_visibility_authority &&
         !carrier.vector_ann_client_claims_transaction_finality_authority &&
         !carrier.vector_ann_reference_claims_visibility_authority &&
         !carrier.vector_ann_reference_claims_transaction_finality_authority &&
         !carrier.vector_ann_wal_claims_visibility_authority &&
         !carrier.vector_ann_wal_claims_transaction_finality_authority;
}

bool BoundVectorCarrierDescriptorMatches(
    const EngineNoSqlProviderGenerationMetadata& carrier,
    const MgaRelationStorageDescriptor& descriptor) {
  const auto& embedding = descriptor.columns.front();
  return carrier.vector_ann_relation_descriptor_uuid ==
             descriptor.descriptor_uuid.canonical &&
         carrier.vector_ann_relation_descriptor_generation ==
             descriptor.descriptor_generation &&
         carrier.vector_ann_embedding_column_uuid ==
             embedding.column_uuid.canonical &&
         carrier.vector_ann_embedding_descriptor_uuid ==
             embedding.value_descriptor.descriptor_uuid.canonical &&
         carrier.vector_ann_embedding_type_uuid ==
             BoundVectorTypeUuid(embedding.value_descriptor) &&
         carrier.vector_ann_dimension == 3 &&
         carrier.vector_ann_element_profile == "real32";
}

}  // namespace

bool ExactBoundVectorStorageDescriptorV1(
    const MgaRelationStorageDescriptor& descriptor,
    const std::string_view collection_uuid) {
  return ExactBoundVectorStorageDescriptorImpl(descriptor, collection_uuid);
}

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

EngineBoundVectorReadResultV1 EngineBoundVectorReadV1(
    const EngineBoundVectorReadRequestV1& request) {
  constexpr std::string_view kOperation = "nosql.bound_vector_read_v1";
  bool resource_acquired = false;
  std::uint64_t scanned_row_versions = 0;
  std::uint64_t decoded_bytes = 0;
  const auto refuse = [&](const std::string& diagnostic_id,
                          const std::string& detail) {
    EngineBoundVectorReadResultV1 result;
    result.ok = false;
    result.diagnostic = MakeEngineApiDiagnostic(
        diagnostic_id, diagnostic_id, detail, true);
    result.scanned_row_version_count = scanned_row_versions;
    result.decoded_byte_count = decoded_bytes;
    result.execution_resource_acquired = resource_acquired;
    result.cleanup_count = resource_acquired ? 1 : 0;
    result.evidence.push_back({"bound_vector_failure_atomic", "true"});
    result.evidence.push_back({"bound_vector_mga_authority",
                               "engine_transaction_inventory"});
    result.evidence.push_back({"bound_vector_provider_visibility_authority",
                               "false"});
    result.evidence.push_back({"bound_vector_provider_finality_authority",
                               "false"});
    return result;
  };
  const auto cancelled = [&]() {
    return request.cancellation_requested &&
           request.cancellation_requested();
  };

  if (cancelled()) {
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "vector execution cancelled before access");
  }
  const bool filtered = request.operation ==
                        EngineBoundVectorReadOperationV1::kFilteredSearch;
  const bool ann_operation =
      request.operation == EngineBoundVectorReadOperationV1::kAnnSearch;
  if (!CanonicalBoundVectorUuid(request.collection_uuid) ||
      !CanonicalBoundVectorUuid(request.expected_descriptor_uuid) ||
      request.expected_descriptor_generation == 0 ||
      !CanonicalBoundVectorUuid(request.selected_alternative_uuid) ||
      !CanonicalBoundVectorUuid(request.selected_provider_uuid) ||
      !CanonicalBoundVectorUuid(request.selected_capability_uuid) ||
      request.selected_implementation_id != "physical_vector_search_v1" ||
      (request.operation != EngineBoundVectorReadOperationV1::kExactSearch &&
       !ann_operation && !filtered) ||
      (request.physical_route !=
           EngineBoundVectorPhysicalRouteV1::kExactScan &&
       request.physical_route !=
           EngineBoundVectorPhysicalRouteV1::kAnnWithExactFallback) ||
      (ann_operation &&
       request.physical_route !=
           EngineBoundVectorPhysicalRouteV1::kAnnWithExactFallback) ||
      filtered != request.filter.present) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "bound vector operation or selected alternative is invalid");
  }
  if (request.metric == EngineBoundVectorMetricV1::kUnknown) {
    return refuse("SB_MODEL_VECTOR_METRIC_REFUSED_V1",
                  "bound vector metric is outside the closed v1 set");
  }
  std::array<float, 3> query_vector{};
  if (!ParseCanonicalBoundVectorLiteral(request.bound_query_vector_literal,
                                        &query_vector)) {
    return refuse("SB_MODEL_VECTOR_QUERY_TYPE_REFUSED_V1",
                  "bound query vector is not canonical DENSE_VECTOR(3,REAL32)");
  }
  if (request.top_k == 0) {
    return refuse("SB_MODEL_VECTOR_TOP_K_REFUSED_V1",
                  "bound vector top-k is zero");
  }
  if (request.filter.present &&
      !CanonicalBoundVectorMetadata(
          request.filter.canonical_metadata_json)) {
    return refuse("SB_MODEL_VECTOR_FILTER_REFUSED_V1",
                  "bound vector filter value is not canonical JSON object text");
  }
  if (!ExactBoundVectorOutputDescriptors(request.output_descriptors)) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "bound vector public descriptor cohort is invalid");
  }
  if (request.maximum_scanned_row_versions == 0 ||
      request.maximum_decoded_bytes == 0 ||
      request.maximum_output_rows == 0 || request.maximum_memory_bytes == 0 ||
      request.top_k > request.maximum_output_rows ||
      !request.cancellation_requested) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "bound vector resource or cancellation contract is incomplete");
  }
  std::uint64_t preflight_bytes = request.bound_query_vector_literal.size();
  if (!CheckedBoundVectorAdd(preflight_bytes,
                             request.filter.canonical_metadata_json.size(),
                             &preflight_bytes) ||
      !CheckedBoundVectorAdd(preflight_bytes,
                             request.output_descriptors.size() * 256U,
                             &preflight_bytes) ||
      preflight_bytes > request.maximum_memory_bytes) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "bound vector pre-access memory contract is exceeded");
  }

  BoundVectorFloatingEnvironment floating_environment;
  if (!floating_environment.active()) {
    return refuse("SB_MODEL_VECTOR_ELEMENT_INVALID_V1",
                  "round-to-nearest floating-point environment is unavailable");
  }
  if (request.metric == EngineBoundVectorMetricV1::kCosine) {
    volatile double query_norm = 0.0;
    for (const float component : query_vector) {
      volatile double widened = static_cast<double>(component);
      volatile double square = widened * widened;
      query_norm = query_norm + square;
    }
    if (query_norm == 0.0) {
      return refuse("SB_MODEL_VECTOR_COSINE_ZERO_NORM_REFUSED_V1",
                    "cosine query vector has zero norm");
    }
  }

  const auto authorization = EvaluateMaterializedAuthorization(
      request.context, request.context.authorization_context, "SELECT",
      request.collection_uuid);
  if (!authorization.authorized || authorization.denied ||
      authorization.policy_recheck_required ||
      request.context.authorization_context.security_epoch !=
          request.context.security_epoch ||
      request.context.authorization_context.catalog_generation_id !=
          request.context.catalog_generation_id) {
    return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                  "current materialized vector SELECT authorization is absent or stale");
  }

  // Catalog/type admission is metadata-only. Bind the exact current relation
  // before any ANN provider or MGA heap row may be inspected.
  const auto preflight = LoadMgaRelationStorageDescriptor(
      request.context, request.collection_uuid);
  if (!preflight.ok ||
      preflight.descriptor.database_uuid.canonical !=
          request.context.database_uuid.canonical ||
      preflight.descriptor.descriptor_uuid.canonical !=
          request.expected_descriptor_uuid ||
      preflight.descriptor.descriptor_generation !=
          request.expected_descriptor_generation ||
      !ExactBoundVectorStorageDescriptorV1(preflight.descriptor,
                                           request.collection_uuid)) {
    return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                  "current vector storage descriptor is outside the exact v1 profile");
  }

  bool ann_carrier_loaded = false;
  // An ordinary exact scan is the requested physical route, not a fallback.
  // This receipt becomes true only when an admitted ANN-with-exact-fallback
  // route cannot use its current carrier and reconstructs from the MGA base.
  bool exact_fallback_selected = false;
  EngineNoSqlProviderGenerationMetadata carrier;
  if (request.physical_route ==
      EngineBoundVectorPhysicalRouteV1::kAnnWithExactFallback) {
    const auto loaded = LoadNoSqlProviderGeneration(
        request.context, EngineNoSqlProviderFamily::kVector,
        request.selected_provider_uuid, request.collection_uuid);
    if (!loaded.ok) {
      if (loaded.diagnostic.detail == kNoSqlProviderGenerationUnavailable) {
        exact_fallback_selected = true;
      } else {
        return refuse("SB_MODEL_PROVIDER_GENERATION_STALE_V1",
                      "vector ANN carrier is corrupt, partial, or duplicated");
      }
    } else if (!ValidateVectorAnnCapabilityBindingV1(loaded.metadata) ||
               !BoundVectorCarrierContextMatches(request, loaded.metadata) ||
               !BoundVectorCarrierDescriptorMatches(loaded.metadata,
                                                     preflight.descriptor)) {
      return refuse("SB_MODEL_PROVIDER_GENERATION_STALE_V1",
                    "vector ANN carrier identity or statement cohort is stale");
    } else {
      ann_carrier_loaded = true;
      carrier = loaded.metadata;
    }
  }

  resource_acquired = true;
  MgaVisibleHeapRelationReadRequest read_request;
  read_request.relation_uuid = request.collection_uuid;
  read_request.maximum_scanned_row_versions =
      request.maximum_scanned_row_versions;
  read_request.maximum_decoded_bytes = request.maximum_decoded_bytes;
  read_request.maximum_output_rows = request.maximum_output_rows;
  read_request.cancellation_requested = request.cancellation_requested;
  const auto read = ReadVisibleMgaHeapRelation(request.context, read_request);
  scanned_row_versions = read.scanned_row_version_count;
  decoded_bytes = read.decoded_byte_count;
  if (!read.ok) {
    if (read.cancellation_observed || cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "vector execution cancelled during MGA base read");
    }
    if (read.diagnostic.detail.find("maximum") != std::string::npos ||
        read.diagnostic.detail.find("bound") != std::string::npos ||
        read.diagnostic.detail.find("overflow") != std::string::npos) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "bounded MGA vector base read exceeded its contract");
    }
    return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                  "current MGA-visible vector base relation is unavailable");
  }
  if (!ExactBoundVectorStorageDescriptorV1(read.descriptor,
                                           request.collection_uuid) ||
      read.descriptor.descriptor_uuid.canonical !=
          preflight.descriptor.descriptor_uuid.canonical ||
      read.descriptor.descriptor_generation !=
          preflight.descriptor.descriptor_generation) {
    return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                  "current vector storage descriptor is outside the exact v1 profile");
  }
  if (read.current_relation_base_generation == 0) {
    return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                  "current vector base generation is absent");
  }

  bool ann_candidate_hint_selected = false;
  if (ann_carrier_loaded) {
    if (!BoundVectorCarrierDescriptorMatches(carrier, read.descriptor) ||
        carrier.vector_ann_base_relation_generation >
            read.current_relation_base_generation) {
      return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                    "vector ANN carrier is ahead of or substituted for the current base");
    }
    const bool recall_unusable =
        !carrier.vector_ann_recall_attestation_present ||
        !carrier.vector_ann_recall_sample_deterministic ||
        carrier.vector_ann_observed_recall_ppm <
            carrier.vector_ann_required_recall_ppm ||
        request.top_k > carrier.vector_ann_recall_contract_top_k;
    if (carrier.vector_ann_base_relation_generation <
            read.current_relation_base_generation ||
        recall_unusable) {
      exact_fallback_selected = true;
    } else {
      ann_candidate_hint_selected = true;
    }
  }

  std::vector<BoundVectorScoredRow> scored_rows;
  scored_rows.reserve(read.visible_rows.size());
  std::unordered_set<std::string> visible_row_uuids;
  std::uint64_t filtered_rows = 0;
  std::uint64_t accounted_memory = preflight_bytes;
  for (const auto& base_row : read.visible_rows) {
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "vector execution cancelled during exact base validation");
    }
    if (!CanonicalBoundVectorUuid(base_row.row_uuid) ||
        !visible_row_uuids.insert(base_row.row_uuid).second) {
      return refuse("SB_MODEL_VECTOR_DUPLICATE_VISIBLE_ROW_UUID_REFUSED_V1",
                    "current MGA-visible vector row UUID is invalid or duplicated");
    }
    if (base_row.values.size() != 2 ||
        base_row.values[0].first != "embedding" ||
        base_row.values[1].first != "metadata") {
      return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                    "current vector row does not match the persisted descriptor");
    }
    std::array<float, 3> stored_vector{};
    if (!ParseCanonicalBoundVectorLiteral(base_row.values[0].second,
                                          &stored_vector)) {
      return refuse("SB_MODEL_VECTOR_ELEMENT_INVALID_V1",
                    "current MGA-visible stored vector is invalid");
    }
    if (!CanonicalBoundVectorMetadata(base_row.values[1].second)) {
      return refuse("SB_MODEL_VECTOR_METADATA_INVALID_V1",
                    "current MGA-visible metadata is not canonical JSON object text");
    }
    if (request.filter.present &&
        base_row.values[1].second !=
            request.filter.canonical_metadata_json) {
      ++filtered_rows;
      if (request.metric == EngineBoundVectorMetricV1::kCosine) {
        BoundVectorScoredRow validation;
        bool cosine_zero_norm = false;
        if (!ScoreBoundVectorRow(query_vector, stored_vector, request.metric,
                                 &validation, &cosine_zero_norm)) {
          return refuse(
              cosine_zero_norm
                  ? "SB_MODEL_VECTOR_COSINE_ZERO_NORM_REFUSED_V1"
                  : "SB_MODEL_VECTOR_ELEMENT_INVALID_V1",
              "current MGA-visible vector failed exact metric validation");
        }
      }
      continue;
    }
    BoundVectorScoredRow scored;
    scored.row_uuid = base_row.row_uuid;
    bool cosine_zero_norm = false;
    if (!ScoreBoundVectorRow(query_vector, stored_vector, request.metric,
                             &scored, &cosine_zero_norm)) {
      return refuse(
          cosine_zero_norm
              ? "SB_MODEL_VECTOR_COSINE_ZERO_NORM_REFUSED_V1"
              : "SB_MODEL_VECTOR_ELEMENT_INVALID_V1",
          "current MGA-visible vector failed exact metric validation");
    }
    std::uint64_t row_memory = sizeof(BoundVectorScoredRow);
    if (!CheckedBoundVectorAdd(row_memory, scored.row_uuid.size(),
                               &row_memory) ||
        !CheckedBoundVectorAdd(row_memory, scored.encoded_distance.size(),
                               &row_memory) ||
        !CheckedBoundVectorAdd(row_memory, scored.encoded_score.size(),
                               &row_memory) ||
        !CheckedBoundVectorAdd(accounted_memory, row_memory,
                               &accounted_memory) ||
        accounted_memory > request.maximum_memory_bytes) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "vector exact scoring memory contract is exceeded");
    }
    scored_rows.push_back(std::move(scored));
  }
  if (cancelled()) {
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "vector execution cancelled before exact ordering");
  }
  std::ranges::sort(scored_rows, [](const auto& left, const auto& right) {
    if (left.distance != right.distance) return left.distance < right.distance;
    return left.row_uuid < right.row_uuid;
  });
  if (scored_rows.size() > request.top_k) {
    scored_rows.resize(request.top_k);
  }

  EngineBoundVectorReadResultV1 result;
  result.ok = true;
  result.diagnostic = MakeEngineApiDiagnostic(
      "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  result.relation_descriptor = read.descriptor;
  result.output_descriptors = request.output_descriptors;
  result.current_relation_base_generation =
      read.current_relation_base_generation;
  result.scanned_row_version_count = read.scanned_row_version_count;
  result.decoded_byte_count = read.decoded_byte_count;
  result.visible_base_row_count = read.visible_rows.size();
  result.filtered_base_row_count = filtered_rows;
  result.scored_base_row_count =
      read.visible_rows.size() - filtered_rows;
  result.ann_carrier_loaded = ann_carrier_loaded;
  result.ann_candidate_hint_selected = ann_candidate_hint_selected;
  result.exact_fallback_selected = exact_fallback_selected;
  result.full_base_exact_recheck_complete = true;
  result.base_row_mga_recheck_complete = true;
  result.security_recheck_complete = true;
  result.execution_resource_acquired = true;
  result.cleanup_count = 1;
  result.rows.reserve(scored_rows.size());
  for (const auto& scored : scored_rows) {
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "vector execution cancelled during result materialization");
    }
    std::uint64_t row_bytes = scored.row_uuid.size();
    if (!CheckedBoundVectorAdd(row_bytes, scored.encoded_distance.size(),
                               &row_bytes) ||
        !CheckedBoundVectorAdd(row_bytes, scored.encoded_score.size(),
                               &row_bytes) ||
        !CheckedBoundVectorAdd(row_bytes, 3, &row_bytes) ||
        !CheckedBoundVectorAdd(result.result_byte_count, row_bytes,
                               &result.result_byte_count) ||
        result.result_byte_count > request.maximum_decoded_bytes) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "vector result byte contract is exceeded");
    }
    result.rows.push_back({scored.row_uuid, scored.distance, scored.score,
                           scored.encoded_distance, scored.encoded_score});
  }
  if (cancelled()) {
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "vector execution cancelled before atomic publication");
  }
  result.evidence.push_back(
      {"bound_vector_physical_route",
       ann_candidate_hint_selected ? "ann_hint_full_base_exact_recheck"
                                   : "exact_full_base_scan"});
  result.evidence.push_back(
      {"bound_vector_exact_fallback_selected",
       exact_fallback_selected ? "true" : "false"});
  result.evidence.push_back(
      {"bound_vector_relation_base_generation",
       std::to_string(result.current_relation_base_generation)});
  result.evidence.push_back(
      {"bound_vector_result_ordering",
       "distance_numeric_ascending_then_row_uuid_bytes_ascending"});
  result.evidence.push_back(
      {"bound_vector_full_base_exact_recheck", "true"});
  result.evidence.push_back(
      {"bound_vector_mga_authority", "engine_transaction_inventory"});
  result.evidence.push_back(
      {"bound_vector_provider_visibility_authority", "false"});
  result.evidence.push_back(
      {"bound_vector_provider_finality_authority", "false"});
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
