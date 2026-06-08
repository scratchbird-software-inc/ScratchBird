// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB_FULL_TEXT_RUNTIME
#include "text_inverted_segment.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kFullTextRuntimeSearchKey = "SB_FULL_TEXT_RUNTIME";

enum class FullTextRuntimeQueryKind : u32 {
  term = 1,
  all_terms = 2,
  phrase = 3
};

struct FullTextRuntimeExactRerankProof {
  bool proof_supplied = false;
  bool exact_source_rows_available = false;
  bool mga_visibility_recheck_available = false;
  bool security_authorization_recheck_available = false;
  bool score_order_recheck_available = false;
  std::string evidence_ref;
  bool parser_finality_authority_claimed = false;
  bool donor_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool index_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
  bool visibility_authority_claimed = false;
  bool security_authority_claimed = false;
  bool transaction_finality_authority_claimed = false;
};

struct FullTextRuntimeRequest {
  std::vector<TextInvertedSegment> segments;
  FullTextRuntimeQueryKind kind = FullTextRuntimeQueryKind::term;
  std::vector<std::string> terms;
  u32 top_k = 0;
  double bm25_k1 = 1.2;
  double bm25_b = 0.75;
  bool analyzer_epoch_current = true;
  bool resource_epoch_current = true;
  u64 lower_bound_row_ordinal = 0;
  TextInvertedExactRecheckProof candidate_recheck_proof;
  FullTextRuntimeExactRerankProof exact_rerank_proof;
};

struct FullTextRuntimeCandidate {
  TextInvertedRowLocator locator;
  double score = 0.0;
  double exact_rerank_score = 0.0;
  u32 matched_term_count = 0;
  bool phrase_match = false;
  u32 phrase_start_position = 0;
  std::string segment_uuid;
  u64 segment_generation = 0;
  std::string source_recheck_evidence_ref;
  bool exact_source_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool final_row_admitted = false;
};

struct FullTextRuntimeResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = true;
  std::vector<FullTextRuntimeCandidate> candidates;
  u64 scanned_block_count = 0;
  u64 skipped_block_count = 0;
  u64 block_max_skipped_count = 0;
  u64 postings_scanned_count = 0;
  u64 phrase_position_check_count = 0;
  u64 candidate_count_before_top_k = 0;
  u64 exact_rerank_count = 0;
  bool segment_engine_consumed = false;
  bool bm25_payload_consumed = false;
  bool phrase_positions_consumed = false;
  bool block_max_pruning_used = false;
  bool impact_ordered_postings_used = false;
  bool top_k_early_termination = false;
  bool exact_rerank_applied = false;
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

const char* FullTextRuntimeQueryKindName(FullTextRuntimeQueryKind kind);

FullTextRuntimeResult ExecuteFullTextRuntime(
    const FullTextRuntimeRequest& request);

DiagnosticRecord MakeFullTextRuntimeDiagnostic(Status status,
                                               std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail = {});

}  // namespace scratchbird::core::index
