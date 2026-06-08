// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "memory.hpp"
#include "public_release_authz_fixture.hpp"
#include "transaction/transaction_inspect_api.hpp"
#include "transaction_inventory.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1770120000000ull;
constexpr u32 kPageSize = 16384;

bool Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool ExpectApiOk(const api::EngineApiResult& result, const char* message) {
  if (!result.ok) {
    std::cerr << message;
    if (!result.diagnostics.empty()) {
      std::cerr << " diagnostic=" << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail;
    }
    std::cerr << '\n';
    return false;
  }
  return true;
}

bool ExpectApiFailCode(const api::EngineApiResult& result,
                       const std::string& code,
                       const char* message) {
  if (result.ok || result.diagnostics.empty() ||
      result.diagnostics.front().code != code) {
    std::cerr << message;
    if (!result.diagnostics.empty()) {
      std::cerr << " diagnostic=" << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail;
    }
    std::cerr << '\n';
    return false;
  }
  return true;
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "public_mga_audit_transaction_location_gate";
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
          MemoryPolicy(), "public_mga_audit_transaction_location_gate");
  return Expect(configured.ok(),
                "PCR-078 memory fixture should configure") &&
         Expect(configured.fixture_mode,
                "PCR-078 should use fixture memory");
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

std::string MakeUuidText(UuidKind kind, u64 offset) {
  const auto generated = MakeUuid(kind, offset);
  return generated.valid() ? uuid::UuidToString(generated.value) : "";
}

struct Fixture {
  std::filesystem::path database_path;
  std::string database_uuid;
};

Fixture MakeFixture(const std::filesystem::path& work_dir) {
  std::filesystem::create_directories(work_dir);
  Fixture fixture;
  fixture.database_path = work_dir / "pcr078_audit_location.sbdb";
  std::filesystem::remove(fixture.database_path);

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = MakeUuid(UuidKind::database, 1000);
  create.filespace_uuid = MakeUuid(UuidKind::filespace, 1001);
  create.page_size = kPageSize;
  create.creation_unix_epoch_millis = kBaseMillis + 1000;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Expect(created.ok(), "PCR-078 fixture database should create");
  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  return fixture;
}

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical = MakeUuidText(UuidKind::principal, 1200);
  context.session_uuid.canonical = MakeUuidText(UuidKind::session, 1201);
  context.security_context_present = true;
  context.trace_tags.push_back("right:MGA_TRANSACTION_INSPECT");
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  scratchbird::tests::release::GrantMaterializedRight(
      &context, "MGA_TRANSACTION_INSPECT");
  return context;
}

struct SeededTransactions {
  u64 hot_local_id = 0;
  std::string hot_uuid;
  u64 archive_local_id = 0;
  std::string archive_uuid;
};

bool PersistInventory(const std::filesystem::path& database_path,
                      const txn::LocalTransactionInventory& inventory,
                      const char* message) {
  const auto persisted =
      db::PersistLocalTransactionInventoryToDatabase(database_path.string(),
                                                     inventory);
  if (!persisted.ok()) {
    std::cerr << persisted.diagnostic.diagnostic_code << ':'
              << persisted.diagnostic.message_key << '\n';
  }
  return Expect(persisted.ok(), message);
}

