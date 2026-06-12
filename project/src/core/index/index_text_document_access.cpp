// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_text_document_access.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status ErrorStatus() { return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine}; }

TextDictionaryMode DictionaryModeForFamily(IndexFamily family, bool multikey_required, bool document_path_required) {
  if (family == IndexFamily::document_path || document_path_required) return TextDictionaryMode::document_path_dictionary;
  if (family == IndexFamily::ngram) return TextDictionaryMode::ngram_dictionary;
  if (family == IndexFamily::gin || multikey_required) return TextDictionaryMode::multikey_dictionary;
  return TextDictionaryMode::term_dictionary;
}

TextPostingPayload PostingPayloadForRequest(const TextDictionaryPostingRequest& request) {
  if (request.family == IndexFamily::document_path || request.document_path_required) return TextPostingPayload::document_path_payload;
  if (request.family == IndexFamily::gin || request.multikey_required) return TextPostingPayload::gin_payload;
  if (request.ranking_required) return TextPostingPayload::rank_payload;
  if (request.phrase_required) return TextPostingPayload::phrase_positions;
  if (request.positions_required) return TextPostingPayload::positions;
  return TextPostingPayload::locator_only;
}

TextDictionaryPostingDecision DictionaryPostingError(const TextDictionaryPostingRequest& request,
                                                    TextAccessDecisionAction action,
                                                    std::string diagnostic_code,
                                                    std::string message_key,
                                                    std::string detail) {
  TextDictionaryPostingDecision decision;
  decision.status = ErrorStatus();
  decision.action = action;
  decision.dictionary_mode = DictionaryModeForFamily(request.family, request.multikey_required, request.document_path_required);
  decision.posting_payload = PostingPayloadForRequest(request);
  decision.requires_segment_merge = request.segment_merge_required;
  decision.diagnostic = MakeTextDocumentAccessDiagnostic(decision.status, std::move(diagnostic_code),
                                                        std::move(message_key), std::move(detail));
  return decision;
}

TextAnalyzerResourceDecision ResourceError(IndexResourceAction action,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail) {
  TextAnalyzerResourceDecision decision;
  decision.status = ErrorStatus();
  decision.action = action;
  decision.mark_index_stale = action == IndexResourceAction::mark_stale;
  decision.rebuild_required = action == IndexResourceAction::rebuild_required ||
                              action == IndexResourceAction::mark_stale;
  decision.diagnostic = MakeTextDocumentAccessDiagnostic(decision.status, std::move(diagnostic_code),
                                                        std::move(message_key), std::move(detail));
  return decision;
}

TextQueryPathDecision QueryPathError(std::string diagnostic_code,
                                     std::string message_key,
                                     std::string detail) {
  TextQueryPathDecision decision;
  decision.status = ErrorStatus();
  decision.path = TextQueryPath::refused;
  decision.diagnostic = MakeTextDocumentAccessDiagnostic(decision.status, std::move(diagnostic_code),
                                                        std::move(message_key), std::move(detail));
  return decision;
}

GinConsistencyDecision GinError(std::string diagnostic_code,
                                std::string message_key,
                                std::string detail,
                                bool policy_blocked = false) {
  GinConsistencyDecision decision;
  decision.status = ErrorStatus();
  decision.policy_blocked = policy_blocked;
  decision.diagnostic = MakeTextDocumentAccessDiagnostic(decision.status, std::move(diagnostic_code),
                                                        std::move(message_key), std::move(detail));
  return decision;
}

SparseWandPruningDecision WandError(std::string diagnostic_code,
                                    std::string message_key,
                                    std::string detail) {
  SparseWandPruningDecision decision;
  decision.status = ErrorStatus();
  decision.diagnostic = MakeTextDocumentAccessDiagnostic(decision.status, std::move(diagnostic_code),
                                                        std::move(message_key), std::move(detail));
  return decision;
}

TextHelperProfileDecision HelperError(std::string diagnostic_code,
                                      std::string message_key,
                                      std::string detail) {
  TextHelperProfileDecision decision;
  decision.status = ErrorStatus();
  decision.diagnostic = MakeTextDocumentAccessDiagnostic(decision.status, std::move(diagnostic_code),
                                                        std::move(message_key), std::move(detail));
  return decision;
}

DocumentPathAccessDecision DocumentError(std::string diagnostic_code,
                                         std::string message_key,
                                         std::string detail,
                                         bool policy_blocked = false) {
  DocumentPathAccessDecision decision;
  decision.status = ErrorStatus();
  decision.policy_blocked = policy_blocked;
  decision.diagnostic = MakeTextDocumentAccessDiagnostic(decision.status, std::move(diagnostic_code),
                                                        std::move(message_key), std::move(detail));
  return decision;
}

TextDocumentProfileDecision ProfileError(const TextDocumentSemanticProfileDescriptor* descriptor,
                                         std::string diagnostic_code,
                                         std::string message_key,
                                         std::string detail,
                                         bool policy_blocked = false) {
  TextDocumentProfileDecision decision;
  decision.status = ErrorStatus();
  decision.descriptor = descriptor;
  decision.policy_blocked = policy_blocked;
  decision.metrics.count_profile_refusal = true;
  if (descriptor != nullptr) {
    decision.native_family = descriptor->native_family;
    decision.semantic_profile_id = descriptor->semantic_profile_id;
    decision.active_controls = descriptor->controls;
    decision.metrics.metrics_prefix = descriptor->metrics_prefix;
  }
  decision.diagnostic = MakeTextDocumentAccessDiagnostic(decision.status, std::move(diagnostic_code),
                                                        std::move(message_key), std::move(detail));
  return decision;
}

