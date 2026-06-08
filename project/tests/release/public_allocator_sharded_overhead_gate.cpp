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
#include <string>
#include <string_view>
#include <utility>

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

memory::AllocationPolicy Policy(memory::u64 bytes = 1024 * 1024) {
  memory::AllocationPolicy policy;
  policy.policy_name = "public_allocator_sharded_overhead_gate";
  policy.hard_limit_bytes = bytes;
  policy.soft_limit_bytes = bytes;
  policy.per_context_limit_bytes = bytes;
  policy.page_buffer_pool_limit_bytes = bytes;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

memory::MemoryTag Tag(std::string suffix,
                      memory::MemoryCategory category = memory::MemoryCategory::test_probe,
                      memory::MemoryLifetime lifetime = memory::MemoryLifetime::statement) {
  memory::MemoryTag tag;
  tag.subsystem = scratchbird::core::platform::Subsystem::memory;
  tag.purpose = "public_allocator_sharded_overhead_" + suffix;
  tag.category = category;
  tag.lifetime = lifetime;
  tag.owner = "owner-" + suffix;
  tag.context_id = "context-" + suffix;
  tag.database_id = "database-public";
  tag.session_id = "session-" + suffix;
  tag.transaction_id = "transaction-" + suffix;
  tag.statement_id = "statement-" + suffix;
  tag.query_id = "query-" + suffix;
  return tag;
}

void RequireShardedParity(const memory::MemoryAccountingSnapshot& snapshot,
                          std::string_view phase) {
  Require(snapshot.sharded_accounting_bound,
          "allocator snapshot does not report sharded accounting bound");
  Require(snapshot.active_records_routed_through_sharded_accounting,
          "allocator active records are not routed through sharded accounting");
  Require(snapshot.sharded_accounting_shard_count >= 2,
          "allocator sharded accounting does not expose multiple shards");
  Require(snapshot.sharded_accounting_current_bytes == snapshot.current_bytes,
          "sharded current bytes diverged from allocator current bytes");
  Require(snapshot.sharded_accounting_active_allocation_count ==
              snapshot.active_allocation_count,
          "sharded active allocation count diverged from allocator active count");
  Require(snapshot.sharded_accounting_failed_release_count == 0,
          "sharded accounting recorded a release failure");
  Require(snapshot.resident_committed_bytes == snapshot.current_bytes,
          "resident committed bytes did not mirror allocator current bytes");
  (void)phase;
}

void ShardedSnapshotTracksAllocatorAccountingAndOverhead() {
  memory::BoundedAllocator allocator(Policy());
  auto small = allocator.Allocate(128, alignof(std::max_align_t), Tag("small"));
  Require(small.ok(), "small allocation failed");
  std::memset(small.pointer, 0x11, small.bytes);

  auto page = allocator.Allocate(4096,
                                 memory::DefaultPageBufferAlignment(),
                                 Tag("page",
                                     memory::MemoryCategory::page_buffer,
                                     memory::MemoryLifetime::page_buffer));
  Require(page.ok(), "page-buffer-class allocation failed");
  std::memset(page.pointer, 0x22, page.bytes);

  auto snapshot = allocator.Snapshot();
  RequireShardedParity(snapshot, "after allocations");
  Require(snapshot.current_bytes == small.bytes + page.bytes,
          "allocator current bytes after allocations changed");
  Require(snapshot.page_buffer_current_bytes == page.bytes,
          "page-buffer bytes missing from allocator snapshot");
  Require(snapshot.allocator_metadata_overhead_bytes > 0,
          "allocator metadata overhead was not reported");
  Require(snapshot.retained_slab_bytes == 0,
          "raw allocator should report no retained slab bytes");
  Require(snapshot.internal_fragmentation_bytes == 0,
          "raw allocator should report no internal fragmentation bytes");
  Require(snapshot.external_fragmentation_bytes == 0,
          "external fragmentation should be zero before release");

  auto release_small = allocator.Deallocate(small.pointer, Tag("release-small"));
  Require(release_small.ok(), "small allocation release failed");
  snapshot = allocator.Snapshot();
  RequireShardedParity(snapshot, "after partial release");
  Require(snapshot.current_bytes == page.bytes,
          "allocator current bytes after partial release changed");
  Require(snapshot.external_fragmentation_bytes >= small.bytes,
          "external fragmentation was not reported after partial release");

  auto release_page = allocator.Deallocate(page.pointer, Tag("release-page"));
  Require(release_page.ok(), "page allocation release failed");
  snapshot = allocator.Snapshot();
  RequireShardedParity(snapshot, "after full release");
  Require(snapshot.current_bytes == 0, "allocator current bytes leaked");
  Require(snapshot.active_allocation_count == 0,
          "allocator active allocation count leaked");
  Require(snapshot.allocator_metadata_overhead_bytes == 0,
          "allocator metadata overhead did not return to zero");
}

void ActiveMapFailureDoesNotLeakShardedAccounting() {
  memory::BoundedAllocator allocator(Policy());
  auto original = allocator.Allocate(64, alignof(std::max_align_t), Tag("active-original"));
  Require(original.ok(), "active-map original allocation failed");

  memory::MemoryFailureInjectionConfiguration injection;
  injection.test_guard = memory::MakeMemoryFailureInjectionTestGuard();
  injection.fixture_enabled = true;
  injection.fixture_name = "public_allocator_sharded_active_map_failure";
  injection.evidence_note =
      "public_test_evidence_only_not_transaction_finality_visibility_recovery_or_authority";
  memory::MemoryFailureInjectionRule rule;
  rule.rule_id = "fail_active_map_validation_once";
  rule.callsite = "core.memory.reallocate.active_map_validation";
  rule.fail_on_matched_sequence = 1;
  injection.rules.push_back(rule);
  Require(allocator.EnableAllocationFailureInjection(std::move(injection)).ok(),
          "active-map failure injection did not enable");

  const auto reallocated =
      allocator.Reallocate(original.pointer,
                           128,
                           alignof(std::max_align_t),
                           Tag("active-replacement"));
  Require(!reallocated.ok(),
          "reallocate should fail when active-map validation is injected");
  auto snapshot = allocator.Snapshot();
  RequireShardedParity(snapshot, "after active-map failure");
  Require(snapshot.current_bytes == original.bytes,
          "active-map failure leaked replacement current bytes");
  Require(snapshot.active_allocation_count == 1,
          "active-map failure did not leave exactly one active allocation");
  Require(snapshot.unknown_pointer_failure_count == 1,
          "active-map failure did not record unknown-pointer evidence");

  Require(allocator.DisableAllocationFailureInjection().ok(),
          "active-map failure injection did not disable");
  Require(allocator.Deallocate(original.pointer, Tag("active-cleanup")).ok(),
          "active-map original cleanup failed");
  snapshot = allocator.Snapshot();
  RequireShardedParity(snapshot, "after active-map cleanup");
  Require(snapshot.current_bytes == 0,
          "active-map cleanup leaked allocator current bytes");
  Require(snapshot.sharded_accounting_current_bytes == 0,
          "active-map cleanup leaked sharded current bytes");
}

}  // namespace

int main() {
  ShardedSnapshotTracksAllocatorAccountingAndOverhead();
  ActiveMapFailureDoesNotLeakShardedAccounting();
  std::cout << "PCR-015 allocator sharded accounting and overhead snapshot gate passed\n";
  return EXIT_SUCCESS;
}
