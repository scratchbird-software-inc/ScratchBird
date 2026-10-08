// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "catalog/schema_tree_api.hpp"
#include "catalog/datatype_bootstrap_identity.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"
#include "security/security_model.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include "../database_lifecycle/database_lifecycle_test_memory.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <stdexcept>
#include <utility>

namespace scratchbird::engine::internal_api {
template <typename... Args>
auto CheckedSchemaTreeRecords(Args&&... args) {
  EngineApiDiagnostic diagnostic;
  auto result = VisibleSchemaTreeRecords(std::forward<Args>(args)..., diagnostic);
  if (diagnostic.error) throw std::runtime_error(diagnostic.code + ":" + diagnostic.detail);
  return result;
}
}  // namespace scratchbird::engine::internal_api

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

#ifndef SB_EXAMPLE_SEED_PACK_ROOT
#define SB_EXAMPLE_SEED_PACK_ROOT \
  "project/resources/seed-packs/initial-resource-pack"
#endif

constexpr std::string_view kBenchmarkPassword = "ScratchBird-E2E-2026!";
constexpr std::string_view kRestrictedSchemaPrincipal =
    "schema_connect_only";
constexpr std::string_view kBenchmarkCredentialFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:"
    "verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";
api::EngineUuid seed_filespace_uuid;

void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

std::uint64_t CurrentUnixMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

api::EngineUuid NewUuid(UuidKind kind) {
  static std::uint64_t sequence = 0;
  const auto seed = CurrentUnixMillis() + (++sequence);
  if (kind == UuidKind::session) {
    const auto generated = uuid::GenerateCompatibilityUnixTimeV7(seed);
    if (!generated.ok()) Fail("UUID generation failed");
    return generated.value;
  }
  const auto generated = uuid::GenerateEngineIdentityV7(kind, seed);
  if (!generated.ok()) Fail("UUID generation failed");
  return generated.value.value;
}

api::EngineLocalizedName Name(std::string name) {
  return {"en", "primary", name, name, true};
}

api::EngineColumnDefinition Column(const api::EngineRequestContext& context,
                                  std::uint32_t ordinal,
                                  std::string name,
                                  std::string type) {
  if (type == "bigint") type = "int64";
  namespace datatypes = scratchbird::core::datatypes;
  const auto manifest = datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) Fail("driver fixture datatype catalog unavailable");
  const auto row = datatypes::LookupDatatypeCatalogRow(
      manifest.manifest, datatypes::CanonicalTypeIdFromStableName(type));
  if (!row.ok() || row.manifest.descriptor_rows.size() != 1)
    Fail("driver fixture datatype is not in the engine catalog");
  const auto& catalog = row.manifest.descriptor_rows.front();
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.requested_column_uuid = NewUuid(UuidKind::object);
  column.names.push_back(Name(name));
  column.descriptor.descriptor_uuid = NewUuid(UuidKind::object);
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = std::move(type);
  column.descriptor.datatype_descriptor_uuid = catalog.descriptor_uuid.value;
  column.descriptor.datatype_descriptor_generation = catalog.descriptor_epoch;
  const auto codec = datatypes::LookupDatatypeTypeCodecIdentityV1(
      context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
      context.datatype_registry_generation, catalog.descriptor_uuid.value,
      catalog.descriptor_epoch);
  if (!codec.ok) Fail("driver fixture datatype codec binding unavailable");
  column.descriptor.type_uuid = codec.row.type_uuid;
  column.descriptor.encoded_descriptor =
      "type=" + column.descriptor.canonical_type_name + ";nullable=true";
  return column;
}

