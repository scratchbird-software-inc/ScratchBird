// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory_reserved_category_claims.hpp"

#include "runtime_platform.hpp"

#include <utility>

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr const char* kAuthorityBoundary =
    "reserved_memory_claim.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_wal_or_benchmark_authority";

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::memory};
}

Status BlockStatus() {
  return {StatusCode::memory_invalid_request, Severity::error,
          Subsystem::memory};
}

bool IsKnownImplementedCategory(ReservedMemoryClaimKind kind) {
  switch (kind) {
    case ReservedMemoryClaimKind::version_chain_cleanup:
    case ReservedMemoryClaimKind::durable_temp_resume:
      return true;
    case ReservedMemoryClaimKind::cluster_memory_pressure_coordination:
    case ReservedMemoryClaimKind::gpu_pinned_device_memory:
    case ReservedMemoryClaimKind::llvm_code_data_lifecycle:
    case ReservedMemoryClaimKind::udr_workspace_governance:
    case ReservedMemoryClaimKind::parser_handoff_workspace:
    case ReservedMemoryClaimKind::deferred_epoch_reclamation:
    case ReservedMemoryClaimKind::sparse_physical_reservation:
    case ReservedMemoryClaimKind::generic_heap_leak_detector:
      return false;
  }
  return false;
}

ReservedMemoryClaimResult Block(const ReservedMemoryClaimRequest& request,
                                std::string code,
                                std::string reason) {
  ReservedMemoryClaimResult result;
  result.status = BlockStatus();
  result.accepted = false;
  result.fail_closed = true;
  result.diagnostic_code = std::move(code);
  result.evidence.push_back("MMCH_RESERVED_MEMORY_CATEGORY_CLAIM_GATES");
  result.evidence.push_back(kAuthorityBoundary);
  result.evidence.push_back("reserved_memory_claim.accepted=false");
  result.evidence.push_back("reserved_memory_claim.reason=" + reason);
  result.evidence.push_back(
      std::string("reserved_memory_claim.kind=") +
      ReservedMemoryClaimKindName(request.claim_kind));
  result.diagnostic = MakeDiagnostic(
      result.status.code, result.status.severity, result.status.subsystem,
      result.diagnostic_code, "memory.reserved_claim.fail_closed",
      {DiagnosticArgument{"claim_kind",
                          ReservedMemoryClaimKindName(request.claim_kind)},
       DiagnosticArgument{"claim_id", request.claim_id},
       DiagnosticArgument{"reason", reason},
       DiagnosticArgument{"authority_scope", kAuthorityBoundary}},
      {}, "memory_reserved_category_claims",
      "Do not advertise this reserved category as production-capable until live implementation, route, and authority evidence exist.");
  return result;
}

}  // namespace

const char* ReservedMemoryClaimKindName(ReservedMemoryClaimKind kind) {
  switch (kind) {
    case ReservedMemoryClaimKind::cluster_memory_pressure_coordination:
      return "cluster_memory_pressure_coordination";
    case ReservedMemoryClaimKind::gpu_pinned_device_memory:
      return "gpu_pinned_device_memory";
    case ReservedMemoryClaimKind::llvm_code_data_lifecycle:
      return "llvm_code_data_lifecycle";
    case ReservedMemoryClaimKind::udr_workspace_governance:
      return "udr_workspace_governance";
    case ReservedMemoryClaimKind::parser_handoff_workspace:
      return "parser_handoff_workspace";
    case ReservedMemoryClaimKind::deferred_epoch_reclamation:
      return "deferred_epoch_reclamation";
    case ReservedMemoryClaimKind::version_chain_cleanup:
      return "version_chain_cleanup";
    case ReservedMemoryClaimKind::durable_temp_resume:
      return "durable_temp_resume";
    case ReservedMemoryClaimKind::sparse_physical_reservation:
      return "sparse_physical_reservation";
    case ReservedMemoryClaimKind::generic_heap_leak_detector:
      return "generic_heap_leak_detector";
  }
  return "unknown";
}

ReservedMemoryClaimResult ValidateReservedMemoryCategoryClaim(
    const ReservedMemoryClaimRequest& request) {
  if (request.claim_id.empty()) {
    return Block(request, "SB_MEMORY_RESERVED_CLAIM.MISSING_ID",
                 "claim_id_required");
  }
  if (request.production_claim && request.policy_generation == 0) {
    return Block(request, "SB_MEMORY_RESERVED_CLAIM.MISSING_POLICY_GENERATION",
                 "policy_generation_required_for_production_claim");
  }
  if (request.parser_or_client_authority || request.reference_authority ||
      request.wal_authority) {
    return Block(request, "SB_MEMORY_RESERVED_CLAIM.UNSAFE_AUTHORITY",
                 "parser_client_reference_or_wal_authority_rejected");
  }
  if (request.claim_kind ==
          ReservedMemoryClaimKind::sparse_physical_reservation &&
      request.sparse_file_claimed_as_physical_reservation) {
    return Block(request, "SB_MEMORY_RESERVED_CLAIM.SPARSE_NOT_PHYSICAL",
                 "sparse_reservation_is_not_physical_disk_reservation");
  }
  if (!IsKnownImplementedCategory(request.claim_kind)) {
    return Block(request, "SB_MEMORY_RESERVED_CLAIM.NOT_IMPLEMENTED",
                 "reserved_category_has_no_live_production_implementation");
  }
  if (!request.implementation_evidence_present ||
      !request.live_route_evidence_present ||
      !request.authoritative_base_input_present) {
    return Block(request, "SB_MEMORY_RESERVED_CLAIM.MISSING_EVIDENCE",
                 "implementation_route_and_authoritative_base_input_required");
  }

  ReservedMemoryClaimResult result;
  result.status = OkStatus();
  result.accepted = true;
  result.fail_closed = false;
  result.diagnostic_code = "SB_MEMORY_RESERVED_CLAIM.ACCEPTED";
  result.evidence.push_back("MMCH_RESERVED_MEMORY_CATEGORY_CLAIM_GATES");
  result.evidence.push_back(kAuthorityBoundary);
  result.evidence.push_back("reserved_memory_claim.accepted=true");
  result.evidence.push_back(std::string("reserved_memory_claim.kind=") +
                            ReservedMemoryClaimKindName(request.claim_kind));
  result.evidence.push_back(
      "reserved_memory_claim.implementation_evidence_present=true");
  result.evidence.push_back("reserved_memory_claim.live_route_evidence_present=true");
  result.evidence.push_back(
      "reserved_memory_claim.authoritative_base_input_present=true");
  result.diagnostic = MakeDiagnostic(
      result.status.code, result.status.severity, result.status.subsystem,
      result.diagnostic_code, "memory.reserved_claim.accepted",
      {DiagnosticArgument{"claim_kind",
                          ReservedMemoryClaimKindName(request.claim_kind)},
       DiagnosticArgument{"claim_id", request.claim_id},
       DiagnosticArgument{"authority_scope", kAuthorityBoundary}},
      {}, "memory_reserved_category_claims", {});
  return result;
}

}  // namespace scratchbird::core::memory
