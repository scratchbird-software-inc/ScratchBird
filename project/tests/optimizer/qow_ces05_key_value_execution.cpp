// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_execute.hpp"
#include "ast/ast.hpp"
#include "cst/cst.hpp"
#include "database_lifecycle.hpp"
#include "datatype_catalog_manifest.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "hash_digest.hpp"
#include "local_transaction_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "model_family_coordinator.hpp"
#include "model_family_executor.hpp"
#include "nosql/key_value_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace dt = scratchbird::core::datatypes;
namespace exec = scratchbird::engine::executor;
namespace hash = scratchbird::core::hash;
namespace opt = scratchbird::engine::optimizer;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
namespace sbsql = scratchbird::parser::sbsql;
namespace txn = scratchbird::transaction::mga;

constexpr std::string_view kStatementTimestamp =
    "2026-08-10T12:00:00Z";
constexpr std::string_view kBaseObjectUuid =
    "30000000-0000-4000-8000-000000000075";
constexpr std::string_view kDuplicateObjectUuid =
    "30000000-0000-4000-8000-000000000076";
constexpr std::string_view kInvalidExpiryObjectUuid =
    "30000000-0000-4000-8000-000000000077";
constexpr std::string_view kJoinObjectUuid =
    "30000000-0000-4000-8000-000000000078";

std::string CoreTypeUuid(std::string_view stable_name);
std::string DescriptorField(const std::string& encoded, std::string_view key);
api::EngineTypedValue TypedValue(const api::EngineDescriptor& descriptor,
                                 std::string encoded,
                                 bool is_null = false);

struct SignedRow {
  const char* row_uuid;
  const char* key;
  const char* value;
  const char* expires_at;
};

constexpr std::array<SignedRow, 8> kBaseRows{{
    {"30000000-0000-4000-8000-000000000001", "alpha", "A", nullptr},
    {"30000000-0000-4000-8000-000000000002", "alpine", "ALP",
     "2026-08-10T12:00:00.000000001Z"},
    {"30000000-0000-4000-8000-000000000003", "beta", "B",
     "2026-08-10T12:00:00.000000000Z"},
    {"30000000-0000-4000-8000-000000000004", "betamax", "BM",
     "2026-08-10T11:59:59.999999999Z"},
    {"30000000-0000-4000-8000-000000000005", "gamma", "G",
     "2026-08-10T12:00:01.000000000Z"},
    {"30000000-0000-4000-8000-000000000006", "zeta", "Z", nullptr},
    {"30000000-0000-4000-8000-000000000007", "éclair", "E", nullptr},
    {"30000000-0000-4000-8000-000000000008", "élan", "EL", nullptr},
}};

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-CES05-KEY-VALUE: " << detail << '\n';
  return condition;
}

std::uint64_t Seed() {
  static std::uint64_t ordinal = 0;
  return static_cast<std::uint64_t>(
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count()) +
         ++ordinal;
}

platform::TypedUuid NewUuid(const platform::UuidKind kind) {
  return uuid::GenerateEngineIdentityV7(kind, Seed()).value;
}

std::string NewUuidText(const platform::UuidKind kind) {
  return uuid::UuidToString(NewUuid(kind).value);
}

struct Fixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string schema_uuid;
  std::map<std::string, api::MgaRelationStorageDescriptor> descriptors;

  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
};

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical = NewUuidText(platform::UuidKind::principal);
  context.session_uuid.canonical = NewUuidText(platform::UuidKind::object);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 75;
  context.security_epoch = 76;
  context.resource_epoch = 77;
  context.name_resolution_epoch = 78;
  context.datatype_catalog_snapshot_uuid.canonical =
      "019d0000-0000-7000-8000-00000000d701";
  context.datatype_catalog_generation = 1;
  context.datatype_registry_generation = 1;
  return context;
}

bool Begin(const Fixture& fixture, std::string request_id,
           api::EngineRequestContext* context) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok) return Require(false, "transaction begin failed");
  *context = request.context;
  context->transaction_uuid = begun.transaction_uuid;
  context->local_transaction_id = begun.local_transaction_id;
  context->snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context->transaction_isolation_level = begun.isolation_level;
  return true;
}

bool Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  return Require(api::EngineCommitTransaction(request).ok,
                 "transaction commit failed");
}

bool Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  return Require(api::EngineRollbackTransaction(request).ok,
                 "transaction rollback failed");
}

bool SetNextTransactionId(const Fixture& fixture,
                          const std::uint64_t next_id) {
  const auto loaded =
      db::LoadLocalTransactionInventoryFromDatabase(
          fixture.database_path.string());
  if (!loaded.ok()) return Require(false, "transaction inventory load failed");
  auto inventory = loaded.inventory;
  inventory.next_local_transaction_id = next_id;
  const auto persisted = db::PersistLocalTransactionInventoryToDatabase(
      fixture.database_path.string(), inventory);
  return Require(persisted.ok(), "transaction inventory exact-ID seed failed");
}

api::EngineLocalizedName Name(std::string name) {
  return {"en", "primary", "", std::move(name), true};
}

api::EngineColumnDefinition Column(const std::uint32_t ordinal,
                                   std::string name,
                                   std::string type,
                                   const bool nullable) {
  api::EngineColumnDefinition column;
  column.requested_column_uuid.canonical =
      NewUuidText(platform::UuidKind::object);
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_kind = "scalar";
  // TIMESTAMP_TZ uses the canonical timestamp catalog identity while retaining
  // its exact storage semantic in the persisted descriptor carrier.
  column.descriptor.canonical_type_name =
      type == "timestamp_tz" ? "timestamp" : type;
  column.descriptor.encoded_descriptor = "canonical=" + type;
  column.ordinal = ordinal;
  column.nullable = nullable;
  return column;
}

void AddAuthorization(api::EngineRequestContext* context,
                      const std::string& right,
                      const std::string& object_uuid) {
  if (!context->authorization_context.present) {
    auto& authorization = context->authorization_context;
    authorization.present = true;
    authorization.authority_uuid.canonical =
        NewUuidText(platform::UuidKind::object);
    authorization.principal_uuid = context->principal_uuid;
    authorization.security_epoch = context->security_epoch;
    authorization.policy_epoch = 79;
    authorization.catalog_generation_id = context->catalog_generation_id;
    authorization.effective_subjects.push_back(
        {context->principal_uuid, "principal"});
  }
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical = NewUuidText(platform::UuidKind::object);
  grant.subject_uuid = context->principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical = object_uuid;
  grant.right = right;
  grant.security_epoch = context->security_epoch;
  context->authorization_context.grants.push_back(std::move(grant));
}

bool MakeFixture(Fixture* fixture) {
  fixture->directory = std::filesystem::temp_directory_path() /
                       ("scratchbird_rcp075_key_value_" +
                        std::to_string(Seed()));
  std::error_code error;
  if (!std::filesystem::create_directories(fixture->directory, error) ||
      error) {
    return Require(false, "fixture directory creation failed");
  }
  fixture->database_path = fixture->directory / "key_value.sbdb";
  db::DatabaseCreateConfig create;
  create.path = fixture->database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace);
  create.creation_unix_epoch_millis = Seed();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  if (!db::CreateDatabaseFile(create).ok()) {
    return Require(false, "fixture database creation failed");
  }
  fixture->database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture->schema_uuid = NewUuidText(platform::UuidKind::schema);

  api::EngineRequestContext context;
  if (!Begin(*fixture, "rcp075-metadata", &context)) return false;
  api::EngineCreateSchemaRequest schema;
  schema.context = context;
  schema.target_object.uuid.canonical = fixture->schema_uuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(Name("key_value_schema"));
  if (!api::EngineCreateSchema(schema).ok) {
    Rollback(context);
    return Require(false, "fixture schema creation failed");
  }
  const std::array<std::pair<std::string_view, std::string_view>, 4> tables{{
      {kBaseObjectUuid, "base"},
      {kDuplicateObjectUuid, "duplicate_visible"},
      {kInvalidExpiryObjectUuid, "invalid_expiry"},
      {kJoinObjectUuid, "relational_uuid_join"},
  }};
  for (const auto& [object_uuid, name] : tables) {
    api::EngineCreateTableRequest table;
    table.context = context;
    table.context.current_schema_uuid.canonical.clear();
    table.target_schema.uuid.canonical = fixture->schema_uuid;
    table.target_schema.object_kind = "schema";
    table.requested_table_uuid.canonical = std::string(object_uuid);
    table.table_names.push_back(Name(std::string(name)));
    if (object_uuid == kJoinObjectUuid) {
      table.table_columns.push_back(
          Column(0, "join_uuid", "uuid", false));
      table.table_columns.push_back(
          Column(1, "payload", "text", false));
    } else {
      table.table_columns.push_back(Column(0, "key", "text", false));
      table.table_columns.push_back(Column(1, "value", "text", false));
      table.table_columns.push_back(
          Column(2, "expires_at", "timestamp_tz", true));
    }
    const auto created = api::EngineCreateTable(table);
    if (!created.ok) {
      for (const auto& diagnostic : created.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.message_key;
        if (!diagnostic.detail.empty()) std::cerr << " (" << diagnostic.detail << ')';
        std::cerr << '\n';
      }
      Rollback(context);
      return Require(false, "key/value fixture table creation failed");
    }
    const auto loaded = api::LoadMgaRelationStorageDescriptor(
        context, std::string(object_uuid));
    const auto expected_columns = object_uuid == kJoinObjectUuid ? 2U : 3U;
    if (!loaded.ok ||
        loaded.descriptor.columns.size() != expected_columns ||
        loaded.descriptor.descriptor_generation == 0) {
      Rollback(context);
      return Require(false, "key/value fixture descriptor load failed");
    }
    fixture->descriptors.emplace(std::string(object_uuid), loaded.descriptor);
  }
  // Supplemental relational rows belong to the metadata transaction so the
  // signed key/value transaction inventory beginning at 800 is unchanged.
  const auto& join_descriptor =
      fixture->descriptors.at(std::string(kJoinObjectUuid));
  api::EngineInsertRowsRequest join_rows;
  join_rows.context = context;
  join_rows.target_table.uuid.canonical = std::string(kJoinObjectUuid);
  join_rows.target_table.object_kind = "table";
  join_rows.require_generated_row_uuid = false;
  join_rows.estimated_row_count = 2;
  for (const auto& [row_uuid, join_uuid, payload] :
       std::array<std::array<const char*, 3>, 2>{{
           {{"30000000-0000-4000-8000-000000000078",
             "30000000-0000-4000-8000-000000000001", "matched"}},
           {{"30000000-0000-4000-8000-000000000079",
             "30000000-0000-4000-8000-000000000099", "unmatched"}},
       }}) {
    api::EngineRowValue row;
    row.requested_row_uuid.canonical = row_uuid;
    row.fields = {
        {"join_uuid",
         TypedValue(join_descriptor.columns[0].value_descriptor, join_uuid)},
        {"payload",
         TypedValue(join_descriptor.columns[1].value_descriptor, payload)},
    };
    join_rows.input_rows.push_back(std::move(row));
  }
  const auto inserted_join_rows = api::EngineInsertRows(join_rows);
  if (!inserted_join_rows.ok || inserted_join_rows.inserted_count != 2) {
    Rollback(context);
    return Require(false, "supplemental relational join rows failed");
  }
  return Commit(context) && SetNextTransactionId(*fixture, 800);
}

api::EngineTypedValue TypedValue(const api::EngineDescriptor& descriptor,
                                 std::string encoded,
                                 const bool is_null) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = is_null ? std::string{} : std::move(encoded);
  value.setState(is_null ? api::EngineValueState::sql_null
                         : api::EngineValueState::value);
  return value;
}

api::EngineRowValue Row(const api::MgaRelationStorageDescriptor& descriptor,
                        const SignedRow& row) {
  api::EngineRowValue value;
  value.requested_row_uuid.canonical = row.row_uuid;
  value.fields = {
      {"key", TypedValue(descriptor.columns[0].value_descriptor, row.key)},
      {"value", TypedValue(descriptor.columns[1].value_descriptor, row.value)},
      {"expires_at",
       TypedValue(descriptor.columns[2].value_descriptor,
                  row.expires_at == nullptr ? std::string{} : row.expires_at,
                  row.expires_at == nullptr)},
  };
  return value;
}