api::EngineIndexDefinition CopyStreamUniqueIdIndex() {
  api::EngineIndexDefinition index;
  index.requested_index_uuid = NewUuid(UuidKind::object);
  index.names.push_back(Name("sbsfc021_stream_table_id_unique"));
  index.index_kind = "btree";
  index.key_envelopes.push_back("unique");
  index.key_envelopes.push_back("id");
  return index;
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.descriptor.encoded_descriptor = "type=text";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineTypedValue BigintValue(std::int64_t value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "int64";
  typed.descriptor.encoded_descriptor = "type=int64";
  std::string encoded;
  if (!scratchbird::core::datatypes::EncodeCanonicalInt64Value(
          value, &encoded)) {
    Fail("canonical INT64 seed encoding failed");
  }
  typed.binary_value.assign(encoded.begin(), encoded.end());
  return typed;
}

api::EngineRowValue CopyStreamRow(api::EngineUuid row_uuid,
                                  std::int64_t id,
                                  std::string payload) {
  api::EngineRowValue row;
  row.requested_row_uuid = std::move(row_uuid);
  row.fields.push_back({"id", BigintValue(id)});
  row.fields.push_back({"payload", TextValue(std::move(payload))});
  return row;
}

void RefreshFixtureAuthorization(api::EngineRequestContext& context,
                                 bool require_fresh_catalog) {
  const auto bootstrap = db::ReadDatabaseBootstrapSecurityCatalog(context.database_path);
  if (!bootstrap.ok() || !bootstrap.state.present || !bootstrap.state.committed_by_inventory ||
      context.principal_uuid != bootstrap.state.principal_uuid.value)
    Fail("driver fixture durable bootstrap principal unavailable or mismatched");
  const auto loaded = api::LoadSecurityPrincipalLifecycleState(context);
  if (!loaded.ok) Fail("driver fixture durable security catalog unavailable");
  const auto& lifecycle = loaded.state;
  if (require_fresh_catalog && !lifecycle.row_policies.empty())
    Fail("driver fixture requires a fresh bootstrap security catalog");
  api::DurableAuthorizationState authority;
  authority.authority_uuid = context.database_uuid;
  authority.security_context_generation = lifecycle.security_context_generation;
  authority.security_epoch = lifecycle.security_generation;
  authority.policy_epoch = lifecycle.policy_generation;
  authority.catalog_generation_id = context.catalog_generation_id;
  authority.engine_owned_sysarch_role_uuid = bootstrap.state.sysarch_role_uuid.value;
  context.security_epoch = authority.security_epoch;
  for (const auto& principal : lifecycle.principals)
    if (!principal.deleted && principal.lifecycle_state == "active")
      authority.principals.push_back({principal.principal_uuid, "principal", true,
                                     authority.security_epoch});
  for (const auto& role : lifecycle.roles)
    if (!role.deleted && role.lifecycle_state == "active")
      authority.roles.push_back({role.role_uuid, true, authority.security_epoch});
  for (const auto& membership : lifecycle.memberships)
    if (!membership.revoked)
      authority.memberships.push_back({membership.member_principal_uuid, "principal",
          membership.container_uuid, membership.container_kind, true, authority.security_epoch});
  for (const auto& grant : lifecycle.grants)
    if (!grant.revoked)
      authority.grants.push_back({grant.grant_uuid, grant.grantee_uuid, grant.grantee_kind,
          grant.target_object_uuid, grant.privilege, grant.grant_effect == "deny", true,
          authority.security_epoch});
  const auto materialized = api::MaterializeDurableAuthorizationContext(authority,
      {context.principal_uuid, authority.security_epoch, authority.policy_epoch,
       authority.catalog_generation_id});
  if (!materialized.ok) Fail("driver fixture durable authorization materialization failed");
  context.authorization_context = materialized.context;
}

api::EngineRequestContext BaseContext(const std::filesystem::path& database_path,
                                      const api::EngineUuid& database_uuid) {
  static const api::EngineUuid seeder_session_uuid = NewUuid(UuidKind::session);
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "sbsql-example-database-seed";
  context.database_path = database_path.string();
  context.database_uuid = database_uuid;
  const auto bootstrap = db::ReadDatabaseBootstrapSecurityCatalog(database_path.string());
  if (!bootstrap.ok() || !bootstrap.state.present || !bootstrap.state.committed_by_inventory)
    Fail("example fixture durable bootstrap principal unavailable");
  context.principal_uuid = bootstrap.state.principal_uuid.value;
  context.default_root_uuid = seed_filespace_uuid;
  context.session_uuid = seeder_session_uuid;
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.datatype_catalog_snapshot_uuid =
      api::kBootstrapDatatypeCatalogUuid;
  context.datatype_catalog_generation = api::kBootstrapDatatypeCatalogGeneration;
  context.datatype_registry_generation = api::kBootstrapDatatypeRegistryGeneration;
  context.trace_tags.push_back("sbsql.example_database_seed");
  context.trace_tags.push_back("security.bootstrap");
  context.trace_tags.push_back("group:SEC");
  RefreshFixtureAuthorization(context, true);
  return context;
}

// Fixture construction uses the real internal transaction API. The later
// wire tests, not a validation-only envelope, establish SBLR/IPC execution.
struct SeedTransaction { api::EngineRequestContext context; };

SeedTransaction BeginSeedTransaction(const std::filesystem::path& database_path,
                                    const api::EngineUuid& database_uuid) {
  auto context = BaseContext(database_path, database_uuid);
  api::EngineBeginTransactionRequest begin;
  begin.context = context; begin.operation_id = "transaction.begin";
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  if (!begun.ok || !begun.local_transaction_id || begun.transaction_uuid.is_nil())
    Fail("example fixture transaction begin failed");
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.transaction_isolation_level = begun.isolation_level;
  context.snapshot_visible_through_local_transaction_id = begun.snapshot_visible_through_local_transaction_id;
  return {std::move(context)};
}

void CommitSeedTransaction(const SeedTransaction& transaction) {
  api::EngineCommitTransactionRequest commit;
  commit.context = transaction.context; commit.operation_id = "transaction.commit";
  const auto committed = api::EngineCommitTransaction(commit);
  if (!committed.ok || !committed.engine_finality_known ||
      committed.commit_finality_state != "committed_by_engine_inventory") {
    for (const auto& diagnostic : committed.diagnostics)
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    Fail("example fixture transaction commit failed");
  }
}

api::EngineUuid CreateDatabase(const std::filesystem::path& database_path,
                           const std::string& bootstrap_principal) {
  if (std::filesystem::exists(database_path)) {
    Fail("example database already exists; refusing to seed without the database UUID association");
  }
  const auto now = CurrentUnixMillis();
  const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, now);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, now + 1);
  if (!database_uuid.ok() || !filespace_uuid.ok()) Fail("example database UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = database_path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = now;
  create.resource_seed_pack_root = SB_EXAMPLE_SEED_PACK_ROOT;
  create.allow_minimal_resource_bootstrap = false;
  create.require_resource_seed_pack = true;
  create.bootstrap_principal_name = bootstrap_principal;
  create.bootstrap_credential_fingerprint =
      std::string(kBenchmarkCredentialFingerprint);
  create.require_bootstrap_principal = true;
  create.allow_uncredentialed_bootstrap = false;
  create.allow_overwrite = false;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':' << created.diagnostic.message_key << '\n';
    Fail("example database creation failed");
  }
  seed_filespace_uuid = created.state.filespace_uuid.value;
  return database_uuid.value.value;
}

