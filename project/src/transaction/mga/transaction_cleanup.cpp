// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_cleanup.hpp"

#include "metric_producer.hpp"
#include "transaction_cleanup_horizon_service.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace scratchbird::transaction::mga {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status CleanupOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status CleanupWarningStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::warning, Subsystem::transaction_mga};
}

Status CleanupErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::transaction_mga};
}

const char* RetentionHorizonStableName(const HistoryRetentionHorizon& horizon) {
  return horizon.stable_name.empty() ? "history_retention_horizon" : horizon.stable_name.c_str();
}

bool IsCleanupReclaimableByKind(const RowVersionMetadata& metadata,
                                const LocalCleanupWorksetRequest& request) {
  if (metadata.state == RowVersionState::rolled_back) {
    return request.reclaim_rolled_back_versions;
  }
  if (metadata.state == RowVersionState::delete_marker) {
    return request.reclaim_delete_markers;
  }
  if (metadata.state == RowVersionState::committed && metadata.chain.has_next()) {
    return request.reclaim_obsolete_committed_versions;
  }
  return false;
}

Status CleanupStatusForDecision(CleanupEligibilityDecision decision) {
  return decision == CleanupEligibilityDecision::eligible_authoritative ? CleanupOkStatus()
                                                                        : CleanupWarningStatus();
}

CleanupEligibilityDecision DecisionForHoldKind(CleanupHoldKind hold_kind) {
  switch (hold_kind) {
    case CleanupHoldKind::limbo_transaction:
      return CleanupEligibilityDecision::blocked_by_limbo;
    case CleanupHoldKind::recovery_required:
      return CleanupEligibilityDecision::blocked_by_recovery;
    case CleanupHoldKind::archive_required:
    case CleanupHoldKind::backup_required:
      return CleanupEligibilityDecision::blocked_by_archive_or_backup;
    case CleanupHoldKind::none:
    case CleanupHoldKind::oldest_interesting_transaction:
    case CleanupHoldKind::oldest_active_transaction:
    case CleanupHoldKind::oldest_snapshot_transaction:
    case CleanupHoldKind::management_operation:
    case CleanupHoldKind::legal_hold:
    case CleanupHoldKind::admin_hold:
    case CleanupHoldKind::unknown:
      return CleanupEligibilityDecision::blocked_by_horizon;
  }
  return CleanupEligibilityDecision::blocked_by_horizon;
}

TransactionCleanupDecisionResult MakeBlockedDecisionWithCode(CleanupHoldKind hold_kind,
                                                             std::string diagnostic_code,
                                                             std::string message_key,
                                                             std::string detail) {
  TransactionCleanupDecisionResult result;
  result.decision = DecisionForHoldKind(hold_kind);
  result.blocking_hold = hold_kind;
  result.status = CleanupStatusForDecision(result.decision);
  result.diagnostic = MakeTransactionCleanupDiagnostic(result.status,
                                                       std::move(diagnostic_code),
                                                       std::move(message_key),
                                                       std::move(detail));
  return result;
}

TransactionCleanupDecisionResult MakeBlockedDecision(CleanupHoldKind hold_kind,
                                                     std::string message_key,
                                                     std::string detail) {
  return MakeBlockedDecisionWithCode(hold_kind,
                                     "SB-SNTXN-CLEANUP-BLOCKED-BY-HORIZON",
                                     std::move(message_key),
                                     std::move(detail));
}

TransactionCleanupDecisionResult MakeEligibleDecision() {
  TransactionCleanupDecisionResult result;
  result.status = CleanupOkStatus();
  result.decision = CleanupEligibilityDecision::eligible_authoritative;
  result.blocking_hold = CleanupHoldKind::none;
  result.diagnostic = MakeTransactionCleanupDiagnostic(result.status,
                                                       "SB-SNTXN-CLEANUP-ELIGIBLE-AUTHORITATIVE",
                                                       "transaction.cleanup.eligible_authoritative");
  return result;
}

bool HorizonBlocks(const LocalTransactionId& horizon, const RowVersionMetadata& metadata) {
  return !horizon.valid() || horizon.value <= metadata.identity.creator_transaction.local_id.value;
}

