// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/engine_evidence_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "memory.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "dml/insert_api.hpp"
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
  if (!condition) {
    Fail(message);
  }
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
  Require(generated.ok(), "ODF-033 UUID generation failed");
  return generated.value;
}

platform::Uuid NewNativeUuid(platform::UuidKind kind, platform::u64 salt) {
  return NewUuid(kind, salt).value;
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRowValue Row(std::string id, std::string name, std::string note) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"name", TextValue(std::move(name))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind, const platform::Uuid& identity) {
  for (const auto& entry : evidence)
    if (entry.evidence_kind == kind && scratchbird::tests::EvidenceIdentityEquals(entry.evidence_id, identity)) return true;
  return false;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind && scratchbird::tests::EvidenceTextFields(entry.evidence_id) == id) {
      return true;
    }
  }
  return false;
}

bool EvidenceContains(const std::vector<api::EngineEvidenceReference>& evidence,
                      std::string_view kind, const platform::Uuid& identity) {
  for (const auto& entry : evidence)
    if (entry.evidence_kind == kind && scratchbird::tests::EvidenceIdentityEquals(entry.evidence_id, identity)) return true;
  return false;
}

bool EvidenceContains(const std::vector<api::EngineEvidenceReference>& evidence,
                      std::string_view kind,
                      std::string_view token) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind &&
        scratchbird::tests::EvidenceTextFields(entry.evidence_id).find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string FirstDetail(const api::EngineApiResult& result) {
  return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().detail;
}

bool IsDuplicateKeyDetail(std::string_view detail) {
  return detail == "crud.unique_index:unique_index_duplicate" ||
         detail.find("duplicate_key") != std::string_view::npos ||
         detail.find("unique_index_duplicate") != std::string_view::npos ||
         detail.find("bulk_unique_proof_persisted_conflict") !=
             std::string_view::npos;
}

std::string FieldValue(const api::EngineInsertRowsResult& result,
                       std::string_view field_name) {
  if (result.result_shape.rows.empty()) {
    return {};
  }
  for (const auto& [field, value] : result.result_shape.rows.front().fields) {
    if (field == field_name) {
      return value.encoded_value;
    }
  }
  return {};
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  platform::Uuid database_uuid;
  api::EngineRequestContext owner_context;
  std::shared_ptr<scratchbird::tests::FixtureEngineSession> session;
  platform::Uuid table_uuid;
  platform::Uuid id_index_uuid;
  platform::Uuid primary_key_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    session.reset();
    std::error_code ignored;
    if (!dir.empty()) {
      std::filesystem::remove_all(dir, ignored);
    }
  }
};

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id,
                                      bool security_context_present = true) {
  api::EngineRequestContext context = fixture.owner_context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid = fixture.database_uuid;
  context.security_context_present = security_context_present;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture,
                                std::string request_id,
                                bool security_context_present = true) {
  api::EngineBeginTransactionRequest request;
  request.context =
      BaseContext(fixture, std::move(request_id), security_context_present);
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "ODF-033 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "ODF-033 commit failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "odf033_unique_preflight";
  api::CatalogColumnMetadata key;
  key.text = {{"canonical", "character"}, {"primary_key", "true"}};
  key.identities = {{"candidate_key_constraint_uuid", fixture.primary_key_uuid},
                    {"support_uuid", fixture.id_index_uuid}};
  std::string key_metadata;
  Require(api::EncodeCatalogColumnMetadata(key, &key_metadata),
          "ODF-033 binary primary key metadata encoding failed");
  table.columns.push_back({"id", std::move(key_metadata)});
  table.columns.push_back({"name", "canonical=character"});
  table.columns.push_back({"note", "canonical=character"});
  return table;
}

