// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "runtime_platform.hpp"
#include "text_inverted_segment.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "text_inverted_segment_engine_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(const std::vector<std::string>& values, std::string_view needle) {
  return std::find(values.begin(), values.end(), needle) != values.end();
}

idx::TextInvertedExactRecheckProof Proof() {
  idx::TextInvertedExactRecheckProof proof;
  proof.proof_supplied = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.source_recheck_required = true;
  proof.evidence_ref = "base_row_exact_recheck_contract";
  return proof;
}

idx::TextInvertedSegmentBuildRequest BuildRequest(std::uint64_t generation,
                                                  std::uint64_t sequence) {
  idx::TextInvertedSegmentBuildRequest request;
  request.relation_uuid = "11111111-1111-7111-8111-111111111111";
  request.index_uuid = "22222222-2222-7222-8222-222222222222";
  request.segment_uuid =
      generation == 11 ? "33333333-3333-7333-8333-333333333333"
                       : "44444444-4444-7444-8444-444444444444";
  request.base_generation = 7;
  request.segment_generation = generation;
  request.analyzer_epoch = 13;
  request.resource_epoch = 17;
  request.block_posting_target = 2;
  request.merge_tier = 0;
  request.sealed_sequence = sequence;
  request.recheck_proof = Proof();
  return request;
}

idx::TextInvertedDocumentInput Doc(std::uint64_t row,
                                   std::string row_uuid,
                                   std::string version_uuid,
                                   std::vector<std::string> terms) {
  idx::TextInvertedDocumentInput doc;
  doc.locator.row_ordinal = row;
  doc.locator.row_uuid = std::move(row_uuid);
  doc.locator.version_uuid = std::move(version_uuid);
  doc.normalized_terms = std::move(terms);
  doc.exact_source_recheck_evidence_ref =
      "exact_source_recheck_row_" + std::to_string(row);
  return doc;
}

std::vector<idx::TextInvertedDocumentInput> BaseDocs() {
  return {
      Doc(10,
          "aaaaaaaa-aaaa-7aaa-8aaa-aaaaaaaaaa10",
          "bbbbbbbb-bbbb-7bbb-8bbb-bbbbbbbbbb10",
          {"alpha", "beta", "gamma"}),
      Doc(20,
          "aaaaaaaa-aaaa-7aaa-8aaa-aaaaaaaaaa20",
          "bbbbbbbb-bbbb-7bbb-8bbb-bbbbbbbbbb20",
          {"alpha", "gamma", "delta"}),
      Doc(30,
          "aaaaaaaa-aaaa-7aaa-8aaa-aaaaaaaaaa30",
          "bbbbbbbb-bbbb-7bbb-8bbb-bbbbbbbbbb30",
          {"beta", "gamma", "alpha"}),
      Doc(40,
          "aaaaaaaa-aaaa-7aaa-8aaa-aaaaaaaaaa40",
          "bbbbbbbb-bbbb-7bbb-8bbb-bbbbbbbbbb40",
          {"alpha", "beta", "gamma"}),
      Doc(50,
          "aaaaaaaa-aaaa-7aaa-8aaa-aaaaaaaaaa50",
          "bbbbbbbb-bbbb-7bbb-8bbb-bbbbbbbbbb50",
          {"alpha", "omega"}),
  };
}

std::vector<idx::TextInvertedDocumentInput> SecondDocs() {
  return {
      Doc(60,
          "aaaaaaaa-aaaa-7aaa-8aaa-aaaaaaaaaa60",
          "bbbbbbbb-bbbb-7bbb-8bbb-bbbbbbbbbb60",
          {"alpha", "sigma"}),
      Doc(70,
          "aaaaaaaa-aaaa-7aaa-8aaa-aaaaaaaaaa70",
          "bbbbbbbb-bbbb-7bbb-8bbb-bbbbbbbbbb70",
          {"beta", "sigma"}),
  };
}

std::vector<std::uint64_t> Rows(
    const std::vector<idx::TextInvertedRowLocator>& locators) {
  std::vector<std::uint64_t> rows;
  for (const auto& locator : locators) {
    rows.push_back(locator.row_ordinal);
  }
  return rows;
}

