// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "database_lifecycle.hpp"
#include "datatype_catalog_manifest.hpp"
#include "dml/select_api.hpp"
#include "dml/delete_predicate_binding.hpp"
#include "dml/delete_effect_authority_provider.hpp"
#include "dml/delete_durable_owner_registry.hpp"
#include "mga_relation_store/mga_savepoint_store.hpp"
#include "dml/mga_relation_read_view.hpp"
#include "dml/transactional_index_provider.hpp"
#include "dml/update_api.hpp"
#include "dml/update_resource_authority_provider.hpp"
#include "dml/update_text_target_authority_provider.hpp"
#include "dml/update_text_value_preparation.hpp"
#include "hash_digest.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "memory.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_dispatch_server.hpp"
#include "sblr_engine_envelope.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include "transaction/local_commit_publication.hpp"
#include "transaction/transaction_api.hpp"
#include "typed_update_carrier_codec.hpp"

#include "canonical_sblr_admission_test_helper.hpp"
#if defined(SB_DELETE_EXECUTION_WRAP_IO)
#include "dml_delete_io_fault_fixture.hpp"
#endif

#include "../database_lifecycle/credentialed_database_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <string_view>
#include <utility>
#include <unistd.h>
#include <sys/wait.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace memory = scratchbird::core::memory;
namespace sblr = scratchbird::engine::sblr;
namespace server = scratchbird::server;
namespace sbps = scratchbird::server::sbps;
namespace update_wire = scratchbird::wire;

#ifndef SB_SBSFC021_SEED_PACK_ROOT
#define SB_SBSFC021_SEED_PACK_ROOT "project/resources/seed-packs/initial-resource-pack"
#endif

constexpr const char* kDatabaseUuid = "019f2100-0000-7000-8000-000000000001";
std::string g_database_uuid = kDatabaseUuid;
std::string g_principal_uuid =
    "019f2100-0000-7000-8000-000000000002";
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
constexpr const char* kBulkRowA = "019f2100-0000-7000-8000-000000000220";
constexpr const char* kBulkRowB = "019f2100-0000-7000-8000-000000000221";
constexpr const char* kCopyFastRowA = "019f2100-0000-7000-8000-000000000222";
constexpr const char* kCopyFastRowB = "019f2100-0000-7000-8000-000000000223";
constexpr const char* kPublicCopyFastRowA = "019f2100-0000-7000-8000-000000000224";
constexpr const char* kPublicCopyFastRowB = "019f2100-0000-7000-8000-000000000225";
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
  std::string tmpl = (std::filesystem::temp_directory_path() /
                       "sb_sbsfc021_dml_mga.XXXXXX").string();
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
  context.database_uuid.canonical = g_database_uuid;
  context.principal_uuid.canonical = g_principal_uuid;
  context.session_uuid.canonical = "019f2100-0000-7000-8000-000000000" + std::move(session_suffix);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.datatype_catalog_snapshot_uuid.canonical =
      "019d0000-0000-7000-8000-00000000d701";
  context.datatype_catalog_generation = 1;
  context.datatype_registry_generation = 1;
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
      scratchbird::test::sbsql::BuildCanonicalSblrAdmissionRequest(
          operation_id, opcode));
  if (!admission.admitted) {
    std::cerr << "server admission rejected " << operation_id << '\n';
    for (const auto& diagnostic : admission.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    }
  }
  Require(admission.admitted, "server SBLR admission rejected DML row-result fixture");
}

sblr::SblrOperationEnvelope Envelope(std::string operation_id, std::string opcode) {
  auto envelope = scratchbird::test::sbsql::BuildCanonicalEngineSblrEnvelopeForTest(
      operation_id, opcode, "SBSFC-021");
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

void RequestFullPayload(api::EngineApiRequest* request) {
  request->option_envelopes.push_back("result_payload_policy:full_payload");
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
  (void)descriptor_uuid;
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.requested_column_uuid.canonical = std::move(column_uuid);
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_uuid.canonical =
      "019d0000-0000-7000-8000-00000000d711";
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "int64";
  column.descriptor.encoded_descriptor =
      "type=int64;nullable=false;type_uuid="
      "019d0000-0000-7000-8000-00000000d712;"
      "datatype_descriptor_uuid="
      "019d0000-0000-7000-8000-00000000d711";
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
  // v4003 is retained only to prove that the historical textual carrier is
  // rejected as noncanonical.  Accepted execution in this test must use the
  // canonical v4015 statement-context route.
  frame.header.payload_schema_id = 4003;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session_uuid;
  frame.header.session_uuid = session_uuid;
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
  session.connection_uuid = session.session_uuid;
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = database_path.string();
  session.database_uuid = g_database_uuid;
  session.local_transaction_id = local_transaction_override.value_or(context.local_transaction_id);
  session.default_local_transaction_id = session.local_transaction_id;
  session.transaction_uuid = context.transaction_uuid.canonical;
  session.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  if (session.local_transaction_id != 0 && !session.transaction_uuid.empty()) {
    server::ServerTransactionState transaction;
    transaction.local_transaction_id = session.local_transaction_id;
    transaction.transaction_uuid = session.transaction_uuid;
    transaction.snapshot_visible_through_local_transaction_id =
        session.snapshot_visible_through_local_transaction_id;
    session.transactions_by_local_id.emplace(transaction.local_transaction_id,
                                             std::move(transaction));
  }
  route.session_uuid = session.session_uuid;
  route.registry.sessions_by_uuid[server::UuidBytesToText(session.session_uuid)] = session;

  route.engine_state.engine_context_active = true;
  server::HostedDatabaseSnapshot database;
  database.state = server::HostedDatabaseState::kOpen;
  database.database_created = true;
  database.database_open = true;
  database.write_admission_fenced = false;
  database.database_path = database_path.string();
  database.database_uuid = g_database_uuid;
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

std::string PublicCopyFailFastExecuteEnvelope() {
  std::string out = AdmissionEnvelope("dml.execute_import_rows", "SBLR_DML_EXECUTE_IMPORT_ROWS");
  out += "target_object_uuid=";
  out += kTableUuid;
  out += "\n";
  out += "target_object_kind=table\n";
  out += "source_kind=csv_stream\n";
  out += "source_fingerprint=sbsfc021-public-copy-fast-fixture\n";
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
  out += "reject_mode=fail_fast\n";
  out += "reject_limit_rows=0\n";
  out += "reject_payload_policy=diagnostic_only\n";
  out += "resume_policy=fail_closed\n";
  out += "checkpoint_mode=disabled\n";
  out += "operand=row_field\t";
  out += kPublicCopyFastRowA;
  out += "|id\t13\n";
  out += "operand=row_field\t";
  out += kPublicCopyFastRowA;
  out += "|note\tcopy-public-fast-a\n";
  out += "operand=row_field\t";
  out += kPublicCopyFastRowB;
  out += "|id\t14\n";
  out += "operand=row_field\t";
  out += kPublicCopyFastRowB;
  out += "|note\tcopy-public-fast-b\n";
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
                                "retired_sblr_frame_or_text_input",
                                true,
                                false);
  RequirePublicAbiImportRefuses(database_path,
                                context,
                                "default_transaction_not_active",
                                true,
                                true,
                                std::optional<std::uint64_t>{0});

  // The historical text carrier above is refusal-only.  Exercise the same
  // engine import contract through the canonical in-memory dispatch seam;
  // independent COPY process tests cover the authenticated v4015 route.
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = kTableUuid;
  request.target_object.object_kind = "table";
  request.rows.push_back(Row(kRowF, "6", "copy-public-a"));
  request.rows.push_back(Row(kRowG, "7", "copy-public-b"));
  request.option_envelopes.push_back("source_kind:csv_stream");
  request.option_envelopes.push_back("format_family:csv");
  request.option_envelopes.push_back("estimated_row_count:2");
  request.option_envelopes.push_back("reject_mode:reject_row");
  request.option_envelopes.push_back("reject_limit_rows:10");
  request.option_envelopes.push_back("reject_payload_policy:diagnostic_only");
  request.option_envelopes.push_back("resume_policy:fail_closed");
  request.option_envelopes.push_back("checkpoint_mode:disabled");
  RequestFullPayload(&request);
  const auto dispatched = Dispatch(database_path,
                                   "dml.execute_import_rows",
                                   "SBLR_DML_EXECUTE_IMPORT_ROWS",
                                   context,
                                   std::move(request),
                                   true);
  Require(dispatched.api_result.ok,
          "canonical import contract execution failed");
  Require(dispatched.api_result.result_shape.rows.size() == 2,
          "canonical import contract row count mismatch");

  return SelectById(database_path, context, "6");
}

api::EngineApiResult ExecuteFailFastImportRowsThroughServer(
    const std::filesystem::path& database_path,
    const api::EngineRequestContext& context) {
  RequirePublicAbiImportRefuses(database_path,
                                context,
                                "retired_sblr_frame_or_text_input",
                                true,
                                true);
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = kTableUuid;
  request.target_object.object_kind = "table";
  request.rows.push_back(Row(kPublicCopyFastRowA, "13", "copy-public-fast-a"));
  request.rows.push_back(Row(kPublicCopyFastRowB, "14", "copy-public-fast-b"));
  request.option_envelopes.push_back("source_kind:csv_stream");
  request.option_envelopes.push_back("source_fingerprint:sbsfc021-public-copy-fast-fixture");
  request.option_envelopes.push_back("source_position:row:0");
  request.option_envelopes.push_back("format_family:csv");
  request.option_envelopes.push_back("estimated_row_count:2");
  request.option_envelopes.push_back("reject_mode:fail_fast");
  request.option_envelopes.push_back("reject_limit_rows:0");
  request.option_envelopes.push_back("reject_payload_policy:diagnostic_only");
  request.option_envelopes.push_back("resume_policy:fail_closed");
  request.option_envelopes.push_back("checkpoint_mode:disabled");
  RequestFullPayload(&request);
  const auto dispatched = Dispatch(database_path,
                                   "dml.execute_import_rows",
                                   "SBLR_DML_EXECUTE_IMPORT_ROWS",
                                   context,
                                   std::move(request),
                                   true);
  Require(dispatched.api_result.ok,
          "canonical fail-fast import contract execution failed");
  Require(dispatched.api_result.result_shape.rows.size() == 2,
          "canonical fail-fast import contract row count mismatch");

  return SelectById(database_path, context, "13");
}

void RequireOrderedSelectThroughServer(const std::filesystem::path& database_path,
                                       const api::EngineRequestContext& context) {
  auto route = MakeServerRoute(database_path, context);
  auto execute = server::HandleExecuteSblr(
      &route.registry,
      route.engine_state,
      ExecuteFrame(route.session_uuid, PublicOrderedSelectEnvelope()));
  Require(!execute.accepted,
          "retired textual dml.select_rows carrier was unexpectedly admitted");
  bool noncanonical = false;
  for (const auto& diagnostic : execute.diagnostics) {
    noncanonical = noncanonical ||
        diagnostic.code == "SBLR.OPERATION.NONCANONICAL";
  }
  Require(noncanonical,
          "retired textual dml.select_rows carrier did not fail as noncanonical");
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
                        "SBLR_TXN_BEGIN",
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
                         "SBLR_TXN_COMMIT",
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
                           "SBLR_TXN_ROLLBACK",
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
  RequestFullPayload(&request);
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
  RequestFullPayload(&request);
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

api::EngineApiResult InsertBulkRowsIntoTable(const std::filesystem::path& database_path,
                                             const api::EngineRequestContext& context) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = kTableUuid;
  request.target_object.object_kind = "table";
  request.rows.push_back(Row(kBulkRowA, "9", "bulk-direct-a"));
  request.rows.push_back(Row(kBulkRowB, "10", "bulk-direct-b"));
  request.option_envelopes.push_back("sblr.canonical_rowset_shared_shape=true");
  RequestFullPayload(&request);
  auto inserted = Dispatch(database_path,
                           "dml.insert_rows",
                           "SBLR_DML_INSERT_ROWS",
                           context,
                           request,
                           true);
  Require(inserted.api_result.ok, "bulk direct insert failed");
  Require(inserted.api_result.result_shape.rows.size() == 2,
          "bulk direct insert did not return two rows");
  Require(HasEvidence(inserted.api_result,
                      "insert_direct_physical_bulk_route",
                      "selected"),
          "bulk insert did not select direct physical route");
  Require(HasEvidence(inserted.api_result,
                      "transactional_relation_store_route",
                      "normal_dml.direct_physical_bulk_append.v1"),
          "bulk insert bypassed the canonical transactional relation store");
  Require(HasEvidence(inserted.api_result, "direct_physical_bulk_row_count", "2"),
          "bulk insert direct physical row count evidence missing");
  Require(HasEvidence(inserted.api_result, "direct_mga_append", "row_version_batch"),
          "bulk insert direct MGA append evidence missing");
  Require(HasEvidence(inserted.api_result,
                      "shared_rowset_value_batch_ownership",
                      "external_logical_batch"),
          "bulk insert shared-rowset external value batch evidence missing");
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
  RequestFullPayload(&request);
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
  RequestFullPayload(&request);
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
  RequestFullPayload(&request);
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
  (void)database_path;
  // This helper is the independent engine post-state oracle for mutations in
  // this fixture.  There is deliberately no public dml.select_rows opcode:
  // public relational reads are query.execute descriptor DAGs.  Keeping the
  // oracle on the private typed API prevents a retired synthetic opcode from
  // becoming admission authority while preserving the MGA visibility check.
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = kTableUuid;
  request.source_object.object_kind = "table";
  request.select_predicate.predicate_kind = "column_equals";
  request.select_predicate.canonical_predicate_envelope = "id";
  request.select_predicate.bound_values.push_back(TextValue(std::move(id)));
  request.select_projection.canonical_projection_envelopes.push_back("id");
  request.select_projection.canonical_projection_envelopes.push_back("note");
  const auto selected = api::EngineSelectRows(request);
  Require(selected.ok, "select failed");
  Require(HasEvidence(selected,
                      "transactional_relation_store_route",
                      "normal_dml.relation_scan.v1"),
          "select did not traverse the canonical transactional relation store");
  return selected;
}

api::EngineApiResult SelectFromTableById(
    const api::EngineRequestContext& context,
    std::string_view table_uuid,
    std::string id) {
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = std::string(table_uuid);
  request.source_object.object_kind = "table";
  request.select_predicate.predicate_kind = "column_equals";
  request.select_predicate.canonical_predicate_envelope = "id";
  request.select_predicate.bound_values.push_back(TextValue(std::move(id)));
  request.select_projection.canonical_projection_envelopes.push_back("id");
  const auto selected = api::EngineSelectRows(request);
  Require(selected.ok, "typed UPDATE post-state select failed");
  return selected;
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
  RequestFullPayload(&request);
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
  Require(HasEvidence(updated.api_result,
                      "transactional_relation_store_route",
                      "normal_dml.mutation_target_rows.v1"),
          "update did not traverse the canonical transactional relation store");
  return updated.api_result;
}

api::EngineApiResult UpdateIdByRowUuid(
    const std::filesystem::path& database_path,
    const api::EngineRequestContext& context,
    std::string row_uuid,
    std::string id) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = kTableUuid;
  request.target_object.object_kind = "table";
  request.predicate.predicate_kind = "row_uuid_match";
  request.predicate.canonical_predicate_envelope = std::move(row_uuid);
  request.assignments.push_back({"id", TextValue(std::move(id))});
  RequestFullPayload(&request);
  auto updated = Dispatch(database_path,
                          "dml.update_rows",
                          "SBLR_DML_UPDATE_ROWS",
                          context,
                          request,
                          true);
  Require(updated.api_result.ok, "indexed-key update failed");
  Require(updated.api_result.result_shape.rows.size() == 1,
          "indexed-key update did not return one row");
  Require(HasEvidence(updated.api_result,
                      "transactional_index_provider_contract",
                      "mga_native_index_family_v1"),
          "indexed-key update bypassed transactional index provider");
  Require(HasEvidence(updated.api_result,
                      "transactional_index_provider_operation",
                      "PrepareRetireEntry"),
          "indexed-key update did not prepare old-key retirement");
  Require(HasEvidence(updated.api_result,
                      "transactional_index_provider_operation",
                      "PrepareInsertEntry"),
          "indexed-key update did not prepare new-key membership");
  return updated.api_result;
}

const api::CrudIndexRecord& RequireFixtureIndex(
    const api::MgaRelationReadView& state) {
  for (const auto& index : state.indexes) {
    if (index.index_uuid == kIndexUuid) return index;
  }
  Require(false, "transactional index fixture metadata missing");
  return state.indexes.front();
}

api::EnginePredicateEnvelope IdPredicate(std::string id) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_equals";
  predicate.canonical_predicate_envelope = "id";
  predicate.bound_values.push_back(TextValue(std::move(id)));
  return predicate;
}

