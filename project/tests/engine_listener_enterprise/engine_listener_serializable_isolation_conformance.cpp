#include "../support/engine_evidence_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/native_catalog_column_fixture.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "isolation.hpp"
#include "database_lifecycle.hpp"
#include "dml/delete_api.hpp"
#include "ddl/create_api.hpp"
#include "catalog/schema_tree_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "dml/update_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "memory.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef UuidToString
#undef UuidToString
#endif
#else
#include <unistd.h>
#endif

namespace {

namespace platform = scratchbird::core::platform;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

using platform::TypedUuid;
using platform::UuidKind;
using platform::u64;

constexpr u64 kBaseMillis = 1770000000000ull;

bool Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

void PrintDiagnostic(const platform::DiagnosticRecord& diagnostic) {
  if (!diagnostic.diagnostic_code.empty()) {
    std::cerr << diagnostic.diagnostic_code << ':' << diagnostic.message_key;
    for (const auto& argument : diagnostic.arguments) {
      std::cerr << ' ' << argument.key << '=' << argument.value;
    }
    std::cerr << '\n';
  }
}

void PrintDiagnostics(const api::EngineApiResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key
              << ':' << diagnostic.detail << '\n';
  }
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "engine_listener_serializable_isolation_conformance";
  policy.hard_limit_bytes = 8 * 1024 * 1024;
  policy.soft_limit_bytes = 8 * 1024 * 1024;
  policy.per_context_limit_bytes = 8 * 1024 * 1024;
  policy.page_buffer_pool_limit_bytes = 8 * 1024 * 1024;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

bool ConfigureMemoryFixture() {
  const auto configured =
      memory::ConfigureDefaultMemoryManagerForFixture(
          MemoryPolicy(),
          "engine_listener_serializable_isolation_conformance");
  if (!configured.ok()) {
    PrintDiagnostic(configured.diagnostic);
  }
  return Require(configured.ok(),
                 "ELER-021 memory fixture should configure") &&
         Require(configured.fixture_mode,
                 "ELER-021 memory fixture should run in fixture mode");
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  if (!generated.ok()) {
    std::cerr << "uuid generation failed\n";
    std::exit(EXIT_FAILURE);
  }
  return generated.value;
}

txn::SerializableAccessRecord Access(txn::LocalTransactionId local_id,
                                     txn::TransactionState state,
                                     txn::SerializableAccessKind kind,
                                     txn::SerializableKeyRange range,
                                     u64 sequence) {
  txn::SerializableAccessRecord access;
  access.local_id = local_id;
  access.transaction_state = state;
  access.kind = kind;
  access.range = std::move(range);
  access.sequence = sequence;
  access.durable_inventory_authoritative = true;
  return access;
}

bool HasEvidence(const txn::SerializableConflictResult& result,
                 std::string_view expected) {
  for (const std::string& evidence : result.evidence) {
    if (evidence == expected) {
      return true;
    }
  }
  return false;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view value) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && scratchbird::tests::EvidenceTextEquals(item.evidence_id, value)) {
      return true;
    }
  }
  return false;
}

std::string FirstDiagnosticCode(const api::EngineApiResult& result) {
  if (result.diagnostics.empty()) {
    return {};
  }
  return result.diagnostics.front().code;
}

std::filesystem::path TempRoot() {
  std::string scope = std::filesystem::current_path().filename().string();
  if (scope.empty()) {
    scope = "default";
  }
#ifdef _WIN32
  const auto pid = static_cast<unsigned long long>(::GetCurrentProcessId());
#else
  const auto pid = static_cast<unsigned long long>(::getpid());
#endif
  auto root = std::filesystem::temp_directory_path() /
              ("scratchbird_engine_listener_serializable_" + scope +
               "_" + std::to_string(pid));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  return root;
}

struct ApiFixture {
  std::filesystem::path root;
  std::filesystem::path database_path;
  api::EngineUuid database_uuid;
  api::EngineUuid table_uuid;
  api::EngineRequestContext owner;
  std::shared_ptr<scratchbird::tests::FixtureEngineSession> engine_session;

  ~ApiFixture() {
    engine_session.reset();
    if (!root.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
    }
  }
};

