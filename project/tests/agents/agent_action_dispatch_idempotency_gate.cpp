// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_action_dispatch.hpp"
#include "agent_package_provenance_test_support.hpp"

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
  image.authority.mga_transaction_uuid = "019f0200-0000-7000-8000-000000000001";
  image.authority.transaction_generation = 11;
  image.authority.evidence_uuid = "019f0200-0000-7000-8000-000000000002";
  image.authority.database_uuid = "019f0200-0000-7000-8000-000000000003";
  image.authority.catalog_storage_uuid = "019f0200-0000-7000-8000-000000000004";
  image.authority.storage_commit_evidence_uuid = image.authority.evidence_uuid;
  image.authority.catalog_generation = 1;
  image.authority.local_transaction_id = 2011;
  image.authority.storage_catalog_record_evidence = true;
  image.authority.transaction_inventory_bound = true;
  image.authority.fsync_or_checkpoint_evidence = true;

  agents::AgentInstanceRecord instance;
  instance.instance_uuid = "019f0200-0000-7000-8000-000000000010";
  instance.agent_type_id = "page_allocation_manager";
  instance.policy_uuid = "019f0200-0000-7000-8000-000000000011";
  instance.scope = "database/filespace/page_family/page_type";
  instance.state = agents::AgentLifecycleState::registered;
  instance.policy_generation = 11;
  image.instances.push_back(instance);
  const auto refresh =
      agents::RefreshDurableAgentCatalogAuthorityDigest(&image,
                                                        image.authority.evidence_uuid);
  Require(refresh.ok, "fixture durable catalog root digest failed");
  return image;
}

agents::AgentActionAuthorityProvenance SealedAuthority() {
  agents::AgentActionAuthorityProvenance authority;
  authority.source = agents::AgentActionAuthoritySource::sealed_internal_bootstrap;
  authority.principal_uuid = "019f0200-0000-7000-8000-000000000020";
  authority.scope_uuid = "019f0200-0000-7000-8000-000000000021";
  authority.provenance_evidence_uuid = "019f0200-0000-7000-8000-000000000022";
  authority.sealed_bootstrap_authority = true;
  return authority;
}

