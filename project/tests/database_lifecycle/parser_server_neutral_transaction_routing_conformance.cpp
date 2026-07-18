// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "ipc_server.hpp"
#include "local_transaction_store.hpp"
#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "extensibility/executable_object_lifecycle.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_state.hpp"
#include "database_lifecycle.hpp"
#include "resource_seed_pack.hpp"
#include "memory.hpp"
#include "uuid.hpp"
#include "parser_server_client.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace ipc = scratchbird::parser::ipc;
namespace sbps = scratchbird::server::sbps;
namespace server = scratchbird::server;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace mga = scratchbird::transaction::mga;
namespace memory = scratchbird::core::memory;
namespace platform = scratchbird::core::platform;
namespace resources = scratchbird::core::resources;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

template <typename TResult>
void RequireEngineOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Fail(message);
  }
}

platform::TypedUuid NewTypedUuid(platform::UuidKind kind,
                                 std::uint64_t salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, 1944000000000ull + salt);
  Require(generated.ok(), "neutral transaction UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, std::uint64_t salt) {
  return uuid::UuidToString(NewTypedUuid(kind, salt).value);
}

struct EngineTransactionFixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string schema_uuid;
  std::string table_uuid;
  std::string resource_table_uuid;
  std::string routine_table_uuid;
  std::string routine_column_uuid;
  std::string gbk_charset_uuid;
  std::string gbk_default_collation_uuid;
  std::string gbk_unicode_collation_uuid;
  std::uint64_t resource_epoch = 0;
  std::uint64_t salt = 0;

  EngineTransactionFixture() = default;
  EngineTransactionFixture(const EngineTransactionFixture&) = delete;
  EngineTransactionFixture& operator=(const EngineTransactionFixture&) = delete;
  EngineTransactionFixture(EngineTransactionFixture&& other) noexcept
      : directory(std::move(other.directory)),
        database_path(std::move(other.database_path)),
        database_uuid(std::move(other.database_uuid)),
        schema_uuid(std::move(other.schema_uuid)),
        table_uuid(std::move(other.table_uuid)),
        resource_table_uuid(std::move(other.resource_table_uuid)),
        routine_table_uuid(std::move(other.routine_table_uuid)),
        routine_column_uuid(std::move(other.routine_column_uuid)),
        gbk_charset_uuid(std::move(other.gbk_charset_uuid)),
        gbk_default_collation_uuid(
            std::move(other.gbk_default_collation_uuid)),
        gbk_unicode_collation_uuid(
            std::move(other.gbk_unicode_collation_uuid)),
        resource_epoch(other.resource_epoch),
        salt(other.salt) {
    other.directory.clear();
  }

  ~EngineTransactionFixture() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
};

EngineTransactionFixture CreateEngineTransactionFixture() {
  EngineTransactionFixture fixture;
  fixture.salt = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  fixture.directory = std::filesystem::temp_directory_path() /
                      ("scratchbird_parser_neutral_transaction_" +
                       std::to_string(fixture.salt));
  std::filesystem::create_directories(fixture.directory);
  fixture.database_path = fixture.directory / "neutral.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid =
      NewTypedUuid(platform::UuidKind::database, fixture.salt + 1);
  create.filespace_uuid =
      NewTypedUuid(platform::UuidKind::filespace, fixture.salt + 2);
  create.creation_unix_epoch_millis = 1944000000000ull + fixture.salt + 3;
  create.page_size = 8192;
  create.resource_seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
  create.require_resource_seed_pack = true;
  create.allow_minimal_resource_bootstrap = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key;
    for (const auto& argument : created.diagnostic.arguments) {
      std::cerr << ':' << argument.key << '=' << argument.value;
    }
    std::cerr << '\n';
  }
  Require(created.ok(), "neutral transaction database creation failed");
  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.resource_epoch = created.state.resource_seed_catalog.resource_epoch;
  const auto* gbk = resources::FindResourceSeedCharset(
      created.state.resource_seed_catalog, "GBK");
  const auto* gbk_default = resources::FindResourceSeedCollation(
      created.state.resource_seed_catalog, "GBK");
  const auto* gbk_unicode = resources::FindResourceSeedCollation(
      created.state.resource_seed_catalog, "GBK_UNICODE");
  Require(fixture.resource_epoch != 0 && gbk != nullptr &&
              gbk_default != nullptr && gbk_unicode != nullptr &&
              !gbk->resource_uuid.empty() &&
              gbk_default->charset_uuid == gbk->resource_uuid &&
              gbk_unicode->charset_uuid == gbk->resource_uuid,
          "neutral transaction resource authority is incomplete");
  fixture.gbk_charset_uuid = gbk->resource_uuid;
  fixture.gbk_default_collation_uuid = gbk_default->resource_uuid;
  fixture.gbk_unicode_collation_uuid = gbk_unicode->resource_uuid;
  return fixture;
}

api::EngineRequestContext BeginEngineTransaction(
    const EngineTransactionFixture& fixture,
    std::uint64_t ordinal) {
  api::EngineBeginTransactionRequest begin;
  begin.context.trust_mode = api::EngineTrustMode::server_isolated;
  begin.context.request_id = "neutral-engine-begin-" +
                             std::to_string(ordinal);
  begin.context.database_path = fixture.database_path.string();
  begin.context.database_uuid.canonical = fixture.database_uuid;
  begin.context.principal_uuid.canonical = NewUuidText(
      platform::UuidKind::principal, fixture.salt + 100 + ordinal);
  begin.context.session_uuid.canonical = NewUuidText(
      platform::UuidKind::object, fixture.salt + 200 + ordinal);
  begin.context.security_context_present = true;
  begin.context.catalog_generation_id = 1;
  begin.context.security_epoch = 1;
  begin.context.resource_epoch = fixture.resource_epoch;
  begin.context.name_resolution_epoch = 1;
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  RequireEngineOk(begun, "neutral engine transaction begin failed");
  Require(begun.local_transaction_id != 0 &&
              !begun.transaction_uuid.canonical.empty(),
          "engine begin did not issue a composite transaction identity");
  auto context = begin.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  return context;
}

api::EngineLocalizedName NeutralName(std::string value) {
  api::EngineLocalizedName name;
  name.language_tag = "en";
  name.name_class = "primary";
  name.name = value;
  name.raw_name_text = value;
  name.display_name = value;
  name.default_name = true;
  return name;
}

api::EngineColumnDefinition NeutralTextColumn(std::string name,
                                              std::uint32_t ordinal) {
  api::EngineColumnDefinition column;
  column.names.push_back(NeutralName(std::move(name)));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "text";
  column.descriptor.encoded_descriptor = "type=text";
  column.ordinal = ordinal;
  column.nullable = false;
  return column;
}

api::EngineColumnDefinition NeutralIntegerColumn(std::string name,
                                                 std::uint32_t ordinal) {
  api::EngineColumnDefinition column;
  column.names.push_back(NeutralName(std::move(name)));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "integer";
  column.descriptor.encoded_descriptor = "type=integer";
  column.ordinal = ordinal;
  column.nullable = false;
  return column;
}

api::EngineTypedValue NeutralIntegerValue(std::int64_t value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "integer";
  typed.descriptor.encoded_descriptor = "type=integer";
  typed.encoded_value = std::to_string(value);
  return typed;
}

api::EngineColumnDefinition NeutralResourceTextColumn(
    std::string name,
    std::uint32_t ordinal,
    std::string_view charset_uuid,
    std::string_view collation_uuid = {}) {
  api::EngineColumnDefinition column;
  column.names.push_back(NeutralName(std::move(name)));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "VARCHAR(20)";
  column.descriptor.encoded_descriptor =
      "type=VARCHAR(20);charset_uuid=" + std::string(charset_uuid);
  if (!collation_uuid.empty()) {
    column.descriptor.encoded_descriptor +=
        ";collation_uuid=" + std::string(collation_uuid);
  }
  column.descriptor.encoded_descriptor += ";character_length=20";
  column.ordinal = ordinal;
  column.nullable = true;
  return column;
}

void CreateNeutralVisibilityTable(EngineTransactionFixture* fixture) {
  Require(fixture != nullptr, "neutral visibility fixture is required");
  const auto context = BeginEngineTransaction(*fixture, 20);

  api::EngineCreateSchemaRequest schema;
  schema.context = context;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(NeutralName("neutral_visibility"));
  const auto created_schema = api::EngineCreateSchema(schema);
  RequireEngineOk(created_schema,
                  "neutral visibility schema creation failed");
  fixture->schema_uuid = created_schema.primary_object.uuid.canonical;
  Require(!fixture->schema_uuid.empty(),
          "engine did not issue the neutral visibility schema UUID");

  api::EngineCreateTableRequest table;
  table.context = context;
  table.target_schema.uuid.canonical = fixture->schema_uuid;
  table.target_schema.object_kind = "schema";
  table.table_names.push_back(NeutralName("selector_visibility"));
  table.table_columns.push_back(NeutralTextColumn("id", 0));
  const auto created_table = api::EngineCreateTable(table);
  RequireEngineOk(created_table,
                  "neutral visibility table creation failed");
  fixture->table_uuid = created_table.primary_object.uuid.canonical;
  Require(!fixture->table_uuid.empty(),
          "engine did not issue the neutral visibility table UUID");

  api::EngineCreateTableRequest resource_table;
  resource_table.context = context;
  resource_table.target_schema.uuid.canonical = fixture->schema_uuid;
  resource_table.target_schema.object_kind = "schema";
  resource_table.table_names.push_back(
      NeutralName("neutral_resource_projection"));
  auto f1 = NeutralResourceTextColumn(
      "f1", 0, fixture->gbk_charset_uuid);
  f1.nullable = false;
  resource_table.table_columns.push_back(std::move(f1));
  resource_table.table_columns.push_back(NeutralResourceTextColumn(
      "f2",
      1,
      fixture->gbk_charset_uuid,
      fixture->gbk_unicode_collation_uuid));
  const auto created_resource_table =
      api::EngineCreateTable(resource_table);
  RequireEngineOk(created_resource_table,
                  "neutral resource projection table creation failed");
  fixture->resource_table_uuid =
      created_resource_table.primary_object.uuid.canonical;
  Require(!fixture->resource_table_uuid.empty(),
          "engine did not issue the neutral resource table UUID");

  api::EngineCommitTransactionRequest commit;
  commit.context = context;
  RequireEngineOk(api::EngineCommitTransaction(commit),
                  "neutral visibility metadata commit failed");
}

void CreateNeutralRoutineTable(EngineTransactionFixture* fixture) {
  Require(fixture != nullptr && !fixture->schema_uuid.empty(),
          "neutral routine fixture requires a committed schema");
  const auto context = BeginEngineTransaction(*fixture, 40);

  api::EngineCreateTableRequest table;
  table.context = context;
  table.target_schema.uuid.canonical = fixture->schema_uuid;
  table.target_schema.object_kind = "schema";
  table.table_names.push_back(NeutralName("neutral_routine_values"));
  table.table_columns.push_back(NeutralIntegerColumn("a", 0));
  const auto created = api::EngineCreateTable(table);
  RequireEngineOk(created, "neutral routine table creation failed");
  fixture->routine_table_uuid = created.primary_object.uuid.canonical;
  Require(!fixture->routine_table_uuid.empty(),
          "engine did not publish the neutral routine table UUID");

  api::EngineInsertRowsRequest insert;
  insert.context = context;
  insert.target_table.uuid.canonical = fixture->routine_table_uuid;
  insert.target_table.object_kind = "table";
  for (std::int64_t value = 1; value <= 10; ++value) {
    api::EngineRowValue row;
    row.fields.push_back({"a", NeutralIntegerValue(value)});
    insert.input_rows.push_back(std::move(row));
  }
  const auto inserted = api::EngineInsertRows(insert);
  RequireEngineOk(inserted, "neutral routine fixture insert failed");
  Require(inserted.inserted_count == 10,
          "neutral routine fixture did not insert ten rows");

  const auto descriptor = api::LoadMgaRelationStorageDescriptor(
      context, fixture->routine_table_uuid);
  Require(descriptor.ok &&
              descriptor.descriptor.relation_uuid.canonical ==
                  fixture->routine_table_uuid &&
              descriptor.descriptor.columns.size() == 1 &&
              descriptor.descriptor.columns.front().canonical_name_key == "a" &&
              !descriptor.descriptor.columns.front()
                   .column_uuid.canonical.empty(),
          "neutral routine fixture lacks its persisted column descriptor");
  fixture->routine_column_uuid =
      descriptor.descriptor.columns.front().column_uuid.canonical;

  api::EngineCommitTransactionRequest commit;
  commit.context = context;
  RequireEngineOk(api::EngineCommitTransaction(commit),
                  "neutral routine fixture commit failed");
}

bool InventoryContainsExactActiveTransaction(
    const EngineTransactionFixture& fixture,
    const api::EngineRequestContext& context) {
  const auto loaded = db::LoadLocalTransactionInventoryFromDatabase(
      fixture.database_path.string());
  if (!loaded.ok()) return false;
  for (const auto& entry : loaded.inventory.entries) {
    if (entry.identity.local_id.value != context.local_transaction_id) {
      continue;
    }
    const bool active = entry.state == mga::TransactionState::active ||
                        entry.state == mga::TransactionState::read_only_active;
    return active &&
           uuid::UuidToString(entry.identity.transaction_uuid.value) ==
               context.transaction_uuid.canonical;
  }
  return false;
}

void RequireCompositeRefusal(const api::EngineCommitTransactionResult& result,
                             std::string_view message) {
  Require(!result.ok && result.engine_finality_known &&
              result.commit_finality_state ==
                  "refused_before_inventory_commit" &&
              result.local_transaction_id == 0 &&
              result.transaction_uuid.canonical.empty(),
          message);
}

void RequireCompositeRefusal(
    const api::EngineRollbackTransactionResult& result,
    std::string_view message) {
  Require(!result.ok && result.engine_finality_known &&
              result.rollback_finality_state ==
                  "refused_before_inventory_rollback" &&
              result.local_transaction_id == 0 &&
              result.transaction_uuid.canonical.empty(),
          message);
}

