// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/constraint_enforcement.hpp"
#include "dml/insert_api.hpp"
#include "domain_support/domain_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "database_lifecycle.hpp"
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

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "ODF-039 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  typed.is_null = typed.encoded_value == "<NULL>";
  return typed;
}

api::EngineRowValue Row(std::initializer_list<std::pair<std::string, std::string>> fields) {
  api::EngineRowValue row;
  for (const auto& [name, value] : fields) {
    row.fields.push_back({name, TextValue(value)});
  }
  return row;
}

std::vector<std::pair<std::string, std::string>> CrudValues(
    std::initializer_list<std::pair<std::string, std::string>> fields) {
  std::vector<std::pair<std::string, std::string>> values;
  for (const auto& [name, value] : fields) { values.push_back({name, value}); }
  return values;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind && entry.evidence_id == id) { return true; }
  }
  return false;
}

bool EvidenceContains(const std::vector<api::EngineEvidenceReference>& evidence,
                      std::string_view kind,
                      std::string_view token) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind &&
        entry.evidence_id.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string FirstCode(const api::EngineApiResult& result) {
  return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().code;
}

std::string FirstKey(const api::EngineApiResult& result) {
  return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().message_key;
}

std::string FirstDetail(const api::EngineApiResult& result) {
  return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().detail;
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string parent_table_uuid;
  std::string child_table_uuid;
  std::string parent_index_uuid;
  std::string child_index_uuid;
  std::string domain_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    std::error_code ignored;
    if (!dir.empty()) { std::filesystem::remove_all(dir, ignored); }
  }
};

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
  context.catalog_generation_id = 100;
  context.security_epoch = 200;
  context.resource_epoch = 300;
  context.name_resolution_epoch = 400;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture,
                                std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "ODF-039 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "ODF-039 commit failed");
}

api::CrudTableRecord ParentTable(const Fixture& fixture,
                                 const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.parent_table_uuid;
  table.default_name = "odf039_parent";
  table.columns.push_back({"id", "canonical=character;primary_key=true"});
  return table;
}

api::CrudTableRecord ChildTable(const Fixture& fixture,
                                const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.child_table_uuid;
  table.default_name = "odf039_child";
  table.columns.push_back({"id", "canonical=character;primary_key=true"});
  table.columns.push_back({"parent_id",
                           "canonical=character;foreign_key=" +
                               fixture.parent_table_uuid + ":id"});
  table.columns.push_back({"nn", "canonical=character;not_null=true"});
  table.columns.push_back({"code", "canonical=character;check=length_gte:2"});
  table.columns.push_back({"dom", "domain_uuid=" + fixture.domain_uuid});
  return table;
}

api::CrudIndexRecord UniqueIndex(const std::string& index_uuid,
                                 const std::string& table_uuid,
                                 const std::string& column_name,
                                 const api::EngineRequestContext& context) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = index_uuid;
  index.table_uuid = table_uuid;
  index.column_name = column_name;
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = true;
  index.key_envelopes.push_back(column_name);
  index.key_envelopes.push_back("unique");
  return index;
}

api::DomainRecord Domain(const Fixture& fixture,
                         const api::EngineRequestContext& context) {
  api::DomainRecord record;
  record.creator_tx = context.local_transaction_id;
  record.domain_uuid = fixture.domain_uuid;
  record.catalog_row_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 50);
  record.schema_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 51);
  record.default_name = "odf039_domain";
  record.base_descriptor_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 52);
  record.base_descriptor_kind = "scalar";
  record.base_canonical_type_name = "character";
  record.base_encoded_descriptor = "canonical=character";
  record.nullable = false;
  record.check_constraint_envelope = "length_gte:2";
  record.validation_hook_status = "builtin";
  return record;
}

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_odf039_" + name + "_" +
                 std::to_string(NowMillis() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf039.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-039 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.parent_table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.child_table_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.parent_index_uuid = NewUuidText(platform::UuidKind::object, salt + 12);
  fixture.child_index_uuid = NewUuidText(platform::UuidKind::object, salt + 13);
  fixture.domain_uuid = NewUuidText(platform::UuidKind::object, salt + 14);

  auto context = Begin(fixture, "odf039-metadata");
  RequireDiagnosticOk(api::AppendMgaTableMetadata(context, ParentTable(fixture, context)),
                      "ODF-039 parent table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaTableMetadata(context, ChildTable(fixture, context)),
                      "ODF-039 child table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          UniqueIndex(fixture.parent_index_uuid,
                                      fixture.parent_table_uuid,
                                      "id",
                                      context)),
                      "ODF-039 parent index metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          UniqueIndex(fixture.child_index_uuid,
                                      fixture.child_table_uuid,
                                      "id",
                                      context)),
                      "ODF-039 child index metadata append failed");
  RequireDiagnosticOk(api::AppendDomainEvent(context,
                                             api::MakeDomainCreateEvent(Domain(fixture, context))),
                      "ODF-039 domain metadata append failed");
  Commit(context);
  return fixture;
}

