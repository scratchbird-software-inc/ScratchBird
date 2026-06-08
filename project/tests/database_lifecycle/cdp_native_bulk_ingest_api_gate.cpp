// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_dirty_manifest.hpp"
#include "database_format.hpp"
#include "database_lifecycle.hpp"
#include "dml/native_bulk_ingest_api.hpp"
#include "dml/select_api.hpp"
#include "memory.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "sblr_dispatch_server.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "sblr_to_sbsql.hpp"
#include "session_registry.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace platform = scratchbird::core::platform;
namespace server = scratchbird::server;
namespace sbps = scratchbird::server::sbps;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "cdp_native_bulk_ingest_api_gate";
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
      MemoryPolicy(), "cdp_native_bulk_ingest_api_gate");
  Require(configured.ok(), "CDP-040 memory fixture configuration failed");
  Require(configured.fixture_mode, "CDP-040 memory fixture mode was not active");
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::u64 UniqueMillis() {
  static platform::u64 counter = 0;
  return NowMillis() + (++counter * 1000);
}

platform::TypedUuid NewTypedUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, UniqueMillis() + salt);
  Require(generated.ok(), "CDP-040 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewTypedUuid(kind, salt).value);
}

std::string UuidText(const platform::TypedUuid& value) {
  return uuid::UuidToString(value.value);
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string table_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    if (!dir.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(dir, ignored);
    }
  }
};

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRowValue Row(std::string id, std::string payload) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = NewUuidText(platform::UuidKind::object, 900);
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"payload", TextValue(std::move(payload))});
  return row;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && item.evidence_id == id) return true;
  }
  return false;
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

void RequireDiagnostic(const api::EngineApiResult& result,
                       std::string_view expected_code,
                       std::string_view expected_detail,
                       std::string_view message) {
  Require(!result.ok, message);
  Require(!result.diagnostics.empty(), message);
  const auto& diagnostic = result.diagnostics.front();
  if (diagnostic.code != expected_code) {
    std::cerr << "expected=" << expected_code << " actual=" << diagnostic.code << '\n';
  }
  Require(diagnostic.code == expected_code, message);
  if (!expected_detail.empty()) {
    Require(diagnostic.detail.find(expected_detail) != std::string::npos, message);
  }
}

api::EngineRequestContext BaseContext(const Fixture& fixture, std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical = NewUuidText(platform::UuidKind::principal, fixture.salt + 100);
  context.session_uuid.canonical = NewUuidText(platform::UuidKind::object, fixture.salt + 101);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "CDP-040 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "CDP-040 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "CDP-040 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "cdp_native_bulk_ingest";
  table.columns.push_back({"id", "canonical=character"});
  table.columns.push_back({"payload", "canonical=character"});
  return table;
}

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_cdp040_" + name + "_" + std::to_string(UniqueMillis()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "cdp040.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewTypedUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewTypedUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = UniqueMillis();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "CDP-040 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);

  auto metadata = Begin(fixture, "cdp040-metadata");
  const auto table = api::AppendMgaTableMetadata(metadata, Table(fixture, metadata));
  Require(!table.error, "CDP-040 table metadata append failed");
  Commit(metadata);
  return fixture;
}

std::vector<api::EngineRowValue> Rows(std::string prefix, int count) {
  std::vector<api::EngineRowValue> rows;
  rows.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    rows.push_back(Row(prefix + "-id-" + std::to_string(index + 1),
                       prefix + "-payload-" + std::to_string(index + 1)));
  }
  return rows;
}

api::EngineExecuteNativeBulkIngestRequest NativeRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows) {
  api::EngineExecuteNativeBulkIngestRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.canonical_rows = std::move(rows);
  request.estimated_row_count = static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.import_policy.reject_mode = "fail_fast";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  return request;
}

api::EngineApiU64 SelectCount(const Fixture& fixture,
                              const api::EngineRequestContext& context) {
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = fixture.table_uuid;
  request.source_object.object_kind = "table";
  request.select_projection.canonical_projection_envelopes.push_back("id");
  const auto selected = api::EngineSelectRows(request);
  RequireOk(selected, "CDP-040 select failed");
  return selected.visible_count;
}

sblr::SblrOperand Operand(std::string type, std::string name, std::string value) {
  sblr::SblrOperand operand;
  operand.type = std::move(type);
  operand.name = std::move(name);
  operand.value = std::move(value);
  return operand;
}

