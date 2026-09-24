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
#include "dml/import_execution_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/insert_physical_integration.hpp"
#include "filespace_lifecycle.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "overflow_persistence.hpp"
#include "page_selection.hpp"
#include "strict_bulk_load_lifecycle.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"
#include "time.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace dml = scratchbird::engine::internal_api::dml;
namespace filespace = scratchbird::storage::filespace;
namespace page = scratchbird::storage::page;
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

platform::u64 TimeSeed() {
  return static_cast<platform::u64>(
      std::chrono::steady_clock::now().time_since_epoch().count());
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

api::EngineTypedValue Value(std::string canonical_type, std::string encoded) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = std::move(canonical_type);
  typed.descriptor.encoded_descriptor =
      "canonical=" + typed.descriptor.canonical_type_name;
  typed.encoded_value = std::move(encoded);
  typed.state = api::EngineValueState::value;
  return typed;
}

api::EngineTypedValue NullValue(std::string canonical_type) {
  auto typed = Value(std::move(canonical_type), "");
  typed.setState(api::EngineValueState::sql_null);
  return typed;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && scratchbird::tests::EvidenceTextEquals(item.evidence_id, id)) { return true; }
  }
  return false;
}

bool HasEvidenceKind(const std::vector<api::EngineEvidenceReference>& evidence,
                     std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind) { return true; }
  }
  return false;
}

