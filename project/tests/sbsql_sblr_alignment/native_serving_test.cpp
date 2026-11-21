// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_NATIVE_SERVING_TEST (thin-slice S3b, plan 38; owner 2026-11-16
// Q236/Q237, 2026-11-19 Q274/Q281). The server opens a natively created
// database and SERVES it to "serving": read-only verification on the database
// loop thread (sb-db-loop), L5's credential derived inside the server from the
// file (database UUID XOR the stored seed) and authenticated. S3d (owner Q241 = 1,
// Q284): the transaction-store Restart then runs once on the loop thread; a clean
// file performs 0 writes and 0 syncs. (Was, S3b/S3c: Restart not run, nothing
// written, the classification native_verified_restart_pending.)
//
// The process gate (HostedProcessAdmission) binds the process permanently after
// one successful open, and the crypto owner is process-start state, so every
// open runs in its own child process: this binary re-executes itself with a
// mode. Modes (each prints one line, "SERVING ..." or "REFUSED code=..."):
//   create <path> <page_size>            engine-API native create; prints identities
//   create-lowlevel <path> <ps> <kind>   storage-level create: kind seedless | mismatched
//   serve <path> [variant]               one StartHostedEngine, then a clean shutdown
//   serve-hold <path>                    serve, wait for SIGTERM, shut down
//   serve <path> read                    S3c: serve, then served reads after custody (below), shut down
//   memory-defaults                      prints the three in-code per-context memory defaults (Q282)
//   fixture <path> <kind>                S3d: the WRITER leaves a crash state in a created file: active | unused |
//                                        header | committing | recovering | prepared | limbo | committing_active |
//                                        mixed | many (three dirty state units)
//   serve <path> kill=N|torn=N|fail=N|failsync=N   S3d: a fault injected into the sink Restart writes through
// With no argument the binary drives the cases for all five page sizes
// (8K..128K) and checks: serving; the file's bytes unchanged (a clean file: no write
// even after Restart) and no file made; the loop thread named sb-db-loop; L5 authenticated
// as the principal the file names; a second server on the same file refused
// (ARCH.DATABASE_MULTI_OWNER); clean shutdown on the loop thread (a wrong-thread
// destroy terminates the process) with the flock released; a byte flipped in
// every page-zero root page refused with a specific code, the file unchanged;
// an L5 seed row absent or inconsistent refused with a specific code.
// S3d (RestartCases): crash states made by the writer itself (an active slot, an unused slot below
// Next, a header over an unused slot, a mixed inventory) are marked dead by the open, the file equal to
// an oracle built from the old image (only the dead slots' state bytes change) and an independent TIP
// reader sees them dead; kill -9 / torn write at every write of a three-unit Restart, then a reopen,
// reaches the same final image; an injected write or sync failure gives the failed serving state
// (fenced, status shows it, a reopen recovers); committing / recovering slots open RESTRICTED with a
// specific code and write nothing; prepared / limbo slots are in doubt (reported, left alone).
// The strace half (0 ".sb." syscalls, 0 lock files, 0 pwrite/fsync/fdatasync on
// the data descriptor, 0 opens of the database path after serving) is native_serving_strace.py.
// SEARCH_KEY: SB_NATIVE_SERVED_READS_AFTER_CUSTODY (S3c, plan 38 2e, owner Q239 = 1,
// 2026-11-19). After "serving" (custody taken, legacy borrowing fenced) a session/engine thread
// still obtains READ-ONLY served device duplicates of the lease's retained descriptor (no path
// open, no lock file, no wait on anything), a writable served open is refused with
// SB-STORAGE-DISK-SERVED-NATIVE-WRITE-REFUSED, and eight threads read concurrently, byte-identical
// to the file. Writes only ever go through the loop thread's custody operation.

#include "lifecycle/engine_lifecycle_api.hpp"
#include "hash_digest.hpp"
#include "database_lifecycle.hpp"
#include "native_database_loop.hpp"
#include "native_database_open_discovery.hpp"
#include "transaction_inventory_writer.hpp"
#include "transaction_state_page.hpp"
#include "native_fixed_root_memory_lease.hpp"
#include "transaction_inventory_reader.hpp"
#include "native_database_create.hpp"
#include "admitted_datatype_cohort.hpp"
#include "filespace_page_zero.hpp"
#include "catalog_language.hpp"
#include "server/config.hpp"
#include "server/engine_host.hpp"
#include "server/database_ownership.hpp"
#include "server/native_database_loop.hpp"
#include "server/process_crypto_owner.hpp"
#include "uuid.hpp"
#include "memory.hpp"
#include "../support/owned_temp_directory.hpp"
#include "../support/native_database_create_fixture.hpp"
#include "../support/path_open_counter.hpp"
#include "memory_policy_config.hpp"

#include <csignal>
#include <pthread.h>
#include <fcntl.h>
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
#include <optional>
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
namespace db = scratchbird::storage::database;
namespace srv = scratchbird::server;
namespace fs = std::filesystem;
namespace pg = scratchbird::storage::page;

