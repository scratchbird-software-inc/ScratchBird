// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/engine_statement_fixture.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "memory.hpp"
#include "database_lifecycle.hpp"
#include "dml/import_execution_api.hpp"
#include "dml/update_api.hpp"
#include "index_apply_planner.hpp"
#include "dml/index_apply_locality_bridge.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace idx = scratchbird::core::index;
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
  Require(generated.ok(), "ODF-043 UUID generation failed");
  return generated.value;
}

platform::Uuid NewUuidValue(platform::UuidKind kind, platform::u64 salt) {
  return NewUuid(kind, salt).value;
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  platform::Uuid database_uuid;
  api::EngineRequestContext owner_context;
  std::shared_ptr<scratchbird::tests::FixtureEngineSession> session;
  platform::Uuid primary_key_uuid;
  platform::Uuid table_uuid;
  platform::Uuid id_index_uuid;
  platform::Uuid city_index_uuid;
  platform::Uuid note_hash_index_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    session.reset();
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

api::EngineRowValue Row(std::string id, std::string city, std::string note) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"city", TextValue(std::move(city))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && std::holds_alternative<std::string>(item.evidence_id) && std::get<std::string>(item.evidence_id) == id) {
      return true;
    }
  }
  return false;
}

bool EvidenceKindContains(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind,
                          std::string_view token) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind &&
        std::holds_alternative<std::string>(item.evidence_id) &&
        std::get<std::string>(item.evidence_id).find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::size_t EvidenceIndex(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind) {
  for (std::size_t index = 0; index < evidence.size(); ++index) {
    if (evidence[index].evidence_kind == kind) {
      return index;
    }
  }
  return std::numeric_limits<std::size_t>::max();
}

void AssertNoRuntimeDocLeaks(const std::vector<api::EngineEvidenceReference>& evidence) {
  const std::vector<std::string_view> forbidden = {
      "docs" "/execution-plans", "execution_plan", "findings", "contracts", "references"};
  for (const auto& item : evidence) {
    for (const auto token : forbidden) {
      Require(item.evidence_kind.find(token) == std::string::npos &&
                  (!std::holds_alternative<std::string>(item.evidence_id) ||
                   std::get<std::string>(item.evidence_id).find(token) == std::string::npos),
              "ODF-043 runtime evidence leaked documentation token");
    }
  }
}

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
  api::EngineRequestContext context = fixture.owner_context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid = fixture.database_uuid;
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "ODF-043 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "ODF-043 commit failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "odf043_locality_apply";
  api::CatalogColumnMetadata key;
  key.text = {{"canonical", "character"}, {"primary_key", "true"}};
  key.identities = {{"candidate_key_constraint_uuid", fixture.primary_key_uuid},
                    {"support_uuid", fixture.id_index_uuid}};
  std::string key_metadata;
  Require(api::EncodeCatalogColumnMetadata(key, &key_metadata),
          "ODF-043 primary key metadata encoding failed");
  table.columns.push_back({"id", std::move(key_metadata)});
  table.columns.push_back({"city", "canonical=character;not_null=true"});
  table.columns.push_back({"note", "canonical=character;not_null=true"});
  return table;
}

api::CrudIndexRecord Index(const Fixture& fixture,
                           const api::EngineRequestContext& context,
                           platform::Uuid index_uuid,
                           std::string column,
                           std::string family,
                           bool unique) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = std::move(index_uuid);
  index.table_uuid = fixture.table_uuid;
  index.column_name = column;
  index.family = std::move(family);
  index.profile = index.family == api::kCrudIndexFamilyHash
                      ? "hash"
                      : api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = unique;
  index.key_envelopes.push_back(std::move(column));
  if (unique) {
    index.key_envelopes.push_back("unique");
  }
  return index;
}

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_odf043_" + name + "_" +
                 std::to_string(UniqueSeed()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf043.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = UniqueSeed();
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-043 database create failed");

  fixture.database_uuid = create.database_uuid.value;
  fixture.primary_key_uuid = NewUuidValue(platform::UuidKind::object, salt + 14);
  fixture.owner_context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  fixture.table_uuid = NewUuidValue(platform::UuidKind::object, salt + 10);
  fixture.id_index_uuid = NewUuidValue(platform::UuidKind::object, salt + 11);
  fixture.city_index_uuid = NewUuidValue(platform::UuidKind::object, salt + 12);
  fixture.note_hash_index_uuid =
      NewUuidValue(platform::UuidKind::object, salt + 13);

  auto metadata = Begin(fixture, "odf043-metadata");
  RequireDiagnosticOk(scratchbird::tests::PublishMgaTableFixture(metadata, Table(fixture, metadata),
                          {"character", "character", "character"}, {Index(fixture, metadata, fixture.id_index_uuid, "id", api::kCrudIndexFamilyBtree, true), Index(fixture, metadata, fixture.city_index_uuid, "city", api::kCrudIndexFamilyBtree, false), Index(fixture, metadata, fixture.note_hash_index_uuid, "note", api::kCrudIndexFamilyHash, false)}),
                      "ODF-043 table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          metadata,
                          Index(fixture,
                                metadata,
                                fixture.id_index_uuid,
                                "id",
                                api::kCrudIndexFamilyBtree,
                                true)),
                      "ODF-043 id index metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          metadata,
                          Index(fixture,
                                metadata,
                                fixture.city_index_uuid,
                                "city",
                                api::kCrudIndexFamilyBtree,
                                false)),
                      "ODF-043 city index metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          metadata,
                          Index(fixture,
                                metadata,
                                fixture.note_hash_index_uuid,
                                "note",
                                api::kCrudIndexFamilyHash,
                                false)),
                      "ODF-043 note hash index metadata append failed");
  Commit(metadata);
  fixture.owner_context.current_schema_uuid = metadata.current_schema_uuid;
  fixture.session = std::make_shared<scratchbird::tests::FixtureEngineSession>(
      BaseContext(fixture, "odf043-session"));
  return fixture;
}