bool Insert(const Fixture& fixture, const std::string& object_uuid,
            const std::vector<SignedRow>& rows, const bool commit,
            api::EngineRequestContext* retained_context = nullptr) {
  api::EngineRequestContext context;
  if (!Begin(fixture, "rcp075-insert-" + object_uuid, &context)) return false;
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = object_uuid;
  request.target_table.object_kind = "table";
  request.require_generated_row_uuid = false;
  request.estimated_row_count = rows.size();
  for (const auto& row : rows) {
    request.input_rows.push_back(Row(fixture.descriptors.at(object_uuid), row));
  }
  const auto inserted = api::EngineInsertRows(request);
  if (!inserted.ok || inserted.inserted_count != rows.size()) {
    Rollback(context);
    return Require(false, "key/value fixture insert failed");
  }
  if (retained_context != nullptr) {
    *retained_context = context;
    return true;
  }
  return commit ? Commit(context) : Rollback(context);
}

bool SeedFixtures(Fixture* fixture,
                  api::EngineRequestContext* active_other) {
  for (std::size_t index = 0; index < kBaseRows.size(); ++index) {
    if (!Insert(*fixture, std::string(kBaseObjectUuid),
                {kBaseRows[index]}, true)) {
      return false;
    }
  }
  const SignedRow ghost{
      "30000000-0000-4000-8000-000000000009", "ghost", "GHOST", nullptr};
  const SignedRow pending{
      "30000000-0000-4000-8000-000000000010", "pending", "PENDING", nullptr};
  const SignedRow old_alpha{
      "30000000-0000-4000-8000-000000000011", "alpha", "OLD", nullptr};
  if (!Insert(*fixture, std::string(kBaseObjectUuid), {ghost}, false) ||
      !Insert(*fixture, std::string(kBaseObjectUuid), {old_alpha}, false)) {
    return false;
  }
  const SignedRow duplicate_one{
      "30000000-0000-4000-8000-000000000012", "dup", "D1", nullptr};
  const SignedRow duplicate_two{
      "30000000-0000-4000-8000-000000000013", "dup", "D2", nullptr};
  if (!Insert(*fixture, std::string(kDuplicateObjectUuid), {duplicate_one},
              true) ||
      !Insert(*fixture, std::string(kDuplicateObjectUuid), {duplicate_two},
              true)) {
    return false;
  }
  api::EngineRequestContext malformed;
  if (!Begin(*fixture, "rcp075-invalid-expiry", &malformed)) return false;
  api::CrudRowVersionRecord row;
  row.creator_tx = malformed.local_transaction_id;
  row.table_uuid = std::string(kInvalidExpiryObjectUuid);
  row.row_uuid = "30000000-0000-4000-8000-000000000014";
  row.version_uuid = NewUuidText(platform::UuidKind::object);
  row.values = {{"key", "bad-expiry"},
                {"value", "BAD"},
                {"expires_at", "2026-08-10T12:00:00Z"}};
  std::uint64_t sequence = 0;
  const auto appended = api::AppendMgaRowVersion(malformed, row, &sequence);
  if (malformed.local_transaction_id != 812 || appended.error ||
      sequence == 0) {
    Rollback(malformed);
    return Require(false, "invalid expiry fixture append failed");
  }
  if (!Commit(malformed) || !SetNextTransactionId(*fixture, 850) ||
      !Insert(*fixture, std::string(kBaseObjectUuid), {pending}, false,
              active_other) ||
      active_other->local_transaction_id != 850 ||
      !SetNextTransactionId(*fixture, 900)) {
    return false;
  }
  api::EngineRequestContext high_water;
  if (!Begin(*fixture, "rcp075-high-water-900", &high_water) ||
      high_water.local_transaction_id != 900) {
    return Require(false, "snapshot high-water transaction 900 was not seeded");
  }
  return Commit(high_water);
}

bool PublishReaderContext(const Fixture& fixture,
                          api::EngineRequestContext* context) {
  if (!Begin(fixture, "rcp075-reader", context)) return false;
  context->statement_uuid.canonical = NewUuidText(platform::UuidKind::object);
  context->statement_receipt_uuid.canonical =
      NewUuidText(platform::UuidKind::object);
  context->statement_timestamp = std::string(kStatementTimestamp);
  api::EnginePublishStatementSnapshotRequest publish;
  publish.context = *context;
  const auto snapshot = api::EnginePublishStatementSnapshot(publish);
  if (!snapshot.ok) return Require(false, "statement snapshot publish failed");
  context->statement_snapshot_uuid = snapshot.statement_snapshot_uuid;
  context->snapshot_visible_through_local_transaction_id =
      snapshot.snapshot_vector.visible_committed_high_watermark;
  context->statement_metadata_snapshot_engine_owned = true;
  context->statement_metadata_snapshot_uuid.canonical =
      NewUuidText(platform::UuidKind::object);
  context->statement_metadata_snapshot_visible_through_local_transaction_id =
      context->snapshot_visible_through_local_transaction_id;
  context->catalog_epoch_uuid.canonical = NewUuidText(platform::UuidKind::object);
  context->optimizer_capability_snapshot_uuid.canonical =
      NewUuidText(platform::UuidKind::object);
  context->optimizer_resource_snapshot_uuid.canonical =
      NewUuidText(platform::UuidKind::object);
  context->optimizer_route_snapshot_uuid.canonical =
      NewUuidText(platform::UuidKind::object);
  context->optimizer_route_epoch = 80;
  context->optimizer_route_generation = 81;
  context->optimizer_memory_budget_bytes = 4 * 1024 * 1024;
  context->optimizer_maximum_candidate_count = 4096;
  context->optimizer_maximum_memo_groups = 4096;
  context->optimizer_maximum_search_steps = 16384;
  context->optimizer_maximum_planning_time_ns = 1'000'000'000;
  context->current_monotonic_ns = std::to_string(Seed());
  context->query_cancellation_requested = [] { return false; };
  if (context->snapshot_visible_through_local_transaction_id != 900 ||
      std::ranges::find(
          snapshot.snapshot_vector.active_excluded_local_transaction_ids,
          std::uint64_t{850}) ==
          snapshot.snapshot_vector.active_excluded_local_transaction_ids.end()) {
    return Require(false,
                   "signed high-water 900 / active-other [850] cohort drifted");
  }
  for (const auto object_uuid :
       {kBaseObjectUuid, kDuplicateObjectUuid, kInvalidExpiryObjectUuid,
        kJoinObjectUuid}) {
    AddAuthorization(context, "SELECT", std::string(object_uuid));
  }
  return true;
}

api::EngineTypedValue RequestText(const std::string& value) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = NewUuidText(platform::UuidKind::object);
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "text";
  descriptor.encoded_descriptor = "canonical=text";
  return TypedValue(descriptor, value);
}

api::EngineBoundKeyValueReadRequestV1 Request(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& descriptor,
    const api::EngineBoundKeyValueReadOperationV1 operation,
    std::vector<std::string> values,
    const bool fallback = false) {
  api::EngineBoundKeyValueReadRequestV1 request;
  request.context = context;
  request.operation = operation;
  request.object_uuid = descriptor.relation_uuid.canonical;
  for (const auto& value : values) request.request_values.push_back(RequestText(value));
  request.statement_timestamp = context.statement_timestamp;
  request.expected_descriptor_uuid = descriptor.descriptor_uuid.canonical;
  request.expected_descriptor_generation = descriptor.descriptor_generation;
  request.selected_alternative_uuid = NewUuidText(platform::UuidKind::object);
  request.capability_uuid = NewUuidText(platform::UuidKind::object);
  request.provider_uuid = NewUuidText(platform::UuidKind::object);
  request.provider_generation = descriptor.descriptor_generation;
  request.maximum_request_keys = 128;
  request.maximum_request_bytes = 64 * 1024;
  request.maximum_scanned_row_versions = 4096;
  request.maximum_decoded_bytes = 1024 * 1024;
  request.maximum_output_rows = 4096;
  request.maximum_value_bytes = 1024 * 1024;
  request.maximum_result_bytes = 1024 * 1024;
  request.maximum_memory_bytes = 2 * 1024 * 1024;
  request.exact_fallback_selected = fallback;
  request.cancellation_requested = [] { return false; };
  return request;
}

exec::PhysicalMgaStatementContext PhysicalMga(
    const api::EngineRequestContext& context) {
  api::EngineResolveStatementSnapshotRequest resolve;
  resolve.context = context;
  const auto snapshot = api::EngineResolveStatementSnapshot(resolve);
  exec::PhysicalMgaStatementContext mga;
  if (!snapshot.ok) return mga;
  const auto& descriptor = snapshot.snapshot_vector;
  mga.statement_uuid = context.statement_uuid.canonical;
  mga.statement_timestamp = context.statement_timestamp;
  mga.owning_transaction_uuid = context.transaction_uuid.canonical;
  mga.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  mga.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  mga.owning_local_transaction_id = descriptor.owning_transaction.value;
  mga.visible_committed_high_watermark =
      descriptor.visible_committed_high_watermark;
  mga.oldest_active_transaction_id =
      descriptor.oldest_active_transaction.value;
  mga.oldest_interesting_transaction_id =
      descriptor.oldest_interesting_transaction.value;
  mga.oldest_snapshot_transaction_id =
      descriptor.oldest_snapshot_transaction.value;
  mga.retention_horizon_transaction_id =
      descriptor.retention_horizon_transaction.value;
  mga.active_excluded_local_transaction_ids =
      descriptor.active_excluded_local_transaction_ids;
  mga.in_doubt_excluded_local_transaction_ids =
      descriptor.in_doubt_excluded_local_transaction_ids;
  mga.snapshot_kind = scratchbird::transaction::mga::SnapshotVectorKindName(
      descriptor.snapshot_kind);
  mga.publication_inventory_next_local_transaction_id =
      descriptor.publication_inventory_next_local_transaction_id;
  mga.inventory_authoritative = descriptor.inventory_authoritative;
  mga.complete = descriptor.complete;
  mga.current = true;
  return mga;
}

bool VerifySignedTransactionInventory(
    const Fixture& fixture,
    const api::EngineRequestContext& reader_context) {
  const auto loaded = db::LoadLocalTransactionInventoryFromDatabase(
      fixture.database_path.string());
  if (!loaded.ok()) {
    return Require(false, "signed transaction inventory could not be loaded");
  }
  const auto state = [&](const std::uint64_t local_id)
      -> std::optional<txn::TransactionState> {
    const auto entry = std::ranges::find_if(
        loaded.inventory.entries, [&](const auto& candidate) {
          return candidate.identity.local_id.value == local_id;
        });
    return entry == loaded.inventory.entries.end()
               ? std::nullopt
               : std::optional<txn::TransactionState>(entry->state);
  };
  bool exact = loaded.inventory.next_local_transaction_id == 902 &&
               reader_context.local_transaction_id == 901 &&
               state(808) == txn::TransactionState::rolled_back &&
               state(809) == txn::TransactionState::rolled_back &&
               state(850) == txn::TransactionState::active &&
               state(900) == txn::TransactionState::committed &&
               state(901) == txn::TransactionState::active;
  for (std::uint64_t local_id = 800; local_id <= 807; ++local_id) {
    exact = exact && state(local_id) == txn::TransactionState::committed;
  }
  for (const std::uint64_t local_id : {810ULL, 811ULL, 812ULL}) {
    exact = exact && state(local_id) == txn::TransactionState::committed;
  }
  return Require(exact,
                 "signed transaction IDs/states 800-812,850,900-902 drifted");
}

