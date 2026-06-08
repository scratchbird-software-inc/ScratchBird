// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "config.hpp"
#include "memory_policy_config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace memory = scratchbird::core::memory;
namespace server = scratchbird::server;

constexpr std::uint64_t kMiB = 1024ull * 1024ull;

std::filesystem::path TempRoot() {
  auto root = std::filesystem::temp_directory_path() / "scratchbird_memory_policy_config_gate";
  std::filesystem::create_directories(root);
  return root;
}

std::string ConfigText(std::string_view memory_body) {
  std::string text;
  text += "[config]\n";
  text += "format = SBCD1\n";
  text += "\n";
  text += "[server.memory]\n";
  text += memory_body;
  return text;
}

std::filesystem::path WriteConfig(const std::string& name, std::string_view memory_body) {
  const auto path = TempRoot() / name;
  std::ofstream out(path);
  out << ConfigText(memory_body);
  return path;
}

server::ServerConfigLoadResult Load(std::string_view name, std::string_view memory_body) {
  server::ServerCliOptions cli;
  cli.config_path = WriteConfig(std::string(name), memory_body).string();
  return server::ResolveServerBootstrapConfig(cli);
}

bool HasDiagnostic(const server::ServerConfigLoadResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

bool HasDiagnostic(const memory::MemoryPolicyConfigResolveResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.diagnostic_code == code) {
      return true;
    }
  }
  return false;
}

bool Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool ValidConfigBuildsAllocationPolicy() {
  const auto loaded = Load("valid.conf",
                           "policy_name = production_gate\n"
                           "hard_limit_bytes = 268435456\n"
                           "soft_limit_bytes = 201326592\n"
                           "per_context_limit_bytes = 67108864\n"
                           "page_buffer_pool_limit_bytes = 33554432\n"
                           "failure_mode = fatal_status\n"
                           "track_allocations = true\n"
                           "zero_memory_on_allocate = false\n"
                           "zero_memory_on_release = true\n"
                           "reject_over_soft_limit = true\n"
                           "policy_provenance = test_config\n"
                           "enable_platform_memory_probe = false\n"
                           "require_platform_memory_ceiling = false\n"
                           "policy_generation = 7\n");
  if (!Expect(loaded.ok(), "valid memory policy config should parse")) return false;
  const auto resolved = server::ResolveServerMemoryAllocationPolicy(loaded.config);
  if (!Expect(resolved.ok(), "valid memory policy should resolve")) return false;
  const auto policy = resolved.policy;
  return Expect(policy.policy_name == "production_gate", "policy name mismatch") &&
         Expect(policy.hard_limit_bytes == 256ull * kMiB, "hard limit mismatch") &&
         Expect(policy.byte_limit == policy.hard_limit_bytes, "byte_limit should mirror hard limit") &&
         Expect(policy.soft_limit_bytes == 192ull * kMiB, "soft limit mismatch") &&
         Expect(policy.per_context_limit_bytes == 64ull * kMiB, "per-context limit mismatch") &&
         Expect(policy.page_buffer_pool_limit_bytes == 32ull * kMiB, "page buffer pool limit mismatch") &&
         Expect(policy.failure_mode == memory::AllocationFailureMode::fatal_status, "failure mode mismatch") &&
         Expect(policy.track_allocations, "track_allocations mismatch") &&
         Expect(policy.zero_memory_on_release, "zero_memory_on_release mismatch") &&
         Expect(policy.reject_over_soft_limit, "reject_over_soft_limit mismatch") &&
         Expect(!loaded.config.memory_enable_platform_memory_probe,
                "enable_platform_memory_probe config mismatch") &&
         Expect(!loaded.config.memory_require_platform_memory_ceiling,
                "require_platform_memory_ceiling config mismatch");
}

bool InvalidConfigFailsClosed(std::string_view name,
                              std::string_view body,
                              std::string_view diagnostic_code) {
  const auto loaded = Load(name, body);
  return Expect(!loaded.ok(), "invalid memory policy config should fail closed") &&
         Expect(HasDiagnostic(loaded, diagnostic_code), "expected diagnostic was not emitted");
}

