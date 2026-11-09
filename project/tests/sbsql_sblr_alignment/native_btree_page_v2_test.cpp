// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_BTREE_COMPRESSED_PAGE
// SBBTP002, the prefix-compressed catalog B-tree page (owner rulings Q37 = 1,
// Q38 = 1, Q40, 2026-11-09), against an independent byte-packing oracle written
// here (its own LEB128, shared-prefix and layout rules and OpenSSL SHA-256; the
// production codec only supplies the 128-byte common header, another format),
// at every canonical page size:
//   - root, branch and leaf pages; keys that are their own row UUIDs (the row
//     UUID then not stored twice) and composite keys; more cells than one jump
//     interval (restart cells and the jump table); deleted cells; references
//     in this page's filespace (compact) and elsewhere (full); creators whose
//     LEB128 spans one, two and three bytes; payloads; fences; an empty root;
//   - encode gives the oracle's bytes, both decoders accept them, and encoding
//     the decoded page gives the bytes back (canonical);
//   - the allocation-free decoder rebuilds keys into its key workspace (none
//     borrow the image, every payload does) with no heap allocation, on pages
//     packed full with the smallest cells (the workspace bound);
//   - limits: a key (or key plus payload) above a quarter page is key_too_long;
//     rebuilt keys above one page, or a full page, are resource_exhausted;
//   - every malformed field, resealed, is refused by both decoders alike:
//     framing, flags, jump interval, jump table start/count/offsets, rebuilt
//     byte count, reserved and trailing bytes, cell flags, overlong LEB128,
//     a restart cell sharing bytes, a shared count above the previous key or
//     below the longest shared prefix, row UUIDs stored when they are the keys
//     (and the reverse), a creator of zero, a full reference that could be
//     compact, an empty payload with its flag;
//   - the SBBTP001 encoder refuses a cell's creator or payload.
#include "native_index_btree_page.hpp"
#include "native_common_page_header.hpp"
#include "filespace_page_zero.hpp"
#include "../support/native_btree_page_oracle.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace d = scratchbird::storage::disk;
namespace pg = scratchbird::storage::page;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::core::platform::Uuid;
using Bytes = std::vector<byte>;

