// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "../support/native_catalog_column_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../database_lifecycle/database_lifecycle_test_memory.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include "catalog/name_registry.hpp"
#include "database_lifecycle.hpp"
#include "engine/internal_api/mga_relation_store/mga_relation_store.hpp"
#include "engine/sblr/sblr_opcode_registry.hpp"
#include "server/engine_host.hpp"
#include "server/session_registry.hpp"
#include "server/sbps.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include "transaction/transaction_api.hpp"
#include "wire/parser_server_ipc/sbps_bulk_import_stream_codec.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace api = scratchbird::engine::internal_api;
namespace bridge = scratchbird::server_engine_bridge;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace server = scratchbird::server;
namespace sbps = scratchbird::server::sbps;
namespace uuid = scratchbird::core::uuid;
namespace wire = scratchbird::wire::sbps_bulk_import;

constexpr std::uint64_t kEpochMillis = 1948000000000ull;

[[noreturn]] void Fail(std::string_view detail) {
  std::cerr << "bulk_import_stream_bind_authority: " << detail << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool value, std::string_view detail) {
  if (!value) Fail(detail);
}

platform::TypedUuid NewUuid(platform::UuidKind kind) {
  static std::atomic<std::uint64_t> ordinal{1};
  const auto timestamp = kEpochMillis + ordinal.fetch_add(1);
  const auto generated = [&]() {
    if (kind != platform::UuidKind::session) {
      return uuid::GenerateEngineIdentityV7(kind, timestamp);
    }
    const auto raw = uuid::GenerateCompatibilityUnixTimeV7(timestamp);
    if (!raw.ok()) {
      uuid::TypedUuidResult failed;
      failed.status = raw.status;
      failed.diagnostic = raw.diagnostic;
      return failed;
    }
    return uuid::MakeTypedUuid(kind, raw.value);
  }();
  Require(generated.ok(), "engine UUID generation failed");
  return generated.value;
}

platform::Uuid Identity(const platform::TypedUuid& value) {
  return value.value;
}

std::array<std::uint8_t, 16> Bytes(const platform::Uuid& value) {
  return value.bytes;
}

sb_engine_uuid_t PublicUuid(const platform::TypedUuid& value) {
  sb_engine_uuid_t result{};
  std::copy(value.value.bytes.begin(), value.value.bytes.end(), result.bytes);
  return result;
}

struct Fixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  platform::TypedUuid database = NewUuid(platform::UuidKind::database);
  platform::TypedUuid filespace = NewUuid(platform::UuidKind::filespace);
  platform::TypedUuid principal = NewUuid(platform::UuidKind::principal);
  platform::TypedUuid session = NewUuid(platform::UuidKind::session);
  platform::TypedUuid schema = NewUuid(platform::UuidKind::schema);
  platform::TypedUuid relation = NewUuid(platform::UuidKind::object);
  api::EngineRequestContext transaction;
  api::MgaRelationStorageDescriptor descriptor;

  ~Fixture() {
    std::error_code ignored;
    if (!directory.empty()) std::filesystem::remove_all(directory, ignored);
  }
};

api::EngineRequestContext BaseContext(const Fixture& fixture) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "bulk-import-bind-authority";
  context.database_path = fixture.database_path.string();
  context.database_uuid = Identity(fixture.database);
  context.database_page_size_bytes = 16384;
  context.default_root_uuid = Identity(fixture.filespace);
  context.current_schema_uuid = Identity(fixture.schema);
  context.principal_uuid = Identity(fixture.principal);
  context.session_uuid = Identity(fixture.session);
  context.identifier_profile_uuid = "sbsql_v3";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(context);
  return context;
}