bool CoreResolverCarriesProvenanceAndGeneration() {
  memory::MemoryPolicyConfig config;
  config.policy_name = "direct_core_policy";
  config.provenance = "unit_test";
  config.source_epoch = 2;
  config.reload_generation = 3;
  config.policy_generation = 4;
  config.enable_platform_memory_probe = false;
  const auto resolved = memory::ResolveMemoryPolicyConfig(config);
  return Expect(resolved.ok(), "core memory policy resolver should accept default production policy") &&
         Expect(resolved.policy.policy_name == "direct_core_policy", "core resolver policy name mismatch");
}

memory::HostContainerMemoryCeilings DirectCeilings(std::uint64_t max_bytes,
                                                   std::uint64_t high_bytes,
                                                   std::uint64_t memtotal_bytes) {
  memory::HostContainerMemoryCeilings ceilings;
  ceilings.platform_name = "test";
  ceilings.platform_supported = true;
  ceilings.signals.push_back({memory::MemoryCeilingSignalKind::cgroup_v2_memory_max,
                              "direct:memory.max",
                              std::to_string(max_bytes),
                              true,
                              true,
                              true,
                              max_bytes,
                              "direct_test_ceiling"});
  ceilings.signals.push_back({memory::MemoryCeilingSignalKind::cgroup_v2_memory_high,
                              "direct:memory.high",
                              std::to_string(high_bytes),
                              true,
                              true,
                              true,
                              high_bytes,
                              "direct_test_ceiling"});
  ceilings.signals.push_back({memory::MemoryCeilingSignalKind::proc_meminfo_memtotal,
                              "direct:MemTotal",
                              std::to_string(memtotal_bytes),
                              true,
                              true,
                              true,
                              memtotal_bytes,
                              "direct_test_ceiling"});
  return ceilings;
}

bool PlatformCeilingClampsEffectivePolicy() {
  memory::MemoryPolicyConfig config;
  config.policy_name = "platform_clamped_policy";
  config.hard_limit_bytes = 256ull * kMiB;
  config.soft_limit_bytes = 64ull * kMiB;
  config.per_context_limit_bytes = 32ull * kMiB;
  config.page_buffer_pool_limit_bytes = 32ull * kMiB;
  config.platform_ceiling_override = DirectCeilings(128ull * kMiB, 96ull * kMiB, 512ull * kMiB);

  const auto resolved = memory::ResolveMemoryPolicyConfig(config);
  return Expect(resolved.ok(), "platform ceiling clamp should be accepted") &&
         Expect(resolved.configured_hard_limit_bytes == 256ull * kMiB,
                "configured hard limit evidence mismatch") &&
         Expect(resolved.platform_ceiling_bytes && *resolved.platform_ceiling_bytes == 96ull * kMiB,
                "available platform ceiling mismatch") &&
         Expect(resolved.effective_hard_limit_bytes == 96ull * kMiB,
                "effective hard limit mismatch") &&
         Expect(resolved.policy.hard_limit_bytes == 96ull * kMiB,
                "allocation policy hard limit should be clamped") &&
         Expect(resolved.policy.byte_limit == resolved.policy.hard_limit_bytes,
                "byte_limit should mirror effective hard limit");
}

bool RequiredCeilingRejectsConfiguredOvercommit() {
  memory::MemoryPolicyConfig config;
  config.hard_limit_bytes = 256ull * kMiB;
  config.soft_limit_bytes = 64ull * kMiB;
  config.per_context_limit_bytes = 32ull * kMiB;
  config.page_buffer_pool_limit_bytes = 32ull * kMiB;
  config.require_platform_memory_ceiling = true;
  config.platform_ceiling_override = DirectCeilings(128ull * kMiB, 128ull * kMiB, 512ull * kMiB);

  const auto resolved = memory::ResolveMemoryPolicyConfig(config);
  return Expect(!resolved.ok(), "required platform ceiling overcommit should fail closed") &&
         Expect(HasDiagnostic(resolved, "MEMORY.POLICY_CONFIGURED_HARD_EXCEEDS_REQUIRED_CEILING"),
                "required ceiling overcommit diagnostic missing");
}

