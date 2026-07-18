// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "agent_workload_resource_quota.hpp"
#include "cache/sblr_template_cache.hpp"
#include "database_lifecycle.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "memory.hpp"
#include "transaction/transaction_api.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_dispatch_server.hpp"
#include "ipc_server.hpp"
#include "session_registry.hpp"
#include "transaction_lock.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace agents = scratchbird::core::agents;
namespace memory = scratchbird::core::memory;
namespace sblr = scratchbird::engine::sblr;
namespace sbps = scratchbird::server::sbps;
namespace tx = scratchbird::transaction::mga;

#ifndef SB_FSP011E_SEED_PACK_ROOT
#define SB_FSP011E_SEED_PACK_ROOT "project/resources/seed-packs/initial-resource-pack"
#endif

constexpr const char* kSchemaUuid = "019e07be-f11e-7000-8000-000000000101";
constexpr const char* kTableUuid = "019e07be-f11e-7000-8000-000000000102";
constexpr const char* kIndexUuid = "019e07be-f11e-7000-8000-000000000103";
constexpr const char* kOverlapTableUuid = "019e07be-f11e-7000-8000-000000000104";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

memory::AllocationPolicy MemoryPolicy() {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "sbsql_concurrent_session_transaction_conformance";
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "sbsql_concurrent_session_transaction_conformance");
  Require(configured.ok(), "FSPE-011E memory fixture configuration failed");
  Require(configured.fixture_mode, "FSPE-011E memory fixture mode was not active");
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_sbsql_concurrent_session_txn.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
  }
  return false;
}

std::uint64_t EvidenceU64(const api::EngineApiResult& result, std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind != kind) { continue; }
    try {
      return static_cast<std::uint64_t>(std::stoull(evidence.evidence_id));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

bool HasDiagnostic(const scratchbird::server::SessionOperationResult& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

bool HasLifecycleDiagnostic(const scratchbird::server::ServerRequestLifecycleResult& result,
                            std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

bool HasApiDiagnostic(const api::EngineApiResult& result,
                      std::string_view code,
                      std::string_view detail = {}) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code != code) { continue; }
    if (detail.empty() || diagnostic.detail.find(detail) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
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

bool ReadString(const std::vector<std::uint8_t>& data, std::size_t* offset, std::string* out) {
  if (*offset + 2 > data.size()) { return false; }
  std::uint64_t length = GetU16(data, *offset);
  *offset += 2;
  if (length == 0xffffu) {
    if (*offset + 8 > data.size()) { return false; }
    length = GetU64(data, *offset);
    *offset += 8;
  }
  if (*offset + length > data.size()) { return false; }
  out->assign(reinterpret_cast<const char*>(data.data() + *offset),
              static_cast<std::size_t>(length));
  *offset += static_cast<std::size_t>(length);
  return true;
}

struct ServerExecuteResultForTest {
  std::string outcome;
  std::uint64_t row_count = 0;
  std::string operation_id;
  std::string row_packet;
  std::string detail;
};

ServerExecuteResultForTest DecodeServerExecuteResult(const std::vector<std::uint8_t>& payload) {
  ServerExecuteResultForTest result;
  std::size_t offset = 0;
  Require(ReadString(payload, &offset, &result.outcome), "server execute outcome malformed");
  Require(offset + 16 + 16 + 8 <= payload.size(), "server execute fixed fields malformed");
  offset += 16;
  offset += 16;
  result.row_count = GetU64(payload, offset);
  offset += 8;
  Require(ReadString(payload, &offset, &result.operation_id), "server execute operation id malformed");
  Require(ReadString(payload, &offset, &result.row_packet), "server execute row packet malformed");
  Require(ReadString(payload, &offset, &result.detail), "server execute detail malformed");
  return result;
}

api::EngineRequestContext BaseContext(const std::filesystem::path& database_path,
                                      std::string session_suffix = "001") {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "fspe011e-concurrency";
  context.database_path = database_path.string();
  context.database_uuid.canonical = "019e07be-f11e-7000-8000-000000000001";
  context.principal_uuid.canonical = "019e07be-f11e-7000-8000-000000000002";
  context.session_uuid.canonical = "019e07be-f11e-7000-8000-000000000" + std::move(session_suffix);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("FSPE-011E");
  context.trace_tags.push_back("concurrent-session-transaction");
  return context;
}

sblr::SblrOperationEnvelope Envelope(std::string operation_id, std::string opcode) {
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id), std::move(opcode), "FSPE-011E");
  envelope.parser_package_uuid = "019e07be-f11e-7000-8000-000000000010";
  envelope.registry_snapshot_uuid = "019e07be-f11e-7000-8000-000000000011";
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  return envelope;
}

std::string ServerAdmissionEnvelope(std::string_view operation_id) {
  std::string out;
  out += "operation_id=";
  out += operation_id;
  out += "\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=FSPE-011E\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=false\n";
  out += "requires_cluster_authority=false\n";
  return out;
}

void RequireServerAdmitted(std::string_view operation_id) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{ServerAdmissionEnvelope(operation_id), false});
  if (!admission.admitted) {
    std::cerr << "server admission rejected " << operation_id << '\n';
    for (const auto& diagnostic : admission.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message_key << '\n';
    }
  }
  Require(admission.admitted, "server SBLR admission rejected operation");
}

sblr::SblrDispatchResult Dispatch(const std::filesystem::path& database_path,
                                  const std::string& operation_id,
                                  const std::string& opcode,
                                  api::EngineRequestContext context,
                                  api::EngineApiRequest request = {},
                                  bool requires_transaction = false,
                                  bool require_api_ok = true) {
  RequireServerAdmitted(operation_id);
  auto envelope = Envelope(operation_id, opcode);
  envelope.requires_transaction_context = requires_transaction;
  request.context = context;
  request.operation_id = operation_id;
  sblr::SblrDispatchRequest dispatch;
  dispatch.context = std::move(context);
  dispatch.envelope = std::move(envelope);
  dispatch.api_request = std::move(request);
  auto result = sblr::DispatchSblrOperation(dispatch);
  if (!result.accepted || !result.envelope_validated || !result.dispatched_to_api ||
      (require_api_ok && !result.api_result.ok)) {
    std::cerr << "dispatch failed for " << operation_id << " path=" << database_path << '\n'
              << sblr::SerializeSblrDispatchResultToJson(result);
  }
  return result;
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineColumnDefinition Column(std::uint32_t ordinal, std::string name, std::string type) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.requested_column_uuid.canonical = "019e07be-f11e-7000-8000-00000000020" + std::to_string(ordinal);
  column.names.push_back({"en", "primary", name, name, true});
  column.descriptor.descriptor_uuid.canonical = "019e07be-f11e-7000-8000-00000000030" + std::to_string(ordinal);
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = std::move(type);
  column.descriptor.encoded_descriptor = "type=" + column.descriptor.canonical_type_name;
  return column;
}

api::EngineColumnDefinition ColumnWithUuidSuffix(std::uint32_t ordinal,
                                                 std::string name,
                                                 std::string type,
                                                 std::string column_suffix,
                                                 std::string descriptor_suffix) {
  auto column = Column(ordinal, std::move(name), std::move(type));
  column.requested_column_uuid.canonical =
      "019e07be-f11e-7000-8000-000000000" + std::move(column_suffix);
  column.descriptor.descriptor_uuid.canonical =
      "019e07be-f11e-7000-8000-000000000" + std::move(descriptor_suffix);
  return column;
}

api::EngineLocalizedName Name(std::string name) {
  return {"en", "primary", name, name, true};
}

api::EngineIndexDefinition BtreeIndex() {
  api::EngineIndexDefinition index;
  index.requested_index_uuid.canonical = kIndexUuid;
  index.names.push_back(Name("fspe011e_table_id_idx"));
  index.index_kind = "btree";
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

api::EngineRequestContext BeginTransaction(const std::filesystem::path& database_path,
                                           std::string session_suffix,
                                           std::string isolation = "read_committed") {
  const std::string session_id = session_suffix;
  auto begin = Dispatch(database_path,
                        "transaction.begin",
                        "SBLR_TRANSACTION_BEGIN",
                        BaseContext(database_path, session_id));
  Require(begin.api_result.local_transaction_id != 0, "transaction begin did not return local transaction id");
  auto context = BaseContext(database_path, session_id);
  context.local_transaction_id = begin.api_result.local_transaction_id;
  context.transaction_uuid = begin.api_result.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      EvidenceU64(begin.api_result, "snapshot_visible_through_local_transaction_id");
  context.transaction_isolation_level = std::move(isolation);
  return context;
}

void Commit(const std::filesystem::path& database_path, const api::EngineRequestContext& context) {
  auto commit = Dispatch(database_path,
                         "transaction.commit",
                         "SBLR_TRANSACTION_COMMIT",
                         context,
                         {},
                         true);
  Require(commit.api_result.ok, "transaction commit failed");
  Require(HasEvidence(commit.api_result, "transaction_state", "committed"), "commit evidence missing");
}

void Rollback(const std::filesystem::path& database_path, const api::EngineRequestContext& context) {
  auto rollback = Dispatch(database_path,
                           "transaction.rollback",
                           "SBLR_TRANSACTION_ROLLBACK",
                           context,
                           {},
                           true);
  Require(rollback.api_result.ok, "transaction rollback failed");
  Require(HasEvidence(rollback.api_result, "transaction_state", "rolled_back"), "rollback evidence missing");
}

std::size_t SelectByIdFromTable(const std::filesystem::path& database_path,
                                const api::EngineRequestContext& context,
                                std::string table_uuid,
                                std::string id) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::move(table_uuid);
  request.target_object.object_kind = "table";
  request.predicate.predicate_kind = "column_equals";
  request.predicate.canonical_predicate_envelope = "id";
  request.predicate.bound_values.push_back(TextValue(std::move(id)));
  request.projection.canonical_projection_envelopes.push_back("id");
  request.projection.canonical_projection_envelopes.push_back("note");
  auto selected = Dispatch(database_path,
                           "dml.select_rows",
                           "SBLR_DML_SELECT_ROWS",
                           context,
                           request,
                           true);
  Require(selected.api_result.ok, "select by id failed");
  return selected.api_result.result_shape.rows.size();
}

