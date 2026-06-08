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
#include "row_version.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_inventory.hpp"
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
namespace mga = scratchbird::transaction::mga;
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
  Require(generated.ok(), "ODF-050 UUID generation failed");
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
  context.catalog_generation_id = 10;
  context.security_epoch = 20;
  context.resource_epoch = 30;
  context.name_resolution_epoch = 40;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture,
                                std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "ODF-050 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "ODF-050 commit failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "odf050_hot_plus";
  table.columns.push_back({"id", "canonical=character"});
  table.columns.push_back({"name", "canonical=character"});
  table.columns.push_back({"note", "canonical=character"});
  return table;
}

api::CrudIndexRecord Index(const Fixture& fixture,
                           const api::EngineRequestContext& context,
                           std::string index_uuid,
                           std::string column,
                           bool unique) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = std::move(index_uuid);
  index.table_uuid = fixture.table_uuid;
  index.column_name = std::move(column);
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = unique;
  index.key_envelopes.push_back(index.column_name);
  if (unique) { index.key_envelopes.push_back("unique"); }
  return index;
}

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_odf050_" + name + "_" +
                 std::to_string(NowMillis() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf050.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-050 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.id_index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.name_index_uuid = NewUuidText(platform::UuidKind::object, salt + 12);

  auto context = Begin(fixture, "odf050-metadata");
  RequireDiagnosticOk(api::AppendMgaTableMetadata(context, Table(fixture, context)),
                      "ODF-050 table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture, context, fixture.id_index_uuid, "id", true)),
                      "ODF-050 id index metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture, context, fixture.name_index_uuid, "name", false)),
                      "ODF-050 name index metadata append failed");
  Commit(context);
  return fixture;
}

api::EnginePredicateEnvelope EqualsPredicate(std::string column,
                                             std::string value) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_equals";
  predicate.canonical_predicate_envelope = std::move(column);
  predicate.bound_values.push_back(TextValue(std::move(value)));
  return predicate;
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
  auto context = Begin(fixture, "odf050-seed");
  const auto inserted =
      InsertRow(fixture, context, std::move(id), std::move(name), std::move(note));
  RequireOk(inserted, "ODF-050 seed insert failed");
  Require(inserted.row_uuids.size() == 1, "ODF-050 seed row UUID missing");
  Commit(context);
  return inserted.row_uuids.front().canonical;
}

api::EngineUpdateRowsResult UpdateOne(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    api::EnginePredicateEnvelope predicate,
    std::vector<std::pair<std::string, api::EngineTypedValue>> assignments) {
  api::EngineUpdateRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.update_predicate = std::move(predicate);
  request.assignments = std::move(assignments);
  return api::EngineUpdateRows(request);
}

api::EngineSelectRowsResult SelectWhere(const Fixture& fixture,
                                        const api::EngineRequestContext& context,
                                        api::EnginePredicateEnvelope predicate) {
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = fixture.table_uuid;
  request.source_object.object_kind = "table";
  request.select_predicate = std::move(predicate);
  return api::EngineSelectRows(request);
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

std::string EvidenceValue(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind) { return entry.evidence_id; }
  }
  return {};
}

std::string FieldValue(const api::EngineSelectRowsResult& result,
                       std::string_view field) {
  Require(result.result_shape.rows.size() == 1, "ODF-050 expected one row");
  for (const auto& [name, value] : result.result_shape.rows.front().fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

api::CrudState LoadState(const Fixture& fixture,
                         const api::EngineRequestContext& context) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "ODF-050 load MGA relation store failed");
  return api::BuildCrudCompatibilityStateFromMga(loaded.state);
}

std::vector<api::CrudRowVersionRecord> VersionsForRow(
    const api::CrudState& state,
    const std::string& row_uuid) {
  std::vector<api::CrudRowVersionRecord> versions;
  for (const auto& row : state.row_versions) {
    if (row.row_uuid == row_uuid) { versions.push_back(row); }
  }
  return versions;
}

