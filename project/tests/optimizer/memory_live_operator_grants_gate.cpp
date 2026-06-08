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

void RequireEvidenceHygiene(const std::vector<std::string>& evidence) {
  for (const auto& row : evidence) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "provider_transaction_finality_authority=true",
          "provider_visibility_authority=true", "parser_executes_sql=true",
          "client_visibility_or_finality_authority=true",
          "write_ahead_log_finality_authority=true"}) {
      Require(row.find(forbidden) == std::string::npos,
              "MMCH-020 evidence leaked forbidden authority or documentation token");
    }
  }
}

mem::AllocationPolicy AllocationPolicy() {
  auto policy = mem::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "mmch020_live_operator_memory";
  policy.hard_limit_bytes = 1024 * 1024;
  policy.soft_limit_bytes = 1024 * 1024;
  policy.per_context_limit_bytes = 1024 * 1024;
  policy.reject_over_soft_limit = false;
  return policy;
}

mem::QueryMemoryContext Context() {
  mem::QueryMemoryContext context;
  context.query_id = "q-mmch020";
  context.statement_id = "stmt-mmch020";
  context.session_id = "session-mmch020";
  context.transaction_id = "txn-mmch020";
  context.database_id = "db-mmch020";
  context.engine_id = "engine-mmch020";
  context.operation_id = "op-mmch020";
  context.engine_mga_authoritative = true;
  return context;
}

mem::QueryMemoryArenaLimits Limits() {
  mem::QueryMemoryArenaLimits limits;
  limits.hard_limit_bytes = 1024 * 1024;
  limits.soft_limit_bytes = 128 * 1024;
  limits.family_limit_bytes = 256 * 1024;
  limits.query_limit_bytes = 512 * 1024;
  limits.spill_limit_bytes = 512 * 1024;
  limits.allow_spill = true;
  return limits;
}