int checks = 0;
void Check(bool ok, const std::string& what, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL %s (line %u)\n", what.c_str(), at.line());
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

std::map<std::string, std::string> Snapshot(const fs::path& dir) {
  std::map<std::string, std::string> out;
  for (const auto& entry : fs::directory_iterator(dir)) {
    std::string value = "dir";
    if (entry.is_regular_file()) {
      std::ifstream in(entry.path(), std::ios::binary);
      const std::string bytes((std::istreambuf_iterator<char>(in)), {});
      const auto digest = scratchbird::core::hash::ComputeSha256Digest(
          reinterpret_cast<const p::byte*>(bytes.data()), bytes.size());
      Check(digest.ok(), "sha256");
      value = std::to_string(bytes.size()) + ":" + scratchbird::core::hash::HexLower(digest.digest);
    }
    out[entry.path().filename().string()] = value;
  }
  return out;
}

// ---- child modes --------------------------------------------------------------

int CreateMode(const fs::path& path, std::uint32_t page_size) {
  scratchbird::tests::NativeFixtureCreateRequest request;
  request.path = path;
  request.page_size = page_size;
  request.database_name = "native_serving";
  request.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  const auto made = scratchbird::tests::CreateNativeFixtureDatabase(request);
  if (!made.ok) {
    std::cerr << made.diagnostic_code << ':' << made.diagnostic_detail << '\n';
    return 1;
  }
  std::cout << "database_uuid=" << Hex(made.database_uuid) << "\nservice_principal_uuid="
            << Hex(made.service_principal_uuid) << '\n';
  return 0;
}

// A storage-level create whose L5 seed is absent (seedless) or present but
// inconsistent with the stored verifier (mismatched): the two files the
// engine-API create never writes.
int CreateLowLevelMode(const fs::path& path, std::uint32_t page_size, std::string_view kind) {
  const auto* profile = d::FindCanonicalFilespacePageProfileForSize(page_size);
  if (!profile) return 2;
  const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  const auto database_uuid = scratchbird::core::uuid::GenerateEngineIdentityV7(p::UuidKind::database, now);
  const auto policy_uuid = scratchbird::core::uuid::GenerateEngineIdentityV7(p::UuidKind::object, now);
  const auto engine_uuid = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  if (!database_uuid.ok() || !policy_uuid.ok() || !engine_uuid) return 2;
  scratchbird::core::uuid::StandaloneUuidV7Issuer issuer({database_uuid.value.value, policy_uuid.value.value},
                                                         scratchbird::core::uuid::kStandardLocalClockUuidV7Policy);
  db::NativeDatabaseCreateRequest create;
  create.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  create.database_name = "native_serving_lowlevel";
  create.database_default_language = scratchbird::core::catalog::kCatalogLanguage_en;
  create.page_size_profile_uuid = profile->uuid;
  create.page_size_bytes = page_size;
  create.total_pages = page_size == 8192 ? 2048 : 512;
  create.creation_utc_millis = now;
  create.engine_uuid = *engine_uuid;
  create.datatype_cohort = {{p::UuidKind::object, scratchbird::core::datatypes::kDatatypeCohortV11}, 11, 11};
  create.bootstrap_principal_name = "BootstrapAdmin";
  // The verifier the enrollment producer test uses (password "DBLC013G-fixture-password").
  static constexpr std::string_view kVerifier =
      "local-password-pbkdf2-sha256:v1:iterations=600000:salt=0123456789abcdef0123456789abcdef:"
      "verifier=4ce03aa5a5657aaf221192635ed9c63acdb76d78a0994ec6e6ab55286e29e6a5";
  create.bootstrap_credential_fingerprint = kVerifier;
  create.service_principal_name = "NodeService";
  create.service_credential_fingerprint = kVerifier;
  create.maximum_retained_image_bytes = db::NativeHostedSourceBackingBytes(page_size);
  if (kind == "mismatched") {
    // A non-zero seed whose derived password is not the one the fingerprint
    // was made from: the row carries a seed, the verifier rejects the derived
    // credential.
    // S4a (2026-11-20): the derived password is database UUID XOR seed, and a zero byte in it is refused as
    // password_inadmissible (no-NUL rule) BEFORE the verifier is consulted. A fixed seed against a time-based
    // database UUID hit that about 1 run in 16, so the expected 'verifier rejects' result flaked. Production
    // re-draws the seed until the XOR has no zero byte; this fixture does the same: bump any seed byte equal
    // to the database UUID byte at the same position.
    for (std::size_t i = 0; i < create.service_password_seed.size(); ++i) {
      auto seed_byte = static_cast<p::byte>(0x11 + i);
      if (static_cast<p::byte>(database_uuid.value.value.bytes[i]) == seed_byte) seed_byte = static_cast<p::byte>(seed_byte + 1);
      create.service_password_seed[i] = seed_byte;
    }
  } else if (kind != "seedless") {
    return 2;
  }
  d::FileDevice created;
  if (!created.Open(path.string(), d::FileOpenMode::create_new, d::DatabaseFileOwnership::native_data_file).ok()) return 1;
  const auto made = db::CreateNativeDatabaseOnOpenDevice(created, create, issuer);
  if (!made.ok() || !created.Close().ok()) {
    std::cerr << "low-level create failed " << int(made.error) << " policy=" << int(made.policy_error)
              << " database=" << int(made.database_error) << '\n';
    return 1;
  }
  return 0;
}


// ---- S3d fixtures and fault sinks ---------------------------------------------------
// SEARCH_KEY: SB_NATIVE_RESTART_TEST. A native file is left in a crash state by the WRITER ITSELF
// (no hand-built byte patterns except the committing / recovering / prepared / limbo words, which no
// current writer stores), and the server's Restart is then run on it.

// Passes calls through until `limit` writes have been performed, then fails every write without
// performing it: the writer is left exactly where a crash after `limit` writes would leave the file.
class StopSink final : public d::CarefulWriteSink {
 public:
  StopSink(d::CarefulWriteSink& inner, unsigned limit) : inner_(inner), limit_(limit) {}
  d::BoundedIoResult WriteAt(p::u64 offset, std::span<const p::byte> bytes) override {
    if (writes_ >= limit_) {
      d::BoundedIoResult r;
      r.operation = d::BoundedIoOperation::write;
      r.error = d::BoundedIoError::native_failure;
      r.offset = offset;
      r.requested_bytes = bytes.size();
      r.native_error = EIO;
      r.native_attempted = true;
      return r;
    }
    const auto r = inner_.WriteAt(offset, bytes);
    if (r.ok()) ++writes_;
    return r;
  }
  d::BoundedIoResult ReadAt(p::u64 offset, std::span<p::byte> bytes) override { return inner_.ReadAt(offset, bytes); }
  d::BoundedIoResult Sync() override { return inner_.Sync(); }
  bool forced_ordered() const noexcept override { return inner_.forced_ordered(); }

 private:
  d::CarefulWriteSink& inner_;
  unsigned limit_;
  unsigned writes_ = 0;
};

// The fault the server's Restart sees. kill: SIGKILL the process after `at` writes were performed
// (0 = before the first); torn: the write numbered `at` reaches the file only as its first half, then
// SIGKILL; fail: write number `at` fails (not performed); failsync: sync number `at` fails.
class RestartFaultSink final : public d::CarefulWriteSink {
 public:
  RestartFaultSink(d::CarefulWriteSink& inner, std::string kind, unsigned at)
      : inner_(inner), kind_(std::move(kind)), at_(at) {
    if (kind_ == "kill" && at_ == 0) ::kill(::getpid(), SIGKILL);
  }
  d::BoundedIoResult WriteAt(p::u64 offset, std::span<const p::byte> bytes) override {
    ++write_calls_;
    if (kind_ == "torn" && write_calls_ == at_) {
      (void)inner_.WriteAt(offset, bytes.first(bytes.size() / 2));
      ::kill(::getpid(), SIGKILL);
    }
    if (kind_ == "fail" && write_calls_ == at_) return Failure(d::BoundedIoOperation::write, offset, bytes.size());
    const auto r = inner_.WriteAt(offset, bytes);
    if (r.ok() && kind_ == "kill" && ++performed_ == at_) ::kill(::getpid(), SIGKILL);
    return r;
  }
  d::BoundedIoResult ReadAt(p::u64 offset, std::span<p::byte> bytes) override { return inner_.ReadAt(offset, bytes); }
  d::BoundedIoResult Sync() override {
    if (kind_ == "failsync" && ++sync_calls_ == at_) return Failure(d::BoundedIoOperation::sync, 0, 0);
    return inner_.Sync();
  }
  bool forced_ordered() const noexcept override { return inner_.forced_ordered(); }

 private:
  static d::BoundedIoResult Failure(d::BoundedIoOperation kind, p::u64 offset, std::size_t bytes) {
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
  std::string kind_;
  unsigned at_;
  unsigned write_calls_ = 0, performed_ = 0, sync_calls_ = 0;
};

p::Uuid FixtureV7(std::uint64_t ms, unsigned salt) {
  p::Uuid id;
  for (int i = 0; i < 6; ++i) id.bytes[i] = static_cast<p::byte>(ms >> (8 * (5 - i)));
  id.bytes[6] = 0x70;
  id.bytes[7] = static_cast<p::byte>(salt);
  id.bytes[8] = 0x80;
  id.bytes[9] = 0x6c;
  id.bytes[14] = static_cast<p::byte>(salt >> 8);
  id.bytes[15] = static_cast<p::byte>(salt);
  return id;
}

// fixture <path> <kind>. kinds: active | unused | header | committing | recovering | prepared | limbo |
// committing_active | mixed | many. Orders: active/unused/header/committing/recovering/prepared/limbo use order 2; mixed:
// 2 active, 3 committed, 4 rolled back, 5 active, 6 header over an unused slot; many: 2..258 active
// (three 512-byte state units).
int FixtureMode(const fs::path& path, std::string_view kind) {
  d::FileDevice device;
  if (!device.Open(path.string(), d::FileOpenMode::open_existing, d::DatabaseFileOwnership::native_data_file).ok()) return 1;
  const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device);
  if (!zero.ok()) return 1;
  pg::TransactionWriterBinding binding;
  binding.database_uuid = zero.record->bootstrap.database_uuid;
  std::optional<d::FilespaceRootReference> state_root;
  for (const auto& root : zero.record->roots) {
    if (root.kind == 4) state_root = root;
    if (root.kind == 3) binding.allocation_root = root;
  }
  if (!state_root) return 1;
  binding.state_root = *state_root;
  const auto policy = scratchbird::core::uuid::IssueRuntimeIdentityV7();
  if (!policy) return 1;
  scratchbird::core::uuid::StandaloneUuidV7Issuer issuer({binding.database_uuid, *policy},
                                                         scratchbird::core::uuid::kStandardLocalClockUuidV7Policy);
  binding.issuer = &issuer;
  const auto now_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  const std::uint64_t now_ns = now_ms * 1000000ULL;
  int code = 0;
  {
    d::FileDeviceCarefulWriteSink file(device);
    pg::TransactionInventoryWriter w(binding);
    const auto begin = [&](unsigned n, d::CarefulWriteSink& sink) {
      pg::TransactionHeaderRecord h;
      h.transaction_uuid = FixtureV7(now_ms + n, n);
      return w.Begin(sink, h);
    };
    const auto set_word = [&](unsigned slot, pg::TipStateCode state) {
      const auto ps = zero.record->bootstrap.page_size_bytes;
      std::vector<p::byte> page(ps);
      if (!device.ReadAt(state_root->page_number * ps, page.data(), page.size()).ok()) return false;
      const auto edit = pg::SetTransactionStateCode(page, slot, state);
      if (!edit.ok()) return false;
      return device.WriteAt(state_root->page_number * ps + edit.unit_offset, page.data() + edit.unit_offset,
                            pg::kTransactionStateWriteUnitBytes).ok() && file.Sync().ok();
    };
    if (!w.Restart(file).ok()) return 1;
    if (kind == "active") {
      code = begin(2, file).ok() ? 0 : 1;
    } else if (kind == "unused") {
      StopSink stop(file, 1);  // Next := 3 is durable; the header and the word never were
      (void)begin(2, stop);
    } else if (kind == "header") {
      StopSink stop(file, 2);  // Next := 3 and the header record are durable; the word never was
      (void)begin(2, stop);
    } else if (kind == "committing" || kind == "recovering" || kind == "prepared" || kind == "limbo") {
      const auto state = kind == "committing" ? pg::TipStateCode::committing
                         : kind == "recovering" ? pg::TipStateCode::recovering
                         : kind == "prepared"   ? pg::TipStateCode::prepared
                                                : pg::TipStateCode::limbo;
      code = begin(2, file).ok() && set_word(2, state) ? 0 : 1;
    } else if (kind == "committing_active") {
      // 2 committing (unresolved), 3 active: Restart must refuse BEFORE marking 3 dead (Q241 = 1).
      code = begin(2, file).ok() && begin(3, file).ok() && set_word(2, pg::TipStateCode::committing) ? 0 : 1;
    } else if (kind == "mixed") {
      bool ok = begin(2, file).ok() && begin(3, file).ok() && w.Commit(file, 3, now_ns + 1000000).ok() &&
                begin(4, file).ok() && w.Rollback(file, 4, now_ns + 2000000).ok() && begin(5, file).ok();
      StopSink stop(file, 2);
      (void)begin(6, stop);
      code = ok ? 0 : 1;
    } else if (kind == "many") {
      for (unsigned n = 2; n <= 258 && code == 0; ++n)
        if (!begin(n, file).ok()) code = 1;
    } else {
      code = 2;
    }
  }
  if (!device.Close().ok()) code = 1;
  if (code == 0) std::cout << "FIXTURE ok kind=" << kind << std::endl;
  return code;
}

std::string ThreadNames() {
  std::set<std::string> names;
  for (const auto& task : fs::directory_iterator("/proc/self/task")) {
    std::ifstream in(task.path() / "comm");
    std::string name;
    std::getline(in, name);
    names.insert(name);
  }
  std::string out;
  for (const auto& name : names) out += (out.empty() ? "" : ",") + name;
  return out;
}

// S3c: served reads after custody. `file` is the whole database file, read BEFORE the server opened it,
// so no path is opened once it is serving. Every read below goes through a served device (a duplicate of
// the lease's retained descriptor).
std::string ServedReads(const srv::HostedEngineState& state, const fs::path& path, std::uint32_t page_size,
                        const std::vector<char>& file) {
  namespace opens = scratchbird::tests::path_open_counter;
  const auto lease = srv::ServedRouteOwnerFor(state, path.string());
  Check(lease != nullptr, "the native lease is resolvable by its path after serving");
  Check(lease->ObserveWriteCustody() == d::RouteWriteCustodyState::retained,
        "write custody is retained by the lease (the runtime took it before serving)");
  opens::Watch(path.string());
  const auto before = d::ObserveFileDevicePathOpens();
  const auto file_opens_before = opens::FileOpens(), sidecar_opens_before = opens::SidecarOpens();

  // Page zero and every root page page zero names (the catalog's among them): what a session reads.
  std::vector<std::uint64_t> pages{0};
  {
    d::FileDevice device;
    Check(device.OpenServed(lease, path.string(), d::FileOpenMode::open_existing_read_only).ok(),
          "a read-only served device is issued after custody");
    Check(device.read_only(), "the served device is read-only");
    const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device);
    Check(zero.ok(), "page zero reads through the served device");
    std::set<std::uint64_t> roots;
    for (const auto& root : zero.record->roots) roots.insert(root.page_number);
    for (const auto root : roots)
      if (root != 0) pages.push_back(root);
    Check(device.Close().ok(), "the served device closes");
  }
  Check(pages.size() >= 3, "page zero names root pages to compare");

  // A writable served open of a native lease is refused with its specific code, custody or not.
  std::string refusal_code;
  {
    d::FileDevice writer;
    const auto refused = writer.OpenServed(lease, path.string(), d::FileOpenMode::open_existing);
    Check(!refused.ok() && !writer.is_open(), "a writable served open of the native lease is refused");
    refusal_code = refused.diagnostic.diagnostic_code;
    Check(refusal_code == "SB-STORAGE-DISK-SERVED-NATIVE-WRITE-REFUSED", "refused with the specific code: " + refusal_code);
  }

  const auto compare = [&](d::FileDevice& device, std::string& why) {
    std::vector<char> page(page_size);
    for (const auto number : pages) {
      const auto read = device.ReadAt(number * page_size, page.data(), page.size());
      if (!read.ok() || read.bytes_transferred != page.size()) { why = "read failed, page " + std::to_string(number); return false; }
      if (!std::equal(page.begin(), page.end(), file.begin() + static_cast<std::ptrdiff_t>(number * page_size))) {
        why = "page " + std::to_string(number) + " differs from the file";
        return false;
      }
    }
    return true;
  };

  // Another thread (a session/engine thread) obtains its own device and reads the pages.
  {
    std::string why = "not run";
    bool ok = false;
    std::thread reader([&] {
      d::FileDevice device;
      if (!device.OpenServed(lease, path.string(), d::FileOpenMode::open_existing_read_only).ok()) { why = "open refused"; return; }
      ok = compare(device, why);
      // The device cannot write: positional write on a read-only duplicate is refused by the device.
      if (device.WriteAt(0, file.data(), page_size).ok()) { ok = false; why = "a read-only served device accepted a write"; }
      if (!device.Close().ok()) { ok = false; why = "close failed"; }
    });
    reader.join();
    Check(ok, "another thread's served device reads page 0 and the catalog pages byte-identical to the file: " + why);
  }

  // Eight threads at once, each opening, reading and closing its own device repeatedly. No thread
  // waits on another (a barrier releases them together and the whole run has a deadline).
  constexpr int kThreads = 8, kRounds = 25;
  std::atomic<int> arrived{0}, failures{0}, completed{0};
  std::atomic<bool> go{false};
  std::vector<std::string> reasons(kThreads);
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t)
    threads.emplace_back([&, t] {
      ++arrived;
      while (!go.load()) std::this_thread::yield();
      for (int round = 0; round < kRounds; ++round) {
        d::FileDevice device;
        if (!device.OpenServed(lease, path.string(), d::FileOpenMode::open_existing_read_only).ok()) {
          reasons[t] = "open refused"; ++failures; return;
        }
        if (!compare(device, reasons[t])) { ++failures; return; }
        if (!device.Close().ok()) { reasons[t] = "close failed"; ++failures; return; }
        ++completed;
      }
    });
  while (arrived.load() < kThreads) std::this_thread::yield();
  const auto started = std::chrono::steady_clock::now();
  go = true;
  for (auto& thread : threads) thread.join();
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
  std::string first_reason;
  for (const auto& r : reasons) if (!r.empty()) { first_reason = r; break; }
  Check(failures.load() == 0 && completed.load() == kThreads * kRounds,
        "eight concurrent readers each read every page byte-identical: " + first_reason);

  const auto after = d::ObserveFileDevicePathOpens();
  const auto served_opens = after.served_opens - before.served_opens;
  const auto path_opens = (after.opens_without_route_owner - before.opens_without_route_owner) +
                          (after.opens_with_route_owner - before.opens_with_route_owner);
  const auto file_opens = opens::FileOpens() - file_opens_before, sidecar_opens = opens::SidecarOpens() - sidecar_opens_before;
  opens::Watch({});
  Check(path_opens == 0, "no served read counted as a FileDevice path open");
  Check(file_opens == 0 && sidecar_opens == 0, "no open of the database file or a sidecar reached libc during the served reads");
  Check(served_opens == 1 + 1 + kThreads * kRounds, "every read used a served device: " + std::to_string(served_opens));
  return "READ pages=" + std::to_string(pages.size()) + " threads=" + std::to_string(kThreads) + " rounds=" +
         std::to_string(kRounds) + " served_opens=" + std::to_string(served_opens) + " path_opens=" + std::to_string(path_opens) +
         " file_opens=" + std::to_string(file_opens) + " sidecar_opens=" + std::to_string(sidecar_opens) +
         " writable_refused=" + refusal_code + " elapsed_ms=" + std::to_string(elapsed_ms);
}

