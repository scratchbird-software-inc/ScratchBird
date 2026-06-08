// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_policy_lifecycle.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

namespace agents = scratchbird::core::agents;

constexpr agents::u64 kNowMicros = 2000000000ull;
constexpr agents::u64 kEngineGeneration = 11;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

const agents::AgentTypeDescriptor& Descriptor(const std::string& agent_type_id) {
  const auto descriptor = agents::FindAgentType(agent_type_id);
  Require(descriptor.has_value(), "missing descriptor: " + agent_type_id);
  static agents::AgentTypeDescriptor storage;
  storage = *descriptor;
  return storage;
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
  proof.handler_evidence_uuid = "018f0000-0000-7000-8000-000000071001";
  proof.live_route_available = true;
  proof.real_subsystem_handler = true;
  proof.idempotent = true;
  proof.supports_retry = true;
  proof.supports_rollback_compensation = true;
  proof.requires_outcome_verification = true;
  proof.physical_mutation_route = true;
  return proof;
}

agents::AgentSystemProfileValidationContext PageProfileContext() {
  agents::AgentSystemProfileValidationContext context;
  context.now_microseconds = kNowMicros;
  context.route_inputs.route_proofs.push_back(PagePreallocationProof());
  return context;
}

agents::AgentSystemProfile ProductionProfile() {
  agents::AgentSystemProfile profile;
  profile.agent_type_id = "page_allocation_manager";
  profile.live_enablement =
      agents::AgentSystemProfileLiveEnablement::production_live;
  profile.public_claim_level =
      agents::AgentSystemProfilePublicClaimLevel::production_live;
  profile.fail_mode = agents::AgentSystemProfileFailMode::fail_closed;
  profile.metric_strictness =
      agents::AgentSystemProfileMetricStrictness::strict;
  profile.durable_profile_evidence_present = true;
  profile.durable_profile_evidence_uuid =
      "018f0000-0000-7000-8000-000000071010";
  profile.durable_profile_storage_digest = "profile-storage-digest";
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
  profile.expires_at_microseconds = kNowMicros + 5000000;
  profile.max_staleness_microseconds = 1000000;
  agents::FinalizeAgentSystemProfile(&profile);
  return profile;
}

agents::AgentPolicy LivePagePolicy() {
  agents::AgentPolicy policy = agents::BaselinePolicyForAgentFamily(
      Descriptor("page_allocation_manager"), "page_preallocation_policy", 7);
  policy.activation = agents::AgentActivationProfile::live_action;
  policy.allow_live_action = true;
  policy.action_mode = "request_action";
  policy.require_manual_approval = true;
  policy.require_dry_run_before_live = true;
  policy.evidence_required = true;
  policy.explainability_required = true;
  policy.action_budget_per_window = 1;
  return policy;
}

agents::AgentPolicy AdvisoryStoragePolicy() {
  agents::AgentPolicy policy = agents::BaselinePolicyForAgentFamily(
      Descriptor("storage_health_manager"), "storage_health_policy", 3);
  policy.activation = agents::AgentActivationProfile::recommend_only;
  policy.allow_live_action = false;
  policy.action_mode = "recommend_only";
  return policy;
}

agents::AgentPolicy DisabledMemoryPolicy() {
  agents::AgentPolicy policy = agents::BaselinePolicyForAgentFamily(
      Descriptor("memory_governor"), "memory_governor_policy", 2);
  policy.enabled = false;
  policy.activation = agents::AgentActivationProfile::disabled;
  policy.allow_live_action = false;
  policy.action_mode = "disabled";
  return policy;
}

