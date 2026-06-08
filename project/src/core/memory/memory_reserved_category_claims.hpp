// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::memory {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

// MMCH_RESERVED_MEMORY_CATEGORY_CLAIM_GATES
// Reserved memory categories are allowed to exist as accounting hooks, but a
// production claim for a reserved category must have live implementation,
// route, and authority evidence. The category name itself is not authority.
enum class ReservedMemoryClaimKind {
  cluster_memory_pressure_coordination,
  gpu_pinned_device_memory,
  llvm_code_data_lifecycle,
  udr_workspace_governance,
  parser_handoff_workspace,
  deferred_epoch_reclamation,
  version_chain_cleanup,
  durable_temp_resume,
  sparse_physical_reservation,
  generic_heap_leak_detector
};

struct ReservedMemoryClaimRequest {
  ReservedMemoryClaimKind claim_kind =
      ReservedMemoryClaimKind::cluster_memory_pressure_coordination;
  std::string claim_id;
  std::string production_profile = "production";
  u64 policy_generation = 0;
  bool production_claim = true;
  bool implementation_evidence_present = false;
  bool live_route_evidence_present = false;
  bool authoritative_base_input_present = false;
  bool exact_diagnostics_required = true;
  bool parser_or_client_authority = false;
  bool donor_authority = false;
  bool wal_authority = false;
  bool sparse_file_claimed_as_physical_reservation = false;
};

struct ReservedMemoryClaimResult {
  Status status;
  bool accepted = false;
  bool fail_closed = false;
  std::string diagnostic_code;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && accepted && !fail_closed;
  }
};

const char* ReservedMemoryClaimKindName(ReservedMemoryClaimKind kind);
ReservedMemoryClaimResult ValidateReservedMemoryCategoryClaim(
    const ReservedMemoryClaimRequest& request);

}  // namespace scratchbird::core::memory
