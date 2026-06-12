// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB_NGRAM_PHYSICAL_PROVIDER
#include "text_inverted_segment.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kNgramPhysicalProviderSearchKey =
    "SB_NGRAM_PHYSICAL_PROVIDER";
inline constexpr const char* kNgramPhysicalProviderArtifactKind =
    "ngram_physical_provider";
inline constexpr u32 kNgramPhysicalProviderCurrentMajor = 1;
inline constexpr u32 kNgramPhysicalProviderCurrentMinor = 0;
inline constexpr u32 kNgramPhysicalProviderMinGramWidth = 1;
inline constexpr u32 kNgramPhysicalProviderMaxGramWidth = 8;

enum class NgramPhysicalOpenClass : u32 {
  current = 1,
  stale_format = 2,
  stale_generation = 3,
  bad_checksum = 4,
  corrupt_payload = 5,
  identity_mismatch = 6,
  stale_epoch = 7,
  unsafe_tokenizer_or_charset = 8,
  invalid_gram_width = 9,
  missing_exact_recheck = 10,
  authority_claim_refused = 11,
  refused = 12
};

enum class NgramQueryKind : u32 {
  prefix = 1,
  suffix = 2,
  contains = 3
};

enum class NgramMutationKind : u32 {
  insert_row = 1,
  delete_row = 2,
  update_row = 3
};

struct NgramExactRecheckProof {
  bool proof_supplied = false;
  bool exact_source_batch_available = false;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  std::string evidence_ref;
  bool parser_finality_authority_claimed = false;
  bool reference_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool index_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
  bool visibility_authority_claimed = false;
  bool security_authority_claimed = false;
  bool transaction_finality_authority_claimed = false;
};

struct NgramTokenizerDescriptor {
  u64 tokenizer_epoch = 0;
  u64 charset_epoch = 0;
  u64 resource_epoch = 0;
  bool deterministic = false;
  bool tokenizer_safe = false;
  bool charset_safe = false;
  bool unicode_boundary_safe = false;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  bool parser_finality_authority_claimed = false;
  bool reference_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool index_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
};

struct NgramSourceRow {
  TextInvertedRowLocator locator;
  std::string exact_source_value;
};

struct NgramPostingEntry {
  std::string gram;
  std::vector<TextInvertedRowLocator> postings;
};

struct NgramPhysicalProvider {
  std::string artifact_kind = kNgramPhysicalProviderArtifactKind;
  PageExtentSummaryFormatVersion format_version{
      kNgramPhysicalProviderCurrentMajor, kNgramPhysicalProviderCurrentMinor};
  std::string relation_uuid;
  std::string index_uuid;
  std::string provider_uuid;
  u64 base_generation = 0;
  u64 provider_generation = 0;
  NgramTokenizerDescriptor tokenizer;
  u32 gram_width = 3;
  bool qgram_extractor_present = true;
  bool qgram_postings_present = true;
  bool prefix_acceleration_present = true;
  bool suffix_acceleration_present = true;
  bool contains_acceleration_present = true;
  bool false_positive_tracking_present = true;
  bool exact_source_recheck_batching_present = true;
  bool candidate_evidence_only = true;
  bool exact_source_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  bool visibility_authority_claimed = false;
  bool security_authority_claimed = false;
  bool transaction_finality_authority_claimed = false;
  bool parser_finality_authority_claimed = false;
  bool reference_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool index_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
  u64 exact_recheck_batch_count = 0;
  u64 false_positive_count = 0;
  std::vector<NgramSourceRow> source_rows;
  std::vector<NgramPostingEntry> postings;
  std::vector<std::string> evidence;
};

struct NgramPhysicalBuildRequest {
  std::string relation_uuid;
  std::string index_uuid;
  std::string provider_uuid;
  u64 base_generation = 0;
  u64 provider_generation = 0;
  NgramTokenizerDescriptor tokenizer;
  u32 gram_width = 3;
  NgramExactRecheckProof recheck_proof;
  std::vector<NgramSourceRow> rows;
};

struct NgramPhysicalBuildResult {
  Status status;
  DiagnosticRecord diagnostic;
  NgramPhysicalProvider provider;
  bool built = false;
  bool fail_closed = true;

