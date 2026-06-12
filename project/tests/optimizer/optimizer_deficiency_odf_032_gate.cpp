// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/delete_api.hpp"
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
  Require(generated.ok(), "ODF-032 UUID generation failed");
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
  return typed;
}

api::EngineRowValue Row(std::string id,
                        std::string name,
                        std::string note,
                        std::string tag = "red") {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"name", TextValue(std::move(name))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  row.fields.push_back({"tag", TextValue(std::move(tag))});
  return row;
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string table_uuid;
  std::string id_index_uuid;
  std::string name_index_uuid;
  std::string note_bitmap_index_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    std::error_code ignored;
    if (!dir.empty()) {
      std::filesystem::remove_all(dir, ignored);
    }
  }
};

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id,
                                      bool security_context_present = true) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical =
      NewUuidText(platform::UuidKind::principal, fixture.salt + 100);
  context.session_uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 101);
  context.security_context_present = security_context_present;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 10;
  context.security_epoch = 20;
  context.resource_epoch = 30;
  context.name_resolution_epoch = 40;
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
  RequireOk(begun, "ODF-032 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "ODF-032 commit failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "odf032_delete_candidates";
  table.columns.push_back({"id", "canonical=character"});
  table.columns.push_back({"name", "canonical=character"});
  table.columns.push_back({"note", "canonical=character"});
  table.columns.push_back({"tag", "canonical=character"});
  return table;
}

api::CrudIndexRecord Index(const Fixture& fixture,
                           const api::EngineRequestContext& context,
                           std::string index_uuid,
                           std::string column,
                           std::string family,
                           bool unique) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = std::move(index_uuid);
  index.table_uuid = fixture.table_uuid;
  index.column_name = std::move(column);
  index.family = std::move(family);
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = unique;
  index.key_envelopes.push_back(index.column_name);
  if (unique) {
    index.key_envelopes.push_back("unique");
  }
  return index;
}

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_odf032_" + name + "_" +
                 std::to_string(NowMillis() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf032.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-032 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.id_index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.name_index_uuid = NewUuidText(platform::UuidKind::object, salt + 12);
  fixture.note_bitmap_index_uuid =
      NewUuidText(platform::UuidKind::object, salt + 13);

  auto context = Begin(fixture, "odf032-metadata");
  RequireDiagnosticOk(api::AppendMgaTableMetadata(context, Table(fixture, context)),
                      "ODF-032 table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture,
                                context,
                                fixture.id_index_uuid,
                                "id",
                                api::kCrudIndexFamilyBtree,
                                true)),
                      "ODF-032 id index metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture,
                                context,
                                fixture.name_index_uuid,
                                "name",
                                api::kCrudIndexFamilyBtree,
                                false)),
                      "ODF-032 name index metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture,
                                context,
                                fixture.note_bitmap_index_uuid,
                                "note",
                                api::kCrudIndexFamilyBitmap,
                                false)),
                      "ODF-032 bitmap index metadata append failed");
  Commit(context);
  return fixture;
}

api::EngineInsertRowsResult InsertRow(const Fixture& fixture,
                                      const api::EngineRequestContext& context,
                                      std::string id,
                                      std::string name,
                                      std::string note,
                                      std::string tag = "red") {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows.push_back(
      Row(std::move(id), std::move(name), std::move(note), std::move(tag)));
  request.estimated_row_count = 1;
  return api::EngineInsertRows(request);
}

std::string SeedRow(Fixture& fixture,
                    std::string id,
                    std::string name,
                    std::string note,
                    std::string tag = "red") {
  auto context = Begin(fixture, "odf032-seed");
  const auto inserted = InsertRow(fixture,
                                  context,
                                  std::move(id),
                                  std::move(name),
                                  std::move(note),
                                  std::move(tag));
  RequireOk(inserted, "ODF-032 seed insert failed");
  Require(inserted.row_uuids.size() == 1, "ODF-032 seed row UUID missing");
  Commit(context);
  return inserted.row_uuids.front().canonical;
}

