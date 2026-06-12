// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory.hpp"
#include "memory_locality_policy.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace mem = scratchbird::core::memory;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool EvidenceHas(const std::vector<std::string>& evidence, std::string_view token) {
  for (const auto& row : evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireEvidence(const std::vector<std::string>& evidence) {
  Require(EvidenceHas(evidence, "MMCH_NUMA_HUGEPAGE_LOCALITY_POLICY"),
          "MMCH-062 evidence marker missing");
  Require(EvidenceHas(
              evidence,
              "memory_locality.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_wal_or_benchmark_authority"),
          "MMCH-062 authority boundary evidence missing");
}

mem::AllocationPolicy Policy() {
  auto policy = mem::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "mmch062_locality_policy";
  policy.hard_limit_bytes = 1024 * 1024;
  policy.soft_limit_bytes = 1024 * 1024;
  policy.per_context_limit_bytes = 1024 * 1024;
  policy.page_buffer_pool_limit_bytes = 1024 * 1024;
  return policy;
}

void CapabilitiesAndFallbackAreExplicit() {
  const auto caps = mem::CurrentMemoryLocalityPlatformCapabilities();
  RequireEvidence(caps.evidence);
  Require(!caps.platform_name.empty(), "MMCH-062 platform name missing");
  Require(caps.page_size_available && caps.page_size_bytes > 0,
          "MMCH-062 page size evidence missing");

  mem::MemoryLocalityPolicy policy;
  policy.numa_mode = mem::MemoryNumaPolicyMode::prefer_node;
  policy.preferred_numa_node = 0;
  policy.huge_page_mode = mem::MemoryHugePagePolicyMode::prefer_huge_pages;
  policy.allow_portable_fallback = true;
  policy.require_platform_evidence = true;
  const auto decision = mem::EvaluateMemoryLocalityPolicy(policy);
  Require(decision.ok(), "MMCH-062 portable fallback locality policy failed");
  RequireEvidence(decision.evidence);
  Require(decision.portable_fallback_used || decision.capabilities.huge_page_hint_supported,
          "MMCH-062 fallback/support evidence missing");
}

void RequiredUnsupportedModesFailClosed() {
  mem::MemoryLocalityPolicy policy;
  policy.huge_page_mode = mem::MemoryHugePagePolicyMode::require_huge_pages;
  const auto decision = mem::EvaluateMemoryLocalityPolicy(policy);
  Require(!decision.ok() && decision.fail_closed,
          "MMCH-062 required huge pages did not fail closed on unsupported require path");
  Require(decision.diagnostic.diagnostic_code ==
              "memory_locality_huge_pages_required_unavailable",
          "MMCH-062 huge page require diagnostic changed");
  RequireEvidence(decision.evidence);

  policy = {};
  policy.numa_mode = mem::MemoryNumaPolicyMode::prefer_node;
  policy.preferred_numa_node = 0;
  policy.allow_portable_fallback = false;
  const auto numa = mem::EvaluateMemoryLocalityPolicy(policy);
  Require(!numa.ok() && numa.fail_closed,
          "MMCH-062 required NUMA support did not fail closed");
  Require(numa.diagnostic.diagnostic_code == "memory_locality_numa_unsupported",
          "MMCH-062 NUMA unsupported diagnostic changed");
}

void ApplyHugePageHintDoesNotLeakMemory() {
  mem::MemoryManager manager(Policy());
  mem::MemoryTag tag;
  tag.purpose = "mmch062_locality_buffer";
  tag.category = mem::MemoryCategory::executor_query_reserved;
  tag.lifetime = mem::MemoryLifetime::temporary;
  tag.owner = "mmch062";
  tag.context_id = "mmch062";
  auto scoped = manager.AllocateScoped(65536, mem::DefaultPageBufferAlignment(), tag);
  Require(scoped.ok(), "MMCH-062 allocation failed");

  mem::MemoryLocalityPolicy policy;
  policy.huge_page_mode = mem::MemoryHugePagePolicyMode::prefer_huge_pages;
  policy.allow_portable_fallback = true;
  const auto applied = mem::ApplyMemoryLocalityPolicy(policy,
                                                      scoped.allocation.data(),
                                                      scoped.allocation.size());
  Require(applied.ok(), "MMCH-062 huge-page hint application failed");
  RequireEvidence(applied.evidence);
  Require(scoped.allocation.Reset().ok(), "MMCH-062 allocation reset failed");
  Require(manager.Snapshot().current_bytes == 0,
          "MMCH-062 locality hint leaked memory");
}

}  // namespace

int main() {
  CapabilitiesAndFallbackAreExplicit();
  RequiredUnsupportedModesFailClosed();
  ApplyHugePageHintDoesNotLeakMemory();
  std::cout << "MMCH-062 authority_note=memory_locality_policy_evidence_only;"
            << " locality_and_hugepage_hints_are_not_finality_visibility_authorization_recovery_or_benchmark_authority\n";
  return EXIT_SUCCESS;
}
