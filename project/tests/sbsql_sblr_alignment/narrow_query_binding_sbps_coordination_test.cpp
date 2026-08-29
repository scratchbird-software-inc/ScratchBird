// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "query/narrow_query_binding_authority.hpp"
#include "runtime_platform.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include "server/session_registry.hpp"
#include "server/sbps.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"
#include "wire/narrow_query_binding_codec.hpp"
#include "wire/narrow_query_binding_demand_codec.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace bridge = scratchbird::server_engine_bridge;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace sbps = scratchbird::server::sbps;
namespace server = scratchbird::server;
namespace uuid = scratchbird::core::uuid;
namespace wire = scratchbird::wire;

constexpr std::string_view kInt32DescriptorUuid =
    "019d0000-0000-7000-8000-00000000d716";
constexpr std::string_view kInt32TypeUuid =
    "019d0000-0000-7000-8000-00000000d717";
constexpr std::uint64_t kScanBytes = 64ull * 1024ull * 1024ull;
constexpr std::uint64_t kTransportBytes = 64ull * 1024ull;
constexpr std::uint64_t kFixtureEpochMillis = 1945000000000ull;

[[noreturn]] void Fail(std::string_view detail) {
  std::cerr << "narrow_query_binding_sbps_coordination: " << detail << '\n';
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

std::array<std::uint8_t, 16> UuidBytes(std::string_view text) {
  const auto parsed = uuid::ParseUuid(std::string(text));
  Require(parsed.ok(), "UUID parsing failed");
  std::array<std::uint8_t, 16> bytes{};
  std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(),
            bytes.begin());
  return bytes;
}

wire::NarrowQueryUuid WireUuid(std::string_view text) {
  return UuidBytes(text);
}

sb_engine_uuid_t PublicUuid(const platform::TypedUuid& value) {
  sb_engine_uuid_t result{};
  std::memcpy(result.bytes, value.value.bytes.data(), value.value.bytes.size());
  return result;
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
              result.response_message_type == static_cast<std::uint16_t>(
                  sbps::MessageType::kDiagnostic) &&
              result.response_schema_id == sbps::kSchemaMessageVectorSetV1 &&
              result.frame_flags == (sbps::kFlagResponse |
                                     sbps::kFlagError |
                                     sbps::kFlagFinal) &&
              result.diagnostics.size() == 1 &&
              HasDiagnostic(result, code),
          detail);
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
  context.request_id = "narrow-query-sbps-coordination";
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
  grant.right = "SELECT";
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
                      ("scratchbird_narrow_sbps_" +
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
  table.default_name = "narrow_sbps_values";
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
  if (status != SB_ENGINE_STATUS_OK && engine_result != nullptr) {
    sb_engine_diagnostic_set_view_t diagnostics{};
    if (sb_engine_result_diagnostics(engine_result, &diagnostics) ==
        SB_ENGINE_STATUS_OK) {
      for (std::size_t index = 0; index < diagnostics.diagnostic_count;
           ++index) {
        const auto& diagnostic = diagnostics.diagnostics[index];
        std::cerr.write(diagnostic.symbolic_code.data,
                        diagnostic.symbolic_code.size_bytes);
        std::cerr << '\n';
      }
    }
  }
  if (engine_result != nullptr) (void)sb_engine_result_release(engine_result);
  Require(status == SB_ENGINE_STATUS_OK && receipt,
          "statement receipt acquisition failed");
  return receipt;
}

wire::NarrowQueryBindingDemand MakeDemand(
    const Fixture& fixture,
    const bridge::StatementContextReceiptView& view) {
  wire::NarrowQueryBindingDemand demand;
  demand.statement_receipt_uuid = WireUuid(view.receipt_uuid);
  demand.requested_profile = wire::NarrowQueryProfile::projection_occurrence;
  demand.maximum_mga_relation_decoded_bytes_per_pass = kScanBytes;
  wire::NarrowQuerySourceDemand source;
  source.source_ordinal = 0;
  source.relation_object_hint_present = true;
  source.relation_object_uuid_hint = WireUuid(UuidText(fixture.relation));
  source.explicit_alias = true;
  source.alias_spelling = "narrow_source";
  demand.sources.push_back(std::move(source));
  for (std::uint32_t ordinal = 0; ordinal < 2; ++ordinal) {
    wire::NarrowQueryOutputDemand output;
    output.output_ordinal = ordinal;
    output.source_ordinal = 0;
    output.source_column_spelling = "id";
    output.output_name_present = true;
    output.output_name_spelling = ordinal == 0 ? "left_id" : "right_id";
    demand.outputs.push_back(std::move(output));
  }
  return demand;
}

