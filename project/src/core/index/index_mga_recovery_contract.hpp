// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-MGA-RECOVERY-CONTRACT-ANCHOR
// CEIC_032_INDEX_MGA_RECOVERY_CONTRACT

#include "index_family_registry.hpp"
#include "index_route_capability.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class IndexGenerationPublishState : u32 {
  unpublished = 1,
  publish_prepared = 2,
  published = 3,
  superseded = 4,
  quarantined = 5,
  unknown = 6
};

enum class IndexCrashRecoveryClassification : u32 {
  clean_reopen = 1,
  crash_before_generation_publish = 2,
  crash_after_generation_publish = 3,
  orphan_generation = 4,
  provider_replay_required = 5,
  unknown = 6
};

enum class IndexCorruptionClassification : u32 {
  none = 1,
  generation_identity_mismatch = 2,
  provider_payload_corrupt = 3,
  checksum_mismatch = 4,
  route_family_mismatch = 5,
  unknown = 6
};

enum class IndexMGARecoveryContractStatus : u32 {
  admitted_contract_evidence = 1,
  unsupported_family = 2,
  non_persistent_family = 3,
  donor_policy_local_route_blocked = 4,
  cluster_external_provider_only = 5,
  missing_provider_evidence = 6,
  missing_mga_inventory = 7,
  missing_mga_snapshot = 8,
  missing_cleanup_horizon = 9,
  stale_mga_evidence = 10,
  forbidden_authority_claim = 11,
  missing_generation_identity = 12,
  cleanup_horizon_not_engine_bound = 13,
  recovery_evidence_not_durable = 14,
  enterprise_readiness_overclaim = 15
};

struct IndexMGARecoveryAuthorityBoundary {
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool authorization_security_authority = false;
  bool security_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool donor_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool provider_finality_authority = false;
  bool cluster_authority = false;
  bool agent_action_authority = false;
};

struct IndexMGARouteFamilyIdentity {
  IndexFamily family = IndexFamily::unknown;
  IndexRouteKind route = IndexRouteKind::unknown;
  std::string provider_id;
  std::string provider_contract_version;
  bool persistent_provider = false;
  bool donor_route_requested = false;
  bool policy_route_requested = false;
  bool cluster_path_requested = false;
  bool external_cluster_provider_only = true;
};

struct IndexMGARecoveryAuthorityEvidence {
  bool inventory_present = false;
  bool inventory_authoritative = false;
  bool inventory_durable = false;
  bool snapshot_present = false;
  bool snapshot_authoritative = false;
  bool cleanup_horizon_present = false;
  bool cleanup_horizon_authoritative = false;
  bool cleanup_horizon_engine_bound = false;
  u64 inventory_epoch = 0;
  u64 snapshot_epoch = 0;
  u64 cleanup_horizon_epoch = 0;
  u64 required_engine_evidence_epoch = 0;
  std::string inventory_evidence_id;
  std::string snapshot_evidence_id;
  std::string cleanup_horizon_evidence_id;
};

struct IndexCOWGenerationIdentity {
  TypedUuid index_uuid;
  TypedUuid generation_uuid;
  u64 generation_number = 0;
  u64 cow_generation_number = 0;
  std::string provider_generation_id;
  bool root_identity_bound = false;
  bool cow_generation_identity_bound = false;
  IndexGenerationPublishState publish_state =
      IndexGenerationPublishState::unknown;
};

struct IndexRecoveryClassificationEvidence {
  IndexCrashRecoveryClassification crash_classification =
      IndexCrashRecoveryClassification::unknown;
  IndexCorruptionClassification corruption_classification =
      IndexCorruptionClassification::unknown;
  std::string recovery_evidence_id;
  bool durable_recovery_evidence = false;
  bool replay_idempotent = false;
  bool provider_evidence_only = true;
};

struct IndexRecoveryRecommendation {
  bool validate = false;
  bool rebuild = false;
  bool replay = false;
  std::vector<std::string> stable_actions;
};

struct IndexMGARecoveryContract {
  IndexMGARouteFamilyIdentity identity;
  IndexMGARecoveryAuthorityEvidence mga_authority;
  IndexCOWGenerationIdentity generation;
  IndexRecoveryClassificationEvidence recovery;
  IndexMGARecoveryAuthorityBoundary authority_boundary;
  std::vector<std::string> provider_evidence;
  bool durable_family_closure_claimed = false;
  bool enterprise_ready_claimed = false;
};

struct IndexMGARecoveryContractResult {
  Status status;
  bool admitted = false;
  bool fail_closed = true;
  bool contract_evidence_only = true;
  bool durable_family_closure_claimed = false;
  bool enterprise_ready_claimed = false;
  IndexMGARecoveryContractStatus contract_status =
      IndexMGARecoveryContractStatus::missing_provider_evidence;
  IndexRecoveryRecommendation recommendation;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && admitted && !fail_closed; }
};

const char* IndexGenerationPublishStateName(IndexGenerationPublishState state);
const char* IndexCrashRecoveryClassificationName(
    IndexCrashRecoveryClassification classification);
const char* IndexCorruptionClassificationName(
    IndexCorruptionClassification classification);
const char* IndexMGARecoveryContractStatusName(
    IndexMGARecoveryContractStatus status);
bool IndexMGARecoveryAuthorityBoundaryClear(
    const IndexMGARecoveryAuthorityBoundary& boundary);
IndexRecoveryRecommendation RecommendIndexRecoveryAction(
    IndexCrashRecoveryClassification crash_classification,
    IndexCorruptionClassification corruption_classification);
IndexMGARecoveryContractResult AdmitIndexMGARecoveryContract(
    const IndexMGARecoveryContract& contract);

}  // namespace scratchbird::core::index
