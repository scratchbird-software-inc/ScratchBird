// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_TRANSACTION_INVENTORY_WRITER
// The in-place transaction writer (BEGIN, COMMIT, ROLLBACK, restart
// dead-marking) on a database written by the production create, one page size
// per run (--profile N). Every file state is compared byte for byte, through
// the page codecs, against an independent oracle of the TIP words, counters
// and header records, and read back through the production fixed-root lease
// in a separate process or after the writer has gone, with the stored-horizon
// rule open verification applies (stored <= projected).
//   - Rules: the restart step is one-shot and required; every refused request
//     writes nothing; a completed transaction cannot complete again; a stored
//     horizon above what the inventory projects is refused at BEGIN and by the
//     reader.
//   - Read-only database: restart and every call write nothing.
//   - Capacity: every formatted header slot is used; the next BEGIN is refused
//     chain_full with no write.
//   - Process loss after every write of a scripted sequence (fork + _exit),
//     and every write torn at its half-way point: the file is exactly the
//     oracle's write prefix (a torn write leaves the old or the new unit), the
//     reader admits it, restart in a new process marks exactly the oracle's
//     in-progress transactions dead, the next BEGIN takes the stored Next (no
//     number is reused) and commits; nothing outside the two chain pages
//     changes.
//   - Every read, write and sync failed in turn: a read failure changes
//     nothing and the call succeeds again; a write or sync failure fails the
//     writer, which then refuses everything with no I/O; the file is exactly
//     the prefix of the writes performed; a COMMIT that fails before its state
//     byte was attempted is never committed; restart resolves the file.
// Limit: fork + _exit and in-process faults model process death and I/O
// errors; the kernel page cache survives, so nothing here is evidence about
// power loss or device write reordering.
#include "native_database_create.hpp"
#include "admitted_datatype_cohort.hpp"
#include "datatype_catalog_manifest.hpp"
#include "native_fixed_root_memory_lease.hpp"
#include "native_catalog_relation_lease_reader.hpp"
#include "transaction_inventory_writer.hpp"
#include "transaction_inventory_reader.hpp"
#include "transaction_inventory_page.hpp"
#include "transaction_horizon_projection.hpp"
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
// A UUIDv7 whose time prefix is `ms`. The script's times are offsets from
// kT, one second after the creator's commit time (the production create
// stamps the creator from the issuer's clock), so the caller's clock is
// later than every stored commit time unless a step says otherwise.
u64 kT = 0;
Uuid V7(u64 ms, unsigned salt) {
  Uuid id;
  for (int i = 0; i < 6; ++i) id.bytes[i] = static_cast<byte>(ms >> (8 * (5 - i)));
  id.bytes[6] = 0x70;
  id.bytes[7] = static_cast<byte>(salt);
  id.bytes[8] = 0x80;
  id.bytes[9] = 0x5b;
  id.bytes[14] = static_cast<byte>(salt >> 8);
  id.bytes[15] = static_cast<byte>(salt);
  return id;
}
u64 Ns(u64 ms) { return ms * 1'000'000; }
// The begin time, decoded here independently of the codec helpers.
u64 BeginNs(const Uuid& id) {
  u64 ms = 0;
  for (int i = 0; i < 6; ++i) ms = ms << 8 | id.bytes[i];
  return Ns(ms);
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

// A database written by the production create, and the root of its
// transaction-state chain as page zero names it.
struct Base {
  scratchbird::tests::OwnedTempDirectory dir;
  std::string path;
  u32 size = 0;
  Uuid database, primary, profile;
  d::FilespaceRootReference root;
  u64 header_page = 0;
  std::vector<byte> bytes;
  explicit Base(unsigned index) {
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
      Check(device.Close().ok(), "close the created file");
    }
    {
      d::FileDevice device;
      Check(device.Open(path, d::FileOpenMode::open_existing_read_only).ok(), "reopen read-only");
      const std::vector<d::NativeFilespaceDevice> files{{primary, profile, &device}};
      std::vector<byte> backing(64u << 20);
      auto fixed = db::AcquireNativeFixedRootBackedLease(database, files, primary, {256ull * size, backing.size()},
                                                         backing);
      Check(fixed.ok() && (*fixed.lease).Root(4), "page zero names the transaction-state chain");
      root = *(*fixed.lease).Root(4);
      fixed.lease.Reset();
      Check(device.Close().ok(), "close");
    }
    bytes = ReadAll(path);
    const auto tip = pg::DecodeTransactionStatePage(std::span<const byte>(bytes).subspan(root.page_number * size, size));
    Check(tip.ok() && tip.view->next_tip_page == 0, "the create writes one transaction-state page");
    header_page = tip.view->header_chain_page;
    const auto headers = pg::DecodeTransactionHeaderPage(std::span<const byte>(bytes).subspan(header_page * size, size));
    Check(headers.ok(), "the created header page decodes");
    const auto creator = pg::TransactionHeaderAt(*headers.view, 1);
    Check(creator && creator->final_state == pg::TipStateCode::committed && creator->commit_time_utc_ns,
          "the creator's header is committed with a commit time");
    kT = creator->commit_time_utc_ns / 1'000'000 + 1000;
  }
  pg::TransactionWriterBinding Binding(bool read_only = false) const {
    pg::TransactionWriterBinding b;
    b.database_uuid = database;
    b.state_root = root;
    b.read_only_database = read_only;
    return b;
  }
  std::string Fresh(const std::string& name) const {
    const auto p = (dir.path() / name).string();
    WriteAll(p, bytes);
    return p;
  }
  // Puts the created chain pages back (the only pages the writer changes;
  // the process-loss sweep checks that on every file).
  void RestoreChains(const std::string& p) const {
    const std::span<const byte> all(bytes);
    WriteAt(p, root.page_number * size, all.subspan(root.page_number * size, size));
    WriteAt(p, header_page * size, all.subspan(header_page * size, size));
  }
  // Every byte outside the two chain pages is the created file's.
  void Untouched(std::span<const byte> file, std::string_view label) const {
    Check(file.size() == bytes.size(), std::string(label) + ": the file keeps its size");
    for (u64 page = 0; page < file.size() / size; ++page) {
      if (page == root.page_number || page == header_page) continue;
      if (!std::equal(file.begin() + page * size, file.begin() + (page + 1) * size, bytes.begin() + page * size))
        Check(false, std::string(label) + ": page " + std::to_string(page) + " outside the chains changed");
    }
    ++checks;
  }
};

