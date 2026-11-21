// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_LOOP_LEDGER, SB_SESSION_LEDGER_ROOT (plan 36 T3/T4, owner Q165 = 1, Q308).
// The session ledger through the database loop on a PRODUCTION-created, really SERVED database (StartHostedEngine, the
// server's open path: open verification with its ledger stage, Restart, the ledger Load), one page size per run:
//   - the created file opens with an EMPTY ledger (1 page, no record, no torn tail) and wasted_pages == 0;
//   - SubmitAppendLedgerRecord appends every record kind (and enough connection records to cross a page boundary: the
//     chain grows, each append one write + one sync), refusals are specific and write nothing, a cited
//     Begin / Commit works and is refused (nothing written, file byte-identical) when it cites an unknown record;
//   - the server is stopped and started again (a real reopen): every record is found, the ledger counts agree, the
//     grown page is NOT counted as wasted space, the earlier cited header still resolves and a new append works;
//   - a torn last record (a prefix of a record written after the last complete one) is ignored at open (torn_tail,
//     never visible) and the next append overwrites it;
//   - the open REFUSES, with a specific code, a file whose page zero has no kind-25 root (LEDGER_MISSING), whose
//     ledger page has a record after an empty slot, whose chain is linked to a page that is not a ledger page, and
//     whose last ledger record, cited by a transaction header, is zeroed or torn (LEDGER_INVALID header_citation);
//   - a restricted database refuses ledger appends with the Q299 code and a failed one with database_failed.
// Modes: create <path> <page_size>; serve <path> <scenario> <database> <bootstrap> <page_size> [records]; the driver
// (no argument, or `size <bytes>`) runs one size. Scenario `ledger_measure` is for native_session_ledger_strace.py.
#include "filespace_page_zero.hpp"
#include "hash_digest.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "local_password_verifier.hpp"
#include "memory.hpp"
#include "memory_policy_config.hpp"
#include "session_ledger_chain.hpp"
#include "allocation_map_inplace_page.hpp"
#include "transaction_inventory_writer.hpp"
#include "server/config.hpp"
#include "server/engine_host.hpp"
#include "server/native_database_loop.hpp"
#include "server/process_crypto_owner.hpp"
#include "uuid.hpp"
#include "../support/native_database_create_fixture.hpp"
#include "../support/owned_temp_directory.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <source_location>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace p = scratchbird::core::platform;
namespace d = scratchbird::storage::disk;
namespace pg = scratchbird::storage::page;
namespace srv = scratchbird::server;
namespace mem = scratchbird::core::memory;
namespace fs = std::filesystem;
using TE = srv::NativeLoopTransactionError;

constexpr std::string_view kBootstrapPassword = "T4-Bootstrap-Pw-71c3d9e0a2";

int checks = 0;
void Check(bool ok, const std::string& what, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL %s (line %u)\n", what.c_str(), at.line());
    std::fflush(stderr);
    std::exit(1);
  }
}
std::string Hex(const p::Uuid& id) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (const auto byte : id.bytes) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 15]);
  }
  return out;
}
p::Uuid FromHex(std::string_view hex) {
  p::Uuid id;
  Check(hex.size() == 32, "a 32-digit hex identity");
  const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (std::size_t i = 0; i < 16; ++i) id.bytes[i] = static_cast<p::byte>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
  return id;
}
p::Uuid Fresh() {
  const auto id = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  Check(id.has_value(), "an identity");
  return *id;
}
p::Uuid Fixed(unsigned n) {
  p::Uuid id;
  id.bytes[0] = 1;
  id.bytes[6] = 0x70;
  id.bytes[8] = 0x80;
  id.bytes[12] = static_cast<p::byte>(n >> 24);
  id.bytes[13] = static_cast<p::byte>(n >> 16);
  id.bytes[14] = static_cast<p::byte>(n >> 8);
  id.bytes[15] = static_cast<p::byte>(n);
  return id;
}
std::vector<p::byte> ReadAll(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(in)), {});
  return std::vector<p::byte>(reinterpret_cast<const p::byte*>(bytes.data()),
                              reinterpret_cast<const p::byte*>(bytes.data()) + bytes.size());
}
void WriteAll(const fs::path& path, const std::vector<p::byte>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  Check(bool(out.flush()), "write the file");
}
std::map<std::string, std::string> Snapshot(const fs::path& dir) {
  std::map<std::string, std::string> out;
  for (const auto& entry : fs::directory_iterator(dir)) {
    std::string value = "dir";
    if (entry.is_regular_file()) {
      const auto bytes = ReadAll(entry.path());
      const auto digest = scratchbird::core::hash::ComputeSha256Digest(bytes.data(), bytes.size());
      Check(digest.ok(), "sha256");
      value = std::to_string(bytes.size()) + ":" + scratchbird::core::hash::HexLower(digest.digest);
    }
    out[entry.path().filename().string()] = value;
  }
  return out;
}

