// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_diagnostics.hpp"
#include "crud_support/retained_row_value.hpp"
#include "core/datatypes/canonical_utf8.hpp"
#include "datatype_type_codec_identity_v3.hpp"
#include "datatype_operations.hpp"
#include "mga_relation_store/mga_relation_descriptor.hpp"
#include <optional>
#include <string_view>

namespace scratchbird::engine::internal_api::dml {

// Representation validation only. The effectful caller must supply its freshly
// admitted destination, not a source descriptor or an execution permission.
// max_inline_bytes is a spill threshold, never a logical TEXT length limit.
inline std::optional<EngineApiDiagnostic> ValidateTextDestinationValue(
    const MgaRelationColumnStorageDescriptor& column, EngineValueState state,
    std::string_view bytes) {
  namespace dt = core::datatypes;
  if (dt::CanonicalTypeIdFromStableName(column.value_descriptor.canonical_type_name) !=
      dt::CanonicalTypeId::character || state != EngineValueState::value)
    return std::nullopt;
  const auto refuse = [&](const char* code, const char* detail) {
    auto diagnostic = MakeEngineApiDiagnostic(code,
        "engine.dml.text_destination.refused", detail, true);
    diagnostic.identity_fields.push_back({"column_uuid", column.column_uuid});
    diagnostic.fields.push_back({"column_generation", std::to_string(column.column_generation)});
    return std::optional<EngineApiDiagnostic>(std::move(diagnostic));
  };
  const auto& descriptor = column.value_descriptor;
  const auto& cohort = descriptor.datatype_cohort;
  const auto* codec = dt::FindDatatypeTypeCodecIdentityV3(
      cohort.catalog_snapshot_uuid, cohort.catalog_generation,
      cohort.registry_generation, descriptor.datatype_descriptor_uuid,
      descriptor.datatype_descriptor_generation);
  if (!codec || codec->legacy_fields.type_uuid != descriptor.type_uuid ||
      codec->legacy_fields.canonical_binary_type_code !=
          static_cast<std::uint32_t>(dt::CanonicalTypeId::character))
    return refuse("DATATYPE.DESCRIPTOR.INVALID", "text_destination_codec_identity_mismatch");
  if (bytes.size() > codec->legacy_fields.canonical_value_maximum_bytes)
    return refuse("CTB.TEXT.LENGTH_EXCEEDED", "text_destination_registry_byte_limit_exceeded");
  std::uint64_t scalars = 0;
  if (!dt::ValidateCanonicalUtf8(reinterpret_cast<const std::uint8_t*>(bytes.data()),
          bytes.size(), &scalars))
    return refuse("CTB.TEXT.INVALID_ENCODING", "text_destination_invalid_utf8");
  if (column.character_length && scalars > column.character_length)
    return refuse("CTB.TEXT.LENGTH_EXCEEDED", "text_destination_character_limit_exceeded");
  return std::nullopt;
}

inline std::optional<EngineApiDiagnostic> ValidateTextDestinationValues(
    const MgaRelationStorageDescriptor& destination, const CrudValueFields& values) {
  for (const auto& column : destination.columns) {
    if (core::datatypes::CanonicalTypeIdFromStableName(column.value_descriptor.canonical_type_name) !=
        core::datatypes::CanonicalTypeId::character) continue;
    for (const auto& [name, value] : values) {
      if (name != column.canonical_name_key) continue;
      if (auto diagnostic = ValidateTextDestinationValue(column, value.state, value.bytes))
        return diagnostic;
    }
  }
  return std::nullopt;
}

}  // namespace scratchbird::engine::internal_api::dml
