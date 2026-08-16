// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "engine/internal_api/api_types.hpp"
#include "engine/internal_api/transaction/transaction_api.hpp"
#include "engine/sblr/sblr_opcode_stream.hpp"
#include "local_transaction_store.hpp"
#include "scratchbird/engine/sblr_envelope.hpp"
#include "server/sblr_admission.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace api = scratchbird::engine::internal_api;
namespace bridge = scratchbird::server_engine_bridge;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace server = scratchbird::server;
namespace uuid = scratchbird::core::uuid;
namespace wire = scratchbird::engine;

using Bytes = std::vector<std::uint8_t>;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "CSC-TEST-002320|CSC-TEST-002324: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

platform::TypedUuid NewUuid(platform::UuidKind kind, std::uint64_t salt) {
  const auto timestamp = 1950000000000ull + salt % 1'000'000ull;
  if (uuid::UuidKindAllowsDurableIdentity(kind)) {
    const auto value = uuid::GenerateEngineIdentityV7(kind, timestamp);
    Require(value.ok(), "durable fixture UUID generation failed");
    return value.value;
  }
  const auto raw = uuid::GenerateCompatibilityUnixTimeV7(timestamp);
  Require(raw.ok(), "fixture UUID generation failed");
  const auto typed = uuid::MakeTypedUuid(kind, raw.value);
  Require(typed.ok(), "fixture UUID typing failed");
  return typed.value;
}

std::string Text(const platform::TypedUuid& value) {
  return uuid::UuidToString(value.value);
}

sb_engine_uuid_t PublicUuid(const platform::TypedUuid& value) {
  sb_engine_uuid_t result{};
  std::memcpy(result.bytes, value.value.bytes.data(), value.value.bytes.size());
  return result;
}

std::array<std::uint8_t, 16> RawUuid(std::string_view text) {
  std::array<std::uint8_t, 16> out{};
  std::size_t byte = 0;
  int high = -1;
  for (const char ch : text) {
    if (ch == '-') continue;
    const int digit = ch >= '0' && ch <= '9' ? ch - '0' : ch - 'a' + 10;
    if (high < 0) high = digit;
    else {
      out[byte++] = static_cast<std::uint8_t>((high << 4) | digit);
      high = -1;
    }
  }
  Require(byte == out.size(), "canonical UUID decoding failed");
  return out;
}

void U16(Bytes* out, std::uint16_t value) {
  out->push_back(value); out->push_back(value >> 8);
}
void U32(Bytes* out, std::uint32_t value) {
  for (unsigned n = 0; n != 4; ++n) out->push_back(value >> (n * 8));
}
void U64(Bytes* out, std::uint64_t value) {
  for (unsigned n = 0; n != 8; ++n) out->push_back(value >> (n * 8));
}
Bytes V16(std::uint16_t value) { Bytes out; U16(&out, value); return out; }
Bytes V32(std::uint32_t value) { Bytes out; U32(&out, value); return out; }
Bytes V64(std::uint64_t value) { Bytes out; U64(&out, value); return out; }
Bytes OptionalUuid(const std::array<std::uint8_t, 16>& value) {
  Bytes out{1}; out.insert(out.end(), value.begin(), value.end()); return out;
}
Bytes CanonicalStruct(std::uint32_t format, std::uint8_t version) {
  Bytes out; U32(&out, format); U16(&out, 1); U16(&out, 0); U64(&out, 1);
  out.push_back(version); return out;
}

struct Fixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  platform::TypedUuid database_uuid;
  platform::TypedUuid filespace_uuid;
  platform::TypedUuid principal_uuid;
  platform::TypedUuid session_uuid;
  std::uint64_t resource_epoch = 1;
  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
};

