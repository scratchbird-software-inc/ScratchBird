// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_COMPACT_TRANSACTION_STATE_PAGE, SB_TRANSACTION_HEADER_RECORD
// Codec coverage for the in-place transaction-state page and the transaction
// header record (owner rulings 2026-11-05). No caller uses either yet.
#include "careful_write.hpp"
#include "transaction_header_record.hpp"
#include "transaction_state_code_map.hpp"
#include "transaction_state_page.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
using disk::BoundedIoError;
using disk::BoundedIoOperation;
using disk::BoundedIoResult;
using page::TipPageError;
using page::TipStateCode;
using page::TransactionHeaderError;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::core::platform::Uuid;

unsigned checks = 0;
void Check(bool ok, std::string_view why, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) throw std::runtime_error(std::string(why) + " (line " + std::to_string(at.line()) + ")");
}
void CheckTip(TipPageError actual, TipPageError expected, std::string_view why,
              std::source_location at = std::source_location::current()) {
  Check(actual == expected, std::string(why) + ": expected " + page::TipPageErrorName(expected) + ", got " +
                                page::TipPageErrorName(actual),
        at);
}
void CheckHdr(TransactionHeaderError actual, TransactionHeaderError expected, std::string_view why,
              std::source_location at = std::source_location::current()) {
  Check(actual == expected, std::string(why) + ": expected " + page::TransactionHeaderErrorName(expected) +
                                ", got " + page::TransactionHeaderErrorName(actual),
        at);
}

// Deterministic v7 identities for tests: version 7, RFC variant.
Uuid V7(std::uint8_t tag) {
  Uuid id;
  for (std::size_t i = 0; i < 16; ++i) id.bytes[i] = static_cast<byte>(0x10 + tag + i);
  id.bytes[6] = 0x70 | (id.bytes[6] & 0x0f);
  id.bytes[8] = 0x80 | (id.bytes[8] & 0x3f);
  return id;
}
Uuid V4(std::uint8_t tag) {
  Uuid id = V7(tag);
  id.bytes[6] = 0x40 | (id.bytes[6] & 0x0f);
  return id;
}

constexpr u32 kPage = 8192;
const Uuid kDatabase = V7(1), kFilespace = V7(2);

disk::NativeCommonPageHeader Header(u64 page_number, u32 page_size = kPage) {
  disk::NativeCommonPageHeader h;
  h.page_size_bytes = page_size;
  h.page_type = page::kTransactionStatePageType;
  h.database_uuid = kDatabase;
  h.filespace_uuid = kFilespace;
  h.page_uuid = V7(static_cast<std::uint8_t>(40 + page_number));
  h.page_number = page_number;
  h.page_generation = 1;
  h.flags = 0;
  h.page_size_profile_uuid = disk::FindCanonicalFilespacePageProfileForSize(page_size)->uuid;
  return h;
}

std::vector<byte> Format(u64 page_number, u64 sequence, std::optional<page::TipCounters> counters,
                         u64 next_page = 0, u32 page_size = kPage) {
  page::TipPageFormat f;
  f.header = Header(page_number, page_size);
  f.tip_sequence = sequence;
  f.next_tip_page = next_page;
  f.counters = counters;
  std::vector<byte> image(page_size, 0xEE);
  CheckTip(page::FormatTransactionStatePage(f, image), TipPageError::none, "format");
  return image;
}
page::TipCounters Counters(u64 next) {
  page::TipCounters c;
  c.next_transaction = next;
  c.horizons = {next, next, next, next, next, next};
  return c;
}
page::TipPageDecodeResult Decode(const std::vector<byte>& image) { return page::DecodeTransactionStatePage(image); }

void TestGeometry() {
  Check(page::TipSlotsPerPage(8192) == 1920, "8 KiB slots");
  Check(page::TipSlotsPerPage(16384) == 3968, "16 KiB slots");
  Check(page::TipSlotsPerPage(4096) == 896, "4 KiB slots");
  Check(page::TipSlotsPerPage(512) == 0 && page::TipSlotsPerPage(0) == 0 && page::TipSlotsPerPage(8000) == 0,
        "unusable page sizes");
  const auto first = page::LocateTransactionStateSlot(kPage, 1);
  Check(first && first->tip_sequence == 0 && first->slot == 1, "order 1 -> page 0 slot 1");
  const auto last = page::LocateTransactionStateSlot(kPage, 1919);
  Check(last && last->tip_sequence == 0 && last->slot == 1919, "order 1919 -> page 0 slot 1919");
  const auto second = page::LocateTransactionStateSlot(kPage, 1920);
  Check(second && second->tip_sequence == 1 && second->slot == 0, "order 1920 -> page 1 slot 0");
  Check(!page::LocateTransactionStateSlot(kPage, 0), "order 0 is reserved");
  Check(!page::LocateTransactionStateSlot(4000, 5), "invalid page size has no location");
  static_assert(page::kTransactionStateWriteUnitBytes % page::kTransactionHeaderRecordBytes == 0,
                "a header record never straddles a write unit");
}

