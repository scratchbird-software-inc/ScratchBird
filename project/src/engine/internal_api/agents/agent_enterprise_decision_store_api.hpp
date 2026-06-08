// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: AEIC_ENTERPRISE_DECISION_EVIDENCE_STORE

#include "agents/agent_durable_catalog_store_api.hpp"
#include "agent_enterprise_evidence.hpp"

namespace scratchbird::engine::internal_api {

struct AgentEnterpriseDecisionStoreRequest {
  EngineRequestContext context;
  scratchbird::core::agents::AgentEnterpriseDecisionEvidenceRequest decision;
  bool production_live_path = true;
  bool fsync_or_checkpoint_evidence = false;
  bool durable_resource_reservation_required = true;
  scratchbird::core::agents::u64 resource_reservation_memory_bytes = 2048;
  scratchbird::core::agents::u64 resource_reservation_worker_slots = 1;
  scratchbird::core::agents::u64 resource_reservation_overhead_microseconds =
      500;
  scratchbird::core::agents::u64 resource_reservation_max_active = 1024;
  scratchbird::core::agents::u64 resource_reservation_max_memory_bytes =
      64 * 1024 * 1024;
  scratchbird::core::agents::u64 resource_reservation_max_worker_slots = 8;
  scratchbird::core::agents::u64 resource_reservation_max_overhead_microseconds =
      10 * 1000 * 1000;
};

struct AgentEnterpriseDecisionStoreResult {
  scratchbird::core::agents::AgentEnterpriseDecisionEvidenceResult decision;
  AgentDurableCatalogStoreResult loaded_catalog;
  AgentDurableCatalogStoreResult persisted_catalog;
  bool loaded_from_store = false;
  bool resource_reservation_acquired = false;
  bool resource_reservation_released = false;
  std::string resource_reservation_uuid;
  bool persisted_to_store = false;
};

AgentEnterpriseDecisionStoreResult AppendEnterpriseAgentDecisionEvidenceToStore(
    const AgentEnterpriseDecisionStoreRequest& request);

}  // namespace scratchbird::engine::internal_api
