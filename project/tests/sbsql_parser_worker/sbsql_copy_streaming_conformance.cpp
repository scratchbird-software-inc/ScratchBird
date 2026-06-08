// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "memory.hpp"
#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"
#include "transaction/transaction_api.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace memory = scratchbird::core::memory;
namespace sbps = scratchbird::server::sbps;

#ifndef SB_SBSFC021_SEED_PACK_ROOT
#define SB_SBSFC021_SEED_PACK_ROOT "project/resources/seed-packs/initial-resource-pack"
#endif

constexpr const char* kDatabaseUuid = "019f2110-0000-7000-8000-000000000001";
constexpr const char* kSchemaUuid = "019f2110-0000-7000-8000-000000000101";
constexpr const char* kTableUuid = "019f2110-0000-7000-8000-000000000102";
constexpr const char* kIndexUuid = "019f2110-0000-7000-8000-000000000103";
constexpr const char* kSeedRowUuid = "019f2110-0000-7000-8000-000000000201";
constexpr const char* kImportGoodRowUuid = "019f2110-0000-7000-8000-000000000202";
constexpr const char* kImportRejectRowUuid = "019f2110-0000-7000-8000-000000000203";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "sbsql_copy_streaming_conformance";
  policy.hard_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 48ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "sbsql_copy_streaming_conformance");
  Require(configured.ok(), "COPY stream memory fixture configuration failed");
  Require(configured.fixture_mode,
          "COPY stream memory fixture mode was not active");
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_sbsql_copy_stream_engine.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
}

std::string CopyStreamEnvelope(std::string_view kind,
                               std::uint64_t total_rows,
                               std::uint64_t reject_rows) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.dml.operation.v3\",";
  out += "\"surface_key\":\"fspe010b5.copy_streaming\",";
  out += "\"sblr_operation_key\":\"op.fspe010b5.copy_streaming\",";
  out += "\"result_shape\":\"rs.fspe010b5.copy_stream.v1\",";
  out += "\"diagnostic_shape\":\"diag.fspe010b5.v1\",";
  out += "\"resource_contract\":\"resource.fspe010b5.v1\",";
  out += "\"trace_key\":\"FSPE-010B5\",";
  out += "\"source_payload_embedded\":false,";
  out += "\"resolved_object_uuids\":[\"019e05df-f010-7000-8000-000000000055\"],";
  out += "\"descriptor_refs\":[\"descriptor.copy.target.uuid\"],";
  out += "\"policy_refs\":[\"policy.copy.reject_row\"],";
  out += "\"copy_stream_kind\":\"";
  out += kind;
  out += "\",\"copy_total_rows\":";
  out += std::to_string(total_rows);
  out += ",\"copy_reject_rows\":";
  out += std::to_string(reject_rows);
  out += "}";
  return out;
}

std::string EngineBackedCopyStreamEnvelope() {
  std::string out;
  out += "operation_id=dml.execute_import_rows\n";
  out += "opcode=SBLR_DML_EXECUTE_IMPORT_ROWS\n";
  out += "sblr_operation_family=sblr.dml.operation.v3\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=SBSFC-021-copy-stream\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=true\n";
  out += "requires_cluster_authority=false\n";
  out += "copy_stream_kind=copy_import\n";
  out += "target_object_uuid=";
  out += kTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "source_kind=csv_stream\n";
  out += "source_fingerprint=sbsfc021-copy-stream-engine-backed\n";
  out += "source_position=row:0\n";
  out += "format_family=csv\n";
  out += "encoding=utf8\n";
  out += "line_ending=lf\n";
  out += "delimiter=,\n";
  out += "quote=\"\n";
  out += "escape=\"\n";
  out += "header_policy=absent\n";
  out += "estimated_row_count=2\n";
  out += "duplicate_mode=error\n";
  out += "require_generated_row_uuid=true\n";
  out += "reject_mode=reject_row\n";
  out += "reject_limit_rows=10\n";
  out += "reject_payload_policy=diagnostic_only\n";
  out += "resume_policy=fail_closed\n";
  out += "checkpoint_mode=disabled\n";
  out += "operand=row_field\t";
  out += kImportGoodRowUuid;
  out += "|id\t8\n";
  out += "operand=row_field\t";
  out += kImportGoodRowUuid;
  out += "|note\tstream-valid\n";
  out += "operand=row_field\t";
  out += kImportRejectRowUuid;
  out += "|id\t6\n";
  out += "operand=row_field\t";
  out += kImportRejectRowUuid;
  out += "|note\tstream-duplicate\n";
  return out;
}