int CreateMode(const fs::path& path, std::uint32_t page_size) {
  std::string verifier;
  if (!scratchbird::core::crypto::DeriveLocalPasswordVerifier(kBootstrapPassword, &verifier)) return 1;
  scratchbird::tests::NativeFixtureCreateRequest request;
  request.path = path;
  request.page_size = page_size;
  request.database_name = "native_session_ledger_loop";
  request.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  request.bootstrap_credential_fingerprint = verifier;
  const auto made = scratchbird::tests::CreateNativeFixtureDatabase(request);
  if (!made.ok) {
    std::cerr << made.diagnostic_code << ':' << made.diagnostic_detail << '\n';
    return 1;
  }
  std::cout << "CREATED database=" << Hex(made.database_uuid) << " bootstrap=" << Hex(made.bootstrap_principal_uuid) << std::endl;
  return 0;
}

// ---- records ------------------------------------------------------------------------------------------------------
pg::SessionLedgerRecord Rec(const p::Uuid& id, pg::LedgerBody body) {
  pg::SessionLedgerRecord r;
  r.record_uuid = id;
  r.body = std::move(body);
  return r;
}
pg::LedgerConnectionRecord Conn(unsigned n) {
  pg::LedgerConnectionRecord c;
  c.listener_uuid = Fixed(7000 + n % 7);
  c.parser_instance_uuid = Fixed(7100 + n % 5);
  c.opened_at_utc_ns = 1'000'000'000ull + n;
  c.transport = pg::LedgerTransport::tcp;
  c.peer_address_type = pg::LedgerPeerAddressType::ipv4;
  c.peer_address_length = 4;
  c.peer_address[0] = 10;
  c.peer_address[3] = static_cast<p::byte>(n);
  c.peer_port = static_cast<p::u16>(1024 + n);
  return c;
}
pg::LedgerSessionRecord Sess(const p::Uuid& principal) {
  pg::LedgerSessionRecord s;
  s.principal_uuid = principal;
  s.engine_instance_uuid = Fixed(7400);
  s.settings.language_uuid = Fixed(7500);
  s.opened_at_utc_ns = 2'000'000'000ull;
  return s;
}
pg::LedgerAttachmentRecord Att(const p::Uuid& session, const p::Uuid& conn, const p::Uuid& pred) {
  pg::LedgerAttachmentRecord a;
  a.session_uuid = session;
  a.connection_uuid = conn;
  a.predecessor_uuid = pred;
  a.engine_instance_uuid = Fixed(7400);
  a.at_utc_ns = 3'000'000'000ull;
  return a;
}
pg::LedgerSettingsChangeRecord Chg(const p::Uuid& session, p::u64 generation) {
  pg::LedgerSettingsChangeRecord c;
  c.session_uuid = session;
  c.generation = generation;
  c.settings.isolation_profile = 2;
  c.effective_from_transaction_order = 5;
  c.at_utc_ns = 4'000'000'000ull;
  return c;
}
const p::Uuid kConn = Fixed(1000), kSession = Fixed(1001), kAtt = Fixed(1002), kAtt2 = Fixed(1003), kChange = Fixed(1004);

// ---- a write failure through the loop's sink seam (the same one the transaction requests use) ----------------------
class FailAfterWritesSink final : public d::CarefulWriteSink {
 public:
  FailAfterWritesSink(d::CarefulWriteSink& inner, int good_writes) : inner_(inner), good_(good_writes) {}
  d::BoundedIoResult WriteAt(p::u64 offset, std::span<const p::byte> bytes) override {
    if (good_-- > 0) return inner_.WriteAt(offset, bytes);
    d::BoundedIoResult r;
    r.operation = d::BoundedIoOperation::write;
    r.error = d::BoundedIoError::native_failure;
    r.offset = offset;
    r.requested_bytes = bytes.size();
    r.native_error = EIO;
    r.native_attempted = true;
    return r;
  }
  d::BoundedIoResult ReadAt(p::u64 offset, std::span<p::byte> bytes) override { return inner_.ReadAt(offset, bytes); }
  d::BoundedIoResult Sync() override { return inner_.Sync(); }
  bool forced_ordered() const noexcept override { return inner_.forced_ordered(); }

 private:
  d::CarefulWriteSink& inner_;
  int good_;
};