void PutU8(std::vector<std::uint8_t>* out, std::uint8_t value) {
  out->push_back(value);
}

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void PutUuid(std::vector<std::uint8_t>* out,
             const std::array<std::uint8_t, 16>& value) {
  out->insert(out->end(), value.begin(), value.end());
}

void PutString(std::vector<std::uint8_t>* out, std::string_view value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

void PutBytes(std::vector<std::uint8_t>* out,
              const std::vector<std::uint8_t>& value) {
  PutU64(out, value.size());
  out->insert(out->end(), value.begin(), value.end());
}

server::ServerTransactionState Transaction(std::uint64_t id,
                                           std::string uuid) {
  server::ServerTransactionState transaction;
  transaction.local_transaction_id = id;
  transaction.snapshot_visible_through_local_transaction_id = id;
  transaction.transaction_uuid = std::move(uuid);
  transaction.isolation_level = "read_committed";
  return transaction;
}

server::ServerSessionRecord Session(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& channel_uuid,
    server::ServerTransactionState transaction) {
  server::ServerSessionRecord session;
  session.session_uuid = session_uuid;
  session.connection_uuid = session_uuid;
  session.server_channel_uuid = channel_uuid;
  session.transaction_routing_v2_negotiated = true;
  session.local_transaction_id = transaction.local_transaction_id;
  session.snapshot_visible_through_local_transaction_id =
      transaction.snapshot_visible_through_local_transaction_id;
  session.transaction_uuid = transaction.transaction_uuid;
  session.default_local_transaction_id = transaction.local_transaction_id;
  session.transactions_by_local_id.emplace(transaction.local_transaction_id,
                                           std::move(transaction));
  return session;
}

sbps::Frame ExecuteFrameV2(
    const std::array<std::uint8_t, 16>& session_uuid,
    std::string_view operation_id,
    const server::ServerTransactionState& transaction) {
  sbps::Frame frame;
  frame.header.message_type =
      static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.payload_schema_id = 4011;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session_uuid;
  frame.header.session_uuid = session_uuid;
  PutUuid(&frame.payload, session_uuid);
  PutUuid(&frame.payload, {});
  PutU8(&frame.payload, 0);
  PutU8(&frame.payload, 1);
  PutU64(&frame.payload, transaction.local_transaction_id);
  PutString(&frame.payload, transaction.transaction_uuid);
  std::string envelope = "operation_id=" + std::string(operation_id) + "\n";
  PutString(&frame.payload, envelope);
  PutBytes(&frame.payload, {});
  return frame;
}

sbps::Frame ClosePreparedFrame(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& prepared_statement_uuid) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(
      sbps::MessageType::kClosePreparedSblr);
  frame.header.payload_schema_id = 4013;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session_uuid;
  frame.header.session_uuid = session_uuid;
  frame.payload = server::EncodeClosePreparedSblrPayloadForTest(
      session_uuid, prepared_statement_uuid);
  return frame;
}

std::vector<std::uint8_t> KnownAppliedCommitPayload() {
  std::vector<std::uint8_t> payload;
  PutString(&payload, "rejected");
  PutUuid(&payload, sbps::MakeUuidV7Bytes());
  PutUuid(&payload, {});
  PutU64(&payload, 0);
  PutString(&payload, "transaction.commit");
  PutString(&payload, "");
  PutString(&payload, "");
  PutU8(&payload, (1u << 0) | (1u << 1) | (1u << 2));
  PutU8(&payload, 1);
  PutU8(&payload, 0);
  PutU64(&payload, 91);
  PutString(&payload, "019f0000-0000-7000-8000-000000000091");
  PutU64(&payload, 91);
  PutString(&payload, "019f0000-0000-7000-8000-000000000091");
  PutString(&payload, "committed_by_engine_inventory");
  PutString(&payload, "SBWP.COMMIT.POST_INVENTORY_SECONDARY_FAILURE");
  return payload;
}

void VerifyNeutralCodecAndPreEngineFinality() {
  ipc::ServerExecutionResult decoded;
  ipc::MessageVectorSet messages;
  Require(ipc::DecodeExecuteResultPayloadV2ForTest(
              KnownAppliedCommitPayload(), &decoded, &messages) &&
              decoded.finality_state ==
                  ipc::ParserTransactionFinality::kKnownApplied &&
              !decoded.accepted &&
              decoded.transaction_diagnostic_code ==
                  "SBWP.COMMIT.POST_INVENTORY_SECONDARY_FAILURE" &&
              !messages.diagnostics.empty() &&
              messages.diagnostics.front().code ==
                  "SBWP.COMMIT.POST_INVENTORY_SECONDARY_FAILURE" &&
              !decoded.transaction_state_present,
          "neutral V2 codec lost coherent typed finality or its diagnostic");
  Require(!ipc::V2RequestMayRetryAfterWriteForTest(4009) &&
              !ipc::V2RequestMayRetryAfterWriteForTest(4011) &&
              !ipc::V2RequestMayRetryAfterWriteForTest(7005) &&
              ipc::V2RequestMayRetryAfterWriteForTest(4013) &&
              !ipc::SessionBoundRequestMayRetryAfterWriteForTest(4001) &&
              !ipc::SessionBoundRequestMayRetryAfterWriteForTest(4007) &&
              !ipc::SessionBoundRequestMayRetryAfterWriteForTest(4013),
          "neutral transport policy permits a session-bound request to replay on a replacement physical route");

  const auto session_uuid = sbps::MakeUuidV7Bytes();
  const auto transaction = Transaction(
      91, "019f0000-0000-7000-8000-000000000091");
  const auto refused = server::RejectExecuteSblrBeforeEngine(
      ExecuteFrameV2(session_uuid, "transaction.commit", transaction),
      "SERVER.MAINTENANCE.SBLR_ADMISSION_FENCED",
      "sblr_admission_fenced");
  Require(!refused.accepted && refused.response_schema_id == 4012 &&
              (refused.frame_flags & sbps::kFlagError) == 0 &&
              refused.transaction_state.has_value() &&
              refused.transaction_state->finality ==
                  server::ServerTransactionResponseState::Finality::
                      kKnownNotApplied &&
              refused.transaction_state->diagnostic_code ==
                  "SERVER.MAINTENANCE.SBLR_ADMISSION_FENCED",
          "neutral pre-engine refusal lost typed known-not-applied finality");
}

void VerifySessionOwnedPreparedCloseAfterFinality() {
  server::ServerSessionRegistry registry;
  const auto session_uuid = sbps::MakeUuidV7Bytes();
  auto session = Session(
      session_uuid,
      sbps::MakeUuidV7Bytes(),
      Transaction(202, "019f0000-0000-7000-8000-000000000202"));
  session.database_uuid = "neutral-close-database";
  registry.sessions_by_uuid[server::UuidBytesToText(session_uuid)] = session;
  auto& stored_session = registry.sessions_by_uuid.at(
      server::UuidBytesToText(session_uuid));

  const auto shared_handle = server::AllocateSessionObjectHandle(
      &registry,
      stored_session,
      "019f0000-0000-7000-8000-000000000301",
      "relation",
      "crud.select",
      "columns/all");
  Require(shared_handle.handle_id != 0 && shared_handle.generation != 0,
          "prepared-close fixture did not allocate a shared session object handle");

  const auto first_uuid = sbps::MakeUuidV7Bytes();
  const auto second_uuid = sbps::MakeUuidV7Bytes();
  auto prepared_record = [&](const std::array<std::uint8_t, 16>& uuid_value) {
    server::ServerPreparedStatementRecord prepared;
    prepared.prepared_statement_uuid = uuid_value;
    prepared.session_uuid = session_uuid;
    prepared.database_uuid = stored_session.database_uuid;
    prepared.encoded_sblr_envelope = "operation_id=crud.select\n";
    prepared.operation_id = "crud.select";
    prepared.session_object_handle_id = shared_handle.handle_id;
    prepared.session_object_handle_generation = shared_handle.generation;
    // This selector has already finalized.  The live session contains only
    // replacement selector 202, and close carries neither identity.
    prepared.prepare_local_transaction_id = 201;
    prepared.prepare_transaction_uuid =
        "019f0000-0000-7000-8000-000000000201";
    prepared.prepare_snapshot_visible_through_local_transaction_id = 201;
    return prepared;
  };
  registry.prepared_by_uuid[server::UuidBytesToText(first_uuid)] =
      prepared_record(first_uuid);
  registry.prepared_by_uuid[server::UuidBytesToText(second_uuid)] =
      prepared_record(second_uuid);
  for (const auto& prepared_uuid : {first_uuid, second_uuid}) {
    server::ServerPreparedExecutionContextRecord context;
    context.prepared_statement_uuid = prepared_uuid;
    context.session_uuid = session_uuid;
    context.database_uuid = stored_session.database_uuid;
    registry.prepared_execution_contexts_by_uuid
        [server::UuidBytesToText(prepared_uuid)] = context;
  }

  const auto cursor_uuid = sbps::MakeUuidV7Bytes();
  const auto cursor_request_uuid = sbps::MakeUuidV7Bytes();
  server::ServerCursorRecord cursor;
  cursor.cursor_uuid = cursor_uuid;
  cursor.request_uuid = cursor_request_uuid;
  cursor.session_uuid = session_uuid;
  cursor.prepared_statement_uuid = first_uuid;
  cursor.row_packet = "buffered-row-packet";
  cursor.bulk_stream_kind = "copy";
  cursor.bulk_reject_records = {"buffered-reject"};
  cursor.multi_result_kind = "procedure";
  cursor.warning_stream_kind = "warning";
  cursor.bulk_total_rows = 3;
  cursor.bulk_rejected_rows = 1;
  cursor.multi_result_count = 2;
  cursor.warning_count = 1;
  cursor.total_row_count = 3;
  cursor.next_row_index = 1;
  cursor.fetch_count = 1;
  registry.cursors_by_uuid[server::UuidBytesToText(cursor_uuid)] = cursor;
  server::ServerRequestRecord cursor_request;
  cursor_request.request_uuid = cursor_request_uuid;
  cursor_request.session_uuid = session_uuid;
  cursor_request.cursor_uuid = cursor_uuid;
  cursor_request.state = server::ServerRequestLifecycleState::kCursorOpen;
  cursor_request.engine_result_retained = true;
  registry.requests_by_uuid[server::UuidBytesToText(cursor_request_uuid)] =
      cursor_request;

  const auto requests_before_zero_identity = registry.requests_by_uuid.size();
  const auto zero_identity = server::HandleClosePreparedSblr(
      &registry, ClosePreparedFrame(session_uuid, {}));
  Require(!zero_identity.accepted && !zero_identity.diagnostics.empty() &&
              zero_identity.diagnostics.front().code ==
                  "PARSER_SERVER_IPC.CLOSE_PREPARED_INVALID" &&
              registry.requests_by_uuid.size() ==
                  requests_before_zero_identity,
          "zero prepared UUID was not rejected before prepared lookup");

  const auto closed_first = server::HandleClosePreparedSblr(
      &registry, ClosePreparedFrame(session_uuid, first_uuid));
  const auto& first = registry.prepared_by_uuid.at(
      server::UuidBytesToText(first_uuid));
  const auto handle_key = server::UuidBytesToText(session_uuid) + "#" +
                          std::to_string(shared_handle.handle_id);
  Require(closed_first.accepted && closed_first.response_schema_id == 4014 &&
              first.closed && first.encoded_sblr_envelope.empty() &&
              first.prepare_local_transaction_id == 201 &&
              first.prepare_transaction_uuid ==
                  "019f0000-0000-7000-8000-000000000201" &&
              !registry.prepared_execution_contexts_by_uuid.contains(
                  server::UuidBytesToText(first_uuid)) &&
              registry.cursors_by_uuid.at(
                  server::UuidBytesToText(cursor_uuid)).closed &&
              registry.cursors_by_uuid.at(
                  server::UuidBytesToText(cursor_uuid)).row_packet.empty() &&
              registry.cursors_by_uuid.at(
                  server::UuidBytesToText(cursor_uuid))
                  .bulk_reject_records.empty() &&
              registry.cursors_by_uuid.at(
                  server::UuidBytesToText(cursor_uuid)).bulk_stream_kind.empty() &&
              registry.cursors_by_uuid.at(
                  server::UuidBytesToText(cursor_uuid)).multi_result_kind.empty() &&
              registry.cursors_by_uuid.at(
                  server::UuidBytesToText(cursor_uuid)).warning_stream_kind.empty() &&
              registry.cursors_by_uuid.at(
                  server::UuidBytesToText(cursor_uuid)).bulk_total_rows == 0 &&
              registry.cursors_by_uuid.at(
                  server::UuidBytesToText(cursor_uuid)).bulk_rejected_rows == 0 &&
              registry.cursors_by_uuid.at(
                  server::UuidBytesToText(cursor_uuid)).multi_result_count == 0 &&
              registry.cursors_by_uuid.at(
                  server::UuidBytesToText(cursor_uuid)).warning_count == 0 &&
              registry.cursors_by_uuid.at(
                  server::UuidBytesToText(cursor_uuid)).total_row_count == 0 &&
              registry.cursors_by_uuid.at(
                  server::UuidBytesToText(cursor_uuid)).next_row_index == 0 &&
              registry.cursors_by_uuid.at(
                  server::UuidBytesToText(cursor_uuid)).fetch_count == 0 &&
              !registry.requests_by_uuid.at(
                   server::UuidBytesToText(cursor_request_uuid))
                   .engine_result_retained &&
              !registry.object_handles_by_key.at(handle_key).closed &&
              stored_session.transactions_by_local_id.size() == 1 &&
              stored_session.transactions_by_local_id.contains(202) &&
              stored_session.default_local_transaction_id == 202 &&
              stored_session.local_transaction_id == 202,
          "session-owned prepared close depended on the finalized prepare selector or altered MGA state");

  const auto repeated = server::HandleClosePreparedSblr(
      &registry, ClosePreparedFrame(session_uuid, first_uuid));
  Require(repeated.accepted &&
              !registry.object_handles_by_key.at(handle_key).closed,
          "owned prepared close was not idempotent or revoked a shared live handle");

  const auto closed_second = server::HandleClosePreparedSblr(
      &registry, ClosePreparedFrame(session_uuid, second_uuid));
  Require(closed_second.accepted &&
              registry.object_handles_by_key.at(handle_key).closed &&
              registry.object_handles_by_key.at(handle_key).generation ==
                  shared_handle.generation + 1,
          "prepared close did not revoke the shared object handle at its last live reference");

  const auto other_session_uuid = sbps::MakeUuidV7Bytes();
  auto other_session = Session(
      other_session_uuid,
      sbps::MakeUuidV7Bytes(),
      Transaction(203, "019f0000-0000-7000-8000-000000000203"));
  other_session.database_uuid = stored_session.database_uuid;
  registry.sessions_by_uuid[server::UuidBytesToText(other_session_uuid)] =
      other_session;
  const auto cross_session = server::HandleClosePreparedSblr(
      &registry, ClosePreparedFrame(other_session_uuid, first_uuid));
  Require(!cross_session.accepted && !cross_session.diagnostics.empty() &&
              cross_session.diagnostics.front().code ==
                  "PARSER_SERVER_IPC.PREPARED_STATEMENT_NOT_FOUND",
          "cross-session prepared close disclosed or retired another session's identity");
}

