// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SEQUENCE_GENERATOR_LIFECYCLE
// Engine-owned sequence/generator/identity lifecycle. Values allocated here are
// allocation evidence only; MGA transaction inventory remains commit/rollback
// and visibility authority.

inline constexpr const char* kSequenceGeneratorLifecycleEventMagic = "SBSEQGEN1";

inline constexpr const char* kSequenceDiagnosticOk = "SB_ENGINE_API_OK";
inline constexpr const char* kSequenceDiagnosticDatabasePathRequired = "GENERATOR.DATABASE_PATH_REQUIRED";
inline constexpr const char* kSequenceDiagnosticMgaTransactionRequired = "GENERATOR.MGA_TRANSACTION_REQUIRED";
inline constexpr const char* kSequenceDiagnosticUuidInvalid = "GENERATOR.UUID_INVALID";
inline constexpr const char* kSequenceDiagnosticDuplicateUuid = "GENERATOR.DUPLICATE_UUID";
inline constexpr const char* kSequenceDiagnosticNotFound = "GENERATOR.NOT_FOUND";
inline constexpr const char* kSequenceDiagnosticDropped = "GENERATOR.DROPPED";
inline constexpr const char* kSequenceDiagnosticIncrementInvalid = "GENERATOR.INCREMENT_INVALID";
inline constexpr const char* kSequenceDiagnosticRangeInvalid = "GENERATOR.RANGE_INVALID";
inline constexpr const char* kSequenceDiagnosticCacheInvalid = "GENERATOR.CACHE_INVALID";
inline constexpr const char* kSequenceDiagnosticExhausted = "GENERATOR.EXHAUSTED";
inline constexpr const char* kSequenceDiagnosticUnavailable = "GENERATOR.UNAVAILABLE";
inline constexpr const char* kSequenceDiagnosticClusterPathAbsent = "GENERATOR.CLUSTER_PATH_ABSENT";
inline constexpr const char* kSequenceDiagnosticReferenceMappingIncomplete = "GENERATOR.REFERENCE_MAPPING_INCOMPLETE";
inline constexpr const char* kSequenceDiagnosticIdentityDoubleUuidForbidden =
    "GENERATOR.IDENTITY_DOUBLE_UUID_FORBIDDEN";
inline constexpr const char* kSequenceDiagnosticIdentityAssigned = "GENERATOR.ROW_UUID_IDENTITY_ASSIGNED";
inline constexpr const char* kSequenceDiagnosticMgaRetentionBlocked = "GENERATOR.MGA_RETENTION_BLOCKED";
inline constexpr const char* kSequenceDiagnosticPolicyResolutionFailed =
    "GENERATOR.POLICY_RESOLUTION_FAILED";
inline constexpr const char* kSequenceDiagnosticDatabaseWriteFailed = "GENERATOR.DATABASE_WRITE_FAILED";

struct EngineSequenceGeneratorDefinition {
  std::string generator_uuid;
  std::string database_uuid;
  std::string schema_uuid;
  std::string table_uuid;
  std::string column_uuid;
  std::string constraint_uuid;
  std::string domain_uuid;
  std::string value_type_uuid = "int64";
  std::string allocation_mode = "local_node_generator";
  std::int64_t start_value = 1;
  std::int64_t increment_by = 1;
  std::int64_t min_value = std::numeric_limits<std::int64_t>::min();
  std::int64_t max_value = std::numeric_limits<std::int64_t>::max();
  std::uint64_t cache_size = 32;
  bool cycle_allowed = false;
  bool gapless_required = false;
  bool transactional_allocation = false;
  bool reusable_if_no_effect = false;
  bool consumed_on_rollback = true;
  std::string policy_uuid;
  std::string policy_version_uuid;
  std::uint64_t policy_generation = 1;
  std::string reference_profile_uuid;
  std::string reference_family;
  std::string reference_mapping_label;
  std::string reference_allocation_timing;
  std::string reference_rollback_behavior;
  std::string reference_finality_behavior;
  std::string reference_cache_behavior;
  bool reference_mapping_complete = false;
  bool requires_cluster_authority = false;
  bool cluster_metric_path_requested = false;
};

struct EngineSequenceGeneratorRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::uint64_t metadata_epoch = 0;
  EngineSequenceGeneratorDefinition definition;
  std::string lifecycle_state = "active";
  bool dropped = false;
  std::int64_t durable_next_value = 1;
  bool durable_next_value_present = false;
  bool durable_exhausted = false;
  std::int64_t last_allocated_value = 0;
  bool last_allocated_value_present = false;
  std::uint64_t cache_window_generation = 0;
  bool cache_window_active = false;
  std::int64_t cache_window_first_value = 0;
  std::int64_t cache_window_last_value = 0;
  std::int64_t cache_next_value = 0;
  std::uint64_t cache_unused_on_recovery = 0;
  bool recovered_from_persisted_state = false;
  std::string recovery_snapshot_uuid;
  bool retained_by_mga_horizon = false;
  std::vector<std::int64_t> reusable_released_values;
};

struct EngineSequenceAllocationRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t sequence_epoch = 0;
  std::uint64_t cache_window_generation = 0;
  std::string allocation_uuid;
  std::string reservation_uuid;
  std::string generator_uuid;
  std::string table_uuid;
  std::string column_uuid;
  std::string statement_uuid;
  std::string record_uuid;
  std::string transaction_uuid;
  std::int64_t allocated_value = 0;
  std::string allocation_mode = "local_node_generator";
  std::string allocation_finality = "allocated_uncommitted";
  std::string lifecycle_state = "allocated";
  std::string transaction_outcome = "unknown";
  bool consumed_on_rollback = true;
  bool reusable_if_no_effect = false;
  bool external_exposure_allowed = true;
  bool row_effect_expected = true;
  bool row_effect_committed = false;
  bool released = false;
  bool unique_validation_required = true;
  std::string reference_mapping_label;
  std::int64_t cache_window_first_value = 0;
  std::int64_t cache_window_last_value = 0;
  std::int64_t durable_high_water_after = 0;
};

struct EngineIdentityValueBindingRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string identity_binding_uuid;
  std::string generator_uuid;
  std::string allocation_uuid;
  std::string table_uuid;
  std::string record_uuid;
  std::string identity_column_uuid;
  std::string identity_value_kind;
  std::string identity_value;
  std::string binding_finality = "allocated_uncommitted";
  std::string transaction_uuid;
  std::uint64_t local_transaction_id = 0;
};

struct EngineSequenceGeneratorMetrics {
  std::uint64_t policy_resolution_total = 0;
  std::uint64_t policy_resolution_reject_total = 0;
  std::uint64_t cache_windows_reserved_total = 0;
  std::uint64_t allocations_total = 0;
  std::uint64_t unique_validation_required_total = 0;
  std::uint64_t consumed_no_row_effect_total = 0;
  std::uint64_t rolled_back_consumed_total = 0;
  std::uint64_t reservations_released_total = 0;
  std::uint64_t recovery_snapshots_total = 0;
  std::uint64_t recovered_cache_gap_values_total = 0;
  std::uint64_t exhaustion_refusals_total = 0;
  std::uint64_t reference_mapping_reject_total = 0;
  std::uint64_t cluster_path_reject_total = 0;
  std::uint64_t mga_retention_blocked_total = 0;
  std::vector<std::string> local_metric_paths;
  std::vector<std::string> cluster_metric_paths;
};

struct EngineSequenceGeneratorLifecycleState {
  std::vector<EngineSequenceGeneratorRecord> generators;
  std::vector<EngineSequenceAllocationRecord> allocations;
  std::vector<EngineIdentityValueBindingRecord> identity_bindings;
  EngineSequenceGeneratorMetrics metrics;
  std::uint64_t metadata_epoch = 0;
  std::uint64_t max_event_sequence = 0;
  bool recovered_from_persisted_state = false;
  std::string recovery_snapshot_uuid;
};

struct EngineLoadSequenceGeneratorLifecycleStateResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineSequenceGeneratorLifecycleState state;
};

struct EngineSequenceCreateGeneratorRequest : EngineApiRequest {
  EngineSequenceGeneratorDefinition definition;
};
struct EngineSequenceCreateGeneratorResult : EngineApiResult {
  EngineSequenceGeneratorRecord generator;
  std::uint64_t metadata_cache_epoch = 0;
};
EngineSequenceCreateGeneratorResult EngineSequenceCreateGenerator(
    const EngineSequenceCreateGeneratorRequest& request);

