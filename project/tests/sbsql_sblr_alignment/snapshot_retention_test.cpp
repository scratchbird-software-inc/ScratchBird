// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "transaction_snapshot.hpp"
#include "transaction_inventory_validation.hpp"
#include "transaction_cleanup_horizon_service.hpp"
#include "transaction_cleanup.hpp"
#include "uuid.hpp"
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <thread>
#include <type_traits>
#include <vector>
#include <algorithm>
#include <array>
#include <functional>
#include <limits>

static_assert(!std::is_copy_constructible_v<scratchbird::transaction::mga::PublishedSnapshotPin>);
static_assert(std::is_nothrow_move_constructible_v<scratchbird::transaction::mga::PublishedSnapshotPin>);

namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
static unsigned checks = 0;
static void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) throw std::runtime_error(message);
}
struct Fixture {
  mga::LocalTransactionInventory inventory;
  mga::SnapshotVectorDescriptor snapshot;
  Fixture() {
    inventory = mga::MakeEmptyLocalTransactionInventory();
    for (unsigned i = 0; i < 2; ++i) {
      const auto id = uuid::GenerateEngineIdentityV7(platform::UuidKind::transaction, 1000+i);
      Check(id.ok(), "real transaction UUID issuance");
      auto begun = mga::BeginLocalTransaction(inventory, id.value, 1000+i);
      Check(begun.ok(), "real transaction begin");
      inventory = std::move(begun.inventory);
    }
    auto published = mga::PublishStatementStableSnapshotVector(inventory, mga::MakeLocalTransactionId(2), 1003);
    Check(published.ok(), "real statement snapshot publication");
    snapshot = std::move(published.descriptor);
    Check(snapshot.retention_horizon_transaction.value == 1, "snapshot retains oldest unresolved transaction1");
    auto committed = mga::CommitLocalTransaction(inventory, mga::MakeLocalTransactionId(1), 1004);
    Check(committed.ok(), "competing transaction commit");
    inventory = std::move(committed.inventory);
  }
  ~Fixture() {
    mga::RevokePublishedSnapshotVector(snapshot.snapshot_uuid);
    mga::ReleasePublishedSnapshotVector(snapshot.snapshot_uuid);
  }
  mga::AuthoritativeCleanupHorizonResult Cleanup() const {
    mga::AuthoritativeCleanupHorizonRequest request;
    request.inventory = inventory;
    request.inventory_authoritative = true;
    request.inventory_complete = true;
    return mga::ComputeAuthoritativeCleanupHorizon(request);
  }
};
static void AdditionalCases();
static void InventoryAdmissionCases();
int main() try {
  Fixture f;
  const auto plain = mga::ComputeLocalTransactionHorizons(f.inventory);
  Check(plain.ok() && plain.horizons.oldest_snapshot_transaction.value == 3,
        "pure inventory calculator remains deterministic and has no runtime pins");
  const auto retained = f.Cleanup();
  Check(retained.ok() && retained.cleanup_horizon.value == 1,
        "actual cleanup service must retain the published statement snapshot");
  mga::RevokePublishedSnapshotVector(f.snapshot.snapshot_uuid);
  const auto released = f.Cleanup();
  Check(released.ok() && released.cleanup_horizon.value == 2,
        "revocation removes snapshot blocker but not active transaction2");
  // Fixed bounded ownership grid:0..8 pins x owner-release/revoke-one/revoke-txn.
  unsigned cases = 0;
  for (unsigned count = 0; count <= 8; ++count) {
    for (unsigned terminal = 0; terminal < 3; ++terminal) {
      Fixture owned;
      std::vector<mga::PublishedSnapshotPin> pins;
      for (unsigned i = 0; i < count; ++i) {
        auto pin = mga::RetainPublishedSnapshotVector(owned.snapshot.snapshot_uuid);
        Check(pin.valid(), "only live published authority issues pin");
        pins.push_back(std::move(pin));
        Check(!pin.valid() && !pin.Resolve().ok(), "move transfers ownership");
      }
      if (terminal == 1) mga::RevokePublishedSnapshotVector(owned.snapshot.snapshot_uuid);
      if (terminal == 2) mga::RevokePublishedSnapshotVectorsForTransaction(
          owned.snapshot.owning_transaction_uuid, owned.snapshot.owning_transaction);
      mga::ReleasePublishedSnapshotVector(owned.snapshot.snapshot_uuid);
      mga::ReleasePublishedSnapshotVector(owned.snapshot.snapshot_uuid);
      Check(!mga::RetainPublishedSnapshotVector(owned.snapshot.snapshot_uuid).valid(),
            "publication retirement cannot issue late pins");
      Check(!mga::ResolvePublishedSnapshotVector(owned.snapshot.snapshot_uuid).ok(),
            "retained pin does not reopen public snapshot lookup");
      for (auto& pin : pins) {
        const auto observed = pin.Resolve();
        Check(observed.ok() == (terminal == 0), "explicit invalidation revokes retained authority");
        if (terminal == 0) Check(mga::SnapshotVectorDescriptorEqual(observed.descriptor, owned.snapshot),
                                 "pinned snapshot contents remain exact");
        Check(owned.Cleanup().cleanup_horizon.value == (terminal == 0 ? 1U : 2U),
              "runtime cleanup respects remaining pins or explicit revocation");
        pin.Release(); pin.Release();
        Check(!pin.valid(), "pin release idempotent");
      }
      Check(owned.Cleanup().cleanup_horizon.value == 2, "last owner releases only its snapshot horizon");
      Check(mga::PublishedSnapshotRetentionHorizons(owned.inventory).horizons.empty(),
            "terminal snapshot has no retained runtime horizon");
      ++cases;
    }
  }
  Check(cases == 27, "complete fixed ownership grid");
  {
    Fixture first, other;
    mga::ReleasePublishedSnapshotVector(other.snapshot.snapshot_uuid);
    Check(other.Cleanup().cleanup_horizon.value == 2, "same local transaction ID in another inventory is isolated");
    auto invalid = first.snapshot.snapshot_uuid;
    invalid.kind = platform::UuidKind::transaction;
    Check(!mga::RetainPublishedSnapshotVector(invalid).valid(), "wrong UUID kind denied");
    invalid = first.snapshot.snapshot_uuid; invalid.value.bytes[6] ^= 0x30;
    Check(!mga::RetainPublishedSnapshotVector(invalid).valid(), "changed version cannot resolve original binary identity");
    auto pin = mga::RetainPublishedSnapshotVector(first.snapshot.snapshot_uuid);
    const auto id = uuid::GenerateEngineIdentityV7(platform::UuidKind::transaction, 1005);
    Check(id.ok(), "concurrent inventory advance UUID");
    auto advanced = mga::BeginLocalTransaction(first.inventory, id.value, 1005);
    Check(advanced.ok(), "concurrent inventory advance");
    const auto newer = mga::PublishStatementStableSnapshotVector(advanced.inventory, mga::MakeLocalTransactionId(2), 1006);
    Check(newer.ok(), "second statement snapshot");
    const auto stale = first.Cleanup();
    Check(!stale.ok() && stale.diagnostic.diagnostic_code == "SB-MGA-SNAPSHOT-VECTOR-STALE",
          "older inventory cannot silently ignore newer snapshot cohort");
    mga::RevokePublishedSnapshotVector(newer.descriptor.snapshot_uuid);
    mga::ReleasePublishedSnapshotVector(newer.descriptor.snapshot_uuid);
    Check(mga::SnapshotVectorDescriptorEqual(pin.Resolve().descriptor, first.snapshot),
          "inventory advance cannot refresh pinned exclusions");
    auto bad_inventory = first.inventory;
    bad_inventory.entries[1].identity.transaction_uuid.value.bytes[6] = 0x40;
    Check(!mga::PublishStatementStableSnapshotVector(bad_inventory, mga::MakeLocalTransactionId(2), 1007).ok(),
          "system snapshot owner cannot be user UUIDv4");
  }
  //64 deterministic competing-owner histories, not an exhaustive scheduler proof.
  for (unsigned iteration = 0; iteration < 64; ++iteration) {
    Fixture raced;
    std::atomic<bool> start{false};
    std::atomic<unsigned> failures{0};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 4; ++i) {
      auto pin = mga::RetainPublishedSnapshotVector(raced.snapshot.snapshot_uuid);
      Check(pin.valid(), "concurrent owner pin issuance");
      threads.emplace_back([pin = std::move(pin), &start, &failures]() mutable {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        if (!pin.Resolve().ok()) ++failures;
        pin.Release();
      });
    }
    mga::ReleasePublishedSnapshotVector(raced.snapshot.snapshot_uuid);
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    Check(failures.load() == 0, "ordinary publication release preserves all concurrent readers");
    Check(raced.Cleanup().cleanup_horizon.value == 2, "last concurrent release removes horizon");
  }
  AdditionalCases();
  InventoryAdmissionCases();
  std::cout << "PASS snapshot_retention ownership_cases=27 concurrent_histories=128 checks=" << checks << '\n';
  return 0;
} catch (const std::exception& error) {
  std::cerr << "FAIL check=" << checks << " " << error.what() << '\n';
  return 1;
}