void VerifyChannelScopedQuarantine() {
  server::ServerSessionRegistry registry;
  registry.channel_state = server::ServerChannelState::kReady;
  const auto channel_a = sbps::MakeUuidV7Bytes();
  const auto channel_b = sbps::MakeUuidV7Bytes();
  const auto session_a_uuid = sbps::MakeUuidV7Bytes();
  const auto session_b_uuid = sbps::MakeUuidV7Bytes();
  auto transaction_a = Transaction(
      101, "019f0000-0000-7000-8000-000000000101");
  transaction_a.lifecycle_state =
      server::ServerTransactionLifecycleState::kFinalityUnknown;
  auto session_a = Session(session_a_uuid, channel_a, transaction_a);
  auto session_b = Session(
      session_b_uuid,
      channel_b,
      Transaction(102, "019f0000-0000-7000-8000-000000000102"));
  registry.physical_channel_by_connection_uuid[
      server::UuidBytesToText(session_a_uuid)] = channel_a;
  registry.physical_channel_by_connection_uuid[
      server::UuidBytesToText(session_b_uuid)] = channel_b;
  registry.sessions_by_uuid[server::UuidBytesToText(session_a_uuid)] =
      session_a;
  registry.sessions_by_uuid[server::UuidBytesToText(session_b_uuid)] =
      session_b;

  const auto closed = server::HandleUnexpectedParserChannelClose(
      &registry, channel_a);
  Require(closed.size() == 1 && closed.front().accepted &&
              registry.channel_state == server::ServerChannelState::kReady &&
              registry.sessions_by_uuid.contains(
                  server::UuidBytesToText(session_b_uuid)) &&
              registry.sessions_by_uuid
                  .at(server::UuidBytesToText(session_a_uuid))
                  .detached_recovery_quarantined &&
              registry.physical_channel_by_connection_uuid.contains(
                  server::UuidBytesToText(session_b_uuid)),
          "one channel's unknown finality drained or detached a sibling channel");
}

void VerifyDefaultMapAndHelloInvariants() {
  const auto session_uuid = sbps::MakeUuidV7Bytes();
  auto session = Session(
      session_uuid,
      sbps::MakeUuidV7Bytes(),
      Transaction(111, "019f0000-0000-7000-8000-000000000111"));
  Require(server::AdoptAndFindExactActiveDefaultTransaction(&session) !=
              nullptr,
          "exact active default transaction was not recognized");
  Require(server::IsCompleteEngineTransactionIdentity(
              111, "019f0000-0000-7000-8000-000000000111") &&
              !server::IsCompleteEngineTransactionIdentity(
                  0, "019f0000-0000-7000-8000-000000000111") &&
              !server::IsCompleteEngineTransactionIdentity(
                  111, "not-a-transaction-uuid"),
          "server accepted an incomplete begin/attach transaction identity");
  session.default_local_transaction_id = 999;
  Require(server::AdoptAndFindExactActiveDefaultTransaction(&session) ==
              nullptr,
          "stale default map identity fell through to scalar authority");
  Require(server::ParserChannelHelloMayBeAdmittedForTest(
              false, false, false) &&
              server::ParserChannelHelloMayBeAdmittedForTest(
                  true, true, true) &&
              !server::ParserChannelHelloMayBeAdmittedForTest(
                  true, false, true) &&
              !server::ParserChannelHelloMayBeAdmittedForTest(
                  true, true, false) &&
              !server::ParserChannelHelloMayBeAdmittedForTest(
                  true, true, true, true),
          "hello did not preserve immutable physical-channel negotiation");
}

void VerifyDedicatedV2ClientChannelIdentity() {
  const std::string endpoint =
      "unix:/tmp/sb_neutral_v2_client_channel_no_server.sock";
  ipc::SbpsClient first(endpoint);
  ipc::SbpsClient second(endpoint);

  Require(!first.V2ChannelCacheKeyForTest().empty() &&
              !second.V2ChannelCacheKeyForTest().empty() &&
              first.V2ChannelCacheKeyForTest() !=
                  second.V2ChannelCacheKeyForTest(),
          "two V2 clients shared one physical-channel socket cache key");
  const auto first_hello = first.V2HelloPayloadForTest();
  const auto relation_v3_hello =
      first.RelationDescriptorV3HelloPayloadForTest();
  const auto decoded_v2_hello = sbps::DecodeHelloRequest(first_hello);
  const auto decoded_relation_v3_hello =
      sbps::DecodeHelloRequest(relation_v3_hello);
  Require(!first_hello.empty() &&
              first_hello == first.V2HelloPayloadForTest() &&
              decoded_v2_hello.has_value() &&
              decoded_relation_v3_hello.has_value() &&
              (decoded_v2_hello->capability_bitmap[0] &
               sbps::kCapabilityTransactionRoutingV2) != 0 &&
              (decoded_v2_hello->capability_bitmap[0] &
               sbps::kCapabilityRelationDescriptorProjectionV3) == 0 &&
              (decoded_relation_v3_hello->capability_bitmap[0] &
               sbps::kCapabilityTransactionRoutingV2) != 0 &&
              (decoded_relation_v3_hello->capability_bitmap[0] &
               sbps::kCapabilityRelationDescriptorProjectionV3) != 0,
          "one V2 client regenerated its immutable HELLO identity");

  ipc::ParserClientConfig config;
  config.require_transaction_routing_v2 = true;
  config.database_token = "neutral";
  ipc::AuthCredentialEnvelope credentials;
  credentials.principal = "neutral-v2-client";
  ipc::ParserSessionContext session;
  ipc::MessageVectorSet messages;
  Require(!first.AuthenticateAndAttach(credentials,
                                       config,
                                       &session,
                                       &messages) &&
              first.UsesDedicatedV2ChannelForTest() &&
              first_hello == first.V2HelloPayloadForTest(),
          "failed V2 authentication setup changed its channel or HELLO identity");
  messages.diagnostics.clear();
  Require(!first.AuthenticateAndAttach(credentials,
                                       config,
                                       &session,
                                       &messages) &&
              first.UsesDedicatedV2ChannelForTest() &&
              first_hello == first.V2HelloPayloadForTest() &&
              !second.UsesDedicatedV2ChannelForTest(),
          "V2 authentication retry did not reuse the same client-owned HELLO");
}

void VerifyEngineCompositeFinalityAuthority() {
  const auto fixture = CreateEngineTransactionFixture();

  const auto commit_context = BeginEngineTransaction(fixture, 1);
  api::EngineCommitTransactionRequest commit_policy_refusal;
  commit_policy_refusal.context = commit_context;
  commit_policy_refusal.policy_profile.encoded_profiles.push_back(
      "fail_closed:false");
  const auto commit_policy_result =
      api::EngineCommitTransaction(commit_policy_refusal);
  Require(!commit_policy_result.ok &&
              commit_policy_result.engine_finality_known &&
              commit_policy_result.commit_finality_state ==
                  "refused_before_inventory_commit" &&
              commit_policy_result.local_transaction_id ==
                  commit_context.local_transaction_id &&
              commit_policy_result.transaction_uuid.canonical ==
                  commit_context.transaction_uuid.canonical &&
              InventoryContainsExactActiveTransaction(fixture,
                                                      commit_context),
          "commit runtime-policy refusal was not exact known-not-applied");
  for (const auto& refused_uuid :
       std::vector<std::string>{
           {},
           "not-a-transaction-uuid",
           NewUuidText(platform::UuidKind::transaction,
                       fixture.salt + 301)}) {
    api::EngineCommitTransactionRequest commit;
    commit.context = commit_context;
    commit.context.transaction_uuid.canonical = refused_uuid;
    RequireCompositeRefusal(
        api::EngineCommitTransaction(commit),
        "commit accepted or echoed a non-exact composite transaction identity");
    Require(InventoryContainsExactActiveTransaction(fixture, commit_context),
            "commit identity refusal mutated the active inventory entry");
  }
  api::EngineRollbackTransactionRequest cleanup_commit_case;
  cleanup_commit_case.context = commit_context;
  RequireEngineOk(api::EngineRollbackTransaction(cleanup_commit_case),
                  "commit identity-refusal cleanup failed");

  const auto rollback_context = BeginEngineTransaction(fixture, 2);
  api::EngineRollbackTransactionRequest rollback_policy_refusal;
  rollback_policy_refusal.context = rollback_context;
  rollback_policy_refusal.policy_profile.encoded_profiles.push_back(
      "fail_closed:false");
  const auto rollback_policy_result =
      api::EngineRollbackTransaction(rollback_policy_refusal);
  Require(!rollback_policy_result.ok &&
              rollback_policy_result.engine_finality_known &&
              rollback_policy_result.rollback_finality_state ==
                  "refused_before_inventory_rollback" &&
              rollback_policy_result.local_transaction_id ==
                  rollback_context.local_transaction_id &&
              rollback_policy_result.transaction_uuid.canonical ==
                  rollback_context.transaction_uuid.canonical &&
              InventoryContainsExactActiveTransaction(fixture,
                                                      rollback_context),
          "rollback runtime-policy refusal was not exact known-not-applied");
  for (const auto& refused_uuid :
       std::vector<std::string>{
           {},
           "not-a-transaction-uuid",
           NewUuidText(platform::UuidKind::transaction,
                       fixture.salt + 302)}) {
    api::EngineRollbackTransactionRequest rollback;
    rollback.context = rollback_context;
    rollback.context.transaction_uuid.canonical = refused_uuid;
    RequireCompositeRefusal(
        api::EngineRollbackTransaction(rollback),
        "rollback accepted or echoed a non-exact composite transaction identity");
    Require(InventoryContainsExactActiveTransaction(fixture, rollback_context),
            "rollback identity refusal mutated the active inventory entry");
  }
  const std::string two_phase_wrong_uuid = NewUuidText(
      platform::UuidKind::transaction, fixture.salt + 303);
  api::EnginePrepareTransactionRequest prepare;
  prepare.context = rollback_context;
  prepare.context.transaction_uuid.canonical = two_phase_wrong_uuid;
  Require(!api::EnginePrepareTransaction(prepare).ok &&
              InventoryContainsExactActiveTransaction(fixture,
                                                      rollback_context),
          "prepare transaction did not fail closed on a wrong UUID");

  api::EngineExecuteTransactionBlockRequest execute_block;
  execute_block.context = rollback_context;
  execute_block.context.transaction_uuid.canonical = two_phase_wrong_uuid;
  Require(!api::EngineExecuteTransactionBlock(execute_block).ok &&
              InventoryContainsExactActiveTransaction(fixture,
                                                      rollback_context),
          "transaction block did not fail closed on a wrong UUID");

  api::EngineRollbackTransactionRequest cleanup_rollback_case;
  cleanup_rollback_case.context = rollback_context;
  RequireEngineOk(api::EngineRollbackTransaction(cleanup_rollback_case),
                  "rollback identity-refusal cleanup failed");
}

sbps::Frame RoutedExecuteFrameV2(
    const std::array<std::uint8_t, 16>& session_uuid,
    std::string_view encoded,
    std::uint8_t route,
    const server::ServerTransactionState* transaction = nullptr) {
  sbps::Frame frame;
  frame.header.message_type =
      static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.payload_schema_id = 4011;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session_uuid;
  frame.header.session_uuid = session_uuid;
  PutUuid(&frame.payload, session_uuid);
  PutUuid(&frame.payload, {});
  PutU8(&frame.payload, 0);
  PutU8(&frame.payload, route);
  PutU64(&frame.payload,
         transaction == nullptr ? 0 : transaction->local_transaction_id);
  PutString(&frame.payload,
            transaction == nullptr ? std::string_view{}
                                   : std::string_view(
                                         transaction->transaction_uuid));
  PutString(&frame.payload, encoded);
  PutBytes(&frame.payload, {});
  return frame;
}

sbps::Frame RoutedPrepareFrameV2(
    const std::array<std::uint8_t, 16>& session_uuid,
    std::string_view encoded,
    const server::ServerTransactionState& transaction) {
  sbps::Frame frame;
  frame.header.message_type =
      static_cast<std::uint16_t>(sbps::MessageType::kPrepareSblr);
  frame.header.payload_schema_id = 4009;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session_uuid;
  frame.header.session_uuid = session_uuid;
  PutUuid(&frame.payload, session_uuid);
  PutUuid(&frame.payload, sbps::MakeUuidV7Bytes());
  PutU64(&frame.payload, 1);
  PutU64(&frame.payload, 1);
  PutU64(&frame.payload, 1);
  PutU64(&frame.payload, transaction.local_transaction_id);
  PutString(&frame.payload, transaction.transaction_uuid);
  PutString(&frame.payload, encoded);
  return frame;
}

