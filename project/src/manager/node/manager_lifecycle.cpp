// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_NODE_MANAGER_LIFECYCLE

#include "manager_lifecycle.hpp"

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#endif

namespace proto = scratchbird::manager::protocol;

namespace scratchbird::manager::node {
namespace {

proto::Diagnostic LifecycleDiag(ManagerLifecycleState from, ManagerLifecycleState to) {
  return proto::MakeDiagnostic(
      "MANAGER.LIFECYCLE_INVALID_TRANSITION",
      "Manager lifecycle transition is invalid.",
      {{"from", ManagerLifecycleStateName(from)}, {"to", ManagerLifecycleStateName(to)}});
}

proto::Diagnostic LifecycleIoDiag(const std::string& code,
                                  const std::filesystem::path& path,
                                  const std::string& action,
                                  const std::string& error) {
  return proto::MakeDiagnostic(
      code,
      "Manager lifecycle evidence write failed.",
      {{"path", path.string()}, {"action", action}, {"error", error}});
}

bool IsTerminal(ManagerLifecycleState state) {
  return state == ManagerLifecycleState::kStopped ||
         state == ManagerLifecycleState::kStartupFailed ||
         state == ManagerLifecycleState::kFailedTerminal;
}

std::string OneLine(std::string value) {
  for (char& ch : value) {
    if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
  }
  return value;
}

std::uint64_t Fnv1a64(const std::string& text) {
  std::uint64_t hash = 1469598103934665603ull;
  for (unsigned char ch : text) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string Hex64(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << value;
  return out.str();
}

std::string LifecycleChecksum(const std::string& text) {
  return Hex64(Fnv1a64(text));
}

std::string StateFileBody(ManagerLifecycleState state) {
  std::ostringstream body;
  body << "format=SBMN_MANAGER_LIFECYCLE_STATE_V1\n";
  body << "state=" << ManagerLifecycleStateName(state) << "\n";
  body << "time_ms=" << proto::CurrentEpochMilliseconds() << "\n";
  const std::string body_text = body.str();
  std::ostringstream out;
  out << body_text;
  out << "checksum=" << LifecycleChecksum(body_text) << "\n";
  return out.str();
}

std::string JournalRecord(const std::string& state, const std::string& detail) {
  std::ostringstream body;
  body << "time_ms=" << proto::CurrentEpochMilliseconds()
       << " state=" << OneLine(state)
       << " detail=" << OneLine(detail);
  const std::string body_text = body.str();
  std::ostringstream out;
  out << body_text << " checksum=" << LifecycleChecksum(body_text) << '\n';
  return out.str();
}

#ifndef _WIN32
bool WriteAll(int fd, const std::string& content) {
  const char* data = content.data();
  std::size_t remaining = content.size();
  while (remaining > 0) {
    const ssize_t written = ::write(fd, data, remaining);
    if (written < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (written == 0) return false;
    data += written;
    remaining -= static_cast<std::size_t>(written);
  }
  return true;
}

std::string ErrnoMessage() {
  return std::error_code(errno, std::generic_category()).message();
}

bool SyncDirectory(const std::filesystem::path& dir,
                   const std::string& code,
                   std::vector<proto::Diagnostic>* diagnostics) {
#ifdef O_DIRECTORY
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#else
  const int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
#endif
  if (fd < 0) {
    if (diagnostics) diagnostics->push_back(LifecycleIoDiag(code, dir, "open_directory", ErrnoMessage()));
    return false;
  }
  if (::fsync(fd) != 0) {
    const std::string error = ErrnoMessage();
    ::close(fd);
    if (diagnostics) diagnostics->push_back(LifecycleIoDiag(code, dir, "sync_directory", error));
    return false;
  }
  if (::close(fd) != 0) {
    if (diagnostics) diagnostics->push_back(LifecycleIoDiag(code, dir, "close_directory", ErrnoMessage()));
    return false;
  }
  return true;
}
#endif

bool WriteAtomicFile(const std::filesystem::path& path,
                     const std::string& content,
                     const std::string& code,
                     std::vector<proto::Diagnostic>* diagnostics) {
  const auto dir = path.parent_path();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    if (diagnostics) diagnostics->push_back(LifecycleIoDiag(code, dir, "create_directory", ec.message()));
    return false;
  }

  const auto tmp = dir / (path.filename().string() + ".tmp." +
#ifndef _WIN32
                          std::to_string(::getpid()) +
#else
                          std::string("win") +
#endif
                          "." + std::to_string(proto::CurrentEpochMilliseconds()));
#ifndef _WIN32
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    if (diagnostics) diagnostics->push_back(LifecycleIoDiag(code, tmp, "open_temp", ErrnoMessage()));
    return false;
  }
  bool ok = WriteAll(fd, content);
  if (ok && ::fsync(fd) != 0) ok = false;
  const std::string write_error = ok ? std::string{} : ErrnoMessage();
  if (::close(fd) != 0) {
    ok = false;
  }
  if (!ok) {
    std::filesystem::remove(tmp, ec);
    if (diagnostics) diagnostics->push_back(LifecycleIoDiag(code, tmp, "write_temp", write_error.empty() ? "close_failed" : write_error));
    return false;
  }
  ::chmod(tmp.c_str(), 0600);
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    if (diagnostics) diagnostics->push_back(LifecycleIoDiag(code, path, "rename_temp", ec.message()));
    return false;
  }
  ::chmod(path.c_str(), 0600);
  return SyncDirectory(dir, code, diagnostics);
#else
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (diagnostics) diagnostics->push_back(LifecycleIoDiag(code, tmp, "open_temp", "open_failed"));
      return false;
    }
    out << content;
    out.flush();
    if (!out) {
      std::filesystem::remove(tmp, ec);
      if (diagnostics) diagnostics->push_back(LifecycleIoDiag(code, tmp, "write_temp", "write_failed"));
      return false;
    }
  }
  std::filesystem::remove(path, ec);
  ec.clear();
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    if (diagnostics) diagnostics->push_back(LifecycleIoDiag(code, path, "rename_temp", ec.message()));
    return false;
  }
  return true;