std::size_t SelectById(const std::filesystem::path& database_path,
                       const api::EngineRequestContext& context,
                       std::string id) {
  return SelectByIdFromTable(database_path, context, kTableUuid, std::move(id));
}

api::EngineApiResult InsertRowIntoTable(const std::filesystem::path& database_path,
                                        const api::EngineRequestContext& context,
                                        std::string table_uuid,
                                        std::string row_uuid,
                                        std::string id,
                                        std::string note,
                                        bool require_success = true) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::move(table_uuid);
  request.target_object.object_kind = "table";
  request.rows.push_back(Row(std::move(row_uuid), std::move(id), std::move(note)));
  auto inserted = Dispatch(database_path,
                           "dml.insert_rows",
                           "SBLR_DML_INSERT_ROWS",
                           context,
                           request,
                           true,
                           require_success);
  if (require_success) {
    Require(inserted.api_result.ok, "insert row failed");
    Require(inserted.api_result.result_shape.rows.size() == 1, "insert did not return one row");
  }
  return inserted.api_result;
}

api::EngineApiResult InsertRow(const std::filesystem::path& database_path,
                               const api::EngineRequestContext& context,
                               std::string row_uuid,
                               std::string id,
                               std::string note) {
  return InsertRowIntoTable(database_path,
                            context,
                            kTableUuid,
                            std::move(row_uuid),
                            std::move(id),
                            std::move(note));
}

void CreateSchemaTableAndIndex(const std::filesystem::path& database_path) {
  auto ddl_context = BeginTransaction(database_path, "101");

  api::EngineApiRequest schema_request;
  schema_request.target_object.uuid.canonical = kSchemaUuid;
  schema_request.target_object.object_kind = "schema";
  schema_request.localized_names.push_back(Name("fspe011e_schema"));
  auto schema = Dispatch(database_path,
                         "ddl.create_schema",
                         "SBLR_DDL_CREATE_SCHEMA",
                         ddl_context,
                         schema_request,
                         true);
  Require(schema.api_result.primary_object.uuid.canonical == kSchemaUuid, "schema create did not preserve UUID");

  api::EngineApiRequest table_request;
  table_request.target_schema.uuid.canonical = kSchemaUuid;
  table_request.target_schema.object_kind = "schema";
  table_request.target_object.uuid.canonical = kTableUuid;
  table_request.target_object.object_kind = "table";
  table_request.localized_names.push_back(Name("fspe011e_table"));
  table_request.columns.push_back(Column(0, "id", "text"));
  table_request.columns.push_back(Column(1, "note", "text"));
  auto table = Dispatch(database_path,
                        "ddl.create_table",
                        "SBLR_DDL_CREATE_TABLE",
                        ddl_context,
                        table_request,
                        true);
  Require(table.api_result.primary_object.uuid.canonical == kTableUuid, "table create did not preserve UUID");

  api::EngineApiRequest index_request;
  index_request.target_object.uuid.canonical = kTableUuid;
  index_request.target_object.object_kind = "table";
  index_request.indexes.push_back(BtreeIndex());
  auto index = Dispatch(database_path,
                        "ddl.create_index",
                        "SBLR_DDL_CREATE_INDEX",
                        ddl_context,
                        index_request,
                        true);
  Require(index.api_result.primary_object.uuid.canonical == kIndexUuid, "index create did not preserve UUID");

  Commit(database_path, ddl_context);
}

void VerifyTransactionVisibility(const std::filesystem::path& database_path) {
  auto writer = BeginTransaction(database_path, "201");
  const auto writer_insert = InsertRow(database_path,
                                       writer,
                                       "019e07be-f11e-7000-8000-000000000301",
                                       "writer-commit",
                                       "uncommitted-before-commit");
  Require(HasEvidence(writer_insert, "identity_range_reservation"), "insert identity reservation evidence missing");
  Require(HasEvidence(writer_insert, "page_reservation"), "insert page reservation evidence missing");

  auto snapshot_reader = BeginTransaction(database_path, "202", "snapshot");
  Require(SelectById(database_path, snapshot_reader, "writer-commit") == 0,
          "snapshot reader saw another session's uncommitted row");

  Commit(database_path, writer);
  Require(SelectById(database_path, snapshot_reader, "writer-commit") == 0,
          "snapshot reader saw a post-snapshot commit");

  auto read_committed_reader = BeginTransaction(database_path, "203");
  Require(SelectById(database_path, read_committed_reader, "writer-commit") == 1,
          "read-committed reader did not see committed row");
  Commit(database_path, read_committed_reader);

  auto rollback_writer = BeginTransaction(database_path, "204");
  (void)InsertRow(database_path,
                  rollback_writer,
                  "019e07be-f11e-7000-8000-000000000302",
                  "writer-rollback",
                  "must-not-be-visible");
  Rollback(database_path, rollback_writer);

  auto rollback_reader = BeginTransaction(database_path, "205");
  Require(SelectById(database_path, rollback_reader, "writer-rollback") == 0,
          "rolled-back row became visible");
  Commit(database_path, rollback_reader);

  auto savepoint_writer = BeginTransaction(database_path, "206");
  (void)InsertRow(database_path,
                  savepoint_writer,
                  "019e07be-f11e-7000-8000-000000000303",
                  "savepoint-before",
                  "kept");
  api::EngineApiRequest savepoint_request;
  savepoint_request.localized_names.push_back(Name("sp_keep_before"));
  auto savepoint = Dispatch(database_path,
                            "transaction.create_savepoint",
                            "SBLR_TRANSACTION_CREATE_SAVEPOINT",
                            savepoint_writer,
                            savepoint_request,
                            true);
  Require(savepoint.api_result.ok, "create savepoint failed");
  (void)InsertRow(database_path,
                  savepoint_writer,
                  "019e07be-f11e-7000-8000-000000000304",
                  "savepoint-after",
                  "discarded");
  auto rollback_to = Dispatch(database_path,
                              "transaction.rollback_to_savepoint",
                              "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT",
                              savepoint_writer,
                              savepoint_request,
                              true);
  Require(rollback_to.api_result.ok, "rollback to savepoint failed");
  Commit(database_path, savepoint_writer);

  auto savepoint_reader = BeginTransaction(database_path, "207");
  Require(SelectById(database_path, savepoint_reader, "savepoint-before") == 1,
          "pre-savepoint row was not visible after commit");
  Require(SelectById(database_path, savepoint_reader, "savepoint-after") == 0,
          "post-savepoint row remained visible after rollback to savepoint");
  Commit(database_path, savepoint_reader);
}

void VerifyDdlDmlOverlapPolicy(const std::filesystem::path& database_path) {
  auto ddl_context = BeginTransaction(database_path, "208");

  api::EngineApiRequest table_request;
  table_request.target_schema.uuid.canonical = kSchemaUuid;
  table_request.target_schema.object_kind = "schema";
  table_request.target_object.uuid.canonical = kOverlapTableUuid;
  table_request.target_object.object_kind = "table";
  table_request.localized_names.push_back(Name("fspe011e_overlap_table"));
  table_request.columns.push_back(ColumnWithUuidSuffix(0, "id", "text", "211", "311"));
  table_request.columns.push_back(ColumnWithUuidSuffix(1, "note", "text", "212", "312"));
  auto table = Dispatch(database_path,
                        "ddl.create_table",
                        "SBLR_DDL_CREATE_TABLE",
                        ddl_context,
                        table_request,
                        true);
  Require(table.api_result.ok, "overlap DDL create table failed");
  Require(table.api_result.primary_object.uuid.canonical == kOverlapTableUuid,
          "overlap DDL did not preserve table UUID");

  auto overlapping_dml = BeginTransaction(database_path, "209");
  auto refused = InsertRowIntoTable(database_path,
                                   overlapping_dml,
                                   kOverlapTableUuid,
                                   "019e07be-f11e-7000-8000-000000000401",
                                   "overlap-before-commit",
                                   "must-not-see-uncommitted-ddl",
                                   false);
  Require(!refused.ok, "DML inserted into another session's uncommitted DDL object");
  if (!HasApiDiagnostic(refused, "SB_ENGINE_API_INVALID_REQUEST", "target_table_not_visible")) {
    for (const auto& diagnostic : refused.diagnostics) {
      std::cerr << "overlap diagnostic: " << diagnostic.code
                << " detail=" << diagnostic.detail << '\n';
    }
  }
  Require(HasApiDiagnostic(refused, "SB_ENGINE_API_INVALID_REQUEST", "target_table_not_visible"),
          "uncommitted DDL overlap did not return target_table_not_visible diagnostic");
  Rollback(database_path, overlapping_dml);

  Commit(database_path, ddl_context);

  auto admitted_dml = BeginTransaction(database_path, "210");
  auto inserted = InsertRowIntoTable(database_path,
                                    admitted_dml,
                                    kOverlapTableUuid,
                                    "019e07be-f11e-7000-8000-000000000402",
                                    "overlap-after-commit",
                                    "sees-committed-ddl");
  Require(inserted.ok, "DML after DDL commit failed");
  Commit(database_path, admitted_dml);

  auto reader = BeginTransaction(database_path, "211");
  Require(SelectByIdFromTable(database_path, reader, kOverlapTableUuid, "overlap-after-commit") == 1,
          "committed DDL/DML overlap row was not visible to later reader");
  Commit(database_path, reader);
}