sbps::Frame RoutedExecutePreparedFrameV2(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& prepared_uuid,
    const server::ServerTransactionState& transaction) {
  sbps::Frame frame;
  frame.header.message_type =
      static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.payload_schema_id = 4011;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session_uuid;
  frame.header.session_uuid = session_uuid;
  PutUuid(&frame.payload, session_uuid);
  PutUuid(&frame.payload, prepared_uuid);
  PutU8(&frame.payload, 0);
  PutU8(&frame.payload, 1);
  PutU64(&frame.payload, transaction.local_transaction_id);
  PutString(&frame.payload, transaction.transaction_uuid);
  PutString(&frame.payload, std::string_view{});
  PutBytes(&frame.payload, {});
  return frame;
}

std::string NeutralOperationEnvelope(std::string_view operation_id,
                                     std::string_view opcode,
                                     std::string_view family,
                                     bool requires_transaction) {
  std::string out;
  out += "operation_id=";
  out += operation_id;
  out += "\nopcode=";
  out += opcode;
  out += "\nsblr_operation_family=";
  out += family;
  out += "\nresult_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=parser-neutral-v2-visibility\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += requires_transaction ? "requires_transaction_context=true\n"
                              : "requires_transaction_context=false\n";
  out += "requires_cluster_authority=false\n";
  return out;
}

std::string NeutralTransactionEnvelope(std::string_view operation_id,
                                       std::string_view opcode) {
  return NeutralOperationEnvelope(operation_id,
                                  opcode,
                                  "sblr.transaction.control.v3",
                                  true);
}

std::string NeutralProceduralBlockEnvelope(std::string_view fields) {
  std::string out =
      "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
      "\"envelope_major\":3,"
      "\"sblr_version\":\"sblr_v3\","
      "\"operation_id\":\"transaction.execute_block\","
      "\"opcode\":\"SBLR_TRANSACTION_EXECUTE_BLOCK\","
      "\"operation_family\":\"sblr.transaction.control.v3\","
      "\"sblr_operation_family\":\"sblr.transaction.control.v3\","
      "\"result_shape\":\"engine.api.result.v1\","
      "\"diagnostic_shape\":\"engine.diagnostic.v1\","
      "\"parser_resolved_names_to_uuids\":true,"
      "\"requires_security_context\":true,"
      "\"requires_transaction_context\":true,"
      "\"requires_cluster_authority\":false,"
      "\"contains_sql_text\":false,"
      "\"identifier_profile_uuid\":\"neutral_conformance\","
      "\"source_dialect\":\"neutral_conformance\"";
  if (!fields.empty()) {
    out += ',';
    out += fields;
  }
  out += '}';
  return out;
}

std::string NeutralEmptyResultProceduralBlockEnvelope() {
  return NeutralProceduralBlockEnvelope(
      "\"procedural_ir_contract\":\"sblr.procedural.block.v1\","
      "\"procedural_block_kind\":\"anonymous\","
      "\"procedural_input_count\":\"0\","
      "\"procedural_local_count\":\"0\","
      "\"procedural_output_count\":\"1\","
      "\"procedural_slot_count\":\"1\","
      "\"procedural_instruction_count\":\"0\","
      "\"procedural_yield_count\":\"0\","
      "\"procedural_slot_0_id\":\"result.0\","
      "\"procedural_slot_0_kind\":\"result\","
      "\"procedural_slot_0_type\":\"int32\","
      "\"procedural_slot_0_nullable\":\"false\"");
}

std::string NeutralTimestampAssignmentProceduralBlockEnvelope() {
  return NeutralProceduralBlockEnvelope(
      "\"procedural_ir_contract\":\"sblr.procedural.block.v1\","
      "\"procedural_block_kind\":\"anonymous\","
      "\"procedural_input_count\":\"0\","
      "\"procedural_local_count\":\"1\","
      "\"procedural_output_count\":\"0\","
      "\"procedural_slot_count\":\"1\","
      "\"procedural_instruction_count\":\"1\","
      "\"procedural_yield_count\":\"0\","
      "\"procedural_slot_0_id\":\"local.0\","
      "\"procedural_slot_0_kind\":\"local\","
      "\"procedural_slot_0_type\":\"character\","
      "\"procedural_slot_0_nullable\":\"true\","
      "\"procedural_slot_0_character_length\":\"100\","
      "\"procedural_instruction_0_kind\":\"assign\","
      "\"procedural_instruction_0_target_slot\":\"local.0\","
      "\"procedural_instruction_0_expression_kind\":\"substring\","
      "\"procedural_instruction_0_source_kind\":\"context_variable\","
      "\"procedural_instruction_0_source_id\":\"ctx_current_timestamp\","
      "\"procedural_instruction_0_source_cast_type\":\"character\","
      "\"procedural_instruction_0_start_kind\":\"literal_int64\","
      "\"procedural_instruction_0_start_value\":\"1\","
      "\"procedural_instruction_0_length_kind\":\"to_end\"");
}

std::string NeutralInsertEnvelope(
    const EngineTransactionFixture& fixture,
    std::string_view value = "neutral-v2-visible-row") {
  auto out = NeutralOperationEnvelope("dml.insert_rows",
                                      "SBLR_DML_INSERT_ROWS",
                                      "sblr.dml.operation.v3",
                                      true);
  out += "target_object_uuid=" + fixture.table_uuid + "\n";
  out += "target_object_kind=table\n";
  out += "estimated_row_count=1\n";
  // The empty row identity before '|id' is deliberate: the engine, not this
  // conformance client, must mint the durable row UUID.
  out += "operand=row_field:text\t|id\t";
  out += value;
  out += "\n";
  return out;
}

std::string NeutralDeleteEnvelope(
    const EngineTransactionFixture& fixture,
    std::string_view result_payload_policy = {}) {
  auto out = NeutralOperationEnvelope("dml.delete_rows",
                                      "SBLR_DML_DELETE_ROWS",
                                      "sblr.dml.operation.v3",
                                      true);
  out += "target_object_uuid=" + fixture.table_uuid + "\n";
  out += "target_object_kind=table\n";
  if (!result_payload_policy.empty()) {
    out += "result_payload_policy=";
    out += result_payload_policy;
    out += "\n";
  }
  return out;
}

std::string NeutralSelectEnvelope(const EngineTransactionFixture& fixture) {
  auto out = NeutralOperationEnvelope("dml.select_rows",
                                      "SBLR_DML_SELECT_ROWS",
                                      "sblr.query.relational.v3",
                                      true);
  out += "target_object_uuid=" + fixture.table_uuid + "\n";
  out += "target_object_kind=table\n";
  out += "projection_count=1\nprojection_0=id\n";
  out += "predicate_kind=column_equals\n";
  out += "predicate_column=id\n";
  out += "predicate_value=neutral-v2-visible-row\n";
  out += "predicate_value_type=text\nlimit=1\n";
  return out;
}

std::string NeutralRoutineCreateOrAlterEnvelope(
    const EngineTransactionFixture& fixture) {
  std::string out =
      "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
      "\"envelope_major\":3,"
      "\"sblr_version\":\"sblr_v3\","
      "\"operation_id\":\"ddl.create_procedure\","
      "\"opcode\":\"SBLR_DDL_CREATE_PROCEDURE\","
      "\"operation_family\":\"sblr.catalog.mutation.v3\","
      "\"sblr_operation_family\":\"sblr.catalog.mutation.v3\","
      "\"result_shape\":\"engine.api.result.v1\","
      "\"diagnostic_shape\":\"engine.diagnostic.v1\","
      "\"parser_resolved_names_to_uuids\":true,"
      "\"requires_security_context\":true,"
      "\"requires_transaction_context\":true,"
      "\"requires_cluster_authority\":false,"
      "\"contains_sql_text\":false,"
      "\"identifier_profile_uuid\":\"sbsql_v3\","
      "\"source_dialect\":\"neutral_conformance\","
      "\"target_object_kind\":\"procedure\","
      "\"procedure_name\":\"neutral_delete_between\","
      "\"target_schema_uuid\":\"";
  out += fixture.schema_uuid;
  out +=
      "\",\"executor\":\"sblr\","
      "\"sblr_hash\":\"sha256:3f4bbd573a74f8a6a99d1073cc8f6f954f030e20f44dfbcebd2f4f3df953f861\","
      "\"sblr_provenance\":\"engine_compiled_uuid_bound_routine_v1\","
      "\"side_effect_class\":\"data_mutation\","
      "\"executable_descriptor_kind\":\"create_or_alter_procedure\","
      "\"compiled_body_descriptor\":\"";
  out += api::kRoutineDeleteColumnRangeCountDescriptorV1;
  out += "|" + fixture.routine_table_uuid + "|" +
         fixture.routine_column_uuid + "|0|1|2|2\",";
  out +=
      "\"routine_parameter_count\":\"2\","
      "\"routine_parameter_0_mode\":\"in\","
      "\"routine_parameter_0_type\":\"integer\","
      "\"routine_parameter_1_mode\":\"in\","
      "\"routine_parameter_1_type\":\"integer\","
      "\"routine_return_count\":\"1\","
      "\"routine_return_0_type\":\"integer\","
      "\"related_object_0_uuid\":\"";
  out += fixture.routine_table_uuid;
  out +=
      "\",\"related_object_0_kind\":\"table\","
      "\"permission\":\"manage_executable\"}";
  return out;
}

std::string NeutralRoutineInvokeEnvelope(std::string_view procedure_uuid) {
  std::string out =
      "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
      "\"envelope_major\":3,"
      "\"sblr_version\":\"sblr_v3\","
      "\"operation_id\":\"routine.procedure_invoke\","
      "\"opcode\":\"SBLR_PROCEDURE_INVOKE\","
      "\"operation_family\":\"sblr.routine.execute.v3\","
      "\"sblr_operation_family\":\"sblr.routine.execute.v3\","
      "\"result_shape\":\"engine.api.result.v1\","
      "\"diagnostic_shape\":\"engine.diagnostic.v1\","
      "\"parser_resolved_names_to_uuids\":true,"
      "\"requires_security_context\":true,"
      "\"requires_transaction_context\":true,"
      "\"requires_cluster_authority\":false,"
      "\"contains_sql_text\":false,"
      "\"identifier_profile_uuid\":\"sbsql_v3\","
      "\"source_dialect\":\"neutral_conformance\","
      "\"target_object_uuid\":\"";
  out += procedure_uuid;
  out +=
      "\",\"target_object_kind\":\"procedure\","
      "\"routine_argument_count\":\"2\","
      "\"routine_argument_0_type\":\"integer\","
      "\"routine_argument_0_value\":\"4\","
      "\"routine_argument_1_type\":\"integer\","
      "\"routine_argument_1_value\":\"7\","
      "\"permission\":\"invoke_executable\"}";
  return out;
}

struct NeutralLiveV2Route {
  server::ServerSessionRegistry registry;
  server::HostedEngineState engine_state;
  std::array<std::uint8_t, 16> session_uuid{};
  server::ServerTransactionState default_transaction;
};

NeutralLiveV2Route MakeNeutralLiveV2Route(
    const EngineTransactionFixture& fixture,
    const api::EngineRequestContext& context) {
  NeutralLiveV2Route route;
  route.session_uuid = sbps::MakeUuidV7Bytes();
  const auto channel_uuid = sbps::MakeUuidV7Bytes();
  route.default_transaction =
      Transaction(context.local_transaction_id,
                  context.transaction_uuid.canonical);
  route.default_transaction.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;

  auto session = Session(route.session_uuid,
                         channel_uuid,
                         route.default_transaction);
  session.relation_descriptor_projection_v3_negotiated = true;
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = fixture.database_path.string();
  session.database_uuid = fixture.database_uuid;
  session.resource_epoch = fixture.resource_epoch;
  session.channel_state = server::ServerChannelState::kReady;
  session.session_binding_present = true;

  route.registry.channel_state = server::ServerChannelState::kReady;
  route.registry.physical_channel_by_connection_uuid[
      server::UuidBytesToText(route.session_uuid)] = channel_uuid;
  route.registry.sessions_by_uuid[
      server::UuidBytesToText(route.session_uuid)] = std::move(session);

  route.engine_state.engine_context_active = true;
  server::HostedDatabaseSnapshot database;
  database.state = server::HostedDatabaseState::kOpen;
  database.database_created = true;
  database.database_open = true;
  database.write_admission_fenced = false;
  database.database_path = fixture.database_path.string();
  database.database_uuid = fixture.database_uuid;
  route.engine_state.databases.push_back(std::move(database));
  return route;
}

sbps::Frame ResolveNameFrame(
    const std::array<std::uint8_t, 16>& session_uuid,
    std::uint32_t schema,
    std::vector<std::uint8_t> payload) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(
      sbps::MessageType::kResolveNameRequest);
  frame.header.payload_schema_id = schema;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session_uuid;
  frame.header.session_uuid = session_uuid;
  frame.payload = std::move(payload);
  return frame;
}

sbps::Frame DecodeServerFrame(const std::vector<std::uint8_t>& bytes,
                              std::string_view message) {
  const auto decoded = sbps::DecodeFrameBytes(bytes, 1024u * 1024u);
  Require(decoded.ok(), message);
  return *decoded.frame;
}

bool FrameHasDiagnosticCode(const sbps::Frame& frame,
                            std::string_view expected) {
  const auto codes =
      sbps::DecodeMessageVectorDiagnosticCodes(frame.payload);
  for (const auto& code : codes) {
    if (code == expected) return true;
  }
  return false;
}

bool ResultHasDiagnosticCode(
    const ipc::PublicNameResolutionResult& result,
    std::string_view expected) {
  for (const auto& diagnostic : result.messages.diagnostics) {
    if (diagnostic.code == expected) return true;
  }
  return false;
}

std::string ReadBinaryFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

const ipc::PublicRelationColumnDescriptor* FindProjectedColumn(
    const ipc::PublicRelationDescriptor& descriptor,
    std::string_view canonical_name_key) {
  for (const auto& column : descriptor.columns) {
    if (column.canonical_name_key == canonical_name_key) return &column;
  }
  return nullptr;
}

