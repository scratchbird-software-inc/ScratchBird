// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "ipc_server.hpp"
#include "lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "parser_server_client.hpp"
#include "parsers/sbsql_worker/cache/sblr_template_cache.hpp"
#include "parsers/sbsql_worker/metrics/parser_metrics.hpp"
#include "parsers/sbsql_worker/wire/sbsql_test_wire.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include "session_registry.hpp"
#include "sbps.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <chrono>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace bridge = scratchbird::server_engine_bridge;
namespace db = scratchbird::storage::database;
namespace ipc = scratchbird::parser::ipc;
namespace platform = scratchbird::core::platform;
namespace sbps = scratchbird::server::sbps;
namespace server = scratchbird::server;
namespace uuid = scratchbird::core::uuid;
namespace sbsql = scratchbird::parser::sbsql;

constexpr std::string_view kFullRoutePrincipal = "qow_packet7_user";
constexpr std::string_view kFullRoutePassword =
    "QOW-Packet7-live-route-password";
constexpr std::string_view kFullRouteCredentialFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:"
    "verifier=7b622f17d15a5e6f2d5122c606937dfc"
    "76f5982782adb52b03f7b1ca024f72c9";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

template <typename TResult>
void RequireEngineOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Fail(message);
  }
}

platform::TypedUuid NewTypedUuid(platform::UuidKind kind,
                                 std::uint64_t salt) {
  const auto timestamp = 1945000000000ull + (salt % 1'000'000ull);
  if (!uuid::UuidKindAllowsDurableIdentity(kind)) {
    const auto raw = uuid::GenerateCompatibilityUnixTimeV7(timestamp);
    Require(raw.ok(), "statement-context UUID generation failed");
    const auto typed = uuid::MakeTypedUuid(kind, raw.value);
    Require(typed.ok(), "statement-context UUID typing failed");
    return typed.value;
  }
  const auto durable = uuid::GenerateEngineIdentityV7(kind, timestamp);
  Require(durable.ok(), "statement-context UUID generation failed");
  return durable.value;
}

std::string NewUuidText(platform::UuidKind kind, std::uint64_t salt) {
  return uuid::UuidToString(NewTypedUuid(kind, salt).value);
}

sb_engine_uuid_t PublicUuid(const platform::TypedUuid& typed) {
  sb_engine_uuid_t out{};
  std::memcpy(out.bytes, typed.value.bytes.data(), typed.value.bytes.size());
  return out;
}

struct Fixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string filespace_uuid;
  platform::TypedUuid principal_uuid;
  platform::TypedUuid session_uuid;
  platform::TypedUuid schema_uuid;
  platform::TypedUuid relation_uuid;
  std::uint64_t resource_epoch = 1;
  std::uint64_t salt = 0;

  Fixture() = default;
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
  Fixture(Fixture&& other) noexcept
      : directory(std::move(other.directory)),
        database_path(std::move(other.database_path)),
        database_uuid(std::move(other.database_uuid)),
        filespace_uuid(std::move(other.filespace_uuid)),
        principal_uuid(other.principal_uuid),
        session_uuid(other.session_uuid),
        schema_uuid(other.schema_uuid),
        relation_uuid(other.relation_uuid),
        resource_epoch(other.resource_epoch),
        salt(other.salt) {
    other.directory.clear();
  }
  ~Fixture() {
    std::error_code ignored;
    if (!directory.empty()) std::filesystem::remove_all(directory, ignored);
  }
};

Fixture CreateFixture(bool credentialed_full_route = false) {
  Fixture fixture;
  fixture.salt = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  fixture.directory = std::filesystem::temp_directory_path() /
                      ("qow_live_statement_context_" +
                       std::to_string(fixture.salt));
  std::filesystem::create_directories(fixture.directory);
  fixture.database_path = fixture.directory / "live_context.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid =
      NewTypedUuid(platform::UuidKind::database, fixture.salt + 1);
  create.filespace_uuid =
      NewTypedUuid(platform::UuidKind::filespace, fixture.salt + 2);
  create.creation_unix_epoch_millis =
      1945000000000ull + (fixture.salt % 1'000'000ull) + 3;
  create.page_size = 16384;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  if (credentialed_full_route) {
    create.resource_seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
    create.require_resource_seed_pack = true;
    create.allow_minimal_resource_bootstrap = false;
    create.bootstrap_principal_name = std::string(kFullRoutePrincipal);
    create.bootstrap_credential_fingerprint =
        std::string(kFullRouteCredentialFingerprint);
    create.require_bootstrap_principal = true;
    create.allow_uncredentialed_bootstrap = false;
  }
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "statement-context database creation failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.filespace_uuid = uuid::UuidToString(create.filespace_uuid.value);
  fixture.principal_uuid = credentialed_full_route
                               ? created.bootstrap_principal_uuid
                               : NewTypedUuid(platform::UuidKind::principal,
                                              fixture.salt + 4);
  fixture.session_uuid =
      NewTypedUuid(platform::UuidKind::session, fixture.salt + 5);
  fixture.schema_uuid =
      NewTypedUuid(platform::UuidKind::schema, fixture.salt + 6);
  fixture.relation_uuid =
      NewTypedUuid(platform::UuidKind::object, fixture.salt + 7);
  fixture.resource_epoch =
      created.state.resource_seed_catalog.resource_epoch == 0
          ? 1
          : created.state.resource_seed_catalog.resource_epoch;
  if (!credentialed_full_route) {
    const auto empty_inventory =
        db::PersistLocalTransactionInventoryToDatabase(
            fixture.database_path.string(),
            scratchbird::transaction::mga::MakeEmptyLocalTransactionInventory());
    Require(empty_inventory.ok(),
            "statement-context empty inventory initialization failed");
  }
  return fixture;
}

api::EngineRequestContext BeginTransaction(const Fixture& fixture) {
  api::EngineBeginTransactionRequest begin;
  begin.context.trust_mode = api::EngineTrustMode::server_isolated;
  begin.context.request_id = "qow-live-statement-context-begin";
  begin.context.database_path = fixture.database_path.string();
  begin.context.database_uuid.canonical = fixture.database_uuid;
  begin.context.principal_uuid.canonical =
      uuid::UuidToString(fixture.principal_uuid.value);
  begin.context.session_uuid.canonical =
      uuid::UuidToString(fixture.session_uuid.value);
  begin.context.security_context_present = true;
  begin.context.catalog_generation_id = 1;
  begin.context.security_epoch = 1;
  begin.context.resource_epoch = fixture.resource_epoch;
  begin.context.name_resolution_epoch = 1;
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  RequireEngineOk(begun, "statement-context transaction begin failed");

  auto context = begin.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 6);
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
  context.optimizer_route_epoch = 1;
  context.optimizer_route_generation = 1;
  context.optimizer_memory_budget_bytes = 64 * 1024 * 1024;
  context.optimizer_maximum_candidate_count = 131072;
  context.optimizer_maximum_memo_groups = 131072;
  context.optimizer_maximum_search_steps = 1048576;
  context.optimizer_maximum_planning_time_ns = 5'000'000'000ull;
  context.optimizer_spill_allowed = true;
  return context;
}