void SeedThreeRows(Fixture& fixture) {
  SeedRow(fixture, "1", "amy", "alpha seed", "red");
  SeedRow(fixture, "2", "bob", "beta seed", "blue");
  SeedRow(fixture, "3", "cyd", "gamma seed", "red");
}

api::EngineDeleteRowsResult Delete(const Fixture& fixture,
                                   api::EngineRequestContext context,
                                   api::EnginePredicateEnvelope predicate,
                                   std::vector<std::string> options = {}) {
  api::EngineDeleteRowsRequest request;
  request.context = std::move(context);
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.delete_predicate = std::move(predicate);
  request.option_envelopes = std::move(options);
  return api::EngineDeleteRows(request);
}

api::EnginePredicateEnvelope RowUuidPredicate(std::string row_uuid) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "row_uuid_match";
  predicate.canonical_predicate_envelope = std::move(row_uuid);
  return predicate;
}

api::EnginePredicateEnvelope EqualsPredicate(std::string column,
                                             std::string value) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_equals";
  predicate.canonical_predicate_envelope = std::move(column);
  predicate.bound_values.push_back(TextValue(std::move(value)));
  return predicate;
}

api::EnginePredicateEnvelope RangePredicate(std::string column,
                                            std::string lower,
                                            std::string upper) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_range";
  predicate.canonical_predicate_envelope = std::move(column);
  predicate.bound_values.push_back(TextValue(std::move(lower)));
  predicate.bound_values.push_back(TextValue(std::move(upper)));
  return predicate;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view value) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind && entry.evidence_id == value) {
      return true;
    }
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

void RequireDeleteEvidence(const api::EngineDeleteRowsResult& result,
                           std::string_view stream_kind,
                           std::string_view access_kind,
                           api::EngineApiU64 expected_deleted_count) {
  RequireOk(result, "ODF-032 delete failed");
  Require(result.deleted_count == expected_deleted_count,
          "ODF-032 delete row count mismatch");
  Require(HasEvidence(result.evidence, "delete_row_candidate_stream", stream_kind),
          "ODF-032 delete candidate stream evidence missing");
  Require(HasEvidence(result.evidence, "delete_target_access_kind", access_kind),
          "ODF-032 delete target access kind evidence missing");
  Require(EvidenceContains(result.evidence,
                           "dml_target_access_plan_evidence",
                           "mga_visibility_recheck=required"),
          "ODF-032 accepted route missing MGA recheck evidence");
  Require(EvidenceContains(result.evidence,
                           "dml_target_access_plan_evidence",
                           "security_recheck=required"),
          "ODF-032 accepted route missing security recheck evidence");
  Require(HasEvidence(result.evidence, "mga_row_version", "row_delete_tombstone"),
          "ODF-032 delete tombstone evidence missing");
}

void RequireUnsafeRouteRefused(const api::EngineDeleteRowsResult& result,
                               std::string_view diagnostic_token) {
  Require(!result.ok, "ODF-032 unsafe target access route was accepted");
  Require(!result.diagnostics.empty(),
          "ODF-032 unsafe target access route emitted no diagnostic");
  Require(result.diagnostics.front().detail.find("target_access_plan_refused") !=
              std::string::npos,
          "ODF-032 unsafe target access diagnostic detail mismatch");
  Require(EvidenceContains(result.evidence,
                           "dml_target_access_plan_refusal",
                           diagnostic_token),
          "ODF-032 unsafe target access refusal evidence missing");
  Require(HasEvidence(result.evidence,
                      "delete_row_candidate_stream",
                      "refused"),
          "ODF-032 unsafe target access did not fail closed");
  Require(HasEvidence(result.evidence,
                      "delete_target_access_refusal",
                      "fail_closed_unsafe_route"),
          "ODF-032 unsafe target access fail-closed evidence missing");
}

