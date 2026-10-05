// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory_policy_config.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/sysctl.h>
#endif

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') {
      return static_cast<char>(ch - 'A' + 'a');
    }
    return static_cast<char>(ch);
  });
  return value;
}

std::string TrimAscii(std::string value) {
  auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
  while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

DiagnosticRecord PolicyDiagnostic(std::string code,
                                  std::string message_key,
                                  std::vector<DiagnosticArgument> arguments) {
  return MakeDiagnostic(StatusCode::memory_invalid_request,
                        Severity::error,
                        Subsystem::memory,
                        std::move(code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.memory.policy_config",
                        "Correct the memory policy configuration and reload the server configuration.");
}

bool ParseUnsignedU64(const std::string& raw, u64* out) {
  const auto value = TrimAscii(raw);
  if (value.empty()) {
    return false;
  }
  u64 parsed = 0;
  for (char ch : value) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    const auto digit = static_cast<u64>(ch - '0');
    if (parsed > (std::numeric_limits<u64>::max() - digit) / 10ull) {
      return false;
    }
    parsed = parsed * 10ull + digit;
  }
  *out = parsed;
  return true;
}

std::optional<std::string> ReadBoundedText(const std::filesystem::path& path,
                                       std::size_t maximum_bytes) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::string text;
  char ch;
  while (in.get(ch)) {
    if (text.size() == maximum_bytes || ch == '\0') {
      return std::nullopt;
    }
    text.push_back(ch);
  }
  if (!in.eof()) {
    return std::nullopt;
  }
  return text;
}

MemoryCeilingSignal UnavailableSignal(MemoryCeilingSignalKind kind,
                                      std::string source,
                                      std::string evidence) {
  MemoryCeilingSignal signal;
  signal.kind = kind;
  signal.source = std::move(source);
  signal.available = false;
  signal.valid = true;
  signal.finite = false;
  signal.evidence = std::move(evidence);
  return signal;
}

MemoryCeilingSignal ParseCgroupLimit(MemoryCeilingSignalKind kind,
                                     const std::filesystem::path& path) {
  auto line = ReadBoundedText(path, 4096);
  if (!line) {
    return UnavailableSignal(kind, path.string(), "cgroup_v2_memory_ceiling_unavailable");
  }

  MemoryCeilingSignal signal;
  signal.kind = kind;
  signal.source = path.string();
  signal.raw_value = TrimAscii(*line);
  signal.available = true;
  signal.valid = true;
  signal.finite = false;
  signal.evidence = "cgroup_v2_memory_ceiling";
  if (signal.raw_value == "max") {
    return signal;
  }
  u64 bytes = 0;
  // Zero is a restrictive finite limit, not the cgroup "max" sentinel.
  if (!ParseUnsignedU64(signal.raw_value, &bytes)) {
    signal.valid = false;
    signal.evidence = "cgroup_v2_memory_ceiling_invalid";
    return signal;
  }
  signal.finite = true;
  signal.bytes = bytes;
  return signal;
}

#if defined(__linux__)
bool SafeAbsoluteCgroupPath(const std::filesystem::path& path) {
  if (!path.is_absolute()) {
    return false;
  }
  std::size_t depth = 0;
  for (const auto& component : path) {
    if (component.empty() || component == "." || component == ".." || ++depth > 256) {
      return false;
    }
  }
  return true;
}

std::optional<std::string> DecodeMountPath(const std::string& raw) {
  std::string result;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] != '\\') {
      result.push_back(raw[i]);
      continue;
    }
    const auto escape = raw.substr(i, 4);
    if (escape == "\\040") result.push_back(' ');
    else if (escape == "\\011") result.push_back('\t');
    else if (escape == "\\012") result.push_back('\n');
    else if (escape == "\\134") result.push_back('\\');
    else return std::nullopt;
    i += 3;
  }
  return result;
}

void IncompleteCgroup(HostContainerMemoryCeilings* ceilings,
                      const std::string& source, const std::string& reason) {
  ceilings->container_limit_incomplete = true;
  ceilings->signals.push_back(UnavailableSignal(
      MemoryCeilingSignalKind::cgroup_v2_memory_max, source, reason));
}

