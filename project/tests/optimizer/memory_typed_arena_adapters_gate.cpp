// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// MMCH_TYPED_ARENA_ADAPTERS
#include "typed_arena.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace mem = scratchbird::core::memory;
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

mem::AllocationPolicy Policy(std::uint64_t hard_limit = 1024 * 1024) {
  mem::AllocationPolicy policy;
  policy.policy_name = "mmch_060_typed_arena_adapters";
  policy.hard_limit_bytes = hard_limit;
  policy.soft_limit_bytes = hard_limit;
  policy.per_context_limit_bytes = hard_limit;
  policy.page_buffer_pool_limit_bytes = hard_limit;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

mem::MemoryTag Tag(std::string_view purpose = "mmch_060_typed_arena") {
  mem::MemoryTag tag;
  tag.purpose = std::string(purpose);
  tag.category = mem::MemoryCategory::executor_query_reserved;
  tag.lifetime = mem::MemoryLifetime::arena;
  tag.owner = "owner-mmch-060";
  tag.context_id = "stmt-mmch-060";
  tag.database_id = "db-mmch-060";
  tag.session_id = "session-mmch-060";
  tag.transaction_id = "txn-mmch-060";
  tag.statement_id = "stmt-mmch-060";
  tag.query_id = "query-mmch-060";
  tag.callsite = "mmch_060.typed_arena";
  return tag;
}

bool DiagnosticArgEquals(const platform::DiagnosticRecord& diagnostic,
                         std::string_view key,
                         std::string_view value) {
  for (const auto& arg : diagnostic.arguments) {
    if (arg.key == key && arg.value == value) {
      return true;
    }
  }
  return false;
}

struct LifecycleProbe {
  LifecycleProbe(int identifier, std::vector<int>* log) : id(identifier), events(log) {
    events->push_back(100 + id);
  }
  ~LifecycleProbe() {
    events->push_back(200 + id);
  }

  LifecycleProbe(const LifecycleProbe&) = delete;
  LifecycleProbe& operator=(const LifecycleProbe&) = delete;

  int id = 0;
  std::vector<int>* events = nullptr;
};

struct DefaultProbe {
  DefaultProbe() {
    ++constructed;
  }
  ~DefaultProbe() {
    ++destroyed;
  }

  static int constructed;
  static int destroyed;
};

int DefaultProbe::constructed = 0;
int DefaultProbe::destroyed = 0;

void ConstructionDestructionAndReverseResetOrder() {
  mem::MemoryManager manager(Policy());
  std::vector<int> events;

  {
    mem::TypedArena arena(manager.CreateArena(Tag()));
    auto first = arena.Make<LifecycleProbe>(1, &events);
    Require(first.ok(), "MMCH-060 first typed allocation failed");
    auto second = arena.Make<LifecycleProbe>(2, &events);
    Require(second.ok(), "MMCH-060 second typed allocation failed");
    Require(events == std::vector<int>({101, 102}), "MMCH-060 construction order changed");
    Require(manager.Snapshot().active_allocation_count != 0,
            "MMCH-060 typed arena did not use MemoryManager accounting");

    const auto reset = arena.Reset();
    Require(reset.ok(), "MMCH-060 typed arena reset failed");
    Require(events == std::vector<int>({101, 102, 202, 201}),
            "MMCH-060 typed arena destructors did not run in reverse order");
    Require(manager.Snapshot().current_bytes == 0, "MMCH-060 reset leaked current bytes");
    Require(manager.Snapshot().active_allocation_count == 0, "MMCH-060 reset leaked active allocations");
  }

  Require(manager.Snapshot().current_bytes == 0, "MMCH-060 arena destructor leaked after explicit reset");
}

void ArraysVectorsAndMoveOnlyArenaOwnership() {
  DefaultProbe::constructed = 0;
  DefaultProbe::destroyed = 0;
  mem::MemoryManager manager(Policy());

  {
    mem::TypedArena arena(manager.CreateArena(Tag("mmch_060_array_vector")));
    auto array = arena.MakeArray<DefaultProbe>(8);
    Require(array.ok(), "MMCH-060 default array allocation failed");
    Require(array.count == 8, "MMCH-060 array count changed");
    Require(DefaultProbe::constructed == 8, "MMCH-060 array construction count mismatch");

    auto vector_result = arena.MakeVector<LifecycleProbe>(3);
    Require(vector_result.ok(), "MMCH-060 vector allocation failed");
    auto vector = std::move(vector_result.vector);
    std::vector<int> events;
    Require(vector.EmplaceBack(10, &events).ok(), "MMCH-060 vector first emplace failed");
    Require(vector.EmplaceBack(11, &events).ok(), "MMCH-060 vector second emplace failed");
    Require(vector.size() == 2, "MMCH-060 vector size mismatch");
    Require(vector[0].id == 10 && vector[1].id == 11, "MMCH-060 vector contents mismatch");

    mem::TypedArena moved(std::move(arena));
    const auto reset = moved.Reset();
    Require(reset.ok(), "MMCH-060 moved typed arena reset failed");
    Require(DefaultProbe::destroyed == 8, "MMCH-060 array destructors did not run");
    Require(events == std::vector<int>({110, 111, 211, 210}),
            "MMCH-060 vector destructors did not run in reverse order");
  }

  auto snapshot = manager.Snapshot();
  Require(snapshot.current_bytes == 0, "MMCH-060 moved arena leaked current bytes");
  Require(snapshot.active_allocation_count == 0, "MMCH-060 moved arena leaked active allocations");
}

void RefusalDiagnosticsAndLeakFreeTeardown() {
  mem::MemoryManager manager(Policy(256));
  {
    mem::TypedArena arena(manager.CreateArena(Tag("mmch_060_refusal")));
    auto refused = arena.MakeArray<char>(4096);
    Require(!refused.ok(), "MMCH-060 oversized array allocation unexpectedly succeeded");
    Require(refused.fail_closed, "MMCH-060 oversized array did not fail closed");
    Require(refused.status.code == platform::StatusCode::memory_limit_exceeded,
            "MMCH-060 oversized array status changed");
    Require(refused.diagnostic.diagnostic_code == "SB-MEMORY-ALLOC-LIMIT-EXCEEDED" ||
                refused.diagnostic.diagnostic_code == "SB-MEMORY-ALLOC-CONTEXT-LIMIT-EXCEEDED",
            "MMCH-060 oversized array did not surface allocator diagnostic");

    auto vector_result = arena.MakeVector<int>(1);
    Require(vector_result.ok(), "MMCH-060 small vector allocation failed");
    auto vector = std::move(vector_result.vector);
    Require(vector.EmplaceBack(7).ok(), "MMCH-060 int vector emplace failed");
    auto over_capacity = vector.EmplaceBack(8);
    Require(!over_capacity.ok(), "MMCH-060 vector accepted over-capacity emplace");
    Require(over_capacity.fail_closed, "MMCH-060 over-capacity vector did not fail closed");
    Require(over_capacity.diagnostic.diagnostic_code == "SB-TYPED-ARENA-VECTOR-CAPACITY-EXCEEDED",
            "MMCH-060 over-capacity vector diagnostic changed");
    Require(DiagnosticArgEquals(
                over_capacity.diagnostic,
                "authority_scope",
                "typed_arena_evidence_only_not_transaction_finality_visibility_authorization_recovery_parser_donor_wal_or_benchmark_authority"),
            "MMCH-060 typed arena authority boundary diagnostic missing");
  }

  auto snapshot = manager.Snapshot();
  Require(snapshot.current_bytes == 0, "MMCH-060 teardown leaked current bytes after refusal");
  Require(snapshot.active_allocation_count == 0, "MMCH-060 teardown leaked active allocations after refusal");
  Require(snapshot.failure_count >= 1, "MMCH-060 allocator refusal did not record failure evidence");
}

void HighVolumeChurnReset() {
  mem::MemoryManager manager(Policy(4ull * 1024ull * 1024ull));
  for (int round = 0; round < 50; ++round) {
    mem::TypedArena arena(manager.CreateArena(Tag("mmch_060_churn")));
    for (int i = 0; i < 100; ++i) {
      auto value = arena.Make<int>(round * 100 + i);
      Require(value.ok(), "MMCH-060 high-volume typed scalar allocation failed");
      Require(*value.pointer == round * 100 + i, "MMCH-060 high-volume typed scalar value mismatch");
    }
    Require(arena.Reset().ok(), "MMCH-060 high-volume reset failed");
  }
  auto snapshot = manager.Snapshot();
  Require(snapshot.current_bytes == 0, "MMCH-060 high-volume churn leaked current bytes");
  Require(snapshot.active_allocation_count == 0, "MMCH-060 high-volume churn leaked active allocations");
}

}  // namespace

int main() {
  ConstructionDestructionAndReverseResetOrder();
  ArraysVectorsAndMoveOnlyArenaOwnership();
  RefusalDiagnosticsAndLeakFreeTeardown();
  HighVolumeChurnReset();
  return EXIT_SUCCESS;
}