api::EngineApiU64 EvidenceU64(
    const std::vector<api::EngineEvidenceReference>& evidence,
    std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind != kind) { continue; }
    try {
      return static_cast<api::EngineApiU64>(std::stoull(std::get<std::string>(item.evidence_id)));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

class StressSession {
 public:
  explicit StressSession(const api::EngineRequestContext& context) {
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
  ~StressSession() {
    sb_engine_session_end_params_v1_t end{};
    end.struct_size = sizeof(end);
    end.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    end.rollback_active_transactions = 1;
    end.cancel_open_results = 1;
    (void)sb_engine_session_end(session_, &end, nullptr);
    (void)sb_engine_close(engine_, nullptr);
  }
  StressSession(const StressSession&) = delete;
  StressSession& operator=(const StressSession&) = delete;
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
      Fail(std::string("stress fixture ") + phase + " failed");
  }
 private:
  sb_engine_handle_t engine_ = nullptr;
  sb_engine_session_t session_ = nullptr;
};

class StressStatement {
 public:
  StressStatement(const StressSession& session, const api::EngineRequestContext& base) {
    namespace bridge = scratchbird::server_engine_bridge;
    bridge::StatementContextAcquireRequest request;
    request.engine_context = &base;
    request.exact_transaction_uuid = base.transaction_uuid;
    bridge::StatementContextReceiptView view;
    sb_engine_result_t result = nullptr;
    const auto status = bridge::AcquireStatementContextReceipt(
        session.get(), &request, &receipt_, &view, &result);
    StressSession::Check(status, result, "statement acquisition");
    result = nullptr;
    const auto copied = bridge::CopyStatementContextEngineContextV1(
        receipt_, &context, &result);
    StressSession::Check(copied, result, "statement context copy");
  }
  ~StressStatement() {
    (void)scratchbird::server_engine_bridge::ReleaseStatementContextReceipt(receipt_);
  }
  StressStatement(const StressStatement&) = delete;
  StressStatement& operator=(const StressStatement&) = delete;
  api::EngineRequestContext context;
 private:
  scratchbird::server_engine_bridge::StatementContextReceiptHandle receipt_;
};

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  api::EngineUuid database_uuid;
  api::EngineRequestContext owner_context;
  std::shared_ptr<StressSession> session;
  api::EngineUuid schema_uuid;
  api::EngineUuid table_uuid;
  api::EngineUuid primary_constraint_uuid;
  api::EngineUuid primary_support_uuid;
  std::vector<api::EngineColumnDefinition> published_columns;
  api::EngineUuid id_index_uuid;
  api::EngineUuid sort_index_uuid;
  api::EngineUuid tag_index_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    session.reset();
    if (!dir.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(dir, ignored);
    }
  }
};

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
  RequireOk(begun, "IPAR-P7-09 begin transaction failed");
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
  RequireOk(committed, "IPAR-P7-09 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  const auto rolled_back = api::EngineRollbackTransaction(request);
  RequireOk(rolled_back, "IPAR-P7-09 rollback failed");
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
  schema.localized_names.push_back(Name("ipar_stress"));
  RequireOk(api::EngineCreateSchema(schema), "IPAR-P7-09 schema publication failed");

  namespace datatypes = scratchbird::core::datatypes;
  const auto manifest = datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  Require(manifest.ok(), "IPAR-P7-09 datatype catalog unavailable");
  const std::vector<std::pair<std::string, std::string>> definitions{
      {"id", "text"}, {"shape", "text"}, {"seq_i64", "int64"},
      {"unsigned_u64", "uint64"}, {"decimal_text", "decimal"},
      {"bool_flag", "boolean"}, {"sort_key", "text"}, {"tag", "text"},
      {"payload", "text"}, {"nullable_note", "text"}};
  api::EngineCreateTableRequest request;
  request.context = context;
  request.target_schema.uuid = fixture.schema_uuid;
  request.target_schema.object_kind = "schema";
  request.requested_table_uuid = fixture.table_uuid;
  request.table_names.push_back(Name("ipar_data_shape_stress"));
  for (const auto& [name, type] : definitions) {
    const auto catalog = datatypes::LookupDatatypeCatalogRow(
        manifest.manifest, datatypes::CanonicalTypeIdFromStableName(type));
    Require(catalog.ok() && catalog.manifest.descriptor_rows.size() == 1,
            "IPAR-P7-09 datatype binding unavailable");
    const auto& row = catalog.manifest.descriptor_rows.front();
    const auto codec = datatypes::LookupDatatypeTypeCodecIdentityV1(
        context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
        context.datatype_registry_generation, row.descriptor_uuid.value,
        row.descriptor_epoch);
    // uint64 uses the engine's existing canonical catalog projection; it is
    // not one of the six admitted scalar codec registry rows.
    Require(type == "uint64" || codec.ok,
            "IPAR-P7-09 datatype codec binding unavailable");
    api::EngineColumnDefinition column;
    column.ordinal = request.table_columns.size();
    column.requested_column_uuid = NewIdentity(platform::UuidKind::object, 0);
    column.names.push_back(Name(name));
    column.nullable = name != "id" && name != "shape";
    column.descriptor.descriptor_uuid = NewIdentity(platform::UuidKind::object, 0);
    column.descriptor.descriptor_kind = "scalar";
    column.descriptor.canonical_type_name = type;
    column.descriptor.datatype_descriptor_uuid = row.descriptor_uuid.value;
    column.descriptor.datatype_descriptor_generation = row.descriptor_epoch;
    column.descriptor.type_uuid = type == "uint64"
        ? row.descriptor_uuid.value : codec.row.type_uuid;
    column.descriptor.encoded_descriptor = "canonical=" + type +
        (column.nullable ? ";nullable=true" : ";nullable=false");
    if (name == "id") column.descriptor.encoded_descriptor += ";primary_key=true";
    request.table_columns.push_back(std::move(column));
  }
  const auto created = api::EngineCreateTable(request);
  RequireOk(created, "IPAR-P7-09 table publication failed");
  Require(created.table_object.uuid == fixture.table_uuid,
          "IPAR-P7-09 table publication changed native identity");
  fixture.published_columns = request.table_columns;
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "IPAR-P7-09 published table metadata unavailable");
  const auto found = std::find_if(loaded.state.relation_metadata.tables.begin(),
      loaded.state.relation_metadata.tables.end(), [&](const auto& table) {
        return table.table_uuid == fixture.table_uuid;
      });
  Require(found != loaded.state.relation_metadata.tables.end(),
          "IPAR-P7-09 published table absent");
  api::CatalogColumnMetadata metadata;
  Require(api::DecodeCatalogColumnMetadata(found->columns.front().second, &metadata),
          "IPAR-P7-09 published primary-key metadata is not binary framed");
  Require(metadata.identities.contains("candidate_key_constraint_uuid") &&
              metadata.identities.contains("support_uuid"),
          "IPAR-P7-09 published primary-key identities absent");
  fixture.primary_constraint_uuid = metadata.identities.at("candidate_key_constraint_uuid");
  fixture.primary_support_uuid = metadata.identities.at("support_uuid");

}

api::CrudIndexRecord IndexRecord(const Fixture& fixture,
                                 const api::EngineRequestContext& context,
                                 api::EngineUuid index_uuid,
                                 std::string name,
                                 std::string column,
                                 bool unique) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = std::move(index_uuid);
  index.table_uuid = fixture.table_uuid;
  index.column_name = std::move(column);
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.default_name = std::move(name);
  index.unique = unique;
  index.key_envelopes.push_back(index.column_name);
  if (unique) { index.key_envelopes.push_back("unique"); }
  return index;
}

