// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "database_lifecycle.hpp"
#include "lowering/lowering.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
using scratchbird::server::HostedDatabaseSnapshot;
using scratchbird::server::HostedDatabaseState;
using scratchbird::server::HostedEngineState;
using scratchbird::server::ServerSessionRecord;
using scratchbird::server::ServerSessionRegistry;
namespace db = scratchbird::storage::database;
namespace sbps = scratchbird::server::sbps;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

struct ExecuteResultForTest {
  std::string outcome;
  std::array<std::uint8_t, 16> request_uuid{};
  std::array<std::uint8_t, 16> cursor_uuid{};
  std::uint64_t row_count = 0;
  std::string operation_id;
  std::string row_packet;
  std::string detail;
};

struct ArchiveRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view sql;
  std::string_view archive_operation;
  std::string_view required_right;
};

struct ActiveTransactionFixture {
  std::uint64_t local_transaction_id = 0;
  std::uint64_t snapshot_visible_through_local_transaction_id = 0;
  std::string transaction_uuid;
};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  for (const auto& value : values) {
    if (value == expected) return true;
  }
  return false;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_sbsfc_073_archive_replication.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "SBSFC-073 mkdtemp failed");
  return std::filesystem::path(made);
}

std::uint16_t GetU16(const std::vector<std::uint8_t>& data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

std::uint64_t GetU64(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint64_t value = 0;
  for (int byte = 7; byte >= 0; --byte) {
    value <<= 8u;
    value |= data[offset + static_cast<std::size_t>(byte)];
  }
  return value;
}

bool ReadString(const std::vector<std::uint8_t>& data,
                std::size_t* offset,
                std::string* out) {
  if (offset == nullptr || out == nullptr || *offset + 2 > data.size()) return false;
  std::size_t length = GetU16(data, *offset);
  *offset += 2;
  if (*offset + length > data.size()) return false;
  out->assign(reinterpret_cast<const char*>(data.data() + *offset), length);
  *offset += length;
  return true;
}

std::array<std::uint8_t, 16> GetUuid(const std::vector<std::uint8_t>& data,
                                     std::size_t offset) {
  std::array<std::uint8_t, 16> uuid{};
  std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(offset),
              uuid.size(),
              uuid.begin());
  return uuid;
}

ExecuteResultForTest DecodeExecuteResult(const std::vector<std::uint8_t>& payload) {
  ExecuteResultForTest result;
  std::size_t offset = 0;
  Require(ReadString(payload, &offset, &result.outcome),
          "SBSFC-073 execute outcome missing");
  Require(offset + 16 <= payload.size(), "SBSFC-073 execute request UUID missing");
  result.request_uuid = GetUuid(payload, offset);
  offset += 16;
  Require(offset + 16 <= payload.size(), "SBSFC-073 execute cursor UUID missing");
  result.cursor_uuid = GetUuid(payload, offset);
  offset += 16;
  Require(offset + 8 <= payload.size(), "SBSFC-073 execute row count missing");
  result.row_count = GetU64(payload, offset);
  offset += 8;
  Require(ReadString(payload, &offset, &result.operation_id),
          "SBSFC-073 execute operation id missing");
  Require(ReadString(payload, &offset, &result.row_packet),
          "SBSFC-073 execute row packet missing");
  Require(ReadString(payload, &offset, &result.detail),
          "SBSFC-073 execute detail missing");
  return result;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f7300-0000-7000-8000-000000000301";
  session.connection_uuid = "019f7300-0000-7000-8000-000000000302";
  session.database_uuid = "019f7300-0000-7000-8000-000000000303";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 73;
  session.security_policy_epoch = 74;
  session.descriptor_epoch = 75;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_073_archive_replication";
  config.parser_uuid = "019f7300-0000-7000-8000-000000000304";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-073-archive-replication";
  config.build_id = "sbsql-sbsfc-073-archive-replication";
  return config;
}

