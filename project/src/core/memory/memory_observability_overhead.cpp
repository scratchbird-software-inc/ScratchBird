// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory_observability_overhead.hpp"

#include <algorithm>
#include <chrono>

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::memory};
}

Status FailedStatus() {
  return {StatusCode::memory_limit_exceeded, Severity::error, Subsystem::memory};
}

u64 Percentile(const std::vector<u64>& values, u64 percentile) {
  if (values.empty()) {
    return 0;
  }
  const u64 index =
      std::min<u64>(values.size() - 1, (percentile * (values.size() - 1)) / 100);
  return values[static_cast<std::size_t>(index)];
}

}  // namespace

MemoryObservabilityOverheadResult MeasureMemoryObservabilityOverhead(
    const MemorySupportBundleRequest& request,
    const MemoryObservabilityOverheadPolicy& policy) {
  MemoryObservabilityOverheadResult result;
  result.evidence.push_back("MMCH_MEMORY_OBSERVABILITY_OVERHEAD_BUDGET");
  result.evidence.push_back(
      "memory_observability.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_wal_or_benchmark_authority");
  const u64 sample_count = std::max<u64>(1, policy.sample_count);
  std::vector<u64> timings;
  timings.reserve(static_cast<std::size_t>(sample_count));

  for (u64 i = 0; i < sample_count; ++i) {
    auto copy = request;
    const auto start = std::chrono::steady_clock::now();
    auto bundle = BuildMemorySupportBundleEvidence(std::move(copy));
    const auto stop = std::chrono::steady_clock::now();
    const auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(stop - start)
            .count();
    timings.push_back(static_cast<u64>(std::max<long long>(0, micros)));
    result.observed_row_count = std::max<u64>(result.observed_row_count,
                                              bundle.rows.size());
  }

  std::sort(timings.begin(), timings.end());
  result.p50_microseconds = Percentile(timings, 50);
  result.p95_microseconds = Percentile(timings, 95);
  result.p99_microseconds = Percentile(timings, 99);
  result.within_budget =
      result.p50_microseconds <= policy.p50_budget_microseconds &&
      result.p95_microseconds <= policy.p95_budget_microseconds &&
      result.p99_microseconds <= policy.p99_budget_microseconds;

  auto sampled = request;
  result.sampled_max_top_contexts = request.max_top_contexts;
  result.sampled_max_top_categories = request.max_top_categories;
  if (policy.enable_sampling) {
    sampled.max_top_contexts = policy.sampled_max_top_contexts;
    sampled.max_top_categories = policy.sampled_max_top_categories;
    result.sampled_max_top_contexts = policy.sampled_max_top_contexts;
    result.sampled_max_top_categories = policy.sampled_max_top_categories;
    result.sampled_mode_exercised = true;
  }
  auto sampled_bundle = BuildMemorySupportBundleEvidence(std::move(sampled));
  result.sampled_row_count = sampled_bundle.rows.size();
  result.sampled_top_context_count = sampled_bundle.top_context_count;
  result.sampled_top_category_count = sampled_bundle.top_category_count;
  result.sampling_bounds_enforced =
      result.sampled_top_context_count <= result.sampled_max_top_contexts &&
      result.sampled_top_category_count <= result.sampled_max_top_categories &&
      result.sampled_row_count <= result.observed_row_count;
  result.failure_evidence_preserved_under_sampling =
      sampled_bundle.failure_reason_count == request.diagnostics.size();

  result.evidence.push_back("memory_observability.sample_count=" +
                            std::to_string(sample_count));
  result.evidence.push_back("memory_observability.p50_microseconds=" +
                            std::to_string(result.p50_microseconds));
  result.evidence.push_back("memory_observability.p95_microseconds=" +
                            std::to_string(result.p95_microseconds));
  result.evidence.push_back("memory_observability.p99_microseconds=" +
                            std::to_string(result.p99_microseconds));
  result.evidence.push_back("memory_observability.within_budget=" +
                            std::string(result.within_budget ? "true" : "false"));
  result.evidence.push_back(
      "memory_observability.failure_evidence_preserved_under_sampling=" +
      std::string(result.failure_evidence_preserved_under_sampling ? "true"
                                                                   : "false"));
  result.evidence.push_back("memory_observability.sampling_bounds_enforced=" +
                            std::string(result.sampling_bounds_enforced ? "true" : "false"));
  result.evidence.push_back("memory_observability.sampled_top_context_count=" +
                            std::to_string(result.sampled_top_context_count));
  result.evidence.push_back("memory_observability.sampled_top_category_count=" +
                            std::to_string(result.sampled_top_category_count));
  result.evidence.push_back("memory_observability.sampled_max_top_contexts=" +
                            std::to_string(result.sampled_max_top_contexts));
  result.evidence.push_back("memory_observability.sampled_max_top_categories=" +
                            std::to_string(result.sampled_max_top_categories));
  result.status = result.ok() ? OkStatus() : FailedStatus();
  return result;
}

}  // namespace scratchbird::core::memory