void CandidateStreamsUsePlans() {
  {
    auto fixture = MakeFixture("row_uuid", 34000);
    const auto row_uuid = SeedRow(fixture, "1", "amy", "alpha seed");
    SeedRow(fixture, "2", "bob", "beta seed");
    auto context = Begin(fixture, "odf032-row-uuid");
    const auto deleted = Delete(fixture, context, RowUuidPredicate(row_uuid));
    RequireDeleteEvidence(deleted, "row_uuid_singleton", "row_uuid_singleton", 1);
    Commit(context);
  }
  {
    auto fixture = MakeFixture("unique", 35000);
    SeedThreeRows(fixture);
    auto context = Begin(fixture, "odf032-unique-index");
    const auto deleted = Delete(fixture, context, EqualsPredicate("id", "2"));
    RequireDeleteEvidence(deleted, "indexed_predicate", "unique_index_lookup", 1);
    Require(EvidenceContains(deleted.evidence, "index_lookup", fixture.id_index_uuid),
            "ODF-032 unique index lookup evidence missing");
    Commit(context);
  }
  {
    auto fixture = MakeFixture("range", 36000);
    SeedThreeRows(fixture);
    auto context = Begin(fixture, "odf032-range-index");
    const auto deleted =
        Delete(fixture, context, RangePredicate("name", "amy", "cyd"));
    RequireDeleteEvidence(deleted, "indexed_predicate", "range_index_lookup", 3);
    Require(EvidenceContains(deleted.evidence, "index_lookup", fixture.name_index_uuid),
            "ODF-032 range index lookup evidence missing");
    Commit(context);
  }
}

void FallbacksAreExplicit() {
  {
    auto fixture = MakeFixture("unsupported", 37000);
    SeedRow(fixture, "1", "amy", "alpha seed");
    auto context = Begin(fixture, "odf032-unsupported");
    api::EnginePredicateEnvelope predicate;
    predicate.predicate_kind = "text_term_contains";
    predicate.canonical_predicate_envelope = "note";
    predicate.bound_values.push_back(TextValue("alpha"));
    const auto deleted = Delete(fixture, context, predicate);
    RequireOk(deleted, "ODF-032 unsupported predicate fallback delete failed");
    Require(HasEvidence(deleted.evidence, "delete_row_candidate_stream", "table_scan"),
            "ODF-032 unsupported predicate did not emit table scan fallback");
    Require(HasEvidence(deleted.evidence,
                        "delete_target_access_fallback",
                        "unsupported predicate"),
            "ODF-032 unsupported predicate fallback evidence missing");
    Commit(context);
  }
  {
    auto fixture = MakeFixture("unusable", 38000);
    SeedRow(fixture, "1", "amy", "alpha seed");
    auto context = Begin(fixture, "odf032-unusable-index");
    const auto deleted = Delete(fixture, context, EqualsPredicate("note", "alpha seed"));
    RequireOk(deleted, "ODF-032 unusable index fallback delete failed");
    Require(HasEvidence(deleted.evidence, "delete_row_candidate_stream", "table_scan"),
            "ODF-032 unusable index did not emit table scan fallback");
    Require(HasEvidence(deleted.evidence,
                        "delete_target_access_fallback",
                        "unusable index"),
            "ODF-032 unusable index fallback evidence missing");
    Commit(context);
  }
  {
    auto fixture = MakeFixture("unindexed", 39000);
    SeedThreeRows(fixture);
    auto context = Begin(fixture, "odf032-unindexed");
    const auto deleted = Delete(fixture, context, EqualsPredicate("tag", "red"));
    RequireOk(deleted, "ODF-032 unindexed predicate fallback delete failed");
    Require(deleted.deleted_count == 2, "ODF-032 unindexed delete count mismatch");
    Require(HasEvidence(deleted.evidence, "delete_row_candidate_stream", "table_scan"),
            "ODF-032 unindexed predicate did not emit table scan fallback");
    Require(HasEvidence(deleted.evidence,
                        "delete_target_access_fallback",
                        "unindexed predicate"),
            "ODF-032 unindexed predicate fallback evidence missing");
    Commit(context);
  }
}

