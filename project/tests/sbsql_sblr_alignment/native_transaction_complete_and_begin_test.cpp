// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_TRANSACTION_INVENTORY_WRITER, SB_TRANSACTION_COMPLETE_AND_BEGIN
// Plan 36 step T1 (owner Q150 = 1; Q305/Q308 2026-11-19):
// TransactionInventoryWriter::CompleteAndBegin, the commit or rollback of
// transaction n and the start of its replacement m as ONE careful-write plan,
// on a database written by the production create, one page size per run
// (--profile N, optional --only rules|shapes|sweep|faults|real|measure).
//   - Rules: every refusal (restart not run, an unknown final state, a bad
//     order or clock, a bad header for m, n unknown / already final / dead, a
//     horizon regression, chain_full, a read-only database, a failed writer)
//     leaves the file byte-identical and the sink untouched.
//   - Shapes: the writes the plan makes are those the geometry predicts. n's
//     and m's header records share a 512-byte unit when n's header slot is
//     even, their state words when n's word is not last of its 128-word unit
//     or page; the plan is then 5, 4 (words merged) or 3 (both) nodes. The
//     headers-only merge cannot occur at any canonical page size (it would
//     need an even n at a word-unit end, which is always odd): proven here by
//     arithmetic over every scenario's geometry, the code path exists.
//   - Sweep: for every scenario, a crash after every write and a tear of
//     every write at every boundary no stage straddles (stages: W1 counters,
//     W2 header of m, W3 word of m, W4 header of n, W5 state byte of n): the
//     file is the independent oracle's stage set, the reader's open rule
//     admits it, Restart marks exactly what the plan 36 crash table says dead
//     (n before W5, m always), the next BEGIN takes the stored Next. The
//     scenarios cover the merged and unmerged plans, a sibling transaction in
//     progress, read-only m, and header- and state-page boundaries where chain
//     growth runs first.
//   - Faults: every read, write and sync of a CompleteAndBegin failed in turn.
//   - Real: the sweep's smallest scenarios again on a real file with fork +
//     _exit crash sinks, read back through the production fixed-root lease.
//   - Measure: begin + commit against CompleteAndBegin, writes and syncs, on
//     a real device (the S5 baseline).
// Limit: process death and I/O errors only; the kernel page cache survives, so
// nothing here is evidence about power loss or device write reordering. A
// tear inside one 256-byte header record or one 4-byte state word is outside
// the model (the codecs' atomic units), so tears are taken at boundaries no
// stage straddles.
#include "native_database_create.hpp"
#include "admitted_datatype_cohort.hpp"
#include "datatype_catalog_manifest.hpp"
#include "native_fixed_root_memory_lease.hpp"
#include "native_catalog_relation_lease_reader.hpp"
#include "filespace_page_zero.hpp"
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
#include <cstring>
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

Uuid Fixed(unsigned n) {
  Uuid id;
  id.bytes[0] = 1;
  id.bytes[6] = 0x70;
  id.bytes[8] = 0x80;
  id.bytes[14] = static_cast<byte>(n >> 8);
  id.bytes[15] = static_cast<byte>(n);
  return id;
}
u64 kT = 0;  // ms; one second after the creator's commit time
Uuid V7(u64 ms, unsigned salt) {
  Uuid id;
  for (int i = 0; i < 6; ++i) id.bytes[i] = static_cast<byte>(ms >> (8 * (5 - i)));
  id.bytes[6] = 0x70;
  id.bytes[7] = static_cast<byte>(salt);
  id.bytes[8] = 0x80;
  id.bytes[9] = 0x5c;
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

// ---- A sink over a flat image: logs writes, injects one fault, undoes -------
class ImageSink final : public d::CarefulWriteSink {
 public:
  struct Write {
    u64 offset = 0;
    std::vector<byte> bytes;
  };
  explicit ImageSink(std::vector<byte> image, bool forced = false) : img_(std::move(image)), forced_(forced) {}
  d::BoundedIoResult ReadAt(u64 offset, std::span<byte> bytes) override {
    if (Fault()) return Fail(d::BoundedIoOperation::read, offset, bytes.size());
    ++reads_;
    if (offset + bytes.size() > img_.size()) return Fail(d::BoundedIoOperation::read, offset, bytes.size());
    std::copy_n(img_.begin() + offset, bytes.size(), bytes.begin());
    return Ok(d::BoundedIoOperation::read, offset, bytes.size());
  }
  d::BoundedIoResult WriteAt(u64 offset, std::span<const byte> bytes) override {
    if (Fault()) return Fail(d::BoundedIoOperation::write, offset, bytes.size());
    Apply(offset, bytes);
    write_calls_.push_back(calls_);
    log_.push_back({offset, std::vector<byte>(bytes.begin(), bytes.end())});
    ++writes_;
    return Ok(d::BoundedIoOperation::write, offset, bytes.size());
  }
  d::BoundedIoResult Sync() override {
    if (Fault()) return Fail(d::BoundedIoOperation::sync, 0, 0);
    ++syncs_;
    return Ok(d::BoundedIoOperation::sync, 0, 0);
  }
  bool forced_ordered() const noexcept override { return forced_; }

  // Applies bytes with an undo record (not logged as a writer write).
  void Apply(u64 offset, std::span<const byte> bytes) {
    undo_.push_back({offset, std::vector<byte>(img_.begin() + offset, img_.begin() + offset + bytes.size())});
    std::copy(bytes.begin(), bytes.end(), img_.begin() + offset);
  }
  std::size_t Mark() const { return undo_.size(); }
  void Rollback(std::size_t mark) {
    while (undo_.size() > mark) {
      const auto& e = undo_.back();
      std::copy(e.old.begin(), e.old.end(), img_.begin() + e.offset);
      undo_.pop_back();
    }
  }
  void FailCall(u32 at) {  // 1-based among reads, writes and syncs from now
    calls_ = 0;
    fail_at_ = at;
    failed_kind_.reset();
  }
  void NoFault() { fail_at_ = 0; }
  void ResetCounters() { reads_ = writes_ = syncs_ = 0; calls_ = 0; log_.clear(); write_calls_.clear(); }
  const std::vector<Write>& log() const { return log_; }
  const std::vector<u32>& write_calls() const { return write_calls_; }
  u32 reads() const { return reads_; }
  u32 writes() const { return writes_; }
  u32 syncs() const { return syncs_; }
  u32 calls() const { return calls_; }
  bool reached() const { return fail_at_ != 0 && calls_ >= fail_at_; }
  std::optional<d::BoundedIoOperation> failed_kind() const { return failed_kind_; }
  const std::vector<byte>& image() const { return img_; }
  void SetForced(bool forced) { forced_ = forced; }

 private:
  struct Undo {
    u64 offset;
    std::vector<byte> old;
  };
  bool Fault() {
    ++calls_;
    return fail_at_ != 0 && calls_ == fail_at_;
  }
  static d::BoundedIoResult Ok(d::BoundedIoOperation op, u64 offset, usize n) {
    d::BoundedIoResult r;
    r.operation = op;
    r.offset = offset;
    r.requested_bytes = n;
    r.bytes_transferred = n;
    return r;
  }
  d::BoundedIoResult Fail(d::BoundedIoOperation kind, u64 offset, usize bytes) {
    failed_kind_ = kind;
    d::BoundedIoResult r;
    r.operation = kind;
    r.error = d::BoundedIoError::native_failure;
    r.offset = offset;
    r.requested_bytes = bytes;
    r.native_error = EIO;
    r.native_attempted = true;
    return r;
  }
  std::vector<byte> img_;
  bool forced_ = false;
  std::vector<Undo> undo_;
  std::vector<Write> log_;
  std::vector<u32> write_calls_;
  u32 reads_ = 0, writes_ = 0, syncs_ = 0, calls_ = 0, fail_at_ = 0;
  std::optional<d::BoundedIoOperation> failed_kind_;
};

// ---- A database written by the production create ----------------------------
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
    r.database_default_language = scratchbird::core::catalog::kCatalogLanguage_en;
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
      Check(device.Open(path, d::FileOpenMode::create_new, d::DatabaseFileOwnership::native_data_file).ok(), "node device");
      u::StandaloneUuidV7Issuer issuer({Fixed(1), Fixed(10)}, {{}, 0, 1000});
      auto made = db::CreateNativeDatabaseOnOpenDevice(device, r, issuer);
      Check(made.ok(), "the production create writes a complete native database");
      database = made.receipt->database_uuid;
      primary = made.receipt->filespace_uuid;
      Check(device.Close().ok(), "close the created file");
    }
    {
      d::FileDevice device;
      Check(device.Open(path, d::FileOpenMode::open_existing_read_only, d::DatabaseFileOwnership::native_data_file).ok(),
            "read-only open");
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
    ImageSink sink(bytes);
    const auto chains = pg::ReadTransactionChainsThroughSink(sink, Binding(nullptr));
    Check(chains.error == WE::none, "the created chains read");
    const auto headers = pg::DecodeTransactionHeaderPage(std::span<const byte>(chains.header_images).first(size));
    Check(headers.ok(), "the created header page decodes");
    const auto creator = pg::TransactionHeaderAt(*headers.view, 1);
    Check(creator && creator->commit_time_utc_ns, "the creator is committed with a commit time");
    kT = creator->commit_time_utc_ns / 1'000'000 + 1000;
  }
  pg::TransactionWriterBinding Binding(u::StandaloneUuidV7Issuer* issuer, bool read_only = false) const {
    pg::TransactionWriterBinding b;
    b.database_uuid = database;
    b.state_root = state_root;
    b.read_only_database = read_only;
    if (issuer) {
      b.allocation_root = map_root;
      b.issuer = issuer;
    }
    return b;
  }
};