std::vector<api::EngineRowValue> Rows(std::string prefix) {
  return {Row(prefix + "-001", "zurich", "alpha"),
          Row(prefix + "-002", "oslo", "bravo"),
          Row(prefix + "-003", "quito", "charlie"),
          Row(prefix + "-004", "oslo", "delta")};
}

scratchbird::tests::FixtureEngineRequest<api::EngineExecuteImportRowsRequest> ImportRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows) {
  scratchbird::tests::FixtureEngineRequest<api::EngineExecuteImportRowsRequest> request(
      *fixture.session, context);
  request.target_table.uuid = fixture.table_uuid;
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

scratchbird::tests::FixtureEngineRequest<api::EngineUpdateRowsRequest> UpdateRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context) {
  scratchbird::tests::FixtureEngineRequest<api::EngineUpdateRowsRequest> request(
      *fixture.session, context);
  request.target_table.uuid = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.update_predicate.predicate_kind = "column_equals";
  request.update_predicate.canonical_predicate_envelope = "city";
  request.update_predicate.bound_values.push_back(TextValue("oslo"));
  request.assignments.push_back({"note", TextValue("echo")});
  return request;
}

bool HasNativeLocality(const std::vector<api::EngineEvidenceReference>& evidence,
                       const api::EngineUuid& index_uuid, std::string_view locality) {
  for (std::size_t i = 1; i < evidence.size(); ++i) {
    const auto& identity = evidence[i - 1];
    const auto& key = evidence[i];
    const auto* id = std::get_if<api::EngineUuid>(&identity.evidence_id);
    const auto* label = std::get_if<std::string>(&key.evidence_id);
    if (identity.evidence_kind == "index_apply_target_index_uuid" && id &&
        *id == index_uuid && key.evidence_kind == "index_apply_target_leaf_page_locality_key" &&
        label && *label == locality) return true;
  }
  return false;
}

void AssertLocalityEvidence(const Fixture& fixture,
                            const std::vector<api::EngineEvidenceReference>& evidence,
                            std::string_view phase) {
  if (!HasEvidence(evidence,
                   "index_apply_planner",
                   "commit_group_locality_aware_v1")) {
    std::cerr << "ODF-043 " << phase << " evidence:\n";
    for (const auto& item : evidence) {
      std::cerr << item.evidence_kind << '=';
      if (const auto* id = std::get_if<api::EngineUuid>(&item.evidence_id)) {
        std::cerr << uuid::UuidToString(*id);
      } else {
        std::cerr << std::get<std::string>(item.evidence_id);
      }
      std::cerr << '\n';
    }
    Fail("ODF-043 planner evidence missing");
  }
  Require(HasEvidence(evidence, "index_apply_grouping_before_append", "true"),
          "ODF-043 grouping-before-append evidence missing");
  Require(HasEvidence(evidence, "mga_finality_authority",
                      "engine_transaction_inventory"),
          "ODF-043 MGA finality authority evidence missing");
  Require(EvidenceKindContains(evidence,
                               "index_apply_target_leaf_page_locality_key",
                               "leaf_bucket:"),
          "ODF-043 target leaf/page locality key evidence missing");
  Require(EvidenceKindContains(evidence,
                               "index_apply_family_profile_key",
                               api::kCrudIndexFamilyHash),
          "ODF-043 hash family/profile evidence missing");
  if (phase == "bulk") {
    Require(EvidenceKindContains(evidence,
                                 "index_apply_family_profile_key",
                                 api::kCrudIndexFamilyBtree),
            "ODF-043 btree family/profile evidence missing");
    Require(HasNativeLocality(evidence, fixture.id_index_uuid, "unique_order"),
            "ODF-043 unique order locality evidence missing");
    Require(HasEvidence(evidence, "index_apply_unique_order_preserved", "true"),
            "ODF-043 unique order preservation evidence missing");
    Require(EvidenceIndex(evidence, "index_apply_grouping_before_append") <
                EvidenceIndex(evidence, "mga_hot_append_index_entries"),
            "ODF-043 grouping evidence did not precede MGA index append evidence");
  }
  AssertNoRuntimeDocLeaks(evidence);
}