bool CoordinatorSelectionProof(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& descriptor) {
  opt::ModelFamilyCoordinatorRequestV1 request;
  request.family_id = "key_value";
  request.operation_id = "KEY_VALUE_GET";
  request.logical_operator_id = "LOGICAL_KEY_VALUE_SOURCE_V1";
  request.logical_node_id = 75;
  request.object_uuid = descriptor.relation_uuid.canonical;
  request.output_descriptor_ids = {101, 102, 103};
  request.mga_statement_context = PhysicalMga(context);
  request.bound_sblr_tree_uuid = NewUuidText(platform::UuidKind::object);
  request.catalog_epoch_uuid = context.catalog_epoch_uuid.canonical;
  request.security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  request.capability_snapshot_uuid =
      context.optimizer_capability_snapshot_uuid.canonical;
  request.resource_snapshot_uuid =
      context.optimizer_resource_snapshot_uuid.canonical;
  request.statistics_snapshot_uuid =
      NewUuidText(platform::UuidKind::object);
  request.route_snapshot_uuid = context.optimizer_route_snapshot_uuid.canonical;
  request.catalog_generation = context.catalog_generation_id;
  request.current_catalog_generation = context.catalog_generation_id;
  request.security_epoch = context.security_epoch;
  request.policy_epoch = context.authorization_context.policy_epoch;
  request.resource_epoch = context.resource_epoch;
  request.statistics_generation = context.catalog_generation_id;
  request.route_epoch = context.optimizer_route_epoch;
  request.route_generation = context.optimizer_route_generation;
  request.memory_budget_bytes = context.optimizer_memory_budget_bytes;

  const auto candidate = [&](const bool fallback, const std::uint64_t cost) {
    opt::ModelFamilyCandidateV1 value;
    value.alternative_uuid = NewUuidText(platform::UuidKind::object);
    value.provider_uuid = NewUuidText(platform::UuidKind::object);
    value.capability_uuid = NewUuidText(platform::UuidKind::object);
    value.implementation_id = "physical_key_value_scan_v1";
    value.provider_generation = descriptor.descriptor_generation;
    value.available = true;
    value.exact = true;
    value.exact_collection_fallback = fallback;
    value.cost.cost_vector_uuid = NewUuidText(platform::UuidKind::object);
    value.cost.cpu_units = cost;
    value.cost.sequential_read_units = cost;
    value.cost.memory_bytes_required = 4096;
    return value;
  };
  request.candidates = {candidate(false, 1), candidate(true, 2)};
  const auto native = opt::CoordinateKeyValueFamilySourceV1(request);
  const bool native_exact =
      native.accepted && native.selected && native.data_access_allowed &&
      !native.exact_fallback_selected &&
      native.selected_candidate.alternative_uuid ==
          request.candidates[0].alternative_uuid &&
      native.selected_candidate.provider_uuid ==
          request.candidates[0].provider_uuid;

  request.candidates[0].available = false;
  const auto fallback = opt::CoordinateKeyValueFamilySourceV1(request);
  const bool fallback_exact =
      fallback.accepted && fallback.selected && fallback.data_access_allowed &&
      fallback.exact_fallback_selected &&
      fallback.selected_candidate.alternative_uuid ==
          request.candidates[1].alternative_uuid &&
      fallback.selected_candidate.provider_uuid ==
          request.candidates[1].provider_uuid &&
      fallback.selected_candidate.alternative_uuid !=
          native.selected_candidate.alternative_uuid &&
      fallback.selected_candidate.provider_uuid !=
          native.selected_candidate.provider_uuid;

  request.candidates[1].available = false;
  const auto unavailable = opt::CoordinateKeyValueFamilySourceV1(request);
  const bool unavailable_exact =
      !unavailable.accepted && !unavailable.selected &&
      !unavailable.data_access_allowed &&
      unavailable.diagnostic_id ==
          "SB_MODEL_KEY_VALUE_EXACT_FALLBACK_UNAVAILABLE_V1";
  return Require(native_exact && fallback_exact && unavailable_exact,
                 "native/fallback coordinator selection identity drifted");
}

struct ExchangeRun {
  exec::ModelFamilyExecutionResultV1 result;
  std::size_t provider_calls{0};
  std::size_t cleanup_calls{0};
};

ExchangeRun ExecuteThroughCommonSpine(
    api::EngineBoundKeyValueReadRequestV1 provider_request,
    const api::MgaRelationStorageDescriptor& descriptor,
    const bool fallback_available = true,
    const std::optional<std::uint64_t> current_catalog_generation = {},
    const std::optional<std::uint64_t> current_provider_generation = {},
    const bool cancel_before_execution = false) {
  ExchangeRun observed;
  const auto mga = PhysicalMga(provider_request.context);
  provider_request.selected_alternative_uuid =
      NewUuidText(platform::UuidKind::object);
  provider_request.capability_uuid =
      NewUuidText(platform::UuidKind::object);
  provider_request.provider_uuid = NewUuidText(platform::UuidKind::object);
  provider_request.provider_generation = descriptor.descriptor_generation;

  exec::ModelFamilyExecutionRequestV1 request;
  auto& input = request.input;
  input.family_id = "key_value";
  input.operation_id =
      provider_request.operation ==
              api::EngineBoundKeyValueReadOperationV1::kGet
          ? "KEY_VALUE_GET"
          : provider_request.operation ==
                    api::EngineBoundKeyValueReadOperationV1::kMultiGet
                ? "KEY_VALUE_MULTI_GET"
                : "KEY_VALUE_PREFIX_RANGE";
  input.object_uuid = provider_request.object_uuid;
  input.physical_node_id = 75;
  input.selected_alternative_uuid =
      provider_request.selected_alternative_uuid;
  input.capability_uuid = provider_request.capability_uuid;
  input.provider_uuid = provider_request.provider_uuid;
  input.provider_generation = provider_request.provider_generation;
  input.result_handle_uuid = NewUuidText(platform::UuidKind::object);
  input.causal_counter_id = 1;
  input.output_descriptor_ids = {101, 102, 103};
  input.mga_statement_context = mga;
  input.catalog_epoch_uuid =
      provider_request.context.catalog_epoch_uuid.canonical;
  input.security_context_uuid = provider_request.context
                                    .authorization_context.authority_uuid
                                    .canonical;
  input.policy_snapshot_uuid = NewUuidText(platform::UuidKind::object);
  input.resource_contract_uuid = NewUuidText(platform::UuidKind::object);
  input.catalog_generation = provider_request.context.catalog_generation_id;
  input.descriptor_generation = descriptor.descriptor_generation;
  input.security_generation = provider_request.context.security_epoch;
  input.policy_generation =
      provider_request.context.authorization_context.policy_epoch;
  input.resource_generation = provider_request.context.resource_epoch;
  input.maximum_rows = provider_request.maximum_output_rows;
  input.maximum_cells = provider_request.maximum_output_rows * 3;
  input.maximum_memory_bytes = provider_request.maximum_memory_bytes;
  input.exact_fallback_selected = provider_request.exact_fallback_selected;
  if (provider_request.operation ==
      api::EngineBoundKeyValueReadOperationV1::kMultiGet) {
    input.maximum_key_value_request_count =
        provider_request.maximum_request_keys;
    input.maximum_key_value_request_bytes =
        provider_request.maximum_request_bytes;
    for (const auto& value : provider_request.request_values) {
      if (std::ranges::find(input.key_value_request_order,
                            value.encoded_value) ==
          input.key_value_request_order.end()) {
        input.key_value_request_order.push_back(value.encoded_value);
      }
    }
  }

  request.capability.capability_uuid = input.capability_uuid;
  request.capability.family_id = input.family_id;
  request.capability.provider_uuid = input.provider_uuid;
  request.capability.provider_generation = input.provider_generation;
  request.capability.available = true;
  request.capability.exact = true;
  request.capability.exact_collection_fallback_available = fallback_available;
  request.capability.cancellation_supported = true;
  request.capability.cleanup_supported = true;
  request.capability.residual_recheck_supported = true;
  request.capability.base_row_mga_recheck_supported = true;
  request.capability.security_recheck_supported = true;
  request.security_admitted = true;
  request.current_catalog_generation = current_catalog_generation.value_or(
      input.catalog_generation);
  request.current_descriptor_generation = input.descriptor_generation;
  request.current_security_generation = input.security_generation;
  request.current_policy_generation = input.policy_generation;
  request.current_resource_generation = input.resource_generation;
  request.current_provider_generation = current_provider_generation.value_or(
      input.provider_generation);
  request.current_mga_statement_context = input.mga_statement_context;
  request.cancellation_requested =
      [cancel_before_execution] { return cancel_before_execution; };
  request.cleanup_provider = [&observed] { ++observed.cleanup_calls; };
  request.exact_fallback_selected = input.exact_fallback_selected;

  std::array<exec::ExecutorColumnDescriptor, 3> columns;
  const auto canonical_uuid_type = CoreTypeUuid("uuid");
  api::EngineDescriptor row_uuid_descriptor;
  row_uuid_descriptor.descriptor_uuid = descriptor.descriptor_uuid;
  row_uuid_descriptor.descriptor_kind = "scalar";
  row_uuid_descriptor.canonical_type_name = "uuid";
  row_uuid_descriptor.encoded_descriptor =
      "canonical=uuid;type_uuid=" + canonical_uuid_type +
      ";nullable=false";
  columns[0] = {"row_uuid", row_uuid_descriptor, false, 101};
  auto key_descriptor = descriptor.columns[0].value_descriptor;
  auto value_descriptor = descriptor.columns[1].value_descriptor;
  key_descriptor.descriptor_kind = "scalar";
  value_descriptor.descriptor_kind = "scalar";
  if (!Require(!canonical_uuid_type.empty() &&
                   DescriptorField(row_uuid_descriptor.encoded_descriptor,
                                   "type_uuid") == canonical_uuid_type &&
                   DescriptorField(key_descriptor.encoded_descriptor,
                                   "type_uuid") == CoreTypeUuid("character") &&
                   DescriptorField(value_descriptor.encoded_descriptor,
                                   "type_uuid") == CoreTypeUuid("character"),
               "public UUID/TEXT descriptor identities are not engine-owned")) {
    return observed;
  }
  columns[1] = {"key", std::move(key_descriptor), false, 102};
  columns[2] = {"value", std::move(value_descriptor), false, 103};
  request.execute_provider =
      [provider_request, columns, &observed](const auto& selected) mutable {
        ++observed.provider_calls;
        exec::ModelProviderExecutionResultV1 provider;
        const auto read = api::EngineBoundKeyValueReadV1(provider_request);
        provider.data_access_observed = read.data_access_observed;
        provider.rows_examined = read.scanned_row_version_count;
        if (!read.ok) {
          provider.diagnostic_id = read.diagnostics.empty()
                                       ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                                       : read.diagnostics.front().code;
          provider.detail = read.diagnostics.empty()
                                ? "key/value provider failed"
                                : read.diagnostics.front().detail;
          return provider;
        }
        provider.ok = true;
        auto& batch = provider.provider_batch;
        batch.provider_uuid = selected.provider_uuid;
        batch.provider_generation = selected.provider_generation;
        batch.selected_alternative_uuid = selected.selected_alternative_uuid;
        batch.capability_uuid = selected.capability_uuid;
        batch.exact_fallback_selected = selected.exact_fallback_selected;
        batch.result_handle_uuid = selected.result_handle_uuid;
        batch.causal_counter_id = selected.causal_counter_id;
        batch.output_descriptor_ids = selected.output_descriptor_ids;
        batch.batch.columns.assign(columns.begin(), columns.end());
        batch.mga_statement_context = selected.mga_statement_context;
        batch.security_receipt_uuid =
            NewUuidText(platform::UuidKind::object);
        batch.properties.property_uuid =
            NewUuidText(platform::UuidKind::object);
        batch.properties.ordering_id = read.ordering_id;
        batch.properties.partitioning_id = "single_local_partition";
        batch.properties.uniqueness_id = "key";
        batch.properties.exact = true;
        batch.properties.residual_recheck_complete = true;
        batch.properties.base_row_mga_recheck_complete = true;
        batch.properties.security_recheck_complete = true;
        batch.residual_recheck_complete = true;
        batch.base_row_mga_recheck_complete = true;
        batch.security_recheck_complete = true;
        for (const auto& row : read.rows) {
          exec::DescriptorTuple tuple;
          tuple.values = {
              TypedValue(columns[0].descriptor, row.row_uuid),
              TypedValue(columns[1].descriptor, row.key),
              TypedValue(columns[2].descriptor, row.value),
          };
          batch.batch.rows.push_back(std::move(tuple));
          exec::ModelProviderRowIdentityV1 identity;
          identity.row_uuid = row.row_uuid;
          identity.key = row.key;
          batch.ordered_row_identities.push_back(std::move(identity));
        }
        return provider;
      };
  observed.result = exec::ExecuteModelFamilySourceV1(request);
  return observed;
}

std::string Diagnostic(const api::EngineApiResult& result) {
  return result.diagnostics.empty() ? std::string{}
                                    : result.diagnostics.front().code;
}

std::string Stream(const api::EngineBoundKeyValueReadResultV1& result) {
  std::string bytes;
  for (const auto& row : result.rows) {
    bytes += row.row_uuid;
    bytes.push_back('\t');
    bytes += row.key;
    bytes.push_back('\t');
    bytes += row.value;
    bytes.push_back('\n');
  }
  return bytes;
}

