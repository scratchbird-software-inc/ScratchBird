// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "database_lifecycle.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "memory.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_dispatch_server.hpp"
#include "sblr_engine_envelope.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <unistd.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace memory = scratchbird::core::memory;
namespace sblr = scratchbird::engine::sblr;
namespace server = scratchbird::server;
namespace sbps = scratchbird::server::sbps;

#ifndef SB_SBSFC021_SEED_PACK_ROOT
#define SB_SBSFC021_SEED_PACK_ROOT "project/resources/seed-packs/initial-resource-pack"
#endif

constexpr const char* kDatabaseUuid = "019f2100-0000-7000-8000-000000000001";
constexpr const char* kSchemaUuid = "019f2100-0000-7000-8000-000000000101";
constexpr const char* kTableUuid = "019f2100-0000-7000-8000-000000000102";
constexpr const char* kIndexUuid = "019f2100-0000-7000-8000-000000000103";
constexpr const char* kQueryLeftTableUuid = "019f2100-0000-7000-8000-000000000104";
constexpr const char* kQueryRightTableUuid = "019f2100-0000-7000-8000-000000000105";
constexpr const char* kAggregateTableUuid = "019f2100-0000-7000-8000-000000000106";
constexpr const char* kByNameLeftTableUuid = "019f2100-0000-7000-8000-000000000107";
constexpr const char* kByNameRightTableUuid = "019f2100-0000-7000-8000-000000000108";
constexpr const char* kBoolAggregateTableUuid = "019f2100-0000-7000-8000-000000000109";
constexpr const char* kRowA = "019f2100-0000-7000-8000-000000000201";
constexpr const char* kRowB = "019f2100-0000-7000-8000-000000000202";
constexpr const char* kRowC = "019f2100-0000-7000-8000-000000000203";
constexpr const char* kRowD = "019f2100-0000-7000-8000-000000000204";
constexpr const char* kRowE = "019f2100-0000-7000-8000-000000000205";
constexpr const char* kRowF = "019f2100-0000-7000-8000-000000000206";
constexpr const char* kRowG = "019f2100-0000-7000-8000-000000000207";
constexpr const char* kRowH = "019f2100-0000-7000-8000-000000000208";
constexpr const char* kRowI = "019f2100-0000-7000-8000-000000000209";
constexpr const char* kJoinRowA = "019f2100-0000-7000-8000-00000000020a";
constexpr const char* kJoinRowB = "019f2100-0000-7000-8000-00000000020b";
constexpr const char* kJoinRowC = "019f2100-0000-7000-8000-00000000020c";
constexpr const char* kJoinRowD = "019f2100-0000-7000-8000-000000000210";
constexpr const char* kJoinRowE = "019f2100-0000-7000-8000-000000000214";
constexpr const char* kSetRowA = "019f2100-0000-7000-8000-00000000020d";
constexpr const char* kSetRowB = "019f2100-0000-7000-8000-00000000020e";
constexpr const char* kSetRowC = "019f2100-0000-7000-8000-00000000020f";
constexpr const char* kSetRowD = "019f2100-0000-7000-8000-000000000215";
constexpr const char* kAggRowA = "019f2100-0000-7000-8000-000000000211";
constexpr const char* kAggRowB = "019f2100-0000-7000-8000-000000000212";
constexpr const char* kAggRowC = "019f2100-0000-7000-8000-000000000213";
constexpr const char* kBoolAggRowA = "019f2100-0000-7000-8000-00000000021a";
constexpr const char* kBoolAggRowB = "019f2100-0000-7000-8000-00000000021b";
constexpr const char* kBoolAggRowC = "019f2100-0000-7000-8000-00000000021c";
constexpr const char* kBoolAggRowD = "019f2100-0000-7000-8000-00000000021d";
constexpr const char* kBoolAggRowE = "019f2100-0000-7000-8000-00000000021e";
constexpr const char* kInsertSourceRow = "019f2100-0000-7000-8000-00000000021f";
constexpr const char* kByNameLeftRowA = "019f2100-0000-7000-8000-000000000216";
constexpr const char* kByNameLeftRowB = "019f2100-0000-7000-8000-000000000217";
constexpr const char* kByNameRightRowA = "019f2100-0000-7000-8000-000000000218";
constexpr const char* kByNameRightRowB = "019f2100-0000-7000-8000-000000000219";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "sbsql_dml_mga_row_result_conformance";
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
      MemoryPolicy(), "sbsql_dml_mga_row_result_conformance");
  Require(configured.ok(), "DML MGA row-result memory fixture configuration failed");
  Require(configured.fixture_mode,
          "DML MGA row-result memory fixture mode was not active");
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_sbsfc021_dml_mga.XXXXXX";
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

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
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

std::string FieldValue(const api::EngineApiResult& result,
                       std::string_view field,
                       std::size_t row_index = 0) {
  if (row_index >= result.result_shape.rows.size()) { return {}; }
  for (const auto& [name, value] : result.result_shape.rows[row_index].fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
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
  context.request_id = "sbsfc021-dml-mga-row-result";
  context.database_path = database_path.string();
  context.database_uuid.canonical = kDatabaseUuid;
  context.principal_uuid.canonical = "019f2100-0000-7000-8000-000000000002";
  context.session_uuid.canonical = "019f2100-0000-7000-8000-000000000" + std::move(session_suffix);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("SBSFC-021");
  context.trace_tags.push_back("dml-mga-row-result");
  return context;
}

bool RequiresTransaction(std::string_view operation_id) {
  return operation_id.starts_with("dml.") ||
         operation_id.starts_with("ddl.") ||
         operation_id == "query.plan_operation" ||
         operation_id.starts_with("transaction.commit") ||
         operation_id.starts_with("transaction.rollback");
}

std::string OperationFamily(std::string_view operation_id) {
  if (operation_id == "query.plan_operation") { return "sblr.query.relational.v3"; }
  if (operation_id == "dml.select_rows") { return "sblr.query.relational.v3"; }
  if (operation_id.starts_with("dml.")) { return "sblr.dml.operation.v3"; }
  if (operation_id.starts_with("ddl.")) { return "sblr.catalog.mutation.v3"; }
  if (operation_id.starts_with("transaction.")) { return "sblr.transaction.control.v3"; }
  if (operation_id.starts_with("lifecycle.")) { return "sblr.management.runtime_operation.v3"; }
  return "sblr.engine.api.v3";
}

std::string AdmissionEnvelope(std::string_view operation_id, std::string_view opcode) {
  std::string out;
  out += "operation_id=";
  out += operation_id;
  out += "\n";
  out += "opcode=";
  out += opcode;
  out += "\n";
  out += "sblr_operation_family=";
  out += OperationFamily(operation_id);
  out += "\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=SBSFC-021\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += RequiresTransaction(operation_id) ? "requires_transaction_context=true\n"
                                          : "requires_transaction_context=false\n";
  out += "requires_cluster_authority=false\n";
  return out;
}

void RequireServerAdmitted(std::string_view operation_id, std::string_view opcode) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{AdmissionEnvelope(operation_id, opcode), false});
  if (!admission.admitted) {
    std::cerr << "server admission rejected " << operation_id << '\n';
    for (const auto& diagnostic : admission.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    }
  }
  Require(admission.admitted, "server SBLR admission rejected DML row-result fixture");
}

sblr::SblrOperationEnvelope Envelope(std::string operation_id, std::string opcode) {
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id), std::move(opcode), "SBSFC-021");
  envelope.parser_package_uuid = "019f2100-0000-7000-8000-000000000010";
  envelope.registry_snapshot_uuid = "019f2100-0000-7000-8000-000000000011";
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  return envelope;
}

sblr::SblrDispatchResult Dispatch(const std::filesystem::path& database_path,
                                  const std::string& operation_id,
                                  const std::string& opcode,
                                  api::EngineRequestContext context,
                                  api::EngineApiRequest request = {},
                                  bool requires_transaction = false) {
  RequireServerAdmitted(operation_id, opcode);
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
      !result.api_result.ok) {
    std::cerr << "dispatch failed for " << operation_id << " path=" << database_path << '\n'
              << sblr::SerializeSblrDispatchResultToJson(result);
  }
  return result;
}

api::EngineTypedValue TextValue(std::string value, bool is_null = false) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.descriptor.encoded_descriptor = "type=text";
  typed.encoded_value = std::move(value);
  typed.is_null = is_null;
  return typed;
}

api::EngineTypedValue BoolTextValue(std::string value, bool is_null = false) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "boolean";
  typed.descriptor.encoded_descriptor = "type=boolean";
  typed.encoded_value = std::move(value);
  typed.is_null = is_null;
  return typed;
}

api::EngineLocalizedName Name(std::string name) {
  return {"en", "primary", name, name, true};
}

api::EngineIndexDefinition UniqueIdIndex() {
  api::EngineIndexDefinition index;
  index.requested_index_uuid.canonical = kIndexUuid;
  index.names.push_back(Name("sbsfc021_table_id_unique"));
  index.index_kind = "btree";
  index.key_envelopes.push_back("unique");
  index.key_envelopes.push_back("id");
  return index;
}

api::EngineColumnDefinition Column(std::uint32_t ordinal, std::string name) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.requested_column_uuid.canonical =
      "019f2100-0000-7000-8000-00000000030" + std::to_string(ordinal);
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_uuid.canonical =
      "019f2100-0000-7000-8000-00000000040" + std::to_string(ordinal);
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "text";
  column.descriptor.encoded_descriptor = "type=text";
  return column;
}

api::EngineColumnDefinition JoinColumn(std::uint32_t ordinal, std::string name) {
  api::EngineColumnDefinition column = Column(ordinal, std::move(name));
  column.requested_column_uuid.canonical =
      "019f2100-0000-7000-8000-00000000050" + std::to_string(ordinal);
  column.descriptor.descriptor_uuid.canonical =
      "019f2100-0000-7000-8000-00000000060" + std::to_string(ordinal);
  return column;
}

api::EngineColumnDefinition QueryPlanInt64Column(std::uint32_t ordinal,
                                                 std::string name,
                                                 std::string column_uuid,
                                                 std::string descriptor_uuid) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.requested_column_uuid.canonical = std::move(column_uuid);
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_uuid.canonical = std::move(descriptor_uuid);
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "int64";
  column.descriptor.encoded_descriptor = "type=int64";
  return column;
}

api::EngineColumnDefinition QueryPlanBoolColumn(std::uint32_t ordinal,
                                                std::string name,
                                                std::string column_uuid,
                                                std::string descriptor_uuid) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.requested_column_uuid.canonical = std::move(column_uuid);
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_uuid.canonical = std::move(descriptor_uuid);
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "boolean";
  column.descriptor.encoded_descriptor = "type=boolean";
  return column;
}

api::EngineRowValue Row(std::string row_uuid, std::string id, std::string note) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

api::EngineRowValue IdOnlyRow(std::string row_uuid, std::string id) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"id", TextValue(std::move(id))});
  return row;
}

api::EngineRowValue AggregateRow(std::string row_uuid,
                                 std::string id,
                                 std::string dept,
                                 std::string cost) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"dept", TextValue(std::move(dept))});
  row.fields.push_back({"cost", TextValue(std::move(cost))});
  return row;
}

api::EngineRowValue BoolAggregateRow(std::string row_uuid,
                                     std::string id,
                                     std::string dept,
                                     std::string flag,
                                     bool flag_is_null = false) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"dept", TextValue(std::move(dept))});
  row.fields.push_back({"flag", BoolTextValue(std::move(flag), flag_is_null)});
  return row;
}

api::EngineRowValue Int64FieldsRow(
    std::string row_uuid,
    std::initializer_list<std::pair<std::string_view, std::string_view>> fields) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  for (const auto& [name, value] : fields) {
    row.fields.push_back({std::string(name), TextValue(std::string(value))});
  }
  return row;
}

api::EngineApiResult SelectById(const std::filesystem::path& database_path,
                                const api::EngineRequestContext& context,
                                std::string id);
api::EngineApiResult InsertRowIntoTable(const std::filesystem::path& database_path,
                                        const api::EngineRequestContext& context,
                                        std::string table_uuid,
                                        std::string row_uuid,
                                        std::string id,
                                        std::string note);
api::EngineApiResult InsertIdOnlyRowIntoTable(const std::filesystem::path& database_path,
                                              const api::EngineRequestContext& context,
                                              std::string table_uuid,
                                              std::string row_uuid,
                                              std::string id);
api::EngineApiResult InsertAggregateRowIntoTable(const std::filesystem::path& database_path,
                                                 const api::EngineRequestContext& context,
                                                 std::string table_uuid,
                                                 std::string row_uuid,
                                                 std::string id,
                                                 std::string dept,
                                                 std::string cost);
api::EngineApiResult InsertBoolAggregateRowIntoTable(const std::filesystem::path& database_path,
                                                     const api::EngineRequestContext& context,
                                                     std::string table_uuid,
                                                     std::string row_uuid,
                                                     std::string id,
                                                     std::string dept,
                                                     std::string flag,
                                                     bool flag_is_null = false);
api::EngineApiResult InsertInt64FieldsRowIntoTable(
    const std::filesystem::path& database_path,
    const api::EngineRequestContext& context,
    std::string table_uuid,
    std::string row_uuid,
    std::initializer_list<std::pair<std::string_view, std::string_view>> fields);

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = server::EncodeExecuteSblrPayloadForTest(session_uuid, {}, encoded, false);
  return frame;
}

struct ServerRouteForTest {
  server::ServerSessionRegistry registry;
  server::HostedEngineState engine_state;
  std::array<std::uint8_t, 16> session_uuid{};
};

ServerRouteForTest MakeServerRoute(const std::filesystem::path& database_path,
                                   const api::EngineRequestContext& context,
                                   std::optional<std::uint64_t> local_transaction_override = std::nullopt) {
  ServerRouteForTest route;
  server::ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = database_path.string();
  session.database_uuid = kDatabaseUuid;
  session.local_transaction_id = local_transaction_override.value_or(context.local_transaction_id);
  session.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  route.session_uuid = session.session_uuid;
  route.registry.sessions_by_uuid[server::UuidBytesToText(session.session_uuid)] = session;

  route.engine_state.engine_context_active = true;
  server::HostedDatabaseSnapshot database;
  database.state = server::HostedDatabaseState::kOpen;
  database.database_created = true;
  database.database_open = true;
  database.write_admission_fenced = false;
  database.database_path = database_path.string();
  database.database_uuid = kDatabaseUuid;
  route.engine_state.databases.push_back(database);
  return route;
}

std::string PublicCopyExecuteEnvelope(bool include_target, bool include_rows) {
  std::string out = AdmissionEnvelope("dml.execute_import_rows", "SBLR_DML_EXECUTE_IMPORT_ROWS");
  if (include_target) {
    out += "target_object_uuid=";
    out += kTableUuid;
    out += "\n";
    out += "target_object_kind=table\n";
  }
  out += "source_kind=csv_stream\n";
  out += "source_fingerprint=sbsfc021-public-copy-fixture\n";
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
  if (include_rows) {
    out += "operand=row_field\t";
    out += kRowF;
    out += "|id\t6\n";
    out += "operand=row_field\t";
    out += kRowF;
    out += "|note\tcopy-public-a\n";
    out += "operand=row_field\t";
    out += kRowG;
    out += "|id\t7\n";
    out += "operand=row_field\t";
    out += kRowG;
    out += "|note\tcopy-public-b\n";
  }
  return out;
}

std::string PublicOrderedSelectEnvelope() {
  std::string out = AdmissionEnvelope("dml.select_rows", "SBLR_DML_SELECT_ROWS");
  out += "target_object_uuid=";
  out += kTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "order_by=id\n";
  out += "order_direction=desc\n";
  out += "limit=2\n";
  out += "offset=1\n";
  return out;
}

std::string PublicFetchBoundedSelectEnvelope() {
  std::string out = AdmissionEnvelope("dml.select_rows", "SBLR_DML_SELECT_ROWS");
  out += "target_object_uuid=";
  out += kTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "limit=2\n";
  return out;
}

std::string PublicTopBoundedSelectEnvelope() {
  std::string out = AdmissionEnvelope("dml.select_rows", "SBLR_DML_SELECT_ROWS");
  out += "target_object_uuid=";
  out += kTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "limit=2\n";
  out += "bounded_top_clause=true\n";
  return out;
}

std::string PublicTableJoinEnvelope() {
  std::string out = AdmissionEnvelope("query.plan_operation", "SBLR_QUERY_PLAN_OPERATION");
  out += "query_envelope_kind=table_inner_join\n";
  out += "target_object_uuid=";
  out += kQueryLeftTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "related_object_0_uuid=";
  out += kQueryRightTableUuid;
  out += "\n";
  out += "related_object_0_kind=table\n";
  out += "join_algorithm=hash\n";
  out += "left_key_field=id\n";
  out += "right_key_field=id\n";
  out += "left_key_column=0\n";
  out += "right_key_column=0\n";
  return out;
}

std::string PublicTableSetOperationEnvelope(std::string_view operation) {
  std::string out = AdmissionEnvelope("query.plan_operation", "SBLR_QUERY_PLAN_OPERATION");
  out += "query_envelope_kind=table_set_operation\n";
  out += "target_object_uuid=";
  out += kQueryLeftTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "related_object_0_uuid=";
  out += kQueryRightTableUuid;
  out += "\n";
  out += "related_object_0_kind=table\n";
  out += "set_operation=";
  out += operation;
  out += "\n";
  return out;
}

std::string PublicTableSetByNameOperationEnvelope(std::string_view operation) {
  std::string out = AdmissionEnvelope("query.plan_operation", "SBLR_QUERY_PLAN_OPERATION");
  out += "query_envelope_kind=table_set_operation\n";
  out += "target_object_uuid=";
  out += kByNameLeftTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "related_object_0_uuid=";
  out += kByNameRightTableUuid;
  out += "\n";
  out += "related_object_0_kind=table\n";
  out += "set_operation=";
  out += operation;
  out += "\n";
  out += "set_by_name=true\n";
  return out;
}

