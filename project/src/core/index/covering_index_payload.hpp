// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-COVERING-INDEX-PAYLOAD-ANCHOR
// Covering payload records are projection evidence bound to a physical
// B-tree row/version locator. They do not decide visibility, authorization,
// transaction finality, cleanup, or recovery.

#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr u32 kCoveringIndexPayloadLayoutVersion = 1;

enum class CoveringIndexPayloadValueKind : u32 {
  null_value = 0,
  inline_value = 1,
  large_payload_reference = 2
};

enum class CoveringIndexPayloadAdmissionKind : u32 {
  refused = 0,
  base_row_recheck = 1,
  index_only = 2
};

struct CoveringIndexPayloadColumnRef {
  TypedUuid column_uuid;
  TypedUuid type_descriptor_uuid;
  u32 projection_ordinal = 0;
  bool required = true;
};

struct CoveringIndexLargePayloadReference {
  TypedUuid payload_uuid;
  TypedUuid owner_object_uuid;
  TypedUuid generation_scope_uuid;
  u64 generation = 0;
  u64 byte_count = 0;
  std::string descriptor_hash;
};

struct CoveringIndexPayloadColumnValue {
  TypedUuid column_uuid;
  u32 projection_ordinal = 0;
  CoveringIndexPayloadValueKind kind =
      CoveringIndexPayloadValueKind::inline_value;
  std::vector<byte> encoded_value;
  CoveringIndexLargePayloadReference large_payload;
  bool binary_result_frame_compatible = false;
  bool redaction_safe = false;
  bool redacted = false;
  bool protected_value = false;
  bool unredacted_authorized = false;
};

struct CoveringIndexPayloadRecord {
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  std::string descriptor_result_contract_hash;
  u64 payload_generation = 0;
  u64 redaction_policy_epoch = 0;
  u64 security_policy_epoch = 0;
  u64 freshness_generation = 0;
  std::vector<CoveringIndexPayloadColumnValue> values;
  std::vector<byte> physical_payload;
  std::string row_shape_hash;
  bool projection_only = true;
  bool binary_result_frame_compatible = true;
  bool visibility_authority = false;
  bool authorization_authority = false;
  bool transaction_finality_authority = false;
  bool cleanup_authority = false;
  bool recovery_authority = false;
};

struct CoveringIndexPayloadAssemblyRequest {
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  std::string descriptor_result_contract_hash;
  u64 payload_generation = 0;
  u64 redaction_policy_epoch = 0;
  u64 security_policy_epoch = 0;
  u64 freshness_generation = 0;
  std::vector<CoveringIndexPayloadColumnRef> descriptor_columns;
  std::vector<TypedUuid> projected_column_uuids;
  std::vector<CoveringIndexPayloadColumnValue> values;
  bool projection_only = true;
  bool result_contract_bound = false;
  bool parser_or_donor_finality_authority = false;
  bool client_finality_authority = false;
  bool provider_finality_authority = false;
};

struct CoveringIndexPayloadAssemblyResult {
  Status status;
  bool assembled = false;
  bool fail_closed = false;
  CoveringIndexPayloadRecord record;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && assembled && !fail_closed; }
};

struct CoveringIndexPayloadLocatorEvidence {
  std::vector<byte> encoded_key;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  u64 leaf_page_number = 0;
  u32 cell_ordinal = 0;
  bool physical_btree_locator_scan = false;
};

struct CoveringIndexExpectedLargePayload {
  TypedUuid column_uuid;
  CoveringIndexLargePayloadReference descriptor;
};

struct CoveringIndexPayloadValidationRequest {
  CoveringIndexPayloadRecord record;
  CoveringIndexPayloadLocatorEvidence locator;
  std::vector<CoveringIndexPayloadColumnRef> required_columns;
  std::vector<TypedUuid> projected_column_uuids;
  std::vector<CoveringIndexExpectedLargePayload> expected_large_payloads;
  std::string expected_descriptor_result_contract_hash;
  u64 expected_payload_generation = 0;
  u64 expected_redaction_policy_epoch = 0;
  u64 expected_security_policy_epoch = 0;
  u64 expected_freshness_generation = 0;
  bool descriptor_epoch_current = false;
  bool result_contract_current = false;
  bool redaction_epoch_current = false;
  bool security_epoch_current = false;
  bool freshness_current = false;
  bool result_frame_contract_proven = false;
  bool redaction_policy_safe = false;
  bool exact_predicate_recheck_planned = false;
  bool mga_visibility_recheck_planned = false;
  bool security_authorization_recheck_planned = false;
  bool exact_predicate_rechecked_by_engine = false;
  bool mga_visibility_rechecked_by_engine = false;
  bool security_authorized_by_engine = false;
  bool base_row_recheck_available = false;
  bool allow_index_only = false;
  bool parser_or_donor_finality_authority = false;
  bool client_finality_authority = false;
  bool provider_finality_authority = false;
};

struct CoveringIndexPayloadAdmission {
  Status status;
  bool admitted = false;
  bool fail_closed = false;
  CoveringIndexPayloadAdmissionKind admission_kind =
      CoveringIndexPayloadAdmissionKind::refused;
  bool index_only_admitted = false;
  bool base_row_recheck_required = true;
  bool base_row_recheck_handoff_proven = false;
  bool payload_projection_only = true;
  bool result_frame_compatible = false;
  bool visibility_authority = false;
  bool authorization_authority = false;
  bool transaction_finality_authority = false;
  bool cleanup_authority = false;
  bool recovery_authority = false;
  CoveringIndexPayloadRecord record;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> blockers;

  bool ok() const { return status.ok() && admitted && !fail_closed; }
};

CoveringIndexPayloadAssemblyResult AssembleCoveringIndexPayload(
    const CoveringIndexPayloadAssemblyRequest& request);

CoveringIndexPayloadAdmission ValidateCoveringIndexPayloadForLocator(
    const CoveringIndexPayloadValidationRequest& request);

const char* CoveringIndexPayloadValueKindName(
    CoveringIndexPayloadValueKind kind);
const char* CoveringIndexPayloadAdmissionKindName(
    CoveringIndexPayloadAdmissionKind kind);

DiagnosticRecord MakeCoveringIndexPayloadDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
