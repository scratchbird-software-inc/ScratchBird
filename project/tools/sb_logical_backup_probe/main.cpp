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
#include "dml/select_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
  }
  return true;
}

scratchbird::core::platform::TypedUuid Generate(scratchbird::core::platform::UuidKind kind, std::uint64_t millis) {
  auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, millis);
  return generated.ok() ? generated.value : scratchbird::core::platform::TypedUuid{};
}

std::uint64_t NowMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool CreateDatabase(const std::string& path, std::uint64_t seed) {
  std::filesystem::remove(path);
  std::filesystem::remove(path + ".sb.owner.lock");
  scratchbird::storage::database::DatabaseCreateConfig create;
  create.path = path;
  create.database_uuid = Generate(scratchbird::core::platform::UuidKind::database, seed);
  create.filespace_uuid = Generate(scratchbird::core::platform::UuidKind::filespace, seed + 1);
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  return create.database_uuid.valid() && create.filespace_uuid.valid() &&
         scratchbird::storage::database::CreateDatabaseFile(create).ok();
}

scratchbird::engine::internal_api::EngineRequestContext BaseContext(const std::string& path) {
  scratchbird::engine::internal_api::EngineRequestContext context;
  context.database_path = path;
  context.database_uuid.canonical = "018f1000-0000-7000-8000-000000000001";
  context.session_uuid.canonical = "018f1000-0000-7000-8000-000000000002";
  context.principal_uuid.canonical = "018f1000-0000-7000-8000-000000000003";
  context.security_context_present = true;
  return context;
}

bool Begin(scratchbird::engine::internal_api::EngineRequestContext* context) {
  scratchbird::engine::internal_api::EngineBeginTransactionRequest begin;
  begin.context = *context;
  auto result = scratchbird::engine::internal_api::EngineBeginTransaction(begin);
  if (!result.ok) { return false; }
  context->local_transaction_id = result.local_transaction_id;
  context->transaction_uuid = result.transaction_uuid;
  context->snapshot_visible_through_local_transaction_id = result.snapshot_visible_through_local_transaction_id;
  context->transaction_isolation_level = result.isolation_level;
  return true;
}

bool Commit(scratchbird::engine::internal_api::EngineRequestContext* context) {
  scratchbird::engine::internal_api::EngineCommitTransactionRequest commit;
  commit.context = *context;
  auto result = scratchbird::engine::internal_api::EngineCommitTransaction(commit);
  if (!result.ok) { return false; }
  context->local_transaction_id = 0;
  context->transaction_uuid.canonical.clear();
  context->snapshot_visible_through_local_transaction_id = 0;
  return true;
}

scratchbird::engine::internal_api::EngineTypedValue TextValue(const std::string& value) {
  scratchbird::engine::internal_api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "datatype";
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = value;
  return typed;
}

bool ContainsValue(const scratchbird::engine::internal_api::EngineApiResult& result, const std::string& value) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.second.encoded_value.find(value) != std::string::npos) { return true; }
    }
  }
  return false;
}

bool ContainsEvidence(const scratchbird::engine::internal_api::EngineApiResult& result,
                      const std::string& kind,
                      const std::string& id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) { return true; }
  }
  return false;
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string HexEncode(const std::string& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (unsigned char c : value) {
    out.push_back(kHex[(c >> 4) & 0x0f]);
    out.push_back(kHex[c & 0x0f]);
  }
  return out;
}

}  // namespace

