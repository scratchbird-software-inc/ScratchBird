// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "repair_history_inspection.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::storage::database {
namespace {

namespace mga = scratchbird::transaction::mga;

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

Status InspectionOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_disk};
}

Status InspectionErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::storage_disk};
}

RepairHistoryInspectionResult InspectionError(std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail = {}) {
  RepairHistoryInspectionResult result;
  result.status = InspectionErrorStatus();
  result.repair_evidence_is_transaction_authority = false;
  result.diagnostic = MakeRepairHistoryInspectionDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  return result;
}

bool SameTypedUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool MatchesRowFilter(const RepairHistoryInspectionRequest& request,
                      const TypedUuid& row_uuid) {
  return !request.row_uuid_filter.valid() ||
         SameTypedUuid(request.row_uuid_filter, row_uuid);
}

bool MatchesPageFilter(const RepairHistoryInspectionRequest& request,
                       const TypedUuid& page_uuid,
                       u64 page_number) {
  if (request.page_uuid_filter.valid() &&
      !SameTypedUuid(request.page_uuid_filter, page_uuid)) {
    return false;
  }
  return request.page_number_filter == 0 ||
         request.page_number_filter == page_number;
}

bool IncludeForFilters(const RepairHistoryInspectionRequest& request,
                       const TypedUuid& row_uuid,
                       const TypedUuid& page_uuid,
                       u64 page_number) {
  return MatchesRowFilter(request, row_uuid) &&
         MatchesPageFilter(request, page_uuid, page_number);
}

bool ValidOptionalTypedUuid(const TypedUuid& uuid, UuidKind kind) {
  return !uuid.valid() || uuid.kind == kind;
}

RepairHistoryInspectionRow AssessmentRow(RepairHistoryDataLossClass loss_class,
                                         bool restore_required,
                                         bool quarantine_present) {
  RepairHistoryInspectionRow row;
  row.record_kind = RepairHistoryRecordKind::data_loss_assessment;
  row.data_loss_class = loss_class;
  row.source_class = "derived_assessment";
  row.observed_state = RepairHistoryDataLossClassName(loss_class);
  row.restore_required = restore_required;
  row.quarantine = quarantine_present;
  row.repair_evidence_is_transaction_authority = false;
  return row;
}

void UpdateLoss(RepairHistoryInspectionResult* result,
                RepairHistoryDataLossClass loss_class) {
  if (loss_class == RepairHistoryDataLossClass::none) {
    return;
  }
  result->data_loss_possible = true;
  if (loss_class == RepairHistoryDataLossClass::restore_required) {
    result->restore_required = true;
  }
}

RepairHistoryDataLossClass CombineLoss(const RepairHistoryInspectionResult& result) {
  if (result.restore_required) {
    return RepairHistoryDataLossClass::restore_required;
  }
  if (result.quarantine_present) {
    return RepairHistoryDataLossClass::review_required;
  }
  if (result.data_loss_possible) {
    return RepairHistoryDataLossClass::possible;
  }
  return RepairHistoryDataLossClass::none;
}

bool RepairEventMatchesFilters(const RepairHistoryInspectionRequest& request,
                               const RepairEventRecord& event) {
  return IncludeForFilters(request,
                           event.row_uuid,
                           event.page_uuid,
                           event.page_number);
}

RepairHistoryDataLossClass EventLossClass(const RepairEventRecord& event) {
  if (event.phase == RepairEventPhase::page_review_blocked) {
    return RepairHistoryDataLossClass::review_required;
  }
  if (event.phase == RepairEventPhase::page_quarantined) {
    return RepairHistoryDataLossClass::possible;
  }
  return RepairHistoryDataLossClass::none;
}

