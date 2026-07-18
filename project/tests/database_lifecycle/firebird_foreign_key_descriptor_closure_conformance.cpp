// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "ddl/alter_api.hpp"
#include "ddl/create_api.hpp"
#include "dml/delete_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/update_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "sblr_dispatch.hpp"
#include "transaction/savepoint_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

void Require(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(result.ok, message);
}

std::uint64_t NowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string NewUuid(UuidKind kind) {
  static std::uint64_t sequence = 1;
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, NowMillis() + sequence++);
  Require(generated.ok(), "typed UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

struct Fixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string schema_uuid;
  std::string parent_uuid;
  std::string child_uuid;
  std::string direct_child_uuid;

  Fixture() = default;
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
  Fixture(Fixture&& other) noexcept
      : directory(std::move(other.directory)),
        database_path(std::move(other.database_path)),
        database_uuid(std::move(other.database_uuid)),
        schema_uuid(std::move(other.schema_uuid)),
        parent_uuid(std::move(other.parent_uuid)),
        child_uuid(std::move(other.child_uuid)),
        direct_child_uuid(std::move(other.direct_child_uuid)) {
    other.directory.clear();
  }
  Fixture& operator=(Fixture&&) = delete;

  ~Fixture() {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
};

Fixture MakeFixture() {
  Fixture fixture;
  fixture.directory = std::filesystem::temp_directory_path() /
      ("sb_firebird_fkey_d1_" + std::to_string(NowMillis()) + "_" +
       std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(fixture.directory);
  fixture.database_path = fixture.directory / "fixture.sbdb";
  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::database, NowMillis() + 100);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::filespace, NowMillis() + 101);
  Require(database_uuid.ok() && filespace_uuid.ok(),
          "database fixture UUID generation failed");
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = NowMillis();
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "Firebird FK fixture database create failed");
  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.schema_uuid = NewUuid(UuidKind::schema);
  return fixture;
}

api::EngineLocalizedName Name(std::string value) {
  return {"en", "primary", "", std::move(value), true};
}

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::uint64_t session) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "firebird-fkey-d1-runtime";
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical = NewUuid(UuidKind::object);
  context.session_uuid.canonical = NewUuid(UuidKind::object);
  context.current_schema_uuid.canonical = fixture.schema_uuid;
  context.default_root_uuid.canonical = NewUuid(UuidKind::object);
  context.security_context_present = true;
  context.identifier_profile_uuid = "firebird_v5";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("FIREBIRD_FKEY_D1_RUNTIME_" +
                               std::to_string(session));
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture,
                                std::uint64_t session) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, session);
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "Firebird FK transaction begin failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineCommitTransaction(request),
            "Firebird FK transaction commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request),
            "Firebird FK transaction rollback failed");
}

api::EngineColumnDefinition IntegerColumn(std::uint32_t ordinal,
                                          std::string name,
                                          bool primary_key = false) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "integer";
  column.descriptor.encoded_descriptor =
      std::string("type=integer;nullable=") +
      (primary_key ? "false" : "true");
  if (primary_key) {
    column.descriptor.encoded_descriptor += ";primary_key=true";
  }
  column.nullable = !primary_key;
  return column;
}

std::string CreateTable(const api::EngineRequestContext& context,
                        std::string name,
                        std::vector<api::EngineColumnDefinition> columns) {
  api::EngineCreateTableRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = context.current_schema_uuid.canonical;
  request.target_schema.object_kind = "schema";
  request.table_names.push_back(Name(std::move(name)));
  request.table_columns = std::move(columns);
  const auto created = api::EngineCreateTable(request);
  RequireOk(created, "Firebird FK table create failed");
  Require(!created.table_object.uuid.canonical.empty(),
          "table create omitted engine UUID");
  return created.table_object.uuid.canonical;
}

const api::MgaRelationColumnStorageDescriptor& ColumnByName(
    const api::MgaRelationStorageDescriptor& descriptor,
    std::string_view name) {
  const auto found = std::find_if(
      descriptor.columns.begin(), descriptor.columns.end(),
      [&](const auto& column) { return column.canonical_name_key == name; });
  Require(found != descriptor.columns.end(),
          "relation storage descriptor column missing");
  return *found;
}

