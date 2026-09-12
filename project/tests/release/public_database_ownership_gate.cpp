// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_ownership.hpp"
#include "database_lifecycle.hpp"
#include "disk_device.hpp"
#include "memory.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <cerrno>
#include <chrono>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace uuid = scratchbird::core::uuid;
namespace server = scratchbird::server;

bool Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

std::filesystem::path TempRoot() {
  static const auto root = [] {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      const auto candidate = std::filesystem::temp_directory_path() /
          ("scratchbird_database_ownership_" + std::to_string(stamp) +
           "_" + std::to_string(attempt));
      std::error_code error;
      if (std::filesystem::create_directory(candidate, error)) {
        std::filesystem::permissions(candidate, std::filesystem::perms::owner_all);
        std::cerr << "ownership fixture retained: " << candidate << '\n';
        return candidate;
      }
      if (error && error != std::errc::file_exists) break;
    }
    std::cerr << "cannot create exclusive ownership fixture directory\n";
    std::exit(EXIT_FAILURE);
  }();
  return root;
}

void CreateRealDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig config;
  config.path = path.string();
  const auto database = uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::database, 1779420001000);
  const auto filespace = uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::filespace, 1779420001001);
  if (!Expect(database.ok() && filespace.ok(), "fixture UUID generation failed"))
    std::exit(EXIT_FAILURE);
  config.database_uuid = database.value;
  config.filespace_uuid = filespace.value;
  config.page_size = 16384;
  config.creation_unix_epoch_millis = 1779420001002;
  // Storage regression only: not credentialed public CREATE DATABASE evidence.
  config.allow_minimal_resource_bootstrap = true;
  config.require_resource_seed_pack = false;
  const auto created = db::CreateDatabaseFile(config);
  if (!Expect(created.ok(), "real ownership fixture create failed")) {
    std::cerr << created.diagnostic.diagnostic_code << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::string ReadBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!Expect(input.is_open(), "fixture byte oracle cannot open file"))
    std::exit(EXIT_FAILURE);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

server::DatabaseOwnershipRequest OwnershipRequest(const std::filesystem::path& path,
                                                  std::string owner_kind) {
  server::DatabaseOwnershipRequest request;
  request.database_path = path;
  request.owner_kind = std::move(owner_kind);
  request.sbps_endpoint = TempRoot() / "sbps.sock";
  return request;
}

bool StorageMutableConflictsFailClosed() {
  const auto path = TempRoot() / "storage-conflict.sbdb";
  CreateRealDatabase(path);

  disk::FileDevice writer;
  const auto writer_open = writer.Open(path.string(), disk::FileOpenMode::open_existing);
  if (!Expect(writer_open.ok(), "first mutable storage open should acquire owner token")) {
    return false;
  }

  disk::FileDevice second_writer;
  const auto second_writer_open =
      second_writer.Open(path.string(), disk::FileOpenMode::open_existing);
  bool ok = Expect(!second_writer_open.ok(),
                   "second mutable storage open must fail closed");
  ok = Expect(second_writer_open.diagnostic.diagnostic_code ==
                  "SB-STORAGE-DISK-OWNER-LOCK-HELD",
              "second mutable storage open used wrong diagnostic") && ok;

  disk::FileDevice read_only_during_writer;
  const auto read_only_open =
      read_only_during_writer.Open(path.string(),
                                   disk::FileOpenMode::open_existing_read_only);
  ok = Expect(!read_only_open.ok(),
              "read-only storage inspection during mutable ownership must fail closed") && ok;
  ok = Expect(read_only_open.diagnostic.diagnostic_code ==
                  "SB-STORAGE-DISK-OWNER-LOCK-HELD",
              "read-only conflict used wrong diagnostic") && ok;

  ok = Expect(writer.Close().ok(), "first mutable storage close should succeed") && ok;
  return ok;
}

bool StorageReadOnlyOwnershipIsExclusive() {
  const auto path = TempRoot() / "storage-readonly.sbdb";
  CreateRealDatabase(path);

  disk::FileDevice reader_a;
  const auto reader_a_open =
      reader_a.Open(path.string(), disk::FileOpenMode::open_existing_read_only);
  if (!Expect(reader_a_open.ok(), "first read-only storage open should succeed")) {
    return false;
  }

  disk::FileDevice reader_b;
  const auto reader_b_open =
      reader_b.Open(path.string(), disk::FileOpenMode::open_existing_read_only);
  bool ok = Expect(!reader_b_open.ok() &&
                       reader_b_open.diagnostic.diagnostic_code ==
                           "SB-STORAGE-DISK-OWNER-LOCK-HELD",
                   "independent second read-only storage open must fail closed");

  disk::FileDevice writer;
  const auto writer_open = writer.Open(path.string(), disk::FileOpenMode::open_existing);
  ok = Expect(!writer_open.ok(), "mutable storage open during read-only ownership must fail closed") && ok;
  ok = Expect(writer_open.diagnostic.diagnostic_code ==
                  "SB-STORAGE-DISK-OWNER-LOCK-HELD",
              "mutable open during read-only ownership used wrong diagnostic") && ok;

  if (reader_b_open.ok()) (void)reader_b.Close();
  ok = Expect(reader_a.Close().ok(), "first read-only storage close should succeed") && ok;
  return ok;
}

bool ServerOwnershipConflictsFailClosed() {
  const auto path = TempRoot() / "server-conflict.sbdb";
  CreateRealDatabase(path);

  auto first = server::AcquireDatabaseOwnership(OwnershipRequest(path, "server"));
  if (!Expect(first.acquired, "first server ownership acquisition should succeed") ||
      !Expect(first.lock && first.lock->valid(), "first server ownership lock is invalid")) {
    return false;
  }

  const auto descriptor = server::ReadDatabaseOwnershipDescriptor(first.lock_path);
  bool ok = Expect(descriptor.format == "SB_DATABASE_OWNERSHIP_V1",
                   "server ownership descriptor format mismatch");
  ok = Expect(descriptor.owner_kind == "server",
              "server ownership descriptor owner_kind mismatch") && ok;
  ok = Expect(descriptor.database_path == path.string(),
              "server ownership descriptor database path mismatch") && ok;
  ok = Expect(!descriptor.pid.empty(),
              "server ownership descriptor must record process identity") && ok;

  auto second = server::AcquireDatabaseOwnership(OwnershipRequest(path, "maintenance"));
  ok = Expect(!second.acquired,
              "second server ownership acquisition must fail closed") && ok;
  ok = Expect(second.diagnostic_code == "ARCH.DATABASE_MULTI_OWNER",
              "server ownership conflict used wrong diagnostic") && ok;
  ok = Expect(second.incumbent.owner_kind == "server",
              "server ownership conflict did not expose incumbent descriptor") && ok;

  return ok;
}

