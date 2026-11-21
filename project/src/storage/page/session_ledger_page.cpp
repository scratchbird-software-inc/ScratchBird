// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "session_ledger_page.hpp"

#include "transaction_header_page.hpp"

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

// The SBTXHD01 geometry (the same family offsets), so a ledger page and a
// header page differ only by their magic and their record codec.
constexpr std::size_t kFamily = kTransactionStateFamilyOffset;  // 128
constexpr std::size_t kMagic = kFamily;
constexpr std::size_t kVersion = kFamily + 8;
constexpr std::size_t kRecordBytes = kFamily + 10;
constexpr std::size_t kSlots = kFamily + 12;
constexpr std::size_t kSequence = kFamily + 16;
constexpr std::size_t kFirstOrdinal = kFamily + 24;
constexpr std::size_t kFamilyEnd = kFamily + 32;
constexpr std::size_t kNextPage = 256;
constexpr std::size_t kLinkEnd = 264;
constexpr std::size_t kRecords = kSessionLedgerPageRecordsOffset;
constexpr std::size_t kRecord = kSessionLedgerRecordBytes;

bool Zero(const byte* data, std::size_t size) {
  return std::all_of(data, data + size, [](byte b) { return b == 0; });
}
std::size_t RecordOffset(u32 slot) { return kRecords + kRecord * slot; }

struct Family {
  u32 slots = 0;
  u64 sequence = 0;
  u64 first_ordinal = 0;
};
std::optional<Family> ReadFamily(std::span<const byte> image) {
  if (image.size() < kRecords) return std::nullopt;
  const byte* p = image.data();
  const u32 page_size = LoadLittle32(p + 12);  // common header page_size_bytes
  const u32 slots = SessionLedgerSlotsPerPage(page_size);
  if (!slots || image.size() != page_size) return std::nullopt;
  if (!std::equal(kSessionLedgerPageMagic.begin(), kSessionLedgerPageMagic.end(), p + kMagic) ||
      LoadLittle16(p + kVersion) != kSessionLedgerPageVersion || LoadLittle16(p + kRecordBytes) != kRecord ||
      LoadLittle32(p + kSlots) != slots)
    return std::nullopt;
  const u64 sequence = LoadLittle64(p + kSequence);
  if (sequence > std::numeric_limits<u64>::max() / slots || LoadLittle64(p + kFirstOrdinal) != sequence * slots)
    return std::nullopt;
  return Family{slots, sequence, sequence * slots};
}
Uuid PageDatabase(std::span<const byte> image) {
  Uuid id;
  std::copy_n(image.data() + 24, 16, id.bytes.begin());  // common header database_uuid
  return id;
}

SessionLedgerAppendEdit Refuse(E error) {
  SessionLedgerAppendEdit edit;
  edit.error = error;
  return edit;
}
void BeginEdit(std::span<byte> image, std::size_t changed_offset, TipPageEdit& edit) {
  edit.error = E::none;
  edit.unit_offset = static_cast<u32>(changed_offset - changed_offset % kTransactionStateWriteUnitBytes);
  std::copy_n(image.data() + edit.unit_offset, kTransactionStateWriteUnitBytes, edit.preimage.begin());
}
}  // namespace

u32 SessionLedgerSlotsPerPage(u32 page_size) noexcept {
  // The same geometry as SBTXHD01 (512-byte prefix, 256-byte records).
  return TransactionHeaderSlotsPerPage(page_size);
}

std::optional<SessionLedgerSlotLocation> LocateSessionLedgerSlot(u32 page_size, u64 ordinal) noexcept {
  const u32 slots = SessionLedgerSlotsPerPage(page_size);
  if (!slots) return std::nullopt;
  return SessionLedgerSlotLocation{ordinal / slots, static_cast<u32>(ordinal % slots)};
}

TipPageError FormatSessionLedgerPage(const SessionLedgerPageFormat& f, std::span<byte> output) noexcept {
  const u32 page_size = f.header.page_size_bytes;
  const u32 slots = SessionLedgerSlotsPerPage(page_size);
  if (!slots) return E::invalid_page_size;
  if (f.header.page_type != kTransactionStatePageType || f.header.flags) return E::invalid_header;
  const auto header = disk::EncodeNativeCommonPageHeader(f.header);
  if (!header.ok()) return E::invalid_header;
  if (f.sequence > std::numeric_limits<u64>::max() / slots) return E::invalid_family;
  if (f.next_page == f.header.page_number) return E::invalid_link;
  if (output.size() < page_size) return E::short_buffer;
  auto page = output.first(page_size);
  std::fill(page.begin(), page.end(), byte{0});
  byte* p = page.data();
  std::copy(header.bytes->begin(), header.bytes->end(), p);
  std::copy(kSessionLedgerPageMagic.begin(), kSessionLedgerPageMagic.end(), p + kMagic);
  StoreLittle16(p + kVersion, kSessionLedgerPageVersion);
  StoreLittle16(p + kRecordBytes, static_cast<u16>(kRecord));
  StoreLittle32(p + kSlots, slots);
  StoreLittle64(p + kSequence, f.sequence);
  StoreLittle64(p + kFirstOrdinal, f.sequence * slots);
  StoreLittle64(p + kNextPage, f.next_page);
  return E::none;
}

bool IsSessionLedgerPageImage(std::span<const byte> image) noexcept {
  return image.size() >= kMagic + kSessionLedgerPageMagic.size() &&
         std::equal(kSessionLedgerPageMagic.begin(), kSessionLedgerPageMagic.end(), image.data() + kMagic);
}