void VerifyNeutralPersistedRelationProjection() {
  auto fixture = CreateEngineTransactionFixture();
  CreateNeutralVisibilityTable(&fixture);
  const auto context = BeginEngineTransaction(fixture, 30);
  auto route = MakeNeutralLiveV2Route(fixture, context);

  ipc::ParserSessionContext client_session;
  client_session.authenticated = true;
  client_session.transaction_routing_v2_negotiated = true;
  client_session.relation_descriptor_projection_v3_negotiated = true;
  client_session.session_uuid =
      server::UuidBytesToText(route.session_uuid);
  client_session.connection_uuid = client_session.session_uuid;
  client_session.database_uuid = fixture.database_uuid;
  client_session.default_language = "en";
  client_session.dialect_profile_uuid = "sbsql_v3";
  client_session.catalog_epoch = 1;
  client_session.security_policy_epoch = 1;
  ipc::ParserClientConfig client_config;
  client_config.dialect_profile_uuid = "sbsql_v3";
  ipc::ParserTransactionSelector selector;
  selector.local_transaction_id = context.local_transaction_id;
  selector.transaction_uuid = context.transaction_uuid.canonical;

  const auto v2_payload =
      ipc::EncodeResolveNameRequestPayloadV2ForTest(
          client_session,
          "neutral_resource_projection",
          false,
          "relation",
          client_config,
          selector);
  std::vector<std::uint8_t> expected_v2;
  PutString(&expected_v2, "neutral_resource_projection");
  PutU8(&expected_v2, 0);
  PutString(&expected_v2, "sbsql_v3");
  PutString(&expected_v2, "en");
  PutString(&expected_v2, "");
  PutString(&expected_v2, "relation");
  PutU8(&expected_v2, 1);
  PutUuid(&expected_v2, route.session_uuid);
  PutU64(&expected_v2, selector.local_transaction_id);
  PutString(&expected_v2, selector.transaction_uuid);
  Require(v2_payload == expected_v2,
          "ResolveName V3 work changed the legacy V2 request bytes");

  const auto v3_payload =
      ipc::EncodeResolveNameRequestPayloadV3ForTest(
          client_session,
          "neutral_resource_projection",
          false,
          "relation",
          client_config,
          selector,
          0x01u);
  auto expected_v3 = expected_v2;
  PutU8(&expected_v3, 0x01u);
  Require(v3_payload == expected_v3,
          "ResolveName V3 did not preserve the exact V2 prefix");

  // This V1 request has the same presented-name/profile/class cache key as
  // the V3 request, but it has neither an exact selector nor a persisted
  // relation-descriptor projection.  It deliberately prewarms an ordinary
  // name-only entry that must never satisfy the later V3 request.
  std::vector<std::uint8_t> v1_cache_prewarm_payload;
  PutString(&v1_cache_prewarm_payload, "neutral_resource_projection");
  PutU8(&v1_cache_prewarm_payload, 0);
  PutString(&v1_cache_prewarm_payload, "sbsql_v3");
  PutString(&v1_cache_prewarm_payload, "en");
  PutString(&v1_cache_prewarm_payload, "");
  PutString(&v1_cache_prewarm_payload, "relation");
  PutU8(&v1_cache_prewarm_payload, 0);
  Require(!ipc::V2RequestMayRetryAfterWriteForTest(
              sbps::kSchemaResolveNameRequestV3),
          "ResolveName V3 was incorrectly marked replayable after write");

  auto client_without_v3 = client_session;
  client_without_v3.relation_descriptor_projection_v3_negotiated = false;
  ipc::SbpsClient no_server(
      "unix:/tmp/sb_neutral_relation_v3_no_server.sock");
  const auto client_refused =
      no_server.ResolveRelationDescriptorPublicOnTransaction(
          client_without_v3,
          "neutral_resource_projection",
          false,
          "relation",
          client_config,
          selector);
  Require(!client_refused.resolved &&
              ResultHasDiagnosticCode(
                  client_refused,
                  "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_V3_NOT_NEGOTIATED"),
          "V3 client emitted schema 7007 without negotiated capability");

  auto& server_session = route.registry.sessions_by_uuid.at(
      server::UuidBytesToText(route.session_uuid));
  server_session.relation_descriptor_projection_v3_negotiated = false;
  const auto server_refused_capability = DecodeServerFrame(
      server::ResolveNamePublicFrameForEmbedded(
          ResolveNameFrame(route.session_uuid,
                           sbps::kSchemaResolveNameRequestV3,
                           v3_payload),
          route.engine_state,
          &route.registry),
      "unnegotiated V3 response did not decode");
  Require((server_refused_capability.header.flags & sbps::kFlagError) != 0 &&
              FrameHasDiagnosticCode(
                  server_refused_capability,
                  "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_V3_NOT_NEGOTIATED"),
          "V3 server accepted schema 7007 without negotiated capability");
  server_session.relation_descriptor_projection_v3_negotiated = true;

  const auto legacy = DecodeServerFrame(
      server::ResolveNamePublicFrameForEmbedded(
          ResolveNameFrame(route.session_uuid,
                           sbps::kSchemaResolveNameRequestV2,
                           v2_payload),
          route.engine_state,
          &route.registry),
      "legacy V2 name response did not decode");
  Require((legacy.header.flags & sbps::kFlagError) == 0 &&
              legacy.header.payload_schema_id ==
                  sbps::kSchemaResolveNameResultV2,
          "ResolveName V3 work changed the legacy V2 response schema");

  const auto normal_cache_before_prewarm =
      route.registry.public_name_resolution_cache_by_key.size();
  const auto stable_cache_before_prewarm =
      route.registry.stable_public_name_resolution_cache_by_key.size();
  const auto prewarmed = DecodeServerFrame(
      server::ResolveNamePublicFrameForEmbedded(
          ResolveNameFrame(route.session_uuid,
                           sbps::kSchemaResolveNameRequestV1,
                           v1_cache_prewarm_payload),
          route.engine_state,
          &route.registry),
      "ordinary V1 cache-prewarm response did not decode");
  Require((prewarmed.header.flags & sbps::kFlagError) == 0 &&
              prewarmed.header.payload_schema_id ==
                  sbps::kSchemaResolveNameResultV1,
          "ordinary name-resolution cache prewarm was rejected");
  bool ordinary_name_only_entry_present = false;
  for (const auto& [_, cached] :
       route.registry.public_name_resolution_cache_by_key) {
    if (cached.object_uuid == fixture.resource_table_uuid &&
        (cached.object_class == "table" ||
         cached.object_class == "relation")) {
      ordinary_name_only_entry_present = true;
      break;
    }
  }
  Require(route.registry.public_name_resolution_cache_by_key.size() >
                  normal_cache_before_prewarm &&
              route.registry.stable_public_name_resolution_cache_by_key
                      .size() >= stable_cache_before_prewarm &&
              ordinary_name_only_entry_present,
          "ordinary name resolution did not prewarm the name-only cache fixture");

  const auto cache_hit_count = [&route]() {
    std::uint64_t count = 0;
    for (const auto& [_, cached] :
         route.registry.public_name_resolution_cache_by_key) {
      count += cached.hit_count;
    }
    for (const auto& [_, cached] :
         route.registry.stable_public_name_resolution_cache_by_key) {
      count += cached.hit_count;
    }
    return count;
  };
  const auto cache_hits_before_v3_projection = cache_hit_count();

  const auto descriptor_path = std::filesystem::path(
      fixture.database_path.string() +
      ".sb.mga_relation_descriptors");
  const std::string descriptor_bytes_before =
      ReadBinaryFile(descriptor_path);
  Require(!descriptor_bytes_before.empty(),
          "neutral resource relation lacks a persisted descriptor");
  const auto normal_cache_before =
      route.registry.public_name_resolution_cache_by_key.size();
  const auto stable_cache_before =
      route.registry.stable_public_name_resolution_cache_by_key.size();

  const auto projected_frame = DecodeServerFrame(
      server::ResolveNamePublicFrameForEmbedded(
          ResolveNameFrame(route.session_uuid,
                           sbps::kSchemaResolveNameRequestV3,
                           v3_payload),
          route.engine_state,
          &route.registry),
      "V3 persisted relation response did not decode");
  Require((projected_frame.header.flags & sbps::kFlagError) == 0 &&
              projected_frame.header.payload_schema_id ==
                  sbps::kSchemaResolveNameResultV3,
          "V3 persisted relation response used the wrong frame contract");
  ipc::PublicNameResolutionResult projected;
  const bool projected_decoded =
      ipc::DecodeResolveNameResultPayloadV3ForTest(
          projected_frame.payload, true, &projected);
  if (!projected_decoded ||
      projected.object_uuid != fixture.resource_table_uuid ||
      !projected.relation_descriptor.present) {
    std::cerr << "projected_decoded=" << projected_decoded
              << " resolved=" << projected.resolved
              << " object_uuid=" << projected.object_uuid
              << " expected_uuid=" << fixture.resource_table_uuid
              << " descriptor_present="
              << projected.relation_descriptor.present
              << " relation_uuid="
              << projected.relation_descriptor.relation_uuid
              << " generation="
              << projected.relation_descriptor.descriptor_generation
              << " validated_epoch="
              << projected.relation_descriptor.validated_resource_epoch
              << " expected_epoch=" << fixture.resource_epoch
              << " columns="
              << projected.relation_descriptor.columns.size() << '\n';
    for (const auto& diagnostic : projected.messages.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    }
  }
  Require(projected_decoded &&
              projected.object_uuid == fixture.resource_table_uuid &&
              projected.relation_descriptor.present &&
              projected.relation_descriptor.relation_uuid ==
                  fixture.resource_table_uuid &&
              projected.relation_descriptor.descriptor_generation != 0 &&
              projected.relation_descriptor.validated_resource_epoch ==
                  fixture.resource_epoch &&
              projected.relation_descriptor.columns.size() == 2,
          "V3 persisted relation projection lost canonical relation authority");
  const auto* f1 = FindProjectedColumn(
      projected.relation_descriptor, "f1");
  const auto* f2 = FindProjectedColumn(
      projected.relation_descriptor, "f2");
  Require(f1 != nullptr && f2 != nullptr &&
              f1->charset_uuid == fixture.gbk_charset_uuid &&
              f1->collation_uuid ==
                  fixture.gbk_default_collation_uuid &&
              f1->charset_canonical_name == "GBK" &&
              f1->collation_canonical_name == "GBK" &&
              f1->canonical_type_name == "VARCHAR(20)" &&
              !f1->nullable && !f1->generated && !f1->identity_column &&
              f1->character_length == 20 &&
              f1->charset_min_bytes == 1 &&
              f1->charset_max_bytes == 2 &&
              f2->charset_uuid == fixture.gbk_charset_uuid &&
              f2->collation_uuid ==
                  fixture.gbk_unicode_collation_uuid &&
              f2->charset_canonical_name == "GBK" &&
              f2->collation_canonical_name == "GBK_UNICODE" &&
              f2->character_length == 20 &&
              !f1->type_descriptor_uuid.empty() &&
              !f2->type_descriptor_uuid.empty(),
          "V3 text projection lost canonical charset/collation metadata");
  Require(ReadBinaryFile(descriptor_path) == descriptor_bytes_before,
          "V3 persisted relation projection changed durable descriptor bytes");
  Require(route.registry.public_name_resolution_cache_by_key.size() ==
                  normal_cache_before &&
              route.registry.stable_public_name_resolution_cache_by_key.size() ==
                  stable_cache_before &&
              cache_hit_count() == cache_hits_before_v3_projection,
          "V3 persisted relation projection consulted, satisfied, or populated an ordinary name cache");

  auto noncanonical_quoted_payload = v3_payload;
  const std::size_t quoted_offset =
      2 + std::string_view("neutral_resource_projection").size();
  noncanonical_quoted_payload[quoted_offset] = 2;
  const auto noncanonical_quoted = DecodeServerFrame(
      server::ResolveNamePublicFrameForEmbedded(
          ResolveNameFrame(route.session_uuid,
                           sbps::kSchemaResolveNameRequestV3,
                           std::move(noncanonical_quoted_payload)),
          route.engine_state,
          &route.registry),
      "noncanonical V3 quoted response did not decode");
  Require((noncanonical_quoted.header.flags & sbps::kFlagError) != 0 &&
              FrameHasDiagnosticCode(
                  noncanonical_quoted,
                  "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_REQUEST_INVALID"),
          "V3 accepted a noncanonical quoted boolean");

  auto noncanonical_bypass_payload = v3_payload;
  const std::size_t selector_bytes =
      16 + 8 + 2 + selector.transaction_uuid.size();
  const std::size_t bypass_offset =
      expected_v2.size() - selector_bytes - 1;
  noncanonical_bypass_payload[bypass_offset] = 2;
  const auto noncanonical_bypass = DecodeServerFrame(
      server::ResolveNamePublicFrameForEmbedded(
          ResolveNameFrame(route.session_uuid,
                           sbps::kSchemaResolveNameRequestV3,
                           std::move(noncanonical_bypass_payload)),
          route.engine_state,
          &route.registry),
      "noncanonical V3 bypass response did not decode");
  Require((noncanonical_bypass.header.flags & sbps::kFlagError) != 0 &&
              FrameHasDiagnosticCode(
                  noncanonical_bypass,
                  "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_REQUEST_INVALID"),
          "V3 accepted a noncanonical cache-bypass boolean");

  const auto oversized_name_payload =
      ipc::EncodeResolveNameRequestPayloadV3ForTest(
          client_session,
          std::string(4097, 'n'),
          false,
          "relation",
          client_config,
          selector,
          0x01u);
  const auto oversized_name = DecodeServerFrame(
      server::ResolveNamePublicFrameForEmbedded(
          ResolveNameFrame(route.session_uuid,
                           sbps::kSchemaResolveNameRequestV3,
                           oversized_name_payload),
          route.engine_state,
          &route.registry),
      "oversized V3 name response did not decode");
  Require((oversized_name.header.flags & sbps::kFlagError) != 0 &&
              FrameHasDiagnosticCode(
                  oversized_name,
                  "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_REQUEST_INVALID"),
          "V3 accepted an oversized request metadata field");

  auto unknown_flags_payload = v3_payload;
  unknown_flags_payload.back() = 0x80u;
  const auto unknown_flags = DecodeServerFrame(
      server::ResolveNamePublicFrameForEmbedded(
          ResolveNameFrame(route.session_uuid,
                           sbps::kSchemaResolveNameRequestV3,
                           std::move(unknown_flags_payload)),
          route.engine_state,
          &route.registry),
      "malformed V3 request response did not decode");
  Require((unknown_flags.header.flags & sbps::kFlagError) != 0 &&
              FrameHasDiagnosticCode(
                  unknown_flags,
                  "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_REQUEST_INVALID"),
          "V3 accepted unknown projection flags");

  auto wrong_selector = selector;
  wrong_selector.transaction_uuid = NewUuidText(
      platform::UuidKind::transaction, fixture.salt + 9000);
  const auto wrong_selector_payload =
      ipc::EncodeResolveNameRequestPayloadV3ForTest(
          client_session,
          "neutral_resource_projection",
          false,
          "relation",
          client_config,
          wrong_selector,
          0x01u);
  const auto refused_selector = DecodeServerFrame(
      server::ResolveNamePublicFrameForEmbedded(
          ResolveNameFrame(route.session_uuid,
                           sbps::kSchemaResolveNameRequestV3,
                           wrong_selector_payload),
          route.engine_state,
          &route.registry),
      "wrong-selector V3 response did not decode");
  Require((refused_selector.header.flags & sbps::kFlagError) != 0 &&
              FrameHasDiagnosticCode(
                  refused_selector,
                  "PARSER_SERVER_IPC.TRANSACTION_SELECTOR_INVALID"),
          "V3 persisted relation projection accepted a wrong selector");

  auto malformed_result = projected_frame.payload;
  malformed_result.push_back(0);
  ipc::PublicNameResolutionResult malformed_decoded;
  Require(!ipc::DecodeResolveNameResultPayloadV3ForTest(
              malformed_result, true, &malformed_decoded) &&
              !malformed_decoded.messages.diagnostics.empty() &&
              malformed_decoded.messages.diagnostics.front().code ==
                  "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID" &&
              !malformed_decoded.relation_descriptor.present &&
              malformed_decoded.relation_descriptor.columns.empty(),
          "V3 client codec accepted trailing relation descriptor bytes");

  const auto not_found_payload =
      ipc::EncodeResolveNameRequestPayloadV3ForTest(
          client_session,
          "neutral_missing_projection",
          false,
          "relation",
          client_config,
          selector,
          0x01u);
  const auto not_found_frame = DecodeServerFrame(
      server::ResolveNamePublicFrameForEmbedded(
          ResolveNameFrame(route.session_uuid,
                           sbps::kSchemaResolveNameRequestV3,
                           not_found_payload),
          route.engine_state,
          &route.registry),
      "V3 not-found response did not decode");
  Require((not_found_frame.header.flags & sbps::kFlagError) == 0 &&
              not_found_frame.header.payload_schema_id ==
                  sbps::kSchemaResolveNameResultV3,
          "V3 not-found response used the wrong frame contract");
  ipc::PublicNameResolutionResult not_found_result;
  Require(!ipc::DecodeResolveNameResultPayloadV3ForTest(
              not_found_frame.payload, false, &not_found_result) &&
              ResultHasDiagnosticCode(
                  not_found_result,
                  "PARSER_SERVER_IPC.NAME_NOT_FOUND_OR_NOT_VISIBLE"),
          "V3 client rejected the exact zero-extension failure envelope");

  auto missing_failure_envelope = not_found_frame.payload;
  missing_failure_envelope.pop_back();
  ipc::PublicNameResolutionResult missing_failure_result;
  Require(!ipc::DecodeResolveNameResultPayloadV3ForTest(
              missing_failure_envelope,
              false,
              &missing_failure_result) &&
              ResultHasDiagnosticCode(
                  missing_failure_result,
                  "PARSER_SERVER_IPC.NAME_RESULT_INVALID"),
          "V3 client accepted a failed result without its extension envelope");

  auto nonzero_failure_envelope = not_found_frame.payload;
  nonzero_failure_envelope.back() = 1;
  ipc::PublicNameResolutionResult nonzero_failure_result;
  Require(!ipc::DecodeResolveNameResultPayloadV3ForTest(
              nonzero_failure_envelope,
              false,
              &nonzero_failure_result) &&
              ResultHasDiagnosticCode(
                  nonzero_failure_result,
                  "PARSER_SERVER_IPC.NAME_RESULT_INVALID"),
          "V3 client accepted a nonzero failed-result extension envelope");

  std::vector<std::uint8_t> overflowing_string_result;
  PutU16(&overflowing_string_result, 0xffffu);
  PutU64(&overflowing_string_result,
         std::numeric_limits<std::uint64_t>::max());
  ipc::PublicNameResolutionResult overflowing_string_decoded;
  Require(!ipc::DecodeResolveNameResultPayloadV3ForTest(
              overflowing_string_result,
              true,
              &overflowing_string_decoded) &&
              ResultHasDiagnosticCode(
                  overflowing_string_decoded,
                  "PARSER_SERVER_IPC.NAME_RESULT_INVALID") &&
              !overflowing_string_decoded.relation_descriptor.present,
          "V3 client accepted an overflowing long-string length");

  api::EngineCommitTransactionRequest commit;
  commit.context = context;
  RequireEngineOk(api::EngineCommitTransaction(commit),
                  "neutral V3 read transaction commit failed");
  const auto retired = DecodeServerFrame(
      server::ResolveNamePublicFrameForEmbedded(
          ResolveNameFrame(route.session_uuid,
                           sbps::kSchemaResolveNameRequestV3,
                           v3_payload),
          route.engine_state,
          &route.registry),
      "retired-selector V3 response did not decode");
  ipc::PublicNameResolutionResult retired_result;
  const bool retired_decoded =
      (retired.header.flags & sbps::kFlagError) == 0 &&
      ipc::DecodeResolveNameResultPayloadV3ForTest(
          retired.payload, true, &retired_result);
  Require(!retired_decoded,
          "V3 persisted relation projection accepted a retired MGA selector");
  Require(ReadBinaryFile(descriptor_path) == descriptor_bytes_before,
          "retired V3 selector changed durable descriptor bytes");

  const auto missing_context = BeginEngineTransaction(fixture, 31);
  auto missing_route = MakeNeutralLiveV2Route(fixture, missing_context);
  ipc::ParserTransactionSelector missing_selector;
  missing_selector.local_transaction_id =
      missing_context.local_transaction_id;
  missing_selector.transaction_uuid =
      missing_context.transaction_uuid.canonical;
  auto missing_client_session = client_session;
  missing_client_session.session_uuid =
      server::UuidBytesToText(missing_route.session_uuid);
  missing_client_session.connection_uuid =
      missing_client_session.session_uuid;
  const auto missing_payload =
      ipc::EncodeResolveNameRequestPayloadV3ForTest(
          missing_client_session,
          "neutral_resource_projection",
          false,
          "relation",
          client_config,
          missing_selector,
          0x01u);
  std::error_code ignored;
  std::filesystem::remove(descriptor_path, ignored);
  Require(!std::filesystem::exists(descriptor_path),
          "neutral V3 missing-descriptor fixture removal failed");
  const auto missing = DecodeServerFrame(
      server::ResolveNamePublicFrameForEmbedded(
          ResolveNameFrame(missing_route.session_uuid,
                           sbps::kSchemaResolveNameRequestV3,
                           missing_payload),
          missing_route.engine_state,
          &missing_route.registry),
      "missing-descriptor V3 response did not decode");
  Require((missing.header.flags & sbps::kFlagError) != 0 &&
              FrameHasDiagnosticCode(missing,
                                     "SB_ENGINE_API_INVALID_REQUEST") &&
              !std::filesystem::exists(descriptor_path),
          "V3 synthesized or wrote a missing persisted relation descriptor");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = missing_context;
  RequireEngineOk(api::EngineRollbackTransaction(rollback),
                  "neutral V3 missing-descriptor rollback failed");
}

