// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_TRANSACTION_INVENTORY_READER
// Read-only load and verification of the in-place transaction inventory (the
// transaction-state chain and the header chain). Not wired into open yet.
#include "transaction_inventory_reader.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
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
}  // namespace

int main() {
  try {
    TestCheckValid();
    TestCrossCheckFindings();
    TestChainFindings();
    TestDeviceReader();
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << " after " << checks << " checks\n";
    return 1;
  }
  std::cout << "transaction_inventory_reader: " << checks << " checks passed\n";
  return 0;
}
