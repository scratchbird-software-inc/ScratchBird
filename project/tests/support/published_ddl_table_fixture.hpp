// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "catalog_column_binding_fixture.hpp"
#include "../../src/engine/internal_api/ddl/create_api.hpp"
#include "../../src/engine/internal_api/catalog/column_metadata_codec.hpp"
#include "../../src/engine/internal_api/catalog/name_resolution_api.hpp"

#include <optional>

namespace scratchbird::tests {

// Unlike a low-level row-store fixture, this publishes the complete DDL
// constraint and backing-index cohort. The returned table is read from the
// actual catalog in the caller's transaction, not reconstructed from input.
inline engine::internal_api::CrudTableRecord PublishDdlTableFixture(
    engine::internal_api::EngineRequestContext& context,
    const engine::internal_api::CrudTableRecord& definition,
    const std::vector<std::string>& canonical_types) {
  namespace api = engine::internal_api;
  namespace dt = core::datatypes;
  if (!context.local_transaction_id || canonical_types.size() != definition.columns.size())
    throw std::invalid_argument("DDL fixture requires a transaction and exact column types");
  const auto checked = [](const auto& result) {
    if (!result.ok) throw std::runtime_error(result.diagnostics.empty()
        ? "fixture DDL publication failed"
        : result.diagnostics.front().code + ":" + result.diagnostics.front().detail);
  };
  if (context.current_schema_uuid.is_nil()) {
    api::EngineCreateSchemaRequest schema;
    schema.context = context;
    schema.target_object.uuid = api::GenerateCrudEngineUuid("object");
    schema.target_object.object_kind = "schema";
    schema.localized_names.push_back({"en", "primary", "", "fixture_schema", true});
    checked(api::EngineCreateSchema(schema));
    context.current_schema_uuid = schema.target_object.uuid;
  }
  api::EngineCreateTableRequest request;
  request.context = context;
  request.target_schema.uuid = context.current_schema_uuid;
  request.target_schema.object_kind = "schema";
  request.requested_table_uuid = definition.table_uuid;
  request.table_names.push_back({"en", "primary", "", definition.default_name, true});
  // All columns belong to this one immutable publication context. Reuse the
  // actual selected resource identities only within this call; the engine
  // still freshly admits the complete descriptors before publication.
  std::optional<std::pair<api::EngineUuid, api::EngineUuid>> binary_utf8_resources;
  for (std::size_t i = 0; i < definition.columns.size(); ++i) {
    api::CatalogColumnMetadata attributes;
    if (!api::AdmitCatalogColumnMetadata(definition.columns[i].second, &attributes))
      throw std::invalid_argument("DDL fixture column metadata is malformed");
    api::EngineColumnDefinition column;
    column.ordinal = i;
    column.names.push_back({"en", "primary", "", definition.columns[i].first, true});
    column.descriptor.descriptor_kind = "scalar";
    column.descriptor.canonical_type_name = canonical_types[i];
    const auto is_true = [&](const char* name) {
      const auto found = attributes.text.find(name);
      return found != attributes.text.end() && found->second == "true";
    };
    column.nullable = !is_true("primary_key") && !is_true("pk") && !is_true("not_null");
    if (const auto found = attributes.text.find("nullable"); found != attributes.text.end())
      column.nullable = found->second == "true";
    attributes.text.erase("not_null");
    attributes.text["nullable"] = column.nullable ? "true" : "false";
    const auto type = dt::CanonicalTypeIdFromStableName(canonical_types[i]);
    BindFixtureColumnDatatype(context, type, column);
    if (type == dt::CanonicalTypeId::character) {
      column.descriptor.canonical_type_name = "text";
      attributes.text.erase("type");
      attributes.text["canonical"] = "text";
      // Fixtures explicitly select catalog-bound binary UTF8 semantics. The
      // engine must not infer text resource authority from a bare type name.
      if (attributes.identities.find("charset_uuid") == attributes.identities.end() &&
          attributes.identities.find("collation_uuid") == attributes.identities.end()) {
        if (!binary_utf8_resources) {
          const auto charset = api::LookupEngineResourceDescriptorByName(context, "UTF8", "charset");
          const auto collation = api::LookupEngineResourceDescriptorByName(context, "SB_UTF8_BINARY", "collation");
          if (!charset.ok || !charset.resource_descriptor.present ||
              !collation.ok || !collation.resource_descriptor.present ||
              collation.resource_descriptor.parent_resource_uuid != charset.resource_descriptor.resource_uuid)
            throw std::invalid_argument("DDL fixture requires real UTF8 binary collation resources");
          binary_utf8_resources.emplace(charset.resource_descriptor.resource_uuid,
                                        collation.resource_descriptor.resource_uuid);
        }
        attributes.identities["charset_uuid"] = binary_utf8_resources->first;
        attributes.identities["collation_uuid"] = binary_utf8_resources->second;
        // Preserve explicit bounds and storage profiles; previously unbounded
        // fixture text uses the admitted codec capacity, not the integer field
        // width or an arbitrary SQL default.
        if (attributes.text.find("character_length") == attributes.text.end() &&
            attributes.text.find("text_resource_storage") == attributes.text.end()) {
          const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
              context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
              context.datatype_registry_generation, column.descriptor.datatype_descriptor_uuid,
              column.descriptor.datatype_descriptor_generation);
          if (!identity.ok || !identity.row.canonical_value_maximum_bytes)
            throw std::invalid_argument("DDL fixture text codec capacity is unavailable");
          attributes.text["character_length"] =
              std::to_string(identity.row.canonical_value_maximum_bytes);
        }
      }
    }
    if (const auto found = attributes.identities.find("charset_uuid"); found != attributes.identities.end())
      column.descriptor.charset_uuid = found->second;
    if (const auto found = attributes.identities.find("collation_uuid"); found != attributes.identities.end())
      column.descriptor.collation_uuid = found->second;
    if (!api::EncodeCatalogColumnMetadata(attributes, &column.descriptor.encoded_descriptor))
      throw std::invalid_argument("DDL fixture column metadata cannot be encoded");
    request.table_columns.push_back(std::move(column));
  }
  checked(api::EngineCreateTable(request));
  const auto loaded = api::LoadMgaRelationStoreState(context);
  if (!loaded.ok) throw std::runtime_error("fixture published DDL table is unreadable");
  for (const auto& table : loaded.state.relation_metadata.tables)
    if (table.table_uuid == definition.table_uuid) return table;
  throw std::runtime_error("fixture DDL table was not published");
}

} // namespace scratchbird::tests
