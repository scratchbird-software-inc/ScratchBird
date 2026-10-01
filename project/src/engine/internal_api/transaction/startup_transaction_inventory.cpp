// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "startup_transaction_inventory.hpp"

#include "local_transaction_store.hpp"
#include "transaction_api.hpp"
#include "uuid.hpp"

namespace scratchbird::engine::internal_api {

namespace {
namespace mga = scratchbird::transaction::mga;
namespace storage = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using Outcome = StartupTransactionInventoryOutcome;

StartupTransactionInventoryEntryObservation ObserveEntry(
    const mga::LocalTransactionInventory& inventory,
    const StartupTransactionInventoryIdentity& request) {
  StartupTransactionInventoryEntryObservation result;

  // Require the exact composite identity and uniqueness in both directions.
  // A reused local number or UUID never names the retained original work.
  const mga::TransactionInventoryEntry* match = nullptr;
  std::size_t local_matches = 0;
  std::size_t uuid_matches = 0;
  for (const auto& entry : inventory.entries) {
    const bool same_local = entry.identity.local_id.value == request.local_transaction_id;
    const bool same_uuid = entry.identity.transaction_uuid.value == request.transaction_uuid;
    local_matches += same_local;
    uuid_matches += same_uuid;
    if (same_local && same_uuid) match = &entry;
  }
  if (local_matches > 1 || uuid_matches > 1) {
    result.outcome = Outcome::identity_ambiguous;
    return result;
  }
  if (!match) {
    result.outcome = local_matches || uuid_matches ? Outcome::identity_mismatch
                                                 : Outcome::identity_missing;
    return result;
  }
  result.observed_state = match->state;
  result.outcome = Outcome::unresolved;
  // Startup's local transaction recovery cannot decide cluster-owned work.
  if (match->identity.scope != mga::TransactionScope::local_node) return result;
  switch (mga::InventoryVisibilityState(*match)) {
    case mga::TransactionState::committed:
      result.outcome = Outcome::committed;
      break;
    case mga::TransactionState::rolled_back:
      result.outcome = Outcome::rolled_back;
      break;
    default:
      break;
  }
  return result;
}
}  // namespace

StartupTransactionInventoryBatchObservation InspectStartupTransactionInventories(
    const std::string& database_path, EngineUuid database_uuid,
    std::span<const StartupTransactionInventoryIdentity> identities,
    std::span<StartupTransactionInventoryEntryObservation> output) {
  using BatchOutcome = StartupTransactionInventoryBatchOutcome;
  StartupTransactionInventoryBatchObservation result;
  if (database_path.empty() || !uuid::IsEngineIdentityUuid(database_uuid) ||
      identities.empty() || output.size() < identities.size()) return result;
  // Validate the complete set before writing anything or opening the database.
  for (const auto& identity : identities) {
    if (!uuid::IsEngineIdentityUuid(identity.transaction_uuid) ||
        identity.local_transaction_id == 0) return result;
  }
  result.outcome = BatchOutcome::authority_unavailable;
  const auto guard = AcquireTransactionInventoryGuard(database_path);
  const auto loaded = storage::AcquireStrongLocalTransactionInventorySnapshot(database_path);
  if (!loaded.ok()) {
    result.diagnostic = loaded.diagnostic;
    return result;
  }
  const auto& inventory = loaded.snapshot->inventory;
  if (!inventory.publication_base || inventory.publication_base->generation == 0)
    return result;
  result.publication_base = inventory.publication_base;
  if (result.publication_base->database_uuid != database_uuid) {
    result.outcome = BatchOutcome::database_mismatch;
    return result;
  }
  for (std::size_t i = 0; i != identities.size(); ++i)
    output[i] = ObserveEntry(inventory, identities[i]);
  result.records_written = identities.size();
  result.outcome = BatchOutcome::observed;
  return result;
}

StartupTransactionInventoryObservation InspectStartupTransactionInventory(
    const StartupTransactionInventoryRequest& request) {
  const StartupTransactionInventoryIdentity identity{
      request.transaction_uuid, request.local_transaction_id};
  StartupTransactionInventoryEntryObservation entry;
  const auto batch = InspectStartupTransactionInventories(
      request.database_path, request.database_uuid, {&identity, 1}, {&entry, 1});
  StartupTransactionInventoryObservation result;
  result.publication_base = batch.publication_base;
  result.diagnostic = batch.diagnostic;
  using BatchOutcome = StartupTransactionInventoryBatchOutcome;
  switch (batch.outcome) {
    case BatchOutcome::invalid_request: result.outcome = Outcome::invalid_request; break;
    case BatchOutcome::authority_unavailable: break;
    case BatchOutcome::database_mismatch: result.outcome = Outcome::database_mismatch; break;
    case BatchOutcome::observed:
      result.outcome = entry.outcome;
      result.observed_state = entry.observed_state;
      break;
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
