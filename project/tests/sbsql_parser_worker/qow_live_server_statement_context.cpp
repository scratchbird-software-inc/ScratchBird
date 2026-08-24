// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "datatype_catalog_manifest.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "ipc_server.hpp"
#include "lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "nosql/spatial_api.hpp"
#include "parser_server_client.hpp"
#include "parsers/sbsql_worker/cache/sblr_template_cache.hpp"
#include "parsers/sbsql_worker/metrics/parser_metrics.hpp"
#include "parsers/sbsql_worker/wire/sbsql_test_wire.hpp"
#include "resource_seed_pack.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include "session_registry.hpp"
#include "sbps.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace bridge = scratchbird::server_engine_bridge;
namespace db = scratchbird::storage::database;
namespace dt = scratchbird::core::datatypes;
namespace ipc = scratchbird::parser::ipc;
namespace nosql = scratchbird::engine::internal_api::nosql;
namespace platform = scratchbird::core::platform;
namespace resources = scratchbird::core::resources;
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
  platform::TypedUuid join_relation_uuid;
  platform::TypedUuid spatial_relation_uuid;
  platform::TypedUuid columnar_relation_uuid;
  platform::TypedUuid spatial_crs_uuid;
  std::uint64_t resource_epoch = 1;
  std::string utf8_charset_uuid;
  std::string utf8_default_collation_uuid;
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
        join_relation_uuid(other.join_relation_uuid),
        spatial_relation_uuid(other.spatial_relation_uuid),
        columnar_relation_uuid(other.columnar_relation_uuid),
        spatial_crs_uuid(other.spatial_crs_uuid),
        resource_epoch(other.resource_epoch),
        utf8_charset_uuid(std::move(other.utf8_charset_uuid)),
        utf8_default_collation_uuid(
            std::move(other.utf8_default_collation_uuid)),
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
  fixture.join_relation_uuid =
      NewTypedUuid(platform::UuidKind::object, fixture.salt + 8);
  fixture.spatial_relation_uuid =
      NewTypedUuid(platform::UuidKind::object, fixture.salt + 9);
  fixture.columnar_relation_uuid =
      NewTypedUuid(platform::UuidKind::object, fixture.salt + 10);
  fixture.spatial_crs_uuid =
      NewTypedUuid(platform::UuidKind::object, fixture.salt + 11);
  fixture.resource_epoch =
      created.state.resource_seed_catalog.resource_epoch == 0
          ? 1
          : created.state.resource_seed_catalog.resource_epoch;
  if (const auto* utf8 = resources::FindResourceSeedCharset(
          created.state.resource_seed_catalog, "UTF8");
      utf8 != nullptr) {
    fixture.utf8_charset_uuid = utf8->resource_uuid;
    fixture.utf8_default_collation_uuid = utf8->default_collation_uuid;
  }
  if (credentialed_full_route) {
    Require(!fixture.utf8_charset_uuid.empty() &&
                !fixture.utf8_default_collation_uuid.empty(),
            "statement-context UTF8 resource authority is unavailable");
  }
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

std::string CoreTypeUuid(const std::string_view stable_name) {
  if (stable_name == "int64") {
    const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
        "019d0000-0000-7000-8000-00000000d701", 1, 1,
        "019d0000-0000-7000-8000-00000000d711", 1);
    Require(identity.ok && !identity.row.type_uuid.empty(),
            "int64 type-codec identity is unavailable");
    return identity.row.type_uuid;
  }
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Require(manifest.ok(), "core datatype catalog manifest is unavailable");
  const auto found = std::ranges::find_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  Require(found != manifest.manifest.descriptor_rows.end() &&
              found->descriptor_uuid.valid(),
          "required core datatype descriptor is unavailable");
  return uuid::UuidToString(found->descriptor_uuid.value);
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
  column.nullable = false;
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
  text_column.descriptor.encoded_descriptor =
      "type=text;character_length=256;charset_uuid=" +
      fixture->utf8_charset_uuid +
      ";collation_uuid=" + fixture->utf8_default_collation_uuid;
  text_column.nullable = true;
  table.table_columns.push_back(std::move(text_column));
  RequireEngineOk(api::EngineCreateTable(table),
                  "object-backed fixture table create failed");

  api::EngineCreateTableRequest join_table;
  join_table.context = context;
  join_table.target_schema = schema.target_object;
  join_table.requested_table_uuid.canonical =
      uuid::UuidToString(fixture->join_relation_uuid.value);
  join_table.table_names.push_back(PrimaryName("qow_packet7_join_relation"));
  api::EngineColumnDefinition join_column;
  join_column.ordinal = 0;
  join_column.names.push_back(PrimaryName("join_value"));
  join_column.descriptor.descriptor_kind = "scalar";
  join_column.descriptor.canonical_type_name = "integer";
  join_column.descriptor.encoded_descriptor = "type=integer";
  join_column.nullable = false;
  join_table.table_columns.push_back(std::move(join_column));
  api::EngineColumnDefinition join_auxiliary_column;
  join_auxiliary_column.ordinal = 1;
  join_auxiliary_column.names.push_back(
      PrimaryName("join_auxiliary_value"));
  join_auxiliary_column.descriptor.descriptor_kind = "scalar";
  join_auxiliary_column.descriptor.canonical_type_name = "integer";
  join_auxiliary_column.descriptor.encoded_descriptor = "type=integer";
  join_auxiliary_column.nullable = false;
  join_table.table_columns.push_back(std::move(join_auxiliary_column));
  api::EngineColumnDefinition join_limit_column;
  join_limit_column.ordinal = 2;
  join_limit_column.names.push_back(
      PrimaryName("join_limit_value"));
  join_limit_column.descriptor.descriptor_kind = "scalar";
  join_limit_column.descriptor.canonical_type_name = "int64";
  join_limit_column.descriptor.encoded_descriptor = "type=int64";
  join_limit_column.nullable = false;
  join_table.table_columns.push_back(std::move(join_limit_column));
  RequireEngineOk(api::EngineCreateTable(join_table),
                  "object-backed join fixture table create failed");

  const auto uuid_type_uuid = CoreTypeUuid("uuid");
  const auto geometry_type_uuid = CoreTypeUuid("geometry");
  const auto int64_type_uuid = CoreTypeUuid("int64");
  const auto character_type_uuid = CoreTypeUuid("character");
  const auto crs_uuid = uuid::UuidToString(fixture->spatial_crs_uuid.value);
  const auto make_column = [](const std::uint32_t ordinal,
                              std::string name,
                              std::string canonical_type,
                              std::string encoded_descriptor,
                              const bool nullable) {
    api::EngineColumnDefinition definition;
    definition.ordinal = ordinal;
    definition.names.push_back(PrimaryName(std::move(name)));
    definition.descriptor.descriptor_kind = "scalar";
    definition.descriptor.canonical_type_name = std::move(canonical_type);
    definition.descriptor.encoded_descriptor =
        std::move(encoded_descriptor);
    definition.nullable = nullable;
    return definition;
  };

  api::EngineCreateTableRequest spatial_table;
  spatial_table.context = context;
  spatial_table.target_schema = schema.target_object;
  spatial_table.requested_table_uuid.canonical =
      uuid::UuidToString(fixture->spatial_relation_uuid.value);
  spatial_table.table_names.push_back(
      PrimaryName("qow_packet7_spatial_relation"));
  spatial_table.table_columns.push_back(make_column(
      0, "row_uuid", "uuid",
      "canonical=uuid;type_uuid=" + uuid_type_uuid + ";nullable=false",
      false));
  spatial_table.table_columns.push_back(make_column(
      1, "spatial_value", "geometry",
      "canonical=geometry;type_uuid=" + geometry_type_uuid +
          ";nullable=false;subtype=POINT;axes=x,y;crs_uuid=" + crs_uuid +
          ";crs_generation=1",
      false));
  spatial_table.table_columns.push_back(make_column(
      2, "crs_uuid", "uuid",
      "canonical=uuid;type_uuid=" + uuid_type_uuid + ";nullable=false",
      false));
  RequireEngineOk(api::EngineCreateTable(spatial_table),
                  "object-backed spatial fixture table create failed");

  api::EngineCreateTableRequest columnar_table;
  columnar_table.context = context;
  columnar_table.target_schema = schema.target_object;
  columnar_table.requested_table_uuid.canonical =
      uuid::UuidToString(fixture->columnar_relation_uuid.value);
  columnar_table.table_names.push_back(
      PrimaryName("qow_packet7_columnar_relation"));
  columnar_table.table_columns.push_back(make_column(
      0, "row_uuid", "uuid",
      "canonical=uuid;type_uuid=" + uuid_type_uuid + ";nullable=false",
      false));
  columnar_table.table_columns.push_back(make_column(
      1, "join_key", "int64",
      "canonical=int64;type_uuid=" + int64_type_uuid + ";nullable=false",
      false));
  columnar_table.table_columns.push_back(make_column(
      2, "payload", "character",
      "canonical=character;type_uuid=" + character_type_uuid +
          ";nullable=false",
      false));
  RequireEngineOk(api::EngineCreateTable(columnar_table),
                  "object-backed columnar fixture table create failed");

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

  api::EngineInsertRowsRequest join_insert;
  join_insert.context = context;
  join_insert.target_table.uuid.canonical =
      uuid::UuidToString(fixture->join_relation_uuid.value);
  join_insert.target_table.object_kind = "table";
  for (const std::int64_t value : {2, 3, 4}) {
    api::EngineTypedValue typed;
    typed.descriptor.descriptor_kind = "scalar";
    typed.descriptor.canonical_type_name = "integer";
    typed.descriptor.encoded_descriptor = "type=integer";
    typed.encoded_value = std::to_string(value);
    api::EngineTypedValue auxiliary_typed;
    auxiliary_typed.descriptor.descriptor_kind = "scalar";
    auxiliary_typed.descriptor.canonical_type_name = "integer";
    auxiliary_typed.descriptor.encoded_descriptor = "type=integer";
    auxiliary_typed.encoded_value =
        value == 2 ? "999" : (value == 3 ? "103" : "101");
    api::EngineTypedValue limit_typed;
    limit_typed.descriptor.descriptor_kind = "scalar";
    limit_typed.descriptor.canonical_type_name = "int64";
    limit_typed.descriptor.encoded_descriptor = "type=int64";
    limit_typed.encoded_value = std::to_string(value);
    api::EngineRowValue row;
    row.fields.push_back({"join_value", std::move(typed)});
    row.fields.push_back(
        {"join_auxiliary_value", std::move(auxiliary_typed)});
    row.fields.push_back(
        {"join_limit_value", std::move(limit_typed)});
    join_insert.input_rows.push_back(std::move(row));
  }
  join_insert.estimated_row_count = join_insert.input_rows.size();
  const auto join_inserted = api::EngineInsertRows(join_insert);
  RequireEngineOk(join_inserted,
                  "object-backed join fixture row insert failed");
  Require(join_inserted.inserted_count == 3,
          "object-backed join fixture did not insert three rows");

  const auto make_typed_value = [](std::string canonical_type,
                                   std::string encoded_descriptor,
                                   std::string encoded_value) {
    api::EngineTypedValue value;
    value.descriptor.descriptor_kind = "scalar";
    value.descriptor.canonical_type_name = std::move(canonical_type);
    value.descriptor.encoded_descriptor = std::move(encoded_descriptor);
    value.encoded_value = std::move(encoded_value);
    return value;
  };
  const auto uuid_descriptor =
      "canonical=uuid;type_uuid=" + uuid_type_uuid + ";nullable=false";
  const auto geometry_descriptor =
      "canonical=geometry;type_uuid=" + geometry_type_uuid +
      ";nullable=false;subtype=POINT;axes=x,y;crs_uuid=" + crs_uuid +
      ";crs_generation=1";
  const auto int64_descriptor =
      "canonical=int64;type_uuid=" + int64_type_uuid + ";nullable=false";
  const auto character_descriptor =
      "canonical=character;type_uuid=" + character_type_uuid +
      ";nullable=false";
  const auto shared_row_uuid =
      NewUuidText(platform::UuidKind::row, fixture->salt + 100);
  const auto spatial_only_row_uuid =
      NewUuidText(platform::UuidKind::row, fixture->salt + 101);
  const auto columnar_only_row_uuid =
      NewUuidText(platform::UuidKind::row, fixture->salt + 102);

  api::EngineInsertRowsRequest spatial_insert;
  spatial_insert.context = context;
  spatial_insert.target_table.uuid.canonical =
      uuid::UuidToString(fixture->spatial_relation_uuid.value);
  spatial_insert.target_table.object_kind = "table";
  const std::array<std::pair<std::string, nosql::SpatialPoint2dV1>, 2>
      spatial_seeds{{{shared_row_uuid, {0, 0}},
                     {spatial_only_row_uuid, {3, 4}}}};
  for (const auto& [row_uuid, point] : spatial_seeds) {
    const auto encoded = nosql::EncodeSpatialPoint2dV1(point);
    api::EngineRowValue row;
    row.requested_row_uuid.canonical = row_uuid;
    row.fields.push_back(
        {"row_uuid", make_typed_value("uuid", uuid_descriptor, row_uuid)});
    row.fields.push_back(
        {"spatial_value",
         make_typed_value(
             "geometry", geometry_descriptor,
             std::string(reinterpret_cast<const char*>(encoded.data()),
                         encoded.size()))});
    row.fields.push_back(
        {"crs_uuid", make_typed_value("uuid", uuid_descriptor, crs_uuid)});
    spatial_insert.input_rows.push_back(std::move(row));
  }
  spatial_insert.estimated_row_count = spatial_insert.input_rows.size();
  const auto spatial_inserted = api::EngineInsertRows(spatial_insert);
  RequireEngineOk(spatial_inserted,
                  "object-backed spatial fixture row insert failed");
  Require(spatial_inserted.inserted_count == 2,
          "object-backed spatial fixture did not insert two rows");

  api::EngineInsertRowsRequest columnar_insert;
  columnar_insert.context = context;
  columnar_insert.target_table.uuid.canonical =
      uuid::UuidToString(fixture->columnar_relation_uuid.value);
  columnar_insert.target_table.object_kind = "table";
  const std::array<std::tuple<std::string, std::int64_t, std::string>, 2>
      columnar_seeds{{{shared_row_uuid, 7, "matched"},
                      {columnar_only_row_uuid, 9, "columnar-only"}}};
  for (const auto& [row_uuid, join_key, payload] : columnar_seeds) {
    api::EngineRowValue row;
    row.requested_row_uuid.canonical = row_uuid;
    row.fields.push_back(
        {"row_uuid", make_typed_value("uuid", uuid_descriptor, row_uuid)});
    row.fields.push_back(
        {"join_key", make_typed_value("int64", int64_descriptor,
                                      std::to_string(join_key))});
    row.fields.push_back({"payload", make_typed_value(
                                         "character", character_descriptor,
                                         payload)});
    columnar_insert.input_rows.push_back(std::move(row));
  }
  columnar_insert.estimated_row_count = columnar_insert.input_rows.size();
  const auto columnar_inserted = api::EngineInsertRows(columnar_insert);
  RequireEngineOk(columnar_inserted,
                  "object-backed columnar fixture row insert failed");
  Require(columnar_inserted.inserted_count == 2,
          "object-backed columnar fixture did not insert two rows");

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

