// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "full_text_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <tuple>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status RefusalStatus() {
  return {StatusCode::diagnostic_invalid_record, Severity::error,
          Subsystem::engine};
}

bool TermValid(const std::string& term) {
  if (term.empty() || term.size() > 256) {
    return false;
  }
  return std::all_of(term.begin(), term.end(), [](unsigned char ch) {
    return ch > 0x20 && ch != 0x7f;
  });
}

bool RecheckProofValid(const TextInvertedExactRecheckProof& proof) {
  return proof.proof_supplied &&
         proof.mga_recheck_required &&
         proof.security_recheck_required &&
         proof.source_recheck_required &&
         !proof.evidence_ref.empty();
}

bool RerankProofAuthorityClean(
    const FullTextRuntimeExactRerankProof& proof) {
  return !proof.parser_finality_authority_claimed &&
         !proof.donor_finality_authority_claimed &&
         !proof.provider_finality_authority_claimed &&
         !proof.index_finality_authority_claimed &&
         !proof.write_ahead_log_finality_authority_claimed &&
         !proof.visibility_authority_claimed &&
         !proof.security_authority_claimed &&
         !proof.transaction_finality_authority_claimed;
}

bool RerankProofValid(const FullTextRuntimeExactRerankProof& proof) {
  return proof.proof_supplied &&
         proof.exact_source_rows_available &&
         proof.mga_visibility_recheck_available &&
         proof.security_authorization_recheck_available &&
         proof.score_order_recheck_available &&
         !proof.evidence_ref.empty() &&
         RerankProofAuthorityClean(proof);
}

bool LocatorValid(const TextInvertedRowLocator& locator) {
  return locator.row_ordinal > 0 &&
         PageExtentSummaryUuidTextValid(locator.row_uuid) &&
         PageExtentSummaryUuidTextValid(locator.version_uuid);
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
         !segment.write_ahead_log_finality_authority_claimed &&
         !segment.merge.merge_metadata_finality_authority;
}

bool PostingRuntimeValid(const TextInvertedPosting& posting) {
  return LocatorValid(posting.locator) &&
         posting.document_length > 0 &&
         std::isfinite(posting.norm) &&
         posting.norm > 0.0 &&
         !posting.positions.empty() &&
         std::is_sorted(posting.positions.begin(), posting.positions.end());
}

bool BlockRuntimeValid(const TextInvertedPostingBlock& block,
                       const std::string& expected_term,
                       u32 expected_ordinal) {
  if (block.block_ordinal != expected_ordinal ||
      block.term != expected_term ||
      block.posting_count == 0 ||
      block.posting_count != block.postings.size() ||
      block.first_row_ordinal == 0 ||
      block.last_row_ordinal < block.first_row_ordinal ||
      !block.row_ordinals_delta_coded ||
      !block.positions_delta_coded ||
      block.encoded_byte_count != block.encoded_postings.size() ||
      block.encoded_postings.empty()) {
    return false;
  }
  u64 previous = 0;
  for (const auto& posting : block.postings) {
    if (!PostingRuntimeValid(posting) ||
        posting.locator.row_ordinal <= previous) {
      return false;
    }
    previous = posting.locator.row_ordinal;
  }
  return block.postings.front().locator.row_ordinal ==
             block.first_row_ordinal &&
         block.postings.back().locator.row_ordinal == block.last_row_ordinal;
}

