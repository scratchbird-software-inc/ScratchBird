// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory.hpp"
#include "operator_memory_grant.hpp"
#include "query_memory_arena.hpp"
#include "query_memory_arena_executor.hpp"
#include "resource_governance_admission.hpp"
#include "temp_workspace_lifecycle.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace exec = scratchbird::engine::executor;
namespace mem = scratchbird::core::memory;
namespace platform = scratchbird::core::platform;

// MMCH_MEMORY_CANCEL_ROLLBACK_CLEANUP
[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool EvidenceHas(const std::vector<std::string>& evidence, std::string_view token) {
  for (const auto& row : evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireAuthorityEvidence(const std::vector<std::string>& evidence) {
  Require(EvidenceHas(evidence, "query_memory_arena.transaction_finality_authority=false") ||
              EvidenceHas(evidence, "executor.operator_memory.authority_scope="),
          "MMCH-022 memory cleanup authority evidence missing");
  for (const auto& row : evidence) {
    for (const auto forbidden :
         {"parser_executes_sql=true", "provider_transaction_finality_authority=true",
          "provider_visibility_authority=true", "client_visibility_or_finality_authority=true",
          "write_ahead_log_finality_authority=true"}) {
      Require(row.find(forbidden) == std::string::npos,
              "MMCH-022 cleanup evidence leaked forbidden authority token");
    }
  }
}

mem::AllocationPolicy AllocationPolicy() {
  auto policy = mem::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "mmch022_cancel_cleanup";
  policy.hard_limit_bytes = 1024 * 1024;
  policy.soft_limit_bytes = 1024 * 1024;
  policy.per_context_limit_bytes = 1024 * 1024;
  policy.reject_over_soft_limit = false;
  return policy;
}

mem::QueryMemoryContext Context(std::string suffix) {
  mem::QueryMemoryContext context;
  context.query_id = "q-mmch022-" + suffix;
  context.statement_id = "stmt-mmch022";
  context.session_id = "session-mmch022";
  context.transaction_id = "txn-mmch022";
  context.database_id = "db-mmch022";
  context.engine_id = "engine-mmch022";
  context.operation_id = "op-mmch022";
  context.engine_mga_authoritative = true;
  return context;
}

mem::QueryMemoryArenaLimits Limits() {
  mem::QueryMemoryArenaLimits limits;
  limits.hard_limit_bytes = 256 * 1024;
  limits.soft_limit_bytes = 64 * 1024;
  limits.family_limit_bytes = 128 * 1024;
  limits.query_limit_bytes = 192 * 1024;
  limits.spill_limit_bytes = 192 * 1024;
  limits.allow_spill = true;
  return limits;
}

mem::TempWorkspacePolicy TempPolicy(const std::filesystem::path& root) {
  mem::TempWorkspacePolicy policy;
  policy.policy_name = "mmch022_temp";
  policy.root_path = root;
  policy.filespace_quota_bytes = 256 * 1024;
  policy.session_quota_bytes = 256 * 1024;
  policy.transaction_quota_bytes = 256 * 1024;
  policy.statement_quota_bytes = 256 * 1024;
  policy.operation_quota_bytes = 256 * 1024;
  policy.create_root_path = true;
  policy.cleanup_files_on_release = true;
  return policy;
}

agents::ResourceGovernanceAdmissionRequest Governance(std::string operation_id,
                                                      platform::u64 bytes,
                                                      bool spillable) {
  agents::ResourceGovernanceAdmissionRequest request;
  request.operation_id = std::move(operation_id);
  request.expected_family = agents::ResourceGovernanceFamily::kQueryMemoryArena;
  request.descriptor.descriptor_id = "mmch022.query_memory.cleanup_quota";
  request.descriptor.family = agents::ResourceGovernanceFamily::kQueryMemoryArena;
  request.descriptor.source = agents::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  request.descriptor.source_path_or_label = "runtime.policy.mmch022.query_memory";
  request.descriptor.descriptor_generation = 22;
  request.descriptor.expected_generation = 22;
  request.descriptor.over_limit_action = agents::ResourceGovernanceAction::kFailClosed;
  request.descriptor.benchmark_clean = true;
  request.descriptor.runtime_dependency_present = true;
  request.descriptor.limits = {
      256 * 1024, 1, 1, 256 * 1024, 4, 1, 8, 256 * 1024, 8, 8, 8, 1, 1000000};
  request.requested = {
      static_cast<std::int64_t>(bytes), 0, 0,
      static_cast<std::int64_t>(spillable ? bytes : 0), 0, 1, 1, 1, 0, 1, 1, 1, 1000};
  return request;
}

exec::ExecutorOperatorMemoryAuthority Authority() {
  exec::ExecutorOperatorMemoryAuthority authority;
  authority.engine_mga_snapshot_bound = true;
  authority.transaction_inventory_authoritative = true;
  authority.security_recheck_required = true;
  return authority;
}

struct Harness {
  explicit Harness(std::string suffix)
      : root(std::filesystem::temp_directory_path() / ("sb_mmch022_" + suffix)),
        allocator(AllocationPolicy()),
        temp(TempPolicy(root)),
        unified("mmch022_" + suffix, 256 * 1024),
        arena(Context(std::move(suffix)), Limits(), &allocator, &temp, &unified) {
    std::filesystem::remove_all(root);
  }

  ~Harness() {
    std::filesystem::remove_all(root);
  }

  void RequireClean(std::string_view label) {
    const auto snapshot = arena.Snapshot();
    Require(snapshot.current_bytes == 0 &&
                snapshot.active_grant_count == 0 &&
                snapshot.leak_count == 0,
            "MMCH-022 arena cleanup failed");
    Require(temp.Snapshot().active_bytes == 0,
            "MMCH-022 temp workspace cleanup failed");
    Require(unified.Snapshot().total_bytes == 0,
            "MMCH-022 unified memory/spill budget cleanup failed");
    Require(allocator.Snapshot().leak_candidate_count == 0,
            "MMCH-022 allocator cleanup failed");
    (void)label;
  }

  std::filesystem::path root;
  mem::BoundedAllocator allocator;
  mem::TempWorkspaceLifecycleManager temp;
  mem::UnifiedMemorySpillBudgetLedger unified;
  mem::QueryMemoryArena arena;
};

void CancellationReleasesSpillAndBudget() {
  Harness harness("cancel");
  exec::ExecutorQueryMemoryRequest request;
  request.shape = exec::ExecutorQueryShape::search;
  request.bytes = 80 * 1024;
  request.spillable = true;
  request.purpose = "mmch022.cancel.spill";
  request.resource_governance = Governance(request.purpose, request.bytes, true);
  auto granted = exec::RequestExecutorQueryMemory(&harness.arena, request);
  Require(granted.ok() &&
              granted.arena_result.grant.has_value() &&
              granted.arena_result.grant->spilled,
          "MMCH-022 spill grant setup failed");
  Require(harness.temp.Snapshot().active_bytes == 80 * 1024,
          "MMCH-022 spill setup missing temp bytes");
  Require(harness.unified.Snapshot().total_bytes == 80 * 1024,
          "MMCH-022 spill setup missing unified budget bytes");

  auto cancelled = exec::CancelExecutorQueryMemory(&harness.arena, "statement_cancelled");
  Require(cancelled.ok(), "MMCH-022 cancellation cleanup failed");
  Require(EvidenceHas(cancelled.evidence, "query_memory_arena.cancelled=true"),
          "MMCH-022 cancellation evidence missing");
  RequireAuthorityEvidence(cancelled.evidence);
  harness.RequireClean("cancel");
}

void RollbackCleanupDoesNotBecomeFinalityAuthority() {
  Harness harness("rollback");
  exec::ExecutorQueryMemoryRequest request;
  request.shape = exec::ExecutorQueryShape::dml;
  request.bytes = 4096;
  request.spillable = false;
  request.purpose = "mmch022.rollback.dml";
  request.resource_governance = Governance(request.purpose, request.bytes, false);
  auto granted = exec::RequestExecutorQueryMemory(&harness.arena, request);
  Require(granted.ok() && granted.arena_result.grant.has_value(),
          "MMCH-022 rollback setup grant failed");
  auto rollback = exec::CancelExecutorQueryMemory(&harness.arena, "rollback_cleanup");
  Require(rollback.ok(), "MMCH-022 rollback cleanup failed");
  Require(EvidenceHas(rollback.evidence, "query_memory_arena.transaction_finality_authority=false"),
          "MMCH-022 rollback cleanup became finality authority");
  RequireAuthorityEvidence(rollback.evidence);
  harness.RequireClean("rollback");
}

void AutocommitFailureReleasesOperatorGrant() {
  Harness harness("autocommit");
  exec::ExecutorOperatorMemoryRequest request;
  request.operator_kind = exec::ExecutorMemoryOperatorKind::dml_write;
  request.route_label = "embedded.sblr.dml.autocommit_failure";
  request.bytes = 4096;
  request.spillable = false;
  request.purpose = "mmch022.autocommit_failure";
  request.arena = &harness.arena;
  request.resource_governance = Governance(request.purpose, request.bytes, false);
  request.authority = Authority();
  auto granted = exec::RequestExecutorOperatorMemory(std::move(request));
  Require(granted.ok(), "MMCH-022 autocommit grant failed");
  auto released = exec::ReleaseExecutorOperatorMemory(
      &harness.arena,
      exec::ExecutorMemoryOperatorKind::dml_write,
      granted.grant_id);
  Require(released.ok(), "MMCH-022 autocommit failure release failed");
  Require(EvidenceHas(released.evidence, "executor.operator_memory.release_routed=true"),
          "MMCH-022 autocommit release evidence missing");
  RequireAuthorityEvidence(granted.evidence);
  harness.RequireClean("autocommit");
}

void RefusedGrantLeavesNoResidue() {
  Harness harness("refused");
  exec::ExecutorQueryMemoryRequest request;
  request.shape = exec::ExecutorQueryShape::relational;
  request.bytes = 512 * 1024;
  request.spillable = false;
  request.purpose = "mmch022.refused";
  request.resource_governance = Governance(request.purpose, request.bytes, false);
  auto refused = exec::RequestExecutorQueryMemory(&harness.arena, request);
  Require(!refused.ok(), "MMCH-022 overlimit grant was accepted");
  Require(EvidenceHas(refused.evidence, "executor.query_memory.fail_closed=true") ||
              EvidenceHas(refused.evidence, "query_memory_arena.fail_closed=true"),
          "MMCH-022 refused grant evidence missing");
  harness.RequireClean("refused");
}

}  // namespace

int main() {
  std::cout << "MMCH-022 authority_note=cancel_rollback_cleanup_evidence_only;"
               "not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority"
            << '\n';
  CancellationReleasesSpillAndBudget();
  RollbackCleanupDoesNotBecomeFinalityAuthority();
  AutocommitFailureReleasesOperatorGrant();
  RefusedGrantLeavesNoResidue();
  return EXIT_SUCCESS;
}