agents::AgentActionAuthorityProvenance OperatorAuthority() {
  auto authority = SealedAuthority();
  authority.source = agents::AgentActionAuthoritySource::operator_request;
  authority.sealed_bootstrap_authority = false;
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
    snapshot.generation = 100 + ordinal;
    snapshot.observed_wall_microseconds = 1700000000000000ull;
    snapshot.scope_uuid = authority.scope_uuid;
    snapshot.digest = "sha256:" + dependency.metric_family;
    snapshot.source_quality = agents::AgentMetricSourceQuality::trusted;
    snapshot.present = true;
    snapshot.trusted = true;
    snapshot.schema_compatible = true;
    snapshot.trust_provenance = "engine_metric_registry";
    snapshot.evidence_uuid = "metric-evidence-" + std::to_string(ordinal);
    snapshot.snapshot_id = "metric-snapshot-" + std::to_string(ordinal);
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
                                  bool dry_run = false) {
  const auto authority = dry_run ? OperatorAuthority() : SealedAuthority();
  agents::AgentActionRequest action;
  action.action_uuid = std::move(uuid);
  action.agent_type_id = "page_allocation_manager";
  action.instance_uuid = "019f0200-0000-7000-8000-000000000010";
  action.actuator_id = "page_manager";
  action.operation_id = "preallocate_page_family";
  action.idempotency_key = std::move(idempotency_key);
  action.dry_run = dry_run;
  action.inputs["evidence_uuid"] = "019f0200-0000-7000-8000-000000000030";
  action.inputs["metric_digest"] = ObservedMetricDigestForAction(authority);
  action.inputs["safety_envelope_version"] = "1";
  action.inputs["safety_evidence_uuid"] = "019f0200-0000-7000-8000-000000000031";
  action.inputs["policy_evidence_uuid"] = "019f0200-0000-7000-8000-000000000032";
  action.inputs["rollout_mode"] = dry_run ? "dry_run" : "live";
  action.inputs["rollout_state"] = "active";
  action.inputs["rollout_evidence_uuid"] = "019f0200-0000-7000-8000-000000000033";
  action.inputs["failure_threshold"] = "3";
  action.inputs["observed_failures"] = "0";
  action.inputs["retry_limit"] = "2";
  action.inputs["retry_count"] = "0";
  action.inputs["rate_limit_key"] = "page-preallocate";
  action.inputs["rate_limit_per_window"] = "4";
  action.inputs["action_count_in_window"] = "1";
  action.inputs["rate_limit_evidence_uuid"] = "019f0200-0000-7000-8000-000000000034";
  action.inputs["blast_radius_units"] = "1";
  action.inputs["max_blast_radius_units"] = "3";
  action.inputs["blast_radius_evidence_uuid"] = "019f0200-0000-7000-8000-000000000035";
  action.inputs["backup_check_required"] = dry_run ? "false" : "true";
  action.inputs["checkpoint_check_required"] = dry_run ? "false" : "true";
  action.inputs["storage_check_required"] = dry_run ? "false" : "true";
  action.inputs["transaction_check_required"] = dry_run ? "false" : "true";
  action.inputs["backup_evidence_uuid"] = "019f0200-0000-7000-8000-000000000036";
  action.inputs["checkpoint_evidence_uuid"] = "019f0200-0000-7000-8000-000000000037";
  action.inputs["storage_check_evidence_uuid"] = "019f0200-0000-7000-8000-000000000038";
  action.inputs["transaction_evidence_uuid"] = "019f0200-0000-7000-8000-000000000039";
  action.inputs["compensation_required"] = dry_run ? "false" : "true";
  action.inputs["rollback_required"] = dry_run ? "false" : "true";
  action.inputs["compensation_plan_evidence_uuid"] = "019f0200-0000-7000-8000-00000000003a";
  action.inputs["rollback_plan_evidence_uuid"] = "019f0200-0000-7000-8000-00000000003b";
  action.inputs["authority_claims"] = "agent_evidence";
  return action;
}

agents::AgentActuatorProviderDescriptor Provider(bool live = true,
                                                 bool compensation = true) {
  agents::AgentActuatorProviderDescriptor provider;
  provider.provider_id = "page_manager:preallocate_page_family";
  provider.owning_agent = "page_allocation_manager";
  provider.actuator_id = "page_manager";
  provider.operation_id = "preallocate_page_family";
  provider.authority_domain = agents::AgentActuatorAuthorityDomain::page;
  provider.supports_dry_run = true;
  provider.live_route_available = live;
  provider.real_subsystem_handler = live;
  provider.subsystem_handler_id = "storage.page.preallocate_page_family";
  provider.handler_provenance = "unit_real_subsystem_handler";
  provider.handler_evidence_uuid = "provider-evidence-preallocate";
  provider.idempotent = true;
  provider.supports_retry = true;
  provider.supports_rollback_compensation = compensation;
  provider.requires_outcome_verification = true;
  provider.required_evidence_fields = {"evidence_uuid", "metric_digest"};
  if (live) {
    provider.package_provenance =
        agent_test_support::PageProviderPackageProvenance(
            "019f0801-0000-7000-8000-00000000008");
  }
  return provider;
}