#endif
}

bool AppendDurableFile(const std::filesystem::path& path,
                       const std::string& record,
                       const std::string& code,
                       std::vector<proto::Diagnostic>* diagnostics) {
  const auto dir = path.parent_path();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    if (diagnostics) diagnostics->push_back(LifecycleIoDiag(code, dir, "create_directory", ec.message()));
    return false;
  }
#ifndef _WIN32
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0) {
    if (diagnostics) diagnostics->push_back(LifecycleIoDiag(code, path, "open_append", ErrnoMessage()));
    return false;
  }
  bool ok = WriteAll(fd, record);
  if (ok && ::fsync(fd) != 0) ok = false;
  const std::string write_error = ok ? std::string{} : ErrnoMessage();
  if (::close(fd) != 0) ok = false;
  if (!ok) {
    if (diagnostics) diagnostics->push_back(LifecycleIoDiag(code, path, "append", write_error.empty() ? "close_failed" : write_error));
    return false;
  }
  ::chmod(path.c_str(), 0600);
  return SyncDirectory(dir, code, diagnostics);
#else
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) {
    if (diagnostics) diagnostics->push_back(LifecycleIoDiag(code, path, "open_append", "open_failed"));
    return false;
  }
  out << record;
  out.flush();
  if (!out) {
    if (diagnostics) diagnostics->push_back(LifecycleIoDiag(code, path, "append", "write_failed"));
    return false;
  }
  return true;
#endif
}

}  // namespace

