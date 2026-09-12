// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace scratchbird::engine::internal_api::detail {

enum class CreateSchemaJournalFileStatus { created, exists, failed };

#if !defined(_WIN32)
inline bool SyncCreateSchemaJournalParent(const std::string& path) {
  auto parent = std::filesystem::path(path).parent_path();
  if (parent.empty()) parent = ".";
  const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool synced = ::fsync(fd) == 0;
  const bool closed = ::close(fd) == 0;
  return synced && closed;
}
#endif

// Callers serialize the whole record transition with the native record lock.
// An existing name must never be truncated, even by a competing creator.
inline CreateSchemaJournalFileStatus CreateSchemaJournalFileExclusive(
    const std::string& path, const std::vector<std::uint8_t>& bytes) {
  using Status = CreateSchemaJournalFileStatus;
#if defined(_WIN32)
  HANDLE file = ::CreateFileA(path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr,
                              CREATE_NEW, FILE_FLAG_WRITE_THROUGH |
                                  FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS
        ? Status::exists : Status::failed;
  }
  bool ok = true;
  std::size_t offset = 0;
  while (offset != bytes.size()) {
    const auto remaining = bytes.size() - offset;
    const DWORD count = static_cast<DWORD>(
        remaining > (std::numeric_limits<DWORD>::max)()
            ? (std::numeric_limits<DWORD>::max)() : remaining);
    DWORD written = 0;
    if (!::WriteFile(file, bytes.data() + offset, count, &written, nullptr) ||
        written == 0 || written > count) {
      ok = false;
      break;
    }
    offset += written;
  }
  // Stream flush is not durable-file evidence. Check the native data and
  // metadata flush on the exact newly created handle before reporting created.
  ok = ok && ::FlushFileBuffers(file) != 0;
  if (!ok) {
    // Mark only our open file for deletion; never delete a possibly retargeted
    // pathname after close. Cleanup failure still cannot become success.
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    (void)::SetFileInformationByHandle(file, FileDispositionInfo,
                                      &disposition, sizeof(disposition));
  }
  const bool closed = ::CloseHandle(file) != 0;
  return ok && closed ? Status::created : Status::failed;
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL |
                                          O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return errno == EEXIST ? Status::exists : Status::failed;
  bool ok = true;
  std::size_t offset = 0;
  while (offset != bytes.size()) {
    const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count <= 0) {
      ok = false;
      break;
    }
    offset += static_cast<std::size_t>(count);
  }
  ok = ok && ::fsync(fd) == 0;
  if (::close(fd) != 0) ok = false;
  if (!ok) {
    (void)::unlink(path.c_str());
    return Status::failed;
  }
  return SyncCreateSchemaJournalParent(path) ? Status::created : Status::failed;
#endif
}

// The caller supplies a completely written/flushed exclusive temporary file
// in the target's directory. Never allow copy/delete across volumes.
inline bool PublishCreateSchemaJournalReplacement(
    const std::string& prepared, const std::string& target) {
#if defined(_WIN32)
  return ::MoveFileExA(prepared.c_str(), target.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code error;
  std::filesystem::rename(prepared, target, error);
  return !error && SyncCreateSchemaJournalParent(target);
#endif
}

}  // namespace scratchbird::engine::internal_api::detail
