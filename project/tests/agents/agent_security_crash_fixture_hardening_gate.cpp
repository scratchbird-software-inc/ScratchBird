// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_action_dispatch.hpp"
#include "agent_commercial_evidence.hpp"
#include "agent_package_provenance_test_support.hpp"
#include "agent_production_fixture_separation.hpp"
#include "agent_runtime_service.hpp"

// SEARCH_KEY: ARHC_SECURITY_NEGATIVE_BYPASS_TESTS
// SEARCH_KEY: ARHC_CRASH_RESTART_CONCURRENCY_RACE_SUITES
// SEARCH_KEY: AEIC_AGENT_SECURITY_NEGATIVE_REDACTION_TESTS

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace agents = scratchbird::core::agents;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) { throw std::runtime_error(message); }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 const std::string& expected) {
  return std::find(evidence.begin(), evidence.end(), expected) != evidence.end();
}

agents::AgentRuntimeContext Context(std::vector<std::string> rights = {},
                                    std::vector<std::string> groups = {},
                                    bool security_context_present = true,
                                    bool cluster_authority_available = false) {
  agents::AgentRuntimeContext context;
  context.security_context_present = security_context_present;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.database_uuid = "019f0800-0000-7000-8000-000000000001";
  context.principal_uuid = "019f0800-0000-7000-8000-000000000002";
  context.rights = std::move(rights);
  context.groups = std::move(groups);
  context.cluster_authority_available = cluster_authority_available;
  context.wall_now_microseconds = 1700000000000080ull;
  context.monotonic_now_microseconds = 8000;
  return context;
}

