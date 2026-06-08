// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_transaction.hpp"

#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::transaction_mga}; }
Status RefuseStatus() { return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::transaction_mga}; }

IndexTransactionalPlan Refuse(std::string code, std::string key, std::string detail = {}) {
  IndexTransactionalPlan plan;
  plan.status = RefuseStatus();
  plan.next_state = IndexTransactionalState::refused;
  plan.diagnostic = MakeIndexTransactionDiagnostic(plan.status, std::move(code), std::move(key), std::move(detail));
  return plan;
}
}  // namespace

IndexTransactionalPlan PlanIndexTransactionalOperation(const IndexTransactionalRequest& request) {
  if (!request.index_uuid.valid() || request.family == IndexFamily::unknown || request.local_transaction_id == 0) {
    return Refuse("SB-INDEX-TX-INVALID-REQUEST", "index.transaction.invalid_request");
  }
  if (request.cluster_authority_active) {
    return Refuse("SB-INDEX-TX-CLUSTER-MAPPING-UNAVAILABLE",
                  "index.transaction.cluster_mapping_unavailable");
  }
  if (!request.page_authority_valid) {
    return Refuse("SB-INDEX-TX-PAGE-AUTHORITY-REFUSED", "index.transaction.page_authority_refused");
  }

  IndexTransactionalPlan plan;
  plan.status = OkStatus();
  plan.admitted = true;
  plan.horizon_guard_required = true;
  switch (request.operation) {
    case IndexTransactionalOperation::create_index:
      plan.catalog_update_required = true;
      plan.next_state = request.catalog_evidence_written ? IndexTransactionalState::building
                                                         : IndexTransactionalState::pending_catalog;
      plan.steps.push_back("write_catalog_create_evidence_before_success");
      plan.steps.push_back("reserve_build_identity_and_resource_epoch");
      break;
    case IndexTransactionalOperation::drop_index:
      plan.catalog_update_required = true;
      plan.next_state = request.catalog_evidence_written ? IndexTransactionalState::dropping
                                                         : IndexTransactionalState::pending_catalog;
      plan.steps.push_back("write_catalog_drop_evidence_before_success");
      plan.steps.push_back("retain_pages_until_horizon_allows_cleanup");
      break;
    case IndexTransactionalOperation::build_index:
      plan.next_state = request.online_build && (!request.snapshot_captured || !request.catchup_complete)
                            ? IndexTransactionalState::catchup
                            : (request.validation_complete ? IndexTransactionalState::ready_to_publish
                                                           : IndexTransactionalState::building);
      plan.steps.push_back("capture_snapshot_for_build");
      plan.steps.push_back("catch_up_concurrent_mutations");
      plan.steps.push_back("validate_before_publish");
      break;
    case IndexTransactionalOperation::insert_entry:
    case IndexTransactionalOperation::update_entry:
    case IndexTransactionalOperation::delete_entry:
      plan.row_mutation_required = true;
      plan.next_state = request.mutation_evidence_written ? IndexTransactionalState::active
                                                          : IndexTransactionalState::pending_catalog;
      plan.steps.push_back("append_index_delta_or_synchronous_entry");
      plan.steps.push_back("link_entry_to_row_version_identity");
      break;
    case IndexTransactionalOperation::savepoint_open:
      plan.durable_evidence_required = false;
      plan.next_state = IndexTransactionalState::active;
      plan.steps.push_back("capture_index_savepoint_boundary");
      break;
    case IndexTransactionalOperation::savepoint_rollback:
      if (request.savepoint_id == 0) {
        return Refuse("SB-INDEX-TX-SAVEPOINT-ID-REQUIRED", "index.transaction.savepoint_id_required");
      }
      plan.next_state = IndexTransactionalState::active;
      plan.steps.push_back("discard_entries_after_savepoint_boundary");
      break;
    case IndexTransactionalOperation::cleanup_dead_entries:
      plan.next_state = IndexTransactionalState::active;
      plan.steps.push_back("scan_dead_entries_visible_to_cleanup_horizon");
      plan.steps.push_back("retain_entries_newer_than_oldest_active_transaction");
      break;
    case IndexTransactionalOperation::recovery_classify:
      plan.next_state = request.interrupted_recovery ? IndexTransactionalState::suspect
                                                     : IndexTransactionalState::active;
      plan.steps.push_back("classify_building_stale_suspect_quarantine_dropping_states");
      break;
    case IndexTransactionalOperation::local_prepare:
      if (!request.local_prepare_supported) {
        return Refuse("SB-INDEX-TX-LOCAL-PREPARE-REFUSED", "index.transaction.local_prepare_refused");
      }
      plan.next_state = IndexTransactionalState::ready_to_publish;
      plan.steps.push_back("flush_prepared_index_state_or_refuse");
      break;
    case IndexTransactionalOperation::cluster_prepare_todo:
      return Refuse("SB-INDEX-TX-CLUSTER-MAPPING-UNAVAILABLE",
                    "index.transaction.cluster_mapping_unavailable");
  }
  if (request.publish_requested) {
    plan.steps.push_back("publish_index_epoch_after_transaction_finality");
    plan.next_state = IndexTransactionalState::committed;
  }
  return plan;
}

DiagnosticRecord MakeIndexTransactionDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code, status.severity, status.subsystem,
                        std::move(diagnostic_code), std::move(message_key),
                        std::move(arguments), {}, "core.index.transaction");
}

}  // namespace scratchbird::core::index
