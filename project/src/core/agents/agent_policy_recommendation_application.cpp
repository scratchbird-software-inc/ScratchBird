// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_policy_recommendation_application.hpp"

#include <utility>

namespace scratchbird::core::agents {
namespace {

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

bool UnsafeAuthority(const AgentPolicyRecommendationApplicationRequest& request) {
  return request.parser_authority || request.client_authority ||
         request.reference_authority || request.sidecar_authority ||
         request.transaction_authority || request.visibility_authority ||
         request.finality_authority || request.recovery_authority ||
         request.security_authority;
}

AgentPolicyRecommendationApplicationResult Finish(
    const AgentPolicyRecommendationApplicationRequest& request,
    AgentPolicyRecommendationApplicationDecision decision,
    bool ok,
    std::string code,
    std::string detail) {
  AgentPolicyRecommendationApplicationResult result;
  result.status = {ok, std::move(code), std::move(detail)};
  result.decision = decision;
  result.ok = ok;
  result.fail_closed = !ok;
  result.recommendation_record_created =
      ok && decision ==
                AgentPolicyRecommendationApplicationDecision::
                    accepted_pending_review;
  result.auto_apply_blocked = true;
  Add(&result.evidence, "AEIC_POLICY_RECOMMENDATION_APPLICATION_CONTRACT");
  Add(&result.evidence, "policy_recommendation_application.diagnostic_code=" +
                            result.status.diagnostic_code);
  Add(&result.evidence, "policy_recommendation_application.agent_type_id=" +
                            request.agent_type_id);
  Add(&result.evidence, "policy_recommendation_application.policy_family=" +
                            request.policy_family);
  Add(&result.evidence, "policy_recommendation_application.scope_uuid_present=" +
                            BoolText(!request.scope_uuid.empty()));
  Add(&result.evidence, "policy_recommendation_application.metric_digest_present=" +
                            BoolText(!request.metric_digest.empty()));
  Add(&result.evidence, "policy_recommendation_application.strict_metric_snapshot=" +
                            BoolText(request.strict_metric_snapshot));
  Add(&result.evidence, "policy_recommendation_application.durable_catalog_state=" +
                            BoolText(request.durable_catalog_state));
  Add(&result.evidence, "policy_recommendation_application.auto_apply_blocked=true");
  Add(&result.evidence, "policy_recommendation_application.parser_authority=false");
  Add(&result.evidence, "policy_recommendation_application.client_authority=false");
  Add(&result.evidence, "policy_recommendation_application.reference_authority=false");
  Add(&result.evidence, "policy_recommendation_application.transaction_authority=false");
  Add(&result.evidence, "policy_recommendation_application.visibility_authority=false");
  Add(&result.evidence, "policy_recommendation_application.finality_authority=false");
  Add(&result.evidence, "policy_recommendation_application.recovery_authority=false");
  Add(&result.evidence, "policy_recommendation_application.security_authority=false");
  return result;
}

}  // namespace

const char* AgentPolicyRecommendationApplicationDecisionName(
    AgentPolicyRecommendationApplicationDecision decision) {
  switch (decision) {
    case AgentPolicyRecommendationApplicationDecision::accepted_pending_review:
      return "accepted_pending_review";
    case AgentPolicyRecommendationApplicationDecision::no_action:
      return "no_action";
    case AgentPolicyRecommendationApplicationDecision::refused:
      return "refused";
  }
  return "refused";
}

AgentPolicyRecommendationApplicationResult
EvaluateAgentPolicyRecommendationApplication(
    const AgentPolicyRecommendationApplicationRequest& request) {
  if (request.recommendation_uuid.empty() || request.agent_type_id.empty() ||
      request.evidence_uuid.empty() || request.policy_family.empty() ||
      request.scope_uuid.empty() || request.proposed_field_name.empty()) {
    return Finish(request,
                  AgentPolicyRecommendationApplicationDecision::refused,
                  false,
                  "SB_AGENT_POLICY_RECOMMENDATION_APPLICATION.IDENTITY_REQUIRED",
                  "recommendation_policy_scope_and_field_identity_required");
  }
  if (request.agent_type_id != "policy_recommendation_manager") {
    return Finish(request,
                  AgentPolicyRecommendationApplicationDecision::refused,
                  false,
                  "SB_AGENT_POLICY_RECOMMENDATION_APPLICATION.AGENT_REFUSED",
                  "only_policy_recommendation_manager_can_create_policy_recommendation_records");
  }
  if (!request.durable_catalog_state) {
    return Finish(request,
                  AgentPolicyRecommendationApplicationDecision::refused,
                  false,
                  "SB_AGENT_POLICY_RECOMMENDATION_APPLICATION.DURABLE_STATE_REQUIRED",
                  "durable_agent_catalog_state_required");
  }
  if (!request.strict_metric_snapshot || !request.metric_trusted ||
      !request.metric_fresh || request.metric_digest.empty()) {
    return Finish(request,
                  AgentPolicyRecommendationApplicationDecision::refused,
                  false,
                  "SB_AGENT_POLICY_RECOMMENDATION_APPLICATION.STRICT_METRIC_REQUIRED",
                  "fresh_trusted_strict_metric_digest_required");
  }
  if (request.policy_generation == 0 ||
      request.observed_policy_generation == 0 ||
      request.policy_generation != request.observed_policy_generation) {
    return Finish(request,
                  AgentPolicyRecommendationApplicationDecision::refused,
                  false,
                  "SB_AGENT_POLICY_RECOMMENDATION_APPLICATION.POLICY_GENERATION_REQUIRED",
                  "current_policy_generation_required");
  }
  if (!request.redaction_policy_valid) {
    return Finish(request,
                  AgentPolicyRecommendationApplicationDecision::refused,
                  false,
                  "SB_AGENT_POLICY_RECOMMENDATION_APPLICATION.REDACTION_REQUIRED",
                  "valid_redaction_policy_required");
  }
  if (UnsafeAuthority(request)) {
    return Finish(request,
                  AgentPolicyRecommendationApplicationDecision::refused,
                  false,
                  "SB_AGENT_POLICY_RECOMMENDATION_APPLICATION.UNSAFE_AUTHORITY",
                  "agent_policy_recommendations_cannot_provide_engine_authority");
  }
  if (!request.no_auto_apply_required) {
    return Finish(request,
                  AgentPolicyRecommendationApplicationDecision::refused,
                  false,
                  "SB_AGENT_POLICY_RECOMMENDATION_APPLICATION.AUTO_APPLY_REFUSED",
                  "policy_recommendations_must_be_recorded_pending_review_not_auto_applied");
  }

  auto fields = DefaultAgentPolicyConfigFieldsForFamily(request.policy_family);
  if (fields.empty()) {
    return Finish(request,
                  AgentPolicyRecommendationApplicationDecision::refused,
                  false,
                  "SB_AGENT_POLICY_RECOMMENDATION_APPLICATION.UNKNOWN_POLICY_FAMILY",
                  request.policy_family);
  }
  fields[request.proposed_field_name] = request.proposed_field_value;

  AgentPolicy candidate;
  candidate.policy_uuid = request.recommendation_uuid;
  candidate.policy_name = "recommended:" + request.policy_family;
  candidate.policy_family = request.policy_family;
  candidate.scope = request.scope_uuid;
  candidate.policy_generation = request.policy_generation;
  candidate.config_fields = fields;
  const auto schema = ValidateAgentPolicyConfigAgainstSchema(candidate);
  if (!schema.ok) {
    return Finish(request,
                  AgentPolicyRecommendationApplicationDecision::refused,
                  false,
                  "SB_AGENT_POLICY_RECOMMENDATION_APPLICATION.SCHEMA_REFUSED",
                  schema.diagnostic_code);
  }

  auto result = Finish(request,
                       AgentPolicyRecommendationApplicationDecision::
                           accepted_pending_review,
                       true,
                       "SB_AGENT_POLICY_RECOMMENDATION_APPLICATION.ACCEPTED",
                       "schema_validated_policy_recommendation_record_created_pending_review");
  result.schema_validated = true;
  result.candidate_policy_fields = std::move(fields);
  Add(&result.evidence, "policy_recommendation_application.schema_validated=true");
  Add(&result.evidence,
      "policy_recommendation_application.recommendation_record_created=true");
  Add(&result.evidence,
      "policy_recommendation_application.proposed_field=" +
          request.proposed_field_name);
  return result;
}

}  // namespace scratchbird::core::agents