void VerifyPersistedIndexDescriptorRoundTrip(
    const api::EngineRequestContext& context,
    const std::string& parent_uuid) {
  const auto persisted =
      api::LoadMgaRelationStorageDescriptor(context, parent_uuid);
  Require(persisted.ok,
          "committed parent relation descriptor reload failed");
  const auto primary = std::find_if(
      persisted.descriptor.indexes.begin(),
      persisted.descriptor.indexes.end(),
      [](const auto& index) {
        return index.unique && index.family == "btree" &&
               std::find(index.key_envelopes.begin(),
                         index.key_envelopes.end(),
                         "PKEY") != index.key_envelopes.end();
      });
  Require(primary != persisted.descriptor.indexes.end(),
          "persisted parent descriptor reload dropped the PK key envelope");

  auto covering = persisted.descriptor;
  const auto covering_primary = std::find_if(
      covering.indexes.begin(), covering.indexes.end(),
      [&](const auto& index) {
        return index.index_uuid.canonical == primary->index_uuid.canonical;
      });
  Require(covering_primary != covering.indexes.end(),
          "round-trip primary index lookup failed");
  covering_primary->include_columns = {"INT_F"};
  const auto serialized =
      api::SerializeMgaRelationStorageDescriptor(covering);
  const auto round_tripped =
      api::DeserializeMgaRelationStorageDescriptor(serialized);
  const auto round_tripped_primary = std::find_if(
      round_tripped.indexes.begin(), round_tripped.indexes.end(),
      [&](const auto& index) {
        return index.index_uuid.canonical == primary->index_uuid.canonical;
      });
  Require(round_tripped_primary != round_tripped.indexes.end() &&
              round_tripped_primary->key_envelopes ==
                  covering_primary->key_envelopes &&
              round_tripped_primary->include_columns ==
                  covering_primary->include_columns,
          "relation descriptor round-trip dropped index keys or included columns");
}

std::string ForeignKeyEnvelope(
    const api::MgaRelationStorageDescriptor& child,
    const api::MgaRelationColumnStorageDescriptor& child_column,
    const api::MgaRelationStorageDescriptor& parent,
    const api::MgaRelationColumnStorageDescriptor& parent_column) {
  std::ostringstream out;
  out << "descriptor_version=neutral_fk_single_column_v1"
      << ";child_table_uuid=" << child.relation_uuid.canonical
      << ";child_column_uuid=" << child_column.column_uuid.canonical
      << ";child_relation_descriptor_uuid="
      << child.descriptor_uuid.canonical
      << ";child_relation_descriptor_generation="
      << child.descriptor_generation
      << ";parent_table_uuid=" << parent.relation_uuid.canonical
      << ";parent_column_uuid=" << parent_column.column_uuid.canonical
      << ";parent_relation_descriptor_uuid="
      << parent.descriptor_uuid.canonical
      << ";parent_relation_descriptor_generation="
      << parent.descriptor_generation
      << ";referenced_table_uuid=" << parent.relation_uuid.canonical
      << ";referenced_column_uuid=" << parent_column.column_uuid.canonical
      << ";referenced_column=" << parent_column.canonical_name_key
      << ";child_column=" << child_column.canonical_name_key
      << ";constraint_name_quoted=false"
      << ";on_update=no_action;on_delete=no_action"
      << ";referential_action=no_action"
      << ";enforcement_timing=immediate;deferrable=false";
  return out.str();
}

api::EngineAlterConstraintResult AlterForeignKey(
    const api::EngineRequestContext& context,
    const std::string& child_uuid,
    std::string constraint_name = "INTEG_1") {
  const auto child = api::LoadMgaRelationStorageDescriptor(context, child_uuid);
  const auto parent = api::LoadMgaRelationStorageDescriptor(
      context,
      [&]() {
        const auto state = api::LoadMgaRelationStoreState(context);
        Require(state.ok, "state unavailable while resolving parent");
        const auto crud = api::BuildCrudCompatibilityStateFromMga(state.state);
        for (const auto& table : crud.tables) {
          if (table.default_name == "MASTER_TABLE") return table.table_uuid;
        }
        return std::string{};
      }());
  Require(child.ok && parent.ok, "FK relation descriptors unavailable");
  const auto& child_column = ColumnByName(child.descriptor, "FKEY");
  const auto& parent_column = ColumnByName(parent.descriptor, "PKEY");
  api::EngineAlterConstraintRequest request;
  request.context = context;
  request.target_object.uuid.canonical = child_uuid;
  request.target_object.object_kind = "table";
  api::EngineConstraintDefinition definition;
  definition.names.push_back(Name(std::move(constraint_name)));
  definition.constraint_kind = "foreign_key";
  definition.canonical_constraint_envelope = ForeignKeyEnvelope(
      child.descriptor, child_column, parent.descriptor, parent_column);
  request.constraints.push_back(std::move(definition));
  return api::EngineAlterConstraint(request);
}