std::string Stream(const exec::ModelFamilyExecutionResultV1& result) {
  std::string bytes;
  for (const auto& row : result.output.batch.rows) {
    if (row.values.size() != 3) return {};
    bytes += row.values[0].encoded_value;
    bytes.push_back('\t');
    bytes += row.values[1].encoded_value;
    bytes.push_back('\t');
    bytes += row.values[2].encoded_value;
    bytes.push_back('\n');
  }
  return bytes;
}

std::string Digest(const std::string& bytes) {
  const auto digest = hash::ComputeSha256Digest(
      reinterpret_cast<const platform::byte*>(bytes.data()), bytes.size());
  return digest.ok() ? hash::HexLower(digest.digest) : std::string{};
}

bool ProviderMatrix(const Fixture& fixture,
                    const api::EngineRequestContext& context,
                    std::set<std::string>* completed) {
  bool passed = true;
  const auto& base = fixture.descriptors.at(std::string(kBaseObjectUuid));
  const auto run = [&](const api::EngineBoundKeyValueReadOperationV1 operation,
                       std::vector<std::string> values,
                       const bool fallback = false) {
    return api::EngineBoundKeyValueReadV1(
        Request(context, base, operation, std::move(values), fallback));
  };
  const auto exact = api::EngineBoundKeyValueReadOperationV1::kGet;
  const auto multi = api::EngineBoundKeyValueReadOperationV1::kMultiGet;
  const auto prefix = api::EngineBoundKeyValueReadOperationV1::kPrefixRange;

  const auto kv01 = run(exact, {"alpha"});
  if (!kv01.ok) {
    for (const auto& diagnostic : kv01.diagnostics) {
      std::cerr << "KV-01 diagnostic " << diagnostic.code << ": "
                << diagnostic.detail << '\n';
    }
  }
  passed &= Require(kv01.ok && kv01.rows.size() == 1 &&
                        kv01.rows.front().row_uuid == kBaseRows[0].row_uuid &&
                        kv01.rows.front().key == "alpha" &&
                        kv01.rows.front().value == "A" &&
                        kv01.ordering_id == "key_value_unordered_v1",
                    "KV-01 exact hit drifted");
  completed->insert("KV-01");
  const auto kv02 = run(exact, {"missing"});
  passed &= Require(kv02.ok && kv02.rows.empty(), "KV-02 miss drifted");
  completed->insert("KV-02");
  const auto kv03 = run(exact, {"beta"});
  passed &= Require(kv03.ok && kv03.rows.empty(), "KV-03 TTL equality drifted");
  completed->insert("KV-03");
  const auto kv04 = run(exact, {"betamax"});
  passed &= Require(kv04.ok && kv04.rows.empty(), "KV-04 expired row drifted");
  completed->insert("KV-04");
  const auto kv05 = run(exact, {"alpine"});
  passed &= Require(kv05.ok && kv05.rows.size() == 1,
                    "KV-05 one-nanosecond TTL boundary drifted");
  completed->insert("KV-05");
  const auto kv06 = run(exact, {"alpha"});
  passed &= Require(kv06.ok && kv06.rows.size() == 1,
                    "KV-06 invisible duplicate handling drifted");
  completed->insert("KV-06");
  const auto ghost = run(exact, {"ghost"});
  const auto pending = run(exact, {"pending"});
  passed &= Require(ghost.ok && ghost.rows.empty() && pending.ok &&
                        pending.rows.empty(),
                    "KV-07 MGA-invisible rows leaked");
  completed->insert("KV-07");

  const auto kv08 = run(
      multi, {"gamma", "missing", "alpha", "gamma", "beta", "alpine", "alpha"});
  passed &= Require(
      kv08.ok && kv08.rows.size() == 3 && kv08.rows[0].key == "gamma" &&
          kv08.rows[1].key == "alpha" && kv08.rows[2].key == "alpine" &&
          kv08.ordering_id == "first_distinct_request_order_v1" &&
          Digest(Stream(kv08)) ==
              "78293410b49797fa69903b3157d5343f6cfc3f14cb7cbb94a5fa00eb43939e59",
      "KV-08 multi-get stream drifted");
  completed->insert("KV-08");
  const auto kv09 = run(prefix, {"al"});
  passed &= Require(
      kv09.ok && kv09.rows.size() == 2 &&
          Digest(Stream(kv09)) ==
              "2e97cd0ad1924d5455fd11e924513b90d816622f1394e4589990ba8fad504e34",
      "KV-09 prefix stream drifted");
  completed->insert("KV-09");
  const auto kv10 = run(prefix, {"be"});
  passed &= Require(kv10.ok && kv10.rows.empty(),
                    "KV-10 expired prefix rows leaked");
  completed->insert("KV-10");
  const auto kv11 = run(prefix, {""});
  passed &= Require(
      kv11.ok && kv11.rows.size() == 6 &&
          kv11.ordering_id == "key_utf8_byte_ascending_v1" &&
          Digest(Stream(kv11)) ==
              "64253fce9e07494d9e5699c72c3695375ecc0e3cf688dd7ddfbaa8117e8e56ca",
      "KV-11 empty-prefix stream drifted");
  completed->insert("KV-11");
  const auto kv12 = run(prefix, {"é"});
  passed &= Require(
      kv12.ok && kv12.rows.size() == 2 &&
          Digest(Stream(kv12)) ==
              "f6b836b708f59402efcad67072f40ae37781555ec9173f084abb6c50d797f92c",
      "KV-12 UTF-8 byte prefix stream drifted");
  completed->insert("KV-12");

  auto duplicate_request = Request(
      context, fixture.descriptors.at(std::string(kDuplicateObjectUuid)),
      exact, {"dup"});
  const auto kv13 = api::EngineBoundKeyValueReadV1(duplicate_request);
  passed &= Require(!kv13.ok && kv13.rows.empty() &&
                        Diagnostic(kv13) ==
                            "SB_MODEL_KEY_VALUE_DUPLICATE_VISIBLE_KEY_REFUSED_V1" &&
                        kv13.data_access_observed,
                    "KV-13 duplicate-visible refusal drifted");
  completed->insert("KV-13");
  auto invalid_request = Request(
      context, fixture.descriptors.at(std::string(kInvalidExpiryObjectUuid)),
      prefix, {""});
  const auto kv14 = api::EngineBoundKeyValueReadV1(invalid_request);
  passed &= Require(!kv14.ok && kv14.rows.empty() &&
                        Diagnostic(kv14) ==
                            "SB_MODEL_KEY_VALUE_EXPIRES_AT_INVALID_V1" &&
                        kv14.data_access_observed,
                    "KV-14 invalid-expiry refusal drifted");
  completed->insert("KV-14");

  auto null_key = Request(context, base, exact, {"alpha"});
  null_key.request_values.front().setState(api::EngineValueState::sql_null);
  const auto kv16_null = api::EngineBoundKeyValueReadV1(null_key);
  auto non_text = Request(context, base, prefix, {"al"});
  non_text.request_values.front().descriptor.canonical_type_name = "int64";
  const auto kv16_type = api::EngineBoundKeyValueReadV1(non_text);
  passed &= Require(!kv16_null.ok && !kv16_type.ok &&
                        Diagnostic(kv16_null) ==
                            "SB_MODEL_KEY_VALUE_KEY_TYPE_REFUSED_V1" &&
                        Diagnostic(kv16_type) ==
                            "SB_MODEL_KEY_VALUE_KEY_TYPE_REFUSED_V1" &&
                        !kv16_null.data_access_observed &&
                        !kv16_type.data_access_observed,
                    "KV-16 request type refusal drifted");
  completed->insert("KV-16");

  auto empty_multi = Request(context, base, multi, {"alpha"});
  empty_multi.request_values.clear();
  const auto kv17 = api::EngineBoundKeyValueReadV1(empty_multi);
  passed &= Require(!kv17.ok &&
                        Diagnostic(kv17) ==
                            "SB_MODEL_KEY_VALUE_MULTI_GET_EMPTY_REFUSED_V1" &&
                        !kv17.data_access_observed,
                    "KV-17 empty multi-get refusal drifted");
  completed->insert("KV-17");

  auto invalid_utf8 = Request(context, base, exact, {"alpha"});
  invalid_utf8.request_values.front().encoded_value =
      std::string("\xc3\x28", 2);
  const auto kv18 = api::EngineBoundKeyValueReadV1(invalid_utf8);
  passed &= Require(!kv18.ok &&
                        Diagnostic(kv18) ==
                            "SB_MODEL_KEY_VALUE_TEXT_INVALID_V1" &&
                        !kv18.data_access_observed,
                    "KV-18 malformed request UTF-8 drifted");
  completed->insert("KV-18");

  auto missing_timestamp = Request(context, base, exact, {"alpha"});
  missing_timestamp.statement_timestamp.clear();
  const auto kv19_missing = api::EngineBoundKeyValueReadV1(missing_timestamp);
  auto mutated_timestamp = Request(context, base, exact, {"alpha"});
  mutated_timestamp.statement_timestamp = "2026-08-10T12:00:00.1Z";
  const auto kv19_mutated = api::EngineBoundKeyValueReadV1(mutated_timestamp);
  passed &= Require(!kv19_missing.ok && !kv19_mutated.ok &&
                        Diagnostic(kv19_missing) ==
                            "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1" &&
                        Diagnostic(kv19_mutated) ==
                            "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1" &&
                        !kv19_missing.data_access_observed &&
                        !kv19_mutated.data_access_observed,
                    "KV-19 timestamp refusal drifted");
  completed->insert("KV-19");

  auto request_bound = Request(context, base, multi, {"alpha", "gamma"});
  request_bound.maximum_request_keys = 1;
  const auto kv20 = api::EngineBoundKeyValueReadV1(request_bound);
  passed &= Require(!kv20.ok &&
                        Diagnostic(kv20) ==
                            "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1" &&
                        !kv20.data_access_observed,
                    "KV-20 request-bound refusal drifted");
  completed->insert("KV-20");

  auto output_bound = Request(context, base, prefix, {""});
  output_bound.maximum_output_rows = 1;
  const auto kv21 = api::EngineBoundKeyValueReadV1(output_bound);
  passed &= Require(!kv21.ok && kv21.rows.empty() &&
                        Diagnostic(kv21) ==
                            "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1" &&
                        kv21.data_access_observed,
                    "KV-21 acquired resource refusal drifted");
  completed->insert("KV-21");

  auto before_cancel = Request(context, base, exact, {"alpha"});
  before_cancel.cancellation_requested = [] { return true; };
  const auto kv22 = api::EngineBoundKeyValueReadV1(before_cancel);
  passed &= Require(!kv22.ok && kv22.rows.empty() &&
                        Diagnostic(kv22) ==
                            "SB_MODEL_EXECUTION_CANCELLED_V1" &&
                        !kv22.data_access_observed,
                    "KV-22 pre-access cancellation drifted");
  completed->insert("KV-22");

  std::size_t during_calls = 0;
  auto during_cancel = Request(context, base, prefix, {""});
  during_cancel.cancellation_requested = [&during_calls] {
    return ++during_calls >= 4;
  };
  const auto kv23 = api::EngineBoundKeyValueReadV1(during_cancel);
  passed &= Require(!kv23.ok && kv23.rows.empty() &&
                        Diagnostic(kv23) ==
                            "SB_MODEL_EXECUTION_CANCELLED_V1" &&
                        kv23.data_access_observed,
                    "KV-23 acquired cancellation drifted");
  completed->insert("KV-23");

  std::size_t observed_calls = 0;
  auto count_calls = Request(context, base, prefix, {""});
  count_calls.cancellation_requested = [&observed_calls] {
    ++observed_calls;
    return false;
  };
  const auto counted = api::EngineBoundKeyValueReadV1(count_calls);
  std::size_t publication_calls = 0;
  auto publication_cancel = Request(context, base, prefix, {""});
  publication_cancel.cancellation_requested =
      [&publication_calls, observed_calls] {
        return ++publication_calls >= observed_calls;
      };
  const auto kv24 = api::EngineBoundKeyValueReadV1(publication_cancel);
  passed &= Require(counted.ok && observed_calls > 1 && !kv24.ok &&
                        kv24.rows.empty() &&
                        Diagnostic(kv24) ==
                            "SB_MODEL_EXECUTION_CANCELLED_V1" &&
                        kv24.data_access_observed,
                    "KV-24 publication-boundary cancellation drifted");
  completed->insert("KV-24");

  const auto kv25 = run(
      multi, {"gamma", "missing", "alpha", "gamma", "beta", "alpine", "alpha"},
      true);
  const auto kv26 = run(prefix, {""}, true);
  passed &= Require(kv25.ok && kv25.exact_fallback_observed &&
                        Stream(kv25) == Stream(kv08) &&
                        Digest(Stream(kv25)) == Digest(Stream(kv08)) &&
                        kv26.ok && kv26.exact_fallback_observed &&
                        Stream(kv26) == Stream(kv11) &&
                        Digest(Stream(kv26)) == Digest(Stream(kv11)),
                    "KV-25/KV-26 exact-fallback equivalence drifted");
  completed->insert("KV-25");
  completed->insert("KV-26");

  auto malformed_identity = Request(context, base, exact, {"alpha"});
  malformed_identity.abi_version = 2;
  const auto kv28 = api::EngineBoundKeyValueReadV1(malformed_identity);
  passed &= Require(!kv28.ok &&
                        Diagnostic(kv28) ==
                            "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1" &&
                        !kv28.data_access_observed,
                    "KV-28 semantic identity refusal drifted");
  completed->insert("KV-28");

  auto stale = Request(context, base, exact, {"alpha"});
  ++stale.expected_descriptor_generation;
  const auto supplemental_post_read_stale =
      api::EngineBoundKeyValueReadV1(stale);
  passed &= Require(!supplemental_post_read_stale.ok &&
                        Diagnostic(supplemental_post_read_stale) ==
                            "SB_MODEL_CATALOG_GENERATION_STALE_V1" &&
                        !supplemental_post_read_stale.data_access_observed,
                    "supplemental descriptor drift escaped pre-access refusal");
  const auto catalog_stale = ExecuteThroughCommonSpine(
      Request(context, base, exact, {"alpha"}), base, true,
      context.catalog_generation_id + 1);
  const auto provider_stale = ExecuteThroughCommonSpine(
      Request(context, base, exact, {"alpha"}), base, true, {},
      base.descriptor_generation + 1);
  passed &= Require(
      !catalog_stale.result.accepted &&
          !catalog_stale.result.execution_started &&
          !catalog_stale.result.root_published &&
          catalog_stale.result.diagnostic_id ==
              "SB_MODEL_CATALOG_GENERATION_STALE_V1" &&
          catalog_stale.provider_calls == 0 &&
          catalog_stale.cleanup_calls == 0 &&
          !provider_stale.result.accepted &&
          !provider_stale.result.execution_started &&
          !provider_stale.result.root_published &&
          provider_stale.result.diagnostic_id ==
              "SB_MODEL_PROVIDER_GENERATION_STALE_V1" &&
          provider_stale.provider_calls == 0 &&
          provider_stale.cleanup_calls == 0,
      "KV-29 catalog/provider generation pre-access refusal drifted");
  completed->insert("KV-29");

  auto denied_context = context;
  denied_context.authorization_context.grants.clear();
  const auto kv30 = api::EngineBoundKeyValueReadV1(
      Request(denied_context, base, exact, {"alpha"}));
  passed &= Require(!kv30.ok &&
                        Diagnostic(kv30) ==
                            "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1" &&
                        !kv30.data_access_observed,
                    "KV-30 security-cohort refusal drifted");
  completed->insert("KV-30");

  const auto replay01 = run(exact, {"alpha"});
  const auto replay08 = run(
      multi, {"gamma", "missing", "alpha", "gamma", "beta", "alpine", "alpha"});
  const auto replay11 = run(prefix, {""});
  const auto replay25 = run(
      multi, {"gamma", "missing", "alpha", "gamma", "beta", "alpine", "alpha"},
      true);
  const auto replay26 = run(prefix, {""}, true);
  passed &= Require(Stream(replay01) == Stream(kv01) &&
                        Stream(replay08) == Stream(kv08) &&
                        Stream(replay11) == Stream(kv11) &&
                        Stream(replay25) == Stream(kv25) &&
                        Stream(replay26) == Stream(kv26),
                    "KV-32 deterministic replay drifted");
  completed->insert("KV-32");

  const auto prove_success =
      [&](api::EngineBoundKeyValueReadRequestV1 request,
          const api::EngineBoundKeyValueReadResultV1& expected,
          const std::string_view id) {
        const auto execution =
            ExecuteThroughCommonSpine(std::move(request), base);
        if (!execution.result.accepted) {
          std::cerr << id << " common-spine diagnostic "
                    << execution.result.diagnostic_id << ": "
                    << execution.result.detail << '\n';
        }
        return Require(
            execution.result.accepted &&
                execution.result.execution_started &&
                execution.result.data_access_observed &&
                execution.result.root_published &&
                execution.result.cleanup_complete &&
                execution.result.cleanup_count == 1 &&
                execution.cleanup_calls == 1 &&
                execution.provider_calls == 1 &&
                execution.result.output.exact_exchange_validated &&
                Stream(execution.result) == Stream(expected),
            std::string(id) +
                " common-spine cleanup/root/stream proof drifted");
      };
  passed &= prove_success(Request(context, base, exact, {"alpha"}), kv01,
                          "KV-01");
  passed &= prove_success(Request(context, base, exact, {"missing"}), kv02,
                          "KV-02");
  passed &= prove_success(Request(context, base, exact, {"beta"}), kv03,
                          "KV-03");
  passed &= prove_success(Request(context, base, exact, {"betamax"}), kv04,
                          "KV-04");
  passed &= prove_success(Request(context, base, exact, {"alpine"}), kv05,
                          "KV-05");
  passed &= prove_success(Request(context, base, exact, {"alpha"}), kv06,
                          "KV-06");
  passed &= prove_success(Request(context, base, exact, {"ghost"}), ghost,
                          "KV-07a");
  passed &= prove_success(Request(context, base, exact, {"pending"}), pending,
                          "KV-07b");
  passed &= prove_success(
      Request(context, base, multi,
              {"gamma", "missing", "alpha", "gamma", "beta", "alpine",
               "alpha"}),
      kv08, "KV-08");
  passed &= prove_success(Request(context, base, prefix, {"al"}), kv09,
                          "KV-09");
  passed &= prove_success(Request(context, base, prefix, {"be"}), kv10,
                          "KV-10");
  passed &= prove_success(Request(context, base, prefix, {""}), kv11,
                          "KV-11");
  passed &= prove_success(Request(context, base, prefix, {"é"}), kv12,
                          "KV-12");
  passed &= prove_success(
      Request(context, base, multi,
              {"gamma", "missing", "alpha", "gamma", "beta", "alpine",
               "alpha"},
              true),
      kv25, "KV-25");
  passed &= prove_success(Request(context, base, prefix, {""}, true), kv26,
                          "KV-26");

  auto acquired_resource_request = Request(context, base, prefix, {""});
  acquired_resource_request.maximum_output_rows = 1;
  const auto acquired_resource = ExecuteThroughCommonSpine(
      acquired_resource_request, base);
  passed &= Require(
      !acquired_resource.result.accepted &&
          acquired_resource.result.execution_started &&
          acquired_resource.result.data_access_observed &&
          !acquired_resource.result.root_published &&
          acquired_resource.result.diagnostic_id ==
              "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1" &&
          acquired_resource.result.cleanup_complete &&
          acquired_resource.result.cleanup_count == 1 &&
          acquired_resource.cleanup_calls == 1 &&
          acquired_resource.provider_calls == 1,
      "KV-21 common-spine atomic cleanup refusal drifted");

  const auto pre_access_cancel = ExecuteThroughCommonSpine(
      Request(context, base, exact, {"alpha"}), base, true, {}, {}, true);
  passed &= Require(
      !pre_access_cancel.result.accepted &&
          !pre_access_cancel.result.execution_started &&
          !pre_access_cancel.result.data_access_observed &&
          !pre_access_cancel.result.root_published &&
          pre_access_cancel.result.diagnostic_id ==
              "SB_MODEL_EXECUTION_CANCELLED_V1" &&
          pre_access_cancel.result.cleanup_count == 0 &&
          pre_access_cancel.cleanup_calls == 0 &&
          pre_access_cancel.provider_calls == 0,
      "KV-22 common-spine pre-access cancellation/cleanup drifted");

  std::size_t acquired_cancel_calls = 0;
  auto acquired_cancel_request = Request(context, base, prefix, {""});
  acquired_cancel_request.cancellation_requested = [&acquired_cancel_calls] {
    return ++acquired_cancel_calls >= 4;
  };
  const auto acquired_cancel = ExecuteThroughCommonSpine(
      acquired_cancel_request, base);
  passed &= Require(
      !acquired_cancel.result.accepted &&
          acquired_cancel.result.execution_started &&
          acquired_cancel.result.data_access_observed &&
          !acquired_cancel.result.root_published &&
          acquired_cancel.result.diagnostic_id ==
              "SB_MODEL_EXECUTION_CANCELLED_V1" &&
          acquired_cancel.result.cleanup_complete &&
          acquired_cancel.result.cleanup_count == 1 &&
          acquired_cancel.cleanup_calls == 1 &&
          acquired_cancel.provider_calls == 1,
      "KV-23 common-spine atomic cleanup cancellation drifted");

  std::size_t final_cancel_calls = 0;
  auto final_cancel_request = Request(context, base, prefix, {""});
  final_cancel_request.cancellation_requested =
      [&final_cancel_calls, observed_calls] {
        return ++final_cancel_calls >= observed_calls;
      };
  const auto final_cancel = ExecuteThroughCommonSpine(
      final_cancel_request, base);
  passed &= Require(
      !final_cancel.result.accepted && final_cancel.result.execution_started &&
          final_cancel.result.data_access_observed &&
          !final_cancel.result.root_published &&
          final_cancel.result.diagnostic_id ==
              "SB_MODEL_EXECUTION_CANCELLED_V1" &&
          final_cancel.result.cleanup_complete &&
          final_cancel.result.cleanup_count == 1 &&
          final_cancel.cleanup_calls == 1 && final_cancel.provider_calls == 1,
      "KV-24 common-spine publication-boundary cleanup drifted");

  const auto unavailable = ExecuteThroughCommonSpine(
      Request(context, base, multi, {"alpha"}, true), base, false);
  passed &= Require(
      !unavailable.result.accepted && !unavailable.result.execution_started &&
          !unavailable.result.root_published &&
          unavailable.result.diagnostic_id ==
              "SB_MODEL_KEY_VALUE_EXACT_FALLBACK_UNAVAILABLE_V1" &&
          unavailable.provider_calls == 0 && unavailable.cleanup_calls == 0,
      "KV-27 unavailable exact fallback did not refuse before access");
  completed->insert("KV-27");
  return passed;
}