// ---- The file through the page codecs ----------------------------------------
struct Raw {
  pg::TipCounters counters;
  std::map<u64, pg::TipStateWord> words;
  std::map<u64, pg::TransactionHeaderRecord> headers;
  bool operator==(const Raw&) const = default;
};
Raw RawOf(const pg::TransactionChainsRead& c) {
  Check(c.error == WE::none, "the chains read and check");
  Raw raw;
  raw.counters = c.counters;
  for (std::size_t i = 0; i < c.state_pages.size(); ++i) {
    const auto tip = pg::DecodeTransactionStatePage(std::span<const byte>(c.state_images).subspan(i * c.page_size, c.page_size));
    Check(tip.ok(), "a state page decodes");
    for (u32 slot = 0; slot < tip.view->slots; ++slot) {
      const auto word = pg::TipWordAt(*tip.view, slot);
      Check(word.has_value(), "every word decodes");
      if (word->state != TipStateCode::unused) raw.words[tip.view->first_number + slot] = *word;
    }
  }
  for (std::size_t i = 0; i < c.header_pages.size(); ++i) {
    const auto headers =
        pg::DecodeTransactionHeaderPage(std::span<const byte>(c.header_images).subspan(i * c.page_size, c.page_size));
    Check(headers.ok(), "a header page decodes");
    for (u32 slot = 0; slot < headers.view->slots; ++slot)
      if (const auto record = pg::TransactionHeaderAt(*headers.view, slot))
        raw.headers[headers.view->first_number + slot] = *record;
  }
  return raw;
}
// The production reader's open rule: the inventory builds from the images and
// the stored horizons do not exceed what it projects.
bool Admissible(const pg::TransactionChainsRead& c) {
  const auto built = pg::BuildLocalTransactionInventoryFromImages(c.page_size, c.state_images, c.header_images);
  Check(built.ok(), "the inventory builds from the images");
  mga::LocalTransactionHorizons projected;
  Check(!mga::detail::ProjectValidatedLocalHorizons(built.inventory, {}, projected), "the inventory projects horizons");
  return pg::StoredTransactionHorizonsAdmissible(c.counters, built.inventory.next_local_transaction_id, projected);
}
std::map<u64, mga::TransactionState> EntryStates(const pg::TransactionChainsRead& c) {
  const auto built = pg::BuildLocalTransactionInventoryFromImages(c.page_size, c.state_images, c.header_images);
  Check(built.ok(), "the inventory builds");
  std::map<u64, mga::TransactionState> out;
  for (const auto& e : built.inventory.entries) out[e.identity.local_id.value] = e.state;
  return out;
}

// ---- The oracle ----------------------------------------------------------------
// Horizons stored at BEGIN of `n`, from the state before n exists (see the
// existing writer test's oracle): OIT counts active, read-only active and dead
// entries below n, OAT the in-progress ones, OST follows OAT.
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

// One CompleteAndBegin, as the oracle sees it. Stages: W1 counters (1), W2
// header of m (2), W3 word of m (4), W4 header of n (8), W5 state byte of n
// (16).
enum : unsigned { W1 = 1, W2 = 2, W3 = 4, W4 = 8, W5 = 16 };
struct Call {
  u64 n = 0, m = 0, now = 0;
  TipStateCode final_state = TipStateCode::committed;
  pg::TransactionHeaderRecord header_m;  // as given (order and database set by the writer)
  Raw pre;
  // derived
  pg::TransactionHeaderRecord record_m;
  pg::TipStateWord word_m;
  pg::TipHorizons horizons;
  u64 at = 0;
};
Call MakeCall(const Raw& pre, u64 n, TipStateCode fin, const pg::TransactionHeaderRecord& header_m, u64 now,
              const Uuid& database) {
  Call c;
  c.n = n;
  c.m = pre.counters.next_transaction;
  c.now = now;
  c.final_state = fin;
  c.header_m = header_m;
  c.pre = pre;
  c.record_m = header_m;
  c.record_m.transaction_order = c.m;
  c.record_m.database_uuid = database;
  c.word_m = pg::TipStateWord{header_m.read_only ? TipStateCode::read_only_active : TipStateCode::active,
                              pg::kTipFlagHasDetailRecord, pg::TipAuthorityClass::local, header_m.intent};
  c.horizons = BeginHorizons(pre, c.m);  // n still active, as the file shows it
  c.at = std::max(now, BeginNs(pre.headers.at(n).transaction_uuid));
  if (fin == TipStateCode::committed) {
    u64 latest = 0;
    for (const auto& [k, h] : pre.headers) latest = std::max(latest, h.commit_time_utc_ns);
    c.at = std::max(c.at, latest + 1);
  }
  return c;
}
// A mask may carry, in bits 8..11, a PARTIAL W1: the first k fields of the
// counters unit written (Next, then OIT, OAT, OST, OCT, OAR, OLR in file
// order; k = 1 is Next alone). A tear of W1 at an 8-byte boundary leaves one
// of these; each is a valid, admissible file.
constexpr unsigned PartialCounters(unsigned k) { return k << 8; }
Raw Apply(const Call& c, unsigned mask) {
  Raw r = c.pre;
  if (mask & W1) {
    r.counters.next_transaction = c.m + 1;
    r.counters.horizons = c.horizons;
  } else if (const unsigned k = (mask >> 8) & 15; k > 0) {
    r.counters.next_transaction = c.m + 1;
    u64* dst[] = {&r.counters.horizons.oldest_interesting, &r.counters.horizons.oldest_active,
                  &r.counters.horizons.oldest_snapshot, &r.counters.horizons.oldest_cleanup,
                  &r.counters.horizons.oldest_archive_required, &r.counters.horizons.oldest_limbo_resolution};
    const u64 src[] = {c.horizons.oldest_interesting, c.horizons.oldest_active, c.horizons.oldest_snapshot,
                       c.horizons.oldest_cleanup, c.horizons.oldest_archive_required, c.horizons.oldest_limbo_resolution};
    for (unsigned i = 0; i + 1 < k && i < 6; ++i) *dst[i] = src[i];
  }
  if (mask & W2) r.headers[c.m] = c.record_m;
  if (mask & W3) r.words[c.m] = c.word_m;
  if (mask & W4) {
    auto& h = r.headers.at(c.n);
    h.final_state = c.final_state;
    h.completed_at_utc_ns = c.at;
    if (c.final_state == TipStateCode::committed) h.commit_time_utc_ns = c.at;
  }
  if (mask & W5) r.words.at(c.n).state = c.final_state;
  return r;
}
// The stage sets a tear of a write whose stages are `group` can leave on top
// of `before`: every subset of the stages, and for W1 every counters prefix.
std::vector<unsigned> TearOutcomes(unsigned before, unsigned group) {
  std::vector<unsigned> out{before};
  for (unsigned stage : {W1, W2, W3, W4, W5})
    if (group & stage) {
      const auto cnt = out.size();
      for (std::size_t i = 0; i < cnt; ++i) out.push_back(out[i] | stage);
    }
  if (group & W1)
    for (unsigned k = 1; k <= 6; ++k) out.push_back(before | PartialCounters(k));
  return out;
}

