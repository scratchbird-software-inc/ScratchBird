// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: ODF_121_ONLINE_MAINTENANCE_PROGRESS_CANCEL_RESUME
// Engine-owned online maintenance progress, cancellation, resume, and
// crash-safe publication control. This layer records operation lifecycle and
// checkpoint state only; it is not transaction finality, visibility, parser,
// reference, catalog, or recovery authority.

#include "agent_runtime.hpp"
#include "agent_workload_resource_quota.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::core::agents {

using scratchbird::core::platform::u32;

inline constexpr const char* kOnlineMaintenanceStarted =
    "ONLINE_MAINTENANCE.STARTED";
inline constexpr const char* kOnlineMaintenanceProgressRecorded =
    "ONLINE_MAINTENANCE.PROGRESS_RECORDED";
inline constexpr const char* kOnlineMaintenanceCancelCheckpointed =
    "ONLINE_MAINTENANCE.CANCEL_CHECKPOINTED";
inline constexpr const char* kOnlineMaintenanceResumedFromCheckpoint =
    "ONLINE_MAINTENANCE.RESUMED_FROM_CHECKPOINT";
inline constexpr const char* kOnlineMaintenanceRecoveredResumable =
    "ONLINE_MAINTENANCE.RECOVERED_RESUMABLE";
inline constexpr const char* kOnlineMaintenanceCompletedUnpublished =
    "ONLINE_MAINTENANCE.COMPLETED_UNPUBLISHED";
inline constexpr const char* kOnlineMaintenancePublishSuccess =
    "ONLINE_MAINTENANCE.PUBLISH_SUCCESS";
inline constexpr const char* kOnlineMaintenanceUnsafePublishRefused =
    "ONLINE_MAINTENANCE.UNSAFE_PUBLISH_REFUSED";
inline constexpr const char* kOnlineMaintenanceUnsafeRestartRefused =
    "ONLINE_MAINTENANCE.RESTART_STATE_UNSAFE_REFUSED";
inline constexpr const char* kOnlineMaintenanceUnsafeResumeRefused =
    "ONLINE_MAINTENANCE.RESUME_CHECKPOINT_UNSAFE_REFUSED";
inline constexpr const char* kOnlineMaintenanceResourceDenied =
    "ONLINE_MAINTENANCE.RESOURCE_ADMISSION_DENIED";

enum class OnlineMaintenanceOperationKind : u32 {
  unknown = 0,
  sorted_index_build = 1,
  index_rebuild = 2,
  nosql_compaction = 3,
  optimizer_stats_refresh = 4,
  optimized_backfill = 5,
  generation_publish = 6,
  storage_cleanup = 7,
  vector_adaptive_tuning = 8,
  vector_retrain = 9,
  vector_rebuild = 10
};

enum class OnlineMaintenancePhase : u32 {
  requested = 1,
  running = 2,
  cancel_requested = 3,
  cancelled = 4,
  resumable = 5,
  publish_ready = 6,
  published = 7,
  completed = 8,
  failed_closed = 9
};

enum class OnlineMaintenanceDecision : u32 {
  admitted = 1,
  progress_recorded = 2,
  cancel_accepted = 3,
  resumed = 4,
  recovered_resumable = 5,
  completed = 6,
  published = 7,
  refused = 8
};

struct OnlineMaintenanceEvidenceField {
  std::string key;
  std::string value;
};

struct OnlineMaintenanceProgressSnapshot {
  std::string operation_uuid;
  std::string database_uuid;
  std::string target_uuid;
  OnlineMaintenanceOperationKind kind = OnlineMaintenanceOperationKind::unknown;
  OnlineMaintenancePhase phase = OnlineMaintenancePhase::requested;
  std::string stage;
  std::string work_unit_label = "work_units";
  u64 completed_units = 0;
  u64 total_units = 0;
  u64 percent_basis_points = 0;
  u64 checkpoint_generation = 0;
  bool durable_checkpoint_persisted = false;
  bool engine_mga_authoritative = false;
  bool cancelable = false;
  bool cancel_requested = false;
  bool resumable = false;
  bool publish_ready = false;
  bool published_visible = false;
  bool partial_publish_visible = false;
  bool support_bundle_evidence_present = false;
  bool observability_evidence_present = false;
  std::string restart_token;
  std::string resource_reservation_token;
  std::string diagnostic_code;
  std::vector<OnlineMaintenanceEvidenceField> evidence;
};

struct OnlineMaintenanceRecord {
  OnlineMaintenanceProgressSnapshot snapshot;
  std::string checkpoint_payload;
  u64 started_at_microseconds = 0;
  u64 updated_at_microseconds = 0;
  bool publication_fence_persisted = false;
  bool authoritative_generation_validated = false;
};

struct OnlineMaintenanceStartRequest {
  OnlineMaintenanceOperationKind kind = OnlineMaintenanceOperationKind::unknown;
  std::string operation_uuid;
  std::string database_uuid;
  std::string target_uuid;
  std::string stage = "admitted";
  std::string work_unit_label = "work_units";
  u64 total_units = 0;
  u64 now_microseconds = 0;
  bool engine_mga_authoritative = false;
  bool durable_checkpoint_persisted = false;
  bool support_bundle_sink_available = false;
  bool observability_sink_available = false;
  bool cancelable = true;
  bool resumable = true;
  bool require_resource_admission = false;
  WorkloadResourceQuotaController* quota_controller = nullptr;
  WorkloadAdmissionRequest resource_request;
};

