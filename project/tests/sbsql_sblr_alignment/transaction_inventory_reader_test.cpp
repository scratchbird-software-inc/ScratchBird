// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_TRANSACTION_INVENTORY_READER
// Read-only load and verification of the in-place transaction inventory (the
// transaction-state chain and the header chain). Not wired into open yet.
#include "transaction_inventory_reader.hpp"
#include "transaction_inventory_validation.hpp"
#include "native_inventory_chain_backing.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory_resource>
#include <source_location>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
using page::TipPageError;
using page::TipStateCode;
using Stage = page::TransactionInventoryReadStage;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::core::platform::Uuid;

unsigned checks = 0;
void Check(bool ok, std::string_view why, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) throw std::runtime_error(std::string(why) + " (line " + std::to_string(at.line()) + ")");
}
void CheckFinding(const page::TransactionInventoryCheck& c, TipPageError e, Stage stage, std::string_view why,
                  std::optional<u64> order = std::nullopt, std::source_location at = std::source_location::current()) {
  Check(c.error == e, std::string(why) + ": expected " + page::TipPageErrorName(e) + ", got " +
                          page::TipPageErrorName(c.error),
        at);
  Check(c.stage == stage, std::string(why) + ": stage", at);
  if (order) Check(c.transaction_order == order, std::string(why) + ": transaction_order", at);
}

Uuid V7(std::uint8_t tag) {
  Uuid id;
  for (std::size_t i = 0; i < 16; ++i) id.bytes[i] = static_cast<byte>(0x10 + tag + i);
  id.bytes[6] = 0x70 | (id.bytes[6] & 0x0f);
  id.bytes[8] = 0x80 | (id.bytes[8] & 0x3f);
  return id;
}

constexpr u32 kPage = 8192;
const Uuid kDatabase = V7(1), kFilespace = V7(2);
const Uuid kProfile = disk::FindCanonicalFilespacePageProfileForSize(kPage)->uuid;

disk::NativeCommonPageHeader Header(u64 number, Uuid database = kDatabase) {
  disk::NativeCommonPageHeader h;
  h.page_size_bytes = kPage;
  h.page_type = page::kTransactionStatePageType;
  h.database_uuid = database;
  h.filespace_uuid = kFilespace;
  h.page_uuid = V7(static_cast<std::uint8_t>(40 + number));
  h.page_number = number;
  h.page_generation = 1;
  h.page_size_profile_uuid = kProfile;
  return h;
}
page::TipCounters Counters(u64 next) {
  page::TipCounters c;
  c.next_transaction = next;
  c.horizons = {next, next, next, next, next, next};
  return c;
}
std::vector<byte> StatePage(u64 number, u64 sequence, u64 next_page, u64 header_chain, std::optional<u64> next) {
  page::TipPageFormat f;
  f.header = Header(number);
  f.tip_sequence = sequence;
  f.next_tip_page = next_page;
  f.header_chain_page = header_chain;
  if (next) f.counters = Counters(*next);
  std::vector<byte> image(kPage);
  Check(page::FormatTransactionStatePage(f, image) == TipPageError::none, "format state page");
  return image;
}
std::vector<byte> HeaderPage(u64 number, u64 sequence, u64 next_page, Uuid database = kDatabase) {
  page::TransactionHeaderPageFormat f;
  f.header = Header(number, database);
  f.sequence = sequence;
  f.next_page = next_page;
  std::vector<byte> image(kPage);
  Check(page::FormatTransactionHeaderPage(f, image) == TipPageError::none, "format header page");
  return image;
}
page::TransactionHeaderRecord HeaderFor(u64 order, TipStateCode final_state = TipStateCode::unused) {
  page::TransactionHeaderRecord r;
  r.transaction_order = order;
  r.database_uuid = kDatabase;
  r.transaction_uuid = V7(static_cast<std::uint8_t>(order));
  if (final_state != TipStateCode::unused)
    Check(page::CompleteTransactionHeader(r, final_state, 1000 + order) == page::TransactionHeaderError::none,
          "complete header");
  return r;
}
void SetWord(std::vector<byte>& state, u32 slot, TipStateCode code, bool has_header = true) {
  page::TipStateWord w{code, static_cast<std::uint8_t>(has_header ? page::kTipFlagHasDetailRecord : 0)};
  Check(page::SetTransactionStateWord(state, slot, w).ok(), "set word");
}
void PutHeader(std::vector<byte>& headers, const page::TransactionHeaderRecord& r) {
  Check(page::WriteTransactionHeader(headers, r).ok(), "write header");
}

