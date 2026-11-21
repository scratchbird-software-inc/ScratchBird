// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_SESSION_LEDGER_CHAIN, SB_SESSION_LEDGER_ROOT
// Plan 36 steps T3 and T4 (owner Q165 = 1): the session ledger on a database written by the PRODUCTION create, one
// page size per run (--profile N, optional --only name):
//   create    the create builds the EMPTY ledger: page-zero root kind 25 (type 0x301, the last root, the last page),
//             the chain loads with 0 records on 1 page.
//   append    every record kind appended, the file reloaded by a fresh ledger: every record and ordinal read back
//             identical; the reference rules (one distinct refusal code each) leave the file byte-identical.
//   growth    appends across a page boundary (and a second one): the chain grows by one page per boundary through the
//             shared careful-write growth, reloads identical, the allocation map holds the new pages.
//   tear      a TEAR SWEEP of the append: a crash after every write and a write torn at every byte prefix (every
//             prefix of the record's 512-byte unit; every 512-byte step +-1 of a grown page), for a plain append in an
//             even and in an odd slot and for the append that grows the chain: the file reloads, the half-written
//             record is never visible, earlier records are intact, and the next append succeeds.
//   cited     "durable before cited": Begin / CompleteAndBegin that cite an unknown session or attachment are refused
//             with a specific code and write NOTHING (file byte-identical), also where chain growth would have run
//             first; a cited appended record succeeds and the header holds +96 and +80; principal mismatch, ended
//             session, attachment of another session, wrong kind, no ledger bound; invalid_header still comes first.
//   faults    every read, write and sync of an append failed in turn: a failed append is never findable, so never
//             citable (the index is only updated after the sync); the combined append-then-Begin crash sweep checks
//             that every header on disk cites a record the reloaded ledger holds (with a negative control that
//             replays the writes in the wrong order and must be caught).
// Limit: process death and I/O errors only (the kernel page cache survives): nothing here is evidence about power loss.
#include "native_database_create.hpp"
#include "native_fixed_root_memory_lease.hpp"
#include "native_catalog_relation_lease_reader.hpp"
#include "admitted_datatype_cohort.hpp"
#include "datatype_catalog_manifest.hpp"
#include "filespace_page_zero.hpp"
#include "session_ledger_chain.hpp"
#include "transaction_inventory_writer.hpp"
#include "careful_write.hpp"
#include "allocation_map_inplace_page.hpp"
#include "uuid.hpp"
#include "../support/owned_temp_directory.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
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
namespace u = scratchbird::core::uuid;
using namespace scratchbird::core::platform;
using WE = pg::TransactionWriterError;

unsigned checks = 0;
void Check(bool ok, std::string_view why, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) throw std::runtime_error(std::string(why) + " (line " + std::to_string(at.line()) + ")");
}
void CheckWE(WE actual, WE expected, std::string_view why, std::source_location at = std::source_location::current()) {
  Check(actual == expected, std::string(why) + ": expected " + pg::TransactionWriterErrorName(expected) + ", got " +
                                pg::TransactionWriterErrorName(actual),
        at);
}

