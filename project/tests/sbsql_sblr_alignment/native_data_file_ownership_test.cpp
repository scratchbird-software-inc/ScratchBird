// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_NATIVE_DATA_FILE_OWNERSHIP (slice step S1, owner 2026-11-13)
// Native ownership without lock files, on real native databases written by the
// production create (CreateNativeDatabaseOnOpenDevice), every page profile.
//
// Checked, per profile:
//  1. native create (FileDevice::Open(create_new, native_data_file)) leaves
//     exactly one directory entry, the database file, during and after create;
//     while the creating device is open no other owner is admitted;
//  2. native AcquireDatabaseOwnership creates no .sb.route.owner.lock,
//     .sb.owner.lock or descriptor file, reports no lock path, and refuses with
//     ARCH.DATABASE_MULTI_OWNER and no incumbent pid/endpoint (Q94):
//       - a second native owner in this process (fresh open file description),
//       - a legacy owner in this process (and it leaves no lock file behind),
//       - a native-ownership FileDevice path-open in this process,
//       - a native and a legacy owner in another process (fork + exec);
//  3. served devices (FileDevice::OpenServed) read and write under the lock;
//  4. releasing the owner (served devices closed) admits a new owner, in this
//     process and in another;
//  5. a legacy owner still creates its two lock files (legacy unchanged, Q93),
//     excludes a native owner in this and another process, and removes them on
//     release.
//  6. (once) the create-then-lock window: if another opener flocks the new
//     file between OpenNewNativeDatabaseFile's O_CREAT|O_EXCL and its flock,
//     the create refuses (SB-STORAGE-DISK-DATA-OWNER-LOCK-HELD), makes no lock
//     file, unlinks nothing (the empty file stays with its holder) and the
//     holder keeps sole ownership. The rival is injected deterministically by
//     wrapping flock (-Wl,--wrap=flock): the armed wrapper reopens the same
//     inode through /proc/self/fd (a separate open file description, exactly
//     like another opener) and flocks it first;
//  7. (once) by construction: CreateNativeDatabaseOnOpenDevice refuses a
//     device not opened with native ownership (invalid_device), writing
//     nothing.
// The contender is this executable re-executed: argv "contend <path>
// native|legacy". Exit 0 = owned, 2 = ARCH.DATABASE_MULTI_OWNER, 3 = other,
// 4 = a native refusal named an incumbent.
#include "../../src/server/database_ownership.hpp"
#include "native_database_create.hpp"
#include "native_catalog_relation_lease_reader.hpp"
#include "admitted_datatype_cohort.hpp"
#include "datatype_catalog_manifest.hpp"
#include "storage/disk/filespace_bootstrap.hpp"
#include "storage/disk/disk_device.hpp"
#include "uuid.hpp"
#include "../support/owned_temp_directory.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>

extern "C" int __real_flock(int fd, int operation);
namespace {
std::atomic<bool> rival_flocks_first{false};
int rival_fd = -1;
}  // namespace
// Test-only: when armed, the next exclusive flock first lets a rival open file
// description of the same inode take the lock.
extern "C" int __wrap_flock(int fd, int operation) {
  if ((operation & LOCK_EX) && rival_flocks_first.exchange(false)) {
    char link[64];
    std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    rival_fd = ::open(link, O_RDONLY | O_CLOEXEC);
    if (rival_fd >= 0 && __real_flock(rival_fd, LOCK_EX | LOCK_NB) != 0) {
      ::close(rival_fd);
      rival_fd = -1;
    }
  }
  return __real_flock(fd, operation);
}

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace server = scratchbird::server;
namespace db = scratchbird::storage::database;
namespace d = scratchbird::storage::disk;
namespace u = scratchbird::core::uuid;
using namespace scratchbird::core::platform;
using Ownership = d::DatabaseFileOwnership;

unsigned checks = 0;
void Check(bool ok, std::string_view why, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) throw std::runtime_error(std::string(why) + " (line " + std::to_string(at.line()) + ")");
}
Uuid Fixed(unsigned n) {
  Uuid id;
  id.bytes[0] = 1;
  id.bytes[6] = 0x70;
  id.bytes[8] = 0x80;
  id.bytes[14] = static_cast<byte>(n >> 8);
  id.bytes[15] = static_cast<byte>(n);
  return id;
}
constexpr const char* kFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";

std::string self_path;

std::set<std::string> Entries(const std::filesystem::path& dir) {
  std::set<std::string> out;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) out.insert(entry.path().filename().string());
  return out;
}
bool OnlyDatabase(const std::filesystem::path& dir) {
  const auto entries = Entries(dir);
  return entries.size() == 1 && *entries.begin() == "native";
}
bool HasLegacyLockFiles(const std::filesystem::path& dir) {
  const auto entries = Entries(dir);
  return entries.count("native.sb.route.owner.lock") == 1 && entries.count("native.sb.owner.lock") == 1;
}

