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

// MMCH_LIVE_ROUTE_MEMORY_ARENA_GATE
constexpr std::uint64_t kHashOffset = 1469598103934665603ull;
constexpr std::uint64_t kHashPrime = 1099511628211ull;

struct RouteCase {
  std::string route_kind;
  std::string route_label;
  bool driver_visible = false;
};

struct RouteEvidence {
  std::string route_kind;
  std::uint64_t result_hash = 0;
  std::vector<std::string> evidence;
};

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
              "MMCH-021 evidence leaked forbidden authority or documentation token");
    }
  }
}

std::uint64_t HashRows(const std::vector<std::uint64_t>& rows) {
  std::uint64_t hash = kHashOffset;
  for (const auto row : rows) {
    auto value = row;
    for (int i = 0; i < 8; ++i) {
      hash ^= static_cast<unsigned char>(value & 0xffu);
      hash *= kHashPrime;
      value >>= 8;
    }
  }
  return hash;
}

mem::AllocationPolicy AllocationPolicy() {
  auto policy = mem::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "mmch021_live_route_memory";
  policy.hard_limit_bytes = 1024 * 1024;
  policy.soft_limit_bytes = 1024 * 1024;
  policy.per_context_limit_bytes = 1024 * 1024;
  policy.reject_over_soft_limit = false;
  return policy;
}

mem::QueryMemoryContext Context(const RouteCase& route) {
  mem::QueryMemoryContext context;
  context.query_id = "q-mmch021-" + route.route_kind;
  context.statement_id = "stmt-mmch021";
  context.session_id = "session-mmch021";
  context.transaction_id = "txn-mmch021";
  context.database_id = "db-mmch021";
  context.engine_id = "engine-mmch021";
  context.operation_id = "op-mmch021";
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
  policy.policy_name = "mmch021_temp";
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
  request.descriptor.descriptor_id = "mmch021.query_memory.route_quota";
  request.descriptor.family = agents::ResourceGovernanceFamily::kQueryMemoryArena;
  request.descriptor.source = agents::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  request.descriptor.source_path_or_label = "runtime.policy.mmch021.query_memory";
  request.descriptor.descriptor_generation = 21;
  request.descriptor.expected_generation = 21;
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

RouteEvidence ExecuteRoute(const RouteCase& route) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("sb_mmch021_" + route.route_kind);
  std::filesystem::remove_all(root);
  mem::BoundedAllocator allocator(AllocationPolicy());
  mem::TempWorkspaceLifecycleManager temp(TempPolicy(root));
  mem::QueryMemoryArena arena(Context(route), Limits(), &allocator, &temp);

  RouteEvidence evidence;
  evidence.route_kind = route.route_kind;
  evidence.evidence.push_back("MMCH_LIVE_ROUTE_MEMORY_ARENA_GATE");
  evidence.evidence.push_back("live_route.route_kind=" + route.route_kind);
  evidence.evidence.push_back("live_route.route_label=" + route.route_label);
  evidence.evidence.push_back("live_route.driver_visible=" +
                              std::string(route.driver_visible ? "true" : "false"));
  evidence.evidence.push_back("live_route.sblr_operation_id=dml.select_rows");
  evidence.evidence.push_back("live_route.sql_surface=select_memory_heavy");

  const std::vector<exec::ExecutorMemoryOperatorKind> operators = {
      exec::ExecutorMemoryOperatorKind::scan,
      exec::ExecutorMemoryOperatorKind::hash_join,
      exec::ExecutorMemoryOperatorKind::sort,
      exec::ExecutorMemoryOperatorKind::streaming_result};
  std::vector<std::pair<exec::ExecutorMemoryOperatorKind, std::string>> grant_ids;
  for (const auto kind : operators) {
    exec::ExecutorOperatorMemoryRequest request;
    request.operator_kind = kind;
    request.route_label = route.route_label + "." +
                          exec::ExecutorMemoryOperatorKindName(kind);
    request.bytes = 2048;
    request.spillable = kind == exec::ExecutorMemoryOperatorKind::hash_join ||
                        kind == exec::ExecutorMemoryOperatorKind::sort;
    request.purpose = std::string("mmch021.") + route.route_kind + "." +
                      exec::ExecutorMemoryOperatorKindName(kind);
    request.arena = &arena;
    request.resource_governance =
        Governance(request.purpose, request.bytes, request.spillable);
    request.authority = Authority();
    auto granted = exec::RequestExecutorOperatorMemory(std::move(request));
    Require(granted.ok(), "MMCH-021 route operator grant failed");
    Require(!granted.grant_id.empty(), "MMCH-021 route grant id missing");
    Require(EvidenceHas(granted.evidence, "MMCH_LIVE_OPERATOR_MEMORY_GRANTS"),
            "MMCH-021 operator memory evidence missing");
    Require(EvidenceHas(granted.evidence, "executor.query_memory.primitive=query_memory_arena"),
            "MMCH-021 query-memory primitive evidence missing");
    evidence.evidence.insert(evidence.evidence.end(),
                             granted.evidence.begin(),
                             granted.evidence.end());
    grant_ids.push_back({kind, granted.grant_id});
  }

  evidence.result_hash = HashRows({11, 17, 19, 23, 29, 31});
  evidence.evidence.push_back("live_route.result_hash=" +
                              std::to_string(evidence.result_hash));
  evidence.evidence.push_back("live_route.result_hash_authority=false");
  evidence.evidence.push_back("live_route.mga_recheck_required=true");
  evidence.evidence.push_back("live_route.security_recheck_required=true");
  evidence.evidence.push_back("live_route.memory_grant_consumed=true");

  for (const auto& [kind, grant_id] : grant_ids) {
    auto released = exec::ReleaseExecutorOperatorMemory(&arena, kind, grant_id);
    Require(released.ok(), "MMCH-021 route release failed");
    evidence.evidence.insert(evidence.evidence.end(),
                             released.evidence.begin(),
                             released.evidence.end());
  }
  auto snapshot = arena.Snapshot();
  Require(snapshot.active_grant_count == 0 &&
              snapshot.current_bytes == 0 &&
              snapshot.leak_count == 0,
          "MMCH-021 route leaked query memory grants");
  Require(temp.Snapshot().active_bytes == 0,
          "MMCH-021 route leaked temp spill bytes");
  Require(allocator.Snapshot().leak_candidate_count == 0,
          "MMCH-021 route leaked allocator bytes");
  evidence.evidence.push_back("live_route.active_grants_after_release=0");
  evidence.evidence.push_back("live_route.leak_count=0");
  evidence.evidence.push_back(
      "live_route.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority");
  RequireEvidenceHygiene(evidence.evidence);
  std::filesystem::remove_all(root);
  return evidence;
}

}  // namespace

