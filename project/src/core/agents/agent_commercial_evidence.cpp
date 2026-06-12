// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_commercial_evidence.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iomanip>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <set>
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

std::string Sha256Digest(const std::string& payload) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(payload.data()),
         payload.size(),
         digest);
  return HexBytes(digest, SHA256_DIGEST_LENGTH);
}

std::string HmacSha256Digest(const std::string& key, const std::string& payload) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_size = 0;
  HMAC(EVP_sha256(),
       key.data(),
       static_cast<int>(key.size()),
       reinterpret_cast<const unsigned char*>(payload.data()),
       payload.size(),
       digest,
       &digest_size);
  return HexBytes(digest, digest_size);
}

bool ContainsToken(const std::string& value, const std::string& token) {
  std::string value_lower = value;
  std::string token_lower = token;
  std::transform(value_lower.begin(), value_lower.end(), value_lower.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  std::transform(token_lower.begin(), token_lower.end(), token_lower.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return value_lower.find(token_lower) != std::string::npos;
}

bool IsProductionKeyProvenance(const std::string& provenance) {
  return provenance == "engine_local_protected_hmac_key" ||
         provenance == "engine_hsm_protected_hmac_key" ||
         provenance == "engine_kms_protected_hmac_key";
}

bool KeyMaterialPolicySafe(const AgentEvidenceRecord& evidence) {
  return !evidence.tamper_key_id.empty() &&
         !evidence.tamper_key_provenance.empty() &&
         evidence.tamper_key_generation != 0 &&
         evidence.tamper_key_rotation_epoch != 0 &&
         evidence.tamper_key_not_before_microseconds != 0 &&
         evidence.tamper_key_not_after_microseconds != 0 &&
         evidence.production_key_material && !evidence.test_key_material &&
         !evidence.key_material_exported &&
         IsProductionKeyProvenance(evidence.tamper_key_provenance) &&
         !ContainsToken(evidence.tamper_key_id, "test") &&
         !ContainsToken(evidence.tamper_key_id, "fixture") &&
         !ContainsToken(evidence.tamper_key_provenance, "test") &&
         !ContainsToken(evidence.tamper_key_provenance, "fixture");
}

std::string EvidenceHmacKeyMaterial(const AgentEvidenceRecord& evidence) {
  if (KeyMaterialPolicySafe(evidence)) {
    return Sha256Digest(
        "scratchbird-agent-evidence-protected-hmac-material-v2|" +
        evidence.evidence_key_policy_id + "|" + evidence.tamper_key_id + "|" +
        evidence.tamper_key_provenance + "|" +
        std::to_string(evidence.tamper_key_generation) + "|" +
        std::to_string(evidence.tamper_key_rotation_epoch) + "|" +
        evidence.key_residency_class);
  }
  return {};
}

std::string Join(const std::vector<std::string>& values) {
  std::ostringstream out;
  bool first = true;
  for (const auto& value : values) {
    if (!first) { out << '|'; }
    first = false;
    out << value;
  }
  return out.str();
}

bool HasRight(const AgentRuntimeContext& context, const std::string& right) {
  return AgentContextHasRight(context, right);
}

bool IsProtectedRedactionClass(const std::string& redaction_class) {
  return redaction_class == "protected_material" ||
         redaction_class == "secret" ||
         redaction_class == "security_sensitive";
}

bool IsRestrictedRedactionClass(const std::string& redaction_class) {
  return redaction_class == "restricted" ||
         IsProtectedRedactionClass(redaction_class);
}

u64 RetentionExpiresAt(const std::string& retention_class,
                       u64 created_at_microseconds) {
  if (retention_class == "short") { return created_at_microseconds + kDayMicros; }
  if (retention_class == "operational") {
    return created_at_microseconds + 90ull * kDayMicros;
  }
  if (retention_class == "audit") {
    return created_at_microseconds + 7ull * kYearMicros;
  }
  if (retention_class == "legal_hold") { return 0; }
  return created_at_microseconds + 90ull * kDayMicros;
}

std::string InputMetricDigestFromAction(const AgentActionRequest& action) {
  const auto it = action.inputs.find("metric_digest");
  return it == action.inputs.end() ? std::string() : it->second;
}

std::vector<std::string> ScopeUuidsForRequest(
    const CommercialAgentEvidenceBuildRequest& request) {
  std::vector<std::string> scopes = request.scope_uuids;
  if (!request.authority.scope_uuid.empty()) {
    scopes.push_back(request.authority.scope_uuid);
  }
  const auto it = request.action.inputs.find("scope_uuid");
  if (it != request.action.inputs.end() && !it->second.empty()) {
    scopes.push_back(it->second);
  }
  std::sort(scopes.begin(), scopes.end());
  scopes.erase(std::unique(scopes.begin(), scopes.end()), scopes.end());
  return scopes;
}

void SuppressProtectedMaterial(AgentEvidenceRecord* evidence) {
  evidence->detail = "redacted";
  evidence->principal_uuid.clear();
  evidence->rights_used.clear();
  evidence->scope_uuids.clear();
  evidence->input_metric_digest.clear();
  evidence->decision_payload_digest.clear();
  evidence->outcome_verification_evidence_uuid.clear();
  evidence->protected_material_suppressed = true;
}

void RedactBeforeBuffering(AgentEvidenceRecord* evidence) {
  if (evidence == nullptr) { return; }
  evidence->redaction_applied_before_buffering = true;
  if (IsProtectedRedactionClass(evidence->redaction_class)) {
    evidence->detail = "redacted:protected-material-suppressed";
    evidence->protected_material_suppressed = true;
  }
}

bool AuthorityFlagsClean(const AgentEvidenceRecord& evidence) {
  return !evidence.parser_authority && !evidence.client_authority &&
         !evidence.reference_authority && !evidence.sidecar_authority &&
         !evidence.transaction_authority && !evidence.finality_authority &&
         !evidence.visibility_authority && !evidence.recovery_authority &&
         !evidence.security_authority;
}

}  // namespace

std::string CommercialAgentEvidenceChainDigest(
    const AgentEvidenceRecord& evidence) {
  std::ostringstream payload;
  payload << "commercial_agent_evidence_chain_v1\n"
          << evidence.previous_tamper_digest << '\n'
          << evidence.tamper_digest << '\n'
          << evidence.storage_linkage_digest << '\n'
          << evidence.evidence_uuid << '\n'
          << evidence.tamper_key_id << '\n'
          << evidence.tamper_key_provenance << '\n'
          << evidence.tamper_key_generation << '\n'
          << evidence.evidence_key_policy_id << '\n'
          << evidence.tamper_key_rotation_epoch << '\n'
          << evidence.key_residency_class << '\n'
          << evidence.data_residency_class << '\n'
          << evidence.tamper_signature_algorithm << '\n';
  return Sha256Digest(payload.str());
}

std::string CommercialAgentEvidenceTamperDigest(
    const AgentEvidenceRecord& evidence) {
  std::ostringstream payload;
  payload << "commercial_agent_evidence_v1\n"
          << evidence.evidence_uuid << '\n'
          << evidence.agent_type_id << '\n'
          << evidence.instance_uuid << '\n'
          << evidence.evidence_kind << '\n'
          << evidence.diagnostic_code << '\n'
          << evidence.detail << '\n'
          << evidence.input_metric_digest << '\n'
          << evidence.policy_generation << '\n'
          << evidence.principal_uuid << '\n'
          << Join(evidence.rights_used) << '\n'
          << Join(evidence.scope_uuids) << '\n'
          << evidence.decision_payload_digest << '\n'
          << evidence.result_state << '\n'
          << evidence.redaction_class << '\n'
          << evidence.retention_class << '\n'
          << evidence.outcome_verification_evidence_uuid << '\n'
          << evidence.tamper_digest_algorithm << '\n'
          << evidence.previous_tamper_digest << '\n'
          << evidence.tamper_signature_algorithm << '\n'
          << evidence.tamper_key_id << '\n'
          << evidence.tamper_key_provenance << '\n'
          << evidence.tamper_key_generation << '\n'
          << evidence.evidence_key_policy_id << '\n'
          << evidence.tamper_key_rotation_epoch << '\n'
          << evidence.tamper_key_not_before_microseconds << '\n'
          << evidence.tamper_key_not_after_microseconds << '\n'
          << evidence.key_residency_class << '\n'
          << evidence.data_residency_class << '\n'
          << evidence.storage_linkage_digest << '\n'
          << evidence.tamper_evidence_generation << '\n'
          << evidence.created_at_microseconds << '\n'
          << evidence.expires_at_microseconds << '\n'
          << (evidence.protected_material_suppressed ? "1" : "0") << '\n'
          << (evidence.redaction_applied_before_buffering ? "1" : "0") << '\n'
          << (evidence.legal_hold_active ? "1" : "0") << '\n'
          << (evidence.production_key_material ? "1" : "0") << '\n'
          << (evidence.test_key_material ? "1" : "0") << '\n'
          << (evidence.key_material_exported ? "1" : "0") << '\n'
          << (evidence.parser_authority ? "1" : "0") << '\n'
          << (evidence.client_authority ? "1" : "0") << '\n'
          << (evidence.reference_authority ? "1" : "0") << '\n'
          << (evidence.sidecar_authority ? "1" : "0") << '\n'
          << (evidence.transaction_authority ? "1" : "0") << '\n'
          << (evidence.finality_authority ? "1" : "0") << '\n'
          << (evidence.visibility_authority ? "1" : "0") << '\n'
          << (evidence.recovery_authority ? "1" : "0") << '\n'
          << (evidence.security_authority ? "1" : "0") << '\n';
  return Sha256Digest(payload.str());
}

std::string CommercialAgentEvidenceSignatureDigest(
    const AgentEvidenceRecord& evidence) {
  const std::string key_material = EvidenceHmacKeyMaterial(evidence);
  if (key_material.empty()) { return {}; }
  std::ostringstream payload;
  payload << "commercial_agent_evidence_signature_v1\n"
          << evidence.evidence_uuid << '\n'
          << evidence.tamper_chain_digest << '\n'
          << evidence.storage_linkage_digest << '\n'
          << evidence.tamper_key_id << '\n'
          << evidence.tamper_key_provenance << '\n'
          << evidence.tamper_key_generation << '\n';
  return HmacSha256Digest(key_material, payload.str());
}

void FinalizeCommercialAgentEvidenceDigests(AgentEvidenceRecord* evidence) {
  if (evidence == nullptr) { return; }
  evidence->tamper_digest = CommercialAgentEvidenceTamperDigest(*evidence);
  evidence->tamper_chain_digest =
      CommercialAgentEvidenceChainDigest(*evidence);
  evidence->tamper_signature =
      CommercialAgentEvidenceSignatureDigest(*evidence);
}

bool CommercialAgentEvidenceExpired(const AgentEvidenceRecord& evidence,
                                    u64 now_microseconds) {
  return now_microseconds != 0 && evidence.expires_at_microseconds != 0 &&
         now_microseconds > evidence.expires_at_microseconds &&
         evidence.retention_class != "legal_hold";
}

CommercialAgentEvidenceKeyPolicy DefaultCommercialAgentEvidenceKeyPolicy() {
  return CommercialAgentEvidenceKeyPolicy{};
}

AgentRuntimeStatus ValidateCommercialAgentEvidenceKeyPolicy(
    const AgentEvidenceRecord& evidence,
    const CommercialAgentEvidenceKeyPolicy& key_policy,
    bool production_live_path) {
  if (evidence.evidence_key_policy_id.empty() ||
      evidence.evidence_key_policy_id != key_policy.policy_id ||
      key_policy.policy_generation == 0) {
    return AgentError("SB_AGENT_COMMERCIAL_EVIDENCE.KEY_POLICY_MISMATCH",
                      evidence.evidence_uuid);
  }
  if (evidence.tamper_key_id != key_policy.key_id ||
      evidence.tamper_key_provenance != key_policy.key_provenance ||
      evidence.tamper_key_generation != key_policy.key_generation ||
      evidence.tamper_key_rotation_epoch != key_policy.key_rotation_epoch) {
    return AgentError("SB_AGENT_COMMERCIAL_EVIDENCE.KEY_DESCRIPTOR_MISMATCH",
                      evidence.evidence_uuid);
  }
  if (evidence.tamper_key_not_before_microseconds == 0 ||
      evidence.tamper_key_not_before_microseconds !=
          key_policy.key_not_before_microseconds ||
      evidence.created_at_microseconds <
          evidence.tamper_key_not_before_microseconds) {
    return AgentError("SB_AGENT_COMMERCIAL_EVIDENCE.KEY_NOT_YET_VALID",
                      evidence.evidence_uuid);
  }
  if (key_policy.key_not_after_microseconds != 0 &&
      evidence.tamper_key_not_after_microseconds !=
          key_policy.key_not_after_microseconds) {
    return AgentError("SB_AGENT_COMMERCIAL_EVIDENCE.KEY_WINDOW_MISMATCH",
                      evidence.evidence_uuid);
  }
  if (evidence.tamper_key_not_after_microseconds != 0 &&
      evidence.created_at_microseconds >
          evidence.tamper_key_not_after_microseconds) {
    return AgentError("SB_AGENT_COMMERCIAL_EVIDENCE.KEY_ROTATION_EXPIRED",
                      evidence.evidence_uuid);
  }
  if (evidence.key_residency_class != key_policy.key_residency_class ||
      evidence.data_residency_class != key_policy.data_residency_class) {
    return AgentError("SB_AGENT_COMMERCIAL_EVIDENCE.RESIDENCY_MISMATCH",
                      evidence.evidence_uuid);
  }
  if (production_live_path &&
      (!evidence.production_key_material || evidence.test_key_material ||
       evidence.key_material_exported || !KeyMaterialPolicySafe(evidence))) {
    return AgentError("SB_AGENT_COMMERCIAL_EVIDENCE.PRODUCTION_KEY_REFUSED",
                      evidence.evidence_uuid);
  }
  if (evidence.production_key_material != key_policy.production_key_material ||
      evidence.test_key_material != key_policy.test_key_material ||
      evidence.key_material_exported != key_policy.key_material_exported) {
    return AgentError("SB_AGENT_COMMERCIAL_EVIDENCE.KEY_MATERIAL_POLICY_MISMATCH",
                      evidence.evidence_uuid);
  }
  if (!key_policy.allow_test_fixture_key &&
      (evidence.test_key_material || ContainsToken(evidence.tamper_key_id, "test") ||
       ContainsToken(evidence.tamper_key_id, "fixture"))) {
    return AgentError("SB_AGENT_COMMERCIAL_EVIDENCE.TEST_KEY_REFUSED",
                      evidence.evidence_uuid);
  }
  if (key_policy.require_redaction_before_buffering &&
      !evidence.redaction_applied_before_buffering) {
    return AgentError("SB_AGENT_COMMERCIAL_EVIDENCE.REDACTION_NOT_PREBUFFERED",
                      evidence.evidence_uuid);
  }
  if (key_policy.require_storage_linkage &&
      evidence.storage_linkage_digest.empty()) {
    return AgentError("SB_AGENT_COMMERCIAL_EVIDENCE.STORAGE_LINK_MISSING",
                      evidence.evidence_uuid);
  }
  if (evidence.retention_class == "legal_hold" &&
      (!evidence.legal_hold_active || evidence.expires_at_microseconds != 0)) {
    return AgentError("SB_AGENT_COMMERCIAL_EVIDENCE.LEGAL_HOLD_INVALID",
                      evidence.evidence_uuid);
  }
  if (evidence.retention_class != "legal_hold" &&
      (evidence.legal_hold_active || evidence.expires_at_microseconds == 0)) {
    return AgentError("SB_AGENT_COMMERCIAL_EVIDENCE.RETENTION_INVALID",
                      evidence.evidence_uuid);
  }
  return AgentRuntimeStatus{true,
                            "SB_AGENT_COMMERCIAL_EVIDENCE.KEY_POLICY_VALID",
                            evidence.evidence_uuid};
}

AgentEvidenceRecord BuildCommercialAgentEvidence(
    const CommercialAgentEvidenceBuildRequest& request) {
  AgentEvidenceRecord evidence;
  evidence.evidence_uuid = DeterministicAgentRuntimeObjectUuidFromKey(
      "commercial_agent_evidence|" + request.action.action_uuid + "|" +
      request.provider_id + "|" + request.input_evidence_digest);
  evidence.agent_type_id = request.action.agent_type_id;
  evidence.instance_uuid = request.action.instance_uuid;
  evidence.evidence_kind = "commercial_agent_action_audit_evidence";
  evidence.diagnostic_code = request.diagnostic_code;
  evidence.detail = "provider=" + request.provider_id +
                    ";action_uuid=" + request.action.action_uuid +
                    ";authority_source=" +
                    AgentActionAuthoritySourceName(request.authority.source);
  evidence.input_metric_digest =
      request.input_metric_digest.empty()
          ? InputMetricDigestFromAction(request.action)
          : request.input_metric_digest;
  evidence.policy_generation = request.policy_generation;
  evidence.principal_uuid = request.authority.principal_uuid;
  evidence.rights_used = request.authority.rights;
  evidence.scope_uuids = ScopeUuidsForRequest(request);
  evidence.decision_payload_digest = Sha256Digest(
      request.decision_payload.empty() ? evidence.detail : request.decision_payload);
  evidence.result_state = request.result_state;
  evidence.redaction_class = request.protected_material_present
                                 ? "protected_material"
                                 : request.redaction_class;
  evidence.retention_class =
      request.retention_class.empty() ? "audit" : request.retention_class;
  evidence.outcome_verification_evidence_uuid =
      request.outcome_verification_evidence_uuid;
  evidence.tamper_digest_algorithm = "sha256-chain-v1";
  evidence.previous_tamper_digest =
      request.previous_tamper_digest.empty()
          ? "scratchbird-agent-evidence-ledger-genesis"
          : request.previous_tamper_digest;
  evidence.tamper_signature_algorithm = "hmac-sha256-v1";
  evidence.tamper_key_id =
      request.tamper_key_id.empty() ? "agent-evidence-ledger-key-v1"
                                    : request.tamper_key_id;
  evidence.tamper_key_provenance =
      request.tamper_key_provenance.empty()
          ? "engine_local_protected_hmac_key"
          : request.tamper_key_provenance;
  evidence.tamper_key_generation =
      request.tamper_key_generation == 0 ? 1 : request.tamper_key_generation;
  evidence.created_at_microseconds =
      request.created_at_microseconds == 0 ? 1 : request.created_at_microseconds;
  evidence.evidence_key_policy_id =
      request.evidence_key_policy_id.empty()
          ? "agent-evidence-key-policy-v1"
          : request.evidence_key_policy_id;
  evidence.tamper_key_rotation_epoch =
      request.tamper_key_rotation_epoch == 0 ? evidence.tamper_key_generation
                                             : request.tamper_key_rotation_epoch;
  evidence.tamper_key_not_before_microseconds =
      request.tamper_key_not_before_microseconds == 0
          ? 1
          : request.tamper_key_not_before_microseconds;
  evidence.tamper_key_not_after_microseconds =
      request.tamper_key_not_after_microseconds == 0
          ? evidence.created_at_microseconds + kYearMicros
          : request.tamper_key_not_after_microseconds;
  evidence.key_residency_class =
      request.key_residency_class.empty() ? "engine_local_protected"
                                          : request.key_residency_class;
  evidence.data_residency_class =
      request.data_residency_class.empty() ? "engine_local"
                                           : request.data_residency_class;
  evidence.storage_linkage_digest =
      request.storage_linkage_digest.empty()
          ? Sha256Digest(evidence.evidence_uuid + "|" + evidence.instance_uuid)
          : request.storage_linkage_digest;
  evidence.tamper_evidence_generation = 1;
  evidence.expires_at_microseconds =
      request.expires_at_microseconds == 0
          ? RetentionExpiresAt(evidence.retention_class,
                               evidence.created_at_microseconds)
          : request.expires_at_microseconds;
  evidence.legal_hold_active =
      request.legal_hold_active || evidence.retention_class == "legal_hold";
  evidence.production_key_material = request.production_key_material;
  evidence.test_key_material = request.test_key_material;
  evidence.key_material_exported = request.key_material_exported;
  evidence.protected_material_suppressed = false;
  evidence.redaction_applied_before_buffering = false;
  evidence.parser_authority = false;
  evidence.client_authority = false;
  evidence.reference_authority = false;
  evidence.sidecar_authority = false;
  evidence.transaction_authority = false;
  evidence.finality_authority = false;
  evidence.visibility_authority = false;
  evidence.recovery_authority = false;
  evidence.security_authority = false;
  RedactBeforeBuffering(&evidence);
  FinalizeCommercialAgentEvidenceDigests(&evidence);
  return evidence;
}

CommercialAgentEvidenceValidation ValidateCommercialAgentEvidence(
    const AgentEvidenceRecord& evidence,
    u64 now_microseconds) {
  CommercialAgentEvidenceValidation validation;
  validation.authority_clean = AuthorityFlagsClean(evidence);
  validation.expired = CommercialAgentEvidenceExpired(evidence, now_microseconds);
  validation.redaction_before_buffering =
      evidence.redaction_applied_before_buffering;
  validation.storage_linked = !evidence.storage_linkage_digest.empty();
  validation.residency_valid = !evidence.key_residency_class.empty() &&
                               !evidence.data_residency_class.empty();
  validation.retention_valid =
      (evidence.retention_class == "legal_hold" &&
       evidence.legal_hold_active && evidence.expires_at_microseconds == 0) ||
      (evidence.retention_class != "legal_hold" &&
       !evidence.legal_hold_active && evidence.expires_at_microseconds != 0);
  const CommercialAgentEvidenceKeyPolicy key_policy = [&]() {
    CommercialAgentEvidenceKeyPolicy policy;
    policy.policy_id = evidence.evidence_key_policy_id;
    policy.key_id = evidence.tamper_key_id;
    policy.key_provenance = evidence.tamper_key_provenance;
    policy.key_generation = evidence.tamper_key_generation;
    policy.key_rotation_epoch = evidence.tamper_key_rotation_epoch;
    policy.key_not_before_microseconds =
        evidence.tamper_key_not_before_microseconds;
    policy.key_not_after_microseconds =
        evidence.tamper_key_not_after_microseconds;
    policy.key_residency_class = evidence.key_residency_class;
    policy.data_residency_class = evidence.data_residency_class;
    return policy;
  }();
  validation.key_policy_valid =
      ValidateCommercialAgentEvidenceKeyPolicy(evidence, key_policy).ok;
  const auto key_status =
      ValidateCommercialAgentEvidenceKeyPolicy(evidence, key_policy);
  if (!key_status.ok) {
    validation.status = key_status;
    return validation;
  }
  validation.key_policy_valid = true;
  validation.tamper_valid =
      !evidence.tamper_digest.empty() &&
      evidence.tamper_digest_algorithm == "sha256-chain-v1" &&
      evidence.tamper_digest == CommercialAgentEvidenceTamperDigest(evidence) &&
      !evidence.previous_tamper_digest.empty() &&
      !evidence.tamper_chain_digest.empty() &&
      evidence.tamper_signature_algorithm == "hmac-sha256-v1" &&
      !evidence.tamper_signature.empty() &&
      !evidence.tamper_key_id.empty() &&
      !evidence.tamper_key_provenance.empty() &&
      evidence.tamper_key_generation != 0 &&
      !evidence.evidence_key_policy_id.empty() &&
      evidence.tamper_key_rotation_epoch != 0 &&
      evidence.tamper_key_not_before_microseconds != 0 &&
      !evidence.key_residency_class.empty() &&
      !evidence.data_residency_class.empty() &&
      !evidence.storage_linkage_digest.empty() &&
      evidence.tamper_chain_digest ==
          CommercialAgentEvidenceChainDigest(evidence) &&
      evidence.tamper_signature ==
          CommercialAgentEvidenceSignatureDigest(evidence);
  if (evidence.evidence_uuid.empty() || evidence.agent_type_id.empty() ||
      evidence.instance_uuid.empty() || evidence.input_metric_digest.empty() ||
      evidence.policy_generation == 0 || evidence.principal_uuid.empty() ||
      evidence.scope_uuids.empty() || evidence.decision_payload_digest.empty() ||
      evidence.result_state.empty() || evidence.diagnostic_code.empty() ||
      evidence.redaction_class.empty() || evidence.retention_class.empty() ||
      evidence.outcome_verification_evidence_uuid.empty() ||
      evidence.previous_tamper_digest.empty() ||
      evidence.tamper_chain_digest.empty() ||
      evidence.tamper_signature_algorithm.empty() ||
      evidence.tamper_signature.empty() ||
      evidence.tamper_key_id.empty() ||
      evidence.tamper_key_provenance.empty() ||
      evidence.tamper_key_generation == 0 ||
      evidence.evidence_key_policy_id.empty() ||
      evidence.tamper_key_rotation_epoch == 0 ||
      evidence.tamper_key_not_before_microseconds == 0 ||
      evidence.key_residency_class.empty() ||
      evidence.data_residency_class.empty() ||
      evidence.storage_linkage_digest.empty() ||
      evidence.tamper_evidence_generation == 0) {
    validation.status =
        AgentError("SB_AGENT_COMMERCIAL_EVIDENCE.REQUIRED_FIELD_MISSING",
                   evidence.evidence_uuid);
    return validation;
  }
  if (!validation.retention_valid) {
    validation.status =
        AgentError("SB_AGENT_COMMERCIAL_EVIDENCE.RETENTION_INVALID",
                   evidence.evidence_uuid);
    return validation;
  }
  if (!validation.authority_clean) {
    validation.status =
        AgentError("SB_AGENT_COMMERCIAL_EVIDENCE.AUTHORITY_FLAG_FORBIDDEN",
                   evidence.evidence_uuid);
    return validation;
  }
  if (!validation.tamper_valid) {
    validation.status =
        AgentError("SB_AGENT_COMMERCIAL_EVIDENCE.TAMPER_DIGEST_MISMATCH",
                   evidence.evidence_uuid);
    return validation;
  }
  if (validation.expired) {
    validation.status =
        AgentError("SB_AGENT_COMMERCIAL_EVIDENCE.RETENTION_EXPIRED",
                   evidence.evidence_uuid);
    return validation;
  }
  validation.status =
      AgentRuntimeStatus{true, "SB_AGENT_COMMERCIAL_EVIDENCE.VALIDATED",
                         evidence.evidence_uuid};
  return validation;
}

CommercialAgentEvidenceChainValidation ValidateCommercialAgentEvidenceChain(
    const CommercialAgentEvidenceChainValidationRequest& request) {
  CommercialAgentEvidenceChainValidation result;
  result.chain_continuity_valid = true;
  result.key_policy_valid = true;
  std::set<std::string> evidence_uuids;
  std::string expected_previous =
      request.expected_initial_previous_digest.empty()
          ? "scratchbird-agent-evidence-ledger-genesis"
          : request.expected_initial_previous_digest;
  if (request.evidence.empty()) {
    result.status =
        AgentError("SB_AGENT_COMMERCIAL_EVIDENCE_CHAIN.EMPTY", "");
    result.chain_continuity_valid = false;
    return result;
  }
  for (const auto& evidence : request.evidence) {
    if (!evidence_uuids.insert(evidence.evidence_uuid).second) {
      result.status =
          AgentError("SB_AGENT_COMMERCIAL_EVIDENCE_CHAIN.DUPLICATE",
                     evidence.evidence_uuid);
      result.chain_continuity_valid = false;
      return result;
    }
    if (evidence.previous_tamper_digest != expected_previous) {
      result.status =
          AgentError("SB_AGENT_COMMERCIAL_EVIDENCE_CHAIN.BROKEN",
                     evidence.evidence_uuid);
      result.chain_continuity_valid = false;
      return result;
    }
    const auto key_status = ValidateCommercialAgentEvidenceKeyPolicy(
        evidence, request.key_policy, request.production_live_path);
    if (!key_status.ok) {
      result.status = key_status;
      result.key_policy_valid = false;
      return result;
    }
    const auto validation =
        ValidateCommercialAgentEvidence(evidence, request.now_microseconds);
    if (!validation.status.ok) {
      result.status = validation.status;
      result.chain_continuity_valid = false;
      return result;
    }
    expected_previous = evidence.tamper_chain_digest;
    ++result.validated_records;
  }
  result.status =
      AgentRuntimeStatus{true, "SB_AGENT_COMMERCIAL_EVIDENCE_CHAIN.VALIDATED",
                         std::to_string(result.validated_records)};
  return result;
}

CommercialAgentEvidenceViewResult ProjectCommercialAgentEvidenceView(
    const CommercialAgentEvidenceViewRequest& request) {
  CommercialAgentEvidenceViewResult result;
  result.evidence = request.evidence;
  const auto family = request.support_bundle_view
      ? AgentSecurityCommandFamily::support_bundle_read
      : AgentSecurityCommandFamily::evidence_read;
  result.grant = EvaluateAgentCommandGrant(request.context, family,
                                           request.evidence.evidence_uuid);
  if (!result.grant.allowed) {
    result.status =
        AgentError("SB_AGENT_COMMERCIAL_EVIDENCE.ACCESS_REFUSED",
                   request.evidence.evidence_uuid);
    result.visible = false;
    result.redacted = true;
    result.evidence.detail.clear();
    result.evidence.principal_uuid.clear();
    result.evidence.rights_used.clear();
    result.evidence.scope_uuids.clear();
    return result;
  }

  const auto validation =
      ValidateCommercialAgentEvidence(request.evidence, request.now_microseconds);
  result.tamper_valid = validation.tamper_valid;
  result.expired = validation.expired;
  if (!validation.status.ok) {
    result.status = validation.status;
    result.visible = false;
    result.redacted = true;
    result.evidence.detail.clear();
    return result;
  }

  result.visible = true;
  result.status = validation.status;
  if (request.support_bundle_view) {
    result.evidence.tamper_signature = "redacted";
    result.evidence.tamper_key_provenance = "redacted";
  }
  if (IsProtectedRedactionClass(request.evidence.redaction_class)) {
    SuppressProtectedMaterial(&result.evidence);
    result.protected_material_suppressed = true;
    result.redacted = true;
    result.status.diagnostic_code =
        "SB_AGENT_COMMERCIAL_EVIDENCE.PROTECTED_MATERIAL_SUPPRESSED";
    return result;
  }
  if (IsRestrictedRedactionClass(request.evidence.redaction_class) &&
      !HasRight(request.context, "SEC_REDACTION_POLICY_EDIT")) {
    result.evidence.detail = "redacted";
    result.evidence.principal_uuid.clear();
    result.redacted = true;
    result.status.diagnostic_code =
        "SB_AGENT_COMMERCIAL_EVIDENCE.REDACTED";
  }
  return result;
}

}  // namespace scratchbird::core::agents
