// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/schema_tree_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "bootstrap_schema_roots.hpp"
#include "catalog_record_codec.hpp"
#include "catalog_schema_record_codec.hpp"
#include "catalog_records.hpp"
#include "catalog_page.hpp"
#include "catalog/name_registry.hpp"
#include "crud_support/crud_store.hpp"
#include "database_format.hpp"
#include "disk_device.hpp"
#include "domain_support/domain_store.hpp"
#include "page_manager.hpp"
#include "startup_state.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <map>
#include <array>
#include <filesystem>
#include <set>
#include <sstream>

namespace scratchbird::engine::internal_api {
namespace {

std::optional<EngineSchemaTreeRecord> SchemaTreeRecordFromApiRecord(const ApiBehaviorRecord& record) {
  EngineSchemaTreeRecord schema;
  schema.creator_tx = record.creator_tx;
  schema.event_sequence = record.event_sequence;
  schema.schema_uuid = record.object_uuid;
  schema.parent_schema_uuid=record.target_schema_uuid;
  schema.default_name = record.default_name;
  schema.payload = record.payload;
  schema.state = record.state;
  if (!DecodeSchemaTreeMetadata(record.payload,&schema.localized_names,&schema.localized_comments)) return std::nullopt;
  return schema;
}

bool NameIndicatesClusterPath(const EngineLocalizedName& name) {
  return name.path == "cluster" || name.path.starts_with("cluster.") || name.path.starts_with("cluster/");
}

using SchemaNameKey=std::tuple<std::string,std::string,bool,EngineUuid,std::string>;
SchemaNameKey NameConflictKey(const EngineLocalizedName& name,const EngineUuid& parent_schema_uuid){
  const auto language=name.language_tag.empty()?std::string("und"):name.language_tag;
  const auto name_class=name.name_class.empty()?std::string("default"):name.name_class;
  return {language,name_class,!name.path.empty(),name.path.empty()?parent_schema_uuid:EngineUuid{},name.path.empty()?name.name:name.path};
}

EngineApiDiagnostic SchemaReadError(const std::string& detail) {
  return MakeInvalidRequestDiagnostic("catalog.schema_tree.read", detail);
}

std::vector<scratchbird::storage::page::CatalogPageRow> ReadBootstrapCatalogRows(
    const EngineRequestContext& context, EngineApiDiagnostic& diagnostic) {
  diagnostic = SchemaReadError("bootstrap_read_incomplete");
  std::vector<scratchbird::storage::page::CatalogPageRow> rows;
  const auto fail = [&](const char* reason) {
    diagnostic = SchemaReadError(reason);
    return std::vector<scratchbird::storage::page::CatalogPageRow>{};
  };
  if (context.database_path.empty()) return fail("database_path_required");
  std::error_code ec;
  const auto status = std::filesystem::status(context.database_path, ec);
  if (ec || !std::filesystem::is_regular_file(status)) return fail("database_not_readable_regular_file");
  scratchbird::storage::disk::FileDevice device;
  const auto opened = device.Open(context.database_path, scratchbird::storage::disk::FileOpenMode::open_existing_read_only);
  if (!opened.ok()) return fail("database_open_failed");
  scratchbird::storage::disk::SerializedDatabaseHeader serialized{};
  if (!device.ReadAt(0, serialized.data(), serialized.size()).ok()) return fail("database_header_read_failed");
  const auto parsed_header = scratchbird::storage::disk::ParseDatabaseHeader(serialized);
  if (!parsed_header.ok()) return fail("database_header_invalid");
  if (!context.database_uuid.is_nil()) {
    if (context.database_uuid.bytes != parsed_header.header.database_uuid.bytes)
      return fail("database_identity_mismatch");
  }
  const auto extent = device.Size();
  if (!extent.ok() || parsed_header.header.page_size == 0 ||
      extent.size_bytes % parsed_header.header.page_size != 0) return fail("catalog_extent_invalid");
  const auto page_count = extent.size_bytes / parsed_header.header.page_size;
  std::uint64_t page_number = scratchbird::storage::database::kCatalogPageNumber;
  std::set<std::uint64_t> visited;
  while (page_number != 0) {
    if (page_number >= page_count || !visited.insert(page_number).second)
      return fail("catalog_chain_cycle_or_limit");
    const auto body_offset = scratchbird::storage::page::CheckedPageBodyOffset(
        parsed_header.header.page_size, page_number, scratchbird::storage::disk::kPageHeaderSerializedBytes);
    if (!body_offset.ok()) return fail("catalog_page_offset_invalid");
    scratchbird::storage::disk::SerializedPageHeader page_header_bytes{};
    if (!device.ReadAt(body_offset.offset - scratchbird::storage::disk::kPageHeaderSerializedBytes,
                       page_header_bytes.data(), page_header_bytes.size()).ok())
      return fail("catalog_page_header_read_failed");
    const auto page_header = scratchbird::storage::disk::ParsePageHeader(page_header_bytes);
    if (!page_header.ok() || page_header.header.page_type != scratchbird::storage::disk::PageType::catalog ||
        page_header.header.page_number != page_number ||
        page_header.header.page_size != parsed_header.header.page_size ||
        page_header.header.database_uuid != parsed_header.header.database_uuid)
      return fail("catalog_page_header_invalid");
    std::vector<scratchbird::core::platform::byte> body(
        parsed_header.header.page_size - scratchbird::storage::disk::kPageHeaderSerializedBytes);
    if (!device.ReadAt(body_offset.offset, body.data(), body.size()).ok())
      return fail("catalog_page_read_failed");
    const auto parsed_body = scratchbird::storage::page::ParseCatalogPageBody(body, page_number);
    if (!parsed_body.ok()) return fail("catalog_page_invalid");
    rows.insert(rows.end(), parsed_body.body.rows.begin(), parsed_body.body.rows.end());
    page_number = parsed_body.body.next_page_number;
  }
  diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return rows;
}

std::vector<EngineSchemaTreeRecord> BootstrapSchemaTreeRecords(
    const EngineRequestContext& context, EngineApiDiagnostic& diagnostic) {
  std::vector<EngineSchemaTreeRecord> schemas;
  const auto rows = ReadBootstrapCatalogRows(context, diagnostic);
  if (diagnostic.error) return {};
  std::vector<scratchbird::core::catalog::CatalogTypedRecord> graph_records;
  for (const auto& row : rows) {
    if (row.kind != scratchbird::storage::page::CatalogPageRowKind::typed_catalog_record) continue;
    const auto decoded = scratchbird::core::catalog::DecodeCatalogTypedRecord(row);
    if (!decoded.ok()) { diagnostic = SchemaReadError("catalog_record_invalid"); return {}; }
    graph_records.push_back(decoded.record);
    if (decoded.record.header.kind != scratchbird::core::catalog::CatalogRecordKind::schema) continue;
    if (!scratchbird::core::catalog::CatalogSchemaPayloadMatchesHeader(decoded.record)) {
      diagnostic = SchemaReadError("schema_binary_payload_or_header_invalid"); return {};
    }
    const auto payload = scratchbird::core::catalog::DecodeCatalogSchemaRecord(decoded.record.payload);
    const auto& r = *payload.record;
    EngineSchemaTreeRecord schema;
    schema.creator_tx = r.creator_transaction_number;
    schema.schema_uuid = r.schema_object_uuid.value;
    if (!r.root_schema)
      schema.parent_schema_uuid = r.parent_object_uuid.value;
    schema.default_name = r.name_cache;
    schema.localized_names.push_back({"en", "default", r.path_cache, r.name_cache, true});
    schema.payload = SchemaTreePayload(schema.parent_schema_uuid, schema.localized_names, {});
    schema.state = "active";
    schemas.push_back(std::move(schema));
  }
  if (!scratchbird::core::catalog::ValidateCatalogSchemaGraph(graph_records)) {
    diagnostic = SchemaReadError("schema_binary_parent_graph_invalid"); return {};
  }
  return schemas;
}

}  // namespace

std::string SchemaTreeDefaultName(const std::vector<EngineLocalizedName>& names, const std::string& fallback) {
  for (const auto& name : names) {
    if (name.default_name && !name.name.empty()) { return name.name; }
  }
  for (const auto& name : names) {
    if (!name.name.empty()) { return name.name; }
  }
  return fallback;
}

std::string SchemaTreePayload(const EngineUuid& parent_schema_uuid,
                              const std::vector<EngineLocalizedName>& names,
                              const std::vector<std::pair<std::string,std::string>>& comments,
                              BinaryCatalogMetadata extensions) {
  (void)parent_schema_uuid; // Parent identity lives in the native API record.
  std::string bytes;
  if (!EncodeSchemaTreeMetadata(names,comments,std::move(extensions),&bytes)) return {};
  return bytes;
}

std::vector<EngineSchemaTreeRecord> VisibleSchemaTreeRecords(const EngineRequestContext& context,
                                                             std::uint64_t observer_tx,
                                                             EngineApiDiagnostic& diagnostic) {
  auto schemas = BootstrapSchemaTreeRecords(context, diagnostic);
  if (diagnostic.error) return {};
  EngineRequestContext effective = context;
  if (effective.local_transaction_id == 0) effective.local_transaction_id = observer_tx;
  const auto loaded = LoadApiBehaviorState(effective);
  if (!loaded.ok) { diagnostic = loaded.diagnostic; return {}; }
  for (const auto& record : loaded.state.records) {
    if (record.object_kind != "schema" || record.state != "active") continue;
    auto decoded = SchemaTreeRecordFromApiRecord(record);
    if (!decoded) { diagnostic=SchemaReadError("invalid_binary_schema_metadata");return {}; }
    auto schema = std::move(*decoded);
    const auto existing = std::find_if(schemas.begin(), schemas.end(), [&schema](const EngineSchemaTreeRecord& candidate) {
      return candidate.schema_uuid == schema.schema_uuid;
    });
    if (existing == schemas.end()) schemas.push_back(std::move(schema));
  }
  return schemas;
}

std::optional<EngineSchemaTreeRecord> FindVisibleSchemaTreeRecord(const EngineRequestContext& context,
                                                                  const EngineUuid& schema_uuid,
                                                                  std::uint64_t observer_tx,
                                                                  EngineApiDiagnostic& diagnostic) {
  const auto schemas = VisibleSchemaTreeRecords(context, observer_tx, diagnostic);
  if (diagnostic.error) return {};
  for (const auto& schema : schemas) if (schema.schema_uuid == schema_uuid) return schema;
  return {};
}

std::optional<std::string> SchemaTreePathConflict(const EngineRequestContext& context,
                                                  const EngineUuid& schema_uuid,
                                                  const EngineUuid& parent_schema_uuid,
                                                  const std::vector<EngineLocalizedName>& names,
                                                  std::uint64_t observer_tx,
                                                  EngineApiDiagnostic& diagnostic) {
  const auto schemas = VisibleSchemaTreeRecords(context, observer_tx, diagnostic);
  if (diagnostic.error) return {};
  std::string registry_conflict;
  const bool conflict = NameRegistryWouldConflict(context, schema_uuid, "schema", parent_schema_uuid,
      names, observer_tx, &registry_conflict, diagnostic);
  if (diagnostic.error) return {};
  if (conflict) return registry_conflict;
  for (const auto& name : names) {
    if (NameIndicatesClusterPath(name) && !context.cluster_authority_available)
      return "cluster_schema_path_absent:" + name.path;
    const auto key = NameConflictKey(name, parent_schema_uuid);
    for (const auto& existing : schemas) {
      if (existing.schema_uuid == schema_uuid) continue;
      for (const auto& existing_name : existing.localized_names)
        if (NameConflictKey(existing_name, existing.parent_schema_uuid) == key)
          return name.path.empty() ? name.name : name.path;
    }
  }
  return {};
}

bool SchemaTreeWouldCreateCycle(const EngineRequestContext& context,
                                const EngineUuid& schema_uuid,
                                const EngineUuid& proposed_parent_schema_uuid,
                                std::uint64_t observer_tx,
                                EngineApiDiagnostic& diagnostic) {
  const auto schemas = VisibleSchemaTreeRecords(context, observer_tx, diagnostic);
  if (diagnostic.error) return false;
  std::set<EngineUuid> visited;
  EngineUuid cursor = proposed_parent_schema_uuid;
  while (!cursor.is_nil()) {
    if (cursor == schema_uuid) return true;
    if (!visited.insert(cursor).second) {
      diagnostic = SchemaReadError("existing_schema_parent_cycle"); return false;
    }
    const auto parent = std::find_if(schemas.begin(), schemas.end(), [&](const auto& s) { return s.schema_uuid == cursor; });
    if (parent == schemas.end()) return false;
    cursor = parent->parent_schema_uuid;
  }
  return false;
}

EngineApiDiagnostic PersistSchemaTreeRecord(const EngineRequestContext& context,
                                            const EngineSchemaTreeRecord& record,
                                            const std::string& operation_id) {
  std::vector<EngineLocalizedName> checked_names;
  std::vector<std::pair<std::string,std::string>> checked_comments;
  if (!DecodeSchemaTreeMetadata(record.payload,&checked_names,&checked_comments)) {
    return SchemaReadError("invalid_binary_schema_metadata");
  }
  ApiBehaviorRecord api_record;
  api_record.creator_tx = context.local_transaction_id;
  api_record.operation_id = operation_id;
  api_record.object_uuid = record.schema_uuid;
  api_record.target_database_uuid=context.database_uuid;
  api_record.target_schema_uuid=record.parent_schema_uuid;
  api_record.target_object_uuid=record.schema_uuid;
  api_record.object_kind = "schema";
  api_record.default_name = record.default_name;
  api_record.payload = record.payload;
  api_record.state = record.state.empty() ? "active" : record.state;
  api_record.deleted = false;
  return AppendApiBehaviorEvent(context, MakeApiBehaviorRecordEvent(api_record));
}

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CATALOG_SCHEMA_TREE_API_BEHAVIOR
EngineListCatalogChildrenResult EngineListCatalogChildren(const EngineListCatalogChildrenRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineListCatalogChildrenResult>(request.context, "catalog.list_children");
  const EngineUuid requested_parent = request.target_schema.uuid;
  EngineApiDiagnostic schema_diagnostic;
  const auto schemas = VisibleSchemaTreeRecords(request.context, request.context.local_transaction_id, schema_diagnostic);
  if (schema_diagnostic.error) return MakeApiBehaviorDiagnostic<EngineListCatalogChildrenResult>(
      request.context, "catalog.list_children", schema_diagnostic);
  for (const auto& schema : schemas) {
    if (!requested_parent.is_nil() && schema.parent_schema_uuid != requested_parent) { continue; }
    AddApiBehaviorRow(&result, {{"object_uuid", schema.schema_uuid},
                                {"object_kind", "schema"},
                                {"name", schema.default_name},
                                {"state", schema.state},
                                {"payload", schema.payload}});
  }
  const auto crud = LoadCrudState(request.context);
  if (!crud.ok) return MakeApiBehaviorDiagnostic<EngineListCatalogChildrenResult>(
      request.context, "catalog.list_children", crud.diagnostic);
  if (crud.ok) {
    for (const auto& table : crud.state.tables) {
      if (CrudCreatorVisible(crud.state, table.creator_tx, table.event_sequence, request.context.local_transaction_id)) {
        AddApiBehaviorRow(&result, {{"object_uuid", table.table_uuid},
                                    {"object_kind", "table"},
                                    {"name", table.default_name},
                                    {"state", "active"},
                                    {"payload", "crud_table=true"}});
      }
    }
  }
  const auto domains = LoadDomainState(request.context);
  if (!domains.ok) return MakeApiBehaviorDiagnostic<EngineListCatalogChildrenResult>(
      request.context, "catalog.list_children", domains.diagnostic);
  if (domains.ok) {
    for (const auto& domain : domains.domains) {
      const auto visible = FindVisibleDomain(request.context, domain.domain_uuid, request.context.local_transaction_id);
      if (visible) {
        AddApiBehaviorRow(&result, {{"object_uuid", visible->domain_uuid},
                                    {"object_kind", "domain"},
                                    {"name", visible->default_name},
                                    {"state", "active"},
                                    {"schema_uuid", visible->schema_uuid}, {"payload", "base_type=" + visible->base_canonical_type_name}});
      }
    }
  }
  const auto api_records = LoadApiBehaviorState(request.context);
  if (!api_records.ok) return MakeApiBehaviorDiagnostic<EngineListCatalogChildrenResult>(
      request.context, "catalog.list_children", api_records.diagnostic);
  for (const auto& record : api_records.state.records) {
    if (record.object_kind == "schema") { continue; }
    AddApiBehaviorRow(&result, {{"object_uuid", record.object_uuid},
                                {"object_kind", record.object_kind},
                                {"name", record.default_name},
                                {"state", record.state},
                                {"payload", record.payload}});
  }
  AddApiBehaviorEvidence(&result, "catalog_children", std::to_string(result.result_shape.rows.size()));
  return result;
}

}  // namespace scratchbird::engine::internal_api
