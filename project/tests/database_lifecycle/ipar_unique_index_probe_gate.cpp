#include "../support/engine_evidence_fixture.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "memory.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include <memory>
#include "catalog/datatype_bootstrap_identity.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"
#include "ddl/create_api.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "security/security_model.hpp"
#include "time.hpp"
#include <algorithm>
#include "dml/insert_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction/savepoint_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
                << diagnostic.detail << '\n';
    }
    Fail(message);
  }
}

platform::u64 MillisSeed() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::u64 IdentityClockMillis() {
  const auto clock = scratchbird::core::time::ReadLocalNodeClockSnapshot();
  Require(clock.ok(), "IPAR-P7-09 node clock unavailable");
  const auto millis = scratchbird::core::time::WallClockToUuidV7Millis(clock.value.wall_clock);
  Require(millis.ok(), "IPAR-P7-09 UUID clock conversion failed");
  return millis.unix_epoch_millis;
}

platform::TypedUuid NewUuid(platform::UuidKind kind, [[maybe_unused]] platform::u64 salt) {
  // A fixture's steady-clock directory suffix is not a UUID timestamp.
  const auto generated = uuid::GenerateEngineIdentityV7(kind, IdentityClockMillis());
  Require(generated.ok(), "IPAR-P7-09 UUID generation failed");
  return generated.value;
}

api::EngineUuid NewIdentity(platform::UuidKind kind, platform::u64 salt) {
  return NewUuid(kind, salt).value;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && scratchbird::tests::EvidenceTextEquals(item.evidence_id, id)) {
      return true;
    }
  }
  return false;
}

bool HasEvidenceKind(const std::vector<api::EngineEvidenceReference>& evidence,
                     std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind) {
      return true;
    }
  }
  return false;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view needle) {
  const std::string text(needle);
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code.find(text) != std::string::npos ||
        diagnostic.message_key.find(text) != std::string::npos ||
        diagnostic.detail.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void DumpDiagnostics(const api::EngineApiResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
}

class ProbeSession {
 public:
  explicit ProbeSession(const api::EngineRequestContext& context) {
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
  ~ProbeSession() {
    sb_engine_session_end_params_v1_t end{};
    end.struct_size = sizeof(end);
    end.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    end.rollback_active_transactions = 1;
    end.cancel_open_results = 1;
    (void)sb_engine_session_end(session_, &end, nullptr);
    (void)sb_engine_close(engine_, nullptr);
  }
  ProbeSession(const ProbeSession&) = delete;
  ProbeSession& operator=(const ProbeSession&) = delete;
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
      Fail(std::string("unique probe fixture ") + phase + " failed");
  }
 private:
  sb_engine_handle_t engine_ = nullptr;
  sb_engine_session_t session_ = nullptr;
};

class ProbeStatement {
 public:
  ProbeStatement(const ProbeSession& session, const api::EngineRequestContext& base) {
    namespace bridge = scratchbird::server_engine_bridge;
    bridge::StatementContextAcquireRequest request;
    request.engine_context = &base;
    request.exact_transaction_uuid = base.transaction_uuid;
    bridge::StatementContextReceiptView view;
    sb_engine_result_t result = nullptr;
    const auto status = bridge::AcquireStatementContextReceipt(
        session.get(), &request, &receipt_, &view, &result);
    ProbeSession::Check(status, result, "statement acquisition");
    result = nullptr;
    const auto copied = bridge::CopyStatementContextEngineContextV1(
        receipt_, &context, &result);
    ProbeSession::Check(copied, result, "statement context copy");
  }
  ~ProbeStatement() {
    (void)scratchbird::server_engine_bridge::ReleaseStatementContextReceipt(receipt_);
  }
  ProbeStatement(const ProbeStatement&) = delete;
  ProbeStatement& operator=(const ProbeStatement&) = delete;
  api::EngineRequestContext context;
 private:
  scratchbird::server_engine_bridge::StatementContextReceiptHandle receipt_;
};

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  api::EngineUuid database_uuid;
  api::EngineRequestContext owner_context;
  std::shared_ptr<ProbeSession> session;
  api::EngineUuid schema_uuid;
  api::EngineUuid table_uuid;
  api::EngineUuid index_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    session.reset();
    if (!dir.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(dir, ignored);
    }
  }
};

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  typed.state = api::EngineValueState::value;
  return typed;
}