agents::AgentActuatorProviderRegistry Registry(int* dispatch_count = nullptr) {
  agents::AgentActuatorProviderRegistry registry;
  const auto status = registry.Register(
      Provider(),
      [dispatch_count](const agents::AgentActuatorProviderRequest& request) {
        if (dispatch_count != nullptr && !request.dry_run) { ++(*dispatch_count); }
        agents::AgentActuatorProviderResult result;
        result.dry_run = request.dry_run;
        if (request.dry_run) {
          result.status = {true, "SB_AGENT_ACTION.DRY_RUN_ONLY", request.action.action_uuid};
          result.outcome_verified = true;
          result.verification_evidence_uuid = "dry-run-verification";
          return result;
        }
        result.dispatched = true;
        result.mutation_attempted = true;
        result.outcome_verified =
            request.subsystem_reported_success && request.intended_state_observed;
        result.compensation_required = !result.outcome_verified;
        result.compensation_attempted = !result.outcome_verified;
        if (!result.outcome_verified) {
          result.compensation_executor_id =
              "storage.page.preallocate_page_family.compensator";
          result.compensation_evidence_uuid =
              "019f0200-0000-7000-8000-000000000092";
        }
        result.verification_evidence_uuid = "live-verification";
        result.status = result.outcome_verified
                            ? agents::AgentRuntimeStatus{
                                  true, "SB_AGENT_ACTION.OUTCOME_VERIFIED",
                                  request.action.action_uuid}
                            : agents::AgentError(
                                  "SB_AGENT_ACTION.OUTCOME_UNVERIFIED_COMPENSATION_REQUIRED",
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
    agents::AgentActionRequest action,
    bool intended_state_observed = true,
    bool include_provider_execution_context = true) {
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
  if (include_provider_execution_context) {
    request.provider_execution_context.engine_owned_registry = true;
    request.provider_execution_context.durable_catalog_store_context = true;
    request.provider_execution_context.engine_request_context_present = true;
    request.provider_execution_context.fsync_or_checkpoint_evidence = true;
    request.provider_execution_context.request_id = "agent-action-idempotency-gate";
    request.provider_execution_context.database_uuid = request.authority.scope_uuid;
    request.provider_execution_context.transaction_uuid =
        "019f0200-0000-7000-8000-000000000090";
    request.provider_execution_context.local_transaction_id = 42;
    request.provider_execution_context.registry_provenance =
        "engine_internal_api_registered_provider_registry";
    request.provider_execution_context.registry_evidence_uuid =
        "019f0200-0000-7000-8000-000000000091";
  }
  request.subsystem_reported_success = true;
  request.intended_state_observed = intended_state_observed;
  return agents::DispatchAgentAction(request);
}

void TestAuthorityProvenance() {
  auto missing = SealedAuthority();
  missing.sealed_bootstrap_authority = false;
  auto status = agents::ValidateAgentActionAuthorityProvenance(missing);
  Require(!status.ok &&
              status.diagnostic_code ==
                  "SB_AGENT_ACTION_AUTHORITY.SEALED_BOOTSTRAP_REQUIRED",
          "unsealed bootstrap authority was accepted");

  auto op = OperatorAuthority();
  op.rights.clear();
  status = agents::ValidateAgentActionAuthorityProvenance(op);
  Require(!status.ok &&
              status.diagnostic_code ==
                  "SB_AGENT_ACTION_AUTHORITY.OPERATOR_CONTROL_REQUIRED",
          "operator authority without control right was accepted");

  auto untrusted = SealedAuthority();
  untrusted.parser_authority = true;
  status = agents::ValidateAgentActionAuthorityProvenance(untrusted);
  Require(!status.ok &&
              status.diagnostic_code ==
                  "SB_AGENT_ACTION_AUTHORITY.UNTRUSTED_SOURCE",
          "parser authority was accepted for action dispatch");
}

void TestProviderRefusals() {
  auto catalog = DurableCatalog();
  agents::AgentActuatorProviderRegistry empty_registry;
  const auto no_provider =
      Dispatch(&catalog, &empty_registry, SealedAuthority(),
               Action("019f0200-0000-7000-8000-000000000040", "idem-no-provider"));
  Require(!no_provider.status.ok &&
              no_provider.status.diagnostic_code ==
                  "SB_AGENT_ACTUATOR_PROVIDER.UNREGISTERED",
          "unregistered provider was accepted");

  agents::AgentActuatorProviderRegistry simulated_registry;
  const auto simulated_registered = simulated_registry.Register(Provider());
  Require(!simulated_registered.ok &&
              simulated_registered.diagnostic_code ==
                  "SB_AGENT_ACTUATOR_PROVIDER.LIVE_EXECUTOR_REQUIRED",
          "live provider without a real executor registered");

  auto no_retry_provider = Provider();
  no_retry_provider.provider_id = "page_manager:preallocate_no_retry";
  no_retry_provider.supports_retry = false;
  const auto no_retry =
      simulated_registry.Register(no_retry_provider, [](const auto&) {
        agents::AgentActuatorProviderResult result;
        result.status = {true, "SHOULD_NOT_REGISTER", "no-retry"};
        return result;
      });
  Require(!no_retry.ok &&
              no_retry.diagnostic_code ==
                  "SB_AGENT_ACTUATOR_PROVIDER.RETRY_CAPABILITY_REQUIRED",
          "live provider without retry capability registered");

  auto no_compensation_provider = Provider();
  no_compensation_provider.provider_id = "page_manager:preallocate_no_comp";
  no_compensation_provider.supports_rollback_compensation = false;
  const auto no_compensation =
      simulated_registry.Register(no_compensation_provider, [](const auto&) {
        agents::AgentActuatorProviderResult result;
        result.status = {true, "SHOULD_NOT_REGISTER", "no-compensation"};
        return result;
      });
  Require(!no_compensation.ok &&
              no_compensation.diagnostic_code ==
                  "SB_AGENT_ACTUATOR_PROVIDER.COMPENSATION_CAPABILITY_REQUIRED",
          "live provider without compensation capability registered");

  auto missing_owner_provider = Provider();
  missing_owner_provider.owning_agent.clear();
  const auto missing_owner =
      simulated_registry.Register(missing_owner_provider, [](const auto&) {
        agents::AgentActuatorProviderResult result;
        result.status = {true, "SHOULD_NOT_REGISTER", "missing-owner"};
        return result;
      });
  Require(!missing_owner.ok &&
              missing_owner.diagnostic_code ==
                  "SB_AGENT_ACTUATOR_PROVIDER.DESCRIPTOR_REQUIRED",
          "provider without owning agent registered");

  auto wrong_agent_provider = Provider();
  wrong_agent_provider.provider_id = "page_manager:preallocate_wrong_agent";
  wrong_agent_provider.owning_agent = "storage_health_manager";
  const auto wrong_agent =
      simulated_registry.Register(wrong_agent_provider, [](const auto&) {
        agents::AgentActuatorProviderResult result;
        result.status = {true, "SHOULD_NOT_REGISTER", "wrong-agent"};
        return result;
      });
  Require(!wrong_agent.ok &&
              wrong_agent.diagnostic_code ==
                  "SB_AGENT_ACTUATOR_PROVIDER.CONTRACT_REQUIRED",
          "provider with mismatched owning agent/action contract registered");

  auto wrong_actuator_provider = Provider();
  wrong_actuator_provider.provider_id = "storage_manager:preallocate_page_family";
  wrong_actuator_provider.actuator_id = "storage_manager";
  wrong_actuator_provider.authority_domain =
      agents::AgentActuatorAuthorityDomain::storage;
  const auto wrong_actuator =
      simulated_registry.Register(wrong_actuator_provider, [](const auto&) {
        agents::AgentActuatorProviderResult result;
        result.status = {true, "SHOULD_NOT_REGISTER", "wrong-actuator"};
        return result;
      });
  Require(!wrong_actuator.ok &&
              wrong_actuator.diagnostic_code ==
                  "SB_AGENT_ACTUATOR_PROVIDER.CONTRACT_ACTUATOR_MISMATCH",
          "provider with mismatched canonical actuator registered");

  int dispatch_count = 0;
  auto registry = Registry(&dispatch_count);
  auto missing = Action("019f0200-0000-7000-8000-000000000041", "idem-missing");
  missing.inputs.erase("metric_digest");
  const auto missing_evidence =
      Dispatch(&catalog, &registry, SealedAuthority(), missing);
  Require(!missing_evidence.status.ok &&
              missing_evidence.status.diagnostic_code ==
                  "SB_AGENT_ACTUATOR_PROVIDER.EVIDENCE_FIELD_REQUIRED",
          "provider missing required evidence was accepted");
  Require(dispatch_count == 0, "provider dispatched before evidence gate");

  agents::AgentActuatorProviderRegistry unavailable_registry;
  bool bypass_callback_called = false;
  Require(unavailable_registry
              .Register(Provider(false),
                        [&bypass_callback_called](
                            const agents::AgentActuatorProviderRequest&) {
                          bypass_callback_called = true;
                          agents::AgentActuatorProviderResult result;
                          result.status = {true, "SHOULD_NOT_RUN", "callback"};
                          result.dispatched = true;
                          result.mutation_attempted = true;
                          result.outcome_verified = true;
                          result.verification_evidence_uuid = "bypass";
                          return result;
                        })
              .ok,
          "unavailable provider registration failed");
  const auto unavailable =
      Dispatch(&catalog, &unavailable_registry, SealedAuthority(),
               Action("019f0200-0000-7000-8000-000000000047",
                      "idem-unavailable-provider"));
  Require(!unavailable.status.ok &&
              unavailable.status.diagnostic_code ==
                  "SB_AGENT_ACTION_SAFETY.LIVE_PROVIDER_UNSAFE",
          "provider callback bypassed live-route proof");
  Require(!bypass_callback_called,
          "unavailable live route still invoked provider callback");

  agents::AgentActuatorProviderRegistry no_mutation_registry;
  Require(no_mutation_registry
              .Register(
                  Provider(),
                  [](const agents::AgentActuatorProviderRequest& request) {
                    agents::AgentActuatorProviderResult result;
                    result.status = {true, "SB_AGENT_ACTION.OUTCOME_VERIFIED",
                                     request.action.action_uuid};
                    result.outcome_verified = true;
                    result.verification_evidence_uuid = "fake-success";
                    return result;
                  })
              .ok,
          "no-mutation provider registration failed");
  const auto no_mutation =
      Dispatch(&catalog, &no_mutation_registry, SealedAuthority(),
               Action("019f0200-0000-7000-8000-000000000048",
                      "idem-no-mutation-evidence"));
  Require(!no_mutation.status.ok &&
              no_mutation.status.diagnostic_code ==
                  "SB_AGENT_ACTUATOR_PROVIDER.LIVE_EXECUTION_EVIDENCE_REQUIRED",
          "live provider success without dispatch/mutation evidence was accepted");

  const auto missing_engine_context =
      Dispatch(&catalog, &registry, SealedAuthority(),
               Action("019f0200-0000-7000-8000-000000000049",
                      "idem-missing-engine-context"),
               true,
               false);
  Require(!missing_engine_context.status.ok &&
              missing_engine_context.status.diagnostic_code ==
                  "SB_AGENT_ACTUATOR_PROVIDER.ENGINE_CONTEXT_REQUIRED",
          "live provider executed without engine request/store context");
}

void TestDryRunNoLiveDispatch() {
  auto catalog = DurableCatalog();
  int dispatch_count = 0;
  auto registry = Registry(&dispatch_count);
  const auto result =
      Dispatch(&catalog, &registry, OperatorAuthority(),
               Action("019f0200-0000-7000-8000-000000000042", "idem-dry-run",
                      true));
  Require(result.status.ok, "dry-run dispatch was refused");
  Require(result.dry_run, "dry-run result flag missing");
  Require(!result.provider_dispatched, "dry-run performed live dispatch");
  Require(dispatch_count == 0, "dry-run incremented live dispatch count");
  Require(result.durable_record_written, "dry-run did not persist action record");
}

void TestLiveDispatchIdempotencyAndCatalogRecord() {
  auto catalog = DurableCatalog();
  int dispatch_count = 0;
  auto registry = Registry(&dispatch_count);
  const auto first =
      Dispatch(&catalog, &registry, SealedAuthority(),
               Action("019f0200-0000-7000-8000-000000000043", "idem-live"));
  Require(first.status.ok, "live dispatch failed");
  Require(first.provider_dispatched, "live dispatch did not call provider");
  Require(first.outcome_verified, "live dispatch did not verify outcome");
  Require(first.durable_record_written, "live dispatch did not persist record");
  Require(catalog.actions.size() == 1, "live dispatch action record missing");
  Require(first.input_evidence_digest.size() == 64,
          "input evidence digest is not SHA-256");
  Require(catalog.actions.front().input_evidence_digest == first.input_evidence_digest,
          "input evidence digest not persisted");
  Require(catalog.actions.front().outcome_verified,
          "outcome verification not persisted");
  Require(!catalog.actions.front().parser_authority &&
              !catalog.actions.front().client_authority &&
              !catalog.actions.front().reference_authority &&
              !catalog.actions.front().sidecar_authority,
          "durable action record claimed untrusted authority");
  Require(dispatch_count == 1, "unexpected dispatch count after first action");

  const auto duplicate =
      Dispatch(&catalog, &registry, SealedAuthority(),
               Action("019f0200-0000-7000-8000-000000000044", "idem-live"));
  Require(duplicate.status.ok &&
              duplicate.status.diagnostic_code ==
                  "SB_AGENT_ACTION_DISPATCH.IDEMPOTENT_REPLAY",
          "duplicate idempotency key did not replay existing outcome");
  Require(duplicate.duplicate_idempotency_key,
          "duplicate idempotency flag missing");
  Require(dispatch_count == 1, "duplicate idempotency key dispatched twice");
  Require(catalog.actions.size() == 1, "duplicate idempotency key wrote new action");
}

void TestVerificationFailureRequiresCompensationOrReplay() {
  auto catalog = DurableCatalog();
  int dispatch_count = 0;
  auto registry = Registry(&dispatch_count);
  const auto failed =
      Dispatch(&catalog, &registry, SealedAuthority(),
               Action("019f0200-0000-7000-8000-000000000045", "idem-fail"),
               false);
  Require(!failed.status.ok &&
              failed.status.diagnostic_code ==
                  "SB_AGENT_ACTION.OUTCOME_UNVERIFIED_COMPENSATION_REQUIRED",
          "unverified outcome did not fail closed");
  Require(failed.compensation_required,
          "unverified outcome did not require compensation");
  Require(failed.quarantined_or_replay_pending,
          "unverified outcome did not enter quarantine/replay state");
  Require(catalog.actions.back().state == agents::DurableAgentActionState::quarantined,
          "unverified action was not quarantined");
  Require(catalog.actions.back().compensation_attempted,
          "compensation attempt evidence was not persisted");
  Require(catalog.actions.back().compensation_executor_id ==
              "storage.page.preallocate_page_family.compensator",
          "compensation executor id was not persisted");
  Require(!catalog.actions.back().compensation_evidence_uuid.empty(),
          "compensation evidence uuid was not persisted");
}

void TestCatalogRoundTripIncludesActionExecutionFields() {
  auto catalog = DurableCatalog();
  auto registry = Registry();
  const auto dispatched =
      Dispatch(&catalog, &registry, SealedAuthority(),
               Action("019f0200-0000-7000-8000-000000000046", "idem-roundtrip"));
  Require(dispatched.status.ok, "roundtrip setup dispatch failed");
  const auto encoded = agents::SerializeDurableAgentCatalogImage(catalog);
  const auto decoded = agents::ValidateDurableAgentCatalogImage(encoded, true);
  Require(decoded.status.ok, "durable catalog action image did not validate");
  Require(decoded.image.actions.size() == 1, "action did not round-trip");
  Require(decoded.image.actions.front().input_evidence_digest ==
              dispatched.input_evidence_digest,
          "action digest did not round-trip");
  Require(decoded.image.actions.front().actuator_provider_id ==
              "page_manager:preallocate_page_family",
          "provider id did not round-trip");
  Require(decoded.image.actions.front().verification_evidence_uuid ==
              "live-verification",
          "verification evidence did not round-trip");
}

int main() {
  try {
    TestAuthorityProvenance();
    TestProviderRefusals();
    TestDryRunNoLiveDispatch();
    TestLiveDispatchIdempotencyAndCatalogRecord();
    TestVerificationFailureRequiresCompensationOrReplay();
    TestCatalogRoundTripIncludesActionExecutionFields();
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