void ProbeCgroupDirectory(const std::filesystem::path& directory,
                          bool hierarchy_root, HostContainerMemoryCeilings* ceilings) {
  auto maximum = ParseCgroupLimit(MemoryCeilingSignalKind::cgroup_v2_memory_max,
                                  directory / "memory.max");
  auto high = ParseCgroupLimit(MemoryCeilingSignalKind::cgroup_v2_memory_high,
                               directory / "memory.high");
  // The global v2 root has no memory limit files. A namespace root exposing
  // those files is a real constrained subgroup, with ancestors outside view.
  if (hierarchy_root && (maximum.available || high.available)) {
    IncompleteCgroup(ceilings, directory.string(), "cgroup_hidden_ancestors");
  }
  if (hierarchy_root) {
    // Unlike memory interfaces, cgroup.type is present on every non-root
    // cgroup, including one whose memory controller is disabled. Namespace
    // virtualization must not make that subgroup look like the global root.
    std::error_code error;
    if (std::filesystem::symlink_status(directory / "cgroup.type", error).type() !=
        std::filesystem::file_type::not_found) {
      IncompleteCgroup(ceilings, directory.string(), "cgroup_root_not_verified");
    }
  }
  if (!maximum.available && !high.available) {
    const auto controllers = ReadBoundedText(directory / "cgroup.controllers", 4096);
    bool memory_controller = false;
    if (controllers) {
      std::istringstream tokens(*controllers);
      std::string token;
      while (tokens >> token) memory_controller |= token == "memory";
    }
    std::error_code max_error, high_error;
    const auto max_status = std::filesystem::symlink_status(directory / "memory.max", max_error);
    const auto high_status = std::filesystem::symlink_status(directory / "memory.high", high_error);
    const bool both_absent = max_status.type() == std::filesystem::file_type::not_found &&
                             high_status.type() == std::filesystem::file_type::not_found;
    if (controllers && both_absent && (hierarchy_root || !memory_controller)) {
      // No local interface; parents are still visited for inherited limits.
      return;
    }
  }
  if (!maximum.available || !maximum.valid || !high.available || !high.valid) {
    ceilings->container_limit_incomplete = true;
  }
  ceilings->signals.push_back(std::move(maximum));
  ceilings->signals.push_back(std::move(high));
}

