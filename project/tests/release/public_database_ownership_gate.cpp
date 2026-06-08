// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_ownership.hpp"
#include "disk_device.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace disk = scratchbird::storage::disk;
namespace server = scratchbird::server;

bool Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

std::filesystem::path TempRoot() {
  auto root = std::filesystem::temp_directory_path() /
              "scratchbird_public_database_ownership_gate";
  std::filesystem::create_directories(root);
  return root;
}

void RemoveKnownFiles(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path.string() + ".sb.owner.lock", ignored);
  std::filesystem::remove(path.string() + ".sb.route.owner.lock", ignored);
}

void WriteDatabasePlaceholder(const std::filesystem::path& path) {
  RemoveKnownFiles(path);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << "scratchbird-public-ownership-placeholder";
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
  WriteDatabasePlaceholder(path);

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

bool StorageReadOnlyPolicyAllowsOnlyReadOnlyPeers() {
  const auto path = TempRoot() / "storage-readonly.sbdb";
  WriteDatabasePlaceholder(path);

  disk::FileDevice reader_a;
  const auto reader_a_open =
      reader_a.Open(path.string(), disk::FileOpenMode::open_existing_read_only);
  if (!Expect(reader_a_open.ok(), "first read-only storage open should succeed")) {
    return false;
  }

  disk::FileDevice reader_b;
  const auto reader_b_open =
      reader_b.Open(path.string(), disk::FileOpenMode::open_existing_read_only);
  bool ok = Expect(reader_b_open.ok(), "second read-only storage open should succeed");

  disk::FileDevice writer;
  const auto writer_open = writer.Open(path.string(), disk::FileOpenMode::open_existing);
  ok = Expect(!writer_open.ok(), "mutable storage open during read-only peers must fail closed") && ok;
  ok = Expect(writer_open.diagnostic.diagnostic_code ==
                  "SB-STORAGE-DISK-OWNER-LOCK-HELD",
              "mutable open during read-only peers used wrong diagnostic") && ok;

  ok = Expect(reader_b.Close().ok(), "second read-only storage close should succeed") && ok;
  ok = Expect(reader_a.Close().ok(), "first read-only storage close should succeed") && ok;
  return ok;
}

bool ServerOwnershipConflictsFailClosed() {
  const auto path = TempRoot() / "server-conflict.sbdb";
  WriteDatabasePlaceholder(path);

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
bool ChildStorageRouteConflict(const std::filesystem::path& path) {
  disk::FileDevice device;
  const auto opened = device.Open(path.string(), disk::FileOpenMode::open_existing);
  return Expect(!opened.ok(),
                "external storage opener should fail while server route owner is held") &&
         Expect(opened.diagnostic.diagnostic_code ==
                    "SB-STORAGE-DISK-ROUTE-OWNER-LOCK-HELD",
                "external storage opener used wrong route-conflict diagnostic");
}

bool ServerRouteBlocksExternalStorageOpen(const char* self_path) {
  const auto path = TempRoot() / "server-blocks-storage.sbdb";
  WriteDatabasePlaceholder(path);

  auto owner = server::AcquireDatabaseOwnership(OwnershipRequest(path, "server"));
  if (!Expect(owner.acquired, "server route owner acquisition should succeed")) {
    return false;
  }

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
#else
bool ServerRouteBlocksExternalStorageOpen(const char*) {
  return true;
}
#endif

}  // namespace

int main(int argc, char** argv) {
#ifndef _WIN32
  if (argc == 3 && std::string(argv[1]) == "--expect-storage-route-conflict") {
    return ChildStorageRouteConflict(argv[2]) ? EXIT_SUCCESS : EXIT_FAILURE;
  }
#endif

  bool ok = true;
  ok = StorageMutableConflictsFailClosed() && ok;
  ok = StorageReadOnlyPolicyAllowsOnlyReadOnlyPeers() && ok;
  ok = ServerOwnershipConflictsFailClosed() && ok;
  ok = ServerRouteBlocksExternalStorageOpen(argv[0]) && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