agents::AgentSignedPolicyLifecycle LifecycleForPolicy(
    const agents::AgentPolicy& policy,
    const std::string& agent_type_id,
    agents::AgentPolicyLifecycleState state,
    bool production_live) {
  agents::AgentSignedPolicyLifecycle lifecycle;
  lifecycle.agent_type_id = agent_type_id;
  lifecycle.policy = policy;
  lifecycle.typed_fields = agents::AgentPolicyTypedFieldEvidenceFromPolicy(policy);
  lifecycle.author_principal_uuid =
      "018f0000-0000-7000-8000-000000071020";
  lifecycle.approver_principal_uuid =
      "018f0000-0000-7000-8000-000000071021";
  lifecycle.approval_evidence_uuid =
      "018f0000-0000-7000-8000-000000071022";
  lifecycle.lifecycle_state = state;
  lifecycle.generation = policy.policy_generation;
  lifecycle.issued_at_microseconds = kNowMicros - 1000;
  lifecycle.activation_time_microseconds = kNowMicros - 500;
  lifecycle.expiry_time_microseconds = kNowMicros + 5000000;
  lifecycle.max_staleness_microseconds = 1000000;
  lifecycle.supersedes_policy_uuid =
      "018f0000-0000-7000-8000-000000071030";
  lifecycle.supersedes_policy_generation =
      policy.policy_generation > 1 ? policy.policy_generation - 1 : 0;
  if (production_live) {
    lifecycle.rollback_policy_uuid =
        "018f0000-0000-7000-8000-000000071031";
    lifecycle.rollback_policy_generation =
        policy.policy_generation > 1 ? policy.policy_generation - 1 : 0;
  }
  lifecycle.engine_min_generation = 1;
  lifecycle.engine_max_generation = 100;
  lifecycle.key_policy_id = "agent-policy-key-policy-v1";
  lifecycle.key_policy_provenance = "engine_local_policy_key_policy";
  lifecycle.key_policy_generation = 1;
  lifecycle.signing_key_id = "agent-policy-signing-key-v1";
  lifecycle.signing_key_provenance = "engine_local_policy_hmac_key";
  lifecycle.signing_key_generation = 1;
  lifecycle.production_live_policy = production_live;
  agents::FinalizeAgentSignedPolicyLifecycle(&lifecycle);
  return lifecycle;
}

void CoupleProfile(agents::AgentSignedPolicyLifecycle* lifecycle,
                   const agents::AgentSystemProfile& profile) {
  lifecycle->coupled_profile_agent_type_id = profile.agent_type_id;
  lifecycle->coupled_profile_generation = profile.profile_generation;
  lifecycle->coupled_profile_digest = profile.profile_digest;
  agents::FinalizeAgentSignedPolicyLifecycle(lifecycle);
}

agents::AgentPolicyLifecycleValidationContext Context(
    const agents::AgentSystemProfile* profile = nullptr) {
  agents::AgentPolicyLifecycleValidationContext context;
  context.now_microseconds = kNowMicros;
  context.engine_generation = kEngineGeneration;
  context.production_environment = true;
  context.system_profile = profile;
  context.profile_context = PageProfileContext();
  return context;
}

void RequireOk(const agents::AgentSignedPolicyLifecycle& lifecycle,
               const agents::AgentPolicyLifecycleValidationContext& context,
               const std::string& message) {
  const auto result =
      agents::ValidateAgentSignedPolicyLifecycle(lifecycle, context);
  Require(result.status.ok,
          message + ": " + result.status.diagnostic_code + " " +
              result.status.detail);
  Require(result.digest_valid, "digest not valid: " + message);
  Require(result.signature_valid, "signature not valid: " + message);
  Require(result.typed_fields_valid, "typed fields not valid: " + message);
  Require(result.lifecycle_time_valid, "lifecycle time not valid: " + message);
  Require(result.engine_bounds_valid, "engine bounds not valid: " + message);
  Require(result.authority_clean, "authority not clean: " + message);
}

void RequireCode(const agents::AgentSignedPolicyLifecycle& lifecycle,
                 const agents::AgentPolicyLifecycleValidationContext& context,
                 const std::string& expected,
                 const std::string& message) {
  const auto result =
      agents::ValidateAgentSignedPolicyLifecycle(lifecycle, context);
  Require(!result.status.ok, "unexpected policy pass: " + message);
  Require(result.status.diagnostic_code == expected,
          message + ": expected " + expected + " got " +
              result.status.diagnostic_code + " " + result.status.detail);
}

agents::AgentPolicyTypedFieldEvidence* Field(
    agents::AgentSignedPolicyLifecycle* lifecycle,
    const std::string& name) {
  for (auto& field : lifecycle->typed_fields) {
    if (field.name == name) { return &field; }
  }
  Fail("missing typed field: " + name);
}

