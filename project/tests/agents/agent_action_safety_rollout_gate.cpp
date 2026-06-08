// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_action_dispatch.hpp"
#include "agent_package_provenance_test_support.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace agents = scratchbird::core::agents;

void Require(bool condition, const std::string& message) {
  if (!condition) { throw std::runtime_error(message); }
}

agents::DurableAgentCatalogImage DurableCatalog() {
  agents::DurableAgentCatalogImage image;
  image.source = agents::AgentCatalogStateSource::durable_catalog_image;
  image.schema_version = 1;
  image.authority.durable_catalog_authority = true;
  image.authority.mga_transaction_evidence = true;
  image.authority.mga_transaction_uuid = "019f0750-0000-7000-8000-000000000001";
  image.authority.transaction_generation = 75;
  image.authority.evidence_uuid = "019f0750-0000-7000-8000-000000000002";
  image.authority.database_uuid = "019f0750-0000-7000-8000-000000000003";
  image.authority.catalog_storage_uuid = "019f0750-0000-7000-8000-000000000004";
  image.authority.storage_commit_evidence_uuid = image.authority.evidence_uuid;
  image.authority.catalog_generation = 1;
  image.authority.local_transaction_id = 7500;
  image.authority.storage_catalog_record_evidence = true;
  image.authority.transaction_inventory_bound = true;
  image.authority.fsync_or_checkpoint_evidence = true;

  agents::AgentInstanceRecord instance;
  instance.instance_uuid = "019f0750-0000-7000-8000-000000000010";
  instance.agent_type_id = "page_allocation_manager";
  instance.policy_uuid = "019f0750-0000-7000-8000-000000000011";
  instance.scope = "database/filespace/page_family/page_type";
  instance.state = agents::AgentLifecycleState::registered;
  instance.policy_generation = 75;
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
  authority.principal_uuid = "019f0750-0000-7000-8000-000000000020";
  authority.scope_uuid = "019f0750-0000-7000-8000-000000000021";
  authority.provenance_evidence_uuid = "019f0750-0000-7000-8000-000000000022";
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
    snapshot.generation = 750 + ordinal;
    snapshot.observed_wall_microseconds = 1700000000000000ull;
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
  context.wall_now_microseconds = 1700000000001000ull;
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

agents::AgentActionRequest Action(std::string uuid,
                                  std::string idempotency_key,
                                  std::string rollout_mode,
                                  bool dry_run) {
  const auto authority = OperatorAuthority();
  agents::AgentActionRequest action;
  action.action_uuid = std::move(uuid);
  action.agent_type_id = "page_allocation_manager";
  action.instance_uuid = "019f0750-0000-7000-8000-000000000010";
  action.actuator_id = "page_manager";
  action.operation_id = "preallocate_page_family";
  action.idempotency_key = std::move(idempotency_key);
  action.dry_run = dry_run;
  action.inputs["evidence_uuid"] = "019f0750-0000-7000-8000-000000000030";
  action.inputs["metric_digest"] = ObservedMetricDigestForAction(authority);
  action.inputs["scope_uuid"] = "019f0750-0000-7000-8000-000000000021";
  action.inputs["redaction_class"] = "standard";
  action.inputs["retention_class"] = "audit";
  action.inputs["safety_envelope_version"] = "1";
  action.inputs["safety_evidence_uuid"] = "019f0750-0000-7000-8000-000000000031";
  action.inputs["policy_evidence_uuid"] = "019f0750-0000-7000-8000-000000000032";
  action.inputs["rollout_mode"] = std::move(rollout_mode);
  action.inputs["rollout_state"] = "active";
  action.inputs["rollout_evidence_uuid"] = "019f0750-0000-7000-8000-000000000033";
  action.inputs["failure_threshold"] = "3";
  action.inputs["observed_failures"] = "0";
  action.inputs["retry_limit"] = "2";
  action.inputs["retry_count"] = "0";
  action.inputs["rate_limit_key"] = "page-preallocate";
  action.inputs["rate_limit_per_window"] = "4";
  action.inputs["action_count_in_window"] = "1";
  action.inputs["rate_limit_evidence_uuid"] = "019f0750-0000-7000-8000-000000000034";
  action.inputs["blast_radius_units"] = "1";
  action.inputs["max_blast_radius_units"] = "3";
  action.inputs["blast_radius_evidence_uuid"] = "019f0750-0000-7000-8000-000000000035";
  action.inputs["backup_check_required"] = dry_run ? "false" : "true";
  action.inputs["checkpoint_check_required"] = dry_run ? "false" : "true";
  action.inputs["storage_check_required"] = dry_run ? "false" : "true";
  action.inputs["transaction_check_required"] = dry_run ? "false" : "true";
  action.inputs["backup_evidence_uuid"] = "019f0750-0000-7000-8000-000000000036";
  action.inputs["checkpoint_evidence_uuid"] = "019f0750-0000-7000-8000-000000000037";
  action.inputs["storage_check_evidence_uuid"] = "019f0750-0000-7000-8000-000000000038";
  action.inputs["transaction_evidence_uuid"] = "019f0750-0000-7000-8000-000000000039";
  action.inputs["compensation_required"] = dry_run ? "false" : "true";
  action.inputs["rollback_required"] = dry_run ? "false" : "true";
  action.inputs["compensation_plan_evidence_uuid"] = "019f0750-0000-7000-8000-00000000003a";
  action.inputs["rollback_plan_evidence_uuid"] = "019f0750-0000-7000-8000-00000000003b";
  action.inputs["authority_claims"] = "agent_evidence";
  if (action.inputs["rollout_mode"] == "canary") {
    action.inputs["canary_percent"] = "10";
    action.inputs["canary_max_subjects"] = "5";
    action.inputs["canary_current_subjects"] = "2";
  }
  if (action.inputs["rollout_mode"] == "phased") {
    action.inputs["phased_percent"] = "25";
    action.inputs["phased_target_percent"] = "75";
  }
  return action;
}

agents::AgentActuatorProviderRegistry Registry(bool live) {
  agents::AgentActuatorProviderRegistry registry;
  agents::AgentActuatorProviderDescriptor provider;
  provider.provider_id = "page_manager:preallocate_page_family";
  provider.owning_agent = "page_allocation_manager";
  provider.actuator_id = "page_manager";
  provider.operation_id = "preallocate_page_family";
  provider.authority_domain = agents::AgentActuatorAuthorityDomain::page;
  provider.supports_dry_run = true;
  provider.live_route_available = live;
  provider.real_subsystem_handler = live;
  provider.subsystem_handler_id = live ? "storage.page.preallocate_page_family" : "";
  provider.handler_provenance = live ? "safety_gate_real_subsystem_handler" : "";
  provider.handler_evidence_uuid = live ? "019f0750-0000-7000-8000-000000000040" : "";
  provider.idempotent = true;
  provider.supports_retry = live;
  provider.supports_rollback_compensation = live;
  provider.requires_outcome_verification = true;
  provider.required_evidence_fields = {"evidence_uuid", "metric_digest"};
  if (live) {
    provider.package_provenance =
        agent_test_support::PageProviderPackageProvenance(
            "019f0751-0000-7000-8000-00000000008");
  }
  const auto status = registry.Register(
      provider,
      [](const agents::AgentActuatorProviderRequest& request) {
        agents::AgentActuatorProviderResult result;
        result.dry_run = request.dry_run;
        result.dispatched = !request.dry_run;
        result.mutation_attempted = !request.dry_run;
        result.outcome_verified = true;
        result.verification_evidence_uuid =
            "019f0750-0000-7000-8000-000000000041";
        result.status = {true,
                         request.dry_run
                             ? "SB_AGENT_ACTION.DRY_RUN_ONLY"
                             : "SB_AGENT_ACTION.OUTCOME_VERIFIED",
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
  request.provider_execution_context.request_id = "agent-action-safety-rollout-gate";
  request.provider_execution_context.database_uuid = request.authority.scope_uuid;
  request.provider_execution_context.transaction_uuid =
      "019f0750-0000-7000-8000-000000000090";
  request.provider_execution_context.local_transaction_id = 75;
  request.provider_execution_context.registry_provenance =
      "engine_internal_api_registered_provider_registry";
  request.provider_execution_context.registry_evidence_uuid =
      "019f0750-0000-7000-8000-000000000091";
  request.subsystem_reported_success = true;
  request.intended_state_observed = true;
  return agents::DispatchAgentAction(request);
}

void RequireDispatchOk(const agents::AgentActionDispatchResult& result,
                       const std::string& label) {
  Require(result.status.ok,
          label + " failed: " + result.status.diagnostic_code + ":" +
              result.status.detail);
  Require(result.safety_envelope_validated,
          label + " did not validate safety envelope");
}

void RequireDispatchRefusal(const agents::AgentActionDispatchResult& result,
                            const std::string& diagnostic_code) {
  Require(!result.status.ok, "dispatch unexpectedly accepted");
  Require(result.status.diagnostic_code == diagnostic_code,
          "diagnostic mismatch: " + result.status.diagnostic_code);
  Require(!result.provider_dispatched,
          "provider executed after safety-envelope refusal");
}

void TestPositiveRolloutModes() {
  const auto dry_registry = Registry(false);
  RequireDispatchOk(Dispatch(Action("019f0750-0000-7000-8000-000000000101",
                                    "idem-dry", "dry_run", true),
                             dry_registry),
                    "dry-run rollout");
  RequireDispatchOk(Dispatch(Action("019f0750-0000-7000-8000-000000000102",
                                    "idem-shadow", "shadow", true),
                             dry_registry),
                    "shadow rollout");
  RequireDispatchOk(Dispatch(Action("019f0750-0000-7000-8000-000000000103",
                                    "idem-observe", "observe", true),
                             dry_registry),
                    "observe rollout");

  const auto live_registry = Registry(true);
  RequireDispatchOk(Dispatch(Action("019f0750-0000-7000-8000-000000000104",
                                    "idem-canary", "canary", false),
                             live_registry),
                    "canary rollout");
  RequireDispatchOk(Dispatch(Action("019f0750-0000-7000-8000-000000000105",
                                    "idem-phased", "phased", false),
                             live_registry),
                    "phased rollout");
  RequireDispatchOk(Dispatch(Action("019f0750-0000-7000-8000-000000000106",
                                    "idem-live", "live", false),
                             live_registry),
                    "live rollout");
}

void TestNegativeSafetyEnvelopeCases() {
  const auto live_registry = Registry(true);

  auto action = Action("019f0750-0000-7000-8000-000000000201",
                       "idem-missing-approval", "live", false);
  action.inputs["approval_required"] = "true";
  RequireDispatchRefusal(
      Dispatch(std::move(action), live_registry),
      "SB_AGENT_ACTION_SAFETY.APPROVAL_REQUIRED");

  action = Action("019f0750-0000-7000-8000-000000000202",
                  "idem-rate", "live", false);
  action.inputs["action_count_in_window"] = "4";
  RequireDispatchRefusal(Dispatch(std::move(action), live_registry),
                         "SB_AGENT_ACTION_SAFETY.RATE_LIMIT_EXCEEDED");

  action = Action("019f0750-0000-7000-8000-000000000203",
                  "idem-blast", "live", false);
  action.inputs["blast_radius_units"] = "4";
  RequireDispatchRefusal(Dispatch(std::move(action), live_registry),
                         "SB_AGENT_ACTION_SAFETY.BLAST_RADIUS_EXCEEDED");

  action = Action("019f0750-0000-7000-8000-000000000204",
                  "idem-backup", "live", false);
  action.inputs.erase("backup_evidence_uuid");
  RequireDispatchRefusal(Dispatch(std::move(action), live_registry),
                         "SB_AGENT_ACTION_SAFETY.BACKUP_CHECK_REQUIRED");

  action = Action("019f0750-0000-7000-8000-000000000205",
                  "idem-storage", "live", false);
  action.inputs.erase("storage_check_evidence_uuid");
  RequireDispatchRefusal(Dispatch(std::move(action), live_registry),
                         "SB_AGENT_ACTION_SAFETY.STORAGE_CHECK_REQUIRED");

  action = Action("019f0750-0000-7000-8000-000000000214",
                  "idem-checkpoint", "live", false);
  action.inputs.erase("checkpoint_evidence_uuid");
  RequireDispatchRefusal(Dispatch(std::move(action), live_registry),
                         "SB_AGENT_ACTION_SAFETY.CHECKPOINT_CHECK_REQUIRED");

  action = Action("019f0750-0000-7000-8000-000000000206",
                  "idem-tx", "live", false);
  action.inputs.erase("transaction_evidence_uuid");
  RequireDispatchRefusal(Dispatch(std::move(action), live_registry),
                         "SB_AGENT_ACTION_SAFETY.TRANSACTION_CHECK_REQUIRED");

  action = Action("019f0750-0000-7000-8000-000000000207",
                  "idem-comp", "live", false);
  action.inputs.erase("compensation_plan_evidence_uuid");
  RequireDispatchRefusal(Dispatch(std::move(action), live_registry),
                         "SB_AGENT_ACTION_SAFETY.COMPENSATION_REQUIRED");

  action = Action("019f0750-0000-7000-8000-000000000208",
                  "idem-rollback", "live", false);
  action.inputs.erase("rollback_plan_evidence_uuid");
  RequireDispatchRefusal(Dispatch(std::move(action), live_registry),
                         "SB_AGENT_ACTION_SAFETY.ROLLBACK_REQUIRED");

  action = Action("019f0750-0000-7000-8000-000000000209",
                  "idem-cluster", "live", false);
  action.inputs["cluster_route_requested"] = "true";
  RequireDispatchRefusal(
      Dispatch(std::move(action), live_registry),
      "SB_AGENT_ACTION_SAFETY.CLUSTER_EXTERNAL_PROVIDER_REQUIRED");

  action = Action("019f0750-0000-7000-8000-000000000210",
                  "idem-authority", "live", false);
  action.inputs["authority_claims"] = "transaction_finality";
  RequireDispatchRefusal(
      Dispatch(std::move(action), live_registry),
      "SB_AGENT_ACTION_SAFETY.FORBIDDEN_AUTHORITY_CLAIM");

  action = Action("019f0750-0000-7000-8000-000000000211",
                  "idem-disabled", "live", false);
  action.inputs["rollout_mode"] = "disabled";
  RequireDispatchRefusal(Dispatch(std::move(action), live_registry),
                         "SB_AGENT_ROLLOUT.DISABLED");

  action = Action("019f0750-0000-7000-8000-000000000212",
                  "idem-canary-fail", "canary", false);
  action.inputs["observed_failures"] = "3";
  RequireDispatchRefusal(
      Dispatch(std::move(action), live_registry),
      "SB_AGENT_ROLLOUT.FAILURE_THRESHOLD_EXCEEDED");

  action = Action("019f0750-0000-7000-8000-000000000215",
                  "idem-quarantine", "live", false);
  action.inputs["quarantine_on_failure"] = "false";
  RequireDispatchRefusal(Dispatch(std::move(action), live_registry),
                         "SB_AGENT_ROLLOUT.QUARANTINE_REQUIRED");

  action = Action("019f0750-0000-7000-8000-000000000213",
                  "idem-evidence", "live", false);
  action.inputs.erase("safety_evidence_uuid");
  RequireDispatchRefusal(Dispatch(std::move(action), live_registry),
                         "SB_AGENT_ACTION_SAFETY.EVIDENCE_REQUIRED");
}

int main() {
  try {
    TestPositiveRolloutModes();
    TestNegativeSafetyEnvelopeCases();
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