void VerifyLockTableConformance() {
  tx::LocalTransactionLockTable locks;
  const auto tx1 = tx::MakeLocalTransactionId(1001);
  const auto tx2 = tx::MakeLocalTransactionId(1002);

  tx::TransactionLockRequest exclusive_a;
  exclusive_a.requester = tx1;
  exclusive_a.resource_key = "relation:" + std::string(kTableUuid);
  exclusive_a.mode = tx::TransactionLockMode::exclusive;
  auto granted = locks.Acquire(exclusive_a);
  Require(granted.ok() && granted.decision == tx::TransactionLockDecision::granted,
          "exclusive lock was not granted");

  tx::TransactionLockRequest blocked_a = exclusive_a;
  blocked_a.requester = tx2;
  blocked_a.mode = tx::TransactionLockMode::shared;
  blocked_a.wait_policy.no_wait = true;
  auto timeout = locks.Acquire(blocked_a);
  Require(timeout.decision == tx::TransactionLockDecision::timeout,
          "no-wait conflicting lock did not time out deterministically");

  tx::TransactionLockRequest exclusive_b;
  exclusive_b.requester = tx2;
  exclusive_b.resource_key = "relation:secondary";
  exclusive_b.mode = tx::TransactionLockMode::exclusive;
  Require(locks.Acquire(exclusive_b).ok(), "second exclusive lock was not granted");

  tx::TransactionLockRequest tx1_waits_on_b = exclusive_b;
  tx1_waits_on_b.requester = tx1;
  tx1_waits_on_b.wait_policy.no_wait = false;
  tx1_waits_on_b.wait_policy.timeout_millis = 1000;
  tx1_waits_on_b.wait_policy.wait_start_unix_epoch_millis = 1000;
  tx1_waits_on_b.wait_policy.now_unix_epoch_millis = 1001;
  auto wait_required = locks.Acquire(tx1_waits_on_b);
  Require(wait_required.decision == tx::TransactionLockDecision::wait_required,
          "lock table did not record bounded wait_required state");

  tx::TransactionLockRequest tx2_waits_on_a = exclusive_a;
  tx2_waits_on_a.requester = tx2;
  tx2_waits_on_a.wait_policy.no_wait = false;
  tx2_waits_on_a.wait_policy.timeout_millis = 1000;
  tx2_waits_on_a.wait_policy.wait_start_unix_epoch_millis = 1000;
  tx2_waits_on_a.wait_policy.now_unix_epoch_millis = 1001;
  auto deadlock = locks.Acquire(tx2_waits_on_a);
  Require(deadlock.decision == tx::TransactionLockDecision::deadlock_detected,
          "lock table did not detect deterministic deadlock cycle");

  Require(locks.ReleaseAll(tx1) != 0, "release all for tx1 released no locks");
  Require(locks.ReleaseAll(tx2) != 0, "release all for tx2 released no locks");
  Require(locks.held_lock_count() == 0 && locks.waiting_lock_count() == 0,
          "lock table did not drain after release_all");
}

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutU8(std::vector<std::uint8_t>* out, std::uint8_t value) {
  out->push_back(value);
}

void PutU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void PutUuid(std::vector<std::uint8_t>* out, const std::array<std::uint8_t, 16>& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
}

void PutString(std::vector<std::uint8_t>* out, std::string_view value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

void PutBytes(std::vector<std::uint8_t>* out,
              const std::vector<std::uint8_t>& value) {
  PutU64(out, static_cast<std::uint64_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

scratchbird::server::HostedEngineState MakeEngineState() {
  scratchbird::server::HostedEngineState state;
  state.engine_context_active = true;
  scratchbird::server::HostedDatabaseSnapshot database;
  database.state = scratchbird::server::HostedDatabaseState::kOpen;
  database.database_open = true;
  database.read_only = false;
  database.write_admission_fenced = false;
  database.database_path = "/tmp/sb_sbsql_concurrent_session_txn.sbdb";
  database.database_uuid = "019e07be-f11e-7000-8000-000000000001";
  state.databases.push_back(database);
  return state;
}

scratchbird::server::HostedEngineState MakeEngineStateForDatabase(
    const std::filesystem::path& database_path) {
  auto state = MakeEngineState();
  state.databases.front().database_path = database_path.string();
  return state;
}

scratchbird::server::ServerSessionRecord MakeSession(std::array<std::uint8_t, 16>* session_uuid) {
  scratchbird::server::ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.connection_uuid = session.session_uuid;
  session.transaction_routing_v2_negotiated = true;
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = "/tmp/sb_sbsql_concurrent_session_txn.sbdb";
  session.database_uuid = "019e07be-f11e-7000-8000-000000000001";
  session.local_transaction_id = 1;
  session.snapshot_visible_through_local_transaction_id = 1;
  session.transaction_uuid =
      scratchbird::server::UuidBytesToText(sbps::MakeUuidV7Bytes());
  session.default_transaction_isolation_level = "read_committed";
  *session_uuid = session.session_uuid;
  return session;
}

scratchbird::server::ServerSessionRecord MakeSessionForContext(
    const std::filesystem::path& database_path,
    const api::EngineRequestContext& context,
    std::array<std::uint8_t, 16>* session_uuid) {
  auto session = MakeSession(session_uuid);
  session.database_path = database_path.string();
  session.database_uuid = context.database_uuid.canonical;
  session.local_transaction_id = context.local_transaction_id;
  session.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  session.transaction_uuid = context.transaction_uuid.canonical;
  session.transaction_timestamp = context.transaction_timestamp;
  session.default_transaction_isolation_level =
      context.transaction_isolation_level.empty() ? "read_committed"
                                                  : context.transaction_isolation_level;
  return session;
}

struct ServerRouteForTest {
  scratchbird::server::ServerSessionRegistry registry;
  scratchbird::server::HostedEngineState engine_state;
  std::array<std::uint8_t, 16> session_uuid{};
};

ServerRouteForTest MakeServerRoute(const std::filesystem::path& database_path,
                                   const api::EngineRequestContext& context) {
  ServerRouteForTest route;
  route.engine_state = MakeEngineStateForDatabase(database_path);
  auto session = MakeSessionForContext(database_path, context, &route.session_uuid);
  route.registry.channel_state = scratchbird::server::ServerChannelState::kReady;
  route.registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(route.session_uuid)] =
      session;
  return route;
}

sbps::Frame PrepareFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kPrepareSblr);
  frame.header.payload_schema_id = 4001;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session_uuid;
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodePrepareSblrPayloadForTest(session_uuid, encoded);
  return frame;
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::array<std::uint8_t, 16>& prepared_uuid,
                         const std::string& encoded,
                         bool cursor_requested = false) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.payload_schema_id = 4003;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session_uuid;
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
      session_uuid, prepared_uuid, encoded, cursor_requested);
  return frame;
}

