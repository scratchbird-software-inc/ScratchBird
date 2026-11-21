// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

// SEARCH_KEY: SB_NATIVE_OFFLINE_FILE_READER (S4e test support). Reads a native database FILE's bytes, with the production
// chain readers, for the audit trace by hand: the transaction inventory (state words and headers) and the session ledger's
// complete records, in order. No device, no lock, nothing written: it works on a copy of the bytes, so it can run while the
// file is served (give the page size) or after the owner closed it. Test code: a failed expectation prints and exits 1.
// (The same BytesSink / ReadTip pair exists privately in native_loop_transaction_test.cpp and native_session_ledger_loop_test.cpp;
// those two were not changed to use this header.)

#include "filespace_page_zero.hpp"
#include "session_ledger_page.hpp"
#include "session_ledger_record.hpp"
#include "transaction_inventory_writer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace scratchbird::tests::offline {

namespace p = scratchbird::core::platform;
namespace d = scratchbird::storage::disk;
namespace pg = scratchbird::storage::page;

inline void Require(bool ok, const char* what) {
  if (ok) return;
  std::fprintf(stderr, "FAIL (offline reader) %s\n", what);
  std::exit(1);
}

inline std::vector<p::byte> ReadAll(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(in)), {});
  return std::vector<p::byte>(reinterpret_cast<const p::byte*>(bytes.data()),
                              reinterpret_cast<const p::byte*>(bytes.data()) + bytes.size());
}

// A read-only sink over the bytes the file had when it was read.
class BytesSink final : public d::CarefulWriteSink {
 public:
  explicit BytesSink(const std::vector<p::byte>& image) : image_(image) {}
  d::BoundedIoResult ReadAt(p::u64 offset, std::span<p::byte> bytes) override {
    d::BoundedIoResult r;
    r.operation = d::BoundedIoOperation::read;
    r.offset = offset;
    r.requested_bytes = bytes.size();
    if (offset + bytes.size() > image_.size()) {
      r.error = d::BoundedIoError::native_failure;
      return r;
    }
    std::copy_n(image_.begin() + offset, bytes.size(), bytes.begin());
    r.bytes_transferred = bytes.size();
    return r;
  }
  d::BoundedIoResult WriteAt(p::u64, std::span<const p::byte>) override {
    d::BoundedIoResult r;
    r.error = d::BoundedIoError::native_failure;
    return r;
  }
  d::BoundedIoResult Sync() override { return {}; }
  bool forced_ordered() const noexcept override { return false; }

 private:
  const std::vector<p::byte>& image_;
};

struct FileView {
  std::vector<p::byte> image;
  std::uint32_t page_size = 0;
  d::FilespacePageZeroDecodeResult zero;
  // The transaction inventory.
  pg::TipCounters counters;
  std::vector<p::u64> state_pages, header_pages, ledger_pages;   // page numbers of the three chains (the pages a write may change)
  std::map<p::u64, pg::TipStateWord> words;                  // order -> state word (unused slots omitted)
  std::map<p::u64, pg::TransactionHeaderRecord> headers;     // order -> header
  // The session ledger: every complete record, in ordinal order.
  std::vector<pg::SessionLedgerRecord> ledger;
  // The latest record of a UUID (a session ends by a second record with the same UUID), or the first one.
  const pg::SessionLedgerRecord* FindRecord(const p::Uuid& uuid, bool latest = true) const {
    const pg::SessionLedgerRecord* found = nullptr;
    for (const auto& record : ledger)
      if (record.record_uuid == uuid && (latest || !found)) found = &record;
    return found;
  }
};

inline FileView ReadFile(const std::filesystem::path& path, std::uint32_t page_size) {
  FileView view;
  view.image = ReadAll(path);
  view.page_size = page_size;
  Require(view.image.size() >= page_size, "the file holds page zero");
  view.zero = d::DecodeFilespacePageZero(view.image.data(), page_size);
  Require(view.zero.ok(), "page zero decodes");
  pg::TransactionWriterBinding binding;
  binding.database_uuid = view.zero.record->bootstrap.database_uuid;
  d::FilespaceRootReference ledger_root;
  bool have_state = false, have_ledger = false;
  for (const auto& root : view.zero.record->roots) {
    if (root.kind == 4) {
      binding.state_root = root;
      have_state = true;
    }
    if (root.kind == 25) {
      ledger_root = root;
      have_ledger = true;
    }
  }
  Require(have_state && have_ledger, "page zero names the transaction inventory and the session ledger");
  BytesSink sink(view.image);
  const auto chains = pg::ReadTransactionChainsThroughSink(sink, binding);
  Require(chains.error == pg::TransactionWriterError::none, "the transaction chains read and check");
  view.counters = chains.counters;
  view.state_pages = chains.state_pages;
  view.header_pages = chains.header_pages;
  for (std::size_t i = 0; i < chains.state_pages.size(); ++i) {
    const auto page = pg::DecodeTransactionStatePage(
        std::span<const p::byte>(chains.state_images).subspan(i * chains.page_size, chains.page_size));
    Require(page.ok(), "a state page decodes");
    for (std::uint32_t slot = 0; slot < page.view->slots; ++slot) {
      const auto word = pg::TipWordAt(*page.view, slot);
      Require(word.has_value(), "every state word decodes");
      if (word->state != pg::TipStateCode::unused) view.words[page.view->first_number + slot] = *word;
    }
  }
  for (std::size_t i = 0; i < chains.header_pages.size(); ++i) {
    const auto page = pg::DecodeTransactionHeaderPage(
        std::span<const p::byte>(chains.header_images).subspan(i * chains.page_size, chains.page_size));
    Require(page.ok(), "a header page decodes");
    for (std::uint32_t slot = 0; slot < page.view->slots; ++slot)
      if (const auto record = pg::TransactionHeaderAt(*page.view, slot)) view.headers[page.view->first_number + slot] = *record;
  }
  p::u64 page = ledger_root.page_number;
  while (page != 0) {
    view.ledger_pages.push_back(page);
    const auto decoded = pg::DecodeSessionLedgerPage(std::span<const p::byte>(view.image).subspan(page * page_size, page_size));
    Require(decoded.ok(), "a ledger page decodes");
    for (std::uint32_t s = 0; s < decoded.view->used; ++s) view.ledger.push_back(*pg::SessionLedgerRecordAt(*decoded.view, s));
    page = decoded.view->next_page;
  }
  return view;
}

}  // namespace scratchbird::tests::offline