// The state of the two chain pages, through the page codecs: the counters,
// every used state word, every header record.
struct Raw {
  pg::TipCounters counters;
  std::map<u64, pg::TipStateWord> words;
  std::map<u64, pg::TransactionHeaderRecord> headers;
  bool operator==(const Raw&) const = default;
};
Raw RawOf(const Base& b, std::span<const byte> file) {
  Raw raw;
  const auto tip = pg::DecodeTransactionStatePage(file.subspan(b.root.page_number * b.size, b.size));
  Check(tip.ok() && tip.view->counters, "the state page decodes");
  raw.counters = *tip.view->counters;
  for (u32 slot = 0; slot < tip.view->slots; ++slot) {
    const auto word = pg::TipWordAt(*tip.view, slot);
    Check(word.has_value(), "every word decodes");
    if (word->state != TipStateCode::unused) raw.words[tip.view->first_number + slot] = *word;
  }
  const auto headers = pg::DecodeTransactionHeaderPage(file.subspan(b.header_page * b.size, b.size));
  Check(headers.ok() && headers.view->next_page == 0, "the header page decodes");
  for (u32 slot = 0; slot < headers.view->slots; ++slot)
    if (const auto record = pg::TransactionHeaderAt(*headers.view, slot))
      raw.headers[headers.view->first_number + slot] = *record;
  return raw;
}

// ---- The oracle ---------------------------------------------------------
// Horizons stored at BEGIN of n, from the state before n exists: an entry is
// a used word with a header below n; OIT counts active, read-only active and
// dead entries (the states W1 can produce that the inventory spec's §12 table
// says pin OIT), OAT the active ones, OST follows OAT (no retained snapshot),
// OAR and OLR are the new Next, OCT their minimum with OAT and OST.
pg::TipHorizons BeginHorizons(const Raw& m, u64 n) {
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
  h.oldest_cleanup = std::min({oat, oat, n + 1, n + 1});
  return h;
}
// Restart: from min(OIT, OAT) to Next, an unused, starting, active, read-only
// active or rolling-back slot becomes dead; nothing else changes.
u64 RestartOracle(Raw& m) {
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
  char kind = 'B';  // B begin, C commit, R rollback
  u64 order = 0;
  bool read_only = false;
  u64 now = 0;
  Uuid uuid;
};
Op Begin(u64 n, u64 ms, bool read_only = false) { return {'B', n, read_only, 0, V7(ms, static_cast<unsigned>(n))}; }
Op Commit(u64 n, u64 now) { return {'C', n, false, now, {}}; }
Op Rollback(u64 n, u64 now) { return {'R', n, false, now, {}}; }
// The scripted sequence: 28 writes. Commit times: the clock steps back for 4
// and repeats for 5 (each must still follow the latest stored commit time);
// 7 rolls back at a time before it began (completion is clamped to its begin).
std::vector<Op> Script() {
  return {Begin(2, kT + 2000),        Begin(3, kT + 3000, true), Commit(2, Ns(kT + 10000)),
          Begin(4, kT + 4000),        Rollback(3, Ns(kT + 11000)), Commit(4, Ns(kT + 9000)),
          Begin(5, kT + 5000),        Commit(5, Ns(kT + 9000)),  Begin(6, kT + 6000),
          Begin(7, kT + 60000),       Rollback(7, Ns(kT + 12000))};
}
pg::TransactionHeaderRecord HeaderOf(const Op& op) {
  pg::TransactionHeaderRecord r;
  r.transaction_uuid = op.uuid;
  r.read_only = op.read_only;
  return r;
}