void RequireTransactionalIndexLifecycle(
    const std::filesystem::path& database_path) {
  auto old_snapshot = BeginTransaction(database_path, "205", "snapshot");
  auto updater = BeginTransaction(database_path, "206");
  const auto updated = UpdateIdByRowUuid(database_path, updater, kRowA, "15");
  Require(FieldValue(updated, "id") == "15",
          "indexed-key update returned the wrong key");
  const auto publication = api::RunLocalCommitPageBarrier(updater);
  Require(publication.ok,
          "transactional index mutation set was rejected before commit");
  bool manifest_insert = false;
  bool manifest_retire = false;
  for (const auto& mutation : publication.mutations) {
    if (mutation.mutation_domain != "index" ||
        mutation.object_identity != kIndexUuid ||
        mutation.physical_identity != "database_page_or_mga_index_segment") {
      continue;
    }
    manifest_insert = manifest_insert || mutation.mutation_kind == "insert";
    manifest_retire = manifest_retire || mutation.mutation_kind == "retire";
  }
  Require(manifest_insert && manifest_retire,
          "publication manifest omitted the transactional index insert/retire pair");
  Commit(database_path, updater);

  auto current_reader = BeginTransaction(database_path, "207");
  auto loaded = api::LoadMgaRelationStoreState(current_reader);
  Require(loaded.ok, "transactional index state reload failed");
  const auto state = api::BuildMgaRelationReadView(std::move(loaded.state));
  const auto& index = RequireFixtureIndex(state);

  api::MgaOrderedBtreeTransactionalIndexProvider old_provider(old_snapshot,
                                                              nullptr);
  const auto old_key = old_provider.ResolveVisibleEntry(
      state, index, IdPredicate("1"));
  Require(old_key.ok && old_key.rows.size() == 1,
          "old snapshot lost the pre-update index key");
  const auto old_new_key = old_provider.ResolveVisibleEntry(
      state, index, IdPredicate("15"));
  Require(old_new_key.ok && old_new_key.rows.empty(),
          "old snapshot observed the post-update index key");

  api::MgaOrderedBtreeTransactionalIndexProvider current_provider(
      current_reader, nullptr);
  const auto current_key = current_provider.ResolveVisibleEntry(
      state, index, IdPredicate("15"));
  Require(current_key.ok && current_key.rows.size() == 1,
          "current snapshot lost the post-update index key");
  const auto current_old_key = current_provider.ResolveVisibleEntry(
      state, index, IdPredicate("1"));
  Require(current_old_key.ok && current_old_key.rows.empty(),
          "current snapshot retained the retired index key");
  const auto validated = current_provider.ValidateAgainstRelation(state, index);
  Require(validated.ok,
          "transactional index did not validate against authoritative rows");

  bool saw_key_change_retire = false;
  bool saw_delete_retire = false;
  for (const auto& entry : state.index_entries) {
    if (entry.index_uuid != kIndexUuid || entry.entry_kind != "retire") {
      continue;
    }
    if (entry.creator_tx == updater.local_transaction_id &&
        entry.row_uuid == kRowA && entry.key_value == "1") {
      saw_key_change_retire = true;
    }
    if (entry.row_uuid == kRowB && entry.key_value == "2") {
      saw_delete_retire = true;
    }
  }
  Require(saw_key_change_retire,
          "key-changing update retire marker was not durable after reload");
  Require(saw_delete_retire,
          "delete retire marker was not durable after reload");

  auto missing_retire_state = state;
  missing_retire_state.index_entries.erase(
      std::remove_if(
          missing_retire_state.index_entries.begin(),
          missing_retire_state.index_entries.end(),
          [&](const auto& entry) {
            return entry.index_uuid == kIndexUuid &&
                   entry.creator_tx == updater.local_transaction_id &&
                   entry.row_uuid == kRowA && entry.key_value == "1" &&
                   entry.entry_kind == "retire";
          }),
      missing_retire_state.index_entries.end());
  const auto missing_retire =
      api::ValidateOrderedBtreeTransactionalIndexMutationSetForCommit(
          updater, missing_retire_state);
  Require(!missing_retire.ok &&
              missing_retire.diagnostic.code ==
                  "INDEX.TRANSACTIONAL_PROVIDER.RETIRE_ENTRY_MISSING",
          "commit validation did not fail closed for a missing old-key retire");

  auto missing_insert_state = state;
  missing_insert_state.index_entries.erase(
      std::remove_if(
          missing_insert_state.index_entries.begin(),
          missing_insert_state.index_entries.end(),
          [&](const auto& entry) {
            return entry.index_uuid == kIndexUuid &&
                   entry.creator_tx == updater.local_transaction_id &&
                   entry.row_uuid == kRowA && entry.key_value == "15" &&
                   entry.entry_kind == "insert";
          }),
      missing_insert_state.index_entries.end());
  const auto missing_insert =
      api::ValidateOrderedBtreeTransactionalIndexMutationSetForCommit(
          updater, missing_insert_state);
  Require(!missing_insert.ok &&
              missing_insert.diagnostic.code ==
                  "INDEX.TRANSACTIONAL_PROVIDER.INSERT_ENTRY_MISSING",
          "commit validation did not fail closed for a missing new-key membership");

  const auto recovered =
      api::MgaOrderedBtreeTransactionalIndexProvider(updater, nullptr)
          .RecoverInterruptedMutation(state);
  Require(recovered.ok && recovered.lifecycle_state == "committed_by_inventory",
          "committed index mutation recovery classification was incorrect");
  const auto published =
      api::MgaOrderedBtreeTransactionalIndexProvider(updater, nullptr)
          .PublishTransaction(state);
  Require(published.ok && published.lifecycle_state == "published_by_inventory",
          "index provider publication did not follow inventory finality");
  Commit(database_path, old_snapshot);
  Commit(database_path, current_reader);

  auto rebuild_context = BeginTransaction(database_path, "208");
  auto rebuild_loaded = api::LoadMgaRelationStoreState(rebuild_context);
  Require(rebuild_loaded.ok, "transactional index rebuild state load failed");
  const auto rebuild_state =
      api::BuildMgaRelationReadView(std::move(rebuild_loaded.state));
  const auto& rebuild_index = RequireFixtureIndex(rebuild_state);
  auto append = api::MgaRelationHotAppendContext(rebuild_context);
  api::MgaOrderedBtreeTransactionalIndexProvider rebuild_provider(
      rebuild_context, &append);
  const auto horizon_refused = rebuild_provider.RebuildFromRelation(
      rebuild_state, rebuild_index, false);
  Require(!horizon_refused.ok &&
              horizon_refused.diagnostic.code ==
                  "INDEX.TRANSACTIONAL_PROVIDER.REBUILD_HORIZON_REQUIRED",
          "index rebuild ignored the old-snapshot cleanup horizon");
  const auto rebuilt = rebuild_provider.RebuildFromRelation(
      rebuild_state, rebuild_index, true);
  Require(rebuilt.ok && rebuilt.rebuilt_entry_count != 0,
          "deterministic index rebuild produced no membership");
  const auto rebuild_flushed = append.FlushIndexEntries();
  Require(!rebuild_flushed.error,
          "deterministic index rebuild did not flush durable entries");
  Commit(database_path, rebuild_context);

  auto reopen_reader = BeginTransaction(database_path, "209");
  auto reopened = api::LoadMgaRelationStoreState(reopen_reader);
  Require(reopened.ok, "transactional index restart/reopen load failed");
  const auto reopened_state =
      api::BuildMgaRelationReadView(std::move(reopened.state));
  const auto& reopened_index = RequireFixtureIndex(reopened_state);
  const auto reopened_validation =
      api::MgaOrderedBtreeTransactionalIndexProvider(reopen_reader, nullptr)
          .ValidateAgainstRelation(reopened_state, reopened_index);
  Require(reopened_validation.ok,
          "rebuilt index failed relation validation after reopen");
  Commit(database_path, reopen_reader);
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
  RequestFullPayload(&request);
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
  Require(HasEvidence(merged.api_result,
                      "transactional_relation_store_route",
                      "normal_dml.constraint_scope.v1"),
          "merge did not traverse the canonical transactional relation store");
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
  RequestFullPayload(&request);
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
  Require(HasEvidence(deleted.api_result,
                      "transactional_relation_store_route",
                      "normal_dml.constraint_scope.v1"),
          "delete did not traverse the canonical transactional relation store");
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
  RequestFullPayload(&request);
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

api::EngineApiResult ExecuteFailFastImportRows(const std::filesystem::path& database_path,
                                               const api::EngineRequestContext& context) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = kTableUuid;
  request.target_object.object_kind = "table";
  request.rows.push_back(Row(kCopyFastRowA, "11", "copy-fast-a"));
  request.rows.push_back(Row(kCopyFastRowB, "12", "copy-fast-b"));
  request.option_envelopes.push_back("source_kind:csv_stream");
  request.option_envelopes.push_back("source_fingerprint:sbsfc021-fast-fixture");
  request.option_envelopes.push_back("source_position:row:0");
  request.option_envelopes.push_back("format_family:csv");
  request.option_envelopes.push_back("estimated_row_count:2");
  request.option_envelopes.push_back("reject_mode:fail_fast");
  request.option_envelopes.push_back("reject_limit_rows:0");
  request.option_envelopes.push_back("reject_payload_policy:diagnostic_only");
  request.option_envelopes.push_back("resume_policy:fail_closed");
  request.option_envelopes.push_back("checkpoint_mode:disabled");
  RequestFullPayload(&request);
  auto executed = Dispatch(database_path,
                           "dml.execute_import_rows",
                           "SBLR_DML_EXECUTE_IMPORT_ROWS",
                           context,
                           request,
                           true);
  Require(executed.api_result.ok, "execute fail-fast import failed");
  Require(HasEvidence(executed.api_result, "import_execution", "direct_physical"),
          "fail-fast import did not select direct physical path");
  Require(HasEvidence(executed.api_result, "import_execution_delegate", "none"),
          "fail-fast import unexpectedly delegated to insert rows");
  Require(HasEvidence(executed.api_result, "direct_physical_bulk_row_count", "2"),
          "fail-fast import direct bulk row count evidence missing");
  Require(HasEvidence(executed.api_result, "mga_row_store", "row_insert"),
          "fail-fast import did not reach MGA row store");
  Require(FieldValue(executed.api_result, "note", 0) == "copy-fast-a",
          "first fail-fast imported row result mismatch");
  Require(FieldValue(executed.api_result, "note", 1) == "copy-fast-b",
          "second fail-fast imported row result mismatch");
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
  RequestFullPayload(&request);
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

std::shared_ptr<api::EngineDmlUpdateResourceGovernorV1> TypedUpdateResourceGovernor() {
  static const auto governor = [] {
    auto result = std::make_shared<api::EngineDmlUpdateResourceGovernorV1>();
    // One full grant across this component fixture. Generation two must reach
    // the actual durable DUBR, independently of the context's resource epoch.
    const api::EngineDmlUpdateResourcePolicyV1 policy{
        1024, 4096, 1048576, 64, 1048576, 16ull * 1024 * 1024, 16ull * 1024 * 1024, 16};
    Require(!result->Configure(policy).error && !result->Configure(policy).error,
            "typed UPDATE resource fixture policy failed");
    return result;
  }();
  return governor;
}

api::EngineRequestContext TypedUpdateStatementContext(
    api::EngineRequestContext context,
    std::string_view suffix, bool publish_native_snapshot = false) {
  context.statement_uuid.canonical =
      "019f2100-0000-7000-8000-0000000008" + std::string(suffix);
  context.statement_receipt_uuid.canonical =
      "019f2100-0000-7000-8000-0000000009" + std::string(suffix);
  context.statement_snapshot_uuid.canonical =
      "019f2100-0000-7000-8000-000000000a" + std::string(suffix);
  context.statement_snapshot_generation = 1;
  context.catalog_epoch_uuid.canonical =
      "019f2100-0000-7000-8000-000000000d" + std::string(suffix);
  context.statement_metadata_snapshot_uuid.canonical =
      "019f2100-0000-7000-8000-000000000b" + std::string(suffix);
  context.statement_metadata_snapshot_engine_owned = true;
  context.datatype_catalog_snapshot_uuid.canonical =
      "019d0000-0000-7000-8000-00000000d701";
  context.datatype_catalog_generation = 1;
  context.datatype_registry_generation = 1;
  context.trace_tags.push_back("private_dml_update_rows_binder");
  const auto security = api::LoadSecurityPrincipalLifecycleState(context);
  if (!security.ok) {
    std::cerr << security.diagnostic.code << ':'
              << security.diagnostic.message_key << ':'
              << security.diagnostic.detail << '\n';
  }
  Require(security.ok && security.state.security_generation != 0 &&
              security.state.policy_generation != 0 &&
              security.state.security_context_generation != 0,
          "typed UPDATE durable security authority was unavailable");
  context.security_epoch = security.state.security_generation;
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      "019f2100-0000-7000-8000-000000000004";
  context.authorization_context.security_context_generation =
      security.state.security_context_generation;
  context.authorization_context.principal_uuid = context.principal_uuid;
  context.authorization_context.security_epoch =
      security.state.security_generation;
  context.authorization_context.policy_epoch =
      security.state.policy_generation;
  context.authorization_context.catalog_generation_id =
      context.catalog_generation_id;
  context.authorization_context.effective_subjects.push_back({context.principal_uuid, "principal"});
  for (const auto& source : security.state.grants) {
    if (source.revoked || source.grantee_uuid != context.principal_uuid.canonical) continue;
    api::EngineMaterializedAuthorizationGrant grant;
    grant.grant_uuid.canonical = source.grant_uuid;
    grant.subject_uuid.canonical = source.grantee_uuid; grant.subject_kind = source.grantee_kind;
    grant.target_uuid.canonical = source.target_object_uuid; grant.right = source.privilege;
    grant.security_epoch = context.security_epoch; grant.deny = source.grant_effect == "deny";
    context.authorization_context.grants.push_back(std::move(grant));
  }
  context.resource_admission_uuid.canonical =
      "019f2100-0000-7000-8000-000000000c" + std::string(suffix);
  if (publish_native_snapshot) {
    api::EnginePublishStatementSnapshotRequest request;
    request.context = context;
    request.context.statement_snapshot_uuid = {};
    const auto published = api::EnginePublishStatementSnapshot(request);
    Require(published.ok, "DELETE fixture MGA statement snapshot publication");
    context.statement_snapshot_uuid = published.statement_snapshot_uuid;
    context.snapshot_visible_through_local_transaction_id =
        published.snapshot_vector.visible_committed_high_watermark;
    context.statement_metadata_snapshot_active_excluded_local_transaction_ids =
        published.snapshot_vector.active_excluded_local_transaction_ids;
    context.statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids =
        published.snapshot_vector.in_doubt_excluded_local_transaction_ids;
  }
  // Trusted internal fixture issuer, not a production fallback. Public
  // receipt issuance and teardown are tested separately through the C ABI.
  static std::vector<std::shared_ptr<api::EngineDmlUpdateResourceReceiptV1>> receipts;
  auto receipt = std::make_shared<api::EngineDmlUpdateResourceReceiptV1>(context, TypedUpdateResourceGovernor());
  context.dml_update_resource_receipt = receipt;
  receipts.push_back(std::move(receipt));
  return context;
}

void RequireTypedUpdateResourceGrants(std::uint64_t expected, std::string_view message) {
  const auto observed = TypedUpdateResourceGovernor()->Observe();
  if (observed.active_grants != expected)
    std::cerr << "resource grants=" << observed.active_grants << " expected=" << expected << '\n';
  Require(observed.active_grants == expected &&
              observed.reserved_canonical_bytes == expected * 16ull * 1024 * 1024, message);
}

void VerifyTextTargetAuthority(const std::filesystem::path& database_path) {
  constexpr const char* text_table_uuid = "019f2100-0000-7000-8000-0000000002e1";
  auto ddl_context = BeginTransaction(database_path, "204");
  api::EngineResolveNameRequest charset_request;
  charset_request.context = ddl_context;
  charset_request.sql_object_reference.expected_object_type = "charset";
  charset_request.sql_object_reference.object_name.raw_text = "utf8";
  const auto charset = api::EngineResolveName(charset_request);
  Require(charset.ok && charset.resource_descriptor.present,
          "canonical TEXT fixture charset resolution failed");
  api::EngineApiRequest table_request;
  table_request.target_schema.uuid.canonical = kSchemaUuid;
  table_request.target_schema.object_kind = "schema";
  table_request.target_object.uuid.canonical = text_table_uuid;
  table_request.target_object.object_kind = "table";
  table_request.localized_names.push_back(Name("text_target_authority"));
  api::EngineColumnDefinition text_column;
  text_column.names.push_back(Name("payload"));
  text_column.nullable = true;
  text_column.descriptor.descriptor_kind = "scalar";
  text_column.descriptor.canonical_type_name = "text";
  text_column.descriptor.encoded_descriptor =
      "type=text;character_length=256;charset_uuid=" +
      charset.resource_descriptor.resource_uuid.canonical + ";collation_uuid=" +
      charset.resource_descriptor.default_collation_uuid.canonical;
  table_request.columns.push_back(text_column);
  text_column.ordinal = 1;
  text_column.names = {Name("required_payload")};
  text_column.nullable = false;
  table_request.columns.push_back(text_column);
  text_column.ordinal = 2;
  text_column.names = {Name("wide_payload")};
  text_column.nullable = true;
  text_column.descriptor.encoded_descriptor =
      "type=text;character_length=65536;charset_uuid=" +
      charset.resource_descriptor.resource_uuid.canonical + ";collation_uuid=" +
      charset.resource_descriptor.default_collation_uuid.canonical;
  table_request.columns.push_back(text_column);
  const auto created = Dispatch(database_path, "ddl.create_table", "SBLR_DDL_CREATE_TABLE",
                                ddl_context, table_request, true);
  Require(created.api_result.ok, "canonical TEXT fixture creation failed");
  Commit(database_path, ddl_context);
  auto transaction = BeginTransaction(database_path, "205");
  auto context = TypedUpdateStatementContext(transaction, "e1");
  const auto relation = api::LoadMgaRelationStorageDescriptor(context, text_table_uuid);
  Require(relation.ok && relation.descriptor.columns.size() == 3,
          "TEXT target fixture relation did not load");
  const auto binary_uuid = [](const std::string& text) {
    const auto parsed = scratchbird::core::uuid::ParseUuid(text);
    Require(parsed.ok(), "TEXT target UUID did not parse");
    scratchbird::engine::sblr::ContextualTextUuidV2 result{};
    std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(), result.begin());
    return result;
  };
  const auto& column = relation.descriptor.columns[0];
  api::MgaTextColumnKeyV2 key{
      binary_uuid(relation.descriptor.relation_uuid.canonical),
      binary_uuid(relation.descriptor.descriptor_uuid.canonical),
      relation.descriptor.descriptor_generation,
      binary_uuid(column.column_uuid.canonical), column.ordinal};
  const auto captured = api::CaptureDmlUpdateTextTargetV2(
      context, key, column.column_generation);
  if (!captured.ok) {
    std::cerr << captured.diagnostic.code << ':' << captured.diagnostic.message_key
              << ':' << captured.diagnostic.detail << '\n';
  }
  Require(captured.ok && captured.handle.valid() && captured.handle.snapshot() != nullptr,
          "sealed MGA TEXT target and live resources failed capture");
  const auto& snapshot = *captured.handle.snapshot();
  Require(snapshot.key == key && snapshot.column_generation == column.column_generation &&
              snapshot.charset.canonical_name == "UTF-8" &&
              snapshot.charset.resource_epoch == context.resource_epoch &&
              snapshot.collation.parent_resource_uuid.canonical ==
                  snapshot.charset.resource_uuid.canonical &&
              !snapshot.descriptor.exact_bytes.empty() &&
              !snapshot.exact_relation_projection.empty(),
          "TEXT target capture did not retain exact descriptor/resource metadata");
  Require(!api::RevalidateDmlUpdateTextTargetV2(context, captured.handle).error,
          "unchanged TEXT target failed revalidation");
  const auto second = api::CaptureDmlUpdateTextTargetV2(context, key, column.column_generation);
  Require(second.ok && second.handle.snapshot()->descriptor.exact_bytes ==
              snapshot.descriptor.exact_bytes &&
              second.handle.snapshot()->exact_relation_projection == snapshot.exact_relation_projection,
          "repeated TEXT selection changed retained canonical metadata");
  for (unsigned field = 0; field < 4; ++field) {
    auto changed = key;
    if (field == 0) changed.relation_uuid = {};
    if (field == 1) changed.relation_descriptor_generation += 1;
    if (field == 2) changed.column_uuid = key.relation_uuid;
    if (field == 3) changed.column_ordinal = 99;
    const auto refused = api::CaptureDmlUpdateTextTargetV2(
        context, changed, column.column_generation);
    Require(!refused.ok && !refused.handle.valid(),
            "wrong TEXT target identity received an authority handle");
  }
  Require(!api::CaptureDmlUpdateTextTargetV2(context, key, column.column_generation + 1).ok,
          "stale TEXT column generation was admitted");
  const auto legacy = api::LoadMgaRelationStorageDescriptor(context, kTableUuid);
  Require(legacy.ok && !legacy.descriptor.columns.empty(), "legacy TEXT fixture did not load");
  const api::MgaTextColumnKeyV2 legacy_key{
      binary_uuid(legacy.descriptor.relation_uuid.canonical),
      binary_uuid(legacy.descriptor.descriptor_uuid.canonical),
      legacy.descriptor.descriptor_generation,
      binary_uuid(legacy.descriptor.columns[0].column_uuid.canonical), 0};
  const auto plain = api::CaptureDmlUpdateTextTargetV2(
      context, legacy_key, legacy.descriptor.columns[0].column_generation);
  Require(plain.ok && !plain.handle.snapshot()->contextual &&
              plain.handle.snapshot()->descriptor.exact_bytes.empty() &&
              !plain.handle.snapshot()->collation.present &&
              plain.handle.snapshot()->charset.canonical_name == "UTF-8" &&
              !plain.handle.snapshot()->exact_relation_projection.empty() &&
              plain.handle.snapshot()->exact_persisted_value_descriptor ==
                  legacy.descriptor.columns[0].value_descriptor.encoded_descriptor,
          "sealed non-comparable TEXT assignment lost its exact authority or invented collation");
  const auto integer_relation = api::LoadMgaRelationStorageDescriptor(context, kQueryLeftTableUuid);
  Require(integer_relation.ok && !integer_relation.descriptor.columns.empty(),
          "non-TEXT target refusal fixture did not load");
  const api::MgaTextColumnKeyV2 integer_key{
      binary_uuid(integer_relation.descriptor.relation_uuid.canonical),
      binary_uuid(integer_relation.descriptor.descriptor_uuid.canonical),
      integer_relation.descriptor.descriptor_generation,
      binary_uuid(integer_relation.descriptor.columns[0].column_uuid.canonical), 0};
  Require(!api::CaptureDmlUpdateTextTargetV2(context, integer_key,
              integer_relation.descriptor.columns[0].column_generation).ok,
          "non-TEXT exact target was admitted as TEXT");
  for (unsigned field = 0; field < 10; ++field) {
    auto changed = context;
    if (field == 0) changed.resource_epoch += 1;
    if (field == 1) changed.statement_receipt_uuid.canonical = transaction.transaction_uuid.canonical;
    if (field == 2) changed.security_context_present = false;
    if (field == 3) changed.authorization_context.policy_epoch += 1;
    if (field == 4) changed.statement_snapshot_generation += 1;
    if (field == 5) changed.snapshot_visible_through_local_transaction_id += 1;
    if (field == 6) changed.statement_metadata_snapshot_visible_through_local_transaction_id += 1;
    if (field == 7) changed.statement_metadata_snapshot_active_excluded_local_transaction_ids.push_back(999);
    if (field == 8) changed.statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids.push_back(999);
    if (field == 9) changed.authorization_context.principal_uuid = transaction.transaction_uuid;
    Require(api::RevalidateDmlUpdateTextTargetV2(changed, captured.handle).error,
            "TEXT target handle was reused across a different authority context");
  }
  auto stale = context;
  stale.resource_epoch += 1;
  Require(!api::CaptureDmlUpdateTextTargetV2(stale, key, column.column_generation).ok,
          "TEXT target capture guessed a resource epoch");
  stale = context;
  stale.local_transaction_id = 0;
  Require(!api::CaptureDmlUpdateTextTargetV2(stale, key, column.column_generation).ok,
          "TEXT target capture admitted an absent transaction");
  for (unsigned field = 0; field < 4; ++field) {
    auto changed = context;
    if (field == 0) changed.cluster_transaction_active = true;
    if (field == 1) changed.route_fence_present = true;
    if (field == 2) changed.authorization_context.principal_uuid = transaction.transaction_uuid;
    if (field == 3) changed.authorization_context.catalog_generation_id += 1;
    const auto refused = api::CaptureDmlUpdateTextTargetV2(changed, key, column.column_generation);
    Require(!refused.ok && !refused.handle.valid(),
            "inconsistent or unsupported TEXT target context received a handle");
  }
  Require(api::RevalidateDmlUpdateTextTargetV2(context, {}).error,
          "empty TEXT target handle was accepted");

  // Canonical payload preparation is engine-owned and non-mutating. It must
  // preserve empty VALUE, SQL NULL and exact unnormalized Unicode separately.
  const auto prepare = [&](api::EngineValueState state,
                           const std::vector<std::uint8_t>& bytes) {
    return api::PrepareDmlUpdateTextValueV2(context, captured.handle, state, bytes);
  };
  Require(snapshot.nullable && snapshot.descriptor.character_limit == 256 &&
              snapshot.descriptor.byte_limit == 1024,
          "TEXT preparation target limits were not exact");
  const auto empty = prepare(api::EngineValueState::value, {});
  const auto null = prepare(api::EngineValueState::sql_null, {});
  Require(empty.ok && null.ok && empty.value.bytes().empty() && null.value.bytes().empty() &&
              empty.value.state() != null.value.state(), "empty TEXT VALUE collapsed into NULL");
  auto required_key = key;
  required_key.column_uuid = binary_uuid(relation.descriptor.columns[1].column_uuid.canonical);
  required_key.column_ordinal = 1;
  const auto required_target = api::CaptureDmlUpdateTextTargetV2(
      context, required_key, relation.descriptor.columns[1].column_generation);
  Require(required_target.ok && !required_target.handle.snapshot()->nullable,
          "non-nullable TEXT fixture did not capture");
  const auto null_refused = api::PrepareDmlUpdateTextValueV2(
      context, required_target.handle, api::EngineValueState::sql_null, {});
  Require(!null_refused.ok && !null_refused.value.valid() &&
              null_refused.diagnostic.code == "CLI.CONSTRAINT_NOT_NULL_VIOLATION",
          "NOT NULL target accepted SQL NULL");
  for (const auto state : {api::EngineValueState::missing, api::EngineValueState::default_requested,
                          api::EngineValueState::unknown, api::EngineValueState::error}) {
    Require(!prepare(state, {}).ok, "non-value TEXT state received prepared authority");
  }
  Require(!prepare(api::EngineValueState::sql_null, {'x'}).ok,
          "SQL NULL with payload was accepted");
  for (const std::vector<std::uint8_t>& malformed : std::vector<std::vector<std::uint8_t>>{
           {0xc0, 0x80}, {0xed, 0xa0, 0x80}, {0xf4, 0x90, 0x80, 0x80}, {0xe2, 0x82}}) {
    const auto refused = prepare(api::EngineValueState::value, malformed);
    Require(!refused.ok && !refused.value.valid() &&
                refused.diagnostic.code == "CTB.TEXT.INVALID_ENCODING",
            "malformed TEXT was not refused before preparation");
  }
  Require(prepare(api::EngineValueState::value, std::vector<std::uint8_t>(256, 'a')).ok,
          "exact TEXT character limit refused");
  const auto too_many = prepare(api::EngineValueState::value, std::vector<std::uint8_t>(257, 'a'));
  Require(!too_many.ok && too_many.diagnostic.code == "CTB.TEXT.LENGTH_EXCEEDED",
          "TEXT character count was treated as a byte limit");
  std::vector<std::uint8_t> maximum;
  for (unsigned i = 0; i < 256; ++i) maximum.insert(maximum.end(), {0xf0, 0x9f, 0x98, 0x80});
  Require(prepare(api::EngineValueState::value, maximum).ok,
          "exact TEXT byte and scalar limits refused");
  maximum.push_back('a');
  Require(!prepare(api::EngineValueState::value, maximum).ok, "over-limit TEXT was truncated");
  const auto oversized = prepare(api::EngineValueState::value, std::vector<std::uint8_t>(65537, 'a'));
  Require(!oversized.ok && oversized.diagnostic.code == "RESOURCE.BUDGET_EXCEEDED",
          "DUAV per-value ceiling was ignored");
  auto wide_key = key;
  wide_key.column_uuid = binary_uuid(relation.descriptor.columns[2].column_uuid.canonical);
  wide_key.column_ordinal = 2;
  const auto wide_target = api::CaptureDmlUpdateTextTargetV2(
      context, wide_key, relation.descriptor.columns[2].column_generation);
  Require(wide_target.ok, "wide TEXT fixture did not capture");
  const std::vector<std::uint8_t> wire_maximum(65536, 'a');
  Require(api::PrepareDmlUpdateTextValueV2(context, wide_target.handle,
              api::EngineValueState::value, wire_maximum).ok,
          "exact DUAV 65536-byte value ceiling was refused");

  {
    memory::MemoryTag pressure_tag;
    pressure_tag.category = memory::MemoryCategory::test_probe;
    pressure_tag.context_id = context.statement_receipt_uuid.canonical;
    pressure_tag.purpose = "text_preparation_budget_refusal_fixture";
    auto pressure = memory::DefaultMemoryManager().AllocateScoped(
        memory::DefaultMemoryManager().policy().per_context_limit_bytes,
        alignof(std::max_align_t), pressure_tag);
    Require(pressure.ok(), "TEXT memory-pressure fixture allocation failed");
    const auto before = memory::DefaultMemoryManager().Snapshot();
    const auto refused = prepare(api::EngineValueState::value, {'a'});
    Require(!refused.ok && !refused.value.valid() &&
                refused.diagnostic.code == "RESOURCE.BUDGET_EXCEEDED",
            "TEXT preparation ignored the live statement memory limit");
    Require(memory::DefaultMemoryManager().Snapshot().current_bytes == before.current_bytes,
            "failed TEXT preparation leaked tracked memory");
  }
  Require(prepare(api::EngineValueState::value, {'a'}).ok,
          "TEXT preparation did not recover after pressure was released");

  const auto baseline = memory::DefaultMemoryManager().Snapshot();
  api::EngineDmlUpdatePreparedTextValueV2 retained;
  {
    std::vector<std::uint8_t> bytes{0, 'e', 0xcc, 0x81, 0xf0, 0x9f, 0x98, 0x80};
    const auto value = prepare(api::EngineValueState::value, bytes);
    Require(value.ok && value.value.scalar_count() == 4 &&
                std::ranges::equal(value.value.bytes(), bytes), "TEXT bytes were normalized or changed");
    retained = value.value;
    bytes[1] = 'x';
    Require(retained.bytes()[1] == 'e', "prepared TEXT borrowed caller memory");
    Require(memory::DefaultMemoryManager().Snapshot().current_bytes == baseline.current_bytes + 8,
            "prepared TEXT payload was not tracked exactly once");
  }
  Require(!api::RevalidateDmlUpdatePreparedTextValueV2(context, retained).error,
          "live prepared TEXT did not revalidate");
  auto wrong_receipt = context;
  wrong_receipt.statement_receipt_uuid = transaction.transaction_uuid;
  Require(api::RevalidateDmlUpdatePreparedTextValueV2(wrong_receipt, retained).error,
          "prepared TEXT crossed statement receipts");
  Require(api::RevalidateDmlUpdatePreparedTextValueV2(context, {}).error,
          "absent prepared TEXT handle was accepted");
  Commit(database_path, transaction);
  Require(api::RevalidateDmlUpdatePreparedTextValueV2(context, retained).error,
          "prepared TEXT remained usable after MGA finality");
  retained = {};
  Require(memory::DefaultMemoryManager().Snapshot().current_bytes == baseline.current_bytes,
          "prepared TEXT payload was not released with its last owner");
  Require(api::RevalidateDmlUpdateTextTargetV2(context, captured.handle).error,
          "TEXT target handle survived transaction finality");
}

