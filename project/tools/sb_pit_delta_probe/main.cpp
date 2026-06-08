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
  context.database_uuid.canonical = "018f3000-0000-7000-8000-000000000001";
  context.session_uuid.canonical = "018f3000-0000-7000-8000-000000000002";
  context.principal_uuid.canonical = "018f3000-0000-7000-8000-000000000003";
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

bool InsertPayload(scratchbird::engine::internal_api::EngineRequestContext context,
                   scratchbird::engine::internal_api::EngineObjectReference table,
                   const std::string& payload) {
  scratchbird::engine::internal_api::EngineInsertRowsRequest insert;
  insert.context = context;
  insert.target_table = table;
  scratchbird::engine::internal_api::EngineRowValue row;
  row.fields.push_back({"payload", Text(payload)});
  insert.input_rows = {row};
  auto result = scratchbird::engine::internal_api::EngineInsertRows(insert);
  return result.ok && result.inserted_count == 1;
}

}  // namespace

int main() {
  using namespace scratchbird::engine::internal_api;
  const auto stamp = std::to_string(NowMillis());
  const std::string source_path = "/tmp/sb_pit_delta_source_" + stamp + ".db";
  const std::string target_path = "/tmp/sb_pit_delta_target_" + stamp + ".db";
  const std::string base_manifest = "/tmp/sb_pit_delta_base_" + stamp + ".sblbk";
  const std::string delta_manifest = "/tmp/sb_pit_delta_" + stamp + ".sbdlt";
  std::filesystem::remove(base_manifest);
  std::filesystem::remove(delta_manifest);

  bool ok = true;
  ok &= Require(CreateDatabase(source_path, NowMillis()), "source database created");
  ok &= Require(CreateDatabase(target_path, NowMillis() + 10), "target database created");
  auto source = Context(source_path);
  ok &= Require(Begin(&source), "source transaction 1 begins");
  EngineCreateTableRequest create;
  create.context = source;
  create.table_names.push_back({"en", "primary", "", "delta_items", true});
  EngineColumnDefinition value_col;
  value_col.names.push_back({"en", "primary", "", "payload", true});
  value_col.descriptor.descriptor_kind = "datatype";
  value_col.descriptor.canonical_type_name = "text";
  create.table_columns = {value_col};
  auto created = EngineCreateTable(create);
  ok &= Require(created.ok, "source table created");
  ok &= Require(InsertPayload(source, created.table_object, "base-alpha"), "base row inserted");
  ok &= Require(Commit(&source), "source transaction 1 commits");

  EngineStartLogicalBackupRequest base;
  base.context = Context(source_path);
  base.context.trace_tags = {"right:BACKUP_CREATE"};
  base.option_envelopes.push_back("target_uri:" + base_manifest);
  auto base_result = EngineStartLogicalBackup(base);
  ok &= Require(base_result.ok && base_result.row_count == 1, "base logical backup created");

  source = Context(source_path);
  ok &= Require(Begin(&source), "source transaction 2 begins");
  ok &= Require(InsertPayload(source, created.table_object, "delta-beta"), "delta row inserted");
  ok &= Require(Commit(&source), "source transaction 2 commits");

  EnginePackageDeltaStreamRequest delta;
  delta.context = Context(source_path);
  delta.context.trace_tags = {"right:BACKUP_CREATE"};
  delta.option_envelopes = {"target_uri:" + delta_manifest,
                            "start_transaction_id:" + std::to_string(base_result.snapshot_visible_through_local_transaction_id + 1)};
  auto delta_result = EnginePackageDeltaStream(delta);
  ok &= Require(delta_result.ok && delta_result.row_count == 1, "delta package captures write-after row");
  ok &= Require(Evidence(delta_result, "authoritative_wal", "false"), "delta package records non-WAL authority");
  ok &= Require(Evidence(delta_result, "delta_source", "mga_row_version_lineage"),
                "delta package records MGA lineage source evidence");
  ok &= Require(Evidence(delta_result, "finality_interval",
                         std::to_string(delta_result.start_transaction_id) + ".." + std::to_string(delta_result.end_transaction_id)),
                "delta package records finality interval evidence");
  ok &= Require(Evidence(delta_result, "archive_retention_max_age_microseconds", "604800000000"),
                "delta package records default archive retention max age evidence");
  ok &= Require(Evidence(delta_result, "archive_slice_bytes", std::to_string(std::filesystem::file_size(delta_manifest))),
                "delta package records archive slice byte evidence");
  const auto delta_body = ReadFile(delta_manifest);
  ok &= Require(delta_body.find(HexEncode("delta_source")) != std::string::npos &&
                delta_body.find(HexEncode("mga_row_version_lineage")) != std::string::npos,
                "delta manifest records lineage source");
  ok &= Require(delta_body.find(HexEncode("start_transaction_id")) != std::string::npos &&
                delta_body.find(HexEncode("end_transaction_id")) != std::string::npos &&
                delta_body.find(HexEncode("finality_source")) != std::string::npos,
                "delta manifest records finality interval");
  ok &= Require(delta_body.find(HexEncode("lineage_checksum")) != std::string::npos &&
                delta_body.find(HexEncode("previous_version_uuid")) != std::string::npos,
                "delta manifest records row-version lineage fields");
  EnginePackageDeltaStreamRequest forbidden_wal = delta;
  forbidden_wal.option_envelopes.push_back("authoritative_wal:true");
  ok &= Require(!EnginePackageDeltaStream(forbidden_wal).ok, "authoritative WAL delta request rejected");

  auto target = Context(target_path);
  ok &= Require(Begin(&target), "target transaction begins");
  EngineRestoreLogicalBackupRequest restore_base;
  restore_base.context = target;
  restore_base.context.trace_tags = {"right:BACKUP_RESTORE"};
  restore_base.option_envelopes.push_back("source_manifest_uri:" + base_manifest);
  ok &= Require(EngineRestoreLogicalBackup(restore_base).ok, "base restore succeeds");
  EngineApplyDeltaStreamRequest apply_delta;
  apply_delta.context = restore_base.context;
  apply_delta.option_envelopes.push_back("source_manifest_uri:" + delta_manifest);
  auto apply_result = EngineApplyDeltaStream(apply_delta);
  ok &= Require(apply_result.ok && apply_result.applied_row_count == 1, "delta apply succeeds");
  auto apply_again = EngineApplyDeltaStream(apply_delta);
  ok &= Require(apply_again.ok && apply_again.applied_row_count == 0, "delta apply is idempotent");
  ok &= Require(Commit(&target), "target transaction commits");

  auto read = Context(target_path);
  ok &= Require(Begin(&read), "target read transaction begins");
  EngineSelectRowsRequest select;
  select.context = read;
  select.source_object = created.table_object;
  auto selected = EngineSelectRows(select);
  ok &= Require(selected.ok && selected.visible_count == 2, "base plus delta rows visible");
  ok &= Require(ContainsValue(selected, "base-alpha") && ContainsValue(selected, "delta-beta"), "delta values round trip");
  ok &= Require(Commit(&read), "target read transaction commits");

  std::filesystem::remove(source_path);
  std::filesystem::remove(source_path + ".sb.owner.lock");
  std::filesystem::remove(target_path);
  std::filesystem::remove(target_path + ".sb.owner.lock");
  std::filesystem::remove(base_manifest);
  std::filesystem::remove(delta_manifest);
  if (!ok) { return 1; }
  std::cout << "pit delta probe passed\n";
  return 0;
}