class PublicSession {
 public:
  PublicSession(const Fixture& fixture, platform::TypedUuid session_uuid)
      : database_path_(fixture.database_path.string()) {
    sb_engine_open_params_v1_t open{};
    open.struct_size = sizeof(open);
    open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    open.database_path_utf8 = database_path_.data();
    open.database_path_size = database_path_.size();
    open.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
    Require(sb_engine_open(&open, &engine_, nullptr) == SB_ENGINE_STATUS_OK &&
                engine_ != nullptr,
            "statement-context public engine open failed");

    sb_engine_session_params_v1_t begin{};
    begin.struct_size = sizeof(begin);
    begin.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    begin.effective_user_uuid = PublicUuid(fixture.principal_uuid);
    begin.session_uuid = PublicUuid(session_uuid);
    begin.default_language_utf8 = "en";
    begin.default_language_size = 2;
    begin.trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
    Require(sb_engine_session_begin(engine_, &begin, &session_, nullptr) ==
                    SB_ENGINE_STATUS_OK &&
                session_ != nullptr,
            "statement-context public session begin failed");
  }

  PublicSession(const PublicSession&) = delete;
  PublicSession& operator=(const PublicSession&) = delete;

  ~PublicSession() {
    End();
    if (engine_ != nullptr) (void)sb_engine_close(engine_, nullptr);
  }

  sb_engine_session_t get() const { return session_; }

  void End() {
    if (session_ == nullptr) return;
    sb_engine_session_end_params_v1_t end{};
    end.struct_size = sizeof(end);
    end.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    end.rollback_active_transactions = 1;
    end.cancel_open_results = 1;
    Require(sb_engine_session_end(session_, &end, nullptr) ==
                SB_ENGINE_STATUS_OK,
            "statement-context public session end failed");
    session_ = nullptr;
  }

 private:
  std::string database_path_;
  sb_engine_handle_t engine_ = nullptr;
  sb_engine_session_t session_ = nullptr;
};

bridge::StatementContextReceiptHandle Acquire(
    sb_engine_session_t session,
    const api::EngineRequestContext& context,
    bridge::StatementContextReceiptView* view,
    sb_engine_status_t expected = SB_ENGINE_STATUS_OK) {
  bridge::StatementContextAcquireRequest request;
  request.engine_context = &context;
  request.exact_transaction_uuid = context.transaction_uuid.canonical;
  bridge::StatementContextReceiptHandle receipt;
  sb_engine_result_t result = nullptr;
  const auto status = bridge::AcquireStatementContextReceipt(
      session, &request, &receipt, view, &result);
  if (result != nullptr) (void)sb_engine_result_release(result);
  Require(status == expected, "statement-context acquisition status drifted");
  return receipt;
}

void AssertDistinctReceiptIdentities(
    const bridge::StatementContextReceiptView& view) {
  const std::set<std::string> identities = {
      view.receipt_uuid,
      view.statement_uuid,
      view.owning_transaction_uuid,
      view.statement_snapshot_uuid,
      view.statement_metadata_snapshot_uuid,
      view.catalog_epoch_uuid,
      view.security_context_uuid,
      view.optimizer_capability_snapshot_uuid,
      view.optimizer_resource_snapshot_uuid,
      view.optimizer_route_snapshot_uuid,
  };
  Require(identities.size() == 10,
          "engine-issued statement-context identities are not distinct");
  Require(identities.find({}) == identities.end(),
          "engine-issued statement-context identity is empty");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  RequireEngineOk(api::EngineRollbackTransaction(rollback),
                  "statement-context transaction rollback failed");
}

api::EngineLocalizedName PrimaryName(std::string name) {
  api::EngineLocalizedName localized;
  localized.language_tag = "en";
  localized.name_class = "primary";
  localized.name = std::move(name);
  localized.raw_name_text = localized.name;
  localized.display_name = localized.name;
  localized.default_name = true;
  return localized;
}

void CreateObjectBackedRelation(Fixture* fixture) {
  Require(fixture != nullptr, "object-backed fixture is missing");
  api::EngineBeginTransactionRequest begin;
  begin.context.trust_mode = api::EngineTrustMode::server_isolated;
  begin.context.request_id = "qow-packet7-create-object";
  begin.context.database_path = fixture->database_path.string();
  begin.context.database_uuid.canonical = fixture->database_uuid;
  begin.context.principal_uuid.canonical =
      uuid::UuidToString(fixture->principal_uuid.value);
  begin.context.session_uuid.canonical =
      uuid::UuidToString(fixture->session_uuid.value);
  begin.context.security_context_present = true;
  begin.context.catalog_generation_id = 1;
  begin.context.security_epoch = 1;
  begin.context.resource_epoch = fixture->resource_epoch;
  begin.context.name_resolution_epoch = 1;
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  RequireEngineOk(begun, "object-backed fixture transaction begin failed");

  auto context = begin.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;

  api::EngineCreateSchemaRequest schema;
  schema.context = context;
  schema.target_object.uuid.canonical =
      uuid::UuidToString(fixture->schema_uuid.value);
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(PrimaryName("qow_packet7"));
  RequireEngineOk(api::EngineCreateSchema(schema),
                  "object-backed fixture schema create failed");

  api::EngineCreateTableRequest table;
  table.context = context;
  table.target_schema = schema.target_object;
  table.requested_table_uuid.canonical =
      uuid::UuidToString(fixture->relation_uuid.value);
  table.table_names.push_back(PrimaryName("qow_packet7_relation"));
  api::EngineColumnDefinition column;
  column.ordinal = 0;
  column.names.push_back(PrimaryName("integer_value"));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "integer";
  column.descriptor.encoded_descriptor = "type=integer";
  column.nullable = true;
  table.table_columns.push_back(std::move(column));
  api::EngineColumnDefinition auxiliary_column;
  auxiliary_column.ordinal = 1;
  auxiliary_column.names.push_back(PrimaryName("auxiliary_value"));
  auxiliary_column.descriptor.descriptor_kind = "scalar";
  auxiliary_column.descriptor.canonical_type_name = "integer";
  auxiliary_column.descriptor.encoded_descriptor = "type=integer";
  auxiliary_column.nullable = true;
  table.table_columns.push_back(std::move(auxiliary_column));
  api::EngineColumnDefinition nullable_order_column;
  nullable_order_column.ordinal = 2;
  nullable_order_column.names.push_back(PrimaryName("nullable_order_value"));
  nullable_order_column.descriptor.descriptor_kind = "scalar";
  nullable_order_column.descriptor.canonical_type_name = "integer";
  nullable_order_column.descriptor.encoded_descriptor = "type=integer";
  nullable_order_column.nullable = true;
  table.table_columns.push_back(std::move(nullable_order_column));
  api::EngineColumnDefinition boolean_column;
  boolean_column.ordinal = 3;
  boolean_column.names.push_back(PrimaryName("nullable_boolean_value"));
  boolean_column.descriptor.descriptor_kind = "scalar";
  boolean_column.descriptor.canonical_type_name = "boolean";
  boolean_column.descriptor.encoded_descriptor = "type=boolean";
  boolean_column.nullable = true;
  table.table_columns.push_back(std::move(boolean_column));
  api::EngineColumnDefinition text_column;
  text_column.ordinal = 4;
  text_column.names.push_back(PrimaryName("text_value"));
  text_column.descriptor.descriptor_kind = "scalar";
  text_column.descriptor.canonical_type_name = "text";
  text_column.descriptor.encoded_descriptor = "type=text";
  text_column.nullable = true;
  table.table_columns.push_back(std::move(text_column));
  RequireEngineOk(api::EngineCreateTable(table),
                  "object-backed fixture table create failed");

  api::EngineInsertRowsRequest insert;
  insert.context = context;
  insert.target_table.uuid.canonical =
      uuid::UuidToString(fixture->relation_uuid.value);
  insert.target_table.object_kind = "table";
  for (std::int64_t value = 1; value <= 3; ++value) {
    api::EngineTypedValue typed;
    typed.descriptor.descriptor_kind = "scalar";
    typed.descriptor.canonical_type_name = "integer";
    typed.descriptor.encoded_descriptor = "type=integer";
    typed.encoded_value = std::to_string(value);
    api::EngineTypedValue auxiliary_typed;
    auxiliary_typed.descriptor.descriptor_kind = "scalar";
    auxiliary_typed.descriptor.canonical_type_name = "integer";
    auxiliary_typed.descriptor.encoded_descriptor = "type=integer";
    auxiliary_typed.encoded_value = std::to_string(100 + value);
    api::EngineTypedValue nullable_order_typed;
    nullable_order_typed.descriptor.descriptor_kind = "scalar";
    nullable_order_typed.descriptor.canonical_type_name = "integer";
    nullable_order_typed.descriptor.encoded_descriptor = "type=integer";
    if (value == 2) {
      nullable_order_typed.is_null = true;
      nullable_order_typed.state = api::EngineValueState::sql_null;
    } else {
      nullable_order_typed.encoded_value = value == 1 ? "20" : "10";
    }
    api::EngineTypedValue boolean_typed;
    boolean_typed.descriptor.descriptor_kind = "scalar";
    boolean_typed.descriptor.canonical_type_name = "boolean";
    boolean_typed.descriptor.encoded_descriptor = "type=boolean";
    if (value == 3) {
      boolean_typed.is_null = true;
      boolean_typed.state = api::EngineValueState::sql_null;
    } else {
      boolean_typed.encoded_value = value == 1 ? "true" : "false";
    }
    api::EngineTypedValue text_typed;
    text_typed.descriptor.descriptor_kind = "scalar";
    text_typed.descriptor.canonical_type_name = "text";
    text_typed.descriptor.encoded_descriptor = "type=text";
    text_typed.encoded_value =
        value == 2 ? "beta" : "alpha";
    api::EngineRowValue row;
    row.fields.push_back({"integer_value", std::move(typed)});
    row.fields.push_back({"auxiliary_value", std::move(auxiliary_typed)});
    row.fields.push_back(
        {"nullable_order_value", std::move(nullable_order_typed)});
    row.fields.push_back(
        {"nullable_boolean_value", std::move(boolean_typed)});
    row.fields.push_back({"text_value", std::move(text_typed)});
    insert.input_rows.push_back(std::move(row));
  }
  insert.estimated_row_count = insert.input_rows.size();
  const auto inserted = api::EngineInsertRows(insert);
  RequireEngineOk(inserted, "object-backed fixture row insert failed");
  Require(inserted.inserted_count == 3,
          "object-backed fixture did not insert three rows");

  api::EngineCommitTransactionRequest commit;
  commit.context = context;
  RequireEngineOk(api::EngineCommitTransaction(commit),
                  "object-backed fixture transaction commit failed");
}