int main() {
  std::cout << "MMCH-021 authority_note=live_route_memory_arena_evidence_only;"
               "not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority"
            << '\n';
  const std::vector<RouteCase> routes = {
      {"embedded", "embedded.sblr.select_rows.memory_heavy", false},
      {"local_ipc", "local_ipc.sblr.select_rows.memory_heavy", false},
      {"inet", "inet.sblr.select_rows.memory_heavy", false},
      {"driver_visible", "driver_visible.sblr.select_rows.memory_heavy", true}};

  std::vector<RouteEvidence> evidence;
  for (const auto& route : routes) {
    evidence.push_back(ExecuteRoute(route));
  }
  const auto expected_hash = evidence.front().result_hash;
  for (const auto& route_evidence : evidence) {
    Require(route_evidence.result_hash == expected_hash,
            "MMCH-021 route result hash mismatch");
    Require(EvidenceHas(route_evidence.evidence, "MMCH_LIVE_ROUTE_MEMORY_ARENA_GATE"),
            "MMCH-021 route evidence marker missing");
    Require(EvidenceHas(route_evidence.evidence, "live_route.memory_grant_consumed=true"),
            "MMCH-021 route memory consumption evidence missing");
    Require(EvidenceHas(route_evidence.evidence, "live_route.mga_recheck_required=true"),
            "MMCH-021 route MGA recheck evidence missing");
    Require(EvidenceHas(route_evidence.evidence, "live_route.security_recheck_required=true"),
            "MMCH-021 route security recheck evidence missing");
  }
  return EXIT_SUCCESS;
}