// The writes the geometry predicts: the stage groups in write order, and the
// file offset of each group's unit.
struct Shape {
  bool merge_headers = false, merge_words = false;
  std::vector<unsigned> groups;  // stage masks, in write order
};
Shape ShapeOf(u32 page_size, u64 n) {
  const u64 m = n + 1;
  const u64 hs = pg::TransactionHeaderSlotsPerPage(page_size), ss = pg::TipSlotsPerPage(page_size);
  Shape s;
  s.merge_headers = n / hs == m / hs && (n % hs) / 2 == (m % hs) / 2;
  s.merge_words = n / ss == m / ss && (n % ss) / 128 == (m % ss) / 128;
  if (!s.merge_headers && !s.merge_words) s.groups = {W1, W2, W3, W4, W5};
  else if (s.merge_headers && !s.merge_words) s.groups = {W1, W2 | W4, W3, W5};
  else if (!s.merge_headers && s.merge_words) s.groups = {W1, W2, W4, W3 | W5};
  else s.groups = {W1, W2 | W4, W3 | W5};
  return s;
}
u64 StageOffset(const pg::TransactionChainsRead& c, unsigned stage, u64 n, u64 m) {
  const u32 ps = c.page_size;
  const u64 hs = pg::TransactionHeaderSlotsPerPage(ps), ss = pg::TipSlotsPerPage(ps);
  auto header = [&](u64 x) { return c.header_pages.at(x / hs) * ps + (512 + 256 * (x % hs)) / 512 * 512; };
  auto word = [&](u64 x) { return c.state_pages.at(x / ss) * ps + (512 + 4 * (x % ss)) / 512 * 512; };
  switch (stage) {
    case W1: return c.state_pages.at(0) * ps;
    case W2: return header(m);
    case W3: return word(m);
    case W4: return header(n);
    default: return word(n);
  }
}
unsigned FirstStage(unsigned mask) { return mask & W1 ? W1 : mask & W2 ? W2 : mask & W3 ? W3 : mask & W4 ? W4 : W5; }

// ---- Driving the writer -----------------------------------------------------------
struct Ctx {
  const Base& b;
  ImageSink sink;
  u::StandaloneUuidV7Issuer issuer;
  Ctx(const Base& base, bool forced = false)
      : b(base), sink(base.bytes, forced), issuer({base.database, Fixed(10)}, {{}, 0, 1000}) {}
  pg::TransactionWriterBinding Binding() { return b.Binding(&issuer); }
  pg::TransactionChainsRead Chains() { return pg::ReadTransactionChainsThroughSink(sink, b.Binding(nullptr)); }
};
pg::TransactionHeaderRecord Header(u64 ms, unsigned salt, bool read_only = false, bool identities = false,
                                   unsigned intent = 0) {
  pg::TransactionHeaderRecord r;
  r.transaction_uuid = V7(ms, salt);
  r.read_only = read_only;
  r.intent = static_cast<std::uint8_t>(intent);
  r.isolation_profile = 2;
  if (identities) {
    r.engine_instance_uuid = V7(ms, salt + 1000);
    r.principal_or_agent_uuid = V7(ms, salt + 2000);
    r.attachment_uuid = V7(ms, salt + 3000);
    r.session_uuid = V7(ms, salt + 4000);
    r.policy_snapshot_uuid = V7(ms, salt + 5000);
  }
  return r;
}
void BeginOk(pg::TransactionInventoryWriter& w, d::CarefulWriteSink& sink, u64 n, bool read_only = false) {
  const auto r = w.Begin(sink, Header(kT + n, static_cast<unsigned>(n), read_only));
  Check(r.ok() && r.transaction_order == n, "prelude BEGIN");
}
void CommitOk(pg::TransactionInventoryWriter& w, d::CarefulWriteSink& sink, u64 n, bool commit) {
  const auto at = Ns(kT + 500000 + n);
  const auto r = commit ? w.Commit(sink, n, at) : w.Rollback(sink, n, at);
  Check(r.ok(), "prelude completion");
}

// ---- Scenarios ----------------------------------------------------------------------
struct Scenario {
  const char* name;
  u64 n;                 // the transaction completed; 0 = computed from the geometry
  int boundary;          // 0 none, 1 last header slot of the first page, 2 last state word of the first page
  bool sibling;          // transaction 2 stays in progress
  TipStateCode final_state;
  bool read_only_m;
  bool identities;
};
// n for a scenario at this page size.
u64 ScenarioN(const Scenario& s, u32 ps) {
  if (s.boundary == 1) return pg::TransactionHeaderSlotsPerPage(ps) - 1;
  if (s.boundary == 2) return pg::TipSlotsPerPage(ps) - 1;
  return s.n;
}
const Scenario kScenarios[] = {
    {"both-merged n=2 commit", 2, 0, false, TipStateCode::committed, false, false},
    {"words-merged n=3 rollback sibling", 3, 0, true, TipStateCode::rolled_back, false, true},
    {"no-merge n=127 commit sibling", 127, 0, true, TipStateCode::committed, false, true},
    {"header-page end rollback ro-m", 0, 1, true, TipStateCode::rolled_back, true, true},
    {"state-page end commit", 0, 2, false, TipStateCode::committed, false, true},
    {"both-merged n=4 rollback ro-m intent", 4, 0, true, TipStateCode::rolled_back, true, true},
};

// Brings Next to n (n in progress), through the writer's own BEGIN/COMMIT.
void PreludeBelow(d::CarefulWriteSink& sink, pg::TransactionInventoryWriter& w, const Scenario& s, u64 n) {
  for (u64 k = 2; k < n; ++k) {
    BeginOk(w, sink, k);
    if (k == 2 && s.sibling) continue;  // stays in progress
    CommitOk(w, sink, k, k % 3 != 0);
  }
}
void Prelude(d::CarefulWriteSink& sink, pg::TransactionInventoryWriter& w, const Scenario& s, u64 n) {
  PreludeBelow(sink, w, s, n);
  BeginOk(w, sink, n);
}

// What a reopen's Restart must leave after the stage set `mask` reached the
// file (plan 36 crash table), asserted against the file itself.
void CheckCrashTable(const Call& c, unsigned mask, const Raw& restarted, const std::string& label) {
  // Dependency closure of what reached the file: the plan's invariants.
  Check(!(mask & W5) || (mask & W4), label + ": n's state byte is never final before its header");
  Check(!(mask & W3) || (mask & W2), label + ": m's word is never active before its header");
  Check(!(mask & W2) || (mask & W1), label + ": m's header is never written before Next covers it");
  const auto& wn = restarted.words.at(c.n);
  if (mask & W5) Check(wn.state == c.final_state, label + ": n is final when W5 reached the file");
  else Check(wn.state == TipStateCode::dead, label + ": n is dead before W5");
  if (mask & W1) {
    Check(restarted.counters.next_transaction == c.m + 1, label + ": Next covers m");
    Check(restarted.words.count(c.m) && restarted.words.at(c.m).state == TipStateCode::dead,
          label + ": m is dead at restart, whether unused, header-only or active");
    Check(restarted.headers.count(c.m) == ((mask & W2) ? 1u : 0u), label + ": m's header is there iff W2 reached the file");
  } else {
    Check(restarted.counters.next_transaction == c.m && !restarted.words.count(c.m) && !restarted.headers.count(c.m),
          label + ": before W1 no number was consumed");
  }
  const auto& hn = restarted.headers.at(c.n);
  Check((mask & W4) ? hn.final_state == c.final_state : hn.final_state == TipStateCode::unused,
        label + ": n's header is final iff W4 reached the file");
}

struct Tally {
  unsigned crash_cases = 0, torn_cases = 0, torn_partial = 0, restarts = 0;
};