std::unique_ptr<Fixture> MakeFixture() {
  auto fixture = std::make_unique<Fixture>();
  fixture->directory = std::filesystem::temp_directory_path() /
      ("scratchbird_bulk_import_bind_" +
       std::to_string(std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count()));
  std::filesystem::create_directories(fixture->directory);
  fixture->database_path = fixture->directory / "authority.sbdb";
  db::DatabaseCreateConfig create;
  create.path = fixture->database_path.string();
  create.database_uuid = fixture->database;
  create.filespace_uuid = fixture->filespace;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = kEpochMillis;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) std::cerr << created.diagnostic.diagnostic_code << ':' << created.diagnostic.message_key << '\n';
  Require(created.ok(), "database creation failed");
  const auto bootstrap = db::ReadDatabaseBootstrapSecurityCatalog(create.path);
  Require(bootstrap.ok() && bootstrap.state.present && bootstrap.state.committed_by_inventory,
          "durable bootstrap principal missing");
  fixture->principal = bootstrap.state.principal_uuid;

  api::EngineBeginTransactionRequest begin;
  begin.context = BaseContext(*fixture);
  begin.isolation_level = "repeatable_read";
  const auto begun = api::EngineBeginTransaction(begin);
  Require(begun.ok, "transaction begin failed");
  fixture->transaction = begin.context;
  fixture->transaction.local_transaction_id = begun.local_transaction_id;
  fixture->transaction.transaction_uuid = begun.transaction_uuid;
  fixture->transaction.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  fixture->transaction.transaction_isolation_level = begun.isolation_level;

  api::CrudTableRecord table;
  table.table_uuid = Identity(fixture->relation);
  table.default_name = "bulk_import_authority_target";
  table.columns = {{"id", scratchbird::tests::NativeCatalogColumnFixture({
      {{"type", "int32"}, {"nullable", "false"}},
      {{"datatype_descriptor_uuid", scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d716")},
       {"type_uuid", scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d717")}}})}};
  fixture->transaction.current_schema_uuid = {};
  Require(!scratchbird::tests::PublishMgaTableFixture(
               fixture->transaction, table, {"int32"}).error,
          "published bound table cohort failed");
  fixture->schema = {platform::UuidKind::schema, fixture->transaction.current_schema_uuid};
  const auto loaded = api::LoadMgaRelationStorageDescriptor(fixture->transaction, table.table_uuid);
  Require(loaded.ok, "published relation descriptor unavailable");
  fixture->descriptor = loaded.descriptor;
  const auto epochs = api::LoadCatalogObjectLifecycleEpochState(fixture->transaction);
  Require(epochs.ok && epochs.state.metadata_epoch && epochs.state.name_resolution_epoch,
          "published catalog epochs unavailable");
  fixture->transaction.catalog_generation_id = epochs.state.metadata_epoch;
  fixture->transaction.name_resolution_epoch = epochs.state.name_resolution_epoch;
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(fixture->transaction);
  return fixture;
}

class Session {
 public:
  explicit Session(const Fixture& fixture)
      : path_(fixture.database_path.string()) {
    sb_engine_open_params_v1_t open{};
    open.struct_size = sizeof(open);
    open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    open.database_path_utf8 = path_.data();
    open.database_path_size = path_.size();
    open.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
    Require(sb_engine_open(&open, &engine_, nullptr) == SB_ENGINE_STATUS_OK,
            "engine open failed");
    sb_engine_session_params_v1_t begin{};
    begin.struct_size = sizeof(begin);
    begin.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    begin.effective_user_uuid = PublicUuid(fixture.principal);
    begin.session_uuid = PublicUuid(fixture.session);
    begin.default_language_utf8 = "en";
    begin.default_language_size = 2;
    begin.trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
    Require(sb_engine_session_begin(engine_, &begin, &session_, nullptr) ==
                SB_ENGINE_STATUS_OK,
            "engine session begin failed");
  }
  ~Session() {
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
  std::string path_;
  sb_engine_handle_t engine_ = nullptr;
  sb_engine_session_t session_ = nullptr;
};

bridge::StatementContextReceiptHandle Acquire(
    const Session& session, const api::EngineRequestContext& context,
    bridge::StatementContextReceiptView* view) {
  bridge::StatementContextAcquireRequest request;
  request.engine_context = &context;
  request.exact_transaction_uuid = context.transaction_uuid;
  bridge::StatementContextReceiptHandle receipt;
  sb_engine_result_t result = nullptr;
  const auto status = bridge::AcquireStatementContextReceipt(
      session.get(), &request, &receipt, view, &result);
  if (result != nullptr) (void)sb_engine_result_release(result);
  Require(status == SB_ENGINE_STATUS_OK && receipt,
          "statement receipt acquisition failed");
  return receipt;
}

bridge::StatementBulkImportBindRequestV1 Demand(
    const bridge::StatementContextReceiptView& view, std::string_view name,
    std::uint64_t structural = 11, std::uint32_t occurrence = 3,
    std::string_view command_surface_id = "SBSQL-465931ED7427") {
  wire::Bind wire_request;
  wire_request.authenticated_receipt_uuid = Bytes(view.receipt_uuid);
  wire_request.command_surface_id = command_surface_id;
  wire_request.structural_occurrence = structural;
  wire_request.import_occurrence = occurrence;
  wire_request.target_name_atom_vector.push_back(1);
  wire_request.target_name_atom_vector.push_back(
      static_cast<std::uint8_t>(name.size()));
  wire_request.target_name_atom_vector.push_back(0);
  wire_request.target_name_atom_vector.insert(
      wire_request.target_name_atom_vector.end(), name.begin(), name.end());
  wire_request.target_name_atom_vector.push_back(0);
  wire_request.input_format_demand = 1;
  wire_request.character_encoding_demand = 1;
  wire_request.conversion_policy_demand = 1;
  wire_request.null_default_policy_demand = 1;
  wire_request.reject_policy_demand = 1;
  wire_request.syntax_demand_sha256 = wire::BindDemandEvidence(wire_request);
  std::vector<std::uint8_t> encoded;
  Require(wire::EncodeBind(wire_request, &encoded),
          "canonical bind request encoding failed");

  bridge::StatementBulkImportBindRequestV1 request;
  request.authenticated_receipt_uuid = view.receipt_uuid;
  request.command_surface_id = wire_request.command_surface_id;
  request.structural_occurrence = structural;
  request.import_occurrence = occurrence;
  request.target_name_atoms.push_back({std::string(name), false});
  request.input_format_demand = 1;
  request.character_encoding_demand = 1;
  request.conversion_policy_demand = 1;
  request.null_default_policy_demand = 1;
  request.reject_policy_demand = 1;
  request.syntax_demand_sha256 = wire_request.syntax_demand_sha256;
  request.exact_bind_request_bytes = std::move(encoded);
  return request;
}

std::string Diagnostic(sb_engine_result_t result) {
  if (result == nullptr) return {};
  sb_engine_diagnostic_set_view_t diagnostics{};
  if (sb_engine_result_diagnostics(result, &diagnostics) !=
          SB_ENGINE_STATUS_OK ||
      diagnostics.diagnostic_count != 1) {
    return {};
  }
  return {diagnostics.diagnostics[0].symbolic_code.data,
          diagnostics.diagnostics[0].symbolic_code.size_bytes};
}

void ReleaseResult(sb_engine_result_t result) {
  if (result != nullptr) (void)sb_engine_result_release(result);
}
}  // namespace

int main() {
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture("bulk_import_bind_authority");
  const auto* bulk_opcode =
      scratchbird::engine::sblr::LookupSblrOpcodeCode(775);
  Require(bulk_opcode != nullptr &&
              bulk_opcode == scratchbird::engine::sblr::LookupSblrOperation(
                                 "engine.op.bulk_import_stream") &&
              bulk_opcode == scratchbird::engine::sblr::LookupSblrOpcode(
                                 "SBLR_BULK_IMPORT_STREAM") &&
              scratchbird::engine::sblr::LookupSblrOperation("bulk.import") ==
                  nullptr,
          "central opcode-775 identity is ambiguous or retains an alias");
  auto fixture = MakeFixture();
  Session session(*fixture);
  bridge::StatementContextReceiptView view;
  const auto receipt = Acquire(session, fixture->transaction, &view);
  const auto request = Demand(view, "bulk_import_authority_target");

  server::ServerSessionRegistry server_registry;
  server::ServerSessionRecord server_session;
  server_session.session_uuid = Bytes(Identity(fixture->session));
  server_registry.sessions_by_uuid.emplace(fixture->session.value,
                                            std::move(server_session));
  const auto cross_session_uuid = NewUuid(platform::UuidKind::session);
  server::ServerSessionRecord cross_session;
  cross_session.session_uuid = Bytes(Identity(cross_session_uuid));
  server_registry.sessions_by_uuid.emplace(cross_session_uuid.value,
                                            std::move(cross_session));
  server::ServerStatementContextRecord statement_record;
  statement_record.session_uuid = Bytes(Identity(fixture->session));
  statement_record.statement_uuid = view.statement_uuid;
  statement_record.owning_local_transaction_id =
      view.owning_local_transaction_id;
  statement_record.owning_transaction_uuid = view.owning_transaction_uuid;
  statement_record.receipt = receipt;
  statement_record.view = view;
  server_registry.statement_contexts_by_statement_uuid.emplace(
      view.statement_uuid, std::move(statement_record));
  sbps::Frame bind_frame;
  bind_frame.header.message_type = static_cast<std::uint16_t>(
      sbps::MessageType::kBulkImportStreamBind);
  bind_frame.header.payload_schema_id = sbps::kSchemaBulkImportStreamBindV1;
  bind_frame.header.session_uuid = Bytes(Identity(fixture->session));
  bind_frame.payload = request.exact_bind_request_bytes;
  const auto bind_response = server::HandleBindBulkImportStream(
      &server_registry, server::HostedEngineState{}, bind_frame);
  if (!bind_response.accepted) for (const auto& diagnostic : bind_response.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << '\n';
    for (const auto& field : diagnostic.fields) std::cerr << field.key << '=' << field.value << '\n';
  }
  Require(bind_response.accepted && bind_response.diagnostics.empty() &&
              bind_response.response_message_type == static_cast<std::uint16_t>(
                  sbps::MessageType::kBulkImportStreamBindAck) &&
              bind_response.response_schema_id ==
                  sbps::kSchemaBulkImportStreamBindAckV1,
          "authenticated SBPS bind route failed");

  auto malformed_frame = bind_frame;
  malformed_frame.payload.back() ^= 1;
  const auto malformed_response = server::HandleBindBulkImportStream(
      &server_registry, server::HostedEngineState{}, malformed_frame);
  Require(!malformed_response.accepted && malformed_response.payload.empty() &&
              malformed_response.diagnostics.size() == 1 &&
              malformed_response.diagnostics[0].code ==
                  "SBLR.OPERAND_INVALID",
          "malformed SBPS bind did not fail before receipt authority");

  auto cross_session_frame = bind_frame;
  cross_session_frame.header.session_uuid = Bytes(Identity(cross_session_uuid));
  const auto cross_session_response = server::HandleBindBulkImportStream(
      &server_registry, server::HostedEngineState{}, cross_session_frame);
  Require(!cross_session_response.accepted &&
              cross_session_response.payload.empty() &&
              cross_session_response.diagnostics.size() == 1 &&
              cross_session_response.diagnostics[0].code ==
                  "SECURITY.ACCESS_DENIED",
          "cross-session bind disclosed receipt authority");

  auto& registered_receipt =
      server_registry.statement_contexts_by_statement_uuid.at(
          view.statement_uuid);
  registered_receipt.released = true;
  const auto released_response = server::HandleBindBulkImportStream(
      &server_registry, server::HostedEngineState{}, bind_frame);
  registered_receipt.released = false;
  Require(!released_response.accepted && released_response.payload.empty() &&
              released_response.diagnostics.size() == 1 &&
              released_response.diagnostics[0].code ==
                  "MGA.TRANSACTION_INVALID",
          "released receipt did not preserve transaction-invalid precedence");

  bridge::StatementBulkImportBindAckV1 acknowledgement;
  sb_engine_result_t result = nullptr;
  Require(bridge::BindStatementBulkImportAuthorityV1(
              receipt, &request, &acknowledgement, &result) ==
              SB_ENGINE_STATUS_OK &&
              result == nullptr &&
              acknowledgement.exact_bind_ack_bytes == bind_response.payload,
          "exact bind replay after SBPS route was not byte-identical");
  wire::BindAck decoded_ack;
  Require(wire::DecodeBindAck(acknowledgement.exact_bind_ack_bytes.data(),
                              acknowledgement.exact_bind_ack_bytes.size(),
                              &decoded_ack) &&
              decoded_ack.binding_uuid == Bytes(acknowledgement.binding_uuid) &&
              decoded_ack.binding_evidence_sha256 ==
                  acknowledgement.binding_evidence_sha256,
          "bind ACK was not canonical");

  bridge::StatementBulkImportAuthorityV1 authority;
  Require(bridge::CopyStatementBulkImportAuthorityV1(
              receipt, 11, 3, &authority, &result) == SB_ENGINE_STATUS_OK &&
              authority.target_relation_uuid == Identity(fixture->relation) &&
              authority.target_relation_generation ==
                  fixture->descriptor.relation_generation &&
              authority.row_shape_uuid ==
                  fixture->descriptor.descriptor_uuid &&
              authority.row_shape_generation ==
                  fixture->descriptor.descriptor_generation &&
              authority.resource_grant_uuid == view.resource_admission_uuid &&
              authority.resource_grant_generation == view.resource_epoch &&
              authority.import_route_snapshot_uuid !=
                  view.optimizer_route_snapshot_uuid &&
              authority.authorization_observation.present &&
              authority.exact_bind_request_bytes ==
                  request.exact_bind_request_bytes &&
              authority.acknowledgement.exact_bind_ack_bytes ==
                  acknowledgement.exact_bind_ack_bytes &&
              view.relation_occurrence_mappings.empty(),
          "private authority derivation or no-leak invariant failed");

  bridge::StatementBulkImportBindAckV1 replay;
  Require(bridge::BindStatementBulkImportAuthorityV1(
              receipt, &request, &replay, &result) == SB_ENGINE_STATUS_OK &&
              replay.exact_bind_ack_bytes == acknowledgement.exact_bind_ack_bytes,
          "exact bind replay was not byte-identical");

  bridge::StatementContextReceiptView subform_view;
  const auto subform_receipt =
      Acquire(session, fixture->transaction, &subform_view);
  const auto subform_request = Demand(
      subform_view, "bulk_import_authority_target", 12, 1,
      "SBSQL-4F912014EA85");
  Require(bridge::BindStatementBulkImportAuthorityV1(
              subform_receipt, &subform_request, &replay, &result) ==
              SB_ENGINE_STATUS_OK,
          "admitted copy_statement row failed");

  const auto conflict = Demand(view, "bulk_import_authority_target", 11, 4);
  result = nullptr;
  Require(bridge::BindStatementBulkImportAuthorityV1(
              receipt, &conflict, &replay, &result) ==
              SB_ENGINE_STATUS_CONFLICT &&
              Diagnostic(result) == "BULK.IMPORT.RECOVERY_CONFLICT",
          "occurrence conflict was not canonical");
  ReleaseResult(result);

  auto unauthorized_context = fixture->transaction;
  unauthorized_context.authorization_context.grants.clear();
  // The owner fixture has both explicit grants and the engine-issued SysArch
  // bundle. Remove both authority carriers for this negative binding snapshot.
  unauthorized_context.authorization_context.engine_owned_bootstrap_role_uuid = {};
  bridge::StatementContextReceiptView unauthorized_view;
  const auto unauthorized_receipt =
      Acquire(session, unauthorized_context, &unauthorized_view);
  const auto unauthorized_request =
      Demand(unauthorized_view, "bulk_import_authority_target", 21, 1);
  result = nullptr;
  Require(bridge::BindStatementBulkImportAuthorityV1(
              unauthorized_receipt, &unauthorized_request, &replay, &result) ==
              SB_ENGINE_STATUS_SECURITY_DENIED &&
              Diagnostic(result) == "SECURITY.ACCESS_DENIED",
          "unauthorized target was not refused");
  ReleaseResult(result);
  Require(bridge::CopyStatementBulkImportAuthorityV1(
              unauthorized_receipt, 21, 1, &authority, &result) ==
              SB_ENGINE_STATUS_SECURITY_DENIED,
          "unauthorized bind leaked private authority");
  ReleaseResult(result);

  auto cancelled_context = fixture->transaction;
  cancelled_context.query_cancellation_requested = [] { return true; };
  bridge::StatementContextReceiptView cancelled_view;
  const auto cancelled_receipt =
      Acquire(session, cancelled_context, &cancelled_view);
  const auto cancelled_request =
      Demand(cancelled_view, "bulk_import_authority_target", 22, 1);
  result = nullptr;
  Require(bridge::BindStatementBulkImportAuthorityV1(
              cancelled_receipt, &cancelled_request, &replay, &result) ==
              SB_ENGINE_STATUS_TIMEOUT &&
              Diagnostic(result) == "PROCESS.CANCELLED",
          "bind cancellation did not stop before authority publication");
  ReleaseResult(result);
  result = nullptr;
  Require(bridge::CopyStatementBulkImportAuthorityV1(
              cancelled_receipt, 22, 1, &authority, &result) ==
              SB_ENGINE_STATUS_SECURITY_DENIED,
          "cancelled bind published private authority");
  ReleaseResult(result);

  bridge::StatementContextReceiptView unresolved_view;
  const auto unresolved_receipt =
      Acquire(session, fixture->transaction, &unresolved_view);
  const auto unresolved_request =
      Demand(unresolved_view, "missing_bulk_import_target", 31, 1);
  result = nullptr;
  Require(bridge::BindStatementBulkImportAuthorityV1(
              unresolved_receipt, &unresolved_request, &replay, &result) ==
              SB_ENGINE_STATUS_CONFLICT &&
              Diagnostic(result) == "CATALOG.NAME.NOT_FOUND",
          "unresolved target was not refused");
  ReleaseResult(result);

  auto cluster_context = fixture->transaction;
  cluster_context.cluster_authority_available = true;
  bridge::StatementContextReceiptView cluster_view;
  const auto cluster_receipt = Acquire(session, cluster_context, &cluster_view);
  const auto cluster_request =
      Demand(cluster_view, "bulk_import_authority_target", 41, 1);
  result = nullptr;
  Require(bridge::BindStatementBulkImportAuthorityV1(
              cluster_receipt, &cluster_request, &replay, &result) ==
              SB_ENGINE_STATUS_UNSUPPORTED &&
              Diagnostic(result) == "CLUSTER.WRITE_AUTHORITY_REQUIRED",
          "cluster context fell back to a local route");
  ReleaseResult(result);

  bridge::StatementContextReceiptView ended_view;
  const auto ended_receipt = Acquire(session, fixture->transaction, &ended_view);
  const auto ended_request =
      Demand(ended_view, "bulk_import_authority_target", 51, 1);
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = fixture->transaction;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "fixture transaction rollback failed");
  result = nullptr;
  Require(bridge::BindStatementBulkImportAuthorityV1(
              ended_receipt, &ended_request, &replay, &result) ==
              SB_ENGINE_STATUS_CONFLICT &&
              Diagnostic(result) == "MGA.TRANSACTION_INVALID",
          "ended transaction did not fail before authority publication");
  ReleaseResult(result);
  result = nullptr;
  Require(bridge::CopyStatementBulkImportAuthorityV1(
              ended_receipt, 51, 1, &authority, &result) ==
              SB_ENGINE_STATUS_SECURITY_DENIED,
          "ended transaction bind published private authority");
  ReleaseResult(result);

  Require(bridge::ReleaseStatementContextReceipt(cluster_receipt) ==
                  SB_ENGINE_STATUS_OK &&
              bridge::ReleaseStatementContextReceipt(unresolved_receipt) ==
                  SB_ENGINE_STATUS_OK &&
              bridge::ReleaseStatementContextReceipt(cancelled_receipt) ==
                  SB_ENGINE_STATUS_OK &&
              bridge::ReleaseStatementContextReceipt(unauthorized_receipt) ==
                  SB_ENGINE_STATUS_OK &&
              bridge::ReleaseStatementContextReceipt(subform_receipt) ==
                  SB_ENGINE_STATUS_OK &&
              bridge::ReleaseStatementContextReceipt(ended_receipt) ==
                  SB_ENGINE_STATUS_OK &&
              bridge::ReleaseStatementContextReceipt(receipt) ==
                  SB_ENGINE_STATUS_OK,
          "receipt cleanup failed");
  return EXIT_SUCCESS;
}
