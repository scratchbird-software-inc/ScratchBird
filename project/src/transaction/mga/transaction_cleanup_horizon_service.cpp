// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_cleanup_horizon_service.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace scratchbird::transaction::mga {
namespace {

// DPC_AUTHORITATIVE_CLEANUP_HORIZON_SERVICE
//
// Engine-owned cleanup horizon authority for storage and index maintenance.
// The service consumes durable MGA transaction inventory and active snapshot
// bindings only; parser, client, timestamp, UUID-order, and event-stream state
// are reported as non-authoritative evidence.
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status CleanupHorizonOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status CleanupHorizonErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::transaction_mga};
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

void AddEvidence(AuthoritativeCleanupHorizonResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back({std::move(key), std::move(value)});
}

void AddBaseEvidence(AuthoritativeCleanupHorizonResult* result,
                     const AuthoritativeCleanupHorizonRequest& request) {
  AddEvidence(result, "cleanup_horizon_service", "dpc030_authoritative_cleanup_horizon_v1");
  AddEvidence(result, "authority_source", "durable_mga_transaction_inventory");
  AddEvidence(result, "inventory_authoritative", BoolText(request.inventory_authoritative));
  AddEvidence(result, "inventory_complete", BoolText(request.inventory_complete));
  AddEvidence(result, "active_snapshot_inventory_authoritative",
              BoolText(request.active_snapshot_inventory_authoritative));
  AddEvidence(result, "always_in_transaction_policy",
              BoolText(request.always_in_transaction_policy));
  AddEvidence(result, "always_active_session_inventory_authoritative",
              BoolText(request.always_active_session_inventory_authoritative));
  AddEvidence(result, "parser_finality_authority", "false");
  AddEvidence(result, "client_state_authority", "false");
  AddEvidence(result, "timestamp_ordering_authority", "false");
  AddEvidence(result, "uuid_ordering_authority", "false");
  AddEvidence(result, "crud_event_stream_authority", "false");
  AddEvidence(result, "cluster_private_implementation", "false");
}

AuthoritativeCleanupHorizonResult ServiceError(const AuthoritativeCleanupHorizonRequest& request,
                                               std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail = {}) {
  AuthoritativeCleanupHorizonResult result;
  result.status = CleanupHorizonErrorStatus();
  AddBaseEvidence(&result, request);
  AddEvidence(&result, "fail_closed", "true");
  result.diagnostic = MakeCleanupHorizonServiceDiagnostic(result.status,
                                                          std::move(diagnostic_code),
                                                          std::move(message_key),
                                                          std::move(detail));
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}

bool IsActiveInventoryState(TransactionState state) {
  return state == TransactionState::active ||
         state == TransactionState::read_only_active ||
         state == TransactionState::preparing ||
         state == TransactionState::prepared ||
         state == TransactionState::committing ||
         state == TransactionState::limbo ||
         state == TransactionState::recovering;
}

bool IsAlwaysActiveSessionState(TransactionState state) {
  return state == TransactionState::active ||
         state == TransactionState::read_only_active;
}

bool IsUnresolvedOutcomeState(TransactionState state) {
  return state == TransactionState::preparing ||
         state == TransactionState::prepared ||
         state == TransactionState::committing ||
         state == TransactionState::limbo ||
         state == TransactionState::recovering ||
         state == TransactionState::failed_terminal;
}

const TransactionInventoryEntry* FindEntry(const LocalTransactionInventory& inventory,
                                           LocalTransactionId local_id) {
  for (const TransactionInventoryEntry& entry : inventory.entries) {
    if (entry.identity.local_id.value == local_id.value) {
      return &entry;
    }
  }
  return nullptr;
}

