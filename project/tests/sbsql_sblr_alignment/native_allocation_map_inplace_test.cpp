// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_ALLOCATION_MAP_IN_PLACE
// The in-place allocation map family (owner Q22 = 1, 2026-11-07), at every
// page size:
//   - the codec: slots per page against an independent formula; a page with
//     every state round-trips; every structural fault is refused; a record
//     over a free slot is a crash leftover, counted and not reported;
//   - every edit changes bytes only inside the 512-byte unit it reports, with
//     the right preimage; the image before and after decodes, and so does each
//     half-and-half tear of the unit (old or new, never invalid);
//   - the production create (fixed roots) writes the family on every map page
//     and the chain reader reads it: coverage, sequence, the create's records
//     (every page it wrote, created by transaction 1), page zero's counters not
//     compared (until W4d the drift-era checkpoint graph wrote the sealed
//     family; W4d-3b removed that family, its codec and the selected and
//     historical reads, owner Q28 = 1);
//   - forgeries on the created file: a head map page of another family is an
//     unknown family, a later one breaks the chain; a later page naming
//     another creator, or linking back into the chain, breaks it; a state
//     that claims a page without a record is refused.
#include "native_database_create.hpp"
#include "native_catalog_relation_lease_reader.hpp"
#include "admitted_datatype_cohort.hpp"
#include "datatype_catalog_manifest.hpp"
#include "native_creation_workspace.hpp"
#include "native_fixed_root_memory_lease.hpp"
#include "allocation_map_inplace_page.hpp"
#include "native_allocation_map.hpp"
#include "uuid.hpp"
#include "../support/owned_temp_directory.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace db = scratchbird::storage::database;
namespace d = scratchbird::storage::disk;
namespace pg = scratchbird::storage::page;
namespace mga = scratchbird::transaction::mga;
namespace u = scratchbird::core::uuid;
using namespace scratchbird::core::platform;
using S = pg::NativeAllocationState;
using AE = pg::NativeAllocationError;

unsigned checks = 0, mixed_refused = 0;
void Check(bool ok, std::string_view why, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) throw std::runtime_error(std::string(why) + " (line " + std::to_string(at.line()) + ")");
}
Uuid V7(unsigned n) {
  Uuid id;
  id.bytes[0] = 1;
  id.bytes[6] = 0x70;
  id.bytes[8] = 0x80;
  id.bytes[13] = static_cast<byte>(n >> 16);
  id.bytes[14] = static_cast<byte>(n >> 8);
  id.bytes[15] = static_cast<byte>(n);
  return id;
}
constexpr const char* kFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";

// Independent of the codec: the largest N with
// roundup128(512 + ceil(N / 2)) + 128 N <= P.
u32 ExpectedSlots(u32 page_size) {
  u32 best = 0;
  for (u32 n = 1; n < page_size; ++n) {
    const u64 states_end = 512 + (n + 1) / 2;
    const u64 records_at = (states_end + 127) / 128 * 128;
    if (records_at + 128ull * n <= page_size) best = n;
  }
  return best;
}

pg::NativeAllocationRecord Record(u64 page, unsigned salt) {
  pg::NativeAllocationRecord r;
  r.page_number = page;
  r.allocation_uuid = V7(100000 + salt);
  r.page_uuid = V7(200000 + salt);
  r.owner_uuid = V7(300000 + salt);
  r.creator_transaction_uuid = V7(7);
  r.creator_local_transaction_id = 1;
  r.page_generation = 1;
  r.page_type = 6;
  return r;
}
pg::AllocationMapInplaceFormat Format(u32 page_size, const Uuid& profile, u64 page_number, u32 slots) {
  pg::AllocationMapInplaceFormat f;
  f.header = {page_size, 3, V7(1), V7(2), V7(3), page_number, 1, 0, profile};
  f.object_uuid = V7(4);
  f.sequence = 0;
  f.first_page = 0;
  f.slots = slots;
  f.total_pages = slots + 10;
  f.creator_transaction_uuid = V7(7);
  f.creator_local_transaction_id = 1;
  return f;
}
pg::AllocationMapInplaceDecodeResult Decode(std::span<const byte> image, u32 slots) {
  static std::vector<S> states;
  static std::vector<pg::NativeAllocationRecord> records;
  states.assign(slots, S::free);
  records.assign(slots, {});
  return pg::DecodeAllocationMapInplaceInto(image, states, records);
}

