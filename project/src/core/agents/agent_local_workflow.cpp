// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_local_workflow.hpp"

#include "agent_commercial_evidence.hpp"

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <openssl/sha.h>
#include <sstream>
#include <utility>

namespace scratchbird::core::agents {
namespace {

constexpr u64 kDayMicros = 86400000000ull;
constexpr u64 kYearMicros = 365ull * kDayMicros;

std::string HexBytes(const unsigned char* bytes, std::size_t size) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < size; ++i) {
    out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
  }
  return out.str();
}

std::string Sha256(const std::string& payload) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(payload.data()),
         payload.size(),
         digest);
  return HexBytes(digest, SHA256_DIGEST_LENGTH);
}

void AddEvidence(AgentLocalWorkflowApplyResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back(std::move(key) + "=" + std::move(value));
}

bool DomainAuthoritySatisfied(const AgentLocalWorkflowRequest& request) {
  const auto& authority = request.authority;
  switch (request.domain) {
    case AgentLocalWorkflowDomain::backup:
    case AgentLocalWorkflowDomain::restore_drill:
    case AgentLocalWorkflowDomain::pitr:
      return authority.storage_snapshot_authoritative &&
             authority.metadata_authoritative;
    case AgentLocalWorkflowDomain::archive:
      return authority.metadata_authoritative;
    case AgentLocalWorkflowDomain::export_adapter:
      return authority.redaction_policy_valid &&
             authority.residency_policy_valid &&
             authority.metadata_authoritative;
    case AgentLocalWorkflowDomain::identity:
      return authority.security_catalog_authoritative &&
             authority.redaction_policy_valid;
    case AgentLocalWorkflowDomain::session_control:
      return authority.session_registry_authoritative &&
             authority.security_catalog_authoritative;
    case AgentLocalWorkflowDomain::job_control:
      return authority.job_queue_authoritative;
  }
  return false;
}

std::string AgentTypeForWorkflowDomain(AgentLocalWorkflowDomain domain) {
  switch (domain) {
    case AgentLocalWorkflowDomain::backup: return "backup_manager";
    case AgentLocalWorkflowDomain::archive: return "archive_manager";
    case AgentLocalWorkflowDomain::restore_drill: return "restore_drill_manager";
    case AgentLocalWorkflowDomain::pitr: return "pitr_manager";
    case AgentLocalWorkflowDomain::export_adapter: return "export_adapter_manager";
    case AgentLocalWorkflowDomain::identity: return "identity_manager";
    case AgentLocalWorkflowDomain::session_control: return "session_control_manager";
    case AgentLocalWorkflowDomain::job_control: return "job_control_manager";
  }
  return "agent_local_workflow";
}

std::string EvidenceDigestForWorkflow(const AgentLocalWorkflowRequest& request,
                                      const AgentLocalWorkflowRecord& record) {
  std::ostringstream payload;
  payload << record.workflow_uuid << '\n'
          << AgentLocalWorkflowDomainName(record.domain) << '\n'
          << record.operation_id << '\n'
          << record.subject_uuid << '\n'
          << record.idempotency_key << '\n'
          << record.input_digest << '\n'
          << AgentLocalWorkflowStateName(record.state) << '\n'
          << request.authority.evidence_uuid << '\n';
  return Sha256(payload.str());
}

AgentLocalWorkflowApplyResult Finish(const AgentLocalWorkflowRequest& request,
                                     AgentRuntimeStatus status,
                                     AgentLocalWorkflowRecord record,
                                     bool idempotent) {
  AgentLocalWorkflowApplyResult result;
  result.status = std::move(status);
  result.record = std::move(record);
  result.ok = result.status.ok;
  result.idempotent = idempotent;
  result.failed_closed = !result.status.ok;
  AddEvidence(&result, "agent_local_workflow",
              AgentLocalWorkflowDomainName(request.domain));
  AddEvidence(&result, "operation_id", request.operation_id);
  AddEvidence(&result, "state", AgentLocalWorkflowStateName(result.record.state));
  AddEvidence(&result, "workflow_uuid", result.record.workflow_uuid);
  AddEvidence(&result, "transaction_finality_authority", "false");
  AddEvidence(&result, "visibility_authority", "false");
  AddEvidence(&result, "recovery_authority", "false");
  AddEvidence(&result, "security_authority", "false");
  AddEvidence(&result, "parser_authority", "false");
  AddEvidence(&result, "donor_authority", "false");
  AddEvidence(&result, "client_authority", "false");
  AddEvidence(&result, "cluster_authority", "false");
  return result;
}

}  // namespace

