// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-MEMORY-PROBE-ANCHOR
#include "memory.hpp"
#include "transaction_memory_hooks.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using scratchbird::core::memory::AllocationPolicy;
using scratchbird::core::memory::ArenaAllocator;
using scratchbird::core::memory::MemoryCategory;
using scratchbird::core::memory::MemoryCategoryName;
using scratchbird::core::memory::MemoryLifetime;
using scratchbird::core::memory::MemoryManager;
using scratchbird::core::memory::MemoryTag;
using scratchbird::core::memory::PageBufferRequest;
using scratchbird::core::memory::ReservedMemoryCategories;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::u64;
using scratchbird::core::platform::usize;
using scratchbird::transaction::mga::MgaMemoryHooks;

struct Args {
  u64 hard_limit = 65536;
  usize page_size = 16384;
};

bool ParseU64(const std::string& text, u64* value) {
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  *value = static_cast<u64>(parsed);
  return true;
}

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (i + 1 >= argc) {
      return false;
    }
    const std::string value = argv[++i];
    if (key == "--hard-limit") {
      if (!ParseU64(value, &args->hard_limit)) {
        return false;
      }
    } else if (key == "--page-size") {
      u64 parsed = 0;
      if (!ParseU64(value, &parsed)) {
        return false;
      }
      args->page_size = static_cast<usize>(parsed);
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_memory_probe [--hard-limit BYTES] [--page-size BYTES]\n";
    return 2;
  }

  AllocationPolicy policy;
  policy.policy_name = "memory_probe_policy";
  policy.hard_limit_bytes = args.hard_limit;
  policy.soft_limit_bytes = args.hard_limit / 2;
  policy.per_context_limit_bytes = 0;
  policy.page_buffer_pool_limit_bytes = args.hard_limit;
  policy.zero_memory_on_release = true;

  MemoryManager manager(policy);

  auto scoped = manager.AllocateScoped(1024,
                                       64,
                                       MemoryTag{Subsystem::memory,
                                                 "probe_scoped_allocation",
                                                 MemoryCategory::test_probe,
                                                 MemoryLifetime::temporary,
                                                 "sb_memory_probe",
                                                 "probe"});
  if (!scoped.ok()) {
    std::cerr << scoped.diagnostic.diagnostic_code << ":" << scoped.diagnostic.message_key << "\n";
    return 1;
  }

  ArenaAllocator arena = manager.CreateArena(MemoryTag{Subsystem::memory,
                                                       "probe_arena",
                                                       MemoryCategory::catalog_bootstrap,
                                                       MemoryLifetime::arena,
                                                       "sb_memory_probe",
                                                       "arena"});
  const auto arena_alloc = arena.Allocate(2048, 64);
  if (!arena_alloc.ok()) {
    std::cerr << arena_alloc.diagnostic.diagnostic_code << ":" << arena_alloc.diagnostic.message_key << "\n";
    return 1;
  }

  PageBufferRequest page_request;
  page_request.page_size = args.page_size;
  page_request.page_count = 1;
  page_request.tag = MemoryTag{Subsystem::memory,
                               "probe_page_buffer",
                               MemoryCategory::page_buffer,
                               MemoryLifetime::page_buffer,
                               "sb_memory_probe",
                               "page"};
  auto page_buffer = manager.AllocateScopedPageBuffer(page_request);
  if (!page_buffer.ok()) {
    std::cerr << page_buffer.diagnostic.diagnostic_code << ":" << page_buffer.diagnostic.message_key << "\n";
    return 1;
  }

  const auto before_failure = manager.Snapshot();
  auto rejected = manager.Allocate(args.hard_limit + 1,
                                   64,
                                   MemoryTag{Subsystem::memory,
                                             "probe_expected_limit_failure",
                                             MemoryCategory::test_probe,
                                             MemoryLifetime::temporary,
                                             "sb_memory_probe",
                                             "failure"});
  const bool bounded_failure_observed = !rejected.ok() && rejected.diagnostic.diagnostic_code == "SB-MEMORY-ALLOC-LIMIT-EXCEEDED";

  const auto reset = arena.Reset();
  if (!reset.ok()) {
    std::cerr << reset.diagnostic.diagnostic_code << ":" << reset.diagnostic.message_key << "\n";
    return 1;
  }
  page_buffer.buffer.Reset();
  scoped.allocation.Reset();

  const auto after_teardown = manager.Snapshot();

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (bounded_failure_observed ? "true" : "false") << ",\n";
  std::cout << "  \"allocations\": " << after_teardown.allocation_count << ",\n";
  std::cout << "  \"deallocations\": " << after_teardown.deallocation_count << ",\n";
  std::cout << "  \"failures\": " << after_teardown.failure_count << ",\n";
  std::cout << "  \"policy_rejections\": " << after_teardown.policy_rejection_count << ",\n";
  std::cout << "  \"current_bytes\": " << after_teardown.current_bytes << ",\n";
  std::cout << "  \"peak_bytes\": " << before_failure.peak_bytes << ",\n";
  std::cout << "  \"page_buffer_peak_bytes\": " << before_failure.page_buffer_peak_bytes << ",\n";
  std::cout << "  \"arena_peak_bytes\": " << before_failure.arena_peak_bytes << ",\n";
  std::cout << "  \"leak_candidates\": " << after_teardown.leak_candidate_count << ",\n";
  std::cout << "  \"reserved_categories\": " << ReservedMemoryCategories().size() << ",\n";
  std::cout << "  \"mga_memory_hooks\": " << MgaMemoryHooks().size() << ",\n";
  std::cout << "  \"first_reserved_category\": \"" << MemoryCategoryName(ReservedMemoryCategories().front()) << "\"\n";
  std::cout << "}\n";
  return bounded_failure_observed ? 0 : 1;
}
