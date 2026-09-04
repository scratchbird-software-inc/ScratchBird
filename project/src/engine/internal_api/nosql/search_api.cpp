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
#include "datatype_catalog_manifest.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "nosql/nosql_batch_point_lookup_support.hpp"
#include "nosql/nosql_provider_generation_store.hpp"
#include "nosql/nosql_surface_support.hpp"
#include "security/security_model.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cfenv>
#include <charconv>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
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

namespace {

constexpr std::string_view kBoundSearchAnalyzerDigest =
    "9033908d159ddd442f2042467fd49e0a12b47679f7514e9aa6e55488e151d316";

bool CanonicalBoundSearchUuid(const std::string_view value) {
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

bool ValidBoundSearchUtf8(const std::string_view value) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto first = static_cast<unsigned char>(value[offset]);
    if (first <= 0x7fU) {
      ++offset;
      continue;
    }
    std::size_t continuation = 0;
    std::uint32_t codepoint = 0;
    if (first >= 0xc2U && first <= 0xdfU) {
      continuation = 1;
      codepoint = first & 0x1fU;
    } else if (first >= 0xe0U && first <= 0xefU) {
      continuation = 2;
      codepoint = first & 0x0fU;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      continuation = 3;
      codepoint = first & 0x07U;
    } else {
      return false;
    }
    if (offset + continuation >= value.size()) return false;
    for (std::size_t index = 1; index <= continuation; ++index) {
      const auto ch = static_cast<unsigned char>(value[offset + index]);
      if ((ch & 0xc0U) != 0x80U) return false;
      codepoint = (codepoint << 6U) | (ch & 0x3fU);
    }
    if ((continuation == 1 && codepoint < 0x80U) ||
        (continuation == 2 && codepoint < 0x800U) ||
        (continuation == 3 && codepoint < 0x10000U) ||
        codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
      return false;
    }
    offset += continuation + 1;
  }
  return true;
}

struct BoundSearchAnalysis {
  bool ok = false;
  bool input_too_large = false;
  bool token_too_large = false;
  std::vector<std::string> tokens;
};

BoundSearchAnalysis AnalyzeBoundSearchAscii(const std::string_view input) {
  BoundSearchAnalysis result;
  if (input.size() > 1U * 1024U * 1024U) {
    result.input_too_large = true;
    return result;
  }
  std::string token;
  for (const unsigned char raw : input) {
    if (raw < 0x20U || raw > 0x7eU) return result;
    const char folded = raw >= 'A' && raw <= 'Z'
                            ? static_cast<char>(raw - 'A' + 'a')
                            : static_cast<char>(raw);
    const bool token_byte =
        (folded >= 'a' && folded <= 'z') ||
        (folded >= '0' && folded <= '9');
    if (token_byte) {
      token.push_back(folded);
      if (token.size() > 64) {
        result.token_too_large = true;
        return result;
      }
    } else if (!token.empty()) {
      result.tokens.push_back(std::move(token));
      token.clear();
    }
  }
  if (!token.empty()) result.tokens.push_back(std::move(token));
  result.ok = true;
  return result;
}

std::vector<std::string> UniqueBoundSearchTerms(
    const std::vector<std::string>& terms) {
  std::unordered_set<std::string> seen;
  std::vector<std::string> unique;
  for (const auto& term : terms) {
    if (seen.insert(term).second) unique.push_back(term);
  }
  return unique;
}

std::size_t BoundSearchLevenshteinAtMostOne(const std::string_view left,
                                             const std::string_view right) {
  if (left.size() + 1 < right.size() || right.size() + 1 < left.size()) {
    return 2;
  }
  if (left.size() == right.size()) {
    std::size_t differences = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
      differences += left[index] != right[index];
      if (differences > 1) return 2;
    }
    return differences;
  }
  const auto shorter = left.size() < right.size() ? left : right;
  const auto longer = left.size() < right.size() ? right : left;
  std::size_t short_index = 0;
  std::size_t long_index = 0;
  std::size_t edits = 0;
  while (short_index < shorter.size() && long_index < longer.size()) {
    if (shorter[short_index] == longer[long_index]) {
      ++short_index;
      ++long_index;
      continue;
    }
    if (++edits > 1) return 2;
    ++long_index;
  }
  return edits + (long_index < longer.size() ? 1U : 0U);
}