const char* AgentLocalWorkflowDomainName(AgentLocalWorkflowDomain domain) {
  switch (domain) {
    case AgentLocalWorkflowDomain::backup: return "backup";
    case AgentLocalWorkflowDomain::archive: return "archive";
    case AgentLocalWorkflowDomain::restore_drill: return "restore_drill";
    case AgentLocalWorkflowDomain::pitr: return "pitr";
    case AgentLocalWorkflowDomain::export_adapter: return "export_adapter";
    case AgentLocalWorkflowDomain::identity: return "identity";
    case AgentLocalWorkflowDomain::session_control: return "session_control";
    case AgentLocalWorkflowDomain::job_control: return "job_control";
  }
  return "unknown";
}

const char* AgentLocalWorkflowStateName(AgentLocalWorkflowState state) {
  switch (state) {
    case AgentLocalWorkflowState::prepared: return "prepared";
    case AgentLocalWorkflowState::applied: return "applied";
    case AgentLocalWorkflowState::verified: return "verified";
    case AgentLocalWorkflowState::cancelled: return "cancelled";
    case AgentLocalWorkflowState::idempotent_replay: return "idempotent_replay";
    case AgentLocalWorkflowState::refused: return "refused";
  }
  return "refused";
}

std::string AgentLocalWorkflowInputDigest(
    const AgentLocalWorkflowRequest& request) {
  std::ostringstream payload;
  payload << "agent_local_workflow_v1\n"
          << AgentLocalWorkflowDomainName(request.domain) << '\n'
          << request.operation_id << '\n'
          << request.idempotency_key << '\n'
          << request.authority.database_uuid << '\n'
          << request.authority.principal_uuid << '\n'
          << request.authority.subject_uuid << '\n'
          << request.authority.mga_transaction_uuid << '\n'
          << request.authority.local_transaction_id << '\n'
          << request.authority.catalog_generation << '\n'
          << request.authority.evidence_uuid << '\n'
          << (request.dry_run ? "dry_run" : "live") << '\n';
  return Sha256(payload.str());
}

AgentRuntimeStatus ValidateAgentLocalWorkflowAuthority(
    const AgentLocalWorkflowRequest& request) {
  const auto& authority = request.authority;
  if (request.operation_id.empty() || request.idempotency_key.empty() ||
      authority.subject_uuid.empty()) {
    return AgentError("SB_AGENT_LOCAL_WORKFLOW.IDENTITY_REQUIRED",
                      AgentLocalWorkflowDomainName(request.domain));
  }
  if (authority.parser_authority || authority.client_authority ||
      authority.donor_authority || authority.recovery_authority ||
      authority.cluster_route_requested) {
    return AgentError("SB_AGENT_LOCAL_WORKFLOW.UNTRUSTED_AUTHORITY",
                      AgentLocalWorkflowDomainName(request.domain));
  }
  if (authority.database_uuid.empty() || authority.principal_uuid.empty() ||
      authority.mga_transaction_uuid.empty() || authority.evidence_uuid.empty() ||
      authority.local_transaction_id == 0 || authority.catalog_generation == 0 ||
      !authority.durable_catalog_bound ||
      !authority.transaction_inventory_bound) {
    return AgentError("SB_AGENT_LOCAL_WORKFLOW.DURABLE_MGA_EVIDENCE_REQUIRED",
                      AgentLocalWorkflowDomainName(request.domain));
  }
  if (!DomainAuthoritySatisfied(request)) {
    return AgentError("SB_AGENT_LOCAL_WORKFLOW.DOMAIN_AUTHORITY_REQUIRED",
                      AgentLocalWorkflowDomainName(request.domain));
  }
  if (!request.subsystem_precondition_satisfied) {
    return AgentError("SB_AGENT_LOCAL_WORKFLOW.PRECONDITION_REFUSED",
                      request.operation_id);
  }
  return {true, "SB_AGENT_LOCAL_WORKFLOW.AUTHORITY_ACCEPTED",
          authority.evidence_uuid};
}