// The file in `cx` after some prefix of the plan: compares with the oracle's
// stage set (any of `candidates`), checks the reader's rule, restarts, checks
// the crash table, and begins and commits the next number. Undone on return.
void CheckFileAfter(Ctx& cx, const Call& c, const std::vector<unsigned>& candidates, const std::string& label,
                    Tally& tally) {
  const auto mark = cx.sink.Mark();
  const auto chains = cx.Chains();
  const Raw seen = RawOf(chains);
  unsigned mask = ~0u;
  for (const auto cand : candidates)
    if (seen == Apply(c, cand)) mask = cand;
  Check(mask != ~0u, label + ": the file is the oracle's stage set");
  if (mask & ~0xFFu) mask = (mask & 0xFFu) | W1;  // a partial W1 has Next in place: the table's W1 row
  Check(Admissible(chains), label + ": the reader admits the file (stored horizons do not exceed the projection)");
  // A new process: Restart resolves the file.
  Raw expected = seen;
  const u64 marked = RestartOracle(expected);
  {
    pg::TransactionInventoryWriter w(cx.Binding());
    const auto r = w.Restart(cx.sink);
    Check(r.ok() && r.marked_dead == marked && r.unresolved.empty(), label + ": restart marks the oracle's slots dead");
    const auto after = cx.Chains();
    Check(RawOf(after) == expected && Admissible(after), label + ": the restarted file is the oracle's");
    CheckCrashTable(c, mask, RawOf(after), label);
    ++tally.restarts;
  }
  {
    pg::TransactionInventoryWriter w(cx.Binding());
    const auto r = w.Restart(cx.sink);
    Check(r.ok() && r.marked_dead == 0 && r.writes == 0, label + ": a second restart has nothing to mark");
    const u64 next = expected.counters.next_transaction;
    const auto begun = w.Begin(cx.sink, Header(kT + 100000 + next, static_cast<unsigned>(next)));
    Check(begun.ok() && begun.transaction_order == next, label + ": the next BEGIN takes the stored Next (no reuse)");
    Check(w.Commit(cx.sink, next, Ns(kT + 900000)).ok(), label + ": and commits");
    const auto fin = cx.Chains();
    Check(Admissible(fin) && EntryStates(fin).at(next) == mga::TransactionState::committed,
          label + ": the next transaction is committed and admitted");
  }
  cx.sink.Rollback(mark);
}

// Tear candidates of a write: lengths L of the new unit's prefix, none
// straddling a stage's changed bytes at the codecs' atomic granularity (256
// for header records, 4 for state words, 8 for counters), deduplicated by
// resulting bytes.
std::vector<std::vector<byte>> TearImages(const std::vector<byte>& old_unit, const std::vector<byte>& new_unit,
                                          unsigned granule) {
  std::vector<std::vector<byte>> out;
  for (std::size_t l = 0; l <= new_unit.size(); l += granule) {
    std::vector<byte> img = old_unit;
    std::copy_n(new_unit.begin(), l, img.begin());
    if (std::find(out.begin(), out.end(), img) == out.end()) out.push_back(std::move(img));
  }
  return out;
}

void Sweep(const Base& b, const Scenario& sc, Tally& tally, std::vector<std::string>& shapes_seen) {
  const u32 ps = b.size;
  const u64 n = ScenarioN(sc, ps);
  Ctx cx(b);
  pg::TransactionInventoryWriter w(cx.Binding());
  Check(w.Restart(cx.sink).ok(), "restart of the created file");
  Prelude(cx.sink, w, sc, n);
  const auto pre_chains = cx.Chains();
  const Raw pre = RawOf(pre_chains);
  Check(pre.counters.next_transaction == n + 1 && pre.words.at(n).state == TipStateCode::active, "n is in progress");
  Check(Admissible(pre_chains), "the prelude leaves an admissible file");
  const auto header_m = Header(kT + 700000, 9000 + static_cast<unsigned>(n), sc.read_only_m, sc.identities,
                               sc.read_only_m ? 3 : 0);
  const u64 now = Ns(kT + 800000);
  const Call call = MakeCall(pre, n, sc.final_state, header_m, now, b.database);
  const auto shape = ShapeOf(ps, n);
  Check(call.m == n + 1, "m is the stored Next");

  // The call, logged.
  const auto mark0 = cx.sink.Mark();
  cx.sink.ResetCounters();
  const auto result = w.CompleteAndBegin(cx.sink, n, sc.final_state, header_m, now);
  Check(result.ok() && result.transaction_order == n && result.replacement_order == call.m &&
            result.replacement_uuid == header_m.transaction_uuid && result.final_state == sc.final_state &&
            result.completed_at_utc_ns == call.at && !result.outcome_uncertain,
        std::string(sc.name) + ": CompleteAndBegin returns n, m and m's UUID");
  Check(result.merged_headers == shape.merge_headers && result.merged_state_words == shape.merge_words,
        std::string(sc.name) + ": the merges are the geometry's");
  Check(result.writes == shape.groups.size() && result.syncs == shape.groups.size(),
        std::string(sc.name) + ": one write and one sync per node on a sink that is not forced");
  const auto log = cx.sink.log();
  const u32 grown_writes = result.growth_writes;
  Check(log.size() == grown_writes + shape.groups.size(), std::string(sc.name) + ": growth writes, then the plan's");
  shapes_seen.push_back(std::string(sc.name) + ": n=" + std::to_string(n) + " " +
                        (shape.merge_headers ? "H" : "-") + (shape.merge_words ? "W" : "-") +
                        " nodes=" + std::to_string(shape.groups.size()) +
                        " growth_writes=" + std::to_string(grown_writes) + " state_pages+" +
                        std::to_string(result.grown_state_pages) + " header_pages+" +
                        std::to_string(result.grown_header_pages));
  const auto full_chains = cx.Chains();
  Check(RawOf(full_chains) == Apply(call, W1 | W2 | W3 | W4 | W5), std::string(sc.name) + ": the full run is the oracle's");
  Check(Admissible(full_chains), std::string(sc.name) + ": the reader admits the stored horizons after the full run");
  {
    const auto states = EntryStates(full_chains);
    Check(states.at(n) == (sc.final_state == TipStateCode::committed ? mga::TransactionState::committed
                                                                      : mga::TransactionState::rolled_back) &&
              states.at(call.m) == (sc.read_only_m ? mga::TransactionState::read_only_active
                                                    : mga::TransactionState::active),
          std::string(sc.name) + ": the reader sees n final and m in progress");
  }
  // The unit offsets are the geometry's.
  for (std::size_t g = 0; g < shape.groups.size(); ++g) {
    const auto& wr = log[grown_writes + g];
    Check(wr.bytes.size() == pg::kTransactionStateWriteUnitBytes &&
              wr.offset == StageOffset(full_chains, FirstStage(shape.groups[g]), n, call.m),
          std::string(sc.name) + ": write " + std::to_string(g + 1) + " is the geometry's unit");
    for (unsigned stage : {W1, W2, W3, W4, W5})
      if (shape.groups[g] & stage)
        Check(wr.offset == StageOffset(full_chains, stage, n, call.m),
              std::string(sc.name) + ": every stage of a merged node lies in its unit");
  }
  cx.sink.Rollback(mark0);
  Check(RawOf(cx.Chains()) == pre, "the undo restores the file");

  // A crash after every write of the growth plans: n is in progress, Next is
  // untouched; restart marks n dead.
  for (u32 k = 0; k < grown_writes; ++k) {
    const auto mark = cx.sink.Mark();
    for (u32 i = 0; i < k + 1; ++i) cx.sink.Apply(log[i].offset, log[i].bytes);
    CheckFileAfter(cx, call, {0}, std::string(sc.name) + " crash in growth after " + std::to_string(k + 1), tally);
    ++tally.crash_cases;
    cx.sink.Rollback(mark);
  }
  // The growth done: the plan's writes are swept from here.
  const auto grown_mark = cx.sink.Mark();
  for (u32 i = 0; i < grown_writes; ++i) cx.sink.Apply(log[i].offset, log[i].bytes);
  const auto grown_chains = cx.Chains();
  const Raw grown_pre = RawOf(grown_chains);
  Check(grown_pre == pre, std::string(sc.name) + ": growth changes no word, header or counter");
  Check(Admissible(grown_chains), std::string(sc.name) + ": the grown file is admitted");
  const std::vector<ImageSink::Write> plan(log.begin() + grown_writes, log.end());
  unsigned before = 0;  // stages durable before write k
  for (std::size_t k = 0; k <= plan.size(); ++k) {
    const std::string at = std::string(sc.name) + " crash after " + std::to_string(k);
    {
      const auto mark = cx.sink.Mark();
      for (std::size_t i = 0; i < k; ++i) cx.sink.Apply(plan[i].offset, plan[i].bytes);
      CheckFileAfter(cx, call, {before}, at, tally);
      ++tally.crash_cases;
      cx.sink.Rollback(mark);
    }
    if (k == plan.size()) break;
    // Write k+1 torn at every boundary no stage straddles.
    const unsigned group = shape.groups[k];
    const bool header_unit = (group & (W2 | W4)) != 0;
    const unsigned granule = header_unit ? 256 : (group & W1) ? 8 : 4;
    const std::size_t unit = pg::kTransactionStateWriteUnitBytes;
    std::vector<byte> old_unit(unit), new_unit = plan[k].bytes;
    {
      ImageSink& s = cx.sink;
      Check(s.image().size() >= plan[k].offset + unit, "the unit is inside the image");
      std::copy_n(s.image().begin() + plan[k].offset, unit, old_unit.begin());
    }
    const auto subsets = TearOutcomes(before, group);
    // Every stage subset of the group is an admissible outcome of a tear; the
    // tear must leave one of them and nothing else.
    for (const auto& img : TearImages(old_unit, new_unit, granule)) {
      const auto mark = cx.sink.Mark();
      for (std::size_t i = 0; i < k; ++i) cx.sink.Apply(plan[i].offset, plan[i].bytes);
      cx.sink.Apply(plan[k].offset, img);
      CheckFileAfter(cx, call, subsets, at + " + torn write " + std::to_string(k + 1), tally);
      ++tally.torn_cases;
      if (img != old_unit && img != new_unit) ++tally.torn_partial;
      cx.sink.Rollback(mark);
    }
    before |= group;
  }
  cx.sink.Rollback(grown_mark);
}