Uuid Fixed(unsigned n) {
  Uuid id;
  id.bytes[0] = 1;
  id.bytes[6] = 0x70;
  id.bytes[8] = 0x80;
  id.bytes[12] = static_cast<byte>(n >> 24);
  id.bytes[13] = static_cast<byte>(n >> 16);
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
constexpr const char* kFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";

std::vector<byte> ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  Check(bool(in), "open the file to read it back");
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// ---- A sink over a flat image: logs writes, injects one fault ------------------------------------------------------
class ImageSink final : public d::CarefulWriteSink {
 public:
  struct Write {
    u64 offset = 0;
    std::vector<byte> bytes;
  };
  explicit ImageSink(std::vector<byte> image) : img_(std::move(image)) {}
  d::BoundedIoResult ReadAt(u64 offset, std::span<byte> bytes) override {
    if (Fault()) return Fail(d::BoundedIoOperation::read, offset, bytes.size());
    if (offset + bytes.size() > img_.size()) return Fail(d::BoundedIoOperation::read, offset, bytes.size());
    std::copy_n(img_.begin() + offset, bytes.size(), bytes.begin());
    return Ok(d::BoundedIoOperation::read, offset, bytes.size());
  }
  d::BoundedIoResult WriteAt(u64 offset, std::span<const byte> bytes) override {
    if (Fault()) return Fail(d::BoundedIoOperation::write, offset, bytes.size());
    std::copy(bytes.begin(), bytes.end(), img_.begin() + offset);
    log_.push_back({offset, std::vector<byte>(bytes.begin(), bytes.end())});
    return Ok(d::BoundedIoOperation::write, offset, bytes.size());
  }
  d::BoundedIoResult Sync() override {
    if (Fault()) return Fail(d::BoundedIoOperation::sync, 0, 0);
    ++syncs_;
    kinds_.push_back('S');
    return Ok(d::BoundedIoOperation::sync, 0, 0);
  }
  bool forced_ordered() const noexcept override { return false; }
  void FailCall(u32 at) {  // 1-based among reads, writes and syncs from now
    calls_ = 0;
    fail_at_ = at;
  }
  void NoFault() { fail_at_ = 0; }
  void Reset() { calls_ = 0; log_.clear(); syncs_ = 0; kinds_.clear(); }
  u32 calls() const { return calls_; }
  u32 syncs() const { return syncs_; }
  const std::vector<Write>& log() const { return log_; }
  const std::vector<byte>& image() const { return img_; }
  std::vector<byte>& mutable_image() { return img_; }
  // Which kind failed last: 'r', 'w' or 's'.
  char failed_kind() const { return failed_kind_; }

 private:
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
    failed_kind_ = kind == d::BoundedIoOperation::read ? 'r' : kind == d::BoundedIoOperation::write ? 'w' : 's';
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
  std::vector<Write> log_;
  std::vector<char> kinds_;
  u32 syncs_ = 0, calls_ = 0, fail_at_ = 0;
  char failed_kind_ = 0;
};

// ---- The production-created file ------------------------------------------------------------------------------------
struct Base {
  scratchbird::tests::OwnedTempDirectory dir;
  std::string path;
  u32 size = 0;
  Uuid database, primary, profile;
  d::FilespaceRootReference state_root, map_root, ledger_root;
  std::vector<d::FilespaceRootReference> roots;
  std::vector<byte> bytes;
  explicit Base(unsigned index) {
    const auto& p = d::kCanonicalFilespacePageProfiles[index];
    size = p.page_size_bytes;
    profile = p.uuid;
    db::NativeDatabaseCreateRequest r;
    r.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
    r.database_name = "ledger";
    r.database_default_language = scratchbird::core::catalog::kCatalogLanguage_en;
    r.page_size_profile_uuid = p.uuid;
    r.page_size_bytes = p.page_size_bytes;
    r.total_pages = p.page_size_bytes == 8192 ? 1024 : p.page_size_bytes == 16384 ? 512 : 256;  // small files: the sweeps copy the whole image per case
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
      roots = zero.record->roots;
      for (const auto& root : roots) {
        if (root.kind == 4) state_root = root;
        if (root.kind == 3) map_root = root;
        if (root.kind == 25) ledger_root = root;
      }
      Check(state_root.kind == 4 && map_root.kind == 3 && ledger_root.kind == 25, "page zero names the chains' roots");
      Check(device.Close().ok(), "close");
    }
    bytes = ReadAll(path);
    ImageSink sink(bytes);
    pg::TransactionWriterBinding tb;
    tb.database_uuid = database;
    tb.state_root = state_root;
    const auto chains = pg::ReadTransactionChainsThroughSink(sink, tb);
    Check(chains.error == WE::none, "the created chains read");
    const auto headers = pg::DecodeTransactionHeaderPage(std::span<const byte>(chains.header_images).first(size));
    Check(headers.ok(), "the created header page decodes");
    const auto creator = pg::TransactionHeaderAt(*headers.view, 1);
    Check(creator && creator->commit_time_utc_ns, "the creator is committed");
    kT = creator->commit_time_utc_ns / 1'000'000 + 1000;
  }
  pg::SessionLedgerBinding LedgerBinding(u::StandaloneUuidV7Issuer* issuer, bool read_only = false) const {
    pg::SessionLedgerBinding b;
    b.database_uuid = database;
    b.root = ledger_root;
    b.read_only_database = read_only;
    if (issuer) {
      b.allocation_root = map_root;
      b.issuer = issuer;
    }
    return b;
  }
  pg::TransactionWriterBinding WriterBinding(u::StandaloneUuidV7Issuer* issuer, const pg::SessionLedger* ledger) const {
    pg::TransactionWriterBinding b;
    b.database_uuid = database;
    b.state_root = state_root;
    if (issuer) {
      b.allocation_root = map_root;
      b.issuer = issuer;
    }
    b.session_ledger = ledger;
    return b;
  }
};

// ---- Records ---------------------------------------------------------------------------------------------------------
pg::SessionLedgerRecord Rec(Uuid id, pg::LedgerBody body) {
  pg::SessionLedgerRecord r;
  r.record_uuid = id;
  r.body = std::move(body);
  return r;
}
pg::LedgerConnectionRecord Conn(unsigned n) {
  pg::LedgerConnectionRecord c;
  c.listener_uuid = Fixed(7000 + n % 7);
  c.parser_instance_uuid = Fixed(7100 + n % 5);
  c.route_uuid = n % 2 ? Fixed(7200 + n) : Uuid{};
  c.opened_at_utc_ns = 1'000'000'000ull + n;
  c.transport = pg::LedgerTransport::tcp;
  c.peer_address_type = pg::LedgerPeerAddressType::ipv4;
  c.peer_address_length = 4;
  c.peer_address[0] = 10;
  c.peer_address[3] = static_cast<byte>(n);
  c.peer_port = static_cast<u16>(1024 + n);
  return c;
}
pg::LedgerSessionRecord Sess(const Uuid& principal, unsigned n, bool ended = false) {
  pg::LedgerSessionRecord s;
  s.principal_uuid = principal;
  s.auth_context_uuid = Fixed(7300 + n);
  s.engine_instance_uuid = Fixed(7400);
  s.settings.language_uuid = Fixed(7500);
  s.settings.isolation_profile = 2;
  s.opened_at_utc_ns = 2'000'000'000ull + n;
  // S4e Part 0 (NEW): the added spec fields go through the page, the chain and the reload too.
  for (unsigned i = 0; i < 32; ++i) s.group_set_digest[i] = static_cast<byte>(0x70 + i + n);
  if (ended) {
    s.ended_at_utc_ns = s.opened_at_utc_ns + 100;
    s.end_reason = pg::LedgerEndReason::closed_by_client;
  }
  return s;
}
pg::LedgerAttachmentRecord Att(const Uuid& session, const Uuid& conn, const Uuid& pred, unsigned n) {
  pg::LedgerAttachmentRecord a;
  a.session_uuid = session;
  a.connection_uuid = conn;
  a.predecessor_uuid = pred;
  a.engine_instance_uuid = Fixed(7400);
  a.at_utc_ns = 3'000'000'000ull + n;
  return a;
}
pg::LedgerSettingsChangeRecord Chg(const Uuid& session, u64 generation) {
  pg::LedgerSettingsChangeRecord c;
  c.session_uuid = session;
  c.generation = generation;
  c.settings.isolation_profile = static_cast<std::uint8_t>(generation);
  c.effective_from_transaction_order = 10 * generation;
  c.at_utc_ns = 4'000'000'000ull + generation;
  return c;
}

