// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_benchmark_evidence_schema.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: CEIC_053_OPTIMIZER_CRASH_REOPEN_PERSISTENCE
// Optimizer crash/reopen evidence proves persisted optimizer artifacts are
// reloadable, refused, rebuilt, or quarantined after modeled crash points. It
// is evidence only. It is not transaction finality, visibility,
// authorization/security, recovery, parser, donor, WAL, benchmark,
// optimizer-plan, index-finality, provider-finality, cluster, or agent-action
// authority.
inline constexpr const char* kOptimizerCrashReopenPersistenceSchemaId =
    "sb.optimizer.crash_reopen_persistence.v1";
inline constexpr std::uint32_t kOptimizerCrashReopenPersistenceSchemaMajor = 1;
inline constexpr std::uint32_t kOptimizerCrashReopenPersistenceSchemaMinor = 0;

enum class OptimizerPersistentArtifactKind {
  kStatisticsSnapshot,
  kPlanCache,
  kAdaptiveCardinalityFeedback,
  kMemoryFeedback,
  kBenchmarkEvidence,
  kRuntimeRouteEvidence,
};

enum class OptimizerCrashPoint {
  kBeforePersist,
  kDuringWrite,
  kAfterFsyncBeforeCommit,
  kAfterCommitBeforePublish,
  kAfterPublish,
  kDuringReopen,
  kAfterReplayRefusal,
};

enum class OptimizerReopenDecision {
  kReloadAccepted,
  kReloadRefused,
  kRecoveredRebuilt,
  kQuarantined,
};

enum class OptimizerCrashReopenClusterMode {
  kNoCluster,
  kExternalProviderDelegated,
  kLocalClusterEvidence,
};

struct OptimizerCrashReopenAuthorityFlags {
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool authorization_security_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool donor_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool provider_finality_authority = false;
  bool local_cluster_authority = false;
  bool cluster_authority = false;
  bool agent_action_authority = false;
};

struct OptimizerMgaInventoryValidationEvidence {
  std::string transaction_inventory_uuid;
  std::string transaction_uuid;
  std::uint64_t transaction_number = 0;
  std::uint64_t inventory_generation = 0;
  std::string inventory_snapshot_digest;
  std::string transaction_state_digest;
  std::string visibility_horizon_digest;
  bool inventory_present = false;
  bool transaction_committed = false;
  bool validated_after_reopen = false;
  bool checksum_valid = false;
  bool generation_matches_artifact = false;
  bool inventory_is_finality_authority = true;
  bool evidence_only_for_optimizer = true;
};

struct OptimizerCrashReopenPersistenceRecord {
  std::string schema_id = kOptimizerCrashReopenPersistenceSchemaId;
  std::uint32_t schema_version_major =
      kOptimizerCrashReopenPersistenceSchemaMajor;
  std::uint32_t schema_version_minor =
      kOptimizerCrashReopenPersistenceSchemaMinor;

  std::string record_id;
  OptimizerPersistentArtifactKind artifact_kind =
      OptimizerPersistentArtifactKind::kStatisticsSnapshot;
  OptimizerCrashPoint crash_point = OptimizerCrashPoint::kBeforePersist;
  OptimizerReopenDecision reopen_decision =
      OptimizerReopenDecision::kReloadRefused;

  std::string artifact_uuid;
  std::string artifact_generation_uuid;
  std::uint64_t artifact_generation = 0;
  std::uint64_t persisted_generation = 0;
  std::string schema_digest;
  std::string payload_checksum;
  std::string pre_crash_payload_digest;
  std::string post_reopen_payload_digest;
  std::string recovery_rebuild_digest;
  std::string quarantine_digest;
  std::string refusal_diagnostic_code;
  std::string replay_refusal_diagnostic_code;
  std::string decision_diagnostic_code;

  std::string route_kind;
  std::string route_label;
  std::string result_contract_hash;
  std::string logical_plan_hash;
  std::string physical_plan_hash;
  std::string plan_cache_key_hash;
  std::string statistics_snapshot_digest;
  std::string feedback_digest;
  std::string memory_feedback_digest;
  std::string runtime_route_evidence_digest;

  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t statistics_epoch = 0;
  std::uint64_t feedback_generation = 0;
  std::uint64_t memory_feedback_generation = 0;
  std::uint64_t provider_generation = 0;

  bool catalog_epoch_compatible = false;
  bool security_epoch_compatible = false;
  bool redaction_epoch_compatible = false;
  bool statistics_epoch_compatible = false;
  bool redaction_applied = false;
  bool trusted_provenance = false;
  bool fresh_after_reopen = false;
  bool durable_storage_write_completed = false;
  bool fsync_evidence_present = false;
  bool publish_generation_visible = false;
  bool reload_attempted = false;
  bool replay_performed = false;
  bool partial_write_detected = false;
  bool checksum_valid = false;
  bool placeholder_runtime_evidence = false;
  bool synthetic_only_artifact = false;
  bool local_default_statistics = false;
  bool policy_default_statistics = false;
  bool donor_reference_only = true;
  bool donor_as_authority = false;
  bool uses_donor_storage_or_finality_for_scratchbird = false;
  bool production_benchmark_clean_claim = false;
  bool evidence_only = true;

  OptimizerCrashReopenClusterMode cluster_mode =
      OptimizerCrashReopenClusterMode::kNoCluster;
  std::string external_cluster_provider_id;
  bool cluster_claim_blocked = false;

  OptimizerMgaInventoryValidationEvidence mga_inventory;
  OptimizerCrashReopenAuthorityFlags authority;

  bool has_benchmark_record = false;
  PersistedOptimizerBenchmarkEvidenceRecord benchmark_record;
};

struct OptimizerCrashReopenPersistenceValidation {
  bool ok = false;
  bool crash_reopen_proven = false;
  std::string diagnostic_code;
  std::vector<std::string> missing_fields;
  std::vector<std::string> diagnostics;
};

const char* OptimizerPersistentArtifactKindName(
    OptimizerPersistentArtifactKind value);

const char* OptimizerCrashPointName(OptimizerCrashPoint value);

const char* OptimizerReopenDecisionName(OptimizerReopenDecision value);

std::vector<OptimizerPersistentArtifactKind>
RequiredOptimizerCrashReopenArtifactKinds();

std::vector<OptimizerCrashPoint> RequiredOptimizerCrashPoints();

OptimizerCrashReopenPersistenceValidation
ValidateOptimizerCrashReopenPersistenceRecord(
    const OptimizerCrashReopenPersistenceRecord& record);

OptimizerCrashReopenPersistenceValidation
ValidateOptimizerCrashReopenPersistenceMatrix(
    const std::vector<OptimizerCrashReopenPersistenceRecord>& records,
    const std::vector<OptimizerPersistentArtifactKind>& required_artifacts,
    const std::vector<OptimizerCrashPoint>& required_crash_points);

}  // namespace scratchbird::engine::optimizer