server::HostedEngineState FullRouteEngineState(const Fixture& fixture) {
  server::HostedEngineState state;
  state.engine_context_active = true;
  server::HostedDatabaseSnapshot database;
  database.state = server::HostedDatabaseState::kOpen;
  database.database_path = fixture.database_path.string();
  database.database_uuid = fixture.database_uuid;
  database.filespace_uuid = fixture.filespace_uuid;
  database.page_size_bytes = 16384;
  database.database_created = true;
  database.database_open = true;
  database.read_only = false;
  database.write_admission_fenced = false;
  database.config_policy_security_lifecycle_present = true;
  database.config_source_epoch = 1;
  database.config_reload_generation = 1;
  database.capability_policy_generation = 1;
  database.policy_generation = 1;
  database.security_epoch = 1;
  database.security_provider_family = "local_password";
  database.security_provider_generation = 1;
  database.security_provider_state = "healthy";
  database.default_policy_installed = true;
  database.cache_invalidation_epoch = fixture.resource_epoch;
  state.databases.push_back(std::move(database));
  return state;
}

void PrintMessages(const sbsql::MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    for (const auto& field : diagnostic.fields) {
      std::cerr << field.name << '=' << field.value << '\n';
    }
  }
}

void VerifyFullParserServerRoute(const Fixture& fixture) {
  constexpr std::string_view kSourceFreeNativeSelect =
      "SELECT key_a,COUNT(*),SUM(amount) FROM (VALUES (1,5), (1,7)) "
      "AS input(key_a,amount) GROUP BY key_a;";
  server::ServerBootstrapConfig config;
  config.mode = server::ServerMode::kForeground;
  config.control_dir = fixture.directory / "server-control";
  config.data_dir = fixture.directory / "server-data";
  config.lifecycle_state_file = config.control_dir / "lifecycle.state";
  config.lifecycle_journal_file = config.control_dir / "lifecycle.journal";
  config.sbps_endpoint = config.control_dir / "packet7.sbps.sock";
  config.database_default_path = fixture.database_path;
  config.embedded_direct_mode = true;
  config.sbps_enabled = true;
  config.database_daemon_scope = "shared";

  server::ServerLifecycleArtifacts artifacts;
  artifacts.generation = 1;
  const auto engine_state = FullRouteEngineState(fixture);
  server::ServerIpcEndpointResult endpoint_result;
  std::mutex ready_mutex;
  std::condition_variable ready_condition;
  bool ready = false;

  server::ResetParserServerStopRequest();
  server::ParserServerIpcLifecycleCallbacks callbacks;
  callbacks.on_ready = [&] {
    {
      std::lock_guard<std::mutex> lock(ready_mutex);
      ready = true;
    }
    ready_condition.notify_one();
  };
  std::thread endpoint([&] {
    endpoint_result = server::RunParserServerIpcEndpoint(
        config, artifacts, engine_state, callbacks);
    ready_condition.notify_one();
  });

  {
    std::unique_lock<std::mutex> lock(ready_mutex);
    const bool signaled = ready_condition.wait_for(
        lock, std::chrono::seconds(10), [&] { return ready; });
    if (!signaled) {
      server::RequestParserServerStop();
      lock.unlock();
      endpoint.join();
      for (const auto& diagnostic : endpoint_result.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      }
      Fail("parser-server endpoint did not become ready");
    }
  }

  {
    sbsql::ParserConfig parser_config;
    parser_config.parser_uuid = "qow-packet7-live-parser-worker";
    parser_config.server_endpoint = config.sbps_endpoint.string();
    parser_config.database_token = "default";
    parser_config.default_search_path = {"qow_packet7"};
    sbsql::ParserMetrics metrics;
    sbsql::SblrTemplateCache cache;
    sbsql::SbsqlTestWireSession parser(parser_config, &metrics, &cache);
    const auto authenticated = parser.HandleLine(
        std::string("AUTH ") + std::string(kFullRoutePrincipal) + " " +
        std::string(kFullRoutePassword));
    Require(authenticated.text.find("OK AUTHENTICATED") !=
                std::string::npos &&
                parser.session().authenticated &&
                parser.session().local_transaction_id != 0 &&
                !parser.session().transaction_uuid.empty(),
            "full parser-server route authentication/attach failed");

    auto source_free = parser.RunPipeline(kSourceFreeNativeSelect, true);
    if (!source_free.accepted) PrintMessages(source_free.messages);
    if (!source_free.accepted ||
        source_free.server_operation_id != "query.execute" ||
        !source_free.server_cursor_uuid.empty()) {
      std::cerr << "source-free route accepted=" << source_free.accepted
                << " operation=" << source_free.server_operation_id
                << " cursor=" << source_free.server_cursor_uuid
                << " rows=" << source_free.server_row_count << '\n';
    }
    Require(source_free.accepted &&
                source_free.server_operation_id == "query.execute" &&
                source_free.server_cursor_uuid.empty(),
            "source-free native SELECT did not complete the full live route");

    auto object_backed = parser.RunPipeline(
        "SELECT * FROM qow_packet7.qow_packet7_relation;", true);
    if (!object_backed.accepted) PrintMessages(object_backed.messages);
    Require(object_backed.accepted &&
                object_backed.server_operation_id == "query.execute" &&
                object_backed.server_cursor_uuid.empty() &&
                object_backed.server_row_count == 3,
            "object-backed native SELECT did not complete the full live route");

    auto object_backed_count = parser.RunPipeline(
        "SELECT COUNT(*) FROM qow_packet7.qow_packet7_relation;", true);
    if (!object_backed_count.accepted) {
      PrintMessages(object_backed_count.messages);
    }
    Require(object_backed_count.accepted &&
                object_backed_count.server_operation_id == "query.execute" &&
                object_backed_count.server_cursor_uuid.empty() &&
                object_backed_count.server_row_count == 1 &&
                object_backed_count.server_result_payload.find("row_count=3") !=
                    std::string::npos,
            "object-backed global COUNT(*) did not complete the canonical "
            "heap aggregate route");

    auto object_backed_filtered_count = parser.RunPipeline(
        "SELECT COUNT(*) FROM qow_packet7.qow_packet7_relation WHERE "
        "integer_value >= 2;",
        true);
    if (!object_backed_filtered_count.accepted) {
      PrintMessages(object_backed_filtered_count.messages);
    }
    Require(object_backed_filtered_count.accepted &&
                object_backed_filtered_count.server_operation_id ==
                    "query.execute" &&
                object_backed_filtered_count.server_cursor_uuid.empty() &&
                object_backed_filtered_count.server_row_count == 1 &&
                object_backed_filtered_count.server_result_payload.find(
                    "row_count=2") != std::string::npos,
            "object-backed WHERE/global COUNT(*) composition did not preserve "
            "the filtered MGA heap input");

    auto object_backed_count_limit = parser.RunPipeline(
        "SELECT COUNT(*) FROM qow_packet7.qow_packet7_relation LIMIT 1;",
        true);
    if (!object_backed_count_limit.accepted) {
      PrintMessages(object_backed_count_limit.messages);
    }
    Require(object_backed_count_limit.accepted &&
                object_backed_count_limit.server_operation_id ==
                    "query.execute" &&
                object_backed_count_limit.server_cursor_uuid.empty() &&
                object_backed_count_limit.server_row_count == 1 &&
                object_backed_count_limit.server_result_payload.find(
                    "row_count=3") != std::string::npos,
            "object-backed global COUNT(*)/LIMIT composition did not publish "
            "its aggregate result");

    auto object_backed_count_expression = parser.RunPipeline(
        "SELECT COUNT(nullable_order_value) FROM "
        "qow_packet7.qow_packet7_relation;",
        true);
    if (!object_backed_count_expression.accepted) {
      PrintMessages(object_backed_count_expression.messages);
    }
    Require(object_backed_count_expression.accepted &&
                object_backed_count_expression.server_operation_id ==
                    "query.execute" &&
                object_backed_count_expression.server_cursor_uuid.empty() &&
                object_backed_count_expression.server_row_count == 1 &&
                object_backed_count_expression.server_result_payload.find(
                    "row_count=2") != std::string::npos,
            "object-backed COUNT(expression) did not exclude the persisted "
            "NULL value");

    auto object_backed_sum = parser.RunPipeline(
        "SELECT SUM(integer_value) FROM qow_packet7.qow_packet7_relation;",
        true);
    if (!object_backed_sum.accepted) PrintMessages(object_backed_sum.messages);
    Require(object_backed_sum.accepted &&
                object_backed_sum.server_operation_id == "query.execute" &&
                object_backed_sum.server_cursor_uuid.empty() &&
                object_backed_sum.server_row_count == 1 &&
                object_backed_sum.server_result_payload.find("total_amount=6") !=
                    std::string::npos,
            "object-backed SUM(expression) did not execute through the "
            "canonical aggregate registry");

    auto object_backed_filtered_sum = parser.RunPipeline(
        "SELECT SUM(integer_value) FROM qow_packet7.qow_packet7_relation "
        "WHERE auxiliary_value >= 102;",
        true);
    if (!object_backed_filtered_sum.accepted) {
      PrintMessages(object_backed_filtered_sum.messages);
    }
    Require(object_backed_filtered_sum.accepted &&
                object_backed_filtered_sum.server_operation_id ==
                    "query.execute" &&
                object_backed_filtered_sum.server_cursor_uuid.empty() &&
                object_backed_filtered_sum.server_row_count == 1 &&
                object_backed_filtered_sum.server_result_payload.find(
                    "total_amount=5") != std::string::npos,
            "object-backed WHERE/SUM composition did not aggregate only the "
            "visible filtered heap rows");

    auto object_backed_avg = parser.RunPipeline(
        "SELECT AVG(integer_value) FROM qow_packet7.qow_packet7_relation;",
        true);
    if (!object_backed_avg.accepted) PrintMessages(object_backed_avg.messages);
    Require(object_backed_avg.accepted &&
                object_backed_avg.server_operation_id == "query.execute" &&
                object_backed_avg.server_cursor_uuid.empty() &&
                object_backed_avg.server_row_count == 1 &&
                object_backed_avg.server_result_payload.find(
                    "average_value=2") != std::string::npos,
            "object-backed AVG(expression) did not execute through the "
            "engine-issued canonical aggregate registry");

    auto object_backed_min = parser.RunPipeline(
        "SELECT MIN(nullable_order_value) FROM "
        "qow_packet7.qow_packet7_relation;",
        true);
    if (!object_backed_min.accepted) PrintMessages(object_backed_min.messages);
    Require(object_backed_min.accepted &&
                object_backed_min.server_operation_id == "query.execute" &&
                object_backed_min.server_cursor_uuid.empty() &&
                object_backed_min.server_row_count == 1 &&
                object_backed_min.server_result_payload.find(
                    "minimum_value=10") != std::string::npos,
            "object-backed MIN(expression) did not ignore NULL and retain the "
            "least visible persisted value");

    auto object_backed_max = parser.RunPipeline(
        "SELECT MAX(nullable_order_value) FROM "
        "qow_packet7.qow_packet7_relation;",
        true);
    if (!object_backed_max.accepted) PrintMessages(object_backed_max.messages);
    Require(object_backed_max.accepted &&
                object_backed_max.server_operation_id == "query.execute" &&
                object_backed_max.server_cursor_uuid.empty() &&
                object_backed_max.server_row_count == 1 &&
                object_backed_max.server_result_payload.find(
                    "maximum_value=20") != std::string::npos,
            "object-backed MAX(expression) did not ignore NULL and retain the "
            "greatest visible persisted value");

    struct AggregateProof {
      std::string_view function;
      std::string_view expected_payload;
    };
    static constexpr std::array<AggregateProof, 6>
        kStatisticalAggregateProofs{{
            {"STDDEV_POP", "stddev_pop_value=0.816496"},
            {"VARIANCE_POP", "variance_pop_value=0.666666"},
            {"STDDEV", "stddev_value=1"},
            {"VARIANCE", "variance_value=1"},
            {"STDDEV_SAMP", "stddev_samp_value=1"},
            {"VARIANCE_SAMP", "variance_samp_value=1"},
        }};
    for (const auto& proof : kStatisticalAggregateProofs) {
      const auto sql = "SELECT " + std::string(proof.function) +
                       "(integer_value) FROM "
                       "qow_packet7.qow_packet7_relation;";
      auto aggregate = parser.RunPipeline(sql, true);
      if (!aggregate.accepted) PrintMessages(aggregate.messages);
      Require(aggregate.accepted &&
                  aggregate.server_operation_id == "query.execute" &&
                  aggregate.server_cursor_uuid.empty() &&
                  aggregate.server_row_count == 1 &&
                  aggregate.server_result_payload.find(
                      proof.expected_payload) != std::string::npos,
              "object-backed unary statistical aggregate did not execute "
              "through the complete engine-issued aggregate registry");
    }

    static constexpr std::array<AggregateProof, 3>
        kBooleanAggregateProofs{{
            {"BOOL_AND", "bool_and_value=false"},
            {"BOOL_OR", "bool_or_value=true"},
            {"EVERY", "every_value=false"},
        }};
    for (const auto& proof : kBooleanAggregateProofs) {
      const auto sql = "SELECT " + std::string(proof.function) +
                       "(nullable_boolean_value) FROM "
                       "qow_packet7.qow_packet7_relation;";
      auto aggregate = parser.RunPipeline(sql, true);
      if (!aggregate.accepted) PrintMessages(aggregate.messages);
      Require(aggregate.accepted &&
                  aggregate.server_operation_id == "query.execute" &&
                  aggregate.server_cursor_uuid.empty() &&
                  aggregate.server_row_count == 1 &&
                  aggregate.server_result_payload.find(
                      proof.expected_payload) != std::string::npos,
              "object-backed boolean aggregate did not execute through the "
              "complete engine-issued aggregate registry");
    }

    static constexpr std::array<AggregateProof, 12>
        kPairStatisticalAggregateProofs{{
            {"CORR", "corr_value=1"},
            {"COVAR_POP", "covar_pop_value=0.666666"},
            {"COVAR_SAMP", "covar_samp_value=1"},
            {"REGR_COUNT", "regr_count_value=3"},
            {"REGR_AVGX", "regr_avgx_value=2"},
            {"REGR_AVGY", "regr_avgy_value=102"},
            {"REGR_INTERCEPT", "regr_intercept_value=100"},
            {"REGR_R2", "regr_r2_value=1"},
            {"REGR_SLOPE", "regr_slope_value=1"},
            {"REGR_SXX", "regr_sxx_value=2"},
            {"REGR_SXY", "regr_sxy_value=2"},
            {"REGR_SYY", "regr_syy_value=2"},
        }};
    for (const auto& proof : kPairStatisticalAggregateProofs) {
      const auto sql = "SELECT " + std::string(proof.function) +
                       "(auxiliary_value, integer_value) FROM "
                       "qow_packet7.qow_packet7_relation;";
      auto aggregate = parser.RunPipeline(sql, true);
      if (!aggregate.accepted) PrintMessages(aggregate.messages);
      Require(aggregate.accepted &&
                  aggregate.server_operation_id == "query.execute" &&
                  aggregate.server_cursor_uuid.empty() &&
                  aggregate.server_row_count == 1 &&
                  aggregate.server_result_payload.find(
                      proof.expected_payload) != std::string::npos,
              "object-backed pair statistical aggregate did not execute "
              "through the complete engine-issued aggregate registry");
    }
    auto repeated_pair_argument = parser.RunPipeline(
        "SELECT CORR(integer_value, integer_value) FROM "
        "qow_packet7.qow_packet7_relation;",
        true);
    if (!repeated_pair_argument.accepted) {
      PrintMessages(repeated_pair_argument.messages);
    }
    Require(repeated_pair_argument.accepted &&
                repeated_pair_argument.server_operation_id ==
                    "query.execute" &&
                repeated_pair_argument.server_cursor_uuid.empty() &&
                repeated_pair_argument.server_row_count == 1 &&
                repeated_pair_argument.server_result_payload.find(
                    "corr_value=1") != std::string::npos,
            "object-backed pair aggregate did not preserve a repeated "
            "source argument binding");

    static constexpr std::array<AggregateProof, 2>
        kApproximateAggregateProofs{{
            {"APPROX_COUNT_DISTINCT(text_value)",
             "approx_count_distinct_value=2"},
            {"APPROX_MEDIAN(integer_value)", "approx_median_value=2"},
        }};
    for (const auto& proof : kApproximateAggregateProofs) {
      const auto sql = "SELECT " + std::string(proof.function) + " FROM "
                       "qow_packet7.qow_packet7_relation;";
      auto aggregate = parser.RunPipeline(sql, true);
      if (!aggregate.accepted) PrintMessages(aggregate.messages);
      Require(aggregate.accepted &&
                  aggregate.server_operation_id == "query.execute" &&
                  aggregate.server_cursor_uuid.empty() &&
                  aggregate.server_row_count == 1 &&
                  aggregate.server_result_payload.find(
                      proof.expected_payload) != std::string::npos,
              "object-backed approximate aggregate did not execute through "
              "the complete engine-issued aggregate registry");
    }

    auto string_aggregate = parser.RunPipeline(
        "SELECT STRING_AGG(text_value, ',') FROM "
        "qow_packet7.qow_packet7_relation;",
        true);
    if (!string_aggregate.accepted) {
      PrintMessages(string_aggregate.messages);
    }
    Require(string_aggregate.accepted &&
                string_aggregate.server_operation_id == "query.execute" &&
                string_aggregate.server_cursor_uuid.empty() &&
                string_aggregate.server_row_count == 1 &&
                string_aggregate.server_result_payload.find(
                    "string_agg_value=alpha,beta,alpha") !=
                    std::string::npos,
            "object-backed STRING_AGG did not preserve its engine-typed "
            "text separator and persisted scan order");

    auto listagg = parser.RunPipeline(
        "SELECT LISTAGG(text_value, ',') WITHIN GROUP "
        "(ORDER BY integer_value) FROM qow_packet7.qow_packet7_relation;",
        true);
    if (!listagg.accepted) PrintMessages(listagg.messages);
    Require(listagg.accepted &&
                listagg.server_operation_id == "query.execute" &&
                listagg.server_cursor_uuid.empty() &&
                listagg.server_row_count == 1 &&
                listagg.server_result_payload.find(
                    "listagg_value=alpha,beta,alpha") != std::string::npos,
            "object-backed LISTAGG did not preserve its exact WITHIN GROUP "
            "ordering and engine-typed text separator");

    auto projected = parser.RunPipeline(
        "SELECT integer_value FROM qow_packet7.qow_packet7_relation;", true);
    if (!projected.accepted) PrintMessages(projected.messages);
    Require(projected.accepted &&
                projected.server_operation_id == "query.execute" &&
                projected.server_cursor_uuid.empty() &&
                projected.server_row_count == 3 &&
                projected.server_result_payload.find("integer_value") !=
                    std::string::npos &&
                projected.server_result_payload.find("auxiliary_value") ==
                    std::string::npos,
            "object-backed source projection did not publish only its bound "
            "persisted column");

    auto reordered_projection = parser.RunPipeline(
        "SELECT auxiliary_value, integer_value FROM "
        "qow_packet7.qow_packet7_relation LIMIT 1;",
        true);
    if (!reordered_projection.accepted) {
      PrintMessages(reordered_projection.messages);
    }
    const auto auxiliary_position =
        reordered_projection.server_result_payload.find("auxiliary_value");
    const auto integer_position =
        reordered_projection.server_result_payload.find("integer_value");
    Require(reordered_projection.accepted &&
                reordered_projection.server_operation_id == "query.execute" &&
                reordered_projection.server_cursor_uuid.empty() &&
                reordered_projection.server_row_count == 1 &&
                auxiliary_position != std::string::npos &&
                integer_position != std::string::npos &&
                auxiliary_position < integer_position,
            "object-backed reordered projection/LIMIT did not preserve its "
            "selected result order");

    auto projected_filter = parser.RunPipeline(
        "SELECT integer_value FROM qow_packet7.qow_packet7_relation WHERE "
        "integer_value >= 2 LIMIT 1;",
        true);
    if (!projected_filter.accepted) {
      PrintMessages(projected_filter.messages);
    }
    Require(projected_filter.accepted &&
                projected_filter.server_operation_id == "query.execute" &&
                projected_filter.server_cursor_uuid.empty() &&
                projected_filter.server_row_count == 1 &&
                projected_filter.server_result_payload.find(
                    "integer_value") != std::string::npos &&
                projected_filter.server_result_payload.find(
                    "auxiliary_value") == std::string::npos,
            "object-backed projection/WHERE/LIMIT did not preserve its bound "
            "source column");

    auto hidden_filter = parser.RunPipeline(
        "SELECT integer_value FROM qow_packet7.qow_packet7_relation WHERE "
        "auxiliary_value >= 102;",
        true);
    if (!hidden_filter.accepted) PrintMessages(hidden_filter.messages);
    Require(hidden_filter.accepted &&
                hidden_filter.server_operation_id == "query.execute" &&
                hidden_filter.server_cursor_uuid.empty() &&
                hidden_filter.server_row_count == 2 &&
                hidden_filter.server_result_payload.find("integer_value") !=
                    std::string::npos &&
                hidden_filter.server_result_payload.find("auxiliary_value") ==
                    std::string::npos,
            "object-backed hidden predicate column did not filter before its "
            "canonical projection");

    auto hidden_filter_limit = parser.RunPipeline(
        "SELECT integer_value FROM qow_packet7.qow_packet7_relation WHERE "
        "auxiliary_value >= 102 LIMIT 1;",
        true);
    if (!hidden_filter_limit.accepted) {
      PrintMessages(hidden_filter_limit.messages);
    }
    Require(hidden_filter_limit.accepted &&
                hidden_filter_limit.server_operation_id == "query.execute" &&
                hidden_filter_limit.server_cursor_uuid.empty() &&
                hidden_filter_limit.server_row_count == 1 &&
                hidden_filter_limit.server_result_payload.find(
                    "integer_value") != std::string::npos &&
                hidden_filter_limit.server_result_payload.find(
                    "auxiliary_value") == std::string::npos,
            "object-backed hidden predicate/project/LIMIT chain leaked its "
            "dependency column");

    auto ordered_limit = parser.RunPipeline(
        "SELECT * FROM qow_packet7.qow_packet7_relation ORDER BY "
        "integer_value DESC NULLS LAST LIMIT 1;",
        true);
    if (!ordered_limit.accepted) PrintMessages(ordered_limit.messages);
    Require(ordered_limit.accepted &&
                ordered_limit.server_operation_id == "query.execute" &&
                ordered_limit.server_cursor_uuid.empty() &&
                ordered_limit.server_row_count == 1 &&
                ordered_limit.server_result_payload.find("integer_value=3") !=
                    std::string::npos,
            "object-backed ORDER BY/LIMIT did not complete the canonical live "
            "route");

    auto hidden_order = parser.RunPipeline(
        "SELECT integer_value FROM qow_packet7.qow_packet7_relation ORDER BY "
        "auxiliary_value DESC NULLS LAST LIMIT 1;",
        true);
    if (!hidden_order.accepted) PrintMessages(hidden_order.messages);
    Require(hidden_order.accepted &&
                hidden_order.server_operation_id == "query.execute" &&
                hidden_order.server_cursor_uuid.empty() &&
                hidden_order.server_row_count == 1 &&
                hidden_order.server_result_payload.find("integer_value") !=
                    std::string::npos &&
                hidden_order.server_result_payload.find("integer_value=3") !=
                    std::string::npos &&
                hidden_order.server_result_payload.find("auxiliary_value") ==
                    std::string::npos,
            "object-backed hidden ORDER BY key leaked through its canonical "
            "projection");

    auto nulls_first = parser.RunPipeline(
        "SELECT integer_value FROM qow_packet7.qow_packet7_relation ORDER BY "
        "nullable_order_value ASC NULLS FIRST LIMIT 1;",
        true);
    if (!nulls_first.accepted) PrintMessages(nulls_first.messages);
    Require(nulls_first.accepted && nulls_first.server_row_count == 1 &&
                nulls_first.server_result_payload.find("integer_value=2") !=
                    std::string::npos &&
                nulls_first.server_result_payload.find(
                    "nullable_order_value") == std::string::npos,
            "object-backed NULLS FIRST ordering did not select the null-key "
            "row without leaking the hidden key");

    auto nulls_last = parser.RunPipeline(
        "SELECT integer_value FROM qow_packet7.qow_packet7_relation ORDER BY "
        "nullable_order_value ASC NULLS LAST LIMIT 1;",
        true);
    if (!nulls_last.accepted) PrintMessages(nulls_last.messages);
    Require(nulls_last.accepted && nulls_last.server_row_count == 1 &&
                nulls_last.server_result_payload.find("integer_value=3") !=
                    std::string::npos &&
                nulls_last.server_result_payload.find(
                    "nullable_order_value") == std::string::npos,
            "object-backed NULLS LAST ordering did not select the lowest "
            "non-null hidden key");

    auto filtered_order = parser.RunPipeline(
        "SELECT integer_value FROM qow_packet7.qow_packet7_relation WHERE "
        "auxiliary_value >= 101 ORDER BY auxiliary_value DESC NULLS LAST, "
        "integer_value ASC NULLS FIRST LIMIT 2;",
        true);
    if (!filtered_order.accepted) PrintMessages(filtered_order.messages);
    const auto ordered_three =
        filtered_order.server_result_payload.find("integer_value=3");
    const auto ordered_two =
        filtered_order.server_result_payload.find("integer_value=2");
    Require(filtered_order.accepted &&
                filtered_order.server_operation_id == "query.execute" &&
                filtered_order.server_cursor_uuid.empty() &&
                filtered_order.server_row_count == 2 &&
                ordered_three != std::string::npos &&
                ordered_two != std::string::npos && ordered_three < ordered_two &&
                filtered_order.server_result_payload.find("integer_value") !=
                    std::string::npos &&
                filtered_order.server_result_payload.find("auxiliary_value") ==
                    std::string::npos,
            "object-backed WHERE/ORDER BY/hidden PROJECT/LIMIT chain did not "
            "complete the canonical live route");

    auto missing_order = parser.RunPipeline(
        "SELECT integer_value FROM qow_packet7.qow_packet7_relation ORDER BY "
        "missing_value;",
        true);
    Require(!missing_order.accepted && missing_order.server_operation_id.empty(),
            "object-backed ORDER BY admitted an unresolved source column");

    auto missing_projection = parser.RunPipeline(
        "SELECT missing_value FROM qow_packet7.qow_packet7_relation;", true);
    Require(!missing_projection.accepted &&
                missing_projection.server_operation_id.empty(),
            "object-backed projection admitted an unresolved source column");

    auto duplicate_projection = parser.RunPipeline(
        "SELECT integer_value, integer_value FROM "
        "qow_packet7.qow_packet7_relation;",
        true);
    Require(!duplicate_projection.accepted &&
                duplicate_projection.server_operation_id.empty(),
            "object-backed projection admitted a duplicate source column");

    struct FilterCase {
      std::string_view predicate;
      std::size_t expected_rows;
    };
    constexpr std::array<FilterCase, 7> kFilterCases = {{
        {"= 2", 1}, {"<> 2", 2}, {"!= 2", 2}, {"< 2", 1},
        {"<= 2", 2}, {"> 2", 1},  {">= 2", 2},
    }};
    for (const auto& filter_case : kFilterCases) {
      auto filtered = parser.RunPipeline(
          "SELECT * FROM qow_packet7.qow_packet7_relation WHERE "
          "integer_value " +
              std::string(filter_case.predicate) + ";",
          true);
      if (!filtered.accepted) PrintMessages(filtered.messages);
      Require(filtered.accepted &&
                  filtered.server_operation_id == "query.execute" &&
                  filtered.server_cursor_uuid.empty() &&
                  filtered.server_row_count == filter_case.expected_rows,
              "object-backed native SELECT WHERE comparison did not complete "
              "the full live route");
    }

    auto object_backed_filter_limit = parser.RunPipeline(
        "SELECT * FROM qow_packet7.qow_packet7_relation WHERE integer_value "
        ">= 2 LIMIT 1;",
        true);
    if (!object_backed_filter_limit.accepted) {
      PrintMessages(object_backed_filter_limit.messages);
    }
    Require(object_backed_filter_limit.accepted &&
                object_backed_filter_limit.server_operation_id ==
                    "query.execute" &&
                object_backed_filter_limit.server_cursor_uuid.empty() &&
                object_backed_filter_limit.server_row_count == 1,
            "object-backed native SELECT WHERE/LIMIT did not complete the "
            "full live route");

    auto object_backed_limit = parser.RunPipeline(
        "SELECT * FROM qow_packet7.qow_packet7_relation LIMIT 1;", true);
    if (!object_backed_limit.accepted) {
      PrintMessages(object_backed_limit.messages);
    }
    Require(object_backed_limit.accepted &&
                object_backed_limit.server_operation_id == "query.execute" &&
                object_backed_limit.server_cursor_uuid.empty() &&
                object_backed_limit.server_row_count == 1,
            "object-backed native SELECT with LIMIT did not complete the full "
            "live route");

    auto object_backed_limit_offset = parser.RunPipeline(
        "SELECT * FROM qow_packet7.qow_packet7_relation LIMIT 1 OFFSET 1;",
        true);
    if (!object_backed_limit_offset.accepted) {
      PrintMessages(object_backed_limit_offset.messages);
    }
    Require(object_backed_limit_offset.accepted &&
                object_backed_limit_offset.server_operation_id ==
                    "query.execute" &&
                object_backed_limit_offset.server_cursor_uuid.empty() &&
                object_backed_limit_offset.server_row_count == 1,
            "object-backed native SELECT with LIMIT/OFFSET did not complete "
            "the full live route");

    auto cursor = parser.RunPipeline(kSourceFreeNativeSelect, true, true);
    if (!cursor.accepted) PrintMessages(cursor.messages);
    Require(cursor.accepted && !cursor.server_cursor_uuid.empty(),
            "canonical cursor execution did not retain its live receipt");
    const auto fetched = parser.FetchCursorOnRoute(cursor.server_cursor_uuid);
    Require(fetched.accepted && fetched.end_of_cursor,
            "canonical cursor fetch did not reach end-of-stream cleanup");
    Require(!parser.CloseCursorOnRoute(cursor.server_cursor_uuid).accepted,
            "end-of-stream cursor allowed a second cleanup");

    auto cancelled = parser.RunPipeline(kSourceFreeNativeSelect, true, true);
    if (!cancelled.accepted) PrintMessages(cancelled.messages);
    Require(cancelled.accepted && !cancelled.server_cursor_uuid.empty(),
            "canonical cancellation cursor did not open");
    Require(parser.CancelCursorOnRoute(cancelled.server_cursor_uuid).accepted,
            "canonical cursor cancellation failed");
    Require(!parser.CancelCursorOnRoute(cancelled.server_cursor_uuid).accepted,
            "canonical cursor allowed double cancellation cleanup");
  }

  server::RequestParserServerStop();
  endpoint.join();
  if (!endpoint_result.ok()) {
    for (const auto& diagnostic : endpoint_result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    }
  }
  Require(endpoint_result.ok() && endpoint_result.exit_code == 0,
          "parser-server endpoint did not stop cleanly");
}