// ---- The codec -------------------------------------------------------------
void Codec(const d::CanonicalFilespacePageProfile& p) {
  const u32 size = p.page_size_bytes;
  const u32 slots = pg::AllocationMapInplaceSlotsPerPage(size);
  Check(slots == ExpectedSlots(size) && slots > 8, "slots per page follow the layout formula");
  const auto records_at = pg::AllocationMapInplaceRecordsOffset(slots);
  Check(records_at % 128 == 0 && records_at + 128ull * slots <= size, "records sit on 128-byte boundaries and fit");
  // A page with every kind of slot.
  auto f = Format(size, p.uuid, 3, slots);
  std::vector<S> states(slots, S::free);
  std::vector<pg::NativeAllocationRecord> records;
  states[0] = S::allocated;
  records.push_back(Record(0, 0));
  states[1] = S::reserved;
  {
    auto r = Record(1, 1);
    r.page_uuid = {};
    r.page_generation = 0;
    records.push_back(r);
  }
  states[2] = S::quarantined;  // no record
  states[3] = S::reusable_free;
  {
    auto r = Record(3, 3);
    r.reuse_horizon = 9;
    records.push_back(r);
  }
  states[4] = S::allocated;
  {
    auto r = Record(4, 4);  // allocated by the system: no creator
    r.creator_transaction_uuid = {};
    r.creator_local_transaction_id = 0;
    records.push_back(r);
  }
  states[5] = S::allocated;
  {
    auto r = Record(5, 5);  // allocated by an operation
    r.creator_transaction_uuid = {};
    r.creator_local_transaction_id = 0;
    r.creator_operation_uuid = V7(55);
    records.push_back(r);
  }
  std::vector<byte> image(size);
  Check(pg::EncodeAllocationMapInplacePage(f, states, records, image) == AE::none, "a page with every slot kind encodes");
  Check(pg::IsAllocationMapInplaceImage(image), "the family magic identifies it");
  const auto decoded = Decode(image, slots);
  Check(decoded.ok() && decoded.view->slots == slots && decoded.view->map.first_page == 0 &&
            decoded.view->map.map_generation == 1 && decoded.view->map.capacity_generation == 1 &&
            !decoded.view->map.next && decoded.view->leftover_records == 0,
        "it decodes");
  Check(std::equal(states.begin(), states.end(), decoded.view->map.states.begin()) &&
            std::equal(records.begin(), records.end(), decoded.view->map.records.begin(),
                       decoded.view->map.records.end()),
        "states and records round-trip");
  // Encode refusals.
  {
    auto bad = records;
    bad[0].creator_operation_uuid = V7(56);  // two creators
    Check(pg::EncodeAllocationMapInplacePage(f, states, bad, image) == AE::invalid_identity, "two creators refused");
    auto missing = states;
    missing[6] = S::allocated;  // no record for it
    Check(pg::EncodeAllocationMapInplacePage(f, missing, records, image) == AE::invalid_record,
          "an allocated slot without a record is refused");
    auto wide = f;
    wide.slots = slots + 1;
    Check(pg::EncodeAllocationMapInplacePage(wide, {}, {}, image) == AE::invalid_range, "more slots than fit refused");
    auto linked = f;
    linked.next_page = 3;  // itself
    linked.next_generation = 1;
    Check(pg::EncodeAllocationMapInplacePage(linked, {}, {}, image) == AE::invalid_reference, "a self link refused");
    Check(pg::EncodeAllocationMapInplacePage(f, states, records, image) == AE::none, "re-encode");
  }
  // Decode refusals: every structural fault in turn.
  const auto refused = [&](auto mutate, AE expected, std::string_view what) {
    auto copy = image;
    mutate(copy);
    const auto r = Decode(copy, slots);
    if (r.ok() || r.error != expected)
      std::cerr << what << ": error=" << int(r.error) << " expected=" << int(expected) << '\n';
    Check(!r.ok() && r.error == expected, what);
  };
  refused([](auto& b) { b[128] ^= 1; }, AE::invalid_family, "a wrong magic is refused");
  refused([](auto& b) { b[128 + 110] = 1; }, AE::invalid_family, "a reserved family byte is refused");
  refused([](auto& b) { b[256 + 20] = 1; }, AE::invalid_family, "a reserved mutable byte is refused");
  refused([&](auto& b) { b[512 + 3] = static_cast<byte>((b[512 + 3] & 0x0f) | 0x90); }, AE::invalid_state,
          "a state code above 7 is refused");
  refused([&](auto& b) { b[512 + 3] = static_cast<byte>((b[512 + 3] & 0x0f) | 0x20); }, AE::invalid_record,
          "an allocated state without a record is refused");
  refused([&](auto& b) { b[records_at - 1] = 1; }, AE::invalid_family, "non-zero padding before the records is refused");
  // At the canonical sizes the records end exactly at the page end.
  if (records_at + 128ull * slots < size)
    refused([&](auto& b) { b[size - 1] = 1; }, AE::invalid_family, "bytes beyond the last record are refused");
  refused([&](auto& b) { b[records_at + 128 * 0 + 120] = 1; }, AE::invalid_record, "a record's reserved tail is refused");
  refused([&](auto& b) { b[records_at + 128 * 0] ^= 1; }, AE::invalid_record, "a record for another page is refused");
  if (slots % 2) refused([&](auto& b) { b[512 + slots / 2] |= 0x10; }, AE::invalid_family, "the unused nibble is refused");

  // Edits: each inside its unit, both sides valid, both tears valid.
  const auto edit_checked = [&](std::vector<byte>& img, auto apply, std::string_view what) {
    const auto before = img;
    const auto edit = apply(img);
    Check(edit.ok() && edit.changed, std::string(what) + ": applies");
    Check(edit.unit_offset % 512 == 0 &&
              std::equal(edit.preimage.begin(), edit.preimage.end(), before.begin() + edit.unit_offset),
          std::string(what) + ": reports its unit with the bytes before");
    for (std::size_t i = 0; i < img.size(); ++i)
      if (i < edit.unit_offset || i >= edit.unit_offset + 512)
        if (img[i] != before[i]) Check(false, std::string(what) + ": a byte outside the unit changed");
    Check(Decode(before, slots).ok() && Decode(img, slots).ok(), std::string(what) + ": old and new decode");
    for (int half = 0; half < 2; ++half) {
      auto torn = before;
      const auto from = edit.unit_offset + (half ? 256 : 0);
      std::copy_n(img.begin() + from, 256, torn.begin() + from);
      Check(Decode(torn, slots).ok() && (torn == before || torn == img),
            std::string(what) + ": a torn unit is the old or the new image");
    }
  };
  // Allocate slot 7: record, then state.
  edit_checked(image, [&](auto& img) { return pg::WriteAllocationMapInplaceRecord(img, 7, Record(7, 7)); },
               "record of a free slot");
  {
    const auto staged = Decode(image, slots);
    Check(staged.ok() && staged.view->leftover_records == 1 && staged.view->map.records.size() == records.size(),
          "a record over a free slot is a leftover, not reported");
  }
  edit_checked(image, [&](auto& img) { return pg::SetAllocationMapInplaceAllocated(img, 7); }, "state free -> allocated");
  {
    const auto allocated = Decode(image, slots);
    Check(allocated.ok() && allocated.view->leftover_records == 0 && allocated.view->map.records.size() == records.size() + 1 &&
              allocated.view->map.states[7] == S::allocated,
          "the allocation is reported");
    Check(!pg::WriteAllocationMapInplaceRecord(image, 7, Record(7, 8)).ok(), "an allocated slot's record is not rewritten");
    const auto again = pg::SetAllocationMapInplaceAllocated(image, 7);
    Check(again.ok() && !again.changed, "allocating it again is a no-op");
    Check(!pg::SetAllocationMapInplaceAllocated(image, 8).ok(), "a slot without a record cannot be allocated");
    Check(!pg::WriteAllocationMapInplaceRecord(image, 8, Record(9, 8)).ok(), "a record must name its slot's page");
    Check(!pg::WriteAllocationMapInplaceRecord(image, slots, Record(slots, 1)).ok(), "a slot beyond the page is refused");
  }
  // A leftover is overwritten by the next allocation of its slot.
  edit_checked(image, [&](auto& img) { return pg::WriteAllocationMapInplaceRecord(img, 9, Record(9, 90)); }, "leftover");
  edit_checked(image, [&](auto& img) { return pg::WriteAllocationMapInplaceRecord(img, 9, Record(9, 91)); },
               "a leftover is overwritten");
  // The last slot (its record in the last unit of the page).
  edit_checked(image, [&](auto& img) { return pg::WriteAllocationMapInplaceRecord(img, slots - 1, Record(slots - 1, 99)); },
               "record of the last slot");
  edit_checked(image, [&](auto& img) { return pg::SetAllocationMapInplaceAllocated(img, slots - 1); }, "last slot state");
  // The next link: once.
  edit_checked(image, [&](auto& img) { return pg::SetAllocationMapInplaceNext(img, 5, 1); }, "link the next map page");
  {
    const auto same = pg::SetAllocationMapInplaceNext(image, 5, 1);
    Check(same.ok() && !same.changed && !pg::SetAllocationMapInplaceNext(image, 6, 1).ok() &&
              !pg::SetAllocationMapInplaceNext(image, 3, 1).ok(),
          "the link is written once and never to the page itself");
    const auto linked = Decode(image, slots);
    Check(linked.ok() && linked.view->map.next && linked.view->map.next->page_number == 5, "the link decodes");
  }
}

