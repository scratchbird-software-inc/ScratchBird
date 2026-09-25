// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_durable_catalog_store_api.hpp"

// SEARCH_KEY: AEIC_DURABLE_AGENT_CATALOG_ENTERPRISE
// SEARCH_KEY: AEIC_DURABLE_AGENT_MANAGEMENT_SURFACES

#include "crud_support/crud_store.hpp"
#include "catalog/schema_tree_api.hpp"
#include "catalog/datatype_bootstrap_identity.hpp"
#include "ddl/create_api.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"
#include "database_format.hpp"
#include "disk_device.hpp"
#include "startup_state.hpp"
#include "mga_relation_store/mga_relation_store.hpp"

#include <algorithm>
#include "uuid.hpp"
#include <optional>
#include <openssl/sha.h>
#include <sstream>
#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace agents = scratchbird::core::agents;

constexpr const char* kAgentCatalogRecordKind = "agent_catalog_image";
std::string IdentityBytes(const EngineUuid& id) {
  return {reinterpret_cast<const char*>(id.bytes.data()), id.bytes.size()};
}
bool ReadIdentity(std::string_view bytes, EngineUuid* id) {
  if (bytes.size() != id->bytes.size()) return false;
  std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.data()), id->bytes.size(), id->bytes.begin());
  return core::uuid::IsEngineIdentityUuid(*id);
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

AgentDurableCatalogStoreResult ErrorResult(std::string detail) {
  AgentDurableCatalogStoreResult result;
  result.diagnostic =
      MakeInvalidRequestDiagnostic("agent.durable_catalog_store", std::move(detail));
  return result;
}

AgentDurableCatalogStoreResult ErrorResult(EngineApiDiagnostic diagnostic) {
  AgentDurableCatalogStoreResult result;
  result.diagnostic = std::move(diagnostic);
  return result;
}

std::string HexBytes(const unsigned char* bytes, std::size_t size) {
  std::ostringstream out;
  out << std::hex;
  for (std::size_t i = 0; i < size; ++i) {
    const unsigned int value = bytes[i];
    if (value < 16) { out << '0'; }
    out << value;
  }
  return out.str();
}

std::string Sha256Hex(const std::string& value) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
  return HexBytes(digest, SHA256_DIGEST_LENGTH);
}

std::vector<std::pair<std::string, std::string>> CatalogColumns() {
  return {{"record_kind", "text:not_null"},
          {"catalog_root_digest", "text:not_null"},
          {"encoded_catalog_image", "binary:not_null"},
          {"catalog_generation", "uint64:not_null"},
          {"authority_evidence_uuid", "uuid:not_null"},
          {"storage_commit_evidence_uuid", "uuid:not_null"},
          {"storage_linkage_digest", "text:not_null"}};
}

std::optional<CrudTableRecord> FindCatalogTable(const RelationReadSnapshot& state,
                                                const EngineRequestContext& context) {
  for (const auto& table : state.tables) {
    if (table.default_name != kAgentDurableCatalogStoreTableName) { continue; }
    if (!CrudCreatorVisible(state,
                            table.creator_tx,
                            table.event_sequence,
                            context.local_transaction_id)) {
      continue;
    }
    return table;
  }
  return std::nullopt;
}

