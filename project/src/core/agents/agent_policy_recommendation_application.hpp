// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: AEIC_POLICY_RECOMMENDATION_APPLICATION_CONTRACT
// Policy recommendations are consumed as durable, schema-validated advisory
// records. They are not parser, client, donor, transaction, visibility,
// finality, recovery, security, or automatic policy-apply authority.

#include "agent_policy_schema.hpp"

#include <map>
#include <string>
#include <vector>

namespace scratchbird::core::agents {

enum class AgentPolicyRecommendationApplicationDecision {
  accepted_pending_review,
  no_action,
  refused
};

struct AgentPolicyRecommendationApplicationRequest {
  std::string recommendation_uuid;
  std::string agent_type_id = "policy_recommendation_manager";
  std::string evidence_uuid;
  std::string policy_family;
  std::string scope_uuid;
  std::string metric_digest;
  std::string proposed_field_name;
  std::string proposed_field_value;
  u64 policy_generation = 0;
  u64 observed_policy_generation = 0;
  bool durable_catalog_state = false;
  bool strict_metric_snapshot = false;
  bool metric_trusted = false;
  bool metric_fresh = false;
  bool redaction_policy_valid = true;
  bool no_auto_apply_required = true;
  bool parser_authority = false;
  bool client_authority = false;
  bool donor_authority = false;
  bool sidecar_authority = false;
  bool transaction_authority = false;
  bool visibility_authority = false;
  bool finality_authority = false;
  bool recovery_authority = false;
  bool security_authority = false;
};

struct AgentPolicyRecommendationApplicationResult {
  AgentRuntimeStatus status;
  AgentPolicyRecommendationApplicationDecision decision =
      AgentPolicyRecommendationApplicationDecision::refused;
  bool ok = false;
  bool fail_closed = true;
  bool recommendation_record_created = false;
  bool schema_validated = false;
  bool auto_apply_blocked = true;
  std::map<std::string, std::string> candidate_policy_fields;
  std::vector<std::string> evidence;
};

const char* AgentPolicyRecommendationApplicationDecisionName(
    AgentPolicyRecommendationApplicationDecision decision);

AgentPolicyRecommendationApplicationResult
EvaluateAgentPolicyRecommendationApplication(
    const AgentPolicyRecommendationApplicationRequest& request);

}  // namespace scratchbird::core::agents
