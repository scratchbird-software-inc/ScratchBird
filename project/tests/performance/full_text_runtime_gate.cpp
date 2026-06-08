// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "full_text_runtime.hpp"
#include "text_inverted_segment.hpp"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace idx = scratchbird::core::index;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "full_text_runtime_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 std::string_view needle) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) {
    return item.find(needle) != std::string::npos;
  });
}

void RequireNoExecution_PlanEvidence(const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    Require(item.find("docs" "/execution-plans") == std::string::npos &&
                item.find("execution_plan") == std::string::npos &&
                item.find("IRC-") == std::string::npos,
            "runtime evidence leaked execution_plan identifier");
  }
}

std::string UuidWithSuffix(std::string prefix, std::uint64_t suffix) {
  std::ostringstream out;
  out << prefix << std::setw(12) << std::setfill('0') << suffix;
  return out.str();
}

idx::TextInvertedExactRecheckProof CandidateProof() {
  idx::TextInvertedExactRecheckProof proof;
  proof.proof_supplied = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.source_recheck_required = true;
  proof.evidence_ref = "full_text_source_mga_security_recheck_contract";
  return proof;
}

idx::FullTextRuntimeExactRerankProof RerankProof() {
  idx::FullTextRuntimeExactRerankProof proof;
  proof.proof_supplied = true;
  proof.exact_source_rows_available = true;
  proof.mga_visibility_recheck_available = true;
  proof.security_authorization_recheck_available = true;
  proof.score_order_recheck_available = true;
  proof.evidence_ref = "full_text_exact_score_order_recheck_contract";
  return proof;
}

idx::TextInvertedSegmentBuildRequest BuildRequest() {
  idx::TextInvertedSegmentBuildRequest request;
  request.relation_uuid = "11111111-1111-7111-8111-111111111111";
  request.index_uuid = "22222222-2222-7222-8222-222222222222";
  request.segment_uuid = "33333333-3333-7333-8333-333333333333";
  request.base_generation = 7;
  request.segment_generation = 11;
  request.analyzer_epoch = 13;
  request.resource_epoch = 17;
  request.block_posting_target = 2;
  request.merge_tier = 0;
  request.sealed_sequence = 101;
  request.recheck_proof = CandidateProof();
  return request;
}

idx::TextInvertedDocumentInput Doc(std::uint64_t row,
                                   std::vector<std::string> terms) {
  idx::TextInvertedDocumentInput doc;
  doc.locator.row_ordinal = row;
  doc.locator.row_uuid =
      UuidWithSuffix("aaaaaaaa-aaaa-7aaa-8aaa-", row);
  doc.locator.version_uuid =
      UuidWithSuffix("bbbbbbbb-bbbb-7bbb-8bbb-", row);
  doc.normalized_terms = std::move(terms);
  doc.exact_source_recheck_evidence_ref =
      "base_row_recheck_evidence_" + std::to_string(row);
  return doc;
}

idx::TextInvertedSegment BuildSegment() {
  std::vector<idx::TextInvertedDocumentInput> docs = {
      Doc(10,
          {"search", "search", "search", "search", "search", "quick",
           "brown", "fox"}),
      Doc(20,
          {"search", "search", "search", "quick", "fox", "brown"}),
      Doc(30, {"search", "brief"}),
      Doc(40,
          {"search", "padding", "padding", "padding", "quick", "brown",
           "fox", "tail", "tail", "tail"}),
      Doc(50,
          {"search", "padding", "padding", "padding", "padding", "padding",
           "padding", "padding", "padding", "padding"}),
      Doc(60, {"quick", "brown", "fox", "topic"}),
  };
  const auto built =
      idx::BuildTextInvertedSegmentFromDocuments(BuildRequest(), docs);
  Require(built.ok(), "text inverted segment fixture build failed");
  return built.segment;
}

idx::FullTextRuntimeRequest RuntimeRequest(
    const idx::TextInvertedSegment& segment,
    idx::FullTextRuntimeQueryKind kind,
    std::vector<std::string> terms) {
  idx::FullTextRuntimeRequest request;
  request.segments.push_back(segment);
  request.kind = kind;
  request.terms = std::move(terms);
  request.candidate_recheck_proof = CandidateProof();
  request.exact_rerank_proof = RerankProof();
  return request;
}

std::vector<std::uint64_t> Rows(const idx::FullTextRuntimeResult& result) {
  std::vector<std::uint64_t> rows;
  for (const auto& candidate : result.candidates) {
    rows.push_back(candidate.locator.row_ordinal);
  }
  return rows;
}

double ScoreFor(const idx::FullTextRuntimeResult& result, std::uint64_t row) {
  for (const auto& candidate : result.candidates) {
    if (candidate.locator.row_ordinal == row) {
      return candidate.score;
    }
  }
  return -1.0;
}

bool ContainsRow(const idx::FullTextRuntimeResult& result,
                 std::uint64_t row) {
  return ScoreFor(result, row) >= 0.0;
}