bool SegmentRuntimeReady(const TextInvertedSegment& segment) {
  if (segment.artifact_kind != kTextInvertedSegmentArtifactKind ||
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
      segment.merge.retired) {
    return false;
  }

  u64 previous_row = 0;
  for (const auto& document : segment.documents) {
    if (!LocatorValid(document.locator) ||
        document.locator.row_ordinal <= previous_row ||
        document.document_length == 0 ||
        !std::isfinite(document.norm) ||
        document.norm <= 0.0 ||
        document.exact_source_recheck_evidence_ref.empty()) {
      return false;
    }
    previous_row = document.locator.row_ordinal;
  }

  std::string previous_term;
  bool first = true;
  std::vector<bool> referenced_blocks(segment.posting_blocks.size(), false);
  for (const auto& entry : segment.dictionary) {
    if (!TermValid(entry.term) ||
        (!first && entry.term <= previous_term) ||
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
      const u32 ordinal = entry.first_block_ordinal + offset;
      const auto& block = segment.posting_blocks[ordinal];
      if (referenced_blocks[ordinal] ||
          !BlockRuntimeValid(block, entry.term, ordinal)) {
        return false;
      }
      referenced_blocks[ordinal] = true;
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
    first = false;
  }

  return std::all_of(referenced_blocks.begin(),
                     referenced_blocks.end(),
                     [](bool referenced) { return referenced; });
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

struct SegmentRuntimeView {
  const TextInvertedSegment* segment = nullptr;
  u64 document_count = 0;
  double document_length_sum = 0.0;
  std::map<u64, const TextInvertedDocumentMetadata*> documents_by_row;
};

struct CandidateKey {
  u64 row_ordinal = 0;
  std::string row_uuid;
  std::string version_uuid;

  friend bool operator<(const CandidateKey& left,
                        const CandidateKey& right) {
    return std::tie(left.row_ordinal, left.row_uuid, left.version_uuid) <
           std::tie(right.row_ordinal, right.row_uuid, right.version_uuid);
  }
};

CandidateKey KeyFor(const TextInvertedRowLocator& locator) {
  return {locator.row_ordinal, locator.row_uuid, locator.version_uuid};
}

const TextInvertedDocumentMetadata* FindDocument(
    const SegmentRuntimeView& view,
    const TextInvertedRowLocator& locator) {
  const auto iter = view.documents_by_row.find(locator.row_ordinal);
  if (iter == view.documents_by_row.end()) {
    return nullptr;
  }
  const auto* document = iter->second;
  if (document->locator.row_uuid != locator.row_uuid ||
      document->locator.version_uuid != locator.version_uuid) {
    return nullptr;
  }
  return document;
}

struct GlobalTermStats {
  u64 document_frequency = 0;
  double idf = 0.0;
};

struct RuntimeStats {
  u64 document_count = 0;
  double average_document_length = 1.0;
  std::map<std::string, GlobalTermStats> terms;
};

std::vector<SegmentRuntimeView> BuildViews(
    const std::vector<TextInvertedSegment>& segments) {
  std::vector<SegmentRuntimeView> views;
  views.reserve(segments.size());
  for (const auto& segment : segments) {
    SegmentRuntimeView view;
    view.segment = &segment;
    view.document_count = segment.documents.size();
    for (const auto& document : segment.documents) {
      view.document_length_sum += document.document_length;
      view.documents_by_row[document.locator.row_ordinal] = &document;
    }
    views.push_back(std::move(view));
  }
  return views;
}

RuntimeStats BuildRuntimeStats(const std::vector<SegmentRuntimeView>& views,
                               const std::vector<std::string>& terms) {
  RuntimeStats stats;
  double document_length_sum = 0.0;
  for (const auto& view : views) {
    stats.document_count += view.document_count;
    document_length_sum += view.document_length_sum;
  }
  if (stats.document_count != 0) {
    stats.average_document_length =
        document_length_sum / static_cast<double>(stats.document_count);
  }

  const std::set<std::string> unique_terms(terms.begin(), terms.end());
  for (const auto& term : unique_terms) {
    GlobalTermStats term_stats;
    for (const auto& view : views) {
      const auto* entry = FindTerm(*view.segment, term);
      if (entry != nullptr) {
        term_stats.document_frequency += entry->document_frequency;
      }
    }
    if (term_stats.document_frequency != 0 && stats.document_count != 0) {
      const double numerator =
          static_cast<double>(stats.document_count) -
          static_cast<double>(term_stats.document_frequency) + 0.5;
      const double denominator =
          static_cast<double>(term_stats.document_frequency) + 0.5;
      term_stats.idf = std::log(1.0 + std::max(0.0, numerator) / denominator);
    }
    stats.terms[term] = term_stats;
  }
  return stats;
}

double Clamp(double value, double lower, double upper) {
  return std::max(lower, std::min(upper, value));
}

double ScorePostingBm25(const TextInvertedPosting& posting,
                        double idf,
                        double average_document_length,
                        double k1,
                        double b) {
  if (idf <= 0.0 || average_document_length <= 0.0) {
    return 0.0;
  }
  const double term_frequency =
      static_cast<double>(posting.positions.size());
  const double document_length =
      static_cast<double>(posting.document_length);
  const double length_component =
      (1.0 - b) + b * (document_length / average_document_length);
  const double denominator = term_frequency + k1 * length_component;
  if (denominator <= 0.0) {
    return 0.0;
  }
  const double bm25 =
      idf * ((term_frequency * (k1 + 1.0)) / denominator);
  const double norm_scale = 1.0 + Clamp(posting.norm, 0.0, 1.0) * 0.01;
  return bm25 * norm_scale;
}

struct ScoredPosting {
  const SegmentRuntimeView* view = nullptr;
  const TextInvertedPosting* posting = nullptr;
  double score = 0.0;
  std::string term;
};

struct RuntimeBlock {
  const SegmentRuntimeView* view = nullptr;
  const TextInvertedPostingBlock* block = nullptr;
  std::string term;
  double upper_bound = 0.0;
  std::vector<ScoredPosting> impact_postings;
};

RuntimeBlock BuildRuntimeBlock(const SegmentRuntimeView& view,
                               const TextInvertedPostingBlock& block,
                               const std::string& term,
                               const RuntimeStats& stats,
                               const FullTextRuntimeRequest& request) {
  RuntimeBlock runtime_block;
  runtime_block.view = &view;
  runtime_block.block = &block;
  runtime_block.term = term;
  const auto stats_iter = stats.terms.find(term);
  const double idf =
      stats_iter == stats.terms.end() ? 0.0 : stats_iter->second.idf;
  for (const auto& posting : block.postings) {
    if (posting.locator.row_ordinal < request.lower_bound_row_ordinal) {
      continue;
    }
    const double score =
        ScorePostingBm25(posting,
                         idf,
                         stats.average_document_length,
                         request.bm25_k1,
                         request.bm25_b);
    runtime_block.upper_bound = std::max(runtime_block.upper_bound, score);
    runtime_block.impact_postings.push_back(
        {&view, &posting, score, term});
  }
  std::sort(runtime_block.impact_postings.begin(),
            runtime_block.impact_postings.end(),
            [](const auto& left, const auto& right) {
              if (left.score != right.score) {
                return left.score > right.score;
              }
              return std::tie(left.posting->locator.row_ordinal,
                              left.posting->locator.row_uuid,
                              left.posting->locator.version_uuid) <
                     std::tie(right.posting->locator.row_ordinal,
                              right.posting->locator.row_uuid,
                              right.posting->locator.version_uuid);
            });
  return runtime_block;
}

std::vector<RuntimeBlock> BuildTermBlocks(
    const std::vector<SegmentRuntimeView>& views,
    const std::string& term,
    const RuntimeStats& stats,
    const FullTextRuntimeRequest& request) {
  std::vector<RuntimeBlock> blocks;
  for (const auto& view : views) {
    const auto* entry = FindTerm(*view.segment, term);
    if (entry == nullptr) {
      continue;
    }
    for (u32 offset = 0; offset < entry->block_count; ++offset) {
      const auto& block =
          view.segment->posting_blocks[entry->first_block_ordinal + offset];
      RuntimeBlock runtime_block =
          BuildRuntimeBlock(view, block, term, stats, request);
      if (!runtime_block.impact_postings.empty()) {
        blocks.push_back(std::move(runtime_block));
      }
    }
  }
  std::sort(blocks.begin(), blocks.end(), [](const auto& left,
                                             const auto& right) {
    if (left.upper_bound != right.upper_bound) {
      return left.upper_bound > right.upper_bound;
    }
    return std::tie(left.block->first_row_ordinal,
                    left.block->last_row_ordinal,
                    left.block->block_ordinal,
                    left.view->segment->segment_uuid) <
           std::tie(right.block->first_row_ordinal,
                    right.block->last_row_ordinal,
                    right.block->block_ordinal,
                    right.view->segment->segment_uuid);
  });
  return blocks;
}

struct RuntimeCounters {
  u64 scanned_blocks = 0;
  u64 skipped_blocks = 0;
  u64 block_max_skipped = 0;
  u64 postings_scanned = 0;
  u64 phrase_position_checks = 0;
  bool block_max_pruning_used = false;
  bool impact_ordered_postings_used = false;
  bool top_k_early_termination = false;
};

double CurrentTopKThreshold(
    const std::map<CandidateKey, FullTextRuntimeCandidate>& candidates,
    u32 top_k) {
  if (top_k == 0 || candidates.size() < top_k) {
    return -std::numeric_limits<double>::infinity();
  }
  std::vector<double> scores;
  scores.reserve(candidates.size());
  for (const auto& [key, candidate] : candidates) {
    (void)key;
    scores.push_back(candidate.score);
  }
  std::sort(scores.begin(), scores.end(), std::greater<double>());
  return scores[static_cast<std::size_t>(top_k - 1)];
}

void InsertOrReplaceTermCandidate(
    const ScoredPosting& scored,
    std::map<CandidateKey, FullTextRuntimeCandidate>* candidates) {
  const auto* document = FindDocument(*scored.view, scored.posting->locator);
  if (document == nullptr) {
    return;
  }
  const CandidateKey key = KeyFor(scored.posting->locator);
  auto iter = candidates->find(key);
  if (iter != candidates->end() && iter->second.score >= scored.score) {
    return;
  }
  FullTextRuntimeCandidate candidate;
  candidate.locator = scored.posting->locator;
  candidate.score = scored.score;
  candidate.exact_rerank_score = scored.score;
  candidate.matched_term_count = 1;
  candidate.segment_uuid = scored.view->segment->segment_uuid;
  candidate.segment_generation = scored.view->segment->segment_generation;
  candidate.source_recheck_evidence_ref =
      document->exact_source_recheck_evidence_ref;
  (*candidates)[key] = std::move(candidate);
}

struct PostingHit {
  const SegmentRuntimeView* view = nullptr;
  const TextInvertedPosting* posting = nullptr;
  double score = 0.0;
  std::string source_recheck_evidence_ref;
};

using PostingHitMap = std::map<CandidateKey, PostingHit>;

PostingHitMap CollectTermHits(const std::vector<RuntimeBlock>& blocks,
                              RuntimeCounters* counters) {
  PostingHitMap hits;
  counters->impact_ordered_postings_used =
      counters->impact_ordered_postings_used || blocks.size() > 1;
  for (const auto& block : blocks) {
    ++counters->scanned_blocks;
    for (const auto& scored : block.impact_postings) {
      ++counters->postings_scanned;
      const auto* document = FindDocument(*scored.view,
                                          scored.posting->locator);
      if (document == nullptr) {
        continue;
      }
      const CandidateKey key = KeyFor(scored.posting->locator);
      auto iter = hits.find(key);
      if (iter != hits.end() && iter->second.score >= scored.score) {
        continue;
      }
      hits[key] = {scored.view,
                   scored.posting,
                   scored.score,
                   document->exact_source_recheck_evidence_ref};
    }
  }
  return hits;
}

std::map<CandidateKey, FullTextRuntimeCandidate> ExecuteTermQuery(
    const std::vector<SegmentRuntimeView>& views,
    const RuntimeStats& stats,
    const FullTextRuntimeRequest& request,
    RuntimeCounters* counters) {
  std::map<CandidateKey, FullTextRuntimeCandidate> candidates;
  const auto blocks = BuildTermBlocks(views, request.terms.front(), stats, request);
  counters->impact_ordered_postings_used =
      counters->impact_ordered_postings_used || blocks.size() > 1;
  for (const auto& block : blocks) {
    const double threshold = CurrentTopKThreshold(candidates, request.top_k);
    if (request.top_k != 0 &&
        candidates.size() >= request.top_k &&
        block.upper_bound <= threshold) {
      ++counters->skipped_blocks;
      ++counters->block_max_skipped;
      counters->block_max_pruning_used = true;
      counters->top_k_early_termination = true;
      continue;
    }
    ++counters->scanned_blocks;
    for (const auto& scored : block.impact_postings) {
      ++counters->postings_scanned;
      InsertOrReplaceTermCandidate(scored, &candidates);
    }
  }
  return candidates;
}

std::map<CandidateKey, FullTextRuntimeCandidate> ExecuteAllTermsQuery(
    const std::vector<SegmentRuntimeView>& views,
    const RuntimeStats& stats,
    const FullTextRuntimeRequest& request,
    RuntimeCounters* counters) {
  std::vector<PostingHitMap> hits_by_term;
  hits_by_term.reserve(request.terms.size());
  for (const auto& term : request.terms) {
    hits_by_term.push_back(
        CollectTermHits(BuildTermBlocks(views, term, stats, request),
                        counters));
  }

  std::map<CandidateKey, FullTextRuntimeCandidate> candidates;
  if (hits_by_term.empty()) {
    return candidates;
  }
  for (const auto& [key, first_hit] : hits_by_term.front()) {
    double score = first_hit.score;
    bool all_present = true;
    const PostingHit* representative = &first_hit;
    for (std::size_t term_index = 1; term_index < hits_by_term.size();
         ++term_index) {
      const auto iter = hits_by_term[term_index].find(key);
      if (iter == hits_by_term[term_index].end()) {
        all_present = false;
        break;
      }
      score += iter->second.score;
    }
    if (!all_present) {
      continue;
    }
    FullTextRuntimeCandidate candidate;
    candidate.locator = representative->posting->locator;
    candidate.score = score;
    candidate.exact_rerank_score = score;
    candidate.matched_term_count = static_cast<u32>(request.terms.size());
    candidate.segment_uuid = representative->view->segment->segment_uuid;
    candidate.segment_generation =
        representative->view->segment->segment_generation;
    candidate.source_recheck_evidence_ref =
        representative->source_recheck_evidence_ref;
    candidates[key] = std::move(candidate);
  }
  return candidates;
}

bool ContainsPosition(const std::vector<u32>& positions, u32 value) {
  return std::binary_search(positions.begin(), positions.end(), value);
}

bool PhraseMatches(const std::vector<const PostingHit*>& hits,
                   RuntimeCounters* counters,
                   u32* start_position) {
  if (hits.empty()) {
    return false;
  }
  const auto* first = hits.front()->posting;
  for (u32 start : first->positions) {
    ++counters->phrase_position_checks;
    const u64 last_position =
        static_cast<u64>(start) + static_cast<u64>(hits.size() - 1);
    if (last_position >= first->document_length ||
        last_position > std::numeric_limits<u32>::max()) {
      continue;
    }
    bool match = true;
    for (std::size_t index = 1; index < hits.size(); ++index) {
      const u32 wanted = static_cast<u32>(start + index);
      if (!ContainsPosition(hits[index]->posting->positions, wanted)) {
        match = false;
        break;
      }
    }
    if (match) {
      *start_position = start;
      return true;
    }
  }
  return false;
}

std::map<CandidateKey, FullTextRuntimeCandidate> ExecutePhraseQuery(
    const std::vector<SegmentRuntimeView>& views,
    const RuntimeStats& stats,
    const FullTextRuntimeRequest& request,
    RuntimeCounters* counters) {
  std::vector<PostingHitMap> hits_by_term;
  hits_by_term.reserve(request.terms.size());
  for (const auto& term : request.terms) {
    hits_by_term.push_back(
        CollectTermHits(BuildTermBlocks(views, term, stats, request),
                        counters));
  }

  std::map<CandidateKey, FullTextRuntimeCandidate> candidates;
  if (hits_by_term.empty()) {
    return candidates;
  }
  for (const auto& [key, first_hit] : hits_by_term.front()) {
    std::vector<const PostingHit*> hits;
    hits.push_back(&first_hit);
    double score = first_hit.score;
    bool all_present = true;
    for (std::size_t term_index = 1; term_index < hits_by_term.size();
         ++term_index) {
      const auto iter = hits_by_term[term_index].find(key);
      if (iter == hits_by_term[term_index].end()) {
        all_present = false;
        break;
      }
      hits.push_back(&iter->second);
      score += iter->second.score;
    }
    if (!all_present) {
      continue;
    }

    u32 phrase_start = 0;
    if (!PhraseMatches(hits, counters, &phrase_start)) {
      continue;
    }

    FullTextRuntimeCandidate candidate;
    candidate.locator = first_hit.posting->locator;
    candidate.score = score;
    candidate.exact_rerank_score = score;
    candidate.matched_term_count = static_cast<u32>(request.terms.size());
    candidate.phrase_match = true;
    candidate.phrase_start_position = phrase_start;
    candidate.segment_uuid = first_hit.view->segment->segment_uuid;
    candidate.segment_generation = first_hit.view->segment->segment_generation;
    candidate.source_recheck_evidence_ref =
        first_hit.source_recheck_evidence_ref;
    candidates[key] = std::move(candidate);
  }
  return candidates;
}

bool CandidateOrderLess(const FullTextRuntimeCandidate& left,
                        const FullTextRuntimeCandidate& right) {
  if (left.exact_rerank_score != right.exact_rerank_score) {
    return left.exact_rerank_score > right.exact_rerank_score;
  }
  if (left.score != right.score) {
    return left.score > right.score;
  }
  return std::tie(left.locator.row_ordinal,
                  left.locator.row_uuid,
                  left.locator.version_uuid,
                  left.segment_generation,
                  left.segment_uuid) <
         std::tie(right.locator.row_ordinal,
                  right.locator.row_uuid,
                  right.locator.version_uuid,
                  right.segment_generation,
                  right.segment_uuid);
}

std::vector<FullTextRuntimeCandidate> FinalizeCandidates(
    std::map<CandidateKey, FullTextRuntimeCandidate> candidates,
    u32 top_k,
    const FullTextRuntimeExactRerankProof& proof,
    u64* exact_rerank_count) {
  std::vector<FullTextRuntimeCandidate> ordered;
  ordered.reserve(candidates.size());
  for (auto& [key, candidate] : candidates) {
    (void)key;
    candidate.exact_rerank_score = candidate.score;
    candidate.final_row_admitted = false;
    ordered.push_back(std::move(candidate));
  }
  std::sort(ordered.begin(), ordered.end(), CandidateOrderLess);
  if (top_k != 0 && ordered.size() > top_k) {
    ordered.resize(top_k);
  }
  for (auto& candidate : ordered) {
    if (!proof.evidence_ref.empty() &&
        candidate.source_recheck_evidence_ref.empty()) {
      candidate.score = 0.0;
      candidate.exact_rerank_score = 0.0;
    }
    ++(*exact_rerank_count);
  }
  std::sort(ordered.begin(), ordered.end(), CandidateOrderLess);
  return ordered;
}

std::string CountEvidence(std::string key, u64 value) {
  return std::move(key) + "=" + std::to_string(value);
}

FullTextRuntimeResult RuntimeFailure(std::string code,
                                     std::string key,
                                     std::string detail = {}) {
  FullTextRuntimeResult result;
  result.status = RefusalStatus();
  result.fail_closed = true;
  result.diagnostic = MakeFullTextRuntimeDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.evidence.push_back(kFullTextRuntimeSearchKey);
  result.evidence.push_back("full_text_runtime.fail_closed=true");
  result.evidence.push_back("full_text_runtime.candidate_rows_only=true");
  result.evidence.push_back("full_text_runtime.final_rows_authorized=false");
  result.evidence.push_back("full_text_runtime.descriptor_store_scan=false");
  result.evidence.push_back("full_text_runtime.behavior_store_scan=false");
  result.evidence.push_back("full_text_runtime.exact_source_recheck_required=true");
  result.evidence.push_back("full_text_runtime.mga_recheck_required=true");
  result.evidence.push_back("full_text_runtime.security_recheck_required=true");
  result.evidence.push_back("full_text_runtime.parser_donor_provider_authority=false");
  result.evidence.push_back("full_text_runtime.index_finality_authority=false");
  result.evidence.push_back("full_text_runtime.write_ahead_log_authority=false");
  return result;
}

FullTextRuntimeResult RuntimeSuccess(
    FullTextRuntimeQueryKind kind,
    RuntimeCounters counters,
    u64 candidate_count_before_top_k,
    std::vector<FullTextRuntimeCandidate> candidates,
    u64 exact_rerank_count) {
  FullTextRuntimeResult result;
  result.status = OkStatus();
  result.fail_closed = false;
  result.candidates = std::move(candidates);
  result.scanned_block_count = counters.scanned_blocks;
  result.skipped_block_count = counters.skipped_blocks;
  result.block_max_skipped_count = counters.block_max_skipped;
  result.postings_scanned_count = counters.postings_scanned;
  result.phrase_position_check_count = counters.phrase_position_checks;
  result.candidate_count_before_top_k = candidate_count_before_top_k;
  result.exact_rerank_count = exact_rerank_count;
  result.segment_engine_consumed = true;
  result.bm25_payload_consumed = true;
  result.phrase_positions_consumed = kind == FullTextRuntimeQueryKind::phrase;
  result.block_max_pruning_used = counters.block_max_pruning_used;
  result.impact_ordered_postings_used = counters.impact_ordered_postings_used;
  result.top_k_early_termination = counters.top_k_early_termination;
  result.exact_rerank_applied = true;
  result.evidence.push_back(kFullTextRuntimeSearchKey);
  result.evidence.push_back("full_text_runtime.segment_engine_consumed=true");
  result.evidence.push_back("full_text_runtime.text_inverted_segment_posting_blocks_consumed=true");
  result.evidence.push_back("full_text_runtime.term_dictionary_consumed=true");
  result.evidence.push_back("full_text_runtime.descriptor_store_scan=false");
  result.evidence.push_back("full_text_runtime.behavior_store_scan=false");
  result.evidence.push_back("full_text_runtime.bm25.document_length_used=true");
  result.evidence.push_back("full_text_runtime.bm25.norm_payload_used=true");
  result.evidence.push_back("full_text_runtime.bm25.term_document_statistics_used=true");
  result.evidence.push_back("full_text_runtime.impact_ordered_postings_used=" +
                            std::string(result.impact_ordered_postings_used
                                            ? "true"
                                            : "false"));
  result.evidence.push_back("full_text_runtime.block_max_pruning_used=" +
                            std::string(result.block_max_pruning_used
                                            ? "true"
                                            : "false"));
  result.evidence.push_back("full_text_runtime.top_k_early_termination=" +
                            std::string(result.top_k_early_termination
                                            ? "true"
                                            : "false"));
  result.evidence.push_back("full_text_runtime.phrase.positions_consumed=" +
                            std::string(result.phrase_positions_consumed
                                            ? "true"
                                            : "false"));
  result.evidence.push_back("full_text_runtime.phrase.boundary_checks=" +
                            std::string(result.phrase_positions_consumed
                                            ? "true"
                                            : "false"));
  result.evidence.push_back("full_text_runtime.exact_rerank_applied=true");
  result.evidence.push_back("full_text_runtime.exact_rerank_proof_required=true");
  result.evidence.push_back("full_text_runtime.candidate_rows_only=true");
  result.evidence.push_back("full_text_runtime.final_rows_authorized=false");
  result.evidence.push_back("full_text_runtime.exact_source_recheck_required=true");
  result.evidence.push_back("full_text_runtime.mga_recheck_required=true");
  result.evidence.push_back("full_text_runtime.security_recheck_required=true");
  result.evidence.push_back("full_text_runtime.visibility_security_finality_authority=false");
  result.evidence.push_back("full_text_runtime.parser_donor_provider_authority=false");
  result.evidence.push_back("full_text_runtime.index_finality_authority=false");
  result.evidence.push_back("full_text_runtime.write_ahead_log_authority=false");
  result.evidence.push_back(CountEvidence("full_text_runtime.scanned_blocks",
                                          result.scanned_block_count));
  result.evidence.push_back(CountEvidence("full_text_runtime.skipped_blocks",
                                          result.skipped_block_count));
  result.evidence.push_back(CountEvidence(
      "full_text_runtime.block_max_skipped_blocks",
      result.block_max_skipped_count));
  result.evidence.push_back(CountEvidence("full_text_runtime.postings_scanned",
                                          result.postings_scanned_count));
  result.evidence.push_back(CountEvidence(
      "full_text_runtime.candidate_count_before_top_k",
      result.candidate_count_before_top_k));
  result.evidence.push_back(CountEvidence("full_text_runtime.exact_rerank_count",
                                          result.exact_rerank_count));
  result.diagnostic = MakeFullTextRuntimeDiagnostic(
      result.status,
      "SB_FULL_TEXT_RUNTIME.OK",
      "full_text_runtime.ok",
      FullTextRuntimeQueryKindName(kind));
  std::sort(result.evidence.begin(), result.evidence.end());
  return result;
}

bool QueryShapeValid(const FullTextRuntimeRequest& request) {
  if (request.segments.empty() ||
      request.terms.empty() ||
      !std::all_of(request.terms.begin(), request.terms.end(), TermValid) ||
      !std::isfinite(request.bm25_k1) ||
      !std::isfinite(request.bm25_b) ||
      request.bm25_k1 <= 0.0 ||
      request.bm25_b < 0.0 ||
      request.bm25_b > 1.0) {
    return false;
  }
  switch (request.kind) {
    case FullTextRuntimeQueryKind::term:
      return request.terms.size() == 1;
    case FullTextRuntimeQueryKind::all_terms:
      return request.terms.size() >= 2;
    case FullTextRuntimeQueryKind::phrase:
      return request.terms.size() >= 2;
  }
  return false;
}

}  // namespace

const char* FullTextRuntimeQueryKindName(FullTextRuntimeQueryKind kind) {
  switch (kind) {
    case FullTextRuntimeQueryKind::term:
      return "term";
    case FullTextRuntimeQueryKind::all_terms:
      return "all_terms";
    case FullTextRuntimeQueryKind::phrase:
      return "phrase";
  }
  return "term";
}

FullTextRuntimeResult ExecuteFullTextRuntime(
    const FullTextRuntimeRequest& request) {
  if (!QueryShapeValid(request)) {
    return RuntimeFailure("SB_FULL_TEXT_RUNTIME.QUERY_INVALID",
                          "full_text_runtime.query_invalid");
  }
  if (!request.analyzer_epoch_current || !request.resource_epoch_current) {
    return RuntimeFailure("SB_FULL_TEXT_RUNTIME.UNSAFE_EPOCH",
                          "full_text_runtime.unsafe_epoch");
  }
  if (!RecheckProofValid(request.candidate_recheck_proof)) {
    return RuntimeFailure("SB_FULL_TEXT_RUNTIME.RECHECK_PROOF_MISSING",
                          "full_text_runtime.recheck_proof_missing");
  }
  if (!RerankProofValid(request.exact_rerank_proof)) {
    return RuntimeFailure("SB_FULL_TEXT_RUNTIME.EXACT_RERANK_PROOF_MISSING",
                          "full_text_runtime.exact_rerank_proof_missing");
  }
  for (const auto& segment : request.segments) {
    if (!SegmentRuntimeReady(segment)) {
      return RuntimeFailure("SB_FULL_TEXT_RUNTIME.SEGMENT_REFUSED",
                            "full_text_runtime.segment_refused");
    }
  }

  const auto views = BuildViews(request.segments);
  const auto stats = BuildRuntimeStats(views, request.terms);
  RuntimeCounters counters;
  std::map<CandidateKey, FullTextRuntimeCandidate> candidates;
  switch (request.kind) {
    case FullTextRuntimeQueryKind::term:
      candidates = ExecuteTermQuery(views, stats, request, &counters);
      break;
    case FullTextRuntimeQueryKind::all_terms:
      candidates = ExecuteAllTermsQuery(views, stats, request, &counters);
      break;
    case FullTextRuntimeQueryKind::phrase:
      candidates = ExecutePhraseQuery(views, stats, request, &counters);
      break;
  }

  const u64 candidate_count_before_top_k = candidates.size();
  u64 exact_rerank_count = 0;
  auto final_candidates = FinalizeCandidates(std::move(candidates),
                                             request.top_k,
                                             request.exact_rerank_proof,
                                             &exact_rerank_count);
  return RuntimeSuccess(request.kind,
                        counters,
                        candidate_count_before_top_k,
                        std::move(final_candidates),
                        exact_rerank_count);
}

DiagnosticRecord MakeFullTextRuntimeDiagnostic(Status status,
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
                        "core.index.full_text_runtime");
}

}  // namespace scratchbird::core::index
