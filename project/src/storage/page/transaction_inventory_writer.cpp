// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "transaction_inventory_writer.hpp"
#include "careful_chain_support.hpp"
#include "session_ledger_chain.hpp"
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
    if (!detail::ReadChainPage(sink, page_size, page, chains.state, result)) return false;
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
    if (!detail::ReadChainPage(sink, page_size, page, chains.headers, result)) return false;
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

// One growth step (W2c): the shared careful-write growth of an in-place chain
// (careful_chain_support.cpp, also the session ledger's), told how to format a
// state or header page and how to link it from the chain's last page. On
// failure result carries the refusal.
bool GrowChain(disk::CarefulWriteSink& sink, const TransactionWriterBinding& binding, u32 page_size, Chains& chains,
               bool state, TransactionBeginResult& result, bool& failed) {
  auto& images = state ? chains.state : chains.headers;
  const auto& numbers = state ? chains.state_pages : chains.header_pages;
  detail::ChainGrowthSpec spec;
  spec.database_uuid = binding.database_uuid;
  spec.allocation_root = *binding.allocation_root;
  spec.issuer = binding.issuer;
  spec.barrier_policy = binding.barrier_policy;
  spec.page_size = page_size;
  spec.page_type = kTransactionStatePageType;
  spec.owner_object = binding.state_root.object_uuid;
  spec.filespace_uuid = binding.state_root.filespace_uuid;
  spec.page_size_profile_uuid = binding.state_root.page_size_profile_uuid;
  spec.last_image = PageOf(images, page_size, numbers.size() - 1);
  spec.last_page_number = numbers.back();
  const u64 sequence = numbers.size();
  spec.format = [&](const disk::NativeCommonPageHeader& header, std::span<byte> image) {
    if (state) {
      TipPageFormat f;
      f.header = header;
      f.tip_sequence = sequence;
      return FormatTransactionStatePage(f, image);
    }
    TransactionHeaderPageFormat f;
    f.header = header;
    f.sequence = sequence;
    return FormatTransactionHeaderPage(f, image);
  };
  spec.link = [&](std::span<byte> last, u64 next_page) {
    return state ? SetTransactionStateNextPage(last, next_page) : SetTransactionHeaderNextPage(last, next_page);
  };
  detail::ChainGrowthOutcome grown;
  const bool ok = detail::GrowChainByOnePage(sink, spec, grown, failed);
  result.growth_writes += grown.growth_writes;
  result.growth_syncs += grown.growth_syncs;
  if (!ok) {
    result.error = grown.error;
    result.page_error = grown.page_error;
    result.allocation_error = grown.allocation_error;
    result.write_error = grown.write_error;
    result.io = grown.io;
    return false;
  }
  ++(state ? result.grown_state_pages : result.grown_header_pages);
  return true;
}

// Chain growth (W2c): grow each chain that lacks the slot of n (= Next) by its
// next page, re-reading the chains after each step. Only the next page: a slot
// further away would mean a Next the chains never covered. Then the capacity
// check: n needs a formatted state slot and header slot. Returns nullopt when
// both exist, else the refusal (BEGIN and CompleteAndBegin share it).
std::optional<TransactionBeginResult> GrowForNext(disk::CarefulWriteSink& sink, const TransactionWriterBinding& binding,
                                                  u32 page_size, Chains& chains, u64 n, TransactionBeginResult& result,
                                                  bool& failed) {
  using R = TransactionBeginResult;
  for (int round = 0; round < 2; ++round) {
    const auto state_need = LocateTransactionStateSlot(page_size, n);
    const auto header_need = LocateTransactionHeaderSlot(page_size, n);
    if (!state_need || !header_need) break;
    const bool grow_state = state_need->tip_sequence == chains.state_pages.size();
    const bool grow_header = header_need->sequence == chains.header_pages.size();
    if ((!grow_state && !grow_header) || !binding.allocation_root || !binding.issuer) break;
    if (!GrowChain(sink, binding, page_size, chains, grow_state, result, failed)) return result;
    chains = Chains{};
    if (!ReadChains(sink, binding, page_size, chains, result)) return result;
    if (chains.counters.next_transaction != n) return Refuse<R>(E::invalid_inventory, TipPageError::invalid_counters);
  }
  const auto state_slot = LocateTransactionStateSlot(page_size, n);
  const auto header_slot = LocateTransactionHeaderSlot(page_size, n);
  if (!state_slot || !header_slot || state_slot->tip_sequence >= chains.state_pages.size() ||
      header_slot->sequence >= chains.header_pages.size()) {
    auto refused = Refuse<R>(E::chain_full, TipPageError::invalid_slot);
    refused.transaction_order = n;
    refused.grown_state_pages = result.grown_state_pages;
    refused.grown_header_pages = result.grown_header_pages;
    return refused;
  }
  return std::nullopt;
}