PipelineArtifacts RunPipeline(std::string_view sql) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(std::string(sql));
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session, {});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void PrintMessages(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
}

void RequireCleanPipeline(const PipelineArtifacts& artifacts,
                          const ArchiveRow& row) {
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-073 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-073 AST failed");
  Require(artifacts.bound.bound, "SBSFC-073 bind failed");
  Require(artifacts.verifier.admitted, "SBSFC-073 SBLR verifier rejected");
  Require(artifacts.envelope.operation_family == "sblr.archive_replication.operation.v3",
          "SBSFC-073 operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == "sblr.archive_replication.operation.v3",
          "SBSFC-073 operation key mismatch");
  Require(artifacts.envelope.operation_id == row.operation_id,
          "SBSFC-073 operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == row.opcode,
          "SBSFC-073 opcode mismatch");
  Require(artifacts.envelope.engine_api_function.find("Engine") == 0,
          "SBSFC-073 missing engine API function");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-073 allowed parser SQL execution");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.backup_archive_api_required"),
          "SBSFC-073 missing backup/archive API authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.backup_archive_lifecycle_required"),
          "SBSFC-073 missing lifecycle authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_snapshot_or_recovery_required"),
          "SBSFC-073 missing MGA snapshot/recovery authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-073 missing no SQL execution authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-073 missing no storage/finality authority");
  Require(HasValue(artifacts.envelope.required_rights, row.required_right),
          "SBSFC-073 missing expected right");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.backup.archive_lifecycle"),
          "SBSFC-073 missing lifecycle descriptor");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.archive.manifest"),
          "SBSFC-073 missing archive manifest descriptor");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.storage.filespace"),
          "SBSFC-073 missing filespace descriptor");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.mga.relation_store"),
          "SBSFC-073 missing MGA relation descriptor");
  Require(Contains(artifacts.envelope.payload, "\"archive_replication_control\":true"),
          "SBSFC-073 missing archive route payload");
  Require(Contains(artifacts.envelope.payload, row.archive_operation),
          "SBSFC-073 missing archive operation payload");
  Require(Contains(artifacts.envelope.payload, "\"backup_archive_api\""),
          "SBSFC-073 missing API payload");
  Require(Contains(artifacts.envelope.payload, "\"target_object_uuid\":\"database:session\""),
          "SBSFC-073 missing database target UUID");
  Require(Contains(artifacts.envelope.payload, "\"authoritative_wal_allowed\":false"),
          "SBSFC-073 missing anti-WAL marker");
  Require(Contains(artifacts.envelope.payload, "\"row_storage_touched\":false"),
          "SBSFC-073 missing no row storage marker");
  Require(Contains(artifacts.envelope.payload, "\"mga_finality_claimed\":false"),
          "SBSFC-073 missing no MGA finality marker");
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_archive\":false"),
          "SBSFC-073 missing parser archive refusal marker");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "SBSFC-073 missing no SQL text marker");
  Require(Contains(artifacts.envelope.payload, row.surface_id),
          "SBSFC-073 payload missing row surface id");
  Require(Contains(artifacts.envelope.payload, "SBSQL-A767F5172F1E"),
          "SBSFC-073 payload missing archive_replication_stmt row id");
  Require(!Contains(artifacts.envelope.payload, row.sql),
          "SBSFC-073 embedded source SQL text");
}

const std::filesystem::path& TempDir() {
  static const std::filesystem::path dir = MakeTempDir();
  return dir;
}

std::string Manifest(const char* name) {
  return (TempDir() / name).string();
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810497000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810497001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810497002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "SBSFC-073 server route database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& database_path,
                                        std::string_view database_uuid) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "sbsql-sbsfc-073-transaction-begin";
  context.database_path = database_path.string();
  context.database_uuid.canonical = std::string(database_uuid);
  context.session_uuid.canonical = "019f7300-0000-7000-8000-000000000401";
  context.principal_uuid.canonical = "019f7300-0000-7000-8000-000000000402";
  context.security_context_present = true;
  context.catalog_generation_id = 73;
  context.security_epoch = 74;
  context.resource_epoch = 75;
  context.name_resolution_epoch = 76;
  context.trace_tags.push_back("right:ARCHIVE");
  context.trace_tags.push_back("right:RESTORE");
  context.trace_tags.push_back("right:REPLICATE");
  return context;
}