// One oracle step per write the writer makes for an operation.
using Step = std::function<void(Raw&)>;
void AddSteps(std::vector<Step>& out, const Op& op, const Uuid& database) {
  const u64 n = op.order;
  if (op.kind == 'B') {
    out.push_back([n](Raw& m) {
      const auto h = BeginHorizons(m, n);
      m.counters.next_transaction = n + 1;
      m.counters.horizons = h;
    });
    auto r = HeaderOf(op);
    r.transaction_order = n;
    r.database_uuid = database;
    out.push_back([n, r](Raw& m) { m.headers[n] = r; });
    const auto state = op.read_only ? TipStateCode::read_only_active : TipStateCode::active;
    out.push_back([n, state](Raw& m) { m.words[n] = pg::TipStateWord{state, pg::kTipFlagHasDetailRecord}; });
    return;
  }
  const auto final_state = op.kind == 'C' ? TipStateCode::committed : TipStateCode::rolled_back;
  out.push_back([n, now = op.now, final_state](Raw& m) {
    auto& r = m.headers.at(n);
    u64 at = std::max(now, BeginNs(r.transaction_uuid));
    if (final_state == TipStateCode::committed) {
      u64 latest = 0;
      for (const auto& [k, h] : m.headers) latest = std::max(latest, h.commit_time_utc_ns);
      at = std::max(at, latest + 1);
      r.commit_time_utc_ns = at;
    }
    r.final_state = final_state;
    r.completed_at_utc_ns = at;
  });
  out.push_back([n, final_state](Raw& m) { m.words.at(n).state = final_state; });
}
std::vector<Step> Steps(std::span<const Op> ops, const Uuid& database) {
  std::vector<Step> out;
  for (const auto& op : ops) AddSteps(out, op, database);
  return out;
}
Raw Prefix(Raw m, const std::vector<Step>& steps, std::size_t k) {
  for (std::size_t i = 0; i < k; ++i) steps[i](m);
  return m;
}

// ---- Running the writer ---------------------------------------------------
struct Done {
  WE error = WE::invalid_request;
  std::optional<u64> order;
  bool uncertain = false;
  u32 writes = 0, syncs = 0;
  bool ok() const { return error == WE::none; }
};
Done Do(pg::TransactionInventoryWriter& w, d::CarefulWriteSink& sink, const Op& op) {
  if (op.kind == 'B') {
    const auto r = w.Begin(sink, HeaderOf(op));
    return {r.error, r.transaction_order, false, r.writes, r.syncs};
  }
  const auto r = op.kind == 'C' ? w.Commit(sink, op.order, op.now) : w.Rollback(sink, op.order, op.now);
  return {r.error, r.transaction_order, r.outcome_uncertain, r.writes, r.syncs};
}

// The production reader's view of a file, from a device opened read-only,
// and the stored-horizon rule open verification applies.
struct Seen {
  pg::TipCounters counters;
  std::vector<mga::TransactionInventoryEntry> entries;
  bool admissible = false;
};
Seen LeaseRead(const Base& b, const std::string& path) {
  d::FileDevice device;
  Check(device.Open(path, d::FileOpenMode::open_existing_read_only).ok(), "independent read-only open");
  Seen out;
  {
    const std::vector<d::NativeFilespaceDevice> files{{b.primary, b.profile, &device}};
    static std::vector<byte> backing(64u << 20);  // reused: the lease is reset before the next read
    auto fixed = db::AcquireNativeFixedRootBackedLease(b.database, files, b.primary, {256ull * b.size, backing.size()},
                                                       backing);
    if (!fixed.ok())
      std::cerr << "lease error=" << int(fixed.error) << " inventory=" << int(fixed.inventory_error) << '\n';
    Check(fixed.ok(), "the production fixed-root lease reads the file");
    const auto& lease = *fixed.lease;
    out.counters = lease.counters();
    const auto& inventory = lease.inventory();
    out.entries.assign(inventory.entries.begin(), inventory.entries.end());
    const pg::NativeTransactionInventoryConstView view{inventory.next_local_transaction_id,
                                                      inventory.next_commit_sequence, inventory.entries};
    mga::LocalTransactionHorizons projected;
    Check(!mga::detail::ProjectValidatedLocalHorizons(view, {}, projected), "the inventory projects horizons");
    out.admissible = pg::StoredTransactionHorizonsAdmissible(out.counters, inventory.next_local_transaction_id, projected);
    fixed.lease.Reset();
  }
  Check(device.Close().ok(), "close the reader");
  return out;
}
// The reader's entries are the oracle's used words that have a header, in
// order, each in its in-memory state; committed ones in commit-time order.
void CheckEntries(const Seen& seen, const Raw& m, std::string_view label) {
  std::vector<std::pair<u64, mga::TransactionState>> expected;
  for (const auto& [k, w] : m.words) {
    if (!m.headers.count(k)) continue;
    const auto state = w.state == TipStateCode::committed          ? mga::TransactionState::committed
                       : w.state == TipStateCode::rolled_back      ? mga::TransactionState::rolled_back
                       : w.state == TipStateCode::active           ? mga::TransactionState::active
                       : w.state == TipStateCode::read_only_active ? mga::TransactionState::read_only_active
                       : w.state == TipStateCode::dead             ? mga::TransactionState::failed_terminal
                                                                   : mga::TransactionState::none;
    expected.emplace_back(k, state);
  }
  Check(seen.entries.size() == expected.size(), std::string(label) + ": one reader entry per transaction");
  std::vector<std::pair<u64, u64>> commits;  // (commit time, order)
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const auto& e = seen.entries[i];
    Check(e.identity.local_id.value == expected[i].first && e.state == expected[i].second &&
              e.identity.transaction_uuid.value == m.headers.at(expected[i].first).transaction_uuid,
          std::string(label) + ": the reader's entry is the oracle's");
    if (e.state == mga::TransactionState::committed)
      commits.emplace_back(m.headers.at(e.identity.local_id.value).commit_time_utc_ns, e.identity.local_id.value);
  }
  std::sort(commits.begin(), commits.end());
  for (std::size_t i = 0; i < commits.size(); ++i) {
    const auto at = std::find_if(seen.entries.begin(), seen.entries.end(),
                                 [&](const auto& e) { return e.identity.local_id.value == commits[i].second; });
    Check(at->commit_sequence == i + 1, std::string(label) + ": commit sequences follow commit time");
  }
}
void ReadBack(const Base& b, const std::string& path, const Raw& expected, std::string_view label) {
  const auto file = ReadAll(path);
  Check(RawOf(b, file) == expected, std::string(label) + ": the file is the oracle's state");
  const auto seen = LeaseRead(b, path);
  Check(seen.counters == expected.counters && seen.admissible,
        std::string(label) + ": the reader admits the stored horizons");
  CheckEntries(seen, expected, label);
}

