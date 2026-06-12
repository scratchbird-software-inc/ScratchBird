// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory_locality_policy.hpp"

#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#if defined(__linux__)
#include <sys/mman.h>
#endif
#endif

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr const char* kAuthorityBoundary =
    "memory_locality.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_wal_or_benchmark_authority";

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::memory};
}

Status ErrorStatus() {
  return {StatusCode::memory_invalid_request, Severity::error, Subsystem::memory};
}

std::string PlatformName() {
#if defined(_WIN32)
  return "windows";
#elif defined(__linux__)
  return "linux";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
  return "bsd";
#else
  return "unsupported";
#endif
}

MemoryLocalityDecision FailClosed(const MemoryLocalityPolicy& policy,
                                  const MemoryLocalityPlatformCapabilities& caps,
                                  std::string code,
                                  std::string message,
                                  std::string reason) {
  MemoryLocalityDecision decision;
  decision.status = ErrorStatus();
  decision.fail_closed = true;
  decision.capabilities = caps;
  decision.diagnostic = MakeDiagnostic(StatusCode::memory_invalid_request,
                                       Severity::error,
                                       Subsystem::memory,
                                       std::move(code),
                                       std::move(message),
                                       {{"numa_mode", MemoryNumaPolicyModeName(policy.numa_mode)},
                                        {"huge_page_mode", MemoryHugePagePolicyModeName(policy.huge_page_mode)},
                                        {"platform", caps.platform_name},
                                        {"reason", std::move(reason)}},
                                       {},
                                       "core.memory.locality_policy",
                                       "use portable fallback or disable required locality policy on unsupported platforms");
  decision.evidence.push_back("MMCH_NUMA_HUGEPAGE_LOCALITY_POLICY");
  decision.evidence.push_back(kAuthorityBoundary);
  decision.evidence.push_back("memory_locality.fail_closed=true");
  return decision;
}

void AddCommonEvidence(const MemoryLocalityPolicy& policy,
                       MemoryLocalityDecision* decision) {
  decision->evidence.push_back("MMCH_NUMA_HUGEPAGE_LOCALITY_POLICY");
  decision->evidence.push_back(kAuthorityBoundary);
  decision->evidence.push_back("memory_locality.numa_mode=" +
                               std::string(MemoryNumaPolicyModeName(policy.numa_mode)));
  decision->evidence.push_back("memory_locality.huge_page_mode=" +
                               std::string(MemoryHugePagePolicyModeName(policy.huge_page_mode)));
  decision->evidence.push_back("memory_locality.platform=" +
                               decision->capabilities.platform_name);
  decision->evidence.push_back("memory_locality.portable_fallback_used=" +
                               std::string(decision->portable_fallback_used ? "true" : "false"));
  decision->evidence.push_back("memory_locality.huge_page_hint_applied=" +
                               std::string(decision->huge_page_hint_applied ? "true" : "false"));
  decision->evidence.push_back("memory_locality.numa_hint_applied=" +
                               std::string(decision->numa_hint_applied ? "true" : "false"));
  decision->evidence.insert(decision->evidence.end(),
                            decision->capabilities.evidence.begin(),
                            decision->capabilities.evidence.end());
}

}  // namespace

const char* MemoryNumaPolicyModeName(MemoryNumaPolicyMode mode) {
  switch (mode) {
    case MemoryNumaPolicyMode::default_local:
      return "default_local";
    case MemoryNumaPolicyMode::disabled:
      return "disabled";
    case MemoryNumaPolicyMode::prefer_node:
      return "prefer_node";
    case MemoryNumaPolicyMode::interleave:
      return "interleave";
  }
  return "unknown";
}

const char* MemoryHugePagePolicyModeName(MemoryHugePagePolicyMode mode) {
  switch (mode) {
    case MemoryHugePagePolicyMode::default_page_size:
      return "default_page_size";
    case MemoryHugePagePolicyMode::disabled:
      return "disabled";
    case MemoryHugePagePolicyMode::prefer_huge_pages:
      return "prefer_huge_pages";
    case MemoryHugePagePolicyMode::require_huge_pages:
      return "require_huge_pages";
  }
  return "unknown";
}

