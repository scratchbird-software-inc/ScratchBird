// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_package_provenance.hpp"

#include <algorithm>
#include <iomanip>
#include <openssl/sha.h>
#include <sstream>

namespace scratchbird::core::agents {
namespace {

bool Contains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
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
  return "sha256:" + HexBytes(digest, SHA256_DIGEST_LENGTH);
}

bool LooksLikeSha256(const std::string& value) {
  if (value.size() != 71 || value.rfind("sha256:", 0) != 0) { return false; }
  return std::all_of(value.begin() + 7, value.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

bool LooksLikeUuidText(const std::string& value) {
  if (value.size() != 36) { return false; }
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') { return false; }
      continue;
    }
    const char c = value[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

bool IsKnownSignatureAlgorithm(const std::string& value) {
  return value == "ed25519" || value == "ecdsa-p256-sha256" ||
         value == "rsa-pss-sha256";
}

bool IsKnownSbomFormat(const std::string& value) {
  return value == "spdx-2.3" || value == "cyclonedx-1.5" ||
         value == "cyclonedx-1.6";
}

bool ForbiddenAuthority(const AgentPackageProvenanceRecord& record) {
  return record.transaction_finality_authority || record.visibility_authority ||
         record.authorization_authority || record.security_authority ||
         record.recovery_authority || record.parser_authority ||
         record.reference_authority || record.wal_authority ||
         record.benchmark_authority || record.optimizer_plan_authority ||
         record.index_finality_authority || record.provider_finality_authority ||
         record.cluster_authority || record.memory_authority ||
         record.agent_action_authority;
}

std::string SubjectKey(AgentPackageSubjectKind kind, const std::string& id) {
  return std::string(AgentPackageSubjectKindName(kind)) + ":" + id;
}

u64 MinimumVersionFor(const AgentPackageProvenancePolicy& policy,
                      const AgentPackageProvenanceRecord& record) {
  u64 minimum = 0;
  for (const auto& requirement : policy.minimum_versions) {
    if (requirement.subject_kind == record.subject_kind &&
        (requirement.subject_id.empty() ||
         requirement.subject_id == record.subject_id)) {
      minimum = std::max(minimum, requirement.minimum_version_ordinal);
    }
  }
  return minimum;
}

}  // namespace

const char* AgentPackageSubjectKindName(AgentPackageSubjectKind kind) {
  switch (kind) {
    case AgentPackageSubjectKind::unknown: return "unknown";
    case AgentPackageSubjectKind::plugin: return "plugin";
    case AgentPackageSubjectKind::actuator_provider: return "actuator_provider";
    case AgentPackageSubjectKind::agent_binary: return "agent_binary";
  }
  return "unknown";
}

const char* AgentPackageRevocationStatusName(
    AgentPackageRevocationStatus status) {
  switch (status) {
    case AgentPackageRevocationStatus::unknown: return "unknown";
    case AgentPackageRevocationStatus::not_revoked: return "not_revoked";
    case AgentPackageRevocationStatus::revoked: return "revoked";
    case AgentPackageRevocationStatus::check_stale: return "check_stale";
    case AgentPackageRevocationStatus::unavailable: return "unavailable";
  }
  return "unknown";
}

std::string ComputeAgentPackageProvenanceDigest(
    const AgentPackageProvenanceRecord& record) {
  std::ostringstream payload;
  payload << AgentPackageSubjectKindName(record.subject_kind) << '\n'
          << record.subject_id << '\n'
          << record.package_uuid << '\n'
          << record.package_version << '\n'
          << record.package_version_ordinal << '\n'
          << record.package_digest << '\n'
          << record.signature_algorithm << '\n'
          << record.signature_digest << '\n'
          << record.signature_verified << '\n'
          << record.signature_evidence_uuid << '\n'
          << record.signer_identity << '\n'
          << record.signer_key_id << '\n'
          << record.signer_policy_id << '\n'
          << record.sbom_format << '\n'
          << record.sbom_digest << '\n'
          << record.sbom_evidence_uuid << '\n'
          << record.sandbox_profile_id << '\n'
          << record.sandbox_profile_digest << '\n'
          << record.sandbox_evidence_uuid << '\n'
          << AgentPackageRevocationStatusName(record.revocation_status) << '\n'
          << record.revocation_generation << '\n'
          << record.revocation_evidence_uuid << '\n'
          << record.provenance_evidence_uuid << '\n'
          << record.production_package << '\n'
          << record.test_fixture_package << '\n'
          << record.debug_only_package << '\n'
          << record.cluster_route_requested << '\n'
          << record.external_cluster_provider_attested << '\n'
          << record.external_cluster_provider_evidence_uuid << '\n';
  return Sha256Digest(payload.str());
}

void FinalizeAgentPackageProvenanceDigest(
    AgentPackageProvenanceRecord* record) {
  if (record == nullptr) { return; }
  record->provenance_digest = ComputeAgentPackageProvenanceDigest(*record);
}

AgentRuntimeStatus ValidateAgentPackageProvenancePolicy(
    const AgentPackageProvenancePolicy& policy) {
  if (policy.policy_id.empty() || policy.policy_generation == 0) {
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.POLICY_REQUIRED");
  }
  if (policy.allowed_signer_identities.empty() ||
      policy.allowed_signer_key_ids.empty()) {
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.ALLOWED_SIGNER_POLICY_REQUIRED",
                      policy.policy_id);
  }
  if (policy.require_sandbox_profile &&
      policy.allowed_sandbox_profiles.empty()) {
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.SANDBOX_POLICY_REQUIRED",
                      policy.policy_id);
  }
  return AgentOk();
}

AgentRuntimeStatus ValidateAgentPackageProvenanceRecord(
    const AgentPackageProvenanceRecord& record,
    const AgentPackageProvenancePolicy& policy) {
  const auto policy_status = ValidateAgentPackageProvenancePolicy(policy);
  if (!policy_status.ok) { return policy_status; }
  if (record.subject_kind == AgentPackageSubjectKind::unknown ||
      record.subject_id.empty()) {
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.SUBJECT_REQUIRED");
  }
  if (policy.require_package_uuid &&
      !LooksLikeUuidText(record.package_uuid)) {
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.PACKAGE_UUID_REQUIRED",
                      SubjectKey(record.subject_kind, record.subject_id));
  }
  if (record.package_version.empty() || record.package_version_ordinal == 0) {
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.VERSION_REQUIRED",
                      SubjectKey(record.subject_kind, record.subject_id));
  }
  const u64 minimum_version = MinimumVersionFor(policy, record);
  if (minimum_version != 0 &&
      record.package_version_ordinal < minimum_version) {
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.VERSION_BELOW_POLICY",
                      SubjectKey(record.subject_kind, record.subject_id));
  }
  if (policy.require_digest && !LooksLikeSha256(record.package_digest)) {
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.PACKAGE_DIGEST_REQUIRED",
                      SubjectKey(record.subject_kind, record.subject_id));
  }
  if (policy.require_signature) {
    if (!IsKnownSignatureAlgorithm(record.signature_algorithm) ||
        !LooksLikeSha256(record.signature_digest) ||
        record.signature_evidence_uuid.empty() ||
        record.signer_identity.empty() || record.signer_key_id.empty()) {
      return AgentError("SB_AGENT_PACKAGE_PROVENANCE.SIGNATURE_REQUIRED",
                        SubjectKey(record.subject_kind, record.subject_id));
    }
    if (policy.require_signature_verification &&
        !record.signature_verified) {
      return AgentError("SB_AGENT_PACKAGE_PROVENANCE.SIGNATURE_NOT_VERIFIED",
                        SubjectKey(record.subject_kind, record.subject_id));
    }
  }
  if (record.signer_policy_id != policy.policy_id ||
      !record.signer_allowed_by_policy ||
      !Contains(policy.allowed_signer_identities, record.signer_identity) ||
      !Contains(policy.allowed_signer_key_ids, record.signer_key_id)) {
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.SIGNER_NOT_ALLOWED",
                      SubjectKey(record.subject_kind, record.subject_id));
  }
  if (policy.require_sbom &&
      (!record.sbom_present || !IsKnownSbomFormat(record.sbom_format) ||
       !LooksLikeSha256(record.sbom_digest) ||
       record.sbom_evidence_uuid.empty())) {
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.SBOM_REQUIRED",
                      SubjectKey(record.subject_kind, record.subject_id));
  }
  if (policy.require_sandbox_profile &&
      (record.sandbox_profile_id.empty() ||
       !LooksLikeSha256(record.sandbox_profile_digest) ||
       record.sandbox_evidence_uuid.empty() ||
       !Contains(policy.allowed_sandbox_profiles,
                 record.sandbox_profile_id))) {
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.SANDBOX_PROFILE_REQUIRED",
                      SubjectKey(record.subject_kind, record.subject_id));
  }
  if (!record.revocation_checked ||
      record.revocation_status != AgentPackageRevocationStatus::not_revoked ||
      record.revocation_generation == 0 ||
      record.revocation_evidence_uuid.empty()) {
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.REVOCATION_STATUS_REQUIRED",
                      SubjectKey(record.subject_kind, record.subject_id));
  }
  if (policy.production_live_path) {
    if (!record.production_package || record.test_fixture_package ||
        record.debug_only_package || record.signed_with_test_key ||
        record.signature_fixture || !policy.allow_test_packages) {
      if (record.test_fixture_package || record.debug_only_package ||
          record.signed_with_test_key || record.signature_fixture ||
          !record.production_package) {
        return AgentError("SB_AGENT_PACKAGE_PROVENANCE.TEST_PACKAGE_REFUSED",
                          SubjectKey(record.subject_kind, record.subject_id));
      }
    }
  }
  if (!policy.allow_debug_packages && record.debug_only_package) {
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.DEBUG_PACKAGE_REFUSED",
                      SubjectKey(record.subject_kind, record.subject_id));
  }
  if (record.cluster_route_requested &&
      (!policy.local_cluster_routes_allowed ||
       (policy.require_external_cluster_provider_proof &&
        (!record.external_cluster_provider_attested ||
         record.external_cluster_provider_evidence_uuid.empty())))) {
    return AgentError(
        "SB_AGENT_PACKAGE_PROVENANCE.CLUSTER_EXTERNAL_PROVIDER_REQUIRED",
        SubjectKey(record.subject_kind, record.subject_id));
  }
  if (record.provenance_evidence_uuid.empty() ||
      !LooksLikeSha256(record.provenance_digest) ||
      record.provenance_digest !=
          ComputeAgentPackageProvenanceDigest(record)) {
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.PROVENANCE_DIGEST_REQUIRED",
                      SubjectKey(record.subject_kind, record.subject_id));
  }
  if (ForbiddenAuthority(record)) {
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.FORBIDDEN_AUTHORITY",
                      SubjectKey(record.subject_kind, record.subject_id));
  }
  return AgentOk();
}