std::string TrimAscii(std::string_view text) {
  std::size_t first = 0;
  std::size_t last = text.size();
  while (first < last && std::isspace(static_cast<unsigned char>(text[first])) != 0) ++first;
  while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0) --last;
  return std::string(text.substr(first, last - first));
}

bool NormalizeDocumentPath(std::string_view raw_path, bool wildcard_requested, std::string* normalized) {
  const std::string trimmed = TrimAscii(raw_path);
  if (trimmed.empty()) return false;
  if (wildcard_requested && (trimmed == "*" || trimmed == "$*" || trimmed == "$**" || trimmed == "**")) {
    *normalized = trimmed == "$*" ? "*" : (trimmed == "$**" ? "**" : trimmed);
    return true;
  }

  std::string out;
  bool previous_separator = false;
  for (std::size_t i = 0; i < trimmed.size(); ++i) {
    const char c = trimmed[i];
    if (std::iscntrl(static_cast<unsigned char>(c)) != 0) return false;
    if (c == '$' && i == 0) {
      previous_separator = true;
      continue;
    }
    if (c == '.' || c == '/') {
      if (!out.empty() && !previous_separator) {
        out.push_back('.');
        previous_separator = true;
      }
      continue;
    }
    if (c == '[' || c == ']') return false;
    if (c == '*') {
      if (!wildcard_requested) return false;
      if (!out.empty() && !previous_separator) out.push_back('.');
      out.push_back('*');
      previous_separator = false;
      continue;
    }
    out.push_back(c);
    previous_separator = false;
  }

  while (!out.empty() && out.back() == '.') out.pop_back();
  if (out.empty()) {
    *normalized = "$";
    return true;
  }
  *normalized = std::move(out);
  return true;
}

TextDocumentProfileControls Controls(bool analyzer,
                                     bool tokenizer,
                                     bool case_behavior,
                                     bool stemming,
                                     bool stopwords,
                                     bool phrase,
                                     bool ranking,
                                     bool wildcard,
                                     bool document_path,
                                     bool catalog_projection) {
  TextDocumentProfileControls controls;
  controls.analyzer = analyzer;
  controls.tokenizer = tokenizer;
  controls.case_behavior = case_behavior;
  controls.stemming = stemming;
  controls.stopwords = stopwords;
  controls.phrase = phrase;
  controls.ranking = ranking;
  controls.wildcard = wildcard;
  controls.document_path = document_path;
  controls.catalog_projection = catalog_projection;
  return controls;
}

TextDocumentSemanticProfileDescriptor Profile(TextDocumentReference reference,
                                              TextDocumentReferenceIndexType index_type,
                                              IndexFamily native_family,
                                              const char* native_physical_family,
                                              const char* semantic_profile_id,
                                              TextDocumentCompatibilityLevel compatibility,
                                              TextDocumentProfileControls controls,
                                              bool requires_recheck,
                                              bool fallback_sort_possible,
                                              bool fallback_sort_required,
                                              const char* catalog_projection,
                                              const char* unsupported_behavior,
                                              const char* metrics_prefix,
                                              const char* diagnostics_key) {
  TextDocumentSemanticProfileDescriptor descriptor;
  descriptor.reference = reference;
  descriptor.index_type = index_type;
  descriptor.native_family = native_family;
  descriptor.native_physical_family = native_physical_family;
  descriptor.semantic_profile_id = semantic_profile_id;
  descriptor.compatibility = compatibility;
  descriptor.controls = controls;
  descriptor.requires_recheck = requires_recheck;
  descriptor.fallback_sort_possible = fallback_sort_possible;
  descriptor.fallback_sort_required = fallback_sort_required;
  descriptor.catalog_projection = catalog_projection;
  descriptor.unsupported_behavior = unsupported_behavior;
  descriptor.metrics_prefix = metrics_prefix;
  descriptor.diagnostics_key = diagnostics_key;
  return descriptor;
}
}  // namespace

const char* TextDictionaryModeName(TextDictionaryMode mode) {
  switch (mode) {
    case TextDictionaryMode::term_dictionary: return "term_dictionary";
    case TextDictionaryMode::multikey_dictionary: return "multikey_dictionary";
    case TextDictionaryMode::ngram_dictionary: return "ngram_dictionary";
    case TextDictionaryMode::document_path_dictionary: return "document_path_dictionary";
  }
  return "unknown";
}

const char* TextPostingPayloadName(TextPostingPayload payload) {
  switch (payload) {
    case TextPostingPayload::locator_only: return "locator_only";
    case TextPostingPayload::positions: return "positions";
    case TextPostingPayload::phrase_positions: return "phrase_positions";
    case TextPostingPayload::rank_payload: return "rank_payload";
    case TextPostingPayload::document_path_payload: return "document_path_payload";
    case TextPostingPayload::gin_payload: return "gin_payload";
  }
  return "unknown";
}

const char* TextAccessDecisionActionName(TextAccessDecisionAction action) {
  switch (action) {
    case TextAccessDecisionAction::use_existing: return "use_existing";
    case TextAccessDecisionAction::build_or_rebuild: return "build_or_rebuild";
    case TextAccessDecisionAction::mark_stale: return "mark_stale";
    case TextAccessDecisionAction::refuse: return "refuse";
  }
  return "unknown";
}

const char* TextQueryOperatorName(TextQueryOperator op) {
  switch (op) {
    case TextQueryOperator::term: return "term";
    case TextQueryOperator::all_terms: return "all_terms";
    case TextQueryOperator::phrase: return "phrase";
    case TextQueryOperator::rank: return "rank";
    case TextQueryOperator::rerank: return "rerank";
    case TextQueryOperator::exact_source_recheck: return "exact_source_recheck";
  }
  return "unknown";
}