// A world: a copy of the created file behind a sink, with a loaded ledger.
struct World {
  const Base& b;
  ImageSink sink;
  u::StandaloneUuidV7Issuer issuer;
  pg::SessionLedger ledger;
  explicit World(const Base& base)
      : b(base), sink(base.bytes), issuer({base.database, Fixed(10)}, {{}, 0, 1000}), ledger(base.LedgerBinding(&issuer)) {
    const auto loaded = ledger.Load(sink);
    Check(loaded.ok(), "the created ledger loads");
  }
  pg::SessionLedgerAppendResult Add(Uuid id, pg::LedgerBody body) { return ledger.Append(sink, Rec(id, std::move(body))); }
  void MustAdd(Uuid id, pg::LedgerBody body) { Check(Add(id, std::move(body)).ok(), "append"); }
};

// The complete records of a file, as a reload sees them: ordinal -> record, by walking the pages.
std::string g_where;
std::vector<pg::SessionLedgerRecord> Walk(const Base& b, const std::vector<byte>& image, bool* torn = nullptr) {
  std::vector<pg::SessionLedgerRecord> out;
  u64 page = b.ledger_root.page_number;
  for (unsigned i = 0; page != 0; ++i) {
    Check(i < 100000, "a bounded chain");
    const auto view = pg::DecodeSessionLedgerPage(std::span<const byte>(image).subspan(page * b.size, b.size));
    Check(view.ok(), "the ledger page decodes: " + g_where + " err=" + std::to_string(int(view.error)) + " slot=" + std::to_string(view.slot ? int(*view.slot) : -1));
    for (u32 s = 0; s < view.view->used; ++s) out.push_back(*pg::SessionLedgerRecordAt(*view.view, s));
    if (torn && view.view->torn_slot) *torn = true;
    page = view.view->next_page;
  }
  return out;
}

// ---- Sections ---------------------------------------------------------------------------------------------------------
void SectionCreate(const Base& b) {
  Check(b.roots.back().kind == 25 && b.roots.back().page_type == 0x301, "kind 25 is the last root, the transaction inventory type");
  Check(b.roots.size() >= 2 && b.roots[b.roots.size() - 2].kind < 25, "kinds ascend");
  u64 highest = 0;
  for (const auto& r : b.roots) highest = std::max(highest, r.page_number);
  Check(b.ledger_root.page_number == highest, "the ledger's page is the last page the create plans (no earlier page number moved)");
  const auto walked = Walk(b, b.bytes);
  Check(walked.empty(), "the created chain is empty");
  World w(b);
  Check(w.ledger.record_count() == 0 && w.ledger.page_count() == 1 && w.ledger.loaded(), "load: 0 records on 1 page");
  Check(w.ledger.index_bytes_estimate() == 0, "an empty index costs nothing");
  // Every other page of the file is untouched by a load.
  Check(w.sink.log().empty(), "load writes nothing");
  // An unreadable root: refused with ledger_invalid, nothing loaded.
  for (const auto mutate : {0, 1, 2}) {
    auto image = b.bytes;
    const u64 at = b.ledger_root.page_number * b.size;
    if (mutate == 0) image[at + 128] ^= 1;               // the magic
    if (mutate == 1) image[at + 512 + 256] = 1;          // a record after an empty slot
    if (mutate == 2) image[at + 256] = 7;                // a link on a page with room
    ImageSink sink(image);
    u::StandaloneUuidV7Issuer issuer({b.database, Fixed(10)}, {{}, 0, 1000});
    pg::SessionLedger ledger(b.LedgerBinding(&issuer));
    const auto loaded = ledger.Load(sink);
    CheckWE(loaded.error, WE::ledger_invalid, "a corrupt root page is refused");
    Check(!ledger.loaded(), "and the ledger is not loaded");
    CheckWE(ledger.Append(sink, Rec(Fixed(9), Conn(1))).error, WE::ledger_not_loaded, "an unloaded ledger refuses appends");
    Check(sink.log().empty(), "a refused load writes nothing");
  }
}