// Next = 10. State page 1 -> header page 3. A representative mix of legal states.
struct Inventory {
  std::vector<byte> state = StatePage(1, 0, 0, 3, 10);
  std::vector<byte> headers = HeaderPage(3, 0, 0);
  Inventory() {
    SetWord(state, 1, TipStateCode::committed);
    PutHeader(headers, HeaderFor(1, TipStateCode::committed));
    SetWord(state, 2, TipStateCode::rolled_back);
    PutHeader(headers, HeaderFor(2, TipStateCode::rolled_back));
    SetWord(state, 3, TipStateCode::active);
    PutHeader(headers, HeaderFor(3));
    SetWord(state, 4, TipStateCode::prepared);
    PutHeader(headers, HeaderFor(4));
    // 5: header written, slot not yet (crash window) -> legal.
    PutHeader(headers, HeaderFor(5));
    // 6: dead after restart, never had a header.
    SetWord(state, 6, TipStateCode::dead, false);
    // 7: header committed but slot still active (crash before the state byte) -> legal.
    SetWord(state, 7, TipStateCode::active);
    PutHeader(headers, HeaderFor(7, TipStateCode::committed));
    // 8: archived, origin in the header.
    SetWord(state, 8, TipStateCode::archived);
    PutHeader(headers, HeaderFor(8, TipStateCode::committed));
  }
  page::TransactionInventoryCheck Run() const { return page::CheckTransactionInventoryImages(kPage, state, headers); }
};

void TestCheckValid() {
  Inventory inv;
  const auto c = inv.Run();
  Check(c.ok(), "valid inventory");
  Check(c.counters == Counters(10), "counters returned");
}

void TestCrossCheckFindings() {
  {
    Inventory inv;  // committed slot whose header is not completed
    SetWord(inv.state, 9, TipStateCode::committed);
    PutHeader(inv.headers, HeaderFor(9));
    CheckFinding(inv.Run(), TipPageError::invalid_slot, Stage::cross_check, "committed without completed header", 9);
  }
  {
    Inventory inv;  // committed slot with no header at all
    SetWord(inv.state, 9, TipStateCode::committed, false);
    CheckFinding(inv.Run(), TipPageError::invalid_slot, Stage::cross_check, "committed without header", 9);
  }
  {
    Inventory inv;  // has-header flag but no header
    SetWord(inv.state, 9, TipStateCode::active, true);
    CheckFinding(inv.Run(), TipPageError::invalid_slot, Stage::cross_check, "flagged header missing", 9);
  }
  {
    Inventory inv;  // an in-progress slot with neither flag nor header
    SetWord(inv.state, 9, TipStateCode::active, false);
    CheckFinding(inv.Run(), TipPageError::invalid_slot, Stage::cross_check, "active slot without header", 9);
  }
  {
    Inventory inv;  // rolled_back slot, committed header
    SetWord(inv.state, 9, TipStateCode::rolled_back);
    PutHeader(inv.headers, HeaderFor(9, TipStateCode::committed));
    CheckFinding(inv.Run(), TipPageError::invalid_slot, Stage::cross_check, "outcome mismatch", 9);
  }
  {
    Inventory inv;  // prepared slot, completed header
    SetWord(inv.state, 9, TipStateCode::prepared);
    PutHeader(inv.headers, HeaderFor(9, TipStateCode::rolled_back));
    CheckFinding(inv.Run(), TipPageError::invalid_slot, Stage::cross_check, "prepared but completed", 9);
  }
  {
    Inventory inv;  // archived slot, header not completed
    SetWord(inv.state, 9, TipStateCode::archived);
    PutHeader(inv.headers, HeaderFor(9));
    CheckFinding(inv.Run(), TipPageError::invalid_slot, Stage::cross_check, "archived without origin", 9);
  }
  {
    Inventory inv;  // header at Next
    PutHeader(inv.headers, HeaderFor(10));
    CheckFinding(inv.Run(), TipPageError::invalid_slot, Stage::cross_check, "header at Next", 10);
  }
}