std::vector<platform::byte> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(static_cast<bool>(in), "could not open persisted segment file");
  return {std::istreambuf_iterator<char>(in),
          std::istreambuf_iterator<char>()};
}

void WriteFile(const std::filesystem::path& path,
               const std::vector<platform::byte>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.flush();
  Require(static_cast<bool>(out), "could not write persisted segment file");
}

idx::TextInvertedSegment OpenSegment(
    const idx::TextInvertedSegmentSerializeResult& serialized) {
  idx::TextInvertedSegmentOpenRequest open;
  open.bytes = serialized.bytes;
  open.expected_relation_uuid_present = true;
  open.expected_relation_uuid = "11111111-1111-7111-8111-111111111111";
  open.expected_index_uuid_present = true;
  open.expected_index_uuid = "22222222-2222-7222-8222-222222222222";
  open.expected_segment_uuid_present = true;
  open.expected_segment_uuid = "33333333-3333-7333-8333-333333333333";
  open.expected_base_generation_present = true;
  open.expected_base_generation = 7;
  open.expected_segment_generation_present = true;
  open.expected_segment_generation = 11;
  open.expected_analyzer_epoch_present = true;
  open.expected_analyzer_epoch = 13;
  open.expected_resource_epoch_present = true;
  open.expected_resource_epoch = 17;
  open.recheck_proof = Proof();
  const auto opened = idx::OpenTextInvertedSegmentArtifact(open);
  Require(opened.ok(), "clean text segment reopen failed");
  return opened.segment;
}

idx::TextInvertedSegment BuildBaseSegment() {
  auto buffer = idx::CreateTextInvertedMutableSegmentBuffer(BuildRequest(11, 101));
  for (const auto& doc : BaseDocs()) {
    const auto added = idx::AddTextInvertedDocument(&buffer, doc);
    Require(added.ok(), "document was not accepted into mutable buffer");
  }
  const auto sealed = idx::SealTextInvertedSegmentBuffer(&buffer);
  Require(sealed.ok(), "mutable buffer did not seal into immutable segment");
  Require(buffer.sealed, "buffer did not record sealed state");
  Require(sealed.segment.term_dictionary_present,
          "term dictionary metadata missing");
  Require(sealed.segment.skip_metadata_present, "skip metadata missing");
  Require(sealed.segment.positions_present, "positions metadata missing");
  Require(sealed.segment.document_length_norms_present,
          "document length/norm metadata missing");
  Require(Contains(sealed.segment.evidence,
                   "posting_blocks_varint_delta_coded=true"),
          "posting compression evidence missing");
  Require(Contains(sealed.segment.evidence,
                   "visibility_security_finality_authority=false"),
          "authority non-claim evidence missing");
  return sealed.segment;
}

void VerifyBuildReopenAndPersistence(const idx::TextInvertedSegment& segment) {
  const auto serialized = idx::SerializeTextInvertedSegmentArtifact(segment);
  Require(serialized.ok(), "text segment serialization failed");
  Require(serialized.checksum != 0, "checksum missing");
  const auto serialized_again = idx::SerializeTextInvertedSegmentArtifact(segment);
  Require(serialized_again.ok() && serialized_again.bytes == serialized.bytes,
          "text segment serialization is not deterministic");

  const auto path =
      std::filesystem::temp_directory_path() /
      "scratchbird_text_inverted_segment_engine_gate.sbtinv";
  WriteFile(path, serialized.bytes);
  const auto persisted = ReadFile(path);
  std::filesystem::remove(path);
  Require(persisted == serialized.bytes,
          "persisted segment file changed byte representation");

  const auto reopened = OpenSegment(serialized);
  const auto reserialized = idx::SerializeTextInvertedSegmentArtifact(reopened);
  Require(reserialized.ok() && reserialized.bytes == serialized.bytes,
          "reopened text segment did not serialize equivalently");
}

