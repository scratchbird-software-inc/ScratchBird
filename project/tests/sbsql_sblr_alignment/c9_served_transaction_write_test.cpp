// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_ROUTE_OWNER_SERVED_DEVICE (register C9 step 6)
// The transaction write path of a route owner's own threads: begin and commit
// persist the transaction inventory, and the commit page barrier syncs and
// hashes the database file, through the owner's served device instead of
// re-opening the database path. Real database, real server-style ownership.
//
// Checks: (1) served begin, barrier and commit make no open of the database
// file; (2) the barrier's database-file size and hash equal a path read of the
// same bytes; (3) the served inventory equals a path load afterwards; (4) a
// read-only owner refuses the inventory write, and a released owner refuses
// begin, neither falling back to a path open.
#define SCRATCHBIRD_IA01_MISSING_EXECUTOR_FIXTURE_ONLY
#include "ia01_package_missing_executor_integration_test.cpp"

#include "../../src/server/database_ownership.hpp"
#include "engine/internal_api/transaction/local_commit_publication.hpp"
#include "storage/disk/disk_device.hpp"
#include "../support/path_open_counter.hpp"

#include <fstream>
#include <iostream>
#include <memory>
#include <string>

namespace {

namespace disk = scratchbird::storage::disk;
namespace opens = scratchbird::tests::path_open_counter;

server::DatabaseOwnershipResult Own(const Fixture& fixture, disk::RouteStorageAccess access) {
  server::DatabaseOwnershipRequest request;
  request.database_path = fixture.database_path;
  request.owner_kind = "server";
  request.sbps_endpoint = fixture.directory / "sbps.sock";
  request.storage_access = access;
  return server::AcquireDatabaseOwnership(request);
}

api::EngineRequestContext RequestContext(const Fixture& fixture,
                                         std::shared_ptr<disk::RouteOwnershipLease> owner) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "c9-served-transaction-write";
  context.database_path = fixture.database_path.string();
  context.served_route_owner = std::move(owner);
  context.database_uuid = Identity(fixture.database_uuid);
  context.principal_uuid = Identity(fixture.principal_uuid);
  context.session_uuid = Identity(fixture.session_uuid);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = fixture.resource_epoch;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineBeginTransactionResult Begin(api::EngineRequestContext* context) {
  api::EngineBeginTransactionRequest begin;
  begin.context = *context;
  begin.isolation_level = "read_committed";
  auto begun = api::EngineBeginTransaction(begin);
  if (begun.ok) {
    context->local_transaction_id = begun.local_transaction_id;
    context->transaction_uuid = begun.transaction_uuid;
    context->snapshot_visible_through_local_transaction_id =
        begun.snapshot_visible_through_local_transaction_id;
  }
  return begun;
}

bool RefusedBecause(const api::EngineBeginTransactionResult& result, std::string_view reason) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& d) {
    return d.code == "SB-STORAGE-DISK-SERVED-REFUSED" &&
           d.detail.find(reason) != std::string::npos;
  });
}

std::string PathSha256(const std::filesystem::path& path, std::uint64_t size) {
  std::ifstream input(path, std::ios::binary);
  const auto digest = scratchbird::core::hash::ComputeSha256Stream(input, size);
  return digest.ok() ? scratchbird::core::hash::HexLower(digest.digest) : std::string{};
}

}  // namespace

