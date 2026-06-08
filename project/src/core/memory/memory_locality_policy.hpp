// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "memory.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::memory {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;
using scratchbird::core::platform::usize;

// MMCH_NUMA_HUGEPAGE_LOCALITY_POLICY
enum class MemoryNumaPolicyMode {
  default_local,
  disabled,
  prefer_node,
  interleave
};

enum class MemoryHugePagePolicyMode {
  default_page_size,
  disabled,
  prefer_huge_pages,
  require_huge_pages
};

struct MemoryLocalityPolicy {
  MemoryNumaPolicyMode numa_mode = MemoryNumaPolicyMode::default_local;
  MemoryHugePagePolicyMode huge_page_mode =
      MemoryHugePagePolicyMode::default_page_size;
  int preferred_numa_node = -1;
  bool allow_portable_fallback = true;
  bool require_platform_evidence = false;
};

struct MemoryLocalityPlatformCapabilities {
  std::string platform_name;
  bool huge_page_hint_supported = false;
  bool huge_page_require_supported = false;
  bool numa_hint_supported = false;
  bool page_size_available = false;
  usize page_size_bytes = 0;
  std::vector<std::string> evidence;
};

struct MemoryLocalityDecision {
  Status status;
  bool fail_closed = false;
  bool portable_fallback_used = false;
  bool huge_page_hint_applied = false;
  bool numa_hint_applied = false;
  MemoryLocalityPlatformCapabilities capabilities;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && !fail_closed;
  }
};

const char* MemoryNumaPolicyModeName(MemoryNumaPolicyMode mode);
const char* MemoryHugePagePolicyModeName(MemoryHugePagePolicyMode mode);
MemoryLocalityPlatformCapabilities CurrentMemoryLocalityPlatformCapabilities();
MemoryLocalityDecision EvaluateMemoryLocalityPolicy(
    const MemoryLocalityPolicy& policy);
MemoryLocalityDecision ApplyMemoryLocalityPolicy(
    const MemoryLocalityPolicy& policy,
    void* pointer,
    usize bytes);

}  // namespace scratchbird::core::memory
