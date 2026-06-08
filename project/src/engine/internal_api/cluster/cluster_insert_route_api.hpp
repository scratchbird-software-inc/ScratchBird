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
// SB_ENGINE_INTERNAL_API_CLUSTER_INSERT_ROUTE_FENCE
// SB_PID012_CLUSTER_INSERT_ROUTE_FENCE

#pragma once

#include <string>

#include "api_types.hpp"

namespace scratchbird::engine::internal_api {

struct EngineClusterInsertRouteFenceRequest : public EngineApiRequest {
  EngineObjectReference target_table;
  EngineObjectReference target_shard;
  EngineObjectReference target_range;
  EngineUuid owner_node_uuid;
  EngineUuid participant_node_uuid;
  EngineUuid route_epoch_uuid;
  EngineUuid participant_uuid;
  EngineUuid policy_snapshot_uuid;
  EngineUuid finality_service_uuid;
  EngineApiU64 route_epoch = 0;
  EngineApiU64 route_generation = 0;
  std::string idempotency_key;
  std::string handoff_proof_ref;
  bool unresolved_prepared_or_limbo_work = false;
  bool stale_owner_observed = false;
  bool remote_participant_requested = false;
};

struct EngineClusterInsertRouteFenceResult : public EngineApiResult {
  bool route_fence_checked = false;
  bool route_activation_allowed = false;
  bool remote_participant_allowed = false;
  bool fail_closed = true;
  bool finality_required = true;
  bool handoff_proof_required = false;
  bool stale_owner_refused = false;
  EngineApiU64 route_epoch = 0;
  EngineApiU64 route_generation = 0;
  std::string refusal_reason;
};

EngineClusterInsertRouteFenceResult EngineValidateClusterInsertRouteFence(
    const EngineClusterInsertRouteFenceRequest& request);

}  // namespace scratchbird::engine::internal_api