void ShapesAndSweeps(const Base& b) {
  Tally tally;
  std::vector<std::string> shapes_seen;
  std::set<std::string> variants;
  for (const auto& sc : kScenarios) {
    const auto t0 = std::chrono::steady_clock::now();
    const unsigned before = checks;
    const auto n = ScenarioN(sc, b.size);
    // The state-page end needs ~n transactions: only where that is bounded.
    if (sc.boundary == 2 && n > 4000) {
      std::cout << "sweep page_size=" << b.size << " scenario '" << sc.name << "' skipped: n=" << n
                << " transactions of prelude (header-page end covers chain growth)\n";
      continue;
    }
    Sweep(b, sc, tally, shapes_seen);
    const auto ShapeKey = ShapeOf(b.size, n);
    variants.insert(std::string(ShapeKey.merge_headers ? "H" : "-") + (ShapeKey.merge_words ? "W" : "-"));
    std::cout << "sweep page_size=" << b.size << " " << shapes_seen.back() << " checks=" << checks - before << " ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()
              << '\n';
  }
  Check(variants.count("HW") && variants.count("-W") && variants.count("--"), "the both, words-only and no-merge plans all ran");
  Check(!variants.count("H-"), "the headers-only merge never arises at a canonical page size");
  Check(tally.torn_partial > 0, "tears left units neither old nor new");
  std::cout << "sweep page_size=" << b.size << " crash_cases=" << tally.crash_cases << " torn_cases=" << tally.torn_cases
            << " torn_partial_units=" << tally.torn_partial << " restarts=" << tally.restarts << " merge_variants=";
  for (const auto& v : variants) std::cout << v << ' ';
  std::cout << '\n';
}

// The headers-only merge needs an even n at the end of a 128-word unit, and
// the end of a state page: both ends are odd at every canonical page size, so
// no scenario reaches it. Proven over the first million numbers.
void HeadersOnlyIsUnreachable(const Base& b) {
  const u32 ps = b.size;
  const u64 hs = pg::TransactionHeaderSlotsPerPage(ps), ss = pg::TipSlotsPerPage(ps);
  Check(hs % 2 == 0 && ss % 128 == 0, "even header slots, whole 128-word units per state page");
  for (u64 n = 1; n < 1'000'000; ++n) {
    const auto s = ShapeOf(ps, n);
    Check(!(s.merge_headers && !s.merge_words), "no n merges headers without merging words");
  }
}

// W1 alone with the horizons computed treating n as FINAL (the first reading
// of plan 36) is a file the reader refuses: the stored OAT would exceed the
// projection while n is still active. Why CompleteAndBegin computes them as
// BEGIN does.
void FinalHorizonsAreInadmissible(const Base& b) {
  Ctx cx(b);
  pg::TransactionInventoryWriter w(cx.Binding());
  Check(w.Restart(cx.sink).ok(), "restart");
  BeginOk(w, cx.sink, 2);
  auto chains = cx.Chains();
  Check(Admissible(chains), "the file with n = 2 in progress is admitted");
  // What a first writer step W1 would store if the horizons were computed with
  // n = 2 treated as final: Next = 4 and OIT = OAT = OST = 3 = m.
  auto stored = chains.counters;
  stored.next_transaction = 4;
  stored.horizons = {3, 3, 3, 3, 4, 4};
  auto first = std::span<byte>(chains.state_images).first(chains.page_size);
  const auto edit = pg::SetTransactionStateCounters(first, stored);
  Check(edit.ok() && edit.changed, "the counters of W1 with n treated as final");
  Check(RawOf(chains).words.at(2).state == TipStateCode::active, "n is still in progress in that file (W5 not yet written)");
  Check(!Admissible(chains),
        "a stored OAT above the in-progress n is refused by the reader: horizons are computed with n still active");
}


// ---- Rules ----------------------------------------------------------------------------
// A restarted writer over the base with transaction 2 in progress.
struct Fresh {
  Ctx cx;
  std::size_t mark;
  std::unique_ptr<pg::TransactionInventoryWriter> w;
  explicit Fresh(const Base& b) : cx(b), mark(cx.sink.Mark()) { Reset(); }
  void Reset() {
    cx.sink.NoFault();
    cx.sink.Rollback(mark);
    w = std::make_unique<pg::TransactionInventoryWriter>(cx.Binding());
    Check(w->Restart(cx.sink).ok(), "restart of the created file");
    BeginOk(*w, cx.sink, 2);
    cx.sink.ResetCounters();
  }
};