void TestChainFindings() {
  {
    Inventory inv;  // first state page links a page that is not passed
    auto state = StatePage(1, 0, 2, 3, 10);
    CheckFinding(page::CheckTransactionInventoryImages(kPage, state, inv.headers), TipPageError::invalid_link,
                 Stage::state_chain, "dangling state link");
  }
  {
    // Two state pages, the second not the linked one.
    Inventory inv;
    auto state = StatePage(1, 0, 2, 3, 10);
    auto other = StatePage(4, 1, 0, 0, std::nullopt);
    state.insert(state.end(), other.begin(), other.end());
    CheckFinding(page::CheckTransactionInventoryImages(kPage, state, inv.headers), TipPageError::invalid_link,
                 Stage::state_chain, "state link mismatch");
  }
  {
    // Next beyond the chain's coverage.
    auto state = StatePage(1, 0, 0, 3, 1925);
    auto headers = HeaderPage(3, 0, 0);
    CheckFinding(page::CheckTransactionInventoryImages(kPage, state, headers), TipPageError::invalid_counters,
                 Stage::state_chain, "chain shorter than Next");
  }
  {
    // A two-page state chain covering Next = 1925; page 2 slots unused (legal).
    auto state = StatePage(1, 0, 2, 3, 1925);
    auto second = StatePage(2, 1, 0, 0, std::nullopt);
    state.insert(state.end(), second.begin(), second.end());
    auto headers = HeaderPage(3, 0, 0);
    Check(page::CheckTransactionInventoryImages(kPage, state, headers).ok(), "two-page state chain");
    // A used slot at Next on page 2 is refused with its order.
    auto bad = state;
    bad[kPage + 512 + 4 * 5] = static_cast<byte>(TipStateCode::active);  // order 1925
    CheckFinding(page::CheckTransactionInventoryImages(kPage, bad, headers), TipPageError::invalid_slot,
                 Stage::state_chain, "slot at Next on page 2", 1925);
  }
  {
    Inventory inv;  // header chain starts at the wrong page
    auto headers = HeaderPage(4, 0, 0);
    CheckFinding(page::CheckTransactionInventoryImages(kPage, inv.state, headers), TipPageError::invalid_link,
                 Stage::header_chain, "header chain start");
  }
  {
    Inventory inv;  // header page of another database
    auto headers = HeaderPage(3, 0, 0, V7(77));
    CheckFinding(page::CheckTransactionInventoryImages(kPage, inv.state, headers), TipPageError::invalid_link,
                 Stage::header_chain, "header page of another database");
  }
  {
    Inventory inv;  // header page sequence wrong
    auto headers = HeaderPage(3, 1, 0);
    CheckFinding(page::CheckTransactionInventoryImages(kPage, inv.state, headers), TipPageError::invalid_link,
                 Stage::header_chain, "header sequence");
  }
  {
    Inventory inv;  // header chain links a page that is not passed
    auto headers = HeaderPage(3, 0, 9);
    CheckFinding(page::CheckTransactionInventoryImages(kPage, inv.state, headers), TipPageError::invalid_link,
                 Stage::header_chain, "dangling header link");
  }
  {
    Inventory inv;
    CheckFinding(page::CheckTransactionInventoryImages(kPage, std::span<const byte>(inv.state.data(), kPage - 1),
                                                       inv.headers),
                 TipPageError::invalid_page_size, Stage::request, "partial page");
    CheckFinding(page::CheckTransactionInventoryImages(kPage, inv.state, {}), TipPageError::invalid_page_size,
                 Stage::request, "no header chain");
  }
}

