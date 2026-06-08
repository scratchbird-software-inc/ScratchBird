// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "disk_device.hpp"

#include "metric_contracts.hpp"
#include "metric_producer.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace scratchbird::storage::disk {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;

Status DiskOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_disk};
}

Status DiskErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_disk};
}

using Clock = std::chrono::steady_clock;

double ElapsedMicros(Clock::time_point start) {
  return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

void RecordDiskLatency(const char* family, double micros, const std::string& path, const char* operation, const char* result) {
  (void)scratchbird::core::metrics::ObserveHistogram(
      family,
      scratchbird::core::metrics::Labels({{"component", "storage.disk"}, {"operation", operation}, {"result", result},
                                          {"device_class", path.empty() ? "unopened" : "file"}}),
      micros,
      "storage_disk");
}

void RecordDiskError(const char* reason, const std::string& path) {
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_storage_device_errors_total",
      scratchbird::core::metrics::Labels({{"component", "storage.disk"}, {"reason", reason},
                                          {"device_class", path.empty() ? "unopened" : "file"}}),
      1.0,
      "storage_disk");
}

void RecordDiskHealth(const char* result, const DiskDevicePolicy& policy, const std::string& path) {
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_storage_device_health_checks_total",
      scratchbird::core::metrics::Labels({{"component", "storage.disk"}, {"result", result},
                                          {"access_mode", DiskAccessModeName(policy.access_mode)},
                                          {"device_class", path.empty() ? "unopened" : "file"}}),
      1.0,
      "storage_disk");
}

void RecordDiskPolicyViolation(const char* reason, const DiskDevicePolicy& policy, const std::string& path) {
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_storage_device_policy_violations_total",
      scratchbird::core::metrics::Labels({{"component", "storage.disk"}, {"reason", reason},
                                          {"unknown_page_policy", UnknownPagePolicyName(policy.unknown_page_policy)},
                                          {"device_class", path.empty() ? "unopened" : "file"}}),
      1.0,
      "storage_disk");
}

void RecordUnknownPageRejection(PageClassificationKind kind, const DiskDevicePolicy& policy) {
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_storage_unknown_page_rejections_total",
      scratchbird::core::metrics::Labels({{"component", "storage.disk"}, {"classification", PageClassificationKindName(kind)},
                                          {"unknown_page_policy", UnknownPagePolicyName(policy.unknown_page_policy)}}),
      1.0,
      "storage_disk");
}

IoResult DiskPolicyIoError(std::string diagnostic_code,
                           std::string message_key,
                           std::string path,
                           std::string detail = {}) {
  IoResult result;
  result.status = DiskErrorStatus();
  result.diagnostic = MakeDiskDiagnostic(result.status,
                                         std::move(diagnostic_code),
                                         std::move(message_key),
                                         std::move(path),
                                         std::move(detail));
  return result;
}

CheckedFileExtentResult DiskCheckedExtentError(std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail = {}) {
  CheckedFileExtentResult result;
  result.status = DiskErrorStatus();
  result.diagnostic = MakeDiskDiagnostic(result.status,
                                         std::move(diagnostic_code),
                                         std::move(message_key),
                                         {},
                                         std::move(detail));
  return result;
}

DiskHealthResult DiskHealthError(std::string diagnostic_code,
                                 std::string message_key,
                                 const FileDevice& device,
                                 const DiskDevicePolicy& policy,
                                 DiskHealthSnapshot snapshot,
                                 std::string detail = {}) {
  RecordDiskHealth("error", policy, device.path());
  DiskHealthResult result;
  result.status = DiskErrorStatus();
  result.snapshot = std::move(snapshot);
  result.snapshot.health = "error";
  result.diagnostic = MakeDiskDiagnostic(result.status,
                                         std::move(diagnostic_code),
                                         std::move(message_key),
                                         device.path(),
                                         std::move(detail));
  return result;
}

DiskPageHeaderResult DiskPageHeaderError(std::string diagnostic_code,
                                         std::string message_key,
                                         const FileDevice* device,
                                         const DiskDevicePolicy& policy,
                                         PageClassification classification,
                                         std::string detail = {}) {
  RecordUnknownPageRejection(classification.kind, policy);
  DiskPageHeaderResult result;
  result.status = DiskErrorStatus();
  result.classification = std::move(classification);
  result.diagnostic = MakeDiskDiagnostic(result.status,
                                         std::move(diagnostic_code),
                                         std::move(message_key),
                                         device == nullptr ? std::string{} : device->path(),
                                         std::move(detail));
  return result;
}

bool PolicyAllowsClassification(PageClassificationKind kind, const DiskDevicePolicy& policy) {
  switch (kind) {
    case PageClassificationKind::supported_local:
      return true;
    case PageClassificationKind::reserved_local:
    case PageClassificationKind::unknown_safe:
      return policy.unknown_page_policy == UnknownPagePolicy::allow_unknown_safe_read_only;
    case PageClassificationKind::cluster_only:
      return false;
    case PageClassificationKind::encrypted_or_opaque:
      return policy.unknown_page_policy == UnknownPagePolicy::allow_encrypted_opaque_read_only;
    case PageClassificationKind::invalid_magic:
    case PageClassificationKind::invalid_header:
    case PageClassificationKind::checksum_mismatch:
      return false;
  }
  return false;
}

bool AddWouldOverflow(u64 lhs, u64 rhs) {
  return lhs > std::numeric_limits<u64>::max() - rhs;
}

bool MulWouldOverflow(u64 lhs, u64 rhs) {
  return rhs != 0 && lhs > std::numeric_limits<u64>::max() / rhs;
}

enum class RouteOwnerProbeResult {
  kAvailable,
  kHeldByCurrentProcess,
  kHeldByOtherProcess,
};

bool RouteOwnerLockHeldByCurrentProcess(const std::string& route_lock_path) {
  std::ifstream in(route_lock_path);
  std::string line;
  while (std::getline(in, line)) {
    constexpr const char* kPidPrefix = "pid=";
    if (line.rfind(kPidPrefix, 0) != 0) {
      continue;
    }
    const std::string pid = line.substr(std::strlen(kPidPrefix));
#ifdef _WIN32
    return pid == std::to_string(static_cast<unsigned long long>(::GetCurrentProcessId()));
#else
    return pid == std::to_string(static_cast<unsigned long long>(::getpid()));
#endif
  }
  return false;
}

std::mutex& RouteOwnedStorageRegistryMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, std::shared_ptr<std::recursive_mutex>>&
RouteOwnedStorageRegistry() {
  static std::map<std::string, std::shared_ptr<std::recursive_mutex>> registry;
  return registry;
}

std::unique_lock<std::recursive_mutex> AcquireRouteOwnedStorageGuard(
    const std::string& path) {
  std::shared_ptr<std::recursive_mutex> mutex;
  {
    std::lock_guard<std::mutex> guard(RouteOwnedStorageRegistryMutex());
    auto& slot = RouteOwnedStorageRegistry()[path];
    if (!slot) {
      slot = std::make_shared<std::recursive_mutex>();
    }
    mutex = slot;
  }
  return std::unique_lock<std::recursive_mutex>(*mutex);
}