SeededTransactions SeedInventory(const Fixture& fixture, bool* ok) {
  SeededTransactions seeded;
  const auto loaded =
      db::LoadLocalTransactionInventoryFromDatabase(fixture.database_path.string());
  *ok = Expect(loaded.ok(), "PCR-078 fixture inventory should load") && *ok;
  txn::LocalTransactionInventory inventory = loaded.inventory;

  const auto hot_begin =
      txn::BeginLocalTransaction(inventory,
                                 MakeUuid(UuidKind::transaction, 2000),
                                 kBaseMillis + 2000);
  *ok = Expect(hot_begin.ok(), "PCR-078 local-hot transaction should begin") && *ok;
  const auto hot_commit =
      txn::CommitLocalTransaction(hot_begin.inventory,
                                  hot_begin.entry.identity.local_id,
                                  kBaseMillis + 2010);
  *ok = Expect(hot_commit.ok(), "PCR-078 local-hot transaction should commit") && *ok;
  inventory = hot_commit.inventory;
  seeded.hot_local_id = hot_commit.entry.identity.local_id.value;
  seeded.hot_uuid = uuid::UuidToString(
      hot_commit.entry.identity.transaction_uuid.value);

  const auto archive_begin =
      txn::BeginLocalTransaction(inventory,
                                 MakeUuid(UuidKind::transaction, 2100),
                                 kBaseMillis + 2100);
  *ok = Expect(archive_begin.ok(),
               "PCR-078 archive transaction should begin") && *ok;
  const auto archive_commit =
      txn::CommitLocalTransaction(archive_begin.inventory,
                                  archive_begin.entry.identity.local_id,
                                  kBaseMillis + 2110);
  *ok = Expect(archive_commit.ok(),
               "PCR-078 archive transaction should commit") && *ok;
  const auto archived =
      txn::ArchiveLocalTransaction(archive_commit.inventory,
                                   archive_commit.entry.identity.local_id);
  *ok = Expect(archived.ok(),
               "PCR-078 archive transition should require finality evidence") && *ok;
  inventory = archived.inventory;
  seeded.archive_local_id = archived.entry.identity.local_id.value;
  seeded.archive_uuid = uuid::UuidToString(
      archived.entry.identity.transaction_uuid.value);

  *ok = PersistInventory(fixture.database_path,
                         inventory,
                         "PCR-078 seeded inventory should persist") && *ok;
  return seeded;
}

const txn::TransactionInventoryEntry* FindEntry(
    const txn::LocalTransactionInventory& inventory,
    u64 local_transaction_id) {
  for (const auto& entry : inventory.entries) {
    if (entry.identity.local_id.value == local_transaction_id) {
      return &entry;
    }
  }
  return nullptr;
}

bool AuditReadPersisted(const Fixture& fixture,
                        const api::EngineBeginAuditReadTransactionResult& result,
                        u64 expected_snapshot_boundary) {
  const auto loaded =
      db::LoadLocalTransactionInventoryFromDatabase(fixture.database_path.string());
  if (!Expect(loaded.ok(), "PCR-078 audit inventory should reload")) {
    return false;
  }
  const auto* entry = FindEntry(loaded.inventory, result.audit_local_transaction_id);
  return Expect(entry != nullptr,
                "PCR-078 audit read transaction should be durable") &&
         Expect(entry->state == txn::TransactionState::read_only_active,
                "PCR-078 audit transaction should be read-only active") &&
         Expect(entry->begin_visible_through_local_transaction_id ==
                    expected_snapshot_boundary,
                "PCR-078 audit transaction should carry as-of boundary");
}

api::EngineLocateTransactionResult Locate(const Fixture& fixture,
                                          const SeededTransactions& seeded,
                                          u64 local_transaction_id,
                                          std::string request_id) {
  api::EngineLocateTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.target_local_transaction_id = local_transaction_id;
  if (local_transaction_id == seeded.hot_local_id) {
    request.target_transaction_uuid.canonical = seeded.hot_uuid;
  } else if (local_transaction_id == seeded.archive_local_id) {
    request.target_transaction_uuid.canonical = seeded.archive_uuid;
  }
  return api::EngineLocateTransaction(request);
}

api::EngineBeginAuditReadTransactionResult BeginAudit(
    const Fixture& fixture,
    u64 local_transaction_id,
    std::string transaction_uuid,
    std::string request_id) {
  api::EngineBeginAuditReadTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.target_local_transaction_id = local_transaction_id;
  request.target_transaction_uuid.canonical = std::move(transaction_uuid);
  return api::EngineBeginAuditReadTransaction(request);
}

