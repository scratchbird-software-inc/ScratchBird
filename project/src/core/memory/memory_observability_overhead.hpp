// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "memory_support_bundle.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::memory {

using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

// MMCH_MEMORY_OBSERVABILITY_OVERHEAD_BUDGET
struct MemoryObservabilityOverheadPolicy {
  u64 sample_count = 64;
  u64 p50_budget_microseconds = 10000;
  u64 p95_budget_microseconds = 20000;
  u64 p99_budget_microseconds = 40000;
  bool enable_sampling = true;
  u64 sampled_max_top_contexts = 2;
  u64 sampled_max_top_categories = 2;
};

struct MemoryObservabilityOverheadResult {
  Status status;
  bool within_budget = false;
  bool sampled_mode_exercised = false;
  bool failure_evidence_preserved_under_sampling = false;
  bool sampling_bounds_enforced = false;
  u64 p50_microseconds = 0;
  u64 p95_microseconds = 0;
  u64 p99_microseconds = 0;
  u64 observed_row_count = 0;
  u64 sampled_row_count = 0;
  u64 sampled_top_context_count = 0;
  u64 sampled_top_category_count = 0;
  u64 sampled_max_top_contexts = 0;
  u64 sampled_max_top_categories = 0;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && within_budget &&
           failure_evidence_preserved_under_sampling &&
           sampling_bounds_enforced;
  }
};

MemoryObservabilityOverheadResult MeasureMemoryObservabilityOverhead(
    const MemorySupportBundleRequest& request,
    const MemoryObservabilityOverheadPolicy& policy);

}  // namespace scratchbird::core::memory