api::EngineRequestContext TypedUpdateConsumerContext(
    api::EngineRequestContext context) {
  context.trace_tags.erase(
      std::remove(context.trace_tags.begin(), context.trace_tags.end(),
                  "private_dml_update_rows_binder"),
      context.trace_tags.end());
  context.trace_tags.erase(
      std::remove(context.trace_tags.begin(), context.trace_tags.end(),
                  "private_dml_update_rows_recovery"),
      context.trace_tags.end());
  if (std::find(context.trace_tags.begin(), context.trace_tags.end(),
                "private_dml_update_rows_consumer") ==
      context.trace_tags.end()) {
    context.trace_tags.push_back("private_dml_update_rows_consumer");
  }
  return context;
}

bool BytesNonzero(const std::vector<std::uint8_t>& bytes,
                  std::size_t offset,
                  std::size_t count) {
  return offset <= bytes.size() && count <= bytes.size() - offset &&
         std::any_of(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                     bytes.begin() + static_cast<std::ptrdiff_t>(offset + count),
                     [](std::uint8_t byte) { return byte != 0; });
}

std::array<std::uint8_t, 32> TypedUpdateHash(
    std::string_view domain,
    const std::vector<std::uint8_t>& material) {
  std::vector<std::uint8_t> domain_material(domain.begin(), domain.end());
  domain_material.insert(domain_material.end(), material.begin(),
                         material.end());
  const auto digest =
      scratchbird::core::hash::ComputeSha256Digest(domain_material);
  Require(digest.ok(), "typed UPDATE SHA-256 computation failed");
  return digest.digest;
}

std::vector<std::uint8_t> TypedUpdateEvidenceMaterial(
    const std::vector<std::uint8_t>& durs,
    const std::vector<api::EngineEvidenceReference>& evidence) {
  Require(durs.size() == 256,
          "typed UPDATE evidence material requires DURS256");
  std::vector<std::string> records;
  records.reserve(evidence.size());
  for (const auto& item : evidence) {
    Require(!item.evidence_kind.empty() && !item.evidence_id.empty() &&
                item.evidence_kind.find('=') == std::string::npos &&
                item.evidence_kind.find('\0') == std::string::npos &&
                item.evidence_id.find('\0') == std::string::npos,
            "typed UPDATE executor emitted malformed hash evidence");
    records.push_back(item.evidence_kind + "=" + item.evidence_id);
  }
  std::sort(records.begin(), records.end(),
            [](std::string_view left, std::string_view right) {
              return std::lexicographical_compare(
                  left.begin(), left.end(), right.begin(), right.end(),
                  [](char left_byte, char right_byte) {
                    return static_cast<unsigned char>(left_byte) <
                           static_cast<unsigned char>(right_byte);
                  });
            });
  std::vector<std::uint8_t> material;
  material.reserve(64 + records.size() * 32);
  for (const auto offset : {16U, 40U, 56U, 80U}) {
    material.insert(material.end(), durs.begin() + offset,
                    durs.begin() + offset + 16);
  }
  for (const auto& record : records) {
    material.insert(material.end(), record.begin(), record.end());
    material.push_back(0);
  }
  return material;
}

void RequireTypedUpdateHash(std::string_view domain,
                            const std::vector<std::uint8_t>& material,
                            const std::vector<std::uint8_t>& durs,
                            std::size_t offset,
                            std::string_view message) {
  const auto expected = TypedUpdateHash(domain, material);
  Require(offset <= durs.size() && expected.size() <= durs.size() - offset &&
              std::equal(expected.begin(), expected.end(),
                         durs.begin() + static_cast<std::ptrdiff_t>(offset)),
          message);
}

