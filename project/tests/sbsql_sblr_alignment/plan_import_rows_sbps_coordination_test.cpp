// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/import_api.hpp"
#include "engine/sblr/sblr_opcode_stream.hpp"
#include "hash_digest.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "runtime_platform.hpp"
#include "scratchbird/engine/sblr_envelope.hpp"
#include "sblr_executor_availability_registry.hpp"
#include "server/sblr_admission.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include "server/session_registry.hpp"
#include "server/sbps.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace bridge = scratchbird::server_engine_bridge;
namespace codec = scratchbird::engine::sblr;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace sbps = scratchbird::server::sbps;
namespace server = scratchbird::server;
namespace uuid = scratchbird::core::uuid;
namespace wire = scratchbird::engine;

using Bytes = std::vector<std::uint8_t>;

constexpr std::string_view kInt32DescriptorUuid =
    "019d0000-0000-7000-8000-00000000d716";
constexpr std::string_view kInt32TypeUuid =
    "019d0000-0000-7000-8000-00000000d717";
constexpr std::uint64_t kFixtureEpochMillis = 1947000000000ull;
constexpr std::uint64_t kOccurrence = 1;

[[noreturn]] void Fail(std::string_view detail) {
  std::cerr << "plan_import_rows_sbps_coordination: " << detail << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view detail) {
  if (!condition) Fail(detail);
}

template <typename Result>
void RequireOk(const Result& result, std::string_view detail) {
  if (!result.ok) {
    if constexpr (requires { result.diagnostic; }) {
      std::cerr << result.diagnostic.code << ':'
                << result.diagnostic.message_key << ':'
                << result.diagnostic.detail << '\n';
    } else if constexpr (requires { result.diagnostics; }) {
      for (const auto& diagnostic : result.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
                  << diagnostic.detail << '\n';
      }
    }
    Fail(detail);
  }
}

std::uint64_t NowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::TypedUuid NewTypedUuid(platform::UuidKind kind) {
  static std::atomic<std::uint64_t> sequence{1};
  const auto timestamp =
      kFixtureEpochMillis + sequence.fetch_add(1, std::memory_order_relaxed);
  if (!uuid::UuidKindAllowsDurableIdentity(kind)) {
    const auto raw = uuid::GenerateCompatibilityUnixTimeV7(timestamp);
    Require(raw.ok(), "compatibility UUID generation failed");
    const auto typed = uuid::MakeTypedUuid(kind, raw.value);
    Require(typed.ok(), "engine UUID typing failed");
    return typed.value;
  }
  const auto durable = uuid::GenerateEngineIdentityV7(kind, timestamp);
  Require(durable.ok(), "durable engine UUID generation failed");
  return durable.value;
}

std::string UuidText(const platform::TypedUuid& value) {
  return uuid::UuidToString(value.value);
}

std::string UuidText(const std::array<std::uint8_t, 16>& value) {
  platform::Uuid raw;
  raw.bytes = value;
  return uuid::UuidToString(raw);
}

std::array<std::uint8_t, 16> UuidBytes(std::string_view text) {
  const auto parsed = uuid::ParseUuid(std::string(text));
  Require(parsed.ok(), "UUID parsing failed");
  return parsed.value.bytes;
}

sb_engine_uuid_t PublicUuid(const platform::TypedUuid& value) {
  sb_engine_uuid_t result{};
  std::memcpy(result.bytes, value.value.bytes.data(), value.value.bytes.size());
  return result;
}

bool Nonzero(const auto& bytes) {
  return std::any_of(bytes.begin(), bytes.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

bool HasDiagnostic(const server::SessionOperationResult& result,
                   std::string_view code) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const auto& diagnostic) {
                       return diagnostic.code == code;
                     });
}

void RequireRefusal(const server::SessionOperationResult& result,
                    std::string_view code,
                    std::string_view detail) {
  Require(!result.accepted && result.payload.empty() &&
              result.response_message_type == 393 &&
              result.response_schema_id == 7406 &&
              result.frame_flags == (sbps::kFlagResponse |
                                     sbps::kFlagError |
                                     sbps::kFlagFinal) &&
              result.diagnostics.size() == 1 && HasDiagnostic(result, code),
          detail);
}

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  Require(input.good(), "durable byte source could not be opened");
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::string Int32Descriptor() {
  return "type=int32;datatype_descriptor_uuid=" +
         std::string(kInt32DescriptorUuid) + ";type_uuid=" +
         std::string(kInt32TypeUuid) + ";nullable=false";
}

struct Fixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  platform::TypedUuid database = NewTypedUuid(platform::UuidKind::database);
  platform::TypedUuid filespace = NewTypedUuid(platform::UuidKind::filespace);
  platform::TypedUuid principal = NewTypedUuid(platform::UuidKind::principal);
  platform::TypedUuid session = NewTypedUuid(platform::UuidKind::session);
  platform::TypedUuid schema = NewTypedUuid(platform::UuidKind::schema);
  platform::TypedUuid relation = NewTypedUuid(platform::UuidKind::object);
  std::shared_ptr<std::atomic_bool> cancelled =
      std::make_shared<std::atomic_bool>(false);
  api::EngineRequestContext transaction;
  api::MgaRelationStorageDescriptor relation_descriptor;

  Fixture() = default;
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
  Fixture(Fixture&& other) noexcept
      : directory(std::move(other.directory)),
        database_path(std::move(other.database_path)),
        database(other.database),
        filespace(other.filespace),
        principal(other.principal),
        session(other.session),
        schema(other.schema),
        relation(other.relation),
        cancelled(std::move(other.cancelled)),
        transaction(std::move(other.transaction)),
        relation_descriptor(std::move(other.relation_descriptor)) {
    other.directory.clear();
  }

  ~Fixture() {
    std::error_code ignored;
    if (!directory.empty()) std::filesystem::remove_all(directory, ignored);
  }
};