scratchbird::server::HostedEngineState MakeEngineState() {
  scratchbird::server::HostedEngineState state;
  state.engine_context_active = true;
  scratchbird::server::HostedDatabaseSnapshot database;
  database.state = scratchbird::server::HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = "/tmp/sb_copy_streaming_conformance.sbdb";
  database.database_uuid = "019e05df-f010-7000-8000-000000000015";
  state.databases.push_back(database);
  return state;
}

scratchbird::server::HostedEngineState MakeEngineStateForDatabase(const std::filesystem::path& database_path) {
  scratchbird::server::HostedEngineState state;
  state.engine_context_active = true;
  scratchbird::server::HostedDatabaseSnapshot database;
  database.state = scratchbird::server::HostedDatabaseState::kOpen;
  database.database_created = true;
  database.database_open = true;
  database.database_path = database_path.string();
  database.database_uuid = kDatabaseUuid;
  state.databases.push_back(database);
  return state;
}

scratchbird::server::ServerSessionRegistry MakeRegistry(
    std::array<std::uint8_t, 16>* session_uuid) {
  scratchbird::server::ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = "/tmp/sb_copy_streaming_conformance.sbdb";
  session.database_uuid = "019e05df-f010-7000-8000-000000000015";
  *session_uuid = session.session_uuid;
  scratchbird::server::ServerSessionRegistry registry;
  registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] = session;
  return registry;
}

scratchbird::server::ServerSessionRegistry MakeRegistryForDatabase(
    const std::filesystem::path& database_path,
    std::uint64_t local_transaction_id,
    std::array<std::uint8_t, 16>* session_uuid) {
  scratchbird::server::ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = database_path.string();
  session.database_uuid = kDatabaseUuid;
  session.local_transaction_id = local_transaction_id;
  session.snapshot_visible_through_local_transaction_id = local_transaction_id;
  *session_uuid = session.session_uuid;
  scratchbird::server::ServerSessionRegistry registry;
  registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] = session;
  return registry;
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
      session_uuid, {}, encoded, true);
  return frame;
}

sbps::Frame FetchFrame(const std::array<std::uint8_t, 16>& session_uuid,
                       const std::array<std::uint8_t, 16>& cursor_uuid,
                       std::uint64_t max_rows) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kFetch);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeFetchPayloadForTest(session_uuid, cursor_uuid, max_rows);
  return frame;
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.descriptor.encoded_descriptor = "type=text";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineLocalizedName Name(std::string name) {
  return {"en", "primary", name, name, true};
}

api::EngineColumnDefinition Column(std::uint32_t ordinal, std::string name) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.requested_column_uuid.canonical =
      "019f2110-0000-7000-8000-00000000030" + std::to_string(ordinal);
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_uuid.canonical =
      "019f2110-0000-7000-8000-00000000040" + std::to_string(ordinal);
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "text";
  column.descriptor.encoded_descriptor = "type=text";
  return column;
}

api::EngineIndexDefinition UniqueIdIndex() {
  api::EngineIndexDefinition index;
  index.requested_index_uuid.canonical = kIndexUuid;
  index.names.push_back(Name("sbsfc021_stream_table_id_unique"));
  index.index_kind = "btree";
  index.key_envelopes.push_back("unique");
  index.key_envelopes.push_back("id");
  return index;
}

api::EngineRowValue Row(std::string row_uuid, std::string id, std::string note) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

api::EngineRequestContext BaseContext(const std::filesystem::path& database_path) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "sbsql-copy-streaming-conformance";
  context.database_path = database_path.string();
  context.database_uuid.canonical = kDatabaseUuid;
  context.principal_uuid.canonical = "019f2110-0000-7000-8000-000000000002";
  context.session_uuid.canonical = "019f2110-0000-7000-8000-000000000003";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext BeginTransaction(const std::filesystem::path& database_path) {
  api::EngineBeginTransactionRequest begin;
  begin.context = BaseContext(database_path);
  const auto result = api::EngineBeginTransaction(begin);
  Require(result.ok && result.local_transaction_id != 0, "engine-backed stream transaction begin failed");
  auto context = BaseContext(database_path);
  context.local_transaction_id = result.local_transaction_id;
  context.transaction_uuid = result.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id = result.snapshot_visible_through_local_transaction_id;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest commit;
  commit.context = context;
  const auto result = api::EngineCommitTransaction(commit);
  Require(result.ok, "engine-backed stream transaction commit failed");
}