void VerifyTypedTextUpdateContract(const std::filesystem::path& database_path) {
  constexpr const char* table_uuid = "019f2100-0000-7000-8000-0000000002e1";
  auto transaction = BeginTransaction(database_path, "20b");
  api::EngineApiRequest insert;
  insert.target_object.uuid.canonical = table_uuid;
  insert.target_object.object_kind = "table";
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = "019f2100-0000-7000-8000-0000000002e2";
  row.fields = {{"payload", TextValue("seed")},
                {"required_payload", TextValue("required")},
                {"wide_payload", TextValue("seed")}};
  insert.rows.push_back(row);
  Require(Dispatch(database_path, "dml.insert_rows", "SBLR_DML_INSERT_ROWS",
                   transaction, insert, true).api_result.ok,
          "typed TEXT fixture insert failed");
  Require(!api::CreateMgaSavepointMarker(transaction, "text_outer").error,
          "typed TEXT outer savepoint failed");
  const auto read_for = [&](const api::EngineRequestContext& observer,
                            std::string_view expected, bool null_value) {
    api::EngineSelectRowsRequest request;
    request.context = observer;
    request.source_object.uuid.canonical = table_uuid;
    request.source_object.object_kind = "table";
    request.select_projection.canonical_projection_envelopes = {"wide_payload"};
    const auto selected = api::EngineSelectRows(request);
    Require(selected.ok && selected.result_shape.rows.size() == 1 &&
                selected.result_shape.rows[0].fields.size() == 1,
            "typed TEXT post-state oracle failed");
    const auto& value = selected.result_shape.rows[0].fields[0].second;
    const bool exact_value = null_value ? value.binary_value.empty() : value.encoded_value == expected;
    if (value.isSqlNull() != null_value || !exact_value) {
      std::cerr << "TEXT state oracle: expected_bytes=" << expected.size()
                << " actual_bytes=" << value.encoded_value.size()
                << " expected_null=" << null_value << " actual_null=" << value.isSqlNull()
                << " actual_prefix=" << value.encoded_value.substr(0, 80) << '\n';
    }
    Require(value.isSqlNull() == null_value && exact_value,
            "typed TEXT post-state changed bytes or NULL/VALUE state");
  };
  const auto read = [&](std::string_view expected, bool null_value) {
    read_for(transaction, expected, null_value);
  };
  unsigned ordinal = 0;
  const auto bind = [&](std::string value, std::string type, std::string column) {
    const char digits[] = "0123456789abcdef";
    std::string suffix{"d0"};
    const unsigned identity = 0xd0 + ordinal++;
    Require(identity < 256, "TEXT test receipt identity exhausted");
    suffix[0] = digits[identity >> 4];
    suffix[1] = digits[identity & 15];
    auto context = TypedUpdateStatementContext(transaction, suffix);
    api::EngineDmlUpdateRowsBindingDemandV1 demand;
    demand.authenticated_statement_receipt_uuid = context.statement_receipt_uuid.canonical;
    demand.structural_occurrence_id = 1;
    demand.target_relation_uuid_hint = table_uuid;
    demand.assignments.push_back({1, std::move(column), std::move(value), std::move(type)});
    return std::make_pair(std::move(context), std::move(demand));
  };
  for (const auto& value : std::vector<std::string>{"", std::string("a\0b", 3),
           "e\xcc\x81\xf0\x9f\x98\x80", std::string(12000, 'x')}) {
    auto [context, demand] = bind(value, "text", "wide_payload");
    const auto bound = api::BindDmlUpdateRowsDescriptorV1(context, demand);
    if (!bound.ok) std::cerr << bound.diagnostic.code << ':' << bound.diagnostic.detail << '\n';
    Require(bound.ok, "typed TEXT real binder refused valid bytes");
    const auto executed = api::ExecuteDmlUpdateRowsDescriptorV1(
        TypedUpdateConsumerContext(context), bound.descriptor_ref, 1);
    if (!executed.ok) std::cerr << executed.diagnostic.code << ':' << executed.diagnostic.detail << '\n';
    Require(executed.ok && executed.update_result.updated_count == 1,
            "typed TEXT real executor failed");
    read(value, false);
    Require(!api::RollbackToMgaSavepointMarker(transaction, "text_outer").error,
            "typed TEXT rollback failed");
    read("seed", false);
  }
  {
    auto [context, demand] = bind("", "null", "wide_payload");
    const auto bound = api::BindDmlUpdateRowsDescriptorV1(context, demand);
    Require(bound.ok, "typed TEXT NULL bind failed");
    Require(api::ExecuteDmlUpdateRowsDescriptorV1(
        TypedUpdateConsumerContext(context), bound.descriptor_ref, 1).ok,
        "typed TEXT NULL execution failed");
    read("", true);
    Require(!api::RollbackToMgaSavepointMarker(transaction, "text_outer").error,
            "typed TEXT NULL rollback failed");
    read("seed", false);
  }
  for (const auto& failure : std::vector<std::array<std::string, 4>>{
           {"\xc0\x80", "text", "wide_payload", "CTB.TEXT.INVALID_ENCODING"},
           {"", "null", "required_payload", "CLI.CONSTRAINT_NOT_NULL_VIOLATION"},
           {std::string(257, 'x'), "text", "payload", "CTB.TEXT.LENGTH_EXCEEDED"},
           {"unexpected", "null", "wide_payload", "DML.ASSIGNMENT_SHAPE_INVALID"}}) {
    auto [context, demand] = bind(failure[0], failure[1], failure[2]);
    const auto refused = api::BindDmlUpdateRowsDescriptorV1(context, demand);
    Require(!refused.ok && refused.diagnostic.code == failure[3],
            "typed TEXT invalid assignment did not fail before mutation");
    RequireTypedUpdateResourceGrants(0, "TEXT bind refusal retained a resource grant");
    read("seed", false);
    Require(!api::ValidateMgaSavepointExists(transaction, "text_outer", "test").error,
            "TEXT refusal invalidated the outer savepoint");
  }
  // Real process loss of the binder/executor and its volatile TEXT/resource
  // handles. The surviving process never receives those handles, only the
  // binary16 descriptor reference and generation. Authentication is the
  // trusted component receipt above; this is not a server-login crash test.
  for (const auto point : {api::EngineDmlUpdateRowsTestFaultPointV1::after_durable_intent,
                          api::EngineDmlUpdateRowsTestFaultPointV1::after_prepared_outcome,
                          api::EngineDmlUpdateRowsTestFaultPointV1::after_publication_barrier}) {
    for (const auto& profile : std::vector<std::pair<std::string, bool>>{
             {"", false}, {std::string("a\0b", 3), false},
             {std::string(12000, 'z'), false}, {"", true}}) {
      // Fork must not duplicate a live engine's cached sequence reservations
      // or decoded rows into two writers of the SAME database path. Give each
      // case an isolated durable baseline, never opened for row access in the
      // surviving process. This also prevents one crash case masking another.
      const auto crash_root = database_path.parent_path() / ("text_crash_" + std::to_string(ordinal));
      Require(std::filesystem::create_directory(crash_root), "TEXT crash directory collision");
      for (const auto& entry : std::filesystem::directory_iterator(database_path.parent_path())) {
        if (entry.path().filename().string().starts_with(database_path.filename().string()))
          std::filesystem::copy(entry.path(), crash_root / entry.path().filename(),
                                std::filesystem::copy_options::recursive);
      }
      auto crash_transaction = transaction;
      crash_transaction.database_path = (crash_root / database_path.filename()).string();
      const auto identity_number = 0xd0 + ordinal++;
      const char digits[] = "0123456789abcdef";
      std::string suffix{digits[identity_number >> 4], digits[identity_number & 15]};
      auto context = TypedUpdateStatementContext(crash_transaction, suffix);
      api::EngineDmlUpdateRowsBindingDemandV1 demand;
      demand.authenticated_statement_receipt_uuid = context.statement_receipt_uuid.canonical;
      demand.structural_occurrence_id = 1;
      demand.target_relation_uuid_hint = table_uuid;
      demand.assignments.push_back({1, "wide_payload", profile.first, profile.second ? "null" : "text"});
      int descriptors[2];
      Require(::pipe(descriptors) == 0, "TEXT crash reference pipe failed");
      const auto child = ::fork();
      Require(child >= 0, "TEXT crash fork failed");
      if (child == 0) {
        ::close(descriptors[0]);
        const auto bound = api::BindDmlUpdateRowsDescriptorV1(context, demand);
        if (!bound.ok) ::_exit(81);
        const auto identity = scratchbird::core::uuid::ParseUuid(bound.descriptor_ref.descriptor_uuid);
        if (!identity.ok()) ::_exit(82);
        std::array<std::uint8_t, 24> reference{};
        std::copy(identity.value.bytes.begin(), identity.value.bytes.end(), reference.begin());
        for (unsigned n = 0; n != 8; ++n)
          reference[16 + n] = static_cast<std::uint8_t>(bound.descriptor_ref.descriptor_generation >> (8 * n));
        if (::write(descriptors[1], reference.data(), reference.size()) !=
            static_cast<ssize_t>(reference.size())) ::_exit(83);
        ::close(descriptors[1]);
        api::SetDmlUpdateRowsTestFaultPointV1(point);
        const auto interrupted = api::ExecuteDmlUpdateRowsDescriptorV1(
            TypedUpdateConsumerContext(context), bound.descriptor_ref, 1);
        if (interrupted.ok || interrupted.diagnostic.code != "DML.UPDATE_FAILED") ::_exit(84);
        ::kill(::getpid(), SIGKILL);
        ::_exit(85);
      }
      ::close(descriptors[1]);
      std::array<std::uint8_t, 24> reference{};
      std::size_t received = 0;
      while (received != reference.size()) {
        const auto count = ::read(descriptors[0], reference.data() + received, reference.size() - received);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        received += static_cast<std::size_t>(count);
      }
      ::close(descriptors[0]);
      int status = 0;
      pid_t waited;
      do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
      if (waited != child || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
        std::cerr << "TEXT crash point=" << static_cast<int>(point) << " child_status=" << status << '\n';
      Require(received == reference.size() && waited == child && WIFSIGNALED(status) &&
                  WTERMSIG(status) == SIGKILL, "TEXT executor did not reach the process-kill boundary");
      scratchbird::core::platform::Uuid identity;
      std::copy(reference.begin(), reference.begin() + 16, identity.bytes.begin());
      api::EngineDmlUpdateRowsDescriptorRefV1 descriptor;
      descriptor.descriptor_uuid = scratchbird::core::uuid::UuidToString(identity);
      for (unsigned n = 0; n != 8; ++n)
        descriptor.descriptor_generation |= static_cast<std::uint64_t>(reference[16 + n]) << (8 * n);
      context = TypedUpdateConsumerContext(std::move(context));
      const auto recovered = api::ExecuteDmlUpdateRowsDescriptorV1(context, descriptor, 1);
      const bool published = point == api::EngineDmlUpdateRowsTestFaultPointV1::after_publication_barrier;
      if (recovered.ok != published)
        std::cerr << "TEXT process recovery: " << recovered.diagnostic.code << ':'
                  << recovered.diagnostic.message_key << ':' << recovered.diagnostic.detail << '\n';
      Require(published ? recovered.ok && recovered.immutable_replay &&
                              recovered.update_result.updated_count == 1 :
                          !recovered.ok && recovered.diagnostic.code == "MGA.TRANSACTION.STALE",
              "TEXT process-loss recovery selected the wrong MGA outcome");
      read_for(crash_transaction,
           published ? std::string_view(profile.first) : std::string_view("seed"),
           published && profile.second);
      const auto replay = api::ExecuteDmlUpdateRowsDescriptorV1(context, descriptor, 1);
      Require(replay.ok == recovered.ok && replay.canonical_result_bytes == recovered.canonical_result_bytes,
              "TEXT recovery retry changed the terminal outcome");
      Require(!api::RollbackToMgaSavepointMarker(crash_transaction, "text_outer").error,
              "TEXT process recovery invalidated the outer boundary");
      read_for(crash_transaction, "seed", false);
      RequireTypedUpdateResourceGrants(0, "TEXT recovery fabricated a surviving-process grant");
      std::filesystem::remove_all(crash_root);
    }
  }
  Rollback(database_path, transaction);
}

void VerifyTypedUpdateAllRowsContract(
    const std::filesystem::path& database_path) {
  auto transaction = BeginTransaction(database_path, "20a");
  const auto execute_all = [&](std::string_view suffix,
                               std::uint64_t matched, std::uint64_t updated) {
    auto context = TypedUpdateStatementContext(transaction, suffix);
    api::EngineDmlUpdateRowsBindingDemandV1 demand;
    demand.authenticated_statement_receipt_uuid =
        context.statement_receipt_uuid.canonical;
    demand.structural_occurrence_id = 1;
    demand.target_relation_uuid_hint = kQueryRightTableUuid;
    demand.assignments.push_back({1, "id", "2", "int64"});
    const auto bound = api::BindDmlUpdateRowsDescriptorV1(context, demand);
    Require(bound.ok, "typed all-rows UPDATE bind failed");
    context = TypedUpdateConsumerContext(std::move(context));
    const auto executed = api::ExecuteDmlUpdateRowsDescriptorV1(
        context, bound.descriptor_ref, 1);
    if (!executed.ok) {
      std::cerr << "typed all-rows UPDATE diagnostic: "
                << executed.diagnostic.code << ':'
                << executed.diagnostic.message_key << ':'
                << executed.diagnostic.detail << '\n';
    }
    Require(executed.ok && executed.update_result.matched_count == matched &&
                executed.update_result.updated_count == updated &&
                executed.canonical_result_bytes.size() == 256 &&
                GetU64(executed.canonical_result_bytes, 104) == matched &&
                GetU64(executed.canonical_result_bytes, 112) == updated,
            "typed all-rows UPDATE counts or DURS differ");
    Require(HasEvidence(executed.update_result, "update_predicate_kind",
                        "all_visible_rows"),
            "typed all-rows UPDATE predicate evidence missing");
    for (const auto& evidence : executed.update_result.evidence) {
      Require(!evidence.evidence_kind.empty() && !evidence.evidence_id.empty(),
              "typed all-rows UPDATE emitted empty evidence");
    }
    const auto replay = api::ExecuteDmlUpdateRowsDescriptorV1(
        context, bound.descriptor_ref, 1);
    Require(replay.ok && replay.immutable_replay &&
                replay.canonical_result_bytes == executed.canonical_result_bytes,
            "typed all-rows UPDATE replay was not immutable");
  };
  execute_all("70", 0, 0);
  (void)InsertInt64FieldsRowIntoTable(database_path, transaction,
      kQueryRightTableUuid, "019f2100-0000-7000-8000-000000000270", {{"id", "1"}});
  (void)InsertInt64FieldsRowIntoTable(database_path, transaction,
      kQueryRightTableUuid, "019f2100-0000-7000-8000-000000000271", {{"id", "3"}});
  execute_all("71", 2, 2);
  Require(SelectFromTableById(transaction, kQueryRightTableUuid, "2")
              .result_shape.rows.size() == 2,
          "typed all-rows UPDATE did not change both visible rows");
  execute_all("72", 2, 0);
  Rollback(database_path, transaction);
}

void VerifyPublicDmlResourceShutdown(const api::EngineRequestContext& transaction) {
  namespace bridge = scratchbird::server_engine_bridge;
  sb_engine_handle_t engine = nullptr;
  sb_engine_open_params_v1_t open{};
  open.struct_size = sizeof(open);
  open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  open.database_path_utf8 = transaction.database_path.data();
  open.database_path_size = transaction.database_path.size();
  open.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
  Require(sb_engine_open(&open, &engine, nullptr) == SB_ENGINE_STATUS_OK,
          "shutdown public engine open failed");
  const auto public_uuid = [](const std::string& text) {
    const auto parsed = scratchbird::core::uuid::ParseUuid(text);
    Require(parsed.ok(), "shutdown public UUID conversion failed");
    sb_engine_uuid_t result{};
    std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(), result.bytes);
    return result;
  };
  sb_engine_session_params_v1_t begin{};
  begin.struct_size = sizeof(begin);
  begin.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  begin.effective_user_uuid = public_uuid(transaction.principal_uuid.canonical);
  begin.session_uuid = public_uuid(transaction.session_uuid.canonical);
  begin.default_language_utf8 = "en";
  begin.default_language_size = 2;
  begin.trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
  sb_engine_session_t session = nullptr;
  Require(sb_engine_session_begin(engine, &begin, &session, nullptr) == SB_ENGINE_STATUS_OK,
          "shutdown public session begin failed");
  bridge::StatementContextAcquireRequest acquire;
  // The trusted component's synthetic statement identities are deliberately
  // discarded: this test must obtain fresh authority from the real C ABI.
  auto acquire_context = transaction;
  acquire_context.statement_uuid = {};
  acquire_context.statement_receipt_uuid = {};
  acquire_context.statement_snapshot_uuid = {};
  acquire_context.statement_snapshot_generation = 0;
  acquire_context.statement_metadata_snapshot_uuid = {};
  acquire_context.statement_metadata_snapshot_engine_owned = false;
  acquire_context.catalog_epoch_uuid = {};
  acquire_context.resource_admission_uuid = {};
  acquire_context.dml_update_resource_receipt.reset();
  acquire_context.trace_tags = {"SBSFC-021", "public_shutdown_fixture"};
  acquire_context.optimizer_route_epoch = 1;
  acquire_context.optimizer_route_generation = 1;
  acquire_context.optimizer_memory_budget_bytes = 64 * 1024 * 1024;
  acquire_context.optimizer_maximum_candidate_count = 131072;
  acquire_context.optimizer_maximum_memo_groups = 131072;
  acquire_context.optimizer_maximum_search_steps = 1048576;
  acquire_context.optimizer_maximum_planning_time_ns = 5'000'000'000ull;
  acquire_context.optimizer_spill_allowed = true;
  acquire.engine_context = &acquire_context;
  acquire.exact_transaction_uuid = transaction.transaction_uuid.canonical;
  bridge::StatementContextReceiptHandle handle;
  bridge::StatementContextReceiptView view;
  sb_engine_result_t acquire_result = nullptr;
  const auto acquired = bridge::AcquireStatementContextReceipt(session, &acquire, &handle, &view, &acquire_result);
  if (acquired != SB_ENGINE_STATUS_OK && acquire_result != nullptr) {
    sb_engine_diagnostic_set_view_t errors{};
    if (sb_engine_result_diagnostics(acquire_result, &errors) == SB_ENGINE_STATUS_OK)
      for (std::size_t n = 0; n < errors.diagnostic_count; ++n) {
        const auto& error = errors.diagnostics[n];
        std::cerr << std::string_view(error.symbolic_code.data, error.symbolic_code.size_bytes) << ':'
                  << std::string_view(error.message_key.data, error.message_key.size_bytes) << ':'
                  << std::string_view(error.safe_detail.data, error.safe_detail.size_bytes) << '\n';
      }
  }
  if (acquire_result != nullptr) (void)sb_engine_result_release(acquire_result);
  Require(acquired == SB_ENGINE_STATUS_OK,
          "shutdown actual statement receipt acquisition failed");
  api::EngineRequestContext issued;
  Require(bridge::CopyStatementContextEngineContextV1(handle, &issued, nullptr) == SB_ENGINE_STATUS_OK,
          "shutdown actual statement receipt projection failed");
  const auto receipt = issued.dml_update_resource_receipt.lock();
  Require(static_cast<bool>(receipt), "shutdown public receipt has no runtime governor");
  const auto pending = receipt->Capture(issued);
  Require(pending.ok, "shutdown actual receipt resource capture failed");
  sb_engine_result_t refused = nullptr;
  Require(sb_engine_close(engine, &refused) == SB_ENGINE_STATUS_CONFLICT && refused != nullptr,
          "public close destroyed a runtime with an unpublished in-flight grant");
  sb_engine_diagnostic_set_view_t diagnostics{};
  Require(sb_engine_result_diagnostics(refused, &diagnostics) == SB_ENGINE_STATUS_OK &&
              diagnostics.diagnostic_count == 1 &&
              std::string_view(diagnostics.diagnostics[0].symbolic_code.data,
                               diagnostics.diagnostics[0].symbolic_code.size_bytes) ==
                  "ENGINE.DML.RESOURCE_DRAIN_PENDING",
          "public pending-drain diagnostic drifted");
  (void)sb_engine_result_release(refused);
  Require(!receipt->Capture(issued).ok,
          "pending public shutdown admitted a new resource grant");
  api::EngineRequestContext still_live;
  Require(bridge::CopyStatementContextEngineContextV1(handle, &still_live, nullptr) == SB_ENGINE_STATUS_OK,
          "pending public close invalidated the retryable engine/receipt");
  Require(receipt->ReleaseNoAlloc(issued, pending.handle,
              api::EngineDmlUpdateResourceReleaseV1::abandoned_before_publication) ==
              api::EngineDmlUpdateResourceReleaseCodeV1::released,
          "pending public shutdown could not release proven unpublished work");
  Require(bridge::ReleaseStatementContextReceipt(handle) == SB_ENGINE_STATUS_OK,
          "pending public shutdown could not retire the receipt");
  sb_engine_session_end_params_v1_t end{};
  end.struct_size = sizeof(end);
  end.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  end.rollback_active_transactions = 1;
  end.cancel_open_results = 1;
  Require(sb_engine_session_end(session, &end, nullptr) == SB_ENGINE_STATUS_OK,
          "pending public shutdown could not end its session");
  Require(sb_engine_close(engine, nullptr) == SB_ENGINE_STATUS_OK,
          "public close did not succeed on a drained retry");
}

void VerifyDeleteBindingTransportRefusals() {
  server::ServerSessionRegistry registry;
  server::HostedEngineState engine;
  sbps::Frame frame;
  frame.header.message_type = 744; frame.header.payload_schema_id = 7757;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = sbps::MakeUuidV7Bytes();
  frame.payload.resize(48);
  frame.payload[0] = 'D'; frame.payload[1] = 'D'; frame.payload[2] = 'B'; frame.payload[3] = 'Q';
  frame.payload[4] = 1; frame.payload[8] = 1; frame.payload[24] = 1; frame.payload[32] = 2;
  const auto decoded = sbps::DecodeFrameBytes(sbps::EncodeFrame(frame.header, frame.payload), 65536);
  Require(decoded.ok() && decoded.frame->payload == frame.payload && sbps::IsKnownMessageType(745),
          "DELETE SBPS pair cannot round trip its binary request");
  const auto refused = [&](const sbps::Frame& input, std::string_view code) {
    const auto result = server::HandleCoordinateDmlDeleteRowsBind(&registry, engine, input);
    Require(!result.accepted && result.payload.empty() && result.response_message_type == 60 &&
                result.response_schema_id == 2001 && result.diagnostics.size() == 1 &&
                result.diagnostics.front().code == code,
            "DELETE malformed/foreign request did not produce the correlated diagnostic pair");
  };
  refused(frame, "SECURITY.ACCESS_DENIED");
  for (std::size_t size = 0; size < 48; ++size) {
    auto input = frame; input.payload.resize(size); refused(input, "SBLR.OPERAND_INVALID");
  }
  for (const auto offset : {0u, 1u, 2u, 3u, 4u, 6u, 24u}) {
    auto input = frame; input.payload[offset] ^= 2; refused(input, "SBLR.OPERAND_INVALID");
  }
  for (const auto offset : {8u, 32u}) {
    auto input = frame; std::fill_n(input.payload.begin() + offset, 16, 0);
    refused(input, "SBLR.OPERAND_INVALID");
  }
  auto input = frame; input.header.payload_schema_id = 7758; refused(input, "SBLR.OPERAND_INVALID");
  input = frame; input.header.message_type = 708; refused(input, "SBLR.OPERAND_INVALID");
  input = frame; input.payload.push_back(0); refused(input, "SBLR.OPERAND_INVALID");
  input = frame; input.payload.resize(65537); refused(input, "SBLR.OPERAND_INVALID");
  input = frame; input.payload[6] = 1; refused(input, "SBLR.OPERAND_INVALID");
  input.payload.insert(input.payload.end(), {255, 255, 0, 0, 0, 0, 0, 0, 0, 0});
  refused(input, "SBLR.OPERAND_INVALID");
}

