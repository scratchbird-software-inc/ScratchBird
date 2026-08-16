// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/schema_tree_api.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "memory.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

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
namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace resources = scratchbird::core::resources;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr scratchbird::core::platform::u64 kDriverFixtureCreationMillis = 1767225600000ull;
constexpr scratchbird::core::platform::u32 kDriverFixturePageSize = 16384;
constexpr std::string_view kAlicePrincipalUuid = "019f0a11-ce00-7000-8000-000000000001";
constexpr std::string_view kAliceBootstrapCredentialFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:"
    "verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";
constexpr std::string_view kAppSchemaUuid = "018f0a2b-0000-7000-9000-000000000100";

struct CreatedDatabaseFixture {
  std::string database_uuid;
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
};

struct FixtureTable {
  std::string path;
  std::string schema_path;
  std::string name;
  std::string uuid;
  std::vector<std::pair<std::string, std::string>> columns;
  std::vector<std::vector<std::pair<std::string, std::string>>> rows;
};

void Usage() {
  std::cerr << "usage: public_driver_test_database_seed --output PATH --manifest PATH "
               "--resource-seed-pack-root PATH [--page-size BYTES] [--overwrite]\n";
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
  static const std::vector<std::string> suffixes = {
      "",
      ".sb.api_events",
      ".sb.catalog_object_events",
      ".sb.event_sequence_allocator",
      ".sb.local_password_auth",
      ".sb.security_principal_events",
      ".sb.owner.lock",
      ".sb.route.owner.lock",
      ".sb.mga_index_entries",
      ".sb.mga_large_values",
      ".sb.mga_relation_descriptors",
      ".sb.mga_relation_metadata",
      ".sb.mga_relation_scope",
      ".sb.mga_row_versions",
      ".sb.mga_savepoints",
      ".sb.mga_event_sequence_allocator",
      ".sb.local_transaction_inventory",
  };
  for (const auto& suffix : suffixes) {
    std::error_code ignored;
    std::filesystem::remove_all(output.string() + suffix, ignored);
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

api::EngineColumnDefinition Column(std::uint32_t ordinal, std::string name, std::string type) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.names.push_back(Name(name, name));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = std::move(type);
  if (column.descriptor.canonical_type_name == "bigint") {
    column.descriptor.encoded_descriptor =
        "type=bigint;nullable=true;"
        "type_uuid=019d0000-0000-7000-8000-00000000d712";
  } else {
    column.descriptor.encoded_descriptor =
        "type=" + column.descriptor.canonical_type_name;
  }
  return column;
}

api::EngineTypedValue Value(std::string value, std::string type = "text") {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = std::move(type);
  typed.descriptor.encoded_descriptor = "type=" + typed.descriptor.canonical_type_name;
  typed.encoded_value = std::move(value);
  typed.is_null = false;
  typed.state = api::EngineValueState::value;
  return typed;
}

api::EngineRowValue Row(const std::vector<std::pair<std::string, std::string>>& fields) {
  api::EngineRowValue row;
  for (const auto& [name, value] : fields) {
    row.fields.push_back({name, Value(value)});
  }
  return row;
}

api::EngineRequestContext BaseContext(const Args& args, const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.security_context_present = true;
  context.request_id = "public-driver-test-database-seed";
  context.database_path = args.output.string();
  context.database_uuid.canonical = database_uuid;
  context.principal_uuid.canonical = std::string(kAlicePrincipalUuid);
  context.session_uuid.canonical = "019f0a11-ce00-7000-8000-0000000000ff";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
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

std::string SchemaUuidForPath(const api::EngineRequestContext& context, const std::string& path) {
  for (const auto& schema : api::VisibleSchemaTreeRecords(context, context.local_transaction_id)) {
    for (const auto& name : schema.localized_names) {
      if (name.path == path) {
        return schema.schema_uuid;
      }
    }
  }
  return {};
}

CreatedDatabaseFixture CreateDatabase(const Args& args) {
  if (std::filesystem::exists(args.output)) {
    if (!args.overwrite) {
      Fail("driver test database already exists");
    }
    RemoveDatabaseArtifacts(args.output);
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
  fixture.database_uuid = uuid::UuidToString(database_uuid.value);
  fixture.state = created.state;
  return fixture;
}

void CreateAppSchema(const api::EngineRequestContext& context) {
  if (!SchemaUuidForPath(context, "app").empty()) {
    return;
  }
  api::EngineCreateSchemaRequest request;
  request.context = context;
  request.target_object.uuid.canonical = std::string(kAppSchemaUuid);
  request.target_object.object_kind = "schema";
  request.localized_names.push_back(Name("app", "app"));
  const auto result = api::EngineCreateSchema(request);
  if (!result.ok) {
    Fail("driver test database app schema create failed");
  }
}

api::EngineObjectReference CreateTable(const api::EngineRequestContext& context,
                                       const FixtureTable& fixture,
                                       const std::string& schema_uuid) {
  api::EngineCreateTableRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = schema_uuid;
  request.target_schema.object_kind = "schema";
  request.requested_table_uuid.canonical = fixture.uuid;
  request.table_names.push_back(Name(fixture.path, fixture.name));
  for (std::uint32_t ordinal = 0; ordinal < fixture.columns.size(); ++ordinal) {
    request.table_columns.push_back(Column(ordinal + 1,
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

void InsertRows(const api::EngineRequestContext& context,
                const api::EngineObjectReference& table,
                const FixtureTable& fixture) {
  if (fixture.rows.empty()) {
    return;
  }
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table = table;
  for (const auto& fields : fixture.rows) {
    request.input_rows.push_back(Row(fields));
  }
  const auto result = api::EngineInsertRows(request);
  if (!result.ok || result.inserted_count != fixture.rows.size()) {
    std::cerr << "table_insert_failed=" << fixture.path << '\n';
    Fail("driver test database row seed failed");
  }
}

std::vector<FixtureTable> FixtureTables() {
  return {
      {
          "app.customers",
          "app",
          "customers",
          "018f0a2b-0000-7000-9000-000000000101",
          {{"id", "bigint"}, {"customer_name", "text"}, {"status", "text"}},
          {{{"id", "1"}, {"customer_name", "Ada Lovelace"}, {"status", "active"}},
           {{"id", "2"}, {"customer_name", "Grace Hopper"}, {"status", "active"}}},
      },
      {
          "app.customer_profiles",
          "app",
          "customer_profiles",
          "018f0a2b-0000-7000-9000-000000000302",
          {{"id", "bigint"}, {"customer_id", "bigint"}, {"profile_json", "text"}},
          {{{"id", "10"}, {"customer_id", "1"}, {"profile_json", "{\"tier\":\"beta\"}"}}},
      },
      {
          "app.payroll_private",
          "app",
          "payroll_private",
          "018f0a2b-0000-7000-9000-000000000301",
          {{"id", "bigint"}, {"employee_name", "text"}, {"salary_cents", "bigint"}},
          {{{"id", "100"}, {"employee_name", "Test Employee"}, {"salary_cents", "123456"}}},
      },
      {
          "sys.security.users",
          "sys.security",
          "users",
          "018f0a2b-0000-7000-9000-000000000401",
          {{"principal_uuid", "text"}, {"principal_name", "text"}, {"principal_state", "text"}},
          {{{"principal_uuid", std::string(kAlicePrincipalUuid)}, {"principal_name", "alice"}, {"principal_state", "active"}}},
      },
  };
}

void SeedFixtureObjects(const api::EngineRequestContext& context) {
  CreateAppSchema(context);
  for (const auto& fixture : FixtureTables()) {
    const std::string schema_uuid = SchemaUuidForPath(context, fixture.schema_path);
    if (schema_uuid.empty()) {
      std::cerr << "schema_not_visible=" << fixture.schema_path << '\n';
      Fail("driver test database fixture schema not visible");
    }
    const auto table = CreateTable(context, fixture, schema_uuid);
    InsertRows(context, table, fixture);
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
                   const std::string& database_uuid,
                   const db::DatabaseLifecycleState& state) {
  std::filesystem::create_directories(args.manifest.parent_path());
  std::ofstream out(args.manifest, std::ios::trunc);
  if (!out) {
    Fail("driver test database manifest open failed");
  }
  out << "{\n";
  out << "  \"schema_version\": \"scratchbird_public_driver_test_database_seed_v1\",\n";
  out << "  \"database\": \"" << args.output.filename().string() << "\",\n";
  out << "  \"database_uuid\": \"" << database_uuid << "\",\n";
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
  const auto tables = FixtureTables();
  for (std::size_t index = 0; index < tables.size(); ++index) {
    out << "    {\"path\": \"" << tables[index].path << "\", \"uuid\": \"" << tables[index].uuid << "\"}";
    out << (index + 1 == tables.size() ? "\n" : ",\n");
  }
  out << "  ],\n";
  out << "  \"security\": {\n";
  out << "    \"principal\": \"alice\",\n";
  out << "    \"principal_uuid\": \"" << kAlicePrincipalUuid << "\",\n";
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
  const auto context = Begin(BaseContext(args, fixture.database_uuid));
  SeedFixtureObjects(context);
  Commit(context);
  const auto bootstrap_security = db::ReadDatabaseBootstrapSecurityCatalog(
      args.output.string());
  if (!bootstrap_security.ok() || !bootstrap_security.state.present ||
      !bootstrap_security.state.committed_by_inventory ||
      bootstrap_security.state.principal_name != "alice" ||
      bootstrap_security.state.credential_fingerprint !=
          kAliceBootstrapCredentialFingerprint ||
      uuid::UuidToString(bootstrap_security.state.sysarch_role_uuid.value) !=
          db::kCanonicalSysarchRoleObjectUuid) {
    Fail("driver test database durable bootstrap security verification failed");
  }
  WriteManifest(args, fixture.database_uuid, fixture.state);

  std::cout << "public_driver_test_database_seed=passed database="
            << args.output.filename().string()
            << " full_create_database=true fixture_objects="
            << FixtureTables().size() << '\n';
  return EXIT_SUCCESS;
}
