// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "engine_internal_api.hpp"

#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status EngineApiOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status EngineApiErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine};
}

bool IsEngineIdentityTypedUuid(const TypedUuid& uuid, UuidKind expected_kind) {
  return uuid.kind == expected_kind && uuid.valid() && scratchbird::core::uuid::IsEngineIdentityUuid(uuid.value);
}

}  // namespace

const char* SblrEnvelopeKindName(SblrEnvelopeKind kind) {
  switch (kind) {
    case SblrEnvelopeKind::native_sblr: return "native_sblr";
    case SblrEnvelopeKind::management_sblr: return "management_sblr";
    case SblrEnvelopeKind::system_bootstrap_sblr: return "system_bootstrap_sblr";
    case SblrEnvelopeKind::unknown: return "unknown";
  }
  return "unknown";
}

const char* EngineOperationCodeName(EngineOperationCode operation_code) {
  switch (operation_code) {
    case EngineOperationCode::show_version: return "show_version";
    case EngineOperationCode::show_database: return "show_database";
    case EngineOperationCode::show_database_resources: return "show_database_resources";
    case EngineOperationCode::catalog_lookup_by_uuid: return "catalog_lookup_by_uuid";
    case EngineOperationCode::catalog_lookup_by_localized_path: return "catalog_lookup_by_localized_path";
    case EngineOperationCode::transaction_begin: return "transaction_begin";
    case EngineOperationCode::transaction_commit: return "transaction_commit";
    case EngineOperationCode::transaction_rollback: return "transaction_rollback";
    case EngineOperationCode::unknown: return "unknown";
  }
  return "unknown";
}

const char* OperationAuthorityClassName(OperationAuthorityClass authority_class) {
  switch (authority_class) {
    case OperationAuthorityClass::baseline_inspect: return "baseline_inspect";
    case OperationAuthorityClass::catalog_inspect: return "catalog_inspect";
    case OperationAuthorityClass::catalog_mutate: return "catalog_mutate";
    case OperationAuthorityClass::transaction_control: return "transaction_control";
    case OperationAuthorityClass::data_read: return "data_read";
    case OperationAuthorityClass::data_mutate: return "data_mutate";
    case OperationAuthorityClass::management_inspect: return "management_inspect";
    case OperationAuthorityClass::management_control: return "management_control";
    case OperationAuthorityClass::unknown: return "unknown";
  }
  return "unknown";
}

const char* EngineResultCardinalityName(EngineResultCardinality cardinality) {
  switch (cardinality) {
    case EngineResultCardinality::none: return "none";
    case EngineResultCardinality::single_row: return "single_row";
    case EngineResultCardinality::bounded_rows: return "bounded_rows";
    case EngineResultCardinality::stream: return "stream";
    case EngineResultCardinality::unknown: return "unknown";
  }
  return "unknown";
}

EngineApiValidationResult ValidateEngineContext(const EngineContext& context) {
  EngineApiValidationResult result;
  result.status = EngineApiOkStatus();

  if (!IsEngineIdentityTypedUuid(context.database_uuid, UuidKind::database)) {
    result.status = EngineApiErrorStatus();
    result.diagnostic = MakeEngineApiDiagnostic(result.status,
                                                "SB-ENGINE-API-DATABASE-UUID-MUST-BE-V7",
                                                "engine.api.database_uuid_must_be_v7");
    return result;
  }

  if (!IsEngineIdentityTypedUuid(context.session_uuid, UuidKind::session)) {
    result.status = EngineApiErrorStatus();
    result.diagnostic = MakeEngineApiDiagnostic(result.status,
                                                "SB-ENGINE-API-SESSION-UUID-MUST-BE-V7",
                                                "engine.api.session_uuid_must_be_v7");
    return result;
  }

  if (!IsEngineIdentityTypedUuid(context.principal_uuid, UuidKind::principal)) {
    result.status = EngineApiErrorStatus();
    result.diagnostic = MakeEngineApiDiagnostic(result.status,
                                                "SB-ENGINE-API-PRINCIPAL-UUID-MUST-BE-V7",
                                                "engine.api.principal_uuid_must_be_v7");
    return result;
  }

  if (context.parser_is_trusted) {
    result.status = EngineApiErrorStatus();
    result.diagnostic = MakeEngineApiDiagnostic(result.status,
                                                "SB-ENGINE-API-PARSER-MUST-NOT-BE-TRUSTED",
                                                "engine.api.parser_must_not_be_trusted");
    return result;
  }

  return result;
}

