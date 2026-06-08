// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_crash_reopen_persistence.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

bool Empty(std::string_view value) {
  return value.empty();
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

bool IsHashLike(std::string_view value) {
  return StartsWith(value, "sha256:");
}

bool IsPlaceholderResultContract(std::string_view value) {
  return value.empty() || value == "result-contract-v1" ||
         value == "sha256:result-contract-v1";
}

void RequireField(OptimizerCrashReopenPersistenceValidation* validation,
                  bool present,
                  std::string field_name) {
  if (!present) validation->missing_fields.push_back(std::move(field_name));
}

std::string RecordPrefix(const OptimizerCrashReopenPersistenceRecord& record) {
  if (!record.record_id.empty()) return record.record_id;
  if (!record.artifact_uuid.empty()) return record.artifact_uuid;
  return OptimizerPersistentArtifactKindName(record.artifact_kind);
}

void AddDiagnostic(OptimizerCrashReopenPersistenceValidation* validation,
                   const OptimizerCrashReopenPersistenceRecord& record,
                   std::string diagnostic) {
  validation->diagnostics.push_back(RecordPrefix(record) + ":" +
                                    std::move(diagnostic));
}

bool IsDriverVisibleRoute(std::string_view route) {
  return route == "embedded" || route == "ipc" || route == "inet" ||
         route == "cli" || route == "driver";
}

bool HasAuthorityDrift(const OptimizerCrashReopenAuthorityFlags& authority) {
  return authority.transaction_finality_authority ||
         authority.visibility_authority ||
         authority.authorization_security_authority ||
         authority.recovery_authority ||
         authority.parser_authority ||
         authority.donor_authority ||
         authority.wal_authority ||
         authority.benchmark_authority ||
         authority.optimizer_plan_authority ||
         authority.index_finality_authority ||
         authority.provider_finality_authority ||
         authority.local_cluster_authority ||
         authority.cluster_authority ||
         authority.agent_action_authority;
}

bool RequiresCommittedMgaInventory(
    OptimizerReopenDecision decision,
    OptimizerCrashPoint point) {
  if (decision == OptimizerReopenDecision::kReloadAccepted ||
      decision == OptimizerReopenDecision::kRecoveredRebuilt) {
    return true;
  }
  return point == OptimizerCrashPoint::kAfterCommitBeforePublish ||
         point == OptimizerCrashPoint::kAfterPublish;
}

bool CommonPersistentFieldsPresent(
    const OptimizerCrashReopenPersistenceRecord& record) {
  return !record.artifact_uuid.empty() &&
         !record.artifact_generation_uuid.empty() &&
         record.artifact_generation > 1 &&
         record.persisted_generation > 1 &&
         IsHashLike(record.schema_digest) &&
         IsHashLike(record.payload_checksum) &&
         IsHashLike(record.pre_crash_payload_digest) &&
         IsHashLike(record.result_contract_hash) &&
         !IsPlaceholderResultContract(record.result_contract_hash);
}

void ValidateMgaInventory(
    OptimizerCrashReopenPersistenceValidation* validation,
    const OptimizerCrashReopenPersistenceRecord& record) {
  const auto& mga = record.mga_inventory;
  if (mga.transaction_inventory_uuid.empty() || mga.transaction_uuid.empty() ||
      mga.transaction_number == 0 || mga.inventory_generation <= 1 ||
      !IsHashLike(mga.inventory_snapshot_digest) ||
      !IsHashLike(mga.transaction_state_digest) ||
      !IsHashLike(mga.visibility_horizon_digest)) {
    AddDiagnostic(validation, record,
                  "SB_OPT_CRASH_REOPEN.MGA_INVENTORY_FIELDS_MISSING");
  }
  if (!mga.inventory_present || !mga.validated_after_reopen ||
      !mga.checksum_valid || !mga.generation_matches_artifact ||
      !mga.inventory_is_finality_authority || !mga.evidence_only_for_optimizer) {
    AddDiagnostic(validation, record,
                  "SB_OPT_CRASH_REOPEN.MGA_INVENTORY_VALIDATION_MISSING");
  }
  if (RequiresCommittedMgaInventory(record.reopen_decision,
                                    record.crash_point) &&
      !mga.transaction_committed) {
    AddDiagnostic(validation, record,
                  "SB_OPT_CRASH_REOPEN.MGA_COMMITTED_INVENTORY_REQUIRED");
  }
}

void ValidateArtifactSpecificFields(
    OptimizerCrashReopenPersistenceValidation* validation,
    const OptimizerCrashReopenPersistenceRecord& record) {
  switch (record.artifact_kind) {
    case OptimizerPersistentArtifactKind::kStatisticsSnapshot:
      if (!IsHashLike(record.statistics_snapshot_digest) ||
          record.statistics_epoch <= 1) {
        AddDiagnostic(validation, record,
                      "SB_OPT_CRASH_REOPEN.STATISTICS_ARTIFACT_FIELDS_MISSING");
      }
      break;
    case OptimizerPersistentArtifactKind::kPlanCache:
      if (!IsHashLike(record.logical_plan_hash) ||
          !IsHashLike(record.physical_plan_hash) ||
          !IsHashLike(record.plan_cache_key_hash)) {
        AddDiagnostic(validation, record,
                      "SB_OPT_CRASH_REOPEN.PLAN_CACHE_FIELDS_MISSING");
      }
      break;
    case OptimizerPersistentArtifactKind::kAdaptiveCardinalityFeedback:
      if (!IsHashLike(record.feedback_digest) ||
          record.feedback_generation <= 1) {
        AddDiagnostic(validation, record,
                      "SB_OPT_CRASH_REOPEN.FEEDBACK_FIELDS_MISSING");
      }
      break;
    case OptimizerPersistentArtifactKind::kMemoryFeedback:
      if (!IsHashLike(record.memory_feedback_digest) ||
          record.memory_feedback_generation <= 1) {
        AddDiagnostic(validation, record,
                      "SB_OPT_CRASH_REOPEN.MEMORY_FEEDBACK_FIELDS_MISSING");
      }
      break;
    case OptimizerPersistentArtifactKind::kBenchmarkEvidence: {
      if (!record.has_benchmark_record) {
        AddDiagnostic(validation, record,
                      "SB_OPT_CRASH_REOPEN.BENCHMARK_RECORD_MISSING");
        break;
      }
      const auto benchmark_validation =
          ValidatePersistedOptimizerBenchmarkEvidenceRecord(
              record.benchmark_record);
      if (!benchmark_validation.ok) {
        AddDiagnostic(validation, record,
                      "SB_OPT_CRASH_REOPEN.BENCHMARK_RECORD_INVALID");
        validation->diagnostics.insert(validation->diagnostics.end(),
                                       benchmark_validation.diagnostics.begin(),
                                       benchmark_validation.diagnostics.end());
        validation->missing_fields.insert(
            validation->missing_fields.end(),
            benchmark_validation.missing_fields.begin(),
            benchmark_validation.missing_fields.end());
      }
      break;
    }
    case OptimizerPersistentArtifactKind::kRuntimeRouteEvidence:
      if (!IsHashLike(record.runtime_route_evidence_digest) ||
          !IsDriverVisibleRoute(record.route_kind) ||
          record.route_label.empty()) {
        AddDiagnostic(validation, record,
                      "SB_OPT_CRASH_REOPEN.RUNTIME_ROUTE_FIELDS_MISSING");
      }
      break;
  }
}

void ValidateCommonFields(
    OptimizerCrashReopenPersistenceValidation* validation,
    const OptimizerCrashReopenPersistenceRecord& record) {
  RequireField(validation,
               record.schema_id == kOptimizerCrashReopenPersistenceSchemaId,
               "schema_id");
  RequireField(validation,
               record.schema_version_major ==
                   kOptimizerCrashReopenPersistenceSchemaMajor,
               "schema_version_major");
  RequireField(validation,
               record.schema_version_minor ==
                   kOptimizerCrashReopenPersistenceSchemaMinor,
               "schema_version_minor");
  RequireField(validation, !Empty(record.record_id), "record_id");
  RequireField(validation, CommonPersistentFieldsPresent(record),
               "persistent_artifact_identity");
  RequireField(validation, IsDriverVisibleRoute(record.route_kind),
               "route_kind");
  RequireField(validation, !Empty(record.route_label), "route_label");
  RequireField(validation, !Empty(record.decision_diagnostic_code),
               "decision_diagnostic_code");

  if (record.catalog_epoch <= 1 || record.security_epoch <= 1 ||
      record.redaction_epoch <= 1 || record.statistics_epoch <= 1 ||
      record.feedback_generation <= 1 ||
      record.memory_feedback_generation <= 1 ||
      record.provider_generation <= 1) {
    AddDiagnostic(validation, record,
                  "SB_OPT_CRASH_REOPEN.PLACEHOLDER_EPOCH");
  }
  if (!record.catalog_epoch_compatible ||
      !record.security_epoch_compatible ||
      !record.redaction_epoch_compatible ||
      !record.statistics_epoch_compatible ||
      !record.redaction_applied ||
      !record.trusted_provenance ||
      !record.fresh_after_reopen ||
      !record.evidence_only) {
    AddDiagnostic(validation, record,
                  "SB_OPT_CRASH_REOPEN.PROVENANCE_EPOCH_INVALID");
  }
  if (record.placeholder_runtime_evidence || record.synthetic_only_artifact ||
      record.local_default_statistics || record.policy_default_statistics) {
    AddDiagnostic(validation, record,
                  "SB_OPT_CRASH_REOPEN.SYNTHETIC_OR_PLACEHOLDER");
  }
  if (!record.donor_reference_only || record.donor_as_authority ||
      record.uses_donor_storage_or_finality_for_scratchbird) {
    AddDiagnostic(validation, record,
                  "SB_OPT_CRASH_REOPEN.DONOR_AUTHORITY_DRIFT");
  }
  if (record.production_benchmark_clean_claim) {
    AddDiagnostic(validation, record,
                  "SB_OPT_CRASH_REOPEN.BENCHMARK_CLEAN_OVERCLAIM");
  }
  if (HasAuthorityDrift(record.authority)) {
    AddDiagnostic(validation, record,
                  "SB_OPT_CRASH_REOPEN.FORBIDDEN_AUTHORITY");
  }
  if (record.cluster_mode ==
          OptimizerCrashReopenClusterMode::kLocalClusterEvidence ||
      record.route_kind == "cluster") {
    AddDiagnostic(validation, record,
                  "SB_OPT_CRASH_REOPEN.LOCAL_CLUSTER_FORBIDDEN");
  } else if (record.cluster_mode ==
             OptimizerCrashReopenClusterMode::kExternalProviderDelegated) {
    if (record.external_cluster_provider_id.empty() ||
        !record.cluster_claim_blocked) {
      AddDiagnostic(validation, record,
                    "SB_OPT_CRASH_REOPEN.EXTERNAL_CLUSTER_CLAIM_BLOCK_REQUIRED");
    }
  }
}

void ValidateReopenDecision(
    OptimizerCrashReopenPersistenceValidation* validation,
    const OptimizerCrashReopenPersistenceRecord& record) {
  switch (record.reopen_decision) {
    case OptimizerReopenDecision::kReloadAccepted:
      if (!record.durable_storage_write_completed ||
          !record.fsync_evidence_present ||
          !record.publish_generation_visible ||
          !record.reload_attempted ||
          !record.checksum_valid ||
          !IsHashLike(record.post_reopen_payload_digest) ||
          record.post_reopen_payload_digest != record.pre_crash_payload_digest ||
          record.partial_write_detected) {
        AddDiagnostic(validation, record,
                      "SB_OPT_CRASH_REOPEN.RELOAD_ACCEPTANCE_PROOF_MISSING");
      }
      break;
    case OptimizerReopenDecision::kReloadRefused:
      if (!record.reload_attempted ||
          record.refusal_diagnostic_code.empty()) {
        AddDiagnostic(validation, record,
                      "SB_OPT_CRASH_REOPEN.RELOAD_REFUSAL_PROOF_MISSING");
      }
      break;
    case OptimizerReopenDecision::kRecoveredRebuilt:
      if (!record.reload_attempted ||
          !record.replay_performed ||
          !IsHashLike(record.recovery_rebuild_digest) ||
          record.decision_diagnostic_code.empty()) {
        AddDiagnostic(validation, record,
                      "SB_OPT_CRASH_REOPEN.RECOVERY_REBUILD_PROOF_MISSING");
      }
      break;
    case OptimizerReopenDecision::kQuarantined:
      if (!record.reload_attempted ||
          !IsHashLike(record.quarantine_digest) ||
          record.refusal_diagnostic_code.empty()) {
        AddDiagnostic(validation, record,
                      "SB_OPT_CRASH_REOPEN.QUARANTINE_PROOF_MISSING");
      }
      break;
  }
}

void ValidateCrashPoint(
    OptimizerCrashReopenPersistenceValidation* validation,
    const OptimizerCrashReopenPersistenceRecord& record) {
  switch (record.crash_point) {
    case OptimizerCrashPoint::kBeforePersist:
      if (record.durable_storage_write_completed ||
          record.reopen_decision == OptimizerReopenDecision::kReloadAccepted) {
        AddDiagnostic(validation, record,
                      "SB_OPT_CRASH_REOPEN.BEFORE_PERSIST_ACCEPTED");
      }
      break;
    case OptimizerCrashPoint::kDuringWrite:
      if (!record.partial_write_detected ||
          (record.reopen_decision != OptimizerReopenDecision::kReloadRefused &&
           record.reopen_decision != OptimizerReopenDecision::kQuarantined)) {
        AddDiagnostic(validation, record,
                      "SB_OPT_CRASH_REOPEN.PARTIAL_WRITE_NOT_REFUSED");
      }
      break;
    case OptimizerCrashPoint::kAfterFsyncBeforeCommit:
      if (!record.fsync_evidence_present ||
          record.mga_inventory.transaction_committed ||
          record.reopen_decision == OptimizerReopenDecision::kReloadAccepted) {
        AddDiagnostic(validation, record,
                      "SB_OPT_CRASH_REOPEN.FSYNC_BEFORE_COMMIT_ACCEPTED");
      }
      break;
    case OptimizerCrashPoint::kAfterCommitBeforePublish:
      if (!record.fsync_evidence_present ||
          !record.mga_inventory.transaction_committed ||
          record.publish_generation_visible ||
          record.reopen_decision == OptimizerReopenDecision::kReloadAccepted) {
        AddDiagnostic(validation, record,
                      "SB_OPT_CRASH_REOPEN.COMMIT_BEFORE_PUBLISH_INVALID");
      }
      break;
    case OptimizerCrashPoint::kAfterPublish:
      if (!record.fsync_evidence_present ||
          !record.mga_inventory.transaction_committed ||
          !record.publish_generation_visible ||
          record.reopen_decision != OptimizerReopenDecision::kReloadAccepted) {
        AddDiagnostic(validation, record,
                      "SB_OPT_CRASH_REOPEN.AFTER_PUBLISH_NOT_ACCEPTED");
      }
      break;
    case OptimizerCrashPoint::kDuringReopen:
      if (!record.reload_attempted ||
          record.reopen_decision == OptimizerReopenDecision::kReloadAccepted) {
        AddDiagnostic(validation, record,
                      "SB_OPT_CRASH_REOPEN.DURING_REOPEN_NOT_SAFE");
      }
      break;
    case OptimizerCrashPoint::kAfterReplayRefusal:
      if (!record.reload_attempted ||
          !record.replay_performed ||
          record.replay_refusal_diagnostic_code.empty() ||
          record.reopen_decision != OptimizerReopenDecision::kReloadRefused) {
        AddDiagnostic(validation, record,
                      "SB_OPT_CRASH_REOPEN.REPLAY_REFUSAL_PROOF_MISSING");
      }
      break;
  }
}

}  // namespace

