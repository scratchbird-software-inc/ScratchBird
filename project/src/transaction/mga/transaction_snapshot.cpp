// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_snapshot.hpp"

#include "uuid.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::transaction::mga {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::GenerateDurableEngineIdentityV7;
using scratchbird::core::uuid::UuidToString;

Status SnapshotOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status SnapshotErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::transaction_mga};
}

u64 LatestCommittedLocalTransactionId(const LocalTransactionInventory& inventory) {
  u64 latest = kInvalidLocalTransactionId;
  for (const TransactionInventoryEntry& entry : inventory.entries) {
    if ((entry.state == TransactionState::committed ||
         entry.state == TransactionState::archived) &&
        entry.identity.local_id.valid() &&
        entry.identity.local_id.value > latest) {
      latest = entry.identity.local_id.value;
    }
  }
  return latest;
}

struct PublishedSnapshotVector {
  SnapshotVectorDescriptor descriptor;
  bool revoked = false;
};

std::mutex& SnapshotVectorRegistryMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, PublishedSnapshotVector>& SnapshotVectorRegistry() {
  static std::map<std::string, PublishedSnapshotVector> registry;
  return registry;
}

SnapshotVectorResult SnapshotVectorError(std::string diagnostic_code,
                                         std::string message_key,
                                         std::string detail = {}) {
  SnapshotVectorResult result;
  result.status = SnapshotErrorStatus();
  result.diagnostic = MakeTransactionSnapshotDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key),
      std::move(detail));
  return result;
}

bool IsSnapshotOwnerEligible(const TransactionState state) {
  return state == TransactionState::active ||
         state == TransactionState::read_only_active;
}

bool IsActiveSnapshotExclusion(const TransactionState state) {
  return state == TransactionState::active ||
         state == TransactionState::read_only_active;
}

bool IsInDoubtSnapshotExclusion(const TransactionState state) {
  return state == TransactionState::created ||
         state == TransactionState::preparing ||
         state == TransactionState::prepared ||
         state == TransactionState::committing ||
         state == TransactionState::rolling_back ||
         state == TransactionState::limbo ||
         state == TransactionState::recovering;
}

bool IsCanonicalExclusionVector(const std::vector<u64>& values,
                                const u64 publication_ceiling) {
  return std::is_sorted(values.begin(), values.end()) &&
         std::adjacent_find(values.begin(), values.end()) == values.end() &&
         std::all_of(values.begin(), values.end(), [&](const u64 value) {
           return value != kInvalidLocalTransactionId &&
                  value <= publication_ceiling;
         });
}

