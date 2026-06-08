// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CLUSTER_REPLICATION_API
struct EngineInspectReplicationRequest : EngineApiRequest {};
struct EngineInspectReplicationResult : EngineApiResult {
  bool standalone_fail_closed = false;
  bool replication_boundary_present = false;
};

struct EngineReplicationBoundaryRequest : EngineApiRequest {
  std::string boundary_kind;
  EngineObjectReference publication;
  EngineObjectReference subscription;
  EngineObjectReference slot;
  EngineApiU64 route_epoch = 0;
  EngineApiU64 route_generation = 0;
  EngineApiU64 retention_horizon_local_transaction_id = 0;
  bool engine_authoritative = false;
  bool security_authorized = false;
  bool event_channel_authorized = false;
  bool backup_archive_hold_satisfied = false;
  bool capability_profile_allows = false;
  bool live_ingest_requested = false;
  std::string policy_snapshot_uuid;
  std::string idempotency_key;
};

struct EngineReplicationBoundaryResult : EngineApiResult {
  bool boundary_checked = false;
  bool standalone_fail_closed = false;
  bool security_checked = false;
  bool retention_checked = false;
  bool publication_checked = false;
  bool subscription_checked = false;
  bool slot_checked = false;
  bool changefeed_checked = false;
  bool live_ingest_checked = false;
  bool route_activation_allowed = false;
  bool fail_closed = false;
  std::string refusal_reason;
  EngineApiU64 route_epoch = 0;
  EngineApiU64 route_generation = 0;
};

EngineInspectReplicationResult EngineInspectReplication(const EngineInspectReplicationRequest& request);
EngineReplicationBoundaryResult EngineEvaluateReplicationBoundary(
    const EngineReplicationBoundaryRequest& request);

}  // namespace scratchbird::engine::internal_api
