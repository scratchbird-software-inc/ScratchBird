// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_NATIVE_LOOP_TRANSACTION_TEST (thin-slice S4d, plan 40; owner Q305, Q309, Q315, 2026-11-19/21).
// The database loop's transaction queue (BeginWork, CompleteWork, CompleteAndBeginWork) with its ownership table, and
// the engine-owned read-transaction scope (RunInEngineReadTransaction) the native login's credential and names reads
// will run in.
//
// Every case runs against a PRODUCTION-created database (EngineCreateLifecycle through the native create fixture),
// served by StartHostedEngine (the real server open path); no mock stands in for the loop, the writer or the file.
// One served child per (page size, scenario), each its own process (one StartHostedEngine per process gate bind).
// Modes (a child process each):
//   create <path> <page_size>        engine-API native create; prints the identities
//   serve <path> <scenario> <ids>    scenario: serving | restricted | failed | writefail | abandon_close |
//                                    abandon_exit | reopen | measure. Prints EXPECT <order> <state> lines for every
//                                    transaction it touched, and LOOP_TXN_OK on success
//   pages <path>                     offline: the TIP state-chain / header-chain pages and Next of the file
// With no argument the driver runs every size; "size <bytes>" runs one. The strace half is
// native_loop_transaction_strace.py (the `measure` scenario).

#include "engine/internal_api/security/authentication_api.hpp"
#include "hash_digest.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "local_password_verifier.hpp"
#include "memory.hpp"
#include "memory_policy_config.hpp"
#include "filespace_page_zero.hpp"
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
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
namespace p = scratchbird::core::platform;
namespace d = scratchbird::storage::disk;
namespace pg = scratchbird::storage::page;
namespace srv = scratchbird::server;
namespace mem = scratchbird::core::memory;
namespace fs = std::filesystem;
using TE = srv::NativeLoopTransactionError;

constexpr std::string_view kBootstrapPassword = "S4d-Bootstrap-Pw-5b19e0c47a";
constexpr std::string_view kWrongPassword = "S4d-Wrong-Pw-91d03c7ee2";

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

std::vector<p::byte> ReadAll(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(in)), {});
  return std::vector<p::byte>(reinterpret_cast<const p::byte*>(bytes.data()), reinterpret_cast<const p::byte*>(bytes.data()) + bytes.size());
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