void RequireCommonRuntimeEvidence(const idx::FullTextRuntimeResult& result) {
  Require(result.ok(), "full-text runtime unexpectedly failed");
  Require(result.segment_engine_consumed,
          "runtime did not report segment engine consumption");
  Require(result.bm25_payload_consumed,
          "runtime did not report BM25 payload consumption");
  Require(result.exact_rerank_applied,
          "runtime did not apply exact rerank phase");
  Require(result.exact_source_recheck_required &&
              result.mga_recheck_required &&
              result.security_recheck_required &&
              result.candidate_rows_only &&
              !result.final_rows_authorized &&
              !result.descriptor_store_scan &&
              !result.behavior_store_scan,
          "runtime violated candidate-only recheck contract");
  for (const auto& candidate : result.candidates) {
    Require(!candidate.final_row_admitted,
            "full-text runtime admitted a final row");
    Require(candidate.exact_source_recheck_required &&
                candidate.mga_recheck_required &&
                candidate.security_recheck_required &&
                !candidate.source_recheck_evidence_ref.empty(),
            "candidate omitted exact source/MGA/security recheck evidence");
  }
  Require(HasEvidence(result.evidence, idx::kFullTextRuntimeSearchKey),
          "runtime search-key evidence missing");
  Require(HasEvidence(result.evidence,
                      "full_text_runtime.text_inverted_segment_posting_blocks_consumed=true"),
          "posting block consumption evidence missing");
  Require(HasEvidence(result.evidence,
                      "full_text_runtime.bm25.document_length_used=true"),
          "BM25 document length evidence missing");
  Require(HasEvidence(result.evidence,
                      "full_text_runtime.bm25.norm_payload_used=true"),
          "BM25 norm payload evidence missing");
  Require(HasEvidence(result.evidence,
                      "full_text_runtime.bm25.term_document_statistics_used=true"),
          "BM25 term statistics evidence missing");
  Require(HasEvidence(result.evidence,
                      "full_text_runtime.exact_rerank_proof_required=true"),
          "exact rerank proof evidence missing");
  Require(HasEvidence(result.evidence,
                      "full_text_runtime.index_finality_authority=false"),
          "index non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "full_text_runtime.write_ahead_log_authority=false"),
          "write-ahead-log non-authority evidence missing");
  RequireNoExecution_PlanEvidence(result.evidence);
}

void VerifyBm25Ranking(const idx::TextInvertedSegment& segment) {
  auto request =
      RuntimeRequest(segment, idx::FullTextRuntimeQueryKind::term, {"search"});
  const auto result = idx::ExecuteFullTextRuntime(request);
  RequireCommonRuntimeEvidence(result);
  Require(Rows(result) ==
              std::vector<std::uint64_t>({10, 20, 30, 40, 50}),
          "BM25 ranking/order changed");
  Require(ScoreFor(result, 10) > ScoreFor(result, 20),
          "term frequency did not affect BM25 score");
  Require(ScoreFor(result, 30) > ScoreFor(result, 40),
          "document length/norm metadata did not affect BM25 score");
  Require(result.exact_rerank_count == result.candidates.size(),
          "exact rerank did not cover every returned candidate");
}

void VerifyPhraseRuntime(const idx::TextInvertedSegment& segment) {
  auto request = RuntimeRequest(segment,
                                idx::FullTextRuntimeQueryKind::phrase,
                                {"quick", "brown", "fox"});
  const auto result = idx::ExecuteFullTextRuntime(request);
  RequireCommonRuntimeEvidence(result);
  Require(result.phrase_positions_consumed,
          "phrase query did not consume positions");
  Require(result.phrase_position_check_count > 0,
          "phrase query did not perform boundary checks");
  Require(ContainsRow(result, 10), "phrase result missed row 10");
  Require(!ContainsRow(result, 20),
          "phrase result accepted out-of-order positions");
  Require(ContainsRow(result, 40), "phrase result missed row 40");
  Require(ContainsRow(result, 60), "phrase result missed row 60");
  for (std::size_t index = 1; index < result.candidates.size(); ++index) {
    Require(result.candidates[index - 1].exact_rerank_score >=
                result.candidates[index].exact_rerank_score,
            "phrase rerank order was not deterministic by score");
  }
}

