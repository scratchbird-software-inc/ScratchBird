// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_replay_quarantine.hpp"

#include "agent_commercial_evidence.hpp"

#include <algorithm>
#include <iomanip>
#include <map>
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

std::string JoinVector(const std::vector<std::string>& values) {
  std::ostringstream out;
  bool first = true;
  for (const auto& value : values) {
    if (!first) { out << '|'; }
    first = false;
    out << value;
  }
  return out.str();
}

std::string JoinMap(const std::map<std::string, std::string>& values) {
  std::ostringstream out;
  bool first = true;
  for (const auto& [key, value] : values) {
    if (!first) { out << '|'; }
    first = false;
    out << key << '=' << value;
  }
  return out.str();
}

const DurableAgentActionRecord* FindAction(
    const DurableAgentCatalogImage& catalog,
    const std::string& action_uuid) {
  for (const auto& action : catalog.actions) {
    if (action.action_uuid == action_uuid) { return &action; }
  }
  return nullptr;
}

DurableAgentActionRecord* FindMutableAction(DurableAgentCatalogImage* catalog,
                                            const std::string& action_uuid) {
  if (catalog == nullptr) { return nullptr; }
  for (auto& action : catalog->actions) {
    if (action.action_uuid == action_uuid) { return &action; }
  }
  return nullptr;
}

const AgentInstanceRecord* FindInstance(const DurableAgentCatalogImage& catalog,
                                        const DurableAgentActionRecord& action) {
  for (const auto& instance : catalog.instances) {
    if (instance.instance_uuid == action.instance_uuid) { return &instance; }
  }
  return nullptr;
}

const AgentPolicy* FindPolicy(const DurableAgentCatalogImage& catalog,
                              const AgentInstanceRecord& instance) {
  for (const auto& policy : catalog.policies) {
    if (policy.policy_uuid == instance.policy_uuid &&
        policy.policy_generation == instance.policy_generation) {
      return &policy;
    }
  }
  return nullptr;
}

const AgentEvidenceRecord* FindEvidence(const DurableAgentCatalogImage& catalog,
                                        const DurableAgentActionRecord& action) {
  for (const auto& evidence : catalog.evidence) {
    if (evidence.evidence_uuid == action.evidence_uuid) { return &evidence; }
  }
  return nullptr;
}

const DurableAgentResourceReservationRecord* FindReservation(
    const DurableAgentCatalogImage& catalog,
    const DurableAgentActionRecord& action) {
  const std::string key_fragment =
      action.idempotency_key.empty() ? action.action_uuid : action.idempotency_key;
  for (const auto& reservation : catalog.resource_reservations) {
    if (reservation.operation_id != action.operation_id) { continue; }
    if (reservation.reservation_key.find(key_fragment) == std::string::npos) {
      continue;
    }
    return &reservation;
  }
  return nullptr;
}

DurableAgentReplayState StateForOperation(AgentReplayOperationKind operation) {
  switch (operation) {
    case AgentReplayOperationKind::mark_replay_pending:
      return DurableAgentReplayState::replay_pending;
    case AgentReplayOperationKind::schedule_retry:
      return DurableAgentReplayState::retry_scheduled;
    case AgentReplayOperationKind::record_compensation:
      return DurableAgentReplayState::compensated;
    case AgentReplayOperationKind::quarantine:
      return DurableAgentReplayState::quarantined;
    case AgentReplayOperationKind::release_quarantine:
      return DurableAgentReplayState::quarantine_released;
  }
  return DurableAgentReplayState::none;
}

std::string ReplayUuidFor(const std::string& action_uuid,
                          AgentReplayOperationKind operation,
                          const AgentReplayDigestCapture& capture) {
  return DeterministicAgentRuntimeObjectUuidFromKey(
      "agent_replay_control|" + action_uuid + "|" +
      AgentReplayOperationKindName(operation) + "|" +
      capture.policy_digest + "|" + capture.metric_digest + "|" +
      capture.catalog_root_digest + "|" + capture.security_digest + "|" +
      capture.resource_reservation_digest + "|" +
      capture.binary_package_digest + "|" +
      capture.action_record_digest + "|" + capture.evidence_chain_digest);
}

