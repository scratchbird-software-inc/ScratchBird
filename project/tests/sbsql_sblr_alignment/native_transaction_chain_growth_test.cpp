// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_TRANSACTION_INVENTORY_WRITER, SB_ALLOCATION_MAP_IN_PLACE
// In-place writes W2c: BEGIN grows the transaction-state chain and the header
// chain from the in-place allocation map, on a database written by the
// production create, one page size per run (--profile N). Every state is
// compared with an independent oracle of the transaction pages (counters,
// every state word and header record, both chains' page lists) and of the map
// (every slot's state and record, decoded here, not through the codec under
// test; the new pages' identities come from the writer's issuer and are
// checked for consistency rather than value), and read back through the
// production fixed-root lease with the open-verification horizon rule.
//   - Growth: BEGIN past the first header page grows the header chain by one
//     system-created page; at 8 KiB, order 1,920 starts both a new state page
//     and a new header page, so that BEGIN grows both chains. BEGIN's own
//     writes stay three; growth is reported separately.
//   - Restart over multi-page chains marks exactly the oracle's in-progress
//     transactions dead.
//   - The fixed-root lease requires each chain's first page to be the
//     create's and every later page to be system-created: a grown page with a
//     transaction creator, or a first page with none, is refused.
//   - With no free page left, BEGIN at a boundary is refused chain_full with
//     no write.
//   - Process loss after every write and every write torn at its half-way
//     point, over a growing BEGIN, its COMMIT and the next BEGIN: the file is
//     exactly the oracle's write prefix; the readers admit it; a page
//     allocated but never linked is skipped and the next BEGIN grows the chain
//     with a fresh page; nothing outside the chains, the map and the pages it
//     allocated changes.
//   - Every read, write and sync failed in turn over the same script: a
//     failed read changes nothing and the call succeeds again (a retry after
//     the growth succeeded allocates nothing more); a failed write or sync
//     leaves exactly the prefix of the writes performed.
// Limit: process death and I/O errors only, not power loss or reordering.
#include "native_database_create.hpp"
#include "native_catalog_relation_lease_reader.hpp"
#include "admitted_datatype_cohort.hpp"
#include "datatype_catalog_manifest.hpp"
#include "native_fixed_root_memory_lease.hpp"
#include "transaction_inventory_writer.hpp"
#include "transaction_inventory_reader.hpp"
#include "transaction_inventory_page.hpp"
#include "transaction_horizon_projection.hpp"
#include "allocation_map_writer.hpp"
#include "careful_write.hpp"
#include "uuid.hpp"
#include "../support/careful_write_crash_sink.hpp"
#include "../support/owned_temp_directory.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
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
namespace fs = std::filesystem;
using namespace scratchbird::core::platform;
using pg::TipStateCode;
using WE = pg::TransactionWriterError;

unsigned checks = 0;
void Check(bool ok, std::string_view why, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) throw std::runtime_error(std::string(why) + " (line " + std::to_string(at.line()) + ")");
}
constexpr int kPlannedCrash = 86, kChildCompleted = 70, kChildFailed = 71, kChildException = 73;
constexpr u32 kNoCrash = ~u32{0};

Uuid Fixed(unsigned n) {
  Uuid id;
  id.bytes[0] = 1;
  id.bytes[6] = 0x70;
  id.bytes[8] = 0x80;
  id.bytes[14] = static_cast<byte>(n >> 8);
  id.bytes[15] = static_cast<byte>(n);
  return id;
}
u64 kT = 0;  // ms, one second after the creator's commit
Uuid V7(u64 ms, unsigned salt) {
  Uuid id;
  for (int i = 0; i < 6; ++i) id.bytes[i] = static_cast<byte>(ms >> (8 * (5 - i)));
  id.bytes[6] = 0x70;
  id.bytes[8] = 0x80;
  id.bytes[9] = 0x7d;
  id.bytes[13] = static_cast<byte>(salt >> 16);
  id.bytes[14] = static_cast<byte>(salt >> 8);
  id.bytes[15] = static_cast<byte>(salt);
  return id;
}
u64 Ns(u64 ms) { return ms * 1'000'000; }
u64 BeginNs(const Uuid& id) {
  u64 ms = 0;
  for (int i = 0; i < 6; ++i) ms = ms << 8 | id.bytes[i];
  return Ns(ms);
}
u64 Load64(const byte* p) {
  u64 v = 0;
  for (int i = 7; i >= 0; --i) v = v << 8 | p[i];
  return v;
}
u32 Load32(const byte* p) {
  u32 v = 0;
  for (int i = 3; i >= 0; --i) v = v << 8 | p[i];
  return v;
}
constexpr const char* kFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";

std::vector<byte> ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  Check(bool(in), "open the file to read it back");
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void WriteAll(const std::string& path, std::span<const byte> bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  Check(bool(out.flush()), "write the file");
}
void WriteAt(const std::string& path, u64 offset, std::span<const byte> bytes) {
  std::fstream out(path, std::ios::binary | std::ios::in | std::ios::out);
  out.seekp(static_cast<std::streamoff>(offset));
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  Check(bool(out.flush()), "write bytes into the file");
}

