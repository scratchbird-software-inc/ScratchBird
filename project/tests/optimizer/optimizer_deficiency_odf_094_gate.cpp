// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory.hpp"
#include "query_memory_arena.hpp"
#include "query_memory_arena_executor.hpp"
#include "temp_workspace_lifecycle.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;
namespace mem = scratchbird::core::memory;
namespace agents = scratchbird::core::agents;
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

bool EvidenceHas(const std::vector<std::string>& evidence,
                 std::string_view token) {
  for (const auto& item : evidence) {
    if (item.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireEvidenceHygiene(const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "contracts", "references",
          "provider_transaction_finality_authority=true",
          "provider_visibility_authority=true", "parser_executes_sql=true",
          "client_visibility_or_finality_authority=true",
          "write_ahead_log_finality_authority=true"}) {
      Require(item.find(forbidden) == std::string::npos,
              "ODF-094 evidence leaked forbidden documentation or authority token");
    }
  }
}

mem::AllocationPolicy AllocationPolicy() {
  auto policy = mem::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "odf_094_bounded_allocator";
  policy.hard_limit_bytes = 1024 * 1024;
  policy.soft_limit_bytes = 1024 * 1024;
  policy.per_context_limit_bytes = 1024 * 1024;
  policy.reject_over_soft_limit = false;
  return policy;
}

mem::QueryMemoryContext Context() {
  mem::QueryMemoryContext context;
  context.query_id = "q-094";
  context.statement_id = "stmt-094";
  context.session_id = "session-094";
  context.transaction_id = "txn-094";
  context.database_id = "db-094";
  context.engine_id = "engine-094";
  context.operation_id = "op-094";
  context.engine_mga_authoritative = true;
  return context;
}

mem::QueryMemoryArenaLimits Limits() {
  mem::QueryMemoryArenaLimits limits;
  limits.hard_limit_bytes = 1024 * 1024;
  limits.soft_limit_bytes = 64 * 1024;
  limits.family_limit_bytes = 96 * 1024;
  limits.query_limit_bytes = 128 * 1024;
  limits.spill_limit_bytes = 128 * 1024;
  limits.allow_spill = true;
  return limits;
}

mem::TempWorkspacePolicy TempPolicy(const std::filesystem::path& root) {
  mem::TempWorkspacePolicy policy;
  policy.policy_name = "odf_094_temp_workspace";
  policy.root_path = root;
  policy.filespace_quota_bytes = 128 * 1024;
  policy.session_quota_bytes = 128 * 1024;
  policy.transaction_quota_bytes = 128 * 1024;
  policy.statement_quota_bytes = 128 * 1024;
  policy.operation_quota_bytes = 128 * 1024;
  policy.create_root_path = true;
  policy.cleanup_files_on_release = true;
  return policy;
}

exec::ExecutorQueryMemoryRequest ExecutorRequest(
    exec::ExecutorQueryShape shape,
    platform::u64 bytes,
    bool spillable,
    std::string purpose) {
  exec::ExecutorQueryMemoryRequest request;
  request.shape = shape;
  request.bytes = bytes;
  request.spillable = spillable;
  request.purpose = std::move(purpose);
  request.resource_governance.operation_id =
      std::string("odf094.query_memory.") + exec::ExecutorQueryShapeName(shape);
  request.resource_governance.descriptor.descriptor_id =
      "odf106.query_memory.runtime_quota";
  request.resource_governance.descriptor.family =
      agents::ResourceGovernanceFamily::kQueryMemoryArena;
  request.resource_governance.descriptor.source =
      agents::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  request.resource_governance.descriptor.source_path_or_label =
      "runtime.policy.odf106.query_memory";
  request.resource_governance.descriptor.descriptor_generation = 94;
  request.resource_governance.descriptor.expected_generation = 94;
  request.resource_governance.descriptor.over_limit_action =
      agents::ResourceGovernanceAction::kFailClosed;
  request.resource_governance.descriptor.benchmark_clean = true;
  request.resource_governance.descriptor.runtime_dependency_present = true;
  request.resource_governance.descriptor.limits = {
      128 * 1024, 1, 1, 128 * 1024, 4, 1, 8, 128 * 1024, 8, 8, 8, 1, 1000000};
  request.resource_governance.requested = {
      static_cast<std::int64_t>(bytes), 0, 0,
      static_cast<std::int64_t>(spillable ? bytes : 0), 0, 1, 1,
      static_cast<std::int64_t>(shape == exec::ExecutorQueryShape::candidate_set
                                    ? 1
                                    : 0),
      0, 1, 1, 1, 1000};
  return request;
}