bool SnapshotVectorStructurallyComplete(
    const SnapshotVectorDescriptor& descriptor) {
  if (!descriptor.snapshot_uuid.valid() ||
      descriptor.snapshot_uuid.kind != UuidKind::object ||
      !descriptor.owning_transaction_uuid.valid() ||
      descriptor.owning_transaction_uuid.kind != UuidKind::transaction ||
      !descriptor.owning_transaction.valid() ||
      descriptor.snapshot_kind != SnapshotVectorKind::statement_stable ||
      descriptor.publication_inventory_next_local_transaction_id == 0 ||
      descriptor.visible_committed_high_watermark >=
          descriptor.publication_inventory_next_local_transaction_id ||
      descriptor.owning_transaction.value >=
          descriptor.publication_inventory_next_local_transaction_id ||
      !descriptor.oldest_active_transaction.valid() ||
      !descriptor.oldest_interesting_transaction.valid() ||
      !descriptor.oldest_snapshot_transaction.valid() ||
      !descriptor.retention_horizon_transaction.valid() ||
      !descriptor.inventory_authoritative || !descriptor.complete ||
      descriptor.oldest_active_transaction.value >=
          descriptor.publication_inventory_next_local_transaction_id ||
      descriptor.oldest_interesting_transaction.value >=
          descriptor.publication_inventory_next_local_transaction_id ||
      descriptor.oldest_snapshot_transaction.value >=
          descriptor.publication_inventory_next_local_transaction_id ||
      descriptor.retention_horizon_transaction.value >=
          descriptor.publication_inventory_next_local_transaction_id ||
      descriptor.oldest_snapshot_transaction.value !=
          descriptor.retention_horizon_transaction.value ||
      descriptor.oldest_snapshot_transaction.value !=
          std::min(descriptor.oldest_interesting_transaction.value,
                   descriptor.owning_transaction.value) ||
      !IsCanonicalExclusionVector(
          descriptor.active_excluded_local_transaction_ids,
          descriptor.publication_inventory_next_local_transaction_id - 1) ||
      !IsCanonicalExclusionVector(
          descriptor.in_doubt_excluded_local_transaction_ids,
          descriptor.publication_inventory_next_local_transaction_id - 1) ||
      !std::binary_search(
          descriptor.active_excluded_local_transaction_ids.begin(),
          descriptor.active_excluded_local_transaction_ids.end(),
          descriptor.owning_transaction.value)) {
    return false;
  }
  std::vector<u64> intersection;
  std::set_intersection(
      descriptor.active_excluded_local_transaction_ids.begin(),
      descriptor.active_excluded_local_transaction_ids.end(),
      descriptor.in_doubt_excluded_local_transaction_ids.begin(),
      descriptor.in_doubt_excluded_local_transaction_ids.end(),
      std::back_inserter(intersection));
  return intersection.empty();
}

}  // namespace

TransactionSnapshotResult CreateLocalTransactionSnapshot(const LocalTransactionInventory& inventory,
                                                         LocalTransactionId reader_transaction) {
  TransactionSnapshotResult result;
  result.status = SnapshotOkStatus();
  const auto lookup = LookupLocalTransaction(inventory, reader_transaction);
  if (!lookup.ok()) {
    result.status = lookup.status;
    result.diagnostic = lookup.diagnostic;
    return result;
  }
  if (lookup.entry.state != TransactionState::active &&
      lookup.entry.state != TransactionState::read_only_active &&
      lookup.entry.state != TransactionState::preparing &&
      lookup.entry.state != TransactionState::prepared) {
    result.status = SnapshotErrorStatus();
    result.diagnostic = MakeTransactionSnapshotDiagnostic(result.status,
                                                          "SB-TXN-SNAPSHOT-UNSUPPORTED-STATE",
                                                          "transaction.snapshot.unsupported_state",
                                                          TransactionStateName(lookup.entry.state));
    return result;
  }
  const auto horizons = ComputeLocalTransactionHorizons(inventory);
  if (!horizons.ok()) {
    result.status = horizons.status;
    result.diagnostic = horizons.diagnostic;
    return result;
  }

  result.snapshot.reader_transaction = reader_transaction;
  result.snapshot.visible_through_local_transaction =
      MakeLocalTransactionId(LatestCommittedLocalTransactionId(inventory));
  result.snapshot.transaction_start_visible_through_local_transaction =
      MakeLocalTransactionId(lookup.entry.begin_visible_through_local_transaction_id);
  result.snapshot.oldest_active_transaction = horizons.horizons.oldest_active_transaction;
  result.snapshot.oldest_snapshot_transaction = horizons.horizons.oldest_snapshot_transaction;
  result.snapshot.allow_reader_own_uncommitted = true;
  result.visibility_snapshot.reader_transaction = reader_transaction;
  result.visibility_snapshot.visible_through_local_transaction_id =
      result.snapshot.visible_through_local_transaction.value;
  result.visibility_snapshot.visible_through_local_transaction_id_is_boundary = true;
  result.visibility_snapshot.allow_reader_own_uncommitted = true;
  result.visibility_snapshot.recovery_context = false;
  return result;
}

const char* SnapshotVectorKindName(const SnapshotVectorKind kind) {
  switch (kind) {
    case SnapshotVectorKind::statement_stable:
      return "statement_stable";
    case SnapshotVectorKind::unknown:
      return "unknown";
  }
  return "unknown";
}

