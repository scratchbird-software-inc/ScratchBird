// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/schema_tree_api.hpp"
#include "database_lifecycle.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "hash_digest.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "security/security_model.hpp"
#include <algorithm>
#include "memory.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace core_hash = scratchbird::core::hash;
namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace resources = scratchbird::core::resources;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr scratchbird::core::platform::u64 kDriverFixtureCreationMillis = 1767225600000ull;
constexpr scratchbird::core::platform::u32 kDriverFixturePageSize = 16384;
constexpr std::string_view kAliceBootstrapCredentialFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:"
    "verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";
constexpr std::string_view kAppSchemaUuid = "018f0a2b-0000-7000-9000-000000000100";
constexpr std::string_view kCustomersRelationUuid =
    "018f0a2b-0000-7000-9000-000000000101";
constexpr std::string_view kSecurityAlterPolicyFixtureUuid =
    "018f0a2b-0000-7000-9000-000000000701";
constexpr std::string_view kSecurityAlterPolicyStatementUuid =
    "018f0a2b-0000-7000-9000-000000000702";
constexpr std::string_view kSecurityAlterPolicyStatementSnapshotUuid =
    "018f0a2b-0000-7000-9000-000000000703";
constexpr std::string_view kSecurityAlterPolicyStatementReceiptUuid =
    "018f0a2b-0000-7000-9000-000000000704";
constexpr std::string_view kSecurityAlterPolicyMetadataSnapshotUuid =
    "018f0a2b-0000-7000-9000-000000000705";
constexpr std::string_view kSecurityAlterPolicyVersionUuid =
    "018f0a2b-0000-7000-9000-000000000706";
constexpr std::string_view kSecurityAlterPolicyEffectiveUuid =
    "018f0a2b-0000-7000-9000-000000000707";
constexpr std::string_view kSecurityAlterPolicyEffectiveExpressionUuid =
    "018f0a2b-0000-7000-9000-000000000708";
constexpr std::string_view kSecurityAlterPolicySourceExpressionUuid =
    "018f0a2b-0000-7000-9000-000000000709";
constexpr std::string_view kDatatypeCatalogSnapshotUuid =
    "019d0000-0000-7000-8000-00000000d701";

struct CreatedDatabaseFixture {
  api::EngineUuid database_uuid;
  db::DatabaseLifecycleState state;
};

const std::vector<std::string>& SysarchRights() {
  static const std::vector<std::string> rights = {
      "CONNECT",
      "VISIBLE",
      "DISCOVER",
      "LIST_CHILD",
      "SELECT",
      "INSERT",
      "UPDATE",
      "DELETE",
      "EXECUTE",
      "CREATE",
      "ALTER",
      "DROP",
      "DOMAIN_USE",
      "DOMAIN_CAST",
      "DOMAIN_METHOD",
      "DOMAIN_POLICY_ADMIN",
      "DOMAIN_UNMASK",
      "UNMASK",
      "POLICY_ADMIN",
      "OBS_METRICS_READ_SELF",
      "OBS_METRICS_READ_ALL",
      "OBS_METRICS_READ_FAMILY",
      "OBS_METRICS_READ_DATABASE",
      "OBS_METRICS_READ_NODE",
      "OBS_METRICS_READ_CLUSTER",
      "OBS_METRICS_EXPORT",
      "OBS_METRICS_EXPORT_READ",
      "OBS_METRICS_EXPORT_CONTROL",
      "OBS_METRICS_RETENTION_CONTROL",
      "SEC_AUTH_METRICS_READ",
      "OBS_POLICY_READ",
      "OBS_RUNTIME_ALL",
      "OBS_RUNTIME_SELF",
      "OBS_INDEX_PROFILE_READ",
      "OBS_MANAGEMENT_INSPECT",
      "OBS_MANAGEMENT_CONTROL",
      "OBS_CONFIG_INSPECT",
      "OBS_CONFIG_CONTROL",
      "OBS_CLUSTER_HEALTH_INSPECT",
      "OBS_CLUSTER_CONTROL",
      "OBS_DATA_MOVEMENT_INSPECT",
      "OBS_AGENT_STATE_READ",
      "OBS_AGENT_EVIDENCE_READ",
      "OBS_AGENT_CONTROL",
      "OBS_AGENT_POLICY_CONTROL",
      "OBS_AGENT_OVERRIDE",
      "OBS_AGENT_RECOMMENDATION_READ",
      "OBS_POLICY_VALIDATE",
      "OBS_METRICS_POLICY_INSPECT",
      "OBS_METRICS_POLICY_CONTROL",
      "SEC_IDENTITY_ADMIN",
      "SEC_MEMBERSHIP_ADMIN",
      "SEC_GRANT_ADMIN",
      "MGA_TRANSACTION_INSPECT",
      "MGA_RECOVERY_INSPECT",
      "MGA_CLEANUP_INSPECT",
      "MGA_CLEANUP_CONTROL",
      "MGA_HORIZON_INSPECT",
      "MGA_LINEAGE_INSPECT",
      "MGA_FORENSIC_INSPECT",
      "MGA_METRICS_READ",
      "AUTH_PROVIDER_ADMIN",
      "AUDIT_READ",
      "AUDIT_ADMIN",
      "PROTECTED_MATERIAL_RELEASE",
      "KEY_RELEASE_APPROVE",
      "SUPPORT_EXPORT",
      "UDR_TRUST_ADMIN",
      "UDR_MANAGE",
      "UDR_INSPECT",
      "UDR_INVOKE",
      "FILESPACE_LIFECYCLE_CONTROL",
      "MIGRATE_DATABASE",
      "BACKUP_CREATE",
      "BACKUP_RESTORE",
      "BACKUP_CONTROL",
      "BACKUP_INSPECT",
      "SYS_BACKUP",
      "EVENT_ADMIN",
      "EVENT_CREATE",
      "EVENT_ALTER",
      "EVENT_DROP",
      "EVENT_SUBSCRIBE",
      "EVENT_PUBLISH",
      "EVENT_DELIVERY_READ",
      "EVENT_DELIVERY_ACK",
      "MANAGER_ADMISSION_ADMIN",
  };
  return rights;
}