AuthoritativeCleanupHorizonResult ValidateInventory(
    const AuthoritativeCleanupHorizonRequest& request) {
  if (!request.inventory_authoritative) {
    return ServiceError(request,
                        "SB-MGA-CLEANUP-HORIZON-NOT-AUTHORITATIVE",
                        "transaction.cleanup_horizon.inventory_not_authoritative",
                        "durable MGA transaction inventory authority is required");
  }
  if (!request.inventory_complete) {
    return ServiceError(request,
                        "SB-MGA-CLEANUP-HORIZON-INVENTORY-MISSING",
                        "transaction.cleanup_horizon.inventory_missing",
                        "complete durable transaction inventory is required");
  }
  if (request.inventory.next_local_transaction_id == kInvalidLocalTransactionId) {
    return ServiceError(request,
                        "SB-MGA-CLEANUP-HORIZON-INVENTORY-INVALID",
                        "transaction.cleanup_horizon.next_transaction_invalid");
  }
  for (std::size_t i = 0; i < request.inventory.entries.size(); ++i) {
    const TransactionInventoryEntry& entry = request.inventory.entries[i];
    if (!entry.identity.valid()) {
      return ServiceError(request,
                          "SB-MGA-CLEANUP-HORIZON-INVENTORY-INVALID",
                          "transaction.cleanup_horizon.invalid_transaction_identity",
                          std::to_string(i));
    }
    if (entry.identity.local_id.value >= request.inventory.next_local_transaction_id) {
      return ServiceError(request,
                          "SB-MGA-CLEANUP-HORIZON-INVENTORY-INVALID",
                          "transaction.cleanup_horizon.future_transaction_in_inventory",
                          std::to_string(entry.identity.local_id.value));
    }
    for (std::size_t j = i + 1; j < request.inventory.entries.size(); ++j) {
      const TransactionInventoryEntry& other = request.inventory.entries[j];
      if (entry.identity.local_id.value == other.identity.local_id.value) {
        return ServiceError(request,
                            "SB-MGA-CLEANUP-HORIZON-INVENTORY-INVALID",
                            "transaction.cleanup_horizon.duplicate_local_transaction_id",
                            std::to_string(entry.identity.local_id.value));
      }
      if (entry.identity.transaction_uuid.value == other.identity.transaction_uuid.value) {
        return ServiceError(request,
                            "SB-MGA-CLEANUP-HORIZON-INVENTORY-INVALID",
                            "transaction.cleanup_horizon.duplicate_transaction_uuid",
                            std::to_string(entry.identity.local_id.value));
      }
    }
  }
  AuthoritativeCleanupHorizonResult result;
  result.status = CleanupHorizonOkStatus();
  return result;
}

AuthoritativeCleanupHorizonResult ValidateAlwaysActiveSessions(
    const AuthoritativeCleanupHorizonRequest& request) {
  if (!request.always_in_transaction_policy) {
    AuthoritativeCleanupHorizonResult result;
    result.status = CleanupHorizonOkStatus();
    return result;
  }
  if (!request.always_active_session_inventory_authoritative) {
    return ServiceError(request,
                        "SB-MGA-CLEANUP-HORIZON-SESSION-INVENTORY-NOT-AUTHORITATIVE",
                        "transaction.cleanup_horizon.session_inventory_not_authoritative",
                        "always-in-transaction sessions require authoritative active binding inventory");
  }
  for (const AlwaysActiveSessionTransactionBinding& binding : request.always_active_sessions) {
    if (!binding.authoritative) {
      return ServiceError(request,
                          "SB-MGA-CLEANUP-HORIZON-SESSION-BINDING-NOT-AUTHORITATIVE",
                          "transaction.cleanup_horizon.session_binding_not_authoritative",
                          binding.stable_session_id);
    }
    if (binding.stable_session_id.empty()) {
      return ServiceError(request,
                          "SB-MGA-CLEANUP-HORIZON-SESSION-BINDING-INVALID",
                          "transaction.cleanup_horizon.session_binding_missing_stable_id");
    }
    if (!binding.current_local_transaction_id.valid()) {
      return ServiceError(request,
                          "SB-MGA-CLEANUP-HORIZON-SESSION-BINDING-INVALID",
                          "transaction.cleanup_horizon.session_binding_missing_transaction",
                          binding.stable_session_id);
    }
    const TransactionInventoryEntry* entry = FindEntry(request.inventory,
                                                       binding.current_local_transaction_id);
    if (entry == nullptr) {
      return ServiceError(request,
                          "SB-MGA-CLEANUP-HORIZON-SESSION-TX-MISSING",
                          "transaction.cleanup_horizon.session_transaction_missing",
                          binding.stable_session_id);
    }
    if (!IsAlwaysActiveSessionState(entry->state)) {
      return ServiceError(request,
                          "SB-MGA-CLEANUP-HORIZON-SESSION-TX-NOT-ACTIVE",
                          "transaction.cleanup_horizon.session_transaction_not_active",
                          binding.stable_session_id + ":" + TransactionStateName(entry->state));
    }
  }
  AuthoritativeCleanupHorizonResult result;
  result.status = CleanupHorizonOkStatus();
  return result;
}