// The counters BEGIN of `n` stores (n = Next): Next := n + 1 and the six
// horizons projected over the inventory as the images stand (retained
// snapshots: none; PLAN36_RETAINING attaches here), each of OIT, OAT, OST at
// most n, none below the stored one. Returns nullopt on success, else the
// refusal.
std::optional<TransactionBeginResult> BuildBeginCounters(u32 page_size, const Chains& chains, u64 n,
                                                         TipCounters& counters) {
  using R = TransactionBeginResult;
  // NEW (slow-cost2): `chains` is the output of a successful ReadChains (CheckTransactionInventoryImages passed on exactly these
  // images, and neither Begin nor CompleteAndBegin edits them before this call: a growth re-reads them, which checks again), so the
  // build skips its own check. OLD: BuildLocalTransactionInventoryFromImages, which ran the check twice more per Begin.
  const auto built = BuildLocalTransactionInventoryFromCheckedImages(page_size, chains.state, chains.headers,
                                                                     chains.counters.next_transaction);
  if (!built.ok()) {
    auto refused = Refuse<R>(E::invalid_inventory, built.error);
    refused.transaction_order = built.transaction_order;  // the entry that could not be built
    return refused;
  }
  mga::LocalTransactionHorizons projected;
  if (mga::detail::ProjectValidatedLocalHorizons(built.inventory, {}, projected))
    return Refuse<R>(E::invalid_inventory, TipPageError::invalid_counters);
  counters = TipCounters{};
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
  return std::nullopt;
}

bool InProgress(TipStateCode code) {
  return code == TipStateCode::active || code == TipStateCode::read_only_active;
}
// A transaction in progress, as the file shows it: its two slots, its header
// record, and the completion time it would get.
struct ActiveTransaction {
  TipSlotLocation state_slot;
  TransactionHeaderSlotLocation header_slot;
  TransactionHeaderRecord record;
};

// Finds n in the chains and checks that it is in progress with an
// unfinished header. Returns none or the refusal code (unknown_transaction,
// invalid_state, invalid_inventory). Reads only.
E FindActiveTransaction(const Chains& chains, u32 page_size, u64 n, ActiveTransaction& out) {
  if (n >= chains.counters.next_transaction) return E::unknown_transaction;
  const auto state_slot = LocateTransactionStateSlot(page_size, n);
  const auto header_slot = LocateTransactionHeaderSlot(page_size, n);
  if (!state_slot || !header_slot || state_slot->tip_sequence >= chains.state_pages.size() ||
      header_slot->sequence >= chains.header_pages.size())
    return E::unknown_transaction;
  const auto state_image = std::span<const byte>(chains.state).subspan(
      static_cast<std::size_t>(state_slot->tip_sequence) * page_size, page_size);
  const auto header_image = std::span<const byte>(chains.headers).subspan(
      static_cast<std::size_t>(header_slot->sequence) * page_size, page_size);
  const auto state_view = DecodeTransactionStatePage(state_image);
  const auto header_view = DecodeTransactionHeaderPage(header_image);
  if (!state_view.ok() || !header_view.ok()) return E::invalid_inventory;
  const auto word = TipWordAt(*state_view.view, state_slot->slot);
  if (!word || word->state == TipStateCode::unused) return E::unknown_transaction;
  if (!InProgress(word->state)) return E::invalid_state;
  auto record = TransactionHeaderAt(*header_view.view, header_slot->slot);
  if (!record) return E::invalid_inventory;  // an in-progress word always has its header
  if (record->final_state != TipStateCode::unused) return E::invalid_state;
  out.state_slot = *state_slot;
  out.header_slot = *header_slot;
  out.record = *record;
  return E::none;
}