#ifndef _WIN32


bool ReadExact(int fd, char* data, std::size_t count) {
  while (count != 0) {
    const auto got = ::read(fd, data, count);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) return false;
    data += got;
    count -= static_cast<std::size_t>(got);
  }
  return true;
}

bool WriteExact(int fd, const char* data, std::size_t count) {
  while (count != 0) {
    const auto wrote = ::write(fd, data, count);
    if (wrote < 0 && errno == EINTR) continue;
    if (wrote <= 0) return false;
    data += wrote;
    count -= static_cast<std::size_t>(wrote);
  }
  return true;
}

server::DatabaseOwnershipRequest ChildOwnershipRequest(
    const std::filesystem::path& path) {
  server::DatabaseOwnershipRequest request;
  request.database_path = path;
  request.sbps_endpoint = path.parent_path() / "child-route.sock";
  return request;
}

bool ChildServerProbe(const std::filesystem::path& path, bool expect_available) {
  auto ownership = server::AcquireDatabaseOwnership(ChildOwnershipRequest(path));
  if (!Expect(ownership.acquired == expect_available,
              "server ownership bypassed the independent storage owner")) return false;
  if (!expect_available) {
    return Expect(!ownership.lock &&
                      ownership.diagnostic_code == "ARCH.DATABASE_MULTI_OWNER",
                  "server/storage conflict did not refuse with the ownership diagnostic");
  }
  db::DatabaseOpenConfig config;
  config.path = path.string();
  config.read_only = true;
  return Expect(ownership.lock && ownership.lock->valid() &&
                    db::OpenDatabaseFile(config).ok(),
                "acquired server route cannot read its real database");
}