void ProbeProcessCgroup(const PlatformMemoryCeilingProbePaths& paths,
                        HostContainerMemoryCeilings* ceilings) {
  if (!paths.cgroup_v2_root.empty()) {
    ProbeCgroupDirectory(paths.cgroup_v2_root, false, ceilings);
    return;
  }
  constexpr std::size_t kMaxProcBytes = 1024 * 1024;
  const auto membership = ReadBoundedText(paths.proc_self_cgroup, kMaxProcBytes);
  const auto mounts = ReadBoundedText(paths.proc_self_mountinfo, kMaxProcBytes);
  if (!membership || !mounts || mounts->empty()) {
    IncompleteCgroup(ceilings, "proc_self_cgroup", "cgroup_discovery_source_unavailable");
    return;
  }
  std::optional<std::filesystem::path> member;
  std::istringstream memberships(*membership);
  std::string line;
  bool invalid = false;
  while (std::getline(memberships, line)) {
    const auto first = line.find(':');
    const auto second = first == std::string::npos ? first : line.find(':', first + 1);
    u64 hierarchy = 0;
    if (first == std::string::npos || second == std::string::npos ||
        !ParseUnsignedU64(line.substr(0, first), &hierarchy)) {
      invalid = true;
      continue;
    }
    const auto controllers = line.substr(first + 1, second - first - 1);
    if ((hierarchy == 0) != controllers.empty() ||
        !SafeAbsoluteCgroupPath(line.substr(second + 1))) {
      invalid = true;
      continue;
    }
    if (hierarchy == 0 && controllers.empty()) {
      const std::filesystem::path path(line.substr(second + 1));
      if (member || !SafeAbsoluteCgroupPath(path)) invalid = true;
      else member = path;
    } else if (("," + controllers + ",").find(",memory,") != std::string::npos) {
      // An applicable v1 controller is not evidence of an unlimited v2 limit.
      IncompleteCgroup(ceilings, paths.proc_self_cgroup, "cgroup_v1_memory_unresolved");
    }
  }
  if (invalid || membership->empty()) {
    IncompleteCgroup(ceilings, paths.proc_self_cgroup, "cgroup_membership_invalid");
    return;
  }
  std::istringstream mount_lines(*mounts);
  std::size_t matching_mounts = 0;
  bool has_v2_mount = false;
  while (std::getline(mount_lines, line)) {
    std::istringstream tokens(line);
    std::vector<std::string> fields;
    std::string token;
    while (tokens >> token) fields.push_back(token);
    const auto separator = std::find(fields.begin(), fields.end(), "-");
    const auto split = static_cast<std::size_t>(separator - fields.begin());
    if (split < 6 || split + 3 >= fields.size()) {
      IncompleteCgroup(ceilings, paths.proc_self_mountinfo, "cgroup_mountinfo_invalid");
      continue;
    }
    if (fields[split + 1] != "cgroup2") continue;
    has_v2_mount = true;
    const auto root_text = DecodeMountPath(fields[3]);
    const auto mount_text = DecodeMountPath(fields[4]);
    if (!root_text || !mount_text || !SafeAbsoluteCgroupPath(*root_text) ||
        !SafeAbsoluteCgroupPath(*mount_text)) {
      IncompleteCgroup(ceilings, paths.proc_self_mountinfo, "cgroup_mount_path_invalid");
      continue;
    }
    if (!member) continue;
    const std::filesystem::path root(*root_text), mount(*mount_text);
    const auto relative = member->lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..") continue;
    ++matching_mounts;
    if (root != "/") {
      IncompleteCgroup(ceilings, mount.string(), "cgroup_hidden_ancestors");
    }
    auto directory = relative == "." ? mount : (mount / relative).lexically_normal();
    for (;;) {
      // Never follow a substituted directory symlink into an unrelated tree.
      std::error_code error;
      if (std::filesystem::symlink_status(directory, error).type() !=
          std::filesystem::file_type::directory) {
        IncompleteCgroup(ceilings, directory.string(), "cgroup_directory_unavailable");
      } else {
        ProbeCgroupDirectory(directory, directory == mount && root == "/", ceilings);
      }
      if (directory == mount) break;
      const auto parent = directory.parent_path();
      if (parent == directory || parent.empty()) {
        IncompleteCgroup(ceilings, mount.string(), "cgroup_mount_boundary_invalid");
        break;
      }
      directory = parent;
    }
  }
  if ((member && matching_mounts != 1) || (!member && has_v2_mount)) {
    IncompleteCgroup(ceilings, paths.proc_self_mountinfo, "cgroup_mount_binding_unresolved");
  }
  // This detects changes during collection; it does not claim an atomic kernel
  // snapshot. The activation consumer must revalidate its selected observation.
  if (ReadBoundedText(paths.proc_self_cgroup, kMaxProcBytes) != membership ||
      ReadBoundedText(paths.proc_self_mountinfo, kMaxProcBytes) != mounts) {
    IncompleteCgroup(ceilings, "proc_self_cgroup", "cgroup_membership_or_mount_changed");
  }
}
#endif

MemoryCeilingSignal ParseMemTotal(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    return UnavailableSignal(MemoryCeilingSignalKind::proc_meminfo_memtotal,
                             path.string(),
                             "proc_meminfo_memtotal_unavailable");
  }

  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("MemTotal:", 0) != 0) {
      continue;
    }
    MemoryCeilingSignal signal;
    signal.kind = MemoryCeilingSignalKind::proc_meminfo_memtotal;
    signal.source = path.string();
    signal.raw_value = TrimAscii(line);
    signal.available = true;
    signal.valid = true;
    signal.evidence = "proc_meminfo_memtotal";

    std::istringstream stream(line.substr(std::string("MemTotal:").size()));
    std::string amount;
    std::string unit;
    stream >> amount >> unit;
    u64 kib = 0;
    if (!ParseUnsignedU64(amount, &kib) || kib == 0 || (unit != "kB" && unit != "KB" && unit != "kb") ||
        kib > std::numeric_limits<u64>::max() / 1024ull) {
      signal.valid = false;
      signal.evidence = "proc_meminfo_memtotal_invalid";
      return signal;
    }
    signal.finite = true;
    signal.bytes = kib * 1024ull;
    return signal;
  }

  return UnavailableSignal(MemoryCeilingSignalKind::proc_meminfo_memtotal,
                           path.string(),
                           "proc_meminfo_memtotal_missing");
}