sblr::SblrOperationEnvelope NativeEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("dml.execute_native_bulk_ingest",
                                         "SBLR_DML_EXECUTE_NATIVE_BULK_INGEST",
                                         "CDP-040-NATIVE-BULK-INGEST");
  envelope.parser_package_uuid = NewUuidText(platform::UuidKind::object, 2000);
  envelope.registry_snapshot_uuid = NewUuidText(platform::UuidKind::object, 2001);
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.result_shape = "engine_api_result";
  envelope.diagnostic_shape = "engine_api_diagnostic_vector";
  return envelope;
}

sblr::SblrOperationEnvelope NativeRoundTripEnvelope() {
  auto envelope = NativeEnvelope();
  envelope.source_artifact_map.policy_status = "non_authoritative_render_metadata";
  envelope.source_artifact_map.source_identity = "cdp040-native-ingest-render-map";
  envelope.source_artifact_map.source_hash = "sha256:cdp040-native-ingest";
  envelope.source_artifact_map.render_metadata_only = true;
  envelope.source_artifact_map.contains_sql_text = false;
  envelope.source_artifact_map.raw_sql_text_authoritative = false;

  sblr::SblrOperationRenderHint hint;
  hint.hint_kind = "operation";
  hint.stable_key = "cdp040.native_bulk_ingest";
  hint.value = "NATIVE BULK INGEST";
  hint.authoritative = false;
  hint.contains_sql_text = false;
  envelope.source_artifact_map.operation_render_hints.push_back(std::move(hint));

  envelope.operands.push_back(Operand("text", "target_object_uuid", "runtime-generated"));
  envelope.operands.push_back(Operand("text", "target_object_kind", "table"));
  envelope.operands.push_back(Operand("text", "native_bulk_ingest_enabled", "true"));
  return envelope;
}

api::EngineApiRequest SblrApiRequest(const Fixture& fixture,
                                     std::vector<api::EngineRowValue> rows) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.rows = std::move(rows);
  request.option_envelopes.push_back("estimated_row_count:" + std::to_string(request.rows.size()));
  return request;
}

void AppendLine(std::string* out, std::string_view value) {
  out->append(value);
  out->push_back('\n');
}

std::string NativeServerEnvelope(const Fixture& fixture,
                                 const std::vector<api::EngineRowValue>& rows,
                                 bool enabled = true) {
  std::string out;
  AppendLine(&out, "operation_id=dml.execute_native_bulk_ingest");
  AppendLine(&out, "opcode=SBLR_DML_EXECUTE_NATIVE_BULK_INGEST");
  AppendLine(&out, "sblr_operation_family=sblr.dml.operation.v3");
  AppendLine(&out, "result_shape=engine.api.result.v1");
  AppendLine(&out, "diagnostic_shape=engine.diagnostic.v1");
  AppendLine(&out, "trace_key=CDP-040-native-server-public-abi");
  AppendLine(&out, "contains_sql_text=false");
  AppendLine(&out, "parser_resolved_names_to_uuids=true");
  AppendLine(&out, "requires_security_context=true");
  AppendLine(&out, "requires_transaction_context=true");
  AppendLine(&out, "requires_cluster_authority=false");
  AppendLine(&out, "target_object_uuid=" + fixture.table_uuid);
  AppendLine(&out, "target_object_kind=table");
  AppendLine(&out, "estimated_row_count=" + std::to_string(rows.size()));
  AppendLine(&out, std::string("native_bulk_ingest_enabled=") +
                       (enabled ? "true" : "false"));
  AppendLine(&out, "reject_mode=fail_fast");
  AppendLine(&out, "checkpoint_mode=disabled");
  for (const auto& row : rows) {
    for (const auto& field : row.fields) {
      AppendLine(&out,
                 "operand=row_field:character\t" + row.requested_row_uuid.canonical +
                     "|" + field.first + "\t" + field.second.encoded_value);
    }
  }
  return out;
}

server::HostedEngineState MakeEngineState(const Fixture& fixture) {
  server::HostedEngineState state;
  state.engine_context_active = true;
  server::HostedDatabaseSnapshot database;
  database.state = server::HostedDatabaseState::kOpen;
  database.database_created = true;
  database.database_open = true;
  database.database_path = fixture.database_path.string();
  database.database_uuid = fixture.database_uuid;
  state.databases.push_back(database);
  return state;
}