Fixture CreateFixture() {
  Fixture fixture;
  const auto salt = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  fixture.directory = std::filesystem::temp_directory_path() /
      ("sbsql_sblr_ia01_cancel_" + std::to_string(salt));
  std::filesystem::create_directories(fixture.directory);
  fixture.database_path = fixture.directory / "fault.sbdb";
  fixture.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  fixture.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  fixture.principal_uuid = NewUuid(platform::UuidKind::principal, salt + 3);
  fixture.session_uuid = NewUuid(platform::UuidKind::session, salt + 4);
  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = fixture.database_uuid;
  create.filespace_uuid = fixture.filespace_uuid;
  create.creation_unix_epoch_millis = 1950000000000ull + salt % 1'000'000ull;
  create.page_size = 16384;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "fixture database creation failed");
  fixture.resource_epoch = created.state.resource_seed_catalog.resource_epoch == 0
      ? 1 : created.state.resource_seed_catalog.resource_epoch;
  const auto inventory = db::PersistLocalTransactionInventoryToDatabase(
      fixture.database_path.string(),
      scratchbird::transaction::mga::MakeEmptyLocalTransactionInventory());
  Require(inventory.ok(), "fixture MGA inventory initialization failed");
  return fixture;
}

struct PublicSession {
  sb_engine_handle_t engine = nullptr;
  sb_engine_session_t session = nullptr;
  std::string path;
  explicit PublicSession(const Fixture& fixture) : path(fixture.database_path.string()) {
    sb_engine_open_params_v1_t open{};
    open.struct_size = sizeof(open); open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    open.database_path_utf8 = path.data(); open.database_path_size = path.size();
    open.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
    Require(sb_engine_open(&open, &engine, nullptr) == SB_ENGINE_STATUS_OK,
            "engine open failed");
    sb_engine_session_params_v1_t begin{};
    begin.struct_size = sizeof(begin); begin.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    begin.effective_user_uuid = PublicUuid(fixture.principal_uuid);
    begin.session_uuid = PublicUuid(fixture.session_uuid);
    begin.default_language_utf8 = "en"; begin.default_language_size = 2;
    begin.trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
    Require(sb_engine_session_begin(engine, &begin, &session, nullptr) ==
                SB_ENGINE_STATUS_OK,
            "engine session begin failed");
  }
  sb_engine_status_t End() {
    if (session == nullptr) return SB_ENGINE_STATUS_ALREADY_RELEASED;
    sb_engine_session_end_params_v1_t end{};
    end.struct_size = sizeof(end); end.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    end.rollback_active_transactions = 1; end.cancel_open_results = 1;
    const auto status = sb_engine_session_end(session, &end, nullptr);
    if (status == SB_ENGINE_STATUS_OK) session = nullptr;
    return status;
  }
  ~PublicSession() {
    if (session) (void)End();
    if (engine) (void)sb_engine_close(engine, nullptr);
  }
};

api::EngineRequestContext BeginTransaction(const Fixture& fixture,
                                            std::atomic<unsigned>* probes) {
  api::EngineBeginTransactionRequest begin;
  begin.context.trust_mode = api::EngineTrustMode::server_isolated;
  begin.context.request_id = "ia01-package-cancellation";
  begin.context.database_path = fixture.database_path.string();
  begin.context.database_uuid.canonical = Text(fixture.database_uuid);
  begin.context.principal_uuid.canonical = Text(fixture.principal_uuid);
  begin.context.session_uuid.canonical = Text(fixture.session_uuid);
  begin.context.security_context_present = true;
  begin.context.catalog_generation_id = 1;
  begin.context.security_epoch = 1;
  begin.context.resource_epoch = fixture.resource_epoch;
  begin.context.name_resolution_epoch = 1;
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  Require(begun.ok, "MGA transaction begin failed");
  auto context = begin.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical = Text(fixture.database_uuid);
  context.authorization_context.principal_uuid = context.principal_uuid;
  context.authorization_context.security_epoch = 1;
  context.authorization_context.policy_epoch = 1;
  context.authorization_context.catalog_generation_id = 1;
  api::EngineAuthorizationSubject subject;
  subject.subject_uuid = context.principal_uuid; subject.subject_kind = "principal";
  context.authorization_context.effective_subjects.push_back(subject);
  context.optimizer_route_epoch = 1; context.optimizer_route_generation = 1;
  context.optimizer_memory_budget_bytes = 64 * 1024 * 1024;
  context.optimizer_maximum_candidate_count = 131072;
  context.optimizer_maximum_memo_groups = 131072;
  context.optimizer_maximum_search_steps = 1048576;
  context.optimizer_maximum_planning_time_ns = 5'000'000'000ull;
  context.optimizer_spill_allowed = true;
  context.query_cancellation_requested = [probes] {
    probes->fetch_add(1, std::memory_order_relaxed);
    return true;
  };
  return context;
}