std::filesystem::path CreateEngineBackedFixture() {
  const auto work = MakeTempDir();
  Require(!work.empty(), "failed to create temp directory for engine-backed COPY stream");
  const auto database_path = work / "sbsfc021_copy_stream.sbdb";

  api::EngineCreateLifecycleRequest create;
  create.context = BaseContext(database_path);
  create.option_envelopes.push_back(std::string("resource_seed_pack_root:") +
                                    SB_SBSFC021_SEED_PACK_ROOT);
  const auto created = api::EngineCreateLifecycle(create);
  Require(created.ok, "engine-backed stream database create failed");

  api::EngineOpenLifecycleRequest open;
  open.context = BaseContext(database_path);
  const auto opened = api::EngineOpenLifecycle(open);
  Require(opened.ok, "engine-backed stream database open failed");

  auto ddl = BeginTransaction(database_path);
  api::EngineCreateSchemaRequest schema;
  schema.context = ddl;
  schema.target_object.uuid.canonical = kSchemaUuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(Name("sbsfc021_stream_schema"));
  const auto created_schema = api::EngineCreateSchema(schema);
  Require(created_schema.ok, "engine-backed stream schema create failed");

  api::EngineCreateTableRequest table;
  table.context = ddl;
  table.target_schema.uuid.canonical = kSchemaUuid;
  table.target_schema.object_kind = "schema";
  table.requested_table_uuid.canonical = kTableUuid;
  table.table_names.push_back(Name("sbsfc021_stream_table"));
  table.table_columns.push_back(Column(0, "id"));
  table.table_columns.push_back(Column(1, "note"));
  table.table_indexes.push_back(UniqueIdIndex());
  const auto created_table = api::EngineCreateTable(table);
  Require(created_table.ok, "engine-backed stream table create failed");
  Commit(ddl);

  auto seed = BeginTransaction(database_path);
  api::EngineInsertRowsRequest insert;
  insert.context = seed;
  insert.target_table.uuid.canonical = kTableUuid;
  insert.target_table.object_kind = "table";
  insert.input_rows.push_back(Row(kSeedRowUuid, "6", "stream-baseline"));
  const auto inserted = api::EngineInsertRows(insert);
  Require(inserted.ok, "engine-backed stream seed insert failed");
  Commit(seed);
  return database_path;
}

}  // namespace

