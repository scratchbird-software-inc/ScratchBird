// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "text_inverted_segment.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::Subsystem;

inline constexpr std::array<byte, 8> kMagic = {'S', 'B', 'T', 'I', 'N', 'V', '0', '1'};
inline constexpr u32 kHeaderBytes = 24;
inline constexpr u64 kFnvOffset = 14695981039346656037ull;
inline constexpr u64 kFnvPrime = 1099511628211ull;
inline constexpr u32 kMaxBlockPostingTarget = 4096;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status WarnStatus() {
  return {StatusCode::ok, Severity::warning, Subsystem::engine};
}
Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

bool SameFormatVersion(PageExtentSummaryFormatVersion left,
                       PageExtentSummaryFormatVersion right) {
  return left.major == right.major && left.minor == right.minor;
}

bool RecheckProofValid(const TextInvertedExactRecheckProof& proof) {
  return proof.proof_supplied &&
         proof.mga_recheck_required &&
         proof.security_recheck_required &&
         proof.source_recheck_required &&
         !proof.evidence_ref.empty();
}

bool TermValid(const std::string& term) {
  if (term.empty() || term.size() > 256) {
    return false;
  }
  return std::all_of(term.begin(), term.end(), [](unsigned char ch) {
    return ch > 0x20 && ch != 0x7f;
  });
}

bool LocatorValid(const TextInvertedRowLocator& locator) {
  return locator.row_ordinal > 0 &&
         PageExtentSummaryUuidTextValid(locator.row_uuid) &&
         PageExtentSummaryUuidTextValid(locator.version_uuid);
}

bool DocumentAuthorityClean(const TextInvertedDocumentInput& document) {
  return !document.parser_finality_authority_claimed &&
         !document.donor_finality_authority_claimed &&
         !document.provider_finality_authority_claimed &&
         !document.write_ahead_log_finality_authority_claimed &&
         !document.visibility_authority_claimed &&
         !document.security_authority_claimed &&
         !document.transaction_finality_authority_claimed;
}

bool DocumentValid(const TextInvertedDocumentInput& document) {
  return LocatorValid(document.locator) &&
         !document.normalized_terms.empty() &&
         std::all_of(document.normalized_terms.begin(),
                     document.normalized_terms.end(),
                     TermValid) &&
         !document.exact_source_recheck_evidence_ref.empty() &&
         DocumentAuthorityClean(document);
}

bool RequestAuthorityClean(const TextInvertedSegmentBuildRequest& request) {
  return !request.parser_finality_authority_claimed &&
         !request.donor_finality_authority_claimed &&
         !request.provider_finality_authority_claimed &&
         !request.write_ahead_log_finality_authority_claimed &&
         !request.visibility_authority_claimed &&
         !request.security_authority_claimed &&
         !request.transaction_finality_authority_claimed;
}

bool BuildRequestValid(const TextInvertedSegmentBuildRequest& request) {
  return PageExtentSummaryUuidTextValid(request.relation_uuid) &&
         PageExtentSummaryUuidTextValid(request.index_uuid) &&
         PageExtentSummaryUuidTextValid(request.segment_uuid) &&
         request.base_generation > 0 &&
         request.segment_generation > 0 &&
         request.analyzer_epoch > 0 &&
         request.resource_epoch > 0 &&
         request.block_posting_target > 0 &&
         request.block_posting_target <= kMaxBlockPostingTarget &&
         RecheckProofValid(request.recheck_proof) &&
         RequestAuthorityClean(request);
}

bool SegmentAuthorityClean(const TextInvertedSegment& segment) {
  return segment.candidate_evidence_only &&
         segment.exact_source_recheck_required &&
         segment.mga_recheck_required &&
         segment.security_recheck_required &&
         !segment.visibility_authority_claimed &&
         !segment.security_authority_claimed &&
         !segment.transaction_finality_authority_claimed &&
         !segment.parser_finality_authority_claimed &&
         !segment.donor_finality_authority_claimed &&
         !segment.provider_finality_authority_claimed &&
         !segment.write_ahead_log_finality_authority_claimed;
}

bool MetadataValid(const TextInvertedDocumentMetadata& document) {
  return LocatorValid(document.locator) &&
         document.document_length > 0 &&
         std::isfinite(document.norm) &&
         document.norm > 0.0 &&
         !document.exact_source_recheck_evidence_ref.empty();
}

bool PostingValid(const TextInvertedPosting& posting) {
  if (!LocatorValid(posting.locator) ||
      posting.document_length == 0 ||
      !std::isfinite(posting.norm) ||
      posting.norm <= 0.0 ||
      posting.positions.empty()) {
    return false;
  }
  return std::is_sorted(posting.positions.begin(), posting.positions.end());
}

bool BlockValid(const TextInvertedPostingBlock& block, u32 expected_ordinal) {
  if (block.block_ordinal != expected_ordinal ||
      !TermValid(block.term) ||
      block.first_row_ordinal == 0 ||
      block.last_row_ordinal < block.first_row_ordinal ||
      block.posting_count == 0 ||
      block.posting_count != block.postings.size() ||
      block.encoded_byte_count != block.encoded_postings.size() ||
      block.encoded_postings.empty() ||
      !block.row_ordinals_delta_coded ||
      !block.positions_delta_coded) {
    return false;
  }
  u64 previous = 0;
  for (const auto& posting : block.postings) {
    if (!PostingValid(posting) || posting.locator.row_ordinal <= previous) {
      return false;
    }
    previous = posting.locator.row_ordinal;
  }
  return block.postings.front().locator.row_ordinal == block.first_row_ordinal &&
         block.postings.back().locator.row_ordinal == block.last_row_ordinal;
}

bool SegmentValid(const TextInvertedSegment& segment) {
  if (segment.artifact_kind != kTextInvertedSegmentArtifactKind ||
      !SameFormatVersion(segment.format_version,
                         {kTextInvertedSegmentCurrentMajor,
                          kTextInvertedSegmentCurrentMinor}) ||
      !PageExtentSummaryUuidTextValid(segment.relation_uuid) ||
      !PageExtentSummaryUuidTextValid(segment.index_uuid) ||
      !PageExtentSummaryUuidTextValid(segment.segment_uuid) ||
      segment.base_generation == 0 ||
      segment.segment_generation == 0 ||
      segment.analyzer_epoch == 0 ||
      segment.resource_epoch == 0 ||
      segment.block_posting_target == 0 ||
      segment.state != TextInvertedSegmentState::sealed ||
      !segment.term_dictionary_present ||
      !segment.positions_present ||
      !segment.document_length_norms_present ||
      !segment.skip_metadata_present ||
      !segment.mutable_buffer_sealed ||
      !SegmentAuthorityClean(segment) ||
      segment.documents.empty() ||
      segment.dictionary.empty() ||
      segment.posting_blocks.empty() ||
      !segment.merge.sealed ||
      !segment.merge.immutable ||
      segment.merge.retired ||
      segment.merge.merge_metadata_finality_authority) {
    return false;
  }

  u64 previous_row = 0;
  for (const auto& document : segment.documents) {
    if (!MetadataValid(document) ||
        document.locator.row_ordinal <= previous_row) {
      return false;
    }
    previous_row = document.locator.row_ordinal;
  }

  std::string previous_term;
  bool first_term = true;
  for (const auto& entry : segment.dictionary) {
    if (!TermValid(entry.term) ||
        (!first_term && entry.term <= previous_term) ||
        entry.block_count == 0 ||
        entry.document_frequency == 0 ||
        entry.total_position_count == 0 ||
        entry.first_block_ordinal >= segment.posting_blocks.size() ||
        entry.first_block_ordinal + entry.block_count >
            segment.posting_blocks.size()) {
      return false;
    }
    u64 actual_document_frequency = 0;
    u64 actual_total_position_count = 0;
    for (u32 offset = 0; offset < entry.block_count; ++offset) {
      const auto& block =
          segment.posting_blocks[entry.first_block_ordinal + offset];
      if (block.term != entry.term) {
        return false;
      }
      actual_document_frequency += block.posting_count;
      for (const auto& posting : block.postings) {
        actual_total_position_count += posting.positions.size();
      }
    }
    if (actual_document_frequency != entry.document_frequency ||
        actual_total_position_count != entry.total_position_count) {
      return false;
    }
    previous_term = entry.term;
    first_term = false;
  }

  for (u32 index = 0; index < segment.posting_blocks.size(); ++index) {
    if (!BlockValid(segment.posting_blocks[index], index)) {
      return false;
    }
  }
  return true;
}

