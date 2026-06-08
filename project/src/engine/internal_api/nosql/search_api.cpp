// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/search_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "nosql/nosql_batch_point_lookup_support.hpp"
#include "nosql/nosql_surface_support.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

struct IndexedSearchDocument {
  std::string document_uuid;
  std::string segment_kind;
  std::map<std::string, EngineApiU64> term_frequency;
  EngineApiU64 token_count = 0;
};

struct SearchCandidate {
  std::string document_uuid;
  std::string segment_kind;
  double score = 0.0;
  std::vector<std::pair<std::string, double>> contributions;
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
    AddApiBehaviorEvidence(result, "search_physical_provider", item);
  }
}

bool IsPhysicalSearchRequest(const EngineSearchQueryRequest& request) {
  return !request.query_text.empty() || request.top_k != 0 ||
         !request.document_corpus.empty() ||
         request.physical_proof.proof_supplied;
}

std::vector<std::string> TokenizeSearchText(const std::string& text) {
  std::vector<std::string> tokens;
  std::string token;
  for (const unsigned char ch : text) {
    if (std::isalnum(ch) != 0) {
      token.push_back(static_cast<char>(std::tolower(ch)));
    } else if (!token.empty()) {
      tokens.push_back(token);
      token.clear();
    }
  }
  if (!token.empty()) {
    tokens.push_back(token);
  }
  return tokens;
}

std::vector<std::string> UniqueTermsPreserveOrder(const std::vector<std::string>& tokens) {
  std::set<std::string> seen;
  std::vector<std::string> unique;
  for (const auto& token : tokens) {
    if (seen.insert(token).second) {
      unique.push_back(token);
    }
  }
  return unique;
}

std::string JoinTerms(const std::vector<std::string>& terms) {
  std::string joined;
  for (const auto& term : terms) {
    if (!joined.empty()) {
      joined += ',';
    }
    joined += term;
  }
  return joined;
}

std::string FormatScore(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(6) << value;
  return out.str();
}

std::string FormatContributions(
    const std::vector<std::pair<std::string, double>>& contributions) {
  std::string formatted;
  for (const auto& [term, score] : contributions) {
    if (!formatted.empty()) {
      formatted += ';';
    }
    formatted += term + ':' + FormatScore(score);
  }
  return formatted;
}

std::vector<IndexedSearchDocument> BuildIndexedDocuments(
    const std::vector<EngineSearchDocumentInput>& corpus) {
  std::vector<IndexedSearchDocument> indexed;
  indexed.reserve(corpus.size());
  for (std::size_t i = 0; i < corpus.size(); ++i) {
    IndexedSearchDocument doc;
    doc.document_uuid = corpus[i].document_uuid.empty()
                            ? "search_doc_" + std::to_string(i + 1)
                            : corpus[i].document_uuid;
    doc.segment_kind = corpus[i].sealed_segment ? "sealed_inverted_segment"
                                                : "mutable_buffer";
    for (const auto& token : TokenizeSearchText(corpus[i].text)) {
      ++doc.term_frequency[token];
      ++doc.token_count;
    }
    indexed.push_back(std::move(doc));
  }
  return indexed;
}

double Bm25Contribution(EngineApiU64 term_frequency,
                        EngineApiU64 document_frequency,
                        EngineApiU64 document_count,
                        EngineApiU64 document_length,
                        double average_document_length) {
  if (term_frequency == 0 || document_frequency == 0 || document_count == 0) {
    return 0.0;
  }
  constexpr double k1 = 1.2;
  constexpr double b = 0.75;
  const double idf =
      std::log(1.0 + (static_cast<double>(document_count - document_frequency) + 0.5) /
                         (static_cast<double>(document_frequency) + 0.5));
  const double normalized_length =
      average_document_length <= 0.0
          ? 1.0
          : static_cast<double>(document_length) / average_document_length;
  const double denominator =
      static_cast<double>(term_frequency) + k1 * (1.0 - b + b * normalized_length);
  return idf * (static_cast<double>(term_frequency) * (k1 + 1.0)) / denominator;
}

template <typename TResult>
std::optional<TResult> ValidatePhysicalProof(
    const EngineSearchQueryRequest& request,
    const std::string& operation_id,
    const EngineSearchPhysicalProof& proof) {
  if (!proof.proof_supplied) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kSearchPhysicalProofMissing);
  }
  if (proof.provider_contract.family != EngineNoSqlProviderFamily::kSearch) {
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
  if (!proof.mutable_buffer_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kSearchMutableBufferProofMissing);
  }
  if (!proof.sealed_inverted_segment_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kSearchSealedInvertedSegmentProofMissing);
  }
  if (!proof.bm25_statistics_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kSearchBm25StatisticsProofMissing);
  }
  if (!proof.sparse_vector_score_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kSearchSparseVectorScoreProofMissing);
  }
  if (!proof.maxscore_wand_topk_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kSearchMaxScoreWandTopKProofMissing);
  }
  if (!proof.bloom_negative_pruning_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kSearchBloomNegativePruningProofMissing);
  }
  return std::nullopt;
}