agents::DurableAgentCatalogImage DurableCatalog(agents::u64 policy_generation = 17) {
  agents::DurableAgentCatalogImage image;
  image.source = agents::AgentCatalogStateSource::durable_catalog_image;
  image.schema_version = 1;
  image.authority.durable_catalog_authority = true;
  image.authority.mga_transaction_evidence = true;
  image.authority.mga_transaction_uuid =
      "019f0800-0000-7000-8000-000000000010";
  image.authority.transaction_generation = 80;
  image.authority.evidence_uuid = "019f0800-0000-7000-8000-000000000011";
  image.authority.database_uuid = "019f0800-0000-7000-8000-000000000012";
  image.authority.catalog_storage_uuid = "019f0800-0000-7000-8000-000000000013";
  image.authority.storage_commit_evidence_uuid = image.authority.evidence_uuid;
  image.authority.catalog_generation = 1;
  image.authority.local_transaction_id = 8080;
  image.authority.storage_catalog_record_evidence = true;
  image.authority.transaction_inventory_bound = true;
  image.authority.fsync_or_checkpoint_evidence = true;

  agents::AgentInstanceRecord instance;
  instance.instance_uuid = "019f0800-0000-7000-8000-000000000020";
  instance.agent_type_id = "page_allocation_manager";
  instance.policy_uuid = "019f0800-0000-7000-8000-000000000021";
  instance.scope = "database/filespace/page_family";
  instance.state = agents::AgentLifecycleState::registered;
  instance.policy_generation = policy_generation;
  instance.instance_generation = 80;
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
  authority.principal_uuid = "019f0800-0000-7000-8000-000000000030";
  authority.scope_uuid = "019f0800-0000-7000-8000-000000000031";
  authority.provenance_evidence_uuid =
      "019f0800-0000-7000-8000-000000000032";
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
    snapshot.generation = 800 + ordinal;
    snapshot.observed_wall_microseconds = 1700000000000080ull;
    snapshot.scope_uuid = authority.scope_uuid;
    snapshot.digest = "sha256:" + dependency.metric_family;
    snapshot.source_quality = agents::AgentMetricSourceQuality::trusted;
    snapshot.present = true;
    snapshot.trusted = true;
    snapshot.schema_compatible = true;
    snapshot.trust_provenance = "engine_metric_registry";
    snapshot.evidence_uuid = "metric-evidence-arhc080-" +
                             std::to_string(ordinal);
    snapshot.snapshot_id = "metric-snapshot-arhc080-" +
                           std::to_string(ordinal);
    snapshot.value_digest = snapshot.digest;
    snapshot.schema_digest = "schema:" + snapshot.metric_family + ":" +
                             std::to_string(snapshot.generation);
    snapshot.attestation_verified = true;
    snapshot.redacted = true;
    snapshot.protected_material_present = false;
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
    source_a.evidence_uuid += ":source-a";
    source_a.snapshot_id += ":source-a";
    snapshots.push_back(std::move(source_a));

    auto source_b = snapshot;
    source_b.source_id = "source-b";
    source_b.source_sequence = snapshot.generation * 2 + 2;
    source_b.previous_source_sequence = source_b.source_sequence - 1;
    source_b.attestation_key_id = "metric-key:" + source_b.source_id;
    source_b.attestation_digest = "attestation:" + source_b.metric_family +
                                  ":" + source_b.source_id;
    source_b.evidence_uuid += ":source-b";
    source_b.snapshot_id += ":source-b";
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
  context.wall_now_microseconds = 1700000000001080ull;
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

agents::AgentActionRequest DispatchAction(std::string action_uuid,
                                          std::string idempotency_key) {
  const auto authority = OperatorAuthority();
  agents::AgentActionRequest action;
  action.action_uuid = std::move(action_uuid);
  action.agent_type_id = "page_allocation_manager";
  action.instance_uuid = "019f0800-0000-7000-8000-000000000020";
  action.actuator_id = "page_manager";
  action.operation_id = "preallocate_page_family";
  action.idempotency_key = std::move(idempotency_key);
  action.dry_run = false;
  action.inputs["evidence_uuid"] = "019f0800-0000-7000-8000-000000000040";
  action.inputs["metric_digest"] = ObservedMetricDigestForAction(authority);
  action.inputs["scope_uuid"] = "019f0800-0000-7000-8000-000000000041";
  action.inputs["safety_envelope_version"] = "1";
  action.inputs["safety_evidence_uuid"] = "019f0800-0000-7000-8000-000000000044";
  action.inputs["policy_evidence_uuid"] = "019f0800-0000-7000-8000-000000000045";
  action.inputs["rollout_mode"] = "live";
  action.inputs["rollout_state"] = "active";
  action.inputs["rollout_evidence_uuid"] = "019f0800-0000-7000-8000-000000000046";
  action.inputs["failure_threshold"] = "3";
  action.inputs["observed_failures"] = "0";
  action.inputs["retry_limit"] = "2";
  action.inputs["retry_count"] = "0";
  action.inputs["rate_limit_key"] = "page-preallocate";
  action.inputs["rate_limit_per_window"] = "4";
  action.inputs["action_count_in_window"] = "1";
  action.inputs["rate_limit_evidence_uuid"] = "019f0800-0000-7000-8000-000000000047";
  action.inputs["blast_radius_units"] = "1";
  action.inputs["max_blast_radius_units"] = "3";
  action.inputs["blast_radius_evidence_uuid"] = "019f0800-0000-7000-8000-000000000048";
  action.inputs["backup_check_required"] = "true";
  action.inputs["checkpoint_check_required"] = "true";
  action.inputs["storage_check_required"] = "true";
  action.inputs["transaction_check_required"] = "true";
  action.inputs["backup_evidence_uuid"] = "019f0800-0000-7000-8000-000000000049";
  action.inputs["checkpoint_evidence_uuid"] = "019f0800-0000-7000-8000-00000000004a";
  action.inputs["storage_check_evidence_uuid"] = "019f0800-0000-7000-8000-00000000004b";
  action.inputs["transaction_evidence_uuid"] = "019f0800-0000-7000-8000-00000000004c";
  action.inputs["compensation_required"] = "true";
  action.inputs["rollback_required"] = "true";
  action.inputs["compensation_plan_evidence_uuid"] = "019f0800-0000-7000-8000-00000000004d";
  action.inputs["rollback_plan_evidence_uuid"] = "019f0800-0000-7000-8000-00000000004e";
  action.inputs["authority_claims"] = "agent_evidence";
  return action;
}

agents::AgentActuatorProviderRegistry Registry(int* dispatch_count = nullptr) {
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
  provider.handler_provenance = "security_gate_real_subsystem_handler";
  provider.handler_evidence_uuid = "019f0800-0000-7000-8000-000000000043";
  provider.idempotent = true;
  provider.supports_retry = true;
  provider.supports_rollback_compensation = true;
  provider.requires_outcome_verification = true;
  provider.required_evidence_fields = {"evidence_uuid", "metric_digest"};
  provider.package_provenance =
      agent_test_support::PageProviderPackageProvenance(
          "019f0802-0000-7000-8000-00000000008");
  const auto status = registry.Register(
      provider,
      [dispatch_count](const agents::AgentActuatorProviderRequest& request) {
        if (dispatch_count != nullptr && !request.dry_run) { ++(*dispatch_count); }
        agents::AgentActuatorProviderResult result;
        result.dispatched = !request.dry_run;
        result.mutation_attempted = !request.dry_run;
        result.dry_run = request.dry_run;
        result.outcome_verified =
            request.subsystem_reported_success && request.intended_state_observed;
        result.verification_evidence_uuid =
            "019f0800-0000-7000-8000-000000000042";
        result.status = result.outcome_verified
                            ? agents::AgentRuntimeStatus{
                                  true, "SB_AGENT_ACTION.OUTCOME_VERIFIED",
                                  request.action.action_uuid}
                            : agents::AgentError(
                                  "SB_AGENT_ACTION.OUTCOME_UNVERIFIED_REPLAY_REQUIRED",
                                  request.action.action_uuid);
        return result;
      });
  Require(status.ok, "provider registration failed: " + status.diagnostic_code);
  return registry;
}

agents::AgentActionDispatchResult Dispatch(
    agents::DurableAgentCatalogImage* catalog,
    const agents::AgentActuatorProviderRegistry* registry,
    agents::AgentActionAuthorityProvenance authority,
    agents::AgentActionRequest action) {
  agents::AgentActionDispatchRequest request;
  request.catalog = catalog;
  request.registry = registry;
  request.authority = std::move(authority);
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
  request.provider_execution_context.request_id = "agent-security-hardening-gate";
  request.provider_execution_context.database_uuid = request.authority.scope_uuid;
  request.provider_execution_context.transaction_uuid =
      "019f0800-0000-7000-8000-000000000090";
  request.provider_execution_context.local_transaction_id = 80;
  request.provider_execution_context.registry_provenance =
      "engine_internal_api_registered_provider_registry";
  request.provider_execution_context.registry_evidence_uuid =
      "019f0800-0000-7000-8000-000000000091";
  request.subsystem_reported_success = true;
  request.intended_state_observed = true;
  return agents::DispatchAgentAction(request);
}

void TestArhc080SecurityNegativeAndBypassCoverage() {
  const auto missing_context = agents::EvaluateAgentCommandGrant(
      Context({"OBS_AGENT_CONTROL"}, {}, false),
      agents::AgentSecurityCommandFamily::control,
      "arhc-080-missing-context");
  Require(!missing_context.allowed &&
              missing_context.diagnostic_code ==
                  "AGENT.SECURITY_CONTEXT_REQUIRED",
          "ARHC-080 missing security context was accepted");

  const auto insufficient = agents::EvaluateAgentCommandGrant(
      Context({"OBS_AGENT_STATE_READ"}),
      agents::AgentSecurityCommandFamily::control,
      "arhc-080-insufficient-right");
  Require(!insufficient.allowed && insufficient.missing_right == "OBS_AGENT_CONTROL",
          "ARHC-080 insufficient rights were accepted");

  const auto security_group = Context({}, {"SEC"});
  Require(agents::AgentContextHasRight(security_group, "SEC_AUTH_METRICS_READ"),
          "ARHC-080 SEC group did not retain security metric read");
  Require(!agents::AgentContextHasRight(security_group, "OBS_AGENT_CONTROL"),
          "ARHC-080 SEC group incorrectly gained generic agent control");

  const auto cluster_refused = agents::EvaluateAgentCommandGrant(
      Context({"OBS_CLUSTER_CONTROL", "OBS_AGENT_CONTROL"}),
      agents::AgentSecurityCommandFamily::cluster_control,
      "arhc-080-cluster-control",
      false,
      true);
  Require(!cluster_refused.allowed &&
              cluster_refused.diagnostic_code == "CLUSTER.NOT_AVAILABLE",
          "ARHC-080 cluster authority absence did not fail closed");

  const auto rollback_without_right = agents::EvaluateAgentCommandGrant(
      Context({"OBS_AGENT_CONTROL"}),
      agents::AgentSecurityCommandFamily::policy_rollback,
      "arhc-080-policy-rollback-abuse");
  Require(!rollback_without_right.allowed &&
              rollback_without_right.missing_right == "OBS_POLICY_ROLLBACK",
          "ARHC-080 policy rollback did not require rollback right");

  auto spoofed = OperatorAuthority();
  spoofed.client_authority = true;
  const auto spoofed_status =
      agents::ValidateAgentActionAuthorityProvenance(spoofed);
  Require(!spoofed_status.ok &&
              spoofed_status.diagnostic_code ==
                  "SB_AGENT_ACTION_AUTHORITY.UNTRUSTED_SOURCE",
          "ARHC-080 client spoofed action authority was accepted");

  agents::AgentActuatorProviderRegistry forbidden_registry;
  agents::AgentActuatorProviderDescriptor forbidden;
  forbidden.provider_id = "forbidden:transaction_finality";
  forbidden.owning_agent = "page_allocation_manager";
  forbidden.actuator_id = "transaction_finality";
  forbidden.operation_id = "commit";
  forbidden.authority_domain = agents::AgentActuatorAuthorityDomain::transaction;
  forbidden.idempotent = true;
  const auto forbidden_status = forbidden_registry.Register(forbidden);
  Require(!forbidden_status.ok &&
              forbidden_status.diagnostic_code ==
                  "SB_AGENT_ACTUATOR_PROVIDER.AUTHORITY_DOMAIN_FORBIDDEN",
          "ARHC-080 direct transaction/finality actuator bypass registered");

  agents::CommercialAgentEvidenceBuildRequest protected_request;
  protected_request.action =
      DispatchAction("019f0800-0000-7000-8000-000000000050", "idem-redact");
  protected_request.authority = OperatorAuthority();
  protected_request.provider_id = "page_manager:preallocate_page_family";
  protected_request.input_evidence_digest = "input-digest";
  protected_request.input_metric_digest = "metric-digest-arhc-080";
  protected_request.policy_generation = 17;
  protected_request.scope_uuids = {"019f0800-0000-7000-8000-000000000031"};
  protected_request.decision_payload = "decision payload with protected material";
  protected_request.result_state = "completed";
  protected_request.diagnostic_code = "SB_AGENT_ACTION.OUTCOME_VERIFIED";
  protected_request.redaction_class = "protected_material";
  protected_request.retention_class = "audit";
  protected_request.outcome_verification_evidence_uuid =
      "019f0800-0000-7000-8000-000000000052";
  protected_request.created_at_microseconds = 1;
  protected_request.protected_material_present = true;
  auto protected_evidence =
      agents::BuildCommercialAgentEvidence(protected_request);
  agents::FinalizeCommercialAgentEvidenceDigests(&protected_evidence);
  agents::CommercialAgentEvidenceViewRequest view_request;
  view_request.evidence = protected_evidence;
  view_request.context = Context({"OBS_SUPPORT_BUNDLE_READ"});
  view_request.support_bundle_view = true;
  view_request.now_microseconds = 2;
  const auto view = agents::ProjectCommercialAgentEvidenceView(view_request);
  Require(view.visible && view.redacted && view.protected_material_suppressed,
          "ARHC-080 protected support evidence was not suppressed");
  Require(view.evidence.detail == "redacted" &&
              view.evidence.principal_uuid.empty() &&
              view.evidence.input_metric_digest.empty(),
          "ARHC-080 redaction leakage detected");

  auto catalog = DurableCatalog();
  agents::AgentActuatorProviderRegistry empty_registry;
  auto bypass_action =
      DispatchAction("019f0800-0000-7000-8000-000000000053",
                     "idem-direct-bypass");
  bypass_action.actuator_id = "transaction_finality";
  bypass_action.operation_id = "commit";
  const auto bypass = Dispatch(&catalog, &empty_registry, OperatorAuthority(),
                               bypass_action);
  Require(!bypass.status.ok &&
              bypass.status.diagnostic_code ==
                  "SB_AGENT_ACTUATOR_PROVIDER.UNREGISTERED" &&
              catalog.actions.empty(),
          "ARHC-080 direct actuator bypass wrote durable action state");
}

void TestArhc081CrashRestartAndRaceCoverage() {
  auto catalog = DurableCatalog();
  agents::DurableLeaseRequest lease;
  lease.lease_uuid = "019f0800-0000-7000-8000-000000000060";
  lease.instance_uuid = catalog.instances.front().instance_uuid;
  lease.owner_uuid = "019f0800-0000-7000-8000-000000000061";
  lease.now_microseconds = 1000;
  lease.lease_duration_microseconds = 5000;
  lease.evidence_uuid = "019f0800-0000-7000-8000-000000000062";
  auto status = agents::AcquireDurableAgentLease(&catalog, lease);
  Require(status.ok, "ARHC-081 initial durable lease acquire failed");

  auto duplicate = lease;
  duplicate.owner_uuid = "019f0800-0000-7000-8000-000000000063";
  status = agents::AcquireDurableAgentLease(&catalog, duplicate);
  Require(!status.ok &&
              status.diagnostic_code ==
                  "SB_AGENT_LEASE.DUPLICATE_LIVE_OWNER_REFUSED",
          "ARHC-081 duplicate live lease owner was accepted");

  agents::DurableAgentActionRecord running;
  running.action_uuid = "019f0800-0000-7000-8000-000000000064";
  running.instance_uuid = catalog.instances.front().instance_uuid;
  running.owner_uuid = lease.owner_uuid;
  running.operation_id = "preallocate_page_family";
  running.state = agents::DurableAgentActionState::running;
  running.idempotency_key = "idem-crash-running";
  running.input_evidence_digest = "input-digest-running";
  running.evidence_uuid = "019f0800-0000-7000-8000-000000000065";
  catalog.actions.push_back(running);
  status = agents::RefreshDurableAgentCatalogAuthorityDigest(
      &catalog, running.evidence_uuid);
  Require(status.ok, "ARHC-081 running action root refresh failed");

  const auto encoded = agents::SerializeDurableAgentCatalogImage(catalog);
  const auto decoded = agents::ValidateDurableAgentCatalogImage(encoded, true);
  Require(decoded.status.ok, "ARHC-081 durable pre-crash catalog did not validate");
  const auto pre_service_root = decoded.image.authority.catalog_root_digest;

  agents::AgentRuntimeService service;
  agents::AgentRuntimeServiceOpenRequest open_request;
  open_request.manifest = agents::CanonicalAgentManifest();
  open_request.catalog = decoded.image;
  open_request.production_live_path = true;
  open_request.worker_foreground_protection_enabled = true;
  open_request.crash_recovery_mode = true;
  open_request.service_owner_uuid = "019f0800-0000-7000-8000-000000000066";
  open_request.evidence_uuid = "019f0800-0000-7000-8000-000000000067";
  auto service_result = service.Open(open_request);
  Require(service_result.status.ok, "ARHC-081 crash recovery service open failed");
  Require(service_result.catalog.authority.catalog_root_digest != pre_service_root,
          "ARHC-081 service open did not persist lifecycle evidence");
  Require(!service_result.catalog.retained_history.empty() &&
              service_result.catalog.retained_history.back().event_kind ==
                  "agent_runtime_service.open",
          "ARHC-081 service open history evidence missing");
  Require(!service_result.catalog.authority.previous_catalog_root_digest.empty(),
          "ARHC-081 service open did not retain previous root chain");
  service_result =
      service.Recover("019f0800-0000-7000-8000-000000000068", 2000);
  Require(service_result.status.ok &&
              service_result.status.diagnostic_code ==
                  "SB_AGENT_RECOVERY.REPLAY_DETERMINISTIC",
          "ARHC-081 crash recovery was not deterministic");
  Require(service_result.catalog.leases.front().state ==
              agents::DurableAgentLeaseState::replay_pending,
          "ARHC-081 running lease was not moved to replay_pending");
  Require(service_result.catalog.actions.front().state ==
              agents::DurableAgentActionState::replay_pending,
          "ARHC-081 running action was not moved to replay_pending");
  Require(!service_result.catalog.authority.previous_catalog_root_digest.empty() &&
              service_result.catalog.authority.previous_catalog_root_digest !=
                  service_result.catalog.authority.catalog_root_digest,
          "ARHC-081 recovery did not maintain root chain");
  Require(service_result.catalog.retained_history.size() >= 3,
          "ARHC-081 durable replay history was not retained");

  auto cancel_catalog = DurableCatalog();
  status = agents::AcquireDurableAgentLease(&cancel_catalog, lease);
  Require(status.ok, "ARHC-081 cancellation lease setup failed");
  auto cancel = lease;
  cancel.now_microseconds = 1500;
  cancel.evidence_uuid = "019f0800-0000-7000-8000-000000000069";
  status = agents::CancelDurableAgentLease(
      &cancel_catalog, cancel, agents::DurableAgentLeaseState::cancelled);
  Require(status.ok, "ARHC-081 cancellation did not persist");
  status = agents::HeartbeatDurableAgentLease(&cancel_catalog, lease);
  Require(!status.ok && status.diagnostic_code == "SB_AGENT_LEASE.EXPIRED",
          "ARHC-081 heartbeat after cancellation reactivated a lease");

  auto policy_race_catalog = DurableCatalog(0);
  int dispatch_count = 0;
  auto registry = Registry(&dispatch_count);
  const auto policy_race = Dispatch(
      &policy_race_catalog, &registry, OperatorAuthority(),
      DispatchAction("019f0800-0000-7000-8000-000000000070",
                     "idem-policy-generation-race"));
  Require(!policy_race.status.ok &&
              policy_race.status.diagnostic_code ==
                  "SB_AGENT_COMMERCIAL_EVIDENCE.POLICY_GENERATION_REQUIRED" &&
              dispatch_count == 0 && policy_race_catalog.actions.empty(),
          "ARHC-081 policy-generation race dispatched without durable generation");

  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "ARHC-081 page allocation descriptor missing");
  agents::AgentPolicy approval_policy =
      agents::BaselinePolicyForAgentFamily(*descriptor, "page_preallocation_policy", 80);
  approval_policy.activation = agents::AgentActivationProfile::live_action;
  approval_policy.allow_live_action = true;
  approval_policy.require_dry_run_before_live = false;
  approval_policy.require_manual_approval = true;
  agents::AgentActionRequest approval_action;
  approval_action.action_uuid = "019f0800-0000-7000-8000-000000000071";
  approval_action.agent_type_id = "page_allocation_manager";
  approval_action.instance_uuid = catalog.instances.front().instance_uuid;
  approval_action.actuator_id = "page_allocation_manager";
  approval_action.operation_id = "page_preallocation_request";
  approval_action.idempotency_key = "idem-approval-race";
  approval_action.dry_run = false;
  const auto approval_required = agents::EvaluateAgentAction(
      Context({"OBS_AGENT_STATE_READ", "OBS_AGENT_CONTROL"}), *descriptor,
      approval_policy, approval_action);
  Require(approval_required.result_class ==
              agents::AgentActionResultClass::approval_required &&
              approval_required.diagnostic_code == "SB_AGENT_APPROVAL.REQUIRED",
          "ARHC-081 missing approval did not block live action: " +
              approval_required.diagnostic_code + " / " +
              approval_required.detail);
  approval_action.manual_approval_present = true;
  const auto approval_accepted = agents::EvaluateAgentAction(
      Context({"OBS_AGENT_STATE_READ", "OBS_AGENT_CONTROL"}), *descriptor,
      approval_policy, approval_action);
  Require(approval_accepted.result_class == agents::AgentActionResultClass::accepted,
          "ARHC-081 manual approval did not produce deterministic accepted state");

  agents::AgentActionRequest override_action = approval_action;
  override_action.action_uuid = "019f0800-0000-7000-8000-000000000072";
  override_action.operator_override = true;
  override_action.inputs["scope_uuid"] = "database/security";
  override_action.actuator_id = "security_policy";
  override_action.operation_id = "identity_update";
  override_action.agent_type_id = "identity_lifecycle_manager";
  const auto override_denied = agents::ArbitrateAgentActionsDetailed(
      Context({"OBS_AGENT_CONTROL"}), {override_action});
  Require(override_denied.diagnostic_code == "SB_AGENT_OVERRIDE.RIGHT_REQUIRED",
          "ARHC-081 manual override without override right was accepted");
  const auto override_forbidden = agents::ArbitrateAgentActionsDetailed(
      Context({"OBS_AGENT_CONTROL", "OBS_AGENT_OVERRIDE"}), {override_action});
  Require(override_forbidden.diagnostic_code ==
              "SB_AGENT_OVERRIDE.AUTHORITY_GRANT_FORBIDDEN",
          "ARHC-081 override granted forbidden security/catalog authority");
}

