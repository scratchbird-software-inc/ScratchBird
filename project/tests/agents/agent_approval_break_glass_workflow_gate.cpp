// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_action_dispatch.hpp"
#include "agent_manual_approval.hpp"
#include "agent_package_provenance_test_support.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace agents = scratchbird::core::agents;

constexpr agents::u64 kNow = 1700000000001000ull;
constexpr agents::u64 kPolicyGeneration = 76;

void Require(bool condition, const std::string& message) {
  if (!condition) { throw std::runtime_error(message); }
}

agents::DurableAgentCatalogImage DurableCatalog() {
  agents::DurableAgentCatalogImage image;
  image.source = agents::AgentCatalogStateSource::durable_catalog_image;
  image.schema_version = 1;
  image.authority.durable_catalog_authority = true;
  image.authority.mga_transaction_evidence = true;
  image.authority.mga_transaction_uuid = "019f0760-0000-7000-8000-000000000001";
  image.authority.transaction_generation = kPolicyGeneration;
  image.authority.evidence_uuid = "019f0760-0000-7000-8000-000000000002";
  image.authority.database_uuid = "019f0760-0000-7000-8000-000000000003";
  image.authority.catalog_storage_uuid = "019f0760-0000-7000-8000-000000000004";
  image.authority.storage_commit_evidence_uuid = image.authority.evidence_uuid;
  image.authority.catalog_generation = 1;
  image.authority.local_transaction_id = 7600;
  image.authority.storage_catalog_record_evidence = true;
  image.authority.transaction_inventory_bound = true;
  image.authority.fsync_or_checkpoint_evidence = true;

  agents::AgentInstanceRecord instance;
  instance.instance_uuid = "019f0760-0000-7000-8000-000000000010";
  instance.agent_type_id = "page_allocation_manager";
  instance.policy_uuid = "019f0760-0000-7000-8000-000000000011";
  instance.scope = "database/filespace/page_family/page_type";
  instance.state = agents::AgentLifecycleState::registered;
  instance.policy_generation = kPolicyGeneration;
  image.instances.push_back(instance);
  const auto refresh =
      agents::RefreshDurableAgentCatalogAuthorityDigest(&image,
                                                        image.authority.evidence_uuid);
  Require(refresh.ok, "fixture durable catalog root digest failed");
  return image;
}

agents::AgentActionAuthorityProvenance OperatorAuthority() {
  agents::AgentActionAuthorityProvenance authority;
  authority.source = agents::AgentActionAuthoritySource::operator_request;
  authority.principal_uuid = "019f0760-0000-7000-8000-000000000020";
  authority.scope_uuid = "019f0760-0000-7000-8000-000000000021";
  authority.provenance_evidence_uuid = "019f0760-0000-7000-8000-000000000022";
  authority.operator_authority = true;
  authority.rights = {"OBS_AGENT_CONTROL"};
  return authority;
}