api::EngineUuid NativeIdentity(UuidKind kind, u64 offset) {
  return MakeUuid(kind, offset).value;
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRowValue Row(std::string id, std::string note) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

api::EnginePredicateEnvelope EqualsPredicate(std::string column,
                                             std::string value) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_equals";
  predicate.canonical_predicate_envelope = std::move(column);
  predicate.bound_values.push_back(TextValue(std::move(value)));
  return predicate;
}

api::EnginePredicateEnvelope RangePredicate(std::string column,
                                            std::string lower,
                                            std::string upper) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_range";
  predicate.canonical_predicate_envelope = std::move(column);
  predicate.bound_values.push_back(TextValue(std::move(lower)));
  predicate.bound_values.push_back(TextValue(std::move(upper)));
  return predicate;
}

api::EngineRequestContext BaseContext(const ApiFixture& fixture,
                                      std::string request_id) {
  auto context = fixture.owner;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid = fixture.database_uuid;
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  const auto epochs = api::LoadCatalogObjectLifecycleEpochState(context);
  if (!epochs.ok) throw std::runtime_error("fixture catalog epochs unavailable");
  if (epochs.state.metadata_epoch) context.catalog_generation_id = epochs.state.metadata_epoch;
  if (epochs.state.name_resolution_epoch) context.name_resolution_epoch = epochs.state.name_resolution_epoch;
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(context);
  return context;
}

api::EngineRequestContext Begin(const ApiFixture& fixture,
                                std::string request_id,
                                std::string isolation = "serializable") {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = std::move(isolation);
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok) {
    PrintDiagnostics(begun);
  }
  Require(begun.ok, "engine API begin transaction should succeed");
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
  const auto committed = api::EngineCommitTransaction(request);
  if (!committed.ok) {
    PrintDiagnostics(committed);
  }
  Require(committed.ok, "engine API commit should succeed");
}

api::CrudTableRecord Table(const ApiFixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "eler021_serializable";
  table.columns.push_back({"id", scratchbird::tests::NativeCatalogColumnFixture(
      {{{"canonical", "text"}, {"primary_key", "true"}}, {}})});
  table.columns.push_back({"note", scratchbird::tests::NativeCatalogColumnFixture(
      {{{"canonical", "text"}}, {}})});
  return table;
}