// ---- The oracle's state ---------------------------------------------------------
struct Slot {
  byte state = 0;
  std::array<byte, 128> record{};
  bool operator==(const Slot&) const = default;
};
struct State {
  pg::TipCounters counters;
  std::map<u64, pg::TipStateWord> words;
  std::map<u64, pg::TransactionHeaderRecord> headers;
  std::vector<u64> tip_pages, header_pages;
  std::map<u64, Slot> slots;    // allocation map, by page number
  std::vector<u64> map_pages;
  std::vector<std::pair<u64, u32>> map_ranges;  // each map page's (first page, pages covered)
};
// Records of pages allocated after the create carry identities the writer's
// issuer chose: compared without their allocation and page ids (bytes 8..39),
// which are checked separately against the allocated page.
bool SameSlots(const State& a, const State& b, const std::set<u64>& issued) {
  if (a.slots.size() != b.slots.size()) return false;
  for (const auto& [n, s] : a.slots) {
    const auto it = b.slots.find(n);
    if (it == b.slots.end() || it->second.state != s.state) return false;
    auto x = s.record, y = it->second.record;
    if (issued.count(n)) {
      std::fill(x.begin() + 8, x.begin() + 40, byte{0});
      std::fill(y.begin() + 8, y.begin() + 40, byte{0});
    }
    if (x != y) return false;
  }
  return true;
}
bool Same(const State& a, const State& b, const std::set<u64>& issued) {
  return a.counters == b.counters && a.words == b.words && a.headers == b.headers && a.tip_pages == b.tip_pages &&
         a.header_pages == b.header_pages && a.map_pages == b.map_pages && SameSlots(a, b, issued);
}

struct Base {
  scratchbird::tests::OwnedTempDirectory dir;
  std::string path;
  u32 size = 0;
  Uuid database, primary, profile;
  d::FilespaceRootReference state_root, map_root;
  std::vector<byte> bytes;
  explicit Base(unsigned index) {
    const auto& p = d::kCanonicalFilespacePageProfiles[index];
    size = p.page_size_bytes;
    profile = p.uuid;
    db::NativeDatabaseCreateRequest r;
    r.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
    r.database_name = "orders";
    r.database_default_language = scratchbird::core::catalog::kCatalogLanguage_en;/* S2: the language constant, not the tag "en" */
    r.page_size_profile_uuid = p.uuid;
    r.page_size_bytes = p.page_size_bytes;
    r.total_pages = p.page_size_bytes == 8192 ? 1024 : 512;
    r.creation_utc_millis = 1789357072000ULL;
    r.engine_uuid = Fixed(4);
    r.datatype_cohort = {{UuidKind::object, scratchbird::core::datatypes::kDatatypeCohortV11}, 11, 11};
    r.bootstrap_principal_name = "BootstrapAdmin";
    r.bootstrap_credential_fingerprint = kFingerprint;
    r.service_principal_name = "NodeService";
    r.service_credential_fingerprint = kFingerprint;
    r.maximum_retained_image_bytes = db::NativeHostedSourceBackingBytes(p.page_size_bytes);
    path = (dir.path() / "created").string();
    {
      d::FileDevice device;
      Check(device.Open(path, d::FileOpenMode::create_new,d::DatabaseFileOwnership::native_data_file).ok(), "node device");
      u::StandaloneUuidV7Issuer issuer({Fixed(1), Fixed(10)}, {{}, 0, 1000});
      auto made = db::CreateNativeDatabaseOnOpenDevice(device, r, issuer);
      Check(made.ok(), "the production create writes a complete native database");
      database = made.receipt->database_uuid;
      primary = made.receipt->filespace_uuid;
      Check(device.Close().ok(), "close");
    }
    {
      d::FileDevice device;
      Check(device.Open(path, d::FileOpenMode::open_existing_read_only).ok(), "read-only open");
      const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device);
      Check(zero.ok(), "page zero");
      for (const auto& root : zero.record->roots) {
        if (root.kind == 4) state_root = root;
        if (root.kind == 3) map_root = root;
      }
      Check(state_root.kind == 4 && map_root.kind == 3, "page zero names both chains' roots");
      Check(device.Close().ok(), "close");
    }
    bytes = ReadAll(path);
    const auto headers = pg::DecodeTransactionHeaderPage(std::span<const byte>(bytes).subspan(
        pg::DecodeTransactionStatePage(std::span<const byte>(bytes).subspan(state_root.page_number * size, size))
                .view->header_chain_page *
            size,
        size));
    kT = pg::TransactionHeaderAt(*headers.view, 1)->commit_time_utc_ns / 1'000'000 + 1000;
  }
  pg::TransactionWriterBinding Binding(u::StandaloneUuidV7Issuer* issuer) const {
    pg::TransactionWriterBinding b;
    b.database_uuid = database;
    b.state_root = state_root;
    if (issuer) {
      b.allocation_root = map_root;
      b.issuer = issuer;
    }
    return b;
  }
  std::string Copy(std::span<const byte> from, const std::string& name) const {
    const auto p = (dir.path() / name).string();
    WriteAll(p, from);
    return p;
  }
  // The file through the page codecs (transaction pages) and the
  // independent map decoder.
  State Read(std::span<const byte> file) const {
    State s;
    for (u64 page = state_root.page_number; page;) {
      const auto v = pg::DecodeTransactionStatePage(file.subspan(page * size, size));
      Check(v.ok(), "a state page decodes");
      if (v.view->counters) s.counters = *v.view->counters;
      for (u32 slot = 0; slot < v.view->slots; ++slot) {
        const auto w = pg::TipWordAt(*v.view, slot);
        if (w && w->state != TipStateCode::unused) s.words[v.view->first_number + slot] = *w;
      }
      s.tip_pages.push_back(page);
      page = v.view->next_tip_page;
      Check(s.tip_pages.size() < 10000, "the state chain ends");
    }
    const auto first = pg::DecodeTransactionStatePage(file.subspan(state_root.page_number * size, size));
    for (u64 page = first.view->header_chain_page; page;) {
      const auto v = pg::DecodeTransactionHeaderPage(file.subspan(page * size, size));
      Check(v.ok(), "a header page decodes");
      for (u32 slot = 0; slot < v.view->slots; ++slot)
        if (const auto r = pg::TransactionHeaderAt(*v.view, slot)) s.headers[v.view->first_number + slot] = *r;
      s.header_pages.push_back(page);
      page = v.view->next_page;
      Check(s.header_pages.size() < 100000, "the header chain ends");
    }
    for (u64 page = map_root.page_number; page;) {
      const auto* p = file.data() + page * size;
      s.map_pages.push_back(page);
      const u32 n = Load32(p + 128 + 12);
      const u64 first_page = Load64(p + 128 + 48);
      const u64 records_at = (512 + (n + 1) / 2 + 127) / 128 * 128;
      s.map_ranges.emplace_back(first_page, n);
      for (u32 i = 0; i < n; ++i) {
        Slot slot;
        slot.state = static_cast<byte>((p[512 + i / 2] >> (4 * (i % 2))) & 15);
        std::copy_n(p + records_at + 128 * i, 128, slot.record.begin());
        s.slots[first_page + i] = slot;
      }
      page = Load64(p + 256);
    }
    return s;
  }
  // Every byte outside the chains, the map and the pages allocated since the
  // create is the created file's.
  void Untouched(std::span<const byte> file, const State& now, std::string_view label) const {
    std::set<u64> excluded(now.tip_pages.begin(), now.tip_pages.end());
    excluded.insert(now.header_pages.begin(), now.header_pages.end());
    excluded.insert(now.map_pages.begin(), now.map_pages.end());
    const auto created = Read(bytes);
    for (const auto& [n, s] : now.slots)
      if (s.state != created.slots.at(n).state) excluded.insert(n);
    Check(file.size() == bytes.size(), std::string(label) + ": the file keeps its size");
    for (u64 page = 0; page < file.size() / size; ++page) {
      if (excluded.count(page)) continue;
      if (!std::equal(file.begin() + page * size, file.begin() + (page + 1) * size, bytes.begin() + page * size))
        Check(false, std::string(label) + ": page " + std::to_string(page) + " outside the written pages changed");
    }
  }
};