api::EngineRequestContext BaseContext(const Fixture& fixture) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "plan-import-rows-sbps-coordination";
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = UuidText(fixture.database);
  context.database_page_size_bytes = 16384;
  context.default_root_uuid.canonical = UuidText(fixture.filespace);
  context.current_schema_uuid.canonical = UuidText(fixture.schema);
  context.principal_uuid.canonical = UuidText(fixture.principal);
  context.session_uuid.canonical = UuidText(fixture.session);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.optimizer_route_epoch = 1;
  context.optimizer_route_generation = 1;
  context.optimizer_memory_budget_bytes = 64ull * 1024ull * 1024ull;
  context.optimizer_maximum_candidate_count = 131072;
  context.optimizer_maximum_memo_groups = 131072;
  context.optimizer_maximum_search_steps = 1048576;
  context.optimizer_maximum_planning_time_ns = 5'000'000'000ull;
  context.optimizer_spill_allowed = true;
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      UuidText(NewTypedUuid(platform::UuidKind::object));
  context.authorization_context.security_context_generation = 1;
  context.authorization_context.principal_uuid = context.principal_uuid;
  context.authorization_context.security_epoch = context.security_epoch;
  context.authorization_context.policy_epoch = 1;
  context.authorization_context.catalog_generation_id =
      context.catalog_generation_id;
  api::EngineAuthorizationSubject subject;
  subject.subject_uuid = context.principal_uuid;
  subject.subject_kind = "principal";
  context.authorization_context.effective_subjects.push_back(
      std::move(subject));
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical =
      UuidText(NewTypedUuid(platform::UuidKind::object));
  grant.subject_uuid = context.principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical = UuidText(fixture.relation);
  grant.right = "INSERT";
  grant.security_epoch = context.security_epoch;
  context.authorization_context.grants.push_back(std::move(grant));
  context.query_cancellation_requested = [flag = fixture.cancelled]() {
    return flag->load();
  };
  return context;
}

Fixture MakeFixture() {
  Fixture fixture;
  fixture.directory = std::filesystem::temp_directory_path() /
                      ("scratchbird_plan_import_sbps_" +
                       std::to_string(NowMillis()));
  std::filesystem::create_directories(fixture.directory);
  fixture.database_path = fixture.directory / "coordination.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = fixture.database;
  create.filespace_uuid = fixture.filespace;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = kFixtureEpochMillis;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  Require(db::CreateDatabaseFile(create).ok(),
          "fixture database creation failed");

  api::EngineBeginTransactionRequest begin;
  begin.context = BaseContext(fixture);
  begin.isolation_level = "repeatable_read";
  const auto begun = api::EngineBeginTransaction(begin);
  RequireOk(begun, "fixture transaction begin failed");
  fixture.transaction = begin.context;
  fixture.transaction.local_transaction_id = begun.local_transaction_id;
  fixture.transaction.transaction_uuid = begun.transaction_uuid;
  fixture.transaction.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  fixture.transaction.transaction_isolation_level = begun.isolation_level;

  api::CrudTableRecord table;
  table.creator_tx = fixture.transaction.local_transaction_id;
  table.table_uuid = UuidText(fixture.relation);
  table.default_name = "plan_import_rows_target";
  table.columns = {{"id", Int32Descriptor()}};
  Require(!api::AppendMgaTableMetadata(fixture.transaction, table).error,
          "fixture table metadata append failed");
  const auto ensured = api::EnsureMgaRelationStorageDescriptor(
      fixture.transaction, table, {}, &fixture.relation_descriptor);
  Require(!ensured.error && fixture.relation_descriptor.columns.size() == 1,
          "fixture relation descriptor creation failed");
  return fixture;
}

class PublicSession {
 public:
  explicit PublicSession(const Fixture& fixture)
      : database_path_(fixture.database_path.string()) {
    sb_engine_open_params_v1_t open{};
    open.struct_size = sizeof(open);
    open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    open.database_path_utf8 = database_path_.data();
    open.database_path_size = database_path_.size();
    open.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
    Require(sb_engine_open(&open, &engine_, nullptr) == SB_ENGINE_STATUS_OK &&
                engine_ != nullptr,
            "public engine open failed");

    sb_engine_session_params_v1_t begin{};
    begin.struct_size = sizeof(begin);
    begin.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    begin.effective_user_uuid = PublicUuid(fixture.principal);
    begin.session_uuid = PublicUuid(fixture.session);
    begin.default_language_utf8 = "en";
    begin.default_language_size = 2;
    begin.trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
    Require(sb_engine_session_begin(engine_, &begin, &session_, nullptr) ==
                    SB_ENGINE_STATUS_OK &&
                session_ != nullptr,
            "public engine session begin failed");
  }

  PublicSession(const PublicSession&) = delete;
  PublicSession& operator=(const PublicSession&) = delete;

  ~PublicSession() {
    if (session_ != nullptr) {
      sb_engine_session_end_params_v1_t end{};
      end.struct_size = sizeof(end);
      end.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
      end.rollback_active_transactions = 1;
      end.cancel_open_results = 1;
      (void)sb_engine_session_end(session_, &end, nullptr);
    }
    if (engine_ != nullptr) (void)sb_engine_close(engine_, nullptr);
  }

  sb_engine_session_t get() const { return session_; }

 private:
  std::string database_path_;
  sb_engine_handle_t engine_ = nullptr;
  sb_engine_session_t session_ = nullptr;
};

bridge::StatementContextReceiptHandle AcquireReceipt(
    const PublicSession& session,
    const api::EngineRequestContext& context,
    bridge::StatementContextReceiptView* view) {
  bridge::StatementContextAcquireRequest request;
  request.engine_context = &context;
  request.exact_transaction_uuid = context.transaction_uuid.canonical;
  bridge::StatementContextReceiptHandle receipt;
  sb_engine_result_t engine_result = nullptr;
  const auto status = bridge::AcquireStatementContextReceipt(
      session.get(), &request, &receipt, view, &engine_result);
  if (engine_result != nullptr) (void)sb_engine_result_release(engine_result);
  Require(status == SB_ENGINE_STATUS_OK && receipt,
          "statement receipt acquisition failed");
  return receipt;
}

api::SblrExecutorAvailabilityRowIdentity AvailabilityIdentity() {
  return {api::kSblrDmlPlanImportRowsExecutorId,
          api::kSblrDmlPlanImportRowsOpcodeCode,
          api::kSblrDmlPlanImportRowsOpcodeVersion,
          api::kSblrDmlPlanImportRowsOperandDescriptorId,
          api::kSblrDmlPlanImportRowsResultDescriptorId,
          api::kSblrDmlPlanImportRowsResultDescriptorVersion};
}

