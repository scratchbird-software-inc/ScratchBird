// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "disk_device.hpp"

#include <cstdlib>
#include <filesystem>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace disk = scratchbird::storage::disk;

bool Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

std::filesystem::path TempRoot() {
  auto root = std::filesystem::temp_directory_path() / "scratchbird_public_disk_file_device_gate";
  std::filesystem::create_directories(root);
  return root;
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool CreateNewDoesNotTruncateExistingFile() {
  const auto path = TempRoot() / "existing.sbdb";
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "keep-existing-content";
  }

  disk::FileDevice device;
  const auto opened = device.Open(path.string(), disk::FileOpenMode::create_new);
  return Expect(!opened.ok(), "create_new should reject an existing file") &&
         Expect(ReadText(path) == "keep-existing-content",
                "create_new must not truncate or rewrite an existing file");
}

bool CreateNewSyncsAndPreservesWrittenBytes() {
  const auto path = TempRoot() / "created.sbdb";
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".sb.owner.lock");

  disk::FileDevice device;
  auto opened = device.Open(path.string(), disk::FileOpenMode::create_new);
  if (!Expect(opened.ok(), "create_new should atomically create a missing file")) {
    return false;
  }
  const std::string payload = "scratchbird-durable-sync";
  auto written = device.WriteAt(0, payload.data(), payload.size());
  auto synced = device.Sync();
  auto closed = device.Close();
  return Expect(written.ok(), "write_at should succeed for create_new file") &&
         Expect(synced.ok(), "sync should perform durable writable-file sync") &&
         Expect(closed.ok(), "close should succeed after sync") &&
         Expect(ReadText(path) == payload, "synced file content mismatch");
}

