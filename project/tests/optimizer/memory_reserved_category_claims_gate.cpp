// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "foreign_memory_reservation.hpp"
#include "llvm_memory_accounting.hpp"
#include "memory_reserved_category_claims.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace memory = scratchbird::core::memory;
using scratchbird::core::platform::u64;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

memory::ReservedMemoryClaimRequest Base(memory::ReservedMemoryClaimKind kind) {
  memory::ReservedMemoryClaimRequest request;
  request.claim_kind = kind;
  request.claim_id = "mmch-reserved-claim";
  request.policy_generation = 7;
  request.production_claim = true;
  return request;
}

void RequireBlocked(memory::ReservedMemoryClaimKind kind, std::string_view why) {
  auto request = Base(kind);
  request.implementation_evidence_present = false;
  request.live_route_evidence_present = false;
  request.authoritative_base_input_present = false;
  auto result = memory::ValidateReservedMemoryCategoryClaim(request);
  Require(!result.ok(), why);
  Require(result.fail_closed, "reserved claim must fail closed");
  Require(!result.diagnostic_code.empty(), "blocked reserved claim needs diagnostic");
}

bool Contains(const std::vector<std::string>& evidence, std::string_view needle) {
  for (const auto& entry : evidence) {
    if (entry.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

memory::HierarchicalMemoryBudgetProvenance Provenance() {
  memory::HierarchicalMemoryBudgetProvenance provenance;
  provenance.source =
      memory::HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  provenance.source_label = "memory_reserved_category_claims_gate";
  provenance.engine_mga_authoritative = true;
  provenance.memory_evidence_only = true;
  return provenance;
}

std::vector<memory::HierarchicalMemoryScopeRef> ScopeChain(
    const std::string& suffix) {
  return {{memory::HierarchicalMemoryScopeKind::process, "mmch-082-process"},
          {memory::HierarchicalMemoryScopeKind::database, "mmch-082-database"},
          {memory::HierarchicalMemoryScopeKind::session,
           "mmch-082-session-" + suffix},
          {memory::HierarchicalMemoryScopeKind::statement,
           "mmch-082-statement-" + suffix}};
}

void SetProcessBudget(memory::HierarchicalMemoryBudgetLedger* ledger,
                      u64 hard_limit) {
  memory::HierarchicalMemoryBudget budget;
  budget.scope = {memory::HierarchicalMemoryScopeKind::process,
                  "mmch-082-process"};
  budget.hard_limit_bytes = hard_limit;
  budget.provenance = Provenance();
  Require(ledger->SetBudget(std::move(budget)).ok(),
          "reserved claim budget setup failed");
}

std::vector<std::string> BuildGpuPinnedRouteEvidence() {
  memory::HierarchicalMemoryBudgetLedger budget_ledger;
  memory::ForeignMemoryReservationLedger foreign_ledger;
  SetProcessBudget(&budget_ledger, 8ull * 1024ull * 1024ull);

  memory::ForeignMemoryReservationRequest request;
  request.source = memory::ForeignMemorySource::gpu_optional;
  request.reservation_ledger = &budget_ledger;
  request.scope_chain = ScopeChain("gpu");
  request.estimated_bytes = 4096;
  request.observed_bytes = 1024;
  request.owner_id = "mmch-082-gpu-owner";
  request.owning_scope = "mmch-082-gpu-scope";
  request.operation_id = "mmch-082-gpu-operation";
  request.native_callsite = "mmch_082.gpu_pinned_device_memory";
  request.confidence = memory::ForeignMemoryConfidence::conservative;
  request.live_route_claim = true;
  request.provenance = Provenance();
  request.authority.provider_available = true;
  request.authority.evidence_label = "mmch_082_gpu_pinned_device_memory";
  request.authority.authority_generation = "mmch-082-runtime";
  request.evidence = {"gpu_pinned_device_memory_route=true",
                      "gpu_provider_available=true",
                      "reserve_before_native_call=true"};

  auto acquired =
      memory::AcquireForeignMemoryReservation(&foreign_ledger, std::move(request));
  Require(acquired.ok(), "gpu pinned memory route reservation failed");
  Require(Contains(acquired.evidence,
                   "CEIC-016_FOREIGN_MEMORY_RESERVATION_COVERAGE"),
          "gpu route evidence missing CEIC-016 anchor");
  Require(Contains(acquired.evidence, "foreign_memory.source=gpu_optional"),
          "gpu route evidence missing source");
  Require(Contains(acquired.evidence,
                   "foreign_memory.category=gpu_device_reserved"),
          "gpu route evidence missing category");
  auto evidence = acquired.evidence;
  Require(acquired.reservation->Release().ok(),
          "gpu pinned memory route release failed");
  Require(foreign_ledger.Snapshot().active_reservation_count == 0,
          "gpu pinned memory route leaked foreign reservation");
  Require(budget_ledger.Snapshot().current_bytes == 0,
          "gpu pinned memory route leaked budget bytes");
  return evidence;
}

std::vector<std::string> BuildLlvmRouteEvidence() {
  memory::HierarchicalMemoryBudgetLedger budget_ledger;
  memory::ForeignMemoryReservationLedger foreign_ledger;
  SetProcessBudget(&budget_ledger, 8ull * 1024ull * 1024ull);

  memory::LlvmMemoryAccountingRequest request;
  request.reservation_ledger = &budget_ledger;
  request.foreign_ledger = &foreign_ledger;
  request.scope_chain = ScopeChain("llvm");
  request.owner_id = "mmch-082-llvm-owner";
  request.owning_scope = "mmch-082-llvm-scope";
  request.operation_id = "mmch-082-llvm-operation";
  request.native_callsite = "mmch_082.llvm_code_data_lifecycle";
  request.provider_label = "mmch-082-configured-llvm-provider";
  request.linkage_mode = memory::ForeignMemoryLinkageMode::dynamic_library;
  request.provider_available = true;
  request.loader_bytes = 512;
  request.code_bytes = 1024;
  request.data_bytes = 768;
  request.native_bytes = 640;
  request.provenance = Provenance();
  request.authority.evidence_label = "mmch_082_llvm_code_data_lifecycle";
  request.authority.authority_generation = "mmch-082-runtime";
  request.evidence = {"llvm_code_data_lifecycle_route=true",
                      "reserve_before_llvm_or_native_call=true",
                      "memory_evidence_only=true"};

  auto acquired =
      memory::AcquireLlvmMemoryAccountingReservation(std::move(request));
  Require(acquired.ok(), "LLVM code/data route reservation failed");
  Require(Contains(acquired.evidence,
                   "CEIC-061_LLVM_DYNAMIC_STATIC_MEMORY_ACCOUNTING"),
          "LLVM route evidence missing CEIC-061 anchor");
  Require(Contains(acquired.evidence,
                   "CEIC-016_FOREIGN_MEMORY_RESERVATION_COVERAGE"),
          "LLVM route evidence missing CEIC-016 anchor");
  auto evidence = acquired.evidence;
  Require(acquired.reservation->Release().ok(),
          "LLVM code/data route release failed");
  Require(foreign_ledger.Snapshot().active_reservation_count == 0,
          "LLVM code/data route leaked foreign reservations");
  Require(budget_ledger.Snapshot().current_bytes == 0,
          "LLVM code/data route leaked budget bytes");
  return evidence;
}

memory::ReservedMemoryClaimResult ValidateImplementedClaim(
    memory::ReservedMemoryClaimKind kind,
    const char* claim_id,
    std::vector<std::string> route_evidence) {
  auto request = Base(kind);
  request.claim_id = claim_id;
  request.implementation_evidence_present = true;
  request.live_route_evidence_present = true;
  request.authoritative_base_input_present = true;
  request.live_route_evidence = std::move(route_evidence);
  return memory::ValidateReservedMemoryCategoryClaim(request);
}

}  // namespace

int main() {
  // MMCH_RESERVED_MEMORY_CATEGORY_CLAIM_GATES
  for (const auto kind : {
           memory::ReservedMemoryClaimKind::cluster_memory_pressure_coordination,
           memory::ReservedMemoryClaimKind::udr_workspace_governance,
           memory::ReservedMemoryClaimKind::parser_handoff_workspace,
           memory::ReservedMemoryClaimKind::deferred_epoch_reclamation,
           memory::ReservedMemoryClaimKind::generic_heap_leak_detector,
       }) {
    RequireBlocked(kind, "unimplemented reserved memory category accepted");
  }

  auto sparse = Base(memory::ReservedMemoryClaimKind::sparse_physical_reservation);
  sparse.sparse_file_claimed_as_physical_reservation = true;
  auto sparse_result = memory::ValidateReservedMemoryCategoryClaim(sparse);
  Require(!sparse_result.ok(), "sparse file must not be physical reservation");
  Require(sparse_result.diagnostic_code ==
              "SB_MEMORY_RESERVED_CLAIM.SPARSE_NOT_PHYSICAL",
          "sparse reservation diagnostic drifted");

  auto unsafe = Base(memory::ReservedMemoryClaimKind::version_chain_cleanup);
  unsafe.implementation_evidence_present = true;
  unsafe.live_route_evidence_present = true;
  unsafe.authoritative_base_input_present = true;
  unsafe.wal_authority = true;
  auto unsafe_result = memory::ValidateReservedMemoryCategoryClaim(unsafe);
  Require(!unsafe_result.ok(), "WAL authority must be rejected");
  Require(unsafe_result.diagnostic_code ==
              "SB_MEMORY_RESERVED_CLAIM.UNSAFE_AUTHORITY",
          "unsafe authority diagnostic drifted");

  auto accepted = Base(memory::ReservedMemoryClaimKind::version_chain_cleanup);
  accepted.claim_id = "mmch-version-chain-cleanup";
  accepted.implementation_evidence_present = true;
  accepted.live_route_evidence_present = true;
  accepted.authoritative_base_input_present = true;
  auto accepted_result = memory::ValidateReservedMemoryCategoryClaim(accepted);
  Require(accepted_result.ok(), "implemented version-chain cleanup claim rejected");
  Require(accepted_result.diagnostic_code == "SB_MEMORY_RESERVED_CLAIM.ACCEPTED",
          "accepted diagnostic drifted");

  auto gpu_missing_route = Base(
      memory::ReservedMemoryClaimKind::gpu_pinned_device_memory);
  gpu_missing_route.implementation_evidence_present = true;
  gpu_missing_route.live_route_evidence_present = true;
  gpu_missing_route.authoritative_base_input_present = true;
  auto gpu_missing_result =
      memory::ValidateReservedMemoryCategoryClaim(gpu_missing_route);
  Require(!gpu_missing_result.ok(),
          "gpu pinned claim accepted without route evidence");
  Require(gpu_missing_result.diagnostic_code ==
              "SB_MEMORY_RESERVED_CLAIM.MISSING_KIND_ROUTE_EVIDENCE",
          "gpu missing-route diagnostic drifted");

  auto llvm_missing_route = Base(
      memory::ReservedMemoryClaimKind::llvm_code_data_lifecycle);
  llvm_missing_route.implementation_evidence_present = true;
  llvm_missing_route.live_route_evidence_present = true;
  llvm_missing_route.authoritative_base_input_present = true;
  auto llvm_missing_result =
      memory::ValidateReservedMemoryCategoryClaim(llvm_missing_route);
  Require(!llvm_missing_result.ok(),
          "LLVM claim accepted without route evidence");
  Require(llvm_missing_result.diagnostic_code ==
              "SB_MEMORY_RESERVED_CLAIM.MISSING_KIND_ROUTE_EVIDENCE",
          "LLVM missing-route diagnostic drifted");

  auto gpu_result = ValidateImplementedClaim(
      memory::ReservedMemoryClaimKind::gpu_pinned_device_memory,
      "mmch-gpu-pinned-device-memory",
      BuildGpuPinnedRouteEvidence());
  Require(gpu_result.ok(), "gpu pinned memory claim rejected with route evidence");
  Require(Contains(gpu_result.evidence, "foreign_memory.source=gpu_optional"),
          "gpu accepted claim omitted live route evidence");
  Require(Contains(gpu_result.evidence,
                   "foreign_memory.category=gpu_device_reserved"),
          "gpu accepted claim omitted memory category evidence");

  auto llvm_result = ValidateImplementedClaim(
      memory::ReservedMemoryClaimKind::llvm_code_data_lifecycle,
      "mmch-llvm-code-data-lifecycle",
      BuildLlvmRouteEvidence());
  Require(llvm_result.ok(), "LLVM code/data claim rejected with route evidence");
  Require(Contains(llvm_result.evidence,
                   "CEIC-061_LLVM_DYNAMIC_STATIC_MEMORY_ACCOUNTING"),
          "LLVM accepted claim omitted live route evidence");

  std::cout << "MMCH_RESERVED_MEMORY_CATEGORY_CLAIM_GATES: PASS\n";
  return EXIT_SUCCESS;
}