void SupportedFamiliesAndRelease() {
  mem::BoundedAllocator allocator(AllocationPolicy());
  mem::TempWorkspaceLifecycleManager temp(
      TempPolicy(std::filesystem::temp_directory_path() / "sb_odf_094_families"));
  mem::QueryMemoryArena arena(Context(), Limits(), &allocator, &temp);

  const std::vector<exec::ExecutorQueryShape> shapes = {
      exec::ExecutorQueryShape::relational,
      exec::ExecutorQueryShape::search,
      exec::ExecutorQueryShape::vector,
      exec::ExecutorQueryShape::graph,
      exec::ExecutorQueryShape::document,
      exec::ExecutorQueryShape::time_series,
      exec::ExecutorQueryShape::dml,
      exec::ExecutorQueryShape::candidate_set};

  std::vector<std::string> grant_ids;
  for (const auto shape : shapes) {
    auto request = ExecutorRequest(
        shape, 1024, false,
        std::string("odf_094_") + exec::ExecutorQueryShapeName(shape));
    auto result = exec::RequestExecutorQueryMemory(&arena, request);
    Require(result.ok(), "ODF-094 executor query memory grant failed");
    Require(result.arena_result.grant.has_value(),
            "ODF-094 executor grant id missing");
    Require(EvidenceHas(result.evidence,
                        "executor.query_memory.primitive=query_memory_arena"),
            "ODF-094 executor did not route through memory arena primitive");
    Require(EvidenceHas(result.evidence,
                        std::string("query_memory_arena.family=") +
                            exec::ExecutorQueryShapeName(shape)),
            "ODF-094 family evidence missing");
    Require(EvidenceHas(result.evidence, "resource_governance.route=odf106"),
            "ODF-094 ODF-106 governance admission evidence missing");
    RequireEvidenceHygiene(result.evidence);
    grant_ids.push_back(result.arena_result.grant->grant_id);
  }

  auto snapshot = arena.Snapshot();
  Require(snapshot.current_bytes == shapes.size() * 1024,
          "ODF-094 current bytes not deterministic");
  Require(snapshot.peak_bytes == shapes.size() * 1024,
          "ODF-094 peak bytes not deterministic");
  Require(snapshot.active_grant_count == shapes.size(),
          "ODF-094 active grant count changed");

  for (const auto& id : grant_ids) {
    auto released = exec::ReleaseExecutorQueryMemory(&arena, id);
    Require(released.ok(), "ODF-094 executor release failed");
    RequireEvidenceHygiene(released.evidence);
  }

  snapshot = arena.Snapshot();
  Require(snapshot.current_bytes == 0, "ODF-094 release leaked current bytes");
  Require(snapshot.leak_count == 0, "ODF-094 release left leak count");
  Require(allocator.Snapshot().leak_candidate_count == 0,
          "ODF-094 bounded allocator leak count not zero");
}