struct OnlineMaintenanceProgressRequest {
  std::string operation_uuid;
  std::string stage;
  u64 completed_units = 0;
  u64 total_units = 0;
  u64 now_microseconds = 0;
  bool durable_checkpoint_persisted = false;
  std::string checkpoint_payload;
};

struct OnlineMaintenanceCancelRequest {
  std::string operation_uuid;
  std::string reason = "operator_cancel";
  u64 now_microseconds = 0;
  bool durable_checkpoint_persisted = false;
  std::string checkpoint_payload;
  bool release_resource_reservation = true;
  WorkloadResourceQuotaController* quota_controller = nullptr;
};

struct OnlineMaintenanceResumeRequest {
  std::string operation_uuid;
  u64 now_microseconds = 0;
  bool engine_mga_authoritative = false;
  bool durable_checkpoint_persisted = false;
  bool support_bundle_sink_available = false;
  bool observability_sink_available = false;
  bool require_resource_admission = false;
  WorkloadResourceQuotaController* quota_controller = nullptr;
  WorkloadAdmissionRequest resource_request;
};

struct OnlineMaintenancePublishRequest {
  std::string operation_uuid;
  u64 now_microseconds = 0;
  bool engine_mga_authoritative = false;
  bool durable_publication_fence_persisted = false;
  bool authoritative_generation_validated = false;
  bool no_partial_visibility = false;
  bool support_bundle_sink_available = false;
  bool observability_sink_available = false;
  bool release_resource_reservation = true;
  WorkloadResourceQuotaController* quota_controller = nullptr;
};

struct OnlineMaintenanceCompleteRequest {
  std::string operation_uuid;
  u64 now_microseconds = 0;
  bool durable_checkpoint_persisted = false;
  bool support_bundle_sink_available = false;
  bool observability_sink_available = false;
  bool release_resource_reservation = true;
  WorkloadResourceQuotaController* quota_controller = nullptr;
};

struct OnlineMaintenanceRecoveryRequest {
  OnlineMaintenanceRecord checkpoint;
  u64 now_microseconds = 0;
  bool engine_mga_authoritative = false;
  bool support_bundle_sink_available = false;
  bool observability_sink_available = false;
};

struct OnlineMaintenanceResult {
  AgentRuntimeStatus status;
  OnlineMaintenanceDecision decision = OnlineMaintenanceDecision::refused;
  OnlineMaintenanceRecord record;
  OnlineMaintenanceProgressSnapshot snapshot;
  bool fail_closed = true;

  bool ok() const { return status.ok && !fail_closed; }
};

struct OnlineMaintenanceCheckpointFileResult {
  AgentRuntimeStatus status;
  OnlineMaintenanceRecord record;
  bool record_present = false;

  bool ok() const { return status.ok && record_present; }
};

class OnlineMaintenanceStateStore {
 public:
  AgentRuntimeStatus Upsert(OnlineMaintenanceRecord record);
  std::optional<OnlineMaintenanceRecord> Find(
      const std::string& operation_uuid) const;
  OnlineMaintenanceRecord* MutableFind(const std::string& operation_uuid);

  const std::vector<OnlineMaintenanceRecord>& records() const {
    return records_;
  }
  const std::vector<OnlineMaintenanceEvidenceField>& evidence_log() const {
    return evidence_log_;
  }
  void AddEvidence(std::string key, std::string value);

 private:
  std::vector<OnlineMaintenanceRecord> records_;
  std::vector<OnlineMaintenanceEvidenceField> evidence_log_;
};

const char* OnlineMaintenanceOperationKindName(
    OnlineMaintenanceOperationKind kind);
const char* OnlineMaintenancePhaseName(OnlineMaintenancePhase phase);
const char* OnlineMaintenanceDecisionName(OnlineMaintenanceDecision decision);

OnlineMaintenanceResult StartOnlineMaintenanceOperation(
    OnlineMaintenanceStateStore* store,
    const OnlineMaintenanceStartRequest& request);
OnlineMaintenanceResult RecordOnlineMaintenanceProgress(
    OnlineMaintenanceStateStore* store,
    const OnlineMaintenanceProgressRequest& request);
OnlineMaintenanceResult CancelOnlineMaintenanceOperation(
    OnlineMaintenanceStateStore* store,
    const OnlineMaintenanceCancelRequest& request);
OnlineMaintenanceResult ResumeOnlineMaintenanceOperation(
    OnlineMaintenanceStateStore* store,
    const OnlineMaintenanceResumeRequest& request);
OnlineMaintenanceResult CompleteOnlineMaintenanceOperation(
    OnlineMaintenanceStateStore* store,
    const OnlineMaintenanceCompleteRequest& request);
OnlineMaintenanceResult PublishOnlineMaintenanceOperation(
    OnlineMaintenanceStateStore* store,
    const OnlineMaintenancePublishRequest& request);
OnlineMaintenanceResult RecoverOnlineMaintenanceOperation(
    OnlineMaintenanceStateStore* store,
    const OnlineMaintenanceRecoveryRequest& request);

std::string SerializeOnlineMaintenanceCheckpoint(
    const OnlineMaintenanceRecord& record);
std::optional<OnlineMaintenanceRecord> ParseOnlineMaintenanceCheckpoint(
    const std::string& text);
AgentRuntimeStatus PersistOnlineMaintenanceCheckpointFile(
    const std::string& path,
    const OnlineMaintenanceRecord& record);
OnlineMaintenanceCheckpointFileResult LoadOnlineMaintenanceCheckpointFile(
    const std::string& path);
std::string SerializeOnlineMaintenanceSnapshot(
    const OnlineMaintenanceProgressSnapshot& snapshot);

}  // namespace scratchbird::core::agents
