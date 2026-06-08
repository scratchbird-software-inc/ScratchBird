// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

inline constexpr std::uint32_t kEngineSblrEnvelopeMajor = 1;
inline constexpr std::uint32_t kEngineSblrEnvelopeMinor = 0;

struct SblrOperand {
  std::string type;
  std::string name;
  std::string value;
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
  std::string operation_id;
  std::string opcode;
  std::string result_shape;
  std::string diagnostic_shape;
  std::string parser_package_uuid;
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
  std::vector<SblrEnvelopeDiagnostic> diagnostics;
};

SblrOperationEnvelope MakeSblrEnvelope(std::string operation_id,
                                       std::string opcode,
                                       std::string trace_key = {});
SblrEnvelopeValidationResult ValidateSblrEnvelope(const SblrOperationEnvelope& envelope);
SblrDecodeResult DecodeSblrEnvelope(std::string_view encoded);
std::string EncodeSblrEnvelope(const SblrOperationEnvelope& envelope);
std::string SerializeSblrEnvelopeToJson(const SblrOperationEnvelope& envelope);
std::string SerializeSblrValidationToJson(const SblrEnvelopeValidationResult& result);

}  // namespace scratchbird::engine::sblr
