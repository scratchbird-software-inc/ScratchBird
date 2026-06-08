// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-CLUSTER-TXN-MAPPING-UNAVAILABLE-ANCHOR
#include "cluster_transaction_fail_closed.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::transaction::mga::ClusterTransactionMetadata;

inline constexpr u32 kClusterTransactionPageBodyHeaderBytes = 96;

struct ClusterTransactionPageBody {
  u64 page_number = 0;
  u64 next_page_number = 0;
  std::vector<ClusterTransactionMetadata> records;
};

struct ClusterTransactionPageBodyResult {
  Status status;
  ClusterTransactionPageBody body;
  std::vector<byte> serialized;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

ClusterTransactionPageBodyResult BuildClusterTransactionPageBodyFailClosed(
    const ClusterTransactionPageBody& body,
    u32 page_size);
ClusterTransactionPageBodyResult ParseClusterTransactionPageBodyFailClosed(
    const std::vector<byte>& serialized,
    u64 page_number);
DiagnosticRecord MakeClusterTransactionPageDiagnostic(Status status,
                                                     std::string diagnostic_code,
                                                     std::string message_key,
                                                     std::string detail = {});

}  // namespace scratchbird::storage::page