api::EngineUuid SchemaUuidForPath(const api::EngineRequestContext& context, const std::string& path) {
  for (const auto& schema : api::CheckedSchemaTreeRecords(context, context.local_transaction_id)) {
    for (const auto& name : schema.localized_names) {
      if (name.path == path) { return schema.schema_uuid; }
    }
  }
  return {};
}


void CreateTable(const api::EngineRequestContext& context,
                 api::EngineUuid table_uuid,
                 api::EngineUuid schema_uuid,
                 std::string name) {
  api::EngineCreateTableRequest request;
  request.context = context;
  request.operation_id = "ddl.create_table";
  request.target_schema.uuid = std::move(schema_uuid);
  request.target_schema.object_kind = "schema";
  request.requested_table_uuid = std::move(table_uuid);
  request.table_names.push_back(Name(std::move(name)));
  request.table_columns.push_back(Column(context, 0, "id", "text"));
  request.table_columns.push_back(Column(context, 1, "payload", "text"));
  if (!api::EngineCreateTable(request).ok) {
    Fail("engine-owned benchmark table seed failed");
  }
}

void CreateTableWithColumns(const api::EngineRequestContext& context,
                            api::EngineUuid table_uuid,
                            api::EngineUuid schema_uuid,
                            std::string name,
                            const std::vector<std::pair<std::string, std::string>>& columns) {
  api::EngineCreateTableRequest request;
  request.context = context;
  request.operation_id = "ddl.create_table";
  request.target_schema.uuid = std::move(schema_uuid);
  request.target_schema.object_kind = "schema";
  request.requested_table_uuid = std::move(table_uuid);
  request.table_names.push_back(Name(std::move(name)));
  for (std::uint32_t ordinal = 0; ordinal < columns.size(); ++ordinal) {
    request.table_columns.push_back(
        Column(context, ordinal, columns[ordinal].first, columns[ordinal].second));
  }
  if (!api::EngineCreateTable(request).ok) {
    Fail("engine-owned benchmark table seed failed");
  }
}