const char* TextQueryPathName(TextQueryPath path) {
  switch (path) {
    case TextQueryPath::term_posting_scan: return "term_posting_scan";
    case TextQueryPath::all_terms_intersection: return "all_terms_intersection";
    case TextQueryPath::phrase_position_scan: return "phrase_position_scan";
    case TextQueryPath::rank_payload_scan: return "rank_payload_scan";
    case TextQueryPath::rerank_source_scan: return "rerank_source_scan";
    case TextQueryPath::exact_source_scan: return "exact_source_scan";
    case TextQueryPath::refused: return "refused";
  }
  return "unknown";
}

const char* TextHelperProfileKindName(TextHelperProfileKind kind) {
  switch (kind) {
    case TextHelperProfileKind::ngram: return "ngram";
    case TextHelperProfileKind::trigram: return "trigram";
    case TextHelperProfileKind::token_bloom: return "token_bloom";
  }
  return "unknown";
}

const char* DocumentValuePolicyName(DocumentValuePolicy policy) {
  switch (policy) {
    case DocumentValuePolicy::skip: return "skip";
    case DocumentValuePolicy::index_presence: return "index_presence";
    case DocumentValuePolicy::index_value: return "index_value";
    case DocumentValuePolicy::expand_elements: return "expand_elements";
    case DocumentValuePolicy::refuse: return "refuse";
  }
  return "unknown";
}

const char* TextDocumentReferenceName(TextDocumentReference reference) {
  switch (reference) {
    case TextDocumentReference::native: return "native";
    case TextDocumentReference::mongodb: return "mongodb";
    case TextDocumentReference::opensearch: return "opensearch";
    case TextDocumentReference::neo4j: return "neo4j";
    case TextDocumentReference::postgresql: return "postgresql";
    case TextDocumentReference::cassandra: return "cassandra";
    case TextDocumentReference::mysql: return "mysql";
    case TextDocumentReference::mariadb: return "mariadb";
    case TextDocumentReference::unknown: return "unknown";
  }
  return "unknown";
}

const char* TextDocumentReferenceIndexTypeName(TextDocumentReferenceIndexType type) {
  switch (type) {
    case TextDocumentReferenceIndexType::native_full_text: return "native_full_text";
    case TextDocumentReferenceIndexType::native_inverted: return "native_inverted";
    case TextDocumentReferenceIndexType::native_document_path: return "native_document_path";
    case TextDocumentReferenceIndexType::mongodb_text: return "mongodb_text";
    case TextDocumentReferenceIndexType::mongodb_wildcard: return "mongodb_wildcard";
    case TextDocumentReferenceIndexType::opensearch_text: return "opensearch_text";
    case TextDocumentReferenceIndexType::neo4j_text: return "neo4j_text";
    case TextDocumentReferenceIndexType::neo4j_fulltext: return "neo4j_fulltext";
    case TextDocumentReferenceIndexType::postgresql_gin: return "postgresql_gin";
    case TextDocumentReferenceIndexType::postgresql_tsvector: return "postgresql_tsvector";
    case TextDocumentReferenceIndexType::cassandra_sai_text: return "cassandra_sai_text";
    case TextDocumentReferenceIndexType::cassandra_sasi_text: return "cassandra_sasi_text";
    case TextDocumentReferenceIndexType::mysql_fulltext: return "mysql_fulltext";
    case TextDocumentReferenceIndexType::mariadb_fulltext: return "mariadb_fulltext";
    case TextDocumentReferenceIndexType::unknown: return "unknown";
  }
  return "unknown";
}

const char* TextDocumentCompatibilityLevelName(TextDocumentCompatibilityLevel level) {
  switch (level) {
    case TextDocumentCompatibilityLevel::exact: return "exact";
    case TextDocumentCompatibilityLevel::compatible_with_recheck: return "compatible_with_recheck";
    case TextDocumentCompatibilityLevel::compatible_with_fallback_sort: return "compatible_with_fallback_sort";
    case TextDocumentCompatibilityLevel::catalog_only_projection: return "catalog_only_projection";
    case TextDocumentCompatibilityLevel::policy_blocked: return "policy_blocked";
    case TextDocumentCompatibilityLevel::rejected: return "rejected";
  }
  return "unknown";
}

bool IsTextDocumentIndexFamily(IndexFamily family) {
  switch (family) {
    case IndexFamily::full_text:
    case IndexFamily::gin:
    case IndexFamily::inverted:
    case IndexFamily::ngram:
    case IndexFamily::sparse_wand:
    case IndexFamily::document_path:
      return true;
    default:
      return false;
  }
}

TextDictionaryPostingDecision DecideTextDictionaryPostings(const TextDictionaryPostingRequest& request) {
  if (!IsTextDocumentIndexFamily(request.family)) {
    return DictionaryPostingError(request, TextAccessDecisionAction::refuse,
                                  "SB-INDEX-TEXT-DOC-FAMILY-REFUSED",
                                  "index.text_document.family_refused",
                                  IndexFamilyName(request.family));
  }
  if (!request.resource_epoch_current) {
    return DictionaryPostingError(request, TextAccessDecisionAction::mark_stale,
                                  "SB-INDEX-TEXT-DOC-RESOURCE-EPOCH-STALE",
                                  "index.text_document.resource_epoch_stale",
                                  IndexFamilyName(request.family));
  }
  if (!request.dictionary_present || !request.posting_segments_present) {
    return DictionaryPostingError(request, TextAccessDecisionAction::build_or_rebuild,
                                  "SB-INDEX-TEXT-DOC-DICTIONARY-POSTINGS-MISSING",
                                  "index.text_document.dictionary_postings_missing",
                                  IndexFamilyName(request.family));
  }

  TextDictionaryPostingDecision decision;
  decision.status = OkStatus();
  decision.action = TextAccessDecisionAction::use_existing;
  decision.dictionary_mode = DictionaryModeForFamily(request.family, request.multikey_required, request.document_path_required);
  decision.posting_payload = PostingPayloadForRequest(request);
  decision.persist_dictionary = true;
  decision.persist_positions = request.positions_required || request.phrase_required || request.family == IndexFamily::full_text;
  decision.persist_rank_payload = request.ranking_required || request.family == IndexFamily::sparse_wand;
  decision.requires_exact_recheck = request.allow_lossy_candidates ||
                                    request.family == IndexFamily::gin ||
                                    request.family == IndexFamily::ngram ||
                                    request.family == IndexFamily::sparse_wand ||
                                    request.family == IndexFamily::document_path;
  decision.requires_segment_merge = request.segment_merge_required ||
                                    request.family == IndexFamily::full_text ||
                                    request.family == IndexFamily::inverted;
  return decision;
}

