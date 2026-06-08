// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-ACCESS-METHOD-CLOSURE-ANCHOR
// CEIC_031_INDEX_ACCESS_METHOD_PROVIDER_INTERFACE

#include "index_family_registry.hpp"
#include "index_route_capability.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

struct IndexKeyEnvelope {
  std::string encoded_key;
  bool lossy = false;
  bool requires_recheck = true;
};

struct IndexRowLocator {
  TypedUuid table_uuid;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  u64 local_transaction_id = 0;
};

struct IndexCandidate {
  IndexKeyEnvelope key;
  IndexRowLocator locator;
  bool mga_visible = false;
  bool predicate_exact = false;
  bool security_visible = false;
};

struct IndexOperationMetricsDelta {
  u64 candidates = 0;
  u64 visible = 0;
  u64 rechecks = 0;
  u64 fallback_sorts = 0;
  u64 pages_read = 0;
  u64 pages_written = 0;
};

struct IndexAccessMethodCapabilities {
  IndexFamily family = IndexFamily::unknown;
  bool supports_insert = false;
  bool supports_delete = false;
  bool supports_update = false;
  bool supports_scan = false;
  bool supports_verify = false;
  bool supports_rebuild = false;
  bool returns_lossy_candidates = true;
  bool requires_mga_recheck = true;
  bool requires_security_recheck = true;
  bool can_satisfy_order = false;
  bool can_be_unique = false;
};

struct IndexCandidatePipelineResult {
  Status status;
  std::vector<IndexCandidate> accepted;
  std::vector<IndexCandidate> rejected;
  IndexOperationMetricsDelta metrics;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

enum class IndexProviderAdmissionStatus : u32 {
  admitted = 1,
  missing_provider_evidence = 2,
  static_capability_only = 3,
  unsupported_family = 4,
  non_persistent_family = 5,
  donor_emulated_non_runtime = 6,
  policy_blocked_non_runtime = 7,
  route_capability_required = 8,
  route_not_supported = 9,
  mutation_batch_admission_required = 10,
  generation_handle_required = 11,
  recovery_context_required = 12,
  cleanup_horizon_required = 13,
  validation_repair_required = 14,
  authority_boundary_refused = 15,
  cluster_external_provider_only = 16
};

struct IndexProviderAuthorityBoundary {
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
  bool cluster_authority = false;
  bool agent_action_authority = false;
};

struct IndexProviderIdentity {
  std::string provider_id;
  std::string provider_name;
  std::string provider_contract_version;
  bool persistent_access_method = false;
  bool provider_backed = false;
};

struct IndexProviderGenerationHandle {
  TypedUuid generation_uuid;
  u64 generation_number = 0;
  std::string provider_generation_id;
  bool root_identity_bound = false;
  bool cow_generation = false;
};

struct IndexProviderMutationBatchAdmission {
  TypedUuid batch_uuid;
  u64 operation_count = 0;
  bool batch_admission_requested = false;
  bool provider_batch_admission_supported = false;
  bool deterministic_batch_order = false;
  bool idempotent_replay_safe = false;
};

struct IndexProviderRecoveryContext {
  std::string recovery_context_id;
  bool recovery_reopen_supported = false;
  bool crash_classification_supported = false;
  bool corruption_classification_supported = false;
  bool mga_recovery_evidence_only = false;
};

struct IndexProviderCleanupHorizon {
  u64 oldest_active_transaction_id = 0;
  u64 cleanup_generation_floor = 0;
  bool engine_mga_horizon_bound = false;
  bool provider_cleanup_supported = false;
};

struct IndexProviderValidationRepairSupport {
  bool validate_supported = false;
  bool repair_supported = false;
  bool rebuild_supported = false;
  bool deterministic_diagnostics = false;
};

struct IndexProviderRouteBoundary {
  bool route_capability_present = false;
  bool provider_route_supported = false;
  bool static_registry_complete_capability_seen = false;
  bool cluster_path_requested = false;
  bool external_cluster_provider_only = true;
  bool route_specific_boundary_declared = false;
};

struct IndexProviderAccessMethodContract {
  IndexFamily family = IndexFamily::unknown;
  IndexRouteKind route = IndexRouteKind::unknown;
  IndexProviderIdentity provider;
  IndexProviderRouteBoundary route_boundary;
  IndexProviderMutationBatchAdmission mutation_batch;
  IndexProviderGenerationHandle generation;
  IndexProviderRecoveryContext recovery;
  IndexProviderCleanupHorizon cleanup;
  IndexProviderValidationRepairSupport validation_repair;
  IndexProviderAuthorityBoundary authority_boundary;
  std::vector<std::string> provider_evidence;
};

struct IndexProviderAdmissionResult {
  Status status;
  bool admitted = false;
  bool fail_closed = true;
  bool provider_contract_only = true;
  bool durable_family_closure_claimed = false;
  bool enterprise_ready_claimed = false;
  IndexProviderAdmissionStatus admission_status =
      IndexProviderAdmissionStatus::missing_provider_evidence;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && admitted && !fail_closed; }
};

IndexAccessMethodCapabilities CapabilitiesForFamily(IndexFamily family);
IndexCandidatePipelineResult ApplyIndexCandidateAuthorityPipeline(std::vector<IndexCandidate> candidates);
const char* IndexProviderAdmissionStatusName(IndexProviderAdmissionStatus status);
bool IndexProviderAuthorityBoundaryClear(
    const IndexProviderAuthorityBoundary& boundary);
bool IndexProviderRouteRequiresGeneration(IndexRouteKind route);
bool IndexProviderRouteRequiresMutationBatchAdmission(IndexRouteKind route);
bool IndexProviderRouteRequiresRecoveryContext(IndexRouteKind route);
bool IndexProviderRouteRequiresCleanupHorizon(IndexRouteKind route);
bool IndexProviderRouteRequiresValidationRepair(IndexRouteKind route);
IndexProviderAdmissionResult AdmitIndexProviderAccessMethod(
    const IndexProviderAccessMethodContract& contract);

}  // namespace scratchbird::core::index