api::EngineRowValue Row(std::string payload) {
  api::EngineRowValue row;
  row.fields.push_back({"payload", TextValue(std::move(payload))});
  return row;
}

api::EngineLocalizedName Name(std::string name) {
  api::EngineLocalizedName value;
  value.language_tag = "en";
  value.name_class = "primary";
  value.name = name;
  value.path = name;
  value.raw_name_text = name;
  value.display_name = name;
  value.default_name = true;
  return value;
}

void CreateTable(Fixture& fixture, const api::EngineRequestContext& context) {
  api::EngineCreateSchemaRequest schema;
  schema.context = context;
  schema.target_object.uuid = fixture.schema_uuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(Name("ipar_unique_probe"));
  RequireOk(api::EngineCreateSchema(schema), "IPAR unique-probe schema publication failed");

  namespace datatypes = scratchbird::core::datatypes;
  const auto manifest = datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  Require(manifest.ok(), "IPAR unique-probe datatype catalog unavailable");
  const std::vector<std::pair<std::string, std::string>> definitions{{"payload", "text"}};
  api::EngineCreateTableRequest request;
  request.context = context;
  request.target_schema.uuid = fixture.schema_uuid;
  request.target_schema.object_kind = "schema";
  request.requested_table_uuid = fixture.table_uuid;
  request.table_names.push_back(Name("ipar_unique_probe_target"));
  for (const auto& [name, type] : definitions) {
    const auto catalog = datatypes::LookupDatatypeCatalogRow(
        manifest.manifest, datatypes::CanonicalTypeIdFromStableName(type));
    Require(catalog.ok() && catalog.manifest.descriptor_rows.size() == 1,
            "IPAR unique-probe datatype binding unavailable");
    const auto& row = catalog.manifest.descriptor_rows.front();
    const auto codec = datatypes::LookupDatatypeTypeCodecIdentityV1(
        context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
        context.datatype_registry_generation, row.descriptor_uuid.value,
        row.descriptor_epoch);
    Require(codec.ok,
            "IPAR unique-probe datatype codec binding unavailable");
    api::EngineColumnDefinition column;
    column.ordinal = request.table_columns.size();
    column.requested_column_uuid = NewIdentity(platform::UuidKind::object, 0);
    column.names.push_back(Name(name));
    column.nullable = true;
    column.descriptor.descriptor_uuid = NewIdentity(platform::UuidKind::object, 0);
    column.descriptor.descriptor_kind = "scalar";
    column.descriptor.canonical_type_name = type;
    column.descriptor.datatype_descriptor_uuid = row.descriptor_uuid.value;
    column.descriptor.datatype_descriptor_generation = row.descriptor_epoch;
    column.descriptor.type_uuid = codec.row.type_uuid;
    column.descriptor.encoded_descriptor = "canonical=" + type +
        (column.nullable ? ";nullable=true" : ";nullable=false");
    request.table_columns.push_back(std::move(column));
  }
  const auto created = api::EngineCreateTable(request);
  RequireOk(created, "IPAR unique-probe table publication failed");
  Require(created.table_object.uuid == fixture.table_uuid,
          "IPAR unique-probe table publication changed native identity");
  const auto descriptor = api::LoadMgaRelationStorageDescriptor(context, fixture.table_uuid);
  Require(descriptor.ok && descriptor.descriptor.relation_uuid == fixture.table_uuid &&
              descriptor.descriptor.relation_generation != 0 &&
              descriptor.descriptor.columns.size() == 1,
          "IPAR unique-probe published column binding absent");
  const auto& column = descriptor.descriptor.columns.front();
  const auto& requested = request.table_columns.front();
  Require(column.column_generation != 0 && column.column_uuid == requested.requested_column_uuid &&
              column.value_descriptor.descriptor_uuid == requested.descriptor.descriptor_uuid &&
              column.value_descriptor.datatype_descriptor_uuid == requested.descriptor.datatype_descriptor_uuid &&
              column.value_descriptor.type_uuid == requested.descriptor.type_uuid,
          "IPAR unique-probe publication changed native column bindings");
}