AgentDurableCatalogStoreResult EnsureCatalogTable(const EngineRequestContext& context) {
  if (context.database_path.empty()) {
    return ErrorResult("database_path_required");
  }
  auto loaded = LoadMgaRelationStoreState(context);
  if (!loaded.ok) { return ErrorResult(std::move(loaded.diagnostic)); }
  const RelationReadSnapshot state = BuildCrudCompatibilityStateFromMga(loaded.state);
  const auto existing = FindCatalogTable(state, context);
  CrudTableRecord table;
  if (existing) {
    table = *existing;
  } else {
    // Publish the system relation through DDL so its column cohort and
    // descriptor generations come from the same catalog as user relations.
    EngineCreateTableRequest create;
    create.context = context;
    EngineApiDiagnostic schema_diagnostic;
    const auto schemas = VisibleSchemaTreeRecords(
        context, context.local_transaction_id, schema_diagnostic);
    if (schema_diagnostic.error) return ErrorResult(schema_diagnostic);
    for (const auto& schema : schemas)
      for (const auto& name : schema.localized_names)
        if (name.path == "sys") create.target_schema.uuid = schema.schema_uuid;
    if (create.target_schema.uuid.is_nil()) return ErrorResult("system_schema_unavailable");
    create.target_schema.object_kind = "schema";
    create.context.current_schema_uuid = create.target_schema.uuid;
    // Read the owning filespace identity without reopening lifecycle recovery
    // while this catalog transaction is active.
    namespace disk = scratchbird::storage::disk;
    disk::FileDevice device;
    if (!device.Open(context.database_path, disk::FileOpenMode::open_existing_read_only).ok())
      return ErrorResult("database_identity_read_failed");
    disk::SerializedDatabaseHeader header_bytes{};
    if (!device.ReadAt(0, header_bytes.data(), header_bytes.size()).ok())
      return ErrorResult("database_header_read_failed");
    const auto header = disk::ParseDatabaseHeader(header_bytes);
    if (!header.ok() || header.header.database_uuid != context.database_uuid)
      return ErrorResult("database_header_identity_invalid");
    const auto startup = scratchbird::storage::database::ReadStartupStatePageBody(
        &device, header.header.page_size);
    if (!startup.ok() || startup.state.database_uuid.value != context.database_uuid ||
        !startup.state.first_filespace_uuid.valid())
      return ErrorResult("database_filespace_authority_unavailable");
    create.context.default_root_uuid = startup.state.first_filespace_uuid.value;
    // The identity read is complete. DDL acquires its own device ownership;
    // retaining this exclusive read handle would refuse embedded callers.
    device.Close();
    namespace types = scratchbird::core::datatypes;
    const auto manifest = types::LoadCurrentCoreDatatypeCatalogManifest();
    if (!manifest.ok()) return ErrorResult("datatype_catalog_unavailable");
    // This system relation is an engine bootstrap publication. Use the exact
    // bootstrap cohort shared with the receipt issuer, never a registry row
    // selected by enumeration order or a caller-supplied default.
    create.context.datatype_catalog_snapshot_uuid = kBootstrapDatatypeCatalogUuid;
    create.context.datatype_catalog_generation = kBootstrapDatatypeCatalogGeneration;
    create.context.datatype_registry_generation = kBootstrapDatatypeRegistryGeneration;
    create.requested_table_uuid = GenerateCrudEngineUuid("object");
    EngineLocalizedName name;
    name.language_tag = "en";
    name.name_class = "primary";
    name.path = kAgentDurableCatalogStoreTableName;
    name.name = kAgentDurableCatalogStoreTableName;
    name.default_name = true;
    create.table_names.push_back(name);
    for (const auto& [column_name, shape] : CatalogColumns()) {
      const auto type_name = shape.substr(0, shape.find(':'));
      const auto type = types::CanonicalTypeIdFromStableName(type_name);
      const auto row = types::LookupDatatypeCatalogRow(manifest.manifest, type);
      if (!row.ok() || row.manifest.descriptor_rows.size() != 1)
        return ErrorResult("catalog_column_datatype_unavailable");
      const auto& catalog = row.manifest.descriptor_rows.front();
      EngineColumnDefinition column;
      column.ordinal = create.table_columns.size();
      column.nullable = false;
      column.requested_column_uuid = GenerateCrudEngineUuid("object");
      name.name = column_name;
      name.path = column_name;
      column.names.push_back(name);
      column.descriptor.descriptor_uuid = GenerateCrudEngineUuid("object");
      column.descriptor.descriptor_kind = "scalar";
      column.descriptor.canonical_type_name = type_name;
      column.descriptor.encoded_descriptor = "type=" + type_name + ";nullable=false";
      column.descriptor.datatype_descriptor_uuid = catalog.descriptor_uuid.value;
      column.descriptor.datatype_descriptor_generation = catalog.descriptor_epoch;
      column.descriptor.type_uuid = catalog.descriptor_uuid.value;
      const auto codec = types::LookupDatatypeTypeCodecIdentityV1(
          create.context.datatype_catalog_snapshot_uuid,
          create.context.datatype_catalog_generation,
          create.context.datatype_registry_generation,
          catalog.descriptor_uuid.value, catalog.descriptor_epoch);
      // The binary image and uint64 counters retain the existing core-manifest
      // bindings used by this engine-owned system relation. UUID and TEXT use
      // the exact admitted descriptor/type/codec tuple; never alias their IDs.
      if (!codec.ok && type != types::CanonicalTypeId::binary &&
          type != types::CanonicalTypeId::uint64)
        return ErrorResult("catalog_column_codec_unavailable");
      if (codec.ok) column.descriptor.type_uuid = codec.row.type_uuid;
      create.table_columns.push_back(std::move(column));
    }
    const auto created = EngineCreateTable(create);
    if (!created.ok)
      return created.diagnostics.empty() ? ErrorResult("catalog_table_publication_failed")
                                        : ErrorResult(created.diagnostics.front());
    loaded = LoadMgaRelationStoreState(context);
    if (!loaded.ok) return ErrorResult(loaded.diagnostic);
    const auto published = FindCatalogTable(BuildCrudCompatibilityStateFromMga(loaded.state), context);
    if (!published || published->table_uuid != created.table_object.uuid)
      return ErrorResult("catalog_table_publication_not_visible");
    table = *published;
  }

  // The durable agent catalog is an MGA relation, not an exceptional
  // descriptorless metadata row.  Persist its relation descriptor through
  // the same engine authority used by every other visible relation so
  // catalog-wide UUID inventories can remain fail-closed.  Running this for
  // an existing table also repairs databases created before the descriptor
  // invariant was enforced.
  MgaRelationStorageDescriptor descriptor;
  const auto descriptor_status = EnsureMgaRelationStorageDescriptor(
      context, table, {}, &descriptor);
  if (descriptor_status.error) {
    return ErrorResult(descriptor_status);
  }

  AgentDurableCatalogStoreResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.table_uuid = IdentityBytes(table.table_uuid);
  return result;
}