std::string PublicRowNumberWindowEnvelope() {
  std::string out = AdmissionEnvelope("query.plan_operation", "SBLR_QUERY_PLAN_OPERATION");
  out += "query_envelope_kind=table_row_number_window\n";
  out += "target_object_uuid=";
  out += kQueryLeftTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "order_by=id\n";
  out += "order_column=0\n";
  out += "window_function=row_number\n";
  return out;
}

std::string PublicPartitionCountWindowEnvelope() {
  std::string out = AdmissionEnvelope("query.plan_operation", "SBLR_QUERY_PLAN_OPERATION");
  out += "query_envelope_kind=table_partition_count_window\n";
  out += "query_operation=partition_count_window\n";
  out += "target_object_uuid=";
  out += kAggregateTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "partition_by=dept\n";
  out += "partition_column=1\n";
  out += "window_function=count_star_partition\n";
  out += "aggregate_function=sb.aggregate.count\n";
  return out;
}

std::string PublicNavigationWindowEnvelope(std::string_view function) {
  std::string out = AdmissionEnvelope("query.plan_operation", "SBLR_QUERY_PLAN_OPERATION");
  out += "query_envelope_kind=table_window\n";
  out += "query_operation=window\n";
  out += "target_object_uuid=";
  out += kQueryLeftTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "order_by=id\n";
  out += "order_column=0\n";
  out += "window_function=";
  out += function;
  out += "\n";
  if (function == "ntile" || function == "nth_value") {
    out += "window_n=2\n";
  }
  if (function == "lag" || function == "lead" || function == "first_value" ||
      function == "last_value" || function == "nth_value") {
    out += "window_value_field=id\n";
    out += "window_value_column=0\n";
  }
  return out;
}

std::string PublicGroupByAggregateEnvelope(std::string_view function = "sb.aggregate.sum") {
  std::string out = AdmissionEnvelope("query.plan_operation", "SBLR_QUERY_PLAN_OPERATION");
  out += function == "sb.aggregate.sum" ? "query_envelope_kind=table_group_sum\n"
                                        : "query_envelope_kind=table_group_aggregate\n";
  out += "target_object_uuid=";
  out += kAggregateTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "group_key_field=dept\n";
  out += "aggregate_value_field=cost\n";
  out += "aggregate_function=";
  out += function;
  out += "\n";
  out += "group_key_column=0\n";
  out += "aggregate_value_column=0\n";
  return out;
}

std::string PublicTableCountEnvelope() {
  std::string out = AdmissionEnvelope("query.plan_operation", "SBLR_QUERY_PLAN_OPERATION");
  out += "query_envelope_kind=table_count\n";
  out += "query_operation=count_all\n";
  out += "target_object_uuid=";
  out += kAggregateTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "aggregate_function=sb.aggregate.count\n";
  out += "count_all=true\n";
  return out;
}

std::string PublicHavingAggregateEnvelope() {
  std::string out = PublicGroupByAggregateEnvelope("sb.aggregate.sum");
  out += "having_predicate=aggregate_gt\n";
  out += "having_threshold=20\n";
  out += "having_aggregate_function=sb.aggregate.sum\n";
  out += "having_value_field=cost\n";
  out += "having_value_column=1\n";
  return out;
}

std::string PublicEveryAggregateEnvelope() {
  std::string out = AdmissionEnvelope("query.plan_operation", "SBLR_QUERY_PLAN_OPERATION");
  out += "query_envelope_kind=table_group_aggregate\n";
  out += "target_object_uuid=";
  out += kBoolAggregateTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "group_key_field=dept\n";
  out += "aggregate_value_field=flag\n";
  out += "aggregate_function=sb.aggregate.every\n";
  out += "group_key_column=0\n";
  out += "aggregate_value_column=0\n";
  return out;
}

std::string PublicBooleanAggregateEnvelope(std::string_view function) {
  std::string out = AdmissionEnvelope("query.plan_operation", "SBLR_QUERY_PLAN_OPERATION");
  out += "query_envelope_kind=table_group_aggregate\n";
  out += "target_object_uuid=";
  out += kBoolAggregateTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "group_key_field=dept\n";
  out += "aggregate_value_field=flag\n";
  out += "aggregate_function=";
  out += function;
  out += "\n";
  out += "group_key_column=0\n";
  out += "aggregate_value_column=0\n";
  return out;
}

std::string PublicListAggEnvelope(bool truncate = false) {
  std::string out = AdmissionEnvelope("query.plan_operation", "SBLR_QUERY_PLAN_OPERATION");
  out += "query_envelope_kind=table_group_aggregate\n";
  out += "target_object_uuid=";
  out += kBoolAggregateTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "group_key_field=dept\n";
  out += "aggregate_value_field=flag\n";
  out += "aggregate_function=sb.aggregate.listagg\n";
  out += "group_key_column=0\n";
  out += "aggregate_value_column=0\n";
  out += "order_by=id\n";
  out += "order_column=0\n";
  out += "listagg_separator=|\n";
  if (truncate) {
    out += "listagg_overflow_mode=truncate\n";
    out += "listagg_max_output_bytes=8\n";
    out += "listagg_truncation_indicator=...\n";
    out += "listagg_with_count=false\n";
  }
  return out;
}

std::string PublicStringAggEnvelope() {
  std::string out = AdmissionEnvelope("query.plan_operation", "SBLR_QUERY_PLAN_OPERATION");
  out += "query_envelope_kind=table_group_aggregate\n";
  out += "target_object_uuid=";
  out += kBoolAggregateTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "group_key_field=dept\n";
  out += "aggregate_value_field=flag\n";
  out += "aggregate_function=sb.aggregate.string_agg\n";
  out += "group_key_column=0\n";
  out += "aggregate_value_column=0\n";
  out += "order_by=id\n";
  out += "order_column=0\n";
  out += "listagg_separator=|\n";
  return out;
}

std::string PublicJsonAggEnvelope() {
  std::string out = AdmissionEnvelope("query.plan_operation", "SBLR_QUERY_PLAN_OPERATION");
  out += "query_envelope_kind=table_group_aggregate\n";
  out += "target_object_uuid=";
  out += kBoolAggregateTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "group_key_field=dept\n";
  out += "aggregate_value_field=flag\n";
  out += "aggregate_function=sb.aggregate.json_agg\n";
  out += "group_key_column=0\n";
  out += "aggregate_value_column=0\n";
  out += "order_by=id\n";
  out += "order_column=0\n";
  return out;
}

std::string PublicJsonObjectAggEnvelope() {
  std::string out = AdmissionEnvelope("query.plan_operation", "SBLR_QUERY_PLAN_OPERATION");
  out += "query_envelope_kind=table_group_aggregate\n";
  out += "target_object_uuid=";
  out += kBoolAggregateTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "group_key_field=dept\n";
  out += "aggregate_value_field=id\n";
  out += "aggregate_pair_value_field=flag\n";
  out += "aggregate_function=sb.aggregate.json_object_agg\n";
  out += "group_key_column=0\n";
  out += "aggregate_value_column=0\n";
  out += "aggregate_pair_value_column=2\n";
  out += "order_by=id\n";
  out += "order_column=0\n";
  return out;
}

std::string PublicJsonObjectAggDuplicateEnvelope() {
  std::string out = AdmissionEnvelope("query.plan_operation", "SBLR_QUERY_PLAN_OPERATION");
  out += "query_envelope_kind=table_group_aggregate\n";
  out += "target_object_uuid=";
  out += kBoolAggregateTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "group_key_field=dept\n";
  out += "aggregate_value_field=dept\n";
  out += "aggregate_pair_value_field=id\n";
  out += "aggregate_function=sb.aggregate.json_object_agg\n";
  out += "group_key_column=0\n";
  out += "aggregate_value_column=1\n";
  out += "aggregate_pair_value_column=0\n";
  out += "order_by=id\n";
  out += "order_column=0\n";
  return out;
}

std::string PublicArrayAggEnvelope() {
  std::string out = AdmissionEnvelope("query.plan_operation", "SBLR_QUERY_PLAN_OPERATION");
  out += "query_envelope_kind=table_group_aggregate\n";
  out += "target_object_uuid=";
  out += kBoolAggregateTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "group_key_field=dept\n";
  out += "aggregate_value_field=flag\n";
  out += "aggregate_function=sb.aggregate.array_agg\n";
  out += "group_key_column=0\n";
  out += "aggregate_value_column=0\n";
  out += "order_by=id\n";
  out += "order_column=0\n";
  return out;
}

std::string PublicPairAggregateEnvelope(std::string_view function) {
  std::string out = PublicGroupByAggregateEnvelope(function);
  out += "aggregate_pair_value_field=id\n";
  out += "aggregate_pair_value_column=0\n";
  return out;
}

std::string PublicAggregateOptionEnvelope(std::string_view function,
                                          std::string_view option_name,
                                          std::string_view option_value) {
  std::string out = PublicGroupByAggregateEnvelope(function);
  out += option_name;
  out += "=";
  out += option_value;
  out += "\n";
  return out;
}

std::string PublicMaterializedCteEnvelope() {
  std::string out = AdmissionEnvelope("query.plan_operation", "SBLR_QUERY_PLAN_OPERATION");
  out += "query_envelope_kind=table_materialized_cte\n";
  out += "target_object_uuid=";
  out += kQueryLeftTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  return out;
}

std::string PublicRecursiveCteEnvelope() {
  std::string out = AdmissionEnvelope("query.plan_operation", "SBLR_QUERY_PLAN_OPERATION");
  out += "query_envelope_kind=values_recursive_cte\n";
  out += "recursive_iterations=8\n";
  out += "relation_0_row_count=1\n";
  out += "relation_1_row_count=2\n";
  out += "values_column_count=1\n";
  out += "relation_0_0_0_name=n\n";
  out += "relation_0_0_0_type=bigint\n";
  out += "relation_0_0_0_value=1\n";
  out += "relation_0_0_0_is_null=false\n";
  out += "relation_1_0_0_name=n\n";
  out += "relation_1_0_0_type=bigint\n";
  out += "relation_1_0_0_value=2\n";
  out += "relation_1_0_0_is_null=false\n";
  out += "relation_1_1_0_name=n\n";
  out += "relation_1_1_0_type=bigint\n";
  out += "relation_1_1_0_value=3\n";
  out += "relation_1_1_0_is_null=false\n";
  return out;
}

std::string PublicScalarSubqueryEnvelope() {
  std::string out = AdmissionEnvelope("query.plan_operation", "SBLR_QUERY_PLAN_OPERATION");
  out += "query_envelope_kind=table_scalar_subquery\n";
  out += "target_object_uuid=";
  out += kQueryLeftTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "project_columns=0\n";
  return out;
}

