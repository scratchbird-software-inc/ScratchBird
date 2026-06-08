// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory.hpp"
#include "operator_typed_arena_work_area.hpp"
#include "optimizer_typed_arena_work_area.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;
namespace mem = scratchbird::core::memory;
namespace opt = scratchbird::engine::optimizer;

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

mem::AllocationPolicy Policy(std::uint64_t bytes = 1024 * 1024) {
  auto policy = mem::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "mmch061_executor_planner_typed_arena";
  policy.hard_limit_bytes = bytes;
  policy.soft_limit_bytes = bytes;
  policy.per_context_limit_bytes = bytes;
  policy.page_buffer_pool_limit_bytes = bytes;
  policy.reject_over_soft_limit = false;
  return policy;
}

void RequireSharedEvidence(const std::vector<std::string>& evidence,
                           std::string_view authority_token) {
  Require(EvidenceHas(evidence, "MMCH_EXECUTOR_PLANNER_TYPED_ARENA_MIGRATION"),
          "MMCH-061 evidence marker missing");
  Require(EvidenceHas(evidence, authority_token),
          "MMCH-061 authority boundary evidence missing");
  Require(EvidenceHas(evidence, "allocation_count_reduced=true"),
          "MMCH-061 allocation reduction evidence missing");
}

void ExecutorAndOptimizerUseTypedArenaWorkAreas() {
  mem::MemoryManager manager(Policy());

  exec::ExecutorTypedArenaWorkAreaRequest executor_request;
  executor_request.memory_manager = &manager;
  executor_request.route_label = "embedded.sql.select.hash_join.mmch061";
  executor_request.row_count = 128;
  auto executor = exec::BuildExecutorTypedArenaWorkArea(executor_request);
  Require(executor.ok(), "MMCH-061 executor typed arena work area failed");
  Require(!executor.result_digest.empty(), "MMCH-061 executor result digest missing");
  Require(executor.typed_arena_allocation_count < executor.baseline_allocation_count,
          "MMCH-061 executor allocation count was not reduced");
  Require(manager.Snapshot().current_bytes == 0,
          "MMCH-061 executor typed arena leaked memory after reset");
  RequireSharedEvidence(
      executor.evidence,
      "executor_typed_arena.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_wal_or_benchmark_authority");

  opt::OptimizerTypedArenaWorkAreaRequest optimizer_request;
  optimizer_request.memory_manager = &manager;
  optimizer_request.planning_route_label = "engine.optimizer.access_path.mmch061";
  optimizer_request.candidate_count = 96;
  auto optimizer = opt::BuildOptimizerTypedArenaWorkArea(optimizer_request);
  Require(optimizer.ok(), "MMCH-061 optimizer typed arena work area failed");
  Require(!optimizer.result_digest.empty(), "MMCH-061 optimizer result digest missing");
  Require(optimizer.typed_arena_allocation_count < optimizer.baseline_allocation_count,
          "MMCH-061 optimizer allocation count was not reduced");
  Require(manager.Snapshot().current_bytes == 0,
          "MMCH-061 optimizer typed arena leaked memory after reset");
  RequireSharedEvidence(
      optimizer.evidence,
      "optimizer_typed_arena.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_wal_or_benchmark_authority");
}

void UnsafeRoutesFailClosed() {
  mem::MemoryManager manager(Policy());

  exec::ExecutorTypedArenaWorkAreaRequest executor_request;
  executor_request.memory_manager = &manager;
  executor_request.route_label = "embedded.sql.select.mmch061";
  executor_request.row_count = 8;
  executor_request.parser_or_donor_authority = true;
  auto executor = exec::BuildExecutorTypedArenaWorkArea(executor_request);
  Require(!executor.ok() && executor.fail_closed,
          "MMCH-061 executor unsafe authority did not fail closed");
  Require(executor.diagnostic.diagnostic_code == "executor_typed_arena_unsafe_authority",
          "MMCH-061 executor unsafe authority diagnostic changed");

  opt::OptimizerTypedArenaWorkAreaRequest optimizer_request;
  optimizer_request.memory_manager = &manager;
  optimizer_request.planning_route_label = "engine.optimizer.mmch061";
  optimizer_request.candidate_count = 8;
  optimizer_request.memory_benchmark_authority = true;
  auto optimizer = opt::BuildOptimizerTypedArenaWorkArea(optimizer_request);
  Require(!optimizer.ok() && optimizer.fail_closed,
          "MMCH-061 optimizer benchmark authority did not fail closed");
  Require(optimizer.diagnostic.diagnostic_code == "optimizer_typed_arena_unsafe_authority",
          "MMCH-061 optimizer unsafe authority diagnostic changed");

  Require(manager.Snapshot().current_bytes == 0,
          "MMCH-061 refused typed arena work leaked memory");
}

}  // namespace

int main() {
  ExecutorAndOptimizerUseTypedArenaWorkAreas();
  UnsafeRoutesFailClosed();
  std::cout << "MMCH-061 authority_note=executor_planner_typed_arena_evidence_only;"
            << " typed_arena_evidence_is_not_transaction_finality_visibility_authorization_recovery_or_benchmark_authority\n";
  return EXIT_SUCCESS;
}