server::DatabaseOwnershipRequest Request(const std::string& path, Ownership ownership,
                                         d::RouteStorageAccess access = d::RouteStorageAccess::read_write) {
  server::DatabaseOwnershipRequest r;
  r.database_path = path;
  r.storage_access = access;
  r.ownership = ownership;
  return r;
}

bool EmptyIncumbent(const server::DatabaseOwnershipResult& r) {
  return r.incumbent.pid.empty() && r.incumbent.sbps_endpoint.empty() && r.incumbent.format.empty() &&
         r.lock_path.empty() && r.diagnostic_detail.find("pid") == std::string::npos;
}

// Run the contender in a separate process image (exec, not a bare fork: a fork
// shares this process's open file descriptions, so its locks, and inherits the
// process-local registry).
int Contend(const std::string& path, std::string_view mode) {
  const pid_t child = ::fork();
  Check(child >= 0, "fork the contender");
  if (child == 0) {
    const std::string m(mode);
    ::execl(self_path.c_str(), self_path.c_str(), "contend", path.c_str(), m.c_str(), nullptr);
    ::_exit(70);
  }
  int status = 0;
  Check(::waitpid(child, &status, 0) == child && WIFEXITED(status), "the contender exits");
  return WEXITSTATUS(status);
}

int ContenderMain(const std::string& path, std::string_view mode) {
  const bool native = mode == "native";
  if (!native && mode != "legacy") return 64;
  auto owned = server::AcquireDatabaseOwnership(
      Request(path, native ? Ownership::native_data_file : Ownership::legacy_lock_files));
  if (owned.acquired) return 0;
  std::cerr << "contender " << mode << ": " << owned.diagnostic_code << ':' << owned.diagnostic_detail << '\n';
  if (owned.diagnostic_code != "ARCH.DATABASE_MULTI_OWNER") return 3;
  if (native && !EmptyIncumbent(owned)) return 4;
  return 2;
}

db::NativeDatabaseCreateRequest CreateRequest(const d::CanonicalFilespacePageProfile& p) {
  db::NativeDatabaseCreateRequest r;
  r.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  r.database_name = "orders";
  r.database_default_language = "en";
  r.page_size_profile_uuid = p.uuid;
  r.page_size_bytes = p.page_size_bytes;
  r.total_pages = p.page_size_bytes == 8192 ? 1024 : 512;
  r.creation_utc_millis = 1789357072000ULL;
  r.engine_uuid = Fixed(4);
  r.datatype_cohort = {{u::UuidKind::object, scratchbird::core::datatypes::kDatatypeCohortV11}, 11, 11};
  r.bootstrap_principal_name = "BootstrapAdmin";
  r.bootstrap_credential_fingerprint = kFingerprint;
  r.service_principal_name = "NodeService";
  r.service_credential_fingerprint = kFingerprint;
  r.maximum_retained_image_bytes = db::NativeHostedSourceBackingBytes(p.page_size_bytes);
  return r;
}

// 6 and 7 (see the header).
void CreateWindowAndConstruction(bool native_only) {
  scratchbird::tests::OwnedTempDirectory dir;
  const std::string path = (dir.path() / "native").string();
  {
    d::FileDevice device;
    rival_flocks_first = true;
    const auto opened = db::OpenNewNativeDatabaseFile(device, path);
    Check(!rival_flocks_first && rival_fd >= 0, "the rival took the new file's lock first");
    Check(!opened.ok() && opened.diagnostic.diagnostic_code == "SB-STORAGE-DISK-DATA-OWNER-LOCK-HELD" &&
              !device.is_open(),
          "a create that loses the lock to a rival refuses cleanly");
    Check(OnlyDatabase(dir.path()) && std::filesystem::file_size(path) == 0,
          "the refused create made no lock file and unlinked nothing: the empty file stays with its holder");
    const auto contender = server::AcquireDatabaseOwnership(Request(path, Ownership::native_data_file));
    Check(!contender.acquired && contender.diagnostic_code == "ARCH.DATABASE_MULTI_OWNER",
          "the rival keeps sole ownership");
    Check(Contend(path, "native") == 2, "another process is refused while the rival holds the file");
    ::close(rival_fd);
    rival_fd = -1;
    const auto retry = db::OpenNewNativeDatabaseFile(device, path);
    Check(!retry.ok() && retry.diagnostic.diagnostic_code == "SB-STORAGE-DISK-CREATE-EXISTS",
          "a retried create never adopts the existing file");
    Check(OnlyDatabase(dir.path()), "still no lock file");
    std::filesystem::remove(path);
  }
  if (!native_only) {
    // A legacy-mode device (the pre-S1 way to open a new file) is refused by
    // construction before anything is written.
    const auto& p = d::kCanonicalFilespacePageProfiles[0];
    d::FileDevice legacy;
    Check(legacy.Open(path, d::FileOpenMode::create_new).ok() && !legacy.native_data_file_owner(),
          "a legacy-mode device is not a native owner");
    u::StandaloneUuidV7Issuer issuer({Fixed(1), Fixed(10)}, {{}, 0, 1000});
    const auto made = db::CreateNativeDatabaseOnOpenDevice(legacy, CreateRequest(p), issuer);
    Check(!made.ok() && made.error == db::NativeDatabaseCreateError::workspace_failure &&
              made.workspace_error == db::NativeCreationWorkspaceError::invalid_device,
          "the native create refuses a device not opened with native ownership");
    const auto size = legacy.Size();
    Check(size.ok() && size.size_bytes == 0, "the refused native create wrote nothing");
    Check(legacy.Close().ok(), "close the legacy device");
    std::filesystem::remove(path);
    d::FileDevice native;
    Check(db::OpenNewNativeDatabaseFile(native, path).ok() && native.native_data_file_owner(),
          "OpenNewNativeDatabaseFile yields a native owner");
    Check(native.Close().ok() && !native.native_data_file_owner(), "closing ends native ownership");
  }
  dir.Cleanup();
}