std::vector<std::uint8_t> Demand(
    const bridge::StatementContextReceiptView& view,
    std::string_view target_table_uuid,
    std::vector<std::array<std::uint8_t, 16>> mapping_targets = {}) {
  constexpr std::size_t kHeaderBytes = 120;
  constexpr std::size_t kMappingBytes = 24;
  const auto total = kHeaderBytes + mapping_targets.size() * kMappingBytes;
  std::vector<std::uint8_t> bytes(total, 0);
  std::copy_n("IPRQ", 4, bytes.begin());
  platform::StoreLittle16(bytes.data() + 4, 1);
  platform::StoreLittle16(bytes.data() + 6, kHeaderBytes);
  platform::StoreLittle32(bytes.data() + 8,
                          static_cast<std::uint32_t>(total));
  const auto receipt = UuidBytes(view.receipt_uuid);
  std::copy(receipt.begin(), receipt.end(), bytes.begin() + 16);
  platform::StoreLittle64(bytes.data() + 32, kOccurrence);
  const auto target = UuidBytes(target_table_uuid);
  std::copy(target.begin(), target.end(), bytes.begin() + 40);
  platform::StoreLittle16(
      bytes.data() + 56,
      static_cast<std::uint16_t>(codec::PlanImportRowsSourceKindV1::csv_stream));
  platform::StoreLittle16(
      bytes.data() + 58,
      static_cast<std::uint16_t>(codec::PlanImportRowsFormatFamilyV1::csv));
  bytes[60] = static_cast<std::uint8_t>(
      codec::PlanImportRowsRejectModeV1::fail_fast);
  bytes[61] = static_cast<std::uint8_t>(
      codec::PlanImportRowsRejectPayloadPolicyV1::diagnostic_only);
  bytes[62] = static_cast<std::uint8_t>(
      codec::PlanImportRowsResumePolicyV1::fail_closed);
  platform::StoreLittle32(bytes.data() + 68,
                          static_cast<std::uint32_t>(mapping_targets.size()));
  for (std::size_t index = 0; index < mapping_targets.size(); ++index) {
    const auto offset = kHeaderBytes + index * kMappingBytes;
    platform::StoreLittle32(bytes.data() + offset,
                            static_cast<std::uint32_t>(index));
    platform::StoreLittle32(bytes.data() + offset + 4,
                            codec::kPlanImportRowsMappingRequiredV1);
    std::copy(mapping_targets[index].begin(), mapping_targets[index].end(),
              bytes.begin() + offset + 8);
  }
  return bytes;
}

sbps::Frame RequestFrame(const server::ServerSessionRecord& session,
                         std::vector<std::uint8_t> payload) {
  sbps::Frame frame;
  frame.header.message_type = 392;
  frame.header.payload_schema_id = 7405;
  frame.header.stream_id = 19;
  frame.header.sequence_number = 1;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session.connection_uuid;
  frame.header.session_uuid = session.session_uuid;
  frame.header.payload_len = static_cast<std::uint32_t>(payload.size());
  frame.payload = std::move(payload);
  return frame;
}

void AppendU16(Bytes* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value));
  out->push_back(static_cast<std::uint8_t>(value >> 8U));
}

void AppendU32(Bytes* out, std::uint32_t value) {
  for (unsigned byte = 0; byte != 4; ++byte) {
    out->push_back(static_cast<std::uint8_t>(value >> (byte * 8U)));
  }
}

void AppendU64(Bytes* out, std::uint64_t value) {
  for (unsigned byte = 0; byte != 8; ++byte) {
    out->push_back(static_cast<std::uint8_t>(value >> (byte * 8U)));
  }
}

Bytes ValueU16(std::uint16_t value) {
  Bytes out;
  AppendU16(&out, value);
  return out;
}

Bytes ValueU32(std::uint32_t value) {
  Bytes out;
  AppendU32(&out, value);
  return out;
}

Bytes ValueU64(std::uint64_t value) {
  Bytes out;
  AppendU64(&out, value);
  return out;
}

Bytes OptionalUuid(const std::array<std::uint8_t, 16>& value) {
  Bytes out{1};
  out.insert(out.end(), value.begin(), value.end());
  return out;
}

Bytes CanonicalStruct(std::uint32_t format, std::uint8_t version) {
  Bytes out;
  AppendU32(&out, format);
  AppendU16(&out, 1);
  AppendU16(&out, 0);
  AppendU64(&out, 1);
  out.push_back(version);
  return out;
}

codec::SblrOperationEnvelope PackageBoundary(
    bool begin, std::string_view parser_uuid, std::string_view registry_uuid,
    const std::array<std::uint8_t, 16>& package_uuid) {
  auto operation = codec::MakeSblrEnvelope(
      begin ? "engine.op.package_begin" : "engine.op.package_end",
      begin ? "SBLR_PACKAGE_BEGIN" : "SBLR_PACKAGE_END",
      begin ? "plan_import_rows.public_abi.package_begin"
            : "plan_import_rows.public_abi.package_end");
  operation.opcode_code = begin ? 1 : 2;
  operation.result_shape = "void";
  operation.diagnostic_shape = "diagnostic_vector";
  operation.parser_package_uuid = parser_uuid;
  operation.registry_snapshot_uuid = registry_uuid;
  operation.parser_resolved_names_to_uuids = true;
  codec::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = begin ? "package.header" : "package.footer";
  operand.name = "package_descriptor";
  operand.value_kind = codec::SblrValueKind::descriptor_ref;
  operand.value_body.assign(package_uuid.begin(), package_uuid.end());
  operation.operands.push_back(std::move(operand));
  return operation;
}

struct PlanSubmission {
  std::string container;
  std::string execution_envelope;
  Bytes opcode_stream;
};

