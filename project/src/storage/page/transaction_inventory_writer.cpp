// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "transaction_inventory_writer.hpp"
#include "transaction_inventory_reader.hpp"
#include "transaction_state_restart.hpp"
#include "transaction_horizon_projection.hpp"
#include "native_common_page_header.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace scratchbird::storage::page {
namespace {
namespace disk = scratchbird::storage::disk;
namespace mga = scratchbird::transaction::mga;
using E = TransactionWriterError;
using disk::CarefulWriteError;

// Both chains as read from the file, page images back to back, with each
// page's number.
struct Chains {
  std::vector<byte> state, headers;
  std::vector<u64> state_pages, header_pages;
  TipCounters counters;
};

template <class Result>
Result Refuse(E error, TipPageError page_error = TipPageError::none) {
  Result r;
  r.error = error;
  r.page_error = page_error;
  return r;
}

// Reads one page through the sink into `out` (appended).
bool ReadPage(disk::CarefulWriteSink& sink, u32 page_size, u64 number, std::vector<byte>& out,
              TransactionWriterOutcome& result) {
  if (number == 0 || number > std::numeric_limits<u64>::max() / page_size) {
    result.error = E::invalid_inventory;
    result.page_error = TipPageError::invalid_link;
    return false;
  }
  const auto at = out.size();
  out.resize(at + page_size);
  const auto io = sink.ReadAt(number * page_size, std::span<byte>(out).subspan(at, page_size));
  if (!io.ok() || io.bytes_transferred != page_size) {
    result.error = E::io_failure;
    result.io = io;
    return false;
  }
  return true;
}

// Reads and checks both chains. The first state page is bound to the root's
// identity; the rest of the structure is CheckTransactionInventoryImages.
bool ReadChains(disk::CarefulWriteSink& sink, const TransactionWriterBinding& binding, u32 page_size, Chains& chains,
                TransactionWriterOutcome& result) {
  disk::NativeCommonPageHeaderBinding first;
  first.filespace = {binding.database_uuid, binding.state_root.filespace_uuid,
                     binding.state_root.page_size_profile_uuid};
  first.page_number = binding.state_root.page_number;
  first.page_generation = binding.state_root.page_generation;
  first.page_type = kTransactionStatePageType;
  u64 page = binding.state_root.page_number, header_chain = 0;
  for (std::size_t i = 0; page != 0; ++i) {
    if (i >= binding.maximum_chain_pages) {
      result.error = E::invalid_inventory;
      result.page_error = TipPageError::invalid_link;
      return false;
    }
    if (!ReadPage(sink, page_size, page, chains.state, result)) return false;
    const auto image = std::span<const byte>(chains.state).subspan(i * page_size, page_size);
    const auto d = DecodeTransactionStatePage(image, i == 0 ? &first : nullptr);
    if (!d.ok() || d.view->tip_sequence != i || d.view->header.page_number != page) {
      result.error = E::invalid_inventory;
      result.page_error = d.ok() ? TipPageError::invalid_link : d.error;
      return false;
    }
    chains.state_pages.push_back(page);
    if (i == 0) header_chain = d.view->header_chain_page;
    page = d.view->next_tip_page;
  }
  page = header_chain;
  for (std::size_t i = 0; page != 0; ++i) {
    if (i >= binding.maximum_chain_pages) {
      result.error = E::invalid_inventory;
      result.page_error = TipPageError::invalid_link;
      return false;
    }
    if (!ReadPage(sink, page_size, page, chains.headers, result)) return false;
    const auto image = std::span<const byte>(chains.headers).subspan(i * page_size, page_size);
    const auto d = DecodeTransactionHeaderPage(image);
    if (!d.ok() || d.view->sequence != i || d.view->header.page_number != page) {
      result.error = E::invalid_inventory;
      result.page_error = d.ok() ? TipPageError::invalid_link : d.error;
      return false;
    }
    chains.header_pages.push_back(page);
    page = d.view->next_page;
  }
  const auto check = CheckTransactionInventoryImages(page_size, chains.state, chains.headers);
  if (!check.ok()) {
    result.error = E::invalid_inventory;
    result.page_error = check.error;
    result.transaction_order = check.transaction_order;
    return false;
  }
  chains.counters = check.counters;
  return true;
}

std::span<byte> PageOf(std::vector<byte>& images, u32 page_size, std::size_t index) {
  return std::span<byte>(images).subspan(index * page_size, page_size);
}

// Adds one edited unit to the plan: the unit's new bytes, and its bytes
// before the edit as the preimage the plan checks on disk before writing.
disk::CarefulWriteAddResult AddUnit(disk::CarefulWritePlan& plan, u64 page_number, u32 page_size,
                                    std::span<const byte> image, const TipPageEdit& edit) {
  return plan.Add(page_number * page_size + edit.unit_offset,
                  image.subspan(edit.unit_offset, kTransactionStateWriteUnitBytes),
                  std::span<const byte>(edit.preimage));
}

// Runs a plan and records its effects. Returns true on success. A failure
// after a write or sync was attempted fails the writer.
bool Run(disk::CarefulWritePlan& plan, disk::CarefulWriteSink& sink, disk::CarefulWriteBarrierPolicy policy,
         TransactionWriterOutcome& result, bool& failed,
         disk::CarefulWriteNodeId* failed_node = nullptr) {
  const auto r = plan.Execute(sink, policy);
  if (failed_node) *failed_node = r.failed_node;
  result.write_error = r.error;
  result.io = r.io;
  result.writes = r.written;
  result.syncs = r.syncs;
  if (r.ok()) {
    result.error = E::none;
    return true;
  }
  const bool attempted = r.written > 0 || r.syncs > 0 || r.error == CarefulWriteError::write_failed ||
                         r.error == CarefulWriteError::barrier_sync_failed ||
                         r.error == CarefulWriteError::final_sync_failed ||
                         r.error == CarefulWriteError::sink_exception;
  if (attempted) {
    failed = true;
    result.error = E::write_failed;
  } else if (r.error == CarefulWriteError::preimage_read_failed) {
    result.error = E::io_failure;
  } else if (r.error == CarefulWriteError::preimage_mismatch) {
    result.error = E::invalid_inventory;  // the file changed after it was read
  } else {
    result.error = E::resource_exhausted;
  }
  return false;
}

bool InProgress(TipStateCode code) {
  return code == TipStateCode::active || code == TipStateCode::read_only_active;
}
}  // namespace