bool FrontdoorRefusalMatrix(std::set<std::string>* completed) {
  const auto has_diagnostic = [](const auto& ast,
                                 const std::string_view diagnostic) {
    return std::ranges::any_of(
        ast.native_relational.messages.diagnostics,
        [&](const auto& entry) { return entry.code == diagnostic; });
  };
  const auto non_equality = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM KEY_VALUE_SOURCE(app.kv_fixture) AS kv "
      "WHERE KV_KEY(kv) < 'alpha';"));
  const auto donor = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM REDIS_COMMAND('GET alpha');"));
  const auto write = sbsql::BuildAst(sbsql::BuildCst(
      "UPDATE KEY_VALUE_SOURCE(app.kv_fixture) SET value = 'x';"));
  bool passed = true;
  passed &= Require(
      has_diagnostic(non_equality,
                     "SB_MODEL_KEY_VALUE_OPERATOR_REFUSED_V1"),
      "KV-15 non-equality KV_KEY did not receive the exact refusal");
  completed->insert("KV-15");
  passed &= Require(
      has_diagnostic(donor, "SB_MODEL_GRAMMAR_DONOR_TEXT_REFUSED_V1") &&
          has_diagnostic(write, "SB_MODEL_QUERY_WRITE_REFUSED_V1"),
      "KV-31 donor text or key/value query write was not refused exactly");
  completed->insert("KV-31");
  return passed;
}

std::string DescriptorField(const std::string& encoded,
                            const std::string_view key) {
  std::size_t offset = 0;
  while (offset <= encoded.size()) {
    const auto end = encoded.find(';', offset);
    const auto field = std::string_view(encoded).substr(
        offset, end == std::string::npos ? std::string::npos : end - offset);
    const auto equal = field.find('=');
    if (equal != std::string_view::npos && field.substr(0, equal) == key) {
      return std::string(field.substr(equal + 1));
    }
    if (end == std::string::npos) break;
    offset = end + 1;
  }
  return {};
}