// The pages allocated since `base` and their consistency: each allocated page
// a chain links is a formatted chain page whose header carries the record's
// page id; the records' issued ids are UUIDv7s and distinct.
std::set<u64> Issued(const State& now, const State& base, std::span<const byte> file, u32 size) {
  std::set<u64> issued;
  std::set<std::array<byte, 32>> ids;
  for (const auto& [n, s] : now.slots) {
    const auto& before = base.slots.at(n);
    if (s.record == before.record) continue;
    issued.insert(n);
    if (s.state != 2) continue;
    std::array<byte, 32> both{};
    std::copy_n(s.record.begin() + 8, 32, both.begin());
    Check(ids.insert(both).second, "issued allocation and page ids are distinct");
    Uuid page_id;
    std::copy_n(s.record.begin() + 24, 16, page_id.bytes.begin());
    Check(u::IsEngineIdentityUuid(page_id), "an issued page id is a UUIDv7");
    const bool linked = std::find(now.tip_pages.begin(), now.tip_pages.end(), n) != now.tip_pages.end() ||
                        std::find(now.header_pages.begin(), now.header_pages.end(), n) != now.header_pages.end();
    if (linked) {
      const auto header = d::DecodeNativeCommonPageHeader(file.data() + n * size, 128);
      Check(header.ok() && header.header->page_uuid == page_id && header.header->page_number == n &&
                header.header->page_generation == 1 && header.header->page_type == pg::kTransactionStatePageType,
            "a linked grown page carries its record's identity");
    }
  }
  return issued;
}


// ---- The oracle's steps ----------------------------------------------------------
// Layout written out (independent of the codecs): state slots per page
// (P - 512) / 4, header slots (P - 512) / 256; a map slot's state is at
// 512 + i / 2 and its record at roundup128(512 + ceil(N / 2)) + 128 i.
u64 TipSlots(u32 size) { return (size - 512) / 4; }
u64 HeaderSlots(u32 size) { return (size - 512) / 256; }
pg::TipHorizons BeginHorizons(const State& m, u64 n) {
  u64 oit = n, oat = n;
  for (const auto& [k, w] : m.words) {
    if (k >= n || !m.headers.count(k)) continue;
    if (w.state == TipStateCode::active || w.state == TipStateCode::read_only_active) {
      oit = std::min(oit, k);
      oat = std::min(oat, k);
    }
    if (w.state == TipStateCode::dead) oit = std::min(oit, k);
  }
  pg::TipHorizons h;
  h.oldest_interesting = oit;
  h.oldest_active = oat;
  h.oldest_snapshot = oat;
  h.oldest_archive_required = n + 1;
  h.oldest_limbo_resolution = n + 1;
  h.oldest_cleanup = std::min<u64>(oat, n + 1);
  return h;
}
u64 RestartOracle(State& m) {
  const u64 from = std::max<u64>(1, std::min(m.counters.horizons.oldest_interesting, m.counters.horizons.oldest_active));
  u64 marked = 0;
  for (u64 k = from; k < m.counters.next_transaction; ++k) {
    const auto it = m.words.find(k);
    const auto state = it == m.words.end() ? TipStateCode::unused : it->second.state;
    if (state != TipStateCode::unused && state != TipStateCode::starting && state != TipStateCode::active &&
        state != TipStateCode::read_only_active && state != TipStateCode::rolling_back)
      continue;
    if (it == m.words.end()) m.words[k] = pg::TipStateWord{TipStateCode::dead};
    else it->second.state = TipStateCode::dead;
    ++marked;
  }
  return marked;
}