AgentLocalWorkflowApplyResult AgentLocalWorkflowLedger::Apply(
    const AgentLocalWorkflowRequest& request) {
  const auto authority = ValidateAgentLocalWorkflowAuthority(request);
  if (!authority.ok) {
    AgentLocalWorkflowRecord record;
    record.domain = request.domain;
    record.state = AgentLocalWorkflowState::refused;
    record.operation_id = request.operation_id;
    record.subject_uuid = request.authority.subject_uuid;
    record.idempotency_key = request.idempotency_key;
    record.input_digest = request.input_digest.empty()
                              ? AgentLocalWorkflowInputDigest(request)
                              : request.input_digest;
    record.diagnostic_code = authority.diagnostic_code;
    record.parser_authority = request.authority.parser_authority;
    record.client_authority = request.authority.client_authority;
    record.donor_authority = request.authority.donor_authority;
    record.recovery_authority = request.authority.recovery_authority;
    record.cluster_authority = request.authority.cluster_route_requested;
    record.workflow_uuid = DeterministicAgentRuntimeObjectUuidFromKey(
        "agent_local_workflow_refused|" + request.operation_id + "|" +
        request.idempotency_key + "|" + record.input_digest);
    return Finish(request, authority, std::move(record), false);
  }

  const std::string input_digest =
      request.input_digest.empty() ? AgentLocalWorkflowInputDigest(request)
                                   : request.input_digest;
  for (const auto& existing : records_) {
    if (existing.idempotency_key == request.idempotency_key &&
        existing.domain == request.domain &&
        existing.operation_id == request.operation_id) {
      auto replay = existing;
      replay.state = AgentLocalWorkflowState::idempotent_replay;
      replay.diagnostic_code = "SB_AGENT_LOCAL_WORKFLOW.IDEMPOTENT_REPLAY";
      return Finish(request,
                    {true, "SB_AGENT_LOCAL_WORKFLOW.IDEMPOTENT_REPLAY",
                     existing.workflow_uuid},
                    std::move(replay),
                    true);
    }
  }
  if (durable_catalog_ != nullptr) {
    const std::string provider_id =
        std::string("agent_local_workflow:") +
        AgentLocalWorkflowDomainName(request.domain);
    for (const auto& action : durable_catalog_->actions) {
      if (action.idempotency_key != request.idempotency_key ||
          action.operation_id != request.operation_id ||
          action.actuator_provider_id != provider_id) {
        continue;
      }
      AgentLocalWorkflowRecord replay;
      replay.workflow_uuid = action.action_uuid;
      replay.domain = request.domain;
      replay.state = AgentLocalWorkflowState::idempotent_replay;
      replay.operation_id = action.operation_id;
      replay.subject_uuid = request.authority.subject_uuid;
      replay.idempotency_key = action.idempotency_key;
      replay.input_digest = action.input_evidence_digest;
      replay.verification_evidence_uuid = action.verification_evidence_uuid;
      replay.diagnostic_code = "SB_AGENT_LOCAL_WORKFLOW.IDEMPOTENT_REPLAY";
      replay.generation = action.generation;
      replay.dry_run = action.state == DurableAgentActionState::completed &&
                       !action.outcome_verified;
      replay.outcome_verified = action.outcome_verified;
      replay.parser_authority = action.parser_authority;
      replay.client_authority = action.client_authority;
      replay.donor_authority = action.donor_authority;
      replay.recovery_authority = false;
      replay.cluster_authority = false;
      return Finish(request,
                    {true, "SB_AGENT_LOCAL_WORKFLOW.IDEMPOTENT_REPLAY",
                     action.action_uuid},
                    std::move(replay),
                    true);
    }
  }

  AgentLocalWorkflowRecord record;
  record.workflow_uuid = DeterministicAgentRuntimeObjectUuidFromKey(
      "agent_local_workflow|" + std::string(AgentLocalWorkflowDomainName(request.domain)) +
      "|" + request.operation_id + "|" + request.idempotency_key + "|" +
      input_digest);
  record.domain = request.domain;
  record.operation_id = request.operation_id;
  record.subject_uuid = request.authority.subject_uuid;
  record.idempotency_key = request.idempotency_key;
  record.input_digest = input_digest;
  record.generation = records_.size() + 1;
  record.dry_run = request.dry_run;
  record.outcome_verified = request.intended_state_observed;
  record.state = request.dry_run
                     ? AgentLocalWorkflowState::prepared
                     : request.intended_state_observed
                           ? AgentLocalWorkflowState::verified
                           : AgentLocalWorkflowState::applied;
  record.verification_evidence_uuid = DeterministicAgentRuntimeObjectUuidFromKey(
      "agent_local_workflow_verification|" + record.workflow_uuid + "|" +
      (record.outcome_verified ? "observed" : "pending"));
  record.diagnostic_code =
      record.outcome_verified || request.dry_run
          ? "SB_AGENT_LOCAL_WORKFLOW.OUTCOME_VERIFIED"
          : "SB_AGENT_LOCAL_WORKFLOW.OUTCOME_PENDING_REPLAY_REQUIRED";
  const auto durable_status = AppendDurableRecord(request, record);
  if (!durable_status.ok) {
    record.state = AgentLocalWorkflowState::refused;
    record.diagnostic_code = durable_status.diagnostic_code;
    return Finish(request, durable_status, std::move(record), false);
  }
  records_.push_back(record);
  return Finish(request,
                {true, record.diagnostic_code, record.workflow_uuid},
                std::move(record),
                false);
}