struct Args {
  std::filesystem::path output;
  std::filesystem::path manifest;
  std::filesystem::path resource_seed_pack_root;
  scratchbird::core::platform::u32 page_size = kDriverFixturePageSize;
  bool overwrite = false;
  bool bulk_import_fixture = false;
  bool trigger_fixture = false;
  bool security_alter_policy_fixture = false;
};

struct FixtureTable {
  std::string path;
  std::string schema_path;
  std::string name;
  api::EngineUuid uuid;
  std::vector<std::pair<std::string, std::string>> columns;
  std::vector<std::vector<std::pair<std::string, std::string>>> rows;
};

void Usage() {
  std::cerr << "usage: public_driver_test_database_seed --output PATH --manifest PATH "
               "--resource-seed-pack-root PATH [--page-size BYTES] [--overwrite] "
               "[--bulk-import-fixture] [--trigger-fixture] "
               "[--security-alter-policy-fixture]\n";
}

bool ParsePageSize(const std::string& value, scratchbird::core::platform::u32* out) {
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
  if (end == value.c_str() || *end != '\0') {
    return false;
  }
  if (parsed != 4096 && parsed != 8192 && parsed != 16384 &&
      parsed != 32768 && parsed != 65536 && parsed != 131072) {
    return false;
  }
  *out = static_cast<scratchbird::core::platform::u32>(parsed);
  return true;
}

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") {
      args->overwrite = true;
      continue;
    }
    if (key == "--bulk-import-fixture") {
      args->bulk_import_fixture = true;
      continue;
    }
    if (key == "--trigger-fixture") {
      args->trigger_fixture = true;
      continue;
    }
    if (key == "--security-alter-policy-fixture") {
      args->security_alter_policy_fixture = true;
      continue;
    }
    if (i + 1 >= argc) {
      return false;
    }
    const std::string value = argv[++i];
    if (key == "--output") {
      args->output = value;
    } else if (key == "--manifest") {
      args->manifest = value;
    } else if (key == "--resource-seed-pack-root") {
      args->resource_seed_pack_root = value;
    } else if (key == "--page-size") {
      if (!ParsePageSize(value, &args->page_size)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return !args->output.empty() && !args->manifest.empty() &&
         !args->resource_seed_pack_root.empty();
}

void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

api::EngineUuid ClientFixtureUuid(std::string_view text) {
  const auto parsed = uuid::ParseUuid(std::string(text));
  if (!parsed.ok()) Fail("invalid UUID in client fixture input");
  return parsed.value;
}


std::array<std::uint8_t, 32> EvidenceSha256(std::string_view value) {
  const auto* bytes = reinterpret_cast<const scratchbird::core::platform::byte*>(
      value.data());
  const auto digest = core_hash::ComputeSha256Digest(bytes, value.size());
  if (!digest.ok()) {
    Fail("driver test database evidence SHA-256 failed");
  }
  return digest.digest;
}

scratchbird::core::platform::TypedUuid MakeIdentity(UuidKind kind,
                                                    scratchbird::core::platform::u64 millis) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, millis);
  if (!generated.ok()) {
    std::cerr << generated.diagnostic.diagnostic_code << ':'
              << generated.diagnostic.message_key << '\n';
    std::exit(EXIT_FAILURE);
  }
  return generated.value;
}