sbps::Frame ExecuteFrameV2(
    const std::array<std::uint8_t, 16>& session_uuid,
    std::string_view encoded,
    std::uint8_t route,
    const scratchbird::server::ServerTransactionState* transaction = nullptr,
    bool cursor_requested = false) {
  sbps::Frame frame;
  frame.header.message_type =
      static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.payload_schema_id = 4011;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session_uuid;
  frame.header.session_uuid = session_uuid;
  PutUuid(&frame.payload, session_uuid);
  PutUuid(&frame.payload, std::array<std::uint8_t, 16>{});
  PutU8(&frame.payload, cursor_requested ? 1 : 0);
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

sbps::Frame PrepareFrameV2(
    const std::array<std::uint8_t, 16>& session_uuid,
    std::string_view encoded,
    const scratchbird::server::ServerTransactionState& transaction) {
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

sbps::Frame ExecutePreparedFrameV2(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& prepared_uuid,
    const scratchbird::server::ServerTransactionState& transaction) {
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

std::string ServerOperationEnvelope(std::string_view operation_id,
                                    std::string_view opcode,
                                    std::string_view family,
                                    bool requires_transaction) {
  std::string out;
  out += "operation_id=";
  out += operation_id;
  out += "\n";
  out += "opcode=";
  out += opcode;
  out += "\n";
  out += "sblr_operation_family=";
  out += family;
  out += "\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=CDP-032\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += requires_transaction ? "requires_transaction_context=true\n"
                              : "requires_transaction_context=false\n";
  out += "requires_cluster_authority=false\n";
  return out;
}

std::string TransactionEnvelope(std::string_view operation_id,
                                std::string_view opcode,
                                bool pressure_restart = false) {
  auto out = ServerOperationEnvelope(operation_id,
                                    opcode,
                                    "sblr.transaction.control.v3",
                                    true);
  if (pressure_restart) {
    out += "transaction_pressure_restart=true\n";
    out += "transaction_pressure_policy=long_idle_restart\n";
  }
  return out;
}

std::string CopyAutocommitEnvelope(std::string row_uuid,
                                   std::string id,
                                   std::string note,
                                   bool include_rows) {
  auto out = ServerOperationEnvelope("dml.execute_import_rows",
                                    "SBLR_DML_EXECUTE_IMPORT_ROWS",
                                    "sblr.dml.operation.v3",
                                    true);
  out += "target_object_uuid=";
  out += kTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "source_kind=csv_stream\n";
  out += "source_fingerprint=cdp032-autocommit-fixture\n";
  out += "source_position=row:0\n";
  out += "format_family=csv\n";
  out += "encoding=utf8\n";
  out += "line_ending=lf\n";
  out += "delimiter=,\n";
  out += "quote=\"\n";
  out += "escape=\"\n";
  out += "header_policy=absent\n";
  out += "estimated_row_count=1\n";
  out += "duplicate_mode=error\n";
  out += "require_generated_row_uuid=true\n";
  out += "reject_mode=reject_row\n";
  out += "reject_limit_rows=1\n";
  out += "reject_payload_policy=diagnostic_only\n";
  out += "resume_policy=fail_closed\n";
  out += "checkpoint_mode=disabled\n";
  out += "autocommit_emulation=true\n";
  if (include_rows) {
    out += "operand=row_field\t";
    out += row_uuid;
    out += "|id\t";
    out += id;
    out += "\n";
    out += "operand=row_field\t";
    out += row_uuid;
    out += "|note\t";
    out += note;
    out += "\n";
  }
  return out;
}

std::string GeneratedUuidText() {
  return scratchbird::server::UuidBytesToText(sbps::MakeUuidV7Bytes());
}

ServerExecuteResultForTest ExecuteServerRoute(ServerRouteForTest* route,
                                              const std::string& encoded,
                                              bool require_accepted = true) {
  auto execute = scratchbird::server::HandleExecuteSblr(
      &route->registry,
      route->engine_state,
      ExecuteFrame(route->session_uuid, {}, encoded));
  if (require_accepted && !execute.accepted) {
    std::cerr << "server route rejected\n";
    for (const auto& diagnostic : execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(!require_accepted || execute.accepted, "server route unexpectedly rejected");
  return execute.accepted ? DecodeServerExecuteResult(execute.payload)
                          : ServerExecuteResultForTest{};
}

void VerifyCdp032AlwaysActiveServerFinality(const std::filesystem::path& database_path) {
  auto commit_context = BeginTransaction(database_path, "301");
  const auto old_commit_tx = commit_context.local_transaction_id;
  auto commit_route = MakeServerRoute(database_path, commit_context);
  const auto commit_result = ExecuteServerRoute(
      &commit_route,
      TransactionEnvelope("transaction.commit", "SBLR_TRANSACTION_COMMIT"));
  const auto& committed_session = commit_route.registry.sessions_by_uuid[
      scratchbird::server::UuidBytesToText(commit_route.session_uuid)];
  Require(Contains(commit_result.row_packet, "evidence=transaction_state:committed"),
          "server commit did not expose committed finality evidence");
  Require(Contains(commit_result.row_packet, "evidence=always_active_transaction_replacement:"),
          "server commit did not expose replacement transaction evidence");
  Require(committed_session.local_transaction_id != 0 &&
              committed_session.local_transaction_id != old_commit_tx,
          "server commit left session idle or reused finalized transaction");

  auto rollback_context = BeginTransaction(database_path, "302");
  const auto old_rollback_tx = rollback_context.local_transaction_id;
  auto rollback_route = MakeServerRoute(database_path, rollback_context);
  const auto rollback_result = ExecuteServerRoute(
      &rollback_route,
      TransactionEnvelope("transaction.rollback", "SBLR_TRANSACTION_ROLLBACK"));
  const auto& rolled_session = rollback_route.registry.sessions_by_uuid[
      scratchbird::server::UuidBytesToText(rollback_route.session_uuid)];
  Require(Contains(rollback_result.row_packet, "evidence=transaction_state:rolled_back"),
          "server rollback did not expose rolled-back finality evidence");
  Require(Contains(rollback_result.row_packet, "evidence=always_active_transaction_replacement:"),
          "server rollback did not expose replacement transaction evidence");
  Require(rolled_session.local_transaction_id != 0 &&
              rolled_session.local_transaction_id != old_rollback_tx,
          "server rollback left session idle or reused finalized transaction");
}

void VerifyCdp032AutocommitEmulation(const std::filesystem::path& database_path) {
  auto success_context = BeginTransaction(database_path, "303");
  auto success_route = MakeServerRoute(database_path, success_context);
  const auto success_id = std::string("autocommit-success");
  const auto success_result = ExecuteServerRoute(
      &success_route,
      CopyAutocommitEnvelope(GeneratedUuidText(), success_id, "committed-by-autocommit", true));
  const auto& success_session = success_route.registry.sessions_by_uuid[
      scratchbird::server::UuidBytesToText(success_route.session_uuid)];
  Require(Contains(success_result.row_packet, "evidence=autocommit_statement_succeeded:committed"),
          "autocommit success did not commit through engine finality");
  Require(Contains(success_result.row_packet, "evidence=always_active_transaction_replacement:"),
          "autocommit success did not expose replacement transaction evidence");
  Require(success_session.local_transaction_id != 0 &&
              success_session.local_transaction_id != success_context.local_transaction_id,
          "autocommit success left the session idle");

  auto success_reader = BeginTransaction(database_path, "304");
  Require(SelectById(database_path, success_reader, success_id) == 1,
          "autocommit success row was not visible after engine commit");
  Commit(database_path, success_reader);

  auto failure_context = BeginTransaction(database_path, "305");
  auto failure_route = MakeServerRoute(database_path, failure_context);
  const auto failure_execute = scratchbird::server::HandleExecuteSblr(
      &failure_route.registry,
      failure_route.engine_state,
      ExecuteFrame(failure_route.session_uuid,
                   {},
                   CopyAutocommitEnvelope(GeneratedUuidText(),
                                          "autocommit-failure",
                                          "must-not-be-visible",
                                          false)));
  Require(!failure_execute.accepted,
          "autocommit refusal unexpectedly accepted malformed COPY-style insert");
  Require(HasDiagnostic(failure_execute, "SB_ENGINE_API_INVALID_REQUEST") ||
              HasDiagnostic(failure_execute, "PARSER_SERVER_IPC.ENGINE_DISPATCH_FAILED"),
          "autocommit refusal did not expose engine refusal diagnostic");
  const auto& failure_session = failure_route.registry.sessions_by_uuid[
      scratchbird::server::UuidBytesToText(failure_route.session_uuid)];
  Require(failure_session.local_transaction_id != 0 &&
              failure_session.local_transaction_id != failure_context.local_transaction_id,
          "autocommit refusal left the session idle instead of opening replacement");

  auto failure_reader = BeginTransaction(database_path, "306");
  Require(SelectById(database_path, failure_reader, "autocommit-failure") == 0,
          "autocommit refusal leaked partial COPY-style rows");
  Commit(database_path, failure_reader);
}

void VerifyCdp032PressureRestartPolicy(const std::filesystem::path& database_path) {
  auto pressure_context = BeginTransaction(database_path, "307");
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  auto pressure_route = MakeServerRoute(database_path, pressure_context);
  const auto pressure_result = ExecuteServerRoute(
      &pressure_route,
      TransactionEnvelope("transaction.rollback",
                          "SBLR_TRANSACTION_ROLLBACK",
                          true));
  const auto& pressure_session = pressure_route.registry.sessions_by_uuid[
      scratchbird::server::UuidBytesToText(pressure_route.session_uuid)];
  Require(Contains(pressure_result.row_packet,
                   "diagnostic_code=SERVER.TRANSACTION_PRESSURE.RESTART_FORCED"),
          "transaction pressure restart did not emit exact diagnostic code");
  Require(Contains(pressure_result.row_packet,
                   "evidence=transaction_pressure_policy:long_idle_restart"),
          "transaction pressure restart did not emit policy evidence");
  Require(Contains(pressure_result.row_packet, "evidence=parser_finality:false"),
          "transaction pressure restart did not prove parser finality stayed false");
  Require(pressure_session.local_transaction_id != 0 &&
              pressure_session.local_transaction_id != pressure_context.local_transaction_id,
          "transaction pressure restart did not open replacement transaction");
}

void VerifyCdp032BackgroundPressureDoesNotStarveForegroundDml(
    const std::filesystem::path& database_path) {
  agents::WorkloadResourceQuotaController quota;
  agents::WorkloadResourcePoolConfig pool;
  pool.pool_id = "cdp032-background";
  pool.workload_class = agents::WorkloadClass::background;
  pool.limits.hard.worker_slots = 1;
  pool.limits.soft.worker_slots = 1;
  pool.limits.queue_on_soft_limit = false;
  Require(quota.RegisterPool(pool).ok, "CDP-032 background quota pool registration failed");

  agents::WorkloadAdmissionRequest first;
  first.request_uuid = "cdp032-background-first";
  first.pool_id = pool.pool_id;
  first.workload_class = agents::WorkloadClass::background;
  first.source = agents::WorkloadAdmissionSource::engine;
  first.requested.worker_slots = 1;
  Require(quota.Admit(first).reservation_created(),
          "CDP-032 background quota first reservation failed");

  agents::WorkloadAdmissionRequest denied = first;
  denied.request_uuid = "cdp032-background-denied";
  const auto denied_result = quota.Admit(denied);
  Require(!denied_result.status.ok &&
              denied_result.diagnostic.diagnostic_code == "WORKLOAD_RESOURCE.HARD_DENIED",
          "CDP-032 background quota did not throttle excess background work");

  auto foreground = BeginTransaction(database_path, "308");
  (void)InsertRow(database_path,
                  foreground,
                  GeneratedUuidText(),
                  "foreground-under-background-pressure",
                  "foreground-dml-admitted");
  Commit(database_path, foreground);

  auto reader = BeginTransaction(database_path, "309");
  Require(SelectById(database_path, reader, "foreground-under-background-pressure") == 1,
          "foreground DML was starved by background resource pressure");
  Commit(database_path, reader);
}

sbps::Frame FetchFrame(const std::array<std::uint8_t, 16>& session_uuid,
                       const std::array<std::uint8_t, 16>& cursor_uuid) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kFetch);
  frame.header.payload_schema_id = 4005;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session_uuid;
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeFetchPayloadForTest(session_uuid, cursor_uuid, 1);
  return frame;
}

sbps::Frame DisconnectFrame(const std::array<std::uint8_t, 16>& session_uuid,
                            std::string_view reason) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kDisconnectNotice);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session_uuid;
  frame.header.session_uuid = session_uuid;
  PutUuid(&frame.payload, session_uuid);
  PutString(&frame.payload, reason);
  return frame;
}

std::vector<std::uint8_t> AttachPayloadForBindingTest(
    const std::array<std::uint8_t, 16>& connection_uuid,
    const std::array<std::uint8_t, 16>& auth_context_uuid) {
  std::vector<std::uint8_t> payload;
  PutUuid(&payload, connection_uuid);
  PutUuid(&payload, auth_context_uuid);
  PutString(&payload, "default");
  PutString(&payload, "read_write");
  return payload;
}

void VerifyNeutralV2MultiTransactionRouting(
    const std::filesystem::path& database_path) {
  auto initial_context = BeginTransaction(database_path, "310");
  auto route = MakeServerRoute(database_path, initial_context);
  auto& session = route.registry.sessions_by_uuid[
      scratchbird::server::UuidBytesToText(route.session_uuid)];
  const std::uint64_t hidden_default_id = session.local_transaction_id;
  const std::string hidden_default_uuid = session.transaction_uuid;
  auto selected_query = ServerOperationEnvelope("dml.select_rows",
                                                "SBLR_DML_SELECT_ROWS",
                                                "sblr.query.relational.v3",
                                                true);
  selected_query += "target_object_uuid=";
  selected_query += kTableUuid;
  selected_query += "\ntarget_object_kind=table\nlimit=1\n";

  auto begin_envelope = TransactionEnvelope("transaction.begin",
                                            "SBLR_TRANSACTION_BEGIN");
  const auto begin_t1 = scratchbird::server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrameV2(route.session_uuid, begin_envelope, 2));
  Require(begin_t1.accepted && begin_t1.response_schema_id == 4012 &&
              begin_t1.transaction_state.has_value() &&
              begin_t1.transaction_state->selected_present,
          "V2 begin-additional did not publish an engine-issued selector");
  const auto t1 = begin_t1.transaction_state->selected;
  Require(t1.local_transaction_id != hidden_default_id &&
              !t1.transaction_uuid.empty(),
          "V2 begin-additional aliased the hidden default transaction");

  const auto begin_t2 = scratchbird::server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrameV2(route.session_uuid, begin_envelope, 2));
  Require(begin_t2.accepted && begin_t2.transaction_state.has_value() &&
              begin_t2.transaction_state->selected_present,
          "second V2 begin-additional did not publish a selector");
  const auto t2 = begin_t2.transaction_state->selected;
  Require(t2.local_transaction_id != t1.local_transaction_id &&
              t2.transaction_uuid != t1.transaction_uuid,
          "independent V2 begins reused transaction identity");
  Require(session.local_transaction_id == hidden_default_id &&
              session.transaction_uuid == hidden_default_uuid,
          "V2 begin-additional swapped the shared default transaction scalar");

  const auto prepared_t2 = scratchbird::server::HandlePrepareSblr(
      &route.registry,
      route.engine_state,
      PrepareFrameV2(route.session_uuid, selected_query, t2));
  Require(prepared_t2.accepted && prepared_t2.response_schema_id == 4010,
          "V2 prepare on the selected transaction failed");
  const auto prepared_t2_uuid =
      scratchbird::server::DecodePreparedStatementUuidForTest(
          prepared_t2.payload);
  Require(prepared_t2_uuid.has_value(),
          "V2 prepare did not publish a prepared UUID");
  auto prepared_t2_it = route.registry.prepared_by_uuid.find(
      scratchbird::server::UuidBytesToText(*prepared_t2_uuid));
  Require(prepared_t2_it != route.registry.prepared_by_uuid.end() &&
              prepared_t2_it->second.prepared_transaction_routing_v2,
          "V2 prepare did not seal its transaction-routed wire origin");
  const auto prepared_same_selector =
      scratchbird::server::HandleExecuteSblr(
          &route.registry,
          route.engine_state,
          ExecutePreparedFrameV2(route.session_uuid,
                                 *prepared_t2_uuid,
                                 t2));
  Require(prepared_same_selector.accepted,
          "V2 prepared execution rejected the exact prepare selector");
  const auto prepared_other_selector =
      scratchbird::server::HandleExecuteSblr(
          &route.registry,
          route.engine_state,
          ExecutePreparedFrameV2(route.session_uuid,
                                 *prepared_t2_uuid,
                                 t1));
  Require(!prepared_other_selector.accepted &&
              HasDiagnostic(
                  prepared_other_selector,
                  "PARSER_SERVER_IPC.PREPARED_TRANSACTION_SELECTOR_MISMATCH"),
          "V2 prepared execution accepted a different transaction selector");
  prepared_t2_it->second.prepared_transaction_routing_v2 = false;
  const auto v1_execute_tampered_v2_prepare =
      scratchbird::server::HandleExecuteSblr(
          &route.registry,
          route.engine_state,
          ExecuteFrame(route.session_uuid, *prepared_t2_uuid, ""));
  Require(!v1_execute_tampered_v2_prepare.accepted &&
              HasDiagnostic(v1_execute_tampered_v2_prepare,
                            "PARSER_SERVER_IPC.PREPARED_STATEMENT_STALE"),
          "V2 prepared wire-origin bit was not sealed into authority proof");
  prepared_t2_it->second.prepared_transaction_routing_v2 = true;
  const auto v1_execute_v2_prepare = scratchbird::server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrame(route.session_uuid, *prepared_t2_uuid, ""));
  Require(!v1_execute_v2_prepare.accepted &&
              HasDiagnostic(
                  v1_execute_v2_prepare,
                  "PARSER_SERVER_IPC.PREPARED_TRANSACTION_SELECTOR_MISMATCH"),
          "V1 execution accepted a V2 transaction-bound prepare");

  const auto prepared_v1 = scratchbird::server::HandlePrepareSblr(
      &route.registry,
      route.engine_state,
      PrepareFrame(route.session_uuid, selected_query));
  Require(prepared_v1.accepted, "V1 prepare fixture failed");
  const auto prepared_v1_uuid =
      scratchbird::server::DecodePreparedStatementUuidForTest(
          prepared_v1.payload);
  Require(prepared_v1_uuid.has_value(), "V1 prepare UUID missing");
  const auto prepared_v1_it = route.registry.prepared_by_uuid.find(
      scratchbird::server::UuidBytesToText(*prepared_v1_uuid));
  Require(prepared_v1_it != route.registry.prepared_by_uuid.end() &&
              !prepared_v1_it->second.prepared_transaction_routing_v2 &&
              prepared_v1_it->second.prepare_local_transaction_id != 0 &&
              !prepared_v1_it->second.prepare_transaction_uuid.empty(),
          "V1 default prepare was confused with transaction-routed prepare");
  const auto v1_execute_v1_prepare = scratchbird::server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrame(route.session_uuid, *prepared_v1_uuid, ""));
  Require(v1_execute_v1_prepare.accepted,
          "V1 prepared execution rejected its exact default transaction");
  const auto v2_execute_v1_prepare =
      scratchbird::server::HandleExecuteSblr(
          &route.registry,
          route.engine_state,
          ExecutePreparedFrameV2(route.session_uuid,
                                 *prepared_v1_uuid,
                                 t2));
  Require(!v2_execute_v1_prepare.accepted &&
              HasDiagnostic(
                  v2_execute_v1_prepare,
                  "PARSER_SERVER_IPC.PREPARED_TRANSACTION_SELECTOR_MISMATCH"),
          "V2 execution accepted a V1 default-bound prepare");

  const auto selected_t1_cursor = scratchbird::server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrameV2(route.session_uuid,
                     selected_query,
                     1,
                     &t1,
                     true));
  Require(selected_t1_cursor.accepted &&
              selected_t1_cursor.transaction_state.has_value() &&
              selected_t1_cursor.transaction_state->selected_present &&
              selected_t1_cursor.transaction_state->selected.local_transaction_id ==
                  t1.local_transaction_id,
          "V2 selected execution did not retain the selected transaction");
  const auto cursor_uuid = scratchbird::server::DecodeCursorUuidForTest(
      selected_t1_cursor.payload);
  Require(cursor_uuid.has_value(),
          "V2 selected cursor execution did not publish a cursor UUID");

  const auto selected_t2 = scratchbird::server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrameV2(route.session_uuid,
                     selected_query,
                     1,
                     &t2));
  Require(selected_t2.accepted,
          "V2 execution through the second transaction failed");
  Require(session.local_transaction_id == hidden_default_id &&
              session.transaction_uuid == hidden_default_uuid,
          "V2 selected execution swapped the shared default transaction scalar");

  const auto special_route = scratchbird::server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrameV2(route.session_uuid,
                     ServerOperationEnvelope("language.session.show",
                                             "SBLR_LANGUAGE_SESSION_SHOW",
                                             "sblr.language.resource_control.v3",
                                             false),
                     1,
                     &t2));
  Require(!special_route.accepted &&
              HasDiagnostic(special_route,
                            "PARSER_SERVER_IPC.V2_ROUTE_UNSUPPORTED"),
          "V2 selected execution entered an unproven server-special route");

  const std::size_t transaction_count_before_autocommit =
      session.transactions_by_local_id.size();
  auto selected_autocommit = selected_query;
  selected_autocommit += "autocommit_emulation=true\n";
  const auto autocommit = scratchbird::server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrameV2(route.session_uuid,
                     selected_autocommit,
                     1,
                     &t2));
  Require(!autocommit.accepted &&
              HasDiagnostic(autocommit,
                            "PARSER_SERVER_IPC.V2_AUTOCOMMIT_UNSUPPORTED") &&
              session.transactions_by_local_id.size() ==
                  transaction_count_before_autocommit,
          "V2 autocommit did not fail closed before selected-transaction finality");

  const auto fetch_after_other_selection = scratchbird::server::HandleFetch(
      &route.registry, FetchFrame(route.session_uuid, *cursor_uuid));
  Require(fetch_after_other_selection.accepted,
          "cursor fetch inferred the current/default transaction instead of its immutable owner");

  auto& live_t1 =
      session.transactions_by_local_id.at(t1.local_transaction_id);
  const auto original_t1_snapshot =
      live_t1.snapshot_visible_through_local_transaction_id;
  live_t1.snapshot_visible_through_local_transaction_id++;
  const auto fetch_with_snapshot_alias = scratchbird::server::HandleFetch(
      &route.registry, FetchFrame(route.session_uuid, *cursor_uuid));
  Require(!fetch_with_snapshot_alias.accepted &&
              HasDiagnostic(fetch_with_snapshot_alias,
                            "PARSER_SERVER_IPC.CURSOR_TRANSACTION_RETIRED"),
          "cursor fetch did not validate the immutable owner snapshot");
  live_t1.snapshot_visible_through_local_transaction_id =
      original_t1_snapshot;

  auto bad_selector = t1;
  bad_selector.transaction_uuid =
      "019e07be-f11e-7000-8000-00000000bad1";
  const auto mismatched_selector = scratchbird::server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrameV2(route.session_uuid,
                     selected_query,
                     1,
                     &bad_selector));
  Require(!mismatched_selector.accepted &&
              mismatched_selector.response_schema_id == 4012 &&
              mismatched_selector.transaction_state.has_value() &&
              mismatched_selector.transaction_state->finality ==
                  scratchbird::server::ServerTransactionResponseState::Finality::
                      kNotApplicable,
          "id/UUID mismatch did not return a typed V2 rejection");

  std::array<std::uint8_t, 16> other_session_uuid{};
  auto other_session = MakeSession(&other_session_uuid);
  route.registry.sessions_by_uuid[
      scratchbird::server::UuidBytesToText(other_session_uuid)] =
      other_session;
  const auto cross_session = scratchbird::server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrameV2(other_session_uuid,
                     selected_query,
                     1,
                     &t1));
  Require(!cross_session.accepted && cross_session.transaction_state.has_value(),
          "cross-session transaction selector was accepted");

  const auto bad_commit = scratchbird::server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrameV2(route.session_uuid,
                     TransactionEnvelope("transaction.commit",
                                         "SBLR_TRANSACTION_COMMIT"),
                     1,
                     &bad_selector));
  Require(!bad_commit.accepted && bad_commit.transaction_state.has_value() &&
              bad_commit.transaction_state->finality ==
                  scratchbird::server::ServerTransactionResponseState::Finality::
                      kKnownNotApplied,
          "pre-engine commit rejection did not publish known-not-applied finality");

  const auto committed_t1 = scratchbird::server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrameV2(route.session_uuid,
                     TransactionEnvelope("transaction.commit",
                                         "SBLR_TRANSACTION_COMMIT"),
                     1,
                     &t1));
  Require(committed_t1.accepted && committed_t1.transaction_state.has_value() &&
              committed_t1.transaction_state->finality ==
                  scratchbird::server::ServerTransactionResponseState::Finality::
                      kKnownApplied &&
              committed_t1.transaction_state->finalized_present &&
              committed_t1.transaction_state->finalized.local_transaction_id ==
                  t1.local_transaction_id,
          "V2 commit did not publish typed known-applied finality");
  Require(session.local_transaction_id == hidden_default_id &&
              session.transaction_uuid == hidden_default_uuid,
          "V2 finality on a non-default transaction swapped the default scalar");
  const auto fetch_after_owner_finality = scratchbird::server::HandleFetch(
      &route.registry, FetchFrame(route.session_uuid, *cursor_uuid));
  Require(!fetch_after_owner_finality.accepted,
          "cursor remained fetchable after its owner transaction finalized");

  auto policy_begin_envelope = begin_envelope;
  policy_begin_envelope += "transaction_isolation_level=snapshot\n";
  policy_begin_envelope += "transaction_read_only=false\n";
  policy_begin_envelope += "transaction_wait_mode=wait\n";
  policy_begin_envelope += "transaction_lock_timeout_ms=250\n";
  const auto policy_begin = scratchbird::server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrameV2(route.session_uuid, policy_begin_envelope, 2));
  Require(policy_begin.accepted && policy_begin.transaction_state.has_value() &&
              policy_begin.transaction_state->selected_present,
          "neutral V2 begin policy was rejected");
  const auto policy_tx = policy_begin.transaction_state->selected;
  Require(policy_tx.isolation_level == "snapshot" && !policy_tx.read_only &&
              policy_tx.wait_mode == "wait" &&
              policy_tx.lock_timeout_present &&
              policy_tx.lock_timeout_ms == 250,
          "neutral V2 begin did not retain the admitted transaction policy");

  auto retaining_commit = TransactionEnvelope("transaction.commit",
                                               "SBLR_TRANSACTION_COMMIT");
  retaining_commit += "retaining=true\n";
  const auto retained = scratchbird::server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrameV2(route.session_uuid,
                     retaining_commit,
                     1,
                     &policy_tx));
  Require(retained.accepted && retained.transaction_state.has_value() &&
              retained.transaction_state->replacement_present &&
              retained.transaction_state->replacement_reason ==
                  scratchbird::server::ServerTransactionResponseState::
                      ReplacementReason::kRetaining,
          "COMMIT RETAINING did not publish a replacement selector");
  const auto& replacement = retained.transaction_state->replacement;
  Require(replacement.isolation_level == policy_tx.isolation_level &&
              replacement.read_only == policy_tx.read_only &&
              replacement.wait_mode == policy_tx.wait_mode &&
              replacement.lock_timeout_present ==
                  policy_tx.lock_timeout_present &&
              replacement.lock_timeout_ms == policy_tx.lock_timeout_ms,
          "COMMIT RETAINING silently dropped admitted neutral transaction policy");

  const std::size_t transaction_count_before_conflict =
      session.transactions_by_local_id.size();
  auto conflicting_begin = begin_envelope;
  conflicting_begin += "transaction_wait_mode=no_wait\n";
  conflicting_begin += "transaction_lock_timeout_ms=50\n";
  const auto conflict = scratchbird::server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrameV2(route.session_uuid, conflicting_begin, 2));
  Require(!conflict.accepted && conflict.transaction_state.has_value() &&
              conflict.transaction_state->finality ==
                  scratchbird::server::ServerTransactionResponseState::Finality::
                      kNotApplicable &&
              session.transactions_by_local_id.size() ==
                  transaction_count_before_conflict,
          "conflicting neutral wait policy did not fail closed before identity publication");

  std::array<std::uint8_t, 16> zero_prepared{};
  const auto v1_payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
      route.session_uuid,
      zero_prepared,
      scratchbird::server::EncodeShowVersionSblrForTest(),
      false);
  std::vector<std::uint8_t> expected_v1_payload;
  PutUuid(&expected_v1_payload, route.session_uuid);
  PutUuid(&expected_v1_payload, zero_prepared);
  PutU8(&expected_v1_payload, 0);
  PutString(&expected_v1_payload,
            scratchbird::server::EncodeShowVersionSblrForTest());
  Require(v1_payload == expected_v1_payload,
          "V1 execute payload bytes changed while adding V2 routing");

  auto zero_header_prepare = PrepareFrame(
      route.session_uuid, scratchbird::server::EncodeShowVersionSblrForTest());
  zero_header_prepare.header.session_uuid = {};
  const auto zero_header_rejected =
      scratchbird::server::HandlePrepareSblr(
          &route.registry, route.engine_state, zero_header_prepare);
  Require(!zero_header_rejected.accepted &&
              HasDiagnostic(zero_header_rejected,
                            "PARSER_SERVER_IPC.ROUTE_ASSOCIATION_MISMATCH"),
          "V1 prepare accepted a zero header session UUID");

  const auto saved_default = session.default_local_transaction_id;
  session.default_local_transaction_id = 999999;
  const auto stale_default = scratchbird::server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrame(route.session_uuid, {}, selected_query));
  Require(!stale_default.accepted &&
              HasDiagnostic(stale_default,
                            "PARSER_SERVER_IPC.DEFAULT_TRANSACTION_NOT_ACTIVE"),
          "V1 execution fell through a stale default map identity to scalar dispatch");
  session.default_local_transaction_id = saved_default;
}

