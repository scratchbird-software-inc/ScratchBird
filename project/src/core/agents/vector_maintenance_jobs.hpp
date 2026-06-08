// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: ORH_VECTOR_MAINTENANCE_JOB_INTEGRATION
// Vector maintenance jobs are control-plane lifecycle records only. They do
// not grant ANN visibility/finality authority; exact fallback/rerank remains
// final scoring authority where policy requires it.

#include "agent_runtime.hpp"
#include "online_maintenance_progress.hpp"
#include "vector_training_recall_lifecycle.hpp"

#include <optional>
#include <string>
#include <vector>

namespace scratchbird::core::agents {

inline constexpr const char* kVectorMaintenanceJobSearchKey =
    "ORH_VECTOR_MAINTENANCE_JOB_INTEGRATION";
inline constexpr const char* kVectorLatencyDeletionDriftSearchKey =
    "ORH_VECTOR_LATENCY_DELETION_DRIFT_TRIGGERS";
inline constexpr const char* kVectorIndexRuntimeCorrectnessBlocker =
    "SB_ORH_VECTOR_INDEX_RUNTIME_CORRECTNESS_UNPROVEN";

enum class VectorMaintenanceActionKind : u32 {
  unknown = 0,
  adaptive_tuning = 1,
  retrain = 2,
  rebuild = 3
};

enum class VectorMaintenancePublishState : u32 {
  not_ready = 0,
  waiting_validation = 1,
  publish_after_validation = 2,
  published = 3,
  refused = 4
};

enum class VectorMaintenanceFailureClass : u32 {
  none = 0,
  transient_provider_unavailable = 1,
  permanent_validation_failed = 2,
  runtime_correctness_unproven = 3,
  authority_boundary_refused = 4
};

enum class VectorMaintenanceRetryState : u32 {
  none = 0,
  retry_scheduled = 1,
  retry_exhausted = 2
};

struct VectorMaintenanceJobRequest {
  std::string job_uuid;
  std::string database_uuid;
  std::string target_collection_uuid;
  std::string target_index_uuid;
  u64 provider_generation = 0;
  u64 old_training_generation = 0;
  u64 new_training_generation = 0;
  VectorMaintenanceActionKind action_kind =
      VectorMaintenanceActionKind::unknown;
  scratchbird::core::index::VectorTrainingRecallLifecycleDecision
      lifecycle_decision;
  u64 total_units = 1;
  u64 now_microseconds = 0;
  u64 max_retry_attempts = 3;
  bool engine_mga_authoritative = true;
  bool durable_checkpoint_persisted = true;
  bool support_bundle_sink_available = true;
  bool observability_sink_available = true;
  bool exact_fallback_available = true;
  bool exact_rerank_final_scoring_authority = true;
  bool ann_visibility_authority = false;
  bool ann_finality_authority = false;
};

struct VectorMaintenanceJobRecord {
  std::string job_uuid;
  std::string database_uuid;
  std::string target_collection_uuid;
  std::string target_index_uuid;
  u64 provider_generation = 0;
  u64 old_training_generation = 0;
  u64 new_training_generation = 0;
  VectorMaintenanceActionKind action_kind =
      VectorMaintenanceActionKind::unknown;
  OnlineMaintenanceRecord progress;
  VectorMaintenancePublishState publish_state =
      VectorMaintenancePublishState::not_ready;
  VectorMaintenanceFailureClass failure_class =
      VectorMaintenanceFailureClass::none;
  VectorMaintenanceRetryState retry_state = VectorMaintenanceRetryState::none;
  u64 retry_attempts = 0;
  u64 max_retry_attempts = 0;
  bool validation_successful = false;
  bool exact_fallback_available = true;
  bool exact_rerank_final_scoring_authority = true;
  bool ann_visibility_authority = false;
  bool ann_finality_authority = false;
  std::string runtime_correctness_blocker =
      kVectorIndexRuntimeCorrectnessBlocker;
  std::vector<std::string> lifecycle_evidence;
  std::vector<std::string> evidence;
};

struct VectorMaintenanceJobResult {
  AgentRuntimeStatus status;
  VectorMaintenanceJobRecord record;
  bool fail_closed = true;

  bool ok() const { return status.ok && !fail_closed; }
};

class VectorMaintenanceJobStore {
 public:
  AgentRuntimeStatus Upsert(VectorMaintenanceJobRecord record);
  std::optional<VectorMaintenanceJobRecord> Find(
      const std::string& job_uuid) const;
  VectorMaintenanceJobRecord* MutableFind(const std::string& job_uuid);

  OnlineMaintenanceStateStore& progress_store() { return progress_store_; }
  const OnlineMaintenanceStateStore& progress_store() const {
    return progress_store_;
  }
  const std::vector<VectorMaintenanceJobRecord>& records() const {
    return records_;
  }

 private:
  std::vector<VectorMaintenanceJobRecord> records_;
  OnlineMaintenanceStateStore progress_store_;
};

const char* VectorMaintenanceActionKindName(
    VectorMaintenanceActionKind action);
const char* VectorMaintenancePublishStateName(
    VectorMaintenancePublishState state);
const char* VectorMaintenanceFailureClassName(
    VectorMaintenanceFailureClass failure_class);
const char* VectorMaintenanceRetryStateName(
    VectorMaintenanceRetryState state);

VectorMaintenanceActionKind VectorMaintenanceActionFromLifecycle(
    scratchbird::core::index::VectorTrainingRecallLifecycleAction action);

VectorMaintenanceJobResult CreateVectorMaintenanceJob(
    VectorMaintenanceJobStore* store,
    const VectorMaintenanceJobRequest& request);
VectorMaintenanceJobResult RecordVectorMaintenanceProgress(
    VectorMaintenanceJobStore* store,
    const std::string& job_uuid,
    u64 completed_units,
    u64 total_units,
    std::string stage,
    u64 now_microseconds);
VectorMaintenanceJobResult CancelVectorMaintenanceJob(
    VectorMaintenanceJobStore* store,
    const std::string& job_uuid,
    std::string reason,
    u64 now_microseconds);
VectorMaintenanceJobResult ResumeVectorMaintenanceJob(
    VectorMaintenanceJobStore* store,
    const std::string& job_uuid,
    u64 now_microseconds);
VectorMaintenanceJobResult MarkVectorMaintenanceValidationReady(
    VectorMaintenanceJobStore* store,
    const std::string& job_uuid,
    u64 now_microseconds);
VectorMaintenanceJobResult PublishVectorMaintenanceAfterValidation(
    VectorMaintenanceJobStore* store,
    const std::string& job_uuid,
    u64 now_microseconds);
VectorMaintenanceJobResult ClassifyVectorMaintenanceFailure(
    VectorMaintenanceJobStore* store,
    const std::string& job_uuid,
    VectorMaintenanceFailureClass failure_class,
    bool transient_retryable,
    u64 now_microseconds);

}  // namespace scratchbird::core::agents