void UnsafeRoutesFailClosed() {
  {
    auto fixture = MakeFixture("missing_security", 40000);
    SeedRow(fixture, "1", "amy", "alpha seed");
    auto context = Begin(fixture, "odf032-missing-security");
    const auto deleted =
        Delete(fixture,
               context,
               EqualsPredicate("id", "1"),
               {"odf032.force_missing_security_context=true"});
    RequireUnsafeRouteRefused(deleted, "missing grants/security context");
    Commit(context);
  }
  {
    auto fixture = MakeFixture("stale_epochs", 41000);
    SeedRow(fixture, "1", "amy", "alpha seed");
    auto context = Begin(fixture, "odf032-stale-epochs");
    api::EngineDeleteRowsRequest request;
    request.context = context;
    request.bound_object_identity.catalog_generation_id = 9;
    request.bound_object_identity.security_epoch = 19;
    request.bound_object_identity.resource_epoch = 29;
    request.target_table.uuid.canonical = fixture.table_uuid;
    request.target_table.object_kind = "table";
    request.delete_predicate = EqualsPredicate("id", "1");
    request.option_envelopes.push_back("odf032.observed_stats_epoch=39");
    request.option_envelopes.push_back("odf032.current_stats_epoch=40");
    const auto deleted = api::EngineDeleteRows(request);
    RequireUnsafeRouteRefused(deleted, "stale catalog epoch");
    Require(EvidenceContains(deleted.evidence,
                             "dml_target_access_plan_refusal",
                             "stale security epoch"),
            "ODF-032 stale security epoch evidence missing");
    Require(EvidenceContains(deleted.evidence,
                             "dml_target_access_plan_refusal",
                             "stale policy epoch"),
            "ODF-032 stale policy epoch evidence missing");
    Require(EvidenceContains(deleted.evidence,
                             "dml_target_access_plan_refusal",
                             "stale stats epoch"),
            "ODF-032 stale stats epoch evidence missing");
    Commit(context);
  }
  {
    auto fixture = MakeFixture("unsafe_authority", 42000);
    SeedRow(fixture, "1", "amy", "alpha seed");
    auto context = Begin(fixture, "odf032-unsafe-route");
    const auto deleted =
        Delete(fixture,
               context,
               EqualsPredicate("id", "1"),
               {"odf032.disable_mga_visibility_recheck=true",
                "odf032.disable_security_recheck=true",
                "odf032.parser_or_reference_authority=true"});
    RequireUnsafeRouteRefused(deleted, "missing MGA recheck");
    Require(EvidenceContains(deleted.evidence,
                             "dml_target_access_plan_refusal",
                             "missing security recheck"),
            "ODF-032 missing security recheck evidence missing");
    Require(EvidenceContains(deleted.evidence,
                             "dml_target_access_plan_refusal",
                             "unsafe parser/reference authority"),
            "ODF-032 unsafe parser/reference authority evidence missing");
    Commit(context);
  }
}

void EvidenceHasNoRuntimeDocDependency() {
  auto fixture = MakeFixture("no_docs", 43000);
  const auto row_uuid = SeedRow(fixture, "1", "amy", "seed");
  auto context = Begin(fixture, "odf032-no-docs");
  const auto deleted = Delete(fixture, context, RowUuidPredicate(row_uuid));
  RequireOk(deleted, "ODF-032 no-doc delete failed");
  for (const auto& evidence : deleted.evidence) {
    for (const auto forbidden : {"docs/", "execution-plans", "findings", "audit",
                                 "contracts", "references"}) {
      Require(evidence.evidence_kind.find(forbidden) == std::string::npos &&
                  evidence.evidence_id.find(forbidden) == std::string::npos,
              "ODF-032 runtime evidence leaked forbidden documentation token");
    }
  }
  Commit(context);
}

}  // namespace

int main() {
  CandidateStreamsUsePlans();
  FallbacksAreExplicit();
  UnsafeRoutesFailClosed();
  EvidenceHasNoRuntimeDocDependency();
  return 0;
}