bool ReplayAuthorityClean(const AgentReplayControlRequest& request) {
  return !request.parser_authority && !request.client_authority &&
         !request.reference_authority && !request.wal_authority &&
         !request.benchmark_authority && !request.optimizer_plan_authority &&
         !request.index_finality_authority &&
         !request.provider_finality_authority && !request.memory_authority &&
         !request.agent_action_authority;
}

bool ReplayRecordMatches(const DurableAgentReplayRecord& record,
                         const AgentReplayControlRequest& request,
                         const std::string& replay_uuid) {
  return record.replay_uuid == replay_uuid &&
         record.action_uuid == request.action_uuid &&
         record.policy_digest == request.capture.policy_digest &&
         record.policy_generation == request.capture.policy_generation &&
         record.metric_digest == request.capture.metric_digest &&
         record.catalog_root_digest == request.capture.catalog_root_digest &&
         record.security_digest == request.capture.security_digest &&
         record.security_epoch == request.capture.security_epoch &&
         record.resource_reservation_digest ==
             request.capture.resource_reservation_digest &&
         record.binary_package_digest == request.capture.binary_package_digest &&
         record.action_input_digest == request.capture.action_input_digest &&
         record.action_evidence_digest ==
             request.capture.action_evidence_digest &&
         record.action_record_digest == request.capture.action_record_digest &&
         record.evidence_chain_digest == request.capture.evidence_chain_digest;
}

AgentRuntimeStatus RequiredCaptureFieldsPresent(
    const AgentReplayDigestCapture& capture) {
  if (capture.policy_digest.empty() || capture.policy_generation == 0 ||
      capture.metric_digest.empty() || capture.catalog_root_digest.empty() ||
      capture.security_digest.empty() || capture.security_epoch == 0 ||
      capture.resource_reservation_digest.empty() ||
      capture.binary_package_digest.empty() ||
      capture.action_input_digest.empty() ||
      capture.action_evidence_digest.empty() ||
      capture.action_record_digest.empty() ||
      capture.evidence_chain_digest.empty()) {
    return AgentError("SB_AGENT_REPLAY.DIGEST_CAPTURE_REQUIRED");
  }
  return AgentOk();
}

AgentRuntimeStatus ValidateCaptureAgainstCatalog(
    const DurableAgentCatalogImage& catalog,
    const DurableAgentActionRecord& action,
    const AgentReplayControlRequest& request) {
  const auto required = RequiredCaptureFieldsPresent(request.capture);
  if (!required.ok) { return required; }
  if (request.cluster_route_requested &&
      !request.external_cluster_provider_attested) {
    return AgentError("SB_AGENT_REPLAY.CLUSTER_EXTERNAL_PROVIDER_REQUIRED",
                      request.action_uuid);
  }
  if (!ReplayAuthorityClean(request)) {
    return AgentError("SB_AGENT_REPLAY.AUTHORITY_FLAG_FORBIDDEN",
                      request.action_uuid);
  }
  if (request.capture.catalog_root_digest !=
      catalog.authority.catalog_root_digest) {
    return AgentError("SB_AGENT_REPLAY.CATALOG_DIGEST_STALE",
                      request.action_uuid);
  }

  AgentReplayDigestCaptureRequest capture_request;
  capture_request.catalog = &catalog;
  capture_request.action_uuid = request.action_uuid;
  capture_request.security_epoch = request.capture.security_epoch;
  capture_request.package_provenance = request.package_provenance;
  capture_request.production_live_path = request.production_live_path;
  const auto actual = CaptureAgentReplayDigests(capture_request);
  if (!actual.status.ok) { return actual.status; }
  if (actual.capture.policy_digest != request.capture.policy_digest ||
      actual.capture.policy_generation != request.capture.policy_generation) {
    return AgentError("SB_AGENT_REPLAY.POLICY_DIGEST_STALE",
                      request.action_uuid);
  }
  if (actual.capture.metric_digest != request.capture.metric_digest) {
    return AgentError("SB_AGENT_REPLAY.METRIC_DIGEST_STALE",
                      request.action_uuid);
  }
  if (actual.capture.security_digest != request.capture.security_digest) {
    return AgentError("SB_AGENT_REPLAY.SECURITY_DIGEST_STALE",
                      request.action_uuid);
  }
  if (actual.capture.resource_reservation_digest !=
      request.capture.resource_reservation_digest) {
    return AgentError("SB_AGENT_REPLAY.RESOURCE_DIGEST_STALE",
                      request.action_uuid);
  }
  if (actual.capture.binary_package_digest !=
      request.capture.binary_package_digest) {
    return AgentError("SB_AGENT_REPLAY.BINARY_PACKAGE_DIGEST_STALE",
                      request.action_uuid);
  }
  if (actual.capture.action_input_digest != request.capture.action_input_digest ||
      actual.capture.action_evidence_digest !=
          request.capture.action_evidence_digest ||
      actual.capture.action_record_digest !=
          request.capture.action_record_digest ||
      actual.capture.evidence_chain_digest !=
          request.capture.evidence_chain_digest) {
    return AgentError("SB_AGENT_REPLAY.ACTION_EVIDENCE_DIGEST_STALE",
                      action.action_uuid);
  }
  return AgentOk();
}