bool RunServerProbe(const char* self_path, const std::filesystem::path& path,
                    bool expect_available) {
  const pid_t child = ::fork();
  if (!Expect(child >= 0, "server probe fork failed")) return false;
  if (child == 0) {
    ::execl(self_path, self_path, "--probe-server", path.c_str(),
            expect_available ? "available" : "refused", static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  pid_t waited;
  do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  return Expect(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "server/storage ownership probe failed");
}

bool StorageOwnerBlocksServer(const char* self_path, bool read_only) {
  const auto path = TempRoot() /
      (read_only ? "reader-blocks-server.sbdb" : "writer-blocks-server.sbdb");
  CreateRealDatabase(path);
  // Use a genuine prior descriptor to detect premature ownership publication.
  auto previous = server::AcquireDatabaseOwnership(OwnershipRequest(path, "server"));
  if (!Expect(previous.acquired, "prior route descriptor setup failed")) return false;
  previous.lock.reset();
  const auto route_path = server::DatabaseOwnershipLockPath(path);
  const auto descriptor_before = ReadBytes(route_path);
  const auto database_before = ReadBytes(path);
  disk::FileDevice owner;
  if (!Expect(owner.Open(path.string(), read_only
                             ? disk::FileOpenMode::open_existing_read_only
                             : disk::FileOpenMode::open_existing).ok(),
              "incumbent storage owner setup failed")) return false;
  bool ok = RunServerProbe(self_path, path, false);
  ok = Expect(ReadBytes(route_path) == descriptor_before,
              "refused server changed incumbent route descriptor") && ok;
  ok = Expect(ReadBytes(path) == database_before,
              "refused server changed database bytes") && ok;
  ok = Expect(owner.Close().ok(), "storage owner close failed") && ok;
  ok = RunServerProbe(self_path, path, true) && ok;
  return Expect(ReadBytes(path) == database_before,
                "server read-only handoff changed database bytes") && ok;
}

bool StorageLockHeld(const std::filesystem::path& path, bool expected) {
  const auto lock_path = path.string() + ".sb.owner.lock";
  const int fd = ::open(lock_path.c_str(), O_RDWR | O_CLOEXEC);
  if (!Expect(fd >= 0, "storage lock inspection open failed")) return false;
  const bool acquired = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
  const int error = errno;
  if (acquired) ::flock(fd, LOCK_UN);
  ::close(fd);
  return Expect(expected ? (!acquired && (error == EWOULDBLOCK || error == EAGAIN))
                         : acquired,
                "server did not retain/release the actual storage lock");
}

bool ServerStorageLockMoveAndRelease() {
  const auto path = TempRoot() / "server-lock-move.sbdb";
  CreateRealDatabase(path);
  auto owner = server::AcquireDatabaseOwnership(OwnershipRequest(path, "server"));
  if (!Expect(owner.acquired, "server move fixture acquisition failed")) return false;
  bool ok = StorageLockHeld(path, true);
  server::DatabaseOwnershipLock moved(std::move(*owner.lock));
  ok = Expect(!owner.lock->valid() && moved.valid(), "move constructor lost ownership") && ok;
  ok = StorageLockHeld(path, true) && ok;
  *owner.lock = std::move(moved);
  ok = Expect(!moved.valid() && owner.lock->valid(), "move assignment lost ownership") && ok;
  ok = StorageLockHeld(path, true) && ok;
  owner.lock->release();
  ok = Expect(!owner.lock->valid(), "released route still reported valid") && ok;
  owner.lock->release();  // Idempotent release must not close an unrelated fd.
  return StorageLockHeld(path, false) && ok;
}


bool ForkedReleasePreservesParentOwner(const char* self_path) {
  const auto path = TempRoot() / "fork-release.sbdb";
  CreateRealDatabase(path);
  const auto before = ReadBytes(path);
  auto owner = server::AcquireDatabaseOwnership(OwnershipRequest(path, "server"));
  if (!Expect(owner.acquired, "fork release owner setup failed")) return false;
  const pid_t child = ::fork();
  if (!Expect(child >= 0, "fork release child creation failed")) return false;
  if (child == 0) {
    const bool inherited_is_invalid = !owner.lock->valid();
    owner.lock->release();
    _exit(inherited_is_invalid ? EXIT_SUCCESS : EXIT_FAILURE);
  }
  int status = 0;
  pid_t waited;
  do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  bool ok = Expect(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                   "forked child treated inherited ownership as its own");
  ok = StorageLockHeld(path, true) && ok;
  ok = RunServerProbe(self_path, path, false) && ok;
  owner.lock.reset();
  ok = RunServerProbe(self_path, path, true) && ok;
  return Expect(ReadBytes(path) == before,
                "fork release changed durable database bytes") && ok;
}

bool StorageOpenFailureDoesNotPublish() {
  const auto path = TempRoot() / "storage-open-failure.sbdb";
  CreateRealDatabase(path);
  auto previous = server::AcquireDatabaseOwnership(OwnershipRequest(path, "server"));
  if (!Expect(previous.acquired, "failure fixture route setup failed")) return false;
  previous.lock.reset();
  const auto before = ReadBytes(path);
  const auto route_before = ReadBytes(server::DatabaseOwnershipLockPath(path));
  const auto lock_path = path.string() + ".sb.owner.lock";
  const auto saved_path = lock_path + ".saved";
  // Preserve this fixture's original inode; restore it after the negative case.
  std::filesystem::rename(lock_path, saved_path);
  std::filesystem::create_directory(lock_path);
  const auto failed = server::AcquireDatabaseOwnership(OwnershipRequest(path, "server"));
  bool ok = Expect(!failed.acquired && !failed.lock &&
                       failed.diagnostic_code == "ARCH.DATABASE_OWNERSHIP_LOCK_FAILED" &&
                       failed.diagnostic_detail.find("storage_owner_open_failed:") == 0,
                   "storage-lock open failure was falsely published as ownership");
  ok = Expect(ReadBytes(server::DatabaseOwnershipLockPath(path)) == route_before,
              "storage-lock open failure changed route descriptor") && ok;
  std::filesystem::remove(lock_path);  // Only the empty directory created above.
  std::filesystem::rename(saved_path, lock_path);
  auto retried = server::AcquireDatabaseOwnership(OwnershipRequest(path, "server"));
  ok = Expect(retried.acquired, "storage-lock failure leaked route ownership") && ok;
  retried.lock.reset();
  return Expect(ReadBytes(path) == before, "storage-lock failure changed database bytes") && ok;
}

bool ChildDescriptorWriteFailure(const std::filesystem::path& path) {
  struct rlimit original{};
  if (!Expect(::getrlimit(RLIMIT_FSIZE, &original) == 0,
              "read file-size limit failed")) return false;
  auto restricted = original;
  restricted.rlim_cur = 0;
  ::signal(SIGXFSZ, SIG_IGN);
  if (!Expect(::setrlimit(RLIMIT_FSIZE, &restricted) == 0,
              "install descriptor write fault failed")) return false;
  const auto failed = server::AcquireDatabaseOwnership(ChildOwnershipRequest(path));
  const bool restored = ::setrlimit(RLIMIT_FSIZE, &original) == 0;
  bool ok = Expect(restored && !failed.acquired && !failed.lock &&
                       failed.diagnostic_code == "ARCH.DATABASE_OWNERSHIP_LOCK_FAILED" &&
                       failed.diagnostic_detail.find("descriptor_write_failed:") == 0,
                   "real descriptor write failure was not refused");
  ok = StorageLockHeld(path, false) && ok;
  const auto retried = server::AcquireDatabaseOwnership(ChildOwnershipRequest(path));
  return Expect(retried.acquired, "descriptor write failure leaked either lock") && ok;
}

bool DescriptorWriteFailureReleasesLocks(const char* self_path) {
  const auto path = TempRoot() / "descriptor-write-failure.sbdb";
  CreateRealDatabase(path);
  const auto before = ReadBytes(path);
  const pid_t child = ::fork();
  if (!Expect(child >= 0, "descriptor fault fork failed")) return false;
  if (child == 0) {
    ::execl(self_path, self_path, "--descriptor-write-failure", path.c_str(),
            static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  pid_t waited;
  do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  bool ok = Expect(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                   "descriptor write fault child failed");
  return Expect(ReadBytes(path) == before, "descriptor write failure changed database bytes") && ok;
}

// The winner retains its actual lock until both competitors have reported.
// Thus two winners cannot be explained by a sequential release/reacquire.
bool ChildOwnerRace(const std::filesystem::path& path, bool server_owner,
                    int ready, int go, int result, int release) {
  if (!WriteExact(ready, "R", 1)) return false;
  ::close(ready);
  char token = 0;
  if (!ReadExact(go, &token, 1)) return false;
  ::close(go);
  disk::FileDevice device;
  server::DatabaseOwnershipResult route;
  bool acquired;
  bool refused;
  if (server_owner) {
    route = server::AcquireDatabaseOwnership(ChildOwnershipRequest(path));
    acquired = route.acquired;
    refused = !acquired && route.diagnostic_code == "ARCH.DATABASE_MULTI_OWNER";
  } else {
    const auto opened = device.Open(path.string(), disk::FileOpenMode::open_existing_read_only);
    acquired = opened.ok();
    refused = !acquired &&
        (opened.diagnostic.diagnostic_code == "SB-STORAGE-DISK-OWNER-LOCK-HELD" ||
         opened.diagnostic.diagnostic_code == "SB-STORAGE-DISK-ROUTE-OWNER-LOCK-HELD" ||
         opened.diagnostic.diagnostic_code == "SB-STORAGE-DISK-DATA-OWNER-LOCK-HELD");
  }
  const char outcome = acquired ? 'W' : (refused ? 'L' : 'E');
  if (!WriteExact(result, &outcome, 1)) return false;
  ::close(result);
  if (acquired && !ReadExact(release, &token, 1)) return false;
  ::close(release);
  return acquired || refused;
}

bool RaceServerAndStorage(const char* self_path, bool alias_race = false) {
  const auto path = TempRoot() / (alias_race ? "alias-owner-race.sbdb" : "server-storage-race.sbdb");
  CreateRealDatabase(path);
  const auto alias = TempRoot() / "alias-owner-race.link";
  if (alias_race) std::filesystem::create_hard_link(path, alias);
  const auto before = ReadBytes(path);
  bool ok = true;
  for (unsigned round = 0; round < 8; ++round) {
    int ready[2], go[2], result[2], release[2];
    if (::pipe(ready) || ::pipe(go) || ::pipe(result) || ::pipe(release))
      return Expect(false, "owner race pipe setup failed");
    const auto ready_arg = std::to_string(ready[1]);
    const auto go_arg = std::to_string(go[0]);
    const auto result_arg = std::to_string(result[1]);
    const auto release_arg = std::to_string(release[0]);
    pid_t children[2]{};
    for (unsigned i = 0; i < 2; ++i) {
      children[i] = ::fork();
      if (children[i] < 0) {
        std::cerr << "owner race fork failed\n";
        std::exit(EXIT_FAILURE);
      }
      if (children[i] == 0) {
        ::close(ready[0]); ::close(go[1]); ::close(result[0]); ::close(release[1]);
        const auto& contender_path = alias_race && i == 1 ? alias : path;
        ::execl(self_path, self_path, "--race-owner", contender_path.c_str(),
                i == 0 ? "server" : "storage", ready_arg.c_str(), go_arg.c_str(),
                result_arg.c_str(), release_arg.c_str(), static_cast<char*>(nullptr));
        _exit(127);
      }
    }
    ::close(ready[1]); ::close(go[0]); ::close(result[1]); ::close(release[0]);
    char tokens[2]{};
    const bool prepared = ReadExact(ready[0], tokens, 2);
    ::close(ready[0]);
    const bool started = prepared && WriteExact(go[1], "GG", 2);
    ::close(go[1]);
    const bool reported = ReadExact(result[0], tokens, 2);
    ::close(result[0]);
    const unsigned winners = (tokens[0] == 'W') + (tokens[1] == 'W');
    const unsigned losers = (tokens[0] == 'L') + (tokens[1] == 'L');
    ok = Expect(started && reported && winners == 1 && losers == 1,
                "server/storage race did not admit exactly one retained owner") && ok;
    if (winners) ok = WriteExact(release[1], "XX", winners) && ok;
    ::close(release[1]);
    for (const auto child : children) {
      int status = 0;
      pid_t waited;
      do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
      ok = Expect(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                  "owner race child failed") && ok;
    }
    ok = Expect(ReadBytes(path) == before, "owner race changed database bytes") && ok;
  }
  return ok;
}

bool ChildStorageProbe(const std::filesystem::path& path,
                       bool read_only, bool expect_available) {
  disk::FileDevice device;
  const auto opened = device.Open(path.string(), read_only
      ? disk::FileOpenMode::open_existing_read_only
      : disk::FileOpenMode::open_existing);
  bool ok = Expect(opened.ok() == expect_available,
                   "separate-process storage availability mismatch");
  if (!opened.ok()) {
    ok = Expect(opened.diagnostic.diagnostic_code ==
                    "SB-STORAGE-DISK-OWNER-LOCK-HELD",
                "separate-process storage refusal diagnostic mismatch") && ok;
  } else {
    disk::SerializedDatabaseHeader header{};
    const auto read = device.ReadAt(0, header.data(), header.size());
    ok = Expect(read.ok() && disk::ParseDatabaseHeader(header).ok(),
                "separate-process real database header read failed") && ok;
    ok = Expect(device.Close().ok(), "probe close failed") && ok;
  }
  db::DatabaseOpenConfig config;
  config.path = path.string();
  config.read_only = read_only;
  config.suppress_background_agents = true;
  const auto lifecycle = db::OpenDatabaseFile(config);
  ok = Expect(lifecycle.ok() == expect_available,
              "separate-process database lifecycle availability mismatch") && ok;
  if (!lifecycle.ok()) {
    ok = Expect(lifecycle.diagnostic.diagnostic_code ==
                    "SB-STORAGE-DISK-OWNER-LOCK-HELD",
                "database lifecycle refusal diagnostic mismatch") && ok;
  }
  return ok;
}

bool RunProbe(const char* self_path, const std::filesystem::path& path,
              bool read_only, bool expect_available) {
  const pid_t child = ::fork();
  if (!Expect(child >= 0, "ownership probe fork failed")) return false;
  if (child == 0) {
    ::execl(self_path, self_path, "--probe", path.c_str(),
            read_only ? "read_only" : "mutable",
            expect_available ? "available" : "refused", static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  pid_t waited;
  do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  return Expect(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "separate-process ownership probe failed");
}

bool ReadOnlyProcessExclusionAndHandoff(const char* self_path) {
  const auto path = TempRoot() / "process-readonly.sbdb";
  CreateRealDatabase(path);
  const auto before = ReadBytes(path);
  bool ok = true;
  {
    disk::FileDevice owner;
    if (!Expect(owner.Open(path.string(),
                           disk::FileOpenMode::open_existing_read_only).ok(),
                "read-only owner acquisition failed")) return false;
    ok = RunProbe(self_path, path, true, false) && ok;
    ok = RunProbe(self_path, path, false, false) && ok;
    ok = Expect(ReadBytes(path) == before,
                "refused peer changed durable database bytes") && ok;
    ok = Expect(owner.Close().ok(), "read-only explicit close failed") && ok;
  }
  ok = RunProbe(self_path, path, true, true) && ok;
  {
    disk::FileDevice owner;
    if (!Expect(owner.Open(path.string(),
                           disk::FileOpenMode::open_existing_read_only).ok(),
                "read-only destructor owner acquisition failed")) return false;
    ok = RunProbe(self_path, path, true, false) && ok;
  }
  ok = RunProbe(self_path, path, true, true) && ok;
  return Expect(ReadBytes(path) == before,
                "read-only handoff changed durable database bytes") && ok;
}

bool StableLockInodeAfterRelease(const char* self_path, bool use_destructor) {
  const auto path = TempRoot() /
      (use_destructor ? "stable-destructor.sbdb" : "stable-close.sbdb");
  CreateRealDatabase(path);
  const auto before = ReadBytes(path);
  const auto lock_path = path.string() + ".sb.owner.lock";
  int contender = -1;
  struct stat held{};
  {
    disk::FileDevice owner;
    if (!Expect(owner.Open(path.string(), disk::FileOpenMode::open_existing).ok(),
                "stable-inode owner open failed")) return false;
    // A contender can already have this inode open before the owner releases.
    contender = ::open(lock_path.c_str(), O_RDWR | O_CLOEXEC);
    if (!Expect(contender >= 0, "open incumbent lock inode failed")) return false;
    if (::fstat(contender, &held) != 0) {
      ::close(contender);
      return Expect(false, "fstat incumbent lock failed");
    }
    if (!use_destructor && !owner.Close().ok()) {
      ::close(contender);
      return Expect(false, "stable-inode explicit close failed");
    }
  }
  struct stat named{};
  bool ok = Expect(::stat(lock_path.c_str(), &named) == 0 &&
                       named.st_dev == held.st_dev && named.st_ino == held.st_ino,
                   "owner release removed or replaced the lock inode");
  if (::flock(contender, LOCK_EX | LOCK_NB) != 0) {
    ::close(contender);
    return Expect(false, "contender could not acquire released inode");
  }
  ok = RunProbe(self_path, path, true, false) && ok;
  ok = RunProbe(self_path, path, false, false) && ok;
  ::flock(contender, LOCK_UN);
  ::close(contender);
  ok = RunProbe(self_path, path, true, true) && ok;
  return Expect(ReadBytes(path) == before,
                "lock handoff changed database bytes") && ok;
}

bool OwnerCrashReleasesOwnership(const char* self_path, bool server_owner) {
  const auto path = TempRoot() /
      (server_owner ? "server-owner-crash.sbdb" : "readonly-crash.sbdb");
  CreateRealDatabase(path);
  const auto before = ReadBytes(path);
  int ready[2];
  if (!Expect(::pipe(ready) == 0, "crash holder pipe failed")) return false;
  const std::string ready_fd = std::to_string(ready[1]);
  const pid_t child = ::fork();
  if (child < 0) {
    ::close(ready[0]);
    ::close(ready[1]);
    return Expect(false, "crash holder fork failed");
  }
  if (child == 0) {
    ::close(ready[0]);
    ::execl(self_path, self_path, server_owner ? "--hold-server" : "--hold-readonly", path.c_str(),
            ready_fd.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  ::close(ready[1]);
  char token = 0;
  ssize_t got;
  do { got = ::read(ready[0], &token, 1); } while (got < 0 && errno == EINTR);
  ::close(ready[0]);
  bool ok = Expect(got == 1 && token == 'R', "crash holder did not acquire ownership");
  if (ok) ok = server_owner ? RunServerProbe(self_path, path, false)
                           : RunProbe(self_path, path, true, false);
  // Target only the child created above; no destructors run in this process.
  if (::kill(child, SIGKILL) != 0) ok = Expect(false, "crash injection failed");
  int status = 0;
  pid_t waited;
  do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  ok = Expect(waited == child && WIFSIGNALED(status) &&
                  WTERMSIG(status) == SIGKILL, "crash holder was not reaped") && ok;
  ok = RunProbe(self_path, path, true, true) && ok;
  return Expect(ReadBytes(path) == before,
                "read-only crash changed database bytes") && ok;
}

bool ChildStorageRouteConflict(const std::filesystem::path& path) {
  disk::FileDevice device;
  const auto opened = device.Open(path.string(), disk::FileOpenMode::open_existing);
  return Expect(!opened.ok(),
                "external storage opener should fail while server route owner is held") &&
         Expect(opened.diagnostic.diagnostic_code ==
                    "SB-STORAGE-DISK-ROUTE-OWNER-LOCK-HELD",
                "external storage opener used wrong route-conflict diagnostic");
}

bool ChildAliasProbe(const std::filesystem::path& path, const std::string& mode,
                     bool available) {
  if (mode == "server") return ChildServerProbe(path, available);
  const auto open_mode = mode == "read_only" ? disk::FileOpenMode::open_existing_read_only
      : mode == "truncate" ? disk::FileOpenMode::create_or_truncate
                           : disk::FileOpenMode::open_existing;
  disk::FileDevice device;
  const auto opened = device.Open(path.string(), open_mode);
  bool ok = Expect(opened.ok() == available, "alias storage availability mismatch");
  if (!opened.ok()) {
    const auto& code = opened.diagnostic.diagnostic_code;
    ok = Expect(code == "SB-STORAGE-DISK-OWNER-LOCK-HELD" ||
                    code == "SB-STORAGE-DISK-ROUTE-OWNER-LOCK-HELD" ||
                    code == "SB-STORAGE-DISK-DATA-OWNER-LOCK-HELD",
                "alias refusal was not an ownership conflict") && ok;
  } else {
    if (available && mode != "truncate") {
      disk::SerializedDatabaseHeader header{};
      ok = Expect(device.ReadAt(0, header.data(), header.size()).ok() &&
                      disk::ParseDatabaseHeader(header).ok(),
                  "alias handoff did not read a real database") && ok;
    }
    ok = Expect(device.Close().ok(), "alias probe close failed") && ok;
  }
  return ok;
}

bool RunAliasProbe(const char* self, const std::filesystem::path& path,
                   const std::string& mode, bool available) {
  const auto child = ::fork();
  if (child < 0) return false;
  if (child == 0) {
    ::execl(self, self, "--alias-probe", path.c_str(), mode.c_str(),
            available ? "available" : "refused", static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  pid_t waited;
  do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  return Expect(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "alias process probe failed");
}

bool AliasExclusionAndTruncation(const char* self) {
  bool ok = true;
  for (bool server_owner : {false, true}) {
    for (bool symlink : {false, true}) {
      for (const std::string mode : {"read_only", "mutable", "truncate", "server"}) {
        const auto stem = std::string("alias-") + (server_owner ? "server-" : "storage-") +
                          (symlink ? "symlink-" : "hardlink-") + mode;
        const auto path = TempRoot() / (stem + ".sbdb");
        const auto alias = TempRoot() / (stem + ".alias");
        CreateRealDatabase(path);
        if (symlink) std::filesystem::create_symlink(path.filename(), alias);
        else std::filesystem::create_hard_link(path, alias);
        const auto before = ReadBytes(path);
        disk::FileDevice storage;
        server::DatabaseOwnershipResult route;
        if (server_owner) {
          route = server::AcquireDatabaseOwnership(OwnershipRequest(path, "server"));
          if (!Expect(route.acquired, "alias fixture server acquisition failed")) return false;
        } else if (!Expect(storage.Open(path.string(),
                                         disk::FileOpenMode::open_existing_read_only).ok(),
                           "alias fixture storage acquisition failed")) return false;
        ok = RunAliasProbe(self, alias, mode, false) && ok;
        const bool unchanged = ReadBytes(path) == before;
        ok = Expect(unchanged, "refused alias probe truncated or changed database bytes") && ok;
        if (server_owner) route.lock->release();
        else ok = Expect(storage.Close().ok(), "alias fixture owner close failed") && ok;
        // Do not confuse a baseline truncation failure with a handoff fixture bug.
        if (unchanged) ok = RunAliasProbe(self, alias, "read_only", true) && ok;
      }
    }
  }
  return ok;
}

bool InternalAliasRetainsOwner(const char* self, unsigned kind) {
  const auto path = TempRoot() / ("internal-alias-" + std::to_string(kind) + ".sbdb");
  auto alias = TempRoot() / ("internal-alias-" + std::to_string(kind) + ".alias");
  CreateRealDatabase(path);
  if (kind == 0) std::filesystem::create_hard_link(path, alias);
  else if (kind == 1) std::filesystem::create_symlink(path.filename(), alias);
  else {
    const auto directory = TempRoot() / "relative-alias";
    std::filesystem::create_directory(directory);
    alias = directory / ".." / path.filename();
  }
  const auto before = ReadBytes(path);
  auto owner = server::AcquireDatabaseOwnership(OwnershipRequest(path, "server"));
  if (!Expect(owner.acquired, "internal alias owner failed")) return false;
  disk::FileDevice reader;
  if (!Expect(reader.Open(alias.string(), disk::FileOpenMode::open_existing_read_only).ok(),
              "genuine internal alias reader was rejected")) return false;
  owner.lock->release();
  bool ok = RunServerProbe(self, path, false);
  ok = RunAliasProbe(self, alias, "server", false) && ok;
  disk::SerializedDatabaseHeader header{};
  ok = Expect(reader.ReadAt(0, header.data(), header.size()).ok() &&
                  disk::ParseDatabaseHeader(header).ok(),
              "internal alias reader lost its actual database") && ok;
  ok = Expect(reader.Close().ok(), "internal alias close failed") && ok;
  ok = RunServerProbe(self, path, true) && ok;
  return Expect(ReadBytes(path) == before, "internal alias handoff changed bytes") && ok;
}

bool CreatedPrimaryPinSurvivesCreator(const char* self) {
  const auto path = TempRoot() / "created-primary-pin.sbdb";
  const auto alias = TempRoot() / "created-primary-pin.link";
  auto owner = server::AcquireDatabaseOwnership(OwnershipRequest(path, "embedded"));
  if (!Expect(owner.acquired, "new primary pathname reservation failed")) return false;
  CreateRealDatabase(path);
  const auto before = ReadBytes(path);
  std::filesystem::create_hard_link(path, alias);
  bool ok = RunAliasProbe(self, alias, "server", false);
  disk::FileDevice reader;
  if (!Expect(reader.Open(alias.string(), disk::FileOpenMode::open_existing_read_only).ok(),
              "new primary did not bind its actual inode to the owner")) return false;
  owner.lock->release();
  ok = RunServerProbe(self, path, false) && ok;
  ok = Expect(reader.Close().ok(), "created primary reader close failed") && ok;
  ok = RunAliasProbe(self, alias, "server", true) && ok;
  return Expect(ReadBytes(path) == before, "created primary handoff changed bytes") && ok;
}

bool PathReplacementCannotRetargetOwner(const char* self) {
  const auto path = TempRoot() / "replacement-owner.sbdb";
  const auto replacement = TempRoot() / "replacement-other.sbdb";
  const auto saved = TempRoot() / "replacement-original.saved";
  CreateRealDatabase(path);
  CreateRealDatabase(replacement);
  const auto before_original = ReadBytes(path);
  const auto before_replacement = ReadBytes(replacement);
  auto owner = server::AcquireDatabaseOwnership(OwnershipRequest(path, "server"));
  if (!Expect(owner.acquired, "replacement fixture owner failed")) return false;
  // Rename only this private fixture's two real databases. The original inode
  // must stay owned, and its pathname must not authorize a different database.
  std::filesystem::rename(path, saved);
  std::filesystem::rename(replacement, path);
  bool ok = true;
  for (const auto mode : {disk::FileOpenMode::open_existing_read_only,
                          disk::FileOpenMode::create_or_truncate}) {
    disk::FileDevice device;
    const auto opened = device.Open(path.string(), mode);
    ok = Expect(!opened.ok() && !device.is_open() &&
                    opened.diagnostic.diagnostic_code == "SB-STORAGE-DISK-DATA-OWNER-LOCK-FAILED",
                "pathname replacement retargeted the native owner") && ok;
  }
  ok = RunAliasProbe(self, saved, "server", false) && ok;
  ok = Expect(ReadBytes(path) == before_replacement && ReadBytes(saved) == before_original,
              "replacement refusal changed a database") && ok;
  owner.lock->release();
  std::filesystem::rename(path, replacement);
  std::filesystem::rename(saved, path);
  return RunServerProbe(self, path, true) && ok;
}

bool AuthorizedTruncationHasRealEffect(const char* self, bool route_owned) {
  const auto path = TempRoot() / (route_owned ? "owned-truncation.sbdb" : "direct-truncation.sbdb");
  CreateRealDatabase(path);
  const auto before = ReadBytes(path);
  server::DatabaseOwnershipResult owner;
  if (route_owned) {
    owner = server::AcquireDatabaseOwnership(OwnershipRequest(path, "maintenance"));
    if (!Expect(owner.acquired, "authorized truncation owner failed")) return false;
  }
  disk::FileDevice device;
  if (!Expect(device.Open(path.string(), disk::FileOpenMode::create_or_truncate).ok(),
              "authorized truncation refused")) return false;
  const auto size = device.Size();
  bool ok = Expect(size.ok() && size.size_bytes == 0 && ReadBytes(path).empty(),
                   "authorized truncation reported success without truncating");
  // Restore this fixture's real database bytes, then verify independent handoff.
  ok = Expect(device.WriteAt(0, before.data(), before.size()).ok() && device.Sync().ok(),
              "truncation fixture restoration failed") && ok;
  ok = Expect(device.Close().ok(), "authorized truncation close failed") && ok;
  if (route_owned) owner.lock->release();
  ok = RunAliasProbe(self, path, "read_only", true) && ok;
  return Expect(ReadBytes(path) == before, "truncation fixture bytes were not restored") && ok;
}

bool ChildForgedPidIsRefused(const std::filesystem::path& path) {
  // Discovery text is attacker-controlled fixture input, never lock authority.
  {
    std::ofstream descriptor(server::DatabaseOwnershipLockPath(path),
                             std::ios::trunc);
    descriptor << "pid=" << static_cast<unsigned long long>(::getpid()) << "\n";
    if (!descriptor) return false;
  }
  disk::FileDevice reader;
  const auto opened = reader.Open(path.string(),
                                  disk::FileOpenMode::open_existing_read_only);
  return Expect(!opened.ok() && opened.diagnostic.diagnostic_code ==
                    "SB-STORAGE-DISK-ROUTE-OWNER-LOCK-HELD",
                "forged discovery PID admitted a foreign storage reader");
}

bool ForgedPidCannotBorrowOwner(const char* self_path) {
  const auto path = TempRoot() / "forged-pid.sbdb";
  CreateRealDatabase(path);
  const auto before = ReadBytes(path);
  auto owner = server::AcquireDatabaseOwnership(OwnershipRequest(path, "server"));
  if (!Expect(owner.acquired, "forged PID fixture owner failed")) return false;
  const pid_t child = ::fork();
  if (child < 0) return false;
  if (child == 0) {
    ::execl(self_path, self_path, "--forged-pid", path.string().c_str(),
            static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  const bool waited = ::waitpid(child, &status, 0) == child;
  bool ok = Expect(waited && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                   "foreign forged PID probe failed");
  // Genuine ownership must also work when the discovery PID is wrong.
  disk::FileDevice reader;
  ok = Expect(reader.Open(path.string(),
                           disk::FileOpenMode::open_existing_read_only).ok(),
              "genuine lease relied on discovery PID") && ok;
  if (reader.is_open()) ok = Expect(reader.Close().ok(), "reader close failed") && ok;
  ok = Expect(ReadBytes(path) == before, "PID probe changed database bytes") && ok;
  return ok;
}

bool InternalReadersRetainOwnership(const char* self_path, bool destructor_close) {
  const auto path = TempRoot() / (destructor_close
      ? "retained-internal-destructors.sbdb" : "retained-internal-close.sbdb");
  CreateRealDatabase(path);
  const auto before = ReadBytes(path);
  auto owner = server::AcquireDatabaseOwnership(OwnershipRequest(path, "server"));
  if (!Expect(owner.acquired, "retained reader owner failed")) return false;
  auto first = std::make_unique<disk::FileDevice>();
  auto last = std::make_unique<disk::FileDevice>();
  if (!Expect(first->Open(path.string(),
                            disk::FileOpenMode::open_existing_read_only).ok() &&
                  last->Open(path.string(),
                             disk::FileOpenMode::open_existing_read_only).ok(),
              "retained readers could not open")) return false;
  owner.lock->release();
  bool ok = Expect(!owner.lock->valid(), "released wrapper still valid");
  ok = RunServerProbe(self_path, path, false) && ok;
  disk::FileDevice late;
  const auto late_open = late.Open(path.string(),
                                   disk::FileOpenMode::open_existing_read_only);
  ok = Expect(!late_open.ok() && late_open.diagnostic.diagnostic_code ==
                  "SB-STORAGE-DISK-ROUTE-OWNER-LOCK-HELD",
              "withdrawn owner admitted a new internal reader") && ok;
  if (late.is_open()) (void)late.Close();
  if (destructor_close) first.reset();
  else ok = Expect(first->Close().ok(), "first reader close failed") && ok;
  ok = RunServerProbe(self_path, path, false) && ok;
  disk::SerializedDatabaseHeader header{};
  ok = Expect(last->ReadAt(0, header.data(), header.size()).ok() &&
                  disk::ParseDatabaseHeader(header).ok(),
              "retained last reader lost access to real data") && ok;
  if (destructor_close) last.reset();
  else ok = Expect(last->Close().ok(), "last reader close failed") && ok;
  ok = RunServerProbe(self_path, path, true) && ok;
  ok = Expect(ReadBytes(path) == before, "reader handoff changed database bytes") && ok;
  return ok;
}

bool FailedInternalOpenDoesNotRetainLease(const char* self_path) {
  const auto path = TempRoot() / "failed-internal-open.sbdb";
  CreateRealDatabase(path);
  auto owner = server::AcquireDatabaseOwnership(OwnershipRequest(path, "server"));
  if (!Expect(owner.acquired, "failed-open owner failed")) return false;
  disk::FileDevice failed;
  const auto opened = failed.Open(path.string(), disk::FileOpenMode::create_new);
  bool ok = Expect(!opened.ok() && !failed.is_open() &&
                       opened.diagnostic.diagnostic_code == "SB-STORAGE-DISK-CREATE-EXISTS",
                   "existing-file create did not refuse");
  owner.lock->release();
  return RunServerProbe(self_path, path, true) && ok;
}

bool ForkCannotBorrowParentLease(const char* self_path) {
  const auto path = TempRoot() / "fork-internal-lease.sbdb";
  CreateRealDatabase(path);
  auto owner = server::AcquireDatabaseOwnership(OwnershipRequest(path, "server"));
  if (!Expect(owner.acquired, "fork lease owner failed")) return false;
  disk::FileDevice reader;
  if (!Expect(reader.Open(path.string(),
                            disk::FileOpenMode::open_existing_read_only).ok(),
              "fork fixture reader failed")) return false;
  const pid_t child = ::fork();
  if (child < 0) return false;
  if (child == 0) {
    disk::FileDevice foreign;
    const auto opened = foreign.Open(path.string(),
                                     disk::FileOpenMode::open_existing_read_only);
    const bool refused = !opened.ok() && opened.diagnostic.diagnostic_code ==
        "SB-STORAGE-DISK-ROUTE-OWNER-LOCK-HELD";
    owner.lock->release();
    // Do not run inherited reader/mutex destructors; exec/_exit is the fork
    // boundary. This tests admission and wrapper cleanup, not fork-safe I/O.
    _exit(refused ? 0 : 1);
  }
  int status = 0;
  bool ok = Expect(::waitpid(child, &status, 0) == child &&
                       WIFEXITED(status) && WEXITSTATUS(status) == 0,
                   "forked process borrowed its parent's live lease");
  ok = RunServerProbe(self_path, path, false) && ok;
  ok = Expect(reader.Close().ok(), "fork fixture reader close failed") && ok;
  owner.lock->release();
  return RunServerProbe(self_path, path, true) && ok;
}

bool ServerRouteBlocksExternalStorageOpen(const char* self_path) {
  const auto path = TempRoot() / "server-blocks-storage.sbdb";
  CreateRealDatabase(path);

  auto owner = server::AcquireDatabaseOwnership(OwnershipRequest(path, "server"));
  if (!Expect(owner.acquired, "server route owner acquisition should succeed")) {
    return false;
  }

  // Multiple internal readers remain inside the same process route owner.
  disk::FileDevice internal_a;
  disk::FileDevice internal_b;
  if (!Expect(internal_a.Open(path.string(),
                              disk::FileOpenMode::open_existing_read_only).ok() &&
                  internal_b.Open(path.string(),
                              disk::FileOpenMode::open_existing_read_only).ok(),
              "same-route-owner internal readers were rejected")) return false;
  db::DatabaseOpenConfig read_only;
  read_only.path = path.string();
  read_only.read_only = true;
  if (!Expect(db::OpenDatabaseFile(read_only).ok(),
              "same-route-owner database inspection was rejected")) return false;

  const pid_t child = ::fork();
  if (child < 0) {
    std::cerr << "fork failed\n";
    return false;
  }
  if (child == 0) {
    ::execl(self_path,
            self_path,
            "--expect-storage-route-conflict",
            path.string().c_str(),
            static_cast<char*>(nullptr));
    _exit(127);
  }

  int status = 0;
  if (::waitpid(child, &status, 0) < 0) {
    std::cerr << "waitpid failed\n";
    return false;
  }
  return Expect(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
                "external storage route-conflict child failed");
}
#endif

}  // namespace

int main(int argc, char** argv) {
  const auto memory = scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
      scratchbird::core::memory::DefaultLocalEngineMemoryPolicy(),
      "public_database_ownership_real_storage_fixture");
  if (!Expect(memory.ok() && memory.fixture_mode,
              "ownership fixture memory setup failed")) return EXIT_FAILURE;
#ifndef _WIN32
  ::signal(SIGPIPE, SIG_IGN);
  if (argc == 5 && std::string(argv[1]) == "--alias-probe") {
    return ChildAliasProbe(argv[2], argv[3], std::string(argv[4]) == "available")
        ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (argc == 3 && std::string(argv[1]) == "--forged-pid") {
    return ChildForgedPidIsRefused(argv[2]) ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (argc == 3 && std::string(argv[1]) == "--descriptor-write-failure") {
    return ChildDescriptorWriteFailure(argv[2]) ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (argc == 4 && std::string(argv[1]) == "--hold-server") {
    auto owner = server::AcquireDatabaseOwnership(ChildOwnershipRequest(argv[2]));
    if (!owner.acquired || !owner.lock || !owner.lock->valid()) return EXIT_FAILURE;
    const int ready = std::stoi(argv[3]);
    if (!WriteExact(ready, "R", 1)) return EXIT_FAILURE;
    ::close(ready);
    for (;;) ::pause();  // Parent kills only this fixture owner after readiness.
  }
  if (argc == 4 && std::string(argv[1]) == "--probe-server") {
    return ChildServerProbe(argv[2], std::string(argv[3]) == "available")
               ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (argc == 8 && std::string(argv[1]) == "--race-owner") {
    return ChildOwnerRace(argv[2], std::string(argv[3]) == "server",
                          std::stoi(argv[4]), std::stoi(argv[5]),
                          std::stoi(argv[6]), std::stoi(argv[7]))
               ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (argc == 5 && std::string(argv[1]) == "--probe") {
    return ChildStorageProbe(argv[2], std::string(argv[3]) == "read_only",
                             std::string(argv[4]) == "available")
               ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (argc == 4 && std::string(argv[1]) == "--hold-readonly") {
    disk::FileDevice owner;
    if (!owner.Open(argv[2], disk::FileOpenMode::open_existing_read_only).ok())
      return EXIT_FAILURE;
    const int ready = std::stoi(argv[3]);
    if (::write(ready, "R", 1) != 1) return EXIT_FAILURE;
    ::close(ready);
    for (;;) ::pause();  // Parent injects SIGKILL after the lock is held.
  }
  if (argc == 3 && std::string(argv[1]) == "--expect-storage-route-conflict") {
    return ChildStorageRouteConflict(argv[2]) ? EXIT_SUCCESS : EXIT_FAILURE;
  }
#endif

  bool ok = true;
  ok = StorageMutableConflictsFailClosed() && ok;
  ok = StorageReadOnlyOwnershipIsExclusive() && ok;
  ok = ServerOwnershipConflictsFailClosed() && ok;
#ifndef _WIN32
  ok = AuthorizedTruncationHasRealEffect(argv[0], false) && ok;
  ok = AuthorizedTruncationHasRealEffect(argv[0], true) && ok;
  ok = CreatedPrimaryPinSurvivesCreator(argv[0]) && ok;
  ok = PathReplacementCannotRetargetOwner(argv[0]) && ok;
  ok = AliasExclusionAndTruncation(argv[0]) && ok;
  for (unsigned kind = 0; kind < 3; ++kind)
    ok = InternalAliasRetainsOwner(argv[0], kind) && ok;
  ok = RaceServerAndStorage(argv[0], true) && ok;
  ok = FailedInternalOpenDoesNotRetainLease(argv[0]) && ok;
  ok = ForkCannotBorrowParentLease(argv[0]) && ok;
  ok = ForgedPidCannotBorrowOwner(argv[0]) && ok;
  ok = InternalReadersRetainOwnership(argv[0], false) && ok;
  ok = InternalReadersRetainOwnership(argv[0], true) && ok;
  ok = ServerRouteBlocksExternalStorageOpen(argv[0]) && ok;
  ok = StorageOwnerBlocksServer(argv[0], true) && ok;
  ok = StorageOwnerBlocksServer(argv[0], false) && ok;
  ok = ServerStorageLockMoveAndRelease() && ok;
  ok = ForkedReleasePreservesParentOwner(argv[0]) && ok;
  ok = StorageOpenFailureDoesNotPublish() && ok;
  ok = DescriptorWriteFailureReleasesLocks(argv[0]) && ok;
  ok = RaceServerAndStorage(argv[0]) && ok;
  ok = ReadOnlyProcessExclusionAndHandoff(argv[0]) && ok;
  ok = StableLockInodeAfterRelease(argv[0], false) && ok;
  ok = StableLockInodeAfterRelease(argv[0], true) && ok;
  ok = OwnerCrashReleasesOwnership(argv[0], false) && ok;
  ok = OwnerCrashReleasesOwnership(argv[0], true) && ok;
#else
  std::cerr << "POSIX exec, inode and crash cases are not run on Windows\n";
#endif
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
