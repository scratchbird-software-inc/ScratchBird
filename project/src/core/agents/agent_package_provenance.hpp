// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: CEIC_080_AGENT_PACKAGE_PLUGIN_ACTUATOR_PROVENANCE
// Package provenance is an admission/evidence control for agent plugins,
// actuator providers, and agent binaries. It is never transaction finality,
// visibility, recovery, parser, reference, benchmark, optimizer-plan, index,
// provider-finality, cluster, memory, or action authority.

#include "agent_runtime.hpp"

#include <map>
#include <string>
#include <vector>

namespace scratchbird::core::agents {

enum class AgentPackageSubjectKind {
  unknown,
  plugin,
  actuator_provider,
  agent_binary
};

enum class AgentPackageRevocationStatus {
  unknown,
  not_revoked,
  revoked,
  check_stale,
  unavailable
};

struct AgentPackageMinimumVersion {
  AgentPackageSubjectKind subject_kind = AgentPackageSubjectKind::unknown;
  std::string subject_id;
  u64 minimum_version_ordinal = 0;
};

struct AgentPackageProvenancePolicy {
  std::string policy_id;
  u64 policy_generation = 0;
  bool production_live_path = true;
  bool require_digest = true;
  bool require_signature = true;
  bool require_signature_verification = true;
  bool require_package_uuid = true;
  bool require_sbom = true;
  bool require_sandbox_profile = true;
  bool allow_test_packages = false;
  bool allow_debug_packages = false;
  bool local_cluster_routes_allowed = false;
  bool require_external_cluster_provider_proof = true;
  std::vector<std::string> allowed_signer_identities;
  std::vector<std::string> allowed_signer_key_ids;
  std::vector<std::string> allowed_sandbox_profiles;
  std::vector<AgentPackageMinimumVersion> minimum_versions;
};

struct AgentPackageProvenanceRecord {
  AgentPackageSubjectKind subject_kind = AgentPackageSubjectKind::unknown;
  std::string subject_id;
  std::string package_uuid;
  std::string package_version;
  u64 package_version_ordinal = 0;
  std::string package_digest;
  std::string signature_algorithm;
  std::string signature_digest;
  bool signature_verified = false;
  std::string signature_evidence_uuid;
  std::string signer_identity;
  std::string signer_key_id;
  std::string signer_policy_id;
  bool signer_allowed_by_policy = false;
  bool signed_with_test_key = false;
  bool signature_fixture = false;
  bool sbom_present = false;
  std::string sbom_format;
  std::string sbom_digest;
  std::string sbom_evidence_uuid;
  std::string sandbox_profile_id;
  std::string sandbox_profile_digest;
  std::string sandbox_evidence_uuid;
  AgentPackageRevocationStatus revocation_status =
      AgentPackageRevocationStatus::unknown;
  bool revocation_checked = false;
  u64 revocation_generation = 0;
  std::string revocation_evidence_uuid;
  bool production_package = false;
  bool test_fixture_package = false;
  bool debug_only_package = false;
  bool cluster_route_requested = false;
  bool external_cluster_provider_attested = false;
  std::string external_cluster_provider_evidence_uuid;
  std::string provenance_evidence_uuid;
  std::string provenance_digest;
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool authorization_authority = false;
  bool security_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool reference_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool provider_finality_authority = false;
  bool cluster_authority = false;
  bool memory_authority = false;
  bool agent_action_authority = false;
};

struct AgentPackageProvenanceBundle {
  AgentPackageProvenancePolicy policy;
  std::vector<AgentPackageProvenanceRecord> records;
  bool require_plugin_record = true;
  bool require_actuator_provider_record = true;
  bool require_agent_binary_record = true;
};

struct AgentPackageProvenanceEvaluation {
  AgentRuntimeStatus status;
  bool accepted = false;
  bool failed_closed = true;
  std::string bundle_digest;
  std::vector<std::string> evidence_rows;
};

const char* AgentPackageSubjectKindName(AgentPackageSubjectKind kind);
const char* AgentPackageRevocationStatusName(
    AgentPackageRevocationStatus status);

std::string ComputeAgentPackageProvenanceDigest(
    const AgentPackageProvenanceRecord& record);
void FinalizeAgentPackageProvenanceDigest(
    AgentPackageProvenanceRecord* record);

AgentRuntimeStatus ValidateAgentPackageProvenancePolicy(
    const AgentPackageProvenancePolicy& policy);
AgentRuntimeStatus ValidateAgentPackageProvenanceRecord(
    const AgentPackageProvenanceRecord& record,
    const AgentPackageProvenancePolicy& policy);
AgentPackageProvenanceEvaluation ValidateAgentPackageProvenanceBundle(
    const AgentPackageProvenanceBundle& bundle);

}  // namespace scratchbird::core::agents
