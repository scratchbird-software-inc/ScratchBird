// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
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

constexpr std::size_t kFamily = kTransactionStateFamilyOffset;  // 128
constexpr std::size_t kMagic = kFamily;
constexpr std::size_t kVersion = kFamily + 8;
constexpr std::size_t kRecordBytes = kFamily + 10;
constexpr std::size_t kSlots = kFamily + 12;
constexpr std::size_t kSequence = kFamily + 16;
constexpr std::size_t kFirstNumber = kFamily + 24;
constexpr std::size_t kFamilyEnd = kFamily + 32;
constexpr std::size_t kNextPage = 256;
constexpr std::size_t kLinkEnd = 264;
constexpr std::size_t kRecords = kTransactionHeaderPageRecordsOffset;
constexpr std::size_t kRecord = kTransactionHeaderRecordBytes;

bool Zero(const byte* data, std::size_t size) {
  return std::all_of(data, data + size, [](byte b) { return b == 0; });
}
std::size_t RecordOffset(u32 slot) { return kRecords + kRecord * slot; }

struct Family {
  u32 slots = 0;
  u64 sequence = 0;
  u64 first_number = 0;
};
std::optional<Family> ReadFamily(std::span<const byte> image) {
  if (image.size() < kRecords) return std::nullopt;
  const byte* p = image.data();
  const u32 page_size = LoadLittle32(p + 12);  // common header page_size_bytes
  const u32 slots = TransactionHeaderSlotsPerPage(page_size);
  if (!slots || image.size() != page_size) return std::nullopt;
  if (!std::equal(kTransactionHeaderPageMagic.begin(), kTransactionHeaderPageMagic.end(), p + kMagic) ||
      LoadLittle16(p + kVersion) != kTransactionHeaderPageVersion || LoadLittle16(p + kRecordBytes) != kRecord ||
      LoadLittle32(p + kSlots) != slots)
    return std::nullopt;
  const u64 sequence = LoadLittle64(p + kSequence);
  if (sequence > std::numeric_limits<u64>::max() / slots || LoadLittle64(p + kFirstNumber) != sequence * slots)
    return std::nullopt;
  return Family{slots, sequence, sequence * slots};
}
scratchbird::core::platform::Uuid PageDatabase(std::span<const byte> image) {
  scratchbird::core::platform::Uuid id;
  std::copy_n(image.data() + 24, 16, id.bytes.begin());  // common header database_uuid
  return id;
}

TipPageEdit Refuse(E error) {
  TipPageEdit edit;
  edit.error = error;
  return edit;
}
TipPageEdit Begin(std::span<byte> image, std::size_t changed_offset) {
  TipPageEdit edit;
  edit.error = E::none;
  edit.unit_offset = static_cast<u32>(changed_offset - changed_offset % kTransactionStateWriteUnitBytes);
  std::copy_n(image.data() + edit.unit_offset, kTransactionStateWriteUnitBytes, edit.preimage.begin());
  return edit;
}

// The fixed fields of a header (everything but intent, intent generation and
// the completion fields) are equal.
bool SameFixedFields(const TransactionHeaderRecord& a, const TransactionHeaderRecord& b) {
  auto x = a, y = b;
  for (auto* r : {&x, &y}) {
    r->intent = 0;
    r->intent_generation = 0;
    r->final_state = TipStateCode::unused;
    r->commit_time_utc_ns = 0;
    r->completed_at_utc_ns = 0;
  }
  return x == y;
}
}  // namespace

u32 TransactionHeaderSlotsPerPage(u32 page_size) noexcept {
  if (page_size <= kRecords || page_size % kTransactionStateWriteUnitBytes) return 0;
  return static_cast<u32>((page_size - kRecords) / kRecord);
}

std::optional<TransactionHeaderSlotLocation> LocateTransactionHeaderSlot(u32 page_size,
                                                                         u64 transaction_order) noexcept {
  const u32 slots = TransactionHeaderSlotsPerPage(page_size);
  if (!slots || transaction_order == 0) return std::nullopt;
  return TransactionHeaderSlotLocation{transaction_order / slots, static_cast<u32>(transaction_order % slots)};
}

TipPageError FormatTransactionHeaderPage(const TransactionHeaderPageFormat& f, std::span<byte> output) noexcept {
  const u32 page_size = f.header.page_size_bytes;
  const u32 slots = TransactionHeaderSlotsPerPage(page_size);
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
  std::copy(kTransactionHeaderPageMagic.begin(), kTransactionHeaderPageMagic.end(), p + kMagic);
  StoreLittle16(p + kVersion, kTransactionHeaderPageVersion);
  StoreLittle16(p + kRecordBytes, static_cast<u16>(kRecord));
  StoreLittle32(p + kSlots, slots);
  StoreLittle64(p + kSequence, f.sequence);
  StoreLittle64(p + kFirstNumber, f.sequence * slots);
  StoreLittle64(p + kNextPage, f.next_page);
  return E::none;
}

bool IsTransactionHeaderPageImage(std::span<const byte> image) noexcept {
  return image.size() >= kMagic + kTransactionHeaderPageMagic.size() &&
         std::equal(kTransactionHeaderPageMagic.begin(), kTransactionHeaderPageMagic.end(), image.data() + kMagic);
}