void Rules(const Base& b) {
  const u32 ps = b.size;
  const auto good = Header(kT + 2500, 2500);
  const u64 clock = Ns(kT + 9000);
  {
    Ctx cx(b);
    pg::TransactionInventoryWriter w(cx.Binding());
    const auto before = cx.sink.image();
    const auto r = w.CompleteAndBegin(cx.sink, 1, TipStateCode::committed, good, Ns(kT));
    Check(r.error == WE::restart_required && r.writes == 0 && r.syncs == 0 && cx.sink.image() == before && cx.sink.calls() == 0,
          "refused before the restart step, no I/O");
  }
  Fresh f(b);
  auto& cx = f.cx;
  auto& w = *f.w;
  const auto before = cx.sink.image();
  auto refused = [&](WE want, const pg::TransactionCompleteAndBeginResult& r, std::string_view why,
                     std::source_location at = std::source_location::current()) {
    Check(r.error == want && r.writes == 0 && r.syncs == 0 && !w.failed() && cx.sink.writes() == 0 && cx.sink.syncs() == 0 &&
              cx.sink.image() == before,
          std::string(why) + ": refused " + pg::TransactionWriterErrorName(want) + ", file byte-identical", at);
  };
  for (const auto state : {TipStateCode::unused, TipStateCode::starting, TipStateCode::active, TipStateCode::read_only_active,
                           TipStateCode::prepared, TipStateCode::committing, TipStateCode::rolling_back, TipStateCode::dead,
                           TipStateCode::limbo, TipStateCode::archived})
    refused(WE::invalid_request, w.CompleteAndBegin(cx.sink, 2, state, good, clock),
            std::string("final state ") + pg::TipStateCodeName(state));
  refused(WE::invalid_request, w.CompleteAndBegin(cx.sink, 0, TipStateCode::committed, good, clock), "order 0");
  refused(WE::invalid_request, w.CompleteAndBegin(cx.sink, 2, TipStateCode::committed, good, 0), "a zero clock");
  auto bad = good;
  bad.transaction_uuid = Fixed(77);
  bad.transaction_uuid.bytes[6] = 0x40;
  {
    const auto r = w.CompleteAndBegin(cx.sink, 2, TipStateCode::committed, bad, clock);
    refused(WE::invalid_header, r, "an id that is not a UUIDv7");
    Check(r.header_error != pg::TransactionHeaderError::none && r.replacement_order == 3,
          "the codec's reason and m are reported");
  }
  bad = good;
  bad.session_uuid = Fixed(5);
  bad.session_uuid.bytes[6] = 0x40;
  refused(WE::invalid_header, w.CompleteAndBegin(cx.sink, 2, TipStateCode::committed, bad, clock),
          "an optional identity that is not a UUIDv7");
  bad = good;
  bad.database_uuid = Fixed(99);
  refused(WE::invalid_request, w.CompleteAndBegin(cx.sink, 2, TipStateCode::committed, bad, clock), "another database");
  bad = good;
  bad.authority = pg::TipAuthorityClass::cluster;
  refused(WE::invalid_request, w.CompleteAndBegin(cx.sink, 2, TipStateCode::committed, bad, clock), "cluster authority");
  bad = good;
  bad.completed_at_utc_ns = 1;
  refused(WE::invalid_request, w.CompleteAndBegin(cx.sink, 2, TipStateCode::committed, bad, clock), "a completion in m");
  bad = good;
  bad.intent_generation = 1;
  refused(WE::invalid_request, w.CompleteAndBegin(cx.sink, 2, TipStateCode::committed, bad, clock), "an intent generation in m");
  bad = good;
  bad.transaction_order = 7;
  refused(WE::invalid_request, w.CompleteAndBegin(cx.sink, 2, TipStateCode::committed, bad, clock), "an order in m");
  refused(WE::unknown_transaction, w.CompleteAndBegin(cx.sink, 3, TipStateCode::committed, good, clock), "n = Next");
  refused(WE::unknown_transaction, w.CompleteAndBegin(cx.sink, 99, TipStateCode::rolled_back, good, clock), "n beyond Next");
  refused(WE::invalid_state, w.CompleteAndBegin(cx.sink, 1, TipStateCode::committed, good, clock), "the committed creator");
  // A read-only database: refused whatever the request.
  {
    pg::TransactionInventoryWriter ro(b.Binding(&cx.issuer, true));
    const auto r = ro.CompleteAndBegin(cx.sink, 2, TipStateCode::committed, good, clock);
    Check(r.error == WE::read_only_database && r.writes == 0 && cx.sink.image() == before, "a read-only database is refused");
  }
  // A stored OIT above what the file projects (2 is in progress): horizon regression.
  {
    auto chains = cx.Chains();
    auto raised = chains.counters;
    raised.horizons.oldest_interesting = 3;
    auto first = std::span<byte>(chains.state_images).first(ps);
    const auto edit = pg::SetTransactionStateCounters(first, raised);
    Check(edit.ok() && edit.changed, "raise the stored OIT");
    cx.sink.Apply(chains.state_pages[0] * ps + edit.unit_offset, first.subspan(edit.unit_offset, pg::kTransactionStateWriteUnitBytes));
    const auto raised_image = cx.sink.image();
    cx.sink.ResetCounters();
    const auto r = w.CompleteAndBegin(cx.sink, 2, TipStateCode::committed, good, clock);
    Check(r.error == WE::horizon_regression && r.writes == 0 && r.syncs == 0 && !w.failed() && cx.sink.writes() == 0 &&
              cx.sink.image() == raised_image,
          "a computed horizon below the stored one is refused with no write");
  }
  // After a real call: the old n is final (invalid_state), the replacement is in
  // progress and completes in turn; plain Begin / Commit / Rollback still work.
  {
    f.Reset();
    auto& w2 = *f.w;
    const auto r = w2.CompleteAndBegin(cx.sink, 2, TipStateCode::committed, Header(kT + 3000, 3), clock);
    Check(r.ok() && r.replacement_order == 3, "CompleteAndBegin 2 -> 3");
    const auto img = cx.sink.image();
    Check(w2.CompleteAndBegin(cx.sink, 2, TipStateCode::rolled_back, Header(kT + 4000, 4), clock + 500).error ==
                  WE::invalid_state &&
              cx.sink.image() == img,
          "a completed transaction cannot complete again, no write");
    const auto r2 = w2.CompleteAndBegin(cx.sink, 3, TipStateCode::rolled_back, Header(kT + 4000, 4), clock + 500);
    Check(r2.ok() && r2.replacement_order == 4, "the replacement completes and is replaced in turn");
    BeginOk(w2, cx.sink, 5);
    CommitOk(w2, cx.sink, 4, true);
    CommitOk(w2, cx.sink, 5, false);
    Check(Admissible(cx.Chains()), "the file after mixed calls is admitted");
  }
  // A dead n and a dead m (crash after W1, then restart): invalid_state, no write.
  {
    f.Reset();
    const auto mark = cx.sink.Mark();
    const auto r = f.w->CompleteAndBegin(cx.sink, 2, TipStateCode::committed, Header(kT + 3000, 3), clock);
    Check(r.ok() && !cx.sink.log().empty(), "the call");
    const auto w1 = cx.sink.log().front();
    cx.sink.Rollback(mark);
    cx.sink.Apply(w1.offset, w1.bytes);
    pg::TransactionInventoryWriter again(cx.Binding());
    Check(again.Restart(cx.sink).ok(), "restart marks n and m dead");
    const auto img = cx.sink.image();
    Check(again.CompleteAndBegin(cx.sink, 2, TipStateCode::committed, Header(kT + 5000, 5), clock).error == WE::invalid_state &&
              again.CompleteAndBegin(cx.sink, 3, TipStateCode::committed, Header(kT + 5000, 5), clock).error == WE::invalid_state &&
              cx.sink.image() == img,
          "a dead n is refused invalid_state, no write");
  }
  // chain_full without a growth binding: n is the last header slot of its page.
  {
    Ctx c2(b);
    pg::TransactionInventoryWriter w2(b.Binding(nullptr));
    Check(w2.Restart(c2.sink).ok(), "restart");
    const u64 last = pg::TransactionHeaderSlotsPerPage(ps) - 1;
    for (u64 k = 2; k < last; ++k) {
      BeginOk(w2, c2.sink, k);
      CommitOk(w2, c2.sink, k, true);
    }
    BeginOk(w2, c2.sink, last);
    const auto img = c2.sink.image();
    c2.sink.ResetCounters();
    const auto r = w2.CompleteAndBegin(c2.sink, last, TipStateCode::committed, Header(kT + 6000, 6), clock);
    Check(r.error == WE::chain_full && r.transaction_order == last && r.writes == 0 && r.syncs == 0 && !w2.failed() &&
              c2.sink.writes() == 0 && c2.sink.image() == img,
          "m past the last formatted header slot with no growth binding: chain_full, nothing written, n untouched");
    // The same n with a growth binding grows the header chain first, then completes.
    pg::TransactionInventoryWriter w3(c2.Binding());
    Check(w3.Restart(c2.sink).ok(), "restart marks the in-progress n dead (a new process)");
  }
  // A failed read writes nothing and the writer lives; a failed first write
  // fails the writer, which then refuses everything with no I/O.
  {
    f.Reset();
    cx.sink.FailCall(1);
    auto r = f.w->CompleteAndBegin(cx.sink, 2, TipStateCode::committed, Header(kT + 3000, 3), clock);
    Check(r.error == WE::io_failure && !f.w->failed() && cx.sink.writes() == 0, "a failed read writes nothing, the writer lives");
    cx.sink.NoFault();
    cx.sink.ResetCounters();
    Check(f.w->CompleteAndBegin(cx.sink, 2, TipStateCode::committed, Header(kT + 3000, 3), clock).ok() &&
              !cx.sink.write_calls().empty(),
          "the same call succeeds after a read fault");
    const u32 first_write_call = cx.sink.write_calls().front();
    f.Reset();
    cx.sink.FailCall(first_write_call);
    r = f.w->CompleteAndBegin(cx.sink, 2, TipStateCode::committed, Header(kT + 3000, 3), clock);
    Check(r.error == WE::write_failed && f.w->failed() && !r.outcome_uncertain && cx.sink.writes() == 0,
          "a failed first write fails the writer; n's outcome is not uncertain");
    cx.sink.NoFault();
    cx.sink.ResetCounters();
    const auto img = cx.sink.image();
    Check(f.w->CompleteAndBegin(cx.sink, 2, TipStateCode::committed, Header(kT + 3000, 3), clock).error == WE::writer_failed &&
              f.w->Begin(cx.sink, Header(kT + 3500, 35)).error == WE::writer_failed &&
              f.w->Commit(cx.sink, 2, clock).error == WE::writer_failed &&
              f.w->Rollback(cx.sink, 2, clock).error == WE::writer_failed && f.w->Restart(cx.sink).error == WE::writer_failed,
          "a failed writer refuses everything, CompleteAndBegin included");
    Check(cx.sink.calls() == 0 && cx.sink.image() == img, "and does no I/O at all");
  }
  std::cout << "rules page_size=" << ps << " ok\n";
}