bool SnapshotVectorDescriptorEqual(const SnapshotVectorDescriptor& left,
                                   const SnapshotVectorDescriptor& right) {
  return left.snapshot_uuid.kind == right.snapshot_uuid.kind &&
         left.snapshot_uuid.value == right.snapshot_uuid.value &&
         left.owning_transaction_uuid.kind ==
             right.owning_transaction_uuid.kind &&
         left.owning_transaction_uuid.value ==
             right.owning_transaction_uuid.value &&
         left.owning_transaction.value == right.owning_transaction.value &&
         left.visible_committed_high_watermark ==
             right.visible_committed_high_watermark &&
         left.oldest_active_transaction.value ==
             right.oldest_active_transaction.value &&
         left.oldest_interesting_transaction.value ==
             right.oldest_interesting_transaction.value &&
         left.oldest_snapshot_transaction.value ==
             right.oldest_snapshot_transaction.value &&
         left.retention_horizon_transaction.value ==
             right.retention_horizon_transaction.value &&
         left.active_excluded_local_transaction_ids ==
             right.active_excluded_local_transaction_ids &&
         left.in_doubt_excluded_local_transaction_ids ==
             right.in_doubt_excluded_local_transaction_ids &&
         left.snapshot_kind == right.snapshot_kind &&
         left.publication_inventory_next_local_transaction_id ==
             right.publication_inventory_next_local_transaction_id &&
         left.inventory_authoritative == right.inventory_authoritative &&
         left.complete == right.complete;
}

