// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "listener_socket_identity.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
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

#include "listener_diagnostics.hpp"

namespace scratchbird::listener {
namespace {

std::string HexBytes(const proto::Sha256Digest& digest, std::size_t bytes) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes * 2);
  for (std::size_t i = 0; i < bytes && i < digest.size(); ++i) {
    out.push_back(hex[(digest[i] >> 4u) & 0x0fu]);
    out.push_back(hex[digest[i] & 0x0fu]);
  }
  return out;
}

std::string StableHash(std::string_view value) {
  proto::Bytes bytes(value.begin(), value.end());
  return HexBytes(proto::Sha256(bytes), 16);
}

std::string Sanitize(std::string value) {
  for (char& c : value) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) c = '_';
  }
  return value;
}

std::filesystem::path ArtifactLockPath(const std::filesystem::path& path) {
  return std::filesystem::path(path.string() + ".lock");
}

std::string ArtifactTempPrefix(const std::filesystem::path& path) {
  return path.filename().string() + ".tmp.";
}

bool PrepareArtifactDirectory(const std::filesystem::path& path, std::string* error_message) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    if (error_message) *error_message = ec.message();
    return false;
  }
  std::filesystem::permissions(path.parent_path(),
                               std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace,
                               ec);
  if (ec) {
    if (error_message) *error_message = ec.message();
    return false;
  }
  return true;
}

class ScopedArtifactLock {
 public:
  ScopedArtifactLock() = default;
  ScopedArtifactLock(const ScopedArtifactLock&) = delete;
  ScopedArtifactLock& operator=(const ScopedArtifactLock&) = delete;

  ScopedArtifactLock(ScopedArtifactLock&& other) noexcept : handle_(other.handle_) {
    other.handle_ = InvalidHandle();
  }

  ScopedArtifactLock& operator=(ScopedArtifactLock&& other) noexcept {
    if (this == &other) return *this;
    Close();
    handle_ = other.handle_;
    other.handle_ = InvalidHandle();
    return *this;
  }

  ~ScopedArtifactLock() {
    Close();
  }

  bool held() const {
    return handle_ != InvalidHandle();
  }

  static ScopedArtifactLock Acquire(const std::filesystem::path& path, std::string* error_message) {
    ScopedArtifactLock lock;
    if (!PrepareArtifactDirectory(path, error_message)) return lock;
    const auto lock_path = ArtifactLockPath(path);
#ifndef _WIN32
    const int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
      if (error_message) *error_message = std::string("artifact lock open failed: ") + std::strerror(errno);
      return lock;
    }
    if (::fchmod(fd, 0600) != 0) {
      const int saved = errno;
      ::close(fd);
      if (error_message) *error_message = std::string("artifact lock chmod failed: ") + std::strerror(saved);
      return lock;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      const int saved = errno;
      ::close(fd);
      if (error_message) {
        *error_message = (saved == EWOULDBLOCK || saved == EAGAIN)
                             ? "artifact lock busy"
                             : std::string("artifact lock failed: ") + std::strerror(saved);
      }
      return lock;
    }
    lock.handle_ = fd;
#else
    HANDLE handle = ::CreateFileA(lock_path.string().c_str(),
                                  GENERIC_READ | GENERIC_WRITE,
                                  0,
                                  nullptr,
                                  OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      if (error_message) {
        *error_message = "artifact lock open failed: Windows error " +
                         std::to_string(::GetLastError());
      }
      return lock;
    }
    OVERLAPPED overlapped{};
    if (!::LockFileEx(handle,
                      LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                      0,
                      1,
                      0,
                      &overlapped)) {
      const DWORD saved = ::GetLastError();
      ::CloseHandle(handle);
      if (error_message) {
        *error_message = saved == ERROR_LOCK_VIOLATION
                             ? "artifact lock busy"
                             : "artifact lock failed: Windows error " +
                                   std::to_string(saved);
      }
      return lock;
    }
    lock.handle_ = handle;
#endif
    return lock;
  }

 private:
#ifdef _WIN32
  static HANDLE InvalidHandle() {
    return INVALID_HANDLE_VALUE;
  }
#else
  static int InvalidHandle() {
    return -1;
  }
