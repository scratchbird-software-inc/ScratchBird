// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "compression_policy.hpp"
#include "row_data_page.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::byte;
using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;

enum class RowPageCompactEncodingKind : u16 {
  row_page_uncompressed = 1,
  row_page_trimmed_zero_tail = 2
};

enum class RowPageCompactRepairState : u16 {
  validated = 1,
  repaired_from_authoritative_row_page = 2,
  refused = 3
};

struct RowPageCompactAuthorityContext {
  bool authoritative_row_page_source_proven = true;
  bool binary_semantic_equivalence_required = true;
  bool repair_backup_restore_equivalence_required = true;
  bool durable_mga_inventory_authority_available = true;
  bool normal_mga_visibility_authority_available = true;
  bool security_recheck_required = true;
  bool parser_client_or_donor_authority = false;
  bool compact_form_visibility_authority = false;
  bool compact_form_finality_authority = false;
  bool compact_form_recovery_authority = false;
};

struct RowPageCompactRequest {
  scratchbird::storage::page::RowDataPageBody body;
  u32 page_size = 0;
  RowPageCompactAuthorityContext authority;
  CompressionPolicyRequest policy =
      DefaultCompressionPolicyRequest(CompressionFamily::kRowPage);
  bool use_policy = true;
};

struct RowPageCompactRepairAdmission {
  bool repair_admitted = false;
  bool authoritative_row_page_source_available = false;
  bool same_page_identity_proven = false;
  bool backup_restore_manifest_equivalence_proven = false;
  std::string proof_detail;
};

struct RowPageCompactResult {
  Status status;
  bool fail_closed = false;
  bool compressed = false;
  bool fallback_uncompressed = false;
  bool exact_round_trip = false;
  bool backup_restore_equivalent = false;
  bool repaired = false;
  RowPageCompactRepairState repair_state = RowPageCompactRepairState::validated;
  RowPageCompactEncodingKind encoding =
      RowPageCompactEncodingKind::row_page_uncompressed;
  scratchbird::storage::page::RowDataPageBody body;
  std::vector<byte> serialized;
  std::vector<byte> canonical_row_page;
  CompressionPolicyDecision policy_decision;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* RowPageCompactEncodingKindName(RowPageCompactEncodingKind kind);
const char* RowPageCompactRepairStateName(RowPageCompactRepairState state);

RowPageCompactResult BuildRowPageCompactEncoding(
    const RowPageCompactRequest& request);
RowPageCompactResult DecodeRowPageCompactEncoding(
    const std::vector<byte>& serialized,
    const RowPageCompactAuthorityContext& authority);
RowPageCompactResult RepairOrValidateRowPageCompactEncoding(
    const std::vector<byte>& serialized,
    const RowPageCompactAuthorityContext& authority,
    const scratchbird::storage::page::RowDataPageBody* authoritative_source,
    u32 page_size,
    const RowPageCompactRepairAdmission& admission);

DiagnosticRecord MakeRowPageCompactEncodingDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
