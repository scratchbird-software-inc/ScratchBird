// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB_VECTOR_PROVIDER_MAINTENANCE
#include "index_spatial_vector_graph_access.hpp"
#include "vector_exact_physical_provider.hpp"
#include "vector_hnsw_physical_provider.hpp"
#include "vector_ivf_pq_physical_provider.hpp"

#include <optional>
#include <string>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kVectorProviderMaintenanceSearchKey =
    "SB_VECTOR_PROVIDER_MAINTENANCE";

enum class VectorProviderMaintenanceKind : u32 {
  unknown = 0,
  exact = 1,
  hnsw = 2,
  ivf_pq = 3
};

enum class VectorProviderMaintenanceJobKind : u32 {
  unknown = 0,
  validate = 1,
  compact = 2,
  retrain = 3,
  rebuild = 4,
  repair = 5,
  publish = 6
};

enum class VectorProviderMaintenanceJobState : u32 {
  created = 1,
  scheduled = 2,
  running = 3,
  cancelled = 4,
  validation_failed = 5,
  validation_passed = 6,
  publish_ready = 7,
  published = 8,
  failed = 9,
  refused = 10
};

enum class VectorProviderMaintenanceFailureClass : u32 {
  none = 0,
  transient_provider_unavailable = 1,
  permanent_validation_failed = 2,
  stale_generation = 3,
  authority_boundary_refused = 4,
  unsafe_publish = 5,
  restricted_repair_required = 6,
  invalid_state = 7,
  unsupported_provider = 8,
  missing_proof = 9
};

enum class VectorProviderRepairClass : u32 {
  none = 0,
  bad_checksum = 1,
  corrupt_payload = 2,
  invalid_graph = 3,
  invalid_list = 4,
  invalid_codebook = 5,
  invalid_centroid = 6,
  invalid_code = 7,
  restricted_repair_required = 8
};

struct VectorProviderMaintenanceProof {
  bool proof_supplied = false;
  bool exact_source_available = false;
  bool exact_recheck_proof_supplied = false;
  bool mga_recheck_proof_supplied = false;
  bool security_recheck_proof_supplied = false;
  bool candidate_only_non_authority = false;
  bool validation_successful = false;
  bool generation_advanced = false;
  bool parser_finality_authority_claimed = false;
  bool reference_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool index_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
  bool visibility_authority_claimed = false;
  bool security_authority_claimed = false;
  bool transaction_finality_authority_claimed = false;
  std::string evidence_ref;
};

struct VectorProviderMaintenancePolicy {
  double min_recall_floor = 0.95;
  double max_recall_drift = 0.03;
  double max_tombstone_ratio = 0.20;
  double max_list_imbalance_ratio = 2.0;
  double max_residual_error_mean = 0.50;
  double max_compression_error_mean = 0.50;
  u64 max_latency_units = 0;
  u64 max_retry_attempts = 3;
};

struct VectorProviderMaintenanceContext {
  std::string collection_uuid;
  std::string index_uuid;
  std::string provider_uuid;
  u64 expected_provider_generation = 0;
  u64 expected_training_generation = 0;
  u64 expected_descriptor_epoch = 0;
  u64 expected_metric_resource_epoch = 0;
  u64 now_microseconds = 0;
  VectorProviderMaintenanceProof proof;
  VectorProviderMaintenancePolicy policy;
};

struct VectorProviderValidationResult {
  Status status;
  DiagnosticRecord diagnostic;
  VectorProviderMaintenanceKind provider_kind =
      VectorProviderMaintenanceKind::unknown;
  bool fail_closed = true;
  bool validated = false;
  bool serialize_open_path_consumed = false;
  bool query_sanity_consumed = false;
  bool candidate_only_non_authority = false;
  bool restricted_repair_required = false;
  VectorProviderRepairClass repair_class = VectorProviderRepairClass::none;
  std::string repair_reason;
  u64 provider_generation = 0;
  u64 training_generation = 0;
  std::vector<std::string> evidence;
  std::vector<std::string> support_bundle_rows;

  bool ok() const { return status.ok() && validated && !fail_closed; }
};

struct VectorProviderMaintenanceJob {
  std::string job_id;
  VectorProviderMaintenanceKind provider_kind =
      VectorProviderMaintenanceKind::unknown;
  VectorProviderMaintenanceJobKind job_kind =
      VectorProviderMaintenanceJobKind::unknown;
  VectorProviderMaintenanceJobState state =
      VectorProviderMaintenanceJobState::created;
  VectorProviderMaintenanceFailureClass failure_class =
      VectorProviderMaintenanceFailureClass::none;
  std::string collection_uuid;
  std::string index_uuid;
  std::string provider_uuid;
  u64 old_provider_generation = 0;
  u64 new_provider_generation = 0;
  u64 old_training_generation = 0;
  u64 new_training_generation = 0;
  u64 completed_units = 0;
  u64 total_units = 1;
  u64 retry_attempts = 0;
  u64 max_retry_attempts = 0;
  bool validation_successful = false;
  bool publish_candidate_available = false;
  bool candidate_only_non_authority = false;
  VectorProviderRepairClass repair_class = VectorProviderRepairClass::none;
  std::string repair_reason;
  std::vector<std::string> policy_reasons;
  std::vector<std::string> evidence;
  std::vector<std::string> support_bundle_rows;
  std::optional<VectorExactPhysicalProvider> exact_candidate;
  std::optional<VectorHnswPhysicalProvider> hnsw_candidate;
  std::optional<VectorIvfPqPhysicalProvider> ivf_pq_candidate;
};