void TestPositivePolicies() {
  const auto profile = ProductionProfile();
  auto live = LifecycleForPolicy(
      LivePagePolicy(), "page_allocation_manager",
      agents::AgentPolicyLifecycleState::active, true);
  CoupleProfile(&live, profile);
  RequireOk(live, Context(&profile), "production live policy");

  auto advisory = LifecycleForPolicy(
      AdvisoryStoragePolicy(), "storage_health_manager",
      agents::AgentPolicyLifecycleState::advisory, false);
  RequireOk(advisory, Context(), "advisory policy");

  auto disabled = LifecycleForPolicy(
      DisabledMemoryPolicy(), "memory_governor",
      agents::AgentPolicyLifecycleState::disabled, false);
  RequireOk(disabled, Context(), "disabled policy");
}

void TestIdentitySignatureAndLifecycleRefusals() {
  const auto profile = ProductionProfile();
  auto lifecycle = LifecycleForPolicy(
      LivePagePolicy(), "page_allocation_manager",
      agents::AgentPolicyLifecycleState::active, true);
  CoupleProfile(&lifecycle, profile);

  auto missing_author = lifecycle;
  missing_author.author_principal_uuid.clear();
  agents::FinalizeAgentSignedPolicyLifecycle(&missing_author);
  RequireCode(missing_author, Context(&profile),
              "SB_AGENT_POLICY_LIFECYCLE.AUTHOR_APPROVER_REQUIRED",
              "missing author");

  auto unsigned_policy = lifecycle;
  unsigned_policy.policy_signature.clear();
  RequireCode(unsigned_policy, Context(&profile),
              "SB_AGENT_POLICY_LIFECYCLE.POLICY_SIGNATURE_INVALID",
              "unsigned policy");

  auto tampered = lifecycle;
  tampered.policy.action_budget_per_window = 2;
  RequireCode(tampered, Context(&profile),
              "SB_AGENT_POLICY_LIFECYCLE.POLICY_DIGEST_INVALID",
              "tampered policy digest");

  auto not_active = lifecycle;
  not_active.activation_time_microseconds = kNowMicros + 1000;
  not_active.expiry_time_microseconds = kNowMicros + 5000000;
  agents::FinalizeAgentSignedPolicyLifecycle(&not_active);
  RequireCode(not_active, Context(&profile),
              "SB_AGENT_POLICY_LIFECYCLE.NOT_YET_ACTIVE",
              "not yet active");

  auto expired = lifecycle;
  expired.expiry_time_microseconds = kNowMicros - 1;
  agents::FinalizeAgentSignedPolicyLifecycle(&expired);
  RequireCode(expired, Context(&profile),
              "SB_AGENT_POLICY_LIFECYCLE.EXPIRED",
              "expired policy");

  auto bad_key = lifecycle;
  bad_key.key_policy_id = "agent-policy-fixture-key-policy-v1";
  bad_key.key_policy_provenance = "fixture_policy_key_policy";
  bad_key.signing_key_id = "agent-policy-fixture-signing-key-v1";
  bad_key.signing_key_provenance = "fixture_policy_hmac_key";
  agents::FinalizeAgentSignedPolicyLifecycle(&bad_key);
  RequireCode(bad_key, Context(&profile),
              "SB_AGENT_POLICY_LIFECYCLE.PRODUCTION_KEY_POLICY_REQUIRED",
              "fixture key in production");
}