void TestArhc082ProductionFixtureSeparation() {
  agents::AgentProductionFixtureSeparationInput non_production;
  non_production.production_live_path = false;
  non_production.test_fixture_mode = true;
  non_production.fixture_policy = true;
  auto result =
      agents::ValidateAgentProductionFixtureSeparation(non_production);
  Require(result.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_PRODUCTION_FIXTURE.TEST_BOOTSTRAP_ACCEPTED",
          "ARHC-082 non-production fixture bootstrap was not explicitly scoped");

  agents::AgentProductionFixtureSeparationInput input;
  input.fixture_auth = true;
  result = agents::ValidateAgentProductionFixtureSeparation(input);
  Require(!result.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_PRODUCTION_FIXTURE.FIXTURE_AUTH_REFUSED",
          "ARHC-082 production accepted fixture authentication");

  input = {};
  input.test_fixture_mode = true;
  result = agents::ValidateAgentProductionFixtureSeparation(input);
  Require(!result.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_PRODUCTION_FIXTURE.TEST_FIXTURE_MODE_REFUSED",
          "ARHC-082 production accepted test fixture mode");

  input = {};
  input.fixture_policy = true;
  result = agents::ValidateAgentProductionFixtureSeparation(input);
  Require(!result.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_PRODUCTION_FIXTURE.FIXTURE_POLICY_REFUSED",
          "ARHC-082 production accepted fixture policy");

  input = {};
  input.observed_metric_snapshot_required = false;
  result = agents::ValidateAgentProductionFixtureSeparation(input);
  Require(!result.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_PRODUCTION_FIXTURE.RELAXED_METRIC_REFUSED",
          "ARHC-082 production accepted relaxed metric path");

  input = {};
  input.test_seed_material = true;
  result = agents::ValidateAgentProductionFixtureSeparation(input);
  Require(!result.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_PRODUCTION_FIXTURE.TEST_SEED_REFUSED",
          "ARHC-082 production accepted test seed material");

  input = {};
  input.forced_collision_hooks = true;
  result = agents::ValidateAgentProductionFixtureSeparation(input);
  Require(!result.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_PRODUCTION_FIXTURE.FORCED_HOOK_REFUSED",
          "ARHC-082 production accepted forced collision hooks");

  input = {};
  input.durable_runtime_catalog = false;
  result = agents::ValidateAgentProductionFixtureSeparation(input);
  Require(!result.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_PRODUCTION_FIXTURE.PROBE_CATALOG_REFUSED",
          "ARHC-082 production accepted probe-only runtime catalog");

  input = {};
  input.durable_evidence_store = false;
  result = agents::ValidateAgentProductionFixtureSeparation(input);
  Require(!result.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_PRODUCTION_FIXTURE.SIDECAR_EVIDENCE_REFUSED",
          "ARHC-082 production accepted sidecar-only evidence");

  input = {};
  input.simulated_actuator_provider = true;
  result = agents::ValidateAgentProductionFixtureSeparation(input);
  Require(!result.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_PRODUCTION_FIXTURE.SIMULATED_ACTUATOR_REFUSED",
          "ARHC-082 production accepted simulated actuator provider");

  input = {};
  input.debug_only_paths_enabled = true;
  result = agents::ValidateAgentProductionFixtureSeparation(input);
  Require(!result.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_PRODUCTION_FIXTURE.DEBUG_PATH_REFUSED",
          "ARHC-082 production accepted debug-only agent path");

  input = {};
  input.live_agent_surface = true;
  input.management_state_durable = false;
  result = agents::ValidateAgentProductionFixtureSeparation(input);
  Require(!result.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_PRODUCTION_FIXTURE.SYNTHETIC_LIVE_STATE_REFUSED",
          "ARHC-082 production accepted synthetic live management state");

  input = {};
  input.cluster_stub_live_claim = true;
  result = agents::ValidateAgentProductionFixtureSeparation(input);
  Require(!result.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_PRODUCTION_FIXTURE.CLUSTER_STUB_LIVE_REFUSED",
          "ARHC-082 production accepted cluster stub live claim");

  input = {};
  input.production_build = true;
  input.production_live_path = false;
  input.test_fixture_mode = true;
  result = agents::ValidateAgentProductionFixtureSeparation(input);
  Require(!result.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_PRODUCTION_FIXTURE.TEST_FIXTURE_MODE_REFUSED",
          "ARHC-082 production build accepted non-live fixture path");

  input = {};
  result = agents::ValidateAgentProductionFixtureSeparation(input);
  Require(result.ok &&
              result.status.diagnostic_code ==
                  "SB_AGENT_PRODUCTION_FIXTURE.PRODUCTION_PATH_ACCEPTED",
          "ARHC-082 durable production path was refused");
  Require(HasEvidence(result.evidence,
                      "agent_fixture_separation.transaction_finality_authority=false") &&
              HasEvidence(result.evidence,
                          "agent_fixture_separation.visibility_authority=false") &&
              HasEvidence(result.evidence,
                          "agent_fixture_separation.recovery_authority=false") &&
              HasEvidence(result.evidence,
                          "agent_fixture_separation.security_authority=false") &&
              HasEvidence(result.evidence,
                          "agent_fixture_separation.parser_authority=false") &&
              HasEvidence(result.evidence,
                          "agent_fixture_separation.reference_authority=false") &&
              HasEvidence(result.evidence,
                          "agent_fixture_separation.client_authority=false"),
          "ARHC-082 fixture separation evidence missed authority non-drift rows");
}

}  // namespace

int main() {
  try {
    TestArhc080SecurityNegativeAndBypassCoverage();
    TestArhc081CrashRestartAndRaceCoverage();
    TestArhc082ProductionFixtureSeparation();
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