struct EngineSequenceAlterGeneratorRequest : EngineApiRequest {
  EngineSequenceGeneratorDefinition definition;
  bool restart_with_value = false;
  std::int64_t restart_value = 0;
};
struct EngineSequenceAlterGeneratorResult : EngineApiResult {
  EngineSequenceGeneratorRecord generator;
  std::uint64_t metadata_cache_epoch = 0;
};
EngineSequenceAlterGeneratorResult EngineSequenceAlterGenerator(
    const EngineSequenceAlterGeneratorRequest& request);

struct EngineSequenceRestartGeneratorRequest : EngineApiRequest {
  std::string generator_uuid;
  std::int64_t restart_value = 1;
};
struct EngineSequenceRestartGeneratorResult : EngineApiResult {
  EngineSequenceGeneratorRecord generator;
  std::uint64_t metadata_cache_epoch = 0;
};
EngineSequenceRestartGeneratorResult EngineSequenceRestartGenerator(
    const EngineSequenceRestartGeneratorRequest& request);

struct EngineSequenceDropGeneratorRequest : EngineApiRequest {
  std::string generator_uuid;
};
struct EngineSequenceDropGeneratorResult : EngineApiResult {
  EngineSequenceGeneratorRecord generator;
  std::uint64_t metadata_cache_epoch = 0;
};
EngineSequenceDropGeneratorResult EngineSequenceDropGenerator(
    const EngineSequenceDropGeneratorRequest& request);

struct EngineSequenceAllocateValueRequest : EngineApiRequest {
  std::string generator_uuid;
  std::string statement_uuid;
  std::string record_uuid;
  bool row_effect_expected = true;
  bool external_exposure_allowed = true;
};
struct EngineSequenceAllocateValueResult : EngineApiResult {
  EngineSequenceAllocationRecord allocation;
  EngineSequenceGeneratorRecord generator;
  std::int64_t allocated_value = 0;
};
EngineSequenceAllocateValueResult EngineSequenceAllocateValue(
    const EngineSequenceAllocateValueRequest& request);

struct EngineSequenceApplyMgaTransactionOutcomeRequest : EngineApiRequest {
  std::uint64_t outcome_local_transaction_id = 0;
  std::string outcome_transaction_uuid;
  std::string mga_outcome;
  bool committed_row_effects = true;
  bool folded_to_no_effect = false;
  bool external_exposure_observed = true;
};
struct EngineSequenceApplyMgaTransactionOutcomeResult : EngineApiResult {
  std::vector<EngineSequenceAllocationRecord> updated_allocations;
};
EngineSequenceApplyMgaTransactionOutcomeResult EngineSequenceApplyMgaTransactionOutcome(
    const EngineSequenceApplyMgaTransactionOutcomeRequest& request);

struct EngineSequenceBindIdentityValueRequest : EngineApiRequest {
  std::string generator_uuid;
  std::string allocation_uuid;
  std::string table_uuid;
  std::string record_uuid;
  std::string identity_column_uuid;
  std::string identity_value_kind = "generator_allocated";
  std::string identity_value;
  bool attempted_second_uuid_for_row_identity = false;
};
struct EngineSequenceBindIdentityValueResult : EngineApiResult {
  EngineIdentityValueBindingRecord binding;
};
EngineSequenceBindIdentityValueResult EngineSequenceBindIdentityValue(
    const EngineSequenceBindIdentityValueRequest& request);

struct EngineSequenceRecoverGeneratorStateRequest : EngineApiRequest {};
struct EngineSequenceRecoverGeneratorStateResult : EngineApiResult {
  EngineSequenceGeneratorLifecycleState state;
  std::string recovery_snapshot_uuid;
};
EngineSequenceRecoverGeneratorStateResult EngineSequenceRecoverGeneratorState(
    const EngineSequenceRecoverGeneratorStateRequest& request);

struct EngineSequenceEvaluateMgaRetentionRequest : EngineApiRequest {
  std::uint64_t retention_visible_through_local_transaction_id = 0;
};
struct EngineSequenceEvaluateMgaRetentionResult : EngineApiResult {
  std::uint64_t blocked_allocation_count = 0;
};
EngineSequenceEvaluateMgaRetentionResult EngineSequenceEvaluateMgaRetention(
    const EngineSequenceEvaluateMgaRetentionRequest& request);

EngineLoadSequenceGeneratorLifecycleStateResult LoadSequenceGeneratorLifecycleState(
    const EngineRequestContext& context);

}  // namespace scratchbird::engine::internal_api