#if !defined(_WIN32)
MemoryCeilingSignal ProbePosixRlimitAs() {
  struct rlimit limit {};
  if (::getrlimit(RLIMIT_AS, &limit) != 0) {
    return UnavailableSignal(MemoryCeilingSignalKind::posix_rlimit_as,
                             "getrlimit:RLIMIT_AS",
                             "posix_rlimit_as_unavailable");
  }
  if (limit.rlim_cur == RLIM_INFINITY) {
    MemoryCeilingSignal signal;
    signal.kind = MemoryCeilingSignalKind::posix_rlimit_as;
    signal.source = "getrlimit:RLIMIT_AS";
    signal.raw_value = "unlimited";
    signal.available = true;
    signal.valid = true;
    signal.finite = false;
    signal.evidence = "posix_rlimit_as_unlimited";
    return signal;
  }
  MemoryCeilingSignal signal;
  signal.kind = MemoryCeilingSignalKind::posix_rlimit_as;
  signal.source = "getrlimit:RLIMIT_AS";
  signal.raw_value = std::to_string(static_cast<u64>(limit.rlim_cur));
  signal.available = true;
  signal.valid = true;
  signal.finite = true;
  signal.bytes = static_cast<u64>(limit.rlim_cur);
  signal.evidence = "posix_rlimit_as";
  return signal;
}

MemoryCeilingSignal ProbePosixSysconfPhysicalMemory() {
#if defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
  const long pages = ::sysconf(_SC_PHYS_PAGES);
  const long page_size = ::sysconf(_SC_PAGESIZE);
  if (pages <= 0 || page_size <= 0) {
    return UnavailableSignal(MemoryCeilingSignalKind::posix_sysconf_physical_memory,
                             "sysconf:_SC_PHYS_PAGES*_SC_PAGESIZE",
                             "posix_sysconf_physical_memory_unavailable");
  }
  const auto upages = static_cast<u64>(pages);
  const auto upage_size = static_cast<u64>(page_size);
  MemoryCeilingSignal signal;
  signal.kind = MemoryCeilingSignalKind::posix_sysconf_physical_memory;
  signal.source = "sysconf:_SC_PHYS_PAGES*_SC_PAGESIZE";
  signal.raw_value = std::to_string(upages) + "*" + std::to_string(upage_size);
  signal.available = true;
  signal.evidence = "posix_sysconf_physical_memory";
  if (upage_size == 0 || upages > std::numeric_limits<u64>::max() / upage_size) {
    signal.valid = false;
    return signal;
  }
  signal.valid = true;
  signal.finite = true;
  signal.bytes = upages * upage_size;
  return signal;
#else
  return UnavailableSignal(MemoryCeilingSignalKind::posix_sysconf_physical_memory,
                           "sysconf:_SC_PHYS_PAGES*_SC_PAGESIZE",
                           "posix_sysconf_physical_memory_not_compiled");
#endif
}
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
MemoryCeilingSignal ProbeDarwinBsdHwMemsize() {
#if defined(__APPLE__)
  const char* name = "hw.memsize";
#else
  const char* name = "hw.physmem";
#endif
  std::uint64_t bytes = 0;
  std::size_t size = sizeof(bytes);
  if (::sysctlbyname(name, &bytes, &size, nullptr, 0) != 0 || bytes == 0) {
    return UnavailableSignal(MemoryCeilingSignalKind::darwin_bsd_hw_memsize,
                             std::string("sysctl:") + name,
                             "darwin_bsd_hw_memsize_unavailable");
  }
  MemoryCeilingSignal signal;
  signal.kind = MemoryCeilingSignalKind::darwin_bsd_hw_memsize;
  signal.source = std::string("sysctl:") + name;
  signal.raw_value = std::to_string(static_cast<u64>(bytes));
  signal.available = true;
  signal.valid = true;
  signal.finite = true;
  signal.bytes = static_cast<u64>(bytes);
  signal.evidence = "darwin_bsd_hw_memsize";
  return signal;
}
#endif

#if defined(_WIN32)
MemoryCeilingSignal ProbeWindowsGlobalMemoryStatus() {
  MEMORYSTATUSEX status;
  status.dwLength = sizeof(status);
  if (!::GlobalMemoryStatusEx(&status)) {
    return UnavailableSignal(MemoryCeilingSignalKind::windows_global_memory_status,
                             "GlobalMemoryStatusEx:ullTotalPhys",
                             "windows_global_memory_status_unavailable");
  }
  MemoryCeilingSignal signal;
  signal.kind = MemoryCeilingSignalKind::windows_global_memory_status;
  signal.source = "GlobalMemoryStatusEx:ullTotalPhys";
  signal.raw_value = std::to_string(static_cast<u64>(status.ullTotalPhys));
  signal.available = true;
  signal.valid = status.ullTotalPhys != 0;
  signal.finite = signal.valid;
  signal.bytes = static_cast<u64>(status.ullTotalPhys);
  signal.evidence = "windows_global_memory_status";
  return signal;
}