int main() {
  using namespace scratchbird::engine::internal_api;
  const auto stamp = std::to_string(NowMillis());
  const std::string source_path = "/tmp/sb_logical_backup_source_" + stamp + ".db";
  const std::string target_path = "/tmp/sb_logical_backup_target_" + stamp + ".db";
  const std::string gap_path = "/tmp/sb_logical_backup_gap_" + stamp + ".db";
  const std::string manifest_path = "/tmp/sb_logical_backup_" + stamp + ".sblbk";
  const std::string gap_manifest_path = "/tmp/sb_logical_backup_gap_" + stamp + ".sblbk";
  std::filesystem::remove(manifest_path);
  std::filesystem::remove(gap_manifest_path);

  bool ok = true;
  ok &= Require(CreateDatabase(source_path, NowMillis()), "source database created");
  ok &= Require(CreateDatabase(target_path, NowMillis() + 10), "target database created");
  ok &= Require(CreateDatabase(gap_path, NowMillis() + 20), "gap database created");

  auto source = BaseContext(source_path);
  ok &= Require(Begin(&source), "source transaction begins");

  EngineCreateTableRequest create_table;
  create_table.context = source;
  create_table.table_names.push_back({"en", "primary", "", "backup_items", true});
  EngineColumnDefinition id_col;
  id_col.names.push_back({"en", "primary", "", "id", true});
  id_col.descriptor.descriptor_kind = "datatype";
  id_col.descriptor.canonical_type_name = "text";
  EngineColumnDefinition value_col;
  value_col.names.push_back({"en", "primary", "", "payload", true});
  value_col.descriptor.descriptor_kind = "datatype";
  value_col.descriptor.canonical_type_name = "text";
  create_table.table_columns = {id_col, value_col};
  auto create_result = EngineCreateTable(create_table);
  ok &= Require(create_result.ok, "source table created");

  EngineInsertRowsRequest insert;
  insert.context = source;
  insert.target_table = create_result.table_object;
  EngineRowValue row1;
  row1.fields.push_back({"id", TextValue("1")});
  row1.fields.push_back({"payload", TextValue("alpha")});
  EngineRowValue row2;
  row2.fields.push_back({"id", TextValue("2")});
  row2.fields.push_back({"payload", TextValue("beta")});
  insert.input_rows = {row1, row2};
  auto insert_result = EngineInsertRows(insert);
  ok &= Require(insert_result.ok && insert_result.inserted_count == 2, "source rows inserted");
  ok &= Require(Commit(&source), "source transaction commits");

  EngineStartLogicalBackupRequest backup;
  backup.context = BaseContext(source_path);
  backup.context.trace_tags = {"right:BACKUP_CREATE"};
  backup.option_envelopes.push_back("target_uri:" + manifest_path);
  auto backup_result = EngineStartLogicalBackup(backup);
  ok &= Require(backup_result.ok, "logical backup succeeds");
  ok &= Require(backup_result.table_count == 1 && backup_result.row_count == 2, "logical backup captures visible table and rows");
  ok &= Require(ContainsEvidence(backup_result, "authoritative_wal", "false"),
                "logical backup records non-WAL authority");
  ok &= Require(ContainsEvidence(backup_result, "lineage_source", "mga_row_version_lineage"),
                "logical backup records MGA lineage source evidence");
  ok &= Require(ContainsEvidence(backup_result, "finality_boundary_local_transaction_id",
                                 std::to_string(backup_result.snapshot_visible_through_local_transaction_id)),
                "logical backup records finality boundary evidence");
  ok &= Require(ContainsEvidence(backup_result, "archive_retention_max_age_microseconds", "604800000000"),
                "logical backup records default archive retention max age evidence");
  ok &= Require(ContainsEvidence(backup_result, "archive_slice_bytes",
                                 std::to_string(std::filesystem::file_size(manifest_path))),
                "logical backup records archive slice byte evidence");

  const auto manifest = ReadFile(manifest_path);
  ok &= Require(manifest.find(HexEncode("archive_slice_kind")) != std::string::npos &&
                manifest.find(HexEncode("mga_logical_snapshot")) != std::string::npos,
                "logical backup manifest records archive slice kind");
  ok &= Require(manifest.find(HexEncode("lineage_source")) != std::string::npos &&
                manifest.find(HexEncode("mga_row_version_lineage")) != std::string::npos,
                "logical backup manifest records lineage source");
  ok &= Require(manifest.find(HexEncode("finality_boundary_local_transaction_id")) != std::string::npos &&
                manifest.find(HexEncode("finality_source")) != std::string::npos,
                "logical backup manifest records finality fields");
  ok &= Require(manifest.find(HexEncode("creator_transaction_state")) != std::string::npos &&
                manifest.find(HexEncode("lineage_checksum")) != std::string::npos &&
                manifest.find(HexEncode("previous_version_uuid")) != std::string::npos,
                "logical backup manifest records row-version lineage fields");

  EngineStartLogicalBackupRequest denied_backup = backup;
  denied_backup.context.trace_tags.clear();
  ok &= Require(!EngineStartLogicalBackup(denied_backup).ok, "logical backup denied without right");

  auto gap_active = BaseContext(gap_path);
  ok &= Require(Begin(&gap_active), "gap active transaction begins");
  auto gap_committed = BaseContext(gap_path);
  ok &= Require(Begin(&gap_committed), "gap committed transaction begins");
  ok &= Require(Commit(&gap_committed), "gap committed transaction commits");
  EngineStartLogicalBackupRequest gap_backup;
  gap_backup.context = BaseContext(gap_path);
  gap_backup.context.trace_tags = {"right:BACKUP_CREATE"};
  gap_backup.option_envelopes.push_back("target_uri:" + gap_manifest_path);
  ok &= Require(!EngineStartLogicalBackup(gap_backup).ok, "logical backup refuses unsafe finality gap");

  auto target = BaseContext(target_path);
  ok &= Require(Begin(&target), "target transaction begins");
  EngineRestoreLogicalBackupRequest restore;
  restore.context = target;
  restore.context.trace_tags = {"right:BACKUP_RESTORE"};
  restore.option_envelopes.push_back("source_manifest_uri:" + manifest_path);
  auto restore_result = EngineRestoreLogicalBackup(restore);
  ok &= Require(restore_result.ok && restore_result.restored_row_count == 2, "logical restore succeeds");
  ok &= Require(Commit(&target), "target transaction commits");

  auto read_target = BaseContext(target_path);
  ok &= Require(Begin(&read_target), "target read transaction begins");
  EngineSelectRowsRequest select;
  select.context = read_target;
  select.source_object = create_result.table_object;
  auto select_result = EngineSelectRows(select);
  ok &= Require(select_result.ok && select_result.visible_count == 2, "restored rows are visible");
  ok &= Require(ContainsValue(select_result, "alpha") && ContainsValue(select_result, "beta"), "restored values round trip");
  ok &= Require(Commit(&read_target), "target read transaction commits");

  EngineRestoreLogicalBackupRequest denied_restore = restore;
  denied_restore.context = target;
  denied_restore.context.trace_tags.clear();
  ok &= Require(!EngineRestoreLogicalBackup(denied_restore).ok, "logical restore denied without right");

  std::filesystem::remove(source_path);
  std::filesystem::remove(source_path + ".sb.owner.lock");
  std::filesystem::remove(target_path);
  std::filesystem::remove(target_path + ".sb.owner.lock");
  std::filesystem::remove(gap_path);
  std::filesystem::remove(gap_path + ".sb.owner.lock");
  std::filesystem::remove(manifest_path);
  std::filesystem::remove(gap_manifest_path);
  if (!ok) { return 1; }
  std::cout << "logical backup probe passed\n";
  return 0;
}