void AddSearchEvidence(EngineApiResult* result,
                       const EngineNoSqlPhysicalProviderSelection& selection,
                       const std::vector<std::string>& query_terms,
                       EngineApiU64 mutable_documents,
                       EngineApiU64 sealed_documents,
                       EngineApiU64 absent_terms,
                       EngineApiU64 pruned_candidates) {
  AddEngineNoSqlSurfaceEvidence(result, "search", "bm25_wand_topk_provider");
  AddSelectionEvidence(selection, result);
  AddApiBehaviorEvidence(result, "search_physical_access",
                         "mutable_buffer_and_sealed_segment_bm25");
  AddApiBehaviorEvidence(result, "search_tokenization", JoinTerms(query_terms));
  AddApiBehaviorEvidence(result, "search_mutable_buffer_documents",
                         std::to_string(mutable_documents));
  AddApiBehaviorEvidence(result, "search_sealed_segment_documents",
                         std::to_string(sealed_documents));
  AddApiBehaviorEvidence(result, "search_bm25_statistics", "document_frequency_idf_avgdl");
  AddApiBehaviorEvidence(result, "search_sparse_vector_score",
                         "per_term_contributions_recorded");
  AddApiBehaviorEvidence(result, "search_wand_topk_pruning",
                         "candidates_pruned=" + std::to_string(pruned_candidates));
  AddApiBehaviorEvidence(result, "search_bloom_negative_pruning",
                         "absent_terms=" + std::to_string(absent_terms));
  AddApiBehaviorEvidence(result, "behavior_store_scan_selected", "false");
  AddApiBehaviorEvidence(result, "descriptor_scan_selected", "false");
  AddApiBehaviorEvidence(result, "row_mga_recheck_evidence", "required");
  AddApiBehaviorEvidence(result, "row_security_recheck_evidence", "required");
  AddApiBehaviorEvidence(result, "mga_finality_authority",
                         "engine_transaction_inventory");
  AddApiBehaviorEvidence(result, "provider_transaction_finality_authority", "false");
  AddApiBehaviorEvidence(result, "parser_transaction_finality_authority", "false");
  AddApiBehaviorEvidence(result, "client_autocommit_authority", "false");
}

