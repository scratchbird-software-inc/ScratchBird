// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_policy_override_resolution.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;

constexpr agents::u64 kNowMicros = 3000000000ull;
constexpr agents::u64 kEngineGeneration = 12;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

const agents::AgentTypeDescriptor& Descriptor(const std::string& id) {
  const auto descriptor = agents::FindAgentType(id);
  Require(descriptor.has_value(), "missing descriptor: " + id);
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
  proof.handler_evidence_uuid = "018f0000-0000-7000-8000-000000072001";
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
      "018f0000-0000-7000-8000-000000072010";
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

agents::AgentPolicy LivePagePolicy(agents::u64 generation) {
  agents::AgentPolicy policy = agents::BaselinePolicyForAgentFamily(
      Descriptor("page_allocation_manager"), "page_preallocation_policy",
      generation);
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
  auto policy = agents::BaselinePolicyForAgentFamily(
      Descriptor("storage_health_manager"), "storage_health_policy", 3);
  policy.activation = agents::AgentActivationProfile::recommend_only;
  policy.allow_live_action = false;
  policy.action_mode = "recommend_only";
  return policy;
}

agents::AgentPolicy DisabledMemoryPolicy() {
  auto policy = agents::BaselinePolicyForAgentFamily(
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
  lifecycle.typed_fields =
      agents::AgentPolicyTypedFieldEvidenceFromPolicy(policy);
  lifecycle.author_principal_uuid =
      "018f0000-0000-7000-8000-000000072020";
  lifecycle.approver_principal_uuid =
      "018f0000-0000-7000-8000-000000072021";
  lifecycle.approval_evidence_uuid =
      "018f0000-0000-7000-8000-000000072022";
  lifecycle.lifecycle_state = state;
  lifecycle.generation = policy.policy_generation;
  lifecycle.issued_at_microseconds = kNowMicros - 1000;
  lifecycle.activation_time_microseconds = kNowMicros - 500;
  lifecycle.expiry_time_microseconds = kNowMicros + 5000000;
  lifecycle.max_staleness_microseconds = 1000000;
  lifecycle.supersedes_policy_uuid =
      "018f0000-0000-7000-8000-000000072030";
  lifecycle.supersedes_policy_generation =
      policy.policy_generation > 1 ? policy.policy_generation - 1 : 0;
  if (production_live) {
    lifecycle.rollback_policy_uuid =
        "018f0000-0000-7000-8000-000000072031";
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

agents::AgentPolicyOverrideResolutionContext Context(
    const agents::AgentSystemProfile* profile = nullptr) {
  agents::AgentPolicyOverrideResolutionContext context;
  context.now_microseconds = kNowMicros;
  context.production_environment = true;
  context.lifecycle_context.now_microseconds = kNowMicros;
  context.lifecycle_context.engine_generation = kEngineGeneration;
  context.lifecycle_context.production_environment = true;
  context.lifecycle_context.system_profile = profile;
  context.lifecycle_context.profile_context = PageProfileContext();
  return context;
}

agents::AgentPolicyOverrideApproval Approval(
    std::vector<std::string> fields,
    agents::u64 suffix = 1) {
  agents::AgentPolicyOverrideApproval approval;
  approval.override_permission_granted = true;
  approval.approval_uuid =
      "018f0000-0000-7000-8000-0000000721" + std::to_string(suffix);
  approval.approver_principal_uuid =
      "018f0000-0000-7000-8000-0000000722" + std::to_string(suffix);
  approval.created_at_microseconds = kNowMicros - 1000;
  approval.expires_at_microseconds = kNowMicros + 5000000;
  approval.allowed_override_fields = std::move(fields);
  return approval;
}

agents::AgentPolicyOverrideLayer Layer(
    agents::AgentPolicyScopeKind kind,
    const std::string& scope_uuid,
    agents::AgentSignedPolicyLifecycle lifecycle,
    agents::AgentPolicyOverrideApproval approval = {}) {
  agents::AgentPolicyOverrideLayer layer;
  layer.scope_kind = kind;
  layer.scope_uuid = scope_uuid;
  layer.lifecycle = std::move(lifecycle);
  layer.approval = std::move(approval);
  return layer;
}

agents::AgentSignedPolicyLifecycle LiveLifecycle(
    const agents::AgentSystemProfile& profile,
    agents::u64 generation,
    const std::string& field = {},
    const std::string& value = {}) {
  auto policy = LivePagePolicy(generation);
  if (!field.empty()) { policy.config_fields[field] = value; }
  auto lifecycle = LifecycleForPolicy(
      policy, "page_allocation_manager",
      agents::AgentPolicyLifecycleState::active, true);
  CoupleProfile(&lifecycle, profile);
  return lifecycle;
}

void RequireOk(const agents::AgentPolicyOverrideResolutionResult& result,
               const std::string& message) {
  Require(result.status.ok,
          message + ": " + result.status.diagnostic_code + " " +
              result.status.detail);
  Require(result.root_present, "root not present: " + message);
  Require(result.order_valid, "order not valid: " + message);
  Require(result.all_lifecycles_valid, "lifecycles not valid: " + message);
  Require(result.conflicts_absent, "conflicts present: " + message);
  Require(result.digest_valid, "digest not valid: " + message);
  Require(result.authority_clean, "authority not clean: " + message);
}

void RequireCode(const std::vector<agents::AgentPolicyOverrideLayer>& layers,
                 const agents::AgentPolicyOverrideResolutionContext& context,
                 const std::string& expected,
                 const std::string& message) {
  const auto result = agents::ResolveAgentPolicyOverrides(layers, context);
  Require(!result.status.ok, "unexpected resolver pass: " + message);
  Require(result.status.diagnostic_code == expected,
          message + ": expected " + expected + " got " +
              result.status.diagnostic_code + " " + result.status.detail);
}

void TestPositiveResolution() {
  const auto profile = ProductionProfile();
  auto root = LiveLifecycle(profile, 7);
  const auto root_result = agents::ResolveAgentPolicyOverrides(
      {Layer(agents::AgentPolicyScopeKind::root,
             "018f0000-0000-7000-8000-000000072100", root)},
      Context(&profile));
  RequireOk(root_result, "root-only live resolution");
  Require(root_result.resolved_fields.size() == root.policy.config_fields.size(),
          "root-only field count mismatch");

  auto database = LiveLifecycle(profile, 8, "allocation_throttle_pages_per_second",
                                "2");
  auto tenant = LiveLifecycle(profile, 9, "max_memory_bytes", "4096");
  auto session = LiveLifecycle(profile, 10, "max_queue_depth", "2");
  const auto layered = agents::ResolveAgentPolicyOverrides(
      {Layer(agents::AgentPolicyScopeKind::root,
             "018f0000-0000-7000-8000-000000072101", root),
       Layer(agents::AgentPolicyScopeKind::database,
             "018f0000-0000-7000-8000-000000072102", database,
             Approval({"allocation_throttle_pages_per_second"}, 1)),
       Layer(agents::AgentPolicyScopeKind::tenant,
             "018f0000-0000-7000-8000-000000072103", tenant,
             Approval({"max_memory_bytes"}, 2)),
       Layer(agents::AgentPolicyScopeKind::session,
             "018f0000-0000-7000-8000-000000072104", session,
             Approval({"max_queue_depth"}, 3))},
      Context(&profile));
  RequireOk(layered, "layered live resolution");
  Require(layered.applied_layers.size() == 4, "layer count mismatch");
  Require(layered.resolved_lifecycle.policy.config_fields.at(
              "allocation_throttle_pages_per_second") == "2",
          "database override missing");
  Require(layered.resolved_lifecycle.policy.config_fields.at("max_memory_bytes") ==
              "4096",
          "tenant override missing");
  Require(layered.resolved_lifecycle.policy.config_fields.at("max_queue_depth") ==
              "2",
          "session override missing");
}

void TestAdvisoryAndDisabledResolution() {
  auto advisory = LifecycleForPolicy(
      AdvisoryStoragePolicy(), "storage_health_manager",
      agents::AgentPolicyLifecycleState::advisory, false);
  const auto advisory_result = agents::ResolveAgentPolicyOverrides(
      {Layer(agents::AgentPolicyScopeKind::root,
             "018f0000-0000-7000-8000-000000072110", advisory)},
      Context());
  RequireOk(advisory_result, "advisory resolution");

  auto disabled = LifecycleForPolicy(
      DisabledMemoryPolicy(), "memory_governor",
      agents::AgentPolicyLifecycleState::disabled, false);
  const auto disabled_result = agents::ResolveAgentPolicyOverrides(
      {Layer(agents::AgentPolicyScopeKind::root,
             "018f0000-0000-7000-8000-000000072111", disabled)},
      Context());
  RequireOk(disabled_result, "disabled resolution");
}

void TestNegativeResolution() {
  const auto profile = ProductionProfile();
  const auto root = LiveLifecycle(profile, 7);
  const auto database = LiveLifecycle(
      profile, 8, "allocation_throttle_pages_per_second", "2");
  const auto tenant = LiveLifecycle(profile, 9, "max_memory_bytes", "4096");

  RequireCode({Layer(agents::AgentPolicyScopeKind::database,
                     "018f0000-0000-7000-8000-000000072201", database)},
              Context(&profile), "SB_AGENT_POLICY_OVERRIDE.ROOT_REQUIRED",
              "missing root");

  auto invalid = root;
  invalid.policy.config_fields["allocation_throttle_pages_per_second"] = "3";
  RequireCode({Layer(agents::AgentPolicyScopeKind::root,
                     "018f0000-0000-7000-8000-000000072202", invalid)},
              Context(&profile), "SB_AGENT_POLICY_OVERRIDE.INVALID_LIFECYCLE",
              "invalid lifecycle");

  RequireCode({Layer(agents::AgentPolicyScopeKind::root,
                     "018f0000-0000-7000-8000-000000072203", root),
               Layer(agents::AgentPolicyScopeKind::tenant,
                     "018f0000-0000-7000-8000-000000072204", tenant,
                     Approval({"max_memory_bytes"}, 4)),
               Layer(agents::AgentPolicyScopeKind::database,
                     "018f0000-0000-7000-8000-000000072205", database,
                     Approval({"allocation_throttle_pages_per_second"}, 5))},
              Context(&profile),
              "SB_AGENT_POLICY_OVERRIDE.SCOPE_ORDER_INVALID",
              "scope order inversion");

  RequireCode({Layer(agents::AgentPolicyScopeKind::root,
                     "018f0000-0000-7000-8000-000000072206", root),
               Layer(agents::AgentPolicyScopeKind::database,
                     "018f0000-0000-7000-8000-000000072207", database,
                     Approval({"allocation_throttle_pages_per_second"}, 6)),
               Layer(agents::AgentPolicyScopeKind::database,
                     "018f0000-0000-7000-8000-000000072208", database,
                     Approval({"allocation_throttle_pages_per_second"}, 7))},
              Context(&profile), "SB_AGENT_POLICY_OVERRIDE.DUPLICATE_SCOPE",
              "duplicate scope");

  RequireCode({Layer(agents::AgentPolicyScopeKind::root,
                     "018f0000-0000-7000-8000-000000072209", root),
               Layer(agents::AgentPolicyScopeKind::database,
                     "018f0000-0000-7000-8000-000000072210", database)},
              Context(&profile),
              "SB_AGENT_POLICY_OVERRIDE.PERMISSION_REQUIRED",
              "override without permission");

  auto missing_approval = Approval({"allocation_throttle_pages_per_second"}, 8);
  missing_approval.approval_uuid.clear();
  RequireCode({Layer(agents::AgentPolicyScopeKind::root,
                     "018f0000-0000-7000-8000-000000072211", root),
               Layer(agents::AgentPolicyScopeKind::database,
                     "018f0000-0000-7000-8000-000000072212", database,
                     missing_approval)},
              Context(&profile), "SB_AGENT_POLICY_OVERRIDE.APPROVAL_REQUIRED",
              "missing approval");

  auto expired = Approval({"allocation_throttle_pages_per_second"}, 9);
  expired.expires_at_microseconds = kNowMicros - 1;
  RequireCode({Layer(agents::AgentPolicyScopeKind::root,
                     "018f0000-0000-7000-8000-000000072213", root),
               Layer(agents::AgentPolicyScopeKind::database,
                     "018f0000-0000-7000-8000-000000072214", database,
                     expired)},
              Context(&profile), "SB_AGENT_POLICY_OVERRIDE.APPROVAL_EXPIRED",
              "expired approval");

  RequireCode({Layer(agents::AgentPolicyScopeKind::root,
                     "018f0000-0000-7000-8000-000000072215", root),
               Layer(agents::AgentPolicyScopeKind::database,
                     "018f0000-0000-7000-8000-000000072216", database,
                     Approval({"max_memory_bytes"}, 10))},
              Context(&profile), "SB_AGENT_POLICY_OVERRIDE.FIELD_NOT_PERMITTED",
              "field not permitted");

  auto bad_typed = database;
  for (auto& field : bad_typed.typed_fields) {
    if (field.name == "allocation_throttle_pages_per_second") {
      field.units = "bad_units";
    }
  }
  agents::FinalizeAgentSignedPolicyLifecycle(&bad_typed);
  RequireCode({Layer(agents::AgentPolicyScopeKind::root,
                     "018f0000-0000-7000-8000-000000072217", root),
               Layer(agents::AgentPolicyScopeKind::database,
                     "018f0000-0000-7000-8000-000000072218", bad_typed,
                     Approval({"allocation_throttle_pages_per_second"}, 11))},
              Context(&profile), "SB_AGENT_POLICY_OVERRIDE.INVALID_LIFECYCLE",
              "typed field mismatch");

  auto local_cluster = root;
  local_cluster.local_cluster_policy_claim = true;
  agents::FinalizeAgentSignedPolicyLifecycle(&local_cluster);
  RequireCode({Layer(agents::AgentPolicyScopeKind::root,
                     "018f0000-0000-7000-8000-000000072219", local_cluster)},
              Context(&profile),
              "SB_AGENT_POLICY_OVERRIDE.LOCAL_CLUSTER_POLICY_FORBIDDEN",
              "local cluster claim");

  auto forbidden_authority = root;
  forbidden_authority.no_authority.memory_authority = true;
  agents::FinalizeAgentSignedPolicyLifecycle(&forbidden_authority);
  RequireCode({Layer(agents::AgentPolicyScopeKind::root,
                     "018f0000-0000-7000-8000-000000072220",
                     forbidden_authority)},
              Context(&profile), "SB_AGENT_POLICY_OVERRIDE.FORBIDDEN_AUTHORITY",
              "forbidden authority");

  auto mismatch_profile = profile;
  mismatch_profile.profile_generation = 77;
  agents::FinalizeAgentSystemProfile(&mismatch_profile);
  RequireCode({Layer(agents::AgentPolicyScopeKind::root,
                     "018f0000-0000-7000-8000-000000072221", root)},
              Context(&mismatch_profile),
              "SB_AGENT_POLICY_OVERRIDE.INVALID_LIFECYCLE",
              "profile generation mismatch");
}

}  // namespace

int main() {
  TestPositiveResolution();
  TestAdvisoryAndDisabledResolution();
  TestNegativeResolution();
  return EXIT_SUCCESS;
}
