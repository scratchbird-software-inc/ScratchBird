// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-TEXT-DOCUMENT-ACCESS-CLOSURE-ANCHOR
#include "index_resource_boundary.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::u64;

enum class TextDictionaryMode : u32 {
  term_dictionary,
  multikey_dictionary,
  ngram_dictionary,
  document_path_dictionary
};

enum class TextPostingPayload : u32 {
  locator_only,
  positions,
  phrase_positions,
  rank_payload,
  document_path_payload,
  gin_payload
};

enum class TextAccessDecisionAction : u32 {
  use_existing,
  build_or_rebuild,
  mark_stale,
  refuse
};

struct TextDictionaryPostingRequest {
  IndexFamily family = IndexFamily::unknown;
  bool dictionary_present = false;
  bool posting_segments_present = false;
  bool resource_epoch_current = false;
  bool positions_required = false;
  bool phrase_required = false;
  bool ranking_required = false;
  bool document_path_required = false;
  bool multikey_required = false;
  bool allow_lossy_candidates = false;
  bool segment_merge_required = false;
};

struct TextDictionaryPostingDecision {
  Status status;
  TextAccessDecisionAction action = TextAccessDecisionAction::refuse;
  TextDictionaryMode dictionary_mode = TextDictionaryMode::term_dictionary;
  TextPostingPayload posting_payload = TextPostingPayload::locator_only;
  bool persist_dictionary = false;
  bool persist_positions = false;
  bool persist_rank_payload = false;
  bool requires_exact_recheck = false;
  bool requires_segment_merge = false;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct TextAnalyzerResourceRequest {
  bool tokenizer_registered = false;
  bool tokenizer_epoch_current = false;
  bool analyzer_registered = false;
  bool analyzer_epoch_current = false;
  bool charset_registered = true;
  bool charset_epoch_current = true;
  bool stemmer_required = false;
  bool stemmer_registered = true;
  bool stemmer_epoch_current = true;
  bool stopword_resource_required = false;
  bool stopword_resource_registered = true;
  bool stopword_epoch_current = true;
  bool empty_stopword_policy_allowed = false;
  bool deterministic = false;
  bool mark_stale_on_epoch_mismatch = true;
};

struct TextAnalyzerResourceDecision {
  Status status;
  IndexResourceAction action = IndexResourceAction::refuse;
  bool tokenizer_usable = false;
  bool analyzer_usable = false;
  bool charset_usable = false;
  bool stemmer_usable = false;
  bool stopword_resource_usable = false;
  bool empty_stopword_policy_used = false;
  bool mark_index_stale = false;
  bool rebuild_required = false;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

enum class TextQueryOperator : u32 {
  term,
  all_terms,
  phrase,
  rank,
  rerank,
  exact_source_recheck
};

enum class TextQueryPath : u32 {
  term_posting_scan,
  all_terms_intersection,
  phrase_position_scan,
  rank_payload_scan,
  rerank_source_scan,
  exact_source_scan,
  refused
};

struct TextQueryPathRequest {
  TextQueryOperator operation = TextQueryOperator::term;
  u32 term_count = 1;
  bool analyzer_epoch_current = true;
  bool postings_available = true;
  bool positions_available = false;
  bool rank_payload_available = false;
  bool exact_source_available = true;
  bool phrase_profile_enabled = true;
  bool ranking_profile_enabled = true;
  bool lossy_candidates = false;
  bool donor_requires_recheck = false;
};

struct TextQueryPathDecision {
  Status status;
  TextQueryPath path = TextQueryPath::refused;
  bool use_term_postings = false;
  bool intersect_all_terms = false;
  bool verify_phrase_boundary = false;
  bool use_rank_payload = false;
  bool rerank_required = false;
  bool exact_source_recheck = false;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct GinConsistencyRequest {
  bool opclass_known = false;
  bool extractor_registered = false;
  bool consistency_registered = false;
  bool deterministic = false;
  u32 extracted_key_count = 0;
  bool partial_match = false;
  bool lossy_candidates = true;
  bool consistency_may_be_lossy = true;
  bool tri_consistent_available = false;
  bool exact_recheck_available = false;
  bool donor_profile_allows = false;
};

struct GinConsistencyDecision {
  Status status;
  bool extract_multikeys = false;
  bool use_tri_consistent = false;
  bool run_consistency_function = false;
  bool consistency_requires_recheck = false;
  bool exact_source_recheck = false;
  bool policy_blocked = false;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct SparseWandTermBound {
  u32 term_ordinal = 0;
  u64 document_frequency = 0;
  double upper_bound_score = 0.0;
  bool required = false;
};

struct SparseWandPruningRequest {
  bool enabled = false;
  std::vector<SparseWandTermBound> terms;
  u32 requested_top_k = 0;
  u32 minimum_should_match = 1;
  double score_cutoff = 0.0;
  bool scores_monotonic = false;
  bool upper_bounds_current = false;
  bool exact_recheck_available = false;
};

struct SparseWandPruningDecision {
  Status status;
  bool use_wand = false;
  bool candidate_pruning = false;
  bool prune_all_candidates = false;
  u32 required_term_count = 0;
  u32 effective_minimum_should_match = 0;
  double aggregate_upper_bound_score = 0.0;
  bool exact_source_recheck = false;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

enum class TextHelperProfileKind : u32 {
  ngram,
  trigram,
  token_bloom
};

struct TextHelperProfileRequest {
  TextHelperProfileKind kind = TextHelperProfileKind::ngram;
  u32 gram_width = 0;
  u32 bloom_bits = 0;
  u32 bloom_hashes = 0;
  bool charset_epoch_current = false;
  bool tokenizer_epoch_current = false;
  bool deterministic = false;
  bool exact_recheck_available = false;
};

struct TextHelperProfileDecision {
  Status status;
  IndexFamily helper_family = IndexFamily::unknown;
  std::string helper_profile_id;
  u32 effective_gram_width = 0;
  bool accepted = false;
  bool false_positive_possible = false;
  bool requires_exact_recheck = false;
  bool metric_visible = false;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

enum class DocumentValuePolicy : u32 {
  skip,
  index_presence,
  index_value,
  expand_elements,
  refuse
};

struct DocumentPathAccessRequest {
  std::string raw_path;
  bool wildcard_requested = false;
  bool wildcard_allowed = false;
  bool exact_source_recheck_available = false;
  bool encrypted_or_policy_blocked = false;
  DocumentValuePolicy missing_policy = DocumentValuePolicy::skip;
  DocumentValuePolicy null_policy = DocumentValuePolicy::index_value;
  DocumentValuePolicy array_policy = DocumentValuePolicy::expand_elements;
  DocumentValuePolicy object_policy = DocumentValuePolicy::index_presence;
};

struct DocumentPathAccessDecision {
  Status status;
  std::string normalized_path;
  bool wildcard_scope = false;
  bool index_missing_presence = false;
  bool index_null_value = false;
  bool expand_array_elements = false;
  bool index_array_presence = false;
  bool index_object_presence = false;
  bool exact_source_recheck = false;
  bool policy_blocked = false;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

enum class TextDocumentDonor : u32 {
  native,
  mongodb,
  opensearch,
  neo4j,
  postgresql,
  cassandra,
  mysql,
  mariadb,
  unknown
};

enum class TextDocumentDonorIndexType : u32 {
  native_full_text,
  native_inverted,
  native_document_path,
  mongodb_text,
  mongodb_wildcard,
  opensearch_text,
  neo4j_text,
  neo4j_fulltext,
  postgresql_gin,
  postgresql_tsvector,
  cassandra_sai_text,
  cassandra_sasi_text,
  mysql_fulltext,
  mariadb_fulltext,
  unknown
};

enum class TextDocumentCompatibilityLevel : u32 {
  exact,
  compatible_with_recheck,
  compatible_with_fallback_sort,
  catalog_only_projection,
  policy_blocked,
  rejected
};

struct TextDocumentProfileControls {
  bool analyzer = false;
  bool tokenizer = false;
  bool case_behavior = false;
  bool stemming = false;
  bool stopwords = false;
  bool phrase = false;
  bool ranking = false;
  bool wildcard = false;
  bool document_path = false;
  bool catalog_projection = false;
};

struct TextDocumentSemanticProfileDescriptor {
  TextDocumentDonor donor = TextDocumentDonor::unknown;
  TextDocumentDonorIndexType index_type = TextDocumentDonorIndexType::unknown;
  IndexFamily native_family = IndexFamily::unknown;
  std::string native_physical_family;
  std::string semantic_profile_id;
  TextDocumentCompatibilityLevel compatibility = TextDocumentCompatibilityLevel::rejected;
  TextDocumentProfileControls controls;
  bool requires_recheck = true;
  bool fallback_sort_possible = false;
  bool fallback_sort_required = false;
  std::string catalog_projection;
  std::string unsupported_behavior;
  std::string metrics_prefix;
  std::string diagnostics_key;
};

struct TextDocumentProfileLookupResult {
  Status status;
  const TextDocumentSemanticProfileDescriptor* descriptor = nullptr;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && descriptor != nullptr; }
};

struct TextDocumentProfileDecisionRequest {
  TextDocumentDonor donor = TextDocumentDonor::unknown;
  TextDocumentDonorIndexType index_type = TextDocumentDonorIndexType::unknown;
  bool profile_policy_allowed = true;
  bool resource_epochs_current = true;
  bool exact_recheck_available = true;
  bool fallback_sort_available = true;
  bool fallback_sort_requested = false;
  bool catalog_projection_requested = false;
};

struct TextDocumentProfileMetricsPlan {
  bool count_profile_hit = false;
  bool count_profile_refusal = false;
  bool count_recheck = false;
  bool count_fallback_sort = false;
  bool count_catalog_projection = false;
  std::string metrics_prefix;
};

struct TextDocumentProfileDecision {
  Status status;
  const TextDocumentSemanticProfileDescriptor* descriptor = nullptr;
  IndexFamily native_family = IndexFamily::unknown;
  std::string semantic_profile_id;
  TextDocumentProfileControls active_controls;
  bool requires_exact_recheck = false;
  bool fallback_sort = false;
  bool catalog_projection = false;
  bool policy_blocked = false;
  TextDocumentProfileMetricsPlan metrics;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && descriptor != nullptr; }
};

const char* TextDictionaryModeName(TextDictionaryMode mode);
const char* TextPostingPayloadName(TextPostingPayload payload);
const char* TextAccessDecisionActionName(TextAccessDecisionAction action);
const char* TextQueryOperatorName(TextQueryOperator op);
const char* TextQueryPathName(TextQueryPath path);
const char* TextHelperProfileKindName(TextHelperProfileKind kind);
const char* DocumentValuePolicyName(DocumentValuePolicy policy);
const char* TextDocumentDonorName(TextDocumentDonor donor);
const char* TextDocumentDonorIndexTypeName(TextDocumentDonorIndexType type);
const char* TextDocumentCompatibilityLevelName(TextDocumentCompatibilityLevel level);

bool IsTextDocumentIndexFamily(IndexFamily family);
TextDictionaryPostingDecision DecideTextDictionaryPostings(const TextDictionaryPostingRequest& request);
TextAnalyzerResourceDecision DecideTextAnalyzerResources(const TextAnalyzerResourceRequest& request);
TextQueryPathDecision DecideTextQueryPath(const TextQueryPathRequest& request);
GinConsistencyDecision DecideGinExtractionConsistency(const GinConsistencyRequest& request);
SparseWandPruningDecision DecideSparseWandPruning(const SparseWandPruningRequest& request);
TextHelperProfileDecision DecideTextHelperProfile(const TextHelperProfileRequest& request);
DocumentPathAccessDecision DecideDocumentPathAccess(const DocumentPathAccessRequest& request);

const std::vector<TextDocumentSemanticProfileDescriptor>& BuiltinTextDocumentSemanticProfiles();
TextDocumentProfileLookupResult FindTextDocumentSemanticProfile(TextDocumentDonor donor,
                                                                TextDocumentDonorIndexType index_type);
TextDocumentProfileLookupResult FindTextDocumentSemanticProfileById(std::string_view semantic_profile_id);
TextDocumentProfileDecision DecideDonorTextDocumentProfile(const TextDocumentProfileDecisionRequest& request);

DiagnosticRecord MakeTextDocumentAccessDiagnostic(Status status,
                                                  std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail = {});

}  // namespace scratchbird::core::index