#ifdef _WIN32
std::string WindowsLastErrorText(DWORD error = ::GetLastError()) {
  char* buffer = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                      FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD length = ::FormatMessageA(flags,
                                        nullptr,
                                        error,
                                        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                        reinterpret_cast<LPSTR>(&buffer),
                                        0,
                                        nullptr);
  std::string message = length == 0 || buffer == nullptr
                            ? ("windows_error_" + std::to_string(error))
                            : std::string(buffer, length);
  if (buffer != nullptr) {
    ::LocalFree(buffer);
  }
  while (!message.empty() &&
         (message.back() == '\n' || message.back() == '\r' ||
          message.back() == ' ')) {
    message.pop_back();
  }
  return message;
}

bool WindowsSharingConflict(DWORD error) {
  return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
}

bool DurableSyncHandle(HANDLE handle, std::string* detail) {
  if (::FlushFileBuffers(handle) != 0) {
    return true;
  }
  if (detail != nullptr) {
    *detail = WindowsLastErrorText();
  }
  return false;
}

bool DurableSyncPath(const std::string& path, bool writable, std::string* detail) {
  const DWORD access = GENERIC_READ | (writable ? GENERIC_WRITE : 0);
  HANDLE handle = ::CreateFileA(path.c_str(),
                                access,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (detail != nullptr) {
      *detail = WindowsLastErrorText();
    }
    return false;
  }
  const bool ok = DurableSyncHandle(handle, detail);
  ::CloseHandle(handle);
  return ok;
}

bool DurableSyncParentDirectory(const std::string& path, std::string* detail) {
  std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  HANDLE handle = ::CreateFileA(parent.string().c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS,
                                nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (detail != nullptr) {
      *detail = WindowsLastErrorText();
    }
    return false;
  }
  const bool ok = DurableSyncHandle(handle, detail);
  ::CloseHandle(handle);
  return ok;
}

void* OpenOwnerLockFile(const std::string& path,
                        bool shared_read_only,
                        bool* lock_held,
                        std::string* detail) {
  if (lock_held != nullptr) {
    *lock_held = false;
  }
  const DWORD share_mode =
      shared_read_only ? FILE_SHARE_READ : 0;
  const DWORD access =
      shared_read_only ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
  HANDLE handle = ::CreateFileA(path.c_str(),
                                access,
                                share_mode,
                                nullptr,
                                OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    if (lock_held != nullptr) {
      *lock_held = WindowsSharingConflict(error);
    }
    if (detail != nullptr) {
      *detail = WindowsLastErrorText(error);
    }
    return nullptr;
  }
  return handle;
}

bool WriteOwnerLockPayload(void* owner_lock_handle,
                           const std::string& payload,
                           std::string* detail) {
  HANDLE handle = static_cast<HANDLE>(owner_lock_handle);
  LARGE_INTEGER zero{};
  if (::SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN) == 0 ||
      ::SetEndOfFile(handle) == 0) {
    if (detail != nullptr) {
      *detail = WindowsLastErrorText();
    }
    return false;
  }
  DWORD written = 0;
  if (::WriteFile(handle,
                  payload.data(),
                  static_cast<DWORD>(payload.size()),
                  &written,
                  nullptr) == 0 ||
      written != payload.size()) {
    if (detail != nullptr) {
      *detail = WindowsLastErrorText();
    }
    return false;
  }
  return true;
}

void* OpenDataFileHandle(const std::string& path,
                         FileOpenMode mode,
                         bool read_only_open,
                         bool* created,
                         bool* exists,
                         std::string* detail) {
  if (created != nullptr) {
    *created = false;
  }
  if (exists != nullptr) {
    *exists = false;
  }
  DWORD disposition = OPEN_EXISTING;
  switch (mode) {
    case FileOpenMode::open_existing:
    case FileOpenMode::open_existing_read_only:
      disposition = OPEN_EXISTING;
      break;
    case FileOpenMode::create_new:
      disposition = CREATE_NEW;
      break;
    case FileOpenMode::create_or_truncate:
      disposition = CREATE_ALWAYS;
      break;
  }
  const DWORD access = GENERIC_READ | (read_only_open ? 0 : GENERIC_WRITE);
  HANDLE handle = ::CreateFileA(path.c_str(),
                                access,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                disposition,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
      if (exists != nullptr) {
        *exists = true;
      }
    }
    if (detail != nullptr) {
      *detail = WindowsLastErrorText(error);
    }
    return nullptr;
  }
  const DWORD create_status = ::GetLastError();
  if (created != nullptr) {
    *created = mode == FileOpenMode::create_new ||
               (mode == FileOpenMode::create_or_truncate &&
                create_status != ERROR_ALREADY_EXISTS);
  }
  return handle;
}

bool CloseDataFileHandle(void* file_handle, std::string* detail) {
  if (file_handle == nullptr) {
    return true;
  }
  if (::CloseHandle(static_cast<HANDLE>(file_handle)) != 0) {
    return true;
  }
  if (detail != nullptr) {
    *detail = WindowsLastErrorText();
  }
  return false;
}

OVERLAPPED OffsetToOverlapped(u64 offset) {
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset & 0xffffffffu);
  overlapped.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xffffffffu);
  return overlapped;
}

bool NativeReadAt(void* file_handle,
                  u64 offset,
                  void* buffer,
                  usize bytes,
                  usize* transferred,
                  std::string* detail) {
  HANDLE handle = static_cast<HANDLE>(file_handle);
  auto* cursor = static_cast<char*>(buffer);
  usize total = 0;
  while (total < bytes) {
    const DWORD chunk = static_cast<DWORD>(std::min<usize>(
        bytes - total,
        static_cast<usize>(std::numeric_limits<DWORD>::max())));
    OVERLAPPED overlapped = OffsetToOverlapped(offset + total);
    DWORD read = 0;
    if (::ReadFile(handle, cursor + total, chunk, &read, &overlapped) == 0) {
      const DWORD error = ::GetLastError();
      if (error == ERROR_HANDLE_EOF) {
        break;
      }
      if (detail != nullptr) {
        *detail = WindowsLastErrorText(error);
      }
      if (transferred != nullptr) {
        *transferred = total;
      }
      return false;
    }
    if (read == 0) {
      break;
    }
    total += static_cast<usize>(read);
  }
  if (transferred != nullptr) {
    *transferred = total;
  }
  return total == bytes;
}

bool NativeWriteAt(void* file_handle,
                   u64 offset,
                   const void* buffer,
                   usize bytes,
                   usize* transferred,
                   std::string* detail) {
  HANDLE handle = static_cast<HANDLE>(file_handle);
  const auto* cursor = static_cast<const char*>(buffer);
  usize total = 0;
  while (total < bytes) {
    const DWORD chunk = static_cast<DWORD>(std::min<usize>(
        bytes - total,
        static_cast<usize>(std::numeric_limits<DWORD>::max())));
    OVERLAPPED overlapped = OffsetToOverlapped(offset + total);
    DWORD written = 0;
    if (::WriteFile(handle, cursor + total, chunk, &written, &overlapped) == 0 ||
        written == 0) {
      if (detail != nullptr) {
        *detail = WindowsLastErrorText();
      }
      if (transferred != nullptr) {
        *transferred = total;
      }
      return false;
    }
    total += static_cast<usize>(written);
  }
  if (transferred != nullptr) {
    *transferred = total;
  }
  return true;
}

