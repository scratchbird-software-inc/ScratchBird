// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "repair_identity_rules.hpp"

#include <limits>
#include <string>
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
using scratchbird::core::platform::UuidKind;

Status RepairIdentityOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status RepairIdentityErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::storage_page};
}

bool SameTypedUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool IsTypedEngineIdentity(const TypedUuid& uuid, UuidKind kind) {
  return uuid.kind == kind && uuid.valid() &&
         scratchbird::core::uuid::IsEngineIdentityUuid(uuid.value);
}

bool SameTransactionIdentity(const mga::TransactionIdentity& left,
                             const mga::TransactionIdentity& right) {
  return left.local_id.value == right.local_id.value &&
         left.scope == right.scope &&
         SameTypedUuid(left.transaction_uuid, right.transaction_uuid);
}

bool SameMetadataIdentity(const RowVersionMetadata& left,
                          const RowVersionMetadata& right) {
  return SameTypedUuid(left.identity.row.row_uuid,
                       right.identity.row.row_uuid) &&
         SameTransactionIdentity(left.identity.creator_transaction,
                                 right.identity.creator_transaction) &&
         left.identity.version_sequence == right.identity.version_sequence &&
         SameTypedUuid(left.chain.previous_version_uuid,
                       right.chain.previous_version_uuid) &&
         SameTypedUuid(left.chain.next_version_uuid,
                       right.chain.next_version_uuid) &&
         left.chain.previous_version_sequence ==
             right.chain.previous_version_sequence &&
         left.chain.next_version_sequence == right.chain.next_version_sequence &&
         left.successor_transaction_local_id.value ==
             right.successor_transaction_local_id.value &&
         left.state == right.state &&
         left.creator_transaction_state == right.creator_transaction_state &&
         left.payload_present == right.payload_present;
}

bool RowCarriesMetadataCreator(const RowDataRecord& row,
                               const RowVersionMetadata& metadata) {
  return SameTypedUuid(row.row_uuid, metadata.identity.row.row_uuid) &&
         SameTypedUuid(row.transaction_uuid,
                       metadata.identity.creator_transaction.transaction_uuid) &&
         row.local_transaction_id ==
             metadata.identity.creator_transaction.local_id.value;
}

bool RowSequenceMatchesMetadata(const RowDataRecord& row,
                                const RowVersionMetadata& metadata) {
  return metadata.identity.version_sequence <=
             static_cast<u64>(std::numeric_limits<
                 decltype(row.row_version)>::max()) &&
         row.row_version ==
             static_cast<decltype(row.row_version)>(
                 metadata.identity.version_sequence);
}

void AddCommonEvidence(RepairIdentityDecision* decision,
                       const RepairIdentityRequest& request) {
  decision->repair_evidence_is_transaction_authority = false;
  decision->evidence.push_back(std::string("repair_identity_action=") +
                               RepairIdentityActionName(request.action));
  decision->evidence.push_back(
      "durable_mga_inventory_authority_available=" +
      std::string(request.durable_mga_inventory_authority_available ? "true"
                                                                    : "false"));
  decision->evidence.push_back(
      "normal_mga_visibility_recheck_available=" +
      std::string(request.normal_mga_visibility_recheck_available ? "true"
                                                                  : "false"));
  decision->evidence.push_back(
      "repair_event_persisted_before_mutation=" +
      std::string(request.repair_event_persisted_before_mutation ? "true"
                                                                 : "false"));
  decision->evidence.push_back("repair_event_digest=" +
                               std::to_string(request.repair_event_digest));
  decision->evidence.push_back("repair_evidence_transaction_authority=false");
}