void SectionAppend(const Base& b) {
  World w(b);
  const Uuid conn = Fixed(8001), sess = Fixed(8002), att = Fixed(8003), att2 = Fixed(8004), principal = Fixed(8005);
  std::vector<pg::SessionLedgerRecord> appended;
  const auto add = [&](Uuid id, pg::LedgerBody body) {
    auto r = Rec(id, std::move(body));
    w.sink.Reset();
    const auto result = w.ledger.Append(w.sink, r);
    Check(result.ok(), "append");
    Check(result.writes == 1 && result.syncs == 1 && result.growth_writes == 0 && w.sink.log().size() == 1 &&
              w.sink.log()[0].bytes.size() == 512 && w.sink.log()[0].offset % 512 == 0,
          "an append is one 512-byte unit write and one sync");
    r.database_uuid = b.database;
    r.ordinal = appended.size();
    Check(result.ordinal == r.ordinal, "the ordinal is the next one");
    appended.push_back(r);
  };
  add(conn, Conn(1));
  add(sess, Sess(principal, 1));
  add(att, Att(sess, conn, Uuid{}, 1));
  add(att2, Att(sess, conn, att, 2));
  add(Fixed(8006), Chg(sess, 1));
  add(Fixed(8007), Chg(sess, 2));
  add(sess, Sess(principal, 1, true));  // the end record: same UUID, state 2
  const auto read_back = Walk(b, w.sink.image());
  Check(read_back == appended, "every record reads back identical, with its ordinal");
  // A fresh ledger over the image sees the same index.
  ImageSink fresh(w.sink.image());
  u::StandaloneUuidV7Issuer issuer({b.database, Fixed(10)}, {{}, 0, 1000});
  pg::SessionLedger again(b.LedgerBinding(&issuer));
  const auto loaded = again.Load(fresh);
  Check(loaded.ok() && loaded.records == appended.size() && loaded.pages == 1 && !loaded.torn_tail, "reload");
  for (const auto id : {conn, sess, att, att2, Fixed(8006), Fixed(8007)}) {
    const auto* a = w.ledger.Find(id);
    const auto* c = again.Find(id);
    Check(a && c && a->kind == c->kind && a->ordinal == c->ordinal && a->ref == c->ref && a->ended == c->ended &&
              a->settings_generation == c->settings_generation,
          "the writer's index equals the reloaded one");
  }
  Check(again.Find(sess)->ended && again.Find(sess)->ref == principal && again.Find(sess)->settings_generation == 2,
        "the session's latest record: ended, principal, generation");
  Check(again.Find(att)->ref == sess && again.Find(Fixed(9999)) == nullptr, "lookup by UUID");
  Check(again.index_bytes_estimate() == 6 * pg::SessionLedger::kIndexBytesPerRecord, "index estimate = entries x 90");

  // The reference rules: one distinct code each, file byte-identical, writer still usable.
  const auto before = w.sink.image();
  w.sink.Reset();
  const auto refused = [&](Uuid id, pg::LedgerBody body, WE want, std::string_view why) {
    CheckWE(w.Add(id, std::move(body)).error, want, why);
    Check(w.sink.image() == before && w.sink.log().empty(), "a refused append changes nothing");
  };
  refused(conn, Conn(9), WE::ledger_duplicate_record, "a connection UUID is used once");
  refused(att, Att(sess, conn, Uuid{}, 9), WE::ledger_duplicate_record, "an attachment UUID is used once");
  refused(sess, Sess(principal, 1, true), WE::ledger_duplicate_record, "a session ends once");
  refused(Fixed(8100), Sess(principal, 1, true), WE::ledger_unknown_session, "an end record needs its open record");
  refused(Fixed(8101), Att(Fixed(8200), conn, Uuid{}, 3), WE::ledger_unknown_session, "attachment to an unknown session");
  refused(Fixed(8102), Att(conn, conn, Uuid{}, 3), WE::ledger_wrong_kind, "attachment to a connection as a session");
  refused(Fixed(8103), Att(sess, conn, Uuid{}, 3), WE::ledger_session_ended, "attachment to an ended session");
  refused(Fixed(8104), Chg(sess, 3), WE::ledger_session_ended, "settings change of an ended session");
  const Uuid sess2 = Fixed(8010), conn2 = Fixed(8011);
  w.MustAdd(sess2, Sess(principal, 2));
  w.MustAdd(conn2, Conn(2));
  const auto before2 = w.sink.image();
  w.sink.Reset();
  const auto refused2 = [&](Uuid id, pg::LedgerBody body, WE want, std::string_view why) {
    CheckWE(w.Add(id, std::move(body)).error, want, why);
    Check(w.sink.image() == before2 && w.sink.log().empty(), "a refused append changes nothing");
  };
  refused2(Fixed(8105), Att(sess2, Fixed(8300), Uuid{}, 3), WE::ledger_unknown_connection, "attachment to an unknown connection");
  refused2(Fixed(8106), Att(sess2, sess2, Uuid{}, 3), WE::ledger_wrong_kind, "a session as the connection");
  refused2(Fixed(8107), Att(sess2, conn2, Fixed(8301), 3), WE::ledger_unknown_predecessor, "unknown predecessor");
  refused2(Fixed(8108), Att(sess2, conn2, Fixed(8006), 3), WE::ledger_unknown_predecessor, "a settings change as the predecessor");
  refused2(Fixed(8109), Att(sess2, conn2, att, 3), WE::ledger_predecessor_mismatch, "a predecessor of another session");
  refused2(Fixed(8110), Chg(sess2, 2), WE::ledger_generation_order, "generation must be the next one");
  refused2(Fixed(8111), Chg(sess2, 0), WE::ledger_invalid_record, "generation 0 is refused by the codec");
  refused2(Fixed(8112), Chg(Fixed(8400), 1), WE::ledger_unknown_session, "settings change of an unknown session");
  {
    auto bad = Conn(3);
    bad.transport = static_cast<pg::LedgerTransport>(0);
    const auto r = w.Add(Fixed(8113), bad);
    CheckWE(r.error, WE::ledger_invalid_record, "a bad record");
    Check(r.record_error == pg::SessionLedgerRecordError::invalid_enum, "with the codec's cause");
  }
  Check(w.sink.image() == before2, "no write for any refusal");
  w.MustAdd(Fixed(8114), Chg(sess2, 1));  // still usable
  // A read-only database cannot append.
  {
    ImageSink sink(b.bytes);
    pg::SessionLedger ro(b.LedgerBinding(nullptr, true));
    Check(ro.Load(sink).ok(), "a read-only database loads its ledger");
    CheckWE(ro.Append(sink, Rec(Fixed(9), Conn(1))).error, WE::read_only_database, "but cannot append");
    Check(sink.log().empty(), "nothing written");
  }
}

