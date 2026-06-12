// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/repair_history_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "repair_event_ledger.hpp"
#include "security/security_model.hpp"
#include "uuid.hpp"

#include <optional>
#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace db = scratchbird::storage::database;

constexpr const char* kOperation = "observability.repair_history.inspect";

std::string UuidText(const scratchbird::core::platform::TypedUuid& uuid) {
  return uuid.valid() ? scratchbird::core::uuid::UuidToString(uuid.value) : "";
}

EngineApiDiagnostic DiagnosticFromStorage(
    const scratchbird::core::platform::DiagnosticRecord& diagnostic,
    const std::string& fallback_code,
    const std::string& fallback_key) {
  std::string detail = diagnostic.remediation_hint;
  for (const auto& argument : diagnostic.arguments) {
    if (!detail.empty()) {
      detail += ";";
    }
    detail += argument.key + "=" + argument.value;
  }
  return MakeEngineApiDiagnostic(diagnostic.diagnostic_code.empty()
                                     ? fallback_code
                                     : diagnostic.diagnostic_code,
                                 diagnostic.message_key.empty() ? fallback_key
                                                                : diagnostic.message_key,
                                 detail,
                                 true);
}

template <typename TResult>
TResult RepairHistoryFailure(const EngineRequestContext& context,
                             EngineApiDiagnostic diagnostic) {
  auto result =
      MakeApiBehaviorDiagnostic<TResult>(context, kOperation, std::move(diagnostic));
  AddApiBehaviorEvidence(&result,
                         "repair_evidence_transaction_authority",
                         "false");
  AddApiBehaviorEvidence(&result,
                         "durable_mga_inventory_authority_required",
                         "true");
  return result;
}

bool HasRepairInspectRight(const EngineRequestContext& context) {
  return SecurityContextHasTag(context, "security.bootstrap") ||
         SecurityContextHasRight(context, "REPAIR_HISTORY_INSPECT") ||
         SecurityContextHasRight(context, "MGA_TRANSACTION_INSPECT") ||
         SecurityContextHasRight(context, "OBS_RUNTIME_ALL");
}

template <typename TResult>
std::optional<TResult> EnforceRepairInspectRight(
    const EngineRequestContext& context) {
  if (!HasRepairInspectRight(context)) {
    return RepairHistoryFailure<TResult>(
        context,
        MakeSecurityDiagnostic("SECURITY.AUTHORIZATION.DENIED",
                               "REPAIR_HISTORY_INSPECT"));
  }
  return std::nullopt;
}

std::string BoolText(bool value) { return value ? "true" : "false"; }

void AddRepairHistoryRow(EngineApiResult* result,
                         const db::RepairHistoryInspectionRow& row) {
  AddApiBehaviorRow(
      result,
      {{"record_kind", db::RepairHistoryRecordKindName(row.record_kind)},
       {"row_uuid", UuidText(row.row_uuid)},
       {"version_uuid", UuidText(row.version_uuid)},
       {"page_uuid", UuidText(row.page_uuid)},
       {"finding_uuid", UuidText(row.finding_uuid)},
       {"operation_uuid", UuidText(row.operation_uuid)},
       {"page_number", std::to_string(row.page_number)},
       {"local_transaction_id", std::to_string(row.local_transaction_id)},
       {"version_sequence", std::to_string(row.version_sequence)},
       {"repair_event_sequence", std::to_string(row.repair_event_sequence)},
       {"repair_event_digest", std::to_string(row.repair_event_digest)},
       {"source_class", row.source_class},
       {"observed_state", row.observed_state},
       {"creator_transaction_state", row.creator_transaction_state},
       {"diagnostic_code", row.diagnostic_code},
       {"detail", row.detail},
       {"payload_present", BoolText(row.payload_present)},
       {"quarantine", BoolText(row.quarantine)},
       {"salvage", BoolText(row.salvage)},
       {"restore_required", BoolText(row.restore_required)},
       {"data_loss_class",
        db::RepairHistoryDataLossClassName(row.data_loss_class)},
       {"repair_evidence_transaction_authority",
        BoolText(row.repair_evidence_is_transaction_authority)}});
}

}  // namespace

EngineInspectRepairHistoryResult EngineInspectRepairHistory(
    const EngineInspectRepairHistoryRequest& request) {
  if (auto denied =
          EnforceRepairInspectRight<EngineInspectRepairHistoryResult>(
              request.context)) {
    return *denied;
  }

  db::RepairHistoryInspectionRequest inspection = request.inspection;
  if (request.load_repair_ledger_from_path) {
    if (request.repair_ledger_path.empty()) {
      return RepairHistoryFailure<EngineInspectRepairHistoryResult>(
          request.context,
          MakeInvalidRequestDiagnostic(kOperation, "repair_ledger_path_required"));
    }
    const auto loaded = db::LoadRepairEventLedger(request.repair_ledger_path);
    if (!loaded.ok()) {
      return RepairHistoryFailure<EngineInspectRepairHistoryResult>(
          request.context,
          DiagnosticFromStorage(loaded.diagnostic,
                                "SB-REPAIR-HISTORY-LEDGER-LOAD-FAILED",
                                "storage.repair_history.ledger_load_failed"));
    }
    inspection.repair_events = loaded.ledger.events;
  }

  const auto inspected = db::InspectRepairHistory(inspection);
  if (!inspected.ok()) {
    return RepairHistoryFailure<EngineInspectRepairHistoryResult>(
        request.context,
        DiagnosticFromStorage(inspected.diagnostic,
                              "SB-REPAIR-HISTORY-INSPECT-FAILED",
                              "storage.repair_history.inspect_failed"));
  }

  auto result =
      MakeApiBehaviorSuccess<EngineInspectRepairHistoryResult>(request.context,
                                                               kOperation);
  result.repair_history_ready = true;
  result.durable_mga_inventory_authority =
      inspected.durable_mga_inventory_authority;
  result.repair_evidence_is_transaction_authority = false;
  result.data_loss_possible = inspected.data_loss_possible;
  result.restore_required = inspected.restore_required;
  result.quarantine_present = inspected.quarantine_present;
  result.ordinary_version_count = inspected.ordinary_version_count;
  result.archive_entry_count = inspected.archive_entry_count;
  result.repair_event_count = inspected.repair_event_count;
  result.salvage_evidence_count = inspected.salvage_evidence_count;
  result.diagnostic_count = inspected.diagnostic_count;

  for (const auto& row : inspected.rows) {
    AddRepairHistoryRow(&result, row);
  }
  AddApiBehaviorEvidence(&result,
                         "repair_history",
                         "ordinary_versions_archive_entries_repair_events");
  AddApiBehaviorEvidence(&result,
                         "durable_mga_inventory_authority",
                         "true");
  AddApiBehaviorEvidence(&result,
                         "repair_evidence_transaction_authority",
                         "false");
  AddApiBehaviorEvidence(&result,
                         "parser_or_reference_authority",
                         "false");
  return result;
}

}  // namespace scratchbird::engine::internal_api