void AppendReplayHistory(DurableAgentCatalogImage* catalog,
                         const DurableAgentReplayRecord& replay) {
  DurableAgentHistoryRecord history;
  history.history_uuid =
      replay.replay_uuid + ":replay:" +
      std::to_string(catalog->retained_history.size() + 1);
  history.subject_uuid = replay.action_uuid;
  history.event_kind =
      std::string("agent_replay_") + DurableAgentReplayStateName(replay.state);
  history.diagnostic_code = replay.diagnostic_code;
  history.evidence_uuid = replay.evidence_uuid;
  history.recorded_at_microseconds = replay.recorded_at_microseconds;
  catalog->retained_history.push_back(std::move(history));
}

}  // namespace

const char* AgentReplayOperationKindName(AgentReplayOperationKind kind) {
  switch (kind) {
    case AgentReplayOperationKind::mark_replay_pending:
      return "mark_replay_pending";
    case AgentReplayOperationKind::schedule_retry: return "schedule_retry";
    case AgentReplayOperationKind::record_compensation:
      return "record_compensation";
    case AgentReplayOperationKind::quarantine: return "quarantine";
    case AgentReplayOperationKind::release_quarantine:
      return "release_quarantine";
  }
  return "mark_replay_pending";
}

std::string AgentReplayPolicyDigest(const DurableAgentCatalogImage& catalog,
                                    const DurableAgentActionRecord& action) {
  const auto* instance = FindInstance(catalog, action);
  if (instance == nullptr || instance->policy_generation == 0 ||
      instance->policy_uuid.empty()) {
    return {};
  }
  const auto* policy = FindPolicy(catalog, *instance);
  if (policy == nullptr) { return {}; }
  std::ostringstream payload;
  payload << "agent_replay_policy_digest_v1\n"
          << instance->instance_uuid << '\n'
          << instance->agent_type_id << '\n'
          << instance->policy_uuid << '\n'
          << instance->scope << '\n'
          << instance->policy_generation << '\n'
          << policy->policy_uuid << '\n'
          << policy->policy_family << '\n'
          << policy->scope << '\n'
          << policy->action_mode << '\n'
          << AgentActivationProfileName(policy->activation) << '\n'
          << policy->policy_generation << '\n'
          << (policy->enabled ? "1" : "0") << '\n'
          << (policy->allow_live_action ? "1" : "0") << '\n'
          << (policy->require_manual_approval ? "1" : "0") << '\n'
          << (policy->require_dry_run_before_live ? "1" : "0") << '\n'
          << JoinVector(policy->required_metric_families) << '\n'
          << JoinVector(policy->policy_dependencies) << '\n'
          << JoinMap(policy->config_fields) << '\n';
  for (const auto& attachment : catalog.attachments) {
    if (attachment.agent_type_id == instance->agent_type_id &&
        attachment.policy_uuid == instance->policy_uuid &&
        attachment.policy_generation == instance->policy_generation &&
        attachment.active && attachment.valid) {
      payload << attachment.attachment_uuid << '\n'
              << attachment.policy_family << '\n'
              << attachment.scope << '\n'
              << attachment.attachment_generation << '\n'
              << attachment.evidence_uuid << '\n';
    }
  }
  return Sha256Digest(payload.str());
}