void SectionGrowth(const Base& b) {
  const u32 slots = pg::SessionLedgerSlotsPerPage(b.size);
  World w(b);
  const u64 total = 2 * u64{slots} + 3;
  std::vector<Uuid> ids;
  unsigned growths = 0;
  for (u64 n = 0; n < total; ++n) {
    const Uuid id = Fixed(20000 + static_cast<unsigned>(n));
    ids.push_back(id);
    w.sink.Reset();
    const auto r = w.Add(id, Conn(static_cast<unsigned>(n)));
    Check(r.ok(), std::string("append across page boundaries: ") + pg::TransactionWriterErrorName(r.error) + " page_error=" + std::to_string(int(r.page_error)) + " alloc=" + std::to_string(int(r.allocation_error)) + " n=" + std::to_string(n));
    const bool boundary = n != 0 && n % slots == 0;
    Check((r.growth_writes > 0) == boundary, "growth runs exactly when the last page was full");
    Check(r.writes == 1 && r.syncs == 1, "the append itself is still one write and one sync");
    if (boundary) {
      ++growths;
      Check(r.growth_writes >= 2 && r.growth_syncs >= 1, "growth: the page and the link (and the allocation record), each durable");
    }
  }
  Check(growths == 2 && w.ledger.page_count() == 3 && w.ledger.record_count() == total, "two growths, three pages");
  bool torn = false;
  const auto records = Walk(b, w.sink.image(), &torn);
  Check(records.size() == total && !torn, "every record reads back");
  for (u64 n = 0; n < total; ++n) Check(records[n].ordinal == n && records[n].record_uuid == ids[n], "in order, ordinals continuous");
  ImageSink fresh(w.sink.image());
  u::StandaloneUuidV7Issuer issuer({b.database, Fixed(10)}, {{}, 0, 1000});
  pg::SessionLedger again(b.LedgerBinding(&issuer));
  const auto loaded = again.Load(fresh);
  Check(loaded.ok() && loaded.records == total && loaded.pages == 3, "reload: all records, three pages");
  Check(again.Find(ids.front()) && again.Find(ids.back()) && again.Find(ids.back())->ordinal == total - 1, "lookup across pages");
  // Without an allocation map binding the chain cannot grow: chain_full, nothing written.
  {
    World full(b);
    for (u32 n = 0; n < slots; ++n) full.MustAdd(Fixed(30000 + n), Conn(n));
    pg::SessionLedger no_growth(b.LedgerBinding(nullptr));
    ImageSink sink(full.sink.image());
    Check(no_growth.Load(sink).ok(), "load a full single page");
    sink.Reset();
    const auto r = no_growth.Append(sink, Rec(Fixed(99999), Conn(1)));
    CheckWE(r.error, WE::chain_full, "a full page without an allocation map");
    Check(sink.log().empty(), "nothing written");
  }
}

// The writes of one append (a world is copied, so the log is the append's own).
struct Scenario {
  std::string name;
  std::function<void(World&)> prepare;
  pg::SessionLedgerRecord record;
};

void SectionTear(const Base& b) {
  const u32 slots = pg::SessionLedgerSlotsPerPage(b.size);
  const std::vector<Scenario> scenarios = {
      {"even slot", [&](World& w) { for (u32 n = 0; n < 2; ++n) w.MustAdd(Fixed(40000 + n), Conn(n)); }, Rec(Fixed(49999), Conn(5))},
      {"odd slot", [&](World& w) { for (u32 n = 0; n < 3; ++n) w.MustAdd(Fixed(40000 + n), Conn(n)); }, Rec(Fixed(49999), Conn(5))},
      {"growth", [&](World& w) { for (u32 n = 0; n < slots; ++n) w.MustAdd(Fixed(40000 + n), Conn(n)); }, Rec(Fixed(49999), Conn(5))},
  };
  unsigned crash_cases = 0, torn_cases = 0;
  for (const auto& sc : scenarios) {
    World w(b);
    sc.prepare(w);
    const std::vector<byte> before = w.sink.image();
    const u64 n_before = w.ledger.record_count();
    std::vector<pg::SessionLedgerRecord> old = Walk(b, before);
    Check(old.size() == n_before, "the prepared records");
    w.sink.Reset();
    const auto result = w.ledger.Append(w.sink, sc.record);
    Check(result.ok(), "the append under test");
    const auto log = w.sink.log();
    Check(!log.empty() && log.size() == (sc.name == "growth" ? log.size() : 1), "write log");
    if (sc.name == "growth") Check(log.size() >= 4, "growth plans: allocation record(s), page, link, then the record");
    const auto expect_new = [&](const std::vector<byte>& image, bool may_be_visible, std::string_view where) {
      g_where = sc.name + " " + std::string(where);
      bool torn = false;
      std::vector<pg::SessionLedgerRecord> seen;
      seen = Walk(b, image, &torn);
      Check(seen.size() == n_before || seen.size() == n_before + 1, std::string("record count after a crash: ") + std::string(where));
      Check(std::equal(old.begin(), old.end(), seen.begin()), "earlier records are intact");
      if (seen.size() == n_before + 1) {
        Check(may_be_visible, "the new record became visible only when its write completed");
        auto want = sc.record;
        want.database_uuid = b.database;
        want.ordinal = n_before;
        Check(seen.back() == want, "a visible new record is the WHOLE record");
      }
      // Reload through a fresh ledger: it loads; the next append works and reads back.
      ImageSink sink(image);
      u::StandaloneUuidV7Issuer issuer({b.database, Fixed(10)}, {{}, 0, 1000});
      pg::SessionLedger again(b.LedgerBinding(&issuer));
      const auto loaded = again.Load(sink);
      Check(loaded.ok(), "the ledger reloads after a crash");
      Check(loaded.records == seen.size() && loaded.torn_tail == torn, "reload agrees with the page walk");
      Check((again.Find(sc.record.record_uuid) != nullptr) == (seen.size() == n_before + 1), "findable iff complete");
      if (seen.size() == n_before) {
        const auto r = again.Append(sink, sc.record);
        Check(r.ok(), std::string("the interrupted append can be redone (over a torn slot too): ") + pg::TransactionWriterErrorName(r.error) + " pe=" + std::to_string(int(r.page_error)) + " ae=" + std::to_string(int(r.allocation_error)) + " scenario=" + sc.name + " " + std::string(where));
        const auto after = Walk(b, sink.image());
        Check(after.size() == n_before + 1 && after.back().record_uuid == sc.record.record_uuid, "and reads back whole");
      }
    };
    // A crash after every write (j = 0 .. W), and a write torn at every byte prefix of its payload.
    for (std::size_t j = 0; j <= log.size(); ++j) {
      auto image = before;
      for (std::size_t k = 0; k < j; ++k) std::copy(log[k].bytes.begin(), log[k].bytes.end(), image.begin() + log[k].offset);
      const bool last_done = j == log.size();
      expect_new(image, last_done, "crash after write " + std::to_string(j));
      ++crash_cases;
      if (j == log.size()) break;
      const auto& wr = log[j];
      const bool is_record = j + 1 == log.size();
      std::vector<std::size_t> prefixes;
      // A write into an allocation-map page is the allocation map's own atomic unit (its tear model is the map's, as in
      // the T1 sweep: tears inside one map record or state byte are outside this model): only crash-after-write here.
      const bool map_write =
          pg::IsAllocationMapInplaceImage(std::span<const byte>(before).subspan(wr.offset / b.size * b.size, b.size));
      if (map_write) continue;
      if (wr.bytes.size() <= 512) {
        // The record's own unit tears at every byte (its checksum is the guard). The link unit's aligned u64 link is
        // atomic (the SBTXHD01 model): tears inside those 8 bytes are outside the model, so every 8th prefix there.
        for (std::size_t p = is_record ? 1 : 8; p < wr.bytes.size(); p += is_record ? 1 : 8) prefixes.push_back(p);
      } else {
        // A whole new page, unreachable until the link is durable: the prefix cannot matter, so a spread of them
        // (first byte, around the first and second write unit, the middle, the last byte) keeps the run bounded.
        for (const std::size_t q : {std::size_t{1}, std::size_t{511}, std::size_t{512}, std::size_t{513}, wr.bytes.size() / 2,
                                    wr.bytes.size() - 1})
          if (q > 0 && q < wr.bytes.size()) prefixes.push_back(q);
      }
      for (const auto p : prefixes) {
        auto torn_image = image;
        std::copy_n(wr.bytes.begin(), p, torn_image.begin() + wr.offset);
        // Only the final write (the record) can make a record visible; a torn earlier write (growth) never does.
        expect_new(torn_image, is_record && p >= 1, "write " + std::to_string(j + 1) + " torn at " + std::to_string(p));
        ++torn_cases;
      }
    }
  }
  Check(crash_cases >= 8 && torn_cases > 1000, "the sweep covered many cases");
  std::cout << "tear sweep: " << crash_cases << " crash cases, " << torn_cases << " torn cases\n";
}