const char* OptimizerPersistentArtifactKindName(
    OptimizerPersistentArtifactKind value) {
  switch (value) {
    case OptimizerPersistentArtifactKind::kStatisticsSnapshot:
      return "statistics_snapshot";
    case OptimizerPersistentArtifactKind::kPlanCache:
      return "plan_cache";
    case OptimizerPersistentArtifactKind::kAdaptiveCardinalityFeedback:
      return "adaptive_cardinality_feedback";
    case OptimizerPersistentArtifactKind::kMemoryFeedback:
      return "memory_feedback";
    case OptimizerPersistentArtifactKind::kBenchmarkEvidence:
      return "benchmark_evidence";
    case OptimizerPersistentArtifactKind::kRuntimeRouteEvidence:
      return "runtime_route_evidence";
  }
  return "unknown";
}

const char* OptimizerCrashPointName(OptimizerCrashPoint value) {
  switch (value) {
    case OptimizerCrashPoint::kBeforePersist:
      return "before_persist";
    case OptimizerCrashPoint::kDuringWrite:
      return "during_write";
    case OptimizerCrashPoint::kAfterFsyncBeforeCommit:
      return "after_fsync_before_commit";
    case OptimizerCrashPoint::kAfterCommitBeforePublish:
      return "after_commit_before_publish";
    case OptimizerCrashPoint::kAfterPublish:
      return "after_publish";
    case OptimizerCrashPoint::kDuringReopen:
      return "during_reopen";
    case OptimizerCrashPoint::kAfterReplayRefusal:
      return "after_replay_refusal";
  }
  return "unknown";
}

