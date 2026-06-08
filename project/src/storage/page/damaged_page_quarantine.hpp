// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-DAMAGED-PAGE-QUARANTINE-ANCHOR
#include "page_body_integrity.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u64;

enum class DamagedPageAccessIntent : u16 {
  normal_access = 1,
  repair_scan = 2,
  repair_mutation = 3
};

enum class DamagedPageClassification : u16 {
  normal_access_allowed = 1,
  repair_scan_allowed = 2,
  repair_mutation_allowed = 3,
  quarantine_required = 4,
  review_blocked = 5
};

struct DamagedPageEvidence {
  PageBodyAgreementKind agreement_kind = PageBodyAgreementKind::accepted;
  bool page_header_valid = true;
  bool page_body_checksum_valid = true;
  bool page_body_parse_valid = true;
  bool page_body_family_agreement_valid = true;
  bool page_uuid_valid = true;
  bool durable_mga_inventory_authority_available = false;
  bool normal_mga_visibility_recheck_available = false;
  bool repair_event_persisted_before_access = false;
  u64 repair_event_digest = 0;
  bool repair_evidence_is_transaction_authority = false;
  bool parser_or_donor_authority = false;
  bool names_are_authority = false;
  bool operator_review_required = false;
  bool safe_quarantine_boundary_available = true;
};

struct DamagedPageQuarantineRequest {
  DamagedPageAccessIntent intent = DamagedPageAccessIntent::normal_access;
  u64 page_number = 0;
  DamagedPageEvidence evidence;
};

struct DamagedPageQuarantineDecision {
  Status status;
  DamagedPageClassification classification =
      DamagedPageClassification::review_blocked;
  bool normal_access_allowed = false;
  bool repair_scan_allowed = false;
  bool repair_mutation_allowed = false;
  bool quarantine_required = false;
  bool review_blocked = true;
  bool fail_closed = true;
  bool durable_mga_inventory_authority_required = true;
  bool repair_evidence_is_transaction_authority = false;
  u64 repair_event_digest = 0;
  std::vector<std::string> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* DamagedPageAccessIntentName(DamagedPageAccessIntent intent);
const char* DamagedPageClassificationName(DamagedPageClassification classification);
DamagedPageEvidence MakeDamagedPageEvidenceFromAgreement(
    const PageBodyAgreementResult& agreement);
DamagedPageQuarantineDecision ClassifyDamagedPageAccess(
    const DamagedPageQuarantineRequest& request);
DiagnosticRecord MakeDamagedPageQuarantineDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::storage::page