// The S3d fields of a started database, as "key=value" words: the serving state and its status
// rendering, the Restart report and the stored horizons.
std::string RestartFields(const srv::HostedEngineState& state) {
  const auto& snapshot = state.databases.front();
  const auto json = srv::HostedEngineStatusJson(state);
  const auto json_has = [&](const std::string& part) { return json.find(part) != std::string::npos; };
  std::string unresolved;
  for (const auto order : snapshot.native_restart_unresolved) unresolved += (unresolved.empty() ? "" : ",") + std::to_string(order);
  std::ostringstream out;
  out << " serving_state=" << snapshot.native_serving_state << " classification=" << snapshot.startup_recovery_classification
      << " restart_ran=" << snapshot.native_restart_ran << " marked_dead=" << snapshot.native_restart_marked_dead
      << " restart_writes=" << snapshot.native_restart_writes << " restart_syncs=" << snapshot.native_restart_syncs
      << " next=" << snapshot.native_restart_next_transaction << " scanned_from=" << snapshot.native_restart_scanned_from
      << " unresolved=" << (unresolved.empty() ? "-" : unresolved) << " uncertain=" << snapshot.native_uncertain_transactions
      << " writer_failed=" << snapshot.native_writer_failed
      << " horizons=" << snapshot.native_horizon_oit << "/" << snapshot.native_horizon_oat << "/" << snapshot.native_horizon_ost
      << "/" << snapshot.native_horizon_oct << "/" << snapshot.native_horizon_oar << "/" << snapshot.native_horizon_olr
      << " diag=" << (snapshot.diagnostic_code.empty() ? "-" : snapshot.diagnostic_code)
      << " status_serving=" << json_has("\"native_serving_state\":\"" + snapshot.native_serving_state + "\"")
      << " status_restart_pending=" << json_has(std::string("\"native_restart_pending\":") + (snapshot.native_restart_pending ? "true" : "false"))
      << " status_marked_dead=" << json_has("\"native_restart_marked_dead\":" + std::to_string(snapshot.native_restart_marked_dead))
      << " status_writer_failed=" << json_has(std::string("\"native_writer_failed\":") + (snapshot.native_writer_failed ? "true" : "false"))
      << " status_diag=" << json_has("\"diagnostic_code\":\"" + snapshot.diagnostic_code + "\"")
      << " status_state=" << json_has(std::string("{\"state\":\"") + srv::HostedDatabaseStateName(snapshot.state) + "\"");
  return out.str();
}

