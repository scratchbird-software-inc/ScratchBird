// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

using SblrSourceArtifactUuidV1 = std::array<std::uint8_t, 16>;
using SblrSourceArtifactSha256V1 = std::array<std::uint8_t, 32>;

inline constexpr std::size_t kSblrSourceArtifactHeaderSizeV1 = 224;
inline constexpr std::size_t kSblrSourceArtifactMaximumBytesV1 = 4'194'304;
inline constexpr std::size_t kSblrSourceArtifactMaximumRecordsV1 = 4'096;
inline constexpr std::size_t kSblrSourceArtifactRetainRequestHeaderSizeV1 =
    144;
inline constexpr std::size_t kSblrSourceArtifactRetainAckSizeV1 = 152;

enum class SblrSourceArtifactRedactionClassV1 : std::uint8_t {
  none = 0,
  diagnostic_safe = 1,
  admin_only = 2,
  security_redacted = 3,
  absent = 4,
};

enum class SblrSourceArtifactDecompilePolicyV1 : std::uint8_t {
  source_preserving = 1,
  canonical_sbsql = 2,
  diagnostic_only = 3,
  forbidden = 4,
};

enum class SblrSourceArtifactSymbolKindV1 : std::uint8_t {
  variable = 1,
  parameter = 2,
  cursor = 3,
  label = 4,
  block_name = 5,
  routine = 6,
  routine_argument = 7,
  exception_handler = 8,
  cte = 9,
  relation_alias = 10,
  column_alias = 11,
  object_display_name = 12,
  generated_temp = 13,
};

enum class SblrSourceArtifactQuoteStyleV1 : std::uint8_t {
  none = 0,
  double_quote = 1,
  backtick = 2,
  bracket = 3,
  native_sbsql = 4,
};

enum class SblrSourceArtifactSymbolRedactionV1 : std::uint8_t {
  visible = 0,
  placeholder = 1,
  hash_only = 2,
  removed = 3,
};

enum class SblrSourceArtifactSpanKindV1 : std::uint8_t {
  statement = 1,
  expression = 2,
  identifier = 3,
  literal = 4,
  comment = 5,
  whitespace = 6,
  generated = 7,
  redacted = 8,
};

enum class SblrSourceArtifactKeywordCaseV1 : std::uint8_t {
  preserve = 0,
  upper = 1,
  lower = 2,
  native_default = 3,
};

enum class SblrSourceArtifactIdentifierPolicyV1 : std::uint8_t {
  preserve_source = 0,
  canonical_name = 1,
  localized_name = 2,
  generated_safe = 3,
  redacted_placeholder = 4,
};

enum class SblrSourceArtifactCommentPolicyV1 : std::uint8_t {
  preserve = 0,
  discard = 1,
  diagnostic_only = 2,
  redacted = 3,
};

struct SblrSourceArtifactSourceTextRefV1 {
  bool present = false;
  SblrSourceArtifactUuidV1 uuid{};
  std::uint64_t declared_size = 0;
  std::uint32_t crc32c = 0;
  SblrSourceArtifactSha256V1 sha256{};
};

struct SblrSourceArtifactSymbolV1 {
  std::uint64_t symbol_id = 0;
  std::string symbol_key;
  SblrSourceArtifactSymbolKindV1 symbol_kind =
      SblrSourceArtifactSymbolKindV1::variable;
  std::uint64_t declaration_node_id = 0;
  std::uint64_t scope_node_id = 0;
  std::vector<std::uint64_t> use_node_ids;
  SblrSourceArtifactUuidV1 related_object_uuid{};
  std::string raw_name_utf8;
  std::string normalized_lookup_key;
  bool was_quoted = false;
  SblrSourceArtifactQuoteStyleV1 quote_style =
      SblrSourceArtifactQuoteStyleV1::none;
  std::string language_tag = "en";
  std::uint32_t ordinal = 0;
  std::uint64_t source_span_id = 0;
  bool generated = false;
  SblrSourceArtifactSymbolRedactionV1 redaction_state =
      SblrSourceArtifactSymbolRedactionV1::visible;
  SblrSourceArtifactSha256V1 record_sha256{};
};

struct SblrSourceArtifactSpanV1 {
  std::uint64_t source_span_id = 0;
  std::uint64_t node_id = 0;
  std::uint64_t byte_start = 0;
  std::uint64_t byte_length = 0;
  std::uint32_t line_start = 0;
  std::uint32_t column_start = 0;
  std::uint32_t line_end = 0;
  std::uint32_t column_end = 0;
  SblrSourceArtifactSpanKindV1 span_kind =
      SblrSourceArtifactSpanKindV1::statement;
  SblrSourceArtifactSha256V1 record_sha256{};
};

struct SblrSourceArtifactRenderHintV1 {
  std::uint64_t render_hint_id = 0;
  std::uint64_t node_id = 0;
  std::uint64_t symbol_id = 0;
  SblrSourceArtifactUuidV1 dialect_family_uuid{};
  SblrSourceArtifactKeywordCaseV1 keyword_case =
      SblrSourceArtifactKeywordCaseV1::preserve;
  SblrSourceArtifactIdentifierPolicyV1 identifier_render_policy =
      SblrSourceArtifactIdentifierPolicyV1::preserve_source;
  SblrSourceArtifactQuoteStyleV1 delimiter_hint =
      SblrSourceArtifactQuoteStyleV1::none;
  SblrSourceArtifactCommentPolicyV1 comment_policy =
      SblrSourceArtifactCommentPolicyV1::preserve;
  std::string format_group;
  SblrSourceArtifactSha256V1 record_sha256{};
};