RepairHistoryInspectionRow RowForOrdinaryVersion(
    const RepairOrdinaryVersionRecord& record) {
  RepairHistoryInspectionRow row;
  row.record_kind = RepairHistoryRecordKind::ordinary_version;
  row.row_uuid = record.metadata.identity.row.row_uuid;
  row.version_uuid = record.version_uuid;
  row.page_uuid = record.page_uuid;
  row.page_number = record.page_number;
  row.local_transaction_id =
      record.metadata.identity.creator_transaction.local_id.value;
  row.version_sequence = record.metadata.identity.version_sequence;
  row.source_class = record.location_class;
  row.observed_state = mga::RowVersionStateName(record.metadata.state);
  row.creator_transaction_state =
      mga::TransactionStateName(record.metadata.creator_transaction_state);
  row.payload_present = record.metadata.payload_present;
  row.repair_evidence_is_transaction_authority = false;
  return row;
}

RepairHistoryInspectionRow RowForArchiveEntry(
    const RepairArchiveEntry& entry) {
  RepairHistoryInspectionRow row;
  row.record_kind = RepairHistoryRecordKind::archive_entry;
  row.data_loss_class =
      entry.payload_present && entry.archive_manifest_authoritative
          ? RepairHistoryDataLossClass::none
          : RepairHistoryDataLossClass::restore_required;
  row.row_uuid = entry.row_uuid;
  row.version_uuid = entry.version_uuid;
  row.page_uuid = entry.page_uuid;
  row.page_number = entry.page_number;
  row.local_transaction_id = entry.local_transaction_id;
  row.version_sequence = entry.version_sequence;
  row.source_class = entry.archive_location_class;
  row.observed_state = entry.archive_manifest_authoritative
                           ? "archive_manifest_authoritative"
                           : "archive_manifest_review_required";
  row.detail = entry.archive_manifest_digest;
  row.payload_present = entry.payload_present;
  row.restore_required =
      row.data_loss_class == RepairHistoryDataLossClass::restore_required;
  row.repair_evidence_is_transaction_authority = false;
  return row;
}

RepairHistoryInspectionRow RowForRepairEvent(
    const RepairEventRecord& event) {
  RepairHistoryInspectionRow row;
  row.record_kind = event.phase == RepairEventPhase::page_quarantined ||
                            event.phase == RepairEventPhase::page_review_blocked
                        ? RepairHistoryRecordKind::quarantine
                        : RepairHistoryRecordKind::repair_event;
  row.data_loss_class = EventLossClass(event);
  row.row_uuid = event.row_uuid;
  row.version_uuid = event.version_uuid;
  row.page_uuid = event.page_uuid;
  row.finding_uuid = event.finding_uuid;
  row.operation_uuid = event.operation_uuid;
  row.page_number = event.page_number;
  row.local_transaction_id = event.local_transaction_id;
  row.repair_event_sequence = event.sequence;
  row.repair_event_digest = event.event_digest;
  row.source_class = "repair_event_ledger";
  row.observed_state = RepairEventPhaseName(event.phase);
  row.detail = event.reason_code;
  row.quarantine = row.record_kind == RepairHistoryRecordKind::quarantine;
  row.repair_evidence_is_transaction_authority = false;
  return row;
}

RepairHistoryInspectionRow RowForSalvage(
    const RepairSalvageEvidence& evidence) {
  RepairHistoryInspectionRow row;
  row.record_kind = RepairHistoryRecordKind::salvage_evidence;
  row.data_loss_class =
      evidence.restore_required
          ? RepairHistoryDataLossClass::restore_required
          : (evidence.uncertain ? RepairHistoryDataLossClass::possible
                                : RepairHistoryDataLossClass::none);
  row.row_uuid = evidence.row_uuid;
  row.version_uuid = evidence.version_uuid;
  row.page_uuid = evidence.page_uuid;
  row.finding_uuid = evidence.finding_uuid;
  row.page_number = evidence.page_number;
  row.source_class = "salvage_evidence";
  row.observed_state = evidence.salvage_class;
  row.detail = evidence.uncertain ? "uncertain" : "bounded";
  row.salvage = true;
  row.restore_required = evidence.restore_required;
  row.repair_evidence_is_transaction_authority = false;
  return row;
}

