// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "legacy_diagnostic_projection.hpp"
#include "engine/internal_api/api_diagnostics.hpp"
#include <utility>

namespace scratchbird::server_engine_bridge {
namespace {
namespace api = engine::internal_api;
namespace render = server::legacy_rendering;
render::EngineRenderedDescriptor Descriptor(const api::EngineDescriptor& source) {
  return {source.descriptor_uuid, source.descriptor_kind, source.canonical_type_name,
      source.encoded_descriptor, source.type_uuid, source.collation_uuid,
      source.datatype_descriptor_uuid, source.datatype_descriptor_generation, source.charset_uuid};
}
render::EngineRenderedDiagnostic Diagnostic(const api::EngineApiDiagnostic& source) {
  return {source.code, source.message_key, source.occurrence_uuid,
      source.canonical_metadata, source.detail, source.error, false};
}
render::LegacyRenderValueState State(api::EngineValueState state) {
  switch (state) {
    case api::EngineValueState::value: return render::LegacyRenderValueState::value;
    case api::EngineValueState::sql_null: return render::LegacyRenderValueState::sql_null;
    case api::EngineValueState::missing: return render::LegacyRenderValueState::missing;
    case api::EngineValueState::default_requested: return render::LegacyRenderValueState::default_requested;
    case api::EngineValueState::unknown: return render::LegacyRenderValueState::unknown;
    case api::EngineValueState::error: return render::LegacyRenderValueState::error;
    case api::EngineValueState::lob_handle: return render::LegacyRenderValueState::lob_handle;
    case api::EngineValueState::protected_value: return render::LegacyRenderValueState::protected_value;
  }
  // Retain invalidity so the renderer refuses it; never turn it into SQL NULL.
  return static_cast<render::LegacyRenderValueState>(255);
}
} // namespace

server::legacy_rendering::EngineRenderedResultEnvelope RenderLegacyEngineResult(
    const engine::internal_api::EngineApiResult& source,
    server::legacy_rendering::EngineParserPackageRenderOptions options) {
  render::LegacyRenderSource copied;
  copied.ok = source.ok;
  copied.operation_id = source.operation_id;
  copied.result_kind = source.result_shape.result_kind;
  copied.primary_object_kind = source.primary_object.object_kind;
  copied.primary_object_uuid = source.primary_object.uuid;
  copied.transaction_uuid = source.transaction_uuid;
  // Context refusals keep the actual API diagnostic source and occurrence.
  // Rendering neither manufactures registry metadata nor mints a new source.
  std::vector<std::string> errors;
  render::ValidateLegacyRenderOptions(options, &errors);
  for (const auto& error : errors)
    copied.diagnostics.push_back(Diagnostic(api::MakeInvalidRequestDiagnostic(
        "diagnostics.render_for_parser_package", error)));
  for (const auto& diagnostic : source.diagnostics)
    copied.diagnostics.push_back(Diagnostic(diagnostic));
  for (const auto& column : source.result_shape.columns)
    copied.columns.push_back(Descriptor(column));
  for (const auto& row : source.result_shape.rows) {
    render::EngineRenderedRow projected;
    projected.row_uuid = row.requested_row_uuid;
    for (const auto& [name, field] : row.fields)
      projected.fields.push_back({name, Descriptor(field.descriptor), field.encoded_value,
          field.binary_value, field.is_null, State(field.state)});
    copied.rows.push_back(std::move(projected));
  }
  for (const auto& evidence : source.evidence)
    copied.evidence.push_back({evidence.evidence_kind, evidence.evidence_id});
  return render::RenderEngineApiResultForParserPackage(copied, std::move(options));
}
} // namespace scratchbird::server_engine_bridge
