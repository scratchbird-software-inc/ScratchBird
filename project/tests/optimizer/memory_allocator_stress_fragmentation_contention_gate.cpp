// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace mem = scratchbird::core::memory;
using Clock = std::chrono::steady_clock;

struct LatencyStats {
  std::string name;
  std::vector<std::uint64_t> samples_ns;
};

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

std::uint64_t ElapsedNs(Clock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

void Record(LatencyStats* stats, Clock::time_point start) {
  stats->samples_ns.push_back(ElapsedNs(start));
}

std::uint64_t PercentileNs(std::vector<std::uint64_t> samples, std::size_t percentile) {
  Require(!samples.empty(), "MMCH-012 latency sample set was empty");
  std::sort(samples.begin(), samples.end());
  const std::size_t index = ((samples.size() - 1) * percentile) / 100;
  return samples[index];
}

void ReportAndCheckLatency(const LatencyStats& stats,
                           std::uint64_t p50_limit_us,
                           std::uint64_t p95_limit_us,
                           std::uint64_t p99_limit_us) {
  const auto p50_ns = PercentileNs(stats.samples_ns, 50);
  const auto p95_ns = PercentileNs(stats.samples_ns, 95);
  const auto p99_ns = PercentileNs(stats.samples_ns, 99);
  std::cout << "MMCH-012 latency_evidence_only"
            << " phase=" << stats.name
            << " samples=" << stats.samples_ns.size()
            << " p50_us=" << (p50_ns / 1000)
            << " p95_us=" << (p95_ns / 1000)
            << " p99_us=" << (p99_ns / 1000)
            << '\n';
  Require(p50_ns <= p50_limit_us * 1000, "MMCH-012 p50 allocator latency smoke threshold exceeded");
  Require(p95_ns <= p95_limit_us * 1000, "MMCH-012 p95 allocator latency smoke threshold exceeded");
  Require(p99_ns <= p99_limit_us * 1000, "MMCH-012 p99 allocator latency smoke threshold exceeded");
}

mem::AllocationPolicy Policy(std::string_view name,
                             mem::u64 hard_limit,
                             mem::u64 per_context_limit,
                             mem::u64 page_pool_limit) {
  mem::AllocationPolicy policy;
  policy.policy_name = std::string(name);
  policy.hard_limit_bytes = hard_limit;
  policy.soft_limit_bytes = hard_limit;
  policy.per_context_limit_bytes = per_context_limit;
  policy.page_buffer_pool_limit_bytes = page_pool_limit;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

mem::MemoryTag Tag(std::string_view purpose,
                   std::string_view context,
                   std::string_view owner = "mmch-012-owner") {
  mem::MemoryTag tag;
  tag.purpose = std::string(purpose);
  tag.category = mem::MemoryCategory::test_probe;
  tag.lifetime = mem::MemoryLifetime::temporary;
  tag.owner = std::string(owner);
  tag.context_id = std::string(context);
  tag.database_id = "db-mmch-012";
  tag.session_id = "session-mmch-012";
  tag.transaction_id = "txn-mmch-012";
  tag.statement_id = std::string(context);
  tag.query_id = "query-mmch-012";
  return tag;
}

void RequireCleanTeardown(const mem::MemoryAccountingSnapshot& snapshot,
                          std::string_view phase) {
  if (snapshot.current_bytes != 0 ||
      snapshot.active_allocation_count != 0 ||
      snapshot.leak_candidate_count != 0 ||
      snapshot.page_buffer_current_bytes != 0 ||
      snapshot.arena_current_bytes != 0) {
    std::cerr << "MMCH-012 teardown failed phase=" << phase
              << " current_bytes=" << snapshot.current_bytes
              << " active=" << snapshot.active_allocation_count
              << " leak_candidates=" << snapshot.leak_candidate_count
              << " page_buffer_current=" << snapshot.page_buffer_current_bytes
              << " arena_current=" << snapshot.arena_current_bytes
              << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void ChurnAndFragmentationGate() {
  mem::BoundedAllocator allocator(Policy("mmch_012_churn_fragmentation", 32ull * 1024ull * 1024ull,
                                         32ull * 1024ull * 1024ull, 8ull * 1024ull * 1024ull));
  LatencyStats latency{"churn_fragmentation"};
  const std::vector<mem::usize> sizes = {
      16, 24, 31, 64, 96, 127, 256, 384, 511, 1024, 1536, 2048, 4093, 8191, 12289};
  std::vector<mem::AllocationResult> live;
  live.reserve(96);

  for (std::size_t i = 0; i < 1600; ++i) {
    const auto size = sizes[(i * 7) % sizes.size()];
    const auto start = Clock::now();
    auto allocation = allocator.Allocate(size, 0, Tag("mmch_012_churn", "churn-" + std::to_string(i % 13)));
    Record(&latency, start);
    Require(allocation.ok(), "MMCH-012 churn allocation failed");
    live.push_back(allocation);

    if ((i % 3) == 0 && !live.empty()) {
      const auto index = (i * 11) % live.size();
      const auto release_start = Clock::now();
      Require(allocator.Deallocate(live[index].pointer, Tag("mmch_012_churn", "release")).ok(),
              "MMCH-012 churn release failed");
      Record(&latency, release_start);
      live[index] = live.back();
      live.pop_back();
    }
  }

  for (auto& allocation : live) {
    const auto start = Clock::now();
    Require(allocator.Deallocate(allocation.pointer, Tag("mmch_012_churn", "final-release")).ok(),
            "MMCH-012 final churn release failed");
    Record(&latency, start);
  }
  RequireCleanTeardown(allocator.Snapshot(), "churn_fragmentation");
  ReportAndCheckLatency(latency, 50000, 200000, 500000);
}

void PageBufferAndLimitGate() {
  mem::MemoryManager manager(Policy("mmch_012_page_buffer", 8ull * 1024ull * 1024ull,
                                    8ull * 1024ull * 1024ull, 2ull * 1024ull * 1024ull));
  LatencyStats latency{"page_buffer"};
  std::vector<mem::PageBuffer> buffers;
  for (std::size_t i = 0; i < 4; ++i) {
    mem::PageBufferRequest request;
    request.page_size = 64 * 1024;
    request.page_count = 8;
    request.tag = Tag("mmch_012_page_buffer", "page-buffer-" + std::to_string(i));
    const auto start = Clock::now();
    auto result = manager.AllocatePageBuffer(request);
    Record(&latency, start);
    Require(result.ok(), "MMCH-012 large page-buffer allocation failed");
    buffers.push_back(result.buffer);
  }

  mem::PageBufferRequest refused_request;
  refused_request.page_size = 64 * 1024;
  refused_request.page_count = 1;
  refused_request.tag = Tag("mmch_012_page_buffer_refusal", "page-buffer-refusal");
  const auto refused = manager.AllocatePageBuffer(refused_request);
  Require(!refused.ok(), "MMCH-012 page-buffer pool limit was not deterministic");
  Require(refused.diagnostic.diagnostic_code == "SB-MEMORY-PAGE-BUFFER-POOL-LIMIT-EXCEEDED",
          "MMCH-012 page-buffer refusal diagnostic changed");

  auto snapshot = manager.Snapshot();
  Require(snapshot.page_buffer_current_bytes == 2ull * 1024ull * 1024ull,
          "MMCH-012 page-buffer current bytes mismatch");
  Require(snapshot.policy_rejection_count >= 1, "MMCH-012 page-buffer rejection was not counted");

  for (auto& buffer : buffers) {
    const auto start = Clock::now();
    Require(manager.ReleasePageBuffer(buffer, Tag("mmch_012_page_buffer", "page-buffer-release")).ok(),
            "MMCH-012 page-buffer release failed");
    Record(&latency, start);
  }
  RequireCleanTeardown(manager.Snapshot(), "page_buffer");
  ReportAndCheckLatency(latency, 100000, 300000, 800000);
}

void HighCardinalityPointerMapGate() {
  mem::BoundedAllocator allocator(Policy("mmch_012_high_cardinality", 16ull * 1024ull * 1024ull,
                                         16ull * 1024ull * 1024ull, 4ull * 1024ull * 1024ull));
  LatencyStats latency{"active_pointer_high_cardinality"};
  std::vector<mem::AllocationResult> active;
  active.reserve(2048);

  for (std::size_t i = 0; i < 2048; ++i) {
    const auto size = 32 + ((i * 37) % 224);
    const auto start = Clock::now();
    auto allocation = allocator.Allocate(size, 0, Tag("mmch_012_high_cardinality",
                                                     "active-" + std::to_string(i),
                                                     "owner-" + std::to_string(i)));
    Record(&latency, start);
    Require(allocation.ok(), "MMCH-012 high-cardinality allocation failed");
    active.push_back(allocation);
  }

  auto snapshot = allocator.Snapshot();
  Require(snapshot.active_allocation_count == active.size(),
          "MMCH-012 active pointer high-cardinality count mismatch");
  Require(snapshot.leak_candidate_count == active.size(),
          "MMCH-012 active pointer leak candidate count mismatch before teardown");
  Require(snapshot.contexts.size() >= active.size(),
          "MMCH-012 high-cardinality context accounting missing entries");

  std::reverse(active.begin(), active.end());
  for (auto& allocation : active) {
    const auto start = Clock::now();
    Require(allocator.Deallocate(allocation.pointer, Tag("mmch_012_high_cardinality", "release")).ok(),
            "MMCH-012 high-cardinality release failed");
    Record(&latency, start);
  }
  RequireCleanTeardown(allocator.Snapshot(), "active_pointer_high_cardinality");
  ReportAndCheckLatency(latency, 50000, 200000, 500000);
}

void DeterministicHardLimitGate() {
  mem::MemoryManager manager(Policy("mmch_012_hard_limit", 128 * 1024, 0, 128 * 1024));
  const auto first = manager.Allocate(64 * 1024, 0, Tag("mmch_012_limit", "limit-a"));
  Require(first.ok(), "MMCH-012 hard-limit setup allocation A failed");
  const auto second = manager.Allocate(64 * 1024, 0, Tag("mmch_012_limit", "limit-b"));
  Require(second.ok(), "MMCH-012 hard-limit setup allocation B failed");
  const auto refused = manager.Allocate(1, 0, Tag("mmch_012_limit", "limit-c"));
  Require(!refused.ok(), "MMCH-012 hard limit accepted an over-limit allocation");
  Require(refused.diagnostic.diagnostic_code == "SB-MEMORY-ALLOC-LIMIT-EXCEEDED",
          "MMCH-012 hard-limit refusal diagnostic changed");
  Require(manager.Snapshot().policy_rejection_count >= 1, "MMCH-012 hard-limit rejection was not counted");
  Require(manager.Deallocate(first.pointer, Tag("mmch_012_limit", "limit-a")).ok(),
          "MMCH-012 hard-limit allocation A release failed");
  Require(manager.Deallocate(second.pointer, Tag("mmch_012_limit", "limit-b")).ok(),
          "MMCH-012 hard-limit allocation B release failed");
  RequireCleanTeardown(manager.Snapshot(), "hard_limit");
}

void ContentionGate() {
  constexpr std::size_t kThreads = 8;
  mem::MemoryManager manager(Policy("mmch_012_contention", 64 * 1024, 64 * 1024, 64 * 1024));
  std::atomic<std::size_t> ready{0};
  std::atomic<bool> start{false};
  std::atomic<std::size_t> success_count{0};
  std::atomic<std::size_t> failure_count{0};
  std::mutex latency_mutex;
  LatencyStats latency{"contention"};
  std::vector<mem::AllocationResult> held(kThreads);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (std::size_t i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i] {
      ready.fetch_add(1, std::memory_order_acq_rel);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      const auto alloc_start = Clock::now();
      auto result = manager.Allocate(16 * 1024, 0, Tag("mmch_012_contention",
                                                       "contention-" + std::to_string(i),
                                                       "contention-owner"));
      const auto elapsed = ElapsedNs(alloc_start);
      {
        std::lock_guard<std::mutex> lock(latency_mutex);
        latency.samples_ns.push_back(elapsed);
      }
      if (result.ok()) {
        held[i] = result;
        success_count.fetch_add(1, std::memory_order_acq_rel);
      } else {
        failure_count.fetch_add(1, std::memory_order_acq_rel);
      }
    });
  }

  while (ready.load(std::memory_order_acquire) != kThreads) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  for (auto& thread : threads) {
    thread.join();
  }

  Require(success_count.load() == 4, "MMCH-012 contention hard limit did not admit exactly four allocations");
  Require(failure_count.load() == 4, "MMCH-012 contention hard limit did not reject exactly four allocations");
  auto snapshot = manager.Snapshot();
  Require(snapshot.current_bytes == 64 * 1024, "MMCH-012 contention current bytes exceeded hard limit");
  Require(snapshot.active_allocation_count == 4, "MMCH-012 contention active count mismatch");
  Require(snapshot.policy_rejection_count == 4, "MMCH-012 contention rejection count mismatch");

  for (auto& allocation : held) {
    if (!allocation.ok()) {
      continue;
    }
    const auto release_start = Clock::now();
    Require(manager.Deallocate(allocation.pointer, Tag("mmch_012_contention", "contention-release")).ok(),
            "MMCH-012 contention release failed");
    std::lock_guard<std::mutex> lock(latency_mutex);
    latency.samples_ns.push_back(ElapsedNs(release_start));
  }

  RequireCleanTeardown(manager.Snapshot(), "contention");
  ReportAndCheckLatency(latency, 100000, 500000, 1000000);
}

}  // namespace

int main() {
  std::cout << "MMCH-012 authority_note=memory_metrics_are_allocator_gate_evidence_only;"
               "not_transaction_finality_visibility_security_or_recovery_authority"
            << '\n';
  ChurnAndFragmentationGate();
  PageBufferAndLimitGate();
  HighCardinalityPointerMapGate();
  DeterministicHardLimitGate();
  ContentionGate();
  return EXIT_SUCCESS;
}