RepairIdentityDecision Refused(const RepairIdentityRequest& request,
                               std::string diagnostic_code,
                               std::string message_key,
                               std::string detail = {}) {
  RepairIdentityDecision decision;
  decision.status = RepairIdentityErrorStatus();
  decision.accepted = false;
  decision.mutation_allowed = false;
  decision.diagnostic = MakeRepairIdentityDiagnostic(decision.status,
                                                     std::move(diagnostic_code),
                                                     std::move(message_key),
                                                     std::move(detail));
  AddCommonEvidence(&decision, request);
  return decision;
}

bool ActionNeedsMutationEvent(RepairIdentityAction action) {
  return action == RepairIdentityAction::exact_relocation ||
         action == RepairIdentityAction::page_rewrite ||
         action == RepairIdentityAction::logical_correction ||
         action == RepairIdentityAction::salvage_promote_with_authority;
}

RepairIdentityDecision AcceptedBase(const RepairIdentityRequest& request) {
  RepairIdentityDecision decision;
  decision.status = RepairIdentityOkStatus();
  decision.accepted = true;
  decision.output_row = request.candidate_row;
  decision.output_metadata = request.candidate_metadata;
  decision.output_version_uuid = request.candidate_version_uuid;
  AddCommonEvidence(&decision, request);
  return decision;
}

RepairIdentityDecision ValidateCommonAuthority(
    const RepairIdentityRequest& request) {
  if (!request.durable_mga_inventory_authority_available ||
      request.repair_evidence_is_transaction_authority ||
      request.parser_or_reference_authority ||
      request.names_are_authority) {
    return Refused(request,
                   "SB-REPAIR-IDENTITY-AUTHORITY-REFUSED",
                   "storage.repair_identity.authority_refused");
  }
  if (ActionNeedsMutationEvent(request.action) &&
      (!request.normal_mga_visibility_recheck_available ||
       !request.repair_event_persisted_before_mutation ||
       request.repair_event_digest == 0)) {
    return Refused(request,
                   "SB-REPAIR-IDENTITY-MUTATION-EVIDENCE-REQUIRED",
                   "storage.repair_identity.mutation_evidence_required");
  }
  return AcceptedBase(request);
}

RepairIdentityDecision ValidateInputMetadata(
    const RepairIdentityRequest& request) {
  const auto original = mga::ValidateRowVersionMetadata(
      request.original_metadata);
  if (!original.ok()) {
    return Refused(request,
                   "SB-REPAIR-IDENTITY-ORIGINAL-METADATA-INVALID",
                   "storage.repair_identity.original_metadata_invalid",
                   original.diagnostic.diagnostic_code);
  }
  const auto candidate = mga::ValidateRowVersionMetadata(
      request.candidate_metadata);
  if (!candidate.ok()) {
    return Refused(request,
                   "SB-REPAIR-IDENTITY-CANDIDATE-METADATA-INVALID",
                   "storage.repair_identity.candidate_metadata_invalid",
                   candidate.diagnostic.diagnostic_code);
  }
  if (!IsTypedEngineIdentity(request.original_version_uuid, UuidKind::row) ||
      !IsTypedEngineIdentity(request.candidate_version_uuid, UuidKind::row)) {
    return Refused(request,
                   "SB-REPAIR-IDENTITY-VERSION-UUID-INVALID",
                   "storage.repair_identity.version_uuid_invalid");
  }
  if (!RowCarriesMetadataCreator(request.original_row,
                                 request.original_metadata) ||
      !RowCarriesMetadataCreator(request.candidate_row,
                                 request.candidate_metadata)) {
    return Refused(request,
                   "SB-REPAIR-IDENTITY-ROW-METADATA-MISMATCH",
                   "storage.repair_identity.row_metadata_mismatch");
  }
  if (!RowSequenceMatchesMetadata(request.original_row,
                                  request.original_metadata) ||
      !RowSequenceMatchesMetadata(request.candidate_row,
                                  request.candidate_metadata)) {
    return Refused(request,
                   "SB-REPAIR-IDENTITY-ROW-SEQUENCE-MISMATCH",
                   "storage.repair_identity.row_sequence_mismatch");
  }
  return AcceptedBase(request);
}