// Heap allocations counted while `counting` is set.
std::atomic<bool> counting{false};
std::atomic<u64> allocations{0};
void* operator new(std::size_t n) {
  if (counting) ++allocations;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {
u64 checks = 0;
void Check(bool ok, const std::string& what, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) throw std::runtime_error(what + " (line " + std::to_string(at.line()) + ")");
}
Uuid Id(u64 n, byte tag = 0) {
  Uuid u{};
  const u64 t = 0x010000000000ULL + n;  // a 48-bit time field, increasing with n
  for (int i = 0; i < 6; ++i) u.bytes[i] = static_cast<byte>(t >> (8 * (5 - i)));
  u.bytes[6] = 0x70;
  u.bytes[7] = tag;
  u.bytes[8] = 0x80;
  for (int i = 0; i < 7; ++i) u.bytes[9 + i] = static_cast<byte>(n >> (8 * i));
  return u;
}
const Uuid kFilespace = Id(2), kOther = Id(7);
Uuid Profile(unsigned p) { return d::kCanonicalFilespacePageProfiles[p].uuid; }
u32 Size(unsigned p) { return d::kCanonicalFilespacePageProfiles[p].page_size_bytes; }
d::NativePageReference Here(unsigned p, u64 n) { return {kFilespace, n, 3, Profile(p)}; }
d::NativePageReference Elsewhere(unsigned p, u64 n) { return {kOther, n, 4, Profile((p + 1) % 5)}; }

// ---- The oracle: tests/support/native_btree_page_oracle.hpp -------------------
using scratchbird::tests::btree_oracle::Knobs;
using scratchbird::tests::btree_oracle::Le;
using scratchbird::tests::btree_oracle::Pack;
using scratchbird::tests::btree_oracle::Seal;

// ---- Example pages -----------------------------------------------------------
pg::NativeBtreePage Frame(unsigned p, u32 type, u16 level) {
  pg::NativeBtreePage r;
  r.header = {Size(p), type, Id(1), kFilespace, Id(90), 12, 102, 0, Profile(p)};
  r.dependencies = {Id(42), 3, 8, Id(43), Id(44), Id(45), {}};
  r.dependencies.dependency_map_sha256.fill(19);
  r.creator_transaction_uuid = Id(98);
  r.creator_local_transaction_id = 17;
  r.maintenance_state = 1;
  r.tree_level = level;
  if (type != 0x200) {
    r.parent = Here(p, 9);
    r.left = Here(p, 10);
    r.right = Here(p, 11);
  }
  if (level) r.first_child = Here(p, 20);
  return r;
}
pg::NativeBtreeKey RowKey(u64 n, u64 version) {
  pg::NativeBtreeKey k;
  k.row_uuid = Id(n);
  k.encoded_key.assign(k.row_uuid.bytes.begin(), k.row_uuid.bytes.end());
  k.version_uuid = Id(version, 1);
  return k;
}
// A root leaf of row-UUID keys: 40 cells (three restart cells), some deleted,
// compact and full references, creators of one, two and three LEB128 bytes.
pg::NativeBtreePage RowLeaf(unsigned p) {
  auto r = Frame(p, 0x200, 0);
  for (u64 i = 0; i < 40; ++i) {
    pg::NativeBtreeCell c;
    c.key = RowKey(1000 + i * 3, 5000 + i);
    c.deleted = i % 7 == 3;
    c.base_page = i % 5 == 4 ? Elsewhere(p, 50 + i) : Here(p, 30 + i % 3);
    c.creator_local_transaction_id = i % 3 == 0 ? 1 : i % 3 == 1 ? 300 : 70000 + i;
    r.cells.push_back(c);
  }
  return r;
}
// A leaf of composite keys with shared prefixes, payloads and fences.
pg::NativeBtreeKey NameKey(const std::string& text, u64 row) {
  pg::NativeBtreeKey k;
  const Uuid scope = Id(77), profile = Id(78);
  k.encoded_key.assign(scope.bytes.begin(), scope.bytes.end());
  k.encoded_key.insert(k.encoded_key.end(), profile.bytes.begin(), profile.bytes.end());
  for (const char ch : std::string("en")) k.encoded_key.push_back(static_cast<byte>(ch));
  k.encoded_key.push_back(0);
  k.encoded_key.push_back(0);
  for (const char ch : text) k.encoded_key.push_back(static_cast<byte>(ch));
  k.encoded_key.push_back(0);
  k.encoded_key.push_back(0);
  k.encoded_key.push_back(42);
  k.row_uuid = Id(row);
  k.version_uuid = Id(row, 2);
  return k;
}
pg::NativeBtreePage NameLeaf(unsigned p) {
  auto r = Frame(p, 0x202, 0);
  const std::vector<std::string> names{"account", "accounts", "accrual", "audit", "audit", "b", "zeta"};
  r.low_fence = NameKey("aa", 1);
  r.high_fence = NameKey("zz", 1);
  for (std::size_t i = 0; i < names.size(); ++i) {
    pg::NativeBtreeCell c;
    c.key = NameKey(names[i], 200 + i);
    c.base_page = i == 2 ? Elsewhere(p, 77) : Here(p, 60);
    c.creator_local_transaction_id = 2 + i;
    if (i % 2 == 0) {
      const Uuid object = Id(300 + i);
      c.payload.assign(object.bytes.begin(), object.bytes.end());
      for (char ch : names[i]) c.payload.push_back(static_cast<byte>(ch));
    }
    r.cells.push_back(c);
  }
  return r;
}
// A branch with compact and full child references and fences.
pg::NativeBtreePage Branch(unsigned p) {
  auto r = Frame(p, 0x201, 1);
  r.low_fence = RowKey(500, 1);
  r.high_fence = RowKey(900, 1);
  for (u64 i = 0; i < 20; ++i) {
    pg::NativeBtreeCell c;
    c.key = RowKey(510 + i * 10, 2);
    c.child = i == 5 ? Elsewhere(p, 88) : Here(p, 21 + i);
    r.cells.push_back(c);
  }
  return r;
}

u64 Get(const Bytes& b, std::size_t at, unsigned n);
// ---- Checks ------------------------------------------------------------------
struct Workspace {
  std::vector<pg::NativeBtreeCellView> cells;
  std::vector<pg::NativeBtreeReferenceSlot> references;
  Bytes keys;
  explicit Workspace(std::size_t bytes) {
    const auto need = pg::NativeBtreePageViewBackingRequirements(bytes);
    cells.resize(need.cells);
    references.resize(need.reference_slots);
    keys.resize(need.key_bytes);
  }
  pg::NativeBtreeViewWorkspace get() { return {cells, references, keys}; }
};
template <class A, class B> bool SameBytes(const A& a, const B& b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end());
}
template <class Key> bool SameKey(const pg::NativeBtreeKey& a, const Key& b) {
  return a.row_uuid == b.row_uuid && a.version_uuid == b.version_uuid && SameBytes(a.encoded_key, b.encoded_key);
}
// Encode gives the oracle's bytes; both decoders accept them and agree with
// the page; the decoded page encodes back to the same bytes.
void Accepts(const pg::NativeBtreePage& page, const std::string& label) {
  const auto bytes = Pack(page);
  const auto enc = pg::EncodeNativeBtreePage(page);
  if (!enc.ok()) std::cerr << label << ": encode error " << int(enc.error) << '\n';
  Check(enc.ok() && enc.bytes == bytes, label + ": encode gives the oracle's bytes");
  const auto dec = pg::DecodeNativeBtreePage(bytes);
  if (!dec.ok()) {
    std::cerr << label << ": decode error " << int(dec.error) << " used=" << Get(bytes, 140, 4) << " count=" << Get(bytes, 496, 4)
              << " deleted=" << Get(bytes, 500, 4) << " table=" << Get(bytes, 504, 4) << " jumps=" << Get(bytes, 508, 4)
              << " expanded=" << Get(bytes, 544, 4) << " form=" << Get(bytes, 284, 2) << " interval=" << Get(bytes, 286, 2) << '\n';
  }
  Check(dec.ok(), label + ": the owning decoder accepts it");
  const auto& got = *dec.page;
  Check(got.cells.size() == page.cells.size() && got.tree_level == page.tree_level && got.parent == page.parent &&
            got.first_child == page.first_child && got.low_fence == page.low_fence && got.high_fence == page.high_fence,
        label + ": the decoded frame equals the page");
  for (std::size_t i = 0; i < page.cells.size(); ++i) {
    const auto& a = page.cells[i];
    const auto& b = got.cells[i];
    Check(SameKey(a.key, b.key) && a.deleted == b.deleted && a.child == b.child && a.base_page == b.base_page &&
              a.creator_local_transaction_id == b.creator_local_transaction_id && a.payload == b.payload,
          label + ": every decoded cell equals the page's");
  }
  const auto again = pg::EncodeNativeBtreePage(got);
  Check(again.ok() && again.bytes == bytes, label + ": encoding the decoded page gives the bytes back");
  Workspace w(bytes.size());
  counting = true;
  allocations = 0;
  const auto view = pg::DecodeNativeBtreePageInto(bytes, w.get());
  counting = false;
  Check(view.ok() && allocations == 0, label + ": the allocation-free decoder accepts it with no heap allocation");
  const auto in = [](const auto& span, const auto& region) {
    return span.empty() || (span.data() >= region.data() && span.data() + span.size() <= region.data() + region.size());
  };
  for (std::size_t i = 0; i < page.cells.size(); ++i) {
    const auto& v = view.page->cells[i];
    Check(SameKey(page.cells[i].key, v.key) && SameBytes(page.cells[i].payload, v.payload) &&
              v.creator_local_transaction_id == page.cells[i].creator_local_transaction_id &&
              v.base_page == page.cells[i].base_page && v.child == page.cells[i].child,
          label + ": every borrowed cell equals the page's");
    Check(in(v.key.encoded_key, w.keys) && in(v.payload, bytes), label + ": keys rebuilt in the workspace, payloads borrowed");
  }
}
// Both decoders refuse the bytes with `error`.
void Refuses(const Bytes& bytes, pg::NativeBtreeError error, const std::string& label) {
  const auto dec = pg::DecodeNativeBtreePage(bytes);
  Workspace w(bytes.size());
  const auto view = pg::DecodeNativeBtreePageInto(bytes, w.get());
  if (dec.error != error || view.error != error)
    std::cerr << label << ": owning " << int(dec.error) << " borrowed " << int(view.error) << " expected " << int(error) << '\n';
  Check(!dec.ok() && !view.ok() && dec.error == error && view.error == error && !view.page && view.bytes.empty(),
        label + ": both decoders refuse it alike");
}
Bytes Resealed(Bytes b, std::size_t at, u64 v, unsigned n) {
  Le(b, at, v, n);
  Seal(b);
  return b;
}
u64 Get(const Bytes& b, std::size_t at, unsigned n) {
  u64 v = 0;
  for (unsigned i = 0; i < n; ++i) v |= u64(b[at + i]) << (8 * i);
  return v;
}