void VerifyTypedDeleteDescriptorContract(const std::filesystem::path& database_path) {
  VerifyDeleteBindingTransportRefusals();
  auto seed = BeginTransaction(database_path, "283");
  api::EngineSecurityGrantPrivilegeRequest privilege;
  privilege.context = TypedUpdateStatementContext(seed, "97");
  // This seed operation uses the explicit embedded fixture bootstrap, not a
  // materialized client authorization (which must never fall back to tags).
  privilege.context.authorization_context = {};
  privilege.context.trust_mode = api::EngineTrustMode::embedded_in_process;
  privilege.context.trace_tags.push_back("security.fixture_trace_authority");
  privilege.context.trace_tags.push_back("right:SEC_GRANT_ADMIN");
  privilege.grant_uuid = "019f2100-0000-7000-8000-000000000284";
  privilege.grantee_uuid = g_principal_uuid; privilege.target_object_uuid = kQueryLeftTableUuid;
  privilege.target_object_kind = "table"; privilege.privilege = "DELETE";
  const auto granted = api::EngineSecurityGrantPrivilege(privilege);
  if (!granted.ok && !granted.diagnostics.empty())
    std::cerr << granted.diagnostics.front().code << ':' << granted.diagnostics.front().detail << '\n';
  Require(granted.ok, "DELETE fixture durable privilege grant");
  Commit(database_path, seed);
  auto transaction = BeginTransaction(database_path, "280");
  (void)InsertInt64FieldsRowIntoTable(database_path, transaction, kQueryLeftTableUuid,
      "019f2100-0000-7000-8000-000000000281", {{"id", "601"}});
  (void)InsertInt64FieldsRowIntoTable(database_path, transaction, kQueryLeftTableUuid,
      "019f2100-0000-7000-8000-000000000282", {{"id", "602"}});
  Require(!api::CreateMgaSavepointMarker(transaction, "issue8_delete_outer").error, "DELETE outer savepoint");
  const auto bind = [&](std::string_view suffix, std::string literal) {
    auto context = TypedUpdateStatementContext(transaction, suffix, true);
    context.trace_tags = {"private_dml_delete_rows_binder"};
    api::EngineDmlDeleteRowsBindingDemandV1 demand;
    demand.authenticated_statement_receipt_uuid = context.statement_receipt_uuid.canonical;
    demand.structural_occurrence_id = 1; demand.target_relation_uuid_hint = kQueryLeftTableUuid;
    demand.predicate_kind = "column_equals"; demand.predicate_column_spelling = "id";
    demand.predicate_literal_spelling = std::move(literal); demand.predicate_literal_type_spelling = "int64";
    const auto bound = api::BindDmlDeleteRowsDescriptorV1(context, demand);
    if (!bound.ok) std::cerr << bound.diagnostic.code << ':' << bound.diagnostic.detail << '\n';
    Require(bound.ok, "ordinary typed DELETE binder failed");
    context.trace_tags = {"private_dml_delete_rows_consumer"};
    return std::pair{context, bound.descriptor_ref};
  };
  auto [context, reference] = bind("90", "601");
  auto execution = api::ExecuteDmlDeleteRowsDescriptorV1(context, reference, 1);
  if (!execution.ok) std::cerr << execution.diagnostic.code << ':' << execution.diagnostic.detail << '\n';
  Require(execution.ok && execution.delete_result.deleted_count == 1 && execution.delete_result.matched_count == 1 &&
              execution.delete_result.dml_summary.rows_changed == 1 && execution.canonical_result_bytes.size() == 256 &&
              SelectFromTableById(transaction, kQueryLeftTableUuid, "601").result_shape.rows.empty(),
          "ordinary typed DELETE did not publish one tombstone");
  auto replay = api::ExecuteDmlDeleteRowsDescriptorV1(context, reference, 1);
  if (!replay.ok) std::cerr << replay.diagnostic.code << ':' << replay.diagnostic.detail << '\n';
  Require(replay.ok && replay.immutable_replay && replay.canonical_result_bytes == execution.canonical_result_bytes &&
              replay.delete_result.dml_summary.rows_changed == 1,
          "DELETE same-receipt replay changed result or reexecuted rows");
  RequireTypedUpdateResourceGrants(0, "DELETE publication did not release original grant");
  Require(!api::RollbackToMgaSavepointMarker(transaction, "issue8_delete_outer").error &&
              SelectFromTableById(transaction, kQueryLeftTableUuid, "601").result_shape.rows.size() == 1,
          "outer rollback did not restore typed DELETE row");
  for (const auto& [suffix, point] : std::vector<std::pair<std::string, api::EngineDmlDeleteRowsTestFaultPointV1>>{
      {"92", api::EngineDmlDeleteRowsTestFaultPointV1::after_native_marker},
      {"93", api::EngineDmlDeleteRowsTestFaultPointV1::after_durable_intent},
      {"94", api::EngineDmlDeleteRowsTestFaultPointV1::after_prepared_outcome},
      {"95", api::EngineDmlDeleteRowsTestFaultPointV1::after_publication_barrier}}) {
    auto [owner, ref] = bind(suffix, "602");
    api::SetDmlDeleteRowsTestFaultPointV1(point);
    auto interrupted = api::ExecuteDmlDeleteRowsDescriptorV1(owner, ref, 1);
    Require(!interrupted.ok, "DELETE fault point was not reached");
    api::ResetDmlDeleteBindingCacheForTestV1();
    const bool published = point == api::EngineDmlDeleteRowsTestFaultPointV1::after_publication_barrier;
    auto recovered = api::ExecuteDmlDeleteRowsDescriptorV1(owner, ref, 1);
    if (published && !recovered.ok) std::cerr << recovered.diagnostic.code << ':' << recovered.diagnostic.detail << '\n';
    Require(recovered.ok == published && (!published || recovered.immutable_replay),
            "DELETE interrupted operation was reexecuted or published outcome was lost");
    RequireTypedUpdateResourceGrants(0, "DELETE recovery leaked original resource grant");
    Require(SelectFromTableById(transaction, kQueryLeftTableUuid, "602").result_shape.rows.size() == (published ? 0u : 1u),
            "DELETE interrupted mutation violated its statement boundary");
    Require(!api::RollbackToMgaSavepointMarker(transaction, "issue8_delete_outer").error,
            "DELETE recovery lost outer user savepoint");
  }
  auto [zero_context, zero_ref] = bind("96", "999999");
  auto zero = api::ExecuteDmlDeleteRowsDescriptorV1(zero_context, zero_ref, 1);
  Require(zero.ok && zero.delete_result.matched_count == 0 && zero.delete_result.deleted_count == 0 &&
              zero.delete_result.dml_summary.rows_changed == 0, "zero-match DELETE did not publish exact zero counts");
  RequireTypedUpdateResourceGrants(0, "zero-match DELETE leaked resources");
#if defined(SB_DELETE_EXECUTION_WRAP_IO)
  unsigned fault_ordinal = 0;
  for (const auto fault : {delete_io_fixture::Fault::native_create_sync,
                          delete_io_fixture::Fault::native_release_sync,
                          delete_io_fixture::Fault::publication_rename,
                          delete_io_fixture::Fault::publication_directory_sync}) {
    auto [owner, ref] = bind("f" + std::to_string(fault_ordinal++), "602");
    delete_io_fixture::Arm(fault, database_path.string());
    const auto result = api::ExecuteDmlDeleteRowsDescriptorV1(owner, ref, 1);
    const bool injected = delete_io_fixture::Disarm();
    if (!injected) std::cerr << "unreached_DELETE_IO_fault=" << static_cast<int>(fault)
                            << " result=" << result.diagnostic.code << ':'
                            << result.diagnostic.detail << '\n';
    Require(injected, "DELETE execution did not reach selected ambiguous I/O boundary");
    const bool published = fault != delete_io_fixture::Fault::native_create_sync;
    if (result.ok != published) std::cerr << result.diagnostic.code << ':' << result.diagnostic.detail << '\n';
    Require(result.ok == published, "DELETE ambiguous I/O misclassified the native barrier");
    RequireTypedUpdateResourceGrants(0, "DELETE ambiguous I/O lost original resource ownership");
    Require(SelectFromTableById(transaction, kQueryLeftTableUuid, "602").result_shape.rows.size() == (published ? 0u : 1u),
            "DELETE ambiguous I/O returned a partial or contradictory result");
    if (published) {
      const auto replayed = api::ExecuteDmlDeleteRowsDescriptorV1(owner, ref, 1);
      Require(replayed.ok && replayed.canonical_result_bytes == result.canonical_result_bytes,
              "DELETE ambiguous publication replay changed result bytes");
    }
    Require(!api::RollbackToMgaSavepointMarker(transaction, "issue8_delete_outer").error,
            "DELETE ambiguous I/O lost the outer savepoint");
  }
#endif
  Rollback(database_path, transaction);
  for (const bool interrupted : {false, true}) {
    transaction = BeginTransaction(database_path, interrupted ? "28a" : "289");
    auto [owner, ref] = bind(interrupted ? "e1" : "e0", "999999");
    if (interrupted) {
      api::SetDmlDeleteRowsTestFaultPointV1(api::EngineDmlDeleteRowsTestFaultPointV1::after_prepared_outcome);
      Require(!api::ExecuteDmlDeleteRowsDescriptorV1(owner, ref, 1).ok,
              "DELETE pre-commit interrupted boundary was not reached");
    }
    Commit(database_path, transaction);
    RequireTypedUpdateResourceGrants(0, "commit leaked an unfinished DELETE owner");
    Require(!api::ExecuteDmlDeleteRowsDescriptorV1(owner, ref, 1).ok,
            "committed unfinished DELETE was later executable");
  }
  std::cout << "ordinary_typed_DELETE_and_four_recovery_boundaries=passed\n";
}