// One StartHostedEngine in this process, as a server process does: the process
// crypto owner started first (it outlives the hosted state), then the open.
// variant: "" normal | keyed | readonly | nocrypto | read | probe | nopack | and the S3d Restart
// faults kill=N | torn=N | fail=N | failsync=N (SB_TEST_SEAM, see RestartFaultSink).
int ServeMode(const fs::path& path, std::string_view variant, bool hold) {
  srv::ServerBootstrapConfig config;
  if (variant != "nopack") config.database_policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  // "probe": the production default (the platform memory ceiling probe on);
  // every other variant turns it off as the enrollment harness does.
  config.memory_enable_platform_memory_probe = variant == "probe";
  const auto policy = srv::ResolveServerMemoryAllocationPolicy(config);
  srv::ServerProcessCryptoOwner crypto;
  if (!policy.ok() || !scratchbird::core::memory::ConfigureDefaultMemoryManager(policy.policy, config.memory_policy_provenance).ok() ||
      !crypto.Start(config, policy).ok())
    return 3;
  config.database_default_path = path;
  // S3c: the whole file is read BEFORE the open, so nothing opens the path once the server is serving.
  std::vector<char> whole_file;
  if (variant == "read") {
    std::ifstream in(path, std::ios::binary);
    whole_file.assign(std::istreambuf_iterator<char>(in), {});
  }
  if (variant == "keyed") config.startup_credential_source = "operator_file";
  if (variant == "readonly") config.database_open_mode = "read_only";
  // S3d: a fault injected into the sink Restart writes through.
  for (const std::string kind : {"kill", "torn", "fail", "failsync"}) {
    if (variant.starts_with(kind + "=")) {
      const unsigned at = static_cast<unsigned>(std::stoul(std::string(variant.substr(kind.size() + 1))));
      srv::SetNativeRestartSinkWrapperForTest([kind, at](d::CarefulWriteSink& inner) -> std::unique_ptr<d::CarefulWriteSink> {
        return std::make_unique<RestartFaultSink>(inner, kind, at);
      });
    }
  }
  sigset_t blocked;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGTERM);
  if (hold) ::pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
  int exit_code = 0;
  {
    auto started = srv::StartHostedEngine(config, nullptr, variant == "nocrypto" ? nullptr : &crypto);
    if (!started.ok()) {
      const auto& diagnostic = started.diagnostics.front();
      std::cout << "REFUSED code=" << diagnostic.code;
      for (const auto& [key, value] : diagnostic.fields)
        if (key != "database_path") std::cout << ' ' << key << '=' << value;
      std::cout << std::endl;
      // A failed serving state (S3d) is reported by the status as well: the database is open and
      // fenced, and the state says it requires a reopen.
      if (!started.state.databases.empty()) {
        const auto& snapshot = started.state.databases.front();
        std::cout << "STATE db_state=" << srv::HostedDatabaseStateName(snapshot.state)
                  << " open=" << snapshot.database_open << " fenced=" << snapshot.write_admission_fenced
                  << " restart_pending=" << snapshot.native_restart_pending << RestartFields(started.state) << std::endl;
      }
    } else {
      const auto& snapshot = started.state.databases.front();
      std::string listing;
      for (const auto& entry : fs::directory_iterator(path.parent_path())) listing += entry.path().filename().string() + ";";
      std::cout << "SERVING state=" << snapshot.native_serving_state << " db_state="
                << srv::HostedDatabaseStateName(snapshot.state) << " restart_pending=" << snapshot.native_restart_pending
                << " open=" << snapshot.database_open << " fenced=" << snapshot.write_admission_fenced
                << " transactions=" << snapshot.native_verified_transactions
                << " principal=" << Hex(snapshot.native_service_principal_uuid)
                << " database=" << Hex(snapshot.database_uuid) << " page_size=" << snapshot.page_size_bytes
                << " threads=" << ThreadNames() << " listing=" << listing
                << " status_has_state=" << (srv::HostedEngineStatusJson(started.state).find("\"native_serving_state\":\"serving\"") != std::string::npos)
                << " served_owner=" << (srv::ServedRouteOwnerFor(started.state, path.string()) != nullptr)
                << RestartFields(started.state) << std::endl;
      if (variant == "read") std::cout << ServedReads(started.state, path, snapshot.page_size_bytes, whole_file) << std::endl;
      if (hold) {
        int signal_number = 0;
        sigwait(&blocked, &signal_number);
      }
    }
  }  // the hosted state is destroyed here: the loop closes and joins on its own thread
  // The flock was released by that close, not by process exit: this process owns the file again.
  if (variant != "nocrypto" && variant != "keyed" && variant != "readonly") {
    auto again = srv::AcquireDatabaseOwnership({path, "server", {}, d::RouteStorageAccess::read_only,
                                                d::DatabaseFileOwnership::native_data_file});
    // The loop's std::thread was JOINED by the close above, but the kernel removes a joined thread's
    // /proc/self/task entry slightly later than the join returns (pthread_join wakes on the cleared tid
    // word in exit_mm; release_task runs afterwards in exit_notify). A listing taken in that window still
    // names "sb-db-loop" although the thread function has returned (seen once under suite load at 128K:
    // "AFTER_CLOSE reowned=true threads=sb-db-loop,..."). Wait, bounded, for the kernel to settle BEFORE
    // listing; the assertion is unchanged (the loop thread must be gone) and still fails if it never goes.
    std::string after_threads = ThreadNames();
    int settle_ms = 0;
    while (after_threads.find("sb-db-loop") != std::string::npos && settle_ms < 2000) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      settle_ms += 5;
      after_threads = ThreadNames();
    }
    std::cout << "AFTER_CLOSE reowned=" << (again.acquired ? "true" : "false") << " threads=" << after_threads
              << " settle_ms=" << settle_ms << std::endl;
    if (again.acquired) again.lock->release();
  }
  return exit_code;
}

// ---- the driver -----------------------------------------------------------------

struct Child {
  pid_t pid = -1;
  int out = -1;
};
Child Spawn(const std::string& self, const std::vector<std::string>& args) {
  int fds[2];
  Check(::pipe(fds) == 0, "pipe");
  const auto pid = ::fork();
  Check(pid >= 0, "fork");
  if (pid == 0) {
    ::dup2(fds[1], 1);
    ::close(fds[0]);
    ::close(fds[1]);
    std::vector<char*> argv;
    std::vector<std::string> storage = args;
    storage.insert(storage.begin(), self);
    for (auto& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);
    ::execv(self.c_str(), argv.data());
    ::_exit(72);
  }
  ::close(fds[1]);
  return {pid, fds[0]};
}
std::string ReadLine(Child& child) {
  std::string line;
  char ch;
  while (::read(child.out, &ch, 1) == 1) {
    if (ch == '\n') break;
    line.push_back(ch);
  }
  return line;
}
std::string ReadAll(Child& child) {
  std::string all;
  char chunk[512];
  ssize_t got;
  while ((got = ::read(child.out, chunk, sizeof chunk)) > 0) all.append(chunk, static_cast<std::size_t>(got));
  return all;
}
int Wait(Child& child) {
  ::close(child.out);
  int status = 0;
  ::waitpid(child.pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}
struct Run {
  int status = 0;
  std::string first_line, rest;
};
Run RunChild(const std::string& self, const std::vector<std::string>& args) {
  auto child = Spawn(self, args);
  Run run;
  run.first_line = ReadLine(child);
  run.rest = ReadAll(child);
  run.status = Wait(child);
  return run;
}
bool Has(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }
std::string Field(const std::string& line, std::string_view key) {
  const auto at = line.find(std::string(key) + "=");
  if (at == std::string::npos) return {};
  const auto from = at + key.size() + 1;
  const auto to = line.find(' ', from);
  return line.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

void ExpectOwnable(const fs::path& path, const std::string& what) {
  d::FileDevice device;
  Check(device.Open(path.string(), d::FileOpenMode::open_existing_read_only, d::DatabaseFileOwnership::native_data_file).ok(),
        what + ": the file is natively ownable");
  Check(device.Close().ok(), "close");
}

// Every root page page zero names (state, allocation, catalog, ...): the pages
// a verification reads.
std::vector<std::uint64_t> RootPages(const fs::path& path) {
  d::FileDevice device;
  Check(device.Open(path.string(), d::FileOpenMode::open_existing_read_only, d::DatabaseFileOwnership::native_data_file).ok(),
        "open for the root list");
  const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device);
  Check(zero.ok(), "page zero reads");
  std::set<std::uint64_t> pages;
  for (const auto& root : zero.record->roots) pages.insert(root.page_number);
  Check(device.Close().ok(), "close");
  return {pages.begin(), pages.end()};
}

void FlipByte(const fs::path& path, std::uint64_t offset) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  Check(fd >= 0, "open to flip");
  unsigned char byte = 0;
  Check(::pread(fd, &byte, 1, static_cast<off_t>(offset)) == 1, "read to flip");
  byte ^= 0x5a;
  Check(::pwrite(fd, &byte, 1, static_cast<off_t>(offset)) == 1 && ::fsync(fd) == 0, "flip");
  ::close(fd);
}