std::vector<std::uint8_t> EncodeDemand(
    const wire::NarrowQueryBindingDemand& demand) {
  std::vector<std::uint8_t> bytes;
  wire::NarrowQueryBindingDemandError error;
  Require(wire::EncodeNarrowQueryBindingDemand(demand, &bytes, &error),
          "SBQNDR encode failed");
  return bytes;
}

sbps::Frame RequestFrame(const server::ServerSessionRecord& session,
                         std::vector<std::uint8_t> payload) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(
      sbps::MessageType::kQueryNarrowBindingIssueRequest);
  frame.header.payload_schema_id =
      sbps::kSchemaQueryNarrowBindingIssueRequestV1;
  frame.header.stream_id = 17;
  frame.header.sequence_number = 1;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session.connection_uuid;
  frame.header.session_uuid = session.session_uuid;
  frame.header.payload_len = static_cast<std::uint32_t>(payload.size());
  frame.payload = std::move(payload);
  return frame;
}

void StoreLittle64(std::vector<std::uint8_t>* bytes,
                   std::size_t offset,
                   std::uint64_t value) {
  Require(bytes != nullptr && offset + 8 <= bytes->size(),
          "test u64 mutation exceeded carrier");
  for (unsigned index = 0; index < 8; ++index) {
    (*bytes)[offset + index] =
        static_cast<std::uint8_t>((value >> (index * 8u)) & 0xffu);
  }
}

