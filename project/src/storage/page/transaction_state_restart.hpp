// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "transaction_state_page.hpp"

#include <array>
#include <span>
#include <vector>

namespace scratchbird::storage::page {

// SEARCH_KEY: SB_TRANSACTION_STATE_RESTART_DEAD_MARKING
// Owner rulings 2026-11-05: open-time verification is read-only; after a
// clean verification, the first write at startup marks the transactions a
// crash left in progress as dead, touching only the transaction-state pages
// and running outside any transaction (FirebirdSQL TRA_cleanup). Nothing is
// replayed: the database file is authoritative, and a dead transaction's
// record versions are simply not visible and are later removed by sweep.
//
// This plans that write on in-memory page images. It scans slots from the
// lower of the stored OIT and OAT up to Next and, in place:
//  - marks dead every slot whose code means "in progress and never finished":
//    unused below Next (a crash between allocating the number and writing
//    its slot), starting, active, read_only_active, rolling_back;
//  - leaves prepared and limbo alone (they await resolution);
//  - leaves committing and recovering alone and reports them as unresolved:
//    no current writer stores them, and deciding their outcome needs the
//    transaction header, which this TIP-only pass does not read.
// Writer rule this depends on: the counters written when BEGIN allocates
// transaction n keep OIT <= n and OAT <= n.
// It does not change the counters: like Firebird, the horizons are
// recomputed when the next transaction starts. Each changed 512-byte unit is
// reported once with its preimage; the units are independent of each other.

struct TipChainPage {
  u64 page_number = 0;
  std::span<byte> image;  // a decoded page of the chain, edited in place
};
struct TipUnitWrite {
  u64 page_number = 0;
  u32 unit_offset = 0;
  std::array<byte, kTransactionStateWriteUnitBytes> preimage{};
};
struct TipRestartPlan {
  TipPageError error = TipPageError::invalid_family;
  u64 next_transaction = 0;
  u64 scanned_from = 0;
  u64 marked_dead = 0;
  std::vector<TipUnitWrite> units;    // in chain order, ascending offset
  std::vector<u64> unresolved;        // transaction_orders left committing/recovering
  bool ok() const noexcept { return error == TipPageError::none; }
};

// `chain` is the whole chain in order, first page first, each image a page of
// that chain (sequence i at index i). On a refusal no image is changed.
TipRestartPlan PlanTransactionStateRestart(std::span<const TipChainPage> chain);

}  // namespace scratchbird::storage::page