// The completion time and the completed header record: never before the
// transaction began; on commit, also after every commit time already stored.
// Returns none or invalid_inventory / invalid_header (header_error set).
E CompleteRecord(const Chains& chains, u32 page_size, u64 now_utc_ns, TipStateCode final_state,
                 TransactionHeaderRecord& record, u64& at, TransactionHeaderError& header_error) {
  const auto begun = scratchbird::core::uuid::ExtractUuidV7TimePrefix(record.transaction_uuid);
  if (!begun.ok || begun.unix_epoch_millis > std::numeric_limits<u64>::max() / 1'000'000)
    return E::invalid_inventory;
  at = std::max<u64>(now_utc_ns, begun.unix_epoch_millis * 1'000'000);
  if (final_state == TipStateCode::committed) {
    u64 latest = 0;
    for (std::size_t i = 0; i < chains.header_pages.size(); ++i) {
      const auto view = DecodeTransactionHeaderPage(
          std::span<const byte>(chains.headers).subspan(i * page_size, page_size));
      if (!view.ok()) return E::invalid_inventory;
      for (u32 slot = 0; slot < view.view->slots; ++slot)
        if (const auto other = TransactionHeaderAt(*view.view, slot))
          latest = std::max(latest, other->commit_time_utc_ns);
    }
    if (latest == std::numeric_limits<u64>::max()) return E::invalid_inventory;
    at = std::max(at, latest + 1);
  }
  header_error = CompleteTransactionHeader(record, final_state, at);
  return header_error == TransactionHeaderError::none ? E::none : E::invalid_header;
}