std::string JsonString(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  for (const unsigned char ch : value) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (ch < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kHex[(ch >> 4) & 0x0f]);
          out.push_back(kHex[ch & 0x0f]);
        } else {
          out.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

memory::AllocationPolicy ProductionMemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "public_driver_test_database_seed";
  policy.hard_limit_bytes = 128ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 96ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

void ConfigureMemory() {
  const auto configured =
      memory::ConfigureDefaultMemoryManager(ProductionMemoryPolicy(),
                                            "public_driver_test_database_seed_startup_policy");
  if (!configured.ok() || configured.fixture_mode) {
    Fail("driver test database seed memory configuration failed");
  }
}

void RemoveDatabaseArtifacts(const std::filesystem::path& output) {
  // Explicit overwrite replaces the database and its engine-owned .sb.*
  // sidecars as a unit. A fixed suffix list left diagnostic/availability and
  // publication records belonging to the previous database UUID behind.
  std::vector<std::filesystem::path> artifacts{output};
  const auto parent = output.has_parent_path() ? output.parent_path()
                                                : std::filesystem::path(".");
  const auto prefix = output.filename().string() + ".sb.";
  std::error_code error;
  const bool parent_exists = std::filesystem::exists(parent, error);
  if (error) Fail("driver fixture overwrite inspection failed: " + error.message());
  if (parent_exists) {
    std::filesystem::directory_iterator entry(parent, error), end;
    if (error) Fail("driver fixture overwrite enumeration failed: " + error.message());
    for (; entry != end; entry.increment(error)) {
      if (error) Fail("driver fixture overwrite enumeration failed: " + error.message());
      if (entry->path().filename().string().starts_with(prefix))
        artifacts.push_back(entry->path());
    }
    if (error) Fail("driver fixture overwrite enumeration failed: " + error.message());
  }
  for (const auto& artifact : artifacts) {
    std::filesystem::remove_all(artifact, error);
    if (error) Fail("driver fixture overwrite cleanup failed: " + error.message());
  }
}

api::EngineLocalizedName Name(std::string path, std::string name) {
  api::EngineLocalizedName localized;
  localized.language_tag = "en";
  localized.name_class = "primary";
  localized.path = std::move(path);
  localized.name = std::move(name);
  localized.raw_name_text = localized.name;
  localized.display_name = localized.name;
  localized.default_name = true;
  return localized;
}

api::EngineColumnDefinition Column(const api::EngineRequestContext& context,
                                  std::uint32_t ordinal,
                                  std::string name,
                                  std::string type) {
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
  column.requested_column_uuid = api::GenerateCrudEngineUuid("object");
  column.names.push_back(Name(name, name));
  column.descriptor.descriptor_uuid = api::GenerateCrudEngineUuid("object");
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = std::move(type);
  column.descriptor.datatype_descriptor_uuid = catalog.descriptor_uuid.value;
  column.descriptor.datatype_descriptor_generation = catalog.descriptor_epoch;
  column.descriptor.type_uuid = catalog.descriptor_uuid.value;
  const auto codec = datatypes::LookupDatatypeTypeCodecIdentityV1(
      context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
      context.datatype_registry_generation, catalog.descriptor_uuid.value,
      catalog.descriptor_epoch);
  if (codec.ok) column.descriptor.type_uuid = codec.row.type_uuid;
  column.descriptor.encoded_descriptor =
      "type=" + column.descriptor.canonical_type_name + ";nullable=true";
  return column;
}

api::EngineTypedValue Value(std::string value, std::string type = "text") {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = std::move(type);
  typed.descriptor.encoded_descriptor = "type=" + typed.descriptor.canonical_type_name;
  if (typed.descriptor.canonical_type_name == "uuid") {
    const auto identity = ClientFixtureUuid(value);
    typed.binary_value.assign(identity.bytes.begin(), identity.bytes.end());
  } else {
    typed.encoded_value = std::move(value);
  }
  typed.is_null = false;
  typed.state = api::EngineValueState::value;
  return typed;
}

api::EngineRowValue Row(const std::vector<std::pair<std::string, std::string>>& fields) {
  api::EngineRowValue row;
  for (const auto& [name, value] : fields) {
    row.fields.push_back({name, Value(value, name == "principal_uuid" ? "uuid" : "text")});
  }
  return row;
}

api::EngineRequestContext BaseContext(const Args& args, const CreatedDatabaseFixture& fixture) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.security_context_present = true;
  context.request_id = "public-driver-test-database-seed";
  context.database_path = args.output.string();
  context.database_uuid = fixture.database_uuid;
  context.default_root_uuid = fixture.state.filespace_uuid.value;
  const auto bootstrap = db::ReadDatabaseBootstrapSecurityCatalog(args.output.string());
  if (!bootstrap.ok() || !bootstrap.state.present || !bootstrap.state.committed_by_inventory)
    Fail("driver fixture durable bootstrap principal unavailable");
  context.principal_uuid = bootstrap.state.principal_uuid.value;
  context.session_uuid = ClientFixtureUuid("019f0a11-ce00-7000-8000-0000000000ff");
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.datatype_catalog_snapshot_uuid =
      ClientFixtureUuid(kDatatypeCatalogSnapshotUuid);
  context.datatype_catalog_generation = 1;
  context.datatype_registry_generation = 1;
  const auto loaded = api::LoadSecurityPrincipalLifecycleState(context);
  if (!loaded.ok) Fail("driver fixture durable security catalog unavailable");
  const auto& lifecycle = loaded.state;
  if (!lifecycle.row_policies.empty())
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
  context.optimizer_route_epoch = 1;
  context.optimizer_route_generation = 1;
  context.optimizer_memory_budget_bytes = 64 * 1024 * 1024;
  context.optimizer_maximum_candidate_count = 131072;
  context.optimizer_maximum_memo_groups = 131072;
  context.optimizer_maximum_search_steps = 1048576;
  context.optimizer_maximum_planning_time_ns = 5'000'000'000ull;
  context.optimizer_spill_allowed = true;
  context.trace_tags.push_back("public.driver_test_database_seed");
  context.trace_tags.push_back("security.bootstrap");
  context.trace_tags.push_back("group:SEC");
  return context;
}