void VerifyNeutralPhysicalChannelGuards(
    const std::filesystem::path& database_path) {
  Require(scratchbird::server::ParserChannelHelloMayBeAdmittedForTest(
              false, false, false) &&
              scratchbird::server::ParserChannelHelloMayBeAdmittedForTest(
                  true, true, true) &&
              !scratchbird::server::ParserChannelHelloMayBeAdmittedForTest(
                  true, false, true) &&
              !scratchbird::server::ParserChannelHelloMayBeAdmittedForTest(
                  true, true, false) &&
              !scratchbird::server::ParserChannelHelloMayBeAdmittedForTest(
                  true, true, true, true),
          "physical channel hello was not idempotent with immutable capabilities");

  scratchbird::server::ServerSessionRegistry binding_registry;
  auto connection_uuid = sbps::MakeUuidV7Bytes();
  auto auth_context_uuid = sbps::MakeUuidV7Bytes();
  auto admitted_channel = sbps::MakeUuidV7Bytes();
  auto replay_channel = sbps::MakeUuidV7Bytes();
  scratchbird::server::ServerSessionRecord auth_context;
  auth_context.connection_uuid = connection_uuid;
  auth_context.auth_context_uuid = auth_context_uuid;
  auth_context.session_uuid = auth_context_uuid;
  auth_context.server_channel_uuid = admitted_channel;
  binding_registry.auth_contexts_by_uuid[
      scratchbird::server::UuidBytesToText(auth_context_uuid)] = auth_context;
  binding_registry.physical_channel_by_connection_uuid[
      scratchbird::server::UuidBytesToText(connection_uuid)] = replay_channel;
  sbps::Frame forged_attach;
  forged_attach.header.message_type =
      static_cast<std::uint16_t>(sbps::MessageType::kAttachDatabase);
  forged_attach.header.request_uuid = sbps::MakeUuidV7Bytes();
  forged_attach.header.connection_uuid = connection_uuid;
  forged_attach.payload =
      AttachPayloadForBindingTest(connection_uuid, auth_context_uuid);
  const auto replay = scratchbird::server::HandleAttachDatabase(
      &binding_registry, MakeEngineStateForDatabase(database_path), forged_attach);
  Require(!replay.accepted &&
              HasDiagnostic(replay,
                            "PARSER_SERVER_IPC.ROUTE_ASSOCIATION_MISMATCH"),
          "an authentication context was replayed from a second physical channel");

  binding_registry.physical_channel_by_connection_uuid[
      scratchbird::server::UuidBytesToText(connection_uuid)] = admitted_channel;
  std::array<std::uint8_t, 16> sibling_uuid{};
  auto sibling = MakeSession(&sibling_uuid);
  sibling.server_channel_uuid = admitted_channel;
  binding_registry.sessions_by_uuid[
      scratchbird::server::UuidBytesToText(sibling_uuid)] = sibling;
  const auto sibling_attach = scratchbird::server::HandleAttachDatabase(
      &binding_registry, MakeEngineStateForDatabase(database_path), forged_attach);
  Require(!sibling_attach.accepted &&
              HasDiagnostic(
                  sibling_attach,
                  "PARSER_SERVER_IPC.PHYSICAL_CHANNEL_SESSION_LIMIT"),
          "one physical parser channel admitted a sibling attached session");

  auto cleanup_context = BeginTransaction(database_path, "320");
  auto cleanup_route = MakeServerRoute(database_path, cleanup_context);
  auto& cleanup_session = cleanup_route.registry.sessions_by_uuid[
      scratchbird::server::UuidBytesToText(cleanup_route.session_uuid)];
  Require(scratchbird::server::AdoptAndFindExactActiveDefaultTransaction(
              &cleanup_session) != nullptr,
          "channel cleanup fixture did not adopt its exact default transaction");
  const auto cleanup_channel = sbps::MakeUuidV7Bytes();
  cleanup_session.server_channel_uuid = cleanup_channel;
  cleanup_route.registry.physical_channel_by_connection_uuid[
      scratchbird::server::UuidBytesToText(cleanup_session.connection_uuid)] =
      cleanup_channel;
  const auto cleaned =
      scratchbird::server::HandleUnexpectedParserChannelClose(
          &cleanup_route.registry, cleanup_channel);
  Require(cleaned.size() == 1 && cleaned.front().accepted &&
              cleanup_route.registry.sessions_by_uuid.empty() &&
              cleanup_route.registry
                  .physical_channel_by_connection_uuid.empty(),
          "unexpected channel close did not roll back and detach its exact owned session");

  auto quarantined_context = BeginTransaction(database_path, "321");
  auto continuity_context = BeginTransaction(database_path, "322");
  auto continuity_route = MakeServerRoute(database_path, quarantined_context);
  auto& quarantined_session = continuity_route.registry.sessions_by_uuid[
      scratchbird::server::UuidBytesToText(continuity_route.session_uuid)];
  auto* quarantined_transaction =
      scratchbird::server::AdoptAndFindExactActiveDefaultTransaction(
          &quarantined_session);
  Require(quarantined_transaction != nullptr,
          "quarantine fixture did not adopt its exact default transaction");
  quarantined_transaction->lifecycle_state =
      scratchbird::server::ServerTransactionLifecycleState::kFinalityUnknown;
  const auto quarantined_channel = sbps::MakeUuidV7Bytes();
  quarantined_session.server_channel_uuid = quarantined_channel;
  continuity_route.registry.physical_channel_by_connection_uuid[
      scratchbird::server::UuidBytesToText(
          quarantined_session.connection_uuid)] = quarantined_channel;

  std::array<std::uint8_t, 16> continuity_session_uuid{};
  auto continuity_session = MakeSessionForContext(
      database_path, continuity_context, &continuity_session_uuid);
  const auto continuity_channel = sbps::MakeUuidV7Bytes();
  continuity_session.server_channel_uuid = continuity_channel;
  auto* continuity_transaction =
      scratchbird::server::AdoptAndFindExactActiveDefaultTransaction(
          &continuity_session);
  Require(continuity_transaction != nullptr,
          "continuity fixture did not adopt its exact default transaction");
  continuity_route.registry.sessions_by_uuid[
      scratchbird::server::UuidBytesToText(continuity_session_uuid)] =
      continuity_session;
  continuity_route.registry.physical_channel_by_connection_uuid[
      scratchbird::server::UuidBytesToText(
          continuity_session.connection_uuid)] = continuity_channel;
  scratchbird::server::ServerCursorRecord continuity_cursor;
  continuity_cursor.cursor_uuid = sbps::MakeUuidV7Bytes();
  continuity_cursor.session_uuid = continuity_session_uuid;
  continuity_cursor.operation_id = "dml.select_rows";
  continuity_cursor.row_packet = "continuity-row\n";
  continuity_cursor.total_row_count = 1;
  continuity_cursor.owning_local_transaction_id =
      continuity_transaction->local_transaction_id;
  continuity_cursor.owning_snapshot_visible_through_local_transaction_id =
      continuity_transaction->snapshot_visible_through_local_transaction_id;
  continuity_cursor.owning_transaction_uuid =
      continuity_transaction->transaction_uuid;
  continuity_route.registry.cursors_by_uuid[
      scratchbird::server::UuidBytesToText(continuity_cursor.cursor_uuid)] =
      continuity_cursor;

  const auto quarantined =
      scratchbird::server::HandleUnexpectedParserChannelClose(
          &continuity_route.registry, quarantined_channel);
  Require(quarantined.size() == 1 && quarantined.front().accepted &&
              continuity_route.registry.channel_state ==
                  scratchbird::server::ServerChannelState::kReady &&
              continuity_route.registry.sessions_by_uuid
                  .at(scratchbird::server::UuidBytesToText(
                      continuity_route.session_uuid))
                  .detached_recovery_quarantined,
          "unknown channel-close finality was not retained as session-scoped quarantine");
  const auto continuity_fetch = scratchbird::server::HandleFetch(
      &continuity_route.registry,
      FetchFrame(continuity_session_uuid, continuity_cursor.cursor_uuid));
  Require(continuity_fetch.accepted,
          "one channel's recovery quarantine drained another channel's cursor");

  api::EngineRollbackTransactionRequest cleanup_quarantine;
  cleanup_quarantine.context = quarantined_context;
  (void)api::EngineRollbackTransaction(cleanup_quarantine);
  api::EngineRollbackTransactionRequest cleanup_continuity;
  cleanup_continuity.context = continuity_context;
  (void)api::EngineRollbackTransaction(cleanup_continuity);
}