ActiveTransactionFixture BeginArchiveTransaction(const std::filesystem::path& database_path,
                                                 std::string_view database_uuid) {
  api::EngineBeginTransactionRequest begin;
  begin.context = EngineContext(database_path, database_uuid);
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  for (const auto& diagnostic : begun.diagnostics) {
    if (diagnostic.error) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(begun.ok, "SBSFC-073 transaction begin failed");
  Require(begun.local_transaction_id != 0,
          "SBSFC-073 transaction begin returned no local transaction id");
  ActiveTransactionFixture fixture;
  fixture.local_transaction_id = begun.local_transaction_id;
  fixture.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  fixture.transaction_uuid = begun.transaction_uuid.canonical;
  return fixture;
}

const ArchiveRow kRows[] = {
    {"SBSQL-01F52A6E564D", "backup_stmt",
     "backup_archive.start_logical_backup", "SBLR_BACKUP_ARCHIVE_START_LOGICAL_BACKUP",
     "", "logical_backup", "right.archive"},
    {"SBSQL-9854186AEBB5", "backup_options",
     "backup_archive.start_logical_backup", "SBLR_BACKUP_ARCHIVE_START_LOGICAL_BACKUP",
     "", "logical_backup", "right.archive"},
    {"SBSQL-57D59EB5A619", "restore_stmt",
     "backup_archive.restore_logical_backup", "SBLR_BACKUP_ARCHIVE_RESTORE_LOGICAL_BACKUP",
     "", "logical_restore", "right.restore"},
    {"SBSQL-3F340C178247", "restore_options",
     "backup_archive.restore_logical_backup", "SBLR_BACKUP_ARCHIVE_RESTORE_LOGICAL_BACKUP",
     "", "logical_restore", "right.restore"},
    {"SBSQL-A5F3182B0ED9", "archive_stmt",
     "backup_archive.package_delta_stream", "SBLR_BACKUP_ARCHIVE_PACKAGE_DELTA_STREAM",
     "", "delta_archive", "right.archive"},
    {"SBSQL-A767F5172F1E", "archive_replication_stmt",
     "backup_archive.package_delta_stream", "SBLR_BACKUP_ARCHIVE_PACKAGE_DELTA_STREAM",
     "", "delta_archive", "right.archive"},
    {"SBSQL-F0FB51E3734D", "replication_stmt",
     "backup_archive.package_delta_stream", "SBLR_BACKUP_ARCHIVE_PACKAGE_DELTA_STREAM",
     "REPLICATE DATABASE TO replica_a;", "replication_delta", "right.replicate"},
    {"SBSQL-C85DE125F4AF", "changefeed_stmt",
     "backup_archive.package_delta_stream", "SBLR_BACKUP_ARCHIVE_PACKAGE_DELTA_STREAM",
     "CHANGEFEED DATABASE TO feed_a;", "changefeed_delta", "right.replicate"},
    {"SBSQL-A42E75DE4695", "changefeed_options",
     "backup_archive.package_delta_stream", "SBLR_BACKUP_ARCHIVE_PACKAGE_DELTA_STREAM",
     "CHANGEFEED DATABASE TO feed_json WITH FORMAT JSON;", "changefeed_delta",
     "right.replicate"},
};

std::string SqlForRow(const ArchiveRow& row) {
  if (!row.sql.empty()) return std::string(row.sql);
  if (row.surface_id == "SBSQL-01F52A6E564D") {
    return "BACKUP DATABASE TO '" + Manifest("source.sblbak") + "';";
  }
  if (row.surface_id == "SBSQL-9854186AEBB5") {
    return "BACKUP DATABASE TO '" + Manifest("source_with_options.sblbak") + "' WITH CHECKSUM;";
  }
  if (row.surface_id == "SBSQL-57D59EB5A619") {
    return "RESTORE DATABASE FROM '" + Manifest("source.sblbak") + "';";
  }
  if (row.surface_id == "SBSQL-3F340C178247") {
    return "RESTORE DATABASE FROM '" + Manifest("source.sblbak") + "' WITH VERIFY;";
  }
  return "ARCHIVE DATABASE TO '" + Manifest("source.sbdelta") + "';";
}

void RequireRegistryEvidence() {
  for (const auto& row : kRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "SBSFC-073 generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "SBSFC-073 generated registry canonical name drifted");
    Require(registry_row->source_status == "native_now",
            "SBSFC-073 generated registry source status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "SBSFC-073 generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == "sblr.archive_replication.operation.v3",
            "SBSFC-073 generated registry SBLR family drifted");
  }
}

ServerSessionRegistry MakeRegistry(std::array<std::uint8_t, 16>* session_uuid,
                                   const std::filesystem::path& database_path,
                                   std::string_view database_uuid,
                                   const ActiveTransactionFixture& transaction) {
  ServerSessionRegistry registry;
  ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = database_path.string();
  session.database_uuid = std::string(database_uuid);
  session.catalog_generation = 73;
  session.security_epoch = 74;
  session.descriptor_epoch = 75;
  session.grant_epoch = 1;
  session.policy_generation = 1;
  session.name_resolution_epoch = 1;
  session.resource_epoch = 1;
  session.local_transaction_id = transaction.local_transaction_id;
  session.snapshot_visible_through_local_transaction_id =
      transaction.snapshot_visible_through_local_transaction_id;
  session.transaction_uuid = transaction.transaction_uuid;
  *session_uuid = session.session_uuid;
  registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] = session;
  return registry;
}