RepairIdentityDecision EvaluateExactIdentity(
    const RepairIdentityRequest& request) {
  if (request.logical_payload_changed) {
    return Refused(request,
                   "SB-REPAIR-IDENTITY-LOGICAL-CORRECTION-REQUIRED",
                   "storage.repair_identity.logical_correction_required");
  }
  if (!SameTypedUuid(request.original_row.row_uuid,
                     request.candidate_row.row_uuid) ||
      !SameTypedUuid(request.original_version_uuid,
                     request.candidate_version_uuid) ||
      !SameMetadataIdentity(request.original_metadata,
                            request.candidate_metadata)) {
    return Refused(request,
                   "SB-REPAIR-IDENTITY-EXACT-IDENTITY-CHANGED",
                   "storage.repair_identity.exact_identity_changed");
  }

  RepairIdentityDecision decision = AcceptedBase(request);
  decision.mutation_allowed = true;
  decision.exact_identity_preserved = true;
  decision.row_uuid_preserved = true;
  decision.version_uuid_preserved = true;
  decision.evidence.push_back("row_uuid_preserved=true");
  decision.evidence.push_back("version_uuid_preserved=true");
  decision.evidence.push_back("logical_correction_created_new_version=false");
  return decision;
}

RepairIdentityDecision EvaluateLogicalCorrection(
    const RepairIdentityRequest& request,
    bool salvage_promotion) {
  if (!request.logical_payload_changed || !request.authoritative_payload_proof) {
    return Refused(request,
                   "SB-REPAIR-IDENTITY-PAYLOAD-PROOF-REQUIRED",
                   "storage.repair_identity.payload_proof_required");
  }
  if (!SameTypedUuid(request.original_row.row_uuid,
                     request.candidate_row.row_uuid) ||
      !SameTypedUuid(request.original_metadata.identity.row.row_uuid,
                     request.candidate_metadata.identity.row.row_uuid)) {
    return Refused(request,
                   "SB-REPAIR-IDENTITY-ROW-UUID-CHANGED",
                   "storage.repair_identity.row_uuid_changed");
  }
  if (SameTypedUuid(request.original_version_uuid,
                   request.candidate_version_uuid)) {
    return Refused(request,
                   "SB-REPAIR-IDENTITY-NEW-VERSION-UUID-REQUIRED",
                   "storage.repair_identity.new_version_uuid_required");
  }
  if (request.candidate_metadata.identity.version_sequence <=
      request.original_metadata.identity.version_sequence) {
    return Refused(request,
                   "SB-REPAIR-IDENTITY-NEW-VERSION-SEQUENCE-REQUIRED",
                   "storage.repair_identity.new_version_sequence_required");
  }
  if (request.candidate_metadata.chain.previous_version_sequence !=
          request.original_metadata.identity.version_sequence ||
      !SameTypedUuid(request.candidate_metadata.chain.previous_version_uuid,
                     request.original_version_uuid)) {
    return Refused(request,
                   "SB-REPAIR-IDENTITY-PREVIOUS-LINK-REQUIRED",
                   "storage.repair_identity.previous_link_required");
  }
  if (request.candidate_metadata.state !=
          mga::RowVersionState::uncommitted ||
      request.candidate_metadata.creator_transaction_state !=
          mga::TransactionState::active ||
      !request.candidate_metadata.payload_present) {
    return Refused(request,
                   "SB-REPAIR-IDENTITY-NEW-VERSION-MUST-BE-ACTIVE",
                   "storage.repair_identity.new_version_must_be_active");
  }

  RepairIdentityDecision decision = AcceptedBase(request);
  decision.mutation_allowed = true;
  decision.row_uuid_preserved = true;
  decision.version_uuid_preserved = false;
  decision.logical_correction_created_new_version = true;
  decision.salvage_remains_evidence = salvage_promotion;
  decision.restore_required = request.salvage_restore_required;
  decision.evidence.push_back("row_uuid_preserved=true");
  decision.evidence.push_back("version_uuid_preserved=false");
  decision.evidence.push_back("logical_correction_created_new_version=true");
  decision.evidence.push_back("new_version_state=uncommitted");
  if (salvage_promotion) {
    decision.evidence.push_back("salvage_evidence_authority=false");
    decision.evidence.push_back("salvage_promoted_through_mga_new_version=true");
  }
  return decision;
}

