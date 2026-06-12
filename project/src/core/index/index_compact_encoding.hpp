// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "candidate_set.hpp"
#include "compression_policy.hpp"
#include "index_posting.hpp"
#include "uuid_v7_index_encoding.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

enum class IndexCompactEncodingKind : u16 {
  exact_page_uncompressed = 1,
  exact_page_prefix_delta = 2,
  posting_uncompressed = 3,
  posting_varint = 4,
  candidate_set_uncompressed_ordinals = 5,
  candidate_set_bitmap = 6,
  uuidv7_prefix_delta = 7
};

enum class IndexCompactRepairState : u16 {
  validated = 1,
  repaired_from_exact_source = 2,
  refused = 3
};

struct IndexCompactAuthorityContext {
  bool exact_source_proven = true;
  bool order_correctness_proven = true;
  bool encoded_key_order_proven = true;
  bool uuidv7_order_equivalence_proven = true;
  bool mga_visibility_recheck_required = true;
  bool security_recheck_required = true;
  bool parser_or_reference_authority = false;
  bool provider_authority = false;
  bool wal_or_finality_authority = false;
  bool uuid_order_finality_authority = false;
};

struct ExactIndexPageCompactRecord {
  std::vector<byte> encoded_key;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  u64 row_ordinal = 0;
  u32 flags = 0;
  std::vector<byte> payload_metadata;
};

struct ExactIndexPageCompactRequest {
  std::vector<ExactIndexPageCompactRecord> records;
  IndexCompactAuthorityContext authority;
  CompressionPolicyRequest policy =
      DefaultCompressionPolicyRequest(CompressionFamily::kExactIndexPage);
  bool use_policy = true;
};

struct IndexCompactRepairAdmission {
  bool repair_admitted = false;
  bool exact_source_available = false;
  bool same_page_identity_proven = false;
  bool order_proof_present = false;
  std::string proof_detail;
};

struct ExactIndexPageCompactResult {
  Status status;
  bool fail_closed = false;
  bool compressed = false;
  bool fallback_uncompressed = false;
  bool exact_round_trip = false;
  bool order_preserved = false;
  bool repaired = false;
  IndexCompactRepairState repair_state = IndexCompactRepairState::validated;
  IndexCompactEncodingKind encoding =
      IndexCompactEncodingKind::exact_page_uncompressed;
  std::vector<byte> serialized;
  std::vector<ExactIndexPageCompactRecord> records;
  CompressionPolicyDecision policy_decision;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct CompactPostingListRequest {
  IndexPostingList posting_list;
  IndexCompactAuthorityContext authority;
  CompressionPolicyRequest policy =
      DefaultCompressionPolicyRequest(CompressionFamily::kPostingList);
  bool use_policy = true;
};

struct CompactPostingListResult {
  Status status;
  bool fail_closed = false;
  bool compressed = false;
  bool fallback_uncompressed = false;
  bool exact_round_trip = false;
  IndexCompactEncodingKind encoding = IndexCompactEncodingKind::posting_uncompressed;
  std::vector<byte> serialized;
  IndexPostingList posting_list;
  CompressionPolicyDecision policy_decision;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct CompactCandidateSetRequest {
  std::vector<u64> row_ordinals;
  CandidateSetAuthorityContext candidate_authority;
  IndexCompactAuthorityContext authority;
  CompressionPolicyRequest policy =
      DefaultCompressionPolicyRequest(CompressionFamily::kCandidateSet);
  bool use_policy = true;
  bool deleted_overlay_present = false;
};

struct CompactCandidateSetResult {
  Status status;
  bool fail_closed = false;
  bool compressed = false;
  bool fallback_uncompressed = false;
  bool exact_round_trip = false;
  IndexCompactEncodingKind encoding =
      IndexCompactEncodingKind::candidate_set_uncompressed_ordinals;
  std::vector<byte> serialized;
  CandidateSet candidate_set;
  std::vector<u64> exact_ordinals;
  CompressionPolicyDecision policy_decision;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct UuidV7CompactKeyBlockRequest {
  std::vector<TypedUuid> keys;
  UuidKind expected_kind = UuidKind::unknown;
  u64 dictionary_generation = 0;
  IndexCompactAuthorityContext authority;
  CompressionPolicyRequest policy =
      DefaultCompressionPolicyRequest(CompressionFamily::kExactIndexPage);
  bool use_policy = true;
};

struct UuidV7CompactKeyBlockResult {
  Status status;
  bool fail_closed = false;
  bool compressed = false;
  bool fallback_uncompressed = false;
  bool exact_round_trip = false;
  bool order_equivalent_to_full_uuid_bytes = false;
  IndexCompactEncodingKind encoding = IndexCompactEncodingKind::uuidv7_prefix_delta;
  std::vector<byte> serialized;
  std::vector<TypedUuid> decoded_keys;
  UuidV7IndexPageDictionary dictionary;
  CompressionPolicyDecision policy_decision;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* IndexCompactEncodingKindName(IndexCompactEncodingKind kind);
const char* IndexCompactRepairStateName(IndexCompactRepairState state);

ExactIndexPageCompactResult BuildExactIndexPageCompactEncoding(
    const ExactIndexPageCompactRequest& request);
ExactIndexPageCompactResult DecodeExactIndexPageCompactEncoding(
    const std::vector<byte>& serialized,
    const IndexCompactAuthorityContext& authority);
ExactIndexPageCompactResult RepairOrValidateExactIndexPageCompactEncoding(
    const std::vector<byte>& serialized,
    const IndexCompactAuthorityContext& authority,
    const std::vector<ExactIndexPageCompactRecord>* exact_source,
    const IndexCompactRepairAdmission& admission);

CompactPostingListResult BuildCompactPostingListEncoding(
    const CompactPostingListRequest& request);
CompactPostingListResult DecodeCompactPostingListEncoding(
    const std::vector<byte>& serialized,
    const IndexCompactAuthorityContext& authority);

CompactCandidateSetResult BuildCompactCandidateSetEncoding(
    const CompactCandidateSetRequest& request);
CompactCandidateSetResult DecodeCompactCandidateSetEncoding(
    const std::vector<byte>& serialized,
    const CandidateSetAuthorityContext& candidate_authority,
    const IndexCompactAuthorityContext& authority);
std::vector<u64> ExpandCompactCandidateSetOrdinalsForProof(
    const CandidateSet& set);

UuidV7CompactKeyBlockResult BuildUuidV7CompactKeyBlock(
    const UuidV7CompactKeyBlockRequest& request);
UuidV7CompactKeyBlockResult DecodeUuidV7CompactKeyBlock(
    const std::vector<byte>& serialized,
    UuidKind expected_kind,
    u64 expected_dictionary_generation,
    const IndexCompactAuthorityContext& authority);

DiagnosticRecord MakeIndexCompactEncodingDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