void VerifyNeutralPreEngineTypedRefusals() {
  scratchbird::server::ServerTransactionState selector;
  selector.local_transaction_id = 77;
  selector.transaction_uuid =
      "019e07be-f11e-7000-8000-000000000077";
  const auto session_uuid = sbps::MakeUuidV7Bytes();
  const auto maintenance_commit =
      scratchbird::server::RejectExecuteSblrBeforeEngine(
          ExecuteFrameV2(
              session_uuid,
              TransactionEnvelope("transaction.commit",
                                  "SBLR_TRANSACTION_COMMIT"),
              1,
              &selector),
          "SERVER.MAINTENANCE.SBLR_ADMISSION_FENCED",
          "sblr_admission_fenced");
  Require(!maintenance_commit.accepted &&
              maintenance_commit.response_schema_id == 4012 &&
              (maintenance_commit.frame_flags & sbps::kFlagError) == 0 &&
              maintenance_commit.transaction_state.has_value() &&
              maintenance_commit.transaction_state->finality ==
                  scratchbird::server::ServerTransactionResponseState::
                      Finality::kKnownNotApplied &&
              maintenance_commit.transaction_state->diagnostic_code ==
                  "SERVER.MAINTENANCE.SBLR_ADMISSION_FENCED",
          "pre-engine maintenance commit refusal lost typed V2 finality or diagnostic authority");

  const auto maintenance_prepare =
      scratchbird::server::RejectPrepareSblrBeforeEngine(
          PrepareFrameV2(session_uuid,
                         scratchbird::server::EncodeShowVersionSblrForTest(),
                         selector),
          "SERVER.MAINTENANCE.SBLR_ADMISSION_FENCED",
          "sblr_admission_fenced");
  Require(!maintenance_prepare.accepted &&
              maintenance_prepare.response_schema_id == 4010 &&
              (maintenance_prepare.frame_flags & sbps::kFlagError) == 0,
          "pre-engine maintenance prepare refusal lost the V2 result schema");
}