#endif

  void Close() {
#ifndef _WIN32
    if (handle_ >= 0) {
      ::flock(handle_, LOCK_UN);
      ::close(handle_);
      handle_ = InvalidHandle();
    }
#else
    if (handle_ != INVALID_HANDLE_VALUE) {
      OVERLAPPED overlapped{};
      (void)::UnlockFileEx(handle_, 0, 1, 0, &overlapped);
      ::CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
#endif
  }

#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int handle_ = -1;
#endif
};

void RemoveStaleArtifactTemps(const std::filesystem::path& path) {
  std::error_code ec;
  const auto prefix = ArtifactTempPrefix(path);
  for (const auto& entry : std::filesystem::directory_iterator(path.parent_path(), ec)) {
    if (ec) return;
    const auto name = entry.path().filename().string();
    if (name.rfind(prefix, 0) == 0) {
      std::filesystem::remove(entry.path(), ec);
      ec.clear();
    }
  }
}

std::map<std::string, std::string> ParseKeyValueBody(const std::string& body) {
  std::map<std::string, std::string> fields;
  std::istringstream in(body);
  std::string line;
  while (std::getline(in, line)) {
    const auto pos = line.find('=');
    if (pos == std::string::npos) continue;
    fields[line.substr(0, pos)] = line.substr(pos + 1);
  }
  return fields;
}

bool ReadSignedArtifact(const std::filesystem::path& path,
                        std::string* body,
                        std::string* signature) {
  if (body == nullptr || signature == nullptr) return false;
  std::ifstream in(path);
  if (!in) return false;
  std::ostringstream signed_body;
  std::string line;
  bool found_signature = false;
  while (std::getline(in, line)) {
    constexpr std::string_view prefix = "signature_sha256_128=";
    if (line.rfind(prefix, 0) == 0) {
      *signature = line.substr(prefix.size());
      found_signature = true;
      continue;
    }
    signed_body << line << '\n';
  }
  *body = signed_body.str();
  return found_signature && !signature->empty();
}

bool SignedArtifactValid(const std::filesystem::path& path,
                         std::string* body,
                         std::string* diagnostic) {
  std::string signature;
  if (!ReadSignedArtifact(path, body, &signature)) {
    if (diagnostic != nullptr) *diagnostic = "signature_missing";
    return false;
  }
  const auto expected = StableHash(*body);
  if (signature != expected) {
    if (diagnostic != nullptr) *diagnostic = "signature_mismatch";
    return false;
  }
  return true;
}

bool FsyncParentDirectory(const std::filesystem::path& path, std::string* error_message) {
#ifndef _WIN32
  const int dir_fd = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir_fd < 0) {
    if (error_message) *error_message = std::string("open parent directory failed: ") + std::strerror(errno);
    return false;
  }
  const bool ok = ::fsync(dir_fd) == 0;
  const int saved = errno;
  ::close(dir_fd);
  if (!ok) {
    if (error_message) *error_message = std::string("parent directory fsync failed: ") + std::strerror(saved);
    return false;
  }
#else
  HANDLE dir = ::CreateFileA(path.parent_path().string().c_str(),
                             FILE_LIST_DIRECTORY,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr,
                             OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS,
                             nullptr);
  if (dir == INVALID_HANDLE_VALUE) {
    if (error_message) {
      *error_message = "open parent directory failed: Windows error " +
                       std::to_string(::GetLastError());
    }
    return false;
  }
  const BOOL ok = ::FlushFileBuffers(dir);
  const DWORD saved = ::GetLastError();
  ::CloseHandle(dir);
  if (!ok && saved != ERROR_ACCESS_DENIED) {
    if (error_message) {
      *error_message = "parent directory flush failed: Windows error " +
                       std::to_string(saved);
    }
    return false;
  }
#endif
  return true;
}

