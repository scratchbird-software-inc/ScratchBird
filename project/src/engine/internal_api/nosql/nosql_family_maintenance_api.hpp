// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "agents/nosql_family_maintenance_agent.hpp"
#include "nosql/nosql_physical_provider_contract.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

struct EngineNoSqlMaintenanceGenerationCandidate {
  EngineNoSqlProviderFamily family = EngineNoSqlProviderFamily::kUnknown;
  std::string generation_id;
  std::string generation_kind;
  EngineApiU64 sealed_local_transaction_id = 0;
  EngineApiU64 superseded_local_transaction_id = 0;
  EngineApiU64 expires_after_local_transaction_id = 0;
  EngineApiU64 estimated_bytes = 0;
  bool generation_evidence_authoritative = false;
  bool ttl_evidence_authoritative = false;
};

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_FAMILY_MAINTENANCE_API
struct EnginePlanNoSqlFamilyMaintenanceRequest : EngineApiRequest {
  scratchbird::transaction::mga::AuthoritativeCleanupHorizonRequest horizon_request;
  scratchbird::core::agents::DynamicCleanupDebtSchedulerPolicy scheduler_policy;
  std::vector<EngineNoSqlMaintenanceGenerationCandidate> candidates;
  EngineApiU64 now_microseconds = 0;
  bool engine_mga_authoritative = false;
  bool foreground_work_active = false;
  bool execute_plan = false;
};

struct EnginePlanNoSqlFamilyMaintenanceResult : EngineApiResult {
  scratchbird::core::agents::implemented_agents::NoSqlFamilyMaintenanceAgentResult
      agent_result;
};

EnginePlanNoSqlFamilyMaintenanceResult EnginePlanNoSqlFamilyMaintenance(
    const EnginePlanNoSqlFamilyMaintenanceRequest& request);

}  // namespace scratchbird::engine::internal_api