std::string ManagerLifecycleStateName(ManagerLifecycleState state) {
  switch (state) {
    case ManagerLifecycleState::kCreated: return "created";
    case ManagerLifecycleState::kArgsParsed: return "args_parsed";
    case ManagerLifecycleState::kConfigLoading: return "config_loading";
    case ManagerLifecycleState::kConfigValidating: return "config_validating";
    case ManagerLifecycleState::kRuntimePreparing: return "runtime_preparing";
    case ManagerLifecycleState::kOwnerAcquiring: return "owner_acquiring";
    case ManagerLifecycleState::kDaemonizing: return "daemonizing";
    case ManagerLifecycleState::kServerEndpointResolving: return "server_endpoint_resolving";
    case ManagerLifecycleState::kServerSupervisionStarting: return "server_supervision_starting";
    case ManagerLifecycleState::kListenerEndpointResolving: return "listener_endpoint_resolving";
    case ManagerLifecycleState::kListenerSupervisionStarting: return "listener_supervision_starting";
    case ManagerLifecycleState::kProxyBinding: return "proxy_binding";
    case ManagerLifecycleState::kManagementBinding: return "management_binding";
    case ManagerLifecycleState::kServerHeartbeatStarting: return "server_heartbeat_starting";
    case ManagerLifecycleState::kReady: return "ready";
    case ManagerLifecycleState::kRestricted: return "restricted";
    case ManagerLifecycleState::kDraining: return "draining";
    case ManagerLifecycleState::kStopping: return "stopping";
    case ManagerLifecycleState::kStopped: return "stopped";
    case ManagerLifecycleState::kStartupFailed: return "startup_failed";
    case ManagerLifecycleState::kFailedTerminal: return "failed_terminal";
    case ManagerLifecycleState::kQuarantined: return "quarantined";
  }
  return "unknown";
}

ManagerLifecycle::ManagerLifecycle(std::filesystem::path control_dir)
    : control_dir_(std::move(control_dir)) {}

ManagerLifecycleState ManagerLifecycle::current() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_;
}

std::string ManagerLifecycle::CurrentName() const {
  return ManagerLifecycleStateName(current());
}

bool ManagerLifecycle::Transition(ManagerLifecycleState next,
                                  const std::string& detail,
                                  std::vector<proto::Diagnostic>* diagnostics) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!IsLegalTransition(current_, next)) {
    if (diagnostics) diagnostics->push_back(LifecycleDiag(current_, next));
    (void)AppendJournalLocked(ManagerLifecycleStateName(current_),
                              "invalid_transition_to=" + ManagerLifecycleStateName(next),
                              diagnostics);
    return false;
  }
  if (!AppendJournalLocked(ManagerLifecycleStateName(next), detail, diagnostics)) return false;
  if (!WriteStateLocked(next, diagnostics)) return false;
  current_ = next;
  return true;
}

bool ManagerLifecycle::Evidence(const std::string& state, const std::string& detail) {
  std::lock_guard<std::mutex> lock(mutex_);
  return AppendJournalLocked(state, detail, nullptr);
}

bool ManagerLifecycle::IsLegalTransition(ManagerLifecycleState from, ManagerLifecycleState to) const {
  if (from == to) return true;
  if (IsTerminal(from)) return false;
  if (to == ManagerLifecycleState::kStartupFailed ||
      to == ManagerLifecycleState::kFailedTerminal ||
      to == ManagerLifecycleState::kQuarantined) {
    return true;
  }
  switch (from) {
    case ManagerLifecycleState::kCreated:
      return to == ManagerLifecycleState::kArgsParsed ||
             to == ManagerLifecycleState::kRuntimePreparing;
    case ManagerLifecycleState::kArgsParsed:
      return to == ManagerLifecycleState::kConfigLoading;
    case ManagerLifecycleState::kConfigLoading:
      return to == ManagerLifecycleState::kConfigValidating;
    case ManagerLifecycleState::kConfigValidating:
      return to == ManagerLifecycleState::kRuntimePreparing;
    case ManagerLifecycleState::kRuntimePreparing:
      return to == ManagerLifecycleState::kOwnerAcquiring ||
             to == ManagerLifecycleState::kServerEndpointResolving ||
             to == ManagerLifecycleState::kStopped;
    case ManagerLifecycleState::kOwnerAcquiring:
      return to == ManagerLifecycleState::kDaemonizing ||
             to == ManagerLifecycleState::kServerEndpointResolving ||
             to == ManagerLifecycleState::kStopped;
    case ManagerLifecycleState::kDaemonizing:
      return to == ManagerLifecycleState::kServerEndpointResolving;
    case ManagerLifecycleState::kServerEndpointResolving:
      return to == ManagerLifecycleState::kServerSupervisionStarting ||
             to == ManagerLifecycleState::kListenerEndpointResolving;
    case ManagerLifecycleState::kServerSupervisionStarting:
      return to == ManagerLifecycleState::kListenerEndpointResolving;
    case ManagerLifecycleState::kListenerEndpointResolving:
      return to == ManagerLifecycleState::kListenerSupervisionStarting ||
             to == ManagerLifecycleState::kProxyBinding;
    case ManagerLifecycleState::kListenerSupervisionStarting:
      return to == ManagerLifecycleState::kProxyBinding;
    case ManagerLifecycleState::kProxyBinding:
      return to == ManagerLifecycleState::kManagementBinding;
    case ManagerLifecycleState::kManagementBinding:
      return to == ManagerLifecycleState::kServerHeartbeatStarting ||
             to == ManagerLifecycleState::kReady;
    case ManagerLifecycleState::kServerHeartbeatStarting:
      return to == ManagerLifecycleState::kReady ||
             to == ManagerLifecycleState::kRestricted;
    case ManagerLifecycleState::kReady:
    case ManagerLifecycleState::kRestricted:
      return to == ManagerLifecycleState::kDraining ||
             to == ManagerLifecycleState::kStopping;
    case ManagerLifecycleState::kDraining:
      return to == ManagerLifecycleState::kStopping ||
             to == ManagerLifecycleState::kStopped;
    case ManagerLifecycleState::kStopping:
      return to == ManagerLifecycleState::kStopped;
    case ManagerLifecycleState::kQuarantined:
      return to == ManagerLifecycleState::kDraining ||
             to == ManagerLifecycleState::kStopping;
    case ManagerLifecycleState::kStopped:
    case ManagerLifecycleState::kStartupFailed:
    case ManagerLifecycleState::kFailedTerminal:
      return false;
  }
  return false;
}

