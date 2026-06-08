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
  image.authority.mga_transaction_uuid = "019f0700-0000-7000-8000-000000000001";
  image.authority.transaction_generation = 71;
  image.authority.evidence_uuid = "019f0700-0000-7000-8000-000000000002";
  image.authority.database_uuid = "019f0700-0000-7000-8000-000000000003";
  image.authority.catalog_storage_uuid = "019f0700-0000-7000-8000-000000000004";
  image.authority.storage_commit_evidence_uuid = image.authority.evidence_uuid;
  image.authority.catalog_generation = 1;
  image.authority.local_transaction_id = 7071;
  image.authority.storage_catalog_record_evidence = true;
  image.authority.transaction_inventory_bound = true;
  image.authority.fsync_or_checkpoint_evidence = true;

  agents::AgentInstanceRecord instance;
  instance.instance_uuid = "019f0700-0000-7000-8000-000000000010";
  instance.agent_type_id = "page_allocation_manager";
  instance.policy_uuid = "019f0700-0000-7000-8000-000000000011";
  instance.scope = "database/filespace/page_family/page_type";
  instance.state = agents::AgentLifecycleState::registered;
  instance.policy_generation = 17;
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
  authority.principal_uuid = "019f0700-0000-7000-8000-000000000020";
  authority.scope_uuid = "019f0700-0000-7000-8000-000000000021";
  authority.provenance_evidence_uuid = "019f0700-0000-7000-8000-000000000022";
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
    snapshot.generation = 700 + ordinal;
    snapshot.observed_wall_microseconds = 1700000000000070ull;
    snapshot.scope_uuid = authority.scope_uuid;
    snapshot.digest = "sha256:" + dependency.metric_family;
    snapshot.source_quality = agents::AgentMetricSourceQuality::trusted;
    snapshot.present = true;
    snapshot.trusted = true;
    snapshot.schema_compatible = true;
    snapshot.trust_provenance = "engine_metric_registry";
    snapshot.evidence_uuid = "metric-evidence-arhc070-" +
                             std::to_string(ordinal);
    snapshot.snapshot_id = "metric-snapshot-arhc070-" +
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
  context.wall_now_microseconds = 1700000000001070ull;
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
                                  std::string redaction_class = "standard") {
  const auto authority = OperatorAuthority();
  agents::AgentActionRequest action;
  action.action_uuid = std::move(uuid);
  action.agent_type_id = "page_allocation_manager";
  action.instance_uuid = "019f0700-0000-7000-8000-000000000010";
  action.actuator_id = "page_manager";
  action.operation_id = "preallocate_page_family";
  action.idempotency_key = std::move(idempotency_key);
  action.dry_run = false;
  action.inputs["evidence_uuid"] = "019f0700-0000-7000-8000-000000000030";
  action.inputs["metric_digest"] = ObservedMetricDigestForAction(authority);
  action.inputs["scope_uuid"] = "019f0700-0000-7000-8000-000000000031";
  action.inputs["redaction_class"] = std::move(redaction_class);
  action.inputs["retention_class"] = "audit";
  action.inputs["safety_envelope_version"] = "1";
  action.inputs["safety_evidence_uuid"] = "019f0700-0000-7000-8000-000000000032";
  action.inputs["policy_evidence_uuid"] = "019f0700-0000-7000-8000-000000000033";
  action.inputs["rollout_mode"] = "live";
  action.inputs["rollout_state"] = "active";
  action.inputs["rollout_evidence_uuid"] = "019f0700-0000-7000-8000-000000000034";
  action.inputs["failure_threshold"] = "3";
  action.inputs["observed_failures"] = "0";
  action.inputs["retry_limit"] = "2";
  action.inputs["retry_count"] = "0";
  action.inputs["rate_limit_key"] = "page-preallocate";
  action.inputs["rate_limit_per_window"] = "4";
  action.inputs["action_count_in_window"] = "1";
  action.inputs["rate_limit_evidence_uuid"] = "019f0700-0000-7000-8000-000000000035";
  action.inputs["blast_radius_units"] = "1";
  action.inputs["max_blast_radius_units"] = "3";
  action.inputs["blast_radius_evidence_uuid"] = "019f0700-0000-7000-8000-000000000036";
  action.inputs["backup_check_required"] = "true";
  action.inputs["checkpoint_check_required"] = "true";
  action.inputs["storage_check_required"] = "true";
  action.inputs["transaction_check_required"] = "true";
  action.inputs["backup_evidence_uuid"] = "019f0700-0000-7000-8000-000000000037";
  action.inputs["checkpoint_evidence_uuid"] = "019f0700-0000-7000-8000-000000000038";
  action.inputs["storage_check_evidence_uuid"] = "019f0700-0000-7000-8000-000000000039";
  action.inputs["transaction_evidence_uuid"] = "019f0700-0000-7000-8000-00000000003a";
  action.inputs["compensation_required"] = "true";
  action.inputs["rollback_required"] = "true";
  action.inputs["compensation_plan_evidence_uuid"] = "019f0700-0000-7000-8000-00000000003b";
  action.inputs["rollback_plan_evidence_uuid"] = "019f0700-0000-7000-8000-00000000003c";
  action.inputs["authority_claims"] = "agent_evidence";
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
  provider.handler_provenance = "commercial_evidence_gate_real_subsystem_handler";
  provider.handler_evidence_uuid = "019f0700-0000-7000-8000-000000000041";
  provider.idempotent = true;
  provider.supports_retry = true;
  provider.supports_rollback_compensation = true;
  provider.requires_outcome_verification = true;
  provider.required_evidence_fields = {"evidence_uuid", "metric_digest"};
  provider.package_provenance =
      agent_test_support::PageProviderPackageProvenance(
          "019f0701-0000-7000-8000-00000000008");
  const auto status = registry.Register(
      provider,
      [](const agents::AgentActuatorProviderRequest& request) {
        agents::AgentActuatorProviderResult result;
        result.dispatched = !request.dry_run;
        result.mutation_attempted = !request.dry_run;
        result.outcome_verified = true;
        result.verification_evidence_uuid =
            "019f0700-0000-7000-8000-000000000040";
        result.status = {true, "SB_AGENT_ACTION.OUTCOME_VERIFIED",
                         request.action.action_uuid};
        return result;
      });
  Require(status.ok, "provider registration failed");
  return registry;
}

