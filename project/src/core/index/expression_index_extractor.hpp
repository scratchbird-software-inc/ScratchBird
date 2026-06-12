// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-EXPRESSION-INDEX-EXTRACTOR-ANCHOR
#include "index_key_encoding.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::byte;
using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class ExpressionIndexNodeKind : u32 {
  literal = 1,
  column_ref = 2,
  operator_call = 3,
  function_call = 4
};

enum class ExpressionIndexOperationKind : u32 {
  operator_call = 1,
  function_call = 2
};

struct ExpressionIndexEpochs {
  u64 resource_epoch = 0;
  u64 collation_epoch = 0;
  u64 function_resource_epoch = 0;
  bool resource_epoch_valid = true;
  bool collation_epoch_valid = true;
  bool function_resource_epoch_valid = true;
};

struct ExpressionIndexValueDescriptor {
  std::string canonical_type_name = "text";
  std::string encoded_descriptor;
  TypedUuid type_descriptor_uuid;
  TypedUuid collation_uuid;
  bool case_folded = false;
};

struct ExpressionIndexTypedValue {
  ExpressionIndexValueDescriptor descriptor;
  std::string encoded_value;
  std::vector<byte> binary_value;
  bool is_null = false;
};

struct ExpressionIndexExpressionNode {
  ExpressionIndexNodeKind kind = ExpressionIndexNodeKind::literal;
  std::string identifier;
  u32 column_ordinal = 0;
  bool column_ordinal_valid = false;
  ExpressionIndexTypedValue literal;
  std::vector<ExpressionIndexExpressionNode> arguments;
};

struct ExpressionIndexDefinition {
  std::string expression_text;
  std::string expression_envelope;
  ExpressionIndexExpressionNode root;
  bool root_present = false;
};

struct ExpressionIndexOperationProof {
  ExpressionIndexOperationKind kind = ExpressionIndexOperationKind::operator_call;
  std::string operation_id;
  bool proof_present = false;
  bool deterministic = false;
  bool resource_epoch_bound = false;
  bool reads_external_resource = false;
  bool depends_on_transaction_state = false;
  bool depends_on_visibility_state = false;
  bool depends_on_authorization_state = false;
  bool depends_on_time_or_random = false;
  std::string proof_digest;
};

struct ExpressionIndexNonAuthorityEvidence {
  bool visibility_authority = false;
  bool authorization_authority = false;
  bool transaction_finality_authority = false;
  bool cleanup_authority = false;
  bool recovery_authority = false;
  bool parser_or_reference_finality_authority = false;
};

struct ExpressionIndexCanonicalizationResult {
  Status status;
  bool ok_value = false;
  std::string canonical_text;
  std::string canonical_envelope;
  std::string expression_digest;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && ok_value; }
};

struct ExpressionIndexExtractorDescriptor {
  std::string cache_key;
  std::string expression_digest;
  std::string canonical_expression_text;
  std::string canonical_expression_envelope;
  std::string result_type_descriptor_signature;
  ExpressionIndexDefinition definition;
  ExpressionIndexValueDescriptor result_descriptor;
  ExpressionIndexEpochs epochs;
  IndexKeySemanticProfile semantic_profile;
  IndexKeySortDirection sort_direction = IndexKeySortDirection::ascending;
  IndexKeyNullPlacement null_placement = IndexKeyNullPlacement::nulls_last;
  ExpressionIndexNonAuthorityEvidence non_authority;
  std::vector<std::string> evidence;
};

struct ExpressionIndexDescriptorCache {
  std::vector<ExpressionIndexExtractorDescriptor> descriptors;
  u64 hits = 0;
  u64 misses = 0;
  u64 invalidations = 0;
};

struct ExpressionIndexBindRequest {
  ExpressionIndexDefinition definition;
  ExpressionIndexValueDescriptor result_descriptor;
  ExpressionIndexEpochs epochs;
  IndexKeySemanticProfile semantic_profile;
  IndexKeySortDirection sort_direction = IndexKeySortDirection::ascending;
  IndexKeyNullPlacement null_placement = IndexKeyNullPlacement::nulls_last;
  std::vector<ExpressionIndexOperationProof> operation_proofs;
};

struct ExpressionIndexBindResult {
  Status status;
  bool admitted = false;
  bool cache_hit = false;
  bool cache_miss = false;
  u64 invalidated_entries = 0;
  ExpressionIndexExtractorDescriptor descriptor;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && admitted; }
};

struct ExpressionIndexColumnValue {
  std::string name;
  u32 ordinal = 0;
  bool ordinal_valid = false;
  ExpressionIndexTypedValue value;
};

struct ExpressionIndexRowInput {
  std::string row_id;
  std::vector<ExpressionIndexColumnValue> columns;
};

struct ExpressionIndexBatchExtractionRequest {
  ExpressionIndexExtractorDescriptor descriptor;
  std::vector<ExpressionIndexRowInput> rows;
  bool continue_on_expression_error = true;
};

struct ExpressionIndexExtractedRow {
  std::string row_id;
  bool extracted = false;
  bool expression_null = false;
  bool expression_error = false;
  ExpressionIndexTypedValue value;
  IndexKeyEncodingResult key;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
};

struct ExpressionIndexBatchExtractionResult {
  Status status;
  bool extracted = false;
  u64 row_count = 0;
  u64 key_count = 0;
  u64 null_count = 0;
  u64 error_count = 0;
  std::vector<ExpressionIndexExtractedRow> rows;
  DiagnosticRecord diagnostic;
  ExpressionIndexNonAuthorityEvidence non_authority;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && extracted; }
};

struct ExpressionIndexKeyEnvelopeValidationResult {
  Status status;
  bool accepted = false;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && accepted; }
};

ExpressionIndexCanonicalizationResult CanonicalizeExpressionIndexDefinition(
    const ExpressionIndexDefinition& definition);

std::string CanonicalizeExpressionIndexTextEnvelope(std::string_view expression_text);
std::string ExpressionIndexDigestForCanonicalEnvelope(std::string_view canonical_envelope);

ExpressionIndexBindResult BindExpressionIndexExtractorDescriptor(
    ExpressionIndexDescriptorCache* cache,
    const ExpressionIndexBindRequest& request);

ExpressionIndexBatchExtractionResult ExtractExpressionIndexKeyBatch(
    const ExpressionIndexBatchExtractionRequest& request);

ExpressionIndexKeyEnvelopeValidationResult ValidateExpressionIndexExtractorKeyEnvelope(
    std::string_view encoded_key);

DiagnosticRecord MakeExpressionIndexExtractorDiagnostic(Status status,
                                                        std::string diagnostic_code,
                                                        std::string message_key,
                                                        std::string detail = {});

}  // namespace scratchbird::core::index
