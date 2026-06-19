// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/insert_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
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
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Fail(message);
  }
}

platform::u64 TimeSeed() {
  return static_cast<platform::u64>(
      std::chrono::steady_clock::now().time_since_epoch().count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1900000000000ull + salt);
  Require(generated.ok(), "IPAR-P3-07 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string schema_uuid;
  std::string table_uuid;
  platform::u64 salt = 0;
  api::EngineRequestContext context;

  ~Fixture() {
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

api::EngineRowValue Row(std::string id) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"payload", TextValue("quota-proof")});
  return row;
}

std::vector<api::EngineRowValue> Rows(std::string prefix, std::size_t count) {
  std::vector<api::EngineRowValue> rows;
  rows.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    rows.push_back(Row(prefix + "-" + std::to_string(index)));
  }
  return rows;
}

api::CrudTableRecord Table(const Fixture& fixture) {
  api::CrudTableRecord table;
  table.creator_tx = fixture.context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "ipar_resource_quota";
  table.columns.push_back({"id", "canonical=character;not_null=true"});
  table.columns.push_back({"payload", "canonical=character"});
  return table;
}

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical =
      NewUuidText(platform::UuidKind::principal, fixture.salt + 100);
  context.session_uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 101);
  context.current_schema_uuid.canonical = fixture.schema_uuid;
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
  RequireOk(begun, "IPAR-P3-07 begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  const auto rolled_back = api::EngineRollbackTransaction(request);
  RequireOk(rolled_back, "IPAR-P3-07 rollback failed");
}

Fixture MakeFixture(std::string_view label, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_ipar_resource_quota_" +
                 std::string(label) + "_" + std::to_string(salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "ipar_resource_quota.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = 1900000000000ull + salt + 3;
  create.page_size = 8192;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "IPAR-P3-07 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.schema_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.context = Begin(fixture, "ipar-p3-07-metadata-" + std::string(label));

  const auto table = api::AppendMgaTableMetadata(fixture.context, Table(fixture));
  Require(!table.error, "IPAR-P3-07 table metadata append failed");
  return fixture;
}

api::EngineInsertRowsRequest InsertRequest(const Fixture& fixture,
                                           std::vector<api::EngineRowValue> rows,
                                           std::vector<std::string> options) {
  api::EngineInsertRowsRequest request;
  request.context = fixture.context;
  request.target_schema.uuid.canonical = fixture.schema_uuid;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.bound_object_identity.object_uuid = request.target_table.uuid;
  request.bound_object_identity.catalog_generation_id =
      request.context.catalog_generation_id;
  request.bound_object_identity.security_epoch = request.context.security_epoch;
  request.bound_object_identity.resource_epoch = request.context.resource_epoch;
  request.estimated_row_count = static_cast<api::EngineApiU64>(rows.size());
  request.input_rows = std::move(rows);
  request.option_envelopes = std::move(options);
  return request;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view value) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && item.evidence_id == value) {
      return true;
    }
  }
  return false;
}

bool HasDiagnostic(const api::EngineInsertRowsResult& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

void VerifyAllowedRoute(platform::u64 salt) {
  auto fixture = MakeFixture("allowed", salt);
  auto request = InsertRequest(
      fixture,
      Rows("allowed", 3),
      {"page_allocation.runtime=enabled",
       "page_allocation.free_pages=32",
       "resource_quota.max_pages_per_reservation=8",
       "resource_quota.agent_queue_max_depth=4",
       "resource_quota.observed_agent_queue_depth=0"});
  const auto inserted = api::EngineInsertRows(request);
  RequireOk(inserted, "IPAR-P3-07 allowed quota route failed");
  Require(inserted.inserted_count == 3,
          "IPAR-P3-07 allowed route inserted count mismatch");
  Require(HasEvidence(inserted.evidence, "resource_quota_policy", "evaluated"),
          "IPAR-P3-07 allowed route did not evaluate quota");
  Require(HasEvidence(inserted.evidence, "resource_quota_decision", "admit"),
          "IPAR-P3-07 allowed route did not emit admit decision");
  Rollback(fixture.context);
}

void VerifyPageReservationRefusal(platform::u64 salt) {
  auto fixture = MakeFixture("page-limit", salt);
  auto request = InsertRequest(
      fixture,
      Rows("page-limit", 4),
      {"page_allocation.runtime=enabled",
       "page_allocation.free_pages=32",
       "resource_quota.max_pages_per_reservation=2"});
  const auto refused = api::EngineInsertRows(request);
  Require(!refused.ok, "IPAR-P3-07 page quota route was not refused");
  Require(HasDiagnostic(refused, "SB-IPAR-RESOURCE-QUOTA-REFUSED"),
          "IPAR-P3-07 page quota diagnostic missing");
  Require(HasEvidence(refused.evidence,
                      "resource_quota_reason",
                      "page_reservation_limit"),
          "IPAR-P3-07 page quota reason missing");
  Require(HasEvidence(refused.evidence, "resource_quota_decision", "refuse"),
          "IPAR-P3-07 page quota refusal evidence missing");
  Rollback(fixture.context);
}

void VerifyAgentQueueBackpressureRefusal(platform::u64 salt) {
  auto fixture = MakeFixture("queue-limit", salt);
  auto request = InsertRequest(
      fixture,
      Rows("queue-limit", 1),
      {"page_allocation.runtime=enabled",
       "page_allocation.free_pages=32",
       "resource_quota.max_pages_per_reservation=8",
       "resource_quota.agent_queue_max_depth=2",
       "resource_quota.observed_agent_queue_depth=3"});
  const auto refused = api::EngineInsertRows(request);
  Require(!refused.ok, "IPAR-P3-07 queue quota route was not refused");
  Require(HasDiagnostic(refused, "SB-IPAR-RESOURCE-QUOTA-REFUSED"),
          "IPAR-P3-07 queue quota diagnostic missing");
  Require(HasEvidence(refused.evidence,
                      "resource_quota_reason",
                      "agent_queue_backpressure"),
          "IPAR-P3-07 queue quota reason missing");
  Require(HasEvidence(refused.evidence,
                      "resource_quota_agent_queue_depth",
                      "3"),
          "IPAR-P3-07 queue depth evidence missing");
  Rollback(fixture.context);
}

}  // namespace

int main() {
  const auto salt = TimeSeed();
  VerifyAllowedRoute(salt + 100);
  VerifyPageReservationRefusal(salt + 200);
  VerifyAgentQueueBackpressureRefusal(salt + 300);
  return EXIT_SUCCESS;
}
