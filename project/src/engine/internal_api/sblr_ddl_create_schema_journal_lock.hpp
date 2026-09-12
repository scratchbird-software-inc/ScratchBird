// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace scratchbird::engine::internal_api::detail {

// Serializes access to one CREATE SCHEMA recovery record. This is not a
// database ownership lease or an OS security boundary. Keep the sidecar inode
// stable: unlinking it on release would let existing waiters lock an old file.
class ScopedCreateSchemaJournalLock final {
 public:
  ScopedCreateSchemaJournalLock() = default;
  ScopedCreateSchemaJournalLock(const ScopedCreateSchemaJournalLock&) = delete;
  ScopedCreateSchemaJournalLock& operator=(const ScopedCreateSchemaJournalLock&) = delete;
  ~ScopedCreateSchemaJournalLock() { Release(); }

  bool Acquire(const std::string& path) {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) return false;
    // Synchronous, non-inheritable handle. Other contenders must be able to
    // open this sidecar and wait on its lock, but not delete/replace it.
    handle_ = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION metadata{};
    if (::GetFileType(handle_) != FILE_TYPE_DISK ||
        !::GetFileInformationByHandle(handle_, &metadata) ||
        (metadata.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
      Release();
      return false;
    }
    OVERLAPPED offset{};
    // All users lock byte zero of the stable sidecar, including an empty
    // newly created file. No journal data is read through this handle.
    if (!::LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &offset)) {
      Release();
      return false;
    }
#else
    if (fd_ >= 0) return false;
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd_ < 0) return false;
    struct stat metadata{};
    if (::fstat(fd_, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        ::flock(fd_, LOCK_EX) != 0) {
      Release();
      return false;
    }
#endif
    acquired_ = true;
    return true;
  }

 private:
  void Release() {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) {
      if (acquired_) {
        OVERLAPPED offset{};
        (void)::UnlockFileEx(handle_, 0, 1, 0, &offset);
      }
      (void)::CloseHandle(handle_);
    }
    handle_ = INVALID_HANDLE_VALUE;
#else
    if (fd_ >= 0) {
      if (acquired_) (void)::flock(fd_, LOCK_UN);
      (void)::close(fd_);
    }
    fd_ = -1;
#endif
    acquired_ = false;
  }

  bool acquired_ = false;
#if defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};

}  // namespace scratchbird::engine::internal_api::detail