std::vector<agents::AgentObservedMetricSnapshot> ObservedMetricSnapshots(
    const agents::AgentActionAuthorityProvenance& authority) {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "page allocation descriptor missing");
  std::vector<agents::AgentObservedMetricSnapshot> snapshots;
  int ordinal = 0;
  for (const auto& dependency : descriptor->metric_dependencies) {
    agents::AgentObservedMetricSnapshot snapshot;
    snapshot.metric_family = dependency.metric_family;
    snapshot.namespace_path = dependency.namespace_prefix + ".observed";
    snapshot.generation = 760 + ordinal;
    snapshot.observed_wall_microseconds = kNow - 1000;
    snapshot.scope_uuid = authority.scope_uuid;
    snapshot.digest = "sha256:" + dependency.metric_family;
    snapshot.value_digest = snapshot.digest;
    snapshot.schema_digest = "schema:" + snapshot.metric_family + ":" +
                             std::to_string(snapshot.generation);
    snapshot.source_quality = agents::AgentMetricSourceQuality::trusted;
    snapshot.present = true;
    snapshot.trusted = true;
    snapshot.schema_compatible = true;
    snapshot.attestation_verified = true;
    snapshot.redacted = true;
    snapshot.protected_material_present = false;
    snapshot.trust_provenance = "engine_metric_registry";
    snapshot.provenance_record = snapshot.trust_provenance + ":" +
                                 snapshot.metric_family;
    snapshot.authority_claims = {"metric_evidence"};

    auto source_a = snapshot;
    source_a.source_id = "source-a";
    source_a.source_sequence = snapshot.generation * 2 + 1;
    source_a.previous_source_sequence = source_a.source_sequence - 1;
    source_a.attestation_key_id = "metric-key:" + source_a.source_id;
    source_a.attestation_digest = "attestation:" + source_a.metric_family +
                                  ":" + source_a.source_id;
    source_a.evidence_uuid = "metric-evidence-" + std::to_string(ordinal) +
                             ":source-a";
    source_a.snapshot_id = "metric-snapshot-" + std::to_string(ordinal) +
                           ":source-a";
    snapshots.push_back(std::move(source_a));

    auto source_b = snapshot;
    source_b.source_id = "source-b";
    source_b.source_sequence = snapshot.generation * 2 + 2;
    source_b.previous_source_sequence = source_b.source_sequence - 1;
    source_b.attestation_key_id = "metric-key:" + source_b.source_id;
    source_b.attestation_digest = "attestation:" + source_b.metric_family +
                                  ":" + source_b.source_id;
    source_b.evidence_uuid = "metric-evidence-" + std::to_string(ordinal) +
                             ":source-b";
    source_b.snapshot_id = "metric-snapshot-" + std::to_string(ordinal) +
                           ":source-b";
    snapshots.push_back(std::move(source_b));
    ++ordinal;
  }
  return snapshots;
}

agents::AgentRuntimeContext MetricContext(
    const agents::AgentActionAuthorityProvenance& authority) {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.database_uuid = authority.scope_uuid;
  context.principal_uuid = authority.principal_uuid;
  context.rights = authority.rights;
  context.wall_now_microseconds = kNow;
  return context;
}

std::string ObservedMetricDigestForAction(
    const agents::AgentActionAuthorityProvenance& authority) {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "page allocation descriptor missing");
  agents::AgentMetricSnapshotEvaluationOptions options;
  options.expected_scope_uuid = authority.scope_uuid;
  const auto evaluation = agents::EvaluateAgentObservedMetricSnapshots(
      *descriptor,
      MetricContext(authority),
      ObservedMetricSnapshots(authority),
      options);
  Require(evaluation.accepted,
          "metric digest helper rejected observed snapshots: " +
              evaluation.status.diagnostic_code);
  return evaluation.input_digest;
}

void AddApprovalGrant(agents::AgentActionRequest* action,
                      int index,
                      const std::string& principal_uuid,
                      bool emergency_approval) {
  const std::string prefix = "approval_" + std::to_string(index) + "_";
  (*action).inputs[prefix + "uuid"] =
      "019f0760-0000-7000-8000-0000000001" + std::to_string(index);
  (*action).inputs[prefix + "action_uuid"] = action->action_uuid;
  (*action).inputs[prefix + "agent_type_id"] = action->agent_type_id;
  (*action).inputs[prefix + "operation_id"] = action->operation_id;
  (*action).inputs[prefix + "scope_uuid"] = "019f0760-0000-7000-8000-000000000021";
  (*action).inputs[prefix + "principal_uuid"] = principal_uuid;
  (*action).inputs[prefix + "evidence_uuid"] =
      "019f0760-0000-7000-8000-0000000002" + std::to_string(index);
  (*action).inputs[prefix + "ticket_id"] = "INC-CEIC-076";
  (*action).inputs[prefix + "policy_generation"] =
      std::to_string(kPolicyGeneration);
  (*action).inputs[prefix + "approved_at_microseconds"] =
      std::to_string(kNow - 1000000);
  (*action).inputs[prefix + "expires_at_microseconds"] =
      std::to_string(kNow + 300000000);
  (*action).inputs[prefix + "approved"] = "true";
  (*action).inputs[prefix + "emergency_approval"] =
      emergency_approval ? "true" : "false";
  (*action).inputs[prefix + "authority_claims"] = "approval_evidence";
}