bool ConcurrentCreateNewHasSingleWinner() {
  const auto path = TempRoot() / "concurrent.sbdb";
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".sb.owner.lock");

  std::vector<bool> opened(2, false);
  std::vector<std::thread> threads;
  for (std::size_t i = 0; i < opened.size(); ++i) {
    threads.emplace_back([&, i]() {
      disk::FileDevice device;
      auto result = device.Open(path.string(), disk::FileOpenMode::create_new);
      opened[i] = result.ok();
      if (result.ok()) {
        const std::string payload = "winner-" + std::to_string(i);
        (void)device.WriteAt(0, payload.data(), payload.size());
        (void)device.Sync();
        (void)device.Close();
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  const int winners = static_cast<int>(opened[0]) + static_cast<int>(opened[1]);
  return Expect(winners == 1, "concurrent create_new must have exactly one winner") &&
         Expect(std::filesystem::exists(path), "concurrent create_new should leave one file");
}

}  // namespace

#ifndef _WIN32
// SEARCH_KEY: SB_OWNER_LOCK_FILE_REMOVED_ON_CLOSE (owner direction 2026-10-30).
std::filesystem::path FreshPath(const char* name) {
  const auto path = TempRoot() / name;
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".sb.owner.lock");
  return path;
}

// Runs `body` in a child process and returns its exit status.
template <typename Body>
int InChild(Body body) {
  const pid_t child = ::fork();
  if (child == 0) ::_exit(body() ? 0 : 1);
  int status = 0;
  if (child < 0 || ::waitpid(child, &status, 0) != child || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

bool LockFileRemovedOnCloseAndDestruction() {
  const auto path = FreshPath("lock_removed.sbdb");
  const auto lock = path.string() + ".sb.owner.lock";
  bool ok = true;
  {
    disk::FileDevice device;
    ok = Expect(device.Open(path.string(), disk::FileOpenMode::create_new).ok(),
                "create_new failed") && ok;
    ok = Expect(std::filesystem::exists(lock), "an open device must hold its lock file") && ok;
    ok = Expect(device.Close().ok(), "close failed") && ok;
    ok = Expect(!std::filesystem::exists(lock), "close left the owner lock file behind") && ok;
  }
  {
    disk::FileDevice device;
    ok = Expect(device.Open(path.string(), disk::FileOpenMode::open_existing).ok(),
                "reopen failed") && ok;
  }  // destroyed without Close()
  ok = Expect(!std::filesystem::exists(lock),
              "destruction without close left the owner lock file behind") && ok;
  return ok;
}

bool SecondProcessStillRefused() {
  const auto path = FreshPath("lock_exclusive.sbdb");
  disk::FileDevice owner;
  if (!Expect(owner.Open(path.string(), disk::FileOpenMode::create_new).ok(), "owner open failed"))
    return false;
  // A forked child inherits this process's locked per-path storage guard, so
  // it contends only at the OS lock, which is what crosses processes.
  const int refused = InChild([&] {
    const auto lock = path.string() + ".sb.owner.lock";
    const auto contender = disk::AcquireRemovableOwnerLockFile(lock, O_CLOEXEC);
    if (contender.ok()) ::close(contender.fd);
    return !contender.ok() && contender.held_by_other;
  });
  const bool closed = owner.Close().ok();
  return Expect(refused == 0, "a second process opened a database whose owner is live") &&
         Expect(closed, "owner close failed");
}

// A contender that opened the lock file before its owner removed it holds a
// descriptor on an unlinked inode. Locking that inode must not let it share
// ownership with the next owner of the path.
bool StaleDescriptorCannotSplitOwnership() {
  const auto path = FreshPath("lock_stale.sbdb");
  const auto lock = path.string() + ".sb.owner.lock";
  disk::FileDevice first;
  if (!Expect(first.Open(path.string(), disk::FileOpenMode::create_new).ok(), "first open failed"))
    return false;
  const int stale = ::open(lock.c_str(), O_RDWR | O_CLOEXEC);
  if (!Expect(stale >= 0, "could not open the live lock file")) return false;
  const bool first_closed = first.Close().ok();
  // Next owner, in another process, takes the path's (new) lock and keeps it.
  int ready[2];
  int release[2];
  if (::pipe(ready) != 0 || ::pipe(release) != 0) return Expect(false, "pipe failed");
  const pid_t child = ::fork();
  if (child == 0) {
    // OS-level protocol only (see SecondProcessStillRefused).
    const auto next = disk::AcquireRemovableOwnerLockFile(lock, O_CLOEXEC);
    const char status = next.ok() ? 'y' : 'n';
    (void)::write(ready[1], &status, 1);
    char go = 0;
    (void)::read(release[0], &go, 1);
    if (next.ok()) disk::ReleaseRemovableOwnerLockFile(next.fd, lock);
    ::_exit(0);
  }
  char next_opened = 0;
  (void)::read(ready[0], &next_opened, 1);
  // The stale inode is unlocked, so locking it succeeds -- and means nothing.
  const bool stale_locked = ::flock(stale, LOCK_EX | LOCK_NB) == 0;
  disk::FileDevice third;
  const auto third_open = third.Open(path.string(), disk::FileOpenMode::open_existing);
  const auto helper = disk::AcquireRemovableOwnerLockFile(lock, O_CLOEXEC);
  if (helper.ok()) disk::ReleaseRemovableOwnerLockFile(helper.fd, lock);
  (void)::write(release[1], "g", 1);
  int status = 0;
  (void)::waitpid(child, &status, 0);
  (void)::close(stale);
  for (int fd : {ready[0], ready[1], release[0], release[1]}) (void)::close(fd);
  return Expect(first_closed, "first close failed") &&
         Expect(next_opened == 'y', "the next owner could not open after the first closed") &&
         Expect(stale_locked, "fixture did not reach the stale-inode state") &&
         Expect(!third_open.ok(), "a stale lock let a third owner in beside the live one") &&
         Expect(!helper.ok() && helper.held_by_other,
                "the lock protocol did not report the live owner") &&
         Expect(!std::filesystem::exists(lock), "the last owner's close left its lock file");
}
#endif

void RemoveTempRoot() {
  std::error_code ignored;
  std::filesystem::remove_all(std::filesystem::temp_directory_path() /
                                  "scratchbird_public_disk_file_device_gate",
                              ignored);
}

int main() {
  // Tests clean up after themselves, including when a check fails.
  RemoveTempRoot();
  std::atexit(RemoveTempRoot);
  bool ok = true;
#ifndef _WIN32
  ok = LockFileRemovedOnCloseAndDestruction() && ok;
  ok = SecondProcessStillRefused() && ok;
  ok = StaleDescriptorCannotSplitOwnership() && ok;
#endif
  ok = CreateNewDoesNotTruncateExistingFile() && ok;
  ok = CreateNewSyncsAndPreservesWrittenBytes() && ok;
  ok = ConcurrentCreateNewHasSingleWinner() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