TransactionCleanupDecisionResult EvaluateHold(const RowVersionMetadata& metadata,
                                              CleanupHoldKind hold_kind,
                                              const LocalTransactionId& horizon,
                                              bool authoritative,
                                              const std::string& stable_name) {
  if (!authoritative) {
    return MakeBlockedDecision(hold_kind,
                               "transaction.cleanup.non_authoritative_hold",
                               "non_authoritative_hold:" + stable_name);
  }
  if (!horizon.valid()) {
    return MakeBlockedDecision(hold_kind,
                               "transaction.cleanup.invalid_hold",
                               "invalid_hold:" + stable_name);
  }
  if (HorizonBlocks(horizon, metadata)) {
    return MakeBlockedDecision(hold_kind,
                               "transaction.cleanup.blocked_by_hold",
                               stable_name);
  }
  return MakeEligibleDecision();
}

bool IsKnownFinalCreatorOutcome(const RowVersionMetadata& metadata) {
  if (metadata.creator_transaction_state == TransactionState::archived) {
    return metadata.state == RowVersionState::committed ||
           metadata.state == RowVersionState::delete_marker ||
           metadata.state == RowVersionState::rolled_back;
  }
  if (metadata.state == RowVersionState::committed ||
      metadata.state == RowVersionState::delete_marker) {
    return metadata.creator_transaction_state == TransactionState::committed;
  }
  if (metadata.state == RowVersionState::rolled_back) {
    return metadata.creator_transaction_state == TransactionState::rolled_back;
  }
  return false;
}

bool CreatorOutcomeCanBecomeKnown(TransactionState state) {
  return state == TransactionState::created ||
         state == TransactionState::active ||
         state == TransactionState::read_only_active ||
         state == TransactionState::preparing ||
         state == TransactionState::prepared ||
         state == TransactionState::committing ||
         state == TransactionState::rolling_back ||
         state == TransactionState::recovering ||
         state == TransactionState::limbo ||
         state == TransactionState::failed_terminal;
}

TransactionCleanupDecisionResult EvaluateCreatorOutcomeForCleanup(const RowVersionMetadata& metadata) {
  if (metadata.state == RowVersionState::limbo ||
      metadata.creator_transaction_state == TransactionState::limbo) {
    return MakeBlockedDecision(CleanupHoldKind::limbo_transaction,
                               "transaction.cleanup.blocked_by_limbo",
                               RowVersionStateName(metadata.state));
  }
  if (metadata.state == RowVersionState::recovery_required ||
      metadata.creator_transaction_state == TransactionState::recovering) {
    return MakeBlockedDecision(CleanupHoldKind::recovery_required,
                               "transaction.cleanup.blocked_by_recovery",
                               TransactionStateName(metadata.creator_transaction_state));
  }
  if (IsKnownFinalCreatorOutcome(metadata)) {
    return MakeEligibleDecision();
  }
  if (CreatorOutcomeCanBecomeKnown(metadata.creator_transaction_state) ||
      metadata.state == RowVersionState::uncommitted ||
      metadata.state == RowVersionState::prepared) {
    return MakeBlockedDecisionWithCode(
        CleanupHoldKind::recovery_required,
        "SB-SNTXN-CLEANUP-BLOCKED-BY-UNKNOWN-OUTCOME",
        "transaction.cleanup.blocked_by_unknown_outcome",
        std::string(RowVersionStateName(metadata.state)) + ":" +
            TransactionStateName(metadata.creator_transaction_state));
  }
  return MakeBlockedDecisionWithCode(
      CleanupHoldKind::recovery_required,
      "SB-SNTXN-CLEANUP-BLOCKED-BY-UNKNOWN-OUTCOME",
      "transaction.cleanup.creator_outcome_mismatch",
      std::string(RowVersionStateName(metadata.state)) + ":" +
          TransactionStateName(metadata.creator_transaction_state));
}

TransactionCleanupDecisionResult EvaluateRetentionHorizon(const RowVersionMetadata& metadata,
                                                          const HistoryRetentionHorizon& horizon) {
  if (!horizon.authoritative) {
    return MakeBlockedDecisionWithCode(CleanupHoldKind::legal_hold,
                                       "SB-SNTXN-CLEANUP-BLOCKED-BY-RETENTION-HORIZON",
                                       "transaction.cleanup.retention_horizon_not_authoritative",
                                       std::string("non_authoritative_retention_horizon:") +
                                           RetentionHorizonStableName(horizon));
  }
  if (!horizon.horizon_transaction.valid()) {
    return MakeBlockedDecisionWithCode(CleanupHoldKind::legal_hold,
                                       "SB-SNTXN-CLEANUP-BLOCKED-BY-RETENTION-HORIZON",
                                       "transaction.cleanup.retention_horizon_invalid",
                                       std::string("invalid_retention_horizon:") +
                                           RetentionHorizonStableName(horizon));
  }
  if (HorizonBlocks(horizon.horizon_transaction, metadata)) {
    return MakeBlockedDecisionWithCode(CleanupHoldKind::legal_hold,
                                       "SB-SNTXN-CLEANUP-BLOCKED-BY-RETENTION-HORIZON",
                                       "transaction.cleanup.blocked_by_retention_horizon",
                                       RetentionHorizonStableName(horizon));
  }
  return MakeEligibleDecision();
}

