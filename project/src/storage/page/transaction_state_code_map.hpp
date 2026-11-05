// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "transaction_state.hpp"
#include "transaction_state_page.hpp"

#include <optional>

namespace scratchbird::storage::page {

// SEARCH_KEY: SB_COMPACT_TRANSACTION_STATE_PAGE
// The explicit table between the durable state codes of the transaction-state
// page (inventory spec §7 list) and the in-memory TransactionState. The two
// vocabularies differ:
//  - the durable `starting` is the in-memory `created`;
//  - the durable `dead` is the in-memory `failed_terminal` (lifecycle spec §6
//    calls it dead; the in-memory rename is a separate cleanup);
//  - the in-memory `preparing` has no durable code: it is still in progress,
//    so it is stored as `active` (a crash in either is dead at restart);
//  - `archived` loses its origin in the state word; the origin is the final
//    state recorded in the transaction's header, which the reader combines.

// The durable code to store for an in-memory state. `none` maps to unused,
// which is never written over a used slot.
TipStateCode DurableTipStateCode(scratchbird::transaction::mga::TransactionState state) noexcept;
// The in-memory state for a durable code; nullopt for a code outside the list.
std::optional<scratchbird::transaction::mga::TransactionState> InMemoryTransactionState(TipStateCode code) noexcept;

}  // namespace scratchbird::storage::page