api::EngineRequestContext TxContext(api::EngineRequestContext base,
                                    const api::EngineBeginTransactionResult& tx) {
  base.local_transaction_id = tx.local_transaction_id;
  base.transaction_uuid = tx.transaction_uuid;
  base.transaction_isolation_level = tx.isolation_level;
  base.snapshot_visible_through_local_transaction_id = tx.snapshot_visible_through_local_transaction_id;
  return base;
}

api::EngineRequestContext Begin(const api::EngineRequestContext& base) {
  api::EngineBeginTransactionRequest request;
  request.context = base;
  request.isolation_level = "read_committed";
  const auto result = api::EngineBeginTransaction(request);
  if (!result.ok) {
    Fail("driver test database seed transaction.begin failed");
  }
  return TxContext(base, result);
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  const auto result = api::EngineCommitTransaction(request);
  if (!result.ok) {
    Fail("driver test database seed transaction.commit failed");
  }
}

api::EngineUuid SchemaUuidForPath(const api::EngineRequestContext& context, const std::string& path) {
  api::EngineApiDiagnostic diagnostic;
  const auto schemas = api::VisibleSchemaTreeRecords(context, context.local_transaction_id, diagnostic);
  if (diagnostic.error) {
    Fail("driver test database seed schema read failed: " + diagnostic.code + ":" + diagnostic.detail);
  }
  for (const auto& schema : schemas) {
    for (const auto& name : schema.localized_names) {
      if (name.path == path) {
        return schema.schema_uuid;
      }
    }
  }
  return {};
}

CreatedDatabaseFixture CreateDatabase(const Args& args) {
  if (args.overwrite) {
    RemoveDatabaseArtifacts(args.output);
  } else if (std::filesystem::exists(args.output)) {
    Fail("driver test database already exists");
  }
  std::filesystem::create_directories(args.output.parent_path());

  const auto database_uuid = MakeIdentity(UuidKind::database, kDriverFixtureCreationMillis);
  const auto filespace_uuid = MakeIdentity(UuidKind::filespace, kDriverFixtureCreationMillis + 1);

  db::DatabaseCreateConfig create;
  create.path = args.output.string();
  create.database_uuid = database_uuid;
  create.filespace_uuid = filespace_uuid;
  create.page_size = args.page_size;
  create.creation_unix_epoch_millis = kDriverFixtureCreationMillis;
  create.resource_seed_pack_root = args.resource_seed_pack_root.string();
  create.require_resource_seed_pack = true;
  create.allow_minimal_resource_bootstrap = false;
  create.bootstrap_principal_name = "alice";
  create.bootstrap_credential_fingerprint =
      std::string(kAliceBootstrapCredentialFingerprint);
  create.require_bootstrap_principal = true;
  create.allow_uncredentialed_bootstrap = false;
  create.allow_overwrite = false;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key;
    for (const auto& argument : created.diagnostic.arguments) {
      std::cerr << ':' << argument.key << '=' << argument.value;
    }
    std::cerr << '\n';
    Fail("driver test database create failed");
  }

  db::DatabaseOpenConfig open;
  open.path = args.output.string();
  open.suppress_background_agents = true;
  const auto opened = db::OpenDatabaseFile(open);
  if (!opened.ok() || !opened.state.resource_seed_catalog_present) {
    Fail("driver test database full resource seed activation failed");
  }
  CreatedDatabaseFixture fixture;
  fixture.database_uuid = database_uuid.value;
  fixture.state = created.state;
  return fixture;
}

void CreateAppSchema(const api::EngineRequestContext& context) {
  if (!SchemaUuidForPath(context, "app").is_nil()) {
    return;
  }
  api::EngineCreateSchemaRequest request;
  request.context = context;
  request.target_object.uuid = ClientFixtureUuid(kAppSchemaUuid);
  request.target_object.object_kind = "schema";
  request.localized_names.push_back(Name("app", "app"));
  const auto result = api::EngineCreateSchema(request);
  if (!result.ok) {
    Fail("driver test database app schema create failed");
  }
}

api::EngineObjectReference CreateTable(const api::EngineRequestContext& context,
                                       const FixtureTable& fixture,
                                       const api::EngineUuid& schema_uuid) {
  api::EngineCreateTableRequest request;
  request.context = context;
  request.context.current_schema_uuid = schema_uuid;
  request.target_schema.uuid = schema_uuid;
  request.target_schema.object_kind = "schema";
  request.requested_table_uuid = fixture.uuid;
  request.table_names.push_back(Name(fixture.path, fixture.name));
  for (std::uint32_t ordinal = 0; ordinal < fixture.columns.size(); ++ordinal) {
    request.table_columns.push_back(Column(context, ordinal,
                                           fixture.columns[ordinal].first,
                                           fixture.columns[ordinal].second));
  }
  const auto result = api::EngineCreateTable(request);
  if (!result.ok) {
    std::cerr << "table_create_failed=" << fixture.path << '\n';
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
                << diagnostic.detail << '\n';
    }
    Fail("driver test database table create failed");
  }
  return result.table_object;
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