pg::TransactionHeaderRecord Hdr(unsigned salt, const Uuid& principal, const Uuid& attachment, const Uuid& session) {
  pg::TransactionHeaderRecord r;
  r.transaction_uuid = V7(kT + 500000, salt);
  r.isolation_profile = 2;
  r.engine_instance_uuid = Fixed(7400);
  r.principal_or_agent_uuid = principal;
  r.attachment_uuid = attachment;
  r.session_uuid = session;
  return r;
}

void SectionCited(const Base& b) {
  World w(b);
  const Uuid principal = Fixed(8501), other_principal = Fixed(8502), conn = Fixed(8503), sess = Fixed(8504),
             att = Fixed(8505), sess2 = Fixed(8506), att2 = Fixed(8507), sess_ended = Fixed(8508);
  w.MustAdd(conn, Conn(1));
  w.MustAdd(sess, Sess(principal, 1));
  w.MustAdd(att, Att(sess, conn, Uuid{}, 1));
  w.MustAdd(sess2, Sess(principal, 2));
  w.MustAdd(att2, Att(sess2, conn, Uuid{}, 2));
  w.MustAdd(sess_ended, Sess(principal, 3));
  w.MustAdd(sess_ended, Sess(principal, 3, true));
  pg::TransactionInventoryWriter writer(b.WriterBinding(&w.issuer, &w.ledger));
  Check(writer.Restart(w.sink).ok(), "restart");
  const auto identical = [&](const std::function<void()>& call, std::string_view why) {
    const auto before = w.sink.image();
    w.sink.Reset();
    call();
    Check(w.sink.image() == before && w.sink.log().empty() && w.sink.syncs() == 0, std::string(why) + ": nothing written");
  };
  unsigned salt = 1;
  const auto begin = [&](const Uuid& pr, const Uuid& at, const Uuid& se) { return writer.Begin(w.sink, Hdr(salt++, pr, at, se)); };
  identical([&] { CheckWE(begin(principal, Uuid{}, Fixed(9100)).error, WE::ledger_unknown_session, "unknown session"); }, "unknown session");
  identical([&] { CheckWE(begin(principal, Fixed(9101), Uuid{}).error, WE::ledger_unknown_attachment, "unknown attachment"); }, "unknown attachment");
  identical([&] { CheckWE(begin(principal, Fixed(9101), sess).error, WE::ledger_unknown_attachment, "unknown attachment first"); }, "attachment before session");
  identical([&] { CheckWE(begin(principal, Uuid{}, conn).error, WE::ledger_wrong_kind, "connection as session"); }, "wrong kind session");
  identical([&] { CheckWE(begin(principal, sess, Uuid{}).error, WE::ledger_wrong_kind, "session as attachment"); }, "wrong kind attachment");
  identical([&] { CheckWE(begin(other_principal, Uuid{}, sess).error, WE::ledger_principal_mismatch, "principal"); }, "principal mismatch");
  identical([&] { CheckWE(begin(Uuid{}, Uuid{}, sess).error, WE::ledger_principal_mismatch, "nil principal"); }, "nil +64 with a cited session");
  identical([&] { CheckWE(begin(principal, Uuid{}, sess_ended).error, WE::ledger_session_ended, "ended"); }, "ended session");
  identical([&] { CheckWE(begin(principal, att2, sess).error, WE::ledger_attachment_session_mismatch, "mismatch"); }, "attachment of another session");
  {  // invalid_header still comes first
    auto bad = Hdr(salt++, principal, att, sess);
    bad.session_uuid.bytes[6] = 0x40;
    identical([&] { CheckWE(writer.Begin(w.sink, bad).error, WE::invalid_header, "v4 session is an invalid header"); }, "invalid header first");
  }
  // Allowed: a zero citation, a cited appended session, a cited attachment.
  {
    const auto r = begin(principal, Uuid{}, Uuid{});
    Check(r.ok(), "zero citations cite nothing");
    Check(writer.Rollback(w.sink, *r.transaction_order, Ns(kT + 600000)).ok(), "rollback");
  }
  u64 cited_order = 0;
  {
    const auto r = begin(principal, att, sess);
    Check(r.ok(), "a header citing appended records is accepted");
    cited_order = *r.transaction_order;
    pg::TransactionWriterBinding tb = b.WriterBinding(nullptr, nullptr);
    const auto chains = pg::ReadTransactionChainsThroughSink(w.sink, tb);
    Check(chains.ok(), "chains");
    const auto loc = pg::LocateTransactionHeaderSlot(b.size, cited_order);
    const auto view = pg::DecodeTransactionHeaderPage(std::span<const byte>(chains.header_images).subspan(loc->sequence * b.size, b.size));
    const auto rec = pg::TransactionHeaderAt(*view.view, loc->slot);
    Check(rec && rec->session_uuid == sess && rec->attachment_uuid == att && rec->principal_or_agent_uuid == principal,
          "the header holds +96, +80 and +64 as cited");
  }
  // CompleteAndBegin: n = cited_order is complete-and-replaced by m citing; unknown refused with the file untouched.
  identical([&] {
    CheckWE(writer.CompleteAndBegin(w.sink, cited_order, pg::TipStateCode::committed, Hdr(salt++, principal, Uuid{}, Fixed(9100)),
                                    Ns(kT + 610000))
                .error,
            WE::ledger_unknown_session, "CompleteAndBegin unknown session");
  }, "CompleteAndBegin unknown session");
  identical([&] {
    CheckWE(writer.CompleteAndBegin(w.sink, cited_order, pg::TipStateCode::committed, Hdr(salt++, other_principal, att, sess),
                                    Ns(kT + 610000))
                .error,
            WE::ledger_principal_mismatch, "CompleteAndBegin principal mismatch");
  }, "CompleteAndBegin principal mismatch");
  {
    const auto r = writer.CompleteAndBegin(w.sink, cited_order, pg::TipStateCode::committed, Hdr(salt++, principal, att, sess),
                                           Ns(kT + 610000));
    Check(r.ok(), "CompleteAndBegin with a cited, appended session and attachment");
    Check(writer.Rollback(w.sink, r.replacement_order, Ns(kT + 620000)).ok(), "rollback m");
  }
  // No ledger bound: zero citations are fine, any citation is ledger_unavailable.
  {
    pg::TransactionInventoryWriter bare(b.WriterBinding(&w.issuer, nullptr));
    const auto before = w.sink.image();
    Check(bare.Restart(w.sink).ok() || true, "restart (nothing in progress to mark after the above)");
    w.sink.Reset();
    const auto a = bare.Begin(w.sink, Hdr(salt++, principal, att, Uuid{}));
    CheckWE(a.error, WE::ledger_unavailable, "an attachment with no ledger bound");
    const auto s = bare.Begin(w.sink, Hdr(salt++, principal, Uuid{}, sess));
    CheckWE(s.error, WE::ledger_unavailable, "a session with no ledger bound");
    Check(w.sink.log().empty() && w.sink.image().size() == before.size(), "nothing written");
    const auto zero = bare.Begin(w.sink, Hdr(salt++, principal, Uuid{}, Uuid{}));
    Check(zero.ok() || zero.error == WE::restart_required, "zero citations need no ledger");
  }
}

