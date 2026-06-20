// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "backup_archive/backup_archive_api.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "filespace_header.hpp"
#include "filespace_lifecycle.hpp"
#include "memory.hpp"
#include "public_release_authz_fixture.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_cleanup.hpp"
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
namespace filespace = scratchbird::storage::filespace;
namespace memory = scratchbird::core::memory;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1771100000000ull;
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

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        evidence.evidence_id.find(id) != std::string::npos) {
      return true;
    }
  }
  return false;
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "public_ipar_p5_10_backup_restore_archive_snapshot_gate";
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
          MemoryPolicy(), "public_ipar_p5_10_backup_restore_archive_snapshot_gate");
  return Expect(configured.ok(),
                "IPAR-P5-10 memory manager should configure") &&
         Expect(configured.fixture_mode,
                "IPAR-P5-10 memory manager should use fixture mode");
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
  context.current_schema_uuid.canonical = UuidText(UuidKind::schema, 22);
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
  scratchbird::tests::release::GrantMaterializedRight(
      &context, "BACKUP_CREATE");
  return context;
}

api::EngineRequestContext RestoreContext(const DatabaseFixture& fixture,
                                         std::string request_id,
                                         u64 local_transaction_id) {
  api::EngineRequestContext context = Context(fixture, std::move(request_id));
  scratchbird::tests::release::GrantMaterializedRight(
      &context, "BACKUP_RESTORE");
  context.local_transaction_id = local_transaction_id;
  context.transaction_uuid.canonical =
      UuidText(UuidKind::transaction, 100 + local_transaction_id);
  return context;
}