// ---- the served child ---------------------------------------------------------------------------------------------
void Mark(const std::string& name) {
  std::fprintf(stderr, "MARK %s\n", name.c_str());
  std::fflush(stderr);
}
void PrintMeasure(const char* op, const srv::NativeLoopLedgerResult& r) {
  std::printf("MEASURE op=%s order=%llu writes=%u syncs=%u growth_writes=%u growth_syncs=%u\n", op,
              static_cast<unsigned long long>(r.ordinal), r.writes, r.syncs, r.growth_writes, r.growth_syncs);
  std::fflush(stdout);
}

int ServeMode(const fs::path& path, const std::string& scenario, const p::Uuid& database, const p::Uuid& bootstrap,
              std::uint32_t page_size, const std::string& expect_refusal, unsigned records) {
  (void)database;
  srv::ServerBootstrapConfig config;
  config.database_policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  config.memory_enable_platform_memory_probe = false;
  const auto policy = srv::ResolveServerMemoryAllocationPolicy(config);
  srv::ServerProcessCryptoOwner crypto;
  if (!policy.ok() || !mem::ConfigureDefaultMemoryManager(policy.policy, config.memory_policy_provenance).ok() ||
      !crypto.Start(config, policy).ok()) {
    std::fprintf(stderr, "process setup failed\n");
    return 3;
  }
  config.database_default_path = path;
  const auto directory = path.parent_path();
  auto started = srv::StartHostedEngine(config, nullptr, &crypto);
  if (scenario == "refused") {
    Check(!started.ok() && !started.diagnostics.empty(), "the open is refused");
    std::string detail;
    for (const auto& f : started.diagnostics.front().fields) detail += f.key + "=" + f.value + " ";
    Check(started.diagnostics.front().code == expect_refusal,
          "the refusal code is " + expect_refusal + ", got " + started.diagnostics.front().code + " " + detail);
    std::printf("REFUSED code=%s %s\n", started.diagnostics.front().code.c_str(), detail.c_str());
    std::printf("LEDGER_LOOP_OK checks=%d scenario=%s\n", checks, scenario.c_str());
    return 0;
  }
  if (scenario == "ledger_failed") {
    Check(!started.ok(), "the seam-less failed fixture does not serve");
  } else {
    Check(started.ok(), "the server serves the file: " + (started.diagnostics.empty() ? std::string() : started.diagnostics.front().code));
  }
  Check(!started.state.database_runtimes.empty() && started.state.database_runtimes.front() &&
            started.state.database_runtimes.front()->native_loop,
        "the hosted state holds the native loop");
  auto& loop = *started.state.database_runtimes.front()->native_loop;
  Check(!loop.on_loop_thread(), "the test is not on the loop thread");
  const unsigned slots = pg::SessionLedgerSlotsPerPage(page_size);
  const auto facts = [&] {
    std::optional<srv::NativeLoopServingFacts> f;
    Check(loop.RunOnLoopThreadForTest([&] { f = loop.FactsOnLoopThread(); }), "the loop ran the facts probe");
    Check(f.has_value(), "facts");
    return *f;
  };
  const auto add = [&](const p::Uuid& id, pg::LedgerBody body) { return loop.SubmitAppendLedgerRecord(Rec(id, std::move(body))).get(); };
  const auto find = [&](const p::Uuid& id) {
    std::optional<pg::SessionLedgerEntry> e;
    Check(loop.RunOnLoopThreadForTest([&] { e = loop.FindLedgerEntryOnLoopThread(id); }), "the loop ran the lookup");
    return e;
  };
  const auto begin = [&](const p::Uuid& principal, const p::Uuid& attachment, const p::Uuid& session) {
    srv::NativeLoopBeginRequest r;
    r.owner_key = Fresh();
    r.isolation_profile = 2;
    r.identity.principal_uuid = principal;
    r.identity.attachment_uuid = attachment;
    r.identity.session_uuid = session;
    return std::pair{r.owner_key, loop.SubmitBeginWork(r).get()};
  };

  if (scenario == "ledger_restricted" || scenario == "ledger_failed2") {
    Check(loop.serving_state() == srv::NativeServingState::restricted, "the database is restricted");
    const auto r = add(kConn, Conn(1));
    Check(!r.ok() && r.error == TE::database_restricted, "a ledger append on a restricted database is refused");
    Check(std::string(srv::NativeLoopTransactionErrorCode(r.error)) == "ENGINE.NATIVE.DATABASE_RESTRICTED_READ_ONLY",
          "with the Q299 code the transactions use");
    Check(r.writes == 0, "nothing written");
    Check(facts().ledger_records == 0 && facts().ledger_pages == 1, "the restricted database still read its (empty) ledger");
  } else if (scenario == "ledger_writefail") {
    Check(add(kConn, Conn(1)).ok(), "an append before the fault");
    srv::SetNativeTransactionSinkWrapperForTest([](d::CarefulWriteSink& inner) -> std::unique_ptr<d::CarefulWriteSink> {
      return std::make_unique<FailAfterWritesSink>(inner, 0);  // the record's own write fails
    });
    const auto failed = add(kSession, Sess(bootstrap));
    srv::SetNativeTransactionSinkWrapperForTest({});
    Check(!failed.ok() && failed.error == TE::writer_failed && failed.outcome_uncertain && failed.ledger_error == "write_failed",
          "an append whose write fails is writer_failed, outcome uncertain");
    Check(!find(kSession).has_value(), "the failed record is not findable (never citable)");
    Check(loop.serving_state() == srv::NativeServingState::failed, "the database became failed (requires reopen)");
    const auto again = add(Fixed(1300), Conn(2));
    Check(!again.ok() && again.error == TE::database_failed, "a later ledger append is refused: the database is failed");
    const auto cite = begin(bootstrap, p::Uuid{}, kSession);
    Check(!cite.second.ok() && cite.second.error == TE::database_failed, "and so is a Begin");
    std::printf("LEDGER_COUNT=1\n");
  } else if (scenario == "ledger_measure") {
    Check(facts().ledger_records == 0, "empty at start");
    Mark("append_plain");
    const auto first = add(kConn, Conn(1));
    Check(first.ok(), "first append");
    PrintMeasure("append_plain", first);
    Mark("fill");
    srv::NativeLoopLedgerResult sum;
    for (unsigned i = 1; i < slots; ++i) {
      const auto r = add(Fixed(2000 + i), Conn(i + 1));
      Check(r.ok(), "fill the first page");
      sum.writes += r.writes;
      sum.syncs += r.syncs;
      sum.growth_writes += r.growth_writes;
      sum.growth_syncs += r.growth_syncs;
    }
    PrintMeasure("fill", sum);
    Mark("append_growth");
    const auto grown = add(Fixed(2999), Conn(999));
    Check(grown.ok() && grown.growth_writes >= 2, "the next append grows the chain");
    PrintMeasure("append_growth", grown);
    Mark("end");
  } else if (scenario == "ledger") {
    const auto f0 = facts();
    Check(f0.ledger_records == 0 && f0.ledger_pages == 1 && !f0.ledger_torn_tail && f0.wasted_pages == 0,
          "a created file opens with an empty one-page ledger and no wasted pages");
    const auto before_files = Snapshot(directory);
    // Refusals first: nothing is written (every file byte-identical) and the loop goes on.
    {
      const auto no_conn = add(Fixed(1100), Att(kSession, kConn, p::Uuid{}));
      Check(!no_conn.ok() && no_conn.error == TE::writer_refused && no_conn.ledger_error == "ledger_unknown_session", "attachment of an unknown session");
      auto bad = Conn(1);
      bad.transport = static_cast<pg::LedgerTransport>(0);
      const auto badr = add(Fixed(1101), bad);
      Check(!badr.ok() && badr.ledger_error == "ledger_invalid_record" && badr.record_error == "invalid_enum", "a bad record names the codec's cause");
      Check(Snapshot(directory) == before_files, "refused appends changed no file");
      const auto wrong_thread = loop.AppendLedgerRecordOnLoopThread(Rec(Fixed(1102), Conn(1)));
      Check(!wrong_thread.ok() && wrong_thread.error == TE::invalid_request, "the on-loop form is refused off the loop thread");
    }
    // One of every kind, then connection records past the page boundary, then the attachment the header will cite.
    const unsigned total = slots + 3;
    unsigned ordinal = 0;
    const auto expect_ok = [&](const srv::NativeLoopLedgerResult& r, const std::string& what) {
      Check(r.ok(), what + ": " + srv::NativeLoopTransactionErrorName(r.error) + " " + r.ledger_error);
      Check(r.ordinal == ordinal, what + ": the next ordinal");
      const bool boundary = ordinal != 0 && ordinal % slots == 0;
      Check((r.growth_writes > 0) == boundary, what + ": growth exactly at the page boundary");
      Check(r.writes == 1 && r.syncs == 1, what + ": one write and one sync");
      ++ordinal;
    };
    expect_ok(add(kConn, Conn(1)), "connection");
    expect_ok(add(kSession, Sess(bootstrap)), "session");
    expect_ok(add(kAtt, Att(kSession, kConn, p::Uuid{})), "attachment");
    expect_ok(add(kChange, Chg(kSession, 1)), "settings change");
    while (ordinal + 1 < total) expect_ok(add(Fixed(2000 + ordinal), Conn(ordinal + 10)), "connection " + std::to_string(ordinal));
    expect_ok(add(kAtt2, Att(kSession, kConn, kAtt)), "the cited attachment, last");
    const auto dup = add(kConn, Conn(2));
    Check(!dup.ok() && dup.ledger_error == "ledger_duplicate_record" && dup.writes == 0, "a duplicate UUID");
    Check(find(kSession) && find(kSession)->ref == bootstrap && find(kAtt2) && find(kAtt2)->ordinal == total - 1, "lookup");
    Check(facts().wasted_pages == 0, "wasted_pages is not changed by the append path (counted at open)");
    // "Durable before cited" through the loop.
    const auto before_cite = Snapshot(directory);
    const auto unknown = begin(bootstrap, Fixed(9100), kSession);
    Check(!unknown.second.ok() && unknown.second.error == TE::writer_refused && unknown.second.writer_error == "ledger_unknown_attachment",
          "Begin citing an unknown attachment");
    const auto unknown_session = begin(bootstrap, p::Uuid{}, Fixed(9101));
    Check(!unknown_session.second.ok() && unknown_session.second.writer_error == "ledger_unknown_session", "Begin citing an unknown session");
    const auto mismatch = begin(Fixed(9102), kAtt2, kSession);
    Check(!mismatch.second.ok() && mismatch.second.writer_error == "ledger_principal_mismatch", "Begin whose principal is not the session's");
    Check(Snapshot(directory) == before_cite, "every refused Begin wrote nothing (every file byte-identical)");
    const auto cited = begin(bootstrap, kAtt2, kSession);
    Check(cited.second.ok(), "Begin citing appended records: " + std::string(srv::NativeLoopTransactionErrorName(cited.second.error)) + " " + cited.second.writer_error);
    srv::NativeLoopCompleteRequest done;
    done.owner_key = cited.first;
    done.transaction_order = cited.second.begun_order;
    done.completion = srv::NativeLoopCompletion::commit;
    Check(loop.SubmitCompleteWork(done).get().ok(), "its commit");
    std::printf("CITED order=%llu\nLEDGER_COUNT=%u\n", static_cast<unsigned long long>(cited.second.begun_order), total);
    Check(loop.Close().ok, "the loop closes");
    Check(!loop.SubmitAppendLedgerRecord(Rec(Fixed(1200), Conn(1))).get().ok(), "an append after close is refused");
  } else if (scenario == "ledger_reopen_after_fail") {
    // The failed write may have reached the file (the child's write failed before any byte, so it did not) or not:
    // the reopen loads whatever is whole and the ledger is usable again.
    Check(facts().ledger_records == records && facts().ledger_records >= 1, "the reopened ledger holds the whole records only");
    const auto r = add(kSession, Sess(bootstrap));
    Check(r.ok() && r.ordinal == records, "and appends again");
  } else if (scenario == "ledger_reopen" || scenario == "ledger_reopen_torn") {
    const bool torn = scenario == "ledger_reopen_torn";
    const auto f = facts();
    Check(f.ledger_records == records && f.ledger_pages == 2 && f.ledger_torn_tail == torn, "the reopened ledger: records, pages, torn tail");
    Check(f.wasted_pages == 0, "the grown ledger page is not counted as wasted space after a reopen");
    for (const auto& id : {kConn, kSession, kAtt, kAtt2, kChange}) Check(find(id).has_value(), "every record is found after the restart");
    Check(find(kSession)->ref == bootstrap && find(kAtt2)->ordinal == records - 1 && find(kSession)->settings_generation == 1, "with their fields");
    const auto cited = begin(bootstrap, kAtt2, kSession);
    if (!torn) {
      Check(cited.second.ok(), "a header can cite the records after the restart");
    } else {
      // The torn slot was past the last complete record: nothing cited it.
      Check(cited.second.ok(), "the cited records are before the torn slot");
    }
    const auto next = add(Fixed(3000), Conn(77));
    Check(next.ok() && next.ordinal == records, "the next append takes the next ordinal (over the torn slot when there is one)");
    Check(find(Fixed(3000)).has_value(), "and is found");
    std::printf("LEDGER_COUNT=%u\n", records + 1);
  } else {
    Check(false, "a known scenario");
  }
  for (const auto& entry : fs::directory_iterator(directory))
    Check(entry.path().filename().string().find(".sb.") == std::string::npos, "no .sb.* file exists");
  std::printf("LEDGER_LOOP_OK checks=%d scenario=%s\n", checks, scenario.c_str());
  return 0;
}