RepairHistoryInspectionRow RowForDiagnostic(
    const RepairDiagnosticEvidence& diagnostic) {
  RepairHistoryInspectionRow row;
  row.record_kind = RepairHistoryRecordKind::diagnostic;
  row.data_loss_class = diagnostic.error
                            ? RepairHistoryDataLossClass::review_required
                            : RepairHistoryDataLossClass::none;
  row.row_uuid = diagnostic.row_uuid;
  row.page_uuid = diagnostic.page_uuid;
  row.page_number = diagnostic.page_number;
  row.source_class = "repair_diagnostic";
  row.observed_state = diagnostic.error ? "error" : "info";
  row.diagnostic_code = diagnostic.diagnostic_code;
  row.detail = diagnostic.detail.empty() ? diagnostic.message_key
                                         : diagnostic.detail;
  row.repair_evidence_is_transaction_authority = false;
  return row;
}

}  // namespace

const char* RepairHistoryRecordKindName(RepairHistoryRecordKind kind) {
  switch (kind) {
    case RepairHistoryRecordKind::ordinary_version: return "ordinary_version";
    case RepairHistoryRecordKind::archive_entry: return "archive_entry";
    case RepairHistoryRecordKind::repair_event: return "repair_event";
    case RepairHistoryRecordKind::quarantine: return "quarantine";
    case RepairHistoryRecordKind::salvage_evidence: return "salvage_evidence";
    case RepairHistoryRecordKind::diagnostic: return "diagnostic";
    case RepairHistoryRecordKind::data_loss_assessment:
      return "data_loss_assessment";
  }
  return "diagnostic";
}

const char* RepairHistoryDataLossClassName(RepairHistoryDataLossClass value) {
  switch (value) {
    case RepairHistoryDataLossClass::none: return "none";
    case RepairHistoryDataLossClass::possible: return "possible";
    case RepairHistoryDataLossClass::restore_required:
      return "restore_required";
    case RepairHistoryDataLossClass::review_required: return "review_required";
  }
  return "review_required";
}