bool Begin(api::EngineRequestContext* context) {
  api::EngineBeginTransactionRequest request;
  request.context = *context;
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok) {
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

api::EngineCoordinateBackupRestoreArchiveSnapshotRequest CoordinationRequest(
    const DatabaseFixture& fixture,
    const api::EngineStartLogicalBackupResult& backup) {
  api::EngineCoordinateBackupRestoreArchiveSnapshotRequest request;
  request.context = BackupContext(fixture, "ipar-p5-10-coordinate");
  request.backup_uuid = backup.backup_uuid;
  request.snapshot_uuid = backup.snapshot_uuid;
  request.snapshot_visible_through_local_transaction_id =
      backup.snapshot_visible_through_local_transaction_id;
  request.online_backup_active = true;
  request.snapshot_hold_acquired = true;
  request.filespace_hold_acquired = true;
  request.shutdown_blocker_registered = true;
  request.drop_blocker_registered = true;
  request.backup_manifest_reachable = true;
  request.archive_reclaim_requested = true;
  request.archive_before_reclaim_verified = true;
  request.restore_coordination_requested = true;
  request.restore_inspection_open = true;
  request.recovery_classification_verified = true;
  request.engine_mga_authoritative = true;
  return request;
}

txn::TransactionIdentity TransactionIdentity(u64 local_id) {
  txn::TransactionIdentity identity;
  identity.local_id = txn::MakeLocalTransactionId(local_id);
  identity.transaction_uuid = MakeUuid(UuidKind::transaction, 200 + local_id);
  identity.scope = txn::TransactionScope::local_node;
  return identity;
}

txn::RowVersionMetadata RetainedHistoryMetadata() {
  txn::RowVersionMetadata metadata;
  metadata.identity.row.row_uuid = MakeUuid(UuidKind::row, 300);
  metadata.identity.creator_transaction = TransactionIdentity(10);
  metadata.identity.version_sequence = 1;
  metadata.chain.next_version_sequence = 2;
  metadata.chain.next_version_uuid = MakeUuid(UuidKind::row, 301);
  metadata.successor_transaction_local_id = txn::MakeLocalTransactionId(11);
  metadata.state = txn::RowVersionState::committed;
  metadata.creator_transaction_state = txn::TransactionState::committed;
  metadata.payload_present = true;
  return metadata;
}

api::EngineArchiveRetainedHistoryRecord RetainedHistoryRecord() {
  api::EngineArchiveRetainedHistoryRecord record;
  record.metadata = RetainedHistoryMetadata();
  record.table_uuid = UuidText(UuidKind::object, 310);
  record.payload_digest = "fnv1a64:ipar-p5-10-retained-history";
  record.retention_class = "history_archive";
  record.retention_policy_ref = "retention.history.local.v1";
  record.key_lineage_id = "ipar-p5-10-local-key-lineage";
  return record;
}

filespace::PhysicalFilespaceHeader ArchiveHeader(TypedUuid database_uuid,
                                                 TypedUuid filespace_uuid) {
  filespace::PhysicalFilespaceHeader header;
  header.database_uuid = database_uuid;
  header.filespace_uuid = filespace_uuid;
  header.role = filespace::FilespaceRole::archive_history;
  header.state = filespace::FilespaceState::online;
  header.page_size = kPageSize;
  header.physical_filespace_id = 17;
  header.total_pages = 32;
  header.free_pages = 8;
  header.preallocated_pages = 4;
  header.allocation_root_page = 2;
  header.header_generation = 3;
  header.writer_identity_uuid = MakeUuid(UuidKind::object, 320);
  header.creation_operation_uuid = "ipar-p5-10-archive-history";
  return header;
}

filespace::FilespaceOperationRequest AttachArchiveRequest(
    const std::filesystem::path& archive_path,
    const filespace::PhysicalFilespaceHeader& header) {
  filespace::FilespaceOperationRequest attach;
  attach.operation = filespace::FilespaceOperation::attach_filespace;
  attach.database_uuid = header.database_uuid;
  attach.filespace_uuid = header.filespace_uuid;
  attach.path = archive_path.string();
  attach.role = filespace::FilespaceRole::archive_history;
  attach.page_size = header.page_size;
  attach.physical_filespace_id = header.physical_filespace_id;
  attach.total_pages = header.total_pages;
  attach.free_pages = header.free_pages;
  attach.preallocated_pages = header.preallocated_pages;
  attach.allocation_root_page = header.allocation_root_page;
  attach.header_generation = header.header_generation;
  attach.writer_identity_uuid = header.writer_identity_uuid;
  attach.reason = "ipar-p5-10-archive-filespace-attach";
  return attach;
}

bool ArchiveBeforeReclaimProof(const std::filesystem::path& work_dir,
                               const DatabaseFixture& fixture) {
  const auto archive_path = work_dir / "archive-history.sbfs";
  const auto manifest_path = work_dir / "archive-before-reclaim.manifest";
  const auto archive_filespace_uuid = MakeUuid(UuidKind::filespace, 400);
  const auto header = ArchiveHeader(fixture.database_uuid, archive_filespace_uuid);
  const auto created =
      filespace::CreatePhysicalFilespaceFile(archive_path.string(), header, true);
  bool ok = Expect(created.ok(),
                   "IPAR-P5-10 archive filespace should create");

  filespace::FilespaceRegistry registry;
  const auto attached =
      filespace::ApplyFilespaceOperation(&registry,
                                         AttachArchiveRequest(archive_path,
                                                              header));
  ok = Expect(attached.ok(),
              "IPAR-P5-10 archive filespace should attach") && ok;

  api::EngineArchiveRetainedHistoryBeforeReclaimRequest request;
  request.context = BackupContext(fixture, "ipar-p5-10-archive-before-reclaim");
  request.archive_filespace = attached.descriptor;
  request.retained_history.push_back(RetainedHistoryRecord());
  request.authoritative_cleanup_horizon_local_transaction_id = 20;
  request.engine_mga_authoritative = true;
  request.cleanup_horizon_authoritative = true;
  request.local_archive_filespace_header_verified = true;
  request.retention_policy_installed = true;
  request.max_row_versions_to_archive = 4;
  request.option_envelopes.push_back("manifest_uri:" + manifest_path.string());
  const auto archived =
      api::EngineArchiveRetainedHistoryBeforeReclaim(request);
  ok = ExpectApiOk(archived,
                   "IPAR-P5-10 archive-before-reclaim should pass") && ok;
  ok = Expect(archived.hot_reclaim_authorized &&
                  archived.manifest_verified &&
                  !archived.transaction_finality_authority,
              "IPAR-P5-10 archive proof should authorize reclaim without finality authority") &&
       ok;
  ok = Expect(HasEvidence(archived,
                          "finality_source",
                          "local_mga_transaction_inventory"),
              "IPAR-P5-10 archive proof should preserve MGA finality source") &&
       ok;
  return ok;
}

bool BackupRestoreSnapshotCoordinationProof(const std::filesystem::path& work_dir) {
  bool ok = ConfigureMemoryFixture();
  const auto source = CreateDatabaseFixture(work_dir / "source.sbdb", 1);
  const auto target = CreateDatabaseFixture(work_dir / "target.sbdb", 101);
  const auto backup_manifest = work_dir / "source.sblbk";

  auto tx1 = Context(source, "ipar-p5-10-source-tx1");
  ok = Expect(Begin(&tx1), "IPAR-P5-10 source tx1 should begin") && ok;
  const std::string schema_uuid = UuidText(UuidKind::schema, 500);
  api::EngineCreateSchemaRequest schema;
  schema.context = tx1;
  schema.target_object.uuid.canonical = schema_uuid;
  schema.localized_names.push_back({"en", "primary", "", "ipar", true});
  const auto created_schema = api::EngineCreateSchema(schema);
  ok = ExpectApiOk(created_schema,
                   "IPAR-P5-10 source schema should create") && ok;
  tx1.current_schema_uuid.canonical = schema_uuid;
  api::EngineCreateTableRequest create;
  create.context = tx1;
  create.target_schema.uuid.canonical = schema_uuid;
  create.table_names.push_back({"en", "primary", "", "ipar_items", true});
  api::EngineColumnDefinition payload_col;
  payload_col.names.push_back({"en", "primary", "", "payload", true});
  payload_col.descriptor.descriptor_kind = "datatype";
  payload_col.descriptor.canonical_type_name = "text";
  create.table_columns = {payload_col};
  const auto created = api::EngineCreateTable(create);
  ok = ExpectApiOk(created, "IPAR-P5-10 source table should create") && ok;
  ok = Expect(InsertPayload(tx1, created.table_object, "base-row"),
              "IPAR-P5-10 base row should insert") && ok;
  ok = Expect(Commit(&tx1), "IPAR-P5-10 source tx1 should commit") && ok;

  api::EngineStartLogicalBackupRequest backup_request;
  backup_request.context = BackupContext(source, "ipar-p5-10-backup");
  backup_request.option_envelopes.push_back("target_uri:" +
                                            backup_manifest.string());
  backup_request.option_envelopes.push_back("filespace_uuid:" +
                                            UuidText(source.filespace_uuid));
  const auto backup = api::EngineStartLogicalBackup(backup_request);
  ok = ExpectApiOk(backup, "IPAR-P5-10 logical backup should pass") && ok;
  ok = Expect(backup.row_count == 1 &&
                  backup.snapshot_visible_through_local_transaction_id > 0,
              "IPAR-P5-10 backup should capture committed base snapshot") &&
       ok;
  ok = Expect(HasEvidence(backup, "snapshot_hold", "") &&
                  HasEvidence(backup, "shutdown_blocker", "") &&
                  HasEvidence(backup, "drop_blocker", ""),
              "IPAR-P5-10 backup should publish online blockers") &&
       ok;

  auto coordination = CoordinationRequest(source, backup);
  coordination.mutation_kind = api::BackupSnapshotMutationKind::dml_write;
  coordination.mutation_local_transaction_id =
      backup.snapshot_visible_through_local_transaction_id + 1;
  const auto dml_decision =
      api::EngineCoordinateBackupRestoreArchiveSnapshot(coordination);
  ok = ExpectApiOk(dml_decision,
                   "IPAR-P5-10 DML coordination should pass") &&
       ok;
  ok = Expect(dml_decision.online_backup_blockers_verified &&
                  dml_decision.archive_before_reclaim_verified &&
                  dml_decision.restore_recovery_classification_verified,
              "IPAR-P5-10 coordination should verify blockers/archive/restore") &&
       ok;
  ok = Expect(!dml_decision.mutation_visible_in_snapshot &&
                  dml_decision.backup_forward_coverage_required &&
                  !dml_decision.transaction_finality_authority,
              "IPAR-P5-10 post-snapshot DML should require backup-forward coverage only") &&
       ok;

  auto ddl_after_snapshot = coordination;
  ddl_after_snapshot.mutation_kind =
      api::BackupSnapshotMutationKind::ddl_metadata_change;
  ddl_after_snapshot.mutation_local_transaction_id =
      backup.snapshot_visible_through_local_transaction_id + 2;
  const auto ddl_result =
      api::EngineCoordinateBackupRestoreArchiveSnapshot(ddl_after_snapshot);
  ok = Expect(!ddl_result.ok && ddl_result.fail_closed &&
                  ddl_result.ddl_blocked_until_snapshot_close &&
                  HasDiagnosticDetail(ddl_result,
                                      "BACKUP_SNAPSHOT_DDL_AFTER_SNAPSHOT_BLOCKED"),
              "IPAR-P5-10 post-snapshot DDL should fail closed") &&
       ok;

  auto missing_blocker = coordination;
  missing_blocker.drop_blocker_registered = false;
  const auto blocker_result =
      api::EngineCoordinateBackupRestoreArchiveSnapshot(missing_blocker);
  ok = Expect(!blocker_result.ok &&
                  HasDiagnosticDetail(blocker_result,
                                      "BACKUP_SNAPSHOT_BLOCKER_PROOF_REQUIRED"),
              "IPAR-P5-10 missing online backup blocker should fail closed") &&
       ok;

  auto missing_restore_classification = coordination;
  missing_restore_classification.recovery_classification_verified = false;
  const auto restore_classification_result =
      api::EngineCoordinateBackupRestoreArchiveSnapshot(
          missing_restore_classification);
  ok = Expect(!restore_classification_result.ok &&
                  HasDiagnosticDetail(restore_classification_result,
                                      "RESTORE_RECOVERY_CLASSIFICATION_REQUIRED"),
              "IPAR-P5-10 missing restore recovery classification should fail closed") &&
       ok;

  auto missing_archive = coordination;
  missing_archive.archive_before_reclaim_verified = false;
  const auto archive_result =
      api::EngineCoordinateBackupRestoreArchiveSnapshot(missing_archive);
  ok = Expect(!archive_result.ok &&
                  HasDiagnosticDetail(archive_result,
                                      "BACKUP_SNAPSHOT_ARCHIVE_BEFORE_RECLAIM_REQUIRED"),
              "IPAR-P5-10 missing archive-before-reclaim proof should fail closed") &&
       ok;

  api::EngineRestoreLogicalBackupRequest restore_missing;
  restore_missing.context = RestoreContext(target,
                                           "ipar-p5-10-restore-missing",
                                           77);
  restore_missing.option_envelopes.push_back("source_manifest_uri:" +
                                             backup_manifest.string());
  const auto restore_missing_result =
      api::EngineRestoreLogicalBackup(restore_missing);
  ok = Expect(!restore_missing_result.ok &&
                  HasDiagnosticDetail(restore_missing_result,
                                      "RESTORE_INSPECTION_OPEN_REQUIRED"),
              "IPAR-P5-10 restore without inspection classification should fail closed") &&
       ok;

  api::EngineRestoreLogicalBackupRequest restore_verify;
  restore_verify.context = RestoreContext(target,
                                          "ipar-p5-10-restore-verify",
                                          78);
  restore_verify.option_envelopes = {
      "source_manifest_uri:" + backup_manifest.string(),
      "restore_inspection_open:true",
      "recovery_classification:restore_inspection",
      "restore_verify_only:true"};
  const auto restore_verified = api::EngineRestoreLogicalBackup(restore_verify);
  ok = ExpectApiOk(restore_verified,
                   "IPAR-P5-10 restore verification should pass") &&
       ok;
  ok = Expect(HasEvidence(restore_verified,
                          "restore_recovery_classification",
                          "verified") &&
                  HasEvidence(restore_verified,
                              "mutation_performed",
                              "false"),
              "IPAR-P5-10 restore verify should publish classification without mutation") &&
       ok;

  ok = ArchiveBeforeReclaimProof(work_dir, source) && ok;
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path work_dir =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::temp_directory_path() /
                     "public_ipar_p5_10_backup_restore_archive_snapshot_gate";
  std::error_code ignored;
  std::filesystem::remove_all(work_dir, ignored);
  std::filesystem::create_directories(work_dir);

  const bool ok = BackupRestoreSnapshotCoordinationProof(work_dir);
  return ok ? 0 : 1;
}