api::CrudIndexRecord UniquePayloadIndex(const Fixture& fixture,
                                        std::uint64_t creator_tx) {
  api::CrudIndexRecord index;
  index.creator_tx = creator_tx;
  index.index_uuid = fixture.index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = "payload";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.default_name = "ipar_unique_probe_payload_uidx";
  index.unique = true;
  index.key_envelopes.push_back("payload");
  index.key_envelopes.push_back("unique");
  return index;
}

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
  api::EngineRequestContext context = fixture.owner_context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid = fixture.database_uuid;
  context.current_schema_uuid = fixture.schema_uuid;
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok) { DumpDiagnostics(begun); }
  Require(begun.ok, "IPAR unique-probe begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  const auto committed = api::EngineCommitTransaction(request);
  if (!committed.ok) { DumpDiagnostics(committed); }
  Require(committed.ok, "IPAR unique-probe commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  const auto rolled_back = api::EngineRollbackTransaction(request);
  if (!rolled_back.ok) { DumpDiagnostics(rolled_back); }
  Require(rolled_back.ok, "IPAR unique-probe rollback failed");
}

void Reopen(const Fixture& fixture) {
  const auto opened =
      db::OpenDatabaseFile({fixture.database_path.string(), false, false, false});
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << ':'
              << opened.diagnostic.message_key << '\n';
  }
  Require(opened.ok(), "IPAR unique-probe database reopen failed");
}

void CreateSavepoint(const api::EngineRequestContext& context,
                     std::string savepoint_name) {
  api::EngineCreateSavepointRequest request;
  request.context = context;
  request.option_envelopes.push_back("savepoint_name:" + std::move(savepoint_name));
  const auto created = api::EngineCreateSavepoint(request);
  if (!created.ok) { DumpDiagnostics(created); }
  Require(created.ok, "IPAR unique-probe savepoint create failed");
}

void RollbackToSavepoint(const api::EngineRequestContext& context,
                         std::string savepoint_name) {
  api::EngineRollbackToSavepointRequest request;
  request.context = context;
  request.option_envelopes.push_back("savepoint_name:" + std::move(savepoint_name));
  const auto rolled_back = api::EngineRollbackToSavepoint(request);
  if (!rolled_back.ok) { DumpDiagnostics(rolled_back); }
  Require(rolled_back.ok, "IPAR unique-probe savepoint rollback failed");
}