struct SblrSourceArtifactMapV1 {
  SblrSourceArtifactUuidV1 artifact_uuid{};
  SblrSourceArtifactUuidV1 sblr_envelope_uuid{};
  SblrSourceArtifactUuidV1 container_request_uuid{};
  SblrSourceArtifactUuidV1 dialect_family_uuid{};
  SblrSourceArtifactUuidV1 parser_package_uuid{};
  std::string language_tag = "en";
  SblrSourceArtifactRedactionClassV1 redaction_class =
      SblrSourceArtifactRedactionClassV1::none;
  SblrSourceArtifactDecompilePolicyV1 decompile_policy =
      SblrSourceArtifactDecompilePolicyV1::source_preserving;
  SblrSourceArtifactSourceTextRefV1 source_text_ref;
  std::vector<SblrSourceArtifactSymbolV1> symbols;
  std::vector<SblrSourceArtifactSpanV1> source_spans;
  std::vector<SblrSourceArtifactRenderHintV1> render_hints;
  SblrSourceArtifactSha256V1 artifact_sha256{};
};

enum class SblrSourceArtifactDecodeStatusV1 {
  ok,
  invalid,
  resource_exceeded,
};

struct SblrSourceArtifactDecodeResultV1 {
  SblrSourceArtifactDecodeStatusV1 status =
      SblrSourceArtifactDecodeStatusV1::invalid;
  SblrSourceArtifactMapV1 artifact;
  std::vector<std::uint8_t> canonical_bytes;
  std::string detail;
};

struct SblrSourceArtifactValidationContextV1 {
  bool operation_validated_without_artifact = false;
  bool source_preserving_requested = false;
  bool admin_artifact_access = false;
  SblrSourceArtifactUuidV1 expected_sblr_envelope_uuid{};
  SblrSourceArtifactUuidV1 expected_container_request_uuid{};
  SblrSourceArtifactUuidV1 expected_dialect_family_uuid{};
  SblrSourceArtifactUuidV1 expected_parser_package_uuid{};
  std::vector<std::uint64_t> admitted_node_ids;
  std::vector<SblrSourceArtifactUuidV1> admitted_object_uuids;
};

// Private, authenticated statement-scoped retention carrier.  The parser may
// present canonical SAM1 bytes, but only the engine-owned receipt retains them.
// The later SBEE carries the immutable reference fields below and never the
// source artifact body itself.
struct SblrSourceArtifactRetainRequestV1 {
  SblrSourceArtifactUuidV1 authenticated_receipt_uuid{};
  SblrSourceArtifactUuidV1 sblr_envelope_uuid{};
  SblrSourceArtifactUuidV1 artifact_uuid{};
  std::uint64_t declared_size = 0;
  std::uint32_t crc32c = 0;
  SblrSourceArtifactRedactionClassV1 redaction_class =
      SblrSourceArtifactRedactionClassV1::none;
  SblrSourceArtifactDecompilePolicyV1 decompile_policy =
      SblrSourceArtifactDecompilePolicyV1::source_preserving;
  SblrSourceArtifactSha256V1 artifact_sha256{};
  SblrSourceArtifactSha256V1 request_evidence_sha256{};
  std::vector<std::uint8_t> canonical_artifact_bytes;
};

struct SblrSourceArtifactRetainAckV1 {
  SblrSourceArtifactUuidV1 authenticated_receipt_uuid{};
  SblrSourceArtifactUuidV1 sblr_envelope_uuid{};
  SblrSourceArtifactUuidV1 artifact_uuid{};
  std::uint64_t declared_size = 0;
  std::uint32_t crc32c = 0;
  SblrSourceArtifactRedactionClassV1 redaction_class =
      SblrSourceArtifactRedactionClassV1::none;
  SblrSourceArtifactDecompilePolicyV1 decompile_policy =
      SblrSourceArtifactDecompilePolicyV1::source_preserving;
  SblrSourceArtifactSha256V1 artifact_sha256{};
  std::uint64_t retention_generation = 0;
  SblrSourceArtifactSha256V1 acknowledgement_evidence_sha256{};
};

std::vector<std::uint8_t> EncodeSblrSourceArtifactMapV1(
    const SblrSourceArtifactMapV1& artifact,
    std::string* detail = nullptr);

SblrSourceArtifactDecodeResultV1 DecodeSblrSourceArtifactMapV1(
    const std::uint8_t* data,
    std::size_t size);

bool ValidateSblrSourceArtifactMapV1(
    const SblrSourceArtifactMapV1& artifact,
    const SblrSourceArtifactValidationContextV1& context,
    std::string* detail);

SblrSourceArtifactSha256V1 HashSblrSourceArtifactBytesV1(
    const std::uint8_t* data,
    std::size_t size);

std::vector<std::uint8_t> EncodeSblrSourceArtifactRetainRequestV1(
    const SblrSourceArtifactRetainRequestV1& request,
    std::string* detail = nullptr);

bool DecodeSblrSourceArtifactRetainRequestV1(
    const std::uint8_t* data,
    std::size_t size,
    SblrSourceArtifactRetainRequestV1* request,
    std::string* detail = nullptr);

std::vector<std::uint8_t> EncodeSblrSourceArtifactRetainAckV1(
    const SblrSourceArtifactRetainAckV1& acknowledgement,
    std::string* detail = nullptr);

bool DecodeSblrSourceArtifactRetainAckV1(
    const std::uint8_t* data,
    std::size_t size,
    SblrSourceArtifactRetainAckV1* acknowledgement,
    std::string* detail = nullptr);

}  // namespace scratchbird::engine::sblr