void VerifyServerSessionConformance() {
  std::array<std::uint8_t, 16> session_a{};
  std::array<std::uint8_t, 16> session_b{};
  scratchbird::server::ServerSessionRegistry registry;
  auto record_a = MakeSession(&session_a);
  auto record_b = MakeSession(&session_b);
  registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session_a)] = record_a;
  registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session_b)] = record_b;
  registry.channel_state = scratchbird::server::ServerChannelState::kReady;
  const auto engine_state = MakeEngineState();

  const auto prepare = scratchbird::server::HandlePrepareSblr(
      &registry, engine_state, PrepareFrame(session_a, scratchbird::server::EncodeShowVersionSblrForTest()));
  Require(prepare.accepted, "server prepare rejected valid SBLR");
  const auto prepared_uuid = scratchbird::server::DecodePreparedStatementUuidForTest(prepare.payload);
  Require(prepared_uuid.has_value(), "prepare did not return prepared statement UUID");

  const auto cross_session_execute = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_b, *prepared_uuid, ""));
  Require(!cross_session_execute.accepted &&
              HasDiagnostic(cross_session_execute, "PARSER_SERVER_IPC.PREPARED_STATEMENT_NOT_FOUND"),
          "session B was able to execute session A prepared statement");

  const auto cursor_execute = scratchbird::server::HandleExecuteSblr(
      &registry,
      engine_state,
      ExecuteFrame(session_a, {}, scratchbird::server::EncodeShowVersionSblrForTest(), true));
  Require(cursor_execute.accepted, "session A cursor execute failed");
  const auto cursor_uuid = scratchbird::server::DecodeCursorUuidForTest(cursor_execute.payload);
  Require(cursor_uuid.has_value(), "cursor execute did not return cursor UUID");

  const auto wrong_session_fetch = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_b, *cursor_uuid));
  Require(!wrong_session_fetch.accepted && HasDiagnostic(wrong_session_fetch, "PARSER_SERVER_IPC.CURSOR_NOT_FOUND"),
          "session B was able to fetch session A cursor");

  auto& session_record = registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session_a)];
  session_record.catalog_generation = 2;
  const auto stale_execute = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_a, *prepared_uuid, ""));
  Require(!stale_execute.accepted &&
              HasDiagnostic(stale_execute, "PARSER_SERVER_IPC.PREPARED_STATEMENT_STALE"),
          "stale prepared statement was not rejected after catalog epoch change");

  session_record.local_transaction_id = 4242;
  session_record.snapshot_visible_through_local_transaction_id = 4241;
  sbps::Frame cancellable_frame;
  cancellable_frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  cancellable_frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  cancellable_frame.header.session_uuid = session_a;
  auto cancellable = scratchbird::server::RegisterServerRequestLifecycle(
      &registry, cancellable_frame, session_record, "execute_sblr", "dml.insert_rows");
  scratchbird::server::ServerCursorRecord cancellable_cursor;
  cancellable_cursor.cursor_uuid = sbps::MakeUuidV7Bytes();
  cancellable_cursor.request_uuid = cancellable.request_uuid;
  cancellable_cursor.finality_token_uuid = cancellable.finality_token_uuid;
  cancellable_cursor.session_uuid = session_a;
  cancellable_cursor.operation_id = "dml.insert_rows";
  cancellable_cursor.finality_state = "active";
  registry.cursors_by_uuid[scratchbird::server::UuidBytesToText(cancellable_cursor.cursor_uuid)] =
      cancellable_cursor;
  scratchbird::server::LinkServerRequestCursor(
      &registry, cancellable.request_uuid, cancellable_cursor.cursor_uuid, false);
  const auto cancelled = scratchbird::server::CancelServerRequestLifecycle(
      &registry,
      scratchbird::server::UuidBytesToText(cancellable.finality_token_uuid),
      session_record,
      true,
      250);
  Require(cancelled.accepted && cancelled.unknown_outcome && cancelled.outcome == "unknown_outcome",
          "request cancellation did not preserve unknown transaction finality");
  Require(HasLifecycleDiagnostic(cancelled, "PARSER_SERVER_IPC.DISCONNECT_OUTCOME_UNKNOWN"),
          "request cancellation did not emit unknown-outcome diagnostic");
  const auto cancelled_record = scratchbird::server::FindServerRequestLifecycle(
      registry, scratchbird::server::UuidBytesToText(cancellable.request_uuid));
  Require(cancelled_record.has_value() &&
              cancelled_record->state == scratchbird::server::ServerRequestLifecycleState::kUnknownOutcome &&
              cancelled_record->detail == "cancel_requested_outcome_unknown_preserved" &&
              cancelled_record->transaction_finality_preserved,
          "request cancellation did not retain MGA finality authority");
  const auto finality_it = registry.finality_by_request_uuid.find(
      scratchbird::server::UuidBytesToText(cancellable.request_uuid));
  Require(finality_it != registry.finality_by_request_uuid.end() &&
              finality_it->second.state == "unknown_outcome",
          "request finality registry did not record unknown_outcome");
  const auto cancelled_cursor_it = registry.cursors_by_uuid.find(
      scratchbird::server::UuidBytesToText(cancellable_cursor.cursor_uuid));
  Require(cancelled_cursor_it != registry.cursors_by_uuid.end() &&
              cancelled_cursor_it->second.closed &&
              cancelled_cursor_it->second.exhausted &&
              cancelled_cursor_it->second.finality_state == "cancelled_unknown_outcome",
          "request cancellation did not close linked cursor as cancelled_unknown_outcome");
  Require(registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session_a)].local_transaction_id == 4242,
          "request cancellation changed session transaction authority");

  const auto unknown_cancel = scratchbird::server::CancelServerRequestLifecycle(
      &registry,
      "019e07be-f11e-7000-8000-000000000999",
      session_record,
      true,
      250);
  Require(unknown_cancel.accepted && unknown_cancel.unknown_outcome &&
              unknown_cancel.outcome == "unknown_finality" &&
              HasLifecycleDiagnostic(unknown_cancel, "SERVER.REQUEST.FINALITY_UNKNOWN"),
          "unknown request finality token did not fail closed as unknown_finality");

  const auto disconnect = scratchbird::server::HandleDisconnectNotice(
      &registry, DisconnectFrame(session_a, "parser_disconnect_notice"));
  Require(disconnect.accepted, "disconnect did not detach session A");
  const auto cursor_it = registry.cursors_by_uuid.find(scratchbird::server::UuidBytesToText(*cursor_uuid));
  Require(cursor_it != registry.cursors_by_uuid.end() &&
              cursor_it->second.closed &&
              cursor_it->second.finality_state == "parser_disconnected",
          "disconnect did not close active cursor with parser_disconnected finality");
  const auto prepared_it = registry.prepared_by_uuid.find(scratchbird::server::UuidBytesToText(*prepared_uuid));
  Require(prepared_it != registry.prepared_by_uuid.end() && prepared_it->second.closed,
          "disconnect did not close prepared SBLR record");
}

