// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: ARHC_COMMERCIAL_AUDIT_EVIDENCE_PERSISTENCE
// SEARCH_KEY: ARHC_AGENT_EVIDENCE_REDACTION_RETENTION_TAMPER

#include "agent_action_dispatch.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace scratchbird::core::agents {

struct CommercialAgentEvidenceKeyPolicy {
  std::string policy_id = "agent-evidence-key-policy-v1";
  u64 policy_generation = 1;
  std::string key_id = "agent-evidence-ledger-key-v1";
  std::string key_provenance = "engine_local_protected_hmac_key";
  u64 key_generation = 1;
  u64 key_rotation_epoch = 1;
  u64 key_not_before_microseconds = 1;
  u64 key_not_after_microseconds = 0;
  std::string key_residency_class = "engine_local_protected";
  std::string data_residency_class = "engine_local";
  bool production_key_material = true;
  bool test_key_material = false;
  bool key_material_exported = false;
  bool allow_test_fixture_key = false;
  bool legal_hold_active = false;
  bool require_redaction_before_buffering = true;
  bool require_storage_linkage = true;
};

struct CommercialAgentEvidenceBuildRequest {
  AgentActionRequest action;
  AgentActionAuthorityProvenance authority;
  std::string provider_id;
  std::string input_evidence_digest;
  std::string input_metric_digest;
  u64 policy_generation = 0;
  std::vector<std::string> scope_uuids;
  std::string decision_payload;
  std::string result_state;
  std::string diagnostic_code;
  std::string redaction_class = "standard";
  std::string retention_class = "audit";
  std::string outcome_verification_evidence_uuid;
  std::string previous_tamper_digest;
  std::string tamper_key_id = "agent-evidence-ledger-key-v1";
  std::string tamper_key_provenance = "engine_local_protected_hmac_key";
  u64 tamper_key_generation = 1;
  std::string evidence_key_policy_id = "agent-evidence-key-policy-v1";
  u64 tamper_key_rotation_epoch = 1;
  u64 tamper_key_not_before_microseconds = 1;
  u64 tamper_key_not_after_microseconds = 0;
  std::string key_residency_class = "engine_local_protected";
  std::string data_residency_class = "engine_local";
  std::string storage_linkage_digest;
  u64 created_at_microseconds = 0;
  u64 expires_at_microseconds = 0;
  bool legal_hold_active = false;
  bool production_key_material = true;
  bool test_key_material = false;
  bool key_material_exported = false;
  bool protected_material_present = false;
};

struct CommercialAgentEvidenceValidation {
  AgentRuntimeStatus status;
  bool tamper_valid = false;
  bool expired = false;
  bool authority_clean = false;
  bool key_policy_valid = false;
  bool residency_valid = false;
  bool retention_valid = false;
  bool redaction_before_buffering = false;
  bool storage_linked = false;
};

struct CommercialAgentEvidenceChainValidationRequest {
  std::vector<AgentEvidenceRecord> evidence;
  CommercialAgentEvidenceKeyPolicy key_policy;
  std::string expected_initial_previous_digest =
      "scratchbird-agent-evidence-ledger-genesis";
  u64 now_microseconds = 0;
  bool production_live_path = true;
};

struct CommercialAgentEvidenceChainValidation {
  AgentRuntimeStatus status;
  std::size_t validated_records = 0;
  bool chain_continuity_valid = false;
  bool key_policy_valid = false;
};

struct CommercialAgentEvidenceViewRequest {
  AgentEvidenceRecord evidence;
  AgentRuntimeContext context;
  bool support_bundle_view = true;
  u64 now_microseconds = 0;
};

struct CommercialAgentEvidenceViewResult {
  AgentRuntimeStatus status;
  AgentSecurityGrantDecision grant;
  AgentEvidenceRecord evidence;
  bool visible = false;
  bool redacted = false;
  bool expired = false;
  bool tamper_valid = false;
  bool protected_material_suppressed = false;
};

std::string CommercialAgentEvidenceTamperDigest(
    const AgentEvidenceRecord& evidence);
std::string CommercialAgentEvidenceChainDigest(
    const AgentEvidenceRecord& evidence);
std::string CommercialAgentEvidenceSignatureDigest(
    const AgentEvidenceRecord& evidence);
void FinalizeCommercialAgentEvidenceDigests(AgentEvidenceRecord* evidence);
bool CommercialAgentEvidenceExpired(const AgentEvidenceRecord& evidence,
                                    u64 now_microseconds);
CommercialAgentEvidenceKeyPolicy DefaultCommercialAgentEvidenceKeyPolicy();
AgentRuntimeStatus ValidateCommercialAgentEvidenceKeyPolicy(
    const AgentEvidenceRecord& evidence,
    const CommercialAgentEvidenceKeyPolicy& key_policy,
    bool production_live_path = true);
AgentEvidenceRecord BuildCommercialAgentEvidence(
    const CommercialAgentEvidenceBuildRequest& request);
CommercialAgentEvidenceValidation ValidateCommercialAgentEvidence(
    const AgentEvidenceRecord& evidence,
    u64 now_microseconds = 0);
CommercialAgentEvidenceChainValidation ValidateCommercialAgentEvidenceChain(
    const CommercialAgentEvidenceChainValidationRequest& request);
CommercialAgentEvidenceViewResult ProjectCommercialAgentEvidenceView(
    const CommercialAgentEvidenceViewRequest& request);

}  // namespace scratchbird::core::agents