void VerifyTypedUpdateDescriptorContract(
    const std::filesystem::path& database_path) {
  auto transaction = BeginTransaction(database_path, "205");
  (void)InsertInt64FieldsRowIntoTable(
      database_path,
      transaction,
      kQueryLeftTableUuid,
      "019f2100-0000-7000-8000-000000000226",
      {{"id", "1"}});
  auto context = TypedUpdateStatementContext(transaction, "01");
  VerifyPublicDmlResourceShutdown(TypedUpdateStatementContext(
      BeginTransaction(database_path, "2ff"), "fe"));
  const auto boolean_identity =
      scratchbird::core::datatypes::LookupDatatypeTypeCodecIdentityV1(
          context.datatype_catalog_snapshot_uuid.canonical,
          context.datatype_catalog_generation,
          context.datatype_registry_generation,
          "01000000-626f-7f6c-a561-6e0000000000", 1);
  Require(boolean_identity.ok &&
              boolean_identity.row.type_uuid ==
                  "01000000-626f-7f6c-a561-6e0000000000" &&
              boolean_identity.row.type_generation == 1 &&
              boolean_identity.row.codec_id == "datatype.boolean.u8.v1" &&
              boolean_identity.row.codec_version == 1 &&
              boolean_identity.row.codec_generation == 1 &&
              boolean_identity.row.canonical_value_bytes == 1,
          "typed UPDATE boolean predicate identity registry row drifted");

  api::EngineDmlUpdateRowsBindingDemandV1 demand;
  demand.authenticated_statement_receipt_uuid =
      context.statement_receipt_uuid.canonical;
  demand.structural_occurrence_id = 1;
  demand.target_relation_uuid_hint = kQueryLeftTableUuid;
  demand.assignments.push_back({1, "id", "2", "int64"});
  demand.predicate_kind = "column_equals";
  demand.predicate_column_spelling = "id";
  demand.predicate_literal_spelling = "1";
  demand.predicate_literal_type_spelling = "int64";

  {
    auto delete_context = context;
    delete_context.trace_tags = {"private_dml_delete_rows_binder"};
    auto delete_consumer = delete_context;
    delete_consumer.trace_tags = {"private_dml_delete_rows_consumer"};
    for (const auto target : {kQueryLeftTableUuid, kTableUuid}) {
      const auto effects = api::CaptureDmlDeleteEffectAuthorityV1(delete_context, target);
      if (!effects.ok) std::cerr << effects.diagnostic.code << ':' << effects.diagnostic.detail << '\n';
      Require(effects.ok && effects.handle.valid() && effects.snapshot.generation != 0 &&
                  effects.snapshot.constraint_count == 0 && effects.snapshot.trigger_count == 0 &&
                  effects.snapshot.index_count == (target == kTableUuid ? 1u : 0u),
              "DELETE effects were not captured from live relation/index/constraint managers");
      Require(!api::RevalidateDmlDeleteEffectAuthorityV1(delete_consumer, effects).error,
              "DELETE live effect authority did not revalidate");
      auto delete_recovery = delete_context;
      delete_recovery.trace_tags = {"private_dml_delete_rows_recovery"};
      Require(!api::RevalidateRecoveredDmlDeleteEffectProjectionV1(delete_recovery, effects.snapshot).error,
              "DELETE recovered effect source did not match actual managers");
      Require(api::RevalidateRecoveredDmlDeleteEffectProjectionV1(delete_consumer, effects.snapshot).error,
              "DELETE recovered projection substituted for live effect handle");
      auto wrong_recovered_effect = effects.snapshot;
      wrong_recovered_effect.relation_shape_sha256[0] ^= 1;
      Require(api::RevalidateRecoveredDmlDeleteEffectProjectionV1(delete_recovery, wrong_recovered_effect).error,
              "DELETE recovered effect source accepted changed relation shape");
      Require(api::RevalidateDmlDeleteEffectAuthorityV1(context, effects).error,
              "UPDATE phase conferred DELETE effect authority");
      auto forged = effects; forged.handle = {};
      Require(api::RevalidateDmlDeleteEffectAuthorityV1(delete_consumer, forged).error,
              "DELETE effect snapshot bytes manufactured a handle");
      forged = effects; forged.snapshot.index_set_sha256[0] ^= 1;
      Require(api::RevalidateDmlDeleteEffectAuthorityV1(delete_consumer, forged).error,
              "DELETE effect projection changed without its provider");
      auto stale = delete_consumer; stale.statement_metadata_snapshot_active_excluded_local_transaction_ids.push_back(999999);
      Require(api::RevalidateDmlDeleteEffectAuthorityV1(stale, effects).error,
              "DELETE effect authority crossed statement visibility projections");
    }
    api::EngineDmlDeleteRowsBindingDemandV1 deletion;
    deletion.authenticated_statement_receipt_uuid = context.statement_receipt_uuid.canonical;
    deletion.structural_occurrence_id = 1;
    deletion.target_relation_uuid_hint = kQueryLeftTableUuid;
    update_wire::TypedUpdateUuid descriptor{}, occurrence{};
    descriptor[0] = occurrence[0] = 1; descriptor[15] = 41; occurrence[15] = 42;
    const auto bind = [&](const api::EngineDmlDeleteRowsBindingDemandV1& input,
                          const api::EngineRequestContext& owner) {
      return api::BindDmlDeletePredicateV1(owner, input, descriptor, 1, occurrence, 1);
    };
    const auto all = bind(deletion, delete_context);
    if (!all.ok) std::cerr << all.diagnostic.code << ':' << all.diagnostic.detail << '\n';
    Require(all.ok && all.predicate.records.size() == 1 &&
                all.predicate.records[0].canonical_value == std::vector<std::uint8_t>{1} &&
                all.execution_predicate.predicate_kind.empty(),
            "DELETE did not bind canonical TRUE from the live relation");
    deletion.predicate_kind = "column_equals";
    deletion.predicate_column_spelling = "ID";
    deletion.predicate_literal_spelling = "1";
    deletion.predicate_literal_type_spelling = "int64";
    const auto equality = bind(deletion, delete_context);
    if (!equality.ok) std::cerr << equality.diagnostic.code << ':' << equality.diagnostic.detail << '\n';
    Require(equality.ok && equality.predicate.records.size() == 3 &&
                equality.predicate.identity.owner_descriptor_uuid == descriptor &&
                equality.predicate.records[0].referenced_relation_occurrence_uuid == occurrence &&
                equality.execution_predicate.canonical_predicate_envelope == "id" &&
                equality.execution_predicate.bound_values[0].binary_value.size() == 8 &&
                equality.execution_predicate.bound_values[0].binary_value[0] == 1,
            "DELETE equality did not preserve exact live column/type/occurrence binding");
    for (const std::string spelling : {"-9223372036854775808", "9223372036854775807"}) {
      auto extreme = deletion; extreme.predicate_literal_spelling = spelling;
      Require(bind(extreme, delete_context).ok, "DELETE int64 boundary refused");
    }
    for (const std::string spelling : {"", "9223372036854775808", "-9223372036854775809", "1garbage", "1;DELETE"}) {
      auto invalid = deletion; invalid.predicate_literal_spelling = spelling;
      Require(!bind(invalid, delete_context).ok, "DELETE malformed integer admitted");
    }
    auto invalid = deletion; invalid.predicate_column_spelling = "missing";
    Require(!bind(invalid, delete_context).ok, "DELETE unresolved column admitted");
    invalid = deletion; invalid.predicate_kind.clear();
    Require(!bind(invalid, delete_context).ok, "DELETE TRUE discarded nonempty predicate fields");
    invalid = deletion; invalid.authenticated_statement_receipt_uuid = context.transaction_uuid.canonical;
    Require(!bind(invalid, delete_context).ok, "DELETE predicate rebound across receipts");
    invalid = deletion; invalid.target_relation_uuid_hint = kTableUuid;
    invalid.predicate_column_spelling = "note";
    Require(!bind(invalid, delete_context).ok, "DELETE invented a TEXT equality provider");
    Require(!bind(deletion, context).ok, "UPDATE binder phase conferred DELETE binding");
    auto invalid_context = delete_context;
    invalid_context.trace_tags.push_back("private_dml_update_rows_consumer");
    Require(!bind(deletion, invalid_context).ok, "DELETE mixed phase admitted");
    invalid_context = delete_context; invalid_context.query_cancellation_requested = [] { return true; };
    Require(!bind(deletion, invalid_context).ok, "DELETE ignored pre-binding cancellation");
    RequireTypedUpdateResourceGrants(0, "read-only DELETE predicate binding manufactured an execution grant");
  }

  auto missing_resource_context = context;
  missing_resource_context.dml_update_resource_receipt.reset();
  const auto missing_resource = api::BindDmlUpdateRowsDescriptorV1(missing_resource_context, demand);
  Require(!missing_resource.ok && missing_resource.diagnostic.code == "SECURITY.ACCESS_DENIED",
          "typed UPDATE binder fabricated a missing resource authority");
  RequireTypedUpdateResourceGrants(0, "missing authority debited capacity");
  const auto bound = api::BindDmlUpdateRowsDescriptorV1(context, demand);
  if (!bound.ok) {
    std::cerr << "typed UPDATE bind diagnostic: "
              << bound.diagnostic.code << ':'
              << bound.diagnostic.message_key << ':'
              << bound.diagnostic.detail << '\n';
  }
  Require(bound.ok && !bound.descriptor_ref.descriptor_uuid.empty() &&
              bound.descriptor_ref.descriptor_generation == 1,
          "typed UPDATE descriptor binding failed");
  RequireTypedUpdateResourceGrants(1, "durable bound descriptor did not retain a grant");
  const auto quota_refused = api::BindDmlUpdateRowsDescriptorV1(context, demand);
  Require(!quota_refused.ok && quota_refused.diagnostic.code == "RESOURCE.BUDGET_EXCEEDED",
          "typed UPDATE binder bypassed aggregate quota");
  RequireTypedUpdateResourceGrants(1, "quota refusal changed the retained grant");
  api::MgaDmlUpdateDurableOperationLookupV1 resource_lookup;
  resource_lookup.descriptor_uuid = bound.descriptor_ref.descriptor_uuid;
  resource_lookup.descriptor_generation = bound.descriptor_ref.descriptor_generation;
  resource_lookup.structural_occurrence_id = 1;
  const auto resource_inspection = api::InspectMgaDmlUpdateDurableOperationForTestingV1(context, resource_lookup);
  update_wire::TypedUpdateResourceBudgetCarrier durable_budget;
  update_wire::TypedUpdateCarrierError resource_error;
  Require(update_wire::DecodeAndValidateTypedUpdateResourceBudget(
              resource_inspection.authority_snapshot.resource_budget_dubr, &durable_budget, &resource_error) &&
              durable_budget.resource_budget_generation == 2 && durable_budget.exact_bytes.size() == 208 &&
              durable_budget.maximum_total_canonical_value_bytes == 16ull * 1024 * 1024,
          "durable DUBR did not preserve the actual governor generation/policy");
  const auto consumer_context = TypedUpdateConsumerContext(context);
  const auto binder_execution = api::ExecuteDmlUpdateRowsDescriptorV1(
      context, bound.descriptor_ref, 1);
  Require(!binder_execution.ok &&
              binder_execution.diagnostic.code == "SECURITY.ACCESS_DENIED",
          "typed UPDATE binder capability was admitted for execution");

  auto stale_ref = bound.descriptor_ref;
  ++stale_ref.descriptor_generation;
  const auto stale =
      api::ExecuteDmlUpdateRowsDescriptorV1(consumer_context, stale_ref, 1);
  Require(!stale.ok && stale.diagnostic.code == "MGA.TRANSACTION.STALE",
          "typed UPDATE stale descriptor generation was admitted");

  auto foreign_context = consumer_context;
  foreign_context.statement_receipt_uuid.canonical =
      "019f2100-0000-7000-8000-000000000902";
  const auto foreign = api::ExecuteDmlUpdateRowsDescriptorV1(
      foreign_context, bound.descriptor_ref, 1);
  Require(!foreign.ok &&
              foreign.diagnostic.code == "SECURITY.ACCESS_DENIED",
          "typed UPDATE cross-receipt descriptor was admitted");

  const auto require_stale_context =
      [&](api::EngineRequestContext stale_context,
          std::string_view failure_message) {
        const auto stale_execution = api::ExecuteDmlUpdateRowsDescriptorV1(
            stale_context, bound.descriptor_ref, 1);
        Require(!stale_execution.ok &&
                    stale_execution.diagnostic.code ==
                        "MGA.TRANSACTION.STALE",
                failure_message);
      };
  auto stale_transaction = consumer_context;
  stale_transaction.transaction_uuid.canonical =
      "019f2100-0000-7000-8000-000000000903";
  require_stale_context(std::move(stale_transaction),
                        "typed UPDATE crossed transaction identity");
  auto stale_statement_snapshot = consumer_context;
  stale_statement_snapshot.statement_snapshot_uuid.canonical =
      "019f2100-0000-7000-8000-000000000904";
  require_stale_context(std::move(stale_statement_snapshot),
                        "typed UPDATE crossed statement snapshot");
  auto stale_metadata_snapshot = consumer_context;
  stale_metadata_snapshot.statement_metadata_snapshot_uuid.canonical =
      "019f2100-0000-7000-8000-000000000905";
  require_stale_context(std::move(stale_metadata_snapshot),
                        "typed UPDATE crossed catalog snapshot");
  auto stale_datatype_snapshot = consumer_context;
  stale_datatype_snapshot.datatype_catalog_snapshot_uuid.canonical =
      "019f2100-0000-7000-8000-000000000906";
  require_stale_context(std::move(stale_datatype_snapshot),
                        "typed UPDATE crossed datatype snapshot");
  auto stale_datatype_catalog_generation = consumer_context;
  ++stale_datatype_catalog_generation.datatype_catalog_generation;
  require_stale_context(std::move(stale_datatype_catalog_generation),
                        "typed UPDATE crossed datatype catalog generation");
  auto stale_datatype_registry_generation = consumer_context;
  ++stale_datatype_registry_generation.datatype_registry_generation;
  require_stale_context(std::move(stale_datatype_registry_generation),
                        "typed UPDATE crossed datatype registry generation");
  auto stale_security_generation = consumer_context;
  ++stale_security_generation.security_epoch;
  require_stale_context(std::move(stale_security_generation),
                        "typed UPDATE crossed security generation");
  auto stale_catalog_generation = consumer_context;
  ++stale_catalog_generation.catalog_generation_id;
  require_stale_context(std::move(stale_catalog_generation),
                        "typed UPDATE crossed catalog generation");
  auto missing_security_context = consumer_context;
  missing_security_context.security_context_present = false;
  require_stale_context(std::move(missing_security_context),
                        "typed UPDATE executed without security context");
  const auto wrong_occurrence = api::ExecuteDmlUpdateRowsDescriptorV1(
      consumer_context, bound.descriptor_ref, 2);
  Require(!wrong_occurrence.ok &&
              wrong_occurrence.diagnostic.code == "MGA.TRANSACTION.STALE",
          "typed UPDATE descriptor crossed structural occurrence");

  const auto executed = api::ExecuteDmlUpdateRowsDescriptorV1(
      consumer_context, bound.descriptor_ref, 1);
  if (!executed.ok) {
    std::cerr << "typed UPDATE execute diagnostic: "
              << executed.diagnostic.code << ':'
              << executed.diagnostic.message_key << ':'
              << executed.diagnostic.detail << '\n';
  }
  Require(executed.ok && !executed.immutable_replay &&
              executed.update_result.ok &&
              executed.update_result.matched_count == 1 &&
              executed.update_result.updated_count == 1,
          "typed UPDATE exact descriptor execution failed");
  RequireTypedUpdateResourceGrants(0, "durable terminal publication retained capacity");
  Require(executed.canonical_result_bytes.size() == 256,
          "typed UPDATE did not publish exact DURS256");
  const auto& durs = executed.canonical_result_bytes;
  Require(durs[0] == 'D' && durs[1] == 'U' && durs[2] == 'R' &&
              durs[3] == 'S' && durs[4] == 1 && durs[5] == 0 &&
              GetU16(durs, 6) == 256 && GetU64(durs, 104) == 1 &&
              GetU64(durs, 112) == 1 && durs[120] == 1,
          "typed UPDATE DURS header/count fields drifted");
  Require(BytesNonzero(durs, 16, 16) && BytesNonzero(durs, 40, 16) &&
              BytesNonzero(durs, 56, 16) && BytesNonzero(durs, 80, 16) &&
              BytesNonzero(durs, 128, 32) && BytesNonzero(durs, 160, 32) &&
              BytesNonzero(durs, 192, 16) && BytesNonzero(durs, 216, 32),
          "typed UPDATE DURS authority/evidence fields were zero");
  Require(std::all_of(durs.begin() + 121, durs.begin() + 128,
                      [](std::uint8_t byte) { return byte == 0; }) &&
              std::all_of(durs.begin() + 248, durs.end(),
                          [](std::uint8_t byte) { return byte == 0; }),
          "typed UPDATE DURS reserved bytes were nonzero");
  const auto evidence_material = TypedUpdateEvidenceMaterial(
      durs, executed.update_result.evidence);
  RequireTypedUpdateHash(
      "ScratchBird.SblrDmlUpdateRowsEffectSet.V1", evidence_material,
      durs, 128, "typed UPDATE effect-set hash material drifted");
  RequireTypedUpdateHash(
      "ScratchBird.SblrDmlUpdateRowsExecutorEvidence.V1",
      evidence_material, durs, 160,
      "typed UPDATE executor-evidence hash material drifted");
  RequireTypedUpdateHash(
      "ScratchBird.SblrDmlUpdateRowsResult.V1",
      std::vector<std::uint8_t>(durs.begin(), durs.begin() + 216), durs,
      216, "typed UPDATE result-evidence hash material drifted");

  const auto replay = api::ExecuteDmlUpdateRowsDescriptorV1(
      consumer_context, bound.descriptor_ref, 1);
  Require(replay.ok && replay.immutable_replay &&
              replay.update_result.updated_count == 1 &&
              replay.update_result.dml_summary.rows_changed == 1 &&
              replay.canonical_result_bytes == durs,
          "typed UPDATE exact replay did not return immutable prior DURS");
  const auto visible = SelectFromTableById(transaction, kQueryLeftTableUuid,
                                           "2");
  Require(visible.result_shape.rows.size() == 1,
          "typed UPDATE post-state did not expose the replacement value");

  auto no_effect_context = TypedUpdateStatementContext(transaction, "0e");
  auto no_effect_demand = demand;
  no_effect_demand.authenticated_statement_receipt_uuid =
      no_effect_context.statement_receipt_uuid.canonical;
  no_effect_demand.assignments.front().literal_spelling = "2";
  no_effect_demand.predicate_literal_spelling = "2";
  const auto no_effect_bound = api::BindDmlUpdateRowsDescriptorV1(
      no_effect_context, no_effect_demand);
  Require(no_effect_bound.ok,
          "typed UPDATE no-effect descriptor bind failed");
  no_effect_context = TypedUpdateConsumerContext(
      std::move(no_effect_context));
  const auto no_effect = api::ExecuteDmlUpdateRowsDescriptorV1(
      no_effect_context, no_effect_bound.descriptor_ref, 1);
  Require(no_effect.ok && !no_effect.immutable_replay &&
              no_effect.update_result.matched_count == 1 &&
              no_effect.update_result.updated_count == 0 &&
              no_effect.canonical_result_bytes.size() == 256 &&
              GetU64(no_effect.canonical_result_bytes, 104) == 1 &&
              GetU64(no_effect.canonical_result_bytes, 112) == 0 &&
              SelectFromTableById(transaction, kQueryLeftTableUuid, "2")
                      .result_shape.rows.size() == 1,
          "typed UPDATE no-effect rule created a replacement effect");

  const auto bind_recovery_update =
      [&](std::string_view context_suffix, std::string_view prior_value,
          std::string_view replacement_value,
          std::string_view row_uuid) {
        (void)InsertInt64FieldsRowIntoTable(
            database_path, transaction, kQueryLeftTableUuid,
            std::string(row_uuid), {{"id", std::string(prior_value)}});
        auto recovery_context =
            TypedUpdateStatementContext(transaction, context_suffix);
        auto recovery_demand = demand;
        recovery_demand.authenticated_statement_receipt_uuid =
            recovery_context.statement_receipt_uuid.canonical;
        recovery_demand.assignments.front().literal_spelling =
            std::string(replacement_value);
        recovery_demand.predicate_literal_spelling = std::string(prior_value);
        const auto recovery_bound = api::BindDmlUpdateRowsDescriptorV1(
            recovery_context, recovery_demand);
        Require(recovery_bound.ok,
                "typed UPDATE recovery descriptor bind failed");
        return std::make_pair(
                              TypedUpdateConsumerContext(
                                  std::move(recovery_context)),
                              recovery_bound.descriptor_ref);
      };

  auto [live_reload_context, live_reload_ref] = bind_recovery_update(
      "09", "20", "21", "019f2100-0000-7000-8000-000000000228");
  api::ResetDmlUpdateRowsDescriptorRegistryForTestV1();
  const auto live_reload = api::ExecuteDmlUpdateRowsDescriptorV1(
      live_reload_context, live_reload_ref, 1);
  Require(!live_reload.ok &&
              live_reload.diagnostic.code == "MGA.TRANSACTION.STALE" &&
              SelectFromTableById(transaction, kQueryLeftTableUuid, "20")
                      .result_shape.rows.size() == 1 &&
              SelectFromTableById(transaction, kQueryLeftTableUuid, "21")
                  .result_shape.rows.empty(),
          "typed UPDATE reopened bound descriptor was executed");
  RequireTypedUpdateResourceGrants(0, "bound recovery abort did not return capacity");

  auto [intent_fault_context, intent_fault_ref] = bind_recovery_update(
      "0a", "30", "31", "019f2100-0000-7000-8000-000000000229");
  api::SetDmlUpdateRowsTestFaultPointV1(
      api::EngineDmlUpdateRowsTestFaultPointV1::after_durable_intent);
  const auto intent_fault = api::ExecuteDmlUpdateRowsDescriptorV1(
      intent_fault_context, intent_fault_ref, 1);
  Require(!intent_fault.ok &&
              intent_fault.diagnostic.code == "DML.UPDATE_FAILED",
          "typed UPDATE durable-intent fault was not injected");
  RequireTypedUpdateResourceGrants(1, "intent fault prematurely returned capacity");
  api::ResetDmlUpdateRowsDescriptorRegistryForTestV1();
  RequireTypedUpdateResourceGrants(1, "descriptor cache reset returned an unresolved grant");
  const auto intent_recovery = api::ExecuteDmlUpdateRowsDescriptorV1(
      intent_fault_context, intent_fault_ref, 1);
  Require(!intent_recovery.ok &&
              intent_recovery.diagnostic.code == "MGA.TRANSACTION.STALE" &&
              SelectFromTableById(transaction, kQueryLeftTableUuid, "30")
                      .result_shape.rows.size() == 1 &&
              SelectFromTableById(transaction, kQueryLeftTableUuid, "31")
                  .result_shape.rows.empty(),
          "typed UPDATE durable-intent recovery retained a row effect");
  RequireTypedUpdateResourceGrants(0, "intent recovery abort retained capacity");

  auto [prepared_fault_context, prepared_fault_ref] = bind_recovery_update(
      "0b", "40", "41", "019f2100-0000-7000-8000-00000000022a");
  api::SetDmlUpdateRowsTestFaultPointV1(
      api::EngineDmlUpdateRowsTestFaultPointV1::after_prepared_outcome);
  const auto prepared_fault = api::ExecuteDmlUpdateRowsDescriptorV1(
      prepared_fault_context, prepared_fault_ref, 1);
  Require(!prepared_fault.ok &&
              prepared_fault.diagnostic.code == "DML.UPDATE_FAILED",
          "typed UPDATE prepared-outcome fault was not injected");
  RequireTypedUpdateResourceGrants(1, "prepared fault prematurely returned capacity");
  api::ResetDmlUpdateRowsDescriptorRegistryForTestV1();
  const auto prepared_recovery = api::ExecuteDmlUpdateRowsDescriptorV1(
      prepared_fault_context, prepared_fault_ref, 1);
  const auto prepared_prior =
      SelectFromTableById(transaction, kQueryLeftTableUuid, "40");
  const auto prepared_replacement =
      SelectFromTableById(transaction, kQueryLeftTableUuid, "41");
  if (prepared_recovery.ok ||
      prepared_recovery.diagnostic.code != "MGA.TRANSACTION.STALE" ||
      prepared_prior.result_shape.rows.size() != 1 ||
      !prepared_replacement.result_shape.rows.empty()) {
    std::cerr << "prepared recovery diagnostic="
              << prepared_recovery.diagnostic.code << ':'
              << prepared_recovery.diagnostic.message_key << ':'
              << prepared_recovery.diagnostic.detail
              << " prior_rows=" << prepared_prior.result_shape.rows.size()
              << " replacement_rows="
              << prepared_replacement.result_shape.rows.size() << '\n';
  }
  Require(!prepared_recovery.ok &&
              prepared_recovery.diagnostic.code == "MGA.TRANSACTION.STALE" &&
              prepared_prior.result_shape.rows.size() == 1 &&
              prepared_replacement.result_shape.rows.empty(),
          "typed UPDATE prepared-outcome recovery retained a row effect");
  RequireTypedUpdateResourceGrants(0, "prepared recovery abort retained capacity");

  auto [barrier_fault_context, barrier_fault_ref] = bind_recovery_update(
      "0c", "50", "51", "019f2100-0000-7000-8000-00000000022b");
  api::SetDmlUpdateRowsTestFaultPointV1(
      api::EngineDmlUpdateRowsTestFaultPointV1::after_publication_barrier);
  const auto barrier_fault = api::ExecuteDmlUpdateRowsDescriptorV1(
      barrier_fault_context, barrier_fault_ref, 1);
  Require(!barrier_fault.ok &&
              barrier_fault.diagnostic.code == "DML.UPDATE_FAILED",
          "typed UPDATE publication-barrier fault was not injected");
  RequireTypedUpdateResourceGrants(1, "known-applied recovery window returned capacity early");
  const auto barrier_same_process_recovery =
      api::ExecuteDmlUpdateRowsDescriptorV1(
          barrier_fault_context, barrier_fault_ref, 1);
  Require(barrier_same_process_recovery.ok &&
              barrier_same_process_recovery.immutable_replay &&
              barrier_same_process_recovery.update_result.updated_count == 1 &&
              barrier_same_process_recovery.canonical_result_bytes.size() ==
                  256 &&
              SelectFromTableById(transaction, kQueryLeftTableUuid, "50")
                  .result_shape.rows.empty() &&
              SelectFromTableById(transaction, kQueryLeftTableUuid, "51")
                      .result_shape.rows.size() == 1,
          "typed UPDATE same-process known-applied recovery did not use the "
          "MGA staged successor");
  RequireTypedUpdateResourceGrants(0, "recovered terminal publication retained capacity");
  api::ResetDmlUpdateRowsDescriptorRegistryForTestV1();
  const auto barrier_recovery = api::ExecuteDmlUpdateRowsDescriptorV1(
      barrier_fault_context, barrier_fault_ref, 1);
  Require(barrier_recovery.ok && barrier_recovery.immutable_replay &&
              barrier_recovery.update_result.updated_count == 1 &&
              barrier_recovery.update_result.dml_summary.rows_changed == 1 &&
              barrier_recovery.canonical_result_bytes.size() == 256 &&
              SelectFromTableById(transaction, kQueryLeftTableUuid, "50")
                  .result_shape.rows.empty() &&
              SelectFromTableById(transaction, kQueryLeftTableUuid, "51")
                      .result_shape.rows.size() == 1,
          "typed UPDATE publication-barrier recovery did not replay exactly");
  api::ResetDmlUpdateRowsDescriptorRegistryForTestV1();
  auto barrier_foreign_context = barrier_fault_context;
  barrier_foreign_context.statement_receipt_uuid.canonical =
      "019f2100-0000-7000-8000-00000000090d";
  const auto barrier_foreign = api::ExecuteDmlUpdateRowsDescriptorV1(
      barrier_foreign_context, barrier_fault_ref, 1);
  Require(!barrier_foreign.ok &&
              barrier_foreign.diagnostic.code == "SECURITY.ACCESS_DENIED",
          "typed UPDATE recovered outcome crossed statement receipts");

  (void)InsertInt64FieldsRowIntoTable(
      database_path,
      transaction,
      kQueryLeftTableUuid,
      "019f2100-0000-7000-8000-000000000227",
      {{"id", "10"}});
  auto cancel_context = TypedUpdateStatementContext(transaction, "05");
  auto cancel_demand = demand;
  cancel_demand.authenticated_statement_receipt_uuid =
      cancel_context.statement_receipt_uuid.canonical;
  cancel_demand.assignments.front().literal_spelling = "11";
  cancel_demand.predicate_literal_spelling = "10";
  const auto cancel_bound =
      api::BindDmlUpdateRowsDescriptorV1(cancel_context, cancel_demand);
  Require(cancel_bound.ok,
          "typed UPDATE prepublication cancellation descriptor bind failed");
  cancel_context = TypedUpdateConsumerContext(std::move(cancel_context));
  auto mutation_observed = std::make_shared<bool>(false);
  cancel_context.query_cancellation_requested =
      [database_path, transaction, mutation_observed] {
        const auto candidate = SelectFromTableById(
            transaction, kQueryLeftTableUuid, "11");
        *mutation_observed = candidate.result_shape.rows.size() == 1;
        return *mutation_observed;
      };
  const auto cancelled_execution = api::ExecuteDmlUpdateRowsDescriptorV1(
      cancel_context, cancel_bound.descriptor_ref, 1);
  Require(!cancelled_execution.ok && *mutation_observed &&
              cancelled_execution.diagnostic.code == "PROCESS.CANCELLED",
          "typed UPDATE did not observe cancellation before publication");
  const auto retained_ten = SelectFromTableById(
      transaction, kQueryLeftTableUuid, "10");
  const auto rolled_back_eleven = SelectFromTableById(
      transaction, kQueryLeftTableUuid, "11");
  Require(retained_ten.result_shape.rows.size() == 1 &&
              rolled_back_eleven.result_shape.rows.empty(),
          "typed UPDATE cancellation left a partial row effect");
  RequireTypedUpdateResourceGrants(0, "durable cancellation abort retained capacity");
  auto cancelled_replay_context = cancel_context;
  cancelled_replay_context.query_cancellation_requested = {};
  const auto cancelled_replay = api::ExecuteDmlUpdateRowsDescriptorV1(
      cancelled_replay_context, cancel_bound.descriptor_ref, 1);
  Require(!cancelled_replay.ok &&
              cancelled_replay.diagnostic.code == "MGA.TRANSACTION.STALE",
          "typed UPDATE cancelled descriptor was replayed as live");

  update_wire::TypedUpdateResultCarrier evidence_result_carrier;
  update_wire::TypedUpdateCarrierError evidence_error;
  Require(update_wire::DecodeAndValidateTypedUpdateResult(
              durs, &evidence_result_carrier, &evidence_error),
          "typed UPDATE evidence test could not decode prior DURS256");
  std::vector<std::uint8_t> evidence_bytes;
  std::vector<update_wire::TypedUpdateResultEvidenceReference>
      malformed_evidence{{"bad=kind", "value"}};
  Require(!update_wire::EncodeTypedUpdateResultEvidenceMaterial(
              evidence_result_carrier, malformed_evidence, &evidence_bytes,
              &evidence_error) &&
              evidence_error.diagnostic_code == "DML.UPDATE_FAILED",
          "typed UPDATE evidence kind containing '=' was hashed");

  const std::vector<update_wire::TypedUpdateResultEvidenceReference>
      nul_evidence{{"kind", std::string("bad\0id", 6)}};
  Require(!update_wire::EncodeTypedUpdateResultEvidenceMaterial(
              evidence_result_carrier, nul_evidence, &evidence_bytes,
              &evidence_error) &&
              evidence_error.diagnostic_code == "DML.UPDATE_FAILED",
          "typed UPDATE evidence id containing NUL was hashed");

  const std::vector<update_wire::TypedUpdateResultEvidenceReference>
      duplicate_evidence{{"kind", "id=allowed"},
                         {"kind", "id=allowed"}};
  update_wire::TypedUpdateHash duplicate_effect_hash{};
  update_wire::TypedUpdateHash duplicate_executor_hash{};
  Require(update_wire::EncodeTypedUpdateResultEvidenceMaterial(
              evidence_result_carrier, duplicate_evidence, &evidence_bytes,
              &evidence_error) &&
              update_wire::ComputeTypedUpdateResultInnerEvidence(
                  evidence_result_carrier, duplicate_evidence,
                  &duplicate_effect_hash, &duplicate_executor_hash,
                  &evidence_error) &&
              evidence_bytes.size() ==
                  64U + 2U * (std::string_view("kind=id=allowed").size() + 1U) &&
              std::any_of(duplicate_effect_hash.begin(),
                          duplicate_effect_hash.end(),
                          [](std::uint8_t byte) { return byte != 0; }) &&
              std::any_of(duplicate_executor_hash.begin(),
                          duplicate_executor_hash.end(),
                          [](std::uint8_t byte) { return byte != 0; }),
          "typed UPDATE valid duplicate evidence was not preserved");

  auto duplicate = demand;
  duplicate.authenticated_statement_receipt_uuid =
      TypedUpdateStatementContext(transaction, "02")
          .statement_receipt_uuid.canonical;
  duplicate.assignments.push_back({2, "id", "3", "int64"});
  auto duplicate_context = TypedUpdateStatementContext(transaction, "02");
  const auto duplicate_result =
      api::BindDmlUpdateRowsDescriptorV1(duplicate_context, duplicate);
  Require(!duplicate_result.ok &&
              duplicate_result.diagnostic.code ==
                  "DML.ASSIGNMENT_SHAPE_INVALID",
          "typed UPDATE duplicate target column was admitted");

  auto unknown_context = TypedUpdateStatementContext(transaction, "03");
  auto unknown = demand;
  unknown.authenticated_statement_receipt_uuid =
      unknown_context.statement_receipt_uuid.canonical;
  unknown.assignments.front().target_column_spelling = "unknown_column";
  const auto unknown_result =
      api::BindDmlUpdateRowsDescriptorV1(unknown_context, unknown);
  Require(!unknown_result.ok &&
              unknown_result.diagnostic.code ==
                  "DML.ASSIGNMENT_SHAPE_INVALID",
          "typed UPDATE unknown target column was admitted");

  auto cancelled_context = TypedUpdateStatementContext(transaction, "04");
  cancelled_context.query_cancellation_requested = [] { return true; };
  auto cancelled = demand;
  cancelled.authenticated_statement_receipt_uuid =
      cancelled_context.statement_receipt_uuid.canonical;
  const auto cancelled_result =
      api::BindDmlUpdateRowsDescriptorV1(cancelled_context, cancelled);
  Require(!cancelled_result.ok &&
              cancelled_result.diagnostic.code ==
                  "PROCESS.CANCELLED",
          "typed UPDATE pre-bind cancellation was ignored");

  for (auto cutpoint : {api::MgaDmlUpdateDurableFaultCutpointV1::before_snapshot_write,
                       api::MgaDmlUpdateDurableFaultCutpointV1::after_snapshot_write_before_bound}) {
    auto fault_context = TypedUpdateStatementContext(transaction, "f1");
    auto fault_demand = demand;
    fault_demand.authenticated_statement_receipt_uuid = fault_context.statement_receipt_uuid.canonical;
    api::SetMgaDmlUpdateDurableFaultCutpointForTestingV1(cutpoint);
    const auto fault_bound = api::BindDmlUpdateRowsDescriptorV1(fault_context, fault_demand);
    api::SetMgaDmlUpdateDurableFaultCutpointForTestingV1(api::MgaDmlUpdateDurableFaultCutpointV1::none);
    Require(!fault_bound.ok, "bound publication fault was ignored");
    RequireTypedUpdateResourceGrants(0, "confirmed non-write stranded a prepared grant");
  }
  auto retired_context = TypedUpdateStatementContext(transaction, "f2");
  auto retired_demand = demand;
  retired_demand.authenticated_statement_receipt_uuid = retired_context.statement_receipt_uuid.canonical;
  const auto retired_bound = api::BindDmlUpdateRowsDescriptorV1(retired_context, retired_demand);
  Require(retired_bound.ok, "receipt retirement bind failed");
  retired_context.dml_update_resource_receipt.lock()->Revoke();
  RequireTypedUpdateResourceGrants(1, "revocation alone returned bound resources");
  api::RetireDmlUpdateResourceReceiptDescriptorsV1(retired_context);
  RequireTypedUpdateResourceGrants(0, "unexecuted receipt retirement retained capacity");
  const auto retired_replay = api::ExecuteDmlUpdateRowsDescriptorV1(
      TypedUpdateConsumerContext(retired_context), retired_bound.descriptor_ref, 1);
  Require(!retired_replay.ok, "retired descriptor was executed");
  auto lost_context = TypedUpdateStatementContext(transaction, "f3");
  auto lost_demand = demand;
  lost_demand.authenticated_statement_receipt_uuid = lost_context.statement_receipt_uuid.canonical;
  const auto lost_bound = api::BindDmlUpdateRowsDescriptorV1(lost_context, lost_demand);
  Require(lost_bound.ok, "lost-descriptor retirement bind failed");
  api::ResetDmlUpdateRowsDescriptorRegistryForTestV1();
  lost_context.dml_update_resource_receipt.lock()->Revoke();
  RequireTypedUpdateResourceGrants(1, "lost descriptor/revocation returned capacity early");
  api::RetireDmlUpdateResourceReceiptDescriptorsV1(lost_context);
  RequireTypedUpdateResourceGrants(0, "receipt retirement failed durable recovery after cache loss");

  auto cancellation_owner = std::make_shared<bool>(false);
  auto original_callback_context = transaction;
  original_callback_context.query_cancellation_requested = [cancellation_owner] { return *cancellation_owner; };
  auto owned_cancel_context = TypedUpdateStatementContext(original_callback_context, "f4");
  auto owned_cancel_demand = demand;
  owned_cancel_demand.authenticated_statement_receipt_uuid = owned_cancel_context.statement_receipt_uuid.canonical;
  const auto owned_cancel_bound = api::BindDmlUpdateRowsDescriptorV1(owned_cancel_context, owned_cancel_demand);
  Require(owned_cancel_bound.ok, "owned cancellation bind failed");
  *cancellation_owner = true;
  owned_cancel_context = TypedUpdateConsumerContext(owned_cancel_context);
  owned_cancel_context.query_cancellation_requested = [] { return false; };
  const auto owned_cancel = api::ExecuteDmlUpdateRowsDescriptorV1(
      owned_cancel_context, owned_cancel_bound.descriptor_ref, 1);
  Require(!owned_cancel.ok && owned_cancel.diagnostic.code == "PROCESS.CANCELLED",
          "consumer replaced the receipt-owned cancellation callback");
  RequireTypedUpdateResourceGrants(0, "pre-execution refusal did not durably abort and release its grant");

  const api::EngineDmlUpdateResourcePolicyV1 retry_policy{
      1024, 4096, 1048576, 64, 1048576, 16ull * 1024 * 1024, 32ull * 1024 * 1024, 16};
  Require(!TypedUpdateResourceGovernor()->Configure(retry_policy).error,
          "deferred retirement fixture policy failed");
  auto deferred_context = TypedUpdateStatementContext(transaction, "f5");
  auto deferred_demand = demand;
  deferred_demand.authenticated_statement_receipt_uuid = deferred_context.statement_receipt_uuid.canonical;
  const auto deferred_bound = api::BindDmlUpdateRowsDescriptorV1(deferred_context, deferred_demand);
  Require(deferred_bound.ok, "deferred retirement bind failed");
  api::RetireDmlUpdateResourceReceiptDescriptorsV1(deferred_context);
  RequireTypedUpdateResourceGrants(1, "cleanup retired an active receipt");

  auto run_retirement = std::make_shared<bool>(false);
  auto contention_context = transaction;
  contention_context.query_cancellation_requested = [run_retirement, deferred_context] {
    if (*run_retirement) {
      *run_retirement = false;
      // Revalidation holds the descriptor mutex but releases the governor
      // mutex around this callback. Use a different thread for try_lock.
      std::thread teardown([deferred_context] {
        deferred_context.dml_update_resource_receipt.lock()->Revoke();
        api::RetireDmlUpdateResourceReceiptDescriptorsV1(deferred_context);
      });
      teardown.join();
      RequireTypedUpdateResourceGrants(2, "busy teardown returned capacity without durable evidence");
    }
    return false;
  };
  contention_context = TypedUpdateStatementContext(contention_context, "f6");
  auto contention_demand = demand;
  contention_demand.authenticated_statement_receipt_uuid = contention_context.statement_receipt_uuid.canonical;
  const auto contention_bound = api::BindDmlUpdateRowsDescriptorV1(contention_context, contention_demand);
  Require(contention_bound.ok, "contention driver bind failed");
  *run_retirement = true;
  auto contention_consumer = TypedUpdateConsumerContext(contention_context);
  contention_consumer.query_cancellation_requested = [] { return false; };
  Require(api::ExecuteDmlUpdateRowsDescriptorV1(contention_consumer,
              contention_bound.descriptor_ref, 1).ok, "contention driver execution failed");
  Require(!*run_retirement, "contention callback was not exercised");
  RequireTypedUpdateResourceGrants(1, "deferred owner was lost after lock contention");
  api::MgaDmlUpdateDurableOperationLookupV1 deferred_lookup;
  deferred_lookup.descriptor_uuid = deferred_bound.descriptor_ref.descriptor_uuid;
  deferred_lookup.descriptor_generation = deferred_bound.descriptor_ref.descriptor_generation;
  deferred_lookup.structural_occurrence_id = 1;
  const auto deferred_before = api::InspectMgaDmlUpdateDurableOperationForTestingV1(
      deferred_context, deferred_lookup);
  Require(deferred_before.journal.size() == 1 &&
              deferred_before.journal.back().lifecycle_state == api::MgaDmlUpdateDurableJournalStateV1::bound,
          "busy retirement changed the durable bound head");

  auto foreign_governor = std::make_shared<api::EngineDmlUpdateResourceGovernorV1>();
  Require(!foreign_governor->Configure(retry_policy).error, "foreign runtime policy failed");
  auto foreign_runtime_context = deferred_context;
  foreign_runtime_context.dml_update_resource_receipt.reset();
  auto foreign_receipt = std::make_shared<api::EngineDmlUpdateResourceReceiptV1>(
      foreign_runtime_context, foreign_governor);
  foreign_runtime_context.dml_update_resource_receipt = foreign_receipt;
  api::RetryRetiredDmlUpdateResourceOwnersV1(foreign_runtime_context);
  RequireTypedUpdateResourceGrants(1, "another engine runtime drained the retired owner");
  auto wrong_database = contention_context;
  wrong_database.database_path += ".other";
  api::RetryRetiredDmlUpdateResourceOwnersV1(wrong_database);
  RequireTypedUpdateResourceGrants(1, "another database path drained the retired owner");
  wrong_database = contention_context;
  wrong_database.database_uuid.canonical = "019f2100-0000-7000-8000-000000000fff";
  api::RetryRetiredDmlUpdateResourceOwnersV1(wrong_database);
  RequireTypedUpdateResourceGrants(1, "another database identity drained the retired owner");

  auto retry_context = TypedUpdateStatementContext(transaction, "f7");
  auto retry_demand = demand;
  retry_demand.authenticated_statement_receipt_uuid = retry_context.statement_receipt_uuid.canonical;
  const auto retry_bound = api::BindDmlUpdateRowsDescriptorV1(retry_context, retry_demand);
  Require(retry_bound.ok, "admission after deferred retirement failed");
  RequireTypedUpdateResourceGrants(1, "admission did not drain the deferred owner before capture");
  const auto deferred_after = api::InspectMgaDmlUpdateDurableOperationForTestingV1(
      deferred_context, deferred_lookup);
  Require(deferred_after.journal.size() == 2 &&
              deferred_after.journal.back().lifecycle_state == api::MgaDmlUpdateDurableJournalStateV1::aborted,
          "admission retry did not durably abort the deferred owner");
  Require(!api::ExecuteDmlUpdateRowsDescriptorV1(TypedUpdateConsumerContext(deferred_context),
              deferred_bound.descriptor_ref, 1).ok, "deferred retired descriptor remained executable");
  retry_context.dml_update_resource_receipt.lock()->Revoke();
  api::RetryRetiredDmlUpdateResourceOwnersV1(retry_context);
  RequireTypedUpdateResourceGrants(0, "retired-owner retry did not drain the final owner");
  const auto released_before_retry = TypedUpdateResourceGovernor()->Observe().released_grants;
  api::RetryRetiredDmlUpdateResourceOwnersV1(retry_context);
  Require(TypedUpdateResourceGovernor()->Observe().released_grants == released_before_retry,
          "repeated retired-owner retry released a grant twice");

  for (const bool lose_descriptor_cache : {false, true}) {
    auto shutdown_context = TypedUpdateStatementContext(transaction, lose_descriptor_cache ? "f9" : "f8");
    auto shutdown_governor = std::make_shared<api::EngineDmlUpdateResourceGovernorV1>();
    Require(!shutdown_governor->Configure(retry_policy).error, "shutdown policy failed");
    shutdown_context.dml_update_resource_receipt.reset();
    auto shutdown_receipt = std::make_shared<api::EngineDmlUpdateResourceReceiptV1>(
        shutdown_context, shutdown_governor);
    shutdown_context.dml_update_resource_receipt = shutdown_receipt;
    auto shutdown_demand = demand;
    shutdown_demand.authenticated_statement_receipt_uuid = shutdown_context.statement_receipt_uuid.canonical;
    const auto shutdown_bound = api::BindDmlUpdateRowsDescriptorV1(shutdown_context, shutdown_demand);
    Require(shutdown_bound.ok && shutdown_governor->Observe().active_grants == 1,
            "shutdown fixture did not own a durable bound grant");
    if (lose_descriptor_cache) api::ResetDmlUpdateRowsDescriptorRegistryForTestV1();
    Require(api::DrainDmlUpdateResourceOwnersForRuntimeV1(foreign_governor) &&
                shutdown_governor->Observe().active_grants == 1,
            "shutdown drained another engine instance");
    Require(api::DrainDmlUpdateResourceOwnersForRuntimeV1(shutdown_governor) &&
                shutdown_receipt->IsRevoked() && shutdown_governor->Observe().active_grants == 0,
            "shutdown failed to reconcile a bound durable operation");
    api::MgaDmlUpdateDurableOperationLookupV1 lookup;
    lookup.descriptor_uuid = shutdown_bound.descriptor_ref.descriptor_uuid;
    lookup.descriptor_generation = shutdown_bound.descriptor_ref.descriptor_generation;
    lookup.structural_occurrence_id = 1;
    const auto retired = api::InspectMgaDmlUpdateDurableOperationForTestingV1(shutdown_context, lookup);
    Require(retired.journal.size() == 2 &&
                retired.journal.back().lifecycle_state == api::MgaDmlUpdateDurableJournalStateV1::aborted,
            "shutdown returned resources without durable aborted evidence");
    const auto released = shutdown_governor->Observe().released_grants;
    Require(api::DrainDmlUpdateResourceOwnersForRuntimeV1(shutdown_governor) &&
                shutdown_governor->Observe().released_grants == released,
            "shutdown retry released capacity twice");
  }
  unsigned shutdown_ordinal = 0;
  for (const auto point : {api::EngineDmlUpdateRowsTestFaultPointV1::after_durable_intent,
                          api::EngineDmlUpdateRowsTestFaultPointV1::after_prepared_outcome,
                          api::EngineDmlUpdateRowsTestFaultPointV1::after_publication_barrier}) {
    const auto ordinal = shutdown_ordinal++;
    const auto old_value = std::to_string(80 + ordinal * 2);
    const auto new_value = std::to_string(81 + ordinal * 2);
    (void)InsertInt64FieldsRowIntoTable(database_path, transaction, kQueryLeftTableUuid,
        "019f2100-0000-7000-8000-000000000fe" + std::to_string(ordinal), {{"id", old_value}});
    auto shutdown_context = TypedUpdateStatementContext(transaction,
        ordinal == 0 ? "fb" : ordinal == 1 ? "fc" : "fd");
    auto governor = std::make_shared<api::EngineDmlUpdateResourceGovernorV1>();
    Require(!governor->Configure(retry_policy).error, "interrupted shutdown policy failed");
    shutdown_context.dml_update_resource_receipt.reset();
    auto receipt = std::make_shared<api::EngineDmlUpdateResourceReceiptV1>(shutdown_context, governor);
    shutdown_context.dml_update_resource_receipt = receipt;
    auto interrupted_demand = demand;
    interrupted_demand.authenticated_statement_receipt_uuid = shutdown_context.statement_receipt_uuid.canonical;
    interrupted_demand.predicate_literal_spelling = old_value;
    interrupted_demand.assignments.front().literal_spelling = new_value;
    const auto bound = api::BindDmlUpdateRowsDescriptorV1(shutdown_context, interrupted_demand);
    Require(bound.ok, "interrupted shutdown fixture failed to bind");
    const auto consumer = TypedUpdateConsumerContext(shutdown_context);
    api::SetDmlUpdateRowsTestFaultPointV1(point);
    const auto interrupted = api::ExecuteDmlUpdateRowsDescriptorV1(consumer, bound.descriptor_ref, 1);
    Require(!interrupted.ok && governor->Observe().active_grants == 1,
            "interrupted shutdown released the nonterminal owner");
    api::ResetDmlUpdateRowsDescriptorRegistryForTestV1();
    Require(api::DrainDmlUpdateResourceOwnersForRuntimeV1(governor) &&
                governor->Observe().active_grants == 0 && receipt->IsRevoked(),
            "shutdown did not reconcile interrupted durable work after cache loss");
    const bool published = point == api::EngineDmlUpdateRowsTestFaultPointV1::after_publication_barrier;
    api::MgaDmlUpdateDurableOperationLookupV1 lookup;
    lookup.descriptor_uuid = bound.descriptor_ref.descriptor_uuid;
    lookup.descriptor_generation = bound.descriptor_ref.descriptor_generation;
    lookup.structural_occurrence_id = 1;
    const auto terminal = api::InspectMgaDmlUpdateDurableOperationForTestingV1(shutdown_context, lookup);
    Require(!terminal.journal.empty() && terminal.journal.back().lifecycle_state ==
                (published ? api::MgaDmlUpdateDurableJournalStateV1::published :
                             api::MgaDmlUpdateDurableJournalStateV1::aborted),
            "shutdown did not prove exact durable terminality before releasing resources");
    Require(SelectFromTableById(transaction, kQueryLeftTableUuid, old_value).result_shape.rows.size() ==
                (published ? 0u : 1u) &&
            SelectFromTableById(transaction, kQueryLeftTableUuid, new_value).result_shape.rows.size() ==
                (published ? 1u : 0u),
            "shutdown rewound a released statement or retained a pre-barrier mutation");
    const auto released = governor->Observe().released_grants;
    Require(api::DrainDmlUpdateResourceOwnersForRuntimeV1(governor) &&
                governor->Observe().released_grants == released,
            "interrupted shutdown double-released its grant");
  }
  {
    auto pending_context = TypedUpdateStatementContext(transaction, "fa");
    auto pending_governor = std::make_shared<api::EngineDmlUpdateResourceGovernorV1>();
    Require(!pending_governor->Configure(retry_policy).error, "pending shutdown policy failed");
    pending_context.dml_update_resource_receipt.reset();
    auto pending_receipt = std::make_shared<api::EngineDmlUpdateResourceReceiptV1>(pending_context, pending_governor);
    pending_context.dml_update_resource_receipt = pending_receipt;
    const auto pending = pending_receipt->Capture(pending_context);
    Require(pending.ok && !api::DrainDmlUpdateResourceOwnersForRuntimeV1(pending_governor) &&
                pending_governor->Observe().active_grants == 1,
            "shutdown inferred abandonment of an in-flight unpublished grant");
    Require(pending_receipt->ReleaseNoAlloc(pending_context, pending.handle,
                api::EngineDmlUpdateResourceReleaseV1::abandoned_before_publication) ==
                api::EngineDmlUpdateResourceReleaseCodeV1::released &&
                api::DrainDmlUpdateResourceOwnersForRuntimeV1(pending_governor),
            "shutdown did not become retryable after proven pre-publication abandonment");
  }

  unsigned io_ordinal = 0;
  for (const auto cutpoint : {
           api::MgaDmlUpdateDurableFaultCutpointV1::after_successor_write_before_fsync,
           api::MgaDmlUpdateDurableFaultCutpointV1::after_successor_fsync_before_ack}) {
    const auto ordinal = io_ordinal++;
    const auto old_value = std::to_string(880 + ordinal * 2);
    const auto new_value = std::to_string(881 + ordinal * 2);
    (void)InsertInt64FieldsRowIntoTable(database_path, transaction, kQueryLeftTableUuid,
        "019f2100-0000-7000-8000-000000000da" + std::to_string(ordinal), {{"id", old_value}});
    Require(!api::CreateMgaSavepointMarker(transaction, "update_io_outer").error,
            "UPDATE I/O fixture outer savepoint failed");
    auto io_context = TypedUpdateStatementContext(transaction, ordinal == 0 ? "d0" : "d1");
    auto io_demand = demand;
    io_demand.authenticated_statement_receipt_uuid = io_context.statement_receipt_uuid.canonical;
    io_demand.predicate_literal_spelling = old_value;
    io_demand.assignments.front().literal_spelling = new_value;
    const auto bound = api::BindDmlUpdateRowsDescriptorV1(io_context, io_demand);
    Require(bound.ok, "UPDATE I/O fixture binding failed");
    const auto consumer = TypedUpdateConsumerContext(io_context);
    api::SetDmlUpdateRowsTestFaultPointV1(api::EngineDmlUpdateRowsTestFaultPointV1::after_publication_barrier);
    Require(!api::ExecuteDmlUpdateRowsDescriptorV1(consumer, bound.descriptor_ref, 1).ok,
            "UPDATE I/O fixture did not reach its native barrier");
    api::ResetDmlUpdateRowsDescriptorRegistryForTestV1();
    api::SetMgaDmlUpdateDurableFaultCutpointForTestingV1(cutpoint);
    const auto uncertain = api::ExecuteDmlUpdateRowsDescriptorV1(consumer, bound.descriptor_ref, 1);
    api::SetMgaDmlUpdateDurableFaultCutpointForTestingV1(api::MgaDmlUpdateDurableFaultCutpointV1::none);
    Require(!uncertain.ok, "UPDATE successor acknowledgement fault was ignored");
    RequireTypedUpdateResourceGrants(1, "uncertain UPDATE publication lost its original grant");
    api::EngineCommitTransactionRequest pending_commit;
    pending_commit.context = transaction;
    const auto commit_refused = api::EngineCommitTransaction(pending_commit);
    Require(!commit_refused.ok && HasEvidence(commit_refused, "identity_refusal_reason",
                "DML_statement_reconciliation_required"),
            "transaction commit bypassed an uncertain UPDATE owner");
    api::EnginePublishStatementSnapshotRequest pending_statement;
    pending_statement.context = io_context;
    pending_statement.context.statement_snapshot_uuid = {};
    const auto statement_refused = api::EnginePublishStatementSnapshot(pending_statement);
    Require(!statement_refused.ok && !statement_refused.diagnostics.empty() &&
                statement_refused.diagnostics.front().detail ==
                    "transaction.snapshot.publish:DML_statement_reconciliation_required",
            "new statement bypassed an uncertain UPDATE owner");
    RequireTypedUpdateResourceGrants(1, "finality refusal discarded an uncertain UPDATE owner");
    const auto recovered = api::ExecuteDmlUpdateRowsDescriptorV1(consumer, bound.descriptor_ref, 1);
    if (!recovered.ok) std::cerr << recovered.diagnostic.code << ':' << recovered.diagnostic.detail << '\n';
    Require(recovered.ok && recovered.immutable_replay,
            "UPDATE ambiguous successor could not replay the released statement");
    RequireTypedUpdateResourceGrants(0, "UPDATE successor recovery leaked its original grant");
    Require(api::PrepareDmlUpdateTransactionFinalityV1(transaction),
            "resolved UPDATE owner continued to block transaction finality");
    const auto exact_replay = api::ExecuteDmlUpdateRowsDescriptorV1(consumer, bound.descriptor_ref, 1);
    Require(exact_replay.ok && exact_replay.canonical_result_bytes == recovered.canonical_result_bytes,
            "UPDATE ambiguous successor changed canonical replay bytes");
    Require(SelectFromTableById(transaction, kQueryLeftTableUuid, old_value).result_shape.rows.empty() &&
                SelectFromTableById(transaction, kQueryLeftTableUuid, new_value).result_shape.rows.size() == 1,
            "UPDATE ambiguous successor repeated or reversed the released mutation");
    Require(!api::RollbackToMgaSavepointMarker(transaction, "update_io_outer").error &&
                SelectFromTableById(transaction, kQueryLeftTableUuid, old_value).result_shape.rows.size() == 1 &&
                SelectFromTableById(transaction, kQueryLeftTableUuid, new_value).result_shape.rows.empty() &&
                !api::ReleaseMgaSavepointMarker(transaction, "update_io_outer").error,
            "UPDATE ambiguous successor damaged the outer user boundary");
  }
  std::cout << "UPDATE_integrated_successor_IO_recovery=passed\n";
  api::ResetDmlUpdateRowsDescriptorRegistryForTestV1();
  api::MgaDmlUpdateDurableOperationLookupV1 tampered_lookup;
  tampered_lookup.descriptor_uuid = barrier_fault_ref.descriptor_uuid;
  tampered_lookup.descriptor_generation =
      barrier_fault_ref.descriptor_generation;
  tampered_lookup.structural_occurrence_id = 1;
  Require(api::CorruptMgaDmlUpdateDurableExtentByteForTestingV1(
              barrier_fault_context, tampered_lookup, 0, 0x01),
          "typed UPDATE MGA durable extent tamper injection failed");
  const auto tampered_replay = api::ExecuteDmlUpdateRowsDescriptorV1(
      barrier_fault_context, barrier_fault_ref, 1);
  Require(!tampered_replay.ok &&
              tampered_replay.diagnostic.code == "DML.UPDATE_FAILED",
          "typed UPDATE admitted a malformed MGA durable extent");

  Commit(database_path, transaction);
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
  // The legacy textual SELECT fixtures below predate canonical query.execute
  // descriptor DAGs.  Prove that carrier remains fail-closed here; the
  // parser/query exact-route suites own executable join/window/set/aggregate
  // coverage and must not be replaced by a synthetic query.plan_operation.
  RequireOrderedSelectThroughServer(database_path, writer);
  const auto bulk_insert = InsertBulkRowsIntoTable(database_path, writer);
  Require(FieldValue(bulk_insert, "note", 0) == "bulk-direct-a",
          "bulk direct insert first row result mismatch");
  Require(FieldValue(bulk_insert, "note", 1) == "bulk-direct-b",
          "bulk direct insert second row result mismatch");
  const auto selected_bulk = SelectById(database_path, writer, "10");
  Require(selected_bulk.result_shape.rows.size() == 1,
          "bulk direct inserted row not visible");
  Require(FieldValue(selected_bulk, "note") == "bulk-direct-b",
          "bulk direct inserted row note mismatch");
  const auto fast_import = ExecuteFailFastImportRows(database_path, writer);
  Require(fast_import.result_shape.rows.size() == 2,
          "fail-fast import execution did not return two rows");
  const auto selected_fast_import_b = SelectById(database_path, writer, "12");
  Require(selected_fast_import_b.result_shape.rows.size() == 1,
          "fail-fast imported row B not visible");
  Require(FieldValue(selected_fast_import_b, "note") == "copy-fast-b",
          "fail-fast imported row B note mismatch");
  const auto public_fast_import = ExecuteFailFastImportRowsThroughServer(database_path, writer);
  Require(public_fast_import.result_shape.rows.size() == 1,
          "server public ABI fail-fast imported row not visible");
  Require(FieldValue(public_fast_import, "note") == "copy-public-fast-a",
          "server public ABI fail-fast imported row note mismatch");
  const auto public_fast_import_b = SelectById(database_path, writer, "14");
  Require(public_fast_import_b.result_shape.rows.size() == 1,
          "second server public ABI fail-fast imported row not visible");
  Require(FieldValue(public_fast_import_b, "note") == "copy-public-fast-b",
          "second server public ABI fail-fast imported row note mismatch");
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
  const auto committed_bulk = SelectById(database_path, reader, "10");
  Require(committed_bulk.result_shape.rows.size() == 1,
          "committed bulk direct row missing after commit");
  Require(FieldValue(committed_bulk, "note") == "bulk-direct-b",
          "committed bulk direct row returned wrong note");
  const auto committed_fast_import = SelectById(database_path, reader, "12");
  Require(committed_fast_import.result_shape.rows.size() == 1,
          "committed fail-fast import row missing after commit");
  Require(FieldValue(committed_fast_import, "note") == "copy-fast-b",
          "committed fail-fast import row returned wrong note");
  const auto committed_public_fast_import = SelectById(database_path, reader, "14");
  Require(committed_public_fast_import.result_shape.rows.size() == 1,
          "committed server public ABI fail-fast import row missing after commit");
  Require(FieldValue(committed_public_fast_import, "note") == "copy-public-fast-b",
          "committed server public ABI fail-fast import row returned wrong note");
  Commit(database_path, reader);

  auto rollback_writer = BeginTransaction(database_path, "203");
  (void)InsertRow(database_path, rollback_writer, kRowC, "3", "rollback-only");
  Rollback(database_path, rollback_writer);
  auto rollback_reader = BeginTransaction(database_path, "204");
  const auto rolled_back = SelectById(database_path, rollback_reader, "3");
  Require(rolled_back.result_shape.rows.empty(), "rolled-back DML row became visible");
  auto rollback_state_load = api::LoadMgaRelationStoreState(rollback_reader);
  Require(rollback_state_load.ok,
          "rolled-back transactional index state load failed");
  const auto rollback_state =
      api::BuildMgaRelationReadView(std::move(rollback_state_load.state));
  const auto& rollback_index = RequireFixtureIndex(rollback_state);
  const auto rolled_back_index_lookup =
      api::MgaOrderedBtreeTransactionalIndexProvider(rollback_reader, nullptr)
          .ResolveVisibleEntry(rollback_state,
                               rollback_index,
                               IdPredicate("3"));
  Require(rolled_back_index_lookup.ok &&
              rolled_back_index_lookup.rows.empty(),
          "rolled-back index membership remained visible as a ghost entry");
  api::MgaOrderedBtreeTransactionalIndexProvider rollback_provider(
      rollback_writer, nullptr);
  const auto recovered_rollback =
      rollback_provider.RecoverInterruptedMutation(rollback_state);
  Require(recovered_rollback.ok &&
              recovered_rollback.lifecycle_state == "abandoned_by_inventory",
          "rolled-back index mutation recovery classification was incorrect");
  const auto aborted = rollback_provider.AbortTransaction(rollback_state);
  Require(aborted.ok && aborted.lifecycle_state == "invisible_by_inventory",
          "index provider abort did not follow inventory rollback");
  Commit(database_path, rollback_reader);

  RequireTransactionalIndexLifecycle(database_path);
}

}  // namespace