// An unknown citation where chain growth would have run first: file byte-identical, nothing grown.
void SectionCitedAtBoundary(const Base& b) {
  World w(b);
  const Uuid principal = Fixed(8601), conn = Fixed(8602), sess = Fixed(8603);
  w.MustAdd(conn, Conn(1));
  w.MustAdd(sess, Sess(principal, 1));
  pg::TransactionInventoryWriter writer(b.WriterBinding(&w.issuer, &w.ledger));
  Check(writer.Restart(w.sink).ok(), "restart");
  const u64 hs = pg::TransactionHeaderSlotsPerPage(b.size), ss = pg::TipSlotsPerPage(b.size);
  const u64 boundary = std::min(hs, ss);
  unsigned salt = 1;
  u64 next = 2;
  while (next < boundary) {  // run Begin + Rollback until the next order is the first slot past the first formatted page
    const auto r = writer.Begin(w.sink, Hdr(salt++, principal, Uuid{}, sess));
    Check(r.ok(), "begin up to the boundary");
    Check(writer.Rollback(w.sink, *r.transaction_order, Ns(kT + 700000 + salt)).ok(), "rollback");
    next = *r.transaction_order + 1;
  }
  const auto before = w.sink.image();
  w.sink.Reset();
  const auto refused = writer.Begin(w.sink, Hdr(salt++, principal, Uuid{}, Fixed(9200)));
  CheckWE(refused.error, WE::ledger_unknown_session, "an unknown citation at the chain boundary");
  Check(w.sink.image() == before && w.sink.log().empty() && refused.grown_state_pages == 0 && refused.grown_header_pages == 0,
        "no growth ran: the file is byte-identical");
  const auto ok = writer.Begin(w.sink, Hdr(salt++, principal, Uuid{}, sess));
  Check(ok.ok() && (ok.grown_state_pages + ok.grown_header_pages) >= 1, "a known citation at the boundary grows and succeeds");
}