void AlterForeignKeyThroughSblr(
    const api::EngineRequestContext& context,
    const std::string& child_uuid,
    std::string_view constraint_name = "INTEG_SBLR") {
  const auto child = api::LoadMgaRelationStorageDescriptor(context, child_uuid);
  const auto parent = api::LoadMgaRelationStorageDescriptor(
      context,
      [&]() {
        const auto state = api::LoadMgaRelationStoreState(context);
        Require(state.ok, "state unavailable while resolving SBLR parent");
        const auto crud = api::BuildCrudCompatibilityStateFromMga(state.state);
        for (const auto& table : crud.tables) {
          if (table.default_name == "MASTER_TABLE") return table.table_uuid;
        }
        return std::string{};
      }());
  Require(child.ok && parent.ok,
          "SBLR FK relation descriptors unavailable");
  const auto& child_column = ColumnByName(child.descriptor, "FKEY");
  const auto& parent_column = ColumnByName(parent.descriptor, "PKEY");
  const std::string canonical = ForeignKeyEnvelope(
      child.descriptor, child_column, parent.descriptor, parent_column);
  Require(canonical.find("constraint_hash=") == std::string::npos,
          "neutral Firebird FK route unexpectedly supplied a hash");

  // Construct the canonical engine SBLR representation corresponding to the
  // neutral server bridge output. This gate covers the SBLR decoder/dispatcher
  // -> EngineAlterConstraint seam, including BaseApiRequest completion; the
  // standalone parser producer is covered separately by the route gate.
  const auto dispatch = [&](std::string_view descriptor) {
    std::ostringstream encoded;
    encoded
        << "operation_id=ddl.constraint.alter\n"
        << "opcode=SBLR_DDL_CONSTRAINT_ALTER\n"
        << "sblr_operation_family=sblr.catalog.mutation.v3\n"
        << "result_shape=engine.api.result.v1\n"
        << "diagnostic_shape=engine.diagnostic.v1\n"
        << "trace_key=firebird.fkey.d1.sblr.route\n"
        << "contains_sql_text=false\n"
        << "parser_resolved_names_to_uuids=true\n"
        << "requires_security_context=true\n"
        << "requires_transaction_context=true\n"
        << "requires_cluster_authority=false\n"
        << "operand=text\tidentifier_profile_uuid\tfirebird_v5\n"
        << "operand=text\ttarget_object_uuid\t" << child_uuid << "\n"
        << "operand=text\ttarget_object_kind\ttable\n"
        << "operand=text\towner_object_uuid\t" << child_uuid << "\n"
        << "operand=text\tconstraint_kind\tforeign_key\n"
        << "operand=text\tconstraint_name\t" << constraint_name << "\n"
        << "operand=text\tenforcement_timing\timmediate\n"
        << "operand=text\tcanonical_constraint_envelope\t"
        << descriptor << "\n";
    return sblr::DecodeAndDispatchSblrOperation(encoded.str(), context);
  };

  // The bounded dispatcher exemption only preserves the neutral descriptor;
  // it must never erase or legitimize a parser-supplied reserved identity.
  const auto reserved_hash = dispatch(
      canonical + ";constraint_hash=parser_supplied_forbidden");
  const bool reserved_hash_rejected =
      !reserved_hash.api_result.ok &&
      std::any_of(reserved_hash.api_result.diagnostics.begin(),
                  reserved_hash.api_result.diagnostics.end(),
                  [](const auto& diagnostic) {
                    return diagnostic.detail.find(
                               "neutral_foreign_key_input_shape_invalid_or_reserved") !=
                           std::string::npos;
                  });
  Require(reserved_hash.envelope_validated && reserved_hash.accepted &&
              reserved_hash.dispatched_to_api && reserved_hash_rejected,
          "SBLR route admitted a parser-supplied FK constraint hash");

  const auto dispatched = dispatch(canonical);
  if (!dispatched.api_result.ok) {
    for (const auto& diagnostic : dispatched.api_result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(dispatched.envelope_validated && dispatched.accepted &&
              dispatched.dispatched_to_api && dispatched.api_result.ok,
          "neutral Firebird FK failed the engine SBLR dispatch route");
}

std::optional<api::CrudTableRecord> VisibleTable(
    const api::EngineRequestContext& context,
    const std::string& table_uuid) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "MGA relation state load failed");
  const auto state = api::BuildCrudCompatibilityStateFromMga(loaded.state);
  return api::FindVisibleCrudTable(
      state, table_uuid, context.local_transaction_id);
}