Fixture MakeFixture(platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_ipar_data_shape_stress_" +
                 std::to_string(salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "ipar_data_shape_stress.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = IdentityClockMillis();
  create.page_size = 8192;
  create.require_resource_seed_pack = true;
  create.resource_seed_pack_root =
      (std::filesystem::path(__FILE__).lexically_normal().parent_path().parent_path()
          .parent_path() / "resources/seed-packs/initial-resource-pack").string();
  create.bootstrap_principal_name = "ipar_stress_owner";
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
  Require(created.ok(), "IPAR-P7-09 database create failed");

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
          "IPAR-P7-09 durable bootstrap principal unavailable");
  owner.principal_uuid = bootstrap.state.principal_uuid.value;
  const auto loaded = api::LoadSecurityPrincipalLifecycleState(owner);
  Require(loaded.ok, "IPAR-P7-09 durable security catalog unavailable");
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
  Require(materialized.ok, "IPAR-P7-09 durable authorization unavailable");
  owner.authorization_context = materialized.context;
  fixture.schema_uuid = NewIdentity(platform::UuidKind::schema, salt + 10);
  fixture.table_uuid = NewIdentity(platform::UuidKind::object, salt + 11);
  fixture.id_index_uuid = NewIdentity(platform::UuidKind::object, salt + 12);
  fixture.sort_index_uuid = NewIdentity(platform::UuidKind::object, salt + 13);
  fixture.tag_index_uuid = NewIdentity(platform::UuidKind::object, salt + 14);

  auto metadata = Begin(fixture, "ipar-p7-09-metadata");
  CreateTable(fixture, metadata);
  const auto id_index = api::AppendMgaIndexMetadata(
      metadata,
      IndexRecord(fixture,
                  metadata,
                  fixture.id_index_uuid,
                  "ipar_data_shape_id_uidx",
                  "id",
                  true));
  Require(!id_index.error, "IPAR-P7-09 id index metadata append failed");
  const auto sort_index = api::AppendMgaIndexMetadata(
      metadata,
      IndexRecord(fixture,
                  metadata,
                  fixture.sort_index_uuid,
                  "ipar_data_shape_sort_idx",
                  "sort_key",
                  false));
  Require(!sort_index.error, "IPAR-P7-09 sort index metadata append failed");
  const auto tag_index = api::AppendMgaIndexMetadata(
      metadata,
      IndexRecord(fixture,
                  metadata,
                  fixture.tag_index_uuid,
                  "ipar_data_shape_tag_idx",
                  "tag",
                  false));
  Require(!tag_index.error, "IPAR-P7-09 tag index metadata append failed");
  Commit(metadata);
  fixture.session = std::make_shared<StressSession>(BaseContext(fixture, "ipar-session"));
  return fixture;
}

std::string FixedWidth(platform::u64 value, std::size_t width) {
  std::string text = std::to_string(value);
  if (text.size() < width) { text.insert(text.begin(), width - text.size(), '0'); }
  return text;
}

api::EngineRowValue Row(std::string id,
                        std::string shape,
                        platform::u64 ordinal,
                        std::string sort_key,
                        std::string tag,
                        std::string payload,
                        bool nullable_note_is_null) {
  api::EngineRowValue row;
  row.fields.push_back({"id", Value("character", std::move(id))});
  row.fields.push_back({"shape", Value("character", std::move(shape))});
  row.fields.push_back({"seq_i64", Value("int64", std::to_string(static_cast<std::int64_t>(ordinal)))});
  row.fields.push_back({"unsigned_u64", Value("uint64", std::to_string(ordinal * 17 + 11))});
  row.fields.push_back({"decimal_text", Value("decimal", std::to_string(ordinal) + ".125")});
  row.fields.push_back({"bool_flag", Value("boolean", (ordinal % 2 == 0) ? "true" : "false")});
  row.fields.push_back({"sort_key", Value("character", std::move(sort_key))});
  row.fields.push_back({"tag", Value("character", std::move(tag))});
  row.fields.push_back({"payload", Value("character", std::move(payload))});
  row.fields.push_back({"nullable_note",
                        nullable_note_is_null
                            ? NullValue("character")
                            : Value("character", "note-" + std::to_string(ordinal))});
  return row;
}