std::string AgentReplayResourceReservationDigest(
    const DurableAgentCatalogImage& catalog,
    const DurableAgentActionRecord& action) {
  const auto* reservation = FindReservation(catalog, action);
  if (reservation == nullptr || reservation->evidence_uuid.empty()) { return {}; }
  std::ostringstream payload;
  payload << "agent_replay_resource_reservation_digest_v1\n"
          << reservation->reservation_uuid << '\n'
          << reservation->reservation_key << '\n'
          << reservation->owner_scope << '\n'
          << reservation->agent_type_id << '\n'
          << reservation->operation_id << '\n'
          << DurableAgentResourceReservationStateName(reservation->state) << '\n'
          << reservation->memory_bytes << '\n'
          << reservation->worker_slots << '\n'
          << reservation->overhead_microseconds << '\n'
          << reservation->evidence_uuid << '\n'
          << reservation->release_evidence_uuid << '\n'
          << reservation->release_reason << '\n'
          << (reservation->parser_authority ? "1" : "0") << '\n'
          << (reservation->client_authority ? "1" : "0") << '\n'
          << (reservation->reference_authority ? "1" : "0") << '\n'
          << (reservation->benchmark_authority ? "1" : "0") << '\n';
  return Sha256Digest(payload.str());
}

std::string AgentReplaySecurityDigest(const AgentEvidenceRecord& evidence,
                                      u64 security_epoch) {
  if (security_epoch == 0 || evidence.principal_uuid.empty() ||
      evidence.rights_used.empty() || evidence.scope_uuids.empty()) {
    return {};
  }
  std::ostringstream payload;
  payload << "agent_replay_security_digest_v1\n"
          << evidence.principal_uuid << '\n'
          << JoinVector(evidence.rights_used) << '\n'
          << JoinVector(evidence.scope_uuids) << '\n'
          << security_epoch << '\n'
          << evidence.evidence_uuid << '\n';
  return Sha256Digest(payload.str());
}

std::string AgentDurableActionRecordDigest(
    const DurableAgentActionRecord& action) {
  std::ostringstream payload;
  payload << "agent_durable_action_record_digest_v1\n"
          << action.action_uuid << '\n'
          << action.instance_uuid << '\n'
          << action.owner_uuid << '\n'
          << action.operation_id << '\n'
          << action.actuator_provider_id << '\n'
          << DurableAgentActionStateName(action.state) << '\n'
          << action.idempotency_key << '\n'
          << action.input_evidence_digest << '\n'
          << action.evidence_uuid << '\n'
          << action.verification_evidence_uuid << '\n'
          << action.diagnostic_code << '\n'
          << action.generation << '\n'
          << action.retry_count << '\n'
          << (action.outcome_verified ? "1" : "0") << '\n'
          << (action.compensation_required ? "1" : "0") << '\n'
          << (action.compensation_attempted ? "1" : "0") << '\n'
          << (action.retry_scheduled ? "1" : "0") << '\n'
          << action.retry_after_microseconds << '\n'
          << action.retry_evidence_uuid << '\n'
          << action.compensation_executor_id << '\n'
          << action.compensation_evidence_uuid << '\n'
          << (action.parser_authority ? "1" : "0") << '\n'
          << (action.client_authority ? "1" : "0") << '\n'
          << (action.reference_authority ? "1" : "0") << '\n'
          << (action.sidecar_authority ? "1" : "0") << '\n';
  return Sha256Digest(payload.str());
}