bool IsRetentionBlockedDecision(const TransactionCleanupDecisionResult& decision) {
  return decision.diagnostic.diagnostic_code == "SB-SNTXN-CLEANUP-BLOCKED-BY-RETENTION-HORIZON";
}

bool IsUnknownOutcomeDecision(const TransactionCleanupDecisionResult& decision) {
  return decision.diagnostic.diagnostic_code == "SB-SNTXN-CLEANUP-BLOCKED-BY-UNKNOWN-OUTCOME";
}

bool IsBackupOrArchiveHold(CleanupHoldKind hold_kind) {
  return hold_kind == CleanupHoldKind::archive_required ||
         hold_kind == CleanupHoldKind::backup_required;
}

std::string StableReclaimEvidenceId(const RowVersionMetadata& row_version,
                                    u64 cleanup_horizon) {
  return std::string("mga-cleanup-reclaim:") +
         std::to_string(row_version.identity.creator_transaction.local_id.value) +
         ":" + std::to_string(row_version.identity.version_sequence) +
         ":" + std::to_string(cleanup_horizon);
}

LocalCleanupReclaimEvidenceRecord MakeReclaimEvidenceRecord(
    const RowVersionMetadata& row_version,
    u64 cleanup_horizon) {
  LocalCleanupReclaimEvidenceRecord record;
  record.row_version_identity = row_version.identity;
  record.row_version_state = row_version.state;
  record.creator_transaction = row_version.identity.creator_transaction.local_id;
  record.successor_transaction = row_version.successor_transaction_local_id;
  record.authoritative_cleanup_horizon_local_transaction_id = cleanup_horizon;
  record.stable_evidence_id = StableReclaimEvidenceId(row_version, cleanup_horizon);
  return record;
}

bool IsKnownSweepFamily(LocalCleanupSweepFamily family) {
  return family != LocalCleanupSweepFamily::unknown;
}

double RetentionAgeMicroseconds(const LocalCleanupWorksetResult& result) {
  if (result.oldest_retained_row_version_local_transaction_id == 0 ||
      result.authoritative_cleanup_horizon_local_transaction_id <=
          result.oldest_retained_row_version_local_transaction_id) {
    return 0.0;
  }
  return static_cast<double>((result.authoritative_cleanup_horizon_local_transaction_id -
                              result.oldest_retained_row_version_local_transaction_id) * 1000ull);
}

void PublishCleanupWorksetMetrics(const LocalCleanupWorksetResult& result) {
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_mga_row_versions_reclaimed_total",
      scratchbird::core::metrics::Labels({{"component", "transaction.mga.cleanup"}, {"result", "reclaimed"}}),
      static_cast<double>(result.reclaimed_row_version_count),
      "transaction_mga_cleanup");
  (void)scratchbird::core::metrics::SetGauge(
      "sb_mga_cleanup_horizon_local_transaction_id",
      scratchbird::core::metrics::Labels({{"component", "transaction.mga.cleanup"}, {"authority", "local_inventory"}}),
      static_cast<double>(result.authoritative_cleanup_horizon_local_transaction_id),
      "transaction_mga_cleanup");
  (void)scratchbird::core::metrics::SetGauge(
      "sb_mga_cleanup_retained_row_versions",
      scratchbird::core::metrics::Labels({{"component", "transaction.mga.cleanup"}, {"authority", "local_inventory"}}),
      static_cast<double>(result.retained_row_version_count),
      "transaction_mga_cleanup");
  (void)scratchbird::core::metrics::SetGauge(
      "sb_mga_cleanup_retention_age_microseconds",
      scratchbird::core::metrics::Labels({{"component", "transaction.mga.cleanup"}, {"authority", "local_inventory"}}),
      RetentionAgeMicroseconds(result),
      "transaction_mga_cleanup");
  if (result.horizon_blocked_row_version_count != 0) {
    (void)scratchbird::core::metrics::IncrementCounter(
        "sb_mga_cleanup_blocked_total",
        scratchbird::core::metrics::Labels({{"component", "transaction.mga.cleanup"}, {"reason", "row_version_horizon"}}),
        static_cast<double>(result.horizon_blocked_row_version_count),
        "transaction_mga_cleanup");
  }
}

