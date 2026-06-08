// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ScratchBird contributors
//
// SB_ENGINE_INTERNAL_API_DML_IMPORT_RESUME_CHECKPOINT_BEHAVIOR
// SB_PID009_IMPORT_RESUME_CHECKPOINT

#include "dml/import_resume_checkpoint.hpp"

#include <initializer_list>
#include <string>

#include "api_diagnostics.hpp"

namespace scratchbird::engine::internal_api {
namespace {

bool OneOf(const std::string& value, std::initializer_list<const char*> allowed) {
  for (const char* item : allowed) {
    if (value == item) {
      return true;
    }
  }
  return false;
}

bool TargetPresent(const EngineObjectReference& target) {
  return !target.uuid.canonical.empty();
}

EngineNormalizeImportCheckpointResult CheckpointFailure(const std::string& detail) {
  EngineNormalizeImportCheckpointResult result;
  result.ok = false;
  result.operation_id = "dml.normalize_import_checkpoint_model";
  result.diagnostics.push_back(MakeInvalidRequestDiagnostic("dml.normalize_import_checkpoint_model", detail));
  return result;
}

bool AnyIntervalSet(const EngineImportCheckpointPolicyEnvelope& policy) {
  return policy.checkpoint_interval_rows > 0 ||
         policy.checkpoint_interval_bytes > 0 ||
         policy.checkpoint_interval_millis > 0;
}

}  // namespace

EngineNormalizeImportCheckpointResult EngineNormalizeImportCheckpointModel(
    const EngineNormalizeImportCheckpointRequest& request) {
  const auto& policy = request.checkpoint_policy;

  if (request.context.local_transaction_id == 0) {
    return CheckpointFailure("local_transaction_id_required");
  }
  if (!request.localized_names.empty()) {
    return CheckpointFailure("localized_names_not_allowed_engine_boundary");
  }
  if (request.target_table.uuid.canonical.empty()) {
    return CheckpointFailure("target_table_uuid_required");
  }
  if (!OneOf(policy.checkpoint_mode, {"disabled", "periodic_rows", "periodic_bytes", "periodic_time", "manual"})) {
    return CheckpointFailure("checkpoint_mode_unsupported:" + policy.checkpoint_mode);
  }
  if (!OneOf(policy.resume_policy, {"fail_closed", "resume_from_checkpoint", "operator_review_required"})) {
    return CheckpointFailure("resume_policy_unsupported:" + policy.resume_policy);
  }
  if (!OneOf(policy.replay_policy, {"require_idempotent_replay", "reject_replay", "operator_review_required"})) {
    return CheckpointFailure("replay_policy_unsupported:" + policy.replay_policy);
  }
  if (!OneOf(policy.failure_action, {"abort_import", "quarantine_checkpoint", "operator_review_required"})) {
    return CheckpointFailure("failure_action_unsupported:" + policy.failure_action);
  }

  const bool target_present = TargetPresent(policy.checkpoint_target);
  if (policy.checkpoint_mode == "disabled") {
    if (AnyIntervalSet(policy) || target_present || policy.resume_policy != "fail_closed") {
      return CheckpointFailure("disabled_checkpoint_requires_zero_intervals_no_target_fail_closed_resume");
    }
  } else {
    if (!target_present) {
      return CheckpointFailure("checkpoint_target_uuid_required");
    }
    if (policy.resume_policy == "fail_closed") {
      return CheckpointFailure("checkpoint_enabled_requires_resume_or_operator_review_policy");
    }
    if (policy.replay_policy != "require_idempotent_replay" && policy.resume_policy == "resume_from_checkpoint") {
      return CheckpointFailure("resume_from_checkpoint_requires_idempotent_replay");
    }
    if (policy.require_source_fingerprint && request.source_fingerprint.empty()) {
      return CheckpointFailure("source_fingerprint_required");
    }
    if (policy.require_source_position && request.source_position.empty()) {
      return CheckpointFailure("source_position_required");
    }
    if (policy.checkpoint_mode == "periodic_rows" && policy.checkpoint_interval_rows == 0) {
      return CheckpointFailure("periodic_rows_interval_required");
    }
    if (policy.checkpoint_mode == "periodic_bytes" && policy.checkpoint_interval_bytes == 0) {
      return CheckpointFailure("periodic_bytes_interval_required");
    }
    if (policy.checkpoint_mode == "periodic_time" && policy.checkpoint_interval_millis == 0) {
      return CheckpointFailure("periodic_time_interval_required");
    }
  }

  EngineNormalizeImportCheckpointResult result;
  result.ok = true;
  result.operation_id = "dml.normalize_import_checkpoint_model";
  result.normalized_checkpoint_mode = policy.checkpoint_mode;
  result.normalized_resume_policy = policy.resume_policy;
  result.normalized_replay_policy = policy.replay_policy;
  result.normalized_failure_action = policy.failure_action;
  result.checkpoint_required = policy.checkpoint_mode != "disabled";
  result.resume_supported = policy.resume_policy == "resume_from_checkpoint";
  result.idempotent_replay_required = policy.replay_policy == "require_idempotent_replay";
  result.source_fingerprint_required = policy.require_source_fingerprint;
  result.source_position_required = policy.require_source_position;
  result.effective_checkpoint_interval_rows = policy.checkpoint_interval_rows;
  result.effective_checkpoint_interval_bytes = policy.checkpoint_interval_bytes;
  result.effective_checkpoint_interval_millis = policy.checkpoint_interval_millis;
  result.checkpoint_target = policy.checkpoint_target;
  result.primary_object = request.target_table;
  result.evidence.push_back({"import_checkpoint_model", policy.checkpoint_mode});
  result.evidence.push_back({"import_resume_policy", policy.resume_policy});
  result.evidence.push_back({"import_replay_policy", policy.replay_policy});
  result.evidence.push_back({"import_failure_action", policy.failure_action});
  result.evidence.push_back({"target_object_uuid", request.target_table.uuid.canonical});
  if (target_present) {
    result.evidence.push_back({"checkpoint_target_uuid", policy.checkpoint_target.uuid.canonical});
  }
  if (!request.source_fingerprint.empty()) {
    result.evidence.push_back({"source_fingerprint", request.source_fingerprint});
  }
  if (!request.source_position.empty()) {
    result.evidence.push_back({"source_position", request.source_position});
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