std::vector<api::EngineRowValue> BuildShapeRows(std::string_view shape,
                                                platform::u64 start,
                                                std::size_t count,
                                                std::size_t payload_bytes) {
  std::vector<api::EngineRowValue> rows;
  rows.reserve(count);
  platform::u64 random_state = 0x9e3779b97f4a7c15ull ^ start;
  for (std::size_t index = 0; index < count; ++index) {
    random_state ^= random_state << 7;
    random_state ^= random_state >> 9;
    random_state ^= random_state << 8;
    const platform::u64 ordinal = start + static_cast<platform::u64>(index);
    std::string sort_key;
    std::string tag;
    std::size_t payload_size = payload_bytes;
    bool note_null = false;
    if (shape == "sequential") {
      sort_key = "seq-" + FixedWidth(index, 6);
      tag = "seq-" + std::to_string(index % 16);
    } else if (shape == "random") {
      sort_key = "rnd-" + FixedWidth(random_state % 1000000, 6);
      tag = "rnd-" + std::to_string(random_state % 31);
    } else if (shape == "hot_range") {
      sort_key = "hot-" + FixedWidth(index % 8, 3);
      tag = "hot-range";
    } else if (shape == "duplicate_heavy") {
      sort_key = "dup-" + FixedWidth(index % 4, 3);
      tag = "same-tag";
    } else if (shape == "null_heavy") {
      sort_key = "null-" + FixedWidth(index % 9, 3);
      tag = (index % 3 == 0) ? "nullable-a" : "nullable-b";
      note_null = index % 2 == 0;
      payload_size = (index % 4 == 0) ? 0 : payload_bytes;
    } else {
      sort_key = std::string(shape) + "-" + FixedWidth(index, 5);
      tag = std::string(shape) + "-" + std::to_string(index % 11);
    }
    rows.push_back(Row(std::string(shape) + "-" + FixedWidth(ordinal, 8),
                       std::string(shape),
                       ordinal,
                       std::move(sort_key),
                       std::move(tag),
                       std::string(payload_size,
                                   static_cast<char>('A' + (ordinal % 26))),
                       note_null));
  }
  return rows;
}

std::vector<std::string> DemandOptions() {
  return {"page_allocation.runtime=enabled",
          "dml_demand_hints=enabled",
          "dml_demand_hints.max_pages=512",
          "dml_demand_hints.available_capacity_pages=512",
          "page_allocation.row_pages_per_mutation=128",
          "page_allocation.index_pages_per_mutation=128",
          "page_allocation.preallocate_index_pages=128"};
}

api::EngineInsertRowsRequest InsertRequest(
    const Fixture& fixture,
    api::EngineRequestContext context,
    std::string request_id,
    std::vector<api::EngineRowValue> rows) {
  api::EngineInsertRowsRequest request;
  request.context = std::move(context);
  request.context.request_id = std::move(request_id);
  request.target_schema.uuid = fixture.schema_uuid;
  request.target_table.uuid = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.target_object.uuid = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.bound_object_identity.object_uuid = request.target_table.uuid;
  request.bound_object_identity.catalog_generation_id =
      request.context.catalog_generation_id;
  request.bound_object_identity.security_epoch = request.context.security_epoch;
  request.bound_object_identity.resource_epoch = request.context.resource_epoch;
  request.estimated_row_count = static_cast<api::EngineApiU64>(rows.size());
  request.input_rows = std::move(rows);
  request.option_envelopes = DemandOptions();
  return request;
}

api::EngineExecuteImportRowsRequest CopyRequest(
    const Fixture& fixture,
    api::EngineRequestContext context,
    std::string request_id,
    std::vector<api::EngineRowValue> rows) {
  api::EngineExecuteImportRowsRequest request;
  request.context = std::move(context);
  request.context.request_id = std::move(request_id);
  request.target_schema.uuid = fixture.schema_uuid;
  request.target_table.uuid = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.target_object.uuid = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.source.source_kind = "csv_stream";
  request.source.source_position = "row:0";
  request.format.format_family = "csv";
  request.import_policy.reject_mode = "fail_fast";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  request.canonical_rows = std::move(rows);
  request.estimated_row_count =
      static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.option_envelopes = DemandOptions();
  request.option_envelopes.push_back("copy_append_batching=enabled");
  request.option_envelopes.push_back("copy_append_batch_rows=16");
  return request;
}

void VerifyDmlEvidence(const api::EngineApiResult& result,
                       bool require_large_value) {
  Require(result.dml_summary.rows_changed > 0,
          "IPAR-P7-09 DML summary did not record changed rows");
  Require(result.dml_summary.index_probes > 0,
          "IPAR-P7-09 indexed shape did not record index probes");
  Require(result.dml_summary.preallocation_granted_pages > 0,
          "IPAR-P7-09 shape did not receive preallocated pages");
  Require(HasEvidence(result.evidence,
                      "dml_demand_hint_decision",
                      "accepted"),
          "IPAR-P7-09 demand hint was not accepted");
  if (require_large_value) {
    Require(HasEvidence(result.evidence,
                        "mga_large_value_batch_writer",
                        "window"),
            "IPAR-P7-09 large-value shape did not use batch writer");
    Require(EvidenceU64(result.evidence,
                        "mga_large_value_batch_chunks") > 0,
            "IPAR-P7-09 large-value shape emitted no chunk evidence");
  }
}