std::optional<CrudRowVersionRecord> LatestCatalogRow(const RelationReadSnapshot& state,
                                                     const EngineRequestContext& context,
                                                     const EngineUuid& table_uuid) {
  std::optional<CrudRowVersionRecord> latest;
  for (const auto& row : VisibleCrudRowsForContext(state, table_uuid, context)) {
    if (row.deleted) { continue; }
    if (CrudFieldValue(row.values, "record_kind") != kAgentCatalogRecordKind) {
      continue;
    }
    if (!latest || row.sequence > latest->sequence) {
      latest = row;
    }
  }
  return latest;
}

std::string MissingCatalogRowDetail(const RelationReadSnapshot& state,
                                    const EngineRequestContext& context,
                                    const EngineUuid& table_uuid) {
  std::size_t table_row_versions = 0;
  std::size_t matching_catalog_rows = 0;
  std::ostringstream detail;
  detail << "catalog_image_not_found"
         << ":row_versions=" << state.row_versions.size()
         << ":context_tx=" << context.local_transaction_id
         << ":snapshot_tx="
         << context.snapshot_visible_through_local_transaction_id;
  for (const auto& row : state.row_versions) {
    if (row.table_uuid != table_uuid) { continue; }
    ++table_row_versions;
    if (CrudFieldValue(row.values, "record_kind") == kAgentCatalogRecordKind) {
      ++matching_catalog_rows;
      const auto tx = state.transactions.find(row.creator_tx);
      detail << ":catalog_row_tx=" << row.creator_tx
             << ":catalog_row_seq=" << row.sequence
             << ":catalog_row_state="
             << (tx == state.transactions.end() ? "missing" : tx->second)
             << ":catalog_row_kind="
             << CrudFieldValue(row.values, "record_kind");
    }
  }
  const auto visible = VisibleCrudRowsForContext(state, table_uuid, context);
  detail << ":table_row_versions=" << table_row_versions
         << ":matching_catalog_rows=" << matching_catalog_rows
         << ":visible_rows=" << visible.size();
  return detail.str();
}

}  // namespace