bool LocationClassificationProof(const Fixture& fixture,
                                 const SeededTransactions& seeded) {
  bool ok = true;
  const auto hot = Locate(fixture, seeded, seeded.hot_local_id, "pcr078-hot-locate");
  ok = ExpectApiOk(hot, "PCR-078 local-hot locate should succeed") && ok;
  ok = Expect(hot.location_class == "local_hot",
              "PCR-078 hot transaction should classify local_hot") && ok;
  ok = Expect(hot.queryable && !hot.fail_closed,
              "PCR-078 local-hot transaction should be queryable") && ok;
  ok = Expect(hot.local_inventory_authoritative && !hot.archive_authoritative,
              "PCR-078 local-hot authority should be inventory hot") && ok;

  const auto archived =
      Locate(fixture, seeded, seeded.archive_local_id, "pcr078-archive-locate");
  ok = ExpectApiOk(archived, "PCR-078 local archive locate should succeed") && ok;
  ok = Expect(archived.location_class == "local_archive",
              "PCR-078 archive transaction should classify local_archive") && ok;
  ok = Expect(archived.queryable && archived.archive_authoritative,
              "PCR-078 archive transaction should be queryable as archive") && ok;

  api::EngineLocateTransactionRequest retired_request;
  retired_request.context = BaseContext(fixture, "pcr078-retired-locate");
  retired_request.target_local_transaction_id = seeded.archive_local_id + 100;
  retired_request.retired_history_evidence_present = true;
  const auto retired = api::EngineLocateTransaction(retired_request);
  ok = ExpectApiOk(retired, "PCR-078 retired locate should return classification") && ok;
  ok = Expect(retired.location_class == "retired" &&
                  retired.fail_closed && !retired.queryable,
              "PCR-078 retired location should fail closed and not query") && ok;
  ok = Expect(retired.location_diagnostic_code ==
                  "ENGINE.MGA_AUDIT_RETIRED_HISTORY_NOT_QUERYABLE",
              "PCR-078 retired diagnostic should be exact") && ok;

  api::EngineLocateTransactionRequest unknown_request;
  unknown_request.context = BaseContext(fixture, "pcr078-unknown-locate");
  unknown_request.target_local_transaction_id = seeded.archive_local_id + 200;
  const auto unknown = api::EngineLocateTransaction(unknown_request);
  ok = ExpectApiOk(unknown, "PCR-078 unknown locate should return classification") && ok;
  ok = Expect(unknown.location_class == "unknown" &&
                  unknown.fail_closed && !unknown.queryable,
              "PCR-078 unknown location should fail closed and not query") && ok;
  ok = Expect(unknown.location_diagnostic_code ==
                  "ENGINE.MGA_AUDIT_LOCATION_UNKNOWN",
              "PCR-078 unknown diagnostic should be exact") && ok;

  api::EngineLocateTransactionRequest remote_request;
  remote_request.context = BaseContext(fixture, "pcr078-remote-locate");
  remote_request.requested_location_class = "remote";
  remote_request.target_local_transaction_id = seeded.hot_local_id;
  const auto remote = api::EngineLocateTransaction(remote_request);
  ok = ExpectApiOk(remote, "PCR-078 remote locate should return classification") && ok;
  ok = Expect(remote.location_class == "remote" &&
                  remote.fail_closed && !remote.queryable &&
                  remote.external_cluster_provider_required,
              "PCR-078 remote location should require external provider") && ok;
  ok = Expect(remote.location_diagnostic_code ==
                  "ENGINE.MGA_AUDIT_REMOTE_CLUSTER_PROVIDER_UNAVAILABLE",
              "PCR-078 no-cluster remote diagnostic should be exact") && ok;
  return ok;
}

bool AuditReadAdmissionProof(const Fixture& fixture,
                             const SeededTransactions& seeded) {
  bool ok = true;
  const auto hot = BeginAudit(fixture,
                              seeded.hot_local_id,
                              seeded.hot_uuid,
                              "pcr078-hot-audit");
  ok = ExpectApiOk(hot, "PCR-078 hot audit read should begin") && ok;
  ok = Expect(hot.read_only && hot.writes_refused,
              "PCR-078 hot audit read should refuse writes") && ok;
  ok = Expect(hot.audit_transaction_distinct,
              "PCR-078 hot audit transaction should be distinct") && ok;
  ok = Expect(hot.location_class == "local_hot" &&
                  hot.snapshot_visible_through_local_transaction_id ==
                      seeded.hot_local_id,
              "PCR-078 hot audit should bind as-of boundary") && ok;
  ok = AuditReadPersisted(fixture, hot, seeded.hot_local_id) && ok;

  const auto archived = BeginAudit(fixture,
                                   seeded.archive_local_id,
                                   seeded.archive_uuid,
                                   "pcr078-archive-audit");
  ok = ExpectApiOk(archived, "PCR-078 archive audit read should begin") && ok;
  ok = Expect(archived.read_only && archived.writes_refused,
              "PCR-078 archive audit read should refuse writes") && ok;
  ok = Expect(archived.audit_transaction_distinct,
              "PCR-078 archive audit transaction should be distinct") && ok;
  ok = Expect(archived.location_class == "local_archive" &&
                  archived.snapshot_visible_through_local_transaction_id ==
                      seeded.archive_local_id,
              "PCR-078 archive audit should bind as-of boundary") && ok;
  ok = AuditReadPersisted(fixture, archived, seeded.archive_local_id) && ok;
  return ok;
}

