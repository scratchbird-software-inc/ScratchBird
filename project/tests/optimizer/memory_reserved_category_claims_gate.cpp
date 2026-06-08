// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory_reserved_category_claims.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

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

}  // namespace

int main() {
  // MMCH_RESERVED_MEMORY_CATEGORY_CLAIM_GATES
  for (const auto kind : {
           memory::ReservedMemoryClaimKind::cluster_memory_pressure_coordination,
           memory::ReservedMemoryClaimKind::gpu_pinned_device_memory,
           memory::ReservedMemoryClaimKind::llvm_code_data_lifecycle,
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

  std::cout << "MMCH_RESERVED_MEMORY_CATEGORY_CLAIM_GATES: PASS\n";
  return EXIT_SUCCESS;
}