EngineApiValidationResult ValidateSblrEnvelope(const SblrEnvelope& envelope) {
  EngineApiValidationResult result;
  result.status = EngineApiOkStatus();

  if (envelope.envelope_major != kSblrEnvelopeMajor || envelope.envelope_minor > kSblrEnvelopeMinor) {
    result.status = EngineApiErrorStatus();
    result.diagnostic = MakeEngineApiDiagnostic(result.status,
                                                "SB-ENGINE-API-UNSUPPORTED-SBLR-ENVELOPE-VERSION",
                                                "engine.api.unsupported_sblr_envelope_version");
    return result;
  }

  if (envelope.kind == SblrEnvelopeKind::unknown) {
    result.status = EngineApiErrorStatus();
    result.diagnostic = MakeEngineApiDiagnostic(result.status,
                                                "SB-ENGINE-API-UNKNOWN-SBLR-ENVELOPE-KIND",
                                                "engine.api.unknown_sblr_envelope_kind");
    return result;
  }

  if (envelope.contains_sql_text) {
    result.status = EngineApiErrorStatus();
    result.diagnostic = MakeEngineApiDiagnostic(result.status,
                                                "SB-ENGINE-API-SQL-TEXT-NOT-ACCEPTED",
                                                "engine.api.sql_text_not_accepted");
    return result;
  }

  if (!envelope.parser_resolved_names_to_uuids) {
    result.status = EngineApiErrorStatus();
    result.diagnostic = MakeEngineApiDiagnostic(result.status,
                                                "SB-ENGINE-API-NAMES-MUST-BE-RESOLVED-TO-UUIDS",
                                                "engine.api.names_must_be_resolved_to_uuids");
    return result;
  }

  if (envelope.payload.empty()) {
    result.status = EngineApiErrorStatus();
    result.diagnostic = MakeEngineApiDiagnostic(result.status,
                                                "SB-ENGINE-API-EMPTY-SBLR-PAYLOAD",
                                                "engine.api.empty_sblr_payload");
    return result;
  }

  return result;
}

EngineApiValidationResult ValidateEngineColumnDescriptor(const EngineColumnDescriptor& column) {
  EngineApiValidationResult result;
  result.status = EngineApiOkStatus();

  if (column.stable_name.empty()) {
    result.status = EngineApiErrorStatus();
    result.diagnostic = MakeEngineApiDiagnostic(result.status,
                                                "SB-ENGINE-API-COLUMN-STABLE-NAME-REQUIRED",
                                                "engine.api.column_stable_name_required");
    return result;
  }

  if (column.type_id == CanonicalTypeId::unknown) {
    result.status = EngineApiErrorStatus();
    result.diagnostic = MakeEngineApiDiagnostic(result.status,
                                                "SB-ENGINE-API-COLUMN-TYPE-UNKNOWN",
                                                "engine.api.column_type_unknown",
                                                column.stable_name);
    return result;
  }

  auto descriptor = scratchbird::core::datatypes::LookupDatatypeDescriptor(column.type_id);
  if (!descriptor.ok()) {
    result.status = descriptor.status;
    result.diagnostic = descriptor.diagnostic;
    return result;
  }

  return result;
}

EngineResultShapeResult MakeEngineResultShape(EngineResultCardinality cardinality,
                                              std::vector<EngineColumnDescriptor> columns) {
  EngineResultShape shape;
  shape.cardinality = cardinality;
  shape.columns = std::move(columns);
  shape.canonical_diagnostics = true;
  shape.parser_package_shaping_required = true;
  return ValidateEngineResultShape(shape);
}

