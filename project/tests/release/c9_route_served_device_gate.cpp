// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_ROUTE_OWNER_SERVED_DEVICE (register C9)
// The route owner's own threads get a device served from its open descriptor
// instead of re-opening the database path. Real database, real server-style
// ownership; no mocks.
#include "database_ownership.hpp"
#include "database_lifecycle.hpp"
#include "disk_device.hpp"
#include "memory.hpp"
#include "uuid.hpp"
#include "../support/owned_temp_directory.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <vector>

namespace {

namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace uuid = scratchbird::core::uuid;
namespace server = scratchbird::server;

bool Expect(bool condition, const char* message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

scratchbird::tests::OwnedTempDirectory& TempOwner() {
  static scratchbird::tests::OwnedTempDirectory owner;
  return owner;
}

std::filesystem::path CreateRealDatabase(const char* name) {
  const auto path = TempOwner().path() / name;
  db::DatabaseCreateConfig config;
  config.path = path.string();
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  config.database_uuid =
      uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::database, now).value;
  config.filespace_uuid =
      uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::filespace, now + 1).value;
  config.page_size = 16384;
  config.creation_unix_epoch_millis = now;
  // Storage regression only: not credentialed public CREATE DATABASE evidence.
  config.allow_minimal_resource_bootstrap = true;
  config.require_resource_seed_pack = false;
  const auto created = db::CreateDatabaseFile(config);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << '\n';
    std::exit(EXIT_FAILURE);
  }
  return path;
}

server::DatabaseOwnershipResult Own(const std::filesystem::path& path,
                                    disk::RouteStorageAccess access) {
  server::DatabaseOwnershipRequest request;
  request.database_path = path;
  request.owner_kind = "server";
  request.sbps_endpoint = TempOwner().path() / "sbps.sock";
  request.storage_access = access;
  return server::AcquireDatabaseOwnership(request);
}

std::vector<char> FileBytes(const std::filesystem::path& path, std::size_t count) {
  std::ifstream in(path, std::ios::binary);
  std::vector<char> bytes(count);
  in.read(bytes.data(), static_cast<std::streamsize>(count));
  return bytes;
}

bool ReadOnlyServedDevice() {
  const auto path = CreateRealDatabase("served-read.sbdb");
  auto owned = Own(path, disk::RouteStorageAccess::read_only);
  bool ok = Expect(owned.acquired && owned.lock, "read-only ownership not acquired");
  if (!ok) return false;
  const auto lease = owned.lock->ServedStorageLease();
  ok = Expect(lease != nullptr, "owner did not expose its storage lease") && ok;
  const auto borrowers_before = lease->ObserveLegacyBorrowers();
  const auto opens_before = disk::ObserveFileDevicePathOpens();

  disk::FileDevice device;
  ok = Expect(device.OpenServed(lease, path.string(), disk::FileOpenMode::open_existing_read_only).ok(),
              "served read-only device refused") && ok;
  std::vector<char> served(16384);
  ok = Expect(device.ReadAt(0, served.data(), served.size()).ok(), "served read failed") && ok;
  ok = Expect(served == FileBytes(path, served.size()), "served bytes differ from the file") && ok;
  const auto opens_during = disk::ObserveFileDevicePathOpens();
  ok = Expect(opens_during.opens_without_route_owner == opens_before.opens_without_route_owner &&
                  opens_during.opens_with_route_owner == opens_before.opens_with_route_owner &&
                  opens_during.served_opens == opens_before.served_opens + 1,
              "a served device must not count as a path open") && ok;
  ok = Expect(lease->ObserveLegacyBorrowers() == std::optional<std::uint64_t>(*borrowers_before + 1),
              "served device is not a counted borrower") && ok;
  disk::FileDevice writer;
  const auto refused_writer =
      writer.OpenServed(lease, path.string(), disk::FileOpenMode::open_existing);
  ok = Expect(!refused_writer.ok(), "read-only ownership served a writable device") && ok;
  ok = Expect(device.Close().ok(), "served device close failed") && ok;
  ok = Expect(lease->ObserveLegacyBorrowers() == borrowers_before,
              "closing the served device did not release its borrow") && ok;

  // Only the owner's own file, named exactly, is served.
  const auto other = CreateRealDatabase("served-other.sbdb");
  disk::FileDevice mismatch;
  ok = Expect(!mismatch.OpenServed(lease, other.string(),
                                   disk::FileOpenMode::open_existing_read_only).ok(),
              "a different file was served from the owner's descriptor") && ok;
  disk::FileDevice create;
  ok = Expect(!create.OpenServed(lease, path.string(), disk::FileOpenMode::create_new).ok(),
              "a create mode was served") && ok;

  owned.lock->release();
  disk::FileDevice after;
  ok = Expect(!after.OpenServed(lease, path.string(),
                                disk::FileOpenMode::open_existing_read_only).ok(),
              "a released owner still served a device") && ok;
  return ok;
}

bool WritableServedDeviceAndSerialization() {
  const auto path = CreateRealDatabase("served-write.sbdb");
  auto owned = Own(path, disk::RouteStorageAccess::read_write);
  bool ok = Expect(owned.acquired && owned.lock, "writable ownership not acquired");
  if (!ok) return false;
  const auto lease = owned.lock->ServedStorageLease();

  disk::FileDevice first;
  ok = Expect(first.OpenServed(lease, path.string(), disk::FileOpenMode::open_existing).ok(),
              "served writable device refused") && ok;
  ok = Expect(!first.read_only(), "writable served device is read-only") && ok;

  // Same per-path serialization as a path open: a second device on another
  // thread waits until the first closes.
  std::atomic<bool> second_open{false};
  std::thread other([&] {
    disk::FileDevice second;
    if (second.OpenServed(lease, path.string(), disk::FileOpenMode::open_existing_read_only).ok()) {
      second_open = true;
      (void)second.Close();
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  ok = Expect(!second_open, "a second served device did not wait for the first") && ok;
  ok = Expect(first.Close().ok(), "writable served device close failed") && ok;
  other.join();
  ok = Expect(second_open, "the waiting served device never opened") && ok;
  owned.lock->release();
  return ok;
}

}  // namespace

int main() {
  const auto memory = scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
      scratchbird::core::memory::DefaultLocalEngineMemoryPolicy(), "c9_route_served_device_gate");
  if (!Expect(memory.ok(), "fixture memory setup failed")) return EXIT_FAILURE;
  bool ok = ReadOnlyServedDevice();
  ok = WritableServedDeviceAndSerialization() && ok;
  std::cout << (ok ? "c9_route_served_device_gate PASS" : "c9_route_served_device_gate FAIL") << '\n';
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