bool WriteSignedArtifactDurably(const std::filesystem::path& path,
                                const std::string& body,
                                std::string* error_message) {
  if (!PrepareArtifactDirectory(path, error_message)) return false;
  RemoveStaleArtifactTemps(path);
  const std::string signed_body = body + "signature_sha256_128=" + StableHash(body) + '\n';
  const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
#ifndef _WIN32
  const auto process_id = static_cast<unsigned long long>(::getpid());
#else
  const auto process_id = static_cast<unsigned long long>(::GetCurrentProcessId());
#endif
  const auto tmp = path.string() + ".tmp." +
                   std::to_string(process_id) + "." +
                   std::to_string(static_cast<unsigned long long>(now_ns));
#ifndef _WIN32
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    if (error_message) *error_message = std::string("could not create temp file: ") + std::strerror(errno);
    return false;
  }
  std::size_t written = 0;
  while (written < signed_body.size()) {
    const ssize_t rc = ::write(fd, signed_body.data() + written, signed_body.size() - written);
    if (rc > 0) {
      written += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    const int saved = errno;
    ::close(fd);
    ::unlink(tmp.c_str());
    if (error_message) *error_message = std::string("temp file write failed: ") + std::strerror(saved);
    return false;
  }
  if (::fchmod(fd, 0600) != 0 || ::fsync(fd) != 0 || ::close(fd) != 0) {
    const int saved = errno;
    ::unlink(tmp.c_str());
    if (error_message) *error_message = std::string("temp file durable close failed: ") + std::strerror(saved);
    return false;
  }
#else
  HANDLE file = ::CreateFileA(tmp.c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (error_message) {
      *error_message = "could not create temp file: Windows error " +
                       std::to_string(::GetLastError());
    }
    return false;
  }
  std::size_t written = 0;
  while (written < signed_body.size()) {
    DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
        signed_body.size() - written,
        static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
    DWORD wrote = 0;
    if (!::WriteFile(file, signed_body.data() + written, chunk, &wrote, nullptr) ||
        wrote == 0) {
      const DWORD saved = ::GetLastError();
      ::CloseHandle(file);
      (void)::DeleteFileA(tmp.c_str());
      if (error_message) {
        *error_message = "temp file write failed: Windows error " +
                         std::to_string(saved);
      }
      return false;
    }
    written += static_cast<std::size_t>(wrote);
  }
  const BOOL flushed = ::FlushFileBuffers(file);
  const DWORD flush_error = ::GetLastError();
  const BOOL closed = ::CloseHandle(file);
  const DWORD close_error = ::GetLastError();
  if (!flushed || !closed) {
    (void)::DeleteFileA(tmp.c_str());
    if (error_message) {
      *error_message = "temp file durable close failed: Windows error " +
                       std::to_string(!flushed ? flush_error : close_error);
    }
    return false;
  }
#endif
  std::error_code ec;
#ifndef _WIN32
  std::filesystem::rename(tmp, path, ec);
#else
  if (!::MoveFileExA(tmp.c_str(),
                     path.string().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
  }
#endif
  if (ec) {
    std::filesystem::remove(tmp);
    if (error_message) *error_message = ec.message();
    return false;
  }
#ifndef _WIN32
  ::chmod(path.c_str(), 0600);
#endif
  return FsyncParentDirectory(path, error_message);
}

bool ExistingOwnerIsLive(const ListenerSocketIdentity& identity, std::string* owner_pid) {
  std::string body;
  std::string diagnostic;
  if (!SignedArtifactValid(identity.owner_file, &body, &diagnostic)) return false;
  const auto fields = ParseKeyValueBody(body);
  const auto pid_it = fields.find("pid");
  const auto endpoint_it = fields.find("endpoint_hash");
  const auto generation_it = fields.find("generation");
  if (pid_it == fields.end() ||
      endpoint_it == fields.end() ||
      generation_it == fields.end() ||
      endpoint_it->second != identity.endpoint_hash ||
      generation_it->second != identity.generation) {
    return false;
  }
  char* end = nullptr;
  const long pid = std::strtol(pid_it->second.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || pid <= 0) return false;
  if (owner_pid != nullptr) *owner_pid = pid_it->second;
#ifndef _WIN32
  if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
  return errno == EPERM;
#else
  HANDLE process = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE,
                                 static_cast<DWORD>(pid));
  if (process == nullptr) {
    return ::GetLastError() == ERROR_ACCESS_DENIED;
  }
  const DWORD wait_rc = ::WaitForSingleObject(process, 0);
  ::CloseHandle(process);
  return wait_rc == WAIT_TIMEOUT;
#endif
}

} // namespace