void BoundedSpillAndCancel() {
  const auto root = std::filesystem::temp_directory_path() / "sb_odf_094_spill";
  std::filesystem::remove_all(root);
  mem::BoundedAllocator allocator(AllocationPolicy());
  mem::TempWorkspaceLifecycleManager temp(TempPolicy(root));
  mem::QueryMemoryArena arena(Context(), Limits(), &allocator, &temp);

  auto request = ExecutorRequest(exec::ExecutorQueryShape::search,
                                 80 * 1024,
                                 true,
                                 "odf_094_soft_pressure_spill");
  auto spilled = exec::RequestExecutorQueryMemory(&arena, request);
  Require(spilled.ok(), "ODF-094 spillable grant under soft pressure failed");
  Require(spilled.arena_result.grant.has_value() &&
              spilled.arena_result.grant->spilled,
          "ODF-094 spill grant was not marked spilled");
  Require(EvidenceHas(spilled.evidence, "query_memory_arena.spilled=true"),
          "ODF-094 spill evidence missing");
  Require(temp.Snapshot().active_bytes == 80 * 1024,
          "ODF-094 temp workspace spill reservation missing");

  auto cancelled = exec::CancelExecutorQueryMemory(&arena, "odf_094_test_cancel");
  Require(cancelled.ok(), "ODF-094 cancel failed");
  Require(cancelled.counters.current_bytes == 0,
          "ODF-094 cancel did not release memory bytes");
  Require(cancelled.counters.leak_count == 0,
          "ODF-094 cancel left arena leaks");
  Require(temp.Snapshot().active_bytes == 0,
          "ODF-094 cancel did not release spill reservation");
  Require(EvidenceHas(cancelled.evidence, "query_memory_arena.cancelled=true"),
          "ODF-094 cancel evidence missing");
  Require(EvidenceHas(cancelled.evidence,
                      "query_memory_arena.transaction_finality_authority=false"),
          "ODF-094 cancel finality authority evidence missing");
  RequireEvidenceHygiene(cancelled.evidence);

  auto after_release = exec::CancelExecutorQueryMemory(&arena, "after_release");
  Require(!after_release.ok(), "ODF-094 cancel-after-release was accepted");
  Require(after_release.diagnostic.diagnostic_code ==
              "SB_QUERY_MEMORY_ARENA.CANCEL_AFTER_RELEASE",
          "ODF-094 cancel-after-release diagnostic changed");
}