LocalGarbageCollectionSweepResult CleanupSweepError(LocalCleanupSweepFamily family,
                                                    u64 scanned_row_version_count,
                                                    std::string diagnostic_code,
                                                    std::string message_key,
                                                    std::string detail = {}) {
  LocalGarbageCollectionSweepResult result;
  result.status = CleanupErrorStatus();
  result.family = family;
  result.scanned_row_version_count = scanned_row_version_count;
  result.diagnostic = MakeTransactionCleanupDiagnostic(result.status,
                                                       std::move(diagnostic_code),
                                                       std::move(message_key),
                                                       std::move(detail));
  return result;
}

bool InventoryContainsCreator(const LocalTransactionInventory& inventory,
                              const RowVersionMetadata& metadata) {
  for (const TransactionInventoryEntry& entry : inventory.entries) {
    if (entry.identity.local_id.value == metadata.identity.creator_transaction.local_id.value &&
        entry.identity.transaction_uuid.value == metadata.identity.creator_transaction.transaction_uuid.value) {
      return true;
    }
  }
  return false;
}

const TransactionInventoryEntry* FindInventoryEntry(const LocalTransactionInventory& inventory,
                                                    LocalTransactionId local_id) {
  for (const TransactionInventoryEntry& entry : inventory.entries) {
    if (entry.identity.local_id.value == local_id.value) {
      return &entry;
    }
  }
  return nullptr;
}

bool IsActiveCleanupBlockerState(TransactionState state) {
  return state == TransactionState::active ||
         state == TransactionState::read_only_active;
}

bool IsUnresolvedCleanupBlockerState(TransactionState state) {
  return state == TransactionState::preparing ||
         state == TransactionState::prepared ||
         state == TransactionState::committing ||
         state == TransactionState::limbo ||
         state == TransactionState::recovering ||
         state == TransactionState::failed_terminal;
}

TransactionCleanupDecisionResult EvaluateSuccessorTransactionForCleanup(
    const RowVersionMetadata& metadata,
    const LocalTransactionInventory& inventory,
    LocalTransactionId cleanup_horizon) {
  if (metadata.state != RowVersionState::committed || !metadata.chain.has_next()) {
    return MakeEligibleDecision();
  }
  if (!metadata.successor_transaction_local_id.valid()) {
    return MakeBlockedDecisionWithCode(
        CleanupHoldKind::recovery_required,
        "SB-SNTXN-CLEANUP-BLOCKED-BY-UNKNOWN-SUCCESSOR",
        "transaction.cleanup.successor_transaction_missing",
        std::to_string(metadata.identity.creator_transaction.local_id.value));
  }
  if (!cleanup_horizon.valid() ||
      cleanup_horizon.value <= metadata.successor_transaction_local_id.value) {
    return MakeBlockedDecision(
        CleanupHoldKind::oldest_active_transaction,
        "transaction.cleanup.successor_not_below_cleanup_horizon",
        std::to_string(metadata.successor_transaction_local_id.value));
  }
  const TransactionInventoryEntry* successor =
      FindInventoryEntry(inventory, metadata.successor_transaction_local_id);
  if (successor == nullptr) {
    return MakeBlockedDecisionWithCode(
        CleanupHoldKind::recovery_required,
        "SB-SNTXN-CLEANUP-BLOCKED-BY-UNKNOWN-SUCCESSOR",
        "transaction.cleanup.successor_transaction_not_in_inventory",
        std::to_string(metadata.successor_transaction_local_id.value));
  }
  if (IsActiveCleanupBlockerState(successor->state)) {
    return MakeBlockedDecision(
        CleanupHoldKind::oldest_active_transaction,
        "transaction.cleanup.successor_transaction_active",
        std::to_string(successor->identity.local_id.value));
  }
  if (IsUnresolvedCleanupBlockerState(successor->state)) {
    return MakeBlockedDecisionWithCode(
        CleanupHoldKind::recovery_required,
        "SB-SNTXN-CLEANUP-BLOCKED-BY-UNKNOWN-SUCCESSOR",
        "transaction.cleanup.successor_transaction_unresolved",
        TransactionStateName(successor->state));
  }
  if (successor->state != TransactionState::committed) {
    return MakeBlockedDecisionWithCode(
        CleanupHoldKind::recovery_required,
        "SB-SNTXN-CLEANUP-BLOCKED-BY-UNKNOWN-SUCCESSOR",
        "transaction.cleanup.successor_transaction_not_committed",
        TransactionStateName(successor->state));
  }
  return MakeEligibleDecision();
}

}  // namespace