struct Op {
  char kind = 'B';
  u64 order = 0;
};
Op Begin(u64 n) { return {'B', n}; }
Op Commit(u64 n) { return {'C', n}; }
Uuid TransactionId(u64 n) { return V7(kT + n, static_cast<unsigned>(n)); }
u64 CommitTime(u64 n) { return Ns(kT + 5'000'000 + n); }

using Step = std::function<void(State&)>;
struct Planned {
  std::vector<Step> steps;
  std::vector<u32> grows;          // per op: chains grown
  std::vector<u32> growth_writes;  // per op
};
// One chain's growth by the lowest free map page: the record (and the state,
// when they share a unit), the state, the format (the page is unreachable, so
// the model does not change), the link.
u32 AddGrowth(Planned& out, State& model, bool state_chain, const Uuid& owner) {
  u64 page = 0;
  for (const auto& [n, s] : model.slots)
    if (s.state == 0) {
      page = n;
      break;
    }
  Check(page != 0, "the oracle finds a free page");
  std::array<byte, 128> record{};
  for (int i = 0; i < 8; ++i) record[i] = static_cast<byte>(page >> (8 * i));
  std::copy(owner.bytes.begin(), owner.bytes.end(), record.begin() + 40);
  record[80] = 1;  // generation 1
  for (int i = 0; i < 4; ++i) record[96 + i] = static_cast<byte>(pg::kTransactionStatePageType >> (8 * i));
  // Owner Q76 = 1 (2026-11-13): the record, then the state, two writes even
  // when they share a unit (was one combined step when the record shared the state's unit).
  out.steps.push_back([page, record](State& m) { m.slots[page].record = record; });
  out.steps.push_back([page](State& m) { m.slots[page].state = 2; });
  out.steps.push_back([](State&) {});
  out.steps.push_back([page, state_chain](State& m) { (state_chain ? m.tip_pages : m.header_pages).push_back(page); });
  model.slots[page].record = record;
  model.slots[page].state = 2;
  (state_chain ? model.tip_pages : model.header_pages).push_back(page);
  return 4u;  // Q76: was 3 when the record shared the state's unit
}
void AddBegin(Planned& out, State& model, u64 n, const Uuid& database) {
  out.steps.push_back([n](State& m) {
    const auto h = BeginHorizons(m, n);
    m.counters.next_transaction = n + 1;
    m.counters.horizons = h;
  });
  pg::TransactionHeaderRecord r;
  r.transaction_uuid = TransactionId(n);
  r.transaction_order = n;
  r.database_uuid = database;
  out.steps.push_back([n, r](State& m) { m.headers[n] = r; });
  out.steps.push_back([n](State& m) { m.words[n] = pg::TipStateWord{TipStateCode::active, pg::kTipFlagHasDetailRecord}; });
  for (auto i = out.steps.size() - 3; i < out.steps.size(); ++i) out.steps[i](model);
}
void AddCommit(Planned& out, State& model, u64 n) {
  out.steps.push_back([n](State& m) {
    auto& r = m.headers.at(n);
    u64 latest = 0;
    for (const auto& [k, h] : m.headers) latest = std::max(latest, h.commit_time_utc_ns);
    const u64 at = std::max({CommitTime(n), BeginNs(r.transaction_uuid), latest + 1});
    r.final_state = TipStateCode::committed;
    r.commit_time_utc_ns = at;
    r.completed_at_utc_ns = at;
  });
  out.steps.push_back([n](State& m) { m.words.at(n).state = TipStateCode::committed; });
  for (auto i = out.steps.size() - 2; i < out.steps.size(); ++i) out.steps[i](model);
}
// The oracle's steps for ops from `model` (which it advances).
Planned Plan(State& model, std::span<const Op> ops, const Base& b) {
  Planned out;
  for (const auto& op : ops) {
    u32 grows = 0, growth = 0;
    if (op.kind == 'B') {
      const u64 n = op.order;
      // The writer grows the state chain first, then the header chain.
      if (n / TipSlots(b.size) >= model.tip_pages.size()) {
        growth += AddGrowth(out, model, true, b.state_root.object_uuid);
        ++grows;
      }
      if (n / HeaderSlots(b.size) >= model.header_pages.size()) {
        growth += AddGrowth(out, model, false, b.state_root.object_uuid);
        ++grows;
      }
      AddBegin(out, model, n, b.database);
    } else {
      AddCommit(out, model, op.order);
    }
    out.grows.push_back(grows);
    out.growth_writes.push_back(growth);
  }
  return out;
}
State Prefix(State m, const std::vector<Step>& steps, std::size_t k) {
  for (std::size_t i = 0; i < k; ++i) steps[i](m);
  return m;
}