void InsertShape(Fixture& fixture,
                 std::string_view shape,
                 platform::u64 start,
                 std::size_t count,
                 std::size_t payload_bytes,
                 bool require_large_value,
                 api::EngineApiU64* expected_rows) {
  auto context = Begin(fixture, "ipar-p7-09-" + std::string(shape));
  StressStatement statement(*fixture.session, context);
  auto request = InsertRequest(fixture,
                               statement.context,
                               "ipar-p7-09-insert-" + std::string(shape),
                               BuildShapeRows(shape, start, count, payload_bytes));
  const auto inserted = api::EngineInsertRows(request);
  RequireOk(inserted, "IPAR-P7-09 shape insert failed");
  Require(inserted.inserted_count == count,
          "IPAR-P7-09 shape inserted row count mismatch");
  VerifyDmlEvidence(inserted, require_large_value);
  Commit(context);
  *expected_rows += static_cast<api::EngineApiU64>(count);
}

void CopyShape(Fixture& fixture,
               std::string_view shape,
               platform::u64 start,
               std::size_t count,
               api::EngineApiU64* expected_rows) {
  auto context = Begin(fixture, "ipar-p7-09-copy-" + std::string(shape));
  StressStatement statement(*fixture.session, context);
  const auto copied = api::EngineExecuteImportRows(CopyRequest(
      fixture,
      statement.context,
      "ipar-p7-09-copy-request-" + std::string(shape),
      BuildShapeRows(shape, start, count, 96)));
  RequireOk(copied, "IPAR-P7-09 COPY shape failed");
  Require(copied.accepted_rows == count && copied.inserted_rows == count,
          "IPAR-P7-09 COPY shape row count mismatch");
  Require(HasEvidence(copied.evidence, "import_execution", "direct_physical"),
          "IPAR-P7-09 COPY shape did not use direct physical lane");
  VerifyDmlEvidence(copied, false);
  Commit(context);
  *expected_rows += static_cast<api::EngineApiU64>(count);
}

void VerifyDuplicateUniqueRefusal(Fixture& fixture) {
  auto seed_context = Begin(fixture, "ipar-p7-09-duplicate-seed");
  StressStatement seed_statement(*fixture.session, seed_context);
  auto seed_request = InsertRequest(
      fixture,
      seed_statement.context,
      "ipar-p7-09-duplicate-seed-request",
      {Row("duplicate-unique-key",
           "duplicate_unique",
           880000,
           "dup-key",
           "duplicate",
           "seed",
           false)});
  const auto seeded = api::EngineInsertRows(seed_request);
  RequireOk(seeded, "IPAR-P7-09 duplicate seed insert failed");
  Commit(seed_context);

  const auto reopened =
      db::OpenDatabaseFile({fixture.database_path.string(), false, false, false});
  if (!reopened.ok()) {
    std::cerr << reopened.diagnostic.diagnostic_code << ':'
              << reopened.diagnostic.message_key << '\n';
  }
  Require(reopened.ok(), "IPAR-P7-09 duplicate probe reopen failed");

  auto duplicate_context = Begin(fixture, "ipar-p7-09-duplicate-refusal");
  StressStatement duplicate_statement(*fixture.session, duplicate_context);
  auto duplicate_request = InsertRequest(
      fixture,
      duplicate_statement.context,
      "ipar-p7-09-duplicate-refusal-request",
      {Row("duplicate-unique-key",
           "duplicate_unique",
           880001,
           "dup-key",
           "duplicate",
           "duplicate",
           false)});
  const auto duplicate = api::EngineInsertRows(duplicate_request);
  Require(!duplicate.ok,
          "IPAR-P7-09 duplicate unique key was unexpectedly admitted");
  Require(HasEvidence(duplicate.evidence,
                      "insert_unique_probe_candidate_source",
                      "persisted_unique_index"),
          "IPAR-P7-09 duplicate path lost its persisted-index source");
  Require(HasEvidence(duplicate.evidence,
                      "bulk_unique_proof_persisted_source",
                      "visible_rows_and_index_entries") &&
              HasEvidence(duplicate.evidence,
                          "bulk_constraint_proof_conflict_reason",
                          "bulk_unique_proof_persisted_conflict"),
          "IPAR-P7-09 duplicate path lost its persisted-index proof");
  bool exact_constraint = false;
  bool exact_support = false;
  for (const auto& evidence : duplicate.evidence) {
    const auto* identity = std::get_if<api::EngineUuid>(&evidence.evidence_id);
    if (evidence.evidence_kind == "bulk_unique_proof_conflict_constraint")
      exact_constraint = identity && *identity == fixture.primary_constraint_uuid;
    if (evidence.evidence_kind == "bulk_unique_proof_index" && identity &&
        *identity == fixture.primary_support_uuid) exact_support = true;
  }
  Require(exact_constraint && exact_support,
          "IPAR-P7-09 unique proof did not preserve published native identities");
  Rollback(duplicate_context);
}

