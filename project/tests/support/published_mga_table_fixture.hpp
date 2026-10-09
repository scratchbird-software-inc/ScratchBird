// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../../src/engine/internal_api/catalog/catalog_object_lifecycle.hpp"
#include "../../src/engine/internal_api/catalog/column_metadata_codec.hpp"
#include "../../src/engine/internal_api/catalog/datatype_bootstrap_identity.hpp"
#include "../../src/engine/internal_api/catalog/name_resolution_api.hpp"
#include "../../src/engine/internal_api/mga_relation_store/mga_relation_store.hpp"
#include "../../src/core/datatypes/datatype_catalog_manifest.hpp"
#include "../../src/core/datatypes/datatype_operations.hpp"
#include <optional>
#include <stdexcept>

namespace scratchbird::tests {
// Low-level storage fixtures still publish their object/column cohort through
// the catalog in the caller's real MGA transaction. No generation or datatype
// identity is reconstructed from the compatibility metadata strings.
inline engine::internal_api::EngineApiDiagnostic PublishMgaTableFixture(
    engine::internal_api::EngineRequestContext& context,
    engine::internal_api::CrudTableRecord table,
    const std::vector<std::string>& canonical_types,
    const std::vector<engine::internal_api::CrudIndexRecord>& indexes = {}) {
  namespace api = engine::internal_api;
  namespace dt = core::datatypes;
  if (canonical_types.size() != table.columns.size() || !context.local_transaction_id)
    throw std::invalid_argument("fixture table requires explicit types and MGA transaction");
  context.datatype_catalog_snapshot_uuid = api::kBootstrapDatatypeCatalogUuid;
  context.datatype_catalog_generation = api::kBootstrapDatatypeCatalogGeneration;
  context.datatype_registry_generation = api::kBootstrapDatatypeRegistryGeneration;
  const auto checked = [](const auto& result) {
    if (!result.ok) throw std::runtime_error(result.diagnostics.empty()
        ? "fixture catalog publication failed"
        : result.diagnostics.front().code + ":" + result.diagnostics.front().detail);
  };
  if (context.current_schema_uuid.is_nil()) {
    api::EngineCatalogCreateObjectRequest schema;
    schema.context = context;
    schema.target_object.uuid = api::GenerateCrudEngineUuid("object");
    schema.target_object.object_kind = "schema";
    schema.localized_names.push_back({"en", "primary", "", "fixture_schema", true});
    const auto created = api::EngineCatalogCreateObject(schema);
    checked(created);
    context.current_schema_uuid = schema.target_object.uuid;
  }
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) throw std::runtime_error("fixture datatype catalog unavailable");
  table.bound_columns.clear();
  // One publication uses one immutable request/cohort. Resolve the explicitly
  // selected text profile once, not two full catalog reads for every column.
  // The publication API still revalidates the supplied descriptor authority.
  std::optional<std::pair<api::EngineUuid, api::EngineUuid>> binary_utf8_resources;
  for (std::size_t i = 0; i < canonical_types.size(); ++i) {
    const auto canonical_type = dt::CanonicalTypeIdFromStableName(canonical_types[i]);
    const auto row = dt::LookupDatatypeCatalogRow(manifest.manifest,
        canonical_type);
    if (!row.ok() || row.manifest.descriptor_rows.size() != 1)
      throw std::invalid_argument("fixture datatype is not in the engine catalog");
    const auto& datatype = row.manifest.descriptor_rows.front();
    const auto binding = dt::LookupDatatypeTypeCodecIdentityV1(
        context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
        context.datatype_registry_generation, datatype.descriptor_uuid.value,
        datatype.descriptor_epoch);
    if (!binding.ok) throw std::invalid_argument("fixture datatype has no admitted codec");
    api::EngineColumnDefinition column;
    column.ordinal = i;
    column.requested_column_uuid = api::GenerateCrudEngineUuid("object");
    column.names.push_back({"en", "primary", "", table.columns[i].first, true});
    column.descriptor.descriptor_uuid = api::GenerateCrudEngineUuid("object");
    column.descriptor.descriptor_kind = "scalar";
    column.descriptor.canonical_type_name = canonical_types[i];
    column.descriptor.datatype_descriptor_uuid = binding.row.descriptor_uuid;
    column.descriptor.datatype_descriptor_generation = binding.row.descriptor_generation;
    column.descriptor.type_uuid = binding.row.type_uuid;
    column.descriptor.encoded_descriptor = table.columns[i].second;
    api::CatalogColumnMetadata attributes;
    if (!api::AdmitCatalogColumnMetadata(table.columns[i].second, &attributes))
      throw std::invalid_argument("fixture column metadata is invalid");
    // These low-level fixtures select binary UTF8 semantics explicitly. Resolve
    // real catalog resources before publishing the descriptor; an engine index
    // writer must never infer collation from a bare character type label.
    if (canonical_type == dt::CanonicalTypeId::character &&
        attributes.identities.find("charset_uuid") == attributes.identities.end() &&
        attributes.identities.find("collation_uuid") == attributes.identities.end()) {
      if (!binary_utf8_resources) {
        const auto charset = api::LookupEngineResourceDescriptorByName(context, "UTF8", "charset");
        const auto collation = api::LookupEngineResourceDescriptorByName(context, "SB_UTF8_BINARY", "collation");
        if (!charset.ok || !charset.resource_descriptor.present ||
            !collation.ok || !collation.resource_descriptor.present ||
            collation.resource_descriptor.parent_resource_uuid != charset.resource_descriptor.resource_uuid)
          throw std::invalid_argument("fixture requires real UTF8 binary collation resources");
        binary_utf8_resources.emplace(charset.resource_descriptor.resource_uuid,
                                      collation.resource_descriptor.resource_uuid);
      }
      attributes.identities["charset_uuid"] = binary_utf8_resources->first;
      attributes.identities["collation_uuid"] = binary_utf8_resources->second;
      // Preserve explicit test bounds. Previously unbounded fixture columns
      // use the admitted codec capacity, not a guessed SQL default or the
      // wider integer field used to carry descriptor limits.
      if (attributes.text.find("character_length") == attributes.text.end() &&
          attributes.text.find("text_resource_storage") == attributes.text.end()) {
        if (!binding.row.canonical_value_maximum_bytes)
          throw std::invalid_argument("fixture text codec capacity is unavailable");
        attributes.text["character_length"] =
            std::to_string(binding.row.canonical_value_maximum_bytes);
      }
      if (!api::EncodeCatalogColumnMetadata(attributes, &table.columns[i].second))
        throw std::invalid_argument("fixture bound text metadata encoding failed");
      column.descriptor.encoded_descriptor = table.columns[i].second;
    }
    // Preserve actual catalog-resolved identities explicitly supplied by the
    // fixture. A type label alone does not grant collation authority.
    const auto charset = attributes.identities.find("charset_uuid");
    const auto collation = attributes.identities.find("collation_uuid");
    if (charset != attributes.identities.end())
      column.descriptor.charset_uuid = charset->second;
    if (collation != attributes.identities.end())
      column.descriptor.collation_uuid = collation->second;
    const auto boolean_attribute = [&](const char* key) -> std::optional<bool> {
      const auto it = attributes.text.find(key);
      if (it == attributes.text.end()) return std::nullopt;
      if (it->second == "true") return true;
      if (it->second == "false") return false;
      throw std::invalid_argument("fixture column boolean attribute is invalid");
    };
    const auto nullable = boolean_attribute("nullable");
    const auto not_null = boolean_attribute("not_null");
    const bool primary_key = boolean_attribute("primary_key").value_or(false) ||
        boolean_attribute("pk").value_or(false);
    if ((nullable && not_null && *nullable == *not_null) ||
        (primary_key && ((nullable && *nullable) || (not_null && !*not_null))))
      throw std::invalid_argument("fixture column nullability attributes conflict");
    column.nullable = primary_key ? false : nullable.value_or(!not_null.value_or(false));
    table.bound_columns.push_back(std::move(column));
  }
  api::EngineCatalogCreateObjectRequest request;
  request.context = context;
  request.target_object.uuid = table.table_uuid;
  request.target_object.object_kind = "table";
  request.target_schema.uuid = context.current_schema_uuid;
  request.localized_names.push_back({"en", "primary", "", table.default_name, true});
  request.columns = table.bound_columns;
  const auto published = api::EngineCatalogCreateObject(request);
  checked(published);
  table.bound_relation_generation = published.bound_object_identity.object_descriptor_generation;
  table.bound_column_generation = published.metadata_cache_epoch;
  const auto appended = api::AppendMgaTableMetadata(context, table);
  if (appended.error) return appended;
  api::MgaRelationStorageDescriptor descriptor;
  return api::EnsureMgaRelationStorageDescriptor(context, table, indexes, &descriptor);
}
} // namespace scratchbird::tests