bool SealedForeignKeyVisible(const api::EngineRequestContext& context,
                             const std::string& table_uuid) {
  const auto table = VisibleTable(context, table_uuid);
  if (!table) return false;
  for (const auto& [name, descriptor] : table->columns) {
    (void)name;
    if (descriptor.find("constraint_mutation_batch_state=sealed") !=
        std::string::npos) {
      return true;
    }
  }
  return false;
}

std::map<std::string, std::string> DescriptorFields(
    std::string_view descriptor) {
  std::map<std::string, std::string> fields;
  std::size_t start = 0;
  while (start <= descriptor.size()) {
    const auto end = descriptor.find(';', start);
    const std::string part(descriptor.substr(
        start, end == std::string_view::npos ? descriptor.size() - start
                                             : end - start));
    const auto equals = part.find('=');
    if (equals != std::string::npos) {
      fields[part.substr(0, equals)] = part.substr(equals + 1);
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return fields;
}

std::string Upsert(std::string descriptor,
                   std::string_view key,
                   std::string_view value) {
  const std::string prefix = std::string(key) + "=";
  std::vector<std::string> parts;
  bool replaced = false;
  std::size_t start = 0;
  while (start <= descriptor.size()) {
    const auto end = descriptor.find(';', start);
    const std::string part = descriptor.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    if (part.rfind(prefix, 0) == 0) {
      parts.push_back(prefix + std::string(value));
      replaced = true;
    } else if (!part.empty()) {
      parts.push_back(part);
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if (!replaced) parts.push_back(prefix + std::string(value));
  std::string out;
  for (const auto& part : parts) {
    if (!out.empty()) out.push_back(';');
    out += part;
  }
  return out;
}

api::MgaConstraintMutationBatch InventedDirectBatch(
    const api::EngineRequestContext& context,
    const std::string& child_uuid,
    const std::string& parent_uuid) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "direct batch state load failed");
  const auto state = api::BuildCrudCompatibilityStateFromMga(loaded.state);
  const auto child = api::FindVisibleCrudTable(
      state, child_uuid, context.local_transaction_id);
  const auto parent = api::FindVisibleCrudTable(
      state, parent_uuid, context.local_transaction_id);
  Require(child && parent, "direct batch tables unavailable");
  const auto child_storage =
      api::LoadMgaRelationStorageDescriptor(context, child_uuid);
  const auto parent_storage =
      api::LoadMgaRelationStorageDescriptor(context, parent_uuid);
  Require(child_storage.ok && parent_storage.ok,
          "direct batch descriptors unavailable");
  const auto& child_column = ColumnByName(child_storage.descriptor, "FKEY");
  const auto& parent_column = ColumnByName(parent_storage.descriptor, "PKEY");

  api::MgaConstraintMutationBatch batch;
  batch.batch_uuid = NewUuid(UuidKind::row);
  batch.mutation_count = 1;
  batch.database_uuid = context.database_uuid.canonical;
  batch.constraint_uuid = NewUuid(UuidKind::object);
  batch.owner_table_uuid = child_uuid;
  batch.child_schema_uuid =
      child_storage.descriptor.schema_uuid.canonical;
  batch.child_relation_descriptor_uuid =
      child_storage.descriptor.descriptor_uuid.canonical;
  batch.child_relation_descriptor_generation =
      child_storage.descriptor.descriptor_generation;
  batch.child_column_uuid = child_column.column_uuid.canonical;
  batch.parent_table_uuid = parent_uuid;
  batch.parent_schema_uuid =
      parent_storage.descriptor.schema_uuid.canonical;
  batch.parent_relation_descriptor_uuid =
      parent_storage.descriptor.descriptor_uuid.canonical;
  batch.parent_relation_descriptor_generation =
      parent_storage.descriptor.descriptor_generation;
  batch.parent_column_uuid = parent_column.column_uuid.canonical;
  // These three values are self-consistent across the proposed child
  // projection/envelope but deliberately do not match the current parent.
  batch.parent_candidate_key_constraint_uuid = NewUuid(UuidKind::object);
  batch.key_descriptor_uuid = NewUuid(UuidKind::object);
  batch.support_uuid = NewUuid(UuidKind::object);
  batch.support_family = "btree";
  batch.support_policy = "required_exact_unique_index";
  batch.match_policy = "simple";
  batch.on_update_action = "no_action";
  batch.on_delete_action = "no_action";
  batch.enforcement_timing = "immediate";
  batch.constraint_metadata_generation = 1;
  batch.base_table_event_sequence = child->event_sequence;
  batch.parent_base_table_event_sequence = parent->event_sequence;
  batch.constraint_name = "DIRECT_INVENTED";
  batch.constraint_kind = "foreign_key";

  std::string envelope = ForeignKeyEnvelope(
      child_storage.descriptor, child_column,
      parent_storage.descriptor, parent_column);
  for (const auto& [key, value] :
       std::vector<std::pair<std::string, std::string>>{
           {"constraint_uuid", batch.constraint_uuid},
           {"constraint_name", batch.constraint_name},
           {"child_column", child_column.canonical_name_key},
           {"referenced_column", parent_column.canonical_name_key},
           {"constraint_name_quoted", "false"},
           {"owner_object_uuid", child_uuid},
           {"key_descriptor_uuid", batch.key_descriptor_uuid},
           {"referenced_candidate_key_constraint_uuid",
            batch.parent_candidate_key_constraint_uuid},
           {"support_uuid", batch.support_uuid},
           {"support_family", "btree"},
           {"constraint_mutation_batch_uuid", batch.batch_uuid},
           {"constraint_mutation_batch_state", "sealed"}}) {
    envelope = Upsert(std::move(envelope), key, value);
  }
  batch.canonical_constraint_envelope = envelope;
  batch.updated_table = *child;
  auto changed = std::find_if(
      batch.updated_table.columns.begin(), batch.updated_table.columns.end(),
      [](const auto& column) { return column.first == "FKEY"; });
  Require(changed != batch.updated_table.columns.end(),
          "direct batch child column missing");
  for (const auto& [key, value] :
       std::vector<std::pair<std::string, std::string>>{
           {"foreign_key", "true"},
           {"constraint_uuid", batch.constraint_uuid},
           {"constraint_name", batch.constraint_name},
           {"constraint_class", "foreign_key"},
           {"owner_object_uuid", child_uuid},
           {"owner_object_name", child->default_name},
           {"child_column_uuid", batch.child_column_uuid},
           {"referenced_table_uuid", parent_uuid},
           {"referenced_table_name", parent->default_name},
           {"referenced_column_uuid", batch.parent_column_uuid},
           {"referenced_column", parent_column.canonical_name_key},
           {"key_descriptor_uuid", batch.key_descriptor_uuid},
           {"referenced_key_descriptor_uuid", batch.key_descriptor_uuid},
           {"referenced_candidate_key_constraint_uuid",
            batch.parent_candidate_key_constraint_uuid},
           {"support_uuid", batch.support_uuid},
           {"referenced_support_uuid", batch.support_uuid},
           {"support_family", "btree"},
           {"on_update", "no_action"},
           {"on_delete", "no_action"},
           {"referential_action", "no_action"},
           {"enforcement_timing", "immediate"},
           {"deferrable", "false"},
           {"constraint_mutation_batch_uuid", batch.batch_uuid},
           {"constraint_mutation_batch_state", "sealed"}}) {
    changed->second = Upsert(std::move(changed->second), key, value);
  }
  return batch;
}

api::EngineTypedValue IntegerValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "integer";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineInsertRowsResult InsertValue(
    const api::EngineRequestContext& context,
    const std::string& table_uuid,
    std::string column,
    std::string value) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = table_uuid;
  request.target_table.object_kind = "table";
  api::EngineRowValue row;
  row.fields.push_back({std::move(column), IntegerValue(std::move(value))});
  request.input_rows.push_back(std::move(row));
  return api::EngineInsertRows(request);
}

api::EngineInsertRowsResult InsertValues(
    const api::EngineRequestContext& context,
    const std::string& table_uuid,
    std::vector<std::pair<std::string, std::string>> fields) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = table_uuid;
  request.target_table.object_kind = "table";
  api::EngineRowValue row;
  for (auto& [column, value] : fields) {
    row.fields.push_back(
        {std::move(column), IntegerValue(std::move(value))});
  }
  request.input_rows.push_back(std::move(row));
  return api::EngineInsertRows(request);
}