int main() {
  bool ok = true;
  const auto check = [&](bool condition, const char* message) {
    if (!condition) std::cerr << "c9_served_transaction_write: " << message << '\n';
    ok = condition && ok;
  };

  {
    auto fixture = CreateFixture();
    auto owned = Own(fixture, disk::RouteStorageAccess::read_write);
    Require(owned.acquired && owned.lock, "read-write ownership not acquired");
    const auto lease = owned.lock->ServedStorageLease();
    Require(lease != nullptr, "owner served no storage lease");

    opens::Watch(fixture.database_path.string());
    const auto before = opens::FileOpens();
    auto context = RequestContext(fixture, lease);
    const auto begun = Begin(&context);
    check(begun.ok, "served begin failed");
    const auto after_begin = opens::FileOpens();
    // The barrier is the one commit runs; called here on its own so its
    // database-file artifact can be compared with a path read. The
    // transaction is then rolled back, which abandons that publication.
    const auto barrier = api::RunLocalCommitPageBarrier(context);
    const auto barrier_opens = opens::FileOpens() - after_begin;
    if (!barrier.ok) std::cerr << "barrier: " << barrier.diagnostic.detail << '\n';
    check(barrier.ok, "served commit barrier refused");
    // Path read of the same bytes, outside the watch.
    opens::Watch({});
    std::error_code size_error;
    const auto path_size = std::filesystem::file_size(fixture.database_path, size_error);
    const auto path_sha256 = size_error ? std::string{} : PathSha256(fixture.database_path, path_size);
    opens::Watch(fixture.database_path.string());
    const auto after_barrier = opens::FileOpens();
    api::EngineRollbackTransactionRequest rollback;
    rollback.context = context;
    check(api::EngineRollbackTransaction(rollback).ok, "served rollback failed");
    const auto after_rollback = opens::FileOpens();

    auto commit_context = RequestContext(fixture, lease);
    check(Begin(&commit_context).ok, "second served begin failed");
    api::EngineCommitTransactionRequest commit;
    commit.context = commit_context;
    const auto committed = api::EngineCommitTransaction(commit);
    check(committed.ok, "served commit failed");
    const auto after_commit = opens::FileOpens();
    opens::Watch({});

    std::cout << "c9_served_transaction_write database_file_opens begin="
              << after_begin - before << " barrier=" << barrier_opens
              << " rollback=" << after_rollback - after_barrier
              << " begin_commit=" << after_commit - after_rollback << '\n';
    check(after_begin == before, "served begin re-opened the database file");
    check(barrier_opens == 0, "served commit barrier re-opened the database file");
    check(after_rollback == after_barrier, "served rollback re-opened the database file");
    check(after_commit == after_rollback, "served begin and commit re-opened the database file");

    const auto database_artifact = std::find_if(
        barrier.artifacts.begin(), barrier.artifacts.end(),
        [](const auto& artifact) { return artifact.artifact_identity == "database_file"; });
    check(database_artifact != barrier.artifacts.end(),
          "commit barrier recorded no database-file artifact");
    if (database_artifact != barrier.artifacts.end()) {
      check(!path_sha256.empty() && database_artifact->durable_size_bytes == path_size &&
                database_artifact->postcondition_sha256 == path_sha256,
            "served database-file size or hash differs from a path read of the same bytes");
    }

    const auto served_load =
        db::LoadLocalTransactionInventoryFromDatabase(fixture.database_path.string(), lease);
    const auto path_load =
        db::LoadLocalTransactionInventoryFromDatabase(fixture.database_path.string());
    check(served_load.ok() && path_load.ok() &&
              served_load.inventory.entries.size() == path_load.inventory.entries.size() &&
              served_load.inventory.next_local_transaction_id ==
                  path_load.inventory.next_local_transaction_id,
          "served inventory differs from the path load after served writes");
    owned.lock->release();
  }

  // Read-only ownership serves no writable device: begin, which must persist
  // the inventory, is refused and the database file is not opened.
  {
    auto fixture = CreateFixture();
    auto owned = Own(fixture, disk::RouteStorageAccess::read_only);
    Require(owned.acquired && owned.lock, "read-only ownership not acquired");
    const auto lease = owned.lock->ServedStorageLease();
    const auto inventory_before =
        db::LoadLocalTransactionInventoryFromDatabase(fixture.database_path.string(), lease);
    opens::Watch(fixture.database_path.string());
    const auto before = opens::FileOpens();
    auto context = RequestContext(fixture, lease);
    const auto refused = Begin(&context);
    check(!refused.ok, "a read-only owner served a writable inventory persist");
    check(RefusedBecause(refused, "writable_not_requested"),
          "read-only begin was not refused at the writable served persist");
    check(opens::FileOpens() == before,
          "a refused read-only persist fell back to opening the database file");
    opens::Watch({});
    const auto inventory_after =
        db::LoadLocalTransactionInventoryFromDatabase(fixture.database_path.string(), lease);
    check(inventory_before.ok() && inventory_after.ok() &&
              inventory_before.inventory.next_local_transaction_id ==
                  inventory_after.inventory.next_local_transaction_id,
          "a refused read-only persist changed the inventory");
    owned.lock->release();
  }

  // A released owner serves nothing: begin fails closed, no path open.
  {
    auto fixture = CreateFixture();
    auto owned = Own(fixture, disk::RouteStorageAccess::read_write);
    Require(owned.acquired && owned.lock, "ownership for released case not acquired");
    const auto lease = owned.lock->ServedStorageLease();
    owned.lock->release();
    opens::Watch(fixture.database_path.string());
    const auto before = opens::FileOpens();
    auto context = RequestContext(fixture, lease);
    const auto refused = Begin(&context);
    check(!refused.ok, "a released owner still served a transaction begin");
    check(RefusedBecause(refused, "withdrawn"),
          "released-owner begin was not refused by the withdrawn served device");
    check(opens::FileOpens() == before,
          "a refused served begin fell back to opening the database file");
    opens::Watch({});
  }
  std::cout << (ok ? "c9_served_transaction_write PASS" : "c9_served_transaction_write FAIL") << '\n';
  return ok ? 0 : 1;
}
