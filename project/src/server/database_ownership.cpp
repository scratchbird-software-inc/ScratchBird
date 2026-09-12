// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_DATABASE_OWNERSHIP_LOCK
// DATABASE_OWNERSHIP: server route ownership is a hard fail-closed token.

#include "database_ownership.hpp"
#include "../storage/disk/route_ownership_lease.hpp"

#include <chrono>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

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

namespace scratchbird::server {

namespace {

std::uint64_t CurrentProcessId() {
#ifdef _WIN32
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

std::uint64_t CurrentUnixMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
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
#else
int FileOpenCloexecFlag() {
#ifdef O_CLOEXEC
  return O_CLOEXEC;
#else
  return 0;
#endif
}
#endif

std::string Trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

void AssignDescriptorField(DatabaseOwnershipDescriptor* descriptor,
                           std::string key,
                           std::string value) {
  if (descriptor == nullptr) return;
  key = Trim(std::move(key));
  value = Trim(std::move(value));
  if (key == "format") descriptor->format = std::move(value);
  else if (key == "owner_kind") descriptor->owner_kind = std::move(value);
  else if (key == "database_path") descriptor->database_path = std::move(value);
  else if (key == "sbps_endpoint") descriptor->sbps_endpoint = std::move(value);
  else if (key == "pid") descriptor->pid = std::move(value);
  else if (key == "created_unix_ms") descriptor->created_unix_ms = std::move(value);
}

std::string DescriptorText(const DatabaseOwnershipRequest& request) {
  std::ostringstream out;
  out << "format=SB_DATABASE_OWNERSHIP_V1\n";
  out << "owner_kind=" << (request.owner_kind.empty() ? "server" : request.owner_kind) << "\n";
  out << "database_path=" << request.database_path.string() << "\n";
  out << "sbps_endpoint=" << request.sbps_endpoint.string() << "\n";
#ifndef _WIN32
  out << "pid=" << static_cast<unsigned long long>(::getpid()) << "\n";
#else
  out << "pid=" << static_cast<unsigned long long>(::GetCurrentProcessId()) << "\n";
#endif
  out << "created_unix_ms=" << CurrentUnixMillis() << "\n";
  return out.str();
}

}  // namespace

DatabaseOwnershipLock::DatabaseOwnershipLock(NativeHandle handle,
                                             NativeHandle storage_handle,
                                             std::filesystem::path lock_path)
    : handle_(handle), storage_handle_(storage_handle),
      owner_pid_(CurrentProcessId()), lock_path_(std::move(lock_path)) {}

DatabaseOwnershipLock::~DatabaseOwnershipLock() {
  release();
}

DatabaseOwnershipLock::DatabaseOwnershipLock(DatabaseOwnershipLock&& other) noexcept
    : lease_(std::move(other.lease_)),
      handle_(other.handle_), storage_handle_(other.storage_handle_),
      owner_pid_(other.owner_pid_), lock_path_(std::move(other.lock_path_)) {
#ifdef _WIN32
  other.handle_ = nullptr;
  other.storage_handle_ = nullptr;
#else
  other.handle_ = -1;
  other.storage_handle_ = -1;
#endif
  other.owner_pid_ = 0;
}

DatabaseOwnershipLock& DatabaseOwnershipLock::operator=(DatabaseOwnershipLock&& other) noexcept {
  if (this != &other) {
    release();
    lease_ = std::move(other.lease_);
    handle_ = other.handle_;
    storage_handle_ = other.storage_handle_;
    owner_pid_ = other.owner_pid_;
    lock_path_ = std::move(other.lock_path_);
#ifdef _WIN32
    other.handle_ = nullptr;
    other.storage_handle_ = nullptr;
#else
    other.handle_ = -1;
    other.storage_handle_ = -1;
#endif
    other.owner_pid_ = 0;
  }
  return *this;
}

#ifdef _WIN32
std::uint32_t DatabaseOwnershipLock::PrepareStorageLease(const std::string& database_path) {
#else
int DatabaseOwnershipLock::PrepareStorageLease(const std::string& database_path) {
#endif
  // Allocate before transferring either handle. Existing primary files are
  // locked by their opened inode before any descriptor publication.
  auto lease = std::shared_ptr<storage::disk::RouteOwnershipLease>(
      new storage::disk::RouteOwnershipLease(lock_path_.string(), owner_pid_));
#ifdef _WIN32
  lease->handle_ = std::exchange(handle_, nullptr);
  lease->storage_handle_ = std::exchange(storage_handle_, nullptr);
#else
  lease->handle_ = std::exchange(handle_, -1);
  lease->storage_handle_ = std::exchange(storage_handle_, -1);
#endif
  lease_ = std::move(lease);
  return lease_->AcquireDataFile(database_path);
}

bool DatabaseOwnershipLock::PublishStorageLease() {
  // Publication still happens only after the checked durable descriptor write.
  return lease_ && lease_->Publish(lease_);
}

DatabaseOwnershipLock::NativeHandle DatabaseOwnershipLock::native_handle() const {
  return lease_ ? lease_->handle_ : handle_;
}

bool DatabaseOwnershipLock::valid() const {
  if (lease_) return lease_->valid();
  if (owner_pid_ != CurrentProcessId()) return false;
#ifdef _WIN32
  return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE &&
         storage_handle_ != nullptr && storage_handle_ != INVALID_HANDLE_VALUE;
#else
  return handle_ >= 0 && storage_handle_ >= 0;
#endif
}

void DatabaseOwnershipLock::release() {
  if (lease_) {
    lease_->Withdraw();
    lease_.reset();  // admitted FileDevices retain the OS locks until final close
  }
#ifdef _WIN32
  if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
    ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
  if (storage_handle_ != nullptr && storage_handle_ != INVALID_HANDLE_VALUE) {
    ::CloseHandle(static_cast<HANDLE>(storage_handle_));
    storage_handle_ = nullptr;
  }
#else
  // A forked child must not unlock the parent's shared open-file description.
  // Closing the inherited fd is safe; flock(LOCK_UN) would release its lock.
  const bool owning_process = owner_pid_ == CurrentProcessId();
  if (handle_ >= 0) {
    if (owning_process) (void)::flock(handle_, LOCK_UN);
    (void)::close(handle_);
    handle_ = -1;
  }
  if (storage_handle_ >= 0) {
    if (owning_process) (void)::flock(storage_handle_, LOCK_UN);
    (void)::close(storage_handle_);
    storage_handle_ = -1;
  }
#endif
  owner_pid_ = 0;
}

std::filesystem::path DatabaseOwnershipLockPath(const std::filesystem::path& database_path) {
  std::filesystem::path out = database_path;
  // Routing metadata has a separate file, but a server must also retain
  // <database>.sb.owner.lock for the entire lifetime of this route owner.
  // The descriptor is discovery data, not a substitute for storage exclusion.
  out += ".sb.route.owner.lock";
  return out;
}

DatabaseOwnershipDescriptor ReadDatabaseOwnershipDescriptor(
    const std::filesystem::path& lock_path) {
  DatabaseOwnershipDescriptor descriptor;
  std::ifstream in(lock_path);
  std::string line;
  while (std::getline(in, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    AssignDescriptorField(&descriptor, line.substr(0, eq), line.substr(eq + 1));
  }
  return descriptor;
}

DatabaseOwnershipResult AcquireDatabaseOwnership(const DatabaseOwnershipRequest& request) {
  DatabaseOwnershipResult result;
  result.lock_path = DatabaseOwnershipLockPath(request.database_path);
  if (request.database_path.empty()) {
    result.diagnostic_code = "ARCH.DATABASE_OWNERSHIP_PATH_REQUIRED";
    result.diagnostic_detail = "database_path_required";
    return result;
  }

  std::error_code ec;
  std::filesystem::create_directories(result.lock_path.parent_path(), ec);
  if (ec) {
    result.diagnostic_code = "ARCH.DATABASE_OWNERSHIP_LOCK_FAILED";
    result.diagnostic_detail = "lock_directory_create_failed";
    return result;
  }

  // Allocate path storage before native acquisition so an allocation failure
  // cannot strand either lock before the RAII owner has been constructed.
  const auto storage_lock_path = request.database_path.string() + ".sb.owner.lock";
  auto retained_lock_path = result.lock_path;
#ifdef _WIN32
  HANDLE handle = ::CreateFileA(result.lock_path.string().c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ,
                                nullptr,
                                OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    if (WindowsSharingConflict(error)) {
      result.incumbent = ReadDatabaseOwnershipDescriptor(result.lock_path);
      result.diagnostic_code = "ARCH.DATABASE_MULTI_OWNER";
      result.diagnostic_detail = "database_owned_by_running_engine";
    } else {
      result.diagnostic_code = "ARCH.DATABASE_OWNERSHIP_LOCK_FAILED";
      result.diagnostic_detail = std::string("open_failed:") + WindowsLastErrorText(error);
    }
    return result;
  }

  HANDLE storage_handle = ::CreateFileA(storage_lock_path.c_str(),
                                        GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (storage_handle == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    ::CloseHandle(handle);
    result.diagnostic_code = WindowsSharingConflict(error)
        ? "ARCH.DATABASE_MULTI_OWNER" : "ARCH.DATABASE_OWNERSHIP_LOCK_FAILED";
    result.diagnostic_detail = std::string("storage_owner_acquire_failed:") +
                               WindowsLastErrorText(error);
    return result;
  }
  DatabaseOwnershipLock ownership(handle, storage_handle, std::move(retained_lock_path));
  const DWORD data_owner_error = ownership.PrepareStorageLease(request.database_path.string());
  if (data_owner_error != ERROR_SUCCESS) {
    result.diagnostic_code = WindowsSharingConflict(data_owner_error)
        ? "ARCH.DATABASE_MULTI_OWNER" : "ARCH.DATABASE_OWNERSHIP_LOCK_FAILED";
    result.diagnostic_detail = std::string("data_owner_acquire_failed:") +
                               WindowsLastErrorText(data_owner_error);
    return result;
  }
  // No descriptor is published until the actual primary is owned as well.
  const std::string text = DescriptorText(request);
  LARGE_INTEGER zero{};
  if (::SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN) == 0 ||
      ::SetEndOfFile(handle) == 0) {
    const DWORD error = ::GetLastError();
    result.diagnostic_code = "ARCH.DATABASE_OWNERSHIP_LOCK_FAILED";
    result.diagnostic_detail = std::string("descriptor_truncate_failed:") +
                               WindowsLastErrorText(error);
    return result;
  }

  DWORD written = 0;
  if (::WriteFile(handle,
                  text.data(),
                  static_cast<DWORD>(text.size()),
                  &written,
                  nullptr) == 0 ||
      written != text.size()) {
    const DWORD error = ::GetLastError();
    result.diagnostic_code = "ARCH.DATABASE_OWNERSHIP_LOCK_FAILED";
    result.diagnostic_detail = std::string("descriptor_write_failed:") +
                               WindowsLastErrorText(error);
    return result;
  }

  if (::FlushFileBuffers(handle) == 0) {
    const DWORD error = ::GetLastError();
    result.diagnostic_code = "ARCH.DATABASE_OWNERSHIP_LOCK_FAILED";
    result.diagnostic_detail = std::string("descriptor_sync_failed:") +
                               WindowsLastErrorText(error);
    return result;
  }
  if (!ownership.PublishStorageLease()) {
    result.diagnostic_code = "ARCH.DATABASE_OWNERSHIP_LOCK_FAILED";
    result.diagnostic_detail = "process_lease_publication_failed";
    return result;
  }
  result.lock = std::make_shared<DatabaseOwnershipLock>(std::move(ownership));
  result.acquired = true;
  return result;
#else
  const int fd = ::open(result.lock_path.c_str(),
                        O_RDWR | O_CREAT | FileOpenCloexecFlag(),
                        S_IRUSR | S_IWUSR);
  if (fd < 0) {
    result.diagnostic_code = "ARCH.DATABASE_OWNERSHIP_LOCK_FAILED";
    result.diagnostic_detail = std::string("open_failed:") + std::strerror(errno);
    return result;
  }

  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int saved_errno = errno;
    (void)::close(fd);
    result.incumbent = ReadDatabaseOwnershipDescriptor(result.lock_path);
    result.diagnostic_code = "ARCH.DATABASE_MULTI_OWNER";
    result.diagnostic_detail =
        (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN)
            ? "database_owned_by_running_engine"
            : std::string("lock_failed:") + std::strerror(saved_errno);
    return result;
  }

  const int storage_fd = ::open(storage_lock_path.c_str(),
                                O_RDWR | O_CREAT | FileOpenCloexecFlag(),
                                S_IRUSR | S_IWUSR);
  if (storage_fd < 0) {
    const int saved_errno = errno;
    (void)::flock(fd, LOCK_UN);
    (void)::close(fd);
    result.diagnostic_code = "ARCH.DATABASE_OWNERSHIP_LOCK_FAILED";
    result.diagnostic_detail = std::string("storage_owner_open_failed:") +
                               std::strerror(saved_errno);
    return result;
  }
  if (::flock(storage_fd, LOCK_EX | LOCK_NB) != 0) {
    const int saved_errno = errno;
    (void)::close(storage_fd);
    (void)::flock(fd, LOCK_UN);
    (void)::close(fd);
    result.diagnostic_code = (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN)
        ? "ARCH.DATABASE_MULTI_OWNER" : "ARCH.DATABASE_OWNERSHIP_LOCK_FAILED";
    result.diagnostic_detail = std::string("storage_owner_lock_failed:") +
                               std::strerror(saved_errno);
    return result;
  }
  DatabaseOwnershipLock ownership(fd, storage_fd, std::move(retained_lock_path));
  const int data_owner_error = ownership.PrepareStorageLease(request.database_path.string());
  if (data_owner_error != 0) {
    result.diagnostic_code = (data_owner_error == EWOULDBLOCK || data_owner_error == EAGAIN)
        ? "ARCH.DATABASE_MULTI_OWNER" : "ARCH.DATABASE_OWNERSHIP_LOCK_FAILED";
    result.diagnostic_detail = std::string("data_owner_acquire_failed:") +
                               std::strerror(data_owner_error);
    return result;
  }
  // Do not truncate or publish metadata while another process owns storage.
  const std::string text = DescriptorText(request);
  if (::ftruncate(fd, 0) != 0 || ::lseek(fd, 0, SEEK_SET) < 0) {
    const int saved_errno = errno;
    result.diagnostic_code = "ARCH.DATABASE_OWNERSHIP_LOCK_FAILED";
    result.diagnostic_detail = std::string("descriptor_truncate_failed:") +
                               std::strerror(saved_errno);
    return result;
  }
  const char* data = text.data();
  std::size_t remaining = text.size();
  while (remaining > 0) {
    const ssize_t wrote = ::write(fd, data, remaining);
    if (wrote <= 0) {
      if (wrote < 0 && errno == EINTR) continue;
      const int saved_errno = wrote == 0 ? EIO : errno;
      result.diagnostic_code = "ARCH.DATABASE_OWNERSHIP_LOCK_FAILED";
      result.diagnostic_detail = std::string("descriptor_write_failed:") +
                                 std::strerror(saved_errno);
      return result;
    }
    data += wrote;
    remaining -= static_cast<std::size_t>(wrote);
  }
  int synced;
  do { synced = ::fsync(fd); } while (synced != 0 && errno == EINTR);
  if (synced != 0) {
    const int saved_errno = errno;
    result.diagnostic_code = "ARCH.DATABASE_OWNERSHIP_LOCK_FAILED";
    result.diagnostic_detail = std::string("descriptor_sync_failed:") +
                               std::strerror(saved_errno);
    return result;
  }
  if (!ownership.PublishStorageLease()) {
    result.diagnostic_code = "ARCH.DATABASE_OWNERSHIP_LOCK_FAILED";
    result.diagnostic_detail = "process_lease_publication_failed";
    return result;
  }
  result.lock = std::make_shared<DatabaseOwnershipLock>(std::move(ownership));
  result.acquired = true;
  return result;
#endif
}

}  // namespace scratchbird::server