void TestFormatAndDecode() {
  for (const u32 size : {8192u, 16384u}) {
    const auto image = Format(7, 0, Counters(1), 0, size);
    const auto d = Decode(image);
    Check(d.ok(), "fresh first page decodes");
    Check(d.view->tip_sequence == 0 && d.view->first_number == 0 && d.view->slots == page::TipSlotsPerPage(size),
          "family fields");
    Check(d.view->counters && *d.view->counters == Counters(1), "counters round-trip");
    Check(d.view->next_tip_page == 0, "no next page");
    bool all_unused = true;
    for (u32 s = 0; s < d.view->slots; ++s) {
      const auto w = page::TipWordAt(*d.view, s);
      all_unused = all_unused && w && w->state == TipStateCode::unused;
    }
    Check(all_unused, "fresh words unused");
    // Magic at 128, words begin at 512, all words zero.
    Check(std::equal(page::kTransactionStatePageMagic.begin(), page::kTransactionStatePageMagic.end(),
                     image.begin() + 128),
          "magic at 128");
    Check(std::all_of(image.begin() + 512, image.end(), [](byte b) { return b == 0; }), "words zero");
  }
  const auto later = Format(9, 3, std::nullopt, 0);
  const auto d = Decode(later);
  Check(d.ok() && d.view->tip_sequence == 3 && d.view->first_number == 3 * 1920 && !d.view->counters,
        "later page decodes without counters");
  // A binding is honoured.
  disk::NativeCommonPageHeaderBinding binding;
  binding.page_number = 10;  // wrong page number among other unset fields
  Check(!page::DecodeTransactionStatePage(later, &binding).ok(), "binding mismatch refused");
}

void TestFormatRefusals() {
  page::TipPageFormat f;
  f.header = Header(5);
  f.counters = Counters(1);
  std::vector<byte> out(kPage, 0xAB);
  auto refused = [&](page::TipPageFormat g, TipPageError e, std::string_view why) {
    std::fill(out.begin(), out.end(), 0xAB);
    CheckTip(page::FormatTransactionStatePage(g, out), e, why);
    Check(std::all_of(out.begin(), out.end(), [](byte b) { return b == 0xAB; }), "output untouched on refusal");
  };
  auto g = f;
  g.header.page_type = 0x0302;
  refused(g, TipPageError::invalid_header, "wrong page type");
  g = f;
  g.header.flags = 1;
  refused(g, TipPageError::invalid_header, "header flags");
  g = f;
  g.counters.reset();
  refused(g, TipPageError::invalid_counters, "first page without counters");
  g = f;
  g.tip_sequence = 1;
  refused(g, TipPageError::invalid_counters, "later page with counters");
  g = f;
  g.counters->next_transaction = 0;
  g.counters->horizons = {0, 0, 0, 0, 0, 0};
  refused(g, TipPageError::invalid_counters, "Next 0");
  for (int i = 0; i < 6; ++i) {
    for (const u64 bad : {u64{0}, u64{2}}) {
      g = f;
      auto& h = g.counters->horizons;
      u64* fields[] = {&h.oldest_interesting, &h.oldest_active, &h.oldest_snapshot, &h.oldest_cleanup,
                       &h.oldest_archive_required, &h.oldest_limbo_resolution};
      *fields[i] = bad;
      refused(g, TipPageError::invalid_counters, "horizon outside [1, Next]");
    }
  }
  g = f;
  g.next_tip_page = 5;
  refused(g, TipPageError::invalid_link, "self link");
  std::vector<byte> small(kPage - 1, 0xAB);
  CheckTip(page::FormatTransactionStatePage(f, small), TipPageError::short_buffer, "short output");
  Check(std::all_of(small.begin(), small.end(), [](byte b) { return b == 0xAB; }), "short output untouched");
}