void VerifyParserCacheConformance() {
  scratchbird::parser::sbsql::SblrTemplateCache cache(512);
  scratchbird::parser::sbsql::CacheKey key;
  key.shape_hash = 42;
  key.catalog_epoch = 1;
  key.security_policy_epoch = 1;
  key.descriptor_epoch = 1;
  key.udr_epoch = 1;
  key.search_path_hash = "public";
  key.language_profile = "en";
  key.policy_profile = "default_policy";
  key.parser_profile = "sbsql";
  key.result_contract_hash = "default_result";
  cache.Store(key, "sblr.payload.v1");
  Require(cache.Lookup(key).has_value(), "cache lookup missed stored SBLR template");

  auto security_changed = key;
  security_changed.security_policy_epoch = 2;
  Require(!cache.Lookup(security_changed).has_value(),
          "cache lookup ignored security policy epoch change");

  cache.InvalidateCatalogEpoch(2);
  Require(cache.Size() == 0, "catalog epoch invalidation retained stale cache entry");

  std::atomic<int> hits{0};
  std::vector<std::thread> workers;
  for (std::uint64_t worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&cache, &hits, worker]() {
      for (std::uint64_t i = 0; i < 50; ++i) {
        scratchbird::parser::sbsql::CacheKey local;
        local.shape_hash = worker * 1000 + i;
        local.catalog_epoch = 2;
        local.security_policy_epoch = 3;
        local.descriptor_epoch = 4;
        local.udr_epoch = 5;
        local.search_path_hash = "worker_" + std::to_string(worker);
        local.language_profile = "en";
        local.policy_profile = "policy_" + std::to_string(worker);
        local.parser_profile = "sbsql";
        local.result_contract_hash = "result_" + std::to_string(i);
        cache.Store(local, "payload_" + std::to_string(worker) + "_" + std::to_string(i));
        if (cache.Lookup(local).has_value()) { ++hits; }
      }
    });
  }
  for (auto& worker : workers) { worker.join(); }
  Require(hits == 200, "concurrent parser cache store/lookup lost deterministic entries");
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  const auto work = MakeTempDir();
  Require(!work.empty(), "failed to create temp directory");
  const auto database_path = work / "fspe011e.sbdb";

  api::EngineCreateLifecycleRequest create;
  create.context = BaseContext(database_path);
  create.option_envelopes.push_back(std::string("resource_seed_pack_root:") + SB_FSP011E_SEED_PACK_ROOT);
  auto created = api::EngineCreateLifecycle(create);
  Require(created.ok, "lifecycle create database failed");
  Require(std::filesystem::exists(database_path), "lifecycle create did not create database file");

  auto open_result = Dispatch(database_path,
                              "lifecycle.open_database",
                              "SBLR_LIFECYCLE_OPEN_DATABASE",
                              BaseContext(database_path));
  Require(open_result.api_result.ok, "SBLR lifecycle open failed");

  CreateSchemaTableAndIndex(database_path);
  VerifyTransactionVisibility(database_path);
  VerifyDdlDmlOverlapPolicy(database_path);
  VerifyCdp032AlwaysActiveServerFinality(database_path);
  VerifyCdp032AutocommitEmulation(database_path);
  VerifyCdp032PressureRestartPolicy(database_path);
  VerifyCdp032BackgroundPressureDoesNotStarveForegroundDml(database_path);
  VerifyNeutralV2MultiTransactionRouting(database_path);
  VerifyNeutralPhysicalChannelGuards(database_path);
  VerifyNeutralPreEngineTypedRefusals();
  VerifyLockTableConformance();
  VerifyServerSessionConformance();
  VerifyParserCacheConformance();

  std::cout << "sbsql_concurrent_session_transaction_conformance=passed\n";
  std::cout << "cdp_concurrency_transaction_stress_gate=passed\n";
  return EXIT_SUCCESS;
}