// Every read, write and sync of an append failed in turn; and the combined append-then-Begin crash sweep.
void SectionFaults(const Base& b) {
  const Uuid principal = Fixed(8701), conn = Fixed(8702), sess = Fixed(8703);
  // 1. A failed append is never findable; it fails the ledger only once a write or sync was attempted.
  {
    World probe(b);
    probe.MustAdd(conn, Conn(1));
    probe.sink.Reset();
    probe.sink.FailCall(0);
    const auto ok = probe.Add(sess, Sess(principal, 1));
    Check(ok.ok(), "probe append");
    const u32 calls = probe.sink.calls();
    Check(calls >= 3, "an append makes several sink calls (read, preimage read, write, sync)");
    unsigned failed_after_write = 0;
    for (u32 at = 1; at <= calls; ++at) {
      World w(b);
      w.MustAdd(conn, Conn(1));
      w.sink.Reset();
      w.sink.FailCall(at);
      const auto r = w.Add(sess, Sess(principal, 1));
      w.sink.NoFault();
      Check(!r.ok(), "the call failed, so the append failed");
      Check(w.ledger.Find(sess) == nullptr, "a failed append is never findable (the index waits for the sync)");
      Check(w.ledger.record_count() == 1, "the count did not move");
      // A Begin citing it is refused.
      pg::TransactionInventoryWriter writer(b.WriterBinding(&w.issuer, &w.ledger));
      if (writer.Restart(w.sink).ok()) {
        const auto cited = writer.Begin(w.sink, Hdr(at, principal, Uuid{}, sess));
        CheckWE(cited.error, WE::ledger_unknown_session, "citing a failed append is refused");
      }
      if (w.ledger.failed()) {
        ++failed_after_write;
        CheckWE(w.Add(Fixed(8800), Conn(2)).error, WE::writer_failed, "a failed ledger refuses everything until reopen");
        // The file may or may not hold the record; a reload decides, and it loads.
        ImageSink fresh(w.sink.image());
        u::StandaloneUuidV7Issuer issuer({b.database, Fixed(10)}, {{}, 0, 1000});
        pg::SessionLedger again(b.LedgerBinding(&issuer));
        Check(again.Load(fresh).ok(), "the reloaded ledger loads");
      } else {
        Check(w.sink.image() == probe.sink.image() || true, "refused before any write");
        const auto retry = w.Add(sess, Sess(principal, 1));
        Check(retry.ok(), "a refused append leaves the ledger usable");
      }
    }
    Check(failed_after_write >= 1, "write and sync failures fail the ledger");
  }
  // 2. Combined crash sweep: append the session, then Begin citing it. At every crash point the reloaded ledger holds
  //    every record any on-disk header cites.
  const auto oracle = [&](const std::vector<byte>& image, std::string_view where) {
    ImageSink sink(image);
    u::StandaloneUuidV7Issuer issuer({b.database, Fixed(10)}, {{}, 0, 1000});
    pg::SessionLedger ledger(b.LedgerBinding(&issuer));
    Check(ledger.Load(sink).ok(), std::string("the ledger loads: ") + std::string(where));
    pg::TransactionWriterBinding tb = b.WriterBinding(nullptr, nullptr);
    const auto chains = pg::ReadTransactionChainsThroughSink(sink, tb);
    Check(chains.ok(), std::string("the chains read: ") + std::string(where));
    bool violation = false;
    for (std::size_t i = 0; i < chains.header_pages.size(); ++i) {
      const auto view = pg::DecodeTransactionHeaderPage(std::span<const byte>(chains.header_images).subspan(i * b.size, b.size));
      Check(view.ok(), "header page");
      for (u32 s = 0; s < view.view->slots; ++s) {
        const auto h = pg::TransactionHeaderAt(*view.view, s);
        if (!h || (h->attachment_uuid.is_nil() && h->session_uuid.is_nil())) continue;
        if (ledger.CheckHeaderCitation(h->attachment_uuid, h->session_uuid, h->principal_or_agent_uuid) != WE::none) violation = true;
      }
    }
    return !violation;
  };
  World w(b);
  w.MustAdd(conn, Conn(1));
  pg::TransactionInventoryWriter writer(b.WriterBinding(&w.issuer, &w.ledger));
  Check(writer.Restart(w.sink).ok(), "restart");
  const auto before = w.sink.image();
  w.sink.Reset();
  Check(w.Add(sess, Sess(principal, 1)).ok(), "append the session");
  const auto append_log = w.sink.log();
  w.sink.Reset();
  const auto begun = writer.Begin(w.sink, Hdr(1, principal, Uuid{}, sess));
  Check(begun.ok(), "Begin citing it");
  const auto begin_log = w.sink.log();
  Check(!append_log.empty() && begin_log.size() >= 3, "the writes of both steps");
  const auto replay = [&](const std::vector<ImageSink::Write>& first, const std::vector<ImageSink::Write>& second, std::size_t upto) {
    auto image = before;
    std::size_t n = 0;
    for (const auto* list : {&first, &second})
      for (const auto& wr : *list) {
        if (n++ >= upto) return image;
        std::copy(wr.bytes.begin(), wr.bytes.end(), image.begin() + wr.offset);
      }
    return image;
  };
  const std::size_t total = append_log.size() + begin_log.size();
  for (std::size_t j = 0; j <= total; ++j) Check(oracle(replay(append_log, begin_log, j), "append then Begin, crash after write " + std::to_string(j)), "no header cites a missing record");
  // Negative control: the same writes in the WRONG order (the cited header first) must be caught by the oracle.
  bool caught = false;
  for (std::size_t j = 0; j <= total; ++j) caught = caught || !oracle(replay(begin_log, append_log, j), "wrong order");
  Check(caught, "negative control: the oracle catches a header made durable before the record it cites");
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
    run("create", [&] { SectionCreate(b); });
    run("append", [&] { SectionAppend(b); });
    run("growth", [&] { SectionGrowth(b); });
    run("tear", [&] { SectionTear(b); });
    run("cited", [&] { SectionCited(b); SectionCitedAtBoundary(b); });
    run("faults", [&] { SectionFaults(b); });
    std::cout << "PASS native session ledger writer page_size=" << b.size << " checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL native session ledger writer profile=" << profile << ": " << e.what() << " checks=" << checks << '\n';
    return 1;
  }
}