bool NativeFileSize(void* file_handle, u64* size_bytes, std::string* detail) {
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(static_cast<HANDLE>(file_handle), &size) == 0) {
    if (detail != nullptr) {
      *detail = WindowsLastErrorText();
    }
    return false;
  }
  if (size.QuadPart < 0) {
    if (detail != nullptr) {
      *detail = "negative file size";
    }
    return false;
  }
  if (size_bytes != nullptr) {
    *size_bytes = static_cast<u64>(size.QuadPart);
  }
  return true;
}

RouteOwnerProbeResult ProbeRouteOwnerLock(const std::string& path,
                                          std::string* detail) {
  const std::string route_lock_path = path + ".sb.route.owner.lock";
  if (!std::filesystem::exists(route_lock_path)) {
    return RouteOwnerProbeResult::kAvailable;
  }
  HANDLE handle = ::CreateFileA(route_lock_path.c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                0,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (detail != nullptr) {
      *detail = route_lock_path + ":" + WindowsLastErrorText();
    }
    return RouteOwnerLockHeldByCurrentProcess(route_lock_path)
               ? RouteOwnerProbeResult::kHeldByCurrentProcess
               : RouteOwnerProbeResult::kHeldByOtherProcess;
  }
  ::CloseHandle(handle);
  return RouteOwnerProbeResult::kAvailable;
}
#else
int FileOpenCloexecFlag() {
#ifdef O_CLOEXEC
  return O_CLOEXEC;
#else
  return 0;
#endif
}

bool DurableSyncFd(int fd, std::string* detail) {
  if (::fsync(fd) == 0) {
    return true;
  }
  if (detail != nullptr) {
    *detail = std::strerror(errno);
  }
  return false;
}

bool DurableSyncPath(const std::string& path, bool writable, std::string* detail) {
  const int flags = (writable ? O_RDWR : O_RDONLY) | FileOpenCloexecFlag();
  const int fd = ::open(path.c_str(), flags);
  if (fd < 0) {
    if (detail != nullptr) {
      *detail = std::strerror(errno);
    }
    return false;
  }
  const bool ok = DurableSyncFd(fd, detail);
  (void)::close(fd);
  return ok;
}

bool DurableSyncParentDirectory(const std::string& path, std::string* detail) {
  std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  const int flags = O_RDONLY | O_DIRECTORY | FileOpenCloexecFlag();
  const int fd = ::open(parent.string().c_str(), flags);
  if (fd < 0) {
    if (detail != nullptr) {
      *detail = std::strerror(errno);
    }
    return false;
  }
  const bool ok = DurableSyncFd(fd, detail);
  (void)::close(fd);
  return ok;
}

int OpenDataFileHandle(const std::string& path,
                       FileOpenMode mode,
                       bool read_only_open,
                       bool* created,
                       bool* exists,
                       std::string* detail) {
  if (created != nullptr) {
    *created = false;
  }
  if (exists != nullptr) {
    *exists = false;
  }
  int flags = read_only_open ? O_RDONLY : O_RDWR;
  if (mode == FileOpenMode::create_new) {
    flags |= O_CREAT | O_EXCL;
  } else if (mode == FileOpenMode::create_or_truncate) {
    if (!std::filesystem::exists(path) && created != nullptr) {
      *created = true;
    }
    flags |= O_CREAT | O_TRUNC;
  }
  flags |= FileOpenCloexecFlag();
  const int fd = ::open(path.c_str(), flags, 0600);
  if (fd < 0) {
    const int saved_errno = errno;
    if (saved_errno == EEXIST && exists != nullptr) {
      *exists = true;
    }
    if (detail != nullptr) {
      *detail = std::strerror(saved_errno);
    }
    return -1;
  }
  if (mode == FileOpenMode::create_new && created != nullptr) {
    *created = true;
  }
  return fd;
}

bool CloseDataFileHandle(int fd, std::string* detail) {
  if (fd < 0) {
    return true;
  }
  if (::close(fd) != 0) {
    if (detail != nullptr) {
      *detail = std::strerror(errno);
    }
    return false;
  }
  return true;
}

bool NativeReadAt(int fd,
                  u64 offset,
                  void* buffer,
                  usize bytes,
                  usize* transferred,
                  std::string* detail) {
  auto* cursor = static_cast<char*>(buffer);
  usize total = 0;
  while (total < bytes) {
    const auto chunk = static_cast<std::size_t>(
        std::min<usize>(bytes - total,
                        static_cast<usize>(std::numeric_limits<ssize_t>::max())));
    const auto rc = ::pread(fd,
                            cursor + total,
                            chunk,
                            static_cast<off_t>(offset + total));
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (detail != nullptr) {
        *detail = std::strerror(errno);
      }
      if (transferred != nullptr) {
        *transferred = total;
      }
      return false;
    }
    if (rc == 0) {
      break;
    }
    total += static_cast<usize>(rc);
  }
  if (transferred != nullptr) {
    *transferred = total;
  }
  return total == bytes;
}

bool NativeWriteAt(int fd,
                   u64 offset,
                   const void* buffer,
                   usize bytes,
                   usize* transferred,
                   std::string* detail) {
  const auto* cursor = static_cast<const char*>(buffer);
  usize total = 0;
  while (total < bytes) {
    const auto chunk = static_cast<std::size_t>(
        std::min<usize>(bytes - total,
                        static_cast<usize>(std::numeric_limits<ssize_t>::max())));
    const auto rc = ::pwrite(fd,
                             cursor + total,
                             chunk,
                             static_cast<off_t>(offset + total));
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (detail != nullptr) {
        *detail = std::strerror(errno);
      }
      if (transferred != nullptr) {
        *transferred = total;
      }
      return false;
    }
    if (rc == 0) {
      if (detail != nullptr) {
        *detail = "short write";
      }
      if (transferred != nullptr) {
        *transferred = total;
      }
      return false;
    }
    total += static_cast<usize>(rc);
  }
  if (transferred != nullptr) {
    *transferred = total;
  }
  return true;
}

bool NativeFileSize(int fd, u64* size_bytes, std::string* detail) {
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    if (detail != nullptr) {
      *detail = std::strerror(errno);
    }
    return false;
  }
  if (st.st_size < 0) {
    if (detail != nullptr) {
      *detail = "negative file size";
    }
    return false;
  }
  if (size_bytes != nullptr) {
    *size_bytes = static_cast<u64>(st.st_size);
  }
  return true;
}