// ---- Rules -----------------------------------------------------------------
void Rules(const Base& b) {
  const auto path = b.Fresh("rules.db");
  const Raw base = RawOf(b, b.bytes);
  Check(base.counters.next_transaction == 2 && base.words.size() == 1 &&
            base.words.at(1).state == TipStateCode::committed && base.headers.size() == 1,
        "the create commits transaction 1 and stores Next = 2");
  Check(base.counters.horizons == pg::TipHorizons{2, 2, 2, 2, 2, 2}, "the created horizons are all Next");
  d::FileDevice device;
  Check(device.Open(path, d::FileOpenMode::open_existing).ok(), "open");
  Raw model = base;
  {
    d::FileDeviceCarefulWriteSink sink(device);
    pg::TransactionInventoryWriter w(b.Binding());
    const auto before = ReadAll(path);
    const auto first = Begin(2, kT + 2000);
    Check(Do(w, sink, first).error == WE::restart_required && Do(w, sink, Commit(1, Ns(kT))).error == WE::restart_required,
          "nothing is written before the restart step");
    const auto restarted = w.Restart(sink);
    Check(restarted.ok() && restarted.marked_dead == 0 && restarted.writes == 0 && restarted.next_transaction == 2,
          "restart of a clean file marks nothing and writes nothing");
    Check(w.Restart(sink).error == WE::restart_already_run, "the restart step runs once per writer");
    // Requests refused with no write and the writer still usable.
    auto bad = HeaderOf(first);
    bad.transaction_uuid = Fixed(77);
    bad.transaction_uuid.bytes[6] = 0x40;  // version 4
    const auto not_v7 = w.Begin(sink, bad);
    Check(not_v7.error == WE::invalid_header && not_v7.header_error != pg::TransactionHeaderError::none,
          "a transaction id that is not a UUIDv7 is refused");
    bad = HeaderOf(first);
    bad.database_uuid = Fixed(99);
    Check(w.Begin(sink, bad).error == WE::invalid_request, "another database's id is refused");
    bad = HeaderOf(first);
    bad.authority = pg::TipAuthorityClass::cluster;
    Check(w.Begin(sink, bad).error == WE::invalid_request, "cluster authority belongs to the cluster library");
    bad = HeaderOf(first);
    bad.completed_at_utc_ns = 1;
    Check(w.Begin(sink, bad).error == WE::invalid_request, "a BEGIN cannot carry a completion");
    Check(w.Commit(sink, 0, Ns(kT)).error == WE::invalid_request && w.Commit(sink, 1, 0).error == WE::invalid_request,
          "order 0 and a zero clock are refused");
    Check(w.Commit(sink, 1, Ns(kT)).error == WE::invalid_state, "the committed creator cannot complete again");
    Check(w.Commit(sink, 2, Ns(kT)).error == WE::unknown_transaction &&
              w.Rollback(sink, 9, Ns(kT)).error == WE::unknown_transaction,
          "a number at or beyond Next is unknown");
    Check(ReadAll(path) == before && !w.failed(), "no refused request writes anything");
    // A transaction completes once.
    const bool forced = device.forced_ordered_writes();
    const auto begun = Do(w, sink, first);
    Check(begun.ok() && begun.order == 2u && begun.writes == 3 && begun.syncs == (forced ? 1u : 3u),
          "BEGIN: three writes, a barrier before each dependent one");
    for (const auto& s : Steps(std::span<const Op>(&first, 1), b.database)) s(model);
    const auto commit = Commit(2, Ns(kT + 10000));
    const auto committed = Do(w, sink, commit);
    Check(committed.ok() && committed.writes == 2 && committed.syncs == (forced ? 1u : 2u),
          "COMMIT: two writes, the header durable before the state byte");
    for (const auto& s : Steps(std::span<const Op>(&commit, 1), b.database)) s(model);
    Check(Do(w, sink, Commit(2, Ns(kT + 20000))).error == WE::invalid_state &&
              Do(w, sink, Rollback(2, Ns(kT + 20000))).error == WE::invalid_state,
          "a completed transaction cannot complete again");
    const auto third = Begin(3, kT + 3000);
    Check(Do(w, sink, third).ok(), "BEGIN 3");
    for (const auto& s : Steps(std::span<const Op>(&third, 1), b.database)) s(model);
  }
  Check(device.Close().ok(), "close");
  ReadBack(b, path, model, "rules");
  // A stored OAT above what the inventory projects (3 is active): the next
  // BEGIN refuses with no write, and the reader refuses the file.
  {
    auto file = ReadAll(path);
    auto tip = std::span<byte>(file).subspan(b.root.page_number * b.size, b.size);
    auto raised = model.counters;
    raised.horizons.oldest_active = 4;
    const auto edit = pg::SetTransactionStateCounters(tip, raised);
    Check(edit.ok() && edit.changed, "raise the stored OAT through the codec");
    WriteAt(path, b.root.page_number * b.size + edit.unit_offset,
            tip.subspan(edit.unit_offset, pg::kTransactionStateWriteUnitBytes));
  }
  Check(!LeaseRead(b, path).admissible, "the reader refuses a stored horizon ahead of the projection");
  Check(device.Open(path, d::FileOpenMode::open_existing).ok(), "reopen");
  {
    d::FileDeviceCarefulWriteSink sink(device);
    pg::TransactionInventoryWriter w(b.Binding());
    Check(w.Restart(sink).ok(), "restart");  // marks 3 dead: it was left in progress
    const auto before = ReadAll(path);
    // After restart 3 is dead: OIT is pinned at 3, and the stored OIT is 3.
    // Raise OIT instead, so that the computed OIT is below the stored one.
    auto file = ReadAll(path);
    auto tip = std::span<byte>(file).subspan(b.root.page_number * b.size, b.size);
    auto c = *pg::DecodeTransactionStatePage(tip).view->counters;
    c.horizons.oldest_interesting = 4;
    const auto edit = pg::SetTransactionStateCounters(tip, c);
    Check(edit.ok(), "raise the stored OIT");
    WriteAt(path, b.root.page_number * b.size + edit.unit_offset,
            tip.subspan(edit.unit_offset, pg::kTransactionStateWriteUnitBytes));
    const auto raised = ReadAll(path);
    Check(raised != before, "the raised OIT is on disk");
    const auto refused = w.Begin(sink, HeaderOf(Begin(4, kT + 4000)));
    Check(refused.error == WE::horizon_regression && refused.writes == 0 && refused.syncs == 0 && !w.failed() &&
              ReadAll(path) == raised,
          "a computed horizon below the stored one is refused with no write");
  }
  Check(device.Close().ok(), "close");
  fs::remove(path);
}

