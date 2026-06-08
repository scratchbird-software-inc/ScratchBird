// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "bulk_placement_order.hpp"
#include "database_lifecycle.hpp"
#include "dml/import_execution_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "ordered_ingest.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace opt = scratchbird::engine::optimizer;
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
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().message_key << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
}

void RequireDiagnosticOk(const api::EngineApiDiagnostic& diagnostic,
                         std::string_view message) {
  if (diagnostic.error) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
    Fail(message);
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

platform::u64 UniqueSeed() {
  static platform::u64 counter = 0;
  return NowMillis() + (++counter * 1000);
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, UniqueSeed() + salt);
  Require(generated.ok(), "ODF-047 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string table_uuid;
  std::string id_index_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    std::error_code ignored;
    if (!dir.empty()) { std::filesystem::remove_all(dir, ignored); }
  }
};

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  typed.is_null = typed.encoded_value == "<NULL>";
  return typed;
}

api::EngineRowValue Row(std::string row_uuid,
                        std::string id,
                        std::string city) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"city", TextValue(std::move(city))});
  return row;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && item.evidence_id == id) { return true; }
  }
  return false;
}

bool AnyEvidenceContains(const std::vector<api::EngineEvidenceReference>& evidence,
                         std::string_view token) {
  for (const auto& item : evidence) {
    if (item.evidence_kind.find(token) != std::string::npos ||
        item.evidence_id.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string FieldValue(const std::vector<std::pair<std::string, std::string>>& values,
                       std::string_view field_name) {
  for (const auto& [name, value] : values) {
    if (name == field_name) { return value; }
  }
  return {};
}

void AssertNoRuntimeDocLeaks(const api::EngineApiResult& result) {
  const std::vector<std::string_view> forbidden = {
      "docs" "/execution-plans", "execution_plan", "findings", "contracts", "references"};
  for (const auto& evidence : result.evidence) {
    for (const auto token : forbidden) {
      Require(evidence.evidence_kind.find(token) == std::string::npos &&
                  evidence.evidence_id.find(token) == std::string::npos,
              "ODF-047 runtime evidence leaked documentation token");
    }
  }
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
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 47;
  context.security_epoch = 48;
  context.resource_epoch = 49;
  context.name_resolution_epoch = 50;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "ODF-047 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "ODF-047 commit failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "odf047_ordered_ingest";
  table.columns.push_back({"id", "canonical=character;not_null=true"});
  table.columns.push_back({"city", "canonical=character;not_null=true"});
  return table;
}

api::CrudIndexRecord IdIndex(const Fixture& fixture,
                             const api::EngineRequestContext& context) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = fixture.id_index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = "id";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = true;
  index.key_envelopes.push_back("id");
  index.key_envelopes.push_back("unique");
  return index;
}

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_odf047_" + name + "_" +
                 std::to_string(UniqueSeed()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf047.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = UniqueSeed();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-047 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.id_index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);

  auto metadata = Begin(fixture, "odf047-metadata");
  RequireDiagnosticOk(api::AppendMgaTableMetadata(metadata, Table(fixture, metadata)),
                      "ODF-047 table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(metadata, IdIndex(fixture, metadata)),
                      "ODF-047 index metadata append failed");
  Commit(metadata);
  return fixture;
}

api::EngineExecuteImportRowsRequest ImportRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows) {
  api::EngineExecuteImportRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.source.source_kind = "csv_stream";
  request.source.source_position = "row:0";
  request.format.format_family = "csv";
  request.import_policy.reject_mode = "fail_fast";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  request.checkpoint_policy.checkpoint_mode = "disabled";
  request.canonical_rows = std::move(rows);
  request.estimated_row_count =
      static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.option_envelopes.push_back("copy_append_batching=enabled");
  return request;
}

std::vector<api::CrudRowVersionRecord> VisibleRowsInEventOrder(
    const Fixture& fixture,
    const api::EngineRequestContext& context) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "ODF-047 relation store reload failed");
  std::vector<api::CrudRowVersionRecord> rows;
  for (const auto& row : loaded.state.row_versions) {
    if (row.table_uuid == fixture.table_uuid && !row.deleted) {
      rows.push_back(row);
    }
  }
  std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
    return left.event_sequence < right.event_sequence;
  });
  return rows;
}