bool SameExactSelector(const server::ServerTransactionState& left,
                       const server::ServerTransactionState& right) {
  return left.local_transaction_id == right.local_transaction_id &&
         left.transaction_uuid == right.transaction_uuid &&
         left.snapshot_visible_through_local_transaction_id ==
             right.snapshot_visible_through_local_transaction_id;
}

bool SessionContainsExactActiveSelector(
    const server::ServerSessionRecord& session,
    const server::ServerTransactionState& selector) {
  const auto found =
      session.transactions_by_local_id.find(selector.local_transaction_id);
  return found != session.transactions_by_local_id.end() &&
         found->second.lifecycle_state ==
             server::ServerTransactionLifecycleState::kActive &&
         SameExactSelector(found->second, selector);
}

bool InventoryContainsExactActiveSelector(
    const EngineTransactionFixture& fixture,
    const server::ServerTransactionState& selector) {
  const auto loaded = db::LoadLocalTransactionInventoryFromDatabase(
      fixture.database_path.string());
  if (!loaded.ok()) return false;
  for (const auto& entry : loaded.inventory.entries) {
    if (entry.identity.local_id.value != selector.local_transaction_id) {
      continue;
    }
    const bool active = entry.state == mga::TransactionState::active ||
                        entry.state == mga::TransactionState::read_only_active;
    return active &&
           uuid::UuidToString(entry.identity.transaction_uuid.value) ==
               selector.transaction_uuid;
  }
  return false;
}

void RequireSelectedExact(
    const server::SessionOperationResult& result,
    const server::ServerTransactionState& selector,
    std::string_view message) {
  Require(result.transaction_state.has_value() &&
              result.transaction_state->selected_present &&
              SameExactSelector(result.transaction_state->selected, selector),
          message);
}

bool SessionResultHasDiagnosticCode(
    const server::SessionOperationResult& result,
    std::string_view expected) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == expected) return true;
  }
  return false;
}

ipc::ServerExecutionResult DecodeAcceptedV2Result(
    const server::SessionOperationResult& result,
    std::string_view message) {
  ipc::ServerExecutionResult decoded;
  ipc::MessageVectorSet messages;
  Require(result.accepted && result.response_schema_id == 4012 &&
              ipc::DecodeExecuteResultPayloadV2ForTest(
                  result.payload, &decoded, &messages) &&
              decoded.accepted,
          message);
  return decoded;
}

std::uint64_t DecodeAcceptedV2RowCount(
    const server::SessionOperationResult& result,
    std::string_view message) {
  return DecodeAcceptedV2Result(result, message).row_count;
}

std::string ResultRowField(std::string_view payload,
                           std::string_view field) {
  const std::string marker = std::string(field) + "=";
  const std::size_t row = payload.find("row[0]=");
  if (row == std::string_view::npos) return {};
  std::size_t begin = payload.find(marker, row + 7);
  if (begin == std::string_view::npos) return {};
  begin += marker.size();
  const std::size_t end = payload.find_first_of(";\r\n", begin);
  return std::string(payload.substr(
      begin, end == std::string_view::npos ? payload.size() - begin
                                           : end - begin));
}

