// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_backup_restore.hpp"

#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status ErrorStatus() { return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine}; }

IndexMovementValidationResult Refuse(std::string code, std::string key) {
  IndexMovementValidationResult result;
  result.status = ErrorStatus();
  result.allowed = false;
  result.refuse_restore = true;
  result.diagnostic = MakeIndexMovementDiagnostic(result.status, std::move(code), std::move(key));
  return result;
}

bool RestoreLike(OptimizedStructureLifecycleOperation operation) {
  return operation == OptimizedStructureLifecycleOperation::restore;
}

bool MutatingRepairOperation(OptimizedStructureLifecycleOperation operation) {
  return operation == OptimizedStructureLifecycleOperation::repair ||
         operation == OptimizedStructureLifecycleOperation::rebuild;
}

bool TransientRebuildStructure(OptimizedPersistedStructureKind structure) {
  return structure == OptimizedPersistedStructureKind::page_extent_summaries ||
         structure == OptimizedPersistedStructureKind::deferred_index_merge_state;
}

bool PreserveCommittedStructure(OptimizedPersistedStructureKind structure) {
  return structure ==
             OptimizedPersistedStructureKind::secondary_index_delta_ledgers ||
         structure ==
             OptimizedPersistedStructureKind::search_inverted_segments ||
         structure == OptimizedPersistedStructureKind::vector_generations;
}

bool DiscardUnpublishedStructure(OptimizedPersistedStructureKind structure) {
  return structure == OptimizedPersistedStructureKind::shadow_index_build_state;
}

void AddOptimizedEvidence(OptimizedStructureLifecycleResult* result,
                          std::string key,
                          std::string value,
                          bool sensitive = false) {
  result->support_evidence.push_back(
      {std::move(key), std::move(value), sensitive});
}

void AddOptimizedDiagnosticEvidence(
    OptimizedStructureLifecycleResult* result) {
  AddOptimizedEvidence(result, "diagnostic_code",
                       result->diagnostic.diagnostic_code);
  AddOptimizedEvidence(result, "message_vector_key",
                       result->diagnostic.message_key);
  AddOptimizedEvidence(result, "diagnostic_source",
                       result->diagnostic.source_component);
}

OptimizedStructureLifecycleResult BaseOptimizedResult(
    const OptimizedStructureLifecycleRequest& request) {
  OptimizedStructureLifecycleResult result;
  result.status = OkStatus();
  result.engine_mga_authority_preserved =
      request.transaction_finality_proven_by_mga_inventory;
  AddOptimizedEvidence(&result, "search_key",
                       "ODF_OPTIMIZED_STRUCTURE_BACKUP_REPAIR_CLOSURE");
  AddOptimizedEvidence(&result, "structure",
                       OptimizedPersistedStructureKindName(request.structure));
  AddOptimizedEvidence(&result, "operation",
                       OptimizedStructureLifecycleOperationName(
                           request.operation));
  AddOptimizedEvidence(&result, "finality_source",
                       request.transaction_finality_proven_by_mga_inventory
                           ? "local_mga_transaction_inventory"
                           : "missing_or_unproven");
  AddOptimizedEvidence(&result, "authoritative_wal", "false");
  AddOptimizedEvidence(&result, "parser_finality_authority", "false");
  AddOptimizedEvidence(&result, "reference_finality_authority", "false");
  return result;
}

OptimizedStructureLifecycleResult RefuseOptimizedLifecycle(
    const OptimizedStructureLifecycleRequest& request,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  auto result = BaseOptimizedResult(request);
  result.status = ErrorStatus();
  result.allowed = false;
  result.exact_refusal = true;
  result.support_bundle_evidence_recorded =
      request.support_bundle_evidence_sink_available;
  result.action = OptimizedStructureLifecycleAction::exact_refusal;
  result.diagnostic = MakeOptimizedStructureLifecycleDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key),
      std::move(detail));
  AddOptimizedDiagnosticEvidence(&result);
  AddOptimizedEvidence(&result, "exact_refusal", "true");
  AddOptimizedEvidence(&result, "support_bundle_evidence_recorded",
                       result.support_bundle_evidence_recorded ? "true"
                                                               : "false");
  return result;
}

