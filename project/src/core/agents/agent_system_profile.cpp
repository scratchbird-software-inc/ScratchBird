// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_system_profile.hpp"

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <sstream>
#include <utility>

namespace scratchbird::core::agents {
namespace {

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

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

std::string HmacSha256Digest(const std::string& key,
                             const std::string& payload) {
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

bool IsLiveClaim(const AgentSystemProfile& profile) {
  return profile.public_claim_level ==
             AgentSystemProfilePublicClaimLevel::live_ready ||
         profile.public_claim_level ==
             AgentSystemProfilePublicClaimLevel::production_live ||
         profile.live_enablement ==
             AgentSystemProfileLiveEnablement::live_ready ||
         profile.live_enablement ==
             AgentSystemProfileLiveEnablement::production_live;
}

bool IsProductionLiveClaim(const AgentSystemProfile& profile) {
  return profile.public_claim_level ==
             AgentSystemProfilePublicClaimLevel::production_live ||
         profile.live_enablement ==
             AgentSystemProfileLiveEnablement::production_live;
}

bool AuthorityClean(const AgentSystemProfileForbiddenAuthority& authority) {
  return !authority.transaction_finality_authority &&
         !authority.visibility_authority &&
         !authority.authorization_security_authority &&
         !authority.recovery_authority &&
         !authority.parser_authority &&
         !authority.donor_authority &&
         !authority.wal_authority &&
         !authority.benchmark_authority &&
         !authority.optimizer_plan_authority &&
         !authority.index_finality_authority &&
         !authority.provider_finality_authority &&
         !authority.cluster_authority &&
         !authority.memory_authority &&
         !authority.agent_action_authority;
}

std::string ProfileSigningKeyMaterial(const AgentSystemProfile& profile) {
  if (profile.key_policy_id == "agent-system-profile-key-policy-v1" &&
      profile.key_policy_provenance == "engine_local_profile_key_policy" &&
      profile.key_policy_generation == 1 &&
      profile.signing_key_id == "agent-system-profile-key-v1" &&
      profile.signing_key_provenance == "engine_local_profile_hmac_key" &&
      profile.signing_key_generation == 1) {
    return "scratchbird-agent-system-profile-key-v1-local-protected";
  }
  return {};
}

std::vector<std::string> BaseEvidenceFields(
    const AgentSystemProfile& profile) {
  return {
      "source=agent_system_profile",
      "agent_type_id=" + profile.agent_type_id,
      "live_enablement=" +
          std::string(AgentSystemProfileLiveEnablementName(
              profile.live_enablement)),
      "fail_mode=" +
          std::string(AgentSystemProfileFailModeName(profile.fail_mode)),
      "metric_strictness=" +
          std::string(AgentSystemProfileMetricStrictnessName(
              profile.metric_strictness)),
      "public_claim_level=" +
          std::string(AgentSystemProfilePublicClaimLevelName(
              profile.public_claim_level)),
      "durable_profile_evidence_uuid=" +
          profile.durable_profile_evidence_uuid,
      "profile_generation=" + std::to_string(profile.profile_generation),
      "profile_digest_algorithm=" + profile.profile_digest_algorithm,
      "profile_signature_algorithm=" + profile.profile_signature_algorithm,
      "transaction_finality_authority=false",
      "visibility_authority=false",
      "authorization_security_authority=false",
      "recovery_authority=false",
      "parser_authority=false",
      "donor_authority=false",
      "wal_authority=false",
      "benchmark_authority=false",
      "optimizer_plan_authority=false",
      "index_finality_authority=false",
      "provider_finality_authority=false",
      "cluster_authority=false",
      "memory_authority=false",
      "agent_action_authority=false"};
}

bool ExternalClusterProviderProofAvailable(
    const AgentSystemProfile& profile,
    const AgentSystemProfileValidationContext& context) {
  return context.route_inputs.real_cluster_provider_authority &&
         profile.external_cluster_provider_proof_present &&
         !profile.external_cluster_provider_id.empty() &&
         !profile.external_cluster_provider_evidence_uuid.empty();
}

const AgentProductionExposureRecord* FindExposure(
    const std::vector<AgentProductionExposureRecord>& records,
    const std::string& agent_type_id) {
  for (const auto& record : records) {
    if (record.agent_type_id == agent_type_id) { return &record; }
  }
  return nullptr;
}

AgentSystemProfileValidationResult ErrorResult(
    AgentSystemProfileValidationResult result,
    std::string code,
    std::string detail = {}) {
  result.status = AgentError(std::move(code), std::move(detail));
  return result;
}

void AppendAuthorityDigestFields(std::ostringstream* payload,
                                 const AgentSystemProfile& profile) {
  *payload << (profile.authority.transaction_finality_authority ? "1" : "0")
           << '\n'
           << (profile.authority.visibility_authority ? "1" : "0") << '\n'
           << (profile.authority.authorization_security_authority ? "1" : "0")
           << '\n'
           << (profile.authority.recovery_authority ? "1" : "0") << '\n'
           << (profile.authority.parser_authority ? "1" : "0") << '\n'
           << (profile.authority.donor_authority ? "1" : "0") << '\n'
           << (profile.authority.wal_authority ? "1" : "0") << '\n'
           << (profile.authority.benchmark_authority ? "1" : "0") << '\n'
           << (profile.authority.optimizer_plan_authority ? "1" : "0")
           << '\n'
           << (profile.authority.index_finality_authority ? "1" : "0")
           << '\n'
           << (profile.authority.provider_finality_authority ? "1" : "0")
           << '\n'
           << (profile.authority.cluster_authority ? "1" : "0") << '\n'
           << (profile.authority.memory_authority ? "1" : "0") << '\n'
           << (profile.authority.agent_action_authority ? "1" : "0")
           << '\n';
}

}  // namespace

const char* AgentSystemProfileLiveEnablementName(
    AgentSystemProfileLiveEnablement value) {
  switch (value) {
    case AgentSystemProfileLiveEnablement::disabled: return "disabled";
    case AgentSystemProfileLiveEnablement::dry_run: return "dry_run";
    case AgentSystemProfileLiveEnablement::advisory: return "advisory";
    case AgentSystemProfileLiveEnablement::live_ready: return "live_ready";
    case AgentSystemProfileLiveEnablement::production_live:
      return "production_live";
  }
  return "disabled";
}

const char* AgentSystemProfileFailModeName(
    AgentSystemProfileFailMode value) {
  switch (value) {
    case AgentSystemProfileFailMode::fail_closed: return "fail_closed";
    case AgentSystemProfileFailMode::disabled_on_error:
      return "disabled_on_error";
    case AgentSystemProfileFailMode::fail_open: return "fail_open";
  }
  return "fail_closed";
}

const char* AgentSystemProfileMetricStrictnessName(
    AgentSystemProfileMetricStrictness value) {
  switch (value) {
    case AgentSystemProfileMetricStrictness::unknown: return "unknown";
    case AgentSystemProfileMetricStrictness::relaxed: return "relaxed";
    case AgentSystemProfileMetricStrictness::strict: return "strict";
  }
  return "unknown";
}

const char* AgentSystemProfilePublicClaimLevelName(
    AgentSystemProfilePublicClaimLevel value) {
  switch (value) {
    case AgentSystemProfilePublicClaimLevel::disabled: return "disabled";
    case AgentSystemProfilePublicClaimLevel::dry_run: return "dry_run";
    case AgentSystemProfilePublicClaimLevel::advisory: return "advisory";
    case AgentSystemProfilePublicClaimLevel::live_ready: return "live_ready";
    case AgentSystemProfilePublicClaimLevel::production_live:
      return "production_live";
  }
  return "disabled";
}

std::string AgentSystemProfileDigest(const AgentSystemProfile& profile) {
  std::ostringstream payload;
  payload << "agent_system_profile_v1\n"
          << profile.agent_type_id << '\n'
          << AgentSystemProfileLiveEnablementName(profile.live_enablement)
          << '\n'
          << AgentSystemProfileFailModeName(profile.fail_mode) << '\n'
          << AgentSystemProfileMetricStrictnessName(
                 profile.metric_strictness)
          << '\n'
          << AgentSystemProfilePublicClaimLevelName(
                 profile.public_claim_level)
          << '\n'
          << (profile.durable_profile_evidence_present ? "1" : "0") << '\n'
          << profile.durable_profile_evidence_uuid << '\n'
          << profile.durable_profile_storage_digest << '\n'
          << (profile.evidence_required ? "1" : "0") << '\n'
          << (profile.approval_required ? "1" : "0") << '\n'
          << (profile.redaction_required ? "1" : "0") << '\n'
          << (profile.retention_required ? "1" : "0") << '\n'
          << profile.evidence_policy_id << '\n'
          << profile.approval_policy_id << '\n'
          << profile.redaction_class << '\n'
          << profile.retention_class << '\n'
          << profile.key_policy_id << '\n'
          << profile.key_policy_provenance << '\n'
          << profile.key_policy_generation << '\n'
          << profile.signing_key_id << '\n'
          << profile.signing_key_provenance << '\n'
          << profile.signing_key_generation << '\n'
          << profile.profile_generation << '\n'
          << profile.issued_at_microseconds << '\n'
          << profile.expires_at_microseconds << '\n'
          << profile.max_staleness_microseconds << '\n'
          << profile.profile_digest_algorithm << '\n'
          << profile.profile_signature_algorithm << '\n'
          << (profile.durable_profile_marks_anchor_only ? "1" : "0")
          << '\n'
          << (profile.durable_profile_marks_stub_only ? "1" : "0") << '\n'
          << (profile.external_cluster_provider_proof_present ? "1" : "0")
          << '\n'
          << profile.external_cluster_provider_id << '\n'
          << profile.external_cluster_provider_evidence_uuid << '\n';
  AppendAuthorityDigestFields(&payload, profile);
  return Sha256Digest(payload.str());
}

std::string AgentSystemProfileSignatureDigest(
    const AgentSystemProfile& profile) {
  const std::string key_material = ProfileSigningKeyMaterial(profile);
  if (key_material.empty()) { return {}; }
  std::ostringstream payload;
  payload << "agent_system_profile_signature_v1\n"
          << profile.agent_type_id << '\n'
          << profile.profile_generation << '\n'
          << profile.profile_digest << '\n'
          << profile.key_policy_id << '\n'
          << profile.key_policy_generation << '\n'
          << profile.signing_key_id << '\n'
          << profile.signing_key_generation << '\n'
          << AgentSystemProfilePublicClaimLevelName(
                 profile.public_claim_level)
          << '\n';
  return HmacSha256Digest(key_material, payload.str());
}

void FinalizeAgentSystemProfile(AgentSystemProfile* profile) {
  if (profile == nullptr) { return; }
  profile->profile_digest = AgentSystemProfileDigest(*profile);
  profile->profile_signature = AgentSystemProfileSignatureDigest(*profile);
}

AgentSystemProfileValidationResult ValidateAgentSystemProfileClaim(
    const AgentSystemProfile& profile,
    const AgentSystemProfileValidationContext& context) {
  AgentSystemProfileValidationResult result;
  result.live_claim = IsLiveClaim(profile);
  result.production_live_claim = IsProductionLiveClaim(profile);
  result.evidence_fields = BaseEvidenceFields(profile);

  const auto registry_status = ValidateCanonicalAgentRegistry();
  if (!registry_status.ok) {
    result.status = registry_status;
    return result;
  }

  const auto exposure_status =
      ValidateAgentProductionExposureMatrix(context.route_inputs);
  if (!exposure_status.ok) {
    result.status = exposure_status;
    return result;
  }

  const auto records =
      ClassifyAllCanonicalAgentProductionExposures(context.route_inputs);
  const auto* exposure = FindExposure(records, profile.agent_type_id);
  if (exposure == nullptr) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_SYSTEM_PROFILE.NON_CANONICAL_AGENT",
                       profile.agent_type_id);
  }
  result.exposure = *exposure;
  result.evidence_fields.push_back(
      std::string("production_exposure=") +
      AgentProductionExposureClassName(result.exposure.exposure));
  result.evidence_fields.push_back(
      "cluster_only=" + BoolText(result.exposure.cluster_only));
  result.evidence_fields.push_back(
      "implementation_anchor_only=" +
      BoolText(result.exposure.implementation_anchor_only));
  result.evidence_fields.push_back(
      "production_live_route_available=" +
      BoolText(result.exposure.production_live_route_available));