sbps::Frame AcquireFrame(
    const std::array<std::uint8_t, 16>& session_uuid,
    std::uint64_t local_transaction_id,
    const std::array<std::uint8_t, 16>& transaction_uuid) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(
      sbps::MessageType::kAcquireStatementContextRequest);
  frame.header.payload_schema_id =
      sbps::kSchemaAcquireStatementContextRequestV1;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session_uuid;
  frame.header.session_uuid = session_uuid;
  ipc::ParserSessionContext parser_session;
  parser_session.session_uuid = server::UuidBytesToText(session_uuid);
  parser_session.connection_uuid = parser_session.session_uuid;
  parser_session.authenticated = true;
  ipc::ParserTransactionSelector parser_transaction;
  parser_transaction.local_transaction_id = local_transaction_id;
  parser_transaction.transaction_uuid =
      uuid::UuidToString(platform::Uuid{transaction_uuid});
  frame.payload =
      ipc::EncodeAcquireStatementContextRequestPayloadV1ForTest(
          parser_session, parser_transaction);
  return frame;
}

void VerifyServerOwnedReceiptAndBoundedParserProjection(
    const Fixture& fixture,
    const api::EngineRequestContext& transaction) {
  server::ServerSessionRegistry registry;
  server::HostedEngineState engine_state;
  const auto session_uuid = fixture.session_uuid.value.bytes;
  const auto transaction_uuid =
      uuid::ParseUuid(transaction.transaction_uuid.canonical);
  Require(transaction_uuid.ok(),
          "server statement-context transaction UUID parse failed");

  server::ServerTransactionState server_transaction;
  server_transaction.local_transaction_id = transaction.local_transaction_id;
  server_transaction.transaction_uuid = transaction.transaction_uuid.canonical;
  server_transaction.snapshot_visible_through_local_transaction_id =
      transaction.snapshot_visible_through_local_transaction_id;
  server_transaction.isolation_level = "read_committed";

  server::ServerSessionRecord session;
  session.session_uuid = session_uuid;
  session.connection_uuid = session_uuid;
  session.server_channel_uuid = sbps::MakeUuidV7Bytes();
  session.channel_state = server::ServerChannelState::kReady;
  session.session_binding_present = true;
  session.transaction_routing_v2_negotiated = true;
  session.database_path = fixture.database_path.string();
  session.database_uuid = fixture.database_uuid;
  session.effective_user_uuid = fixture.principal_uuid.value.bytes;
  session.principal_uuid = session.effective_user_uuid;
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.catalog_generation = 1;
  session.security_epoch = 1;
  session.policy_generation = 1;
  session.resource_epoch = fixture.resource_epoch;
  session.name_resolution_epoch = 1;
  session.local_transaction_id = server_transaction.local_transaction_id;
  session.default_local_transaction_id =
      server_transaction.local_transaction_id;
  session.transaction_uuid = server_transaction.transaction_uuid;
  session.snapshot_visible_through_local_transaction_id =
      server_transaction.snapshot_visible_through_local_transaction_id;
  session.transactions_by_local_id.emplace(
      server_transaction.local_transaction_id, server_transaction);
  registry.channel_state = server::ServerChannelState::kReady;
  registry.physical_channel_by_connection_uuid[
      server::UuidBytesToText(session_uuid)] = session.server_channel_uuid;
  registry.sessions_by_uuid.emplace(server::UuidBytesToText(session_uuid),
                                    std::move(session));

  engine_state.engine_context_active = true;
  server::HostedDatabaseSnapshot database;
  database.state = server::HostedDatabaseState::kOpen;
  database.database_created = true;
  database.database_open = true;
  database.database_path = fixture.database_path.string();
  database.database_uuid = fixture.database_uuid;
  engine_state.databases.push_back(std::move(database));

  const auto acquired = server::HandleAcquireStatementContext(
      &registry,
      engine_state,
      AcquireFrame(session_uuid,
                   server_transaction.local_transaction_id,
                   transaction_uuid.value.bytes));
  if (!acquired.accepted) {
    for (const auto& diagnostic : acquired.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << field.key << '=' << field.value << '\n';
      }
    }
  }
  ipc::ParserStatementContext parser_context;
  Require(acquired.accepted &&
              acquired.response_message_type == static_cast<std::uint16_t>(
                  sbps::MessageType::kAcquireStatementContextResult) &&
              acquired.response_schema_id ==
                  sbps::kSchemaAcquireStatementContextResultV1 &&
              acquired.payload.size() == 115 &&
              ipc::DecodeAcquireStatementContextResultPayloadV1ForTest(
                  acquired.payload, &parser_context) &&
              parser_context.transaction.local_transaction_id ==
                  server_transaction.local_transaction_id &&
              parser_context.transaction.transaction_uuid ==
                  server_transaction.transaction_uuid &&
              parser_context.snapshot_visible_through_local_transaction_id ==
                  0 &&
              registry.statement_contexts_by_statement_uuid.size() == 1,
          "server did not return the exact bounded statement-context projection");

  const auto statement =
      registry.statement_contexts_by_statement_uuid.find(
          parser_context.statement_uuid);
  Require(statement != registry.statement_contexts_by_statement_uuid.end() &&
              statement->second.receipt &&
              statement->second.view.receipt_uuid !=
                  parser_context.statement_uuid &&
              statement->second.view.receipt_uuid !=
                  parser_context.statement_snapshot_uuid,
          "opaque statement receipt crossed or escaped server ownership");
  const auto private_receipt = statement->second.receipt;

  auto swapped_uuid = NewTypedUuid(platform::UuidKind::transaction,
                                   fixture.salt + 91).value.bytes;
  const auto swapped = server::HandleAcquireStatementContext(
      &registry,
      engine_state,
      AcquireFrame(session_uuid,
                   server_transaction.local_transaction_id,
                   swapped_uuid));
  Require(!swapped.accepted && !swapped.diagnostics.empty() &&
              swapped.diagnostics.front().code ==
                  "PARSER_SERVER_IPC.STATEMENT_CONTEXT_TRANSACTION_INVALID" &&
              registry.statement_contexts_by_statement_uuid.size() == 1,
          "server admitted a swapped statement-context transaction UUID");

  auto cross_session = AcquireFrame(session_uuid,
                                    server_transaction.local_transaction_id,
                                    transaction_uuid.value.bytes);
  cross_session.payload[2] ^= 0x01u;
  const auto refused_cross_session = server::HandleAcquireStatementContext(
      &registry, engine_state, cross_session);
  Require(!refused_cross_session.accepted &&
              !refused_cross_session.diagnostics.empty() &&
              refused_cross_session.diagnostics.front().code ==
                  "PARSER_SERVER_IPC.STATEMENT_CONTEXT_SELECTOR_INVALID" &&
              registry.statement_contexts_by_statement_uuid.size() == 1,
          "server admitted a statement-context session mismatch");

  server::CloseServerPublicAbiSessionForSession(
      &registry, session_uuid, "statement_context_test_cleanup");
  Require(registry.statement_contexts_by_statement_uuid.empty() &&
              registry.public_abi_sessions_by_session_uuid.empty() &&
              bridge::ReleaseStatementContextReceipt(private_receipt) ==
                  SB_ENGINE_STATUS_ALREADY_RELEASED,
          "server cleanup did not release its private receipt exactly once");
}

}  // namespace