void TestBuildInventory() {
  namespace mga = scratchbird::transaction::mga;
  Inventory inv;
  const auto built = page::BuildLocalTransactionInventoryFromImages(kPage, inv.state, inv.headers);
  Check(built.ok(), "inventory built");
  const auto& i = built.inventory;
  Check(i.next_local_transaction_id == 10, "next local id is Next");
  // Entries: 1 committed, 2 rolled_back, 3 active, 4 prepared, 7 active, 8 archived.
  // Skipped: 5 (header, slot unused), 6 (dead, no header), 9 (unused).
  std::vector<u64> ids;
  for (const auto& e : i.entries) ids.push_back(e.identity.local_id.value);
  Check(ids == std::vector<u64>({1, 2, 3, 4, 7, 8}), "entries in transaction_order");
  auto at = [&](u64 n) -> const mga::TransactionInventoryEntry& {
    return *std::find_if(i.entries.begin(), i.entries.end(), [&](const auto& e) { return e.identity.local_id.value == n; });
  };
  Check(at(1).state == mga::TransactionState::committed && at(1).commit_sequence == 1, "1 committed first");
  Check(at(8).state == mga::TransactionState::archived && at(8).archived_from_state == mga::TransactionState::committed &&
            at(8).commit_sequence == 2,
        "8 archived from committed, second commit");
  Check(at(2).state == mga::TransactionState::rolled_back && at(2).commit_sequence == 0, "2 rolled back");
  Check(at(3).state == mga::TransactionState::active && at(4).state == mga::TransactionState::prepared, "3 active, 4 prepared");
  Check(at(7).state == mga::TransactionState::active && at(7).commit_sequence == 0,
        "7: the state word decides, not the header");
  Check(i.next_commit_sequence == 3, "next commit sequence");
  Check(at(1).identity.transaction_uuid.value == HeaderFor(1).transaction_uuid &&
            at(1).identity.scope == mga::TransactionScope::local_node,
        "identity from the header");
  Check(*mga::ValidateLocalTransactionInventoryStructure(i) == '\0', "valid in-memory inventory");
  // Commit order follows commit time, not transaction_order.
  Inventory later;
  SetWord(later.state, 9, TipStateCode::committed);
  auto early = HeaderFor(9);
  Check(page::CompleteTransactionHeader(early, TipStateCode::committed, 500) == page::TransactionHeaderError::none, "early");
  PutHeader(later.headers, early);
  const auto ordered = page::BuildLocalTransactionInventoryFromImages(kPage, later.state, later.headers);
  Check(ordered.ok(), "built with an early commit");
  const auto nine = std::find_if(ordered.inventory.entries.begin(), ordered.inventory.entries.end(),
                                 [](const auto& e) { return e.identity.local_id.value == 9; });
  Check(nine->commit_sequence == 1, "transaction 9 committed first by time");
  // A finding stops the build.
  Inventory bad;
  SetWord(bad.state, 9, TipStateCode::committed, false);
  const auto refused = page::BuildLocalTransactionInventoryFromImages(kPage, bad.state, bad.headers);
  Check(!refused.ok() && refused.transaction_order == 9, "build refuses a failing check");
}

// --- device reader -------------------------------------------------------

struct TempFile {
  std::string path;
  TempFile() {
    path = (std::filesystem::temp_directory_path() /
            ("sb_txn_inventory_reader_" + std::to_string(::getpid()) + ".db"))
               .string();
    std::filesystem::remove(path);
  }
  ~TempFile() { std::filesystem::remove(path); }
};

void WritePages(const std::string& path, const std::vector<std::pair<u64, std::vector<byte>>>& pages, u64 total) {
  std::vector<byte> file(total * kPage, 0);
  for (const auto& [number, image] : pages) std::copy(image.begin(), image.end(), file.begin() + number * kPage);
  std::FILE* f = std::fopen(path.c_str(), "wb");
  Check(f != nullptr, "open temp file");
  Check(std::fwrite(file.data(), 1, file.size(), f) == file.size(), "write temp file");
  std::fclose(f);
}

