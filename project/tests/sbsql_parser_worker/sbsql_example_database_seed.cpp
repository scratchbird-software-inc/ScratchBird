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
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "hash_digest.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "sblr_transaction_begin_runtime.hpp"
#include "sblr_transaction_commit_runtime.hpp"
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

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

#ifndef SB_EXAMPLE_SEED_PACK_ROOT
#define SB_EXAMPLE_SEED_PACK_ROOT \
  "project/resources/seed-packs/initial-resource-pack"
#endif

constexpr std::string_view kBenchmarkPassword = "ScratchBird-E2E-2026!";
constexpr std::string_view kBenchmarkCredentialFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:"
    "verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";
constexpr std::string_view kDatatypeCatalogSnapshotUuid =
    "019d0000-0000-7000-8000-00000000d701";

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

api::EngineTypedValue BigintValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "int64";
  typed.descriptor.encoded_descriptor = "type=int64";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRowValue CopyStreamRow(std::string row_uuid,
                                  std::string id,
                                  std::string payload) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"id", BigintValue(std::move(id))});
  row.fields.push_back({"payload", TextValue(std::move(payload))});
  return row;
}

api::EngineRequestContext BaseContext(const std::filesystem::path& database_path,
                                      const std::string& database_uuid) {
  static const std::string seeder_principal_uuid = NewUuid(UuidKind::principal);
  static const std::string seeder_session_uuid = NewUuid(UuidKind::session);
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::embedded_in_process;
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
  context.datatype_catalog_snapshot_uuid.canonical =
      std::string(kDatatypeCatalogSnapshotUuid);
  context.datatype_catalog_generation = 1;
  context.datatype_registry_generation = 1;
  context.trace_tags.push_back("sbsql.example_database_seed");
  context.trace_tags.push_back("security.bootstrap");
  context.trace_tags.push_back("security.fixture_trace_authority");
  context.trace_tags.push_back("group:SEC");
  context.trace_tags.push_back("right:SEC_IDENTITY_ADMIN");
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  context.trace_tags.push_back("right:DML_MUTATE");
  return context;
}

sblr::SblrOperationEnvelope Envelope(std::string operation_id, std::string opcode) {
  const auto* registry_entry = sblr::LookupSblrOperation(operation_id);
  if (registry_entry == nullptr) {
    Fail("seed operation is absent from the canonical SBLR registry: " +
         operation_id);
  }
  if (registry_entry->opcode != opcode) {
    Fail("seed opcode mnemonic drifted from the canonical SBLR registry: " +
         operation_id);
  }
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id), std::move(opcode), "sbsql.example_database_seed");
  envelope.opcode_code = registry_entry->code;
  envelope.result_shape = registry_entry->result_contract;
  envelope.diagnostic_shape = "diagnostic_vector";
  static const std::string parser_package_uuid = NewUuid(UuidKind::object);
  static const std::string registry_snapshot_uuid = NewUuid(UuidKind::object);
  envelope.parser_package_uuid = parser_package_uuid;
  envelope.registry_snapshot_uuid = registry_snapshot_uuid;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  return envelope;
}

struct SeedTransaction {
  api::EngineRequestContext context;
  scratchbird::core::hash::Digest256 begin_admission_sha256{};
};

SeedTransaction BeginSeedTransaction(const std::filesystem::path& database_path,
                                     const std::string& database_uuid) {
  auto context = BaseContext(database_path, database_uuid);
  auto envelope = Envelope("engine.op.txn_begin", "SBLR_TXN_BEGIN");
  envelope.requires_transaction_context = false;

  sblr::SblrTransactionBeginOptionsV1 options;
  options.isolation_profile_uuid[0] = 1;
  options.isolation_profile_generation = 1;
  options.transaction_policy_snapshot_uuid[0] = 2;
  options.transaction_policy_generation = 1;
  options.read_mode = 1;
  options.authority_scope = 1;
  options.wait_policy = 1;
  auto body = sblr::EncodeSblrTransactionBeginOptionsV1(&options);
  if (body.empty()) Fail("canonical transaction-begin options failed to encode");
  const auto admission_sha =
      scratchbird::core::hash::ComputeSha256Digest(body);
  if (!admission_sha.ok()) Fail("canonical transaction-begin evidence hash failed");

  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "transaction.begin_options";
  operand.name = "options";
  operand.value_kind = sblr::SblrValueKind::transaction_begin_options;
  operand.value_body = std::move(body);
  envelope.operands.push_back(std::move(operand));

  api::EngineApiRequest api_request;
  api_request.context = context;
  api_request.operation_id = "engine.op.txn_begin";
  auto admitted = sblr::DispatchSblrOperation(
      {context, std::move(envelope), std::move(api_request), std::nullopt});
  if (!admitted.envelope_validated || !admitted.accepted ||
      !admitted.dispatched_to_api || !admitted.api_result.ok) {
    std::cerr << "seed canonical transaction-begin admission failed\n"
              << sblr::SerializeSblrDispatchResultToJson(admitted);
    Fail("canonical transaction-begin admission failed");
  }
  if (admitted.api_result.local_transaction_id != 0 ||
      !admitted.api_result.transaction_uuid.canonical.empty()) {
    Fail("SBLR transaction-begin admission published engine MGA state");
  }

  api::EngineBeginTransactionRequest begin;
  begin.context = context;
  begin.operation_id = "transaction.begin";
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  if (!begun.ok || begun.local_transaction_id == 0 ||
      begun.transaction_uuid.canonical.empty()) {
    Fail("engine-owned transaction begin failed after canonical SBLR admission");
  }
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return {std::move(context), admission_sha.digest};
}