  result.durable_profile_evidence_valid =
      profile.durable_profile_evidence_present &&
      !profile.durable_profile_evidence_uuid.empty() &&
      !profile.durable_profile_storage_digest.empty();
  if (!result.durable_profile_evidence_valid) {
    return ErrorResult(
        std::move(result),
        "SB_AGENT_SYSTEM_PROFILE.DURABLE_PROFILE_EVIDENCE_REQUIRED",
        profile.agent_type_id);
  }

  if (profile.profile_generation == 0 ||
      profile.issued_at_microseconds == 0 ||
      profile.expires_at_microseconds == 0 ||
      profile.max_staleness_microseconds == 0) {
    return ErrorResult(
        std::move(result),
        "SB_AGENT_SYSTEM_PROFILE.GENERATION_EXPIRY_REQUIRED",
        profile.agent_type_id);
  }
  result.expired = context.now_microseconds != 0 &&
                   context.now_microseconds > profile.expires_at_microseconds;
  if (result.expired) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_SYSTEM_PROFILE.PROFILE_EXPIRED",
                       profile.agent_type_id);
  }
  result.stale =
      context.now_microseconds != 0 &&
      context.now_microseconds >
          profile.issued_at_microseconds + profile.max_staleness_microseconds;
  if (result.stale) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_SYSTEM_PROFILE.PROFILE_STALE",
                       profile.agent_type_id);
  }

  if (!profile.evidence_required || profile.evidence_policy_id.empty()) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_SYSTEM_PROFILE.EVIDENCE_REQUIREMENT_MISSING",
                       profile.agent_type_id);
  }
  if (!profile.approval_required || profile.approval_policy_id.empty()) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_SYSTEM_PROFILE.APPROVAL_REQUIREMENT_MISSING",
                       profile.agent_type_id);
  }
  if (!profile.redaction_required || profile.redaction_class.empty() ||
      profile.redaction_class == "none" ||
      profile.redaction_class == "unredacted") {
    return ErrorResult(std::move(result),
                       "SB_AGENT_SYSTEM_PROFILE.REDACTION_REQUIREMENT_MISSING",
                       profile.agent_type_id);
  }
  if (!profile.retention_required || profile.retention_class.empty() ||
      profile.retention_class == "none") {
    return ErrorResult(std::move(result),
                       "SB_AGENT_SYSTEM_PROFILE.RETENTION_REQUIREMENT_MISSING",
                       profile.agent_type_id);
  }

  if (profile.key_policy_id.empty() ||
      profile.key_policy_provenance.empty() ||
      profile.key_policy_generation == 0 ||
      profile.signing_key_id.empty() ||
      profile.signing_key_provenance.empty() ||
      profile.signing_key_generation == 0) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_SYSTEM_PROFILE.KEY_POLICY_REQUIRED",
                       profile.agent_type_id);
  }

  result.digest_valid =
      profile.profile_digest_algorithm == "sha256-v1" &&
      !profile.profile_digest.empty() &&
      profile.profile_digest == AgentSystemProfileDigest(profile);
  if (!result.digest_valid) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_SYSTEM_PROFILE.PROFILE_DIGEST_INVALID",
                       profile.agent_type_id);
  }
  result.signature_valid =
      profile.profile_signature_algorithm == "hmac-sha256-v1" &&
      !profile.profile_signature.empty() &&
      profile.profile_signature == AgentSystemProfileSignatureDigest(profile);
  if (!result.signature_valid) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_SYSTEM_PROFILE.PROFILE_SIGNATURE_INVALID",
                       profile.agent_type_id);
  }

  result.authority_clean = AuthorityClean(profile.authority);
  if (!result.authority_clean) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_SYSTEM_PROFILE.FORBIDDEN_AUTHORITY",
                       profile.agent_type_id);
  }

  if (result.live_claim &&
      profile.fail_mode != AgentSystemProfileFailMode::fail_closed) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_SYSTEM_PROFILE.FAIL_CLOSED_REQUIRED",
                       profile.agent_type_id);
  }
  if (result.live_claim &&
      profile.metric_strictness != AgentSystemProfileMetricStrictness::strict) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_SYSTEM_PROFILE.STRICT_METRICS_REQUIRED",
                       profile.agent_type_id);
  }

  if (result.live_claim &&
      (profile.durable_profile_marks_anchor_only ||
       profile.durable_profile_marks_stub_only)) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_SYSTEM_PROFILE.ANCHOR_OR_STUB_LIVE_CLAIM",
                       profile.agent_type_id);
  }

  if (result.live_claim && result.exposure.cluster_only) {
    if (!ExternalClusterProviderProofAvailable(profile, context)) {
      return ErrorResult(
          std::move(result),
          "SB_AGENT_SYSTEM_PROFILE.CLUSTER_EXTERNAL_PROVIDER_REQUIRED",
          profile.agent_type_id);
    }
    result.external_provider_only = true;
    result.status =
        AgentRuntimeStatus{true,
                           "SB_AGENT_SYSTEM_PROFILE.EXTERNAL_CLUSTER_PROVIDER_ONLY",
                           profile.agent_type_id};
    result.evidence_fields.push_back("external_provider_only=true");
    return result;
  }

  if (result.live_claim &&
      (result.exposure.implementation_anchor_only ||
       result.exposure.exposure != AgentProductionExposureClass::live_action ||
       !result.exposure.production_live_route_available ||
       !result.exposure.real_subsystem_route_proven)) {
    return ErrorResult(
        std::move(result),
        "SB_AGENT_SYSTEM_PROFILE.PRODUCTION_LIVE_EXPOSURE_BLOCKED",
        profile.agent_type_id + ":" + result.exposure.diagnostic_code);
  }

  result.status =
      AgentRuntimeStatus{true, "SB_AGENT_SYSTEM_PROFILE.VALIDATED",
                         profile.agent_type_id};
  return result;
}

}  // namespace scratchbird::core::agents
