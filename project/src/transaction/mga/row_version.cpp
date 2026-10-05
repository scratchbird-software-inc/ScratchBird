// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "row_version.hpp"
#include "row_version_observation.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace scratchbird::transaction::mga {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status RowVersionOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status RowVersionErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::transaction_mga};
}

bool TypedUuidMatches(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool IsValidRowTypedUuid(const TypedUuid& typed) {
  return typed.kind == UuidKind::row &&
         typed.valid() &&
         scratchbird::core::uuid::IsEngineIdentityUuid(typed.value);
}

HotStableRowHeadDecisionResult HotStableRowHeadRefusal(
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  HotStableRowHeadDecisionResult result;
  result.status = RowVersionErrorStatus();
  result.decision = HotStableRowHeadDecisionKind::refused;
  result.proof_accepted = false;
  result.diagnostic = MakeRowVersionDiagnostic(result.status,
                                               std::move(diagnostic_code),
                                               std::move(message_key),
                                               std::move(detail));
  return result;
}

}  // namespace

const char* RowVersionStateName(RowVersionState state) {
  switch (state) {
    case RowVersionState::unknown: return "unknown";
    case RowVersionState::uncommitted: return "uncommitted";
    case RowVersionState::prepared: return "prepared";
    case RowVersionState::committed: return "committed";
    case RowVersionState::rolled_back: return "rolled_back";
    case RowVersionState::delete_marker: return "delete_marker";
    case RowVersionState::limbo: return "limbo";
    case RowVersionState::recovery_required: return "recovery_required";
  }
  return "unknown";
}

const char* VisibilityDecisionName(VisibilityDecision decision) {
  switch (decision) {
    case VisibilityDecision::visible: return "visible";
    case VisibilityDecision::invisible: return "invisible";
    case VisibilityDecision::wait_for_transaction: return "wait_for_transaction";
    case VisibilityDecision::requires_recovery: return "requires_recovery";
    case VisibilityDecision::unknown: return "unknown";
  }
  return "unknown";
}

const char* HotStableRowHeadDecisionName(HotStableRowHeadDecisionKind decision) {
  switch (decision) {
    case HotStableRowHeadDecisionKind::refused: return "refused";
    case HotStableRowHeadDecisionKind::page_local_hot: return "page_local_hot";
    case HotStableRowHeadDecisionKind::stable_row_head_indirection: return "stable_row_head_indirection";
    case HotStableRowHeadDecisionKind::ordinary_index_rewrite: return "ordinary_index_rewrite";
  }
  return "refused";
}

RowIdentityResult MakeRowIdentity(TypedUuid row_uuid) {
  RowIdentity identity;
  identity.row_uuid = row_uuid;
  return ValidateRowIdentity(identity);
}

RowIdentityResult ValidateRowIdentity(const RowIdentity& identity) {
  const auto observed = ObserveRowIdentity(identity);
  return {observed.status, identity, MaterializeMgaObservationDiagnostic(observed)};
}

RowVersionIdentityResult MakeRowVersionIdentity(RowIdentity row,
                                                TransactionIdentity creator_transaction,
                                                u64 version_sequence,
                                                scratchbird::core::platform::Uuid version_uuid) {
  RowVersionIdentity identity;
  identity.row = row;
  identity.creator_transaction = creator_transaction;
  identity.version_sequence = version_sequence;
  identity.version_uuid = version_uuid;
  return ValidateRowVersionIdentity(identity);
}

RowVersionIdentityResult ValidateRowVersionIdentity(const RowVersionIdentity& identity) {
  const auto observed = ObserveRowVersionIdentity(identity);
  return {observed.status, identity, MaterializeMgaObservationDiagnostic(observed)};
}

RowVersionMetadataResult ValidateRowVersionMetadata(const RowVersionMetadata& metadata) {
  const auto observed = ObserveRowVersionMetadata(metadata);
  return {observed.status, metadata, MaterializeMgaObservationDiagnostic(observed)};
}

VisibilityResult EvaluateVisibility(const RowVersionMetadata& metadata,
                                    const VisibilitySnapshot& snapshot) {
  const auto observed = ObserveVisibility(metadata, BorrowVisibilitySnapshot(snapshot));
  return {observed.outcome.status, observed.decision,
          MaterializeMgaObservationDiagnostic(observed.outcome)};
}

VisibilityResult EvaluateVersionEffectVisibility(const RowVersionMetadata& metadata,
                                                const VisibilitySnapshot& snapshot) {
  const auto observed = ObserveVersionEffectVisibility(metadata, BorrowVisibilitySnapshot(snapshot));
  return {observed.outcome.status, observed.decision,
          MaterializeMgaObservationDiagnostic(observed.outcome)};
}