MemoryCeilingSignal ProbeWindowsJobObjectMemoryLimit() {
  BOOL in_job = FALSE;
  if (!::IsProcessInJob(::GetCurrentProcess(), nullptr, &in_job) || !in_job) {
    return UnavailableSignal(MemoryCeilingSignalKind::windows_job_object_memory_limit,
                             "QueryInformationJobObject:ProcessMemoryLimit",
                             "windows_job_object_unavailable");
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info {};
  DWORD returned = 0;
  if (!::QueryInformationJobObject(nullptr,
                                   JobObjectExtendedLimitInformation,
                                   &info,
                                   sizeof(info),
                                   &returned)) {
    return UnavailableSignal(MemoryCeilingSignalKind::windows_job_object_memory_limit,
                             "QueryInformationJobObject:ProcessMemoryLimit",
                             "windows_job_object_query_failed");
  }
  if ((info.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) == 0 ||
      info.ProcessMemoryLimit == 0) {
    MemoryCeilingSignal signal;
    signal.kind = MemoryCeilingSignalKind::windows_job_object_memory_limit;
    signal.source = "QueryInformationJobObject:ProcessMemoryLimit";
    signal.raw_value = "unlimited";
    signal.available = true;
    signal.valid = true;
    signal.finite = false;
    signal.evidence = "windows_job_object_unlimited";
    return signal;
  }
  MemoryCeilingSignal signal;
  signal.kind = MemoryCeilingSignalKind::windows_job_object_memory_limit;
  signal.source = "QueryInformationJobObject:ProcessMemoryLimit";
  signal.raw_value = std::to_string(static_cast<u64>(info.ProcessMemoryLimit));
  signal.available = true;
  signal.valid = true;
  signal.finite = true;
  signal.bytes = static_cast<u64>(info.ProcessMemoryLimit);
  signal.evidence = "windows_job_object_memory_limit";
  return signal;
}
#endif

void FinalizeAvailableCeiling(HostContainerMemoryCeilings* ceilings) {
  std::optional<u64> limit;
  for (const auto& signal : ceilings->signals) {
    if (!signal.available || !signal.valid || !signal.finite) {
      continue;
    }
    limit = limit ? std::min(*limit, signal.bytes) : std::optional<u64>(signal.bytes);
  }
  ceilings->available_ceiling_bytes = limit;
}

bool HasInvalidCeilingSignal(const HostContainerMemoryCeilings& ceilings) {
  for (const auto& signal : ceilings.signals) {
    if (signal.available && !signal.valid) {
      return true;
    }
  }
  return false;
}

void AddInvalidCeilingDiagnostics(std::vector<DiagnosticRecord>* diagnostics,
                                  const HostContainerMemoryCeilings& ceilings) {
  for (const auto& signal : ceilings.signals) {
    if (!signal.available || signal.valid) {
      continue;
    }
    diagnostics->push_back(PolicyDiagnostic(
        "MEMORY.POLICY_INVALID_CEILING_VALUE",
        "memory.policy_invalid_ceiling_value",
        {{"ceiling_kind", MemoryCeilingSignalKindName(signal.kind)},
         {"source", signal.source},
         {"raw_value", signal.raw_value}}));
  }
}

void ValidateLimit(std::vector<DiagnosticRecord>* diagnostics,
                   const char* field,
                   u64 value,
                   u64 hard_limit) {
  if (value > hard_limit) {
    diagnostics->push_back(PolicyDiagnostic(
        "MEMORY.POLICY_LIMIT_EXCEEDS_HARD",
        "memory.policy_limit_exceeds_hard",
        {{"field", field},
         {"value", std::to_string(value)},
         {"hard_limit_bytes", std::to_string(hard_limit)}}));
  }
}

void ValidateEffectiveLimit(std::vector<DiagnosticRecord>* diagnostics,
                            const char* field,
                            const char* code,
                            const char* message_key,
                            u64 value,
                            u64 effective_hard_limit) {
  if (value > effective_hard_limit) {
    diagnostics->push_back(PolicyDiagnostic(
        code,
        message_key,
        {{"field", field},
         {"value", std::to_string(value)},
         {"effective_hard_limit_bytes", std::to_string(effective_hard_limit)}}));
  }
}

}  // namespace