agents::AgentActionDispatchResult Dispatch(
    agents::DurableAgentCatalogImage* catalog,
    const agents::AgentActuatorProviderRegistry* registry,
    agents::AgentActionRequest action) {
  agents::AgentActionDispatchRequest request;
  request.catalog = catalog;
  request.registry = registry;
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
  request.provider_execution_context.request_id = "agent-commercial-evidence-gate";
  request.provider_execution_context.database_uuid = request.authority.scope_uuid;
  request.provider_execution_context.transaction_uuid =
      "019f0700-0000-7000-8000-000000000090";
  request.provider_execution_context.local_transaction_id = 70;
  request.provider_execution_context.registry_provenance =
      "engine_internal_api_registered_provider_registry";
  request.provider_execution_context.registry_evidence_uuid =
      "019f0700-0000-7000-8000-000000000091";
  request.subsystem_reported_success = true;
  request.intended_state_observed = true;
  return agents::DispatchAgentAction(request);
}

agents::AgentRuntimeContext Context(std::vector<std::string> rights) {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.database_uuid = "019f0700-0000-7000-8000-000000000050";
  context.principal_uuid = "019f0700-0000-7000-8000-000000000051";
  context.rights = std::move(rights);
  context.wall_now_microseconds = 1700000000000070ull;
  return context;
}

