// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scratchbird::server::legacy_rendering {

using engine::internal_api::EngineApiResult;
using engine::internal_api::EngineDescriptor;

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DIAGNOSTIC_RENDERING
// Legacy in-process projection used by old parser/probe consumers. It is not
// a validated DiagnosticVector, ExecutionResultEnvelope or MessageVectorSet.
// This adapter belongs outside the engine. Its historical heuristic fields
// remain required replacement work, not canonical source or policy authority.

struct EngineParserPackageRenderOptions {
  std::string parser_package_uuid;
  std::string parser_package_version;
  std::string client_dialect;
  std::string language_tag = "en";
  std::string correlation_uuid;
  std::string request_uuid;
  std::string session_uuid;
  std::string database_uuid;
  std::string transaction_uuid;
  bool redact_internal_detail = true;
  bool include_evidence = true;
};

struct EngineRenderedDiagnostic {
  std::string code;
  std::string message_key;
  std::string severity;
  std::string detail;
  std::string public_shape_id = "diag.message_vector.v1";
  std::string private_shape_id = "diag.message_vector.v1";
  std::string redaction_class = "diagnostic_safe";
  std::string recommended_action;
  bool error = true;
  bool retryable = false;
  bool internal_detail_redacted = false;
};

struct EngineRenderedField {
  std::string name;
  std::string descriptor_kind;
  std::string canonical_type_name;
  std::string encoded_value;
  bool is_null = false;
};

struct EngineRenderedRow {
  std::string row_uuid;
  std::vector<EngineRenderedField> fields;
};

struct EngineRenderedEvidence {
  std::string evidence_kind;
  std::string evidence_id;
};

struct EngineRenderedResultEnvelope {
  bool ok = false;
  std::string operation_id;
  std::string result_kind;
  std::string parser_package_uuid;
  std::string parser_package_version;
  std::string client_dialect;
  std::string language_tag;
  std::string correlation_uuid;
  std::string request_uuid;
  std::string session_uuid;
  std::string database_uuid;
  std::string transaction_uuid;
  bool parser_package_rendering_required = true;
  bool render_context_valid = true;
  bool parser_finality_authority = false;
  bool reference_finality_authority = false;
  bool redaction_applied = true;
  std::vector<EngineRenderedDiagnostic> diagnostics;
  std::vector<EngineDescriptor> columns;
  std::vector<EngineRenderedRow> rows;
  std::vector<EngineRenderedEvidence> evidence;
};

EngineRenderedResultEnvelope RenderEngineApiResultForParserPackage(const EngineApiResult& result,
                                                                   EngineParserPackageRenderOptions options);

// Checks only this legacy projection's structural consistency. This does not
// validate canonical diagnostics/results, source registration, policy or wire
// conformance and must never be used to authorize public message publication.
bool ValidateLegacyRenderedProjectionStructure(const EngineRenderedResultEnvelope& envelope,
                                               std::vector<std::string>* errors);

}  // namespace scratchbird::server::legacy_rendering