void OptimizerCanDerivePlacementOrderForLargeLoad() {
  opt::BulkPlacementOrderRequest request;
  request.derive_for_large_load = true;
  request.large_load_row_threshold = 3;
  request.placement_key_column = "city";
  request.rows.push_back({0, "row-z", "zurich"});
  request.rows.push_back({1, "row-b", "berlin"});
  request.rows.push_back({2, "row-o", "oslo"});
  const auto plan = opt::PlanBulkPlacementOrder(request);
  Require(plan.ok, "ODF-047 optimizer derived placement order refused");
  Require(plan.ordered_ingest_selected,
          "ODF-047 optimizer did not derive ordered ingest for large load");
  Require(plan.source_ordinals_in_apply_order.size() == 3 &&
              plan.source_ordinals_in_apply_order[0] == 1 &&
              plan.source_ordinals_in_apply_order[1] == 2 &&
              plan.source_ordinals_in_apply_order[2] == 0,
          "ODF-047 optimizer placement permutation drifted");
  Require(plan.row_identity_preserved,
          "ODF-047 optimizer placement plan rewrote row identity");
}

void StorageClusteringPolicyFailsClosedWithoutExplicitChangePolicy() {
  page::OrderedIngestPhysicalClusteringRequest request;
  request.current_descriptor.relation_uuid = "relation-1";
  request.current_descriptor.placement_key_column = "city";
  request.current_descriptor.policy_uuid = "policy-old";
  request.current_descriptor.descriptor_generation = 4;
  request.current_descriptor.physical_clustering_enabled = true;
  request.requested_placement_key_column = "id";
  request.physical_clustering_requested = true;
  request.explicit_policy_present = true;
  request.allow_clustering_key_change = false;
  const auto resolved = page::ResolveOrderedIngestPhysicalClustering(request);
  Require(!resolved.ok, "ODF-047 clustering key change was accepted without policy");
  Require(resolved.fail_closed,
          "ODF-047 clustering key change did not fail closed");
  Require(resolved.diagnostic_code ==
              "SB-STORAGE-ORDERED-INGEST-CLUSTERING-KEY-CHANGE-REFUSED",
          "ODF-047 clustering key diagnostic drifted");
  Require(resolved.descriptor.placement_key_column == "city",
          "ODF-047 refused clustering change mutated descriptor");
}

void DirectBulkAppliesRowsInPlacementOrderAndPreservesUuidIdentity() {
  auto fixture = MakeFixture("explicit", 47100);
  const std::string row_z = NewUuidText(platform::UuidKind::row, 47120);
  const std::string row_b = NewUuidText(platform::UuidKind::row, 47121);
  const std::string row_q = NewUuidText(platform::UuidKind::row, 47122);
  const std::string row_o = NewUuidText(platform::UuidKind::row, 47123);
  auto context = Begin(fixture, "odf047-explicit");
  auto request = ImportRequest(fixture,
                               context,
                               {Row(row_z, "004", "zurich"),
                                Row(row_b, "001", "berlin"),
                                Row(row_q, "003", "quito"),
                                Row(row_o, "002", "oslo")});
  request.option_envelopes.push_back("ordered_ingest=enabled");
  request.option_envelopes.push_back("ordered_ingest.placement_key=city");
  const auto imported = api::EngineExecuteImportRows(request);
  RequireOk(imported, "ODF-047 ordered direct bulk import failed");
  Require(imported.inserted_rows == 4, "ODF-047 ordered row count drifted");
  Require(HasEvidence(imported.evidence, "bulk_placement_order_selected", "true"),
          "ODF-047 ordered placement evidence missing");
  Require(HasEvidence(imported.evidence, "ordered_ingest_apply_order", "placement_key"),
          "ODF-047 ordered apply evidence missing");
  Require(HasEvidence(imported.evidence,
                      "bulk_placement_row_identity_preserved",
                      "true"),
          "ODF-047 row identity evidence missing");
  Require(HasEvidence(imported.evidence,
                      "mga_finality_authority",
                      "engine_transaction_inventory"),
          "ODF-047 MGA authority evidence missing");
  AssertNoRuntimeDocLeaks(imported);

  const auto rows = VisibleRowsInEventOrder(fixture, context);
  Require(rows.size() == 4, "ODF-047 stored row count drifted");
  Require(FieldValue(rows[0].values, "city") == "berlin" &&
              rows[0].row_uuid == row_b,
          "ODF-047 first applied row was not berlin with original UUID");
  Require(FieldValue(rows[1].values, "city") == "oslo" &&
              rows[1].row_uuid == row_o,
          "ODF-047 second applied row was not oslo with original UUID");
  Require(FieldValue(rows[2].values, "city") == "quito" &&
              rows[2].row_uuid == row_q,
          "ODF-047 third applied row was not quito with original UUID");
  Require(FieldValue(rows[3].values, "city") == "zurich" &&
              rows[3].row_uuid == row_z,
          "ODF-047 fourth applied row was not zurich with original UUID");
  Commit(context);
}