// ---- Production files ---------------------------------------------------------
struct Created {
  scratchbird::tests::OwnedTempDirectory dir;
  std::string path;
  u32 size = 0;
  Uuid database, primary, profile;
  explicit Created(unsigned index) {
    const auto& p = d::kCanonicalFilespacePageProfiles[index];
    size = p.page_size_bytes;
    profile = p.uuid;
    db::NativeDatabaseCreateRequest r;
    r.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
    r.database_name = "orders";
    r.database_default_language = "en";
    r.page_size_profile_uuid = p.uuid;
    r.page_size_bytes = p.page_size_bytes;
    r.total_pages = p.page_size_bytes == 8192 ? 1024 : 512;
    r.creation_utc_millis = 1789357072000ULL;
    r.engine_uuid = V7(4);
    r.datatype_cohort = {{UuidKind::object, scratchbird::core::datatypes::kDatatypeCohortV11}, 11, 11};
    r.bootstrap_principal_name = "BootstrapAdmin";
    r.bootstrap_credential_fingerprint = kFingerprint;
    r.service_principal_name = "NodeService";
    r.service_credential_fingerprint = kFingerprint;
    r.maximum_retained_image_bytes = db::NativeHostedSourceBackingBytes(p.page_size_bytes);
    path = (dir.path() / "created").string();
    d::FileDevice device;
    Check(device.Open(path, d::FileOpenMode::create_new,d::DatabaseFileOwnership::native_data_file).ok(), "node device");
    u::StandaloneUuidV7Issuer issuer({V7(1), V7(10)}, {{}, 0, 1000});
    auto made = db::CreateNativeDatabaseOnOpenDevice(device, r, issuer);
    Check(made.ok(), "the production create writes a complete native database");
    database = made.receipt->database_uuid;
    primary = made.receipt->filespace_uuid;
    Check(device.Close().ok(), "close");
  }
};
std::vector<byte> ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void WriteAt(const std::string& path, u64 offset, std::span<const byte> bytes) {
  std::fstream out(path, std::ios::binary | std::ios::in | std::ios::out);
  out.seekp(static_cast<std::streamoff>(offset));
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  Check(bool(out.flush()), "write into the file");
}
pg::NativeAllocationChainResult Chain(const std::string& path, const d::FilespaceBootstrapBinding& binding) {
  d::FileDevice device;
  Check(device.Open(path, d::FileOpenMode::open_existing_read_only).ok(), "read-only open");
  auto chain = pg::ReadNativeAllocationChainFromOpenDevice(device, binding, 64u << 20);
  Check(device.Close().ok(), "close");
  return chain;
}

