// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_system_profile.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

namespace agents = scratchbird::core::agents;

constexpr agents::u64 kNowMicros = 1000000000ull;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

agents::AgentProductionRouteProofInputs::RouteProof PagePreallocationProof() {
  agents::AgentProductionRouteProofInputs::RouteProof proof;
  proof.agent_type_id = "page_allocation_manager";
  proof.action_id = "preallocate_page_family";
  proof.provider_id = "page_manager:preallocate_page_family";
  proof.actuator_id = "page_manager";
  proof.authority_domain = agents::AgentActuatorAuthorityDomain::page;
  proof.subsystem_handler_id = "storage.page.preallocate_page_family";
  proof.handler_provenance = "storage_page_preallocation_route";
  proof.handler_evidence_uuid = "018f0000-0000-7000-8000-000000070001";
  proof.live_route_available = true;
  proof.real_subsystem_handler = true;
  proof.idempotent = true;
  proof.supports_retry = true;
  proof.supports_rollback_compensation = true;
  proof.requires_outcome_verification = true;
  proof.physical_mutation_route = true;
  return proof;
}

agents::AgentSystemProfileValidationContext PageLiveContext() {
  agents::AgentSystemProfileValidationContext context;
  context.now_microseconds = kNowMicros;
  context.route_inputs.route_proofs.push_back(PagePreallocationProof());
  return context;
}

agents::AgentSystemProfileValidationContext DefaultContext() {
  agents::AgentSystemProfileValidationContext context;
  context.now_microseconds = kNowMicros;
  return context;
}

agents::AgentSystemProfile CompleteProfile(
    const std::string& agent_type_id,
    agents::AgentSystemProfileLiveEnablement live_enablement,
    agents::AgentSystemProfilePublicClaimLevel public_claim_level) {
  agents::AgentSystemProfile profile;
  profile.agent_type_id = agent_type_id;
  profile.live_enablement = live_enablement;
  profile.public_claim_level = public_claim_level;
  profile.fail_mode = agents::AgentSystemProfileFailMode::fail_closed;
  profile.metric_strictness =
      agents::AgentSystemProfileMetricStrictness::strict;
  profile.durable_profile_evidence_present = true;
  profile.durable_profile_evidence_uuid =
      "018f0000-0000-7000-8000-000000070010";
  profile.durable_profile_storage_digest = "durable-profile-storage-digest";
  profile.evidence_required = true;
  profile.approval_required = true;
  profile.redaction_required = true;
  profile.retention_required = true;
  profile.evidence_policy_id = "agent-system-profile-evidence-policy-v1";
  profile.approval_policy_id = "agent-system-profile-approval-policy-v1";
  profile.redaction_class = "standard";
  profile.retention_class = "audit";
  profile.key_policy_id = "agent-system-profile-key-policy-v1";
  profile.key_policy_provenance = "engine_local_profile_key_policy";
  profile.key_policy_generation = 1;
  profile.signing_key_id = "agent-system-profile-key-v1";
  profile.signing_key_provenance = "engine_local_profile_hmac_key";
  profile.signing_key_generation = 1;
  profile.profile_generation = 7;
  profile.issued_at_microseconds = kNowMicros - 1000;
  profile.expires_at_microseconds = kNowMicros + 1000000;
  profile.max_staleness_microseconds = 100000;
  agents::FinalizeAgentSystemProfile(&profile);
  return profile;
}

void RequireOk(const agents::AgentSystemProfile& profile,
               const agents::AgentSystemProfileValidationContext& context,
               const std::string& message) {
  const auto result = agents::ValidateAgentSystemProfileClaim(profile, context);
  Require(result.status.ok,
          message + ": " + result.status.diagnostic_code + " " +
              result.status.detail);
  Require(result.digest_valid, "profile digest was not validated: " + message);
  Require(result.signature_valid,
          "profile signature was not validated: " + message);
  Require(result.authority_clean,
          "profile authority flags were not clean: " + message);
}

