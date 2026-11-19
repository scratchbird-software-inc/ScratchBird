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
// file (database UUID XOR the stored seed) and authenticated, Restart (the TIP,
// S3d) not run, nothing written.
//
// The process gate (HostedProcessAdmission) binds the process permanently after
// one successful open, and the crypto owner is process-start state, so every
// open runs in its own child process: this binary re-executes itself with a
// mode. Modes (each prints one line, "SERVING ..." or "REFUSED code=..."):
//   create <path> <page_size>            engine-API native create; prints identities
//   create-lowlevel <path> <ps> <kind>   storage-level create: kind seedless | mismatched
//   serve <path> [variant]               one StartHostedEngine, then a clean shutdown
//   serve-hold <path>                    serve, wait for SIGTERM, shut down
// With no argument the binary drives the cases for all five page sizes
// (8K..128K) and checks: serving; the file's bytes unchanged (no write before
// Restart) and no file made; the loop thread named sb-db-loop; L5 authenticated
// as the principal the file names; a second server on the same file refused
// (ARCH.DATABASE_MULTI_OWNER); clean shutdown on the loop thread (a wrong-thread
// destroy terminates the process) with the flock released; a byte flipped in
// every page-zero root page refused with a specific code, the file unchanged;
// an L5 seed row absent or inconsistent refused with a specific code.
// The strace half (0 ".sb." syscalls, 0 lock files, 0 pwrite/fsync/fdatasync on
// the data descriptor) is native_serving_strace.py.

#include "lifecycle/engine_lifecycle_api.hpp"
#include "hash_digest.hpp"
#include "database_lifecycle.hpp"
#include "native_database_loop.hpp"
#include "native_database_open_discovery.hpp"
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

#include <csignal>
#include <pthread.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace p = scratchbird::core::platform;
namespace d = scratchbird::storage::disk;
namespace db = scratchbird::storage::database;
namespace srv = scratchbird::server;
namespace fs = std::filesystem;

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
    for (std::size_t i = 0; i < create.service_password_seed.size(); ++i)
      create.service_password_seed[i] = static_cast<p::byte>(0x11 + i);
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

// One StartHostedEngine in this process, as a server process does: the process
// crypto owner started first (it outlives the hosted state), then the open.
// variant: "" normal | keyed | readonly | nocrypto.
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
  if (variant == "keyed") config.startup_credential_source = "operator_file";
  if (variant == "readonly") config.database_open_mode = "read_only";
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
                << std::endl;
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
    std::cout << "AFTER_CLOSE reowned=" << (again.acquired ? "true" : "false") << " threads=" << ThreadNames() << std::endl;
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

  // 1. serving: verified, write-fenced, Restart pending, nothing written.
  const auto served = RunChild(self, {"serve", path.string()});
  const std::string tag = "[" + std::to_string(page_size) + "] ";
  Check(served.status == 0, tag + "serve child exits 0 (a wrong-thread destroy would terminate): " + served.first_line);
  Check(served.first_line.starts_with("SERVING state=serving "), tag + "serving: " + served.first_line);
  Check(Field(served.first_line, "restart_pending") == "1" && Field(served.first_line, "fenced") == "1" &&
            Field(served.first_line, "open") == "1" && Field(served.first_line, "db_state") == "open",
        tag + "verified, open, write-fenced, Restart pending: " + served.first_line);
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
  Check(Snapshot(dir) == before, tag + "serving and shutdown wrote nothing (bytes unchanged) and made no file");
  ExpectOwnable(path, tag + "after shutdown");

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
    if (mode == "serve" && (argc == 3 || argc == 4)) return ServeMode(argv[2], argc == 4 ? argv[3] : "", false);
    if (mode == "serve-hold" && argc == 3) return ServeMode(argv[2], "", true);
    return 2;
  }
  const std::string self = fs::read_symlink("/proc/self/exe").string();
  scratchbird::tests::OwnedTempDirectory artifacts;
  LoopLifecycle();
  for (const std::uint32_t page_size : {8192u, 16384u, 32768u, 65536u, 131072u})
    ServePageSize(self, artifacts.path(), page_size);
  L5SeedCases(self, artifacts.path());
  RefusalsBeforeTheLoop(self, artifacts.path());
  artifacts.Cleanup();
  std::printf("native_serving: %d checks passed\n", checks);
  return 0;
}