// Inject a malformed native metadata successor in a transaction, then prove
// that the existing sealed-descriptor guard refuses it before bulk proof and
// rollback restores the committed cohort. The fixture does not reseal corruption.
void VerifyUnsealedCandidateMutationRefusal(Fixture& fixture) {
  for (const std::string missing_field : {"candidate_key_constraint_uuid", "support_uuid"}) {
  auto context = Begin(fixture, "ipar-p7-09-missing-" + missing_field);
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "IPAR-P7-09 negative metadata load failed");
  const auto found = std::find_if(loaded.state.relation_metadata.tables.begin(),
      loaded.state.relation_metadata.tables.end(), [&](const auto& table) {
        return table.table_uuid == fixture.table_uuid;
      });
  Require(found != loaded.state.relation_metadata.tables.end(),
          "IPAR-P7-09 negative metadata target absent");
  auto malformed = *found;
  const auto persisted = api::LoadMgaRelationStorageDescriptor(context, fixture.table_uuid);
  Require(persisted.ok && !persisted.descriptor.columns.empty(),
          "IPAR-P7-09 negative fixture storage descriptor unavailable");
  malformed.bound_relation_generation = persisted.descriptor.relation_generation;
  malformed.bound_column_generation = persisted.descriptor.columns.front().column_generation;
  malformed.bound_columns.clear();
  for (const auto& stored : persisted.descriptor.columns) {
    Require(stored.column_generation == malformed.bound_column_generation,
            "IPAR-P7-09 negative fixture column generation mismatch");
    api::EngineColumnDefinition bound;
    bound.requested_column_uuid = stored.column_uuid;
    bound.ordinal = stored.ordinal;
    bound.descriptor = stored.value_descriptor;
    bound.nullable = stored.nullable;
    bound.names.push_back(Name(stored.canonical_name_key));
    malformed.bound_columns.push_back(std::move(bound));
  }
  api::CatalogColumnMetadata metadata;
  Require(api::DecodeCatalogColumnMetadata(malformed.columns.front().second, &metadata),
          "IPAR-P7-09 negative metadata decode failed");
  Require(metadata.identities.erase(missing_field) == 1,
          "IPAR-P7-09 negative fixture did not remove candidate-key authority");
  // A generic constraint identity must not replace the missing candidate key.
  metadata.identities["constraint_uuid"] = NewIdentity(platform::UuidKind::object, 0);
  Require(api::EncodeCatalogColumnMetadata(metadata, &malformed.columns.front().second),
          "IPAR-P7-09 negative metadata encode failed");
  malformed.creator_tx = context.local_transaction_id;
  const auto appended = api::AppendMgaTableMetadata(context, malformed);
  Require(!appended.error, "IPAR-P7-09 negative metadata staging failed");
  {
    StressStatement statement(*fixture.session, context);
    const auto result = api::EngineInsertRows(InsertRequest(fixture, statement.context,
        "ipar-p7-09-missing-candidate-request",
        {Row("missing-candidate-key", "missing_candidate", 990000,
             "missing-key", "negative", "payload", false)}));
    bool identity_refusal = false;
    for (const auto& diagnostic : result.diagnostics)
      identity_refusal |= diagnostic.code == "SB_ENGINE_API_INVALID_REQUEST" &&
          diagnostic.detail == "mga.relation_descriptor:sealed_relation_descriptor_snapshot_conflict";
    if (result.ok || !identity_refusal) {
      std::cerr << "negative_metadata_field=" << missing_field << " result_ok=" << result.ok << '\n';
      for (const auto& diagnostic : result.diagnostics)
        std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Require(!result.ok && identity_refusal,
            "IPAR-P7-09 unsealed candidate mutation bypassed the storage descriptor authority");
  }
  Rollback(context);
  }
}