const api::EngineApiDiagnostic* Diagnostic(
    const api::EngineApiResult& result,
    std::string_view code) {
  const auto found = std::find_if(
      result.diagnostics.begin(), result.diagnostics.end(),
      [&](const auto& diagnostic) { return diagnostic.code == code; });
  return found == result.diagnostics.end() ? nullptr : &*found;
}

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  Require(input.good(), "metadata sidecar open failed");
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) lines.push_back(line);
  return lines;
}

void WriteLines(const std::filesystem::path& path,
                const std::vector<std::string>& lines) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  Require(output.good(), "metadata sidecar rewrite failed");
  for (const auto& line : lines) output << line << '\n';
  output.flush();
  Require(output.good(), "metadata sidecar flush failed");
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start <= line.size()) {
    const auto end = line.find('\t', start);
    fields.push_back(line.substr(
        start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return fields;
}

std::string JoinTabs(const std::vector<std::string>& fields) {
  std::string line;
  for (const auto& field : fields) {
    if (!line.empty()) line.push_back('\t');
    line += field;
  }
  return line;
}

void VerifyCorruptionFailsClosed(const Fixture& fixture,
                                 const api::EngineRequestContext& context) {
  const auto metadata = std::filesystem::path(
      fixture.database_path.string() + ".sb.mga_relation_metadata");
  const auto original_lines = ReadLines(metadata);
  std::size_t batch_line = original_lines.size();
  for (std::size_t index = 0; index < original_lines.size(); ++index) {
    if (original_lines[index].find("\tCONSTRAINT_MUTATION_BATCH\t") !=
        std::string::npos) {
      batch_line = index;
    }
  }
  Require(batch_line < original_lines.size(),
          "sealed constraint batch line not found");
  const auto original_fields = SplitTabs(original_lines[batch_line]);
  Require(original_fields.size() == 43,
          "sealed constraint batch codec is not 43 fields");

  auto reject = [&](std::vector<std::string> fields,
                    std::string_view label) {
    auto lines = original_lines;
    lines[batch_line] = JoinTabs(fields);
    WriteLines(metadata, lines);
    const auto loaded = api::LoadMgaRelationStoreState(context);
    Require(!loaded.ok && loaded.diagnostic.error,
            std::string("corrupted batch was admitted: ") +
                std::string(label));
    WriteLines(metadata, original_lines);
    Require(api::LoadMgaRelationStoreState(context).ok,
            "valid metadata did not reload after corruption restore");
  };

  auto short_fields = original_fields;
  short_fields.pop_back();
  Require(short_fields.size() == 42, "short codec fixture is not 42 fields");
  reject(std::move(short_fields), "42-field record");
  auto long_fields = original_fields;
  long_fields.push_back("trailing");
  Require(long_fields.size() == 44, "long codec fixture is not 44 fields");
  reject(std::move(long_fields), "44-field record");
  auto hash = original_fields;
  hash[7].back() = hash[7].back() == '0' ? '1' : '0';
  reject(std::move(hash), "batch hash");
  auto event = original_fields;
  event[3] += "1";
  reject(std::move(event), "sealed metadata event sequence");
  auto child_event = original_fields;
  child_event[31] += "1";
  reject(std::move(child_event), "child base table event sequence");
  auto parent_event = original_fields;
  parent_event[32] += "1";
  reject(std::move(parent_event), "parent base table event sequence");
}

}  // namespace

int main() {
  Fixture fixture = MakeFixture();
  auto setup = Begin(fixture, 1);
  api::EngineCreateSchemaRequest schema;
  schema.context = setup;
  schema.target_object.uuid.canonical = fixture.schema_uuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(Name("FKEY_D1_SCHEMA"));
  RequireOk(api::EngineCreateSchema(schema), "FK schema create failed");
  fixture.parent_uuid = CreateTable(
      setup, "MASTER_TABLE",
      {IntegerColumn(0, "PKEY", true), IntegerColumn(1, "INT_F")});
  fixture.child_uuid = CreateTable(
      setup, "DETAIL_TABLE",
      {IntegerColumn(0, "ID", true), IntegerColumn(1, "FKEY")});
  fixture.direct_child_uuid = CreateTable(
      setup, "DIRECT_DETAIL", {IntegerColumn(0, "FKEY")});
  Commit(setup);

  // Storage authority must reject both malformed typed identities and a
  // complete, self-consistent child proposal whose parent identities were
  // invented by an internal caller.
  auto direct = Begin(fixture, 2);
  VerifyPersistedIndexDescriptorRoundTrip(
      direct, fixture.parent_uuid);
  auto invented = InventedDirectBatch(
      direct, fixture.direct_child_uuid, fixture.parent_uuid);
  auto malformed_uuid = invented;
  malformed_uuid.batch_uuid = "not-a-row-uuid";
  Require(api::AppendMgaConstraintMutationBatch(direct, malformed_uuid).error,
          "append admitted malformed typed batch UUID");
  auto stale_owner = invented;
  ++stale_owner.base_table_event_sequence;
  const auto stale_owner_result =
      api::AppendMgaConstraintMutationBatch(direct, stale_owner);
  Require(stale_owner_result.error &&
              stale_owner_result.detail.find(
                  "owner_metadata_event_changed_before_append") !=
                  std::string::npos,
          "append admitted stale child metadata event binding");
  auto stale_parent = invented;
  ++stale_parent.parent_base_table_event_sequence;
  const auto stale_parent_result =
      api::AppendMgaConstraintMutationBatch(direct, stale_parent);
  Require(stale_parent_result.error &&
              stale_parent_result.detail.find(
                  "parent_metadata_event_changed_before_append") !=
                  std::string::npos,
          "append admitted stale parent metadata event binding");
  const auto invented_result =
      api::AppendMgaConstraintMutationBatch(direct, invented);
  Require(invented_result.error &&
              invented_result.detail.find("parent_candidate_key_projection_changed") !=
                  std::string::npos,
          "append admitted invented parent candidate/key/support identities");
  Rollback(direct);

  auto writer = Begin(fixture, 3);
  api::EngineCreateSavepointRequest savepoint;
  savepoint.context = writer;
  savepoint.localized_names.push_back(Name("BEFORE_FKEY"));
  RequireOk(api::EngineCreateSavepoint(savepoint),
            "FK savepoint create failed");
  RequireOk(AlterForeignKey(writer, fixture.child_uuid),
            "FK ALTER in writer failed");
  Require(SealedForeignKeyVisible(writer, fixture.child_uuid),
          "writer cannot see its sealed FK batch");
  auto sibling = Begin(fixture, 4);
  Require(!SealedForeignKeyVisible(sibling, fixture.child_uuid),
          "sibling saw uncommitted FK batch");
  api::EngineRollbackToSavepointRequest rollback_to;
  rollback_to.context = writer;
  rollback_to.localized_names.push_back(Name("BEFORE_FKEY"));
  RequireOk(api::EngineRollbackToSavepoint(rollback_to),
            "rollback-to-savepoint failed");
  Require(!SealedForeignKeyVisible(writer, fixture.child_uuid),
          "savepoint rollback did not hide FK batch");
  Commit(writer);
  Require(!SealedForeignKeyVisible(sibling, fixture.child_uuid),
          "sibling saw savepoint-rolled-back FK batch");
  Rollback(sibling);

  auto full_rollback = Begin(fixture, 5);
  RequireOk(AlterForeignKey(full_rollback, fixture.child_uuid),
            "FK ALTER before full rollback failed");
  Require(SealedForeignKeyVisible(full_rollback, fixture.child_uuid),
          "full-rollback writer cannot see FK batch");
  Rollback(full_rollback);
  auto after_rollback = Begin(fixture, 6);
  Require(!SealedForeignKeyVisible(after_rollback, fixture.child_uuid),
          "fully rolled-back FK batch remained visible");
  Rollback(after_rollback);

  auto committed_writer = Begin(fixture, 7);
  AlterForeignKeyThroughSblr(committed_writer, fixture.child_uuid);
  Commit(committed_writer);
  auto reader = Begin(fixture, 8);
  Require(SealedForeignKeyVisible(reader, fixture.child_uuid),
          "committed FK batch is not visible");

  VerifyCorruptionFailsClosed(fixture, reader);

  const auto missing = InsertValues(
      reader, fixture.child_uuid, {{"ID", "7"}, {"FKEY", "1"}});
  const auto* missing_diagnostic = Diagnostic(
      missing, "CLI.CONSTRAINT_FOREIGN_KEY_VIOLATION");
  if (missing_diagnostic == nullptr || missing_diagnostic->fields.size() != 6 ||
      missing_diagnostic->fields[0].key != "violation_kind" ||
      missing_diagnostic->fields[0].value != "parent_missing" ||
      missing_diagnostic->fields[5].key != "key_display_text" ||
      missing_diagnostic->fields[5].value != "(\"FKEY\" = 1)") {
    for (const auto& diagnostic : missing.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << "  " << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(missing_diagnostic != nullptr &&
              missing_diagnostic->fields.size() == 6 &&
              missing_diagnostic->fields[0].key == "violation_kind" &&
              missing_diagnostic->fields[0].value == "parent_missing" &&
              missing_diagnostic->fields[5].key == "key_display_text" &&
              missing_diagnostic->fields[5].value == "(\"FKEY\" = 1)",
          "missing-parent DML did not emit exact structured FK fields");

  const auto parent = InsertValue(
      reader, fixture.parent_uuid, "PKEY", "1");
  RequireOk(parent, "parent fixture insert failed");
  Require(!parent.row_uuids.empty(), "parent insert omitted row UUID");
  RequireOk(InsertValues(
                reader,
                fixture.child_uuid,
                {{"ID", "1"}, {"FKEY", "1"}}),
            "valid child insert failed");

  api::EngineUpdateRowsRequest update;
  update.context = reader;
  update.target_table.uuid.canonical = fixture.parent_uuid;
  update.target_table.object_kind = "table";
  update.update_predicate.predicate_kind = "row_uuid_match";
  update.update_predicate.canonical_predicate_envelope =
      parent.row_uuids.front().canonical;
  update.assignments.push_back({"PKEY", IntegerValue("2")});
  const auto update_result = api::EngineUpdateRows(update);
  const auto* update_diagnostic = Diagnostic(
      update_result, "CLI.CONSTRAINT_FOREIGN_KEY_VIOLATION");
  Require(update_diagnostic != nullptr &&
              update_diagnostic->fields.size() == 6 &&
              update_diagnostic->fields[0].value == "references_present",
          "parent-key update did not emit references-present fields");

  api::EngineDeleteRowsRequest deletion;
  deletion.context = reader;
  deletion.target_table.uuid.canonical = fixture.parent_uuid;
  deletion.target_table.object_kind = "table";
  deletion.delete_predicate.predicate_kind = "column_equals";
  deletion.delete_predicate.canonical_predicate_envelope = "PKEY";
  deletion.delete_predicate.bound_values.push_back(IntegerValue("1"));
  const auto delete_result = api::EngineDeleteRows(deletion);
  const auto* delete_diagnostic = Diagnostic(
      delete_result, "CLI.CONSTRAINT_FOREIGN_KEY_VIOLATION");
  Require(delete_diagnostic != nullptr &&
              delete_diagnostic->fields.size() == 6 &&
              delete_diagnostic->fields[0].value == "references_present",
          "parent-key delete did not emit references-present fields");
  Commit(reader);

  // Match Firebird fkey/primary/test_upd_pk_01.py: an uncommitted update of
  // an unrelated parent column must not invalidate the persisted FK support
  // descriptor, and a sibling transaction may update only the child's PK.
  auto parent_writer = Begin(fixture, 9);
  api::EngineUpdateRowsRequest parent_non_key_update;
  parent_non_key_update.context = parent_writer;
  parent_non_key_update.target_table.uuid.canonical = fixture.parent_uuid;
  parent_non_key_update.target_table.object_kind = "table";
  parent_non_key_update.assignments.push_back(
      {"INT_F", IntegerValue("10")});
  RequireOk(api::EngineUpdateRows(parent_non_key_update),
            "unrelated parent-column update failed");

  auto child_writer = Begin(fixture, 10);
  api::EngineUpdateRowsRequest child_primary_key_update;
  child_primary_key_update.context = child_writer;
  child_primary_key_update.target_table.uuid.canonical = fixture.child_uuid;
  child_primary_key_update.target_table.object_kind = "table";
  child_primary_key_update.update_predicate.predicate_kind =
      "column_equals";
  child_primary_key_update.update_predicate.canonical_predicate_envelope =
      "ID";
  child_primary_key_update.update_predicate.bound_values.push_back(
      IntegerValue("1"));
  child_primary_key_update.assignments.push_back(
      {"ID", IntegerValue("2")});
  const auto child_update_result =
      api::EngineUpdateRows(child_primary_key_update);
  if (!child_update_result.ok) {
    for (const auto& diagnostic : child_update_result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  RequireOk(child_update_result,
            "unrelated child-PK update rejected the unchanged FK");
  Require(child_update_result.updated_count == 1,
          "child-PK update changed the wrong row count");
  Commit(child_writer);
  Rollback(parent_writer);

  auto verifier = Begin(fixture, 11);
  const auto verified_store = api::LoadMgaRelationStoreState(verifier);
  Require(verified_store.ok,
          "post-update MGA relation state load failed");
  const auto verified_state =
      api::BuildCrudCompatibilityStateFromMga(verified_store.state);
  const auto visible_children = api::VisibleCrudRowsForContext(
      verified_state, fixture.child_uuid, verifier);
  Require(visible_children.size() == 1,
          "child-PK update produced the wrong visible-row count");
  const auto id = std::find_if(
      visible_children.front().values.begin(),
      visible_children.front().values.end(),
      [](const auto& field) { return field.first == "ID"; });
  const auto fkey = std::find_if(
      visible_children.front().values.begin(),
      visible_children.front().values.end(),
      [](const auto& field) { return field.first == "FKEY"; });
  Require(id != visible_children.front().values.end() && id->second == "2" &&
              fkey != visible_children.front().values.end() &&
              fkey->second == "1",
          "child-PK update changed the FK value or lost the new PK value");
  Rollback(verifier);

  std::cout << "Firebird FK descriptor/MGA closure conformance passed\n";
  return EXIT_SUCCESS;
}
