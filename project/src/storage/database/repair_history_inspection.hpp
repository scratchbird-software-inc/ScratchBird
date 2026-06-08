// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-REPAIR-HISTORY-INSPECTION-ANCHOR
#include "repair_event_ledger.hpp"
#include "row_version.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::database {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u64;

enum class RepairHistoryRecordKind : u16 {
  ordinary_version = 1,
  archive_entry = 2,
  repair_event = 3,
  quarantine = 4,
  salvage_evidence = 5,
  diagnostic = 6,
  data_loss_assessment = 7
};

enum class RepairHistoryDataLossClass : u16 {
  none = 0,
  possible = 1,
  restore_required = 2,
  review_required = 3
};

struct RepairOrdinaryVersionRecord {
  scratchbird::transaction::mga::RowVersionMetadata metadata;
  TypedUuid version_uuid;
  TypedUuid page_uuid;
  u64 page_number = 0;
  std::string location_class = "hot_page";
};

struct RepairArchiveEntry {
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  TypedUuid page_uuid;
  TypedUuid object_uuid;
  u64 page_number = 0;
  u64 version_sequence = 0;
  u64 local_transaction_id = 0;
  std::string archive_location_class = "local_archive";
  std::string archive_manifest_digest;
  bool archive_manifest_authoritative = true;
  bool payload_present = true;
  bool repair_evidence_is_transaction_authority = false;
};

struct RepairSalvageEvidence {
  TypedUuid finding_uuid;
  TypedUuid page_uuid;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  u64 page_number = 0;
  std::string salvage_class = "review_only";
  bool uncertain = true;
  bool restore_required = false;
  bool payload_promoted_to_committed = false;
  bool repair_evidence_is_transaction_authority = false;
};

struct RepairDiagnosticEvidence {
  TypedUuid row_uuid;
  TypedUuid page_uuid;
  u64 page_number = 0;
  std::string diagnostic_code;
  std::string message_key;
  std::string detail;
  bool error = true;
};

struct RepairHistoryInspectionRequest {
  std::vector<RepairOrdinaryVersionRecord> ordinary_versions;
  std::vector<RepairArchiveEntry> archive_entries;
  std::vector<RepairEventRecord> repair_events;
  std::vector<RepairSalvageEvidence> salvage_evidence;
  std::vector<RepairDiagnosticEvidence> diagnostics;
  TypedUuid row_uuid_filter;
  TypedUuid page_uuid_filter;
  u64 page_number_filter = 0;
  bool durable_mga_inventory_authority = true;
  bool repair_evidence_is_transaction_authority = false;
  bool parser_or_donor_authority = false;
  bool names_are_authority = false;
  bool include_diagnostics = true;
};

struct RepairHistoryInspectionRow {
  RepairHistoryRecordKind record_kind = RepairHistoryRecordKind::diagnostic;
  RepairHistoryDataLossClass data_loss_class =
      RepairHistoryDataLossClass::none;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  TypedUuid page_uuid;
  TypedUuid finding_uuid;
  TypedUuid operation_uuid;
  u64 page_number = 0;
  u64 local_transaction_id = 0;
  u64 version_sequence = 0;
  u64 repair_event_sequence = 0;
  u64 repair_event_digest = 0;
  std::string source_class;
  std::string observed_state;
  std::string creator_transaction_state;
  std::string diagnostic_code;
  std::string detail;
  bool payload_present = false;
  bool quarantine = false;
  bool salvage = false;
  bool restore_required = false;
  bool repair_evidence_is_transaction_authority = false;
};

struct RepairHistoryInspectionResult {
  Status status;
  bool inspection_ready = false;
  bool durable_mga_inventory_authority = false;
  bool repair_evidence_is_transaction_authority = false;
  bool data_loss_possible = false;
  bool restore_required = false;
  bool quarantine_present = false;
  u64 ordinary_version_count = 0;
  u64 archive_entry_count = 0;
  u64 repair_event_count = 0;
  u64 salvage_evidence_count = 0;
  u64 diagnostic_count = 0;
  std::vector<RepairHistoryInspectionRow> rows;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && inspection_ready; }
};

const char* RepairHistoryRecordKindName(RepairHistoryRecordKind kind);
const char* RepairHistoryDataLossClassName(RepairHistoryDataLossClass value);
RepairHistoryInspectionResult InspectRepairHistory(
    const RepairHistoryInspectionRequest& request);
DiagnosticRecord MakeRepairHistoryInspectionDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::storage::database