void TestRoundTripDurableCommercialEvidence() {
  auto catalog = DurableCatalog();
  const auto registry = Registry();
  const auto result = Dispatch(
      &catalog, &registry,
      Action("019f0700-0000-7000-8000-000000000060", "idem-roundtrip"));
  Require(result.status.ok, "action dispatch failed");
  Require(result.commercial_evidence_written_before_action_record,
          "action success was not gated by prior commercial evidence");
  Require(catalog.evidence.size() == 1, "commercial evidence not persisted");
  Require(catalog.actions.size() == 1, "action record not persisted");
  Require(catalog.evidence.front().evidence_uuid ==
              catalog.actions.front().evidence_uuid,
          "action record is not bound to commercial evidence");

  const auto& evidence = catalog.evidence.front();
  Require(evidence.input_metric_digest == ObservedMetricDigestForAction(OperatorAuthority()),
          "input metric digest missing");
  Require(evidence.policy_generation == 17, "policy generation missing");
  Require(evidence.principal_uuid ==
              "019f0700-0000-7000-8000-000000000020",
          "principal missing");
  Require(!evidence.rights_used.empty(), "rights used missing");
  Require(evidence.scope_uuids.size() == 2, "scope UUIDs missing");
  Require(!evidence.decision_payload_digest.empty(),
          "decision payload digest missing");
  Require(evidence.result_state == "completed", "result state missing");
  Require(evidence.retention_class == "audit", "retention class missing");
  Require(evidence.outcome_verification_evidence_uuid ==
              "019f0700-0000-7000-8000-000000000040",
          "outcome verification evidence missing");
  Require(!evidence.parser_authority && !evidence.client_authority &&
              !evidence.donor_authority && !evidence.sidecar_authority &&
              !evidence.transaction_authority && !evidence.finality_authority &&
              !evidence.visibility_authority && !evidence.recovery_authority &&
              !evidence.security_authority,
          "commercial evidence claimed forbidden authority");
  Require(agents::ValidateCommercialAgentEvidence(evidence).status.ok,
          "commercial evidence did not validate");

  const auto encoded = agents::SerializeDurableAgentCatalogImage(catalog);
  const auto decoded = agents::ValidateDurableAgentCatalogImage(encoded, true);
  Require(decoded.status.ok, "durable image failed validation");
  Require(decoded.image.evidence.size() == 1,
          "commercial evidence did not round-trip");
  Require(decoded.image.evidence.front().tamper_digest == evidence.tamper_digest,
          "tamper digest did not round-trip");
  Require(decoded.image.evidence.front().decision_payload_digest ==
              evidence.decision_payload_digest,
          "decision payload digest did not round-trip");
}

void TestSupportBundleProtectedMaterialSuppression() {
  auto catalog = DurableCatalog();
  const auto registry = Registry();
  const auto result = Dispatch(
      &catalog, &registry,
      Action("019f0700-0000-7000-8000-000000000061", "idem-protected",
             "protected_material"));
  Require(result.status.ok, "protected-material setup failed");
  agents::CommercialAgentEvidenceViewRequest request;
  request.evidence = catalog.evidence.front();
  request.context = Context({"OBS_SUPPORT_BUNDLE_READ"});
  request.support_bundle_view = true;
  request.now_microseconds = 2;
  const auto view = agents::ProjectCommercialAgentEvidenceView(request);
  Require(view.visible, "support-bundle evidence was hidden");
  Require(view.redacted, "protected material was not redacted");
  Require(view.protected_material_suppressed,
          "protected material suppression flag missing");
  Require(view.evidence.detail == "redacted", "protected detail leaked");
  Require(view.evidence.principal_uuid.empty(), "protected principal leaked");
  Require(view.evidence.input_metric_digest.empty(),
          "protected metric digest leaked");
}