  bool ok() const { return status.ok() && built && !fail_closed; }
};

struct NgramPhysicalSerializeResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<byte> bytes;
  u64 checksum = 0;

  bool ok() const { return status.ok() && !bytes.empty(); }
};

struct NgramPhysicalOpenRequest {
  std::vector<byte> bytes;
  bool expected_relation_uuid_present = false;
  std::string expected_relation_uuid;
  bool expected_index_uuid_present = false;
  std::string expected_index_uuid;
  bool expected_provider_uuid_present = false;
  std::string expected_provider_uuid;
  bool expected_base_generation_present = false;
  u64 expected_base_generation = 0;
  bool expected_provider_generation_present = false;
  u64 expected_provider_generation = 0;
  bool expected_tokenizer_epoch_present = false;
  u64 expected_tokenizer_epoch = 0;
  bool expected_charset_epoch_present = false;
  u64 expected_charset_epoch = 0;
  bool expected_resource_epoch_present = false;
  u64 expected_resource_epoch = 0;
  NgramExactRecheckProof recheck_proof;
};

struct NgramPhysicalOpenResult {
  Status status;
  DiagnosticRecord diagnostic;
  NgramPhysicalOpenClass open_class = NgramPhysicalOpenClass::refused;
  NgramPhysicalProvider provider;
  bool fail_closed = true;
  bool restricted_repair_required = false;
  std::vector<std::string> actions;

  bool ok() const {
    return status.ok() && open_class == NgramPhysicalOpenClass::current &&
           !fail_closed;
  }
};

struct NgramQueryRequest {
  NgramPhysicalProvider provider;
  NgramQueryKind kind = NgramQueryKind::contains;
  std::string pattern;
  NgramExactRecheckProof recheck_proof;
  bool tokenizer_epoch_current = true;
  bool charset_epoch_current = true;
  bool resource_epoch_current = true;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
};

struct NgramCandidate {
  TextInvertedRowLocator locator;
  bool accelerated_candidate = false;
  bool exact_match = false;
  bool false_positive = false;
  bool exact_source_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool final_row_admitted = false;
  std::string source_recheck_evidence_ref;
};

struct NgramQueryResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = true;
  std::vector<NgramCandidate> candidates;
  u64 qgram_probe_count = 0;
  u64 exact_recheck_batch_count = 0;
  u64 false_positive_count = 0;
  bool prefix_acceleration_used = false;
  bool suffix_acceleration_used = false;
  bool contains_acceleration_used = false;
  bool candidate_rows_only = true;
  bool final_rows_authorized = false;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  bool exact_source_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct NgramPhysicalMutation {
  NgramMutationKind kind = NgramMutationKind::insert_row;
  bool before_row_present = false;
  NgramSourceRow before_row;
  bool after_row_present = false;
  NgramSourceRow after_row;
  NgramExactRecheckProof recheck_proof;
};

struct NgramPhysicalMutationResult {
  Status status;
  DiagnosticRecord diagnostic;
  NgramPhysicalProvider provider;
  bool applied = false;
  bool fail_closed = true;
  std::vector<std::string> actions;

  bool ok() const { return status.ok() && applied && !fail_closed; }
};

NgramPhysicalBuildResult BuildNgramPhysicalProvider(
    const NgramPhysicalBuildRequest& request);
NgramPhysicalSerializeResult SerializeNgramPhysicalProvider(
    const NgramPhysicalProvider& provider);
NgramPhysicalOpenResult OpenNgramPhysicalProvider(
    const NgramPhysicalOpenRequest& request);
NgramQueryResult QueryNgramPhysicalProvider(const NgramQueryRequest& request);
NgramPhysicalMutationResult ApplyNgramPhysicalMutation(
    const NgramPhysicalProvider& provider,
    const NgramPhysicalMutation& mutation);
std::vector<std::string> ExtractNgramsForProvider(std::string value,
                                                  u32 gram_width);

const char* NgramPhysicalOpenClassName(NgramPhysicalOpenClass open_class);
DiagnosticRecord MakeNgramPhysicalProviderDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
