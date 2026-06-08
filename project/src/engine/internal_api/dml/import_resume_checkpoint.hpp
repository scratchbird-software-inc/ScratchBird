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
// SB_ENGINE_INTERNAL_API_DML_IMPORT_RESUME_CHECKPOINT
// SB_PID009_IMPORT_RESUME_CHECKPOINT

#pragma once

#include <string>
#include <vector>

#include "api_types.hpp"

namespace scratchbird::engine::internal_api {

struct EngineImportCheckpointPolicyEnvelope {
  std::string checkpoint_mode = "disabled";
  EngineApiU64 checkpoint_interval_rows = 0;
  EngineApiU64 checkpoint_interval_bytes = 0;
  EngineApiU64 checkpoint_interval_millis = 0;
  EngineObjectReference checkpoint_target;
  std::string resume_policy = "fail_closed";
  std::string replay_policy = "require_idempotent_replay";
  std::string failure_action = "abort_import";
  bool require_source_fingerprint = true;
  bool require_source_position = true;
};

struct EngineImportCheckpointStateEnvelope {
  EngineUuid checkpoint_uuid;
  EngineObjectReference target_table;
  std::string source_fingerprint;
  std::string source_position;
  EngineApiU64 accepted_rows = 0;
  EngineApiU64 rejected_rows = 0;
  EngineApiU64 committed_batches = 0;
  EngineApiU64 last_local_transaction_id = 0;
  bool final_checkpoint = false;
};

struct EngineNormalizeImportCheckpointRequest : public EngineApiRequest {
  EngineObjectReference target_table;
  EngineImportCheckpointPolicyEnvelope checkpoint_policy;
  std::string source_fingerprint;
  std::string source_position;
};

struct EngineNormalizeImportCheckpointResult : public EngineApiResult {
  std::string normalized_checkpoint_mode;
  std::string normalized_resume_policy;
  std::string normalized_replay_policy;
  std::string normalized_failure_action;
  bool checkpoint_required = false;
  bool resume_supported = false;
  bool idempotent_replay_required = true;
  bool source_fingerprint_required = true;
  bool source_position_required = true;
  EngineApiU64 effective_checkpoint_interval_rows = 0;
  EngineApiU64 effective_checkpoint_interval_bytes = 0;
  EngineApiU64 effective_checkpoint_interval_millis = 0;
  EngineObjectReference checkpoint_target;
};

EngineNormalizeImportCheckpointResult EngineNormalizeImportCheckpointModel(
    const EngineNormalizeImportCheckpointRequest& request);

}  // namespace scratchbird::engine::internal_api
