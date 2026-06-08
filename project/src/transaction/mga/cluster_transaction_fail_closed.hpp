// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-CLUSTER-TXN-FAIL-CLOSED-ANCHOR
#include "runtime_platform.hpp"
#include "transaction_state.hpp"

#include <string>

namespace scratchbird::transaction::mga {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u64;

struct ClusterTransactionMetadata {
  TypedUuid transaction_uuid;
  u64 cluster_epoch = 0;
  u64 decision_generation = 0;
  TransactionState state = TransactionState::none;
  bool cluster_authority_active = false;
};

struct ClusterTransactionResult {
  Status status;
  ClusterTransactionMetadata metadata;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

ClusterTransactionResult RejectClusterTransactionUntilMappingAvailable(
    ClusterTransactionMetadata metadata,
    std::string operation_name = {});
ClusterTransactionResult BeginClusterTransactionFailClosed(ClusterTransactionMetadata metadata);
ClusterTransactionResult CommitClusterTransactionFailClosed(ClusterTransactionMetadata metadata);
DiagnosticRecord MakeClusterTransactionDiagnostic(Status status,
                                                  std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail = {});

}  // namespace scratchbird::transaction::mga
