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
// SB_ENGINE_INTERNAL_API_REMOTE_PARTICIPANT_INSERT_API
// SB_PID013_REMOTE_PARTICIPANT_INSERT_API

#pragma once

#include <string>
#include <vector>

#include "api_types.hpp"

namespace scratchbird::engine::internal_api {

struct EngineRemoteParticipantInsertRequest : public EngineApiRequest {
  EngineUuid remote_request_uuid;
  EngineObjectReference target_database;
  EngineObjectReference target_table;
  EngineObjectReference target_shard;
  EngineObjectReference target_range;
  EngineUuid owner_node_uuid;
  EngineUuid participant_node_uuid;
  EngineUuid route_epoch_uuid;
  EngineUuid participant_uuid;
  EngineUuid policy_snapshot_uuid;
  EngineUuid finality_service_uuid;
  EngineUuid checkpoint_uuid;
  EngineApiU64 route_epoch = 0;
  EngineApiU64 route_generation = 0;
  EngineApiU64 timeout_millis = 0;
  std::string idempotency_key;
  std::string retry_policy = "fail_closed";
  std::string failure_model = "participant_refusal";
  std::string insert_mode = "copy_import";
  std::string source_fingerprint;
  std::string source_position;
  bool strict_bulk_load_requested = false;
  bool import_execution_requested = false;
  bool participant_admission_durable = false;
  bool route_fence_validated = false;
  bool finality_proof_available = false;
  std::vector<EngineRowValue> canonical_rows;
};

struct EngineRemoteParticipantInsertResult : public EngineApiResult {
  bool participant_envelope_validated = false;
  bool participant_admitted = false;
  bool local_insert_allowed = false;
  bool remote_execution_allowed = false;
  bool finality_required = true;
  bool fail_closed = true;
  bool retryable = false;
  bool checkpoint_replay_required = false;
  EngineApiU64 canonical_row_count = 0;
  EngineApiU64 route_epoch = 0;
  EngineApiU64 route_generation = 0;
  std::string normalized_retry_policy;
  std::string normalized_failure_model;
  std::string normalized_insert_mode;
  std::string refusal_reason;
};

EngineRemoteParticipantInsertResult EnginePrepareRemoteParticipantInsert(
    const EngineRemoteParticipantInsertRequest& request);

}  // namespace scratchbird::engine::internal_api
