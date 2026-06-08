// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_enterprise_evidence.hpp"

#include <cstddef>
#include <iomanip>
#include <openssl/sha.h>
#include <sstream>
#include <utility>

namespace scratchbird::core::agents {
namespace {

std::string HexBytes(const unsigned char* bytes, std::size_t size) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < size; ++i) {
    out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
  }
  return out.str();
}

std::string Sha256Digest(const std::string& payload) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(payload.data()),
         payload.size(),
         digest);
  return HexBytes(digest, SHA256_DIGEST_LENGTH);
}

std::string Join(const std::vector<std::string>& values) {
  std::ostringstream out;
  bool first = true;
  for (const auto& value : values) {
    if (!first) { out << ','; }
    first = false;
    out << value;
  }
  return out.str();
}

std::string DecisionPayload(
    const AgentEnterpriseDecisionEvidenceRequest& request) {
  std::ostringstream payload;
  payload << "enterprise_agent_decision_v1\n"
          << request.agent_type_id << '\n'
          << request.instance_uuid << '\n'
          << request.operation_id << '\n'
          << request.decision_kind << '\n'
          << request.result_state << '\n'
          << request.diagnostic_code << '\n'
          << request.observed_metric_digest << '\n'
          << request.policy_generation << '\n'
          << Join(request.scope_uuids) << '\n';
  for (const auto& field : request.decision_fields) {
    payload << field.first << '=' << field.second << '\n';
  }
  return payload.str();
}

AgentRuntimeStatus ValidateRequest(
    const AgentEnterpriseDecisionEvidenceRequest& request) {
  if (request.catalog == nullptr) {
    return AgentError("SB_AGENT_ENTERPRISE_EVIDENCE.CATALOG_REQUIRED");
  }
  const auto catalog_status =
      ValidateDurableAgentCatalogForProduction(*request.catalog);
  if (!catalog_status.ok) { return catalog_status; }
  if (request.agent_type_id.empty() || request.instance_uuid.empty() ||
      request.operation_id.empty() || request.principal_uuid.empty() ||
      request.rights_used.empty() || request.scope_uuids.empty() ||
      request.policy_generation == 0 ||
      request.decision_kind.empty() || request.result_state.empty() ||
      request.diagnostic_code.empty()) {
    return AgentError(
        "SB_AGENT_ENTERPRISE_EVIDENCE.REQUIRED_FIELD_MISSING",
        request.agent_type_id);
  }
  if (request.outcome_verification_evidence_uuid.empty()) {
    return AgentError(
        "SB_AGENT_ENTERPRISE_EVIDENCE.OUTCOME_VERIFICATION_REQUIRED",
        request.agent_type_id);
  }
  return AgentRuntimeStatus{
      true, "SB_AGENT_ENTERPRISE_EVIDENCE.REQUEST_ACCEPTED",
      request.agent_type_id};
}

AgentMetricSnapshotEvaluation EvaluateStrictMetricEvidence(
    const AgentEnterpriseDecisionEvidenceRequest& request) {
  AgentMetricSnapshotEvaluation evaluation;
  const auto descriptor = FindAgentType(request.agent_type_id);
  if (!descriptor.has_value()) {
    evaluation.status = AgentError(
        "SB_AGENT_ENTERPRISE_EVIDENCE.AGENT_DESCRIPTOR_REQUIRED",
        request.agent_type_id);
    evaluation.accepted = false;
    evaluation.failed_closed = true;
    return evaluation;
  }

  AgentRuntimeContext context = request.metric_context;
  if (context.database_uuid.empty() && !request.scope_uuids.empty()) {
    context.database_uuid = request.scope_uuids.front();
  }
  if (context.principal_uuid.empty()) {
    context.principal_uuid = request.principal_uuid;
  }
  if (context.wall_now_microseconds == 0) {
    context.wall_now_microseconds =
        request.created_at_microseconds == 0 ? 1 : request.created_at_microseconds;
  }
  context.security_context_present = true;

  AgentMetricSnapshotEvaluationOptions options =
      request.metric_snapshot_options;
  if (request.production_live_path) {
    options.mode = AgentMetricRuntimeMode::production_strict;
  }
  if (options.expected_scope_uuid.empty() && !request.scope_uuids.empty()) {
    options.expected_scope_uuid = request.scope_uuids.front();
  }
  return EvaluateAgentObservedMetricSnapshots(
      *descriptor, context, request.observed_metric_snapshots, options);
}

}  // namespace