void RequirePublicAbiImportRefuses(const std::filesystem::path& database_path,
                                   const api::EngineRequestContext& context,
                                   std::string_view detail,
                                   bool include_target,
                                   bool include_rows,
                                   std::optional<std::uint64_t> local_transaction_override = std::nullopt) {
  auto route = MakeServerRoute(database_path, context, local_transaction_override);
  auto execute = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrame(route.session_uuid, PublicCopyExecuteEnvelope(include_target, include_rows)));
  if (execute.accepted) {
    const auto decoded = DecodeServerExecuteResult(execute.payload);
    std::cerr << "unexpected public ABI import acceptance: " << decoded.row_packet << '\n';
  }
  Require(!execute.accepted, "server public ABI COPY execution did not fail closed");
  bool matched = false;
  for (const auto& diagnostic : execute.diagnostics) {
    if (Contains(diagnostic.code, detail) ||
        Contains(diagnostic.safe_message, detail)) {
      matched = true;
    }
    for (const auto& field : diagnostic.fields) {
      if (Contains(field.value, detail)) {
        matched = true;
      }
    }
  }
  if (!matched) {
    std::cerr << "server public ABI COPY refusal detail mismatch, expected " << detail << '\n';
    for (const auto& diagnostic : execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(matched || execute.diagnostics.empty(),
          "server public ABI COPY refusal did not expose expected safe detail");
}

api::EngineApiResult ExecuteImportRowsThroughServer(const std::filesystem::path& database_path,
                                                    const api::EngineRequestContext& context) {
  RequirePublicAbiImportRefuses(database_path,
                                context,
                                "canonical_rows_required",
                                true,
                                false);
  RequirePublicAbiImportRefuses(database_path,
                                context,
                                "transaction_uuid and local_transaction_id are both absent",
                                true,
                                true,
                                std::optional<std::uint64_t>{0});

  auto route = MakeServerRoute(database_path, context);
  auto execute = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrame(route.session_uuid, PublicCopyExecuteEnvelope(true, true)));
  if (!execute.accepted) {
    std::cerr << "server public ABI import rejected\n";
    for (const auto& diagnostic : execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(execute.accepted, "server public ABI COPY execution was rejected");
  const auto decoded = DecodeServerExecuteResult(execute.payload);
  Require(decoded.outcome == "accepted", "server public ABI COPY outcome mismatch");
  Require(decoded.operation_id == "dml.execute_import_rows",
          "server public ABI COPY operation mismatch");
  Require(decoded.row_count == 2, "server public ABI COPY row count mismatch");
  Require(Contains(decoded.row_packet, "operation_id=dml.execute_import_rows"),
          "server public ABI COPY row packet missing operation id");
  Require(Contains(decoded.row_packet, "note=copy-public-a") &&
              Contains(decoded.row_packet, "note=copy-public-b"),
          "server public ABI COPY row packet missing imported rows");
  Require(Contains(decoded.row_packet, "evidence=import_execution:delegated_to_dml.insert_rows"),
          "server public ABI COPY did not report insert delegation evidence");
  Require(Contains(decoded.row_packet, "evidence=mga_row_store:row_insert"),
          "server public ABI COPY did not report MGA insert evidence");

  return SelectById(database_path, context, "6");
}

void RequireOrderedSelectThroughServer(const std::filesystem::path& database_path,
                                       const api::EngineRequestContext& context) {
  auto route = MakeServerRoute(database_path, context);
  auto execute = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrame(route.session_uuid, PublicOrderedSelectEnvelope()));
  if (!execute.accepted) {
    std::cerr << "server public ABI ordered SELECT rejected\n";
    for (const auto& diagnostic : execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(execute.accepted, "server public ABI ordered SELECT was rejected");
  const auto decoded = DecodeServerExecuteResult(execute.payload);
  Require(decoded.outcome == "accepted", "server ordered SELECT outcome mismatch");
  Require(decoded.operation_id == "dml.select_rows",
          "server ordered SELECT operation mismatch");
  Require(decoded.row_count == 2, "server ordered SELECT row count mismatch");
  const auto id7 = decoded.row_packet.find("id=7");
  const auto id6 = decoded.row_packet.find("id=6");
  Require(id7 != std::string::npos && id6 != std::string::npos,
          "server ordered SELECT did not return expected ordered/sliced ids");
  Require(id7 < id6, "server ordered SELECT did not preserve descending order after offset");
  Require(Contains(decoded.row_packet, "note=copy-public-b") &&
              Contains(decoded.row_packet, "note=copy-public-a"),
          "server ordered SELECT did not return expected row values");
}

void RequireFetchBoundedSelectThroughServer(const std::filesystem::path& database_path,
                                            const api::EngineRequestContext& context) {
  auto route = MakeServerRoute(database_path, context);
  auto execute = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrame(route.session_uuid, PublicFetchBoundedSelectEnvelope()));
  if (!execute.accepted) {
    std::cerr << "server public ABI FETCH-bounded SELECT rejected\n";
    for (const auto& diagnostic : execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(execute.accepted, "server public ABI FETCH-bounded SELECT was rejected");
  const auto decoded = DecodeServerExecuteResult(execute.payload);
  Require(decoded.outcome == "accepted", "server FETCH-bounded SELECT outcome mismatch");
  Require(decoded.operation_id == "dml.select_rows",
          "server FETCH-bounded SELECT operation mismatch");
  Require(decoded.row_count == 2,
          "server FETCH-bounded SELECT row count did not prove limit application");
  Require(Contains(decoded.row_packet, "operation_id=dml.select_rows"),
          "server FETCH-bounded SELECT row packet missing operation id");
  Require(Contains(decoded.row_packet, "row_count=2"),
          "server FETCH-bounded SELECT row packet missing bounded row count");
}

void RequireTopBoundedSelectThroughServer(const std::filesystem::path& database_path,
                                          const api::EngineRequestContext& context) {
  auto route = MakeServerRoute(database_path, context);
  auto execute = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrame(route.session_uuid, PublicTopBoundedSelectEnvelope()));
  if (!execute.accepted) {
    std::cerr << "server public ABI TOP-bounded SELECT rejected\n";
    for (const auto& diagnostic : execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(execute.accepted, "server public ABI TOP-bounded SELECT was rejected");
  const auto decoded = DecodeServerExecuteResult(execute.payload);
  Require(decoded.outcome == "accepted", "server TOP-bounded SELECT outcome mismatch");
  Require(decoded.operation_id == "dml.select_rows",
          "server TOP-bounded SELECT operation mismatch");
  Require(decoded.row_count == 2,
          "server TOP-bounded SELECT row count did not prove limit application");
  Require(Contains(decoded.row_packet, "operation_id=dml.select_rows"),
          "server TOP-bounded SELECT row packet missing operation id");
  Require(Contains(decoded.row_packet, "row_count=2"),
          "server TOP-bounded SELECT row packet missing bounded row count");
}

void RequireTableJoinThroughServer(const std::filesystem::path& database_path,
                                   const api::EngineRequestContext& context) {
  auto route = MakeServerRoute(database_path, context);
  auto execute = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrame(route.session_uuid, PublicTableJoinEnvelope()));
  if (!execute.accepted) {
    std::cerr << "server public ABI table JOIN rejected\n";
    for (const auto& diagnostic : execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(execute.accepted, "server public ABI table JOIN was rejected");
  const auto decoded = DecodeServerExecuteResult(execute.payload);
  Require(decoded.outcome == "accepted", "server table JOIN outcome mismatch");
  Require(decoded.operation_id == "query.plan_operation",
          "server table JOIN operation mismatch");
  if (decoded.row_count != 2) {
    std::cerr << "server table JOIN row_count=" << decoded.row_count
              << " packet:\n" << decoded.row_packet << '\n';
  }
  Require(decoded.row_count == 2, "server table JOIN row count mismatch");
  Require(Contains(decoded.row_packet, "result_kind=query_rowset"),
          "server table JOIN did not return a query rowset");
  Require(Contains(decoded.row_packet, "row_count=2"),
          "server table JOIN payload row count mismatch");
  Require(Contains(decoded.row_packet, "c0=1") &&
              Contains(decoded.row_packet, "c0=7"),
          "server table JOIN did not include expected joined left ids");
  Require(Contains(decoded.row_packet, "c1=1") &&
              Contains(decoded.row_packet, "c1=7"),
          "server table JOIN did not include expected joined right ids");
  Require(Contains(decoded.row_packet, "evidence=query_join_algorithm:hash"),
          "server table JOIN did not use engine hash-join evidence");
  Require(Contains(decoded.row_packet, "evidence=query_join_key_binding:descriptor_field"),
          "server table JOIN did not bind join keys through descriptor fields");
  Require(Contains(decoded.row_packet, "evidence=query_relation_count:2"),
          "server table JOIN did not report two engine relations");
}

void RequireTableSetOperationThroughServer(const std::filesystem::path& database_path,
                                           const api::EngineRequestContext& context,
                                           std::string_view operation,
                                           std::uint64_t expected_rows,
                                           std::initializer_list<std::string_view> expected_ids) {
  auto route = MakeServerRoute(database_path, context);
  auto execute = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrame(route.session_uuid, PublicTableSetOperationEnvelope(operation)));
  if (!execute.accepted) {
    std::cerr << "server public ABI table set operation rejected\n";
    for (const auto& diagnostic : execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(execute.accepted, "server public ABI table set operation was rejected");
  const auto decoded = DecodeServerExecuteResult(execute.payload);
  Require(decoded.outcome == "accepted", "server table set operation outcome mismatch");
  Require(decoded.operation_id == "query.plan_operation",
          "server table set operation operation mismatch");
  if (decoded.row_count != expected_rows) {
    std::cerr << "server table set operation row_count=" << decoded.row_count
              << " packet:\n" << decoded.row_packet << '\n';
  }
  Require(decoded.row_count == expected_rows, "server table set operation row count mismatch");
  Require(Contains(decoded.row_packet, "result_kind=query_rowset"),
          "server table set operation did not return a query rowset");
  for (const auto expected_id : expected_ids) {
    Require(Contains(decoded.row_packet, std::string("c0=") + std::string(expected_id)),
            "server table set operation missing expected id");
  }
  Require(Contains(decoded.row_packet,
                   std::string("evidence=query_set_operation:") + std::string(operation)),
          "server table set operation did not report matching engine set-operation evidence");
  Require(Contains(decoded.row_packet, "evidence=query_relation_count:2"),
          "server table set operation did not report two engine relations");
}

void RequireTableSetOperationsThroughServer(const std::filesystem::path& database_path,
                                            const api::EngineRequestContext& context) {
  RequireTableSetOperationThroughServer(database_path,
                                        context,
                                        "union_distinct",
                                        4,
                                        {"1", "7", "8", "9"});
  RequireTableSetOperationThroughServer(database_path,
                                        context,
                                        "intersect_distinct",
                                        2,
                                        {"1", "7"});
  RequireTableSetOperationThroughServer(database_path,
                                        context,
                                        "except_distinct",
                                        1,
                                        {"8"});
}

void RequireTableSetAllOperationsThroughServer(const std::filesystem::path& database_path,
                                               const api::EngineRequestContext& context) {
  RequireTableSetOperationThroughServer(database_path,
                                        context,
                                        "union_all",
                                        9,
                                        {"1", "7", "8", "9"});
  RequireTableSetOperationThroughServer(database_path,
                                        context,
                                        "intersect_all",
                                        3,
                                        {"1", "7"});
  RequireTableSetOperationThroughServer(database_path,
                                        context,
                                        "except_all",
                                        2,
                                        {"8", "7"});
}

void RequireTableSetByNameOperationThroughServer(
    const std::filesystem::path& database_path,
    const api::EngineRequestContext& context,
    std::string_view operation,
    std::uint64_t expected_rows,
    std::initializer_list<std::string_view> expected_fragments) {
  auto route = MakeServerRoute(database_path, context);
  auto execute = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrame(route.session_uuid, PublicTableSetByNameOperationEnvelope(operation)));
  if (!execute.accepted) {
    std::cerr << "server public ABI BY NAME table set operation rejected\n";
    for (const auto& diagnostic : execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(execute.accepted, "server public ABI BY NAME table set operation was rejected");
  const auto decoded = DecodeServerExecuteResult(execute.payload);
  Require(decoded.outcome == "accepted", "server BY NAME table set operation outcome mismatch");
  Require(decoded.operation_id == "query.plan_operation",
          "server BY NAME table set operation operation mismatch");
  if (decoded.row_count != expected_rows) {
    std::cerr << "server BY NAME table set operation row_count=" << decoded.row_count
              << " packet:\n" << decoded.row_packet << '\n';
  }
  Require(decoded.row_count == expected_rows,
          "server BY NAME table set operation row count mismatch");
  Require(Contains(decoded.row_packet, "result_kind=query_rowset"),
          "server BY NAME table set operation did not return a query rowset");
  for (const auto expected : expected_fragments) {
    Require(Contains(decoded.row_packet, std::string(expected)),
            "server BY NAME table set operation missing expected row fragment");
  }
  Require(Contains(decoded.row_packet,
                   std::string("evidence=query_set_operation:") + std::string(operation)),
          "server BY NAME table set operation did not report matching engine evidence");
  Require(Contains(decoded.row_packet, "evidence=query_set_binding:descriptor_name"),
          "server BY NAME table set operation did not report descriptor-name binding");
  Require(Contains(decoded.row_packet, "evidence=query_relation_count:2"),
          "server BY NAME table set operation did not report two engine relations");
}

void RequireTableSetByNameOperationsThroughServer(const std::filesystem::path& database_path,
                                                  const api::EngineRequestContext& context) {
  RequireTableSetByNameOperationThroughServer(database_path,
                                              context,
                                              "union_distinct",
                                              3,
                                              {"c0=1;c1=10", "c0=2;c1=20", "c0=3;c1=30"});
  RequireTableSetByNameOperationThroughServer(database_path,
                                              context,
                                              "intersect_distinct",
                                              1,
                                              {"c0=2;c1=20"});
  RequireTableSetByNameOperationThroughServer(database_path,
                                              context,
                                              "except_distinct",
                                              1,
                                              {"c0=1;c1=10"});
  RequireTableSetByNameOperationThroughServer(database_path,
                                              context,
                                              "union_all",
                                              4,
                                              {"c0=3;c1=30"});
}

void RequireRowNumberWindowThroughServer(const std::filesystem::path& database_path,
                                         const api::EngineRequestContext& context) {
  auto route = MakeServerRoute(database_path, context);
  auto execute = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrame(route.session_uuid, PublicRowNumberWindowEnvelope()));
  if (!execute.accepted) {
    std::cerr << "server public ABI row_number window rejected\n";
    for (const auto& diagnostic : execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(execute.accepted, "server public ABI row_number window was rejected");
  const auto decoded = DecodeServerExecuteResult(execute.payload);
  Require(decoded.outcome == "accepted", "server row_number window outcome mismatch");
  Require(decoded.operation_id == "query.plan_operation",
          "server row_number window operation mismatch");
  if (decoded.row_count != 3) {
    std::cerr << "server row_number window row_count=" << decoded.row_count
              << " packet:\n" << decoded.row_packet << '\n';
  }
  Require(decoded.row_count == 3, "server row_number window row count mismatch");
  Require(Contains(decoded.row_packet, "result_kind=query_rowset"),
          "server row_number window did not return a query rowset");
  Require(Contains(decoded.row_packet, "c0=1") &&
              Contains(decoded.row_packet, "c0=7") &&
              Contains(decoded.row_packet, "c0=8"),
          "server row_number window missing ordered source ids");
  Require(Contains(decoded.row_packet, "c1=1") &&
              Contains(decoded.row_packet, "c1=2") &&
              Contains(decoded.row_packet, "c1=3"),
          "server row_number window missing row-number ordinals");
  Require(Contains(decoded.row_packet, "evidence=query_window:row_number"),
          "server row_number window did not report engine window evidence");
  Require(Contains(decoded.row_packet, "evidence=query_window_binding:descriptor_field"),
          "server row_number window did not report descriptor-field binding");
  Require(Contains(decoded.row_packet, "evidence=query_relation_count:1"),
          "server row_number window did not report one engine relation");
}

void RequirePartitionCountWindowThroughServer(const std::filesystem::path& database_path,
                                              const api::EngineRequestContext& context) {
  auto route = MakeServerRoute(database_path, context);
  auto execute = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrame(route.session_uuid, PublicPartitionCountWindowEnvelope()));
  if (!execute.accepted) {
    std::cerr << "server public ABI partition-count window rejected\n";
    for (const auto& diagnostic : execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(execute.accepted, "server public ABI partition-count window was rejected");
  const auto decoded = DecodeServerExecuteResult(execute.payload);
  Require(decoded.outcome == "accepted", "server partition-count window outcome mismatch");
  Require(decoded.operation_id == "query.plan_operation",
          "server partition-count window operation mismatch");
  if (decoded.row_count != 3) {
    std::cerr << "server partition-count window row_count=" << decoded.row_count
              << " packet:\n" << decoded.row_packet << '\n';
  }
  Require(decoded.row_count == 3, "server partition-count window row count mismatch");
  Require(Contains(decoded.row_packet, "result_kind=query_rowset"),
          "server partition-count window did not return a query rowset");
  Require(Contains(decoded.row_packet, "c0=1;c1=10;c2=12;c3=2") &&
              Contains(decoded.row_packet, "c0=2;c1=10;c2=13;c3=2") &&
              Contains(decoded.row_packet, "c0=3;c1=20;c2=7;c3=1"),
          "server partition-count window missing expected partition counts");
  Require(Contains(decoded.row_packet, "evidence=query_window:count_star_partition"),
          "server partition-count window did not report engine window evidence");
  Require(Contains(decoded.row_packet, "evidence=query_window_partition_binding:descriptor_field"),
          "server partition-count window did not report descriptor-field partition binding");
  Require(Contains(decoded.row_packet, "evidence=query_relation_count:1"),
          "server partition-count window did not report one engine relation");
}

void RequireNavigationWindowThroughServer(const std::filesystem::path& database_path,
                                          const api::EngineRequestContext& context,
                                          std::string_view function,
                                          std::initializer_list<std::string_view> expected_fragments) {
  auto route = MakeServerRoute(database_path, context);
  auto execute = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrame(route.session_uuid, PublicNavigationWindowEnvelope(function)));
  if (!execute.accepted) {
    std::cerr << "server public ABI navigation window rejected\n";
    for (const auto& diagnostic : execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(execute.accepted, "server public ABI navigation window was rejected");
  const auto decoded = DecodeServerExecuteResult(execute.payload);
  Require(decoded.outcome == "accepted", "server navigation window outcome mismatch");
  Require(decoded.operation_id == "query.plan_operation",
          "server navigation window operation mismatch");
  Require(decoded.row_count == 3, "server navigation window row count mismatch");
  Require(Contains(decoded.row_packet, "result_kind=query_rowset"),
          "server navigation window did not return a query rowset");
  for (const auto expected : expected_fragments) {
    if (!Contains(decoded.row_packet, std::string(expected))) {
      std::cerr << "server navigation window function=" << function
                << " missing fragment=" << expected
                << "\npacket:\n" << decoded.row_packet << '\n';
    }
    Require(Contains(decoded.row_packet, std::string(expected)),
            "server navigation window missing expected row fragment");
  }
  Require(Contains(decoded.row_packet,
                   std::string("evidence=query_window:") + std::string(function)),
          "server navigation window did not report matching engine window evidence");
  Require(Contains(decoded.row_packet, "evidence=query_window_binding:descriptor_field"),
          "server navigation window did not report descriptor-field binding");
  Require(Contains(decoded.row_packet, "evidence=query_relation_count:1"),
          "server navigation window did not report one engine relation");
}

void RequireNavigationWindowsThroughServer(const std::filesystem::path& database_path,
                                           const api::EngineRequestContext& context) {
  RequireNavigationWindowThroughServer(database_path,
                                       context,
                                       "rank",
                                       {"c0=1;c1=1", "c0=7;c1=2", "c0=8;c1=3"});
  RequireNavigationWindowThroughServer(database_path,
                                       context,
                                       "dense_rank",
                                       {"c0=1;c1=1", "c0=7;c1=2", "c0=8;c1=3"});
  RequireNavigationWindowThroughServer(database_path,
                                       context,
                                       "percent_rank",
                                        {"c0=1;c1=0",
                                         "c0=7;c1=0.5",
                                         "c0=8;c1=1",
                                        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
                                        "evidence=query_window_typed_result:descriptor_nullable"});
  RequireNavigationWindowThroughServer(database_path,
                                       context,
                                       "cume_dist",
                                        {"c0=1;c1=0.333333333333333",
                                         "c0=7;c1=0.666666666666666",
                                         "c0=8;c1=1",
                                        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
                                        "evidence=query_window_typed_result:descriptor_nullable"});
  RequireNavigationWindowThroughServer(database_path,
                                       context,
                                       "ntile",
                                       {"c0=1;c1=1", "c0=7;c1=1", "c0=8;c1=2"});
  RequireNavigationWindowThroughServer(database_path,
                                       context,
                                       "lag",
                                       {"c0=1;c1=0", "c0=7;c1=1", "c0=8;c1=7"});
  RequireNavigationWindowThroughServer(database_path,
                                       context,
                                       "lead",
                                       {"c0=1;c1=7", "c0=7;c1=8", "c0=8;c1=0"});
  RequireNavigationWindowThroughServer(database_path,
                                       context,
                                       "first_value",
                                       {"c0=1;c1=1", "c0=7;c1=1", "c0=8;c1=1"});
  RequireNavigationWindowThroughServer(database_path,
                                       context,
                                       "last_value",
                                       {"c0=1;c1=8", "c0=7;c1=8", "c0=8;c1=8"});
  RequireNavigationWindowThroughServer(database_path,
                                       context,
                                       "nth_value",
                                        {"c0=1;c1=",
                                         "c0=7;c1=7",
                                         "c0=8;c1=7",
                                        "row_meta[0]=c0:int64:not_null;c1:int64:null",
                                        "row_meta[1]=c0:int64:not_null;c1:int64:not_null",
                                        "evidence=query_window_typed_result:descriptor_nullable"});
}

void RequireGroupByAggregateThroughServer(const std::filesystem::path& database_path,
                                          const api::EngineRequestContext& context) {
  auto route = MakeServerRoute(database_path, context);
  auto execute = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrame(route.session_uuid, PublicGroupByAggregateEnvelope()));
  if (!execute.accepted) {
    std::cerr << "server public ABI grouped aggregate rejected\n";
    for (const auto& diagnostic : execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(execute.accepted, "server public ABI grouped aggregate was rejected");
  const auto decoded = DecodeServerExecuteResult(execute.payload);
  Require(decoded.outcome == "accepted", "server grouped aggregate outcome mismatch");
  Require(decoded.operation_id == "query.plan_operation",
          "server grouped aggregate operation mismatch");
  if (decoded.row_count != 2) {
    std::cerr << "server grouped aggregate row_count=" << decoded.row_count
              << " packet:\n" << decoded.row_packet << '\n';
  }
  Require(decoded.row_count == 2, "server grouped aggregate row count mismatch");
  Require(Contains(decoded.row_packet, "result_kind=query_rowset"),
          "server grouped aggregate did not return a query rowset");
  Require(Contains(decoded.row_packet, "c0=10;c1=25"),
          "server grouped aggregate missing dept=10 sum=25");
  Require(Contains(decoded.row_packet, "c0=20;c1=7"),
          "server grouped aggregate missing dept=20 sum=7");
  Require(Contains(decoded.row_packet, "evidence=query_aggregate_group_key_column:1"),
          "server grouped aggregate did not bind group key by descriptor field");
  Require(Contains(decoded.row_packet, "evidence=query_aggregate_value_column:2"),
          "server grouped aggregate did not bind aggregate value by descriptor field");
  Require(Contains(decoded.row_packet, "evidence=query_aggregate_key_binding:descriptor_field"),
          "server grouped aggregate did not report descriptor key binding");
  Require(Contains(decoded.row_packet, "evidence=query_aggregate_value_binding:descriptor_field"),
          "server grouped aggregate did not report descriptor value binding");
  Require(Contains(decoded.row_packet, "evidence=query_aggregate:sum_by_key"),
          "server grouped aggregate did not report engine aggregate evidence");
  Require(Contains(decoded.row_packet, "evidence=query_relation_count:1"),
          "server grouped aggregate did not report one engine relation");

  auto having_route = MakeServerRoute(database_path, context);
  auto having_execute = server::HandleExecuteSblr(
      &having_route.registry,
      having_route.engine_state,
      ExecuteFrame(having_route.session_uuid, PublicHavingAggregateEnvelope()));
  if (!having_execute.accepted) {
    std::cerr << "server public ABI HAVING aggregate rejected\n";
    for (const auto& diagnostic : having_execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(having_execute.accepted, "server public ABI HAVING aggregate was rejected");
  const auto having_decoded = DecodeServerExecuteResult(having_execute.payload);
  Require(having_decoded.outcome == "accepted", "server HAVING aggregate outcome mismatch");
  Require(having_decoded.operation_id == "query.plan_operation",
          "server HAVING aggregate operation mismatch");
  if (having_decoded.row_count != 1) {
    std::cerr << "server HAVING aggregate row_count=" << having_decoded.row_count
              << " packet:\n" << having_decoded.row_packet << '\n';
  }
  Require(having_decoded.row_count == 1, "server HAVING aggregate row count mismatch");
  Require(Contains(having_decoded.row_packet, "result_kind=query_rowset"),
          "server HAVING aggregate did not return a query rowset");
  Require(Contains(having_decoded.row_packet, "c0=10;c1=25"),
          "server HAVING aggregate missing kept dept=10 sum=25");
  Require(!Contains(having_decoded.row_packet, "c0=20;c1=7"),
          "server HAVING aggregate failed to filter dept=20 sum=7");
  Require(Contains(having_decoded.row_packet, "evidence=query_aggregate_having_predicate:aggregate_gt"),
          "server HAVING aggregate did not report predicate evidence");
  Require(Contains(having_decoded.row_packet, "evidence=query_aggregate_having_threshold:20"),
          "server HAVING aggregate did not report threshold evidence");
  Require(Contains(having_decoded.row_packet, "evidence=query_aggregate_having_value_column:1"),
          "server HAVING aggregate did not report value column evidence");
  Require(Contains(having_decoded.row_packet, "evidence=query_aggregate_having_filter_after_grouping:true"),
          "server HAVING aggregate did not report post-grouping filter evidence");
  Require(Contains(having_decoded.row_packet, "evidence=query_aggregate:sum_by_key"),
          "server HAVING aggregate did not report grouped sum evidence");

  struct CoreAggregateCase {
    std::string_view function;
    std::string_view typed_result_profile;
    std::initializer_list<std::string_view> expected_fragments;
  };
  const CoreAggregateCase core_cases[] = {
      {"sb.aggregate.count",
       "int64_nonnull",
       {"c0=10;c1=2",
        "c0=20;c1=1",
        "row_meta[0]=c0:int64:not_null;c1:int64:not_null",
        "evidence=query_aggregate:count_by_key"}},
      {"sb.aggregate.avg",
       "real64_nullable",
       {"c0=10;c1=12.5",
        "c0=20;c1=7",
        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
        "evidence=query_aggregate:avg_by_key"}},
      {"sb.aggregate.min",
       "real64_nullable",
       {"c0=10;c1=12",
        "c0=20;c1=7",
        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
        "evidence=query_aggregate:min_by_key"}},
      {"sb.aggregate.max",
       "real64_nullable",
       {"c0=10;c1=13",
        "c0=20;c1=7",
        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
        "evidence=query_aggregate:max_by_key"}},
  };
  for (const auto& test : core_cases) {
    auto core_route = MakeServerRoute(database_path, context);
    auto core_execute = server::HandleExecuteSblr(
        &core_route.registry,
        core_route.engine_state,
        ExecuteFrame(core_route.session_uuid, PublicGroupByAggregateEnvelope(test.function)));
    if (!core_execute.accepted) {
      std::cerr << "server public ABI core grouped aggregate rejected for "
                << test.function << '\n';
      for (const auto& diagnostic : core_execute.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << field.key << '=' << field.value << '\n';
        }
      }
    }
    Require(core_execute.accepted,
            "server public ABI core grouped aggregate was rejected");
    const auto core_decoded = DecodeServerExecuteResult(core_execute.payload);
    Require(core_decoded.outcome == "accepted",
            "server core grouped aggregate outcome mismatch");
    Require(core_decoded.operation_id == "query.plan_operation",
            "server core grouped aggregate operation mismatch");
    Require(core_decoded.row_count == 2,
            "server core grouped aggregate row count mismatch");
    Require(Contains(core_decoded.row_packet, "result_kind=query_rowset"),
            "server core grouped aggregate did not return a query rowset");
    for (const auto expected : test.expected_fragments) {
      if (!Contains(core_decoded.row_packet, std::string(expected))) {
        std::cerr << "server core aggregate function=" << test.function
                  << " missing fragment=" << expected
                  << "\npacket:\n" << core_decoded.row_packet << '\n';
      }
      Require(Contains(core_decoded.row_packet, std::string(expected)),
              "server core grouped aggregate missing expected fragment");
    }
    Require(Contains(core_decoded.row_packet,
                     std::string("evidence=query_aggregate_typed_result:") +
                         std::string(test.typed_result_profile)),
            "server core grouped aggregate did not report typed aggregate evidence");
    Require(Contains(core_decoded.row_packet,
                     "evidence=query_aggregate_key_binding:descriptor_field"),
            "server core grouped aggregate did not report descriptor key binding");
    Require(Contains(core_decoded.row_packet,
                     "evidence=query_aggregate_value_binding:descriptor_field"),
            "server core grouped aggregate did not report descriptor value binding");
    Require(Contains(core_decoded.row_packet, "evidence=query_relation_count:1"),
            "server core grouped aggregate did not report one engine relation");
  }

  struct StatisticalAggregateCase {
    std::string_view function;
    std::initializer_list<std::string_view> expected_fragments;
  };
  const StatisticalAggregateCase statistical_cases[] = {
      {"sb.aggregate.stddev",
       {"c0=10;c1=0.707106781186547", "c0=20;c1=",
        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
        "row_meta[1]=c0:int64:not_null;c1:real64:null",
        "evidence=query_aggregate:stddev_by_key"}},
      {"sb.aggregate.stddev_samp",
       {"c0=10;c1=0.707106781186547", "c0=20;c1=",
        "evidence=query_aggregate:stddev_samp_by_key"}},
      {"sb.aggregate.stddev_pop",
       {"c0=10;c1=0.5", "c0=20;c1=0",
        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
        "row_meta[1]=c0:int64:not_null;c1:real64:not_null",
        "evidence=query_aggregate:stddev_pop_by_key"}},
      {"sb.aggregate.variance",
       {"c0=10;c1=0.5", "c0=20;c1=",
        "evidence=query_aggregate:variance_by_key"}},
      {"sb.aggregate.variance_samp",
       {"c0=10;c1=0.5", "c0=20;c1=",
        "evidence=query_aggregate:variance_samp_by_key"}},
      {"sb.aggregate.variance_pop",
       {"c0=10;c1=0.25", "c0=20;c1=0",
        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
        "row_meta[1]=c0:int64:not_null;c1:real64:not_null",
        "evidence=query_aggregate:variance_pop_by_key"}},
  };
  for (const auto& test : statistical_cases) {
    auto stat_route = MakeServerRoute(database_path, context);
    auto stat_execute = server::HandleExecuteSblr(
        &stat_route.registry,
        stat_route.engine_state,
        ExecuteFrame(stat_route.session_uuid, PublicGroupByAggregateEnvelope(test.function)));
    if (!stat_execute.accepted) {
      std::cerr << "server public ABI statistical grouped aggregate rejected for "
                << test.function << '\n';
      for (const auto& diagnostic : stat_execute.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << field.key << '=' << field.value << '\n';
        }
      }
    }
    Require(stat_execute.accepted,
            "server public ABI statistical grouped aggregate was rejected");
    const auto stat_decoded = DecodeServerExecuteResult(stat_execute.payload);
    Require(stat_decoded.outcome == "accepted",
            "server statistical grouped aggregate outcome mismatch");
    Require(stat_decoded.operation_id == "query.plan_operation",
            "server statistical grouped aggregate operation mismatch");
    Require(stat_decoded.row_count == 2,
            "server statistical grouped aggregate row count mismatch");
    Require(Contains(stat_decoded.row_packet, "result_kind=query_rowset"),
            "server statistical grouped aggregate did not return a query rowset");
    for (const auto expected : test.expected_fragments) {
      if (!Contains(stat_decoded.row_packet, std::string(expected))) {
        std::cerr << "server statistical aggregate function=" << test.function
                  << " missing fragment=" << expected
                  << "\npacket:\n" << stat_decoded.row_packet << '\n';
      }
      Require(Contains(stat_decoded.row_packet, std::string(expected)),
              "server statistical grouped aggregate missing expected fragment");
    }
    Require(Contains(stat_decoded.row_packet,
                     "evidence=query_aggregate_typed_result:real64_nullable"),
            "server statistical grouped aggregate did not report typed aggregate evidence");
    Require(Contains(stat_decoded.row_packet,
                     "evidence=query_aggregate_key_binding:descriptor_field"),
            "server statistical grouped aggregate did not report descriptor key binding");
    Require(Contains(stat_decoded.row_packet,
                     "evidence=query_aggregate_value_binding:descriptor_field"),
            "server statistical grouped aggregate did not report descriptor value binding");
    Require(Contains(stat_decoded.row_packet, "evidence=query_relation_count:1"),
            "server statistical grouped aggregate did not report one engine relation");
  }

  struct ApproxAggregateCase {
    std::string_view function;
    std::string_view typed_result_profile;
    std::initializer_list<std::string_view> expected_fragments;
  };
  const ApproxAggregateCase approx_cases[] = {
      {"sb.aggregate.approx_count_distinct",
       "int64_nonnull",
       {"c0=10;c1=2",
        "c0=20;c1=1",
        "row_meta[0]=c0:int64:not_null;c1:int64:not_null",
        "row_meta[1]=c0:int64:not_null;c1:int64:not_null",
        "evidence=query_aggregate:approx_count_distinct_by_key"}},
      {"sb.aggregate.approx_median",
       "real64_nullable",
       {"c0=10;c1=12.5",
        "c0=20;c1=7",
        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
        "row_meta[1]=c0:int64:not_null;c1:real64:not_null",
        "evidence=query_aggregate:approx_median_by_key"}},
  };
  for (const auto& test : approx_cases) {
    auto approx_route = MakeServerRoute(database_path, context);
    auto approx_execute = server::HandleExecuteSblr(
        &approx_route.registry,
        approx_route.engine_state,
        ExecuteFrame(approx_route.session_uuid, PublicGroupByAggregateEnvelope(test.function)));
    if (!approx_execute.accepted) {
      std::cerr << "server public ABI approximate grouped aggregate rejected for "
                << test.function << '\n';
      for (const auto& diagnostic : approx_execute.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << field.key << '=' << field.value << '\n';
        }
      }
    }
    Require(approx_execute.accepted,
            "server public ABI approximate grouped aggregate was rejected");
    const auto approx_decoded = DecodeServerExecuteResult(approx_execute.payload);
    Require(approx_decoded.outcome == "accepted",
            "server approximate grouped aggregate outcome mismatch");
    Require(approx_decoded.operation_id == "query.plan_operation",
            "server approximate grouped aggregate operation mismatch");
    Require(approx_decoded.row_count == 2,
            "server approximate grouped aggregate row count mismatch");
    Require(Contains(approx_decoded.row_packet, "result_kind=query_rowset"),
            "server approximate grouped aggregate did not return a query rowset");
    for (const auto expected : test.expected_fragments) {
      if (!Contains(approx_decoded.row_packet, std::string(expected))) {
        std::cerr << "server approximate aggregate function=" << test.function
                  << " missing fragment=" << expected
                  << "\npacket:\n" << approx_decoded.row_packet << '\n';
      }
      Require(Contains(approx_decoded.row_packet, std::string(expected)),
              "server approximate grouped aggregate missing expected fragment");
    }
    Require(Contains(approx_decoded.row_packet,
                     std::string("evidence=query_aggregate_typed_result:") +
                         std::string(test.typed_result_profile)),
            "server approximate grouped aggregate did not report typed aggregate evidence");
    Require(Contains(approx_decoded.row_packet,
                     "evidence=query_aggregate_key_binding:descriptor_field"),
            "server approximate grouped aggregate did not report descriptor key binding");
    Require(Contains(approx_decoded.row_packet,
                     "evidence=query_aggregate_value_binding:descriptor_field"),
            "server approximate grouped aggregate did not report descriptor value binding");
    Require(Contains(approx_decoded.row_packet, "evidence=query_relation_count:1"),
            "server approximate grouped aggregate did not report one engine relation");
  }

  struct PairAggregateCase {
    std::string_view function;
    std::string_view typed_result_profile;
    std::initializer_list<std::string_view> expected_fragments;
  };
  const PairAggregateCase pair_cases[] = {
      {"sb.aggregate.corr",
       "real64_nullable",
       {"c0=10;c1=1",
        "c0=20;c1=",
        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
        "row_meta[1]=c0:int64:not_null;c1:real64:null",
        "evidence=query_aggregate:corr_by_key"}},
      {"sb.aggregate.covar_pop",
       "real64_nullable",
       {"c0=10;c1=0.25",
        "c0=20;c1=0",
        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
        "row_meta[1]=c0:int64:not_null;c1:real64:not_null",
        "evidence=query_aggregate:covar_pop_by_key"}},
      {"sb.aggregate.covar_samp",
       "real64_nullable",
       {"c0=10;c1=0.5",
        "c0=20;c1=",
        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
        "row_meta[1]=c0:int64:not_null;c1:real64:null",
        "evidence=query_aggregate:covar_samp_by_key"}},
      {"sb.aggregate.regr_count",
       "int64_nonnull",
       {"c0=10;c1=2",
        "c0=20;c1=1",
        "row_meta[0]=c0:int64:not_null;c1:int64:not_null",
        "row_meta[1]=c0:int64:not_null;c1:int64:not_null",
        "evidence=query_aggregate:regr_count_by_key"}},
      {"sb.aggregate.regr_avgx",
       "real64_nullable",
       {"c0=10;c1=1.5",
        "c0=20;c1=3",
        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
        "row_meta[1]=c0:int64:not_null;c1:real64:not_null",
        "evidence=query_aggregate:regr_avgx_by_key"}},
      {"sb.aggregate.regr_avgy",
       "real64_nullable",
       {"c0=10;c1=12.5",
        "c0=20;c1=7",
        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
        "row_meta[1]=c0:int64:not_null;c1:real64:not_null",
        "evidence=query_aggregate:regr_avgy_by_key"}},
      {"sb.aggregate.regr_intercept",
       "real64_nullable",
       {"c0=10;c1=11",
        "c0=20;c1=",
        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
        "row_meta[1]=c0:int64:not_null;c1:real64:null",
        "evidence=query_aggregate:regr_intercept_by_key"}},
      {"sb.aggregate.regr_r2",
       "real64_nullable",
       {"c0=10;c1=1",
        "c0=20;c1=",
        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
        "row_meta[1]=c0:int64:not_null;c1:real64:null",
        "evidence=query_aggregate:regr_r2_by_key"}},
      {"sb.aggregate.regr_slope",
       "real64_nullable",
       {"c0=10;c1=1",
        "c0=20;c1=",
        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
        "row_meta[1]=c0:int64:not_null;c1:real64:null",
        "evidence=query_aggregate:regr_slope_by_key"}},
      {"sb.aggregate.regr_sxx",
       "real64_nullable",
       {"c0=10;c1=0.5",
        "c0=20;c1=0",
        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
        "row_meta[1]=c0:int64:not_null;c1:real64:not_null",
        "evidence=query_aggregate:regr_sxx_by_key"}},
      {"sb.aggregate.regr_sxy",
       "real64_nullable",
       {"c0=10;c1=0.5",
        "c0=20;c1=0",
        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
        "row_meta[1]=c0:int64:not_null;c1:real64:not_null",
        "evidence=query_aggregate:regr_sxy_by_key"}},
      {"sb.aggregate.regr_syy",
       "real64_nullable",
       {"c0=10;c1=0.5",
        "c0=20;c1=0",
        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
        "row_meta[1]=c0:int64:not_null;c1:real64:not_null",
        "evidence=query_aggregate:regr_syy_by_key"}},
  };
  for (const auto& test : pair_cases) {
    auto pair_route = MakeServerRoute(database_path, context);
    auto pair_execute = server::HandleExecuteSblr(
        &pair_route.registry,
        pair_route.engine_state,
        ExecuteFrame(pair_route.session_uuid, PublicPairAggregateEnvelope(test.function)));
    if (!pair_execute.accepted) {
      std::cerr << "server public ABI pair grouped aggregate rejected for "
                << test.function << '\n';
      for (const auto& diagnostic : pair_execute.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << field.key << '=' << field.value << '\n';
        }
      }
    }
    Require(pair_execute.accepted,
            "server public ABI pair grouped aggregate was rejected");
    const auto pair_decoded = DecodeServerExecuteResult(pair_execute.payload);
    Require(pair_decoded.outcome == "accepted",
            "server pair grouped aggregate outcome mismatch");
    Require(pair_decoded.operation_id == "query.plan_operation",
            "server pair grouped aggregate operation mismatch");
    Require(pair_decoded.row_count == 2,
            "server pair grouped aggregate row count mismatch");
    Require(Contains(pair_decoded.row_packet, "result_kind=query_rowset"),
            "server pair grouped aggregate did not return a query rowset");
    for (const auto expected : test.expected_fragments) {
      if (!Contains(pair_decoded.row_packet, std::string(expected))) {
        std::cerr << "server pair aggregate function=" << test.function
                  << " missing fragment=" << expected
                  << "\npacket:\n" << pair_decoded.row_packet << '\n';
      }
      Require(Contains(pair_decoded.row_packet, std::string(expected)),
              "server pair grouped aggregate missing expected fragment");
    }
    Require(Contains(pair_decoded.row_packet,
                     std::string("evidence=query_aggregate_typed_result:") +
                         std::string(test.typed_result_profile)),
            "server pair grouped aggregate did not report typed aggregate evidence");
    Require(Contains(pair_decoded.row_packet,
                     "evidence=query_aggregate_pair_value_column:0"),
            "server pair grouped aggregate did not report pair value column");
    Require(Contains(pair_decoded.row_packet,
                     "evidence=query_aggregate_pair_value_binding:descriptor_field"),
            "server pair grouped aggregate did not report descriptor pair binding");
    Require(Contains(pair_decoded.row_packet, "evidence=query_relation_count:1"),
            "server pair grouped aggregate did not report one engine relation");
  }

  struct DistributionAggregateCase {
    std::string_view function;
    std::string_view option_name;
    std::string_view option_value;
    std::string_view typed_result_profile;
    std::initializer_list<std::string_view> expected_fragments;
  };
  const DistributionAggregateCase distribution_cases[] = {
      {"sb.aggregate.mode",
       "aggregate_fraction",
       "0.5",
       "real64_nullable",
       {"c0=10;c1=12",
        "c0=20;c1=7",
        "row_meta[0]=c0:int64:not_null;c1:real64:not_null",
        "evidence=query_aggregate:mode_by_key"}},
      {"sb.aggregate.percentile_cont",
       "aggregate_fraction",
       "0.5",
       "real64_nullable",
       {"c0=10;c1=12.5",
        "c0=20;c1=7",
        "evidence=query_aggregate:percentile_cont_by_key"}},
      {"sb.aggregate.percentile_disc",
       "aggregate_fraction",
       "0.5",
       "real64_nullable",
       {"c0=10;c1=12",
        "c0=20;c1=7",
        "evidence=query_aggregate:percentile_disc_by_key"}},
      {"sb.aggregate.approx_percentile_cont",
       "aggregate_fraction",
       "0.5",
       "real64_nullable",
       {"c0=10;c1=12.5",
        "c0=20;c1=7",
        "evidence=query_aggregate:approx_percentile_cont_by_key"}},
      {"sb.aggregate.approx_percentile_disc",
       "aggregate_fraction",
       "0.5",
       "real64_nullable",
       {"c0=10;c1=12",
        "c0=20;c1=7",
        "evidence=query_aggregate:approx_percentile_disc_by_key"}},
      {"sb.aggregate.approx_top_k",
       "aggregate_limit",
       "1",
       "json_nullable",
       {"c0=10;c1=[{\"value\":\"12\",\"count\":1}]",
        "c0=20;c1=[{\"value\":\"7\",\"count\":1}]",
        "row_meta[0]=c0:int64:not_null;c1:json:not_null",
        "evidence=query_aggregate:approx_top_k_by_key"}},
  };
  for (const auto& test : distribution_cases) {
    auto dist_route = MakeServerRoute(database_path, context);
    auto dist_execute = server::HandleExecuteSblr(
        &dist_route.registry,
        dist_route.engine_state,
        ExecuteFrame(dist_route.session_uuid,
                     PublicAggregateOptionEnvelope(test.function,
                                                   test.option_name,
                                                   test.option_value)));
    if (!dist_execute.accepted) {
      std::cerr << "server public ABI distribution grouped aggregate rejected for "
                << test.function << '\n';
      for (const auto& diagnostic : dist_execute.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << field.key << '=' << field.value << '\n';
        }
      }
    }
    Require(dist_execute.accepted,
            "server public ABI distribution grouped aggregate was rejected");
    const auto dist_decoded = DecodeServerExecuteResult(dist_execute.payload);
    Require(dist_decoded.outcome == "accepted",
            "server distribution grouped aggregate outcome mismatch");
    Require(dist_decoded.operation_id == "query.plan_operation",
            "server distribution grouped aggregate operation mismatch");
    Require(dist_decoded.row_count == 2,
            "server distribution grouped aggregate row count mismatch");
    Require(Contains(dist_decoded.row_packet, "result_kind=query_rowset"),
            "server distribution grouped aggregate did not return a query rowset");
    for (const auto expected : test.expected_fragments) {
      if (!Contains(dist_decoded.row_packet, std::string(expected))) {
        std::cerr << "server distribution aggregate function=" << test.function
                  << " missing fragment=" << expected
                  << "\npacket:\n" << dist_decoded.row_packet << '\n';
      }
      Require(Contains(dist_decoded.row_packet, std::string(expected)),
              "server distribution grouped aggregate missing expected fragment");
    }
    Require(Contains(dist_decoded.row_packet,
                     std::string("evidence=query_aggregate_typed_result:") +
                         std::string(test.typed_result_profile)),
            "server distribution grouped aggregate did not report typed aggregate evidence");
    Require(Contains(dist_decoded.row_packet, "evidence=query_relation_count:1"),
            "server distribution grouped aggregate did not report one engine relation");
  }

  auto listagg_route = MakeServerRoute(database_path, context);
  auto listagg_execute = server::HandleExecuteSblr(
      &listagg_route.registry,
      listagg_route.engine_state,
      ExecuteFrame(listagg_route.session_uuid, PublicListAggEnvelope()));
  if (!listagg_execute.accepted) {
    std::cerr << "server public ABI LISTAGG grouped aggregate rejected\n";
    for (const auto& diagnostic : listagg_execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(listagg_execute.accepted,
          "server public ABI LISTAGG grouped aggregate was rejected");
  const auto listagg_decoded = DecodeServerExecuteResult(listagg_execute.payload);
  Require(listagg_decoded.outcome == "accepted",
          "server LISTAGG grouped aggregate outcome mismatch");
  Require(listagg_decoded.operation_id == "query.plan_operation",
          "server LISTAGG grouped aggregate operation mismatch");
  Require(listagg_decoded.row_count == 3,
          "server LISTAGG grouped aggregate row count mismatch");
  Require(Contains(listagg_decoded.row_packet, "result_kind=query_rowset"),
          "server LISTAGG grouped aggregate did not return a query rowset");
  Require(Contains(listagg_decoded.row_packet, "c0=10;c1=true|true"),
          "server LISTAGG grouped aggregate missing dept=10 text result");
  Require(Contains(listagg_decoded.row_packet, "c0=20;c1=true|false"),
          "server LISTAGG grouped aggregate missing dept=20 text result");
  Require(Contains(listagg_decoded.row_packet, "c0=30;c1="),
          "server LISTAGG grouped aggregate missing dept=30 null result");
  Require(Contains(listagg_decoded.row_packet, "row_meta[0]=c0:int64:not_null;c1:text:not_null"),
          "server LISTAGG grouped aggregate missing non-null text metadata");
  Require(Contains(listagg_decoded.row_packet, "row_meta[2]=c0:int64:not_null;c1:text:null"),
          "server LISTAGG grouped aggregate missing null text metadata");
  Require(Contains(listagg_decoded.row_packet, "evidence=query_aggregate:listagg_by_key"),
          "server LISTAGG grouped aggregate did not report LISTAGG evidence");
  Require(Contains(listagg_decoded.row_packet,
                   "evidence=query_aggregate_typed_result:text_nullable"),
          "server LISTAGG grouped aggregate did not report typed text aggregate evidence");
  Require(Contains(listagg_decoded.row_packet,
                   "evidence=query_aggregate_order_binding:descriptor_field"),
          "server LISTAGG grouped aggregate did not report descriptor order binding");
  Require(Contains(listagg_decoded.row_packet, "evidence=query_relation_count:1"),
          "server LISTAGG grouped aggregate did not report one engine relation");

  auto string_agg_route = MakeServerRoute(database_path, context);
  auto string_agg_execute = server::HandleExecuteSblr(
      &string_agg_route.registry,
      string_agg_route.engine_state,
      ExecuteFrame(string_agg_route.session_uuid, PublicStringAggEnvelope()));
  if (!string_agg_execute.accepted) {
    std::cerr << "server public ABI STRING_AGG grouped aggregate rejected\n";
    for (const auto& diagnostic : string_agg_execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(string_agg_execute.accepted,
          "server public ABI STRING_AGG grouped aggregate was rejected");
  const auto string_agg_decoded = DecodeServerExecuteResult(string_agg_execute.payload);
  Require(string_agg_decoded.outcome == "accepted",
          "server STRING_AGG grouped aggregate outcome mismatch");
  Require(string_agg_decoded.operation_id == "query.plan_operation",
          "server STRING_AGG grouped aggregate operation mismatch");
  Require(string_agg_decoded.row_count == 3,
          "server STRING_AGG grouped aggregate row count mismatch");
  Require(Contains(string_agg_decoded.row_packet, "result_kind=query_rowset"),
          "server STRING_AGG grouped aggregate did not return a query rowset");
  Require(Contains(string_agg_decoded.row_packet, "c0=10;c1=true|true"),
          "server STRING_AGG grouped aggregate missing dept=10 text result");
  Require(Contains(string_agg_decoded.row_packet, "c0=20;c1=true|false"),
          "server STRING_AGG grouped aggregate missing dept=20 text result");
  Require(Contains(string_agg_decoded.row_packet, "c0=30;c1="),
          "server STRING_AGG grouped aggregate missing dept=30 null result");
  Require(Contains(string_agg_decoded.row_packet, "row_meta[0]=c0:int64:not_null;c1:text:not_null"),
          "server STRING_AGG grouped aggregate missing non-null text metadata");
  Require(Contains(string_agg_decoded.row_packet, "row_meta[2]=c0:int64:not_null;c1:text:null"),
          "server STRING_AGG grouped aggregate missing null text metadata");
  Require(Contains(string_agg_decoded.row_packet, "evidence=query_aggregate:string_agg_by_key"),
          "server STRING_AGG grouped aggregate did not report STRING_AGG evidence");
  Require(Contains(string_agg_decoded.row_packet,
                   "evidence=query_aggregate_typed_result:text_nullable"),
          "server STRING_AGG grouped aggregate did not report typed text aggregate evidence");
  Require(Contains(string_agg_decoded.row_packet,
                   "evidence=query_aggregate_order_binding:descriptor_field"),
          "server STRING_AGG grouped aggregate did not report descriptor order binding");
  Require(Contains(string_agg_decoded.row_packet,
                   "evidence=query_aggregate_listagg_separator:|"),
          "server STRING_AGG grouped aggregate did not report delimiter evidence");
  Require(Contains(string_agg_decoded.row_packet, "evidence=query_relation_count:1"),
          "server STRING_AGG grouped aggregate did not report one engine relation");

  auto json_agg_route = MakeServerRoute(database_path, context);
  auto json_agg_execute = server::HandleExecuteSblr(
      &json_agg_route.registry,
      json_agg_route.engine_state,
      ExecuteFrame(json_agg_route.session_uuid, PublicJsonAggEnvelope()));
  if (!json_agg_execute.accepted) {
    std::cerr << "server public ABI JSON_AGG grouped aggregate rejected\n";
    for (const auto& diagnostic : json_agg_execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(json_agg_execute.accepted,
          "server public ABI JSON_AGG grouped aggregate was rejected");
  const auto json_agg_decoded = DecodeServerExecuteResult(json_agg_execute.payload);
  Require(json_agg_decoded.outcome == "accepted",
          "server JSON_AGG grouped aggregate outcome mismatch");
  Require(json_agg_decoded.operation_id == "query.plan_operation",
          "server JSON_AGG grouped aggregate operation mismatch");
  Require(json_agg_decoded.row_count == 3,
          "server JSON_AGG grouped aggregate row count mismatch");
  Require(Contains(json_agg_decoded.row_packet, "result_kind=query_rowset"),
          "server JSON_AGG grouped aggregate did not return a query rowset");
  Require(Contains(json_agg_decoded.row_packet, "c0=10;c1=[true,true]"),
          "server JSON_AGG grouped aggregate missing dept=10 json result");
  Require(Contains(json_agg_decoded.row_packet, "c0=20;c1=[true,false]"),
          "server JSON_AGG grouped aggregate missing dept=20 json result");
  Require(Contains(json_agg_decoded.row_packet, "c0=30;c1=[null]"),
          "server JSON_AGG grouped aggregate missing dept=30 json null element result");
  Require(Contains(json_agg_decoded.row_packet, "row_meta[0]=c0:int64:not_null;c1:json:not_null"),
          "server JSON_AGG grouped aggregate missing non-null json metadata");
  Require(Contains(json_agg_decoded.row_packet, "row_meta[2]=c0:int64:not_null;c1:json:not_null"),
          "server JSON_AGG grouped aggregate missing null-element json metadata");
  Require(Contains(json_agg_decoded.row_packet, "evidence=query_aggregate:json_agg_by_key"),
          "server JSON_AGG grouped aggregate did not report JSON_AGG evidence");
  Require(Contains(json_agg_decoded.row_packet,
                   "evidence=query_aggregate_typed_result:json_nullable"),
          "server JSON_AGG grouped aggregate did not report typed json aggregate evidence");
  Require(Contains(json_agg_decoded.row_packet,
                   "evidence=query_aggregate_order_binding:descriptor_field"),
          "server JSON_AGG grouped aggregate did not report descriptor order binding");
  Require(!Contains(json_agg_decoded.row_packet,
                    "evidence=query_aggregate_listagg_separator:"),
          "server JSON_AGG grouped aggregate reported ordered text delimiter evidence");
  Require(Contains(json_agg_decoded.row_packet, "evidence=query_relation_count:1"),
          "server JSON_AGG grouped aggregate did not report one engine relation");

  auto json_object_agg_route = MakeServerRoute(database_path, context);
  auto json_object_agg_execute = server::HandleExecuteSblr(
      &json_object_agg_route.registry,
      json_object_agg_route.engine_state,
      ExecuteFrame(json_object_agg_route.session_uuid, PublicJsonObjectAggEnvelope()));
  if (!json_object_agg_execute.accepted) {
    std::cerr << "server public ABI JSON_OBJECT_AGG grouped aggregate rejected\n";
    for (const auto& diagnostic : json_object_agg_execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(json_object_agg_execute.accepted,
          "server public ABI JSON_OBJECT_AGG grouped aggregate was rejected");
  const auto json_object_agg_decoded = DecodeServerExecuteResult(json_object_agg_execute.payload);
  Require(json_object_agg_decoded.outcome == "accepted",
          "server JSON_OBJECT_AGG grouped aggregate outcome mismatch");
  Require(json_object_agg_decoded.operation_id == "query.plan_operation",
          "server JSON_OBJECT_AGG grouped aggregate operation mismatch");
  Require(json_object_agg_decoded.row_count == 3,
          "server JSON_OBJECT_AGG grouped aggregate row count mismatch");
  Require(Contains(json_object_agg_decoded.row_packet, "result_kind=query_rowset"),
          "server JSON_OBJECT_AGG grouped aggregate did not return a query rowset");
  Require(Contains(json_object_agg_decoded.row_packet, "c0=10;c1={\"1\":true,\"2\":true}"),
          "server JSON_OBJECT_AGG grouped aggregate missing dept=10 json object result");
  Require(Contains(json_object_agg_decoded.row_packet, "c0=20;c1={\"3\":true,\"4\":false}"),
          "server JSON_OBJECT_AGG grouped aggregate missing dept=20 json object result");
  Require(Contains(json_object_agg_decoded.row_packet, "c0=30;c1={\"5\":null}"),
          "server JSON_OBJECT_AGG grouped aggregate missing dept=30 JSON null value result");
  Require(Contains(json_object_agg_decoded.row_packet,
                   "row_meta[0]=c0:int64:not_null;c1:json:not_null"),
          "server JSON_OBJECT_AGG grouped aggregate missing non-null json metadata");
  Require(Contains(json_object_agg_decoded.row_packet,
                   "row_meta[2]=c0:int64:not_null;c1:json:not_null"),
          "server JSON_OBJECT_AGG grouped aggregate missing null-value json metadata");
  Require(Contains(json_object_agg_decoded.row_packet,
                   "evidence=query_aggregate:json_object_agg_by_key"),
          "server JSON_OBJECT_AGG grouped aggregate did not report JSON_OBJECT_AGG evidence");
  Require(Contains(json_object_agg_decoded.row_packet,
                   "evidence=query_aggregate_typed_result:json_nullable"),
          "server JSON_OBJECT_AGG grouped aggregate did not report typed json aggregate evidence");
  Require(Contains(json_object_agg_decoded.row_packet,
                   "evidence=query_aggregate_pair_value_binding:descriptor_field"),
          "server JSON_OBJECT_AGG grouped aggregate did not report value descriptor binding");
  Require(Contains(json_object_agg_decoded.row_packet,
                   "evidence=query_aggregate_order_binding:descriptor_field"),
          "server JSON_OBJECT_AGG grouped aggregate did not report descriptor order binding");
  Require(Contains(json_object_agg_decoded.row_packet,
                   "evidence=query_aggregate_duplicate_key_policy:last_key_wins_by_order"),
          "server JSON_OBJECT_AGG grouped aggregate did not report duplicate-key policy");
  Require(!Contains(json_object_agg_decoded.row_packet,
                    "evidence=query_aggregate_listagg_separator:"),
          "server JSON_OBJECT_AGG grouped aggregate reported ordered text delimiter evidence");
  Require(Contains(json_object_agg_decoded.row_packet, "evidence=query_relation_count:1"),
          "server JSON_OBJECT_AGG grouped aggregate did not report one engine relation");

  auto json_object_dup_route = MakeServerRoute(database_path, context);
  auto json_object_dup_execute = server::HandleExecuteSblr(
      &json_object_dup_route.registry,
      json_object_dup_route.engine_state,
      ExecuteFrame(json_object_dup_route.session_uuid, PublicJsonObjectAggDuplicateEnvelope()));
  Require(json_object_dup_execute.accepted,
          "server public ABI JSON_OBJECT_AGG duplicate-key aggregate was rejected");
  const auto json_object_dup_decoded = DecodeServerExecuteResult(json_object_dup_execute.payload);
  Require(json_object_dup_decoded.outcome == "accepted",
          "server JSON_OBJECT_AGG duplicate-key aggregate outcome mismatch");
  Require(Contains(json_object_dup_decoded.row_packet, "c0=10;c1={\"10\":2}"),
          "server JSON_OBJECT_AGG duplicate-key aggregate did not keep dept=10 last ordered value");
  Require(Contains(json_object_dup_decoded.row_packet, "c0=20;c1={\"20\":4}"),
          "server JSON_OBJECT_AGG duplicate-key aggregate did not keep dept=20 last ordered value");
  Require(Contains(json_object_dup_decoded.row_packet,
                   "evidence=query_aggregate_duplicate_key_policy:last_key_wins_by_order"),
          "server JSON_OBJECT_AGG duplicate-key aggregate did not report last-key-wins policy");

  auto array_agg_route = MakeServerRoute(database_path, context);
  auto array_agg_execute = server::HandleExecuteSblr(
      &array_agg_route.registry,
      array_agg_route.engine_state,
      ExecuteFrame(array_agg_route.session_uuid, PublicArrayAggEnvelope()));
  if (!array_agg_execute.accepted) {
    std::cerr << "server public ABI ARRAY_AGG grouped aggregate rejected\n";
    for (const auto& diagnostic : array_agg_execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(array_agg_execute.accepted,
          "server public ABI ARRAY_AGG grouped aggregate was rejected");
  const auto array_agg_decoded = DecodeServerExecuteResult(array_agg_execute.payload);
  Require(array_agg_decoded.outcome == "accepted",
          "server ARRAY_AGG grouped aggregate outcome mismatch");
  Require(array_agg_decoded.operation_id == "query.plan_operation",
          "server ARRAY_AGG grouped aggregate operation mismatch");
  Require(array_agg_decoded.row_count == 3,
          "server ARRAY_AGG grouped aggregate row count mismatch");
  Require(Contains(array_agg_decoded.row_packet, "result_kind=query_rowset"),
          "server ARRAY_AGG grouped aggregate did not return a query rowset");
  Require(Contains(array_agg_decoded.row_packet, "c0=10;c1=list[boolean:true;boolean:true]"),
          "server ARRAY_AGG grouped aggregate missing dept=10 list result");
  Require(Contains(array_agg_decoded.row_packet, "c0=20;c1=list[boolean:true;boolean:false]"),
          "server ARRAY_AGG grouped aggregate missing dept=20 list result");
  Require(Contains(array_agg_decoded.row_packet, "c0=30;c1=list[NULL]"),
          "server ARRAY_AGG grouped aggregate did not preserve NULL element");
  Require(Contains(array_agg_decoded.row_packet, "row_meta[0]=c0:int64:not_null;c1:list:not_null"),
          "server ARRAY_AGG grouped aggregate missing non-null list metadata");
  Require(Contains(array_agg_decoded.row_packet, "row_meta[2]=c0:int64:not_null;c1:list:not_null"),
          "server ARRAY_AGG grouped aggregate missing null-element list metadata");
  Require(Contains(array_agg_decoded.row_packet, "evidence=query_aggregate:array_agg_by_key"),
          "server ARRAY_AGG grouped aggregate did not report ARRAY_AGG evidence");
  Require(Contains(array_agg_decoded.row_packet,
                   "evidence=query_aggregate_typed_result:list_nullable"),
          "server ARRAY_AGG grouped aggregate did not report typed list evidence");
  Require(Contains(array_agg_decoded.row_packet,
                   "evidence=query_aggregate_array_descriptor:list"),
          "server ARRAY_AGG grouped aggregate did not report list descriptor evidence");
  Require(Contains(array_agg_decoded.row_packet,
                   "evidence=query_aggregate_order_binding:descriptor_field"),
          "server ARRAY_AGG grouped aggregate did not report descriptor order binding");
  Require(!Contains(array_agg_decoded.row_packet,
                    "evidence=query_aggregate_typed_result:json_nullable"),
          "server ARRAY_AGG grouped aggregate substituted json aggregate evidence");
  Require(Contains(array_agg_decoded.row_packet, "evidence=query_relation_count:1"),
          "server ARRAY_AGG grouped aggregate did not report one engine relation");

  auto listagg_truncate_route = MakeServerRoute(database_path, context);
  auto listagg_truncate_execute = server::HandleExecuteSblr(
      &listagg_truncate_route.registry,
      listagg_truncate_route.engine_state,
      ExecuteFrame(listagg_truncate_route.session_uuid, PublicListAggEnvelope(true)));
  if (!listagg_truncate_execute.accepted) {
    std::cerr << "server public ABI LISTAGG truncate grouped aggregate rejected\n";
    for (const auto& diagnostic : listagg_truncate_execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(listagg_truncate_execute.accepted,
          "server public ABI LISTAGG truncate grouped aggregate was rejected");
  const auto listagg_truncate_decoded = DecodeServerExecuteResult(listagg_truncate_execute.payload);
  Require(listagg_truncate_decoded.outcome == "accepted",
          "server LISTAGG truncate grouped aggregate outcome mismatch");
  Require(listagg_truncate_decoded.operation_id == "query.plan_operation",
          "server LISTAGG truncate grouped aggregate operation mismatch");
  Require(listagg_truncate_decoded.row_count == 3,
          "server LISTAGG truncate grouped aggregate row count mismatch");
  Require(Contains(listagg_truncate_decoded.row_packet, "c0=10;c1=true|..."),
          "server LISTAGG truncate missing dept=10 truncated result");
  Require(Contains(listagg_truncate_decoded.row_packet, "c0=20;c1=true|..."),
          "server LISTAGG truncate missing dept=20 truncated result");
  Require(Contains(listagg_truncate_decoded.row_packet,
                   "evidence=query_aggregate_listagg_overflow:truncate"),
          "server LISTAGG truncate did not report overflow policy evidence");
  Require(Contains(listagg_truncate_decoded.row_packet,
                   "evidence=query_aggregate_typed_result:text_nullable"),
          "server LISTAGG truncate did not report typed text aggregate evidence");

  auto every_route = MakeServerRoute(database_path, context);
  auto every_execute = server::HandleExecuteSblr(
      &every_route.registry,
      every_route.engine_state,
      ExecuteFrame(every_route.session_uuid, PublicEveryAggregateEnvelope()));
  if (!every_execute.accepted) {
    std::cerr << "server public ABI every grouped aggregate rejected\n";
    for (const auto& diagnostic : every_execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(every_execute.accepted,
          "server public ABI every grouped aggregate was rejected");
  const auto every_decoded = DecodeServerExecuteResult(every_execute.payload);
  Require(every_decoded.outcome == "accepted",
          "server every grouped aggregate outcome mismatch");
  Require(every_decoded.operation_id == "query.plan_operation",
          "server every grouped aggregate operation mismatch");
  Require(every_decoded.row_count == 3,
          "server every grouped aggregate row count mismatch");
  Require(Contains(every_decoded.row_packet, "result_kind=query_rowset"),
          "server every grouped aggregate did not return a query rowset");
  Require(Contains(every_decoded.row_packet, "c0=10;c1=true"),
          "server every grouped aggregate missing dept=10 true result");
  Require(Contains(every_decoded.row_packet, "c0=20;c1=false"),
          "server every grouped aggregate missing dept=20 false result");
  Require(Contains(every_decoded.row_packet, "c0=30;c1="),
          "server every grouped aggregate missing dept=30 null result");
  Require(Contains(every_decoded.row_packet, "row_meta[0]=c0:int64:not_null;c1:boolean:not_null"),
          "server every grouped aggregate missing non-null boolean metadata");
  Require(Contains(every_decoded.row_packet, "row_meta[2]=c0:int64:not_null;c1:boolean:null"),
          "server every grouped aggregate missing null boolean metadata");
  Require(Contains(every_decoded.row_packet, "evidence=query_aggregate:every_by_key"),
          "server every grouped aggregate did not report every evidence");
  Require(Contains(every_decoded.row_packet,
                   "evidence=query_aggregate_typed_result:boolean_nullable"),
          "server every grouped aggregate did not report typed boolean aggregate evidence");
  Require(Contains(every_decoded.row_packet, "evidence=query_relation_count:1"),
          "server every grouped aggregate did not report one engine relation");

  struct BooleanAggregateCase {
    std::string_view function;
    std::initializer_list<std::string_view> expected_fragments;
  };
  const BooleanAggregateCase boolean_cases[] = {
      {"sb.aggregate.bool_and",
       {"c0=10;c1=true",
        "c0=20;c1=false",
        "c0=30;c1=",
        "evidence=query_aggregate:bool_and_by_key"}},
      {"sb.aggregate.bool_or",
       {"c0=10;c1=true",
        "c0=20;c1=true",
        "c0=30;c1=",
        "evidence=query_aggregate:bool_or_by_key"}},
  };
  for (const auto& test : boolean_cases) {
    auto bool_route = MakeServerRoute(database_path, context);
    auto bool_execute = server::HandleExecuteSblr(
        &bool_route.registry,
        bool_route.engine_state,
        ExecuteFrame(bool_route.session_uuid, PublicBooleanAggregateEnvelope(test.function)));
    if (!bool_execute.accepted) {
      std::cerr << "server public ABI boolean grouped aggregate rejected for "
                << test.function << '\n';
      for (const auto& diagnostic : bool_execute.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << field.key << '=' << field.value << '\n';
        }
      }
    }
    Require(bool_execute.accepted,
            "server public ABI boolean grouped aggregate was rejected");
    const auto bool_decoded = DecodeServerExecuteResult(bool_execute.payload);
    Require(bool_decoded.outcome == "accepted",
            "server boolean grouped aggregate outcome mismatch");
    Require(bool_decoded.operation_id == "query.plan_operation",
            "server boolean grouped aggregate operation mismatch");
    Require(bool_decoded.row_count == 3,
            "server boolean grouped aggregate row count mismatch");
    Require(Contains(bool_decoded.row_packet, "result_kind=query_rowset"),
            "server boolean grouped aggregate did not return a query rowset");
    for (const auto expected : test.expected_fragments) {
      if (!Contains(bool_decoded.row_packet, std::string(expected))) {
        std::cerr << "server boolean aggregate function=" << test.function
                  << " missing fragment=" << expected
                  << "\npacket:\n" << bool_decoded.row_packet << '\n';
      }
      Require(Contains(bool_decoded.row_packet, std::string(expected)),
              "server boolean grouped aggregate missing expected fragment");
    }
    Require(Contains(bool_decoded.row_packet,
                     "evidence=query_aggregate_typed_result:boolean_nullable"),
            "server boolean grouped aggregate did not report typed boolean aggregate evidence");
    Require(Contains(bool_decoded.row_packet, "evidence=query_relation_count:1"),
            "server boolean grouped aggregate did not report one engine relation");
  }
}

void RequireTableCountThroughServer(const std::filesystem::path& database_path,
                                    const api::EngineRequestContext& context) {
  auto route = MakeServerRoute(database_path, context);
  auto execute = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrame(route.session_uuid, PublicTableCountEnvelope()));
  if (!execute.accepted) {
    std::cerr << "server public ABI table COUNT rejected\n";
    for (const auto& diagnostic : execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(execute.accepted, "server public ABI table COUNT was rejected");
  const auto decoded = DecodeServerExecuteResult(execute.payload);
  Require(decoded.outcome == "accepted", "server table COUNT outcome mismatch");
  Require(decoded.operation_id == "query.plan_operation",
          "server table COUNT operation mismatch");
  if (decoded.row_count != 1) {
    std::cerr << "server table COUNT row_count=" << decoded.row_count
              << " packet:\n" << decoded.row_packet << '\n';
  }
  Require(decoded.row_count == 1, "server table COUNT row count mismatch");
  Require(Contains(decoded.row_packet, "result_kind=query_rowset"),
          "server table COUNT did not return a query rowset");
  Require(Contains(decoded.row_packet, "c0=3"),
          "server table COUNT missing total row count");
  Require(Contains(decoded.row_packet, "evidence=query_aggregate:count_all"),
          "server table COUNT did not report count_all evidence");
  Require(Contains(decoded.row_packet, "evidence=query_count_input_row_count:3"),
          "server table COUNT did not report input row count");
  Require(Contains(decoded.row_packet, "evidence=query_relation_count:1"),
          "server table COUNT did not report one engine relation");
}

void RequireMaterializedCteThroughServer(const std::filesystem::path& database_path,
                                         const api::EngineRequestContext& context) {
  auto route = MakeServerRoute(database_path, context);
  auto execute = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrame(route.session_uuid, PublicMaterializedCteEnvelope()));
  if (!execute.accepted) {
    std::cerr << "server public ABI materialized CTE rejected\n";
    for (const auto& diagnostic : execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(execute.accepted, "server public ABI materialized CTE was rejected");
  const auto decoded = DecodeServerExecuteResult(execute.payload);
  Require(decoded.outcome == "accepted", "server materialized CTE outcome mismatch");
  Require(decoded.operation_id == "query.plan_operation",
          "server materialized CTE operation mismatch");
  if (decoded.row_count != 3) {
    std::cerr << "server materialized CTE row_count=" << decoded.row_count
              << " packet:\n" << decoded.row_packet << '\n';
  }
  Require(decoded.row_count == 3, "server materialized CTE row count mismatch");
  Require(Contains(decoded.row_packet, "result_kind=query_rowset"),
          "server materialized CTE did not return a query rowset");
  Require(Contains(decoded.row_packet, "c0=1") &&
              Contains(decoded.row_packet, "c0=7") &&
              Contains(decoded.row_packet, "c0=8"),
          "server materialized CTE missing source ids");
  Require(Contains(decoded.row_packet, "evidence=query_cte:materialized"),
          "server materialized CTE did not report engine CTE evidence");
  Require(Contains(decoded.row_packet, "evidence=query_relation_count:1"),
          "server materialized CTE did not report one engine relation");
}

void RequireRecursiveCteThroughServer(const std::filesystem::path& database_path,
                                      const api::EngineRequestContext& context) {
  auto route = MakeServerRoute(database_path, context);
  auto execute = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrame(route.session_uuid, PublicRecursiveCteEnvelope()));
  if (!execute.accepted) {
    std::cerr << "server public ABI recursive CTE rejected\n";
    for (const auto& diagnostic : execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(execute.accepted, "server public ABI recursive CTE was rejected");
  const auto decoded = DecodeServerExecuteResult(execute.payload);
  Require(decoded.outcome == "accepted", "server recursive CTE outcome mismatch");
  Require(decoded.operation_id == "query.plan_operation",
          "server recursive CTE operation mismatch");
  if (decoded.row_count != 3) {
    std::cerr << "server recursive CTE row_count=" << decoded.row_count
              << " packet:\n" << decoded.row_packet << '\n';
  }
  Require(decoded.row_count == 3, "server recursive CTE row count mismatch");
  Require(Contains(decoded.row_packet, "result_kind=query_rowset"),
          "server recursive CTE did not return a query rowset");
  Require(Contains(decoded.row_packet, "c0=1") &&
              Contains(decoded.row_packet, "c0=2") &&
              Contains(decoded.row_packet, "c0=3"),
          "server recursive CTE missing fixed-point values");
  Require(Contains(decoded.row_packet, "evidence=query_cte:recursive_fixed_point_materialized"),
          "server recursive CTE did not report recursive fixed-point evidence");
  Require(Contains(decoded.row_packet, "evidence=query_relation_count:2"),
          "server recursive CTE did not report two engine relations");
}

void RequireScalarSubqueryThroughServer(const std::filesystem::path& database_path,
                                        const api::EngineRequestContext& context) {
  auto route = MakeServerRoute(database_path, context);
  auto execute = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrame(route.session_uuid, PublicScalarSubqueryEnvelope()));
  if (!execute.accepted) {
    std::cerr << "server public ABI scalar subquery rejected\n";
    for (const auto& diagnostic : execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  Require(execute.accepted, "server public ABI scalar subquery was rejected");
  const auto decoded = DecodeServerExecuteResult(execute.payload);
  Require(decoded.outcome == "accepted", "server scalar subquery outcome mismatch");
  Require(decoded.operation_id == "query.plan_operation",
          "server scalar subquery operation mismatch");
  if (decoded.row_count != 1) {
    std::cerr << "server scalar subquery row_count=" << decoded.row_count
              << " packet:\n" << decoded.row_packet << '\n';
  }
  Require(decoded.row_count == 1, "server scalar subquery row count mismatch");
  Require(Contains(decoded.row_packet, "result_kind=query_rowset"),
          "server scalar subquery did not return a query rowset");
  Require(Contains(decoded.row_packet, "c0=1"),
          "server scalar subquery missing first source id");
  Require(Contains(decoded.row_packet, "evidence=query_subquery:scalar_subquery"),
          "server scalar subquery did not report engine subquery evidence");
  Require(Contains(decoded.row_packet, "evidence=query_relation_count:1"),
          "server scalar subquery did not report one engine relation");
}

api::EngineRequestContext BeginTransaction(const std::filesystem::path& database_path,
                                           std::string session_suffix,
                                           std::string isolation = "read_committed") {
  auto begin = Dispatch(database_path,
                        "transaction.begin",
                        "SBLR_TRANSACTION_BEGIN",
                        BaseContext(database_path, session_suffix));
  Require(begin.api_result.local_transaction_id != 0, "transaction begin did not return local id");
  auto context = BaseContext(database_path, std::move(session_suffix));
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
  Require(commit.api_result.ok, "commit failed");
  Require(HasEvidence(commit.api_result, "transaction_state", "committed"),
          "commit evidence missing");
}

void Rollback(const std::filesystem::path& database_path, const api::EngineRequestContext& context) {
  auto rollback = Dispatch(database_path,
                           "transaction.rollback",
                           "SBLR_TRANSACTION_ROLLBACK",
                           context,
                           {},
                           true);
  Require(rollback.api_result.ok, "rollback failed");
  Require(HasEvidence(rollback.api_result, "transaction_state", "rolled_back"),
          "rollback evidence missing");
}

api::EngineApiResult InsertRow(const std::filesystem::path& database_path,
                               const api::EngineRequestContext& context,
                               std::string row_uuid,
                               std::string id,
                               std::string note) {
  return InsertRowIntoTable(database_path, context, kTableUuid, std::move(row_uuid), std::move(id), std::move(note));
}

api::EngineApiResult InsertRowIntoTable(const std::filesystem::path& database_path,
                                        const api::EngineRequestContext& context,
                                        std::string table_uuid,
                                        std::string row_uuid,
                                        std::string id,
                                        std::string note) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::move(table_uuid);
  request.target_object.object_kind = "table";
  request.rows.push_back(Row(std::move(row_uuid), std::move(id), std::move(note)));
  auto inserted = Dispatch(database_path,
                           "dml.insert_rows",
                           "SBLR_DML_INSERT_ROWS",
                           context,
                           request,
                           true);
  Require(inserted.api_result.ok, "insert failed");
  Require(inserted.api_result.result_shape.rows.size() == 1, "insert did not return one row");
  Require(HasEvidence(inserted.api_result, "mga_row_store", "row_insert"),
          "insert MGA row-store evidence missing");
  return inserted.api_result;
}

api::EngineApiResult InsertIdOnlyRowIntoTable(const std::filesystem::path& database_path,
                                              const api::EngineRequestContext& context,
                                              std::string table_uuid,
                                              std::string row_uuid,
                                              std::string id) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::move(table_uuid);
  request.target_object.object_kind = "table";
  request.rows.push_back(IdOnlyRow(std::move(row_uuid), std::move(id)));
  auto inserted = Dispatch(database_path,
                           "dml.insert_rows",
                           "SBLR_DML_INSERT_ROWS",
                           context,
                           request,
                           true);
  Require(inserted.api_result.ok, "id-only insert failed");
  Require(inserted.api_result.result_shape.rows.size() == 1,
          "id-only insert did not return one row");
  Require(HasEvidence(inserted.api_result, "mga_row_store", "row_insert"),
          "id-only insert MGA row-store evidence missing");
  return inserted.api_result;
}

void RequireInsertSourceRowLabeledMgaProof(const std::filesystem::path& database_path,
                                           const api::EngineRequestContext& context) {
  const auto inserted = InsertIdOnlyRowIntoTable(
      database_path, context, kTableUuid, kInsertSourceRow, "0");
  Require(HasEvidence(inserted, "mga_row_store", "row_insert"),
          "SBSQL-FC67CA158753 insert_source MGA row insert proof missing");
  Require(inserted.operation_id == "dml.insert_rows",
          "SBSQL-FC67CA158753 insert_source operation id mismatch");
  Require(FieldValue(inserted, "id") == "0",
          "SBSQL-FC67CA158753 insert_source id-only insert result mismatch");
}

api::EngineApiResult InsertAggregateRowIntoTable(const std::filesystem::path& database_path,
                                                 const api::EngineRequestContext& context,
                                                 std::string table_uuid,
                                                 std::string row_uuid,
                                                 std::string id,
                                                 std::string dept,
                                                 std::string cost) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::move(table_uuid);
  request.target_object.object_kind = "table";
  request.rows.push_back(AggregateRow(std::move(row_uuid),
                                      std::move(id),
                                      std::move(dept),
                                      std::move(cost)));
  auto inserted = Dispatch(database_path,
                           "dml.insert_rows",
                           "SBLR_DML_INSERT_ROWS",
                           context,
                           request,
                           true);
  Require(inserted.api_result.ok, "aggregate insert failed");
  Require(inserted.api_result.result_shape.rows.size() == 1,
          "aggregate insert did not return one row");
  Require(HasEvidence(inserted.api_result, "mga_row_store", "row_insert"),
          "aggregate insert MGA row-store evidence missing");
  return inserted.api_result;
}

api::EngineApiResult InsertBoolAggregateRowIntoTable(const std::filesystem::path& database_path,
                                                     const api::EngineRequestContext& context,
                                                     std::string table_uuid,
                                                     std::string row_uuid,
                                                     std::string id,
                                                     std::string dept,
                                                     std::string flag,
                                                     bool flag_is_null) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::move(table_uuid);
  request.target_object.object_kind = "table";
  request.rows.push_back(BoolAggregateRow(std::move(row_uuid),
                                          std::move(id),
                                          std::move(dept),
                                          std::move(flag),
                                          flag_is_null));
  auto inserted = Dispatch(database_path,
                           "dml.insert_rows",
                           "SBLR_DML_INSERT_ROWS",
                           context,
                           request,
                           true);
  Require(inserted.api_result.ok, "boolean aggregate insert failed");
  Require(inserted.api_result.result_shape.rows.size() == 1,
          "boolean aggregate insert did not return one row");
  Require(HasEvidence(inserted.api_result, "mga_row_store", "row_insert"),
          "boolean aggregate insert MGA row-store evidence missing");
  return inserted.api_result;
}

api::EngineApiResult InsertInt64FieldsRowIntoTable(
    const std::filesystem::path& database_path,
    const api::EngineRequestContext& context,
    std::string table_uuid,
    std::string row_uuid,
    std::initializer_list<std::pair<std::string_view, std::string_view>> fields) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::move(table_uuid);
  request.target_object.object_kind = "table";
  request.rows.push_back(Int64FieldsRow(std::move(row_uuid), fields));
  auto inserted = Dispatch(database_path,
                           "dml.insert_rows",
                           "SBLR_DML_INSERT_ROWS",
                           context,
                           request,
                           true);
  Require(inserted.api_result.ok, "named int64 insert failed");
  Require(inserted.api_result.result_shape.rows.size() == 1,
          "named int64 insert did not return one row");
  Require(HasEvidence(inserted.api_result, "mga_row_store", "row_insert"),
          "named int64 insert MGA row-store evidence missing");
  return inserted.api_result;
}

api::EngineApiResult SelectById(const std::filesystem::path& database_path,
                                const api::EngineRequestContext& context,
                                std::string id) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = kTableUuid;
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
  Require(selected.api_result.ok, "select failed");
  return selected.api_result;
}

void RequireWhereEqualityPredicateRowResult(const std::filesystem::path& database_path,
                                            const api::EngineRequestContext& context) {
  const auto selected = SelectById(database_path, context, "1");
  Require(selected.operation_id == "dml.select_rows",
          "WHERE equality predicate row result operation mismatch");
  Require(selected.result_shape.rows.size() == 1,
          "WHERE equality predicate did not return exactly one MGA-visible row");
  Require(FieldValue(selected, "id") == "1",
          "WHERE equality predicate returned wrong id");
  Require(FieldValue(selected, "note") == "merge-update",
          "WHERE equality predicate returned wrong row payload");
}

api::EngineApiResult UpdateByRowUuid(const std::filesystem::path& database_path,
                                     const api::EngineRequestContext& context,
                                     std::string row_uuid,
                                     std::string note) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = kTableUuid;
  request.target_object.object_kind = "table";
  request.predicate.predicate_kind = "row_uuid_match";
  request.predicate.canonical_predicate_envelope = std::move(row_uuid);
  request.assignments.push_back({"note", TextValue(std::move(note))});
  auto updated = Dispatch(database_path,
                          "dml.update_rows",
                          "SBLR_DML_UPDATE_ROWS",
                          context,
                          request,
                          true);
  Require(updated.api_result.ok, "update failed");
  Require(updated.api_result.result_shape.rows.size() == 1, "update did not return one row");
  Require(HasEvidence(updated.api_result, "mga_row_version", "row_update"),
          "update MGA version evidence missing");
  return updated.api_result;
}

api::EngineApiResult MergeRow(const std::filesystem::path& database_path,
                              const api::EngineRequestContext& context,
                              std::string row_uuid,
                              std::string id,
                              std::string note) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = kTableUuid;
  request.target_object.object_kind = "table";
  request.predicate.predicate_kind = "row_uuid_match";
  request.rows.push_back(Row(std::move(row_uuid), std::move(id), note));
  request.assignments.push_back({"note", TextValue(std::move(note))});
  request.option_envelopes.push_back("update_when_matched:true");
  request.option_envelopes.push_back("insert_when_not_matched:true");
  auto merged = Dispatch(database_path,
                         "dml.merge_rows",
                         "SBLR_DML_MERGE_ROWS",
                         context,
                         request,
                         true);
  Require(merged.api_result.ok, "merge failed");
  Require(merged.api_result.result_shape.rows.size() == 1, "merge did not return one row");
  Require(HasEvidence(merged.api_result, "merge_surface", "matched_update_or_not_matched_insert"),
          "merge surface evidence missing");
  return merged.api_result;
}

api::EngineApiResult DeleteByRowUuid(const std::filesystem::path& database_path,
                                     const api::EngineRequestContext& context,
                                     std::string row_uuid) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = kTableUuid;
  request.target_object.object_kind = "table";
  request.predicate.predicate_kind = "row_uuid_match";
  request.predicate.canonical_predicate_envelope = std::move(row_uuid);
  auto deleted = Dispatch(database_path,
                          "dml.delete_rows",
                          "SBLR_DML_DELETE_ROWS",
                          context,
                          request,
                          true);
  Require(deleted.api_result.ok, "delete failed");
  Require(deleted.api_result.result_shape.rows.size() == 1, "delete did not return one row");
  Require(HasEvidence(deleted.api_result, "mga_row_version", "row_delete_tombstone"),
          "delete tombstone evidence missing");
  return deleted.api_result;
}

api::EngineApiResult ExecuteImportRows(const std::filesystem::path& database_path,
                                       const api::EngineRequestContext& context) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = kTableUuid;
  request.target_object.object_kind = "table";
  request.rows.push_back(Row(kRowD, "4", "copy-exec-a"));
  request.rows.push_back(Row(kRowE, "5", "copy-exec-b"));
  request.option_envelopes.push_back("source_kind:csv_stream");
  request.option_envelopes.push_back("format_family:csv");
  request.option_envelopes.push_back("estimated_row_count:2");
  request.option_envelopes.push_back("reject_mode:reject_row");
  request.option_envelopes.push_back("reject_limit_rows:10");
  request.option_envelopes.push_back("reject_payload_policy:diagnostic_only");
  request.option_envelopes.push_back("resume_policy:fail_closed");
  request.option_envelopes.push_back("checkpoint_mode:disabled");
  auto executed = Dispatch(database_path,
                           "dml.execute_import_rows",
                           "SBLR_DML_EXECUTE_IMPORT_ROWS",
                           context,
                           request,
                           true);
  Require(executed.api_result.ok, "execute import failed");
  Require(HasEvidence(executed.api_result, "import_execution", "delegated_to_dml.insert_rows"),
          "import execution did not delegate to engine insert rows");
  Require(HasEvidence(executed.api_result, "import_reject_model", "reject_row"),
          "import reject-row policy was not normalized");
  Require(HasEvidence(executed.api_result, "import_checkpoint_model", "disabled"),
          "import checkpoint disabled policy was not normalized");
  Require(HasEvidence(executed.api_result, "mga_row_store", "row_insert"),
          "import execution did not reach MGA row-store insert");
  Require(FieldValue(executed.api_result, "note", 0) == "copy-exec-a",
          "first imported row result mismatch");
  Require(FieldValue(executed.api_result, "note", 1) == "copy-exec-b",
          "second imported row result mismatch");
  return executed.api_result;
}

api::EngineApiResult ExecuteRejectedImportRows(const std::filesystem::path& database_path,
                                               const api::EngineRequestContext& context) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = kTableUuid;
  request.target_object.object_kind = "table";
  request.rows.push_back(Row(kRowH, "8", "copy-reject-valid"));
  request.rows.push_back(Row(kRowI, "6", "copy-reject-duplicate"));
  request.option_envelopes.push_back("source_kind:csv_stream");
  request.option_envelopes.push_back("source_fingerprint:sbsfc021-reject-fixture");
  request.option_envelopes.push_back("source_position:row:0");
  request.option_envelopes.push_back("format_family:csv");
  request.option_envelopes.push_back("estimated_row_count:2");
  request.option_envelopes.push_back("reject_mode:reject_row");
  request.option_envelopes.push_back("reject_limit_rows:10");
  request.option_envelopes.push_back("reject_payload_policy:diagnostic_only");
  request.option_envelopes.push_back("resume_policy:fail_closed");
  request.option_envelopes.push_back("checkpoint_mode:disabled");
  auto executed = Dispatch(database_path,
                           "dml.execute_import_rows",
                           "SBLR_DML_EXECUTE_IMPORT_ROWS",
                           context,
                           request,
                           true);
  Require(executed.api_result.ok, "execute rejected import failed");
  Require(HasEvidence(executed.api_result, "import_execution", "delegated_to_dml.insert_rows"),
          "rejected import did not delegate accepted row to engine insert rows");
  Require(HasEvidence(executed.api_result, "import_reject_materialization", "result_shape"),
          "rejected import did not materialize reject row into result shape");
  Require(HasEvidence(executed.api_result, "import_rejected_rows", "1"),
          "rejected import did not count one rejected row");
  Require(executed.api_result.result_shape.rows.size() == 2,
          "rejected import did not return accepted row plus reject diagnostic row");
  Require(FieldValue(executed.api_result, "note", 0) == "copy-reject-valid",
          "rejected import accepted row result mismatch");
  const std::string diagnostic_code = FieldValue(executed.api_result, "diagnostic_code", 1);
  Require(diagnostic_code == "SB_ENGINE_API_INVALID_REQUEST" ||
              diagnostic_code == "CLI.CONSTRAINT_PRIMARY_KEY_VIOLATION" ||
              diagnostic_code == "CLI.CONSTRAINT_UNIQUE_VIOLATION",
          "rejected import diagnostic code mismatch");
  const std::string diagnostic_detail = FieldValue(executed.api_result, "diagnostic_detail", 1);
  Require(Contains(diagnostic_detail, "unique_index_duplicate") ||
              Contains(diagnostic_detail, "duplicate_key"),
          "rejected import diagnostic detail did not describe duplicate key");
  Require(FieldValue(executed.api_result, "value_redacted", 1) == "true",
          "rejected import diagnostic did not mark value redacted");
  return executed.api_result;
}

void CreateSchemaAndTable(const std::filesystem::path& database_path) {
  auto context = BeginTransaction(database_path, "101");

  api::EngineApiRequest schema_request;
  schema_request.target_object.uuid.canonical = kSchemaUuid;
  schema_request.target_object.object_kind = "schema";
  schema_request.localized_names.push_back(Name("sbsfc021_schema"));
  auto schema = Dispatch(database_path,
                         "ddl.create_schema",
                         "SBLR_DDL_CREATE_SCHEMA",
                         context,
                         schema_request,
                         true);
  Require(schema.api_result.ok, "schema create failed");
  Require(schema.api_result.primary_object.uuid.canonical == kSchemaUuid,
          "schema create did not preserve UUID");

  api::EngineApiRequest table_request;
  table_request.target_schema.uuid.canonical = kSchemaUuid;
  table_request.target_schema.object_kind = "schema";
  table_request.target_object.uuid.canonical = kTableUuid;
  table_request.target_object.object_kind = "table";
  table_request.localized_names.push_back(Name("sbsfc021_table"));
  table_request.columns.push_back(Column(0, "id"));
  table_request.columns.push_back(Column(1, "note"));
  table_request.indexes.push_back(UniqueIdIndex());
  auto table = Dispatch(database_path,
                        "ddl.create_table",
                        "SBLR_DDL_CREATE_TABLE",
                        context,
                        table_request,
                        true);
  Require(table.api_result.ok, "table create failed");
  Require(table.api_result.primary_object.uuid.canonical == kTableUuid,
          "table create did not preserve UUID");
  Require(HasEvidence(table.api_result, "mga_relation_metadata", "table_create"),
          "table create MGA metadata evidence missing");

  api::EngineApiRequest query_left_table_request;
  query_left_table_request.target_schema.uuid.canonical = kSchemaUuid;
  query_left_table_request.target_schema.object_kind = "schema";
  query_left_table_request.target_object.uuid.canonical = kQueryLeftTableUuid;
  query_left_table_request.target_object.object_kind = "table";
  query_left_table_request.localized_names.push_back(Name("sbsfc021_query_left"));
  query_left_table_request.columns.push_back(QueryPlanInt64Column(
      0,
      "id",
      "019f2100-0000-7000-8000-000000000700",
      "019f2100-0000-7000-8000-000000000710"));
  auto query_left_table = Dispatch(database_path,
                                   "ddl.create_table",
                                   "SBLR_DDL_CREATE_TABLE",
                                   context,
                                   query_left_table_request,
                                   true);
  Require(query_left_table.api_result.ok, "query left table create failed");
  Require(query_left_table.api_result.primary_object.uuid.canonical == kQueryLeftTableUuid,
          "query left table create did not preserve UUID");
  Require(HasEvidence(query_left_table.api_result, "mga_relation_metadata", "table_create"),
          "query left table create MGA metadata evidence missing");

  api::EngineApiRequest query_right_table_request;
  query_right_table_request.target_schema.uuid.canonical = kSchemaUuid;
  query_right_table_request.target_schema.object_kind = "schema";
  query_right_table_request.target_object.uuid.canonical = kQueryRightTableUuid;
  query_right_table_request.target_object.object_kind = "table";
  query_right_table_request.localized_names.push_back(Name("sbsfc021_query_right"));
  query_right_table_request.columns.push_back(QueryPlanInt64Column(
      0,
      "id",
      "019f2100-0000-7000-8000-000000000720",
      "019f2100-0000-7000-8000-000000000730"));
  auto query_right_table = Dispatch(database_path,
                                    "ddl.create_table",
                                    "SBLR_DDL_CREATE_TABLE",
                                    context,
                                    query_right_table_request,
                                    true);
  Require(query_right_table.api_result.ok, "query right table create failed");
  Require(query_right_table.api_result.primary_object.uuid.canonical == kQueryRightTableUuid,
          "query right table create did not preserve UUID");
  Require(HasEvidence(query_right_table.api_result, "mga_relation_metadata", "table_create"),
          "query right table create MGA metadata evidence missing");

  api::EngineApiRequest aggregate_table_request;
  aggregate_table_request.target_schema.uuid.canonical = kSchemaUuid;
  aggregate_table_request.target_schema.object_kind = "schema";
  aggregate_table_request.target_object.uuid.canonical = kAggregateTableUuid;
  aggregate_table_request.target_object.object_kind = "table";
  aggregate_table_request.localized_names.push_back(Name("sbsfc021_aggregate"));
  aggregate_table_request.columns.push_back(QueryPlanInt64Column(
      0,
      "id",
      "019f2100-0000-7000-8000-000000000740",
      "019f2100-0000-7000-8000-000000000750"));
  aggregate_table_request.columns.push_back(QueryPlanInt64Column(
      1,
      "dept",
      "019f2100-0000-7000-8000-000000000741",
      "019f2100-0000-7000-8000-000000000751"));
  aggregate_table_request.columns.push_back(QueryPlanInt64Column(
      2,
      "cost",
      "019f2100-0000-7000-8000-000000000742",
      "019f2100-0000-7000-8000-000000000752"));
  auto aggregate_table = Dispatch(database_path,
                                  "ddl.create_table",
                                  "SBLR_DDL_CREATE_TABLE",
                                  context,
                                  aggregate_table_request,
                                  true);
  Require(aggregate_table.api_result.ok, "aggregate table create failed");
  Require(aggregate_table.api_result.primary_object.uuid.canonical == kAggregateTableUuid,
          "aggregate table create did not preserve UUID");
  Require(HasEvidence(aggregate_table.api_result, "mga_relation_metadata", "table_create"),
          "aggregate table create MGA metadata evidence missing");

  api::EngineApiRequest bool_aggregate_table_request;
  bool_aggregate_table_request.target_schema.uuid.canonical = kSchemaUuid;
  bool_aggregate_table_request.target_schema.object_kind = "schema";
  bool_aggregate_table_request.target_object.uuid.canonical = kBoolAggregateTableUuid;
  bool_aggregate_table_request.target_object.object_kind = "table";
  bool_aggregate_table_request.localized_names.push_back(Name("sbsfc021_bool_aggregate"));
  bool_aggregate_table_request.columns.push_back(QueryPlanInt64Column(
      0,
      "id",
      "019f2100-0000-7000-8000-000000000743",
      "019f2100-0000-7000-8000-000000000753"));
  bool_aggregate_table_request.columns.push_back(QueryPlanInt64Column(
      1,
      "dept",
      "019f2100-0000-7000-8000-000000000744",
      "019f2100-0000-7000-8000-000000000754"));
  bool_aggregate_table_request.columns.push_back(QueryPlanBoolColumn(
      2,
      "flag",
      "019f2100-0000-7000-8000-000000000745",
      "019f2100-0000-7000-8000-000000000755"));
  auto bool_aggregate_table = Dispatch(database_path,
                                       "ddl.create_table",
                                       "SBLR_DDL_CREATE_TABLE",
                                       context,
                                       bool_aggregate_table_request,
                                       true);
  Require(bool_aggregate_table.api_result.ok, "boolean aggregate table create failed");
  Require(bool_aggregate_table.api_result.primary_object.uuid.canonical == kBoolAggregateTableUuid,
          "boolean aggregate table create did not preserve UUID");
  Require(HasEvidence(bool_aggregate_table.api_result, "mga_relation_metadata", "table_create"),
          "boolean aggregate table create MGA metadata evidence missing");

  api::EngineApiRequest by_name_left_table_request;
  by_name_left_table_request.target_schema.uuid.canonical = kSchemaUuid;
  by_name_left_table_request.target_schema.object_kind = "schema";
  by_name_left_table_request.target_object.uuid.canonical = kByNameLeftTableUuid;
  by_name_left_table_request.target_object.object_kind = "table";
  by_name_left_table_request.localized_names.push_back(Name("sbsfc021_by_name_left"));
  by_name_left_table_request.columns.push_back(QueryPlanInt64Column(
      0,
      "id",
      "019f2100-0000-7000-8000-000000000760",
      "019f2100-0000-7000-8000-000000000770"));
  by_name_left_table_request.columns.push_back(QueryPlanInt64Column(
      1,
      "score",
      "019f2100-0000-7000-8000-000000000761",
      "019f2100-0000-7000-8000-000000000771"));
  auto by_name_left_table = Dispatch(database_path,
                                     "ddl.create_table",
                                     "SBLR_DDL_CREATE_TABLE",
                                     context,
                                     by_name_left_table_request,
                                     true);
  Require(by_name_left_table.api_result.ok, "BY NAME left table create failed");
  Require(by_name_left_table.api_result.primary_object.uuid.canonical == kByNameLeftTableUuid,
          "BY NAME left table create did not preserve UUID");

  api::EngineApiRequest by_name_right_table_request;
  by_name_right_table_request.target_schema.uuid.canonical = kSchemaUuid;
  by_name_right_table_request.target_schema.object_kind = "schema";
  by_name_right_table_request.target_object.uuid.canonical = kByNameRightTableUuid;
  by_name_right_table_request.target_object.object_kind = "table";
  by_name_right_table_request.localized_names.push_back(Name("sbsfc021_by_name_right"));
  by_name_right_table_request.columns.push_back(QueryPlanInt64Column(
      0,
      "score",
      "019f2100-0000-7000-8000-000000000762",
      "019f2100-0000-7000-8000-000000000772"));
  by_name_right_table_request.columns.push_back(QueryPlanInt64Column(
      1,
      "id",
      "019f2100-0000-7000-8000-000000000763",
      "019f2100-0000-7000-8000-000000000773"));
  auto by_name_right_table = Dispatch(database_path,
                                      "ddl.create_table",
                                      "SBLR_DDL_CREATE_TABLE",
                                      context,
                                      by_name_right_table_request,
                                      true);
  Require(by_name_right_table.api_result.ok, "BY NAME right table create failed");
  Require(by_name_right_table.api_result.primary_object.uuid.canonical == kByNameRightTableUuid,
          "BY NAME right table create did not preserve UUID");

  Commit(database_path, context);
}

void VerifyDmlRowEffects(const std::filesystem::path& database_path) {
  auto writer = BeginTransaction(database_path, "201");
  const auto inserted = InsertRow(database_path, writer, kRowA, "1", "alpha");
  Require(FieldValue(inserted, "note") == "alpha", "insert returned wrong note");

  const auto selected_alpha = SelectById(database_path, writer, "1");
  Require(selected_alpha.result_shape.rows.size() == 1, "select did not see inserted row");
  Require(FieldValue(selected_alpha, "note") == "alpha", "select returned wrong inserted note");

  const auto updated = UpdateByRowUuid(database_path, writer, kRowA, "bravo");
  Require(FieldValue(updated, "note") == "bravo", "update returned wrong note");
  const auto selected_bravo = SelectById(database_path, writer, "1");
  Require(FieldValue(selected_bravo, "note") == "bravo", "select did not see update version");

  const auto merge_insert = MergeRow(database_path, writer, kRowB, "2", "merge-insert");
  Require(HasEvidence(merge_insert, "merge_action", "insert"), "merge insert action evidence missing");
  const auto selected_merged_insert = SelectById(database_path, writer, "2");
  Require(selected_merged_insert.result_shape.rows.size() == 1, "merge insert row not visible");
  Require(FieldValue(selected_merged_insert, "note") == "merge-insert",
          "merge insert returned wrong note");

  const auto merge_update = MergeRow(database_path, writer, kRowA, "1", "merge-update");
  Require(HasEvidence(merge_update, "merge_action", "update"), "merge update action evidence missing");
  const auto selected_merged_update = SelectById(database_path, writer, "1");
  Require(FieldValue(selected_merged_update, "note") == "merge-update",
          "merge update version not visible");
  RequireWhereEqualityPredicateRowResult(database_path, writer);
  RequireInsertSourceRowLabeledMgaProof(database_path, writer);

  (void)DeleteByRowUuid(database_path, writer, kRowB);
  const auto selected_deleted = SelectById(database_path, writer, "2");
  Require(selected_deleted.result_shape.rows.empty(), "delete tombstone did not hide row");

  const auto imported = ExecuteImportRows(database_path, writer);
  Require(imported.result_shape.rows.size() == 2, "import execution did not return two rows");
  const auto selected_import_a = SelectById(database_path, writer, "4");
  Require(selected_import_a.result_shape.rows.size() == 1, "imported row A not visible");
  Require(FieldValue(selected_import_a, "note") == "copy-exec-a",
          "imported row A note mismatch");
  const auto selected_import_b = SelectById(database_path, writer, "5");
  Require(selected_import_b.result_shape.rows.size() == 1, "imported row B not visible");
  Require(FieldValue(selected_import_b, "note") == "copy-exec-b",
          "imported row B note mismatch");
  const auto public_import = ExecuteImportRowsThroughServer(database_path, writer);
  Require(public_import.result_shape.rows.size() == 1,
          "server public ABI imported row not visible");
  Require(FieldValue(public_import, "note") == "copy-public-a",
          "server public ABI imported row note mismatch");
  const auto public_import_b = SelectById(database_path, writer, "7");
  Require(public_import_b.result_shape.rows.size() == 1,
          "second server public ABI imported row not visible");
  Require(FieldValue(public_import_b, "note") == "copy-public-b",
          "second server public ABI imported row note mismatch");
  const auto rejected_import = ExecuteRejectedImportRows(database_path, writer);
  Require(rejected_import.result_shape.rows.size() == 2,
          "rejected import result shape mismatch");
  const auto selected_reject_valid = SelectById(database_path, writer, "8");
  Require(selected_reject_valid.result_shape.rows.size() == 1,
          "valid row from rejected import not visible");
  Require(FieldValue(selected_reject_valid, "note") == "copy-reject-valid",
          "valid row from rejected import note mismatch");
  const auto selected_rejected_duplicate = SelectById(database_path, writer, "6");
  Require(selected_rejected_duplicate.result_shape.rows.size() == 1,
          "duplicate id baseline row missing after rejected import");
  Require(FieldValue(selected_rejected_duplicate, "note") == "copy-public-a",
          "duplicate rejected row replaced the baseline row");
  RequireOrderedSelectThroughServer(database_path, writer);
  RequireTopBoundedSelectThroughServer(database_path, writer);
  RequireFetchBoundedSelectThroughServer(database_path, writer);
  (void)InsertIdOnlyRowIntoTable(database_path, writer, kQueryLeftTableUuid, kJoinRowA, "1");
  (void)InsertIdOnlyRowIntoTable(database_path, writer, kQueryLeftTableUuid, kJoinRowB, "7");
  (void)InsertIdOnlyRowIntoTable(database_path, writer, kQueryLeftTableUuid, kJoinRowC, "8");
  (void)InsertIdOnlyRowIntoTable(database_path, writer, kQueryRightTableUuid, kSetRowA, "1");
  (void)InsertIdOnlyRowIntoTable(database_path, writer, kQueryRightTableUuid, kSetRowB, "7");
  (void)InsertIdOnlyRowIntoTable(database_path, writer, kQueryRightTableUuid, kSetRowC, "9");
  (void)InsertAggregateRowIntoTable(database_path, writer, kAggregateTableUuid, kAggRowA, "1", "10", "12");
  (void)InsertAggregateRowIntoTable(database_path, writer, kAggregateTableUuid, kAggRowB, "2", "10", "13");
  (void)InsertAggregateRowIntoTable(database_path, writer, kAggregateTableUuid, kAggRowC, "3", "20", "7");
  (void)InsertBoolAggregateRowIntoTable(database_path, writer, kBoolAggregateTableUuid, kBoolAggRowA, "1", "10", "true");
  (void)InsertBoolAggregateRowIntoTable(database_path, writer, kBoolAggregateTableUuid, kBoolAggRowB, "2", "10", "true");
  (void)InsertBoolAggregateRowIntoTable(database_path, writer, kBoolAggregateTableUuid, kBoolAggRowC, "3", "20", "true");
  (void)InsertBoolAggregateRowIntoTable(database_path, writer, kBoolAggregateTableUuid, kBoolAggRowD, "4", "20", "false");
  (void)InsertBoolAggregateRowIntoTable(database_path, writer, kBoolAggregateTableUuid, kBoolAggRowE, "5", "30", "", true);
  (void)InsertInt64FieldsRowIntoTable(database_path,
                                      writer,
                                      kByNameLeftTableUuid,
                                      kByNameLeftRowA,
                                      {{"id", "1"}, {"score", "10"}});
  (void)InsertInt64FieldsRowIntoTable(database_path,
                                      writer,
                                      kByNameLeftTableUuid,
                                      kByNameLeftRowB,
                                      {{"id", "2"}, {"score", "20"}});
  (void)InsertInt64FieldsRowIntoTable(database_path,
                                      writer,
                                      kByNameRightTableUuid,
                                      kByNameRightRowA,
                                      {{"score", "20"}, {"id", "2"}});
  (void)InsertInt64FieldsRowIntoTable(database_path,
                                      writer,
                                      kByNameRightTableUuid,
                                      kByNameRightRowB,
                                      {{"score", "30"}, {"id", "3"}});
  RequireTableJoinThroughServer(database_path, writer);
  RequireTableSetOperationsThroughServer(database_path, writer);
  RequireTableSetByNameOperationsThroughServer(database_path, writer);
  RequireRowNumberWindowThroughServer(database_path, writer);
  RequirePartitionCountWindowThroughServer(database_path, writer);
  RequireNavigationWindowsThroughServer(database_path, writer);
  RequireGroupByAggregateThroughServer(database_path, writer);
  RequireTableCountThroughServer(database_path, writer);
  RequireMaterializedCteThroughServer(database_path, writer);
  RequireRecursiveCteThroughServer(database_path, writer);
  RequireScalarSubqueryThroughServer(database_path, writer);
  (void)InsertIdOnlyRowIntoTable(database_path, writer, kQueryLeftTableUuid, kJoinRowD, "7");
  (void)InsertIdOnlyRowIntoTable(database_path, writer, kQueryLeftTableUuid, kJoinRowE, "7");
  (void)InsertIdOnlyRowIntoTable(database_path, writer, kQueryRightTableUuid, kSetRowD, "7");
  RequireTableSetAllOperationsThroughServer(database_path, writer);
  Commit(database_path, writer);

  auto reader = BeginTransaction(database_path, "202");
  const auto committed_a = SelectById(database_path, reader, "1");
  Require(committed_a.result_shape.rows.size() == 1, "committed updated row missing");
  Require(FieldValue(committed_a, "note") == "merge-update",
          "committed updated row returned wrong note");
  const auto committed_b = SelectById(database_path, reader, "2");
  Require(committed_b.result_shape.rows.empty(), "deleted row visible after commit");
  const auto committed_import = SelectById(database_path, reader, "4");
  Require(committed_import.result_shape.rows.size() == 1,
          "committed import row missing after commit");
  Require(FieldValue(committed_import, "note") == "copy-exec-a",
          "committed import row returned wrong note");
  const auto committed_public_import = SelectById(database_path, reader, "6");
  Require(committed_public_import.result_shape.rows.size() == 1,
          "committed server public ABI import row missing after commit");
  Require(FieldValue(committed_public_import, "note") == "copy-public-a",
          "committed server public ABI import row returned wrong note");
  const auto committed_reject_valid = SelectById(database_path, reader, "8");
  Require(committed_reject_valid.result_shape.rows.size() == 1,
          "committed valid row from rejected import missing");
  Require(FieldValue(committed_reject_valid, "note") == "copy-reject-valid",
          "committed valid row from rejected import returned wrong note");
  Commit(database_path, reader);

  auto rollback_writer = BeginTransaction(database_path, "203");
  (void)InsertRow(database_path, rollback_writer, kRowC, "3", "rollback-only");
  Rollback(database_path, rollback_writer);
  auto rollback_reader = BeginTransaction(database_path, "204");
  const auto rolled_back = SelectById(database_path, rollback_reader, "3");
  Require(rolled_back.result_shape.rows.empty(), "rolled-back DML row became visible");
  Commit(database_path, rollback_reader);
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  const auto work = MakeTempDir();
  Require(!work.empty(), "failed to create temp directory");
  const auto database_path = work / "sbsfc021.sbdb";

  api::EngineCreateLifecycleRequest create;
  create.context = BaseContext(database_path);
  create.option_envelopes.push_back(std::string("resource_seed_pack_root:") +
                                    SB_SBSFC021_SEED_PACK_ROOT);
  auto created = api::EngineCreateLifecycle(create);
  Require(created.ok, "lifecycle create database failed");
  Require(std::filesystem::exists(database_path), "lifecycle create did not create database file");

  auto open = Dispatch(database_path,
                       "lifecycle.open_database",
                       "SBLR_LIFECYCLE_OPEN_DATABASE",
                       BaseContext(database_path));
  Require(open.api_result.ok, "lifecycle open failed");

  CreateSchemaAndTable(database_path);
  VerifyDmlRowEffects(database_path);

  std::cout << "sbsql_dml_mga_row_result_conformance=passed\n";
  return EXIT_SUCCESS;
}