void RequireCode(const agents::AgentSystemProfile& profile,
                 const agents::AgentSystemProfileValidationContext& context,
                 const std::string& expected_code,
                 const std::string& message) {
  const auto result = agents::ValidateAgentSystemProfileClaim(profile, context);
  Require(!result.status.ok,
          "profile unexpectedly passed: " + message);
  Require(result.status.diagnostic_code == expected_code,
          message + ": expected " + expected_code + " got " +
              result.status.diagnostic_code + " " + result.status.detail);
}

void TestPositiveNonclusterProfiles() {
  RequireOk(CompleteProfile(
                "page_allocation_manager",
                agents::AgentSystemProfileLiveEnablement::live_ready,
                agents::AgentSystemProfilePublicClaimLevel::live_ready),
            PageLiveContext(),
            "live-ready noncluster profile should validate with route proof");

  RequireOk(CompleteProfile(
                "storage_health_manager",
                agents::AgentSystemProfileLiveEnablement::advisory,
                agents::AgentSystemProfilePublicClaimLevel::advisory),
            DefaultContext(),
            "advisory noncluster profile should validate");

  RequireOk(CompleteProfile(
                "memory_governor",
                agents::AgentSystemProfileLiveEnablement::dry_run,
                agents::AgentSystemProfilePublicClaimLevel::dry_run),
            DefaultContext(),
            "dry-run noncluster profile should validate");

  RequireOk(CompleteProfile(
                "cluster_autoscale_manager",
                agents::AgentSystemProfileLiveEnablement::disabled,
                agents::AgentSystemProfilePublicClaimLevel::disabled),
            DefaultContext(),
            "disabled cluster profile should validate without live exposure");
}

void TestLiveClaimsNeedRealExposure() {
  RequireCode(CompleteProfile(
                  "storage_health_manager",
                  agents::AgentSystemProfileLiveEnablement::production_live,
                  agents::AgentSystemProfilePublicClaimLevel::production_live),
              DefaultContext(),
              "SB_AGENT_SYSTEM_PROFILE.PRODUCTION_LIVE_EXPOSURE_BLOCKED",
              "recommendation-only agent production-live claim");

  RequireCode(CompleteProfile(
                  "memory_governor",
                  agents::AgentSystemProfileLiveEnablement::production_live,
                  agents::AgentSystemProfilePublicClaimLevel::production_live),
              DefaultContext(),
              "SB_AGENT_SYSTEM_PROFILE.PRODUCTION_LIVE_EXPOSURE_BLOCKED",
              "dry-run-only agent production-live claim");

  auto anchor = CompleteProfile(
      "page_allocation_manager",
      agents::AgentSystemProfileLiveEnablement::production_live,
      agents::AgentSystemProfilePublicClaimLevel::production_live);
  anchor.durable_profile_marks_anchor_only = true;
  agents::FinalizeAgentSystemProfile(&anchor);
  RequireCode(anchor,
              PageLiveContext(),
              "SB_AGENT_SYSTEM_PROFILE.ANCHOR_OR_STUB_LIVE_CLAIM",
              "anchor-only live exposure");

  auto stub = CompleteProfile(
      "page_allocation_manager",
      agents::AgentSystemProfileLiveEnablement::production_live,
      agents::AgentSystemProfilePublicClaimLevel::production_live);
  stub.durable_profile_marks_stub_only = true;
  agents::FinalizeAgentSystemProfile(&stub);
  RequireCode(stub,
              PageLiveContext(),
              "SB_AGENT_SYSTEM_PROFILE.ANCHOR_OR_STUB_LIVE_CLAIM",
              "stub-only live exposure");
}