RepairIdentityDecision EvaluateSalvageReview(
    const RepairIdentityRequest& request) {
  if (request.salvage_payload_promoted_to_committed) {
    return Refused(request,
                   "SB-REPAIR-IDENTITY-SALVAGE-PROMOTION-REFUSED",
                   "storage.repair_identity.salvage_promotion_refused");
  }
  RepairIdentityDecision decision = AcceptedBase(request);
  decision.accepted = true;
  decision.mutation_allowed = false;
  decision.salvage_remains_evidence = true;
  decision.restore_required =
      request.salvage_uncertain || request.salvage_restore_required;
  decision.output_row = request.original_row;
  decision.output_metadata = request.original_metadata;
  decision.output_version_uuid = request.original_version_uuid;
  decision.evidence.push_back("salvage_evidence_authority=false");
  decision.evidence.push_back("salvage_ordinary_commit_created=false");
  decision.evidence.push_back(
      std::string("restore_required=") +
      (decision.restore_required ? "true" : "false"));
  return decision;
}

}  // namespace

const char* RepairIdentityActionName(RepairIdentityAction action) {
  switch (action) {
    case RepairIdentityAction::exact_relocation:
      return "exact_relocation";
    case RepairIdentityAction::page_rewrite:
      return "page_rewrite";
    case RepairIdentityAction::logical_correction:
      return "logical_correction";
    case RepairIdentityAction::salvage_review:
      return "salvage_review";
    case RepairIdentityAction::salvage_promote_with_authority:
      return "salvage_promote_with_authority";
  }
  return "exact_relocation";
}

RepairIdentityDecision EvaluateRepairIdentityRule(
    const RepairIdentityRequest& request) {
  RepairIdentityDecision authority = ValidateCommonAuthority(request);
  if (!authority.ok()) {
    return authority;
  }

  if (request.action == RepairIdentityAction::salvage_review) {
    return EvaluateSalvageReview(request);
  }
  if (request.action == RepairIdentityAction::salvage_promote_with_authority &&
      !request.authoritative_payload_proof) {
    return Refused(request,
                   "SB-REPAIR-IDENTITY-SALVAGE-PROOF-REQUIRED",
                   "storage.repair_identity.salvage_proof_required");
  }

  RepairIdentityDecision metadata = ValidateInputMetadata(request);
  if (!metadata.ok()) {
    return metadata;
  }

  switch (request.action) {
    case RepairIdentityAction::exact_relocation:
    case RepairIdentityAction::page_rewrite:
      return EvaluateExactIdentity(request);
    case RepairIdentityAction::logical_correction:
      return EvaluateLogicalCorrection(request, false);
    case RepairIdentityAction::salvage_promote_with_authority:
      if (!request.salvage_payload_promoted_to_committed) {
        return Refused(request,
                       "SB-REPAIR-IDENTITY-SALVAGE-PROMOTION-REQUIRED",
                       "storage.repair_identity.salvage_promotion_required");
      }
      return EvaluateLogicalCorrection(request, true);
    case RepairIdentityAction::salvage_review:
      return EvaluateSalvageReview(request);
  }
  return Refused(request,
                 "SB-REPAIR-IDENTITY-ACTION-INVALID",
                 "storage.repair_identity.action_invalid");
}

DiagnosticRecord MakeRepairIdentityDiagnostic(Status status,
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
                        "storage.page.repair_identity");
}

}  // namespace scratchbird::storage::page