api::CrudIndexRecord IdUniqueIndex(const Fixture& fixture,
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
                ("scratchbird_odf033_" + name + "_" +
                 std::to_string(NowMillis() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf033.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + salt + 3;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-033 database create failed");

  fixture.database_uuid = create.database_uuid.value;
  fixture.owner_context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  fixture.table_uuid = NewNativeUuid(platform::UuidKind::object, salt + 10);
  fixture.id_index_uuid = NewNativeUuid(platform::UuidKind::object, salt + 11);
  fixture.primary_key_uuid = NewNativeUuid(platform::UuidKind::object, salt + 12);

  auto context = Begin(fixture, "odf033-metadata");
  RequireDiagnosticOk(scratchbird::tests::PublishMgaTableFixture(
                          context, Table(fixture, context), {"character", "character", "character"},
                          {IdUniqueIndex(fixture, context)}),
                      "ODF-033 table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(context, IdUniqueIndex(fixture, context)),
                      "ODF-033 id unique index metadata append failed");
  Commit(context);
  fixture.owner_context.current_schema_uuid = context.current_schema_uuid;
  fixture.session = std::make_shared<scratchbird::tests::FixtureEngineSession>(
      BaseContext(fixture, "odf033-session"));
  return fixture;
}

api::EngineInsertRowsResult InsertRows(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows,
    std::string conflict_action = {},
    std::vector<std::string> conflict_update_columns = {},
    std::vector<std::string> options = {}) {
  scratchbird::tests::FixtureEngineStatement statement(*fixture.session, context);
  api::EngineInsertRowsRequest request;
  request.context = statement.context;
  request.target_table.uuid = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows = std::move(rows);
  request.estimated_row_count = request.input_rows.size();
  request.on_conflict_action = std::move(conflict_action);
  if (!request.on_conflict_action.empty()) {
    request.conflict_target_column = "id";
  }
  request.conflict_update_columns = std::move(conflict_update_columns);
  request.option_envelopes = std::move(options);
  return api::EngineInsertRows(request);
}

void SeedRow(Fixture& fixture,
             std::string id,
             std::string name,
             std::string note = "seed") {
  auto context = Begin(fixture, "odf033-seed");
  const auto inserted = InsertRows(fixture,
                                   context,
                                   {Row(std::move(id), std::move(name), std::move(note))});
  RequireOk(inserted, "ODF-033 seed insert failed");
  Require(inserted.inserted_count == 1, "ODF-033 seed insert count mismatch");
  Commit(context);
}

void DuplicateInsertRefusedByIndexPreflight() {
  auto fixture = MakeFixture("duplicate", 33000);
  SeedRow(fixture, "1", "amy");
  auto context = Begin(fixture, "odf033-duplicate");
  const auto duplicate = InsertRows(fixture, context, {Row("1", "ada", "dup")});
  Require(!duplicate.ok, "ODF-033 duplicate insert was accepted");
  Require(IsDuplicateKeyDetail(FirstDetail(duplicate)),
          "ODF-033 duplicate diagnostic drifted: " + FirstDetail(duplicate));
  Require(HasEvidence(duplicate.evidence,
                      "insert_unique_preflight_path",
                      "index_backed"),
          "ODF-033 duplicate missing index-backed evidence");
  Require(HasEvidence(duplicate.evidence,
                      "insert_unique_delta_overlay",
                      "statement"),
          "ODF-033 duplicate missing delta overlay evidence");
  Require(HasEvidence(duplicate.evidence,
                      "insert_unique_probe_candidate_source",
                      "persisted_unique_index"),
          "ODF-033 duplicate did not use persisted unique index candidates");
  Require(!EvidenceContains(duplicate.evidence,
                            "constraint_key_support",
                            fixture.id_index_uuid),
          "ODF-033 duplicate reached descriptor unique validation");
  Commit(context);
}

void OnConflictDoNothingUsesUniqueIndexProbe() {
  auto fixture = MakeFixture("do_nothing", 34000);
  SeedRow(fixture, "1", "amy");
  auto context = Begin(fixture, "odf033-do-nothing");
  const auto skipped = InsertRows(fixture,
                                  context,
                                  {Row("1", "ignored", "dup")},
                                  "do_nothing");
  RequireOk(skipped, "ODF-033 ON CONFLICT DO NOTHING failed");
  Require(skipped.skipped_count == 1 && skipped.inserted_count == 0,
          "ODF-033 ON CONFLICT DO NOTHING count mismatch");
  Require(HasEvidence(skipped.evidence,
                      "on_conflict_probe_path",
                      "unique_index_lookup"),
          "ODF-033 ON CONFLICT DO NOTHING missing probe path evidence");
  Require(HasEvidence(skipped.evidence,
                      "on_conflict_match_source",
                      "persisted_unique_index"),
          "ODF-033 ON CONFLICT DO NOTHING did not use unique index match");
  Commit(context);
}

void OnConflictDoUpdateUsesUniqueIndexProbe() {
  auto fixture = MakeFixture("do_update", 35000);
  SeedRow(fixture, "1", "amy", "old");
  auto context = Begin(fixture, "odf033-do-update");
  const auto updated = InsertRows(fixture,
                                  context,
                                  {Row("1", "grace", "new")},
                                  "do_update",
                                  {"name", "note"});
  RequireOk(updated, "ODF-033 ON CONFLICT DO UPDATE failed");
  Require(updated.updated_count == 1 && updated.inserted_count == 0,
          "ODF-033 ON CONFLICT DO UPDATE count mismatch");
  Require(FieldValue(updated, "name") == "grace" &&
              FieldValue(updated, "note") == "new",
          "ODF-033 ON CONFLICT DO UPDATE returning row mismatch");
  Require(HasEvidence(updated.evidence,
                      "on_conflict_probe_path",
                      "unique_index_lookup"),
          "ODF-033 ON CONFLICT DO UPDATE missing probe path evidence");
  Require(HasEvidence(updated.evidence,
                      "on_conflict_update_path",
                      "persisted_unique_index"),
          "ODF-033 ON CONFLICT DO UPDATE missing update path evidence");
  Commit(context);
}

void StatementOverlayCatchesBatchDuplicates() {
  {
    auto fixture = MakeFixture("overlay_refuse", 36000);
    auto context = Begin(fixture, "odf033-overlay-refuse");
    const auto duplicate = InsertRows(fixture,
                                      context,
                                      {Row("1", "amy", "first"),
                                       Row("1", "ada", "second")});
    Require(!duplicate.ok, "ODF-033 intra-batch duplicate was accepted");
    Require(IsDuplicateKeyDetail(FirstDetail(duplicate)),
            "ODF-033 intra-batch duplicate diagnostic drifted: " +
                FirstDetail(duplicate));
    Require(HasEvidence(duplicate.evidence,
                        "insert_unique_probe_candidate_source",
                        "statement_delta_overlay"),
            "ODF-033 intra-batch duplicate missing statement overlay evidence");
    Commit(context);
  }
  {
    auto fixture = MakeFixture("overlay_skip", 37000);
    auto context = Begin(fixture, "odf033-overlay-skip");
    const auto skipped = InsertRows(fixture,
                                    context,
                                    {Row("1", "amy", "first"),
                                     Row("1", "ada", "second")},
                                    "do_nothing");
    RequireOk(skipped, "ODF-033 intra-batch DO NOTHING failed");
    Require(skipped.inserted_count == 1 && skipped.skipped_count == 1,
            "ODF-033 intra-batch DO NOTHING count mismatch");
    Require(HasEvidence(skipped.evidence,
                        "on_conflict_match_source",
                        "statement_delta_overlay"),
            "ODF-033 intra-batch DO NOTHING missing statement overlay evidence");
    Commit(context);
  }
}

void UnsafeRoutesFailClosed() {
  {
    auto fixture = MakeFixture("missing_tx", 38000);
    api::EngineInsertRowsRequest request;
    request.context = BaseContext(fixture, "odf033-missing-tx");
    request.target_table.uuid = fixture.table_uuid;
    request.target_table.object_kind = "table";
    request.input_rows.push_back(Row("1", "amy", "missing tx"));
    const auto refused = api::EngineInsertRows(request);
    Require(!refused.ok, "ODF-033 missing transaction was accepted");
    Require(FirstDetail(refused) == "dml.insert_rows:local_transaction_id_required",
            "ODF-033 missing transaction diagnostic drifted");
  }
  {
    auto fixture = MakeFixture("unsafe_route", 39000);
    auto context = Begin(fixture, "odf033-unsafe-route");
    scratchbird::tests::FixtureEngineStatement statement(*fixture.session, context);
    api::EngineInsertRowsRequest request;
    request.context = statement.context;
    request.bound_object_identity.catalog_generation_id = 9;
    request.bound_object_identity.security_epoch = 19;
    request.bound_object_identity.resource_epoch = 29;
    request.target_table.uuid = fixture.table_uuid;
    request.target_table.object_kind = "table";
    request.input_rows.push_back(Row("1", "amy", "unsafe"));
    request.option_envelopes.push_back("odf033.disable_mga_visibility_recheck=true");
    request.option_envelopes.push_back("odf033.disable_security_recheck=true");
    request.option_envelopes.push_back("odf033.parser_or_reference_authority=true");
    request.option_envelopes.push_back("odf033.observed_stats_epoch=39");
    request.option_envelopes.push_back("odf033.current_stats_epoch=40");
    const auto refused = api::EngineInsertRows(request);
    Require(!refused.ok, "ODF-033 unsafe route was accepted");
    Require(FirstDetail(refused).find("dml.insert_rows:unique_preflight_route_refused:") == 0,
            "ODF-033 unsafe route diagnostic drifted");
    Require(HasEvidence(refused.evidence,
                        "insert_unique_preflight_route",
                        "refused"),
            "ODF-033 unsafe route missing refused evidence");
    Require(HasEvidence(refused.evidence,
                        "insert_unique_preflight_fail_closed",
                        "true"),
            "ODF-033 unsafe route missing fail-closed evidence");
    Require(EvidenceContains(refused.evidence,
                             "insert_unique_preflight_refusal",
                             "missing MGA recheck"),
            "ODF-033 missing MGA recheck refusal evidence missing");
    Require(EvidenceContains(refused.evidence,
                             "insert_unique_preflight_refusal",
                             "missing security recheck"),
            "ODF-033 missing security recheck refusal evidence missing");
    Require(EvidenceContains(refused.evidence,
                             "insert_unique_preflight_refusal",
                             "unsafe parser/reference authority"),
            "ODF-033 parser/reference authority refusal evidence missing");
    Require(EvidenceContains(refused.evidence,
                             "insert_unique_preflight_refusal",
                             "stale catalog epoch"),
            "ODF-033 stale catalog epoch refusal evidence missing");
    Require(EvidenceContains(refused.evidence,
                             "insert_unique_preflight_refusal",
                             "stale security epoch"),
            "ODF-033 stale security epoch refusal evidence missing");
    Require(EvidenceContains(refused.evidence,
                             "insert_unique_preflight_refusal",
                             "stale policy epoch"),
            "ODF-033 stale policy epoch refusal evidence missing");
    Require(EvidenceContains(refused.evidence,
                             "insert_unique_preflight_refusal",
                             "stale stats epoch"),
            "ODF-033 stale stats epoch refusal evidence missing");
    Commit(context);
  }
}

void EvidenceHasNoRuntimeDocDependency() {
  auto fixture = MakeFixture("no_docs", 40000);
  auto context = Begin(fixture, "odf033-no-docs");
  const auto inserted = InsertRows(fixture, context, {Row("1", "amy", "clean")});
  RequireOk(inserted, "ODF-033 no-doc insert failed");
  Require(HasEvidence(inserted.evidence,
                      "constraint_key_unique_preflight",
                      fixture.id_index_uuid),
          "ODF-033 descriptor primary key did not use preflight proof");
  const std::vector<std::string_view> forbidden = {
      "docs" "/execution-plans",
      "public_execution_plan",
      "docs" "/findings",
      "docs" "\\findings",
      "public_audit_summary",
      "docs" "/contracts",
      "docs" "\\contracts",
      "docs" "/references",
      "docs" "\\references"};
  for (const auto& evidence : inserted.evidence) {
    for (const auto token : forbidden) {
      Require(evidence.evidence_kind.find(token) == std::string::npos &&
                  scratchbird::tests::EvidenceTextFields(evidence.evidence_id).find(token) == std::string::npos,
              "ODF-033 runtime evidence leaked forbidden documentation token");
    }
  }
  Commit(context);
}

}  // namespace

int main() {
  auto policy = scratchbird::core::memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "odf033_statement_fixture";
  Require(scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
              policy, "odf033_statement_fixture").ok(),
          "ODF-033 memory policy configuration failed");
  DuplicateInsertRefusedByIndexPreflight();
  OnConflictDoNothingUsesUniqueIndexProbe();
  OnConflictDoUpdateUsesUniqueIndexProbe();
  StatementOverlayCatchesBatchDuplicates();
  UnsafeRoutesFailClosed();
  EvidenceHasNoRuntimeDocDependency();
  return EXIT_SUCCESS;
}