HotStableRowHeadDecisionResult EvaluateHotStableRowHeadDecision(
    const HotStableRowHeadProofInput& input) {
  HotStableRowHeadDecisionResult result;
  result.status = RowVersionOkStatus();

  if (input.parser_or_reference_authority) {
    return HotStableRowHeadRefusal("SB-MGA-HOT-STABLE-HEAD-PARSER-REFERENCE-REFUSED",
                                   "row_version.hot_stable_head.parser_reference_refused");
  }

  if (!input.exact_index_keys_unchanged) {
    result.decision = HotStableRowHeadDecisionKind::ordinary_index_rewrite;
    return result;
  }

  if (!IsValidRowTypedUuid(input.old_version_uuid)) {
    return HotStableRowHeadRefusal("SB-MGA-HOT-STABLE-HEAD-OLD-VERSION-UUID-INVALID",
                                   "row_version.hot_stable_head.old_version_uuid_invalid");
  }
  if (!IsValidRowTypedUuid(input.new_previous_version_uuid)) {
    return HotStableRowHeadRefusal("SB-MGA-HOT-STABLE-HEAD-PREVIOUS-UUID-INVALID",
                                   "row_version.hot_stable_head.previous_uuid_invalid");
  }

  const RowVersionMetadataResult old_metadata =
      ValidateRowVersionMetadata(input.old_visible_version);
  if (!old_metadata.ok()) {
    return HotStableRowHeadRefusal("SB-MGA-HOT-STABLE-HEAD-OLD-METADATA-INVALID",
                                   "row_version.hot_stable_head.old_metadata_invalid",
                                   old_metadata.diagnostic.diagnostic_code);
  }
  const RowVersionMetadataResult new_metadata =
      ValidateRowVersionMetadata(input.new_version);
  if (!new_metadata.ok()) {
    return HotStableRowHeadRefusal("SB-MGA-HOT-STABLE-HEAD-NEW-METADATA-INVALID",
                                   "row_version.hot_stable_head.new_metadata_invalid",
                                   new_metadata.diagnostic.diagnostic_code);
  }
  if (input.old_visible_version.identity.version_uuid != input.old_version_uuid.value ||
      input.new_version.identity.version_uuid == input.old_visible_version.identity.version_uuid) {
    return HotStableRowHeadRefusal("CATALOG.INVALID_INPUT",
                                   "row_version.hot_stable_head.version_identity_mismatch");
  }

  if (!TypedUuidMatches(input.old_visible_version.identity.row.row_uuid,
                        input.new_version.identity.row.row_uuid)) {
    return HotStableRowHeadRefusal("SB-MGA-HOT-STABLE-HEAD-ROW-UUID-CHANGED",
                                   "row_version.hot_stable_head.row_uuid_changed");
  }
  if (!TypedUuidMatches(input.old_version_uuid, input.new_previous_version_uuid)) {
    return HotStableRowHeadRefusal("SB-MGA-HOT-STABLE-HEAD-PREVIOUS-UUID-MISMATCH",
                                   "row_version.hot_stable_head.previous_uuid_mismatch");
  }
  if (input.new_version.chain.previous_version_sequence ==
      kInvalidRowVersionSequence) {
    return HotStableRowHeadRefusal("SB-MGA-HOT-STABLE-HEAD-PREVIOUS-SEQUENCE-MISSING",
                                   "row_version.hot_stable_head.previous_sequence_missing");
  }
  if (input.new_version.chain.previous_version_sequence !=
      input.old_visible_version.identity.version_sequence) {
    return HotStableRowHeadRefusal("SB-MGA-HOT-STABLE-HEAD-PREVIOUS-SEQUENCE-MISMATCH",
                                   "row_version.hot_stable_head.previous_sequence_mismatch");
  }
  if (!TypedUuidMatches(input.new_version.chain.previous_version_uuid,
                        input.old_version_uuid)) {
    return HotStableRowHeadRefusal("SB-MGA-HOT-STABLE-HEAD-CHAIN-PREVIOUS-UUID-MISMATCH",
                                   "row_version.hot_stable_head.chain_previous_uuid_mismatch");
  }
  if (input.new_version.identity.creator_transaction.local_id.value !=
      input.visibility_snapshot.reader_transaction.value) {
    return HotStableRowHeadRefusal("SB-MGA-HOT-STABLE-HEAD-CREATOR-NOT-READER",
                                   "row_version.hot_stable_head.creator_not_reader");
  }
  if (input.new_version.creator_transaction_state != TransactionState::active ||
      input.new_version.state != RowVersionState::uncommitted) {
    return HotStableRowHeadRefusal("SB-MGA-HOT-STABLE-HEAD-CREATOR-NOT-ACTIVE",
                                   "row_version.hot_stable_head.creator_not_active",
                                   TransactionStateName(input.new_version.creator_transaction_state));
  }

  const VisibilityResult old_visibility =
      EvaluateVisibility(input.old_visible_version, input.visibility_snapshot);
  if (!old_visibility.ok() ||
      old_visibility.decision != VisibilityDecision::visible) {
    return HotStableRowHeadRefusal("SB-MGA-HOT-STABLE-HEAD-OLD-VISIBILITY-REFUSED",
                                   "row_version.hot_stable_head.old_visibility_refused",
                                   VisibilityDecisionName(old_visibility.decision));
  }

  const VisibilityResult new_visibility =
      EvaluateVisibility(input.new_version, input.visibility_snapshot);
  if (!new_visibility.ok() ||
      new_visibility.decision != VisibilityDecision::visible) {
    return HotStableRowHeadRefusal("SB-MGA-HOT-STABLE-HEAD-NEW-VISIBILITY-REFUSED",
                                   "row_version.hot_stable_head.new_visibility_refused",
                                   VisibilityDecisionName(new_visibility.decision));
  }

  result.proof_accepted = true;
  result.decision = input.same_page_budget_available
                        ? HotStableRowHeadDecisionKind::page_local_hot
                        : HotStableRowHeadDecisionKind::stable_row_head_indirection;
  return result;
}

DiagnosticRecord MakeRowVersionDiagnostic(Status status,
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
                        "transaction.mga.row_version");
}

}  // namespace scratchbird::transaction::mga