HostedEngineState MakeEngineState(const std::filesystem::path& database_path,
                                  std::string_view database_uuid) {
  HostedEngineState state;
  state.engine_context_active = true;
  HostedDatabaseSnapshot database;
  database.state = HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = database_path.string();
  database.database_uuid = std::string(database_uuid);
  state.databases.push_back(database);
  return state;
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(session_uuid, {}, encoded);
  return frame;
}

ExecuteResultForTest ExecuteAccepted(ServerSessionRegistry* registry,
                                     const HostedEngineState& engine_state,
                                     const std::array<std::uint8_t, 16>& session_uuid,
                                     const PipelineArtifacts& artifacts) {
  const auto result = scratchbird::server::HandleExecuteSblr(
      registry, engine_state, ExecuteFrame(session_uuid, artifacts.envelope.payload));
  if (!result.accepted) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << "  " << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(result.accepted, "SBSFC-073 server route was not accepted");
  auto decoded = DecodeExecuteResult(result.payload);
  Require(decoded.outcome == "accepted", "SBSFC-073 execute outcome mismatch");
  Require(decoded.operation_id == artifacts.envelope.operation_id,
          "SBSFC-073 execute operation id mismatch");
  Require(decoded.row_count == 1, "SBSFC-073 execute row count mismatch");
  Require(Contains(decoded.row_packet, "\"status\":\"accepted\""),
          "SBSFC-073 execute row packet missing accepted status");
  Require(Contains(decoded.detail, "archive_replication_route=backup_archive_api"),
          "SBSFC-073 execute detail missing archive route evidence");
  if (!Contains(decoded.detail, "authoritative_wal=false")) {
    std::cerr << decoded.detail << '\n';
  }
  Require(Contains(decoded.detail, "authoritative_wal=false"),
          "SBSFC-073 execute detail missing anti-WAL evidence");
  return decoded;
}

