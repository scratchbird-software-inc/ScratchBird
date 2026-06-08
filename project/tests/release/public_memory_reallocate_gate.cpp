// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>

namespace {

namespace memory = scratchbird::core::memory;

bool Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

memory::AllocationPolicy Policy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "public_reallocate_gate";
  policy.byte_limit = 1024 * 1024;
  policy.hard_limit_bytes = policy.byte_limit;
  policy.soft_limit_bytes = policy.byte_limit;
  policy.per_context_limit_bytes = policy.byte_limit;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

memory::MemoryTag Tag(const char* purpose) {
  memory::MemoryTag tag;
  tag.purpose = purpose;
  tag.category = memory::MemoryCategory::test_probe;
  tag.lifetime = memory::MemoryLifetime::temporary;
  tag.owner = "public_release_correctness";
  tag.context_id = "PCR-010";
  return tag;
}

bool UnknownPointerDoesNotPublishReplacement() {
  memory::BoundedAllocator allocator(Policy());
  int stack_value = 0;
  const auto result = allocator.Reallocate(&stack_value, 128, alignof(std::max_align_t), Tag("unknown_pointer"));
  const auto snapshot = allocator.Snapshot();
  return Expect(!result.ok(), "unknown reallocate should fail") &&
         Expect(snapshot.current_bytes == 0, "unknown reallocate must not publish allocation bytes") &&
         Expect(snapshot.active_allocation_count == 0, "unknown reallocate must not leave active allocations") &&
         Expect(snapshot.unknown_pointer_failure_count == 1, "unknown reallocate should record pointer failure");
}

bool OldDeallocationFailureCleansReplacementAccounting() {
  memory::BoundedAllocator allocator(Policy());
  auto original = allocator.Allocate(64, alignof(std::max_align_t), Tag("original"));
  if (!Expect(original.ok(), "initial allocation failed")) {
    return false;
  }
  std::memset(original.pointer, 0x5a, original.bytes);

  memory::MemoryFailureInjectionConfiguration injection;
  injection.test_guard = memory::MakeMemoryFailureInjectionTestGuard();
  injection.fixture_enabled = true;
  injection.fixture_name = "public_reallocate_old_deallocate_failure";
  injection.evidence_note =
      "public_test_evidence_only_not_mga_transaction_finality_visibility_recovery_or_authority";
  memory::MemoryFailureInjectionRule rule;
  rule.rule_id = "fail_first_old_deallocate";
  rule.callsite = "core.memory.deallocate";
  rule.fail_on_matched_sequence = 1;
  injection.rules.push_back(rule);
  const auto enabled = allocator.EnableAllocationFailureInjection(std::move(injection));
  if (!Expect(enabled.ok(), "deallocation failure injection should enable")) {
    allocator.Deallocate(original.pointer, Tag("cleanup_original"));
    return false;
  }

  const auto reallocated =
      allocator.Reallocate(original.pointer, 128, alignof(std::max_align_t), Tag("replacement"));
  const auto after_failure = allocator.Snapshot();
  bool ok = true;
  ok = Expect(!reallocated.ok(), "reallocate should report old deallocation failure") && ok;
  ok = Expect(after_failure.current_bytes == original.bytes,
              "replacement allocation must be removed from current byte accounting") && ok;
  ok = Expect(after_failure.active_allocation_count == 1,
              "replacement allocation must not remain active after old deallocation failure") && ok;

  const auto disabled = allocator.DisableAllocationFailureInjection();
  ok = Expect(disabled.ok(), "deallocation failure injection should disable") && ok;
  const auto cleanup = allocator.Deallocate(original.pointer, Tag("cleanup_original"));
  ok = Expect(cleanup.ok(), "original allocation should remain cleanup-able after failed reallocate") && ok;
  const auto final_snapshot = allocator.Snapshot();
  ok = Expect(final_snapshot.current_bytes == 0, "allocator should return to zero current bytes") && ok;
  ok = Expect(final_snapshot.active_allocation_count == 0, "allocator should return to zero active allocations") && ok;
  return ok;
}

bool ActiveMapValidationFailureCleansReplacementAccounting() {
  memory::BoundedAllocator allocator(Policy());
  auto original = allocator.Allocate(64, alignof(std::max_align_t), Tag("original_for_active_map"));
  if (!Expect(original.ok(), "initial active-map allocation failed")) {
    return false;
  }
  std::memset(original.pointer, 0xa5, original.bytes);

  memory::MemoryFailureInjectionConfiguration injection;
  injection.test_guard = memory::MakeMemoryFailureInjectionTestGuard();
  injection.fixture_enabled = true;
  injection.fixture_name = "public_reallocate_active_map_validation_failure";
  injection.evidence_note =
      "public_test_evidence_only_not_mga_transaction_finality_visibility_recovery_or_authority";
  memory::MemoryFailureInjectionRule rule;
  rule.rule_id = "fail_first_reallocate_active_map_validation";
  rule.callsite = "core.memory.reallocate.active_map_validation";
  rule.fail_on_matched_sequence = 1;
  injection.rules.push_back(rule);
  const auto enabled = allocator.EnableAllocationFailureInjection(std::move(injection));
  if (!Expect(enabled.ok(), "active-map validation failure injection should enable")) {
    allocator.Deallocate(original.pointer, Tag("cleanup_original_active_map"));
    return false;
  }

  const auto reallocated =
      allocator.Reallocate(original.pointer, 128, alignof(std::max_align_t), Tag("active_map_replacement"));
  const auto after_failure = allocator.Snapshot();
  bool ok = true;
  ok = Expect(!reallocated.ok(), "reallocate should fail when replacement active-map validation fails") && ok;
  ok = Expect(after_failure.current_bytes == original.bytes,
              "active-map validation failure must remove replacement byte accounting") && ok;
  ok = Expect(after_failure.active_allocation_count == 1,
              "active-map validation failure must leave only original allocation active") && ok;
  ok = Expect(after_failure.unknown_pointer_failure_count == 1,
              "active-map validation failure should record pointer validation failure") && ok;

  const auto disabled = allocator.DisableAllocationFailureInjection();
  ok = Expect(disabled.ok(), "active-map validation failure injection should disable") && ok;
  const auto cleanup = allocator.Deallocate(original.pointer, Tag("cleanup_original_active_map"));
  ok = Expect(cleanup.ok(), "original allocation should remain cleanup-able after active-map failure") && ok;
  const auto final_snapshot = allocator.Snapshot();
  ok = Expect(final_snapshot.current_bytes == 0, "allocator should return to zero bytes after active-map test") && ok;
  ok = Expect(final_snapshot.active_allocation_count == 0,
              "allocator should return to zero active allocations after active-map test") && ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = UnknownPointerDoesNotPublishReplacement() && ok;
  ok = OldDeallocationFailureCleansReplacementAccounting() && ok;
  ok = ActiveMapValidationFailureCleansReplacementAccounting() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
