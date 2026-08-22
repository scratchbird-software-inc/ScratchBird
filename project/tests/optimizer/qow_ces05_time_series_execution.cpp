// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "canonical_query_execute.hpp"
#include "cst/cst.hpp"
#include "crud_support/crud_store.hpp"
#include "database_lifecycle.hpp"
#include "datatype_catalog_manifest.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "hash_digest.hpp"
#include "local_transaction_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "model_family_coordinator.hpp"
#include "model_family_executor.hpp"
#include "nosql/nosql_provider_generation_store.hpp"
#include "nosql/time_series_api.hpp"
#include "logical_plan.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cfenv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace dt = scratchbird::core::datatypes;
namespace exec = scratchbird::engine::executor;
namespace hash = scratchbird::core::hash;
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;
namespace platform = scratchbird::core::platform;
namespace sbsql = scratchbird::parser::sbsql;
namespace sblr = scratchbird::engine::sblr;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kBaseObjectUuid =
    "40000000-0000-4000-8000-000000007600";
constexpr std::string_view kMetricOneUuid =
    "40000000-0000-4000-8000-000000007601";
constexpr std::string_view kMetricTwoUuid =
    "40000000-0000-4000-8000-000000007602";
constexpr std::string_view kPreEpochObjectUuid =
    "40000000-0000-4000-8000-000000007610";
constexpr std::string_view kDuplicateTagObjectUuid =
    "40000000-0000-4000-8000-000000007611";
constexpr std::string_view kNonfiniteObjectUuid =
    "40000000-0000-4000-8000-000000007612";
constexpr std::string_view kInvisibleInvalidObjectUuid =
    "40000000-0000-4000-8000-000000007613";
constexpr std::string_view kJoinObjectUuid =
    "40000000-0000-4000-8000-000000007614";
constexpr std::string_view kUnicodeOrderObjectUuid =
    "40000000-0000-4000-8000-000000007615";
constexpr std::string_view kLoneSurrogateObjectUuid =
    "40000000-0000-4000-8000-000000007616";
constexpr std::string_view kReversedSurrogateObjectUuid =
    "40000000-0000-4000-8000-000000007617";
constexpr std::string_view kOutputBoundObjectUuid =
    "40000000-0000-4000-8000-000000007618";
constexpr std::string_view kAsofRightObjectUuid =
    "40000000-0000-4000-8000-000000007619";
constexpr std::string_view kPreferredProviderUuid =
    "40000000-0000-4000-8000-0000000076f0";
constexpr std::string_view kAmbiguousProviderUuid =
    "40000000-0000-4000-8000-0000000076f1";
constexpr std::string_view kRangeStart =
    "2026-08-10T12:00:00.000000000Z";
constexpr std::string_view kRangeEnd =
    "2026-08-10T12:02:00.000000000Z";
constexpr std::int64_t kMinuteNs = 60'000'000'000LL;

struct SignedPoint {
  const char* row_uuid;
  const char* metric_uuid;
  const char* timestamp;
  const char* tags;
  const char* value;
};

constexpr std::array<SignedPoint, 9> kBaseRows{{
    {"40000000-0000-4000-8000-000000000001", kMetricOneUuid.data(),
     "2026-08-10T12:00:00.000000000Z", "{\"zone\":\"east\",\"host\":\"a\"}", "1"},
    {"40000000-0000-4000-8000-000000000002", kMetricOneUuid.data(),
     "2026-08-10T12:00:30.000000000Z", "{\"host\":\"a\",\"zone\":\"east\"}", "3"},
    {"40000000-0000-4000-8000-000000000004", kMetricOneUuid.data(),
     "2026-08-10T12:01:00.000000000Z", "{\"host\":\"a\",\"zone\":\"east\"}", "5"},
    {"40000000-0000-4000-8000-000000000005", kMetricOneUuid.data(),
     "2026-08-10T12:01:00.000000000Z", "{\"host\":\"a\",\"zone\":\"east\"}", "7"},
    {"40000000-0000-4000-8000-000000000006", kMetricOneUuid.data(),
     "2026-08-10T12:01:30.000000000Z", "{\"host\":\"a\",\"zone\":\"west\"}", "9"},
    {"40000000-0000-4000-8000-000000000007", kMetricTwoUuid.data(),
     "2026-08-10T12:00:15.000000000Z", "{\"host\":\"a\",\"zone\":\"east\"}", "2"},
    {"40000000-0000-4000-8000-000000000003", kMetricOneUuid.data(),
     "2026-08-10T12:00:45.000000000Z", "{\"host\":\"a\",\"zone\":\"east\"}", "4"},
    {"40000000-0000-4000-8000-000000000008", kMetricOneUuid.data(),
     "2026-08-10T12:02:00.000000000Z", "{\"host\":\"a\",\"zone\":\"east\"}", "11"},
    {"40000000-0000-4000-8000-000000000009", kMetricOneUuid.data(),
     "2026-08-10T11:59:59.999999999Z", "{\"host\":\"a\",\"zone\":\"east\"}", "13"},
}};

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-CES05-TIME-SERIES: " << detail << '\n';
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

api::EngineNoSqlProviderGenerationMetadata TimeSeriesProviderGeneration(
    const api::EngineRequestContext& context,
    const std::string_view provider_uuid,
    const std::string_view object_uuid,
    const std::uint64_t generation_id) {
  api::EngineNoSqlProviderGenerationMetadata metadata;
  metadata.family = api::EngineNoSqlProviderFamily::kTimeSeries;
  metadata.provider_id = provider_uuid;
  metadata.database_identity =
      api::EngineNoSqlProviderDatabaseIdentity(context);
  metadata.database_uuid = context.database_uuid.canonical;
  metadata.collection_uuid = object_uuid;
  metadata.generation_uuid = NewUuidText(platform::UuidKind::object);
  metadata.generation_id = generation_id;
  metadata.descriptor_epoch =
      std::max<std::uint64_t>(1, context.resource_epoch);
  metadata.security_epoch =
      std::max<std::uint64_t>(1, context.security_epoch);
  metadata.redaction_epoch =
      std::max<std::uint64_t>(1, context.security_epoch);
  metadata.catalog_epoch =
      std::max<std::uint64_t>(1, context.catalog_generation_id);
  metadata.publish_state = "published";
  metadata.validation_state = "validated";
  metadata.backup_metadata_ref =
      "backup.time_series_provider_generation:" + metadata.generation_uuid;
  metadata.restore_metadata_ref =
      "restore.time_series_provider_generation:" + metadata.generation_uuid;
  metadata.repair_metadata_ref =
      "repair.time_series_provider_generation:" + metadata.generation_uuid;
  metadata.support_bundle_evidence_id =
      "support.time_series_provider_generation:" + metadata.generation_uuid;
  return metadata;
}

void AttachExactTimeSeriesRollupCarrier(
    api::EngineNoSqlProviderGenerationMetadata* metadata,
    const api::EngineRequestContext& context,
    const std::uint64_t rollup_generation,
    const std::uint64_t visible_late_arrival_generation) {
  metadata->time_series_rollup_candidate_present = true;
  metadata->time_series_rollup_generation = rollup_generation;
  metadata->time_series_visible_late_arrival_generation =
      visible_late_arrival_generation;
  metadata->time_series_rollup_interval_ns = kMinuteNs;
  metadata->time_series_rollup_exactness_attestation_state =
      "TIME_SERIES_ROLLUP_SECTION_8_EXACT_V1";
  metadata->time_series_rollup_statement_snapshot_uuid =
      context.statement_snapshot_uuid.canonical;
  metadata->time_series_rollup_statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  metadata->time_series_rollup_owning_transaction_uuid =
      context.transaction_uuid.canonical;
  metadata->time_series_rollup_local_transaction_id =
      context.local_transaction_id;
  metadata
      ->time_series_rollup_snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  metadata->time_series_rollup_security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  metadata->time_series_rollup_catalog_epoch_uuid =
      context.catalog_epoch_uuid.canonical;
  metadata->time_series_rollup_exact_residual_recheck_required = true;
  metadata->time_series_rollup_base_row_mga_recheck_required = true;
  metadata->time_series_rollup_security_recheck_required = true;
  metadata->time_series_rollup_capability_uuid =
      api::DeriveTimeSeriesRollupCapabilityUuidV1(*metadata);
}

bool ExactTimeSeriesRollupCapabilityKnownAnswer() {
  api::EngineNoSqlProviderGenerationMetadata metadata;
  metadata.family = api::EngineNoSqlProviderFamily::kTimeSeries;
  metadata.provider_id = "11111111-1111-4111-8111-111111111111";
  metadata.database_identity = "/fixed/rcp076/database.sbdb";
  metadata.database_uuid = "22222222-2222-4222-8222-222222222222";
  metadata.collection_uuid = "33333333-3333-4333-8333-333333333333";
  metadata.generation_uuid = "44444444-4444-4444-8444-444444444444";
  metadata.generation_id = 76;
  metadata.descriptor_epoch = 77;
  metadata.security_epoch = 78;
  metadata.redaction_epoch = 79;
  metadata.catalog_epoch = 80;
  metadata.publish_state = "published";
  metadata.validation_state = "validated";
  metadata.time_series_rollup_candidate_present = true;
  metadata.time_series_rollup_generation = 7;
  metadata.time_series_visible_late_arrival_generation = 8;
  metadata.time_series_rollup_interval_ns = 60'000'000'000LL;
  metadata.time_series_rollup_exactness_attestation_state =
      "TIME_SERIES_ROLLUP_SECTION_8_EXACT_V1";
  metadata.time_series_rollup_statement_snapshot_uuid =
      "55555555-5555-4555-8555-555555555555";
  metadata.time_series_rollup_statement_metadata_snapshot_uuid =
      "66666666-6666-4666-8666-666666666666";
  metadata.time_series_rollup_owning_transaction_uuid =
      "77777777-7777-4777-8777-777777777777";
  metadata.time_series_rollup_local_transaction_id = 850;
  metadata.time_series_rollup_snapshot_visible_through_local_transaction_id =
      900;
  metadata.time_series_rollup_security_context_uuid =
      "88888888-8888-4888-8888-888888888888";
  metadata.time_series_rollup_catalog_epoch_uuid =
      "99999999-9999-4999-8999-999999999999";
  metadata.time_series_rollup_exact_residual_recheck_required = true;
  metadata.time_series_rollup_base_row_mga_recheck_required = true;
  metadata.time_series_rollup_security_recheck_required = true;

  const auto derived =
      api::DeriveTimeSeriesRollupCapabilityUuidV1(metadata);
  static constexpr std::string_view kExpected =
      "0c4dfc10-09bc-8874-9744-5fe33947709b";
  if (derived != kExpected || derived.size() != 36 || derived[14] != '8' ||
      (derived[19] != '8' && derived[19] != '9' && derived[19] != 'a' &&
       derived[19] != 'b')) {
    return false;
  }
  metadata.time_series_rollup_capability_uuid = derived;
  return api::ValidateTimeSeriesRollupCapabilityBindingV1(metadata);
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
  context.catalog_generation_id = 76;
  context.security_epoch = 77;
  context.resource_epoch = 78;
  context.name_resolution_epoch = 79;
  return context;
}

bool Begin(const Fixture& fixture, std::string request_id,
           api::EngineRequestContext* context) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto result = api::EngineBeginTransaction(request);
  if (!result.ok) return Require(false, "transaction begin failed");
  *context = request.context;
  context->transaction_uuid = result.transaction_uuid;
  context->local_transaction_id = result.local_transaction_id;
  context->snapshot_visible_through_local_transaction_id =
      result.snapshot_visible_through_local_transaction_id;
  context->transaction_isolation_level = result.isolation_level;
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

bool SetNextTransactionId(const Fixture& fixture, const std::uint64_t next_id) {
  const auto loaded =
      db::LoadLocalTransactionInventoryFromDatabase(fixture.database_path.string());
  if (!loaded.ok()) return Require(false, "transaction inventory load failed");
  auto inventory = loaded.inventory;
  inventory.next_local_transaction_id = next_id;
  return Require(db::PersistLocalTransactionInventoryToDatabase(
                     fixture.database_path.string(), inventory)
                     .ok(),
                 "transaction inventory exact-ID seed failed");
}

api::EngineLocalizedName Name(std::string name) {
  return {"en", "primary", "", std::move(name), true};
}

api::EngineColumnDefinition Column(const std::uint32_t ordinal,
                                   std::string name,
                                   const std::string_view type) {
  api::EngineColumnDefinition column;
  column.requested_column_uuid.canonical = NewUuidText(platform::UuidKind::object);
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name =
      type == "timestamp_tz" ? "timestamp" : std::string(type);
  column.descriptor.encoded_descriptor = "canonical=" + std::string(type);
  column.ordinal = ordinal;
  column.nullable = false;
  return column;
}

void AddAuthorization(api::EngineRequestContext* context,
                      const std::string& object_uuid) {
  if (!context->authorization_context.present) {
    auto& auth = context->authorization_context;
    auth.present = true;
    auth.authority_uuid.canonical = NewUuidText(platform::UuidKind::object);
    auth.principal_uuid = context->principal_uuid;
    auth.security_epoch = context->security_epoch;
    auth.policy_epoch = 80;
    auth.catalog_generation_id = context->catalog_generation_id;
    auth.effective_subjects.push_back({context->principal_uuid, "principal"});
  }
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical = NewUuidText(platform::UuidKind::object);
  grant.subject_uuid = context->principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical = object_uuid;
  grant.right = "SELECT";
  grant.security_epoch = context->security_epoch;
  context->authorization_context.grants.push_back(std::move(grant));
}

bool MakeFixture(Fixture* fixture) {
  fixture->directory = std::filesystem::temp_directory_path() /
                       ("scratchbird_rcp076_time_series_" +
                        std::to_string(Seed()));
  std::error_code error;
  if (!std::filesystem::create_directories(fixture->directory, error) || error) {
    return Require(false, "fixture directory creation failed");
  }
  fixture->database_path = fixture->directory / "time_series.sbdb";
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
  if (!Begin(*fixture, "rcp076-metadata", &context)) return false;
  api::EngineCreateSchemaRequest schema;
  schema.context = context;
  schema.target_object.uuid.canonical = fixture->schema_uuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(Name("time_series_schema"));
  if (!api::EngineCreateSchema(schema).ok) {
    Rollback(context);
    return Require(false, "fixture schema creation failed");
  }
  const std::array<std::pair<std::string_view, std::string_view>, 11> tables{{
      {kBaseObjectUuid, "base"},
      {kPreEpochObjectUuid, "pre_epoch"},
      {kDuplicateTagObjectUuid, "duplicate_tag"},
      {kNonfiniteObjectUuid, "nonfinite"},
      {kInvisibleInvalidObjectUuid, "invisible_invalid"},
      {kJoinObjectUuid, "relational_uuid_join"},
      {kUnicodeOrderObjectUuid, "unicode_order"},
      {kLoneSurrogateObjectUuid, "lone_surrogate"},
      {kReversedSurrogateObjectUuid, "reversed_surrogate"},
      {kOutputBoundObjectUuid, "output_bound"},
      {kAsofRightObjectUuid, "relational_asof_right"},
  }};
  for (const auto& [object_uuid, name] : tables) {
    api::EngineCreateTableRequest table;
    table.context = context;
    table.context.current_schema_uuid.canonical.clear();
    table.target_schema.uuid.canonical = fixture->schema_uuid;
    table.target_schema.object_kind = "schema";
    table.requested_table_uuid.canonical = std::string(object_uuid);
    table.table_names.push_back(Name(std::string(name)));
    if (object_uuid == kJoinObjectUuid ||
        object_uuid == kAsofRightObjectUuid) {
      table.table_columns = {Column(0, "join_uuid", "uuid"),
                             Column(1, "payload", "text"),
                             Column(2, "event_timestamp", "timestamp_tz"),
                             Column(3, "metric_uuid", "uuid"),
                             Column(4, "tags", "text")};
    } else {
      table.table_columns = {
          Column(0, "metric_uuid", "uuid"),
          Column(1, "point_timestamp", "timestamp_tz"),
          Column(2, "tags", "text"),
          Column(3, "value", "real64"),
      };
    }
    const auto created = api::EngineCreateTable(table);
    if (!created.ok) {
      Rollback(context);
      return Require(false, "time-series fixture table creation failed");
    }
    const auto loaded = api::LoadMgaRelationStorageDescriptor(
        context, std::string(object_uuid));
    const auto expected_columns =
        object_uuid == kJoinObjectUuid || object_uuid == kAsofRightObjectUuid
            ? 5U
            : 4U;
    if (!loaded.ok ||
        loaded.descriptor.columns.size() != expected_columns ||
        loaded.descriptor.descriptor_generation == 0) {
      Rollback(context);
      return Require(false, "time-series fixture descriptor load failed");
    }
    fixture->descriptors.emplace(std::string(object_uuid), loaded.descriptor);
  }
  const auto preferred_generation = TimeSeriesProviderGeneration(
      context, kPreferredProviderUuid, kBaseObjectUuid,
      fixture->descriptors.at(std::string(kBaseObjectUuid))
              .descriptor_generation +
          1'000);
  const auto published_preferred =
      api::PublishNoSqlProviderGeneration(context, preferred_generation);
  if (!published_preferred.ok) {
    Rollback(context);
    return Require(false,
                   "time-series preferred provider generation publish failed");
  }
  const auto& join_descriptor =
      fixture->descriptors.at(std::string(kJoinObjectUuid));
  const auto typed = [](const api::EngineDescriptor& descriptor,
                        std::string encoded) {
    api::EngineTypedValue value;
    value.descriptor = descriptor;
    value.encoded_value = std::move(encoded);
    value.setState(api::EngineValueState::value);
    return value;
  };
  api::EngineInsertRowsRequest join_rows;
  join_rows.context = context;
  join_rows.target_table.uuid.canonical = std::string(kJoinObjectUuid);
  join_rows.target_table.object_kind = "table";
  join_rows.require_generated_row_uuid = false;
  join_rows.estimated_row_count = 2;
  for (const auto& [row_uuid, join_uuid, payload, event_timestamp] :
       std::array<std::array<const char*, 4>, 2>{{
           {{"40000000-0000-4000-8000-000000000078",
             "40000000-0000-4000-8000-000000000001", "matched",
             "2026-08-10T12:00:20.000000000Z"}},
           {{"40000000-0000-4000-8000-000000000079",
             "40000000-0000-4000-8000-000000000099", "unmatched",
             "2026-08-10T12:00:46.000000000Z"}},
       }}) {
    api::EngineRowValue row;
    row.requested_row_uuid.canonical = row_uuid;
    row.fields = {
        {"join_uuid",
         typed(join_descriptor.columns[0].value_descriptor, join_uuid)},
        {"payload",
         typed(join_descriptor.columns[1].value_descriptor, payload)},
        {"event_timestamp",
         typed(join_descriptor.columns[2].value_descriptor,
               event_timestamp)},
        {"metric_uuid",
         typed(join_descriptor.columns[3].value_descriptor,
               std::string(kMetricOneUuid))},
        {"tags",
         typed(join_descriptor.columns[4].value_descriptor,
               "{\"host\":\"a\",\"zone\":\"east\"}")},
    };
    join_rows.input_rows.push_back(std::move(row));
  }
  const auto inserted_join_rows = api::EngineInsertRows(join_rows);
  if (!inserted_join_rows.ok || inserted_join_rows.inserted_count != 2) {
    Rollback(context);
    return Require(false, "supplemental relational join rows failed");
  }
  const auto& asof_right_descriptor =
      fixture->descriptors.at(std::string(kAsofRightObjectUuid));
  api::EngineInsertRowsRequest asof_right_rows;
  asof_right_rows.context = context;
  asof_right_rows.target_table.uuid.canonical =
      std::string(kAsofRightObjectUuid);
  asof_right_rows.target_table.object_kind = "table";
  asof_right_rows.require_generated_row_uuid = false;
  asof_right_rows.estimated_row_count = 2;
  for (const auto& [row_uuid, join_uuid, payload, event_timestamp] :
       std::array<std::array<const char*, 4>, 2>{{
           {{"40000000-0000-4000-8000-000000000080",
             "40000000-0000-4000-8000-000000000010", "right-10",
             "2026-08-10T12:00:10.000000000Z"}},
           {{"40000000-0000-4000-8000-000000000081",
             "40000000-0000-4000-8000-000000000040", "right-40",
             "2026-08-10T12:00:40.000000000Z"}},
       }}) {
    api::EngineRowValue row;
    row.requested_row_uuid.canonical = row_uuid;
    row.fields = {
        {"join_uuid",
         typed(asof_right_descriptor.columns[0].value_descriptor,
               join_uuid)},
        {"payload",
         typed(asof_right_descriptor.columns[1].value_descriptor, payload)},
        {"event_timestamp",
         typed(asof_right_descriptor.columns[2].value_descriptor,
               event_timestamp)},
        {"metric_uuid",
         typed(asof_right_descriptor.columns[3].value_descriptor,
               std::string(kMetricOneUuid))},
        {"tags",
         typed(asof_right_descriptor.columns[4].value_descriptor,
               "{\"host\":\"a\",\"zone\":\"east\"}")},
    };
    asof_right_rows.input_rows.push_back(std::move(row));
  }
  const auto inserted_asof_right_rows = api::EngineInsertRows(asof_right_rows);
  if (!inserted_asof_right_rows.ok ||
      inserted_asof_right_rows.inserted_count != 2) {
    Rollback(context);
    return Require(false, "supplemental ASOF-right rows failed");
  }
  return Commit(context) && SetNextTransactionId(*fixture, 800);
}

api::EngineTypedValue TypedValue(const api::EngineDescriptor& descriptor,
                                 std::string encoded) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = std::move(encoded);
  value.setState(api::EngineValueState::value);
  return value;
}

api::EngineRowValue Row(const api::MgaRelationStorageDescriptor& descriptor,
                        const SignedPoint& point) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = point.row_uuid;
  row.fields = {
      {"metric_uuid", TypedValue(descriptor.columns[0].value_descriptor,
                                 point.metric_uuid)},
      {"point_timestamp", TypedValue(descriptor.columns[1].value_descriptor,
                                     point.timestamp)},
      {"tags", TypedValue(descriptor.columns[2].value_descriptor, point.tags)},
      {"value", TypedValue(descriptor.columns[3].value_descriptor, point.value)},
  };
  return row;
}

bool Insert(const Fixture& fixture, const std::string& object_uuid,
            const SignedPoint& point, const bool commit,
            api::EngineRequestContext* retained = nullptr) {
  api::EngineRequestContext context;
  if (!Begin(fixture, "rcp076-insert-" + std::string(point.row_uuid), &context)) {
    return false;
  }
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = object_uuid;
  request.target_table.object_kind = "table";
  request.require_generated_row_uuid = false;
  request.estimated_row_count = 1;
  request.input_rows.push_back(Row(fixture.descriptors.at(object_uuid), point));
  const auto inserted = api::EngineInsertRows(request);
  if (!inserted.ok || inserted.inserted_count != 1) {
    Rollback(context);
    return Require(false, "time-series fixture insert failed");
  }
  if (retained != nullptr) {
    *retained = context;
    return true;
  }
  return commit ? Commit(context) : Rollback(context);
}

bool AppendInvalid(const Fixture& fixture, const std::string& object_uuid,
                   const SignedPoint& point, const bool commit) {
  api::EngineRequestContext context;
  if (!Begin(fixture, "rcp076-invalid-" + std::string(point.row_uuid), &context)) {
    return false;
  }
  api::CrudRowVersionRecord row;
  row.creator_tx = context.local_transaction_id;
  row.table_uuid = object_uuid;
  row.row_uuid = point.row_uuid;
  row.version_uuid = NewUuidText(platform::UuidKind::object);
  row.values = {{"metric_uuid", point.metric_uuid},
                {"point_timestamp", point.timestamp},
                {"tags", point.tags},
                {"value", point.value}};
  std::uint64_t sequence = 0;
  const auto appended = api::AppendMgaRowVersion(context, row, &sequence);
  if (appended.error || sequence == 0) {
    Rollback(context);
    return Require(false, "invalid time-series fixture append failed");
  }
  return commit ? Commit(context) : Rollback(context);
}

bool SeedFixture(Fixture* fixture, api::EngineRequestContext* active_other) {
  for (const auto& point : kBaseRows) {
    if (!Insert(*fixture, std::string(kBaseObjectUuid), point, true)) return false;
  }
  const SignedPoint rolled_back{
      "40000000-0000-4000-8000-000000000010", kMetricOneUuid.data(),
      "2026-08-10T12:00:20.000000000Z", "{\"host\":\"a\",\"zone\":\"east\"}",
      "100"};
  if (!Insert(*fixture, std::string(kBaseObjectUuid), rolled_back, false)) return false;
  const SignedPoint pre_epoch{
      "40000000-0000-4000-8000-000000000012", kMetricOneUuid.data(),
      "1969-12-31T23:59:30.000000000Z", "{}", "1"};
  if (!Insert(*fixture, std::string(kPreEpochObjectUuid), pre_epoch, true)) return false;
  const SignedPoint duplicate_tag{
      "40000000-0000-4000-8000-000000000013", kMetricOneUuid.data(),
      "2026-08-10T12:00:10.000000000Z", "{\"host\":\"a\",\"host\":\"b\"}", "1"};
  if (!Insert(*fixture, std::string(kDuplicateTagObjectUuid), duplicate_tag, true)) {
    return false;
  }
  const SignedPoint nonfinite{
      "40000000-0000-4000-8000-000000000014", kMetricOneUuid.data(),
      "2026-08-10T12:00:10.000000000Z", "{\"host\":\"a\",\"zone\":\"east\"}",
      "NaN"};
  if (!AppendInvalid(*fixture, std::string(kNonfiniteObjectUuid), nonfinite, true)) {
    return false;
  }
  const SignedPoint invisible_invalid{
      "40000000-0000-4000-8000-000000000015", kMetricOneUuid.data(),
      "malformed", "{\"host\":\"a\",\"host\":\"b\"}", "NaN"};
  if (!AppendInvalid(*fixture, std::string(kInvisibleInvalidObjectUuid),
                     invisible_invalid, false)) {
    return false;
  }
  const SignedPoint unicode_low{
      "40000000-0000-4000-8000-000000000016", kMetricOneUuid.data(),
      "2026-08-10T12:00:10.000000000Z",
      "{\"label\":\"\xc2\x80\"}", "1"};
  const SignedPoint unicode_surrogate_pair{
      "40000000-0000-4000-8000-000000000017", kMetricOneUuid.data(),
      "2026-08-10T12:00:10.000000000Z",
      "{\"label\":\"\\uD83D\\uDE00\"}", "2"};
  if (!Insert(*fixture, std::string(kUnicodeOrderObjectUuid), unicode_low,
              true) ||
      !Insert(*fixture, std::string(kUnicodeOrderObjectUuid),
              unicode_surrogate_pair, true)) {
    return false;
  }
  const SignedPoint lone_surrogate{
      "40000000-0000-4000-8000-000000000018", kMetricOneUuid.data(),
      "2026-08-10T12:00:10.000000000Z",
      "{\"label\":\"\\uD83D\"}", "1"};
  const SignedPoint reversed_surrogate{
      "40000000-0000-4000-8000-000000000019", kMetricOneUuid.data(),
      "2026-08-10T12:00:10.000000000Z",
      "{\"label\":\"\\uDE00\\uD83D\"}", "1"};
  if (!Insert(*fixture, std::string(kLoneSurrogateObjectUuid),
              lone_surrogate, true) ||
      !Insert(*fixture, std::string(kReversedSurrogateObjectUuid),
              reversed_surrogate, true)) {
    return false;
  }
  const std::array<SignedPoint, 5> output_bound_points{{
      {"40000000-0000-4000-8000-000000000020", kMetricOneUuid.data(),
       "2026-08-10T11:58:00.000000000Z", "{}", "1"},
      {"40000000-0000-4000-8000-000000000021", kMetricOneUuid.data(),
       "2026-08-10T11:59:00.000000000Z", "{}", "2"},
      {"40000000-0000-4000-8000-000000000022", kMetricOneUuid.data(),
       "2026-08-10T12:01:00.000000000Z", "{}", "3"},
      {"40000000-0000-4000-8000-000000000023", kMetricOneUuid.data(),
       "2026-08-10T12:02:00.000000000Z", "{}", "4"},
      {"40000000-0000-4000-8000-000000000024", kMetricOneUuid.data(),
       "2026-08-10T12:03:00.000000000Z", "{}", "5"},
  }};
  for (const auto& point : output_bound_points) {
    if (!Insert(*fixture, std::string(kOutputBoundObjectUuid), point, true)) {
      return false;
    }
  }
  if (!SetNextTransactionId(*fixture, 850)) return false;
  const SignedPoint pending{
      "40000000-0000-4000-8000-000000000011", kMetricOneUuid.data(),
      "2026-08-10T12:00:25.000000000Z", "{\"host\":\"a\",\"zone\":\"east\"}",
      "200"};
  if (!Insert(*fixture, std::string(kBaseObjectUuid), pending, false,
              active_other) ||
      active_other->local_transaction_id != 850 ||
      !SetNextTransactionId(*fixture, 900)) {
    return false;
  }
  api::EngineRequestContext high_water;
  return Begin(*fixture, "rcp076-high-water-900", &high_water) &&
         Require(high_water.local_transaction_id == 900,
                 "snapshot high-water transaction 900 was not seeded") &&
         Commit(high_water);
}

bool PublishReaderContext(const Fixture& fixture,
                          api::EngineRequestContext* context) {
  if (!Begin(fixture, "rcp076-reader", context)) return false;
  context->statement_uuid.canonical = NewUuidText(platform::UuidKind::object);
  context->statement_timestamp = std::string(kRangeStart);
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
  context->optimizer_memory_budget_bytes = 16 * 1024 * 1024;
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
    return Require(false, "signed high-water 900 / active-other [850] drifted");
  }
  for (const auto object_uuid :
       {kBaseObjectUuid, kPreEpochObjectUuid, kDuplicateTagObjectUuid,
        kNonfiniteObjectUuid, kInvisibleInvalidObjectUuid,
        kJoinObjectUuid, kUnicodeOrderObjectUuid,
        kLoneSurrogateObjectUuid, kReversedSurrogateObjectUuid,
        kOutputBoundObjectUuid, kAsofRightObjectUuid}) {
    AddAuthorization(context, std::string(object_uuid));
  }
  return true;
}

api::EngineBoundTimeSeriesReadRequestV1 Request(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& descriptor,
    const api::EngineBoundTimeSeriesReadOperationV1 operation,
    const api::EngineBoundTimeSeriesAggregateV1 aggregate,
    std::string start = std::string(kRangeStart),
    std::string end = std::string(kRangeEnd)) {
  api::EngineBoundTimeSeriesReadRequestV1 request;
  request.context = context;
  request.operation = operation;
  request.aggregate = aggregate;
  request.object_uuid = descriptor.relation_uuid.canonical;
  request.range_start = TypedValue(descriptor.columns[1].value_descriptor,
                                   std::move(start));
  request.range_end = TypedValue(descriptor.columns[1].value_descriptor,
                                 std::move(end));
  request.bucket_interval_ns =
      operation == api::EngineBoundTimeSeriesReadOperationV1::kBucketDownsample
          ? kMinuteNs
          : 0;
  request.expected_descriptor_uuid = descriptor.descriptor_uuid.canonical;
  request.expected_descriptor_generation = descriptor.descriptor_generation;
  request.selected_alternative_uuid = NewUuidText(platform::UuidKind::object);
  request.capability_uuid = NewUuidText(platform::UuidKind::object);
  request.provider_uuid = NewUuidText(platform::UuidKind::object);
  request.provider_generation = descriptor.descriptor_generation + 3'000;
  request.maximum_scanned_row_versions = 4096;
  request.maximum_decoded_bytes = 1024 * 1024;
  request.maximum_output_rows = 4096;
  request.maximum_groups = 4096;
  request.maximum_tag_bytes = 1024 * 1024;
  request.maximum_result_bytes = 1024 * 1024;
  request.maximum_memory_bytes = 2 * 1024 * 1024;
  request.cancellation_requested = [] { return false; };
  return request;
}

std::string Diagnostic(const api::EngineApiResult& result) {
  return result.diagnostics.empty() ? std::string{}
                                    : result.diagnostics.front().code;
}

std::string DiagnosticDetail(const api::EngineApiResult& result) {
  return result.diagnostics.empty() ? std::string{}
                                    : result.diagnostics.front().detail;
}

std::string RealBits(const double value) {
  std::ostringstream out;
  out << std::hex << std::nouppercase << std::setfill('0') << std::setw(16)
      << std::bit_cast<std::uint64_t>(value);
  return out.str();
}

std::string RawStream(const api::EngineBoundTimeSeriesReadResultV1& result) {
  std::string bytes;
  for (const auto& row : result.rows) {
    bytes += row.row_uuid + "\t" + row.series_uuid + "\t" + row.metric_uuid +
             "\t" + row.point_timestamp + "\t" + row.tags + "\t" +
             RealBits(row.value) + "\n";
  }
  return bytes;
}

std::string DownsampleStream(
    const api::EngineBoundTimeSeriesReadResultV1& result,
    const api::EngineBoundTimeSeriesAggregateV1 aggregate) {
  std::string bytes;
  for (const auto& row : result.downsample_rows) {
    const auto value = aggregate == api::EngineBoundTimeSeriesAggregateV1::kCount
                           ? std::to_string(row.aggregate_count)
                           : RealBits(row.aggregate_value);
    bytes += row.series_uuid + "\t" + row.metric_uuid + "\t" +
             row.bucket_start + "\t" + row.bucket_end + "\t" + row.tags +
             "\t" + std::to_string(row.sample_count) + "\t" + value + "\n";
  }
  return bytes;
}

std::string Digest(const std::string& bytes) {
  const auto digest = hash::ComputeSha256Digest(
      reinterpret_cast<const platform::byte*>(bytes.data()), bytes.size());
  return digest.ok() ? hash::HexLower(digest.digest) : std::string{};
}

bool InventoryExact(const Fixture& fixture,
                    const api::EngineRequestContext& reader) {
  const auto loaded =
      db::LoadLocalTransactionInventoryFromDatabase(fixture.database_path.string());
  if (!loaded.ok()) return Require(false, "signed inventory could not be loaded");
  const auto state = [&](const std::uint64_t id)
      -> std::optional<txn::TransactionState> {
    const auto found = std::ranges::find_if(
        loaded.inventory.entries,
        [&](const auto& entry) { return entry.identity.local_id.value == id; });
    return found == loaded.inventory.entries.end()
               ? std::nullopt
               : std::optional<txn::TransactionState>(found->state);
  };
  bool exact = reader.local_transaction_id == 901 &&
               loaded.inventory.next_local_transaction_id == 902 &&
               state(809) == txn::TransactionState::rolled_back &&
               state(813) == txn::TransactionState::rolled_back &&
               state(850) == txn::TransactionState::active &&
               state(900) == txn::TransactionState::committed;
  for (std::uint64_t id = 800; id <= 808; ++id) {
    exact = exact && state(id) == txn::TransactionState::committed;
  }
  for (const auto id : {810ULL, 811ULL, 812ULL}) {
    exact = exact && state(id) == txn::TransactionState::committed;
  }
  return Require(exact, "signed transaction inventory drifted");
}

exec::PhysicalMgaStatementContext PhysicalMga(
    const api::EngineRequestContext& context) {
  api::EngineResolveStatementSnapshotRequest resolve;
  resolve.context = context;
  const auto snapshot = api::EngineResolveStatementSnapshot(resolve);
  exec::PhysicalMgaStatementContext mga;
  if (!snapshot.ok) return mga;
  const auto& vector = snapshot.snapshot_vector;
  mga.statement_uuid = context.statement_uuid.canonical;
  mga.statement_timestamp = context.statement_timestamp;
  mga.owning_transaction_uuid = context.transaction_uuid.canonical;
  mga.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  mga.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  mga.owning_local_transaction_id = vector.owning_transaction.value;
  mga.visible_committed_high_watermark =
      vector.visible_committed_high_watermark;
  mga.oldest_active_transaction_id = vector.oldest_active_transaction.value;
  mga.oldest_interesting_transaction_id =
      vector.oldest_interesting_transaction.value;
  mga.oldest_snapshot_transaction_id = vector.oldest_snapshot_transaction.value;
  mga.retention_horizon_transaction_id =
      vector.retention_horizon_transaction.value;
  mga.active_excluded_local_transaction_ids =
      vector.active_excluded_local_transaction_ids;
  mga.in_doubt_excluded_local_transaction_ids =
      vector.in_doubt_excluded_local_transaction_ids;
  mga.snapshot_kind = txn::SnapshotVectorKindName(vector.snapshot_kind);
  mga.publication_inventory_next_local_transaction_id =
      vector.publication_inventory_next_local_transaction_id;
  mga.inventory_authoritative = vector.inventory_authoritative;
  mga.complete = vector.complete;
  mga.current = true;
  return mga;
}

plan::CanonicalMgaStatementContext LogicalMga(
    const api::EngineRequestContext& context) {
  const auto physical = PhysicalMga(context);
  plan::CanonicalMgaStatementContext logical;
  logical.statement_uuid = physical.statement_uuid;
  logical.statement_timestamp = physical.statement_timestamp;
  logical.owning_transaction_uuid = physical.owning_transaction_uuid;
  logical.statement_snapshot_uuid = physical.statement_snapshot_uuid;
  logical.statement_metadata_snapshot_uuid =
      physical.statement_metadata_snapshot_uuid;
  logical.owning_local_transaction_id =
      physical.owning_local_transaction_id;
  logical.visible_committed_high_watermark =
      physical.visible_committed_high_watermark;
  logical.oldest_active_transaction_id =
      physical.oldest_active_transaction_id;
  logical.oldest_interesting_transaction_id =
      physical.oldest_interesting_transaction_id;
  logical.oldest_snapshot_transaction_id =
      physical.oldest_snapshot_transaction_id;
  logical.retention_horizon_transaction_id =
      physical.retention_horizon_transaction_id;
  logical.active_excluded_local_transaction_ids =
      physical.active_excluded_local_transaction_ids;
  logical.in_doubt_excluded_local_transaction_ids =
      physical.in_doubt_excluded_local_transaction_ids;
  logical.snapshot_kind = physical.snapshot_kind;
  logical.publication_inventory_next_local_transaction_id =
      physical.publication_inventory_next_local_transaction_id;
  logical.inventory_authoritative = physical.inventory_authoritative;
  logical.complete = physical.complete;
  logical.current = physical.current;
  return logical;
}

std::string CoreTypeUuid(const std::string_view stable_name) {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto descriptor = std::ranges::find_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  return descriptor == manifest.manifest.descriptor_rows.end()
             ? std::string{}
             : uuid::UuidToString(descriptor->descriptor_uuid.value);
}

std::vector<opt::MultilegDescriptorProfileV1>
Rcp079DirectProofMultilegProfilesV10() {
  const std::array<std::string, 5> type_uuids{
      CoreTypeUuid("uuid"), CoreTypeUuid("uint64"),
      CoreTypeUuid("real64"), CoreTypeUuid("boolean"),
      CoreTypeUuid("geometry")};
  std::vector<opt::MultilegDescriptorProfileV1> profiles;
  profiles.reserve(320);
  for (std::uint16_t type_pair = 0; type_pair < type_uuids.size();
       ++type_pair) {
    for (std::uint16_t nullable = 0; nullable < 2; ++nullable) {
      const auto kind =
          static_cast<std::uint8_t>(14 + type_pair * 2 + nullable);
      for (std::uint16_t slot = 0; slot < 32; ++slot) {
        profiles.push_back(
            {kind, slot, NewUuidText(platform::UuidKind::object),
             type_uuids[type_pair], nullable != 0});
      }
    }
  }
  return profiles;
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

api::EngineDescriptor DerivedDescriptor(const std::string& instance_uuid,
                                        const std::string_view public_type,
                                        const std::string_view core_type) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = instance_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::string(public_type);
  descriptor.encoded_descriptor =
      "canonical=" + std::string(public_type) +
      ";type_uuid=" + CoreTypeUuid(core_type) + ";nullable=false";
  return descriptor;
}

std::string Number(const double value) {
  if (value == 0.0) return "0";
  std::array<char, 64> encoded{};
  const auto converted = std::to_chars(
      encoded.data(), encoded.data() + encoded.size(), value,
      std::chars_format::general,
      std::numeric_limits<double>::max_digits10);
  return converted.ec == std::errc{}
             ? std::string(encoded.data(), converted.ptr)
             : std::string{};
}

struct ExchangeRun {
  exec::ModelFamilyExecutionResultV1 result;
  std::size_t provider_calls{0};
  std::size_t cleanup_calls{0};
  std::size_t cancellation_checks{0};
  std::uint64_t preferred_access_invocation_count{0};
  std::uint64_t exact_fallback_access_invocation_count{0};
  std::string selected_access_path_id;
  std::string provider_stream;
};

enum class TimeSeriesExchangeMutation : std::uint8_t {
  kNone,
  kOrder,
  kDuplicateRowUuid,
  kNoncanonicalTags,
  kNoncanonicalNumeric,
  kNoncanonicalSampleCount,
  kSubstituteRawValue,
  kSubstituteSampleCount,
  kSubstituteAggregateValue,
};

std::string ExactExchangeStream(
    const exec::ModelFamilyExecutionResultV1& execution) {
  std::ostringstream out;
  const auto& output = execution.output;
  out << output.properties.ordering_id << '|'
      << output.properties.partitioning_id << '|'
      << output.properties.uniqueness_id << '\n';
  for (const auto descriptor_id : output.output_descriptor_ids) {
    out << descriptor_id << ',';
  }
  out << '\n';
  for (const auto& column : output.batch.columns) {
    out << column.descriptor_id << '|' << column.stable_name << '|'
        << column.descriptor.descriptor_uuid.canonical << '|'
        << column.descriptor.canonical_type_name << '|'
        << column.descriptor.encoded_descriptor << '|'
        << column.nullable << '\n';
  }
  for (const auto& identity : output.ordered_row_identities) {
    out << identity.row_uuid << '|' << identity.series_uuid << '|'
        << identity.metric_uuid << '|' << identity.tags << '|'
        << identity.point_timestamp_ns << '|' << identity.bucket_start_ns
        << '|' << identity.time_series_payload_kind << '|'
        << identity.time_series_raw_value << '|'
        << identity.time_series_sample_count << '|'
        << identity.time_series_aggregate_value
        << '\n';
  }
  for (const auto& row : output.batch.rows) {
    for (const auto& value : row.values) {
      out << static_cast<unsigned>(value.state) << ':' << value.is_null << ':'
          << value.descriptor.descriptor_uuid.canonical << ':'
          << value.descriptor.canonical_type_name << ':'
          << value.descriptor.encoded_descriptor << ':'
          << value.encoded_value << ':';
      for (const auto byte : value.binary_value) {
        out << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned>(byte);
      }
      out << std::dec << ';';
    }
    out << '\n';
  }
  return out.str();
}

ExchangeRun ExecuteThroughCommonSpine(
    api::EngineBoundTimeSeriesReadRequestV1 provider_request,
    const api::MgaRelationStorageDescriptor& descriptor,
    const bool fallback_available = true,
    const bool current_provider_stale = false,
    const bool current_mga_substituted = false,
    const bool output_substituted = false,
    const bool fault_injected = false,
    const opt::ModelFamilyCoordinatorResultV1* coordinated = nullptr,
    const TimeSeriesExchangeMutation mutation =
        TimeSeriesExchangeMutation::kNone,
    const std::size_t exchange_cancel_threshold = 0,
    const std::uint64_t exchange_memory_override = 0) {
  ExchangeRun observed;
  const bool raw = provider_request.operation ==
                   api::EngineBoundTimeSeriesReadOperationV1::kRangeRead;
  const auto mga = PhysicalMga(provider_request.context);
  if (coordinated != nullptr && coordinated->accepted &&
      coordinated->selected && coordinated->physical_dag.nodes.size() == 1) {
    provider_request.selected_alternative_uuid =
        coordinated->selected_candidate.alternative_uuid;
    provider_request.capability_uuid =
        coordinated->selected_candidate.capability_uuid;
    provider_request.provider_uuid =
        coordinated->selected_candidate.provider_uuid;
    provider_request.provider_generation =
        coordinated->selected_candidate.provider_generation;
    provider_request.exact_fallback_selected =
        coordinated->exact_fallback_selected;
  } else {
    provider_request.selected_alternative_uuid =
        NewUuidText(platform::UuidKind::object);
    provider_request.capability_uuid = NewUuidText(platform::UuidKind::object);
    provider_request.provider_uuid = NewUuidText(platform::UuidKind::object);
  }

  exec::ModelFamilyExecutionRequestV1 request;
  auto& input = request.input;
  input.family_id = "time_series";
  input.operation_id = raw ? "TIME_SERIES_RANGE_READ"
                           : "TIME_SERIES_DOWNSAMPLE";
  input.object_uuid = provider_request.object_uuid;
  input.physical_node_id =
      coordinated != nullptr && coordinated->accepted &&
              coordinated->selected &&
              coordinated->physical_dag.nodes.size() == 1
          ? coordinated->physical_dag.nodes.front().physical_node_id
          : 76;
  input.selected_alternative_uuid =
      provider_request.selected_alternative_uuid;
  input.capability_uuid = provider_request.capability_uuid;
  input.provider_uuid = provider_request.provider_uuid;
  input.provider_generation = provider_request.provider_generation;
  input.result_handle_uuid = NewUuidText(platform::UuidKind::object);
  input.causal_counter_id =
      coordinated != nullptr && coordinated->accepted &&
              coordinated->selected &&
              coordinated->physical_dag.nodes.size() == 1
          ? coordinated->physical_dag.nodes.front().causal_counter_id
          : 1;
  input.output_descriptor_ids = raw
                                    ? std::vector<std::uint32_t>{101, 102, 103,
                                                                 104, 105, 106}
                                    : std::vector<std::uint32_t>{201, 202, 203,
                                                                 204, 205, 206,
                                                                 207};
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
  input.maximum_cells = provider_request.maximum_output_rows *
                        input.output_descriptor_ids.size();
  input.maximum_memory_bytes =
      exchange_memory_override == 0 ? provider_request.maximum_memory_bytes
                                    : exchange_memory_override;
  input.exact_fallback_selected = provider_request.exact_fallback_selected;

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
  request.current_catalog_generation = input.catalog_generation;
  request.current_descriptor_generation = input.descriptor_generation;
  request.current_security_generation = input.security_generation;
  request.current_policy_generation = input.policy_generation;
  request.current_resource_generation = input.resource_generation;
  request.current_provider_generation =
      input.provider_generation + (current_provider_stale ? 1 : 0);
  request.current_mga_statement_context = input.mga_statement_context;
  if (current_mga_substituted) {
    ++request.current_mga_statement_context.visible_committed_high_watermark;
  }
  request.cancellation_requested =
      [&observed, exchange_cancel_threshold] {
        ++observed.cancellation_checks;
        return exchange_cancel_threshold != 0 &&
               observed.cancellation_checks >= exchange_cancel_threshold;
      };
  request.cleanup_provider = [&observed] { ++observed.cleanup_calls; };
  request.exact_fallback_selected = input.exact_fallback_selected;
  request.fault_injected = false;

  std::vector<exec::ExecutorColumnDescriptor> columns;
  if (raw) {
    columns = {
        {"row_uuid",
         DerivedDescriptor(descriptor.descriptor_uuid.canonical, "uuid", "uuid"),
         false, 101},
        {"series_uuid",
         DerivedDescriptor(descriptor.schema_uuid.canonical, "uuid", "uuid"),
         false, 102},
        {"metric_uuid", descriptor.columns[0].value_descriptor, false, 103},
        {"point_timestamp", descriptor.columns[1].value_descriptor, false, 104},
        {"tags", descriptor.columns[2].value_descriptor, false, 105},
        {"value", descriptor.columns[3].value_descriptor, false, 106},
    };
  } else {
    const bool count = provider_request.aggregate ==
                       api::EngineBoundTimeSeriesAggregateV1::kCount;
    columns = {
        {"series_uuid",
         DerivedDescriptor(descriptor.schema_uuid.canonical, "uuid", "uuid"),
         false, 201},
        {"metric_uuid", descriptor.columns[0].value_descriptor, false, 202},
        {"bucket_start", descriptor.columns[1].value_descriptor, false, 203},
        {"bucket_end",
         DerivedDescriptor(descriptor.columns[1].column_uuid.canonical,
                           "timestamp_tz", "timestamp"),
         false, 204},
        {"tags", descriptor.columns[2].value_descriptor, false, 205},
        {"sample_count",
         DerivedDescriptor(CoreTypeUuid("int64"), "int64", "int64"),
         false, 206},
        {"aggregate_value",
         count ? DerivedDescriptor(descriptor.columns[3].column_uuid.canonical,
                                   "int64", "int64")
               : descriptor.columns[3].value_descriptor,
         false, 207},
    };
  }
  for (std::size_t index = 0; index < columns.size(); ++index) {
    columns[index].descriptor_id = input.output_descriptor_ids[index];
    columns[index].descriptor.descriptor_kind = "scalar";
  }

  request.execute_provider =
      [provider_request, columns, raw, output_substituted, fault_injected,
       mutation,
       &observed](const auto& selected) mutable {
        ++observed.provider_calls;
        exec::ModelProviderExecutionResultV1 provider;
        const auto read = api::EngineBoundTimeSeriesReadV1(provider_request);
        observed.preferred_access_invocation_count =
            read.preferred_access_invocation_count;
        observed.exact_fallback_access_invocation_count =
            read.exact_fallback_access_invocation_count;
        observed.selected_access_path_id = read.selected_access_path_id;
        observed.provider_stream =
            raw ? RawStream(read)
                : DownsampleStream(read, provider_request.aggregate);
        provider.data_access_observed = read.data_access_observed;
        provider.rows_examined = read.scanned_row_version_count;
        if (fault_injected && read.ok) {
          provider.diagnostic_id = "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
          provider.detail =
              "injected provider-leg failure after engine acquisition";
          return provider;
        }
        if (!read.ok) {
          provider.diagnostic_id = read.diagnostics.empty()
                                       ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                                       : read.diagnostics.front().code;
          provider.detail = read.diagnostics.empty()
                                ? "time-series provider failed"
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
        if (output_substituted) ++batch.output_descriptor_ids.front();
        batch.batch.columns = columns;
        batch.mga_statement_context = selected.mga_statement_context;
        batch.security_receipt_uuid = NewUuidText(platform::UuidKind::object);
        batch.properties.property_uuid = NewUuidText(platform::UuidKind::object);
        batch.properties.ordering_id = read.ordering_id;
        batch.properties.partitioning_id = "single_local_partition";
        batch.properties.uniqueness_id = read.rows.empty()
                                             ? "series_metric_tags_bucket_v1"
                                             : "row_uuid";
        batch.properties.exact = true;
        batch.properties.residual_recheck_complete = true;
        batch.properties.base_row_mga_recheck_complete = true;
        batch.properties.security_recheck_complete = true;
        batch.residual_recheck_complete = true;
        batch.base_row_mga_recheck_complete = true;
        batch.security_recheck_complete = true;
        for (const auto& row : read.rows) {
          const auto raw_value = Number(row.value);
          exec::DescriptorTuple tuple;
          tuple.values.reserve(6);
          tuple.values.push_back(
              TypedValue(columns[0].descriptor, row.row_uuid));
          tuple.values.push_back(
              TypedValue(columns[1].descriptor, row.series_uuid));
          tuple.values.push_back(
              TypedValue(columns[2].descriptor, row.metric_uuid));
          tuple.values.push_back(
              TypedValue(columns[3].descriptor, row.point_timestamp));
          tuple.values.push_back(TypedValue(columns[4].descriptor, row.tags));
          tuple.values.push_back(
              TypedValue(columns[5].descriptor, raw_value));
          batch.batch.rows.push_back(std::move(tuple));
          exec::ModelProviderRowIdentityV1 identity;
          identity.row_uuid = row.row_uuid;
          identity.series_uuid = row.series_uuid;
          identity.metric_uuid = row.metric_uuid;
          identity.tags = row.tags;
          identity.point_timestamp_ns = row.point_timestamp_ns;
          identity.time_series_payload_kind = "raw.real64.v1";
          identity.time_series_raw_value = raw_value;
          batch.ordered_row_identities.push_back(std::move(identity));
        }
        for (const auto& row : read.downsample_rows) {
          const bool count = columns[6].descriptor.canonical_type_name == "int64";
          const auto sample_count = std::to_string(row.sample_count);
          const auto aggregate_value =
              count ? std::to_string(row.aggregate_count)
                    : Number(row.aggregate_value);
          exec::DescriptorTuple tuple;
          tuple.values.reserve(7);
          tuple.values.push_back(
              TypedValue(columns[0].descriptor, row.series_uuid));
          tuple.values.push_back(
              TypedValue(columns[1].descriptor, row.metric_uuid));
          tuple.values.push_back(
              TypedValue(columns[2].descriptor, row.bucket_start));
          tuple.values.push_back(
              TypedValue(columns[3].descriptor, row.bucket_end));
          tuple.values.push_back(TypedValue(columns[4].descriptor, row.tags));
          tuple.values.push_back(
              TypedValue(columns[5].descriptor, sample_count));
          tuple.values.push_back(
              TypedValue(columns[6].descriptor, aggregate_value));
          batch.batch.rows.push_back(std::move(tuple));
          exec::ModelProviderRowIdentityV1 identity;
          identity.series_uuid = row.series_uuid;
          identity.metric_uuid = row.metric_uuid;
          identity.tags = row.tags;
          identity.bucket_start_ns = row.bucket_start_ns;
          switch (provider_request.aggregate) {
            case api::EngineBoundTimeSeriesAggregateV1::kCount:
              identity.time_series_payload_kind =
                  "downsample.count.int64.v1";
              break;
            case api::EngineBoundTimeSeriesAggregateV1::kSum:
              identity.time_series_payload_kind =
                  "downsample.sum.real64.v1";
              break;
            case api::EngineBoundTimeSeriesAggregateV1::kMin:
              identity.time_series_payload_kind =
                  "downsample.min.real64.v1";
              break;
            case api::EngineBoundTimeSeriesAggregateV1::kMax:
              identity.time_series_payload_kind =
                  "downsample.max.real64.v1";
              break;
            case api::EngineBoundTimeSeriesAggregateV1::kAvg:
              identity.time_series_payload_kind =
                  "downsample.avg.real64.v1";
              break;
            case api::EngineBoundTimeSeriesAggregateV1::kNone:
              break;
          }
          identity.time_series_sample_count = sample_count;
          identity.time_series_aggregate_value = aggregate_value;
          batch.ordered_row_identities.push_back(std::move(identity));
        }
        if (mutation == TimeSeriesExchangeMutation::kOrder &&
            batch.batch.rows.size() > 1) {
          std::swap(batch.batch.rows.front(), batch.batch.rows.back());
          std::swap(batch.ordered_row_identities.front(),
                    batch.ordered_row_identities.back());
        } else if (mutation ==
                       TimeSeriesExchangeMutation::kDuplicateRowUuid &&
                   raw && batch.batch.rows.size() > 1) {
          batch.ordered_row_identities.back().row_uuid =
              batch.ordered_row_identities.front().row_uuid;
          batch.batch.rows.back().values[0].encoded_value =
              batch.batch.rows.front().values[0].encoded_value;
        } else if (mutation ==
                       TimeSeriesExchangeMutation::kNoncanonicalTags &&
                   !batch.batch.rows.empty()) {
          batch.ordered_row_identities.front().tags =
              "{\"zone\":\"east\",\"host\":\"a\"}";
          batch.batch.rows.front().values[4].encoded_value =
              batch.ordered_row_identities.front().tags;
        } else if (mutation ==
                       TimeSeriesExchangeMutation::kNoncanonicalNumeric &&
                   !batch.batch.rows.empty()) {
          batch.batch.rows.front().values[raw ? 5 : 6].encoded_value = "NaN";
        } else if (mutation == TimeSeriesExchangeMutation::
                                   kNoncanonicalSampleCount &&
                   !raw && !batch.batch.rows.empty()) {
          batch.batch.rows.front().values[5].encoded_value = "01";
        } else if (mutation ==
                       TimeSeriesExchangeMutation::kSubstituteRawValue &&
                   raw && !batch.batch.rows.empty()) {
          batch.batch.rows.front().values[5].encoded_value = "2";
        } else if (mutation == TimeSeriesExchangeMutation::
                                   kSubstituteSampleCount &&
                   !raw && !batch.batch.rows.empty()) {
          batch.batch.rows.front().values[5].encoded_value = "2";
        } else if (mutation == TimeSeriesExchangeMutation::
                                   kSubstituteAggregateValue &&
                   !raw && !batch.batch.rows.empty()) {
          batch.batch.rows.front().values[6].encoded_value = "2";
        }
        return provider;
      };
  observed.result = exec::ExecuteModelFamilySourceV1(request);
  return observed;
}

bool CoordinatorMatrix(const api::EngineRequestContext& context,
                       const api::MgaRelationStorageDescriptor& descriptor,
                       std::set<std::string>* completed) {
  opt::ModelFamilyCoordinatorRequestV1 request;
  request.family_id = "time_series";
  request.operation_id = "TIME_SERIES_RANGE_READ";
  request.logical_operator_id = "LOGICAL_TIME_SERIES_SOURCE_V1";
  request.logical_node_id = 76;
  request.object_uuid = descriptor.relation_uuid.canonical;
  request.output_descriptor_ids = {101, 102, 103, 104, 105, 106};
  request.mga_statement_context = PhysicalMga(context);
  request.bound_sblr_tree_uuid = NewUuidText(platform::UuidKind::object);
  request.catalog_epoch_uuid = context.catalog_epoch_uuid.canonical;
  request.security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  request.capability_snapshot_uuid = NewUuidText(platform::UuidKind::object);
  request.resource_snapshot_uuid = NewUuidText(platform::UuidKind::object);
  request.statistics_snapshot_uuid = NewUuidText(platform::UuidKind::object);
  request.route_snapshot_uuid = NewUuidText(platform::UuidKind::object);
  request.catalog_generation = context.catalog_generation_id;
  request.current_catalog_generation = context.catalog_generation_id;
  request.security_epoch = context.security_epoch;
  request.policy_epoch = context.authorization_context.policy_epoch;
  request.resource_epoch = context.resource_epoch;
  request.statistics_generation = context.catalog_generation_id;
  request.route_epoch = 80;
  request.route_generation = 81;
  request.memory_budget_bytes = 2 * 1024 * 1024;
  const auto candidate = [&](const bool fallback, const std::uint64_t cost) {
    opt::ModelFamilyCandidateV1 candidate;
    candidate.alternative_uuid = NewUuidText(platform::UuidKind::object);
    candidate.provider_uuid = NewUuidText(platform::UuidKind::object);
    candidate.capability_uuid = NewUuidText(platform::UuidKind::object);
    candidate.implementation_id = "physical_time_series_range_scan_v1";
    candidate.provider_generation = descriptor.descriptor_generation + 3'000;
    candidate.available = true;
    candidate.exact = true;
    candidate.exact_collection_fallback = fallback;
    candidate.cost.cost_vector_uuid = NewUuidText(platform::UuidKind::object);
    candidate.cost.cpu_units = cost;
    candidate.cost.memory_bytes_required = 4096;
    return candidate;
  };
  request.candidates = {candidate(false, 1), candidate(true, 2)};
  const auto preferred = opt::CoordinateModelFamilySourceV1(request);
  request.candidates[0].available = false;
  const auto fallback = opt::CoordinateModelFamilySourceV1(request);
  request.candidates[1].available = false;
  const auto absent = opt::CoordinateModelFamilySourceV1(request);
  const bool exact =
      preferred.accepted && preferred.selected &&
      !preferred.exact_fallback_selected && fallback.accepted &&
      fallback.selected && fallback.exact_fallback_selected &&
      request.candidates[0].provider_generation ==
          descriptor.descriptor_generation + 3'000 &&
      request.candidates[1].provider_generation ==
          descriptor.descriptor_generation + 3'000 &&
      request.candidates[0].provider_generation !=
          descriptor.descriptor_generation &&
      preferred.selected_candidate.provider_generation ==
          request.candidates[0].provider_generation &&
      fallback.selected_candidate.provider_generation ==
          request.candidates[1].provider_generation &&
      fallback.selected_candidate.alternative_uuid ==
          request.candidates[1].alternative_uuid &&
      !absent.accepted && !absent.selected &&
      absent.diagnostic_id ==
          "SB_MODEL_TIME_SERIES_EXACT_FALLBACK_UNAVAILABLE_V1";
  completed->insert("TS-26");
  completed->insert("TS-33");
  return Require(exact,
                 "TS-26/TS-33 provider/fallback coordinator selection drifted");
}

bool ExchangeMatrix(const Fixture& fixture,
                    const api::EngineRequestContext& context,
                    std::set<std::string>* completed) {
  bool passed = true;
  const auto& base = fixture.descriptors.at(std::string(kBaseObjectUuid));
  const auto raw_request =
      Request(context, base,
              api::EngineBoundTimeSeriesReadOperationV1::kRangeRead,
              api::EngineBoundTimeSeriesAggregateV1::kNone);
  const auto coordinate_fallback = [&](const bool raw) {
    opt::ModelFamilyCoordinatorRequestV1 request;
    request.family_id = "time_series";
    request.operation_id = raw ? "TIME_SERIES_RANGE_READ"
                               : "TIME_SERIES_DOWNSAMPLE";
    request.logical_operator_id = "LOGICAL_TIME_SERIES_SOURCE_V1";
    request.logical_node_id = 76;
    request.object_uuid = base.relation_uuid.canonical;
    request.output_descriptor_ids =
        raw ? std::vector<std::uint32_t>{101, 102, 103, 104, 105, 106}
            : std::vector<std::uint32_t>{201, 202, 203, 204, 205, 206,
                                         207};
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
    request.route_snapshot_uuid =
        context.optimizer_route_snapshot_uuid.canonical;
    request.catalog_generation = context.catalog_generation_id;
    request.current_catalog_generation = context.catalog_generation_id;
    request.security_epoch = context.security_epoch;
    request.policy_epoch = context.authorization_context.policy_epoch;
    request.resource_epoch = context.resource_epoch;
    request.statistics_generation = context.catalog_generation_id;
    request.route_epoch = context.optimizer_route_epoch;
    request.route_generation = context.optimizer_route_generation;
    request.memory_budget_bytes = context.optimizer_memory_budget_bytes;
    const auto candidate = [&](const bool fallback,
                               const std::uint64_t cost) {
      opt::ModelFamilyCandidateV1 candidate;
      candidate.alternative_uuid = NewUuidText(platform::UuidKind::object);
      candidate.provider_uuid = NewUuidText(platform::UuidKind::object);
      candidate.capability_uuid = NewUuidText(platform::UuidKind::object);
      candidate.implementation_id =
          "physical_time_series_range_scan_v1";
      candidate.provider_generation = base.descriptor_generation + 3'000;
      candidate.available = fallback;
      candidate.exact = true;
      candidate.exact_collection_fallback = fallback;
      candidate.cost.cost_vector_uuid =
          NewUuidText(platform::UuidKind::object);
      candidate.cost.cpu_units = cost;
      candidate.cost.sequential_read_units = cost;
      candidate.cost.memory_bytes_required = 4096;
      return candidate;
    };
    request.candidates = {candidate(false, 1), candidate(true, 2)};
    return opt::CoordinateModelFamilySourceV1(request);
  };
  const auto success = ExecuteThroughCommonSpine(raw_request, base);
  passed &= Require(success.result.accepted && success.result.root_published &&
                        success.result.cleanup_complete &&
                        success.result.cleanup_count == 1 &&
                        raw_request.provider_generation ==
                            base.descriptor_generation + 3'000 &&
                        raw_request.provider_generation !=
                            base.descriptor_generation &&
                        success.result.output.provider_generation ==
                            raw_request.provider_generation &&
                        success.provider_calls == 1 && success.cleanup_calls == 1,
                    "time-series common-spine success/cleanup drifted");
  std::size_t raw_checkpoint_count = 0;
  auto checkpoint_probe = raw_request;
  checkpoint_probe.cancellation_requested = [&raw_checkpoint_count] {
    ++raw_checkpoint_count;
    return false;
  };
  const auto checkpoint_probe_result =
      api::EngineBoundTimeSeriesReadV1(checkpoint_probe);
  std::size_t scan_cancel_checks = 0;
  auto scan_cancel_request = raw_request;
  scan_cancel_request.cancellation_requested = [&scan_cancel_checks] {
    return ++scan_cancel_checks >= 2;
  };
  const auto scan_cancel =
      ExecuteThroughCommonSpine(scan_cancel_request, base);
  std::size_t publish_cancel_checks = 0;
  auto publish_cancel_request = raw_request;
  publish_cancel_request.cancellation_requested =
      [&publish_cancel_checks, raw_checkpoint_count] {
        return ++publish_cancel_checks >= raw_checkpoint_count;
      };
  const auto publish_cancel =
      ExecuteThroughCommonSpine(publish_cancel_request, base);
  const auto sum_request =
      Request(context, base,
              api::EngineBoundTimeSeriesReadOperationV1::kBucketDownsample,
              api::EngineBoundTimeSeriesAggregateV1::kSum);
  const auto count_request =
      Request(context, base,
              api::EngineBoundTimeSeriesReadOperationV1::kBucketDownsample,
              api::EngineBoundTimeSeriesAggregateV1::kCount);
  std::size_t aggregate_cancel_checks = 0;
  auto aggregate_cancel_request = sum_request;
  aggregate_cancel_request.cancellation_requested =
      [&aggregate_cancel_checks, raw_checkpoint_count] {
        return ++aggregate_cancel_checks >= raw_checkpoint_count;
      };
  const auto aggregate_cancel =
      ExecuteThroughCommonSpine(aggregate_cancel_request, base);
  const auto exchange_cancel = ExecuteThroughCommonSpine(
      raw_request, base, true, false, false, false, false, nullptr,
      TimeSeriesExchangeMutation::kNone, 3);
  const auto exact_cancelled = [](const ExchangeRun& run) {
    return !run.result.accepted && run.result.execution_started &&
           run.result.data_access_observed && !run.result.root_published &&
           run.result.diagnostic_id == "SB_MODEL_EXECUTION_CANCELLED_V1" &&
           run.provider_calls == 1 && run.cleanup_calls == 1 &&
           run.result.cleanup_count == 1;
  };
  passed &= Require(
      checkpoint_probe_result.ok && raw_checkpoint_count > 2 &&
          exact_cancelled(scan_cancel) && exact_cancelled(aggregate_cancel) &&
          exact_cancelled(publish_cancel) && exact_cancelled(exchange_cancel) &&
          exchange_cancel.result.detail.find("exchange") != std::string::npos,
      "TS-30 scan/aggregate/publish cancellation or cleanup drifted");
  completed->insert("TS-30");
  const auto preferred_sum = ExecuteThroughCommonSpine(sum_request, base);
  const auto preferred_count = ExecuteThroughCommonSpine(count_request, base);
  const auto fallback_raw_selection = coordinate_fallback(true);
  const auto fallback_sum_selection = coordinate_fallback(false);
  const auto fallback_count_selection = coordinate_fallback(false);
  const auto fallback_raw = ExecuteThroughCommonSpine(
      raw_request, base, true, false, false, false, false,
      &fallback_raw_selection);
  const auto fallback_sum = ExecuteThroughCommonSpine(
      sum_request, base, true, false, false, false, false,
      &fallback_sum_selection);
  const auto fallback_count = ExecuteThroughCommonSpine(
      count_request, base, true, false, false, false, false,
      &fallback_count_selection);
  const auto fallback_raw_replay = ExecuteThroughCommonSpine(
      raw_request, base, true, false, false, false, false,
      &fallback_raw_selection);
  const auto fallback_sum_replay = ExecuteThroughCommonSpine(
      sum_request, base, true, false, false, false, false,
      &fallback_sum_selection);
  const auto fallback_count_replay = ExecuteThroughCommonSpine(
      count_request, base, true, false, false, false, false,
      &fallback_count_selection);
  const auto exact_fallback_receipt = [](const ExchangeRun& run) {
    return run.result.accepted && run.result.root_published &&
           run.result.cleanup_complete && run.result.cleanup_count == 1 &&
           run.provider_calls == 1 && run.cleanup_calls == 1 &&
           run.preferred_access_invocation_count == 0 &&
           run.exact_fallback_access_invocation_count == 1 &&
           run.selected_access_path_id ==
               "TIME_SERIES_BUCKET_STORE_SCAN_EXACT_V1" &&
           run.result.output.exact_fallback_selected;
  };
  const bool distinct_fallback_identities =
      fallback_raw_selection.accepted && fallback_raw_selection.selected &&
      fallback_raw_selection.exact_fallback_selected &&
      fallback_sum_selection.accepted && fallback_sum_selection.selected &&
      fallback_sum_selection.exact_fallback_selected &&
      fallback_count_selection.accepted &&
      fallback_count_selection.selected &&
      fallback_count_selection.exact_fallback_selected &&
      fallback_raw.result.output.selected_alternative_uuid ==
          fallback_raw_selection.selected_candidate.alternative_uuid &&
      fallback_raw.result.output.capability_uuid ==
          fallback_raw_selection.selected_candidate.capability_uuid &&
      fallback_raw.result.output.provider_uuid ==
          fallback_raw_selection.selected_candidate.provider_uuid &&
      fallback_sum.result.output.selected_alternative_uuid ==
          fallback_sum_selection.selected_candidate.alternative_uuid &&
      fallback_sum.result.output.capability_uuid ==
          fallback_sum_selection.selected_candidate.capability_uuid &&
      fallback_sum.result.output.provider_uuid ==
          fallback_sum_selection.selected_candidate.provider_uuid &&
      fallback_count.result.output.selected_alternative_uuid ==
          fallback_count_selection.selected_candidate.alternative_uuid &&
      fallback_count.result.output.capability_uuid ==
          fallback_count_selection.selected_candidate.capability_uuid &&
      fallback_count.result.output.provider_uuid ==
          fallback_count_selection.selected_candidate.provider_uuid &&
      fallback_raw.result.output.selected_alternative_uuid !=
          success.result.output.selected_alternative_uuid &&
      fallback_raw.result.output.capability_uuid !=
          success.result.output.capability_uuid &&
      fallback_raw.result.output.provider_uuid !=
          success.result.output.provider_uuid &&
      fallback_sum.result.output.selected_alternative_uuid !=
          preferred_sum.result.output.selected_alternative_uuid &&
      fallback_sum.result.output.capability_uuid !=
          preferred_sum.result.output.capability_uuid &&
      fallback_sum.result.output.provider_uuid !=
          preferred_sum.result.output.provider_uuid &&
      fallback_count.result.output.selected_alternative_uuid !=
          preferred_count.result.output.selected_alternative_uuid &&
      fallback_count.result.output.capability_uuid !=
          preferred_count.result.output.capability_uuid &&
      fallback_count.result.output.provider_uuid !=
          preferred_count.result.output.provider_uuid;
  passed &= Require(
      preferred_sum.result.accepted && preferred_count.result.accepted &&
          exact_fallback_receipt(fallback_raw) &&
          exact_fallback_receipt(fallback_sum) &&
          exact_fallback_receipt(fallback_count) &&
          distinct_fallback_identities &&
          ExactExchangeStream(fallback_raw.result) ==
              ExactExchangeStream(success.result) &&
          ExactExchangeStream(fallback_sum.result) ==
              ExactExchangeStream(preferred_sum.result) &&
          ExactExchangeStream(fallback_count.result) ==
              ExactExchangeStream(preferred_count.result) &&
          Digest(fallback_raw.provider_stream) ==
              "bd587d51678f2da5ae92b9a57a0f646bdff6fc750b06d6159ee6179616d0696c" &&
          Digest(fallback_sum.provider_stream) ==
              "8b5d48cff675200e1abe0e83f9bbb1656a59020de9f05497d85b18571bb66191" &&
          Digest(fallback_count.provider_stream) ==
              "84d41074a7c4b7fc9b2c09ef32df0b879b24cade6687800adc224d999fde0fce" &&
          exact_fallback_receipt(fallback_raw_replay) &&
          exact_fallback_receipt(fallback_sum_replay) &&
          exact_fallback_receipt(fallback_count_replay) &&
          ExactExchangeStream(fallback_raw_replay.result) ==
              ExactExchangeStream(fallback_raw.result) &&
          ExactExchangeStream(fallback_sum_replay.result) ==
              ExactExchangeStream(fallback_sum.result) &&
          ExactExchangeStream(fallback_count_replay.result) ==
              ExactExchangeStream(fallback_count.result),
      "TS-32 exact raw reconstruction fallback exchange drifted");
  completed->insert("TS-32");
  auto stale_rollup_request = sum_request;
  stale_rollup_request.rollup_candidate_selected = true;
  stale_rollup_request.rollup_generation = 7;
  stale_rollup_request.visible_late_arrival_generation = 8;
  const auto stale_rollup = ExecuteThroughCommonSpine(
      stale_rollup_request, base, true, false, false, false, false,
      &fallback_sum_selection);
  passed &= Require(
      exact_fallback_receipt(stale_rollup) &&
          ExactExchangeStream(stale_rollup.result) ==
              ExactExchangeStream(preferred_sum.result) &&
          Digest(stale_rollup.provider_stream) ==
              "8b5d48cff675200e1abe0e83f9bbb1656a59020de9f05497d85b18571bb66191" &&
          stale_rollup.provider_stream.find("\t3\t4020000000000000\n") !=
              std::string::npos,
      "TS-34 stale rollup did not use the exact raw reconstruction fallback");
  completed->insert("TS-34");
  auto unproved_rollup_request = sum_request;
  unproved_rollup_request.rollup_candidate_selected = true;
  unproved_rollup_request.rollup_generation = 8;
  unproved_rollup_request.visible_late_arrival_generation = 8;
  const auto unproved_rollup =
      ExecuteThroughCommonSpine(unproved_rollup_request, base);
  passed &= Require(
      !unproved_rollup.result.accepted &&
          unproved_rollup.result.execution_started &&
          !unproved_rollup.result.data_access_observed &&
          !unproved_rollup.result.root_published &&
          unproved_rollup.result.diagnostic_id ==
              "SB_MODEL_TIME_SERIES_ROLLUP_EQUIVALENCE_REFUSED_V1" &&
          unproved_rollup.preferred_access_invocation_count == 0 &&
          unproved_rollup.exact_fallback_access_invocation_count == 0 &&
          unproved_rollup.provider_calls == 1 &&
          unproved_rollup.cleanup_calls == 1,
      "unproved equal-generation rollup was published or accessed");
  const auto substituted =
      ExecuteThroughCommonSpine(raw_request, base, true, false, false, true);
  const auto raw_order_substituted = ExecuteThroughCommonSpine(
      raw_request, base, true, false, false, false, false, nullptr,
      TimeSeriesExchangeMutation::kOrder);
  const auto downsample_order_substituted = ExecuteThroughCommonSpine(
      sum_request, base, true, false, false, false, false, nullptr,
      TimeSeriesExchangeMutation::kOrder);
  const auto duplicate_row_uuid = ExecuteThroughCommonSpine(
      raw_request, base, true, false, false, false, false, nullptr,
      TimeSeriesExchangeMutation::kDuplicateRowUuid);
  const auto noncanonical_tags = ExecuteThroughCommonSpine(
      raw_request, base, true, false, false, false, false, nullptr,
      TimeSeriesExchangeMutation::kNoncanonicalTags);
  const auto noncanonical_raw_numeric = ExecuteThroughCommonSpine(
      raw_request, base, true, false, false, false, false, nullptr,
      TimeSeriesExchangeMutation::kNoncanonicalNumeric);
  const auto noncanonical_downsample_numeric = ExecuteThroughCommonSpine(
      sum_request, base, true, false, false, false, false, nullptr,
      TimeSeriesExchangeMutation::kNoncanonicalNumeric);
  const auto noncanonical_sample_count = ExecuteThroughCommonSpine(
      sum_request, base, true, false, false, false, false, nullptr,
      TimeSeriesExchangeMutation::kNoncanonicalSampleCount);
  const auto substituted_raw_value = ExecuteThroughCommonSpine(
      raw_request, base, true, false, false, false, false, nullptr,
      TimeSeriesExchangeMutation::kSubstituteRawValue);
  const auto substituted_sample_count = ExecuteThroughCommonSpine(
      sum_request, base, true, false, false, false, false, nullptr,
      TimeSeriesExchangeMutation::kSubstituteSampleCount);
  const auto substituted_aggregate_value = ExecuteThroughCommonSpine(
      sum_request, base, true, false, false, false, false, nullptr,
      TimeSeriesExchangeMutation::kSubstituteAggregateValue);
  const auto exchange_memory_refused = ExecuteThroughCommonSpine(
      raw_request, base, true, false, false, false, false, nullptr,
      TimeSeriesExchangeMutation::kNone, 0, 1);
  const auto exact_exchange_refusal = [](const ExchangeRun& run) {
    return !run.result.accepted && !run.result.root_published &&
           run.result.diagnostic_id == "SB_MODEL_TYPED_EXCHANGE_INVALID_V1" &&
           run.provider_calls == 1 && run.cleanup_calls == 1 &&
           run.result.cleanup_count == 1;
  };
  passed &= Require(!substituted.result.accepted &&
                        !substituted.result.root_published &&
                        substituted.result.diagnostic_id ==
                            "SB_MODEL_TYPED_EXCHANGE_INVALID_V1" &&
                        substituted.cleanup_calls == 1 &&
                        !raw_order_substituted.result.accepted &&
                        !raw_order_substituted.result.root_published &&
                        raw_order_substituted.result.diagnostic_id ==
                            "SB_MODEL_TYPED_EXCHANGE_INVALID_V1" &&
                        raw_order_substituted.cleanup_calls == 1 &&
                        !downsample_order_substituted.result.accepted &&
                        !downsample_order_substituted.result.root_published &&
                        downsample_order_substituted.result.diagnostic_id ==
                            "SB_MODEL_TYPED_EXCHANGE_INVALID_V1" &&
                        downsample_order_substituted.cleanup_calls == 1 &&
                        exact_exchange_refusal(duplicate_row_uuid) &&
                        exact_exchange_refusal(noncanonical_tags) &&
                        exact_exchange_refusal(noncanonical_raw_numeric) &&
                        exact_exchange_refusal(
                            noncanonical_downsample_numeric) &&
                        exact_exchange_refusal(noncanonical_sample_count) &&
                        exact_exchange_refusal(substituted_raw_value) &&
                        exact_exchange_refusal(substituted_sample_count) &&
                        exact_exchange_refusal(
                            substituted_aggregate_value) &&
                        !exchange_memory_refused.result.accepted &&
                        !exchange_memory_refused.result.root_published &&
                        exchange_memory_refused.result.diagnostic_id ==
                            "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1" &&
                        exchange_memory_refused.result.data_access_observed &&
                        exchange_memory_refused.provider_calls == 1 &&
                        exchange_memory_refused.cleanup_calls == 1 &&
                        exchange_memory_refused.result.cleanup_count == 1,
                    "TS-27 typed exchange substitution was published");
  completed->insert("TS-27");
  const auto stale =
      ExecuteThroughCommonSpine(raw_request, base, true, true);
  passed &= Require(!stale.result.accepted &&
                        !stale.result.execution_started &&
                        stale.result.diagnostic_id ==
                            "SB_MODEL_PROVIDER_GENERATION_STALE_V1" &&
                        stale.provider_calls == 0 && stale.cleanup_calls == 0,
                    "TS-26 stale provider generation was accessed");
  const auto mga =
      ExecuteThroughCommonSpine(raw_request, base, true, false, true);
  passed &= Require(!mga.result.accepted && !mga.result.execution_started &&
                        mga.result.diagnostic_id ==
                            "SB_MODEL_MGA_CONTEXT_MISMATCH_V1" &&
                        mga.provider_calls == 0 && mga.cleanup_calls == 0,
                    "TS-29 substituted MGA context was accessed");
  completed->insert("TS-29");
  auto fallback_request = raw_request;
  fallback_request.exact_fallback_selected = true;
  const auto unavailable =
      ExecuteThroughCommonSpine(fallback_request, base, false);
  passed &= Require(!unavailable.result.accepted &&
                        !unavailable.result.execution_started &&
                        unavailable.result.diagnostic_id ==
                            "SB_MODEL_TIME_SERIES_EXACT_FALLBACK_UNAVAILABLE_V1" &&
                        unavailable.provider_calls == 0 &&
                        unavailable.cleanup_calls == 0,
                    "TS-33 unavailable fallback was accessed");
  const auto failed = ExecuteThroughCommonSpine(raw_request, base, true, false,
                                                 false, false, true);
  passed &= Require(!failed.result.accepted && failed.result.execution_started &&
                        failed.result.data_access_observed &&
                        !failed.result.root_published &&
                        failed.result.diagnostic_id ==
                            "SB_MODEL_COORDINATOR_LEG_FAILED_V1" &&
                        failed.result.cleanup_count == 1 &&
                        failed.provider_calls == 1 && failed.cleanup_calls == 1,
                    "TS-40 post-acquisition provider-leg failure cleanup drifted");
  completed->insert("TS-40");
  return passed;
}

bool FrontdoorMatrix(const api::EngineRequestContext& context,
                     std::set<std::string>* completed) {
  const auto has_diagnostic = [](const auto& ast,
                                 const std::string_view diagnostic) {
    return std::ranges::any_of(
        ast.native_relational.messages.diagnostics,
        [&](const auto& entry) { return entry.code == diagnostic; });
  };
  const auto append = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM TIME_SERIES_APPEND(app.series_fixture);"));
  const auto donor = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM INFLUX_LINE_PROTOCOL('m,t=v f=1 0');"));
  const auto wrong_alias = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM TIME_SERIES_SOURCE(app.series_fixture) AS ts WHERE "
      "TIME_RANGE(other, TIMESTAMP '2026-08-10T12:00:00Z', "
      "TIMESTAMP '2026-08-10T12:02:00Z');"));
  const auto missing_range = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM TIME_SERIES_SOURCE(app.series_fixture) AS ts;"));
  const auto duplicate_range = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM TIME_SERIES_SOURCE(app.series_fixture) AS ts WHERE "
      "TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
      "TIMESTAMP '2026-08-10T12:02:00Z') AND "
      "TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
      "TIMESTAMP '2026-08-10T12:02:00Z');"));
  const auto null_range = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM TIME_SERIES_SOURCE(app.series_fixture) AS ts WHERE "
      "TIME_RANGE(ts, NULL, TIMESTAMP '2026-08-10T12:02:00Z');"));
  const auto unsupported_aggregate = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT TIME_DOWNSAMPLE(MEDIAN, INTERVAL 'PT60S', ts.value) FROM "
      "TIME_SERIES_SOURCE(app.series_fixture) AS ts WHERE "
      "TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
      "TIMESTAMP '2026-08-10T12:02:00Z');"));
  const auto lowercase_aggregate = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT TIME_DOWNSAMPLE(count, INTERVAL 'PT60S', ts.value) FROM "
      "TIME_SERIES_SOURCE(app.series_fixture) AS ts WHERE "
      "TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
      "TIMESTAMP '2026-08-10T12:02:00Z');"));
  const auto distinct_aggregate = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT TIME_DOWNSAMPLE(SUM, INTERVAL 'PT60S', ts.value) DISTINCT "
      "FROM TIME_SERIES_SOURCE(app.series_fixture) AS ts WHERE "
      "TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
      "TIMESTAMP '2026-08-10T12:02:00Z');"));
  const auto filtered_aggregate = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT TIME_DOWNSAMPLE(SUM, INTERVAL 'PT60S', ts.value) FILTER "
      "(WHERE ts.value > 0) FROM "
      "TIME_SERIES_SOURCE(app.series_fixture) AS ts WHERE "
      "TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
      "TIMESTAMP '2026-08-10T12:02:00Z');"));
  const auto wrong_value_aggregate = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT TIME_DOWNSAMPLE(SUM, INTERVAL 'PT60S', ts.tags) FROM "
      "TIME_SERIES_SOURCE(app.series_fixture) AS ts WHERE "
      "TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
      "TIMESTAMP '2026-08-10T12:02:00Z');"));
  const bool passed = Require(
      append.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          !append.produces_sblr &&
          has_diagnostic(append,
                         "SB_MODEL_TIME_SERIES_APPEND_NOT_QUERY_SOURCE_V1") &&
          donor.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          !donor.produces_sblr &&
          has_diagnostic(donor, "SB_MODEL_GRAMMAR_DONOR_TEXT_REFUSED_V1") &&
          wrong_alias.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          has_diagnostic(wrong_alias,
                         "SB_MODEL_TIME_SERIES_RANGE_INVALID_V1") &&
          missing_range.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          has_diagnostic(missing_range,
                         "SB_MODEL_TIME_SERIES_RANGE_INVALID_V1") &&
          duplicate_range.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          has_diagnostic(duplicate_range,
                         "SB_MODEL_TIME_SERIES_RANGE_INVALID_V1") &&
          null_range.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          has_diagnostic(null_range,
                         "SB_MODEL_TIME_SERIES_RANGE_INVALID_V1") &&
          unsupported_aggregate.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          has_diagnostic(unsupported_aggregate,
                         "SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1") &&
          lowercase_aggregate.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          !lowercase_aggregate.produces_sblr &&
          has_diagnostic(lowercase_aggregate,
                         "SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1") &&
          distinct_aggregate.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          has_diagnostic(distinct_aggregate,
                         "SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1") &&
          filtered_aggregate.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          has_diagnostic(filtered_aggregate,
                         "SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1") &&
          wrong_value_aggregate.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kRefused &&
          has_diagnostic(wrong_value_aggregate,
                         "SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1"),
      "TS-19/20/22/23/24 frontdoor refusal identity drifted");
  plan::CanonicalLogicalRelationalGraph substituted_graph;
  substituted_graph.bound_sblr_tree_uuid =
      NewUuidText(platform::UuidKind::object);
  substituted_graph.catalog_epoch_uuid = context.catalog_epoch_uuid.canonical;
  substituted_graph.security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  substituted_graph.local_transaction_id = context.local_transaction_id;
  substituted_graph.statement_snapshot_id =
      context.snapshot_visible_through_local_transaction_id;
  substituted_graph.mga_statement_context = LogicalMga(context);
  substituted_graph.root_logical_node_id = 1;
  substituted_graph.result_descriptor_ids = {201, 202, 203, 204, 205, 206,
                                               207};
  plan::CanonicalLogicalRelationalNode substituted_aggregate;
  substituted_aggregate.logical_node_id = 1;
  substituted_aggregate.node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kAggregate;
  substituted_aggregate.output_descriptor_ids =
      substituted_graph.result_descriptor_ids;
  substituted_aggregate.bound_expression_ids = {1, 2, 3, 4, 5,
                                                 6, 7, 8, 9};
  substituted_aggregate.origin_relational_node_ids = {1};
  substituted_aggregate.required_object_uuids = {
      std::string(kBaseObjectUuid)};
  substituted_aggregate.semantic_variant_id = "SBLR_MODEL_AGGREGATE_V1";
  substituted_aggregate.model_family_identity =
      plan::CanonicalLogicalModelFamilyIdentity::kDocument;
  substituted_graph.nodes.push_back(std::move(substituted_aggregate));
  const auto substituted_admission =
      plan::ValidateCanonicalLogicalRelationalGraph(substituted_graph);
  const bool logical_family_refused =
      !substituted_admission.accepted &&
      !substituted_admission.issues.empty() &&
      substituted_admission.issues.front().field_id ==
          "model_semantic_node_shape";
  completed->insert("TS-23");
  completed->insert("TS-24");
  completed->insert("TS-19");
  completed->insert("TS-20");
  completed->insert("TS-22");
  return passed &&
         Require(logical_family_refused,
                 "cross-family SBLR_MODEL_AGGREGATE_V1 substitution was admitted");
}

api::RelationalTypeDescriptor DagDescriptor(
    const std::uint32_t descriptor_id, const std::string& descriptor_uuid,
    const std::string_view core_type, const bool timestamp = false) {
  api::RelationalTypeDescriptor descriptor;
  descriptor.descriptor_id = descriptor_id;
  descriptor.descriptor_uuid = descriptor_uuid;
  descriptor.type_uuid = CoreTypeUuid(core_type);
  descriptor.nullability = api::RelationalNullability::kNonNull;
  if (timestamp) descriptor.timezone_profile_id = "UTC";
  return descriptor;
}

api::TypedRelationalDag TimeSeriesDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& storage,
    const api::EngineBoundTimeSeriesAggregateV1 aggregate) {
  const bool downsample =
      aggregate != api::EngineBoundTimeSeriesAggregateV1::kNone;
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

  if (!downsample) {
    dag.descriptors = {
        DagDescriptor(101, storage.descriptor_uuid.canonical, "uuid"),
        DagDescriptor(102, storage.schema_uuid.canonical, "uuid"),
        DagDescriptor(103,
                      storage.columns[0].value_descriptor.descriptor_uuid.canonical,
                      "uuid"),
        DagDescriptor(104,
                      storage.columns[1].value_descriptor.descriptor_uuid.canonical,
                      "timestamp", true),
        DagDescriptor(105,
                      storage.columns[2].value_descriptor.descriptor_uuid.canonical,
                      "character"),
        DagDescriptor(106,
                      storage.columns[3].value_descriptor.descriptor_uuid.canonical,
                      "real64"),
        DagDescriptor(107, CoreTypeUuid("boolean"), "boolean"),
    };
  } else {
    dag.descriptors = {
        DagDescriptor(200, storage.descriptor_uuid.canonical, "uuid"),
        DagDescriptor(201, storage.schema_uuid.canonical, "uuid"),
        DagDescriptor(202,
                      storage.columns[0].value_descriptor.descriptor_uuid.canonical,
                      "uuid"),
        DagDescriptor(203,
                      storage.columns[1].value_descriptor.descriptor_uuid.canonical,
                      "timestamp", true),
        DagDescriptor(204, storage.columns[1].column_uuid.canonical,
                      "timestamp", true),
        DagDescriptor(205,
                      storage.columns[2].value_descriptor.descriptor_uuid.canonical,
                      "character"),
        DagDescriptor(206, CoreTypeUuid("int64"), "int64"),
        DagDescriptor(208, CoreTypeUuid("interval"), "interval"),
        DagDescriptor(209, CoreTypeUuid("boolean"), "boolean"),
        DagDescriptor(212,
                      storage.columns[3].value_descriptor.descriptor_uuid.canonical,
                      "real64"),
    };
    if (aggregate == api::EngineBoundTimeSeriesAggregateV1::kCount) {
      dag.descriptors.push_back(
          DagDescriptor(207, storage.columns[3].column_uuid.canonical,
                        "int64"));
    }
  }

  static constexpr std::array<std::string_view, 6> kRawNames{
      "row_uuid", "series_uuid", "metric_uuid", "point_timestamp", "tags",
      "value"};
  static constexpr std::array<std::string_view, 7> kAggregateNames{
      "series_uuid", "metric_uuid", "bucket_start", "bucket_end", "tags",
      "sample_count", "aggregate_value"};
  const auto width = downsample ? kAggregateNames.size() : kRawNames.size();
  std::vector<std::uint32_t> output_descriptors;
  if (!downsample) {
    output_descriptors = {101, 102, 103, 104, 105, 106};
  } else {
    output_descriptors = {
        201, 202, 203, 204, 205, 206,
        aggregate == api::EngineBoundTimeSeriesAggregateV1::kCount ? 207u
                                                                   : 212u};
  }
  for (std::size_t ordinal = 0; ordinal < width; ++ordinal) {
    api::RelationalExpressionRecord expression;
    expression.expression_id = static_cast<std::uint32_t>(ordinal + 1);
    expression.expression_kind = api::RelationalExpressionKind::kIdentifier;
    expression.result_descriptor_id = output_descriptors[ordinal];
    if (!downsample) {
      expression.bound_name_uuid =
          ordinal < 2
              ? (ordinal == 0 ? storage.descriptor_uuid.canonical
                              : storage.schema_uuid.canonical)
              : storage.columns[ordinal - 2].column_uuid.canonical;
    } else {
      expression.bound_name_uuid =
          ordinal == 0
              ? storage.schema_uuid.canonical
              : ordinal == 1
                    ? storage.columns[0].column_uuid.canonical
                    : ordinal == 2 || ordinal == 3
                          ? storage.columns[1].column_uuid.canonical
                          : ordinal == 4
                                ? storage.columns[2].column_uuid.canonical
                                : ordinal == 5
                                      ? storage.relation_uuid.canonical
                                      : aggregate ==
                                                api::EngineBoundTimeSeriesAggregateV1::kCount
                                            ? storage.relation_uuid.canonical
                                            : storage.columns[3].column_uuid.canonical;
    }
    dag.expressions.push_back(std::move(expression));
    dag.outputs.push_back(
        {static_cast<std::uint32_t>(ordinal + 1), 1,
         static_cast<std::uint32_t>(ordinal + 1),
         std::string(downsample ? kAggregateNames[ordinal] : kRawNames[ordinal]),
         output_descriptors[ordinal], true, static_cast<std::uint32_t>(ordinal)});
  }

  api::RelationalExpressionRecord alias;
  alias.expression_id = 20;
  alias.expression_kind = api::RelationalExpressionKind::kIdentifier;
  alias.result_descriptor_id = downsample ? 201 : 102;
  alias.bound_name_uuid = storage.relation_uuid.canonical;
  dag.expressions.push_back(std::move(alias));
  for (const auto [id, encoded] :
       {std::pair<std::uint32_t, std::string_view>{21, kRangeStart},
        {22, kRangeEnd}}) {
    api::RelationalExpressionRecord endpoint;
    endpoint.expression_id = id;
    endpoint.expression_kind = api::RelationalExpressionKind::kLiteral;
    endpoint.result_descriptor_id = downsample ? 203 : 104;
    endpoint.literal_kind = api::RelationalLiteralKind::kTemporal;
    endpoint.literal_or_parameter_ref = std::string(encoded);
    dag.expressions.push_back(std::move(endpoint));
  }
  api::RelationalExpressionRecord range;
  range.expression_id = 23;
  range.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  range.child_expression_ids = {20, 21, 22};
  range.result_descriptor_id = downsample ? 209 : 107;
  range.operator_name = "TIME_RANGE";
  dag.expressions.push_back(std::move(range));

  if (downsample) {
    std::string aggregate_id;
    switch (aggregate) {
      case api::EngineBoundTimeSeriesAggregateV1::kCount:
        aggregate_id = "COUNT";
        break;
      case api::EngineBoundTimeSeriesAggregateV1::kSum:
        aggregate_id = "SUM";
        break;
      case api::EngineBoundTimeSeriesAggregateV1::kMin:
        aggregate_id = "MIN";
        break;
      case api::EngineBoundTimeSeriesAggregateV1::kMax:
        aggregate_id = "MAX";
        break;
      case api::EngineBoundTimeSeriesAggregateV1::kAvg:
        aggregate_id = "AVG";
        break;
      case api::EngineBoundTimeSeriesAggregateV1::kNone:
        break;
    }
    api::RelationalExpressionRecord aggregate_literal;
    aggregate_literal.expression_id = 24;
    aggregate_literal.expression_kind = api::RelationalExpressionKind::kLiteral;
    aggregate_literal.result_descriptor_id = 205;
    aggregate_literal.literal_kind = api::RelationalLiteralKind::kString;
    aggregate_literal.literal_or_parameter_ref = aggregate_id;
    dag.expressions.push_back(std::move(aggregate_literal));
    api::RelationalExpressionRecord interval;
    interval.expression_id = 25;
    interval.expression_kind = api::RelationalExpressionKind::kLiteral;
    interval.result_descriptor_id = 208;
    interval.literal_kind = api::RelationalLiteralKind::kTemporal;
    interval.literal_or_parameter_ref = "PT60S";
    dag.expressions.push_back(std::move(interval));
    api::RelationalExpressionRecord value;
    value.expression_id = 26;
    value.expression_kind = api::RelationalExpressionKind::kIdentifier;
    value.result_descriptor_id = 212;
    value.bound_name_uuid = storage.columns[3].column_uuid.canonical;
    dag.expressions.push_back(std::move(value));
    api::RelationalExpressionRecord operation;
    operation.expression_id = 27;
    operation.expression_kind = api::RelationalExpressionKind::kFunctionCall;
    operation.child_expression_ids = {24, 25, 26};
    operation.result_descriptor_id = output_descriptors.back();
    operation.operator_name = "TIME_DOWNSAMPLE";
    dag.expressions.push_back(std::move(operation));
  }

  api::RelationalDagNode source;
  source.node_id = 1;
  source.node_kind = downsample ? api::RelationalDagNodeKind::kAggregate
                                : api::RelationalDagNodeKind::kScan;
  source.output_descriptor_ids = output_descriptors;
  for (std::uint32_t id = 1; id <= width; ++id) {
    source.bound_expression_ids.push_back(id);
  }
  if (downsample) source.bound_expression_ids.push_back(27);
  source.bound_expression_ids.push_back(23);
  source.required_object_uuids = {storage.relation_uuid.canonical};
  source.semantic_variant_id = downsample ? "SBLR_MODEL_AGGREGATE_V1"
                                          : "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(std::move(source));
  return dag;
}

api::TypedRelationalDag TimeSeriesBucketDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& storage,
    const std::string_view range_start = kRangeStart,
    const std::string_view range_end = kRangeEnd,
    const std::string_view bucket_interval = "PT60S") {
  auto dag = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kNone);
  std::erase_if(dag.expressions, [](const auto& expression) {
    return expression.expression_id == 1;
  });
  const auto start = std::ranges::find_if(dag.expressions, [](const auto& item) {
    return item.expression_id == 21;
  });
  const auto end = std::ranges::find_if(dag.expressions, [](const auto& item) {
    return item.expression_id == 22;
  });
  if (start != dag.expressions.end()) {
    start->literal_or_parameter_ref = std::string(range_start);
  }
  if (end != dag.expressions.end()) {
    end->literal_or_parameter_ref = std::string(range_end);
  }
  dag.descriptors.push_back(
      DagDescriptor(110, CoreTypeUuid("interval"), "interval"));
  api::RelationalExpressionRecord interval;
  interval.expression_id = 29;
  interval.expression_kind = api::RelationalExpressionKind::kLiteral;
  interval.result_descriptor_id = 110;
  interval.literal_kind = api::RelationalLiteralKind::kTemporal;
  interval.literal_or_parameter_ref = std::string(bucket_interval);
  dag.expressions.push_back(std::move(interval));
  api::RelationalExpressionRecord bucket;
  bucket.expression_id = 30;
  bucket.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  bucket.child_expression_ids = {29, 4};
  bucket.result_descriptor_id = 104;
  bucket.operator_name = "TIME_BUCKET";
  dag.expressions.push_back(std::move(bucket));
  dag.nodes.front().output_descriptor_ids = {104};
  dag.nodes.front().bound_expression_ids = {30, 2, 3, 5, 6, 23};
  dag.outputs.clear();
  dag.outputs.push_back({1, 1, 30, "bucket_start", 104, true, 0});
  return dag;
}

api::TypedRelationalDag TimeSeriesBucketDownsampleDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& storage,
    const std::string_view bucket_interval = "PT1M") {
  auto dag = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kSum);
  api::RelationalExpressionRecord interval;
  interval.expression_id = 28;
  interval.expression_kind = api::RelationalExpressionKind::kLiteral;
  interval.result_descriptor_id = 208;
  interval.literal_kind = api::RelationalLiteralKind::kTemporal;
  interval.literal_or_parameter_ref = std::string(bucket_interval);
  dag.expressions.push_back(std::move(interval));
  api::RelationalExpressionRecord timestamp;
  timestamp.expression_id = 29;
  timestamp.expression_kind = api::RelationalExpressionKind::kIdentifier;
  timestamp.result_descriptor_id = 203;
  timestamp.bound_name_uuid = storage.columns[1].column_uuid.canonical;
  dag.expressions.push_back(std::move(timestamp));
  api::RelationalExpressionRecord bucket;
  bucket.expression_id = 30;
  bucket.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  bucket.child_expression_ids = {28, 29};
  bucket.result_descriptor_id = 203;
  bucket.operator_name = "TIME_BUCKET";
  dag.expressions.push_back(std::move(bucket));
  dag.nodes.front().bound_expression_ids = {1, 2, 3, 4, 5, 6, 7,
                                            30, 27, 23};
  return dag;
}

api::TypedRelationalDag TimeSeriesFilterProjectDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& storage) {
  auto dag = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kNone);
  dag.root_node_id = 3;
  api::RelationalExpressionRecord predicate;
  predicate.expression_id = 40;
  predicate.expression_kind = api::RelationalExpressionKind::kLiteral;
  predicate.result_descriptor_id = 107;
  predicate.literal_kind = api::RelationalLiteralKind::kBoolean;
  predicate.literal_or_parameter_ref = "TRUE";
  dag.expressions.push_back(std::move(predicate));
  api::RelationalDagNode filter;
  filter.node_id = 2;
  filter.node_kind = api::RelationalDagNodeKind::kFilter;
  filter.input_node_ids = {1};
  filter.output_descriptor_ids = {101, 102, 103, 104, 105, 106};
  filter.bound_expression_ids = {40};
  filter.semantic_variant_id = "filter.where.v1";
  dag.nodes.push_back(std::move(filter));
  api::RelationalDagNode project;
  project.node_id = 3;
  project.node_kind = api::RelationalDagNodeKind::kProject;
  project.input_node_ids = {2};
  project.output_descriptor_ids = {101, 102, 103, 104, 105, 106};
  project.bound_expression_ids = {1, 2, 3, 4, 5, 6};
  project.semantic_variant_id = "project.select-list.v1";
  dag.nodes.push_back(std::move(project));
  static constexpr std::array<std::string_view, 6> kNames{
      "row_uuid", "series_uuid", "metric_uuid", "point_timestamp", "tags",
      "value"};
  for (std::size_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
    dag.outputs.push_back(
        {static_cast<std::uint32_t>(30 + ordinal), 3,
         static_cast<std::uint32_t>(ordinal + 1), std::string(kNames[ordinal]),
         static_cast<std::uint32_t>(101 + ordinal), true,
         static_cast<std::uint32_t>(ordinal)});
  }
  return dag;
}

api::TypedRelationalDag TimeSeriesUnaryCompositionDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& storage) {
  auto dag = TimeSeriesFilterProjectDag(context, storage);
  dag.root_node_id = 5;

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
  sort.output_descriptor_ids = {101, 102, 103, 104, 105, 106};
  sort.bound_expression_ids = {1};
  sort.semantic_variant_id = "sort.required-order.v1";
  sort.required_property_uuids = {ordering_uuid};
  sort.delivered_property_uuids = {ordering_uuid};
  dag.nodes.push_back(std::move(sort));

  dag.descriptors.push_back(DagDescriptor(
      110, NewUuidText(platform::UuidKind::object), "int64"));
  constexpr std::string_view kRowNumberFunctionUuid =
      "019de5fc-2400-7539-bcce-00eef3ae7220";
  api::RelationalExpressionRecord row_number;
  row_number.expression_id = 41;
  row_number.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  row_number.result_descriptor_id = 110;
  row_number.function_uuid = std::string(kRowNumberFunctionUuid);
  dag.expressions.push_back(std::move(row_number));
  static constexpr std::array<std::string_view, 6> kNames{
      "row_uuid", "series_uuid", "metric_uuid", "point_timestamp", "tags",
      "value"};
  for (std::size_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
    dag.outputs.push_back(
        {static_cast<std::uint32_t>(40 + ordinal), 5,
         static_cast<std::uint32_t>(ordinal + 1), std::string(kNames[ordinal]),
         static_cast<std::uint32_t>(101 + ordinal), true,
         static_cast<std::uint32_t>(ordinal)});
  }
  dag.outputs.push_back({46, 5, 41, "row_number", 110, true, 6});
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
  window.output_descriptor_ids = {101, 102, 103, 104, 105, 106, 110};
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
  invocation.result_descriptor_id = 110;
  invocation.output_name_utf8 = "row_number";
  dag.window_invocations.push_back(std::move(invocation));

  return dag;
}

api::TypedRelationalDag TimeSeriesCteLimitDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& storage) {
  auto dag = TimeSeriesFilterProjectDag(context, storage);
  dag.root_node_id = 5;

  api::RelationalDagNode cte;
  cte.node_id = 4;
  cte.node_kind = api::RelationalDagNodeKind::kCte;
  cte.input_node_ids = {3};
  cte.output_descriptor_ids = {101, 102, 103, 104, 105, 106};
  cte.shareable = true;
  cte.semantic_variant_id = "cte.bound.v1";
  dag.nodes.push_back(std::move(cte));
  dag.descriptors.push_back(DagDescriptor(
      110, NewUuidText(platform::UuidKind::object), "int64"));
  api::RelationalExpressionRecord limit;
  limit.expression_id = 41;
  limit.expression_kind = api::RelationalExpressionKind::kLiteral;
  limit.result_descriptor_id = 110;
  limit.literal_kind = api::RelationalLiteralKind::kNumeric;
  limit.literal_or_parameter_ref = "3";
  dag.expressions.push_back(std::move(limit));
  api::RelationalDagNode limit_node;
  limit_node.node_id = 5;
  limit_node.node_kind = api::RelationalDagNodeKind::kLimit;
  limit_node.input_node_ids = {4};
  limit_node.output_descriptor_ids = {101, 102, 103, 104, 105, 106};
  limit_node.bound_expression_ids = {41};
  limit_node.semantic_variant_id = "limit.bound-count.v1";
  dag.nodes.push_back(std::move(limit_node));
  return dag;
}

api::TypedRelationalDag TimeSeriesCountDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& storage) {
  auto dag = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kNone);
  dag.root_node_id = 2;
  dag.descriptors.push_back(DagDescriptor(
      110, NewUuidText(platform::UuidKind::object), "int64"));
  const auto count = exec::LookupCanonicalAggregateByFunctionV1(
      exec::CanonicalAggregateFunction::count);
  api::RelationalExpressionRecord aggregate;
  aggregate.expression_id = 40;
  aggregate.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  aggregate.result_descriptor_id = 110;
  if (count != nullptr) aggregate.function_uuid = count->function_uuid;
  dag.expressions.push_back(std::move(aggregate));
  dag.outputs.push_back({40, 2, 40, "point_count", 110, true, 0});
  api::RelationalDagNode aggregate_node;
  aggregate_node.node_id = 2;
  aggregate_node.node_kind = api::RelationalDagNodeKind::kAggregate;
  aggregate_node.input_node_ids = {1};
  aggregate_node.output_descriptor_ids = {110};
  aggregate_node.bound_expression_ids = {40};
  aggregate_node.semantic_variant_id = "aggregate.global-count-star.v1";
  dag.nodes.push_back(std::move(aggregate_node));
  return dag;
}

api::TypedRelationalDag TimeSeriesRecursiveDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& storage) {
  auto dag = TimeSeriesCountDag(context, storage);
  dag.root_node_id = 4;
  api::RelationalExpressionRecord bound;
  bound.expression_id = 41;
  bound.expression_kind = api::RelationalExpressionKind::kLiteral;
  bound.result_descriptor_id = 110;
  bound.literal_kind = api::RelationalLiteralKind::kNumeric;
  bound.literal_or_parameter_ref = "9";
  dag.expressions.push_back(std::move(bound));
  api::RelationalDagNode term;
  term.node_id = 3;
  term.node_kind = api::RelationalDagNodeKind::kCte;
  term.output_descriptor_ids = {110};
  term.semantic_variant_id = "cte.recursive-term-int64-increment.v1";
  dag.nodes.push_back(std::move(term));
  api::RelationalDagNode recursive;
  recursive.node_id = 4;
  recursive.node_kind = api::RelationalDagNodeKind::kRecursiveCte;
  recursive.input_node_ids = {2, 3};
  recursive.output_descriptor_ids = {110};
  recursive.bound_expression_ids = {41};
  recursive.semantic_variant_id =
      "cte.recursive-union-all-int64-increment.v1";
  dag.nodes.push_back(std::move(recursive));
  return dag;
}

api::TypedRelationalDag TimeSeriesSetDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& storage) {
  auto dag = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kNone);
  dag.root_node_id = 4;
  dag.outputs.push_back({40, 2, 1, "row_uuid", 101, true, 0});
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
      "40000000-0000-4000-8000-000000000099";
  dag.expressions.push_back(std::move(literal));
  dag.values_rows.push_back({1, {40}});
  dag.outputs.push_back({41, 3, 40, "row_uuid", 101, true, 0});
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

api::TypedRelationalDag TimeSeriesMixedJoinDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& storage,
    const api::MgaRelationStorageDescriptor& heap_storage,
    std::string semantic_variant_id) {
  auto dag = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kNone);
  dag.root_node_id = 3;
  for (std::size_t ordinal = 0; ordinal < 2; ++ordinal) {
    const auto descriptor_id = static_cast<std::uint32_t>(201 + ordinal);
    const auto expression_id = static_cast<std::uint32_t>(50 + ordinal);
    const auto& column = heap_storage.columns[ordinal];
    api::RelationalTypeDescriptor type;
    type.descriptor_id = descriptor_id;
    type.descriptor_uuid =
        column.value_descriptor.descriptor_uuid.canonical;
    type.type_uuid = DescriptorField(
        column.value_descriptor.encoded_descriptor, "type_uuid");
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
  heap.required_object_uuids = {heap_storage.relation_uuid.canonical};
  heap.semantic_variant_id = "relation.source.v1";
  dag.nodes.push_back(std::move(heap));

  const bool cross = semantic_variant_id == "join.cross.v1";
  const bool left_only = semantic_variant_id == "join.left-semi.v1" ||
                         semantic_variant_id == "join.left-anti.v1";
  if (!cross) {
    api::RelationalExpressionRecord equality;
    equality.expression_id = 60;
    equality.expression_kind = api::RelationalExpressionKind::kBinary;
    equality.child_expression_ids = {1, 50};
    equality.result_descriptor_id = 107;
    equality.operator_name = "=";
    dag.expressions.push_back(std::move(equality));
  }
  const std::array<std::string_view, 8> names{
      "row_uuid", "series_uuid", "metric_uuid", "point_timestamp", "tags",
      "value", "join_uuid", "payload"};
  const std::array<std::uint32_t, 8> expressions{
      1, 2, 3, 4, 5, 6, 50, 51};
  const std::array<std::uint32_t, 8> descriptors{
      101, 102, 103, 104, 105, 106, 201, 202};
  const auto width = left_only ? 6U : 8U;
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

api::TypedRelationalDag TimeSeriesAsofJoinDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& storage,
    const api::MgaRelationStorageDescriptor& heap_storage,
    const api::EngineBoundTimeSeriesAggregateV1 aggregate,
    const bool time_series_left, const bool left_outer,
    const bool heap_is_columnar_model = false) {
  const bool downsample =
      aggregate != api::EngineBoundTimeSeriesAggregateV1::kNone;
  auto dag = TimeSeriesDag(context, storage, aggregate);
  dag.root_node_id = 3;
  for (std::size_t ordinal = 0; ordinal < heap_storage.columns.size();
       ++ordinal) {
    const auto descriptor_id = static_cast<std::uint32_t>(301 + ordinal);
    const auto expression_id = static_cast<std::uint32_t>(50 + ordinal);
    const auto& column = heap_storage.columns[ordinal];
    api::RelationalTypeDescriptor type;
    type.descriptor_id = descriptor_id;
    type.descriptor_uuid =
        column.value_descriptor.descriptor_uuid.canonical;
    type.type_uuid = DescriptorField(
        column.value_descriptor.encoded_descriptor, "type_uuid");
    type.nullability = column.nullable
                           ? api::RelationalNullability::kNullable
                           : api::RelationalNullability::kNonNull;
    if (!column.collation_uuid.empty()) {
      type.collation_uuid = column.collation_uuid;
    }
    const auto timezone_profile = DescriptorField(
        column.value_descriptor.encoded_descriptor, "timezone_profile_id");
    if (!timezone_profile.empty()) {
      type.timezone_profile_id = timezone_profile;
    }
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
  heap.output_descriptor_ids = {301, 302, 303, 304, 305};
  heap.bound_expression_ids = {50, 51, 52, 53, 54};
  heap.required_object_uuids = {heap_storage.relation_uuid.canonical};
  if (heap_is_columnar_model) {
    api::RelationalExpressionRecord source_expression;
    source_expression.expression_id = 61;
    source_expression.expression_kind =
        api::RelationalExpressionKind::kFunctionCall;
    source_expression.result_descriptor_id = 301;
    source_expression.operator_name = "COLUMNAR_SOURCE";
    source_expression.bound_name_uuid =
        heap_storage.relation_uuid.canonical;
    dag.expressions.push_back(std::move(source_expression));
    heap.bound_expression_ids.push_back(61);
    heap.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  } else {
    heap.semantic_variant_id = "relation.source.v1";
  }
  dag.nodes.push_back(std::move(heap));

  if (!downsample) {
    dag.descriptors.push_back(
        DagDescriptor(306, CoreTypeUuid("int64"), "int64"));
  }
  api::RelationalExpressionRecord tolerance;
  tolerance.expression_id = 60;
  tolerance.expression_kind = api::RelationalExpressionKind::kLiteral;
  tolerance.result_descriptor_id = downsample ? 206 : 306;
  tolerance.literal_kind = api::RelationalLiteralKind::kNumeric;
  tolerance.literal_or_parameter_ref = "30000000000";
  dag.expressions.push_back(std::move(tolerance));

  const std::array<std::uint32_t, 3> time_keys =
      downsample ? std::array<std::uint32_t, 3>{2, 5, 3}
                 : std::array<std::uint32_t, 3>{3, 5, 4};
  constexpr std::array<std::uint32_t, 3> heap_keys{53, 54, 52};
  std::vector<std::uint32_t> source_descriptors =
      dag.nodes.front().output_descriptor_ids;
  const std::vector<std::uint32_t> heap_descriptors{301, 302, 303, 304, 305};
  std::vector<std::uint32_t> join_descriptors =
      time_series_left ? source_descriptors : heap_descriptors;
  const auto& trailing_descriptors =
      time_series_left ? heap_descriptors : source_descriptors;
  join_descriptors.insert(join_descriptors.end(),
                          trailing_descriptors.begin(),
                          trailing_descriptors.end());

  std::vector<api::RelationalOutputRecord> source_outputs;
  std::vector<api::RelationalOutputRecord> heap_outputs;
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == 1) source_outputs.push_back(output);
    if (output.relation_node_id == 2) heap_outputs.push_back(output);
  }
  std::ranges::sort(source_outputs, {},
                    &api::RelationalOutputRecord::ordinal);
  std::ranges::sort(heap_outputs, {},
                    &api::RelationalOutputRecord::ordinal);
  std::vector<api::RelationalOutputRecord> join_outputs =
      time_series_left ? source_outputs : heap_outputs;
  const auto& trailing_outputs =
      time_series_left ? heap_outputs : source_outputs;
  join_outputs.insert(join_outputs.end(), trailing_outputs.begin(),
                      trailing_outputs.end());
  for (std::size_t ordinal = 0; ordinal < join_outputs.size(); ++ordinal) {
    dag.outputs.push_back(
        {static_cast<std::uint32_t>(70 + ordinal), 3,
         join_outputs[ordinal].expression_id,
         join_outputs[ordinal].output_name_utf8,
         join_outputs[ordinal].descriptor_id, true,
         static_cast<std::uint32_t>(ordinal)});
  }
  api::RelationalDagNode join;
  join.node_id = 3;
  join.node_kind = api::RelationalDagNodeKind::kJoin;
  join.input_node_ids = time_series_left
                            ? std::vector<std::uint32_t>{1, 2}
                            : std::vector<std::uint32_t>{2, 1};
  join.output_descriptor_ids = std::move(join_descriptors);
  if (time_series_left) {
    join.bound_expression_ids.assign(time_keys.begin(), time_keys.end());
    join.bound_expression_ids.insert(join.bound_expression_ids.end(),
                                     heap_keys.begin(), heap_keys.end());
  } else {
    join.bound_expression_ids.assign(heap_keys.begin(), heap_keys.end());
    join.bound_expression_ids.insert(join.bound_expression_ids.end(),
                                     time_keys.begin(), time_keys.end());
  }
  join.bound_expression_ids.push_back(60);
  join.semantic_variant_id = left_outer ? "join.asof.left.v1"
                                        : "join.asof.inner.v1";
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

std::string ApiRealBits(const std::string_view encoded) {
  double value = 0.0;
  const auto parsed = std::from_chars(encoded.data(),
                                      encoded.data() + encoded.size(), value,
                                      std::chars_format::general);
  return parsed.ec == std::errc{} &&
                 parsed.ptr == encoded.data() + encoded.size()
             ? RealBits(value)
             : std::string{};
}

std::string ProductionRawStream(
    const sblr::CanonicalObjectFreeValuesExecutionResult& execution) {
  std::ostringstream stream;
  for (std::size_t row = 0;
       row < execution.api_result.result_shape.rows.size(); ++row) {
    const auto value_bits =
        ApiRealBits(ApiRowField(execution.api_result, row, "value"));
    if (value_bits.empty()) return {};
    stream << ApiRowField(execution.api_result, row, "row_uuid") << '\t'
           << ApiRowField(execution.api_result, row, "series_uuid") << '\t'
           << ApiRowField(execution.api_result, row, "metric_uuid") << '\t'
           << ApiRowField(execution.api_result, row, "point_timestamp")
           << '\t' << ApiRowField(execution.api_result, row, "tags") << '\t'
           << value_bits << '\n';
  }
  return stream.str();
}

std::string ProductionDownsampleStream(
    const sblr::CanonicalObjectFreeValuesExecutionResult& execution,
    const bool count) {
  std::ostringstream stream;
  for (std::size_t row = 0;
       row < execution.api_result.result_shape.rows.size(); ++row) {
    const auto aggregate =
        ApiRowField(execution.api_result, row, "aggregate_value");
    const auto encoded_aggregate = count ? aggregate : ApiRealBits(aggregate);
    if (encoded_aggregate.empty()) return {};
    stream << ApiRowField(execution.api_result, row, "series_uuid") << '\t'
           << ApiRowField(execution.api_result, row, "metric_uuid") << '\t'
           << ApiRowField(execution.api_result, row, "bucket_start") << '\t'
           << ApiRowField(execution.api_result, row, "bucket_end") << '\t'
           << ApiRowField(execution.api_result, row, "tags") << '\t'
           << ApiRowField(execution.api_result, row, "sample_count") << '\t'
           << encoded_aggregate << '\n';
  }
  return stream.str();
}

std::string ProductionDescriptorStream(
    const sblr::CanonicalObjectFreeValuesExecutionResult& execution) {
  std::ostringstream stream;
  for (const auto& descriptor : execution.api_result.result_shape.columns) {
    stream << descriptor.descriptor_uuid.canonical << '\t'
           << descriptor.descriptor_kind << '\t'
           << descriptor.canonical_type_name << '\t'
           << descriptor.encoded_descriptor << '\n';
  }
  return stream.str();
}

std::string ProductionResultProofStream(
    const sblr::CanonicalObjectFreeValuesExecutionResult& execution) {
  const auto evidence_id = [&](const std::string_view kind) {
    const auto found = std::ranges::find_if(
        execution.api_result.evidence, [&](const auto& evidence) {
          return evidence.evidence_kind == kind;
        });
    return found == execution.api_result.evidence.end()
               ? std::string{}
               : found->evidence_id;
  };
  std::ostringstream stream;
  stream << execution.selected_plan_uuid << '\n'
         << Digest(ProductionDescriptorStream(execution)) << '\n';
  for (const auto& row : execution.api_result.result_shape.rows) {
    for (const auto& [name, value] : row.fields) {
      stream << name << '=' << value.encoded_value << ':'
             << static_cast<unsigned>(value.state) << '\t';
    }
    stream << '\n';
  }
  stream << evidence_id("canonical.time_series_root_causal_counter") << '\n'
         << evidence_id("canonical.time_series_cleanup_count") << '\n'
         << evidence_id("canonical.time_series_cleanup_complete") << '\n'
         << Digest(execution.canonical_result_bytes) << '\n';
  return stream.str();
}

bool ProductionRouteMatrix(const Fixture& fixture,
                           const api::EngineRequestContext& context,
                           std::set<std::string>* completed) {
  const auto& storage = fixture.descriptors.at(std::string(kBaseObjectUuid));
  const bool rollup_binding_known_answer =
      ExactTimeSeriesRollupCapabilityKnownAnswer();
  std::set<std::string> frontdoor_outcomes;
  const bool frontdoor_passed =
      FrontdoorMatrix(context, &frontdoor_outcomes);
  const auto execute = [&](const api::EngineBoundTimeSeriesAggregateV1 aggregate) {
    sblr::CanonicalCurrentHeapExecutionRequest request;
    request.context = context;
    request.relational_dag = TimeSeriesDag(context, storage, aggregate);
    return sblr::ExecuteCanonicalCurrentHeapQuery(request);
  };
  const auto raw = execute(api::EngineBoundTimeSeriesAggregateV1::kNone);
  const auto sum = execute(api::EngineBoundTimeSeriesAggregateV1::kSum);
  const auto count = execute(api::EngineBoundTimeSeriesAggregateV1::kCount);
  const auto avg = execute(api::EngineBoundTimeSeriesAggregateV1::kAvg);
  const auto minimum = execute(api::EngineBoundTimeSeriesAggregateV1::kMin);
  const auto maximum = execute(api::EngineBoundTimeSeriesAggregateV1::kMax);
  const auto raw_replay_dag = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kNone);
  const auto raw_replay = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, raw_replay_dag});
  const auto raw_replay_second = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, raw_replay_dag});
  const auto sum_replay_dag = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kSum);
  const auto sum_replay = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, sum_replay_dag});
  const auto sum_replay_second = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, sum_replay_dag});
  const auto count_replay_dag = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kCount);
  const auto count_replay = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, count_replay_dag});
  const auto count_replay_second = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, count_replay_dag});
  const auto execute_range = [&](std::string start, std::string end) {
    auto range_dag = TimeSeriesDag(
        context, storage, api::EngineBoundTimeSeriesAggregateV1::kNone);
    const auto start_expression = std::ranges::find_if(
        range_dag.expressions,
        [](const auto& expression) { return expression.expression_id == 21; });
    const auto end_expression = std::ranges::find_if(
        range_dag.expressions,
        [](const auto& expression) { return expression.expression_id == 22; });
    if (start_expression != range_dag.expressions.end()) {
      start_expression->literal_or_parameter_ref = std::move(start);
    }
    if (end_expression != range_dag.expressions.end()) {
      end_expression->literal_or_parameter_ref = std::move(end);
    }
    return sblr::ExecuteCanonicalCurrentHeapQuery(
        {context, std::move(range_dag)});
  };
  const auto empty_range = execute_range(std::string(kRangeStart),
                                         std::string(kRangeStart));
  const auto offset_range = execute_range(
      "2026-08-10T08:00:00.000000000-04:00",
      "2026-08-10T08:02:00.000000000-04:00");
  const auto reversed_range = execute_range(std::string(kRangeEnd),
                                            std::string(kRangeStart));
  const auto malformed_timestamp =
      execute_range("malformed", std::string(kRangeEnd));
  const std::array invalid_timestamps{
      malformed_timestamp,
      execute_range("2026-08-10T12:00:00", std::string(kRangeEnd)),
      execute_range("2016-12-31T23:59:60.000000000Z",
                    std::string(kRangeEnd)),
      execute_range("999999999999-01-01T00:00:00.000000000Z",
                    std::string(kRangeEnd))};
  auto empty_bucket_dag = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kSum);
  const auto empty_bucket_start = std::ranges::find_if(
      empty_bucket_dag.expressions,
      [](const auto& expression) { return expression.expression_id == 21; });
  const auto empty_bucket_end = std::ranges::find_if(
      empty_bucket_dag.expressions,
      [](const auto& expression) { return expression.expression_id == 22; });
  if (empty_bucket_start != empty_bucket_dag.expressions.end() &&
      empty_bucket_end != empty_bucket_dag.expressions.end()) {
    empty_bucket_start->literal_or_parameter_ref =
        "2026-08-10T12:03:00.000000000Z";
    empty_bucket_end->literal_or_parameter_ref =
        "2026-08-10T12:04:00.000000000Z";
  }
  const auto empty_bucket = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, std::move(empty_bucket_dag)});
  auto missing_range_dag = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kNone);
  const auto missing_start = std::ranges::find_if(
      missing_range_dag.expressions,
      [](const auto& expression) { return expression.expression_id == 21; });
  if (missing_start != missing_range_dag.expressions.end()) {
    missing_start->literal_or_parameter_ref.reset();
  }
  const auto missing_range = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, std::move(missing_range_dag)});
  auto wrong_alias_dag = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kNone);
  const auto range_expression = std::ranges::find_if(
      wrong_alias_dag.expressions,
      [](const auto& expression) { return expression.operator_name == "TIME_RANGE"; });
  if (range_expression != wrong_alias_dag.expressions.end() &&
      !range_expression->child_expression_ids.empty()) {
    const auto alias_expression = std::ranges::find_if(
        wrong_alias_dag.expressions, [&](const auto& expression) {
          return expression.expression_id ==
                 range_expression->child_expression_ids.front();
        });
    if (alias_expression != wrong_alias_dag.expressions.end()) {
      alias_expression->bound_name_uuid =
          NewUuidText(platform::UuidKind::object);
    }
  }
  const auto wrong_alias = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, std::move(wrong_alias_dag)});
  auto unsupported_aggregate_dag = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kSum);
  const auto aggregate_literal = std::ranges::find_if(
      unsupported_aggregate_dag.expressions,
      [](const auto& expression) { return expression.expression_id == 24; });
  if (aggregate_literal != unsupported_aggregate_dag.expressions.end()) {
    aggregate_literal->literal_or_parameter_ref = "MEDIAN";
  }
  const auto unsupported_aggregate = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, std::move(unsupported_aggregate_dag)});
  const auto& duplicate_tag_storage =
      fixture.descriptors.at(std::string(kDuplicateTagObjectUuid));
  const auto duplicate_tag = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, TimeSeriesDag(context, duplicate_tag_storage,
                              api::EngineBoundTimeSeriesAggregateV1::kNone)});
  const auto& nonfinite_storage =
      fixture.descriptors.at(std::string(kNonfiniteObjectUuid));
  const auto nonfinite = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, TimeSeriesDag(context, nonfinite_storage,
                              api::EngineBoundTimeSeriesAggregateV1::kNone)});
  const auto& invisible_storage =
      fixture.descriptors.at(std::string(kInvisibleInvalidObjectUuid));
  const auto invisible = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, TimeSeriesDag(context, invisible_storage,
                              api::EngineBoundTimeSeriesAggregateV1::kNone)});
  const auto ambiguous_generation = TimeSeriesProviderGeneration(
      context, kAmbiguousProviderUuid, kBaseObjectUuid,
      storage.descriptor_generation + 2'000);
  const auto published_ambiguous =
      api::PublishNoSqlProviderGeneration(context, ambiguous_generation);
  const auto ambiguous_fallback =
      execute(api::EngineBoundTimeSeriesAggregateV1::kNone);
  const auto dropped_ambiguous = api::DropNoSqlProviderGeneration(
      context, api::EngineNoSqlProviderFamily::kTimeSeries,
      std::string(kAmbiguousProviderUuid), std::string(kBaseObjectUuid));
  const auto dropped_preferred = api::DropNoSqlProviderGeneration(
      context, api::EngineNoSqlProviderFamily::kTimeSeries,
      std::string(kPreferredProviderUuid), std::string(kBaseObjectUuid));
  const auto fallback_raw_dag = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kNone);
  const auto fallback_raw = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, fallback_raw_dag});
  const auto fallback_raw_replay = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, fallback_raw_dag});
  const auto fallback_sum_dag = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kSum);
  const auto fallback_sum = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, fallback_sum_dag});
  const auto fallback_sum_replay = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, fallback_sum_dag});
  const auto fallback_count_dag = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kCount);
  const auto fallback_count = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, fallback_count_dag});
  const auto fallback_count_replay = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, fallback_count_dag});
  const auto restored_generation = TimeSeriesProviderGeneration(
      context, kPreferredProviderUuid, kBaseObjectUuid,
      storage.descriptor_generation + 1'000);
  const auto restored_preferred =
      api::PublishNoSqlProviderGeneration(context, restored_generation);
  auto stale_rollup_generation = TimeSeriesProviderGeneration(
      context, kPreferredProviderUuid, kBaseObjectUuid,
      storage.descriptor_generation + 1'000);
  AttachExactTimeSeriesRollupCarrier(&stale_rollup_generation, context,
                                     7, 8);
  const auto rollup_capability_uuid =
      stale_rollup_generation.time_series_rollup_capability_uuid;
  auto mismatched_rollup_generation = stale_rollup_generation;
  mismatched_rollup_generation.time_series_rollup_capability_uuid =
      "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
  const auto refused_mismatched_rollup_publish =
      api::PublishNoSqlProviderGeneration(context,
                                          mismatched_rollup_generation);
  auto uppercase_rollup_generation = stale_rollup_generation;
  uppercase_rollup_generation.provider_id =
      "AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA";
  const bool uppercase_rollup_derivation_empty =
      api::DeriveTimeSeriesRollupCapabilityUuidV1(
          uppercase_rollup_generation)
          .empty();
  const auto refused_uppercase_rollup_publish =
      api::PublishNoSqlProviderGeneration(context,
                                          uppercase_rollup_generation);
  auto zero_uuid_rollup_generation = stale_rollup_generation;
  zero_uuid_rollup_generation.database_uuid =
      "00000000-0000-0000-0000-000000000000";
  const bool zero_uuid_rollup_derivation_empty =
      api::DeriveTimeSeriesRollupCapabilityUuidV1(
          zero_uuid_rollup_generation)
          .empty();
  const auto refused_zero_uuid_rollup_publish =
      api::PublishNoSqlProviderGeneration(context,
                                          zero_uuid_rollup_generation);
  const auto published_stale_rollup =
      api::PublishNoSqlProviderGeneration(context, stale_rollup_generation);
  const auto cleared_rollup_cache =
      api::CleanupNoSqlProviderGenerations(context, false);
  const auto loaded_stale_rollup = api::LoadNoSqlProviderGeneration(
      context, api::EngineNoSqlProviderFamily::kTimeSeries,
      std::string(kPreferredProviderUuid), std::string(kBaseObjectUuid));
  const auto listed_stale_rollups =
      api::ListNoSqlProviderGenerations(context);
  api::EngineNoSqlProviderGenerationRepairRequest repair_rollup;
  repair_rollup.family = api::EngineNoSqlProviderFamily::kTimeSeries;
  repair_rollup.provider_id = std::string(kPreferredProviderUuid);
  repair_rollup.collection_uuid = std::string(kBaseObjectUuid);
  repair_rollup.repair_admitted = true;
  repair_rollup.authoritative_source_generations.push_back(
      stale_rollup_generation);
  const auto repaired_stale_rollup =
      api::RepairNoSqlProviderGeneration(context, repair_rollup);
  const auto rewrite_probe_generation = TimeSeriesProviderGeneration(
      context, kAmbiguousProviderUuid, kPreEpochObjectUuid,
      fixture.descriptors.at(std::string(kPreEpochObjectUuid))
              .descriptor_generation +
          2'000);
  const auto published_rewrite_probe =
      api::PublishNoSqlProviderGeneration(context, rewrite_probe_generation);
  const auto dropped_rewrite_probe = api::DropNoSqlProviderGeneration(
      context, api::EngineNoSqlProviderFamily::kTimeSeries,
      std::string(kAmbiguousProviderUuid), std::string(kPreEpochObjectUuid));
  const auto cleared_rewritten_rollup_cache =
      api::CleanupNoSqlProviderGenerations(context, false);
  const auto loaded_rewritten_rollup = api::LoadNoSqlProviderGeneration(
      context, api::EngineNoSqlProviderFamily::kTimeSeries,
      std::string(kPreferredProviderUuid), std::string(kBaseObjectUuid));

  using PersistedGenerationPairs =
      std::vector<std::pair<std::string, std::string>>;
  using ProductionExecution = std::remove_cvref_t<decltype(raw)>;
  const auto generation_store_path =
      std::filesystem::path(context.database_path +
                            ".sb.nosql_provider_generations");
  const auto pair_value = [](const PersistedGenerationPairs& pairs,
                             const std::string_view key) {
    const auto found = std::ranges::find_if(pairs, [&](const auto& pair) {
      return pair.first == key;
    });
    return found == pairs.end() ? std::string{} : found->second;
  };
  const auto set_pair = [](PersistedGenerationPairs* pairs,
                           const std::string_view key,
                           const std::string& value) {
    if (pairs == nullptr) return false;
    const auto found = std::ranges::find_if(*pairs, [&](const auto& pair) {
      return pair.first == key;
    });
    if (found == pairs->end()) return false;
    found->second = value;
    return true;
  };
  std::vector<std::string> unrelated_generation_records;
  PersistedGenerationPairs valid_rollup_pairs;
  bool captured_single_valid_rollup_record = false;
  {
    std::ifstream in(generation_store_path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      const auto first_tab = line.find('\t');
      const auto second_tab =
          first_tab == std::string::npos
              ? std::string::npos
              : line.find('\t', first_tab + 1);
      const bool generation_record =
          first_tab != std::string::npos &&
          second_tab != std::string::npos &&
          line.substr(0, first_tab) == "SBNOSQLPG1" &&
          line.substr(first_tab + 1, second_tab - first_tab - 1) ==
              "GENERATION";
      if (!generation_record) {
        unrelated_generation_records.push_back(line);
        continue;
      }
      auto pairs = api::DecodeCrudPairs(line.substr(second_tab + 1));
      const bool target_record =
          pair_value(pairs, "family") == "time_series" &&
          pair_value(pairs, "provider_id") == kPreferredProviderUuid &&
          pair_value(pairs, "collection_uuid") == kBaseObjectUuid;
      if (target_record) {
        valid_rollup_pairs = std::move(pairs);
        captured_single_valid_rollup_record = true;
      } else {
        unrelated_generation_records.push_back(line);
      }
    }
  }
  const auto write_target_generation =
      [&](const PersistedGenerationPairs& pairs) {
        std::ofstream out(generation_store_path,
                          std::ios::binary | std::ios::trunc);
        if (!out) return false;
        for (const auto& line : unrelated_generation_records) {
          out << line << '\n';
        }
        out << "SBNOSQLPG1\tGENERATION\t"
            << api::EncodeCrudPairs(pairs) << '\n';
        out.flush();
        return static_cast<bool>(out);
      };
  struct PersistedMutationCase {
    std::string label;
    std::string field;
    std::string substituted_value;
    std::string expected_diagnostic;
    bool security_redaction = false;
  };
  const auto substituted_capability =
      NewUuidText(platform::UuidKind::object);
  const auto substituted_generation =
      NewUuidText(platform::UuidKind::object);
  const auto substituted_statement_snapshot =
      NewUuidText(platform::UuidKind::object);
  const auto substituted_metadata_snapshot =
      NewUuidText(platform::UuidKind::object);
  const auto substituted_transaction =
      NewUuidText(platform::UuidKind::transaction);
  const auto substituted_security_context =
      NewUuidText(platform::UuidKind::principal);
  const auto substituted_catalog_epoch =
      NewUuidText(platform::UuidKind::object);
  const auto substituted_provider =
      NewUuidText(platform::UuidKind::object);
  const auto substituted_database =
      NewUuidText(platform::UuidKind::database);
  const auto substituted_collection =
      NewUuidText(platform::UuidKind::object);
  const std::vector<PersistedMutationCase> persisted_mutation_cases{
      {"capability_uuid", "time_series_rollup_capability_uuid",
       substituted_capability, "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"family", "family", "document",
       "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"generation_uuid", "generation_uuid", substituted_generation,
       "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"generation_id", "generation_id",
       std::to_string(stale_rollup_generation.generation_id + 1),
       "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"publish_state", "publish_state", "substituted",
       "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"validation_state", "validation_state", "substituted",
       "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"provider_finality_authority",
       "provider_claims_transaction_finality_authority", "true",
       "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"provider_visibility_authority",
       "provider_claims_visibility_authority", "true",
       "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"candidate_present", "time_series_rollup_candidate_present", "false",
       "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"rollup_generation_zero", "time_series_rollup_generation", "0",
       "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"rollup_generation_ahead", "time_series_rollup_generation", "9",
       "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"rollup_generation_valid_substitution",
       "time_series_rollup_generation", "6",
       "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"rollup_generation_nonnumeric", "time_series_rollup_generation",
       "substituted-generation", "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"visible_late_arrival_generation",
       "time_series_visible_late_arrival_generation", "0",
       "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"visible_late_arrival_valid_substitution",
       "time_series_visible_late_arrival_generation", "9",
       "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"interval", "time_series_rollup_interval_ns", "30000000000",
       "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"attestation", "time_series_rollup_exactness_attestation_state",
       "SUBSTITUTED_ATTESTATION", "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"residual_recheck", "time_series_rollup_exact_residual_recheck_required",
       "false", "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"base_row_mga_recheck",
       "time_series_rollup_base_row_mga_recheck_required", "false",
       "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"security_recheck", "time_series_rollup_security_recheck_required",
       "false", "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1", true},
      {"statement_snapshot", "time_series_rollup_statement_snapshot_uuid",
       substituted_statement_snapshot, "SB_MODEL_MGA_CONTEXT_MISMATCH_V1"},
      {"metadata_snapshot",
       "time_series_rollup_statement_metadata_snapshot_uuid",
       substituted_metadata_snapshot, "SB_MODEL_MGA_CONTEXT_MISMATCH_V1"},
      {"owning_transaction", "time_series_rollup_owning_transaction_uuid",
       substituted_transaction, "SB_MODEL_MGA_CONTEXT_MISMATCH_V1"},
      {"local_transaction", "time_series_rollup_local_transaction_id",
       std::to_string(context.local_transaction_id + 1),
       "SB_MODEL_MGA_CONTEXT_MISMATCH_V1"},
      {"visibility_horizon",
       "time_series_rollup_snapshot_visible_through_local_transaction_id",
       std::to_string(
           context.snapshot_visible_through_local_transaction_id + 1),
       "SB_MODEL_MGA_CONTEXT_MISMATCH_V1"},
      {"security_context", "time_series_rollup_security_context_uuid",
       substituted_security_context, "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
       true},
      {"security_epoch", "security_epoch",
       std::to_string(std::max<std::uint64_t>(1, context.security_epoch) + 1),
       "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1", true},
      {"redaction_epoch", "redaction_epoch",
       std::to_string(std::max<std::uint64_t>(1, context.security_epoch) + 1),
       "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1", true},
      {"catalog_epoch_uuid", "time_series_rollup_catalog_epoch_uuid",
       substituted_catalog_epoch, "SB_MODEL_CATALOG_GENERATION_STALE_V1"},
      {"catalog_epoch", "catalog_epoch",
       std::to_string(
           std::max<std::uint64_t>(1, context.catalog_generation_id) + 1),
       "SB_MODEL_CATALOG_GENERATION_STALE_V1"},
      {"descriptor_epoch", "descriptor_epoch",
       std::to_string(std::max<std::uint64_t>(1, context.resource_epoch) + 1),
       "SB_MODEL_CATALOG_GENERATION_STALE_V1"},
      {"provider_id", "provider_id", substituted_provider,
       "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"database_identity", "database_identity",
       api::EngineNoSqlProviderDatabaseIdentity(context) + ".substituted",
       "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"database_uuid", "database_uuid", substituted_database,
       "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
      {"collection_uuid", "collection_uuid", substituted_collection,
       "SB_MODEL_PROVIDER_GENERATION_STALE_V1"},
  };
  struct PersistedMutationObservation {
    PersistedMutationCase mutation;
    std::optional<ProductionExecution> execution;
    bool cache_cleared = false;
    bool restored_and_revalidated = false;
  };
  std::vector<PersistedMutationObservation> persisted_mutation_observations;
  bool mutation_fixture_exact =
      captured_single_valid_rollup_record &&
      pair_value(valid_rollup_pairs, "time_series_rollup_capability_uuid") ==
          rollup_capability_uuid;
  if (mutation_fixture_exact) {
    for (const auto& mutation : persisted_mutation_cases) {
      auto mutated_pairs = valid_rollup_pairs;
      PersistedMutationObservation observation;
      observation.mutation = mutation;
      const bool mutation_written =
          set_pair(&mutated_pairs, mutation.field, mutation.substituted_value) &&
          write_target_generation(mutated_pairs);
      const auto cleared_mutation_cache =
          api::CleanupNoSqlProviderGenerations(context, false);
      observation.cache_cleared = mutation_written && cleared_mutation_cache.ok;
      if (observation.cache_cleared) {
        observation.execution =
            execute(api::EngineBoundTimeSeriesAggregateV1::kSum);
      }
      const bool valid_record_restored =
          write_target_generation(valid_rollup_pairs);
      const auto cleared_restored_cache =
          api::CleanupNoSqlProviderGenerations(context, false);
      const auto reloaded_valid_rollup = api::LoadNoSqlProviderGeneration(
          context, api::EngineNoSqlProviderFamily::kTimeSeries,
          std::string(kPreferredProviderUuid), std::string(kBaseObjectUuid));
      observation.restored_and_revalidated =
          valid_record_restored && cleared_restored_cache.ok &&
          reloaded_valid_rollup.ok &&
          api::ValidateTimeSeriesRollupCapabilityBindingV1(
              reloaded_valid_rollup.metadata) &&
          reloaded_valid_rollup.metadata.time_series_rollup_capability_uuid ==
              rollup_capability_uuid;
      mutation_fixture_exact =
          mutation_fixture_exact && observation.cache_cleared &&
          observation.restored_and_revalidated;
      persisted_mutation_observations.push_back(std::move(observation));
    }
  }
  const auto stale_rollup_sum =
      execute(api::EngineBoundTimeSeriesAggregateV1::kSum);
  const auto stale_rollup_count =
      execute(api::EngineBoundTimeSeriesAggregateV1::kCount);

  auto legacy_no_key_pairs = valid_rollup_pairs;
  legacy_no_key_pairs.erase(
      std::remove_if(legacy_no_key_pairs.begin(), legacy_no_key_pairs.end(),
                     [](const auto& pair) {
                       return pair.first.starts_with("time_series_");
                     }),
      legacy_no_key_pairs.end());
  const bool legacy_no_key_physically_omitted =
      captured_single_valid_rollup_record &&
      std::ranges::none_of(legacy_no_key_pairs, [](const auto& pair) {
        return pair.first.starts_with("time_series_");
      }) &&
      write_target_generation(legacy_no_key_pairs);
  const auto cleared_legacy_no_key_cache =
      api::CleanupNoSqlProviderGenerations(context, false);
  const auto loaded_legacy_no_key = api::LoadNoSqlProviderGeneration(
      context, api::EngineNoSqlProviderFamily::kTimeSeries,
      std::string(kPreferredProviderUuid), std::string(kBaseObjectUuid));
  const auto legacy_no_key_downsample =
      execute(api::EngineBoundTimeSeriesAggregateV1::kSum);
  auto request_only_rollup = Request(
      context, storage,
      api::EngineBoundTimeSeriesReadOperationV1::kBucketDownsample,
      api::EngineBoundTimeSeriesAggregateV1::kSum);
  request_only_rollup.rollup_candidate_selected = true;
  request_only_rollup.rollup_generation = 7;
  request_only_rollup.visible_late_arrival_generation = 8;
  request_only_rollup.exact_fallback_selected = true;
  const auto request_only_rollup_supplement =
      api::EngineBoundTimeSeriesReadV1(request_only_rollup);
  const auto restored_legacy_generation = TimeSeriesProviderGeneration(
      context, kPreferredProviderUuid, kBaseObjectUuid,
      storage.descriptor_generation + 1'000);
  const auto restored_legacy_provider =
      api::PublishNoSqlProviderGeneration(context, restored_legacy_generation);
  const auto cleared_legacy_cache =
      api::CleanupNoSqlProviderGenerations(context, false);
  const auto loaded_legacy_provider = api::LoadNoSqlProviderGeneration(
      context, api::EngineNoSqlProviderFamily::kTimeSeries,
      std::string(kPreferredProviderUuid), std::string(kBaseObjectUuid));
  auto stale_provider_generation = TimeSeriesProviderGeneration(
      context, kPreferredProviderUuid, kBaseObjectUuid,
      storage.descriptor_generation + 1'000);
  ++stale_provider_generation.catalog_epoch;
  const auto published_stale_provider =
      api::PublishNoSqlProviderGeneration(context, stale_provider_generation);
  const auto cleared_stale_provider_cache =
      api::CleanupNoSqlProviderGenerations(context, false);
  const auto stale_provider_fallback =
      execute(api::EngineBoundTimeSeriesAggregateV1::kNone);
  const auto restored_after_stale_provider =
      api::PublishNoSqlProviderGeneration(context, restored_legacy_generation);
  const auto cleared_restored_provider_cache =
      api::CleanupNoSqlProviderGenerations(context, false);
  const auto bucket = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, TimeSeriesBucketDag(context, storage)});
  const auto& pre_epoch_storage =
      fixture.descriptors.at(std::string(kPreEpochObjectUuid));
  const auto pre_epoch_bucket = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context,
       TimeSeriesBucketDag(context, pre_epoch_storage,
                           "1969-12-31T23:59:00.000000000Z",
                           "1970-01-01T00:00:00.000000000Z")});
  const auto bucket_downsample = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, TimeSeriesBucketDownsampleDag(context, storage)});
  const auto mismatched_bucket_downsample =
      sblr::ExecuteCanonicalCurrentHeapQuery(
          {context,
           TimeSeriesBucketDownsampleDag(context, storage, "PT30S")});
  const auto day_bucket = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context,
       TimeSeriesBucketDag(context, storage, kRangeStart, kRangeEnd, "P1D")});
  const auto invalid_bucket = [&](const std::string_view interval) {
    return sblr::ExecuteCanonicalCurrentHeapQuery(
        {context, TimeSeriesBucketDag(context, storage, kRangeStart,
                                      kRangeEnd, interval)});
  };
  const std::array invalid_intervals{
      invalid_bucket("0"), invalid_bucket("-1"), invalid_bucket("P1M"),
      invalid_bucket("PT0.0000000001S"), invalid_bucket("P106752D")};
  const auto execute_dag = [&](api::TypedRelationalDag dag) {
    return sblr::ExecuteCanonicalCurrentHeapQuery({context, std::move(dag)});
  };
  auto substituted_raw_descriptor = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kNone);
  substituted_raw_descriptor.descriptors[3].descriptor_uuid =
      NewUuidText(platform::UuidKind::object);
  const auto refused_raw_descriptor =
      execute_dag(std::move(substituted_raw_descriptor));
  auto substituted_raw_binding = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kNone);
  substituted_raw_binding.expressions[3].bound_name_uuid =
      NewUuidText(platform::UuidKind::object);
  const auto refused_raw_binding =
      execute_dag(std::move(substituted_raw_binding));
  auto substituted_raw_name = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kNone);
  substituted_raw_name.outputs.front().output_name_utf8 = "substituted";
  const auto refused_raw_name = execute_dag(std::move(substituted_raw_name));
  auto substituted_bucket = TimeSeriesBucketDag(context, storage);
  const auto bucket_timestamp = std::ranges::find_if(
      substituted_bucket.expressions, [](const auto& expression) {
        return expression.expression_id == 4;
      });
  if (bucket_timestamp != substituted_bucket.expressions.end()) {
    bucket_timestamp->bound_name_uuid =
        NewUuidText(platform::UuidKind::object);
  }
  const auto refused_bucket_binding =
      execute_dag(std::move(substituted_bucket));
  auto substituted_bucket_name = TimeSeriesBucketDag(context, storage);
  substituted_bucket_name.outputs.front().output_name_utf8 = "substituted";
  const auto refused_bucket_name =
      execute_dag(std::move(substituted_bucket_name));
  auto substituted_downsample = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kSum);
  const auto downsample_value = std::ranges::find_if(
      substituted_downsample.expressions, [](const auto& expression) {
        return expression.expression_id == 26;
      });
  if (downsample_value != substituted_downsample.expressions.end()) {
    downsample_value->bound_name_uuid =
        NewUuidText(platform::UuidKind::object);
  }
  const auto refused_downsample_binding =
      execute_dag(std::move(substituted_downsample));
  auto substituted_object = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kNone);
  substituted_object.nodes.front().required_object_uuids.front() =
      std::string(kPreEpochObjectUuid);
  const auto refused_object = execute_dag(std::move(substituted_object));
  constexpr std::string_view kUnavailableObjectUuid =
      "40000000-0000-4000-8000-0000000076ff";
  auto unavailable_storage = storage;
  unavailable_storage.relation_uuid.canonical =
      std::string(kUnavailableObjectUuid);
  auto unavailable_context = context;
  AddAuthorization(&unavailable_context, std::string(kUnavailableObjectUuid));
  const auto unavailable = sblr::ExecuteCanonicalCurrentHeapQuery(
      {unavailable_context,
       TimeSeriesDag(unavailable_context, unavailable_storage,
                     api::EngineBoundTimeSeriesAggregateV1::kNone)});
  sblr::CanonicalCurrentHeapExecutionRequest composition_request;
  composition_request.context = context;
  composition_request.relational_dag =
      TimeSeriesFilterProjectDag(context, storage);
  const auto composition =
      sblr::ExecuteCanonicalCurrentHeapQuery(composition_request);
  const auto composition_replay =
      sblr::ExecuteCanonicalCurrentHeapQuery(composition_request);
  sblr::CanonicalCurrentHeapExecutionRequest unary_request;
  unary_request.context = context;
  unary_request.relational_dag =
      TimeSeriesUnaryCompositionDag(context, storage);
  const auto unary = sblr::ExecuteCanonicalCurrentHeapQuery(unary_request);
  sblr::CanonicalCurrentHeapExecutionRequest cte_limit_request;
  cte_limit_request.context = context;
  cte_limit_request.relational_dag =
      TimeSeriesCteLimitDag(context, storage);
  const auto cte_limit =
      sblr::ExecuteCanonicalCurrentHeapQuery(cte_limit_request);
  sblr::CanonicalCurrentHeapExecutionRequest count_request;
  count_request.context = context;
  count_request.relational_dag = TimeSeriesCountDag(context, storage);
  const auto counted = sblr::ExecuteCanonicalCurrentHeapQuery(count_request);
  const auto recursive_dag = TimeSeriesRecursiveDag(context, storage);
  const auto recursive = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, recursive_dag});
  const auto recursive_replay = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, recursive_dag});
  const auto set_dag = TimeSeriesSetDag(context, storage);
  const auto set_union = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, set_dag});
  const auto set_replay = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, set_dag});
  const auto& heap_storage =
      fixture.descriptors.at(std::string(kJoinObjectUuid));
  const auto direct_multileg_profiles =
      Rcp079DirectProofMultilegProfilesV10();
  bool direct_multileg_scope_exact = direct_multileg_profiles.size() == 320;
  const auto execute_multileg =
      [&](const api::EngineRequestContext& request_context,
          api::TypedRelationalDag dag) {
        sblr::CanonicalObjectFreeValuesExecutionResult execution;
        bool installed = false;
        {
          opt::MultilegDescriptorDispatchScopeV1 descriptor_scope(
              request_context.statement_uuid.canonical,
              direct_multileg_profiles);
          installed = descriptor_scope.installed();
          if (installed) {
            execution = sblr::ExecuteCanonicalCurrentHeapQuery(
                {request_context, std::move(dag)});
          }
        }
        const auto released = opt::LookupMultilegDescriptorDispatchScopeV1(
            request_context.statement_uuid.canonical);
        direct_multileg_scope_exact &= Require(
            installed && !released.accepted && released.profiles.empty() &&
                released.diagnostic_id ==
                    "SB_MODEL_RESULT_DESCRIPTOR_SCOPE_REQUIRED_V1",
            "direct multileg V10 descriptor scope installation/release "
            "drifted");
        return execution;
      };
  const auto cross_join = execute_multileg(
      context, TimeSeriesMixedJoinDag(context, storage, heap_storage,
                                      "join.cross.v1"));
  const auto inner_join = execute_multileg(
      context, TimeSeriesMixedJoinDag(context, storage, heap_storage,
                                      "join.inner.v1"));
  const auto left_join = execute_multileg(
      context, TimeSeriesMixedJoinDag(context, storage, heap_storage,
                                      "join.left-outer.v1"));
  const auto right_join = execute_multileg(
      context, TimeSeriesMixedJoinDag(context, storage, heap_storage,
                                      "join.right-outer.v1"));
  const auto full_join = execute_multileg(
      context, TimeSeriesMixedJoinDag(context, storage, heap_storage,
                                      "join.full-outer.v1"));
  const auto semi_join = execute_multileg(
      context, TimeSeriesMixedJoinDag(context, storage, heap_storage,
                                      "join.left-semi.v1"));
  const auto anti_join = execute_multileg(
      context, TimeSeriesMixedJoinDag(context, storage, heap_storage,
                                      "join.left-anti.v1"));
  const auto execute_asof =
      [&](const api::EngineBoundTimeSeriesAggregateV1 aggregate,
          const bool time_series_left, const bool left_outer) {
        const auto& selected_heap = fixture.descriptors.at(std::string(
            time_series_left ? kAsofRightObjectUuid : kJoinObjectUuid));
        return execute_multileg(
            context, TimeSeriesAsofJoinDag(context, storage, selected_heap,
                                           aggregate, time_series_left,
                                           left_outer));
      };
  const auto raw_series_left_outer = execute_asof(
      api::EngineBoundTimeSeriesAggregateV1::kNone, true, true);
  const auto raw_series_left_inner = execute_asof(
      api::EngineBoundTimeSeriesAggregateV1::kNone, true, false);
  const auto raw_series_left_columnar =
      execute_multileg(
          context,
          TimeSeriesAsofJoinDag(
              context, storage,
              fixture.descriptors.at(std::string(kAsofRightObjectUuid)),
              api::EngineBoundTimeSeriesAggregateV1::kNone, true, true,
              true));
  const auto raw_series_right_dag = TimeSeriesAsofJoinDag(
      context, storage, heap_storage,
      api::EngineBoundTimeSeriesAggregateV1::kNone, false, true);
  const auto raw_series_right_outer =
      execute_multileg(context, raw_series_right_dag);
  const auto raw_series_right_inner = execute_asof(
      api::EngineBoundTimeSeriesAggregateV1::kNone, false, false);
  const auto raw_series_right_columnar =
      execute_multileg(
          context,
          TimeSeriesAsofJoinDag(
              context, storage, heap_storage,
              api::EngineBoundTimeSeriesAggregateV1::kNone, false, true,
              true));
  const auto raw_series_right_replay =
      execute_multileg(context, raw_series_right_dag);
  const auto downsample_series_left_outer = execute_asof(
      api::EngineBoundTimeSeriesAggregateV1::kSum, true, true);
  const auto downsample_series_left_inner = execute_asof(
      api::EngineBoundTimeSeriesAggregateV1::kSum, true, false);
  const auto downsample_series_right_outer = execute_asof(
      api::EngineBoundTimeSeriesAggregateV1::kSum, false, true);
  const auto downsample_series_right_inner = execute_asof(
      api::EngineBoundTimeSeriesAggregateV1::kSum, false, false);
  const auto stale_catalog_dag = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kNone);
  auto stale_catalog_context = context;
  ++stale_catalog_context.catalog_generation_id;
  ++stale_catalog_context.authorization_context.catalog_generation_id;
  stale_catalog_context.catalog_epoch_uuid.canonical =
      NewUuidText(platform::UuidKind::object);
  const auto stale_catalog = sblr::ExecuteCanonicalCurrentHeapQuery(
      {stale_catalog_context, stale_catalog_dag});
  auto denied_context = context;
  std::erase_if(denied_context.authorization_context.grants,
                [](const auto& grant) {
                  return grant.target_uuid.canonical == kBaseObjectUuid;
                });
  const auto denied = sblr::ExecuteCanonicalCurrentHeapQuery(
      {denied_context,
       TimeSeriesDag(denied_context, storage,
                     api::EngineBoundTimeSeriesAggregateV1::kNone)});
  const auto security_generation_dag = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kNone);
  auto security_generation_context = context;
  ++security_generation_context.authorization_context.security_epoch;
  const auto changed_security_generation =
      sblr::ExecuteCanonicalCurrentHeapQuery(
          {security_generation_context, security_generation_dag});
  auto substituted_mga_dag = TimeSeriesDag(
      context, storage, api::EngineBoundTimeSeriesAggregateV1::kNone);
  substituted_mga_dag.statement_snapshot_uuid =
      NewUuidText(platform::UuidKind::object);
  const auto substituted_mga = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, std::move(substituted_mga_dag)});
  auto cancelled_context = context;
  cancelled_context.query_cancellation_requested = [] { return true; };
  const auto cancelled = sblr::ExecuteCanonicalCurrentHeapQuery(
      {cancelled_context,
       TimeSeriesDag(cancelled_context, storage,
                     api::EngineBoundTimeSeriesAggregateV1::kNone)});
  const auto first_diagnostic = [](const auto& execution) {
    return execution.api_result.diagnostics.empty()
               ? std::string("no-diagnostic")
               : execution.api_result.diagnostics.front().code + ":" +
                     execution.api_result.diagnostics.front().detail;
  };
  std::optional<std::remove_cvref_t<decltype(raw)>> combined_peak_refused;
  std::ostringstream combined_peak_attempts;
  for (const std::uint64_t memory_budget :
       {8192ULL, 12288ULL, 16384ULL, 24576ULL, 32768ULL, 49152ULL,
        65536ULL, 98304ULL, 131072ULL}) {
    auto low_memory_context = context;
    low_memory_context.optimizer_memory_budget_bytes = memory_budget;
    const auto low_memory = execute_multileg(
        low_memory_context,
        TimeSeriesAsofJoinDag(
            low_memory_context, storage,
            fixture.descriptors.at(std::string(kAsofRightObjectUuid)),
            api::EngineBoundTimeSeriesAggregateV1::kNone, true, true));
    combined_peak_attempts << '[' << memory_budget << ':'
                           << first_diagnostic(low_memory) << ']';
    if (!low_memory.api_result.ok &&
        !low_memory.api_result.diagnostics.empty() &&
        low_memory.api_result.diagnostics.front().code ==
            "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1" &&
        (low_memory.api_result.diagnostics.front().detail ==
             "time-series ASOF combined input/copy/key/result peak exceeded the selected memory bound" ||
         low_memory.api_result.diagnostics.front().detail.find(
             "heap_read_maximum_memory_bytes_exceeded") !=
             std::string::npos)) {
      combined_peak_refused = low_memory;
      break;
    }
  }
  auto scan_bound_context = context;
  scan_bound_context.optimizer_maximum_search_steps = 4;
  const auto scan_bound_refused = sblr::ExecuteCanonicalCurrentHeapQuery(
      {scan_bound_context,
       TimeSeriesDag(scan_bound_context, storage,
                     api::EngineBoundTimeSeriesAggregateV1::kNone)});
  auto output_bound_context = context;
  output_bound_context.optimizer_maximum_candidate_count = 2;
  const auto output_bound_refused = sblr::ExecuteCanonicalCurrentHeapQuery(
      {output_bound_context,
       TimeSeriesDag(output_bound_context, storage,
                     api::EngineBoundTimeSeriesAggregateV1::kNone)});
  auto group_bound_context = context;
  group_bound_context.optimizer_maximum_memo_groups = 2;
  const auto group_bound_refused = sblr::ExecuteCanonicalCurrentHeapQuery(
      {group_bound_context,
       TimeSeriesDag(group_bound_context, storage,
                     api::EngineBoundTimeSeriesAggregateV1::kSum)});
  std::optional<std::remove_cvref_t<decltype(raw)>> byte_bound_refused;
  std::ostringstream byte_bound_attempts;
  for (const std::uint64_t memory_budget :
       {512ULL, 1024ULL, 2048ULL, 4096ULL, 8192ULL, 12288ULL,
        16384ULL}) {
    auto byte_bound_context = context;
    byte_bound_context.optimizer_memory_budget_bytes = memory_budget;
    const auto candidate = sblr::ExecuteCanonicalCurrentHeapQuery(
        {byte_bound_context,
         TimeSeriesDag(byte_bound_context, storage,
                       api::EngineBoundTimeSeriesAggregateV1::kNone)});
    byte_bound_attempts << '[' << memory_budget << ':'
                        << first_diagnostic(candidate) << ']';
    if (!candidate.api_result.ok &&
        !candidate.api_result.diagnostics.empty() &&
        candidate.api_result.diagnostics.front().code ==
            "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1" &&
        (candidate.api_result.diagnostics.front().detail.find(
             "heap_read_maximum_decoded_bytes_exceeded") !=
             std::string::npos ||
         candidate.api_result.diagnostics.front().detail.find(
             "heap_read_maximum_memory_bytes_exceeded") !=
             std::string::npos)) {
      byte_bound_refused = candidate;
      break;
    }
  }
  const auto route_diagnostic = [](const auto& result) {
    return result.api_result.diagnostics.empty()
               ? std::string("no-diagnostic")
               : result.api_result.diagnostics.front().code + ":" +
                     result.api_result.diagnostics.front().detail;
  };
  const auto has_evidence = [](const auto& result,
                               const std::string_view kind,
                               const std::string_view id) {
    return std::ranges::any_of(result.api_result.evidence,
                               [&](const auto& evidence) {
                                  return evidence.evidence_kind == kind &&
                                        evidence.evidence_id == id;
                               });
  };
  const auto evidence_id = [](const auto& result,
                              const std::string_view kind) {
    const auto found = std::ranges::find_if(
        result.api_result.evidence, [&](const auto& evidence) {
          return evidence.evidence_kind == kind;
        });
    return found == result.api_result.evidence.end()
               ? std::string("absent")
               : found->evidence_id;
  };
  const auto atomic_no_root = [](const auto& execution) {
    return !execution.api_result.ok && !execution.physical_dag_executed &&
           !execution.canonical_result_published &&
           execution.physical_node_count == 0 &&
           execution.canonical_result_column_count == 0 &&
           execution.canonical_result_row_count == 0 &&
           execution.selected_plan_uuid.empty() &&
           execution.canonical_result_bytes.empty() &&
           execution.api_result.result_shape.columns.empty() &&
           execution.api_result.result_shape.rows.empty();
  };
  const auto post_access_cleanup_once = [&](const auto& execution) {
    return has_evidence(execution,
                        "canonical.time_series_source_execution_started",
                        "true") &&
           has_evidence(execution,
                        "canonical.time_series_source_data_access_observed",
                        "true") &&
           has_evidence(execution, "canonical.time_series_cleanup_count",
                        "1") &&
           has_evidence(execution, "canonical.time_series_cleanup_complete",
                        "true");
  };
  const auto pre_access_cleanup_zero = [&](const auto& execution) {
    return has_evidence(execution,
                        "canonical.time_series_source_execution_started",
                        "false") &&
           has_evidence(execution,
                        "canonical.time_series_source_data_access_observed",
                        "false") &&
           has_evidence(execution, "canonical.time_series_cleanup_count",
                        "0") &&
           has_evidence(execution, "canonical.time_series_cleanup_complete",
                        "false");
  };
  const auto atomic_success_cleanup_once = [&](const auto& execution) {
    return execution.api_result.ok && execution.physical_dag_executed &&
           execution.canonical_result_published &&
           execution.canonical_result_column_count ==
               execution.api_result.result_shape.columns.size() &&
           execution.canonical_result_row_count ==
               execution.api_result.result_shape.rows.size() &&
           post_access_cleanup_once(execution);
  };
  const auto exact_replay_receipt = [&](const auto& execution) {
    const auto counter = std::ranges::find_if(
        execution.api_result.evidence, [](const auto& evidence) {
          return evidence.evidence_kind ==
                 "canonical.time_series_root_causal_counter";
        });
    return atomic_success_cleanup_once(execution) &&
           !execution.selected_plan_uuid.empty() &&
           has_evidence(execution, "canonical.selected_plan",
                        execution.selected_plan_uuid) &&
           counter != execution.api_result.evidence.end() &&
           !counter->evidence_id.empty() && counter->evidence_id != "0";
  };
  const auto rendered_execution = [](const auto& execution) {
    std::ostringstream observed;
    observed << execution.canonical_result_bytes;
    for (const auto& diagnostic : execution.api_result.diagnostics) {
      observed << diagnostic.code << diagnostic.message_key
               << diagnostic.detail;
      for (const auto& field : diagnostic.fields) {
        observed << field.key << field.value;
      }
    }
    for (const auto& evidence : execution.api_result.evidence) {
      observed << evidence.evidence_kind << evidence.evidence_id;
    }
    return observed.str();
  };
  const auto protected_identity_absent = [&](const auto& execution) {
    const auto rendered = rendered_execution(execution);
    return rendered.find(kBaseObjectUuid) == std::string::npos &&
           rendered.find(kPreferredProviderUuid) == std::string::npos;
  };
  const auto rollup_protected_identities_absent =
      [&](const auto& execution, const std::string_view extra = {}) {
    const auto rendered = rendered_execution(execution);
    const std::array<std::string_view, 8> protected_identities{
        kBaseObjectUuid,
        kPreferredProviderUuid,
        rollup_capability_uuid,
        context.transaction_uuid.canonical,
        context.statement_snapshot_uuid.canonical,
        context.statement_metadata_snapshot_uuid.canonical,
        context.authorization_context.authority_uuid.canonical,
        context.catalog_epoch_uuid.canonical,
    };
    return std::ranges::none_of(
               protected_identities, [&](const std::string_view identity) {
                 return !identity.empty() &&
                        rendered.find(identity) != std::string::npos;
               }) &&
           (extra.empty() || rendered.find(extra) == std::string::npos);
  };
  const auto exact_groups = [](const auto& execution,
                               const std::array<std::string_view, 4>& values,
                               const bool real_bits) {
    static constexpr std::array<std::string_view, 4> kMetrics{
        kMetricOneUuid, kMetricOneUuid, kMetricOneUuid, kMetricTwoUuid};
    static constexpr std::array<std::string_view, 4> kBuckets{
        "2026-08-10T12:00:00.000000000Z",
        "2026-08-10T12:01:00.000000000Z",
        "2026-08-10T12:01:00.000000000Z",
        "2026-08-10T12:00:00.000000000Z"};
    static constexpr std::array<std::string_view, 4> kTags{
        "{\"host\":\"a\",\"zone\":\"east\"}",
        "{\"host\":\"a\",\"zone\":\"east\"}",
        "{\"host\":\"a\",\"zone\":\"west\"}",
        "{\"host\":\"a\",\"zone\":\"east\"}"};
    static constexpr std::array<std::string_view, 4> kCounts{"3", "2", "1",
                                                             "1"};
    if (!execution.api_result.ok ||
        execution.api_result.result_shape.rows.size() != 4) {
      return false;
    }
    for (std::size_t row = 0; row < 4; ++row) {
      const auto aggregate =
          ApiRowField(execution.api_result, row, "aggregate_value");
      if (ApiRowField(execution.api_result, row, "series_uuid") !=
              kBaseObjectUuid ||
          ApiRowField(execution.api_result, row, "metric_uuid") !=
              kMetrics[row] ||
          ApiRowField(execution.api_result, row, "bucket_start") !=
              kBuckets[row] ||
          ApiRowField(execution.api_result, row, "tags") != kTags[row] ||
          ApiRowField(execution.api_result, row, "sample_count") !=
              kCounts[row] ||
          (real_bits ? ApiRealBits(aggregate) : aggregate) != values[row]) {
        return false;
      }
    }
    return true;
  };
  const auto exact_rollup_metadata = [&](const auto& metadata) {
    return metadata.family == api::EngineNoSqlProviderFamily::kTimeSeries &&
           metadata.provider_id == kPreferredProviderUuid &&
           metadata.collection_uuid == kBaseObjectUuid &&
           metadata.time_series_rollup_candidate_present &&
           metadata.time_series_rollup_capability_uuid ==
               rollup_capability_uuid &&
           metadata.time_series_rollup_generation == 7 &&
           metadata.time_series_visible_late_arrival_generation == 8 &&
           metadata.time_series_rollup_interval_ns == kMinuteNs &&
           metadata.time_series_rollup_exactness_attestation_state ==
               "TIME_SERIES_ROLLUP_SECTION_8_EXACT_V1" &&
           metadata.time_series_rollup_statement_snapshot_uuid ==
               context.statement_snapshot_uuid.canonical &&
           metadata.time_series_rollup_statement_metadata_snapshot_uuid ==
               context.statement_metadata_snapshot_uuid.canonical &&
           metadata.time_series_rollup_owning_transaction_uuid ==
               context.transaction_uuid.canonical &&
           metadata.time_series_rollup_local_transaction_id ==
               context.local_transaction_id &&
           metadata
                   .time_series_rollup_snapshot_visible_through_local_transaction_id ==
               context.snapshot_visible_through_local_transaction_id &&
           metadata.time_series_rollup_security_context_uuid ==
               context.authorization_context.authority_uuid.canonical &&
           metadata.time_series_rollup_catalog_epoch_uuid ==
               context.catalog_epoch_uuid.canonical &&
           metadata.time_series_rollup_exact_residual_recheck_required &&
           metadata.time_series_rollup_base_row_mga_recheck_required &&
           metadata.time_series_rollup_security_recheck_required &&
           api::ValidateTimeSeriesRollupCapabilityBindingV1(metadata);
  };
  const auto listed_rollup = std::ranges::find_if(
      listed_stale_rollups, exact_rollup_metadata);
  const bool rollup_persistence_exact =
      rollup_binding_known_answer &&
      !refused_mismatched_rollup_publish.ok &&
      uppercase_rollup_derivation_empty &&
      !refused_uppercase_rollup_publish.ok &&
      zero_uuid_rollup_derivation_empty &&
      !refused_zero_uuid_rollup_publish.ok && published_stale_rollup.ok &&
      cleared_rollup_cache.ok &&
      loaded_stale_rollup.ok &&
      exact_rollup_metadata(loaded_stale_rollup.metadata) &&
      listed_rollup != listed_stale_rollups.end() &&
      repaired_stale_rollup.ok &&
      exact_rollup_metadata(repaired_stale_rollup.metadata) &&
      published_rewrite_probe.ok && dropped_rewrite_probe.ok &&
      cleared_rewritten_rollup_cache.ok && loaded_rewritten_rollup.ok &&
      exact_rollup_metadata(loaded_rewritten_rollup.metadata) &&
      std::ranges::find(
          loaded_rewritten_rollup.evidence,
          "provider_generation_time_series_rollup_generation=7") !=
          loaded_rewritten_rollup.evidence.end() &&
      std::ranges::find(
          loaded_rewritten_rollup.evidence,
          "provider_generation_time_series_visible_late_arrival_generation=8") !=
          loaded_rewritten_rollup.evidence.end() &&
      std::ranges::find(
          loaded_rewritten_rollup.evidence,
          "provider_generation_mga_authority=engine_transaction_inventory") !=
          loaded_rewritten_rollup.evidence.end();
  const auto exact_default_rollup_metadata = [](const auto& metadata) {
    return !metadata.time_series_rollup_candidate_present &&
           metadata.time_series_rollup_capability_uuid.empty() &&
           metadata.time_series_rollup_generation == 0 &&
           metadata.time_series_visible_late_arrival_generation == 0 &&
           metadata.time_series_rollup_interval_ns == 0 &&
           metadata.time_series_rollup_exactness_attestation_state.empty() &&
           metadata.time_series_rollup_statement_snapshot_uuid.empty() &&
           metadata.time_series_rollup_statement_metadata_snapshot_uuid.empty() &&
           metadata.time_series_rollup_owning_transaction_uuid.empty() &&
           metadata.time_series_rollup_local_transaction_id == 0 &&
           metadata
                   .time_series_rollup_snapshot_visible_through_local_transaction_id ==
               0 &&
           metadata.time_series_rollup_security_context_uuid.empty() &&
           metadata.time_series_rollup_catalog_epoch_uuid.empty() &&
           !metadata.time_series_rollup_exact_residual_recheck_required &&
           !metadata.time_series_rollup_base_row_mga_recheck_required &&
           !metadata.time_series_rollup_security_recheck_required;
  };
  const bool legacy_rollup_defaults_exact =
      legacy_no_key_physically_omitted && cleared_legacy_no_key_cache.ok &&
      loaded_legacy_no_key.ok &&
      exact_default_rollup_metadata(loaded_legacy_no_key.metadata) &&
      restored_legacy_provider.ok && cleared_legacy_cache.ok &&
      loaded_legacy_provider.ok &&
      exact_default_rollup_metadata(loaded_legacy_provider.metadata);
  const bool legacy_no_key_route_exact =
      exact_groups(legacy_no_key_downsample,
                   {"4020000000000000", "4028000000000000",
                    "4022000000000000", "4000000000000000"},
                   true) &&
      has_evidence(legacy_no_key_downsample,
                   "canonical.time_series_provider_route",
                   "TIME_SERIES_PREFERRED_PROVIDER_V1") &&
      std::ranges::none_of(
          legacy_no_key_downsample.api_result.evidence,
          [](const auto& evidence) {
            return evidence.evidence_kind.starts_with(
                "canonical.time_series_rollup_");
          }) &&
      atomic_success_cleanup_once(legacy_no_key_downsample);
  const bool request_only_rollup_remains_supplemental =
      request_only_rollup_supplement.ok &&
      request_only_rollup_supplement.exact_fallback_observed &&
      !request_only_rollup_supplement.rollup_observed &&
      request_only_rollup_supplement.preferred_access_invocation_count == 0 &&
      request_only_rollup_supplement
              .exact_fallback_access_invocation_count ==
          1;
  const bool persisted_mutation_matrix_exact =
      mutation_fixture_exact &&
      persisted_mutation_observations.size() ==
          persisted_mutation_cases.size() &&
      std::ranges::all_of(
          persisted_mutation_observations, [&](const auto& observation) {
            if (!observation.cache_cleared ||
                !observation.restored_and_revalidated ||
                !observation.execution.has_value()) {
              return false;
            }
            const auto& execution = *observation.execution;
            const bool exact_diagnostic =
                !execution.api_result.diagnostics.empty() &&
                execution.api_result.diagnostics.front().code ==
                    observation.mutation.expected_diagnostic;
            const bool redacted =
                !observation.mutation.security_redaction ||
                rollup_protected_identities_absent(
                    execution,
                    observation.mutation.field ==
                            "time_series_rollup_security_context_uuid"
                        ? std::string_view(
                              observation.mutation.substituted_value)
                        : std::string_view{});
            return exact_diagnostic && atomic_no_root(execution) &&
                   pre_access_cleanup_zero(execution) && redacted;
          });
  if (!persisted_mutation_matrix_exact) {
    std::cerr << "TS-34 mutation fixture exact="
              << (mutation_fixture_exact ? "true" : "false") << '\n';
    for (const auto& observation : persisted_mutation_observations) {
      const bool present = observation.execution.has_value();
      const bool redacted =
          !observation.mutation.security_redaction ||
          (present && rollup_protected_identities_absent(
                          *observation.execution,
                          observation.mutation.field ==
                                  "time_series_rollup_security_context_uuid"
                              ? std::string_view(
                                    observation.mutation.substituted_value)
                              : std::string_view{}));
      const bool exact_diagnostic =
          present && !observation.execution->api_result.diagnostics.empty() &&
          observation.execution->api_result.diagnostics.front().code ==
              observation.mutation.expected_diagnostic;
      if (!observation.cache_cleared ||
          !observation.restored_and_revalidated || !present ||
          !exact_diagnostic ||
          (present && (!atomic_no_root(*observation.execution) ||
                       !pre_access_cleanup_zero(*observation.execution))) ||
          !redacted) {
        std::cerr << "TS-34 mutation " << observation.mutation.label
                  << " expected=" << observation.mutation.expected_diagnostic
                  << " actual="
                  << (present ? route_diagnostic(*observation.execution)
                              : "not-executed")
                  << " cache="
                  << (observation.cache_cleared ? "true" : "false")
                  << " restored="
                  << (observation.restored_and_revalidated ? "true" : "false")
                  << " atomic="
                  << (present && atomic_no_root(*observation.execution)
                          ? "true"
                          : "false")
                  << " cleanup0="
                  << (present &&
                              pre_access_cleanup_zero(*observation.execution)
                          ? "true"
                          : "false")
                  << " redacted=" << (redacted ? "true" : "false")
                  << '\n';
      }
    }
  }
  if (!legacy_rollup_defaults_exact || !legacy_no_key_route_exact ||
      !request_only_rollup_remains_supplemental) {
    std::cerr << "TS-34 legacy defaults="
              << (legacy_rollup_defaults_exact ? "true" : "false")
              << " route=" << (legacy_no_key_route_exact ? "true" : "false")
              << " request-only="
              << (request_only_rollup_remains_supplemental ? "true" : "false")
              << " route-diagnostic="
              << route_diagnostic(legacy_no_key_downsample) << '\n';
  }
  std::set<std::string> observed_cancellation_checkpoints;
  const auto cancellation_at_checkpoint =
      [&](const auto& make_dag,
          const std::string_view expected_checkpoint,
          const bool multileg = false)
      -> std::optional<std::remove_cvref_t<decltype(raw)>> {
    std::size_t probe_count = 0;
    auto probe_context = context;
    probe_context.query_cancellation_requested = [&probe_count] {
      ++probe_count;
      return false;
    };
    const auto probe_dag = make_dag(probe_context);
    const auto probe = multileg
                           ? execute_multileg(probe_context, probe_dag)
                           : sblr::ExecuteCanonicalCurrentHeapQuery(
                                 {probe_context, probe_dag});
    observed_cancellation_checkpoints.insert(
        std::string(expected_checkpoint) + ":probe-count=" +
        std::to_string(probe_count));
    if (!probe.api_result.ok) {
      observed_cancellation_checkpoints.insert(
          std::string(expected_checkpoint) + ":probe-failed=" +
          route_diagnostic(probe));
      return std::nullopt;
    }
    for (std::size_t threshold = 1; threshold <= probe_count; ++threshold) {
      std::size_t observed = 0;
      auto injected_context = context;
      injected_context.query_cancellation_requested =
          [&observed, threshold] { return ++observed == threshold; };
      auto injected_dag = make_dag(injected_context);
      auto injected = multileg
                          ? execute_multileg(injected_context,
                                             std::move(injected_dag))
                          : sblr::ExecuteCanonicalCurrentHeapQuery(
                                {injected_context, std::move(injected_dag)});
      for (const auto& evidence : injected.api_result.evidence) {
        if (evidence.evidence_kind ==
            "canonical.time_series_cancellation_checkpoint") {
          observed_cancellation_checkpoints.insert(evidence.evidence_id);
        }
      }
      if (atomic_no_root(injected) &&
          !injected.api_result.diagnostics.empty() &&
          injected.api_result.diagnostics.front().code ==
              "SB_MODEL_EXECUTION_CANCELLED_V1" &&
          post_access_cleanup_once(injected) &&
          has_evidence(injected,
                       "canonical.time_series_cancellation_checkpoint",
                       expected_checkpoint)) {
        return injected;
      }
    }
    return std::nullopt;
  };
  const auto cancelled_during_scan = cancellation_at_checkpoint(
      [&](const auto& cancellation_context) {
        return TimeSeriesDag(
            cancellation_context, storage,
            api::EngineBoundTimeSeriesAggregateV1::kNone);
      },
      "time-series MGA-visible row read was cancelled");
  const auto cancelled_during_aggregate = cancellation_at_checkpoint(
      [&](const auto& cancellation_context) {
        return TimeSeriesDag(cancellation_context, storage,
                             api::EngineBoundTimeSeriesAggregateV1::kSum);
      },
      "time-series downsample was cancelled");
  const auto cancelled_during_asof = cancellation_at_checkpoint(
      [&](const auto& cancellation_context) {
        return TimeSeriesAsofJoinDag(
            cancellation_context, storage,
            fixture.descriptors.at(std::string(kAsofRightObjectUuid)),
            api::EngineBoundTimeSeriesAggregateV1::kNone, true, true);
      },
      "time-series ASOF bridge was cancelled before execution", true);
  const auto cancelled_before_publish = cancellation_at_checkpoint(
      [&](const auto& cancellation_context) {
        return TimeSeriesDag(
            cancellation_context, storage,
            api::EngineBoundTimeSeriesAggregateV1::kNone);
      },
      "time-series execution was cancelled at final publication");
  std::size_t ordinary_probe_count = 0;
  auto ordinary_probe_context = context;
  ordinary_probe_context.query_cancellation_requested =
      [&ordinary_probe_count] {
        ++ordinary_probe_count;
        return false;
      };
  const auto ordinary_probe = sblr::ExecuteCanonicalCurrentHeapQuery(
      {ordinary_probe_context,
       TimeSeriesFilterProjectDag(ordinary_probe_context, storage)});
  bool ordinary_post_acquisition_failure = false;
  for (std::size_t threshold = 1;
       ordinary_probe.api_result.ok && threshold <= ordinary_probe_count &&
       !ordinary_post_acquisition_failure;
       ++threshold) {
    std::size_t observed = 0;
    auto injected_context = context;
    injected_context.query_cancellation_requested =
        [&observed, threshold] {
          if (++observed == threshold) {
            throw std::runtime_error(
                "injected ordinary post-acquisition probe failure");
          }
          return false;
        };
    const auto injected = sblr::ExecuteCanonicalCurrentHeapQuery(
        {injected_context,
         TimeSeriesFilterProjectDag(injected_context, storage)});
    ordinary_post_acquisition_failure =
        !injected.api_result.ok &&
        !injected.api_result.diagnostics.empty() &&
        injected.api_result.diagnostics.front().code ==
            "SB_MODEL_COORDINATOR_LEG_FAILED_V1" &&
        injected.profile_matched && injected.optimizer_admitted &&
        !injected.physical_dag_executed &&
        !injected.canonical_result_published &&
        injected.canonical_result_column_count == 0 &&
        injected.canonical_result_row_count == 0 &&
        injected.canonical_result_bytes.empty() &&
        has_evidence(injected,
                     "canonical.time_series_source_execution_started",
                     "true") &&
        has_evidence(injected,
                     "canonical.time_series_source_data_access_observed",
                     "true") &&
        has_evidence(injected, "canonical.time_series_cleanup_count", "1") &&
        has_evidence(injected, "canonical.time_series_cleanup_complete",
                     "true");
  }
  const auto join_complete = [](const auto& execution,
                                const std::size_t columns,
                                const std::size_t rows) {
    return execution.profile_matched && execution.optimizer_admitted &&
           execution.optimizer_selected &&
           execution.physical_dag_published &&
           execution.physical_dag_executed &&
           execution.runtime_actuals_attached &&
           execution.canonical_result_published && execution.api_result.ok &&
           execution.optimizer_admission_stage_count == 8 &&
           execution.physical_node_count == 3 &&
           execution.canonical_result_column_count == columns &&
           execution.canonical_result_row_count == rows;
  };
  const bool mixed_joins_complete =
      join_complete(cross_join, 8, 14) &&
      join_complete(inner_join, 8, 1) &&
      join_complete(left_join, 8, 7) &&
      join_complete(right_join, 8, 2) &&
      join_complete(full_join, 8, 8) &&
      join_complete(semi_join, 6, 1) &&
      join_complete(anti_join, 6, 6);
  const bool asof_joins_complete =
      join_complete(raw_series_left_outer, 11, 7) &&
      join_complete(raw_series_left_inner, 11, 4) &&
      join_complete(raw_series_left_columnar, 11, 7) &&
      has_evidence(raw_series_left_columnar,
                   "canonical.model_composition",
                   "time_series_TO_columnar_ONE_ROOT_V1") &&
      has_evidence(raw_series_left_columnar,
                   "canonical.model_join_left_provider_route",
                   "canonical.model-provider.time_series.v1") &&
      has_evidence(raw_series_left_columnar,
                   "canonical.model_join_right_provider_route",
                   "canonical.model-provider.columnar.v1") &&
      has_evidence(raw_series_left_columnar,
                   "canonical.model_join_consumer_route",
                   "canonical.relational.time-series-asof.v1") &&
      has_evidence(raw_series_left_columnar,
                   "canonical.model_join_condition_route",
                   "canonical.relational.asof-key-binding.v1") &&
      join_complete(raw_series_right_outer, 11, 2) &&
      join_complete(raw_series_right_inner, 11, 2) &&
      join_complete(raw_series_right_columnar, 11, 2) &&
      has_evidence(raw_series_right_columnar,
                   "canonical.model_composition",
                   "columnar_TO_time_series_ONE_ROOT_V1") &&
      has_evidence(raw_series_right_columnar,
                   "canonical.model_join_left_provider_route",
                   "canonical.model-provider.columnar.v1") &&
      has_evidence(raw_series_right_columnar,
                   "canonical.model_join_right_provider_route",
                   "canonical.model-provider.time_series.v1") &&
      has_evidence(raw_series_right_columnar,
                   "canonical.model_join_consumer_route",
                   "canonical.relational.time-series-asof.v1") &&
      has_evidence(raw_series_right_columnar,
                   "canonical.model_join_condition_route",
                   "canonical.relational.asof-key-binding.v1") &&
      join_complete(downsample_series_left_outer, 12, 4) &&
      join_complete(downsample_series_left_inner, 12, 1) &&
      join_complete(downsample_series_right_outer, 12, 2) &&
      join_complete(downsample_series_right_inner, 12, 1) &&
      ApiRowField(raw_series_right_outer.api_result, 0,
                  "row_uuid") ==
          "40000000-0000-4000-8000-000000000001" &&
      ApiRowField(raw_series_right_outer.api_result, 1,
                  "row_uuid") ==
          "40000000-0000-4000-8000-000000000003" &&
      ApiRowField(raw_series_left_outer.api_result, 0,
                  "event_timestamp").empty() &&
      ApiRowField(raw_series_left_outer.api_result, 1,
                  "event_timestamp") ==
          "2026-08-10T12:00:10.000000000Z" &&
      ApiRowField(raw_series_left_outer.api_result, 2,
                  "event_timestamp") ==
          "2026-08-10T12:00:40.000000000Z" &&
      ApiRowField(downsample_series_right_outer.api_result, 0,
                  "bucket_start") ==
          "2026-08-10T12:00:00.000000000Z" &&
      ApiRowField(downsample_series_right_outer.api_result, 1,
                  "bucket_start").empty();
  const bool provider_selection_exact =
      published_ambiguous.ok && dropped_ambiguous.ok &&
      dropped_preferred.ok && restored_preferred.ok &&
      has_evidence(raw, "canonical.time_series_provider_route",
                   "TIME_SERIES_PREFERRED_PROVIDER_V1") &&
      ambiguous_fallback.api_result.ok &&
      has_evidence(ambiguous_fallback,
                   "canonical.time_series_provider_route",
                   "TIME_SERIES_BUCKET_STORE_SCAN_EXACT_V1") &&
      ProductionDescriptorStream(ambiguous_fallback) ==
          ProductionDescriptorStream(raw) &&
      ProductionRawStream(ambiguous_fallback) == ProductionRawStream(raw) &&
      fallback_raw.api_result.ok && fallback_sum.api_result.ok &&
      fallback_count.api_result.ok &&
      has_evidence(fallback_raw, "canonical.time_series_provider_route",
                   "TIME_SERIES_BUCKET_STORE_SCAN_EXACT_V1") &&
      has_evidence(fallback_sum, "canonical.time_series_provider_route",
                   "TIME_SERIES_BUCKET_STORE_SCAN_EXACT_V1") &&
      has_evidence(fallback_count, "canonical.time_series_provider_route",
                   "TIME_SERIES_BUCKET_STORE_SCAN_EXACT_V1") &&
      ProductionDescriptorStream(fallback_raw) ==
          ProductionDescriptorStream(raw) &&
      ProductionRawStream(fallback_raw) == ProductionRawStream(raw) &&
      ProductionDescriptorStream(fallback_sum) ==
          ProductionDescriptorStream(sum) &&
      ProductionDownsampleStream(fallback_sum, false) ==
          ProductionDownsampleStream(sum, false) &&
      ProductionDescriptorStream(fallback_count) ==
          ProductionDescriptorStream(count) &&
      ProductionDownsampleStream(fallback_count, true) ==
          ProductionDownsampleStream(count, true);
  if (!provider_selection_exact) {
    const auto provider_route = [&](const auto& execution) {
      return evidence_id(execution, "canonical.time_series_provider_route");
    };
    std::cerr << "QOW-CES05-TIME-SERIES: provider route evidence raw="
              << provider_route(raw)
              << ";ambiguous=" << provider_route(ambiguous_fallback)
              << ";fallback-raw=" << provider_route(fallback_raw)
              << ";fallback-sum=" << provider_route(fallback_sum)
              << ";fallback-count=" << provider_route(fallback_count)
              << ";bytes=" << Digest(raw.canonical_result_bytes) << ','
              << Digest(ambiguous_fallback.canonical_result_bytes) << ','
              << Digest(fallback_raw.canonical_result_bytes) << ','
              << Digest(sum.canonical_result_bytes) << ','
              << Digest(fallback_sum.canonical_result_bytes) << ','
              << Digest(count.canonical_result_bytes) << ','
              << Digest(fallback_count.canonical_result_bytes) << '\n';
  }
  bool passed = Require(direct_multileg_scope_exact,
                        "direct multileg V10 descriptor scope proof failed") &&
         Require(raw.profile_matched && raw.optimizer_admitted &&
                     raw.optimizer_selected && raw.physical_dag_published &&
                     raw.physical_dag_executed && raw.canonical_result_published &&
                     raw.canonical_result_column_count == 6 &&
                     raw.canonical_result_row_count == 7,
                 "ordinary raw production route did not publish seven rows: " +
                     route_diagnostic(raw)) &&
         Require(
             published_ambiguous.ok && dropped_ambiguous.ok &&
                 dropped_preferred.ok && restored_preferred.ok &&
                 has_evidence(raw, "canonical.time_series_provider_route",
                              "TIME_SERIES_PREFERRED_PROVIDER_V1") &&
                 ambiguous_fallback.api_result.ok &&
                 has_evidence(
                     ambiguous_fallback,
                     "canonical.time_series_provider_route",
                     "TIME_SERIES_BUCKET_STORE_SCAN_EXACT_V1") &&
                 ProductionDescriptorStream(ambiguous_fallback) ==
                     ProductionDescriptorStream(raw) &&
                 ProductionRawStream(ambiguous_fallback) ==
                     ProductionRawStream(raw) &&
                 fallback_raw.api_result.ok && fallback_sum.api_result.ok &&
                 fallback_count.api_result.ok &&
                 has_evidence(
                     fallback_raw, "canonical.time_series_provider_route",
                     "TIME_SERIES_BUCKET_STORE_SCAN_EXACT_V1") &&
                 has_evidence(
                     fallback_sum, "canonical.time_series_provider_route",
                     "TIME_SERIES_BUCKET_STORE_SCAN_EXACT_V1") &&
                 has_evidence(
                     fallback_count, "canonical.time_series_provider_route",
                     "TIME_SERIES_BUCKET_STORE_SCAN_EXACT_V1") &&
                 ProductionDescriptorStream(fallback_raw) ==
                     ProductionDescriptorStream(raw) &&
                 ProductionRawStream(fallback_raw) ==
                     ProductionRawStream(raw) &&
                 ProductionDescriptorStream(fallback_sum) ==
                     ProductionDescriptorStream(sum) &&
                 ProductionDownsampleStream(fallback_sum, false) ==
                     ProductionDownsampleStream(sum, false) &&
                 ProductionDescriptorStream(fallback_count) ==
                     ProductionDescriptorStream(count) &&
                 ProductionDownsampleStream(fallback_count, true) ==
                     ProductionDownsampleStream(count, true),
             "ordinary engine-owned provider generation selection/fallback "
             "drifted: " +
                 std::string(published_ambiguous.ok ? "ambiguous-publish-ok"
                                                    : "ambiguous-publish-fail") +
                 "/" +
                 std::string(dropped_ambiguous.ok ? "ambiguous-drop-ok"
                                                  : "ambiguous-drop-fail") +
                 "/" +
                 std::string(dropped_preferred.ok ? "drop-ok" : "drop-fail") +
                 "/" +
                 std::string(restored_preferred.ok ? "restore-ok"
                                                   : "restore-fail") +
                 "/" + route_diagnostic(raw) + "/" +
                 route_diagnostic(ambiguous_fallback) + "/" +
                 route_diagnostic(fallback_raw) + "/" +
                 route_diagnostic(fallback_sum) + "/" +
                 route_diagnostic(fallback_count)) &&
         Require(unavailable.profile_matched && !unavailable.api_result.ok &&
                     !unavailable.physical_dag_executed &&
                     route_diagnostic(unavailable).starts_with(
                         "SB_MODEL_TIME_SERIES_EXACT_FALLBACK_UNAVAILABLE_V1"),
                 "ordinary absent provider/fallback did not refuse exactly: " +
                     route_diagnostic(unavailable)) &&
         Require(sum.profile_matched && sum.optimizer_admitted &&
                     sum.physical_dag_executed &&
                     sum.canonical_result_column_count == 7 &&
                     sum.canonical_result_row_count == 4,
                 "ordinary SUM production route did not publish four groups: " +
                     route_diagnostic(sum)) &&
         Require(count.profile_matched && count.optimizer_admitted &&
                     count.physical_dag_executed &&
                     count.canonical_result_column_count == 7 &&
                     count.canonical_result_row_count == 4,
                 "ordinary COUNT production route did not publish four groups: " +
                     route_diagnostic(count)) &&
         Require(bucket.profile_matched && bucket.optimizer_admitted &&
                     bucket.optimizer_selected &&
                     bucket.physical_dag_published &&
                     bucket.physical_dag_executed &&
                     bucket.runtime_actuals_attached &&
                     bucket.canonical_result_published && bucket.api_result.ok &&
                     bucket.canonical_result_column_count == 1 &&
                     bucket.canonical_result_row_count == 7 &&
                     ApiRowField(bucket.api_result, 1, "bucket_start") ==
                         "2026-08-10T12:00:00.000000000Z",
                 "TS-07 ordinary scalar TIME_BUCKET route drifted: " +
                     route_diagnostic(bucket)) &&
         Require(pre_epoch_bucket.profile_matched &&
                     pre_epoch_bucket.optimizer_admitted &&
                     pre_epoch_bucket.optimizer_selected &&
                     pre_epoch_bucket.physical_dag_published &&
                     pre_epoch_bucket.physical_dag_executed &&
                     pre_epoch_bucket.runtime_actuals_attached &&
                     pre_epoch_bucket.canonical_result_published &&
                     pre_epoch_bucket.api_result.ok &&
                     pre_epoch_bucket.canonical_result_column_count == 1 &&
                     pre_epoch_bucket.canonical_result_row_count == 1 &&
                     ApiRowField(pre_epoch_bucket.api_result, 0,
                                 "bucket_start") ==
                         "1969-12-31T23:59:00.000000000Z",
                 "TS-08 ordinary pre-epoch TIME_BUCKET floor drifted: " +
                     route_diagnostic(pre_epoch_bucket)) &&
         Require(bucket_downsample.profile_matched &&
                     bucket_downsample.optimizer_admitted &&
                     bucket_downsample.physical_dag_executed &&
                     bucket_downsample.canonical_result_published &&
                     bucket_downsample.api_result.ok &&
                     bucket_downsample.canonical_result_column_count == 7 &&
                     bucket_downsample.canonical_result_row_count == 4,
                 "equal evaluated TIME_BUCKET/TIME_DOWNSAMPLE intervals were "
                 "not admitted: " +
                     route_diagnostic(bucket_downsample)) &&
         Require(mismatched_bucket_downsample.profile_matched &&
                     !mismatched_bucket_downsample.api_result.ok &&
                     !mismatched_bucket_downsample.api_result.diagnostics.empty() &&
                     mismatched_bucket_downsample.api_result.diagnostics.front()
                             .code ==
                         "SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1",
                 "mismatched TIME_BUCKET/TIME_DOWNSAMPLE intervals were not "
                 "refused exactly: " +
                     route_diagnostic(mismatched_bucket_downsample)) &&
         Require(day_bucket.profile_matched && day_bucket.api_result.ok &&
                     day_bucket.canonical_result_row_count == 7 &&
                     ApiRowField(day_bucket.api_result, 0, "bucket_start") ==
                         "2026-08-10T00:00:00.000000000Z",
                 "fixed P1D interval was not evaluated as 86400 seconds: " +
                     route_diagnostic(day_bucket)) &&
         Require(std::ranges::all_of(
                     invalid_intervals, [](const auto& execution) {
                       return execution.profile_matched &&
                              !execution.api_result.ok &&
                              !execution.api_result.diagnostics.empty() &&
                              execution.api_result.diagnostics.front().code ==
                                  "SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1";
                     }),
                 "TS-21 zero/negative/calendar/fractional-nanosecond/overflow "
                 "interval refusal matrix drifted") &&
         Require(
             !refused_raw_descriptor.api_result.ok &&
                 !refused_raw_descriptor.physical_dag_executed &&
                 !refused_raw_binding.api_result.ok &&
                 !refused_raw_binding.physical_dag_executed &&
                 !refused_raw_name.api_result.ok &&
                 !refused_raw_name.physical_dag_executed &&
                 !refused_bucket_binding.api_result.ok &&
                 !refused_bucket_binding.physical_dag_executed &&
                 !refused_bucket_name.api_result.ok &&
                 !refused_bucket_name.physical_dag_executed &&
                 !refused_downsample_binding.api_result.ok &&
                 !refused_downsample_binding.physical_dag_executed &&
                 !refused_object.api_result.ok &&
                 !refused_object.physical_dag_executed &&
                 route_diagnostic(refused_raw_descriptor).starts_with(
                     "SB_MODEL_TYPED_EXCHANGE_INVALID_V1") &&
                 route_diagnostic(refused_raw_binding).starts_with(
                     "SB_MODEL_TYPED_EXCHANGE_INVALID_V1") &&
                 route_diagnostic(refused_raw_name).starts_with(
                     "SB_MODEL_TYPED_EXCHANGE_INVALID_V1") &&
                 route_diagnostic(refused_bucket_binding).starts_with(
                     "SB_MODEL_TYPED_EXCHANGE_INVALID_V1") &&
                 route_diagnostic(refused_bucket_name).starts_with(
                     "SB_MODEL_TYPED_EXCHANGE_INVALID_V1") &&
                 route_diagnostic(refused_downsample_binding).starts_with(
                     "SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1") &&
                 route_diagnostic(refused_object).starts_with(
                     "SB_MODEL_TIME_SERIES_RANGE_INVALID_V1"),
             "time-series descriptor/object/bound-column substitution reached "
             "data access: " + route_diagnostic(refused_raw_descriptor) + "/" +
                 route_diagnostic(refused_raw_binding) + "/" +
                 route_diagnostic(refused_raw_name) + "/" +
                 route_diagnostic(refused_bucket_binding) + "/" +
                 route_diagnostic(refused_bucket_name) + "/" +
                 route_diagnostic(refused_downsample_binding) + "/" +
                 route_diagnostic(refused_object)) &&
         Require(composition.profile_matched &&
                     composition.optimizer_admitted &&
                     composition.optimizer_selected &&
                     composition.physical_dag_published &&
                     composition.physical_dag_executed &&
                     composition.runtime_actuals_attached &&
                     composition.canonical_result_published &&
                     composition.physical_node_count == 3 &&
                     composition.canonical_result_column_count == 6 &&
                     composition.canonical_result_row_count == 7 &&
                     composition_replay.api_result.ok &&
                     composition_replay.selected_plan_uuid ==
                         composition.selected_plan_uuid &&
                     composition_replay.canonical_result_bytes ==
                         composition.canonical_result_bytes &&
                     std::ranges::any_of(
                         composition.api_result.evidence,
                         [](const auto& evidence) {
                           return evidence.evidence_kind ==
                                      "canonical.time_series_composition_root" &&
                                  evidence.evidence_id == "3";
                         }),
                 "ordinary FILTER/PROJECT root was not executed: " +
                     route_diagnostic(composition)) &&
         Require(unary.profile_matched && unary.optimizer_admitted &&
                     unary.optimizer_selected &&
                     unary.physical_dag_published &&
                     unary.physical_dag_executed &&
                     unary.runtime_actuals_attached &&
                     unary.canonical_result_published && unary.api_result.ok &&
                     unary.physical_node_count == 5 &&
                     unary.canonical_result_column_count == 7 &&
                     unary.canonical_result_row_count == 7 &&
                     ApiRowField(unary.api_result, 0, "row_number") == "1" &&
                     ApiRowField(unary.api_result, 6, "row_number") == "7" &&
                     std::ranges::any_of(
                         unary.api_result.evidence,
                         [](const auto& evidence) {
                           return evidence.evidence_kind ==
                                      "canonical.time_series_composition_root" &&
                                  evidence.evidence_id == "5";
                         }),
                 "ordinary SORT/WINDOW root was not executed: " +
                     route_diagnostic(unary)) &&
         Require(cte_limit.profile_matched &&
                     cte_limit.optimizer_admitted &&
                     cte_limit.optimizer_selected &&
                     cte_limit.physical_dag_published &&
                     cte_limit.physical_dag_executed &&
                     cte_limit.runtime_actuals_attached &&
                     cte_limit.canonical_result_published &&
                     cte_limit.api_result.ok &&
                     cte_limit.physical_node_count == 5 &&
                     cte_limit.canonical_result_column_count == 6 &&
                     cte_limit.canonical_result_row_count == 3 &&
                     std::ranges::any_of(
                         cte_limit.api_result.evidence,
                         [](const auto& evidence) {
                           return evidence.evidence_kind ==
                                      "canonical.time_series_composition_root" &&
                                  evidence.evidence_id == "5";
                         }),
                 "ordinary CTE/LIMIT root was not executed: " +
                     route_diagnostic(cte_limit)) &&
         Require(counted.profile_matched && counted.optimizer_admitted &&
                     counted.optimizer_selected &&
                     counted.physical_dag_published &&
                     counted.physical_dag_executed &&
                     counted.runtime_actuals_attached &&
                     counted.canonical_result_published &&
                     counted.api_result.ok &&
                     counted.physical_node_count == 2 &&
                     counted.canonical_result_column_count == 1 &&
                     counted.canonical_result_row_count == 1 &&
                     ApiRowField(counted.api_result, 0, "point_count") == "7" &&
                     std::ranges::any_of(
                         counted.api_result.evidence,
                         [](const auto& evidence) {
                           return evidence.evidence_kind ==
                                      "canonical.time_series_composition_root" &&
                                  evidence.evidence_id == "2";
                         }),
                 "ordinary global COUNT(*) root was not executed: " +
                     route_diagnostic(counted)) &&
         Require(recursive.profile_matched &&
                     recursive.optimizer_admitted &&
                     recursive.optimizer_selected &&
                     recursive.physical_dag_published &&
                     recursive.physical_dag_executed &&
                     recursive.runtime_actuals_attached &&
                     recursive.canonical_result_published &&
                     recursive.api_result.ok &&
                     recursive.physical_node_count == 4 &&
                     recursive.canonical_result_column_count == 1 &&
                     recursive.canonical_result_row_count == 3 &&
                     ApiRowField(recursive.api_result, 0, "point_count") ==
                         "7" &&
                     ApiRowField(recursive.api_result, 1, "point_count") ==
                         "8" &&
                     ApiRowField(recursive.api_result, 2, "point_count") ==
                         "9" &&
                     recursive_replay.api_result.ok &&
                     recursive_replay.canonical_result_bytes ==
                         recursive.canonical_result_bytes,
                 "ordinary bounded recursive CTE root was not executed: " +
                     route_diagnostic(recursive)) &&
         Require(set_union.profile_matched &&
                     set_union.optimizer_admitted &&
                     set_union.optimizer_selected &&
                     set_union.physical_dag_published &&
                     set_union.physical_dag_executed &&
                     set_union.runtime_actuals_attached &&
                     set_union.canonical_result_published &&
                     set_union.api_result.ok &&
                     set_union.physical_node_count == 4 &&
                     set_union.canonical_result_column_count == 1 &&
                     set_union.canonical_result_row_count == 8 &&
                     ApiRowField(set_union.api_result, 7, "row_uuid") ==
                         "40000000-0000-4000-8000-000000000099" &&
                     set_replay.api_result.ok &&
                     set_replay.canonical_result_bytes ==
                         set_union.canonical_result_bytes,
                 "ordinary UNION ALL root was not executed: " +
                     route_diagnostic(set_union)) &&
         Require(mixed_joins_complete,
                 "ordinary relational/time-series join matrix was not executed: " +
                     std::string("cross=") + route_diagnostic(cross_join) +
                     ":" + std::to_string(cross_join.canonical_result_row_count) +
                     ";inner=" + route_diagnostic(inner_join) + ":" +
                     std::to_string(inner_join.canonical_result_row_count) +
                     ";left=" + route_diagnostic(left_join) + ":" +
                     std::to_string(left_join.canonical_result_row_count) +
                     ";right=" + route_diagnostic(right_join) + ":" +
                     std::to_string(right_join.canonical_result_row_count) +
                     ";full=" + route_diagnostic(full_join) + ":" +
                     std::to_string(full_join.canonical_result_row_count) +
                     ";semi=" + route_diagnostic(semi_join) + ":" +
                     std::to_string(semi_join.canonical_result_row_count) +
                     ";anti=" + route_diagnostic(anti_join) + ":" +
                     std::to_string(anti_join.canonical_result_row_count)) &&
         Require(asof_joins_complete,
                 "ordinary raw/downsample bidirectional LEFT/INNER ASOF "
                 "matrix was not executed: " +
                     route_diagnostic(raw_series_left_outer) + "/" +
                     route_diagnostic(raw_series_left_inner) + "/" +
                     route_diagnostic(raw_series_left_columnar) + "/" +
                     route_diagnostic(raw_series_right_outer) + "/" +
                     route_diagnostic(raw_series_right_inner) + "/" +
                     route_diagnostic(raw_series_right_columnar) + "/" +
                     route_diagnostic(downsample_series_left_outer) + "/" +
                     route_diagnostic(downsample_series_left_inner) + "/" +
                     route_diagnostic(downsample_series_right_outer) + "/" +
                     route_diagnostic(downsample_series_right_inner) +
                     ";raw-shapes=" +
                     std::to_string(raw_series_left_outer.physical_node_count) +
                     "," +
                     std::to_string(
                         raw_series_left_outer.canonical_result_column_count) +
                     "," +
                     std::to_string(
                         raw_series_left_outer.canonical_result_row_count) +
                     "/" +
                     std::to_string(raw_series_left_inner.canonical_result_row_count) +
                     "/" +
                     std::to_string(raw_series_right_outer.canonical_result_row_count) +
                     "/" +
                     std::to_string(raw_series_right_inner.canonical_result_row_count) +
                     ";raw-fields=" +
                     ApiRowField(raw_series_right_outer.api_result, 0,
                                 "row_uuid") + "," +
                     ApiRowField(raw_series_right_outer.api_result, 1,
                                 "row_uuid") + "," +
                     ApiRowField(raw_series_left_outer.api_result, 0,
                                 "event_timestamp") + "," +
                     ApiRowField(raw_series_left_outer.api_result, 1,
                                 "event_timestamp") + "," +
                     ApiRowField(raw_series_left_outer.api_result, 2,
                                 "event_timestamp"));
  const auto credit = [&](const std::string_view outcome,
                          const bool exact,
                          const std::string_view detail) {
    const bool credited = Require(exact, detail);
    if (credited) completed->insert(std::string(outcome));
    passed = passed && credited;
  };
  credit("TS-01", raw.api_result.ok &&
                      raw.canonical_result_row_count == 7 &&
                      Digest(ProductionRawStream(raw)) ==
                          "bd587d51678f2da5ae92b9a57a0f646bdff6fc750b06d6159ee6179616d0696c" &&
                      ApiRowField(raw.api_result, 0, "row_uuid") ==
                          "40000000-0000-4000-8000-000000000001" &&
                      ApiRowField(raw.api_result, 6, "row_uuid") ==
                          "40000000-0000-4000-8000-000000000007" &&
                      post_access_cleanup_once(raw),
         "TS-01 ordinary raw signed range drifted");
  credit("TS-02", ApiRowField(raw.api_result, 0, "point_timestamp") ==
                      kRangeStart,
         "TS-02 ordinary inclusive start boundary drifted");
  credit("TS-03", std::ranges::none_of(
                      raw.api_result.result_shape.rows,
                      [](const auto& row) {
                        return std::ranges::any_of(
                            row.fields, [](const auto& field) {
                              return field.first == "row_uuid" &&
                                     field.second.encoded_value ==
                                         "40000000-0000-4000-8000-000000000008";
                            });
                      }),
         "TS-03 ordinary exclusive end boundary drifted");
  credit("TS-04", empty_range.api_result.ok &&
                      empty_range.canonical_result_row_count == 0 &&
                      atomic_success_cleanup_once(empty_range),
         "TS-04 ordinary empty range drifted");
  credit("TS-05", offset_range.api_result.ok &&
                      ProductionDescriptorStream(offset_range) ==
                          ProductionDescriptorStream(raw) &&
                      ProductionRawStream(offset_range) ==
                          ProductionRawStream(raw) &&
                      atomic_success_cleanup_once(offset_range),
         "TS-05 ordinary offset-equivalent endpoints drifted");
  credit("TS-06", ApiRowField(raw.api_result, 3, "row_uuid") ==
                          "40000000-0000-4000-8000-000000000004" &&
                      ApiRowField(raw.api_result, 4, "row_uuid") ==
                          "40000000-0000-4000-8000-000000000005" &&
                      ApiRowField(sum.api_result, 1, "sample_count") == "2" &&
                      ApiRealBits(ApiRowField(sum.api_result, 1,
                                              "aggregate_value")) ==
                          "4028000000000000" &&
                      ApiRowField(count.api_result, 1, "sample_count") ==
                          "2" &&
                      ApiRowField(count.api_result, 1,
                                  "aggregate_value") == "2",
         "TS-06 ordinary duplicate-coordinate ordering drifted");
  credit("TS-07", bucket.api_result.ok &&
                      ApiRowField(bucket.api_result, 1, "bucket_start") ==
                          "2026-08-10T12:00:00.000000000Z" &&
                      atomic_success_cleanup_once(bucket),
         "TS-07 ordinary bucket result drifted");
  credit("TS-08", pre_epoch_bucket.api_result.ok &&
                      ApiRowField(pre_epoch_bucket.api_result, 0,
                                  "bucket_start") ==
                          "1969-12-31T23:59:00.000000000Z" &&
                      atomic_success_cleanup_once(pre_epoch_bucket),
         "TS-08 ordinary pre-epoch floor drifted");
  credit("TS-09", exact_groups(
                      sum, {"4020000000000000", "4028000000000000",
                            "4022000000000000", "4000000000000000"}, true) &&
                      Digest(ProductionDownsampleStream(sum, false)) ==
                          "8b5d48cff675200e1abe0e83f9bbb1656a59020de9f05497d85b18571bb66191" &&
                      post_access_cleanup_once(sum),
         "TS-09 ordinary SUM groups drifted");
  credit("TS-10", exact_groups(count, {"3", "2", "1", "1"}, false) &&
                      Digest(ProductionDownsampleStream(count, true)) ==
                          "84d41074a7c4b7fc9b2c09ef32df0b879b24cade6687800adc224d999fde0fce" &&
                      post_access_cleanup_once(count),
         "TS-10 ordinary COUNT groups drifted");
  credit("TS-11", exact_groups(
                       avg, {"4005555555555555", "4018000000000000",
                             "4022000000000000", "4000000000000000"}, true) &&
                      atomic_success_cleanup_once(avg),
         "TS-11 ordinary AVG bits drifted");
  credit("TS-12", exact_groups(
                       minimum, {"3ff0000000000000", "4014000000000000",
                                 "4022000000000000", "4000000000000000"}, true) &&
                      atomic_success_cleanup_once(minimum),
         "TS-12 ordinary MIN groups drifted");
  credit("TS-13", exact_groups(
                       maximum, {"4010000000000000", "401c000000000000",
                                 "4022000000000000", "4000000000000000"}, true) &&
                      atomic_success_cleanup_once(maximum),
         "TS-13 ordinary MAX groups drifted");
  credit("TS-14", empty_bucket.api_result.ok &&
                      empty_bucket.canonical_result_published &&
                      empty_bucket.canonical_result_row_count == 0 &&
                      atomic_success_cleanup_once(empty_bucket),
         "TS-14 ordinary no-gap-fill group cardinality drifted");
  credit("TS-15", ApiRowField(raw.api_result, 0, "tags") ==
                      "{\"host\":\"a\",\"zone\":\"east\"}",
         "TS-15 ordinary canonical tag ordering drifted");
  credit("TS-16", atomic_no_root(duplicate_tag) &&
                      route_diagnostic(duplicate_tag).starts_with(
                          "SB_MODEL_TIME_SERIES_DUPLICATE_TAG_REFUSED_V1") &&
                      post_access_cleanup_once(duplicate_tag),
         "TS-16 ordinary duplicate-tag refusal drifted");
  credit("TS-17", atomic_no_root(nonfinite) &&
                      route_diagnostic(nonfinite).starts_with(
                          "SB_MODEL_TIME_SERIES_VALUE_INVALID_V1") &&
                      post_access_cleanup_once(nonfinite),
         "TS-17 ordinary nonfinite refusal drifted");
  for (std::size_t index = 0; index < invalid_timestamps.size(); ++index) {
    const auto& execution = invalid_timestamps[index];
    if (execution.api_result.ok || !atomic_no_root(execution) ||
        !pre_access_cleanup_zero(execution) ||
        !route_diagnostic(execution).starts_with(
            "SB_MODEL_TIME_SERIES_TIMESTAMP_INVALID_V1")) {
      std::cerr << "TS-18 case=" << index
                << " diagnostic=" << route_diagnostic(execution)
                << " atomic="
                << (atomic_no_root(execution) ? "true" : "false")
                << " cleanup0="
                << (pre_access_cleanup_zero(execution) ? "true" : "false")
                << '\n';
    }
  }
  credit("TS-18", std::ranges::all_of(
                      invalid_timestamps, [&](const auto& execution) {
                        return !execution.api_result.ok &&
                               atomic_no_root(execution) &&
                               pre_access_cleanup_zero(execution) &&
                               route_diagnostic(execution).starts_with(
                                   "SB_MODEL_TIME_SERIES_TIMESTAMP_INVALID_V1");
                      }),
         "TS-18 ordinary malformed timestamp refusal drifted");
  if (!route_diagnostic(reversed_range).starts_with(
          "SB_MODEL_TIME_SERIES_RANGE_INVALID_V1") ||
      !route_diagnostic(wrong_alias).starts_with(
          "SB_MODEL_TIME_SERIES_RANGE_INVALID_V1") ||
      !atomic_no_root(reversed_range) ||
      !pre_access_cleanup_zero(reversed_range) ||
      !atomic_no_root(wrong_alias) || !pre_access_cleanup_zero(wrong_alias)) {
    std::cerr << "TS-19 reversed=" << route_diagnostic(reversed_range)
              << ",atomic="
              << (atomic_no_root(reversed_range) ? "true" : "false")
              << ",cleanup0="
              << (pre_access_cleanup_zero(reversed_range) ? "true" : "false")
              << " wrong-alias=" << route_diagnostic(wrong_alias)
              << ",atomic="
              << (atomic_no_root(wrong_alias) ? "true" : "false")
              << ",cleanup0="
              << (pre_access_cleanup_zero(wrong_alias) ? "true" : "false")
              << '\n';
  }
  credit("TS-19", route_diagnostic(reversed_range).starts_with(
                      "SB_MODEL_TIME_SERIES_RANGE_INVALID_V1") &&
                      route_diagnostic(wrong_alias).starts_with(
                          "SB_MODEL_TIME_SERIES_RANGE_INVALID_V1") &&
                      atomic_no_root(reversed_range) &&
                      pre_access_cleanup_zero(reversed_range) &&
                      atomic_no_root(wrong_alias) &&
                      pre_access_cleanup_zero(wrong_alias) &&
                      frontdoor_passed && frontdoor_outcomes.contains("TS-19"),
         "TS-19 ordinary/frontdoor range refusal drifted");
  credit("TS-20", frontdoor_passed && frontdoor_outcomes.contains("TS-20"),
         "TS-20 ordinary/frontdoor missing-range refusal drifted");
  credit("TS-21", std::ranges::all_of(
                      invalid_intervals, [&](const auto& execution) {
                        return !execution.api_result.ok &&
                               atomic_no_root(execution) &&
                               pre_access_cleanup_zero(execution) &&
                               !execution.api_result.diagnostics.empty() &&
                               execution.api_result.diagnostics.front().code ==
                                   "SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1";
                      }),
         "TS-21 ordinary invalid-interval matrix drifted");
  credit("TS-22", route_diagnostic(unsupported_aggregate).starts_with(
                          "SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1") &&
                      atomic_no_root(unsupported_aggregate) &&
                      pre_access_cleanup_zero(unsupported_aggregate) &&
                      frontdoor_passed && frontdoor_outcomes.contains("TS-22"),
         "TS-22 ordinary/frontdoor aggregate refusal drifted");
  credit("TS-23", frontdoor_passed && frontdoor_outcomes.contains("TS-23"),
         "TS-23 production frontdoor append-source refusal drifted");
  credit("TS-24", frontdoor_passed && frontdoor_outcomes.contains("TS-24"),
         "TS-24 production frontdoor donor-text refusal drifted");
  credit("TS-25", route_diagnostic(stale_catalog).starts_with(
                      "SB_MODEL_CATALOG_GENERATION_STALE_V1") &&
                      atomic_no_root(stale_catalog) &&
                      pre_access_cleanup_zero(stale_catalog),
         std::string("TS-25 ordinary stale catalog refusal drifted: ") +
             route_diagnostic(stale_catalog));
  credit("TS-26", published_stale_provider.ok &&
                      cleared_stale_provider_cache.ok &&
                      stale_provider_fallback.api_result.ok &&
                      ProductionDescriptorStream(stale_provider_fallback) ==
                          ProductionDescriptorStream(raw) &&
                      ProductionRawStream(stale_provider_fallback) ==
                          ProductionRawStream(raw) &&
                      Digest(ProductionRawStream(stale_provider_fallback)) ==
                          "bd587d51678f2da5ae92b9a57a0f646bdff6fc750b06d6159ee6179616d0696c" &&
                      has_evidence(
                          stale_provider_fallback,
                          "canonical.time_series_provider_route",
                          "TIME_SERIES_BUCKET_STORE_SCAN_EXACT_V1") &&
                      post_access_cleanup_once(stale_provider_fallback) &&
                      restored_after_stale_provider.ok &&
                      cleared_restored_provider_cache.ok &&
                      ambiguous_fallback.api_result.ok &&
                      has_evidence(ambiguous_fallback,
                                   "canonical.time_series_provider_route",
                                   "TIME_SERIES_BUCKET_STORE_SCAN_EXACT_V1"),
         "TS-26 ordinary ambiguous/stale provider fallback drifted");
  credit("TS-27", std::ranges::all_of(
                      std::array{
                          std::pair{&refused_raw_descriptor,
                                    "SB_MODEL_TYPED_EXCHANGE_INVALID_V1"},
                          std::pair{&refused_raw_binding,
                                    "SB_MODEL_TYPED_EXCHANGE_INVALID_V1"},
                          std::pair{&refused_raw_name,
                                    "SB_MODEL_TYPED_EXCHANGE_INVALID_V1"},
                          std::pair{&refused_bucket_binding,
                                    "SB_MODEL_TYPED_EXCHANGE_INVALID_V1"},
                          std::pair{&refused_bucket_name,
                                    "SB_MODEL_TYPED_EXCHANGE_INVALID_V1"},
                          std::pair{
                              &refused_downsample_binding,
                              "SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1"},
                          std::pair{&refused_object,
                                    "SB_MODEL_TIME_SERIES_RANGE_INVALID_V1"}},
                      [&](const auto& refusal) {
                        const auto& execution = *refusal.first;
                        return atomic_no_root(execution) &&
                               pre_access_cleanup_zero(execution) &&
                               execution.api_result.diagnostics.size() == 1 &&
                               execution.api_result.diagnostics.front().code ==
                                   refusal.second;
                      }),
         "TS-27 ordinary descriptor/binding/name substitution drifted");
  credit("TS-28", atomic_no_root(denied) &&
                      route_diagnostic(denied).starts_with(
                          "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1") &&
                      pre_access_cleanup_zero(denied) &&
                      protected_identity_absent(denied) &&
                      atomic_no_root(changed_security_generation) &&
                      route_diagnostic(changed_security_generation)
                          .starts_with(
                              "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1") &&
                      pre_access_cleanup_zero(changed_security_generation) &&
                      protected_identity_absent(changed_security_generation),
         std::string("TS-28 ordinary disclosure denial drifted: denied=") +
             route_diagnostic(denied) + ";denied-cleanup0=" +
             (pre_access_cleanup_zero(denied) ? "true" : "false") +
             ";denied-redacted=" +
             (protected_identity_absent(denied) ? "true" : "false") +
             ";generation=" +
             route_diagnostic(changed_security_generation) +
             ";generation-cleanup0=" +
             (pre_access_cleanup_zero(changed_security_generation) ? "true"
                                                                  : "false") +
             ";generation-redacted=" +
             (protected_identity_absent(changed_security_generation) ? "true"
                                                                     : "false"));
  credit("TS-29", route_diagnostic(substituted_mga).starts_with(
                      "SB_MODEL_MGA_CONTEXT_MISMATCH_V1") &&
                      atomic_no_root(substituted_mga) &&
                      pre_access_cleanup_zero(substituted_mga),
         std::string("TS-29 ordinary MGA substitution refusal drifted: ") +
             route_diagnostic(substituted_mga));
  credit("TS-30", atomic_no_root(cancelled) &&
                      route_diagnostic(cancelled).starts_with(
                          "SB_MODEL_EXECUTION_CANCELLED_V1") &&
                      pre_access_cleanup_zero(cancelled) &&
                      cancelled_during_scan.has_value() &&
                      cancelled_during_aggregate.has_value() &&
                      cancelled_during_asof.has_value() &&
                      cancelled_before_publish.has_value(),
         std::string("TS-30 ordinary before/scan/aggregate/ASOF/publication ") +
             "cancellation matrix drifted: before=" +
             (pre_access_cleanup_zero(cancelled) ? "true" : "false") +
             ";scan=" +
             (cancelled_during_scan.has_value() ? "true" : "false") +
             ";aggregate=" +
             (cancelled_during_aggregate.has_value() ? "true" : "false") +
             ";asof=" +
             (cancelled_during_asof.has_value() ? "true" : "false") +
             ";publish=" +
             (cancelled_before_publish.has_value() ? "true" : "false") +
             ";observed=" + [&] {
               std::ostringstream checkpoints;
               for (const auto& checkpoint : observed_cancellation_checkpoints) {
                 checkpoints << '[' << checkpoint << ']';
               }
               return checkpoints.str();
             }());
  credit("TS-31", atomic_no_root(scan_bound_refused) &&
                      route_diagnostic(scan_bound_refused).find(
                          "heap_read_maximum_row_versions_exceeded") !=
                          std::string::npos &&
                      post_access_cleanup_once(scan_bound_refused) &&
                      atomic_no_root(output_bound_refused) &&
                      route_diagnostic(output_bound_refused).find(
                          "time-series raw output row count exceeded its bound") !=
                          std::string::npos &&
                      post_access_cleanup_once(output_bound_refused) &&
                      atomic_no_root(group_bound_refused) &&
                      route_diagnostic(group_bound_refused).find(
                          "time-series group count exceeded its bound") !=
                          std::string::npos &&
                      post_access_cleanup_once(group_bound_refused) &&
                      byte_bound_refused.has_value() &&
                      atomic_no_root(*byte_bound_refused) &&
                      post_access_cleanup_once(*byte_bound_refused) &&
                      combined_peak_refused.has_value() &&
                      atomic_no_root(*combined_peak_refused) &&
                      post_access_cleanup_once(*combined_peak_refused),
         "TS-31 ordinary count/byte/group/memory refusal matrix drifted: " +
             route_diagnostic(scan_bound_refused) + "/" +
             route_diagnostic(output_bound_refused) + "/" +
             route_diagnostic(group_bound_refused) +
             ";byte=" +
             (byte_bound_refused.has_value()
                  ? route_diagnostic(*byte_bound_refused)
                  : std::string("absent")) +
             ";combined=" +
             (combined_peak_refused.has_value()
                  ? route_diagnostic(*combined_peak_refused)
                  : std::string("absent")) +
             ";byte-attempts=" + byte_bound_attempts.str() +
             ";combined-attempts=" + combined_peak_attempts.str());
  credit("TS-32", fallback_raw.api_result.ok && fallback_sum.api_result.ok &&
                      fallback_count.api_result.ok &&
                      ProductionDescriptorStream(fallback_raw) ==
                          ProductionDescriptorStream(raw) &&
                      ProductionDescriptorStream(fallback_sum) ==
                          ProductionDescriptorStream(sum) &&
                      ProductionDescriptorStream(fallback_count) ==
                          ProductionDescriptorStream(count) &&
                      ProductionRawStream(fallback_raw) ==
                          ProductionRawStream(raw) &&
                      ProductionDownsampleStream(fallback_sum, false) ==
                          ProductionDownsampleStream(sum, false) &&
                      ProductionDownsampleStream(fallback_count, true) ==
                          ProductionDownsampleStream(count, true) &&
                      Digest(ProductionRawStream(fallback_raw)) ==
                          "bd587d51678f2da5ae92b9a57a0f646bdff6fc750b06d6159ee6179616d0696c" &&
                      Digest(ProductionDownsampleStream(fallback_sum,
                                                        false)) ==
                          "8b5d48cff675200e1abe0e83f9bbb1656a59020de9f05497d85b18571bb66191" &&
                      Digest(ProductionDownsampleStream(fallback_count,
                                                        true)) ==
                          "84d41074a7c4b7fc9b2c09ef32df0b879b24cade6687800adc224d999fde0fce" &&
                      post_access_cleanup_once(fallback_raw) &&
                      post_access_cleanup_once(fallback_sum) &&
                      post_access_cleanup_once(fallback_count),
         "TS-32 ordinary exact fallback equivalence drifted");
  credit("TS-33", unavailable.profile_matched && !unavailable.api_result.ok &&
                      route_diagnostic(unavailable).starts_with(
                          "SB_MODEL_TIME_SERIES_EXACT_FALLBACK_UNAVAILABLE_V1") &&
                      atomic_no_root(unavailable) &&
                      pre_access_cleanup_zero(unavailable),
         "TS-33 ordinary absent provider/fallback refusal drifted");
  credit("TS-34", rollup_persistence_exact &&
                      legacy_rollup_defaults_exact &&
                      legacy_no_key_route_exact &&
                      request_only_rollup_remains_supplemental &&
                      persisted_mutation_matrix_exact &&
                      exact_groups(
                          stale_rollup_sum,
                          {"4020000000000000", "4028000000000000",
                           "4022000000000000", "4000000000000000"},
                          true) &&
                      exact_groups(stale_rollup_count,
                                   {"3", "2", "1", "1"}, false) &&
                      Digest(ProductionDownsampleStream(stale_rollup_sum,
                                                        false)) ==
                          "8b5d48cff675200e1abe0e83f9bbb1656a59020de9f05497d85b18571bb66191" &&
                      Digest(ProductionDownsampleStream(stale_rollup_count,
                                                        true)) ==
                          "84d41074a7c4b7fc9b2c09ef32df0b879b24cade6687800adc224d999fde0fce" &&
                      has_evidence(stale_rollup_sum,
                                   "canonical.time_series_rollup_candidate",
                                   rollup_capability_uuid) &&
                      has_evidence(stale_rollup_sum,
                                   "canonical.time_series_rollup_generation",
                                   "7") &&
                      has_evidence(
                          stale_rollup_sum,
                          "canonical.time_series_visible_late_arrival_generation",
                          "8") &&
                      has_evidence(stale_rollup_sum,
                                   "canonical.time_series_rollup_rejection",
                                   "stale_generation") &&
                      has_evidence(
                          stale_rollup_sum,
                          "canonical.time_series_rollup_request_derivation",
                          "engine_loaded_provider_generation_metadata") &&
                      has_evidence(
                          stale_rollup_sum,
                          "canonical.time_series_provider_route",
                          "TIME_SERIES_BUCKET_STORE_SCAN_EXACT_V1") &&
                      has_evidence(stale_rollup_sum,
                                   "canonical.time_series_cleanup_count",
                                   "1") &&
                      has_evidence(stale_rollup_sum,
                                   "canonical.time_series_cleanup_complete",
                                   "true") &&
                      atomic_success_cleanup_once(stale_rollup_sum) &&
                      atomic_success_cleanup_once(stale_rollup_count),
         "TS-34 persisted stale-rollup rejection/exact fallback drifted");
  credit("TS-35", join_complete(raw_series_right_outer, 11, 2) &&
                      ApiRowField(raw_series_right_outer.api_result, 0,
                                  "row_uuid") ==
                          "40000000-0000-4000-8000-000000000001" &&
                      ApiRowField(raw_series_right_outer.api_result, 1,
                                  "row_uuid") ==
                          "40000000-0000-4000-8000-000000000003" &&
                      std::ranges::all_of(
                          std::array{&raw_series_right_outer,
                                     &raw_series_right_inner,
                                     &downsample_series_right_outer,
                                     &downsample_series_right_inner},
                          [&](const auto* execution) {
                            return atomic_success_cleanup_once(*execution);
                          }),
         "TS-35 ordinary relational-left ASOF matches drifted");
  credit("TS-36", join_complete(raw_series_left_outer, 11, 7) &&
                      ApiRowField(raw_series_left_outer.api_result, 0,
                                  "event_timestamp").empty() &&
                      ApiRowField(raw_series_left_outer.api_result, 1,
                                  "event_timestamp") ==
                          "2026-08-10T12:00:10.000000000Z" &&
                      ApiRowField(raw_series_left_outer.api_result, 2,
                                  "event_timestamp") ==
                          "2026-08-10T12:00:40.000000000Z" &&
                      std::ranges::all_of(
                          std::array{&raw_series_left_outer,
                                     &raw_series_left_inner,
                                     &downsample_series_left_outer,
                                     &downsample_series_left_inner},
                          [&](const auto* execution) {
                            return atomic_success_cleanup_once(*execution);
                          }),
         "TS-36 ordinary time-series-left ASOF matches drifted");
  credit("TS-37", composition.api_result.ok && unary.api_result.ok &&
                      cte_limit.api_result.ok && counted.api_result.ok &&
                      recursive.api_result.ok && set_union.api_result.ok &&
                      mixed_joins_complete &&
                      std::ranges::all_of(
                          std::array{&composition, &unary, &cte_limit,
                                     &counted, &recursive, &set_union,
                                     &cross_join, &inner_join, &left_join,
                                     &right_join, &full_join, &semi_join,
                                     &anti_join},
                          [&](const auto* execution) {
                            return atomic_success_cleanup_once(*execution);
                          }),
         "TS-37 ordinary canonical execution-spine matrix drifted");
  credit("TS-38", std::ranges::all_of(
                      std::array{&raw_replay, &raw_replay_second,
                                 &sum_replay, &sum_replay_second,
                                 &count_replay, &count_replay_second,
                                 &fallback_raw, &fallback_raw_replay,
                                 &fallback_sum, &fallback_sum_replay,
                                 &fallback_count, &fallback_count_replay,
                                 &raw_series_right_outer,
                                 &raw_series_right_replay, &composition,
                                 &composition_replay, &set_union, &set_replay,
                                 &recursive, &recursive_replay},
                      [&](const auto* execution) {
                        return exact_replay_receipt(*execution);
                      }) &&
                      ProductionResultProofStream(raw_replay) ==
                          ProductionResultProofStream(raw_replay_second) &&
                      ProductionResultProofStream(sum_replay) ==
                          ProductionResultProofStream(sum_replay_second) &&
                      ProductionResultProofStream(count_replay) ==
                          ProductionResultProofStream(count_replay_second) &&
                      ProductionResultProofStream(fallback_raw) ==
                          ProductionResultProofStream(fallback_raw_replay) &&
                      ProductionResultProofStream(fallback_sum) ==
                          ProductionResultProofStream(fallback_sum_replay) &&
                      ProductionResultProofStream(fallback_count) ==
                          ProductionResultProofStream(fallback_count_replay) &&
                      ProductionResultProofStream(raw_series_right_outer) ==
                          ProductionResultProofStream(
                              raw_series_right_replay) &&
                      ProductionResultProofStream(composition) ==
                          ProductionResultProofStream(composition_replay) &&
                      ProductionResultProofStream(set_union) ==
                          ProductionResultProofStream(set_replay) &&
                      ProductionResultProofStream(recursive) ==
                          ProductionResultProofStream(recursive_replay),
         "TS-38 ordinary deterministic replay drifted");
  credit("TS-39", invisible.api_result.ok &&
                      invisible.canonical_result_row_count == 0 &&
                      atomic_success_cleanup_once(invisible),
         "TS-39 ordinary MGA-invisible invalid row was observed");
  credit("TS-40", ordinary_post_acquisition_failure,
         "TS-40 ordinary post-acquisition failure/cleanup receipt drifted");
  return passed;
}

void BindPublishedNodeContexts(exec::TypedPhysicalNodeDag* dag) {
  for (auto& node : dag->nodes) {
    node.selected_alternative_uuid = NewUuidText(platform::UuidKind::object);
    node.executor_capability_uuid = NewUuidText(platform::UuidKind::object);
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid = NewUuidText(platform::UuidKind::object);
    node.memory_bytes_required = 4096;
    node.engine_capability_validated = true;
    node.mga_statement_context = dag->mga_statement_context;
  }
}

exec::TypedPhysicalNodeDag AsofDag(
    const api::EngineRequestContext& context,
    std::vector<std::uint32_t> left_descriptors,
    std::vector<std::uint32_t> right_descriptors,
    const bool left_outer = true,
    const std::int64_t tolerance_ns = 30'000'000'000LL,
    const std::uint64_t maximum_comparisons = 65'536,
    const exec::CanonicalTimeSeriesAsofInputBindingV1 left_binding = {},
    const exec::CanonicalTimeSeriesAsofInputBindingV1 right_binding = {},
    const std::uint64_t maximum_output_rows = 16) {
  exec::TypedPhysicalNodeDag dag;
  dag.abi_version = 2;
  dag.selected_plan_uuid = NewUuidText(platform::UuidKind::object);
  dag.root_physical_node_id = 3;
  dag.local_transaction_id = context.local_transaction_id;
  dag.statement_snapshot_id =
      context.snapshot_visible_through_local_transaction_id;
  dag.mga_statement_context = PhysicalMga(context);
  dag.bound_sblr_tree_uuid = NewUuidText(platform::UuidKind::object);
  dag.catalog_epoch_uuid = context.catalog_epoch_uuid.canonical;
  dag.security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  dag.capability_snapshot_uuid = NewUuidText(platform::UuidKind::object);
  dag.resource_snapshot_uuid = NewUuidText(platform::UuidKind::object);
  dag.statistics_snapshot_uuid = NewUuidText(platform::UuidKind::object);
  dag.route_snapshot_uuid = NewUuidText(platform::UuidKind::object);
  dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest, dag.bound_sblr_tree_uuid},
      {exec::PhysicalAdmissionStage::kCatalogEpoch, dag.catalog_epoch_uuid},
      {exec::PhysicalAdmissionStage::kSecurity, dag.security_context_uuid},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       dag.mga_statement_context.statement_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       dag.capability_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kResource, dag.resource_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       dag.statistics_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kCanonicalRoute, dag.route_snapshot_uuid},
  };
  auto output = left_descriptors;
  output.insert(output.end(), right_descriptors.begin(), right_descriptors.end());
  exec::PhysicalNodeRecord left;
  left.physical_node_id = 1;
  left.relational_node_id = 1;
  left.node_kind = exec::PhysicalNodeKind::kScan;
  left.implementation_id = "scan.asof-left.typed.v1";
  left.output_descriptor_ids = std::move(left_descriptors);
  left.causal_counter_id = 1;
  exec::PhysicalNodeRecord right;
  right.physical_node_id = 2;
  right.relational_node_id = 2;
  right.node_kind = exec::PhysicalNodeKind::kScan;
  right.implementation_id = "scan.asof-right.typed.v1";
  right.output_descriptor_ids = std::move(right_descriptors);
  right.causal_counter_id = 2;
  exec::PhysicalNodeRecord join;
  join.physical_node_id = 3;
  join.relational_node_id = 3;
  join.node_kind = exec::PhysicalNodeKind::kJoin;
  join.implementation_id = left_outer ? "join.asof.left.typed.v1"
                                      : "join.asof.inner.typed.v1";
  join.input_physical_node_ids = {1, 2};
  join.output_descriptor_ids = std::move(output);
  join.causal_counter_id = 3;
  join.logical_semantic_variant_id = left_outer ? "join.asof.left.v1"
                                                : "join.asof.inner.v1";
  join.transformation_uuid = NewUuidText(platform::UuidKind::object);
  exec::CanonicalTimeSeriesAsofJoinRequestV1 receipt;
  receipt.tolerance_ns = tolerance_ns;
  receipt.maximum_comparisons = maximum_comparisons;
  receipt.maximum_output_rows = maximum_output_rows;
  receipt.left_outer = left_outer;
  receipt.left_binding = left_binding;
  receipt.right_binding = right_binding;
  join.transformation_rule_id =
      exec::CanonicalTimeSeriesAsofTransformationReceiptV1(receipt);
  dag.nodes = {std::move(left), std::move(right), std::move(join)};
  dag.catalog_generation = context.catalog_generation_id;
  dag.security_epoch = context.security_epoch;
  dag.policy_epoch = context.authorization_context.policy_epoch;
  dag.resource_epoch = context.resource_epoch;
  dag.statistics_generation = context.catalog_generation_id;
  dag.route_epoch = 80;
  dag.route_generation = 81;
  dag.memory_budget_bytes = 16 * 1024 * 1024;
  dag.optimizer_published = true;
  dag.immutable_node_identity_validated = true;
  dag.capability_validated_before_access = true;
  BindPublishedNodeContexts(&dag);
  return dag;
}

exec::CanonicalExecutionMgaAuthority AsofAuthority(
    const exec::TypedPhysicalNodeDag& dag) {
  exec::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = dag.mga_statement_context;
  authority.origin = exec::CanonicalMgaAuthorityOrigin::kEngineTransactionInventory;
  const auto current = authority.statement_context;
  authority.resolve_current = [current] {
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = current;
    return resolution;
  };
  return authority;
}

exec::DescriptorBatch RelationalTimestampBatch(
    const api::EngineDescriptor& timestamp_descriptor,
    const std::uint32_t descriptor_id,
    const bool nullable,
    const std::vector<std::string>& timestamps) {
  exec::DescriptorBatch batch;
  auto descriptor = timestamp_descriptor;
  descriptor.descriptor_uuid.canonical = NewUuidText(platform::UuidKind::object);
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "timestamp_tz";
  descriptor.encoded_descriptor =
      "canonical=timestamp_tz;type_uuid=" + CoreTypeUuid("timestamp") +
      ";nullable=" + (nullable ? std::string("true") : std::string("false"));
  batch.columns.push_back({"event_timestamp", descriptor, nullable,
                           descriptor_id});
  auto metric_descriptor =
      DerivedDescriptor(NewUuidText(platform::UuidKind::object), "uuid",
                        "uuid");
  metric_descriptor.descriptor_kind = "scalar";
  auto tags_descriptor =
      DerivedDescriptor(NewUuidText(platform::UuidKind::object), "text",
                        "character");
  tags_descriptor.descriptor_kind = "scalar";
  batch.columns.push_back(
      {"metric_uuid", metric_descriptor, false, descriptor_id + 1});
  batch.columns.push_back(
      {"tags", tags_descriptor, false, descriptor_id + 2});
  for (const auto& timestamp : timestamps) {
    exec::DescriptorTuple row;
    row.values = {
        TypedValue(descriptor, timestamp),
        TypedValue(metric_descriptor, std::string(kMetricOneUuid)),
        TypedValue(tags_descriptor,
                   "{\"host\":\"a\",\"zone\":\"east\"}"),
    };
    batch.rows.push_back(std::move(row));
  }
  return batch;
}

exec::CanonicalTimeSeriesAsofInputBindingV1 RelationalAsofBinding(
    const std::uint32_t descriptor_id) {
  exec::CanonicalTimeSeriesAsofInputBindingV1 binding;
  binding.timestamp_expression_id = descriptor_id * 10;
  binding.metric_expression_id = descriptor_id * 10 + 1;
  binding.tags_expression_id = descriptor_id * 10 + 2;
  binding.timestamp_descriptor_id = descriptor_id;
  binding.metric_descriptor_id = descriptor_id + 1;
  binding.tags_descriptor_id = descriptor_id + 2;
  binding.timestamp_column_ordinal = 0;
  binding.metric_column_ordinal = 1;
  binding.tags_column_ordinal = 2;
  return binding;
}

exec::CanonicalTimeSeriesAsofInputBindingV1 RawAsofBinding() {
  exec::CanonicalTimeSeriesAsofInputBindingV1 binding;
  binding.metric_expression_id = 3;
  binding.tags_expression_id = 5;
  binding.timestamp_expression_id = 4;
  binding.row_uuid_expression_id = 1;
  binding.metric_descriptor_id = 103;
  binding.tags_descriptor_id = 105;
  binding.timestamp_descriptor_id = 104;
  binding.row_uuid_descriptor_id = 101;
  binding.metric_column_ordinal = 2;
  binding.tags_column_ordinal = 4;
  binding.timestamp_column_ordinal = 3;
  binding.row_uuid_column_ordinal = 0;
  binding.raw_time_series = true;
  return binding;
}

exec::CanonicalTimeSeriesAsofInputBindingV1 DownsampleAsofBinding() {
  exec::CanonicalTimeSeriesAsofInputBindingV1 binding;
  binding.metric_expression_id = 2;
  binding.tags_expression_id = 5;
  binding.timestamp_expression_id = 3;
  binding.metric_descriptor_id = 202;
  binding.tags_descriptor_id = 205;
  binding.timestamp_descriptor_id = 203;
  binding.metric_column_ordinal = 1;
  binding.tags_column_ordinal = 4;
  binding.timestamp_column_ordinal = 2;
  binding.downsample_time_series = true;
  return binding;
}

bool AsofMatrix(const Fixture& fixture,
                const api::EngineRequestContext& context,
                std::set<std::string>* completed) {
  const auto batch_stream = [](const exec::DescriptorBatch& batch) {
    std::ostringstream out;
    for (const auto& column : batch.columns) {
      out << column.descriptor_id << ':' << column.stable_name << ':'
          << column.descriptor.descriptor_uuid.canonical << '\n';
    }
    for (const auto& row : batch.rows) {
      for (const auto& value : row.values) {
        out << static_cast<unsigned>(value.state) << ':'
            << value.descriptor.descriptor_uuid.canonical << ':'
            << value.encoded_value << ';';
      }
      out << '\n';
    }
    return out.str();
  };
  const auto& base = fixture.descriptors.at(std::string(kBaseObjectUuid));
  const auto raw = ExecuteThroughCommonSpine(
      Request(context, base,
              api::EngineBoundTimeSeriesReadOperationV1::kRangeRead,
              api::EngineBoundTimeSeriesAggregateV1::kNone),
      base);
  if (!raw.result.accepted) {
    return Require(false, "ASOF source did not cross the model exchange");
  }
  const std::string east = "{\"host\":\"a\",\"zone\":\"east\"}";
  const auto key = [&](const std::int64_t timestamp_ns) {
    return exec::CanonicalTimeSeriesAsofKeyV1{
        std::string(kMetricOneUuid), east, timestamp_ns};
  };
  const auto seconds = [](const std::int64_t value) {
    return 1'786'363'200'000'000'000LL + value * 1'000'000'000LL;
  };

  auto relational_left = RelationalTimestampBatch(
      base.columns[1].value_descriptor, 301, false,
      {"2026-08-10T11:59:00.000000000Z",
       "2026-08-10T12:00:20.000000000Z",
       "2026-08-10T12:00:46.000000000Z"});
  auto right_raw = raw.result.output.batch;
  const auto relational_301_binding = RelationalAsofBinding(301);
  const auto raw_binding = RawAsofBinding();
  auto dag35 = AsofDag(context, {301, 302, 303},
                       {101, 102, 103, 104, 105, 106}, true,
                       30'000'000'000LL, 65'536,
                       relational_301_binding, raw_binding);
  exec::CanonicalTimeSeriesAsofJoinRequestV1 request35;
  request35.physical_dag = dag35;
  request35.selected_physical_node_id = 3;
  request35.left_batch = std::move(relational_left);
  request35.right_batch = right_raw;
  request35.left_keys = {key(seconds(-60)), key(seconds(20)),
                         key(seconds(46))};
  for (const auto& identity : raw.result.output.ordered_row_identities) {
    request35.right_keys.push_back(
        {identity.metric_uuid, identity.tags, identity.point_timestamp_ns});
    request35.right_tie_break_row_uuids.push_back(identity.row_uuid);
  }
  request35.tolerance_ns = 30'000'000'000LL;
  request35.maximum_output_rows = 16;
  request35.maximum_comparisons = 65'536;
  request35.maximum_memory_bytes = 16 * 1024 * 1024;
  request35.right_is_time_series_raw = true;
  request35.left_binding = relational_301_binding;
  request35.right_binding = raw_binding;
  request35.mga_authority = AsofAuthority(dag35);
  request35.cancellation_requested = [] { return false; };
  const auto result35 = exec::ExecuteCanonicalTimeSeriesAsofJoinV1(request35);
  const auto replay35 = exec::ExecuteCanonicalTimeSeriesAsofJoinV1(request35);
  auto asof_before_cancel = request35;
  asof_before_cancel.cancellation_requested = [] { return true; };
  const auto asof_before_cancelled =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(asof_before_cancel);
  std::size_t asof_checkpoint_count = 0;
  auto asof_probe = request35;
  asof_probe.cancellation_requested = [&asof_checkpoint_count] {
    ++asof_checkpoint_count;
    return false;
  };
  const auto asof_probe_result =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(asof_probe);
  bool final_publication_cancelled = false;
  for (std::size_t threshold = 1;
       threshold <= asof_checkpoint_count && !final_publication_cancelled;
       ++threshold) {
    std::size_t observed = 0;
    auto final_cancel = request35;
    final_cancel.cancellation_requested = [&observed, threshold] {
      return ++observed >= threshold;
    };
    const auto refusal =
        exec::ExecuteCanonicalTimeSeriesAsofJoinV1(final_cancel);
    final_publication_cancelled =
        !refusal.diagnostic.ok &&
        refusal.diagnostic.diagnostic_code ==
            "SB_MODEL_EXECUTION_CANCELLED_V1" &&
        refusal.diagnostic.detail ==
            "ASOF join was cancelled at final publication" &&
        refusal.output_batch.rows.empty() &&
        refusal.output_batch.columns.empty() &&
        refusal.matched_right_ordinals.empty();
  }
  std::size_t asof_during_checks = 0;
  auto asof_during_cancel = request35;
  asof_during_cancel.cancellation_requested = [&asof_during_checks] {
    return ++asof_during_checks >= 2;
  };
  const auto asof_during_cancelled =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(asof_during_cancel);
  auto disposition_substitution = request35;
  disposition_substitution.left_outer = false;
  const auto disposition_refused =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(disposition_substitution);
  auto tolerance_substitution = request35;
  ++tolerance_substitution.tolerance_ns;
  const auto tolerance_refused =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(tolerance_substitution);
  auto output_bound_enlargement = request35;
  ++output_bound_enlargement.maximum_output_rows;
  const auto output_bound_enlargement_refused =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(output_bound_enlargement);
  auto memory_bound_enlargement = request35;
  ++memory_bound_enlargement.maximum_memory_bytes;
  const auto memory_bound_enlargement_refused =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(memory_bound_enlargement);
  auto dynamic_memory = request35;
  dynamic_memory.maximum_memory_bytes = 64 * 1024;
  dynamic_memory.physical_dag.memory_budget_bytes =
      dynamic_memory.maximum_memory_bytes;
  dynamic_memory.right_batch.rows.front().values.back().binary_value.assign(
      256 * 1024, std::uint8_t{0x7f});
  const auto dynamic_memory_refused =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(dynamic_memory);
  auto key_substitution = request35;
  ++key_substitution.right_keys.front().timestamp_ns;
  const auto key_substitution_refused =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(key_substitution);
  auto tie_substitution = request35;
  tie_substitution.right_tie_break_row_uuids.front() =
      "40000000-0000-4000-8000-000000000099";
  const auto tie_substitution_refused =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(tie_substitution);
  auto duplicate_raw_identity = request35;
  duplicate_raw_identity.right_batch.rows[1].values[0].encoded_value =
      duplicate_raw_identity.right_batch.rows[0].values[0].encoded_value;
  duplicate_raw_identity.right_tie_break_row_uuids[1] =
      duplicate_raw_identity.right_tie_break_row_uuids[0];
  const auto duplicate_raw_identity_refused =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(duplicate_raw_identity);
  auto expression_substitution = request35;
  ++expression_substitution.left_binding.timestamp_expression_id;
  const auto expression_substitution_refused =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(expression_substitution);
  auto ordinal_substitution = request35;
  std::swap(ordinal_substitution.left_binding.metric_column_ordinal,
            ordinal_substitution.left_binding.tags_column_ordinal);
  const auto ordinal_substitution_refused =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(ordinal_substitution);
  const auto refresh_asof_receipt = [](auto* request) {
    request->physical_dag.nodes.back().transformation_rule_id =
        exec::CanonicalTimeSeriesAsofTransformationReceiptV1(*request);
  };
  auto aliased_raw_binding = request35;
  aliased_raw_binding.right_binding.row_uuid_expression_id =
      aliased_raw_binding.right_binding.metric_expression_id;
  aliased_raw_binding.right_binding.row_uuid_descriptor_id =
      aliased_raw_binding.right_binding.metric_descriptor_id;
  aliased_raw_binding.right_binding.row_uuid_column_ordinal =
      aliased_raw_binding.right_binding.metric_column_ordinal;
  refresh_asof_receipt(&aliased_raw_binding);
  const auto aliased_raw_binding_refused =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(aliased_raw_binding);
  auto nonraw_row_ordinal = request35;
  nonraw_row_ordinal.left_binding.row_uuid_column_ordinal = 1;
  refresh_asof_receipt(&nonraw_row_ordinal);
  const auto nonraw_row_ordinal_refused =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(nonraw_row_ordinal);
  auto family_substitution = request35;
  family_substitution.left_binding.raw_time_series = true;
  const auto family_substitution_refused =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(family_substitution);
  auto oversized_relational_tags = request35;
  oversized_relational_tags.maximum_memory_bytes = 64 * 1024;
  oversized_relational_tags.physical_dag.memory_budget_bytes =
      oversized_relational_tags.maximum_memory_bytes;
  oversized_relational_tags.left_batch.rows.front().values[2].encoded_value =
      "{\"label\":\"" + std::string(256 * 1024, 'a') + "\"}";
  oversized_relational_tags.left_keys.front().canonical_tags =
      oversized_relational_tags.left_batch.rows.front().values[2]
          .encoded_value;
  const auto oversized_relational_tags_refused =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(
          oversized_relational_tags);
  auto substituted_current_authority = request35;
  auto changed_current_context =
      substituted_current_authority.mga_authority.statement_context;
  changed_current_context.statement_timestamp =
      "2026-08-10T12:00:00.000000001Z";
  substituted_current_authority.mga_authority.resolve_current =
      [changed_current_context] {
        exec::CanonicalMgaCurrentResolution resolution;
        resolution.statement_context = changed_current_context;
        return resolution;
      };
  const auto substituted_current_authority_refused =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(
          substituted_current_authority);
  auto comparison_bounded = request35;
  comparison_bounded.maximum_comparisons = 1;
  comparison_bounded.physical_dag.nodes.back().transformation_rule_id =
      exec::CanonicalTimeSeriesAsofTransformationReceiptV1(
          comparison_bounded);
  const auto comparison_refused =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(comparison_bounded);
  bool passed = Require(
      result35.diagnostic.ok && result35.output_batch.rows.size() == 3 &&
          result35.matched_right_ordinals ==
              std::vector<std::int64_t>{-1, 0, 2} &&
          result35.output_batch.rows[0].values[3].state ==
              api::EngineValueState::sql_null &&
          result35.output_batch.rows[1].values[3].encoded_value ==
              "40000000-0000-4000-8000-000000000001" &&
          result35.output_batch.rows[2].values[3].encoded_value ==
              "40000000-0000-4000-8000-000000000003" &&
          std::ranges::all_of(
              result35.output_batch.columns |
                  std::views::drop(request35.left_batch.columns.size()),
              [](const auto& column) {
                return column.nullable &&
                       (column.descriptor.encoded_descriptor.find(
                            "nullable=true") != std::string::npos ||
                        column.descriptor.encoded_descriptor.find(
                            "nullability=nullable") != std::string::npos);
              }) &&
          std::ranges::all_of(result35.output_batch.rows,
                              [&](const auto& row) {
                                for (std::size_t ordinal =
                                         request35.left_batch.columns.size();
                                     ordinal < row.values.size(); ++ordinal) {
                                  if (row.values[ordinal].descriptor
                                          .encoded_descriptor !=
                                      result35.output_batch.columns[ordinal]
                                          .descriptor.encoded_descriptor) {
                                    return false;
                                  }
                                }
                                return true;
                              }) &&
          request35.physical_dag.nodes.back().transformation_rule_id.size() <=
              128 &&
          replay35.diagnostic.ok &&
          replay35.selected_plan_uuid == result35.selected_plan_uuid &&
          replay35.executed_physical_node_id ==
              result35.executed_physical_node_id &&
          replay35.causal_counter_id == result35.causal_counter_id &&
          replay35.matched_right_ordinals ==
              result35.matched_right_ordinals &&
          batch_stream(replay35.output_batch) ==
              batch_stream(result35.output_batch) &&
          !asof_before_cancelled.diagnostic.ok &&
          asof_before_cancelled.diagnostic.diagnostic_code ==
              "SB_MODEL_EXECUTION_CANCELLED_V1" &&
          asof_probe_result.diagnostic.ok && asof_checkpoint_count > 1 &&
          final_publication_cancelled &&
          !asof_during_cancelled.diagnostic.ok &&
          asof_during_cancelled.diagnostic.diagnostic_code ==
              "SB_MODEL_EXECUTION_CANCELLED_V1" &&
          asof_during_cancelled.output_batch.rows.empty() &&
          !disposition_refused.diagnostic.ok &&
          disposition_refused.diagnostic.diagnostic_code ==
              "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1" &&
          disposition_refused.output_batch.rows.empty() &&
          !tolerance_refused.diagnostic.ok &&
          tolerance_refused.diagnostic.diagnostic_code ==
              "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1" &&
          tolerance_refused.output_batch.rows.empty() &&
          !output_bound_enlargement_refused.diagnostic.ok &&
          output_bound_enlargement_refused.diagnostic.diagnostic_code ==
              "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1" &&
          !memory_bound_enlargement_refused.diagnostic.ok &&
          memory_bound_enlargement_refused.diagnostic.diagnostic_code ==
              "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1" &&
          !dynamic_memory_refused.diagnostic.ok &&
          dynamic_memory_refused.diagnostic.diagnostic_code ==
              "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1" &&
          dynamic_memory_refused.output_batch.columns.empty() &&
          dynamic_memory_refused.output_batch.rows.empty() &&
          dynamic_memory_refused.matched_right_ordinals.empty() &&
          !key_substitution_refused.diagnostic.ok &&
          key_substitution_refused.diagnostic.diagnostic_code ==
              "SB_MODEL_TIME_SERIES_IDENTITY_INVALID_V1" &&
          !tie_substitution_refused.diagnostic.ok &&
          tie_substitution_refused.diagnostic.diagnostic_code ==
              "SB_MODEL_TIME_SERIES_IDENTITY_INVALID_V1" &&
          !duplicate_raw_identity_refused.diagnostic.ok &&
          duplicate_raw_identity_refused.diagnostic.diagnostic_code ==
              "SB_MODEL_TIME_SERIES_IDENTITY_INVALID_V1" &&
          !expression_substitution_refused.diagnostic.ok &&
          expression_substitution_refused.diagnostic.diagnostic_code ==
              "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1" &&
          !ordinal_substitution_refused.diagnostic.ok &&
          ordinal_substitution_refused.diagnostic.diagnostic_code ==
              "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1" &&
          !aliased_raw_binding_refused.diagnostic.ok &&
          aliased_raw_binding_refused.diagnostic.diagnostic_code ==
              "SB_MODEL_TIME_SERIES_IDENTITY_INVALID_V1" &&
          !nonraw_row_ordinal_refused.diagnostic.ok &&
          nonraw_row_ordinal_refused.diagnostic.diagnostic_code ==
              "SB_MODEL_TIME_SERIES_IDENTITY_INVALID_V1" &&
          !family_substitution_refused.diagnostic.ok &&
          family_substitution_refused.diagnostic.diagnostic_code ==
              "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1" &&
          !oversized_relational_tags_refused.diagnostic.ok &&
          oversized_relational_tags_refused.diagnostic.diagnostic_code ==
              "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1" &&
          oversized_relational_tags_refused.output_batch.rows.empty() &&
          !substituted_current_authority_refused.diagnostic.ok &&
          substituted_current_authority_refused.diagnostic.diagnostic_code ==
              "QOW-DIAG-MGA-RUNTIME-CURRENT-V1" &&
          !comparison_refused.diagnostic.ok &&
          comparison_refused.diagnostic.diagnostic_code ==
              "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1" &&
          comparison_refused.output_batch.rows.empty(),
      "TS-35 relational-left ASOF matches drifted");
  auto tie = request35;
  tie.left_batch.rows = {request35.left_batch.rows[1]};
  tie.left_keys = {key(seconds(20))};
  tie.right_batch.rows = {right_raw.rows[0], right_raw.rows[0]};
  tie.right_batch.rows[0].values[0].encoded_value =
      "40000000-0000-4000-8000-000000000009";
  tie.right_batch.rows[1].values[0].encoded_value =
      "40000000-0000-4000-8000-000000000001";
  tie.right_keys = {key(seconds(0)), key(seconds(0))};
  tie.right_tie_break_row_uuids = {
      "40000000-0000-4000-8000-000000000009",
      "40000000-0000-4000-8000-000000000001"};
  const auto tie_result = exec::ExecuteCanonicalTimeSeriesAsofJoinV1(tie);
  passed &= Require(tie_result.diagnostic.ok &&
                        tie_result.matched_right_ordinals ==
                            std::vector<std::int64_t>{1} &&
                        tie_result.output_batch.rows.front().values[3]
                                .encoded_value ==
                            "40000000-0000-4000-8000-000000000001",
                    "TS-35 equal-timestamp raw row-UUID tie-break drifted");
  completed->insert("TS-35");
  completed->insert("TS-31");

  const auto downsample = ExecuteThroughCommonSpine(
      Request(context, base,
              api::EngineBoundTimeSeriesReadOperationV1::kBucketDownsample,
              api::EngineBoundTimeSeriesAggregateV1::kSum),
      base);
  auto downsample_relational_left = RelationalTimestampBatch(
      base.columns[1].value_descriptor, 501, false,
      {"2026-08-10T12:00:30.000000000Z",
       "2026-08-10T12:01:30.000000000Z"});
  const auto relational_501_binding = RelationalAsofBinding(501);
  const auto downsample_binding = DownsampleAsofBinding();
  auto downsample_dag = AsofDag(
      context, {501, 502, 503}, {201, 202, 203, 204, 205, 206, 207},
      true, 60'000'000'000LL, 65'536, relational_501_binding,
      downsample_binding);
  exec::CanonicalTimeSeriesAsofJoinRequestV1 downsample_asof;
  downsample_asof.physical_dag = downsample_dag;
  downsample_asof.selected_physical_node_id = 3;
  downsample_asof.left_batch = std::move(downsample_relational_left);
  downsample_asof.right_batch = downsample.result.output.batch;
  downsample_asof.left_keys = {key(seconds(30)), key(seconds(90))};
  for (const auto& identity :
       downsample.result.output.ordered_row_identities) {
    downsample_asof.right_keys.push_back(
        {identity.metric_uuid, identity.tags, identity.bucket_start_ns});
  }
  downsample_asof.left_binding = relational_501_binding;
  downsample_asof.right_binding = downsample_binding;
  downsample_asof.tolerance_ns = 60'000'000'000LL;
  downsample_asof.maximum_output_rows = 16;
  downsample_asof.maximum_comparisons = 65'536;
  downsample_asof.maximum_memory_bytes = 16 * 1024 * 1024;
  downsample_asof.mga_authority = AsofAuthority(downsample_dag);
  downsample_asof.cancellation_requested = [] { return false; };
  const auto downsample_asof_result =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(downsample_asof);
  const auto downsample_asof_replay =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(downsample_asof);
  auto wrong_downsample_timestamp = downsample_asof;
  wrong_downsample_timestamp.right_binding.timestamp_expression_id = 4;
  wrong_downsample_timestamp.right_binding.timestamp_descriptor_id = 204;
  wrong_downsample_timestamp.right_binding.timestamp_column_ordinal = 3;
  const auto wrong_downsample_timestamp_refused =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(
          wrong_downsample_timestamp);
  auto duplicate_downsample_key = downsample_asof;
  duplicate_downsample_key.right_batch.rows[1] =
      duplicate_downsample_key.right_batch.rows[0];
  duplicate_downsample_key.right_keys[1] =
      duplicate_downsample_key.right_keys[0];
  const auto duplicate_downsample_key_refused =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(
          duplicate_downsample_key);
  passed &= Require(
      downsample.result.accepted && downsample_asof_result.diagnostic.ok &&
          downsample_asof_result.matched_right_ordinals ==
              std::vector<std::int64_t>{0, 1} &&
          downsample_asof_replay.diagnostic.ok &&
          batch_stream(downsample_asof_replay.output_batch) ==
              batch_stream(downsample_asof_result.output_batch) &&
          !wrong_downsample_timestamp_refused.diagnostic.ok &&
          wrong_downsample_timestamp_refused.diagnostic.diagnostic_code ==
              "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1" &&
          !duplicate_downsample_key_refused.diagnostic.ok &&
          duplicate_downsample_key_refused.diagnostic.diagnostic_code ==
              "SB_MODEL_TIME_SERIES_IDENTITY_INVALID_V1",
      "TS-35 downsample bucket_start ASOF binding/replay drifted");

  exec::DescriptorBatch left_raw;
  left_raw.columns = raw.result.output.batch.columns;
  std::vector<exec::CanonicalTimeSeriesAsofKeyV1> left_keys;
  for (std::size_t index = 0; index < raw.result.output.batch.rows.size(); ++index) {
    const auto& identity = raw.result.output.ordered_row_identities[index];
    if (identity.metric_uuid == kMetricOneUuid && identity.tags == east &&
        identity.point_timestamp_ns <= seconds(45)) {
      left_raw.rows.push_back(raw.result.output.batch.rows[index]);
      left_keys.push_back(
          {identity.metric_uuid, identity.tags, identity.point_timestamp_ns});
    }
  }
  auto relational_right = RelationalTimestampBatch(
      base.columns[1].value_descriptor, 401, false,
      {"2026-08-10T12:00:10.000000000Z",
       "2026-08-10T12:00:40.000000000Z"});
  const auto relational_401_binding = RelationalAsofBinding(401);
  auto dag36 = AsofDag(context, {101, 102, 103, 104, 105, 106},
                       {401, 402, 403}, true, 30'000'000'000LL, 65'536,
                       raw_binding, relational_401_binding);
  exec::CanonicalTimeSeriesAsofJoinRequestV1 request36;
  request36.physical_dag = dag36;
  request36.selected_physical_node_id = 3;
  request36.left_batch = std::move(left_raw);
  request36.right_batch = std::move(relational_right);
  request36.left_keys = std::move(left_keys);
  request36.right_keys = {key(seconds(10)), key(seconds(40))};
  request36.tolerance_ns = 30'000'000'000LL;
  request36.maximum_output_rows = 16;
  request36.maximum_comparisons = 65'536;
  request36.maximum_memory_bytes = 16 * 1024 * 1024;
  request36.left_binding = raw_binding;
  request36.right_binding = relational_401_binding;
  request36.mga_authority = AsofAuthority(dag36);
  request36.cancellation_requested = [] { return false; };
  const auto result36 = exec::ExecuteCanonicalTimeSeriesAsofJoinV1(request36);
  const auto replay36 = exec::ExecuteCanonicalTimeSeriesAsofJoinV1(request36);
  auto duplicate_relational_key = request36;
  duplicate_relational_key.right_batch.rows[1] =
      duplicate_relational_key.right_batch.rows[0];
  duplicate_relational_key.right_keys[1] =
      duplicate_relational_key.right_keys[0];
  const auto duplicate_relational_key_refused =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(
          duplicate_relational_key);
  passed &= Require(
      result36.diagnostic.ok && result36.output_batch.rows.size() == 3 &&
          result36.matched_right_ordinals ==
              std::vector<std::int64_t>{-1, 0, 1} &&
          result36.output_batch.rows[0].values[6].state ==
              api::EngineValueState::sql_null &&
          result36.output_batch.rows[1].values[6].encoded_value ==
              "2026-08-10T12:00:10.000000000Z" &&
          result36.output_batch.rows[2].values[6].encoded_value ==
              "2026-08-10T12:00:40.000000000Z" &&
          replay36.diagnostic.ok &&
          replay36.selected_plan_uuid == result36.selected_plan_uuid &&
          replay36.executed_physical_node_id ==
              result36.executed_physical_node_id &&
          replay36.causal_counter_id == result36.causal_counter_id &&
          replay36.matched_right_ordinals ==
              result36.matched_right_ordinals &&
          batch_stream(replay36.output_batch) ==
              batch_stream(result36.output_batch) &&
          !duplicate_relational_key_refused.diagnostic.ok &&
          duplicate_relational_key_refused.diagnostic.diagnostic_code ==
              "SB_MODEL_TIME_SERIES_IDENTITY_INVALID_V1",
      "TS-36 time-series-left ASOF LEFT matches drifted");
  auto inner36 = request36;
  inner36.physical_dag = AsofDag(
      context, {101, 102, 103, 104, 105, 106}, {401, 402, 403}, false,
      inner36.tolerance_ns, inner36.maximum_comparisons,
      raw_binding, relational_401_binding);
  inner36.mga_authority = AsofAuthority(inner36.physical_dag);
  inner36.left_outer = false;
  const auto inner_result36 =
      exec::ExecuteCanonicalTimeSeriesAsofJoinV1(inner36);
  passed &= Require(inner_result36.diagnostic.ok &&
                        inner_result36.output_batch.rows.size() == 2 &&
                        inner_result36.matched_right_ordinals ==
                            std::vector<std::int64_t>{0, 1} &&
                        std::ranges::none_of(
                            inner_result36.output_batch.columns,
                            [](const auto& column) {
                              return column.nullable;
                            }),
                    "TS-36 time-series-left ASOF INNER semantics drifted");
  completed->insert("TS-36");

  auto composition_failure_dag = AsofDag(context, {101}, {102});
  std::erase_if(composition_failure_dag.nodes, [](const auto& node) {
    return node.physical_node_id == 2;
  });
  const auto composition_root = std::ranges::find_if(
      composition_failure_dag.nodes, [](const auto& node) {
        return node.physical_node_id == 3;
      });
  if (composition_root == composition_failure_dag.nodes.end()) {
    return Require(false, "TS-40 composition failure root is absent");
  }
  composition_root->node_kind = exec::PhysicalNodeKind::kProject;
  composition_root->implementation_id =
      "project.time-series-failure.v1";
  composition_root->input_physical_node_ids = {1};
  composition_root->output_descriptor_ids = {101};
  composition_root->logical_semantic_variant_id = "project.select-list.v1";
  std::size_t acquired_source_calls = 0;
  std::size_t acquired_live_provider_calls = 0;
  std::size_t acquired_source_cleanup_calls = 0;
  std::size_t failed_composition_calls = 0;
  exec::CanonicalPhysicalExecutorRegistration source_registration;
  source_registration.node_kind = exec::PhysicalNodeKind::kScan;
  source_registration.implementation_id = "scan.asof-left.typed.v1";
  source_registration.executor_capability_uuid =
      composition_failure_dag.nodes[0].executor_capability_uuid;
  source_registration.executor_capability_abi_version =
      composition_failure_dag.nodes[0].executor_capability_abi_version;
  source_registration.engine_owned = true;
  source_registration.accepts_optimizer_publication_v2 = true;
  source_registration.execute =
      [&](const auto& selected_dag, const auto& selected_node,
          const auto& inputs) {
        ++acquired_source_calls;
        exec::CanonicalPhysicalDispatchStepResult step;
        if (!inputs.empty()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
          return step;
        }
        const auto live = ExecuteThroughCommonSpine(
            Request(context, base,
                    api::EngineBoundTimeSeriesReadOperationV1::kRangeRead,
                    api::EngineBoundTimeSeriesAggregateV1::kNone),
            base);
        acquired_live_provider_calls += live.provider_calls;
        acquired_source_cleanup_calls += live.cleanup_calls;
        step.data_access_observation_known = true;
        step.data_access_observed = live.result.data_access_observed;
        if (!live.result.accepted || !live.result.root_published ||
            !live.result.cleanup_complete || live.provider_calls != 1 ||
            live.cleanup_calls != 1) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              live.result.diagnostic_id.empty()
                  ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                  : live.result.diagnostic_id;
          step.diagnostic.detail =
              "live time-series source acquisition failed before composition";
          return step;
        }
        exec::DescriptorBatch acquired_batch;
        acquired_batch.columns = {
            live.result.output.batch.columns.front()};
        for (const auto& source_row : live.result.output.batch.rows) {
          exec::DescriptorTuple row;
          row.values = {source_row.values.front()};
          acquired_batch.rows.push_back(std::move(row));
        }
        step.selected_plan_uuid = selected_dag.selected_plan_uuid;
        step.executed_physical_node_id = selected_node.physical_node_id;
        step.causal_counter_id = selected_node.causal_counter_id;
        step.result_handle_id = selected_node.physical_node_id;
        step.output_descriptor_ids = selected_node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        step.input_row_count = 0;
        step.output_row_count = acquired_batch.rows.size();
        step.rows_examined = acquired_batch.rows.size();
        step.materialized_output_batch = acquired_batch;
        step.mga_statement_context = selected_dag.mga_statement_context;
        return step;
      };
  exec::CanonicalPhysicalExecutorRegistration project_registration;
  project_registration.node_kind = exec::PhysicalNodeKind::kProject;
  project_registration.implementation_id =
      "project.time-series-failure.v1";
  project_registration.executor_capability_uuid =
      composition_root->executor_capability_uuid;
  project_registration.executor_capability_abi_version =
      composition_root->executor_capability_abi_version;
  project_registration.engine_owned = true;
  project_registration.accepts_optimizer_publication_v2 = true;
  project_registration.execute =
      [&](const auto&, const auto&, const auto& inputs) {
        ++failed_composition_calls;
        exec::CanonicalPhysicalDispatchStepResult step;
        step.diagnostic.ok = false;
        step.diagnostic.diagnostic_code =
            "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
        step.diagnostic.detail =
            "injected composition leg failure after source acquisition";
        step.data_access_observation_known = true;
        step.data_access_observed = false;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.detail =
              "composition failure leg did not receive its acquired input";
        }
        return step;
      };
  exec::CanonicalPhysicalDagDispatchRequest composition_failure_request;
  composition_failure_request.physical_dag = composition_failure_dag;
  composition_failure_request.mga_authority =
      AsofAuthority(composition_failure_dag);
  composition_failure_request.cancellation_requested = [] { return false; };
  composition_failure_request.available_executors = {
      std::move(source_registration), std::move(project_registration)};
  const auto composition_failed = exec::ExecuteCanonicalPhysicalDag(
      composition_failure_request);
  passed &= Require(
      !composition_failed.diagnostic.ok &&
          composition_failed.diagnostic.diagnostic_code ==
              "SB_MODEL_COORDINATOR_LEG_FAILED_V1" &&
          composition_failed.execution_started &&
          composition_failed.data_access_observed &&
          composition_failed.root_result_handle_id == 0 &&
          acquired_source_calls == 1 &&
          acquired_live_provider_calls == 1 &&
          acquired_source_cleanup_calls == 1 &&
          failed_composition_calls == 1,
      "TS-40 post-acquisition composition-leg failure or cleanup drifted");
  completed->insert("TS-40");
  completed->insert("TS-30");
  completed->insert("TS-38");
  return passed;
}

bool DirectCompositionMatrix(const Fixture& fixture,
                             const api::EngineRequestContext& context,
                             std::set<std::string>* completed) {
  const auto& base = fixture.descriptors.at(std::string(kBaseObjectUuid));
  auto dag = AsofDag(context, {101}, {102});
  const auto cte = std::ranges::find_if(dag.nodes, [](const auto& node) {
    return node.physical_node_id == 2;
  });
  const auto project = std::ranges::find_if(dag.nodes, [](const auto& node) {
    return node.physical_node_id == 3;
  });
  if (cte == dag.nodes.end() || project == dag.nodes.end()) {
    return Require(false, "direct CTE/project composition root is absent");
  }
  cte->node_kind = exec::PhysicalNodeKind::kCte;
  cte->implementation_id = "cte.nonrecursive.materialize.typed.v1";
  cte->input_physical_node_ids = {1};
  cte->output_descriptor_ids = {101};
  cte->logical_semantic_variant_id = "cte.nonrecursive.v1";
  project->node_kind = exec::PhysicalNodeKind::kProject;
  project->implementation_id = "project.time-series-pass-through.v1";
  project->input_physical_node_ids = {2};
  project->output_descriptor_ids = {101};
  project->logical_semantic_variant_id = "project.select-list.v1";

  std::size_t source_calls = 0;
  std::size_t provider_calls = 0;
  std::size_t cleanup_calls = 0;
  std::size_t cte_calls = 0;
  std::size_t project_calls = 0;
  exec::CanonicalPhysicalExecutorRegistration source_registration;
  source_registration.node_kind = exec::PhysicalNodeKind::kScan;
  source_registration.implementation_id = "scan.asof-left.typed.v1";
  source_registration.executor_capability_uuid =
      dag.nodes.front().executor_capability_uuid;
  source_registration.executor_capability_abi_version =
      dag.nodes.front().executor_capability_abi_version;
  source_registration.engine_owned = true;
  source_registration.accepts_optimizer_publication_v2 = true;
  source_registration.execute =
      [&](const auto& selected_dag, const auto& selected_node,
          const auto& inputs) {
        ++source_calls;
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = selected_dag.selected_plan_uuid;
        step.executed_physical_node_id = selected_node.physical_node_id;
        step.causal_counter_id = selected_node.causal_counter_id;
        step.output_descriptor_ids = selected_node.output_descriptor_ids;
        step.mga_statement_context = selected_dag.mga_statement_context;
        step.authority.engine_mga_snapshot_bound = true;
        step.data_access_observation_known = true;
        if (!inputs.empty()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
          step.diagnostic.detail =
              "direct composition source received an input";
          return step;
        }
        const auto live = ExecuteThroughCommonSpine(
            Request(context, base,
                    api::EngineBoundTimeSeriesReadOperationV1::kRangeRead,
                    api::EngineBoundTimeSeriesAggregateV1::kNone),
            base);
        provider_calls += live.provider_calls;
        cleanup_calls += live.cleanup_calls;
        step.data_access_observed = live.result.data_access_observed;
        if (!live.result.accepted || !live.result.root_published ||
            !live.result.cleanup_complete || live.provider_calls != 1 ||
            live.cleanup_calls != 1 || live.result.output.batch.columns.empty()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              live.result.diagnostic_id.empty()
                  ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                  : live.result.diagnostic_id;
          step.diagnostic.detail =
              "direct composition source acquisition failed";
          return step;
        }
        exec::DescriptorBatch output;
        output.columns = {live.result.output.batch.columns.front()};
        for (const auto& input_row : live.result.output.batch.rows) {
          exec::DescriptorTuple output_row;
          output_row.values = {input_row.values.front()};
          output.rows.push_back(std::move(output_row));
        }
        step.result_handle_id = selected_node.physical_node_id;
        step.output_row_count = output.rows.size();
        step.rows_examined = output.rows.size();
        step.materialized_output_batch = std::move(output);
        return step;
      };

  exec::CanonicalPhysicalExecutorRegistration cte_registration;
  cte_registration.node_kind = exec::PhysicalNodeKind::kCte;
  cte_registration.implementation_id =
      "cte.nonrecursive.materialize.typed.v1";
  cte_registration.executor_capability_uuid =
      cte->executor_capability_uuid;
  cte_registration.executor_capability_abi_version =
      cte->executor_capability_abi_version;
  cte_registration.engine_owned = true;
  cte_registration.accepts_optimizer_publication_v2 = true;
  cte_registration.execute =
      [&](const auto& selected_dag, const auto& selected_node,
          const auto& inputs) {
        ++cte_calls;
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = selected_dag.selected_plan_uuid;
        step.executed_physical_node_id = selected_node.physical_node_id;
        step.causal_counter_id = selected_node.causal_counter_id;
        step.output_descriptor_ids = selected_node.output_descriptor_ids;
        step.mga_statement_context = selected_dag.mga_statement_context;
        step.authority.engine_mga_snapshot_bound = true;
        step.data_access_observation_known = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
          step.diagnostic.detail =
              "direct nonrecursive CTE input is absent";
          return step;
        }
        step.result_handle_id = selected_node.physical_node_id;
        step.input_row_count =
            inputs.front().materialized_output_batch->rows.size();
        step.output_row_count = step.input_row_count;
        step.rows_examined = step.input_row_count;
        step.materialized_output_batch =
            *inputs.front().materialized_output_batch;
        return step;
      };

  exec::CanonicalPhysicalExecutorRegistration project_registration;
  project_registration.node_kind = exec::PhysicalNodeKind::kProject;
  project_registration.implementation_id =
      "project.time-series-pass-through.v1";
  project_registration.executor_capability_uuid =
      project->executor_capability_uuid;
  project_registration.executor_capability_abi_version =
      project->executor_capability_abi_version;
  project_registration.engine_owned = true;
  project_registration.accepts_optimizer_publication_v2 = true;
  project_registration.execute =
      [&](const auto& selected_dag, const auto& selected_node,
          const auto& inputs) {
        ++project_calls;
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = selected_dag.selected_plan_uuid;
        step.executed_physical_node_id = selected_node.physical_node_id;
        step.causal_counter_id = selected_node.causal_counter_id;
        step.output_descriptor_ids = selected_node.output_descriptor_ids;
        step.mga_statement_context = selected_dag.mga_statement_context;
        step.authority.engine_mga_snapshot_bound = true;
        step.data_access_observation_known = true;
        step.data_access_observed = false;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
          step.diagnostic.detail =
              "direct composition project input is absent";
          return step;
        }
        step.result_handle_id = selected_node.physical_node_id;
        step.input_row_count =
            inputs.front().materialized_output_batch->rows.size();
        step.output_row_count = step.input_row_count;
        step.rows_examined = step.input_row_count;
        step.materialized_output_batch =
            *inputs.front().materialized_output_batch;
        return step;
      };

  exec::CanonicalPhysicalDagDispatchRequest request;
  request.physical_dag = dag;
  request.mga_authority = AsofAuthority(dag);
  request.cancellation_requested = [] { return false; };
  request.available_executors = {std::move(source_registration),
                                 std::move(cte_registration),
                                 std::move(project_registration)};
  const auto executed = exec::ExecuteCanonicalPhysicalDag(request);
  if (!executed.diagnostic.ok || executed.executed_steps.empty() ||
      !executed.executed_steps.back().materialized_output_batch.has_value()) {
    return Require(
        false, "TS-37 direct source batch was not materialized: " +
                   executed.diagnostic.diagnostic_code + ':' +
                   executed.diagnostic.detail);
  }
  const auto source_batch =
      *executed.executed_steps.back().materialized_output_batch;
  const auto unary_dag = [&](const exec::PhysicalNodeKind kind,
                             std::string implementation) {
    auto proof = AsofDag(context, {101}, {101});
    std::erase_if(proof.nodes, [](const auto& node) {
      return node.physical_node_id == 2;
    });
    const auto root = std::ranges::find_if(proof.nodes, [](const auto& node) {
      return node.physical_node_id == 3;
    });
    root->node_kind = kind;
    root->implementation_id = std::move(implementation);
    root->input_physical_node_ids = {1};
    root->output_descriptor_ids = {101};
    root->required_property_uuids.clear();
    root->delivered_property_uuids.clear();
    return proof;
  };

  auto projection_dag = unary_dag(exec::PhysicalNodeKind::kProject,
                                  "project.descriptor-direct.v1");
  exec::CanonicalDescriptorProjectionRequest projection_request;
  projection_request.physical_dag = projection_dag;
  projection_request.selected_physical_node_id = 3;
  projection_request.input_batch = source_batch;
  projection_request.projected_columns = {0};
  projection_request.mga_authority = AsofAuthority(projection_dag);
  const auto projected =
      exec::ExecuteCanonicalDescriptorProjection(projection_request);
  if (!projected.diagnostic.ok ||
      projected.output_batch.columns.size() != 1 ||
      projected.output_batch.rows.size() != 7) {
    return Require(
        false,
        "TS-37 direct project refused before composition: " +
            projected.diagnostic.diagnostic_code + ':' +
            projected.diagnostic.detail);
  }

  auto join_dag = AsofDag(context, {101}, {102});
  const auto join_root = std::ranges::find_if(
      join_dag.nodes,
      [](const auto& node) { return node.physical_node_id == 3; });
  join_root->node_kind = exec::PhysicalNodeKind::kJoin;
  join_root->implementation_id = "join.nested-loop.inner.typed.v1";
  join_root->input_physical_node_ids = {1, 2};
  join_root->output_descriptor_ids = {101, 102};
  auto right_batch = projected.output_batch;
  right_batch.columns.front().descriptor_id = 102;
  if (right_batch.rows.size() > 1) right_batch.rows.resize(1);
  exec::CanonicalDescriptorInnerJoinRequest join_request;
  join_request.physical_dag = join_dag;
  join_request.selected_physical_node_id = 3;
  join_request.left_batch = projected.output_batch;
  join_request.right_batch = right_batch;
  join_request.pair_truth_values.assign(
      projected.output_batch.rows.size() * right_batch.rows.size(),
      api::EngineSqlTruthValue::true_value);
  join_request.consumer = api::EnginePredicateConsumer::join_on;
  join_request.mga_authority = AsofAuthority(join_dag);
  const auto joined =
      exec::ExecuteCanonicalDescriptorInnerJoin(join_request);

  auto aggregate_dag = unary_dag(exec::PhysicalNodeKind::kAggregate,
                                 "aggregate.registry-core.v1");
  const auto aggregate_root = std::ranges::find_if(
      aggregate_dag.nodes,
      [](const auto& node) { return node.physical_node_id == 3; });
  aggregate_root->output_descriptor_ids = {110};
  exec::CanonicalAggregateRuntimeRequest aggregate_request;
  aggregate_request.physical_dag = aggregate_dag;
  aggregate_request.selected_physical_node_id = 3;
  aggregate_request.input_batch = projected.output_batch;
  const auto count_entry = exec::LookupCanonicalAggregateByFunctionV1(
      exec::CanonicalAggregateFunction::count);
  if (count_entry != nullptr) {
    aggregate_request.descriptor = {
        count_entry->abi_version, count_entry->function,
        count_entry->builtin_id, count_entry->function_uuid, true};
  }
  aggregate_request.result_column = {
      "point_count",
      DerivedDescriptor(NewUuidText(platform::UuidKind::object), "int64",
                        "int64"),
      false, 110};
  aggregate_request.mga_authority = AsofAuthority(aggregate_dag);
  const auto aggregated =
      exec::ExecuteCanonicalAggregateRuntime(aggregate_request);

  auto sort_dag =
      unary_dag(exec::PhysicalNodeKind::kSort, "sort.typed.terms.v1");
  exec::CanonicalDescriptorSortRequest sort_request;
  sort_request.physical_dag = sort_dag;
  sort_request.selected_physical_node_id = 3;
  sort_request.input_batch = projected.output_batch;
  exec::CanonicalDescriptorOrderTerm order_term;
  order_term.column = 0;
  order_term.expression_descriptor_id = 101;
  sort_request.order_terms = {order_term};
  sort_request.deterministic_tie_evidence_uuid =
      NewUuidText(platform::UuidKind::object);
  sort_request.maximum_pair_comparisons = 128;
  sort_request.mga_authority = AsofAuthority(sort_dag);
  const auto sorted = exec::ExecuteCanonicalDescriptorSort(sort_request);

  auto window_dag = unary_dag(exec::PhysicalNodeKind::kWindow,
                              "window.partition-order-peer.v1");
  const auto window_root = std::ranges::find_if(
      window_dag.nodes,
      [](const auto& node) { return node.physical_node_id == 3; });
  const auto window_property_uuid =
      NewUuidText(platform::UuidKind::object);
  const auto ordering_property_uuid =
      NewUuidText(platform::UuidKind::object);
  const auto partition_property_uuid =
      NewUuidText(platform::UuidKind::object);
  window_root->required_property_uuids = {partition_property_uuid,
                                          ordering_property_uuid};
  window_root->delivered_property_uuids = {window_property_uuid};
  exec::CanonicalWindowPartitionOrderRequest window_request;
  window_request.physical_dag = window_dag;
  window_request.selected_physical_node_id = 3;
  window_request.input_batch = sorted.output_batch;
  window_request.partition_terms = {
      {.column = 0, .expression_descriptor_id = 101}};
  window_request.order_terms = {order_term};
  window_request.window_property_uuid = window_property_uuid;
  window_request.partition_property_uuid = partition_property_uuid;
  window_request.ordering_property_uuid = ordering_property_uuid;
  window_request.term_binding_evidence_uuid =
      NewUuidText(platform::UuidKind::object);
  window_request.deterministic_tie_evidence_uuid =
      NewUuidText(platform::UuidKind::object);
  window_request.maximum_pair_comparisons = 128;
  window_request.mga_authority = AsofAuthority(window_dag);
  const auto window_ordered =
      exec::ExecuteCanonicalWindowPartitionOrder(window_request);
  exec::CanonicalWindowFrameRequest frame_request;
  frame_request.partition_order = window_ordered;
  frame_request.frame.frame_descriptor_uuid =
      NewUuidText(platform::UuidKind::object);
  frame_request.frame_property_binding_evidence_uuid =
      NewUuidText(platform::UuidKind::object);
  frame_request.mga_authority = AsofAuthority(window_dag);
  const auto framed = exec::ExecuteCanonicalWindowFrames(frame_request);

  auto limit_dag =
      unary_dag(exec::PhysicalNodeKind::kLimit, "limit.typed.v1");
  exec::CanonicalDescriptorLimitRequest limit_request;
  limit_request.physical_dag = limit_dag;
  limit_request.selected_physical_node_id = 3;
  limit_request.input_batch = framed.ordered_batch;
  limit_request.limit = 3;
  limit_request.offset = 1;
  limit_request.mga_authority = AsofAuthority(limit_dag);
  const auto limited = exec::ExecuteCanonicalDescriptorLimit(limit_request);

  auto set_dag = AsofDag(context, {101}, {101});
  for (auto& node : set_dag.nodes) {
    if (node.physical_node_id == 1 || node.physical_node_id == 2) {
      node.node_kind = exec::PhysicalNodeKind::kValues;
      node.input_physical_node_ids.clear();
      node.output_descriptor_ids = {101};
    } else if (node.physical_node_id == 3) {
      node.node_kind = exec::PhysicalNodeKind::kSetOperation;
      node.implementation_id = "setop.union-all.ordinal.typed.v1";
      node.input_physical_node_ids = {1, 2};
      node.output_descriptor_ids = {101};
    }
  }
  auto set_right = limited.output_batch;
  if (set_right.rows.size() > 1) set_right.rows.resize(1);
  exec::CanonicalSetOperationAllRequest set_request;
  set_request.physical_dag = set_dag;
  set_request.selected_physical_node_id = 3;
  set_request.left_batch = limited.output_batch;
  set_request.right_batch = set_right;
  set_request.result_columns = limited.output_batch.columns;
  set_request.operation = exec::CanonicalSetOperationKind::kUnion;
  set_request.alignment = exec::CanonicalSetOperationAlignment::kOrdinal;
  set_request.quantifier = exec::CanonicalSetOperationQuantifier::kAll;
  set_request.equality_profile =
      exec::CanonicalSetOperationEqualityProfile::kExactTyped;
  set_request.type_profile = exec::CanonicalSetOperationTypeProfile::kExact;
  set_request.maximum_output_row_count = 16;
  set_request.mga_authority = AsofAuthority(set_dag);
  const auto set_union = exec::ExecuteCanonicalSetOperationAll(set_request);

  auto recursive_dag = AsofDag(context, {101}, {101});
  for (auto& node : recursive_dag.nodes) {
    node.output_descriptor_ids = {101};
    if (node.physical_node_id == 1) {
      node.node_kind = exec::PhysicalNodeKind::kValues;
      node.input_physical_node_ids.clear();
    } else if (node.physical_node_id == 2) {
      node.node_kind = exec::PhysicalNodeKind::kCte;
      node.input_physical_node_ids.clear();
    } else if (node.physical_node_id == 3) {
      node.node_kind = exec::PhysicalNodeKind::kRecursiveCte;
      node.implementation_id = "cte.recursive.working.typed.v1";
      node.input_physical_node_ids = {1, 2};
    }
  }
  exec::CanonicalRecursiveCteWorkingRequest recursive_request;
  recursive_request.physical_dag = recursive_dag;
  recursive_request.selected_physical_node_id = 3;
  recursive_request.anchor_batch = set_right;
  recursive_request.recursive_step = [](const exec::DescriptorBatch& working,
                                        const std::size_t) {
    exec::DescriptorBatch empty;
    empty.columns = working.columns;
    return empty;
  };
  recursive_request.maximum_iteration_count = 2;
  recursive_request.maximum_working_row_count = 4;
  recursive_request.maximum_result_row_count = 4;
  recursive_request.mga_authority = AsofAuthority(recursive_dag);
  const auto recursive =
      exec::ExecuteCanonicalRecursiveCteWorking(recursive_request);

  exec::CanonicalResultPublicationRequest publication_request;
  publication_request.statement_uuid = context.statement_uuid.canonical;
  publication_request.mga_authority = AsofAuthority(limit_dag);
  publication_request.selected_physical_dag = limit_dag;
  publication_request.selected_catalog_epoch_uuid =
      context.catalog_epoch_uuid.canonical;
  publication_request.execution_attempt_uuid =
      NewUuidText(platform::UuidKind::object);
  publication_request.result_kind = exec::CanonicalResultKind::kRows;
  publication_request.physical_output_batch = recursive.output_batch;
  exec::CanonicalResultColumnBinding result_binding;
  result_binding.physical_column_ordinal = 0;
  result_binding.visible = true;
  result_binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
      0, "row_uuid",
      recursive.output_batch.columns.empty()
          ? std::string{}
          : recursive.output_batch.columns.front().descriptor.descriptor_uuid
                .canonical,
      recursive.output_batch.columns.empty()
          ? std::string{}
          : DescriptorField(
                recursive.output_batch.columns.front()
                    .descriptor.encoded_descriptor,
                "type_uuid"),
      exec::CanonicalResultNullability::kNonNull, std::nullopt, std::nullopt};
  publication_request.column_bindings = {std::move(result_binding)};
  publication_request.transaction_effect_evidence_uuid =
      NewUuidText(platform::UuidKind::object);
  publication_request.maximum_row_count = 4;
  const auto publication =
      exec::PublishCanonicalResultEnvelope(publication_request);

  const bool direct_operator_matrix =
      projected.diagnostic.ok && projected.output_batch.rows.size() == 7 &&
      joined.diagnostic.ok && joined.output_batch.rows.size() == 7 &&
      aggregated.diagnostic.ok && aggregated.output_batch.rows.size() == 1 &&
      aggregated.output_batch.rows.front().values.front().encoded_value ==
          "7" &&
      sorted.diagnostic.ok && sorted.output_batch.rows.size() == 7 &&
      window_ordered.diagnostic.ok &&
      window_ordered.ordered_batch.rows.size() == 7 && framed.diagnostic.ok &&
      framed.ordered_batch.rows.size() == 7 && limited.diagnostic.ok &&
      limited.output_batch.rows.size() == 3 && set_union.diagnostic.ok &&
      set_union.output_batch.rows.size() == 4 && recursive.diagnostic.ok &&
      recursive.converged && recursive.output_batch.rows.size() == 1 &&
      publication.diagnostic.ok && publication.published &&
      publication.row_stream.rows.size() == 1 &&
      completed->contains("TS-09") && completed->contains("TS-10") &&
      completed->contains("TS-11") && completed->contains("TS-12") &&
      completed->contains("TS-13") && completed->contains("TS-35") &&
      completed->contains("TS-36");
  if (!direct_operator_matrix) {
    const auto code = [](const auto& value) {
      return value.diagnostic.ok ? std::string("ok")
                                 : value.diagnostic.diagnostic_code + ":" +
                                       value.diagnostic.detail;
    };
    std::cerr << "TS-37 direct diagnostics project=" << code(projected)
              << " join=" << code(joined)
              << " aggregate=" << code(aggregated)
              << " sort=" << code(sorted)
              << " window=" << code(window_ordered)
              << " frame=" << code(framed)
              << " limit=" << code(limited)
              << " set=" << code(set_union)
              << " recursive=" << code(recursive)
              << " publish=" << code(publication) << '\n';
  }
  const bool passed = Require(
      executed.diagnostic.ok && executed.execution_started &&
          executed.data_access_observed && executed.executed_steps.size() == 3 &&
          executed.executed_root_physical_node_id == 3 &&
          executed.root_result_handle_id == 3 &&
          executed.root_output_descriptor_ids ==
              std::vector<std::uint32_t>{101} &&
          executed.executed_steps.back().materialized_output_batch.has_value() &&
          executed.executed_steps.back().materialized_output_batch->rows.size() ==
              7 &&
          source_calls == 1 && provider_calls == 1 && cleanup_calls == 1 &&
          cte_calls == 1 && project_calls == 1 && direct_operator_matrix,
      "TS-37 direct SELECT/filter/project/join/aggregate/window/CTE/recursion/"
      "set/sort/limit/result matrix drifted");
  if (passed) completed->insert("TS-37");
  return passed;
}

bool ProviderMatrix(const Fixture& fixture,
                    const api::EngineRequestContext& context,
                    std::set<std::string>* completed) {
  bool passed = true;
  const auto& base = fixture.descriptors.at(std::string(kBaseObjectUuid));
  const auto range = api::EngineBoundTimeSeriesReadOperationV1::kRangeRead;
  const auto downsample =
      api::EngineBoundTimeSeriesReadOperationV1::kBucketDownsample;
  const auto none = api::EngineBoundTimeSeriesAggregateV1::kNone;
  const auto run = [&](const auto operation, const auto aggregate) {
    return api::EngineBoundTimeSeriesReadV1(
        Request(context, base, operation, aggregate));
  };

  const auto raw = run(range, none);
  if (!raw.ok) {
    std::cerr << "TS-01 diagnostic " << Diagnostic(raw) << '\n';
  }
  passed &= Require(
      raw.ok && raw.rows.size() == 7 &&
          raw.descriptor_generation == base.descriptor_generation &&
          raw.provider_generation == base.descriptor_generation + 3'000 &&
          raw.provider_generation != raw.descriptor_generation &&
          raw.ordering_id == "series_metric_timestamp_tags_row_ascending_v1" &&
          Digest(RawStream(raw)) ==
              "bd587d51678f2da5ae92b9a57a0f646bdff6fc750b06d6159ee6179616d0696c",
      "TS-01 raw stream/digest drifted");
  completed->insert("TS-01");
  passed &= Require(raw.ok && raw.rows.front().row_uuid ==
                                  "40000000-0000-4000-8000-000000000001",
                    "TS-02 start boundary drifted");
  completed->insert("TS-02");
  passed &= Require(raw.ok && std::none_of(raw.rows.begin(), raw.rows.end(), [](const auto& row) {
                      return row.row_uuid ==
                             "40000000-0000-4000-8000-000000000008";
                    }),
                    "TS-03 end boundary leaked");
  completed->insert("TS-03");
  const auto empty = api::EngineBoundTimeSeriesReadV1(
      Request(context, base, range, none, std::string(kRangeStart),
              std::string(kRangeStart)));
  passed &= Require(empty.ok && empty.rows.empty(), "TS-04 empty range drifted");
  completed->insert("TS-04");
  const auto offset = api::EngineBoundTimeSeriesReadV1(Request(
      context, base, range, none, "2026-08-10T08:00:00.000000000-04:00",
      "2026-08-10T08:02:00.000000000-04:00"));
  passed &= Require(offset.ok && RawStream(offset) == RawStream(raw),
                    "TS-05 offset-equivalent endpoints drifted");
  completed->insert("TS-05");
  passed &= Require(raw.ok && raw.rows[3].row_uuid < raw.rows[4].row_uuid &&
                                raw.rows[3].point_timestamp_ns ==
                                    raw.rows[4].point_timestamp_ns,
                    "TS-06 duplicate-coordinate row identity drifted");
  completed->insert("TS-06");
  passed &= Require(raw.ok && raw.rows[1].point_timestamp ==
                                  "2026-08-10T12:00:30.000000000Z",
                    "TS-07 bucket input row drifted");
  completed->insert("TS-07");

  auto output_bound_request = Request(
      context, fixture.descriptors.at(std::string(kOutputBoundObjectUuid)),
      range, none);
  output_bound_request.maximum_output_rows = 1;
  output_bound_request.maximum_scanned_row_versions = 5;
  const auto output_bound =
      api::EngineBoundTimeSeriesReadV1(output_bound_request);
  passed &= Require(
      output_bound.ok && output_bound.rows.size() == 1 &&
          output_bound.rows.front().row_uuid ==
              "40000000-0000-4000-8000-000000000022",
      "TS-07 out-of-range visible rows consumed the selected-output bound");

  const auto& pre_descriptor =
      fixture.descriptors.at(std::string(kPreEpochObjectUuid));
  const auto pre = api::EngineBoundTimeSeriesReadV1(Request(
      context, pre_descriptor, downsample,
      api::EngineBoundTimeSeriesAggregateV1::kCount,
      "1969-12-31T23:59:00.000000000Z", "1970-01-01T00:00:00.000000000Z"));
  passed &= Require(pre.ok && pre.downsample_rows.size() == 1 &&
                        pre.downsample_rows[0].bucket_start ==
                            "1969-12-31T23:59:00.000000000Z",
                    "TS-08 pre-epoch mathematical floor drifted");
  completed->insert("TS-08");

  const auto sum = run(downsample, api::EngineBoundTimeSeriesAggregateV1::kSum);
  const auto count =
      run(downsample, api::EngineBoundTimeSeriesAggregateV1::kCount);
  passed &= Require(
      sum.ok && sum.downsample_rows.size() == 4 &&
          Digest(DownsampleStream(
              sum, api::EngineBoundTimeSeriesAggregateV1::kSum)) ==
              "8b5d48cff675200e1abe0e83f9bbb1656a59020de9f05497d85b18571bb66191",
      "TS-09 SUM stream/digest drifted");
  completed->insert("TS-09");
  passed &= Require(
      count.ok && count.downsample_rows.size() == 4 &&
          Digest(DownsampleStream(
              count, api::EngineBoundTimeSeriesAggregateV1::kCount)) ==
              "84d41074a7c4b7fc9b2c09ef32df0b879b24cade6687800adc224d999fde0fce",
      "TS-10 COUNT stream/digest drifted");
  completed->insert("TS-10");
  const auto avg = run(downsample, api::EngineBoundTimeSeriesAggregateV1::kAvg);
  std::fenv_t prior_environment{};
  const bool environment_saved = std::fegetenv(&prior_environment) == 0;
  const bool alternate_rounding_established =
      environment_saved &&
      std::feclearexcept(FE_ALL_EXCEPT) == 0 &&
      std::feraiseexcept(FE_INVALID) == 0 &&
      std::fesetround(FE_UPWARD) == 0;
  const auto expected_exception_flags = std::fetestexcept(FE_ALL_EXCEPT);
  const auto upward_avg =
      run(downsample, api::EngineBoundTimeSeriesAggregateV1::kAvg);
  const bool success_environment_restored =
      alternate_rounding_established && std::fegetround() == FE_UPWARD &&
      std::fetestexcept(FE_ALL_EXCEPT) == expected_exception_flags;
  auto refusing_environment_request = Request(
      context, base, downsample,
      api::EngineBoundTimeSeriesAggregateV1::kAvg);
  refusing_environment_request.maximum_groups = 1;
  const auto refusing_environment =
      api::EngineBoundTimeSeriesReadV1(refusing_environment_request);
  const bool refusal_environment_restored =
      alternate_rounding_established && std::fegetround() == FE_UPWARD &&
      std::fetestexcept(FE_ALL_EXCEPT) == expected_exception_flags;
  if (environment_saved) (void)std::fesetenv(&prior_environment);
  const bool avg_exact = avg.ok && avg.downsample_rows.size() == 4 &&
                        RealBits(avg.downsample_rows[0].aggregate_value) ==
                            "4005555555555555" &&
                        RealBits(avg.downsample_rows[1].aggregate_value) ==
                            "4018000000000000" &&
                        RealBits(avg.downsample_rows[2].aggregate_value) ==
                            "4022000000000000" &&
                        RealBits(avg.downsample_rows[3].aggregate_value) ==
                            "4000000000000000" &&
                        upward_avg.ok &&
                        DownsampleStream(
                            upward_avg,
                            api::EngineBoundTimeSeriesAggregateV1::kAvg) ==
                            DownsampleStream(
                                avg,
                                api::EngineBoundTimeSeriesAggregateV1::kAvg) &&
                        success_environment_restored &&
                        !refusing_environment.ok &&
                        Diagnostic(refusing_environment) ==
                            "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1" &&
                        refusal_environment_restored;
  if (!avg_exact) {
    std::cerr << "TS-11 detail saved=" << environment_saved
              << " alternate=" << alternate_rounding_established
              << " success_restore=" << success_environment_restored
              << " refusal_restore=" << refusal_environment_restored
              << " avg_ok=" << avg.ok
              << " avg_rows=" << avg.downsample_rows.size()
              << " upward_ok=" << upward_avg.ok
              << " refusal=" << Diagnostic(refusing_environment);
    for (const auto& row : avg.downsample_rows) {
      std::cerr << " bits=" << RealBits(row.aggregate_value);
    }
    std::cerr << '\n';
  }
  passed &= Require(avg_exact,
                    "TS-11 AVG bits drifted");
  completed->insert("TS-11");
  const auto minimum =
      run(downsample, api::EngineBoundTimeSeriesAggregateV1::kMin);
  passed &= Require(minimum.ok && minimum.downsample_rows.size() == 4 &&
                        minimum.downsample_rows[0].aggregate_value == 1.0 &&
                        minimum.downsample_rows[1].aggregate_value == 5.0 &&
                        minimum.downsample_rows[2].aggregate_value == 9.0 &&
                        minimum.downsample_rows[3].aggregate_value == 2.0,
                    "TS-12 MIN values drifted");
  completed->insert("TS-12");
  const auto maximum =
      run(downsample, api::EngineBoundTimeSeriesAggregateV1::kMax);
  passed &= Require(maximum.ok && maximum.downsample_rows.size() == 4 &&
                        maximum.downsample_rows[0].aggregate_value == 4.0 &&
                        maximum.downsample_rows[1].aggregate_value == 7.0 &&
                        maximum.downsample_rows[2].aggregate_value == 9.0 &&
                        maximum.downsample_rows[3].aggregate_value == 2.0,
                    "TS-13 MAX values drifted");
  completed->insert("TS-13");
  passed &= Require(sum.ok && sum.downsample_rows.size() == 4,
                    "TS-14 empty bucket was emitted");
  completed->insert("TS-14");
  passed &= Require(raw.ok && raw.rows.front().tags ==
                                  "{\"host\":\"a\",\"zone\":\"east\"}",
                    "TS-15 tag canonicalization drifted");
  const auto& unicode_descriptor =
      fixture.descriptors.at(std::string(kUnicodeOrderObjectUuid));
  const auto unicode = api::EngineBoundTimeSeriesReadV1(
      Request(context, unicode_descriptor, range, none));
  const std::string unicode_low = "{\"label\":\"\xc2\x80\"}";
  const std::string unicode_high =
      "{\"label\":\"\xf0\x9f\x98\x80\"}";
  passed &= Require(
      unicode.ok && unicode.rows.size() == 2 &&
          unicode.rows[0].tags == unicode_low &&
          unicode.rows[1].tags == unicode_high &&
          unicode.rows[0].row_uuid ==
              "40000000-0000-4000-8000-000000000016" &&
          unicode.rows[1].row_uuid ==
              "40000000-0000-4000-8000-000000000017",
      "TS-15 unsigned UTF-8 ordering or surrogate-pair canonicalization drifted");
  completed->insert("TS-15");

  const auto duplicate = api::EngineBoundTimeSeriesReadV1(Request(
      context, fixture.descriptors.at(std::string(kDuplicateTagObjectUuid)),
      range, none));
  passed &= Require(!duplicate.ok && duplicate.rows.empty() &&
                        Diagnostic(duplicate) ==
                            "SB_MODEL_TIME_SERIES_DUPLICATE_TAG_REFUSED_V1" &&
                        duplicate.data_access_observed,
                    "TS-16 duplicate tag refusal drifted");
  const auto lone_surrogate = api::EngineBoundTimeSeriesReadV1(Request(
      context, fixture.descriptors.at(std::string(kLoneSurrogateObjectUuid)),
      range, none));
  const auto reversed_surrogate = api::EngineBoundTimeSeriesReadV1(Request(
      context,
      fixture.descriptors.at(std::string(kReversedSurrogateObjectUuid)),
      range, none));
  passed &= Require(
      !lone_surrogate.ok && lone_surrogate.rows.empty() &&
          Diagnostic(lone_surrogate) ==
              "SB_MODEL_TIME_SERIES_TAG_INVALID_V1" &&
          !reversed_surrogate.ok && reversed_surrogate.rows.empty() &&
          Diagnostic(reversed_surrogate) ==
              "SB_MODEL_TIME_SERIES_TAG_INVALID_V1",
      "TS-16 lone/reversed UTF-16 surrogate refusal drifted");
  completed->insert("TS-16");
  const auto nonfinite = api::EngineBoundTimeSeriesReadV1(Request(
      context, fixture.descriptors.at(std::string(kNonfiniteObjectUuid)), range,
      none));
  passed &= Require(!nonfinite.ok && nonfinite.rows.empty() &&
                        Diagnostic(nonfinite) ==
                            "SB_MODEL_TIME_SERIES_VALUE_INVALID_V1" &&
                        nonfinite.data_access_observed,
                    "TS-17 nonfinite refusal drifted");
  completed->insert("TS-17");

  for (const auto malformed : {"2026-08-10T12:00:00",
                               "2026-08-10T12:00:60Z",
                               "9999-99-99T99:99:99Z",
                               "2262-04-11T23:47:16.854775808Z"}) {
    const auto invalid = api::EngineBoundTimeSeriesReadV1(
        Request(context, base, range, none, malformed, std::string(kRangeEnd)));
    passed &= Require(!invalid.ok && !invalid.data_access_observed &&
                          Diagnostic(invalid) ==
                              "SB_MODEL_TIME_SERIES_TIMESTAMP_INVALID_V1",
                      "TS-18 malformed timestamp refusal drifted");
  }
  completed->insert("TS-18");
  const auto reversed = api::EngineBoundTimeSeriesReadV1(Request(
      context, base, range, none, std::string(kRangeEnd),
      std::string(kRangeStart)));
  passed &= Require(!reversed.ok && !reversed.data_access_observed &&
                        Diagnostic(reversed) ==
                            "SB_MODEL_TIME_SERIES_RANGE_INVALID_V1",
                    "TS-19 reversed range refusal drifted");
  completed->insert("TS-19");

  auto null_range = Request(context, base, range, none);
  null_range.range_start.setState(api::EngineValueState::sql_null);
  const auto null_result = api::EngineBoundTimeSeriesReadV1(null_range);
  auto missing_range = Request(context, base, range, none);
  missing_range.range_end.setState(api::EngineValueState::missing);
  const auto missing_result = api::EngineBoundTimeSeriesReadV1(missing_range);
  passed &= Require(
      !null_result.ok && !null_result.data_access_observed &&
          Diagnostic(null_result) ==
              "SB_MODEL_TIME_SERIES_RANGE_INVALID_V1" &&
          !missing_result.ok && !missing_result.data_access_observed &&
          Diagnostic(missing_result) ==
              "SB_MODEL_TIME_SERIES_RANGE_INVALID_V1",
      "TS-20 null/missing range endpoint refusal drifted");
  completed->insert("TS-20");
  for (const auto interval : {0LL, -1LL}) {
    auto invalid_interval = Request(
        context, base, downsample, api::EngineBoundTimeSeriesAggregateV1::kSum);
    invalid_interval.bucket_interval_ns = interval;
    const auto result = api::EngineBoundTimeSeriesReadV1(invalid_interval);
    passed &= Require(!result.ok && !result.data_access_observed &&
                          Diagnostic(result) ==
                              "SB_MODEL_TIME_SERIES_INTERVAL_INVALID_V1",
                      "TS-21 invalid interval refusal drifted");
  }
  completed->insert("TS-21");
  auto invalid_aggregate = Request(context, base, downsample, none);
  const auto aggregate_result =
      api::EngineBoundTimeSeriesReadV1(invalid_aggregate);
  passed &= Require(!aggregate_result.ok && !aggregate_result.data_access_observed &&
                        Diagnostic(aggregate_result) ==
                            "SB_MODEL_TIME_SERIES_AGGREGATE_REFUSED_V1",
                    "TS-22 unsupported aggregate refusal drifted");
  completed->insert("TS-22");

  auto stale = Request(context, base, range, none);
  ++stale.expected_descriptor_generation;
  const auto stale_result = api::EngineBoundTimeSeriesReadV1(stale);
  passed &= Require(!stale_result.ok && !stale_result.data_access_observed &&
                        Diagnostic(stale_result) ==
                            "SB_MODEL_CATALOG_GENERATION_STALE_V1",
                    "TS-25 descriptor generation refusal drifted");
  completed->insert("TS-25");
  auto denied_context = context;
  denied_context.authorization_context.grants.clear();
  const auto denied = api::EngineBoundTimeSeriesReadV1(
      Request(denied_context, base, range, none));
  passed &= Require(!denied.ok && !denied.data_access_observed &&
                        Diagnostic(denied) ==
                            "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                    "TS-28 authorization refusal drifted");
  auto changed_security_context = context;
  ++changed_security_context.authorization_context.security_epoch;
  const auto changed_security = api::EngineBoundTimeSeriesReadV1(
      Request(changed_security_context, base, range, none));
  passed &= Require(
      !changed_security.ok && !changed_security.data_access_observed &&
          Diagnostic(changed_security) ==
              "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
      "TS-28 changed security generation was not refused pre-access");
  completed->insert("TS-28");

  auto before_cancel = Request(context, base, range, none);
  before_cancel.cancellation_requested = [] { return true; };
  const auto cancelled_before = api::EngineBoundTimeSeriesReadV1(before_cancel);
  std::size_t calls = 0;
  auto during_cancel = Request(context, base, downsample,
                               api::EngineBoundTimeSeriesAggregateV1::kSum);
  during_cancel.cancellation_requested = [&calls] { return ++calls >= 4; };
  const auto cancelled_during = api::EngineBoundTimeSeriesReadV1(during_cancel);
  std::size_t sort_checkpoint_count = 0;
  auto sort_probe = Request(context, base, range, none);
  sort_probe.cancellation_requested = [&sort_checkpoint_count] {
    ++sort_checkpoint_count;
    return false;
  };
  const auto sort_probe_result = api::EngineBoundTimeSeriesReadV1(sort_probe);
  const auto tag_cancellation_atomic = [&](const auto& descriptor) {
    std::size_t checkpoint_count = 0;
    auto probe = Request(context, descriptor, range, none);
    probe.cancellation_requested = [&checkpoint_count] {
      ++checkpoint_count;
      return false;
    };
    (void)api::EngineBoundTimeSeriesReadV1(probe);
    for (std::size_t threshold = 1; threshold <= checkpoint_count;
         ++threshold) {
      std::size_t observed = 0;
      auto cancel = Request(context, descriptor, range, none);
      cancel.cancellation_requested = [&observed, threshold] {
        return ++observed >= threshold;
      };
      const auto refusal = api::EngineBoundTimeSeriesReadV1(cancel);
      if (!refusal.ok &&
          Diagnostic(refusal) == "SB_MODEL_EXECUTION_CANCELLED_V1" &&
          DiagnosticDetail(refusal) ==
              "time-series tag canonicalization was cancelled") {
        return refusal.rows.empty() && refusal.downsample_rows.empty() &&
               refusal.result_shape.columns.empty() &&
               refusal.result_shape.rows.empty();
      }
    }
    return false;
  };
  const bool duplicate_scan_cancelled = tag_cancellation_atomic(
      fixture.descriptors.at(std::string(kDuplicateTagObjectUuid)));
  const bool canonical_output_cancelled =
      tag_cancellation_atomic(unicode_descriptor);
  std::size_t downsample_checkpoint_count = 0;
  auto downsample_probe = Request(
      context, base, downsample,
      api::EngineBoundTimeSeriesAggregateV1::kSum);
  downsample_probe.cancellation_requested = [&downsample_checkpoint_count] {
    ++downsample_checkpoint_count;
    return false;
  };
  const auto downsample_probe_result =
      api::EngineBoundTimeSeriesReadV1(downsample_probe);
  bool group_search_cancelled = false;
  for (std::size_t threshold = 1;
       threshold <= downsample_checkpoint_count && !group_search_cancelled;
       ++threshold) {
    std::size_t observed = 0;
    auto cancel = Request(context, base, downsample,
                          api::EngineBoundTimeSeriesAggregateV1::kSum);
    cancel.cancellation_requested = [&observed, threshold] {
      return ++observed >= threshold;
    };
    const auto refusal = api::EngineBoundTimeSeriesReadV1(cancel);
    group_search_cancelled =
        !refusal.ok &&
        Diagnostic(refusal) == "SB_MODEL_EXECUTION_CANCELLED_V1" &&
        DiagnosticDetail(refusal) ==
            "time-series downsample group search was cancelled" &&
        refusal.rows.empty() && refusal.downsample_rows.empty() &&
        refusal.result_shape.columns.empty() &&
        refusal.result_shape.rows.empty();
  }
  bool during_sort_refused = false;
  bool during_sort_atomic = false;
  for (std::size_t threshold = 1;
       threshold <= sort_checkpoint_count && !during_sort_refused;
       ++threshold) {
    std::size_t observed = 0;
    auto sort_cancel = Request(context, base, range, none);
    sort_cancel.cancellation_requested = [&observed, threshold] {
      return ++observed >= threshold;
    };
    const auto refusal = api::EngineBoundTimeSeriesReadV1(sort_cancel);
    during_sort_refused =
        !refusal.ok &&
        Diagnostic(refusal) == "SB_MODEL_EXECUTION_CANCELLED_V1" &&
        DiagnosticDetail(refusal) ==
            "time-series row ordering sort was cancelled";
    during_sort_atomic =
        refusal.rows.empty() && refusal.downsample_rows.empty() &&
        refusal.result_shape.columns.empty() &&
        refusal.result_shape.rows.empty();
  }
  bool final_publication_cancelled = false;
  for (std::size_t threshold = 1;
       threshold <= sort_checkpoint_count && !final_publication_cancelled;
       ++threshold) {
    std::size_t observed = 0;
    auto final_cancel = Request(context, base, range, none);
    final_cancel.cancellation_requested = [&observed, threshold] {
      return ++observed >= threshold;
    };
    const auto refusal = api::EngineBoundTimeSeriesReadV1(final_cancel);
    final_publication_cancelled =
        !refusal.ok &&
        Diagnostic(refusal) == "SB_MODEL_EXECUTION_CANCELLED_V1" &&
        DiagnosticDetail(refusal) ==
            "time-series execution was cancelled at final publication" &&
        refusal.rows.empty() && refusal.downsample_rows.empty() &&
        refusal.result_shape.columns.empty() &&
        refusal.result_shape.rows.empty();
  }
  passed &= Require(!cancelled_before.ok &&
                        Diagnostic(cancelled_before) ==
                            "SB_MODEL_EXECUTION_CANCELLED_V1" &&
                        !cancelled_before.data_access_observed &&
                        !cancelled_during.ok &&
                        Diagnostic(cancelled_during) ==
                            "SB_MODEL_EXECUTION_CANCELLED_V1" &&
                        cancelled_during.data_access_observed &&
                        sort_probe_result.ok && sort_checkpoint_count > 1 &&
                        during_sort_refused && during_sort_atomic &&
                        final_publication_cancelled &&
                        duplicate_scan_cancelled &&
                        canonical_output_cancelled &&
                        downsample_probe_result.ok &&
                        group_search_cancelled,
                    "TS-30 cancellation checkpoints drifted");
  completed->insert("TS-30");
  auto bounded = Request(context, base, downsample,
                         api::EngineBoundTimeSeriesAggregateV1::kSum);
  bounded.maximum_groups = 1;
  auto count_bounded = Request(context, base, range, none);
  count_bounded.maximum_output_rows = 1;
  auto byte_bounded = Request(context, base, range, none);
  byte_bounded.maximum_result_bytes = 1;
  auto memory_bounded = Request(context, base, range, none);
  memory_bounded.maximum_memory_bytes = 1;
  auto canonical_memory_request = Request(context, base, range, none);
  std::uint64_t canonical_low = 1;
  std::uint64_t canonical_high = canonical_memory_request.maximum_memory_bytes;
  const auto pre_canonical_failure = [](const std::string_view detail) {
    return detail ==
               "mga.heap_relation_read:heap_read_maximum_memory_bytes_exceeded" ||
           detail ==
               "time-series retained read/result preflight exceeded its memory bound" ||
           detail ==
               "time-series selected-row vector exceeded its memory bound";
  };
  while (canonical_low < canonical_high) {
    const auto middle = canonical_low + (canonical_high - canonical_low) / 2;
    canonical_memory_request.maximum_memory_bytes = middle;
    const auto probe =
        api::EngineBoundTimeSeriesReadV1(canonical_memory_request);
    if (!probe.ok && pre_canonical_failure(DiagnosticDetail(probe))) {
      canonical_low = middle + 1;
    } else {
      canonical_high = middle;
    }
  }
  canonical_memory_request.maximum_memory_bytes = canonical_low;
  const auto canonical_memory_refused =
      api::EngineBoundTimeSeriesReadV1(canonical_memory_request);

  auto working_group_request = Request(
      context, base, downsample,
      api::EngineBoundTimeSeriesAggregateV1::kSum);
  working_group_request.bucket_interval_ns = 1'000'000'000;
  std::uint64_t group_low = 1;
  std::uint64_t group_high = working_group_request.maximum_memory_bytes;
  const auto pre_group_failure = [&](const std::string_view detail) {
    return pre_canonical_failure(detail) ||
           detail ==
               "time-series tag canonicalization exceeded its memory bound" ||
           detail ==
               "time-series selected-row retained memory exceeded its bound" ||
           detail ==
               "time-series downsample lookup key exceeded its memory bound";
  };
  while (group_low < group_high) {
    const auto middle = group_low + (group_high - group_low) / 2;
    working_group_request.maximum_memory_bytes = middle;
    const auto probe = api::EngineBoundTimeSeriesReadV1(working_group_request);
    if (!probe.ok && pre_group_failure(DiagnosticDetail(probe))) {
      group_low = middle + 1;
    } else {
      group_high = middle;
    }
  }
  working_group_request.maximum_memory_bytes = group_low;
  const auto working_group_refused =
      api::EngineBoundTimeSeriesReadV1(working_group_request);
  const std::array resources{
      api::EngineBoundTimeSeriesReadV1(count_bounded),
      api::EngineBoundTimeSeriesReadV1(byte_bounded),
      api::EngineBoundTimeSeriesReadV1(bounded),
      api::EngineBoundTimeSeriesReadV1(memory_bounded)};
  std::ostringstream resource_observations;
  for (const auto& resource : resources) {
    resource_observations << '[' << (resource.ok ? "ok" : "refused") << ':'
                          << Diagnostic(resource) << ':'
                          << DiagnosticDetail(resource) << ']';
  }
  resource_observations
      << "[canonical:" << Diagnostic(canonical_memory_refused) << ':'
      << DiagnosticDetail(canonical_memory_refused) << "]"
      << "[group:" << Diagnostic(working_group_refused) << ':'
      << DiagnosticDetail(working_group_refused) << ']';
  const auto complete_bounded_success = [](const auto& result,
                                           const std::uint64_t grant) {
    return result.ok && result.memory_receipt_complete &&
           result.memory_grant_bytes == grant &&
           result.current_live_memory_bytes <= result.peak_live_memory_bytes &&
           result.peak_live_memory_bytes <= result.memory_grant_bytes;
  };
  const bool canonical_memory_closed =
      (!canonical_memory_refused.ok &&
       Diagnostic(canonical_memory_refused) ==
           "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1" &&
       DiagnosticDetail(canonical_memory_refused) ==
           "time-series tag canonicalization exceeded its memory bound" &&
       canonical_memory_refused.rows.empty() &&
       canonical_memory_refused.downsample_rows.empty() &&
       canonical_memory_refused.result_shape.rows.empty()) ||
      complete_bounded_success(canonical_memory_refused,
                               canonical_memory_request.maximum_memory_bytes);
  const bool working_group_memory_closed =
      (!working_group_refused.ok &&
       Diagnostic(working_group_refused) ==
           "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1" &&
       DiagnosticDetail(working_group_refused) ==
           "time-series downsample working group memory exceeded its bound" &&
       working_group_refused.rows.empty() &&
       working_group_refused.downsample_rows.empty() &&
       working_group_refused.result_shape.rows.empty()) ||
      complete_bounded_success(working_group_refused,
                               working_group_request.maximum_memory_bytes);
  passed &= Require(
      std::ranges::all_of(resources, [](const auto& resource) {
        return !resource.ok && resource.rows.empty() &&
               resource.downsample_rows.empty() &&
               Diagnostic(resource) ==
                   "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
      }) &&
          canonical_memory_closed && working_group_memory_closed,
      "TS-31 count/byte/group/memory atomic refusal matrix drifted: " +
          resource_observations.str());
  completed->insert("TS-31");

  auto fallback_request = Request(context, base, range, none);
  fallback_request.exact_fallback_selected = true;
  const auto fallback = api::EngineBoundTimeSeriesReadV1(fallback_request);
  passed &= Require(fallback.ok && fallback.exact_fallback_observed &&
                        !fallback.rollup_observed &&
                        fallback.preferred_access_invocation_count == 0 &&
                        fallback.exact_fallback_access_invocation_count == 1 &&
                        fallback.selected_access_path_id ==
                            "TIME_SERIES_BUCKET_STORE_SCAN_EXACT_V1" &&
                        RawStream(fallback) == RawStream(raw),
                    "TS-32 exact fallback stream drifted");
  auto stale_rollup = Request(context, base, downsample,
                              api::EngineBoundTimeSeriesAggregateV1::kSum);
  stale_rollup.rollup_candidate_selected = true;
  stale_rollup.rollup_generation = 7;
  stale_rollup.visible_late_arrival_generation = 8;
  stale_rollup.exact_fallback_selected = true;
  const auto late_fallback = api::EngineBoundTimeSeriesReadV1(stale_rollup);
  passed &= Require(late_fallback.ok && late_fallback.exact_fallback_observed &&
                        !late_fallback.rollup_observed &&
                        late_fallback.preferred_access_invocation_count == 0 &&
                        late_fallback.exact_fallback_access_invocation_count == 1 &&
                        DownsampleStream(
                            late_fallback,
                            api::EngineBoundTimeSeriesAggregateV1::kSum) ==
                            DownsampleStream(
                                sum,
                                api::EngineBoundTimeSeriesAggregateV1::kSum),
                    "TS-34 late-arrival exact fallback drifted");
  auto unproved_rollup = stale_rollup;
  unproved_rollup.rollup_generation = 8;
  unproved_rollup.exact_fallback_selected = false;
  const auto refused_rollup =
      api::EngineBoundTimeSeriesReadV1(unproved_rollup);
  passed &= Require(
      !refused_rollup.ok && !refused_rollup.data_access_observed &&
          !refused_rollup.rollup_observed &&
          refused_rollup.preferred_access_invocation_count == 0 &&
          refused_rollup.exact_fallback_access_invocation_count == 0 &&
          Diagnostic(refused_rollup) ==
              "SB_MODEL_TIME_SERIES_ROLLUP_EQUIVALENCE_REFUSED_V1",
      "unproved rollup was observed without an exact fallback");
  const auto replay = run(range, none);
  const auto replay_sum =
      run(downsample, api::EngineBoundTimeSeriesAggregateV1::kSum);
  const auto replay_count =
      run(downsample, api::EngineBoundTimeSeriesAggregateV1::kCount);
  passed &= Require(RawStream(replay) == RawStream(raw) &&
                        DownsampleStream(
                            replay_sum,
                            api::EngineBoundTimeSeriesAggregateV1::kSum) ==
                            DownsampleStream(
                                sum,
                                api::EngineBoundTimeSeriesAggregateV1::kSum) &&
                        DownsampleStream(
                            replay_count,
                            api::EngineBoundTimeSeriesAggregateV1::kCount) ==
                            DownsampleStream(
                                count,
                                api::EngineBoundTimeSeriesAggregateV1::kCount),
                    "TS-38 deterministic replay drifted");
  completed->insert("TS-38");
  const auto invisible = api::EngineBoundTimeSeriesReadV1(Request(
      context, fixture.descriptors.at(std::string(kInvisibleInvalidObjectUuid)),
      range, none));
  passed &= Require(invisible.ok && invisible.rows.empty(),
                    "TS-39 MGA-invisible invalid row was observed");
  completed->insert("TS-39");
  return passed;
}

}  // namespace

int main() {
  Fixture fixture;
  api::EngineRequestContext active_other;
  api::EngineRequestContext reader;
  std::set<std::string> completed;
  bool passed = MakeFixture(&fixture) && SeedFixture(&fixture, &active_other) &&
                PublishReaderContext(fixture, &reader) &&
                InventoryExact(fixture, reader);
#if defined(SB_CES05_TIME_SERIES_PRODUCTION_QUERY_ROUTE)
  passed = passed && ProductionRouteMatrix(fixture, reader, &completed);
#else
  passed = passed && ProviderMatrix(fixture, reader, &completed) &&
           CoordinatorMatrix(
               reader,
               fixture.descriptors.at(std::string(kBaseObjectUuid)),
               &completed) &&
           ExchangeMatrix(fixture, reader, &completed) &&
           FrontdoorMatrix(reader, &completed) &&
           AsofMatrix(fixture, reader, &completed) &&
           DirectCompositionMatrix(fixture, reader, &completed);
#endif
  std::set<std::string> expected_outcomes;
  for (std::size_t ordinal = 1; ordinal <= 40; ++ordinal) {
    std::ostringstream outcome;
    outcome << "TS-" << std::setfill('0') << std::setw(2) << ordinal;
    expected_outcomes.insert(outcome.str());
  }
  if (passed) {
    passed &= Require(completed == expected_outcomes,
                      "time-series outcome accounting is not the exact "
                      "signed TS-01..TS-40 set");
  }
  passed &= Rollback(reader);
  passed &= Rollback(active_other);
  if (!passed) return 1;
#if defined(SB_CES05_TIME_SERIES_PRODUCTION_QUERY_ROUTE)
  std::cout << "QOW-CES05-TIME-SERIES: ordinary production canonical route "
               "matrix passed (40/40 outcomes covered)\n";
#else
  std::cout << "QOW-CES05-TIME-SERIES: direct execution proof matrix passed ("
            << completed.size() << "/40 outcomes covered)\n";
#endif
  return 0;
}