void VerifyNeutralProceduralBlockBridge() {
  const auto fixture = CreateEngineTransactionFixture();
  const auto context = BeginEngineTransaction(fixture, 40);
  auto route = MakeNeutralLiveV2Route(fixture, context);
  const auto selector = route.default_transaction;

  const std::string empty_result =
      NeutralEmptyResultProceduralBlockEnvelope();
  Require(empty_result.find("source_sql") == std::string::npos &&
              empty_result.find("raw_sql") == std::string::npos &&
              empty_result.find("parser_ast") == std::string::npos &&
              empty_result.find("parser_plan") == std::string::npos,
          "neutral procedural block fixture carried parser source payload");
  const auto prepared = server::HandlePrepareSblr(
      &route.registry,
      route.engine_state,
      RoutedPrepareFrameV2(route.session_uuid, empty_result, selector));
  const auto prepared_uuid =
      server::DecodePreparedStatementUuidForTest(prepared.payload);
  Require(prepared.accepted && prepared_uuid.has_value(),
          "neutral procedural block was not admitted on the prepared V2 route");

  const auto empty_execution = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecutePreparedFrameV2(
          route.session_uuid, *prepared_uuid, selector));
  const auto empty_result_decoded = DecodeAcceptedV2Result(
      empty_execution,
      "prepared neutral procedural block did not reach the engine runtime");
  RequireSelectedExact(
      empty_execution,
      selector,
      "prepared neutral procedural block lost its exact MGA selector");
  Require(empty_result_decoded.row_count == 0,
          "prepared neutral zero-yield block returned rows");
  Require(empty_result_decoded.cursor_uuid.empty(),
          "prepared neutral zero-yield block returned cursor " +
              empty_result_decoded.cursor_uuid);
  Require(empty_result_decoded.row_packet.find(
              "result_kind=sblr.procedural.block.rows.v1") !=
              std::string::npos,
          "prepared neutral zero-yield block fell back to behavior rows");
  Require(empty_result_decoded.row_packet.find(
              "evidence=procedural_yield_count_executed:0") !=
              std::string::npos,
          "prepared neutral zero-yield block omitted runtime yield evidence");
  Require(empty_result_decoded.row_packet.find("parser_executes_sql=true") ==
              std::string::npos,
          "prepared neutral zero-yield block exposed parser SQL execution");
  Require(InventoryContainsExactActiveSelector(fixture, selector),
          "prepared neutral zero-yield block changed MGA finality");
  Require(server::HandleClosePreparedSblr(
              &route.registry,
              ClosePreparedFrame(route.session_uuid, *prepared_uuid))
              .accepted,
          "prepared neutral procedural block did not close cleanly");

  const auto assignment_execution = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecuteFrameV2(
          route.session_uuid,
          NeutralTimestampAssignmentProceduralBlockEnvelope(),
          1,
          &selector));
  const auto assignment_result = DecodeAcceptedV2Result(
      assignment_execution,
      "neutral context-timestamp assignment did not reach the engine runtime");
  RequireSelectedExact(
      assignment_execution,
      selector,
      "neutral assignment procedural block lost its exact MGA selector");
  Require(assignment_result.row_count == 0 &&
              assignment_result.row_packet.find(
                  "result_kind=sblr.procedural.block.rows.v1") !=
                  std::string::npos &&
              assignment_result.row_packet.find(
                  "evidence=procedural_instruction_count_executed:1") !=
                  std::string::npos,
          "neutral assignment procedural operands were lost in the server bridge");

  const auto malformed = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecuteFrameV2(
          route.session_uuid,
          NeutralProceduralBlockEnvelope(
              "\"procedural_ir_contract\":\"sblr.procedural.block.v1\""),
          1,
          &selector));
  Require(!malformed.accepted &&
              SessionResultHasDiagnosticCode(
                  malformed, "SB_SBLR_PROCEDURAL_IR_FIELD_REQUIRED") &&
              InventoryContainsExactActiveSelector(fixture, selector),
          "partial neutral procedural IR fell through to the legacy block route");

  const auto source_only = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecuteFrameV2(
          route.session_uuid,
          NeutralProceduralBlockEnvelope("\"source_sql\":\"present\""),
          1,
          &selector));
  Require(!source_only.accepted &&
              SessionResultHasDiagnosticCode(
                  source_only,
                  "SB_SBLR_PROCEDURAL_SOURCE_PAYLOAD_FORBIDDEN") &&
              InventoryContainsExactActiveSelector(fixture, selector),
          "source-only neutral procedural payload fell through to legacy execution");

  const auto legacy_execution = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecuteFrameV2(
          route.session_uuid,
          NeutralTransactionEnvelope("transaction.execute_block",
                                     "SBLR_TRANSACTION_EXECUTE_BLOCK"),
          1,
          &selector));
  const auto legacy_result = DecodeAcceptedV2Result(
      legacy_execution,
      "contract-absent transaction block lost its legacy route");
  Require(legacy_result.row_count == 1 &&
              legacy_result.row_packet.find("result_kind=api_behavior_rows") !=
                  std::string::npos &&
              InventoryContainsExactActiveSelector(fixture, selector),
          "procedural v1 forwarding changed contract-absent legacy behavior");

  const auto rollback = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecuteFrameV2(
          route.session_uuid,
          NeutralTransactionEnvelope("transaction.rollback",
                                     "SBLR_TRANSACTION_ROLLBACK"),
          1,
          &selector));
  Require(rollback.accepted &&
              !InventoryContainsExactActiveSelector(fixture, selector),
          "neutral procedural block fixture rollback lost engine MGA finality");
}

void VerifyServerRoutedCreateOrAlterRoutineEnvelope() {
  auto fixture = CreateEngineTransactionFixture();
  CreateNeutralVisibilityTable(&fixture);
  CreateNeutralRoutineTable(&fixture);
  const auto context = BeginEngineTransaction(fixture, 41);
  auto route = MakeNeutralLiveV2Route(fixture, context);
  const auto selector = route.default_transaction;

  const std::string create_envelope =
      NeutralRoutineCreateOrAlterEnvelope(fixture);
  Require(create_envelope.find("\"target_object_uuid\":") ==
                  std::string::npos &&
              create_envelope.find("\"procedure_object_uuid\":") ==
                  std::string::npos &&
              create_envelope.find("\"sql_text\":") ==
                  std::string::npos &&
              create_envelope.find("\"contains_sql_text\":false") !=
                  std::string::npos,
          "neutral routine CREATE envelope supplied identity or SQL text");
  const auto created = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecuteFrameV2(route.session_uuid,
                           create_envelope,
                           1,
                           &selector));
  const auto created_result = DecodeAcceptedV2Result(
      created,
      "server-routed CREATE OR ALTER did not preserve absent identity and SBLR provenance");
  RequireSelectedExact(
      created,
      selector,
      "server-routed CREATE OR ALTER lost its exact MGA selector");
  const std::string created_procedure_uuid =
      ResultRowField(created_result.row_packet, "object_uuid");
  if (created_procedure_uuid.empty()) {
    std::cerr << "routine_create_row_count=" << created_result.row_count
              << " routine_create_payload=" << created_result.row_packet
              << '\n';
  }
  Require(created_result.row_count >= 1 &&
              !created_procedure_uuid.empty() &&
              created_result.row_packet.find(
                  "sblr_hash:sha256:3f4bbd573a74f8a6a99d1073cc8f6f954f030e20f44dfbcebd2f4f3df953f861") !=
                  std::string::npos &&
              created_result.row_packet.find(
                  "sblr_provenance:engine_compiled_uuid_bound_routine_v1") !=
                  std::string::npos &&
              created_result.row_packet.find(
                  "evidence=create_or_alter_binding:engine_allocated_uuid") !=
                  std::string::npos,
          "server-routed CREATE OR ALTER lost engine identity allocation or SBLR provenance");

  ipc::ParserSessionContext client_session;
  client_session.authenticated = true;
  client_session.transaction_routing_v2_negotiated = true;
  client_session.relation_descriptor_projection_v3_negotiated = true;
  client_session.session_uuid = server::UuidBytesToText(route.session_uuid);
  client_session.connection_uuid = client_session.session_uuid;
  client_session.database_uuid = fixture.database_uuid;
  client_session.default_language = "en";
  client_session.dialect_profile_uuid = "sbsql_v3";
  client_session.catalog_epoch = 1;
  client_session.security_policy_epoch = 1;
  ipc::ParserClientConfig client_config;
  client_config.dialect_profile_uuid = "sbsql_v3";
  ipc::ParserTransactionSelector parser_selector;
  parser_selector.local_transaction_id = selector.local_transaction_id;
  parser_selector.transaction_uuid = selector.transaction_uuid;
  const auto resolve_payload =
      ipc::EncodeResolveNameRequestPayloadV3ForTest(
          client_session,
          "neutral_delete_between",
          false,
          "procedure",
          client_config,
          parser_selector,
          0);
  const auto resolved_frame = DecodeServerFrame(
      server::ResolveNamePublicFrameForEmbedded(
          ResolveNameFrame(route.session_uuid,
                           sbps::kSchemaResolveNameRequestV3,
                           resolve_payload),
          route.engine_state,
          &route.registry),
      "server-routed procedure name response did not decode");
  ipc::PublicNameResolutionResult resolved;
  Require((resolved_frame.header.flags & sbps::kFlagError) == 0 &&
              resolved_frame.header.payload_schema_id ==
                  sbps::kSchemaResolveNameResultV3 &&
              ipc::DecodeResolveNameResultPayloadV3ForTest(
                  resolved_frame.payload, false, &resolved) &&
              resolved.resolved &&
              resolved.object_class == "procedure" &&
              resolved.object_uuid == created_procedure_uuid,
          "same-transaction public resolution lost the engine-issued procedure UUID");

  const auto invoked = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecuteFrameV2(
          route.session_uuid,
          NeutralRoutineInvokeEnvelope(resolved.object_uuid),
          1,
          &selector));
  const auto invoked_result = DecodeAcceptedV2Result(
      invoked, "server-routed UUID-bound procedure invocation was rejected");
  RequireSelectedExact(invoked,
                       selector,
                       "server-routed procedure invocation lost its exact MGA selector");
  if (invoked_result.row_count != 1 ||
      ResultRowField(invoked_result.row_packet,
                     "routine_output_slot_2") != "4") {
    std::cerr << "routine_invoke_row_count=" << invoked_result.row_count
              << " affected_present="
              << invoked_result.affected_rows_present
              << " affected_rows=" << invoked_result.affected_rows
              << " routine_invoke_payload=" << invoked_result.row_packet
              << '\n';
  }
  Require(invoked_result.row_count == 1 &&
              invoked_result.row_packet.find(
                  "result_kind=routine.procedure.result.v1") !=
                  std::string::npos &&
              ResultRowField(invoked_result.row_packet,
                             "routine_output_slot_2") == "4" &&
              invoked_result.row_packet.find(
                  "evidence=routine_affected_rows_output_slot:2:4") !=
                  std::string::npos,
          "server-routed procedure invocation lost authoritative ROW_COUNT=4");

  const auto rolled_back = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecuteFrameV2(
          route.session_uuid,
          NeutralTransactionEnvelope("transaction.rollback",
                                     "SBLR_TRANSACTION_ROLLBACK"),
          1,
          &selector));
  Require(rolled_back.accepted &&
              rolled_back.transaction_state.has_value() &&
              rolled_back.transaction_state->finality ==
                  server::ServerTransactionResponseState::Finality::
                      kKnownApplied &&
              rolled_back.transaction_state->finalized_present &&
              SameExactSelector(
                  rolled_back.transaction_state->finalized, selector) &&
              !InventoryContainsExactActiveSelector(fixture, selector),
          "server-routed procedure rollback lost exact MGA finality");
}

server::ServerTransactionState BeginAdditionalTransaction(
    NeutralLiveV2Route* route,
    std::string_view isolation_level) {
  Require(route != nullptr, "neutral V2 route is required");
  auto begin = NeutralTransactionEnvelope("transaction.begin",
                                          "SBLR_TRANSACTION_BEGIN");
  begin += "transaction_isolation_level=";
  begin += isolation_level;
  begin += "\ntransaction_read_only=false\n";
  const auto result = server::HandleExecuteSblr(
      &route->registry,
      route->engine_state,
      RoutedExecuteFrameV2(route->session_uuid, begin, 2));
  Require(result.accepted && result.response_schema_id == 4012 &&
              result.transaction_state.has_value() &&
              result.transaction_state->selected_present,
          "neutral V2 begin-additional did not publish a selector");
  return result.transaction_state->selected;
}

void VerifyTransferablePreparedRoutineMetadata() {
  auto fixture = CreateEngineTransactionFixture();
  CreateNeutralVisibilityTable(&fixture);
  CreateNeutralRoutineTable(&fixture);
  const auto data_context = BeginEngineTransaction(fixture, 61);
  auto route = MakeNeutralLiveV2Route(fixture, data_context);
  auto& session = route.registry.sessions_by_uuid.at(
      server::UuidBytesToText(route.session_uuid));
  const auto data_selector = route.default_transaction;

  const auto ddl_selector =
      BeginAdditionalTransaction(&route, "read_committed");
  const auto created = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecuteFrameV2(
          route.session_uuid,
          NeutralRoutineCreateOrAlterEnvelope(fixture),
          1,
          &ddl_selector));
  const auto created_result = DecodeAcceptedV2Result(
      created,
      "transferable metadata fixture could not create its routine");
  const std::string procedure_uuid =
      ResultRowField(created_result.row_packet, "object_uuid");
  Require(!procedure_uuid.empty(),
          "transferable metadata fixture did not receive an engine routine UUID");

  const auto ddl_commit = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecuteFrameV2(
          route.session_uuid,
          NeutralTransactionEnvelope("transaction.commit",
                                     "SBLR_TRANSACTION_COMMIT"),
          1,
          &ddl_selector));
  Require(ddl_commit.accepted && ddl_commit.transaction_state.has_value() &&
              ddl_commit.transaction_state->finality ==
                  server::ServerTransactionResponseState::Finality::
                      kKnownApplied &&
              !InventoryContainsExactActiveSelector(fixture, ddl_selector),
          "routine metadata DDL did not commit under its exact selector");

  // Without the negotiated capability, the existing exact-selector rule is
  // unchanged even for the same UUID-bound routine envelope.
  session.prepared_metadata_transfer_v1_negotiated = false;
  const auto strict_prepare_selector =
      BeginAdditionalTransaction(&route, "read_committed");
  const auto strict_prepare = server::HandlePrepareSblr(
      &route.registry,
      route.engine_state,
      RoutedPrepareFrameV2(
          route.session_uuid,
          NeutralRoutineInvokeEnvelope(procedure_uuid),
          strict_prepare_selector));
  const auto strict_uuid =
      server::DecodePreparedStatementUuidForTest(strict_prepare.payload);
  Require(strict_prepare.accepted && strict_uuid.has_value(),
          "strict prepared routine fixture was not admitted");
  const auto strict_cross_execute = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecutePreparedFrameV2(
          route.session_uuid, *strict_uuid, data_selector));
  Require(!strict_cross_execute.accepted &&
              !strict_cross_execute.diagnostics.empty() &&
              strict_cross_execute.diagnostics.front().code ==
                  "PARSER_SERVER_IPC.PREPARED_TRANSACTION_SELECTOR_MISMATCH",
          "unnegotiated prepared routine crossed transaction selectors");
  (void)server::HandleClosePreparedSblr(
      &route.registry,
      ClosePreparedFrame(route.session_uuid, *strict_uuid));
  const auto strict_rollback = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecuteFrameV2(
          route.session_uuid,
          NeutralTransactionEnvelope("transaction.rollback",
                                     "SBLR_TRANSACTION_ROLLBACK"),
          1,
          &strict_prepare_selector));
  Require(strict_rollback.accepted,
          "strict prepare transaction cleanup failed");

  session.prepared_metadata_transfer_v1_negotiated = true;
  const auto metadata_selector =
      BeginAdditionalTransaction(&route, "read_committed");
  const auto transferable_prepare = server::HandlePrepareSblr(
      &route.registry,
      route.engine_state,
      RoutedPrepareFrameV2(
          route.session_uuid,
          NeutralRoutineInvokeEnvelope(procedure_uuid),
          metadata_selector));
  const auto transferable_uuid =
      server::DecodePreparedStatementUuidForTest(
          transferable_prepare.payload);
  const std::string transferable_prepare_detail =
      transferable_prepare.diagnostics.empty()
          ? "no_server_diagnostic"
          : transferable_prepare.diagnostics.front().code + ":" +
                transferable_prepare.diagnostics.front().safe_message;
  Require(transferable_prepare.accepted && transferable_uuid.has_value(),
          "capability-gated engine metadata binding was not prepared: " +
              transferable_prepare_detail);
  const auto& prepared_record = route.registry.prepared_by_uuid.at(
      server::UuidBytesToText(*transferable_uuid));
  Require(prepared_record.prepared_metadata_transferable &&
              prepared_record.prepared_metadata_binding != nullptr,
          "server accepted transferable prepare without an engine binding");

  // Finalizing D__trans does not transfer or refresh M__trans.  The opaque
  // binding remains attachment-owned while the exact data selector stays
  // active with its original snapshot.
  const auto metadata_rollback = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecuteFrameV2(
          route.session_uuid,
          NeutralTransactionEnvelope("transaction.rollback",
                                     "SBLR_TRANSACTION_ROLLBACK"),
          1,
          &metadata_selector));
  Require(metadata_rollback.accepted &&
              !InventoryContainsExactActiveSelector(fixture,
                                                    metadata_selector) &&
              InventoryContainsExactActiveSelector(fixture, data_selector),
          "metadata transaction finality changed the data selector");

  const auto invoked = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecutePreparedFrameV2(
          route.session_uuid, *transferable_uuid, data_selector));
  const auto invoked_result = DecodeAcceptedV2Result(
      invoked,
      "transferable routine metadata did not execute on the old data transaction");
  RequireSelectedExact(
      invoked,
      data_selector,
      "transferable prepared execution did not echo the exact data selector");
  Require(invoked_result.row_count == 1 &&
              ResultRowField(invoked_result.row_packet,
                             "routine_output_slot_2") == "4" &&
              InventoryContainsExactActiveSelector(fixture, data_selector) &&
              session.transactions_by_local_id.at(
                  data_selector.local_transaction_id)
                      .snapshot_visible_through_local_transaction_id ==
                  data_selector.snapshot_visible_through_local_transaction_id,
          "prepared metadata execution advanced or replaced the data snapshot");

  const auto closed = server::HandleClosePreparedSblr(
      &route.registry,
      ClosePreparedFrame(route.session_uuid, *transferable_uuid));
  Require(closed.accepted,
          "transferable prepared metadata binding did not close cleanly");
  const auto data_rollback = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecuteFrameV2(
          route.session_uuid,
          NeutralTransactionEnvelope("transaction.rollback",
                                     "SBLR_TRANSACTION_ROLLBACK"),
          1,
          &data_selector));
  Require(data_rollback.accepted &&
              !InventoryContainsExactActiveSelector(fixture, data_selector),
          "transferable prepared invocation lost exact data finality");
}