void LoopLifecycle() {
  // The loop alone: created, closed by a submitted request, joined; a second
  // close is a no-op; an open without a crypto owner is refused.
  auto loop = srv::NativeDatabaseLoop::Create();
  Check(loop->state() == srv::NativeLoopState::opening && !loop->on_loop_thread(), "a new loop is opening, off its thread");
  const auto refused = loop->SubmitOpen({}).get();
  Check(!refused.ok && refused.diagnostic.code == "DATABASE.OPEN_NATIVE_LOOP_REQUEST_INVALID", "an open with no crypto owner/ownership is refused");
  const auto closed = loop->Close();
  Check(closed.ok && loop->state() == srv::NativeLoopState::closed, "a submitted close tears down and joins");
  Check(loop->Close().ok, "a second close is a no-op");
  const auto late = loop->SubmitOpen({}).get();
  Check(!late.ok, "an open after close is refused");
  std::printf("loop lifecycle: create, refused open, close, second close, late open\n");
}

void ServePageSize(const std::string& self, const fs::path& root, std::uint32_t page_size) {
  const auto dir = root / ("ps" + std::to_string(page_size));
  fs::create_directory(dir);
  const auto path = dir / "serving.sdb";
  const auto created = RunChild(self, {"create", path.string(), std::to_string(page_size)});
  Check(created.status == 0, "create child " + std::to_string(page_size));
  const auto principal = created.first_line.substr(created.first_line.find('=') + 1);  // database_uuid first
  std::string service_principal;
  {
    const auto at = created.rest.find("service_principal_uuid=");
    Check(at != std::string::npos, "the create printed the service principal");
    service_principal = created.rest.substr(at + 23, 32);
  }
  (void)principal;
  const auto before = Snapshot(dir);
  Check(before.size() == 1, "native create left one file");

  // 1. serving: verified, Restart ran (S3d), write-fenced, nothing written (a clean file needs no
  //    dead-marking: 0 writes and 0 syncs even after Restart).
  const auto served = RunChild(self, {"serve", path.string()});
  const std::string tag = "[" + std::to_string(page_size) + "] ";
  Check(served.status == 0, tag + "serve child exits 0 (a wrong-thread destroy would terminate): " + served.first_line);
  Check(served.first_line.starts_with("SERVING state=serving "), tag + "serving: " + served.first_line);
  // S3d: Restart ran. (Was, S3b/S3c: restart_pending == "1", classification native_verified_restart_pending.)
  Check(Field(served.first_line, "restart_pending") == "0" && Field(served.first_line, "fenced") == "1" &&
            Field(served.first_line, "open") == "1" && Field(served.first_line, "db_state") == "open",
        tag + "verified, open, write-fenced, Restart ran: " + served.first_line);
  Check(Field(served.first_line, "restart_ran") == "1" && Field(served.first_line, "marked_dead") == "0" &&
            Field(served.first_line, "restart_writes") == "0" && Field(served.first_line, "restart_syncs") == "0" &&
            Field(served.first_line, "classification") == "native_verified_restart_clean" &&
            Field(served.first_line, "serving_state") == "serving" && Field(served.first_line, "writer_failed") == "0" &&
            Field(served.first_line, "unresolved") == "-" && Field(served.first_line, "next") == "2" &&
            Field(served.first_line, "diag") == "-",
        tag + "a clean file: Restart ran, marked 0 dead, performed 0 writes and 0 syncs; Next = 2: " + served.first_line);
  Check(Field(served.first_line, "status_serving") == "1" && Field(served.first_line, "status_restart_pending") == "1" &&
            Field(served.first_line, "status_marked_dead") == "1" && Field(served.first_line, "status_writer_failed") == "1" &&
            Field(served.first_line, "status_state") == "1",
        tag + "the status JSON reports the serving state, Restart pending false, the marked count and writer_failed false");
  Check(Field(served.first_line, "transactions") == "1", tag + "the verified inventory is the create's one transaction");
  Check(Field(served.first_line, "page_size") == std::to_string(page_size), tag + "page size reported");
  Check(Field(served.first_line, "principal") == service_principal,
        tag + "L5's credential derived from the file authenticated as the principal the file names");
  Check(Has(Field(served.first_line, "threads"), "sb-db-loop"), tag + "the database loop thread is named sb-db-loop: " + Field(served.first_line, "threads"));
  Check(Field(served.first_line, "listing") == "serving.sdb;", tag + "no file made while serving: " + Field(served.first_line, "listing"));
  Check(Field(served.first_line, "status_has_state") == "1" && Field(served.first_line, "served_owner") == "1",
        tag + "the status reports serving; the served owner resolves");
  Check(Has(served.rest, "AFTER_CLOSE reowned=true") && !Has(Field(served.rest.substr(served.rest.find("threads=")), "threads"), "sb-db-loop"),
        tag + "clean shutdown: the loop thread is gone and the flock released by the close: " + served.rest);
  Check(Snapshot(dir) == before, tag + "serving, Restart and shutdown wrote nothing (bytes unchanged) and made no file");
  ExpectOwnable(path, tag + "after shutdown");

  // 1b. S3c: served reads after custody (read-only duplicates; writable refused; eight concurrent readers).
  {
    const auto reads = RunChild(self, {"serve", path.string(), "read"});
    Check(reads.status == 0, tag + "serve-read child exits 0: " + reads.first_line + " / " + reads.rest);
    Check(reads.first_line.starts_with("SERVING state=serving "), tag + "serving before the reads: " + reads.first_line);
    const auto at = reads.rest.find("READ ");
    Check(at != std::string::npos, tag + "the served reads ran: " + reads.rest);
    const auto line = reads.rest.substr(at, reads.rest.find('\n', at) - at);
    Check(Field(line, "path_opens") == "0" && Field(line, "file_opens") == "0" && Field(line, "sidecar_opens") == "0",
          tag + "no path open after serving: " + line);
    Check(Field(line, "writable_refused") == "SB-STORAGE-DISK-SERVED-NATIVE-WRITE-REFUSED", tag + "writable refused: " + line);
    Check(Has(reads.rest, "AFTER_CLOSE reowned=true"), tag + "clean shutdown after the reads");
    Check(Snapshot(dir) == before, tag + "the served reads changed nothing");
    std::printf("[%u] %s\n", page_size, line.c_str());
  }

  // 2. a second server on the file while the first serves.
  {
    auto holder = Spawn(self, {"serve-hold", path.string()});
    const auto line = ReadLine(holder);
    Check(line.starts_with("SERVING state=serving "), tag + "the first server serves: " + line);
    const auto second = RunChild(self, {"serve", path.string()});
    Check(second.first_line.starts_with("REFUSED code=ARCH.DATABASE_MULTI_OWNER"), tag + "a second server: " + second.first_line);
    Check(Snapshot(dir) == before, tag + "the refused second server changed nothing");
    ::kill(holder.pid, SIGTERM);
    const auto rest = ReadAll(holder);
    Check(Has(rest, "AFTER_CLOSE reowned=true"), tag + "the held server shut down cleanly on SIGTERM: " + rest);
    Check(Wait(holder) == 0, tag + "the held server exits 0");
    ExpectOwnable(path, tag + "after the held server");
  }

  // 3. bytes flipped in every root page page zero names (state, allocation,
  //    inventory, catalog...). Each is refused with a specific code, the
  //    corrupted file is unchanged by the refusal and ownable, and the code
  //    names the step that caught it.
  std::set<std::string> codes;
  for (const auto page : RootPages(path)) {
    if (page == 0) continue;
    const auto offset = page * page_size + 200;
    FlipByte(path, offset);
    const auto corrupted = Snapshot(dir);
    const auto refused = RunChild(self, {"serve", path.string()});
    Check(refused.first_line.starts_with("REFUSED code=DATABASE.OPEN_") && Has(refused.first_line, " step="),
          tag + "root page " + std::to_string(page) + " corrupted is refused with a specific code and step: " + refused.first_line);
    Check(Snapshot(dir) == corrupted, tag + "the refusal left the corrupted file unchanged (sha256)");
    ExpectOwnable(path, tag + "after a refused open");
    codes.insert(refused.first_line.substr(0, refused.first_line.find(" ", 8)) + " " +
                 (Has(refused.first_line, " stage=") ? "stage=" + Field(refused.first_line, "stage") : "step=" + Field(refused.first_line, "step")));
    FlipByte(path, offset);  // restore
    Check(Snapshot(dir) == before, tag + "the byte restored");
  }
  std::string summary;
  for (const auto& code : codes) summary += "\n    " + code;
  std::printf("[%u] serving, unchanged bytes, second server, clean shutdown; root-page corruption refused at:%s\n", page_size, summary.c_str());
  Check(!codes.empty(), tag + "root pages were exercised");
}


