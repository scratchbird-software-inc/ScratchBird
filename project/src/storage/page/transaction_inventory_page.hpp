// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-TXN-INVENTORY-PAGE-ANCHOR
#include "runtime_platform.hpp"
#include "transaction_horizon.hpp"
#include "transaction_inventory.hpp"

#include <array>
#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::transaction::mga::LocalTransactionHorizons;
using scratchbird::transaction::mga::LocalTransactionInventory;

inline constexpr u32 kTransactionInventoryPageDigestBytes = 32;
inline constexpr u32 kTransactionInventoryPageBodyHeaderBytes = 144;
using TransactionInventoryPageDigest =
    std::array<byte, kTransactionInventoryPageDigestBytes>;

struct TransactionInventoryPageBody {
  u64 page_number = 0;
  u64 previous_page_number = 0;
  u64 next_page_number = 0;
  u64 inventory_generation = 1;
  TransactionInventoryPageDigest chain_digest{};
  LocalTransactionInventory inventory;
  LocalTransactionHorizons horizons;
};

struct TransactionInventoryPageBodyResult {
  Status status;
  TransactionInventoryPageBody body;
  std::vector<byte> serialized;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

u64 ComputeTransactionInventoryPageChecksum(const std::vector<byte>& body);
TransactionInventoryPageDigest ComputeTransactionInventoryPageChecksumDigest(
    const std::vector<byte>& body);
TransactionInventoryPageDigest ComputeTransactionInventoryPageChainDigest(
    const TransactionInventoryPageBody& body);
u64 TransactionInventoryPageDigestLow64(
    const TransactionInventoryPageDigest& digest);
bool TransactionInventoryPageDigestPresent(
    const TransactionInventoryPageDigest& digest);
u32 MaxTransactionInventoryEntriesPerPage(u32 page_size);
TransactionInventoryPageBodyResult BuildTransactionInventoryPageBody(const TransactionInventoryPageBody& body,
                                                                     u32 page_size);
TransactionInventoryPageBodyResult ParseTransactionInventoryPageBody(const std::vector<byte>& serialized,
                                                                     u64 page_number);
DiagnosticRecord MakeTransactionInventoryPageDiagnostic(Status status,
                                                       std::string diagnostic_code,
                                                       std::string message_key,
                                                       std::string detail = {});

}  // namespace scratchbird::storage::page