// ---- Read-only database -------------------------------------------------------
void ReadOnly(const Base& b) {
  const auto path = b.Fresh("read-only.db");
  d::FileDevice device;
  Check(device.Open(path, d::FileOpenMode::open_existing).ok(), "open");
  {
    d::FileDeviceCarefulWriteSink sink(device);
    pg::TransactionInventoryWriter w(b.Binding());
    Check(w.Restart(sink).ok() && Do(w, sink, Begin(2, kT + 2000)).ok(), "leave transaction 2 in progress");
  }
  const auto before = ReadAll(path);
  {
    d::FileDeviceCarefulWriteSink sink(device);
    pg::TransactionInventoryWriter w(b.Binding(true));
    Check(Do(w, sink, Begin(3, kT + 3000)).error == WE::read_only_database, "BEGIN is refused");
    const auto r = w.Restart(sink);
    Check(r.ok() && r.read_only_skipped && r.writes == 0 && r.syncs == 0 && r.next_transaction == 3,
          "restart of a read-only database checks the chains and writes nothing");
    Check(Do(w, sink, Begin(3, kT + 3000)).error == WE::read_only_database &&
              w.Commit(sink, 2, Ns(kT + 9000)).error == WE::read_only_database &&
              w.Rollback(sink, 2, Ns(kT + 9000)).error == WE::read_only_database,
          "BEGIN, COMMIT and ROLLBACK are refused");
  }
  Check(device.Close().ok(), "close");
  Check(ReadAll(path) == before, "a read-only database gets no write: transaction 2 stays as it was");
  fs::remove(path);
}