bool InvalidRequiredCeilingFailsClosed() {
  memory::HostContainerMemoryCeilings ceilings;
  ceilings.platform_name = "test";
  ceilings.platform_supported = true;
  ceilings.signals.push_back({memory::MemoryCeilingSignalKind::cgroup_v2_memory_max,
                              "direct:memory.max",
                              "not-a-number",
                              true,
                              false,
                              false,
                              0,
                              "direct_invalid_ceiling"});

  memory::MemoryPolicyConfig config;
  config.require_platform_memory_ceiling = true;
  config.platform_ceiling_override = ceilings;

  const auto resolved = memory::ResolveMemoryPolicyConfig(config);
  return Expect(!resolved.ok(), "invalid required platform ceiling should fail closed") &&
         Expect(HasDiagnostic(resolved, "MEMORY.POLICY_INVALID_CEILING_VALUE"),
                "invalid ceiling diagnostic missing");
}

bool RequiredUnavailableCeilingFailsClosed() {
  memory::HostContainerMemoryCeilings ceilings;
  ceilings.platform_name = "test";
  ceilings.platform_supported = true;
  ceilings.signals.push_back({memory::MemoryCeilingSignalKind::cgroup_v2_memory_max,
                              "direct:memory.max",
                              "",
                              false,
                              true,
                              false,
                              0,
                              "direct_unavailable_ceiling"});

  memory::MemoryPolicyConfig config;
  config.require_platform_memory_ceiling = true;
  config.platform_ceiling_override = ceilings;

  const auto resolved = memory::ResolveMemoryPolicyConfig(config);
  return Expect(!resolved.ok(), "unavailable required platform ceiling should fail closed") &&
         Expect(HasDiagnostic(resolved, "MEMORY.POLICY_REQUIRED_CEILING_UNAVAILABLE"),
                "required ceiling unavailable diagnostic missing");
}

bool FixturePathProbeIsDeterministic() {
  const auto root = TempRoot() / "fixture_probe";
  const auto cgroup = root / "cgroup";
  std::filesystem::create_directories(cgroup);
  {
    std::ofstream out(cgroup / "memory.max");
    out << (300ull * kMiB) << '\n';
  }
  {
    std::ofstream out(cgroup / "memory.high");
    out << "max\n";
  }
  const auto meminfo = root / "meminfo";
  {
    std::ofstream out(meminfo);
    out << "MemTotal:       524288 kB\n";
  }

  memory::PlatformMemoryCeilingProbePaths paths;
  paths.cgroup_v2_root = cgroup.string();
  paths.proc_meminfo = meminfo.string();
  const auto ceilings = memory::ProbeHostContainerMemoryCeilings(paths);
  if (!Expect(ceilings.available_ceiling_bytes &&
                  *ceilings.available_ceiling_bytes == 300ull * kMiB,
              "fixture platform ceiling probe mismatch")) {
    return false;
  }

  memory::MemoryPolicyConfig config;
  config.hard_limit_bytes = 256ull * kMiB;
  config.soft_limit_bytes = 192ull * kMiB;
  config.per_context_limit_bytes = 64ull * kMiB;
  config.page_buffer_pool_limit_bytes = 64ull * kMiB;
  config.require_platform_memory_ceiling = true;
  config.platform_probe_paths = paths;
  const auto resolved = memory::ResolveMemoryPolicyConfig(config);
  return Expect(resolved.ok(), "fixture path required ceiling should resolve") &&
         Expect(resolved.effective_hard_limit_bytes == 256ull * kMiB,
                "fixture path effective hard limit mismatch");
}