const char* OptimizerReopenDecisionName(OptimizerReopenDecision value) {
  switch (value) {
    case OptimizerReopenDecision::kReloadAccepted:
      return "reload_accepted";
    case OptimizerReopenDecision::kReloadRefused:
      return "reload_refused";
    case OptimizerReopenDecision::kRecoveredRebuilt:
      return "recovered_rebuilt";
    case OptimizerReopenDecision::kQuarantined:
      return "quarantined";
  }
  return "unknown";
}

std::vector<OptimizerPersistentArtifactKind>
RequiredOptimizerCrashReopenArtifactKinds() {
  return {
      OptimizerPersistentArtifactKind::kStatisticsSnapshot,
      OptimizerPersistentArtifactKind::kPlanCache,
      OptimizerPersistentArtifactKind::kAdaptiveCardinalityFeedback,
      OptimizerPersistentArtifactKind::kMemoryFeedback,
      OptimizerPersistentArtifactKind::kBenchmarkEvidence,
      OptimizerPersistentArtifactKind::kRuntimeRouteEvidence,
  };
}

std::vector<OptimizerCrashPoint> RequiredOptimizerCrashPoints() {
  return {
      OptimizerCrashPoint::kBeforePersist,
      OptimizerCrashPoint::kDuringWrite,
      OptimizerCrashPoint::kAfterFsyncBeforeCommit,
      OptimizerCrashPoint::kAfterCommitBeforePublish,
      OptimizerCrashPoint::kAfterPublish,
      OptimizerCrashPoint::kDuringReopen,
      OptimizerCrashPoint::kAfterReplayRefusal,
  };
}

