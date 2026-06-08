// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: AEIC_LOCAL_OPERATIONAL_WORKFLOW_STORE

#include "agents/agent_durable_catalog_store_api.hpp"
#include "agent_local_workflow.hpp"

namespace scratchbird::engine::internal_api {

struct AgentLocalWorkflowStoreRequest {
  EngineRequestContext context;
  scratchbird::core::agents::AgentLocalWorkflowRequest workflow;
  bool production_live_path = true;
  bool fsync_or_checkpoint_evidence = false;
};

struct AgentLocalWorkflowStoreResult {
  scratchbird::core::agents::AgentLocalWorkflowApplyResult workflow;
  AgentDurableCatalogStoreResult loaded_catalog;
  AgentDurableCatalogStoreResult persisted_catalog;
  bool loaded_from_store = false;
  bool persisted_to_store = false;
};

AgentLocalWorkflowStoreResult ApplyAgentLocalWorkflowWithDurableCatalogStore(
    const AgentLocalWorkflowStoreRequest& request);

}  // namespace scratchbird::engine::internal_api