// ---- create --------------------------------------------------------------------------------------------------
int CreateMode(const fs::path& path, std::uint32_t page_size) {
  std::string verifier;
  if (!scratchbird::core::crypto::DeriveLocalPasswordVerifier(kBootstrapPassword, &verifier)) return 1;
  scratchbird::tests::NativeFixtureCreateRequest request;
  request.path = path;
  request.page_size = page_size;
  request.database_name = "native_loop_transaction";
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

// ---- the file's transaction inventory, read offline through the writer's own chain reader -------------------------
// A read-only sink over the file's bytes (no lock, nothing written): what the production chain reader needs.
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

struct Tip {
  std::uint32_t page_size = 0;
  pg::TipCounters counters;
  std::map<p::u64, pg::TipStateWord> words;
  std::map<p::u64, pg::TransactionHeaderRecord> headers;
  std::vector<p::u64> state_pages, header_pages;
};
// `page_size_hint` 0: the file is opened read-only through the device (only when no server holds it). Non-zero: page
// zero is decoded from the file's bytes (no device, no ownership), for a read while the file is served.
Tip ReadTip(const fs::path& path, std::uint32_t page_size_hint = 0) {
  const auto image = ReadAll(path);
  d::FilespacePageZeroDecodeResult zero;
  if (page_size_hint != 0) {
    Check(image.size() >= page_size_hint, "the file holds page zero");
    zero = d::DecodeFilespacePageZero(image.data(), page_size_hint);
  } else {
    d::FileDevice device;
    Check(device.Open(path.string(), d::FileOpenMode::open_existing_read_only, d::DatabaseFileOwnership::native_data_file).ok(),
          "the offline read opens the file read-only");
    zero = d::ReadFilespacePageZeroFromOpenDevice(device);
    Check(device.Close().ok(), "close");
  }
  Check(zero.ok(), "page zero reads");
  pg::TransactionWriterBinding binding;
  binding.database_uuid = zero.record->bootstrap.database_uuid;
  bool found = false;
  for (const auto& root : zero.record->roots)
    if (root.kind == 4) {
      binding.state_root = root;
      found = true;
    }
  Check(found, "page zero names the transaction-state chain");
  BytesSink sink(image);
  const auto chains = pg::ReadTransactionChainsThroughSink(sink, binding);
  Check(chains.error == pg::TransactionWriterError::none, "the chains read and check");
  Tip tip;
  tip.page_size = chains.page_size;
  tip.counters = chains.counters;
  tip.state_pages = chains.state_pages;
  tip.header_pages = chains.header_pages;
  for (std::size_t i = 0; i < chains.state_pages.size(); ++i) {
    const auto page = pg::DecodeTransactionStatePage(std::span<const p::byte>(chains.state_images).subspan(i * chains.page_size, chains.page_size));
    Check(page.ok(), "a state page decodes");
    for (std::uint32_t slot = 0; slot < page.view->slots; ++slot) {
      const auto word = pg::TipWordAt(*page.view, slot);
      Check(word.has_value(), "every word decodes");
      if (word->state != pg::TipStateCode::unused) tip.words[page.view->first_number + slot] = *word;
    }
  }
  for (std::size_t i = 0; i < chains.header_pages.size(); ++i) {
    const auto page = pg::DecodeTransactionHeaderPage(std::span<const p::byte>(chains.header_images).subspan(i * chains.page_size, chains.page_size));
    Check(page.ok(), "a header page decodes");
    for (std::uint32_t slot = 0; slot < page.view->slots; ++slot)
      if (const auto record = pg::TransactionHeaderAt(*page.view, slot)) tip.headers[page.view->first_number + slot] = *record;
  }
  return tip;
}

int PagesMode(const fs::path& path) {
  const auto tip = ReadTip(path);
  std::ostringstream out;
  out << "CHAIN_PAGES page_size=" << tip.page_size << " next=" << tip.counters.next_transaction << " state=";
  for (std::size_t i = 0; i < tip.state_pages.size(); ++i) out << (i ? "," : "") << tip.state_pages[i];
  out << " header=";
  for (std::size_t i = 0; i < tip.header_pages.size(); ++i) out << (i ? "," : "") << tip.header_pages[i];
  std::cout << out.str() << std::endl;
  return 0;
}

// Page numbers whose bytes differ between two images of the same file (the images may differ in length).
std::set<p::u64> ChangedPages(const std::vector<p::byte>& before, const std::vector<p::byte>& after, std::uint32_t page_size) {
  std::set<p::u64> changed;
  const auto pages = std::max(before.size(), after.size()) / page_size;
  for (p::u64 page = 0; page < pages; ++page) {
    const auto lo = page * page_size, hi = lo + page_size;
    const bool in_before = hi <= before.size(), in_after = hi <= after.size();
    if (in_before != in_after || (in_before && !std::equal(before.begin() + lo, before.begin() + hi, after.begin() + lo))) changed.insert(page);
  }
  return changed;
}

// ---- the protected password ------------------------------------------------------------------------------------
struct PasswordFactory {
  mem::MemoryManager manager{mem::DefaultLocalEngineMemoryPolicy()};
  mem::MemoryTag tag;
  explicit PasswordFactory(const p::Uuid& database) {
    tag.category = mem::MemoryCategory::core_runtime;
    tag.purpose = "native loop transaction test password";
    tag.callsite = "native_loop_transaction_test";
    tag.binary_ownership[mem::MemoryBinaryScopeKind::database] = database.bytes;
    tag.binary_ownership[mem::MemoryBinaryScopeKind::owner] = database.bytes;
    tag.binary_ownership[mem::MemoryBinaryScopeKind::context] = database.bytes;
  }
  mem::ScopedProtectedBuffer Make(std::string_view bytes) {
    mem::ProtectedMemoryRequest request;
    request.bytes = std::max<std::size_t>(bytes.size(), 1);
    request.tag = tag;
    request.material_class = "native loop transaction test password";
    request.platform_policy = mem::ProtectedMemoryPlatformPolicy::require_lock_and_no_dump;
    auto allocated = manager.AllocateProtected(request);
    Check(allocated.ok(), "protected memory for the test password");
    std::memcpy(allocated.buffer.data(), bytes.data(), bytes.size());
    return std::move(allocated.buffer);
  }
  srv::NativeLoopLoginRequest Request(const p::Uuid& principal, std::string_view password) {
    srv::NativeLoopLoginRequest request;
    request.principal_uuid = principal;
    request.password = Make(password);
    request.password_length = password.size();
    return request;
  }
};

// ---- fault injection through the loop's transaction-sink seam --------------------------------------------------
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
class FailFirstRestartWriteSink final : public d::CarefulWriteSink {
 public:
  explicit FailFirstRestartWriteSink(d::CarefulWriteSink& inner) : inner_(inner) {}
  d::BoundedIoResult WriteAt(p::u64 offset, std::span<const p::byte> bytes) override {
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
};

// ---- the served child ---------------------------------------------------------------------------------------------
std::map<p::u64, std::string> g_expect;  // order -> final state name the file must show after the child
std::mutex g_expect_mutex;
void Expect(p::u64 order, const char* state) {
  std::lock_guard<std::mutex> guard(g_expect_mutex);
  g_expect[order] = state;
}
double Ms(std::chrono::steady_clock::time_point from) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
}
void Mark(const std::string& name) {
  std::fprintf(stderr, "MARK %s\n", name.c_str());
  std::fflush(stderr);
}

constexpr std::uint8_t kReadCommittedStatement = 2;  // IsolationProfileKind::read_committed_statement

struct Child {
  srv::NativeDatabaseLoop* loop = nullptr;
  p::Uuid database, bootstrap;
  std::unique_ptr<PasswordFactory> passwords;
  fs::path path, directory;

  srv::NativeLoopTransactionResult Begin(const p::Uuid& owner, bool read_only) {
    srv::NativeLoopBeginRequest request;
    request.owner_key = owner;
    request.read_only = read_only;
    request.isolation_profile = kReadCommittedStatement;
    return loop->SubmitBeginWork(request).get();
  }
  srv::NativeLoopTransactionResult Complete(const p::Uuid& owner, p::u64 order, srv::NativeLoopCompletion how) {
    srv::NativeLoopCompleteRequest request;
    request.owner_key = owner;
    request.transaction_order = order;
    request.completion = how;
    return loop->SubmitCompleteWork(request).get();
  }
  srv::NativeLoopTransactionResult CompleteAndBegin(const p::Uuid& owner, p::u64 order, srv::NativeLoopCompletion how, bool read_only = false) {
    srv::NativeLoopCompleteAndBeginRequest request;
    request.owner_key = owner;
    request.transaction_order = order;
    request.completion = how;
    request.read_only = read_only;
    request.isolation_profile = kReadCommittedStatement;
    return loop->SubmitCompleteAndBeginWork(request).get();
  }
  std::vector<p::u64> Table() {
    std::vector<p::u64> orders;
    Check(loop->RunOnLoopThreadForTest([&] { orders = loop->OpenTransactionOrdersOnLoopThread(); }), "the loop ran the table probe");
    return orders;
  }
};

// The scope's durable variant (an engine-owned read-only transaction: header, TIP slot, a number). The default
// RunInEngineReadTransaction is the snapshot point (owner ruling 2026-11-21): no header, no write, no number.
srv::NativeLoopReadScopeResult DurableScope(srv::NativeDatabaseLoop& loop,
                                            std::function<void(const srv::NativeLoopReadTransaction&)> work) {
  return loop.RunInEngineReadTransaction(std::move(work), srv::NativeLoopReadScopeMode::durable_transaction);
}

void ExpectError(const srv::NativeLoopTransactionResult& r, TE expected, const std::string& what) {
  Check(!r.ok() && r.error == expected,
        what + ": expected " + srv::NativeLoopTransactionErrorName(expected) + ", got " + srv::NativeLoopTransactionErrorName(r.error));
  Check(std::string(srv::NativeLoopTransactionErrorCode(r.error)).starts_with("ENGINE.NATIVE."), what + ": a stable code");
}

void PrintMeasure(const char* op, const srv::NativeLoopTransactionResult& r, p::u64 order) {
  std::printf("MEASURE op=%s order=%llu writes=%u syncs=%u growth_writes=%u growth_syncs=%u\n", op,
              static_cast<unsigned long long>(order), r.writes, r.syncs, r.growth_writes, r.growth_syncs);
  std::fflush(stdout);
}

int ServeMode(const fs::path& path, const std::string& scenario, const p::Uuid& database, const p::Uuid& bootstrap,
              std::uint32_t page_size) {
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
  if (scenario == "failed")
    srv::SetNativeRestartSinkWrapperForTest([](d::CarefulWriteSink& inner) -> std::unique_ptr<d::CarefulWriteSink> {
      return std::make_unique<FailFirstRestartWriteSink>(inner);
    });
  auto started = srv::StartHostedEngine(config, nullptr, &crypto);
  if (scenario == "failed") {
    Check(!started.ok() && started.diagnostics.front().code == "DATABASE.RESTART_WRITE_FAILED", "the seam produced a REAL failed Restart");
  } else {
    Check(started.ok(), "the server serves the file: " + (started.diagnostics.empty() ? std::string() : started.diagnostics.front().code));
  }
  Check(!started.state.database_runtimes.empty() && started.state.database_runtimes.front() &&
            started.state.database_runtimes.front()->native_loop,
        "the hosted state holds the native loop");
  Child c;
  c.loop = started.state.database_runtimes.front()->native_loop.get();
  c.database = database;
  c.bootstrap = bootstrap;
  c.passwords = std::make_unique<PasswordFactory>(database);
  c.path = path;
  c.directory = directory;
  auto& loop = *c.loop;
  Check(!loop.on_loop_thread(), "the test is not on the loop thread");
  const auto before_files = Snapshot(directory);

  // ---- failed: every transaction request and the scope refuse; nothing is written ----
  if (scenario == "failed") {
    Check(loop.serving_state() == srv::NativeServingState::failed, "the database is failed");
    const auto k = Fresh();
    ExpectError(c.Begin(k, true), TE::database_failed, "begin on a failed database");
    ExpectError(c.Complete(k, 2, srv::NativeLoopCompletion::commit), TE::database_failed, "complete on a failed database");
    ExpectError(c.CompleteAndBegin(k, 2, srv::NativeLoopCompletion::commit), TE::database_failed, "complete-and-begin on a failed database");
    bool ran = false;
    const auto scope = DurableScope(loop, [&](const srv::NativeLoopReadTransaction&) { ran = true; });
    ExpectError(scope.begin, TE::database_failed, "the read scope on a failed database");
    Check(!ran && !scope.work_ran && !scope.ok(), "the scope's work did not run");
    {
      bool ran2 = false;
      const auto snap = loop.RunInEngineReadTransaction([&](const srv::NativeLoopReadTransaction&) { ran2 = true; },
                                                        srv::NativeLoopReadScopeMode::snapshot_point);
      ExpectError(snap.begin, TE::database_failed, "the snapshot-point scope on a failed database");
      Check(!ran2, "no work ran");
    }
    Check(std::string(srv::NativeLoopTransactionErrorCode(TE::database_failed)) !=
              srv::NativeLoopTransactionErrorCode(TE::database_not_serving),
          "failed and not-serving are distinct codes");
    Check(Snapshot(directory) == before_files, "the failed database's files are unchanged by every refused request");
  }

  // ---- restricted: transactions refused with the Q299 code; the S4a read is still allowed (Q309) ----
  else if (scenario == "restricted") {
    Check(loop.serving_state() == srv::NativeServingState::restricted, "the database is restricted");
    const auto k = Fresh();
    const auto begin = c.Begin(k, true);
    ExpectError(begin, TE::database_restricted, "begin on a restricted database");
    Check(std::string(srv::NativeLoopTransactionErrorCode(begin.error)) == "ENGINE.NATIVE.DATABASE_RESTRICTED_READ_ONLY",
          "the Q299 code, the same one the native handle uses");
    ExpectError(c.Complete(k, 2, srv::NativeLoopCompletion::commit), TE::database_restricted, "complete on a restricted database");
    ExpectError(c.CompleteAndBegin(k, 2, srv::NativeLoopCompletion::rollback), TE::database_restricted, "complete-and-begin on a restricted database");
    bool ran = false;
    const auto scope = DurableScope(loop, [&](const srv::NativeLoopReadTransaction&) { ran = true; });
    ExpectError(scope.begin, TE::database_restricted, "the read scope on a restricted database");
    Check(!ran && !scope.work_ran, "the scope's work did not run on a restricted database");
    {
      // The snapshot-point body (no header, no write) is a read: a restricted database serves it.
      bool login_ok = false;
      const auto snap = loop.RunInEngineReadTransaction(
          [&](const srv::NativeLoopReadTransaction& t) {
            login_ok = loop.ReadLoginMaterialOnLoopThread(c.passwords->Request(c.bootstrap, kBootstrapPassword)).ok() &&
                       t.snapshot_next_transaction >= 2 && t.order == 0;
          },
          srv::NativeLoopReadScopeMode::snapshot_point);
      Check(snap.ok() && login_ok, "the snapshot-point scope runs on a restricted database (reads only)");
    }
    // Q309 says login is allowed on a restricted database; the owner's 2026-11-21 decision runs its reads in a
    // transaction; a restricted database refuses every transaction. The S4a read itself (no transaction) still works:
    const auto read = loop.SubmitReadLoginMaterial(c.passwords->Request(bootstrap, kBootstrapPassword)).get();
    Check(read.ok(), "the S4a read (no transaction) still authenticates on a restricted database");
    Check(Snapshot(directory) == before_files, "the restricted database's files are unchanged by every request");
  }

  // ---- writefail: a real write failure inside Begin ----
  else if (scenario == "writefail") {
    Check(loop.serving_state() == srv::NativeServingState::serving, "serving before the fault");
    const auto k = Fresh();
    const auto good = c.Begin(k, false);
    Check(good.ok(), "a begin before the fault");
    Expect(good.begun_order, "committed");
    Check(c.Complete(k, good.begun_order, srv::NativeLoopCompletion::commit).ok(), "its commit");
    srv::SetNativeTransactionSinkWrapperForTest([](d::CarefulWriteSink& inner) -> std::unique_ptr<d::CarefulWriteSink> {
      return std::make_unique<FailAfterWritesSink>(inner, 1);  // W1 (Next := n + 1) reaches the file; W2 (the header) fails
    });
    const auto failed = c.Begin(k, false);
    ExpectError(failed, TE::writer_failed, "a begin whose second write fails");
    Check(failed.writes == 1 && !failed.outcome_uncertain, "exactly one write reached the file");
    srv::SetNativeTransactionSinkWrapperForTest({});
    Check(loop.serving_state() == srv::NativeServingState::failed, "the database became failed (requires reopen)");
    ExpectError(c.Begin(k, true), TE::database_failed, "a begin after the writer failed");
    ExpectError(c.Complete(k, good.begun_order, srv::NativeLoopCompletion::commit), TE::database_failed, "a complete after the writer failed");
    bool ran = false;
    const auto scope = DurableScope(loop, [&](const srv::NativeLoopReadTransaction&) { ran = true; });
    ExpectError(scope.begin, TE::database_failed, "the read scope after the writer failed");
    Check(!ran, "no work ran");
    const auto read = loop.SubmitReadLoginMaterial(c.passwords->Request(bootstrap, kBootstrapPassword)).get();
    Check(!read.ok() && read.error == srv::NativeLoginMaterialError::database_failed, "the S4a read is refused too: the database is failed");
    // The order the failed begin took: Next - 1; its slot is unused below Next (Restart marks it dead at reopen).
    std::printf("FAILED_BEGIN_ORDER_FOLLOWS=%llu\n", static_cast<unsigned long long>(good.begun_order));
  }

  // ---- abandon: transactions left open, then the loop closes (graceful) or the process exits (forced) ----
  else if (scenario == "abandon_close" || scenario == "abandon_exit") {
    const auto k = Fresh();
    const auto ro = c.Begin(k, true), rw = c.Begin(k, false);
    Check(ro.ok() && rw.ok() && rw.begun_order == ro.begun_order + 1, "two begun transactions");
    Expect(ro.begun_order, "read_only_active");
    Expect(rw.begun_order, "active");
    Check(c.Table().size() == 2, "the table holds both");
    if (scenario == "abandon_exit") {
      for (const auto& [order, state] : g_expect) std::printf("EXPECT %llu %s\n", static_cast<unsigned long long>(order), state.c_str());
      std::printf("LOOP_TXN_OK checks=%d scenario=%s\n", checks, scenario.c_str());
      std::fflush(stdout);
      std::_Exit(0);  // forced: no close, no completion, no flush of anything the engine holds
    }
    Check(loop.Close().ok, "the loop closes with two transactions open (nothing completes them)");
  }

  // ---- reopen: the next open after an abandon or a failed write ----
  else if (scenario == "reopen") {
    Check(loop.serving_state() == srv::NativeServingState::serving, "reopened serving");
    const auto k = Fresh();
    const auto b = c.Begin(k, false);
    Check(b.ok(), "a begin after the reopen");
    Check(c.Complete(k, b.begun_order, srv::NativeLoopCompletion::commit).ok(), "its commit");
    Expect(b.begun_order, "committed");
    std::printf("REOPEN_FIRST_ORDER=%llu\n", static_cast<unsigned long long>(b.begun_order));
  }

  // ---- measure: a deterministic sequence, each region marked on stderr for the strace half ----
  else if (scenario == "measure") {
    const auto k = Fresh();
    Mark("scope_login_names");
    srv::NativeLoopReadScopeResult scope = DurableScope(loop, [&](const srv::NativeLoopReadTransaction&) {
      const auto login = loop.ReadLoginMaterialOnLoopThread(c.passwords->Request(c.bootstrap, kBootstrapPassword));
      const auto names = loop.ReadNamesSnapshotOnLoopThread();
      Check(login.ok() && names.ok(), "the reads inside the scope");
    });
    Check(scope.ok(), "the scope ran");
    PrintMeasure("scope_login_names_begin", scope.begin, scope.begin.begun_order);
    PrintMeasure("scope_login_names_complete", scope.complete, scope.complete.completed_order);
    Expect(scope.begin.begun_order, "committed");
    Mark("scope_snapshot");
    srv::NativeLoopReadScopeResult snapshot_scope = loop.RunInEngineReadTransaction(
        [&](const srv::NativeLoopReadTransaction&) {
          const auto login = loop.ReadLoginMaterialOnLoopThread(c.passwords->Request(c.bootstrap, kBootstrapPassword));
          const auto names = loop.ReadNamesSnapshotOnLoopThread();
          Check(login.ok() && names.ok(), "the reads inside the snapshot scope");
        },
        srv::NativeLoopReadScopeMode::snapshot_point);
    Check(snapshot_scope.ok(), "the snapshot scope ran");
    PrintMeasure("scope_snapshot", snapshot_scope.begin, 0);
    Mark("begin_ro");
    const auto ro = c.Begin(k, true);
    Check(ro.ok(), "begin ro");
    PrintMeasure("begin_ro", ro, ro.begun_order);
    Mark("commit_ro");
    const auto cro = c.Complete(k, ro.begun_order, srv::NativeLoopCompletion::commit);
    Check(cro.ok(), "commit ro");
    PrintMeasure("commit_ro", cro, ro.begun_order);
    Expect(ro.begun_order, "committed");
    Mark("begin_rw");
    const auto rw = c.Begin(k, false);
    Check(rw.ok(), "begin rw");
    PrintMeasure("begin_rw", rw, rw.begun_order);
    Mark("cab_commit");
    const auto cab = c.CompleteAndBegin(k, rw.begun_order, srv::NativeLoopCompletion::commit);
    Check(cab.ok(), "complete-and-begin");
    PrintMeasure("cab_commit", cab, rw.begun_order);
    Expect(rw.begun_order, "committed");
    Mark("cab_rollback");
    const auto cab2 = c.CompleteAndBegin(k, cab.begun_order, srv::NativeLoopCompletion::rollback);
    Check(cab2.ok(), "complete-and-begin (rollback)");
    PrintMeasure("cab_rollback", cab2, cab.begun_order);
    Expect(cab.begun_order, "rolled_back");
    Mark("rollback_rw");
    const auto last = c.Complete(k, cab2.begun_order, srv::NativeLoopCompletion::rollback);
    Check(last.ok(), "rollback");
    PrintMeasure("rollback_rw", last, cab2.begun_order);
    Expect(cab2.begun_order, "rolled_back");
    Mark("end");
    Check(c.Table().empty(), "the table is empty");
  }

  // ---- serving: the whole contract ----
  else {
    Check(scenario == "serving", "a known scenario");
    Check(loop.serving_state() == srv::NativeServingState::serving, "the database is serving");
    const auto image_a = ReadAll(path);
    const auto k1 = Fresh(), k2 = Fresh();

    // begin / ownership refusals write nothing / complete / replay
    const auto ro = c.Begin(k1, true);
    Check(ro.ok() && ro.begun_order >= 2 && !ro.begun_uuid.is_nil(), "a read-only begin");
    Check(ro.writes > 0 && ro.syncs > 0, "a begin writes and syncs (it is durable)");
    Check(c.Table() == std::vector<p::u64>{ro.begun_order}, "the loop's table holds exactly the new transaction");
    const auto bytes_open = ReadAll(path);
    ExpectError(c.Complete(k2, ro.begun_order, srv::NativeLoopCompletion::commit), TE::not_owner, "a completion by a non-owner");
    ExpectError(c.CompleteAndBegin(k2, ro.begun_order, srv::NativeLoopCompletion::commit), TE::not_owner, "a complete-and-begin by a non-owner");
    ExpectError(c.Complete(k1, ro.begun_order + 1000, srv::NativeLoopCompletion::commit), TE::unknown_transaction, "an order the table does not hold");
    ExpectError(c.Complete(p::Uuid{}, ro.begun_order, srv::NativeLoopCompletion::commit), TE::invalid_request, "a nil owner key");
    ExpectError(c.Begin(p::Uuid{}, true), TE::invalid_request, "a begin with a nil owner key");
    ExpectError(c.Complete(k1, 0, srv::NativeLoopCompletion::commit), TE::invalid_request, "order zero");
    ExpectError(c.Complete(k1, ro.begun_order, static_cast<srv::NativeLoopCompletion>(9)), TE::invalid_request, "an unknown completion kind");
    Check(ReadAll(path) == bytes_open, "every refusal wrote nothing: the file is byte-identical");
    Check(c.Table() == std::vector<p::u64>{ro.begun_order}, "the refusals left the table alone");
    const auto done = c.Complete(k1, ro.begun_order, srv::NativeLoopCompletion::commit);
    Check(done.ok() && done.committed && done.completed_order == ro.begun_order && done.completed_at_utc_ns != 0, "the owner commits");
    Expect(ro.begun_order, "committed");
    Check(c.Table().empty(), "a completed transaction leaves the table");
    ExpectError(c.Complete(k1, ro.begun_order, srv::NativeLoopCompletion::commit), TE::unknown_transaction, "a second completion");

    // complete-and-begin: the replacement has the same owner
    const auto rw = c.Begin(k1, false);
    Check(rw.ok() && rw.begun_order == ro.begun_order + 1, "a read-write begin takes the next number");
    const auto cab = c.CompleteAndBegin(k1, rw.begun_order, srv::NativeLoopCompletion::commit);
    Check(cab.ok() && cab.committed && cab.completed_order == rw.begun_order && cab.begun_order == rw.begun_order + 1 &&
              !cab.begun_uuid.is_nil() && !(cab.begun_uuid == rw.begun_uuid),
          "complete-and-begin: n committed and m = n + 1 begun with its own UUID");
    Expect(rw.begun_order, "committed");
    Check(c.Table() == std::vector<p::u64>{cab.begun_order}, "the table swapped n for m");
    ExpectError(c.Complete(k2, cab.begun_order, srv::NativeLoopCompletion::commit), TE::not_owner, "the replacement is not k2's");
    ExpectError(c.CompleteAndBegin(k1, rw.begun_order, srv::NativeLoopCompletion::commit), TE::unknown_transaction, "n is no longer in the table");
    const auto cab2 = c.CompleteAndBegin(k1, cab.begun_order, srv::NativeLoopCompletion::rollback, true);
    Check(cab2.ok() && !cab2.committed && cab2.begun_order == cab.begun_order + 1, "the replacement's owner rolls it back and begins its own replacement");
    Expect(cab.begun_order, "rolled_back");
    const auto last = c.Complete(k1, cab2.begun_order, srv::NativeLoopCompletion::rollback);
    Check(last.ok() && !last.committed, "rollback");
    Expect(cab2.begun_order, "rolled_back");

    // The engine read scope: the S4a credential read and the Part A names read under ONE transaction.
    p::u64 scope_order = 0;
    bool login_ok = false, names_ok = false;
    const auto scope_start = std::chrono::steady_clock::now();
    auto scope = DurableScope(loop, [&](const srv::NativeLoopReadTransaction& txn) {
      scope_order = txn.order;
      Check(loop.on_loop_thread(), "the scope's work runs on the loop thread");
      Check(loop.OpenTransactionOrdersOnLoopThread() == std::vector<p::u64>{txn.order}, "the scope's transaction is the one open transaction");
      const auto login = loop.ReadLoginMaterialOnLoopThread(c.passwords->Request(c.bootstrap, kBootstrapPassword));
      login_ok = login.ok() && login.material && login.material->principal_uuid == c.bootstrap;
      Check(loop.OpenTransactionOrdersOnLoopThread() == std::vector<p::u64>{txn.order}, "still open between the two reads");
      const auto names = loop.ReadNamesSnapshotOnLoopThread();
      names_ok = names.ok() && !names.rows.empty();
    });
    const double scope_ms = Ms(scope_start);
    Check(scope.ok() && scope.begin.begun_order == scope_order && scope.complete.committed && scope.complete.completed_order == scope_order,
          "the scope ran and committed");
    Check(login_ok && names_ok, "the credential read and the names read both succeeded inside ONE transaction");
    Check(scope.begin.begun_order == last.completed_order + 1, "the scope consumed exactly one transaction number");
    Check(scope.begin.writes > 0 && scope.complete.writes > 0 && scope.begin.syncs > 0 && scope.complete.syncs > 0,
          "the begin and the completion of the scope wrote and synced");
    Check(c.Table().empty(), "the table is empty after the scope");
    Expect(scope_order, "committed");

    // A refusal inside the work (wrong password): the scope still completes.
    p::u64 refused_order = 0;
    srv::NativeLoginMaterialError refusal = srv::NativeLoginMaterialError::none;
    const auto refused_scope = DurableScope(loop, [&](const srv::NativeLoopReadTransaction& txn) {
      refused_order = txn.order;
      refusal = loop.ReadLoginMaterialOnLoopThread(c.passwords->Request(c.bootstrap, kWrongPassword)).error;
    });
    Check(refusal == srv::NativeLoginMaterialError::password_mismatch, "the wrong password was refused inside the scope");
    Check(refused_scope.ok() && refused_scope.complete.committed && c.Table().empty(), "the scope completed although its work refused");
    Check(refused_scope.begin.begun_order == scope_order + 1, "a refused login also consumes one number");
    Expect(refused_order, "committed");

    // An exception inside the work: absorbed, reported, still completed.
    p::u64 thrown_order = 0;
    const auto thrown_scope = DurableScope(loop, [&](const srv::NativeLoopReadTransaction& txn) {
      thrown_order = txn.order;
      throw 7;
    });
    Check(thrown_scope.work_ran && thrown_scope.work_threw && !thrown_scope.ok(), "the exception was reported");
    Check(thrown_scope.complete.ok() && thrown_scope.complete.committed && c.Table().empty(), "the scope completed although its work threw");
    Expect(thrown_order, "committed");

    // The snapshot-point body: the same two reads, NO header, NO write, NO transaction number.
    {
      const auto image_s = ReadAll(path);
      bool login = false, names = false;
      p::u64 next_seen = 0;
      const auto t0 = std::chrono::steady_clock::now();
      const auto snap = loop.RunInEngineReadTransaction(
          [&](const srv::NativeLoopReadTransaction& t) {
            next_seen = t.snapshot_next_transaction;
            Check(t.order == 0 && t.transaction_uuid.is_nil() && loop.OpenTransactionOrdersOnLoopThread().empty(),
                  "a snapshot-point scope holds no transaction");
            login = loop.ReadLoginMaterialOnLoopThread(c.passwords->Request(c.bootstrap, kBootstrapPassword)).ok();
            names = loop.ReadNamesSnapshotOnLoopThread().ok();
          },
          srv::NativeLoopReadScopeMode::snapshot_point);
      std::printf("TIMING snapshot_login_scope_ms=%.2f\n", Ms(t0));
      Check(snap.ok() && login && names, "the snapshot-point scope ran both reads");
      Check(snap.begin.writes == 0 && snap.begin.syncs == 0 && ReadAll(path) == image_s, "the snapshot-point scope wrote nothing");
      const auto after = c.Begin(k1, true);
      Check(after.ok(),
            "begin after the snapshot scope");
      Check(next_seen == thrown_order + 1, "the capture saw Next = the next free number");
      Check(after.begun_order == thrown_order + 1, "the snapshot scope consumed no transaction number");
      Check(c.Complete(k1, after.begun_order, srv::NativeLoopCompletion::commit).ok(), "its commit");
      Expect(after.begun_order, "committed");

      // A refused login, a throwing body and a good read at a snapshot point: the file is BYTE-IDENTICAL (no TIP or
      // header page changes at all) and no number is consumed. (Default mode = the snapshot point.)
      const auto image_n = ReadAll(path);
      srv::NativeLoginMaterialError refusal = srv::NativeLoginMaterialError::none;
      const auto refused = loop.RunInEngineReadTransaction([&](const srv::NativeLoopReadTransaction&) {
        refusal = loop.ReadLoginMaterialOnLoopThread(c.passwords->Request(c.bootstrap, kWrongPassword)).error;
      });
      Check(refusal == srv::NativeLoginMaterialError::password_mismatch && refused.ok() && refused.mode == srv::NativeLoopReadScopeMode::snapshot_point,
            "a refused login at the snapshot point: the scope completes, nothing to complete");
      const auto thrown = loop.RunInEngineReadTransaction([&](const srv::NativeLoopReadTransaction&) { throw 7; });
      Check(thrown.work_threw && !thrown.ok(), "a throwing body at the snapshot point is reported");
      const auto good = loop.RunInEngineReadTransaction([&](const srv::NativeLoopReadTransaction&) {
        (void)loop.ReadLoginMaterialOnLoopThread(c.passwords->Request(c.bootstrap, kBootstrapPassword));
      });
      Check(good.ok(), "a good read at the snapshot point");
      Check(ReadAll(path) == image_n, "refused, thrown and good snapshot reads left the file byte-identical");
      const auto probe = c.Begin(k1, true);
      Check(probe.ok() && probe.begun_order == after.begun_order + 1, "no snapshot read consumed a number");
      Check(c.Complete(k1, probe.begun_order, srv::NativeLoopCompletion::commit).ok(), "its commit");
      Expect(probe.begun_order, "committed");

      // SB_READ_TXN_SNAPSHOT_SEAM probe: a transaction begun and committed BETWEEN the capture and the read. The capture
      // (Next, horizons) is a memory value and does not move; the readers take no snapshot parameter, so what they
      // read is committed state at read time, not the capture. (No catalog row is changed here, so the rows are the
      // same either way: the seam is a property of the reader's signature, shown by the capture staying behind.)
      p::u64 captured = 0, current = 0;
      const auto between = loop.RunInEngineReadTransaction([&](const srv::NativeLoopReadTransaction& t) {
        captured = t.snapshot_next_transaction;
        const auto b = loop.BeginWorkOnLoopThread({k2, true, kReadCommittedStatement, {}});
        Check(b.ok(), "a loop transaction begun inside the snapshot work");
        Check(loop.CompleteWorkOnLoopThread({k2, b.begun_order, srv::NativeLoopCompletion::commit}).ok(), "and committed");
        Expect(b.begun_order, "committed");
        current = b.begun_order + 1;
        Check(loop.ReadNamesSnapshotOnLoopThread().ok(), "the names read after the commit");
      });
      Check(between.ok() && captured == probe.begun_order + 1 && current == captured + 1,
            "the capture stayed at its own Next while the loop's Next moved on");
    }

    // From the loop thread itself (login is loop work): the scope runs directly, no queue, no wait.
    srv::NativeLoopReadScopeResult direct;
    p::u64 direct_order = 0;
    Check(loop.RunOnLoopThreadForTest([&] {
            direct = DurableScope(loop, [&](const srv::NativeLoopReadTransaction& txn) { direct_order = txn.order; });
            // the cross-thread submits refuse on the loop thread (it never waits on itself); the direct forms work
            const auto refused = loop.SubmitBeginWork(srv::NativeLoopBeginRequest{}).get();
            Check(refused.error == TE::invalid_request, "a cross-thread submit from the loop thread is refused");
          }),
          "the loop ran the direct scope");
    Check(direct.ok() && direct_order == direct.begin.begun_order, "the scope called on the loop thread ran directly");
    Expect(direct_order, "committed");
    // The loop-thread forms are refused anywhere else.
    srv::NativeLoopBeginRequest off_thread;
    off_thread.owner_key = k1;
    Check(loop.BeginWorkOnLoopThread(off_thread).error == TE::invalid_request, "the OnLoopThread form is refused off the loop thread");

    // The file: only transaction-store pages changed (the TIP state and header chains) across all of the above.
    {
      const auto image_b = ReadAll(path);
      const auto tip = ReadTip(path, page_size);
      std::set<p::u64> allowed(tip.state_pages.begin(), tip.state_pages.end());
      allowed.insert(tip.header_pages.begin(), tip.header_pages.end());
      const auto changed = ChangedPages(image_a, image_b, tip.page_size);
      Check(!changed.empty(), "some pages changed");
      for (const auto page : changed) Check(allowed.contains(page), "a changed page is a TIP or header page, not page " + std::to_string(page));
      std::printf("PAGES changed=%zu of tip=%zu pages (state %zu, header %zu), file %zu bytes\n", changed.size(), allowed.size(),
                  tip.state_pages.size(), tip.header_pages.size(), image_b.size());
    }

    // Timing (informational): the begin/commit pair, the scope with no work, the scope with the S4a read (PBKDF2).
    {
      double pair = 0, empty_scope = 0, login_scope = 0, pbkdf2_in = 0;
      constexpr int kReps = 3;
      for (int i = 0; i < kReps; ++i) {
        auto t = std::chrono::steady_clock::now();
        const auto b = c.Begin(k1, true);
        Check(b.ok() && c.Complete(k1, b.begun_order, srv::NativeLoopCompletion::commit).ok(), "timed pair");
        pair += Ms(t);
        Expect(b.begun_order, "committed");
        t = std::chrono::steady_clock::now();
        const auto s = DurableScope(loop, [](const srv::NativeLoopReadTransaction&) {});
        Check(s.ok(), "timed empty scope");
        empty_scope += Ms(t);
        Expect(s.begin.begun_order, "committed");
        t = std::chrono::steady_clock::now();
        const auto l = DurableScope(loop, [&](const srv::NativeLoopReadTransaction&) {
          const auto t0 = std::chrono::steady_clock::now();
          (void)loop.ReadLoginMaterialOnLoopThread(c.passwords->Request(c.bootstrap, kBootstrapPassword));
          pbkdf2_in += Ms(t0);
        });
        Check(l.ok(), "timed login scope");
        login_scope += Ms(t);
        Expect(l.begin.begun_order, "committed");
      }
      std::printf("TIMING begin_commit_ms=%.2f empty_scope_ms=%.2f login_scope_ms=%.2f login_read_in_scope_ms=%.2f first_scope_ms=%.2f\n",
                  pair / kReps, empty_scope / kReps, login_scope / kReps, pbkdf2_in / kReps, scope_ms);
    }

    // Several threads: six transaction threads (begin, three complete-and-begin, complete) and two scope threads with
    // the S4a read (PBKDF2 on the loop). Every number is distinct; every call succeeds; nobody completes another's.
    {
      constexpr int kThreads = 6;
      std::mutex m;
      std::set<p::u64> orders;
      std::atomic<int> failures{0};
      std::atomic<std::uint32_t> growth_writes{0}, total_writes{0}, total_syncs{0};
      const auto add = [&](p::u64 order) {
        std::lock_guard<std::mutex> g(m);
        if (!orders.insert(order).second) failures.fetch_add(1);
      };
      std::vector<std::thread> threads;
      std::vector<p::Uuid> keys(kThreads);
      for (auto& key : keys) key = Fresh();
      std::vector<p::u64> held(kThreads, 0);
      for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
          const auto account = [&](const srv::NativeLoopTransactionResult& r) {
            if (!r.ok()) failures.fetch_add(1);
            growth_writes += r.growth_writes;
            total_writes += r.writes + r.growth_writes;
            total_syncs += r.syncs + r.growth_syncs;
          };
          auto b = c.Begin(keys[t], false);
          account(b);
          if (!b.ok()) return;
          add(b.begun_order);
          p::u64 current = b.begun_order;
          for (int i = 0; i < 3; ++i) {
            const auto how = (i % 2) ? srv::NativeLoopCompletion::rollback : srv::NativeLoopCompletion::commit;
            const auto r = c.CompleteAndBegin(keys[t], current, how);
            account(r);
            if (!r.ok()) return;
            Expect(current, (i % 2) ? "rolled_back" : "committed");
            add(r.begun_order);
            current = r.begun_order;
            // another thread's key is never the owner
            const auto other = c.Complete(keys[(t + 1) % kThreads], current, srv::NativeLoopCompletion::commit);
            if (other.error != TE::not_owner) failures.fetch_add(1);
          }
          const auto fin = c.Complete(keys[t], current, srv::NativeLoopCompletion::commit);
          account(fin);
          Expect(current, "committed");
        });
      for (int t = 0; t < 2; ++t)
        threads.emplace_back([&] {
          const auto s = DurableScope(loop, [&](const srv::NativeLoopReadTransaction&) {
            if (!loop.ReadLoginMaterialOnLoopThread(c.passwords->Request(c.bootstrap, kBootstrapPassword)).ok()) failures.fetch_add(1);
          });
          if (!s.ok()) failures.fetch_add(1);
          else {
            add(s.begin.begun_order);
            Expect(s.begin.begun_order, "committed");
          }
        });
      for (auto& th : threads) th.join();
      Check(failures.load() == 0, "no failure and no duplicate transaction number across the threads");
      Check(orders.size() == static_cast<std::size_t>(kThreads) * 4 + 2, "every transaction got its own number");
      Check(c.Table().empty(), "every thread completed its own transactions");
      std::printf("THREADS orders=%zu total_writes=%u total_syncs=%u growth_writes=%u\n", orders.size(), total_writes.load(),
                  total_syncs.load(), growth_writes.load());
    }

    // After the loop closes every request is refused specifically.
    Check(loop.Close().ok, "the loop closes");
    ExpectError(c.Begin(k1, true), TE::database_not_serving, "begin after close");
    ExpectError(c.Complete(k1, 2, srv::NativeLoopCompletion::commit), TE::database_not_serving, "complete after close");
    ExpectError(c.CompleteAndBegin(k1, 2, srv::NativeLoopCompletion::commit), TE::database_not_serving, "complete-and-begin after close");
    bool ran = false;
    const auto closed_scope = DurableScope(loop, [&](const srv::NativeLoopReadTransaction&) { ran = true; });
    ExpectError(closed_scope.begin, TE::database_not_serving, "the scope after close");
    Check(!ran, "no work ran after close");
  }

  // Common: every request left no side file.
  for (const auto& entry : fs::directory_iterator(directory))
    Check(entry.path().filename().string().find(".sb.") == std::string::npos, "no .sb.* file exists");
  if (scenario != "abandon_exit") {
    for (const auto& [order, state] : g_expect) std::printf("EXPECT %llu %s\n", static_cast<unsigned long long>(order), state.c_str());
    std::printf("LOOP_TXN_OK checks=%d scenario=%s\n", checks, scenario.c_str());
  }
  return 0;
}

