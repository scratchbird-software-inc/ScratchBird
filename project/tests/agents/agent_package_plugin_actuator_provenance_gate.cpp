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
  image.authority.mga_transaction_uuid = "019f0803-0000-7000-8000-000000000001";
  image.authority.transaction_generation = 80;
  image.authority.evidence_uuid = "019f0803-0000-7000-8000-000000000002";
  image.authority.database_uuid = "019f0803-0000-7000-8000-000000000003";
  image.authority.catalog_storage_uuid = "019f0803-0000-7000-8000-000000000004";
  image.authority.storage_commit_evidence_uuid = image.authority.evidence_uuid;
  image.authority.catalog_generation = 1;
  image.authority.local_transaction_id = 8001;
  image.authority.storage_catalog_record_evidence = true;
  image.authority.transaction_inventory_bound = true;
  image.authority.fsync_or_checkpoint_evidence = true;

  agents::AgentInstanceRecord instance;
  instance.instance_uuid = "019f0803-0000-7000-8000-000000000010";
  instance.agent_type_id = "page_allocation_manager";
  instance.policy_uuid = "019f0803-0000-7000-8000-000000000011";
  instance.scope = "database/filespace/page_family/page_type";
  instance.state = agents::AgentLifecycleState::registered;
  instance.policy_generation = 80;
  image.instances.push_back(instance);
  const auto refresh =
      agents::RefreshDurableAgentCatalogAuthorityDigest(&image,
                                                        image.authority.evidence_uuid);
  Require(refresh.ok, "CEIC-080 catalog root digest failed");
  return image;
}