SessionLedgerPageDecodeResult DecodeSessionLedgerPage(std::span<const byte> image,
                                                      const disk::NativeCommonPageHeaderBinding* expected) noexcept {
  SessionLedgerPageDecodeResult r;
  if (image.size() < disk::kNativeCommonPageHeaderBytes) return r;
  const auto header = disk::DecodeNativeCommonPageHeader(image.data(), disk::kNativeCommonPageHeaderBytes, expected);
  if (!header.ok() || header.header->page_type != kTransactionStatePageType || header.header->flags) return r;
  if (image.size() != header.header->page_size_bytes || !SessionLedgerSlotsPerPage(header.header->page_size_bytes)) {
    r.error = E::invalid_page_size;
    return r;
  }
  const auto family = ReadFamily(image);
  const byte* p = image.data();
  if (!family || !Zero(p + kFamilyEnd, kNextPage - kFamilyEnd)) {
    r.error = E::invalid_family;
    return r;
  }
  SessionLedgerPageView v;
  v.header = *header.header;
  v.sequence = family->sequence;
  v.first_ordinal = family->first_ordinal;
  v.slots = family->slots;
  v.next_page = LoadLittle64(p + kNextPage);
  v.image = image;
  if (v.next_page == v.header.page_number) {
    r.error = E::invalid_link;
    return r;
  }
  if (!Zero(p + kLinkEnd, kRecords - kLinkEnd)) {
    r.error = E::invalid_counters;
    return r;
  }
  u32 slot = 0;
  for (; slot < v.slots; ++slot) {
    const auto bytes = image.subspan(RecordOffset(slot), kRecord);
    if (Zero(bytes.data(), bytes.size())) break;  // the first empty slot
    const auto record = DecodeSessionLedgerRecord(bytes);
    if (record.ok()) {
      if (record.record->ordinal != v.first_ordinal + slot || record.record->database_uuid != v.header.database_uuid) {
        r.error = E::invalid_slot;
        r.slot = slot;
        return r;
      }
      continue;
    }
    if (record.error != SessionLedgerRecordError::invalid_checksum) {
      r.error = E::invalid_word;
      r.slot = slot;
      r.record_error = record.error;
      return r;
    }
    v.torn_slot = slot;  // the one torn slot a crash can leave
    break;
  }
  v.used = slot;
  // After the first empty or torn slot every slot is empty.
  for (u32 later = v.torn_slot ? slot + 1 : slot; later < v.slots; ++later) {
    if (!Zero(image.data() + RecordOffset(later), kRecord)) {
      r.error = E::invalid_slot;
      r.slot = later;
      return r;
    }
  }
  // A page with a successor is full: the link is written only after the page fills.
  if (v.next_page != 0 && v.used != v.slots) {
    r.error = E::invalid_link;
    r.slot = v.used;
    return r;
  }
  r.error = E::none;
  r.view = v;
  return r;
}

std::optional<SessionLedgerRecord> SessionLedgerRecordAt(const SessionLedgerPageView& view, u32 slot) noexcept {
  if (slot >= view.used || view.image.size() < RecordOffset(slot) + kRecord) return std::nullopt;
  const auto record = DecodeSessionLedgerRecord(view.image.subspan(RecordOffset(slot), kRecord));
  if (!record.ok()) return std::nullopt;
  return record.record;
}

SessionLedgerAppendEdit AppendSessionLedgerRecord(std::span<byte> image, const SessionLedgerRecord& record) noexcept {
  const auto decoded = DecodeSessionLedgerPage(image);
  if (!decoded.ok()) return Refuse(decoded.error == E::none ? E::invalid_family : decoded.error);
  const auto& view = *decoded.view;
  if (view.used >= view.slots || view.next_page != 0) return Refuse(E::invalid_slot);  // full: grow first
  const u32 slot = view.used;
  if (record.ordinal != view.first_ordinal + slot || record.database_uuid != view.header.database_uuid)
    return Refuse(E::invalid_slot);
  std::array<byte, kRecord> encoded{};
  if (EncodeSessionLedgerRecord(record, encoded) != SessionLedgerRecordError::none) return Refuse(E::invalid_word);
  const std::size_t at = RecordOffset(slot);
  SessionLedgerAppendEdit edit;
  BeginEdit(image, at, edit);
  edit.slot = slot;
  std::copy(encoded.begin(), encoded.end(), image.data() + at);
  edit.changed = true;
  return edit;
}

TipPageEdit SetSessionLedgerNextPage(std::span<byte> image, u64 next_page) noexcept {
  const auto decoded = DecodeSessionLedgerPage(image);
  if (!decoded.ok()) return Refuse(E::invalid_family);
  const auto& view = *decoded.view;
  const u64 current = view.next_page;
  if (next_page == 0 || next_page == view.header.page_number) return Refuse(E::invalid_link);
  if (current != 0 && current != next_page) return Refuse(E::invalid_link);
  if (current == 0 && (view.used != view.slots || view.torn_slot)) return Refuse(E::invalid_link);  // link only a full page
  SessionLedgerAppendEdit edit;
  BeginEdit(image, kNextPage, edit);
  if (current != next_page) {
    StoreLittle64(image.data() + kNextPage, next_page);
    edit.changed = true;
  }
  return edit;
}

}  // namespace scratchbird::storage::page