std::size_t IndexEntryCount(const api::CrudState& state,
                            const std::string& table_uuid) {
  std::size_t count = 0;
  for (const auto& entry : state.index_entries) {
    if (entry.table_uuid == table_uuid) { ++count; }
  }
  return count;
}

void RequireNoRuntimeDocTokens(
    const std::vector<api::EngineEvidenceReference>& evidence) {
  for (const auto& item : evidence) {
    for (const auto forbidden : {"docs/", "execution-plans", "findings",
                                 "contracts", "references"}) {
      Require(item.evidence_kind.find(forbidden) == std::string::npos &&
                  item.evidence_id.find(forbidden) == std::string::npos,
              "ODF-050 runtime evidence leaked documentation token");
    }
  }
}

void PageLocalHotKeepsStableRowHead() {
  auto fixture = MakeFixture("page_local", 50000);
  const auto row_uuid = SeedRow(fixture, "1", "amy", "seed");
  auto context = Begin(fixture, "odf050-page-local");
  const auto before = LoadState(fixture, context);
  const auto before_index_entries = IndexEntryCount(before, fixture.table_uuid);

  const auto updated = UpdateOne(
      fixture,
      context,
      EqualsPredicate("id", "1"),
      {{"note", TextValue("page-local-hot")}});
  RequireOk(updated, "ODF-050 page-local update failed");
  Require(updated.updated_count == 1, "ODF-050 page-local update count mismatch");
  Require(HasEvidence(updated.evidence, "hot_plus_decision", "page_local_hot"),
          "ODF-050 page-local HOT decision evidence missing");
  Require(EvidenceValue(updated.evidence, "hot_plus_exact_secondary_churn_avoided") != "0",
          "ODF-050 exact secondary churn was not avoided");
  Require(EvidenceValue(updated.evidence, "hot_plus_mga_visibility_proof_accepted") == "1",
          "ODF-050 MGA proof was not accepted");

  const auto after = LoadState(fixture, context);
  Require(IndexEntryCount(after, fixture.table_uuid) == before_index_entries,
          "ODF-050 unchanged-key update appended exact index entries");
  const auto versions = VersionsForRow(after, row_uuid);
  Require(versions.size() == 2, "ODF-050 linked row version was not appended");
  Require(versions[1].row_uuid == versions[0].row_uuid,
          "ODF-050 row UUID changed across HOT-plus update");
  Require(versions[1].previous_version_uuid == versions[0].version_uuid,
          "ODF-050 previous version UUID was not linked");
  Require(versions[1].previous_sequence == versions[0].sequence,
          "ODF-050 previous sequence was not linked");

  const auto selected = SelectWhere(fixture, context, EqualsPredicate("id", "1"));
  RequireOk(selected, "ODF-050 indexed lookup after page-local update failed");
  Require(FieldValue(selected, "note") == "page-local-hot",
          "ODF-050 old index entry did not resolve the new visible value");
  RequireNoRuntimeDocTokens(updated.evidence);
  Commit(context);
}

void LargeUnchangedKeyUsesStableHeadIndirection() {
  auto fixture = MakeFixture("stable_head", 51000);
  SeedRow(fixture, "2", "bea", "seed");
  auto context = Begin(fixture, "odf050-stable-head");
  const std::string large_note(6000, 'x');
  const auto updated = UpdateOne(
      fixture,
      context,
      EqualsPredicate("id", "2"),
      {{"note", TextValue(large_note)}});
  RequireOk(updated, "ODF-050 stable-head update failed");
  Require(HasEvidence(updated.evidence,
                      "hot_plus_decision",
                      "stable_row_head_indirection"),
          "ODF-050 large unchanged-key update did not choose stable row head");
  Require(EvidenceValue(updated.evidence,
                        "hot_plus_ordinary_index_rewrite_updates") == "0",
          "ODF-050 stable-head update fell back to ordinary rewrite");
  Require(EvidenceValue(updated.evidence,
                        "hot_plus_mga_visibility_proof_accepted") == "1",
          "ODF-050 stable-head proof was not accepted");
  Commit(context);
}