server::ServerSessionRegistry MakeServerRegistry(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::array<std::uint8_t, 16>* session_uuid) {
  server::ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = fixture.database_path.string();
  session.database_uuid = fixture.database_uuid;
  session.local_transaction_id = context.local_transaction_id;
  session.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  session.transaction_uuid = context.transaction_uuid.canonical;
  session.transaction_timestamp = context.transaction_timestamp;
  *session_uuid = session.session_uuid;

  server::ServerSessionRegistry registry;
  registry.sessions_by_uuid[server::UuidBytesToText(session.session_uuid)] = session;
  return registry;
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = server::EncodeExecuteSblrPayloadForTest(session_uuid, {}, encoded, false);
  return frame;
}

std::uint16_t ReadU16(const std::vector<std::uint8_t>& data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

std::uint64_t ReadU64(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint64_t value = 0;
  for (int byte = 7; byte >= 0; --byte) {
    value <<= 8u;
    value |= data[offset + static_cast<std::size_t>(byte)];
  }
  return value;
}

std::string ReadProtocolString(const std::vector<std::uint8_t>& data,
                               std::size_t* offset) {
  Require(*offset + 2 <= data.size(), "CDP-040 protocol string length missing");
  auto length = static_cast<std::uint64_t>(ReadU16(data, *offset));
  *offset += 2;
  if (length == 0xffffu) {
    Require(*offset + 8 <= data.size(), "CDP-040 long protocol string length missing");
    length = ReadU64(data, *offset);
    *offset += 8;
  }
  Require(*offset + length <= data.size(), "CDP-040 protocol string payload truncated");
  std::string out(reinterpret_cast<const char*>(data.data() + *offset),
                  static_cast<std::size_t>(length));
  *offset += static_cast<std::size_t>(length);
  return out;
}

struct ExecuteResultPayload {
  std::string outcome;
  std::uint64_t row_count = 0;
  std::string operation_id;
  std::string row_packet;
  std::string detail;
};

ExecuteResultPayload DecodeExecuteResultPayload(const std::vector<std::uint8_t>& payload) {
  std::size_t offset = 0;
  ExecuteResultPayload result;
  result.outcome = ReadProtocolString(payload, &offset);
  Require(offset + 16 <= payload.size(), "CDP-040 execute result request UUID missing");
  offset += 16;
  Require(offset + 16 <= payload.size(), "CDP-040 execute result cursor UUID missing");
  offset += 16;
  Require(offset + 8 <= payload.size(), "CDP-040 execute result row count missing");
  result.row_count = ReadU64(payload, offset);
  offset += 8;
  result.operation_id = ReadProtocolString(payload, &offset);
  result.row_packet = ReadProtocolString(payload, &offset);
  result.detail = ReadProtocolString(payload, &offset);
  return result;
}

void TestApiAndSblrAcceptedRoutes() {
  auto fixture = MakeFixture("accepted", 1000);

  auto api_context = Begin(fixture, "cdp040-api-accepted");
  auto api_result = api::EngineExecuteNativeBulkIngest(
      NativeRequest(fixture, api_context, Rows("api", 3)));
  RequireOk(api_result, "CDP-040 API native bulk ingest failed");
  Require(api_result.accepted_rows == 3 && api_result.inserted_rows == 3,
          "CDP-040 API native bulk ingest row counts drifted");
  Require(HasEvidence(api_result.evidence, "native_bulk_ingest", "enabled"),
          "CDP-040 API native ingest evidence missing");
  Require(HasEvidence(api_result.evidence, "native_bulk_ingest_route", "engine_internal_api"),
          "CDP-040 API route evidence missing");
  Require(HasEvidence(api_result.evidence, "native_bulk_ingest_source", "binary_typed_rows"),
          "CDP-040 API binary typed source evidence missing");
  Require(HasEvidence(api_result.evidence, "parser_finality_authority", "false"),
          "CDP-040 parser finality evidence missing");
  Require(HasEvidence(api_result.evidence, "donor_finality_authority", "false"),
          "CDP-040 donor finality evidence missing");
  Require(HasEvidence(api_result.evidence, "native_bulk_ingest_lane", "direct_physical"),
          "CDP-040 direct physical lane evidence missing");
  Require(HasEvidence(api_result.evidence, "native_bulk_ingest_delegate", "none"),
          "CDP-040 native ingest delegated instead of using direct lane");
  Require(SelectCount(fixture, api_context) == 3,
          "CDP-040 API native ingest rows not visible in writer transaction");
  Commit(api_context);

  auto sblr_context = Begin(fixture, "cdp040-sblr-accepted");
  sblr::SblrDispatchRequest dispatch;
  dispatch.context = sblr_context;
  dispatch.envelope = NativeEnvelope();
  dispatch.api_request = SblrApiRequest(fixture, Rows("sblr", 2));
  const auto sblr_result = sblr::DispatchSblrOperation(dispatch);
  if (!(sblr_result.accepted && sblr_result.envelope_validated &&
        sblr_result.dispatched_to_api && sblr_result.api_result.ok)) {
    std::cerr << "accepted=" << sblr_result.accepted
              << " envelope_validated=" << sblr_result.envelope_validated
              << " dispatched_to_api=" << sblr_result.dispatched_to_api
              << " api_ok=" << sblr_result.api_result.ok << '\n';
    if (!sblr_result.diagnostics.empty()) {
      std::cerr << "dispatch_diagnostic=" << sblr_result.diagnostics.front().code
                << ':' << sblr_result.diagnostics.front().message << '\n';
    }
    if (!sblr_result.api_result.diagnostics.empty()) {
      std::cerr << "api_diagnostic=" << sblr_result.api_result.diagnostics.front().code
                << ':' << sblr_result.api_result.diagnostics.front().detail << '\n';
    }
    Fail("CDP-040 SBLR native bulk ingest route failed");
  }
  Require(HasEvidence(sblr_result.api_result.evidence, "native_bulk_ingest", "enabled"),
          "CDP-040 SBLR native ingest evidence missing");
  Require(SelectCount(fixture, sblr_context) == 5,
          "CDP-040 SBLR native ingest rows not visible in writer transaction");
  Rollback(sblr_context);
}

void TestDisabledAndInvalidRefusals() {
  auto fixture = MakeFixture("refusals", 2000);
  auto context = Begin(fixture, "cdp040-refusals");

  auto disabled_request = NativeRequest(fixture, context, Rows("disabled", 1));
  disabled_request.option_envelopes.push_back("native_bulk_ingest_enabled:false");
  const auto disabled = api::EngineExecuteNativeBulkIngest(disabled_request);
  RequireDiagnostic(disabled,
                    "DML.NATIVE_BULK_INGEST.DISABLED",
                    "native_bulk_ingest_enabled:false",
                    "CDP-040 disabled refusal diagnostic drifted");
  Require(HasEvidence(disabled.evidence, "native_bulk_ingest", "disabled"),
          "CDP-040 disabled native ingest evidence missing");
  Require(HasEvidence(disabled.evidence, "native_bulk_ingest_enabled", "false"),
          "CDP-040 disabled option evidence missing");

  auto invalid = NativeRequest(fixture, context, {});
  const auto invalid_result = api::EngineExecuteNativeBulkIngest(invalid);
  RequireDiagnostic(invalid_result,
                    "SB_ENGINE_API_INVALID_REQUEST",
                    "canonical_rows_required",
                    "CDP-040 invalid native ingest diagnostic drifted");
  Require(HasEvidence(invalid_result.evidence, "native_bulk_ingest_source", "binary_typed_rows"),
          "CDP-040 invalid path lost native source evidence");
  Rollback(context);
}

void TestServerPublicAbiRoute() {
  auto fixture = MakeFixture("server_public_abi", 2500);
  auto context = Begin(fixture, "cdp040-server-public-abi");
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeServerRegistry(fixture, context, &session_uuid);
  const auto engine_state = MakeEngineState(fixture);

  const auto execute = server::HandleExecuteSblr(
      &registry,
      engine_state,
      ExecuteFrame(session_uuid, NativeServerEnvelope(fixture, Rows("server", 2))));
  Require(execute.accepted, "CDP-040 server public ABI native ingest was rejected");
  const auto payload = DecodeExecuteResultPayload(execute.payload);
  Require(payload.outcome == "accepted", "CDP-040 server native ingest outcome drifted");
  Require(payload.operation_id == "dml.execute_native_bulk_ingest",
          "CDP-040 server native ingest operation id drifted");
  Require(Contains(payload.row_packet, "operation_id=dml.execute_native_bulk_ingest"),
          "CDP-040 server native ingest did not return native operation packet");
  Require(Contains(payload.row_packet, "native_bulk_ingest") &&
              Contains(payload.row_packet, "engine_internal_api"),
          "CDP-040 server native ingest evidence missing from public ABI route");
  Require(SelectCount(fixture, context) == 2,
          "CDP-040 server public ABI native ingest rows not visible in transaction");

  const auto disabled = server::HandleExecuteSblr(
      &registry,
      engine_state,
      ExecuteFrame(session_uuid,
                   NativeServerEnvelope(fixture, Rows("server-disabled", 1), false)));
  Require(!disabled.accepted, "CDP-040 server disabled native ingest was accepted");
  Require(!disabled.diagnostics.empty(), "CDP-040 server disabled native ingest lacked diagnostics");
  Require(disabled.diagnostics.front().code == "DML.NATIVE_BULK_INGEST.DISABLED",
          "CDP-040 server disabled diagnostic drifted");
  Rollback(context);
}

void TestRollbackInvisibilityAndCommittedReopenVisibility() {
  auto rollback_fixture = MakeFixture("rollback", 3000);
  auto rollback_context = Begin(rollback_fixture, "cdp040-rollback-writer");
  const auto rolled = api::EngineExecuteNativeBulkIngest(
      NativeRequest(rollback_fixture, rollback_context, Rows("rollback", 4)));
  RequireOk(rolled, "CDP-040 rollback native ingest failed before rollback");
  Rollback(rollback_context);

  auto rollback_reader = Begin(rollback_fixture, "cdp040-rollback-reader");
  Require(SelectCount(rollback_fixture, rollback_reader) == 0,
          "CDP-040 rolled-back native ingest rows became visible");
  Rollback(rollback_reader);

  auto commit_fixture = MakeFixture("commit_reopen", 4000);
  auto commit_context = Begin(commit_fixture, "cdp040-commit-writer");
  const auto committed = api::EngineExecuteNativeBulkIngest(
      NativeRequest(commit_fixture, commit_context, Rows("commit", 5)));
  RequireOk(committed, "CDP-040 committed native ingest failed");
  Commit(commit_context);

  const auto opened = db::OpenDatabaseFile({commit_fixture.database_path.string(), false, false, false});
  Require(opened.ok(), "CDP-040 committed database did not reopen");

  auto reopen_reader = Begin(commit_fixture, "cdp040-reopen-reader");
  Require(SelectCount(commit_fixture, reopen_reader) == 5,
          "CDP-040 committed native ingest rows were not visible after reopen");
  Rollback(reopen_reader);
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(static_cast<bool>(in), "CDP-040 could not open recovery evidence");
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteTextFile(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(static_cast<bool>(out), "CDP-040 could not open dirty manifest");
  out << text;
  out.flush();
  Require(static_cast<bool>(out), "CDP-040 could not write dirty manifest");
}

void WriteRecoverableDirtyManifest(const Fixture& fixture) {
  db::DirtyObjectManifest manifest;
  manifest.checkpoint_generation = 1;
  manifest.completed = true;
  manifest.classification_only = true;

  db::DirtyObjectManifestEntry entry;
  entry.kind = db::DirtyObjectKind::catalog_page;
  entry.object_uuid = NewTypedUuid(platform::UuidKind::object, fixture.salt + 500);
  entry.page_number = db::kCatalogPageNumber;
  entry.page_generation = 1;
  entry.object_checksum = 177;
  entry.local_transaction_id = 2;
  entry.operation_envelope_checksum = 277;
  entry.transaction_evidence_checksum = 377;
  entry.dirty = true;
  entry.authoritative = true;
  manifest.entries.push_back(entry);

  const auto built = db::BuildDirtyObjectManifest(manifest);
  if (!built.ok()) {
    std::cerr << built.diagnostic.diagnostic_code << '\n';
  }
  Require(built.ok(), "CDP-040 recoverable dirty manifest did not build");
  WriteTextFile(fixture.database_path.string() + ".dirty.manifest", built.serialized);
}

void TestReopenRecoveryEvidence() {
  auto fixture = MakeFixture("recovery", 5000);
  const auto first_open = db::OpenDatabaseFile({fixture.database_path.string(), false, false, false});
  Require(first_open.ok(), "CDP-040 recovery fixture first open failed");

  WriteRecoverableDirtyManifest(fixture);
  const auto recovered = db::OpenDatabaseFile({fixture.database_path.string(), false, false, false});
  Require(recovered.ok(), "CDP-040 dirty manifest recovery open failed");
  Require(recovered.state.startup_recovery_classification == "repaired_recovery",
          "CDP-040 dirty manifest recovery classification drifted");

  const auto evidence_path = fixture.database_path.string() + ".recovery.evidence";
  Require(std::filesystem::exists(evidence_path),
          "CDP-040 recovery evidence was not persisted");
  const auto evidence = ReadTextFile(evidence_path);
  Require(evidence.find("SBRECOVERY1") != std::string::npos,
          "CDP-040 recovery evidence marker missing");
  Require(evidence.find("WAL") == std::string::npos &&
              evidence.find("wal") == std::string::npos,
          "CDP-040 recovery evidence used WAL authority language");

  const auto second_open = db::OpenDatabaseFile({fixture.database_path.string(), false, false, false});
  Require(second_open.ok(), "CDP-040 second recovery open failed");
  Require(ReadTextFile(evidence_path) == evidence,
          "CDP-040 recovery evidence was not idempotent");
}

void TestSblrRegistryEntry() {
  const auto* entry = sblr::LookupSblrOperation("dml.execute_native_bulk_ingest");
  Require(entry != nullptr, "CDP-040 native ingest SBLR registry entry missing");
  Require(entry->opcode == "SBLR_DML_EXECUTE_NATIVE_BULK_INGEST",
          "CDP-040 native ingest opcode drifted");
  Require(entry->category == sblr::SblrOpcodeCategory::data_mutation,
          "CDP-040 native ingest is not a data mutation operation");
  Require(entry->transaction_effect == sblr::SblrOpcodeTransactionEffect::local_write,
          "CDP-040 native ingest transaction effect drifted");
  Require(entry->requires_transaction_context,
          "CDP-040 native ingest does not require transaction context");
  Require(!entry->requires_cluster_authority && !entry->cluster_private,
          "CDP-040 native ingest incorrectly entered cluster-private scope");

  auto envelope = NativeEnvelope();
  const auto validated = sblr::ValidateSblrOpcodeForEnvelope(envelope);
  Require(validated.ok && validated.entry == entry,
          "CDP-040 native ingest SBLR opcode validation failed");

  envelope.requires_transaction_context = false;
  const auto refused = sblr::ValidateSblrOpcodeForEnvelope(envelope);
  Require(!refused.ok &&
              refused.diagnostic_id == "SB_DIAG_SBLR_TRANSACTION_CONTEXT_REQUIRED",
          "CDP-040 native ingest transaction-context refusal drifted");

  const auto encoded = sblr::EncodeSblrEnvelope(NativeRoundTripEnvelope());
  const auto decoded = sblr::DecodeSblrEnvelope(encoded);
  Require(decoded.ok, "CDP-040 native ingest encoded SBLR envelope failed decode");
  Require(decoded.envelope.operation_id == "dml.execute_native_bulk_ingest" &&
              decoded.envelope.opcode == "SBLR_DML_EXECUTE_NATIVE_BULK_INGEST",
          "CDP-040 native ingest round trip changed operation identity");

  sblr::SblrToSbsqlOptions options;
  options.source_preserving = true;
  const auto rendered = sblr::RenderSblrEnvelopeToSbsql(decoded.envelope, options);
  Require(!rendered.ok && !rendered.diagnostics.empty(),
          "CDP-040 native ingest SBLR-to-SBsql conversion should refuse without SQL text");
  Require(rendered.diagnostics.front().code ==
              "SB_SBLR_TO_SBSQL_NO_SOURCE_PRESERVING_RENDER_CONTRACT",
          "CDP-040 native ingest SBLR-to-SBsql refusal diagnostic drifted");
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  TestSblrRegistryEntry();
  TestApiAndSblrAcceptedRoutes();
  TestDisabledAndInvalidRefusals();
  TestServerPublicAbiRoute();
  TestRollbackInvisibilityAndCommittedReopenVisibility();
  TestReopenRecoveryEvidence();
  return EXIT_SUCCESS;
}