api::EngineInsertRowsResult InsertRows(const Fixture& fixture,
                                       const api::EngineRequestContext& context,
                                       const std::string& table_uuid,
                                       std::vector<api::EngineRowValue> rows) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows = std::move(rows);
  request.estimated_row_count = request.input_rows.size();
  return api::EngineInsertRows(request);
}

void SeedParent(Fixture& fixture) {
  auto context = Begin(fixture, "odf039-seed-parent");
  const auto inserted = InsertRows(fixture,
                                   context,
                                   fixture.parent_table_uuid,
                                   {Row({{"id", "p1"}})});
  RequireOk(inserted, "ODF-039 parent seed insert failed");
  Require(inserted.inserted_count == 1, "ODF-039 parent seed count mismatch");
  Commit(context);
}

api::CrudState LoadFixtureCrudState(const api::EngineRequestContext& context) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "ODF-039 relation store load failed");
  return api::BuildCrudCompatibilityStateFromMga(loaded.state);
}

api::CrudTableRecord VisibleChildTable(const api::CrudState& state,
                                       const Fixture& fixture,
                                       const api::EngineRequestContext& context) {
  const auto table = api::FindVisibleCrudTable(state,
                                               fixture.child_table_uuid,
                                               context.local_transaction_id);
  Require(table.has_value(), "ODF-039 child table not visible");
  return *table;
}

void RepeatedInsertReusesProofs() {
  auto fixture = MakeFixture("reuse", 39000);
  SeedParent(fixture);
  auto context = Begin(fixture, "odf039-reuse");
  const auto inserted = InsertRows(
      fixture,
      context,
      fixture.child_table_uuid,
      {Row({{"id", "c1"}, {"parent_id", "p1"}, {"nn", "present"}, {"code", "ok"}, {"dom", "xy"}}),
       Row({{"id", "c2"}, {"parent_id", "p1"}, {"nn", "also-present"}, {"code", "ok"}, {"dom", "xy"}})});
  RequireOk(inserted, "ODF-039 repeated insert failed");
  Require(inserted.inserted_count == 2, "ODF-039 repeated insert count mismatch");
  Require(EvidenceContains(inserted.evidence,
                           "constraint_proof_store",
                           "unique_preflight:" + fixture.child_index_uuid),
          "ODF-039 missing unique preflight proof store");
  Require(EvidenceContains(inserted.evidence,
                           "constraint_proof_hit",
                           "unique_preflight:" + fixture.child_index_uuid),
          "ODF-039 missing unique preflight proof hit");
  Require(EvidenceContains(inserted.evidence,
                           "constraint_proof_store",
                           "not_null_descriptor:"),
          "ODF-039 missing not-null proof store");
  Require(EvidenceContains(inserted.evidence,
                           "constraint_proof_hit",
                           "not_null_descriptor:"),
          "ODF-039 missing not-null proof hit");
  Require(EvidenceContains(inserted.evidence,
                           "constraint_proof_store",
                           "check_predicate:"),
          "ODF-039 missing check proof store");
  Require(EvidenceContains(inserted.evidence,
                           "constraint_proof_hit",
                           "check_predicate:"),
          "ODF-039 missing check proof hit");
  Require(EvidenceContains(inserted.evidence,
                           "constraint_proof_store",
                           "foreign_key_parent_exists:"),
          "ODF-039 missing FK parent proof store");
  Require(EvidenceContains(inserted.evidence,
                           "constraint_proof_hit",
                           "foreign_key_parent_exists:"),
          "ODF-039 missing FK parent proof hit");
  Require(EvidenceContains(inserted.evidence,
                           "constraint_proof_store",
                           "domain_check:" + fixture.domain_uuid),
          "ODF-039 missing domain proof store");
  Require(EvidenceContains(inserted.evidence,
                           "constraint_proof_hit",
                           "domain_check:" + fixture.domain_uuid),
          "ODF-039 missing domain proof hit");
  Commit(context);
}

