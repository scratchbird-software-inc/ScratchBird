// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "config.hpp"
#include "memory_policy_config.hpp"
#include "startup.hpp"
#include "../support/owned_temp_directory.hpp"

#include <cstdlib>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <cerrno>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace memory = scratchbird::core::memory;
namespace server = scratchbird::server;

constexpr std::uint64_t kMiB = 1024ull * 1024ull;

void SetPolicyRootEnv() {
#if defined(_WIN32)
  _putenv_s("SCRATCHBIRD_POLICY_SEED_PACK_ROOT", SB_DEFAULT_POLICY_PACK_ROOT);
#else
  setenv("SCRATCHBIRD_POLICY_SEED_PACK_ROOT", SB_DEFAULT_POLICY_PACK_ROOT, 1);
#endif
}

scratchbird::tests::OwnedTempDirectory& TestArtifacts() {
  static scratchbird::tests::OwnedTempDirectory artifacts;
  return artifacts;
}

std::filesystem::path TempRoot() {
  return TestArtifacts().path();
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
  SetPolicyRootEnv();
  const auto loaded = Load("valid.conf",
                           "policy_name = production_gate\n"
                           "hard_limit_bytes = 1073741824\n"
                           "soft_limit_bytes = 805306368\n"
                           "per_context_limit_bytes = 268435456\n"
                           "page_buffer_pool_limit_bytes = 134217728\n"
                           "min_startup_available_bytes = 1073741824\n"
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
         Expect(policy.hard_limit_bytes == 1024ull * kMiB, "hard limit mismatch") &&
         Expect(policy.byte_limit == policy.hard_limit_bytes, "byte_limit should mirror hard limit") &&
         Expect(policy.soft_limit_bytes == 768ull * kMiB, "soft limit mismatch") &&
         Expect(policy.per_context_limit_bytes == 256ull * kMiB, "per-context limit mismatch") &&
         Expect(policy.page_buffer_pool_limit_bytes == 128ull * kMiB, "page buffer pool limit mismatch") &&
         Expect(policy.failure_mode == memory::AllocationFailureMode::fatal_status, "failure mode mismatch") &&
         Expect(policy.track_allocations, "track_allocations mismatch") &&
         Expect(policy.zero_memory_on_release, "zero_memory_on_release mismatch") &&
         Expect(policy.reject_over_soft_limit, "reject_over_soft_limit mismatch") &&
         Expect(!loaded.config.memory_enable_platform_memory_probe,
                "enable_platform_memory_probe config mismatch") &&
         Expect(!loaded.config.memory_require_platform_memory_ceiling,
                "require_platform_memory_ceiling config mismatch");
}

bool DefaultPolicyPackMemoryPolicyLoads() {
  SetPolicyRootEnv();
  const auto loaded = Load("policy_pack_defaults.conf", "");
  if (!Expect(loaded.ok(), "default server config should load policy-pack memory defaults")) {
    return false;
  }
  const auto resolved = server::ResolveServerMemoryAllocationPolicy(loaded.config);
  return Expect(resolved.ok(), "default policy-pack memory policy should resolve") &&
         Expect(loaded.config.memory_policy_name == "default_local_server_memory_cache_v1",
                "default memory policy name mismatch") &&
         Expect(loaded.config.memory_hard_limit_bytes == 1024ull * kMiB,
                "default policy-pack hard limit mismatch") &&
         Expect(loaded.config.memory_soft_limit_bytes == 768ull * kMiB,
                "default policy-pack soft limit mismatch") &&
         Expect(loaded.config.memory_per_context_limit_bytes == 256ull * kMiB,
                "default policy-pack per-context limit mismatch") &&
         Expect(loaded.config.memory_page_buffer_pool_limit_bytes == 512ull * kMiB,
                "default policy-pack page-buffer pool mismatch") &&
         Expect(loaded.config.memory_openssl_budget_bytes == 4ull * kMiB,
                "default OpenSSL backing budget mismatch") &&
         Expect(loaded.config.memory_min_startup_available_bytes == 1024ull * kMiB,
                "default policy-pack startup floor mismatch") &&
         Expect(loaded.config.memory_adaptive_page_cache_enabled,
                "default policy-pack adaptive page cache should be enabled") &&
         Expect(loaded.config.memory_index_read_cache_enabled,
                "default policy-pack index read cache should be enabled") &&
         Expect(!loaded.config.memory_trim_heap_on_disconnect,
                "default policy-pack should retain adaptive cache on disconnect") &&
         Expect(loaded.config.memory_policy_provenance ==
                    "default_policy_pack:default-local-password:server_memory_cache_policy",
                "default policy-pack provenance mismatch");
}

bool ExplicitConfigOverridesPolicyPackMemoryFields() {
  SetPolicyRootEnv();
  const auto loaded = Load("explicit_override.conf",
                           "policy_name = operator_override\n"
                           "hard_limit_bytes = 2147483648\n"
                           "soft_limit_bytes = 1073741824\n"
                           "per_context_limit_bytes = 536870912\n"
                           "page_buffer_pool_limit_bytes = 268435456\n"
                           "min_startup_available_bytes = 2147483648\n"
                           "trim_heap_on_disconnect = true\n"
                           "enable_platform_memory_probe = false\n"
                           "policy_provenance = operator_config\n");
  if (!Expect(loaded.ok(), "explicit memory overrides should parse with policy defaults")) {
    return false;
  }
  const auto resolved = server::ResolveServerMemoryAllocationPolicy(loaded.config);
  return Expect(resolved.ok(), "explicit override memory policy should resolve") &&
         Expect(loaded.config.memory_policy_name == "operator_override",
                "explicit policy name override mismatch") &&
         Expect(loaded.config.memory_hard_limit_bytes == 2048ull * kMiB,
                "explicit hard limit override mismatch") &&
         Expect(loaded.config.memory_soft_limit_bytes == 1024ull * kMiB,
                "explicit soft limit override mismatch") &&
         Expect(loaded.config.memory_per_context_limit_bytes == 512ull * kMiB,
                "explicit per-context override mismatch") &&
         Expect(loaded.config.memory_page_buffer_pool_limit_bytes == 256ull * kMiB,
                "explicit page-buffer pool override mismatch") &&
         Expect(loaded.config.memory_min_startup_available_bytes == 2048ull * kMiB,
                "explicit startup floor override mismatch") &&
         Expect(loaded.config.memory_trim_heap_on_disconnect,
                "explicit disconnect trim override mismatch") &&
         Expect(!loaded.config.memory_enable_platform_memory_probe,
                "explicit platform probe override mismatch") &&
         Expect(loaded.config.memory_policy_provenance == "operator_config",
                "explicit provenance override mismatch");
}

bool InvalidConfigFailsClosed(std::string_view name,
                              std::string_view body,
                              std::string_view diagnostic_code) {
  const auto loaded = Load(name, body);
  for (const auto& diagnostic : loaded.diagnostics) {
    if (diagnostic.code != diagnostic_code || !diagnostic.code.starts_with("MEMORY."))
      continue;
    if (!Expect(diagnostic.native_platform_source.has_value(),
                "Core memory-policy cause was discarded by the server") ||
        !Expect(diagnostic.native_platform_source->diagnostic_code == diagnostic.code &&
                    diagnostic.native_platform_source->message_key == diagnostic.message_key &&
                    !diagnostic.native_platform_source->status.ok() &&
                    !diagnostic.native_platform_source->arguments.empty(),
                "server did not retain the complete failing memory-policy source")) return false;
    for (const auto& argument : diagnostic.native_platform_source->arguments) {
      if (!argument.text()) continue;
      bool found = false;
      for (const auto& field : diagnostic.fields)
        found = found || (field.key == argument.key && field.value == *argument.text());
      if (!Expect(found, "memory-policy text argument lost during server adaptation")) return false;
    }
  }
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

bool ProcessCgroupDiscoveryIncludesAncestors() {
#if defined(__linux__)
  const auto root = TempRoot() / "membership_probe";
  const auto mount = root / "mounted tree";
  const auto parent = mount / "tenant";
  const auto leaf = parent / "server";
  std::filesystem::create_directories(leaf);
  auto write = [](const std::filesystem::path& path, const std::string& value) {
    std::ofstream out(path);
    out << value;
  };
  memory::PlatformMemoryCeilingProbePaths paths;
  paths.proc_self_cgroup = (root / "membership").string();
  paths.proc_self_mountinfo = (root / "mountinfo").string();
  paths.proc_meminfo = (root / "meminfo").string();
  write(paths.proc_meminfo, "MemTotal: 524288 kB\n");
  write(paths.proc_self_cgroup, "0::/tenant/server\n");
  std::string escaped_mount = mount.string();
  escaped_mount.replace(escaped_mount.find(' '), 1, "\\040");
  const std::string mount_line = "37 28 0:31 / " + escaped_mount +
      " rw,nosuid shared:9 - cgroup2 cgroup2 rw\n";
  write(paths.proc_self_mountinfo, mount_line);
  write(mount / "cgroup.controllers", "cpu memory pids\n");
  write(parent / "memory.max", std::to_string(128 * kMiB) + "\n");
  write(parent / "memory.high", "max\n");
  write(leaf / "memory.max", std::to_string(256 * kMiB) + "\n");
  write(leaf / "memory.high", "max\n");
  auto observed = memory::ProbeHostContainerMemoryCeilings(paths);
  bool ok = Expect(!observed.container_limit_incomplete,
                   "visible complete hierarchy must resolve") &&
            Expect(observed.available_ceiling_bytes == 128 * kMiB,
                   "parent clamp must constrain the selected process leaf");
  write(parent / "memory.high", "0\n");
  observed = memory::ProbeHostContainerMemoryCeilings(paths);
  ok = Expect(observed.available_ceiling_bytes == 0,
              "zero ancestor limit must remain restrictive") && ok;
  write(parent / "memory.high", "invalid\n");
  observed = memory::ProbeHostContainerMemoryCeilings(paths);
  ok = Expect(observed.container_limit_incomplete &&
                  observed.available_ceiling_bytes == 128 * kMiB,
              "invalid ancestor must retain known clamps and incomplete state") && ok;
  write(parent / "memory.high", "max\n");
  std::filesystem::remove(leaf / "memory.max");
  observed = memory::ProbeHostContainerMemoryCeilings(paths);
  ok = Expect(observed.container_limit_incomplete &&
                  observed.available_ceiling_bytes == 128 * kMiB,
              "missing applicable leaf limit cannot fall back to host RAM") && ok;
  memory::MemoryPolicyConfig config;
  config.platform_probe_paths = paths;
  config.hard_limit_bytes = 256 * kMiB;
  config.soft_limit_bytes = 0;
  config.per_context_limit_bytes = 0;
  config.page_buffer_pool_limit_bytes = 0;
  auto resolved = memory::ResolveMemoryPolicyConfig(config);
  ok = Expect(!resolved.ok() && HasDiagnostic(resolved, "MEMORY.CONTAINER_LIMIT_UNVERIFIED"),
              "real incomplete source must refuse normal admission") && ok;
  config.allow_degraded_container_limit = true;
  config.degraded_container_cap_bytes = 64 * kMiB;
  resolved = memory::ResolveMemoryPolicyConfig(config);
  ok = Expect(resolved.ok() && resolved.degraded_container_limit &&
                  resolved.effective_hard_limit_bytes == 64 * kMiB &&
                  resolved.warnings.size() == 1,
              "real incomplete source must consume explicit degraded policy") && ok;
  write(leaf / "memory.max", "max\n");
  // Namespace roots remain incomplete even without local memory interfaces.
  write(mount / "cgroup.type", "domain\n");
  observed = memory::ProbeHostContainerMemoryCeilings(paths);
  ok = Expect(observed.container_limit_incomplete,
              "namespace root with disabled memory still hides ancestors") && ok;
  std::filesystem::remove(mount / "cgroup.type");
  std::filesystem::remove(leaf / "memory.max");
  std::filesystem::remove(leaf / "memory.high");
  write(leaf / "cgroup.controllers", "cpu pids\n");
  observed = memory::ProbeHostContainerMemoryCeilings(paths);
  ok = Expect(!observed.container_limit_incomplete &&
                  observed.available_ceiling_bytes == 128 * kMiB,
              "disabled local controller must retain inherited parent clamp") && ok;
  write(leaf / "memory.max", "max\n");
  write(leaf / "memory.high", "max\n");
  for (const std::string membership : {"0::/tenant/../server\n", "0::/tenant/server\n0::/\n",
                                       "0::relative\n", "5::/tenant\n", "0:cpu:/tenant\n",
                                       "garbage\n"}) {
    write(paths.proc_self_cgroup, membership);
    observed = memory::ProbeHostContainerMemoryCeilings(paths);
    ok = Expect(observed.container_limit_incomplete,
                "ambiguous or escaping membership must refuse complete discovery") && ok;
  }
  write(paths.proc_self_cgroup, "0::/tenant/server\n");
  write(paths.proc_self_mountinfo, "37 28 0:31 /tenant " + escaped_mount +
      " rw - cgroup2 cgroup2 rw\n");
  observed = memory::ProbeHostContainerMemoryCeilings(paths);
  ok = Expect(observed.container_limit_incomplete,
              "hidden ancestors cannot be certified by a subtree mount") && ok;
  write(paths.proc_self_mountinfo, mount_line + mount_line);
  observed = memory::ProbeHostContainerMemoryCeilings(paths);
  ok = Expect(observed.container_limit_incomplete,
              "ambiguous mounts cannot certify the selected hierarchy") && ok;
  write(paths.proc_self_mountinfo, "37 28 0:31 / " + escaped_mount +
      "/ rw - cgroup2 cgroup2 rw\n");
  observed = memory::ProbeHostContainerMemoryCeilings(paths);
  ok = Expect(observed.container_limit_incomplete,
              "noncanonical mount boundary must terminate with incomplete evidence") && ok;
  write(paths.proc_self_mountinfo, mount_line);
  write(paths.proc_self_cgroup, "0::/\n");
  observed = memory::ProbeHostContainerMemoryCeilings(paths);
  ok = Expect(!observed.container_limit_incomplete,
              "global-root membership must terminate at mount root") && ok;
  write(paths.proc_self_cgroup, "5:memory:/tenant\n");
  write(paths.proc_self_mountinfo, "37 28 0:31 / /other rw - tmpfs tmpfs rw\n");
  observed = memory::ProbeHostContainerMemoryCeilings(paths);
  ok = Expect(observed.container_limit_incomplete,
              "unimplemented applicable v1 limits are unknown not unsupported") && ok;
  write(paths.proc_self_cgroup, "5:cpu:/tenant\n");
  observed = memory::ProbeHostContainerMemoryCeilings(paths);
  ok = Expect(!observed.container_limit_incomplete,
              "demonstrably inapplicable memory hierarchy is not unknown") && ok;
  write(paths.proc_self_mountinfo, "");
  observed = memory::ProbeHostContainerMemoryCeilings(paths);
  ok = Expect(observed.container_limit_incomplete,
              "empty mount data cannot prove the controller inapplicable") && ok;
  write(paths.proc_self_mountinfo, mount_line);
  write(paths.proc_self_cgroup, std::string(1024 * 1024 + 1, 'x'));
  observed = memory::ProbeHostContainerMemoryCeilings(paths);
  ok = Expect(observed.container_limit_incomplete,
              "oversized discovery source must fail boundedly") && ok;
  std::filesystem::remove(paths.proc_self_cgroup);
  observed = memory::ProbeHostContainerMemoryCeilings(paths);
  ok = Expect(observed.container_limit_incomplete,
              "unavailable membership must not certify host fallback") && ok;
  return ok;
#else
  return true;
#endif
}

bool ZeroCgroupCeilingRemainsRestrictive() {
#if defined(__linux__)
  const auto root = TempRoot() / "zero_ceiling_probe";
  std::filesystem::create_directories(root);
  memory::PlatformMemoryCeilingProbePaths paths;
  paths.cgroup_v2_root = root.string();
  paths.proc_meminfo = (root / "meminfo").string();
  { std::ofstream out(paths.proc_meminfo); out << "MemTotal: 524288 kB\n"; }
  bool ok = true;
  for (const auto zero_file : {"memory.max", "memory.high"}) {
    for (const auto name : {"memory.max", "memory.high"}) {
      std::ofstream out(root / name);
      out << (std::string_view(name) == zero_file ? "0\n" : "max\n");
    }
    const auto ceilings = memory::ProbeHostContainerMemoryCeilings(paths);
    ok = Expect(ceilings.available_ceiling_bytes == 0,
                "zero cgroup ceiling must not fall back to host memory") && ok;
    for (const auto& signal : ceilings.signals) {
      if (signal.source == (root / zero_file).string()) {
        ok = Expect(signal.available && signal.valid && signal.finite && signal.bytes == 0,
                    "zero cgroup signal must remain available valid and finite") && ok;
      }
    }
    for (bool required : {false, true}) {
      memory::MemoryPolicyConfig config;
      config.hard_limit_bytes = memory::kMinimumProductionMemoryHardLimitBytes;
      config.soft_limit_bytes = config.per_context_limit_bytes = config.page_buffer_pool_limit_bytes = 0;
      config.platform_probe_paths = paths;
      config.require_platform_memory_ceiling = required;
      const auto resolved = memory::ResolveMemoryPolicyConfig(config);
      ok = Expect(!resolved.ok() && resolved.effective_hard_limit_bytes == 0,
                  "actual zero cgroup limit must prevent policy admission in either mode") && ok;
      ok = Expect(HasDiagnostic(resolved, "MEMORY.POLICY_HARD_LIMIT_TOO_SMALL"),
                  "actual zero cgroup limit must reach effective minimum validation") && ok;
      ok = Expect(!HasDiagnostic(resolved, "MEMORY.POLICY_INVALID_CEILING_VALUE"),
                  "zero is not malformed cgroup data") && ok;
    }
  }
  return ok;
#else
  return true;  // Linux cgroup probe is not compiled on other platforms.
#endif
}

bool CgroupNumericParsingPreservesSignalClassification() {
#if defined(__linux__)
  const auto root = TempRoot() / "numeric_ceiling_probe";
  std::filesystem::create_directories(root);
  memory::PlatformMemoryCeilingProbePaths paths;
  paths.cgroup_v2_root = root.string();
  paths.proc_meminfo = (root / "meminfo").string();
  { std::ofstream out(root / "memory.high"); out << "max\n"; }
  // A real host total of zero remains invalid even though cgroup zero is valid.
  { std::ofstream out(paths.proc_meminfo); out << "MemTotal: 0 kB\n"; }
  struct Case { const char* text; bool valid; bool finite; std::uint64_t bytes; };
  const Case cases[] = {
      {"0", true, true, 0}, {" 0\t", true, true, 0}, {"1", true, true, 1},
      {"18446744073709551615", true, true, UINT64_MAX}, {"max", true, false, 0},
      {"", false, false, 0}, {"-1", false, false, 0}, {"+0", false, false, 0},
      {"0 garbage", false, false, 0}, {"1.5", false, false, 0},
      {"18446744073709551616", false, false, 0}, {"MAX", false, false, 0}};
  bool ok = true;
  for (const auto& item : cases) {
    { std::ofstream out(root / "memory.max"); out << item.text << '\n'; }
    const auto ceilings = memory::ProbeHostContainerMemoryCeilings(paths);
    bool saw_limit = false, saw_memtotal = false;
    for (const auto& signal : ceilings.signals) {
      if (signal.kind == memory::MemoryCeilingSignalKind::cgroup_v2_memory_max) {
        saw_limit = true;
        ok = Expect(signal.available && signal.valid == item.valid &&
                        signal.finite == item.finite && signal.bytes == item.bytes,
                    "cgroup numeric signal classification mismatch") && ok;
      }
      if (signal.kind == memory::MemoryCeilingSignalKind::proc_meminfo_memtotal) {
        saw_memtotal = true;
        ok = Expect(signal.available && !signal.valid && !signal.finite,
                    "zero MemTotal must remain invalid") && ok;
      }
    }
    ok = Expect(saw_limit && saw_memtotal, "probe omitted required signal evidence") && ok;
  }
  return ok;
#else
  return true;
#endif
}

bool IncompleteContainerAdmissionRequiresExplicitCap() {
  memory::MemoryPolicyConfig config;
  config.hard_limit_bytes = 256ull * kMiB;
  config.soft_limit_bytes = config.per_context_limit_bytes = config.page_buffer_pool_limit_bytes = 0;
  config.platform_ceiling_override = DirectCeilings(128ull * kMiB, 128ull * kMiB, 512ull * kMiB);
  config.platform_ceiling_override->container_limit_incomplete = true;
  bool ok = true;
  for (bool enabled : {false, true}) {
    for (bool strict : {false, true}) {
      for (const auto cap : {std::uint64_t{0}, std::uint64_t{64} * kMiB, std::uint64_t{192} * kMiB}) {
        config.allow_degraded_container_limit = enabled;
        config.require_platform_memory_ceiling = strict;
        config.degraded_container_cap_bytes = cap;
        const auto result = memory::ResolveMemoryPolicyConfig(config);
        const bool accepted = enabled && !strict && cap != 0;
        ok = Expect(result.ok() == accepted, "incomplete container admission mismatch") && ok;
        ok = Expect(result.ceiling_evidence.container_limit_incomplete,
                    "degraded resolution erased incomplete evidence") && ok;
        ok = Expect(result.degraded_container_limit == accepted &&
                        result.warnings.size() == (accepted ? 1u : 0u),
                    "degraded result/warning must only accompany accepted policy") && ok;
        if (accepted) {
          const auto expected = cap < 128ull * kMiB ? cap : 128ull * kMiB;
          ok = Expect(result.policy.hard_limit_bytes == expected &&
                          result.warnings.front().diagnostic_code == "MEMORY.CONTAINER_LIMIT_DEGRADED",
                      "degraded policy must clamp and retain its warning") && ok;
          ok = Expect(result.warnings.front().status.severity ==
                          scratchbird::core::platform::Severity::warning,
                      "degraded outcome must retain warning severity") && ok;
        } else {
          ok = Expect(HasDiagnostic(result, "MEMORY.CONTAINER_LIMIT_UNVERIFIED"),
                      "unverified container refusal missing") && ok;
        }
      }
    }
  }
  config.allow_degraded_container_limit = true;
  config.require_platform_memory_ceiling = false;
  config.degraded_container_cap_bytes = 1;
  const auto tiny = memory::ResolveMemoryPolicyConfig(config);
  ok = Expect(!tiny.ok() && tiny.warnings.empty() && !tiny.degraded_container_limit,
              "degraded cap cannot waive minimum budget") && ok;
  config.degraded_container_cap_bytes = 64ull * kMiB;
  config.platform_ceiling_override->signals[0].bytes = 0;
  const auto zero = memory::ResolveMemoryPolicyConfig(config);
  ok = Expect(!zero.ok() && zero.effective_hard_limit_bytes == 0 && zero.warnings.empty(),
              "degraded cap cannot raise a known zero ceiling") && ok;
  config.platform_ceiling_override->signals[0].bytes = 128ull * kMiB;
  config.platform_ceiling_override->container_limit_incomplete = false;
  config.platform_ceiling_override->signals[0].valid = false;
  const auto malformed = memory::ResolveMemoryPolicyConfig(config);
  ok = Expect(malformed.ok() && malformed.degraded_container_limit,
              "known malformed container signal must use explicit degraded mode") && ok;
  const auto parsed = Load("degraded_fields.conf",
      "hard_limit_bytes = 1073741824\nsoft_limit_bytes = 0\n"
      "per_context_limit_bytes = 0\npage_buffer_pool_limit_bytes = 0\n"
      "enable_platform_memory_probe = false\nallow_degraded_container_limit = true\n"
      "degraded_container_cap_bytes = 67108864\n");
  ok = Expect(parsed.ok() && parsed.config.memory_allow_degraded_container_limit &&
                  parsed.config.memory_degraded_container_cap_bytes == 64ull * kMiB,
              "explicit degraded settings must survive config loading") && ok;
  return ok;
}

bool StartupFloorUsesEffectivePlatformLimit() {
#if defined(__linux__)
  // Isolate the real platform clamp: no parent/host limit changes and no memory
  // exhaustion. The child only parses a small configuration under this ceiling.
  const auto child = ::fork();
  if (child < 0) return Expect(false, "startup floor fork failed");
  if (child == 0) {
    struct rlimit limit {};
    if (::getrlimit(RLIMIT_AS, &limit) != 0) ::_exit(2);
    const auto ceiling = static_cast<rlim_t>(2048ull * kMiB);
    if (limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur > ceiling)
      limit.rlim_cur = ceiling;
    if (::setrlimit(RLIMIT_AS, &limit) != 0) ::_exit(3);
    const auto loaded = Load("effective_startup_floor.conf",
        "hard_limit_bytes = 4294967296\n"
        "soft_limit_bytes = 0\n"
        "per_context_limit_bytes = 0\n"
        "page_buffer_pool_limit_bytes = 0\n"
        "min_startup_available_bytes = 3221225472\n"
        "enable_platform_memory_probe = true\n"
        "require_platform_memory_ceiling = false\n");
    bool ok = Expect(!loaded.ok(), "platform clamp below startup floor must refuse") &&
        Expect(HasDiagnostic(loaded, "CONFIG.MEMORY_POLICY_MIN_STARTUP_UNAVAILABLE"),
               "effective startup-floor diagnostic missing");
    const auto detected = memory::ProbeHostContainerMemoryCeilings().available_ceiling_bytes;
    if (!detected || *detected < memory::kMinimumProductionMemoryHardLimitBytes ||
        *detected > 2048ull * kMiB) ::_exit(4);
    for (const auto& diagnostic : loaded.diagnostics) {
      if (diagnostic.code != "CONFIG.MEMORY_POLICY_MIN_STARTUP_UNAVAILABLE") continue;
      bool configured = false, effective = false, floor = false;
      for (const auto& field : diagnostic.fields) {
        configured |= field.key == "hard_limit_bytes" && field.value == "4294967296";
        effective |= field.key == "effective_hard_limit_bytes" && field.value == std::to_string(*detected);
        floor |= field.key == "min_startup_available_bytes" && field.value == "3221225472";
      }
      ok = Expect(configured && effective && floor,
                  "startup floor diagnostic must distinguish requested and effective limits") && ok;
    }
    for (const auto floor : {*detected - 1, *detected, *detected + 1}) {
      const auto boundary = Load("effective_startup_boundary.conf",
          "hard_limit_bytes = 4294967296\n"
          "soft_limit_bytes = 0\nper_context_limit_bytes = 0\n"
          "page_buffer_pool_limit_bytes = 0\n"
          "enable_platform_memory_probe = true\nrequire_platform_memory_ceiling = false\n"
          "min_startup_available_bytes = " + std::to_string(floor) + "\n");
      ok = Expect(boundary.ok() == (floor <= *detected),
                  "effective startup-floor equality boundary mismatch") && ok;
    }
    ::_exit(ok ? 0 : 1);
  }
  int status = 0;
  pid_t waited;
  do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  return Expect(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "limited startup-floor child failed");
#else
  return true;
#endif
}

bool EffectiveHardPreservesProductionMinimum() {
  bool ok = true;
  const auto minimum = memory::kMinimumProductionMemoryHardLimitBytes;
  for (bool required : {false, true}) {
    for (const auto ceiling : {std::uint64_t{0}, std::uint64_t{1}, minimum - 1, minimum}) {
      memory::MemoryPolicyConfig config;
      config.hard_limit_bytes = minimum;
      config.soft_limit_bytes = 0;
      config.per_context_limit_bytes = 0;
      config.page_buffer_pool_limit_bytes = 0;
      config.require_platform_memory_ceiling = required;
      config.platform_ceiling_override = DirectCeilings(ceiling, ceiling, 512ull * kMiB);
      const auto resolved = memory::ResolveMemoryPolicyConfig(config);
      ok = Expect(resolved.platform_ceiling_bytes == ceiling,
                  "finite ceiling, including zero, must remain observable") && ok;
      ok = Expect(resolved.effective_hard_limit_bytes == ceiling,
                  "refusal must preserve the actual effective ceiling") && ok;
      if (ceiling < minimum) {
        ok = Expect(!resolved.ok(), "subminimum effective policy must refuse") && ok;
        ok = Expect(HasDiagnostic(resolved, "MEMORY.POLICY_HARD_LIMIT_TOO_SMALL"),
                    "effective minimum diagnostic missing") && ok;
      } else {
        ok = Expect(resolved.ok(), "exact minimum effective policy must succeed") && ok;
        ok = Expect(resolved.policy.byte_limit == minimum &&
                        resolved.policy.hard_limit_bytes == minimum,
                    "accepted policy must retain the finite minimum") && ok;
      }
    }
  }
  return ok;
}

bool DegradedStartupWarningDelivery() {
#if defined(__linux__)
  const auto child = ::fork();
  if (child < 0) return Expect(false, "warning startup fork failed");
  if (child == 0) {
    const auto root = TempRoot() / "warning_startup";
    std::filesystem::create_directories(root);
    { std::ofstream out(root / "memory.max"); out << 512 * kMiB << '\n'; }
    { std::ofstream out(root / "meminfo"); out << "MemTotal: 524288 kB\n"; }
    // Missing memory.high is an actual incomplete file observation, not an
    // injected accepted policy or a fabricated server warning.
    server::ServerConfigResolutionContext context;
    context.current_directory = root;
    context.include_system_paths = false;
    context.memory_probe_paths.cgroup_v2_root = root.string();
    context.memory_probe_paths.proc_meminfo = (root / "meminfo").string();
    server::ServerCliOptions cli;
    cli.validate_config = true;
    cli.config_path = WriteConfig("warning_startup_refused.conf",
        "hard_limit_bytes = 268435456\nsoft_limit_bytes = 0\n"
        "per_context_limit_bytes = 0\npage_buffer_pool_limit_bytes = 0\n"
        "min_startup_available_bytes = 16777216\n").string();
    const auto refused = server::RunServerStartup(cli, context);
    bool refused_unverified = false;
    for (const auto& diagnostic : refused.diagnostics) {
      refused_unverified |= diagnostic.code == "MEMORY.CONTAINER_LIMIT_UNVERIFIED";
    }
    bool ok = Expect(refused.exit_code == 2 && refused_unverified,
                     "actual startup must refuse incomplete discovery without opt-in");
    cli.config_path = WriteConfig("warning_startup_insufficient_reserve.conf",
        "hard_limit_bytes = 1073741824\nsoft_limit_bytes = 0\n"
        "per_context_limit_bytes = 0\npage_buffer_pool_limit_bytes = 0\n"
        "min_startup_available_bytes = 16777216\n"
        "allow_degraded_container_limit = true\ndegraded_container_cap_bytes = 67108864\n").string();
    const auto insufficient = server::RunServerStartup(cli, context);
    ok = Expect(insufficient.exit_code == 2 && !insufficient.emergency_reserve &&
                    insufficient.diagnostics.size() == 1 &&
                    insufficient.diagnostics.front().code == "MEMORY.EMERGENCY_RESERVE_INVALID",
                "degraded startup cannot waive the default emergency reserve") && ok;
    for (const auto& diagnostic : insufficient.diagnostics) {
      ok = Expect(diagnostic.code != "MEMORY.CONTAINER_LIMIT_DEGRADED",
                  "failed reserve admission must not emit a degraded-success warning") && ok;
    }
    cli.config_path = WriteConfig("warning_startup.conf",
        "hard_limit_bytes = 1073741824\nsoft_limit_bytes = 0\n"
        "per_context_limit_bytes = 0\npage_buffer_pool_limit_bytes = 0\n"
        "min_startup_available_bytes = 16777216\npolicy_generation = 7\n"
        "allow_degraded_container_limit = true\ndegraded_container_cap_bytes = 268435456\n").string();
    auto startup = server::RunServerStartup(cli, context);
    ok = Expect(startup.exit_code == 0 && startup.diagnostics.size() == 1,
                "real degraded startup must retain exactly one warning") && ok;
    ok = Expect(startup.emergency_reserve &&
                    startup.emergency_reserve->Snapshot().available_bytes == 128 * kMiB &&
                    memory::DefaultMemoryManager().Snapshot().current_bytes == 128 * kMiB,
                "startup must own real emergency backing before reporting readiness") && ok;
    unsigned binary_charges = 0;
    for (const auto& scope : memory::DefaultMemoryManager().Snapshot().contexts) {
      if (scope.current_bytes == 128 * kMiB && scope.binary_scope && scope.scope_id.empty() &&
          memory::MemorySystemUuidValid(scope.binary_scope->uuid)) ++binary_charges;
    }
    ok = Expect(binary_charges == 2, "startup backing must retain native owner/context identities") && ok;
    const auto competing = memory::DefaultMemoryManager().AllocateScoped(
        128 * kMiB + 1, alignof(std::max_align_t), {});
    ok = Expect(!competing.ok(), "ordinary allocation cannot spend bootstrap reserve backing") && ok;
    if (startup.diagnostics.size() == 1) {
      const auto& warning = startup.diagnostics.front();
      ok = Expect(warning.code == "MEMORY.CONTAINER_LIMIT_DEGRADED" &&
                      warning.severity == server::ServerDiagnosticSeverity::kWarning &&
                      warning.native_platform_source.has_value(),
                  "startup must preserve native warning severity and evidence") && ok;
      for (const auto& expected : std::vector<std::pair<std::string, std::string>>{
               {"memory_generation", "7"}, {"configured_hard_limit_bytes", "1073741824"},
               {"degraded_cap_bytes", "268435456"}, {"known_ceiling_bytes", "536870912"},
               {"effective_hard_limit_bytes", "268435456"}}) {
        bool found = false;
        for (const auto& field : warning.fields) {
          found |= field.key == expected.first && field.value == expected.second;
        }
        ok = Expect(found, "startup warning changed the selected budget evidence") && ok;
      }
    }
    std::ostringstream output;
    ok = Expect(server::WriteServerStartupDiagnostics(startup, output),
                "healthy warning channel must succeed") && ok;
    const auto rendered = output.str();
    for (const auto* field : {"MEMORY.CONTAINER_LIMIT_DEGRADED", "memory_generation",
                              "configured_hard_limit_bytes", "degraded_cap_bytes",
                              "known_ceiling_bytes", "effective_hard_limit_bytes",
                              "applicable_container_limit_incomplete"}) {
      ok = Expect(rendered.find(field) != std::string::npos,
                  "operator warning omitted required public evidence") && ok;
    }
    ok = Expect(rendered.find(root.string()) == std::string::npos,
                "public warning leaked a protected discovery path") && ok;
    struct FailedWrite : std::streambuf {
      std::streamsize xsputn(const char*, std::streamsize) override { return 0; }
      int_type overflow(int_type) override { return traits_type::eof(); }
    } failed_write;
    std::ostream broken(&failed_write);
    ok = Expect(!server::WriteServerStartupDiagnostics(startup, broken),
                "failed warning write must prohibit admission") && ok;
    broken.clear();
    broken.exceptions(std::ios::badbit | std::ios::failbit);
    ok = Expect(!server::WriteServerStartupDiagnostics(startup, broken),
                "throwing warning channel must prohibit admission") && ok;
    struct FailedFlush : std::stringbuf { int sync() override { return -1; } } failed_flush;
    std::ostream unflushed(&failed_flush);
    ok = Expect(!server::WriteServerStartupDiagnostics(startup, unflushed) &&
                    !failed_flush.str().empty(),
                "buffered warning with failed flush must prohibit admission") && ok;
    std::ofstream full_device("/dev/full");
    ok = Expect(full_device.is_open() &&
                    !server::WriteServerStartupDiagnostics(startup, full_device),
                "real OS channel write failure must prohibit admission") && ok;
    startup.emergency_reserve.reset();
    ok = Expect(memory::DefaultMemoryManager().Snapshot().current_bytes == 0,
                "startup owner teardown must release real reserve charges") && ok;
    ::_exit(ok ? 0 : 2);
  }
  int status = 0;
  pid_t waited;
  do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  return Expect(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "degraded startup warning child failed");
#else
  return true;
#endif
}

bool StartupRechecksEnvelopeAfterReserve(std::uint64_t final_limit, bool pinned_budget) {
#if defined(__linux__)
  const auto child = ::fork();
  if (child < 0) return Expect(false, "activation recheck fork failed");
  if (child == 0) {
    ::alarm(20);  // Bound a fixture handshake failure; never change host limits.
    const auto root = TempRoot() / "activation_recheck";
    std::filesystem::create_directories(root);
    const auto maximum = root / "memory.max";
    const auto high = root / "memory.high";
    if (::mkfifo(maximum.c_str(), 0600) || ::mkfifo(high.c_str(), 0600)) ::_exit(1);
    { std::ofstream out(root / "meminfo"); out << "MemTotal: 1048576 kB\n"; }
    server::ServerConfigResolutionContext context;
    context.current_directory = root;
    context.include_system_paths = false;
    context.memory_probe_paths.cgroup_v2_root = root.string();
    context.memory_probe_paths.proc_meminfo = (root / "meminfo").string();
    server::ServerCliOptions cli;
    cli.validate_config = true;
    cli.config_path = WriteConfig("activation_recheck.conf",
        std::string("hard_limit_bytes = ") + (pinned_budget ? "268435456" : "1073741824") +
        "\nsoft_limit_bytes = 0\n"
        "per_context_limit_bytes = 0\npage_buffer_pool_limit_bytes = 0\n"
        "min_startup_available_bytes = 16777216\n").string();
    // The second FIFO makes each complete probe a handshake: its reader is
    // reached only after the preceding maximum reader has closed. No sleeps
    // or timing assumptions are needed to move the limit after installation.
    std::thread source([&] {
      for (const std::uint64_t limit : std::vector<std::uint64_t>{512 * kMiB, 512 * kMiB, final_limit}) {
        { std::ofstream out(maximum); out << limit << '\n'; }
        { std::ofstream out(high); out << "max\n"; }
      }
    });
    auto startup = server::RunServerStartup(cli, context);
    source.join();
    bool changed = false;
    for (const auto& diagnostic : startup.diagnostics) {
      if (final_limit == 0 && diagnostic.severity == server::ServerDiagnosticSeverity::kError &&
          diagnostic.code == "MEMORY.POLICY_HARD_LIMIT_TOO_SMALL") {
        for (const auto& field : diagnostic.fields) {
          changed |= field.key == "value" && field.value == "0";
        }
      }
      for (const auto& field : diagnostic.fields) {
        changed |= diagnostic.code == "MEMORY.EMERGENCY_RESERVE_INVALID" &&
                   field.key == "reason" && field.value == "memory_envelope_changed_during_bootstrap";
      }
    }
    const bool ok = Expect(startup.exit_code == 2 && changed && !startup.serving_requested &&
        startup.stdout_text.empty() && !startup.emergency_reserve &&
        memory::DefaultMemoryManager().Snapshot().current_bytes == 0,
        "changed post-reserve envelope must refuse admission and release backing");
    std::filesystem::remove_all(root);
    ::_exit(ok ? 0 : 1);
  }
  int status = 0;
  pid_t waited;
  do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  return Expect(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "post-reserve activation child failed");
#else
  return true;
#endif
}

bool DefaultBootstrapReserveSizingIsBounded() {
  bool ok = true;
  const std::uint64_t minimum = 128 * kMiB;
  const auto transition = minimum * 100 / 3;
  for (const auto& [budget, expected] : std::vector<std::pair<std::uint64_t, std::uint64_t>>{
           {0, minimum}, {minimum, minimum}, {1024 * kMiB, minimum},
           {transition, minimum}, {transition + 1, minimum + 1},
           {8192 * kMiB, 257698038}, {UINT64_MAX, 1024 * kMiB}}) {
    ok = Expect(memory::DefaultBootstrapEmergencyReserveBytes(budget) == expected,
                "default reserve percentage/minimum/maximum/overflow boundary mismatch") && ok;
  }
  return ok;
}

bool FinalMemoryResolutionRechecksStartupFloor() {
#if defined(__linux__)
  const auto child = ::fork();
  if (child < 0) return Expect(false, "final memory resolution fork failed");
  if (child == 0) {
    const auto loaded = Load("final_startup_floor.conf",
        "hard_limit_bytes = 268435456\nsoft_limit_bytes = 0\n"
        "per_context_limit_bytes = 0\npage_buffer_pool_limit_bytes = 0\n"
        "min_startup_available_bytes = 201326592\n"
        "enable_platform_memory_probe = true\n");
    if (!Expect(loaded.ok(), "initial configuration must meet its startup floor")) ::_exit(2);
    struct rlimit limit {};
    if (::getrlimit(RLIMIT_AS, &limit) != 0) ::_exit(3);
    const auto ceiling = static_cast<rlim_t>(128 * kMiB);
    if (limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur > ceiling) limit.rlim_cur = ceiling;
    if (::setrlimit(RLIMIT_AS, &limit) != 0) ::_exit(4);
    const auto resolved = server::ResolveServerMemoryAllocationPolicy(loaded.config);
    const bool ok = Expect(!resolved.ok() &&
                              HasDiagnostic(resolved, "CONFIG.MEMORY_POLICY_MIN_STARTUP_UNAVAILABLE"),
                          "fresh pre-install resolution must reject a newly unreachable floor") &&
                    Expect(resolved.effective_hard_limit_bytes <= 128 * kMiB,
                           "final resolution must retain the changed platform clamp");
    ::_exit(ok ? 0 : 5);
  }
  int status = 0;
  pid_t waited;
  do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  return Expect(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "final memory startup-floor child failed");
#else
  return true;
#endif
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

bool PackagedResourceRootsResolveBesideExecutable() {
  const auto root = TempRoot() / "packaged-resource-discovery";
  std::filesystem::remove_all(root);
  const auto install_root = root / "opt" / "ScratchBird";
  const auto resource_root = install_root / "share" / "scratchbird" / "resources";
  const auto seed_pack = resource_root / "seed-packs" / "initial-resource-pack";
  const auto policy_pack =
      resource_root / "policy-packs" / "default-local-password";
  std::filesystem::create_directories(seed_pack);
  std::filesystem::create_directories(policy_pack);

  const auto discovered = server::DiscoverServerPackagedResourceRoots(
      install_root / "bin" / "SBsrv", root / "unrelated-working-directory");
  const bool ok =
      Expect(discovered.resource_seed_pack_root == seed_pack,
             "packaged resource seed pack must resolve relative to the executable") &&
      Expect(discovered.policy_seed_pack_root == policy_pack,
             "packaged default policy pack must resolve relative to the executable");
  std::filesystem::remove_all(root);
  return ok;
}

}  // namespace

namespace {
bool OpenSslBudgetConfiguration() {
  SetPolicyRootEnv();
  bool ok = Expect(server::ServerBootstrapConfig{}.memory_openssl_budget_bytes == 4*kMiB,
                   "compiled OpenSSL default must equal packaged default");
  for (const auto bytes : std::array<std::uint64_t,3>{1, 4*kMiB, 8*kMiB}) {
    const auto loaded = Load("crypto-override.conf", "openssl_budget_bytes = " +
        std::to_string(bytes) + "\nenable_platform_memory_probe = false\n");
    ok = Expect(loaded.ok() && loaded.config.memory_openssl_budget_bytes == bytes,
                "explicit OpenSSL override must survive packaged defaults") && ok;
  }
  for (const auto* value : {"-1", "+1", "1.5", "4MiB", "1e6", "18446744073709551616"}) {
    const auto loaded = Load("crypto-invalid.conf", std::string("openssl_budget_bytes = ") + value + "\n");
    ok = Expect(!loaded.ok() && HasDiagnostic(loaded,"CONFIG.VALUE_INVALID_UINT"),
                "malformed OpenSSL byte count must refuse") && ok;
  }
  for (const auto* value : {"0", "1073741824", "18446744073709551615"}) {
    const auto loaded = Load("crypto-size.conf", std::string("openssl_budget_bytes = ") + value +
                            "\nenable_platform_memory_probe = false\n");
    ok = Expect(!loaded.ok() && HasDiagnostic(loaded,"CONFIG.VALUE_INVALID_SIZE"),
                "invalid OpenSSL envelope must refuse without wrapping") && ok;
  }
  server::ServerBootstrapConfig config;
  config.memory_enable_platform_memory_probe = false;
  config.memory_min_startup_available_bytes = 0;
  const auto hard = config.memory_hard_limit_bytes;
  const auto reserve = memory::DefaultBootstrapEmergencyReserveBytes(hard);
  config.memory_openssl_budget_bytes = hard-reserve-1;
  ok = Expect(server::ResolveServerMemoryAllocationPolicy(config).ok(),
              "budget may leave exactly one ordinary byte (not physical grant proof)") && ok;
  config.memory_openssl_budget_bytes++;
  const auto full = server::ResolveServerMemoryAllocationPolicy(config);
  ok = Expect(!full.ok() && HasDiagnostic(full,"CONFIG.VALUE_INVALID_SIZE"),
              "budget must leave ordinary capacity after emergency reserve") && ok;
  bool key = false, reason = false, budget = false, emergency = false, cap = false;
  for (const auto& diagnostic : full.diagnostics) {
    if (diagnostic.diagnostic_code != "CONFIG.VALUE_INVALID_SIZE") continue;
    for (const auto& argument : diagnostic.arguments) {
      const auto* text = argument.text();
      if (!text) continue;
      key |= argument.key == "canonical_key" && *text == "server.memory.openssl_budget_bytes";
      reason |= argument.key == "reason" && *text == "openssl_budget_leaves_no_ordinary_capacity";
      budget |= argument.key == "openssl_budget_bytes" && *text == std::to_string(hard-reserve);
      emergency |= argument.key == "emergency_reserve_bytes" && *text == std::to_string(reserve);
      cap |= argument.key == "effective_hard_limit_bytes" && *text == std::to_string(hard);
    }
  }
  ok = Expect(key && reason && budget && emergency && cap,
              "crypto budget refusal must retain the complete policy-bound vector") && ok;
  // Re-observation of a changed platform cap must invalidate the same budget.
  const auto probe = TempRoot()/"crypto-ceiling";
  std::filesystem::create_directories(probe);
  { std::ofstream(probe/"memory.max") << hard; std::ofstream(probe/"memory.high") << "max"; }
  { std::ofstream(probe/"meminfo") << "MemTotal: 1048576 kB\n"; }
  config.memory_enable_platform_memory_probe = true;
  config.memory_probe_paths.cgroup_v2_root = probe.string();
  config.memory_probe_paths.proc_meminfo = (probe/"meminfo").string();
  config.memory_soft_limit_bytes = config.memory_per_context_limit_bytes =
      config.memory_page_buffer_pool_limit_bytes = 0;
  config.memory_openssl_budget_bytes = 192*kMiB;
  ok = Expect(server::ResolveServerMemoryAllocationPolicy(config).ok(),
              "crypto budget fits initial observed cap") && ok;
  { std::ofstream(probe/"memory.max") << 256*kMiB; }
  const auto clamped = server::ResolveServerMemoryAllocationPolicy(config);
  ok = Expect(!clamped.ok() && HasDiagnostic(clamped,"CONFIG.VALUE_INVALID_SIZE"),
              "changed effective cap must refuse existing crypto budget") && ok;
  return ok;
}

bool OpenSslPackagedBudgetValidation() {
  const auto root = TempRoot()/"crypto-policy-pack";
  std::filesystem::create_directories(root/"policies");
  const auto filename = "server_memory_cache_policy.json";
  std::ifstream source(std::filesystem::path(SB_DEFAULT_POLICY_PACK_ROOT)/"policies"/filename);
  std::ostringstream buffer;
  buffer << source.rdbuf();
  const auto original = buffer.str();
  const std::string field = "\"openssl_budget_bytes\": 4194304";
  const auto offset = original.find(field);
  if (!Expect(source.good() && offset != std::string::npos, "read canonical crypto budget resource"))
    return false;
  bool ok = true;
  for (const auto* value : {"8388608", "0", "1073741824", "-1", "1.5", "4e6",
                             "4194304bytes", "04194304", "18446744073709551616", "\"4194304\""}) {
    auto text = original;
    text.replace(offset, field.size(), std::string("\"openssl_budget_bytes\": ")+value);
    { std::ofstream output(root/"policies"/filename); output << text; }
    const auto loaded = Load("crypto-pack.conf", std::string("enable_platform_memory_probe = false\n")+
        "[server.database]\npolicy_seed_pack_root = "+root.string()+"\n");
    if (std::string_view(value)=="8388608") {
      ok = Expect(loaded.ok() && loaded.config.memory_openssl_budget_bytes == 8*kMiB,
                  "selected pack supplies actual crypto backing value") && ok;
    } else {
      const bool invalid_size = std::string_view(value)=="0" || std::string_view(value)=="1073741824";
      ok = Expect(!loaded.ok() && HasDiagnostic(loaded, invalid_size ?
          "CONFIG.DEFAULT_MEMORY_POLICY_INVALID" : "CONFIG.DEFAULT_MEMORY_POLICY_MALFORMED"),
          "invalid packaged crypto value cannot parse a numeric prefix or use compiled fallback") && ok;
    }
  }
  auto missing = original;
  missing.erase(offset, field.size()+1); // Remove the field and comma, leave valid JSON.
  { std::ofstream output(root/"policies"/filename); output << missing; }
  const auto absent = Load("crypto-pack-missing.conf", std::string("enable_platform_memory_probe = false\n")+
      "[server.database]\npolicy_seed_pack_root = "+root.string()+"\n");
  return Expect(!absent.ok() && HasDiagnostic(absent,"CONFIG.DEFAULT_MEMORY_POLICY_MALFORMED"),
                "missing packaged crypto budget cannot silently fall back") && ok;
}
} // namespace

int main() try {
  bool ok = true;
  ok = OpenSslBudgetConfiguration() && ok;
  ok = OpenSslPackagedBudgetValidation() && ok;
  ok = ValidConfigBuildsAllocationPolicy() && ok;
  ok = DefaultPolicyPackMemoryPolicyLoads() && ok;
  ok = ExplicitConfigOverridesPolicyPackMemoryFields() && ok;
  ok = CoreResolverCarriesProvenanceAndGeneration() && ok;
  ok = PlatformCeilingClampsEffectivePolicy() && ok;
  ok = RequiredCeilingRejectsConfiguredOvercommit() && ok;
  ok = InvalidRequiredCeilingFailsClosed() && ok;
  ok = RequiredUnavailableCeilingFailsClosed() && ok;
  ok = FixturePathProbeIsDeterministic() && ok;
  ok = EffectiveHardBoundsDerivedLimits() && ok;
  ok = EffectiveHardPreservesProductionMinimum() && ok;
  ok = ZeroCgroupCeilingRemainsRestrictive() && ok;
  ok = ProcessCgroupDiscoveryIncludesAncestors() && ok;
  ok = CgroupNumericParsingPreservesSignalClassification() && ok;
  ok = StartupFloorUsesEffectivePlatformLimit() && ok;
  ok = FinalMemoryResolutionRechecksStartupFloor() && ok;
  ok = DegradedStartupWarningDelivery() && ok;
  ok = StartupRechecksEnvelopeAfterReserve(256 * kMiB, false) && ok;
  ok = StartupRechecksEnvelopeAfterReserve(384 * kMiB, true) && ok;
  ok = StartupRechecksEnvelopeAfterReserve(0, false) && ok;
  ok = DefaultBootstrapReserveSizingIsBounded() && ok;
  ok = IncompleteContainerAdmissionRequiresExplicitCap() && ok;
  ok = DefaultManagerInstallsConfiguredPolicyOnce() && ok;
  ok = PackagedResourceRootsResolveBesideExecutable() && ok;
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
  TestArtifacts().Cleanup();
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return EXIT_FAILURE;
}
