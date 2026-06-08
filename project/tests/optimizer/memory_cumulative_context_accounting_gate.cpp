// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace mem = scratchbird::core::memory;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

mem::AllocationPolicy Policy() {
  mem::AllocationPolicy policy;
  policy.policy_name = "mmch_010_cumulative_context_policy";
  policy.hard_limit_bytes = 4096;
  policy.soft_limit_bytes = 4096;
  policy.per_context_limit_bytes = 100;
  policy.page_buffer_pool_limit_bytes = 4096;
  policy.zero_memory_on_release = true;
  return policy;
}

mem::MemoryTag Tag(std::string_view context,
                   std::string_view owner,
                   std::string_view query) {
  mem::MemoryTag tag;
  tag.purpose = "mmch_010_context_accounting";
  tag.category = mem::MemoryCategory::test_probe;
  tag.lifetime = mem::MemoryLifetime::statement;
  tag.context_id = std::string(context);
  tag.owner = std::string(owner);
  tag.database_id = "db-mmch-010";
  tag.session_id = "session-mmch-010";
  tag.transaction_id = "txn-mmch-010";
  tag.statement_id = std::string(context);
  tag.query_id = std::string(query);
  return tag;
}

const mem::MemoryContextSnapshot* FindContext(
    const mem::MemoryAccountingSnapshot& snapshot,
    std::string_view scope_kind,
    std::string_view scope_id) {
  for (const auto& context : snapshot.contexts) {
    if (context.scope_kind == scope_kind && context.scope_id == scope_id) {
      return &context;
    }
  }
  return nullptr;
}

bool DiagnosticArgEquals(const scratchbird::core::platform::DiagnosticRecord& diagnostic,
                         std::string_view key,
                         std::string_view value) {
  for (const auto& arg : diagnostic.arguments) {
    if (arg.key == key && arg.value == value) {
      return true;
    }
  }
  return false;
}

void RequireContextBytes(const mem::MemoryAccountingSnapshot& snapshot,
                         std::string_view scope_kind,
                         std::string_view scope_id,
                         mem::u64 current,
                         mem::u64 peak,
                         mem::u64 active_count) {
  const auto* context = FindContext(snapshot, scope_kind, scope_id);
  Require(context != nullptr, "MMCH-010 context snapshot missing");
  Require(context->current_bytes == current, "MMCH-010 context current bytes mismatch");
  Require(context->peak_bytes == peak, "MMCH-010 context peak bytes mismatch");
  Require(context->active_allocation_count == active_count,
          "MMCH-010 context active allocation count mismatch");
}

void CumulativeContextLimit() {
  mem::BoundedAllocator allocator(Policy());

  const auto first = allocator.Allocate(60, 0, Tag("stmt-a", "owner-a", "query-a"));
  Require(first.ok(), "MMCH-010 first allocation failed");

  auto snapshot = allocator.Snapshot();
  RequireContextBytes(snapshot, "context", "stmt-a", 60, 60, 1);
  RequireContextBytes(snapshot, "statement", "stmt-a", 60, 60, 1);
  RequireContextBytes(snapshot, "query", "query-a", 60, 60, 1);
  RequireContextBytes(snapshot, "owner", "owner-a", 60, 60, 1);
  RequireContextBytes(snapshot, "database", "db-mmch-010", 60, 60, 1);
  RequireContextBytes(snapshot, "session", "session-mmch-010", 60, 60, 1);
  RequireContextBytes(snapshot, "transaction", "txn-mmch-010", 60, 60, 1);

  const auto refused = allocator.Allocate(50, 0, Tag("stmt-a", "owner-a", "query-a"));
  Require(!refused.ok(), "MMCH-010 cumulative context limit accepted second allocation");
  Require(refused.diagnostic.diagnostic_code == "SB-MEMORY-ALLOC-CONTEXT-LIMIT-EXCEEDED",
          "MMCH-010 diagnostic code changed");
  Require(DiagnosticArgEquals(refused.diagnostic, "scope_kind", "context"),
          "MMCH-010 diagnostic missing context scope kind");
  Require(DiagnosticArgEquals(refused.diagnostic, "scope_id", "stmt-a"),
          "MMCH-010 diagnostic missing context scope id");
  Require(DiagnosticArgEquals(refused.diagnostic, "scope_current_bytes", "60"),
          "MMCH-010 diagnostic missing context current bytes");
  Require(DiagnosticArgEquals(refused.diagnostic, "scope_limit_bytes", "100"),
          "MMCH-010 diagnostic missing context limit bytes");

  snapshot = allocator.Snapshot();
  const auto* context = FindContext(snapshot, "context", "stmt-a");
  Require(context != nullptr && context->failure_count == 1,
          "MMCH-010 context failure count was not recorded");

  const auto released = allocator.Deallocate(first.pointer, Tag("stmt-a", "owner-a", "query-a"));
  Require(released.ok(), "MMCH-010 release failed");
  snapshot = allocator.Snapshot();
  RequireContextBytes(snapshot, "context", "stmt-a", 0, 60, 0);

  const auto after_release = allocator.Allocate(100, 0, Tag("stmt-a", "owner-a", "query-a"));
  Require(after_release.ok(), "MMCH-010 context limit did not release bytes");
  Require(allocator.Deallocate(after_release.pointer, Tag("stmt-a", "owner-a", "query-a")).ok(),
          "MMCH-010 final release failed");
}

void OwnerLimitCannotBeBypassedWithNewContext() {
  mem::BoundedAllocator allocator(Policy());

  const auto first = allocator.Allocate(70, 0, Tag("stmt-b1", "shared-owner", "query-b1"));
  Require(first.ok(), "MMCH-010 owner setup allocation failed");

  const auto refused = allocator.Allocate(40, 0, Tag("stmt-b2", "shared-owner", "query-b2"));
  Require(!refused.ok(), "MMCH-010 owner cumulative limit accepted context bypass");
  Require(DiagnosticArgEquals(refused.diagnostic, "scope_kind", "owner"),
          "MMCH-010 owner diagnostic missing scope kind");
  Require(DiagnosticArgEquals(refused.diagnostic, "scope_id", "shared-owner"),
          "MMCH-010 owner diagnostic missing scope id");

  Require(allocator.Deallocate(first.pointer, Tag("stmt-b1", "shared-owner", "query-b1")).ok(),
          "MMCH-010 owner release failed");
}

}  // namespace

int main() {
  CumulativeContextLimit();
  OwnerLimitCannotBeBypassedWithNewContext();
  return EXIT_SUCCESS;
}
