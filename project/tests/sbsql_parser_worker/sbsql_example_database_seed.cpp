// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "catalog/schema_tree_api.hpp"
#include "database_lifecycle.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "uuid.hpp"

#include "../database_lifecycle/database_lifecycle_test_memory.hpp"

#include <chrono>
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
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kLiveRouteAlicePrincipalUuid = "019f0a11-ce00-7000-8000-000000000001";
constexpr std::string_view kLiveRouteSysarchRoleUuid = "019f0a11-ce00-7000-8000-00000000f001";
constexpr std::string_view kLiveRoutePublicGroupUuid = "019f0a11-ce00-7000-8000-00000000f101";
constexpr std::string_view kLiveRouteSysarchMembershipUuid = "019f0a11-ce00-7000-8000-00000000f201";
constexpr std::string_view kLiveRoutePublicMembershipUuid = "019f0a11-ce00-7000-8000-00000000f202";

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
      "POLICY_ADMIN",
      "OBS_METRICS_READ_ALL",
      "OBS_RUNTIME_ALL",
      "OBS_INDEX_PROFILE_READ",
      "OBS_MANAGEMENT_INSPECT",
      "OBS_MANAGEMENT_CONTROL",
      "OBS_CONFIG_INSPECT",
      "OBS_CONFIG_CONTROL",
      "SEC_IDENTITY_ADMIN",
      "SEC_MEMBERSHIP_ADMIN",
      "SEC_GRANT_ADMIN",
      "MGA_TRANSACTION_INSPECT",
      "MGA_RECOVERY_INSPECT",
      "MGA_CLEANUP_INSPECT",
      "MGA_CLEANUP_CONTROL",
      "AUTH_PROVIDER_ADMIN",
      "AUDIT_READ",
      "AUDIT_ADMIN",
      "SUPPORT_EXPORT",
      "UDR_TRUST_ADMIN",
      "UDR_MANAGE",
      "UDR_INSPECT",
      "UDR_INVOKE",
      "BACKUP_CREATE",
      "BACKUP_RESTORE",
      "BACKUP_CONTROL",
      "BACKUP_INSPECT",
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

void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

std::uint64_t CurrentUnixMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::string NewUuid(UuidKind kind) {
  static std::uint64_t sequence = 0;
  const auto seed = CurrentUnixMillis() + (++sequence);
  if (kind == UuidKind::session) {
    const auto generated = uuid::GenerateCompatibilityUnixTimeV7(seed);
    if (!generated.ok()) Fail("UUID generation failed");
    return uuid::UuidToString(generated.value);
  }
  const auto generated = uuid::GenerateEngineIdentityV7(kind, seed);
  if (!generated.ok()) Fail("UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

std::string HexText(std::string_view value) {
  constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (const unsigned char ch : value) {
    out.push_back(kDigits[(ch >> 4) & 0x0f]);
    out.push_back(kDigits[ch & 0x0f]);
  }
  return out;
}

std::string StableGrantUuid(std::size_t index) {
  std::string suffix = "000000000000";
  std::string hex;
  std::size_t value = 0x1000 + index + 1;
  do {
    const int digit = static_cast<int>(value & 0x0f);
    hex.insert(hex.begin(), "0123456789abcdef"[digit]);
    value >>= 4;
  } while (value != 0);
  suffix.replace(suffix.size() - hex.size(), hex.size(), hex);
  return "019f0a11-ce00-7000-8000-" + suffix;
}

api::EngineLocalizedName Name(std::string name) {
  return {"en", "primary", name, name, true};
}

api::EngineColumnDefinition Column(std::uint32_t ordinal, std::string name, std::string type) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.requested_column_uuid.canonical = NewUuid(UuidKind::object);
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_uuid.canonical = NewUuid(UuidKind::object);
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = std::move(type);
  column.descriptor.encoded_descriptor = "type=" + column.descriptor.canonical_type_name;
  return column;
}

api::EngineIndexDefinition CopyStreamUniqueIdIndex() {
  api::EngineIndexDefinition index;
  index.requested_index_uuid.canonical = NewUuid(UuidKind::object);
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

api::EngineRowValue CopyStreamRow(std::string row_uuid,
                                  std::string id,
                                  std::string payload) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"payload", TextValue(std::move(payload))});
  return row;
}