std::vector<char> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

// native_only (argv "native-only"): no legacy owner or contender runs, so an
// strace of the whole process must show no .sb.* path at all.
void Run(unsigned index, bool native_only) {
  const auto& p = d::kCanonicalFilespacePageProfiles[index];
  scratchbird::tests::OwnedTempDirectory dir;
  const std::string path = (dir.path() / "native").string();

  // 1. Native create: one file, no transient lock file.
  {
    const auto r = CreateRequest(p);
    d::FileDevice device;
    Check(db::OpenNewNativeDatabaseFile(device, path).ok() && device.native_data_file_owner(),
          "native create opens a new file under the data-file lock");
    Check(OnlyDatabase(dir.path()), "native create made no lock file");
    const auto busy = server::AcquireDatabaseOwnership(Request(path, Ownership::native_data_file));
    Check(!busy.acquired && busy.diagnostic_code == "ARCH.DATABASE_MULTI_OWNER" && EmptyIncumbent(busy),
          "while create holds the file, a native owner in this process is refused without an incumbent");
    Check(Contend(path, "native") == 2, "while create holds the file, a native owner in another process is refused");
    if (!native_only)
      Check(Contend(path, "legacy") == 2, "while create holds the file, a legacy owner in another process is refused");
    Check(OnlyDatabase(dir.path()), "refused contenders leave no lock file");
    u::StandaloneUuidV7Issuer issuer({Fixed(1), Fixed(10)}, {{}, 0, 1000});
    const auto made = db::CreateNativeDatabaseOnOpenDevice(device, r, issuer);
    Check(made.ok(), "the production create writes a complete native database");
    Check(OnlyDatabase(dir.path()), "the written database is the only file");
    Check(device.Close().ok(), "close the created file");
    Check(OnlyDatabase(dir.path()), "after create the database is the only file");
  }
  const auto created = ReadFile(path);
  Check(created.size() >= p.page_size_bytes, "the created file reads back");

  // 2. Native ownership.
  {
    auto owner = server::AcquireDatabaseOwnership(Request(path, Ownership::native_data_file));
    if (!owner.acquired) std::cerr << owner.diagnostic_code << ':' << owner.diagnostic_detail << '\n';
    Check(owner.acquired && owner.lock && owner.lock->valid(), "the native owner acquires the database");
    Check(owner.lock_path.empty() && owner.lock->lock_path().empty() && EmptyIncumbent(owner),
          "a native owner names no lock file and no descriptor");
    Check(OnlyDatabase(dir.path()), "native ownership made no lock file");
    Check(owner.lock->ObserveWriteCustody() == d::RouteWriteCustodyState::retained,
          "the native owner retains write custody");

    const auto second = server::AcquireDatabaseOwnership(Request(path, Ownership::native_data_file));
    Check(!second.acquired && second.diagnostic_code == "ARCH.DATABASE_MULTI_OWNER" &&
              second.diagnostic_detail == "database_data_file_owned" && EmptyIncumbent(second),
          "a second native owner in this process is refused, naming no incumbent");
    const auto read_only = server::AcquireDatabaseOwnership(
        Request(path, Ownership::native_data_file, d::RouteStorageAccess::read_only));
    Check(!read_only.acquired && read_only.diagnostic_code == "ARCH.DATABASE_MULTI_OWNER",
          "a read-only native owner in this process is refused too");
    if (!native_only) {
      const auto legacy = server::AcquireDatabaseOwnership(Request(path, Ownership::legacy_lock_files));
      Check(!legacy.acquired && legacy.diagnostic_code == "ARCH.DATABASE_MULTI_OWNER",
            "a legacy owner in this process is refused by the data-file lock");
    }
    Check(OnlyDatabase(dir.path()), "the refused legacy owner removed the lock files it made");
    {
      d::FileDevice stray;
      const auto opened = stray.Open(path, d::FileOpenMode::open_existing_read_only, Ownership::native_data_file);
      Check(!opened.ok() && opened.diagnostic.diagnostic_code == "SB-STORAGE-DISK-DATA-OWNER-LOCK-HELD",
            "a fresh native path-open in this process is refused, not borrowed");
    }
    Check(OnlyDatabase(dir.path()), "the refused path-open made no lock file");
    Check(Contend(path, "native") == 2, "a native owner in another process is refused, naming no incumbent");
    if (!native_only) Check(Contend(path, "legacy") == 2, "a legacy owner in another process is refused");
    Check(OnlyDatabase(dir.path()), "refused contenders in other processes leave no lock file");

    // 3. Served devices work under the native lock.
    const auto lease = owner.lock->ServedStorageLease();
    Check(static_cast<bool>(lease), "the native owner publishes its storage lease");
    {
      d::FileDevice reader, writer;
      Check(reader.OpenServed(lease, path, d::FileOpenMode::open_existing_read_only).ok(), "served reader");
      Check(writer.OpenServed(lease, path, d::FileOpenMode::open_existing).ok(), "served writer");
      std::vector<char> page(p.page_size_bytes);
      const auto read = reader.ReadAt(0, page.data(), page.size());
      Check(read.ok() && read.bytes_transferred == page.size() &&
                std::equal(page.begin(), page.end(), created.begin()),
            "the served reader reads page zero as written");
      // Write the last page's own bytes back: a real write that changes nothing.
      const u64 last = created.size() - p.page_size_bytes;
      const auto wrote = writer.WriteAt(last, created.data() + last, p.page_size_bytes);
      Check(wrote.ok() && wrote.bytes_transferred == p.page_size_bytes, "the served writer writes");
      Check(writer.Sync().ok(), "the served writer syncs");
      Check(OnlyDatabase(dir.path()), "served devices made no lock file");
      Check(Contend(path, "native") == 2, "served devices keep the file owned against another process");
      Check(reader.Close().ok() && writer.Close().ok(), "served devices close");
    }
    Check(ReadFile(path) == created, "the served write changed no byte");
    Check(OnlyDatabase(dir.path()), "after served devices close, still no lock file");
  }

  // 4. Release admits a new owner, here and in another process.
  Check(OnlyDatabase(dir.path()), "release leaves only the database");
  {
    auto again = server::AcquireDatabaseOwnership(Request(path, Ownership::native_data_file));
    Check(again.acquired, "after release a new native owner in this process is admitted");
  }
  Check(Contend(path, "native") == 0, "after release a native owner in another process is admitted");
  Check(OnlyDatabase(dir.path()), "the other process's native ownership made no lock file");

  // 5. Legacy ownership is unchanged and excludes native owners.
  if (!native_only) {
    auto legacy = server::AcquireDatabaseOwnership(Request(path, Ownership::legacy_lock_files));
    Check(legacy.acquired && !legacy.lock_path.empty(), "a legacy owner acquires");
    Check(HasLegacyLockFiles(dir.path()), "the legacy owner still creates both lock files");
    const auto native = server::AcquireDatabaseOwnership(Request(path, Ownership::native_data_file));
    Check(!native.acquired && native.diagnostic_code == "ARCH.DATABASE_MULTI_OWNER" && EmptyIncumbent(native),
          "a native owner in this process is refused by the legacy owner's data-file lock");
    Check(Contend(path, "native") == 2, "a native owner in another process is refused by the legacy owner");
    Check(HasLegacyLockFiles(dir.path()), "the legacy owner's lock files survive refused native contenders");
  }
  Check(OnlyDatabase(dir.path()), "the legacy owner removes its lock files on release");
  Check(ReadFile(path) == created, "no owner changed the database");
  dir.Cleanup();
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::string_view(argv[1]) == "contend") return ContenderMain(argv[2], argv[3]);
  const bool native_only = argc == 2 && std::string_view(argv[1]) == "native-only";
  if (argc != 1 && !native_only) return 64;
  self_path = std::filesystem::canonical("/proc/self/exe").string();
  try {
    for (unsigned profile = 0; profile < 5; ++profile) Run(profile, native_only);
    CreateWindowAndConstruction(native_only);
  } catch (const std::exception& e) {
    std::cerr << "native data file ownership FAIL " << e.what() << '\n';
    return 1;
  }
  std::cout << "native data file ownership PASS checks=" << checks << '\n';
  return 0;
}