void AddInventoryBlockers(const LocalTransactionInventory& inventory,
                          AuthoritativeCleanupHorizonResult* result) {
  for (const TransactionInventoryEntry& entry : inventory.entries) {
    if (!IsActiveInventoryState(entry.state) && !IsUnresolvedOutcomeState(entry.state)) {
      continue;
    }
    CleanupHorizonBlocker blocker;
    blocker.kind = IsUnresolvedOutcomeState(entry.state)
                       ? CleanupHorizonBlockerKind::unresolved_outcome
                       : CleanupHorizonBlockerKind::active_transaction;
    blocker.local_transaction_id = entry.identity.local_id;
    blocker.observed_state = TransactionStateName(entry.state);
    blocker.stable_name = std::string("local_transaction:") +
                          std::to_string(entry.identity.local_id.value);
    blocker.authoritative = true;
    result->blockers.push_back(std::move(blocker));
  }
}

void AddSnapshotBlockers(const std::vector<LocalTransactionId>& snapshots,
                         AuthoritativeCleanupHorizonResult* result) {
  for (const LocalTransactionId& snapshot : snapshots) {
    CleanupHorizonBlocker blocker;
    blocker.kind = CleanupHorizonBlockerKind::active_snapshot;
    blocker.local_transaction_id = snapshot;
    blocker.observed_state = "active_snapshot";
    blocker.stable_name = std::string("active_snapshot:") +
                          std::to_string(snapshot.value);
    blocker.authoritative = true;
    result->blockers.push_back(std::move(blocker));
  }
}

void AddAlwaysActiveSessionBlockers(const AuthoritativeCleanupHorizonRequest& request,
                                    AuthoritativeCleanupHorizonResult* result) {
  if (!request.always_in_transaction_policy) {
    return;
  }
  for (const AlwaysActiveSessionTransactionBinding& binding : request.always_active_sessions) {
    const TransactionInventoryEntry* entry = FindEntry(request.inventory,
                                                       binding.current_local_transaction_id);
    CleanupHorizonBlocker blocker;
    blocker.kind = CleanupHorizonBlockerKind::always_active_session;
    blocker.local_transaction_id = binding.current_local_transaction_id;
    blocker.observed_state = entry == nullptr ? "missing" : TransactionStateName(entry->state);
    blocker.stable_name = binding.stable_session_id;
    blocker.authoritative = binding.authoritative;
    result->blockers.push_back(std::move(blocker));
  }
}

}  // namespace

const char* CleanupHorizonBlockerKindName(CleanupHorizonBlockerKind kind) {
  switch (kind) {
    case CleanupHorizonBlockerKind::active_transaction:
      return "active_transaction";
    case CleanupHorizonBlockerKind::active_snapshot:
      return "active_snapshot";
    case CleanupHorizonBlockerKind::unresolved_outcome:
      return "unresolved_outcome";
    case CleanupHorizonBlockerKind::always_active_session:
      return "always_active_session";
    case CleanupHorizonBlockerKind::inventory_authority:
      return "inventory_authority";
    case CleanupHorizonBlockerKind::unknown:
      return "unknown";
  }
  return "unknown";
}