ApiFixture MakeApiFixture() {
  ApiFixture fixture;
  fixture.root = TempRoot();
  fixture.database_path = fixture.root / "eler021_serializable_api.sbdb";
  fixture.table_uuid = NativeIdentity(UuidKind::object, 402);

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = MakeUuid(UuidKind::database, 401);
  create.filespace_uuid = MakeUuid(UuidKind::filespace, 404);
  create.creation_unix_epoch_millis = kBaseMillis + 405;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    PrintDiagnostic(created.diagnostic);
  }
  Require(created.ok(), "ELER-021 API fixture database should create");

  fixture.database_uuid = create.database_uuid.value;
  fixture.owner = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  fixture.engine_session = std::make_shared<scratchbird::tests::FixtureEngineSession>(fixture.owner);
  auto metadata = Begin(fixture, "eler021-metadata", "read_committed");
  api::EngineApiDiagnostic schema_diagnostic;
  const auto schemas = api::VisibleSchemaTreeRecords(metadata,
      metadata.local_transaction_id, schema_diagnostic);
  if (schema_diagnostic.error || schemas.empty())
    throw std::runtime_error("serializable fixture bootstrap schema unavailable");
  metadata.current_schema_uuid = schemas.front().schema_uuid;
  fixture.owner.current_schema_uuid = metadata.current_schema_uuid;
  api::EngineCreateTableRequest table_request;
  table_request.context = metadata;
  table_request.requested_table_uuid = fixture.table_uuid;
  table_request.target_schema.uuid = metadata.current_schema_uuid;
  table_request.target_schema.object_kind = "schema";
  table_request.table_names.push_back({"en", "primary", "", "eler021_serializable", true});
  namespace dt = scratchbird::core::datatypes;
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  const auto type = dt::LookupDatatypeCatalogRow(manifest.manifest,
      dt::CanonicalTypeIdFromStableName("text"));
  if (!manifest.ok() || !type.ok() || type.manifest.descriptor_rows.size() != 1)
    throw std::runtime_error("serializable fixture text datatype unavailable");
  const auto& datatype = type.manifest.descriptor_rows.front();
  const auto binding = dt::LookupDatatypeTypeCodecIdentityV1(
      metadata.datatype_catalog_snapshot_uuid, metadata.datatype_catalog_generation,
      metadata.datatype_registry_generation, datatype.descriptor_uuid.value,
      datatype.descriptor_epoch);
  if (!binding.ok) throw std::runtime_error("serializable fixture text codec unavailable");
  const auto definition = Table(fixture, metadata);
  for (std::size_t ordinal = 0; ordinal < definition.columns.size(); ++ordinal) {
    api::EngineColumnDefinition column;
    column.ordinal = ordinal;
    column.requested_column_uuid = api::GenerateCrudEngineUuid("object");
    column.names.push_back({"en", "primary", "", definition.columns[ordinal].first, true});
    column.descriptor.descriptor_uuid = api::GenerateCrudEngineUuid("object");
    column.descriptor.descriptor_kind = "scalar";
    column.descriptor.canonical_type_name = "text";
    column.descriptor.datatype_descriptor_uuid = binding.row.descriptor_uuid;
    column.descriptor.datatype_descriptor_generation = binding.row.descriptor_generation;
    column.descriptor.type_uuid = binding.row.type_uuid;
    column.descriptor.encoded_descriptor = definition.columns[ordinal].second;
    column.nullable = ordinal != 0;
    table_request.table_columns.push_back(std::move(column));
  }
  const auto table_created = api::EngineCreateTable(table_request);
  if (!table_created.ok) {
    PrintDiagnostics(table_created);
    throw std::runtime_error("serializable fixture table creation failed");
  }
  const auto state = api::LoadMgaRelationStoreState(metadata);
  if (!state.ok) throw std::runtime_error("serializable fixture catalog reload failed");
  const auto indexes = api::VisibleCrudIndexesForTableColumn(
      state.state.relation_metadata, fixture.table_uuid, "id", metadata.local_transaction_id);
  if (indexes.size() != 1 || !indexes.front().unique ||
      indexes.front().family != api::kCrudIndexFamilyBtree ||
      indexes.front().profile != api::kCrudIndexProfileRowStoreScalarBtreeV1 ||
      !uuid::IsEngineIdentityUuid(indexes.front().index_uuid)) {
    std::cerr << "actual index count=" << indexes.size() << '\n';
    for (const auto& index : indexes) std::cerr << "unique=" << index.unique
        << " family=" << index.family << " profile=" << index.profile << '\n';
    throw std::runtime_error("serializable fixture actual primary-key support index missing");
  }
  Commit(metadata);
  return fixture;
}

std::vector<std::string> DmlOptions() {
  return {"page_allocation.runtime=enabled",
          "page_allocation.free_pages=64",
          "page_allocation.preallocate_data_pages=16",
          "page_allocation.preallocate_index_pages=16",
          "result_payload_policy=summary_only"};
}

api::EngineSelectRowsResult SelectRange(const ApiFixture& fixture,
                                        const api::EngineRequestContext& context,
                                        std::string lower,
                                        std::string upper) {
  scratchbird::tests::FixtureEngineRequest<api::EngineSelectRowsRequest> request(
      *fixture.engine_session, context);
  request.source_object.uuid = fixture.table_uuid;
  request.source_object.object_kind = "table";
  request.select_predicate =
      RangePredicate("id", std::move(lower), std::move(upper));
  request.option_envelopes = DmlOptions();
  return api::EngineSelectRows(request);
}

api::EngineInsertRowsResult InsertRow(const ApiFixture& fixture,
                                      const api::EngineRequestContext& context,
                                      std::string id,
                                      std::string note) {
  scratchbird::tests::FixtureEngineRequest<api::EngineInsertRowsRequest> request(
      *fixture.engine_session, context);
  request.target_table.uuid = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows.push_back(Row(std::move(id), std::move(note)));
  request.estimated_row_count = 1;
  request.option_envelopes = DmlOptions();
  return api::EngineInsertRows(request);
}

api::EngineUpdateRowsResult UpdateRow(const ApiFixture& fixture,
                                      const api::EngineRequestContext& context,
                                      std::string id) {
  scratchbird::tests::FixtureEngineRequest<api::EngineUpdateRowsRequest> request(
      *fixture.engine_session, context);
  request.target_table.uuid = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.update_predicate = EqualsPredicate("id", std::move(id));
  request.assignments.push_back({"note", TextValue("updated")});
  request.option_envelopes = DmlOptions();
  return api::EngineUpdateRows(request);
}