Fixture MakeFixture() {
  Fixture fixture;
  fixture.salt = MillisSeed();
  const auto steady_suffix = static_cast<long long>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_ipar_unique_probe_" +
                 std::to_string(fixture.salt) + "_" +
                 std::to_string(steady_suffix));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "ipar_unique_probe.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, fixture.salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, fixture.salt + 2);
  create.creation_unix_epoch_millis = IdentityClockMillis();
  create.require_resource_seed_pack = true;
  create.resource_seed_pack_root =
      (std::filesystem::path(__FILE__).lexically_normal().parent_path().parent_path()
          .parent_path() / "resources/seed-packs/initial-resource-pack").string();
  create.bootstrap_principal_name = "ipar_unique_probe_owner";
  create.require_bootstrap_principal = true;
  create.allow_uncredentialed_bootstrap = false;
  create.bootstrap_credential_fingerprint =
      "local-password-pbkdf2-sha256:v1:iterations=600000:"
      "salt=0123456789abcdef0123456789abcdef:"
      "verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "IPAR unique-probe database create failed");

  fixture.database_uuid = create.database_uuid.value;
  auto& owner = fixture.owner_context;
  owner.database_uuid = fixture.database_uuid;
  owner.database_path = fixture.database_path.string();
  owner.default_root_uuid = created.state.filespace_uuid.value;
  owner.session_uuid = NewIdentity(platform::UuidKind::object, 0);
  owner.datatype_catalog_snapshot_uuid = api::kBootstrapDatatypeCatalogUuid;
  owner.datatype_catalog_generation = api::kBootstrapDatatypeCatalogGeneration;
  owner.datatype_registry_generation = api::kBootstrapDatatypeRegistryGeneration;
  const auto bootstrap = db::ReadDatabaseBootstrapSecurityCatalog(owner.database_path);
  Require(bootstrap.ok() && bootstrap.state.present && bootstrap.state.committed_by_inventory,
          "IPAR unique-probe durable bootstrap principal unavailable");
  owner.principal_uuid = bootstrap.state.principal_uuid.value;
  const auto loaded = api::LoadSecurityPrincipalLifecycleState(owner);
  Require(loaded.ok, "IPAR unique-probe durable security catalog unavailable");
  const auto& lifecycle = loaded.state;
  api::DurableAuthorizationState authority;
  authority.authority_uuid = owner.database_uuid;
  authority.security_context_generation = lifecycle.security_context_generation;
  authority.security_epoch = lifecycle.security_generation;
  authority.policy_epoch = lifecycle.policy_generation;
  authority.catalog_generation_id = 1;
  authority.engine_owned_sysarch_role_uuid = bootstrap.state.sysarch_role_uuid.value;
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
      {owner.principal_uuid, authority.security_epoch, authority.policy_epoch,
       authority.catalog_generation_id});
  Require(materialized.ok, "IPAR unique-probe durable authorization unavailable");
  owner.authorization_context = materialized.context;

  fixture.schema_uuid = NewIdentity(platform::UuidKind::schema, fixture.salt + 10);
  fixture.table_uuid = NewIdentity(platform::UuidKind::object, fixture.salt + 20);
  fixture.index_uuid = NewIdentity(platform::UuidKind::object, fixture.salt + 21);

  auto metadata = Begin(fixture, "ipar-unique-probe-metadata");
  CreateTable(fixture, metadata);
  const auto index = api::AppendMgaIndexMetadata(
      metadata,
      UniquePayloadIndex(fixture, metadata.local_transaction_id));
  Require(!index.error, "IPAR unique-probe index metadata append failed");
  Commit(metadata);
  fixture.session = std::make_shared<ProbeSession>(BaseContext(fixture, "unique-probe-session"));
  return fixture;
}

api::EngineInsertRowsResult InsertBatchInto(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<std::string> payloads,
    std::vector<std::string> options = {}) {
  ProbeStatement statement(*fixture.session, context);
  api::EngineInsertRowsRequest request;
  request.context = statement.context;
  request.target_schema.uuid = fixture.schema_uuid;
  request.target_table.uuid = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.target_object.uuid = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.estimated_row_count = payloads.size();
  request.input_rows.reserve(payloads.size());
  for (auto& payload : payloads) {
    request.input_rows.push_back(Row(std::move(payload)));
  }
  request.option_envelopes = std::move(options);
  return api::EngineInsertRows(request);
}

api::EngineInsertRowsResult InsertInto(const Fixture& fixture,
                                       const api::EngineRequestContext& context,
                                       std::string payload,
                                       std::vector<std::string> options = {}) {
  return InsertBatchInto(fixture,
                         context,
                         std::vector<std::string>{std::move(payload)},
                         std::move(options));
}

void RequireInsertOk(const api::EngineInsertRowsResult& result,
                     std::string_view message) {
  if (!result.ok) { DumpDiagnostics(result); }
  Require(result.ok, message);
}