const char* AllocationFailureModeName(AllocationFailureMode mode) {
  switch (mode) {
    case AllocationFailureMode::return_error:
      return "return_error";
    case AllocationFailureMode::fatal_status:
      return "fatal_status";
  }
  return "return_error";
}

bool ParseAllocationFailureMode(const std::string& value, AllocationFailureMode* out) {
  const auto lower = LowerAscii(value);
  if (lower == "return_error") {
    *out = AllocationFailureMode::return_error;
    return true;
  }
  if (lower == "fatal_status") {
    *out = AllocationFailureMode::fatal_status;
    return true;
  }
  return false;
}

const char* MemoryCeilingSignalKindName(MemoryCeilingSignalKind kind) {
  switch (kind) {
    case MemoryCeilingSignalKind::cgroup_v2_memory_max:
      return "cgroup_v2_memory_max";
    case MemoryCeilingSignalKind::cgroup_v2_memory_high:
      return "cgroup_v2_memory_high";
    case MemoryCeilingSignalKind::posix_rlimit_as:
      return "posix_rlimit_as";
    case MemoryCeilingSignalKind::posix_sysconf_physical_memory:
      return "posix_sysconf_physical_memory";
    case MemoryCeilingSignalKind::proc_meminfo_memtotal:
      return "proc_meminfo_memtotal";
    case MemoryCeilingSignalKind::darwin_bsd_hw_memsize:
      return "darwin_bsd_hw_memsize";
    case MemoryCeilingSignalKind::windows_job_object_memory_limit:
      return "windows_job_object_memory_limit";
    case MemoryCeilingSignalKind::windows_global_memory_status:
      return "windows_global_memory_status";
    case MemoryCeilingSignalKind::platform_unsupported:
      return "platform_unsupported";
  }
  return "platform_unsupported";
}

HostContainerMemoryCeilings ProbeHostContainerMemoryCeilings(
    const PlatformMemoryCeilingProbePaths& paths) {
  HostContainerMemoryCeilings ceilings;
#if defined(_WIN32)
  ceilings.platform_name = "windows";
  ceilings.platform_supported = true;
  ceilings.signals.push_back(ProbeWindowsJobObjectMemoryLimit());
  ceilings.signals.push_back(ProbeWindowsGlobalMemoryStatus());
#else
  ceilings.platform_supported = true;
#if defined(__linux__)
  ceilings.platform_name = "linux";
  ProbeProcessCgroup(paths, &ceilings);
  ceilings.signals.push_back(ParseMemTotal(std::filesystem::path(paths.proc_meminfo)));
#elif defined(__APPLE__)
  ceilings.platform_name = "macos";
  ceilings.signals.push_back(ProbeDarwinBsdHwMemsize());
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
  ceilings.platform_name = "bsd";
  ceilings.signals.push_back(ProbeDarwinBsdHwMemsize());
#else
  ceilings.platform_name = "posix";
#endif
  ceilings.signals.push_back(ProbePosixRlimitAs());
  ceilings.signals.push_back(ProbePosixSysconfPhysicalMemory());
#endif
  if (ceilings.signals.empty()) {
    ceilings.platform_name = "unsupported";
    ceilings.platform_supported = false;
    ceilings.signals.push_back(UnavailableSignal(MemoryCeilingSignalKind::platform_unsupported,
                                                 "host_platform",
                                                 "platform_memory_ceiling_probe_unsupported"));
  }
  FinalizeAvailableCeiling(&ceilings);
  return ceilings;
}

u64 DefaultBootstrapEmergencyReserveBytes(u64 effective_budget_bytes) noexcept {
  constexpr u64 minimum = 128ull * 1024ull * 1024ull;
  constexpr u64 maximum = 1024ull * 1024ull * 1024ull;
  const u64 percentage = (effective_budget_bytes / 100) * 3 +
                        ((effective_budget_bytes % 100) * 3 + 99) / 100;
  return std::clamp(percentage, minimum, maximum);
}