agents::AgentActionRequest Action(std::string uuid,
                                  std::string idempotency_key,
                                  bool break_glass) {
  const auto authority = OperatorAuthority();
  agents::AgentActionRequest action;
  action.action_uuid = std::move(uuid);
  action.agent_type_id = "page_allocation_manager";
  action.instance_uuid = "019f0760-0000-7000-8000-000000000010";
  action.actuator_id = "page_manager";
  action.operation_id = "preallocate_page_family";
  action.idempotency_key = std::move(idempotency_key);
  action.dry_run = false;
  action.manual_approval_present = true;
  action.inputs["evidence_uuid"] = "019f0760-0000-7000-8000-000000000030";
  action.inputs["metric_digest"] = ObservedMetricDigestForAction(authority);
  action.inputs["scope_uuid"] = authority.scope_uuid;
  action.inputs["redaction_class"] = "standard";
  action.inputs["retention_class"] = "audit";
  action.inputs["safety_envelope_version"] = "1";
  action.inputs["safety_evidence_uuid"] = "019f0760-0000-7000-8000-000000000031";
  action.inputs["policy_evidence_uuid"] = "019f0760-0000-7000-8000-000000000032";
  action.inputs["approval_required"] = "true";
  action.inputs["manual_approval_present"] = "true";
  action.inputs["approval_workflow_version"] = "1";
  action.inputs["approval_evidence_uuid"] = "019f0760-0000-7000-8000-000000000033";
  action.inputs["approval_ticket_id"] = "INC-CEIC-076";
  action.inputs["approval_review_deadline_microseconds"] =
      std::to_string(kNow + 3600000000ull);
  action.inputs["approval_notification_required"] = "true";
  action.inputs["approval_notification_triggered"] = "true";
  action.inputs["approval_notification_evidence_uuid"] =
      "019f0760-0000-7000-8000-000000000034";
  action.inputs["approval_notification_channel"] = "ops-page";
  action.inputs["approval_notified_principals"] =
      "019f0760-0000-7000-8000-0000000000a1,"
      "019f0760-0000-7000-8000-0000000000a2";
  action.inputs["rollout_mode"] = "live";
  action.inputs["rollout_state"] = "active";
  action.inputs["rollout_evidence_uuid"] = "019f0760-0000-7000-8000-000000000035";
  action.inputs["failure_threshold"] = "3";
  action.inputs["observed_failures"] = "0";
  action.inputs["retry_limit"] = "2";
  action.inputs["retry_count"] = "0";
  action.inputs["rate_limit_key"] = "page-preallocate";
  action.inputs["rate_limit_per_window"] = "4";
  action.inputs["action_count_in_window"] = "1";
  action.inputs["rate_limit_evidence_uuid"] = "019f0760-0000-7000-8000-000000000036";
  action.inputs["blast_radius_units"] = "1";
  action.inputs["max_blast_radius_units"] = "3";
  action.inputs["blast_radius_evidence_uuid"] = "019f0760-0000-7000-8000-000000000037";
  action.inputs["backup_check_required"] = "true";
  action.inputs["checkpoint_check_required"] = "true";
  action.inputs["storage_check_required"] = "true";
  action.inputs["transaction_check_required"] = "true";
  action.inputs["backup_evidence_uuid"] = "019f0760-0000-7000-8000-000000000038";
  action.inputs["checkpoint_evidence_uuid"] = "019f0760-0000-7000-8000-000000000039";
  action.inputs["storage_check_evidence_uuid"] = "019f0760-0000-7000-8000-00000000003a";
  action.inputs["transaction_evidence_uuid"] = "019f0760-0000-7000-8000-00000000003b";
  action.inputs["compensation_required"] = "true";
  action.inputs["rollback_required"] = "true";
  action.inputs["compensation_plan_evidence_uuid"] = "019f0760-0000-7000-8000-00000000003c";
  action.inputs["rollback_plan_evidence_uuid"] = "019f0760-0000-7000-8000-00000000003d";
  action.inputs["authority_claims"] = "agent_evidence";

  if (break_glass) {
    action.inputs["break_glass_requested"] = "true";
    action.inputs["break_glass_reason"] = "imminent filespace exhaustion";
    action.inputs["break_glass_scope_uuid"] = authority.scope_uuid;
    action.inputs["break_glass_ticket_id"] = "INC-CEIC-076";
    action.inputs["break_glass_activated_by_principal_uuid"] =
        "019f0760-0000-7000-8000-0000000000a1";
    action.inputs["break_glass_activated_at_microseconds"] =
        std::to_string(kNow - 1000000);
    action.inputs["break_glass_expires_at_microseconds"] =
        std::to_string(kNow + 300000000);
    action.inputs["break_glass_max_duration_microseconds"] =
        std::to_string(600000000ull);
    action.inputs["break_glass_review_deadline_microseconds"] =
        std::to_string(kNow + 3600000000ull);
    action.inputs["approval_escalation_required"] = "true";
    action.inputs["approval_escalation_triggered"] = "true";
    action.inputs["approval_escalation_evidence_uuid"] =
        "019f0760-0000-7000-8000-000000000040";
    action.inputs["approval_escalation_chain_id"] = "break-glass-sre-duty";
    action.inputs["approval_escalated_principals"] =
        "019f0760-0000-7000-8000-0000000000b1";
    action.inputs["approval_count"] = "1";
    AddApprovalGrant(&action, 1, "019f0760-0000-7000-8000-0000000000b1", true);
  } else {
    action.inputs["approval_count"] = "2";
    AddApprovalGrant(&action, 1, "019f0760-0000-7000-8000-0000000000a1", false);
    AddApprovalGrant(&action, 2, "019f0760-0000-7000-8000-0000000000a2", false);
  }
  return action;
}