TextAnalyzerResourceDecision DecideTextAnalyzerResources(const TextAnalyzerResourceRequest& request) {
  if (!request.deterministic) {
    return ResourceError(IndexResourceAction::refuse,
                         "SB-INDEX-TEXT-DOC-RESOURCE-NONDETERMINISTIC",
                         "index.text_document.resource_nondeterministic",
                         "tokenizer/analyzer");
  }
  if (!request.tokenizer_registered) {
    return ResourceError(IndexResourceAction::refuse,
                         "SB-INDEX-TEXT-DOC-TOKENIZER-MISSING",
                         "index.seed.tokenizer_missing",
                         "tokenizer");
  }
  if (!request.analyzer_registered) {
    return ResourceError(IndexResourceAction::refuse,
                         "SB-INDEX-TEXT-DOC-ANALYZER-MISSING",
                         "index.text_document.analyzer_missing",
                         "analyzer");
  }
  if (!request.charset_registered) {
    return ResourceError(IndexResourceAction::refuse,
                         "SB-INDEX-TEXT-DOC-CHARSET-MISSING",
                         "index.seed.charset_missing",
                         "charset");
  }
  if (request.stemmer_required && !request.stemmer_registered) {
    return ResourceError(IndexResourceAction::refuse,
                         "SB-INDEX-TEXT-DOC-STEMMER-MISSING",
                         "index.text_document.stemmer_missing",
                         "stemmer");
  }
  if (request.stopword_resource_required && !request.stopword_resource_registered &&
      !request.empty_stopword_policy_allowed) {
    return ResourceError(IndexResourceAction::refuse,
                         "SB-INDEX-TEXT-DOC-STOPWORDS-MISSING",
                         "index.seed.stopwords_missing",
                         "stopwords");
  }

  const bool epoch_current = request.tokenizer_epoch_current &&
                             request.analyzer_epoch_current &&
                             request.charset_epoch_current &&
                             (!request.stemmer_required || request.stemmer_epoch_current) &&
                             (!request.stopword_resource_required ||
                              !request.stopword_resource_registered ||
                              request.stopword_epoch_current);
  if (!epoch_current) {
    return ResourceError(request.mark_stale_on_epoch_mismatch ? IndexResourceAction::mark_stale
                                                              : IndexResourceAction::rebuild_required,
                         "SB-INDEX-TEXT-DOC-RESOURCE-EPOCH-STALE",
                         "index.text_document.resource_epoch_stale",
                         "analyzer/tokenizer");
  }

  TextAnalyzerResourceDecision decision;
  decision.status = OkStatus();
  decision.action = IndexResourceAction::usable;
  decision.tokenizer_usable = true;
  decision.analyzer_usable = true;
  decision.charset_usable = true;
  decision.stemmer_usable = request.stemmer_required;
  decision.stopword_resource_usable = request.stopword_resource_required && request.stopword_resource_registered;
  decision.empty_stopword_policy_used = request.stopword_resource_required &&
                                        !request.stopword_resource_registered &&
                                        request.empty_stopword_policy_allowed;
  return decision;
}