RepairHistoryInspectionResult InspectRepairHistory(
    const RepairHistoryInspectionRequest& request) {
  if (!request.durable_mga_inventory_authority ||
      request.repair_evidence_is_transaction_authority ||
      request.parser_or_donor_authority ||
      request.names_are_authority) {
    return InspectionError("SB-REPAIR-HISTORY-AUTHORITY-REFUSED",
                           "storage.repair_history_inspection.authority_refused");
  }
  if (request.row_uuid_filter.valid() &&
      request.row_uuid_filter.kind != UuidKind::row) {
    return InspectionError("SB-REPAIR-HISTORY-ROW-FILTER-INVALID",
                           "storage.repair_history_inspection.row_filter_invalid");
  }
  if (request.page_uuid_filter.valid() &&
      request.page_uuid_filter.kind != UuidKind::page) {
    return InspectionError("SB-REPAIR-HISTORY-PAGE-FILTER-INVALID",
                           "storage.repair_history_inspection.page_filter_invalid");
  }

  RepairHistoryInspectionResult result;
  result.status = InspectionOkStatus();
  result.inspection_ready = true;
  result.durable_mga_inventory_authority = true;
  result.repair_evidence_is_transaction_authority = false;

  for (const auto& ordinary : request.ordinary_versions) {
    const auto validated = mga::ValidateRowVersionMetadata(ordinary.metadata);
    if (!validated.ok()) {
      result.status = validated.status;
      result.inspection_ready = false;
      result.diagnostic = validated.diagnostic;
      return result;
    }
    if (!ordinary.version_uuid.valid() ||
        ordinary.version_uuid.kind != UuidKind::row ||
        !ordinary.page_uuid.valid() ||
        ordinary.page_uuid.kind != UuidKind::page ||
        ordinary.page_number == 0) {
      return InspectionError(
          "SB-REPAIR-HISTORY-ORDINARY-VERSION-IDENTITY-INVALID",
          "storage.repair_history_inspection.ordinary_version_identity_invalid");
    }
    if (!IncludeForFilters(request,
                           ordinary.metadata.identity.row.row_uuid,
                           ordinary.page_uuid,
                           ordinary.page_number)) {
      continue;
    }
    result.rows.push_back(RowForOrdinaryVersion(ordinary));
    ++result.ordinary_version_count;
  }

  for (const auto& entry : request.archive_entries) {
    if (entry.repair_evidence_is_transaction_authority ||
        !entry.row_uuid.valid() ||
        !entry.version_uuid.valid() ||
        !ValidOptionalTypedUuid(entry.page_uuid, UuidKind::page) ||
        entry.version_sequence == 0 ||
        entry.local_transaction_id == 0) {
      return InspectionError("SB-REPAIR-HISTORY-ARCHIVE-ENTRY-INVALID",
                             "storage.repair_history_inspection.archive_entry_invalid");
    }
    if (!IncludeForFilters(request,
                           entry.row_uuid,
                           entry.page_uuid,
                           entry.page_number)) {
      continue;
    }
    auto row = RowForArchiveEntry(entry);
    UpdateLoss(&result, row.data_loss_class);
    result.rows.push_back(std::move(row));
    ++result.archive_entry_count;
  }

  for (const auto& event : request.repair_events) {
    const auto serialized = SerializeRepairEventRecord(event);
    if (!serialized.ok()) {
      result.status = serialized.status;
      result.inspection_ready = false;
      result.diagnostic = serialized.diagnostic;
      return result;
    }
    if (!RepairEventMatchesFilters(request, event)) {
      continue;
    }
    auto row = RowForRepairEvent(event);
    if (row.quarantine) {
      result.quarantine_present = true;
    }
    UpdateLoss(&result, row.data_loss_class);
    result.rows.push_back(std::move(row));
    ++result.repair_event_count;
  }

  for (const auto& salvage : request.salvage_evidence) {
    if (salvage.repair_evidence_is_transaction_authority ||
        salvage.payload_promoted_to_committed ||
        !salvage.finding_uuid.valid() ||
        !ValidOptionalTypedUuid(salvage.page_uuid, UuidKind::page) ||
        !ValidOptionalTypedUuid(salvage.row_uuid, UuidKind::row) ||
        !ValidOptionalTypedUuid(salvage.version_uuid, UuidKind::row) ||
        salvage.page_number == 0) {
      return InspectionError("SB-REPAIR-HISTORY-SALVAGE-INVALID",
                             "storage.repair_history_inspection.salvage_invalid");
    }
    if (!IncludeForFilters(request,
                           salvage.row_uuid,
                           salvage.page_uuid,
                           salvage.page_number)) {
      continue;
    }
    auto row = RowForSalvage(salvage);
    UpdateLoss(&result, row.data_loss_class);
    result.rows.push_back(std::move(row));
    ++result.salvage_evidence_count;
  }

  if (request.include_diagnostics) {
    for (const auto& diagnostic : request.diagnostics) {
      if (diagnostic.diagnostic_code.empty()) {
        return InspectionError("SB-REPAIR-HISTORY-DIAGNOSTIC-INVALID",
                               "storage.repair_history_inspection.diagnostic_invalid");
      }
      if (!IncludeForFilters(request,
                             diagnostic.row_uuid,
                             diagnostic.page_uuid,
                             diagnostic.page_number)) {
        continue;
      }
      auto row = RowForDiagnostic(diagnostic);
      UpdateLoss(&result, row.data_loss_class);
      result.rows.push_back(std::move(row));
      ++result.diagnostic_count;
    }
  }

  const RepairHistoryDataLossClass loss_class = CombineLoss(result);
  result.rows.push_back(
      AssessmentRow(loss_class, result.restore_required, result.quarantine_present));
  return result;
}

DiagnosticRecord MakeRepairHistoryInspectionDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.database.repair_history_inspection");
}

}  // namespace scratchbird::storage::database