void TestTypedFieldRefusals() {
  const auto profile = ProductionProfile();
  auto lifecycle = LifecycleForPolicy(
      LivePagePolicy(), "page_allocation_manager",
      agents::AgentPolicyLifecycleState::active, true);
  CoupleProfile(&lifecycle, profile);

  auto bad_type = lifecycle;
  Field(&bad_type, "preallocation_allowed")->type =
      agents::AgentPolicyFieldType::token;
  agents::FinalizeAgentSignedPolicyLifecycle(&bad_type);
  RequireCode(bad_type, Context(&profile),
              "SB_AGENT_POLICY_LIFECYCLE.FIELD_TYPE_MISMATCH",
              "typed field type mismatch");

  auto bad_unit = lifecycle;
  Field(&bad_unit, "preallocation_allowed")->units = "wrong_units";
  agents::FinalizeAgentSignedPolicyLifecycle(&bad_unit);
  RequireCode(bad_unit, Context(&profile),
              "SB_AGENT_POLICY_LIFECYCLE.FIELD_UNIT_MISMATCH",
              "typed field unit mismatch");

  auto bad_range = lifecycle;
  Field(&bad_range, "history_confidence_threshold")->maximum = 2.0;
  agents::FinalizeAgentSignedPolicyLifecycle(&bad_range);
  RequireCode(bad_range, Context(&profile),
              "SB_AGENT_POLICY_LIFECYCLE.FIELD_RANGE_MISMATCH",
              "typed field range mismatch");

  auto sensitive = lifecycle;
  Field(&sensitive, "allowed_page_families")->value = "secret:page-family";
  agents::FinalizeAgentSignedPolicyLifecycle(&sensitive);
  RequireCode(sensitive, Context(&profile),
              "SB_AGENT_POLICY_LIFECYCLE.SENSITIVE_VALUE_FORBIDDEN",
              "sensitive typed value");
}

void TestGenerationBoundsProfileAndAuthorityRefusals() {
  const auto profile = ProductionProfile();
  auto lifecycle = LifecycleForPolicy(
      LivePagePolicy(), "page_allocation_manager",
      agents::AgentPolicyLifecycleState::active, true);
  CoupleProfile(&lifecycle, profile);

  auto bad_supersedes = lifecycle;
  bad_supersedes.supersedes_policy_generation = bad_supersedes.generation;
  agents::FinalizeAgentSignedPolicyLifecycle(&bad_supersedes);
  RequireCode(bad_supersedes, Context(&profile),
              "SB_AGENT_POLICY_LIFECYCLE.SUPERSEDES_INVALID",
              "bad supersedes generation");

  auto bad_rollback = lifecycle;
  bad_rollback.rollback_policy_generation = bad_rollback.generation;
  agents::FinalizeAgentSignedPolicyLifecycle(&bad_rollback);
  RequireCode(bad_rollback, Context(&profile),
              "SB_AGENT_POLICY_LIFECYCLE.ROLLBACK_INVALID",
              "bad rollback generation");

  auto bounds = lifecycle;
  bounds.engine_min_generation = 20;
  bounds.engine_max_generation = 30;
  agents::FinalizeAgentSignedPolicyLifecycle(&bounds);
  RequireCode(bounds, Context(&profile),
              "SB_AGENT_POLICY_LIFECYCLE.ENGINE_BOUNDS_VIOLATION",
              "engine generation bounds");

  auto profile_mismatch = lifecycle;
  profile_mismatch.coupled_profile_generation = profile.profile_generation + 1;
  agents::FinalizeAgentSignedPolicyLifecycle(&profile_mismatch);
  RequireCode(profile_mismatch, Context(&profile),
              "SB_AGENT_POLICY_LIFECYCLE.PROFILE_GENERATION_MISMATCH",
              "profile generation mismatch");

  auto local_cluster = lifecycle;
  local_cluster.local_cluster_policy_claim = true;
  agents::FinalizeAgentSignedPolicyLifecycle(&local_cluster);
  RequireCode(local_cluster, Context(&profile),
              "SB_AGENT_POLICY_LIFECYCLE.LOCAL_CLUSTER_POLICY_FORBIDDEN",
              "local cluster policy claim");

  auto authority = lifecycle;
  authority.no_authority.transaction_finality_authority = true;
  agents::FinalizeAgentSignedPolicyLifecycle(&authority);
  RequireCode(authority, Context(&profile),
              "SB_AGENT_POLICY_LIFECYCLE.FORBIDDEN_AUTHORITY",
              "transaction authority drift");
}

}  // namespace

int main() {
  TestPositivePolicies();
  TestIdentitySignatureAndLifecycleRefusals();
  TestTypedFieldRefusals();
  TestGenerationBoundsProfileAndAuthorityRefusals();
  return EXIT_SUCCESS;
}