void TestDecodeRefusals() {
  const auto base = Format(5, 0, Counters(10));
  auto mutate = [&](auto change, TipPageError e, std::string_view why, std::optional<u32> slot = std::nullopt) {
    auto image = base;
    change(image);
    const auto d = Decode(image);
    CheckTip(d.error, e, why);
    Check(!d.view, "no view on refusal");
    if (slot) Check(d.slot == slot, std::string(why) + ": slot reported");
  };
  mutate([](auto& i) { i[100] ^= 1; }, TipPageError::invalid_header, "header checksum");
  mutate([](auto& i) { i[129] ^= 1; }, TipPageError::invalid_family, "magic");
  mutate([](auto& i) { i[136] = 2; }, TipPageError::invalid_family, "version");
  mutate([](auto& i) { i[140] ^= 1; }, TipPageError::invalid_family, "slot count");
  mutate([](auto& i) { i[152] = 1; }, TipPageError::invalid_family, "first number");
  mutate([](auto& i) { i[200] = 1; }, TipPageError::invalid_family, "family reserved byte");
  mutate([](auto& i) { i[400] = 1; }, TipPageError::invalid_counters, "counter reserved byte");
  mutate([](auto& i) { i[264] = 0; }, TipPageError::invalid_counters, "Next 0");
  mutate([](auto& i) { i[272] = 11; }, TipPageError::invalid_counters, "OIT beyond Next");
  mutate([](auto& i) { i[256] = 5; }, TipPageError::invalid_link, "self link");
  // Words: state 13, reserved flag bit, authority 3, unused with residue.
  mutate([](auto& i) { i[512 + 4 * 3] = 13; }, TipPageError::invalid_word, "state 13", 3);
  mutate([](auto& i) { i[512 + 4 * 3] = 6; i[512 + 4 * 3 + 1] = 0x20; }, TipPageError::invalid_word,
         "reserved flag", 3);
  mutate([](auto& i) { i[512 + 4 * 3] = 6; i[512 + 4 * 3 + 2] = 3; }, TipPageError::invalid_word, "authority 3", 3);
  mutate([](auto& i) { i[512 + 4 * 3 + 3] = 1; }, TipPageError::invalid_word, "unused with residue", 3);
  // Used slots: number 0, at Next, beyond Next.
  mutate([](auto& i) { i[512] = 6; }, TipPageError::invalid_slot, "transaction 0 used", 0);
  mutate([](auto& i) { i[512 + 4 * 10] = 1; }, TipPageError::invalid_slot, "slot at Next used", 10);
  mutate([](auto& i) { i[512 + 4 * 1000] = 9; }, TipPageError::invalid_slot, "slot beyond Next used", 1000);
  // Counters on a later page are refused.
  auto later = Format(6, 1, std::nullopt);
  later[264] = 1;
  CheckTip(Decode(later).error, TipPageError::invalid_counters, "counters on a later page");
  // Wrong image size.
  auto shorter = base;
  shorter.pop_back();
  Check(!Decode(shorter).ok(), "short image refused");
  // An unused slot below Next is legal (crash between allocation and slot write).
  auto image = base;
  image[512 + 4 * 2] = static_cast<byte>(TipStateCode::committed);
  image[512 + 4 * 2 + 1] = page::kTipFlagHasDetailRecord;
  const auto d = Decode(image);
  Check(d.ok(), "used slot 2 and unused slots 1, 3..9 below Next decode");
  const auto w = page::TipWordAt(*d.view, 2);
  Check(w && w->state == TipStateCode::committed && w->flags == page::kTipFlagHasDetailRecord, "word read back");
}

void TestWordCodec() {
  page::TipStateWord w;
  w.state = TipStateCode::limbo;
  w.flags = page::kTipFlagHasDetailRecord | page::kTipFlagHasLimboRecord;
  w.authority = page::TipAuthorityClass::cluster;
  w.intent = 63;
  w.epoch_delta = 200;
  const auto bytes = page::EncodeTipStateWord(w);
  Check(bytes && (*bytes)[0] == 10 && (*bytes)[1] == 3 && (*bytes)[2] == (1 | (63 << 2)) && (*bytes)[3] == 200,
        "word layout");
  const auto back = page::DecodeTipStateWord(*bytes);
  Check(back && *back == w, "word round-trip");
  auto bad = w;
  bad.intent = 64;
  Check(!page::EncodeTipStateWord(bad), "intent 64 refused");
  bad = w;
  bad.flags = 0x20;
  Check(!page::EncodeTipStateWord(bad), "reserved flag refused");
  bad = w;
  bad.authority = static_cast<page::TipAuthorityClass>(3);
  Check(!page::EncodeTipStateWord(bad), "authority 3 refused");
  bad = {};
  bad.epoch_delta = 1;
  Check(!page::EncodeTipStateWord(bad), "unused word with residue refused");
  for (int code = 0; code <= 12; ++code)
    Check(std::string(page::TipStateCodeName(static_cast<TipStateCode>(code))) != "invalid", "every code named");
}

