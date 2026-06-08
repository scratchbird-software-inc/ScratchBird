// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory.hpp"
#include "temp_workspace_lifecycle.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace memory = scratchbird::core::memory;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool SanitizerRuntimeAvailable() {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__)
  return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
  return true;
#else
  return false;
#endif
#else
  return false;
#endif
}

memory::MemoryTag TagFor(int thread_id, int iteration) {
  memory::MemoryTag tag;
  tag.category = memory::MemoryCategory::executor_query_reserved;
  tag.lifetime = memory::MemoryLifetime::statement;
  tag.owner = "memory_soak_thread_" + std::to_string(thread_id);
  tag.context_id = "memory_soak_context_" + std::to_string(thread_id);
  tag.session_id = "session-" + std::to_string(thread_id);
  tag.statement_id = "statement-" + std::to_string(iteration);
  tag.query_id = "query-" + std::to_string(thread_id);
  tag.purpose = "MMCH_MEMORY_SANITIZER_SOAK_CONCURRENCY";
  return tag;
}

}  // namespace

int main() {
  // MMCH_MEMORY_SANITIZER_SOAK_CONCURRENCY
  memory::AllocationPolicy policy;
  policy.policy_name = "mmch-soak";
  policy.hard_limit_bytes = 64 * 1024 * 1024;
  policy.soft_limit_bytes = 48 * 1024 * 1024;
  policy.per_context_limit_bytes = 8 * 1024 * 1024;
  policy.page_buffer_pool_limit_bytes = 8 * 1024 * 1024;
  policy.zero_memory_on_release = true;
  memory::MemoryManager manager(policy);

  std::atomic<int> failures{0};
  constexpr int kThreads = 8;
  constexpr int kIterations = 512;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&manager, &failures, t]() {
      for (int i = 0; i < kIterations; ++i) {
        const std::size_t bytes = 128 + ((t + 1) * (i % 31));
        auto scoped = manager.AllocateScoped(bytes, alignof(std::max_align_t),
                                             TagFor(t, i));
        if (!scoped.ok()) {
          ++failures;
          continue;
        }
        if ((i % 17) == 0) {
          auto page = manager.AllocateScopedPageBuffer(
              {4096, 1, 4096, TagFor(t, i)});
          if (!page.ok()) {
            ++failures;
          }
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  Require(failures.load() == 0, "concurrent allocation churn had failures");
  auto snapshot = manager.Snapshot();
  Require(snapshot.current_bytes == 0, "memory leak after concurrent allocation churn");
  Require(snapshot.page_buffer_current_bytes == 0,
          "page-buffer leak after concurrent churn");

  const auto root = std::filesystem::temp_directory_path() /
                    "scratchbird_mmch_soak_workspace";
  std::filesystem::remove_all(root);
  memory::TempWorkspacePolicy temp_policy;
  temp_policy.root_path = root;
  temp_policy.filespace_quota_bytes = 1024 * 1024;
  temp_policy.session_quota_bytes = 1024 * 1024;
  temp_policy.statement_quota_bytes = 1024 * 1024;
  temp_policy.disk_reservation_mode =
      memory::TempWorkspaceDiskReservationMode::logical_quota_only;
  memory::TempWorkspaceLifecycleManager temp_manager(temp_policy);
  memory::TempWorkspaceAllocationRequest request;
  request.bytes = 4096;
  request.owner.temp_object_uuid = "018f4f4c-3333-7333-8333-333333333333";
  request.owner.session_id = "session-soak";
  request.owner.statement_id = "statement-soak";
  request.owner.operation_id = "operation-soak";
  request.purpose = "MMCH_MEMORY_SANITIZER_SOAK_CONCURRENCY";
  auto allocation = temp_manager.AllocateSpillFile(request);
  Require(allocation.ok(), "temp workspace allocation failed during soak gate");
  auto release = temp_manager.CleanupOperation(request.owner.operation_id);
  Require(release.ok(), "temp workspace release failed during soak gate");
  std::filesystem::remove_all(root);

  std::cout << "MMCH_MEMORY_SANITIZER_SOAK_CONCURRENCY: PASS\n";
  std::cout << "sanitizer_runtime_available="
            << (SanitizerRuntimeAvailable() ? "true" : "false") << '\n';
  std::cout << "threads=" << kThreads << " iterations=" << kIterations << '\n';
  return EXIT_SUCCESS;
}
