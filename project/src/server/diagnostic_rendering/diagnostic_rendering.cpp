// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "diagnostic_rendering.hpp"

#include "api_diagnostics.hpp"

#include "uuid.hpp"
#include <utility>

namespace scratchbird::server::legacy_rendering {
using engine::internal_api::EngineApiDiagnostic;
using engine::internal_api::EngineTypedValue;
using engine::internal_api::EngineRowValue;
using engine::internal_api::MakeInvalidRequestDiagnostic;
namespace {

bool SourceMetadataValid(const EngineRenderedDiagnostic& diagnostic) {
  if (diagnostic.code.empty() || diagnostic.message_key.empty() ||
      !diagnostic.source_metadata || diagnostic.source_metadata->code != diagnostic.code ||
      diagnostic.source_metadata->is_failure != diagnostic.error ||
      !scratchbird::core::uuid::IsEngineIdentityUuid(
          scratchbird::core::platform::Uuid{diagnostic.occurrence_uuid})) return false;
  using Severity = scratchbird::core::diagnostics::CanonicalSeverity;
  switch (diagnostic.source_metadata->severity) {
    case Severity::informational: case Severity::warning: case Severity::error:
    case Severity::fatal: case Severity::security: case Severity::critical:
    case Severity::corruption: case Severity::panic: case Severity::debug:
    case Severity::notice: case Severity::audit: case Severity::support:
    case Severity::internal: return true;
  }
  return false;
}

EngineRenderedDiagnostic RenderDiagnostic(const EngineApiDiagnostic& diagnostic, bool redact_internal_detail) {
  EngineRenderedDiagnostic rendered;
  rendered.code = diagnostic.code;
  rendered.message_key = diagnostic.message_key;
  rendered.occurrence_uuid = diagnostic.occurrence_uuid;
  rendered.source_metadata = diagnostic.canonical_metadata;
  rendered.error = diagnostic.error;
  rendered.internal_detail_redacted = redact_internal_detail && !diagnostic.detail.empty();
  rendered.detail = rendered.internal_detail_redacted ? "redacted" : diagnostic.detail;
  return rendered;
}

EngineRenderedField RenderField(const std::pair<std::string, EngineTypedValue>& field) {
  EngineRenderedField rendered;
  rendered.name = field.first;
  rendered.descriptor_kind = field.second.descriptor.descriptor_kind;
  rendered.canonical_type_name = field.second.descriptor.canonical_type_name;
  rendered.encoded_value = field.second.encoded_value;
  rendered.binary_value = field.second.binary_value;
  rendered.is_null = field.second.is_null;
  return rendered;
}

EngineRenderedRow RenderRow(const EngineRowValue& row) {
  EngineRenderedRow rendered;
  if (!row.requested_row_uuid.is_nil())
    rendered.row_uuid = scratchbird::core::uuid::UuidToString(row.requested_row_uuid);
  for (const auto& field : row.fields) { rendered.fields.push_back(RenderField(field)); }
  return rendered;
}

}  // namespace

EngineRenderedResultEnvelope RenderEngineApiResultForParserPackage(const EngineApiResult& result,
                                                                   EngineParserPackageRenderOptions options) {
  EngineRenderedResultEnvelope envelope;
  envelope.ok = result.ok;
  envelope.operation_id = result.operation_id;
  envelope.result_kind = result.result_shape.result_kind;
  envelope.parser_package_uuid = std::move(options.parser_package_uuid);
  envelope.parser_package_version = std::move(options.parser_package_version);
  envelope.client_dialect = std::move(options.client_dialect);
  envelope.language_tag = std::move(options.language_tag);
  envelope.correlation_uuid = std::move(options.correlation_uuid);
  envelope.request_uuid = std::move(options.request_uuid);
  envelope.session_uuid = std::move(options.session_uuid);
  envelope.database_uuid = std::move(options.database_uuid);
  envelope.transaction_uuid = std::move(options.transaction_uuid);
  envelope.redaction_applied = options.redact_internal_detail;
  envelope.columns = result.result_shape.columns;
  if (envelope.transaction_uuid.empty()) {
    if (!result.transaction_uuid.is_nil())
      envelope.transaction_uuid = scratchbird::core::uuid::UuidToString(result.transaction_uuid);
  }
  if (envelope.database_uuid.empty() && result.primary_object.object_kind == "database") {
    if (!result.primary_object.uuid.is_nil())
      envelope.database_uuid = scratchbird::core::uuid::UuidToString(result.primary_object.uuid);
  }

  if (envelope.parser_package_uuid.empty()) {
    envelope.ok = false;
    envelope.render_context_valid = false;
    envelope.diagnostics.push_back(RenderDiagnostic(
        MakeInvalidRequestDiagnostic("diagnostics.render_for_parser_package", "parser_package_uuid_required"),
        false));
  }
  if (envelope.parser_package_version.empty()) {
    envelope.ok = false;
    envelope.render_context_valid = false;
    envelope.diagnostics.push_back(RenderDiagnostic(
        MakeInvalidRequestDiagnostic("diagnostics.render_for_parser_package", "parser_package_version_required"),
        false));
  }

  for (const auto& diagnostic : result.diagnostics) {
    envelope.diagnostics.push_back(RenderDiagnostic(diagnostic, options.redact_internal_detail));
    if (!SourceMetadataValid(envelope.diagnostics.back()) ||
        (result.ok && envelope.diagnostics.back().source_metadata->is_failure)) {
      envelope.ok = false;
      envelope.render_context_valid = false;
    }
  }
  for (const auto& row : result.result_shape.rows) { envelope.rows.push_back(RenderRow(row)); }
  if (options.include_evidence) {
    for (const auto& evidence : result.evidence) {
      envelope.evidence.push_back({evidence.evidence_kind, evidence.evidence_id});
    }
  }

  return envelope;
}

bool ValidateLegacyRenderedProjectionStructure(const EngineRenderedResultEnvelope& envelope,
                                               std::vector<std::string>* errors) {
  bool ok = true;
  auto fail = [&](std::string error) {
    ok = false;
    if (errors) { errors->push_back(std::move(error)); }
  };

  if (!envelope.parser_package_rendering_required) { fail("parser_package_rendering_required_must_be_true"); }
  if (!envelope.render_context_valid) { fail("render_context_invalid"); }
  if (envelope.parser_finality_authority) { fail("parser_finality_authority_must_be_false"); }
  if (envelope.reference_finality_authority) { fail("reference_finality_authority_must_be_false"); }
  if (envelope.parser_package_uuid.empty()) { fail("parser_package_uuid_required"); }
  if (envelope.parser_package_version.empty()) { fail("parser_package_version_required"); }
  if (envelope.operation_id.empty()) { fail("operation_id_required"); }
  if (envelope.ok) {
    for (const auto& diagnostic : envelope.diagnostics) {
      if (diagnostic.error || (diagnostic.source_metadata && diagnostic.source_metadata->is_failure)) {
        fail("successful_envelope_contains_error_diagnostic");
      }
    }
  }
  for (const auto& diagnostic : envelope.diagnostics) {
    if (diagnostic.code.empty()) { fail("diagnostic_code_required"); }
    if (diagnostic.message_key.empty()) { fail("diagnostic_message_key_required"); }
    if (!SourceMetadataValid(diagnostic)) { fail("diagnostic_source_metadata_invalid"); }
  }
  for (const auto& row : envelope.rows) {
    if (row.row_uuid.empty()) { fail("row_uuid_required"); }
    for (const auto& field : row.fields) {
      if (field.name.empty()) { fail("field_name_required"); }
      if (field.canonical_type_name.empty()) { fail("field_canonical_type_required"); }
    }
  }
  return ok;
}

}  // namespace scratchbird::server::legacy_rendering
