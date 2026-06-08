// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// DPC_AUTHORITATIVE_CLEANUP_HORIZON_SERVICE
#include "transaction_horizon.hpp"

#include <string>
#include <vector>

namespace scratchbird::transaction::mga {

enum class CleanupHorizonBlockerKind : u16 {
  active_transaction,
  active_snapshot,
  unresolved_outcome,
  always_active_session,
  inventory_authority,
  unknown
};

struct CleanupHorizonEvidenceField {
  std::string key;
  std::string value;
};

struct CleanupHorizonBlocker {
  CleanupHorizonBlockerKind kind = CleanupHorizonBlockerKind::unknown;
  LocalTransactionId local_transaction_id;
  std::string stable_name;
  std::string observed_state;
  bool authoritative = false;
};

struct AlwaysActiveSessionTransactionBinding {
  std::string stable_session_id;
  LocalTransactionId current_local_transaction_id;
  bool authoritative = false;
};

struct AuthoritativeCleanupHorizonRequest {
  LocalTransactionInventory inventory;
  bool inventory_authoritative = false;
  bool inventory_complete = false;
  std::vector<LocalTransactionId> active_snapshot_horizons;
  bool active_snapshot_inventory_authoritative = true;
  bool always_in_transaction_policy = false;
  bool always_active_session_inventory_authoritative = true;
  std::vector<AlwaysActiveSessionTransactionBinding> always_active_sessions;
};

struct AuthoritativeCleanupHorizonResult {
  Status status;
  LocalTransactionHorizons horizons;
  LocalTransactionId cleanup_horizon;
  std::vector<CleanupHorizonBlocker> blockers;
  std::vector<CleanupHorizonEvidenceField> evidence;
  DiagnosticRecord diagnostic;
  bool cleanup_horizon_authoritative = false;

  bool ok() const {
    return status.ok() && cleanup_horizon_authoritative;
  }
};

const char* CleanupHorizonBlockerKindName(CleanupHorizonBlockerKind kind);
AuthoritativeCleanupHorizonResult ComputeAuthoritativeCleanupHorizon(
    const AuthoritativeCleanupHorizonRequest& request);
DiagnosticRecord MakeCleanupHorizonServiceDiagnostic(Status status,
                                                     std::string diagnostic_code,
                                                     std::string message_key,
                                                     std::string detail = {});

}  // namespace scratchbird::transaction::mga
