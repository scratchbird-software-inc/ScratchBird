// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "allocator_lifetime_hot_path.hpp"

#include "memory.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace exec = scratchbird::engine::executor;
namespace memory = scratchbird::core::memory;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "ORH-281 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 std::string_view needle) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) {
    return item.find(needle) != std::string::npos;
  });
}

std::string StableHash(std::vector<std::string> rows) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const auto& row : rows) {
    for (const unsigned char ch : row) {
      hash ^= ch;
      hash *= 1099511628211ull;
    }
    hash ^= 0xffu;
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << "fnv1a64:" << std::hex << hash;
  return out.str();
}

agents::ResourceGovernanceQuotaVector Quotas(std::int64_t value) {
  agents::ResourceGovernanceQuotaVector quotas;
  quotas.memory_bytes = value;
  quotas.device_memory_bytes = value;
  quotas.pinned_memory_bytes = value;
  quotas.io_bytes = value;
  quotas.io_ops = value;
  quotas.worker_threads = value;
  quotas.backlog_items = value;
  quotas.candidate_rows = value;
  quotas.cache_entries = value;
  quotas.batch_rows = value;
  quotas.fragments = value;
  quotas.lanes = value;
  quotas.time_budget_microseconds = value;
  return quotas;
}

agents::ResourceGovernanceAdmissionRequest Governance(
    agents::ResourceGovernanceAction over_limit =
        agents::ResourceGovernanceAction::kExactScalarFallback,
    std::int64_t limit = 1000000,
    std::int64_t requested = 6) {
  agents::ResourceGovernanceAdmissionRequest request;
  request.operation_id = "orh281.allocator.hot_path";
  request.expected_family = agents::ResourceGovernanceFamily::kQueryMemoryArena;
  request.descriptor.descriptor_id = "runtime.allocator.orh281";
  request.descriptor.family = agents::ResourceGovernanceFamily::kQueryMemoryArena;
  request.descriptor.source =
      agents::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  request.descriptor.source_path_or_label = "runtime_policy:orh281";
  request.descriptor.descriptor_generation = 281;
  request.descriptor.expected_generation = 281;
  request.descriptor.limits = Quotas(limit);
  request.descriptor.benchmark_clean = true;
  request.descriptor.runtime_dependency_present = true;
  request.descriptor.over_limit_action = over_limit;
  request.requested = Quotas(requested);
  request.requested.memory_bytes = requested;
  request.requested.cache_entries = requested;
  request.requested.batch_rows = requested;
  request.requested.time_budget_microseconds = requested;
  request.require_exact_scalar_fallback_available = true;
  request.exact_scalar_fallback_available = true;
  return request;
}

memory::AllocationPolicy Policy() {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "orh281_allocator_lifetime";
  policy.hard_limit_bytes = 1024 * 1024;
  policy.soft_limit_bytes = 512 * 1024;
  policy.per_context_limit_bytes = 1024 * 1024;
  policy.track_allocations = true;
  return policy;
}

memory::QueryMemoryContext ArenaContext(bool unsafe_authority = false) {
  memory::QueryMemoryContext context;
  context.query_id = "orh281.query";
  context.statement_id = "orh281.statement";
  context.session_id = "orh281.session";
  context.transaction_id = "orh281.transaction";
  context.database_id = "orh281.database";
  context.engine_id = "orh281.engine";
  context.operation_id = "orh281.allocator.hot_path";
  context.engine_mga_authoritative = true;
  context.parser_or_donor_finality_or_visibility_authority = unsafe_authority;
  return context;
}

memory::QueryMemoryArenaLimits ArenaLimits() {
  memory::QueryMemoryArenaLimits limits;
  limits.hard_limit_bytes = 1024 * 1024;
  limits.soft_limit_bytes = 512 * 1024;
  limits.family_limit_bytes = 1024 * 1024;
  limits.query_limit_bytes = 1024 * 1024;
  limits.spill_limit_bytes = 0;
  limits.allow_spill = false;
  return limits;
}

std::vector<exec::AllocatorHotPathObjectRequest> Objects() {
  return {
      {exec::AllocatorHotPathObjectKind::kRowBatch, 512, "rows"},
      {exec::AllocatorHotPathObjectKind::kPlanNode, 256, "plan"},
      {exec::AllocatorHotPathObjectKind::kEvidenceObject, 192, "evidence"},
      {exec::AllocatorHotPathObjectKind::kCursorFrame, 384, "cursor"},
      {exec::AllocatorHotPathObjectKind::kResultFrame, 384, "result"},
      {exec::AllocatorHotPathObjectKind::kDmlLocatorStream, 448, "locator"},
  };
}