RouteOwnerProbeResult ProbeRouteOwnerLock(const std::string& path,
                                          std::string* detail) {
  const std::string route_lock_path = path + ".sb.route.owner.lock";
  if (!std::filesystem::exists(route_lock_path)) {
    return RouteOwnerProbeResult::kAvailable;
  }
  const int fd = ::open(route_lock_path.c_str(), O_RDWR | FileOpenCloexecFlag());
  if (fd < 0) {
    if (detail != nullptr) {
      *detail = route_lock_path + ":" + std::strerror(errno);
    }
    return RouteOwnerProbeResult::kHeldByOtherProcess;
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int saved_errno = errno;
    (void)::close(fd);
    if (detail != nullptr) {
      *detail = route_lock_path + ":" + std::strerror(saved_errno);
    }
    return RouteOwnerLockHeldByCurrentProcess(route_lock_path)
               ? RouteOwnerProbeResult::kHeldByCurrentProcess
               : RouteOwnerProbeResult::kHeldByOtherProcess;
  }
  (void)::flock(fd, LOCK_UN);
  (void)::close(fd);
  return RouteOwnerProbeResult::kAvailable;
}
#endif

}  // namespace

FileDevice::FileDevice() = default;

FileDevice::~FileDevice() {
#ifdef _WIN32
  if (file_handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(file_handle_));
    file_handle_ = nullptr;
  }
  if (owner_lock_handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(owner_lock_handle_));
    owner_lock_handle_ = nullptr;
  }
#else
  if (file_fd_ >= 0) {
    (void)::close(file_fd_);
    file_fd_ = -1;
  }
  if (owner_lock_fd_ >= 0) {
    (void)::flock(owner_lock_fd_, LOCK_UN);
    (void)::close(owner_lock_fd_);
    owner_lock_fd_ = -1;
  }
#endif
  if (owner_lock_held_ && owner_lock_exclusive_ && !owner_lock_path_.empty()) {
    std::error_code ignored;
    std::filesystem::remove(owner_lock_path_, ignored);
  }
}

IoResult FileDevice::Open(std::string path, FileOpenMode mode) {
  if (is_open()) {
    return MakeIoError("SB-STORAGE-DISK-ALREADY-OPEN",
                       "storage.disk.already_open",
                       path);
  }

  const bool read_only_open = mode == FileOpenMode::open_existing_read_only;
  bool created_new_file = false;
  if ((mode == FileOpenMode::open_existing ||
       mode == FileOpenMode::open_existing_read_only) &&
      !std::filesystem::exists(path)) {
    return MakeIoError("SB-STORAGE-DISK-OPEN-MISSING",
                       "storage.disk.open_missing",
                       path);
  }

  // OWNER_LOCK: direct storage opens must not bypass an active server route owner.
  std::string route_owner_lock_detail;
  const auto route_owner_probe =
      ProbeRouteOwnerLock(path, &route_owner_lock_detail);
  if (route_owner_probe == RouteOwnerProbeResult::kHeldByOtherProcess) {
    return MakeIoError("SB-STORAGE-DISK-ROUTE-OWNER-LOCK-HELD",
                       "storage.disk.route_owner_lock_held",
                       route_owner_lock_detail);
  }
  const bool route_owned_by_current_process =
      route_owner_probe == RouteOwnerProbeResult::kHeldByCurrentProcess;
  std::unique_lock<std::recursive_mutex> route_owner_storage_guard;
  if (route_owned_by_current_process) {
    route_owner_storage_guard = AcquireRouteOwnedStorageGuard(path);
  }

  const std::string prospective_owner_lock_path = path + ".sb.owner.lock";
#ifdef _WIN32
  std::string owner_lock_open_detail;
  void* prospective_owner_lock_handle = nullptr;
  if (!route_owned_by_current_process) {
    bool owner_lock_held = false;
    prospective_owner_lock_handle =
        OpenOwnerLockFile(prospective_owner_lock_path,
                          read_only_open,
                          &owner_lock_held,
                          &owner_lock_open_detail);
    if (prospective_owner_lock_handle == nullptr) {
      if (owner_lock_held) {
        return MakeIoError("SB-STORAGE-DISK-OWNER-LOCK-HELD",
                           "storage.disk.owner_lock_held",
                           prospective_owner_lock_path + ":" +
                               owner_lock_open_detail);
      }
      return MakeIoError("SB-STORAGE-DISK-OWNER-LOCK-OPEN-FAILED",
                         "storage.disk.owner_lock_open_failed",
                         prospective_owner_lock_path + ":" +
                             owner_lock_open_detail);
    }
  }
  auto release_owner_lock = [&]() {
    if (prospective_owner_lock_handle != nullptr) {
      ::CloseHandle(static_cast<HANDLE>(prospective_owner_lock_handle));
      prospective_owner_lock_handle = nullptr;
    }
  };
#else
  int prospective_owner_lock_fd = -1;
  if (!route_owned_by_current_process) {
    prospective_owner_lock_fd =
        ::open(prospective_owner_lock_path.c_str(),
               O_RDWR | O_CREAT | FileOpenCloexecFlag(),
               0600);
    if (prospective_owner_lock_fd < 0) {
      return MakeIoError("SB-STORAGE-DISK-OWNER-LOCK-OPEN-FAILED",
                         "storage.disk.owner_lock_open_failed",
                         prospective_owner_lock_path + ":" + std::strerror(errno));
    }
    const int lock_mode = (read_only_open ? LOCK_SH : LOCK_EX) | LOCK_NB;
    if (::flock(prospective_owner_lock_fd, lock_mode) != 0) {
      (void)::close(prospective_owner_lock_fd);
      prospective_owner_lock_fd = -1;
      return MakeIoError("SB-STORAGE-DISK-OWNER-LOCK-HELD",
                         "storage.disk.owner_lock_held",
                         prospective_owner_lock_path);
    }
  }
  auto release_owner_lock = [&]() {
    if (prospective_owner_lock_fd >= 0) {
      (void)::flock(prospective_owner_lock_fd, LOCK_UN);
      (void)::close(prospective_owner_lock_fd);
      prospective_owner_lock_fd = -1;
    }
  };
#endif

#ifdef _WIN32
  bool create_exists = false;
  std::string open_detail;
  void* prospective_file_handle = OpenDataFileHandle(path,
                                                     mode,
                                                     read_only_open,
                                                     &created_new_file,
                                                     &create_exists,
                                                     &open_detail);
  if (prospective_file_handle == nullptr) {
    release_owner_lock();
    if (create_exists) {
      return MakeIoError("SB-STORAGE-DISK-CREATE-EXISTS",
                         "storage.disk.create_exists",
                         path);
    }
    return MakeIoError(mode == FileOpenMode::open_existing ||
                               mode == FileOpenMode::open_existing_read_only
                           ? "SB-STORAGE-DISK-OPEN-FAILED"
                           : "SB-STORAGE-DISK-CREATE-FAILED",
                       mode == FileOpenMode::open_existing ||
                               mode == FileOpenMode::open_existing_read_only
                           ? "storage.disk.open_failed"
                           : "storage.disk.create_failed",
                       path + ":" + open_detail);
  }
  auto release_file_handle = [&]() {
    if (prospective_file_handle != nullptr) {
      (void)CloseDataFileHandle(prospective_file_handle, nullptr);
      prospective_file_handle = nullptr;
    }
  };
#else
  bool create_exists = false;
  std::string open_detail;
  int prospective_file_fd = OpenDataFileHandle(path,
                                               mode,
                                               read_only_open,
                                               &created_new_file,
                                               &create_exists,
                                               &open_detail);
  if (prospective_file_fd < 0) {
    release_owner_lock();
    if (create_exists) {
      return MakeIoError("SB-STORAGE-DISK-CREATE-EXISTS",
                         "storage.disk.create_exists",
                         path);
    }
    return MakeIoError(mode == FileOpenMode::open_existing ||
                               mode == FileOpenMode::open_existing_read_only
                           ? "SB-STORAGE-DISK-OPEN-FAILED"
                           : "SB-STORAGE-DISK-CREATE-FAILED",
                       mode == FileOpenMode::open_existing ||
                               mode == FileOpenMode::open_existing_read_only
                           ? "storage.disk.open_failed"
                           : "storage.disk.create_failed",
                       path + ":" + open_detail);
  }
  auto release_file_handle = [&]() {
    if (prospective_file_fd >= 0) {
      (void)CloseDataFileHandle(prospective_file_fd, nullptr);
      prospective_file_fd = -1;
    }
  };
#endif

  if (!read_only_open && !route_owned_by_current_process) {
#ifdef _WIN32
    const std::string lock_payload =
        "scratchbird.owner.lock.v1\npath=" + path + "\nmode=exclusive\n";
    std::string lock_write_detail;
    if (!WriteOwnerLockPayload(prospective_owner_lock_handle,
                               lock_payload,
                               &lock_write_detail)) {
      release_file_handle();
      release_owner_lock();
      return MakeIoError("SB-STORAGE-DISK-OWNER-LOCK-WRITE-FAILED",
                         "storage.disk.owner_lock_write_failed",
                         prospective_owner_lock_path + ":" +
                             lock_write_detail);
    }
    std::string lock_sync_detail;
    if (!DurableSyncHandle(static_cast<HANDLE>(prospective_owner_lock_handle),
                           &lock_sync_detail)) {
      release_file_handle();
      release_owner_lock();
      if (created_new_file) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
      }
      return MakeIoError("SB-STORAGE-DISK-OWNER-LOCK-SYNC-FAILED",
                         "storage.disk.owner_lock_sync_failed",
                         prospective_owner_lock_path + ":" + lock_sync_detail);
    }
#else
    if (::ftruncate(prospective_owner_lock_fd, 0) != 0) {
      release_file_handle();
      release_owner_lock();
      return MakeIoError("SB-STORAGE-DISK-OWNER-LOCK-WRITE-FAILED",
                         "storage.disk.owner_lock_write_failed",
                         prospective_owner_lock_path);
    }
    const std::string lock_payload = "scratchbird.owner.lock.v1\npath=" + path + "\nmode=exclusive\n";
    const ssize_t written = ::write(prospective_owner_lock_fd, lock_payload.data(), lock_payload.size());
    if (written < 0 || static_cast<std::size_t>(written) != lock_payload.size()) {
      release_file_handle();
      release_owner_lock();
      return MakeIoError("SB-STORAGE-DISK-OWNER-LOCK-WRITE-FAILED",
                         "storage.disk.owner_lock_write_failed",
                         prospective_owner_lock_path);
    }
    std::string lock_sync_detail;
    if (!DurableSyncFd(prospective_owner_lock_fd, &lock_sync_detail)) {
      release_file_handle();
      release_owner_lock();
      if (created_new_file) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
      }
      return MakeIoError("SB-STORAGE-DISK-OWNER-LOCK-SYNC-FAILED",
                         "storage.disk.owner_lock_sync_failed",
                         prospective_owner_lock_path + ":" + lock_sync_detail);
    }