api::EngineRequestContext BaseContext(const std::filesystem::path& database_path,
                                      const std::string& database_uuid) {
  static const std::string seeder_principal_uuid = NewUuid(UuidKind::principal);
  static const std::string seeder_session_uuid = NewUuid(UuidKind::session);
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "sbsql-example-database-seed";
  context.database_path = database_path.string();
  context.database_uuid.canonical = database_uuid;
  context.principal_uuid.canonical = seeder_principal_uuid;
  context.session_uuid.canonical = seeder_session_uuid;
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("sbsql.example_database_seed");
  context.trace_tags.push_back("security.bootstrap");
  context.trace_tags.push_back("group:SEC");
  return context;
}

sblr::SblrOperationEnvelope Envelope(std::string operation_id, std::string opcode) {
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id), std::move(opcode), "sbsql.example_database_seed");
  static const std::string parser_package_uuid = NewUuid(UuidKind::object);
  static const std::string registry_snapshot_uuid = NewUuid(UuidKind::object);
  envelope.parser_package_uuid = parser_package_uuid;
  envelope.registry_snapshot_uuid = registry_snapshot_uuid;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  return envelope;
}

sblr::SblrDispatchResult Dispatch(std::string operation_id,
                                  std::string opcode,
                                  api::EngineRequestContext context,
                                  api::EngineApiRequest request = {},
                                  bool requires_transaction = false) {
  auto envelope = Envelope(operation_id, std::move(opcode));
  envelope.requires_transaction_context = requires_transaction;
  request.context = context;
  request.operation_id = operation_id;
  sblr::SblrDispatchRequest dispatch;
  dispatch.context = std::move(context);
  dispatch.envelope = std::move(envelope);
  dispatch.api_request = std::move(request);
  auto result = sblr::DispatchSblrOperation(dispatch);
  if (!result.accepted || !result.envelope_validated || !result.dispatched_to_api || !result.api_result.ok) {
    std::cerr << "seed dispatch failed for " << operation_id << '\n'
              << sblr::SerializeSblrDispatchResultToJson(result);
    std::exit(EXIT_FAILURE);
  }
  return result;
}

std::string CreateDatabase(const std::filesystem::path& database_path) {
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
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = false;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':' << created.diagnostic.message_key << '\n';
    Fail("example database creation failed");
  }
  return uuid::UuidToString(database_uuid.value.value);
}

void WriteAuthStore(const std::filesystem::path& database_path,
                    const std::string& database_uuid,
                    const std::string& user,
                    const std::string& verifier) {
  std::ofstream out(database_path.string() + ".sb.local_password_auth", std::ios::trunc);
  out << user << "\tlocal_password\t" << verifier << '\n';
  if (!out) Fail("example database local password verifier store creation failed");
  scratchbird::tests::database_lifecycle::CreateDurableLocalPasswordPrincipal(
      database_path,
      database_uuid,
      kLiveRouteAlicePrincipalUuid,
      user,
      verifier,
      17,
      "sbsql_example_database_seed");
  std::ofstream events(database_path.string() + ".sb.security_principal_events", std::ios::app);
  events << "SBSECPL1\tROLE\t0\t" << kLiveRouteSysarchRoleUuid << '\t'
         << HexText("sysarch") << '\t' << kLiveRouteAlicePrincipalUuid
         << "\tactive\t18\t0\n";
  events << "SBSECPL1\tGROUP\t0\t" << kLiveRoutePublicGroupUuid << '\t'
         << HexText("PUBLIC") << "\t\tactive\t19\t0\n";
  events << "SBSECPL1\tMEMBERSHIP\t0\t" << kLiveRouteSysarchMembershipUuid << '\t'
         << kLiveRouteAlicePrincipalUuid << '\t' << kLiveRouteSysarchRoleUuid
         << "\trole\t" << kLiveRouteAlicePrincipalUuid << "\t20\t0\n";
  events << "SBSECPL1\tMEMBERSHIP\t0\t" << kLiveRoutePublicMembershipUuid << '\t'
         << kLiveRouteAlicePrincipalUuid << '\t' << kLiveRoutePublicGroupUuid
         << "\tgroup\t" << kLiveRouteAlicePrincipalUuid << "\t21\t0\n";
  std::size_t grant_index = 0;
  for (const auto& right : SysarchRights()) {
    events << "SBSECPL1\tGRANT\t0\t" << StableGrantUuid(grant_index++) << '\t'
           << kLiveRouteSysarchRoleUuid << "\trole\t\t\t" << right << '\t'
           << kLiveRouteAlicePrincipalUuid << "\tallow\t"
           << (22 + grant_index) << "\t0\n";
  }
  if (!events) Fail("example database role/group authorization seed write failed");
}