void CheckUnit(const std::vector<byte>& before, const std::vector<byte>& after, const page::TipPageEdit& edit,
               std::string_view why) {
  Check(edit.ok(), std::string(why) + ": edit ok");
  Check(edit.unit_offset % page::kTransactionStateWriteUnitBytes == 0, std::string(why) + ": unit aligned");
  Check(std::equal(edit.preimage.begin(), edit.preimage.end(), before.begin() + edit.unit_offset),
        std::string(why) + ": preimage is the old unit");
  bool outside_same = before.size() == after.size();
  for (std::size_t i = 0; outside_same && i < after.size(); ++i) {
    const bool inside = i >= edit.unit_offset && i < edit.unit_offset + page::kTransactionStateWriteUnitBytes;
    outside_same = inside || before[i] == after[i];
  }
  Check(outside_same, std::string(why) + ": nothing changes outside the unit");
  Check(Decode(after).ok(), std::string(why) + ": the edited page decodes");
  Check(Decode(before).ok(), std::string(why) + ": the old page decodes");
}

void TestEdits() {
  auto image = Format(5, 0, Counters(1));
  // A slot at or beyond Next cannot be written: allocation comes first.
  page::TipStateWord starting{TipStateCode::starting, page::kTipFlagHasDetailRecord};
  CheckTip(page::SetTransactionStateWord(image, 1, starting).error, TipPageError::invalid_slot, "slot >= Next");
  CheckTip(page::SetTransactionStateWord(image, 0, starting).error, TipPageError::invalid_slot, "transaction 0");
  CheckTip(page::SetTransactionStateWord(image, 1920, starting).error, TipPageError::invalid_slot, "slot range");
  // Allocate: Next 1 -> 3, horizons follow.
  auto before = image;
  auto edit = page::SetTransactionStateCounters(image, Counters(3));
  CheckUnit(before, image, edit, "counters");
  Check(edit.changed && edit.unit_offset == 0, "counters live in unit 0");
  Check(page::SetTransactionStateCounters(image, Counters(3)).ok() &&
            !page::SetTransactionStateCounters(image, Counters(3)).changed,
        "repeating counters is a no-op");
  CheckTip(page::SetTransactionStateCounters(image, Counters(2)).error, TipPageError::next_regression,
           "Next never decreases");
  auto bad = Counters(4);
  bad.horizons.oldest_active = 5;
  CheckTip(page::SetTransactionStateCounters(image, bad).error, TipPageError::invalid_counters, "horizon > Next");
  // BEGIN writes the slot word after Next.
  before = image;
  edit = page::SetTransactionStateWord(image, 1, starting);
  CheckUnit(before, image, edit, "begin word");
  Check(edit.unit_offset == 512, "slot 1 is in unit 512");
  // COMMIT rewrites only the state byte.
  before = image;
  edit = page::SetTransactionStateCode(image, 1, TipStateCode::committed);
  CheckUnit(before, image, edit, "commit byte");
  bool one_byte = before[512 + 4] != image[512 + 4];
  for (std::size_t i = 0; one_byte && i < image.size(); ++i) one_byte = i == 512 + 4 || before[i] == image[i];
  Check(one_byte, "commit changes exactly one byte");
  Check(image[512 + 5] == page::kTipFlagHasDetailRecord, "flags kept");
  Check(!page::SetTransactionStateCode(image, 1, TipStateCode::committed).changed, "repeat commit is a no-op");
  CheckTip(page::SetTransactionStateCode(image, 1, TipStateCode::unused).error, TipPageError::invalid_word,
           "a used number never becomes unused (code)");
  page::TipStateWord unused{};
  CheckTip(page::SetTransactionStateWord(image, 1, unused).error, TipPageError::invalid_word,
           "a used number never becomes unused (word)");
  CheckTip(page::SetTransactionStateCode(image, 1, static_cast<TipStateCode>(13)).error, TipPageError::invalid_word,
           "code 13");
  // Dead-marking an unused slot below Next.
  before = image;
  edit = page::SetTransactionStateCode(image, 2, TipStateCode::dead);
  CheckUnit(before, image, edit, "dead-marking");
  // Linking the next page: once.
  before = image;
  edit = page::SetTransactionStateNextPage(image, 77);
  CheckUnit(before, image, edit, "link");
  Check(edit.unit_offset == 0, "link lives in unit 0");
  Check(page::SetTransactionStateNextPage(image, 77).ok() && !page::SetTransactionStateNextPage(image, 77).changed,
        "same link is a no-op");
  CheckTip(page::SetTransactionStateNextPage(image, 78).error, TipPageError::invalid_link, "link written once");
  CheckTip(page::SetTransactionStateNextPage(image, 5).error, TipPageError::invalid_link, "self link");
  CheckTip(page::SetTransactionStateNextPage(image, 0).error, TipPageError::invalid_link, "zero link");
  // Counters only on the first page; a later page accepts any slot (Next is checked by the chain reader).
  auto later = Format(6, 1, std::nullopt);
  CheckTip(page::SetTransactionStateCounters(later, Counters(5)).error, TipPageError::invalid_counters,
           "counters on a later page");
  before = later;
  edit = page::SetTransactionStateWord(later, 0, starting);
  CheckUnit(before, later, edit, "later page slot 0 (order 1920)");
  const auto view = Decode(later);
  Check(view.ok(), "later page decodes");
  CheckTip(page::CheckTransactionStateSlotsBelowNext(*view.view, 1920).error, TipPageError::invalid_slot,
           "slot at Next on a later page");
  Check(page::CheckTransactionStateSlotsBelowNext(*view.view, 1921).ok(), "slot below Next on a later page");
  // A foreign image is refused by every edit and left unchanged.
  std::vector<byte> foreign(kPage, 0);
  const auto copy = foreign;
  CheckTip(page::SetTransactionStateWord(foreign, 1, starting).error, TipPageError::invalid_family, "foreign word");
  CheckTip(page::SetTransactionStateCode(foreign, 1, TipStateCode::dead).error, TipPageError::invalid_family,
           "foreign code");
  CheckTip(page::SetTransactionStateCounters(foreign, Counters(2)).error, TipPageError::invalid_family,
           "foreign counters");
  CheckTip(page::SetTransactionStateNextPage(foreign, 3).error, TipPageError::invalid_family, "foreign link");
  Check(foreign == copy, "foreign image untouched");
}