void RequireServerRoute() {
  const auto temp_dir = TempDir();
  const auto database_path = temp_dir / "source.sbdb";
  const auto database_uuid = CreateMinimalDatabase(database_path);
  const auto transaction = BeginArchiveTransaction(database_path, database_uuid);
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid, database_path, database_uuid, transaction);
  const auto engine_state = MakeEngineState(database_path, database_uuid);

  const auto backup = RunPipeline("BACKUP DATABASE TO '" + Manifest("source.sblbak") + "';");
  (void)ExecuteAccepted(&registry, engine_state, session_uuid, backup);
  Require(std::filesystem::exists(Manifest("source.sblbak")),
          "SBSFC-073 logical backup manifest was not created");

  const auto backup_with_options =
      RunPipeline("BACKUP DATABASE TO '" + Manifest("source_with_options.sblbak") + "' WITH CHECKSUM;");
  (void)ExecuteAccepted(&registry, engine_state, session_uuid, backup_with_options);
  Require(std::filesystem::exists(Manifest("source_with_options.sblbak")),
          "SBSFC-073 logical backup options manifest was not created");

  const auto restore = RunPipeline("RESTORE DATABASE FROM '" + Manifest("source.sblbak") + "';");
  (void)ExecuteAccepted(&registry, engine_state, session_uuid, restore);

  const auto restore_with_options =
      RunPipeline("RESTORE DATABASE FROM '" + Manifest("source.sblbak") + "' WITH VERIFY;");
  (void)ExecuteAccepted(&registry, engine_state, session_uuid, restore_with_options);

  const auto archive = RunPipeline("ARCHIVE DATABASE TO '" + Manifest("source.sbdelta") + "';");
  (void)ExecuteAccepted(&registry, engine_state, session_uuid, archive);
  Require(std::filesystem::exists(Manifest("source.sbdelta")),
          "SBSFC-073 delta archive manifest was not created");

  const auto replicate = RunPipeline("REPLICATE DATABASE TO replica_a;");
  (void)ExecuteAccepted(&registry, engine_state, session_uuid, replicate);
  Require(std::filesystem::exists(database_path.string() + ".replica_a.sbdelta"),
          "SBSFC-073 replication delta manifest was not created");

  const auto changefeed = RunPipeline("CHANGEFEED DATABASE TO feed_json WITH FORMAT JSON;");
  (void)ExecuteAccepted(&registry, engine_state, session_uuid, changefeed);
  Require(std::filesystem::exists(database_path.string() + ".feed_json.sbdelta"),
          "SBSFC-073 changefeed delta manifest was not created");
}

}  // namespace

int main() {
  RequireRegistryEvidence();

  for (const auto& row : kRows) {
    auto sql = SqlForRow(row);
    auto row_with_sql = row;
    row_with_sql.sql = sql;
    const auto artifacts = RunPipeline(sql);
    RequireCleanPipeline(artifacts, row_with_sql);
    if (row.surface_id == "SBSQL-A42E75DE4695") {
      Require(Contains(artifacts.envelope.payload, "\"changefeed_format_json\":true"),
              "SBSFC-073 changefeed options did not preserve FORMAT JSON");
      Require(Contains(artifacts.envelope.payload, "SBSQL-A42E75DE4695"),
              "SBSFC-073 missing changefeed options row id");
    }
    if (row.surface_id == "SBSQL-9854186AEBB5" ||
        row.surface_id == "SBSQL-3F340C178247") {
      Require(Contains(artifacts.envelope.payload, "\"option_list_present\":true"),
              "SBSFC-073 option row did not preserve option-list evidence");
    }
    if (row.surface_id == "SBSQL-3F340C178247") {
      Require(Contains(artifacts.envelope.payload, "\"restore_verify_only\":true"),
              "SBSFC-073 RESTORE WITH VERIFY did not preserve verify-only mode");
    }
  }

  RequireServerRoute();
  return EXIT_SUCCESS;
}