// ---- Capacity --------------------------------------------------------------------
void Capacity(const Base& b) {
  const auto path = b.Fresh("capacity.db");
  Raw model = RawOf(b, b.bytes);
  const u32 limit = pg::TransactionHeaderSlotsPerPage(b.size) - 2;  // 0 reserved, 1 the creator
  Check(limit + 2 < pg::TipSlotsPerPage(b.size), "the header page fills before the state page");
  d::FileDevice device;
  Check(device.Open(path, d::FileOpenMode::open_existing).ok(), "open");
  std::chrono::nanoseconds begin_time{}, complete_time{};
  u32 completions = 0;
  {
    d::FileDeviceCarefulWriteSink sink(device);
    pg::TransactionInventoryWriter w(b.Binding());
    Check(w.Restart(sink).ok(), "restart");
    for (u32 i = 0; i < limit; ++i) {
      const u64 n = 2 + i;
      const auto op = Begin(n, kT + n);
      const auto t0 = std::chrono::steady_clock::now();
      const auto r = Do(w, sink, op);
      begin_time += std::chrono::steady_clock::now() - t0;
      Check(r.ok() && r.order == n, "BEGIN within the formatted slots");
      for (const auto& s : Steps(std::span<const Op>(&op, 1), b.database)) s(model);
      if (i % 3 == 2) continue;  // left in progress
      const auto done = i % 3 == 0 ? Commit(n, Ns(kT + 500000 + n)) : Rollback(n, Ns(kT + 500000 + n));
      const auto t1 = std::chrono::steady_clock::now();
      Check(Do(w, sink, done).ok(), "complete");
      complete_time += std::chrono::steady_clock::now() - t1;
      ++completions;
      for (const auto& s : Steps(std::span<const Op>(&done, 1), b.database)) s(model);
    }
    const auto before = ReadAll(path);
    const auto full = Do(w, sink, Begin(limit + 2, kT + 900000));
    Check(full.error == WE::chain_full && full.order == u64{limit + 2} && full.writes == 0 && full.syncs == 0 &&
              !w.failed() && ReadAll(path) == before,
          "the BEGIN past the last formatted header slot is refused chain_full with no write");
  }
  Check(device.Close().ok(), "close");
  ReadBack(b, path, model, "capacity");
  std::cout << "capacity page_size=" << b.size << " transactions=" << limit
            << " mean_begin_us=" << begin_time.count() / 1000 / limit
            << " mean_complete_us=" << (completions ? complete_time.count() / 1000 / completions : 0)
            << " (this machine, this filesystem; not a performance claim)\n";
  fs::remove(path);
}

// ---- Process loss and torn writes ---------------------------------------------
[[noreturn]] void Child(const Base& b, const std::string& path, u32 limit, bool torn) {
  try {
    d::FileDevice device;
    if (!device.Open(path, d::FileOpenMode::open_existing).ok()) _exit(kChildFailed);
    int code = kChildCompleted;
    {
      d::FileDeviceCarefulWriteSink file(device);
      std::optional<scratchbird::tests::CrashAfterWritesSink> crash;
      std::optional<scratchbird::tests::TornWriteSink> tear;
      d::CarefulWriteSink* sink = &file;
      if (torn) sink = &tear.emplace(file, limit, kPlannedCrash);
      else sink = &crash.emplace(file, limit, kPlannedCrash);
      pg::TransactionInventoryWriter w(b.Binding());
      if (!w.Restart(*sink).ok()) code = kChildFailed;
      for (const auto& op : Script()) {
        if (code != kChildCompleted) break;
        const auto r = Do(w, *sink, op);
        if (!r.ok() || r.order != op.order) code = kChildFailed;
      }
    }
    if (!device.Close().ok()) code = kChildFailed;
    _exit(code);
  } catch (...) {
    _exit(kChildException);
  }
}

// The commit and completion times the full script must leave: 2 commits at
// the caller's clock; 4's clock stepped back and 5 read the same clock, so
// each is one nanosecond after the latest stored commit; 3 rolls back at the
// caller's clock; 7 rolls back at a time before it began, so it completes at
// its begin time.
void CheckScriptTimes(const Raw& m, std::string_view label) {
  const auto& h = m.headers;
  Check(h.at(2).commit_time_utc_ns == Ns(kT + 10000) && h.at(2).completed_at_utc_ns == Ns(kT + 10000) &&
            h.at(3).commit_time_utc_ns == 0 && h.at(3).completed_at_utc_ns == Ns(kT + 11000) &&
            h.at(4).commit_time_utc_ns == Ns(kT + 10000) + 1 && h.at(5).commit_time_utc_ns == Ns(kT + 10000) + 2 &&
            h.at(7).commit_time_utc_ns == 0 && h.at(7).completed_at_utc_ns == Ns(kT + 60000) &&
            h.at(6).final_state == TipStateCode::unused,
        std::string(label) + ": commit times follow the caller's clock and never go back");
}