const char* TransactionWriterErrorName(TransactionWriterError error) noexcept {
  switch (error) {
    case E::none: return "none";
    case E::invalid_request: return "invalid_request";
    case E::read_only_database: return "read_only_database";
    case E::restart_required: return "restart_required";
    case E::restart_already_run: return "restart_already_run";
    case E::writer_failed: return "writer_failed";
    case E::io_failure: return "io_failure";
    case E::invalid_inventory: return "invalid_inventory";
    case E::chain_full: return "chain_full";
    case E::horizon_regression: return "horizon_regression";
    case E::unknown_transaction: return "unknown_transaction";
    case E::invalid_state: return "invalid_state";
    case E::invalid_header: return "invalid_header";
    case E::write_failed: return "write_failed";
    case E::resource_exhausted: return "resource_exhausted";
  }
  return "unknown";
}

TransactionInventoryWriter::TransactionInventoryWriter(const TransactionWriterBinding& binding) noexcept
    : binding_(binding) {
  const auto* profile = disk::FindCanonicalFilespacePageProfile(binding.state_root.page_size_profile_uuid);
  if (profile && binding.state_root.kind == 4 && binding.state_root.page_type == kTransactionStatePageType &&
      binding.state_root.page_number != 0 && binding.state_root.page_generation != 0 &&
      !binding.database_uuid.is_nil() && binding.maximum_chain_pages != 0 &&
      TipSlotsPerPage(profile->page_size_bytes) != 0 && TransactionHeaderSlotsPerPage(profile->page_size_bytes) != 0)
    page_size_ = profile->page_size_bytes;
}

