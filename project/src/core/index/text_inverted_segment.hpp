// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB_TEXT_INVERTED_SEGMENT_ENGINE
#include "page_extent_summary.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr const char* kTextInvertedSegmentFormatSearchKey =
    "SB_TEXT_INVERTED_SEGMENT_ENGINE";
inline constexpr const char* kTextInvertedSegmentArtifactKind =
    "text_inverted_segment";
inline constexpr u32 kTextInvertedSegmentCurrentMajor = 1;
inline constexpr u32 kTextInvertedSegmentCurrentMinor = 0;
inline constexpr u32 kTextInvertedSegmentDefaultBlockPostings = 4;

enum class TextInvertedSegmentState : u32 {
  mutable_buffer = 1,
  sealed = 2,
  retired = 3,
  refused = 4
};

enum class TextInvertedSegmentOpenClass : u32 {
  current = 1,
  stale_format = 2,
  bad_checksum = 3,
  corrupt_payload = 4,
  identity_mismatch = 5,
  stale_generation = 6,
  unsafe_analyzer_or_resource_epoch = 7,
  missing_exact_recheck_proof = 8,
  authority_claim_refused = 9,
  refused = 10
};

enum class TextInvertedQueryKind : u32 {
  term = 1,
  all_terms = 2,
  phrase = 3
};

struct TextInvertedRowLocator {
  u64 row_ordinal = 0;
  std::string row_uuid;
  std::string version_uuid;
};

struct TextInvertedExactRecheckProof {
  bool proof_supplied = false;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool source_recheck_required = true;
  std::string evidence_ref;
};

struct TextInvertedDocumentInput {
  TextInvertedRowLocator locator;
  std::vector<std::string> normalized_terms;
  u32 document_length = 0;
  double norm = 0.0;
  std::string exact_source_recheck_evidence_ref;
  bool parser_finality_authority_claimed = false;
  bool donor_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
  bool visibility_authority_claimed = false;
  bool security_authority_claimed = false;
  bool transaction_finality_authority_claimed = false;
};

struct TextInvertedDocumentMetadata {
  TextInvertedRowLocator locator;
  u32 document_length = 0;
  double norm = 0.0;
  std::string exact_source_recheck_evidence_ref;
};

struct TextInvertedPosting {
  TextInvertedRowLocator locator;
  u32 document_length = 0;
  double norm = 0.0;
  std::vector<u32> positions;
};

struct TextInvertedPostingBlock {
  u32 block_ordinal = 0;
  std::string term;
  u64 first_row_ordinal = 0;
  u64 last_row_ordinal = 0;
  u32 posting_count = 0;
  u64 encoded_byte_count = 0;
  bool row_ordinals_delta_coded = true;
  bool positions_delta_coded = true;
  std::vector<byte> encoded_postings;
  std::vector<TextInvertedPosting> postings;
};

struct TextInvertedTermDictionaryEntry {
  std::string term;
  u32 first_block_ordinal = 0;
  u32 block_count = 0;
  u64 document_frequency = 0;
  u64 total_position_count = 0;
};

struct TextInvertedMergeScheduleMetadata {
  u32 merge_tier = 0;
  u64 sealed_sequence = 0;
  u64 artifact_byte_count = 0;
  bool sealed = false;
  bool immutable = false;
  bool merge_in_progress = false;
  bool retired = false;
  bool merge_metadata_finality_authority = false;
};

struct TextInvertedSegment {
  std::string artifact_kind = kTextInvertedSegmentArtifactKind;
  PageExtentSummaryFormatVersion format_version{
      kTextInvertedSegmentCurrentMajor, kTextInvertedSegmentCurrentMinor};
  std::string relation_uuid;
  std::string index_uuid;
  std::string segment_uuid;
  u64 base_generation = 0;
  u64 segment_generation = 0;
  u64 analyzer_epoch = 0;
  u64 resource_epoch = 0;
  u32 block_posting_target = kTextInvertedSegmentDefaultBlockPostings;
  TextInvertedSegmentState state = TextInvertedSegmentState::mutable_buffer;
  bool term_dictionary_present = false;
  bool positions_present = false;
  bool document_length_norms_present = false;
  bool skip_metadata_present = false;
  bool mutable_buffer_sealed = false;
  bool candidate_evidence_only = true;
  bool exact_source_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool visibility_authority_claimed = false;
  bool security_authority_claimed = false;
  bool transaction_finality_authority_claimed = false;
  bool parser_finality_authority_claimed = false;
  bool donor_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
  std::vector<TextInvertedDocumentMetadata> documents;
  std::vector<TextInvertedTermDictionaryEntry> dictionary;
  std::vector<TextInvertedPostingBlock> posting_blocks;
  TextInvertedMergeScheduleMetadata merge;
  std::vector<std::string> evidence;
};

struct TextInvertedSegmentBuildRequest {
  std::string relation_uuid;
  std::string index_uuid;
  std::string segment_uuid;
  u64 base_generation = 0;
  u64 segment_generation = 0;
  u64 analyzer_epoch = 0;
  u64 resource_epoch = 0;
  u32 block_posting_target = kTextInvertedSegmentDefaultBlockPostings;
  u32 merge_tier = 0;
  u64 sealed_sequence = 0;
  TextInvertedExactRecheckProof recheck_proof;
  bool parser_finality_authority_claimed = false;
  bool donor_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
  bool visibility_authority_claimed = false;
  bool security_authority_claimed = false;
  bool transaction_finality_authority_claimed = false;
};