void ProcessLoss(const Base& b, bool torn) {
  const auto ops = Script();
  const auto steps = Steps(ops, b.database);
  const u32 total = static_cast<u32>(steps.size());
  Check(total == 28, "the script makes 28 writes");
  const Raw base = RawOf(b, b.bytes);
  unsigned old_units = 0, new_units = 0;
  for (u32 k = torn ? 1 : 0; k <= total + (torn ? 0 : 1); ++k) {
    const bool full = !torn && k == total + 1;
    const std::string label = std::string(torn ? "torn write " : "crash after ") + std::to_string(k) +
                              (full ? " (full run)" : "");
    const auto path = b.Fresh("loss.db");
    std::cout.flush();
    std::cerr.flush();
    const pid_t child = fork();
    Check(child >= 0, label + ": fork");
    if (child == 0) Child(b, path, full ? kNoCrash : k, torn);
    int status = 0;
    Check(waitpid(child, &status, 0) == child && WIFEXITED(status), label + ": the child exits");
    Check(WEXITSTATUS(status) == (full ? kChildCompleted : kPlannedCrash),
          label + ": child exit code " + std::to_string(WEXITSTATUS(status)));
    const auto file = ReadAll(path);
    b.Untouched(file, label);
    // The write prefix; a torn write leaves its unit old or new.
    Raw expected = Prefix(base, steps, full ? total : torn ? k - 1 : k);
    const Raw seen = RawOf(b, file);
    if (torn && seen != expected) {
      expected = Prefix(base, steps, k);
      ++new_units;
    } else if (torn) {
      ++old_units;
    }
    Check(seen == expected, label + ": the file is the oracle's write prefix");
    if (full) CheckScriptTimes(seen, label);
    const auto read = LeaseRead(b, path);
    Check(read.counters == expected.counters && read.admissible, label + ": the reader admits the file");
    CheckEntries(read, expected, label);
    // A new process: restart marks exactly the in-progress transactions dead;
    // the next number is the stored Next, and it commits.
    Raw after = expected;
    const u64 marked = RestartOracle(after);
    const u64 next = after.counters.next_transaction;
    const auto begin = Begin(next, kT + 100000 + k);
    const auto commit = Commit(next, Ns(kT + 200000 + k));
    {
      d::FileDevice device;
      Check(device.Open(path, d::FileOpenMode::open_existing).ok(), label + ": reopen");
      {
        d::FileDeviceCarefulWriteSink sink(device);
        pg::TransactionInventoryWriter w(b.Binding());
        const auto r = w.Restart(sink);
        Check(r.ok() && r.marked_dead == marked && r.unresolved.empty(),
              label + ": restart marks the oracle's in-progress transactions dead");
      }
      Check(device.Close().ok(), label + ": close");
    }
    ReadBack(b, path, after, label + " after restart");
    {
      // Opened again: its restart finds nothing in progress.
      d::FileDevice device;
      Check(device.Open(path, d::FileOpenMode::open_existing).ok(), label + ": reopen");
      {
        d::FileDeviceCarefulWriteSink sink(device);
        pg::TransactionInventoryWriter w(b.Binding());
        const auto r = w.Restart(sink);
        Check(r.ok() && r.marked_dead == 0 && r.writes == 0, label + ": a second restart has nothing to mark");
        const auto begun = Do(w, sink, begin);
        Check(begun.ok() && begun.order == next, label + ": the next BEGIN takes the stored Next");
        Check(Do(w, sink, commit).ok(), label + ": and commits");
      }
      Check(device.Close().ok(), label + ": close");
    }
    for (const auto& op : {begin, commit})
      for (const auto& s : Steps(std::span<const Op>(&op, 1), b.database)) s(after);
    ReadBack(b, path, after, label + " restarted");
    b.Untouched(ReadAll(path), label + " restarted");
    fs::remove(path);
  }
  if (torn) Check(old_units > 0 && new_units > 0, "torn writes left both old and new units");
  std::cout << (torn ? "torn writes " : "process loss ") << "page_size=" << b.size << " cases="
            << (torn ? total : total + 2) << (torn ? " old=" + std::to_string(old_units) + " new=" +
                                                         std::to_string(new_units)
                                                   : std::string())
            << '\n';
}

// ---- I/O failures -----------------------------------------------------------------
// Fails the sink call numbered `fail_at` (1-based, reads, writes and syncs
// counted together) without performing it.
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
  u32 calls() const { return calls_; }
  u32 writes() const { return writes_; }
  bool reached() const { return calls_ >= fail_at_; }
  d::BoundedIoOperation failed_kind() const { return kind_; }

 private:
  d::BoundedIoResult Fail(d::BoundedIoOperation kind, u64 offset, usize bytes) {
    kind_ = kind;
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
  d::BoundedIoOperation kind_ = d::BoundedIoOperation::size;
};