int main() {
  auto fixture = CreateFixture();
  auto transaction = BeginTransaction(fixture);
  Require(transaction.snapshot_visible_through_local_transaction_id == 0,
          "fresh inventory did not preserve zero as a valid high-watermark");

  PublicSession owner(fixture, fixture.session_uuid);
  const auto other_session_uuid =
      NewTypedUuid(platform::UuidKind::session, fixture.salt + 7);
  PublicSession other(fixture, other_session_uuid);

  bridge::StatementContextReceiptView refused_view;
  const auto cross_session =
      Acquire(other.get(), transaction, &refused_view,
              SB_ENGINE_STATUS_SECURITY_DENIED);
  Require(!cross_session,
          "cross-session statement-context acquisition returned a receipt");

  auto caller_authority = transaction;
  caller_authority.statement_uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 8);
  const auto caller_forgery =
      Acquire(owner.get(), caller_authority, &refused_view,
              SB_ENGINE_STATUS_CONFLICT);
  Require(!caller_forgery,
          "caller-authored statement identity returned a receipt");

  bridge::StatementContextReceiptView first_view;
  const auto first = Acquire(owner.get(), transaction, &first_view);
  Require(first && first_view.snapshot_complete &&
              first_view.inventory_authoritative,
          "live statement-context receipt is incomplete");
  Require(first_view.owning_transaction_uuid ==
                  transaction.transaction_uuid.canonical &&
              first_view.owning_local_transaction_id ==
                  transaction.local_transaction_id,
          "live statement-context receipt transaction identity drifted");
  Require(first_view.visible_committed_high_watermark == 0,
          "live statement-context receipt rejected zero high-watermark");
  Require(first_view.optimizer_memory_budget_bytes ==
                  transaction.optimizer_memory_budget_bytes &&
              first_view.optimizer_maximum_search_steps ==
                  transaction.optimizer_maximum_search_steps,
          "live statement-context resource projection drifted");
  AssertDistinctReceiptIdentities(first_view);

  auto resolve_context = transaction;
  resolve_context.statement_uuid.canonical = first_view.statement_uuid;
  resolve_context.statement_snapshot_uuid.canonical =
      first_view.statement_snapshot_uuid;
  resolve_context.snapshot_visible_through_local_transaction_id =
      first_view.visible_committed_high_watermark;
  api::EngineResolveStatementSnapshotRequest resolve;
  resolve.context = resolve_context;
  const auto resolved = api::EngineResolveStatementSnapshot(resolve);
  RequireEngineOk(resolved, "live receipt statement snapshot did not resolve");
  Require(resolved.snapshot_vector.active_excluded_local_transaction_ids ==
                  first_view.active_excluded_local_transaction_ids &&
              resolved.snapshot_vector.in_doubt_excluded_local_transaction_ids ==
                  first_view.in_doubt_excluded_local_transaction_ids,
          "live receipt exclusion vectors drifted from MGA authority");

  Require(bridge::ReleaseStatementContextReceipt(first) ==
              SB_ENGINE_STATUS_OK,
          "live statement-context receipt release failed");
  Require(bridge::ReleaseStatementContextReceipt(first) ==
              SB_ENGINE_STATUS_ALREADY_RELEASED,
          "live statement-context receipt did not enforce exactly-once release");
  Require(!api::EngineResolveStatementSnapshot(resolve).ok,
          "released statement snapshot remained resolvable");

  bridge::StatementContextReceiptView cleanup_view;
  const auto cleanup_receipt = Acquire(owner.get(), transaction, &cleanup_view);
  Require(static_cast<bool>(cleanup_receipt),
          "session-cleanup receipt acquisition failed");
  owner.End();
  Require(bridge::ReleaseStatementContextReceipt(cleanup_receipt) ==
              SB_ENGINE_STATUS_ALREADY_RELEASED,
          "session end did not revoke its live statement-context receipts");

  VerifyServerOwnedReceiptAndBoundedParserProjection(fixture, transaction);
  Rollback(transaction);
  auto full_route_fixture = CreateFixture(true);
  CreateObjectBackedRelation(&full_route_fixture);
  VerifyFullParserServerRoute(full_route_fixture);
  std::cout << "qow_live_server_statement_context=passed\n";
  return EXIT_SUCCESS;
}