void InsertRows(const SeedSession& session,
                const api::EngineRequestContext& context,
                const api::EngineObjectReference& table,
                const FixtureTable& fixture) {
  if (fixture.rows.empty()) {
    return;
  }
  SeedStatement statement(session, context);
  api::EngineInsertRowsRequest request;
  request.context = statement.context;
  request.target_table = table;
  for (const auto& fields : fixture.rows) {
    request.input_rows.push_back(Row(fields));
  }
  const auto result = api::EngineInsertRows(request);
  if (!result.ok || result.inserted_count != fixture.rows.size()) {
    std::cerr << "table_insert_failed=" << fixture.path << '\n';
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
                << diagnostic.detail << '\n';
    }
    Fail("driver test database row seed failed");
  }
}

std::vector<FixtureTable> FixtureTables(const api::EngineUuid& principal_uuid,
                                        bool include_bulk_import_fixture,
                                        bool include_trigger_fixture) {
  std::vector<FixtureTable> fixtures{
      {
          "app.customers",
          "app",
          "customers",
          ClientFixtureUuid("018f0a2b-0000-7000-9000-000000000101"),
          {{"id", "bigint"}, {"customer_name", "text"}, {"status", "text"}},
          {{{"id", "1"}, {"customer_name", "Ada Lovelace"}, {"status", "active"}},
           {{"id", "2"}, {"customer_name", "Grace Hopper"}, {"status", "active"}}},
      },
      {
          "app.customer_profiles",
          "app",
          "customer_profiles",
          ClientFixtureUuid("018f0a2b-0000-7000-9000-000000000302"),
          {{"id", "bigint"}, {"customer_id", "bigint"}, {"profile_json", "text"}},
          {{{"id", "10"}, {"customer_id", "1"}, {"profile_json", "{\"tier\":\"beta\"}"}}},
      },
      {
          "app.payroll_private",
          "app",
          "payroll_private",
          ClientFixtureUuid("018f0a2b-0000-7000-9000-000000000301"),
          {{"id", "bigint"}, {"employee_name", "text"}, {"salary_cents", "bigint"}},
          {{{"id", "100"}, {"employee_name", "Test Employee"}, {"salary_cents", "123456"}}},
      },
      {
          "sys.security.users",
          "sys.security",
          "users",
          ClientFixtureUuid("018f0a2b-0000-7000-9000-000000000401"),
          {{"principal_uuid", "uuid"}, {"principal_name", "text"}, {"principal_state", "text"}},
          {{{"principal_uuid", uuid::UuidToString(principal_uuid)}, {"principal_name", "alice"}, {"principal_state", "active"}}},
      },
  };
  if (include_bulk_import_fixture) {
    fixtures.push_back({
        "app.bulk_import_v1",
        "app",
        "bulk_import_v1",
        ClientFixtureUuid("018f0a2b-0000-7000-9000-000000000501"),
        {{"id", "int32"},
         {"item_name", "text"},
         {"item_state", "text"}},
        {},
    });
  }
  if (include_trigger_fixture) {
    fixtures.push_back({
        "app.trig_items",
        "app",
        "trig_items",
        ClientFixtureUuid("018f0a2b-0000-7000-9000-000000000601"),
        {{"item_id", "bigint"},
         {"item_name", "text"},
         {"item_price", "bigint"}},
        {},
    });
    fixtures.push_back({
        "app.trig_audit",
        "app",
        "trig_audit",
        ClientFixtureUuid("018f0a2b-0000-7000-9000-000000000602"),
        {{"audit_id", "bigint"},
         {"event_kind", "text"},
         {"item_id", "bigint"},
         {"old_price", "bigint"},
         {"new_price", "bigint"},
         {"audit_note", "text"}},
        {},
    });
  }
  return fixtures;
}

void CreateTriggerFixtureSequence(const api::EngineRequestContext& context,
                                  const api::EngineUuid& schema_uuid) {
  api::EngineCreateSequenceRequest request;
  request.context = context;
  request.context.current_schema_uuid = schema_uuid;
  request.target_schema.uuid = schema_uuid;
  request.target_schema.object_kind = "schema";
  request.target_object.uuid =
      ClientFixtureUuid("018f0a2b-0000-7000-9000-000000000603");
  request.target_object.object_kind = "sequence";
  request.localized_names.push_back(
      Name("app.trig_audit_seq", "trig_audit_seq"));
  request.option_envelopes.push_back(
      "sequence_lookup_key:app.trig_audit_seq");
  request.option_envelopes.push_back("sequence_start_value:1");
  request.option_envelopes.push_back("sequence_increment:1");
  const auto result = api::EngineCreateSequence(request);
  if (!result.ok) {
    std::cerr << "sequence_create_failed=app.trig_audit_seq\n";
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
                << diagnostic.detail << '\n';
    }
    Fail("driver test database trigger sequence create failed");
  }
}