// ---- S3d: Restart on files a crash left behind ---------------------------------------------
// SEARCH_KEY: SB_NATIVE_RESTART_TEST (plan 38 sub-step S3d, owner Q241 = 1, Q284).
std::vector<unsigned char> ReadWhole(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(in)), {});
  return {bytes.begin(), bytes.end()};
}

// Byte-for-byte equality (a size check, then memcmp; the vector == operator draws a library warning).
bool SameBytes(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b) {
  return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

// Page number of the transaction-state chain's first page (page-zero root kind 4).
std::uint64_t StatePageNumber(const fs::path& path) {
  d::FileDevice device;
  Check(device.Open(path.string(), d::FileOpenMode::open_existing_read_only, d::DatabaseFileOwnership::native_data_file).ok(),
        "open for the state root");
  const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device);
  Check(zero.ok(), "page zero reads");
  std::uint64_t page = 0;
  for (const auto& root : zero.record->roots)
    if (root.kind == 4) page = root.page_number;
  Check(device.Close().ok() && page != 0, "the state root");
  return page;
}

// The oracle: the old image with ONLY the state byte of each listed slot set to dead (9) in the first
// state page. Built from the old bytes and the transaction-state word layout (word = 4 bytes at 512 +
// 4 * order; byte 0 = state code), not from the restart planner.
std::vector<unsigned char> Oracle(std::vector<unsigned char> image, std::uint64_t state_page, std::uint32_t page_size,
                                  const std::vector<unsigned>& dead_orders) {
  for (const auto order : dead_orders) {
    const auto at = state_page * page_size + 512 + 4ull * order;
    Check(at < image.size() && image[at] != 9, "the oracle's slot is a live word in the old image");
    image[at] = 9;
  }
  return image;
}

std::string Sha(const std::vector<unsigned char>& bytes) {
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(bytes.data(), bytes.size());
  Check(digest.ok(), "sha256");
  return scratchbird::core::hash::HexLower(digest.digest);
}

// The file as an independent read-only reader (the production fixed-root lease over a read-only
// device, no server) sees the transaction inventory: order -> state name, and Next.
std::map<std::uint64_t, std::string> ReaderStates(const fs::path& path, std::uint64_t& next) {
  namespace mga = scratchbird::transaction::mga;
  d::FileDevice device;
  Check(device.Open(path.string(), d::FileOpenMode::open_existing_read_only, d::DatabaseFileOwnership::native_data_file).ok(),
        "independent read-only open");
  const auto zero = d::ReadFilespacePageZeroFromOpenDevice(device);
  Check(zero.ok(), "page zero reads");
  const auto size = zero.record->bootstrap.page_size_bytes;
  std::map<std::uint64_t, std::string> out;
  {
    const std::vector<d::NativeFilespaceDevice> files{
        {zero.record->bootstrap.filespace_uuid, zero.record->bootstrap.page_size_profile_uuid, &device}};
    std::vector<p::byte> backing(64u << 20);
    auto fixed = db::AcquireNativeFixedRootBackedLease(zero.record->bootstrap.database_uuid, files,
                                                       zero.record->bootstrap.filespace_uuid, {256ull * size, backing.size()}, backing);
    Check(fixed.ok(), "the production fixed-root lease reads the file");
    const auto& inventory = fixed.lease->inventory();
    next = inventory.next_local_transaction_id;
    for (const auto& e : inventory.entries) {
      const auto state = e.state;
      out[e.identity.local_id.value] = state == mga::TransactionState::committed ? "committed"
                                       : state == mga::TransactionState::rolled_back ? "rolled_back"
                                       : state == mga::TransactionState::active ? "active"
                                       : state == mga::TransactionState::failed_terminal ? "dead" : "other";
    }
    fixed.lease.Reset();
  }
  Check(device.Close().ok(), "close");
  return out;
}

// Every byte of `file` is the old byte or the oracle's byte, and every byte outside the state page is
// the old byte: what a process killed at any point of Restart leaves. Returns the number of 512-byte
// units of the state page that equal the oracle's (new) bytes and are not the old bytes.
unsigned OldOrNewOnly(const std::vector<unsigned char>& file, const std::vector<unsigned char>& old,
                      const std::vector<unsigned char>& expected, std::uint64_t state_page, std::uint32_t page_size,
                      const std::string& what) {
  Check(file.size() == old.size(), what + ": the file size is unchanged");
  // One pass, one Check: the first byte that is neither the old nor the new byte, or that changed outside the state page.
  std::optional<std::size_t> outside_change, neither;
  const auto lo = state_page * page_size, hi = (state_page + 1) * page_size;
  for (std::size_t i = 0; i < file.size(); ++i) {
    if (file[i] == old[i]) continue;
    if (i < lo || i >= hi) { outside_change = i; break; }
    if (file[i] != expected[i]) { neither = i; break; }
  }
  Check(!outside_change, what + ": a byte outside the transaction-state page changed at " + std::to_string(outside_change.value_or(0)));
  Check(!neither, what + ": a state-page byte is neither old nor new at " + std::to_string(neither.value_or(0)));
  unsigned fully_new = 0;
  for (std::size_t unit = state_page * page_size; unit < (state_page + 1) * page_size; unit += 512) {
    const bool is_old = std::equal(file.begin() + unit, file.begin() + unit + 512, old.begin() + unit);
    const bool is_new = std::equal(file.begin() + unit, file.begin() + unit + 512, expected.begin() + unit);
    if (is_new && !is_old) ++fully_new;
  }
  return fully_new;
}

// The state code of one transaction's word, decoded by the TIP page codec from the file's bytes (a
// slot with no header record, such as an unused slot Restart marked dead, is not an inventory entry,
// so the reader above cannot show it; the word still can).
const char* TipWordName(const fs::path& path, std::uint32_t page_size, std::uint64_t order) {
  const auto image = ReadWhole(path);
  const auto state_page = StatePageNumber(path);
  const auto decoded = pg::DecodeTransactionStatePage(
      std::span<const p::byte>(reinterpret_cast<const p::byte*>(image.data()) + state_page * page_size, page_size));
  Check(decoded.ok(), "the state page decodes");
  const auto word = pg::TipWordAt(*decoded.view, static_cast<std::uint32_t>(order));
  Check(word.has_value(), "the slot has a word");
  return pg::TipStateCodeName(word->state);
}

std::vector<unsigned> Orders(unsigned from, unsigned to) {
  std::vector<unsigned> v;
  for (unsigned n = from; n <= to; ++n) v.push_back(n);
  return v;
}