// ---- Running the writer ----------------------------------------------------------
struct Done {
  WE error = WE::invalid_request;
  std::optional<u64> order;
  u32 writes = 0, syncs = 0, grown = 0, growth_writes = 0;
  pg::AllocationWriteError allocation = pg::AllocationWriteError::none;
  bool ok() const { return error == WE::none; }
};
Done Do(pg::TransactionInventoryWriter& w, d::CarefulWriteSink& sink, const Op& op) {
  if (op.kind == 'B') {
    pg::TransactionHeaderRecord h;
    h.transaction_uuid = TransactionId(op.order);
    const auto r = w.Begin(sink, h);
    return {r.error, r.transaction_order, r.writes, r.syncs, r.grown_state_pages + r.grown_header_pages,
            r.growth_writes, r.allocation_error};
  }
  const auto r = w.Commit(sink, op.order, CommitTime(op.order));
  return {r.error, r.transaction_order, r.writes, r.syncs, 0, 0, pg::AllocationWriteError::none};
}
std::vector<Op> Range(u64 from, u64 to_exclusive) {
  std::vector<Op> ops;
  for (u64 n = from; n < to_exclusive; ++n) {
    ops.push_back(Begin(n));
    ops.push_back(Commit(n));
  }
  return ops;
}

// The production lease's view, with the open-verification horizon rule and
// each inventory page's chain sequence.
struct Seen {
  bool ok = false, admissible = false;
  std::vector<u64> sequences;
  std::size_t entries = 0;
};
Seen LeaseRead(const Base& b, const std::string& path) {
  d::FileDevice device;
  Check(device.Open(path, d::FileOpenMode::open_existing_read_only).ok(), "independent read-only open");
  Seen out;
  {
    const std::vector<d::NativeFilespaceDevice> files{{b.primary, b.profile, &device}};
    static std::vector<byte> backing(128u << 20);
    auto fixed = db::AcquireNativeFixedRootBackedLease(b.database, files, b.primary, {4096ull * b.size, backing.size()},
                                                       backing);
    out.ok = fixed.ok();
    if (out.ok) {
      const auto& lease = *fixed.lease;
      const auto& inventory = lease.inventory();
      out.entries = inventory.entries.size();
      out.sequences.assign(lease.inventory_page_sequences().begin(), lease.inventory_page_sequences().end());
      const pg::NativeTransactionInventoryConstView view{inventory.next_local_transaction_id,
                                                        inventory.next_commit_sequence, inventory.entries};
      mga::LocalTransactionHorizons projected;
      out.admissible = !mga::detail::ProjectValidatedLocalHorizons(view, {}, projected) &&
                       pg::StoredTransactionHorizonsAdmissible(lease.counters(), inventory.next_local_transaction_id,
                                                               projected);
      fixed.lease.Reset();
    } else {
      std::cerr << "lease error=" << int(fixed.error) << " root_kind=" << (fixed.root_kind ? int(*fixed.root_kind) : -1)
                << '\n';
    }
  }
  Check(device.Close().ok(), "close the reader");
  return out;
}
// The file is the oracle's state; issued identities consistent; the lease
// admits it, with sequences 0.. for each chain.
void ReadBack(const Base& b, const std::string& path, const State& expected, const State& created,
              std::string_view label) {
  const auto file = ReadAll(path);
  const auto seen = b.Read(file);
  const auto issued = Issued(seen, created, file, b.size);
  Check(Same(seen, expected, issued), std::string(label) + ": the file is the oracle's state");
  b.Untouched(file, seen, label);
  const auto lease = LeaseRead(b, path);
  Check(lease.ok && lease.admissible, std::string(label) + ": the lease admits the file and its horizons");
  std::vector<u64> sequences;
  for (u64 i = 0; i < seen.tip_pages.size(); ++i) sequences.push_back(i);
  for (u64 i = 0; i < seen.header_pages.size(); ++i) sequences.push_back(i);
  Check(lease.sequences == sequences, std::string(label) + ": the lease reads both chains whole");
}

// A copy of `from` advanced by ops through the writer (with growth), checked
// op by op against the oracle; returns the file and the oracle's state.
std::pair<std::vector<byte>, State> Advance(const Base& b, std::span<const byte> from, State model,
                                            std::span<const Op> ops, bool restart_first, std::string_view label) {
  const auto path = b.Copy(from, "advance.db");
  const auto planned = Plan(model, ops, b);
  d::FileDevice device;
  Check(device.Open(path, d::FileOpenMode::open_existing).ok(), "open");
  {
    d::FileDeviceCarefulWriteSink sink(device);
    u::StandaloneUuidV7Issuer issuer({b.database, Fixed(10)}, {{}, 0, 1000});
    pg::TransactionInventoryWriter w(b.Binding(&issuer));
    const auto restarted = w.Restart(sink);
    Check(restarted.ok() && (restart_first || restarted.marked_dead == 0), std::string(label) + ": restart");
    const bool forced = device.forced_ordered_writes();
    for (std::size_t i = 0; i < ops.size(); ++i) {
      const auto r = Do(w, sink, ops[i]);
      Check(r.ok() && r.order == ops[i].order, std::string(label) + ": op " + std::to_string(i));
      if (ops[i].kind != 'B') continue;
      Check(r.writes == 3 && r.syncs == (forced ? 1u : 3u),
            std::string(label) + ": BEGIN's own writes stay three, growth or not");
      Check(r.grown == planned.grows[i] && r.growth_writes == planned.growth_writes[i],
            std::string(label) + ": growth exactly where the oracle grows, with its writes");
    }
  }
  Check(device.Close().ok(), "close");
  auto file = ReadAll(path);
  fs::remove(path);
  return {std::move(file), model};
}