PlanSubmission BuildPlanSubmission(
    const Fixture& fixture, const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid,
    const std::vector<std::uint8_t>& descriptor_ref_bytes) {
  const auto package_uuid = UuidBytes(view.bound_ast_uuid);
  auto member = codec::MakeSblrEnvelope(
      "dml.plan_import_rows", "SBLR_DML_PLAN_IMPORT_ROWS",
      "plan_import_rows.public_abi.member");
  member.opcode_code = codec::kPlanImportRowsOpcodeCodeV1;
  member.operation_version_major = 1;
  member.operation_version_minor = 0;
  member.result_shape = "import_plan_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.parser_resolved_names_to_uuids = true;
  codec::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "import_rows_plan_descriptor";
  operand.name = "request";
  operand.value_kind = codec::SblrValueKind::descriptor_ref;
  operand.value_body = descriptor_ref_bytes;
  member.operands.push_back(std::move(operand));

  codec::SblrOpcodeStream stream;
  stream.package_descriptor_uuid = view.bound_ast_uuid;
  stream.registry_snapshot_uuid = view.catalog_epoch_uuid;
  stream.operations.push_back(PackageBoundary(
      true, parser_uuid, view.catalog_epoch_uuid, package_uuid));
  stream.operations.push_back(std::move(member));
  stream.operations.push_back(PackageBoundary(
      false, parser_uuid, view.catalog_epoch_uuid, package_uuid));
  auto opcode_stream = codec::EncodeSblrOpcodeStream(stream);
  Require(!opcode_stream.empty(), "canonical plan-import SBOS encoding failed");

  const auto decoded_stream = codec::DecodeSblrOpcodeStream(
      std::string_view(reinterpret_cast<const char*>(opcode_stream.data()),
                       opcode_stream.size()));
  Require(decoded_stream.ok && decoded_stream.stream.operations.size() == 3,
          "canonical plan-import SBOS did not decode as standalone package");
  const auto& decoded_member = decoded_stream.stream.operations[1];
  Require(decoded_member.operation_id == "dml.plan_import_rows" &&
              decoded_member.opcode == "SBLR_DML_PLAN_IMPORT_ROWS" &&
              decoded_member.opcode_code == 793 &&
              decoded_member.operation_version_major == 1 &&
              decoded_member.operation_version_minor == 0 &&
              decoded_member.operands.size() == 1 &&
              decoded_member.operands.front().ordinal == 1 &&
              decoded_member.operands.front().type ==
                  "import_rows_plan_descriptor" &&
              decoded_member.operands.front().name == "request" &&
              decoded_member.operands.front().value_kind ==
                  codec::SblrValueKind::descriptor_ref &&
              decoded_member.operands.front().value_body ==
                  descriptor_ref_bytes,
          "canonical plan-import member identity or descriptor-ref drifted");

  const auto database_uuid = UuidBytes(UuidText(fixture.database));
  const auto dialect_uuid =
      NewTypedUuid(platform::UuidKind::object).value.bytes;
  const auto parser_uuid_bytes = UuidBytes(parser_uuid);
  const auto registry_uuid = UuidBytes(view.catalog_epoch_uuid);
  const auto statement_uuid = UuidBytes(view.statement_uuid);
  const auto principal_uuid = UuidBytes(UuidText(fixture.principal));
  wire::SblrCanonicalContainer container;
  std::copy(database_uuid.begin(), database_uuid.end(),
            container.canonical_anchor.begin());
  std::copy(dialect_uuid.begin(), dialect_uuid.end(),
            container.canonical_anchor.begin() + 16);
  std::copy(parser_uuid_bytes.begin(), parser_uuid_bytes.end(),
            container.canonical_anchor.begin() + 32);
  const auto put32 = [&](std::size_t offset, std::uint32_t value) {
    for (unsigned byte = 0; byte != 4; ++byte) {
      container.canonical_anchor[offset + byte] =
          static_cast<std::uint8_t>(value >> (byte * 8U));
    }
  };
  const auto put64 = [&](std::size_t offset, std::uint64_t value) {
    for (unsigned byte = 0; byte != 8; ++byte) {
      container.canonical_anchor[offset + byte] =
          static_cast<std::uint8_t>(value >> (byte * 8U));
    }
  };
  put32(48, 1);
  put64(52, 1);
  put64(60, 1);
  put64(68, 1);
  std::copy(registry_uuid.begin(), registry_uuid.end(),
            container.canonical_anchor.begin() + 76);
  put64(92, 1);
  container.canonical_anchor[100] = 1;
  std::copy(statement_uuid.begin(), statement_uuid.end(),
            container.canonical_anchor.begin() + 116);
  container.operation_payload = opcode_stream;
  const auto encoded_container = wire::EncodeSblrContainer(container);
  Require(!encoded_container.empty(),
          "canonical plan-import outer container encoding failed");

  wire::SblrExecutionEnvelopeV1 execution;
  auto& fields = execution.fields;
  fields[0] = Bytes(statement_uuid.begin(), statement_uuid.end());
  fields[1] = ValueU16(1);
  fields[2] = ValueU16(0);
  fields[3] = ValueU32(0x10001);
  fields[4] = ValueU16(1);
  fields[5] = {1};
  AppendU64(&fields[5], opcode_stream.size());
  fields[5].insert(fields[5].end(), opcode_stream.begin(), opcode_stream.end());
  fields[6] = {0};
  fields[7] = {1};
  AppendU32(&fields[7],
            wire::SblrCrc32c(opcode_stream.data(), opcode_stream.size()));
  fields[8] = ValueU64(opcode_stream.size());
  fields[9] = ValueU16(1);
  fields[10] = OptionalUuid(dialect_uuid);
  fields[11] = OptionalUuid(principal_uuid);
  fields[12] = CanonicalStruct(0x1001, 1);
  fields[13] = CanonicalStruct(0x1002, 2);
  fields[14] = {0};
  fields[15] = ValueU64(1);
  fields[16] = ValueU32(0);
  fields[17] = ValueU32(0);
  fields[18] = ValueU32(0);
  fields[19] = {0};
  fields[20] = ValueU32(0);
  fields[21] = CanonicalStruct(0x1005, 5);
  fields[22] = {0};
  fields[23] = {0};
  fields[24] = {0};
  fields[25] = ValueU16(0);
  fields[26] = {0};
  fields[27] = {0};
  const auto encoded_execution =
      wire::EncodeSblrExecutionEnvelopeV1(execution);
  Require(!encoded_execution.empty(),
          "canonical plan-import execution envelope encoding failed");
  return {{encoded_container.begin(), encoded_container.end()},
          {encoded_execution.begin(), encoded_execution.end()},
          std::move(opcode_stream)};
}