AuthoritativeCleanupHorizonResult ComputeAuthoritativeCleanupHorizon(
    const AuthoritativeCleanupHorizonRequest& request) {
  auto validated = ValidateInventory(request);
  if (!validated.status.ok()) {
    return validated;
  }
  if (!request.active_snapshot_inventory_authoritative) {
    return ServiceError(request,
                        "SB-MGA-CLEANUP-HORIZON-SNAPSHOT-INVENTORY-NOT-AUTHORITATIVE",
                        "transaction.cleanup_horizon.snapshot_inventory_not_authoritative",
                        "active snapshot registry must be authoritative");
  }
  auto sessions = ValidateAlwaysActiveSessions(request);
  if (!sessions.status.ok()) {
    return sessions;
  }

  LocalTransactionHorizonRequest horizon_request;
  horizon_request.inventory = request.inventory;
  horizon_request.active_snapshot_horizons = request.active_snapshot_horizons;
  const auto horizons = ComputeLocalTransactionHorizons(horizon_request);
  if (!horizons.ok()) {
    AuthoritativeCleanupHorizonResult result;
    result.status = horizons.status;
    AddBaseEvidence(&result, request);
    AddEvidence(&result, "fail_closed", "true");
    result.diagnostic = horizons.diagnostic;
    AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
    return result;
  }

  AuthoritativeCleanupHorizonResult result;
  result.status = CleanupHorizonOkStatus();
  result.horizons = horizons.horizons;
  result.cleanup_horizon = MakeLocalTransactionId(
      std::min({horizons.horizons.oldest_interesting_transaction.value,
                horizons.horizons.oldest_active_transaction.value,
                horizons.horizons.oldest_snapshot_transaction.value}));
  AddInventoryBlockers(request.inventory, &result);
  AddSnapshotBlockers(request.active_snapshot_horizons, &result);
  AddAlwaysActiveSessionBlockers(request, &result);

  result.cleanup_horizon_authoritative = true;
  AddBaseEvidence(&result, request);
  AddEvidence(&result, "fail_closed", "false");
  AddEvidence(&result, "oit_local_transaction_id",
              std::to_string(result.horizons.oldest_interesting_transaction.value));
  AddEvidence(&result, "oat_local_transaction_id",
              std::to_string(result.horizons.oldest_active_transaction.value));
  AddEvidence(&result, "ost_local_transaction_id",
              std::to_string(result.horizons.oldest_snapshot_transaction.value));
  AddEvidence(&result, "cleanup_horizon_local_transaction_id",
              std::to_string(result.cleanup_horizon.value));
  AddEvidence(&result, "next_local_transaction_id",
              std::to_string(result.horizons.next_transaction_id.value));
  AddEvidence(&result, "inventory_entry_count",
              std::to_string(request.inventory.entries.size()));
  AddEvidence(&result, "active_snapshot_count",
              std::to_string(request.active_snapshot_horizons.size()));
  AddEvidence(&result, "always_active_session_count",
              std::to_string(request.always_active_sessions.size()));
  AddEvidence(&result, "blocker_count", std::to_string(result.blockers.size()));
  result.diagnostic = MakeCleanupHorizonServiceDiagnostic(
      result.status,
      "SB-MGA-CLEANUP-HORIZON-AUTHORITATIVE",
      "transaction.cleanup_horizon.authoritative",
      std::to_string(result.cleanup_horizon.value));
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}

DiagnosticRecord MakeCleanupHorizonServiceDiagnostic(Status status,
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
                        "transaction.mga.cleanup_horizon");
}

}  // namespace scratchbird::transaction::mga