api::EngineUuid CreateCopyStreamFixtureTable(const api::EngineRequestContext& context,
                                         const api::EngineUuid& public_schema_uuid) {
  api::EngineUuid table_uuid = NewUuid(UuidKind::object);
  api::EngineCreateTableRequest request;
  request.context = context;
  request.operation_id = "ddl.create_table";
  request.target_schema.uuid = public_schema_uuid;
  request.target_schema.object_kind = "schema";
  request.requested_table_uuid = table_uuid;
  request.table_names.push_back(Name("sbsfc021_stream_table"));
  request.table_columns.push_back(Column(context, 0, "id", "int64"));
  request.table_columns.push_back(Column(context, 1, "payload", "text"));
  request.table_indexes.push_back(CopyStreamUniqueIdIndex());
  if (!api::EngineCreateTable(request).ok) {
    Fail("engine-owned copy-stream table seed failed");
  }
  return table_uuid;
}

void CreateCurrentBenchmarkTables(const api::EngineRequestContext& context,
                                  const api::EngineUuid& public_schema_uuid) {
  CreateTableWithColumns(context,
                         NewUuid(UuidKind::object),
                         public_schema_uuid,
                         "benchmark_customers",
                         {
                             {"id", "bigint"},
                             {"customer_id", "bigint"},
                             {"first_name", "text"},
                             {"last_name", "text"},
                             {"email", "text"},
                             {"phone", "text"},
                             {"registration_date", "text"},
                             {"country_code", "text"},
                             {"account_balance", "bigint"},
                         });
  CreateTableWithColumns(context,
                         NewUuid(UuidKind::object),
                         public_schema_uuid,
                         "benchmark_products",
                         {
                             {"id", "bigint"},
                             {"product_id", "bigint"},
                             {"product_code", "text"},
                             {"name", "text"},
                             {"category", "text"},
                             {"price", "bigint"},
                             {"cost", "bigint"},
                             {"stock_quantity", "bigint"},
                             {"is_active", "bigint"},
                         });
  CreateTableWithColumns(context,
                         NewUuid(UuidKind::object),
                         public_schema_uuid,
                         "benchmark_orders",
                         {
                             {"id", "bigint"},
                             {"order_id", "bigint"},
                             {"customer_id", "bigint"},
                             {"order_date", "text"},
                             {"status", "text"},
                             {"total_amount", "bigint"},
                             {"shipping_cost", "bigint"},
                             {"discount_amount", "bigint"},
                         });
  CreateTableWithColumns(context,
                         NewUuid(UuidKind::object),
                         public_schema_uuid,
                         "benchmark_order_items",
                         {
                             {"id", "bigint"},
                             {"item_id", "bigint"},
                             {"order_id", "bigint"},
                             {"product_id", "bigint"},
                             {"quantity", "bigint"},
                             {"unit_price", "bigint"},
                             {"discount_pct", "bigint"},
                         });
}