bridge::StatementContextDispatchRequest MakeDispatchRequest(
    bridge::StatementContextReceiptHandle receipt,
    sb_engine_session_t engine_session,
    bridge::StatementPackageAdmissionReservationHandle reservation,
    const server::ServerSblrAdmissionToken& token) {
  bridge::StatementContextDispatchRequest request;
  request.receipt = receipt;
  request.engine_session = engine_session;
  request.canonical_container_bytes = token->canonical_container_bytes;
  request.canonical_execution_envelope_bytes =
      token->canonical_execution_envelope_bytes;
  request.canonical_operation_bytes = token->canonical_operation_bytes;
  request.container_sha256 = token->container_sha256;
  request.execution_envelope_sha256 = token->execution_envelope_sha256;
  request.operation_sha256 = token->operation_sha256;
  request.admission_binding_sha256 = token->admission_binding_sha256;
  request.authenticated_principal_uuid = token->authenticated_principal_uuid;
  request.catalog_snapshot_uuid = token->catalog_snapshot_uuid;
  request.engine_mga_statement_uuid = token->engine_mga_statement_uuid;
  request.engine_mga_snapshot_uuid = token->engine_mga_snapshot_uuid;
  request.catalog_epoch = token->catalog_epoch;
  request.security_epoch = token->security_epoch;
  request.resource_epoch = token->resource_epoch;
  request.package_admission_reservation = reservation;
  request.admitted_payload_kind =
      bridge::StatementSblrPayloadKind::kOpcodeStream;
  request.gateway_evidence.source =
      bridge::StatementGatewayEvidenceSource::kLocalObserved;
  request.gateway_evidence.disposition =
      bridge::StatementGatewayDisposition::kPassThrough;
  request.gateway_evidence.provider_observation_generation =
      token->gateway_evidence.provider_observation_generation;
  request.gateway_evidence.canonical_payload_sha256 =
      token->gateway_evidence.canonical_payload_sha256;
  request.gateway_evidence.route_snapshot_uuid =
      token->gateway_evidence.route_snapshot_uuid;
  request.gateway_evidence.route_epoch = token->gateway_evidence.route_epoch;
  request.gateway_evidence.route_generation =
      token->gateway_evidence.route_generation;
  request.gateway_evidence.security_snapshot_uuid =
      token->gateway_evidence.security_snapshot_uuid;
  request.gateway_evidence.security_epoch =
      token->gateway_evidence.security_epoch;
  request.gateway_evidence.security_observation_generation =
      token->gateway_evidence.security_observation_generation;
  request.gateway_evidence.cluster_context_active =
      token->gateway_evidence.cluster_context_active;
  request.gateway_evidence.cluster_transaction_active =
      token->gateway_evidence.cluster_transaction_active;
  request.gateway_evidence.route_fence_present =
      token->gateway_evidence.route_fence_present;
  request.package_executor_evidence.begin_executor_id =
      token->package_executor_evidence.begin_executor_id;
  request.package_executor_evidence.end_executor_id =
      token->package_executor_evidence.end_executor_id;
  request.package_executor_evidence.registry_snapshot_uuid =
      token->package_executor_evidence.registry_snapshot_uuid;
  request.package_executor_evidence.executor_evidence_generation =
      token->package_executor_evidence.executor_evidence_generation;
  request.package_executor_evidence.canonical_payload_sha256 =
      token->package_executor_evidence.canonical_payload_sha256;
  return request;
}

std::string HexLower(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    result.push_back(kDigits[byte >> 4U]);
    result.push_back(kDigits[byte & 0x0fU]);
  }
  return result;
}