void CreateSecurityAlterPolicyFixture(
    const api::EngineRequestContext& context,
    const api::EngineUuid& schema_uuid) {
  // Fixture publication is an engine-internal bootstrap action.  Keep this
  // authority local to the seeder: the authenticated parser session used by
  // the process E2E must obtain its own materialized POLICY_ADMIN authority.
  auto fixture_context = context;
  fixture_context.trust_mode = api::EngineTrustMode::embedded_in_process;
  fixture_context.trace_tags.push_back("security.fixture_trace_authority");
  fixture_context.trace_tags.push_back("right:POLICY_ADMIN");
  fixture_context.statement_uuid =
      ClientFixtureUuid(kSecurityAlterPolicyStatementUuid);
  fixture_context.statement_snapshot_uuid =
      ClientFixtureUuid(kSecurityAlterPolicyStatementSnapshotUuid);
  fixture_context.statement_snapshot_generation = 1;
  fixture_context.statement_receipt_uuid =
      ClientFixtureUuid(kSecurityAlterPolicyStatementReceiptUuid);
  fixture_context.statement_metadata_snapshot_engine_owned = true;
  fixture_context.statement_metadata_snapshot_uuid =
      ClientFixtureUuid(kSecurityAlterPolicyMetadataSnapshotUuid);

  const auto relation = api::LoadMgaRelationStorageDescriptor(
      fixture_context, ClientFixtureUuid(kCustomersRelationUuid));
  if (!relation.ok || relation.descriptor.relation_generation == 0) {
    std::cerr << "security_policy_target_load_failed=app.customers\n";
    if (relation.diagnostic.error) {
      std::cerr << relation.diagnostic.code << ':'
                << relation.diagnostic.message_key << ':'
                << relation.diagnostic.detail << '\n';
    }
    Fail("driver test database security policy target load failed");
  }

  api::EngineSecurityCreatePolicyRequest create;
  create.context = fixture_context;
  create.policy_uuid = ClientFixtureUuid(kSecurityAlterPolicyFixtureUuid);
  create.policy_name = "app_policy";
  create.target_schema_uuid = schema_uuid;
  create.target_object_uuid = ClientFixtureUuid(kCustomersRelationUuid);
  create.target_object_kind = "relation";
  create.policy_effect = "row_filter";
  create.predicate_envelope = "predicate:true";
  create.definer_principal_uuid = context.principal_uuid;
  create.localized_names.push_back(Name("app.app_policy", "app_policy"));
  create.native_authority.present = true;
  create.native_authority.policy_version_uuid =
      ClientFixtureUuid(kSecurityAlterPolicyVersionUuid);
  create.native_authority.target_relation_generation =
      relation.descriptor.relation_generation;
  create.native_authority.phase = 1;
  create.native_authority.effective_policy_uuid =
      ClientFixtureUuid(kSecurityAlterPolicyEffectiveUuid);
  create.native_authority.effective_policy_generation = 1;
  create.native_authority.effective_expression_uuid =
      ClientFixtureUuid(kSecurityAlterPolicyEffectiveExpressionUuid);
  create.native_authority.effective_expression_generation = 1;
  create.native_authority.effective_expression_evidence_sha256 =
      EvidenceSha256("ScratchBird.PublicDriverFixture.AppPolicy.EffectiveExpression.V1");
  create.native_authority.source_expression_uuid =
      ClientFixtureUuid(kSecurityAlterPolicySourceExpressionUuid);
  create.native_authority.source_expression_generation = 1;
  create.native_authority.source_expression_evidence_sha256 =
      EvidenceSha256("ScratchBird.PublicDriverFixture.AppPolicy.SourceExpression.V1");
  const auto created = api::EngineSecurityCreatePolicy(create);
  if (!created.ok || !created.policy_created ||
      created.policy_generation == 0) {
    std::cerr << "security_policy_create_failed=app.app_policy\n";
    for (const auto& diagnostic : created.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
                << diagnostic.detail << '\n';
    }
    Fail("driver test database security policy fixture create failed");
  }

  api::EngineSecurityAlterPolicyRequest disable;
  disable.context = fixture_context;
  disable.policy_uuid = ClientFixtureUuid(kSecurityAlterPolicyFixtureUuid);
  disable.expected_policy_generation = created.policy_generation;
  disable.lifecycle_state = "disabled";
  const auto disabled = api::EngineSecurityAlterPolicy(disable);
  if (!disabled.ok || !disabled.policy_altered ||
      disabled.previous_policy_generation != created.policy_generation ||
      disabled.policy_generation != created.policy_generation + 1) {
    std::cerr << "security_policy_disable_failed=app.app_policy\n";
    for (const auto& diagnostic : disabled.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
                << diagnostic.detail << '\n';
    }
    Fail("driver test database security policy fixture disable failed");
  }
}

void SeedFixtureObjects(const SeedSession& session,
                        const api::EngineRequestContext& context,
                        bool include_bulk_import_fixture,
                        bool include_trigger_fixture,
                        bool include_security_alter_policy_fixture) {
  CreateAppSchema(context);
  for (const auto& fixture :
       FixtureTables(context.principal_uuid, include_bulk_import_fixture, include_trigger_fixture)) {
    const api::EngineUuid schema_uuid = SchemaUuidForPath(context, fixture.schema_path);
    if (schema_uuid.is_nil()) {
      std::cerr << "schema_not_visible=" << fixture.schema_path << '\n';
      Fail("driver test database fixture schema not visible");
    }
    const auto table = CreateTable(context, fixture, schema_uuid);
    InsertRows(session, context, table, fixture);
  }
  if (include_trigger_fixture) {
    const auto schema_uuid = SchemaUuidForPath(context, "app");
    if (schema_uuid.is_nil()) {
      Fail("driver test database trigger fixture schema not visible");
    }
    CreateTriggerFixtureSequence(context, schema_uuid);
  }
  if (include_security_alter_policy_fixture) {
    const auto schema_uuid = SchemaUuidForPath(context, "app");
    if (schema_uuid.is_nil()) {
      Fail("driver test database security policy fixture schema not visible");
    }
    CreateSecurityAlterPolicyFixture(context, schema_uuid);
  }
}

