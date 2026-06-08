// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "row_data_physical_sweep.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace scratchbird::storage::page {
namespace {

namespace mga = scratchbird::transaction::mga;

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status SweepOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status SweepErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::storage_page};
}

DiagnosticRecord MakeSweepDiagnostic(Status status,
                                     std::string diagnostic_code,
                                     std::string message_key,
                                     std::string detail = {}) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.page.row_data.physical_sweep");
}

RowDataPhysicalSweepResult SweepError(std::string diagnostic_code,
                                      std::string message_key,
                                      std::string detail = {}) {
  RowDataPhysicalSweepResult result;
  result.status = SweepErrorStatus();
  result.diagnostic = MakeSweepDiagnostic(result.status,
                                          std::move(diagnostic_code),
                                          std::move(message_key),
                                          std::move(detail));
  return result;
}

bool SameUuid(const TypedUuid& lhs, const TypedUuid& rhs) {
  return lhs.kind == rhs.kind && lhs.value == rhs.value;
}

bool EvidenceMatchesRow(const mga::LocalCleanupReclaimEvidenceRecord& evidence,
                        const RowDataRecord& row) {
  return SameUuid(evidence.row_version_identity.row.row_uuid, row.row_uuid) &&
         SameUuid(evidence.row_version_identity.creator_transaction
                      .transaction_uuid,
                  row.transaction_uuid) &&
         evidence.row_version_identity.creator_transaction.local_id.value ==
             row.local_transaction_id &&
         evidence.row_version_identity.version_sequence == row.row_version;
}

const mga::LocalCleanupReclaimEvidenceRecord* MatchingEvidence(
    const std::vector<mga::LocalCleanupReclaimEvidenceRecord>& evidence_records,
    const RowDataRecord& row) {
  for (const auto& evidence : evidence_records) {
    if (EvidenceMatchesRow(evidence, row)) {
      return &evidence;
    }
  }
  return nullptr;
}

bool EvidenceIdSeen(const std::vector<std::string>& seen,
                    const std::string& evidence_id) {
  return std::find(seen.begin(), seen.end(), evidence_id) != seen.end();
}

}  // namespace

RowDataPhysicalSweepResult ApplyRowDataPhysicalSweep(
    const RowDataPhysicalSweepRequest& request) {
  const u64 scanned = static_cast<u64>(request.page.rows.size());
  if (!request.engine_mga_authoritative) {
    return SweepError("SB-ROW-DATA-PHYSICAL-SWEEP-MGA-AUTHORITY-REQUIRED",
                      "storage.row_data_page.physical_sweep_mga_authority_required");
  }
  if (!request.sweep.ok() ||
      !request.sweep.cleanup.cleanup_horizon_authoritative) {
    return SweepError("SB-ROW-DATA-PHYSICAL-SWEEP-CLEANUP-REFUSED",
                      "storage.row_data_page.physical_sweep_cleanup_refused",
                      request.sweep.diagnostic.diagnostic_code);
  }
  if (request.sweep.cleanup.physical_storage_mutated) {
    return SweepError("SB-ROW-DATA-PHYSICAL-SWEEP-DOUBLE-MUTATION-REFUSED",
                      "storage.row_data_page.physical_sweep_double_mutation_refused");
  }
  if (request.page_size == 0) {
    return SweepError("SB-ROW-DATA-PHYSICAL-SWEEP-PAGE-SIZE-REQUIRED",
                      "storage.row_data_page.physical_sweep_page_size_required");
  }
  if (request.max_reclaim_rows == 0 || scanned > request.max_reclaim_rows) {
    return SweepError("SB-ROW-DATA-PHYSICAL-SWEEP-BOUNDED-SCAN-REQUIRED",
                      "storage.row_data_page.physical_sweep_bounded_scan_required",
                      std::to_string(request.max_reclaim_rows));
  }
  const auto& evidence_records = request.sweep.cleanup.reclaim_evidence_records;
  if (request.sweep.cleanup.reclaimed_row_version_count != 0 &&
      evidence_records.empty()) {
    return SweepError("SB-ROW-DATA-PHYSICAL-SWEEP-EVIDENCE-REQUIRED",
                      "storage.row_data_page.physical_sweep_evidence_required");
  }

  RowDataPhysicalSweepResult result;
  result.scanned_row_count = scanned;
  result.free_space_before = request.page.free_space_bytes;
  RowDataPageBody compacted = request.page;
  compacted.rows.clear();
  compacted.slots.clear();
  compacted.compaction_generation =
      compacted.compaction_generation == 0 ? compacted.page_generation + 1
                                           : compacted.compaction_generation + 1;

  std::vector<std::string> matched_evidence_ids;
  for (const RowDataRecord& row : request.page.rows) {
    const auto* evidence = MatchingEvidence(evidence_records, row);
    if (evidence == nullptr) {
      compacted.rows.push_back(row);
      ++result.retained_row_count;
      continue;
    }
    ++result.removed_row_count;
    ++result.reclaimed_slot_count;
    matched_evidence_ids.push_back(evidence->stable_evidence_id);
    result.reclaim_evidence_ids.push_back(evidence->stable_evidence_id);
  }

  for (const auto& evidence : evidence_records) {
    if (!EvidenceIdSeen(matched_evidence_ids, evidence.stable_evidence_id)) {
      return SweepError("SB-ROW-DATA-PHYSICAL-SWEEP-EVIDENCE-NOT-ON-PAGE",
                        "storage.row_data_page.physical_sweep_evidence_not_on_page",
                        evidence.stable_evidence_id);
    }
  }
  if (result.removed_row_count !=
      request.sweep.cleanup.reclaimed_row_version_count) {
    return SweepError("SB-ROW-DATA-PHYSICAL-SWEEP-RECLAIM-COUNT-MISMATCH",
                      "storage.row_data_page.physical_sweep_reclaim_count_mismatch",
                      std::to_string(result.removed_row_count) + ":" +
                          std::to_string(request.sweep.cleanup
                                             .reclaimed_row_version_count));
  }

  const auto rebuilt = BuildRowDataPageBody(compacted, request.page_size);
  if (!rebuilt.ok()) {
    result.status = rebuilt.status;
    result.diagnostic = rebuilt.diagnostic;
    return result;
  }
  result.status = SweepOkStatus();
  result.page = rebuilt.body;
  result.serialized = rebuilt.serialized;
  result.free_space_after = rebuilt.body.free_space_bytes;
  result.physical_storage_mutated = result.removed_row_count != 0;
  result.diagnostic = MakeSweepDiagnostic(
      result.status,
      "SB-ROW-DATA-PHYSICAL-SWEEP-APPLIED",
      "storage.row_data_page.physical_sweep_applied",
      std::to_string(result.removed_row_count));
  return result;
}

}  // namespace scratchbird::storage::page