void AllowOptimizedLifecycle(OptimizedStructureLifecycleResult* result,
                             OptimizedStructureLifecycleAction action,
                             std::string diagnostic_code,
                             std::string message_key,
                             std::string detail = {}) {
  result->status = OkStatus();
  result->allowed = true;
  result->exact_refusal = false;
  result->support_bundle_evidence_recorded = true;
  result->action = action;
  result->diagnostic = MakeOptimizedStructureLifecycleDiagnostic(
      result->status, std::move(diagnostic_code), std::move(message_key),
      std::move(detail));
  AddOptimizedDiagnosticEvidence(result);
  AddOptimizedEvidence(result, "lifecycle_action",
                       OptimizedStructureLifecycleActionName(action));
  AddOptimizedEvidence(result, "support_bundle_evidence_recorded", "true");
}
}  // namespace

IndexMovementValidationResult ValidateIndexMovement(const IndexMovementValidationRequest& request) {
  if (request.family == IndexFamily::unknown || !request.destination_supports_family) {
    return Refuse("SB-INDEX-MOVEMENT-FAMILY-UNSUPPORTED", "index.movement.family_unsupported");
  }
  const IndexQuarantineDecision page = ClassifyIndexPageAuthority(request.page_authority);
  if (!page.ok()) {
    return Refuse("SB-INDEX-MOVEMENT-PAGE-AUTHORITY-REFUSED", "index.movement.page_authority_refused");
  }
  const IndexLineageDecision lineage = DecideIndexLineage(request.family,
                                                         request.resource_available,
                                                         request.transaction_finality_proven);
  if (!lineage.status.ok()) {
    return Refuse("SB-INDEX-MOVEMENT-LINEAGE-REFUSED", "index.movement.lineage_refused");
  }
  IndexMovementValidationResult result;
  result.status = OkStatus();
  result.allowed = true;
  const IndexLineageBehavior behavior =
      request.operation == IndexMovementOperation::restore ? lineage.restore_behavior : lineage.archive_behavior;
  result.rebuild_after_move = behavior == IndexLineageBehavior::rebuild || behavior == IndexLineageBehavior::transform;
  result.verify_after_move = behavior == IndexLineageBehavior::verify || result.rebuild_after_move;
  return result;
}

DiagnosticRecord MakeIndexMovementDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code, status.severity, status.subsystem,
                        std::move(diagnostic_code), std::move(message_key),
                        std::move(arguments), {}, "core.index.movement");
}

const char* OptimizedPersistedStructureKindName(
    OptimizedPersistedStructureKind structure) {
  switch (structure) {
    case OptimizedPersistedStructureKind::page_extent_summaries:
      return "page_extent_summaries";
    case OptimizedPersistedStructureKind::secondary_index_delta_ledgers:
      return "secondary_index_delta_ledgers";
    case OptimizedPersistedStructureKind::deferred_index_merge_state:
      return "deferred_index_merge_state";
    case OptimizedPersistedStructureKind::cleanup_horizon_markers:
      return "cleanup_horizon_markers";
    case OptimizedPersistedStructureKind::shadow_index_build_state:
      return "shadow_index_build_state";
    case OptimizedPersistedStructureKind::search_inverted_segments:
      return "search_inverted_segments";
    case OptimizedPersistedStructureKind::vector_generations:
      return "vector_generations";
    case OptimizedPersistedStructureKind::optimization_management_metadata:
      return "optimization_management_metadata";
  }
  return "unknown";
}

const char* OptimizedStructureLifecycleOperationName(
    OptimizedStructureLifecycleOperation operation) {
  switch (operation) {
    case OptimizedStructureLifecycleOperation::backup: return "backup";
    case OptimizedStructureLifecycleOperation::restore: return "restore";
    case OptimizedStructureLifecycleOperation::validate: return "validate";
    case OptimizedStructureLifecycleOperation::repair: return "repair";
    case OptimizedStructureLifecycleOperation::rebuild: return "rebuild";
  }
  return "unknown";
}

const char* OptimizedStructureLifecycleActionName(
    OptimizedStructureLifecycleAction action) {
  switch (action) {
    case OptimizedStructureLifecycleAction::preserve_committed:
      return "preserve_committed";
    case OptimizedStructureLifecycleAction::validate_then_use:
      return "validate_then_use";
    case OptimizedStructureLifecycleAction::repair_then_use:
      return "repair_then_use";
    case OptimizedStructureLifecycleAction::rebuild_from_authoritative_base:
      return "rebuild_from_authoritative_base";
    case OptimizedStructureLifecycleAction::recompute_from_mga_inventory:
      return "recompute_from_mga_inventory";
    case OptimizedStructureLifecycleAction::discard_unpublished_then_fallback:
      return "discard_unpublished_then_fallback";
    case OptimizedStructureLifecycleAction::rebuild_management_projection:
      return "rebuild_management_projection";
    case OptimizedStructureLifecycleAction::exact_refusal:
      return "exact_refusal";
  }
  return "unknown";
}

