// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../../core/diagnostics/canonical_diagnostic_snapshot.hpp"
#include <array>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace scratchbird::server::legacy_rendering {
using RenderUuid = core::platform::Uuid;

// This owned in-process projection is not a DiagnosticVector, public engine
// result, MessageVectorSet, or authority to publish on the parser channel.
// Its engine adapter is outside the server. The renderer imports only Core
// value types; source registration facts are never inferred from message text.
struct EngineParserPackageRenderOptions {
  RenderUuid parser_package_uuid;
  std::string parser_package_version;
  std::string client_dialect;
  std::string language_tag = "en";
  RenderUuid correlation_uuid, request_uuid, session_uuid, database_uuid, transaction_uuid;
  bool redact_internal_detail = true;
  bool include_evidence = true;
};

struct EngineRenderedDiagnostic {
  std::string code;
  std::string message_key;
  std::array<std::uint8_t, 16> occurrence_uuid{};
  std::optional<core::diagnostics::CanonicalDiagnosticMetadata> source_metadata;
  std::string detail;
  bool error = true;
  bool internal_detail_redacted = false;
};

struct EngineRenderedDescriptor {
  RenderUuid descriptor_uuid;
  std::string descriptor_kind, canonical_type_name, encoded_descriptor;
  RenderUuid type_uuid, collation_uuid, datatype_descriptor_uuid;
  std::uint64_t datatype_descriptor_generation = 0;
  RenderUuid charset_uuid;
  bool operator==(const EngineRenderedDescriptor&) const = default;
};

enum class LegacyRenderValueState : std::uint8_t {
  value = 0, sql_null = 1, missing = 2, default_requested = 3,
  unknown = 4, error = 5, lob_handle = 6, protected_value = 7
};

struct EngineRenderedField {
  std::string name;
  EngineRenderedDescriptor descriptor;
  std::string encoded_value;
  std::vector<std::uint8_t> binary_value;
  bool is_null = false;
  LegacyRenderValueState state = LegacyRenderValueState::value;
};

struct EngineRenderedRow {
  RenderUuid row_uuid;
  std::vector<EngineRenderedField> fields;
};

struct EngineRenderedEvidence {
  std::string evidence_kind;
  std::variant<std::string, RenderUuid> evidence_id;
};

// Lossless for this legacy projection's fields, not a complete engine result.
// No pointer, alias, or dependency on a private engine structure crosses here.
struct LegacyRenderSource {
  bool ok = false;
  std::string operation_id, result_kind, primary_object_kind;
  RenderUuid primary_object_uuid, transaction_uuid;
  std::vector<EngineRenderedDiagnostic> diagnostics;
  std::vector<EngineRenderedDescriptor> columns;
  std::vector<EngineRenderedRow> rows;
  std::vector<EngineRenderedEvidence> evidence;
};

struct EngineRenderedResultEnvelope {
  bool ok = false;
  std::string operation_id;
  std::string result_kind;
  RenderUuid parser_package_uuid;
  std::string parser_package_version;
  std::string client_dialect;
  std::string language_tag;
  RenderUuid correlation_uuid, request_uuid, session_uuid, database_uuid, transaction_uuid;
  bool parser_package_rendering_required = true;
  bool render_context_valid = true;
  bool parser_finality_authority = false;
  bool reference_finality_authority = false;
  bool redaction_applied = true;
  std::vector<EngineRenderedDiagnostic> diagnostics;
  std::vector<EngineRenderedDescriptor> columns;
  std::vector<EngineRenderedRow> rows;
  std::vector<EngineRenderedEvidence> evidence;
};

bool ValidateLegacyRenderOptions(const EngineParserPackageRenderOptions& options,
                                 std::vector<std::string>* errors);
EngineRenderedResultEnvelope RenderEngineApiResultForParserPackage(
    const LegacyRenderSource& source, EngineParserPackageRenderOptions options);

// Structural consistency only, never canonical conformance or publication
// authorization. System identities are UUIDv7; user UUID values are raw binary16.
bool ValidateLegacyRenderedProjectionStructure(const EngineRenderedResultEnvelope& envelope,
                                               std::vector<std::string>* errors);
} // namespace scratchbird::server::legacy_rendering
