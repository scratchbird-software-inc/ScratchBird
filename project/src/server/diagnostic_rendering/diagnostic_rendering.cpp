// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "diagnostic_rendering.hpp"
#include "../../core/uuid/uuid.hpp"
#include <utility>

namespace scratchbird::server::legacy_rendering {
namespace {
bool Identity(const RenderUuid& id) { return core::uuid::IsEngineIdentityUuid(id); }
bool OptionalIdentity(const RenderUuid& id) { return id.is_nil() || Identity(id); }

bool SourceMetadataValid(const EngineRenderedDiagnostic& diagnostic) {
  if (diagnostic.code.empty() || diagnostic.message_key.empty() ||
      !diagnostic.source_metadata || diagnostic.source_metadata->code != diagnostic.code ||
      diagnostic.source_metadata->is_failure != diagnostic.error ||
      !Identity(RenderUuid{diagnostic.occurrence_uuid})) return false;
  using Severity = core::diagnostics::CanonicalSeverity;
  switch (diagnostic.source_metadata->severity) {
    case Severity::informational: case Severity::warning: case Severity::error:
    case Severity::fatal: case Severity::security: case Severity::critical:
    case Severity::corruption: case Severity::panic: case Severity::debug:
    case Severity::notice: case Severity::audit: case Severity::support:
    case Severity::internal: return true;
  }
  return false;
}
bool DescriptorIdentitiesValid(const EngineRenderedDescriptor& descriptor) {
  return OptionalIdentity(descriptor.descriptor_uuid) && OptionalIdentity(descriptor.type_uuid) &&
      OptionalIdentity(descriptor.collation_uuid) && OptionalIdentity(descriptor.datatype_descriptor_uuid) &&
      OptionalIdentity(descriptor.charset_uuid);
}
bool FieldValid(const EngineRenderedField& field) {
  if (field.name.empty() || field.descriptor.canonical_type_name.empty() ||
      !DescriptorIdentitiesValid(field.descriptor)) return false;
  switch (field.state) {
    case LegacyRenderValueState::value:
      // Legacy is_null is still admitted only in its historical value state.
      if (field.is_null) return field.encoded_value.empty() && field.binary_value.empty();
      if (field.descriptor.canonical_type_name == "uuid")
        return field.encoded_value.empty() && field.binary_value.size() == 16;
      return true;
    case LegacyRenderValueState::sql_null:
      return field.is_null && field.encoded_value.empty() && field.binary_value.empty();
    case LegacyRenderValueState::missing:
    case LegacyRenderValueState::default_requested:
    case LegacyRenderValueState::unknown:
      return !field.is_null && field.encoded_value.empty() && field.binary_value.empty();
    case LegacyRenderValueState::error:
    case LegacyRenderValueState::lob_handle:
    case LegacyRenderValueState::protected_value:
      return !field.is_null;
  }
  return false;
}
} // namespace

bool ValidateLegacyRenderOptions(const EngineParserPackageRenderOptions& options,
                                 std::vector<std::string>* errors) {
  bool valid = true;
  auto reject = [&](const char* reason) {
    valid = false;
    if (errors) errors->emplace_back(reason);
  };
  if (!Identity(options.parser_package_uuid)) reject("parser_package_uuid_invalid");
  if (options.parser_package_version.empty()) reject("parser_package_version_required");
  if (!OptionalIdentity(options.correlation_uuid)) reject("correlation_uuid_invalid");
  if (!OptionalIdentity(options.request_uuid)) reject("request_uuid_invalid");
  if (!OptionalIdentity(options.session_uuid)) reject("session_uuid_invalid");
  if (!OptionalIdentity(options.database_uuid)) reject("database_uuid_invalid");
  if (!OptionalIdentity(options.transaction_uuid)) reject("transaction_uuid_invalid");
  return valid;
}

EngineRenderedResultEnvelope RenderEngineApiResultForParserPackage(
    const LegacyRenderSource& source, EngineParserPackageRenderOptions options) {
  EngineRenderedResultEnvelope envelope;
  envelope.ok = source.ok;
  envelope.render_context_valid = ValidateLegacyRenderOptions(options, nullptr);
  envelope.operation_id = source.operation_id;
  envelope.result_kind = source.result_kind;
  envelope.parser_package_uuid = options.parser_package_uuid;
  envelope.parser_package_version = std::move(options.parser_package_version);
  envelope.client_dialect = std::move(options.client_dialect);
  envelope.language_tag = std::move(options.language_tag);
  envelope.correlation_uuid = options.correlation_uuid;
  envelope.request_uuid = options.request_uuid;
  envelope.session_uuid = options.session_uuid;
  envelope.database_uuid = options.database_uuid;
  envelope.transaction_uuid = options.transaction_uuid;
  envelope.redaction_applied = options.redact_internal_detail;
  envelope.columns = source.columns;
  if (!OptionalIdentity(source.transaction_uuid) ||
      (!source.transaction_uuid.is_nil() && !envelope.transaction_uuid.is_nil() &&
       source.transaction_uuid != envelope.transaction_uuid))
    envelope.render_context_valid = false;
  if (envelope.transaction_uuid.is_nil()) envelope.transaction_uuid = source.transaction_uuid;
  if (source.primary_object_kind == "database") {
    if (!Identity(source.primary_object_uuid) ||
        (!envelope.database_uuid.is_nil() && envelope.database_uuid != source.primary_object_uuid))
      envelope.render_context_valid = false;
    if (envelope.database_uuid.is_nil()) envelope.database_uuid = source.primary_object_uuid;
  }
  for (const auto& source_diagnostic : source.diagnostics) {
    auto diagnostic = source_diagnostic;
    diagnostic.internal_detail_redacted = source_diagnostic.internal_detail_redacted ||
        (options.redact_internal_detail && !source_diagnostic.detail.empty());
    if (diagnostic.internal_detail_redacted) diagnostic.detail = "redacted";
    if (!SourceMetadataValid(diagnostic) ||
        (source.ok && diagnostic.source_metadata->is_failure))
      envelope.render_context_valid = false;
    envelope.diagnostics.push_back(std::move(diagnostic));
  }
  envelope.rows = source.rows;
  if (options.include_evidence) envelope.evidence = source.evidence;
  if (!envelope.render_context_valid ||
      !ValidateLegacyRenderedProjectionStructure(envelope, nullptr)) {
    envelope.ok = false;
    envelope.render_context_valid = false;
  }
  return envelope;
}

bool ValidateLegacyRenderedProjectionStructure(const EngineRenderedResultEnvelope& envelope,
                                               std::vector<std::string>* errors) {
  bool valid = true;
  auto reject = [&](const char* reason) {
    valid = false;
    if (errors) errors->emplace_back(reason);
  };
  if (!envelope.parser_package_rendering_required) reject("parser_package_rendering_required_must_be_true");
  if (!envelope.render_context_valid) reject("render_context_invalid");
  if (envelope.parser_finality_authority) reject("parser_finality_authority_must_be_false");
  if (envelope.reference_finality_authority) reject("reference_finality_authority_must_be_false");
  if (!Identity(envelope.parser_package_uuid)) reject("parser_package_uuid_invalid");
  if (envelope.parser_package_version.empty()) reject("parser_package_version_required");
  if (envelope.operation_id.empty()) reject("operation_id_required");
  if (!OptionalIdentity(envelope.correlation_uuid)) reject("correlation_uuid_invalid");
  if (!OptionalIdentity(envelope.request_uuid)) reject("request_uuid_invalid");
  if (!OptionalIdentity(envelope.session_uuid)) reject("session_uuid_invalid");
  if (!OptionalIdentity(envelope.database_uuid)) reject("database_uuid_invalid");
  if (!OptionalIdentity(envelope.transaction_uuid)) reject("transaction_uuid_invalid");
  for (const auto& diagnostic : envelope.diagnostics) {
    if (!SourceMetadataValid(diagnostic)) reject("diagnostic_source_metadata_invalid");
    if (envelope.ok && (diagnostic.error ||
        (diagnostic.source_metadata && diagnostic.source_metadata->is_failure)))
      reject("successful_envelope_contains_error_diagnostic");
  }
  for (const auto& column : envelope.columns)
    if (!DescriptorIdentitiesValid(column)) reject("column_identity_invalid");
  for (const auto& row : envelope.rows) {
    if (!Identity(row.row_uuid)) reject("row_uuid_invalid");
    for (const auto& field : row.fields)
      if (!FieldValid(field)) reject("field_value_or_descriptor_invalid");
  }
  for (const auto& evidence : envelope.evidence)
    if (const auto* identity = std::get_if<RenderUuid>(&evidence.evidence_id);
        identity && !Identity(*identity)) reject("evidence_uuid_invalid");
  return valid;
}
} // namespace scratchbird::server::legacy_rendering