TextQueryPathDecision DecideTextQueryPath(const TextQueryPathRequest& request) {
  if (!request.analyzer_epoch_current) {
    return QueryPathError("SB-INDEX-TEXT-DOC-QUERY-RESOURCE-EPOCH-STALE",
                          "index.text_document.query_resource_epoch_stale",
                          TextQueryOperatorName(request.operation));
  }
  if (request.term_count == 0) {
    return QueryPathError("SB-INDEX-TEXT-DOC-QUERY-EMPTY",
                          "index.text_document.query_empty",
                          TextQueryOperatorName(request.operation));
  }
  if (!request.postings_available && request.operation != TextQueryOperator::exact_source_recheck) {
    return QueryPathError("SB-INDEX-TEXT-DOC-POSTINGS-MISSING",
                          "index.text_document.postings_missing",
                          TextQueryOperatorName(request.operation));
  }

  TextQueryPathDecision decision;
  decision.status = OkStatus();
  decision.use_term_postings = request.operation != TextQueryOperator::exact_source_recheck;
  decision.exact_source_recheck = request.lossy_candidates || request.reference_requires_recheck;

  switch (request.operation) {
    case TextQueryOperator::term:
      decision.path = TextQueryPath::term_posting_scan;
      break;
    case TextQueryOperator::all_terms:
      decision.path = TextQueryPath::all_terms_intersection;
      decision.intersect_all_terms = true;
      break;
    case TextQueryOperator::phrase:
      if (!request.phrase_profile_enabled) {
        return QueryPathError("SB-INDEX-TEXT-DOC-PHRASE-PROFILE-REFUSED",
                              "index.text_document.phrase_profile_refused",
                              "phrase");
      }
      decision.verify_phrase_boundary = true;
      decision.exact_source_recheck = true;
      if (request.positions_available) {
        decision.path = TextQueryPath::phrase_position_scan;
      } else if (request.exact_source_available) {
        decision.path = TextQueryPath::exact_source_scan;
        decision.use_term_postings = true;
      } else {
        return QueryPathError("SB-INDEX-TEXT-DOC-PHRASE-POSITIONS-MISSING",
                              "index.text_document.phrase_positions_missing",
                              "phrase");
      }
      break;
    case TextQueryOperator::rank:
      if (!request.ranking_profile_enabled) {
        return QueryPathError("SB-INDEX-TEXT-DOC-RANK-PROFILE-REFUSED",
                              "index.text_document.rank_profile_refused",
                              "rank");
      }
      if (request.rank_payload_available) {
        decision.path = TextQueryPath::rank_payload_scan;
        decision.use_rank_payload = true;
      } else if (request.exact_source_available) {
        decision.path = TextQueryPath::rerank_source_scan;
        decision.rerank_required = true;
        decision.exact_source_recheck = true;
      } else {
        return QueryPathError("SB-INDEX-TEXT-DOC-RANK-PAYLOAD-MISSING",
                              "index.text_document.rank_payload_missing",
                              "rank");
      }
      break;
    case TextQueryOperator::rerank:
      if (!request.exact_source_available) {
        return QueryPathError("SB-INDEX-TEXT-DOC-RERANK-SOURCE-MISSING",
                              "index.text_document.rerank_source_missing",
                              "rerank");
      }
      decision.path = TextQueryPath::rerank_source_scan;
      decision.rerank_required = true;
      decision.exact_source_recheck = true;
      break;
    case TextQueryOperator::exact_source_recheck:
      if (!request.exact_source_available) {
        return QueryPathError("SB-INDEX-TEXT-DOC-EXACT-SOURCE-MISSING",
                              "index.text_document.exact_source_missing",
                              "exact_source_recheck");
      }
      decision.path = TextQueryPath::exact_source_scan;
      decision.use_term_postings = false;
      decision.exact_source_recheck = true;
      break;
  }

  if (decision.exact_source_recheck && !request.exact_source_available) {
    return QueryPathError("SB-INDEX-TEXT-DOC-RECHECK-SOURCE-MISSING",
                          "index.text_document.recheck_source_missing",
                          TextQueryOperatorName(request.operation));
  }
  return decision;
}

GinConsistencyDecision DecideGinExtractionConsistency(const GinConsistencyRequest& request) {
  if (!request.reference_profile_allows || !request.opclass_known) {
    return GinError("SB-INDEX-TEXT-DOC-GIN-OPCLASS-REFUSED",
                    "index.reference.postgresql.gin",
                    "policy_block_unknown_opclass",
                    true);
  }
  if (!request.extractor_registered || !request.consistency_registered || !request.deterministic) {
    return GinError("SB-INDEX-TEXT-DOC-GIN-HELPER-REFUSED",
                    "index.text_document.gin_helper_refused",
                    "extractor/consistency");
  }
  if (request.extracted_key_count == 0) {
    return GinError("SB-INDEX-TEXT-DOC-GIN-NO-KEYS",
                    "index.text_document.gin_no_keys",
                    "extractor");
  }

  GinConsistencyDecision decision;
  decision.status = OkStatus();
  decision.extract_multikeys = true;
  decision.use_tri_consistent = request.tri_consistent_available;
  decision.run_consistency_function = true;
  decision.consistency_requires_recheck = request.partial_match ||
                                          request.lossy_candidates ||
                                          request.consistency_may_be_lossy;
  decision.exact_source_recheck = decision.consistency_requires_recheck;
  if (decision.exact_source_recheck && !request.exact_recheck_available) {
    return GinError("SB-INDEX-TEXT-DOC-GIN-RECHECK-MISSING",
                    "index.text_document.gin_recheck_missing",
                    "exact_source_recheck");
  }
  return decision;
}

SparseWandPruningDecision DecideSparseWandPruning(const SparseWandPruningRequest& request) {
  SparseWandPruningDecision decision;
  decision.status = OkStatus();
  if (!request.enabled) return decision;

  if (request.terms.empty()) {
    return WandError("SB-INDEX-TEXT-DOC-WAND-EMPTY",
                     "index.text_document.wand_empty",
                     "terms");
  }
  if (!request.scores_monotonic || !request.upper_bounds_current) {
    return WandError("SB-INDEX-TEXT-DOC-WAND-BOUNDS-REFUSED",
                     "index.text_document.wand_bounds_refused",
                     "scores_monotonic_or_bounds");
  }
  if (!request.exact_recheck_available) {
    return WandError("SB-INDEX-TEXT-DOC-WAND-RECHECK-MISSING",
                     "index.text_document.wand_recheck_missing",
                     "exact_source_recheck");
  }

  for (const auto& term : request.terms) {
    if (term.upper_bound_score < 0.0) {
      return WandError("SB-INDEX-TEXT-DOC-WAND-NEGATIVE-BOUND",
                       "index.text_document.wand_negative_bound",
                       "upper_bound_score");
    }
    decision.aggregate_upper_bound_score += term.upper_bound_score;
    if (term.required) ++decision.required_term_count;
  }

  const u32 requested_minimum = request.minimum_should_match == 0 ? 1 : request.minimum_should_match;
  if (requested_minimum > request.terms.size()) {
    return WandError("SB-INDEX-TEXT-DOC-WAND-MINIMUM-REFUSED",
                     "index.text_document.wand_minimum_refused",
                     "minimum_should_match");
  }

  decision.use_wand = true;
  decision.candidate_pruning = request.requested_top_k > 0 || request.score_cutoff > 0.0;
  decision.effective_minimum_should_match = std::max(requested_minimum, decision.required_term_count);
  decision.prune_all_candidates = request.score_cutoff > 0.0 &&
                                  decision.aggregate_upper_bound_score < request.score_cutoff;
  decision.exact_source_recheck = true;
  return decision;
}