struct TextInvertedMutableSegmentBuffer {
  TextInvertedSegmentBuildRequest request;
  std::vector<TextInvertedDocumentInput> documents;
  bool sealed = false;
};

struct TextInvertedSegmentMutationResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool accepted = false;
  bool fail_closed = true;

  bool ok() const { return status.ok() && accepted && !fail_closed; }
};

struct TextInvertedSegmentSealResult {
  Status status;
  DiagnosticRecord diagnostic;
  TextInvertedSegment segment;
  bool sealed = false;
  bool fail_closed = true;

  bool ok() const { return status.ok() && sealed && !fail_closed; }
};

struct TextInvertedSegmentSerializeResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<byte> bytes;
  u64 checksum = 0;

  bool ok() const { return status.ok() && !bytes.empty(); }
};

struct TextInvertedSegmentOpenRequest {
  std::vector<byte> bytes;
  bool expected_relation_uuid_present = false;
  std::string expected_relation_uuid;
  bool expected_index_uuid_present = false;
  std::string expected_index_uuid;
  bool expected_segment_uuid_present = false;
  std::string expected_segment_uuid;
  bool expected_base_generation_present = false;
  u64 expected_base_generation = 0;
  bool expected_segment_generation_present = false;
  u64 expected_segment_generation = 0;
  bool expected_analyzer_epoch_present = false;
  u64 expected_analyzer_epoch = 0;
  bool expected_resource_epoch_present = false;
  u64 expected_resource_epoch = 0;
  TextInvertedExactRecheckProof recheck_proof;
};

struct TextInvertedSegmentOpenResult {
  Status status;
  DiagnosticRecord diagnostic;
  TextInvertedSegmentOpenClass open_class =
      TextInvertedSegmentOpenClass::refused;
  TextInvertedSegment segment;
  bool fail_closed = true;
  bool restricted_repair_required = false;
  std::vector<std::string> actions;

  bool ok() const {
    return status.ok() && open_class == TextInvertedSegmentOpenClass::current &&
           !fail_closed;
  }
};

struct TextInvertedQueryRequest {
  TextInvertedSegment segment;
  TextInvertedQueryKind kind = TextInvertedQueryKind::term;
  std::vector<std::string> terms;
  TextInvertedExactRecheckProof recheck_proof;
  bool analyzer_epoch_current = true;
  bool resource_epoch_current = true;
  u64 lower_bound_row_ordinal = 0;
};

struct TextInvertedQueryResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<TextInvertedRowLocator> candidates;
  bool exact_source_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  u64 skipped_block_count = 0;
  u64 scanned_block_count = 0;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok(); }
};

struct TextInvertedMergeCandidate {
  std::string relation_uuid;
  std::string index_uuid;
  std::string segment_uuid;
  u64 segment_generation = 0;
  u64 analyzer_epoch = 0;
  u64 resource_epoch = 0;
  u32 merge_tier = 0;
  u64 sealed_sequence = 0;
  u64 document_count = 0;
  u64 term_count = 0;
  u64 posting_block_count = 0;
  u64 artifact_byte_count = 0;
  bool sealed = false;
  bool immutable = false;
  bool merge_in_progress = false;
  bool retired = false;
  bool eligible = false;
  bool merge_metadata_finality_authority = false;
  bool parser_finality_authority_claimed = false;
  bool donor_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
};

struct TextInvertedMergeScheduleRequest {
  std::vector<TextInvertedMergeCandidate> candidates;
  u32 minimum_segment_count = 2;
  u64 max_total_artifact_bytes = 0;
  TextInvertedExactRecheckProof recheck_proof;
  bool scheduler_claims_finality_authority = false;
};

struct TextInvertedMergeScheduleResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool scheduled = false;
  bool fail_closed = true;
  bool merge_metadata_finality_authority = false;
  bool old_segments_remain_searchable_until_engine_publish = true;
  std::vector<TextInvertedMergeCandidate> selected;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && scheduled && !fail_closed; }
};

const char* TextInvertedSegmentStateName(TextInvertedSegmentState state);
const char* TextInvertedSegmentOpenClassName(
    TextInvertedSegmentOpenClass open_class);
const char* TextInvertedQueryKindName(TextInvertedQueryKind kind);

TextInvertedMutableSegmentBuffer CreateTextInvertedMutableSegmentBuffer(
    const TextInvertedSegmentBuildRequest& request);
TextInvertedSegmentMutationResult AddTextInvertedDocument(
    TextInvertedMutableSegmentBuffer* buffer,
    const TextInvertedDocumentInput& document);
TextInvertedSegmentSealResult SealTextInvertedSegmentBuffer(
    TextInvertedMutableSegmentBuffer* buffer);
TextInvertedSegmentSealResult BuildTextInvertedSegmentFromDocuments(
    const TextInvertedSegmentBuildRequest& request,
    const std::vector<TextInvertedDocumentInput>& documents);

TextInvertedSegmentSerializeResult SerializeTextInvertedSegmentArtifact(
    const TextInvertedSegment& segment);
TextInvertedSegmentOpenResult OpenTextInvertedSegmentArtifact(
    const TextInvertedSegmentOpenRequest& request);
TextInvertedQueryResult ProbeTextInvertedSegment(
    const TextInvertedQueryRequest& request);
TextInvertedMergeCandidate TextInvertedMergeCandidateFromSegment(
    const TextInvertedSegment& segment);
TextInvertedMergeScheduleResult SelectTextInvertedSegmentsForMerge(
    const TextInvertedMergeScheduleRequest& request);

DiagnosticRecord MakeTextInvertedSegmentDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail = {});

}  // namespace scratchbird::core::index