disk::FilespaceRootReference Root(u64 page_number = 1, u64 generation = 1) {
  disk::FilespaceRootReference r;
  r.kind = 4;
  r.page_type = page::kTransactionStatePageType;
  r.filespace_uuid = kFilespace;
  r.page_number = page_number;
  r.page_generation = generation;
  r.page_size_profile_uuid = kProfile;
  r.object_uuid = V7(41);
  return r;
}

page::TransactionInventoryRead ReadFile(const std::string& path, disk::FilespaceRootReference root,
                                        std::size_t backing_pages) {
  disk::FileDevice device;
  Check(device.Open(path, disk::FileOpenMode::open_existing_read_only).ok(), "open read-only");
  disk::FileDevice::ReadLatencyBatch batch(device);
  const disk::NativeFilespaceDevice files[] = {{kFilespace, kProfile, &device}};
  disk::FileDevice::ReadLatencyBatch* const batches[] = {&batch};
  std::vector<byte> backing(backing_pages * kPage);
  page::TransactionInventoryReadRequest request;
  request.database_uuid = kDatabase;
  request.state_root = root;
  request.files = files;
  request.batches = batches;
  request.backing = backing;
  return page::ReadTransactionInventory(request);
}

void TestDeviceReader() {
  TempFile tmp;
  Inventory inv;
  // Pages: 0 empty, 1 state, 3 headers.
  WritePages(tmp.path, {{1, inv.state}, {3, inv.headers}}, 6);
  auto read = ReadFile(tmp.path, Root(), 8);
  Check(read.ok(), "device read");
  Check(read.page_size == kPage && read.state_pages == 1 && read.header_pages == 1, "page counts");
  Check(read.check.counters == Counters(10), "counters from the device");
  // The backing is exactly one page short.
  read = ReadFile(tmp.path, Root(), 1);
  Check(!read.ok() && !read.io_failure, "short backing refused");
  CheckFinding(read.check, TipPageError::short_buffer, Stage::header_chain, "short backing");
  // Root bound to the wrong generation.
  read = ReadFile(tmp.path, Root(1, 2), 8);
  CheckFinding(read.check, TipPageError::invalid_header, Stage::state_chain, "root generation mismatch");
  // Root of the wrong kind or type.
  auto root = Root();
  root.kind = 5;
  CheckFinding(ReadFile(tmp.path, root, 8).check, TipPageError::invalid_header, Stage::request, "root kind");
  root = Root();
  root.page_type = 0x0302;
  CheckFinding(ReadFile(tmp.path, root, 8).check, TipPageError::invalid_header, Stage::request, "root type");
  root = Root();
  root.filespace_uuid = V7(55);
  CheckFinding(ReadFile(tmp.path, root, 8).check, TipPageError::invalid_header, Stage::request, "unknown filespace");
  // A file that ends before the header page.
  WritePages(tmp.path, {{1, inv.state}}, 3);
  read = ReadFile(tmp.path, Root(), 8);
  Check(read.io_failure && !read.ok(), "read past end of file is an I/O failure");
  Check(read.check.stage == Stage::header_chain, "I/O failure stage");
  // A header chain that loops back to its first page.
  auto h0 = HeaderPage(3, 0, 4), h1 = HeaderPage(4, 1, 3);
  auto state = StatePage(1, 0, 0, 3, 10);
  WritePages(tmp.path, {{1, state}, {3, h0}, {4, h1}}, 6);
  CheckFinding(ReadFile(tmp.path, Root(), 8).check, TipPageError::invalid_link, Stage::header_chain, "header cycle");
  // Two-page state chain and two-page header chain read in order.
  auto s0 = StatePage(1, 0, 2, 3, 40), s1 = StatePage(2, 1, 0, 0, std::nullopt);
  auto g0 = HeaderPage(3, 0, 4), g1 = HeaderPage(4, 1, 0);
  SetWord(s0, 35, TipStateCode::committed);
  PutHeader(g1, HeaderFor(35, TipStateCode::committed));  // order 35 lives on header page 1 (slots 30..59)
  WritePages(tmp.path, {{1, s0}, {2, s1}, {3, g0}, {4, g1}}, 6);
  read = ReadFile(tmp.path, Root(), 8);
  Check(read.ok() && read.state_pages == 2 && read.header_pages == 2, "two-page chains");
  // The reader never writes: the file is byte-identical afterwards.
  std::vector<byte> before(6 * kPage), after(6 * kPage);
  {
    std::FILE* f = std::fopen(tmp.path.c_str(), "rb");
    Check(std::fread(before.data(), 1, before.size(), f) == before.size(), "read file");
    std::fclose(f);
  }
  (void)ReadFile(tmp.path, Root(), 8);
  {
    std::FILE* f = std::fopen(tmp.path.c_str(), "rb");
    Check(std::fread(after.data(), 1, after.size(), f) == after.size(), "reread file");
    std::fclose(f);
  }
  Check(before == after, "reader leaves the file unchanged");
}