EngineSearchQueryResult PhysicalSearchQuery(
    const EngineSearchQueryRequest& request,
    const std::string& operation_id) {
  if (request.query_text.empty()) {
    return DiagnosticResult<EngineSearchQueryResult>(
        request.context, operation_id, kSearchQueryTextRequired);
  }
  if (auto failure = ValidatePhysicalProof<EngineSearchQueryResult>(
          request, operation_id, request.physical_proof)) {
    return *failure;
  }

  const auto selection =
      SelectLocalNoSqlPhysicalProvider(request.physical_proof.provider_contract);
  const auto query_terms =
      UniqueTermsPreserveOrder(TokenizeSearchText(request.query_text));
  const auto indexed_documents = BuildIndexedDocuments(request.document_corpus);
  const EngineApiU64 top_k = request.top_k == 0 ? 10 : request.top_k;

  std::map<std::string, EngineApiU64> document_frequency;
  EngineApiU64 total_tokens = 0;
  EngineApiU64 mutable_documents = 0;
  EngineApiU64 sealed_documents = 0;
  for (const auto& doc : indexed_documents) {
    total_tokens += doc.token_count;
    if (doc.segment_kind == "mutable_buffer") {
      ++mutable_documents;
    } else {
      ++sealed_documents;
    }
    for (const auto& [term, frequency] : doc.term_frequency) {
      (void)frequency;
      ++document_frequency[term];
    }
  }

  EngineApiU64 absent_terms = 0;
  for (const auto& term : query_terms) {
    if (document_frequency[term] == 0) {
      ++absent_terms;
    }
  }

  const double average_document_length =
      indexed_documents.empty()
          ? 0.0
          : static_cast<double>(total_tokens) /
                static_cast<double>(indexed_documents.size());

  std::map<std::string, double> term_upper_bound;
  for (const auto& term : query_terms) {
    double upper_bound = 0.0;
    for (const auto& doc : indexed_documents) {
      const auto frequency = doc.term_frequency.find(term);
      if (frequency == doc.term_frequency.end()) {
        continue;
      }
      upper_bound = std::max(
          upper_bound,
          Bm25Contribution(frequency->second,
                           document_frequency[term],
                           indexed_documents.size(),
                           doc.token_count,
                           average_document_length));
    }
    term_upper_bound[term] = upper_bound;
  }

  std::vector<SearchCandidate> top_candidates;
  EngineApiU64 pruned_candidates = 0;
  double threshold = 0.0;
  for (const auto& doc : indexed_documents) {
    double max_possible_score = 0.0;
    for (const auto& term : query_terms) {
      if (doc.term_frequency.find(term) != doc.term_frequency.end()) {
        max_possible_score += term_upper_bound[term];
      }
    }
    if (top_k != 0 && top_candidates.size() >= top_k &&
        max_possible_score <= threshold + 0.0000001) {
      ++pruned_candidates;
      continue;
    }

    SearchCandidate candidate;
    candidate.document_uuid = doc.document_uuid;
    candidate.segment_kind = doc.segment_kind;
    for (const auto& term : query_terms) {
      const auto frequency = doc.term_frequency.find(term);
      if (frequency == doc.term_frequency.end()) {
        continue;
      }
      const double contribution =
          Bm25Contribution(frequency->second,
                           document_frequency[term],
                           indexed_documents.size(),
                           doc.token_count,
                           average_document_length);
      candidate.score += contribution;
      candidate.contributions.push_back({term, contribution});
    }
    if (candidate.score <= 0.0) {
      continue;
    }
    top_candidates.push_back(std::move(candidate));
    std::sort(top_candidates.begin(),
              top_candidates.end(),
              [](const SearchCandidate& left, const SearchCandidate& right) {
                if (std::abs(left.score - right.score) > 0.0000001) {
                  return left.score > right.score;
                }
                return left.document_uuid < right.document_uuid;
              });
    if (top_k != 0 && top_candidates.size() > top_k) {
      top_candidates.resize(top_k);
    }
    if (top_k != 0 && top_candidates.size() >= top_k) {
      threshold = top_candidates.back().score;
    }
  }

  auto result =
      MakeApiBehaviorSuccess<EngineSearchQueryResult>(request.context, operation_id);
  AddSearchEvidence(&result,
                    selection,
                    query_terms,
                    mutable_documents,
                    sealed_documents,
                    absent_terms,
                    pruned_candidates);
  std::vector<EngineNoSqlBatchPointLookupItem> lookup_items;
  lookup_items.reserve(top_candidates.size());
  for (const auto& candidate : top_candidates) {
    lookup_items.push_back(
        {candidate.document_uuid,
         candidate.document_uuid,
         candidate.score,
         "search_payload",
         {{"segment_kind", candidate.segment_kind},
          {"sparse_vector_terms", FormatContributions(candidate.contributions)}}});
  }
  if (auto failure = AddEngineNoSqlOrderedBatchLookupEvidence<
          EngineSearchQueryResult>(
          request.context,
          operation_id,
          "search",
          scratchbird::core::index::BatchPointLookupPurpose::search_payload,
          selection,
          lookup_items,
          &result)) {
    return *failure;
  }
  EngineApiU64 rank = 1;
  for (const auto& candidate : top_candidates) {
    AddApiBehaviorRow(&result,
                      {{"surface", "search"},
                       {"document_uuid", candidate.document_uuid},
                       {"rank", std::to_string(rank++)},
                       {"score", FormatScore(candidate.score)},
                       {"segment_kind", candidate.segment_kind},
                       {"sparse_vector_terms",
                        FormatContributions(candidate.contributions)},
                       {"row_mga_recheck_required", "true"},
                       {"row_security_recheck_required", "true"}});
  }
  result.dml_summary.index_probes = indexed_documents.size();
  result.dml_summary.visible_rows_scanned = 0;
  AddApiBehaviorEvidence(&result, "search_rows_returned",
                         std::to_string(result.result_shape.rows.size()));
  return result;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_SEARCH_API_BEHAVIOR
EngineSearchQueryResult EngineSearchQuery(const EngineSearchQueryRequest& request) {
  constexpr const char* kOperation = "nosql.search_query";
  if (!request.context.cluster_authority_available && EngineNoSqlRequiresClusterAuthority(request)) {
    return EngineNoSqlClusterAuthorityUnavailable<EngineSearchQueryResult>(request, kOperation);
  }
  if (IsPhysicalSearchRequest(request)) {
    return PhysicalSearchQuery(request, kOperation);
  }
  if (EngineNoSqlRequestsHeavyImmutableGeneration(request)) {
    return EngineNoSqlPublishHeavyImmutableGeneration<EngineSearchQueryResult>(
        request,
        kOperation,
        "search",
        "text_search",
        "text_search_immutable_segment_v1",
        "search_query");
  }
  auto payload = EngineNoSqlResolvePayloadForStorage(request, kOperation, "text");
  if (!payload.ok) {
    auto failure = MakeApiBehaviorDiagnostic<EngineSearchQueryResult>(
        request.context,
        kOperation,
        payload.diagnostic);
    for (const auto& evidence : payload.evidence) {
      failure.evidence.push_back(evidence);
    }
    return failure;
  }
  auto result = MakeApiBehaviorSuccess<EngineSearchQueryResult>(request.context, kOperation);
  AddApiBehaviorRow(&result, {{"surface", "search"},
                              {"search_kind", "full_text_descriptor_query"},
                              {"execution", "specialized_descriptor_fallback"},
                              {"payload", payload.payload}});
  for (const auto& evidence : payload.evidence) {
    result.evidence.push_back(evidence);
  }
  AddApiBehaviorEvidence(&result, "search_query", "full_text_descriptor_query");
  AddEngineNoSqlSurfaceEvidence(&result, "search", "specialized_descriptor_fallback");
  return result;
}

}  // namespace scratchbird::engine::internal_api
