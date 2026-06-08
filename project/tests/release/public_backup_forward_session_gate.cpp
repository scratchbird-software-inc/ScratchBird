// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "backup_archive/backup_archive_api.hpp"
#include "crud_support/crud_store.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "memory.hpp"
#include "public_release_authz_fixture.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1770600000000ull;
constexpr scratchbird::core::platform::u32 kPageSize = 16384;

struct DatabaseFixture {
  std::filesystem::path path;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
};

bool Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool ExpectApiOk(const api::EngineApiResult& result,
                 std::string_view message) {
  if (!result.ok) {
    std::cerr << message;
    if (!result.diagnostics.empty()) {
      std::cerr << ": " << result.diagnostics.front().code << ':'
                << result.diagnostics.front().message_key << ':'
                << result.diagnostics.front().detail;
    }
    std::cerr << '\n';
    return false;
  }
  return true;
}

bool HasDiagnosticDetail(const api::EngineApiResult& result,
                         std::string_view detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.detail.find(detail) != std::string::npos) {
      return true;
    }
  }
  return false;
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "public_backup_forward_session_gate";
  policy.hard_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 48ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

bool ConfigureMemoryFixture() {
  const auto configured =
      memory::ConfigureDefaultMemoryManagerForFixture(
          MemoryPolicy(), "public_backup_forward_session_gate");
  return Expect(configured.ok(),
                "PCR-085 memory manager should configure") &&
         Expect(configured.fixture_mode,
                "PCR-085 memory manager should use fixture mode");
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

std::string UuidText(TypedUuid typed_uuid) {
  return uuid::UuidToString(typed_uuid.value);
}

std::string UuidText(UuidKind kind, u64 offset) {
  return UuidText(MakeUuid(kind, offset));
}

DatabaseFixture CreateDatabaseFixture(const std::filesystem::path& path,
                                      u64 seed) {
  DatabaseFixture fixture;
  fixture.path = path;
  fixture.database_uuid = MakeUuid(UuidKind::database, seed);
  fixture.filespace_uuid = MakeUuid(UuidKind::filespace, seed + 1);
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = fixture.database_uuid;
  create.filespace_uuid = fixture.filespace_uuid;
  create.page_size = kPageSize;
  create.creation_unix_epoch_millis = kBaseMillis + seed;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  return fixture;
}

api::EngineRequestContext Context(const DatabaseFixture& fixture,
                                  std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.path.string();
  context.database_uuid.canonical = UuidText(fixture.database_uuid);
  context.principal_uuid.canonical = UuidText(UuidKind::principal, 20);
  context.session_uuid.canonical = UuidText(UuidKind::object, 21);
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

api::EngineRequestContext BackupContext(const DatabaseFixture& fixture,
                                        std::string request_id) {
  api::EngineRequestContext context = Context(fixture, std::move(request_id));
  context.trace_tags.push_back("right:BACKUP_CREATE");
  scratchbird::tests::release::GrantMaterializedRight(
      &context, "BACKUP_CREATE");
  return context;
}

api::EngineRequestContext RestoreContext(const DatabaseFixture& fixture,
                                         std::string request_id) {
  api::EngineRequestContext context = Context(fixture, std::move(request_id));
  context.trace_tags.push_back("right:BACKUP_RESTORE");
  scratchbird::tests::release::GrantMaterializedRight(
      &context, "BACKUP_RESTORE");
  return context;
}

bool Begin(api::EngineRequestContext* context) {
  api::EngineBeginTransactionRequest request;
  request.context = *context;
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok) {
    if (!begun.diagnostics.empty()) {
      std::cerr << "PCR-085 begin diagnostic: "
                << begun.diagnostics.front().code << ':'
                << begun.diagnostics.front().message_key << ':'
                << begun.diagnostics.front().detail << '\n';
    }
    return false;
  }
  context->local_transaction_id = begun.local_transaction_id;
  context->transaction_uuid = begun.transaction_uuid;
  context->snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context->transaction_isolation_level = begun.isolation_level;
  return true;
}

bool Commit(api::EngineRequestContext* context) {
  api::EngineCommitTransactionRequest request;
  request.context = *context;
  const auto committed = api::EngineCommitTransaction(request);
  if (!committed.ok) {
    return false;
  }
  context->local_transaction_id = 0;
  context->transaction_uuid.canonical.clear();
  return true;
}

api::EngineTypedValue TextValue(const std::string& value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "datatype";
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = value;
  return typed;
}

bool ContainsValue(const api::EngineApiResult& result,
                   const std::string& value) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.second.encoded_value.find(value) != std::string::npos) {
        return true;
      }
    }
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) {
      return true;
    }
  }
  return false;
}