static void InventoryAdmissionCases() {
  Fixture f;
  using Mutation = std::function<void(mga::LocalTransactionInventory&, unsigned)>;
  std::vector<std::pair<std::string, Mutation>> mutations;
  const auto add = [&](const char* name, Mutation mutation) {
    mutations.emplace_back(name, std::move(mutation));
  };
  add("next_zero", [](auto& v, unsigned) { v.next_local_transaction_id = 0; });
  add("next_at_last", [](auto& v, unsigned) { v.next_local_transaction_id = 2; });
  add("next_before_last", [](auto& v, unsigned) { v.next_local_transaction_id = 1; });
  add("local_zero", [](auto& v, unsigned i) { v.entries[i].identity.local_id.value = 0; });
  add("local_at_next", [](auto& v, unsigned i) { v.entries[i].identity.local_id.value = 3; });
  add("local_after_next", [](auto& v, unsigned i) { v.entries[i].identity.local_id.value = 4; });
  add("local_max", [](auto& v, unsigned i) { v.entries[i].identity.local_id.value = std::numeric_limits<platform::u64>::max(); });
  add("duplicate_local", [](auto& v, unsigned i) { v.entries[i].identity.local_id = v.entries[1-i].identity.local_id; });
  add("duplicate_uuid", [](auto& v, unsigned i) { v.entries[i].identity.transaction_uuid = v.entries[1-i].identity.transaction_uuid; });
  add("nil_uuid", [](auto& v, unsigned i) { v.entries[i].identity.transaction_uuid.value.bytes.fill(0); });
  add("wrong_kind", [](auto& v, unsigned i) { v.entries[i].identity.transaction_uuid.kind = platform::UuidKind::object; });
  add("unknown_scope", [](auto& v, unsigned i) { v.entries[i].identity.scope = mga::TransactionScope::unknown; });
  add("invalid_scope", [](auto& v, unsigned i) { v.entries[i].identity.scope = static_cast<mga::TransactionScope>(65535); });
  add("state_none", [](auto& v, unsigned i) { v.entries[i].state = mga::TransactionState::none; });
  add("state_after_last", [](auto& v, unsigned i) { v.entries[i].state = static_cast<mga::TransactionState>(14); });
  add("state_max", [](auto& v, unsigned i) { v.entries[i].state = static_cast<mga::TransactionState>(65535); });
  for (unsigned version = 0; version < 16; ++version) {
    if (version == 7) continue;
    mutations.emplace_back("version_" + std::to_string(version), [version](auto& v, unsigned i) {
      auto& octet = v.entries[i].identity.transaction_uuid.value.bytes[6];
      octet = static_cast<platform::byte>((octet & 15U) | (version << 4U));
    });
  }
  for (unsigned variant = 0; variant < 16; ++variant) {
    if (variant >= 8 && variant <= 11) continue;
    mutations.emplace_back("variant_" + std::to_string(variant), [variant](auto& v, unsigned i) {
      auto& octet = v.entries[i].identity.transaction_uuid.value.bytes[8];
      octet = static_cast<platform::byte>((octet & 15U) | (variant << 4U));
    });
  }
  Check(mutations.size() == 43, "immutable malformed inventory profile count");
  std::array<unsigned, 5> missed{};
  unsigned cases = 0;
  for (const auto& [name, mutate] : mutations) {
    for (unsigned position = 0; position < 2; ++position) {
      for (unsigned reverse = 0; reverse < 2; ++reverse) {
        auto inventory = f.inventory;
        mutate(inventory, position);
        if (reverse) std::reverse(inventory.entries.begin(), inventory.entries.end());
        const auto published = mga::PublishStatementStableSnapshotVector(inventory, f.snapshot.owning_transaction, 3000);
        if (published.ok() || published.descriptor.snapshot_uuid.valid()) ++missed[0];
        // Preserve the original failure without contaminating following cases.
        if (published.descriptor.snapshot_uuid.valid()) {
          mga::RevokePublishedSnapshotVector(published.descriptor.snapshot_uuid);
          mga::ReleasePublishedSnapshotVector(published.descriptor.snapshot_uuid);
        }
        const auto local = mga::CreateLocalTransactionSnapshot(inventory, f.snapshot.owning_transaction);
        if (local.ok()) ++missed[1];
        const auto current = mga::ResolveCurrentStatementStableSnapshotVector(inventory,
            f.snapshot.snapshot_uuid, f.snapshot.owning_transaction_uuid, f.snapshot.owning_transaction);
        if (current.ok() || current.descriptor.snapshot_uuid.valid()) ++missed[2];
        const auto retention = mga::PublishedSnapshotRetentionHorizons(inventory);
        if (retention.ok() || !retention.horizons.empty()) ++missed[3];
        mga::AuthoritativeCleanupHorizonRequest request;
        request.inventory = inventory;
        request.inventory_authoritative = true;
        request.inventory_complete = true;
        const auto cleanup = mga::ComputeAuthoritativeCleanupHorizon(request);
        if (cleanup.ok() || cleanup.cleanup_horizon_authoritative || cleanup.cleanup_horizon.valid()) ++missed[4];
        Check(mga::SnapshotVectorDescriptorEqual(
            mga::ResolvePublishedSnapshotVector(f.snapshot.snapshot_uuid).descriptor, f.snapshot),
            "malformed inventory admission must not mutate live snapshot authority");
        ++cases;
      }
    }
  }
  std::cout << "inventory_admission cases=" << cases << " missed_by_route=";
  for (const auto count : missed) std::cout << count << ',';
  std::cout << '\n';
  Check(cases == 172 && std::all_of(missed.begin(), missed.end(), [](auto n) { return n == 0; }),
        "all malformed inventories rejected at all five authoritative consumers");
  // All existing non-none state values; both typed scopes and all RFC variant nibbles.
  unsigned positives = 0;
  for (unsigned state = 1; state <= 13; ++state) {
    for (unsigned scope = 0; scope < 2; ++scope) {
      for (unsigned variant = 8; variant <= 11; ++variant) {
        auto inventory = f.inventory;
        auto& entry = inventory.entries[0];
        entry.state = static_cast<mga::TransactionState>(state);
        entry.identity.scope = static_cast<mga::TransactionScope>(scope);
        entry.identity.transaction_uuid.value.bytes[8] = static_cast<platform::byte>(
            (entry.identity.transaction_uuid.value.bytes[8] & 15U) | (variant << 4U));
        const auto published = mga::PublishStatementStableSnapshotVector(inventory, f.snapshot.owning_transaction, 3001);
        Check(published.ok(), "valid inventory state and binary identity must not be over-rejected");
        mga::ReleasePublishedSnapshotVector(published.descriptor.snapshot_uuid);
        Check(mga::CreateLocalTransactionSnapshot(inventory, f.snapshot.owning_transaction).ok(), "valid local snapshot");
        Check(mga::ResolveCurrentStatementStableSnapshotVector(inventory, f.snapshot.snapshot_uuid,
            f.snapshot.owning_transaction_uuid, f.snapshot.owning_transaction).ok(), "valid current snapshot");
        Check(mga::PublishedSnapshotRetentionHorizons(inventory).ok(), "valid retained snapshot inventory");
        mga::AuthoritativeCleanupHorizonRequest request;
        request.inventory = inventory; request.inventory_authoritative = true; request.inventory_complete = true;
        Check(mga::ComputeAuthoritativeCleanupHorizon(request).ok(), "valid cleanup inventory");
        ++positives;
      }
    }
  }
  Check(positives == 104, "complete finite positive inventory grid");
  std::cout << "inventory_admission positive_cases=" << positives << '\n';
  // Exhaust the complete encoded membership domains independently of the
  // implementation switch. These are structural checks, not lifecycle proof.
  for (unsigned encoded = 0; encoded <= 65535; ++encoded) {
    auto inventory = f.inventory;
    inventory.entries[0].state = static_cast<mga::TransactionState>(encoded);
    Check((*mga::ValidateLocalTransactionInventoryStructure(inventory) == '\0') ==
        (encoded >= 1 && encoded <= 13), "complete u16 state membership domain");
    inventory = f.inventory;
    inventory.entries[0].identity.scope = static_cast<mga::TransactionScope>(encoded);
    Check((*mga::ValidateLocalTransactionInventoryStructure(inventory) == '\0') ==
        (encoded == 0 || encoded == 1), "complete u16 scope membership domain");
    inventory = f.inventory;
    auto& bytes = inventory.entries[0].identity.transaction_uuid.value.bytes;
    bytes[6] = static_cast<platform::byte>(encoded >> 8);
    bytes[8] = static_cast<platform::byte>(encoded & 255U);
    const bool admitted_uuid = (encoded & 0xf000U) == 0x7000U &&
                               (encoded & 0xc0U) == 0x80U;
    Check((*mga::ValidateLocalTransactionInventoryStructure(inventory) == '\0') == admitted_uuid,
          "complete version/variant octet domain preserves ordinary UUID data bits");
  }
  auto empty = mga::MakeEmptyLocalTransactionInventory();
  Check(*mga::ValidateLocalTransactionInventoryStructure(empty) == '\0', "empty initial inventory is valid");
  empty.next_local_transaction_id = std::numeric_limits<platform::u64>::max();
  Check(*mga::ValidateLocalTransactionInventoryStructure(empty) == '\0', "empty compacted inventory does not require dense IDs");
  auto sparse = f.inventory;
  sparse.next_local_transaction_id = std::numeric_limits<platform::u64>::max();
  sparse.entries[0].identity.local_id.value = sparse.next_local_transaction_id - 1;
  Check(*mga::ValidateLocalTransactionInventoryStructure(sparse) == '\0', "sparse highest allocated number remains below next");
  std::reverse(sparse.entries.begin(), sparse.entries.end());
  Check(*mga::ValidateLocalTransactionInventoryStructure(sparse) == '\0', "inventory ordering is not identity authority");
  std::cout << "inventory_admission encoded_membership_cases=196608 sparse_empty_cases=4\n";
}