void VerifyFullParserServerRoute(const Fixture& fixture,
                                 const bool join_tail_proof_only,
                                 const bool table_function_proof_only = false,
                                 const bool match_recognize_proof_only = false) {
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

    if (!join_tail_proof_only && !table_function_proof_only &&
        !match_recognize_proof_only) {
      auto source_free = parser.RunPipeline(kSourceFreeNativeSelect, true);
      if (!source_free.accepted) PrintMessages(source_free.messages);
      Require(source_free.accepted &&
                  source_free.server_operation_id == "query.execute" &&
                  source_free.server_cursor_uuid.empty(),
              "source-free native SELECT did not complete the full live route");

      auto object_backed = parser.RunPipeline(
          "SELECT * FROM qow_packet7.qow_packet7_relation;", true);
      if (!object_backed.accepted) PrintMessages(object_backed.messages);
      Require(
          object_backed.accepted &&
              object_backed.server_operation_id == "query.execute" &&
              object_backed.server_cursor_uuid.empty() &&
              object_backed.server_row_count == 3,
          "object-backed native SELECT did not complete the full live route");

      auto object_backed_cross_join = parser.RunPipeline(
          "SELECT * FROM qow_packet7.qow_packet7_relation CROSS JOIN "
          "qow_packet7.qow_packet7_join_relation;",
          true);
      if (!object_backed_cross_join.accepted) {
        PrintMessages(object_backed_cross_join.messages);
      }
      Require(
          object_backed_cross_join.accepted &&
              object_backed_cross_join.server_operation_id ==
                  "query.execute" &&
              object_backed_cross_join.server_cursor_uuid.empty() &&
              object_backed_cross_join.server_row_count == 9 &&
              object_backed_cross_join.server_result_payload.find(
                  "join_value") != std::string::npos,
          "object-backed native CROSS JOIN did not complete the canonical "
          "two-heap-scan route");
    }

    const auto text_parameter = [](const std::string_view text) {
      sbsql::PreparedParameterWireValue value;
      value.encoding =
          sbsql::PreparedParameterPayloadEncoding::utf8_text;
      value.raw_bytes.assign(text.begin(), text.end());
      return value;
    };
    const auto run_direct_parameterized =
        [&](const std::string_view sql,
            const std::vector<sbsql::PreparedParameterWireValue>& values) {
          return parser.RunDirectParameterizedForWire(sql, values);
        };

    if (!join_tail_proof_only && !match_recognize_proof_only) {
      auto generate_series = run_direct_parameterized(
          "SELECT * FROM generate_series(?, ?, ?);",
          {text_parameter("1"), text_parameter("5"), text_parameter("2")});
      if (!generate_series.accepted) {
        PrintMessages(generate_series.messages);
      }
      Require(
          generate_series.accepted &&
              generate_series.server_operation_id == "query.execute" &&
              generate_series.server_cursor_uuid.empty() &&
              generate_series.server_row_count == 3 &&
              generate_series.server_result_payload.find(
                  "generate_series=1") != std::string::npos &&
              generate_series.server_result_payload.find(
                  "generate_series=5") != std::string::npos,
          "generate_series did not complete the independent SBSQL parser, "
          "bound SBLR, optimizer, physical source, and executor route");

      auto generate_series_default_step = run_direct_parameterized(
          "SELECT * FROM generate_series(?, ?);",
          {text_parameter("3"), text_parameter("5")});
      if (!generate_series_default_step.accepted) {
        PrintMessages(generate_series_default_step.messages);
      }
      Require(generate_series_default_step.accepted &&
                  generate_series_default_step.server_row_count == 3 &&
                  generate_series_default_step.server_result_payload.find(
                      "generate_series=3") != std::string::npos &&
                  generate_series_default_step.server_result_payload.find(
                      "generate_series=5") != std::string::npos,
              "generate_series default step did not preserve inclusive int64 semantics");

      auto generate_series_descending = run_direct_parameterized(
          "SELECT * FROM generate_series(?, ?, ?);",
          {text_parameter("5"), text_parameter("1"), text_parameter("-2")});
      if (!generate_series_descending.accepted) {
        PrintMessages(generate_series_descending.messages);
      }
      Require(generate_series_descending.accepted &&
                  generate_series_descending.server_row_count == 3 &&
                  generate_series_descending.server_result_payload.find(
                      "generate_series=5") != std::string::npos &&
                  generate_series_descending.server_result_payload.find(
                      "generate_series=1") != std::string::npos,
              "generate_series descending step did not preserve inclusive int64 semantics");

      auto generate_series_empty = run_direct_parameterized(
          "SELECT * FROM generate_series(?, ?, ?);",
          {text_parameter("1"), text_parameter("5"), text_parameter("-1")});
      Require(generate_series_empty.accepted &&
                  generate_series_empty.server_row_count == 0,
              "generate_series incompatible direction did not publish an empty rowset");

      auto generate_series_zero_step = run_direct_parameterized(
          "SELECT * FROM generate_series(?, ?, ?);",
          {text_parameter("1"), text_parameter("5"), text_parameter("0")});
      Require(!generate_series_zero_step.accepted,
              "generate_series admitted a zero step");

      auto generate_series_overflow = run_direct_parameterized(
          "SELECT * FROM generate_series(?, ?, ?);",
          {text_parameter("1"), text_parameter("10001"),
           text_parameter("1")});
      Require(!generate_series_overflow.accepted,
              "generate_series exceeded its bounded 10000-row profile");

      auto generate_series_literal =
          parser.RunPipeline("SELECT * FROM generate_series(1, 5, 2);", true);
      Require(!generate_series_literal.accepted,
              "generate_series admitted parser-authored literal arguments");
    }

    if (!join_tail_proof_only && !table_function_proof_only) {
      constexpr std::string_view kMatchRecognizeQuery =
          "SELECT * FROM generate_series(?, ?, ?) MATCH_RECOGNIZE ("
          "PARTITION BY generate_series ORDER BY generate_series ASC "
          "ALL ROWS PER MATCH AFTER MATCH SKIP PAST LAST ROW "
          "PATTERN (A+) DEFINE A AS TRUE);";
      auto match_recognize = run_direct_parameterized(
          kMatchRecognizeQuery,
          {text_parameter("5"), text_parameter("1"),
           text_parameter("-2")});
      if (!match_recognize.accepted) {
        PrintMessages(match_recognize.messages);
      }
      const auto one = match_recognize.server_result_payload.find(
          "generate_series=1");
      const auto three = match_recognize.server_result_payload.find(
          "generate_series=3");
      const auto five = match_recognize.server_result_payload.find(
          "generate_series=5");
      Require(
          match_recognize.accepted &&
              match_recognize.server_operation_id == "query.execute" &&
              match_recognize.server_cursor_uuid.empty() &&
              match_recognize.server_row_count == 3 &&
              one != std::string::npos && three != std::string::npos &&
              five != std::string::npos && one < three && three < five,
          "MATCH_RECOGNIZE did not complete parser, bound SBLR, optimizer, "
          "partition/order pattern execution, and ordered result publication");

      auto refused_one_row = run_direct_parameterized(
          "SELECT * FROM generate_series(?, ?, ?) MATCH_RECOGNIZE ("
          "PARTITION BY generate_series ORDER BY generate_series "
          "ONE ROW PER MATCH AFTER MATCH SKIP PAST LAST ROW "
          "PATTERN (A+) DEFINE A AS TRUE);",
          {text_parameter("1"), text_parameter("3"),
           text_parameter("1")});
      Require(!refused_one_row.accepted,
              "bounded MATCH_RECOGNIZE admitted an unsupported output mode");
    }

    if (!table_function_proof_only && !match_recognize_proof_only) {

    auto joined_literal_parameter_tail = run_direct_parameterized(
        "SELECT l.integer_value FROM "
        "qow_packet7.qow_packet7_relation AS l CROSS JOIN "
        "qow_packet7.qow_packet7_join_relation AS r WHERE "
        "r.join_limit_value >= 3 LIMIT ?;",
        {text_parameter("1")});
    if (!joined_literal_parameter_tail.accepted) {
      PrintMessages(joined_literal_parameter_tail.messages);
    }
    Require(
        joined_literal_parameter_tail.accepted &&
            joined_literal_parameter_tail.server_operation_id ==
                "query.execute" &&
            joined_literal_parameter_tail.server_cursor_uuid.empty() &&
            joined_literal_parameter_tail.server_row_count == 1 &&
            joined_literal_parameter_tail.server_result_payload.find(
                "integer_value") != std::string::npos &&
            joined_literal_parameter_tail.server_result_payload.find(
                "join_limit_value") == std::string::npos,
        "joined literal FILTER/PROJECT/parameter LIMIT did not cross the "
        "combined SBEL/SBPE/SBPV transport");

    auto joined_unprojected_literal_parameter_tail =
        run_direct_parameterized(
            "SELECT * FROM qow_packet7.qow_packet7_relation AS l CROSS "
            "JOIN qow_packet7.qow_packet7_join_relation AS r WHERE "
            "r.join_limit_value >= 3 LIMIT ?;",
            {text_parameter("1")});
    if (!joined_unprojected_literal_parameter_tail.accepted) {
      PrintMessages(joined_unprojected_literal_parameter_tail.messages);
    }
    Require(
        joined_unprojected_literal_parameter_tail.accepted &&
            joined_unprojected_literal_parameter_tail.server_operation_id ==
                "query.execute" &&
            joined_unprojected_literal_parameter_tail.server_cursor_uuid
                .empty() &&
            joined_unprojected_literal_parameter_tail.server_row_count == 1 &&
            joined_unprojected_literal_parameter_tail.server_result_payload
                    .find("join_limit_value") != std::string::npos,
        "joined literal FILTER/parameter LIMIT did not preserve its full "
        "immediate-input publication");

    auto three_way_literal_parameter_tail = run_direct_parameterized(
        "SELECT * FROM qow_packet7.qow_packet7_relation AS l CROSS JOIN "
        "qow_packet7.qow_packet7_join_relation AS r CROSS JOIN "
        "qow_packet7.qow_packet7_columnar_relation AS c WHERE "
        "r.join_limit_value >= 3 LIMIT ?;",
        {text_parameter("1")});
    if (!three_way_literal_parameter_tail.accepted) {
      PrintMessages(three_way_literal_parameter_tail.messages);
    }
    Require(
        three_way_literal_parameter_tail.accepted &&
            three_way_literal_parameter_tail.server_operation_id ==
                "query.execute" &&
            three_way_literal_parameter_tail.server_cursor_uuid.empty() &&
            three_way_literal_parameter_tail.server_row_count == 1 &&
            three_way_literal_parameter_tail.server_result_payload.find(
                "join_limit_value") != std::string::npos,
        "three-way literal FILTER/parameter LIMIT did not complete the "
        "bounded multi-source route");

    auto unsupported_literal_pair = parser.RunPipeline(
        "SELECT l.integer_value FROM "
        "qow_packet7.qow_packet7_relation AS l CROSS JOIN "
        "qow_packet7.qow_packet7_join_relation AS r WHERE "
        "r.join_limit_value >= 3 LIMIT 1;",
        true);
    Require(!unsupported_literal_pair.accepted,
            "joined two-literal FILTER/LIMIT exceeded the bounded mixed-tail "
            "profile");
    }

    if (!join_tail_proof_only && !table_function_proof_only &&
        !match_recognize_proof_only) {
    auto three_way_join_limit = parser.RunPipeline(
        "SELECT * FROM qow_packet7.qow_packet7_relation AS l CROSS JOIN "
        "qow_packet7.qow_packet7_join_relation AS r CROSS JOIN "
        "qow_packet7.qow_packet7_columnar_relation AS c LIMIT 1;",
        true);
    if (!three_way_join_limit.accepted) {
      PrintMessages(three_way_join_limit.messages);
    }
    Require(three_way_join_limit.accepted &&
                three_way_join_limit.server_operation_id == "query.execute" &&
                three_way_join_limit.server_cursor_uuid.empty() &&
                three_way_join_limit.server_row_count == 1,
            "three-way ordinary CROSS JOIN/LIMIT did not complete the "
            "bounded multi-source live route");

    auto object_backed_inner_join = parser.RunPipeline(
        "SELECT * FROM qow_packet7.qow_packet7_relation INNER JOIN "
        "qow_packet7.qow_packet7_join_relation ON integer_value = "
        "join_value;",
        true);
    if (!object_backed_inner_join.accepted) {
      PrintMessages(object_backed_inner_join.messages);
    }
    Require(object_backed_inner_join.accepted &&
                object_backed_inner_join.server_operation_id ==
                    "query.execute" &&
                object_backed_inner_join.server_cursor_uuid.empty() &&
                object_backed_inner_join.server_row_count == 2,
            "object-backed native INNER JOIN did not evaluate its typed ON "
            "predicate over two heap scans");

    const auto verify_inner_comparison =
        [&](const std::string_view comparison_operator,
            const std::uint64_t expected_rows) {
          auto joined = parser.RunPipeline(
              "SELECT * FROM qow_packet7.qow_packet7_relation INNER JOIN "
              "qow_packet7.qow_packet7_join_relation ON integer_value " +
                  std::string(comparison_operator) + " join_value;",
              true);
          if (!joined.accepted) PrintMessages(joined.messages);
          Require(joined.accepted &&
                      joined.server_operation_id == "query.execute" &&
                      joined.server_cursor_uuid.empty() &&
                      joined.server_row_count == expected_rows,
                  "object-backed INNER JOIN did not execute canonical typed "
                  "comparison operator " +
                      std::string(comparison_operator));
        };
    verify_inner_comparison("=", 2);
    verify_inner_comparison("<>", 7);
    verify_inner_comparison("!=", 7);
    verify_inner_comparison("<", 6);
    verify_inner_comparison("<=", 8);
    verify_inner_comparison(">", 1);
    verify_inner_comparison(">=", 3);
    verify_inner_comparison("IS DISTINCT FROM", 7);
    verify_inner_comparison("IS NOT DISTINCT FROM", 2);

    const auto verify_composite_join_predicate =
        [&](const std::string_view predicate,
            const std::uint64_t expected_rows) {
          auto joined = parser.RunPipeline(
              "SELECT * FROM qow_packet7.qow_packet7_relation INNER JOIN "
              "qow_packet7.qow_packet7_join_relation ON " +
                  std::string(predicate) + ";",
              true);
          if (!joined.accepted) PrintMessages(joined.messages);
          Require(joined.accepted &&
                      joined.server_operation_id == "query.execute" &&
                      joined.server_cursor_uuid.empty() &&
                      joined.server_row_count == expected_rows,
                  "object-backed INNER JOIN did not execute composite typed "
                  "predicate " +
                      std::string(predicate));
        };
    verify_composite_join_predicate(
        "integer_value = join_value AND auxiliary_value = "
        "join_auxiliary_value",
        1);
    verify_composite_join_predicate(
        "integer_value = join_value OR auxiliary_value = "
        "join_auxiliary_value",
        3);
    verify_composite_join_predicate(
        "integer_value = join_value AND auxiliary_value < "
        "join_auxiliary_value",
        1);
    verify_composite_join_predicate(
        "integer_value = join_value AND (auxiliary_value = "
        "join_auxiliary_value OR auxiliary_value < join_auxiliary_value)",
        2);
    verify_composite_join_predicate(
        "integer_value = join_value OR auxiliary_value = "
        "join_auxiliary_value AND auxiliary_value < join_auxiliary_value",
        2);
    verify_composite_join_predicate(
        "(integer_value = join_value OR auxiliary_value = "
        "join_auxiliary_value) AND auxiliary_value < join_auxiliary_value",
        1);
    verify_composite_join_predicate(
        "integer_value = join_value OR auxiliary_value = "
        "join_auxiliary_value OR auxiliary_value < join_auxiliary_value",
        7);

    const auto verify_outer_join = [&](const std::string_view join_sql,
                                       const std::uint64_t expected_rows,
                                       const std::string_view label) {
      auto joined = parser.RunPipeline(
          "SELECT * FROM qow_packet7.qow_packet7_relation " +
              std::string(join_sql) +
              " qow_packet7.qow_packet7_join_relation ON integer_value = "
              "join_value;",
          true);
      if (!joined.accepted) PrintMessages(joined.messages);
      Require(joined.accepted &&
                  joined.server_operation_id == "query.execute" &&
                  joined.server_cursor_uuid.empty() &&
                  joined.server_row_count == expected_rows,
              label);
    };
    verify_outer_join("LEFT OUTER JOIN", 3,
                      "object-backed LEFT OUTER JOIN did not preserve its "
                      "unmatched left row");
    verify_outer_join("RIGHT OUTER JOIN", 3,
                      "object-backed RIGHT OUTER JOIN did not preserve its "
                      "unmatched right row");
    verify_outer_join("FULL OUTER JOIN", 4,
                      "object-backed FULL OUTER JOIN did not preserve both "
                      "unmatched sides");

    auto mixed_model_join = parser.RunPipeline(
        "SELECT * FROM "
        "SPATIAL_SOURCE(qow_packet7.qow_packet7_spatial_relation) AS s "
        "INNER JOIN "
        "COLUMNAR_SOURCE(qow_packet7.qow_packet7_columnar_relation) AS c "
        "ON s.row_uuid = c.row_uuid;",
        true);
    if (!mixed_model_join.accepted) {
      PrintMessages(mixed_model_join.messages);
      std::cerr << mixed_model_join.server_result_payload << '\n';
    }
    Require(
        mixed_model_join.accepted &&
            mixed_model_join.server_operation_id == "query.execute" &&
            mixed_model_join.server_cursor_uuid.empty() &&
            mixed_model_join.server_row_count == 1 &&
            mixed_model_join.server_result_payload.find("payload=matched") !=
                std::string::npos &&
            mixed_model_join.server_result_payload.find(
                "evidence=canonical.model_join_left_provider_route:"
                "canonical.model-provider.spatial.v1") != std::string::npos &&
            mixed_model_join.server_result_payload.find(
                "evidence=canonical.model_join_right_provider_route:"
                "canonical.model-provider.columnar.v1") !=
                std::string::npos &&
            mixed_model_join.server_result_payload.find(
                "evidence=canonical.model_join_consumer_route:"
                "canonical.relational.join-3vl-nested.v1") !=
                std::string::npos,
        "ordinary mixed spatial/columnar SBSQL did not complete the "
        "authenticated V10 statement-receipt production route");

    const auto verify_left_only_join = [&](const std::string_view join_sql,
                                           const std::uint64_t expected_rows,
                                           const std::string_view label) {
      auto joined = parser.RunPipeline(
          "SELECT * FROM qow_packet7.qow_packet7_relation " +
              std::string(join_sql) +
              " qow_packet7.qow_packet7_join_relation ON integer_value = "
              "join_value;",
          true);
      if (!joined.accepted) PrintMessages(joined.messages);
      Require(joined.accepted &&
                  joined.server_operation_id == "query.execute" &&
                  joined.server_cursor_uuid.empty() &&
                  joined.server_row_count == expected_rows &&
                  joined.server_result_payload.find("integer_value") !=
                      std::string::npos &&
                  joined.server_result_payload.find("join_value") ==
                      std::string::npos,
              label);
    };
    verify_left_only_join(
        "LEFT SEMI JOIN", 2,
        "object-backed LEFT SEMI JOIN did not publish each matching left row "
        "once with a left-only result shape");
    verify_left_only_join(
        "LEFT ANTI JOIN", 1,
        "object-backed LEFT ANTI JOIN did not publish only its unmatched left "
        "row with a left-only result shape");

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

    auto approximate_top_k = parser.RunPipeline(
        "SELECT APPROX_TOP_K(text_value, 2) FROM "
        "qow_packet7.qow_packet7_relation;",
        true);
    if (!approximate_top_k.accepted) {
      PrintMessages(approximate_top_k.messages);
    }
    Require(approximate_top_k.accepted &&
                approximate_top_k.server_operation_id == "query.execute" &&
                approximate_top_k.server_cursor_uuid.empty() &&
                approximate_top_k.server_row_count == 1 &&
                approximate_top_k.server_result_payload.find(
                    "approx_top_k_value=[{\"value\":\"alpha\",\"count\":2},"
                    "{\"value\":\"beta\",\"count\":1}]") !=
                    std::string::npos,
            "object-backed APPROX_TOP_K did not preserve its exact numeric "
            "bound and canonical frequency ordering");

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

    struct CollectionAggregateProof {
      std::string_view expression;
      std::string_view expected_payload;
    };
    static constexpr std::array<CollectionAggregateProof, 3>
        kCollectionAggregateProofs{{
            {"ARRAY_AGG(text_value ORDER BY integer_value)",
             "array_agg_value=list[text:alpha;text:beta;text:alpha]"},
            {"JSON_AGG(text_value ORDER BY integer_value)",
             "json_agg_value=[\"alpha\",\"beta\",\"alpha\"]"},
            {"JSON_OBJECT_AGG(text_value, auxiliary_value ORDER BY "
             "integer_value)",
             "json_object_agg_value={\"beta\":102,\"alpha\":103}"},
        }};
    for (const auto& proof : kCollectionAggregateProofs) {
      const auto sql = "SELECT " + std::string(proof.expression) +
                       " FROM qow_packet7.qow_packet7_relation;";
      auto aggregate = parser.RunPipeline(sql, true);
      if (!aggregate.accepted) PrintMessages(aggregate.messages);
      if (aggregate.accepted &&
          aggregate.server_result_payload.find(proof.expected_payload) ==
              std::string::npos) {
        std::cerr << "collection_expression=" << proof.expression << '\n'
                  << "collection_payload="
                  << aggregate.server_result_payload << '\n';
      }
      Require(aggregate.accepted &&
                  aggregate.server_operation_id == "query.execute" &&
                  aggregate.server_cursor_uuid.empty() &&
                  aggregate.server_row_count == 1 &&
                  aggregate.server_result_payload.find(
                      proof.expected_payload) != std::string::npos,
              "object-backed ordered collection aggregate did not preserve "
              "its canonical result and persisted ordering route");
    }

    auto mode = parser.RunPipeline(
        "SELECT MODE() WITHIN GROUP (ORDER BY integer_value) FROM "
        "qow_packet7.qow_packet7_relation;",
        true);
    if (!mode.accepted) PrintMessages(mode.messages);
    Require(mode.accepted &&
                mode.server_operation_id == "query.execute" &&
                mode.server_cursor_uuid.empty() &&
                mode.server_row_count == 1 &&
                mode.server_result_payload.find("mode_value=1") !=
                    std::string::npos,
            "object-backed MODE did not preserve its exact WITHIN GROUP "
            "signed-integer ordering route");

    static constexpr std::array<AggregateProof, 4> kPercentileProofs{{
        {"PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY integer_value)",
         "percentile_cont_value=2"},
        {"PERCENTILE_DISC(0.5) WITHIN GROUP (ORDER BY integer_value)",
         "percentile_disc_value=2"},
        {"APPROX_PERCENTILE_CONT(0.5) WITHIN GROUP "
         "(ORDER BY integer_value)",
         "approx_percentile_cont_value=2"},
        {"APPROX_PERCENTILE_DISC(0.5) WITHIN GROUP "
         "(ORDER BY integer_value)",
         "approx_percentile_disc_value=2"},
    }};
    for (const auto& proof : kPercentileProofs) {
      const auto sql = "SELECT " + std::string(proof.function) + " FROM "
                       "qow_packet7.qow_packet7_relation;";
      auto percentile = parser.RunPipeline(sql, true);
      if (!percentile.accepted) PrintMessages(percentile.messages);
      Require(percentile.accepted &&
                  percentile.server_operation_id == "query.execute" &&
                  percentile.server_cursor_uuid.empty() &&
                  percentile.server_row_count == 1 &&
                  percentile.server_result_payload.find(
                      proof.expected_payload) != std::string::npos,
              "object-backed percentile did not preserve its exact numeric "
              "fraction and persisted WITHIN GROUP ordering route");
    }

    static constexpr std::array<AggregateProof, 4> kHypotheticalProofs{{
        {"RANK(2) WITHIN GROUP (ORDER BY integer_value)", "rank_value=2"},
        {"DENSE_RANK(2) WITHIN GROUP (ORDER BY integer_value)",
         "dense_rank_value=2"},
        {"PERCENT_RANK(2) WITHIN GROUP (ORDER BY integer_value)",
         "percent_rank_value=0.333333"},
        {"CUME_DIST(2) WITHIN GROUP (ORDER BY integer_value)",
         "cume_dist_value=0.75"},
    }};
    for (const auto& proof : kHypotheticalProofs) {
      const auto sql = "SELECT " + std::string(proof.function) + " FROM "
                       "qow_packet7.qow_packet7_relation;";
      auto hypothetical = parser.RunPipeline(sql, true);
      if (!hypothetical.accepted) PrintMessages(hypothetical.messages);
      Require(hypothetical.accepted &&
                  hypothetical.server_operation_id == "query.execute" &&
                  hypothetical.server_cursor_uuid.empty() &&
                  hypothetical.server_row_count == 1 &&
                  hypothetical.server_result_payload.find(
                      proof.expected_payload) != std::string::npos,
              "object-backed hypothetical set aggregate did not preserve "
              "its exact direct value and persisted ordering route");
    }

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

sbps::Frame AcquireNativeFrame(
    const std::array<std::uint8_t, 16>& session_uuid,
    std::uint64_t local_transaction_id,
    const std::array<std::uint8_t, 16>& transaction_uuid,
    std::uint16_t version,
    std::uint32_t schema_id) {
  auto frame = AcquireFrame(session_uuid,
                            local_transaction_id,
                            transaction_uuid);
  frame.header.payload_schema_id = schema_id;
  frame.payload[0] = static_cast<std::uint8_t>(version);
  frame.payload[1] = static_cast<std::uint8_t>(version >> 8u);
  return frame;
}

std::uint16_t PayloadU16(const std::vector<std::uint8_t>& payload,
                         const std::size_t offset) {
  return static_cast<std::uint16_t>(payload[offset]) |
         (static_cast<std::uint16_t>(payload[offset + 1]) << 8u);
}

void SetPayloadU16(std::vector<std::uint8_t>* payload,
                   const std::size_t offset,
                   const std::uint16_t value) {
  (*payload)[offset] = static_cast<std::uint8_t>(value & 0xffu);
  (*payload)[offset + 1] = static_cast<std::uint8_t>(value >> 8u);
}

void SetPayloadU32(std::vector<std::uint8_t>* payload,
                   const std::size_t offset,
                   const std::uint32_t value) {
  for (std::size_t byte = 0; byte < 4; ++byte) {
    (*payload)[offset + byte] =
        static_cast<std::uint8_t>(value >> (byte * 8u));
  }
}

std::string PayloadUuidText(const std::vector<std::uint8_t>& payload,
                            const std::size_t offset) {
  std::array<std::uint8_t, 16> bytes{};
  std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(offset),
              bytes.size(), bytes.begin());
  return server::UuidBytesToText(bytes);
}

struct NativePayloadLayout {
  std::size_t timestamp_length_offset{0};
  std::size_t timestamp_bytes_offset{0};
  std::size_t profile_count_offset{0};
  std::size_t profiles_offset{0};
  std::uint16_t profile_count{0};
};

std::optional<NativePayloadLayout> LocateNativePayloadLayout(
    const std::vector<std::uint8_t>& payload,
    const std::uint16_t expected_version) {
  constexpr std::size_t kBaseBytes = 2 + 1 + (6 * 16) + (2 * 8);
  constexpr std::size_t kProfileBytes = 64;
  if (payload.size() < kBaseBytes + 2 ||
      PayloadU16(payload, 0) != expected_version) {
    return std::nullopt;
  }
  NativePayloadLayout layout;
  std::size_t offset = kBaseBytes;
  layout.timestamp_length_offset = offset;
  const auto timestamp_size = PayloadU16(payload, offset);
  offset += 2;
  layout.timestamp_bytes_offset = offset;
  if (offset + timestamp_size + 6 * 16 + 2 > payload.size()) {
    return std::nullopt;
  }
  offset += timestamp_size + 6 * 16;
  const auto skip_registry = [&](const std::uint16_t expected_count,
                                 std::size_t* cursor) {
    if (*cursor + 2 > payload.size() ||
        PayloadU16(payload, *cursor) != expected_count) {
      return false;
    }
    *cursor += 2;
    for (std::uint16_t index = 0; index < expected_count; ++index) {
      if (*cursor + 4 > payload.size()) return false;
      *cursor += 2;
      const auto name_size = PayloadU16(payload, *cursor);
      *cursor += 2;
      if (*cursor + name_size + 17 > payload.size()) return false;
      *cursor += name_size + 17;
    }
    return true;
  };
  if (!skip_registry(43, &offset) || !skip_registry(11, &offset) ||
      offset + 2 > payload.size()) {
    return std::nullopt;
  }
  layout.profile_count_offset = offset;
  layout.profile_count = PayloadU16(payload, offset);
  layout.profiles_offset = offset + 2;
  if (payload.size() != layout.profiles_offset +
                            static_cast<std::size_t>(layout.profile_count) *
                                kProfileBytes) {
    return std::nullopt;
  }
  return layout;
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

  const auto verify_legacy_native_projection =
      [&](const std::uint16_t version,
          const std::uint32_t request_schema_id,
          const std::uint32_t response_schema_id,
          const std::size_t native_uuid_count) {
        constexpr std::size_t kBaseBytes = 2 + 1 + (6 * 16) + (2 * 8);
        constexpr std::size_t kProfileBytes = 64;
        const auto projected = server::HandleAcquireStatementContext(
            &registry, engine_state,
            AcquireNativeFrame(session_uuid,
                               server_transaction.local_transaction_id,
                               transaction_uuid.value.bytes, version,
                               request_schema_id));
        const auto profile_count_offset =
            kBaseBytes + native_uuid_count * 16;
        bool exact_profiles = projected.payload.size() ==
            profile_count_offset + 2 + 192 * kProfileBytes;
        if (exact_profiles) {
          exact_profiles = PayloadU16(projected.payload, 0) == version &&
                           projected.payload[2] == 1 &&
                           PayloadU16(projected.payload,
                                      profile_count_offset) == 192;
        }
        for (std::size_t index = 0; exact_profiles && index < 192; ++index) {
          const auto record = profile_count_offset + 2 + index * kProfileBytes;
          exact_profiles =
              projected.payload[record] == index / 32 + 1 &&
              PayloadU16(projected.payload, record + 1) == index % 32;
        }
        const auto statement_uuid =
            projected.payload.size() >= 19
                ? PayloadUuidText(projected.payload, 3)
                : std::string{};
        const auto projected_statement =
            registry.statement_contexts_by_statement_uuid.find(statement_uuid);
        Require(projected.accepted &&
                    projected.response_schema_id == response_schema_id &&
                    exact_profiles &&
                    projected_statement !=
                        registry.statement_contexts_by_statement_uuid.end() &&
                    bridge::ReleaseStatementContextReceipt(
                        projected_statement->second.receipt) ==
                        SB_ENGINE_STATUS_OK,
                "native V2/V3 statement-context projection drifted");
        registry.statement_contexts_by_statement_uuid.erase(
            projected_statement);
      };
  verify_legacy_native_projection(
      2, sbps::kSchemaAcquireStatementContextRequestV2,
      sbps::kSchemaAcquireStatementContextResultV2, 3);
  verify_legacy_native_projection(
      3, sbps::kSchemaAcquireStatementContextRequestV3,
      sbps::kSchemaAcquireStatementContextResultV3, 6);

  const auto verify_native_projection =
      [&](std::uint16_t version,
          std::uint32_t request_schema_id,
          std::uint32_t response_schema_id,
          std::size_t expected_profile_count,
          std::uint8_t expected_maximum_kind,
          std::size_t expected_window_function_count) {
        const auto projected = server::HandleAcquireStatementContext(
            &registry,
            engine_state,
            AcquireNativeFrame(session_uuid,
                               server_transaction.local_transaction_id,
                               transaction_uuid.value.bytes,
                               version,
                               request_schema_id));
        ipc::ParserStatementContext projected_context;
        const bool decoded =
            version == 10
                ? ipc::DecodeAcquireStatementContextResultPayloadV10ForTest(
                      projected.payload, &projected_context)
                : version == 9
                ? ipc::DecodeAcquireStatementContextResultPayloadV9ForTest(
                      projected.payload, &projected_context)
                : version == 8
                ? ipc::DecodeAcquireStatementContextResultPayloadV8ForTest(
                      projected.payload, &projected_context)
                : version == 4
                ? ipc::DecodeAcquireStatementContextResultPayloadV4ForTest(
                      projected.payload, &projected_context)
                : (version == 5
                       ? ipc::DecodeAcquireStatementContextResultPayloadV5ForTest(
                             projected.payload, &projected_context)
                       : (version == 6
                              ? ipc::DecodeAcquireStatementContextResultPayloadV6ForTest(
                                    projected.payload, &projected_context)
                              : ipc::DecodeAcquireStatementContextResultPayloadV7ForTest(
                                    projected.payload, &projected_context)));
        std::array<std::uint16_t, 24> profiles_by_kind{};
        for (const auto& profile : projected_context.descriptor_profiles) {
          if (profile.profile_kind < profiles_by_kind.size()) {
            ++profiles_by_kind[profile.profile_kind];
          }
        }
        bool exact_profile_families = true;
        for (std::uint8_t kind = 1; kind <= expected_maximum_kind; ++kind) {
          exact_profile_families =
              exact_profile_families &&
              profiles_by_kind[kind] ==
                  ((version == 10 && kind >= 14) ? 32
                                                : (kind >= 11 ? 2 : 32));
        }
        Require(projected.accepted &&
                    projected.response_schema_id == response_schema_id &&
                    decoded &&
                    projected_context.aggregate_function_profiles.size() ==
                        43 &&
                    projected_context.window_function_profiles.size() ==
                        expected_window_function_count &&
                    projected_context.descriptor_profiles.size() ==
                        expected_profile_count &&
                    exact_profile_families &&
                    (version == 10
                         ? projected_context.native_v10_complete()
                         : version == 9
                         ? projected_context.native_v9_complete()
                         : (version == 8
                                ? projected_context.native_v8_complete()
                         : (version == 7
                                ? projected_context.native_v7_complete()
                                : projected_context.statement_timestamp.empty()))),
                "native statement-context descriptor projection drifted");
        const auto projected_statement =
            registry.statement_contexts_by_statement_uuid.find(
                projected_context.statement_uuid);
        Require(projected_statement !=
                    registry.statement_contexts_by_statement_uuid.end() &&
                    (version < 7 ||
                     (!projected_context.statement_timestamp.empty() &&
                      projected_context.statement_timestamp ==
                          projected_statement->second.view.statement_timestamp)) &&
                    bridge::ReleaseStatementContextReceipt(
                        projected_statement->second.receipt) ==
                        SB_ENGINE_STATUS_OK,
                "native projection test receipt cleanup failed");
        registry.statement_contexts_by_statement_uuid.erase(
            projected_statement);
      };
  verify_native_projection(
      4,
      sbps::kSchemaAcquireStatementContextRequestV4,
      sbps::kSchemaAcquireStatementContextResultV4,
      192,
      6,
      0);
  verify_native_projection(
      5,
      sbps::kSchemaAcquireStatementContextRequestV5,
      sbps::kSchemaAcquireStatementContextResultV5,
      320,
      10,
      0);
  verify_native_projection(
      6,
      sbps::kSchemaAcquireStatementContextRequestV6,
      sbps::kSchemaAcquireStatementContextResultV6,
      320,
      10,
      11);
  verify_native_projection(
      7,
      sbps::kSchemaAcquireStatementContextRequestV7,
      sbps::kSchemaAcquireStatementContextResultV7,
      320,
      10,
      11);
  verify_native_projection(
      8,
      sbps::kSchemaAcquireStatementContextRequestV8,
      sbps::kSchemaAcquireStatementContextResultV8,
      322,
      11,
      11);
  verify_native_projection(
      9,
      sbps::kSchemaAcquireStatementContextRequestV9,
      sbps::kSchemaAcquireStatementContextResultV9,
      326,
      13,
      11);
  verify_native_projection(
      10,
      sbps::kSchemaAcquireStatementContextRequestV10,
      sbps::kSchemaAcquireStatementContextResultV10,
      646,
      23,
      11);
  Require(registry.statement_contexts_by_statement_uuid.size() == 1,
          "native projection compatibility receipts escaped test cleanup");

  const auto v7_projection = server::HandleAcquireStatementContext(
      &registry, engine_state,
      AcquireNativeFrame(session_uuid,
                         server_transaction.local_transaction_id,
                         transaction_uuid.value.bytes, 7,
                         sbps::kSchemaAcquireStatementContextRequestV7));
  ipc::ParserStatementContext v7_context;
  Require(v7_projection.accepted &&
              ipc::DecodeAcquireStatementContextResultPayloadV7ForTest(
                  v7_projection.payload, &v7_context) &&
              v7_context.native_v7_complete(),
          "native V7 statement timestamp projection was not current");
  constexpr std::size_t kV7TimestampLengthOffset =
      2 + 1 + (6 * 16) + (2 * 8);
  auto missing_timestamp = v7_projection.payload;
  Require(missing_timestamp.size() > kV7TimestampLengthOffset + 2,
          "native V7 timestamp payload was unexpectedly short");
  missing_timestamp[kV7TimestampLengthOffset] = 0;
  missing_timestamp[kV7TimestampLengthOffset + 1] = 0;
  ipc::ParserStatementContext refused_timestamp;
  Require(!ipc::DecodeAcquireStatementContextResultPayloadV7ForTest(
              missing_timestamp, &refused_timestamp),
          "native V7 decoder admitted a missing statement timestamp");
  auto malformed_timestamp = v7_projection.payload;
  malformed_timestamp[kV7TimestampLengthOffset + 2 + 10] = 'X';
  Require(!ipc::DecodeAcquireStatementContextResultPayloadV7ForTest(
              malformed_timestamp, &refused_timestamp),
          "native V7 decoder admitted a malformed statement timestamp");
  const auto v7_statement = registry.statement_contexts_by_statement_uuid.find(
      v7_context.statement_uuid);
  Require(v7_statement != registry.statement_contexts_by_statement_uuid.end() &&
              bridge::ReleaseStatementContextReceipt(
                  v7_statement->second.receipt) == SB_ENGINE_STATUS_OK,
          "native V7 timestamp test receipt cleanup failed");
  registry.statement_contexts_by_statement_uuid.erase(v7_statement);

  // QOW-SOURCE-RCP-077-LIVE-STATEMENT-CONTEXT-V8-PROOF
  const auto v8_projection = server::HandleAcquireStatementContext(
      &registry, engine_state,
      AcquireNativeFrame(session_uuid,
                         server_transaction.local_transaction_id,
                         transaction_uuid.value.bytes, 8,
                         sbps::kSchemaAcquireStatementContextRequestV8));
  ipc::ParserStatementContext v8_context;
  const auto v8_layout = LocateNativePayloadLayout(v8_projection.payload, 8);
  const auto core_manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  const auto real64_count =
      core_manifest.ok()
          ? std::count_if(core_manifest.manifest.descriptor_rows.begin(),
                          core_manifest.manifest.descriptor_rows.end(),
                          [](const auto& row) {
                            return row.stable_name == "real64";
                          })
          : 0;
  const auto real64_row =
      core_manifest.ok()
          ? std::find_if(core_manifest.manifest.descriptor_rows.begin(),
                         core_manifest.manifest.descriptor_rows.end(),
                         [](const auto& row) {
                           return row.stable_name == "real64";
                         })
          : core_manifest.manifest.descriptor_rows.end();
  const auto canonical_real64_type_uuid =
      real64_count == 1 &&
              real64_row != core_manifest.manifest.descriptor_rows.end() &&
              real64_row->descriptor_uuid.valid()
          ? uuid::UuidToString(real64_row->descriptor_uuid.value)
          : std::string{};
  std::set<std::string> v8_descriptor_uuids;
  bool v8_unique_descriptors = false;
  if (ipc::DecodeAcquireStatementContextResultPayloadV8ForTest(
          v8_projection.payload, &v8_context)) {
    v8_unique_descriptors =
        std::all_of(v8_context.descriptor_profiles.begin(),
                    v8_context.descriptor_profiles.end(),
                    [&](const auto& profile) {
                      return v8_descriptor_uuids.insert(
                                 profile.descriptor_uuid)
                          .second;
                    });
  }
  Require(v8_projection.accepted &&
              v8_projection.response_schema_id ==
                  sbps::kSchemaAcquireStatementContextResultV8 &&
              v8_layout.has_value() && v8_layout->profile_count == 322 &&
              v8_context.native_v8_complete() && v8_unique_descriptors &&
              v8_descriptor_uuids.size() == 322 &&
              v8_context.descriptor_profiles[320].profile_kind == 11 &&
              v8_context.descriptor_profiles[320].slot == 0 &&
              v8_context.descriptor_profiles[321].profile_kind == 11 &&
              v8_context.descriptor_profiles[321].slot == 1 &&
              v8_context.descriptor_profiles[320].descriptor_uuid !=
                  v8_context.descriptor_profiles[321].descriptor_uuid &&
              !canonical_real64_type_uuid.empty() &&
              v8_context.descriptor_profiles[320].type_uuid ==
                  canonical_real64_type_uuid &&
              v8_context.descriptor_profiles[321].type_uuid ==
                  canonical_real64_type_uuid &&
              !v8_context.descriptor_profiles[320].nullable &&
              !v8_context.descriptor_profiles[321].nullable,
          "native V8 REAL64 result descriptor projection drifted");

  constexpr std::size_t kProfileBytes = 64;
  const auto profile_offset = [&](const std::size_t ordinal) {
    return v8_layout->profiles_offset + ordinal * kProfileBytes;
  };
  const auto expect_v8_refusal = [&](std::string_view mutation,
                                     const std::vector<std::uint8_t>& payload) {
    ipc::ParserStatementContext refused;
    Require(!ipc::DecodeAcquireStatementContextResultPayloadV8ForTest(
                payload, &refused),
            mutation);
  };
  {
    auto mutation = v8_projection.payload;
    mutation.erase(mutation.begin() +
                       static_cast<std::ptrdiff_t>(profile_offset(321)),
                   mutation.end());
    SetPayloadU16(&mutation, v8_layout->profile_count_offset, 321);
    expect_v8_refusal("V8 decoder admitted a missing kind-11 slot", mutation);
  }
  {
    auto mutation = v8_projection.payload;
    std::copy_n(mutation.begin() +
                    static_cast<std::ptrdiff_t>(profile_offset(320)),
                kProfileBytes,
                mutation.begin() +
                    static_cast<std::ptrdiff_t>(profile_offset(321)));
    expect_v8_refusal("V8 decoder admitted a duplicate record", mutation);
  }
  {
    auto mutation = v8_projection.payload;
    std::swap_ranges(mutation.begin() +
                         static_cast<std::ptrdiff_t>(profile_offset(320)),
                     mutation.begin() +
                         static_cast<std::ptrdiff_t>(profile_offset(321)),
                     mutation.begin() +
                         static_cast<std::ptrdiff_t>(profile_offset(321)));
    expect_v8_refusal("V8 decoder admitted reordered kind-11 records",
                      mutation);
  }
  {
    auto mutation = v8_projection.payload;
    mutation[profile_offset(320)] = 10;
    expect_v8_refusal("V8 decoder admitted a wrong kind-11 record", mutation);
  }
  {
    auto mutation = v8_projection.payload;
    mutation[profile_offset(320)] = 12;
    expect_v8_refusal("V8 decoder admitted an unknown profile kind", mutation);
  }
  {
    auto mutation = v8_projection.payload;
    SetPayloadU16(&mutation, profile_offset(321) + 1, 2);
    expect_v8_refusal("V8 decoder admitted a wrong kind-11 slot", mutation);
  }
  {
    auto mutation = v8_projection.payload;
    const std::vector<std::uint8_t> extra_record(
        mutation.begin() +
            static_cast<std::ptrdiff_t>(profile_offset(321)),
        mutation.begin() +
            static_cast<std::ptrdiff_t>(profile_offset(322)));
    mutation.insert(mutation.end(), extra_record.begin(), extra_record.end());
    SetPayloadU16(&mutation, v8_layout->profile_count_offset, 323);
    SetPayloadU16(&mutation, profile_offset(322) + 1, 2);
    expect_v8_refusal("V8 decoder admitted an extra kind-11 slot", mutation);
  }
  {
    auto mutation = v8_projection.payload;
    mutation[profile_offset(321) + 51] = 1;
    expect_v8_refusal("V8 decoder admitted a nullable REAL64 profile",
                      mutation);
  }
  {
    auto mutation = v8_projection.payload;
    mutation[profile_offset(321) + 35] = 1;
    expect_v8_refusal("V8 decoder admitted a collation-bearing REAL64 profile",
                      mutation);
  }
  for (const auto [field_offset, label] :
       std::array<std::pair<std::size_t, std::string_view>, 3>{
           std::pair{std::size_t{52},
                     std::string_view{"V8 decoder admitted REAL64 width"}},
           std::pair{std::size_t{56},
                     std::string_view{"V8 decoder admitted REAL64 precision"}},
           std::pair{std::size_t{60},
                     std::string_view{"V8 decoder admitted REAL64 scale"}},
       }) {
    auto mutation = v8_projection.payload;
    SetPayloadU32(&mutation, profile_offset(321) + field_offset, 1);
    expect_v8_refusal(label, mutation);
  }
  {
    auto mutation = v8_projection.payload;
    std::fill_n(mutation.begin() +
                    static_cast<std::ptrdiff_t>(profile_offset(321) + 3),
                16, 0);
    expect_v8_refusal("V8 decoder admitted a zero descriptor UUID", mutation);
  }
  {
    auto mutation = v8_projection.payload;
    mutation[profile_offset(321) + 3 + 6] &= 0x0fu;
    mutation[profile_offset(321) + 3 + 8] &= 0x3fu;
    expect_v8_refusal("V8 decoder admitted a malformed descriptor UUID",
                      mutation);
  }
  {
    auto mutation = v8_projection.payload;
    std::copy_n(mutation.begin() +
                    static_cast<std::ptrdiff_t>(profile_offset(320) + 3),
                16,
                mutation.begin() +
                    static_cast<std::ptrdiff_t>(profile_offset(321) + 3));
    expect_v8_refusal("V8 decoder admitted a duplicate descriptor UUID",
                      mutation);
  }
  {
    auto mutation = v8_projection.payload;
    std::copy_n(mutation.begin() +
                    static_cast<std::ptrdiff_t>(profile_offset(0) + 19),
                16,
                mutation.begin() +
                    static_cast<std::ptrdiff_t>(profile_offset(321) + 19));
    expect_v8_refusal("V8 decoder admitted mismatched REAL64 type UUIDs",
                      mutation);
  }
  {
    auto mutation = v8_projection.payload;
    std::fill_n(mutation.begin() +
                    static_cast<std::ptrdiff_t>(profile_offset(321) + 19),
                16, 0);
    expect_v8_refusal("V8 decoder admitted a zero REAL64 type UUID", mutation);
  }
  {
    auto mutation = v8_projection.payload;
    mutation[profile_offset(321) + 19 + 6] &= 0x0fu;
    mutation[profile_offset(321) + 19 + 8] &= 0x3fu;
    expect_v8_refusal("V8 decoder admitted a malformed REAL64 type UUID",
                      mutation);
  }
  {
    auto mutation = v8_projection.payload;
    mutation[v8_layout->timestamp_bytes_offset + 10] = 'X';
    expect_v8_refusal("V8 decoder admitted a malformed timestamp", mutation);
  }
  {
    auto mutation = v8_projection.payload;
    mutation.pop_back();
    expect_v8_refusal("V8 decoder admitted a truncated payload", mutation);
  }
  {
    auto mutation = v8_projection.payload;
    mutation.push_back(0);
    expect_v8_refusal("V8 decoder admitted trailing payload bytes", mutation);
  }
  {
    auto mutation = v8_projection.payload;
    SetPayloadU16(&mutation, 0, 7);
    expect_v8_refusal("V8 decoder admitted a V7 version collision", mutation);
  }

  const auto v8_statement = registry.statement_contexts_by_statement_uuid.find(
      v8_context.statement_uuid);
  Require(v8_statement != registry.statement_contexts_by_statement_uuid.end() &&
              bridge::ReleaseStatementContextReceipt(
                  v8_statement->second.receipt) == SB_ENGINE_STATUS_OK,
          "native V8 mutation test receipt cleanup failed");
  registry.statement_contexts_by_statement_uuid.erase(v8_statement);

  // QOW-SOURCE-RCP-078-LIVE-STATEMENT-CONTEXT-V9-PROOF
  const auto v9_projection = server::HandleAcquireStatementContext(
      &registry, engine_state,
      AcquireNativeFrame(session_uuid,
                         server_transaction.local_transaction_id,
                         transaction_uuid.value.bytes, 9,
                         sbps::kSchemaAcquireStatementContextRequestV9));
  ipc::ParserStatementContext v9_context;
  const auto v9_layout = LocateNativePayloadLayout(v9_projection.payload, 9);
  const auto canonical_type_uuid = [&](const std::string_view stable_name) {
    const auto count =
        core_manifest.ok()
            ? std::count_if(core_manifest.manifest.descriptor_rows.begin(),
                            core_manifest.manifest.descriptor_rows.end(),
                            [&](const auto& row) {
                              return row.stable_name == stable_name;
                            })
            : 0;
    const auto row =
        core_manifest.ok()
            ? std::find_if(core_manifest.manifest.descriptor_rows.begin(),
                           core_manifest.manifest.descriptor_rows.end(),
                           [&](const auto& candidate) {
                             return candidate.stable_name == stable_name;
                           })
            : core_manifest.manifest.descriptor_rows.end();
    return count == 1 &&
                   row != core_manifest.manifest.descriptor_rows.end() &&
                   row->descriptor_uuid.valid()
               ? uuid::UuidToString(row->descriptor_uuid.value)
               : std::string{};
  };
  const auto canonical_uuid_type_uuid = canonical_type_uuid("uuid");
  const auto canonical_uint64_type_uuid = canonical_type_uuid("uint64");
  std::set<std::string> v9_descriptor_uuids;
  bool v9_unique_descriptors = false;
  if (ipc::DecodeAcquireStatementContextResultPayloadV9ForTest(
          v9_projection.payload, &v9_context)) {
    v9_unique_descriptors =
        std::all_of(v9_context.descriptor_profiles.begin(),
                    v9_context.descriptor_profiles.end(),
                    [&](const auto& profile) {
                      return v9_descriptor_uuids.insert(
                                 profile.descriptor_uuid)
                          .second;
                    });
  const auto exact_v9_profile = [&](const std::size_t ordinal,
                                    const std::uint8_t kind,
                                    const std::uint16_t slot,
                                    const std::string& type_uuid) {
    const auto& profile = v9_context.descriptor_profiles[ordinal];
    return profile.profile_kind == kind && profile.slot == slot &&
           profile.type_uuid == type_uuid && !profile.descriptor_uuid.empty() &&
           !profile.nullable && profile.collation_uuid.empty() &&
           profile.width == 0 && profile.precision == 0 &&
           profile.scale == 0;
  };
  Require(v9_projection.accepted &&
              v9_projection.response_schema_id ==
                  sbps::kSchemaAcquireStatementContextResultV9 &&
              v9_layout.has_value() && v9_layout->profile_count == 326 &&
              v9_context.native_v9_complete() && v9_unique_descriptors &&
              v9_descriptor_uuids.size() == 326 &&
              !canonical_real64_type_uuid.empty() &&
              !canonical_uuid_type_uuid.empty() &&
              !canonical_uint64_type_uuid.empty() &&
              exact_v9_profile(320, 11, 0, canonical_real64_type_uuid) &&
              exact_v9_profile(321, 11, 1, canonical_real64_type_uuid) &&
              exact_v9_profile(322, 12, 0, canonical_uuid_type_uuid) &&
              exact_v9_profile(323, 12, 1, canonical_uuid_type_uuid) &&
              exact_v9_profile(324, 13, 0, canonical_uint64_type_uuid) &&
              exact_v9_profile(325, 13, 1, canonical_uint64_type_uuid),
          "native V9 search result descriptor projection drifted");

  const auto v9_profile_offset = [&](const std::size_t ordinal) {
    return v9_layout->profiles_offset + ordinal * kProfileBytes;
  };
  std::size_t v9_mutation_count = 0;
  const auto expect_v9_refusal = [&](std::string_view mutation,
                                     const std::vector<std::uint8_t>& payload) {
    ++v9_mutation_count;
    ipc::ParserStatementContext refused;
    Require(!ipc::DecodeAcquireStatementContextResultPayloadV9ForTest(
                payload, &refused),
            mutation);
  };
  {
    auto mutation = v9_projection.payload;
    SetPayloadU16(&mutation, v9_layout->profile_count_offset, 325);
    expect_v9_refusal("MUT-001 V9 admitted a wrong total profile count",
                      mutation);
  }
  for (std::size_t ordinal = 322; ordinal < 326; ++ordinal) {
    auto mutation = v9_projection.payload;
    mutation.erase(
        mutation.begin() +
            static_cast<std::ptrdiff_t>(v9_profile_offset(ordinal)),
        mutation.begin() +
            static_cast<std::ptrdiff_t>(v9_profile_offset(ordinal + 1)));
    SetPayloadU16(&mutation, v9_layout->profile_count_offset, 325);
    expect_v9_refusal("MUT-002..005 V9 admitted a missing appended profile",
                      mutation);
  }
  {
    auto mutation = v9_projection.payload;
    std::array<std::uint8_t, kProfileBytes> extra_profile{};
    std::copy_n(
        mutation.begin() +
            static_cast<std::ptrdiff_t>(v9_profile_offset(325)),
        kProfileBytes, extra_profile.begin());
    mutation.insert(mutation.end(), extra_profile.begin(),
                    extra_profile.end());
    SetPayloadU16(&mutation, v9_layout->profile_count_offset, 327);
    expect_v9_refusal("MUT-006 V9 admitted an extra profile", mutation);
  }
  for (std::size_t ordinal = 322; ordinal < 325; ++ordinal) {
    auto mutation = v9_projection.payload;
    std::swap_ranges(
        mutation.begin() +
            static_cast<std::ptrdiff_t>(v9_profile_offset(ordinal)),
        mutation.begin() +
            static_cast<std::ptrdiff_t>(v9_profile_offset(ordinal + 1)),
        mutation.begin() +
            static_cast<std::ptrdiff_t>(v9_profile_offset(ordinal + 1)));
    expect_v9_refusal("MUT-007..009 V9 admitted reordered appended profiles",
                      mutation);
  }
  for (std::size_t ordinal = 322; ordinal < 326; ++ordinal) {
    auto mutation = v9_projection.payload;
    mutation[v9_profile_offset(ordinal)] =
        ordinal < 324 ? static_cast<std::uint8_t>(13)
                      : static_cast<std::uint8_t>(12);
    expect_v9_refusal("MUT-010..013 V9 admitted a wrong profile kind",
                      mutation);
  }
  for (std::size_t ordinal = 322; ordinal < 326; ++ordinal) {
    auto mutation = v9_projection.payload;
    SetPayloadU16(&mutation, v9_profile_offset(ordinal) + 1,
                  static_cast<std::uint16_t>(ordinal % 2 + 2));
    expect_v9_refusal("MUT-014..017 V9 admitted a wrong profile slot",
                      mutation);
  }
  for (std::size_t ordinal = 322; ordinal < 326; ++ordinal) {
    auto mutation = v9_projection.payload;
    mutation[v9_profile_offset(ordinal) + 3 + 6] &= 0x0fu;
    mutation[v9_profile_offset(ordinal) + 3 + 8] &= 0x3fu;
    expect_v9_refusal(
        "MUT-018..021 V9 admitted a noncanonical descriptor UUID", mutation);
  }
  for (std::size_t ordinal = 322; ordinal < 326; ++ordinal) {
    auto mutation = v9_projection.payload;
    const auto source_ordinal = ordinal == 322 ? 321 : ordinal - 1;
    std::copy_n(
        mutation.begin() +
            static_cast<std::ptrdiff_t>(v9_profile_offset(source_ordinal) + 3),
        16,
        mutation.begin() +
            static_cast<std::ptrdiff_t>(v9_profile_offset(ordinal) + 3));
    expect_v9_refusal("MUT-022..025 V9 admitted a duplicate descriptor UUID",
                      mutation);
  }
  for (std::size_t ordinal = 322; ordinal < 326; ++ordinal) {
    auto mutation = v9_projection.payload;
    const auto source_ordinal = ordinal < 324 ? 324 : 322;
    std::copy_n(
        mutation.begin() + static_cast<std::ptrdiff_t>(
                               v9_profile_offset(source_ordinal) + 19),
        16,
        mutation.begin() +
            static_cast<std::ptrdiff_t>(v9_profile_offset(ordinal) + 19));
    expect_v9_refusal("MUT-026..029 V9 admitted a wrong canonical type UUID",
                      mutation);
  }
  for (std::size_t ordinal = 322; ordinal < 326; ++ordinal) {
    auto mutation = v9_projection.payload;
    mutation[v9_profile_offset(ordinal) + 51] = 1;
    expect_v9_refusal("MUT-030..033 V9 admitted a nullable appended profile",
                      mutation);
  }
  for (std::size_t ordinal = 322; ordinal < 326; ++ordinal) {
    auto mutation = v9_projection.payload;
    std::copy_n(
        mutation.begin() +
            static_cast<std::ptrdiff_t>(v9_profile_offset(ordinal) + 3),
        16,
        mutation.begin() +
            static_cast<std::ptrdiff_t>(v9_profile_offset(ordinal) + 35));
    expect_v9_refusal("MUT-034..037 V9 admitted a collation UUID", mutation);
  }
  for (std::size_t ordinal = 322; ordinal < 326; ++ordinal) {
    auto mutation = v9_projection.payload;
    SetPayloadU32(&mutation, v9_profile_offset(ordinal) + 52, 1);
    expect_v9_refusal("MUT-038..041 V9 admitted nonzero width", mutation);
  }
  for (std::size_t ordinal = 322; ordinal < 326; ++ordinal) {
    auto mutation = v9_projection.payload;
    SetPayloadU32(&mutation, v9_profile_offset(ordinal) + 56, 1);
    expect_v9_refusal("MUT-042..045 V9 admitted nonzero precision",
                      mutation);
  }
  for (std::size_t ordinal = 322; ordinal < 326; ++ordinal) {
    auto mutation = v9_projection.payload;
    SetPayloadU32(&mutation, v9_profile_offset(ordinal) + 60, 1);
    expect_v9_refusal("MUT-046..049 V9 admitted nonzero scale", mutation);
  }
  {
    auto mutation = v9_projection.payload;
    mutation.pop_back();
    expect_v9_refusal("MUT-050 V9 admitted a truncated payload", mutation);
  }
  {
    auto mutation = v9_projection.payload;
    mutation.push_back(0);
    expect_v9_refusal("MUT-051 V9 admitted trailing payload bytes", mutation);
  }
  Require(v9_mutation_count == 51,
          "native V9 mutation inventory did not execute exactly 51 cases");

  const auto v9_statement = registry.statement_contexts_by_statement_uuid.find(
      v9_context.statement_uuid);
  Require(v9_statement != registry.statement_contexts_by_statement_uuid.end() &&
              bridge::ReleaseStatementContextReceipt(
                  v9_statement->second.receipt) == SB_ENGINE_STATUS_OK,
          "native V9 mutation test receipt cleanup failed");
  registry.statement_contexts_by_statement_uuid.erase(v9_statement);

  // QOW-SOURCE-RCP-079-LIVE-STATEMENT-CONTEXT-V10-PROOF
  const auto v10_projection = server::HandleAcquireStatementContext(
      &registry, engine_state,
      AcquireNativeFrame(session_uuid,
                         server_transaction.local_transaction_id,
                         transaction_uuid.value.bytes, 10,
                         sbps::kSchemaAcquireStatementContextRequestV10));
  ipc::ParserStatementContext v10_context;
  const auto v10_layout = LocateNativePayloadLayout(v10_projection.payload, 10);
  const auto canonical_boolean_type_uuid = canonical_type_uuid("boolean");
  const auto canonical_geometry_type_uuid = canonical_type_uuid("geometry");
  const std::array<std::string, 5> expected_multileg_types = {
      canonical_uuid_type_uuid, canonical_uint64_type_uuid,
      canonical_real64_type_uuid, canonical_boolean_type_uuid,
      canonical_geometry_type_uuid};
  std::set<std::string> v10_descriptor_uuids;
  bool v10_unique_descriptors = false;
  if (ipc::DecodeAcquireStatementContextResultPayloadV10ForTest(
          v10_projection.payload, &v10_context)) {
    v10_unique_descriptors = std::ranges::all_of(
        v10_context.descriptor_profiles, [&](const auto& profile) {
          return v10_descriptor_uuids.insert(profile.descriptor_uuid).second;
        });
  }
  bool exact_v10_suffix = v10_context.descriptor_profiles.size() == 646;
  for (std::size_t suffix = 0; exact_v10_suffix && suffix < 320; ++suffix) {
    const auto& profile = v10_context.descriptor_profiles[326 + suffix];
    const auto expected_kind = static_cast<std::uint8_t>(14 + suffix / 32);
    const auto expected_slot = static_cast<std::uint16_t>(suffix % 32);
    const auto type_group = suffix / 64;
    exact_v10_suffix =
        profile.profile_kind == expected_kind &&
        profile.slot == expected_slot &&
        profile.type_uuid == expected_multileg_types[type_group] &&
        profile.nullable == (expected_kind % 2 == 1) &&
        profile.collation_uuid.empty() && profile.width == 0 &&
        profile.precision == 0 && profile.scale == 0;
  }
  const bool dv001_exact =
      v10_projection.accepted &&
      v10_projection.response_schema_id ==
          sbps::kSchemaAcquireStatementContextResultV10 &&
      v10_layout.has_value() && v10_layout->profile_count == 646 &&
      v10_context.native_v10_complete() && v10_unique_descriptors &&
      v10_descriptor_uuids.size() == 646 && exact_v10_suffix &&
      std::ranges::all_of(expected_multileg_types,
                          [](const auto& value) { return !value.empty(); }) &&
      std::set<std::string>(expected_multileg_types.begin(),
                            expected_multileg_types.end()).size() == 5;
  Require(dv001_exact,
          "native V10 multileg descriptor projection drifted");
  std::cout << "RCP-079 literal catalog case=RCP079-DV-001;"
               "status=passed;skipped=0\n";

  const auto v10_profile_offset = [&](const std::size_t ordinal) {
    return v10_layout->profiles_offset + ordinal * kProfileBytes;
  };
  std::size_t v10_mutation_count = 0;
  const auto expect_v10_refusal = [&](std::string_view mutation,
                                      const std::vector<std::uint8_t>& payload) {
    ++v10_mutation_count;
    ipc::ParserStatementContext refused;
    Require(!ipc::DecodeAcquireStatementContextResultPayloadV10ForTest(
                payload, &refused),
            mutation);
  };
  {
    auto mutation = v10_projection.payload;
    SetPayloadU16(&mutation, v10_layout->profile_count_offset, 645);
    expect_v10_refusal("V10 admitted a wrong total profile count", mutation);
  }
  for (std::size_t ordinal = 326; ordinal < 646; ++ordinal) {
    auto mutation = v10_projection.payload;
    mutation.erase(
        mutation.begin() +
            static_cast<std::ptrdiff_t>(v10_profile_offset(ordinal)),
        mutation.begin() +
            static_cast<std::ptrdiff_t>(v10_profile_offset(ordinal + 1)));
    SetPayloadU16(&mutation, v10_layout->profile_count_offset, 645);
    expect_v10_refusal("V10 admitted a missing suffix profile", mutation);
  }
  {
    auto mutation = v10_projection.payload;
    mutation.insert(
        mutation.end(),
        mutation.begin() +
            static_cast<std::ptrdiff_t>(v10_profile_offset(645)),
        mutation.begin() +
            static_cast<std::ptrdiff_t>(v10_profile_offset(646)));
    SetPayloadU16(&mutation, v10_layout->profile_count_offset, 647);
    expect_v10_refusal("V10 admitted an extra suffix profile", mutation);
  }
  for (std::size_t ordinal = 326; ordinal < 645; ++ordinal) {
    auto mutation = v10_projection.payload;
    std::swap_ranges(
        mutation.begin() +
            static_cast<std::ptrdiff_t>(v10_profile_offset(ordinal)),
        mutation.begin() +
            static_cast<std::ptrdiff_t>(v10_profile_offset(ordinal + 1)),
        mutation.begin() +
            static_cast<std::ptrdiff_t>(v10_profile_offset(ordinal + 1)));
    expect_v10_refusal("V10 admitted adjacent suffix reorder", mutation);
  }
  for (std::size_t ordinal = 326; ordinal < 646; ++ordinal) {
    auto mutation = v10_projection.payload;
    mutation[v10_profile_offset(ordinal)] =
        mutation[v10_profile_offset(ordinal)] == 23
            ? static_cast<std::uint8_t>(22)
            : static_cast<std::uint8_t>(23);
    expect_v10_refusal("V10 admitted wrong suffix kind", mutation);
  }
  for (std::size_t ordinal = 326; ordinal < 646; ++ordinal) {
    auto mutation = v10_projection.payload;
    SetPayloadU16(&mutation, v10_profile_offset(ordinal) + 1, 32);
    expect_v10_refusal("V10 admitted wrong suffix slot", mutation);
  }
  for (std::size_t ordinal = 326; ordinal < 646; ++ordinal) {
    auto mutation = v10_projection.payload;
    std::fill_n(mutation.begin() + static_cast<std::ptrdiff_t>(
                    v10_profile_offset(ordinal) + 3),
                16, 0);
    expect_v10_refusal("V10 admitted invalid descriptor UUID", mutation);
  }
  for (std::size_t ordinal = 326; ordinal < 646; ++ordinal) {
    auto mutation = v10_projection.payload;
    const auto source_ordinal = ordinal == 326 ? 327 : 326;
    std::copy_n(
        mutation.begin() + static_cast<std::ptrdiff_t>(
                               v10_profile_offset(source_ordinal) + 3),
        16,
        mutation.begin() + static_cast<std::ptrdiff_t>(
                               v10_profile_offset(ordinal) + 3));
    expect_v10_refusal("V10 admitted duplicate descriptor UUID", mutation);
  }
  for (std::size_t ordinal = 326; ordinal < 646; ++ordinal) {
    auto mutation = v10_projection.payload;
    const auto type_group = (ordinal - 326) / 64;
    const auto source_ordinal = 326 + ((type_group + 1) % 5) * 64;
    std::copy_n(
        mutation.begin() + static_cast<std::ptrdiff_t>(
                               v10_profile_offset(source_ordinal) + 19),
        16,
        mutation.begin() + static_cast<std::ptrdiff_t>(
                               v10_profile_offset(ordinal) + 19));
    expect_v10_refusal("V10 admitted wrong suffix type UUID", mutation);
  }
  for (std::size_t ordinal = 326; ordinal < 646; ++ordinal) {
    auto mutation = v10_projection.payload;
    mutation[v10_profile_offset(ordinal) + 51] ^= 1;
    expect_v10_refusal("V10 admitted wrong suffix nullability", mutation);
  }
  for (std::size_t ordinal = 326; ordinal < 646; ++ordinal) {
    auto mutation = v10_projection.payload;
    std::copy_n(
        mutation.begin() + static_cast<std::ptrdiff_t>(
                               v10_profile_offset(ordinal) + 3),
        16,
        mutation.begin() + static_cast<std::ptrdiff_t>(
                               v10_profile_offset(ordinal) + 35));
    expect_v10_refusal("V10 admitted suffix collation UUID", mutation);
  }
  for (std::size_t ordinal = 326; ordinal < 646; ++ordinal) {
    auto mutation = v10_projection.payload;
    SetPayloadU32(&mutation, v10_profile_offset(ordinal) + 52, 1);
    expect_v10_refusal("V10 admitted nonzero suffix width", mutation);
  }
  for (std::size_t ordinal = 326; ordinal < 646; ++ordinal) {
    auto mutation = v10_projection.payload;
    SetPayloadU32(&mutation, v10_profile_offset(ordinal) + 56, 1);
    expect_v10_refusal("V10 admitted nonzero suffix precision", mutation);
  }
  for (std::size_t ordinal = 326; ordinal < 646; ++ordinal) {
    auto mutation = v10_projection.payload;
    SetPayloadU32(&mutation, v10_profile_offset(ordinal) + 60, 1);
    expect_v10_refusal("V10 admitted nonzero suffix scale", mutation);
  }
  {
    auto mutation = v10_projection.payload;
    mutation.pop_back();
    expect_v10_refusal("V10 admitted a truncated payload", mutation);
  }
  {
    auto mutation = v10_projection.payload;
    mutation.push_back(0);
    expect_v10_refusal("V10 admitted a trailing payload byte", mutation);
  }
  const bool dv002_exact = v10_mutation_count == 3843;
  Require(dv002_exact,
          "native V10 mutation inventory did not execute exactly 3843 cases");
  std::cout << "RCP-079 literal catalog case=RCP079-DV-002;"
               "status=passed;skipped=0;mutations=3843/3843\n";
  std::cout << "RCP-079 live descriptor catalog passed "
               "(2/2;skipped=0)\n";

  const auto v10_statement = registry.statement_contexts_by_statement_uuid.find(
      v10_context.statement_uuid);
  Require(v10_statement != registry.statement_contexts_by_statement_uuid.end() &&
              bridge::ReleaseStatementContextReceipt(
                  v10_statement->second.receipt) == SB_ENGINE_STATUS_OK,
          "native V10 mutation test receipt cleanup failed");
  registry.statement_contexts_by_statement_uuid.erase(v10_statement);
  }

  const auto v8_schema_v7_version = server::HandleAcquireStatementContext(
      &registry, engine_state,
      AcquireNativeFrame(session_uuid,
                         server_transaction.local_transaction_id,
                         transaction_uuid.value.bytes, 7,
                         sbps::kSchemaAcquireStatementContextRequestV8));
  const auto v7_schema_v8_version = server::HandleAcquireStatementContext(
      &registry, engine_state,
      AcquireNativeFrame(session_uuid,
                         server_transaction.local_transaction_id,
                         transaction_uuid.value.bytes, 8,
                         sbps::kSchemaAcquireStatementContextRequestV7));
  Require(!v8_schema_v7_version.accepted &&
              !v7_schema_v8_version.accepted &&
              !v8_schema_v7_version.diagnostics.empty() &&
              !v7_schema_v8_version.diagnostics.empty() &&
              v8_schema_v7_version.diagnostics.front().code ==
                  "PARSER_SERVER_IPC.STATEMENT_CONTEXT_REQUEST_INVALID" &&
              v7_schema_v8_version.diagnostics.front().code ==
                  "PARSER_SERVER_IPC.STATEMENT_CONTEXT_REQUEST_INVALID",
          "V7/V8 statement-context schema/version collision was admitted");

  const auto v9_schema_v8_version = server::HandleAcquireStatementContext(
      &registry, engine_state,
      AcquireNativeFrame(session_uuid,
                         server_transaction.local_transaction_id,
                         transaction_uuid.value.bytes, 8,
                         sbps::kSchemaAcquireStatementContextRequestV9));
  const auto v8_schema_v9_version = server::HandleAcquireStatementContext(
      &registry, engine_state,
      AcquireNativeFrame(session_uuid,
                         server_transaction.local_transaction_id,
                         transaction_uuid.value.bytes, 9,
                         sbps::kSchemaAcquireStatementContextRequestV8));
  Require(!v9_schema_v8_version.accepted &&
              !v8_schema_v9_version.accepted &&
              !v9_schema_v8_version.diagnostics.empty() &&
              !v8_schema_v9_version.diagnostics.empty() &&
              v9_schema_v8_version.diagnostics.front().code ==
                  "PARSER_SERVER_IPC.STATEMENT_CONTEXT_REQUEST_INVALID" &&
              v8_schema_v9_version.diagnostics.front().code ==
                  "PARSER_SERVER_IPC.STATEMENT_CONTEXT_REQUEST_INVALID",
          "V8/V9 statement-context schema/version collision was admitted");

  const auto v10_schema_v9_version = server::HandleAcquireStatementContext(
      &registry, engine_state,
      AcquireNativeFrame(session_uuid,
                         server_transaction.local_transaction_id,
                         transaction_uuid.value.bytes, 9,
                         sbps::kSchemaAcquireStatementContextRequestV10));
  const auto v9_schema_v10_version = server::HandleAcquireStatementContext(
      &registry, engine_state,
      AcquireNativeFrame(session_uuid,
                         server_transaction.local_transaction_id,
                         transaction_uuid.value.bytes, 10,
                         sbps::kSchemaAcquireStatementContextRequestV9));
  Require(!v10_schema_v9_version.accepted &&
              !v9_schema_v10_version.accepted &&
              !v10_schema_v9_version.diagnostics.empty() &&
              !v9_schema_v10_version.diagnostics.empty() &&
              v10_schema_v9_version.diagnostics.front().code ==
                  "PARSER_SERVER_IPC.STATEMENT_CONTEXT_REQUEST_INVALID" &&
              v9_schema_v10_version.diagnostics.front().code ==
                  "PARSER_SERVER_IPC.STATEMENT_CONTEXT_REQUEST_INVALID",
          "V9/V10 statement-context schema/version collision was admitted");

  const auto mismatched_version = server::HandleAcquireStatementContext(
      &registry, engine_state,
      AcquireNativeFrame(session_uuid,
                         server_transaction.local_transaction_id,
                         transaction_uuid.value.bytes, 6,
                         sbps::kSchemaAcquireStatementContextRequestV7));
  Require(!mismatched_version.accepted &&
              !mismatched_version.diagnostics.empty() &&
              mismatched_version.diagnostics.front().code ==
                  "PARSER_SERVER_IPC.STATEMENT_CONTEXT_REQUEST_INVALID" &&
              registry.statement_contexts_by_statement_uuid.size() == 1,
          "native V7 schema admitted a mismatched version carrier");

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

int main(int argc, char** argv) {
  const bool join_tail_proof_only =
      argc == 2 &&
      std::string_view(argv[1]) == "--join-tail-proof-only";
  const bool table_function_proof_only =
      argc == 2 &&
      std::string_view(argv[1]) == "--table-function-proof-only";
  const bool match_recognize_proof_only =
      argc == 2 &&
      std::string_view(argv[1]) == "--match-recognize-proof-only";
  Require(argc == 1 || join_tail_proof_only || table_function_proof_only ||
              match_recognize_proof_only,
          "unsupported qow live statement-context regression argument");
  if (join_tail_proof_only || table_function_proof_only ||
      match_recognize_proof_only) {
    auto bootstrap_fixture = CreateFixture();
    auto full_route_fixture = CreateFixture(true);
    CreateObjectBackedRelation(&full_route_fixture);
    VerifyFullParserServerRoute(full_route_fixture, join_tail_proof_only,
                                table_function_proof_only,
                                match_recognize_proof_only);
    std::cout
        << (join_tail_proof_only
                ? "qow_join_tail_literal_filter_parameter_limit=passed\n"
                : (table_function_proof_only
                       ? "qow_table_function_generate_series=passed\n"
                       : "qow_match_recognize_generate_series=passed\n"));
    return EXIT_SUCCESS;
  }

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
              first_view.inventory_authoritative &&
              !first_view.statement_timestamp.empty() &&
              first_view.statement_timestamp.back() == 'Z',
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

  auto caller_timestamp = transaction;
  caller_timestamp.statement_timestamp = "2000-01-01T00:00:00Z";
  caller_timestamp.current_timestamp = caller_timestamp.statement_timestamp;
  bridge::StatementContextReceiptView engine_timestamp_view;
  const auto engine_timestamp_receipt =
      Acquire(owner.get(), caller_timestamp, &engine_timestamp_view);
  Require(engine_timestamp_receipt &&
              engine_timestamp_view.statement_timestamp !=
                  caller_timestamp.statement_timestamp &&
              engine_timestamp_view.statement_timestamp.back() == 'Z',
          "caller timestamp substituted the current engine statement clock");
  Require(bridge::ReleaseStatementContextReceipt(engine_timestamp_receipt) ==
              SB_ENGINE_STATUS_OK,
          "engine statement-timestamp proof receipt cleanup failed");

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
  VerifyFullParserServerRoute(full_route_fixture, false);
  std::cout << "qow_live_server_statement_context=passed\n";
  return EXIT_SUCCESS;
}
