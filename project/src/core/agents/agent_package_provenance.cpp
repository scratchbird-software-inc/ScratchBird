// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_package_provenance.hpp"
#include "uuid.hpp"

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

bool IsBinaryUuid(const std::string& value) {
  if (value.size() != 16) return false;
  core::platform::Uuid uuid;
  std::copy(value.begin(),value.end(),uuid.bytes.begin());
  return core::uuid::IsEngineIdentityUuid(uuid);
}

bool AppendFramedValue(std::string* out, const std::string& value) {
  constexpr std::size_t limit = 16777216;
  if (out->size() > limit-4 || value.size() > limit-4-out->size()) return false;
  const auto size = static_cast<std::uint32_t>(value.size());
  for (unsigned i=0;i<4;++i) out->push_back(static_cast<char>(size>>(8*i)));
  out->append(value);
  return true;
}
std::string IntegerBytes(u64 value) {
  std::string bytes(8,0);
  for (unsigned i=0;i<8;++i) bytes[i]=static_cast<char>(value>>(8*i));
  return bytes;
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
  std::string payload = "SBPKP002";
  const auto append = [&](const std::string& key, const std::string& value) {
    return AppendFramedValue(&payload,key) && AppendFramedValue(&payload,value);
  };
  if (!append("subject_kind", IntegerBytes(static_cast<u64>(record.subject_kind)))) return {};
  if (!append("subject_id", record.subject_id)) return {};
  if (!record.package_uuid.empty() && !IsBinaryUuid(record.package_uuid)) return {};
  if (!append("package_uuid", record.package_uuid)) return {};
  if (!append("package_version", record.package_version)) return {};
  if (!append("package_version_ordinal", IntegerBytes(static_cast<u64>(record.package_version_ordinal)))) return {};
  if (!append("package_digest", record.package_digest)) return {};
  if (!append("signature_algorithm", record.signature_algorithm)) return {};
  if (!append("signature_digest", record.signature_digest)) return {};
  if (!append("signature_verified", std::string(1,record.signature_verified ? 1 : 0))) return {};
  if (!record.signature_evidence_uuid.empty() && !IsBinaryUuid(record.signature_evidence_uuid)) return {};
  if (!append("signature_evidence_uuid", record.signature_evidence_uuid)) return {};
  if (!append("signer_identity", record.signer_identity)) return {};
  if (!append("signer_key_id", record.signer_key_id)) return {};
  if (!append("signer_policy_id", record.signer_policy_id)) return {};
  if (!append("signer_allowed_by_policy", std::string(1,record.signer_allowed_by_policy ? 1 : 0))) return {};
  if (!append("signed_with_test_key", std::string(1,record.signed_with_test_key ? 1 : 0))) return {};
  if (!append("signature_fixture", std::string(1,record.signature_fixture ? 1 : 0))) return {};
  if (!append("sbom_present", std::string(1,record.sbom_present ? 1 : 0))) return {};
  if (!append("sbom_format", record.sbom_format)) return {};
  if (!append("sbom_digest", record.sbom_digest)) return {};
  if (!record.sbom_evidence_uuid.empty() && !IsBinaryUuid(record.sbom_evidence_uuid)) return {};
  if (!append("sbom_evidence_uuid", record.sbom_evidence_uuid)) return {};
  if (!append("sandbox_profile_id", record.sandbox_profile_id)) return {};
  if (!append("sandbox_profile_digest", record.sandbox_profile_digest)) return {};
  if (!record.sandbox_evidence_uuid.empty() && !IsBinaryUuid(record.sandbox_evidence_uuid)) return {};
  if (!append("sandbox_evidence_uuid", record.sandbox_evidence_uuid)) return {};
  if (!append("revocation_status", IntegerBytes(static_cast<u64>(record.revocation_status)))) return {};
  if (!append("revocation_checked", std::string(1,record.revocation_checked ? 1 : 0))) return {};
  if (!append("revocation_generation", IntegerBytes(static_cast<u64>(record.revocation_generation)))) return {};
  if (!record.revocation_evidence_uuid.empty() && !IsBinaryUuid(record.revocation_evidence_uuid)) return {};
  if (!append("revocation_evidence_uuid", record.revocation_evidence_uuid)) return {};
  if (!append("production_package", std::string(1,record.production_package ? 1 : 0))) return {};
  if (!append("test_fixture_package", std::string(1,record.test_fixture_package ? 1 : 0))) return {};
  if (!append("debug_only_package", std::string(1,record.debug_only_package ? 1 : 0))) return {};
  if (!append("cluster_route_requested", std::string(1,record.cluster_route_requested ? 1 : 0))) return {};
  if (!append("external_cluster_provider_attested", std::string(1,record.external_cluster_provider_attested ? 1 : 0))) return {};
  if (!record.external_cluster_provider_evidence_uuid.empty() && !IsBinaryUuid(record.external_cluster_provider_evidence_uuid)) return {};
  if (!append("external_cluster_provider_evidence_uuid", record.external_cluster_provider_evidence_uuid)) return {};
  if (!record.provenance_evidence_uuid.empty() && !IsBinaryUuid(record.provenance_evidence_uuid)) return {};
  if (!append("provenance_evidence_uuid", record.provenance_evidence_uuid)) return {};
  if (!append("transaction_finality_authority", std::string(1,record.transaction_finality_authority ? 1 : 0))) return {};
  if (!append("visibility_authority", std::string(1,record.visibility_authority ? 1 : 0))) return {};
  if (!append("authorization_authority", std::string(1,record.authorization_authority ? 1 : 0))) return {};
  if (!append("security_authority", std::string(1,record.security_authority ? 1 : 0))) return {};
  if (!append("recovery_authority", std::string(1,record.recovery_authority ? 1 : 0))) return {};
  if (!append("parser_authority", std::string(1,record.parser_authority ? 1 : 0))) return {};
  if (!append("reference_authority", std::string(1,record.reference_authority ? 1 : 0))) return {};
  if (!append("wal_authority", std::string(1,record.wal_authority ? 1 : 0))) return {};
  if (!append("benchmark_authority", std::string(1,record.benchmark_authority ? 1 : 0))) return {};
  if (!append("optimizer_plan_authority", std::string(1,record.optimizer_plan_authority ? 1 : 0))) return {};
  if (!append("index_finality_authority", std::string(1,record.index_finality_authority ? 1 : 0))) return {};
  if (!append("provider_finality_authority", std::string(1,record.provider_finality_authority ? 1 : 0))) return {};
  if (!append("cluster_authority", std::string(1,record.cluster_authority ? 1 : 0))) return {};
  if (!append("memory_authority", std::string(1,record.memory_authority ? 1 : 0))) return {};
  if (!append("agent_action_authority", std::string(1,record.agent_action_authority ? 1 : 0))) return {};
  return Sha256Digest(payload);
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
      !IsBinaryUuid(record.package_uuid)) {
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.PACKAGE_UUID_REQUIRED",
                      SubjectKey(record.subject_kind, record.subject_id));
  }
  if (!record.package_uuid.empty() && !IsBinaryUuid(record.package_uuid))
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.IDENTITY_INVALID", "package_uuid");
  if (!record.signature_evidence_uuid.empty() && !IsBinaryUuid(record.signature_evidence_uuid))
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.IDENTITY_INVALID", "signature_evidence_uuid");
  if (!record.sbom_evidence_uuid.empty() && !IsBinaryUuid(record.sbom_evidence_uuid))
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.IDENTITY_INVALID", "sbom_evidence_uuid");
  if (!record.sandbox_evidence_uuid.empty() && !IsBinaryUuid(record.sandbox_evidence_uuid))
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.IDENTITY_INVALID", "sandbox_evidence_uuid");
  if (!record.revocation_evidence_uuid.empty() && !IsBinaryUuid(record.revocation_evidence_uuid))
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.IDENTITY_INVALID", "revocation_evidence_uuid");
  if (!record.external_cluster_provider_evidence_uuid.empty() && !IsBinaryUuid(record.external_cluster_provider_evidence_uuid))
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.IDENTITY_INVALID", "external_cluster_provider_evidence_uuid");
  if (!record.provenance_evidence_uuid.empty() && !IsBinaryUuid(record.provenance_evidence_uuid))
    return AgentError("SB_AGENT_PACKAGE_PROVENANCE.IDENTITY_INVALID", "provenance_evidence_uuid");
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
    std::string evidence = "SBPKE002";
    if (!AppendFramedValue(&evidence,key) || !AppendFramedValue(&evidence,record.package_uuid) ||
        !AppendFramedValue(&evidence,record.package_version) || !AppendFramedValue(&evidence,record.provenance_digest)) {
      result.status = AgentError("SB_AGENT_PACKAGE_PROVENANCE.EVIDENCE_TOO_LARGE");
      return result;
    }
    result.evidence_rows.push_back(std::move(evidence));
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