void DirectBulkRefusesUncontrolledClusteringKeyChangeBeforeAppend() {
  auto fixture = MakeFixture("refuse", 47200);
  auto context = Begin(fixture, "odf047-refuse");
  auto request = ImportRequest(fixture,
                               context,
                               {Row(NewUuidText(platform::UuidKind::row, 47220),
                                    "002",
                                    "oslo"),
                                Row(NewUuidText(platform::UuidKind::row, 47221),
                                    "001",
                                    "berlin")});
  request.option_envelopes.push_back("ordered_ingest=enabled");
  request.option_envelopes.push_back("ordered_ingest.placement_key=id");
  request.option_envelopes.push_back("physical_clustering=enabled");
  request.option_envelopes.push_back("physical_clustering.current_key=city");
  request.option_envelopes.push_back("physical_clustering.key=id");
  request.option_envelopes.push_back("physical_clustering.policy=explicit");
  const auto imported = api::EngineExecuteImportRows(request);
  Require(!imported.ok,
          "ODF-047 uncontrolled physical clustering key change was accepted");
  Require(!imported.diagnostics.empty(),
          "ODF-047 uncontrolled clustering refusal lacked diagnostic");
  Require(imported.diagnostics.front().code ==
              "SB-STORAGE-ORDERED-INGEST-CLUSTERING-KEY-CHANGE-REFUSED",
          "ODF-047 uncontrolled clustering diagnostic code drifted");
  Require(HasEvidence(imported.evidence,
                      "ordered_ingest_physical_clustering",
                      "fail_closed"),
          "ODF-047 uncontrolled clustering fail-closed evidence missing");
  Require(!AnyEvidenceContains(imported.evidence, "mga_row_version"),
          "ODF-047 uncontrolled clustering refusal wrote rows");
  const auto rows = VisibleRowsInEventOrder(fixture, context);
  Require(rows.empty(),
          "ODF-047 uncontrolled clustering refusal published row versions");
}

void DirectBulkAllowsControlledClusteringDescriptorUpdate() {
  auto fixture = MakeFixture("allowed", 47300);
  auto context = Begin(fixture, "odf047-allowed");
  auto request = ImportRequest(fixture,
                               context,
                               {Row(NewUuidText(platform::UuidKind::row, 47320),
                                    "002",
                                    "oslo"),
                                Row(NewUuidText(platform::UuidKind::row, 47321),
                                    "001",
                                    "berlin")});
  request.option_envelopes.push_back("ordered_ingest=enabled");
  request.option_envelopes.push_back("ordered_ingest.placement_key=id");
  request.option_envelopes.push_back("physical_clustering=enabled");
  request.option_envelopes.push_back("physical_clustering.current_key=city");
  request.option_envelopes.push_back("physical_clustering.current_generation=4");
  request.option_envelopes.push_back("physical_clustering.key=id");
  request.option_envelopes.push_back("physical_clustering.policy_uuid=policy-odf047");
  request.option_envelopes.push_back("physical_clustering.allow_key_change=true");
  const auto imported = api::EngineExecuteImportRows(request);
  RequireOk(imported, "ODF-047 controlled clustering import failed");
  Require(HasEvidence(imported.evidence,
                      "ordered_ingest_physical_clustering",
                      "controlled"),
          "ODF-047 controlled clustering evidence missing");
  Require(HasEvidence(imported.evidence,
                      "ordered_ingest_clustering_descriptor_updated",
                      "true"),
          "ODF-047 clustering descriptor update evidence missing");
  Require(HasEvidence(imported.evidence,
                      "ordered_ingest_clustering_descriptor_generation",
                      "5"),
          "ODF-047 clustering descriptor generation evidence drifted");
  AssertNoRuntimeDocLeaks(imported);
  Commit(context);
}

}  // namespace

int main() {
  OptimizerCanDerivePlacementOrderForLargeLoad();
  StorageClusteringPolicyFailsClosedWithoutExplicitChangePolicy();
  DirectBulkAppliesRowsInPlacementOrderAndPreservesUuidIdentity();
  DirectBulkRefusesUncontrolledClusteringKeyChangeBeforeAppend();
  DirectBulkAllowsControlledClusteringDescriptorUpdate();
  return EXIT_SUCCESS;
}