AgentDurableCatalogStoreResult PersistAgentDurableCatalogImage(
    const AgentDurableCatalogStoreRequest& request) {
  if (request.production_live_path && !request.fsync_or_checkpoint_evidence) {
    return ErrorResult("fsync_or_checkpoint_evidence_required");
  }
  EngineUuid evidence_uuid;
  if (!ReadIdentity(request.evidence_uuid, &evidence_uuid)) {
    return ErrorResult("evidence_uuid_binary16_required");
  }
  if (request.context.local_transaction_id == 0 ||
      request.context.transaction_uuid.is_nil()) {
    return ErrorResult("mga_transaction_context_required");
  }

  auto table = EnsureCatalogTable(request.context);
  if (!table.ok) { return table; }

  auto loaded = LoadMgaRelationStoreState(request.context);
  if (!loaded.ok) { return ErrorResult(std::move(loaded.diagnostic)); }
  const RelationReadSnapshot state = BuildCrudCompatibilityStateFromMga(loaded.state);
  EngineUuid table_uuid;
  if (!ReadIdentity(table.table_uuid, &table_uuid)) return ErrorResult("catalog_table_identity_invalid");
  const auto previous = LatestCatalogRow(state, request.context, table_uuid);
  if (previous) {
    const std::string previous_root =
        CrudFieldValue(previous->values, "catalog_root_digest");
    const bool source_matches =
        !previous_root.empty() &&
        (previous_root == request.expected_catalog_root_digest ||
         previous_root == request.image.authority.catalog_root_digest ||
         previous_root == request.image.authority.previous_catalog_root_digest);
    if (!source_matches) {
      return ErrorResult("catalog_root_digest_stale_write_refused");
    }
  }

  agents::DurableAgentCatalogImage image = request.image;
  if (!request.expected_catalog_root_digest.empty()) {
    image.authority.catalog_root_digest = request.expected_catalog_root_digest;
  }
  image.source = agents::AgentCatalogStateSource::durable_catalog_image;
  image.authority.durable_catalog_authority = true;
  image.authority.mga_transaction_evidence = true;
  image.authority.mga_transaction_uuid = IdentityBytes(request.context.transaction_uuid);
  image.authority.database_uuid = IdentityBytes(request.context.database_uuid);
  image.authority.catalog_storage_uuid = table.table_uuid;
  image.authority.local_transaction_id = request.context.local_transaction_id;
  image.authority.storage_catalog_record_evidence = true;
  image.authority.transaction_inventory_bound = true;
  image.authority.fsync_or_checkpoint_evidence =
      request.fsync_or_checkpoint_evidence;
  image.authority.sidecar_storage = false;
  image.authority.in_memory_only = false;
  if (image.authority.transaction_generation == 0) {
    image.authority.transaction_generation = request.context.local_transaction_id;
  }

  const auto refreshed =
      agents::RefreshDurableAgentCatalogAuthorityDigest(&image, request.evidence_uuid);
  if (!refreshed.ok) { return ErrorResult(refreshed.diagnostic_code); }

  const std::string encoded = agents::SerializeDurableAgentCatalogImage(image);
  if (encoded.empty()) return ErrorResult("SB_AGENT_CATALOG.IMAGE_FIELD_INVALID");
  const EngineUuid row_uuid = previous ? previous->row_uuid : GenerateCrudEngineUuid("row");
  if (row_uuid.is_nil()) return ErrorResult("catalog_row_identity_allocation_failed");
  const std::string storage_linkage =
      Sha256Hex(table.table_uuid + IdentityBytes(row_uuid) +
                image.authority.catalog_root_digest + request.evidence_uuid);

  CrudRowVersionRecord row;
  row.creator_tx = request.context.local_transaction_id;
  row.creator_transaction_uuid = request.context.transaction_uuid;
  row.table_uuid = table_uuid;
  row.row_uuid = row_uuid;
  row.version_uuid = GenerateCrudEngineUuid("row");
  if (previous) {
    row.previous_version_uuid = previous->version_uuid;
    row.previous_sequence = previous->sequence;
  }
  row.values = {{"record_kind", kAgentCatalogRecordKind},
                {"catalog_root_digest", image.authority.catalog_root_digest},
                {"encoded_catalog_image", encoded},
                {"catalog_generation",
                 std::to_string(image.authority.catalog_generation)},
                {"authority_evidence_uuid", image.authority.evidence_uuid},
                {"storage_commit_evidence_uuid",
                 image.authority.storage_commit_evidence_uuid},
                {"storage_linkage_digest", storage_linkage}};

  std::uint64_t event_sequence = 0;
  const auto appended = AppendMgaRowVersion(request.context, row, &event_sequence);
  if (appended.error) { return ErrorResult(appended); }

  AgentDurableCatalogStoreResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.image = std::move(image);
  result.table_uuid = table.table_uuid;
  result.row_uuid = IdentityBytes(row.row_uuid);
  result.version_uuid = IdentityBytes(row.version_uuid);
  result.row_event_sequence = event_sequence;
  result.storage_linkage_digest = storage_linkage;
  return result;
}