agents::AgentActionAuthorityProvenance OperatorAuthority() {
  agents::AgentActionAuthorityProvenance authority;
  authority.source = agents::AgentActionAuthoritySource::operator_request;
  authority.principal_uuid = "019f0803-0000-7000-8000-000000000020";
  authority.scope_uuid = "019f0803-0000-7000-8000-000000000021";
  authority.provenance_evidence_uuid = "019f0803-0000-7000-8000-000000000022";
  authority.operator_authority = true;
  authority.rights = {"OBS_AGENT_CONTROL"};
  return authority;
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

std::vector<agents::AgentObservedMetricSnapshot> ObservedMetricSnapshots(
    const agents::AgentActionAuthorityProvenance& authority) {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "CEIC-080 page allocation descriptor missing");
  std::vector<agents::AgentObservedMetricSnapshot> snapshots;
  int ordinal = 0;
  for (const auto& dependency : descriptor->metric_dependencies) {
    agents::AgentObservedMetricSnapshot snapshot;
    snapshot.metric_family = dependency.metric_family;
    snapshot.namespace_path = dependency.namespace_prefix + ".observed";
    snapshot.generation = 8000 + ordinal;
    snapshot.observed_wall_microseconds = 1700000000000000ull;
    snapshot.scope_uuid = authority.scope_uuid;
    snapshot.digest = "sha256:" + dependency.metric_family;
    snapshot.source_quality = agents::AgentMetricSourceQuality::trusted;
    snapshot.present = true;
    snapshot.trusted = true;
    snapshot.schema_compatible = true;
    snapshot.trust_provenance = "engine_metric_registry";
    snapshot.evidence_uuid = "ceic080-metric-evidence-" + std::to_string(ordinal);
    snapshot.snapshot_id = "ceic080-metric-snapshot-" + std::to_string(ordinal);
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
    source_a.source_id = "ceic080-source-a";
    source_a.source_sequence = snapshot.generation * 2 + 1;
    source_a.previous_source_sequence = source_a.source_sequence - 1;
    source_a.attestation_key_id = "metric-key:" + source_a.source_id;
    source_a.attestation_digest = "attestation:" + source_a.metric_family +
                                  ":" + source_a.source_id;
    source_a.evidence_uuid += ":source-a";
    source_a.snapshot_id += ":source-a";
    snapshots.push_back(std::move(source_a));

    auto source_b = snapshot;
    source_b.source_id = "ceic080-source-b";
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

std::string ObservedMetricDigestForAction(
    const agents::AgentActionAuthorityProvenance& authority) {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "CEIC-080 page allocation descriptor missing");
  agents::AgentMetricSnapshotEvaluationOptions options;
  options.expected_scope_uuid = authority.scope_uuid;
  const auto evaluation = agents::EvaluateAgentObservedMetricSnapshots(
      *descriptor,
      MetricContext(authority),
      ObservedMetricSnapshots(authority),
      options);
  Require(evaluation.accepted,
          "CEIC-080 metric digest helper rejected observed snapshots: " +
              evaluation.status.diagnostic_code);
  return evaluation.input_digest;
}

agents::AgentActionRequest Action() {
  const auto authority = OperatorAuthority();
  agents::AgentActionRequest action;
  action.action_uuid = "019f0803-0000-7000-8000-000000000030";
  action.agent_type_id = "page_allocation_manager";
  action.instance_uuid = "019f0803-0000-7000-8000-000000000010";
  action.actuator_id = "page_manager";
  action.operation_id = "preallocate_page_family";
  action.idempotency_key = "ceic080-preallocate-idempotency";
  action.dry_run = false;
  action.inputs["evidence_uuid"] = "019f0803-0000-7000-8000-000000000031";
  action.inputs["metric_digest"] = ObservedMetricDigestForAction(authority);
  action.inputs["safety_envelope_version"] = "1";
  action.inputs["safety_evidence_uuid"] = "019f0803-0000-7000-8000-000000000032";
  action.inputs["policy_evidence_uuid"] = "019f0803-0000-7000-8000-000000000033";
  action.inputs["rollout_mode"] = "live";
  action.inputs["rollout_state"] = "active";
  action.inputs["rollout_evidence_uuid"] = "019f0803-0000-7000-8000-000000000034";
  action.inputs["failure_threshold"] = "3";
  action.inputs["observed_failures"] = "0";
  action.inputs["retry_limit"] = "2";
  action.inputs["retry_count"] = "0";
  action.inputs["rate_limit_key"] = "page-preallocate";
  action.inputs["rate_limit_per_window"] = "4";
  action.inputs["action_count_in_window"] = "1";
  action.inputs["rate_limit_evidence_uuid"] =
      "019f0803-0000-7000-8000-000000000035";
  action.inputs["blast_radius_units"] = "1";
  action.inputs["max_blast_radius_units"] = "3";
  action.inputs["blast_radius_evidence_uuid"] =
      "019f0803-0000-7000-8000-000000000036";
  action.inputs["backup_check_required"] = "true";
  action.inputs["checkpoint_check_required"] = "true";
  action.inputs["storage_check_required"] = "true";
  action.inputs["transaction_check_required"] = "true";
  action.inputs["backup_evidence_uuid"] = "019f0803-0000-7000-8000-000000000037";
  action.inputs["checkpoint_evidence_uuid"] =
      "019f0803-0000-7000-8000-000000000038";
  action.inputs["storage_check_evidence_uuid"] =
      "019f0803-0000-7000-8000-000000000039";
  action.inputs["transaction_evidence_uuid"] =
      "019f0803-0000-7000-8000-00000000003a";
  action.inputs["compensation_required"] = "true";
  action.inputs["rollback_required"] = "true";
  action.inputs["compensation_plan_evidence_uuid"] =
      "019f0803-0000-7000-8000-00000000003b";
  action.inputs["rollback_plan_evidence_uuid"] =
      "019f0803-0000-7000-8000-00000000003c";
  action.inputs["authority_claims"] = "agent_evidence";
  return action;
}

agents::AgentActuatorProviderDescriptor Provider() {
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
  provider.handler_provenance = "ceic080_real_subsystem_handler";
  provider.handler_evidence_uuid = "019f0803-0000-7000-8000-000000000040";
  provider.idempotent = true;
  provider.supports_retry = true;
  provider.supports_rollback_compensation = true;
  provider.requires_outcome_verification = true;
  provider.required_evidence_fields = {"evidence_uuid", "metric_digest"};
  provider.package_provenance =
      agent_test_support::PageProviderPackageProvenance(
          "019f0803-0000-7000-8000-00000000008");
  return provider;
}

agents::AgentActuatorProviderRegistry Registry() {
  agents::AgentActuatorProviderRegistry registry;
  const auto status = registry.Register(
      Provider(),
      [](const agents::AgentActuatorProviderRequest& request) {
        agents::AgentActuatorProviderResult result;
        result.dispatched = !request.dry_run;
        result.mutation_attempted = !request.dry_run;
        result.outcome_verified = true;
        result.verification_evidence_uuid =
            "019f0803-0000-7000-8000-000000000041";
        result.status = {true, "SB_AGENT_ACTION.OUTCOME_VERIFIED",
                         request.action.action_uuid};
        return result;
      });
  Require(status.ok, "CEIC-080 provider registration failed: " +
                         status.diagnostic_code + ":" + status.detail);
  return registry;
}

void ExerciseProvenanceValidator() {
  auto bundle = agent_test_support::PageProviderPackageProvenance(
      "019f0804-0000-7000-8000-00000000008");
  auto accepted = agents::ValidateAgentPackageProvenanceBundle(bundle);
  Require(accepted.accepted && !accepted.bundle_digest.empty(),
          "CEIC-080 valid package provenance refused");

  auto missing_signature = bundle;
  missing_signature.records[0].signature_verified = false;
  agents::FinalizeAgentPackageProvenanceDigest(&missing_signature.records[0]);
  auto status =
      agents::ValidateAgentPackageProvenanceBundle(missing_signature);
  Require(!status.accepted &&
              status.status.diagnostic_code ==
                  "SB_AGENT_PACKAGE_PROVENANCE.SIGNATURE_NOT_VERIFIED",
          "CEIC-080 missing signature verification accepted");

  auto missing_sbom = bundle;
  missing_sbom.records[1].sbom_present = false;
  agents::FinalizeAgentPackageProvenanceDigest(&missing_sbom.records[1]);
  status = agents::ValidateAgentPackageProvenanceBundle(missing_sbom);
  Require(!status.accepted &&
              status.status.diagnostic_code ==
                  "SB_AGENT_PACKAGE_PROVENANCE.SBOM_REQUIRED",
          "CEIC-080 missing SBOM accepted");

  auto bad_signer = bundle;
  bad_signer.records[2].signer_identity = "unapproved-signer";
  agents::FinalizeAgentPackageProvenanceDigest(&bad_signer.records[2]);
  status = agents::ValidateAgentPackageProvenanceBundle(bad_signer);
  Require(!status.accepted &&
              status.status.diagnostic_code ==
                  "SB_AGENT_PACKAGE_PROVENANCE.SIGNER_NOT_ALLOWED",
          "CEIC-080 unapproved signer accepted");

  auto below_minimum = bundle;
  below_minimum.records[0].package_version_ordinal = 99;
  agents::FinalizeAgentPackageProvenanceDigest(&below_minimum.records[0]);
  status = agents::ValidateAgentPackageProvenanceBundle(below_minimum);
  Require(!status.accepted &&
              status.status.diagnostic_code ==
                  "SB_AGENT_PACKAGE_PROVENANCE.VERSION_BELOW_POLICY",
          "CEIC-080 below-minimum package version accepted");

  auto revoked = bundle;
  revoked.records[1].revocation_status =
      agents::AgentPackageRevocationStatus::revoked;
  agents::FinalizeAgentPackageProvenanceDigest(&revoked.records[1]);
  status = agents::ValidateAgentPackageProvenanceBundle(revoked);
  Require(!status.accepted &&
              status.status.diagnostic_code ==
                  "SB_AGENT_PACKAGE_PROVENANCE.REVOCATION_STATUS_REQUIRED",
          "CEIC-080 revoked package accepted");

  auto missing_sandbox = bundle;
  missing_sandbox.records[2].sandbox_profile_id.clear();
  agents::FinalizeAgentPackageProvenanceDigest(&missing_sandbox.records[2]);
  status = agents::ValidateAgentPackageProvenanceBundle(missing_sandbox);
  Require(!status.accepted &&
              status.status.diagnostic_code ==
                  "SB_AGENT_PACKAGE_PROVENANCE.SANDBOX_PROFILE_REQUIRED",
          "CEIC-080 missing sandbox profile accepted");

  auto test_package = bundle;
  test_package.records[0].production_package = false;
  test_package.records[0].test_fixture_package = true;
  agents::FinalizeAgentPackageProvenanceDigest(&test_package.records[0]);
  status = agents::ValidateAgentPackageProvenanceBundle(test_package);
  Require(!status.accepted &&
              status.status.diagnostic_code ==
                  "SB_AGENT_PACKAGE_PROVENANCE.TEST_PACKAGE_REFUSED",
          "CEIC-080 test package accepted on production path");

  auto cluster_without_external_provider = bundle;
  cluster_without_external_provider.policy.local_cluster_routes_allowed = true;
  cluster_without_external_provider.records[1].cluster_route_requested = true;
  agents::FinalizeAgentPackageProvenanceDigest(
      &cluster_without_external_provider.records[1]);
  status = agents::ValidateAgentPackageProvenanceBundle(
      cluster_without_external_provider);
  Require(!status.accepted &&
              status.status.diagnostic_code ==
                  "SB_AGENT_PACKAGE_PROVENANCE.CLUSTER_EXTERNAL_PROVIDER_REQUIRED",
          "CEIC-080 cluster package accepted without external provider proof");

  auto cluster_with_external_provider = bundle;
  cluster_with_external_provider.policy.local_cluster_routes_allowed = true;
  cluster_with_external_provider.records[1].cluster_route_requested = true;
  cluster_with_external_provider.records[1].external_cluster_provider_attested =
      true;
  cluster_with_external_provider.records[1]
      .external_cluster_provider_evidence_uuid =
      "external-cluster-provider-evidence";
  agents::FinalizeAgentPackageProvenanceDigest(
      &cluster_with_external_provider.records[1]);
  status =
      agents::ValidateAgentPackageProvenanceBundle(cluster_with_external_provider);
  Require(status.accepted,
          "CEIC-080 external-cluster-attested package provenance refused: " +
              status.status.diagnostic_code);

  auto forbidden_authority = bundle;
  forbidden_authority.records[0].memory_authority = true;
  agents::FinalizeAgentPackageProvenanceDigest(&forbidden_authority.records[0]);
  status = agents::ValidateAgentPackageProvenanceBundle(forbidden_authority);
  Require(!status.accepted &&
              status.status.diagnostic_code ==
                  "SB_AGENT_PACKAGE_PROVENANCE.FORBIDDEN_AUTHORITY",
          "CEIC-080 package provenance authority drift accepted");
}

void ExerciseProviderRegistrationAndDispatch() {
  agents::AgentActuatorProviderRegistry rejected_registry;
  auto missing_provenance = Provider();
  missing_provenance.package_provenance.records.clear();
  auto status = rejected_registry.Register(missing_provenance, [](const auto&) {
    return agents::AgentActuatorProviderResult{};
  });
  Require(!status.ok &&
              status.diagnostic_code ==
                  "SB_AGENT_PACKAGE_PROVENANCE.PLUGIN_RECORD_REQUIRED",
          "CEIC-080 live provider registered without package provenance");

  auto registry = Registry();
  auto catalog = DurableCatalog();
  agents::AgentActionDispatchRequest request;
  request.catalog = &catalog;
  request.registry = &registry;
  request.authority = OperatorAuthority();
  request.action = Action();
  request.production_live_path = true;
  request.metric_context = MetricContext(request.authority);
  request.metric_snapshot_options.expected_scope_uuid =
      request.authority.scope_uuid;
  request.observed_metric_snapshots = ObservedMetricSnapshots(request.authority);
  request.provider_execution_context.engine_owned_registry = true;
  request.provider_execution_context.durable_catalog_store_context = true;
  request.provider_execution_context.engine_request_context_present = true;
  request.provider_execution_context.fsync_or_checkpoint_evidence = true;
  request.provider_execution_context.request_id = "ceic080-dispatch";
  request.provider_execution_context.database_uuid = request.authority.scope_uuid;
  request.provider_execution_context.transaction_uuid =
      "019f0803-0000-7000-8000-000000000090";
  request.provider_execution_context.local_transaction_id = 8080;
  request.provider_execution_context.registry_provenance =
      "engine_owned_agent_provider_registry";
  request.provider_execution_context.registry_evidence_uuid =
      "019f0803-0000-7000-8000-000000000091";

  const auto result = agents::DispatchAgentAction(request);
  Require(result.status.ok,
          "CEIC-080 dispatch with package provenance failed: " +
              result.status.diagnostic_code + ":" + result.status.detail);
  Require(result.package_provenance_validated,
          "CEIC-080 dispatch did not validate package provenance");
  Require(result.provider_dispatched && result.outcome_verified,
          "CEIC-080 provider did not execute with verified outcome");
}

int main() {
  try {
    ExerciseProvenanceValidator();
    ExerciseProviderRegistrationAndDispatch();
    std::cout << "agent_package_plugin_actuator_provenance_gate=pass\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "agent_package_plugin_actuator_provenance_gate=fail:"
              << ex.what() << "\n";
    return 1;
  }
}