sbps::Frame OrderlyDisconnectFrame(
    const std::array<std::uint8_t, 16>& session_uuid) {
  sbps::Frame frame;
  frame.header.message_type =
      static_cast<std::uint16_t>(sbps::MessageType::kDisconnectNotice);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session_uuid;
  frame.header.session_uuid = session_uuid;
  PutUuid(&frame.payload, session_uuid);
  PutString(&frame.payload, "parser_disconnect_notice");
  return frame;
}

void VerifyLiveV2MultiTransactionVisibility() {
  auto fixture = CreateEngineTransactionFixture();
  CreateNeutralVisibilityTable(&fixture);
  const auto initial_context = BeginEngineTransaction(fixture, 21);
  auto route = MakeNeutralLiveV2Route(fixture, initial_context);
  auto& session = route.registry.sessions_by_uuid.at(
      server::UuidBytesToText(route.session_uuid));
  const auto default_transaction = route.default_transaction;

  const auto t1 = BeginAdditionalTransaction(&route, "read_committed");
  const auto t2 = BeginAdditionalTransaction(&route, "snapshot");
  Require(t1.local_transaction_id != t2.local_transaction_id &&
              t1.transaction_uuid != t2.transaction_uuid &&
              SessionContainsExactActiveSelector(session, t1) &&
              SessionContainsExactActiveSelector(session, t2) &&
              InventoryContainsExactActiveSelector(fixture, t1) &&
              InventoryContainsExactActiveSelector(fixture, t2),
          "one session did not own two independent engine-issued selectors");
  Require(session.local_transaction_id ==
                  default_transaction.local_transaction_id &&
              session.transaction_uuid ==
                  default_transaction.transaction_uuid &&
              session.snapshot_visible_through_local_transaction_id ==
                  default_transaction
                      .snapshot_visible_through_local_transaction_id &&
              session.default_local_transaction_id ==
                  default_transaction.local_transaction_id,
          "begin-additional swapped the neutral default projection");

  const auto inserted = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecuteFrameV2(route.session_uuid,
                           NeutralInsertEnvelope(fixture),
                           1,
                           &t1));
  const auto inserted_result = DecodeAcceptedV2Result(
      inserted, "neutral V2 T1 mutation did not reach the engine");
  Require(inserted_result.row_count == 1 &&
              inserted_result.affected_rows_present &&
              inserted_result.affected_rows == 1,
          "neutral V2 T1 mutation lost its exact engine completion count");
  RequireSelectedExact(inserted,
                       t1,
                       "neutral V2 mutation lost its exact T1 selector");

  for (std::uint64_t ordinal = 2; ordinal <= 4; ++ordinal) {
    const auto additional = server::HandleExecuteSblr(
        &route.registry,
        route.engine_state,
        RoutedExecuteFrameV2(
            route.session_uuid,
            NeutralInsertEnvelope(
                fixture, "neutral-v2-delete-row-" + std::to_string(ordinal)),
            1,
            &t1));
    const auto additional_result = DecodeAcceptedV2Result(
        additional, "neutral V2 additional mutation did not reach the engine");
    Require(additional_result.affected_rows_present &&
                additional_result.affected_rows == 1,
            "neutral V2 additional mutation lost its exact engine completion count");
    RequireSelectedExact(additional,
                         t1,
                         "neutral V2 additional mutation lost its exact T1 selector");
  }

  const auto hidden_from_t2 = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecuteFrameV2(route.session_uuid,
                           NeutralSelectEnvelope(fixture),
                           1,
                           &t2));
  Require(DecodeAcceptedV2RowCount(
              hidden_from_t2,
              "neutral V2 T2 pre-commit read did not reach the engine") == 0,
          "T2 observed T1's uncommitted mutation");
  RequireSelectedExact(hidden_from_t2,
                       t2,
                       "neutral V2 pre-commit read lost its exact T2 selector");
  Require(session.local_transaction_id ==
                  default_transaction.local_transaction_id &&
              session.transaction_uuid ==
                  default_transaction.transaction_uuid &&
              session.snapshot_visible_through_local_transaction_id ==
                  default_transaction
                      .snapshot_visible_through_local_transaction_id,
          "selected T1/T2 execution swapped the neutral default projection");

  const auto committed_t1 = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecuteFrameV2(
          route.session_uuid,
          NeutralTransactionEnvelope("transaction.commit",
                                     "SBLR_TRANSACTION_COMMIT"),
          1,
          &t1));
  Require(committed_t1.accepted &&
              committed_t1.transaction_state.has_value() &&
              committed_t1.transaction_state->finality ==
                  server::ServerTransactionResponseState::Finality::
                      kKnownApplied &&
              committed_t1.transaction_state->finalized_present &&
              SameExactSelector(
                  committed_t1.transaction_state->finalized, t1),
          "neutral V2 T1 commit did not publish exact known-applied finality");
  RequireSelectedExact(committed_t1,
                       t1,
                       "neutral V2 T1 commit lost its exact selected identity");
  Require(!session.transactions_by_local_id.contains(
              t1.local_transaction_id) &&
              !InventoryContainsExactActiveSelector(fixture, t1) &&
              SessionContainsExactActiveSelector(session, t2) &&
              session.local_transaction_id ==
                  default_transaction.local_transaction_id &&
              session.transaction_uuid ==
                  default_transaction.transaction_uuid &&
              session.snapshot_visible_through_local_transaction_id ==
                  default_transaction
                      .snapshot_visible_through_local_transaction_id,
          "T1 finality retired or replaced the wrong session selector");

  const auto t3 = BeginAdditionalTransaction(&route, "read_committed");
  Require(t3.local_transaction_id != t1.local_transaction_id &&
              t3.local_transaction_id != t2.local_transaction_id &&
              t3.transaction_uuid != t1.transaction_uuid &&
              t3.transaction_uuid != t2.transaction_uuid &&
              SessionContainsExactActiveSelector(session, t3) &&
              InventoryContainsExactActiveSelector(fixture, t3),
          "post-commit visibility boundary did not issue a distinct selector");
  const auto visible_from_t3 = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecuteFrameV2(route.session_uuid,
                           NeutralSelectEnvelope(fixture),
                           1,
                           &t3));
  Require(DecodeAcceptedV2RowCount(
              visible_from_t3,
              "neutral V2 T3 post-commit read did not reach the engine") == 1,
          "post-commit transaction did not observe T1's committed mutation");
  RequireSelectedExact(visible_from_t3,
                       t3,
                       "neutral V2 post-commit read lost its exact T3 selector");
  Require(session.local_transaction_id ==
                  default_transaction.local_transaction_id &&
              session.transaction_uuid ==
                  default_transaction.transaction_uuid &&
              session.snapshot_visible_through_local_transaction_id ==
                  default_transaction
                      .snapshot_visible_through_local_transaction_id,
          "post-commit T3 execution swapped the neutral default projection");

  const auto deleted = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecuteFrameV2(route.session_uuid,
                           NeutralDeleteEnvelope(fixture),
                           1,
                           &t3));
  const auto deleted_result = DecodeAcceptedV2Result(
      deleted, "neutral V2 DELETE did not reach the engine");
  Require(deleted_result.affected_rows_present &&
              deleted_result.affected_rows == 4,
          "neutral V2 DELETE lost the engine-authoritative four-row completion count");
  RequireSelectedExact(deleted,
                       t3,
                       "neutral V2 DELETE lost its exact T3 selector");

  const auto deleted_empty = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      RoutedExecuteFrameV2(route.session_uuid,
                           NeutralDeleteEnvelope(fixture),
                           1,
                           &t3));
  const auto deleted_empty_result = DecodeAcceptedV2Result(
      deleted_empty, "neutral V2 empty DELETE did not reach the engine");
  Require(deleted_empty_result.affected_rows_present &&
              deleted_empty_result.affected_rows == 0,
          "neutral V2 empty DELETE confused exact zero with a missing completion count");
  RequireSelectedExact(deleted_empty,
                       t3,
                       "neutral V2 empty DELETE lost its exact T3 selector");

  const auto verify_payload_policy_completion =
      [&](std::string_view policy, std::string_view value) {
        const auto added = server::HandleExecuteSblr(
            &route.registry,
            route.engine_state,
            RoutedExecuteFrameV2(route.session_uuid,
                                 NeutralInsertEnvelope(fixture, value),
                                 1,
                                 &t3));
        const auto added_result = DecodeAcceptedV2Result(
            added, "neutral V2 policy fixture INSERT did not reach the engine");
        Require(added_result.affected_rows_present &&
                    added_result.affected_rows == 1,
                "neutral V2 policy fixture INSERT lost its exact completion count");

        const auto removed = server::HandleExecuteSblr(
            &route.registry,
            route.engine_state,
            RoutedExecuteFrameV2(route.session_uuid,
                                 NeutralDeleteEnvelope(fixture, policy),
                                 1,
                                 &t3));
        const auto removed_result = DecodeAcceptedV2Result(
            removed, "neutral V2 policy DELETE did not reach the engine");
        Require(removed_result.affected_rows_present &&
                    removed_result.affected_rows == 1,
                "neutral V2 policy DELETE coupled completion count to row-payload presentation");
        RequireSelectedExact(removed,
                             t3,
                             "neutral V2 policy DELETE lost its exact T3 selector");
      };
  verify_payload_policy_completion("full_payload",
                                   "neutral-v2-full-payload-row");
  verify_payload_policy_completion("summary_only",
                                   "neutral-v2-summary-payload-row");

  // Orderly detach is the one-shot cleanup authority for every remaining
  // selector. Unknown finality retains the session in quarantine and fails
  // these assertions; this proof never retries or guesses rollback outcome.
  const auto session_key = server::UuidBytesToText(route.session_uuid);
  const auto disconnected = server::HandleDisconnectNotice(
      &route.registry, OrderlyDisconnectFrame(route.session_uuid));
  Require(disconnected.accepted &&
              !route.registry.sessions_by_uuid.contains(session_key) &&
              !InventoryContainsExactActiveSelector(fixture, t2) &&
              !InventoryContainsExactActiveSelector(fixture, t3) &&
              !InventoryContainsExactActiveSelector(fixture,
                                                    default_transaction),
          "orderly neutral V2 cleanup did not prove exact engine rollback");
}

}  // namespace

int main() {
  auto memory_policy = memory::DefaultLocalEngineMemoryPolicy();
  memory_policy.policy_name =
      "parser_server_neutral_transaction_routing_conformance";
  const auto memory_configured =
      memory::ConfigureDefaultMemoryManagerForFixture(
          memory_policy,
          "parser_server_neutral_transaction_routing_conformance");
  Require(memory_configured.ok(),
          "neutral transaction memory fixture configuration failed");
  VerifyNeutralCodecAndPreEngineFinality();
  VerifySessionOwnedPreparedCloseAfterFinality();
  VerifyChannelScopedQuarantine();
  VerifyDefaultMapAndHelloInvariants();
  VerifyDedicatedV2ClientChannelIdentity();
  VerifyEngineCompositeFinalityAuthority();
  VerifyNeutralPersistedRelationProjection();
  VerifyNeutralProceduralBlockBridge();
  VerifyServerRoutedCreateOrAlterRoutineEnvelope();
  VerifyTransferablePreparedRoutineMetadata();
  VerifyLiveV2MultiTransactionVisibility();
  std::cout << "parser_server_neutral_transaction_routing_conformance=passed\n";
  return EXIT_SUCCESS;
}