void Malformed(unsigned p) {
  using E = pg::NativeBtreeError;
  const auto leaf = NameLeaf(p);
  const auto rows = RowLeaf(p);
  const auto good = Pack(leaf), row_bytes = Pack(rows);
  Refuses(Resealed(good, 128, 0x31, 1), E::invalid_family, "magic");
  Refuses(Resealed(good, 136, 1, 2), E::invalid_family, "version 1");
  Refuses(Resealed(good, 138, 256, 2), E::invalid_family, "header bytes");
  Refuses(Resealed(good, 284, 2, 2), E::invalid_family, "an unknown key-form flag");
  Refuses(Resealed(good, 284, 1, 2), E::invalid_family, "row_is_key on composite keys");
  Refuses(Resealed(good, 286, 8, 2), E::invalid_family, "a jump interval other than 16");
  Refuses(Resealed(good, 504, Get(good, 504, 4) + 4, 4), E::invalid_family, "the jump table moved");
  Refuses(Resealed(row_bytes, 508, 2, 4), E::invalid_family, "the jump count");
  Refuses(Resealed(row_bytes, Get(row_bytes, 504, 4) + 4, Get(row_bytes, Get(row_bytes, 504, 4) + 4, 4) + 1, 4),
          E::invalid_family, "a jump offset");
  Refuses(Resealed(good, 544, Get(good, 544, 4) + 1, 4), E::invalid_family, "the rebuilt byte count");
  Refuses(Resealed(good, 548, 1, 1), E::invalid_family, "a reserved byte");
  Refuses(Resealed(good, Get(good, 140, 4), 1, 1), E::invalid_family, "a byte after the used end");
  {
    const auto first = Get(good, Get(good, 504, 4), 4);  // the first cell's flags
    Refuses(Resealed(good, first, Get(good, first, 1) | 8, 1), E::invalid_family, "an unknown cell flag");
    const auto branch = Pack(Branch(p));
    const auto at = Get(branch, Get(branch, 504, 4), 4);
    Refuses(Resealed(branch, at, Get(branch, at, 1) | 1, 1), E::invalid_family, "a deleted branch cell");
    Refuses(Resealed(branch, at, Get(branch, at, 1) | 4, 1), E::invalid_family, "a branch cell with a payload flag");
  }
  Refuses(Pack(leaf, {.overlong_shared_at = 1}), E::invalid_family, "an overlong LEB128");
  Refuses(Pack(leaf, {.short_shared_at = 1}), E::invalid_family, "a shared count below the longest shared prefix");
  Refuses(Pack(rows, {.full_references = true}), E::invalid_family, "a full reference that could be compact");
  Refuses(Pack(rows, {.rows_always = true}), E::invalid_family, "row UUIDs stored when they are the keys");
  Refuses(Pack(leaf, {.empty_payload_flag_at = 1}), E::invalid_family, "a payload flag with an empty payload");
  {
    // A restart cell (cell 16) that shares bytes.
    const auto& bytes = row_bytes;
    const auto at = Get(bytes, Get(bytes, 504, 4) + 4, 4);  // cell 16
    // cell 16: flags, shared (0), suffix length (16). Make it share 1 byte.
    Check(bytes[at + 1] == 0 && bytes[at + 2] == 16, "cell 16 is a restart cell storing its whole key");
    Refuses(Resealed(bytes, at + 1, 1, 1), E::invalid_family, "a restart cell sharing bytes");
  }
  {
    // A shared count above the previous key's length.
    auto bytes = Pack(leaf);
    std::size_t at = Get(bytes, Get(bytes, 504, 4), 4);
    // Walk to cell 1: flags, shared, suffix len, suffix, row 16, version 16,
    // creator, reference, payload.
    const auto leb = [&](std::size_t& i) { u64 v = 0; unsigned s = 0; byte x; do { x = bytes[i++]; v |= u64(x & 0x7f) << s; s += 7; } while (x & 0x80); return v; };
    const auto flags = bytes[at++];
    leb(at);
    const auto suffix = leb(at);
    at += suffix + 32;
    leb(at);
    at += (flags & 2) ? 16 : 48;
    if (flags & 4) { const auto n = leb(at); at += n; }
    // cell 1 starts here: its shared count byte is at + 1.
    Refuses(Resealed(bytes, at + 1, 127, 1), E::invalid_family, "a shared count above the previous key");
  }
  {
    // A creator of zero (cell 0 of the row leaf: flags, 0, 16, key, version).
    auto bytes = row_bytes;
    const auto at = Get(bytes, Get(bytes, 504, 4), 4);
    Check(bytes[at + 1] == 0 && bytes[at + 2] == 16 && bytes[at + 19 + 16] == 1, "cell 0's creator is 1");
    Refuses(Resealed(bytes, at + 19 + 16, 0, 1), E::invalid_family, "a creator of zero");
  }
}