TransactionRestartResult TransactionInventoryWriter::Restart(disk::CarefulWriteSink& sink) noexcept {
  using R = TransactionRestartResult;
  if (!page_size_) return Refuse<R>(E::invalid_request);
  if (failed_) return Refuse<R>(E::writer_failed);
  if (restarted_) return Refuse<R>(E::restart_already_run);
  try {
    R result;
    Chains chains;
    if (!ReadChains(sink, binding_, page_size_, chains, result)) return result;
    result.next_transaction = chains.counters.next_transaction;
    if (binding_.read_only_database) {
      // FirebirdSQL: a read-only database gets no transaction-state write.
      restarted_ = true;
      result.read_only_skipped = true;
      result.error = E::none;
      return result;
    }
    std::vector<TipChainPage> pages;
    for (std::size_t i = 0; i < chains.state_pages.size(); ++i)
      pages.push_back({chains.state_pages[i], PageOf(chains.state, page_size_, i)});
    const auto planned = PlanTransactionStateRestart(pages);
    if (!planned.ok()) return Refuse<R>(E::invalid_inventory, planned.error);
    result.scanned_from = planned.scanned_from;
    result.marked_dead = planned.marked_dead;
    result.unresolved = planned.unresolved;
    if (planned.units.empty()) {
      restarted_ = true;
      result.error = E::none;
      return result;
    }
    // The units are independent of one another: no ordering between them,
    // one durability barrier after the last.
    disk::CarefulWritePlan plan;
    for (const auto& unit : planned.units) {
      const auto index = static_cast<std::size_t>(
          std::find(chains.state_pages.begin(), chains.state_pages.end(), unit.page_number) -
          chains.state_pages.begin());
      const auto image = PageOf(chains.state, page_size_, index);
      if (!plan.Add(unit.page_number * page_size_ + unit.unit_offset,
                    image.subspan(unit.unit_offset, kTransactionStateWriteUnitBytes),
                    std::span<const byte>(unit.preimage))
               .ok())
        return Refuse<R>(E::resource_exhausted);
    }
    if (Run(plan, sink, binding_.barrier_policy, result, failed_)) restarted_ = true;
    return result;
  } catch (const std::bad_alloc&) {
    return Refuse<R>(E::resource_exhausted);
  } catch (const std::length_error&) {
    return Refuse<R>(E::resource_exhausted);
  }
}

TransactionBeginResult TransactionInventoryWriter::Begin(disk::CarefulWriteSink& sink,
                                                         const TransactionHeaderRecord& header) noexcept {
  using R = TransactionBeginResult;
  if (!page_size_) return Refuse<R>(E::invalid_request);
  if (failed_) return Refuse<R>(E::writer_failed);
  if (binding_.read_only_database) return Refuse<R>(E::read_only_database);
  if (!restarted_) return Refuse<R>(E::restart_required);
  if (header.authority != TipAuthorityClass::local || header.transaction_order != 0 ||
      header.final_state != TipStateCode::unused || header.commit_time_utc_ns != 0 ||
      header.completed_at_utc_ns != 0 || header.intent_generation != 0 ||
      !(header.database_uuid.is_nil() || header.database_uuid == binding_.database_uuid))
    return Refuse<R>(E::invalid_request);
  try {
    R result;
    Chains chains;
    if (!ReadChains(sink, binding_, page_size_, chains, result)) return result;
    const u64 n = chains.counters.next_transaction;
    result.transaction_order = n;
    if (n == std::numeric_limits<u64>::max()) return Refuse<R>(E::chain_full, TipPageError::invalid_counters);
    // Capacity: n needs a formatted state slot and header slot.
    const auto state_slot = LocateTransactionStateSlot(page_size_, n);
    const auto header_slot = LocateTransactionHeaderSlot(page_size_, n);
    if (!state_slot || !header_slot || state_slot->tip_sequence >= chains.state_pages.size() ||
        header_slot->sequence >= chains.header_pages.size()) {
      auto refused = Refuse<R>(E::chain_full, TipPageError::invalid_slot);
      refused.transaction_order = n;
      return refused;
    }
    // The record, validated before anything is planned.
    TransactionHeaderRecord record = header;
    record.transaction_order = n;
    record.database_uuid = binding_.database_uuid;
    if (const auto valid = ValidateTransactionHeaderRecord(record); valid != TransactionHeaderError::none) {
      auto refused = Refuse<R>(E::invalid_header);
      refused.header_error = valid;
      refused.transaction_order = n;
      return refused;
    }
    // Horizons over the inventory before n exists (its Next is n).
    const auto built = BuildLocalTransactionInventoryFromImages(page_size_, chains.state, chains.headers);
    if (!built.ok()) return Refuse<R>(E::invalid_inventory, built.error);
    mga::LocalTransactionHorizons projected;
    if (mga::detail::ProjectValidatedLocalHorizons(built.inventory, {}, projected))
      return Refuse<R>(E::invalid_inventory, TipPageError::invalid_counters);
    TipCounters counters;
    counters.next_transaction = n + 1;
    auto& h = counters.horizons;
    h.oldest_interesting = projected.oldest_interesting_transaction.value;
    h.oldest_active = projected.oldest_active_transaction.value;
    h.oldest_snapshot = projected.oldest_snapshot_transaction.value;
    h.oldest_archive_required = n + 1;
    h.oldest_limbo_resolution = n + 1;
    for (const auto& entry : built.inventory.entries)
      if (entry.state == mga::TransactionState::prepared || entry.state == mga::TransactionState::limbo)
        h.oldest_limbo_resolution = std::min(h.oldest_limbo_resolution, entry.identity.local_id.value);
    h.oldest_cleanup = std::min({h.oldest_active, h.oldest_snapshot, h.oldest_limbo_resolution,
                                 h.oldest_archive_required});
    const auto& stored = chains.counters.horizons;
    if (h.oldest_interesting < stored.oldest_interesting || h.oldest_active < stored.oldest_active ||
        h.oldest_snapshot < stored.oldest_snapshot || h.oldest_cleanup < stored.oldest_cleanup ||
        h.oldest_archive_required < stored.oldest_archive_required ||
        h.oldest_limbo_resolution < stored.oldest_limbo_resolution ||
        h.oldest_interesting > n || h.oldest_active > n || h.oldest_snapshot > n) {
      auto refused = Refuse<R>(E::horizon_regression);
      refused.transaction_order = n;
      return refused;
    }
    // The three edits, each one unit of a page image held here.
    auto first = PageOf(chains.state, page_size_, 0);
    const auto counters_edit = SetTransactionStateCounters(first, counters);
    auto header_image = PageOf(chains.headers, page_size_, static_cast<std::size_t>(header_slot->sequence));
    const auto header_edit = WriteTransactionHeader(header_image, record);
    auto state_image = PageOf(chains.state, page_size_, static_cast<std::size_t>(state_slot->tip_sequence));
    TipStateWord word;
    word.state = record.read_only ? TipStateCode::read_only_active : TipStateCode::active;
    word.flags = kTipFlagHasDetailRecord;
    word.authority = TipAuthorityClass::local;
    word.intent = record.intent;
    const auto word_edit = SetTransactionStateWord(state_image, state_slot->slot, word);
    if (!counters_edit.ok() || !header_edit.ok() || !word_edit.ok() || !counters_edit.changed ||
        !header_edit.changed || !word_edit.changed) {
      auto refused = Refuse<R>(E::invalid_inventory, !counters_edit.ok() ? counters_edit.error
                                                     : !header_edit.ok() ? header_edit.error
                                                     : !word_edit.ok()   ? word_edit.error
                                                                         : TipPageError::invalid_slot);
      refused.transaction_order = n;
      return refused;
    }
    disk::CarefulWritePlan plan;
    const auto a = AddUnit(plan, chains.state_pages[0], page_size_, first, counters_edit);
    const auto b = AddUnit(plan, chains.header_pages[header_slot->sequence], page_size_, header_image, header_edit);
    const auto c =
        AddUnit(plan, chains.state_pages[state_slot->tip_sequence], page_size_, state_image, word_edit);
    if (!a.ok() || !b.ok() || !c.ok() || plan.Before(a.node, b.node) != CarefulWriteError::none ||
        plan.Before(b.node, c.node) != CarefulWriteError::none)
      return Refuse<R>(E::resource_exhausted);
    result.counters = counters;
    Run(plan, sink, binding_.barrier_policy, result, failed_);
    return result;
  } catch (const std::bad_alloc&) {
    return Refuse<R>(E::resource_exhausted);
  } catch (const std::length_error&) {
    return Refuse<R>(E::resource_exhausted);
  }
}