const char* LocalCleanupSweepFamilyName(LocalCleanupSweepFamily family) {
  switch (family) {
    case LocalCleanupSweepFamily::cooperative: return "cooperative";
    case LocalCleanupSweepFamily::background: return "background";
    case LocalCleanupSweepFamily::explicit_request: return "explicit_request";
    case LocalCleanupSweepFamily::maintenance: return "maintenance";
    case LocalCleanupSweepFamily::emergency: return "emergency";
    case LocalCleanupSweepFamily::unknown: return "unknown";
  }
  return "unknown";
}

TransactionCleanupDecisionResult EvaluateLocalCleanupWithHorizons(const RowVersionMetadata& metadata,
                                                                  const LocalTransactionHorizons& horizons) {
  if (!horizons.valid) {
    (void)scratchbird::core::metrics::IncrementCounter(
        "sb_mga_cleanup_blocked_total",
        scratchbird::core::metrics::Labels({{"component", "transaction.mga.cleanup"}, {"reason", "invalid_horizons"}}),
        1.0,
        "transaction_mga_cleanup");
    TransactionCleanupDecisionResult result;
    result.status = CleanupWarningStatus();
    result.decision = CleanupEligibilityDecision::blocked_by_horizon;
    result.blocking_hold = CleanupHoldKind::oldest_interesting_transaction;
    result.diagnostic = MakeTransactionCleanupDiagnostic(result.status,
                                                         "SB-SNTXN-CLEANUP-BLOCKED-BY-HORIZON",
                                                         "transaction.cleanup.invalid_horizons");
    return result;
  }

  RowVersionMetadataResult metadata_result = ValidateRowVersionMetadata(metadata);
  if (!metadata_result.ok()) {
    TransactionCleanupDecisionResult result;
    result.status = metadata_result.status;
    result.decision = CleanupEligibilityDecision::unknown;
    result.diagnostic = metadata_result.diagnostic;
    return result;
  }

  const auto creator_outcome = EvaluateCreatorOutcomeForCleanup(metadata);
  if (creator_outcome.decision != CleanupEligibilityDecision::eligible_authoritative) {
    return creator_outcome;
  }

  const auto oit = EvaluateHold(metadata,
                                CleanupHoldKind::oldest_interesting_transaction,
                                horizons.oldest_interesting_transaction,
                                true,
                                "local_oit");
  if (oit.decision != CleanupEligibilityDecision::eligible_authoritative) {
    (void)scratchbird::core::metrics::IncrementCounter(
        "sb_mga_cleanup_blocked_total",
        scratchbird::core::metrics::Labels({{"component", "transaction.mga.cleanup"}, {"reason", "blocked_by_horizon"}}),
        1.0,
        "transaction_mga_cleanup");
    return oit;
  }
  const auto oat = EvaluateHold(metadata,
                                CleanupHoldKind::oldest_active_transaction,
                                horizons.oldest_active_transaction,
                                true,
                                "local_oat");
  if (oat.decision != CleanupEligibilityDecision::eligible_authoritative) {
    return oat;
  }
  const auto ost = EvaluateHold(metadata,
                                CleanupHoldKind::oldest_snapshot_transaction,
                                horizons.oldest_snapshot_transaction,
                                true,
                                "local_ost");
  if (ost.decision != CleanupEligibilityDecision::eligible_authoritative) {
    return ost;
  }
  return MakeEligibleDecision();
}