void TestClusterBoundary() {
  RequireCode(CompleteProfile(
                  "cluster_autoscale_manager",
                  agents::AgentSystemProfileLiveEnablement::production_live,
                  agents::AgentSystemProfilePublicClaimLevel::production_live),
              DefaultContext(),
              "SB_AGENT_SYSTEM_PROFILE.CLUSTER_EXTERNAL_PROVIDER_REQUIRED",
              "local cluster live exposure");

  auto external = CompleteProfile(
      "cluster_autoscale_manager",
      agents::AgentSystemProfileLiveEnablement::production_live,
      agents::AgentSystemProfilePublicClaimLevel::production_live);
  external.external_cluster_provider_proof_present = true;
  external.external_cluster_provider_id = "closed-source-cluster-provider";
  external.external_cluster_provider_evidence_uuid =
      "018f0000-0000-7000-8000-000000070099";
  agents::FinalizeAgentSystemProfile(&external);
  auto context = DefaultContext();
  context.route_inputs.real_cluster_provider_authority = true;
  const auto result = agents::ValidateAgentSystemProfileClaim(external, context);
  Require(result.status.ok,
          "external cluster provider proof should validate as provider-only: " +
              result.status.diagnostic_code);
  Require(result.external_provider_only,
          "cluster profile did not classify external-provider-only");
  Require(result.status.diagnostic_code ==
              "SB_AGENT_SYSTEM_PROFILE.EXTERNAL_CLUSTER_PROVIDER_ONLY",
          "cluster profile did not use provider-only diagnostic");
}

void TestDurableProfileIntegrityRefusals() {
  auto expired = CompleteProfile(
      "page_allocation_manager",
      agents::AgentSystemProfileLiveEnablement::production_live,
      agents::AgentSystemProfilePublicClaimLevel::production_live);
  expired.expires_at_microseconds = kNowMicros - 1;
  agents::FinalizeAgentSystemProfile(&expired);
  RequireCode(expired,
              PageLiveContext(),
              "SB_AGENT_SYSTEM_PROFILE.PROFILE_EXPIRED",
              "expired profile");

  auto stale = CompleteProfile(
      "page_allocation_manager",
      agents::AgentSystemProfileLiveEnablement::production_live,
      agents::AgentSystemProfilePublicClaimLevel::production_live);
  stale.max_staleness_microseconds = 1;
  agents::FinalizeAgentSystemProfile(&stale);
  RequireCode(stale,
              PageLiveContext(),
              "SB_AGENT_SYSTEM_PROFILE.PROFILE_STALE",
              "stale profile");

  auto unsigned_profile = CompleteProfile(
      "page_allocation_manager",
      agents::AgentSystemProfileLiveEnablement::production_live,
      agents::AgentSystemProfilePublicClaimLevel::production_live);
  unsigned_profile.profile_signature.clear();
  RequireCode(unsigned_profile,
              PageLiveContext(),
              "SB_AGENT_SYSTEM_PROFILE.PROFILE_SIGNATURE_INVALID",
              "unsigned profile");

  auto missing_digest = CompleteProfile(
      "page_allocation_manager",
      agents::AgentSystemProfileLiveEnablement::production_live,
      agents::AgentSystemProfilePublicClaimLevel::production_live);
  missing_digest.profile_digest.clear();
  RequireCode(missing_digest,
              PageLiveContext(),
              "SB_AGENT_SYSTEM_PROFILE.PROFILE_DIGEST_INVALID",
              "missing profile digest");
}