api::EngineDeleteRowsResult DeleteRow(const ApiFixture& fixture,
                                      const api::EngineRequestContext& context,
                                      std::string id) {
  scratchbird::tests::FixtureEngineRequest<api::EngineDeleteRowsRequest> request(
      *fixture.engine_session, context);
  request.target_table.uuid = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.delete_predicate = EqualsPredicate("id", std::move(id));
  request.option_envelopes = DmlOptions();
  return api::EngineDeleteRows(request);
}

bool SerializableIntegratedEngineApiProof() {
  bool ok = true;
  ok = ConfigureMemoryFixture() && ok;
  if (!ok) {
    return false;
  }
  auto fixture = MakeApiFixture();

  auto unique_writer = Begin(fixture, "eler021-unique-seed", "read_committed");
  const auto unique_seed = InsertRow(fixture, unique_writer, "001", "original");
  if (!unique_seed.ok) PrintDiagnostics(unique_seed);
  ok = Require(unique_seed.ok, "real primary-key fixture seed failed") && ok;
  Commit(unique_writer);
  auto duplicate_writer = Begin(fixture, "eler021-unique-duplicate", "read_committed");
  const auto duplicate = InsertRow(fixture, duplicate_writer, "001", "duplicate");
  ok = Require(!duplicate.ok && FirstDiagnosticCode(duplicate) ==
      "CLI.CONSTRAINT_PRIMARY_KEY_VIOLATION",
      "real primary-key fixture did not reject duplicate insertion") && ok;
  const auto retained = SelectRange(fixture, duplicate_writer, "001", "001");
  ok = Require(retained.ok && retained.result_shape.rows.size() == 1,
      "duplicate refusal changed the committed primary-key row count") && ok;
  Commit(duplicate_writer);
  auto reader = Begin(fixture, "eler021-reader");
  const auto range_read = SelectRange(fixture, reader, "100", "200");
  ok = Require(range_read.ok,
               "serializable SELECT range read should succeed") && ok;
  ok = Require(HasEvidence(range_read.evidence,
                           "serializable.admission",
                           "recorded"),
               "serializable SELECT should record durable read access") && ok;
  ok = Require(HasEvidence(range_read.evidence,
                           "serializable.ledger_persisted",
                           "true"),
               "serializable SELECT should persist access ledger") && ok;

  auto phantom_writer = Begin(fixture, "eler021-phantom-writer");
  const auto phantom = InsertRow(fixture, phantom_writer, "150", "phantom");
  ok = Require(!phantom.ok,
               "insert inside active serializable range should be refused") && ok;
  ok = Require(FirstDiagnosticCode(phantom) ==
                   "SB-SNTXN-SERIALIZABLE-PHANTOM-REFUSED",
               "phantom refusal should surface through EngineInsertRows") && ok;
  ok = Require(HasEvidence(phantom.evidence,
                           "serializable.conflict",
                           "phantom_insert"),
               "phantom refusal should expose conflict evidence") && ok;

  auto outside_writer = Begin(fixture, "eler021-outside-writer");
  const auto outside = InsertRow(fixture, outside_writer, "250", "outside");
  if (!outside.ok) PrintDiagnostics(outside);
  ok = Require(outside.ok,
               "insert outside active serializable range should be admitted") && ok;
  ok = Require(HasEvidence(outside.evidence,
                           "serializable.admission",
                           "recorded"),
               "admitted insert should record durable write access") && ok;

  auto update_writer = Begin(fixture, "eler021-update-writer");
  const auto update = UpdateRow(fixture, update_writer, "120");
  ok = Require(!update.ok,
               "update inside active serializable range should be refused") && ok;
  ok = Require(FirstDiagnosticCode(update) ==
                   "SB-SNTXN-SERIALIZABLE-READ-WRITE-CONFLICT",
               "update refusal should surface read-write diagnostic") && ok;
  ok = Require(HasEvidence(update.evidence,
                           "serializable.retry_class",
                           "serialization_retry"),
               "update conflict should classify retry") && ok;

  auto delete_writer = Begin(fixture, "eler021-delete-writer");
  const auto deleted = DeleteRow(fixture, delete_writer, "130");
  ok = Require(!deleted.ok,
               "delete inside active serializable range should be refused") && ok;
  ok = Require(FirstDiagnosticCode(deleted) ==
                   "SB-SNTXN-SERIALIZABLE-READ-WRITE-CONFLICT",
               "delete refusal should surface read-write diagnostic") && ok;

  auto external = Begin(fixture, "eler021-external-authority");
  scratchbird::tests::FixtureEngineRequest<api::EngineInsertRowsRequest> external_request(
      *fixture.engine_session, external);
  external_request.target_table.uuid = fixture.table_uuid;
  external_request.target_table.object_kind = "table";
  external_request.input_rows.push_back(Row("300", "external"));
  external_request.estimated_row_count = 1;
  external_request.option_envelopes = DmlOptions();
  external_request.option_envelopes.push_back(
      "serializable.parser_or_reference_authority=true");
  const auto external_insert = api::EngineInsertRows(external_request);
  ok = Require(!external_insert.ok,
               "parser/reference serializable authority should fail closed in DML path") && ok;
  ok = Require(FirstDiagnosticCode(external_insert) ==
                   "SB-SNTXN-SERIALIZABLE-EXTERNAL-AUTHORITY-REFUSED",
               "external authority refusal should surface through EngineInsertRows") && ok;

  const auto ledger_path =
      std::filesystem::path(fixture.database_path.string() +
                            ".sb.serializable_access");
  ok = Require(std::filesystem::exists(ledger_path),
               "serializable access ledger should be durable beside database") && ok;
  return ok;
}