#endif
  }

  if (created_new_file) {
    std::string parent_sync_detail;
    if (!DurableSyncParentDirectory(path, &parent_sync_detail)) {
      release_file_handle();
      release_owner_lock();
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      return MakeIoError("SB-STORAGE-DISK-PARENT-SYNC-FAILED",
                         "storage.disk.parent_sync_failed",
                         path + ":" + parent_sync_detail);
    }
  }

  path_ = std::move(path);
  owner_lock_path_ =
      route_owned_by_current_process ? std::string{} : prospective_owner_lock_path;
  route_owner_storage_guard_ = std::move(route_owner_storage_guard);
#ifdef _WIN32
  file_handle_ = prospective_file_handle;
  prospective_file_handle = nullptr;
  owner_lock_handle_ = prospective_owner_lock_handle;
  prospective_owner_lock_handle = nullptr;
#else
  file_fd_ = prospective_file_fd;
  prospective_file_fd = -1;
  owner_lock_fd_ = prospective_owner_lock_fd;
  prospective_owner_lock_fd = -1;
#endif
  read_only_ = read_only_open;
  owner_lock_held_ = !route_owned_by_current_process;
  owner_lock_exclusive_ = !route_owned_by_current_process && !read_only_open;
  capabilities_.write_at = !read_only_;
  if (metric_device_class_.empty()) {
    metric_device_class_ = "file";
  }
  IoResult result;
  result.status = DiskOkStatus();
  return result;
}

IoResult FileDevice::Close() {
  if (!is_open()) {
    return MakeIoError("SB-STORAGE-DISK-NOT-OPEN",
                       "storage.disk.not_open");
  }

  if (!read_only_) {
    std::string sync_detail;
#ifdef _WIN32
    if (!DurableSyncHandle(static_cast<HANDLE>(file_handle_), &sync_detail)) {
#else
    if (!DurableSyncFd(file_fd_, &sync_detail)) {
#endif
      return MakeIoError("SB-STORAGE-DISK-CLOSE-SYNC-FAILED",
                         "storage.disk.close_sync_failed",
                         sync_detail);
    }
  }
#ifdef _WIN32
  std::string close_detail;
  if (!CloseDataFileHandle(file_handle_, &close_detail)) {
    return MakeIoError("SB-STORAGE-DISK-CLOSE-FAILED",
                       "storage.disk.close_failed",
                       close_detail);
  }
  file_handle_ = nullptr;
  if (owner_lock_handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(owner_lock_handle_));
    owner_lock_handle_ = nullptr;
  }
#else
  std::string close_detail;
  if (!CloseDataFileHandle(file_fd_, &close_detail)) {
    return MakeIoError("SB-STORAGE-DISK-CLOSE-FAILED",
                       "storage.disk.close_failed",
                       close_detail);
  }
  file_fd_ = -1;
  if (owner_lock_fd_ >= 0) {
    (void)::flock(owner_lock_fd_, LOCK_UN);
    (void)::close(owner_lock_fd_);
    owner_lock_fd_ = -1;
  }
#endif
  if (owner_lock_held_ && owner_lock_exclusive_ && !owner_lock_path_.empty()) {
    std::error_code ignored;
    std::filesystem::remove(owner_lock_path_, ignored);
  }
  path_.clear();
  owner_lock_path_.clear();
  read_only_ = false;
  owner_lock_held_ = false;
  owner_lock_exclusive_ = false;
  route_owner_storage_guard_ = std::unique_lock<std::recursive_mutex>{};
  capabilities_.write_at = true;

  IoResult result;
  result.status = DiskOkStatus();
  return result;
}