std::string AgentEnterpriseDecisionPayloadDigest(
    const AgentEnterpriseDecisionEvidenceRequest& request) {
  return Sha256Digest(DecisionPayload(request));
}

AgentEnterpriseDecisionEvidenceResult AppendEnterpriseAgentDecisionEvidence(
    const AgentEnterpriseDecisionEvidenceRequest& request) {
  AgentEnterpriseDecisionEvidenceResult result;
  const auto request_status = ValidateRequest(request);
  if (!request_status.ok) {
    result.status = request_status;
    return result;
  }
  const auto metric_evaluation = EvaluateStrictMetricEvidence(request);
  if (!metric_evaluation.accepted) {
    result.status = metric_evaluation.status;
    return result;
  }
  if (!request.observed_metric_digest.empty() &&
      request.observed_metric_digest != metric_evaluation.input_digest) {
    result.status = AgentError(
        "SB_AGENT_ENTERPRISE_EVIDENCE.METRIC_DIGEST_MISMATCH",
        request.agent_type_id);
    return result;
  }
  AgentEnterpriseDecisionEvidenceRequest effective_request = request;
  effective_request.observed_metric_digest = metric_evaluation.input_digest;

  const auto payload = DecisionPayload(effective_request);
  const auto payload_digest = Sha256Digest(payload);
  const auto action_uuid = DeterministicAgentRuntimeObjectUuidFromKey(
      "enterprise_agent_action|" + effective_request.agent_type_id + "|" +
      effective_request.instance_uuid + "|" + effective_request.operation_id + "|" +
      payload_digest);
  const std::string idempotency_key =
      "enterprise-agent-decision|" + payload_digest;
  for (const auto& existing : request.catalog->actions) {
    if (existing.idempotency_key != idempotency_key ||
        existing.operation_id != effective_request.operation_id) {
      continue;
    }
    result.status = AgentRuntimeStatus{
        true, "SB_AGENT_ENTERPRISE_EVIDENCE.IDEMPOTENT_REPLAY",
        existing.action_uuid};
    result.evidence_uuid = existing.evidence_uuid;
    result.action_uuid = existing.action_uuid;
    result.catalog_root_digest = request.catalog->authority.catalog_root_digest;
    result.idempotent_replay = true;
    return result;
  }

  AgentActionRequest action;
  action.action_uuid = action_uuid;
  action.agent_type_id = effective_request.agent_type_id;
  action.instance_uuid = effective_request.instance_uuid;
  action.action_class = effective_request.action_class;
  action.actuator_id = effective_request.actuator_provider_id;
  action.operation_id = effective_request.operation_id;
  action.idempotency_key = idempotency_key;
  action.dry_run = effective_request.action_class != AgentActionClass::direct_bounded_action;
  action.inputs["metric_digest"] = effective_request.observed_metric_digest;
  action.inputs["scope_uuid"] = effective_request.scope_uuids.front();
  action.inputs["decision_payload_digest"] = payload_digest;

  AgentActionAuthorityProvenance authority;
  authority.source = AgentActionAuthoritySource::sealed_internal_bootstrap;
  authority.principal_uuid = effective_request.principal_uuid;
  authority.scope_uuid = effective_request.scope_uuids.front();
  authority.provenance_evidence_uuid =
      effective_request.outcome_verification_evidence_uuid;
  authority.rights = effective_request.rights_used;
  authority.sealed_bootstrap_authority = true;

  CommercialAgentEvidenceBuildRequest build;
  build.action = action;
  build.authority = authority;
  build.provider_id = effective_request.actuator_provider_id;
  build.input_evidence_digest = payload_digest;
  build.input_metric_digest = effective_request.observed_metric_digest;
  build.policy_generation = effective_request.policy_generation;
  build.scope_uuids = effective_request.scope_uuids;
  build.decision_payload = payload;
  build.result_state = effective_request.result_state;
  build.diagnostic_code = effective_request.diagnostic_code;
  build.redaction_class = effective_request.redaction_class;
  build.retention_class = effective_request.retention_class;
  build.outcome_verification_evidence_uuid =
      effective_request.outcome_verification_evidence_uuid;
  build.previous_tamper_digest =
      request.catalog->evidence.empty()
          ? "scratchbird-agent-evidence-ledger-genesis"
          : request.catalog->evidence.back().tamper_chain_digest;
  build.tamper_key_id = "agent-evidence-ledger-key-v1";
  build.storage_linkage_digest =
      Sha256Digest(request.catalog->authority.catalog_storage_uuid + "|" +
      request.catalog->authority.catalog_root_digest + "|" +
                   action_uuid);
  build.created_at_microseconds =
      effective_request.created_at_microseconds == 0 ? 1 : effective_request.created_at_microseconds;
  AgentEvidenceRecord evidence = BuildCommercialAgentEvidence(build);

  const auto evidence_validation = ValidateCommercialAgentEvidence(evidence);
  if (!evidence_validation.status.ok) {
    result.status = evidence_validation.status;
    return result;
  }

  DurableAgentActionRecord action_record;
  action_record.action_uuid = action_uuid;
  action_record.instance_uuid = effective_request.instance_uuid;
  action_record.owner_uuid = effective_request.principal_uuid;
  action_record.operation_id = effective_request.operation_id;
  action_record.actuator_provider_id = effective_request.actuator_provider_id;
  action_record.state = DurableAgentActionState::completed;
  action_record.idempotency_key = action.idempotency_key;
  action_record.input_evidence_digest = payload_digest;
  action_record.evidence_uuid = evidence.evidence_uuid;
  action_record.verification_evidence_uuid =
      effective_request.outcome_verification_evidence_uuid;
  action_record.diagnostic_code = effective_request.diagnostic_code;
  action_record.generation = request.catalog->authority.catalog_generation + 1;
  action_record.outcome_verified = true;

  DurableAgentHealthRecord health;
  health.instance_uuid = effective_request.instance_uuid;
  health.health_state = effective_request.result_state;
  health.diagnostic_code = effective_request.diagnostic_code;
  health.evidence_uuid = evidence.evidence_uuid;
  health.observed_at_microseconds =
      effective_request.created_at_microseconds == 0 ? 1 : effective_request.created_at_microseconds;

  DurableAgentHistoryRecord history;
  history.history_uuid = DeterministicAgentRuntimeObjectUuidFromKey(
      "enterprise_agent_history|" + evidence.evidence_uuid + "|" +
      effective_request.operation_id);
  history.subject_uuid = effective_request.instance_uuid;
  history.event_kind = "agent_enterprise_decision." + effective_request.operation_id;
  history.diagnostic_code = effective_request.diagnostic_code;
  history.evidence_uuid = evidence.evidence_uuid;
  history.recorded_at_microseconds =
      effective_request.created_at_microseconds == 0 ? 1 : effective_request.created_at_microseconds;

  request.catalog->evidence.push_back(std::move(evidence));
  result.evidence_written = true;
  request.catalog->actions.push_back(std::move(action_record));
  result.action_written = true;
  request.catalog->health.push_back(std::move(health));
  request.catalog->retained_history.push_back(std::move(history));
  result.history_written = true;

  const auto refresh = RefreshDurableAgentCatalogAuthorityDigest(
      request.catalog, request.catalog->evidence.back().evidence_uuid);
  if (!refresh.ok) {
    result.status = refresh;
    return result;
  }
  result.catalog_root_refreshed = true;
  result.evidence_uuid = request.catalog->evidence.back().evidence_uuid;
  result.action_uuid = action_uuid;
  result.catalog_root_digest = request.catalog->authority.catalog_root_digest;
  result.status = AgentRuntimeStatus{
      true, "SB_AGENT_ENTERPRISE_EVIDENCE.PERSISTED", result.evidence_uuid};
  return result;
}

}  // namespace scratchbird::core::agents
