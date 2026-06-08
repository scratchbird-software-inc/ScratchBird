// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_horizon.hpp"

#include "metric_producer.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace scratchbird::transaction::mga {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status HorizonOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status HorizonErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::transaction_mga};
}

bool IsInteresting(TransactionState state) {
  return state == TransactionState::active || state == TransactionState::read_only_active ||
         state == TransactionState::preparing || state == TransactionState::prepared || state == TransactionState::committing ||
         state == TransactionState::limbo || state == TransactionState::recovering ||
         state == TransactionState::failed_terminal;
}

bool IsActiveForOat(TransactionState state) {
  return state == TransactionState::active || state == TransactionState::read_only_active ||
         state == TransactionState::preparing || state == TransactionState::prepared || state == TransactionState::committing ||
         state == TransactionState::limbo || state == TransactionState::recovering;
}

u64 OldestActiveBeginMillis(const LocalTransactionInventory& inventory) {
  u64 oldest = 0;
  for (const auto& entry : inventory.entries) {
    if (!IsActiveForOat(entry.state) || entry.begin_unix_epoch_millis == 0) {
      continue;
    }
    if (oldest == 0 || entry.begin_unix_epoch_millis < oldest) {
      oldest = entry.begin_unix_epoch_millis;
    }
  }
  return oldest;
}

}  // namespace

TransactionHorizonResult ComputeLocalTransactionHorizons(const LocalTransactionInventory& inventory) {
  LocalTransactionHorizonRequest request;
  request.inventory = inventory;
  return ComputeLocalTransactionHorizons(request);
}

TransactionHorizonResult ComputeLocalTransactionHorizons(const LocalTransactionHorizonRequest& request) {
  const LocalTransactionInventory& inventory = request.inventory;
  TransactionHorizonResult result;
  result.status = HorizonOkStatus();
  result.horizons.next_transaction_id = MakeLocalTransactionId(inventory.next_local_transaction_id);

  u64 oit = inventory.next_local_transaction_id;
  u64 oat = inventory.next_local_transaction_id;
  u64 ost = inventory.next_local_transaction_id;

  for (const TransactionInventoryEntry& entry : inventory.entries) {
    if (!entry.identity.local_id.valid()) {
      result.status = HorizonErrorStatus();
      result.diagnostic = MakeTransactionHorizonDiagnostic(result.status,
                                                           "SB-TXN-HORIZON-INVALID",
                                                           "transaction.horizon.invalid_local_id");
      return result;
    }
    if (IsInteresting(entry.state)) {
      oit = std::min(oit, entry.identity.local_id.value);
    }
    if (IsActiveForOat(entry.state)) {
      oat = std::min(oat, entry.identity.local_id.value);
    }
  }

  for (const LocalTransactionId& snapshot_horizon : request.active_snapshot_horizons) {
    if (!snapshot_horizon.valid()) {
      result.status = HorizonErrorStatus();
      result.diagnostic = MakeTransactionHorizonDiagnostic(result.status,
                                                           "SB-TXN-HORIZON-INVALID",
                                                           "transaction.horizon.invalid_snapshot_horizon");
      return result;
    }
    if (snapshot_horizon.value > inventory.next_local_transaction_id) {
      result.status = HorizonErrorStatus();
      result.diagnostic = MakeTransactionHorizonDiagnostic(result.status,
                                                           "SB-TXN-HORIZON-INVALID",
                                                           "transaction.horizon.future_snapshot_horizon");
      return result;
    }
    ost = std::min(ost, snapshot_horizon.value);
  }

  result.horizons.oldest_interesting_transaction = MakeLocalTransactionId(oit);
  result.horizons.oldest_active_transaction = MakeLocalTransactionId(oat);
  result.horizons.oldest_snapshot_transaction = MakeLocalTransactionId(ost);
  result.horizons.valid = true;
  return result;
}

void PublishTransactionHorizonMetrics(const LocalTransactionInventory& inventory,
                                      const LocalTransactionHorizons& horizons,
                                      u64 observation_unix_epoch_millis) {
  if (!horizons.valid || observation_unix_epoch_millis == 0) {
    return;
  }
  const u64 oldest_begin = OldestActiveBeginMillis(inventory);
  if (oldest_begin == 0 || oldest_begin > observation_unix_epoch_millis) {
    (void)scratchbird::core::metrics::SetGauge(
        "sb_tx_oldest_snapshot_age_microseconds",
        scratchbird::core::metrics::Labels({{"component", "transaction.mga"}, {"reason_class", "no_active_snapshot"}}),
        0.0,
        "transaction_mga");
    return;
  }
  const u64 age_microseconds = (observation_unix_epoch_millis - oldest_begin) * 1000ull;
  (void)scratchbird::core::metrics::SetGauge(
      "sb_tx_oldest_snapshot_age_microseconds",
      scratchbird::core::metrics::Labels({{"component", "transaction.mga"}, {"reason_class", "oldest_active_begin"}}),
      static_cast<double>(age_microseconds),
      "transaction_mga");
}

DiagnosticRecord MakeTransactionHorizonDiagnostic(Status status,
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
                        "transaction.mga.horizon");
}

}  // namespace scratchbird::transaction::mga
