// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-BACKUP-RESTORE-CLOSURE-ANCHOR

#include "index_lineage.hpp"
#include "index_quarantine.hpp"

namespace scratchbird::core::index {

enum class IndexMovementOperation : u32 {
  backup = 1,
  restore = 2,
  archive = 3,
  filespace_move = 4,
  filespace_shrink = 5
};

struct IndexMovementValidationRequest {
  IndexMovementOperation operation = IndexMovementOperation::backup;
  IndexFamily family = IndexFamily::unknown;
  IndexPageAuthorityInput page_authority;
  bool resource_available = false;
  bool transaction_finality_proven = false;
  bool destination_supports_family = false;
};

struct IndexMovementValidationResult {
  Status status;
  bool allowed = false;
  bool verify_after_move = true;
  bool rebuild_after_move = false;
  bool refuse_restore = false;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && allowed && !refuse_restore; }
};

IndexMovementValidationResult ValidateIndexMovement(const IndexMovementValidationRequest& request);
DiagnosticRecord MakeIndexMovementDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail = {});

enum class OptimizedPersistedStructureKind : u32 {
  page_extent_summaries = 1,
  secondary_index_delta_ledgers = 2,
  deferred_index_merge_state = 3,
  cleanup_horizon_markers = 4,
  shadow_index_build_state = 5,
  search_inverted_segments = 6,
  vector_generations = 7,
  optimization_management_metadata = 8
};

enum class OptimizedStructureLifecycleOperation : u32 {
  backup = 1,
  restore = 2,
  validate = 3,
  repair = 4,
  rebuild = 5
};

enum class OptimizedStructureLifecycleAction : u32 {
  preserve_committed = 1,
  validate_then_use = 2,
  repair_then_use = 3,
  rebuild_from_authoritative_base = 4,
  recompute_from_mga_inventory = 5,
  discard_unpublished_then_fallback = 6,
  rebuild_management_projection = 7,
  exact_refusal = 8
};

struct OptimizedStructureLifecycleEvidence {
  std::string key;
  std::string value;
  bool sensitive = false;
};

struct OptimizedStructureLifecycleRequest {
  OptimizedPersistedStructureKind structure =
      OptimizedPersistedStructureKind::page_extent_summaries;
  OptimizedStructureLifecycleOperation operation =
      OptimizedStructureLifecycleOperation::backup;
  IndexMovementValidationRequest movement;
  bool movement_validation_required = true;
  bool manifest_coverage_verified = false;
  bool restore_inspection_open = false;
  bool transaction_finality_proven_by_mga_inventory = false;
  bool authoritative_base_available = false;
  bool support_bundle_evidence_sink_available = false;
  bool repair_mutation_allowed = false;
  bool target_identity_resolved_to_generated_uuids = true;
  bool checksum_valid = true;
  bool structure_supported = true;
  bool published_or_committed_generation = false;
  bool unpublished_generation_present = false;
};

struct OptimizedStructureLifecycleResult {
  Status status;
  bool allowed = false;
  bool survived_backup_restore = false;
  bool validate_required = false;
  bool repair_required = false;
  bool rebuild_required = false;
  bool exact_refusal = false;
  bool support_bundle_evidence_recorded = false;
  bool engine_mga_authority_preserved = false;
  OptimizedStructureLifecycleAction action =
      OptimizedStructureLifecycleAction::exact_refusal;
  std::vector<OptimizedStructureLifecycleEvidence> support_evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && allowed && !exact_refusal; }
};

const char* OptimizedPersistedStructureKindName(
    OptimizedPersistedStructureKind structure);
const char* OptimizedStructureLifecycleOperationName(
    OptimizedStructureLifecycleOperation operation);
const char* OptimizedStructureLifecycleActionName(
    OptimizedStructureLifecycleAction action);
OptimizedStructureLifecycleResult EvaluateOptimizedStructureLifecycle(
    const OptimizedStructureLifecycleRequest& request);
DiagnosticRecord MakeOptimizedStructureLifecycleDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
