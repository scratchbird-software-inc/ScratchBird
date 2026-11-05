// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "transaction_state_page.hpp"

#include <algorithm>
#include <limits>

namespace scratchbird::storage::page {
namespace {
namespace disk = scratchbird::storage::disk;
using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using E = TipPageError;

// Family header fields (offsets within the page).
constexpr std::size_t kMagic = kTransactionStateFamilyOffset;          // 8 bytes
constexpr std::size_t kVersion = kTransactionStateFamilyOffset + 8;    // u16
constexpr std::size_t kWordBytes = kTransactionStateFamilyOffset + 10; // u16
constexpr std::size_t kSlots = kTransactionStateFamilyOffset + 12;     // u32
constexpr std::size_t kSequence = kTransactionStateFamilyOffset + 16;  // u64
constexpr std::size_t kFirstNumber = kTransactionStateFamilyOffset + 24; // u64
constexpr std::size_t kFamilyEnd = kTransactionStateFamilyOffset + 32;
// Counters.
constexpr std::size_t kNextPage = kTransactionStateCountersOffset;            // every page
constexpr std::size_t kNext = kTransactionStateCountersOffset + 8;            // first page only
constexpr std::size_t kHorizons = kTransactionStateCountersOffset + 16;       // six u64
constexpr std::size_t kCountersEnd = kTransactionStateCountersOffset + 64;

bool Zero(const byte* data, std::size_t size) {
  return std::all_of(data, data + size, [](byte b) { return b == 0; });
}

bool HorizonsValid(const TipCounters& c) {
  if (c.next_transaction == 0) return false;
  const auto& h = c.horizons;
  for (const u64 v : {h.oldest_interesting, h.oldest_active, h.oldest_snapshot, h.oldest_cleanup,
                      h.oldest_archive_required, h.oldest_limbo_resolution})
    if (v == 0 || v > c.next_transaction) return false;
  return true;
}

void PutCounters(byte* page, const TipCounters& c) {
  StoreLittle64(page + kNext, c.next_transaction);
  const auto& h = c.horizons;
  const u64 values[] = {h.oldest_interesting, h.oldest_active, h.oldest_snapshot, h.oldest_cleanup,
                        h.oldest_archive_required, h.oldest_limbo_resolution};
  for (std::size_t i = 0; i < 6; ++i) StoreLittle64(page + kHorizons + 8 * i, values[i]);
}

TipCounters GetCounters(const byte* page) {
  TipCounters c;
  c.next_transaction = LoadLittle64(page + kNext);
  auto& h = c.horizons;
  u64* values[] = {&h.oldest_interesting, &h.oldest_active, &h.oldest_snapshot, &h.oldest_cleanup,
                   &h.oldest_archive_required, &h.oldest_limbo_resolution};
  for (std::size_t i = 0; i < 6; ++i) *values[i] = LoadLittle64(page + kHorizons + 8 * i);
  return c;
}

std::size_t WordOffset(u32 slot) { return kTransactionStateWordsOffset + kTransactionStateWordBytes * slot; }

// Family header of an image these functions produced. Returns the slot count,
// or 0 when the image is not such a page.
struct Family {
  u32 page_size = 0;
  u32 slots = 0;
  u64 sequence = 0;
  u64 first_number = 0;
};
std::optional<Family> ReadFamily(std::span<const byte> image) {
  if (image.size() < kTransactionStateWordsOffset) return std::nullopt;
  const byte* p = image.data();
  const u32 page_size = LoadLittle32(p + 12);  // common header page_size_bytes
  const u32 slots = TipSlotsPerPage(page_size);
  if (!slots || image.size() != page_size) return std::nullopt;
  if (!std::equal(kTransactionStatePageMagic.begin(), kTransactionStatePageMagic.end(), p + kMagic) ||
      LoadLittle16(p + kVersion) != kTransactionStatePageVersion ||
      LoadLittle16(p + kWordBytes) != kTransactionStateWordBytes || LoadLittle32(p + kSlots) != slots)
    return std::nullopt;
  const u64 sequence = LoadLittle64(p + kSequence);
  if (sequence > std::numeric_limits<u64>::max() / slots ||
      LoadLittle64(p + kFirstNumber) != sequence * slots)
    return std::nullopt;
  return Family{page_size, slots, sequence, sequence * slots};
}

std::array<byte, kTransactionStateWordBytes> WordBytes(std::span<const byte> image, u32 slot) {
  std::array<byte, kTransactionStateWordBytes> w{};
  std::copy_n(image.data() + WordOffset(slot), w.size(), w.begin());
  return w;
}

TipPageEdit Begin(std::span<byte> image, std::size_t changed_offset) {
  TipPageEdit edit;
  edit.error = E::none;
  edit.unit_offset = static_cast<u32>(changed_offset - changed_offset % kTransactionStateWriteUnitBytes);
  std::copy_n(image.data() + edit.unit_offset, kTransactionStateWriteUnitBytes, edit.preimage.begin());
  return edit;
}
TipPageEdit Refuse(E error) {
  TipPageEdit edit;
  edit.error = error;
  return edit;
}

// Common edit preconditions for a slot: a valid family, a slot in range, never
// transaction_order 0, and on the first page a number below Next.
E CheckEditSlot(std::span<const byte> image, const Family& f, u32 slot) {
  if (slot >= f.slots) return E::invalid_slot;
  if (f.sequence == 0 && slot == 0) return E::invalid_slot;
  if (f.sequence == 0 && f.first_number + slot >= LoadLittle64(image.data() + kNext)) return E::invalid_slot;
  return E::none;
}
}  // namespace

const char* TipStateCodeName(TipStateCode code) noexcept {
  switch (code) {
    case TipStateCode::unused: return "unused";
    case TipStateCode::starting: return "starting";
    case TipStateCode::active: return "active";
    case TipStateCode::read_only_active: return "read_only_active";
    case TipStateCode::prepared: return "prepared";
    case TipStateCode::committing: return "committing";
    case TipStateCode::committed: return "committed";
    case TipStateCode::rolling_back: return "rolling_back";
    case TipStateCode::rolled_back: return "rolled_back";
    case TipStateCode::dead: return "dead";
    case TipStateCode::limbo: return "limbo";
    case TipStateCode::recovering: return "recovering";
    case TipStateCode::archived: return "archived";
  }
  return "invalid";
}

const char* TipPageErrorName(TipPageError error) noexcept {
  switch (error) {
    case E::none: return "none";
    case E::invalid_page_size: return "invalid_page_size";
    case E::invalid_header: return "invalid_header";
    case E::invalid_family: return "invalid_family";
    case E::invalid_counters: return "invalid_counters";
    case E::invalid_word: return "invalid_word";
    case E::invalid_slot: return "invalid_slot";
    case E::invalid_link: return "invalid_link";
    case E::next_regression: return "next_regression";
    case E::short_buffer: return "short_buffer";
  }
  return "invalid";
}

u32 TipSlotsPerPage(u32 page_size) noexcept {
  if (page_size <= kTransactionStateWordsOffset || page_size % kTransactionStateWriteUnitBytes) return 0;
  return static_cast<u32>((page_size - kTransactionStateWordsOffset) / kTransactionStateWordBytes);
}

std::optional<TipStateWord> DecodeTipStateWord(std::span<const byte, kTransactionStateWordBytes> b) noexcept {
  if (b[0] > kTipStateCodeMax || (b[1] & ~kTipFlagMask) || (b[2] & 3u) == 3u) return std::nullopt;
  TipStateWord w;
  w.state = static_cast<TipStateCode>(b[0]);
  w.flags = b[1];
  w.authority = static_cast<TipAuthorityClass>(b[2] & 3u);
  w.intent = static_cast<std::uint8_t>(b[2] >> 2);
  w.epoch_delta = b[3];
  // An unused slot is all zero.
  if (w.state == TipStateCode::unused && (b[1] || b[2] || b[3])) return std::nullopt;
  return w;
}

std::optional<std::array<byte, kTransactionStateWordBytes>> EncodeTipStateWord(const TipStateWord& w) noexcept {
  const auto state = static_cast<std::uint8_t>(w.state);
  const auto authority = static_cast<std::uint8_t>(w.authority);
  if (state > kTipStateCodeMax || (w.flags & ~kTipFlagMask) || authority > 2 || w.intent > kTipIntentMax)
    return std::nullopt;
  if (w.state == TipStateCode::unused && (w.flags || authority || w.intent || w.epoch_delta))
    return std::nullopt;
  return std::array<byte, kTransactionStateWordBytes>{
      state, w.flags, static_cast<byte>(authority | (w.intent << 2)), w.epoch_delta};
}

std::optional<TipSlotLocation> LocateTransactionStateSlot(u32 page_size, u64 transaction_order) noexcept {
  const u32 slots = TipSlotsPerPage(page_size);
  if (!slots || transaction_order == 0) return std::nullopt;
  return TipSlotLocation{transaction_order / slots, static_cast<u32>(transaction_order % slots)};
}

TipPageError FormatTransactionStatePage(const TipPageFormat& f, std::span<byte> output) noexcept {
  const u32 page_size = f.header.page_size_bytes;
  const u32 slots = TipSlotsPerPage(page_size);
  if (!slots) return E::invalid_page_size;
  if (f.header.page_type != kTransactionStatePageType || f.header.flags) return E::invalid_header;
  const auto header = disk::EncodeNativeCommonPageHeader(f.header);
  if (!header.ok()) return E::invalid_header;
  if (f.tip_sequence > std::numeric_limits<u64>::max() / slots) return E::invalid_family;
  if ((f.tip_sequence == 0) != f.counters.has_value()) return E::invalid_counters;
  if (f.counters && !HorizonsValid(*f.counters)) return E::invalid_counters;
  if (f.next_tip_page == f.header.page_number) return E::invalid_link;
  if (output.size() < page_size) return E::short_buffer;
  auto page = output.first(page_size);
  std::fill(page.begin(), page.end(), byte{0});
  byte* p = page.data();
  std::copy(header.bytes->begin(), header.bytes->end(), p);
  std::copy(kTransactionStatePageMagic.begin(), kTransactionStatePageMagic.end(), p + kMagic);
  StoreLittle16(p + kVersion, kTransactionStatePageVersion);
  StoreLittle16(p + kWordBytes, static_cast<u16>(kTransactionStateWordBytes));
  StoreLittle32(p + kSlots, slots);
  StoreLittle64(p + kSequence, f.tip_sequence);
  StoreLittle64(p + kFirstNumber, f.tip_sequence * slots);
  StoreLittle64(p + kNextPage, f.next_tip_page);
  if (f.counters) PutCounters(p, *f.counters);
  return E::none;
}

TipPageDecodeResult DecodeTransactionStatePage(std::span<const byte> image,
                                               const disk::NativeCommonPageHeaderBinding* expected) noexcept {
  TipPageDecodeResult r;
  if (image.size() < disk::kNativeCommonPageHeaderBytes) return r;
  const auto header = disk::DecodeNativeCommonPageHeader(image.data(), disk::kNativeCommonPageHeaderBytes, expected);
  if (!header.ok() || header.header->page_type != kTransactionStatePageType || header.header->flags) return r;
  if (image.size() != header.header->page_size_bytes || !TipSlotsPerPage(header.header->page_size_bytes)) {
    r.error = E::invalid_page_size;
    return r;
  }
  const auto family = ReadFamily(image);
  const byte* p = image.data();
  if (!family || !Zero(p + kFamilyEnd, kTransactionStateCountersOffset - kFamilyEnd)) {
    r.error = E::invalid_family;
    return r;
  }
  TipPageView v;
  v.header = *header.header;
  v.tip_sequence = family->sequence;
  v.first_number = family->first_number;
  v.slots = family->slots;
  v.next_tip_page = LoadLittle64(p + kNextPage);
  v.image = image;
  if (v.next_tip_page == v.header.page_number) {
    r.error = E::invalid_link;
    return r;
  }
  const bool first = v.tip_sequence == 0;
  if (!Zero(p + kCountersEnd, kTransactionStateWordsOffset - kCountersEnd) ||
      (!first && !Zero(p + kNext, kCountersEnd - kNext))) {
    r.error = E::invalid_counters;
    return r;
  }
  if (first) {
    v.counters = GetCounters(p);
    if (!HorizonsValid(*v.counters)) {
      r.error = E::invalid_counters;
      return r;
    }
  }
  for (u32 slot = 0; slot < v.slots; ++slot) {
    const auto bytes = WordBytes(image, slot);
    const auto word = DecodeTipStateWord(bytes);
    if (!word) {
      r.error = E::invalid_word;
      r.slot = slot;
      return r;
    }
    if (word->state == TipStateCode::unused) continue;
    if ((first && slot == 0) || (first && v.first_number + slot >= v.counters->next_transaction)) {
      r.error = E::invalid_slot;
      r.slot = slot;
      return r;
    }
  }
  r.error = E::none;
  r.view = v;
  return r;
}

TipPageDecodeResult CheckTransactionStateSlotsBelowNext(const TipPageView& view, u64 next_transaction) noexcept {
  TipPageDecodeResult r;
  r.error = E::none;
  r.view = view;
  if (next_transaction == 0) {
    r.error = E::invalid_counters;
    r.view.reset();
    return r;
  }
  for (u32 slot = 0; slot < view.slots; ++slot) {
    if (view.first_number + slot < next_transaction) continue;
    if (view.image[WordOffset(slot)] != 0) {
      r.error = E::invalid_slot;
      r.slot = slot;
      r.view.reset();
      return r;
    }
  }
  return r;
}

std::optional<TipStateWord> TipWordAt(const TipPageView& view, u32 slot) noexcept {
  if (slot >= view.slots || view.image.size() < WordOffset(slot) + kTransactionStateWordBytes) return std::nullopt;
  const auto bytes = WordBytes(view.image, slot);
  return DecodeTipStateWord(bytes);
}

TipPageEdit SetTransactionStateWord(std::span<byte> image, u32 slot, const TipStateWord& word) noexcept {
  const auto family = ReadFamily(image);
  if (!family) return Refuse(E::invalid_family);
  if (const auto e = CheckEditSlot(image, *family, slot); e != E::none) return Refuse(e);
  const auto encoded = EncodeTipStateWord(word);
  if (!encoded) return Refuse(E::invalid_word);
  const auto current = WordBytes(image, slot);
  // A used number never becomes unused again (inventory spec §4).
  if (word.state == TipStateCode::unused && current[0] != 0) return Refuse(E::invalid_word);
  const std::size_t at = WordOffset(slot);
  auto edit = Begin(image, at);
  if (current == *encoded) return edit;
  std::copy(encoded->begin(), encoded->end(), image.data() + at);
  edit.changed = true;
  return edit;
}

TipPageEdit SetTransactionStateCode(std::span<byte> image, u32 slot, TipStateCode code) noexcept {
  const auto family = ReadFamily(image);
  if (!family) return Refuse(E::invalid_family);
  if (const auto e = CheckEditSlot(image, *family, slot); e != E::none) return Refuse(e);
  if (static_cast<std::uint8_t>(code) > kTipStateCodeMax || code == TipStateCode::unused)
    return Refuse(E::invalid_word);
  const std::size_t at = WordOffset(slot);
  auto edit = Begin(image, at);
  if (image[at] == static_cast<byte>(code)) return edit;
  image[at] = static_cast<byte>(code);
  edit.changed = true;
  return edit;
}

TipPageEdit SetTransactionStateCounters(std::span<byte> image, const TipCounters& counters) noexcept {
  const auto family = ReadFamily(image);
  if (!family) return Refuse(E::invalid_family);
  if (family->sequence != 0 || !HorizonsValid(counters)) return Refuse(E::invalid_counters);
  const auto current = GetCounters(image.data());
  if (counters.next_transaction < current.next_transaction) return Refuse(E::next_regression);
  auto edit = Begin(image, kNext);
  if (current == counters) return edit;
  PutCounters(image.data(), counters);
  edit.changed = true;
  return edit;
}

TipPageEdit SetTransactionStateNextPage(std::span<byte> image, u64 next_tip_page) noexcept {
  const auto family = ReadFamily(image);
  if (!family) return Refuse(E::invalid_family);
  const u64 self = LoadLittle64(image.data() + 72);  // common header page_number
  const u64 current = LoadLittle64(image.data() + kNextPage);
  if (next_tip_page == 0 || next_tip_page == self) return Refuse(E::invalid_link);
  if (current != 0 && current != next_tip_page) return Refuse(E::invalid_link);
  auto edit = Begin(image, kNextPage);
  if (current == next_tip_page) return edit;
  StoreLittle64(image.data() + kNextPage, next_tip_page);
  edit.changed = true;
  return edit;
}

}  // namespace scratchbird::storage::page