struct VectorProviderMaintenanceResult {
  Status status;
  DiagnosticRecord diagnostic;
  VectorProviderMaintenanceJob job;
  bool fail_closed = true;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct VectorProviderPublishResult {
  Status status;
  DiagnosticRecord diagnostic;
  VectorProviderMaintenanceJob job;
  bool fail_closed = true;
  bool published = false;
  std::optional<VectorExactPhysicalProvider> exact_provider;
  std::optional<VectorHnswPhysicalProvider> hnsw_provider;
  std::optional<VectorIvfPqPhysicalProvider> ivf_pq_provider;
  std::vector<std::string> evidence;
  std::vector<std::string> support_bundle_rows;

  bool ok() const { return status.ok() && published && !fail_closed; }
};

const char* VectorProviderMaintenanceKindName(
    VectorProviderMaintenanceKind kind);
const char* VectorProviderMaintenanceJobKindName(
    VectorProviderMaintenanceJobKind kind);
const char* VectorProviderMaintenanceJobStateName(
    VectorProviderMaintenanceJobState state);
const char* VectorProviderMaintenanceFailureClassName(
    VectorProviderMaintenanceFailureClass failure_class);
const char* VectorProviderRepairClassName(VectorProviderRepairClass repair);

VectorExactRecheckProof ToVectorExactRecheckProof(
    const VectorProviderMaintenanceProof& proof);

std::string DeterministicVectorProviderMaintenanceJobId(
    const VectorProviderMaintenanceContext& context,
    VectorProviderMaintenanceKind provider_kind,
    VectorProviderMaintenanceJobKind job_kind,
    u64 new_provider_generation,
    u64 new_training_generation);

VectorProviderValidationResult ValidateVectorExactProviderForMaintenance(
    const VectorExactPhysicalProvider& provider,
    const VectorProviderMaintenanceContext& context);
VectorProviderValidationResult ValidateVectorHnswProviderForMaintenance(
    const VectorHnswPhysicalProvider& provider,
    const VectorProviderMaintenanceContext& context);
VectorProviderValidationResult ValidateVectorIvfPqProviderForMaintenance(
    const VectorIvfPqPhysicalProvider& provider,
    const VectorProviderMaintenanceContext& context);

VectorProviderValidationResult DiagnoseVectorExactProviderRepair(
    const std::vector<byte>& bytes,
    const VectorProviderMaintenanceContext& context);
VectorProviderValidationResult DiagnoseVectorHnswProviderRepair(
    const std::vector<byte>& bytes,
    const VectorProviderMaintenanceContext& context);
VectorProviderValidationResult DiagnoseVectorIvfPqProviderRepair(
    const std::vector<byte>& bytes,
    const VectorProviderMaintenanceContext& context);

VectorProviderMaintenanceResult ScheduleVectorExactProviderMaintenance(
    const VectorExactPhysicalProvider& provider,
    const VectorProviderMaintenanceContext& context);
VectorProviderMaintenanceResult ScheduleVectorHnswProviderMaintenance(
    const VectorHnswPhysicalProvider& provider,
    const VectorProviderMaintenanceContext& context);
VectorProviderMaintenanceResult ScheduleVectorIvfPqProviderMaintenance(
    const VectorIvfPqPhysicalProvider& provider,
    const VectorProviderMaintenanceContext& context,
    const std::vector<VectorIvfPqSourceRow>& authoritative_source_rows);

VectorProviderMaintenanceResult RecordVectorProviderMaintenanceProgress(
    VectorProviderMaintenanceJob job,
    u64 completed_units,
    u64 total_units,
    std::string stage);
VectorProviderMaintenanceResult CancelVectorProviderMaintenanceJob(
    VectorProviderMaintenanceJob job,
    std::string reason);
VectorProviderMaintenanceResult ResumeVectorProviderMaintenanceJob(
    VectorProviderMaintenanceJob job);
VectorProviderMaintenanceResult ClassifyVectorProviderMaintenanceFailure(
    VectorProviderMaintenanceJob job,
    VectorProviderMaintenanceFailureClass failure_class,
    bool retryable);

VectorProviderPublishResult PublishVectorProviderMaintenanceCandidate(
    VectorProviderMaintenanceJob job,
    const VectorProviderMaintenanceContext& context,
    u64 active_provider_generation);

DiagnosticRecord MakeVectorProviderMaintenanceDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
