// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "transaction_inventory_writer.hpp"

#include <functional>
#include <span>
#include <vector>

namespace scratchbird::storage::page::detail {

// SEARCH_KEY: SB_CAREFUL_CHAIN_SUPPORT
// The careful-write helpers every in-place page chain of the transaction
// inventory family shares: the state chain and the header chain
// (transaction_inventory_writer.cpp) and the session ledger chain
// (session_ledger_chain.cpp, plan 36 T4). They were file-static in the
// transaction writer; the ledger writer uses the same code, not a copy.

// Reads one page of `page_size` through the sink and appends it to `out`. On a
// failure `result` carries invalid_inventory (a bad page number) or io_failure.
bool ReadChainPage(scratchbird::storage::disk::CarefulWriteSink& sink, u32 page_size, u64 number,
                   std::vector<byte>& out, TransactionWriterOutcome& result);

// Adds one edited 512-byte unit of a page image to the plan: the unit's new
// bytes, and its bytes before the edit as the preimage the plan checks on disk.
scratchbird::storage::disk::CarefulWriteAddResult AddChainUnit(scratchbird::storage::disk::CarefulWritePlan& plan,
                                                              u64 page_number, u32 page_size,
                                                              std::span<const byte> image, const TipPageEdit& edit);

// Runs a plan and records its effects in `result` (error, write_error, io,
// writes, syncs). True on success. A failure after a write or sync was
// attempted sets `failed` (the owner's writer is then failed and the database
// must be reopened) and result.error = write_failed.
bool RunChainPlan(scratchbird::storage::disk::CarefulWritePlan& plan, scratchbird::storage::disk::CarefulWriteSink& sink,
                  scratchbird::storage::disk::CarefulWriteBarrierPolicy policy, TransactionWriterOutcome& result,
                  bool& failed, scratchbird::storage::disk::CarefulWriteNodeId* failed_node = nullptr);

// One growth step of an in-place chain (W2c, owner Q22 = 1): allocate a page
// from the allocation map (created by the system: no transaction), format it
// as the chain's next page, then link it from the chain's last page; its own
// plan, each write durable before the next (the Q76 record-first plan runs
// first when the allocation record shares the state's map unit).
struct ChainGrowthSpec {
  Uuid database_uuid;
  scratchbird::storage::disk::FilespaceRootReference allocation_root;  // kind 3
  scratchbird::core::uuid::StandaloneUuidV7Issuer* issuer = nullptr;
  scratchbird::storage::disk::CarefulWriteBarrierPolicy barrier_policy =
      scratchbird::storage::disk::CarefulWriteBarrierPolicy::trust_forced_writes;
  u32 page_size = 0;
  u32 page_type = 0;
  Uuid owner_object;  // the allocation record's owner: the chain's object
  Uuid filespace_uuid, page_size_profile_uuid;
  std::span<byte> last_image;  // the chain's last page, edited by `link`
  u64 last_page_number = 0;
  // Formats the new page from its common header into the image.
  std::function<TipPageError(const scratchbird::storage::disk::NativeCommonPageHeader&, std::span<byte>)> format;
  // Links the new page from the last page's image (the edit is the plan's node).
  std::function<TipPageEdit(std::span<byte>, u64)> link;
};
struct ChainGrowthOutcome : TransactionWriterOutcome {
  u32 growth_writes = 0, growth_syncs = 0;
  AllocationWriteError allocation_error = AllocationWriteError::none;
};
// True on success; on failure `out` carries the refusal exactly as BEGIN's
// growth reports it (invalid_inventory, io_failure, chain_full with
// page_error invalid_slot, resource_exhausted, write_failed).
bool GrowChainByOnePage(scratchbird::storage::disk::CarefulWriteSink& sink, const ChainGrowthSpec& spec,
                        ChainGrowthOutcome& out, bool& failed);

}  // namespace scratchbird::storage::page::detail