AgentPackageProvenanceEvaluation ValidateAgentPackageProvenanceBundle(
    const AgentPackageProvenanceBundle& bundle) {
  AgentPackageProvenanceEvaluation result;
  const auto policy_status =
      ValidateAgentPackageProvenancePolicy(bundle.policy);
  if (!policy_status.ok) {
    result.status = policy_status;
    return result;
  }
  bool saw_plugin = false;
  bool saw_actuator_provider = false;
  bool saw_agent_binary = false;
  std::vector<std::string> seen_subjects;
  std::ostringstream bundle_payload;
  for (const auto& record : bundle.records) {
    const std::string key = SubjectKey(record.subject_kind, record.subject_id);
    if (Contains(seen_subjects, key)) {
      result.status =
          AgentError("SB_AGENT_PACKAGE_PROVENANCE.DUPLICATE_SUBJECT", key);
      return result;
    }
    seen_subjects.push_back(key);
    const auto status =
        ValidateAgentPackageProvenanceRecord(record, bundle.policy);
    if (!status.ok) {
      result.status = status;
      return result;
    }
    saw_plugin = saw_plugin || record.subject_kind == AgentPackageSubjectKind::plugin;
    saw_actuator_provider =
        saw_actuator_provider ||
        record.subject_kind == AgentPackageSubjectKind::actuator_provider;
    saw_agent_binary =
        saw_agent_binary || record.subject_kind == AgentPackageSubjectKind::agent_binary;
    result.evidence_rows.push_back(key + "|" + record.package_uuid + "|" +
                                   record.package_version + "|" +
                                   record.provenance_digest);
    bundle_payload << key << '|' << record.provenance_digest << '\n';
  }
  if (bundle.require_plugin_record && !saw_plugin) {
    result.status =
        AgentError("SB_AGENT_PACKAGE_PROVENANCE.PLUGIN_RECORD_REQUIRED");
    return result;
  }
  if (bundle.require_actuator_provider_record && !saw_actuator_provider) {
    result.status = AgentError(
        "SB_AGENT_PACKAGE_PROVENANCE.ACTUATOR_PROVIDER_RECORD_REQUIRED");
    return result;
  }
  if (bundle.require_agent_binary_record && !saw_agent_binary) {
    result.status =
        AgentError("SB_AGENT_PACKAGE_PROVENANCE.AGENT_BINARY_RECORD_REQUIRED");
    return result;
  }
  result.bundle_digest = Sha256Digest(bundle_payload.str());
  result.status = {true, "SB_AGENT_PACKAGE_PROVENANCE.ACCEPTED",
                   result.bundle_digest};
  result.accepted = true;
  return result;
}

}  // namespace scratchbird::core::agents