void TestRequirementRefusals() {
  auto relaxed = CompleteProfile(
      "page_allocation_manager",
      agents::AgentSystemProfileLiveEnablement::production_live,
      agents::AgentSystemProfilePublicClaimLevel::production_live);
  relaxed.metric_strictness =
      agents::AgentSystemProfileMetricStrictness::relaxed;
  agents::FinalizeAgentSystemProfile(&relaxed);
  RequireCode(relaxed,
              PageLiveContext(),
              "SB_AGENT_SYSTEM_PROFILE.STRICT_METRICS_REQUIRED",
              "relaxed production metrics");

  auto missing_evidence = CompleteProfile(
      "page_allocation_manager",
      agents::AgentSystemProfileLiveEnablement::production_live,
      agents::AgentSystemProfilePublicClaimLevel::production_live);
  missing_evidence.durable_profile_evidence_present = false;
  missing_evidence.durable_profile_evidence_uuid.clear();
  RequireCode(
      missing_evidence,
      PageLiveContext(),
      "SB_AGENT_SYSTEM_PROFILE.DURABLE_PROFILE_EVIDENCE_REQUIRED",
      "missing durable evidence");

  auto missing_approval = CompleteProfile(
      "page_allocation_manager",
      agents::AgentSystemProfileLiveEnablement::production_live,
      agents::AgentSystemProfilePublicClaimLevel::production_live);
  missing_approval.approval_required = false;
  missing_approval.approval_policy_id.clear();
  RequireCode(missing_approval,
              PageLiveContext(),
              "SB_AGENT_SYSTEM_PROFILE.APPROVAL_REQUIREMENT_MISSING",
              "missing approval requirement");

  auto missing_redaction = CompleteProfile(
      "page_allocation_manager",
      agents::AgentSystemProfileLiveEnablement::production_live,
      agents::AgentSystemProfilePublicClaimLevel::production_live);
  missing_redaction.redaction_class = "unredacted";
  agents::FinalizeAgentSystemProfile(&missing_redaction);
  RequireCode(missing_redaction,
              PageLiveContext(),
              "SB_AGENT_SYSTEM_PROFILE.REDACTION_REQUIREMENT_MISSING",
              "missing redaction requirement");

  auto missing_retention = CompleteProfile(
      "page_allocation_manager",
      agents::AgentSystemProfileLiveEnablement::production_live,
      agents::AgentSystemProfilePublicClaimLevel::production_live);
  missing_retention.retention_required = false;
  missing_retention.retention_class.clear();
  RequireCode(missing_retention,
              PageLiveContext(),
              "SB_AGENT_SYSTEM_PROFILE.RETENTION_REQUIREMENT_MISSING",
              "missing retention requirement");

  auto missing_key = CompleteProfile(
      "page_allocation_manager",
      agents::AgentSystemProfileLiveEnablement::production_live,
      agents::AgentSystemProfilePublicClaimLevel::production_live);
  missing_key.key_policy_id.clear();
  missing_key.signing_key_id.clear();
  RequireCode(missing_key,
              PageLiveContext(),
              "SB_AGENT_SYSTEM_PROFILE.KEY_POLICY_REQUIRED",
              "missing key policy");
}

void TestForbiddenAuthorityDrift() {
  auto profile = CompleteProfile(
      "page_allocation_manager",
      agents::AgentSystemProfileLiveEnablement::production_live,
      agents::AgentSystemProfilePublicClaimLevel::production_live);
  profile.authority.transaction_finality_authority = true;
  agents::FinalizeAgentSystemProfile(&profile);
  RequireCode(profile,
              PageLiveContext(),
              "SB_AGENT_SYSTEM_PROFILE.FORBIDDEN_AUTHORITY",
              "transaction finality authority drift");

  profile = CompleteProfile(
      "page_allocation_manager",
      agents::AgentSystemProfileLiveEnablement::production_live,
      agents::AgentSystemProfilePublicClaimLevel::production_live);
  profile.authority.cluster_authority = true;
  agents::FinalizeAgentSystemProfile(&profile);
  RequireCode(profile,
              PageLiveContext(),
              "SB_AGENT_SYSTEM_PROFILE.FORBIDDEN_AUTHORITY",
              "cluster authority drift");
}

}  // namespace

int main() {
  TestPositiveNonclusterProfiles();
  TestLiveClaimsNeedRealExposure();
  TestClusterBoundary();
  TestDurableProfileIntegrityRefusals();
  TestRequirementRefusals();
  TestForbiddenAuthorityDrift();
  return EXIT_SUCCESS;
}
