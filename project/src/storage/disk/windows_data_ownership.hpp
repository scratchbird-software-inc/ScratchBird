// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstring>

namespace scratchbird::storage::disk::detail {

inline DWORD WindowsDataFileIdentity(HANDLE file, FILE_ID_INFO* identity) {
  BY_HANDLE_FILE_INFORMATION attributes{};
  if (::GetFileType(file) != FILE_TYPE_DISK) return ERROR_INVALID_HANDLE;
  if (!::GetFileInformationByHandle(file, &attributes)) return ::GetLastError();
  if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    return ERROR_DIRECTORY;
  // Keep the full binary file ID: the legacy 64-bit index is not sufficient
  // for every Windows filesystem. This is physical identity, not a UUID key.
  if (!::GetFileInformationByHandleEx(file, FileIdInfo, identity, sizeof(*identity)))
    return ::GetLastError();
  return ERROR_SUCCESS;
}

inline bool WindowsSameDataFile(const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
  return a.VolumeSerialNumber == b.VolumeSerialNumber &&
      std::memcmp(a.FileId.Identifier, b.FileId.Identifier,
                  sizeof(a.FileId.Identifier)) == 0;
}

inline DWORD WindowsLockDataFile(HANDLE file) {
  FILE_ID_INFO identity{};
  const DWORD error = WindowsDataFileIdentity(file, &identity);
  if (error != ERROR_SUCCESS) return error;
  // Cooperative ownership byte beyond the data extents supported by Windows
  // filesystems. Locking beyond EOF does not grow/write the file. Using a data
  // byte would block legitimate owner-internal I/O through separate handles.
  // This is NOT protection against arbitrary native or memory-mapped I/O.
  OVERLAPPED offset{};
  offset.Offset = 0xfffffffeu;
  offset.OffsetHigh = 0x7fffffffu;
  if (!::LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, 1, 0, &offset)) return ::GetLastError();
  return ERROR_SUCCESS;
}

}  // namespace scratchbird::storage::disk::detail
#endif