void VerifyCommittedRowsAfterReopen(const Fixture& fixture,
                                    api::EngineApiU64 expected_rows) {
  const auto opened =
      db::OpenDatabaseFile({fixture.database_path.string(), false, false, false});
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << ':'
              << opened.diagnostic.message_key << '\n';
  }
  Require(opened.ok(), "IPAR-P7-09 reopen failed");

  auto context = Begin(fixture, "ipar-p7-09-readback");
  const auto loaded = api::LoadMgaRelationStoreState(context);
  if (!loaded.ok) {
    std::cerr << loaded.diagnostic.code << ':' << loaded.diagnostic.message_key
              << ':' << loaded.diagnostic.detail << '\n';
  }
  Require(loaded.ok, "IPAR-P7-09 relation-state load failed");
  const auto persisted = api::LoadMgaRelationStorageDescriptor(context, fixture.table_uuid);
  Require(persisted.ok && persisted.descriptor.relation_uuid == fixture.table_uuid &&
              persisted.descriptor.relation_generation != 0 &&
              persisted.descriptor.columns.size() == fixture.published_columns.size(),
          "IPAR-P7-09 reopened storage descriptor lost its published bound cohort");
  for (std::size_t i = 0; i < fixture.published_columns.size(); ++i) {
    const auto& expected = fixture.published_columns[i];
    const auto& actual = persisted.descriptor.columns[i];
    Require(actual.column_generation != 0 &&
                actual.column_uuid == expected.requested_column_uuid &&
                actual.value_descriptor.descriptor_uuid == expected.descriptor.descriptor_uuid &&
                actual.value_descriptor.datatype_descriptor_uuid == expected.descriptor.datatype_descriptor_uuid &&
                actual.value_descriptor.type_uuid == expected.descriptor.type_uuid,
            "IPAR-P7-09 reopen changed a native column or datatype identity");
  }
  api::EngineApiU64 visible_rows = 0;
  api::EngineApiU64 index_entries = 0;
  for (const auto& row : loaded.state.row_versions) {
    if (row.table_uuid == fixture.table_uuid && !row.deleted) {
      ++visible_rows;
    }
  }
  for (const auto& entry : loaded.state.index_entries) {
    if (entry.table_uuid == fixture.table_uuid) { ++index_entries; }
  }
  Require(visible_rows == expected_rows,
          "IPAR-P7-09 committed data-shape row count mismatch after reopen");
  Require(index_entries >= expected_rows,
          "IPAR-P7-09 committed data-shape index entries missing after reopen");
  Rollback(context);
}

void RegisterCandidate(page::PageSelectionLedger* ledger,
                       platform::TypedUuid database_uuid,
                       platform::TypedUuid filespace_uuid,
                       platform::TypedUuid object_uuid,
                       std::string page_family,
                       platform::u64 seed) {
  page::InsertPageCandidate candidate;
  candidate.database_uuid = database_uuid;
  candidate.filespace_uuid = filespace_uuid;
  candidate.object_uuid = object_uuid;
  candidate.page_uuid = NewUuid(platform::UuidKind::page, seed + 5000);
  candidate.page_family = std::move(page_family);
  candidate.page_number = 100 + seed;
  candidate.page_generation = 1;
  candidate.free_bytes = 8192;
  const auto registered = page::RegisterInsertPageCandidate(ledger, candidate);
  Require(registered.ok(), "IPAR-P7-09 page candidate registration failed");
}

filespace::FilespaceDescriptor Descriptor(platform::TypedUuid database_uuid,
                                          platform::TypedUuid filespace_uuid,
                                          filespace::FilespaceRole role,
                                          platform::u64 seed) {
  filespace::FilespaceDescriptor descriptor;
  descriptor.database_uuid = database_uuid;
  descriptor.filespace_uuid = filespace_uuid;
  descriptor.path = "ipar-p7-09-filespace-" + std::to_string(seed) + ".sbfs";
  descriptor.role = role;
  descriptor.state = filespace::FilespaceState::online;
  descriptor.page_size = 8192;
  descriptor.generation = 10 + seed;
  descriptor.read_only = false;
  descriptor.active = true;
  descriptor.physical_filespace_id = static_cast<platform::u16>(seed);
  descriptor.total_pages = 256;
  descriptor.free_pages = 128;
  descriptor.preallocated_pages = 8;
  descriptor.allocation_root_page = 1;
  descriptor.header_generation = 1;
  descriptor.writer_identity_uuid =
      NewUuid(platform::UuidKind::object, seed + 6000);
  return descriptor;
}