std::string CoreTypeUuid(const std::string_view stable_name) {
  static const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto count = std::ranges::count_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  const auto descriptor = std::ranges::find_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  if (count != 1 || descriptor == manifest.manifest.descriptor_rows.end() ||
      !descriptor->descriptor_uuid.valid()) {
    return {};
  }
  const auto descriptor_uuid =
      uuid::UuidToString(descriptor->descriptor_uuid.value);
  const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
      "019d0000-0000-7000-8000-00000000d701",
      manifest.manifest.catalog_epoch, 1, descriptor_uuid,
      descriptor->descriptor_epoch);
  return identity.ok ? identity.row.type_uuid : descriptor_uuid;
}

api::TypedRelationalDag KeyValueDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& descriptor,
    const api::EngineBoundKeyValueReadOperationV1 operation,
    const std::vector<std::string>& values) {
  api::TypedRelationalDag dag;
  dag.wire_version = 2;
  dag.bound_sblr_tree_uuid = NewUuidText(platform::UuidKind::object);
  dag.bound_catalog_epoch_uuid = context.catalog_epoch_uuid.canonical;
  dag.bound_security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  dag.statement_uuid = context.statement_uuid.canonical;
  dag.statement_timestamp = context.statement_timestamp;
  dag.owning_transaction_uuid = context.transaction_uuid.canonical;
  dag.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  dag.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  dag.local_transaction_id = context.local_transaction_id;
  dag.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  dag.root_node_id = 1;

  api::RelationalTypeDescriptor row_uuid;
  row_uuid.descriptor_id = 101;
  row_uuid.descriptor_uuid = descriptor.descriptor_uuid.canonical;
  row_uuid.type_uuid = CoreTypeUuid("uuid");
  row_uuid.nullability = api::RelationalNullability::kNonNull;
  dag.descriptors.push_back(std::move(row_uuid));
  for (std::size_t ordinal = 0; ordinal < 2; ++ordinal) {
    api::RelationalTypeDescriptor type;
    type.descriptor_id = static_cast<std::uint32_t>(102 + ordinal);
    type.descriptor_uuid =
        descriptor.columns[ordinal].value_descriptor.descriptor_uuid.canonical;
    type.type_uuid = DescriptorField(
        descriptor.columns[ordinal].value_descriptor.encoded_descriptor,
        "type_uuid");
    type.nullability = api::RelationalNullability::kNonNull;
    dag.descriptors.push_back(std::move(type));
  }
  api::RelationalTypeDescriptor boolean;
  boolean.descriptor_id = 104;
  boolean.descriptor_uuid = NewUuidText(platform::UuidKind::object);
  boolean.type_uuid = CoreTypeUuid("boolean");
  boolean.nullability = api::RelationalNullability::kNonNull;
  dag.descriptors.push_back(std::move(boolean));

  static constexpr std::array<std::string_view, 3> names{
      "row_uuid", "key", "value"};
  for (std::size_t ordinal = 0; ordinal < names.size(); ++ordinal) {
    api::RelationalExpressionRecord output;
    output.expression_id = static_cast<std::uint32_t>(1 + ordinal);
    output.expression_kind = api::RelationalExpressionKind::kIdentifier;
    output.result_descriptor_id = static_cast<std::uint32_t>(101 + ordinal);
    output.bound_name_uuid = ordinal == 0
                                 ? descriptor.relation_uuid.canonical
                                 : descriptor.columns[ordinal - 1]
                                       .column_uuid.canonical;
    dag.expressions.push_back(std::move(output));
    dag.outputs.push_back(
        {static_cast<std::uint32_t>(1 + ordinal), 1,
         static_cast<std::uint32_t>(1 + ordinal), std::string(names[ordinal]),
         static_cast<std::uint32_t>(101 + ordinal), true,
         static_cast<std::uint32_t>(ordinal)});
  }
  api::RelationalExpressionRecord alias;
  alias.expression_id = 10;
  alias.expression_kind = api::RelationalExpressionKind::kIdentifier;
  alias.result_descriptor_id = 102;
  alias.bound_name_uuid = descriptor.relation_uuid.canonical;
  dag.expressions.push_back(std::move(alias));
  std::vector<std::uint32_t> literal_ids;
  for (std::size_t ordinal = 0; ordinal < values.size(); ++ordinal) {
    api::RelationalExpressionRecord literal;
    literal.expression_id = static_cast<std::uint32_t>(11 + ordinal);
    literal.expression_kind = api::RelationalExpressionKind::kLiteral;
    literal.result_descriptor_id = 102;
    literal.literal_kind = api::RelationalLiteralKind::kString;
    literal.literal_or_parameter_ref = values[ordinal];
    literal_ids.push_back(literal.expression_id);
    dag.expressions.push_back(std::move(literal));
  }
  api::RelationalExpressionRecord model_operation;
  model_operation.expression_id = 20;
  model_operation.expression_kind =
      api::RelationalExpressionKind::kFunctionCall;
  model_operation.result_descriptor_id = 102;
  model_operation.operator_name =
      operation == api::EngineBoundKeyValueReadOperationV1::kGet
          ? "KV_KEY"
          : operation == api::EngineBoundKeyValueReadOperationV1::kMultiGet
                ? "KV_MULTI_GET"
                : "KV_PREFIX";
  model_operation.child_expression_ids = {10};
  if (operation != api::EngineBoundKeyValueReadOperationV1::kGet) {
    model_operation.child_expression_ids.insert(
        model_operation.child_expression_ids.end(), literal_ids.begin(),
        literal_ids.end());
  }
  dag.expressions.push_back(std::move(model_operation));

  api::RelationalDagNode source;
  source.node_id = 1;
  source.node_kind = api::RelationalDagNodeKind::kScan;
  source.output_descriptor_ids = {101, 102, 103};
  source.bound_expression_ids = {1, 2, 3};
  if (operation == api::EngineBoundKeyValueReadOperationV1::kGet) {
    api::RelationalExpressionRecord equality;
    equality.expression_id = 30;
    equality.expression_kind = api::RelationalExpressionKind::kBinary;
    equality.child_expression_ids = {20, literal_ids.front()};
    equality.result_descriptor_id = 104;
    equality.operator_name = "=";
    dag.expressions.push_back(std::move(equality));
  }
  source.required_object_uuids = {descriptor.relation_uuid.canonical};
  source.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(std::move(source));
  return dag;
}

api::TypedRelationalDag KeyValueUnaryCompositionDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& descriptor) {
  auto dag = KeyValueDag(
      context, descriptor,
      api::EngineBoundKeyValueReadOperationV1::kPrefixRange, {""});
  dag.root_node_id = 5;

  api::RelationalExpressionRecord predicate;
  predicate.expression_id = 40;
  predicate.expression_kind = api::RelationalExpressionKind::kLiteral;
  predicate.result_descriptor_id = 104;
  predicate.literal_kind = api::RelationalLiteralKind::kBoolean;
  predicate.literal_or_parameter_ref = "TRUE";
  dag.expressions.push_back(std::move(predicate));
  api::RelationalDagNode filter;
  filter.node_id = 2;
  filter.node_kind = api::RelationalDagNodeKind::kFilter;
  filter.input_node_ids = {1};
  filter.output_descriptor_ids = {101, 102, 103};
  filter.bound_expression_ids = {40};
  filter.semantic_variant_id = "filter.where.v1";
  dag.nodes.push_back(std::move(filter));

  for (std::size_t ordinal = 0; ordinal < 3; ++ordinal) {
    dag.outputs.push_back(
        {static_cast<std::uint32_t>(20 + ordinal), 3,
         static_cast<std::uint32_t>(1 + ordinal),
         ordinal == 0 ? "row_uuid" : ordinal == 1 ? "key" : "value",
         static_cast<std::uint32_t>(101 + ordinal), true,
         static_cast<std::uint32_t>(ordinal)});
  }
  api::RelationalDagNode project;
  project.node_id = 3;
  project.node_kind = api::RelationalDagNodeKind::kProject;
  project.input_node_ids = {2};
  project.output_descriptor_ids = {101, 102, 103};
  project.bound_expression_ids = {1, 2, 3};
  project.semantic_variant_id = "project.select-list.v1";
  dag.nodes.push_back(std::move(project));

  const auto ordering_uuid = NewUuidText(platform::UuidKind::object);
  api::RelationalPropertyRecord ordering;
  ordering.property_uuid = ordering_uuid;
  ordering.property_kind = api::RelationalPropertyKind::kOrdering;
  ordering.origin_node_id = 4;
  ordering.ordering_terms.push_back(
      {1, api::RelationalPropertySortDirection::kAscending,
       api::RelationalPropertyNullPlacement::kNullsLast, {}});
  dag.properties.push_back(std::move(ordering));
  api::RelationalDagNode sort;
  sort.node_id = 4;
  sort.node_kind = api::RelationalDagNodeKind::kSort;
  sort.input_node_ids = {3};
  sort.output_descriptor_ids = {101, 102, 103};
  sort.bound_expression_ids = {1};
  sort.semantic_variant_id = "sort.required-order.v1";
  sort.required_property_uuids = {ordering_uuid};
  sort.delivered_property_uuids = {ordering_uuid};
  dag.nodes.push_back(std::move(sort));

  api::RelationalTypeDescriptor row_number_descriptor;
  row_number_descriptor.descriptor_id = 105;
  row_number_descriptor.descriptor_uuid =
      NewUuidText(platform::UuidKind::object);
  row_number_descriptor.type_uuid = CoreTypeUuid("int64");
  row_number_descriptor.nullability = api::RelationalNullability::kNonNull;
  dag.descriptors.push_back(std::move(row_number_descriptor));
  constexpr std::string_view kRowNumberFunctionUuid =
      "019de5fc-2400-7539-bcce-00eef3ae7220";
  api::RelationalExpressionRecord row_number;
  row_number.expression_id = 41;
  row_number.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  row_number.result_descriptor_id = 105;
  row_number.function_uuid = std::string(kRowNumberFunctionUuid);
  dag.expressions.push_back(std::move(row_number));
  for (std::size_t ordinal = 0; ordinal < 3; ++ordinal) {
    dag.outputs.push_back(
        {static_cast<std::uint32_t>(30 + ordinal), 5,
         static_cast<std::uint32_t>(1 + ordinal),
         ordinal == 0 ? "row_uuid" : ordinal == 1 ? "key" : "value",
         static_cast<std::uint32_t>(101 + ordinal), true,
         static_cast<std::uint32_t>(ordinal)});
  }
  dag.outputs.push_back({33, 5, 41, "row_number", 105, true, 3});
  const auto window_uuid = NewUuidText(platform::UuidKind::object);
  api::RelationalPropertyRecord window_property;
  window_property.property_uuid = window_uuid;
  window_property.property_kind = api::RelationalPropertyKind::kWindow;
  window_property.origin_node_id = 5;
  window_property.dependency_property_uuids = {ordering_uuid};
  window_property.window_frame_descriptor_uuid =
      NewUuidText(platform::UuidKind::object);
  dag.properties.push_back(std::move(window_property));
  api::RelationalDagNode window;
  window.node_id = 5;
  window.node_kind = api::RelationalDagNodeKind::kWindow;
  window.input_node_ids = {4};
  window.output_descriptor_ids = {101, 102, 103, 105};
  window.bound_expression_ids = {1, 41};
  window.semantic_variant_id = "window.row-number.v1";
  window.required_property_uuids = {ordering_uuid};
  window.delivered_property_uuids = {ordering_uuid, window_uuid};
  dag.nodes.push_back(std::move(window));
  api::RelationalWindowDefinitionRecord definition;
  definition.window_id = 1;
  definition.relation_node_id = 5;
  definition.ordering_terms = {
      {1, api::RelationalPropertySortDirection::kAscending,
       api::RelationalPropertyNullPlacement::kNullsLast, {}}};
  dag.window_definitions.push_back(std::move(definition));
  api::RelationalWindowInvocationRecord invocation;
  invocation.invocation_id = 1;
  invocation.relation_node_id = 5;
  invocation.function_expression_id = 41;
  invocation.window_definition_id = 1;
  invocation.function_abi_version = 1;
  invocation.builtin_id = "sb.window.row_number";
  invocation.function_uuid = std::string(kRowNumberFunctionUuid);
  invocation.result_descriptor_id = 105;
  invocation.output_name_utf8 = "row_number";
  dag.window_invocations.push_back(std::move(invocation));

  return dag;
}