// ---- Faults ---------------------------------------------------------------------------
void Faults(const Base& b) {
  unsigned points = 0, read_faults = 0, write_faults = 0, sync_faults = 0, uncertain = 0, certain = 0;
  for (const auto& sc : kScenarios) {
    const u64 n = ScenarioN(sc, b.size);
    if (sc.boundary == 2) continue;  // the state-page end is covered by the sweep
    Ctx cx(b);
    {
      pg::TransactionInventoryWriter w(cx.Binding());
      Check(w.Restart(cx.sink).ok(), "restart");
      PreludeBelow(cx.sink, w, sc, n);
    }
    const auto mark = cx.sink.Mark();  // Next = n, no n yet
    const auto header_m = Header(kT + 700000, 9000 + static_cast<unsigned>(n), sc.read_only_m, sc.identities, sc.read_only_m ? 3 : 0);
    const u64 now = Ns(kT + 800000);
    const auto shape = ShapeOf(b.size, n);
    Tally tally;
    for (u32 at = 1;; ++at) {
      const std::string label = std::string("fault ") + sc.name + " at call " + std::to_string(at);
      cx.sink.NoFault();
      cx.sink.Rollback(mark);
      // A new process over the file: restart (a sibling is marked dead), then n begins.
      pg::TransactionInventoryWriter victim(cx.Binding());
      Check(victim.Restart(cx.sink).ok(), label + ": restart");
      BeginOk(victim, cx.sink, n);
      const Raw pre = RawOf(cx.Chains());
      const Call call = MakeCall(pre, n, sc.final_state, header_m, now, b.database);
      cx.sink.ResetCounters();
      cx.sink.FailCall(at);
      auto r = victim.CompleteAndBegin(cx.sink, n, sc.final_state, header_m, now);
      if (!cx.sink.reached()) {
        Check(r.ok(), label + ": no fault reached, the call succeeded");
        break;
      }
      ++points;
      const auto kind = *cx.sink.failed_kind();
      (kind == d::BoundedIoOperation::read ? read_faults : kind == d::BoundedIoOperation::write ? write_faults : sync_faults)++;
      // A read fault before the first write changes nothing; a read of a
      // node's preimage after earlier writes fails the writer like a failed write.
      if (r.error == WE::io_failure) {
        Check(kind == d::BoundedIoOperation::read && r.error == WE::io_failure && !victim.failed() && r.writes == 0 && cx.sink.writes() == r.growth_writes,
              label + ": a failed read writes nothing but the growth plans already completed");
        cx.sink.NoFault();
        r = victim.CompleteAndBegin(cx.sink, n, sc.final_state, header_m, now);
        Check(r.ok() && !r.outcome_uncertain, label + ": and the same call succeeds again");
        continue;
      }
      Check(r.error == WE::write_failed && victim.failed(), label + ": a failed write or sync fails the writer");
      const u32 performed = cx.sink.writes();
      cx.sink.NoFault();
      cx.sink.ResetCounters();
      const auto image = cx.sink.image();
      Check(victim.CompleteAndBegin(cx.sink, n, sc.final_state, header_m, now).error == WE::writer_failed &&
                victim.Commit(cx.sink, n, now).error == WE::writer_failed && cx.sink.calls() == 0 && cx.sink.image() == image,
            label + ": a failed writer refuses everything with no I/O");
      // The file is the oracle's stage prefix of the writes performed (growth writes first).
      const u32 growth = r.growth_writes;
      unsigned mask = 0;
      for (u32 g = 0; g + growth < performed && g < shape.groups.size(); ++g) mask |= shape.groups[g];
      const auto chains = cx.Chains();
      // n's state byte is in the plan's last node (every shape): the outcome is
      // uncertain exactly when that write was attempted and failed, or the final
      // sync after it failed.
      const u32 plan_writes = static_cast<u32>(shape.groups.size());
      const bool expect_uncertain =
          (kind == d::BoundedIoOperation::write && performed == growth + plan_writes - 1) ||
          (kind == d::BoundedIoOperation::sync && performed == growth + plan_writes);
      Check(r.outcome_uncertain == expect_uncertain, label + ": outcome_uncertain is exactly 'W5 attempted'");
      if (r.outcome_uncertain) {
        ++uncertain;
      } else {
        ++certain;
        Check(!(mask & W5) && RawOf(chains).words.at(n).state != sc.final_state,
              label + ": a completion not marked uncertain never reached n's state byte");
      }
      CheckFileAfter(cx, call, {mask}, label, tally);
    }
    cx.sink.NoFault();
    cx.sink.Rollback(mark);
    (void)shape;
  }
  Check(points > 10 && read_faults > 0 && write_faults > 0 && sync_faults > 0 && uncertain > 0 && certain > 0,
        "every kind of sink call was failed, and some completions were left uncertain, some not");
  std::cout << "faults page_size=" << b.size << " points=" << points << " reads=" << read_faults << " writes=" << write_faults
            << " syncs=" << sync_faults << " uncertain=" << uncertain << " certain=" << certain << '\n';
}

// ---- Real file: fork + _exit crash sinks, the production lease -------------------------
struct Seen {
  pg::TipCounters counters;
  bool admissible = false;
};
Seen LeaseRead(const Base& b, const std::string& path) {
  d::FileDevice device;
  Check(device.Open(path, d::FileOpenMode::open_existing_read_only, d::DatabaseFileOwnership::native_data_file).ok(),
        "independent read-only open");
  Seen out;
  {
    const std::vector<d::NativeFilespaceDevice> files{{b.primary, b.profile, &device}};
    static std::vector<byte> backing(64u << 20);
    auto fixed = db::AcquireNativeFixedRootBackedLease(b.database, files, b.primary, {256ull * b.size, backing.size()}, backing);
    Check(fixed.ok(), "the production fixed-root lease reads the file");
    const auto& lease = *fixed.lease;
    out.counters = lease.counters();
    const auto& inventory = lease.inventory();
    const pg::NativeTransactionInventoryConstView view{inventory.next_local_transaction_id, inventory.next_commit_sequence,
                                                      inventory.entries};
    mga::LocalTransactionHorizons projected;
    Check(!mga::detail::ProjectValidatedLocalHorizons(view, {}, projected), "the inventory projects horizons");
    out.admissible = pg::StoredTransactionHorizonsAdmissible(out.counters, inventory.next_local_transaction_id, projected);
    fixed.lease.Reset();
  }
  Check(device.Close().ok(), "close the reader");
  return out;
}
[[noreturn]] void Child(const Base& b, const std::string& path, u32 limit, bool torn, const Scenario& sc, const Call& call) {
  try {
    d::FileDevice device;
    if (!device.Open(path, d::FileOpenMode::open_existing, d::DatabaseFileOwnership::native_data_file).ok()) _exit(kChildFailed);
    int code = kChildCompleted;
    {
      d::FileDeviceCarefulWriteSink file(device);
      u::StandaloneUuidV7Issuer issuer({b.database, Fixed(10)}, {{}, 0, 1000});
      pg::TransactionInventoryWriter w(b.Binding(&issuer));
      if (!w.Restart(file).ok()) code = kChildFailed;
      // The prelude goes through the plain file sink: only CompleteAndBegin may crash.
      try {
        Prelude(file, w, sc, call.n);
      } catch (...) {
        code = kChildFailed;
      }
      std::optional<scratchbird::tests::CrashAfterWritesSink> crash;
      std::optional<scratchbird::tests::TornWriteSink> tear;
      d::CarefulWriteSink* sink = torn ? static_cast<d::CarefulWriteSink*>(&tear.emplace(file, limit, kPlannedCrash))
                                       : static_cast<d::CarefulWriteSink*>(&crash.emplace(file, limit, kPlannedCrash));
      if (code == kChildCompleted) {
        const auto r = w.CompleteAndBegin(*sink, call.n, call.final_state, call.header_m, call.now);
        if (!r.ok()) code = kChildFailed;
      }
    }
    if (!device.Close().ok()) code = kChildFailed;
    _exit(code);
  } catch (...) {
    _exit(kChildException);
  }
}

