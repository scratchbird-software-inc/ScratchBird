// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "dml/update_api.hpp"
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
  Require(generated.ok(), "ODF-031 UUID generation failed");
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

api::EngineRowValue Row(std::string id, std::string name, std::string note) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"name", TextValue(std::move(name))});
  row.fields.push_back({"note", TextValue(std::move(note))});
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
  RequireOk(begun, "ODF-031 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "ODF-031 commit failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "odf031_update_candidates";
  table.columns.push_back({"id", "canonical=character"});
  table.columns.push_back({"name", "canonical=character"});
  table.columns.push_back({"note", "canonical=character"});
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
                ("scratchbird_odf031_" + name + "_" +
                 std::to_string(NowMillis() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf031.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-031 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.id_index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.name_index_uuid = NewUuidText(platform::UuidKind::object, salt + 12);
  fixture.note_bitmap_index_uuid =
      NewUuidText(platform::UuidKind::object, salt + 13);

  auto context = Begin(fixture, "odf031-metadata");
  RequireDiagnosticOk(api::AppendMgaTableMetadata(context, Table(fixture, context)),
                      "ODF-031 table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture,
                                context,
                                fixture.id_index_uuid,
                                "id",
                                api::kCrudIndexFamilyBtree,
                                true)),
                      "ODF-031 id index metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture,
                                context,
                                fixture.name_index_uuid,
                                "name",
                                api::kCrudIndexFamilyBtree,
                                false)),
                      "ODF-031 name index metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture,
                                context,
                                fixture.note_bitmap_index_uuid,
                                "note",
                                api::kCrudIndexFamilyBitmap,
                                false)),
                      "ODF-031 bitmap index metadata append failed");
  Commit(context);
  return fixture;
}

api::EngineInsertRowsResult InsertRow(const Fixture& fixture,
                                      const api::EngineRequestContext& context,
                                      std::string id,
                                      std::string name,
                                      std::string note) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows.push_back(
      Row(std::move(id), std::move(name), std::move(note)));
  request.estimated_row_count = 1;
  return api::EngineInsertRows(request);
}

std::string SeedRow(Fixture& fixture,
                    std::string id,
                    std::string name,
                    std::string note) {
  auto context = Begin(fixture, "odf031-seed");
  const auto inserted =
      InsertRow(fixture, context, std::move(id), std::move(name), std::move(note));
  RequireOk(inserted, "ODF-031 seed insert failed");
  Require(inserted.row_uuids.size() == 1, "ODF-031 seed row UUID missing");
  Commit(context);
  return inserted.row_uuids.front().canonical;
}