ListenerSocketIdentity BuildSocketIdentity(const ListenerConfig& config) {
  ListenerSocketIdentity identity;
  identity.listener_uuid = config.listener_uuid.empty()
                               ? "listener-" + StableHash(config.protocol_family + config.bind_address + std::to_string(config.port))
                               : config.listener_uuid;
  identity.profile = config.listener_profile;
  identity.endpoint_hash = StableHash(config.server_endpoint + "|" + config.database_selector + "|" + config.protocol_family);
  identity.generation = std::to_string(config.lifecycle_generation);
  const std::string stem = Sanitize(config.protocol_family + "_" + identity.endpoint_hash);
  identity.control_socket = std::filesystem::path(config.control_dir) / (stem + ".control.sock");
  identity.management_socket = std::filesystem::path(config.control_dir) / (stem + ".management.sock");
  identity.owner_file = std::filesystem::path(config.control_dir) / (stem + ".owner");
  identity.lifecycle_file = std::filesystem::path(config.runtime_dir) / (stem + ".lifecycle.state");
  return identity;
}

bool WriteOwnerToken(const ListenerSocketIdentity& identity, std::string* error_message) {
  auto lock = ScopedArtifactLock::Acquire(identity.owner_file, error_message);
  if (!lock.held()) return false;
  std::error_code ec;
  if (std::filesystem::exists(identity.owner_file, ec)) {
    std::string owner_pid;
    if (!ec && ExistingOwnerIsLive(identity, &owner_pid)) {
      if (error_message) *error_message = "live owner token exists for pid " + owner_pid;
      return false;
    }
  }
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  std::ostringstream body;
  body << "format=SB_LISTENER_OWNER_V1\n"
       << "listener_uuid=" << identity.listener_uuid << '\n'
       << "profile=" << identity.profile << '\n'
       << "endpoint_hash=" << identity.endpoint_hash << '\n'
       << "generation=" << identity.generation << '\n'
#ifndef _WIN32
       << "pid=" << ::getpid() << '\n'
#else
       << "pid=" << ::GetCurrentProcessId() << '\n'
#endif
       << "startup_time_ms=" << now_ms << '\n'
       << "last_state=starting\n"
       << "clean_shutdown_required=true\n"
       << "file_mode=0600\n"
       << "atomic_write=true\n"
       << "file_fsync=true\n"
       << "parent_fsync=true\n"
       << "advisory_lock=true\n"
       << "lock_file_mode=0600\n"
       << "tamper_evidence=signature_sha256_128\n"
       << "control_socket=" << identity.control_socket.string() << '\n'
       << "management_socket=" << identity.management_socket.string() << '\n';
  return WriteSignedArtifactDurably(identity.owner_file, body.str(), error_message);
}

bool WriteLifecycleStateToken(const ListenerSocketIdentity& identity,
                              const std::string& effective_state,
                              const std::string& requested_state,
                              const std::string& identity_json,
                              const std::string& pool_json,
                              std::string* error_message) {
  auto lock = ScopedArtifactLock::Acquire(identity.lifecycle_file, error_message);
  if (!lock.held()) return false;
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  std::ostringstream body;
  body << "format=SB_LISTENER_LIFECYCLE_V1\n"
       << "state=" << effective_state << '\n'
       << "requested_state=" << requested_state << '\n'
       << "listener_uuid=" << identity.listener_uuid << '\n'
       << "profile=" << identity.profile << '\n'
       << "endpoint_hash=" << identity.endpoint_hash << '\n'
       << "generation=" << identity.generation << '\n'
       << "updated_time_ms=" << now_ms << '\n'
       << "file_mode=0600\n"
       << "atomic_write=true\n"
       << "file_fsync=true\n"
       << "parent_fsync=true\n"
       << "advisory_lock=true\n"
       << "lock_file_mode=0600\n"
       << "tamper_evidence=signature_sha256_128\n"
       << "identity=" << identity_json << '\n'
       << "pool=" << pool_json << '\n';
  return WriteSignedArtifactDurably(identity.lifecycle_file, body.str(), error_message);
}

std::string SocketIdentityJson(const ListenerSocketIdentity& identity) {
  std::ostringstream out;
  out << "{\"listener_uuid\":\"" << QuoteJson(identity.listener_uuid) << "\","
      << "\"profile\":\"" << QuoteJson(identity.profile) << "\","
      << "\"endpoint_hash\":\"" << QuoteJson(identity.endpoint_hash) << "\","
      << "\"generation\":\"" << QuoteJson(identity.generation) << "\","
      << "\"control_socket\":\"" << QuoteJson(identity.control_socket.string()) << "\","
      << "\"management_socket\":\"" << QuoteJson(identity.management_socket.string()) << "\"}";
  return out.str();
}

} // namespace scratchbird::listener