void RestartCases(const std::string& self, const fs::path& root, std::uint32_t page_size) {
  const std::string tag = "[" + std::to_string(page_size) + "] ";
  const auto make = [&](const std::string& name, const std::string& kind) {
    const auto dir = root / ("r" + std::to_string(page_size) + "_" + name);
    fs::create_directory(dir);
    const auto path = dir / "restart.sdb";
    Check(RunChild(self, {"create", path.string(), std::to_string(page_size)}).status == 0, tag + name + ": create");
    if (!kind.empty()) {
      const auto fixture = RunChild(self, {"fixture", path.string(), kind});
      Check(fixture.status == 0 && fixture.first_line == "FIXTURE ok kind=" + kind, tag + name + ": the writer built the fixture: " + fixture.first_line);
    }
    return path;
  };

  // (b) single crash states: the writer left one transaction behind; Restart marks it dead, nothing else changes.
  struct Single { const char* kind; const char* left_as; };
  for (const Single& s : {Single{"active", "active"}, Single{"unused", "unused slot below Next"},
                          Single{"header", "header record over an unused slot"}}) {
    const auto path = make(s.kind, s.kind);
    const auto old = ReadWhole(path);
    const auto state_page = StatePageNumber(path);
    const auto expected = Oracle(old, state_page, page_size, {2});
    std::uint64_t next = 0;
    const auto before = ReaderStates(path, next);
    std::string listing;
    for (const auto& [order, state] : before) listing += std::to_string(order) + "=" + state + " ";
    // The reader's view of the crash state itself (it reads an in-progress slot as active and an unused
    // slot below Next as a transaction that crashed while starting).
    Check(next == 3 && before.count(1) && before.at(1) == "committed",
          tag + s.kind + ": before Restart the reader sees Next 3 and 1 committed: " + listing);
    std::printf("%s%s before Restart, the reader sees: %s(Next %llu)\n", tag.c_str(), s.kind, listing.c_str(), static_cast<unsigned long long>(next));
    const auto run = RunChild(self, {"serve", path.string()});
    Check(run.status == 0 && run.first_line.starts_with("SERVING state=serving "), tag + s.kind + ": serves: " + run.first_line);
    Check(Field(run.first_line, "restart_ran") == "1" && Field(run.first_line, "marked_dead") == "1" &&
              Field(run.first_line, "restart_writes") == "1" && Field(run.first_line, "restart_syncs") == "1" &&
              Field(run.first_line, "next") == "3" && Field(run.first_line, "classification") == "native_verified_restarted" &&
              Field(run.first_line, "restart_pending") == "0" && Field(run.first_line, "fenced") == "1" &&
              Field(run.first_line, "unresolved") == "-" && Field(run.first_line, "writer_failed") == "0",
          tag + s.kind + " (" + s.left_as + "): Restart marked it dead with one 512-byte write and one sync: " + run.first_line);
    Check(Field(run.first_line, "status_serving") == "1" && Field(run.first_line, "status_marked_dead") == "1" &&
              Field(run.first_line, "status_restart_pending") == "1",
          tag + s.kind + ": the status reports the Restart");
    const auto after = ReadWhole(path);
    Check(SameBytes(after, expected), tag + s.kind + ": the file is byte-identical to the oracle (the old image with slot 2's state byte = dead)");
    const auto states = ReaderStates(path, next);
    Check(next == 3 && states.at(1) == "committed" &&
              (states.count(2) ? states.at(2) == "dead" : std::string(s.kind) == "unused"),
          tag + s.kind + ": an independent TIP reader sees 1 committed, Next 3 and 2 dead (an unused slot has no header, so no entry)");
    Check(std::string(TipWordName(path, page_size, 2)) == "dead" && std::string(TipWordName(path, page_size, 1)) == "committed",
          tag + s.kind + ": the TIP page codec decodes word 2 as dead and word 1 as committed");
    // Restart is once per open and idempotent: a second open finds nothing to mark and writes nothing.
    const auto again = RunChild(self, {"serve", path.string()});
    Check(Field(again.first_line, "marked_dead") == "0" && Field(again.first_line, "restart_writes") == "0" &&
              Field(again.first_line, "classification") == "native_verified_restart_clean" && SameBytes(ReadWhole(path), expected),
          tag + s.kind + ": a second open marks 0 and writes 0, the file unchanged: " + again.first_line);
  }

  // (b2) a mixed inventory: 2 active, 3 committed, 4 rolled back, 5 active, 6 a header over an unused slot.
  {
    const auto path = make("mixed", "mixed");
    const auto old = ReadWhole(path);
    const auto state_page = StatePageNumber(path);
    const auto expected = Oracle(old, state_page, page_size, {2, 5, 6});
    const auto run = RunChild(self, {"serve", path.string()});
    Check(run.status == 0 && Field(run.first_line, "marked_dead") == "3" && Field(run.first_line, "next") == "7" &&
              Field(run.first_line, "restart_writes") == "1" && Field(run.first_line, "serving_state") == "serving",
          tag + "mixed: Restart marked 2, 5 and 6 dead: " + run.first_line);
    Check(SameBytes(ReadWhole(path), expected), tag + "mixed: the file equals the oracle: only the dead slots' state bytes changed");
    std::uint64_t next = 0;
    const auto states = ReaderStates(path, next);
    Check(next == 7 && states.at(2) == "dead" && states.at(3) == "committed" && states.at(4) == "rolled_back" &&
              states.at(5) == "dead" && states.at(6) == "dead",
          tag + "mixed: an independent reader sees dead, committed, rolled back, dead, dead");
  }

  // (c) kill -9 / torn write during Restart, then a reopen completes it. Three 512-byte state units are dirty.
  {
    std::map<std::string, unsigned> seen;
    for (const std::string fault : {"kill=0", "kill=1", "kill=2", "kill=3", "torn=1", "torn=2", "torn=3"}) {
      // Each file is its own database (identities, times): its oracle comes from its own old image.
      const auto path = make("crash_" + fault.substr(0, 4) + fault.substr(5), "many");
      const auto old = ReadWhole(path);
      const auto state_page = StatePageNumber(path);
      const auto expected = Oracle(old, state_page, page_size, Orders(2, 258));
      const auto killed = RunChild(self, {"serve", path.string(), fault});
      Check(killed.status == 128 + SIGKILL, tag + fault + ": the server process was killed by SIGKILL mid-Restart (status " + std::to_string(killed.status) + "): " + killed.first_line);
      const auto crashed = ReadWhole(path);
      const unsigned new_units = OldOrNewOnly(crashed, old, expected, state_page, page_size, tag + fault);
      if (fault.starts_with("kill=")) Check(new_units == static_cast<unsigned>(fault[5] - '0'), tag + fault + ": exactly " + fault.substr(5) + " units are written: " + std::to_string(new_units));
      // The reopen: verification admits the crash state and Restart completes it, whatever the point.
      const auto reopened = RunChild(self, {"serve", path.string()});
      Check(reopened.status == 0 && reopened.first_line.starts_with("SERVING state=serving ") &&
                Field(reopened.first_line, "restart_ran") == "1" && Field(reopened.first_line, "serving_state") == "serving" &&
                Field(reopened.first_line, "next") == "259" && Field(reopened.first_line, "writer_failed") == "0",
            tag + fault + ": the reopen verifies and serves: " + reopened.first_line);
      Check(SameBytes(ReadWhole(path), expected), tag + fault + ": after the reopen the file equals the oracle (the same final image whatever the crash point)");
      std::uint64_t next = 0;
      const auto states = ReaderStates(path, next);
      Check(next == 259 && states.at(1) == "committed" && states.at(2) == "dead" && states.at(258) == "dead",
            tag + fault + ": an independent reader sees every in-progress slot dead");
      seen[fault] = new_units;
    }
    std::string summary;
    for (const auto& [k, v] : seen) summary += " " + k + "->" + std::to_string(v) + "new";
    std::printf("%s kill -9 / torn Restart, reopened to the same final image:%s\n", tag.c_str(), summary.c_str());

    // (d) an injected write / sync failure: the database goes to the failed serving state, fenced, the
    // status shows it, and a reopen recovers.
    for (const std::string fault : {"fail=1", "fail=2", "fail=3", "failsync=1"}) {
      const auto path = make("fail_" + fault.substr(0, 5) + fault.substr(fault.find('=') + 1), "many");
      const auto old = ReadWhole(path);
      const auto state_page = StatePageNumber(path);
      const auto expected = Oracle(old, state_page, page_size, Orders(2, 258));
      const auto failed = RunChild(self, {"serve", path.string(), fault});
      Check(failed.status == 0 && failed.first_line.starts_with("REFUSED code=DATABASE.RESTART_WRITE_FAILED") &&
                Has(failed.first_line, "requires=reopen"),
            tag + fault + ": a failed write is refused with the specific code: " + failed.first_line);
      const auto at = failed.rest.find("STATE ");
      Check(at != std::string::npos, tag + fault + ": the failed state is reported: " + failed.rest);
      const auto state = failed.rest.substr(at, failed.rest.find('\n', at) - at);
      Check(Field(state, "serving_state") == "failed" && Field(state, "db_state") == "failed" && Field(state, "fenced") == "1" &&
                Field(state, "writer_failed") == "1" && Field(state, "restart_pending") == "1" && Field(state, "restart_ran") == "0" &&
                Field(state, "diag") == "DATABASE.RESTART_WRITE_FAILED" && Field(state, "status_serving") == "1" &&
                Field(state, "status_writer_failed") == "1" && Field(state, "status_diag") == "1" && Field(state, "status_state") == "1" &&
                Field(state, "classification") == "native_restart_write_failed_reopen_required",
            tag + fault + ": failed, fenced, writer failed, status shows it: " + state);
      Check(Has(failed.rest, "AFTER_CLOSE reowned=true"), tag + fault + ": the failed database still closes cleanly (flock released)");
      const unsigned written = OldOrNewOnly(ReadWhole(path), old, expected, state_page, page_size, tag + fault);
      if (fault.starts_with("fail=")) Check(written == static_cast<unsigned>(fault[5] - '0') - 1, tag + fault + ": the units written before the failure: " + std::to_string(written));
      if (fault == "failsync=1") Check(written == 3, tag + fault + ": every unit reached the file, the barrier failed");
      const auto reopened = RunChild(self, {"serve", path.string()});
      Check(reopened.status == 0 && reopened.first_line.starts_with("SERVING state=serving ") &&
                Field(reopened.first_line, "serving_state") == "serving" && Field(reopened.first_line, "writer_failed") == "0" &&
                Field(reopened.first_line, "restart_pending") == "0",
            tag + fault + ": a reopen recovers: " + reopened.first_line);
      Check(SameBytes(ReadWhole(path), expected), tag + fault + ": after the reopen the file equals the oracle");
    }
    std::printf("%s injected write/sync failures: failed state, fenced, status, reopen recovers\n", tag.c_str());
  }

  // (e) unresolved committing / recovering slots: restricted, specific code, NOTHING written (not even the
  // dead-markable slot 3 beside them); prepared / limbo are in doubt, not unresolved: served, left alone.
  for (const std::string kind : {"committing", "recovering", "committing_active"}) {
    const auto path = make("unres_" + kind, kind);
    const auto old = ReadWhole(path);
    const auto run = RunChild(self, {"serve", path.string()});
    Check(run.status == 0 && run.first_line.starts_with("SERVING state=restricted db_state=restricted_open "),
          tag + kind + ": opened restricted, not served normally: " + run.first_line);
    Check(Field(run.first_line, "restart_pending") == "1" && Field(run.first_line, "fenced") == "1" &&
              Field(run.first_line, "restart_ran") == "0" && Field(run.first_line, "marked_dead") == "0" &&
              Field(run.first_line, "restart_writes") == "0" && Field(run.first_line, "restart_syncs") == "0" &&
              Field(run.first_line, "unresolved") == "2" && Field(run.first_line, "serving_state") == "restricted" &&
              Field(run.first_line, "uncertain") == "1" &&  // the verification's count includes the committing slot itself
              Field(run.first_line, "diag") == "DATABASE.OPEN_RESTRICTED_UNRESOLVED_TRANSACTIONS" &&
              Field(run.first_line, "classification") == "native_verified_restart_unresolved_restricted" &&
              Field(run.first_line, "status_serving") == "1" && Field(run.first_line, "status_diag") == "1",
          tag + kind + ": restricted with the specific code and the unresolved order reported: " + run.first_line);
    Check(SameBytes(ReadWhole(path), old), tag + kind + ": the restricted open wrote nothing");
    Check(Has(run.rest, "AFTER_CLOSE reowned=true"), tag + kind + ": clean shutdown");
  }
  for (const std::string kind : {"prepared", "limbo"}) {
    const auto path = make("indoubt_" + kind, kind);
    const auto old = ReadWhole(path);
    const auto run = RunChild(self, {"serve", path.string()});
    Check(run.status == 0 && run.first_line.starts_with("SERVING state=serving ") && Field(run.first_line, "uncertain") == "1" &&
              Field(run.first_line, "marked_dead") == "0" && Field(run.first_line, "restart_writes") == "0" &&
              Field(run.first_line, "unresolved") == "-" && SameBytes(ReadWhole(path), old),
          tag + kind + ": in doubt (left alone, reported), not unresolved; served, 0 writes: " + run.first_line);
  }
  std::printf("%s restart: single crash states, mixed, kill/torn sweep, failures, unresolved (restricted), in doubt\n", tag.c_str());
}