TextHelperProfileDecision DecideTextHelperProfile(const TextHelperProfileRequest& request) {
  if (!request.deterministic || !request.charset_epoch_current || !request.tokenizer_epoch_current) {
    return HelperError("SB-INDEX-TEXT-DOC-HELPER-RESOURCE-REFUSED",
                       "index.text_document.helper_resource_refused",
                       TextHelperProfileKindName(request.kind));
  }
  if (!request.exact_recheck_available) {
    return HelperError("SB-INDEX-TEXT-DOC-HELPER-RECHECK-MISSING",
                       "index.text_document.helper_recheck_missing",
                       TextHelperProfileKindName(request.kind));
  }

  TextHelperProfileDecision decision;
  decision.status = OkStatus();
  decision.accepted = true;
  decision.false_positive_possible = true;
  decision.requires_exact_recheck = true;
  decision.metric_visible = true;

  switch (request.kind) {
    case TextHelperProfileKind::ngram:
      if (request.gram_width == 0) {
        return HelperError("SB-INDEX-TEXT-DOC-NGRAM-WIDTH-REFUSED",
                           "index.text_document.ngram_width_refused",
                           "gram_width");
      }
      decision.helper_family = IndexFamily::ngram;
      decision.helper_profile_id = "native_ngram";
      decision.effective_gram_width = request.gram_width;
      break;
    case TextHelperProfileKind::trigram:
      if (request.gram_width != 0 && request.gram_width != 3) {
        return HelperError("SB-INDEX-TEXT-DOC-TRIGRAM-WIDTH-REFUSED",
                           "index.text_document.trigram_width_refused",
                           "gram_width");
      }
      decision.helper_family = IndexFamily::ngram;
      decision.helper_profile_id = "native_trigram";
      decision.effective_gram_width = 3;
      break;
    case TextHelperProfileKind::token_bloom:
      if (request.bloom_bits == 0 || request.bloom_hashes == 0) {
        return HelperError("SB-INDEX-TEXT-DOC-TOKEN-BLOOM-REFUSED",
                           "index.text_document.token_bloom_refused",
                           "bloom_bits_or_hashes");
      }
      decision.helper_family = IndexFamily::full_text;
      decision.helper_profile_id = "native_token_bloom";
      decision.effective_gram_width = 0;
      break;
  }
  return decision;
}

DocumentPathAccessDecision DecideDocumentPathAccess(const DocumentPathAccessRequest& request) {
  if (request.encrypted_or_policy_blocked) {
    return DocumentError("SB-INDEX-TEXT-DOC-DOCUMENT-POLICY-BLOCKED",
                         "index.text_document.document_policy_blocked",
                         "policy_block_encrypted_range",
                         true);
  }
  if (request.wildcard_requested && !request.wildcard_allowed) {
    return DocumentError("SB-INDEX-TEXT-DOC-WILDCARD-REFUSED",
                         "index.text_document.wildcard_refused",
                         "wildcard");
  }
  if (request.missing_policy == DocumentValuePolicy::refuse ||
      request.null_policy == DocumentValuePolicy::refuse ||
      request.array_policy == DocumentValuePolicy::refuse ||
      request.object_policy == DocumentValuePolicy::refuse) {
    return DocumentError("SB-INDEX-TEXT-DOC-VALUE-POLICY-REFUSED",
                         "index.text_document.value_policy_refused",
                         "missing/null/array/object");
  }
  if (!request.exact_source_recheck_available) {
    return DocumentError("SB-INDEX-TEXT-DOC-DOCUMENT-RECHECK-MISSING",
                         "index.text_document.document_recheck_missing",
                         "exact_source_recheck");
  }

  DocumentPathAccessDecision decision;
  decision.status = OkStatus();
  if (!NormalizeDocumentPath(request.raw_path, request.wildcard_requested, &decision.normalized_path)) {
    return DocumentError("SB-INDEX-TEXT-DOC-PATH-NORMALIZATION-REFUSED",
                         "index.text_document.path_normalization_refused",
                         request.raw_path);
  }
  decision.wildcard_scope = request.wildcard_requested;
  decision.index_missing_presence = request.missing_policy == DocumentValuePolicy::index_presence ||
                                    request.missing_policy == DocumentValuePolicy::index_value;
  decision.index_null_value = request.null_policy == DocumentValuePolicy::index_value ||
                              request.null_policy == DocumentValuePolicy::index_presence;
  decision.expand_array_elements = request.array_policy == DocumentValuePolicy::expand_elements;
  decision.index_array_presence = request.array_policy == DocumentValuePolicy::index_presence ||
                                  request.array_policy == DocumentValuePolicy::index_value;
  decision.index_object_presence = request.object_policy == DocumentValuePolicy::index_presence ||
                                   request.object_policy == DocumentValuePolicy::index_value;
  decision.exact_source_recheck = true;
  return decision;
}