TransactionCleanupDecisionResult EvaluateLocalCleanupWithAuthoritativeHolds(
    const RowVersionMetadata& metadata,
    const LocalTransactionHorizons& horizons,
    const std::vector<AuthoritativeCleanupHold>& additional_holds) {
  auto result = EvaluateLocalCleanupWithHorizons(metadata, horizons);
  if (result.decision != CleanupEligibilityDecision::eligible_authoritative) {
    return result;
  }
  for (const AuthoritativeCleanupHold& hold : additional_holds) {
    result = EvaluateHold(metadata, hold.hold_kind, hold.horizon_transaction, hold.authoritative, hold.stable_name);
    if (result.decision != CleanupEligibilityDecision::eligible_authoritative) {
      return result;
    }
  }
  return result;
}

TransactionCleanupDecisionResult EvaluateLocalCleanupWithRetentionHolds(
    const RowVersionMetadata& metadata,
    const LocalTransactionHorizons& horizons,
    const std::vector<AuthoritativeCleanupHold>& additional_holds,
    const std::vector<HistoryRetentionHorizon>& history_retention_horizons) {
  auto result = EvaluateLocalCleanupWithAuthoritativeHolds(metadata, horizons, additional_holds);
  if (result.decision != CleanupEligibilityDecision::eligible_authoritative) {
    return result;
  }
  for (const HistoryRetentionHorizon& horizon : history_retention_horizons) {
    result = EvaluateRetentionHorizon(metadata, horizon);
    if (result.decision != CleanupEligibilityDecision::eligible_authoritative) {
      return result;
    }
  }
  return result;
}