void RequireUniqueViolation(const api::EngineInsertRowsResult& result,
                            std::string_view message) {
  Require(!result.ok, message);
  if (!HasDiagnostic(result, "CONSTRAINT_UNIQUE_VIOLATION")) {
    DumpDiagnostics(result);
    Fail("IPAR unique-probe duplicate diagnostic mismatch");
  }
}

void RequireSynchronousUniqueInsertEvidence(
    const api::EngineInsertRowsResult& result) {
  if (HasEvidence(result.evidence,
                  "insert_direct_physical_bulk_route",
                  "selected")) {
    Require(HasEvidence(result.evidence,
                        "bulk_constraint_proof_result",
                        "accepted") &&
                HasEvidence(result.evidence,
                            "bulk_constraint_proof_unique_selected",
                            "true") &&
                HasEvidence(result.evidence,
                            "unique_index_bulk_proof_probes",
                            "1"),
            "IPAR unique-probe direct route lost its accepted unique proof");
    Require(HasEvidence(result.evidence,
                        "insert_hot_append_index_entries",
                        "1"),
            "IPAR unique-probe direct route did not append its unique index");
    Require(HasEvidenceKind(result.evidence, "constraint_proof_store") &&
                HasEvidenceKind(result.evidence, "constraint_proof_hit"),
            "IPAR unique-probe direct route lost its proof-store receipt");
    return;
  }
  Require(HasEvidence(result.evidence,
                      "insert_unique_preflight_path",
                      "index_backed"),
          "IPAR unique-probe insert did not use index-backed preflight");
  Require(HasEvidence(result.evidence,
                      "insert_unique_preflight_route",
                      "accepted"),
          "IPAR unique-probe unique preflight route was not accepted");
  Require(HasEvidence(result.evidence,
                      "insert_unique_authority",
                      "engine_mga"),
          "IPAR unique-probe unique authority was not engine MGA");
  Require(HasEvidence(result.evidence,
                      "unique_index_physical_probe_cache_indexes",
                      "1"),
          "IPAR unique-probe physical cache did not cover one unique index");
  Require(HasEvidence(result.evidence,
                      "unique_index_physical_probe_authority",
                      "candidate_only_mga_visibility_recheck_required"),
          "IPAR unique-probe physical cache authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "insert_hot_append_index_entries",
                      "1"),
          "IPAR unique-probe insert did not synchronously append index entry");
  Require(HasEvidenceKind(result.evidence, "constraint_proof_store"),
          "IPAR unique-probe insert did not store unique preflight proof");
}

void RequirePhysicalProbeEvidence(const api::EngineInsertRowsResult& result,
                                  std::string_view source_kind) {
  if (HasEvidence(result.evidence,
                  "insert_direct_physical_bulk_route",
                  "selected")) {
    Require(HasEvidence(result.evidence,
                        source_kind,
                        "persisted_unique_index"),
            "IPAR unique-probe direct route lost persisted-index source");
    Require(HasEvidence(result.evidence,
                        "bulk_unique_proof_persisted_source",
                        "visible_rows_and_index_entries") &&
                HasEvidence(result.evidence,
                            "bulk_constraint_proof_conflict_reason",
                            "bulk_unique_proof_persisted_conflict"),
            "IPAR unique-probe direct route lost persisted-index proof");
    return;
  }
  Require(HasEvidence(result.evidence,
                      source_kind,
                      "persisted_unique_index_physical_probe"),
          "IPAR unique-probe did not use persisted unique index probe");
  Require(HasEvidence(result.evidence,
                      "physical_unique_index_probe_path",
                      "mga_persisted_index_entry_lookup"),
          "IPAR unique-probe physical lookup path evidence missing");
  Require(HasEvidence(result.evidence,
                      "unique_index_physical_probes",
                      "1"),
          "IPAR unique-probe did not record exactly one physical probe");
  Require(HasEvidence(result.evidence,
                      "unique_index_physical_probe_hits",
                      "1"),
          "IPAR unique-probe did not record physical probe hit");
  Require(HasEvidence(result.evidence,
                      "unique_index_scan_fallbacks",
                      "0"),
          "IPAR unique-probe used scan fallback");
}