api::TypedRelationalDag KeyValueCteLimitDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& descriptor) {
  auto dag = KeyValueDag(
      context, descriptor,
      api::EngineBoundKeyValueReadOperationV1::kPrefixRange, {""});
  dag.root_node_id = 5;
  api::RelationalExpressionRecord predicate;
  predicate.expression_id = 40;
  predicate.expression_kind = api::RelationalExpressionKind::kLiteral;
  predicate.result_descriptor_id = 104;
  predicate.literal_kind = api::RelationalLiteralKind::kBoolean;
  predicate.literal_or_parameter_ref = "TRUE";
  dag.expressions.push_back(std::move(predicate));
  api::RelationalDagNode filter;
  filter.node_id = 2;
  filter.node_kind = api::RelationalDagNodeKind::kFilter;
  filter.input_node_ids = {1};
  filter.output_descriptor_ids = {101, 102, 103};
  filter.bound_expression_ids = {40};
  filter.semantic_variant_id = "filter.where.v1";
  dag.nodes.push_back(std::move(filter));
  for (std::size_t ordinal = 0; ordinal < 3; ++ordinal) {
    dag.outputs.push_back(
        {static_cast<std::uint32_t>(20 + ordinal), 3,
         static_cast<std::uint32_t>(1 + ordinal),
         ordinal == 0 ? "row_uuid" : ordinal == 1 ? "key" : "value",
         static_cast<std::uint32_t>(101 + ordinal), true,
         static_cast<std::uint32_t>(ordinal)});
  }
  api::RelationalDagNode project;
  project.node_id = 3;
  project.node_kind = api::RelationalDagNodeKind::kProject;
  project.input_node_ids = {2};
  project.output_descriptor_ids = {101, 102, 103};
  project.bound_expression_ids = {1, 2, 3};
  project.semantic_variant_id = "project.select-list.v1";
  dag.nodes.push_back(std::move(project));
  api::RelationalDagNode cte;
  cte.node_id = 4;
  cte.node_kind = api::RelationalDagNodeKind::kCte;
  cte.input_node_ids = {3};
  cte.output_descriptor_ids = {101, 102, 103};
  cte.shareable = true;
  cte.semantic_variant_id = "cte.bound.v1";
  dag.nodes.push_back(std::move(cte));
  api::RelationalTypeDescriptor limit_descriptor;
  limit_descriptor.descriptor_id = 105;
  limit_descriptor.descriptor_uuid = NewUuidText(platform::UuidKind::object);
  limit_descriptor.type_uuid = CoreTypeUuid("int64");
  limit_descriptor.nullability = api::RelationalNullability::kNonNull;
  dag.descriptors.push_back(std::move(limit_descriptor));
  api::RelationalExpressionRecord limit;
  limit.expression_id = 41;
  limit.expression_kind = api::RelationalExpressionKind::kLiteral;
  limit.result_descriptor_id = 105;
  limit.literal_kind = api::RelationalLiteralKind::kNumeric;
  limit.literal_or_parameter_ref = "4";
  dag.expressions.push_back(std::move(limit));
  api::RelationalDagNode limit_node;
  limit_node.node_id = 5;
  limit_node.node_kind = api::RelationalDagNodeKind::kLimit;
  limit_node.input_node_ids = {4};
  limit_node.output_descriptor_ids = {101, 102, 103};
  limit_node.bound_expression_ids = {41};
  limit_node.semantic_variant_id = "limit.bound-count.v1";
  dag.nodes.push_back(std::move(limit_node));
  return dag;
}

api::TypedRelationalDag KeyValueCountDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& descriptor) {
  auto dag = KeyValueDag(
      context, descriptor,
      api::EngineBoundKeyValueReadOperationV1::kPrefixRange, {""});
  dag.root_node_id = 2;
  api::RelationalTypeDescriptor count_descriptor;
  count_descriptor.descriptor_id = 105;
  count_descriptor.descriptor_uuid = NewUuidText(platform::UuidKind::object);
  count_descriptor.type_uuid = CoreTypeUuid("int64");
  count_descriptor.nullability = api::RelationalNullability::kNonNull;
  dag.descriptors.push_back(std::move(count_descriptor));
  const auto count = exec::LookupCanonicalAggregateByFunctionV1(
      exec::CanonicalAggregateFunction::count);
  api::RelationalExpressionRecord aggregate;
  aggregate.expression_id = 40;
  aggregate.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  aggregate.result_descriptor_id = 105;
  if (count != nullptr) aggregate.function_uuid = count->function_uuid;
  dag.expressions.push_back(std::move(aggregate));
  dag.outputs.push_back({20, 2, 40, "key_count", 105, true, 0});
  api::RelationalDagNode aggregate_node;
  aggregate_node.node_id = 2;
  aggregate_node.node_kind = api::RelationalDagNodeKind::kAggregate;
  aggregate_node.input_node_ids = {1};
  aggregate_node.output_descriptor_ids = {105};
  aggregate_node.bound_expression_ids = {40};
  aggregate_node.semantic_variant_id = "aggregate.global-count-star.v1";
  dag.nodes.push_back(std::move(aggregate_node));
  return dag;
}

api::TypedRelationalDag KeyValueRecursiveDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& descriptor) {
  auto dag = KeyValueCountDag(context, descriptor);
  dag.root_node_id = 4;
  api::RelationalExpressionRecord bound;
  bound.expression_id = 41;
  bound.expression_kind = api::RelationalExpressionKind::kLiteral;
  bound.result_descriptor_id = 105;
  bound.literal_kind = api::RelationalLiteralKind::kNumeric;
  bound.literal_or_parameter_ref = "8";
  dag.expressions.push_back(std::move(bound));
  api::RelationalDagNode term;
  term.node_id = 3;
  term.node_kind = api::RelationalDagNodeKind::kCte;
  term.output_descriptor_ids = {105};
  term.semantic_variant_id = "cte.recursive-term-int64-increment.v1";
  dag.nodes.push_back(std::move(term));
  api::RelationalDagNode recursive;
  recursive.node_id = 4;
  recursive.node_kind = api::RelationalDagNodeKind::kRecursiveCte;
  recursive.input_node_ids = {2, 3};
  recursive.output_descriptor_ids = {105};
  recursive.bound_expression_ids = {41};
  recursive.semantic_variant_id =
      "cte.recursive-union-all-int64-increment.v1";
  dag.nodes.push_back(std::move(recursive));
  return dag;
}

api::TypedRelationalDag KeyValueSetDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& descriptor) {
  auto dag = KeyValueDag(
      context, descriptor,
      api::EngineBoundKeyValueReadOperationV1::kPrefixRange, {""});
  dag.root_node_id = 4;
  dag.outputs.push_back({20, 2, 1, "row_uuid", 101, true, 0});
  api::RelationalDagNode project;
  project.node_id = 2;
  project.node_kind = api::RelationalDagNodeKind::kProject;
  project.input_node_ids = {1};
  project.output_descriptor_ids = {101};
  project.bound_expression_ids = {1};
  project.semantic_variant_id = "project.select-list.v1";
  dag.nodes.push_back(std::move(project));
  api::RelationalExpressionRecord literal;
  literal.expression_id = 40;
  literal.expression_kind = api::RelationalExpressionKind::kLiteral;
  literal.result_descriptor_id = 101;
  literal.literal_kind = api::RelationalLiteralKind::kUuid;
  literal.literal_or_parameter_ref =
      "30000000-0000-4000-8000-000000000099";
  dag.expressions.push_back(std::move(literal));
  dag.values_rows.push_back({1, {40}});
  dag.outputs.push_back({21, 3, 40, "row_uuid", 101, true, 0});
  api::RelationalDagNode values;
  values.node_id = 3;
  values.node_kind = api::RelationalDagNodeKind::kValues;
  values.output_descriptor_ids = {101};
  values.values_row_ids = {1};
  values.semantic_variant_id = "values.literal-table.v1";
  dag.nodes.push_back(std::move(values));
  api::RelationalDagNode set;
  set.node_id = 4;
  set.node_kind = api::RelationalDagNodeKind::kSetOperation;
  set.input_node_ids = {2, 3};
  set.output_descriptor_ids = {101};
  set.semantic_variant_id = "set-operation.union-all.v1";
  dag.nodes.push_back(std::move(set));
  return dag;
}

api::TypedRelationalDag KeyValueMixedJoinDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& descriptor,
    const api::MgaRelationStorageDescriptor& heap_descriptor,
    std::string semantic_variant_id) {
  auto dag = KeyValueDag(
      context, descriptor,
      api::EngineBoundKeyValueReadOperationV1::kPrefixRange, {""});
  dag.root_node_id = 3;
  for (std::size_t ordinal = 0; ordinal < 2; ++ordinal) {
    const auto descriptor_id = static_cast<std::uint32_t>(201 + ordinal);
    const auto expression_id = static_cast<std::uint32_t>(50 + ordinal);
    const auto& column = heap_descriptor.columns[ordinal];
    api::RelationalTypeDescriptor type;
    type.descriptor_id = descriptor_id;
    type.descriptor_uuid = column.value_descriptor.descriptor_uuid.canonical;
    type.type_uuid =
        DescriptorField(column.value_descriptor.encoded_descriptor,
                        "type_uuid");
    type.nullability = column.nullable
                           ? api::RelationalNullability::kNullable
                           : api::RelationalNullability::kNonNull;
    dag.descriptors.push_back(std::move(type));
    api::RelationalExpressionRecord expression;
    expression.expression_id = expression_id;
    expression.expression_kind = api::RelationalExpressionKind::kIdentifier;
    expression.result_descriptor_id = descriptor_id;
    expression.bound_name_uuid = column.column_uuid.canonical;
    dag.expressions.push_back(std::move(expression));
    dag.outputs.push_back(
        {expression_id, 2, expression_id, column.canonical_name_key,
         descriptor_id, true, static_cast<std::uint32_t>(ordinal)});
  }
  api::RelationalDagNode heap;
  heap.node_id = 2;
  heap.node_kind = api::RelationalDagNodeKind::kScan;
  heap.output_descriptor_ids = {201, 202};
  heap.bound_expression_ids = {50, 51};
  heap.required_object_uuids = {heap_descriptor.relation_uuid.canonical};
  heap.semantic_variant_id = "relation.source.v1";
  dag.nodes.push_back(std::move(heap));

  const bool cross = semantic_variant_id == "join.cross.v1";
  const bool left_only = semantic_variant_id == "join.left-semi.v1" ||
                         semantic_variant_id == "join.left-anti.v1";
  if (!cross) {
    // Both sides participate in the heterogeneous residual: the model
    // source's engine row UUID is compared with an ordinary heap UUID column.
    api::RelationalExpressionRecord equality;
    equality.expression_id = 60;
    equality.expression_kind = api::RelationalExpressionKind::kBinary;
    equality.child_expression_ids = {1, 50};
    equality.result_descriptor_id = 104;
    equality.operator_name = "=";
    dag.expressions.push_back(std::move(equality));
  }
  // A bare JOIN root publishes the exact concatenated input schema.  Output
  // aliases belong to an explicit PROJECT above the join, so keep these root
  // names identical to the physical input column names.
  const std::array<std::string_view, 5> names{
      "row_uuid", "key", "value", "join_uuid", "payload"};
  const std::array<std::uint32_t, 5> expressions{1, 2, 3, 50, 51};
  const std::array<std::uint32_t, 5> descriptors{101, 102, 103, 201, 202};
  const auto width = left_only ? 3U : 5U;
  for (std::size_t ordinal = 0; ordinal < width; ++ordinal) {
    dag.outputs.push_back(
        {static_cast<std::uint32_t>(70 + ordinal), 3,
         expressions[ordinal], std::string(names[ordinal]),
         descriptors[ordinal], true, static_cast<std::uint32_t>(ordinal)});
  }
  api::RelationalDagNode join;
  join.node_id = 3;
  join.node_kind = api::RelationalDagNodeKind::kJoin;
  join.input_node_ids = {1, 2};
  join.output_descriptor_ids.assign(descriptors.begin(),
                                    descriptors.begin() + width);
  if (!cross) join.bound_expression_ids = {60};
  join.semantic_variant_id = std::move(semantic_variant_id);
  dag.nodes.push_back(std::move(join));
  return dag;
}

std::string ApiRowField(const api::EngineApiResult& result,
                        const std::size_t row_ordinal,
                        const std::string_view field_name) {
  if (row_ordinal >= result.result_shape.rows.size()) return {};
  const auto& row = result.result_shape.rows[row_ordinal];
  const auto field = std::ranges::find_if(row.fields, [&](const auto& item) {
    return item.first == field_name;
  });
  return field == row.fields.end() ? std::string{}
                                   : field->second.encoded_value;
}