void Real(const Base& b) {
  unsigned cases = 0;
  for (int si = 0; si < 2; ++si) {
    const auto& sc = kScenarios[si];
    const u64 n = ScenarioN(sc, b.size);
    Ctx rc(b);
    pg::TransactionInventoryWriter rw(rc.Binding());
    Check(rw.Restart(rc.sink).ok(), "restart");
    Prelude(rc.sink, rw, sc, n);
    const Raw pre = RawOf(rc.Chains());
    const auto header_m = Header(kT + 700000, 9000 + static_cast<unsigned>(n), sc.read_only_m, sc.identities, sc.read_only_m ? 3 : 0);
    const Call call = MakeCall(pre, n, sc.final_state, header_m, Ns(kT + 800000), b.database);
    const auto shape = ShapeOf(b.size, n);
    for (const bool torn : {false, true}) {
      for (u32 k = torn ? 1 : 0; k <= shape.groups.size() + (torn ? 0 : 1); ++k) {
        const bool full = !torn && k == shape.groups.size() + 1;
        const std::string label = std::string("real ") + sc.name + (torn ? " torn write " : " crash after ") + std::to_string(k) +
                                  (full ? " (full run)" : "");
        const auto path = (b.dir.path() / "real.db").string();
        WriteAll(path, b.bytes);
        std::cout.flush();
        std::cerr.flush();
        const pid_t child = fork();
        Check(child >= 0, label + ": fork");
        if (child == 0) Child(b, path, full ? 1000 : k, torn, sc, call);
        int status = 0;
        Check(waitpid(child, &status, 0) == child && WIFEXITED(status), label + ": the child exits");
        Check(WEXITSTATUS(status) == (full ? kChildCompleted : kPlannedCrash),
              label + ": child exit code " + std::to_string(WEXITSTATUS(status)));
        // The stage sets this can leave.
        unsigned before = 0;
        for (std::size_t g = 0; g < shape.groups.size() && g + 1 < (torn ? k : k + 1) && !full; ++g) before |= shape.groups[g];
        if (full) before = W1 | W2 | W3 | W4 | W5;
        const auto candidates = torn ? TearOutcomes(before, shape.groups[k - 1]) : std::vector<unsigned>{before};
        pg::TransactionChainsRead chains;
        {
          d::FileDevice device;
          Check(device.Open(path, d::FileOpenMode::open_existing_read_only, d::DatabaseFileOwnership::native_data_file).ok(),
                label + ": open");
          d::FileDeviceCarefulWriteSink sink(device);
          chains = pg::ReadTransactionChainsThroughSink(sink, b.Binding(nullptr));
          Check(device.Close().ok(), label + ": close");
        }
        const Raw seen = RawOf(chains);
        unsigned mask = ~0u;
        for (const auto cand : candidates)
          if (seen == Apply(call, cand)) mask = cand;
        Check(mask != ~0u, label + ": the file is the oracle's stage set");
        const auto lease = LeaseRead(b, path);
        Check(lease.admissible && lease.counters == seen.counters, label + ": the production lease admits the file");
        // Restart in a new process (here: a fresh device and writer).
        Raw expected = seen;
        const u64 marked = RestartOracle(expected);
        {
          d::FileDevice device;
          Check(device.Open(path, d::FileOpenMode::open_existing, d::DatabaseFileOwnership::native_data_file).ok(), label + ": reopen");
          {
            d::FileDeviceCarefulWriteSink sink(device);
            u::StandaloneUuidV7Issuer issuer({b.database, Fixed(10)}, {{}, 0, 1000});
            pg::TransactionInventoryWriter w(b.Binding(&issuer));
            const auto r = w.Restart(sink);
            Check(r.ok() && r.marked_dead == marked, label + ": restart marks the oracle's slots dead");
            const auto after = pg::ReadTransactionChainsThroughSink(sink, b.Binding(nullptr));
            Check(RawOf(after) == expected && Admissible(after), label + ": the restarted file is the oracle's");
            CheckCrashTable(call, (mask & 0xFFu) | ((mask & ~0xFFu) ? W1 : 0u), RawOf(after), label);
          }
          Check(device.Close().ok(), label + ": close");
        }
        const auto lease2 = LeaseRead(b, path);
        Check(lease2.admissible, label + ": the lease admits the restarted file");
        fs::remove(path);
        ++cases;
      }
    }
  }
  std::cout << "real page_size=" << b.size << " cases=" << cases << '\n';
}

// ---- Measure: writes and syncs on a real device (the S5 baseline) ----------------------
void Mark(const std::string& what) {
  const std::string line = "MARK " + what + "\n";
  (void)!::write(2, line.data(), line.size());
}
void Measure(const Base& b) {
  for (const auto policy : {d::CarefulWriteBarrierPolicy::trust_forced_writes, d::CarefulWriteBarrierPolicy::sync_every_dependency}) {
    const char* pname = policy == d::CarefulWriteBarrierPolicy::trust_forced_writes ? "trust_forced_writes" : "sync_every_dependency";
    const auto path = (b.dir.path() / "measure.db").string();
    WriteAll(path, b.bytes);
    d::FileDevice device;
    Check(device.Open(path, d::FileOpenMode::open_existing, d::DatabaseFileOwnership::native_data_file).ok(), "open");
    {
      d::FileDeviceCarefulWriteSink sink(device);
      u::StandaloneUuidV7Issuer issuer({b.database, Fixed(10)}, {{}, 0, 1000});
      auto binding = b.Binding(&issuer);
      binding.barrier_policy = policy;
      pg::TransactionInventoryWriter w(binding);
      Check(w.Restart(sink).ok(), "restart");
      const bool forced = device.forced_ordered_writes();
      auto line = [&](const char* op, u32 writes, u32 syncs, const char* extra = "") {
        std::cout << "measure page_size=" << b.size << " forced_ordered=" << forced << " policy=" << pname << " op=" << op
                  << " writes=" << writes << " syncs=" << syncs << extra << '\n';
      };
      // BEGIN + COMMIT, the pair today.
      Mark(std::string("begin ") + pname);
      auto begin2 = w.Begin(sink, Header(kT + 2, 2));
      Check(begin2.ok(), "begin 2");
      Mark(std::string("idle ") + pname);
      line("begin", begin2.writes, begin2.syncs);
      Mark(std::string("commit ") + pname);
      auto commit2 = w.Commit(sink, 2, Ns(kT + 10000));
      Check(commit2.ok(), "commit 2");
      Mark(std::string("idle ") + pname);
      line("commit", commit2.writes, commit2.syncs);
      line("begin+commit", begin2.writes + commit2.writes, begin2.syncs + commit2.syncs);
      // CompleteAndBegin from an odd n (words merge, headers do not) and an even n (both merge).
      Check(w.Begin(sink, Header(kT + 3, 3)).ok(), "begin 3");
      Mark(std::string("complete_and_begin_odd ") + pname);
      auto odd = w.CompleteAndBegin(sink, 3, TipStateCode::committed, Header(kT + 4, 4), Ns(kT + 11000));
      Mark(std::string("idle ") + pname);
      Check(odd.ok() && odd.replacement_order == 4, "complete 3 -> 4");
      line("complete_and_begin(n=odd)", odd.writes, odd.syncs, odd.merged_headers ? " merged=headers" : odd.merged_state_words ? " merged=words" : " merged=none");
      Mark(std::string("complete_and_begin_even ") + pname);
      auto even = w.CompleteAndBegin(sink, 4, TipStateCode::rolled_back, Header(kT + 5, 5), Ns(kT + 12000));
      Mark(std::string("idle ") + pname);
      Check(even.ok() && even.replacement_order == 5, "complete 4 -> 5");
      line("complete_and_begin(n=even)", even.writes, even.syncs, " merged=both");
      // The two-step replacement the plan replaces: COMMIT then BEGIN.
      Mark(std::string("commit_then_begin ") + pname);
      auto c5 = w.Commit(sink, 5, Ns(kT + 13000));
      auto b6 = w.Begin(sink, Header(kT + 6, 6));
      Mark(std::string("idle ") + pname);
      Check(c5.ok() && b6.ok(), "commit 5, begin 6");
      line("commit+begin", c5.writes + b6.writes, c5.syncs + b6.syncs);
      Mark(std::string("end ") + pname);
      Check(even.merged_headers && even.merged_state_words && odd.merged_state_words && !odd.merged_headers, "the merges are as predicted");
    }
    Check(device.Close().ok(), "close");
    fs::remove(path);
  }
}
}  // namespace

int main(int argc, char** argv) {
  unsigned profile = 0;
  std::string only;
  try {
    for (int i = 1; i + 1 < argc; i += 2) {
      if (std::string(argv[i]) == "--profile") profile = static_cast<unsigned>(std::stoul(argv[i + 1]));
      else if (std::string(argv[i]) == "--only") only = argv[i + 1];
    }
    Check(profile < d::kCanonicalFilespacePageProfiles.size(), "a canonical page profile");
    const Base b(profile);
    auto run = [&](const char* name, const std::function<void()>& fn) {
      if (!only.empty() && only != name) return;
      const auto t0 = std::chrono::steady_clock::now();
      fn();
      std::cout << "section " << name << " page_size=" << b.size << " ms="
                << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() << '\n';
    };
    run("rules", [&] { Rules(b); });
    run("shapes", [&] {
      HeadersOnlyIsUnreachable(b);
      FinalHorizonsAreInadmissible(b);
    });
    run("sweep", [&] { ShapesAndSweeps(b); });
    run("faults", [&] { Faults(b); });
    run("real", [&] { Real(b); });
    run("measure", [&] { Measure(b); });
    std::cout << "PASS native transaction complete-and-begin page_size=" << b.size << " checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL native transaction complete-and-begin profile=" << profile << ": " << e.what() << " checks=" << checks << '\n';
    return 1;
  }
}