OptimizerCrashReopenPersistenceValidation
ValidateOptimizerCrashReopenPersistenceRecord(
    const OptimizerCrashReopenPersistenceRecord& record) {
  OptimizerCrashReopenPersistenceValidation validation;

  ValidateCommonFields(&validation, record);
  ValidateMgaInventory(&validation, record);
  ValidateArtifactSpecificFields(&validation, record);
  ValidateReopenDecision(&validation, record);
  ValidateCrashPoint(&validation, record);

  validation.ok =
      validation.missing_fields.empty() && validation.diagnostics.empty();
  validation.crash_reopen_proven =
      validation.ok &&
      record.cluster_mode == OptimizerCrashReopenClusterMode::kNoCluster;
  if (validation.ok) {
    validation.diagnostic_code = "SB_OPT_CRASH_REOPEN.OK";
  } else if (!validation.missing_fields.empty()) {
    validation.diagnostic_code =
        "SB_OPT_CRASH_REOPEN.MISSING_REQUIRED_FIELD";
  } else {
    validation.diagnostic_code = "SB_OPT_CRASH_REOPEN.INVALID_CONTRACT";
  }
  return validation;
}

OptimizerCrashReopenPersistenceValidation
ValidateOptimizerCrashReopenPersistenceMatrix(
    const std::vector<OptimizerCrashReopenPersistenceRecord>& records,
    const std::vector<OptimizerPersistentArtifactKind>& required_artifacts,
    const std::vector<OptimizerCrashPoint>& required_crash_points) {
  OptimizerCrashReopenPersistenceValidation validation;
  if (records.empty()) {
    validation.diagnostic_code = "SB_OPT_CRASH_REOPEN.EMPTY_MATRIX";
    validation.diagnostics.push_back("SB_OPT_CRASH_REOPEN.EMPTY_MATRIX");
    return validation;
  }

  std::set<std::string> seen_records;
  std::set<OptimizerPersistentArtifactKind> seen_artifacts;
  std::set<OptimizerCrashPoint> seen_crash_points;
  std::set<OptimizerReopenDecision> seen_decisions;
  bool all_records_proven = true;

  for (const auto& record : records) {
    if (!record.record_id.empty() &&
        !seen_records.insert(record.record_id).second) {
      AddDiagnostic(&validation, record,
                    "SB_OPT_CRASH_REOPEN.DUPLICATE_RECORD");
    }
    seen_artifacts.insert(record.artifact_kind);
    seen_crash_points.insert(record.crash_point);
    seen_decisions.insert(record.reopen_decision);

    const auto record_validation =
        ValidateOptimizerCrashReopenPersistenceRecord(record);
    if (!record_validation.ok) {
      validation.diagnostics.push_back(
          RecordPrefix(record) + ":" + record_validation.diagnostic_code);
      validation.diagnostics.insert(validation.diagnostics.end(),
                                    record_validation.diagnostics.begin(),
                                    record_validation.diagnostics.end());
      validation.missing_fields.insert(
          validation.missing_fields.end(),
          record_validation.missing_fields.begin(),
          record_validation.missing_fields.end());
    }
    all_records_proven =
        all_records_proven && record_validation.crash_reopen_proven;
  }

  for (const auto artifact : required_artifacts) {
    if (!seen_artifacts.contains(artifact)) {
      validation.diagnostics.push_back(
          std::string("SB_OPT_CRASH_REOPEN.MISSING_ARTIFACT_KIND:") +
          OptimizerPersistentArtifactKindName(artifact));
    }
  }
  for (const auto point : required_crash_points) {
    if (!seen_crash_points.contains(point)) {
      validation.diagnostics.push_back(
          std::string("SB_OPT_CRASH_REOPEN.MISSING_CRASH_POINT:") +
          OptimizerCrashPointName(point));
    }
  }
  for (const auto decision :
       {OptimizerReopenDecision::kReloadAccepted,
        OptimizerReopenDecision::kReloadRefused,
        OptimizerReopenDecision::kRecoveredRebuilt,
        OptimizerReopenDecision::kQuarantined}) {
    if (!seen_decisions.contains(decision)) {
      validation.diagnostics.push_back(
          std::string("SB_OPT_CRASH_REOPEN.MISSING_REOPEN_DECISION:") +
          OptimizerReopenDecisionName(decision));
    }
  }

  validation.ok =
      validation.missing_fields.empty() && validation.diagnostics.empty();
  validation.crash_reopen_proven = validation.ok && all_records_proven;
  validation.diagnostic_code = validation.ok
                                   ? "SB_OPT_CRASH_REOPEN.MATRIX_OK"
                                   : "SB_OPT_CRASH_REOPEN.MATRIX_INVALID";
  return validation;
}

}  // namespace scratchbird::engine::optimizer