std::string SchemaUuidForPath(const api::EngineRequestContext& context, const std::string& path) {
  for (const auto& schema : api::VisibleSchemaTreeRecords(context, context.local_transaction_id)) {
    for (const auto& name : schema.localized_names) {
      if (name.path == path) { return schema.schema_uuid; }
    }
  }
  return {};
}

std::string CreateBenchmarkUser(const api::EngineRequestContext& context, const std::string& user) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = NewUuid(UuidKind::principal);
  request.target_object.object_kind = "security_identity";
  request.localized_names.push_back(Name(user));
  request.option_envelopes.push_back("identity_kind:user");
  request.option_envelopes.push_back("principal_name:" + user);
  request.option_envelopes.push_back("home_schema_path:users." + user);
  const auto created = Dispatch("security.create_identity", "SBLR_SECURITY_CREATE_IDENTITY", context, std::move(request), true);
  for (const auto& evidence : created.api_result.evidence) {
    if (evidence.evidence_kind == "home_schema") { return evidence.evidence_id; }
  }
  Fail("security.create_identity did not return generated home schema UUID");
  return {};
}

void CreateTable(const api::EngineRequestContext& context,
                 std::string table_uuid,
                 std::string schema_uuid,
                 std::string name) {
  api::EngineApiRequest request;
  request.target_schema.uuid.canonical = std::move(schema_uuid);
  request.target_schema.object_kind = "schema";
  request.target_object.uuid.canonical = std::move(table_uuid);
  request.target_object.object_kind = "table";
  request.localized_names.push_back(Name(std::move(name)));
  request.columns.push_back(Column(0, "id", "text"));
  request.columns.push_back(Column(1, "payload", "text"));
  (void)Dispatch("ddl.create_table", "SBLR_DDL_CREATE_TABLE", context, std::move(request), true);
}

void CreateTableWithColumns(const api::EngineRequestContext& context,
                            std::string table_uuid,
                            std::string schema_uuid,
                            std::string name,
                            const std::vector<std::pair<std::string, std::string>>& columns) {
  api::EngineApiRequest request;
  request.target_schema.uuid.canonical = std::move(schema_uuid);
  request.target_schema.object_kind = "schema";
  request.target_object.uuid.canonical = std::move(table_uuid);
  request.target_object.object_kind = "table";
  request.localized_names.push_back(Name(std::move(name)));
  for (std::uint32_t ordinal = 0; ordinal < columns.size(); ++ordinal) {
    request.columns.push_back(Column(ordinal, columns[ordinal].first, columns[ordinal].second));
  }
  (void)Dispatch("ddl.create_table", "SBLR_DDL_CREATE_TABLE", context, std::move(request), true);
}