void WriteResourceSeedCatalogJson(std::ofstream& out,
                                  const resources::ResourceSeedCatalogImage& catalog,
                                  std::string_view trailing = ",") {
  out << "  \"resource_seed_catalog\": {\n";
  out << "    \"seed_pack_name\": " << JsonString(catalog.seed_pack_name) << ",\n";
  out << "    \"seed_pack_version\": " << JsonString(catalog.seed_pack_version) << ",\n";
  out << "    \"seed_pack_hash\": " << JsonString(catalog.content_hash) << ",\n";
  out << "    \"active\": " << (catalog.active ? "true" : "false") << ",\n";
  out << "    \"minimal_bootstrap\": " << (catalog.minimal_bootstrap ? "true" : "false") << ",\n";
  out << "    \"database_create_ready\": " << (catalog.database_create_ready ? "true" : "false") << ",\n";
  out << "    \"database_open_ready\": " << (catalog.database_open_ready ? "true" : "false") << ",\n";
  out << "    \"versions\": {\n";
  out << "      \"i18n\": " << JsonString(catalog.i18n_version) << ",\n";
  out << "      \"timezone\": " << JsonString(catalog.timezone_version) << ",\n";
  out << "      \"charset\": " << JsonString(catalog.charset_version) << ",\n";
  out << "      \"collation\": " << JsonString(catalog.collation_version) << ",\n";
  out << "      \"locale\": " << JsonString(catalog.locale_version) << "\n";
  out << "    },\n";
  out << "    \"content_hashes\": {\n";
  out << "      \"charset\": " << JsonString(catalog.charset_content_hash) << ",\n";
  out << "      \"collation\": " << JsonString(catalog.collation_content_hash) << ",\n";
  out << "      \"locale\": " << JsonString(catalog.locale_content_hash) << ",\n";
  out << "      \"timezone\": " << JsonString(catalog.timezone_content_hash) << "\n";
  out << "    },\n";
  out << "    \"epochs\": {\n";
  out << "      \"resource\": " << catalog.resource_epoch << ",\n";
  out << "      \"charset\": " << catalog.charset_epoch << ",\n";
  out << "      \"collation\": " << catalog.collation_epoch << ",\n";
  out << "      \"timezone\": " << catalog.timezone_epoch << ",\n";
  out << "      \"locale\": " << catalog.locale_epoch << ",\n";
  out << "      \"runtime_cache\": " << catalog.runtime_cache_epoch << "\n";
  out << "    },\n";
  out << "    \"counts\": {\n";
  out << "      \"resource_bundle_records\": " << catalog.resource_bundle_records << ",\n";
  out << "      \"resource_artifact_records\": " << catalog.resource_artifact_records << ",\n";
  out << "      \"resource_activation_records\": " << catalog.resource_activation_records << ",\n";
  out << "      \"charset_records\": " << catalog.charset_records << ",\n";
  out << "      \"charset_alias_records\": " << catalog.charset_alias_records << ",\n";
  out << "      \"charset_mapping_artifacts\": " << catalog.charset_mapping_artifacts << ",\n";
  out << "      \"collation_records\": " << catalog.collation_records << ",\n";
  out << "      \"collation_tailoring_records\": " << catalog.collation_tailoring_records << ",\n";
  out << "      \"locale_records\": " << catalog.locale_records << ",\n";
  out << "      \"timezone_records\": " << catalog.timezone_records << ",\n";
  out << "      \"timezone_transition_records\": " << catalog.timezone_transition_records << ",\n";
  out << "      \"timezone_leap_second_records\": " << catalog.timezone_leap_second_records << ",\n";
  out << "      \"runtime_cache_invalidation_records\": " << catalog.runtime_cache_invalidation_records << ",\n";
  out << "      \"index_dependency_records\": " << catalog.index_dependency_records << ",\n";
  out << "      \"family_version_records\": " << catalog.family_versions.size() << ",\n";
  out << "      \"resource_alias_records\": " << catalog.aliases.size() << "\n";
  out << "    },\n";
  out << "    \"language_profiles_required\": [\"en-US\", \"en-CA\", \"fr-CA\", \"fr-FR\", \"de-DE\", \"it-IT\", \"es-ES\"],\n";
  out << "    \"language_resource_artifacts_loaded\": "
      << (catalog.i18n_version.empty() ? "false" : "true") << ",\n";
  out << "    \"families\": [\n";
  for (std::size_t index = 0; index < catalog.family_versions.size(); ++index) {
    const auto& family = catalog.family_versions[index];
    out << "      {\"family\": " << JsonString(resources::ResourceSeedFamilyName(family.family))
        << ", \"version\": " << JsonString(family.version)
        << ", \"content_hash\": " << JsonString(family.content_hash)
        << ", \"activation_epoch\": " << family.activation_epoch
        << ", \"active\": " << (family.active ? "true" : "false") << "}";
    out << (index + 1 == catalog.family_versions.size() ? "\n" : ",\n");
  }
  out << "    ]\n";
  out << "  }" << trailing << "\n";
}

