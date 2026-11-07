// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "transaction_inventory_reader.hpp"
#include "transaction_state_code_map.hpp"
#include "transaction_inventory_page.hpp"
#include "transaction_inventory_validation.hpp"
#include "transaction_horizon_projection.hpp"
#include "native_inventory_chain_backing.hpp"
#include "native_metadata_memory.hpp"
#include "native_common_page_header.hpp"
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>

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
      if (!header && ((word->flags & kTipFlagHasDetailRecord) || word->state != TipStateCode::dead)) ok = false;
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

TransactionInventoryBuildView BuildLocalTransactionInventoryInto(u32 page_size, std::span<const byte> state_images,
                                                                 std::span<const byte> header_images,
                                                                 const TransactionInventoryBuildScratch& scratch) noexcept {
  namespace mga = scratchbird::transaction::mga;
  TransactionInventoryBuildView out;
  const auto check = CheckTransactionInventoryImages(page_size, state_images, header_images);
  if (!check.ok()) {
    out.error = check.error;
    out.transaction_order = check.transaction_order;
    return out;
  }
  const u64 next = check.counters.next_transaction;
  const u64 capacity = next - 1;
  if (scratch.entries.size() < capacity || scratch.commits.size() < capacity || scratch.indices.size() < capacity ||
      scratch.markers.size() < capacity) {
    out.error = E::short_buffer;
    return out;
  }
  const u32 header_slots = TransactionHeaderSlotsPerPage(page_size);
  std::size_t count = 0, committed = 0;
  for (std::size_t i = 0; i < state_images.size() / page_size; ++i) {
    const auto d = DecodeTransactionStatePage(state_images.subspan(i * page_size, page_size));
    const auto& v = *d.view;
    const u32 limit = static_cast<u32>(std::min<u64>(v.slots, next > v.first_number ? next - v.first_number : 0));
    for (u32 slot = 0; slot < limit; ++slot) {
      const auto word = TipWordAt(v, slot);
      if (!word || word->state == TipStateCode::unused) continue;
      const u64 n = v.first_number + slot;
      const auto header = HeaderOf(header_images, page_size, header_slots, n);
      if (!header) continue;  // dead without a header (checked above)
      const auto state = InMemoryTransactionState(word->state);
      if (!state || count >= capacity) {
        out.error = E::invalid_word;
        out.transaction_order = n;
        return out;
      }
      mga::TransactionInventoryEntry e;
      e.identity.local_id = mga::MakeLocalTransactionId(n);
      e.identity.transaction_uuid = {scratchbird::core::platform::UuidKind::transaction, header->transaction_uuid};
      e.identity.scope = header->authority == TipAuthorityClass::cluster ? mga::TransactionScope::cluster_global
                                                                          : mga::TransactionScope::local_node;
      e.state = *state;
      if (*state == mga::TransactionState::archived) {
        const auto origin = InMemoryTransactionState(header->final_state);
        e.archived_from_state = origin ? *origin : mga::TransactionState::none;
      }
      const auto begun = scratchbird::core::uuid::ExtractUuidV7TimePrefix(header->transaction_uuid);
      e.begin_unix_epoch_millis = begun.ok ? begun.unix_epoch_millis : 0;
      e.final_unix_epoch_millis = header->completed_at_utc_ns / 1'000'000;
      e.evidence_record_written = header->final_state != TipStateCode::unused;
      if (mga::HasCommittedInventoryOutcome(e)) scratch.commits[committed++] = {header->commit_time_utc_ns, count};
      scratch.entries[count++] = e;
    }
  }
  // Commit sequences 1..k in commit-time order; ties by transaction_order,
  // which is entry order.
  std::sort(scratch.commits.begin(), scratch.commits.begin() + committed);
  for (std::size_t k = 0; k < committed; ++k) scratch.entries[scratch.commits[k].second].commit_sequence = k + 1;
  out.next_local_transaction_id = next;
  out.next_commit_sequence = committed + 1;
  out.entries = scratch.entries.first(count);
  const NativeTransactionInventoryConstView view{out.next_local_transaction_id, out.next_commit_sequence, out.entries};
  if (!ValidateNativeTransactionInventoryConstView(view, scratch.indices.first(count), scratch.markers.first(count)).ok()) {
    out.error = E::invalid_word;
    out.entries = {};
    return out;
  }
  out.error = E::none;
  return out;
}