LocalCleanupWorksetResult ApplyLocalCleanupWithAuthoritativeInventory(
    const LocalCleanupWorksetRequest& request) {
  LocalCleanupWorksetResult result;
  if (!request.inventory_authoritative) {
    result.status = CleanupErrorStatus();
    result.diagnostic = MakeTransactionCleanupDiagnostic(result.status,
                                                         "SB-SNTXN-CLEANUP-HORIZON-NOT-AUTHORITATIVE",
                                                         "transaction.cleanup.horizon_not_authoritative",
                                                         "destructive cleanup requires durable local inventory authority");
    return result;
  }
  for (const AuthoritativeCleanupHold& hold : request.additional_holds) {
    if (!hold.authoritative) {
      result.status = CleanupErrorStatus();
      result.diagnostic = MakeTransactionCleanupDiagnostic(result.status,
                                                           "SB-SNTXN-CLEANUP-HOLD-NOT-AUTHORITATIVE",
                                                           "transaction.cleanup.hold_not_authoritative",
                                                           hold.stable_name);
      return result;
    }
    if (!hold.horizon_transaction.valid()) {
      result.status = CleanupErrorStatus();
      result.diagnostic = MakeTransactionCleanupDiagnostic(result.status,
                                                           "SB-SNTXN-CLEANUP-HOLD-INVALID",
                                                           "transaction.cleanup.hold_invalid",
                                                           hold.stable_name);
      return result;
    }
  }
  for (const HistoryRetentionHorizon& horizon : request.history_retention_horizons) {
    if (!horizon.authoritative) {
      result.status = CleanupErrorStatus();
      result.diagnostic = MakeTransactionCleanupDiagnostic(result.status,
                                                           "SB-SNTXN-CLEANUP-RETENTION-HORIZON-NOT-AUTHORITATIVE",
                                                           "transaction.cleanup.retention_horizon_not_authoritative",
                                                           RetentionHorizonStableName(horizon));
      return result;
    }
    if (!horizon.horizon_transaction.valid()) {
      result.status = CleanupErrorStatus();
      result.diagnostic = MakeTransactionCleanupDiagnostic(result.status,
                                                           "SB-SNTXN-CLEANUP-RETENTION-HORIZON-INVALID",
                                                           "transaction.cleanup.retention_horizon_invalid",
                                                           RetentionHorizonStableName(horizon));
      return result;
    }
  }

  AuthoritativeCleanupHorizonRequest horizon_request;
  horizon_request.inventory = request.inventory;
  horizon_request.inventory_authoritative = request.inventory_authoritative;
  horizon_request.inventory_complete = request.inventory_complete;
  horizon_request.active_snapshot_horizons = request.active_snapshot_horizons;
  horizon_request.active_snapshot_inventory_authoritative =
      request.active_snapshot_inventory_authoritative;
  horizon_request.always_in_transaction_policy = request.always_in_transaction_policy;
  horizon_request.always_active_session_inventory_authoritative =
      request.always_active_session_inventory_authoritative;
  horizon_request.always_active_sessions = request.always_active_sessions;
  const auto authoritative_horizon = ComputeAuthoritativeCleanupHorizon(horizon_request);
  if (!authoritative_horizon.ok()) {
    result.status = authoritative_horizon.status;
    result.diagnostic = authoritative_horizon.diagnostic;
    return result;
  }
  result.horizons = authoritative_horizon.horizons;
  result.authoritative_cleanup_horizon_local_transaction_id =
      authoritative_horizon.cleanup_horizon.value;
  for (const AuthoritativeCleanupHold& hold : request.additional_holds) {
    result.authoritative_cleanup_horizon_local_transaction_id =
        std::min(result.authoritative_cleanup_horizon_local_transaction_id, hold.horizon_transaction.value);
  }
  for (const HistoryRetentionHorizon& horizon : request.history_retention_horizons) {
    result.authoritative_cleanup_horizon_local_transaction_id =
        std::min(result.authoritative_cleanup_horizon_local_transaction_id,
                 horizon.horizon_transaction.value);
  }
  result.cleanup_horizon_authoritative = true;

  for (const RowVersionMetadata& row_version : request.row_versions) {
    const auto decision = EvaluateLocalCleanupWithRetentionHolds(row_version,
                                                                 authoritative_horizon.horizons,
                                                                 request.additional_holds,
                                                                 request.history_retention_horizons);
    const auto effective_cleanup_horizon =
        MakeLocalTransactionId(result.authoritative_cleanup_horizon_local_transaction_id);
    const auto successor_decision = decision.decision == CleanupEligibilityDecision::eligible_authoritative
                                        ? EvaluateSuccessorTransactionForCleanup(
                                              row_version,
                                              request.inventory,
                                              effective_cleanup_horizon)
                                        : decision;
    if (!successor_decision.ok() ||
        successor_decision.decision != CleanupEligibilityDecision::eligible_authoritative ||
        !IsCleanupReclaimableByKind(row_version, request)) {
      if (request.retain_row_versions_in_result) {
        if (request.max_retained_row_versions != 0 &&
            result.retained_row_versions.size() >= request.max_retained_row_versions) {
          result.bounded_memory_limit_hit = true;
          result.status = CleanupErrorStatus();
          result.diagnostic = MakeTransactionCleanupDiagnostic(
              result.status,
              "SB-SNTXN-CLEANUP-RESULT-LIMIT-EXCEEDED",
              "transaction.cleanup.result_limit_exceeded",
              std::to_string(request.max_retained_row_versions));
          return result;
        }
        result.retained_row_versions.push_back(row_version);
      }
      ++result.retained_row_version_count;
      const u64 creator = row_version.identity.creator_transaction.local_id.value;
      if (creator != 0 &&
          (result.oldest_retained_row_version_local_transaction_id == 0 ||
           creator < result.oldest_retained_row_version_local_transaction_id)) {
        result.oldest_retained_row_version_local_transaction_id = creator;
      }
      if (successor_decision.decision == CleanupEligibilityDecision::blocked_by_horizon ||
          successor_decision.decision == CleanupEligibilityDecision::blocked_by_limbo ||
          successor_decision.decision == CleanupEligibilityDecision::blocked_by_recovery ||
          successor_decision.decision == CleanupEligibilityDecision::blocked_by_archive_or_backup) {
        ++result.horizon_blocked_row_version_count;
      }
      if (successor_decision.decision == CleanupEligibilityDecision::blocked_by_archive_or_backup ||
          IsBackupOrArchiveHold(successor_decision.blocking_hold)) {
        ++result.backup_archive_blocked_row_version_count;
      }
      if (IsRetentionBlockedDecision(successor_decision)) {
        ++result.retention_blocked_row_version_count;
      }
      if (successor_decision.decision == CleanupEligibilityDecision::blocked_by_limbo ||
          IsUnknownOutcomeDecision(successor_decision) ||
          successor_decision.diagnostic.diagnostic_code ==
              "SB-SNTXN-CLEANUP-BLOCKED-BY-UNKNOWN-SUCCESSOR") {
        ++result.limbo_or_unknown_outcome_blocked_row_version_count;
      }
      continue;
    }
    ++result.reclaimed_row_version_count;
    if (request.emit_reclaim_evidence_records) {
      if (request.max_reclaim_evidence_records == 0 ||
          result.reclaim_evidence_records.size() >= request.max_reclaim_evidence_records) {
        result.bounded_memory_limit_hit = true;
        result.status = CleanupErrorStatus();
        result.diagnostic = MakeTransactionCleanupDiagnostic(
            result.status,
            "SB-SNTXN-CLEANUP-RECLAIM-EVIDENCE-LIMIT-EXCEEDED",
            "transaction.cleanup.reclaim_evidence_limit_exceeded",
            std::to_string(request.max_reclaim_evidence_records));
        return result;
      }
      result.reclaim_evidence_records.push_back(
          MakeReclaimEvidenceRecord(
              row_version,
              result.authoritative_cleanup_horizon_local_transaction_id));
    }
  }

  result.status = CleanupOkStatus();
  result.diagnostic = MakeTransactionCleanupDiagnostic(result.status,
                                                       "SB-SNTXN-CLEANUP-APPLIED",
                                                       "transaction.cleanup.applied",
                                                       std::to_string(result.reclaimed_row_version_count));
  PublishCleanupWorksetMetrics(result);
  return result;
}