// ---- Growth, restart, forgery, exhaustion --------------------------------------
void Growth(const Base& b, std::vector<byte>& boundary_file, State& boundary_state, std::vector<byte>& both_file,
            State& both_state) {
  const State created = b.Read(b.bytes);
  const u64 header_slots = HeaderSlots(b.size), tip_slots = TipSlots(b.size);
  // To the first header boundary (Next = header_slots), then through it.
  auto [to_boundary, at_boundary] = Advance(b, b.bytes, created, Range(2, header_slots), false, "fill");
  boundary_file = to_boundary;
  boundary_state = at_boundary;
  {
    const auto ops = Range(header_slots, header_slots + 2);
    auto [grown, after] = Advance(b, to_boundary, at_boundary, ops, false, "header growth");
    const auto path = b.Copy(grown, "grown.db");
    ReadBack(b, path, after, created, "header growth");
    Check(after.header_pages.size() == 2 && after.tip_pages.size() == 1, "one header page grown");
    // Forgery: the grown page with a transaction creator; the first header
    // page with none. The lease refuses both.
    const auto record_of = [&](u64 page) -> u64 {
      for (std::size_t i = 0; i < after.map_ranges.size(); ++i) {
        const auto [first, n] = after.map_ranges[i];
        if (page >= first && page < first + n)
          return after.map_pages[i] * b.size + (512 + (n + 1) / 2 + 127) / 128 * 128 + 128 * (page - first);
      }
      return 0;
    };
    const auto forge = [&](u64 page, bool give_creator, std::string_view what) {
      const auto at = record_of(page);
      Check(at != 0, "the page's record");
      const auto original = std::vector<byte>(grown.begin() + at, grown.begin() + at + 128);
      auto forged = original;
      if (give_creator) {
        const auto creator = TransactionId(2);
        std::copy(creator.bytes.begin(), creator.bytes.end(), forged.begin() + 56);
        forged[72] = 2;
      } else {
        std::fill(forged.begin() + 56, forged.begin() + 80, byte{0});
      }
      WriteAt(path, at, forged);
      Check(!LeaseRead(b, path).ok, what);
      WriteAt(path, at, original);
      Check(LeaseRead(b, path).ok, "restored");
    };
    forge(after.header_pages[1], true, "a grown chain page with a transaction creator is refused");
    forge(after.header_pages[0], false, "a chain's first page with no creator is refused");
    fs::remove(path);
  }
  if (b.size != 8192) return;
  // At 8 KiB, through the first state-page boundary: order 1920 starts a new
  // state page and a new header page, so its BEGIN grows both chains.
  Check(tip_slots == 1920 && 1920 % header_slots == 0, "the boundary orders coincide");
  auto [to_both, at_both] = Advance(b, to_boundary, at_boundary, Range(header_slots, 1920), false, "fill to 1920");
  both_file = to_both;
  both_state = at_both;
  {
    std::vector<Op> ops{Begin(1920), Begin(1921), Commit(1921)};
    State model = at_both;
    const auto planned = Plan(model, ops, b);
    Check(planned.grows[0] == 2, "the BEGIN of 1920 grows both chains");
    const auto t0 = std::chrono::steady_clock::now();
    auto [grown, after] = Advance(b, to_both, at_both, ops, false, "both chains");
    const auto per_op = (std::chrono::steady_clock::now() - t0) / 3;
    const auto path = b.Copy(grown, "both.db");
    ReadBack(b, path, after, created, "both chains");
    const auto t1 = std::chrono::steady_clock::now();
    (void)LeaseRead(b, path);
    const auto lease_time = std::chrono::steady_clock::now() - t1;
    std::cout << "open cost page_size=8192 transactions=" << after.counters.next_transaction - 1
              << " header_pages=" << after.header_pages.size()
              << " lease_read_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(lease_time).count()
              << " op_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(per_op).count()
              << " (this machine; not a performance claim)\n";
    // Restart over multi-page chains: 1920 is still in progress.
    State restarted = after;
    const u64 marked = RestartOracle(restarted);
    Check(marked == 1, "the oracle marks 1920 dead");
    d::FileDevice device;
    Check(device.Open(path, d::FileOpenMode::open_existing).ok(), "open");
    {
      d::FileDeviceCarefulWriteSink sink(device);
      pg::TransactionInventoryWriter w(b.Binding(nullptr));
      const auto r = w.Restart(sink);
      Check(r.ok() && r.marked_dead == marked, "restart over a two-page state chain marks the in-progress transaction");
    }
    Check(device.Close().ok(), "close");
    ReadBack(b, path, restarted, created, "restart over multi-page chains");
    fs::remove(path);
  }
}

