// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/internal_api/sblr_ddl_create_schema_journal_lock.hpp"

#include <cerrno>
#include <iostream>
#include <string>

// An independent nonblocking native probe, not another use of Acquire's
// return value as the exclusion oracle. Exit 2 means actual lock contention.
int Probe(const std::string& path) {
#if defined(_WIN32)
  HANDLE file = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return 3;
  OVERLAPPED offset{};
  const bool acquired = ::LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK |
      LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &offset) != 0;
  const DWORD error = ::GetLastError();
  if (acquired) (void)::UnlockFileEx(file, 0, 1, 0, &offset);
  (void)::CloseHandle(file);
  return acquired ? 0 : error == ERROR_LOCK_VIOLATION ? 2 : 3;
#else
  const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) return 3;
  const bool acquired = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
  const int error = errno;
  if (acquired) (void)::flock(fd, LOCK_UN);
  (void)::close(fd);
  return acquired ? 0 : (error == EAGAIN || error == EWOULDBLOCK) ? 2 : 3;
#endif
}

int main(int argc, char** argv) {
  if (argc != 3) return 64;
  const std::string mode = argv[1];
  const std::string path = argv[2];
  if (mode == "probe") return Probe(path);
  if (mode != "hold" && mode != "refuse") return 64;
  scratchbird::engine::internal_api::detail::ScopedCreateSchemaJournalLock lock;
  if (mode == "refuse") {
    for (int i = 0; i < 64; ++i) if (lock.Acquire(path)) return 1;
    return 0;
  }
  std::cout << "ready" << std::endl;
  if (!lock.Acquire(path)) return 1;
  // A repeated acquisition must neither leak nor replace the original lock.
  if (lock.Acquire(path + ".second")) return 1;
  std::cout << "acquired" << std::endl;
  std::string release;
  if (!std::getline(std::cin, release) || release != "release") return 1;
  return 0;
}