TransactionCompleteResult TransactionInventoryWriter::Commit(disk::CarefulWriteSink& sink, u64 transaction_order,
                                                             u64 now_utc_ns) noexcept {
  return Complete(sink, transaction_order, now_utc_ns, TipStateCode::committed);
}

TransactionCompleteResult TransactionInventoryWriter::Rollback(disk::CarefulWriteSink& sink, u64 transaction_order,
                                                               u64 now_utc_ns) noexcept {
  return Complete(sink, transaction_order, now_utc_ns, TipStateCode::rolled_back);
}

TransactionCompleteResult TransactionInventoryWriter::Complete(disk::CarefulWriteSink& sink, u64 n, u64 now_utc_ns,
                                                               TipStateCode final_state) noexcept {
  using R = TransactionCompleteResult;
  if (!page_size_) return Refuse<R>(E::invalid_request);
  if (failed_) return Refuse<R>(E::writer_failed);
  if (binding_.read_only_database) return Refuse<R>(E::read_only_database);
  if (!restarted_) return Refuse<R>(E::restart_required);
  if (n == 0 || now_utc_ns == 0) return Refuse<R>(E::invalid_request);
  try {
    R result;
    result.transaction_order = n;
    result.final_state = final_state;
    Chains chains;
    if (!ReadChains(sink, binding_, page_size_, chains, result)) return result;
    auto refuse = [&](E error) {
      auto refused = Refuse<R>(error);
      refused.transaction_order = n;
      refused.final_state = final_state;
      return refused;
    };
    if (n >= chains.counters.next_transaction) return refuse(E::unknown_transaction);
    const auto state_slot = LocateTransactionStateSlot(page_size_, n);
    const auto header_slot = LocateTransactionHeaderSlot(page_size_, n);
    if (!state_slot || !header_slot || state_slot->tip_sequence >= chains.state_pages.size() ||
        header_slot->sequence >= chains.header_pages.size())
      return refuse(E::unknown_transaction);
    auto state_image = PageOf(chains.state, page_size_, static_cast<std::size_t>(state_slot->tip_sequence));
    auto header_image = PageOf(chains.headers, page_size_, static_cast<std::size_t>(header_slot->sequence));
    const auto state_view = DecodeTransactionStatePage(state_image);
    const auto header_view = DecodeTransactionHeaderPage(header_image);
    if (!state_view.ok() || !header_view.ok()) return refuse(E::invalid_inventory);
    const auto word = TipWordAt(*state_view.view, state_slot->slot);
    if (!word || word->state == TipStateCode::unused) return refuse(E::unknown_transaction);
    if (!InProgress(word->state)) return refuse(E::invalid_state);
    auto record = TransactionHeaderAt(*header_view.view, header_slot->slot);
    if (!record) return refuse(E::invalid_inventory);  // an in-progress word always has its header
    if (record->final_state != TipStateCode::unused) return refuse(E::invalid_state);
    // The completion time: never before the transaction began; on commit,
    // also after every commit time already stored.
    const auto begun = scratchbird::core::uuid::ExtractUuidV7TimePrefix(record->transaction_uuid);
    if (!begun.ok || begun.unix_epoch_millis > std::numeric_limits<u64>::max() / 1'000'000)
      return refuse(E::invalid_inventory);
    u64 at = std::max<u64>(now_utc_ns, begun.unix_epoch_millis * 1'000'000);
    if (final_state == TipStateCode::committed) {
      u64 latest = 0;
      for (std::size_t i = 0; i < chains.header_pages.size(); ++i) {
        const auto view = DecodeTransactionHeaderPage(PageOf(chains.headers, page_size_, i));
        if (!view.ok()) return refuse(E::invalid_inventory);
        for (u32 slot = 0; slot < view.view->slots; ++slot)
          if (const auto other = TransactionHeaderAt(*view.view, slot))
            latest = std::max(latest, other->commit_time_utc_ns);
      }
      if (latest == std::numeric_limits<u64>::max()) return refuse(E::invalid_inventory);
      at = std::max(at, latest + 1);
    }
    if (const auto completed = CompleteTransactionHeader(*record, final_state, at);
        completed != TransactionHeaderError::none) {
      auto refused = refuse(E::invalid_header);
      refused.header_error = completed;
      return refused;
    }
    const auto header_edit = UpdateTransactionHeader(header_image, *record);
    const auto word_edit = SetTransactionStateCode(state_image, state_slot->slot, final_state);
    if (!header_edit.ok() || !word_edit.ok() || !header_edit.changed || !word_edit.changed)
      return refuse(E::invalid_inventory);
    disk::CarefulWritePlan plan;
    const auto a = AddUnit(plan, chains.header_pages[header_slot->sequence], page_size_, header_image, header_edit);
    const auto b =
        AddUnit(plan, chains.state_pages[state_slot->tip_sequence], page_size_, state_image, word_edit);
    if (!a.ok() || !b.ok() || plan.Before(a.node, b.node) != CarefulWriteError::none)
      return refuse(E::resource_exhausted);
    result.completed_at_utc_ns = at;
    disk::CarefulWriteNodeId stopped = disk::kCarefulWriteNoNode;
    if (!Run(plan, sink, binding_.barrier_policy, result, failed_, &stopped)) {
      // Uncertain only once the state byte's write was attempted: a failed
      // write of it (which may have reached the file) or a failed final sync
      // after it was written.
      result.outcome_uncertain =
          result.write_error == CarefulWriteError::final_sync_failed ||
          (stopped == b.node && (result.write_error == CarefulWriteError::write_failed ||
                                 result.write_error == CarefulWriteError::sink_exception));
    }
    return result;
  } catch (const std::bad_alloc&) {
    return Refuse<R>(E::resource_exhausted);
  } catch (const std::length_error&) {
    return Refuse<R>(E::resource_exhausted);
  }
}

}  // namespace scratchbird::storage::page