void StaleContextRefusesProofReuse() {
  auto fixture = MakeFixture("stale", 40000);
  SeedParent(fixture);
  auto context = Begin(fixture, "odf039-stale");
  const auto state = LoadFixtureCrudState(context);
  const auto table = VisibleChildTable(state, fixture, context);
  const auto values =
      CrudValues({{"id", "c1"}, {"parent_id", "p1"}, {"nn", "present"}, {"code", "ok"}, {"dom", "xy"}});

  auto assert_refusal = [&](api::EngineRequestContext stale_context,
                            std::string_view expected) {
    api::ConstraintDmlValidationCache cache;
    const auto first = api::ValidateImmediateRowConstraints(context,
                                                            state,
                                                            table,
                                                            "row-stale",
                                                            values,
                                                            "insert",
                                                            &cache);
    Require(first.ok, "ODF-039 initial direct validation failed");
    const auto second = api::ValidateImmediateRowConstraints(stale_context,
                                                             state,
                                                             table,
                                                             "row-stale",
                                                             values,
                                                             "insert",
                                                             &cache);
    Require(second.ok, "ODF-039 stale-context validation should still validate directly");
    Require(EvidenceContains(second.evidence, "constraint_proof_refusal", expected),
            "ODF-039 stale-context proof refusal evidence missing");
    Require(!EvidenceContains(second.evidence, "constraint_proof_hit", "foreign_key_parent_exists:"),
            "ODF-039 stale-context FK proof was reused");
  };

  auto stale_catalog = context;
  ++stale_catalog.catalog_generation_id;
  assert_refusal(stale_catalog, "catalog_epoch_mismatch");

  auto stale_visibility = context;
  ++stale_visibility.snapshot_visible_through_local_transaction_id;
  assert_refusal(stale_visibility, "visibility_epoch_mismatch");

  auto stale_security = context;
  ++stale_security.security_epoch;
  assert_refusal(stale_security, "security_epoch_mismatch");
  Commit(context);
}

void FailuresRemainFailClosed() {
  auto fixture = MakeFixture("failures", 41000);
  SeedParent(fixture);
  auto context = Begin(fixture, "odf039-failures");
  const auto missing_parent = InsertRows(
      fixture,
      context,
      fixture.child_table_uuid,
      {Row({{"id", "c1"}, {"parent_id", "missing"}, {"nn", "present"}, {"code", "ok"}, {"dom", "xy"}})});
  Require(!missing_parent.ok, "ODF-039 missing FK parent was accepted");
  Require(FirstCode(missing_parent) == "CLI.CONSTRAINT_FOREIGN_KEY_VIOLATION",
          "ODF-039 FK diagnostic code drifted");
  Require(FirstKey(missing_parent) == "constraint.foreign_key.violation",
          "ODF-039 FK diagnostic key drifted");
  Require(FirstDetail(missing_parent).find("detail=referenced_parent_key_missing") !=
              std::string::npos,
          "ODF-039 FK diagnostic detail drifted");

  const auto null_nn = InsertRows(
      fixture,
      context,
      fixture.child_table_uuid,
      {Row({{"id", "c2"}, {"parent_id", "p1"}, {"nn", "<NULL>"}, {"code", "ok"}, {"dom", "xy"}})});
  Require(!null_nn.ok, "ODF-039 null not-null value was accepted");
  Require(FirstCode(null_nn) == "CLI.CONSTRAINT_NOT_NULL_VIOLATION",
          "ODF-039 not-null diagnostic code drifted");
  Require(FirstKey(null_nn) == "constraint.not_null.violation",
          "ODF-039 not-null diagnostic key drifted");
  Require(FirstDetail(null_nn).find("detail=null_value_forbidden") != std::string::npos,
          "ODF-039 not-null diagnostic detail drifted");
  Commit(context);
}

void EvidenceHasNoRuntimeDocDependency() {
  auto fixture = MakeFixture("no_docs", 42000);
  SeedParent(fixture);
  auto context = Begin(fixture, "odf039-no-docs");
  const auto inserted = InsertRows(
      fixture,
      context,
      fixture.child_table_uuid,
      {Row({{"id", "c1"}, {"parent_id", "p1"}, {"nn", "present"}, {"code", "ok"}, {"dom", "xy"}}),
       Row({{"id", "c2"}, {"parent_id", "p1"}, {"nn", "present"}, {"code", "ok"}, {"dom", "xy"}})});
  RequireOk(inserted, "ODF-039 no-doc insert failed");
  const std::vector<std::string_view> forbidden = {
      "docs" "/execution-plans",
      "execution_plan",
      "findings",
      "audit",
      "contracts",
      "references"};
  for (const auto& evidence : inserted.evidence) {
    for (const auto token : forbidden) {
      Require(evidence.evidence_kind.find(token) == std::string::npos &&
                  evidence.evidence_id.find(token) == std::string::npos,
              "ODF-039 runtime evidence leaked forbidden documentation token");
    }
  }
  Commit(context);
}

}  // namespace

int main() {
  RepeatedInsertReusesProofs();
  StaleContextRefusesProofReuse();
  FailuresRemainFailClosed();
  EvidenceHasNoRuntimeDocDependency();
  return EXIT_SUCCESS;
}