IoResult FileDevice::ReadAt(u64 offset, void* buffer, usize bytes) {
  const auto metric_start = Clock::now();
  if (!is_open()) {
    return MakeIoError("SB-STORAGE-DISK-NOT-OPEN",
                       "storage.disk.not_open");
  }
  if (buffer == nullptr && bytes != 0) {
    return MakeIoError("SB-STORAGE-DISK-READ-BUFFER-NULL",
                       "storage.disk.read_buffer_null");
  }
  const auto checked_extent = CheckFileDeviceExtent(offset, bytes);
  if (!checked_extent.ok()) {
    return MakeIoError(checked_extent.diagnostic.diagnostic_code,
                       checked_extent.diagnostic.message_key,
                       std::to_string(offset) + ":" + std::to_string(bytes));
  }
  if (bytes == 0) {
    IoResult result;
    result.status = DiskOkStatus();
    return result;
  }

  usize transferred = 0;
  std::string read_detail;
#ifdef _WIN32
  const bool read_ok = NativeReadAt(file_handle_,
                                    offset,
                                    buffer,
                                    bytes,
                                    &transferred,
                                    &read_detail);
#else
  const bool read_ok = NativeReadAt(file_fd_,
                                    offset,
                                    buffer,
                                    bytes,
                                    &transferred,
                                    &read_detail);
#endif
  if (!read_ok) {
    return MakeIoError("SB-STORAGE-DISK-READ-SHORT",
                       "storage.disk.read_short",
                       read_detail.empty() ? std::to_string(offset) : read_detail,
                       transferred);
  }

  IoResult result;
  result.status = DiskOkStatus();
  result.bytes_transferred = transferred;
  RecordDiskLatency("sb_storage_device_read_latency_microseconds", ElapsedMicros(metric_start), path_, "read_at", "ok");
  if (!metric_filespace_uuid_.empty()) {
    (void)scratchbird::core::metrics::ObserveFilespaceDeviceReadLatency(
        ElapsedMicros(metric_start),
        metric_database_uuid_,
        metric_filespace_uuid_,
        metric_node_uuid_,
        metric_filespace_role_,
        metric_device_class_.empty() ? "file" : metric_device_class_);
  }
  return result;
}

IoResult FileDevice::WriteAt(u64 offset, const void* buffer, usize bytes) {
  const auto metric_start = Clock::now();
  if (!is_open()) {
    return MakeIoError("SB-STORAGE-DISK-NOT-OPEN",
                       "storage.disk.not_open");
  }
  if (buffer == nullptr && bytes != 0) {
    return MakeIoError("SB-STORAGE-DISK-WRITE-BUFFER-NULL",
                       "storage.disk.write_buffer_null");
  }
  if (read_only_) {
    RecordDiskPolicyViolation("read_only_write_rejected",
                              DiskDevicePolicy{},
                              path_);
    return MakeIoError("SB-STORAGE-DISK-WRITE-READ-ONLY",
                       "storage.disk.write_read_only",
                       std::to_string(offset));
  }
  const auto checked_extent = CheckFileDeviceExtent(offset, bytes);
  if (!checked_extent.ok()) {
    return MakeIoError(checked_extent.diagnostic.diagnostic_code,
                       checked_extent.diagnostic.message_key,
                       std::to_string(offset) + ":" + std::to_string(bytes));
  }
  if (bytes == 0) {
    IoResult result;
    result.status = DiskOkStatus();
    return result;
  }

  usize transferred = 0;
  std::string write_detail;
#ifdef _WIN32
  const bool write_ok = NativeWriteAt(file_handle_,
                                      offset,
                                      buffer,
                                      bytes,
                                      &transferred,
                                      &write_detail);
#else
  const bool write_ok = NativeWriteAt(file_fd_,
                                      offset,
                                      buffer,
                                      bytes,
                                      &transferred,
                                      &write_detail);
#endif
  if (!write_ok || transferred != bytes) {
    return MakeIoError("SB-STORAGE-DISK-WRITE-FAILED",
                       "storage.disk.write_failed",
                       write_detail.empty() ? std::to_string(offset) : write_detail,
                       transferred);
  }

  IoResult result;
  result.status = DiskOkStatus();
  result.bytes_transferred = bytes;
  RecordDiskLatency("sb_storage_device_write_latency_microseconds", ElapsedMicros(metric_start), path_, "write_at", "ok");
  if (!metric_filespace_uuid_.empty()) {
    (void)scratchbird::core::metrics::ObserveFilespaceDeviceWriteLatency(
        ElapsedMicros(metric_start),
        metric_database_uuid_,
        metric_filespace_uuid_,
        metric_node_uuid_,
        metric_filespace_role_,
        metric_device_class_.empty() ? "file" : metric_device_class_);
  }
  return result;
}