exec::AllocatorHotPathRequest Request(memory::QueryMemoryArena* arena) {
  exec::AllocatorHotPathRequest request;
  request.route_label = "orh281.allocator.hot_path";
  request.result_contract_hash =
      StableHash({"row:1:a", "row:2:b", "evidence:allocator"});
  request.fallback_result_contract_hash = request.result_contract_hash;
  request.arena_generation = 281;
  request.expected_arena_generation = 281;
  request.route_epoch = 2810;
  request.owner_route_epoch = 2810;
  request.runtime_consumed = true;
  request.exact_fallback_available = true;
  request.arena = arena;
  request.resource_governance = Governance();
  request.objects = Objects();
  request.authority.engine_mga_snapshot_bound = true;
  request.authority.transaction_inventory_authoritative = true;
  request.authority.security_recheck_required = true;
  request.profiler.source_label = "engine_internal_allocator_counter";
  request.profiler.measured = true;
  request.profiler.sample_count = 9;
  request.profiler.baseline_allocation_count = 36;
  request.profiler.arena_allocation_count = 6;
  request.profiler.baseline_allocation_bytes = 8192;
  request.profiler.arena_allocation_bytes = 2176;
  return request;
}

void RequireAccepted(const exec::AllocatorHotPathResult& result) {
  Require(result.ok && result.benchmark_clean,
          "allocator hot path was not benchmark-clean");
  Require(!result.fallback_used && !result.fail_closed,
          "allocator hot path unexpectedly fell back/refused");
  Require(result.grant_ids.size() == 6,
          "allocator hot path did not allocate all object families");
  Require(result.allocation_count_saved > 0 &&
              result.allocation_bytes_saved > 0,
          "allocator hot path did not reduce allocation churn");
  Require(HasEvidence(result.evidence,
                      "allocator_hot_path.allocated_family=row_batch"),
          "row batch allocation evidence missing");
  Require(HasEvidence(result.evidence,
                      "allocator_hot_path.allocated_family=plan_node"),
          "plan node allocation evidence missing");
  Require(HasEvidence(result.evidence,
                      "allocator_hot_path.allocated_family=evidence_object"),
          "evidence allocation evidence missing");
  Require(HasEvidence(result.evidence,
                      "allocator_hot_path.allocated_family=cursor_frame"),
          "cursor frame allocation evidence missing");
  Require(HasEvidence(result.evidence,
                      "allocator_hot_path.allocated_family=result_frame"),
          "result frame allocation evidence missing");
  Require(HasEvidence(result.evidence,
                      "allocator_hot_path.allocated_family=dml_locator_stream"),
          "DML locator stream allocation evidence missing");
  Require(HasEvidence(result.evidence,
                      "allocator_hot_path.mga_finality_authority=engine_transaction_inventory"),
          "MGA authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "allocator_hot_path.security_recheck_required=true"),
          "security recheck evidence missing");
  Require(HasEvidence(result.evidence,
                      "allocator_hot_path.row_identity_authority=false"),
          "row identity non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "allocator_hot_path.active_grants_after_release=0"),
          "active grant cleanup evidence missing");
  Require(HasEvidence(result.evidence, "allocator_hot_path.leak_count=0"),
          "leak-free evidence missing");
  Require(HasEvidence(result.evidence,
                      "allocator_hot_path.no_dangling_references=true"),
          "dangling-reference evidence missing");
  Require(HasEvidence(result.evidence,
                      "resource_governance.action=admit"),
          "resource governance admission evidence missing");
}

void RequireRejected(const exec::AllocatorHotPathResult& result,
                     std::string_view diagnostic,
                     std::string_view label) {
  Require(!result.benchmark_clean,
          std::string(label) + " was marked benchmark-clean");
  Require(result.diagnostic_code.find(diagnostic) != std::string::npos,
          std::string(label) + " diagnostic mismatch: " +
              result.diagnostic_code);
}

void RequireFallback(const exec::AllocatorHotPathResult& result,
                     std::string_view diagnostic,
                     std::string_view label) {
  Require(result.fallback_used && !result.benchmark_clean,
          std::string(label) + " did not use exact fallback");
  Require(result.diagnostic_code.find(diagnostic) != std::string::npos,
          std::string(label) + " fallback diagnostic mismatch: " +
              result.diagnostic_code);
  Require(HasEvidence(result.evidence,
                      "allocator_hot_path.exact_fallback_available=true"),
          std::string(label) + " missing exact fallback evidence");
}

void TestPositiveAllocatorHotPath() {
  memory::BoundedAllocator allocator(Policy());
  memory::QueryMemoryArena arena(ArenaContext(), ArenaLimits(), &allocator);
  RequireAccepted(exec::ExecuteAllocatorHotPath(Request(&arena)));
  const auto snapshot = allocator.Snapshot();
  Require(snapshot.active_allocation_count == 0,
          "bounded allocator still has active allocations");
  Require(snapshot.leak_candidate_count == 0,
          "bounded allocator reported leak candidates");
}