agents::AgentActuatorProviderRegistry Registry() {
  agents::AgentActuatorProviderRegistry registry;
  agents::AgentActuatorProviderDescriptor provider;
  provider.provider_id = "page_manager:preallocate_page_family";
  provider.owning_agent = "page_allocation_manager";
  provider.actuator_id = "page_manager";
  provider.operation_id = "preallocate_page_family";
  provider.authority_domain = agents::AgentActuatorAuthorityDomain::page;
  provider.supports_dry_run = true;
  provider.live_route_available = true;
  provider.real_subsystem_handler = true;
  provider.subsystem_handler_id = "storage.page.preallocate_page_family";
  provider.handler_provenance = "ceic076_real_subsystem_handler";
  provider.handler_evidence_uuid = "019f0760-0000-7000-8000-000000000050";
  provider.idempotent = true;
  provider.supports_retry = true;
  provider.supports_rollback_compensation = true;
  provider.requires_outcome_verification = true;
  provider.required_evidence_fields = {"evidence_uuid", "metric_digest"};
  provider.package_provenance =
      agent_test_support::PageProviderPackageProvenance(
          "019f0761-0000-7000-8000-00000000008");
  const auto status = registry.Register(
      provider,
      [](const agents::AgentActuatorProviderRequest& request) {
        agents::AgentActuatorProviderResult result;
        result.dispatched = true;
        result.mutation_attempted = true;
        result.outcome_verified = true;
        result.verification_evidence_uuid =
            "019f0760-0000-7000-8000-000000000051";
        result.status = {true, "SB_AGENT_ACTION.OUTCOME_VERIFIED",
                         request.action.action_uuid};
        return result;
      });
  Require(status.ok, "provider registration failed: " + status.diagnostic_code);
  return registry;
}

agents::AgentActionDispatchResult Dispatch(
    agents::AgentActionRequest action,
    const agents::AgentActuatorProviderRegistry& registry) {
  auto catalog = DurableCatalog();
  agents::AgentActionDispatchRequest request;
  request.catalog = &catalog;
  request.registry = &registry;
  request.authority = OperatorAuthority();
  request.action = std::move(action);
  request.production_live_path = true;
  request.metric_context = MetricContext(request.authority);
  request.metric_snapshot_options.expected_scope_uuid =
      request.authority.scope_uuid;
  request.observed_metric_snapshots = ObservedMetricSnapshots(request.authority);
  request.provider_execution_context.engine_owned_registry = true;
  request.provider_execution_context.durable_catalog_store_context = true;
  request.provider_execution_context.engine_request_context_present = true;
  request.provider_execution_context.fsync_or_checkpoint_evidence = true;
  request.provider_execution_context.request_id =
      "agent-approval-break-glass-workflow-gate";
  request.provider_execution_context.database_uuid = request.authority.scope_uuid;
  request.provider_execution_context.transaction_uuid =
      "019f0760-0000-7000-8000-000000000090";
  request.provider_execution_context.local_transaction_id = 76;
  request.provider_execution_context.registry_provenance =
      "engine_internal_api_registered_provider_registry";
  request.provider_execution_context.registry_evidence_uuid =
      "019f0760-0000-7000-8000-000000000091";
  request.subsystem_reported_success = true;
  request.intended_state_observed = true;
  return agents::DispatchAgentAction(request);
}