// A medium that accepts only the first `budget` writes, then fails: the
// process-loss prefix of a careful-write plan.
class PrefixSink final : public disk::CarefulWriteSink {
 public:
  PrefixSink(std::vector<byte> medium, int budget) : medium(std::move(medium)), budget_(budget) {}
  BoundedIoResult WriteAt(u64 offset, std::span<const byte> bytes) override {
    BoundedIoResult r;
    r.operation = BoundedIoOperation::write;
    r.offset = offset;
    r.requested_bytes = bytes.size();
    if (budget_-- <= 0) {
      r.error = BoundedIoError::native_failure;
      return r;
    }
    std::memcpy(medium.data() + offset, bytes.data(), bytes.size());
    r.bytes_transferred = bytes.size();
    return r;
  }
  BoundedIoResult ReadAt(u64 offset, std::span<byte> bytes) override {
    BoundedIoResult r;
    r.operation = BoundedIoOperation::read;
    r.offset = offset;
    r.requested_bytes = bytes.size();
    std::memcpy(bytes.data(), medium.data() + offset, bytes.size());
    r.bytes_transferred = bytes.size();
    return r;
  }
  BoundedIoResult Sync() override {
    BoundedIoResult r;
    r.operation = BoundedIoOperation::sync;
    r.synchronized = true;
    return r;
  }
  bool forced_ordered() const noexcept override { return true; }
  std::vector<byte> medium;

 private:
  int budget_;
};

std::vector<byte> PageOf(const std::vector<byte>& medium, u64 page_number) {
  return {medium.begin() + page_number * kPage, medium.begin() + (page_number + 1) * kPage};
}

// Walk the chain from page 1 and require every page to decode, every used
// slot to lie below Next, and the walk to end. Returns the pages walked.
int WalkChain(const std::vector<byte>& medium) {
  auto first = PageOf(medium, 1);
  const auto d = Decode(first);
  Check(d.ok(), "first page decodes");
  const u64 next = d.view->counters->next_transaction;
  int pages = 1;
  u64 link = d.view->next_tip_page;
  u64 sequence = 0;
  while (link) {
    const auto bytes = PageOf(medium, link);
    const auto p = Decode(bytes);
    Check(p.ok(), "linked page decodes");
    Check(p.view->tip_sequence == ++sequence, "sequence advances by one");
    Check(page::CheckTransactionStateSlotsBelowNext(*p.view, next).ok(), "linked page slots below Next");
    link = p.view->next_tip_page;
    Check(++pages < 8, "chain terminates");
  }
  return pages;
}