void WriteManifest(const Args& args,
                   const api::EngineUuid& database_uuid,
                   const api::EngineUuid& principal_uuid,
                   const db::DatabaseLifecycleState& state) {
  std::filesystem::create_directories(args.manifest.parent_path());
  std::ofstream out(args.manifest, std::ios::trunc);
  if (!out) {
    Fail("driver test database manifest open failed");
  }
  out << "{\n";
  out << "  \"schema_version\": \"scratchbird_public_driver_test_database_seed_v1\",\n";
  out << "  \"database\": \"" << args.output.filename().string() << "\",\n";
  out << "  \"database_uuid\": \"" << uuid::UuidToString(database_uuid) << "\",\n";
  out << "  \"page_size\": " << args.page_size << ",\n";
  out << "  \"creation_unix_epoch_millis\": " << kDriverFixtureCreationMillis << ",\n";
  out << "  \"full_create_database\": true,\n";
  out << "  \"resource_seed_pack_active\": true,\n";
  out << "  \"policy_seed_pack_active\": "
      << (state.policy_seed_catalog_present ? "true" : "false") << ",\n";
  out << "  \"policy_pack_id\": \"" << state.policy_seed_catalog.policy_pack_id << "\",\n";
  out << "  \"default_policy_records\": "
      << state.policy_seed_catalog.default_policy_records << ",\n";
  out << "  \"minimal_resource_bootstrap\": false,\n";
  WriteResourceSeedCatalogJson(out, state.resource_seed_catalog);
  out << "  \"fixture_objects_seeded\": [\n";
  const auto tables =
      FixtureTables(principal_uuid, args.bulk_import_fixture, args.trigger_fixture);
  for (std::size_t index = 0; index < tables.size(); ++index) {
    out << "    {\"path\": \"" << tables[index].path << "\", \"uuid\": \"" << uuid::UuidToString(tables[index].uuid) << "\"}";
    out << (index + 1 == tables.size() && !args.trigger_fixture &&
                    !args.security_alter_policy_fixture
                ? "\n"
                : ",\n");
  }
  if (args.trigger_fixture) {
    out << "    {\"path\": \"app.trig_audit_seq\", "
           "\"uuid\": \"018f0a2b-0000-7000-9000-000000000603\"}";
    out << (args.security_alter_policy_fixture ? ",\n" : "\n");
  }
  if (args.security_alter_policy_fixture) {
    out << "    {\"path\": \"app.app_policy\", \"uuid\": \""
        << kSecurityAlterPolicyFixtureUuid << "\"}\n";
  }
  out << "  ],\n";
  out << "  \"security\": {\n";
  out << "    \"principal\": \"alice\",\n";
  out << "    \"principal_uuid\": \"" << uuid::UuidToString(principal_uuid) << "\",\n";
  out << "    \"role\": \"sysarch\",\n";
  out << "    \"grant_count\": " << SysarchRights().size() << "\n";
  out << "  },\n";
  out << "  \"authority\": {\n";
  out << "    \"database_identity\": \"engine_uuid\",\n";
  out << "    \"database_create_surface\": \"storage_database_lifecycle_api\",\n";
  out << "    \"fixture_seed_surface\": \"engine_internal_api_after_full_create\",\n";
  out << "    \"sql_text_authority\": false,\n";
  out << "    \"transaction_authority\": \"mga_inventory\"\n";
  out << "  }\n";
  out << "}\n";
  if (!out) {
    Fail("driver test database manifest write failed");
  }
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    Usage();
    return EXIT_FAILURE;
  }
  if (!std::filesystem::exists(args.resource_seed_pack_root)) {
    std::cerr << "resource seed pack root is missing: " << args.resource_seed_pack_root << '\n';
    return EXIT_FAILURE;
  }

  ConfigureMemory();
  const CreatedDatabaseFixture fixture = CreateDatabase(args);
  const auto context = Begin(BaseContext(args, fixture));
  SeedSession session(context);
  SeedFixtureObjects(session, context, args.bulk_import_fixture, args.trigger_fixture,
                     args.security_alter_policy_fixture);
  Commit(context);
  const auto bootstrap_security = db::ReadDatabaseBootstrapSecurityCatalog(
      args.output.string());
  if (!bootstrap_security.ok() || !bootstrap_security.state.present ||
      !bootstrap_security.state.committed_by_inventory ||
      bootstrap_security.state.principal_name != "alice" ||
      bootstrap_security.state.credential_fingerprint !=
          kAliceBootstrapCredentialFingerprint ||
      bootstrap_security.state.sysarch_role_uuid.value !=
          ClientFixtureUuid(db::kCanonicalSysarchRoleObjectUuid)) {
    Fail("driver test database durable bootstrap security verification failed");
  }
  WriteManifest(args, fixture.database_uuid, context.principal_uuid, fixture.state);

  std::cout << "public_driver_test_database_seed=passed database="
            << args.output.filename().string()
            << " full_create_database=true fixture_objects="
            << (FixtureTables(context.principal_uuid, args.bulk_import_fixture, args.trigger_fixture).size() +
                (args.trigger_fixture ? 1U : 0U) +
                (args.security_alter_policy_fixture ? 1U : 0U))
            << '\n';
  return EXIT_SUCCESS;
}