// --- the chain-view reader the fixed-root lease reads the inventory with ----
// W4d-3 (2026-11-08): the SBTINV01 reader this one once handed unplaceable
// heads to is removed; every refusal below is this reader's own.

page::detail::NativeInPlaceInventoryRead ReadChainView(const std::string& path, disk::FilespaceRootReference root,
                                                       u64 budget, std::vector<byte>* file_bytes = nullptr) {
  disk::FileDevice device;
  Check(device.Open(path, disk::FileOpenMode::open_existing_read_only).ok(), "open read-only");
  disk::FileDevice::ReadLatencyBatch batch(device);
  const disk::NativeFilespaceDevice files[] = {{kFilespace, kProfile, &device}};
  disk::FileDevice::ReadLatencyBatch* const batches[] = {&batch};
  static std::pmr::monotonic_buffer_resource resource;
  auto out = page::detail::ReadNativeInPlaceInventoryBacked(kDatabase, files, root, budget, batches, resource);
  if (file_bytes) {
    // Copy the borrowed images while the backing is alive (the resource is).
    file_bytes->clear();
    for (const auto& view : out.read.chain.pages) file_bytes->insert(file_bytes->end(), view.image.begin(), view.image.end());
  }
  return out;
}

void TestChainViewReader() {
  TempFile tmp;
  Inventory inv;
  WritePages(tmp.path, {{1, inv.state}, {3, inv.headers}}, 6);
  std::vector<byte> images;
  auto r = ReadChainView(tmp.path, Root(), 64 * kPage, &images);
  Check(r.read.ok() && r.counters.has_value(), "in-place inventory read as a chain view");
  const auto& chain = r.read.chain;
  Check(chain.pages.size() == 2 && chain.pages[0].page.header.page_number == 1 &&
            chain.pages[1].page.header.page_number == 3,
        "one page view per state page, then per header page");
  Check(chain.pages[0].page.header.page_type == page::kTransactionStatePageType &&
            chain.pages[1].page.header.page_type == page::kTransactionStatePageType,
        "both families carry the inventory page type");
  Check(chain.pages[0].page.object_uuid == Root().object_uuid && chain.pages[1].page.object_uuid == Root().object_uuid,
        "each page carries the inventory object");
  Check(chain.pages[0].page.inventory_generation == 1 && chain.pages[1].page.inventory_generation == 1,
        "generation 1, the constructor's");
  std::vector<byte> expected = inv.state;
  expected.insert(expected.end(), inv.headers.begin(), inv.headers.end());
  Check(images == expected, "page views borrow the exact images read");
  std::vector<u64> ids;
  for (const auto& e : chain.inventory.entries) ids.push_back(e.identity.local_id.value);
  Check(chain.inventory.next_local_transaction_id == 10 && chain.inventory.next_commit_sequence == 3 &&
            ids == std::vector<u64>({1, 2, 3, 4, 7, 8}),
        "the same in-memory inventory the image build produces");
  // Interesting: 3 active, 4 prepared, 7 active; committed, rolled back and
  // archived are not.
  Check(chain.horizons.valid && chain.horizons.oldest_interesting_transaction.value == 3 &&
            chain.horizons.oldest_active_transaction.value == 3 &&
            chain.horizons.oldest_snapshot_transaction.value == 3 && chain.horizons.next_transaction_id.value == 10,
        "horizons projected from the built inventory");
  Check(chain.retained_image_bytes == 2 * kPage, "retained bytes are the pages read");
  Check(r.read.physical_bytes_read == page::kTransactionStateWordsOffset + 2 * kPage, "probe plus both pages");

  // Not an in-place head (the page at the root is a header page, family
  // SBTXHD01): an unknown family, refused by this reader, not handed on.
  WritePages(tmp.path, {{3, inv.headers}}, 6);
  r = ReadChainView(tmp.path, Root(), 64 * kPage);
  Check(!r.read.ok() && r.read.chain.error == page::NativeInventoryError::invalid_family && !r.counters &&
            r.read.chain.pages.empty(),
        "a head page without the SBTIPW01 family is refused as an unknown family");
  WritePages(tmp.path, {{1, inv.state}, {3, inv.headers}}, 6);
  const auto unplaced = [&](disk::FilespaceRootReference root, std::string_view why) {
    const auto refused = ReadChainView(tmp.path, root, 64 * kPage);
    Check(!refused.read.ok() && refused.read.chain.error == page::NativeInventoryError::invalid_reference &&
              refused.read.physical_bytes_read == 0 && !refused.counters,
          why);
  };
  auto root = Root();
  root.kind = 5;
  unplaced(root, "a root of another kind is refused before any read");
  root = Root();
  root.page_type = 0x0300;
  unplaced(root, "a root of another page type is refused before any read");
  root = Root();
  root.filespace_uuid = V7(55);
  unplaced(root, "a root in an unknown filespace is refused before any read");
  root = Root();
  for (const auto& other : disk::kCanonicalFilespacePageProfiles)
    if (other.uuid != kProfile) {
      root.page_size_profile_uuid = other.uuid;
      break;
    }
  Check(root.page_size_profile_uuid != kProfile, "another canonical page-size profile");
  unplaced(root, "a root whose page-size profile is not its filespace's is refused before any read");
  r = ReadChainView(tmp.path, Root(), 64 * kPage);
  Check(r.read.ok(), "the same file reads again with its own root");

  // Refusals after the probe.
  r = ReadChainView(tmp.path, Root(), kPage);
  Check(r.read.chain.error == page::NativeInventoryError::resource_exhausted && !r.read.ok(),
        "a budget below the fewest chain pages is refused before reading the chains");
  r = ReadChainView(tmp.path, Root(), 2 * kPage);
  Check(r.read.ok() && r.read.chain.retained_image_bytes == 2 * kPage,
        "a budget of exactly the fewest chain pages reads the inventory");
  WritePages(tmp.path, {{1, inv.state}}, 3);
  r = ReadChainView(tmp.path, Root(), 64 * kPage);
  Check(r.read.chain.error == page::NativeInventoryError::io_failure && !r.read.io_status.ok(),
        "a header chain beyond the end of the file is an I/O failure");
  r = ReadChainView(tmp.path, Root(5), 64 * kPage);
  Check(r.read.chain.error == page::NativeInventoryError::io_failure,
        "a head beyond the end of the file is an I/O failure of the probe");
  Inventory bad;
  SetWord(bad.state, 9, TipStateCode::committed, false);
  WritePages(tmp.path, {{1, bad.state}, {3, bad.headers}}, 6);
  r = ReadChainView(tmp.path, Root(), 64 * kPage);
  Check(r.read.chain.error == page::NativeInventoryError::invalid_inventory,
        "a failing cross check is an invalid inventory");
  auto foreign = Inventory();
  foreign.headers = HeaderPage(3, 0, 0, V7(77));
  WritePages(tmp.path, {{1, foreign.state}, {3, foreign.headers}}, 6);
  r = ReadChainView(tmp.path, Root(), 64 * kPage);
  Check(!r.read.ok(), "a header page of another database is refused");
}
}  // namespace

int main() {
  try {
    TestCheckValid();
    TestCrossCheckFindings();
    TestChainFindings();
    TestBuildInventory();
    TestDeviceReader();
    TestChainViewReader();
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << " after " << checks << " checks\n";
    return 1;
  }
  std::cout << "transaction_inventory_reader: " << checks << " checks passed\n";
  return 0;
}