static void AdditionalCases() {
  Fixture f;
  auto pin = mga::RetainPublishedSnapshotVector(f.snapshot.snapshot_uuid);
  Check(pin.valid(), "cleanup consumer owns actual snapshot pin");
  const auto row_id = uuid::GenerateEngineIdentityV7(platform::UuidKind::row, 2000);
  Check(row_id.ok(), "row identity issuance");
  mga::RowVersionMetadata row;
  row.identity.row.row_uuid = row_id.value;
  row.identity.creator_transaction = f.inventory.entries[0].identity;
  row.identity.version_sequence = 1;
  row.state = mga::RowVersionState::delete_marker;
  row.creator_transaction_state = mga::TransactionState::committed;
  Check(mga::ValidateRowVersionMetadata(row).ok(), "real cleanup input is canonical");
  mga::LocalCleanupWorksetRequest request;
  request.inventory = f.inventory;
  request.inventory_authoritative = true;
  request.row_versions = {row};
  request.emit_reclaim_evidence_records = true;
  mga::ReleasePublishedSnapshotVector(f.snapshot.snapshot_uuid);
  const auto held = mga::ApplyLocalCleanupWithAuthoritativeInventory(request);
  Check(held.ok() && held.retained_row_version_count == 1 &&
        held.reclaimed_row_version_count == 0 && held.reclaim_evidence_records.empty(),
        "actual cleanup workset retains version under pinned snapshot");
  pin.Release();
  const auto unbudgeted = mga::ApplyLocalCleanupWithAuthoritativeInventory(request);
  Check(!unbudgeted.ok() && unbudgeted.bounded_memory_limit_hit &&
        unbudgeted.diagnostic.diagnostic_code == "SB-SNTXN-CLEANUP-RECLAIM-EVIDENCE-LIMIT-EXCEEDED",
        "cleanup evidence requires an explicit positive budget");
  request.max_reclaim_evidence_records = 1;
  const auto freed = mga::ApplyLocalCleanupWithAuthoritativeInventory(request);
  Check(freed.ok() && freed.retained_row_version_count == 0 &&
        freed.reclaimed_row_version_count == 1 && freed.reclaim_evidence_records.size() == 1,
        "actual cleanup workset reclaims after final pin release");
  for (unsigned iteration = 0; iteration < 64; ++iteration) {
    Fixture raced;
    std::atomic<unsigned> ready{0}, failures{0};
    std::atomic<bool> revoked{false};
    std::vector<std::thread> readers;
    for (unsigned i = 0; i < 4; ++i) {
      auto retained = mga::RetainPublishedSnapshotVector(raced.snapshot.snapshot_uuid);
      Check(retained.valid(), "competing revocation pin issuance");
      readers.emplace_back([pin = std::move(retained), &ready, &failures, &revoked]() mutable {
        if (!pin.Resolve().ok()) ++failures;
        ready.fetch_add(1, std::memory_order_release);
        while (!revoked.load(std::memory_order_acquire)) std::this_thread::yield();
        if (pin.Resolve().ok()) ++failures;
        pin.Release();
      });
    }
    while (ready.load(std::memory_order_acquire) != 4) std::this_thread::yield();
    mga::RevokePublishedSnapshotVectorsForTransaction(raced.snapshot.owning_transaction_uuid,
                                                      raced.snapshot.owning_transaction);
    mga::ReleasePublishedSnapshotVector(raced.snapshot.snapshot_uuid);
    revoked.store(true, std::memory_order_release);
    for (auto& reader : readers) reader.join();
    Check(failures.load() == 0, "revocation fences every retained reader");
    Check(raced.Cleanup().cleanup_horizon.value == 2, "revoked readers hold no cleanup horizon");
  }
}