api::EngineUpdateRowsResult Update(const Fixture& fixture,
                                   api::EngineRequestContext context,
                                   api::EnginePredicateEnvelope predicate,
                                   std::string note,
                                   std::vector<std::string> options = {}) {
  api::EngineUpdateRowsRequest request;
  request.context = std::move(context);
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.update_predicate = std::move(predicate);
  request.assignments.push_back({"note", TextValue(std::move(note))});
  request.option_envelopes = std::move(options);
  return api::EngineUpdateRows(request);
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

void RequireUpdateEvidence(const api::EngineUpdateRowsResult& result,
                           std::string_view stream_kind,
                           std::string_view access_kind) {
  RequireOk(result, "ODF-031 update failed");
  Require(result.updated_count >= 1, "ODF-031 update changed no rows");
  Require(HasEvidence(result.evidence, "update_row_candidate_stream", stream_kind),
          "ODF-031 update candidate stream evidence missing");
  Require(HasEvidence(result.evidence, "update_target_access_kind", access_kind),
          "ODF-031 update target access kind evidence missing");
  Require(EvidenceContains(result.evidence,
                           "dml_target_access_plan_evidence",
                           "mga_visibility_recheck=required"),
          "ODF-031 accepted route missing MGA recheck evidence");
  Require(EvidenceContains(result.evidence,
                           "dml_target_access_plan_evidence",
                           "security_recheck=required"),
          "ODF-031 accepted route missing security recheck evidence");
}

void RequireUnsafeRouteRefused(const api::EngineUpdateRowsResult& result,
                               std::string_view diagnostic_token) {
  Require(!result.ok, "ODF-031 unsafe target access route was accepted");
  Require(!result.diagnostics.empty(),
          "ODF-031 unsafe target access route emitted no diagnostic");
  Require(result.diagnostics.front().detail.find("target_access_plan_refused") !=
              std::string::npos,
          "ODF-031 unsafe target access diagnostic detail mismatch");
  Require(EvidenceContains(result.evidence,
                           "dml_target_access_plan_refusal",
                           diagnostic_token),
          "ODF-031 unsafe target access refusal evidence missing");
  Require(HasEvidence(result.evidence,
                      "update_row_candidate_stream",
                      "refused"),
          "ODF-031 unsafe target access did not fail closed");
  Require(HasEvidence(result.evidence,
                      "update_target_access_refusal",
                      "fail_closed_unsafe_route"),
          "ODF-031 unsafe target access fail-closed evidence missing");
}

void CandidateStreamsUsePlans() {
  auto fixture = MakeFixture("streams", 31000);
  const auto row_a = SeedRow(fixture, "1", "amy", "seed-a");
  SeedRow(fixture, "2", "bob", "seed-b");
  SeedRow(fixture, "3", "cyd", "seed-c");

  {
    auto context = Begin(fixture, "odf031-row-uuid");
    const auto updated =
        Update(fixture, context, RowUuidPredicate(row_a), "row-uuid-hit");
    RequireUpdateEvidence(updated, "row_uuid_singleton", "row_uuid_singleton");
    Require(updated.updated_count == 1, "ODF-031 row UUID updated extra rows");
    Commit(context);
  }
  {
    auto context = Begin(fixture, "odf031-unique-index");
    const auto updated =
        Update(fixture, context, EqualsPredicate("id", "2"), "unique-index-hit");
    RequireUpdateEvidence(updated, "indexed_predicate", "unique_index_lookup");
    Require(EvidenceContains(updated.evidence, "index_lookup", fixture.id_index_uuid),
            "ODF-031 unique index lookup evidence missing");
    Require(updated.updated_count == 1, "ODF-031 unique index updated extra rows");
    Commit(context);
  }
  {
    auto context = Begin(fixture, "odf031-range-index");
    const auto updated = Update(fixture,
                                context,
                                RangePredicate("name", "amy", "cyd"),
                                "range-index-hit");
    RequireUpdateEvidence(updated, "indexed_predicate", "range_index_lookup");
    Require(EvidenceContains(updated.evidence, "index_lookup", fixture.name_index_uuid),
            "ODF-031 range index lookup evidence missing");
    Require(updated.updated_count == 3, "ODF-031 range index row count mismatch");
    Commit(context);
  }
}

void FallbacksAreExplicit() {
  auto fixture = MakeFixture("fallbacks", 32000);
  SeedRow(fixture, "1", "amy", "alpha seed");

  {
    auto context = Begin(fixture, "odf031-unsupported");
    api::EnginePredicateEnvelope predicate;
    predicate.predicate_kind = "text_term_contains";
    predicate.canonical_predicate_envelope = "note";
    predicate.bound_values.push_back(TextValue("alpha"));
    const auto updated = Update(fixture, context, predicate, "unsupported-hit");
    RequireOk(updated, "ODF-031 unsupported predicate fallback update failed");
    Require(HasEvidence(updated.evidence, "update_row_candidate_stream", "table_scan"),
            "ODF-031 unsupported predicate did not emit table scan fallback");
    Require(HasEvidence(updated.evidence,
                        "update_target_access_fallback",
                        "unsupported predicate"),
            "ODF-031 unsupported predicate fallback evidence missing");
    Commit(context);
  }
  {
    auto context = Begin(fixture, "odf031-unusable-index");
    const auto updated =
        Update(fixture, context, EqualsPredicate("note", "unsupported-hit"), "bitmap-hit");
    RequireOk(updated, "ODF-031 unusable index fallback update failed");
    Require(HasEvidence(updated.evidence, "update_row_candidate_stream", "table_scan"),
            "ODF-031 unusable index did not emit table scan fallback");
    Require(HasEvidence(updated.evidence,
                        "update_target_access_fallback",
                        "unusable index"),
            "ODF-031 unusable index fallback evidence missing");
    Commit(context);
  }
  {
    auto context = Begin(fixture, "odf031-missing-security");
    const auto updated =
        Update(fixture,
               context,
               EqualsPredicate("id", "1"),
               "security-fallback",
               {"odf031.force_missing_security_context=true"});
    RequireUnsafeRouteRefused(updated, "missing grants/security context");
    Commit(context);
  }
  {
    auto context = Begin(fixture, "odf031-stale-epochs");
    api::EngineUpdateRowsRequest request;
    request.context = context;
    request.bound_object_identity.catalog_generation_id = 9;
    request.bound_object_identity.security_epoch = 19;
    request.bound_object_identity.resource_epoch = 29;
    request.target_table.uuid.canonical = fixture.table_uuid;
    request.target_table.object_kind = "table";
    request.update_predicate = EqualsPredicate("id", "1");
    request.assignments.push_back({"note", TextValue("stale-fallback")});
    request.option_envelopes.push_back("odf031.observed_stats_epoch=39");
    request.option_envelopes.push_back("odf031.current_stats_epoch=40");
    const auto updated = api::EngineUpdateRows(request);
    RequireUnsafeRouteRefused(updated, "stale catalog epoch");
    Require(EvidenceContains(updated.evidence,
                             "dml_target_access_plan_refusal",
                             "stale security epoch"),
            "ODF-031 stale security epoch evidence missing");
    Require(EvidenceContains(updated.evidence,
                             "dml_target_access_plan_refusal",
                             "stale policy epoch"),
            "ODF-031 stale policy epoch evidence missing");
    Require(EvidenceContains(updated.evidence,
                             "dml_target_access_plan_refusal",
                            "stale stats epoch"),
            "ODF-031 stale stats epoch evidence missing");
    Commit(context);
  }
  {
    auto context = Begin(fixture, "odf031-unsafe-route");
    const auto updated =
        Update(fixture,
               context,
               EqualsPredicate("id", "1"),
               "unsafe-route-fallback",
               {"odf031.disable_mga_visibility_recheck=true",
                "odf031.disable_security_recheck=true",
                "odf031.parser_or_donor_authority=true"});
    RequireUnsafeRouteRefused(updated, "missing MGA recheck");
    Require(EvidenceContains(updated.evidence,
                             "dml_target_access_plan_refusal",
                             "missing security recheck"),
            "ODF-031 missing security recheck evidence missing");
    Require(EvidenceContains(updated.evidence,
                             "dml_target_access_plan_refusal",
                             "unsafe parser/donor authority"),
            "ODF-031 unsafe parser/donor authority evidence missing");
    Commit(context);
  }
}

void EvidenceHasNoRuntimeDocDependency() {
  auto fixture = MakeFixture("no_docs", 33000);
  const auto row_uuid = SeedRow(fixture, "1", "amy", "seed");
  auto context = Begin(fixture, "odf031-no-docs");
  const auto updated =
      Update(fixture, context, RowUuidPredicate(row_uuid), "no-docs-hit");
  RequireOk(updated, "ODF-031 no-doc update failed");
  for (const auto& evidence : updated.evidence) {
    for (const auto forbidden : {"docs/", "execution-plans", "findings", "audit",
                                 "contracts", "references"}) {
      Require(evidence.evidence_kind.find(forbidden) == std::string::npos &&
                  evidence.evidence_id.find(forbidden) == std::string::npos,
              "ODF-031 runtime evidence leaked forbidden documentation token");
    }
  }
  Commit(context);
}

}  // namespace

int main() {
  CandidateStreamsUsePlans();
  FallbacksAreExplicit();
  EvidenceHasNoRuntimeDocDependency();
  return 0;
}
