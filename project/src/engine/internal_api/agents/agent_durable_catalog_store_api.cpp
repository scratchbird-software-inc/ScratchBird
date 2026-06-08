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
#include "mga_relation_store/mga_relation_store.hpp"

#include <algorithm>
#include <optional>
#include <openssl/sha.h>
#include <sstream>
#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace agents = scratchbird::core::agents;

constexpr const char* kAgentCatalogRecordKind = "agent_catalog_image";
constexpr const char* kAgentCatalogRowUuid = "agent-catalog-runtime-root";

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
          {"encoded_catalog_image", "text:not_null"},
          {"catalog_generation", "u64:not_null"},
          {"authority_evidence_uuid", "text:not_null"},
          {"storage_commit_evidence_uuid", "text:not_null"},
          {"storage_linkage_digest", "text:not_null"}};
}

std::optional<CrudTableRecord> FindCatalogTable(const CrudState& state,
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
  const CrudState state = BuildCrudCompatibilityStateFromMga(loaded.state);
  const auto existing = FindCatalogTable(state, context);
  if (existing) {
    AgentDurableCatalogStoreResult result;
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    result.table_uuid = existing->table_uuid;
    return result;
  }

  CrudTableRecord table;
  table.table_uuid = GenerateCrudEngineUuid("agent_catalog_table");
  table.default_name = kAgentDurableCatalogStoreTableName;
  table.columns = CatalogColumns();
  const auto appended = AppendMgaTableMetadata(context, table);
  if (appended.error) { return ErrorResult(appended); }

  AgentDurableCatalogStoreResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.table_uuid = table.table_uuid;
  return result;
}

std::optional<CrudRowVersionRecord> LatestCatalogRow(const CrudState& state,
                                                     const EngineRequestContext& context,
                                                     const std::string& table_uuid) {
  std::optional<CrudRowVersionRecord> latest;
  for (const auto& row : VisibleCrudRowsForContext(state, table_uuid, context)) {
    if (row.deleted || row.row_uuid != kAgentCatalogRowUuid) { continue; }
    if (CrudFieldValue(row.values, "record_kind") != kAgentCatalogRecordKind) {
      continue;
    }
    if (!latest || row.sequence > latest->sequence) {
      latest = row;
    }
  }
  return latest;
}

}  // namespace

AgentDurableCatalogStoreResult PersistAgentDurableCatalogImage(
    const AgentDurableCatalogStoreRequest& request) {
  if (request.production_live_path && !request.fsync_or_checkpoint_evidence) {
    return ErrorResult("fsync_or_checkpoint_evidence_required");
  }
  if (request.evidence_uuid.empty()) {
    return ErrorResult("evidence_uuid_required");
  }
  if (request.context.local_transaction_id == 0 ||
      request.context.transaction_uuid.canonical.empty()) {
    return ErrorResult("mga_transaction_context_required");
  }

  auto table = EnsureCatalogTable(request.context);
  if (!table.ok) { return table; }

  auto loaded = LoadMgaRelationStoreState(request.context);
  if (!loaded.ok) { return ErrorResult(std::move(loaded.diagnostic)); }
  const CrudState state = BuildCrudCompatibilityStateFromMga(loaded.state);
  const auto previous = LatestCatalogRow(state, request.context, table.table_uuid);
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
  image.authority.mga_transaction_uuid = request.context.transaction_uuid.canonical;
  image.authority.database_uuid = request.context.database_uuid.canonical;
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
  const std::string storage_linkage =
      Sha256Hex(table.table_uuid + "|" + kAgentCatalogRowUuid + "|" +
                image.authority.catalog_root_digest + "|" + request.evidence_uuid);

  CrudRowVersionRecord row;
  row.creator_tx = request.context.local_transaction_id;
  row.table_uuid = table.table_uuid;
  row.row_uuid = kAgentCatalogRowUuid;
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
  result.row_uuid = row.row_uuid;
  result.version_uuid = row.version_uuid;
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
  const CrudState state = BuildCrudCompatibilityStateFromMga(loaded.state);
  const auto table = FindCatalogTable(state, context);
  if (!table) { return ErrorResult("catalog_table_not_found"); }
  const auto latest = LatestCatalogRow(state, context, table->table_uuid);
  if (!latest) { return ErrorResult("catalog_image_not_found"); }

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
  result.table_uuid = table->table_uuid;
  result.row_uuid = latest->row_uuid;
  result.version_uuid = latest->version_uuid;
  result.row_event_sequence = latest->sequence;
  result.storage_linkage_digest =
      CrudFieldValue(latest->values, "storage_linkage_digest");
  result.schema_migration_applied = validation.migrated;
  return result;
}

}  // namespace scratchbird::engine::internal_api
