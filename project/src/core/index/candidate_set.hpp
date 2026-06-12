// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-CANDIDATE-SET-ALGEBRA-CLOSURE-ANCHOR
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <functional>
#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class CandidateSetEncoding : u32 {
  unknown = 0,
  exact_row_uuid_ordered_stream = 1,
  compressed_bitmap = 2
};

enum class CandidateSetOperationKind : u32 {
  build_exact_stream = 1,
  build_compressed_bitmap = 2,
  union_sets = 3,
  intersect_sets = 4,
  subtract_sets = 5,
  top_k = 6,
  rerank = 7,
  exact_recheck = 8,
  complement = 9,
  popcount = 10
};

struct CandidateSetRow {
  TypedUuid row_uuid;
  double score = 0.0;
  bool exact_predicate_match = false;
  bool mga_visible = false;
  bool security_authorized = false;
  bool exact_payload_available = false;
  std::string source;
};

struct CandidateSetCompressedRange {
  u64 start_ordinal = 0;
  u64 run_length = 0;
};

enum class CandidateSetCompressedBitmapContainerType : u16 {
  array_sparse = 1,
  run = 2,
  dense_bitmap = 3
};

struct CandidateSetCompressedBitmapRun {
  u16 start_offset = 0;
  u32 run_length = 0;
};

struct CandidateSetCompressedBitmapContainer {
  CandidateSetCompressedBitmapContainerType type =
      CandidateSetCompressedBitmapContainerType::array_sparse;
  u64 base_row_ordinal = 0;
  u32 ordinal_span = 0;
  u32 cardinality = 0;
  std::vector<u16> array_offsets;
  std::vector<CandidateSetCompressedBitmapRun> runs;
  std::vector<u64> bitmap_words;
};

struct CandidateSetAuthorityContext {
  bool engine_mga_authoritative = true;
  bool security_context_bound = true;
  bool row_mga_recheck_required = true;
  bool row_security_recheck_required = true;
  bool exact_recheck_available = true;
  bool exact_rerank_source_available = true;
  bool parser_or_reference_finality_or_visibility_authority = false;
  bool client_finality_or_visibility_authority = false;
  bool provider_finality_or_visibility_authority = false;
  bool wal_recovery_or_finality_authority = false;
};

struct CandidateSet {
  CandidateSetEncoding encoding = CandidateSetEncoding::unknown;
  bool row_uuid_ordered = false;
  bool compressed = false;
  bool approximate = false;
  bool requires_exact_recheck = true;
  bool requires_mga_visibility_recheck = true;
  bool requires_security_authorization_recheck = true;
  bool final_rows_authorized = false;
  bool deleted_overlay_present = false;
  bool candidate_set_finality_authority = false;
  bool parser_or_reference_finality_or_visibility_authority = false;
  bool provider_finality_or_visibility_authority = false;
  bool wal_recovery_or_finality_authority = false;
  bool non_authority_evidence_present = false;
  u64 compressed_bitmap_cardinality = 0;
  bool compressed_bitmap_row_ordinal_range_present = false;
  u64 compressed_bitmap_min_row_ordinal = 0;
  u64 compressed_bitmap_max_row_ordinal = 0;
  std::vector<CandidateSetRow> rows;
  std::vector<CandidateSetCompressedRange> compressed_ranges;
  std::vector<CandidateSetCompressedBitmapContainer>
      compressed_bitmap_containers;
  std::vector<std::string> evidence;
};

struct CandidateSetResult {
  Status status;
  bool fail_closed = false;
  CandidateSetOperationKind operation = CandidateSetOperationKind::build_exact_stream;
  CandidateSet output;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct CandidateSetSerializedResult {
  Status status;
  bool fail_closed = false;
  std::vector<byte> serialized;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct CandidateSetCardinalityResult {
  Status status;
  bool fail_closed = false;
  CandidateSetOperationKind operation = CandidateSetOperationKind::popcount;
  u64 cardinality = 0;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* CandidateSetEncodingName(CandidateSetEncoding encoding);
const char* CandidateSetOperationKindName(CandidateSetOperationKind operation);
const char* CandidateSetCompressionPolicyFamilyName();
const char* CandidateSetCompressedBitmapContainerTypeName(
    CandidateSetCompressedBitmapContainerType type);

CandidateSetResult MakeExactRowUuidOrderedCandidateSet(
    std::vector<CandidateSetRow> rows,
    const CandidateSetAuthorityContext& authority,
    bool require_ordered_input = true);

CandidateSetResult MakeCompressedBitmapCandidateSet(
    std::vector<CandidateSetRow> row_dictionary,
    std::vector<CandidateSetCompressedRange> ranges,
    const CandidateSetAuthorityContext& authority);

CandidateSetResult MakeCompressedBitmapCandidateSetFromRowOrdinals(
    std::vector<u64> row_ordinals,
    const CandidateSetAuthorityContext& authority,
    bool deleted_overlay_present = false);

CandidateSetSerializedResult SerializeCompressedBitmapCandidateSet(
    const CandidateSet& set);

CandidateSetResult DeserializeCompressedBitmapCandidateSet(
    const std::vector<byte>& serialized,
    const CandidateSetAuthorityContext& authority);

std::vector<std::string> InspectCompressedBitmapCandidateSet(
    const CandidateSet& set);

std::vector<CandidateSetRow>
MaterializeCandidateSetRowsForCompatibilityBridge(const CandidateSet& set);

CandidateSetResult UnionCandidateSets(const CandidateSet& left,
                                      const CandidateSet& right,
                                      const CandidateSetAuthorityContext& authority);
CandidateSetResult IntersectCandidateSets(const CandidateSet& left,
                                          const CandidateSet& right,
                                          const CandidateSetAuthorityContext& authority);
CandidateSetResult SubtractCandidateSets(const CandidateSet& left,
                                         const CandidateSet& right,
                                         const CandidateSetAuthorityContext& authority);
CandidateSetResult ComplementCandidateSetWithinUniverse(
    const CandidateSet& input,
    u64 universe_min_row_ordinal,
    u64 universe_max_row_ordinal,
    const CandidateSetAuthorityContext& authority);
CandidateSetResult TopKCandidateSet(const CandidateSet& input,
                                    u64 k,
                                    const CandidateSetAuthorityContext& authority);
CandidateSetCardinalityResult CandidateSetPopcount(
    const CandidateSet& input,
    const CandidateSetAuthorityContext& authority);
CandidateSetResult RerankCandidateSet(
    const CandidateSet& input,
    const std::function<double(const CandidateSetRow&)>& scorer,
    const CandidateSetAuthorityContext& authority);
CandidateSetResult ExactRecheckCandidateSet(
    const CandidateSet& input,
    const CandidateSetAuthorityContext& authority);

DiagnosticRecord MakeCandidateSetDiagnostic(Status status,
                                            std::string diagnostic_code,
                                            std::string message_key,
                                            std::string detail = {});

}  // namespace scratchbird::core::index