sblr::SblrOperationEnvelope Frame(bool begin, std::string_view parser,
                                   std::string_view registry,
                                   const std::array<std::uint8_t, 16>& package) {
  auto operation = sblr::MakeSblrEnvelope(
      begin ? "engine.op.package_begin" : "engine.op.package_end",
      begin ? "SBLR_PACKAGE_BEGIN" : "SBLR_PACKAGE_END",
      begin ? "ia01.cancel.begin" : "ia01.cancel.end");
  operation.opcode_code = begin ? 1 : 2;
  operation.result_shape = "void"; operation.diagnostic_shape = "diagnostic_vector";
  operation.parser_package_uuid = parser; operation.registry_snapshot_uuid = registry;
  operation.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1; operand.type = begin ? "package.header" : "package.footer";
  operand.name = "package_descriptor";
  operand.value_kind = sblr::SblrValueKind::descriptor_ref;
  operand.value_body.assign(package.begin(), package.end());
  operation.operands.push_back(std::move(operand));
  return operation;
}

struct Submission { std::string container; std::string ingress; Bytes stream; };
Submission BuildSubmission(const Fixture& fixture,
                           const bridge::StatementContextReceiptView& view,
                           std::string_view parser_uuid) {
  const auto package = RawUuid(view.bound_ast_uuid);
  auto member = sblr::MakeSblrEnvelope("query.execute", "SBLR_QUERY_EXECUTE",
                                      "ia01.cancel.contained_query");
  member.opcode_code = 0x1207;
  member.result_shape = "query_execute_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOpcodeStream package_stream;
  package_stream.package_descriptor_uuid = view.bound_ast_uuid;
  package_stream.registry_snapshot_uuid = view.catalog_epoch_uuid;
  package_stream.operations = {
      Frame(true, parser_uuid, view.catalog_epoch_uuid, package),
      std::move(member),
      Frame(false, parser_uuid, view.catalog_epoch_uuid, package)};
  const auto stream = sblr::EncodeSblrOpcodeStream(package_stream);
  Require(!stream.empty(), "canonical cancellation SBOS encoding failed");

  const auto database = RawUuid(Text(fixture.database_uuid));
  const auto dialect = RawUuid(Text(NewUuid(platform::UuidKind::object, 501)));
  const auto parser = RawUuid(parser_uuid);
  const auto registry = RawUuid(view.catalog_epoch_uuid);
  const auto statement = RawUuid(view.statement_uuid);
  const auto principal = RawUuid(Text(fixture.principal_uuid));
  wire::SblrCanonicalContainer container;
  std::copy(database.begin(), database.end(), container.canonical_anchor.begin());
  std::copy(dialect.begin(), dialect.end(), container.canonical_anchor.begin() + 16);
  std::copy(parser.begin(), parser.end(), container.canonical_anchor.begin() + 32);
  auto put32 = [&](std::size_t offset, std::uint32_t value) {
    for (unsigned n = 0; n != 4; ++n) container.canonical_anchor[offset + n] = value >> (n * 8);
  };
  auto put64 = [&](std::size_t offset, std::uint64_t value) {
    for (unsigned n = 0; n != 8; ++n) container.canonical_anchor[offset + n] = value >> (n * 8);
  };
  put32(48, 1); put64(52, 1); put64(60, 1); put64(68, 1);
  std::copy(registry.begin(), registry.end(), container.canonical_anchor.begin() + 76);
  put64(92, 1); container.canonical_anchor[100] = 1;
  std::copy(statement.begin(), statement.end(), container.canonical_anchor.begin() + 116);
  container.operation_payload = stream;
  const auto outer = wire::EncodeSblrContainer(container);

  wire::SblrExecutionEnvelopeV1 ingress;
  auto& fields = ingress.fields;
  fields[0] = Bytes(statement.begin(), statement.end()); fields[1] = V16(1);
  fields[2] = V16(0); fields[3] = V32(0x10001); fields[4] = V16(1);
  fields[5] = {1}; U64(&fields[5], stream.size());
  fields[5].insert(fields[5].end(), stream.begin(), stream.end()); fields[6] = {0};
  fields[7] = {1}; U32(&fields[7], wire::SblrCrc32c(stream.data(), stream.size()));
  fields[8] = V64(stream.size()); fields[9] = V16(1);
  fields[10] = OptionalUuid(dialect); fields[11] = OptionalUuid(principal);
  fields[12] = CanonicalStruct(0x1001, 1); fields[13] = CanonicalStruct(0x1002, 2);
  fields[14] = {0}; fields[15] = V64(1); fields[16] = V32(0); fields[17] = V32(0);
  fields[18] = V32(0); fields[19] = {0}; fields[20] = V32(0);
  fields[21] = CanonicalStruct(0x1005, 5); fields[22] = {0}; fields[23] = {0};
  fields[24] = {0}; fields[25] = V16(0); fields[26] = {0}; fields[27] = {0};
  const auto execution = wire::EncodeSblrExecutionEnvelopeV1(ingress);
  return {{outer.begin(), outer.end()}, {execution.begin(), execution.end()},
          stream};
}
}  // namespace