// ---- driver --------------------------------------------------------------------------------------------------------
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
std::map<p::u64, std::string> Expectations(const std::string& output) {
  std::map<p::u64, std::string> out;
  std::istringstream in(output);
  std::string line;
  while (std::getline(in, line))
    if (line.starts_with("EXPECT ")) {
      std::istringstream parts(line.substr(7));
      p::u64 order = 0;
      std::string state;
      parts >> order >> state;
      out[order] = state;
    }
  return out;
}
std::string StateName(const pg::TipStateWord& word) { return pg::TipStateCodeName(word.state); }

void OneSize(const std::string& self, const fs::path& root, std::uint32_t page_size) {
  const std::string tag = "[" + std::to_string(page_size) + "] ";
  const auto dir = root / ("ps" + std::to_string(page_size));
  fs::create_directory(dir);
  int status = 0;
  const auto make = [&](const std::string& name, std::string* ids) {
    const auto sub = dir / name;
    fs::create_directory(sub);
    const auto path = sub / "loop.sdb";
    const auto created = Capture("'" + self + "' create '" + path.string() + "' " + std::to_string(page_size), &status);
    Check(status == 0 && created.starts_with("CREATED "), tag + name + ": create: " + created);
    *ids = Field(created, "database") + " " + Field(created, "bootstrap") + " " + std::to_string(page_size);
    return path;
  };
  const auto serve = [&](const fs::path& path, const std::string& scenario, const std::string& ids) {
    const auto start = std::chrono::steady_clock::now();
    const auto out = Capture("'" + self + "' serve '" + path.string() + "' " + scenario + " " + ids, &status);
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    Check(status == 0 && out.find("LOOP_TXN_OK") != std::string::npos, tag + scenario + ": serve child (exit " + std::to_string(status) + "): " + out.substr(0, 3000));
    for (const auto& forbidden : {kBootstrapPassword, kWrongPassword})
      Check(out.find(forbidden) == std::string::npos, tag + scenario + ": no test password in any child output");
    std::printf("%s%s: %.1f s\n", tag.c_str(), scenario.c_str(), seconds);
    for (const char* key : {"PAGES ", "TIMING ", "THREADS "})
      if (const auto at = out.find(key); at != std::string::npos) std::printf("%s%s: %s\n", tag.c_str(), scenario.c_str(), out.substr(at, out.find('\n', at) - at).c_str());
    std::fflush(stdout);
    return out;
  };
  // The file against what the child said it did: every expected order has exactly the stated state, headers carry the
  // engine's own identities, and nothing else (no unexpected committed work) is in the store.
  const auto verify = [&](const fs::path& path, const std::map<p::u64, std::string>& expected, const std::string& what, bool single_instance) {
    const auto tip = ReadTip(path);
    p::Uuid engine;
    for (const auto& [order, state] : expected) {
      if (state == "dead_unused") {  // a number a failed begin took: dead, and it never had a header
        const auto word = tip.words.find(order);
        Check(word != tip.words.end() && word->second.state == pg::TipStateCode::dead && !tip.headers.contains(order),
              tag + what + ": order " + std::to_string(order) + " is dead with no header");
        continue;
      }
      const auto word = tip.words.find(order);
      Check(word != tip.words.end(), tag + what + ": order " + std::to_string(order) + " has a state word");
      Check(StateName(word->second) == state, tag + what + ": order " + std::to_string(order) + " is " + StateName(word->second) + ", expected " + state);
      const auto header = tip.headers.find(order);
      Check(header != tip.headers.end(), tag + what + ": order " + std::to_string(order) + " has a durable header");
      const auto& h = header->second;
      Check(h.transaction_order == order && !h.transaction_uuid.is_nil(), tag + what + ": header order and UUID");
      Check(!h.engine_instance_uuid.is_nil(), tag + what + ": header +48 is the engine instance");
      if (engine.is_nil()) engine = h.engine_instance_uuid;
      if (single_instance) Check(h.engine_instance_uuid == engine, tag + what + ": one engine instance for every header of one open");
      Check(h.principal_or_agent_uuid.is_nil() && h.attachment_uuid.is_nil() && h.session_uuid.is_nil(),
            tag + what + ": header +64 / +80 / +96 stay zero until S4e (principal unsettled)");
      Check(h.isolation_profile == kReadCommittedStatement, tag + what + ": isolation profile as the request stated");
      if (state == "committed") Check(h.final_state == pg::TipStateCode::committed && h.commit_time_utc_ns != 0 && h.completed_at_utc_ns == h.commit_time_utc_ns, tag + what + ": committed header");
      if (state == "rolled_back") Check(h.final_state == pg::TipStateCode::rolled_back && h.commit_time_utc_ns == 0 && h.completed_at_utc_ns != 0, tag + what + ": rolled-back header");
      if (state == "active" || state == "read_only_active" || state == "dead") Check(h.final_state == pg::TipStateCode::unused || state == "dead", tag + what + ": an in-progress header has no final state");
    }
    return tip;
  };

  // ---- serving: the whole contract, then the file ----
  std::string ids;
  {
    const auto path = make("serving", &ids);
    const auto before = Capture("'" + self + "' pages '" + path.string() + "'", &status);
    const auto first_next = std::stoull(Field(before, "next"));
    const auto out = serve(path, "serving", ids);
    const auto expected = Expectations(out);
    Check(!expected.empty(), tag + "serving: the child reported its transactions");
    const auto tip = verify(path, expected, "serving", true);
    // Every number the child consumed is accounted for: Next advanced by exactly the number of transactions begun.
    Check(tip.counters.next_transaction == first_next + expected.size(), tag + "serving: numbers consumed = transactions begun (" + std::to_string(expected.size()) + ")");
    // Nothing in the file is in progress or dead (the close left no open transaction); the creator's own rows are untouched.
    for (const auto& [order, word] : tip.words)
      if (!expected.contains(order)) Check(word.state == pg::TipStateCode::committed, tag + "serving: an order the child never touched is the creator's committed row");
    std::printf("%sserving: %zu transactions, Next %llu -> %llu, header chain %zu page(s)\n", tag.c_str(), expected.size(),
                static_cast<unsigned long long>(first_next), static_cast<unsigned long long>(tip.counters.next_transaction), tip.header_pages.size());
  }

  // ---- restricted and failed ----
  const auto native_serving = std::string(SB_NATIVE_SERVING_TEST_BIN);
  for (const std::string state : {"restricted", "failed"}) {
    const auto path = make(state, &ids);
    Check(Capture("'" + native_serving + "' fixture '" + path.string() + "' " + (state == "restricted" ? "committing" : "active"), &status).find("FIXTURE ok") != std::string::npos && status == 0,
          tag + state + ": the native_serving writer left the fixture slot");
    const auto before = ReadAll(path);
    serve(path, state, ids);
    if (state == "restricted") Check(ReadAll(path) == before, tag + "restricted: the file is byte-identical");
  }

  // ---- a failed write inside begin, then the reopen ----
  {
    const auto path = make("writefail", &ids);
    const auto out = serve(path, "writefail", ids);
    auto expected = Expectations(out);
    const auto mid = verify(path, expected, "writefail", true);
    const auto good = expected.begin()->first;
    Check(mid.counters.next_transaction == good + 2, tag + "writefail: the failed begin advanced Next (W1) and nothing more");
    Check(!mid.words.contains(good + 1) && !mid.headers.contains(good + 1), tag + "writefail: its slot is unused: no state word, no header");
    const auto reopened = serve(path, "reopen", ids);
    const auto first = std::stoull(Field(reopened, "REOPEN_FIRST_ORDER"));
    Check(first == good + 2, tag + "writefail: the failed number is never reused");
    auto after = Expectations(reopened);
    after[good + 1] = "dead_unused";  // Restart marks the unused slot below Next dead
    verify(path, after, "writefail after reopen", false);
  }

  // ---- abandoned transactions: graceful close and forced exit; the reopen's Restart marks them dead ----
  for (const std::string scenario : {"abandon_close", "abandon_exit"}) {
    const auto path = make(scenario, &ids);
    const auto out = serve(path, scenario, ids);
    auto expected = Expectations(out);
    Check(expected.size() == 2, tag + scenario + ": two transactions left open");
    const auto open = verify(path, expected, scenario + " before reopen", true);
    for (const auto& [order, state] : expected) Check(StateName(open.words.at(order)) == state, tag + scenario + ": the slot is still in progress in the file");
    const auto reopened = serve(path, "reopen", ids);
    const auto first = std::stoull(Field(reopened, "REOPEN_FIRST_ORDER"));
    Check(first > expected.rbegin()->first, tag + scenario + ": numbers are not reused");
    auto after = Expectations(reopened);
    for (const auto& [order, state] : expected) after[order] = "dead";
    verify(path, after, scenario + " after reopen", false);
    std::printf("%s%s: open transactions become dead at the next open's Restart\n", tag.c_str(), scenario.c_str());
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "create") return CreateMode(argv[2], static_cast<std::uint32_t>(std::stoul(argv[3])));
  if (argc == 3 && std::string(argv[1]) == "pages") return PagesMode(argv[2]);
  if (argc == 7 && std::string(argv[1]) == "serve")
    return ServeMode(argv[2], argv[3], FromHex(argv[4]), FromHex(argv[5]), static_cast<std::uint32_t>(std::stoul(argv[6])));
  if (argc == 1 || (argc == 3 && std::string(argv[1]) == "size")) {
    const std::string self = fs::read_symlink("/proc/self/exe").string();
    scratchbird::tests::OwnedTempDirectory artifacts;
    if (argc == 3) OneSize(self, artifacts.path(), static_cast<std::uint32_t>(std::stoul(argv[2])));
    else
      for (const std::uint32_t size : {8192u, 16384u, 32768u, 65536u, 131072u}) OneSize(self, artifacts.path(), size);
    artifacts.Cleanup();
    std::printf("native_loop_transaction: %d driver checks passed\n", checks);
    return 0;
  }
  return 2;
}