OptimizedStructureLifecycleResult EvaluateOptimizedStructureLifecycle(
    const OptimizedStructureLifecycleRequest& request) {
  if (!request.structure_supported) {
    return RefuseOptimizedLifecycle(
        request,
        "ODF.OPT_STRUCTURE.UNSUPPORTED_STRUCTURE_REFUSED",
        "odf.opt_structure.unsupported_structure_refused",
        "optimized persisted structure is not supported by this build");
  }
  if (!request.target_identity_resolved_to_generated_uuids) {
    return RefuseOptimizedLifecycle(
        request,
        "ODF.OPT_STRUCTURE.IDENTITY_REFUSED",
        "odf.opt_structure.identity_refused",
        "catalog identity must be resolved to generated UUIDs before "
        "backup, restore, validate, repair, or rebuild");
  }
  if (!request.support_bundle_evidence_sink_available) {
    return RefuseOptimizedLifecycle(
        request,
        "ODF.OPT_STRUCTURE.SUPPORT_BUNDLE_REQUIRED",
        "odf.opt_structure.support_bundle_required",
        "support-bundle evidence sink is required for lifecycle decision");
  }
  if (!request.transaction_finality_proven_by_mga_inventory) {
    return RefuseOptimizedLifecycle(
        request,
        "ODF.OPT_STRUCTURE.MGA_FINALITY_REQUIRED",
        "odf.opt_structure.mga_finality_required",
        "local MGA transaction inventory finality proof is required");
  }
  if (!request.manifest_coverage_verified) {
    return RefuseOptimizedLifecycle(
        request,
        "ODF.OPT_STRUCTURE.MANIFEST_COVERAGE_REQUIRED",
        "odf.opt_structure.manifest_coverage_required",
        "backup or restore manifest coverage proof is required");
  }
  if (RestoreLike(request.operation) && !request.restore_inspection_open) {
    return RefuseOptimizedLifecycle(
        request,
        "ODF.OPT_STRUCTURE.RESTORE_INSPECTION_REQUIRED",
        "odf.opt_structure.restore_inspection_required",
        "restore must run under restore-inspection open classification");
  }
  if (!request.checksum_valid) {
    return RefuseOptimizedLifecycle(
        request,
        "ODF.OPT_STRUCTURE.CHECKSUM_REFUSED",
        "odf.opt_structure.checksum_refused",
        "optimized persisted structure checksum or integrity marker failed");
  }
  if (request.movement_validation_required &&
      (request.operation == OptimizedStructureLifecycleOperation::backup ||
       request.operation == OptimizedStructureLifecycleOperation::restore)) {
    const auto movement = ValidateIndexMovement(request.movement);
    if (!movement.ok()) {
      return RefuseOptimizedLifecycle(
          request,
          "ODF.OPT_STRUCTURE.MOVEMENT_REFUSED",
          "odf.opt_structure.movement_refused",
          movement.diagnostic.diagnostic_code);
    }
  }

  auto result = BaseOptimizedResult(request);
  result.support_bundle_evidence_recorded = true;
  result.validate_required =
      request.operation == OptimizedStructureLifecycleOperation::restore ||
      request.operation == OptimizedStructureLifecycleOperation::validate;
  AddOptimizedEvidence(&result, "manifest_coverage_verified", "true");
  AddOptimizedEvidence(&result, "restore_inspection_open",
                       request.restore_inspection_open ? "true" : "false");

  if (PreserveCommittedStructure(request.structure) &&
      request.published_or_committed_generation) {
    result.survived_backup_restore = true;
    AllowOptimizedLifecycle(
        &result,
        request.operation == OptimizedStructureLifecycleOperation::validate
            ? OptimizedStructureLifecycleAction::validate_then_use
            : OptimizedStructureLifecycleAction::preserve_committed,
        "ODF.OPT_STRUCTURE.PRESERVED_COMMITTED",
        "odf.opt_structure.preserved_committed");
    return result;
  }

  if (TransientRebuildStructure(request.structure)) {
    if (!request.authoritative_base_available) {
      return RefuseOptimizedLifecycle(
          request,
          "ODF.OPT_STRUCTURE.REBUILD_EVIDENCE_REQUIRED",
          "odf.opt_structure.rebuild_evidence_required",
          "authoritative base-page or committed-ledger evidence is required "
          "to rebuild transient optimized state");
    }
    result.rebuild_required = true;
    result.survived_backup_restore = true;
    AllowOptimizedLifecycle(
        &result,
        OptimizedStructureLifecycleAction::rebuild_from_authoritative_base,
        "ODF.OPT_STRUCTURE.REBUILD_FROM_AUTHORITY",
        "odf.opt_structure.rebuild_from_authority");
    return result;
  }

  if (request.structure ==
      OptimizedPersistedStructureKind::cleanup_horizon_markers) {
    if (!request.authoritative_base_available) {
      return RefuseOptimizedLifecycle(
          request,
          "ODF.OPT_STRUCTURE.MGA_HORIZON_REQUIRED",
          "odf.opt_structure.mga_horizon_required",
          "MGA transaction inventory horizon evidence is required to "
          "recompute cleanup markers");
    }
    result.rebuild_required = true;
    result.survived_backup_restore = true;
    AllowOptimizedLifecycle(
        &result,
        OptimizedStructureLifecycleAction::recompute_from_mga_inventory,
        "ODF.OPT_STRUCTURE.RECOMPUTED_FROM_MGA",
        "odf.opt_structure.recomputed_from_mga");
    return result;
  }

  if (DiscardUnpublishedStructure(request.structure)) {
    if (!request.published_or_committed_generation &&
        !request.unpublished_generation_present) {
      return RefuseOptimizedLifecycle(
          request,
          "ODF.OPT_STRUCTURE.PUBLISH_EVIDENCE_REQUIRED",
          "odf.opt_structure.publish_evidence_required",
          "shadow structure needs published generation or unpublished-state "
          "evidence before restore admission");
    }
    if (request.unpublished_generation_present) {
      if (!request.repair_mutation_allowed &&
          MutatingRepairOperation(request.operation)) {
        return RefuseOptimizedLifecycle(
            request,
            "ODF.OPT_STRUCTURE.REPAIR_POLICY_REQUIRED",
            "odf.opt_structure.repair_policy_required",
            "discarding unpublished optimized state requires repair policy "
            "authority");
      }
      result.repair_required = true;
      result.survived_backup_restore = true;
      AllowOptimizedLifecycle(
          &result,
          OptimizedStructureLifecycleAction::discard_unpublished_then_fallback,
          "ODF.OPT_STRUCTURE.UNPUBLISHED_DISCARDED",
          "odf.opt_structure.unpublished_discarded");
      return result;
    }
    result.survived_backup_restore = true;
    AllowOptimizedLifecycle(&result,
                            OptimizedStructureLifecycleAction::validate_then_use,
                            "ODF.OPT_STRUCTURE.VALIDATED_VISIBLE_GENERATION",
                            "odf.opt_structure.validated_visible_generation");
    return result;
  }

  if (request.structure ==
      OptimizedPersistedStructureKind::optimization_management_metadata) {
    if (!request.authoritative_base_available) {
      return RefuseOptimizedLifecycle(
          request,
          "ODF.OPT_STRUCTURE.MANAGEMENT_METADATA_EVIDENCE_REQUIRED",
          "odf.opt_structure.management_metadata_evidence_required",
          "feature-map and repair-evidence projection is required");
    }
    result.rebuild_required = true;
    result.survived_backup_restore = true;
    AllowOptimizedLifecycle(
        &result,
        OptimizedStructureLifecycleAction::rebuild_management_projection,
        "ODF.OPT_STRUCTURE.MANAGEMENT_METADATA_REBUILT",
        "odf.opt_structure.management_metadata_rebuilt");
    return result;
  }

  return RefuseOptimizedLifecycle(
      request,
      "ODF.OPT_STRUCTURE.UNCLASSIFIED_REFUSED",
      "odf.opt_structure.unclassified_refused",
      "optimized persisted structure has no lifecycle policy");
}

DiagnosticRecord MakeOptimizedStructureLifecycleDiagnostic(
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
                        "core.index.optimized_structure_lifecycle");
}

}  // namespace scratchbird::core::index