void Limits(unsigned p) {
  using E = pg::NativeBtreeError;
  const std::size_t quarter = Size(p) / 4;
  {
    auto r = Frame(p, 0x200, 0);
    pg::NativeBtreeCell c;
    c.key = NameKey(std::string(quarter - 39, 'q'), 7);
    Check(c.key.encoded_key.size() == quarter, "a key of exactly a quarter page");
    c.base_page = Here(p, 30);
    c.creator_local_transaction_id = 1;
    r.cells = {c};
    Accepts(r, "a key of a quarter page");
    r.cells[0].payload = {1};
    const auto over = pg::EncodeNativeBtreePage(r);
    Check(over.error == E::key_too_long, "a key plus payload above a quarter page: key_too_long");
    r.cells[0].payload.clear();
    r.cells[0].key.encoded_key.push_back('q');
    Check(pg::EncodeNativeBtreePage(r).error == E::key_too_long, "a key above a quarter page: key_too_long");
    // The same key one byte longer, packed by the oracle: both decoders refuse it.
    Refuses(Pack(r), E::key_too_long, "a stored key above a quarter page");
  }
  {
    // Rebuilt keys above one page on a page that would fit: quarter-page keys
    // differing only in their last byte, five of them.
    auto r = Frame(p, 0x200, 0);
    for (unsigned i = 0; i < 5; ++i) {
      pg::NativeBtreeCell c;
      c.key = NameKey(std::string(quarter - 40, 'q') + static_cast<char>('a' + i), 7);
      c.base_page = Here(p, 30);
      c.creator_local_transaction_id = 1;
      r.cells.push_back(c);
    }
    Check(pg::EncodeNativeBtreePage(r).error == E::resource_exhausted, "rebuilt keys above one page: page full");
    r.cells.pop_back();
    Accepts(r, "rebuilt keys of exactly one page");
  }
  {
    // A full page: the smallest cells (a repeated row key, compact child
    // references) until the page refuses; the fullest page decodes in the
    // allocation-free decoder's required workspace.
    auto r = Frame(p, 0x200, 1);
    r.first_child = Here(p, 20);
    pg::NativeBtreePage last;
    for (u64 i = 0;; ++i) {
      pg::NativeBtreeCell c;
      c.key = RowKey(4242, 100 + i);
      c.child = Here(p, 1000 + i);
      r.cells.push_back(c);
      const auto enc = pg::EncodeNativeBtreePage(r);
      if (!enc.ok()) {
        if (enc.error != E::resource_exhausted) std::cerr << "full page: error " << int(enc.error) << " at " << i << '\n';
        Check(enc.error == E::resource_exhausted, "a page one cell past full: resource_exhausted");
        break;
      }
      last = r;
    }
    Accepts(last, "a page packed full with the smallest cells");
    const auto need = pg::NativeBtreePageViewBackingRequirements(Size(p));
    std::cout << "page_size=" << Size(p) << " smallest-cell page holds " << last.cells.size()
              << " cells (workspace bound " << need.cells << ")\n";
  }
}

