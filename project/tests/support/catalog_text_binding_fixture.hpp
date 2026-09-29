// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../../src/engine/internal_api/catalog/column_metadata_codec.hpp"
#include "../../src/engine/internal_api/catalog/name_resolution_api.hpp"
#include "../../src/core/datatypes/datatype_catalog_manifest.hpp"
#include <stdexcept>

namespace scratchbird::tests {
// Explicit positive-fixture profile. This binds existing column identities;
// it neither issues replacements nor repairs stale/contradictory authority.
// Production consumers must obtain their resources from real bound receipts.
inline void BindFixtureUtf8BinaryTextResources(
    const engine::internal_api::EngineRequestContext& context,
    engine::internal_api::EngineColumnDefinition& column) {
  namespace api = engine::internal_api;
  namespace dt = core::datatypes;
  const auto binding = dt::LookupDatatypeTypeCodecIdentityV1(
      context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
      context.datatype_registry_generation, column.descriptor.datatype_descriptor_uuid,
      column.descriptor.datatype_descriptor_generation);
  if (!binding.ok || !dt::IsExactCanonicalTextTypeCodecIdentityV1(binding.row) ||
      binding.row.type_uuid != column.descriptor.type_uuid ||
      !binding.row.canonical_value_maximum_bytes)
    throw std::invalid_argument("TEXT fixture requires an admitted character codec");
  api::CatalogColumnMetadata attributes;
  if (!api::AdmitCatalogColumnMetadata(column.descriptor.encoded_descriptor, &attributes))
    throw std::invalid_argument("TEXT fixture column metadata is malformed");
  const auto charset = api::LookupEngineResourceDescriptorByName(context, "UTF8", "charset");
  const auto collation = api::LookupEngineResourceDescriptorByName(context, "SB_UTF8_BINARY", "collation");
  for (const auto* result : {&charset, &collation})
    if (!result->ok || !result->resource_descriptor.present)
      throw std::runtime_error("TEXT fixture resource lookup failed: " +
          result->diagnostic.code + ":" + result->diagnostic.detail);
  const auto& cs = charset.resource_descriptor;
  const auto& co = collation.resource_descriptor;
  if (cs.database_uuid != context.database_uuid || co.database_uuid != context.database_uuid ||
      co.parent_resource_uuid != cs.resource_uuid || !cs.family_epoch || !co.family_epoch ||
      !context.resource_epoch || cs.resource_epoch != context.resource_epoch ||
      co.resource_epoch != context.resource_epoch)
    throw std::invalid_argument("TEXT fixture resource cohort mismatch");
  const auto bind_identity = [&](const char* key, const api::EngineUuid& value,
                                 const api::EngineUuid& descriptor_value) {
    const auto found = attributes.identities.find(key);
    if ((!descriptor_value.is_nil() && descriptor_value != value) ||
        (found != attributes.identities.end() && found->second != value))
      throw std::invalid_argument("TEXT fixture cannot replace existing resource identity");
    attributes.identities[key] = value;
  };
  bind_identity("charset_uuid", cs.resource_uuid, column.descriptor.charset_uuid);
  bind_identity("collation_uuid", co.resource_uuid, column.descriptor.collation_uuid);
  const auto bind_generation = [&](const char* key, std::uint64_t generation) {
    const auto text = std::to_string(generation);
    const auto found = attributes.text.find(key);
    if (found != attributes.text.end() && found->second != text)
      throw std::invalid_argument("TEXT fixture cannot replace existing resource generation");
    attributes.text[key] = text;
  };
  bind_generation("charset_generation", cs.family_epoch);
  bind_generation("collation_generation", co.family_epoch);
  bind_generation("resource_epoch", cs.resource_epoch);
  if (!attributes.text.contains("character_length") &&
      !attributes.text.contains("char_length") &&
      !attributes.text.contains("text_resource_storage"))
    attributes.text["character_length"] = std::to_string(binding.row.canonical_value_maximum_bytes);
  std::string encoded;
  if (!api::EncodeCatalogColumnMetadata(attributes, &encoded))
    throw std::invalid_argument("TEXT fixture column metadata cannot be encoded");
  column.descriptor.charset_uuid = cs.resource_uuid;
  column.descriptor.collation_uuid = co.resource_uuid;
  column.descriptor.encoded_descriptor = std::move(encoded);
}
} // namespace scratchbird::tests