bool SerializablePhantomAndReadWriteConflictProof() {
  bool ok = true;
  const TypedUuid relation = MakeUuid(UuidKind::object, 10);
  txn::SerializableConflictTracker tracker;
  const auto reader = txn::MakeLocalTransactionId(1);
  const auto writer = txn::MakeLocalTransactionId(2);

  const auto read = tracker.RecordAccess(
      Access(reader,
             txn::TransactionState::active,
             txn::SerializableAccessKind::range_read,
             txn::MakeSerializableBoundedRange(relation, "100", "200"),
             1));
  ok = Require(read.ok(), "serializable range read should be recorded") && ok;

  const auto phantom = tracker.CheckWrite(
      Access(writer,
             txn::TransactionState::active,
             txn::SerializableAccessKind::insert,
             txn::MakeSerializablePointRange(relation, "150"),
             2));
  ok = Require(!phantom.ok(), "insert inside range read should be refused") && ok;
  ok = Require(phantom.conflict == txn::SerializableConflictKind::phantom_insert,
               "insert inside range should classify as phantom") && ok;
  ok = Require(phantom.retry_class == txn::SerializableRetryClass::serialization_retry,
               "phantom should be serialization retry") && ok;
  ok = Require(phantom.diagnostic.diagnostic_code ==
                   "SB-SNTXN-SERIALIZABLE-PHANTOM-REFUSED",
               "phantom diagnostic should be stable") && ok;

  const auto outside = tracker.RecordWrite(
      Access(writer,
             txn::TransactionState::active,
             txn::SerializableAccessKind::insert,
             txn::MakeSerializablePointRange(relation, "250"),
             3));
  ok = Require(outside.ok(), "insert outside range should be admitted") && ok;
  ok = Require(tracker.access_count() == 2,
               "admitted write should be recorded") && ok;

  const auto update_conflict = tracker.CheckWrite(
      Access(writer,
             txn::TransactionState::active,
             txn::SerializableAccessKind::update,
             txn::MakeSerializablePointRange(relation, "120"),
             4));
  ok = Require(!update_conflict.ok(),
               "update inside range read should be refused") && ok;
  ok = Require(update_conflict.conflict == txn::SerializableConflictKind::read_write,
               "update inside range should classify as read-write") && ok;
  ok = Require(update_conflict.retry_class ==
                   txn::SerializableRetryClass::serialization_retry,
               "read-write conflict should request serialization retry") && ok;
  return ok;
}

