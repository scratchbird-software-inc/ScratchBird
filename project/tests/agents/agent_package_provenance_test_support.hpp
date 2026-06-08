// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_package_provenance.hpp"

#include <string>
#include <utility>
#include <vector>

namespace agent_test_support {

inline std::string DigestHex(char value) {
  return "sha256:" + std::string(64, value);
}

inline scratchbird::core::agents::AgentPackageProvenanceBundle
PageProviderPackageProvenance(const std::string& uuid_prefix =
                                  "019f0802-0000-7000-8000-00000000008") {
  namespace agents = scratchbird::core::agents;
  agents::AgentPackageProvenanceBundle bundle;
  bundle.policy.policy_id = "ceic080-agent-package-policy";
  bundle.policy.policy_generation = 1;
  bundle.policy.allowed_signer_identities = {"scratchbird-release-signing"};
  bundle.policy.allowed_signer_key_ids = {"release-key-v1"};
  bundle.policy.allowed_sandbox_profiles = {"agent-bounded-local"};
  bundle.policy.minimum_versions = {
      {agents::AgentPackageSubjectKind::plugin, "", 100},
      {agents::AgentPackageSubjectKind::actuator_provider, "", 100},
      {agents::AgentPackageSubjectKind::agent_binary, "", 100}};
  const std::vector<std::pair<agents::AgentPackageSubjectKind, std::string>>
      subjects = {
          {agents::AgentPackageSubjectKind::plugin,
           "page_allocation_manager.plugin"},
          {agents::AgentPackageSubjectKind::actuator_provider,
           "page_manager.preallocate_page_family.provider"},
          {agents::AgentPackageSubjectKind::agent_binary,
           "page_allocation_manager.agent_binary"}};
  int index = 0;
  for (const auto& subject : subjects) {
    agents::AgentPackageProvenanceRecord record;
    record.subject_kind = subject.first;
    record.subject_id = subject.second;
    record.package_uuid = uuid_prefix + std::to_string(index);
    record.package_version = "1.0." + std::to_string(index);
    record.package_version_ordinal = 100;
    record.package_digest = DigestHex(static_cast<char>('1' + index));
    record.signature_algorithm = "ed25519";
    record.signature_digest = DigestHex(static_cast<char>('4' + index));
    record.signature_verified = true;
    record.signature_evidence_uuid =
        "signature-evidence-" + std::to_string(index);
    record.signer_identity = "scratchbird-release-signing";
    record.signer_key_id = "release-key-v1";
    record.signer_policy_id = bundle.policy.policy_id;
    record.signer_allowed_by_policy = true;
    record.sbom_present = true;
    record.sbom_format = "spdx-2.3";
    record.sbom_digest = DigestHex(static_cast<char>('7' + index));
    record.sbom_evidence_uuid = "sbom-evidence-" + std::to_string(index);
    record.sandbox_profile_id = "agent-bounded-local";
    record.sandbox_profile_digest = DigestHex(static_cast<char>('a' + index));
    record.sandbox_evidence_uuid =
        "sandbox-evidence-" + std::to_string(index);
    record.revocation_status =
        agents::AgentPackageRevocationStatus::not_revoked;
    record.revocation_checked = true;
    record.revocation_generation = 10 + index;
    record.revocation_evidence_uuid =
        "revocation-evidence-" + std::to_string(index);
    record.production_package = true;
    record.provenance_evidence_uuid =
        "package-provenance-evidence-" + std::to_string(index);
    agents::FinalizeAgentPackageProvenanceDigest(&record);
    bundle.records.push_back(std::move(record));
    ++index;
  }
  return bundle;
}

}  // namespace agent_test_support