AgentDurableCatalogStoreResult LoadAgentDurableCatalogImage(
    const EngineRequestContext& context,
    bool production_live_path) {
  AgentDurableCatalogLoadRequest request;
  request.context = context;
  request.production_live_path = production_live_path;
  return LoadAgentDurableCatalogImage(request);
}

AgentDurableCatalogStoreResult LoadAgentDurableCatalogImage(
    const AgentDurableCatalogLoadRequest& request) {
  const EngineRequestContext& context = request.context;
  if (context.database_path.empty()) {
    return ErrorResult("database_path_required");
  }
  auto loaded = LoadMgaRelationStoreState(context);
  if (!loaded.ok) { return ErrorResult(std::move(loaded.diagnostic)); }
  const RelationReadSnapshot state = BuildCrudCompatibilityStateFromMga(loaded.state);
  const auto table = FindCatalogTable(state, context);
  if (!table) { return ErrorResult("catalog_table_not_found"); }
  const auto latest = LatestCatalogRow(state, context, table->table_uuid);
  if (!latest) {
    return ErrorResult(MissingCatalogRowDetail(state, context, table->table_uuid));
  }

  const std::string encoded = CrudFieldValue(latest->values, "encoded_catalog_image");
  auto validation =
      agents::ValidateDurableAgentCatalogImage(encoded, request.production_live_path);
  if (!validation.status.ok) {
    return ErrorResult(validation.status.diagnostic_code);
  }
  const std::string expected_root =
      CrudFieldValue(latest->values, "catalog_root_digest");
  const std::string validated_source_root =
      validation.migrated && !validation.image.migrations.empty()
          ? validation.image.migrations.back().source_root_digest
          : validation.image.authority.catalog_root_digest;
  if (expected_root.empty() || expected_root != validated_source_root) {
    return ErrorResult("catalog_root_digest_record_mismatch");
  }

  if (validation.migrated && request.persist_schema_migration) {
    if (request.production_live_path && !request.fsync_or_checkpoint_evidence) {
      return ErrorResult("fsync_or_checkpoint_evidence_required");
    }
    AgentDurableCatalogStoreRequest persist;
    persist.context = context;
    persist.image = validation.image;
    persist.evidence_uuid = request.migration_evidence_uuid.empty()
                                ? validation.image.authority.evidence_uuid
                                : request.migration_evidence_uuid;
    persist.production_live_path = request.production_live_path;
    persist.fsync_or_checkpoint_evidence =
        request.fsync_or_checkpoint_evidence;
    auto persisted = PersistAgentDurableCatalogImage(persist);
    if (!persisted.ok) { return persisted; }
    persisted.schema_migration_applied = true;
    persisted.schema_migration_persisted = true;
    return persisted;
  }

  AgentDurableCatalogStoreResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.image = std::move(validation.image);
  result.table_uuid = IdentityBytes(table->table_uuid);
  result.row_uuid = IdentityBytes(latest->row_uuid);
  result.version_uuid = IdentityBytes(latest->version_uuid);
  result.row_event_sequence = latest->sequence;
  result.storage_linkage_digest =
      CrudFieldValue(latest->values, "storage_linkage_digest");
  result.schema_migration_applied = validation.migrated;
  return result;
}

}  // namespace scratchbird::engine::internal_api