void TestCoordination() {
  Require(sbps::IsKnownMessageType(392) && sbps::IsKnownMessageType(393) &&
              static_cast<std::uint16_t>(
                  sbps::MessageType::kCoordinateDmlPlanImportRowsBindRequest) ==
                  392 &&
              static_cast<std::uint16_t>(
                  sbps::MessageType::kCoordinateDmlPlanImportRowsBindResult) ==
                  393 &&
              sbps::kSchemaCoordinateDmlPlanImportRowsBindRequestV1 == 7405 &&
              sbps::kSchemaCoordinateDmlPlanImportRowsBindResultV1 == 7406,
          "dedicated message/schema constants are not registered exactly");

  auto fixture = MakeFixture();
  const auto availability_path = std::filesystem::path(
      fixture.transaction.database_path +
      ".sb.sblr_executor_availability_registry.v1.dml_plan_import_rows");
  Require(!std::filesystem::exists(availability_path),
          "fresh fixture already had plan-import availability state");

  PublicSession public_session(fixture);
  bridge::StatementContextReceiptView view;
  const auto receipt =
      AcquireReceipt(public_session, fixture.transaction, &view);
  const auto available = api::LoadCurrentSblrExecutorAvailabilitySnapshot(
      fixture.transaction, AvailabilityIdentity());
  RequireOk(available, "receipt acquisition did not bootstrap availability");
  Require(available.snapshot.installed && available.snapshot.generation != 0 &&
              std::filesystem::exists(availability_path),
          "receipt bootstrap did not publish exact plan-import availability");
  Require(!view.receipt_uuid.empty() && !view.resource_admission_uuid.empty(),
          "receipt omitted statement or resource-admission authority");
  api::EngineRequestContext copied_context;
  Require(bridge::CopyStatementContextEngineContextV1(
              receipt, &copied_context, nullptr) == SB_ENGINE_STATUS_OK &&
              copied_context.statement_receipt_uuid.canonical ==
                  view.receipt_uuid &&
              copied_context.resource_admission_uuid.canonical ==
                  view.resource_admission_uuid,
          "opaque receipt context copy lost import authority");

  server::ServerSessionRegistry registry;
  server::ServerSessionRecord live;
  live.connection_uuid = sbps::MakeUuidV7Bytes();
  live.server_channel_uuid = sbps::MakeUuidV7Bytes();
  live.channel_state = server::ServerChannelState::kReady;
  live.session_uuid = UuidBytes(UuidText(fixture.session));
  live.auth_context_uuid = sbps::MakeUuidV7Bytes();
  live.principal_uuid = UuidBytes(UuidText(fixture.principal));
  live.effective_user_uuid = live.principal_uuid;
  live.database_path = fixture.database_path.string();
  live.database_uuid = UuidText(fixture.database);
  live.catalog_generation = view.catalog_generation_id;
  live.security_epoch = view.security_epoch;
  live.resource_epoch = view.resource_epoch;
  live.local_transaction_id = view.owning_local_transaction_id;
  live.default_local_transaction_id = view.owning_local_transaction_id;
  live.transaction_uuid = view.owning_transaction_uuid;
  live.snapshot_visible_through_local_transaction_id =
      view.visible_committed_high_watermark;
  live.session_binding_present = true;
  registry.physical_channel_by_connection_uuid.emplace(
      server::UuidBytesToText(live.connection_uuid), live.server_channel_uuid);
  registry.sessions_by_uuid.emplace(server::UuidBytesToText(live.session_uuid),
                                    live);

  server::ServerStatementContextRecord statement;
  statement.session_uuid = live.session_uuid;
  statement.statement_uuid = view.statement_uuid;
  statement.owning_local_transaction_id = view.owning_local_transaction_id;
  statement.owning_transaction_uuid = view.owning_transaction_uuid;
  statement.receipt = receipt;
  statement.view = view;
  registry.statement_contexts_by_statement_uuid.emplace(statement.statement_uuid,
                                                         statement);

  server::HostedEngineState engine_state;
  const auto canonical = Demand(view, UuidText(fixture.relation));
  Require(canonical.size() == 120 &&
              std::equal(canonical.begin(), canonical.begin() + 4,
                         std::string_view("IPRQ").begin()),
          "canonical IPRQ fixture is not exact 120-byte zero-map demand");

  const auto database_before = ReadBytes(fixture.database_path);
  const auto availability_before = ReadBytes(availability_path);

  auto malformed_payload = canonical;
  malformed_payload[60] = 0xff;
  auto malformed = RequestFrame(live, std::move(malformed_payload));
  malformed.header.session_uuid = sbps::MakeUuidV7Bytes();
  RequireRefusal(server::HandleCoordinateDmlPlanImportRowsBind(
                     &registry, engine_state, malformed),
                 "SBLR.OPERAND_INVALID",
                 "malformed policy demand did not precede authentication");

  auto unauthorized_payload =
      Demand(view, UuidText(NewTypedUuid(platform::UuidKind::object)));
  auto unauthorized = RequestFrame(live, std::move(unauthorized_payload));
  RequireRefusal(server::HandleCoordinateDmlPlanImportRowsBind(
                     &registry, engine_state, unauthorized),
                 "SECURITY.ACCESS_DENIED",
                 "ungranted INSERT target was not refused exactly");

  const auto mapping_target =
      NewTypedUuid(platform::UuidKind::object).value.bytes;
  auto nonzero_mapping = RequestFrame(
      live, Demand(view, UuidText(fixture.relation), {mapping_target}));
  RequireRefusal(server::HandleCoordinateDmlPlanImportRowsBind(
                     &registry, engine_state, nonzero_mapping),
                 "SBLR.OPERATION_UNSUPPORTED",
                 "nonzero generation-free mapping demand was not refused");

  api::EngineRequestContext binder_context = copied_context;
  binder_context.trace_tags = {"private_dml_plan_import_rows_binder"};
  const auto refused_release =
      api::ReleaseEngineBoundImportRowsPlanDescriptorsV1(binder_context);
  RequireOk(refused_release, "refusal publication audit failed");
  Require(refused_release.released_row_count == 0,
          "a refused IPRQ published a bound descriptor row");

  auto success_request = RequestFrame(live, canonical);
  const auto success = server::HandleCoordinateDmlPlanImportRowsBind(
      &registry, engine_state, success_request);
  Require(success.accepted && success.diagnostics.empty() &&
              success.response_message_type == 393 &&
              success.response_schema_id == 7406 &&
              success.frame_flags ==
                  (sbps::kFlagResponse | sbps::kFlagFinal) &&
              success.payload.size() == 24,
          "successful bind was not exact 393/7406 24-byte result");

  sbps::FrameHeader success_header;
  success_header.message_type = success.response_message_type;
  success_header.flags = success.frame_flags;
  success_header.payload_schema_id = success.response_schema_id;
  success_header.stream_id = success_request.header.stream_id;
  success_header.sequence_number = success_request.header.sequence_number;
  success_header.request_uuid = success_request.header.request_uuid;
  success_header.connection_uuid = success_request.header.connection_uuid;
  success_header.session_uuid = success.session_uuid;
  const auto encoded_success =
      sbps::EncodeFrame(success_header, success.payload);
  const auto decoded_success =
      sbps::DecodeFrameBytes(encoded_success, sbps::kHeaderBytes + 24);
  Require(decoded_success.ok() &&
              decoded_success.frame->header.message_type == 393 &&
              decoded_success.frame->header.payload_schema_id == 7406 &&
              decoded_success.frame->header.stream_id ==
                  success_request.header.stream_id &&
              decoded_success.frame->header.request_uuid ==
                  success_request.header.request_uuid &&
              decoded_success.frame->header.connection_uuid ==
                  success_request.header.connection_uuid &&
              decoded_success.frame->header.session_uuid ==
                  success_request.header.session_uuid &&
              decoded_success.frame->payload == success.payload,
          "IPRS frame was altered or decorrelated");

  codec::PlanImportRowsDescriptorRefV1 descriptor_ref;
  codec::PlanImportRowsCodecDiagnosticV1 codec_diagnostic;
  Require(codec::DecodePlanImportRowsDescriptorRefV1(
              success.payload.data(), success.payload.size(), &descriptor_ref,
              &codec_diagnostic) &&
              Nonzero(descriptor_ref.descriptor_uuid) &&
              descriptor_ref.descriptor_generation != 0,
          "IPRS did not carry one exact descriptor reference");

  api::EnginePlanImportRowsRequest plan;
  plan.context = copied_context;
  plan.context.trace_tags = {"private_dml_plan_import_rows_consumer"};
  plan.operation_id = "dml.plan_import_rows";
  plan.descriptor_ref = descriptor_ref;
  const auto planned = api::EnginePlanImportRows(plan);
  RequireOk(planned, "bound descriptor was not consumed by EnginePlanImportRows");
  Require(planned.surface_accepted && planned.planning_only &&
              planned.execution_requires_execute_import_rows &&
              !planned.row_execution_completed &&
              !planned.row_persistence_claimed &&
              planned.normalized_insert_mode_code == 1 &&
              planned.normalized_source_kind_code == 2 &&
              planned.normalized_format_family_code == 1 &&
              planned.mapped_column_count == 0 &&
              planned.validated_request_descriptor_uuid.canonical ==
                  UuidText(descriptor_ref.descriptor_uuid) &&
              planned.validated_request_descriptor_generation ==
                  descriptor_ref.descriptor_generation &&
              Nonzero(planned.validated_request_projection_sha256),
          "the exact twelve import_plan_result extensions drifted");

  std::vector<std::uint8_t> evidence_bytes;
  codec_diagnostic = {};
  Require(codec::EncodePlanImportRowsExecutorEvidenceV1(
              planned.accepted_executor_evidence, &evidence_bytes,
              &codec_diagnostic) &&
              evidence_bytes.size() ==
                  codec::kPlanImportRowsExecutorEvidenceBytesV1 &&
              evidence_bytes == planned.accepted_executor_evidence.exact_bytes &&
              platform::LoadLittle16(evidence_bytes.data() + 96) == 793 &&
              platform::LoadLittle16(evidence_bytes.data() + 98) == 1 &&
              platform::LoadLittle16(evidence_bytes.data() + 100) == 0 &&
              platform::LoadLittle64(evidence_bytes.data() + 164) ==
                  codec::kPlanImportRowsAcceptedValidationBitsV1 &&
              planned.accepted_executor_evidence.request_descriptor_uuid ==
                  descriptor_ref.descriptor_uuid &&
              planned.accepted_executor_evidence
                      .request_descriptor_generation ==
                  descriptor_ref.descriptor_generation &&
              planned.accepted_executor_evidence.request_projection_sha256 ==
                  planned.validated_request_projection_sha256 &&
              planned.accepted_executor_evidence
                      .executor_availability_generation ==
                  available.snapshot.generation &&
              planned.accepted_executor_evidence.completed_validation_bits ==
                  codec::kPlanImportRowsAcceptedValidationBitsV1 &&
              Nonzero(planned.accepted_executor_evidence.evidence_sha256),
          "accepted IPEV was not exact 208-byte all-ten-gate evidence");

  const auto parser_uuid =
      UuidText(NewTypedUuid(platform::UuidKind::object));
  const auto submission =
      BuildPlanSubmission(fixture, view, parser_uuid, success.payload);
  bridge::StatementPackageAdmissionReservationRequest reservation_request;
  reservation_request.receipt = receipt;
  reservation_request.canonical_payload_bytes =
      submission.opcode_stream.data();
  reservation_request.canonical_payload_size =
      submission.opcode_stream.size();
  reservation_request.payload_kind =
      bridge::StatementSblrPayloadKind::kOpcodeStream;
  bridge::StatementPackageAdmissionReservationHandle reservation;
  bridge::StatementPackageAdmissionReservationView reservation_view;
  sb_engine_result_t reservation_result = nullptr;
  Require(bridge::AcquireStatementPackageAdmissionReservation(
              &reservation_request, &reservation, &reservation_view,
              &reservation_result) == SB_ENGINE_STATUS_OK &&
              reservation && reservation_view.record_count == 3,
          "canonical plan-import package reservation failed");
  if (reservation_result != nullptr) {
    (void)sb_engine_result_release(reservation_result);
  }

  server::ServerSblrAdmissionRequest admission;
  admission.encoded_sblr_container = submission.container;
  admission.encoded_execution_envelope = submission.execution_envelope;
  admission.admitted_parser_package_uuid = parser_uuid;
  admission.admitted_parser_package_version_major = 1;
  admission.admitted_registry_snapshot_uuid = view.catalog_epoch_uuid;
  admission.authenticated_principal_uuid = UuidText(fixture.principal);
  admission.catalog_snapshot_uuid = view.statement_metadata_snapshot_uuid;
  admission.engine_mga_statement_uuid = view.statement_uuid;
  admission.engine_mga_snapshot_uuid = view.statement_snapshot_uuid;
  admission.catalog_epoch = view.catalog_generation_id;
  admission.security_epoch = view.security_epoch;
  admission.resource_epoch = view.resource_epoch;
  admission.route_snapshot_uuid = view.optimizer_route_snapshot_uuid;
  admission.route_epoch = view.optimizer_route_epoch;
  admission.route_generation = view.optimizer_route_generation;
  admission.security_snapshot_uuid = view.security_context_uuid;
  admission.security_observation_generation = view.security_epoch;
  admission.route_snapshot_engine_owned = true;
  admission.security_snapshot_engine_owned = true;
  admission.package_reservation_handle = reservation.opaque_id;
  admission.reserved_payload_kind =
      server::ServerSblrPayloadKind::opcode_stream;
  admission.reserved_payload_size = reservation_view.payload_size;
  admission.reserved_record_count = reservation_view.record_count;
  admission.reserved_resource_policy_generation =
      reservation_view.resource_policy_generation;
  admission.reserved_payload_sha256 = reservation_view.payload_sha256;
  server::BindServerSblrGatewayReceiptObservation(view, &admission);
  const auto admitted = server::AdmitServerSblrEnvelope(admission);
  if (!admitted.admitted) {
    for (const auto& diagnostic : admitted.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    }
  }
  Require(admitted.admitted && admitted.admission_token &&
              admitted.admission_token->opcode_stream,
          "canonical code-793 standalone package was not admitted");

  auto dispatch = MakeDispatchRequest(receipt, public_session.get(),
                                      reservation,
                                      admitted.admission_token);
  sb_engine_result_t public_result = nullptr;
  const auto public_status = bridge::DispatchStatementContextReceipt(
      &dispatch, &public_result);
  if (public_status != SB_ENGINE_STATUS_OK && public_result != nullptr) {
    sb_engine_diagnostic_set_view_t diagnostics{};
    if (sb_engine_result_diagnostics(public_result, &diagnostics) ==
        SB_ENGINE_STATUS_OK) {
      for (std::uint64_t index = 0; index != diagnostics.diagnostic_count;
           ++index) {
        std::cerr.write(diagnostics.diagnostics[index].symbolic_code.data,
                        diagnostics.diagnostics[index]
                            .symbolic_code.size_bytes);
        std::cerr << ':';
        std::cerr.write(diagnostics.diagnostics[index].message_key.data,
                        diagnostics.diagnostics[index].message_key.size_bytes);
        std::cerr << ':';
        std::cerr.write(diagnostics.diagnostics[index].safe_detail.data,
                        diagnostics.diagnostics[index].safe_detail.size_bytes);
        std::cerr << '\n';
      }
    }
  }
  Require(public_status == SB_ENGINE_STATUS_OK && public_result != nullptr,
          "admitted plan-import did not reach public ABI result projection");

  sb_engine_result_class_t result_class = SB_ENGINE_RESULT_NONE;
  sb_engine_execution_summary_view_v1_t summary{};
  sb_engine_command_completion_view_v1_t completion{};
  sb_engine_diagnostic_set_view_t diagnostics{};
  Require(sb_engine_result_class(public_result, &result_class) ==
                  SB_ENGINE_STATUS_OK &&
              result_class == SB_ENGINE_RESULT_ROW_BATCH &&
              sb_engine_result_summary(public_result, &summary) ==
                  SB_ENGINE_STATUS_OK &&
              summary.rows_produced == 1 &&
              summary.diagnostics_count == 0 &&
              sb_engine_result_completion(public_result, &completion) ==
                  SB_ENGINE_STATUS_OK &&
              completion.affected_rows == 0 &&
              sb_engine_result_diagnostics(public_result, &diagnostics) ==
                  SB_ENGINE_STATUS_OK &&
              diagnostics.diagnostic_count == 0,
          "public import planning result claimed execution or diagnostics");
  bridge::StatementQueryExecuteResultHandleView query_handle;
  Require(bridge::ReadStatementQueryExecuteResultHandle(
              public_result, &query_handle) != SB_ENGINE_STATUS_OK,
          "planning-only import result published a query handle");

  sb_engine_string_view_t payload_view{};
  Require(sb_engine_result_payload(public_result, &payload_view) ==
              SB_ENGINE_STATUS_OK,
          "public import planning payload was unavailable");
  const std::string payload(payload_view.data, payload_view.size_bytes);
  const std::string expected_row =
      "surface_accepted_bool=1;planning_only_bool=1;"
      "execution_requires_execute_import_rows_bool=1;"
      "row_execution_completed_bool=0;row_persistence_claimed_bool=0;"
      "normalized_insert_mode=" +
      std::to_string(planned.normalized_insert_mode_code) +
      ";normalized_source_kind=" +
      std::to_string(planned.normalized_source_kind_code) +
      ";normalized_format_family=" +
      std::to_string(planned.normalized_format_family_code) +
      ";mapped_column_count_u64=" +
      std::to_string(planned.mapped_column_count) +
      ";validated_request_descriptor_uuid_16=" +
      planned.validated_request_descriptor_uuid.canonical +
      ";validated_request_descriptor_generation_u64=" +
      std::to_string(planned.validated_request_descriptor_generation) +
      ";validated_request_projection_sha256_32=sha256:" +
      scratchbird::core::hash::HexLower(
          planned.validated_request_projection_sha256);
  constexpr std::string_view kExpectedMetadata =
      "surface_accepted_bool:bool_u8:not_null;"
      "planning_only_bool:bool_u8:not_null;"
      "execution_requires_execute_import_rows_bool:bool_u8:not_null;"
      "row_execution_completed_bool:bool_u8:not_null;"
      "row_persistence_claimed_bool:bool_u8:not_null;"
      "normalized_insert_mode:enum_u16:not_null;"
      "normalized_source_kind:enum_u16:not_null;"
      "normalized_format_family:enum_u16:not_null;"
      "mapped_column_count_u64:u64:not_null;"
      "validated_request_descriptor_uuid_16:uuid16:not_null;"
      "validated_request_descriptor_generation_u64:u64:not_null;"
      "validated_request_projection_sha256_32:bstr32:not_null";
  Require(payload.find("operation_id=dml.plan_import_rows\n") == 0 &&
              payload.find("result_kind=import_plan_result\n") !=
                  std::string::npos &&
              payload.find("row_count=1\n") != std::string::npos &&
              payload.find("row[0]=" + expected_row + "\n") !=
                  std::string::npos &&
              payload.find("row_meta[0]=" +
                           std::string(kExpectedMetadata) + "\n") !=
                  std::string::npos &&
              std::count(expected_row.begin(), expected_row.end(), '=') == 12 &&
              payload.find("plan_import_rows_target") == std::string::npos &&
              payload.find(fixture.database_path.string()) ==
                  std::string::npos &&
              payload.find(UuidText(fixture.relation)) == std::string::npos,
          "public twelve-field import_plan_result drifted or leaked names");

  constexpr std::string_view kIpevPrefix =
      "evidence=accepted_executor_evidence_ipev_v1:";
  const auto evidence_begin = payload.find(kIpevPrefix);
  const auto hex_begin = evidence_begin == std::string::npos
                             ? std::string::npos
                             : evidence_begin + kIpevPrefix.size();
  const auto hex_end = hex_begin == std::string::npos
                           ? std::string::npos
                           : payload.find('\n', hex_begin);
  Require(hex_begin != std::string::npos && hex_end != std::string::npos &&
              hex_end - hex_begin ==
                  codec::kPlanImportRowsExecutorEvidenceBytesV1 * 2,
          "public result omitted the exact 416-hex-character IPEV");
  const auto public_evidence_hex =
      payload.substr(hex_begin, hex_end - hex_begin);
  Require(std::all_of(public_evidence_hex.begin(), public_evidence_hex.end(),
                      [](char ch) {
                        return (ch >= '0' && ch <= '9') ||
                               (ch >= 'a' && ch <= 'f');
                      }),
          "public IPEV was not canonical lowercase hexadecimal");
  Bytes public_evidence_bytes;
  public_evidence_bytes.reserve(public_evidence_hex.size() / 2);
  const auto hex_digit = [](char ch) -> std::uint8_t {
    return static_cast<std::uint8_t>(ch <= '9' ? ch - '0'
                                               : ch - 'a' + 10);
  };
  for (std::size_t offset = 0; offset != public_evidence_hex.size();
       offset += 2) {
    public_evidence_bytes.push_back(static_cast<std::uint8_t>(
        (hex_digit(public_evidence_hex[offset]) << 4U) |
        hex_digit(public_evidence_hex[offset + 1])));
  }
  codec::PlanImportRowsExecutorEvidenceV1 public_evidence;
  codec_diagnostic = {};
  Require(codec::DecodePlanImportRowsExecutorEvidenceV1(
              public_evidence_bytes.data(), public_evidence_bytes.size(),
              &public_evidence, &codec_diagnostic) &&
              public_evidence_bytes.size() == 208 &&
              public_evidence.request_descriptor_uuid ==
                  descriptor_ref.descriptor_uuid &&
              public_evidence.request_descriptor_generation ==
                  descriptor_ref.descriptor_generation &&
              public_evidence.request_projection_sha256 ==
                  planned.validated_request_projection_sha256 &&
              public_evidence.executor_availability_generation ==
                  available.snapshot.generation &&
              public_evidence.completed_validation_bits ==
                  codec::kPlanImportRowsAcceptedValidationBitsV1 &&
              public_evidence_hex == HexLower(public_evidence_bytes),
          "public IPEV did not bind the exact descriptor and all ten gates");
  (void)sb_engine_result_release(public_result);

  Require(ReadBytes(fixture.database_path) == database_before &&
              ReadBytes(availability_path) == availability_before,
          "validation-only binding or planning changed durable bytes");

  api::EngineRollbackTransactionRequest rollback;
  rollback.context = fixture.transaction;
  RequireOk(api::EngineRollbackTransaction(rollback),
            "fixture rollback failed");

  auto ended = RequestFrame(live, canonical);
  RequireRefusal(server::HandleCoordinateDmlPlanImportRowsBind(
                     &registry, engine_state, ended),
                 "MGA.TRANSACTION_INVALID",
                 "ended transaction did not use canonical transaction refusal");

  const auto final_release =
      api::ReleaseEngineBoundImportRowsPlanDescriptorsV1(binder_context);
  RequireOk(final_release, "bound descriptor release failed");
  Require(final_release.released_row_count == 1,
          "refusals published rows or successful binder row was not unique");
  Require(server::ReleaseServerStatementContext(&registry, view.statement_uuid),
          "statement receipt release failed");
}

}  // namespace

int main() {
  TestCoordination();
  return EXIT_SUCCESS;
}