MemoryLocalityPlatformCapabilities CurrentMemoryLocalityPlatformCapabilities() {
  MemoryLocalityPlatformCapabilities caps;
  caps.platform_name = PlatformName();
  caps.evidence.push_back("MMCH_NUMA_HUGEPAGE_LOCALITY_POLICY");
  caps.evidence.push_back(kAuthorityBoundary);
#if defined(__linux__) && defined(MADV_HUGEPAGE)
  caps.huge_page_hint_supported = true;
  caps.huge_page_require_supported = false;
  caps.evidence.push_back("memory_locality.huge_page_hint_provider=madvise_madv_hugepage");
#else
  caps.evidence.push_back("memory_locality.huge_page_hint_provider=portable_fallback");
#endif
#if defined(_WIN32)
  SYSTEM_INFO info;
  ::GetSystemInfo(&info);
  caps.page_size_available = info.dwPageSize != 0;
  caps.page_size_bytes = static_cast<usize>(info.dwPageSize);
#elif defined(_SC_PAGESIZE)
  const long page_size = ::sysconf(_SC_PAGESIZE);
  caps.page_size_available = page_size > 0;
  caps.page_size_bytes = page_size > 0 ? static_cast<usize>(page_size) : 0;
#endif
  caps.numa_hint_supported = false;
  caps.evidence.push_back("memory_locality.numa_provider=portable_fallback_no_libnuma_dependency");
  caps.evidence.push_back("memory_locality.page_size_bytes=" +
                          std::to_string(caps.page_size_bytes));
  return caps;
}

MemoryLocalityDecision EvaluateMemoryLocalityPolicy(
    const MemoryLocalityPolicy& policy) {
  const auto caps = CurrentMemoryLocalityPlatformCapabilities();
  if (policy.require_platform_evidence && !caps.page_size_available) {
    return FailClosed(policy, caps, "memory_locality_platform_evidence_missing",
                      "core.memory.locality.platform_evidence_missing",
                      "page_size_evidence_required");
  }
  if (policy.numa_mode == MemoryNumaPolicyMode::prefer_node &&
      policy.preferred_numa_node < 0) {
    return FailClosed(policy, caps, "memory_locality_invalid_numa_node",
                      "core.memory.locality.invalid_numa_node",
                      "preferred_numa_node_required");
  }
  if ((policy.numa_mode == MemoryNumaPolicyMode::prefer_node ||
       policy.numa_mode == MemoryNumaPolicyMode::interleave) &&
      !caps.numa_hint_supported && !policy.allow_portable_fallback) {
    return FailClosed(policy, caps, "memory_locality_numa_unsupported",
                      "core.memory.locality.numa_unsupported",
                      "numa_policy_requires_platform_support");
  }
  if (policy.huge_page_mode == MemoryHugePagePolicyMode::require_huge_pages &&
      !caps.huge_page_require_supported) {
    return FailClosed(policy, caps, "memory_locality_huge_pages_required_unavailable",
                      "core.memory.locality.huge_pages_required_unavailable",
                      "required_huge_pages_unavailable");
  }
  MemoryLocalityDecision decision;
  decision.status = OkStatus();
  decision.capabilities = caps;
  decision.portable_fallback_used =
      ((policy.numa_mode == MemoryNumaPolicyMode::prefer_node ||
        policy.numa_mode == MemoryNumaPolicyMode::interleave) &&
       !caps.numa_hint_supported) ||
      (policy.huge_page_mode == MemoryHugePagePolicyMode::prefer_huge_pages &&
       !caps.huge_page_hint_supported);
  AddCommonEvidence(policy, &decision);
  return decision;
}

MemoryLocalityDecision ApplyMemoryLocalityPolicy(
    const MemoryLocalityPolicy& policy,
    void* pointer,
    usize bytes) {
  auto decision = EvaluateMemoryLocalityPolicy(policy);
  if (!decision.ok()) {
    return decision;
  }
  if (pointer == nullptr || bytes == 0) {
    return FailClosed(policy, decision.capabilities,
                      "memory_locality_invalid_allocation",
                      "core.memory.locality.invalid_allocation",
                      "pointer_and_bytes_required");
  }
#if defined(__linux__) && defined(MADV_HUGEPAGE)
  if (policy.huge_page_mode == MemoryHugePagePolicyMode::prefer_huge_pages) {
    if (::madvise(pointer, bytes, MADV_HUGEPAGE) == 0) {
      decision.huge_page_hint_applied = true;
      decision.portable_fallback_used = false;
    } else {
      decision.portable_fallback_used = true;
    }
  }
#endif
  if (policy.numa_mode == MemoryNumaPolicyMode::prefer_node ||
      policy.numa_mode == MemoryNumaPolicyMode::interleave) {
    decision.portable_fallback_used = !decision.capabilities.numa_hint_supported;
  }
  decision.evidence.clear();
  AddCommonEvidence(policy, &decision);
  return decision;
}

}  // namespace scratchbird::core::memory