void VerifyQueryProbes(const idx::TextInvertedSegment& segment) {
  idx::TextInvertedQueryRequest term;
  term.segment = segment;
  term.kind = idx::TextInvertedQueryKind::term;
  term.terms = {"alpha"};
  term.recheck_proof = Proof();
  const auto term_result = idx::ProbeTextInvertedSegment(term);
  Require(term_result.ok(), "term probe failed");
  Require(Rows(term_result.candidates) ==
              std::vector<std::uint64_t>({10, 20, 30, 40, 50}),
          "term probe returned wrong candidate rows");
  Require(term_result.exact_source_recheck_required &&
              term_result.mga_recheck_required &&
              term_result.security_recheck_required &&
              !term_result.descriptor_store_scan &&
              !term_result.behavior_store_scan,
          "term probe did not preserve exact/MGA/security recheck contract");

  idx::TextInvertedQueryRequest all_terms = term;
  all_terms.kind = idx::TextInvertedQueryKind::all_terms;
  all_terms.terms = {"beta", "gamma"};
  const auto all_result = idx::ProbeTextInvertedSegment(all_terms);
  Require(all_result.ok(), "all-terms probe failed");
  Require(Rows(all_result.candidates) ==
              std::vector<std::uint64_t>({10, 30, 40}),
          "all-terms intersection returned wrong candidate rows");

  idx::TextInvertedQueryRequest phrase = term;
  phrase.kind = idx::TextInvertedQueryKind::phrase;
  phrase.terms = {"alpha", "beta"};
  const auto phrase_result = idx::ProbeTextInvertedSegment(phrase);
  Require(phrase_result.ok(), "phrase probe failed");
  Require(Rows(phrase_result.candidates) ==
              std::vector<std::uint64_t>({10, 40}),
          "phrase positions returned wrong candidate rows");

  idx::TextInvertedQueryRequest seek = term;
  seek.lower_bound_row_ordinal = 35;
  const auto seek_result = idx::ProbeTextInvertedSegment(seek);
  Require(seek_result.ok(), "skip-block seek probe failed");
  Require(Rows(seek_result.candidates) ==
              std::vector<std::uint64_t>({40, 50}),
          "skip-block seek returned wrong candidate rows");
  Require(seek_result.skipped_block_count >= 1,
          "skip-block seek did not skip an earlier block");
}

void VerifyMergeScheduling(const idx::TextInvertedSegment& first) {
  const auto second_build =
      idx::BuildTextInvertedSegmentFromDocuments(BuildRequest(12, 102),
                                                 SecondDocs());
  Require(second_build.ok(), "second segment build failed");

  idx::TextInvertedMergeScheduleRequest request;
  request.recheck_proof = Proof();
  request.minimum_segment_count = 2;
  request.candidates.push_back(idx::TextInvertedMergeCandidateFromSegment(first));
  request.candidates.push_back(
      idx::TextInvertedMergeCandidateFromSegment(second_build.segment));
  const auto selected = idx::SelectTextInvertedSegmentsForMerge(request);
  Require(selected.ok(), "eligible sealed segments were not selected for merge");
  Require(selected.selected.size() == 2,
          "merge scheduler selected wrong segment count");
  Require(!selected.merge_metadata_finality_authority &&
              selected.old_segments_remain_searchable_until_engine_publish,
          "merge metadata claimed finality authority");

  request.candidates[0].provider_finality_authority_claimed = true;
  const auto refused = idx::SelectTextInvertedSegmentsForMerge(request);
  Require(!refused.ok() && refused.fail_closed,
          "merge scheduler accepted provider authority claim");
}