const std::vector<TextDocumentSemanticProfileDescriptor>& BuiltinTextDocumentSemanticProfiles() {
  static const std::vector<TextDocumentSemanticProfileDescriptor> descriptors = {
      Profile(TextDocumentReference::native, TextDocumentReferenceIndexType::native_full_text,
              IndexFamily::full_text, "full_text", "native_full_text",
              TextDocumentCompatibilityLevel::exact,
              Controls(true, true, true, true, true, true, true, false, false, true),
              false, false, false, "native_catalog_projection", "refuse_unknown_native_text_option",
              "sys.metrics.index.full_text.profile.native", "index.text_document.native.full_text"),
      Profile(TextDocumentReference::native, TextDocumentReferenceIndexType::native_inverted,
              IndexFamily::inverted, "full_text", "native_inverted",
              TextDocumentCompatibilityLevel::exact,
              Controls(true, true, true, false, false, false, false, false, false, true),
              false, false, false, "native_catalog_projection", "refuse_unknown_native_inverted_option",
              "sys.metrics.index.full_text.profile.native_inverted", "index.text_document.native.inverted"),
      Profile(TextDocumentReference::native, TextDocumentReferenceIndexType::native_document_path,
              IndexFamily::document_path, "full_text", "native_document_path",
              TextDocumentCompatibilityLevel::compatible_with_recheck,
              Controls(false, false, false, false, false, false, false, true, true, true),
              true, true, false, "native_document_path_projection", "refuse_unknown_document_path_option",
              "sys.metrics.index.full_text.profile.native_document_path", "index.text_document.native.document_path"),
      Profile(TextDocumentReference::mongodb, TextDocumentReferenceIndexType::mongodb_text,
              IndexFamily::full_text, "full_text", "mongodb_text_profile",
              TextDocumentCompatibilityLevel::compatible_with_recheck,
              Controls(true, true, true, true, true, true, true, false, false, true),
              true, true, false, "listIndexes_projection", "policy_block_unknown_text_option",
              "sys.metrics.index.reference.mongodb.text", "index.reference.mongodb.text"),
      Profile(TextDocumentReference::mongodb, TextDocumentReferenceIndexType::mongodb_wildcard,
              IndexFamily::document_path, "full_text", "mongodb_wildcard_profile",
              TextDocumentCompatibilityLevel::compatible_with_recheck,
              Controls(false, false, true, false, false, false, false, true, true, true),
              true, true, false, "listIndexes_projection", "policy_block_encrypted_range",
              "sys.metrics.index.reference.mongodb.wildcard", "index.reference.mongodb.wildcard"),
      Profile(TextDocumentReference::opensearch, TextDocumentReferenceIndexType::opensearch_text,
              IndexFamily::full_text, "full_text", "opensearch_text_profile",
              TextDocumentCompatibilityLevel::compatible_with_recheck,
              Controls(true, true, true, true, true, true, true, false, false, true),
              true, true, false, "mapping_projection", "policy_block_unknown_analyzer",
              "sys.metrics.index.reference.opensearch.text", "index.reference.opensearch.text"),
      Profile(TextDocumentReference::neo4j, TextDocumentReferenceIndexType::neo4j_text,
              IndexFamily::full_text, "full_text", "neo4j_text_profile",
              TextDocumentCompatibilityLevel::compatible_with_recheck,
              Controls(true, true, true, false, false, false, false, false, false, true),
              true, true, false, "SHOW_INDEXES_projection", "policy_block_unknown_provider",
              "sys.metrics.index.reference.neo4j.text", "index.reference.neo4j.text"),
      Profile(TextDocumentReference::neo4j, TextDocumentReferenceIndexType::neo4j_fulltext,
              IndexFamily::full_text, "full_text", "neo4j_fulltext_profile",
              TextDocumentCompatibilityLevel::compatible_with_recheck,
              Controls(true, true, true, true, true, true, true, false, false, true),
              true, true, false, "SHOW_INDEXES_projection", "policy_block_unknown_provider",
              "sys.metrics.index.reference.neo4j.fulltext", "index.reference.neo4j.fulltext"),
      Profile(TextDocumentReference::postgresql, TextDocumentReferenceIndexType::postgresql_gin,
              IndexFamily::gin, "full_text", "postgresql_gin_profile",
              TextDocumentCompatibilityLevel::compatible_with_recheck,
              Controls(true, true, true, true, true, true, false, false, false, true),
              true, false, false, "pg_catalog_projection", "policy_block_unknown_opclass",
              "sys.metrics.index.reference.postgresql.gin", "index.reference.postgresql.gin"),
      Profile(TextDocumentReference::postgresql, TextDocumentReferenceIndexType::postgresql_tsvector,
              IndexFamily::full_text, "full_text", "postgresql_tsvector_profile",
              TextDocumentCompatibilityLevel::compatible_with_recheck,
              Controls(true, true, true, true, true, true, true, false, false, true),
              true, false, false, "pg_catalog_projection", "policy_block_unknown_text_config",
              "sys.metrics.index.reference.postgresql.tsvector", "index.reference.postgresql.tsvector"),
      Profile(TextDocumentReference::cassandra, TextDocumentReferenceIndexType::cassandra_sai_text,
              IndexFamily::full_text, "full_text", "cassandra_sai_text_profile",
              TextDocumentCompatibilityLevel::compatible_with_recheck,
              Controls(true, true, true, false, false, false, false, false, false, true),
              true, true, false, "system_schema_projection", "policy_block_unknown_sai_analyzer",
              "sys.metrics.index.reference.cassandra.sai_text", "index.reference.cassandra.sai_text"),
      Profile(TextDocumentReference::cassandra, TextDocumentReferenceIndexType::cassandra_sasi_text,
              IndexFamily::full_text, "full_text", "cassandra_sasi_text_profile",
              TextDocumentCompatibilityLevel::compatible_with_recheck,
              Controls(true, true, true, false, false, false, false, false, false, true),
              true, true, false, "system_schema_projection", "policy_block_unknown_sasi_mode",
              "sys.metrics.index.reference.cassandra.sasi_text", "index.reference.cassandra.sasi_text"),
      Profile(TextDocumentReference::mysql, TextDocumentReferenceIndexType::mysql_fulltext,
              IndexFamily::full_text, "full_text", "mysql_fulltext_profile",
              TextDocumentCompatibilityLevel::compatible_with_recheck,
              Controls(true, true, true, true, true, true, true, false, false, true),
              true, true, false, "information_schema_projection", "policy_block_engine_only",
              "sys.metrics.index.reference.mysql.fulltext", "index.reference.mysql.fulltext"),
      Profile(TextDocumentReference::mariadb, TextDocumentReferenceIndexType::mariadb_fulltext,
              IndexFamily::full_text, "full_text", "mariadb_fulltext_profile",
              TextDocumentCompatibilityLevel::compatible_with_recheck,
              Controls(true, true, true, true, true, true, true, false, false, true),
              true, true, false, "information_schema_projection", "policy_block_engine_only",
              "sys.metrics.index.reference.mariadb.fulltext", "index.reference.mariadb.fulltext")};
  return descriptors;
}

