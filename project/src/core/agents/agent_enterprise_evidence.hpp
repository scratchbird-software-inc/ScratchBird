// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: AEIC_CRYPTO_DURABLE_AGENT_EVIDENCE_LEDGER
// SEARCH_KEY: AEIC_STRICT_AGENT_METRIC_RESOURCE_CONSUMPTION
// SEARCH_KEY: AEIC_AGENT_CRASH_RESTART_CONCURRENCY_TESTS

#include "agent_commercial_evidence.hpp"
#include "agent_durable_catalog.hpp"
#include "agent_metric_runtime.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scratchbird::core::agents {

struct AgentEnterpriseDecisionEvidenceRequest {
  DurableAgentCatalogImage* catalog = nullptr;
  std::string agent_type_id;
  std::string instance_uuid;
  std::string operation_id;
  std::string actuator_provider_id = "local_enterprise_agent_evaluator";
  AgentActionClass action_class = AgentActionClass::recommendation;
  std::string principal_uuid;
  std::vector<std::string> rights_used;
  std::vector<std::string> scope_uuids;
  std::string observed_metric_digest;
  AgentRuntimeContext metric_context;
  std::vector<AgentObservedMetricSnapshot> observed_metric_snapshots;
  AgentMetricSnapshotEvaluationOptions metric_snapshot_options;
  bool production_live_path = true;
  u64 policy_generation = 0;
  std::string decision_kind;
  std::string result_state;
  std::string diagnostic_code;
  std::vector<std::pair<std::string, std::string>> decision_fields;
  std::string outcome_verification_evidence_uuid;
  std::string redaction_class = "standard";
  std::string retention_class = "audit";
  u64 created_at_microseconds = 0;
};

struct AgentEnterpriseDecisionEvidenceResult {
  AgentRuntimeStatus status;
  std::string evidence_uuid;
  std::string action_uuid;
  std::string catalog_root_digest;
  bool evidence_written = false;
  bool action_written = false;
  bool history_written = false;
  bool catalog_root_refreshed = false;
  bool idempotent_replay = false;
};

std::string AgentEnterpriseDecisionPayloadDigest(
    const AgentEnterpriseDecisionEvidenceRequest& request);

AgentEnterpriseDecisionEvidenceResult AppendEnterpriseAgentDecisionEvidence(
    const AgentEnterpriseDecisionEvidenceRequest& request);

}  // namespace scratchbird::core::agents