void CreateTriggerDefinitionFixtures(
    const api::EngineRequestContext& context,
    const api::EngineUuid& public_schema_uuid) {
  CreateTableWithColumns(context,
                         NewUuid(UuidKind::object),
                         public_schema_uuid,
                         "trig_items",
                         {
                             {"item_id", "bigint"},
                             {"item_name", "text"},
                             {"item_price", "bigint"},
                         });
  CreateTableWithColumns(context,
                         NewUuid(UuidKind::object),
                         public_schema_uuid,
                         "trig_audit",
                         {
                             {"audit_id", "bigint"},
                             {"event_kind", "text"},
                             {"item_id", "bigint"},
                             {"old_price", "bigint"},
                             {"new_price", "bigint"},
                             {"audit_note", "text"},
                         });

  api::EngineCreateSequenceRequest sequence;
  sequence.context = context;
  sequence.operation_id = "ddl.create_sequence";
  sequence.target_schema.uuid = public_schema_uuid;
  sequence.target_schema.object_kind = "schema";
  sequence.target_object.uuid = NewUuid(UuidKind::object);
  sequence.target_object.object_kind = "sequence";
  sequence.localized_names.push_back(Name("trig_audit_seq"));
  sequence.option_envelopes.push_back(
      "sequence_lookup_key:users.public.trig_audit_seq");
  sequence.option_envelopes.push_back("sequence_start_value:1");
  sequence.option_envelopes.push_back("sequence_increment:1");
  const auto created = api::EngineCreateSequence(sequence);
  if (!created.ok) {
    for (const auto& diagnostic : created.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Fail("engine-owned trigger fixture sequence seed failed");
  }
}

// The seeder owns an embedded engine session; statement identities and resource
// snapshots are issued by that engine, never reconstructed by the fixture.
class SeedSession {
 public:
  explicit SeedSession(const api::EngineRequestContext& context) {
    sb_engine_open_params_v1_t open{};
    open.struct_size = sizeof(open);
    open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    open.database_path_utf8 = context.database_path.data();
    open.database_path_size = context.database_path.size();
    open.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
    Check(sb_engine_open(&open, &engine_, nullptr), nullptr, "engine open");
    sb_engine_session_params_v1_t begin{};
    begin.struct_size = sizeof(begin);
    begin.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    std::copy(context.principal_uuid.bytes.begin(), context.principal_uuid.bytes.end(),
              begin.effective_user_uuid.bytes);
    std::copy(context.session_uuid.bytes.begin(), context.session_uuid.bytes.end(),
              begin.session_uuid.bytes);
    begin.default_language_utf8 = "en";
    begin.default_language_size = 2;
    begin.trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
    Check(sb_engine_session_begin(engine_, &begin, &session_, nullptr), nullptr,
          "session begin");
  }
  ~SeedSession() {
    sb_engine_session_end_params_v1_t end{};
    end.struct_size = sizeof(end);
    end.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    end.rollback_active_transactions = 1;
    end.cancel_open_results = 1;
    (void)sb_engine_session_end(session_, &end, nullptr);
    (void)sb_engine_close(engine_, nullptr);
  }
  SeedSession(const SeedSession&) = delete;
  SeedSession& operator=(const SeedSession&) = delete;
  sb_engine_session_t get() const { return session_; }
  static void Check(sb_engine_status_t status, sb_engine_result_t result,
                    const char* phase) {
    if (status != SB_ENGINE_STATUS_OK && result != nullptr) {
      sb_engine_diagnostic_set_view_t diagnostics{};
      if (sb_engine_result_diagnostics(result, &diagnostics) == SB_ENGINE_STATUS_OK) {
        for (std::size_t i = 0; i < diagnostics.diagnostic_count; ++i) {
          const auto& d = diagnostics.diagnostics[i];
          for (const auto text : {d.symbolic_code, d.message_key, d.safe_detail}) {
            if (text.data) std::cerr.write(text.data, text.size_bytes);
            std::cerr << ':';
          }
          std::cerr << '\n';
        }
      }
    }
    if (result) sb_engine_result_release(result);
    if (status != SB_ENGINE_STATUS_OK)
      Fail(std::string("driver fixture ") + phase + " failed");
  }
 private:
  sb_engine_handle_t engine_ = nullptr;
  sb_engine_session_t session_ = nullptr;
};

class SeedStatement {
 public:
  SeedStatement(const SeedSession& session, const api::EngineRequestContext& base) {
    namespace bridge = scratchbird::server_engine_bridge;
    bridge::StatementContextAcquireRequest request;
    request.engine_context = &base;
    request.exact_transaction_uuid = base.transaction_uuid;
    bridge::StatementContextReceiptView view;
    sb_engine_result_t result = nullptr;
    const auto status = bridge::AcquireStatementContextReceipt(
        session.get(), &request, &receipt_, &view, &result);
    SeedSession::Check(status, result, "statement acquisition");
    result = nullptr;
    const auto copied = bridge::CopyStatementContextEngineContextV1(
        receipt_, &context, &result);
    SeedSession::Check(copied, result, "statement context copy");
  }
  ~SeedStatement() {
    (void)scratchbird::server_engine_bridge::ReleaseStatementContextReceipt(receipt_);
  }
  SeedStatement(const SeedStatement&) = delete;
  SeedStatement& operator=(const SeedStatement&) = delete;
  api::EngineRequestContext context;
 private:
  scratchbird::server_engine_bridge::StatementContextReceiptHandle receipt_;
};

void SeedCopyStreamFixtureRow(const SeedSession& session, const api::EngineRequestContext& context,
                              const api::EngineUuid& table_uuid) {
  SeedStatement statement(session, context);
  api::EngineInsertRowsRequest request;
  request.context = statement.context;
  request.operation_id = "dml.insert_rows";
  request.target_table.uuid = table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows.push_back(CopyStreamRow(NewUuid(UuidKind::row),
                                                6,
                                             "stream-baseline"));
  const auto inserted = api::EngineInsertRows(request);
  if (!inserted.ok || inserted.inserted_count != request.input_rows.size()) {
    for (const auto& diagnostic : inserted.diagnostics)
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    Fail("engine-owned copy-stream row seed failed");
  }
}

void SeedChunkedResponseFixtureRows(const SeedSession& session, const api::EngineRequestContext& context,
                                    const api::EngineUuid& table_uuid) {
  constexpr std::size_t kRowCount = 300;
  constexpr std::size_t kPayloadBytes = 3800;
  SeedStatement statement(session, context);
  api::EngineInsertRowsRequest request;
  request.context = statement.context;
  request.operation_id = "dml.insert_rows";
  request.target_table.uuid = table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows.reserve(kRowCount);
  const std::string payload_prefix(kPayloadBytes - 1, 'x');
  for (std::size_t ordinal = 0; ordinal < kRowCount; ++ordinal) {
    api::EngineRowValue row;
    row.requested_row_uuid = NewUuid(UuidKind::row);
    row.fields.push_back(
        {"id", TextValue("chunk-response-" + std::to_string(ordinal + 1))});
    row.fields.push_back(
        {"payload",
         TextValue(payload_prefix +
                   static_cast<char>('a' + (ordinal % 26)))});
    request.input_rows.push_back(std::move(row));
  }
  const auto inserted = api::EngineInsertRows(request);
  if (!inserted.ok || inserted.inserted_count != kRowCount) {
    for (const auto& diagnostic : inserted.diagnostics)
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    Fail("engine-owned chunked-response row seed failed");
  }
}

void SeedUserSchemas(const std::filesystem::path& database_path,
                     const api::EngineUuid& database_uuid) {
  auto transaction = BeginSeedTransaction(database_path, database_uuid);
  const auto& context = transaction.context;

  const api::EngineUuid public_schema_uuid = SchemaUuidForPath(context, "users.public");
  if (public_schema_uuid.is_nil()) Fail("users.public schema UUID was not visible after database create");
  const api::EngineUuid chunked_response_table_uuid = NewUuid(UuidKind::object);
  CreateTable(context,
              chunked_response_table_uuid,
              public_schema_uuid,
              "benchmark_public_items");
  const api::EngineUuid copy_stream_table_uuid = CreateCopyStreamFixtureTable(context, public_schema_uuid);
  CreateCurrentBenchmarkTables(context, public_schema_uuid);
  CreateTriggerDefinitionFixtures(context, public_schema_uuid);
  CommitSeedTransaction(transaction);

  auto security_transaction =
      BeginSeedTransaction(database_path, database_uuid);
  api::EngineSecurityCreatePrincipalRequest restricted_principal;
  restricted_principal.context = security_transaction.context;
  restricted_principal.target_object.uuid =
      NewUuid(UuidKind::principal);
  restricted_principal.target_object.object_kind = "security_principal";
  restricted_principal.principal_uuid =
      restricted_principal.target_object.uuid;
  restricted_principal.principal_name =
      std::string(kRestrictedSchemaPrincipal);
  restricted_principal.credential_fingerprint =
      std::string(kBenchmarkCredentialFingerprint);
  restricted_principal.option_envelopes.push_back(
      "principal_authority:engine");
  const auto created_restricted_principal =
      api::EngineSecurityCreatePrincipal(restricted_principal);
  if (!created_restricted_principal.ok ||
      !created_restricted_principal.principal_created) {
    for (const auto& diagnostic :
         created_restricted_principal.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Fail("engine-owned restricted schema principal seed failed");
  }
  api::EngineSecurityGrantPrivilegeRequest connect_grant;
  RefreshFixtureAuthorization(security_transaction.context, true);
  connect_grant.context = security_transaction.context;
  connect_grant.grantee_uuid = restricted_principal.principal_uuid;
  connect_grant.grantee_kind = "principal";
  connect_grant.privilege = "CONNECT";
  connect_grant.grant_effect = "allow";
  const auto granted_connect =
      api::EngineSecurityGrantPrivilege(connect_grant);
  if (!granted_connect.ok || !granted_connect.privilege_granted) {
    for (const auto& diagnostic : granted_connect.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Fail("engine-owned restricted principal CONNECT grant failed");
  }
  CommitSeedTransaction(security_transaction);

  auto seed_transaction = BeginSeedTransaction(database_path, database_uuid);
  SeedSession session(seed_transaction.context);
  SeedCopyStreamFixtureRow(session, seed_transaction.context, copy_stream_table_uuid);
  SeedChunkedResponseFixtureRows(session, seed_transaction.context,
                                 chunked_response_table_uuid);
  CommitSeedTransaction(seed_transaction);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: sbsql_example_database_seed <database> <user> <password>\n";
    return EXIT_FAILURE;
  }
  const std::filesystem::path database_path = argv[1];
  const std::string user = argv[2];
  const std::string password = argv[3];
  if (password != kBenchmarkPassword) {
    Fail("example database seeder accepts only its fixed production PBKDF2 fixture password");
  }
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture(
      "sbsql_example_database_seed");
  const api::EngineUuid database_uuid = CreateDatabase(database_path, user);
  SeedUserSchemas(database_path, database_uuid);
  std::cout << "sbsql_example_database_seed=passed database=" << database_path
            << " schemas=users,users.public principal=" << user << '\n';
  return EXIT_SUCCESS;
}