void Faults(const Base& b) {
  const auto ops = Script();
  const auto steps = Steps(ops, b.database);
  const Raw base = RawOf(b, b.bytes);
  const auto path = b.Fresh("faults.db");
  unsigned points = 0, read_faults = 0, write_faults = 0, sync_faults = 0, uncertain = 0;
  for (u32 at = 1;; ++at) {
    const std::string label = "fault at call " + std::to_string(at);
    b.RestoreChains(path);
    d::FileDevice device;
    Check(device.Open(path, d::FileOpenMode::open_existing).ok(), label + ": open");
    bool reached = false;
    u32 performed = 0;
    std::optional<Op> failed_completion;
    bool failed_uncertain = false;
    {
      d::FileDeviceCarefulWriteSink file(device);
      FaultSink sink(file, at);
      pg::TransactionInventoryWriter w(b.Binding());
      auto restart = w.Restart(sink);
      if (!restart.ok()) {
        Check(restart.error == WE::io_failure && !w.failed() && !w.restarted() && restart.writes == 0,
              label + ": a failed read in restart changes nothing");
        restart = w.Restart(sink);
        Check(restart.ok(), label + ": and restart succeeds again");
      }
      for (const auto& op : ops) {
        auto r = Do(w, sink, op);
        if (r.ok()) {
          Check(r.order == op.order, label + ": in order");
          continue;
        }
        if (r.error == WE::io_failure) {
          Check(!w.failed() && r.writes == 0 && r.syncs == 0, label + ": a failed read writes nothing");
          r = Do(w, sink, op);
          Check(r.ok() && r.order == op.order, label + ": and the same call succeeds again");
          continue;
        }
        Check(r.error == WE::write_failed && w.failed(), label + ": a failed write or sync fails the writer");
        if (op.kind != 'B') {
          failed_completion = op;
          failed_uncertain = r.uncertain;
        }
        const auto calls = sink.calls();
        Check(Do(w, sink, ops.front()).error == WE::writer_failed && Do(w, sink, op).error == WE::writer_failed &&
                  w.Restart(sink).error == WE::writer_failed && sink.calls() == calls,
              label + ": a failed writer refuses everything with no I/O");
        break;
      }
      reached = sink.reached();
      performed = sink.writes();
      if (reached) {
        const auto kind = sink.failed_kind();
        (kind == d::BoundedIoOperation::read ? read_faults : kind == d::BoundedIoOperation::write ? write_faults
                                                                                                  : sync_faults)++;
      }
    }
    Check(device.Close().ok(), label + ": close");
    // Exactly the writes that were performed, then restart resolves the file.
    Raw expected = Prefix(base, steps, performed);
    ReadBack(b, path, expected, label);
    if (failed_completion) {
      const auto state = expected.words.at(failed_completion->order).state;
      if (!failed_uncertain)
        Check(state == TipStateCode::active || state == TipStateCode::read_only_active,
              label + ": a completion that failed before its state byte was attempted is still in progress");
      else
        ++uncertain;
    }
    Raw after = expected;
    const u64 marked = RestartOracle(after);
    {
      Check(device.Open(path, d::FileOpenMode::open_existing).ok(), label + ": reopen");
      {
        d::FileDeviceCarefulWriteSink sink(device);
        pg::TransactionInventoryWriter w(b.Binding());
        const auto r = w.Restart(sink);
        Check(r.ok() && r.marked_dead == marked, label + ": restart resolves the file");
      }
      Check(device.Close().ok(), label + ": close");
    }
    ReadBack(b, path, after, label + " restarted");
    if (!reached) break;  // the whole script ran without reaching call `at`
    ++points;
  }
  Check(points > 50 && read_faults > 0 && write_faults > 0 && sync_faults > 0 && uncertain > 0,
        "every kind of sink call was failed, and some completions were left uncertain");
  b.Untouched(ReadAll(path), "faults");
  fs::remove(path);
  std::cout << "faults page_size=" << b.size << " points=" << points << " reads=" << read_faults
            << " writes=" << write_faults << " syncs=" << sync_faults << " uncertain_completions=" << uncertain << '\n';
}
}  // namespace

int main(int argc, char** argv) {
  unsigned profile = 0;
  try {
    if (argc == 3 && std::string(argv[1]) == "--profile") profile = static_cast<unsigned>(std::stoul(argv[2]));
    Check(profile < d::kCanonicalFilespacePageProfiles.size(), "a canonical page profile");
    const Base b(profile);
    Rules(b);
    ReadOnly(b);
    Capacity(b);
    ProcessLoss(b, false);
    ProcessLoss(b, true);
    Faults(b);
    std::cout << "PASS native transaction inventory writer page_size=" << b.size << " checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL native transaction inventory writer profile=" << profile << ": " << e.what()
              << " checks=" << checks << '\n';
    return 1;
  }
}