bool BoundSearchPhraseMatch(const std::vector<std::string>& document,
                            const std::vector<std::string>& query) {
  if (query.empty() || query.size() > document.size()) return false;
  for (std::size_t start = 0; start + query.size() <= document.size();
       ++start) {
    bool match = true;
    for (std::size_t index = 0; index < query.size(); ++index) {
      if (document[start + index] != query[index]) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

std::string BoundSearchDescriptorField(const std::string_view descriptor,
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

std::string BoundSearchTypeUuid(const EngineDescriptor& descriptor) {
  return BoundSearchDescriptorField(descriptor.encoded_descriptor,
                                    "type_uuid");
}

std::string ExactBoundSearchCoreTypeUuid(const std::string_view stable_name) {
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

bool ExactBoundSearchDescriptorFields(
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

bool ExactBoundSearchStorageDescriptorImpl(
    const MgaRelationStorageDescriptor& descriptor,
    const std::string_view collection_uuid) {
  const auto text_type_uuid = ExactBoundSearchCoreTypeUuid("character");
  if (descriptor.relation_uuid.canonical != collection_uuid ||
      text_type_uuid.empty() ||
      !CanonicalBoundSearchUuid(descriptor.database_uuid.canonical) ||
      !CanonicalBoundSearchUuid(descriptor.schema_uuid.canonical) ||
      descriptor.relation_kind != "table" ||
      descriptor.storage_profile != "local_mga_rowstore_v1" ||
      descriptor.descriptor_generation == 0 ||
      !CanonicalBoundSearchUuid(descriptor.descriptor_uuid.canonical) ||
      descriptor.columns.size() != 2) {
    return false;
  }
  const auto exact_text = [](const auto& column,
                             const std::uint32_t ordinal,
                             const std::string_view name,
                             const std::string_view text_type_uuid) {
    return column.ordinal == ordinal && column.canonical_name_key == name &&
           !column.nullable && !column.generated && !column.identity_column &&
           column.storage_class == "inline_row_value" &&
           column.max_inline_bytes == 4096 &&
           column.overflow_policy == "mga_large_value_locator" &&
           column.charset_uuid.empty() && column.collation_uuid.empty() &&
           column.character_length == 0 &&
           column.value_descriptor.descriptor_kind ==
               "canonical_type_descriptor" &&
           column.value_descriptor.canonical_type_name == "text" &&
           CanonicalBoundSearchUuid(column.column_uuid.canonical) &&
           CanonicalBoundSearchUuid(
               column.value_descriptor.descriptor_uuid.canonical) &&
           column.value_descriptor.descriptor_uuid.canonical ==
               column.column_uuid.canonical &&
           ExactBoundSearchDescriptorFields(
               column.value_descriptor,
               {{"canonical", "text"},
                {"type_uuid", text_type_uuid},
                {"nullable", "false"},
                {"column_uuid", column.column_uuid.canonical},
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
  };
  return descriptor.columns[0].column_uuid.canonical !=
             descriptor.columns[1].column_uuid.canonical &&
         descriptor.columns[0].value_descriptor.descriptor_uuid.canonical !=
             descriptor.columns[1].value_descriptor.descriptor_uuid.canonical &&
         exact_text(descriptor.columns[0], 0, "body", text_type_uuid) &&
         exact_text(descriptor.columns[1], 1, "category", text_type_uuid);
}

bool ExactBoundSearchOutputDescriptors(
    const std::vector<EngineDescriptor>& descriptors) {
  if (descriptors.size() != 5) return false;
  static constexpr std::array<std::string_view, 5> kTypes{
      "uuid", "uuid", "uint64", "real64", "uint64"};
  std::unordered_set<std::string> descriptor_uuids;
  for (std::size_t index = 0; index < descriptors.size(); ++index) {
    const auto& descriptor = descriptors[index];
    const auto type_uuid = ExactBoundSearchCoreTypeUuid(kTypes[index]);
    if (!CanonicalBoundSearchUuid(descriptor.descriptor_uuid.canonical) ||
        !descriptor_uuids.insert(descriptor.descriptor_uuid.canonical).second ||
        descriptor.descriptor_kind != "scalar" || type_uuid.empty() ||
        descriptor.canonical_type_name != kTypes[index] ||
        !ExactBoundSearchDescriptorFields(
            descriptor,
            {{"type_uuid", type_uuid}, {"nullability", "non_null"}})) {
      return false;
    }
  }
  return true;
}

bool CheckedBoundSearchAdd(const std::uint64_t left,
                           const std::uint64_t right,
                           std::uint64_t* result) {
  if (result == nullptr ||
      right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  *result = left + right;
  return true;
}

class BoundSearchFloatingEnvironment final {
 public:
  BoundSearchFloatingEnvironment() {
    active_ = std::fegetenv(&saved_) == 0;
    if (active_) {
      std::feclearexcept(FE_ALL_EXCEPT);
      if (std::fesetround(FE_TONEAREST) != 0) active_ = false;
    }
  }
  ~BoundSearchFloatingEnvironment() {
    if (active_) std::fesetenv(&saved_);
  }
  bool active() const { return active_; }

 private:
  std::fenv_t saved_{};
  bool active_ = false;
};

std::string CanonicalBoundSearchReal64(double value) {
  if (!std::isfinite(value)) return {};
  if (value == 0.0) value = 0.0;
  std::array<char, 128> encoded{};
  const auto rendered = std::to_chars(encoded.data(),
                                      encoded.data() + encoded.size(), value,
                                      std::chars_format::general);
  if (rendered.ec != std::errc{}) return {};
  return std::string(encoded.data(), rendered.ptr);
}

struct BoundSearchDocument {
  std::string document_uuid;
  std::string category;
  std::vector<std::string> tokens;
};

struct BoundSearchScoredRow {
  std::string document_uuid;
  double score = 0.0;
  std::string encoded_score;
};

bool BoundSearchCarrierContextMatches(
    const EngineBoundSearchReadRequestV1& request,
    const EngineNoSqlProviderGenerationMetadata& carrier) {
  const auto& context = request.context;
  return carrier.family == EngineNoSqlProviderFamily::kSearch &&
         carrier.provider_id == request.selected_provider_uuid &&
         carrier.search_segment_capability_uuid ==
             request.selected_capability_uuid &&
         carrier.database_identity ==
             EngineNoSqlProviderDatabaseIdentity(context) &&
         carrier.database_uuid == context.database_uuid.canonical &&
         carrier.collection_uuid == request.collection_uuid &&
         carrier.search_segment_base_relation_uuid == request.collection_uuid &&
         carrier.search_segment_analyzer_uuid == request.analyzer_uuid &&
         carrier.search_segment_analyzer_generation ==
             request.analyzer_generation &&
         carrier.search_segment_analyzer_pipeline_sha256 ==
             request.analyzer_pipeline_sha256 &&
         carrier.search_segment_statement_uuid ==
             context.statement_uuid.canonical &&
         carrier.search_segment_statement_snapshot_uuid ==
             context.statement_snapshot_uuid.canonical &&
         carrier.search_segment_statement_metadata_snapshot_uuid ==
             context.statement_metadata_snapshot_uuid.canonical &&
         carrier.search_segment_owning_transaction_uuid ==
             context.transaction_uuid.canonical &&
         carrier.search_segment_local_transaction_id ==
             context.local_transaction_id &&
         carrier.search_segment_snapshot_visible_through_local_transaction_id ==
             context.snapshot_visible_through_local_transaction_id &&
         carrier.search_segment_security_context_uuid ==
             context.authorization_context.authority_uuid.canonical &&
         carrier.search_segment_catalog_epoch_uuid ==
             context.catalog_epoch_uuid.canonical &&
         carrier.security_epoch == context.security_epoch &&
         carrier.catalog_epoch == context.catalog_generation_id &&
         carrier.search_segment_exact_fallback_available &&
         carrier.search_segment_full_corpus_exact_recheck_required &&
         carrier.search_segment_residual_recheck_required &&
         carrier.search_segment_base_row_mga_recheck_required &&
         carrier.search_segment_security_recheck_required &&
         !carrier.provider_claims_visibility_authority &&
         !carrier.provider_claims_transaction_finality_authority;
}

bool BoundSearchCarrierDescriptorMatches(
    const EngineNoSqlProviderGenerationMetadata& carrier,
    const MgaRelationStorageDescriptor& descriptor) {
  return carrier.search_segment_relation_descriptor_uuid ==
             descriptor.descriptor_uuid.canonical &&
         carrier.search_segment_relation_descriptor_generation ==
             descriptor.descriptor_generation &&
         carrier.search_segment_body_column_uuid ==
             descriptor.columns[0].column_uuid.canonical &&
         carrier.search_segment_body_descriptor_uuid ==
             descriptor.columns[0].value_descriptor.descriptor_uuid.canonical &&
         carrier.search_segment_body_type_uuid ==
             BoundSearchTypeUuid(descriptor.columns[0].value_descriptor) &&
         carrier.search_segment_category_column_uuid ==
             descriptor.columns[1].column_uuid.canonical &&
         carrier.search_segment_category_descriptor_uuid ==
             descriptor.columns[1].value_descriptor.descriptor_uuid.canonical &&
         carrier.search_segment_category_type_uuid ==
             BoundSearchTypeUuid(descriptor.columns[1].value_descriptor);
}

}  // namespace

bool ExactBoundSearchStorageDescriptorV1(
    const MgaRelationStorageDescriptor& descriptor,
    const std::string_view collection_uuid) {
  return ExactBoundSearchStorageDescriptorImpl(descriptor, collection_uuid);
}

EngineBoundSearchReadResultV1 EngineBoundSearchReadV1(
    const EngineBoundSearchReadRequestV1& request) {
  bool resource_acquired = false;
  std::uint64_t scanned_row_versions = 0;
  std::uint64_t decoded_bytes = 0;
  const auto refuse = [&](const std::string& diagnostic_id,
                          const std::string& detail) {
    EngineBoundSearchReadResultV1 result;
    result.ok = false;
    result.diagnostic = MakeEngineApiDiagnostic(
        diagnostic_id, diagnostic_id, detail, true);
    result.scanned_row_version_count = scanned_row_versions;
    result.decoded_byte_count = decoded_bytes;
    result.execution_resource_acquired = resource_acquired;
    result.cleanup_count = resource_acquired ? 1 : 0;
    result.evidence.push_back({"bound_search_failure_atomic", "true"});
    result.evidence.push_back(
        {"bound_search_mga_authority", "engine_transaction_inventory"});
    result.evidence.push_back(
        {"bound_search_segment_visibility_authority", "false"});
    result.evidence.push_back(
        {"bound_search_segment_finality_authority", "false"});
    return result;
  };
  const auto cancelled = [&]() {
    return request.cancellation_requested && request.cancellation_requested();
  };

  if (cancelled()) {
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "search execution cancelled before access");
  }
  if (!CanonicalBoundSearchUuid(request.collection_uuid) ||
      !CanonicalBoundSearchUuid(request.expected_descriptor_uuid) ||
      request.expected_descriptor_generation == 0 ||
      !CanonicalBoundSearchUuid(request.selected_alternative_uuid) ||
      !CanonicalBoundSearchUuid(request.selected_provider_uuid) ||
      !CanonicalBoundSearchUuid(request.selected_capability_uuid) ||
      request.selected_implementation_id !=
          "physical_search_rank_scan_v1" ||
      (request.operation != EngineBoundSearchOperationV1::kTerms &&
       request.operation != EngineBoundSearchOperationV1::kPhrase &&
       request.operation != EngineBoundSearchOperationV1::kFuzzy) ||
      (request.physical_route !=
           EngineBoundSearchPhysicalRouteV1::kExactCorpusScan &&
       request.physical_route !=
           EngineBoundSearchPhysicalRouteV1::kSegmentWithExactFallback)) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "bound search operation or selected alternative is invalid");
  }
  if (!CanonicalBoundSearchUuid(request.analyzer_uuid) ||
      request.analyzer_generation == 0 ||
      request.analyzer_pipeline_sha256 != kBoundSearchAnalyzerDigest) {
    return refuse("SB_MODEL_SEARCH_ANALYZER_BINDING_REQUIRED_V1",
                  "bound search analyzer cohort is absent or substituted");
  }
  const auto query = AnalyzeBoundSearchAscii(request.bound_query_text);
  if (!query.ok || query.tokens.empty()) {
    return refuse(query.input_too_large || query.token_too_large
                      ? "SB_MODEL_SEARCH_QUERY_TOKEN_LIMIT_REFUSED_V1"
                      : "SB_MODEL_SEARCH_QUERY_TYPE_REFUSED_V1",
                  "bound search query is outside SB_ASCII_POSITIONAL_V1");
  }
  if (query.tokens.size() > 16 ||
      (request.operation == EngineBoundSearchOperationV1::kFuzzy &&
       (query.tokens.size() != 1 || request.fuzzy_maximum_edits != 1)) ||
      (request.operation != EngineBoundSearchOperationV1::kFuzzy &&
       request.fuzzy_maximum_edits != 0)) {
    return refuse("SB_MODEL_SEARCH_QUERY_TOKEN_LIMIT_REFUSED_V1",
                  "bound search query token/edit contract is invalid");
  }
  if (request.top_k == 0) {
    return refuse("SB_MODEL_SEARCH_TOP_K_REFUSED_V1",
                  "bound search top-k is zero");
  }
  if (request.filter.present &&
      (!ValidBoundSearchUtf8(request.filter.category_text) ||
       request.filter.category_text.size() > 4096)) {
    return refuse("SB_MODEL_SEARCH_FILTER_REFUSED_V1",
                  "bound category filter is invalid TEXT");
  }
  if (!ExactBoundSearchOutputDescriptors(request.output_descriptors)) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "bound search public descriptor cohort is invalid");
  }
  if (request.maximum_scanned_row_versions == 0 ||
      request.maximum_decoded_bytes == 0 || request.maximum_tokens == 0 ||
      request.maximum_positions == 0 || request.maximum_candidates == 0 ||
      request.maximum_scored_rows == 0 ||
      request.maximum_output_rows == 0 || request.maximum_memory_bytes == 0 ||
      request.top_k > request.maximum_output_rows ||
      !request.cancellation_requested) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "bound search resource/cancellation contract is incomplete");
  }
  std::uint64_t accounted_memory = request.bound_query_text.size();
  if (!CheckedBoundSearchAdd(accounted_memory,
                             request.filter.category_text.size(),
                             &accounted_memory) ||
      !CheckedBoundSearchAdd(accounted_memory,
                             request.output_descriptors.size() * 256U,
                             &accounted_memory) ||
      accounted_memory > request.maximum_memory_bytes) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "bound search pre-access memory contract is exceeded");
  }
  BoundSearchFloatingEnvironment floating_environment;
  if (!floating_environment.active()) {
    return refuse("SB_MODEL_SEARCH_SCORE_INVALID_V1",
                  "round-to-nearest floating-point environment is unavailable");
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
                  "current materialized search SELECT authorization is stale");
  }

  // Catalog/type admission is metadata-only. Bind the exact current relation
  // before any segment provider or MGA heap row may be inspected.
  const auto preflight = LoadMgaRelationStorageDescriptor(
      request.context, request.collection_uuid);
  if (!preflight.ok ||
      preflight.descriptor.database_uuid.canonical !=
          request.context.database_uuid.canonical ||
      preflight.descriptor.descriptor_uuid.canonical !=
          request.expected_descriptor_uuid ||
      preflight.descriptor.descriptor_generation !=
          request.expected_descriptor_generation ||
      !ExactBoundSearchStorageDescriptorV1(preflight.descriptor,
                                           request.collection_uuid)) {
    return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                  "current search storage descriptor is outside the exact v1 profile");
  }

  bool segment_carrier_loaded = false;
  bool exact_fallback_selected = false;
  EngineNoSqlProviderGenerationMetadata carrier;
  if (request.physical_route ==
      EngineBoundSearchPhysicalRouteV1::kSegmentWithExactFallback) {
    const auto loaded = LoadNoSqlProviderGeneration(
        request.context, EngineNoSqlProviderFamily::kSearch,
        request.selected_provider_uuid, request.collection_uuid);
    if (!loaded.ok) {
      if (loaded.diagnostic.detail == kNoSqlProviderGenerationUnavailable ||
          loaded.diagnostic.detail.ends_with(
              std::string(":") + kNoSqlProviderGenerationUnavailable)) {
        exact_fallback_selected = true;
      } else {
        return refuse("SB_MODEL_PROVIDER_GENERATION_STALE_V1",
                      "search segment carrier is corrupt, partial, or duplicated");
      }
    } else if (!ValidateSearchSegmentCapabilityBindingV1(loaded.metadata) ||
               !BoundSearchCarrierContextMatches(request, loaded.metadata) ||
               !BoundSearchCarrierDescriptorMatches(loaded.metadata,
                                                    preflight.descriptor)) {
      return refuse("SB_MODEL_PROVIDER_GENERATION_STALE_V1",
                    "search segment identity or statement cohort is stale");
    } else {
      segment_carrier_loaded = true;
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
                    "search execution cancelled during MGA base read");
    }
    if (read.diagnostic.detail.find("maximum") != std::string::npos ||
        read.diagnostic.detail.find("bound") != std::string::npos ||
        read.diagnostic.detail.find("overflow") != std::string::npos) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "bounded MGA search base read exceeded its contract");
    }
    return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                  "current MGA-visible search base relation is unavailable");
  }
  if (!ExactBoundSearchStorageDescriptorV1(read.descriptor,
                                           request.collection_uuid) ||
      read.descriptor.descriptor_uuid.canonical !=
          preflight.descriptor.descriptor_uuid.canonical ||
      read.descriptor.descriptor_generation !=
          preflight.descriptor.descriptor_generation ||
      read.current_relation_base_generation == 0) {
    return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                  "current search storage descriptor/generation is stale");
  }
  // The ordinary heap projection intentionally returns one newest visible
  // version per row UUID.  Search has one additional, stricter invariant:
  // two independently-current chain heads cannot both claim the same hidden
  // document UUID.  Inspect the same transaction-inventory-authorized scoped
  // rows and distinguish ordinary linked versions (a visible successor names
  // its predecessor) from disconnected current heads before analysis.
  const auto scoped_rows =
      LoadMgaRelationStoreRowsOnlyForMutationTarget(request.context,
                                                    request.collection_uuid);
  if (!scoped_rows.ok ||
      scoped_rows.state.row_versions.size() >
          request.maximum_scanned_row_versions) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "bounded duplicate-document recheck is unavailable");
  }
  std::unordered_set<std::string> visible_version_uuids;
  std::unordered_set<std::string> visible_predecessor_uuids;
  for (const auto& row : scoped_rows.state.row_versions) {
    if (row.table_uuid == request.collection_uuid &&
        CrudRowVersionVisibleToContext(scoped_rows.state.crud_metadata, row,
                                       request.context)) {
      visible_version_uuids.insert(row.version_uuid);
    }
  }
  for (const auto& row : scoped_rows.state.row_versions) {
    if (visible_version_uuids.contains(row.version_uuid) &&
        !row.previous_version_uuid.empty() &&
        visible_version_uuids.contains(row.previous_version_uuid)) {
      visible_predecessor_uuids.insert(row.previous_version_uuid);
    }
  }
  std::unordered_set<std::string> current_document_uuids;
  for (const auto& row : scoped_rows.state.row_versions) {
    if (!visible_version_uuids.contains(row.version_uuid) || row.deleted ||
        visible_predecessor_uuids.contains(row.version_uuid)) {
      continue;
    }
    if (!current_document_uuids.insert(row.row_uuid).second) {
      return refuse(
          "SB_MODEL_SEARCH_DUPLICATE_VISIBLE_DOCUMENT_UUID_REFUSED_V1",
          "multiple current visible chain heads claim one document UUID");
    }
  }
  bool segment_candidate_hint_selected = false;
  if (segment_carrier_loaded) {
    if (!BoundSearchCarrierDescriptorMatches(carrier, read.descriptor) ||
        carrier.search_segment_base_relation_generation >
            read.current_relation_base_generation) {
      return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                    "search segment is ahead of or substituted for the base");
    }
    if (carrier.search_segment_base_relation_generation <
            read.current_relation_base_generation ||
        (request.operation == EngineBoundSearchOperationV1::kPhrase &&
         !carrier.search_segment_position_payload_present)) {
      exact_fallback_selected = true;
    } else {
      segment_candidate_hint_selected = true;
    }
  }

  std::vector<BoundSearchDocument> documents;
  documents.reserve(read.visible_rows.size());
  std::unordered_set<std::string> visible_document_uuids;
  std::uint64_t analyzed_tokens = 0;
  std::uint64_t analyzed_positions = 0;
  std::uint64_t filtered_rows = 0;
  for (const auto& base_row : read.visible_rows) {
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "search execution cancelled during row validation");
    }
    if (!CanonicalBoundSearchUuid(base_row.row_uuid) ||
        !visible_document_uuids.insert(base_row.row_uuid).second) {
      return refuse("SB_MODEL_SEARCH_DUPLICATE_VISIBLE_DOCUMENT_UUID_REFUSED_V1",
                    "visible search document UUID is invalid or duplicated");
    }
    if (base_row.values.size() != 2 ||
        base_row.values[0].first != "body" ||
        base_row.values[1].first != "category" ||
        !ValidBoundSearchUtf8(base_row.values[1].second) ||
        base_row.values[1].second.size() > 4096) {
      return refuse("SB_MODEL_SEARCH_DOCUMENT_INVALID_V1",
                    "visible search row does not match BODY/CATEGORY TEXT");
    }
    const auto analysis = AnalyzeBoundSearchAscii(base_row.values[0].second);
    if (!analysis.ok) {
      return refuse(analysis.input_too_large || analysis.token_too_large
                        ? "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1"
                        : "SB_MODEL_SEARCH_DOCUMENT_INVALID_V1",
                    "visible search body is outside the bound analyzer");
    }
    if (!CheckedBoundSearchAdd(analyzed_tokens, analysis.tokens.size(),
                               &analyzed_tokens) ||
        !CheckedBoundSearchAdd(analyzed_positions, analysis.tokens.size(),
                               &analyzed_positions) ||
        analyzed_tokens > request.maximum_tokens ||
        analyzed_positions > request.maximum_positions) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "search token/position contract is exceeded");
    }
    std::uint64_t row_memory = sizeof(BoundSearchDocument);
    if (!CheckedBoundSearchAdd(row_memory, base_row.row_uuid.size(),
                               &row_memory) ||
        !CheckedBoundSearchAdd(row_memory, base_row.values[0].second.size(),
                               &row_memory) ||
        !CheckedBoundSearchAdd(row_memory, base_row.values[1].second.size(),
                               &row_memory) ||
        !CheckedBoundSearchAdd(accounted_memory, row_memory,
                               &accounted_memory) ||
        accounted_memory > request.maximum_memory_bytes) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "search corpus analysis memory contract is exceeded");
    }
    if (request.filter.present &&
        base_row.values[1].second != request.filter.category_text) {
      ++filtered_rows;
      continue;
    }
    documents.push_back({base_row.row_uuid, base_row.values[1].second,
                         analysis.tokens});
  }
  if (documents.size() > request.maximum_candidates) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "search candidate contract is exceeded");
  }

  const auto unique_query = UniqueBoundSearchTerms(query.tokens);
  std::map<std::string, std::uint64_t> document_frequency;
  std::uint64_t total_document_tokens = 0;
  std::uint64_t fuzzy_document_frequency = 0;
  for (const auto& document : documents) {
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "search execution cancelled during statistics");
    }
    total_document_tokens += document.tokens.size();
    const std::unordered_set<std::string> document_terms(
        document.tokens.begin(), document.tokens.end());
    for (const auto& term : unique_query) {
      if (document_terms.contains(term)) ++document_frequency[term];
    }
    if (request.operation == EngineBoundSearchOperationV1::kFuzzy &&
        std::ranges::any_of(document.tokens, [&](const auto& term) {
          return BoundSearchLevenshteinAtMostOne(term, query.tokens.front()) <= 1;
        })) {
      ++fuzzy_document_frequency;
    }
  }
  const double average_document_length =
      documents.empty()
          ? 0.0
          : static_cast<double>(total_document_tokens) /
                static_cast<double>(documents.size());

  std::vector<BoundSearchScoredRow> scored_rows;
  for (const auto& document : documents) {
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "search execution cancelled during exact scoring");
    }
    std::map<std::string, std::uint64_t> term_frequency;
    for (const auto& term : document.tokens) ++term_frequency[term];
    const bool phrase_match =
        request.operation != EngineBoundSearchOperationV1::kPhrase ||
        BoundSearchPhraseMatch(document.tokens, query.tokens);
    std::uint64_t fuzzy_tf = 0;
    if (request.operation == EngineBoundSearchOperationV1::kFuzzy) {
      for (const auto& term : document.tokens) {
        fuzzy_tf += BoundSearchLevenshteinAtMostOne(
                        term, query.tokens.front()) <= 1;
      }
    }
    const bool eligible = phrase_match &&
        (request.operation == EngineBoundSearchOperationV1::kFuzzy
             ? fuzzy_tf != 0
             : std::ranges::any_of(unique_query, [&](const auto& term) {
                 return term_frequency[term] != 0;
               }));
    if (!eligible) continue;
    volatile double score = 0.0;
    const auto contribute = [&](const std::uint64_t tf,
                                const std::uint64_t df) -> bool {
      if (tf == 0 || df == 0 || df > documents.size() ||
          average_document_length <= 0.0) {
        return false;
      }
      volatile double n = static_cast<double>(documents.size());
      volatile double frequency = static_cast<double>(df);
      volatile double ratio = (n - frequency + 0.5) / (frequency + 0.5);
      volatile double idf = std::log1p(static_cast<double>(ratio));
      volatile double normalized =
          static_cast<double>(document.tokens.size()) /
          average_document_length;
      volatile double denominator =
          static_cast<double>(tf) +
          1.2 * (1.0 - 0.75 + 0.75 * normalized);
      volatile double numerator = static_cast<double>(tf) * (1.2 + 1.0);
      volatile double contribution = idf * numerator / denominator;
      if (!std::isfinite(static_cast<double>(contribution)) ||
          contribution <= 0.0) {
        return false;
      }
      score = score + contribution;
      return true;
    };
    bool valid_score = true;
    if (request.operation == EngineBoundSearchOperationV1::kFuzzy) {
      valid_score = contribute(fuzzy_tf, fuzzy_document_frequency);
    } else {
      for (const auto& term : unique_query) {
        if (term_frequency[term] != 0 &&
            !contribute(term_frequency[term], document_frequency[term])) {
          valid_score = false;
          break;
        }
      }
    }
    BoundSearchScoredRow scored;
    scored.document_uuid = document.document_uuid;
    scored.score = static_cast<double>(score);
    scored.encoded_score = CanonicalBoundSearchReal64(scored.score);
    if (!valid_score || !std::isfinite(scored.score) || scored.score <= 0.0 ||
        scored.encoded_score.empty()) {
      return refuse("SB_MODEL_SEARCH_SCORE_INVALID_V1",
                    "search BM25 score is invalid");
    }
    scored_rows.push_back(std::move(scored));
    if (scored_rows.size() > request.maximum_scored_rows) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "search scored-row contract is exceeded");
    }
  }
  std::ranges::sort(scored_rows, [](const auto& left, const auto& right) {
    if (left.score != right.score) return left.score > right.score;
    return left.document_uuid < right.document_uuid;
  });
  if (scored_rows.size() > request.top_k) scored_rows.resize(request.top_k);
  if (cancelled()) {
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "search execution cancelled before materialization");
  }

  EngineBoundSearchReadResultV1 result;
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
  result.analyzed_token_count = analyzed_tokens;
  result.analyzed_position_count = analyzed_positions;
  result.scored_base_row_count = scored_rows.size();
  result.segment_carrier_loaded = segment_carrier_loaded;
  result.segment_candidate_hint_selected = segment_candidate_hint_selected;
  result.exact_fallback_selected = exact_fallback_selected;
  result.full_corpus_exact_recheck_complete = true;
  result.base_row_mga_recheck_complete = true;
  result.security_recheck_complete = true;
  result.execution_resource_acquired = true;
  result.cleanup_count = 1;
  result.rows.reserve(scored_rows.size());
  std::uint64_t rank = 1;
  for (const auto& scored : scored_rows) {
    std::uint64_t row_bytes = scored.document_uuid.size();
    if (!CheckedBoundSearchAdd(row_bytes, request.analyzer_uuid.size(),
                               &row_bytes) ||
        !CheckedBoundSearchAdd(row_bytes, scored.encoded_score.size(),
                               &row_bytes) ||
        !CheckedBoundSearchAdd(row_bytes, 32, &row_bytes) ||
        !CheckedBoundSearchAdd(result.result_byte_count, row_bytes,
                               &result.result_byte_count) ||
        result.result_byte_count > request.maximum_decoded_bytes) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "search result byte contract is exceeded");
    }
    result.rows.push_back({scored.document_uuid, request.analyzer_uuid,
                           request.analyzer_generation, scored.score, rank++,
                           scored.encoded_score});
  }
  if (cancelled()) {
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "search execution cancelled before atomic publication");
  }
  result.evidence.push_back(
      {"bound_search_physical_route",
       segment_candidate_hint_selected
           ? "segment_hint_full_corpus_exact_recheck"
           : "exact_full_corpus_scan"});
  result.evidence.push_back(
      {"bound_search_exact_fallback_selected",
       exact_fallback_selected ? "true" : "false"});
  result.evidence.push_back(
      {"bound_search_result_ordering",
       "score_numeric_descending_then_document_uuid_bytes_ascending"});
  result.evidence.push_back(
      {"bound_search_full_corpus_exact_recheck", "true"});
  result.evidence.push_back(
      {"bound_search_mga_authority", "engine_transaction_inventory"});
  result.evidence.push_back(
      {"bound_search_segment_visibility_authority", "false"});
  result.evidence.push_back(
      {"bound_search_segment_finality_authority", "false"});
  return result;
}

}  // namespace scratchbird::engine::internal_api