bool InsertPayload(api::EngineRequestContext context,
                   api::EngineObjectReference table,
                   const std::string& payload) {
  api::EngineInsertRowsRequest insert;
  insert.context = context;
  insert.target_table = table;
  api::EngineRowValue row;
  row.fields.push_back({"payload", TextValue(payload)});
  insert.input_rows = {row};
  const auto inserted = api::EngineInsertRows(insert);
  return inserted.ok && inserted.inserted_count == 1;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

bool ManifestHasField(const std::string& manifest,
                      std::string_view kind,
                      std::string_view key,
                      std::string_view value) {
  std::istringstream lines(manifest);
  std::string line;
  while (std::getline(lines, line)) {
    const auto separator = line.find('\t');
    if (separator == std::string::npos || line.substr(0, separator) != kind) {
      continue;
    }
    for (const auto& field : api::DecodeCrudPairs(line.substr(separator + 1))) {
      if (field.first == key && (value.empty() || field.second == value)) {
        return true;
      }
    }
  }
  return false;
}

void AddRestoreInspectionOptions(std::vector<std::string>* options) {
  options->push_back("restore_inspection_open:true");
  options->push_back("recovery_classification_verified:true");
}

bool BackupForwardSessionProof(const std::filesystem::path& work_dir) {
  bool ok = true;
  const auto source = CreateDatabaseFixture(work_dir / "source.sbdb", 1);
  const auto target = CreateDatabaseFixture(work_dir / "target.sbdb", 101);
  const auto base_manifest = work_dir / "base.sblbk";
  const auto delta_manifest = work_dir / "write-after.sbdlt";
  const std::string timeline_uuid = UuidText(UuidKind::object, 200);
  const std::string fork_uuid = UuidText(UuidKind::object, 201);

  auto tx1 = Context(source, "pcr085-source-tx1");
  ok = Expect(Begin(&tx1), "PCR-085 source tx1 should begin") && ok;
  api::EngineCreateTableRequest create;
  create.context = tx1;
  create.table_names.push_back({"en", "primary", "", "backup_forward_items", true});
  api::EngineColumnDefinition payload_col;
  payload_col.names.push_back({"en", "primary", "", "payload", true});
  payload_col.descriptor.descriptor_kind = "datatype";
  payload_col.descriptor.canonical_type_name = "text";
  create.table_columns = {payload_col};
  const auto created = api::EngineCreateTable(create);
  ok = ExpectApiOk(created, "PCR-085 source table should create") && ok;
  ok = Expect(InsertPayload(tx1, created.table_object, "base-alpha"),
              "PCR-085 base row should insert") && ok;
  ok = Expect(Commit(&tx1), "PCR-085 source tx1 should commit") && ok;

  api::EngineStartLogicalBackupRequest base_backup;
  base_backup.context = BackupContext(source, "pcr085-base-backup");
  base_backup.option_envelopes.push_back("target_uri:" + base_manifest.string());
  base_backup.option_envelopes.push_back("filespace_uuid:" + UuidText(source.filespace_uuid));
  base_backup.option_envelopes.push_back("timeline_uuid:" + timeline_uuid);
  base_backup.option_envelopes.push_back("fork_uuid:" + fork_uuid);
  const auto base = api::EngineStartLogicalBackup(base_backup);
  ok = ExpectApiOk(base, "PCR-085 base logical backup should succeed") && ok;
  ok = Expect(base.row_count == 1,
              "PCR-085 base backup should capture the base row") && ok;

  api::EngineStartBackupForwardSessionRequest start;
  start.context = BackupContext(source, "pcr085-backup-forward-start");
  start.base_backup_uuid = base.backup_uuid;
  start.base_snapshot_visible_through_local_transaction_id =
      base.snapshot_visible_through_local_transaction_id;
  start.source_manifest_uri = base_manifest.string();
  start.filespace_uuid = UuidText(source.filespace_uuid);
  start.timeline_uuid = timeline_uuid;
  start.fork_uuid = fork_uuid;
  const auto session = api::EngineStartBackupForwardSession(start);
  ok = ExpectApiOk(session,
                   "PCR-085 backup-forward session should start") && ok;
  ok = Expect(session.selected_start_transaction_id ==
                  base.snapshot_visible_through_local_transaction_id + 1,
              "PCR-085 selected transaction should follow base snapshot") &&
       ok;
  ok = Expect(!session.write_after_recovery_authority &&
                  !session.transaction_finality_authority,
              "PCR-085 backup-forward session must be non-authority") &&
       ok;

  auto tx2 = Context(source, "pcr085-source-tx2");
  ok = Expect(Begin(&tx2), "PCR-085 source tx2 should begin") && ok;
  const auto finish_transaction_id = tx2.local_transaction_id;
  ok = Expect(InsertPayload(tx2, created.table_object, "delta-beta"),
              "PCR-085 delta row should insert") && ok;
  ok = Expect(Commit(&tx2), "PCR-085 source tx2 should commit") && ok;

  api::EngineFinishBackupForwardSessionRequest finish;
  finish.context = BackupContext(source, "pcr085-backup-forward-finish");
  finish.session_uuid = session.session_uuid;
  finish.base_backup_uuid = base.backup_uuid;
  finish.source_manifest_uri = base_manifest.string();
  finish.delta_manifest_uri = delta_manifest.string();
  finish.filespace_uuid = UuidText(source.filespace_uuid);
  finish.timeline_uuid = timeline_uuid;
  finish.fork_uuid = fork_uuid;
  finish.selected_start_transaction_id = session.selected_start_transaction_id;
  finish.finish_transaction_id = finish_transaction_id;
  finish.expected_previous_end_transaction_id =
      base.snapshot_visible_through_local_transaction_id;
  const auto finished = api::EngineFinishBackupForwardSession(finish);
  ok = ExpectApiOk(finished,
                   "PCR-085 backup-forward session should finish") && ok;
  ok = Expect(finished.coverage_contiguous &&
                  finished.write_after_segment_immutable,
              "PCR-085 finish should prove contiguous immutable write-after") &&
       ok;
  ok = Expect(finished.packaged_row_count == 1,
              "PCR-085 finish should package the write-after row") && ok;
  ok = Expect(!finished.write_after_recovery_authority &&
                  !finished.cluster_recovery_authority &&
                  !finished.transaction_finality_authority,
              "PCR-085 write-after segment must remain non-authority") &&
       ok;
  ok = Expect(HasEvidence(finished, "coverage_contiguous", "true") &&
                  HasEvidence(finished,
                              "finality_source",
                              "local_mga_transaction_inventory"),
              "PCR-085 finish evidence should record coverage and finality source") &&
       ok;

  const std::string delta_body = ReadFile(delta_manifest);
  ok = Expect(ManifestHasField(delta_body,
                               "META",
                               "source_backup_uuid",
                               base.backup_uuid.canonical),
              "PCR-085 delta manifest should bind source backup UUID") &&
       ok;
  ok = Expect(ManifestHasField(delta_body, "META", "coverage_contiguous", "true"),
              "PCR-085 delta manifest should prove contiguous coverage") &&
       ok;
  ok = Expect(ManifestHasField(delta_body,
                               "META",
                               "finality_source",
                               "local_mga_transaction_inventory"),
              "PCR-085 delta manifest should use MGA finality source") &&
       ok;
  ok = Expect(ManifestHasField(delta_body, "META", "authoritative_wal", "false"),
              "PCR-085 delta manifest should disclaim WAL authority") &&
       ok;
  ok = Expect(ManifestHasField(delta_body,
                               "META",
                               "replay_profile",
                               "deterministic_mga_operation_envelopes"),
              "PCR-085 delta manifest should identify deterministic replay profile") &&
       ok;

  auto restore_tx = RestoreContext(target, "pcr085-target-restore");
  ok = Expect(Begin(&restore_tx), "PCR-085 target restore tx should begin") && ok;
  api::EngineRestoreLogicalBackupRequest restore_base;
  restore_base.context = restore_tx;
  restore_base.option_envelopes.push_back("source_manifest_uri:" + base_manifest.string());
  AddRestoreInspectionOptions(&restore_base.option_envelopes);
  const auto restored = api::EngineRestoreLogicalBackup(restore_base);
  ok = ExpectApiOk(restored, "PCR-085 base restore should succeed") && ok;

  api::EngineApplyDeltaStreamRequest apply_delta;
  apply_delta.context = restore_tx;
  apply_delta.option_envelopes.push_back("source_manifest_uri:" + delta_manifest.string());
  apply_delta.option_envelopes.push_back(
      "expected_previous_end_transaction_id:" +
      std::to_string(base.snapshot_visible_through_local_transaction_id));
  AddRestoreInspectionOptions(&apply_delta.option_envelopes);
  const auto applied = api::EngineApplyDeltaStream(apply_delta);
  ok = ExpectApiOk(applied, "PCR-085 delta apply should succeed") && ok;
  ok = Expect(applied.applied_row_count == 1,
              "PCR-085 delta apply should apply one row") && ok;
  const auto applied_again = api::EngineApplyDeltaStream(apply_delta);
  ok = ExpectApiOk(applied_again,
                   "PCR-085 delta apply should be idempotent") && ok;
  ok = Expect(applied_again.applied_row_count == 0,
              "PCR-085 second delta apply should skip already-applied row") &&
       ok;
  ok = Expect(Commit(&restore_tx), "PCR-085 target restore tx should commit") && ok;

  auto read = Context(target, "pcr085-target-read");
  ok = Expect(Begin(&read), "PCR-085 target read tx should begin") && ok;
  api::EngineSelectRowsRequest select;
  select.context = read;
  select.source_object = created.table_object;
  const auto selected = api::EngineSelectRows(select);
  ok = ExpectApiOk(selected, "PCR-085 target select should succeed") && ok;
  ok = Expect(selected.visible_count == 2,
              "PCR-085 target should contain base plus write-after rows") && ok;
  ok = Expect(ContainsValue(selected, "base-alpha") &&
                  ContainsValue(selected, "delta-beta"),
              "PCR-085 restored values should round trip") && ok;
  ok = Expect(Commit(&read), "PCR-085 target read tx should commit") && ok;
  return ok;
}

bool FailClosedProof(const std::filesystem::path& work_dir) {
  bool ok = true;
  const auto source = CreateDatabaseFixture(work_dir / "fail-source.sbdb", 301);
  const auto manifest = work_dir / "fail.sbdlt";

  api::EngineStartBackupForwardSessionRequest missing_base;
  missing_base.context = BackupContext(source, "pcr085-missing-base");
  missing_base.filespace_uuid = UuidText(source.filespace_uuid);
  missing_base.source_manifest_uri = manifest.string();
  const auto missing_base_result =
      api::EngineStartBackupForwardSession(missing_base);
  ok = Expect(!missing_base_result.ok &&
                  HasDiagnosticDetail(missing_base_result,
                                      "BACKUP_FORWARD_BASE_SNAPSHOT_REQUIRED"),
              "PCR-085 missing base snapshot should fail closed") &&
       ok;

  api::EngineFinishBackupForwardSessionRequest finish;
  finish.context = BackupContext(source, "pcr085-fail-finish");
  finish.session_uuid.canonical = UuidText(UuidKind::object, 400);
  finish.base_backup_uuid.canonical = UuidText(UuidKind::object, 401);
  finish.source_manifest_uri = (work_dir / "base.sblbk").string();
  finish.delta_manifest_uri = manifest.string();
  finish.filespace_uuid = UuidText(source.filespace_uuid);
  finish.selected_start_transaction_id = 2;
  finish.finish_transaction_id = 2;

  auto cluster = finish;
  cluster.option_envelopes.push_back("scope:cluster");
  const auto cluster_result = api::EngineFinishBackupForwardSession(cluster);
  ok = Expect(!cluster_result.ok && cluster_result.cluster_authority_required &&
                  HasDiagnosticDetail(cluster_result,
                                      "BACKUP_FORWARD_CLUSTER_PROVIDER_REQUIRED"),
              "PCR-085 cluster backup-forward should fail closed") &&
       ok;

  auto wal = finish;
  wal.option_envelopes.push_back("authoritative_wal:true");
  const auto wal_result = api::EngineFinishBackupForwardSession(wal);
  ok = Expect(!wal_result.ok &&
                  HasDiagnosticDetail(wal_result,
                                      "BACKUP_FORWARD_AUTHORITATIVE_WAL_FORBIDDEN"),
              "PCR-085 authoritative WAL backup-forward should fail closed") &&
       ok;

  auto gap = finish;
  gap.expected_previous_end_transaction_id = 10;
  const auto gap_result = api::EngineFinishBackupForwardSession(gap);
  ok = Expect(!gap_result.ok &&
                  HasDiagnosticDetail(gap_result,
                                      "BACKUP_FORWARD_COVERAGE_GAP"),
              "PCR-085 coverage gap should fail closed") &&
       ok;
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path work_dir =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::temp_directory_path() /
                     "public_backup_forward_session_gate";
  std::error_code ignored;
  std::filesystem::remove_all(work_dir, ignored);
  std::filesystem::create_directories(work_dir);

  bool ok = true;
  ok = ConfigureMemoryFixture() && ok;
  ok = BackupForwardSessionProof(work_dir) && ok;
  ok = FailClosedProof(work_dir) && ok;
  return ok ? 0 : 1;
}