void FailClosedDiagnostics() {
  mem::BoundedAllocator allocator(AllocationPolicy());
  mem::TempWorkspaceLifecycleManager temp(
      TempPolicy(std::filesystem::temp_directory_path() / "sb_odf_094_refuse"));

  {
    mem::QueryMemoryArena arena(Context(), Limits(), &allocator, &temp);
    mem::QueryMemoryGrantRequest request;
    request.family = mem::QueryMemoryFamily::candidate_set;
    request.bytes = 0;
    auto refused = arena.Grant(request);
    Require(!refused.ok(), "ODF-094 zero-sized grant was accepted");
    Require(refused.diagnostic.diagnostic_code ==
                "SB_QUERY_MEMORY_ARENA.ZERO_SIZE_GRANT",
            "ODF-094 zero-sized diagnostic changed");
  }

  {
    mem::QueryMemoryArena arena(Context(), Limits(), &allocator, &temp);
    mem::QueryMemoryGrantRequest request;
    request.family = mem::QueryMemoryFamily::unknown;
    request.bytes = 1024;
    auto refused = arena.Grant(request);
    Require(!refused.ok(), "ODF-094 unsupported family was accepted");
    Require(refused.diagnostic.diagnostic_code ==
                "SB_QUERY_MEMORY_ARENA.UNSUPPORTED_FAMILY",
            "ODF-094 unsupported family diagnostic changed");
  }

  {
    auto limits = Limits();
    limits.hard_limit_bytes = 0;
    limits.soft_limit_bytes = 0;
    limits.query_limit_bytes = 0;
    limits.family_limit_bytes = 0;
    mem::QueryMemoryArena arena(Context(), limits, &allocator, &temp);
    mem::QueryMemoryGrantRequest request;
    request.family = mem::QueryMemoryFamily::candidate_set;
    request.bytes = 1024;
    auto granted = arena.Grant(request);
    Require(granted.ok(), "ODF-094 overflow setup grant failed");
    request.bytes = std::numeric_limits<platform::u64>::max();
    auto refused = arena.Grant(request);
    Require(!refused.ok(), "ODF-094 accounting overflow was accepted");
    Require(refused.diagnostic.diagnostic_code ==
                "SB_QUERY_MEMORY_ARENA.GRANT_OVERFLOW",
            "ODF-094 overflow diagnostic changed");
  }

  {
    auto missing = Context();
    missing.statement_id.clear();
    mem::QueryMemoryArena arena(missing, Limits(), &allocator, &temp);
    mem::QueryMemoryGrantRequest request;
    request.family = mem::QueryMemoryFamily::search;
    request.bytes = 1024;
    auto refused = arena.Grant(request);
    Require(!refused.ok(), "ODF-094 missing context was accepted");
    Require(refused.diagnostic.diagnostic_code ==
                "SB_QUERY_MEMORY_ARENA.CONTEXT_REQUIRED",
            "ODF-094 missing context diagnostic changed");
  }

  {
    auto unsafe = Context();
    unsafe.parser_or_donor_finality_or_visibility_authority = true;
    mem::QueryMemoryArena arena(unsafe, Limits(), &allocator, &temp);
    mem::QueryMemoryGrantRequest request;
    request.family = mem::QueryMemoryFamily::vector;
    request.bytes = 1024;
    auto refused = arena.Grant(request);
    Require(!refused.ok(), "ODF-094 parser authority drift was accepted");
    Require(refused.diagnostic.diagnostic_code ==
                "SB_QUERY_MEMORY_ARENA.UNSAFE_AUTHORITY",
            "ODF-094 parser authority diagnostic changed");
  }

  {
    auto unsafe = Context();
    unsafe.client_finality_or_visibility_authority = true;
    mem::QueryMemoryArena arena(unsafe, Limits(), &allocator, &temp);
    mem::QueryMemoryGrantRequest request;
    request.family = mem::QueryMemoryFamily::document;
    request.bytes = 1024;
    auto refused = arena.Grant(request);
    Require(!refused.ok(), "ODF-094 client authority drift was accepted");
  }

  {
    auto unsafe = Context();
    unsafe.provider_finality_or_visibility_authority = true;
    mem::QueryMemoryArena arena(unsafe, Limits(), &allocator, &temp);
    mem::QueryMemoryGrantRequest request;
    request.family = mem::QueryMemoryFamily::graph;
    request.bytes = 1024;
    auto refused = arena.Grant(request);
    Require(!refused.ok(), "ODF-094 provider authority drift was accepted");
  }

  {
    auto unsafe = Context();
    unsafe.wal_recovery_or_finality_authority = true;
    mem::QueryMemoryArena arena(unsafe, Limits(), &allocator, &temp);
    mem::QueryMemoryGrantRequest request;
    request.family = mem::QueryMemoryFamily::time_series;
    request.bytes = 1024;
    auto refused = arena.Grant(request);
    Require(!refused.ok(), "ODF-094 WAL authority drift was accepted");
  }

  {
    auto limits = Limits();
    limits.hard_limit_bytes = 2048;
    limits.query_limit_bytes = 2048;
    limits.family_limit_bytes = 2048;
    mem::QueryMemoryArena arena(Context(), limits, &allocator, &temp);
    mem::QueryMemoryGrantRequest request;
    request.family = mem::QueryMemoryFamily::dml;
    request.bytes = 4096;
    auto refused = arena.Grant(request);
    Require(!refused.ok(), "ODF-094 over-hard-limit grant was accepted");
    Require(refused.diagnostic.diagnostic_code ==
                "SB_QUERY_MEMORY_ARENA.HARD_LIMIT_EXCEEDED",
            "ODF-094 hard-limit diagnostic changed");
  }

  {
    auto limits = Limits();
    limits.family_limit_bytes = 2048;
    mem::QueryMemoryArena arena(Context(), limits, &allocator, &temp);
    mem::QueryMemoryGrantRequest request;
    request.family = mem::QueryMemoryFamily::relational;
    request.bytes = 4096;
    auto refused = arena.Grant(request);
    Require(!refused.ok(), "ODF-094 over-family-limit grant was accepted");
    Require(refused.diagnostic.diagnostic_code ==
                "SB_QUERY_MEMORY_ARENA.FAMILY_LIMIT_EXCEEDED",
            "ODF-094 family-limit diagnostic changed");
  }

  {
    auto limits = Limits();
    limits.soft_limit_bytes = 1024;
    limits.allow_spill = false;
    mem::QueryMemoryArena arena(Context(), limits, &allocator, &temp);
    mem::QueryMemoryGrantRequest request;
    request.family = mem::QueryMemoryFamily::search;
    request.bytes = 4096;
    request.spillable = true;
    auto refused = arena.Grant(request);
    Require(!refused.ok(), "ODF-094 disallowed spill was accepted");
    Require(refused.diagnostic.diagnostic_code ==
                "SB_QUERY_MEMORY_ARENA.SOFT_LIMIT_EXCEEDED",
            "ODF-094 spill-disabled diagnostic changed");
  }

  {
    auto limits = Limits();
    limits.soft_limit_bytes = 1024;
    limits.spill_limit_bytes = 2048;
    mem::QueryMemoryArena arena(Context(), limits, &allocator, &temp);
    mem::QueryMemoryGrantRequest request;
    request.family = mem::QueryMemoryFamily::search;
    request.bytes = 4096;
    request.spillable = true;
    auto refused = arena.Grant(request);
    Require(!refused.ok(), "ODF-094 spill quota denial was accepted");
    Require(refused.diagnostic.diagnostic_code ==
                "SB_QUERY_MEMORY_ARENA.SPILL_QUOTA_DENIED",
            "ODF-094 spill quota diagnostic changed");
  }

  {
    auto limits = Limits();
    limits.soft_limit_bytes = 1024;
    limits.spill_limit_bytes = 128 * 1024;
    auto temp_policy =
        TempPolicy(std::filesystem::temp_directory_path() / "sb_odf_094_temp_quota");
    temp_policy.operation_quota_bytes = 2048;
    mem::TempWorkspaceLifecycleManager quota_temp(temp_policy);
    mem::QueryMemoryArena arena(Context(), limits, &allocator, &quota_temp);
    mem::QueryMemoryGrantRequest request;
    request.family = mem::QueryMemoryFamily::search;
    request.bytes = 4096;
    request.spillable = true;
    auto refused = arena.Grant(request);
    Require(!refused.ok(), "ODF-094 temp workspace spill denial was accepted");
    Require(refused.diagnostic.diagnostic_code ==
                "SB_QUERY_MEMORY_ARENA.SPILL_QUOTA_DENIED",
            "ODF-094 temp quota spill diagnostic changed");
    Require(EvidenceHas(refused.evidence,
                        "query_memory_arena.spill_workspace_refused="),
            "ODF-094 temp quota denial evidence missing");
  }

  {
    mem::QueryMemoryArena arena(Context(), Limits(), &allocator, &temp);
    auto refused = arena.Release("missing-grant");
    Require(!refused.ok(), "ODF-094 unknown grant release was accepted");
    Require(refused.diagnostic.diagnostic_code ==
                "SB_QUERY_MEMORY_ARENA.UNKNOWN_GRANT",
            "ODF-094 unknown grant diagnostic changed");
  }
}

}  // namespace

int main() {
  SupportedFamiliesAndRelease();
  BoundedSpillAndCancel();
  FailClosedDiagnostics();
  std::cout << "optimizer_deficiency_odf_094_gate passed\n";
  return EXIT_SUCCESS;
}