void TestCrashPrefixes() {
  // Medium: page 0 unused, page 1 = first state page, page 2 = spare.
  std::vector<byte> medium(3 * kPage, 0);
  {
    auto first = Format(1, 0, Counters(1919));
    std::copy(first.begin(), first.end(), medium.begin() + kPage);
  }
  // Extend then BEGIN of transaction 1919 and 1920 (first slot of page 2):
  // new page -> link on the prior page -> Next -> slot word.
  auto first = PageOf(medium, 1);
  const auto spare = Format(2, 1, std::nullopt);
  disk::CarefulWritePlan plan;
  const auto spare_node = plan.Add(2 * kPage, spare, std::span<const byte>(medium.data() + 2 * kPage, kPage));
  Check(spare_node.ok(), "spare node");
  auto link_edit = page::SetTransactionStateNextPage(first, 2);
  Check(link_edit.ok(), "link edit");
  const auto link_node = plan.Add(kPage + link_edit.unit_offset,
                                  std::span<const byte>(first.data() + link_edit.unit_offset, 512),
                                  std::span<const byte>(link_edit.preimage));
  Check(link_node.ok(), "link node");
  Check(plan.Before(spare_node.node, link_node.node) == disk::CarefulWriteError::none, "spare before link");
  // Next and the link share unit 0 of page 1, so the Next change is a second
  // plan that runs after the first (one node per extent).
  for (int budget = 0; budget <= 2; ++budget) {
    PrefixSink sink(medium, budget);
    const auto result = plan.Execute(sink);
    Check(result.ok() == (budget >= 2), "extend plan stops at the budget");
    Check(WalkChain(sink.medium) == (budget >= 2 ? 2 : 1), "chain length after the prefix");
  }
  PrefixSink extended(medium, 2);
  Check(plan.Execute(extended).ok(), "extend completes");
  // BEGIN of 1920 on page 2: Next (page 1 unit 0) -> slot word (page 2 unit 512).
  auto first_now = PageOf(extended.medium, 1);
  auto second_now = PageOf(extended.medium, 2);
  const auto next_edit = page::SetTransactionStateCounters(first_now, Counters(1921));
  Check(next_edit.ok(), "Next edit");
  const auto word_edit =
      page::SetTransactionStateWord(second_now, 0, {TipStateCode::starting, page::kTipFlagHasDetailRecord});
  Check(word_edit.ok(), "slot edit");
  disk::CarefulWritePlan begin;
  const auto next_node = begin.Add(kPage + next_edit.unit_offset,
                                   std::span<const byte>(first_now.data() + next_edit.unit_offset, 512),
                                   std::span<const byte>(next_edit.preimage));
  const auto word_node = begin.Add(2 * kPage + word_edit.unit_offset,
                                   std::span<const byte>(second_now.data() + word_edit.unit_offset, 512),
                                   std::span<const byte>(word_edit.preimage));
  Check(next_node.ok() && word_node.ok(), "begin nodes");
  Check(begin.Before(next_node.node, word_node.node) == disk::CarefulWriteError::none, "Next before slot");
  for (int budget = 0; budget <= 2; ++budget) {
    PrefixSink sink(extended.medium, budget);
    begin.Execute(sink);
    Check(WalkChain(sink.medium) == 2, "every BEGIN prefix keeps a valid chain");
    const auto p2 = Decode(PageOf(sink.medium, 2));
    const auto w = page::TipWordAt(*p2.view, 0);
    Check(w && w->state == (budget >= 2 ? TipStateCode::starting : TipStateCode::unused),
          "slot written only after Next");
  }
  // The reverse order (slot before Next) would leave a used slot at Next: the
  // reader refuses it, which is why allocation comes first.
  {
    PrefixSink sink(extended.medium, 1);
    disk::CarefulWritePlan wrong;
    const auto w = wrong.Add(2 * kPage + word_edit.unit_offset,
                             std::span<const byte>(second_now.data() + word_edit.unit_offset, 512),
                             std::span<const byte>(word_edit.preimage));
    Check(w.ok(), "wrong-order node");
    wrong.Execute(sink);
    const auto p1 = Decode(PageOf(sink.medium, 1));
    const auto p2 = Decode(PageOf(sink.medium, 2));
    Check(p1.ok() && p1.view->counters->next_transaction == 1919, "Next not yet advanced");
    Check(p2.ok() && !page::CheckTransactionStateSlotsBelowNext(*p2.view, 1919).ok(),
          "slot before Next is detected");
  }
}

page::TransactionHeaderRecord SampleHeader() {
  page::TransactionHeaderRecord r;
  r.authority = page::TipAuthorityClass::local;
  r.intent = 5;
  r.isolation_profile = 2;
  r.read_only = false;
  r.transaction_order = 1920;
  r.database_uuid = kDatabase;
  r.transaction_uuid = V7(60);
  r.engine_instance_uuid = V7(61);
  r.principal_or_agent_uuid = V7(62);
  r.session_uuid = V7(63);
  r.policy_snapshot_uuid = V7(64);
  r.security_snapshot_uuid = V7(65);
  r.start_snapshot_uuid = V7(66);
  return r;
}

