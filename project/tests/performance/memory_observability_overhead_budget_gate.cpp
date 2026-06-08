// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory_observability_overhead.hpp"
#include "runtime_platform.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace memory = scratchbird::core::memory;
namespace platform = scratchbird::core::platform;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

}  // namespace

int main() {
  // MMCH_MEMORY_OBSERVABILITY_OVERHEAD_BUDGET
  memory::MemorySupportBundleRequest request;
  request.snapshot.current_bytes = 4096;
  request.snapshot.peak_bytes = 8192;
  request.snapshot.allocation_count = 12;
  request.snapshot.deallocation_count = 10;
  request.snapshot.failure_count = 1;
  request.snapshot.contexts.push_back({"query", "query-secret-token", 4096, 8192, 4, 2, 1, 2});
  request.snapshot.categories.push_back({memory::MemoryCategory::executor_query_reserved,
                                         4096, 8192, 4, 2, 1, 2});
  request.diagnostics.push_back(platform::MakeDiagnostic(
      platform::StatusCode::memory_limit_exceeded, platform::Severity::error,
      platform::Subsystem::memory, "SB_MEMORY.TEST_LIMIT",
      "memory.test.limit", {{"secret_seed", "secret-token-123"}},
      {}, "memory_observability_overhead_budget_gate", {}));

  memory::MemoryObservabilityOverheadPolicy policy;
  policy.sample_count = 32;
  policy.p50_budget_microseconds = 100000;
  policy.p95_budget_microseconds = 250000;
  policy.p99_budget_microseconds = 500000;
  policy.sampled_max_top_contexts = 0;
  policy.sampled_max_top_categories = 0;

  auto result = memory::MeasureMemoryObservabilityOverhead(request, policy);
  Require(result.ok(), "observability overhead gate failed");
  Require(result.within_budget, "observability exceeded configured budget");
  Require(result.sampled_mode_exercised, "sampled/reduced mode not exercised");
  Require(result.failure_evidence_preserved_under_sampling,
          "failure evidence hidden by sampled support-bundle mode");
  Require(result.sampled_row_count < result.observed_row_count,
          "sampled mode did not reduce row count");

  std::cout << "MMCH_MEMORY_OBSERVABILITY_OVERHEAD_BUDGET: PASS\n";
  std::cout << "p50_us=" << result.p50_microseconds
            << " p95_us=" << result.p95_microseconds
            << " p99_us=" << result.p99_microseconds << '\n';
  return EXIT_SUCCESS;
}
