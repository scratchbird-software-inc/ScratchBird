// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory.hpp"
#include "transaction_memory_governance.hpp"
#include "transaction_memory_hooks.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

namespace mem = scratchbird::core::memory;
namespace mga = scratchbird::transaction::mga;

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

mem::AllocationPolicy Policy(std::uint64_t bytes) {
  auto policy = mem::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "mmch070_mga_transaction_memory";
  policy.hard_limit_bytes = bytes;
  policy.soft_limit_bytes = bytes;
  policy.per_context_limit_bytes = bytes;
  policy.page_buffer_pool_limit_bytes = bytes;
  policy.reject_over_soft_limit = false;
  return policy;
}

mga::MgaTransactionMemoryRequest Request(mga::MgaTransactionMemoryUseKind use_kind,
                                         std::string transaction,
                                         std::string scope,
                                         std::uint64_t bytes) {
  mga::MgaTransactionMemoryRequest request;
  request.use_kind = use_kind;
  request.transaction_id = std::move(transaction);
  request.scope_id = std::move(scope);
  request.bytes = bytes;
  request.purpose = mga::MgaTransactionMemoryUseKindName(use_kind);
  request.authority.engine_mga_transaction_inventory_authoritative = true;
  return request;
}

void RequireGovernanceEvidence(const mga::MgaTransactionMemoryResult& result) {
  Require(EvidenceHas(result.evidence, "MMCH_MGA_TRANSACTION_MEMORY_GOVERNANCE"),
          "MMCH-070 evidence marker missing");
  Require(EvidenceHas(
              result.evidence,
              "mga_memory.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_wal_or_benchmark_authority"),
          "MMCH-070 authority boundary evidence missing");
}

void HooksAreNowImplementedForGovernedCategories() {
  bool snapshot = false;
  bool version_chain = false;
  bool cleanup = false;
  for (const auto& hook : mga::MgaMemoryHooks()) {
    if (hook.category == mem::MemoryCategory::transaction_snapshot) {
      snapshot = hook.implemented_now;
    }
    if (hook.category == mem::MemoryCategory::version_chain) {
      version_chain = hook.implemented_now;
    }
    if (hook.category == mem::MemoryCategory::cleanup) {
      cleanup = hook.implemented_now;
    }
  }
  Require(snapshot && version_chain && cleanup,
          "MMCH-070 MGA memory hooks were not marked implemented");
}

void TransactionScopesAcquireAndReleaseMemory() {
  mem::MemoryManager manager(Policy(1024 * 1024));
  mga::MgaTransactionMemoryGovernor governor(&manager, 512 * 1024);

  const std::vector<mga::MgaTransactionMemoryUseKind> kinds = {
      mga::MgaTransactionMemoryUseKind::transaction_snapshot,
      mga::MgaTransactionMemoryUseKind::long_reader_snapshot,
      mga::MgaTransactionMemoryUseKind::version_chain_cleanup,
      mga::MgaTransactionMemoryUseKind::rollback_cleanup,
      mga::MgaTransactionMemoryUseKind::abort_cleanup,
      mga::MgaTransactionMemoryUseKind::savepoint_cleanup,
      mga::MgaTransactionMemoryUseKind::cleanup_sweep,
      mga::MgaTransactionMemoryUseKind::recovery_scan};

  std::vector<std::string> ids;
  std::uint64_t expected = 0;
  for (std::size_t i = 0; i < kinds.size(); ++i) {
    const auto result = governor.Acquire(Request(kinds[i],
                                                "tx-mmch070",
                                                "scope-" + std::to_string(i),
                                                4096));
    Require(result.ok(), "MMCH-070 transaction memory reservation failed");
    RequireGovernanceEvidence(result);
    ids.push_back(result.reservation.reservation_id);
    expected += 4096;
  }
  auto snapshot = governor.Snapshot();
  Require(snapshot.current_bytes == expected &&
              snapshot.active_reservations == kinds.size() &&
              manager.Snapshot().current_bytes == expected,
          "MMCH-070 reservation accounting mismatch");
  Require(snapshot.bytes_by_transaction["tx-mmch070"] == expected,
          "MMCH-070 transaction accounting missing");
  Require(snapshot.bytes_by_use[mga::MgaTransactionMemoryUseKind::version_chain_cleanup] == 4096,
          "MMCH-070 version-chain accounting missing");

  const auto released_one = governor.Release(ids.front());
  Require(released_one.ok(), "MMCH-070 single release failed");
  RequireGovernanceEvidence(released_one);
  snapshot = governor.Snapshot();
  Require(snapshot.current_bytes == expected - 4096 &&
              manager.Snapshot().current_bytes == expected - 4096,
          "MMCH-070 single release leaked memory");

  const auto cancelled = governor.CancelTransaction("tx-mmch070");
  Require(cancelled.ok(), "MMCH-070 transaction cancel release failed");
  RequireGovernanceEvidence(cancelled);
  snapshot = governor.Snapshot();
  Require(snapshot.current_bytes == 0 &&
              snapshot.active_reservations == 0 &&
              manager.Snapshot().current_bytes == 0,
          "MMCH-070 transaction cancel leaked memory");
}