void L5SeedCases(const std::string& self, const fs::path& root) {
  const std::uint32_t page_size = 16384;
  for (const std::string kind : {"seedless", "mismatched"}) {
    const auto dir = root / kind;
    fs::create_directory(dir);
    const auto path = dir / "l5.sdb";
    Check(RunChild(self, {"create-lowlevel", path.string(), std::to_string(page_size), kind}).status == 0, kind + " create");
    const auto before = Snapshot(dir);
    const auto refused = RunChild(self, {"serve", path.string()});
    if (kind == "seedless")
      Check(refused.first_line.starts_with("REFUSED code=DATABASE.OPEN_NATIVE_L5_IDENTITY_REFUSED") &&
                Has(refused.first_line, "error=seed_absent"),
            "a file whose L5 row carries no seed: " + refused.first_line);
    else
      Check(refused.first_line.starts_with("REFUSED code=DATABASE.OPEN_NATIVE_L5_CREDENTIAL_REFUSED") &&
                Has(refused.first_line, "credential_error=credential_verification") &&
                Has(refused.first_line, "verification_error=credential_rejected"),
            "a file whose L5 seed does not match its stored verifier: " + refused.first_line);
    Check(Snapshot(dir) == before, kind + ": the file is unchanged");
    ExpectOwnable(path, kind);
    std::printf("L5 %s: %s\n", kind.c_str(), refused.first_line.c_str());
  }
}

void RefusalsBeforeTheLoop(const std::string& self, const fs::path& root) {
  const auto dir = root / "refusals";
  fs::create_directory(dir);
  const auto path = dir / "refusals.sdb";
  Check(RunChild(self, {"create", path.string(), "16384"}).status == 0, "create for the refusals");
  const auto before = Snapshot(dir);
  struct Case {
    const char* variant;
    const char* code;
  };
  for (const Case& c : {Case{"keyed", "DATABASE.OPEN_DECRYPTION_UNAVAILABLE"},
                        Case{"readonly", "DATABASE.OPEN_NATIVE_MODE_UNAVAILABLE"},
                        Case{"nocrypto", "DATABASE.OPEN_NATIVE_CRYPTO_OWNER_REQUIRED"}}) {
    const auto run = RunChild(self, {"serve", path.string(), c.variant});
    Check(run.first_line.starts_with(std::string("REFUSED code=") + c.code), std::string(c.variant) + ": " + run.first_line);
    Check(Snapshot(dir) == before, std::string(c.variant) + ": the file is unchanged");
    ExpectOwnable(path, c.variant);
  }
  std::printf("refusals before the loop: configured key, non-normal mode, no crypto owner\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) {
    const std::string mode = argv[1];
    if (mode == "create" && argc == 4) return CreateMode(argv[2], static_cast<std::uint32_t>(std::stoul(argv[3])));
    if (mode == "create-lowlevel" && argc == 5)
      return CreateLowLevelMode(argv[2], static_cast<std::uint32_t>(std::stoul(argv[3])), argv[4]);
    if (mode == "fixture" && argc == 4) return FixtureMode(argv[2], argv[3]);
    if (mode == "serve" && (argc == 3 || argc == 4)) return ServeMode(argv[2], argc == 4 ? argv[3] : "", false);
    if (mode == "serve-hold" && argc == 3) return ServeMode(argv[2], "", true);
    if (mode == "memory-defaults" && argc == 2) {
      // Q282 = 1: the three in-code defaults, for the default-SBsrv test to compare with the shipped files.
      std::cout << "MEMORY_DEFAULTS bootstrap_config=" << srv::ServerBootstrapConfig{}.memory_per_context_limit_bytes
                << " engine_policy=" << scratchbird::core::memory::DefaultLocalEngineMemoryPolicy().per_context_limit_bytes
                << " policy_config=" << scratchbird::core::memory::MemoryPolicyConfig{}.per_context_limit_bytes << std::endl;
      return 0;
    }
    if (mode != "common" && mode != "size") return 2;  // those two are handled below (they need the temp directory)
  }
  const std::string self = fs::read_symlink("/proc/self/exe").string();
  scratchbird::tests::OwnedTempDirectory artifacts;
  // SB_NATIVE_SERVING_SPLIT (owner Q304, 2026-11-19): the five-size run takes ~785 s under -j8 of a 1800 s timeout, so
  // ctest runs it as separate targets with the SAME checks, each in its own process and temp directory:
  //   size <bytes>  ServePageSize + RestartCases at that one page size (8192..131072), registered per size;
  //   common        LoopLifecycle + L5SeedCases + RefusalsBeforeTheLoop (the page-size independent cases, which
  //                 use their own fixed sizes), registered once.
  // The no-argument run below is the sum of those (kept for a manual full run; no ctest entry).
  if (argc == 2 && std::string(argv[1]) == "common") {
    LoopLifecycle();
    L5SeedCases(self, artifacts.path());
    RefusalsBeforeTheLoop(self, artifacts.path());
    artifacts.Cleanup();
    std::printf("native_serving[common]: %d checks passed\n", checks);
    return 0;
  }
  if (argc == 3 && std::string(argv[1]) == "size") {
    const auto page_size = static_cast<std::uint32_t>(std::stoul(argv[2]));
    if (page_size != 8192 && page_size != 16384 && page_size != 32768 && page_size != 65536 && page_size != 131072) return 2;
    ServePageSize(self, artifacts.path(), page_size);
    RestartCases(self, artifacts.path(), page_size);
    artifacts.Cleanup();
    std::printf("native_serving[%u]: %d checks passed\n", page_size, checks);
    return 0;
  }
  if (argc != 1) return 2;
  LoopLifecycle();
  for (const std::uint32_t page_size : {8192u, 16384u, 32768u, 65536u, 131072u})
    ServePageSize(self, artifacts.path(), page_size);
  for (const std::uint32_t page_size : {8192u, 16384u, 32768u, 65536u, 131072u})
    RestartCases(self, artifacts.path(), page_size);
  L5SeedCases(self, artifacts.path());
  RefusalsBeforeTheLoop(self, artifacts.path());
  artifacts.Cleanup();
  std::printf("native_serving: %d checks passed\n", checks);
  return 0;
}
