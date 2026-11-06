// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "transaction_header_page.hpp"
#include "transaction_horizon.hpp"
#include "transaction_state_page.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace scratchbird::storage::database::detail {

// SB_COMPACT_TRANSACTION_STATE_PAGE / SB_TRANSACTION_HEADER_PAGE (owner
// rulings 2026-11-05). The genesis transaction inventory that native create
// writes and that create-recovery rebuilds byte for byte: one transaction-state
// page and one transaction header page.
//  - Transaction 1, the construction transaction, is committed, with its
//    header completed at the creation time (UTC milliseconds -> nanoseconds).
//  - Next is the next transaction number (2).
//  - The stored horizons are OIT, OAT and OST as derived from the in-memory
//    inventory; OAR and OLR are Next (nothing to archive, nothing in limbo);
//    OCT is the minimum of OAT, OST, OLR and OAR (inventory spec §12).
// Only the database and transaction identities are bound in the header: the
// subsystems that issue the other identities are not bound to transactions.
struct NativeCreationTransactionInventoryImages {
  std::vector<scratchbird::core::platform::byte> state, headers;
};

inline std::optional<NativeCreationTransactionInventoryImages> EncodeNativeCreationTransactionInventory(
    const scratchbird::storage::disk::NativeCommonPageHeader& state_header,
    const scratchbird::storage::disk::NativeCommonPageHeader& header_page_header,
    const scratchbird::core::platform::Uuid& creator, scratchbird::core::platform::u64 next,
    const scratchbird::transaction::mga::LocalTransactionHorizons& derived,
    scratchbird::core::platform::u64 creation_utc_millis) {
  namespace page = scratchbird::storage::page;
  using scratchbird::core::platform::u64;
  if (creation_utc_millis == 0 || creation_utc_millis > UINT64_MAX / 1'000'000) return std::nullopt;
  NativeCreationTransactionInventoryImages out;
  page::TipPageFormat f;
  f.header = state_header;
  f.tip_sequence = 0;
  f.next_tip_page = 0;
  f.header_chain_page = header_page_header.page_number;
  page::TipCounters c;
  c.next_transaction = next;
  c.horizons.oldest_interesting = derived.oldest_interesting_transaction.value;
  c.horizons.oldest_active = derived.oldest_active_transaction.value;
  c.horizons.oldest_snapshot = derived.oldest_snapshot_transaction.value;
  c.horizons.oldest_archive_required = next;
  c.horizons.oldest_limbo_resolution = next;
  c.horizons.oldest_cleanup = std::min<u64>({c.horizons.oldest_active, c.horizons.oldest_snapshot,
                                             c.horizons.oldest_limbo_resolution, c.horizons.oldest_archive_required});
  f.counters = c;
  out.state.resize(state_header.page_size_bytes);
  if (page::FormatTransactionStatePage(f, out.state) != page::TipPageError::none) return std::nullopt;
  if (!page::SetTransactionStateWord(out.state, 1, {page::TipStateCode::committed, page::kTipFlagHasDetailRecord}).ok())
    return std::nullopt;
  page::TransactionHeaderPageFormat h;
  h.header = header_page_header;
  h.sequence = 0;
  h.next_page = 0;
  out.headers.resize(header_page_header.page_size_bytes);
  if (page::FormatTransactionHeaderPage(h, out.headers) != page::TipPageError::none) return std::nullopt;
  page::TransactionHeaderRecord r;
  r.transaction_order = 1;
  r.database_uuid = state_header.database_uuid;
  r.transaction_uuid = creator;
  if (page::CompleteTransactionHeader(r, page::TipStateCode::committed, creation_utc_millis * 1'000'000) !=
      page::TransactionHeaderError::none)
    return std::nullopt;
  if (!page::WriteTransactionHeader(out.headers, r).ok()) return std::nullopt;
  return out;
}

}  // namespace scratchbird::storage::database::detail