TextDocumentProfileLookupResult FindTextDocumentSemanticProfile(TextDocumentReference reference,
                                                                TextDocumentReferenceIndexType index_type) {
  for (const auto& descriptor : BuiltinTextDocumentSemanticProfiles()) {
    if (descriptor.reference == reference && descriptor.index_type == index_type) {
      return TextDocumentProfileLookupResult{OkStatus(), &descriptor, {}};
    }
  }
  return TextDocumentProfileLookupResult{
      ErrorStatus(),
      nullptr,
      MakeTextDocumentAccessDiagnostic(ErrorStatus(), "SB-INDEX-TEXT-DOC-PROFILE-MISSING",
                                       "index.text_document.profile_missing",
                                       std::string(TextDocumentReferenceName(reference)) + ":" +
                                           TextDocumentReferenceIndexTypeName(index_type))};
}

TextDocumentProfileLookupResult FindTextDocumentSemanticProfileById(std::string_view semantic_profile_id) {
  for (const auto& descriptor : BuiltinTextDocumentSemanticProfiles()) {
    if (descriptor.semantic_profile_id == semantic_profile_id) {
      return TextDocumentProfileLookupResult{OkStatus(), &descriptor, {}};
    }
  }
  return TextDocumentProfileLookupResult{
      ErrorStatus(),
      nullptr,
      MakeTextDocumentAccessDiagnostic(ErrorStatus(), "SB-INDEX-TEXT-DOC-PROFILE-ID-MISSING",
                                       "index.text_document.profile_id_missing",
                                       std::string(semantic_profile_id))};
}

TextDocumentProfileDecision DecideReferenceTextDocumentProfile(const TextDocumentProfileDecisionRequest& request) {
  const auto lookup = FindTextDocumentSemanticProfile(request.reference, request.index_type);
  if (!lookup.ok()) {
    return ProfileError(nullptr,
                        "SB-INDEX-TEXT-DOC-PROFILE-MISSING",
                        "index.text_document.profile_missing",
                        std::string(TextDocumentReferenceName(request.reference)) + ":" +
                            TextDocumentReferenceIndexTypeName(request.index_type));
  }

  const auto* descriptor = lookup.descriptor;
  if (!request.profile_policy_allowed ||
      descriptor->compatibility == TextDocumentCompatibilityLevel::policy_blocked ||
      descriptor->compatibility == TextDocumentCompatibilityLevel::rejected) {
    return ProfileError(descriptor,
                        "SB-INDEX-TEXT-DOC-PROFILE-POLICY-BLOCKED",
                        descriptor->diagnostics_key,
                        descriptor->unsupported_behavior,
                        true);
  }
  if (!request.resource_epochs_current) {
    return ProfileError(descriptor,
                        "SB-INDEX-TEXT-DOC-PROFILE-EPOCH-STALE",
                        "index.text_document.profile_epoch_stale",
                        descriptor->semantic_profile_id);
  }
  if (descriptor->requires_recheck && !request.exact_recheck_available) {
    return ProfileError(descriptor,
                        "SB-INDEX-TEXT-DOC-PROFILE-RECHECK-MISSING",
                        "index.text_document.profile_recheck_missing",
                        descriptor->semantic_profile_id);
  }
  const bool needs_fallback_sort = descriptor->fallback_sort_required ||
                                   (descriptor->fallback_sort_possible && request.fallback_sort_requested);
  if (needs_fallback_sort && !request.fallback_sort_available) {
    return ProfileError(descriptor,
                        "SB-INDEX-TEXT-DOC-PROFILE-FALLBACK-SORT-MISSING",
                        "index.text_document.profile_fallback_sort_missing",
                        descriptor->semantic_profile_id);
  }

  TextDocumentProfileDecision decision;
  decision.status = OkStatus();
  decision.descriptor = descriptor;
  decision.native_family = descriptor->native_family;
  decision.semantic_profile_id = descriptor->semantic_profile_id;
  decision.active_controls = descriptor->controls;
  decision.requires_exact_recheck = descriptor->requires_recheck;
  decision.fallback_sort = needs_fallback_sort;
  decision.catalog_projection = request.catalog_projection_requested && descriptor->controls.catalog_projection;
  decision.metrics.count_profile_hit = true;
  decision.metrics.count_recheck = decision.requires_exact_recheck;
  decision.metrics.count_fallback_sort = decision.fallback_sort;
  decision.metrics.count_catalog_projection = decision.catalog_projection;
  decision.metrics.metrics_prefix = descriptor->metrics_prefix;
  return decision;
}

DiagnosticRecord MakeTextDocumentAccessDiagnostic(Status status,
                                                  std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail) {
  DiagnosticRecord record;
  record.status = status;
  record.diagnostic_code = std::move(diagnostic_code);
  record.message_key = std::move(message_key);
  if (!detail.empty()) record.arguments.push_back({"detail", std::move(detail)});
  record.source_component = "sb_core_index.text_document_access";
  return record;
}

}  // namespace scratchbird::core::index