TransactionHeaderPageDecodeResult DecodeTransactionHeaderPage(
    std::span<const byte> image, const disk::NativeCommonPageHeaderBinding* expected) noexcept {
  TransactionHeaderPageDecodeResult r;
  if (image.size() < disk::kNativeCommonPageHeaderBytes) return r;
  const auto header = disk::DecodeNativeCommonPageHeader(image.data(), disk::kNativeCommonPageHeaderBytes, expected);
  if (!header.ok() || header.header->page_type != kTransactionStatePageType || header.header->flags) return r;
  if (image.size() != header.header->page_size_bytes || !TransactionHeaderSlotsPerPage(header.header->page_size_bytes)) {
    r.error = E::invalid_page_size;
    return r;
  }
  const auto family = ReadFamily(image);
  const byte* p = image.data();
  if (!family || !Zero(p + kFamilyEnd, kNextPage - kFamilyEnd)) {
    r.error = E::invalid_family;
    return r;
  }
  TransactionHeaderPageView v;
  v.header = *header.header;
  v.sequence = family->sequence;
  v.first_number = family->first_number;
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
  for (u32 slot = 0; slot < v.slots; ++slot) {
    const auto bytes = image.subspan(RecordOffset(slot), kRecord);
    if (Zero(bytes.data(), bytes.size())) continue;
    const auto record = DecodeTransactionHeaderRecord(bytes);
    if (!record.ok()) {
      r.error = E::invalid_word;
      r.slot = slot;
      return r;
    }
    if (record.record->transaction_order != v.first_number + slot ||
        record.record->database_uuid != v.header.database_uuid) {
      r.error = E::invalid_slot;
      r.slot = slot;
      return r;
    }
  }
  r.error = E::none;
  r.view = v;
  return r;
}

std::optional<TransactionHeaderRecord> TransactionHeaderAt(const TransactionHeaderPageView& view, u32 slot) noexcept {
  if (slot >= view.slots || view.image.size() < RecordOffset(slot) + kRecord) return std::nullopt;
  const auto bytes = view.image.subspan(RecordOffset(slot), kRecord);
  if (Zero(bytes.data(), bytes.size())) return std::nullopt;
  const auto record = DecodeTransactionHeaderRecord(bytes);
  if (!record.ok()) return std::nullopt;
  return record.record;
}

TipPageEdit WriteTransactionHeader(std::span<byte> image, const TransactionHeaderRecord& record) noexcept {
  const auto family = ReadFamily(image);
  if (!family) return Refuse(E::invalid_family);
  if (record.transaction_order < family->first_number ||
      record.transaction_order - family->first_number >= family->slots || record.database_uuid != PageDatabase(image))
    return Refuse(E::invalid_slot);
  std::array<byte, kRecord> encoded{};
  if (EncodeTransactionHeaderRecord(record, encoded) != TransactionHeaderError::none) return Refuse(E::invalid_word);
  const auto slot = static_cast<u32>(record.transaction_order - family->first_number);
  const std::size_t at = RecordOffset(slot);
  auto edit = Begin(image, at);
  if (std::equal(encoded.begin(), encoded.end(), image.data() + at)) return edit;
  if (!Zero(image.data() + at, kRecord)) return Refuse(E::invalid_slot);  // written once
  std::copy(encoded.begin(), encoded.end(), image.data() + at);
  edit.changed = true;
  return edit;
}

TipPageEdit UpdateTransactionHeader(std::span<byte> image, const TransactionHeaderRecord& record) noexcept {
  const auto family = ReadFamily(image);
  if (!family) return Refuse(E::invalid_family);
  if (record.transaction_order < family->first_number ||
      record.transaction_order - family->first_number >= family->slots)
    return Refuse(E::invalid_slot);
  const auto slot = static_cast<u32>(record.transaction_order - family->first_number);
  const std::size_t at = RecordOffset(slot);
  const auto current = DecodeTransactionHeaderRecord(std::span<const byte>(image.data() + at, kRecord));
  if (!current.ok()) return Refuse(E::invalid_slot);  // empty or unreadable: BEGIN writes first
  const auto& old = *current.record;
  if (!SameFixedFields(old, record)) return Refuse(E::invalid_word);
  // Completion happens once; after it nothing mutable changes.
  if (old.final_state != TipStateCode::unused &&
      (record.final_state != old.final_state || record.commit_time_utc_ns != old.commit_time_utc_ns ||
       record.completed_at_utc_ns != old.completed_at_utc_ns || record.intent != old.intent ||
       record.intent_generation != old.intent_generation))
    return Refuse(E::invalid_word);
  // The intent generation only increases, and an intent change needs a new generation.
  if (record.intent_generation < old.intent_generation ||
      (record.intent_generation == old.intent_generation && record.intent != old.intent))
    return Refuse(E::invalid_word);
  std::array<byte, kRecord> encoded{};
  if (EncodeTransactionHeaderRecord(record, encoded) != TransactionHeaderError::none) return Refuse(E::invalid_word);
  auto edit = Begin(image, at);
  if (std::equal(encoded.begin(), encoded.end(), image.data() + at)) return edit;
  std::copy(encoded.begin(), encoded.end(), image.data() + at);
  edit.changed = true;
  return edit;
}

TipPageEdit SetTransactionHeaderNextPage(std::span<byte> image, u64 next_page) noexcept {
  const auto family = ReadFamily(image);
  if (!family) return Refuse(E::invalid_family);
  const u64 self = LoadLittle64(image.data() + 72);  // common header page_number
  const u64 current = LoadLittle64(image.data() + kNextPage);
  if (next_page == 0 || next_page == self) return Refuse(E::invalid_link);
  if (current != 0 && current != next_page) return Refuse(E::invalid_link);
  auto edit = Begin(image, kNextPage);
  if (current == next_page) return edit;
  StoreLittle64(image.data() + kNextPage, next_page);
  edit.changed = true;
  return edit;
}

}  // namespace scratchbird::storage::page