void RequireDispatchOk(const agents::AgentActionDispatchResult& result,
                       const std::string& label,
                       agents::AgentManualApprovalMode mode) {
  Require(result.status.ok,
          label + " failed: " + result.status.diagnostic_code + ":" +
              result.status.detail);
  Require(result.safety_envelope_validated,
          label + " did not validate action safety envelope");
  Require(result.approval_workflow_validated,
          label + " did not validate approval workflow");
  Require(result.approval_workflow.mode == mode,
          label + " approved with wrong workflow mode");
  Require(result.approval_workflow.notification_evidence_present,
          label + " missing notification evidence");
  Require(result.provider_dispatched,
          label + " did not dispatch after approval");
}

void RequireDispatchRefusal(const agents::AgentActionDispatchResult& result,
                            const std::string& diagnostic_code) {
  Require(!result.status.ok, "dispatch unexpectedly accepted");
  Require(result.status.diagnostic_code == diagnostic_code,
          "diagnostic mismatch: " + result.status.diagnostic_code);
  Require(!result.provider_dispatched,
          "provider executed after approval refusal");
}

void TestPositiveWorkflows() {
  const auto registry = Registry();
  RequireDispatchOk(Dispatch(Action("019f0760-0000-7000-8000-000000000101",
                                    "idem-normal-approval", false),
                             registry),
                    "normal approval",
                    agents::AgentManualApprovalMode::normal_two_person);
  const auto break_glass = Dispatch(
      Action("019f0760-0000-7000-8000-000000000102",
             "idem-break-glass", true),
      registry);
  RequireDispatchOk(break_glass,
                    "break-glass approval",
                    agents::AgentManualApprovalMode::emergency_break_glass);
  Require(break_glass.approval_workflow.break_glass_accepted,
          "break-glass workflow did not record emergency acceptance");
  Require(break_glass.approval_workflow.escalation_evidence_present,
          "break-glass workflow did not record escalation evidence");
}