#ifndef SCRATCHBIRD_IA01_PACKAGE_FIXTURE_ONLY
int main() {
  auto fixture = CreateFixture();
  PublicSession session(fixture);
  std::atomic<unsigned> cancellation_probes{0};
  auto context = BeginTransaction(fixture, &cancellation_probes);
  bridge::StatementContextAcquireRequest acquire;
  acquire.engine_context = &context;
  acquire.exact_transaction_uuid = context.transaction_uuid.canonical;
  bridge::StatementContextReceiptHandle receipt;
  bridge::StatementContextReceiptView view;
  sb_engine_result_t acquire_result = nullptr;
  Require(bridge::AcquireStatementContextReceipt(session.session, &acquire,
                                                  &receipt, &view,
                                                  &acquire_result) ==
              SB_ENGINE_STATUS_OK,
          "live statement receipt acquisition failed");
  if (acquire_result) (void)sb_engine_result_release(acquire_result);

  const auto parser_uuid = Text(NewUuid(platform::UuidKind::object, 502));
  const auto submission = BuildSubmission(fixture, view, parser_uuid);
  bridge::StatementPackageAdmissionReservationRequest reservation_request;
  reservation_request.receipt = receipt;
  reservation_request.canonical_payload_bytes = submission.stream.data();
  reservation_request.canonical_payload_size = submission.stream.size();
  reservation_request.payload_kind =
      bridge::StatementSblrPayloadKind::kOpcodeStream;
  bridge::StatementPackageAdmissionReservationHandle reservation_handle;
  bridge::StatementPackageAdmissionReservationView reservation_view;
  sb_engine_result_t reservation_result = nullptr;
  Require(bridge::AcquireStatementPackageAdmissionReservation(
              &reservation_request, &reservation_handle, &reservation_view,
              &reservation_result) == SB_ENGINE_STATUS_OK,
          "engine package pre-admission reservation failed");
  if (reservation_result) (void)sb_engine_result_release(reservation_result);
  server::ServerSblrAdmissionRequest admission;
  admission.encoded_sblr_container = submission.container;
  admission.encoded_execution_envelope = submission.ingress;
  admission.admitted_parser_package_uuid = parser_uuid;
  admission.admitted_parser_package_version_major = 1;
  admission.admitted_registry_snapshot_uuid = view.catalog_epoch_uuid;
  admission.authenticated_principal_uuid = Text(fixture.principal_uuid);
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
  admission.package_reservation_handle = reservation_handle.opaque_id;
  admission.reserved_payload_kind = server::ServerSblrPayloadKind::opcode_stream;
  admission.reserved_payload_size = reservation_view.payload_size;
  admission.reserved_record_count = reservation_view.record_count;
  admission.reserved_resource_policy_generation =
      reservation_view.resource_policy_generation;
  admission.reserved_payload_sha256 = reservation_view.payload_sha256;
  const auto admitted = server::AdmitServerSblrEnvelope(admission);
  for (const auto& diagnostic : admitted.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  }
  Require(admitted.admitted && admitted.admission_token,
          "valid typed gateway/executor evidence was not admitted");

  bridge::StatementContextDispatchRequest dispatch;
  dispatch.receipt = receipt; dispatch.engine_session = session.session;
  dispatch.canonical_container_bytes = admitted.admission_token->canonical_container_bytes;
  dispatch.canonical_execution_envelope_bytes = admitted.admission_token->canonical_execution_envelope_bytes;
  dispatch.canonical_operation_bytes = admitted.admission_token->canonical_operation_bytes;
  dispatch.container_sha256 = admitted.admission_token->container_sha256;
  dispatch.execution_envelope_sha256 = admitted.admission_token->execution_envelope_sha256;
  dispatch.operation_sha256 = admitted.admission_token->operation_sha256;
  dispatch.admission_binding_sha256 = admitted.admission_token->admission_binding_sha256;
  dispatch.authenticated_principal_uuid = admitted.admission_token->authenticated_principal_uuid;
  dispatch.catalog_snapshot_uuid = admitted.admission_token->catalog_snapshot_uuid;
  dispatch.engine_mga_statement_uuid = admitted.admission_token->engine_mga_statement_uuid;
  dispatch.engine_mga_snapshot_uuid = admitted.admission_token->engine_mga_snapshot_uuid;
  dispatch.catalog_epoch = admitted.admission_token->catalog_epoch;
  dispatch.security_epoch = admitted.admission_token->security_epoch;
  dispatch.resource_epoch = admitted.admission_token->resource_epoch;
  dispatch.package_admission_reservation = reservation_handle;
  dispatch.admitted_payload_kind =
      bridge::StatementSblrPayloadKind::kOpcodeStream;
  dispatch.gateway_evidence.source = bridge::StatementGatewayEvidenceSource::kLocalObserved;
  dispatch.gateway_evidence.disposition = bridge::StatementGatewayDisposition::kPassThrough;
  dispatch.gateway_evidence.provider_observation_generation = admitted.admission_token->gateway_evidence.provider_observation_generation;
  dispatch.gateway_evidence.canonical_payload_sha256 = admitted.admission_token->gateway_evidence.canonical_payload_sha256;
  dispatch.gateway_evidence.cluster_context_active = admitted.admission_token->gateway_evidence.cluster_context_active;
  dispatch.gateway_evidence.cluster_transaction_active = admitted.admission_token->gateway_evidence.cluster_transaction_active;
  dispatch.gateway_evidence.route_fence_present = admitted.admission_token->gateway_evidence.route_fence_present;
  dispatch.gateway_evidence.route_snapshot_uuid = admitted.admission_token->gateway_evidence.route_snapshot_uuid;
  dispatch.gateway_evidence.route_epoch = admitted.admission_token->gateway_evidence.route_epoch;
  dispatch.gateway_evidence.route_generation = admitted.admission_token->gateway_evidence.route_generation;
  dispatch.gateway_evidence.security_snapshot_uuid = admitted.admission_token->gateway_evidence.security_snapshot_uuid;
  dispatch.gateway_evidence.security_epoch = admitted.admission_token->gateway_evidence.security_epoch;
  dispatch.gateway_evidence.security_observation_generation = admitted.admission_token->gateway_evidence.security_observation_generation;
  dispatch.package_executor_evidence.begin_executor_id = admitted.admission_token->package_executor_evidence.begin_executor_id;
  dispatch.package_executor_evidence.end_executor_id = admitted.admission_token->package_executor_evidence.end_executor_id;
  dispatch.package_executor_evidence.registry_snapshot_uuid = admitted.admission_token->package_executor_evidence.registry_snapshot_uuid;
  dispatch.package_executor_evidence.executor_evidence_generation = admitted.admission_token->package_executor_evidence.executor_evidence_generation;
  dispatch.package_executor_evidence.canonical_payload_sha256 = admitted.admission_token->package_executor_evidence.canonical_payload_sha256;

  sb_engine_result_t result = nullptr;
  const auto status = bridge::DispatchStatementContextReceipt(&dispatch, &result);
  Require(status == SB_ENGINE_STATUS_TIMEOUT && result != nullptr,
          "admitted receipt dispatch did not cancel before contained execution");
  Require(cancellation_probes.load(std::memory_order_relaxed) != 0,
          "production cancellation authority was not consulted");
  sb_engine_execution_summary_view_v1_t summary{};
  sb_engine_command_completion_view_v1_t completion{};
  sb_engine_diagnostic_set_view_t diagnostics{};
  const auto summary_status = sb_engine_result_summary(result, &summary);
  const auto completion_status = sb_engine_result_completion(result, &completion);
  const auto diagnostic_status = sb_engine_result_diagnostics(result, &diagnostics);
  const auto exact_cancel = diagnostic_status == SB_ENGINE_STATUS_OK &&
      diagnostics.diagnostic_count == 1 &&
      std::string_view(diagnostics.diagnostics[0].symbolic_code.data,
                       diagnostics.diagnostics[0].symbolic_code.size_bytes) ==
          "PROCESS.CANCELLED";
  const auto no_operation_completion = completion_status != SB_ENGINE_STATUS_OK ||
      (completion.affected_rows == 0 && completion.operation_id.size_bytes == 0);
  if ((summary_status == SB_ENGINE_STATUS_OK && summary.rows_produced != 0) ||
      !no_operation_completion || !exact_cancel) {
    std::cerr << "summary_status=" << summary_status
              << " rows=" << summary.rows_produced
              << " completion_status=" << completion_status
              << " affected=" << completion.affected_rows
              << " operation_id_size=" << completion.operation_id.size_bytes
              << " diagnostic_status=" << diagnostic_status
              << " diagnostic_count=" << diagnostics.diagnostic_count << '\n';
    Fail("cancelled package published contained output or terminal completion");
  }
  (void)sb_engine_result_release(result);

  // The engine consumed the opaque handle before cancellation. Replay must
  // refuse before any contained operation or second resource release.
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&dispatch, &result) ==
              SB_ENGINE_STATUS_INVALID_HANDLE,
          "consumed package reservation handle replay was not refused");
  if (result) (void)sb_engine_result_release(result);

  bridge::StatementPackageAdmissionReservationHandle released_handle;
  bridge::StatementPackageAdmissionReservationView released_view;
  Require(bridge::AcquireStatementPackageAdmissionReservation(
              &reservation_request, &released_handle, &released_view,
              &reservation_result) == SB_ENGINE_STATUS_OK,
          "focused release-cycle reservation acquisition failed");
  if (reservation_result) {
    (void)sb_engine_result_release(reservation_result);
    reservation_result = nullptr;
  }
  Require(bridge::ReleaseStatementPackageAdmissionReservation(
              released_handle,
              bridge::StatementPackageReservationReleaseReason::kRelease) ==
              SB_ENGINE_STATUS_OK &&
              bridge::ReleaseStatementPackageAdmissionReservation(
                  released_handle,
                  bridge::StatementPackageReservationReleaseReason::kRelease) ==
                  SB_ENGINE_STATUS_ALREADY_RELEASED,
          "opaque reservation release/replay lifecycle was not exact");

  bridge::StatementPackageAdmissionReservationHandle shutdown_handle;
  bridge::StatementPackageAdmissionReservationView shutdown_view;
  Require(bridge::AcquireStatementPackageAdmissionReservation(
              &reservation_request, &shutdown_handle, &shutdown_view,
              &reservation_result) == SB_ENGINE_STATUS_OK,
          "shutdown-cycle reservation acquisition failed");
  if (reservation_result) (void)sb_engine_result_release(reservation_result);
  Require(session.End() == SB_ENGINE_STATUS_OK,
          "engine session shutdown failed");
  Require(bridge::ReleaseStatementPackageAdmissionReservation(
              shutdown_handle,
              bridge::StatementPackageReservationReleaseReason::kRelease) ==
              SB_ENGINE_STATUS_ALREADY_RELEASED,
          "session shutdown did not revoke the live reservation handle");
  Require(bridge::ReleaseStatementContextReceipt(receipt) ==
              SB_ENGINE_STATUS_ALREADY_RELEASED,
          "session shutdown did not revoke the statement receipt");

  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "fixture MGA rollback failed after cancellation");
  return EXIT_SUCCESS;
}
#endif