void Refusals(unsigned p) {
  using E = pg::NativeBtreeError;
  {
    // A key workspace one byte short of the page's rebuilt keys.
    const auto bytes = Pack(NameLeaf(p));
    Workspace w(bytes.size());
    const auto expanded = Get(bytes, 544, 4);
    auto ws = w.get();
    ws.keys = ws.keys.first(expanded - 1);
    counting = true;
    allocations = 0;
    const auto view = pg::DecodeNativeBtreePageInto(bytes, ws);
    counting = false;
    Check(view.error == E::resource_exhausted && !view.page && allocations == 0,
          "a key workspace too small: resource_exhausted with no heap fallback");
    ws = w.get();
    ws.keys = {reinterpret_cast<byte*>(const_cast<byte*>(bytes.data())), bytes.size()};
    Check(pg::DecodeNativeBtreePageInto(bytes, ws).error == E::invalid_workspace,
          "a key workspace overlapping the image: invalid_workspace");
  }
  auto leaf = RowLeaf(p);
  leaf.cells[3].creator_local_transaction_id = 0;
  Check(pg::EncodeNativeBtreePage(leaf).error == E::invalid_family, "a leaf cell without a creator");
  leaf = RowLeaf(p);
  leaf.cells[3].base_page.reset();
  Check(pg::EncodeNativeBtreePage(leaf).error == E::invalid_family, "a leaf cell without a base page");
  auto branch = Branch(p);
  branch.cells[2].creator_local_transaction_id = 5;
  Check(pg::EncodeNativeBtreePage(branch).error == E::invalid_family, "a branch cell with a creator");
  branch = Branch(p);
  branch.cells[2].payload = {1};
  Check(pg::EncodeNativeBtreePage(branch).error == E::invalid_family, "a branch cell with a payload");
}
}  // namespace

int main() {
  try {
    for (unsigned p = 0; p < d::kCanonicalFilespacePageProfiles.size(); ++p) {
      Accepts(RowLeaf(p), "a root leaf of row keys");
      Accepts(NameLeaf(p), "a leaf of composite keys with payloads");
      Accepts(Branch(p), "a branch");
      Accepts(Frame(p, 0x200, 0), "an empty root leaf");
      Malformed(p);
      Limits(p);
      Refusals(p);
    }
    std::cout << "PASS native B-tree page SBBTP002 checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL native B-tree page SBBTP002: " << e.what() << " checks=" << checks << '\n';
    return 1;
  }
}