LocalGarbageCollectionSweepResult RunLocalGarbageCollectionSweep(
    const LocalGarbageCollectionSweepRequest& request) {
  const u64 scanned = static_cast<u64>(request.workset.row_versions.size());
  if (!request.engine_mga_authoritative) {
    return CleanupSweepError(request.family,
                             scanned,
                             "SB-SNTXN-CLEANUP-SWEEP-NOT-ENGINE-AUTHORITATIVE",
                             "transaction.cleanup.sweep_not_engine_authoritative",
                             "local MGA engine authority is required");
  }
  if (!IsKnownSweepFamily(request.family)) {
    return CleanupSweepError(request.family,
                             scanned,
                             "SB-SNTXN-CLEANUP-SWEEP-FAMILY-UNKNOWN",
                             "transaction.cleanup.sweep_family_unknown");
  }
  if (request.max_candidate_row_versions == 0 ||
      scanned > request.max_candidate_row_versions) {
    return CleanupSweepError(request.family,
                             scanned,
                             "SB-SNTXN-CLEANUP-BOUNDED-SCAN-REQUIRED",
                             "transaction.cleanup.bounded_scan_required",
                             std::to_string(request.max_candidate_row_versions));
  }
  if (request.retain_row_versions_in_result &&
      request.max_retained_row_versions == 0) {
    return CleanupSweepError(request.family,
                             scanned,
                             "SB-SNTXN-CLEANUP-BOUNDED-RESULT-REQUIRED",
                             "transaction.cleanup.bounded_result_required");
  }
  for (const RowVersionMetadata& row_version : request.workset.row_versions) {
    if (!InventoryContainsCreator(request.workset.inventory, row_version)) {
      return CleanupSweepError(request.family,
                               scanned,
                               "SB-SNTXN-CLEANUP-CREATOR-NOT-IN-INVENTORY",
                               "transaction.cleanup.creator_not_in_inventory",
                               std::to_string(row_version.identity.creator_transaction.local_id.value));
    }
  }

  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_mga_cleanup_sweeps_started_total",
      scratchbird::core::metrics::Labels({{"component", "transaction.mga.cleanup"},
                                          {"family", LocalCleanupSweepFamilyName(request.family)}}),
      1.0,
      "transaction_mga_cleanup");

  LocalCleanupWorksetRequest workset = request.workset;
  workset.max_retained_row_versions = request.max_retained_row_versions;
  workset.retain_row_versions_in_result = request.retain_row_versions_in_result;
  auto cleanup = ApplyLocalCleanupWithAuthoritativeInventory(workset);

  LocalGarbageCollectionSweepResult result;
  result.status = cleanup.status;
  result.cleanup = std::move(cleanup);
  result.family = request.family;
  result.scanned_row_version_count = scanned;
  result.bounded_memory_enforced = true;
  result.diagnostic = result.cleanup.diagnostic;

  (void)scratchbird::core::metrics::IncrementCounter(
      result.status.ok() ? "sb_mga_cleanup_sweeps_completed_total"
                         : "sb_mga_cleanup_sweeps_failed_total",
      scratchbird::core::metrics::Labels({{"component", "transaction.mga.cleanup"},
                                          {"family", LocalCleanupSweepFamilyName(request.family)}}),
      1.0,
      "transaction_mga_cleanup");

  return result;
}

DiagnosticRecord MakeTransactionCleanupDiagnostic(Status status,
                                                  std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail) {
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
                        "transaction.mga.cleanup");
}

}  // namespace scratchbird::transaction::mga