bool FailClosedAdmissionProof(const Fixture& fixture,
                              const SeededTransactions& seeded) {
  bool ok = true;
  const auto before =
      db::LoadLocalTransactionInventoryFromDatabase(fixture.database_path.string());
  ok = Expect(before.ok(), "PCR-078 fail-closed inventory should load before") && ok;
  const std::size_t before_count = before.inventory.entries.size();

  api::EngineBeginAuditReadTransactionRequest remote_request;
  remote_request.context = BaseContext(fixture, "pcr078-remote-audit");
  remote_request.requested_location_class = "remote";
  remote_request.target_local_transaction_id = seeded.hot_local_id;
  const auto remote = api::EngineBeginAuditReadTransaction(remote_request);
  ok = ExpectApiFailCode(remote,
                         "ENGINE.MGA_AUDIT_REMOTE_CLUSTER_PROVIDER_UNAVAILABLE",
                         "PCR-078 remote audit should fail closed") && ok;
  ok = Expect(remote.location_class == "remote" &&
                  remote.external_cluster_provider_required &&
                  remote.fail_closed && !remote.queryable,
              "PCR-078 remote audit should expose provider boundary") && ok;

  api::EngineBeginAuditReadTransactionRequest unknown_request;
  unknown_request.context = BaseContext(fixture, "pcr078-unknown-audit");
  unknown_request.target_local_transaction_id = seeded.archive_local_id + 300;
  const auto unknown = api::EngineBeginAuditReadTransaction(unknown_request);
  ok = ExpectApiFailCode(unknown,
                         "ENGINE.MGA_AUDIT_LOCATION_UNKNOWN",
                         "PCR-078 unknown audit should fail closed") && ok;
  ok = Expect(unknown.location_class == "unknown" &&
                  unknown.fail_closed && !unknown.queryable,
              "PCR-078 unknown audit should not query") && ok;

  const auto after =
      db::LoadLocalTransactionInventoryFromDatabase(fixture.database_path.string());
  ok = Expect(after.ok(), "PCR-078 fail-closed inventory should reload") && ok;
  ok = Expect(after.inventory.entries.size() == before_count,
              "PCR-078 fail-closed admissions should not mutate inventory") && ok;
  return ok;
}

bool IdentityMismatchProof(const Fixture& fixture,
                           const SeededTransactions& seeded) {
  api::EngineLocateTransactionRequest request;
  request.context = BaseContext(fixture, "pcr078-mismatch-locate");
  request.target_local_transaction_id = seeded.hot_local_id;
  request.target_transaction_uuid.canonical = seeded.archive_uuid;
  const auto mismatch = api::EngineLocateTransaction(request);
  return ExpectApiOk(mismatch,
                     "PCR-078 identity mismatch locate should return classification") &&
         Expect(mismatch.location_class == "unknown" &&
                    mismatch.fail_closed && !mismatch.queryable,
                "PCR-078 identity mismatch should fail closed") &&
         Expect(mismatch.location_diagnostic_code ==
                    "ENGINE.MGA_AUDIT_LOCATION_IDENTITY_MISMATCH",
                "PCR-078 identity mismatch diagnostic should be exact");
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path work_dir =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::path("public_mga_audit_transaction_location_gate_tmp");
  std::filesystem::remove_all(work_dir);

  bool ok = ConfigureMemoryFixture();
  const Fixture fixture = MakeFixture(work_dir);
  const SeededTransactions seeded = SeedInventory(fixture, &ok);
  ok = LocationClassificationProof(fixture, seeded) && ok;
  ok = AuditReadAdmissionProof(fixture, seeded) && ok;
  ok = FailClosedAdmissionProof(fixture, seeded) && ok;
  ok = IdentityMismatchProof(fixture, seeded) && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