void CorePlannerGroupsByFamilyAndLocality() {
  std::vector<idx::CommitGroupLocalityIndexApplyItem> items;
  idx::CommitGroupLocalityIndexApplyItem unique_a;
  unique_a.source_batch_ordinal = 2;
  unique_a.source_row_ordinal = 1;
  unique_a.index_uuid = scratchbird::tests::FixtureUuid(6043, 1);
  unique_a.family = "btree";
  unique_a.profile = "rowstore_scalar_btree_v1";
  unique_a.unique = true;
  unique_a.target_keys.push_back("u-2");
  items.push_back(unique_a);
  auto unique_b = unique_a;
  unique_b.source_row_ordinal = 0;
  unique_b.target_keys = {"u-1"};
  items.push_back(unique_b);
  idx::CommitGroupLocalityIndexApplyItem btree;
  btree.source_batch_ordinal = 0;
  btree.source_row_ordinal = 0;
  btree.index_uuid = scratchbird::tests::FixtureUuid(6043, 2);
  btree.family = "btree";
  btree.profile = "rowstore_scalar_btree_v1";
  btree.target_keys.push_back("city=oslo");
  items.push_back(btree);
  idx::CommitGroupLocalityIndexApplyItem hash;
  hash.source_batch_ordinal = 1;
  hash.source_row_ordinal = 0;
  hash.index_uuid = scratchbird::tests::FixtureUuid(6043, 3);
  hash.family = "hash";
  hash.profile = "hash";
  hash.target_keys.push_back("note=alpha");
  items.push_back(hash);

  const auto plan = idx::PlanCommitGroupLocalityIndexApply(items);
  Require(plan.accepted, "ODF-043 core planner refused valid items");
  Require(plan.grouped_family_count == 2,
          "ODF-043 core planner did not group family/profile keys");
  Require(plan.locality_group_count >= 3,
          "ODF-043 core planner did not expose locality groups");
  Require(plan.planned_before_append,
          "ODF-043 core planner did not mark pre-append planning");
  Require(plan.unique_order_preserved,
          "ODF-043 core planner did not preserve unique order");
  Require(!plan.groups.empty() && plan.groups.front().unique_order_preserved,
          "ODF-043 unique group was not isolated");
  Require(plan.groups.front().item_ordinals.size() == 2 &&
              plan.groups.front().item_ordinals[0] == 1 &&
              plan.groups.front().item_ordinals[1] == 0,
          "ODF-043 unique group did not retain source row order");
}

void LocalityBridgePreservesMutationKind() {
  api::MgaExactIndexEntryAppendBatch source;
  source.index.index_uuid = scratchbird::tests::FixtureUuid(6043, 20);
  source.index.table_uuid = scratchbird::tests::FixtureUuid(6043, 21);
  source.table_uuid = source.index.table_uuid;
  source.index.family = api::kCrudIndexFamilyHash;
  source.index.profile = "hash";
  source.entries.push_back({"key", "payload",
      scratchbird::tests::FixtureUuid(6043, 22),
      scratchbird::tests::FixtureUuid(6043, 23)});
  for (const auto kind : {"retire", "insert", "exact"}) {
    source.entry_kind = kind;
    const auto plan = api::PlanLocalityAwareExactIndexApplyBatches({source});
    Require(!plan.diagnostic.error && plan.batches.size() == 1,
            "ODF-043 exact locality planning failed");
    const auto& batch = plan.batches.front();
    Require(batch.entry_kind == kind && batch.table_uuid == source.table_uuid &&
                batch.index.index_uuid == source.index.index_uuid &&
                batch.entries.size() == 1 &&
                batch.entries.front().row_uuid == source.entries.front().row_uuid &&
                batch.entries.front().version_uuid == source.entries.front().version_uuid,
            "ODF-043 locality regrouping changed native mutation identity or kind");
  }
}

void DirectBulkAndUpdateUseLocalityPlanner() {
  auto fixture = MakeFixture("route", 43000);
  auto context = Begin(fixture, "odf043-bulk");
  const auto imported =
      api::EngineExecuteImportRows(ImportRequest(fixture, context, Rows("bulk")));
  RequireOk(imported, "ODF-043 direct bulk import failed");
  Require(imported.accepted_rows == 4 && imported.inserted_rows == 4,
          "ODF-043 direct bulk row count drifted");
  Require(HasEvidence(imported.evidence, "import_execution", "direct_physical"),
          "ODF-043 import did not use direct physical lane");
  AssertLocalityEvidence(fixture, imported.evidence, "bulk");

  const auto updated = api::EngineUpdateRows(UpdateRequest(fixture, context));
  RequireOk(updated, "ODF-043 update route failed");
  Require(updated.updated_count == 2,
          "ODF-043 update row count drifted");
  AssertLocalityEvidence(fixture, updated.evidence, "update");
  Commit(context);
}

}  // namespace

int main() {
  auto policy = scratchbird::core::memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "odf043_statement_fixture";
  Require(scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
              policy, "odf043_statement_fixture").ok(),
          "ODF-043 memory policy configuration failed");
  CorePlannerGroupsByFamilyAndLocality();
  LocalityBridgePreservesMutationKind();
  DirectBulkAndUpdateUseLocalityPlanner();
  return EXIT_SUCCESS;
}