SnapshotVectorResult PublishStatementStableSnapshotVector(
    const LocalTransactionInventory& inventory,
    const LocalTransactionId owning_transaction,
    const u64 publication_unix_epoch_millis) {
  if (publication_unix_epoch_millis == 0 ||
      inventory.next_local_transaction_id == 0) {
    return SnapshotVectorError(
        "SB-MGA-SNAPSHOT-VECTOR-INVENTORY-INVALID",
        "transaction.snapshot_vector.inventory_invalid");
  }
  const auto owner = LookupLocalTransaction(inventory, owning_transaction);
  if (!owner.ok() || !IsSnapshotOwnerEligible(owner.entry.state)) {
    return SnapshotVectorError(
        "SB-MGA-SNAPSHOT-VECTOR-OWNER-INELIGIBLE",
        "transaction.snapshot_vector.owner_ineligible",
        owner.ok() ? TransactionStateName(owner.entry.state)
                   : std::to_string(owning_transaction.value));
  }
  if (owner.entry.identity.transaction_uuid.kind != UuidKind::transaction ||
      !owner.entry.identity.transaction_uuid.valid() ||
      inventory.next_local_transaction_id <= owning_transaction.value) {
    return SnapshotVectorError(
        "SB-MGA-SNAPSHOT-VECTOR-OWNER-MISMATCH",
        "transaction.snapshot_vector.owner_mismatch");
  }

  const auto established_snapshot =
      CreateLocalTransactionSnapshot(inventory, owning_transaction);
  if (!established_snapshot.ok()) {
    SnapshotVectorResult result;
    result.status = established_snapshot.status;
    result.diagnostic = established_snapshot.diagnostic;
    return result;
  }
  const auto horizons = ComputeLocalTransactionHorizons(inventory);
  if (!horizons.ok()) {
    SnapshotVectorResult result;
    result.status = horizons.status;
    result.diagnostic = horizons.diagnostic;
    return result;
  }

  SnapshotVectorDescriptor descriptor;
  descriptor.owning_transaction_uuid = owner.entry.identity.transaction_uuid;
  descriptor.owning_transaction = owning_transaction;
  descriptor.visible_committed_high_watermark =
      established_snapshot.snapshot.visible_through_local_transaction.value;
  descriptor.oldest_active_transaction =
      established_snapshot.snapshot.oldest_active_transaction;
  descriptor.oldest_interesting_transaction =
      horizons.horizons.oldest_interesting_transaction;
  descriptor.oldest_snapshot_transaction = MakeLocalTransactionId(
      std::min(horizons.horizons.oldest_interesting_transaction.value,
               owning_transaction.value));
  descriptor.retention_horizon_transaction =
      descriptor.oldest_snapshot_transaction;
  descriptor.snapshot_kind = SnapshotVectorKind::statement_stable;
  descriptor.publication_inventory_next_local_transaction_id =
      inventory.next_local_transaction_id;

  for (const auto& entry : inventory.entries) {
    if (!entry.identity.valid() ||
        entry.identity.local_id.value >
            descriptor.publication_inventory_next_local_transaction_id - 1) {
      if (!entry.identity.valid()) {
        return SnapshotVectorError(
            "SB-MGA-SNAPSHOT-VECTOR-INVENTORY-INVALID",
            "transaction.snapshot_vector.inventory_entry_invalid");
      }
      continue;
    }
    if (IsActiveSnapshotExclusion(entry.state)) {
      descriptor.active_excluded_local_transaction_ids.push_back(
          entry.identity.local_id.value);
    } else if (IsInDoubtSnapshotExclusion(entry.state)) {
      descriptor.in_doubt_excluded_local_transaction_ids.push_back(
          entry.identity.local_id.value);
    }
  }
  std::sort(descriptor.active_excluded_local_transaction_ids.begin(),
            descriptor.active_excluded_local_transaction_ids.end());
  std::sort(descriptor.in_doubt_excluded_local_transaction_ids.begin(),
            descriptor.in_doubt_excluded_local_transaction_ids.end());

  constexpr u64 kGenerationAttempts = 32;
  for (u64 attempt = 0; attempt < kGenerationAttempts; ++attempt) {
    const auto generated = GenerateDurableEngineIdentityV7(
        UuidKind::object, publication_unix_epoch_millis + attempt);
    if (!generated.ok()) {
      SnapshotVectorResult result;
      result.status = generated.status;
      result.diagnostic = generated.diagnostic;
      return result;
    }
    const std::string key = UuidToString(generated.value.value);
    std::lock_guard<std::mutex> guard(SnapshotVectorRegistryMutex());
    if (SnapshotVectorRegistry().find(key) != SnapshotVectorRegistry().end()) {
      continue;
    }
    descriptor.snapshot_uuid = generated.value;
    descriptor.inventory_authoritative = true;
    descriptor.complete = true;
    if (!SnapshotVectorStructurallyComplete(descriptor)) {
      return SnapshotVectorError(
          "SB-MGA-SNAPSHOT-VECTOR-INCOMPLETE",
          "transaction.snapshot_vector.incomplete");
    }
    SnapshotVectorRegistry().emplace(
        key, PublishedSnapshotVector{descriptor, false});
    SnapshotVectorResult result;
    result.status = SnapshotOkStatus();
    result.descriptor = std::move(descriptor);
    return result;
  }
  return SnapshotVectorError(
      "SB-MGA-SNAPSHOT-VECTOR-UUID-COLLISION",
      "transaction.snapshot_vector.uuid_collision");
}

SnapshotVectorResult ResolvePublishedSnapshotVector(
    const TypedUuid& snapshot_uuid) {
  if (!snapshot_uuid.valid() || snapshot_uuid.kind != UuidKind::object) {
    return SnapshotVectorError(
        "SB-MGA-SNAPSHOT-VECTOR-UUID-INVALID",
        "transaction.snapshot_vector.uuid_invalid");
  }
  const std::string key = UuidToString(snapshot_uuid.value);
  std::lock_guard<std::mutex> guard(SnapshotVectorRegistryMutex());
  const auto found = SnapshotVectorRegistry().find(key);
  if (found == SnapshotVectorRegistry().end()) {
    return SnapshotVectorError(
        "SB-MGA-SNAPSHOT-VECTOR-UNKNOWN",
        "transaction.snapshot_vector.unknown", key);
  }
  if (found->second.revoked) {
    return SnapshotVectorError(
        "SB-MGA-SNAPSHOT-VECTOR-REVOKED",
        "transaction.snapshot_vector.revoked", key);
  }
  if (!SnapshotVectorStructurallyComplete(found->second.descriptor)) {
    return SnapshotVectorError(
        "SB-MGA-SNAPSHOT-VECTOR-INCOMPLETE",
        "transaction.snapshot_vector.incomplete", key);
  }
  SnapshotVectorResult result;
  result.status = SnapshotOkStatus();
  result.descriptor = found->second.descriptor;
  return result;
}