TransactionInventoryBuild BuildLocalTransactionInventoryFromImages(u32 page_size, std::span<const byte> state_images,
                                                                   std::span<const byte> header_images) {
  TransactionInventoryBuild out;
  const auto check = CheckTransactionInventoryImages(page_size, state_images, header_images);
  if (!check.ok()) {
    out.error = check.error;
    out.transaction_order = check.transaction_order;
    return out;
  }
  const std::size_t capacity = check.counters.next_transaction - 1;
  std::vector<scratchbird::transaction::mga::TransactionInventoryEntry> entries(capacity);
  std::vector<std::pair<u64, std::size_t>> commits(capacity);
  std::vector<std::size_t> indices(capacity);
  std::vector<byte> markers(capacity);
  const auto built = BuildLocalTransactionInventoryInto(page_size, state_images, header_images,
                                                        {entries, commits, indices, markers});
  out.error = built.error;
  out.transaction_order = built.transaction_order;
  if (!built.ok()) return out;
  out.inventory.next_local_transaction_id = built.next_local_transaction_id;
  out.inventory.next_commit_sequence = built.next_commit_sequence;
  out.inventory.entries.assign(built.entries.begin(), built.entries.end());
  return out;
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
    out.physical_bytes_read += u64{io.bytes_transferred};
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

detail::NativeInPlaceInventoryRead detail::ReadNativeInPlaceInventoryBacked(
    const scratchbird::core::platform::Uuid& database, std::span<const disk::NativeFilespaceDevice> files,
    const disk::FilespaceRootReference& head, u64 maximum_retained_image_bytes,
    std::span<disk::FileDevice::ReadLatencyBatch* const> batches, std::pmr::memory_resource& resource,
    std::span<const byte> head_image) noexcept {
  namespace mga = scratchbird::transaction::mga;
  using NE = NativeInventoryError;
  NativeInPlaceInventoryRead out;
  // Anything this reader cannot place is left to the SBTINV01 reader, which
  // refuses it with its own finding.
  if (files.empty() || batches.size() != files.size() || head.kind != 4 || head.page_type != kTransactionStatePageType)
    return out;
  std::size_t file = files.size();
  for (std::size_t i = 0; i < files.size(); ++i) {
    if (!files[i].device || !batches[i] || &batches[i]->device() != files[i].device) return out;
    if (files[i].filespace_uuid == head.filespace_uuid) file = i;
  }
  if (file == files.size() || files[file].page_size_profile_uuid != head.page_size_profile_uuid) return out;
  const auto* profile = disk::FindCanonicalFilespacePageProfile(head.page_size_profile_uuid);
  if (!profile || head.page_number > std::numeric_limits<u64>::max() / profile->page_size_bytes) return out;
  const u32 page_size = profile->page_size_bytes;
  auto& receipt = out.read;
  const auto fail = [&](NE error) {
    receipt.chain = {};
    receipt.chain.error = error;
    return out;
  };
  const auto failed_io = [&](const disk::BoundedIoResult& io) {
    receipt.io_status = disk::BoundedIoStatus(io);
    return fail(NE::io_failure);
  };
  std::array<byte, kTransactionStateWordsOffset> probe{};
  if (!head_image.empty()) {
    if (head_image.size() != page_size) return out;
    std::copy_n(head_image.begin(), probe.size(), probe.begin());
  } else {
    const auto probed = batches[file]->ReadAtBounded(head.page_number * page_size, probe.data(), probe.size());
    receipt.physical_bytes_read += probed.bytes_transferred;
    if (!probed.ok() || probed.bytes_transferred != probe.size()) {
      out.handled = true;
      return failed_io(probed);
    }
  }
  if (!std::equal(kTransactionStatePageMagic.begin(), kTransactionStatePageMagic.end(),
                  probe.begin() + kTransactionStateFamilyOffset))
    return out;
  out.handled = true;
  try {
    const auto first = disk::DecodeNativeCommonPageHeader(probe.data(), disk::kNativeCommonPageHeaderBytes);
    if (!first.ok()) return fail(NE::invalid_header);
    if (first.header->flags & 1u) return fail(NE::encrypted_requires_crypto_authority);
    const u64 next = scratchbird::core::platform::LoadLittle64(probe.data() + kTransactionStateCountersOffset + 8);
    const u32 state_slots = TipSlotsPerPage(page_size), header_slots = TransactionHeaderSlotsPerPage(page_size);
    if (!next || !state_slots || !header_slots) return fail(NE::invalid_inventory);
    // Both chains cover every number below Next (the fewest pages they can
    // have), and each may link one spare page. Refuse only when the fewest do
    // not fit; read spares only as far as the allowance reaches.
    const u64 fewest = ((next - 1) / state_slots + 1) + ((next - 1) / header_slots + 1);
    const u64 allowed = maximum_retained_image_bytes / page_size;
    if (fewest > allowed) return fail(NE::resource_exhausted);
    const u64 pages = std::min<u64>(fewest + 2, allowed);
    if (pages > std::numeric_limits<std::size_t>::max() / page_size) return fail(NE::resource_exhausted);
    disk::detail::NativeMetadataScratch scratch{&resource};
    auto backing = scratch.Array<byte>(static_cast<std::size_t>(pages * page_size));
    const TransactionInventoryReadRequest request{database, head, files, batches, backing};
    const auto read = ReadTransactionInventory(request);
    // Every byte read, on every exit: pages read before a failed read or a
    // refused page count as much as the pages of a complete chain.
    receipt.physical_bytes_read += read.physical_bytes_read;
    if (read.io_failure) return failed_io(read.io);
    const std::size_t count = read.state_pages + read.header_pages;
    if (!read.ok()) return fail(read.check.error == TipPageError::short_buffer ? NE::resource_exhausted : NE::invalid_inventory);
    // The head was read before (the probe, or the caller's whole page): the
    // chain's read of it must be the same bytes.
    if (!std::equal(probe.begin(), probe.end(), backing.begin()) ||
        (!head_image.empty() && !std::equal(head_image.begin(), head_image.end(), backing.begin())))
      return fail(NE::binding_mismatch);
    const std::span<const byte> state_images(backing.data(), read.state_pages * page_size);
    const std::span<const byte> header_images(backing.data() + read.state_pages * page_size,
                                              read.header_pages * page_size);
    const std::size_t capacity = static_cast<std::size_t>(next - 1);
    const TransactionInventoryBuildScratch build{scratch.Array<mga::TransactionInventoryEntry>(capacity),
                                                 scratch.Array<std::pair<u64, std::size_t>>(capacity),
                                                 scratch.Array<std::size_t>(capacity), scratch.Array<byte>(capacity)};
    const auto built = BuildLocalTransactionInventoryInto(page_size, state_images, header_images, build);
    if (!built.ok()) return fail(NE::invalid_inventory);
    NativeInventoryChainView chain;
    chain.inventory = {built.next_local_transaction_id, built.next_commit_sequence, built.entries};
    LocalTransactionHorizons horizons;
    if (mga::detail::ProjectValidatedLocalHorizons(chain.inventory, {}, horizons)) return fail(NE::invalid_inventory);
    auto views = scratch.Array<NativeInventoryChainPageView>(count);
    for (std::size_t n = 0; n < count; ++n) {
      const auto image = std::span<const byte>(backing.data() + n * page_size, page_size);
      const auto header = disk::DecodeNativeCommonPageHeader(image.data(), disk::kNativeCommonPageHeaderBytes);
      if (!header.ok() || header.header->database_uuid != database) return fail(NE::binding_mismatch);
      if (header.header->flags & 1u) return fail(NE::encrypted_requires_crypto_authority);
      views[n].page.header = *header.header;
      views[n].page.object_uuid = head.object_uuid;
      views[n].page.inventory_generation = 1;
      views[n].image = image;
    }
    chain.pages = views;
    chain.horizons = horizons;
    chain.retained_image_bytes = u64{count} * page_size;
    chain.error = NE::none;
    receipt.chain = chain;
    out.counters = read.check.counters;
    return out;
  } catch (const std::bad_alloc&) {
    return fail(NE::resource_exhausted);
  } catch (const std::length_error&) {
    return fail(NE::resource_exhausted);
  } catch (...) {
    return fail(NE::io_failure);
  }
}

}  // namespace scratchbird::storage::page