std::string AgentReplayEvidenceDigest(const AgentEvidenceRecord& evidence) {
  if (evidence.evidence_uuid.empty() || evidence.tamper_chain_digest.empty()) {
    return {};
  }
  std::ostringstream payload;
  payload << "agent_replay_evidence_digest_v1\n"
          << evidence.evidence_uuid << '\n'
          << evidence.agent_type_id << '\n'
          << evidence.instance_uuid << '\n'
          << evidence.input_metric_digest << '\n'
          << evidence.policy_generation << '\n'
          << evidence.decision_payload_digest << '\n'
          << evidence.result_state << '\n'
          << evidence.outcome_verification_evidence_uuid << '\n'
          << evidence.tamper_digest << '\n'
          << evidence.tamper_chain_digest << '\n'
          << evidence.tamper_signature << '\n'
          << evidence.storage_linkage_digest << '\n';
  return Sha256Digest(payload.str());
}

std::string AgentReplayRecordDigest(const DurableAgentReplayRecord& replay) {
  std::ostringstream payload;
  payload << "agent_replay_record_digest_v1\n"
          << replay.replay_uuid << '\n'
          << replay.action_uuid << '\n'
          << replay.instance_uuid << '\n'
          << replay.operation_id << '\n'
          << DurableAgentReplayStateName(replay.state) << '\n'
          << replay.replay_generation << '\n'
          << replay.retry_count << '\n'
          << replay.policy_digest << '\n'
          << replay.metric_digest << '\n'
          << replay.catalog_root_digest << '\n'
          << replay.security_digest << '\n'
          << replay.resource_reservation_digest << '\n'
          << replay.binary_package_digest << '\n'
          << replay.action_input_digest << '\n'
          << replay.action_evidence_digest << '\n'
          << replay.action_record_digest << '\n'
          << replay.evidence_chain_digest << '\n'
          << replay.evidence_uuid << '\n'
          << replay.review_evidence_uuid << '\n'
          << replay.compensation_evidence_uuid << '\n'
          << replay.diagnostic_code << '\n';
  return Sha256Digest(payload.str());
}

AgentReplayDigestCaptureResult CaptureAgentReplayDigests(
    const AgentReplayDigestCaptureRequest& request) {
  AgentReplayDigestCaptureResult result;
  if (request.catalog == nullptr) {
    result.status = AgentError("SB_AGENT_REPLAY.CATALOG_REQUIRED");
    return result;
  }
  if (request.production_live_path) {
    const auto catalog_status =
        ValidateDurableAgentCatalogForProduction(*request.catalog);
    if (!catalog_status.ok) {
      result.status = catalog_status;
      return result;
    }
  }
  const auto* action = FindAction(*request.catalog, request.action_uuid);
  if (action == nullptr) {
    result.status = AgentError("SB_AGENT_REPLAY.ACTION_RECORD_REQUIRED",
                               request.action_uuid);
    return result;
  }
  const auto* evidence = FindEvidence(*request.catalog, *action);
  if (evidence == nullptr) {
    result.status = AgentError("SB_AGENT_REPLAY.ACTION_EVIDENCE_REQUIRED",
                               request.action_uuid);
    return result;
  }
  const auto* instance = FindInstance(*request.catalog, *action);
  if (instance == nullptr || instance->policy_generation == 0 ||
      evidence->policy_generation != instance->policy_generation) {
    result.status = AgentError("SB_AGENT_REPLAY.POLICY_GENERATION_STALE",
                               request.action_uuid);
    return result;
  }
  const auto evidence_status = ValidateCommercialAgentEvidence(*evidence);
  if (!evidence_status.status.ok) {
    result.status = evidence_status.status;
    return result;
  }
  const auto package = ValidateAgentPackageProvenanceBundle(
      request.package_provenance);
  if (!package.accepted) {
    result.status = AgentError(package.status.diagnostic_code,
                               package.status.detail);
    return result;
  }
  result.capture.policy_digest =
      AgentReplayPolicyDigest(*request.catalog, *action);
  result.capture.policy_generation = instance->policy_generation;
  result.capture.metric_digest = evidence->input_metric_digest;
  result.capture.catalog_root_digest =
      request.catalog->authority.catalog_root_digest;
  result.capture.security_digest =
      AgentReplaySecurityDigest(*evidence, request.security_epoch);
  result.capture.security_epoch = request.security_epoch;
  result.capture.resource_reservation_digest =
      AgentReplayResourceReservationDigest(*request.catalog, *action);
  result.capture.binary_package_digest = package.bundle_digest;
  result.capture.action_input_digest = action->input_evidence_digest;
  result.capture.action_evidence_digest = AgentReplayEvidenceDigest(*evidence);
  result.capture.action_record_digest = AgentDurableActionRecordDigest(*action);
  result.capture.evidence_chain_digest = evidence->tamper_chain_digest;

  const auto required = RequiredCaptureFieldsPresent(result.capture);
  if (!required.ok) {
    result.status = required;
    return result;
  }
  result.status = {true, "SB_AGENT_REPLAY.DIGEST_CAPTURED",
                   request.action_uuid};
  return result;
}

