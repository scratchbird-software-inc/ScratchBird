// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "allocation_map_inplace_page.hpp"

#include <algorithm>
#include <limits>

namespace scratchbird::storage::page {
namespace {
using namespace scratchbird::core::platform;
using E = NativeAllocationError;
using S = NativeAllocationState;
constexpr std::size_t kFamily = 128, kMutable = 256, kStates = kAllocationMapInplaceStatesOffset;
constexpr std::size_t kRecord = kAllocationMapInplaceRecordBytes, kUnit = kAllocationMapInplaceWriteUnitBytes;

bool Zero(const byte* p, std::size_t n) { return std::all_of(p, p + n, [](byte b) { return b == 0; }); }
bool V7(const Uuid& id) { return !id.is_nil() && (id.bytes[6] >> 4) == 7 && (id.bytes[8] & 0xc0) == 0x80; }
Uuid GetUuid(const byte* p) {
  Uuid id;
  std::copy_n(p, 16, id.bytes.begin());
  return id;
}
void PutUuid(byte* p, const Uuid& id) { std::copy(id.bytes.begin(), id.bytes.end(), p); }
std::size_t RecordsAt(u32 slots) { return (kStates + (slots + 1) / 2 + kRecord - 1) / kRecord * kRecord; }

// The map page's own creator: exactly one of a transaction or an operation.
bool MapCreatorValid(const Uuid& transaction, u64 number, const Uuid& operation) {
  return operation.is_nil() ? V7(transaction) && number != 0 : transaction.is_nil() && V7(operation) && number == 0;
}
// A record's creator: at most one (none = allocated by the system itself).
bool RecordCreatorValid(const NativeAllocationRecord& r) {
  if (!r.creator_operation_uuid.is_nil())
    return r.creator_transaction_uuid.is_nil() && V7(r.creator_operation_uuid) && r.creator_local_transaction_id == 0;
  if (!r.creator_transaction_uuid.is_nil()) return V7(r.creator_transaction_uuid) && r.creator_local_transaction_id != 0;
  return r.creator_local_transaction_id == 0;
}
// The sealed family's record rules, with the creator rule above.
E RecordValid(const NativeAllocationRecord& r, S state, u64 page_number) {
  if (r.page_number != page_number) return E::invalid_record;
  if (!V7(r.allocation_uuid) || !V7(r.owner_uuid) || (!r.page_uuid.is_nil() && !V7(r.page_uuid)) ||
      !RecordCreatorValid(r))
    return E::invalid_identity;
  if (!r.page_type || !disk::IsRegisteredNativePageType(r.page_type)) return E::invalid_record;
  if (r.page_uuid.is_nil() != (r.page_generation == 0)) return E::invalid_record;
  if (r.page_uuid.is_nil() && state != S::reserved && state != S::preallocated && state != S::quarantined)
    return E::invalid_record;
  if ((state == S::reusable_pending_mga || state == S::reusable_free) != (r.reuse_horizon != 0)) return E::invalid_record;
  return E::none;
}
void EncodeRecord(const NativeAllocationRecord& r, byte* p) {
  std::fill(p, p + kRecord, byte{0});
  StoreLittle64(p, r.page_number);
  PutUuid(p + 8, r.allocation_uuid);
  PutUuid(p + 24, r.page_uuid);
  PutUuid(p + 40, r.owner_uuid);
  PutUuid(p + 56, r.creator_transaction_uuid);
  StoreLittle64(p + 72, r.creator_local_transaction_id);
  StoreLittle64(p + 80, r.page_generation);
  StoreLittle64(p + 88, r.reuse_horizon);
  StoreLittle32(p + 96, r.page_type);
  PutUuid(p + 100, r.creator_operation_uuid);
}
std::optional<NativeAllocationRecord> DecodeRecord(const byte* p) {
  if (!Zero(p + 116, 12)) return std::nullopt;
  NativeAllocationRecord r;
  r.page_number = LoadLittle64(p);
  r.allocation_uuid = GetUuid(p + 8);
  r.page_uuid = GetUuid(p + 24);
  r.owner_uuid = GetUuid(p + 40);
  r.creator_transaction_uuid = GetUuid(p + 56);
  r.creator_local_transaction_id = LoadLittle64(p + 72);
  r.page_generation = LoadLittle64(p + 80);
  r.reuse_horizon = LoadLittle64(p + 88);
  r.page_type = LoadLittle32(p + 96);
  r.creator_operation_uuid = GetUuid(p + 100);
  return r;
}
S StateAt(std::span<const byte> image, u32 slot) {
  return static_cast<S>((image[kStates + slot / 2] >> (4 * (slot % 2))) & 15);
}

struct Family {
  u32 slots = 0;
  std::size_t records_at = 0;
  u64 first_page = 0, total_pages = 0, page_number = 0;
};
// Re-checks the fixed family fields of an image this codec produced.
std::optional<Family> FamilyOf(std::span<const byte> image) {
  if (image.size() < kStates + kUnit) return std::nullopt;
  const auto page_size = LoadLittle32(image.data() + 12);  // common header page size
  if (page_size != image.size()) return std::nullopt;
  const auto* f = image.data() + kFamily;
  if (!std::equal(kAllocationMapInplaceMagic.begin(), kAllocationMapInplaceMagic.end(), f) ||
      LoadLittle16(f + 8) != kAllocationMapInplaceVersion || LoadLittle16(f + 10) != kRecord)
    return std::nullopt;
  Family family;
  family.slots = LoadLittle32(f + 12);
  family.records_at = LoadLittle32(f + 16);
  family.first_page = LoadLittle64(f + 48);
  family.total_pages = LoadLittle64(f + 56);
  if (!family.slots || family.slots > AllocationMapInplaceSlotsPerPage(page_size) ||
      family.records_at != RecordsAt(family.slots))
    return std::nullopt;
  return family;
}
AllocationMapInplaceEdit Refused(E error) {
  AllocationMapInplaceEdit edit;
  edit.error = error;
  return edit;
}
}  // namespace

bool IsAllocationMapInplaceImage(std::span<const byte> image) noexcept {
  return image.size() >= kFamily + 8 &&
         std::equal(kAllocationMapInplaceMagic.begin(), kAllocationMapInplaceMagic.end(), image.data() + kFamily);
}

u32 AllocationMapInplaceSlotsPerPage(u32 page_size) noexcept {
  if (page_size < kStates + kUnit || page_size % kUnit) return 0;
  u64 slots = (page_size - kStates) / kRecord;
  while (slots && RecordsAt(static_cast<u32>(slots)) + kRecord * slots > page_size) --slots;
  return static_cast<u32>(slots);
}

std::size_t AllocationMapInplaceRecordsOffset(u32 slots) noexcept { return RecordsAt(slots); }

NativeAllocationError EncodeAllocationMapInplacePage(const AllocationMapInplaceFormat& format,
                                                     std::span<const NativeAllocationState> states,
                                                     std::span<const NativeAllocationRecord> records,
                                                     std::span<byte> output) noexcept {
  try {
    const auto& h = format.header;
    const auto header = disk::EncodeNativeCommonPageHeader(h);
    if (!header.ok() || h.page_type != 3 || h.flags) return E::invalid_header;
    const u32 size = h.page_size_bytes;
    if (output.size() < size || !AllocationMapInplaceSlotsPerPage(size)) return E::resource_exhausted;
    if (!V7(format.object_uuid) || !MapCreatorValid(format.creator_transaction_uuid,
                                                    format.creator_local_transaction_id, format.creator_operation_uuid))
      return E::invalid_identity;
    if (!format.slots || format.slots > AllocationMapInplaceSlotsPerPage(size) || !format.total_pages ||
        format.total_pages > std::numeric_limits<u64>::max() / size || format.first_page >= format.total_pages ||
        format.slots > format.total_pages - format.first_page || h.page_number >= format.total_pages)
      return E::invalid_range;
    if ((format.next_page == 0) != (format.next_generation == 0) ||
        (format.next_page && (format.next_page >= format.total_pages || format.next_page == h.page_number)))
      return E::invalid_reference;
    if (!states.empty() && states.size() != format.slots) return E::invalid_range;
    // Records: ascending, one per non-free slot (a quarantined slot may have none).
    std::size_t next = 0;
    for (u32 i = 0; i < format.slots; ++i) {
      const auto state = states.empty() ? S::free : states[i];
      if (static_cast<byte>(state) > 7) return E::invalid_state;
      const u64 number = format.first_page + i;
      const bool present = next < records.size() && records[next].page_number == number;
      if (next < records.size() && records[next].page_number < number) return E::invalid_record;
      if (state == S::free) {
        if (present) return E::invalid_record;
        continue;
      }
      if (!present) {
        if (state != S::quarantined) return E::invalid_record;
        continue;
      }
      if (const auto valid = RecordValid(records[next], state, number); valid != E::none) return valid;
      ++next;
    }
    if (next != records.size()) return E::invalid_record;
    auto bytes = output.first(size);
    std::fill(bytes.begin(), bytes.end(), byte{0});
    std::copy(header.bytes->begin(), header.bytes->end(), bytes.begin());
    auto* f = bytes.data() + kFamily;
    std::copy(kAllocationMapInplaceMagic.begin(), kAllocationMapInplaceMagic.end(), f);
    StoreLittle16(f + 8, kAllocationMapInplaceVersion);
    StoreLittle16(f + 10, static_cast<u16>(kRecord));
    StoreLittle32(f + 12, format.slots);
    StoreLittle32(f + 16, static_cast<u32>(RecordsAt(format.slots)));
    PutUuid(f + 24, format.object_uuid);
    StoreLittle64(f + 40, format.sequence);
    StoreLittle64(f + 48, format.first_page);
    StoreLittle64(f + 56, format.total_pages);
    PutUuid(f + 64, format.creator_transaction_uuid);
    StoreLittle64(f + 80, format.creator_local_transaction_id);
    PutUuid(f + 88, format.creator_operation_uuid);
    StoreLittle64(bytes.data() + kMutable, format.next_page);
    StoreLittle64(bytes.data() + kMutable + 8, format.next_generation);
    for (u32 i = 0; i < format.slots && !states.empty(); ++i)
      bytes[kStates + i / 2] |= static_cast<byte>(static_cast<byte>(states[i]) << (4 * (i % 2)));
    const auto records_at = RecordsAt(format.slots);
    for (const auto& r : records)
      EncodeRecord(r, bytes.data() + records_at + kRecord * static_cast<std::size_t>(r.page_number - format.first_page));
    return E::none;
  } catch (...) {
    return E::resource_exhausted;
  }
}

AllocationMapInplaceDecodeResult DecodeAllocationMapInplaceInto(std::span<const byte> image,
                                                                std::span<NativeAllocationState> states,
                                                                std::span<NativeAllocationRecord> records,
                                                                const disk::NativeCommonPageHeaderBinding* expected) noexcept {
  AllocationMapInplaceDecodeResult out;
  const auto fail = [&](E error) {
    out.error = error;
    out.view.reset();
    return out;
  };
  try {
    if (image.size() < kFamily) return fail(E::invalid_header);
    const auto common = disk::DecodeNativeCommonPageHeader(image.data(), std::min<std::size_t>(image.size(), 128), expected);
    if (!common.ok() || common.header->page_type != 3 || common.header->page_size_bytes != image.size())
      return fail(expected && !common.ok() ? E::binding_mismatch : E::invalid_header);
    if (common.header->flags) return fail(E::invalid_header);
    const auto family = FamilyOf(image);
    if (!family) return fail(E::invalid_family);
    const auto* f = image.data() + kFamily;
    if (!Zero(f + 20, 4) || !Zero(f + 104, 24) || !Zero(image.data() + kMutable + 16, kStates - kMutable - 16))
      return fail(E::invalid_family);
    if (states.size() < family->slots || records.size() < family->slots) return fail(E::resource_exhausted);
    AllocationMapInplaceView view;
    auto& map = view.map;
    map.header = *common.header;
    map.object_uuid = GetUuid(f + 24);
    map.map_generation = map.capacity_generation = 1;
    map.total_pages = family->total_pages;
    map.first_page = family->first_page;
    map.creator_transaction_uuid = GetUuid(f + 64);
    map.creator_local_transaction_id = LoadLittle64(f + 80);
    map.creator_operation_uuid = GetUuid(f + 88);
    view.sequence = LoadLittle64(f + 40);
    view.slots = family->slots;
    view.next_page = LoadLittle64(image.data() + kMutable);
    view.next_generation = LoadLittle64(image.data() + kMutable + 8);
    const auto page_size = static_cast<u32>(image.size());
    if (!V7(map.object_uuid) ||
        !MapCreatorValid(map.creator_transaction_uuid, map.creator_local_transaction_id, map.creator_operation_uuid))
      return fail(E::invalid_identity);
    if (!map.total_pages || map.total_pages > std::numeric_limits<u64>::max() / page_size ||
        map.first_page >= map.total_pages || view.slots > map.total_pages - map.first_page ||
        map.header.page_number >= map.total_pages)
      return fail(E::invalid_range);
    if ((view.next_page == 0) != (view.next_generation == 0) ||
        (view.next_page && (view.next_page >= map.total_pages || view.next_page == map.header.page_number)))
      return fail(E::invalid_reference);
    if (view.next_page)
      map.next = disk::NativePageReference{map.header.filespace_uuid, view.next_page, view.next_generation,
                                           map.header.page_size_profile_uuid};
    // States: valid codes; the unused high nibble and the padding are zero.
    const auto state_bytes = (view.slots + 1) / 2;
    if ((view.slots & 1) && (image[kStates + view.slots / 2] & 0xf0)) return fail(E::invalid_family);
    if (!Zero(image.data() + kStates + state_bytes, family->records_at - kStates - state_bytes))
      return fail(E::invalid_family);
    const auto records_end = family->records_at + kRecord * view.slots;
    if (!Zero(image.data() + records_end, image.size() - records_end)) return fail(E::invalid_family);
    std::size_t count = 0;
    for (u32 i = 0; i < view.slots; ++i) {
      const auto state = StateAt(image, i);
      if (static_cast<byte>(state) > 7) return fail(E::invalid_state);
      states[i] = state;
      const auto* p = image.data() + family->records_at + kRecord * i;
      const u64 number = map.first_page + i;
      if (Zero(p, kRecord)) {
        if (state != S::free && state != S::quarantined) return fail(E::invalid_record);
        continue;
      }
      const auto record = DecodeRecord(p);
      if (!record) return fail(E::invalid_record);
      if (state == S::free) {
        // A crash leftover: written for an allocation whose state never followed.
        if (const auto valid = RecordValid(*record, S::allocated, number); valid != E::none) return fail(valid);
        ++view.leftover_records;
        continue;
      }
      if (const auto valid = RecordValid(*record, state, number); valid != E::none) return fail(valid);
      records[count++] = *record;
    }
    map.states = states.first(view.slots);
    map.records = records.first(count);
    out.view = view;
    out.error = E::none;
    return out;
  } catch (...) {
    return fail(E::resource_exhausted);
  }
}

AllocationMapInplaceEdit WriteAllocationMapInplaceRecord(std::span<byte> image, u32 slot,
                                                         const NativeAllocationRecord& record) noexcept {
  const auto family = FamilyOf(image);
  if (!family) return Refused(E::invalid_family);
  if (slot >= family->slots) return Refused(E::invalid_range);
  if (StateAt(image, slot) != S::free) return Refused(E::invalid_state);
  if (const auto valid = RecordValid(record, S::allocated, family->first_page + slot); valid != E::none)
    return Refused(valid);
  const auto at = family->records_at + kRecord * slot;
  std::array<byte, kRecord> encoded{};
  EncodeRecord(record, encoded.data());
  AllocationMapInplaceEdit edit;
  edit.unit_offset = static_cast<u32>(at / kUnit * kUnit);
  std::copy_n(image.data() + edit.unit_offset, kUnit, edit.preimage.begin());
  edit.changed = !std::equal(encoded.begin(), encoded.end(), image.data() + at);
  std::copy(encoded.begin(), encoded.end(), image.data() + at);
  edit.error = E::none;
  return edit;
}

AllocationMapInplaceEdit SetAllocationMapInplaceAllocated(std::span<byte> image, u32 slot) noexcept {
  const auto family = FamilyOf(image);
  if (!family) return Refused(E::invalid_family);
  if (slot >= family->slots) return Refused(E::invalid_range);
  const auto state = StateAt(image, slot);
  const auto at = kStates + slot / 2;
  AllocationMapInplaceEdit edit;
  edit.unit_offset = static_cast<u32>(at / kUnit * kUnit);
  if (state == S::allocated) {
    edit.error = E::none;  // already allocated: a no-op
    std::copy_n(image.data() + edit.unit_offset, kUnit, edit.preimage.begin());
    return edit;
  }
  if (state != S::free) return Refused(E::invalid_state);
  const auto record = DecodeRecord(image.data() + family->records_at + kRecord * slot);
  if (!record || Zero(image.data() + family->records_at + kRecord * slot, kRecord) ||
      RecordValid(*record, S::allocated, family->first_page + slot) != E::none)
    return Refused(E::invalid_record);
  std::copy_n(image.data() + edit.unit_offset, kUnit, edit.preimage.begin());
  const auto shift = 4 * (slot % 2);
  image[at] = static_cast<byte>((image[at] & ~(0x0f << shift)) | (static_cast<byte>(S::allocated) << shift));
  edit.changed = true;
  edit.error = E::none;
  return edit;
}

AllocationMapInplaceEdit SetAllocationMapInplaceNext(std::span<byte> image, u64 next_page,
                                                     u64 next_generation) noexcept {
  const auto family = FamilyOf(image);
  if (!family) return Refused(E::invalid_family);
  const auto own = LoadLittle64(image.data() + 72);  // common header page number
  if (!next_page || !next_generation || next_page >= family->total_pages || next_page == own)
    return Refused(E::invalid_reference);
  const auto current = LoadLittle64(image.data() + kMutable);
  const auto current_generation = LoadLittle64(image.data() + kMutable + 8);
  AllocationMapInplaceEdit edit;
  edit.unit_offset = static_cast<u32>(kMutable / kUnit * kUnit);
  std::copy_n(image.data() + edit.unit_offset, kUnit, edit.preimage.begin());
  if (current || current_generation) {
    if (current != next_page || current_generation != next_generation) return Refused(E::invalid_reference);
    edit.error = E::none;
    return edit;
  }
  StoreLittle64(image.data() + kMutable, next_page);
  StoreLittle64(image.data() + kMutable + 8, next_generation);
  edit.changed = true;
  edit.error = E::none;
  return edit;
}

}  // namespace scratchbird::storage::page