SnapshotVectorResult ResolveCurrentStatementStableSnapshotVector(
    const LocalTransactionInventory& inventory,
    const TypedUuid& snapshot_uuid,
    const TypedUuid& expected_owning_transaction_uuid,
    const LocalTransactionId expected_owning_transaction) {
  auto resolved = ResolvePublishedSnapshotVector(snapshot_uuid);
  if (!resolved.ok()) return resolved;
  const auto& descriptor = resolved.descriptor;
  if (descriptor.snapshot_kind != SnapshotVectorKind::statement_stable ||
      expected_owning_transaction_uuid.kind != UuidKind::transaction ||
      !expected_owning_transaction_uuid.valid() ||
      descriptor.owning_transaction_uuid.value !=
          expected_owning_transaction_uuid.value ||
      descriptor.owning_transaction.value != expected_owning_transaction.value) {
    return SnapshotVectorError(
        "SB-MGA-SNAPSHOT-VECTOR-OWNER-MISMATCH",
        "transaction.snapshot_vector.owner_mismatch");
  }
  if (inventory.next_local_transaction_id == 0 ||
      expected_owning_transaction.value >=
          inventory.next_local_transaction_id ||
      inventory.next_local_transaction_id <
          descriptor.publication_inventory_next_local_transaction_id) {
    return SnapshotVectorError(
        "SB-MGA-SNAPSHOT-VECTOR-STALE",
        "transaction.snapshot_vector.stale_inventory_generation");
  }
  const auto owner = LookupLocalTransaction(inventory, expected_owning_transaction);
  if (!owner.ok() || !IsSnapshotOwnerEligible(owner.entry.state) ||
      owner.entry.identity.transaction_uuid.value !=
          expected_owning_transaction_uuid.value) {
    return SnapshotVectorError(
        "SB-MGA-SNAPSHOT-VECTOR-STALE",
        "transaction.snapshot_vector.stale_owner");
  }
  // Captured exclusion vectors are immutable contents.  Current validation
  // deliberately does not recompute or compare them with the later inventory.
  return resolved;
}

void RevokePublishedSnapshotVectorsForTransaction(
    const TypedUuid& owning_transaction_uuid,
    const LocalTransactionId owning_transaction) {
  if (!owning_transaction_uuid.valid() ||
      owning_transaction_uuid.kind != UuidKind::transaction ||
      !owning_transaction.valid()) {
    return;
  }
  std::lock_guard<std::mutex> guard(SnapshotVectorRegistryMutex());
  for (auto& [key, published] : SnapshotVectorRegistry()) {
    (void)key;
    if (published.descriptor.owning_transaction.value ==
            owning_transaction.value &&
        published.descriptor.owning_transaction_uuid.value ==
            owning_transaction_uuid.value) {
      published.revoked = true;
    }
  }
}

void RevokePublishedSnapshotVector(const TypedUuid& snapshot_uuid) {
  if (!snapshot_uuid.valid() || snapshot_uuid.kind != UuidKind::object) {
    return;
  }
  std::lock_guard<std::mutex> guard(SnapshotVectorRegistryMutex());
  const auto found = SnapshotVectorRegistry().find(
      UuidToString(snapshot_uuid.value));
  if (found != SnapshotVectorRegistry().end()) {
    found->second.revoked = true;
  }
}

DiagnosticRecord MakeTransactionSnapshotDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "transaction.mga.snapshot");
}

}  // namespace scratchbird::transaction::mga