void TestTamperDetection() {
  auto catalog = DurableCatalog();
  const auto registry = Registry();
  const auto result = Dispatch(
      &catalog, &registry,
      Action("019f0700-0000-7000-8000-000000000062", "idem-tamper"));
  Require(result.status.ok, "tamper setup failed");
  auto evidence = catalog.evidence.front();
  evidence.detail = "tampered";
  agents::CommercialAgentEvidenceViewRequest request;
  request.evidence = evidence;
  request.context = Context({"OBS_SUPPORT_BUNDLE_READ"});
  request.support_bundle_view = true;
  const auto view = agents::ProjectCommercialAgentEvidenceView(request);
  Require(!view.visible, "tampered evidence was visible");
  Require(!view.tamper_valid, "tamper digest was accepted");
  Require(view.status.diagnostic_code ==
              "SB_AGENT_COMMERCIAL_EVIDENCE.TAMPER_DIGEST_MISMATCH",
          "tamper diagnostic mismatch");
}

void TestRetentionExpiry() {
  auto catalog = DurableCatalog();
  const auto registry = Registry();
  const auto result = Dispatch(
      &catalog, &registry,
      Action("019f0700-0000-7000-8000-000000000063", "idem-expire"));
  Require(result.status.ok, "retention setup failed");
  auto evidence = catalog.evidence.front();
  evidence.retention_class = "short";
  evidence.expires_at_microseconds = 10;
  agents::FinalizeCommercialAgentEvidenceDigests(&evidence);
  agents::CommercialAgentEvidenceViewRequest request;
  request.evidence = evidence;
  request.context = Context({"OBS_SUPPORT_BUNDLE_READ"});
  request.support_bundle_view = true;
  request.now_microseconds = 11;
  const auto view = agents::ProjectCommercialAgentEvidenceView(request);
  Require(!view.visible, "expired evidence was visible");
  Require(view.expired, "retention expiry was not reported");
  Require(view.status.diagnostic_code ==
              "SB_AGENT_COMMERCIAL_EVIDENCE.RETENTION_EXPIRED",
          "retention expiry diagnostic mismatch");
}

void TestHmacSignatureTamperDetection() {
  auto catalog = DurableCatalog();
  const auto registry = Registry();
  const auto result = Dispatch(
      &catalog, &registry,
      Action("019f0700-0000-7000-8000-000000000065", "idem-signature"));
  Require(result.status.ok, "signature setup failed");
  auto evidence = catalog.evidence.front();
  Require(evidence.tamper_signature_algorithm == "hmac-sha256-v1",
          "commercial evidence did not use HMAC signature algorithm");
  Require(!evidence.tamper_signature.empty(),
          "commercial evidence signature missing");
  Require(!evidence.tamper_key_provenance.empty(),
          "commercial evidence key provenance missing");
  auto bad_signature = evidence;
  bad_signature.tamper_signature = "00";
  const auto validation =
      agents::ValidateCommercialAgentEvidence(bad_signature);
  Require(!validation.status.ok &&
              validation.status.diagnostic_code ==
                  "SB_AGENT_COMMERCIAL_EVIDENCE.TAMPER_DIGEST_MISMATCH",
          "tampered HMAC signature was accepted");
}

void TestAccessControlRefusal() {
  auto catalog = DurableCatalog();
  const auto registry = Registry();
  const auto result = Dispatch(
      &catalog, &registry,
      Action("019f0700-0000-7000-8000-000000000064", "idem-access"));
  Require(result.status.ok, "access setup failed");
  agents::CommercialAgentEvidenceViewRequest request;
  request.evidence = catalog.evidence.front();
  request.context = Context({});
  request.support_bundle_view = true;
  const auto view = agents::ProjectCommercialAgentEvidenceView(request);
  Require(!view.visible, "evidence was visible without support-bundle right");
  Require(view.status.diagnostic_code ==
              "SB_AGENT_COMMERCIAL_EVIDENCE.ACCESS_REFUSED",
          "access-control diagnostic mismatch");
}

int main() {
  try {
    TestRoundTripDurableCommercialEvidence();
    TestSupportBundleProtectedMaterialSuppression();
    TestTamperDetection();
    TestRetentionExpiry();
    TestAccessControlRefusal();
    TestHmacSignatureTamperDetection();
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