void Exhaustion(const Base& b, std::span<const byte> boundary_file) {
  // Every free page allocated, then the BEGIN that needs a new header page.
  const auto path = b.Copy(boundary_file, "full.db");
  d::FileDevice device;
  Check(device.Open(path, d::FileOpenMode::open_existing).ok(), "open");
  {
    d::FileDeviceCarefulWriteSink sink(device);
    for (unsigned i = 0;; ++i) {
      const pg::PageAllocationRequest request{6, Fixed(3000), V7(kT, 100000 + i * 2), V7(kT, 100001 + i * 2), {}, 0};
      const auto r = pg::AllocatePage(sink, {b.database, b.map_root}, request);
      if (r.error == pg::AllocationWriteError::filespace_full) break;
      Check(r.ok(), "allocate every free page");
    }
    const auto before = ReadAll(path);
    u::StandaloneUuidV7Issuer issuer({b.database, Fixed(10)}, {{}, 0, 1000});
    pg::TransactionInventoryWriter w(b.Binding(&issuer));
    Check(w.Restart(sink).ok(), "restart");
    const auto r = Do(w, sink, Begin(HeaderSlots(b.size)));
    Check(r.error == WE::chain_full && r.allocation == pg::AllocationWriteError::filespace_full && r.writes == 0 &&
              r.growth_writes == 0 && !w.failed() && ReadAll(path) == before,
          "with no free page the BEGIN that needs one is refused chain_full with no write");
  }
  Check(device.Close().ok(), "close");
  fs::remove(path);
}

// ---- Process loss, torn writes, I/O failures -------------------------------------
[[noreturn]] void Child(const Base& b, const std::string& path, std::span<const Op> ops, u32 limit, bool torn) {
  try {
    d::FileDevice device;
    if (!device.Open(path, d::FileOpenMode::open_existing).ok()) _exit(kChildFailed);
    int code = kChildCompleted;
    {
      d::FileDeviceCarefulWriteSink file(device);
      std::optional<scratchbird::tests::CrashAfterWritesSink> crash;
      std::optional<scratchbird::tests::TornWriteSink> tear;
      d::CarefulWriteSink* sink = torn ? static_cast<d::CarefulWriteSink*>(&tear.emplace(file, limit, kPlannedCrash))
                                       : static_cast<d::CarefulWriteSink*>(&crash.emplace(file, limit, kPlannedCrash));
      u::StandaloneUuidV7Issuer issuer({b.database, Fixed(10)}, {{}, 0, 1000});
      pg::TransactionInventoryWriter w(b.Binding(&issuer));
      if (!w.Restart(*sink).ok()) code = kChildFailed;
      for (const auto& op : ops)
        if (code == kChildCompleted && !Do(w, *sink, op).ok()) code = kChildFailed;
    }
    if (!device.Close().ok()) code = kChildFailed;
    _exit(code);
  } catch (...) {
    _exit(kChildException);
  }
}

// After a loss: a new process restarts and runs BEGIN + COMMIT of the next
// number, growing again if the lost growth never linked its page.
void Recover(const Base& b, const std::string& path, State expected, const State& created, std::string_view label) {
  RestartOracle(expected);
  const u64 next = expected.counters.next_transaction;
  const std::vector<Op> ops{Begin(next), Commit(next)};
  State after = expected;
  (void)Plan(after, ops, b);
  d::FileDevice device;
  Check(device.Open(path, d::FileOpenMode::open_existing).ok(), std::string(label) + ": reopen");
  {
    d::FileDeviceCarefulWriteSink sink(device);
    u::StandaloneUuidV7Issuer issuer({b.database, Fixed(10)}, {{}, 0, 1000});
    pg::TransactionInventoryWriter w(b.Binding(&issuer));
    Check(w.Restart(sink).ok(), std::string(label) + ": restart");
    for (const auto& op : ops) Check(Do(w, sink, op).ok(), std::string(label) + ": the next transaction");
  }
  Check(device.Close().ok(), std::string(label) + ": close");
  ReadBack(b, path, after, created, std::string(label) + " recovered");
}

void Losses(const Base& b, std::span<const byte> from, const State& start, std::string_view where) {
  const State created = b.Read(b.bytes);
  const u64 n = start.counters.next_transaction;
  const std::vector<Op> ops{Begin(n), Commit(n), Begin(n + 1)};
  State model = start;
  const auto planned = Plan(model, ops, b);
  const u32 total = static_cast<u32>(planned.steps.size());
  unsigned cases = 0, old_units = 0, new_units = 0;
  for (const bool torn : {false, true}) {
    for (u32 k = torn ? 1 : 0; k <= total + (torn ? 0 : 1); ++k) {
      const bool full = !torn && k == total + 1;
      const std::string label = std::string(where) + (torn ? " torn write " : " crash after ") + std::to_string(k);
      const auto path = b.Copy(from, "loss.db");
      std::cout.flush();
      std::cerr.flush();
      const pid_t child = fork();
      Check(child >= 0, label + ": fork");
      if (child == 0) Child(b, path, ops, full ? kNoCrash : k, torn);
      int status = 0;
      Check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                WEXITSTATUS(status) == (full ? kChildCompleted : kPlannedCrash),
            label + ": the child stops where planned");
      const auto file = ReadAll(path);
      const auto seen = b.Read(file);
      const auto issued = Issued(seen, created, file, b.size);
      State expected = Prefix(start, planned.steps, full ? total : torn ? k - 1 : k);
      if (torn && !Same(seen, expected, issued)) {
        expected = Prefix(start, planned.steps, k);
        ++new_units;
      } else if (torn) {
        ++old_units;
      }
      ReadBack(b, path, expected, created, label);
      Recover(b, path, expected, created, label);
      fs::remove(path);
      ++cases;
    }
  }
  Check(old_units > 0 && new_units > 0, "torn writes left both old and new units");
  std::cout << "losses " << where << " page_size=" << b.size << " writes=" << total << " cases=" << cases << '\n';
}

