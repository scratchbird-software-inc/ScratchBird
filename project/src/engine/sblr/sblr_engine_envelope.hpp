// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

inline constexpr std::uint32_t kEngineSblrEnvelopeMajor = 1;
inline constexpr std::uint32_t kEngineSblrEnvelopeMinor = 0;
inline constexpr std::uint32_t kSblrOperationMagic = 0x504f4253u;
inline constexpr std::uint32_t kSblrOperationTrailerMagic = 0x544f4253u;
inline constexpr std::uint16_t kSblrOperationHeaderSize = 64;
inline constexpr std::uint16_t kSblrOperationSectionCount = 9;
inline constexpr std::uint32_t kSblrOperationSectionTableSize = 216;
inline constexpr std::uint32_t kSblrOperationSectionPayloadOffset = 280;
inline constexpr std::uint64_t kSblrOperationMaximumBytes = 33'554'432;
inline constexpr std::uint32_t kSblrOperationMaximumOperands = 262'144;
inline constexpr std::uint32_t kSblrOperationMaximumValues = 1'048'576;
inline constexpr std::uint32_t kSblrOperationMaximumDepth = 256;
inline constexpr std::uint64_t kSblrOperationMaximumScalarBytes = 65'536;

enum class SblrValueKind : std::uint16_t {
  uuid_ref = 1,
  descriptor_ref = 2,
  policy_ref = 3,
  principal_ref = 4,
  literal_typed = 5,
  parameter_slot = 6,
  result_target = 7,
  proof_token = 8,
  epoch_token = 9,
  profile_ref = 10,
  artifact_ref = 11,
  udr_ref = 12,
  list = 13,
  map = 14,
  null_value = 15,
};

struct SblrOperand {
  std::string type;
  std::string name;
  std::string value;
  std::uint32_t ordinal = 0;
  SblrValueKind value_kind = SblrValueKind::null_value;
  std::uint16_t value_flags = 0;
  std::vector<std::uint8_t> value_body;
};

struct SblrSourceSymbolArtifact {
  std::string symbol_kind;
  std::string stable_key;
  std::string resolved_uuid;
  std::string render_hint;
  std::string scope;
  std::string source_hash;
  bool authoritative = false;
  bool contains_sql_text = false;
};

struct SblrOperationRenderHint {
  std::string hint_kind;
  std::string stable_key;
  std::string value;
  bool authoritative = false;
  bool contains_sql_text = false;
};

struct SblrSourceArtifactMap {
  std::string policy_status = "absent";
  std::string source_identity;
  std::string source_hash;
  std::string artifact_format = "sblr.source_artifact_map.v1";
  bool render_metadata_only = true;
  bool contains_sql_text = false;
  bool raw_sql_text_authoritative = false;
  std::vector<SblrSourceSymbolArtifact> symbols;
  std::vector<SblrOperationRenderHint> operation_render_hints;
};

struct SblrOperationEnvelope {
  std::uint32_t envelope_major = kEngineSblrEnvelopeMajor;
  std::uint32_t envelope_minor = kEngineSblrEnvelopeMinor;
  std::uint16_t opcode_code = 0;
  std::uint16_t operation_version_major = 1;
  std::uint16_t operation_version_minor = 0;
  std::string operation_id;
  std::string opcode;
  std::string result_shape;
  std::string diagnostic_shape;
  std::string parser_package_uuid;
  std::uint32_t parser_package_version_major = 1;
  std::uint32_t parser_package_version_minor = 0;
  std::uint32_t parser_package_version_patch = 0;
  std::string registry_snapshot_uuid;
  std::string trace_key;
  std::vector<SblrOperand> operands;
  SblrSourceArtifactMap source_artifact_map;
  bool contains_sql_text = false;
  bool parser_resolved_names_to_uuids = false;
  bool requires_security_context = true;
  bool requires_transaction_context = false;
  bool requires_cluster_authority = false;
};

struct SblrEnvelopeDiagnostic {
  std::string code;
  std::string message;
  bool error = true;
};

struct SblrEnvelopeValidationResult {
  bool ok = false;
  std::vector<SblrEnvelopeDiagnostic> diagnostics;
};

struct SblrDecodeResult {
  bool ok = false;
  SblrOperationEnvelope envelope;
  std::vector<std::uint8_t> canonical_bytes;
  std::vector<SblrEnvelopeDiagnostic> diagnostics;
};

SblrOperationEnvelope MakeSblrEnvelope(std::string operation_id,
                                       std::string opcode,
                                       std::string trace_key = {});
SblrEnvelopeValidationResult ValidateSblrEnvelope(const SblrOperationEnvelope& envelope);
SblrDecodeResult DecodeSblrEnvelope(std::string_view encoded);
std::string EncodeSblrEnvelope(const SblrOperationEnvelope& envelope);
std::uint32_t SblrCrc32c(const std::uint8_t* data, std::size_t size) noexcept;
std::string SerializeSblrEnvelopeToJson(const SblrOperationEnvelope& envelope);
std::string SerializeSblrValidationToJson(const SblrEnvelopeValidationResult& result);

}  // namespace scratchbird::engine::sblr