std::string CreateCopyStreamFixtureTable(const api::EngineRequestContext& context,
                                         const std::string& public_schema_uuid) {
  std::string table_uuid = NewUuid(UuidKind::object);
  api::EngineApiRequest request;
  request.target_schema.uuid.canonical = public_schema_uuid;
  request.target_schema.object_kind = "schema";
  request.target_object.uuid.canonical = table_uuid;
  request.target_object.object_kind = "table";
  request.localized_names.push_back(Name("sbsfc021_stream_table"));
  request.columns.push_back(Column(0, "id", "text"));
  request.columns.push_back(Column(1, "payload", "text"));
  request.indexes.push_back(CopyStreamUniqueIdIndex());
  (void)Dispatch("ddl.create_table", "SBLR_DDL_CREATE_TABLE", context, std::move(request), true);
  return table_uuid;
}

void CreateCurrentBenchmarkTables(const api::EngineRequestContext& context,
                                  const std::string& public_schema_uuid) {
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

void SeedCopyStreamFixtureRow(const api::EngineRequestContext& context,
                              const std::string& table_uuid) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = table_uuid;
  request.target_object.object_kind = "table";
  request.rows.push_back(CopyStreamRow(NewUuid(UuidKind::row),
                                       "6",
                                       "stream-baseline"));
  (void)Dispatch("dml.insert_rows", "SBLR_DML_INSERT_ROWS", context, std::move(request), true);
}

void SeedUserSchemas(const std::filesystem::path& database_path,
                     const std::string& database_uuid,
                     const std::string& user) {
  auto begin = Dispatch("transaction.begin",
                        "SBLR_TRANSACTION_BEGIN",
                        BaseContext(database_path, database_uuid));
  auto context = BaseContext(database_path, database_uuid);
  context.local_transaction_id = begin.api_result.local_transaction_id;
  context.transaction_uuid = begin.api_result.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id = begin.api_result.local_transaction_id;

  const std::string private_schema_uuid = CreateBenchmarkUser(context, user);
  const std::string public_schema_uuid = SchemaUuidForPath(context, "users.public");
  if (public_schema_uuid.empty()) Fail("users.public schema UUID was not visible after database create");
  CreateTable(context,
              NewUuid(UuidKind::object),
              public_schema_uuid,
              "benchmark_public_items");
  CreateTable(context, NewUuid(UuidKind::object), private_schema_uuid, "benchmark_private_items");
  const std::string copy_stream_table_uuid = CreateCopyStreamFixtureTable(context, public_schema_uuid);
  CreateCurrentBenchmarkTables(context, public_schema_uuid);
  (void)Dispatch("transaction.commit", "SBLR_TRANSACTION_COMMIT", context, {}, true);

  auto seed_begin = Dispatch("transaction.begin",
                             "SBLR_TRANSACTION_BEGIN",
                             BaseContext(database_path, database_uuid));
  auto seed_context = BaseContext(database_path, database_uuid);
  seed_context.local_transaction_id = seed_begin.api_result.local_transaction_id;
  seed_context.transaction_uuid = seed_begin.api_result.transaction_uuid;
  seed_context.snapshot_visible_through_local_transaction_id =
      seed_begin.api_result.local_transaction_id;
  SeedCopyStreamFixtureRow(seed_context, copy_stream_table_uuid);
  (void)Dispatch("transaction.commit", "SBLR_TRANSACTION_COMMIT", seed_context, {}, true);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: sbsql_example_database_seed <database> <user> <verifier-hex>\n";
    return EXIT_FAILURE;
  }
  const std::filesystem::path database_path = argv[1];
  const std::string user = argv[2];
  const std::string verifier = argv[3];
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture(
      "sbsql_example_database_seed");
  const std::string database_uuid = CreateDatabase(database_path);
  SeedUserSchemas(database_path, database_uuid, user);
  WriteAuthStore(database_path, database_uuid, user, verifier);
  std::cout << "sbsql_example_database_seed=passed database=" << database_path
            << " schemas=users,users.public,users." << user << '\n';
  return EXIT_SUCCESS;
}
