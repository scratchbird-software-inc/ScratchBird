// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-REPAIR-EVENT-LEDGER-ANCHOR
#include "page_header.hpp"
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::database {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::storage::disk::PageType;

inline constexpr u32 kRepairEventLedgerFormatVersion = 1;

enum class RepairEventPhase : u16 {
  unknown = 0,
  finding_recorded = 1,
  scan_admission = 2,
  mutation_admission = 3,
  page_quarantined = 4,
  page_review_blocked = 5,
  retention_hold_recorded = 6,
  retention_purge_blocked = 7,
  crash_resume_started = 8,
  crash_resume_replay_admitted = 9,
  crash_resume_completed = 10
};

enum class RepairAccessIntent : u16 {
  normal_access = 1,
  repair_scan = 2,
  repair_mutation = 3
};

struct RepairEventAuthority {
  bool durable_mga_inventory_authority = true;
  bool normal_mga_visibility_recheck_required = true;
  bool repair_evidence_is_transaction_finality_authority = false;
  bool repair_evidence_is_visibility_authority = false;
  bool repair_evidence_is_recovery_authority = false;
  bool parser_or_donor_authority = false;
  bool names_are_authority = false;
  bool sblr_or_internal_operation = true;
};

struct RepairEventRecord {
  u32 format_version = kRepairEventLedgerFormatVersion;
  u64 sequence = 0;
  u64 ledger_epoch = 0;
  RepairEventPhase phase = RepairEventPhase::unknown;
  TypedUuid database_uuid;
  TypedUuid operation_uuid;
  TypedUuid finding_uuid;
  TypedUuid page_uuid;
  TypedUuid object_uuid;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  u64 page_number = 0;
  u64 page_generation = 0;
  PageType page_type = PageType::unknown;
  u64 observed_header_checksum = 0;
  u64 observed_body_checksum_low64 = 0;
  u64 observed_body_checksum_high64 = 0;
  u64 previous_event_digest = 0;
  u64 event_digest = 0;
  std::string reason_code;
  std::string stable_detail;
  RepairEventAuthority authority;
};

struct RepairEventLedger {
  std::vector<RepairEventRecord> events;
  bool verified_append_only = false;
  u64 last_sequence = 0;
  u64 last_event_digest = 0;
};

struct RepairEventLedgerResult {
  Status status;
  RepairEventLedger ledger;
  RepairEventRecord event;
  std::string serialized;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct RepairAccessRequest {
  RepairAccessIntent intent = RepairAccessIntent::normal_access;
  TypedUuid operation_uuid;
  TypedUuid finding_uuid;
  TypedUuid page_uuid;
  u64 page_number = 0;
  bool durable_mga_inventory_authority = true;
  bool repair_evidence_is_transaction_authority = false;
  bool parser_or_donor_authority = false;
  bool names_are_authority = false;
};

struct RepairAccessDecision {
  Status status;
  bool admitted = false;
  bool normal_access_allowed = false;
  bool scan_allowed = false;
  bool mutation_allowed = false;
  bool prior_event_persisted = false;
  bool repair_evidence_is_transaction_authority = false;
  u64 prior_event_digest = 0;
  std::vector<std::string> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && admitted; }
};

struct RepairEventRetentionRequest {
  RepairEventLedger ledger;
  u64 now_epoch_millis = 0;
  u64 retention_deadline_epoch_millis = 0;
  bool durable_retention_policy_loaded = false;
  bool purge_requested = false;
  bool legal_hold_active = false;
  bool maintenance_hold_active = false;
  bool repair_evidence_is_transaction_authority = false;
  bool parser_or_donor_authority = false;
  bool names_are_authority = false;
};

struct RepairEventRetentionDecision {
  Status status;
  bool evaluated = false;
  bool purge_allowed = false;
  bool purge_blocked = false;
  bool legal_hold_blocker = false;
  bool maintenance_hold_blocker = false;
  bool retention_deadline_blocker = false;
  bool tamper_chain_verified = false;
  bool repair_evidence_is_transaction_authority = false;
  std::vector<std::string> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && evaluated; }
};

struct RepairCrashResumeRequest {
  RepairEventLedger ledger;
  bool crash_recovery_open = true;
  bool durable_mga_inventory_authority = true;
  bool repair_evidence_is_recovery_authority = false;
  bool parser_or_donor_authority = false;
  bool names_are_authority = false;
};

struct RepairCrashResumeDecision {
  Status status;
  bool evaluated = false;
  bool resume_required = false;
  bool replay_required = false;
  bool completed = false;
  bool tamper_chain_verified = false;
  bool repair_evidence_is_recovery_authority = false;
  RepairEventPhase last_phase = RepairEventPhase::unknown;
  u64 last_event_digest = 0;
  std::vector<std::string> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && evaluated; }
};

const char* RepairEventPhaseName(RepairEventPhase phase);
const char* RepairAccessIntentName(RepairAccessIntent intent);

u64 ComputeRepairEventDigest(const RepairEventRecord& event);
RepairEventLedgerResult BuildRepairEventRecord(RepairEventRecord event);
RepairEventLedgerResult SerializeRepairEventRecord(const RepairEventRecord& event);
RepairEventLedgerResult ParseRepairEventRecord(const std::string& serialized);
RepairEventLedgerResult LoadRepairEventLedger(const std::string& ledger_path);
RepairEventLedgerResult AppendRepairEventToLedger(const std::string& ledger_path,
                                                  RepairEventRecord event);
RepairAccessDecision AdmitRepairAccessFromLedger(
    const RepairEventLedger& ledger,
    const RepairAccessRequest& request);
RepairEventRetentionDecision EvaluateRepairEventRetention(
    const RepairEventRetentionRequest& request);
RepairCrashResumeDecision EvaluateRepairCrashResumeFromLedger(
    const RepairCrashResumeRequest& request);
DiagnosticRecord MakeRepairEventLedgerDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail = {});

}  // namespace scratchbird::storage::database