bool ProductionCanonicalRoute(
    const Fixture& fixture,
    const api::EngineRequestContext& context) {
  const auto& descriptor =
      fixture.descriptors.at(std::string(kBaseObjectUuid));
  const auto& heap_descriptor =
      fixture.descriptors.at(std::string(kJoinObjectUuid));
  const auto exact = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, KeyValueDag(
                    context, descriptor,
                    api::EngineBoundKeyValueReadOperationV1::kGet,
                    {"alpha"})});
  const auto multi = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, KeyValueDag(
                    context, descriptor,
                    api::EngineBoundKeyValueReadOperationV1::kMultiGet,
                    {"gamma", "missing", "alpha", "gamma", "beta",
                     "alpine", "alpha"})});
  const auto prefix = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, KeyValueDag(
                    context, descriptor,
                    api::EngineBoundKeyValueReadOperationV1::kPrefixRange,
                    {""})});
  const auto unary = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, KeyValueUnaryCompositionDag(context, descriptor)});
  const auto cte_limit = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, KeyValueCteLimitDag(context, descriptor)});
  const auto counted = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, KeyValueCountDag(context, descriptor)});
  const auto recursive_dag = KeyValueRecursiveDag(context, descriptor);
  const auto recursive = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, recursive_dag});
  const auto recursive_replay = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, recursive_dag});
  const auto set_dag = KeyValueSetDag(context, descriptor);
  const auto set_union = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, set_dag});
  const auto set_replay = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, set_dag});
  const auto cross_join = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, KeyValueMixedJoinDag(context, descriptor, heap_descriptor,
                                     "join.cross.v1")});
  const auto inner_join = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, KeyValueMixedJoinDag(context, descriptor, heap_descriptor,
                                     "join.inner.v1")});
  const auto left_join = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context,
       KeyValueMixedJoinDag(context, descriptor, heap_descriptor,
                            "join.left-outer.v1")});
  const auto right_join = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context,
       KeyValueMixedJoinDag(context, descriptor, heap_descriptor,
                            "join.right-outer.v1")});
  const auto full_join = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context,
       KeyValueMixedJoinDag(context, descriptor, heap_descriptor,
                            "join.full-outer.v1")});
  const auto semi_join = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context,
       KeyValueMixedJoinDag(context, descriptor, heap_descriptor,
                            "join.left-semi.v1")});
  const auto anti_join = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context,
       KeyValueMixedJoinDag(context, descriptor, heap_descriptor,
                            "join.left-anti.v1")});
  const auto complete = [](const auto& result, const std::size_t rows) {
    return result.profile_matched && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.optimizer_admission_stage_count == 8 &&
           result.physical_node_count == 1 &&
           result.canonical_result_column_count == 3 &&
           result.canonical_result_row_count == rows;
  };
  if (!exact.api_result.ok || !multi.api_result.ok || !prefix.api_result.ok ||
      !unary.api_result.ok || !cte_limit.api_result.ok ||
      !counted.api_result.ok ||
      !recursive.api_result.ok || !set_union.api_result.ok ||
      !cross_join.api_result.ok || !inner_join.api_result.ok ||
      !left_join.api_result.ok || !right_join.api_result.ok ||
      !full_join.api_result.ok || !semi_join.api_result.ok ||
      !anti_join.api_result.ok) {
    for (const auto* result : {&exact, &multi, &prefix, &unary, &cte_limit,
                               &counted, &recursive, &set_union, &cross_join,
                               &inner_join, &left_join, &right_join,
                               &full_join, &semi_join, &anti_join}) {
      for (const auto& diagnostic : result->api_result.diagnostics) {
        std::cerr << "QOW-CES05-KEY-VALUE route: " << diagnostic.code << ' '
                  << diagnostic.detail << '\n';
      }
    }
  }
  const bool composition_complete =
      unary.profile_matched && unary.optimizer_admitted &&
      unary.optimizer_selected && unary.physical_dag_published &&
      unary.physical_dag_executed && unary.runtime_actuals_attached &&
      unary.canonical_result_published && unary.api_result.ok &&
      unary.physical_node_count == 5 &&
      unary.canonical_result_column_count == 4 &&
      unary.canonical_result_row_count == 6 &&
      ApiRowField(unary.api_result, 0, "key") == "alpha" &&
      ApiRowField(unary.api_result, 1, "key") == "alpine" &&
      ApiRowField(unary.api_result, 2, "key") == "gamma" &&
      ApiRowField(unary.api_result, 3, "key") == "zeta" &&
      ApiRowField(unary.api_result, 0, "row_number") == "1" &&
      ApiRowField(unary.api_result, 3, "row_number") == "4" &&
      ApiRowField(unary.api_result, 5, "row_number") == "6";
  const bool cte_limit_complete =
      cte_limit.profile_matched && cte_limit.optimizer_admitted &&
      cte_limit.optimizer_selected && cte_limit.physical_dag_published &&
      cte_limit.physical_dag_executed &&
      cte_limit.runtime_actuals_attached &&
      cte_limit.canonical_result_published && cte_limit.api_result.ok &&
      cte_limit.physical_node_count == 5 &&
      cte_limit.canonical_result_column_count == 3 &&
      cte_limit.canonical_result_row_count == 4;
  const bool aggregate_complete =
      counted.profile_matched && counted.optimizer_admitted &&
      counted.optimizer_selected && counted.physical_dag_published &&
      counted.physical_dag_executed && counted.runtime_actuals_attached &&
      counted.canonical_result_published && counted.api_result.ok &&
      counted.physical_node_count == 2 &&
      counted.canonical_result_column_count == 1 &&
      counted.canonical_result_row_count == 1 &&
      ApiRowField(counted.api_result, 0, "key_count") == "6";
  const bool recursive_complete =
      recursive.profile_matched && recursive.optimizer_admitted &&
      recursive.optimizer_selected && recursive.physical_dag_published &&
      recursive.physical_dag_executed && recursive.runtime_actuals_attached &&
      recursive.canonical_result_published && recursive.api_result.ok &&
      recursive.physical_node_count == 4 &&
      recursive.canonical_result_column_count == 1 &&
      recursive.canonical_result_row_count == 3 &&
      ApiRowField(recursive.api_result, 0, "key_count") == "6" &&
      ApiRowField(recursive.api_result, 1, "key_count") == "7" &&
      ApiRowField(recursive.api_result, 2, "key_count") == "8" &&
      recursive_replay.api_result.ok &&
      recursive_replay.canonical_result_bytes ==
          recursive.canonical_result_bytes;
  const bool set_complete =
      set_union.profile_matched && set_union.optimizer_admitted &&
      set_union.optimizer_selected && set_union.physical_dag_published &&
      set_union.physical_dag_executed && set_union.runtime_actuals_attached &&
      set_union.canonical_result_published && set_union.api_result.ok &&
      set_union.physical_node_count == 4 &&
      set_union.canonical_result_column_count == 1 &&
      set_union.canonical_result_row_count == 7 &&
      ApiRowField(set_union.api_result, 6, "row_uuid") ==
          "30000000-0000-4000-8000-000000000099" &&
      set_replay.api_result.ok &&
      set_replay.canonical_result_bytes == set_union.canonical_result_bytes;
  const auto join_complete = [](const auto& execution,
                                const std::size_t columns,
                                const std::size_t rows) {
    return execution.profile_matched && execution.optimizer_admitted &&
           execution.optimizer_selected && execution.physical_dag_published &&
           execution.physical_dag_executed &&
           execution.runtime_actuals_attached &&
           execution.canonical_result_published && execution.api_result.ok &&
           execution.optimizer_admission_stage_count == 8 &&
           execution.physical_node_count == 3 &&
           execution.canonical_result_column_count == columns &&
           execution.canonical_result_row_count == rows;
  };
  const bool mixed_joins_complete =
      join_complete(cross_join, 5, 12) &&
      join_complete(inner_join, 5, 1) &&
      join_complete(left_join, 5, 6) &&
      join_complete(right_join, 5, 2) &&
      join_complete(full_join, 5, 7) &&
      join_complete(semi_join, 3, 1) &&
      join_complete(anti_join, 3, 5);
  if (!(complete(exact, 1) && complete(multi, 3) && complete(prefix, 6) &&
        composition_complete && cte_limit_complete && aggregate_complete &&
        recursive_complete && set_complete && mixed_joins_complete)) {
    const auto report = [](const std::string_view name, const auto& execution) {
      std::cerr << "QOW-CES05-KEY-VALUE summary " << name
                << " profile=" << execution.profile_matched
                << " admitted=" << execution.optimizer_admitted
                << " selected=" << execution.optimizer_selected
                << " published=" << execution.physical_dag_published
                << " executed=" << execution.physical_dag_executed
                << " actuals=" << execution.runtime_actuals_attached
                << " result=" << execution.canonical_result_published
                << " ok=" << execution.api_result.ok
                << " stages=" << execution.optimizer_admission_stage_count
                << " nodes=" << execution.physical_node_count
                << " columns=" << execution.canonical_result_column_count
                << " rows=" << execution.canonical_result_row_count << '\n';
    };
    report("exact", exact);
    report("multi", multi);
    report("prefix", prefix);
    report("unary", unary);
    report("cte_limit", cte_limit);
    report("counted", counted);
    report("recursive", recursive);
    report("set_union", set_union);
    report("cross_join", cross_join);
    report("inner_join", inner_join);
    report("left_join", left_join);
    report("right_join", right_join);
    report("full_join", full_join);
    report("semi_join", semi_join);
    report("anti_join", anti_join);
    std::cerr << "QOW-CES05-KEY-VALUE values unary="
              << ApiRowField(unary.api_result, 0, "key") << ','
              << ApiRowField(unary.api_result, 1, "key") << ','
              << ApiRowField(unary.api_result, 2, "key") << ','
              << ApiRowField(unary.api_result, 3, "key") << " row_numbers="
              << ApiRowField(unary.api_result, 0, "row_number") << ','
              << ApiRowField(unary.api_result, 3, "row_number") << ','
              << ApiRowField(unary.api_result, 5, "row_number")
              << " count="
              << ApiRowField(counted.api_result, 0, "key_count")
              << " recursive="
              << ApiRowField(recursive.api_result, 0, "key_count") << ','
              << ApiRowField(recursive.api_result, 1, "key_count") << ','
              << ApiRowField(recursive.api_result, 2, "key_count")
              << " set_tail="
              << ApiRowField(set_union.api_result, 6, "row_uuid") << '\n';
  }
  return Require(
      complete(exact, 1) && complete(multi, 3) && complete(prefix, 6) &&
          composition_complete && cte_limit_complete && aggregate_complete &&
          recursive_complete && set_complete && mixed_joins_complete,
      "production key/value source/composition route did not complete");
}

bool CaseInventory(const std::set<std::string>& completed) {
  bool passed = true;
  for (std::uint32_t ordinal = 1; ordinal <= 32; ++ordinal) {
    char id[6];
    std::snprintf(id, sizeof(id), "KV-%02u", ordinal);
    passed &= Require(completed.contains(id),
                      std::string("immutable case was not executed: ") + id);
  }
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  Fixture fixture;
  api::EngineRequestContext active_other;
  if (!MakeFixture(&fixture) || !SeedFixtures(&fixture, &active_other)) return 1;
  api::EngineRequestContext context;
  if (!PublishReaderContext(fixture, &context)) {
    Rollback(active_other);
    return 1;
  }
  passed &= VerifySignedTransactionInventory(fixture, context);
  passed &= CoordinatorSelectionProof(
      context, fixture.descriptors.at(std::string(kBaseObjectUuid)));
  std::set<std::string> completed;
  passed &= ProviderMatrix(fixture, context, &completed);
  passed &= FrontdoorRefusalMatrix(&completed);
#if defined(SB_CES05_KEY_VALUE_PRODUCTION_QUERY_ROUTE)
  passed &= ProductionCanonicalRoute(fixture, context);
#endif
  passed &= CaseInventory(completed);
  passed &= Commit(context);
  passed &= Rollback(active_other);
  if (!passed) return 1;
#if defined(SB_CES05_KEY_VALUE_PRODUCTION_QUERY_ROUTE)
  std::cout << "QOW CES-05 RCP-075 key/value production query route: PASS\n";
#else
  std::cout << "QOW CES-05 RCP-075 key/value execution: PASS\n";
#endif
  return 0;
}