// ---- driver ---------------------------------------------------------------------------------------------------------
std::string Capture(const std::string& command, int* status) {
  std::string output;
  FILE* pipe = ::popen((command + " 2>&1").c_str(), "r");
  Check(pipe != nullptr, "popen");
  char chunk[512];
  std::size_t got;
  while ((got = std::fread(chunk, 1, sizeof chunk, pipe)) > 0) output.append(chunk, got);
  const int raw = ::pclose(pipe);
  *status = raw == -1 ? -1 : (WIFEXITED(raw) ? WEXITSTATUS(raw) : 128 + WTERMSIG(raw));
  return output;
}
std::string Field(const std::string& text, std::string_view key) {
  const auto at = text.find(std::string(key) + "=");
  if (at == std::string::npos) return {};
  const auto from = at + key.size() + 1;
  const auto to = text.find_first_of(" \n", from);
  return text.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

struct Roots {
  d::FilespacePageZeroDecodeResult zero;
  d::FilespaceRootReference ledger, state;
};
Roots ReadRoots(const std::vector<p::byte>& image, std::uint32_t page_size) {
  Roots r;
  r.zero = d::DecodeFilespacePageZero(image.data(), page_size);
  Check(r.zero.ok(), "page zero decodes");
  for (const auto& root : r.zero.record->roots) {
    if (root.kind == 25) r.ledger = root;
    if (root.kind == 4) r.state = root;
  }
  Check(r.ledger.kind == 25 && r.state.kind == 4, "page zero names the ledger and the TIP");
  return r;
}
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
// The complete records of the file's ledger chain, in order.
std::vector<pg::SessionLedgerRecord> WalkLedger(const std::vector<p::byte>& image, std::uint32_t page_size, std::vector<p::u64>* pages = nullptr) {
  const auto roots = ReadRoots(image, page_size);
  std::vector<pg::SessionLedgerRecord> out;
  p::u64 page = roots.ledger.page_number;
  while (page != 0) {
    if (pages) pages->push_back(page);
    const auto view = pg::DecodeSessionLedgerPage(std::span<const p::byte>(image).subspan(page * page_size, page_size));
    Check(view.ok(), "a ledger page decodes");
    for (std::uint32_t s = 0; s < view.view->used; ++s) out.push_back(*pg::SessionLedgerRecordAt(*view.view, s));
    page = view.view->next_page;
  }
  return out;
}

// `pages <path>`: the ledger chain's pages and the allocation map's pages (for the strace half).
int PagesMode(const fs::path& path, std::uint32_t page_size) {
  const auto image = ReadAll(path);
  std::vector<p::u64> pages;
  (void)WalkLedger(image, page_size, &pages);
  std::ostringstream out;
  out << "LEDGER_PAGES page_size=" << page_size << " ledger=";
  for (std::size_t i = 0; i < pages.size(); ++i) out << (i ? "," : "") << pages[i];
  out << " map=";
  bool first = true;
  for (p::u64 n = 1; (n + 1) * page_size <= image.size(); ++n) {
    if (!pg::IsAllocationMapInplaceImage(std::span<const p::byte>(image).subspan(n * page_size, page_size))) break;
    out << (first ? "" : ",") << n;
    first = false;
  }
  std::cout << out.str() << std::endl;
  return 0;
}

void OneSize(const std::string& self, const fs::path& root, std::uint32_t page_size) {
  const std::string tag = "[" + std::to_string(page_size) + "] ";
  const auto dir = root / ("ps" + std::to_string(page_size));
  fs::create_directory(dir);
  int status = 0;
  std::string ids;
  const auto make = [&](const std::string& name) {
    const auto sub = dir / name;
    fs::create_directory(sub);
    const auto path = sub / "ledger.sdb";
    const auto created = Capture("'" + self + "' create '" + path.string() + "' " + std::to_string(page_size), &status);
    Check(status == 0 && created.starts_with("CREATED "), tag + name + ": create: " + created);
    ids = Field(created, "database") + " " + Field(created, "bootstrap") + " " + std::to_string(page_size);
    return path;
  };
  const auto serve = [&](const fs::path& path, const std::string& scenario, const std::string& extra = "") {
    const auto start = std::chrono::steady_clock::now();
    const auto out = Capture("'" + self + "' serve '" + path.string() + "' " + scenario + " " + ids + " " + extra, &status);
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    Check(status == 0 && out.find("LEDGER_LOOP_OK") != std::string::npos,
          tag + scenario + ": serve child (exit " + std::to_string(status) + "): " + out.substr(0, 3000));
    Check(out.find(kBootstrapPassword) == std::string::npos, tag + scenario + ": no test password in any child output");
    std::printf("%s%s: %.1f s\n", tag.c_str(), scenario.c_str(), seconds);
    std::fflush(stdout);
    return out;
  };
  const auto copy_of = [&](const fs::path& from, const std::string& name) {
    const auto sub = dir / name;
    fs::create_directory(sub);
    const auto to = sub / "ledger.sdb";
    fs::copy_file(from, to);
    return to;
  };
  const unsigned slots = pg::SessionLedgerSlotsPerPage(page_size);

  // ---- the whole contract on one file, then the file as the engine left it ----
  const auto path = make("main");
  {
    const auto created = ReadAll(path);
    Check(WalkLedger(created, page_size).empty(), tag + "the created file's ledger chain is empty");
  }
  const auto out = serve(path, "ledger");
  const unsigned total = static_cast<unsigned>(std::stoul(Field(out, "LEDGER_COUNT")));
  const auto cited_order = std::stoull(Field(out, "CITED order"));
  Check(total == slots + 3, tag + "the child appended a page and three more records");
  const auto image = ReadAll(path);
  std::vector<p::u64> pages;
  const auto records = WalkLedger(image, page_size, &pages);
  Check(records.size() == total && pages.size() == 2, tag + "the file holds every record on two pages");
  for (unsigned i = 0; i < total; ++i) Check(records[i].ordinal == i, tag + "ordinals continue across the page boundary");
  Check(records[0].record_uuid == kConn && records[1].record_uuid == kSession && records[2].record_uuid == kAtt &&
            records[3].record_uuid == kChange && records.back().record_uuid == kAtt2,
        tag + "the four kinds, in the order appended");
  Check(std::holds_alternative<pg::LedgerConnectionRecord>(records[0].body) && std::holds_alternative<pg::LedgerSessionRecord>(records[1].body) &&
            std::holds_alternative<pg::LedgerAttachmentRecord>(records[2].body) && std::holds_alternative<pg::LedgerSettingsChangeRecord>(records[3].body),
        tag + "kinds read back");
  {
    // The cited transaction's header on disk holds +64/+80/+96 as the loop was told.
    const auto roots = ReadRoots(image, page_size);
    pg::TransactionWriterBinding tb;
    tb.database_uuid = roots.zero.record->bootstrap.database_uuid;
    tb.state_root = roots.state;
    BytesSink sink(image);
    const auto chains = pg::ReadTransactionChainsThroughSink(sink, tb);
    Check(chains.ok(), tag + "the TIP chains read");
    const auto loc = pg::LocateTransactionHeaderSlot(page_size, cited_order);
    const auto view = pg::DecodeTransactionHeaderPage(std::span<const p::byte>(chains.header_images).subspan(loc->sequence * page_size, page_size));
    const auto h = pg::TransactionHeaderAt(*view.view, loc->slot);
    Check(h && h->session_uuid == kSession && h->attachment_uuid == kAtt2 && h->final_state == pg::TipStateCode::committed,
          tag + "the cited header holds +96 and +80 and is committed");
  }
  // ---- a real restart: the server is started again on the same file ----
  const auto reopen_file = copy_of(path, "reopen");
  serve(reopen_file, "ledger_reopen", std::to_string(total));
  {
    const auto after = ReadAll(reopen_file);
    const auto again = WalkLedger(after, page_size);
    Check(again.size() == total + 1 && std::equal(records.begin(), records.end(), again.begin()), tag + "after the restart the old records are byte-for-byte intact and one more was appended");
  }
  // ---- a torn tail: a prefix of a record after the last complete one ----
  {
    const auto torn_file = copy_of(path, "torn");
    auto bytes = ReadAll(torn_file);
    const std::uint32_t used_on_last = total - slots;
    Check(used_on_last + 1 <= slots, tag + "room for a torn slot");
    std::array<p::byte, 256> encoded{};
    auto next = Rec(Fixed(3000), Conn(77));
    next.database_uuid = records[0].database_uuid;
    next.ordinal = total;
    Check(pg::EncodeSessionLedgerRecord(next, encoded) == pg::SessionLedgerRecordError::none, tag + "encode a torn candidate");
    const auto at = pages.back() * page_size + 512 + 256ull * used_on_last;
    std::copy_n(encoded.begin(), 100, bytes.begin() + at);
    WriteAll(torn_file, bytes);
    serve(torn_file, "ledger_reopen_torn", std::to_string(total));
    const auto after = WalkLedger(ReadAll(torn_file), page_size);
    Check(after.size() == total + 1 && after.back().record_uuid == Fixed(3000), tag + "the torn slot was overwritten by the next append");
  }
  // ---- refusals at open ----
  const auto refuse = [&](const std::string& name, const std::vector<p::byte>& bytes, const std::string& code, const std::string& finding) {
    const auto file = copy_of(path, name);
    WriteAll(file, bytes);
    const auto before = Snapshot(file.parent_path());
    const auto refused = serve(file, "refused", code);
    Check(refused.find("ledger_finding=" + finding + " ") != std::string::npos, tag + name + ": the finding is " + finding + ": " + refused.substr(0, 1500));
    Check(Snapshot(file.parent_path()) == before, tag + name + ": the refused open changed nothing");
    std::printf("%s%s: %s\n", tag.c_str(), name.c_str(), refused.substr(refused.find("REFUSED"), 200).c_str());
  };
  const std::string missing = "DATABASE.OPEN_VERIFICATION_LEDGER_MISSING", invalid = "DATABASE.OPEN_VERIFICATION_LEDGER_INVALID";
  {
    auto bytes = image;
    auto zero = ReadRoots(bytes, page_size).zero;
    auto& roots = zero.record->roots;
    roots.erase(std::remove_if(roots.begin(), roots.end(), [](const auto& r) { return r.kind == 25; }), roots.end());
    const auto encoded = d::EncodeFilespacePageZero(*zero.record);
    Check(encoded.ok(), tag + "page zero without the ledger root encodes");
    std::copy(encoded.bytes->begin(), encoded.bytes->end(), bytes.begin());
    refuse("missing_root", bytes, missing, "none");
  }
  {
    auto bytes = image;  // a record after an empty slot on the last page
    bytes[pages.back() * page_size + 512 + 256ull * (slots - 1)] = 1;
    refuse("corrupt_slot", bytes, invalid, "chain_unreadable");
  }
  {
    auto bytes = image;  // the first page linked to the TIP page: not a ledger page
    const auto roots = ReadRoots(bytes, page_size);
    const auto at = pages.front() * page_size + 256;
    for (int i = 0; i < 8; ++i) bytes[at + i] = static_cast<p::byte>(roots.state.page_number >> (8 * i));
    refuse("mislinked", bytes, invalid, "chain_unreadable");
  }
  {
    auto bytes = image;  // the cited record (the last one) lost: zeroed
    const auto at = pages.back() * page_size + 512 + 256ull * (total - slots - 1);
    std::fill(bytes.begin() + at, bytes.begin() + at + 256, p::byte{0});
    refuse("cited_record_zeroed", bytes, invalid, "header_citation");
  }
  {
    auto bytes = image;  // the cited record torn: a prefix of its bytes, the rest zero
    const auto at = pages.back() * page_size + 512 + 256ull * (total - slots - 1);
    std::fill(bytes.begin() + at + 100, bytes.begin() + at + 256, p::byte{0});
    refuse("cited_record_torn", bytes, invalid, "header_citation");
  }
  // ---- a failed write, then the reopen: the record is either whole or never visible, and the file opens ----
  {
    const auto file = make("writefail");
    serve(file, "ledger_writefail");
    const auto after = WalkLedger(ReadAll(file), page_size);
    Check(!after.empty() && after.front().record_uuid == kConn && after.size() <= 2, tag + "writefail: the first record is intact");
    serve(file, "ledger_reopen_after_fail", std::to_string(after.size()));
  }
  // ---- restricted and failed (the S4d fixtures: a committing slot / an active slot) ----
  const auto native_serving = std::string(SB_NATIVE_SERVING_TEST_BIN);
  {
    const auto file = make("restricted");
    Check(Capture("'" + native_serving + "' fixture '" + file.string() + "' committing", &status).find("FIXTURE ok") != std::string::npos && status == 0,
          tag + "the restricted fixture");
    const auto before = ReadAll(file);
    serve(file, "ledger_restricted");
    Check(ReadAll(file) == before, tag + "restricted: the file is byte-identical");
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "create") return CreateMode(argv[2], static_cast<std::uint32_t>(std::stoul(argv[3])));
  if (argc == 4 && std::string(argv[1]) == "pages") return PagesMode(argv[2], static_cast<std::uint32_t>(std::stoul(argv[3])));
  if (argc >= 7 && std::string(argv[1]) == "serve") {
    const std::string scenario = argv[3];
    const std::string extra = argc >= 8 ? argv[7] : "";
    return ServeMode(argv[2], scenario == "refused" ? "refused" : scenario, FromHex(argv[4]), FromHex(argv[5]),
                     static_cast<std::uint32_t>(std::stoul(argv[6])), scenario == "refused" ? extra : "",
                     scenario == "refused" || extra.empty() ? 0 : static_cast<unsigned>(std::stoul(extra)));
  }
  if (argc == 1 || (argc == 3 && std::string(argv[1]) == "size")) {
    const std::string self = fs::read_symlink("/proc/self/exe").string();
    scratchbird::tests::OwnedTempDirectory artifacts;
    if (argc == 3) OneSize(self, artifacts.path(), static_cast<std::uint32_t>(std::stoul(argv[2])));
    else
      for (const std::uint32_t size : {8192u, 16384u, 32768u, 65536u, 131072u}) OneSize(self, artifacts.path(), size);
    artifacts.Cleanup();
    std::printf("native_session_ledger_loop: %d driver checks passed\n", checks);
    return 0;
  }
  return 2;
}
