// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "transaction_inventory_reader.hpp"

#include <algorithm>
#include <limits>

namespace scratchbird::storage::page {
namespace {
namespace disk = scratchbird::storage::disk;
using E = TipPageError;
using Stage = TransactionInventoryReadStage;

TransactionInventoryCheck Finding(E error, Stage stage, std::size_t page_index = 0,
                                  std::optional<u64> order = std::nullopt) {
  TransactionInventoryCheck c;
  c.error = error;
  c.stage = stage;
  c.page_index = page_index;
  c.transaction_order = order;
  return c;
}

bool SamePlace(const disk::NativeCommonPageHeader& a, const disk::NativeCommonPageHeader& b) {
  return a.database_uuid == b.database_uuid && a.filespace_uuid == b.filespace_uuid &&
         a.page_size_profile_uuid == b.page_size_profile_uuid && a.page_size_bytes == b.page_size_bytes;
}

// Record of transaction_order n in the header chain, or nullopt when its slot is
// beyond the chain or empty. The header pages were all validated first.
std::optional<TransactionHeaderRecord> HeaderOf(std::span<const byte> header_images, u32 page_size, u32 slots,
                                                u64 n) {
  const u64 page = n / slots;
  if (page >= header_images.size() / page_size) return std::nullopt;
  const auto at = page * page_size + kTransactionHeaderPageRecordsOffset +
                  kTransactionHeaderRecordBytes * static_cast<std::size_t>(n % slots);
  const auto bytes = header_images.subspan(at, kTransactionHeaderRecordBytes);
  if (std::all_of(bytes.begin(), bytes.end(), [](byte b) { return b == 0; })) return std::nullopt;
  const auto record = DecodeTransactionHeaderRecord(bytes);
  if (!record.ok()) return std::nullopt;
  return record.record;
}
}  // namespace

TransactionInventoryCheck CheckTransactionInventoryImages(u32 page_size, std::span<const byte> state_images,
                                                          std::span<const byte> header_images) noexcept {
  if (!page_size || state_images.empty() || state_images.size() % page_size || header_images.empty() ||
      header_images.size() % page_size)
    return Finding(E::invalid_page_size, Stage::request);
  const std::size_t state_count = state_images.size() / page_size;
  const std::size_t header_count = header_images.size() / page_size;
  auto state_page = [&](std::size_t i) { return state_images.subspan(i * page_size, page_size); };
  auto header_page = [&](std::size_t i) { return header_images.subspan(i * page_size, page_size); };

  // State chain.
  const auto first = DecodeTransactionStatePage(state_page(0));
  if (!first.ok()) return Finding(first.error, Stage::state_chain, 0);
  const TipPageView head = *first.view;
  if (head.tip_sequence != 0) return Finding(E::invalid_link, Stage::state_chain, 0);
  const u64 next = head.counters->next_transaction;
  TipPageView last = head;
  for (std::size_t i = 0; i < state_count; ++i) {
    const auto d = i == 0 ? first : DecodeTransactionStatePage(state_page(i));
    if (!d.ok()) return Finding(d.error, Stage::state_chain, i);
    const auto& v = *d.view;
    if (v.tip_sequence != i || !SamePlace(v.header, head.header)) return Finding(E::invalid_link, Stage::state_chain, i);
    if (i > 0) {
      if (last.next_tip_page != v.header.page_number) return Finding(E::invalid_link, Stage::state_chain, i);
      const auto below = CheckTransactionStateSlotsBelowNext(v, next);
      if (!below.ok()) return Finding(below.error, Stage::state_chain, i, v.first_number + *below.slot);
    }
    last = v;
  }
  if (last.next_tip_page != 0) return Finding(E::invalid_link, Stage::state_chain, state_count - 1);
  if (last.first_number + last.slots < next) return Finding(E::invalid_counters, Stage::state_chain, state_count - 1);

  // Header chain.
  u32 header_slots = 0;
  u64 previous_link = head.header_chain_page;
  for (std::size_t i = 0; i < header_count; ++i) {
    const auto d = DecodeTransactionHeaderPage(header_page(i));
    if (!d.ok()) return Finding(d.error, Stage::header_chain, i);
    const auto& v = *d.view;
    if (v.sequence != i || !SamePlace(v.header, head.header) || v.header.page_number != previous_link)
      return Finding(E::invalid_link, Stage::header_chain, i);
    header_slots = v.slots;
    previous_link = v.next_page;
    // No header at or beyond Next.
    for (u32 slot = 0; slot < v.slots; ++slot) {
      if (v.first_number + slot < next) continue;
      if (TransactionHeaderAt(v, slot)) return Finding(E::invalid_slot, Stage::cross_check, i, v.first_number + slot);
    }
  }
  if (previous_link != 0) return Finding(E::invalid_link, Stage::header_chain, header_count - 1);

  // Cross-check every used slot below Next against its header.
  for (std::size_t i = 0; i < state_count; ++i) {
    const auto d = DecodeTransactionStatePage(state_page(i));
    const auto& v = *d.view;
    const u32 limit = static_cast<u32>(std::min<u64>(v.slots, next > v.first_number ? next - v.first_number : 0));
    for (u32 slot = 0; slot < limit; ++slot) {
      const auto word = TipWordAt(v, slot);
      if (!word || word->state == TipStateCode::unused) continue;
      const u64 n = v.first_number + slot;
      const auto header = HeaderOf(header_images, page_size, header_slots, n);
      const auto final_state = header ? header->final_state : TipStateCode::unused;
      bool ok = true;
      if ((word->flags & kTipFlagHasDetailRecord) && !header) ok = false;
      switch (word->state) {
        case TipStateCode::committed:
        case TipStateCode::rolled_back: ok = ok && header && final_state == word->state; break;
        case TipStateCode::archived: ok = ok && header && final_state != TipStateCode::unused; break;
        case TipStateCode::prepared:
        case TipStateCode::limbo: ok = ok && header && final_state == TipStateCode::unused; break;
        default: break;
      }
      if (!ok) return Finding(E::invalid_slot, Stage::cross_check, i, n);
    }
  }
  TransactionInventoryCheck c;
  c.error = E::none;
  c.stage = Stage::none;
  c.counters = *head.counters;
  return c;
}

TransactionInventoryRead ReadTransactionInventory(const TransactionInventoryReadRequest& r) noexcept {
  TransactionInventoryRead out;
  out.check = Finding(E::invalid_header, Stage::request);
  if (r.files.empty() || r.batches.size() != r.files.size() || r.state_root.page_type != kTransactionStatePageType ||
      r.state_root.kind != 4 || !r.state_root.page_number || !r.state_root.page_generation)
    return out;
  std::size_t file = r.files.size();
  for (std::size_t i = 0; i < r.files.size(); ++i) {
    if (!r.files[i].device || !r.batches[i] || &r.batches[i]->device() != r.files[i].device) return out;
    if (r.files[i].filespace_uuid == r.state_root.filespace_uuid) file = i;
  }
  if (file == r.files.size() || r.files[file].page_size_profile_uuid != r.state_root.page_size_profile_uuid)
    return out;
  const auto* profile = disk::FindCanonicalFilespacePageProfile(r.state_root.page_size_profile_uuid);
  if (!profile) return out;
  const u32 page_size = profile->page_size_bytes;
  out.page_size = page_size;
  auto& batch = *r.batches[file];

  std::size_t used = 0;
  // Reads page `number` into the next backing page; false on a refusal (out set).
  auto read_page = [&](u64 number, Stage stage, std::size_t index) -> std::optional<std::span<const byte>> {
    if (r.backing.size() / page_size <= used) {
      out.check = Finding(E::short_buffer, stage, index);
      return std::nullopt;
    }
    if (number > std::numeric_limits<u64>::max() / page_size) {
      out.check = Finding(E::invalid_link, stage, index);
      return std::nullopt;
    }
    auto target = r.backing.subspan(used * page_size, page_size);
    const auto io = batch.ReadAtBounded(number * page_size, target.data(), page_size);
    if (!io.ok() || io.bytes_transferred != page_size) {
      out.io_failure = true;
      out.io = io;
      out.check = Finding(E::invalid_header, stage, index);
      return std::nullopt;
    }
    ++used;
    return std::span<const byte>(target);
  };

  // State chain from the root, bound to the root's identity.
  disk::NativeCommonPageHeaderBinding binding;
  binding.filespace = {r.database_uuid, r.state_root.filespace_uuid, r.state_root.page_size_profile_uuid};
  binding.page_number = r.state_root.page_number;
  binding.page_generation = r.state_root.page_generation;
  binding.page_type = kTransactionStatePageType;
  u64 page = r.state_root.page_number;
  u64 header_chain = 0;
  for (std::size_t i = 0; page != 0; ++i) {
    const auto image = read_page(page, Stage::state_chain, i);
    if (!image) return out;
    const auto d = DecodeTransactionStatePage(*image, i == 0 ? &binding : nullptr);
    if (!d.ok()) {
      out.check = Finding(d.error, Stage::state_chain, i);
      return out;
    }
    if (d.view->tip_sequence != i) {
      out.check = Finding(E::invalid_link, Stage::state_chain, i);
      return out;
    }
    if (i == 0) header_chain = d.view->header_chain_page;
    page = d.view->next_tip_page;
  }
  out.state_pages = used;
  // Header chain.
  page = header_chain;
  for (std::size_t i = 0; page != 0; ++i) {
    const auto image = read_page(page, Stage::header_chain, i);
    if (!image) return out;
    const auto d = DecodeTransactionHeaderPage(*image);
    if (!d.ok()) {
      out.check = Finding(d.error, Stage::header_chain, i);
      return out;
    }
    if (d.view->sequence != i) {
      out.check = Finding(E::invalid_link, Stage::header_chain, i);
      return out;
    }
    page = d.view->next_page;
  }
  out.header_pages = used - out.state_pages;
  out.check = CheckTransactionInventoryImages(page_size, r.backing.first(out.state_pages * page_size),
                                              r.backing.subspan(out.state_pages * page_size,
                                                                out.header_pages * page_size));
  return out;
}

}  // namespace scratchbird::storage::page