void PressureAndUnsafeAuthorityFailClosed() {
  mem::MemoryManager manager(Policy(128 * 1024));
  mga::MgaTransactionMemoryGovernor governor(&manager, 8192);

  const auto first = governor.Acquire(Request(mga::MgaTransactionMemoryUseKind::long_reader_snapshot,
                                             "tx-pressure",
                                             "long-reader",
                                             8192));
  Require(first.ok(), "MMCH-070 pressure setup failed");
  const auto denied = governor.Acquire(Request(mga::MgaTransactionMemoryUseKind::version_chain_cleanup,
                                              "tx-pressure",
                                              "version-cleanup",
                                              1));
  Require(!denied.ok() && denied.fail_closed,
          "MMCH-070 over-budget request did not fail closed");
  Require(denied.diagnostic.diagnostic_code == "mga_memory_budget_exceeded",
          "MMCH-070 over-budget diagnostic changed");
  Require(manager.Snapshot().current_bytes == 8192,
          "MMCH-070 refused over-budget request changed memory accounting");
  RequireGovernanceEvidence(denied);

  auto unsafe = Request(mga::MgaTransactionMemoryUseKind::rollback_cleanup,
                        "tx-unsafe",
                        "rollback",
                        1024);
  unsafe.authority.parser_or_client_authority = true;
  const auto unsafe_result = governor.Acquire(unsafe);
  Require(!unsafe_result.ok() && unsafe_result.fail_closed,
          "MMCH-070 parser/client authority did not fail closed");
  Require(unsafe_result.diagnostic.diagnostic_code == "mga_memory_unsafe_authority",
          "MMCH-070 unsafe authority diagnostic changed");

  unsafe = Request(mga::MgaTransactionMemoryUseKind::abort_cleanup,
                   "tx-unsafe",
                   "abort",
                   1024);
  unsafe.authority.reference_authority = true;
  Require(!governor.Acquire(unsafe).ok(),
          "MMCH-070 reference authority did not fail closed");

  unsafe = Request(mga::MgaTransactionMemoryUseKind::cleanup_sweep,
                   "tx-unsafe",
                   "sweep",
                   1024);
  unsafe.authority.wal_or_redo_authority = true;
  Require(!governor.Acquire(unsafe).ok(),
          "MMCH-070 WAL authority did not fail closed");

  unsafe = Request(mga::MgaTransactionMemoryUseKind::recovery_scan,
                   "tx-unsafe",
                   "recovery",
                   1024);
  unsafe.authority.memory_pressure_finality_authority = true;
  Require(!governor.Acquire(unsafe).ok(),
          "MMCH-070 memory pressure finality authority did not fail closed");

  Require(governor.CancelTransaction("tx-pressure").ok(),
          "MMCH-070 cleanup of pressure reservation failed");
  Require(manager.Snapshot().current_bytes == 0,
          "MMCH-070 pressure cleanup leaked memory");
}

void ScopeReleaseCleansSavepointAndSweepMemory() {
  mem::MemoryManager manager(Policy(128 * 1024));
  mga::MgaTransactionMemoryGovernor governor(&manager, 128 * 1024);
  Require(governor.Acquire(Request(mga::MgaTransactionMemoryUseKind::savepoint_cleanup,
                                  "tx-savepoint",
                                  "savepoint-a",
                                  2048)).ok(),
          "MMCH-070 savepoint reservation failed");
  Require(governor.Acquire(Request(mga::MgaTransactionMemoryUseKind::cleanup_sweep,
                                  "tx-savepoint",
                                  "savepoint-a",
                                  4096)).ok(),
          "MMCH-070 cleanup sweep reservation failed");
  auto released = governor.ReleaseScope("savepoint-a");
  Require(released.ok(), "MMCH-070 scope release failed");
  RequireGovernanceEvidence(released);
  Require(governor.Snapshot().current_bytes == 0 &&
              manager.Snapshot().current_bytes == 0,
          "MMCH-070 scope release leaked memory");
}

}  // namespace

int main() {
  HooksAreNowImplementedForGovernedCategories();
  TransactionScopesAcquireAndReleaseMemory();
  PressureAndUnsafeAuthorityFailClosed();
  ScopeReleaseCleansSavepointAndSweepMemory();
  std::cout << "MMCH-070 authority_note=mga_transaction_memory_evidence_only;"
            << " durable_transaction_inventory_remains_finality_and_recovery_authority\n";
  return EXIT_SUCCESS;
}