void Production(unsigned index) {
  const Created c(index);
  const d::FilespaceBootstrapBinding binding{c.database, c.primary, c.profile};
  const auto chain = Chain(c.path, binding);
  if (!chain.ok()) std::cerr << "chain error=" << int(chain.error) << '\n';
  Check(chain.ok(), "the chain reader reads the production create's map");
  const auto file = ReadAll(c.path);
  const u32 slots = pg::AllocationMapInplaceSlotsPerPage(c.size);
  u64 covered = 0, records = 0, allocated = 0;
  for (std::size_t i = 0; i < chain.pages.size(); ++i) {
    const auto& page = chain.pages[i];
    const auto image = std::span<const byte>(file).subspan(page.map->header.page_number * c.size, c.size);
    Check(pg::IsAllocationMapInplaceImage(image), "every map page the create writes is in place");
    const auto decoded = Decode(image, slots);
    Check(decoded.ok() && decoded.view->sequence == i && decoded.view->map.first_page == covered &&
              decoded.view->leftover_records == 0,
          "each page decodes in sequence, contiguous");
    covered += decoded.view->slots;
    for (const auto& r : page.map->records) {
      ++records;
      Check(r.creator_transaction_uuid == page.map->creator_transaction_uuid && r.creator_local_transaction_id == 1 &&
                r.creator_operation_uuid.is_nil(),
            "every page the create wrote was created by transaction 1");
    }
    allocated += std::count(page.map->states.begin(), page.map->states.end(), S::allocated);
  }
  Check(covered == chain.pages.front().map->total_pages && records == allocated && records > chain.pages.size(),
        "the chain covers the file and has a record for every allocated page");
  // The fixed-root lease (the production reader) reads it too.
  {
    d::FileDevice device;
    Check(device.Open(c.path, d::FileOpenMode::open_existing_read_only).ok(), "read-only open");
    const std::vector<d::NativeFilespaceDevice> files{{c.primary, c.profile, &device}};
    std::vector<byte> backing(64u << 20);
    auto fixed = db::AcquireNativeFixedRootBackedLease(c.database, files, c.primary, {256ull * c.size, backing.size()},
                                                       backing);
    Check(fixed.ok(), "the fixed-root lease reads a file with an in-place map");
    fixed.lease.Reset();
    Check(device.Close().ok(), "close");
  }
  // W4d-3b: the selected (digest-bound) read is gone; the bootstrap read is
  // the only read, so "an in-place map is never read as a selected root" has
  // no API left to ask.
  // A head map page of another family (the removed sealed family's magic, the
  // likeliest stale bytes) is an unknown family.
  constexpr std::array<byte, 8> removed_family{{'S', 'B', 'A', 'B', 'M', '0', '0', '2'}};
  {
    const u64 at = chain.pages.front().map->header.page_number * c.size + 128;
    WriteAt(c.path, at, removed_family);
    const auto forged = Chain(c.path, binding);
    Check(!forged.ok() && forged.error == AE::invalid_family, "a head map page of another family is an unknown family");
    WriteAt(c.path, at, std::span<const byte>(file).subspan(at, removed_family.size()));
    Check(Chain(c.path, binding).ok(), "restored");
  }
  // Damage on disk: a free slot claimed without a record.
  {
    const auto& last = chain.pages.back();
    const auto free_at = std::find(last.map->states.begin(), last.map->states.end(), S::free);
    Check(free_at != last.map->states.end(), "the last map page has a free slot");
    const auto slot = static_cast<u32>(free_at - last.map->states.begin());
    const u64 at = last.map->header.page_number * c.size + 512 + slot / 2;
    auto byte_now = file[at];
    byte_now = static_cast<byte>((byte_now & ~(0x0f << (4 * (slot % 2)))) | (0x2 << (4 * (slot % 2))));
    WriteAt(c.path, at, std::span<const byte>(&byte_now, 1));
    const auto damaged = Chain(c.path, binding);
    Check(!damaged.ok() && damaged.error == AE::invalid_record, "a page claimed without a record is refused");
    WriteAt(c.path, at, std::span<const byte>(&file[at], 1));
    Check(Chain(c.path, binding).ok(), "restored");
  }
  // Later map pages (needs a second map page): one field rewritten in place,
  // read, then restored byte for byte.
  if (chain.pages.size() > 1) {
    const auto& second = chain.pages[1];
    const u64 page_at = second.map->header.page_number * c.size;
    const auto forge = [&](u64 offset, std::span<const byte> bytes, AE expected, std::string_view why) {
      WriteAt(c.path, page_at + offset, bytes);
      const auto forged = Chain(c.path, binding);
      Check(!forged.ok() && forged.error == expected, why);
      WriteAt(c.path, page_at + offset, std::span<const byte>(file).subspan(page_at + offset, bytes.size()));
      Check(Chain(c.path, binding).ok(), "restored");
    };
    // Another family after an in-place head breaks the chain.
    forge(128, removed_family, AE::chain_mismatch, "a later map page of another family breaks the chain");
    ++mixed_refused;
    // Every map page names the head's creator (family header +80: the local
    // transaction number).
    std::array<byte, 8> number{};
    scratchbird::core::platform::StoreLittle64(number.data(), second.map->creator_local_transaction_id + 1);
    forge(128 + 80, number, AE::chain_mismatch, "a later map page naming another creator breaks the chain");
    // A link back into the chain is a cycle: the second page's next link
    // (offset 256: page, then generation) names the head map page.
    std::array<byte, 16> link{};
    scratchbird::core::platform::StoreLittle64(link.data(), chain.pages.front().map->header.page_number);
    scratchbird::core::platform::StoreLittle64(link.data() + 8, chain.pages.front().map->header.page_generation);
    forge(256, link, AE::chain_mismatch, "a link back to the head map page is a cycle");
  }
}

}  // namespace

int main() {
  try {
    for (unsigned index = 0; index < d::kCanonicalFilespacePageProfiles.size(); ++index) {
      Codec(d::kCanonicalFilespacePageProfiles[index]);
      Production(index);
      std::cout << "allocation map in place page_size=" << d::kCanonicalFilespacePageProfiles[index].page_size_bytes
                << " slots=" << pg::AllocationMapInplaceSlotsPerPage(d::kCanonicalFilespacePageProfiles[index].page_size_bytes)
                << " ok\n";
    }
    Check(mixed_refused >= 4, "the later-page forgeries ran at every page size with more than one map page");
    std::cout << "PASS native allocation map in place checks=" << checks << " mixed_refused=" << mixed_refused << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL native allocation map in place: " << e.what() << " checks=" << checks << '\n';
    return 1;
  }
}