MemoryPolicyConfigResolveResult ResolveMemoryPolicyConfig(const MemoryPolicyConfig& config) {
  MemoryPolicyConfigResolveResult result;
  result.configured_hard_limit_bytes = config.hard_limit_bytes;

  if (config.platform_ceiling_override) {
    result.ceiling_evidence = *config.platform_ceiling_override;
    FinalizeAvailableCeiling(&result.ceiling_evidence);
  } else if (config.enable_platform_memory_probe) {
    result.ceiling_evidence = ProbeHostContainerMemoryCeilings(config.platform_probe_paths);
  } else {
    result.ceiling_evidence.platform_name = "disabled";
    result.ceiling_evidence.platform_supported = false;
    MemoryCeilingSignal signal;
    signal.kind = MemoryCeilingSignalKind::platform_unsupported;
    signal.source = "memory_policy_config";
    signal.available = false;
    signal.valid = true;
    signal.finite = false;
    signal.evidence = "platform_memory_ceiling_probe_disabled";
    result.ceiling_evidence.signals.push_back(std::move(signal));
  }
  result.platform_ceiling_bytes = result.ceiling_evidence.available_ceiling_bytes;
  result.effective_hard_limit_bytes = result.platform_ceiling_bytes
                                           ? std::min(config.hard_limit_bytes,
                                                      *result.platform_ceiling_bytes)
                                           : config.hard_limit_bytes;

  const bool incomplete_container = result.ceiling_evidence.container_limit_incomplete ||
      std::any_of(result.ceiling_evidence.signals.begin(), result.ceiling_evidence.signals.end(),
          [](const MemoryCeilingSignal& signal) {
            return (signal.kind == MemoryCeilingSignalKind::cgroup_v2_memory_max ||
                    signal.kind == MemoryCeilingSignalKind::cgroup_v2_memory_high ||
                    signal.kind == MemoryCeilingSignalKind::windows_job_object_memory_limit) &&
                   signal.available && !signal.valid;
          });
  if (incomplete_container) {
    result.ceiling_evidence.container_limit_incomplete = true;
    if (!config.allow_degraded_container_limit || config.require_platform_memory_ceiling ||
        config.degraded_container_cap_bytes == 0) {
      result.diagnostics.push_back(PolicyDiagnostic(
          "MEMORY.CONTAINER_LIMIT_UNVERIFIED", "memory.container_limit_unverified",
          {{"policy_generation", std::to_string(config.policy_generation)},
           {"reason", config.require_platform_memory_ceiling ? "verified_limit_required" :
                       !config.allow_degraded_container_limit ? "degraded_not_enabled" : "missing_finite_cap"}}));
    } else {
      result.effective_hard_limit_bytes = std::min(result.effective_hard_limit_bytes,
                                                 config.degraded_container_cap_bytes);
      result.degraded_container_limit = true;
    }
  }

  if (config.require_platform_memory_ceiling && HasInvalidCeilingSignal(result.ceiling_evidence)) {
    AddInvalidCeilingDiagnostics(&result.diagnostics, result.ceiling_evidence);
  }
  if (config.require_platform_memory_ceiling && !result.platform_ceiling_bytes) {
    result.diagnostics.push_back(PolicyDiagnostic(
        "MEMORY.POLICY_REQUIRED_CEILING_UNAVAILABLE",
        "memory.policy_required_ceiling_unavailable",
        {{"platform_name", result.ceiling_evidence.platform_name},
         {"signal_count", std::to_string(result.ceiling_evidence.signals.size())}}));
  }
  if (config.require_platform_memory_ceiling && result.platform_ceiling_bytes &&
      config.hard_limit_bytes > *result.platform_ceiling_bytes) {
    result.diagnostics.push_back(PolicyDiagnostic(
        "MEMORY.POLICY_CONFIGURED_HARD_EXCEEDS_REQUIRED_CEILING",
        "memory.policy_configured_hard_exceeds_required_ceiling",
        {{"configured_hard_limit_bytes", std::to_string(config.hard_limit_bytes)},
         {"required_ceiling_bytes", std::to_string(*result.platform_ceiling_bytes)}}));
  }

  if (config.hard_limit_bytes < kMinimumProductionMemoryHardLimitBytes) {
    result.diagnostics.push_back(PolicyDiagnostic(
        "MEMORY.POLICY_HARD_LIMIT_TOO_SMALL",
        "memory.policy_hard_limit_too_small",
        {{"field", "hard_limit_bytes"},
         {"value", std::to_string(config.hard_limit_bytes)},
         {"minimum_bytes", std::to_string(kMinimumProductionMemoryHardLimitBytes)}}));
  } else if (result.effective_hard_limit_bytes < kMinimumProductionMemoryHardLimitBytes) {
    // Platform clamping must not bypass the production minimum. In particular,
    // a finite zero ceiling must never become an allocator's unlimited policy.
    // Keep the observed ceiling intact for diagnostics; do not raise it.
    result.diagnostics.push_back(PolicyDiagnostic(
        "MEMORY.POLICY_HARD_LIMIT_TOO_SMALL",
        "memory.policy_hard_limit_too_small",
        {{"field", "effective_hard_limit_bytes"},
         {"value", std::to_string(result.effective_hard_limit_bytes)},
         {"minimum_bytes", std::to_string(kMinimumProductionMemoryHardLimitBytes)}}));
  }
  ValidateLimit(&result.diagnostics,
                "soft_limit_bytes",
                config.soft_limit_bytes,
                config.hard_limit_bytes);
  ValidateLimit(&result.diagnostics,
                "per_context_limit_bytes",
                config.per_context_limit_bytes,
                config.hard_limit_bytes);
  ValidateLimit(&result.diagnostics,
                "page_buffer_pool_limit_bytes",
                config.page_buffer_pool_limit_bytes,
                config.hard_limit_bytes);
  ValidateEffectiveLimit(&result.diagnostics,
                         "soft_limit_bytes",
                         "MEMORY.POLICY_SOFT_EXCEEDS_EFFECTIVE_HARD",
                         "memory.policy_soft_exceeds_effective_hard",
                         config.soft_limit_bytes,
                         result.effective_hard_limit_bytes);
  ValidateEffectiveLimit(&result.diagnostics,
                         "per_context_limit_bytes",
                         "MEMORY.POLICY_PER_CONTEXT_EXCEEDS_EFFECTIVE_HARD",
                         "memory.policy_per_context_exceeds_effective_hard",
                         config.per_context_limit_bytes,
                         result.effective_hard_limit_bytes);
  ValidateEffectiveLimit(&result.diagnostics,
                         "page_buffer_pool_limit_bytes",
                         "MEMORY.POLICY_PAGE_POOL_EXCEEDS_EFFECTIVE_HARD",
                         "memory.policy_page_pool_exceeds_effective_hard",
                         config.page_buffer_pool_limit_bytes,
                         result.effective_hard_limit_bytes);
  if (config.source_epoch == 0 || config.reload_generation == 0 || config.policy_generation == 0) {
    result.diagnostics.push_back(PolicyDiagnostic(
        "MEMORY.POLICY_GENERATION_INVALID",
        "memory.policy_generation_invalid",
        {{"source_epoch", std::to_string(config.source_epoch)},
         {"reload_generation", std::to_string(config.reload_generation)},
         {"policy_generation", std::to_string(config.policy_generation)}}));
  }
  if (!result.ok()) {
    result.degraded_container_limit = false;
    return result;
  }

  if (result.degraded_container_limit) {
    result.warnings.push_back(MakeDiagnostic(
        StatusCode::ok, Severity::warning, Subsystem::memory,
        "MEMORY.CONTAINER_LIMIT_DEGRADED", "memory.container_limit_degraded",
        {{"policy_generation", std::to_string(config.policy_generation)},
         {"reason", "applicable_container_limit_incomplete"},
         {"configured_hard_limit_bytes", std::to_string(config.hard_limit_bytes)},
         {"degraded_cap_bytes", std::to_string(config.degraded_container_cap_bytes)},
         {"known_ceiling_bytes", result.platform_ceiling_bytes ? std::to_string(*result.platform_ceiling_bytes) : "unavailable"},
         {"effective_hard_limit_bytes", std::to_string(result.effective_hard_limit_bytes)}},
        {}, "core.memory.policy_config",
        "Restore container limit discovery; the degraded cap is not a verified container allowance."));
  }

  result.policy.policy_name = config.policy_name.empty()
                                  ? "default_local_server_memory_cache_v1"
                                  : config.policy_name;
  result.policy.byte_limit = result.effective_hard_limit_bytes;
  result.policy.hard_limit_bytes = result.effective_hard_limit_bytes;
  result.policy.soft_limit_bytes = config.soft_limit_bytes;
  result.policy.per_context_limit_bytes = config.per_context_limit_bytes;
  result.policy.page_buffer_pool_limit_bytes = config.page_buffer_pool_limit_bytes;
  result.policy.failure_mode = config.failure_mode;
  result.policy.track_allocations = config.track_allocations;
  result.policy.zero_memory_on_allocate = config.zero_memory_on_allocate;
  result.policy.zero_memory_on_release = config.zero_memory_on_release;
  result.policy.reject_over_soft_limit = config.reject_over_soft_limit;
  return result;
}

}  // namespace scratchbird::core::memory