void TestNegativeDispatchWorkflows() {
  const auto registry = Registry();

  auto action = Action("019f0760-0000-7000-8000-000000000201",
                       "idem-same-principal", false);
  action.inputs["approval_1_principal_uuid"] = OperatorAuthority().principal_uuid;
  RequireDispatchRefusal(
      Dispatch(std::move(action), registry),
      "SB_AGENT_APPROVAL_WORKFLOW.SAME_PRINCIPAL_REFUSED");

  action = Action("019f0760-0000-7000-8000-000000000202",
                  "idem-expired", false);
  action.inputs["approval_1_expires_at_microseconds"] = std::to_string(kNow - 1);
  RequireDispatchRefusal(
      Dispatch(std::move(action), registry),
      "SB_AGENT_APPROVAL_WORKFLOW.APPROVAL_EXPIRED");

  action = Action("019f0760-0000-7000-8000-000000000203",
                  "idem-revoked", false);
  action.inputs["approval_1_revoked"] = "true";
  action.inputs["approval_1_revocation_evidence_uuid"] =
      "019f0760-0000-7000-8000-0000000000ff";
  RequireDispatchRefusal(
      Dispatch(std::move(action), registry),
      "SB_AGENT_APPROVAL_WORKFLOW.APPROVAL_REVOKED");

  action = Action("019f0760-0000-7000-8000-000000000204",
                  "idem-ticket", false);
  action.inputs.erase("approval_ticket_id");
  RequireDispatchRefusal(Dispatch(std::move(action), registry),
                         "SB_AGENT_APPROVAL_WORKFLOW.TICKET_REQUIRED");

  action = Action("019f0760-0000-7000-8000-000000000205",
                  "idem-notification", false);
  action.inputs.erase("approval_notification_evidence_uuid");
  RequireDispatchRefusal(
      Dispatch(std::move(action), registry),
      "SB_AGENT_APPROVAL_WORKFLOW.NOTIFICATION_REQUIRED");

  action = Action("019f0760-0000-7000-8000-000000000206",
                  "idem-escalation", true);
  action.inputs.erase("approval_escalation_evidence_uuid");
  RequireDispatchRefusal(Dispatch(std::move(action), registry),
                         "SB_AGENT_APPROVAL_WORKFLOW.ESCALATION_REQUIRED");

  action = Action("019f0760-0000-7000-8000-000000000207",
                  "idem-review", false);
  action.inputs.erase("approval_review_deadline_microseconds");
  RequireDispatchRefusal(
      Dispatch(std::move(action), registry),
      "SB_AGENT_APPROVAL_WORKFLOW.REVIEW_DEADLINE_REQUIRED");

  action = Action("019f0760-0000-7000-8000-000000000208",
                  "idem-duration", true);
  action.inputs["break_glass_expires_at_microseconds"] =
      std::to_string(kNow + 1200000000ull);
  RequireDispatchRefusal(
      Dispatch(std::move(action), registry),
      "SB_AGENT_APPROVAL_WORKFLOW.BREAK_GLASS_DURATION_EXCEEDED");

  action = Action("019f0760-0000-7000-8000-000000000209",
                  "idem-scope", false);
  action.inputs["approval_1_scope_uuid"] =
      "019f0760-0000-7000-8000-000000000099";
  RequireDispatchRefusal(Dispatch(std::move(action), registry),
                         "SB_AGENT_APPROVAL_WORKFLOW.SCOPE_MISMATCH");

  action = Action("019f0760-0000-7000-8000-000000000210",
                  "idem-policy-generation", false);
  action.inputs["approval_1_policy_generation"] = "75";
  RequireDispatchRefusal(
      Dispatch(std::move(action), registry),
      "SB_AGENT_APPROVAL_WORKFLOW.POLICY_GENERATION_MISMATCH");

  action = Action("019f0760-0000-7000-8000-000000000211",
                  "idem-forbidden-authority", false);
  action.inputs["approval_authority_claims"] = "transaction_finality";
  RequireDispatchRefusal(
      Dispatch(std::move(action), registry),
      "SB_AGENT_APPROVAL_WORKFLOW.FORBIDDEN_AUTHORITY_CLAIM");

  action = Action("019f0760-0000-7000-8000-000000000212",
                  "idem-break-glass-activator", true);
  action.inputs["approval_1_principal_uuid"] =
      action.inputs["break_glass_activated_by_principal_uuid"];
  RequireDispatchRefusal(
      Dispatch(std::move(action), registry),
      "SB_AGENT_APPROVAL_WORKFLOW.BREAK_GLASS_ACTIVATOR_APPROVER_SEPARATION_REQUIRED");
}

void TestLocalClusterApprovalRefusal() {
  auto action = Action("019f0760-0000-7000-8000-000000000301",
                       "idem-local-cluster", false);
  action.agent_type_id = "cluster_autoscale_manager";
  action.inputs["cluster_route_requested"] = "true";
  const auto authority = OperatorAuthority();
  const auto workflow = agents::AgentManualApprovalWorkflowFromActionInputs(
      action,
      authority.principal_uuid,
      authority.scope_uuid,
      kPolicyGeneration,
      kNow,
      true,
      true,
      false);
  const auto evaluation = agents::ValidateAgentManualApprovalWorkflow(workflow);
  Require(!evaluation.status.ok, "local cluster approval unexpectedly accepted");
  Require(evaluation.status.diagnostic_code ==
              "SB_AGENT_APPROVAL_WORKFLOW.CLUSTER_EXTERNAL_PROVIDER_REQUIRED",
          "cluster diagnostic mismatch: " + evaluation.status.diagnostic_code);
}

int main() {
  try {
    TestPositiveWorkflows();
    TestNegativeDispatchWorkflows();
    TestLocalClusterApprovalRefusal();
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
