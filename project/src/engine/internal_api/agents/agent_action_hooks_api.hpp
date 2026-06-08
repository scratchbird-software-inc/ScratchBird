// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <string>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_PID017_AGENT_ACTION_HOOKS
// Engine-owned agent action hooks. Agents request work here; page/filespace/index
// mutation remains owned by the engine and storage subsystems.

struct EngineAgentActionHookRequest : EngineApiRequest {
  std::string agent_type;
  std::string action_class;
  EngineUuid agent_uuid;
  EngineUuid policy_snapshot_uuid;
  EngineObjectReference target_filespace;
  EngineObjectReference target_index;
  std::string page_family;
  std::string page_type;
  std::string safety_fence_result;
  std::string cooldown_key;
  EngineApiU64 requested_pages = 0;
  EngineApiU64 requested_bytes = 0;
  bool policy_authorized = false;
  bool evidence_sink_available = false;
  bool metrics_fresh = false;
  bool cooldown_active = false;
  bool manual_override_active = false;
  bool lifecycle_fence_active = false;
  bool dry_run = false;
  bool shadow_build = false;
};

struct EngineAgentActionHookResult : EngineApiResult {
  bool action_accepted = false;
  bool action_deferred = false;
  bool dry_run = false;
  std::string refusal_reason;
  std::string normalized_action;
};

struct EngineRequestPagePreallocationRequest : EngineAgentActionHookRequest {};
struct EngineRequestPagePreallocationResult : EngineAgentActionHookResult {};
EngineRequestPagePreallocationResult EngineRequestPagePreallocation(const EngineRequestPagePreallocationRequest& request);

struct EngineRequestPageRelocationRequest : EngineAgentActionHookRequest {};
struct EngineRequestPageRelocationResult : EngineAgentActionHookResult {};
EngineRequestPageRelocationResult EngineRequestPageRelocation(const EngineRequestPageRelocationRequest& request);

struct EngineRequestFilespaceGrowthRequest : EngineAgentActionHookRequest {};
struct EngineRequestFilespaceGrowthResult : EngineAgentActionHookResult {};
EngineRequestFilespaceGrowthResult EngineRequestFilespaceGrowth(const EngineRequestFilespaceGrowthRequest& request);

struct EngineNotifyFilespaceShrinkReadinessRequest : EngineAgentActionHookRequest {};
struct EngineNotifyFilespaceShrinkReadinessResult : EngineAgentActionHookResult {};
EngineNotifyFilespaceShrinkReadinessResult EngineNotifyFilespaceShrinkReadiness(
    const EngineNotifyFilespaceShrinkReadinessRequest& request);

struct EngineRequestIndexDeltaMergeRequest : EngineAgentActionHookRequest {};
struct EngineRequestIndexDeltaMergeResult : EngineAgentActionHookResult {};
EngineRequestIndexDeltaMergeResult EngineRequestIndexDeltaMerge(const EngineRequestIndexDeltaMergeRequest& request);

struct EngineRequestIndexRebuildOrShadowBuildRequest : EngineAgentActionHookRequest {};
struct EngineRequestIndexRebuildOrShadowBuildResult : EngineAgentActionHookResult {};
EngineRequestIndexRebuildOrShadowBuildResult EngineRequestIndexRebuildOrShadowBuild(
    const EngineRequestIndexRebuildOrShadowBuildRequest& request);

}  // namespace scratchbird::engine::internal_api
