// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "memory.hpp"

#include <optional>
#include <string>
#include <vector>

namespace scratchbird::core::memory {

inline constexpr u64 kMinimumProductionMemoryHardLimitBytes = 16ull * 1024ull * 1024ull;

enum class MemoryCeilingSignalKind {
  cgroup_v2_memory_max,
  cgroup_v2_memory_high,
  posix_rlimit_as,
  posix_sysconf_physical_memory,
  proc_meminfo_memtotal,
  darwin_bsd_hw_memsize,
  windows_job_object_memory_limit,
  windows_global_memory_status,
  platform_unsupported
};

// MMCH_OS_CONTAINER_MEMORY_LIMITS
struct MemoryCeilingSignal {
  MemoryCeilingSignalKind kind = MemoryCeilingSignalKind::platform_unsupported;
  std::string source;
  std::string raw_value;
  bool available = false;
  bool valid = true;
  bool finite = false;
  u64 bytes = 0;
  std::string evidence;
};

struct HostContainerMemoryCeilings {
  std::string platform_name = "unknown";
  bool platform_supported = false;
  std::vector<MemoryCeilingSignal> signals;
  std::optional<u64> available_ceiling_bytes;
};

struct PlatformMemoryCeilingProbePaths {
  std::string cgroup_v2_root = "/sys/fs/cgroup";
  std::string proc_meminfo = "/proc/meminfo";
};

struct MemoryPolicyConfig {
  std::string policy_name = "default_local_server_memory_cache_v1";
  // MMCH_MEMORY_METADATA_OPEN_UPGRADE_COMPATIBILITY
  // Versioned policy metadata is open/upgrade evidence only. It is never
  // transaction finality, visibility, authorization, or recovery authority.
  u64 metadata_format_version = 2;
  u64 hard_limit_bytes = 1024ull * 1024ull * 1024ull;
  u64 soft_limit_bytes = 768ull * 1024ull * 1024ull;
  u64 per_context_limit_bytes = 256ull * 1024ull * 1024ull;
  u64 page_buffer_pool_limit_bytes = 512ull * 1024ull * 1024ull;
  AllocationFailureMode failure_mode = AllocationFailureMode::return_error;
  bool track_allocations = true;
  bool zero_memory_on_allocate = false;
  bool zero_memory_on_release = true;
  bool reject_over_soft_limit = false;
  std::string provenance =
      "default_policy_pack:default-local-password:server_memory_cache_policy";
  u64 source_epoch = 1;
  u64 reload_generation = 1;
  u64 policy_generation = 1;
  bool enable_platform_memory_probe = true;
  bool require_platform_memory_ceiling = false;
  PlatformMemoryCeilingProbePaths platform_probe_paths;
  std::optional<HostContainerMemoryCeilings> platform_ceiling_override;
};

struct MemoryPolicyConfigResolveResult {
  AllocationPolicy policy;
  std::vector<DiagnosticRecord> diagnostics;
  u64 configured_hard_limit_bytes = 0;
  std::optional<u64> platform_ceiling_bytes;
  u64 effective_hard_limit_bytes = 0;
  HostContainerMemoryCeilings ceiling_evidence;

  bool ok() const { return diagnostics.empty(); }
};

const char* AllocationFailureModeName(AllocationFailureMode mode);
bool ParseAllocationFailureMode(const std::string& value, AllocationFailureMode* out);
const char* MemoryCeilingSignalKindName(MemoryCeilingSignalKind kind);
HostContainerMemoryCeilings ProbeHostContainerMemoryCeilings(
    const PlatformMemoryCeilingProbePaths& paths = {});
MemoryPolicyConfigResolveResult ResolveMemoryPolicyConfig(const MemoryPolicyConfig& config);

}  // namespace scratchbird::core::memory
