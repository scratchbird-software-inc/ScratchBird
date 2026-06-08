// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: CEIC_070_AGENT_SYSTEM_PROFILE_DURABLE_CLAIM_LEVELS
//
// Durable public claim-level profile for operational agents. This profile is
// evidence only: it never owns transaction finality, visibility, recovery,
// parser execution, donor behavior, optimizer plans, memory authority, cluster
// authority, provider finality, security authorization, or agent actions.

#include "agent_production_classification.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents {

enum class AgentSystemProfileLiveEnablement {
  disabled,
  dry_run,
  advisory,
  live_ready,
  production_live
};

enum class AgentSystemProfileFailMode {
  fail_closed,
  disabled_on_error,
  fail_open
};

enum class AgentSystemProfileMetricStrictness {
  unknown,
  relaxed,
  strict
};

enum class AgentSystemProfilePublicClaimLevel {
  disabled,
  dry_run,
  advisory,
  live_ready,
  production_live
};

struct AgentSystemProfileForbiddenAuthority {
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool authorization_security_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool donor_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool provider_finality_authority = false;
  bool cluster_authority = false;
  bool memory_authority = false;
  bool agent_action_authority = false;
};

struct AgentSystemProfile {
  std::string agent_type_id;
  AgentSystemProfileLiveEnablement live_enablement =
      AgentSystemProfileLiveEnablement::disabled;
  AgentSystemProfileFailMode fail_mode =
      AgentSystemProfileFailMode::fail_closed;
  AgentSystemProfileMetricStrictness metric_strictness =
      AgentSystemProfileMetricStrictness::unknown;
  AgentSystemProfilePublicClaimLevel public_claim_level =
      AgentSystemProfilePublicClaimLevel::disabled;

  bool durable_profile_evidence_present = false;
  std::string durable_profile_evidence_uuid;
  std::string durable_profile_storage_digest;
  bool evidence_required = false;
  bool approval_required = false;
  bool redaction_required = false;
  bool retention_required = false;
  std::string evidence_policy_id;
  std::string approval_policy_id;
  std::string redaction_class;
  std::string retention_class;

  std::string key_policy_id;
  std::string key_policy_provenance;
  u64 key_policy_generation = 0;
  std::string signing_key_id;
  std::string signing_key_provenance;
  u64 signing_key_generation = 0;

  u64 profile_generation = 0;
  u64 issued_at_microseconds = 0;
  u64 expires_at_microseconds = 0;
  u64 max_staleness_microseconds = 0;

  std::string profile_digest_algorithm = "sha256-v1";
  std::string profile_digest;
  std::string profile_signature_algorithm = "hmac-sha256-v1";
  std::string profile_signature;

  bool durable_profile_marks_anchor_only = false;
  bool durable_profile_marks_stub_only = false;
  bool external_cluster_provider_proof_present = false;
  std::string external_cluster_provider_id;
  std::string external_cluster_provider_evidence_uuid;

  AgentSystemProfileForbiddenAuthority authority;
};

struct AgentSystemProfileValidationContext {
  AgentProductionRouteProofInputs route_inputs;
  u64 now_microseconds = 0;
};

struct AgentSystemProfileValidationResult {
  AgentRuntimeStatus status;
  AgentProductionExposureRecord exposure;
  bool live_claim = false;
  bool production_live_claim = false;
  bool durable_profile_evidence_valid = false;
  bool expired = false;
  bool stale = false;
  bool digest_valid = false;
  bool signature_valid = false;
  bool authority_clean = false;
  bool external_provider_only = false;
  std::vector<std::string> evidence_fields;
};

const char* AgentSystemProfileLiveEnablementName(
    AgentSystemProfileLiveEnablement value);
const char* AgentSystemProfileFailModeName(
    AgentSystemProfileFailMode value);
const char* AgentSystemProfileMetricStrictnessName(
    AgentSystemProfileMetricStrictness value);
const char* AgentSystemProfilePublicClaimLevelName(
    AgentSystemProfilePublicClaimLevel value);

std::string AgentSystemProfileDigest(const AgentSystemProfile& profile);
std::string AgentSystemProfileSignatureDigest(
    const AgentSystemProfile& profile);
void FinalizeAgentSystemProfile(AgentSystemProfile* profile);

AgentSystemProfileValidationResult ValidateAgentSystemProfileClaim(
    const AgentSystemProfile& profile,
    const AgentSystemProfileValidationContext& context =
        AgentSystemProfileValidationContext{});

}  // namespace scratchbird::core::agents