bool SerializableWriteWriteAndSelfAccessProof() {
  bool ok = true;
  const TypedUuid relation = MakeUuid(UuidKind::object, 20);
  txn::SerializableConflictTracker tracker;
  const auto first = txn::MakeLocalTransactionId(10);
  const auto second = txn::MakeLocalTransactionId(11);

  const auto first_write = tracker.RecordWrite(
      Access(first,
             txn::TransactionState::active,
             txn::SerializableAccessKind::update,
             txn::MakeSerializablePointRange(relation, "42"),
             1));
  ok = Require(first_write.ok(), "first write should be admitted") && ok;

  const auto second_write = tracker.CheckWrite(
      Access(second,
             txn::TransactionState::active,
             txn::SerializableAccessKind::delete_row,
             txn::MakeSerializablePointRange(relation, "42"),
             2));
  ok = Require(!second_write.ok(), "overlapping write should be refused") && ok;
  ok = Require(second_write.conflict == txn::SerializableConflictKind::write_write,
               "overlapping write should classify as write-write") && ok;
  ok = Require(second_write.retry_class ==
                   txn::SerializableRetryClass::wait_for_transaction,
               "active write-write conflict should wait for finality") && ok;

  const auto self_read = tracker.RecordAccess(
      Access(first,
             txn::TransactionState::active,
             txn::SerializableAccessKind::range_read,
             txn::MakeSerializableBoundedRange(relation, "1", "99"),
             3));
  ok = Require(self_read.ok(), "same transaction read should be recorded") && ok;
  const auto self_write = tracker.CheckWrite(
      Access(first,
             txn::TransactionState::active,
             txn::SerializableAccessKind::insert,
             txn::MakeSerializablePointRange(relation, "50"),
             4));
  ok = Require(self_write.ok(), "same transaction write should not conflict with own read") && ok;
  return ok;
}

bool SerializableRecoveryAndAuthorityRefusalProof() {
  bool ok = true;
  const TypedUuid relation = MakeUuid(UuidKind::object, 30);
  txn::SerializableConflictTracker tracker;

  const auto limbo_read = tracker.RecordAccess(
      Access(txn::MakeLocalTransactionId(20),
             txn::TransactionState::limbo,
             txn::SerializableAccessKind::predicate_read,
             txn::MakeSerializablePredicateRange(relation, "sha256:predicate-all-open-orders"),
             1));
  ok = Require(limbo_read.ok(), "limbo predicate read should be recorded as recovery state") && ok;

  const auto recovery = tracker.CheckWrite(
      Access(txn::MakeLocalTransactionId(21),
             txn::TransactionState::active,
             txn::SerializableAccessKind::insert,
             txn::MakeSerializablePointRange(relation, "order:50"),
             2));
  ok = Require(!recovery.ok(), "write overlapping limbo predicate should require recovery") && ok;
  ok = Require(recovery.retry_class == txn::SerializableRetryClass::recovery_required,
               "limbo conflict should classify recovery required") && ok;

  auto external = Access(txn::MakeLocalTransactionId(22),
                         txn::TransactionState::active,
                         txn::SerializableAccessKind::range_read,
                         txn::MakeSerializableBoundedRange(relation, "a", "z"),
                         3);
  external.parser_or_reference_authority = true;
  const auto external_result = tracker.RecordAccess(external);
  ok = Require(!external_result.ok(),
               "parser or reference serializable authority should fail closed") && ok;
  ok = Require(external_result.conflict ==
                   txn::SerializableConflictKind::external_authority_refused,
               "external authority refusal should be classified") && ok;

  auto missing_inventory = external;
  missing_inventory.parser_or_reference_authority = false;
  missing_inventory.durable_inventory_authoritative = false;
  const auto missing_inventory_result = tracker.RecordAccess(missing_inventory);
  ok = Require(!missing_inventory_result.ok(),
               "serializable access without durable inventory should fail closed") && ok;
  ok = Require(missing_inventory_result.conflict ==
                   txn::SerializableConflictKind::inventory_authority_required,
               "missing inventory authority should be classified") && ok;
  ok = Require(HasEvidence(recovery,
                           "serializable.inventory_authority=durable_transaction_inventory"),
               "recovery conflict evidence should name inventory authority") && ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = Require(txn::ValidateLocalIsolationLevel(txn::IsolationLevel::serializable).ok(),
               "serializable isolation should be supported") && ok;
  ok = SerializablePhantomAndReadWriteConflictProof() && ok;
  ok = SerializableWriteWriteAndSelfAccessProof() && ok;
  ok = SerializableRecoveryAndAuthorityRefusalProof() && ok;
  ok = SerializableIntegratedEngineApiProof() && ok;
  if (!ok) {
    return EXIT_FAILURE;
  }
  std::cout << "engine_listener_serializable_isolation_conformance=passed\n";
  return EXIT_SUCCESS;
}
