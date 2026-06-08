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
#include <filesystem>
#include <iostream>
#include <string>

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) { std::cerr << "FAIL: " << message << "\n"; return false; }
  return true;
}

std::uint64_t NowMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

scratchbird::core::platform::TypedUuid Generate(scratchbird::core::platform::UuidKind kind, std::uint64_t millis) {
  auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, millis);
  return generated.ok() ? generated.value : scratchbird::core::platform::TypedUuid{};
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

scratchbird::engine::internal_api::EngineRequestContext Context(const std::string& path) {
  scratchbird::engine::internal_api::EngineRequestContext context;
  context.database_path = path;
  context.database_uuid.canonical = "018f2000-0000-7000-8000-000000000001";
  context.session_uuid.canonical = "018f2000-0000-7000-8000-000000000002";
  context.principal_uuid.canonical = "018f2000-0000-7000-8000-000000000003";
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
  return true;
}

scratchbird::engine::internal_api::EngineTypedValue Text(const std::string& value) {
  scratchbird::engine::internal_api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "datatype";
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = value;
  return typed;
}

bool Evidence(const scratchbird::engine::internal_api::EngineApiResult& result,
              const std::string& kind,
              const std::string& id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) { return true; }
  }
  return false;
}

bool ContainsValue(const scratchbird::engine::internal_api::EngineApiResult& result, const std::string& value) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.second.encoded_value.find(value) != std::string::npos) { return true; }
    }
  }
  return false;
}

}  // namespace

int main() {
  using namespace scratchbird::engine::internal_api;
  const auto stamp = std::to_string(NowMillis());
  const std::string source_path = "/tmp/sb_physical_backup_source_" + stamp + ".db";
  const std::string restored_path = "/tmp/sb_physical_backup_restored_" + stamp + ".db";
  const std::string manifest_path = "/tmp/sb_physical_backup_" + stamp + ".sbpbk";
  const std::string image_path = manifest_path + ".image";
  std::filesystem::remove(restored_path);
  std::filesystem::remove(manifest_path);
  std::filesystem::remove(image_path);

  bool ok = true;
  ok &= Require(CreateDatabase(source_path, NowMillis()), "source database created");
  auto source = Context(source_path);
  ok &= Require(Begin(&source), "source transaction begins");
  EngineCreateTableRequest create;
  create.context = source;
  create.table_names.push_back({"en", "primary", "", "physical_items", true});
  EngineColumnDefinition value_col;
  value_col.names.push_back({"en", "primary", "", "payload", true});
  value_col.descriptor.descriptor_kind = "datatype";
  value_col.descriptor.canonical_type_name = "text";
  create.table_columns = {value_col};
  auto created = EngineCreateTable(create);
  ok &= Require(created.ok, "source table created");
  EngineInsertRowsRequest insert;
  insert.context = source;
  insert.target_table = created.table_object;
  EngineRowValue row;
  row.fields.push_back({"payload", Text("physical-alpha")});
  insert.input_rows = {row};
  auto inserted = EngineInsertRows(insert);
  ok &= Require(inserted.ok && inserted.inserted_count == 1, "source row inserted");
  ok &= Require(Commit(&source), "source transaction commits");

  EngineStartPhysicalBackupRequest backup;
  backup.context = Context(source_path);
  backup.context.trace_tags = {"right:BACKUP_CREATE"};
  backup.option_envelopes = {"target_uri:" + manifest_path, "image_uri:" + image_path};
  auto backup_result = EngineStartPhysicalBackup(backup);
  ok &= Require(backup_result.ok && backup_result.image_bytes > 0, "physical backup succeeds");
  ok &= Require(Evidence(backup_result, "authoritative_wal", "false"), "physical backup records non-WAL authority");
  EngineStartPhysicalBackupRequest denied_backup = backup;
  denied_backup.context.trace_tags.clear();
  ok &= Require(!EngineStartPhysicalBackup(denied_backup).ok, "physical backup denied without right");

  EngineRestorePhysicalBackupRequest denied_restore;
  denied_restore.context = Context(restored_path);
  denied_restore.option_envelopes = {"source_manifest_uri:" + manifest_path};
  ok &= Require(!EngineRestorePhysicalBackup(denied_restore).ok, "physical restore denied without right");
  EngineRestorePhysicalBackupRequest restore = denied_restore;
  restore.context.trace_tags = {"right:BACKUP_RESTORE"};
  auto restore_result = EngineRestorePhysicalBackup(restore);
  ok &= Require(restore_result.ok && restore_result.image_bytes == backup_result.image_bytes, "physical restore succeeds");

  auto read = Context(restored_path);
  ok &= Require(Begin(&read), "restored database transaction begins");
  EngineSelectRowsRequest select;
  select.context = read;
  select.source_object = created.table_object;
  auto selected = EngineSelectRows(select);
  ok &= Require(selected.ok && selected.visible_count == 1, "restored physical row visible");
  ok &= Require(ContainsValue(selected, "physical-alpha"), "restored physical value round trips");
  ok &= Require(Commit(&read), "restored database transaction commits");

  std::filesystem::remove(source_path);
  std::filesystem::remove(source_path + ".sb.owner.lock");
  std::filesystem::remove(restored_path);
  std::filesystem::remove(restored_path + ".sb.owner.lock");
  std::filesystem::remove(manifest_path);
  std::filesystem::remove(image_path);
  if (!ok) { return 1; }
  std::cout << "physical backup probe passed\n";
  return 0;
}