void KeyChangingUpdateUsesOrdinaryRewrite() {
  auto fixture = MakeFixture("ordinary", 52000);
  SeedRow(fixture, "3", "cyd", "seed");
  auto context = Begin(fixture, "odf050-ordinary");
  const auto updated = UpdateOne(
      fixture,
      context,
      EqualsPredicate("id", "3"),
      {{"id", TextValue("4")}});
  RequireOk(updated, "ODF-050 key-changing update failed");
  Require(HasEvidence(updated.evidence,
                      "hot_plus_decision",
                      "ordinary_index_rewrite"),
          "ODF-050 key-changing update did not choose ordinary rewrite");

  const auto new_key = SelectWhere(fixture, context, EqualsPredicate("id", "4"));
  RequireOk(new_key, "ODF-050 new-key lookup failed");
  Require(new_key.visible_count == 1, "ODF-050 new key was not indexed");
  const auto old_key = SelectWhere(fixture, context, EqualsPredicate("id", "3"));
  RequireOk(old_key, "ODF-050 old-key lookup failed");
  Require(old_key.visible_count == 0,
          "ODF-050 old predicate still returned the changed row");
  Commit(context);
}

mga::RowVersionMetadata VersionMetadata(
    const mga::RowIdentity& row,
    const mga::TransactionIdentity& creator,
    platform::u64 sequence,
    platform::u64 previous_sequence,
    platform::TypedUuid previous_version_uuid) {
  mga::RowVersionMetadata metadata;
  metadata.identity.row = row;
  metadata.identity.creator_transaction = creator;
  metadata.identity.version_sequence = sequence;
  metadata.chain.previous_version_sequence = previous_sequence;
  metadata.chain.previous_version_uuid = previous_version_uuid;
  metadata.state = mga::RowVersionState::uncommitted;
  metadata.creator_transaction_state = mga::TransactionState::active;
  metadata.payload_present = true;
  return metadata;
}

void InvalidHotPlusProofFailsClosed() {
  const auto row_uuid = NewUuid(platform::UuidKind::row, 53000);
  const auto old_version_uuid = NewUuid(platform::UuidKind::row, 53001);
  const auto tx_uuid = NewUuid(platform::UuidKind::transaction, 53002);
  const auto identity_result = mga::MakeTransactionIdentity(
      mga::MakeLocalTransactionId(7),
      tx_uuid,
      mga::TransactionScope::local_node);
  Require(identity_result.ok(), "ODF-050 transaction identity failed");
  const auto row_result = mga::MakeRowIdentity(row_uuid);
  Require(row_result.ok(), "ODF-050 row identity failed");

  mga::HotStableRowHeadProofInput input;
  input.old_visible_version = VersionMetadata(
      row_result.identity,
      identity_result.identity,
      10,
      mga::kInvalidRowVersionSequence,
      platform::TypedUuid{});
  input.new_version = VersionMetadata(
      row_result.identity,
      identity_result.identity,
      20,
      9,
      old_version_uuid);
  input.old_version_uuid = old_version_uuid;
  input.new_previous_version_uuid = old_version_uuid;
  input.visibility_snapshot.reader_transaction = mga::MakeLocalTransactionId(7);
  input.visibility_snapshot.allow_reader_own_uncommitted = true;
  input.exact_index_keys_unchanged = true;
  input.same_page_budget_available = true;

  const auto decision = mga::EvaluateHotStableRowHeadDecision(input);
  Require(!decision.ok(), "ODF-050 invalid HOT-plus proof did not fail closed");
  Require(decision.diagnostic.diagnostic_code ==
              "SB-MGA-HOT-STABLE-HEAD-PREVIOUS-SEQUENCE-MISMATCH",
          "ODF-050 invalid proof diagnostic mismatch");
}

}  // namespace

int main() {
  PageLocalHotKeepsStableRowHead();
  LargeUnchangedKeyUsesStableHeadIndirection();
  KeyChangingUpdateUsesOrdinaryRewrite();
  InvalidHotPlusProofFailsClosed();
  return 0;
}