void RequireStatementOverlayEvidence(const api::EngineInsertRowsResult& result,
                                     std::string_view source_kind) {
  Require(HasEvidence(result.evidence, source_kind, "statement_delta_overlay"),
          "IPAR unique-probe same-statement duplicate did not use overlay");
  if (HasEvidence(result.evidence,
                  "insert_direct_physical_bulk_route",
                  "selected")) {
    Require(HasEvidence(result.evidence,
                        "bulk_constraint_proof_conflict_reason",
                        "bulk_unique_proof_duplicate_in_batch") &&
                HasEvidence(result.evidence,
                            "bulk_unique_proof_duplicate_run_count",
                            "1") &&
                HasEvidence(result.evidence,
                            "bulk_constraint_proof_refused",
                            "true"),
            "IPAR unique-probe direct route lost duplicate-run proof");
    return;
  }
  Require(HasEvidence(result.evidence,
                      "physical_unique_index_probe_path",
                      "statement_delta_overlay"),
          "IPAR unique-probe same-statement duplicate path mismatch");
  Require(HasEvidence(result.evidence,
                      "unique_index_physical_probe_hits",
                      "0"),
          "IPAR unique-probe same-statement path unexpectedly hit index");
  Require(HasEvidence(result.evidence,
                      "unique_index_scan_fallbacks",
                      "0"),
          "IPAR unique-probe same-statement path used scan fallback");
}

void VerifyDuplicateWithinStatementAndTransaction() {
  auto fixture = MakeFixture();

  auto statement = Begin(fixture, "ipar-unique-probe-same-statement");
  const auto same_statement = InsertBatchInto(
      fixture,
      statement,
      {"same-statement-key", "same-statement-key"});
  RequireUniqueViolation(
      same_statement,
      "IPAR unique-probe same-statement duplicate unexpectedly succeeded");
  RequireStatementOverlayEvidence(same_statement,
                                  "insert_unique_probe_candidate_source");
  Rollback(statement);

  auto after_statement = Begin(fixture, "ipar-unique-probe-after-statement");
  const auto clean_statement_key =
      InsertInto(fixture, after_statement, "same-statement-key");
  RequireInsertOk(clean_statement_key,
                  "IPAR unique-probe statement rollback release insert failed");
  RequireSynchronousUniqueInsertEvidence(clean_statement_key);
  Commit(after_statement);

  auto transaction = Begin(fixture, "ipar-unique-probe-same-transaction");
  const auto first = InsertInto(fixture, transaction, "same-transaction-key");
  RequireInsertOk(first, "IPAR unique-probe same-transaction first insert failed");
  RequireSynchronousUniqueInsertEvidence(first);

  const auto same_transaction =
      InsertInto(fixture, transaction, "same-transaction-key");
  RequireUniqueViolation(
      same_transaction,
      "IPAR unique-probe same-transaction duplicate unexpectedly succeeded");
  RequirePhysicalProbeEvidence(same_transaction,
                               "insert_unique_probe_candidate_source");
  Rollback(transaction);
}