void CommitSeedTransaction(const SeedTransaction& transaction) {
  auto envelope = Envelope("engine.op.txn_commit", "SBLR_TXN_COMMIT");
  envelope.requires_transaction_context = true;

  const auto parsed_transaction =
      uuid::ParseUuid(transaction.context.transaction_uuid.canonical);
  if (!parsed_transaction.ok()) Fail("seed transaction UUID is not canonical");
  sblr::SblrTransactionCommitOptionsV1 options;
  std::copy(parsed_transaction.value.bytes.begin(),
            parsed_transaction.value.bytes.end(),
            options.transaction_uuid.begin());
  options.local_transaction_id = transaction.context.local_transaction_id;
  options.admitted_handle_evidence_sha256 =
      transaction.begin_admission_sha256;
  options.commit_mode = 1;
  options.authority_scope = 1;
  options.wait_policy = 1;
  auto body = sblr::EncodeSblrTransactionCommitOptionsV1(&options);
  if (body.empty()) Fail("canonical transaction-commit options failed to encode");

  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "transaction.commit.options";
  operand.name = "options";
  operand.value_kind = sblr::SblrValueKind::transaction_commit_options;
  operand.value_body = std::move(body);
  envelope.operands.push_back(std::move(operand));

  api::EngineApiRequest api_request;
  api_request.context = transaction.context;
  api_request.operation_id = "engine.op.txn_commit";
  auto admitted = sblr::DispatchSblrOperation(
      {transaction.context, std::move(envelope), std::move(api_request),
       std::nullopt});
  if (!admitted.envelope_validated || !admitted.accepted ||
      !admitted.dispatched_to_api || !admitted.api_result.ok) {
    std::cerr << "seed canonical transaction-commit admission failed\n"
              << sblr::SerializeSblrDispatchResultToJson(admitted);
    Fail("canonical transaction-commit admission failed");
  }

  api::EngineCommitTransactionRequest commit;
  commit.context = transaction.context;
  commit.operation_id = "transaction.commit";
  const auto committed = api::EngineCommitTransaction(commit);
  if (!committed.ok || !committed.engine_finality_known ||
      committed.commit_finality_state != "committed_by_engine_inventory") {
    std::cerr << "commit ok=" << (committed.ok ? "true" : "false")
              << " finality_known="
              << (committed.engine_finality_known ? "true" : "false")
              << " state=" << committed.commit_finality_state << '\n';
    for (const auto& diagnostic : committed.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Fail("engine-owned transaction commit failed after canonical SBLR admission");
  }
}

std::string CreateDatabase(const std::filesystem::path& database_path,
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
  return uuid::UuidToString(database_uuid.value.value);
}

std::string SchemaUuidForPath(const api::EngineRequestContext& context, const std::string& path) {
  for (const auto& schema : api::VisibleSchemaTreeRecords(context, context.local_transaction_id)) {
    for (const auto& name : schema.localized_names) {
      if (name.path == path) { return schema.schema_uuid; }
    }
  }
  return {};
}


void CreateTable(const api::EngineRequestContext& context,
                 std::string table_uuid,
                 std::string schema_uuid,
                 std::string name) {
  api::EngineCreateTableRequest request;
  request.context = context;
  request.operation_id = "ddl.create_table";
  request.target_schema.uuid.canonical = std::move(schema_uuid);
  request.target_schema.object_kind = "schema";
  request.requested_table_uuid.canonical = std::move(table_uuid);
  request.table_names.push_back(Name(std::move(name)));
  request.table_columns.push_back(Column(0, "id", "text"));
  request.table_columns.push_back(Column(1, "payload", "text"));
  if (!api::EngineCreateTable(request).ok) {
    Fail("engine-owned benchmark table seed failed");
  }
}

void CreateTableWithColumns(const api::EngineRequestContext& context,
                            std::string table_uuid,
                            std::string schema_uuid,
                            std::string name,
                            const std::vector<std::pair<std::string, std::string>>& columns) {
  api::EngineCreateTableRequest request;
  request.context = context;
  request.operation_id = "ddl.create_table";
  request.target_schema.uuid.canonical = std::move(schema_uuid);
  request.target_schema.object_kind = "schema";
  request.requested_table_uuid.canonical = std::move(table_uuid);
  request.table_names.push_back(Name(std::move(name)));
  for (std::uint32_t ordinal = 0; ordinal < columns.size(); ++ordinal) {
    request.table_columns.push_back(
        Column(ordinal, columns[ordinal].first, columns[ordinal].second));
  }
  if (!api::EngineCreateTable(request).ok) {
    Fail("engine-owned benchmark table seed failed");
  }
}

std::string CreateCopyStreamFixtureTable(const api::EngineRequestContext& context,
                                         const std::string& public_schema_uuid) {
  std::string table_uuid = NewUuid(UuidKind::object);
  api::EngineCreateTableRequest request;
  request.context = context;
  request.operation_id = "ddl.create_table";
  request.target_schema.uuid.canonical = public_schema_uuid;
  request.target_schema.object_kind = "schema";
  request.requested_table_uuid.canonical = table_uuid;
  request.table_names.push_back(Name("sbsfc021_stream_table"));
  request.table_columns.push_back(Column(0, "id", "int64"));
  request.table_columns.push_back(Column(1, "payload", "text"));
  request.table_indexes.push_back(CopyStreamUniqueIdIndex());
  if (!api::EngineCreateTable(request).ok) {
    Fail("engine-owned copy-stream table seed failed");
  }
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
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.operation_id = "dml.insert_rows";
  request.target_table.uuid.canonical = table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows.push_back(CopyStreamRow(NewUuid(UuidKind::row),
                                             "6",
                                             "stream-baseline"));
  if (!api::EngineInsertRows(request).ok) {
    Fail("engine-owned copy-stream row seed failed");
  }
}

void SeedChunkedResponseFixtureRows(const api::EngineRequestContext& context,
                                    const std::string& table_uuid) {
  constexpr std::size_t kRowCount = 300;
  constexpr std::size_t kPayloadBytes = 3800;
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.operation_id = "dml.insert_rows";
  request.target_table.uuid.canonical = table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows.reserve(kRowCount);
  const std::string payload_prefix(kPayloadBytes - 1, 'x');
  for (std::size_t ordinal = 0; ordinal < kRowCount; ++ordinal) {
    api::EngineRowValue row;
    row.requested_row_uuid.canonical = NewUuid(UuidKind::row);
    row.fields.push_back(
        {"id", TextValue("chunk-response-" + std::to_string(ordinal + 1))});
    row.fields.push_back(
        {"payload",
         TextValue(payload_prefix +
                   static_cast<char>('a' + (ordinal % 26)))});
    request.input_rows.push_back(std::move(row));
  }
  if (!api::EngineInsertRows(request).ok) {
    Fail("engine-owned chunked-response row seed failed");
  }
}

void SeedUserSchemas(const std::filesystem::path& database_path,
                     const std::string& database_uuid) {
  auto transaction = BeginSeedTransaction(database_path, database_uuid);
  const auto& context = transaction.context;

  const std::string public_schema_uuid = SchemaUuidForPath(context, "users.public");
  if (public_schema_uuid.empty()) Fail("users.public schema UUID was not visible after database create");
  const std::string chunked_response_table_uuid = NewUuid(UuidKind::object);
  CreateTable(context,
              chunked_response_table_uuid,
              public_schema_uuid,
              "benchmark_public_items");
  const std::string copy_stream_table_uuid = CreateCopyStreamFixtureTable(context, public_schema_uuid);
  CreateCurrentBenchmarkTables(context, public_schema_uuid);
  CommitSeedTransaction(transaction);

  auto seed_transaction = BeginSeedTransaction(database_path, database_uuid);
  SeedCopyStreamFixtureRow(seed_transaction.context, copy_stream_table_uuid);
  SeedChunkedResponseFixtureRows(seed_transaction.context,
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
  const std::string database_uuid = CreateDatabase(database_path, user);
  SeedUserSchemas(database_path, database_uuid);
  std::cout << "sbsql_example_database_seed=passed database=" << database_path
            << " schemas=users,users.public principal=" << user << '\n';
  return EXIT_SUCCESS;
}