int main() {
  ConfigureMemoryFixture();

  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);
  const auto engine_state = MakeEngineState();

  const auto execute = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_uuid, CopyStreamEnvelope("copy_import", 10, 2)));
  Require(execute.accepted, "COPY stream execute was rejected");
  const auto cursor_uuid = scratchbird::server::DecodeCursorUuidForTest(execute.payload);
  Require(cursor_uuid.has_value(), "COPY stream execute did not return a cursor UUID");

  const auto fetch1 = scratchbird::server::HandleFetch(&registry, FetchFrame(session_uuid, *cursor_uuid, 2));
  const auto payload1 = scratchbird::server::DecodeFetchResultForTest(fetch1.payload);
  Require(fetch1.accepted && payload1.has_value() && payload1->row_count == 2 && !payload1->end_of_cursor,
          "COPY first fetch did not return progress plus first reject");
  Require(Contains(payload1->row_packet, "\"event\":\"progress\"") &&
              Contains(payload1->row_packet, "\"rows_processed\":10") &&
              Contains(payload1->row_packet, "\"event\":\"reject_record\"") &&
              Contains(payload1->row_packet, "\"source_row_number\":1"),
          "COPY first fetch missing progress or first reject record");

  const auto fetch2 = scratchbird::server::HandleFetch(&registry, FetchFrame(session_uuid, *cursor_uuid, 2));
  const auto payload2 = scratchbird::server::DecodeFetchResultForTest(fetch2.payload);
  Require(fetch2.accepted && payload2.has_value() && payload2->row_count == 2 && !payload2->end_of_cursor,
          "COPY second fetch did not return second reject plus summary");
  Require(Contains(payload2->row_packet, "\"source_row_number\":2") &&
              Contains(payload2->row_packet, "\"event\":\"bulk_summary\"") &&
              Contains(payload2->row_packet, "\"accepted_rows\":8") &&
              Contains(payload2->row_packet, "\"rejected_rows\":2"),
          "COPY second fetch missing reject record or bulk summary");

  const auto fetch3 = scratchbird::server::HandleFetch(&registry, FetchFrame(session_uuid, *cursor_uuid, 1));
  const auto payload3 = scratchbird::server::DecodeFetchResultForTest(fetch3.payload);
  Require(fetch3.accepted && payload3.has_value() && payload3->row_count == 1 && payload3->end_of_cursor,
          "COPY final fetch did not return final status");
  Require(Contains(payload3->row_packet, "\"event\":\"final_status\"") &&
              Contains(payload3->row_packet, "\"status\":\"completed_with_rejects\"") &&
              Contains(payload3->detail, "\"end_of_cursor\":true"),
          "COPY final fetch missing final status or cursor metadata");

  const auto engine_database_path = CreateEngineBackedFixture();
  auto import_context = BeginTransaction(engine_database_path);
  std::array<std::uint8_t, 16> engine_session_uuid{};
  auto engine_registry = MakeRegistryForDatabase(
      engine_database_path, import_context.local_transaction_id, &engine_session_uuid);
  const auto engine_backed_state = MakeEngineStateForDatabase(engine_database_path);
  const auto engine_execute = scratchbird::server::HandleExecuteSblr(
      &engine_registry, engine_backed_state, ExecuteFrame(engine_session_uuid, EngineBackedCopyStreamEnvelope()));
  Require(engine_execute.accepted, "engine-backed COPY stream execute was rejected");
  const auto engine_cursor_uuid = scratchbird::server::DecodeCursorUuidForTest(engine_execute.payload);
  Require(engine_cursor_uuid.has_value(), "engine-backed COPY stream did not return a cursor");

  const auto engine_fetch1 =
      scratchbird::server::HandleFetch(&engine_registry, FetchFrame(engine_session_uuid, *engine_cursor_uuid, 2));
  const auto engine_payload1 = scratchbird::server::DecodeFetchResultForTest(engine_fetch1.payload);
  Require(engine_fetch1.accepted && engine_payload1.has_value() &&
              engine_payload1->row_count == 2 && !engine_payload1->end_of_cursor,
          "engine-backed COPY first fetch did not stream progress plus reject");
  const bool engine_reject_packet_ok =
      Contains(engine_payload1->row_packet, "\"event\":\"progress\"") &&
      Contains(engine_payload1->row_packet, "\"rows_processed\":2") &&
      Contains(engine_payload1->row_packet, "\"rows_rejected\":1") &&
      Contains(engine_payload1->row_packet, "\"event\":\"reject_record\"") &&
      (Contains(engine_payload1->row_packet, "\"diagnostic_detail\":\"crud.unique_index:unique_index_duplicate\"") ||
       Contains(engine_payload1->row_packet, "\"diagnostic_detail\":\"constraint.primary_key.violation:duplicate_key") ||
       Contains(engine_payload1->row_packet, "\"diagnostic_detail\":\"constraint.unique.violation:duplicate_key") ||
       Contains(engine_payload1->row_packet, "\"diagnostic_detail\":\"duplicate_key:"));
  if (!engine_reject_packet_ok) {
    std::cerr << "engine-backed COPY first fetch packet: "
              << engine_payload1->row_packet << '\n';
  }
  Require(engine_reject_packet_ok,
          "engine-backed COPY first fetch did not use engine reject diagnostics");

  const auto engine_fetch2 =
      scratchbird::server::HandleFetch(&engine_registry, FetchFrame(engine_session_uuid, *engine_cursor_uuid, 2));
  const auto engine_payload2 = scratchbird::server::DecodeFetchResultForTest(engine_fetch2.payload);
  Require(engine_fetch2.accepted && engine_payload2.has_value() &&
              engine_payload2->row_count == 2 && engine_payload2->end_of_cursor,
          "engine-backed COPY final fetch did not stream summary plus final status");
  Require(Contains(engine_payload2->row_packet, "\"event\":\"bulk_summary\"") &&
              Contains(engine_payload2->row_packet, "\"accepted_rows\":1") &&
              Contains(engine_payload2->row_packet, "\"rejected_rows\":1") &&
              Contains(engine_payload2->row_packet, "\"event\":\"final_status\"") &&
              Contains(engine_payload2->row_packet, "\"status\":\"completed_with_rejects\""),
          "engine-backed COPY final fetch missing engine-derived summary or final status");
  Commit(import_context);

  std::cout << "sbsql_copy_streaming_conformance=passed\n";
  return EXIT_SUCCESS;
}
