// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-SNTXN-POLICY-ANCHOR
#include "transaction_inventory.hpp"

namespace scratchbird::transaction::mga {

struct TransactionRuntimePolicy {
  u64 max_active_millis = 0;
  u64 max_idle_millis = 0;
  bool fail_closed_on_violation = true;
};

struct TransactionPolicyResult {
  Status status;
  bool allowed = true;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && allowed;
  }
};

TransactionRuntimePolicy DefaultLocalTransactionRuntimePolicy();
TransactionPolicyResult EvaluateTransactionRuntimePolicy(const TransactionInventoryEntry& entry,
                                                         TransactionRuntimePolicy policy,
                                                         u64 now_unix_epoch_millis,
                                                         u64 last_activity_unix_epoch_millis);
DiagnosticRecord MakeTransactionPolicyDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail = {});

}  // namespace scratchbird::transaction::mga
