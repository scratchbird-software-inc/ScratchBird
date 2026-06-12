// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-REPAIR-IDENTITY-RULES-ANCHOR
#include "row_data_page.hpp"
#include "row_version.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u64;
using scratchbird::transaction::mga::RowVersionMetadata;

enum class RepairIdentityAction : u16 {
  exact_relocation = 1,
  page_rewrite = 2,
  logical_correction = 3,
  salvage_review = 4,
  salvage_promote_with_authority = 5
};

struct RepairIdentityRequest {
  RepairIdentityAction action = RepairIdentityAction::exact_relocation;
  RowDataRecord original_row;
  RowDataRecord candidate_row;
  RowVersionMetadata original_metadata;
  RowVersionMetadata candidate_metadata;
  TypedUuid original_version_uuid;
  TypedUuid candidate_version_uuid;
  bool durable_mga_inventory_authority_available = true;
  bool normal_mga_visibility_recheck_available = true;
  bool repair_event_persisted_before_mutation = true;
  u64 repair_event_digest = 0;
  bool repair_evidence_is_transaction_authority = false;
  bool parser_or_reference_authority = false;
  bool names_are_authority = false;
  bool authoritative_payload_proof = false;
  bool logical_payload_changed = false;
  bool salvage_uncertain = false;
  bool salvage_restore_required = false;
  bool salvage_payload_promoted_to_committed = false;
};

struct RepairIdentityDecision {
  Status status;
  bool accepted = false;
  bool mutation_allowed = false;
  bool exact_identity_preserved = false;
  bool row_uuid_preserved = false;
  bool version_uuid_preserved = false;
  bool logical_correction_created_new_version = false;
  bool salvage_remains_evidence = false;
  bool restore_required = false;
  bool repair_evidence_is_transaction_authority = false;
  RowDataRecord output_row;
  RowVersionMetadata output_metadata;
  TypedUuid output_version_uuid;
  std::vector<std::string> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && accepted; }
};

const char* RepairIdentityActionName(RepairIdentityAction action);
RepairIdentityDecision EvaluateRepairIdentityRule(
    const RepairIdentityRequest& request);
DiagnosticRecord MakeRepairIdentityDiagnostic(Status status,
                                              std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail = {});

}  // namespace scratchbird::storage::page