void AppendU8(std::vector<byte>* out, byte value) { out->push_back(value); }

void AppendU32(std::vector<byte>* out, u32 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(u32));
  StoreLittle32(out->data() + offset, value);
}

void AppendU64(std::vector<byte>* out, u64 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(u64));
  StoreLittle64(out->data() + offset, value);
}

void AppendDouble(std::vector<byte>* out, double value) {
  u64 bits = 0;
  static_assert(sizeof(bits) == sizeof(value),
                "double serialization requires stable width");
  std::memcpy(&bits, &value, sizeof(bits));
  AppendU64(out, bits);
}

void AppendString(std::vector<byte>* out, const std::string& value) {
  AppendU32(out, static_cast<u32>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

void AppendBytes(std::vector<byte>* out, const std::vector<byte>& bytes) {
  AppendU64(out, static_cast<u64>(bytes.size()));
  out->insert(out->end(), bytes.begin(), bytes.end());
}

void AppendVarint(std::vector<byte>* out, u64 value) {
  while (value >= 0x80) {
    out->push_back(static_cast<byte>((value & 0x7full) | 0x80ull));
    value >>= 7;
  }
  out->push_back(static_cast<byte>(value));
}

u64 ComputeChecksum(std::vector<byte> bytes) {
  if (bytes.size() >= kHeaderBytes) {
    StoreLittle64(bytes.data() + 16, 0);
  }
  u64 hash = kFnvOffset;
  for (byte value : bytes) {
    hash ^= static_cast<u64>(value);
    hash *= kFnvPrime;
  }
  return hash == 0 ? 1 : hash;
}

class Reader {
 public:
  explicit Reader(const std::vector<byte>& bytes) : bytes_(bytes) {}

  void SetOffset(std::size_t offset) { offset_ = offset; }

  bool ReadU8(byte* out) {
    if (offset_ + 1 > bytes_.size()) {
      return false;
    }
    *out = bytes_[offset_++];
    return true;
  }

  bool ReadU32(u32* out) {
    if (offset_ + sizeof(u32) > bytes_.size()) {
      return false;
    }
    *out = LoadLittle32(bytes_.data() + offset_);
    offset_ += sizeof(u32);
    return true;
  }

  bool ReadU64(u64* out) {
    if (offset_ + sizeof(u64) > bytes_.size()) {
      return false;
    }
    *out = LoadLittle64(bytes_.data() + offset_);
    offset_ += sizeof(u64);
    return true;
  }

  bool ReadDouble(double* out) {
    u64 bits = 0;
    if (!ReadU64(&bits)) {
      return false;
    }
    std::memcpy(out, &bits, sizeof(bits));
    return true;
  }

  bool ReadString(std::string* out) {
    u32 size = 0;
    if (!ReadU32(&size) || offset_ + size > bytes_.size()) {
      return false;
    }
    out->assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
    offset_ += size;
    return true;
  }

  bool ReadBytes(std::vector<byte>* out) {
    u64 size = 0;
    if (!ReadU64(&size) ||
        size > static_cast<u64>(std::numeric_limits<u32>::max()) ||
        offset_ + static_cast<std::size_t>(size) > bytes_.size()) {
      return false;
    }
    out->assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
    offset_ += static_cast<std::size_t>(size);
    return true;
  }

  bool ReadVarint(u64* out) {
    u64 value = 0;
    u32 shift = 0;
    for (;;) {
      byte encoded = 0;
      if (!ReadU8(&encoded) || shift >= 64) {
        return false;
      }
      value |= (static_cast<u64>(encoded & 0x7f) << shift);
      if ((encoded & 0x80) == 0) {
        *out = value;
        return true;
      }
      shift += 7;
    }
  }

  bool Done() const { return offset_ == bytes_.size(); }

 private:
  const std::vector<byte>& bytes_;
  std::size_t offset_ = 0;
};

TextInvertedSegmentMutationResult MutationFailure(std::string code,
                                                  std::string key,
                                                  std::string detail = {}) {
  TextInvertedSegmentMutationResult result;
  result.status = ErrorStatus();
  result.accepted = false;
  result.fail_closed = true;
  result.diagnostic = MakeTextInvertedSegmentDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

TextInvertedSegmentSealResult SealFailure(std::string code,
                                          std::string key,
                                          std::string detail = {}) {
  TextInvertedSegmentSealResult result;
  result.status = ErrorStatus();
  result.sealed = false;
  result.fail_closed = true;
  result.diagnostic = MakeTextInvertedSegmentDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

TextInvertedSegmentOpenResult OpenFailure(TextInvertedSegmentOpenClass open_class,
                                          std::string code,
                                          std::string key,
                                          std::string detail = {}) {
  TextInvertedSegmentOpenResult result;
  result.status = open_class == TextInvertedSegmentOpenClass::stale_format ||
                          open_class == TextInvertedSegmentOpenClass::stale_generation
                      ? WarnStatus()
                      : ErrorStatus();
  result.open_class = open_class;
  result.fail_closed = true;
  result.restricted_repair_required =
      open_class == TextInvertedSegmentOpenClass::bad_checksum ||
      open_class == TextInvertedSegmentOpenClass::corrupt_payload;
  result.diagnostic = MakeTextInvertedSegmentDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.actions.push_back("refuse_text_inverted_segment");
  if (result.restricted_repair_required) {
    result.actions.push_back(
        "repair_requires_authoritative_source_rows_and_exact_recheck");
  }
  return result;
}

TextInvertedQueryResult QueryFailure(std::string code,
                                     std::string key,
                                     std::string detail = {}) {
  TextInvertedQueryResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeTextInvertedSegmentDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.evidence.push_back("text_inverted_segment_candidates_refused=true");
  result.evidence.push_back("exact_source_recheck_required=true");
  result.evidence.push_back("mga_recheck_required=true");
  result.evidence.push_back("security_recheck_required=true");
  return result;
}

TextInvertedMergeScheduleResult MergeFailure(std::string code,
                                             std::string key,
                                             std::string detail = {}) {
  TextInvertedMergeScheduleResult result;
  result.status = ErrorStatus();
  result.scheduled = false;
  result.fail_closed = true;
  result.diagnostic = MakeTextInvertedSegmentDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.evidence.push_back("text_inverted_merge_schedule_refused=true");
  result.evidence.push_back("merge_metadata_finality_authority=false");
  return result;
}

std::vector<byte> EncodePostingBlock(
    const std::vector<TextInvertedPosting>& postings) {
  std::vector<byte> encoded;
  u64 previous_row = 0;
  for (const auto& posting : postings) {
    AppendVarint(&encoded, posting.locator.row_ordinal - previous_row);
    previous_row = posting.locator.row_ordinal;
    AppendString(&encoded, posting.locator.row_uuid);
    AppendString(&encoded, posting.locator.version_uuid);
    AppendU32(&encoded, posting.document_length);
    AppendDouble(&encoded, posting.norm);
    AppendVarint(&encoded, static_cast<u64>(posting.positions.size()));
    u64 previous_position = 0;
    for (u32 position : posting.positions) {
      AppendVarint(&encoded,
                   static_cast<u64>(position) - previous_position);
      previous_position = position;
    }
  }
  return encoded;
}

bool DecodePostingBlock(const TextInvertedPostingBlock& block,
                        std::vector<TextInvertedPosting>* postings) {
  Reader reader(block.encoded_postings);
  postings->clear();
  u64 row_ordinal = 0;
  for (u32 index = 0; index < block.posting_count; ++index) {
    u64 row_delta = 0;
    u64 position_count = 0;
    TextInvertedPosting posting;
    if (!reader.ReadVarint(&row_delta) ||
        !reader.ReadString(&posting.locator.row_uuid) ||
        !reader.ReadString(&posting.locator.version_uuid) ||
        !reader.ReadU32(&posting.document_length) ||
        !reader.ReadDouble(&posting.norm) ||
        !reader.ReadVarint(&position_count) ||
        position_count == 0 ||
        position_count > std::numeric_limits<u32>::max()) {
      return false;
    }
    row_ordinal += row_delta;
    posting.locator.row_ordinal = row_ordinal;
    u64 position = 0;
    posting.positions.reserve(static_cast<std::size_t>(position_count));
    for (u64 pos_index = 0; pos_index < position_count; ++pos_index) {
      u64 delta = 0;
      if (!reader.ReadVarint(&delta)) {
        return false;
      }
      position += delta;
      if (position > std::numeric_limits<u32>::max()) {
        return false;
      }
      posting.positions.push_back(static_cast<u32>(position));
    }
    postings->push_back(std::move(posting));
  }
  return reader.Done();
}

struct PostingAccumulator {
  TextInvertedRowLocator locator;
  u32 document_length = 0;
  double norm = 0.0;
  std::vector<u32> positions;
};

u64 EstimateArtifactBytes(const TextInvertedSegment& segment) {
  u64 total = 256;
  total += static_cast<u64>(segment.documents.size()) * 96;
  total += static_cast<u64>(segment.dictionary.size()) * 64;
  for (const auto& block : segment.posting_blocks) {
    total += block.encoded_byte_count + 48;
  }
  return total;
}

TextInvertedSegmentSealResult BuildSegment(
    const TextInvertedSegmentBuildRequest& request,
    const std::vector<TextInvertedDocumentInput>& input_documents) {
  if (!BuildRequestValid(request)) {
    return SealFailure("INDEX.TEXT_INVERTED_SEGMENT.BUILD_REFUSED",
                       "index.text_inverted_segment.build_refused",
                       "segment build requires valid identities generations epochs exact recheck proof and clean authority flags");
  }
  if (input_documents.empty()) {
    return SealFailure("INDEX.TEXT_INVERTED_SEGMENT.EMPTY_BUFFER_REFUSED",
                       "index.text_inverted_segment.empty_buffer_refused");
  }

  std::vector<TextInvertedDocumentInput> documents = input_documents;
  for (const auto& document : documents) {
    if (!DocumentValid(document)) {
      return SealFailure(
          "INDEX.TEXT_INVERTED_SEGMENT.DOCUMENT_REFUSED",
          "index.text_inverted_segment.document_refused",
          "documents require row/version locators normalized terms exact recheck evidence and no external authority claims");
    }
  }
  std::sort(documents.begin(), documents.end(),
            [](const auto& left, const auto& right) {
              return left.locator.row_ordinal < right.locator.row_ordinal;
            });
  for (std::size_t index = 1; index < documents.size(); ++index) {
    if (documents[index - 1].locator.row_ordinal ==
        documents[index].locator.row_ordinal) {
      return SealFailure(
          "INDEX.TEXT_INVERTED_SEGMENT.DUPLICATE_ROW_ORDINAL",
          "index.text_inverted_segment.duplicate_row_ordinal");
    }
  }

  std::map<std::string, std::map<u64, PostingAccumulator>> inverted;
  TextInvertedSegment segment;
  segment.relation_uuid = request.relation_uuid;
  segment.index_uuid = request.index_uuid;
  segment.segment_uuid = request.segment_uuid;
  segment.base_generation = request.base_generation;
  segment.segment_generation = request.segment_generation;
  segment.analyzer_epoch = request.analyzer_epoch;
  segment.resource_epoch = request.resource_epoch;
  segment.block_posting_target = request.block_posting_target;
  segment.state = TextInvertedSegmentState::sealed;
  segment.term_dictionary_present = true;
  segment.positions_present = true;
  segment.document_length_norms_present = true;
  segment.skip_metadata_present = true;
  segment.mutable_buffer_sealed = true;
  segment.merge.merge_tier = request.merge_tier;
  segment.merge.sealed_sequence = request.sealed_sequence;
  segment.merge.sealed = true;
  segment.merge.immutable = true;

  for (const auto& document : documents) {
    const u32 document_length =
        document.document_length == 0
            ? static_cast<u32>(document.normalized_terms.size())
            : document.document_length;
    const double norm =
        std::isfinite(document.norm) && document.norm > 0.0
            ? document.norm
            : 1.0 / std::sqrt(static_cast<double>(document_length));
    TextInvertedDocumentMetadata metadata;
    metadata.locator = document.locator;
    metadata.document_length = document_length;
    metadata.norm = norm;
    metadata.exact_source_recheck_evidence_ref =
        document.exact_source_recheck_evidence_ref;
    segment.documents.push_back(metadata);

    for (u32 position = 0; position < document.normalized_terms.size();
         ++position) {
      const auto& term = document.normalized_terms[position];
      auto& posting = inverted[term][document.locator.row_ordinal];
      if (posting.locator.row_ordinal == 0) {
        posting.locator = document.locator;
        posting.document_length = document_length;
        posting.norm = norm;
      }
      posting.positions.push_back(position);
    }
  }

  for (const auto& [term, by_row] : inverted) {
    TextInvertedTermDictionaryEntry entry;
    entry.term = term;
    entry.first_block_ordinal =
        static_cast<u32>(segment.posting_blocks.size());

    std::vector<TextInvertedPosting> postings;
    postings.reserve(by_row.size());
    for (const auto& [row, accumulator] : by_row) {
      (void)row;
      TextInvertedPosting posting;
      posting.locator = accumulator.locator;
      posting.document_length = accumulator.document_length;
      posting.norm = accumulator.norm;
      posting.positions = accumulator.positions;
      std::sort(posting.positions.begin(), posting.positions.end());
      entry.total_position_count += posting.positions.size();
      postings.push_back(std::move(posting));
    }
    entry.document_frequency = postings.size();

    for (std::size_t offset = 0; offset < postings.size();
         offset += request.block_posting_target) {
      const auto end =
          std::min<std::size_t>(offset + request.block_posting_target,
                                postings.size());
      TextInvertedPostingBlock block;
      block.block_ordinal = static_cast<u32>(segment.posting_blocks.size());
      block.term = term;
      block.postings.assign(postings.begin() + static_cast<std::ptrdiff_t>(offset),
                            postings.begin() + static_cast<std::ptrdiff_t>(end));
      block.posting_count = static_cast<u32>(block.postings.size());
      block.first_row_ordinal = block.postings.front().locator.row_ordinal;
      block.last_row_ordinal = block.postings.back().locator.row_ordinal;
      block.encoded_postings = EncodePostingBlock(block.postings);
      block.encoded_byte_count = block.encoded_postings.size();
      segment.posting_blocks.push_back(std::move(block));
      ++entry.block_count;
    }
    segment.dictionary.push_back(std::move(entry));
  }

  segment.merge.artifact_byte_count = EstimateArtifactBytes(segment);
  segment.evidence.push_back("text_inverted_term_dictionary_present=true");
  segment.evidence.push_back("posting_blocks_varint_delta_coded=true");
  segment.evidence.push_back("posting_positions_delta_coded=true");
  segment.evidence.push_back("skip_blocks_first_last_row_ordinal=true");
  segment.evidence.push_back("document_length_norm_metadata_present=true");
  segment.evidence.push_back("mutable_buffer_sealed_immutable_segment=true");
  segment.evidence.push_back("candidate_evidence_only=true");
  segment.evidence.push_back("descriptor_store_scan=false");
  segment.evidence.push_back("behavior_store_scan=false");
  segment.evidence.push_back("exact_source_recheck_required=true");
  segment.evidence.push_back("mga_recheck_required=true");
  segment.evidence.push_back("security_recheck_required=true");
  segment.evidence.push_back("visibility_security_finality_authority=false");
  segment.evidence.push_back("parser_donor_provider_finality_authority=false");
  segment.evidence.push_back("write_ahead_log_finality_authority=false");
  std::sort(segment.evidence.begin(), segment.evidence.end());

  if (!SegmentValid(segment)) {
    return SealFailure(
        "INDEX.TEXT_INVERTED_SEGMENT.BUILD_RESULT_INVALID",
        "index.text_inverted_segment.build_result_invalid");
  }

  TextInvertedSegmentSealResult result;
  result.status = OkStatus();
  result.segment = std::move(segment);
  result.sealed = true;
  result.fail_closed = false;
  result.diagnostic = MakeTextInvertedSegmentDiagnostic(
      result.status,
      "INDEX.TEXT_INVERTED_SEGMENT.SEALED",
      "index.text_inverted_segment.sealed");
  return result;
}

bool ReadBool(Reader* reader, bool* value) {
  byte raw = 0;
  if (!reader->ReadU8(&raw) || raw > 1) {
    return false;
  }
  *value = raw != 0;
  return true;
}

void AppendBool(std::vector<byte>* out, bool value) {
  AppendU8(out, value ? 1 : 0);
}

bool ReadDocument(Reader* reader, TextInvertedDocumentMetadata* document) {
  return reader->ReadU64(&document->locator.row_ordinal) &&
         reader->ReadString(&document->locator.row_uuid) &&
         reader->ReadString(&document->locator.version_uuid) &&
         reader->ReadU32(&document->document_length) &&
         reader->ReadDouble(&document->norm) &&
         reader->ReadString(&document->exact_source_recheck_evidence_ref);
}

bool ReadDictionaryEntry(Reader* reader,
                         TextInvertedTermDictionaryEntry* entry) {
  return reader->ReadString(&entry->term) &&
         reader->ReadU32(&entry->first_block_ordinal) &&
         reader->ReadU32(&entry->block_count) &&
         reader->ReadU64(&entry->document_frequency) &&
         reader->ReadU64(&entry->total_position_count);
}

bool ReadBlock(Reader* reader, TextInvertedPostingBlock* block) {
  if (!reader->ReadU32(&block->block_ordinal) ||
      !reader->ReadString(&block->term) ||
      !reader->ReadU64(&block->first_row_ordinal) ||
      !reader->ReadU64(&block->last_row_ordinal) ||
      !reader->ReadU32(&block->posting_count) ||
      !reader->ReadU64(&block->encoded_byte_count) ||
      !ReadBool(reader, &block->row_ordinals_delta_coded) ||
      !ReadBool(reader, &block->positions_delta_coded) ||
      !reader->ReadBytes(&block->encoded_postings) ||
      block->encoded_postings.size() != block->encoded_byte_count) {
    return false;
  }
  return DecodePostingBlock(*block, &block->postings);
}

bool ReadMergeMetadata(Reader* reader,
                       TextInvertedMergeScheduleMetadata* merge) {
  return reader->ReadU32(&merge->merge_tier) &&
         reader->ReadU64(&merge->sealed_sequence) &&
         reader->ReadU64(&merge->artifact_byte_count) &&
         ReadBool(reader, &merge->sealed) &&
         ReadBool(reader, &merge->immutable) &&
         ReadBool(reader, &merge->merge_in_progress) &&
         ReadBool(reader, &merge->retired) &&
         ReadBool(reader, &merge->merge_metadata_finality_authority);
}

bool ReadStringVector(Reader* reader, std::vector<std::string>* out) {
  u32 count = 0;
  if (!reader->ReadU32(&count)) {
    return false;
  }
  out->clear();
  out->reserve(count);
  for (u32 index = 0; index < count; ++index) {
    std::string value;
    if (!reader->ReadString(&value)) {
      return false;
    }
    out->push_back(std::move(value));
  }
  return true;
}

bool CandidateAuthorityClaimed(const TextInvertedMergeCandidate& candidate) {
  return candidate.merge_metadata_finality_authority ||
         candidate.parser_finality_authority_claimed ||
         candidate.donor_finality_authority_claimed ||
         candidate.provider_finality_authority_claimed ||
         candidate.write_ahead_log_finality_authority_claimed;
}

const TextInvertedTermDictionaryEntry* FindTerm(
    const TextInvertedSegment& segment,
    const std::string& term) {
  auto iter = std::lower_bound(
      segment.dictionary.begin(), segment.dictionary.end(), term,
      [](const TextInvertedTermDictionaryEntry& entry,
         const std::string& value) {
        return entry.term < value;
      });
  if (iter == segment.dictionary.end() || iter->term != term) {
    return nullptr;
  }
  return &*iter;
}

std::vector<TextInvertedPosting> CollectTermPostings(
    const TextInvertedSegment& segment,
    const std::string& term,
    u64 lower_bound_row_ordinal,
    u64* skipped_block_count,
    u64* scanned_block_count) {
  std::vector<TextInvertedPosting> postings;
  const auto* entry = FindTerm(segment, term);
  if (entry == nullptr) {
    return postings;
  }
  for (u32 offset = 0; offset < entry->block_count; ++offset) {
    const auto& block =
        segment.posting_blocks[entry->first_block_ordinal + offset];
    if (block.last_row_ordinal < lower_bound_row_ordinal) {
      ++(*skipped_block_count);
      continue;
    }
    ++(*scanned_block_count);
    for (const auto& posting : block.postings) {
      if (posting.locator.row_ordinal >= lower_bound_row_ordinal) {
        postings.push_back(posting);
      }
    }
  }
  return postings;
}

bool ContainsPosition(const std::vector<u32>& positions, u32 value) {
  return std::binary_search(positions.begin(), positions.end(), value);
}

TextInvertedQueryResult QuerySuccess(std::vector<TextInvertedRowLocator> candidates,
                                     u64 skipped_blocks,
                                     u64 scanned_blocks,
                                     std::string diagnostic_code,
                                     std::string message_key) {
  TextInvertedQueryResult result;
  result.status = OkStatus();
  result.candidates = std::move(candidates);
  result.skipped_block_count = skipped_blocks;
  result.scanned_block_count = scanned_blocks;
  result.evidence.push_back("text_inverted_segment_candidate_rows_only=true");
  result.evidence.push_back("term_dictionary_lookup=true");
  result.evidence.push_back("descriptor_store_scan=false");
  result.evidence.push_back("behavior_store_scan=false");
  result.evidence.push_back("exact_source_recheck_required=true");
  result.evidence.push_back("mga_recheck_required=true");
  result.evidence.push_back("security_recheck_required=true");
  result.evidence.push_back("visibility_security_finality_authority=false");
  result.diagnostic = MakeTextInvertedSegmentDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key));
  return result;
}

}  // namespace

const char* TextInvertedSegmentStateName(TextInvertedSegmentState state) {
  switch (state) {
    case TextInvertedSegmentState::mutable_buffer: return "mutable_buffer";
    case TextInvertedSegmentState::sealed: return "sealed";
    case TextInvertedSegmentState::retired: return "retired";
    case TextInvertedSegmentState::refused: return "refused";
  }
  return "refused";
}

const char* TextInvertedSegmentOpenClassName(
    TextInvertedSegmentOpenClass open_class) {
  switch (open_class) {
    case TextInvertedSegmentOpenClass::current: return "current";
    case TextInvertedSegmentOpenClass::stale_format: return "stale_format";
    case TextInvertedSegmentOpenClass::bad_checksum: return "bad_checksum";
    case TextInvertedSegmentOpenClass::corrupt_payload: return "corrupt_payload";
    case TextInvertedSegmentOpenClass::identity_mismatch:
      return "identity_mismatch";
    case TextInvertedSegmentOpenClass::stale_generation:
      return "stale_generation";
    case TextInvertedSegmentOpenClass::unsafe_analyzer_or_resource_epoch:
      return "unsafe_analyzer_or_resource_epoch";
    case TextInvertedSegmentOpenClass::missing_exact_recheck_proof:
      return "missing_exact_recheck_proof";
    case TextInvertedSegmentOpenClass::authority_claim_refused:
      return "authority_claim_refused";
    case TextInvertedSegmentOpenClass::refused: return "refused";
  }
  return "refused";
}

const char* TextInvertedQueryKindName(TextInvertedQueryKind kind) {
  switch (kind) {
    case TextInvertedQueryKind::term: return "term";
    case TextInvertedQueryKind::all_terms: return "all_terms";
    case TextInvertedQueryKind::phrase: return "phrase";
  }
  return "term";
}

TextInvertedMutableSegmentBuffer CreateTextInvertedMutableSegmentBuffer(
    const TextInvertedSegmentBuildRequest& request) {
  TextInvertedMutableSegmentBuffer buffer;
  buffer.request = request;
  return buffer;
}

TextInvertedSegmentMutationResult AddTextInvertedDocument(
    TextInvertedMutableSegmentBuffer* buffer,
    const TextInvertedDocumentInput& document) {
  if (buffer == nullptr || buffer->sealed) {
    return MutationFailure("INDEX.TEXT_INVERTED_SEGMENT.BUFFER_CLOSED",
                           "index.text_inverted_segment.buffer_closed");
  }
  if (!DocumentValid(document)) {
    return MutationFailure(
        "INDEX.TEXT_INVERTED_SEGMENT.DOCUMENT_REFUSED",
        "index.text_inverted_segment.document_refused",
        "document requires normalized terms exact recheck evidence and no finality authority claims");
  }
  buffer->documents.push_back(document);
  TextInvertedSegmentMutationResult result;
  result.status = OkStatus();
  result.accepted = true;
  result.fail_closed = false;
  result.diagnostic = MakeTextInvertedSegmentDiagnostic(
      result.status,
      "INDEX.TEXT_INVERTED_SEGMENT.DOCUMENT_BUFFERED",
      "index.text_inverted_segment.document_buffered");
  return result;
}

TextInvertedSegmentSealResult SealTextInvertedSegmentBuffer(
    TextInvertedMutableSegmentBuffer* buffer) {
  if (buffer == nullptr || buffer->sealed) {
    return SealFailure("INDEX.TEXT_INVERTED_SEGMENT.BUFFER_CLOSED",
                       "index.text_inverted_segment.buffer_closed");
  }
  auto result = BuildSegment(buffer->request, buffer->documents);
  if (result.ok()) {
    buffer->sealed = true;
  }
  return result;
}

TextInvertedSegmentSealResult BuildTextInvertedSegmentFromDocuments(
    const TextInvertedSegmentBuildRequest& request,
    const std::vector<TextInvertedDocumentInput>& documents) {
  return BuildSegment(request, documents);
}

TextInvertedSegmentSerializeResult SerializeTextInvertedSegmentArtifact(
    const TextInvertedSegment& input_segment) {
  TextInvertedSegmentSerializeResult result;
  TextInvertedSegment segment = input_segment;
  std::sort(segment.evidence.begin(), segment.evidence.end());
  if (!SegmentValid(segment)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeTextInvertedSegmentDiagnostic(
        result.status,
        "INDEX.TEXT_INVERTED_SEGMENT.SERIALIZE_REFUSED",
        "index.text_inverted_segment.serialize_refused");
    return result;
  }

  auto& out = result.bytes;
  out.insert(out.end(), kMagic.begin(), kMagic.end());
  AppendU32(&out, segment.format_version.major);
  AppendU32(&out, segment.format_version.minor);
  AppendU64(&out, 0);
  AppendString(&out, segment.artifact_kind);
  AppendString(&out, segment.relation_uuid);
  AppendString(&out, segment.index_uuid);
  AppendString(&out, segment.segment_uuid);
  AppendU64(&out, segment.base_generation);
  AppendU64(&out, segment.segment_generation);
  AppendU64(&out, segment.analyzer_epoch);
  AppendU64(&out, segment.resource_epoch);
  AppendU32(&out, segment.block_posting_target);
  AppendU32(&out, static_cast<u32>(segment.state));
  AppendBool(&out, segment.term_dictionary_present);
  AppendBool(&out, segment.positions_present);
  AppendBool(&out, segment.document_length_norms_present);
  AppendBool(&out, segment.skip_metadata_present);
  AppendBool(&out, segment.mutable_buffer_sealed);
  AppendBool(&out, segment.candidate_evidence_only);
  AppendBool(&out, segment.exact_source_recheck_required);
  AppendBool(&out, segment.mga_recheck_required);
  AppendBool(&out, segment.security_recheck_required);
  AppendBool(&out, segment.visibility_authority_claimed);
  AppendBool(&out, segment.security_authority_claimed);
  AppendBool(&out, segment.transaction_finality_authority_claimed);
  AppendBool(&out, segment.parser_finality_authority_claimed);
  AppendBool(&out, segment.donor_finality_authority_claimed);
  AppendBool(&out, segment.provider_finality_authority_claimed);
  AppendBool(&out, segment.write_ahead_log_finality_authority_claimed);

  AppendU32(&out, static_cast<u32>(segment.documents.size()));
  for (const auto& document : segment.documents) {
    AppendU64(&out, document.locator.row_ordinal);
    AppendString(&out, document.locator.row_uuid);
    AppendString(&out, document.locator.version_uuid);
    AppendU32(&out, document.document_length);
    AppendDouble(&out, document.norm);
    AppendString(&out, document.exact_source_recheck_evidence_ref);
  }

  AppendU32(&out, static_cast<u32>(segment.dictionary.size()));
  for (const auto& entry : segment.dictionary) {
    AppendString(&out, entry.term);
    AppendU32(&out, entry.first_block_ordinal);
    AppendU32(&out, entry.block_count);
    AppendU64(&out, entry.document_frequency);
    AppendU64(&out, entry.total_position_count);
  }

  AppendU32(&out, static_cast<u32>(segment.posting_blocks.size()));
  for (const auto& block : segment.posting_blocks) {
    AppendU32(&out, block.block_ordinal);
    AppendString(&out, block.term);
    AppendU64(&out, block.first_row_ordinal);
    AppendU64(&out, block.last_row_ordinal);
    AppendU32(&out, block.posting_count);
    AppendU64(&out, block.encoded_byte_count);
    AppendBool(&out, block.row_ordinals_delta_coded);
    AppendBool(&out, block.positions_delta_coded);
    AppendBytes(&out, block.encoded_postings);
  }

  AppendU32(&out, static_cast<u32>(segment.evidence.size()));
  for (const auto& evidence : segment.evidence) {
    AppendString(&out, evidence);
  }

  AppendU32(&out, segment.merge.merge_tier);
  AppendU64(&out, segment.merge.sealed_sequence);
  AppendU64(&out, segment.merge.artifact_byte_count);
  AppendBool(&out, segment.merge.sealed);
  AppendBool(&out, segment.merge.immutable);
  AppendBool(&out, segment.merge.merge_in_progress);
  AppendBool(&out, segment.merge.retired);
  AppendBool(&out, segment.merge.merge_metadata_finality_authority);

  result.checksum = ComputeChecksum(out);
  StoreLittle64(out.data() + 16, result.checksum);
  result.status = OkStatus();
  result.diagnostic = MakeTextInvertedSegmentDiagnostic(
      result.status,
      "INDEX.TEXT_INVERTED_SEGMENT.SERIALIZED",
      "index.text_inverted_segment.serialized");
  return result;
}

TextInvertedSegmentOpenResult OpenTextInvertedSegmentArtifact(
    const TextInvertedSegmentOpenRequest& request) {
  if (request.bytes.size() < kHeaderBytes ||
      !std::equal(kMagic.begin(), kMagic.end(), request.bytes.begin())) {
    return OpenFailure(TextInvertedSegmentOpenClass::corrupt_payload,
                       "INDEX.TEXT_INVERTED_SEGMENT.BAD_MAGIC",
                       "index.text_inverted_segment.bad_magic");
  }
  Reader reader(request.bytes);
  reader.SetOffset(8);
  TextInvertedSegment segment;
  u64 stored_checksum = 0;
  u32 state = 0;
  u32 document_count = 0;
  u32 dictionary_count = 0;
  u32 block_count = 0;
  if (!reader.ReadU32(&segment.format_version.major) ||
      !reader.ReadU32(&segment.format_version.minor) ||
      !reader.ReadU64(&stored_checksum)) {
    return OpenFailure(TextInvertedSegmentOpenClass::corrupt_payload,
                       "INDEX.TEXT_INVERTED_SEGMENT.TRUNCATED_HEADER",
                       "index.text_inverted_segment.truncated_header");
  }
  if (stored_checksum != ComputeChecksum(request.bytes)) {
    return OpenFailure(TextInvertedSegmentOpenClass::bad_checksum,
                       "INDEX.TEXT_INVERTED_SEGMENT.BAD_CHECKSUM",
                       "index.text_inverted_segment.bad_checksum");
  }
  if (!SameFormatVersion(segment.format_version,
                         {kTextInvertedSegmentCurrentMajor,
                          kTextInvertedSegmentCurrentMinor})) {
    return OpenFailure(TextInvertedSegmentOpenClass::stale_format,
                       "INDEX.TEXT_INVERTED_SEGMENT.STALE_FORMAT",
                       "index.text_inverted_segment.stale_format");
  }

  if (!reader.ReadString(&segment.artifact_kind) ||
      !reader.ReadString(&segment.relation_uuid) ||
      !reader.ReadString(&segment.index_uuid) ||
      !reader.ReadString(&segment.segment_uuid) ||
      !reader.ReadU64(&segment.base_generation) ||
      !reader.ReadU64(&segment.segment_generation) ||
      !reader.ReadU64(&segment.analyzer_epoch) ||
      !reader.ReadU64(&segment.resource_epoch) ||
      !reader.ReadU32(&segment.block_posting_target) ||
      !reader.ReadU32(&state) ||
      !ReadBool(&reader, &segment.term_dictionary_present) ||
      !ReadBool(&reader, &segment.positions_present) ||
      !ReadBool(&reader, &segment.document_length_norms_present) ||
      !ReadBool(&reader, &segment.skip_metadata_present) ||
      !ReadBool(&reader, &segment.mutable_buffer_sealed) ||
      !ReadBool(&reader, &segment.candidate_evidence_only) ||
      !ReadBool(&reader, &segment.exact_source_recheck_required) ||
      !ReadBool(&reader, &segment.mga_recheck_required) ||
      !ReadBool(&reader, &segment.security_recheck_required) ||
      !ReadBool(&reader, &segment.visibility_authority_claimed) ||
      !ReadBool(&reader, &segment.security_authority_claimed) ||
      !ReadBool(&reader, &segment.transaction_finality_authority_claimed) ||
      !ReadBool(&reader, &segment.parser_finality_authority_claimed) ||
      !ReadBool(&reader, &segment.donor_finality_authority_claimed) ||
      !ReadBool(&reader, &segment.provider_finality_authority_claimed) ||
      !ReadBool(&reader, &segment.write_ahead_log_finality_authority_claimed) ||
      !reader.ReadU32(&document_count)) {
    return OpenFailure(TextInvertedSegmentOpenClass::corrupt_payload,
                       "INDEX.TEXT_INVERTED_SEGMENT.MALFORMED_PAYLOAD",
                       "index.text_inverted_segment.malformed_payload");
  }
  segment.state = static_cast<TextInvertedSegmentState>(state);

  segment.documents.resize(document_count);
  for (auto& document : segment.documents) {
    if (!ReadDocument(&reader, &document)) {
      return OpenFailure(TextInvertedSegmentOpenClass::corrupt_payload,
                         "INDEX.TEXT_INVERTED_SEGMENT.MALFORMED_DOCUMENTS",
                         "index.text_inverted_segment.malformed_documents");
    }
  }
  if (!reader.ReadU32(&dictionary_count)) {
    return OpenFailure(TextInvertedSegmentOpenClass::corrupt_payload,
                       "INDEX.TEXT_INVERTED_SEGMENT.MALFORMED_DICTIONARY",
                       "index.text_inverted_segment.malformed_dictionary");
  }
  segment.dictionary.resize(dictionary_count);
  for (auto& entry : segment.dictionary) {
    if (!ReadDictionaryEntry(&reader, &entry)) {
      return OpenFailure(TextInvertedSegmentOpenClass::corrupt_payload,
                         "INDEX.TEXT_INVERTED_SEGMENT.MALFORMED_DICTIONARY",
                         "index.text_inverted_segment.malformed_dictionary");
    }
  }
  if (!reader.ReadU32(&block_count)) {
    return OpenFailure(TextInvertedSegmentOpenClass::corrupt_payload,
                       "INDEX.TEXT_INVERTED_SEGMENT.MALFORMED_BLOCKS",
                       "index.text_inverted_segment.malformed_blocks");
  }
  segment.posting_blocks.resize(block_count);
  for (auto& block : segment.posting_blocks) {
    if (!ReadBlock(&reader, &block)) {
      return OpenFailure(TextInvertedSegmentOpenClass::corrupt_payload,
                         "INDEX.TEXT_INVERTED_SEGMENT.MALFORMED_BLOCKS",
                         "index.text_inverted_segment.malformed_blocks");
    }
  }
  if (!ReadStringVector(&reader, &segment.evidence) ||
      !ReadMergeMetadata(&reader, &segment.merge) ||
      !reader.Done()) {
    return OpenFailure(TextInvertedSegmentOpenClass::corrupt_payload,
                       "INDEX.TEXT_INVERTED_SEGMENT.MALFORMED_TRAILER",
                       "index.text_inverted_segment.malformed_trailer");
  }

  if (!SegmentAuthorityClean(segment) ||
      segment.merge.merge_metadata_finality_authority) {
    auto result =
        OpenFailure(TextInvertedSegmentOpenClass::authority_claim_refused,
                    "INDEX.TEXT_INVERTED_SEGMENT.AUTHORITY_CLAIM_REFUSED",
                    "index.text_inverted_segment.authority_claim_refused");
    result.segment = segment;
    return result;
  }
  if ((request.expected_relation_uuid_present &&
       request.expected_relation_uuid != segment.relation_uuid) ||
      (request.expected_index_uuid_present &&
       request.expected_index_uuid != segment.index_uuid) ||
      (request.expected_segment_uuid_present &&
       request.expected_segment_uuid != segment.segment_uuid)) {
    auto result =
        OpenFailure(TextInvertedSegmentOpenClass::identity_mismatch,
                    "INDEX.TEXT_INVERTED_SEGMENT.IDENTITY_MISMATCH",
                    "index.text_inverted_segment.identity_mismatch");
    result.segment = segment;
    return result;
  }
  if ((request.expected_base_generation_present &&
       request.expected_base_generation != segment.base_generation) ||
      (request.expected_segment_generation_present &&
       request.expected_segment_generation != segment.segment_generation)) {
    auto result =
        OpenFailure(TextInvertedSegmentOpenClass::stale_generation,
                    "INDEX.TEXT_INVERTED_SEGMENT.STALE_GENERATION",
                    "index.text_inverted_segment.stale_generation");
    result.segment = segment;
    return result;
  }
  if ((request.expected_analyzer_epoch_present &&
       request.expected_analyzer_epoch != segment.analyzer_epoch) ||
      (request.expected_resource_epoch_present &&
       request.expected_resource_epoch != segment.resource_epoch)) {
    auto result = OpenFailure(
        TextInvertedSegmentOpenClass::unsafe_analyzer_or_resource_epoch,
        "INDEX.TEXT_INVERTED_SEGMENT.UNSAFE_ANALYZER_RESOURCE_EPOCH",
        "index.text_inverted_segment.unsafe_analyzer_resource_epoch");
    result.segment = segment;
    return result;
  }
  if (!RecheckProofValid(request.recheck_proof)) {
    auto result =
        OpenFailure(TextInvertedSegmentOpenClass::missing_exact_recheck_proof,
                    "INDEX.TEXT_INVERTED_SEGMENT.RECHECK_PROOF_MISSING",
                    "index.text_inverted_segment.recheck_proof_missing");
    result.segment = segment;
    return result;
  }
  if (!SegmentValid(segment)) {
    auto result =
        OpenFailure(TextInvertedSegmentOpenClass::corrupt_payload,
                    "INDEX.TEXT_INVERTED_SEGMENT.PAYLOAD_INVALID",
                    "index.text_inverted_segment.payload_invalid");
    result.segment = segment;
    return result;
  }

  TextInvertedSegmentOpenResult result;
  result.status = OkStatus();
  result.open_class = TextInvertedSegmentOpenClass::current;
  result.segment = std::move(segment);
  result.fail_closed = false;
  result.restricted_repair_required = false;
  result.actions.push_back("use_text_inverted_segment_as_candidate_evidence");
  result.actions.push_back("require_exact_source_mga_security_recheck");
  result.diagnostic = MakeTextInvertedSegmentDiagnostic(
      result.status,
      "INDEX.TEXT_INVERTED_SEGMENT.OPENED",
      "index.text_inverted_segment.opened");
  return result;
}

TextInvertedQueryResult ProbeTextInvertedSegment(
    const TextInvertedQueryRequest& request) {
  if (!SegmentValid(request.segment)) {
    return QueryFailure("INDEX.TEXT_INVERTED_SEGMENT.QUERY_SEGMENT_INVALID",
                        "index.text_inverted_segment.query_segment_invalid");
  }
  if (!request.analyzer_epoch_current || !request.resource_epoch_current) {
    return QueryFailure(
        "INDEX.TEXT_INVERTED_SEGMENT.QUERY_UNSAFE_EPOCH",
        "index.text_inverted_segment.query_unsafe_epoch");
  }
  if (!RecheckProofValid(request.recheck_proof)) {
    return QueryFailure(
        "INDEX.TEXT_INVERTED_SEGMENT.QUERY_RECHECK_PROOF_MISSING",
        "index.text_inverted_segment.query_recheck_proof_missing");
  }
  if (request.terms.empty() ||
      !std::all_of(request.terms.begin(), request.terms.end(), TermValid)) {
    return QueryFailure("INDEX.TEXT_INVERTED_SEGMENT.QUERY_TERMS_INVALID",
                        "index.text_inverted_segment.query_terms_invalid",
                        TextInvertedQueryKindName(request.kind));
  }

  u64 skipped_blocks = 0;
  u64 scanned_blocks = 0;
  std::vector<TextInvertedRowLocator> candidates;
  switch (request.kind) {
    case TextInvertedQueryKind::term: {
      const auto postings = CollectTermPostings(request.segment,
                                                request.terms.front(),
                                                request.lower_bound_row_ordinal,
                                                &skipped_blocks,
                                                &scanned_blocks);
      candidates.reserve(postings.size());
      for (const auto& posting : postings) {
        candidates.push_back(posting.locator);
      }
      return QuerySuccess(std::move(candidates),
                          skipped_blocks,
                          scanned_blocks,
                          "INDEX.TEXT_INVERTED_SEGMENT.TERM_PROBED",
                          "index.text_inverted_segment.term_probed");
    }
    case TextInvertedQueryKind::all_terms: {
      std::map<u64, TextInvertedRowLocator> intersection;
      bool first = true;
      for (const auto& term : request.terms) {
        const auto postings = CollectTermPostings(request.segment,
                                                  term,
                                                  request.lower_bound_row_ordinal,
                                                  &skipped_blocks,
                                                  &scanned_blocks);
        std::set<u64> rows;
        for (const auto& posting : postings) {
          rows.insert(posting.locator.row_ordinal);
          if (first) {
            intersection[posting.locator.row_ordinal] = posting.locator;
          }
        }
        if (!first) {
          for (auto iter = intersection.begin(); iter != intersection.end();) {
            if (rows.find(iter->first) == rows.end()) {
              iter = intersection.erase(iter);
            } else {
              ++iter;
            }
          }
        }
        first = false;
      }
      for (const auto& [row, locator] : intersection) {
        (void)row;
        candidates.push_back(locator);
      }
      return QuerySuccess(std::move(candidates),
                          skipped_blocks,
                          scanned_blocks,
                          "INDEX.TEXT_INVERTED_SEGMENT.ALL_TERMS_PROBED",
                          "index.text_inverted_segment.all_terms_probed");
    }
    case TextInvertedQueryKind::phrase: {
      std::vector<std::map<u64, TextInvertedPosting>> postings_by_term;
      postings_by_term.reserve(request.terms.size());
      for (const auto& term : request.terms) {
        const auto postings = CollectTermPostings(request.segment,
                                                  term,
                                                  request.lower_bound_row_ordinal,
                                                  &skipped_blocks,
                                                  &scanned_blocks);
        std::map<u64, TextInvertedPosting> by_row;
        for (const auto& posting : postings) {
          by_row[posting.locator.row_ordinal] = posting;
        }
        postings_by_term.push_back(std::move(by_row));
      }
      if (!postings_by_term.empty()) {
        for (const auto& [row, first_posting] : postings_by_term.front()) {
          bool all_terms_present = true;
          for (std::size_t term_index = 1;
               term_index < postings_by_term.size();
               ++term_index) {
            if (postings_by_term[term_index].find(row) ==
                postings_by_term[term_index].end()) {
              all_terms_present = false;
              break;
            }
          }
          if (!all_terms_present) {
            continue;
          }
          bool phrase_match = request.terms.size() == 1;
          for (u32 start : first_posting.positions) {
            phrase_match = true;
            for (std::size_t term_index = 1;
                 term_index < postings_by_term.size();
                 ++term_index) {
              const auto& positions =
                  postings_by_term[term_index].find(row)->second.positions;
              const u64 wanted = static_cast<u64>(start) + term_index;
              if (wanted > std::numeric_limits<u32>::max() ||
                  !ContainsPosition(positions, static_cast<u32>(wanted))) {
                phrase_match = false;
                break;
              }
            }
            if (phrase_match) {
              candidates.push_back(first_posting.locator);
              break;
            }
          }
        }
      }
      return QuerySuccess(std::move(candidates),
                          skipped_blocks,
                          scanned_blocks,
                          "INDEX.TEXT_INVERTED_SEGMENT.PHRASE_PROBED",
                          "index.text_inverted_segment.phrase_probed");
    }
  }
  return QueryFailure("INDEX.TEXT_INVERTED_SEGMENT.QUERY_KIND_INVALID",
                      "index.text_inverted_segment.query_kind_invalid");
}

TextInvertedMergeCandidate TextInvertedMergeCandidateFromSegment(
    const TextInvertedSegment& segment) {
  TextInvertedMergeCandidate candidate;
  candidate.relation_uuid = segment.relation_uuid;
  candidate.index_uuid = segment.index_uuid;
  candidate.segment_uuid = segment.segment_uuid;
  candidate.segment_generation = segment.segment_generation;
  candidate.analyzer_epoch = segment.analyzer_epoch;
  candidate.resource_epoch = segment.resource_epoch;
  candidate.merge_tier = segment.merge.merge_tier;
  candidate.sealed_sequence = segment.merge.sealed_sequence;
  candidate.document_count = segment.documents.size();
  candidate.term_count = segment.dictionary.size();
  candidate.posting_block_count = segment.posting_blocks.size();
  candidate.artifact_byte_count =
      segment.merge.artifact_byte_count == 0
          ? EstimateArtifactBytes(segment)
          : segment.merge.artifact_byte_count;
  candidate.sealed = segment.merge.sealed;
  candidate.immutable = segment.merge.immutable;
  candidate.merge_in_progress = segment.merge.merge_in_progress;
  candidate.retired = segment.merge.retired ||
                      segment.state == TextInvertedSegmentState::retired;
  candidate.merge_metadata_finality_authority =
      segment.merge.merge_metadata_finality_authority;
  candidate.parser_finality_authority_claimed =
      segment.parser_finality_authority_claimed;
  candidate.donor_finality_authority_claimed =
      segment.donor_finality_authority_claimed;
  candidate.provider_finality_authority_claimed =
      segment.provider_finality_authority_claimed;
  candidate.write_ahead_log_finality_authority_claimed =
      segment.write_ahead_log_finality_authority_claimed;
  candidate.eligible = SegmentValid(segment) &&
                       candidate.sealed &&
                       candidate.immutable &&
                       !candidate.merge_in_progress &&
                       !candidate.retired &&
                       !CandidateAuthorityClaimed(candidate);
  return candidate;
}

TextInvertedMergeScheduleResult SelectTextInvertedSegmentsForMerge(
    const TextInvertedMergeScheduleRequest& request) {
  if (request.scheduler_claims_finality_authority) {
    return MergeFailure(
        "INDEX.TEXT_INVERTED_SEGMENT.MERGE_AUTHORITY_REFUSED",
        "index.text_inverted_segment.merge_authority_refused");
  }
  if (!RecheckProofValid(request.recheck_proof)) {
    return MergeFailure(
        "INDEX.TEXT_INVERTED_SEGMENT.MERGE_RECHECK_PROOF_MISSING",
        "index.text_inverted_segment.merge_recheck_proof_missing");
  }
  const u32 minimum = std::max<u32>(2, request.minimum_segment_count);
  std::vector<TextInvertedMergeCandidate> candidates = request.candidates;
  for (const auto& candidate : candidates) {
    if (CandidateAuthorityClaimed(candidate)) {
      return MergeFailure(
          "INDEX.TEXT_INVERTED_SEGMENT.MERGE_INPUT_AUTHORITY_REFUSED",
          "index.text_inverted_segment.merge_input_authority_refused",
          candidate.segment_uuid);
    }
  }
  std::sort(candidates.begin(), candidates.end(), [](const auto& left,
                                                     const auto& right) {
    return std::tie(left.index_uuid,
                    left.relation_uuid,
                    left.analyzer_epoch,
                    left.resource_epoch,
                    left.merge_tier,
                    left.sealed_sequence,
                    left.segment_generation,
                    left.segment_uuid) <
           std::tie(right.index_uuid,
                    right.relation_uuid,
                    right.analyzer_epoch,
                    right.resource_epoch,
                    right.merge_tier,
                    right.sealed_sequence,
                    right.segment_generation,
                    right.segment_uuid);
  });

  for (std::size_t start = 0; start < candidates.size(); ++start) {
    if (!candidates[start].eligible) {
      continue;
    }
    std::vector<TextInvertedMergeCandidate> selected;
    u64 total_bytes = 0;
    for (std::size_t index = start; index < candidates.size(); ++index) {
      const auto& candidate = candidates[index];
      if (!candidate.eligible ||
          candidate.index_uuid != candidates[start].index_uuid ||
          candidate.relation_uuid != candidates[start].relation_uuid ||
          candidate.analyzer_epoch != candidates[start].analyzer_epoch ||
          candidate.resource_epoch != candidates[start].resource_epoch ||
          candidate.merge_tier != candidates[start].merge_tier) {
        break;
      }
      const u64 next_total = total_bytes + candidate.artifact_byte_count;
      if (request.max_total_artifact_bytes != 0 &&
          next_total > request.max_total_artifact_bytes) {
        break;
      }
      total_bytes = next_total;
      selected.push_back(candidate);
      if (selected.size() >= minimum) {
        TextInvertedMergeScheduleResult result;
        result.status = OkStatus();
        result.scheduled = true;
        result.fail_closed = false;
        result.selected = std::move(selected);
        result.evidence.push_back("text_inverted_merge_candidates_selected=true");
        result.evidence.push_back("merge_metadata_finality_authority=false");
        result.evidence.push_back(
            "old_segments_remain_searchable_until_engine_publish=true");
        result.diagnostic = MakeTextInvertedSegmentDiagnostic(
            result.status,
            "INDEX.TEXT_INVERTED_SEGMENT.MERGE_SCHEDULED",
            "index.text_inverted_segment.merge_scheduled");
        return result;
      }
    }
  }

  TextInvertedMergeScheduleResult result;
  result.status = WarnStatus();
  result.scheduled = false;
  result.fail_closed = false;
  result.evidence.push_back("text_inverted_merge_candidates_selected=false");
  result.evidence.push_back("merge_metadata_finality_authority=false");
  result.diagnostic = MakeTextInvertedSegmentDiagnostic(
      result.status,
      "INDEX.TEXT_INVERTED_SEGMENT.MERGE_NOT_ELIGIBLE",
      "index.text_inverted_segment.merge_not_eligible");
  return result;
}

DiagnosticRecord MakeTextInvertedSegmentDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.index.text_inverted_segment");
}

}  // namespace scratchbird::core::index