void TestHeaderRecord() {
  auto r = SampleHeader();
  std::array<byte, page::kTransactionHeaderRecordBytes> bytes{};
  CheckHdr(page::EncodeTransactionHeaderRecord(r, bytes), TransactionHeaderError::none, "encode");
  using scratchbird::core::platform::LoadLittle16;
  using scratchbird::core::platform::LoadLittle64;
  Check(LoadLittle16(bytes.data()) == 1 && LoadLittle16(bytes.data() + 2) == 256, "version and size");
  Check(bytes[4] == 0 && bytes[5] == 5 && bytes[6] == 2 && bytes[7] == 0, "class, intent, isolation, flags");
  Check(LoadLittle64(bytes.data() + 8) == 1920, "transaction_order at +8");
  Check(std::equal(kDatabase.bytes.begin(), kDatabase.bytes.end(), bytes.begin() + 16), "database UUID at +16");
  Check(std::equal(r.transaction_uuid.bytes.begin(), r.transaction_uuid.bytes.end(), bytes.begin() + 32),
        "transaction UUID at +32");
  Check(std::all_of(bytes.begin() + 80, bytes.begin() + 96, [](byte b) { return b == 0; }), "no attachment");
  Check(LoadLittle64(bytes.data() + 240) == 0 && LoadLittle64(bytes.data() + 248) == 0, "not complete");
  auto decoded = page::DecodeTransactionHeaderRecord(bytes);
  Check(decoded.ok() && *decoded.record == r, "round trip");

  // Completion: commit sets both times; once only.
  const u64 when = 1'800'000'000'000'000'000ull;
  CheckHdr(page::CompleteTransactionHeader(r, TipStateCode::committed, when), TransactionHeaderError::none, "commit");
  Check(r.final_state == TipStateCode::committed && r.commit_time_utc_ns == when && r.completed_at_utc_ns == when,
        "commit time recorded");
  CheckHdr(page::CompleteTransactionHeader(r, TipStateCode::rolled_back, when + 1),
           TransactionHeaderError::already_complete, "complete once");
  CheckHdr(page::UpgradeTransactionHeaderIntent(r, 6), TransactionHeaderError::already_complete,
           "no intent change after completion");
  CheckHdr(page::EncodeTransactionHeaderRecord(r, bytes), TransactionHeaderError::none, "encode committed");
  Check(LoadLittle64(bytes.data() + 240) == when && bytes[236] == 6, "commit time at +240, state at +236");
  decoded = page::DecodeTransactionHeaderRecord(bytes);
  Check(decoded.ok() && *decoded.record == r, "committed round trip");

  auto rb = SampleHeader();
  CheckHdr(page::CompleteTransactionHeader(rb, TipStateCode::rolled_back, when), TransactionHeaderError::none,
           "rollback");
  Check(rb.commit_time_utc_ns == 0 && rb.completed_at_utc_ns == when, "rollback has no commit time");
  auto dead = SampleHeader();
  CheckHdr(page::CompleteTransactionHeader(dead, TipStateCode::dead, when), TransactionHeaderError::none, "dead");
  auto bad = SampleHeader();
  CheckHdr(page::CompleteTransactionHeader(bad, TipStateCode::active, when), TransactionHeaderError::invalid_completion,
           "active is not final");
  CheckHdr(page::CompleteTransactionHeader(bad, TipStateCode::committed, 0), TransactionHeaderError::invalid_completion,
           "zero time");
  Check(bad == SampleHeader(), "refused completion leaves the record unchanged");

  // Intent changes.
  auto up = SampleHeader();
  CheckHdr(page::UpgradeTransactionHeaderIntent(up, 9), TransactionHeaderError::none, "intent change");
  Check(up.intent == 9 && up.intent_generation == 1, "generation advances");
  CheckHdr(page::UpgradeTransactionHeaderIntent(up, 9), TransactionHeaderError::invalid_class, "same intent");
  CheckHdr(page::UpgradeTransactionHeaderIntent(up, 64), TransactionHeaderError::invalid_class, "intent 64");

  // Validation refusals.
  auto refuse = [&](auto change, TransactionHeaderError e, std::string_view why) {
    auto x = SampleHeader();
    change(x);
    CheckHdr(page::ValidateTransactionHeaderRecord(x), e, why);
    std::array<byte, page::kTransactionHeaderRecordBytes> out;
    out.fill(0xCD);
    CheckHdr(page::EncodeTransactionHeaderRecord(x, out), e, why);
    Check(std::all_of(out.begin(), out.end(), [](byte b) { return b == 0xCD; }), "output untouched on refusal");
  };
  refuse([](auto& x) { x.transaction_order = 0; }, TransactionHeaderError::invalid_order, "order 0");
  refuse([](auto& x) { x.database_uuid = Uuid{}; }, TransactionHeaderError::invalid_identity, "nil database");
  refuse([](auto& x) { x.transaction_uuid = V4(9); }, TransactionHeaderError::invalid_identity, "v4 transaction");
  refuse([](auto& x) { x.session_uuid = V4(9); }, TransactionHeaderError::invalid_optional_identity, "v4 session");
  refuse([](auto& x) { x.cluster_epoch = 3; }, TransactionHeaderError::invalid_cluster, "epoch without cluster");
  refuse([](auto& x) { x.cluster_transaction_uuid = V7(70); }, TransactionHeaderError::invalid_cluster,
         "cluster transaction without cluster");
  refuse([](auto& x) { x.authority = static_cast<page::TipAuthorityClass>(3); }, TransactionHeaderError::invalid_class,
         "authority 3");
  refuse([](auto& x) { x.intent = 64; }, TransactionHeaderError::invalid_class, "intent 64");
  refuse([](auto& x) { x.commit_time_utc_ns = 5; }, TransactionHeaderError::invalid_completion,
         "commit time without completion");
  refuse([](auto& x) { x.final_state = TipStateCode::committed; x.completed_at_utc_ns = 5; x.commit_time_utc_ns = 6; },
         TransactionHeaderError::invalid_completion, "commit time differs from completion");
  refuse([](auto& x) { x.final_state = TipStateCode::rolled_back; x.completed_at_utc_ns = 5; x.commit_time_utc_ns = 5; },
         TransactionHeaderError::invalid_completion, "rollback with a commit time");
  refuse([](auto& x) { x.final_state = TipStateCode::dead; }, TransactionHeaderError::invalid_completion,
         "dead without completion time");
  refuse([](auto& x) { x.final_state = TipStateCode::prepared; x.completed_at_utc_ns = 5; },
         TransactionHeaderError::invalid_completion, "prepared is not final");
  // A cluster-scoped header is accepted.
  auto cluster = SampleHeader();
  cluster.authority = page::TipAuthorityClass::cluster;
  cluster.cluster_uuid = V7(71);
  cluster.cluster_transaction_uuid = V7(72);
  cluster.cluster_epoch = 4;
  CheckHdr(page::ValidateTransactionHeaderRecord(cluster), TransactionHeaderError::none, "cluster header");

  // Decode refusals.
  CheckHdr(page::EncodeTransactionHeaderRecord(SampleHeader(), bytes), TransactionHeaderError::none, "re-encode");
  auto corrupt = [&](std::size_t at, byte value, std::string_view why) {
    auto x = bytes;
    x[at] = value;
    Check(!page::DecodeTransactionHeaderRecord(x).ok(), why);
  };
  corrupt(0, 2, "version 2");
  corrupt(3, 0, "record size");
  corrupt(4, 3, "authority 3");
  corrupt(7, 2, "reserved flag bit");
  corrupt(236, 13, "final state 13");
  corrupt(237, 1, "reserved byte 237");
  {
    auto x = bytes;
    x[8] = 0;
    x[9] = 0;  // 1920 = 0x0780
    Check(!page::DecodeTransactionHeaderRecord(x).ok(), "order 0");
  }
  Check(!page::DecodeTransactionHeaderRecord(std::span<const byte>(bytes.data(), 255)).ok(), "short record");
  std::array<byte, 255> small{};
  CheckHdr(page::EncodeTransactionHeaderRecord(SampleHeader(), small), TransactionHeaderError::short_buffer,
           "short output");
}