AgentRuntimeStatus AgentLocalWorkflowLedger::AppendDurableRecord(
    const AgentLocalWorkflowRequest& request,
    const AgentLocalWorkflowRecord& record) {
  if (durable_catalog_ == nullptr) { return AgentOk(); }
  const auto catalog_status =
      ValidateDurableAgentCatalogForProduction(*durable_catalog_);
  if (!catalog_status.ok) { return catalog_status; }

  const std::string agent_type = AgentTypeForWorkflowDomain(record.domain);
  const std::string evidence_uuid = DeterministicAgentRuntimeObjectUuidFromKey(
      "agent_local_workflow_evidence|" + record.workflow_uuid);
  const std::string verification_uuid =
      record.verification_evidence_uuid.empty()
          ? DeterministicAgentRuntimeObjectUuidFromKey(
                "agent_local_workflow_verification|" + record.workflow_uuid)
          : record.verification_evidence_uuid;

  AgentEvidenceRecord evidence;
  evidence.evidence_uuid = evidence_uuid;
  evidence.agent_type_id = agent_type;
  evidence.instance_uuid = record.subject_uuid;
  evidence.evidence_kind = "agent_local_workflow_durable_audit_evidence";
  evidence.diagnostic_code = record.diagnostic_code;
  evidence.detail = std::string("domain=") +
                    AgentLocalWorkflowDomainName(record.domain) +
                    ";operation=" + record.operation_id;
  evidence.input_metric_digest = record.input_digest;
  evidence.policy_generation = request.authority.catalog_generation;
  evidence.principal_uuid = request.authority.principal_uuid;
  evidence.rights_used = {"OBS_AGENT_CONTROL"};
  evidence.scope_uuids = {request.authority.database_uuid, record.subject_uuid};
  evidence.decision_payload_digest = EvidenceDigestForWorkflow(request, record);
  evidence.result_state = AgentLocalWorkflowStateName(record.state);
  evidence.redaction_class = "standard";
  evidence.retention_class = "audit";
  evidence.outcome_verification_evidence_uuid = verification_uuid;
  evidence.tamper_digest_algorithm = "sha256-chain-v1";
  evidence.previous_tamper_digest =
      durable_catalog_->evidence.empty()
          ? "scratchbird-agent-evidence-ledger-genesis"
          : durable_catalog_->evidence.back().tamper_chain_digest;
  evidence.tamper_signature_algorithm = "hmac-sha256-v1";
  evidence.tamper_key_id = "agent-evidence-ledger-key-v1";
  evidence.tamper_key_provenance = "engine_local_protected_hmac_key";
  evidence.tamper_key_generation = 1;
  evidence.evidence_key_policy_id = "agent-evidence-key-policy-v1";
  evidence.tamper_key_rotation_epoch = 1;
  evidence.tamper_key_not_before_microseconds = 1;
  evidence.tamper_key_not_after_microseconds =
      request.authority.local_transaction_id + 7ull * kYearMicros;
  evidence.key_residency_class = "engine_local_protected";
  evidence.data_residency_class = "engine_local";
  evidence.storage_linkage_digest =
      Sha256(evidence.evidence_uuid + "|" +
             durable_catalog_->authority.catalog_storage_uuid + "|" +
             durable_catalog_->authority.catalog_root_digest);
  evidence.tamper_evidence_generation = 1;
  evidence.created_at_microseconds = request.authority.local_transaction_id;
  evidence.expires_at_microseconds =
      request.authority.local_transaction_id + 7ull * kYearMicros;
  evidence.legal_hold_active = false;
  evidence.production_key_material = true;
  evidence.test_key_material = false;
  evidence.key_material_exported = false;
  evidence.protected_material_suppressed = false;
  evidence.redaction_applied_before_buffering = true;
  evidence.parser_authority = false;
  evidence.client_authority = false;
  evidence.donor_authority = false;
  evidence.sidecar_authority = false;
  evidence.transaction_authority = false;
  evidence.finality_authority = false;
  evidence.visibility_authority = false;
  evidence.recovery_authority = false;
  evidence.security_authority = false;
  FinalizeCommercialAgentEvidenceDigests(&evidence);

  const auto evidence_validation = ValidateCommercialAgentEvidence(evidence);
  if (!evidence_validation.status.ok) {
    return evidence_validation.status;
  }
  durable_catalog_->evidence.push_back(evidence);

  DurableAgentActionRecord action;
  action.action_uuid = record.workflow_uuid;
  action.instance_uuid = record.subject_uuid;
  action.owner_uuid = request.authority.principal_uuid;
  action.operation_id = record.operation_id;
  action.actuator_provider_id =
      std::string("agent_local_workflow:") +
      AgentLocalWorkflowDomainName(record.domain);
  action.state = record.outcome_verified || record.dry_run
                     ? DurableAgentActionState::completed
                     : DurableAgentActionState::replay_pending;
  action.idempotency_key = record.idempotency_key;
  action.input_evidence_digest = record.input_digest;
  action.evidence_uuid = evidence_uuid;
  action.verification_evidence_uuid = verification_uuid;
  action.diagnostic_code = record.diagnostic_code;
  action.generation = record.generation == 0 ? durable_catalog_->actions.size() + 1
                                             : record.generation;
  action.outcome_verified = record.outcome_verified;
  action.compensation_required = !record.dry_run && !record.outcome_verified;
  action.compensation_attempted = false;
  action.parser_authority = false;
  action.client_authority = false;
  action.donor_authority = false;
  action.sidecar_authority = false;
  durable_catalog_->actions.push_back(std::move(action));

  DurableAgentHistoryRecord history;
  history.history_uuid = DeterministicAgentRuntimeObjectUuidFromKey(
      "agent_local_workflow_history|" + record.workflow_uuid);
  history.subject_uuid = record.subject_uuid;
  history.event_kind =
      std::string("agent_local_workflow.") +
      AgentLocalWorkflowDomainName(record.domain);
  history.diagnostic_code = record.diagnostic_code;
  history.evidence_uuid = evidence_uuid;
  history.recorded_at_microseconds =
      request.authority.local_transaction_id + durable_catalog_->retained_history.size();
  durable_catalog_->retained_history.push_back(std::move(history));

  return RefreshDurableAgentCatalogAuthorityDigest(durable_catalog_,
                                                   evidence_uuid);
}

}  // namespace scratchbird::core::agents
