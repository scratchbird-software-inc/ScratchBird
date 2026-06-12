// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "diagnostics/diagnostic_rendering.hpp"

#include "api_diagnostics.hpp"

#include <cctype>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

std::string SeverityForDiagnostic(const EngineApiDiagnostic& diagnostic) {
  if (!diagnostic.error) {
    if (diagnostic.code.find("WARNING") != std::string::npos) { return "warning"; }
    return "info";
  }
  if (diagnostic.code.find("FATAL") != std::string::npos) { return "fatal"; }
  return "error";
}

std::string Lower(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool RetryableDiagnostic(const EngineApiDiagnostic& diagnostic) {
  const auto code = Lower(diagnostic.code);
  const auto detail = Lower(diagnostic.detail);
  return code.find("timeout") != std::string::npos ||
         code.find("retry") != std::string::npos ||
         code.find("stale") != std::string::npos ||
         code.find("busy") != std::string::npos ||
         code.find("unavailable") != std::string::npos ||
         code.find("serialization") != std::string::npos ||
         detail.find("retry") != std::string::npos ||
         detail.find("timeout") != std::string::npos;
}

std::string ShapeForDiagnostic(const EngineApiDiagnostic& diagnostic) {
  const auto code = Lower(diagnostic.code);
  if (code.find("parser") != std::string::npos || code.find("ipc") != std::string::npos) {
    return "diag.parser_server_ipc.v1";
  }
  if (code.find("security") != std::string::npos ||
      code.find("auth") != std::string::npos ||
      code.find("access") != std::string::npos) {
    return "diag.rights.failure.v1";
  }
  if (code.find("lifecycle") != std::string::npos ||
      code.find("dblc") != std::string::npos ||
      code.find("shutdown") != std::string::npos ||
      code.find("maintenance") != std::string::npos) {
    return "diag.server.lifecycle.v1";
  }
  return "diag.message_vector.v1";
}

EngineRenderedDiagnostic RenderDiagnostic(const EngineApiDiagnostic& diagnostic, bool redact_internal_detail) {
  EngineRenderedDiagnostic rendered;
  rendered.code = diagnostic.code;
  rendered.message_key = diagnostic.message_key;
  rendered.severity = SeverityForDiagnostic(diagnostic);
  rendered.error = diagnostic.error;
  rendered.retryable = RetryableDiagnostic(diagnostic);
  rendered.public_shape_id = ShapeForDiagnostic(diagnostic);
  rendered.private_shape_id = rendered.public_shape_id + ".private";
  rendered.redaction_class = redact_internal_detail ? "security_redacted" : "diagnostic_safe";
  rendered.recommended_action = rendered.retryable ? "retry_after_backoff_or_operator_recheck"
                                                   : "inspect_canonical_diagnostic";
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
  rendered.is_null = field.second.is_null;
  return rendered;
}

EngineRenderedRow RenderRow(const EngineRowValue& row) {
  EngineRenderedRow rendered;
  rendered.row_uuid = row.requested_row_uuid.canonical;
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
    envelope.transaction_uuid = result.transaction_uuid.canonical;
  }
  if (envelope.database_uuid.empty() && result.primary_object.object_kind == "database") {
    envelope.database_uuid = result.primary_object.uuid.canonical;
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
  }
  for (const auto& row : result.result_shape.rows) { envelope.rows.push_back(RenderRow(row)); }
  if (options.include_evidence) {
    for (const auto& evidence : result.evidence) {
      envelope.evidence.push_back({evidence.evidence_kind, evidence.evidence_id});
    }
  }

  return envelope;
}

bool ValidateEngineRenderedResultEnvelope(const EngineRenderedResultEnvelope& envelope,
                                          std::vector<std::string>* errors) {
  bool ok = true;
  auto fail = [&](std::string error) {
    ok = false;
    if (errors) { errors->push_back(std::move(error)); }
  };

  if (!envelope.parser_package_rendering_required) { fail("parser_package_rendering_required_must_be_true"); }
  if (!envelope.canonical_diagnostics) { fail("canonical_diagnostics_must_be_true"); }
  if (!envelope.canonical_result_shape) { fail("canonical_result_shape_must_be_true"); }
  if (envelope.parser_finality_authority) { fail("parser_finality_authority_must_be_false"); }
  if (envelope.reference_finality_authority) { fail("reference_finality_authority_must_be_false"); }
  if (envelope.parser_package_uuid.empty()) { fail("parser_package_uuid_required"); }
  if (envelope.parser_package_version.empty()) { fail("parser_package_version_required"); }
  if (envelope.operation_id.empty()) { fail("operation_id_required"); }
  if (envelope.ok) {
    for (const auto& diagnostic : envelope.diagnostics) {
      if (diagnostic.error) { fail("successful_envelope_contains_error_diagnostic"); }
    }
  }
  for (const auto& diagnostic : envelope.diagnostics) {
    if (diagnostic.code.empty()) { fail("diagnostic_code_required"); }
    if (diagnostic.message_key.empty()) { fail("diagnostic_message_key_required"); }
    if (diagnostic.severity.empty()) { fail("diagnostic_severity_required"); }
    if (diagnostic.public_shape_id.empty()) { fail("diagnostic_public_shape_required"); }
    if (diagnostic.private_shape_id.empty()) { fail("diagnostic_private_shape_required"); }
    if (diagnostic.redaction_class.empty()) { fail("diagnostic_redaction_class_required"); }
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

}  // namespace scratchbird::engine::internal_api