AgentReplayControlResult ApplyAgentReplayControl(
    const AgentReplayControlRequest& request) {
  AgentReplayControlResult result;
  if (request.catalog == nullptr) {
    result.status = AgentError("SB_AGENT_REPLAY.CATALOG_REQUIRED");
    return result;
  }
  const std::string replay_uuid =
      ReplayUuidFor(request.action_uuid, request.operation, request.capture);
  for (const auto& replay : request.catalog->replay_records) {
    if (ReplayRecordMatches(replay, request, replay_uuid)) {
      result.status = {true, "SB_AGENT_REPLAY.IDEMPOTENT_REPLAY",
                       replay.replay_uuid};
      result.replay_record = replay;
      if (const auto* action = FindAction(*request.catalog,
                                          request.action_uuid)) {
        result.action_record = *action;
      }
      result.idempotent = true;
      return result;
    }
  }

  if (request.production_live_path) {
    const auto catalog_status =
        ValidateDurableAgentCatalogForProduction(*request.catalog);
    if (!catalog_status.ok) {
      result.status = catalog_status;
      return result;
    }
  }
  if (request.evidence_uuid.empty() || request.now_microseconds == 0) {
    result.status = AgentError("SB_AGENT_REPLAY.EVIDENCE_REQUIRED",
                               request.action_uuid);
    return result;
  }
  DurableAgentActionRecord* action =
      FindMutableAction(request.catalog, request.action_uuid);
  if (action == nullptr) {
    result.status = AgentError("SB_AGENT_REPLAY.ACTION_RECORD_REQUIRED",
                               request.action_uuid);
    return result;
  }
  const auto validation =
      ValidateCaptureAgainstCatalog(*request.catalog, *action, request);
  if (!validation.ok) {
    result.status = validation;
    return result;
  }
  if (request.operation == AgentReplayOperationKind::schedule_retry &&
      request.max_retry_count != 0 &&
      action->retry_count >= request.max_retry_count) {
    result.status = AgentError("SB_AGENT_REPLAY.RETRY_LIMIT_EXCEEDED",
                               request.action_uuid);
    return result;
  }
  if (request.operation == AgentReplayOperationKind::record_compensation &&
      request.compensation_evidence_uuid.empty()) {
    result.status = AgentError("SB_AGENT_REPLAY.COMPENSATION_EVIDENCE_REQUIRED",
                               request.action_uuid);
    return result;
  }
  if (request.operation == AgentReplayOperationKind::release_quarantine &&
      request.review_evidence_uuid.empty()) {
    result.status = AgentError("SB_AGENT_REPLAY.QUARANTINE_REVIEW_REQUIRED",
                               request.action_uuid);
    return result;
  }

  DurableAgentReplayRecord replay;
  replay.replay_uuid = replay_uuid;
  replay.action_uuid = action->action_uuid;
  replay.instance_uuid = action->instance_uuid;
  replay.operation_id = action->operation_id;
  replay.idempotency_key = action->idempotency_key;
  replay.state = StateForOperation(request.operation);
  replay.replay_generation = request.catalog->replay_records.size() + 1;
  replay.retry_count = action->retry_count;
  replay.max_retry_count = request.max_retry_count;
  replay.retry_after_microseconds = request.retry_after_microseconds;
  replay.recorded_at_microseconds = request.now_microseconds;
  replay.policy_digest = request.capture.policy_digest;
  replay.policy_generation = request.capture.policy_generation;
  replay.metric_digest = request.capture.metric_digest;
  replay.catalog_root_digest = request.capture.catalog_root_digest;
  replay.security_digest = request.capture.security_digest;
  replay.security_epoch = request.capture.security_epoch;
  replay.resource_reservation_digest =
      request.capture.resource_reservation_digest;
  replay.binary_package_digest = request.capture.binary_package_digest;
  replay.action_input_digest = request.capture.action_input_digest;
  replay.action_evidence_digest = request.capture.action_evidence_digest;
  replay.action_record_digest = request.capture.action_record_digest;
  replay.evidence_chain_digest = request.capture.evidence_chain_digest;
  replay.evidence_uuid = request.evidence_uuid;
  replay.review_evidence_uuid = request.review_evidence_uuid;
  replay.compensation_evidence_uuid = request.compensation_evidence_uuid;
  replay.diagnostic_code =
      std::string("SB_AGENT_REPLAY.") +
      DurableAgentReplayStateName(replay.state);
  replay.cluster_route_requested = request.cluster_route_requested;
  replay.external_cluster_provider_attested =
      request.external_cluster_provider_attested;
  replay.parser_authority = request.parser_authority;
  replay.client_authority = request.client_authority;
  replay.reference_authority = request.reference_authority;
  replay.wal_authority = request.wal_authority;
  replay.benchmark_authority = request.benchmark_authority;
  replay.optimizer_plan_authority = request.optimizer_plan_authority;
  replay.index_finality_authority = request.index_finality_authority;
  replay.provider_finality_authority = request.provider_finality_authority;
  replay.memory_authority = request.memory_authority;
  replay.agent_action_authority = request.agent_action_authority;

  switch (request.operation) {
    case AgentReplayOperationKind::mark_replay_pending:
      action->state = DurableAgentActionState::replay_pending;
      action->retry_scheduled = false;
      break;
    case AgentReplayOperationKind::schedule_retry:
      action->state = DurableAgentActionState::replay_pending;
      action->retry_scheduled = true;
      action->retry_after_microseconds = request.retry_after_microseconds;
      action->retry_evidence_uuid = request.evidence_uuid;
      ++action->retry_count;
      replay.retry_scheduled = true;
      replay.retry_count = action->retry_count;
      result.retry_scheduled = true;
      break;
    case AgentReplayOperationKind::record_compensation:
      action->state = DurableAgentActionState::quarantined;
      action->compensation_required = true;
      action->compensation_attempted = true;
      action->compensation_evidence_uuid = request.compensation_evidence_uuid;
      replay.compensation_required = true;
      replay.compensation_attempted = true;
      result.compensation_recorded = true;
      break;
    case AgentReplayOperationKind::quarantine:
      action->state = DurableAgentActionState::quarantined;
      replay.review_required = true;
      break;
    case AgentReplayOperationKind::release_quarantine:
      action->state = DurableAgentActionState::replay_pending;
      action->retry_scheduled = false;
      replay.review_required = true;
      replay.review_approved = true;
      result.quarantine_released = true;
      break;
  }
  ++action->generation;
  result.action_state_updated = true;
  result.action_record = *action;
  result.replay_record = replay;
  request.catalog->replay_records.push_back(replay);
  AppendReplayHistory(request.catalog, replay);
  const auto refreshed =
      RefreshDurableAgentCatalogAuthorityDigest(request.catalog,
                                                request.evidence_uuid);
  if (!refreshed.ok) {
    result.status = refreshed;
    return result;
  }
  result.replay_record_written = true;
  result.status = {true, "SB_AGENT_REPLAY.APPLIED", replay.replay_uuid};
  return result;
}

}  // namespace scratchbird::core::agents