void TestAuthorityAndProofRefusals() {
  memory::BoundedAllocator allocator(Policy());
  memory::QueryMemoryArena arena(ArenaContext(), ArenaLimits(), &allocator);

  auto parser = Request(&arena);
  parser.authority.parser_client_or_donor_allocator_authority = true;
  RequireRejected(exec::ExecuteAllocatorHotPath(parser),
                  "ORH_ALLOCATOR_LIFETIME_UNSAFE_AUTHORITY",
                  "parser/client/donor allocator authority");

  auto finality = Request(&arena);
  finality.authority.allocator_visibility_or_finality_authority = true;
  RequireRejected(exec::ExecuteAllocatorHotPath(finality),
                  "ORH_ALLOCATOR_LIFETIME_UNSAFE_AUTHORITY",
                  "allocation metadata as finality authority");

  auto mga = Request(&arena);
  mga.authority.engine_mga_snapshot_bound = false;
  RequireRejected(exec::ExecuteAllocatorHotPath(mga),
                  "ORH_ALLOCATOR_LIFETIME_MGA_UNPROVEN",
                  "missing MGA evidence");

  auto security = Request(&arena);
  security.authority.security_recheck_required = false;
  RequireRejected(exec::ExecuteAllocatorHotPath(security),
                  "ORH_ALLOCATOR_LIFETIME_SECURITY_UNPROVEN",
                  "missing security evidence");
}

void TestLifetimeAndProfilerRefusals() {
  memory::BoundedAllocator allocator(Policy());
  memory::QueryMemoryArena arena(ArenaContext(), ArenaLimits(), &allocator);

  auto stale = Request(&arena);
  stale.arena_generation = 280;
  RequireRejected(exec::ExecuteAllocatorHotPath(stale),
                  "ORH_ALLOCATOR_LIFETIME_STALE_GENERATION",
                  "stale arena generation");

  auto scope = Request(&arena);
  scope.cross_route_ownership_transfer = true;
  RequireRejected(exec::ExecuteAllocatorHotPath(scope),
                  "ORH_ALLOCATOR_LIFETIME_SCOPE_UNSAFE",
                  "cross-route ownership");

  auto use_after_scope = Request(&arena);
  use_after_scope.use_after_scope_observed = true;
  RequireRejected(exec::ExecuteAllocatorHotPath(use_after_scope),
                  "ORH_ALLOCATOR_LIFETIME_SCOPE_UNSAFE",
                  "use-after-scope");

  auto profiler = Request(&arena);
  profiler.profiler.measured = false;
  profiler.profiler.source_label = "contract_only";
  RequireRejected(exec::ExecuteAllocatorHotPath(profiler),
                  "ORH_ALLOCATOR_LIFETIME_PROFILER_MISSING",
                  "profiler/allocation evidence missing");

  auto mismatch = Request(&arena);
  mismatch.fallback_result_contract_hash = "fnv1a64:mismatch";
  RequireRejected(exec::ExecuteAllocatorHotPath(mismatch),
                  "ORH_ALLOCATOR_LIFETIME_RESULT_MISMATCH",
                  "result/evidence equivalence mismatch");

  auto fallback = Request(&arena);
  fallback.exact_fallback_available = false;
  RequireRejected(exec::ExecuteAllocatorHotPath(fallback),
                  "ORH_ALLOCATOR_LIFETIME_EXACT_FALLBACK_UNAVAILABLE",
                  "exact fallback unavailable");
}

void TestGovernanceAndArenaRefusals() {
  memory::BoundedAllocator allocator(Policy());
  memory::QueryMemoryArena arena(ArenaContext(), ArenaLimits(), &allocator);

  auto pressure = Request(&arena);
  pressure.resource_governance = Governance(
      agents::ResourceGovernanceAction::kExactScalarFallback, 1, 64);
  RequireFallback(exec::ExecuteAllocatorHotPath(pressure),
                  "ORH_ALLOCATOR_LIFETIME_RESOURCE_PRESSURE_FALLBACK",
                  "resource-governance memory pressure");

  memory::BoundedAllocator unsafe_allocator(Policy());
  memory::QueryMemoryArena unsafe_arena(ArenaContext(true),
                                        ArenaLimits(),
                                        &unsafe_allocator);
  auto unsafe = Request(&unsafe_arena);
  RequireRejected(exec::ExecuteAllocatorHotPath(unsafe),
                  "ORH_ALLOCATOR_LIFETIME_GRANT_REFUSED",
                  "arena internal unsafe authority");
}

}  // namespace

int main() {
  TestPositiveAllocatorHotPath();
  TestAuthorityAndProofRefusals();
  TestLifetimeAndProfilerRefusals();
  TestGovernanceAndArenaRefusals();
  std::cout << "ORH-281 allocator lifetime hot path gate passed\n";
  return EXIT_SUCCESS;
}