void VerifyMixedFilespacePlacementShape() {
  const auto database_uuid = NewUuid(platform::UuidKind::database, 900001);
  const auto transaction_uuid = NewUuid(platform::UuidKind::transaction, 900002);
  const auto object_uuid = NewUuid(platform::UuidKind::object, 900003);
  const auto data_filespace_uuid = NewUuid(platform::UuidKind::filespace, 900010);
  const auto index_filespace_uuid = NewUuid(platform::UuidKind::filespace, 900011);

  filespace::FilespaceRegistry registry;
  registry.filespaces.push_back(Descriptor(database_uuid,
                                           data_filespace_uuid,
                                           filespace::FilespaceRole::secondary_data,
                                           1));
  registry.filespaces.push_back(Descriptor(database_uuid,
                                           index_filespace_uuid,
                                           filespace::FilespaceRole::secondary_index,
                                           2));

  filespace::FilespacePlacementPolicy policy;
  policy.present = true;
  policy.require_explicit_binding = true;
  policy.default_preallocate_page_count = 2;
  policy.bindings.push_back({filespace::FilespaceObjectClass::exact_index,
                             "index",
                             index_filespace_uuid,
                             true,
                             4});

  page::PageReservationLedger reservation_ledger;
  page::PageSelectionLedger selection_ledger;
  filespace::FilespaceGrowthLedger growth_ledger;
  scratchbird::storage::page::OverflowLedger overflow_ledger;
  scratchbird::core::bulk_load::StrictBulkLoadLedger strict_bulk_ledger;
  dml::InsertPhysicalIntegrationContext context;
  context.page_reservation_ledger = &reservation_ledger;
  context.page_selection_ledger = &selection_ledger;
  context.filespace_growth_ledger = &growth_ledger;
  context.filespace_registry = &registry;
  context.overflow_ledger = &overflow_ledger;
  context.strict_bulk_load_ledger = &strict_bulk_ledger;

  RegisterCandidate(&selection_ledger,
                    database_uuid,
                    data_filespace_uuid,
                    object_uuid,
                    "index",
                    1);
  RegisterCandidate(&selection_ledger,
                    database_uuid,
                    index_filespace_uuid,
                    object_uuid,
                    "index",
                    2);

  dml::InsertPhysicalIntegrationRequest request;
  request.database_uuid = database_uuid;
  request.object_uuid = object_uuid;
  request.transaction_uuid = transaction_uuid;
  request.local_transaction_id = 990001;
  request.policy_uuid = NewUuid(platform::UuidKind::object, 900020);
  request.request_id = NewUuid(platform::UuidKind::object, 900021);
  request.page_family = "index";
  request.estimated_row_count = 96;
  request.estimated_payload_bytes = 8192;
  request.encoded_row_bytes = 128;
  request.page_size = 8192;
  request.placement_object_class = filespace::FilespaceObjectClass::exact_index;
  request.placement_policy = policy;
  request.require_placement_policy = true;
  request.require_placement_preallocation = true;
  request.placement_preallocation_pages = 4;

  const auto placed = dml::ExecuteInsertPhysicalIntegration(&context, request);
  Require(placed.ok(), "IPAR-P7-09 mixed-filespace placement failed");
  Require(placed.filespace_placement_resolved,
          "IPAR-P7-09 mixed-filespace placement was not resolved");
  Require(placed.resolved_filespace_uuid.value == index_filespace_uuid.value,
          "IPAR-P7-09 mixed-filespace placement selected wrong filespace");
  Require(selection_ledger.selections.size() == 1 &&
              selection_ledger.selections.front().filespace_uuid.value ==
                  index_filespace_uuid.value,
          "IPAR-P7-09 mixed-filespace selection ignored reservation filespace");
  Require(growth_ledger.preallocation_operations.size() == 1 &&
              growth_ledger.preallocation_operations.front().filespace_uuid.value ==
                  index_filespace_uuid.value,
          "IPAR-P7-09 mixed-filespace preallocation targeted wrong filespace");
}

void VerifyDataShapeStressMatrix() {
  auto fixture = MakeFixture(TimeSeed());
  api::EngineApiU64 expected_rows = 0;

  InsertShape(fixture, "narrow", 1000, 64, 16, false, &expected_rows);
  InsertShape(fixture, "wide", 10000, 24, 2048, false, &expected_rows);
  InsertShape(fixture, "null_heavy", 20000, 40, 48, false, &expected_rows);
  InsertShape(fixture, "large_value", 30000, 12, 10000, true, &expected_rows);
  InsertShape(fixture, "sequential", 40000, 128, 64, false, &expected_rows);
  InsertShape(fixture, "random", 50000, 128, 64, false, &expected_rows);
  InsertShape(fixture, "duplicate_heavy", 60000, 128, 80, false, &expected_rows);
  InsertShape(fixture, "hot_range", 70000, 96, 64, false, &expected_rows);
  CopyShape(fixture, "copy_native", 80000, 40, &expected_rows);

  VerifyDuplicateUniqueRefusal(fixture);
  ++expected_rows;
  VerifyUnsealedCandidateMutationRefusal(fixture);
  VerifyCommittedRowsAfterReopen(fixture, expected_rows);
  VerifyMixedFilespacePlacementShape();
}

}  // namespace

int main() {
  const auto memory = scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
      scratchbird::core::memory::DefaultLocalEngineMemoryPolicy(), "ipar_data_shape_stress");
  Require(memory.ok() && memory.fixture_mode,
          "IPAR-P7-09 engine memory fixture configuration failed");
  VerifyDataShapeStressMatrix();
  std::cout << "ipar_data_shape_stress_matrix_gate=passed\n";
  return EXIT_SUCCESS;
}
