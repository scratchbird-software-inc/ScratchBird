// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_durable_catalog.hpp"
#include "api_types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_AGENT_MANAGEMENT
// Engine-owned agent management API. Parser packages may render these results,
// but security, policy, and action authority remain inside the engine.

struct EngineAgentDurableRuntimeState {
  const scratchbird::core::agents::DurableAgentCatalogImage* catalog = nullptr;
  scratchbird::core::agents::DurableAgentCatalogImage* mutable_catalog = nullptr;
  bool production_live_path = false;
  bool require_durable_runtime_state = false;
};

struct EngineAgentCatalogIdentitySource {
  std::string agent_type_id;
  std::string agent_uuid;
  std::string scope_uuid;
  std::string policy_uuid;
  std::string policy_name;
  std::string component;
  std::string scope_kind;
  bool scope_visible = true;
  bool policy_visible = true;
};

struct EngineListAgentsRequest : EngineApiRequest {
  std::vector<EngineAgentCatalogIdentitySource> agent_catalog_identity_sources;
  EngineAgentDurableRuntimeState durable_runtime_state;
};
struct EngineListAgentsResult : EngineApiResult {};
EngineListAgentsResult EngineListAgents(const EngineListAgentsRequest& request);

struct EngineShowAgentRequest : EngineApiRequest {
  std::vector<EngineAgentCatalogIdentitySource> agent_catalog_identity_sources;
  EngineAgentDurableRuntimeState durable_runtime_state;
};
struct EngineShowAgentResult : EngineApiResult {};
EngineShowAgentResult EngineShowAgent(const EngineShowAgentRequest& request);

struct EngineStartAgentRequest : EngineApiRequest {
  EngineAgentDurableRuntimeState durable_runtime_state;
};
struct EngineStartAgentResult : EngineApiResult {};
EngineStartAgentResult EngineStartAgent(const EngineStartAgentRequest& request);

struct EngineStopAgentRequest : EngineApiRequest {
  EngineAgentDurableRuntimeState durable_runtime_state;
};
struct EngineStopAgentResult : EngineApiResult {};
EngineStopAgentResult EngineStopAgent(const EngineStopAgentRequest& request);

struct EnginePauseAgentRequest : EngineApiRequest {
  EngineAgentDurableRuntimeState durable_runtime_state;
};
struct EnginePauseAgentResult : EngineApiResult {};
EnginePauseAgentResult EnginePauseAgent(const EnginePauseAgentRequest& request);

struct EngineResumeAgentRequest : EngineApiRequest {
  EngineAgentDurableRuntimeState durable_runtime_state;
};
struct EngineResumeAgentResult : EngineApiResult {};
EngineResumeAgentResult EngineResumeAgent(const EngineResumeAgentRequest& request);

struct EngineConfigureAgentRequest : EngineApiRequest {
  EngineAgentDurableRuntimeState durable_runtime_state;
};
struct EngineConfigureAgentResult : EngineApiResult {};
EngineConfigureAgentResult EngineConfigureAgent(const EngineConfigureAgentRequest& request);

struct EngineRunAgentRequest : EngineApiRequest {
  EngineAgentDurableRuntimeState durable_runtime_state;
};
struct EngineRunAgentResult : EngineApiResult {};
EngineRunAgentResult EngineRunAgent(const EngineRunAgentRequest& request);

struct EngineDryRunAgentRequest : EngineApiRequest {
  EngineAgentDurableRuntimeState durable_runtime_state;
};
struct EngineDryRunAgentResult : EngineApiResult {};
EngineDryRunAgentResult EngineDryRunAgent(const EngineDryRunAgentRequest& request);

struct EngineOverrideAgentRequest : EngineApiRequest {
  EngineAgentDurableRuntimeState durable_runtime_state;
};
struct EngineOverrideAgentResult : EngineApiResult {};
EngineOverrideAgentResult EngineOverrideAgent(const EngineOverrideAgentRequest& request);

struct EngineSysAgentsRequest : EngineApiRequest {
  std::vector<EngineAgentCatalogIdentitySource> agent_catalog_identity_sources;
  EngineAgentDurableRuntimeState durable_runtime_state;
};
struct EngineSysAgentsResult : EngineApiResult {};
EngineSysAgentsResult EngineSysAgents(const EngineSysAgentsRequest& request);

struct EngineClusterSysAgentsRequest : EngineApiRequest {};
struct EngineClusterSysAgentsResult : EngineApiResult {};
EngineClusterSysAgentsResult EngineClusterSysAgents(const EngineClusterSysAgentsRequest& request);

struct EngineAgentCommandSurfaceRequest : EngineApiRequest {
  std::vector<EngineAgentCatalogIdentitySource> agent_catalog_identity_sources;
};
struct EngineAgentCommandSurfaceResult : EngineApiResult {};
EngineAgentCommandSurfaceResult EngineAgentCommandSurfaceOperation(
    const EngineAgentCommandSurfaceRequest& request);

struct EngineThirdPartyAgentManagementRequestRecord {
  std::string request_uuid;
  std::string requester_principal_uuid;
  std::string external_system_id;
  std::string agent_ref;
  std::string operation;
  std::string requested_action;
  std::string policy_ref;
  std::string reason_code;
  std::string requested_expiry;
  std::string redaction_context;
  std::string idempotency_key;
  bool residency_context_present = true;
  bool residency_allowed = true;
  bool redaction_context_present = true;
  bool backpressure = false;
  std::string retry_after;
  std::string protected_payload;
  bool evidence_store_available = true;
};

struct EngineThirdPartyAgentManagementRequest : EngineApiRequest {
  EngineThirdPartyAgentManagementRequestRecord management_request;
  std::vector<EngineAgentCatalogIdentitySource> agent_catalog_identity_sources;
};
struct EngineThirdPartyAgentManagementResult : EngineApiResult {};
EngineThirdPartyAgentManagementResult EngineSubmitThirdPartyAgentManagementRequest(
    const EngineThirdPartyAgentManagementRequest& request);

struct EngineAgentZeroGreyOutputContract {
  std::vector<std::string> allowed_result_states;
  std::vector<std::string> prohibited_fragments;
  std::vector<std::string> evidence_payload_fields;
};

EngineAgentZeroGreyOutputContract BuiltinAgentZeroGreyOutputContract();
bool EngineAgentZeroGreyResultStateAllowed(std::string_view result_state);
bool EngineAgentZeroGreyResultStateAmbiguous(std::string_view result_state);

}  // namespace scratchbird::engine::internal_api