bool ManagerLifecycle::WriteStateLocked(ManagerLifecycleState state,
                                        std::vector<proto::Diagnostic>* diagnostics) {
  std::error_code ec;
  std::filesystem::create_directories(control_dir_, ec);
  if (ec) {
    if (diagnostics) diagnostics->push_back(LifecycleIoDiag("MANAGER.LIFECYCLE_STATE_WRITE_FAILED", control_dir_, "create_directory", ec.message()));
    return false;
  }
  if (!CleanupStaleStateTempsLocked(diagnostics)) return false;
  return WriteAtomicFile(control_dir_ / "sbmn_manager.lifecycle.state",
                         StateFileBody(state),
                         "MANAGER.LIFECYCLE_STATE_WRITE_FAILED",
                         diagnostics);
}

bool ManagerLifecycle::AppendJournalLocked(const std::string& state,
                                           const std::string& detail,
                                           std::vector<proto::Diagnostic>* diagnostics) {
  std::error_code ec;
  std::filesystem::create_directories(control_dir_, ec);
  if (ec) {
    if (diagnostics) diagnostics->push_back(LifecycleIoDiag("MANAGER.LIFECYCLE_JOURNAL_WRITE_FAILED", control_dir_, "create_directory", ec.message()));
    return false;
  }
  return AppendDurableFile(control_dir_ / "sbmn_manager.lifecycle.journal",
                           JournalRecord(state, detail),
                           "MANAGER.LIFECYCLE_JOURNAL_WRITE_FAILED",
                           diagnostics);
}

bool ManagerLifecycle::CleanupStaleStateTempsLocked(std::vector<proto::Diagnostic>* diagnostics) {
  std::error_code ec;
  if (!std::filesystem::exists(control_dir_, ec)) return true;
  for (const auto& entry : std::filesystem::directory_iterator(control_dir_, ec)) {
    if (ec) {
      if (diagnostics) diagnostics->push_back(LifecycleIoDiag("MANAGER.LIFECYCLE_STATE_WRITE_FAILED", control_dir_, "scan_temp", ec.message()));
      return false;
    }
    const auto name = entry.path().filename().string();
    if (name.rfind("sbmn_manager.lifecycle.state.tmp.", 0) != 0) continue;
    std::filesystem::remove(entry.path(), ec);
    if (ec) {
      if (diagnostics) diagnostics->push_back(LifecycleIoDiag("MANAGER.LIFECYCLE_STATE_WRITE_FAILED", entry.path(), "remove_stale_temp", ec.message()));
      return false;
    }
  }
  return true;
}

}  // namespace scratchbird::manager::node