void VerifyFailClosed(const idx::TextInvertedSegment& segment) {
  const auto serialized = idx::SerializeTextInvertedSegmentArtifact(segment);
  Require(serialized.ok(), "serialization failed before fail-closed checks");

  auto corrupt = serialized.bytes;
  corrupt.back() ^= 0x01;
  idx::TextInvertedSegmentOpenRequest corrupt_open;
  corrupt_open.bytes = corrupt;
  corrupt_open.recheck_proof = Proof();
  const auto corrupt_result = idx::OpenTextInvertedSegmentArtifact(corrupt_open);
  Require(corrupt_result.open_class ==
              idx::TextInvertedSegmentOpenClass::bad_checksum &&
              corrupt_result.fail_closed,
          "corrupt checksum did not fail closed");

  idx::TextInvertedSegmentOpenRequest stale = corrupt_open;
  stale.bytes = serialized.bytes;
  stale.expected_segment_generation_present = true;
  stale.expected_segment_generation = 999;
  const auto stale_result = idx::OpenTextInvertedSegmentArtifact(stale);
  Require(stale_result.open_class ==
              idx::TextInvertedSegmentOpenClass::stale_generation &&
              stale_result.fail_closed,
          "stale generation did not fail closed");

  idx::TextInvertedSegmentOpenRequest identity = corrupt_open;
  identity.bytes = serialized.bytes;
  identity.expected_segment_uuid_present = true;
  identity.expected_segment_uuid = "99999999-9999-7999-8999-999999999999";
  const auto identity_result = idx::OpenTextInvertedSegmentArtifact(identity);
  Require(identity_result.open_class ==
              idx::TextInvertedSegmentOpenClass::identity_mismatch &&
              identity_result.fail_closed,
          "identity mismatch did not fail closed");

  idx::TextInvertedSegmentOpenRequest epoch = corrupt_open;
  epoch.bytes = serialized.bytes;
  epoch.expected_analyzer_epoch_present = true;
  epoch.expected_analyzer_epoch = 999;
  const auto epoch_result = idx::OpenTextInvertedSegmentArtifact(epoch);
  Require(epoch_result.open_class ==
              idx::TextInvertedSegmentOpenClass::
                  unsafe_analyzer_or_resource_epoch &&
              epoch_result.fail_closed,
          "unsafe analyzer epoch did not fail closed");

  idx::TextInvertedSegmentOpenRequest missing_proof;
  missing_proof.bytes = serialized.bytes;
  const auto missing_result =
      idx::OpenTextInvertedSegmentArtifact(missing_proof);
  Require(missing_result.open_class ==
              idx::TextInvertedSegmentOpenClass::
                  missing_exact_recheck_proof &&
              missing_result.fail_closed,
          "missing exact recheck proof did not fail closed");

  auto authority_segment = segment;
  authority_segment.provider_finality_authority_claimed = true;
  const auto authority_serialized =
      idx::SerializeTextInvertedSegmentArtifact(authority_segment);
  Require(!authority_serialized.ok(),
          "authority-claiming segment serialized successfully");

  auto bad_dictionary_stats = segment;
  ++bad_dictionary_stats.dictionary.front().document_frequency;
  const auto bad_dictionary_serialized =
      idx::SerializeTextInvertedSegmentArtifact(bad_dictionary_stats);
  Require(!bad_dictionary_serialized.ok(),
          "dictionary/posting statistic mismatch serialized successfully");

  idx::TextInvertedQueryRequest unsafe_query;
  unsafe_query.segment = segment;
  unsafe_query.kind = idx::TextInvertedQueryKind::term;
  unsafe_query.terms = {"alpha"};
  unsafe_query.recheck_proof = Proof();
  unsafe_query.analyzer_epoch_current = false;
  const auto unsafe_query_result = idx::ProbeTextInvertedSegment(unsafe_query);
  Require(!unsafe_query_result.ok(),
          "unsafe analyzer epoch query did not fail closed");

  idx::TextInvertedQueryRequest missing_query = unsafe_query;
  missing_query.analyzer_epoch_current = true;
  missing_query.recheck_proof = {};
  const auto missing_query_result = idx::ProbeTextInvertedSegment(missing_query);
  Require(!missing_query_result.ok(),
          "missing recheck proof query did not fail closed");

  Require(!segment.visibility_authority_claimed &&
              !segment.security_authority_claimed &&
              !segment.transaction_finality_authority_claimed &&
              !segment.parser_finality_authority_claimed &&
              !segment.reference_finality_authority_claimed &&
              !segment.provider_finality_authority_claimed &&
              !segment.write_ahead_log_finality_authority_claimed,
          "segment carried forbidden authority claim");
}

}  // namespace

int main() {
  const auto segment = BuildBaseSegment();
  VerifyBuildReopenAndPersistence(segment);
  VerifyQueryProbes(segment);
  VerifyMergeScheduling(segment);
  VerifyFailClosed(segment);
  return EXIT_SUCCESS;
}