void TestCoordination() {
  Require(sbps::IsKnownMessageType(694) && sbps::IsKnownMessageType(695) &&
              sbps::kSchemaQueryNarrowBindingIssueRequestV1 == 7707 &&
              sbps::kSchemaQueryNarrowBindingIssueResultV1 == 7708,
          "dedicated message/schema constants are not registered");

  auto fixture = MakeFixture();
  PublicSession public_session(fixture);

  auto caller_selected_transport = fixture.transaction;
  caller_selected_transport.maximum_typed_result_transport_bytes_per_packet =
      kTransportBytes;
  bridge::StatementContextAcquireRequest forbidden_request;
  forbidden_request.engine_context = &caller_selected_transport;
  forbidden_request.exact_transaction_uuid =
      caller_selected_transport.transaction_uuid.canonical;
  bridge::StatementContextReceiptHandle forbidden_receipt;
  bridge::StatementContextReceiptView forbidden_view;
  sb_engine_result_t forbidden_result = nullptr;
  const auto forbidden_status = bridge::AcquireStatementContextReceipt(
      public_session.get(), &forbidden_request, &forbidden_receipt,
      &forbidden_view, &forbidden_result);
  if (forbidden_result != nullptr) {
    (void)sb_engine_result_release(forbidden_result);
  }
  Require(forbidden_status == SB_ENGINE_STATUS_CONFLICT &&
              !forbidden_receipt &&
              fixture.transaction
                      .maximum_typed_result_transport_bytes_per_packet == 0,
          "caller-selected typed-result packet ceiling was accepted");

  bridge::StatementContextReceiptView view;
  const auto receipt =
      AcquireReceipt(public_session, fixture.transaction, &view);
  Require(view.maximum_mga_relation_decoded_bytes_per_pass == kScanBytes &&
              view.maximum_typed_result_transport_bytes_per_packet ==
                  kTransportBytes,
          "private statement resource-policy projection drifted");
  api::EngineRequestContext copied_context;
  Require(bridge::CopyStatementContextEngineContextV1(
              receipt, &copied_context, nullptr) == SB_ENGINE_STATUS_OK &&
              copied_context.resource_epoch == view.resource_epoch &&
              copied_context.maximum_mga_relation_decoded_bytes_per_pass ==
                  view.maximum_mga_relation_decoded_bytes_per_pass &&
              copied_context
                      .maximum_typed_result_transport_bytes_per_packet ==
                  view.maximum_typed_result_transport_bytes_per_packet,
          "statement context copy lost an issued resource policy");

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
      server::UuidBytesToText(live.connection_uuid),
      live.server_channel_uuid);
  registry.sessions_by_uuid.emplace(server::UuidBytesToText(live.session_uuid),
                                    live);

  server::ServerStatementContextRecord statement;
  statement.session_uuid = live.session_uuid;
  statement.statement_uuid = view.statement_uuid;
  statement.owning_local_transaction_id = view.owning_local_transaction_id;
  statement.owning_transaction_uuid = view.owning_transaction_uuid;
  statement.receipt = receipt;
  statement.view = view;
  registry.statement_contexts_by_statement_uuid.emplace(
      statement.statement_uuid, statement);

  server::HostedEngineState engine_state;
  const auto canonical = EncodeDemand(MakeDemand(fixture, view));

  auto wrong_schema = RequestFrame(live, canonical);
  wrong_schema.header.payload_schema_id = 7708;
  RequireRefusal(server::HandleQueryNarrowBindingIssue(
                     &registry, engine_state, wrong_schema),
                 "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
                 "wrong request/schema pair did not use 60/2001 refusal");

  auto truncated_payload = canonical;
  truncated_payload.resize(79);
  auto truncated = RequestFrame(live, std::move(truncated_payload));
  RequireRefusal(server::HandleQueryNarrowBindingIssue(
                     &registry, engine_state, truncated),
                 "RESOURCE.BUDGET_EXCEEDED",
                 "truncated mandatory scan byte was not refused exactly");

  auto zero_payload = canonical;
  StoreLittle64(&zero_payload, 72, 0);
  auto zero = RequestFrame(live, std::move(zero_payload));
  RequireRefusal(server::HandleQueryNarrowBindingIssue(
                     &registry, engine_state, zero),
                 "RESOURCE.BUDGET_EXCEEDED",
                 "zero scan-byte policy was not refused exactly");

  auto wrong_scan_payload = canonical;
  StoreLittle64(&wrong_scan_payload, 72, kScanBytes * 2);
  auto wrong_scan = RequestFrame(live, std::move(wrong_scan_payload));
  RequireRefusal(server::HandleQueryNarrowBindingIssue(
                     &registry, engine_state, wrong_scan),
                 "RESOURCE.BUDGET_EXCEEDED",
                 "wrong-context scan-byte policy was not refused exactly");

  auto foreign = RequestFrame(live, canonical);
  foreign.header.connection_uuid = sbps::MakeUuidV7Bytes();
  RequireRefusal(server::HandleQueryNarrowBindingIssue(
                     &registry, engine_state, foreign),
                 "SECURITY.ACCESS_DENIED",
                 "foreign connection was not refused without binding bytes");

  auto foreign_demand = MakeDemand(fixture, view);
  foreign_demand.statement_receipt_uuid =
      WireUuid(UuidText(NewTypedUuid(platform::UuidKind::object)));
  auto foreign_receipt = RequestFrame(live, EncodeDemand(foreign_demand));
  RequireRefusal(server::HandleQueryNarrowBindingIssue(
                     &registry, engine_state, foreign_receipt),
                 "SECURITY.ACCESS_DENIED",
                 "foreign receipt was not hidden before authority issue");

  auto& stored_session =
      registry.sessions_by_uuid.at(server::UuidBytesToText(live.session_uuid));
  const auto live_transaction_uuid = stored_session.transaction_uuid;
  stored_session.transaction_uuid =
      UuidText(NewTypedUuid(platform::UuidKind::transaction));
  auto stale_transaction = RequestFrame(live, canonical);
  RequireRefusal(server::HandleQueryNarrowBindingIssue(
                     &registry, engine_state, stale_transaction),
                 "MGA.TRANSACTION.STALE",
                 "live transaction drift was not refused before issue");
  stored_session.transaction_uuid = live_transaction_uuid;

  fixture.cancelled->store(true);
  auto cancelled = RequestFrame(live, canonical);
  RequireRefusal(server::HandleQueryNarrowBindingIssue(
                     &registry, engine_state, cancelled),
                 "PROCESS.CANCELLED",
                 "cancel-before-publication was not terminally refused");
  fixture.cancelled->store(false);

  auto success_request = RequestFrame(live, canonical);
  const auto success = server::HandleQueryNarrowBindingIssue(
      &registry, engine_state, success_request);
  Require(success.accepted && success.diagnostics.empty() &&
              success.response_message_type == 695 &&
              success.response_schema_id == 7708 &&
              success.frame_flags ==
                  (sbps::kFlagResponse | sbps::kFlagFinal) &&
              success.payload.size() >= 480 &&
              std::equal(std::string_view("SBQNPB01").begin(),
                         std::string_view("SBQNPB01").end(),
                         success.payload.begin()) &&
              platform::LoadLittle64(success.payload.data() + 472) ==
                  kScanBytes,
          "successful issue was not exact raw 695/7708 SBQNPB01");
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
  const auto decoded_success = sbps::DecodeFrameBytes(
      encoded_success, wire::kNarrowQueryMaximumCarrierBytes +
                           sbps::kHeaderBytes);
  Require(decoded_success.ok() &&
              decoded_success.frame->header.message_type == 695 &&
              decoded_success.frame->header.payload_schema_id == 7708 &&
              decoded_success.frame->header.stream_id ==
                  success_request.header.stream_id &&
              decoded_success.frame->header.request_uuid ==
                  success_request.header.request_uuid &&
              decoded_success.frame->header.connection_uuid ==
                  success_request.header.connection_uuid &&
              decoded_success.frame->header.session_uuid ==
                  success_request.header.session_uuid &&
              decoded_success.frame->payload == success.payload,
          "success framing wrapped, altered, or decorrelated SBQNPB01");

  copied_context.trace_tags = {"private_narrow_query_binding_consumer"};
  api::EngineNarrowQueryBindingAuthorityConsumeRequestV1 consume;
  consume.context = copied_context;
  consume.exact_binding_bytes = success.payload;
  auto consumed = api::ConsumeNarrowQueryBindingAuthorityV1(consume);
  RequireOk(consumed, "private issued binding consume failed");
  api::EngineNarrowQueryBindingAuthoritySnapshotV1 snapshot;
  api::EngineApiDiagnostic snapshot_diagnostic;
  Require(api::CopyNarrowQueryBindingAuthoritySnapshotV1(
              consumed.authority, &snapshot, &snapshot_diagnostic) &&
              snapshot.resource_grant
                      .maximum_typed_result_transport_bytes_per_packet ==
                  kTransportBytes &&
              snapshot.resource_grant.grant_generation ==
                  snapshot.binding.resource_grant_generation &&
              WireUuid(snapshot.resource_grant.grant_receipt_uuid.canonical) ==
                  snapshot.binding.resource_grant_receipt_uuid,
          "handler did not privately pin the typed-result grant");

  RequireRefusal(server::HandleQueryNarrowBindingIssue(
                     &registry, engine_state, success_request),
                 "PARSER_SERVER_IPC.SEQUENCE_INVALID",
                 "duplicate request identity was not refused before replay");

  auto second_request = RequestFrame(live, canonical);
  RequireRefusal(server::HandleQueryNarrowBindingIssue(
                     &registry, engine_state, second_request),
                 "MGA.TRANSACTION.STALE",
                 "second issue did not refuse stale without binding bytes");

  Require(!api::ReleaseNarrowQueryBindingAuthorityV1(&consumed.authority).error,
          "consumed binding authority release failed");

  const auto statement_uuid = view.statement_uuid;
  Require(server::ReleaseServerStatementContext(&registry, statement_uuid),
          "statement receipt release failed");
  auto released_request = RequestFrame(live, canonical);
  RequireRefusal(server::HandleQueryNarrowBindingIssue(
                     &registry, engine_state, released_request),
                 "SECURITY.ACCESS_DENIED",
                 "released receipt did not refuse without binding bytes");

  api::EngineRollbackTransactionRequest rollback;
  rollback.context = fixture.transaction;
  RequireOk(api::EngineRollbackTransaction(rollback),
            "fixture rollback failed");
}

}  // namespace

int main() {
  TestCoordination();
  return EXIT_SUCCESS;
}