void VerifyRollbackSavepointAndRestartRelease() {
  auto fixture = MakeFixture();

  auto rolled_back = Begin(fixture, "ipar-unique-probe-rollback-source");
  const auto rollback_source = InsertInto(fixture, rolled_back, "rollback-key");
  RequireInsertOk(rollback_source, "IPAR unique-probe rollback source insert failed");
  RequireSynchronousUniqueInsertEvidence(rollback_source);
  Rollback(rolled_back);

  auto released = Begin(fixture, "ipar-unique-probe-rollback-release");
  const auto rollback_release = InsertInto(fixture, released, "rollback-key");
  RequireInsertOk(rollback_release,
                  "IPAR unique-probe rollback did not release uniqueness");
  RequireSynchronousUniqueInsertEvidence(rollback_release);
  Commit(released);

  Reopen(fixture);
  auto restart_duplicate = Begin(fixture, "ipar-unique-probe-restart-duplicate");
  const auto restart_violation = InsertInto(fixture, restart_duplicate, "rollback-key");
  RequireUniqueViolation(
      restart_violation,
      "IPAR unique-probe committed duplicate after restart unexpectedly succeeded");
  RequirePhysicalProbeEvidence(restart_violation,
                               "insert_unique_probe_candidate_source");
  Rollback(restart_duplicate);

  auto savepoint = Begin(fixture, "ipar-unique-probe-savepoint");
  CreateSavepoint(savepoint, "before_unique");
  const auto before_rollback = InsertInto(fixture, savepoint, "savepoint-key");
  RequireInsertOk(before_rollback,
                  "IPAR unique-probe savepoint source insert failed");
  RequireSynchronousUniqueInsertEvidence(before_rollback);
  RollbackToSavepoint(savepoint, "before_unique");

  const auto after_savepoint_rollback =
      InsertInto(fixture, savepoint, "savepoint-key");
  RequireInsertOk(after_savepoint_rollback,
                  "IPAR unique-probe savepoint rollback did not release uniqueness");
  RequireSynchronousUniqueInsertEvidence(after_savepoint_rollback);
  Commit(savepoint);

  Reopen(fixture);
  auto savepoint_duplicate =
      Begin(fixture, "ipar-unique-probe-savepoint-restart-duplicate");
  const auto savepoint_violation =
      InsertInto(fixture, savepoint_duplicate, "savepoint-key");
  RequireUniqueViolation(
      savepoint_violation,
      "IPAR unique-probe savepoint committed duplicate unexpectedly succeeded");
  RequirePhysicalProbeEvidence(savepoint_violation,
                               "insert_unique_probe_candidate_source");
  Rollback(savepoint_duplicate);
}

void VerifyPhysicalUniqueProbe() {
  auto fixture = MakeFixture();

  auto seed = Begin(fixture, "ipar-unique-probe-seed");
  const auto seed_insert = InsertInto(fixture, seed, "dup-key");
  RequireInsertOk(seed_insert, "IPAR unique-probe seed insert failed");
  RequireSynchronousUniqueInsertEvidence(seed_insert);
  Commit(seed);

  Reopen(fixture);
  auto duplicate = Begin(fixture, "ipar-unique-probe-duplicate");
  const auto violation = InsertInto(fixture, duplicate, "dup-key");
  RequireUniqueViolation(violation,
                         "IPAR unique-probe duplicate insert unexpectedly succeeded");
  RequirePhysicalProbeEvidence(violation, "insert_unique_probe_candidate_source");
  Rollback(duplicate);

  auto conflict = Begin(fixture, "ipar-unique-probe-on-conflict");
  const auto do_nothing = InsertInto(
      fixture,
      conflict,
      "dup-key",
      {"on_conflict_action:do_nothing", "conflict_target_column:payload"});
  RequireInsertOk(do_nothing, "IPAR unique-probe ON CONFLICT probe failed");
  Require(do_nothing.skipped_count == 1,
          "IPAR unique-probe ON CONFLICT did not skip duplicate");
  RequirePhysicalProbeEvidence(do_nothing, "on_conflict_match_source");
  Rollback(conflict);
}

}  // namespace

int main() {
  const auto memory = scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
      scratchbird::core::memory::DefaultLocalEngineMemoryPolicy(), "ipar_unique_index_probe");
  Require(memory.ok() && memory.fixture_mode,
          "IPAR unique-probe engine memory fixture configuration failed");

  VerifyDuplicateWithinStatementAndTransaction();
  VerifyRollbackSavepointAndRestartRelease();
  VerifyPhysicalUniqueProbe();
  return EXIT_SUCCESS;
}