// SB_SESSION_LEDGER, durable before cited (plan 36 T4): the citations of a
// header (+80 attachment, +96 session; the session's principal must be +64),
// against the ledger bound to the writer. none when the header cites nothing.
// Reads only; every caller runs it before its first write.
E CheckLedgerCitations(const TransactionWriterBinding& binding, const TransactionHeaderRecord& header) {
  if (header.attachment_uuid.is_nil() && header.session_uuid.is_nil()) return E::none;
  if (!binding.session_ledger) return E::ledger_unavailable;
  return binding.session_ledger->CheckHeaderCitation(header.attachment_uuid, header.session_uuid,
                                                     header.principal_or_agent_uuid);
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
    case E::unresolved_transactions: return "unresolved_transactions";
    case E::ledger_unavailable: return "ledger_unavailable";
    case E::ledger_unknown_attachment: return "ledger_unknown_attachment";
    case E::ledger_unknown_session: return "ledger_unknown_session";
    case E::ledger_wrong_kind: return "ledger_wrong_kind";
    case E::ledger_principal_mismatch: return "ledger_principal_mismatch";
    case E::ledger_session_ended: return "ledger_session_ended";
    case E::ledger_attachment_session_mismatch: return "ledger_attachment_session_mismatch";
    case E::ledger_invalid: return "ledger_invalid";
    case E::ledger_full: return "ledger_full";
    case E::ledger_duplicate_record: return "ledger_duplicate_record";
    case E::ledger_unknown_connection: return "ledger_unknown_connection";
    case E::ledger_unknown_predecessor: return "ledger_unknown_predecessor";
    case E::ledger_predecessor_mismatch: return "ledger_predecessor_mismatch";
    case E::ledger_generation_order: return "ledger_generation_order";
    case E::ledger_invalid_record: return "ledger_invalid_record";
    case E::ledger_not_loaded: return "ledger_not_loaded";
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
    if (binding_.refuse_restart_with_unresolved && !planned.unresolved.empty()) {
      // Q241 = 1: nothing is written; the planner worked on copies, so the
      // file and the writer (not restarted, not failed) are as they were.
      result.marked_dead = 0;  // the planned count was not applied
      result.error = E::unresolved_transactions;
      return result;
    }
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
    if (detail::RunChainPlan(plan, sink, binding_.barrier_policy, result, failed_)) restarted_ = true;
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
    // Durable before cited: the header is validated (with a provisional order,
    // the codec's only order rule is non-zero) and its citations are checked
    // BEFORE anything is read for a write, so chain growth cannot run first.
    {
      TransactionHeaderRecord probe = header;
      probe.transaction_order = 1;
      probe.database_uuid = binding_.database_uuid;
      if (const auto valid = ValidateTransactionHeaderRecord(probe); valid != TransactionHeaderError::none) {
        auto refused = Refuse<R>(E::invalid_header);
        refused.header_error = valid;
        return refused;
      }
      if (const auto cited = CheckLedgerCitations(binding_, header); cited != E::none) return Refuse<R>(cited);
    }
    R result;
    Chains chains;
    if (!ReadChains(sink, binding_, page_size_, chains, result)) return result;
    const u64 n = chains.counters.next_transaction;
    result.transaction_order = n;
    if (n == std::numeric_limits<u64>::max()) return Refuse<R>(E::chain_full, TipPageError::invalid_counters);
    if (auto refused = GrowForNext(sink, binding_, page_size_, chains, n, result, failed_)) return *refused;
    const auto state_slot = LocateTransactionStateSlot(page_size_, n);
    const auto header_slot = LocateTransactionHeaderSlot(page_size_, n);
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
    TipCounters counters;
    if (auto refused = BuildBeginCounters(page_size_, chains, n, counters)) return *refused;
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
    const auto a = detail::AddChainUnit(plan, chains.state_pages[0], page_size_, first, counters_edit);
    const auto b = detail::AddChainUnit(plan, chains.header_pages[header_slot->sequence], page_size_, header_image, header_edit);
    const auto c =
        detail::AddChainUnit(plan, chains.state_pages[state_slot->tip_sequence], page_size_, state_image, word_edit);
    if (!a.ok() || !b.ok() || !c.ok() || plan.Before(a.node, b.node) != CarefulWriteError::none ||
        plan.Before(b.node, c.node) != CarefulWriteError::none)
      return Refuse<R>(E::resource_exhausted);
    result.counters = counters;
    detail::RunChainPlan(plan, sink, binding_.barrier_policy, result, failed_);
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
    ActiveTransaction active;
    if (const auto found = FindActiveTransaction(chains, page_size_, n, active); found != E::none)
      return refuse(found);
    const auto state_slot = active.state_slot;
    const auto header_slot = active.header_slot;
    auto state_image = PageOf(chains.state, page_size_, static_cast<std::size_t>(state_slot.tip_sequence));
    auto header_image = PageOf(chains.headers, page_size_, static_cast<std::size_t>(header_slot.sequence));
    auto record = &active.record;
    u64 at = 0;
    TransactionHeaderError completed = TransactionHeaderError::none;
    if (const auto done = CompleteRecord(chains, page_size_, now_utc_ns, final_state, *record, at, completed);
        done != E::none) {
      auto refused = refuse(done);
      if (done == E::invalid_header) refused.header_error = completed;
      return refused;
    }
    const auto header_edit = UpdateTransactionHeader(header_image, *record);
    const auto word_edit = SetTransactionStateCode(state_image, state_slot.slot, final_state);
    if (!header_edit.ok() || !word_edit.ok() || !header_edit.changed || !word_edit.changed)
      return refuse(E::invalid_inventory);
    disk::CarefulWritePlan plan;
    const auto a = detail::AddChainUnit(plan, chains.header_pages[header_slot.sequence], page_size_, header_image, header_edit);
    const auto b =
        detail::AddChainUnit(plan, chains.state_pages[state_slot.tip_sequence], page_size_, state_image, word_edit);
    if (!a.ok() || !b.ok() || plan.Before(a.node, b.node) != CarefulWriteError::none)
      return refuse(E::resource_exhausted);
    result.completed_at_utc_ns = at;
    disk::CarefulWriteNodeId stopped = disk::kCarefulWriteNoNode;
    if (!detail::RunChainPlan(plan, sink, binding_.barrier_policy, result, failed_, &stopped)) {
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

TransactionCompleteAndBeginResult TransactionInventoryWriter::CompleteAndBegin(
    disk::CarefulWriteSink& sink, u64 n, TipStateCode final_state, const TransactionHeaderRecord& header_m,
    u64 now_utc_ns) noexcept {
  using R = TransactionCompleteAndBeginResult;
  if (!page_size_) return Refuse<R>(E::invalid_request);
  if (failed_) return Refuse<R>(E::writer_failed);
  if (binding_.read_only_database) return Refuse<R>(E::read_only_database);
  if (!restarted_) return Refuse<R>(E::restart_required);
  if (n == 0 || now_utc_ns == 0 ||
      (final_state != TipStateCode::committed && final_state != TipStateCode::rolled_back))
    return Refuse<R>(E::invalid_request);
  if (header_m.authority != TipAuthorityClass::local || header_m.transaction_order != 0 ||
      header_m.final_state != TipStateCode::unused || header_m.commit_time_utc_ns != 0 ||
      header_m.completed_at_utc_ns != 0 || header_m.intent_generation != 0 ||
      !(header_m.database_uuid.is_nil() || header_m.database_uuid == binding_.database_uuid))
    return Refuse<R>(E::invalid_request);
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
    const u64 m = chains.counters.next_transaction;
    if (m == std::numeric_limits<u64>::max()) return refuse(E::chain_full);
    // m's record, validated before anything is planned.
    TransactionHeaderRecord record_m = header_m;
    record_m.transaction_order = m;
    record_m.database_uuid = binding_.database_uuid;
    if (const auto valid = ValidateTransactionHeaderRecord(record_m); valid != TransactionHeaderError::none) {
      auto refused = refuse(E::invalid_header);
      refused.header_error = valid;
      refused.replacement_order = m;
      return refused;
    }
    // Durable before cited (plan 36 T4): m's citations, before anything is
    // prepared or written (chain growth below is a write).
    if (const auto cited = CheckLedgerCitations(binding_, record_m); cited != E::none) {
      auto refused = refuse(cited);
      refused.replacement_order = m;
      return refused;
    }
    // Everything about n that can be refused, before the first write (chain
    // growth for m is a write): n in progress, its completed record.
    ActiveTransaction active;
    TransactionHeaderRecord record_n;
    u64 at = 0;
    auto prepare_n = [&]() -> std::optional<R> {
      if (const auto found = FindActiveTransaction(chains, page_size_, n, active); found != E::none)
        return refuse(found);
      record_n = active.record;
      TransactionHeaderError completed = TransactionHeaderError::none;
      if (const auto done = CompleteRecord(chains, page_size_, now_utc_ns, final_state, record_n, at, completed);
          done != E::none) {
        auto refused = refuse(done);
        if (done == E::invalid_header) refused.header_error = completed;
        return refused;
      }
      return std::nullopt;
    };
    if (auto refused = prepare_n()) return *refused;
    // Chain growth for m's slots (its own plan, as BEGIN), then the capacity
    // check.
    if (auto refused = GrowForNext(sink, binding_, page_size_, chains, m, result, failed_)) {
      static_cast<TransactionBeginResult&>(result) = *refused;
      result.transaction_order = n;
      result.replacement_order = m;
      return result;
    }
    if (result.grown_state_pages || result.grown_header_pages)
      if (auto refused = prepare_n()) return *refused;  // the chains were re-read
    const auto state_slot_m = LocateTransactionStateSlot(page_size_, m);
    const auto header_slot_m = LocateTransactionHeaderSlot(page_size_, m);
    const auto state_slot_n = active.state_slot;
    const auto header_slot_n = active.header_slot;
    // Horizons over the inventory as the file shows it, n still active (see
    // the design comment: the file stays admissible after every write).
    TipCounters counters;
    if (auto refused = BuildBeginCounters(page_size_, chains, m, counters)) {
      static_cast<TransactionBeginResult&>(result) = *refused;
      result.transaction_order = n;
      result.replacement_order = m;
      return result;
    }
    // W1..W5 as edits of the page images held here, applied in plan order so
    // that the first edit of a write unit carries the unit's preimage.
    auto first = PageOf(chains.state, page_size_, 0);
    const auto w1 = SetTransactionStateCounters(first, counters);
    auto header_image_m = PageOf(chains.headers, page_size_, static_cast<std::size_t>(header_slot_m->sequence));
    const auto w2 = WriteTransactionHeader(header_image_m, record_m);
    auto state_image_m = PageOf(chains.state, page_size_, static_cast<std::size_t>(state_slot_m->tip_sequence));
    TipStateWord word;
    word.state = record_m.read_only ? TipStateCode::read_only_active : TipStateCode::active;
    word.flags = kTipFlagHasDetailRecord;
    word.authority = TipAuthorityClass::local;
    word.intent = record_m.intent;
    const auto w3 = SetTransactionStateWord(state_image_m, state_slot_m->slot, word);
    auto header_image_n = PageOf(chains.headers, page_size_, static_cast<std::size_t>(header_slot_n.sequence));
    const auto w4 = UpdateTransactionHeader(header_image_n, record_n);
    auto state_image_n = PageOf(chains.state, page_size_, static_cast<std::size_t>(state_slot_n.tip_sequence));
    const auto w5 = SetTransactionStateCode(state_image_n, state_slot_n.slot, final_state);
    if (!w1.ok() || !w2.ok() || !w3.ok() || !w4.ok() || !w5.ok() || !w1.changed || !w2.changed || !w3.changed ||
        !w4.changed || !w5.changed) {
      auto refused = refuse(E::invalid_inventory);
      refused.replacement_order = m;
      return refused;
    }
    const bool merge_headers = header_slot_m->sequence == header_slot_n.sequence && w2.unit_offset == w4.unit_offset;
    const bool merge_words = state_slot_m->tip_sequence == state_slot_n.tip_sequence && w3.unit_offset == w5.unit_offset;
    disk::CarefulWritePlan plan;
    std::vector<disk::CarefulWriteAddResult> nodes;  // in write order
    const auto page_h_m = chains.header_pages[header_slot_m->sequence];
    const auto page_h_n = chains.header_pages[header_slot_n.sequence];
    const auto page_s_m = chains.state_pages[state_slot_m->tip_sequence];
    const auto page_s_n = chains.state_pages[state_slot_n.tip_sequence];
    nodes.push_back(detail::AddChainUnit(plan, chains.state_pages[0], page_size_, first, w1));  // W1
    disk::CarefulWriteAddResult n5{};
    if (merge_headers) {
      nodes.push_back(detail::AddChainUnit(plan, page_h_m, page_size_, header_image_m, w2));  // [W2 W4]: W4 may come early
    } else {
      nodes.push_back(detail::AddChainUnit(plan, page_h_m, page_size_, header_image_m, w2));  // W2
      if (merge_words) nodes.push_back(detail::AddChainUnit(plan, page_h_n, page_size_, header_image_n, w4));  // W4 before [W3 W5]
    }
    if (merge_words) {
      n5 = detail::AddChainUnit(plan, page_s_m, page_size_, state_image_m, w3);  // [W3 W5]: after W4
      nodes.push_back(n5);
    } else {
      nodes.push_back(detail::AddChainUnit(plan, page_s_m, page_size_, state_image_m, w3));  // W3
      if (!merge_headers) nodes.push_back(detail::AddChainUnit(plan, page_h_n, page_size_, header_image_n, w4));  // W4
      n5 = detail::AddChainUnit(plan, page_s_n, page_size_, state_image_n, w5);  // W5
      nodes.push_back(n5);
    }
    bool built = true;
    for (const auto& node : nodes) built = built && node.ok();
    for (std::size_t i = 1; built && i < nodes.size(); ++i)
      built = plan.Before(nodes[i - 1].node, nodes[i].node) == CarefulWriteError::none;
    if (!built) {
      auto refused = refuse(E::resource_exhausted);
      refused.replacement_order = m;
      return refused;
    }
    result.counters = counters;
    result.completed_at_utc_ns = at;
    result.replacement_order = m;
    result.replacement_uuid = record_m.transaction_uuid;
    result.merged_headers = merge_headers;
    result.merged_state_words = merge_words;
    disk::CarefulWriteNodeId stopped = disk::kCarefulWriteNoNode;
    if (!detail::RunChainPlan(plan, sink, binding_.barrier_policy, result, failed_, &stopped)) {
      // Uncertain only once the write that holds n's state byte was attempted
      // (a failed write of it may have reached the file), or when the final
      // sync after it failed.
      result.outcome_uncertain =
          result.write_error == CarefulWriteError::final_sync_failed ||
          (stopped == n5.node && (result.write_error == CarefulWriteError::write_failed ||
                                  result.write_error == CarefulWriteError::sink_exception));
    }
    return result;
  } catch (const std::bad_alloc&) {
    return Refuse<R>(E::resource_exhausted);
  } catch (const std::length_error&) {
    return Refuse<R>(E::resource_exhausted);
  }
}

TransactionChainsRead ReadTransactionChainsThroughSink(disk::CarefulWriteSink& sink,
                                                       const TransactionWriterBinding& binding) noexcept {
  TransactionChainsRead out;
  try {
    const TransactionInventoryWriter checked(binding);
    if (!checked.page_size()) return out;
    Chains chains;
    if (!ReadChains(sink, binding, checked.page_size(), chains, out)) return out;
    out.page_size = checked.page_size();
    out.state_images = std::move(chains.state);
    out.header_images = std::move(chains.headers);
    out.state_pages = std::move(chains.state_pages);
    out.header_pages = std::move(chains.header_pages);
    out.counters = chains.counters;
    out.error = E::none;
  } catch (const std::bad_alloc&) {
    out.error = E::resource_exhausted;
  } catch (const std::length_error&) {
    out.error = E::resource_exhausted;
  }
  return out;
}

}  // namespace scratchbird::storage::page
