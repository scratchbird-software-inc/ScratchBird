// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "transaction_state_code_map.hpp"

namespace scratchbird::storage::page {
using scratchbird::transaction::mga::TransactionState;

TipStateCode DurableTipStateCode(TransactionState state) noexcept {
  switch (state) {
    case TransactionState::none: return TipStateCode::unused;
    case TransactionState::created: return TipStateCode::starting;
    case TransactionState::active: return TipStateCode::active;
    case TransactionState::preparing: return TipStateCode::active;
    case TransactionState::prepared: return TipStateCode::prepared;
    case TransactionState::committing: return TipStateCode::committing;
    case TransactionState::committed: return TipStateCode::committed;
    case TransactionState::rolling_back: return TipStateCode::rolling_back;
    case TransactionState::rolled_back: return TipStateCode::rolled_back;
    case TransactionState::limbo: return TipStateCode::limbo;
    case TransactionState::recovering: return TipStateCode::recovering;
    case TransactionState::failed_terminal: return TipStateCode::dead;
    case TransactionState::archived: return TipStateCode::archived;
    case TransactionState::read_only_active: return TipStateCode::read_only_active;
  }
  return TipStateCode::unused;
}

std::optional<TransactionState> InMemoryTransactionState(TipStateCode code) noexcept {
  switch (code) {
    case TipStateCode::unused: return TransactionState::none;
    case TipStateCode::starting: return TransactionState::created;
    case TipStateCode::active: return TransactionState::active;
    case TipStateCode::read_only_active: return TransactionState::read_only_active;
    case TipStateCode::prepared: return TransactionState::prepared;
    case TipStateCode::committing: return TransactionState::committing;
    case TipStateCode::committed: return TransactionState::committed;
    case TipStateCode::rolling_back: return TransactionState::rolling_back;
    case TipStateCode::rolled_back: return TransactionState::rolled_back;
    case TipStateCode::dead: return TransactionState::failed_terminal;
    case TipStateCode::limbo: return TransactionState::limbo;
    case TipStateCode::recovering: return TransactionState::recovering;
    case TipStateCode::archived: return TransactionState::archived;
  }
  return std::nullopt;
}

}  // namespace scratchbird::storage::page