void TestStateCodeMap() {
  using scratchbird::transaction::mga::TransactionState;
  // Durable -> in-memory -> durable is the identity for every code.
  for (int c = 0; c <= page::kTipStateCodeMax; ++c) {
    const auto code = static_cast<TipStateCode>(c);
    const auto state = page::InMemoryTransactionState(code);
    Check(state.has_value(), "every durable code maps");
    Check(page::DurableTipStateCode(*state) == code, "durable round trip");
  }
  Check(!page::InMemoryTransactionState(static_cast<TipStateCode>(13)), "code 13 has no state");
  // The named differences.
  Check(page::DurableTipStateCode(TransactionState::created) == TipStateCode::starting, "created is starting");
  Check(page::DurableTipStateCode(TransactionState::failed_terminal) == TipStateCode::dead, "failed_terminal is dead");
  Check(page::DurableTipStateCode(TransactionState::preparing) == TipStateCode::active, "preparing stores as active");
  Check(page::DurableTipStateCode(TransactionState::none) == TipStateCode::unused, "none is unused");
  // In-memory -> durable -> in-memory is the identity except preparing.
  for (int s = 0; s <= static_cast<int>(TransactionState::read_only_active); ++s) {
    const auto state = static_cast<TransactionState>(s);
    const auto back = page::InMemoryTransactionState(page::DurableTipStateCode(state));
    Check(back.has_value(), "every state maps back");
    Check(*back == (state == TransactionState::preparing ? TransactionState::active : state), "state round trip");
  }
}
}  // namespace

int main() {
  try {
    TestGeometry();
    TestFormatAndDecode();
    TestFormatRefusals();
    TestDecodeRefusals();
    TestWordCodec();
    TestEdits();
    TestCrashPrefixes();
    TestHeaderRecord();
    TestStateCodeMap();
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << " after " << checks << " checks\n";
    return 1;
  }
  std::cout << "transaction_state_page: " << checks << " checks passed\n";
  return 0;
}