void VerifyAllTermsAndTopKPruning(const idx::TextInvertedSegment& segment) {
  auto all_terms = RuntimeRequest(segment,
                                  idx::FullTextRuntimeQueryKind::all_terms,
                                  {"quick", "brown", "fox"});
  const auto all_result = idx::ExecuteFullTextRuntime(all_terms);
  RequireCommonRuntimeEvidence(all_result);
  Require(ContainsRow(all_result, 20),
          "all-terms runtime incorrectly applied phrase ordering");

  auto topk =
      RuntimeRequest(segment, idx::FullTextRuntimeQueryKind::term, {"search"});
  topk.top_k = 1;
  const auto topk_result = idx::ExecuteFullTextRuntime(topk);
  RequireCommonRuntimeEvidence(topk_result);
  Require(topk_result.candidates.size() == 1 &&
              topk_result.candidates.front().locator.row_ordinal == 10,
          "top-k runtime returned the wrong winner");
  Require(topk_result.block_max_pruning_used &&
              topk_result.top_k_early_termination &&
              topk_result.impact_ordered_postings_used,
          "block-max/impact-ordered top-k pruning was not used");
  Require(topk_result.skipped_block_count >= 1 &&
              topk_result.block_max_skipped_count >= 1 &&
              topk_result.scanned_block_count < 3,
          "top-k runtime did not skip lower-impact posting blocks");
  Require(topk_result.candidate_count_before_top_k >= 1,
          "top-k runtime did not expose candidate count before pruning");
}

void VerifyFailClosed(const idx::TextInvertedSegment& segment) {
  auto missing_rerank =
      RuntimeRequest(segment, idx::FullTextRuntimeQueryKind::term, {"search"});
  missing_rerank.exact_rerank_proof = {};
  const auto missing_result = idx::ExecuteFullTextRuntime(missing_rerank);
  Require(!missing_result.ok() && missing_result.fail_closed &&
              missing_result.diagnostic.diagnostic_code ==
                  "SB_FULL_TEXT_RUNTIME.EXACT_RERANK_PROOF_MISSING",
          "missing exact rerank proof did not fail closed");

  auto stale =
      RuntimeRequest(segment, idx::FullTextRuntimeQueryKind::term, {"search"});
  stale.analyzer_epoch_current = false;
  const auto stale_result = idx::ExecuteFullTextRuntime(stale);
  Require(!stale_result.ok() && stale_result.fail_closed &&
              stale_result.diagnostic.diagnostic_code ==
                  "SB_FULL_TEXT_RUNTIME.UNSAFE_EPOCH",
          "unsafe analyzer epoch did not fail closed");

  auto authority =
      RuntimeRequest(segment, idx::FullTextRuntimeQueryKind::term, {"search"});
  authority.exact_rerank_proof.provider_finality_authority_claimed = true;
  const auto authority_result = idx::ExecuteFullTextRuntime(authority);
  Require(!authority_result.ok() && authority_result.fail_closed,
          "provider authority claim in rerank proof was accepted");

  auto segment_authority = segment;
  segment_authority.provider_finality_authority_claimed = true;
  auto segment_request = RuntimeRequest(segment_authority,
                                        idx::FullTextRuntimeQueryKind::term,
                                        {"search"});
  const auto segment_authority_result =
      idx::ExecuteFullTextRuntime(segment_request);
  Require(!segment_authority_result.ok() &&
              segment_authority_result.fail_closed,
          "authority-claiming segment was accepted");

  auto uncovered_block = segment;
  uncovered_block.posting_blocks.push_back(uncovered_block.posting_blocks.front());
  uncovered_block.posting_blocks.back().block_ordinal =
      static_cast<std::uint32_t>(uncovered_block.posting_blocks.size() - 1);
  auto uncovered_request = RuntimeRequest(uncovered_block,
                                          idx::FullTextRuntimeQueryKind::term,
                                          {"search"});
  const auto uncovered_result = idx::ExecuteFullTextRuntime(uncovered_request);
  Require(!uncovered_result.ok() && uncovered_result.fail_closed &&
              uncovered_result.diagnostic.diagnostic_code ==
                  "SB_FULL_TEXT_RUNTIME.SEGMENT_REFUSED",
          "segment with uncovered posting block was accepted");

  auto single_phrase = RuntimeRequest(segment,
                                      idx::FullTextRuntimeQueryKind::phrase,
                                      {"search"});
  const auto single_phrase_result = idx::ExecuteFullTextRuntime(single_phrase);
  Require(!single_phrase_result.ok() && single_phrase_result.fail_closed &&
              single_phrase_result.diagnostic.diagnostic_code ==
                  "SB_FULL_TEXT_RUNTIME.QUERY_INVALID",
          "single-term phrase shape was accepted");

  auto single_all_terms = RuntimeRequest(segment,
                                         idx::FullTextRuntimeQueryKind::all_terms,
                                         {"search"});
  const auto single_all_terms_result =
      idx::ExecuteFullTextRuntime(single_all_terms);
  Require(!single_all_terms_result.ok() && single_all_terms_result.fail_closed &&
              single_all_terms_result.diagnostic.diagnostic_code ==
                  "SB_FULL_TEXT_RUNTIME.QUERY_INVALID",
          "single-term all-terms shape was accepted");
}

}  // namespace

int main() {
  const auto segment = BuildSegment();
  VerifyBm25Ranking(segment);
  VerifyPhraseRuntime(segment);
  VerifyAllTermsAndTopKPruning(segment);
  VerifyFailClosed(segment);
  std::cout << "full_text_runtime_gate=passed\n";
  return EXIT_SUCCESS;
}