EngineResultShapeResult ValidateEngineResultShape(const EngineResultShape& result_shape) {
  EngineResultShapeResult result;
  result.status = EngineApiOkStatus();
  result.result_shape = result_shape;

  if (result_shape.cardinality == EngineResultCardinality::unknown) {
    result.status = EngineApiErrorStatus();
    result.diagnostic = MakeEngineApiDiagnostic(result.status,
                                                "SB-ENGINE-API-RESULT-CARDINALITY-UNKNOWN",
                                                "engine.api.result_cardinality_unknown");
    return result;
  }

  if (result_shape.cardinality == EngineResultCardinality::none && !result_shape.columns.empty()) {
    result.status = EngineApiErrorStatus();
    result.diagnostic = MakeEngineApiDiagnostic(result.status,
                                                "SB-ENGINE-API-NONE-RESULT-HAS-COLUMNS",
                                                "engine.api.none_result_has_columns");
    return result;
  }

  if (result_shape.cardinality != EngineResultCardinality::none && result_shape.columns.empty()) {
    result.status = EngineApiErrorStatus();
    result.diagnostic = MakeEngineApiDiagnostic(result.status,
                                                "SB-ENGINE-API-RESULT-COLUMNS-REQUIRED",
                                                "engine.api.result_columns_required");
    return result;
  }

  if (!result_shape.canonical_diagnostics || !result_shape.parser_package_shaping_required) {
    result.status = EngineApiErrorStatus();
    result.diagnostic = MakeEngineApiDiagnostic(result.status,
                                                "SB-ENGINE-API-RESULT-SHAPE-AUTHORITY-VIOLATION",
                                                "engine.api.result_shape_authority_violation");
    return result;
  }

  for (const EngineColumnDescriptor& column : result_shape.columns) {
    EngineApiValidationResult column_result = ValidateEngineColumnDescriptor(column);
    if (!column_result.ok()) {
      result.status = column_result.status;
      result.diagnostic = column_result.diagnostic;
      return result;
    }
  }

  return result;
}

EngineApiValidationResult ValidateBoundEngineOperation(const BoundEngineOperation& operation) {
  EngineApiValidationResult result;
  result.status = EngineApiOkStatus();

  if (operation.operation_code == EngineOperationCode::unknown) {
    result.status = EngineApiErrorStatus();
    result.diagnostic = MakeEngineApiDiagnostic(result.status,
                                                "SB-ENGINE-API-UNKNOWN-OPERATION",
                                                "engine.api.unknown_operation");
    return result;
  }

  if (operation.authority_class == OperationAuthorityClass::unknown) {
    result.status = EngineApiErrorStatus();
    result.diagnostic = MakeEngineApiDiagnostic(result.status,
                                                "SB-ENGINE-API-UNKNOWN-AUTHORITY-CLASS",
                                                "engine.api.unknown_authority_class",
                                                EngineOperationCodeName(operation.operation_code));
    return result;
  }

  if (!operation.requires_engine_security_check) {
    result.status = EngineApiErrorStatus();
    result.diagnostic = MakeEngineApiDiagnostic(result.status,
                                                "SB-ENGINE-API-ENGINE-SECURITY-CHECK-REQUIRED",
                                                "engine.api.engine_security_check_required",
                                                EngineOperationCodeName(operation.operation_code));
    return result;
  }

  EngineResultShapeResult shape_result = ValidateEngineResultShape(operation.result_shape);
  if (!shape_result.ok()) {
    result.status = shape_result.status;
    result.diagnostic = shape_result.diagnostic;
    return result;
  }

  return result;
}

EngineDispatchRequestResult MakeEngineDispatchRequest(EngineContext context,
                                                      SblrEnvelope envelope,
                                                      BoundEngineOperation operation) {
  EngineDispatchRequest request;
  request.context = std::move(context);
  request.envelope = std::move(envelope);
  request.operation = std::move(operation);
  return ValidateEngineDispatchRequest(request);
}

EngineDispatchRequestResult ValidateEngineDispatchRequest(const EngineDispatchRequest& request) {
  EngineDispatchRequestResult result;
  result.status = EngineApiOkStatus();
  result.request = request;

  EngineApiValidationResult context_result = ValidateEngineContext(request.context);
  if (!context_result.ok()) {
    result.status = context_result.status;
    result.diagnostic = context_result.diagnostic;
    return result;
  }

  EngineApiValidationResult envelope_result = ValidateSblrEnvelope(request.envelope);
  if (!envelope_result.ok()) {
    result.status = envelope_result.status;
    result.diagnostic = envelope_result.diagnostic;
    return result;
  }

  EngineApiValidationResult operation_result = ValidateBoundEngineOperation(request.operation);
  if (!operation_result.ok()) {
    result.status = operation_result.status;
    result.diagnostic = operation_result.diagnostic;
    return result;
  }

  return result;
}

DiagnosticRecord MakeEngineApiDiagnostic(Status status,
                                         std::string diagnostic_code,
                                         std::string message_key,
                                         std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "engine.internal_api");
}

}  // namespace scratchbird::engine::internal_api
