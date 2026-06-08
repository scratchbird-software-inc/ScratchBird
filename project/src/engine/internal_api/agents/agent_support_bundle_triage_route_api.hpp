// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agents/support_bundle_triage_agent.hpp"
#include "management/support_bundle_api.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: AEIC_SUPPORT_BUNDLE_TRIAGE_ROUTE_API
// Consumes support-bundle triage agent decisions through the engine
// support-bundle API. The route requires durable evidence, redaction and
// engine authorization; sidecar/caller-owned evidence cannot satisfy production
// support-bundle preparation.
struct AgentSupportBundleTriageRouteRequest {
  scratchbird::core::agents::implemented_agents::SupportBundleTriageResult
      triage_result;
  EnginePrepareSupportBundleRequest support_request;
  std::string agent_uuid;
  std::string evidence_uuid;
  bool durable_evidence_store_authority = false;
  bool tamper_chain_verified = false;
  bool redaction_profile_authoritative = false;
  bool support_export_authorized_by_engine = false;
  bool sidecar_authority = false;
};

struct AgentSupportBundleTriageRouteResult {
  bool ok = false;
  bool fail_closed = true;
  bool support_bundle_prepared = false;
  bool protected_material_suppressed = false;
  std::string diagnostic_code;
  std::vector<std::pair<std::string, std::string>> evidence;
  EnginePrepareSupportBundleResult support_result;
};

AgentSupportBundleTriageRouteResult ApplySupportBundleTriageAgentRoute(
    const AgentSupportBundleTriageRouteRequest& request);

}  // namespace scratchbird::engine::internal_api