mem::TempWorkspacePolicy TempPolicy(const std::filesystem::path& root) {
  mem::TempWorkspacePolicy policy;
  policy.policy_name = "mmch020_temp";
  policy.root_path = root;
  policy.filespace_quota_bytes = 512 * 1024;
  policy.session_quota_bytes = 512 * 1024;
  policy.transaction_quota_bytes = 512 * 1024;
  policy.statement_quota_bytes = 512 * 1024;
  policy.operation_quota_bytes = 512 * 1024;
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
  request.descriptor.descriptor_id = "mmch020.query_memory.runtime_quota";
  request.descriptor.family = agents::ResourceGovernanceFamily::kQueryMemoryArena;
  request.descriptor.source = agents::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  request.descriptor.source_path_or_label = "runtime.policy.mmch020.query_memory";
  request.descriptor.descriptor_generation = 20;
  request.descriptor.expected_generation = 20;
  request.descriptor.over_limit_action = agents::ResourceGovernanceAction::kFailClosed;
  request.descriptor.benchmark_clean = true;
  request.descriptor.runtime_dependency_present = true;
  request.descriptor.limits = {
      512 * 1024, 1, 1, 512 * 1024, 4, 1, 8, 512 * 1024, 8, 8, 8, 1, 1000000};
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

void AllOperatorKindsRequestAndReleaseArenaGrants() {
  const auto root = std::filesystem::temp_directory_path() / "sb_mmch020_live_operator_memory";
  std::filesystem::remove_all(root);
  mem::BoundedAllocator allocator(AllocationPolicy());
  mem::TempWorkspaceLifecycleManager temp(TempPolicy(root));
  mem::QueryMemoryArena arena(Context(), Limits(), &allocator, &temp);

  const std::vector<exec::ExecutorMemoryOperatorKind> operators = {
      exec::ExecutorMemoryOperatorKind::scan,
      exec::ExecutorMemoryOperatorKind::sort,
      exec::ExecutorMemoryOperatorKind::hash_join,
      exec::ExecutorMemoryOperatorKind::merge_join,
      exec::ExecutorMemoryOperatorKind::aggregate,
      exec::ExecutorMemoryOperatorKind::window,
      exec::ExecutorMemoryOperatorKind::candidate_set,
      exec::ExecutorMemoryOperatorKind::vector_search,
      exec::ExecutorMemoryOperatorKind::full_text_search,
      exec::ExecutorMemoryOperatorKind::graph_traversal,
      exec::ExecutorMemoryOperatorKind::document_path,
      exec::ExecutorMemoryOperatorKind::time_series_rollup,
      exec::ExecutorMemoryOperatorKind::dml_write,
      exec::ExecutorMemoryOperatorKind::streaming_result};

  std::vector<std::pair<exec::ExecutorMemoryOperatorKind, std::string>> grants;
  for (const auto kind : operators) {
    exec::ExecutorOperatorMemoryRequest request;
    request.operator_kind = kind;
    request.route_label = std::string("engine.executor.") +
                          exec::ExecutorMemoryOperatorKindName(kind);
    request.bytes = 1024;
    request.spillable = kind == exec::ExecutorMemoryOperatorKind::sort ||
                        kind == exec::ExecutorMemoryOperatorKind::hash_join ||
                        kind == exec::ExecutorMemoryOperatorKind::aggregate ||
                        kind == exec::ExecutorMemoryOperatorKind::window ||
                        kind == exec::ExecutorMemoryOperatorKind::full_text_search;
    request.purpose = std::string("mmch020.") +
                      exec::ExecutorMemoryOperatorKindName(kind);
    request.arena = &arena;
    request.resource_governance =
        Governance(request.purpose, request.bytes, request.spillable);
    request.authority = Authority();

    auto result = exec::RequestExecutorOperatorMemory(std::move(request));
    Require(result.ok(), "MMCH-020 operator memory grant failed");
    Require(!result.grant_id.empty(), "MMCH-020 operator grant id missing");
    Require(EvidenceHas(result.evidence, "MMCH_LIVE_OPERATOR_MEMORY_GRANTS"),
            "MMCH-020 live operator evidence marker missing");
    Require(EvidenceHas(result.evidence, "executor.operator_memory.live_operator_route=true"),
            "MMCH-020 live operator route evidence missing");
    Require(EvidenceHas(result.evidence, "executor.query_memory.primitive=query_memory_arena"),
            "MMCH-020 query memory primitive evidence missing");
    Require(EvidenceHas(
                result.evidence,
                "executor.operator_memory.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority"),
            "MMCH-020 authority boundary evidence missing");
    RequireEvidenceHygiene(result.evidence);
    grants.push_back({kind, result.grant_id});
  }

  auto snapshot = arena.Snapshot();
  Require(snapshot.active_grant_count == operators.size(),
          "MMCH-020 active grant count did not match operator count");
  Require(snapshot.current_bytes == operators.size() * 1024,
          "MMCH-020 current bytes did not match operator grants");

  for (const auto& [kind, grant_id] : grants) {
    auto released = exec::ReleaseExecutorOperatorMemory(&arena, kind, grant_id);
    Require(released.ok(), "MMCH-020 operator release failed");
    Require(EvidenceHas(released.evidence, "executor.operator_memory.release_routed=true"),
            "MMCH-020 release evidence missing");
    RequireEvidenceHygiene(released.evidence);
  }

  snapshot = arena.Snapshot();
  Require(snapshot.active_grant_count == 0 &&
              snapshot.current_bytes == 0 &&
              snapshot.leak_count == 0,
          "MMCH-020 operator releases leaked query memory");
  Require(allocator.Snapshot().leak_candidate_count == 0,
          "MMCH-020 allocator leak candidates remained");
  std::filesystem::remove_all(root);
}

void UnsafeOperatorMemoryRequestsFailClosed() {
  const auto root = std::filesystem::temp_directory_path() / "sb_mmch020_refuse";
  std::filesystem::remove_all(root);
  mem::BoundedAllocator allocator(AllocationPolicy());
  mem::TempWorkspaceLifecycleManager temp(TempPolicy(root));
  mem::QueryMemoryArena arena(Context(), Limits(), &allocator, &temp);

  exec::ExecutorOperatorMemoryRequest request;
  request.operator_kind = exec::ExecutorMemoryOperatorKind::hash_join;
  request.route_label = "engine.executor.hash_join";
  request.bytes = 1024;
  request.arena = &arena;
  request.resource_governance = Governance("mmch020.refuse", request.bytes, false);
  request.authority = Authority();
  request.authority.parser_client_or_donor_memory_authority = true;

  auto refused = exec::RequestExecutorOperatorMemory(std::move(request));
  Require(!refused.ok() &&
              refused.fail_closed &&
              refused.diagnostic.diagnostic_code ==
                  "SB_EXECUTOR_OPERATOR_MEMORY.UNSAFE_AUTHORITY",
          "MMCH-020 unsafe operator authority did not fail closed");
  Require(EvidenceHas(refused.evidence, "MMCH_LIVE_OPERATOR_MEMORY_GRANTS"),
          "MMCH-020 refusal evidence marker missing");

  request = {};
  request.operator_kind = exec::ExecutorMemoryOperatorKind::sort;
  request.route_label = "engine.executor.sort";
  request.bytes = 1024;
  request.arena = &arena;
  request.resource_governance = Governance("mmch020.security_refuse", request.bytes, false);
  request.authority = Authority();
  request.authority.security_recheck_required = false;
  refused = exec::RequestExecutorOperatorMemory(std::move(request));
  Require(!refused.ok() &&
              refused.fail_closed &&
              refused.diagnostic.diagnostic_code ==
                  "SB_EXECUTOR_OPERATOR_MEMORY.SECURITY_RECHECK_REQUIRED",
          "MMCH-020 missing security recheck did not fail closed");
  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  std::cout << "MMCH-020 authority_note=live_operator_memory_grants_evidence_only;"
               "not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority"
            << '\n';
  AllOperatorKindsRequestAndReleaseArenaGrants();
  UnsafeOperatorMemoryRequestsFailClosed();
  return EXIT_SUCCESS;
}