IoResult FileDevice::Sync() {
  const auto metric_start = Clock::now();
  if (!is_open()) {
    return MakeIoError("SB-STORAGE-DISK-NOT-OPEN",
                       "storage.disk.not_open");
  }

  if (read_only_) {
    IoResult result;
    result.status = DiskOkStatus();
    RecordDiskLatency("sb_storage_fsync_latency_microseconds", ElapsedMicros(metric_start), path_, "sync", "read_only_noop");
    if (!metric_filespace_uuid_.empty()) {
      (void)scratchbird::core::metrics::ObserveFilespaceFsyncLatency(
          ElapsedMicros(metric_start),
          metric_database_uuid_,
          metric_filespace_uuid_,
          metric_node_uuid_,
          metric_filespace_role_,
          metric_device_class_.empty() ? "file" : metric_device_class_);
    }
    return result;
  }

  std::string sync_detail;
#ifdef _WIN32
  if (!DurableSyncHandle(static_cast<HANDLE>(file_handle_), &sync_detail)) {
#else
  if (!DurableSyncFd(file_fd_, &sync_detail)) {
#endif
    return MakeIoError("SB-STORAGE-DISK-SYNC-FAILED",
                       "storage.disk.sync_failed",
                       sync_detail);
  }

  IoResult result;
  result.status = DiskOkStatus();
  RecordDiskLatency("sb_storage_fsync_latency_microseconds", ElapsedMicros(metric_start), path_, "sync", "ok");
  if (!metric_filespace_uuid_.empty()) {
    (void)scratchbird::core::metrics::ObserveFilespaceFsyncLatency(
        ElapsedMicros(metric_start),
        metric_database_uuid_,
        metric_filespace_uuid_,
        metric_node_uuid_,
        metric_filespace_role_,
        metric_device_class_.empty() ? "file" : metric_device_class_);
  }
  return result;
}

void FileDevice::SetMetricContext(std::string database_uuid,
                                  std::string filespace_uuid,
                                  std::string node_uuid,
                                  std::string filespace_role,
                                  std::string device_class) {
  metric_database_uuid_ = std::move(database_uuid);
  metric_filespace_uuid_ = std::move(filespace_uuid);
  metric_node_uuid_ = std::move(node_uuid);
  metric_filespace_role_ = std::move(filespace_role);
  metric_device_class_ = device_class.empty() ? "file" : std::move(device_class);
}

SizeResult FileDevice::Size() const {
  SizeResult result;
  result.status = DiskOkStatus();

  if (path_.empty()) {
    result.status = DiskErrorStatus();
    result.diagnostic = MakeDiskDiagnostic(result.status,
                                           "SB-STORAGE-DISK-SIZE-NOT-OPEN",
                                           "storage.disk.size_not_open",
                                           path_);
    return result;
  }

  u64 size = 0;
  std::string size_detail;
#ifdef _WIN32
  const bool size_ok = NativeFileSize(file_handle_, &size, &size_detail);
#else
  const bool size_ok = NativeFileSize(file_fd_, &size, &size_detail);
#endif
  if (!size_ok) {
    result.status = DiskErrorStatus();
    result.diagnostic = MakeDiskDiagnostic(result.status,
                                           "SB-STORAGE-DISK-SIZE-FAILED",
                                           "storage.disk.size_failed",
                                           path_,
                                           size_detail);
    return result;
  }

  result.size_bytes = size;
  return result;
}

CapabilityResult FileDevice::Capabilities() const {
  CapabilityResult result;
  result.status = DiskOkStatus();
  result.capabilities = capabilities_;
  return result;
}

bool FileDevice::is_open() const {
#ifdef _WIN32
  return file_handle_ != nullptr;
#else
  return file_fd_ >= 0;
#endif
}

bool FileDevice::read_only() const {
  return read_only_;
}

const std::string& FileDevice::path() const {
  return path_;
}

IoResult FileDevice::MakeIoError(std::string diagnostic_code,
                                 std::string message_key,
                                 std::string detail,
                                 usize bytes_transferred) const {
  RecordDiskError(diagnostic_code.c_str(), path_);
  if (!metric_filespace_uuid_.empty()) {
    (void)scratchbird::core::metrics::RecordFilespaceDeviceError(
        diagnostic_code,
        metric_database_uuid_,
        metric_filespace_uuid_,
        metric_node_uuid_,
        metric_filespace_role_,
        metric_device_class_.empty() ? "file" : metric_device_class_);
  }
  IoResult result;
  result.status = DiskErrorStatus();
  result.bytes_transferred = bytes_transferred;
  result.diagnostic = MakeDiskDiagnostic(result.status,
                                         std::move(diagnostic_code),
                                         std::move(message_key),
                                         path_,
                                         std::move(detail));
  return result;
}

const char* DiskAccessModeName(DiskAccessMode mode) {
  switch (mode) {
    case DiskAccessMode::read_write: return "read_write";
    case DiskAccessMode::read_only: return "read_only";
  }
  return "unknown";
}

const char* DiskFsyncPolicyName(DiskFsyncPolicy policy) {
  switch (policy) {
    case DiskFsyncPolicy::never: return "never";
    case DiskFsyncPolicy::after_mutation: return "after_mutation";
    case DiskFsyncPolicy::always: return "always";
  }
  return "unknown";
}

const char* DiskChecksumPolicyName(DiskChecksumPolicy policy) {
  switch (policy) {
    case DiskChecksumPolicy::accept_declared: return "accept_declared";
    case DiskChecksumPolicy::require_supported: return "require_supported";
    case DiskChecksumPolicy::require_valid: return "require_valid";
  }
  return "unknown";
}

const char* UnknownPagePolicyName(UnknownPagePolicy policy) {
  switch (policy) {
    case UnknownPagePolicy::reject_all: return "reject_all";
    case UnknownPagePolicy::allow_unknown_safe_read_only: return "allow_unknown_safe_read_only";
    case UnknownPagePolicy::reject_cluster_pages_until_mapping_available:
      return "reject_cluster_pages_until_mapping_available";
    case UnknownPagePolicy::allow_encrypted_opaque_read_only: return "allow_encrypted_opaque_read_only";
  }
  return "unknown";
}

CheckedFileExtentResult CheckFileDeviceExtent(u64 offset, usize bytes) {
  const auto max_streamoff = static_cast<u64>(std::numeric_limits<std::streamoff>::max());
  const auto max_streamsize = static_cast<u64>(std::numeric_limits<std::streamsize>::max());
  if (offset > max_streamoff) {
    return DiskCheckedExtentError("SB-STORAGE-DISK-OFFSET-CONVERSION-OVERFLOW",
                                  "storage.disk.offset_conversion_overflow",
                                  std::to_string(offset));
  }
  if (static_cast<u64>(bytes) > max_streamsize) {
    return DiskCheckedExtentError("SB-STORAGE-DISK-BYTE-COUNT-CONVERSION-OVERFLOW",
                                  "storage.disk.byte_count_conversion_overflow",
                                  std::to_string(bytes));
  }
  if (AddWouldOverflow(offset, static_cast<u64>(bytes)) ||
      offset + static_cast<u64>(bytes) > max_streamoff) {
    return DiskCheckedExtentError("SB-STORAGE-DISK-EXTENT-OVERFLOW",
                                  "storage.disk.extent_overflow",
                                  std::to_string(offset) + ":" + std::to_string(bytes));
  }

  CheckedFileExtentResult result;
  result.status = DiskOkStatus();
  result.offset = offset;
  result.bytes = bytes;
  return result;
}

CheckedFileExtentResult CheckDevicePageOffset(u32 page_size,
                                              u64 page_number,
                                              u64 in_page_offset) {
  if (!IsSupportedDatabasePageSize(page_size)) {
    return DiskCheckedExtentError("SB-STORAGE-DISK-PAGE-SIZE-INVALID",
                                  "storage.disk.page_size_invalid",
                                  std::to_string(page_size));
  }
  if (in_page_offset >= page_size) {
    return DiskCheckedExtentError("SB-STORAGE-DISK-PAGE-IN-PAGE-OFFSET-INVALID",
                                  "storage.disk.page_in_page_offset_invalid",
                                  std::to_string(in_page_offset));
  }
  const u64 page_size_u64 = static_cast<u64>(page_size);
  if (MulWouldOverflow(page_size_u64, page_number)) {
    return DiskCheckedExtentError("SB-STORAGE-DISK-PAGE-OFFSET-OVERFLOW",
                                  "storage.disk.page_offset_overflow",
                                  std::to_string(page_size) + ":" + std::to_string(page_number));
  }
  const u64 base_offset = page_size_u64 * page_number;
  if (AddWouldOverflow(base_offset, in_page_offset)) {
    return DiskCheckedExtentError("SB-STORAGE-DISK-PAGE-OFFSET-OVERFLOW",
                                  "storage.disk.page_offset_overflow",
                                  std::to_string(page_size) + ":" + std::to_string(page_number) +
                                      ":" + std::to_string(in_page_offset));
  }
  return CheckFileDeviceExtent(base_offset + in_page_offset, 0);
}

DiskHealthResult CheckDiskDeviceHealth(const FileDevice& device,
                                       const DiskDevicePolicy& policy) {
  DiskHealthSnapshot snapshot;
  snapshot.opened = device.is_open();
  snapshot.read_only = device.read_only();
  snapshot.page_size = policy.page_size;
  snapshot.file_present = !device.path().empty() && std::filesystem::exists(device.path());

  if (policy.require_open_device && !snapshot.opened) {
    return DiskHealthError("SB-STORAGE-DISK-HEALTH-NOT-OPEN",
                           "storage.disk.health_not_open",
                           device,
                           policy,
                           snapshot);
  }
  if (!snapshot.file_present) {
    return DiskHealthError("SB-STORAGE-DISK-HEALTH-FILE-MISSING",
                           "storage.disk.health_file_missing",
                           device,
                           policy,
                           snapshot);
  }

  const auto capabilities = device.Capabilities();
  if (!capabilities.ok()) {
    return DiskHealthError("SB-STORAGE-DISK-HEALTH-CAPABILITY-FAILED",
                           "storage.disk.health_capability_failed",
                           device,
                           policy,
                           snapshot,
                           capabilities.diagnostic.diagnostic_code);
  }
  snapshot.can_read = capabilities.capabilities.read_at;
  snapshot.can_write = capabilities.capabilities.write_at;
  snapshot.can_sync = capabilities.capabilities.sync;

  const auto size = device.Size();
  snapshot.size_query_ok = size.ok();
  if (!size.ok()) {
    return DiskHealthError("SB-STORAGE-DISK-HEALTH-SIZE-FAILED",
                           "storage.disk.health_size_failed",
                           device,
                           policy,
                           snapshot,
                           size.diagnostic.diagnostic_code);
  }
  snapshot.size_bytes = size.size_bytes;
  snapshot.size_aligned = policy.page_size == 0 || (snapshot.size_bytes % policy.page_size) == 0;
  if (policy.require_size_alignment && !snapshot.size_aligned) {
    RecordDiskPolicyViolation("size_alignment_failed", policy, device.path());
    return DiskHealthError("SB-STORAGE-DISK-HEALTH-SIZE-UNALIGNED",
                           "storage.disk.health_size_unaligned",
                           device,
                           policy,
                           snapshot,
                           std::to_string(snapshot.size_bytes));
  }
  if (!snapshot.can_read) {
    RecordDiskPolicyViolation("read_not_supported", policy, device.path());
    return DiskHealthError("SB-STORAGE-DISK-HEALTH-READ-UNAVAILABLE",
                           "storage.disk.health_read_unavailable",
                           device,
                           policy,
                           snapshot);
  }
  if (policy.access_mode == DiskAccessMode::read_write && !snapshot.can_write) {
    RecordDiskPolicyViolation("write_not_supported", policy, device.path());
    return DiskHealthError("SB-STORAGE-DISK-HEALTH-WRITE-UNAVAILABLE",
                           "storage.disk.health_write_unavailable",
                           device,
                           policy,
                           snapshot);
  }
  if (policy.fsync_policy != DiskFsyncPolicy::never && !snapshot.can_sync) {
    RecordDiskPolicyViolation("sync_not_supported", policy, device.path());
    return DiskHealthError("SB-STORAGE-DISK-HEALTH-SYNC-UNAVAILABLE",
                           "storage.disk.health_sync_unavailable",
                           device,
                           policy,
                           snapshot);
  }

  RecordDiskHealth("ok", policy, device.path());
  DiskHealthResult result;
  result.status = DiskOkStatus();
  snapshot.health = "ok";
  result.snapshot = std::move(snapshot);
  return result;
}

IoResult SyncFileDeviceWithPolicy(FileDevice* device,
                                  const DiskDevicePolicy& policy) {
  if (device == nullptr) {
    RecordDiskPolicyViolation("sync_null_device", policy, {});
    return DiskPolicyIoError("SB-STORAGE-DISK-SYNC-DEVICE-NULL",
                             "storage.disk.sync_device_null",
                             {});
  }
  if (policy.fsync_policy == DiskFsyncPolicy::never) {
    IoResult result;
    result.status = DiskOkStatus();
    return result;
  }
  return device->Sync();
}

IoResult SyncFilesystemPath(const std::string& path, bool writable) {
  std::string detail;
  if (DurableSyncPath(path, writable, &detail)) {
    IoResult result;
    result.status = DiskOkStatus();
    return result;
  }
  return DiskPolicyIoError("SB-STORAGE-DISK-PATH-SYNC-FAILED",
                           "storage.disk.path_sync_failed",
                           path,
                           detail);
}

IoResult SyncParentDirectoryPath(const std::string& path) {
  std::string detail;
  if (DurableSyncParentDirectory(path, &detail)) {
    IoResult result;
    result.status = DiskOkStatus();
    return result;
  }
  return DiskPolicyIoError("SB-STORAGE-DISK-PARENT-SYNC-FAILED",
                           "storage.disk.parent_sync_failed",
                           path,
                           detail);
}

DiskPageHeaderResult ReadDevicePageHeader(FileDevice* device,
                                          u32 page_size,
                                          u64 page_number,
                                          const DiskDevicePolicy& policy) {
  if (device == nullptr) {
    PageClassification classification;
    classification.kind = PageClassificationKind::invalid_header;
    return DiskPageHeaderError("SB-STORAGE-DISK-PAGE-DEVICE-NULL",
                               "storage.disk.page_device_null",
                               device,
                               policy,
                               classification);
  }
  if (page_size < kPageHeaderSerializedBytes) {
    PageClassification classification;
    classification.kind = PageClassificationKind::invalid_header;
    return DiskPageHeaderError("SB-STORAGE-DISK-PAGE-SIZE-TOO-SMALL",
                               "storage.disk.page_size_too_small",
                               device,
                               policy,
                               classification,
                               std::to_string(page_size));
  }

  SerializedPageHeader serialized{};
  const auto checked_offset = CheckDevicePageOffset(page_size, page_number);
  if (!checked_offset.ok()) {
    DiskPageHeaderResult result;
    result.status = checked_offset.status;
    result.diagnostic = checked_offset.diagnostic;
    return result;
  }
  const auto checked_extent = CheckFileDeviceExtent(checked_offset.offset, serialized.size());
  if (!checked_extent.ok()) {
    DiskPageHeaderResult result;
    result.status = checked_extent.status;
    result.diagnostic = checked_extent.diagnostic;
    return result;
  }
  const auto read = device->ReadAt(checked_offset.offset,
                                  serialized.data(),
                                  serialized.size());
  if (!read.ok()) {
    DiskPageHeaderResult result;
    result.status = read.status;
    result.diagnostic = read.diagnostic;
    return result;
  }

  PageClassification classification = ClassifyPageHeader(serialized);
  if (!PolicyAllowsClassification(classification.kind, policy)) {
    return DiskPageHeaderError("SB-STORAGE-DISK-PAGE-POLICY-REJECTED",
                               "storage.disk.page_policy_rejected",
                               device,
                               policy,
                               classification,
                               PageClassificationKindName(classification.kind));
  }
  if (policy.checksum_policy != DiskChecksumPolicy::accept_declared &&
      classification.kind == PageClassificationKind::checksum_mismatch) {
    return DiskPageHeaderError("SB-STORAGE-DISK-PAGE-CHECKSUM-REJECTED",
                               "storage.disk.page_checksum_rejected",
                               device,
                               policy,
                               classification);
  }

  DiskPageHeaderResult result;
  result.status = DiskOkStatus();
  result.serialized = serialized;
  result.classification = std::move(classification);
  return result;
}

DiagnosticRecord MakeDiskDiagnostic(Status status,
                                    std::string diagnostic_code,
                                    std::string message_key,
                                    std::string path,
                                    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  arguments.push_back({"path", path});
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.disk");
}

}  // namespace scratchbird::storage::disk
