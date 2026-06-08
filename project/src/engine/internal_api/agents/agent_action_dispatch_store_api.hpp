// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: AEIC_REAL_ACTUATOR_PROVIDER_FRAMEWORK
// SEARCH_KEY: AEIC_DURABLE_AGENT_ACTION_DISPATCH_STORE

#include "agents/agent_durable_catalog_store_api.hpp"
#include "agent_action_dispatch.hpp"
#include "agent_production_classification.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

struct EngineOwnedAgentActuatorRegistryBuildResult;

// Engine-owned wrapper around actuator providers. Production store dispatch
// accepts only this sealed handle so a caller cannot make a raw registry live by
// setting provenance booleans on the request.
class EngineOwnedAgentActuatorRegistry {
 public:
  EngineOwnedAgentActuatorRegistry() = default;

  bool valid() const { return sealed_; }
  const scratchbird::core::agents::AgentActuatorProviderRegistry& registry()
      const {
    return registry_;
  }
  const std::string& provenance() const { return provenance_; }
  const std::string& evidence_uuid() const { return evidence_uuid_; }
  bool HasLiveRouteProofForAction(const std::string& agent_type_id,
                                  const std::string& action_id,
                                  const std::string& actuator_id) const;

 private:
  friend EngineOwnedAgentActuatorRegistryBuildResult
  BuildEngineOwnedAgentActuatorRegistry(
      const EngineRequestContext& context,
      scratchbird::core::agents::AgentActuatorProviderRegistry registry,
      scratchbird::core::agents::AgentProductionRouteProofInputs route_proofs);

  bool sealed_ = false;
  std::string provenance_;
  std::string evidence_uuid_;
  scratchbird::core::agents::AgentActuatorProviderRegistry registry_;
  scratchbird::core::agents::AgentProductionRouteProofInputs route_proofs_;
};

struct EngineOwnedAgentActuatorRegistryBuildResult {
  scratchbird::core::agents::AgentRuntimeStatus status;
  EngineOwnedAgentActuatorRegistry registry;
};

EngineOwnedAgentActuatorRegistryBuildResult
BuildEngineOwnedAgentActuatorRegistry(
    const EngineRequestContext& context,
    scratchbird::core::agents::AgentActuatorProviderRegistry registry,
    scratchbird::core::agents::AgentProductionRouteProofInputs route_proofs);

struct AgentActionDispatchStoreRequest {
  EngineRequestContext context;
  scratchbird::core::agents::AgentActionRequest action;
  scratchbird::core::agents::AgentActionAuthorityProvenance authority;
  const EngineOwnedAgentActuatorRegistry* engine_registry = nullptr;
  scratchbird::core::agents::AgentRuntimeContext metric_context;
  scratchbird::core::agents::AgentMetricSnapshotEvaluationOptions
      metric_snapshot_options;
  std::vector<scratchbird::core::agents::AgentObservedMetricSnapshot>
      observed_metric_snapshots;
  bool production_live_path = true;
  bool fsync_or_checkpoint_evidence = false;
  bool durable_resource_reservation_required = true;
  scratchbird::core::agents::u64 resource_reservation_memory_bytes = 4096;
  scratchbird::core::agents::u64 resource_reservation_worker_slots = 1;
  scratchbird::core::agents::u64 resource_reservation_overhead_microseconds =
      1000;
  scratchbird::core::agents::u64 resource_reservation_max_active = 1024;
  scratchbird::core::agents::u64 resource_reservation_max_memory_bytes =
      64 * 1024 * 1024;
  scratchbird::core::agents::u64 resource_reservation_max_worker_slots = 8;
  scratchbird::core::agents::u64 resource_reservation_max_overhead_microseconds =
      10 * 1000 * 1000;
  bool subsystem_reported_success = true;
  bool intended_state_observed = true;
};

struct AgentActionDispatchStoreResult {
  scratchbird::core::agents::AgentActionDispatchResult dispatch;
  AgentDurableCatalogStoreResult loaded_catalog;
  AgentDurableCatalogStoreResult persisted_catalog;
  bool loaded_from_store = false;
  bool engine_owned_registry_validated = false;
  bool resource_reservation_acquired = false;
  bool resource_reservation_released = false;
  bool resource_reservation_persisted_before_dispatch = false;
  bool pending_action_intent_persisted_before_dispatch = false;
  std::string resource_reservation_uuid;
  bool persisted_to_store = false;
};

AgentActionDispatchStoreResult DispatchAgentActionWithDurableCatalogStore(
    const AgentActionDispatchStoreRequest& request);

}  // namespace scratchbird::engine::internal_api