bool EffectiveHardBoundsDerivedLimits() {
  memory::MemoryPolicyConfig config;
  config.hard_limit_bytes = 256ull * kMiB;
  config.soft_limit_bytes = 192ull * kMiB;
  config.per_context_limit_bytes = 64ull * kMiB;
  config.page_buffer_pool_limit_bytes = 64ull * kMiB;
  config.platform_ceiling_override = DirectCeilings(128ull * kMiB, 128ull * kMiB, 512ull * kMiB);

  const auto resolved = memory::ResolveMemoryPolicyConfig(config);
  return Expect(!resolved.ok(), "derived limits above effective hard should fail") &&
         Expect(HasDiagnostic(resolved, "MEMORY.POLICY_SOFT_EXCEEDS_EFFECTIVE_HARD"),
                "soft effective hard diagnostic missing");
}

bool DefaultManagerInstallsConfiguredPolicyOnce() {
  memory::AllocationPolicy policy;
  policy.policy_name = "mmch011_startup_policy";
  policy.byte_limit = 128ull * kMiB;
  policy.hard_limit_bytes = 128ull * kMiB;
  policy.soft_limit_bytes = 96ull * kMiB;
  policy.per_context_limit_bytes = 32ull * kMiB;
  policy.page_buffer_pool_limit_bytes = 16ull * kMiB;
  policy.zero_memory_on_release = true;

  const auto installed = memory::ConfigureDefaultMemoryManager(policy, "memory_policy_config_gate");
  if (!Expect(installed.ok(), "configured default memory policy should install before first use")) {
    return false;
  }
  const auto& active = memory::DefaultMemoryManager().policy();
  if (!Expect(active.policy_name == "mmch011_startup_policy",
              "default memory manager did not use configured policy")) {
    return false;
  }
  const auto idempotent = memory::ConfigureDefaultMemoryManager(policy, "memory_policy_config_gate");
  return Expect(idempotent.ok(), "idempotent default memory policy install should be accepted") &&
         Expect(idempotent.already_initialized,
                "idempotent default memory policy install should report initialized manager");
}

}  // namespace

int main() {
  bool ok = true;
  ok = ValidConfigBuildsAllocationPolicy() && ok;
  ok = CoreResolverCarriesProvenanceAndGeneration() && ok;
  ok = PlatformCeilingClampsEffectivePolicy() && ok;
  ok = RequiredCeilingRejectsConfiguredOvercommit() && ok;
  ok = InvalidRequiredCeilingFailsClosed() && ok;
  ok = RequiredUnavailableCeilingFailsClosed() && ok;
  ok = FixturePathProbeIsDeterministic() && ok;
  ok = EffectiveHardBoundsDerivedLimits() && ok;
  ok = DefaultManagerInstallsConfiguredPolicyOnce() && ok;
  ok = InvalidConfigFailsClosed("soft_gt_hard.conf",
                                "hard_limit_bytes = 67108864\n"
                                "soft_limit_bytes = 134217728\n",
                                "MEMORY.POLICY_LIMIT_EXCEEDS_HARD") && ok;
  ok = InvalidConfigFailsClosed("per_context_gt_hard.conf",
                                "hard_limit_bytes = 268435456\n"
                                "per_context_limit_bytes = 536870912\n",
                                "MEMORY.POLICY_LIMIT_EXCEEDS_HARD") && ok;
  ok = InvalidConfigFailsClosed("page_pool_gt_hard.conf",
                                "hard_limit_bytes = 268435456\n"
                                "page_buffer_pool_limit_bytes = 536870912\n",
                                "MEMORY.POLICY_LIMIT_EXCEEDS_HARD") && ok;
  ok = InvalidConfigFailsClosed("bad_failure_mode.conf",
                                "failure_mode = retry_forever\n",
                                "CONFIG.VALUE_INVALID_ENUM") && ok;
  ok = InvalidConfigFailsClosed("hard_too_small.conf",
                                "hard_limit_bytes = 4096\n",
                                "MEMORY.POLICY_HARD_LIMIT_TOO_SMALL") && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