int main(int argc, char** argv) {
  const bool descriptor_only = argc == 2 && std::string_view(argv[1]) == "--typed-descriptor-only";
  Require(argc == 1 || descriptor_only ||
              (argc == 2 && std::string_view(argv[1]) == "--text-only"),
          "unknown DML conformance mode");
  ConfigureMemoryFixture();
  const auto work = MakeTempDir();
  Require(!work.empty(), "failed to create temp directory");
  std::cout << "dml_mga_artifacts=" << work << std::endl;
  const auto database_path = work / "sbsfc021.sbdb";

  const auto created =
      scratchbird::tests::database_lifecycle::CreateCredentialedDatabaseFixture(
          database_path, SB_SBSFC021_SEED_PACK_ROOT);
  Require(created.ok(), "credentialed lifecycle fixture database create failed");
  Require(created.state.database_uuid.valid(),
          "credentialed lifecycle fixture database UUID missing");
  Require(created.bootstrap_principal_uuid.valid(),
          "credentialed lifecycle fixture principal UUID missing");
  g_database_uuid = scratchbird::core::uuid::UuidToString(
      created.state.database_uuid.value);
  g_principal_uuid = scratchbird::core::uuid::UuidToString(
      created.bootstrap_principal_uuid.value);
  Require(std::filesystem::exists(database_path), "lifecycle create did not create database file");

  auto open = Dispatch(database_path,
                       "lifecycle.open_database",
                       "SBLR_LIFECYCLE_OPEN_DATABASE",
                       BaseContext(database_path));
  Require(open.api_result.ok, "lifecycle open failed");

  CreateSchemaAndTable(database_path);
  if (descriptor_only) {
    VerifyTypedDeleteDescriptorContract(database_path);
    VerifyTypedUpdateDescriptorContract(database_path);
    std::filesystem::remove_all(work);
    std::cout << "sbsql_dml_typed_descriptor_conformance=passed\n";
    return EXIT_SUCCESS;
  }
  VerifyTextTargetAuthority(database_path);
  VerifyTypedTextUpdateContract(database_path);
  if (argc == 2 && std::string_view(argv[1]) == "--text-only") {
    std::filesystem::remove_all(work);
    std::cout << "sbsql_dml_text_update_conformance=passed\n";
    return EXIT_SUCCESS;
  }
  VerifyTypedUpdateAllRowsContract(database_path);
  VerifyTypedUpdateDescriptorContract(database_path);
  VerifyTypedDeleteDescriptorContract(database_path);
  VerifyDmlRowEffects(database_path);

  std::filesystem::remove_all(work);
  std::cout << "sbsql_dml_mga_row_result_conformance=passed\n";
  return EXIT_SUCCESS;
}