class FaultSink final : public d::CarefulWriteSink {
 public:
  FaultSink(d::CarefulWriteSink& inner, u32 fail_at) : inner_(inner), fail_at_(fail_at) {}
  d::BoundedIoResult ReadAt(u64 offset, std::span<byte> bytes) override {
    if (++calls_ == fail_at_) return Fail(d::BoundedIoOperation::read, offset, bytes.size());
    return inner_.ReadAt(offset, bytes);
  }
  d::BoundedIoResult WriteAt(u64 offset, std::span<const byte> bytes) override {
    if (++calls_ == fail_at_) return Fail(d::BoundedIoOperation::write, offset, bytes.size());
    const auto r = inner_.WriteAt(offset, bytes);
    if (r.ok()) ++writes_;
    return r;
  }
  d::BoundedIoResult Sync() override {
    if (++calls_ == fail_at_) return Fail(d::BoundedIoOperation::sync, 0, 0);
    return inner_.Sync();
  }
  bool forced_ordered() const noexcept override { return inner_.forced_ordered(); }
  u32 writes() const { return writes_; }
  bool reached() const { return calls_ >= fail_at_; }

 private:
  static d::BoundedIoResult Fail(d::BoundedIoOperation kind, u64 offset, usize bytes) {
    d::BoundedIoResult r;
    r.operation = kind;
    r.error = d::BoundedIoError::native_failure;
    r.offset = offset;
    r.requested_bytes = bytes;
    r.native_error = EIO;
    r.native_attempted = true;
    return r;
  }
  d::CarefulWriteSink& inner_;
  u32 fail_at_;
  u32 calls_ = 0, writes_ = 0;
};

void Faults(const Base& b, std::span<const byte> from, const State& start, std::string_view where) {
  const State created = b.Read(b.bytes);
  const u64 n = start.counters.next_transaction;
  const std::vector<Op> ops{Begin(n), Commit(n), Begin(n + 1)};
  State model = start;
  const auto planned = Plan(model, ops, b);
  unsigned points = 0, retried = 0, failed = 0;
  for (u32 at = 1;; ++at) {
    const std::string label = std::string(where) + " fault at call " + std::to_string(at);
    const auto path = b.Copy(from, "faults.db");
    d::FileDevice device;
    Check(device.Open(path, d::FileOpenMode::open_existing).ok(), label + ": open");
    bool reached = false;
    u32 performed = 0;
    {
      d::FileDeviceCarefulWriteSink file(device);
      FaultSink sink(file, at);
      u::StandaloneUuidV7Issuer issuer({b.database, Fixed(10)}, {{}, 0, 1000});
      pg::TransactionInventoryWriter w(b.Binding(&issuer));
      auto restart = w.Restart(sink);
      if (!restart.ok()) {
        Check(restart.error == WE::io_failure && !w.failed(), label + ": a failed restart read changes nothing");
        restart = w.Restart(sink);
        Check(restart.ok(), label + ": restart again");
      }
      for (const auto& op : ops) {
        auto r = Do(w, sink, op);
        if (r.ok()) continue;
        if (!w.failed()) {
          Check(r.error == WE::io_failure, label + ": a failed read is reported as one");
          r = Do(w, sink, op);
          Check(r.ok(), label + ": and the same call succeeds again");
          ++retried;
          continue;
        }
        Check(r.error == WE::write_failed, label + ": a failed write or sync fails the writer");
        ++failed;
        break;
      }
      reached = sink.reached();
      performed = sink.writes();
    }
    Check(device.Close().ok(), label + ": close");
    // Exactly the prefix of the writes performed (a retried growth or BEGIN
    // repeats nothing that had reached the file).
    ReadBack(b, path, Prefix(start, planned.steps, performed), created, label);
    fs::remove(path);
    if (!reached) break;
    ++points;
  }
  Check(points > 10 && retried > 0 && failed > 0, "reads, writes and syncs were each failed");
  std::cout << "faults " << where << " page_size=" << b.size << " points=" << points << " retried=" << retried
            << " failed=" << failed << '\n';
}
}  // namespace

int main(int argc, char** argv) {
  unsigned profile = 0;
  try {
    if (argc == 3 && std::string(argv[1]) == "--profile") profile = static_cast<unsigned>(std::stoul(argv[2]));
    Check(profile < d::kCanonicalFilespacePageProfiles.size(), "a canonical page profile");
    const Base b(profile);
    std::vector<byte> boundary_file, both_file;
    State boundary_state, both_state;
    Growth(b, boundary_file, boundary_state, both_file, both_state);
    Exhaustion(b, boundary_file);
    Losses(b, boundary_file, boundary_state, "header boundary");
    Faults(b, boundary_file, boundary_state, "header boundary");
    if (!both_file.empty()) {
      Losses(b, both_file, both_state, "both boundaries");
      Faults(b, both_file, both_state, "both boundaries");
    }
    std::cout << "PASS native transaction chain growth page_size=" << b.size << " checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL native transaction chain growth profile=" << profile << ": " << e.what() << " checks=" << checks
              << '\n';
    return 1;
  }
}
