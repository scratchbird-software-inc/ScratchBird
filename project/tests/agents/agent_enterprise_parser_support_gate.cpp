// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/parser_interface_manager.hpp"
#include "agents/support_bundle_triage_agent.hpp"
#include "agent_durable_catalog.hpp"
#include "agent_enterprise_evidence.hpp"
#include "agents/agent_support_bundle_triage_route_api.hpp"
#include "lifecycle/agent_parser_interface_bridge.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace impl = scratchbird::core::agents::implemented_agents;
namespace api = scratchbird::engine::internal_api;
namespace parser = scratchbird::parser::sbsql;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

agents::DurableAgentCatalogImage DurableCatalog() {
  agents::DurableAgentCatalogImage image;
  image.source = agents::AgentCatalogStateSource::durable_catalog_image;
  image.schema_version = 1;
  image.authority.durable_catalog_authority = true;
  image.authority.mga_transaction_evidence = true;
  image.authority.mga_transaction_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic026-mga");
  image.authority.transaction_generation = 26;
  image.authority.evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic026-open");
  image.authority.database_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic026-db");
  image.authority.catalog_storage_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic026-storage");
  image.authority.storage_commit_evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic026-commit");
  image.authority.catalog_generation = 1;
  image.authority.local_transaction_id = 2600;
  image.authority.storage_catalog_record_evidence = true;
  image.authority.transaction_inventory_bound = true;
  image.authority.fsync_or_checkpoint_evidence = true;
  const auto refreshed = agents::RefreshDurableAgentCatalogAuthorityDigest(
      &image, image.authority.evidence_uuid);
  Require(refreshed.ok, refreshed.diagnostic_code);
  return image;
}

std::vector<agents::AgentObservedMetricSnapshot> ObservedSnapshotsFor(
    const std::string& agent_type_id,
    const std::string& scope_uuid,
    agents::u64 observed_wall_microseconds) {
  const auto descriptor = agents::FindAgentType(agent_type_id);
  Require(descriptor.has_value(), "AEIC-026 missing agent descriptor");
  std::vector<agents::AgentObservedMetricSnapshot> snapshots;
  for (const auto& dependency : descriptor->metric_dependencies) {
    agents::AgentObservedMetricSnapshot snapshot;
    snapshot.metric_family = dependency.metric_family;
    snapshot.namespace_path = dependency.namespace_prefix.empty()
                                  ? dependency.metric_family
                                  : dependency.namespace_prefix + ".observed";
    snapshot.generation = 2600;
    snapshot.observed_wall_microseconds = observed_wall_microseconds;
    snapshot.scope_uuid = scope_uuid;
    snapshot.digest = "sha256:aeic026:" + dependency.metric_family;
    snapshot.source_quality = agents::AgentMetricSourceQuality::trusted;
    snapshot.present = true;
    snapshot.trusted = true;
    snapshot.schema_compatible = true;
    snapshot.trust_provenance = "enterprise_parser_support_gate";
    snapshot.evidence_uuid =
        agents::DeterministicAgentRuntimeObjectUuidFromKey(
            "aeic026-metric-evidence|" + dependency.metric_family);
    snapshot.snapshot_id = "aeic026:" + dependency.metric_family;
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
  }
  return snapshots;
}

void PersistDecision(
    agents::DurableAgentCatalogImage* catalog,
    const std::string& agent_type_id,
    const std::string& operation_id,
    const std::string& decision_kind,
    const std::string& diagnostic_code,
    const std::vector<std::pair<std::string, std::string>>& fields) {
  const auto before_generation = catalog->authority.catalog_generation;
  agents::AgentEnterpriseDecisionEvidenceRequest request;
  request.catalog = catalog;
  request.agent_type_id = agent_type_id;
  request.instance_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(agent_type_id + "-aeic026");
  request.operation_id = operation_id;
  request.principal_uuid =
      agents::DeterministicAgentRuntimePrincipalUuidFromKey("aeic026-principal");
  request.rights_used = {"agent.execute", "agent.recommend", "agent.observe"};
  request.scope_uuids = {
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic026-scope")};
  request.policy_generation = 26;
  request.decision_kind = decision_kind;
  request.result_state = "completed";
  request.diagnostic_code = diagnostic_code;
  request.decision_fields = fields;
  request.outcome_verification_evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          agent_type_id + "-aeic026-verification");
  request.created_at_microseconds = before_generation + 260000;
  request.metric_context.database_uuid = request.scope_uuids.front();
  request.metric_context.principal_uuid = request.principal_uuid;
  request.metric_context.security_context_present = true;
  request.metric_context.wall_now_microseconds = request.created_at_microseconds;
  request.metric_snapshot_options.expected_scope_uuid = request.scope_uuids.front();
  request.observed_metric_snapshots = ObservedSnapshotsFor(
      agent_type_id, request.scope_uuids.front(), request.created_at_microseconds);
  const auto persisted = agents::AppendEnterpriseAgentDecisionEvidence(request);
  Require(persisted.status.ok, persisted.status.diagnostic_code);
  Require(persisted.evidence_written && persisted.action_written &&
              persisted.history_written && persisted.catalog_root_refreshed,
          "AEIC-026 durable evidence write incomplete");
  Require(catalog->authority.catalog_generation > before_generation,
          "AEIC-026 catalog generation did not advance");
}

std::vector<std::pair<std::string, std::string>> EvidencePairsFromStrings(
    const std::vector<std::string>& fields) {
  std::vector<std::pair<std::string, std::string>> pairs;
  for (const auto& field : fields) {
    pairs.emplace_back("route_evidence", field);
  }
  return pairs;
}

parser::ParserLifecycle AttachedParserLifecycle() {
  parser::ParserLifecycle lifecycle;
  parser::ParserPackageLifecycleProof package;
  package.admitted = true;
  package.attestation_verified = true;
  package.no_authority_bypass = true;
  package.parser_package_uuid = "aeic026-parser-package";
  Require(lifecycle.RecordPackageAdmitted(package).accepted,
          "AEIC-026 parser package admission failed");
  Require(lifecycle.RecordWorkerSpawned().accepted,
          "AEIC-026 parser worker spawn failed");
  Require(lifecycle.RecordHelloSent().accepted,
          "AEIC-026 parser HELLO failed");
  Require(lifecycle.RecordHelloAck(true).accepted,
          "AEIC-026 parser HELLO ACK failed");
  parser::ParserEngineAuthorityProof proof;
  proof.authentication_by_engine = true;
  proof.authorization_by_engine = true;
  proof.mga_context_by_engine = true;
  Require(lifecycle.RecordAttachAccepted(proof).accepted,
          "AEIC-026 parser attach failed");
  return lifecycle;
}

parser::ParserInterfaceAgentLifecycleRouteRequest ParserRouteRequest(
    parser::ParserLifecycle* lifecycle,
    const impl::ParserInterfaceManagerResult* decision) {
  parser::ParserInterfaceAgentLifecycleRouteRequest request;
  request.lifecycle = lifecycle;
  request.decision = decision;
  request.engine_supervisor_authority = true;
  request.durable_runtime_store_authority = true;
  request.parser_metrics_authoritative = true;
  return request;
}

void TestParserInterfaceRoute(agents::DurableAgentCatalogImage* catalog) {
  impl::ParserInterfaceManagerSnapshot snapshot;
  snapshot.parser_family = "sbsql";
  snapshot.package_uuid = "aeic026-parser-package";
  snapshot.parser_metrics_authoritative = true;
  snapshot.parser_crashes_total = 4;
  snapshot.parser_sessions_active = 0;
  const auto drain = impl::EvaluateParserInterfaceManager(snapshot);
  Require(drain.ok() &&
              drain.decision ==
                  impl::ParserInterfaceManagerDecisionKind::drain_parser_family,
          "AEIC-026 parser interface did not request drain");

  auto lifecycle = AttachedParserLifecycle();
  const auto routed = parser::ApplyParserInterfaceAgentLifecycleRoute(
      ParserRouteRequest(&lifecycle, &drain));
  Require(routed.ok && routed.drain_applied &&
              lifecycle.state() == parser::ParserLifecycleState::kRecycling,
          "AEIC-026 parser drain route did not mutate lifecycle");
  PersistDecision(catalog,
                  "parser_interface_manager",
                  "parser_interface.route_drain",
                  impl::ParserInterfaceManagerDecisionKindName(drain.decision),
                  routed.diagnostic_code,
                  EvidencePairsFromStrings(routed.evidence));

  snapshot.package_signature_valid = false;
  const auto quarantine = impl::EvaluateParserInterfaceManager(snapshot);
  Require(quarantine.ok() &&
              quarantine.decision ==
                  impl::ParserInterfaceManagerDecisionKind::quarantine_parser_package,
          "AEIC-026 parser interface did not quarantine invalid package");
  auto quarantine_lifecycle = AttachedParserLifecycle();
  const auto quarantine_routed =
      parser::ApplyParserInterfaceAgentLifecycleRoute(
          ParserRouteRequest(&quarantine_lifecycle, &quarantine));
  Require(quarantine_routed.ok && quarantine_routed.quarantine_applied &&
              quarantine_lifecycle.state() ==
                  parser::ParserLifecycleState::kQuarantined,
          "AEIC-026 parser quarantine route did not mutate lifecycle");

  auto unsafe = ParserRouteRequest(&quarantine_lifecycle, &quarantine);
  unsafe.parser_finality_authority = true;
  const auto refused = parser::ApplyParserInterfaceAgentLifecycleRoute(unsafe);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_AGENT_PARSER_INTERFACE_ROUTE.UNSAFE_AUTHORITY",
          "AEIC-026 parser route accepted parser finality authority");
}

api::EnginePrepareSupportBundleRequest SupportRequest() {
  api::EnginePrepareSupportBundleRequest request;
  request.context.trust_mode = api::EngineTrustMode::server_isolated;
  request.context.security_context_present = true;
  request.context.database_uuid.canonical =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic026-db");
  request.context.principal_uuid.canonical =
      agents::DeterministicAgentRuntimePrincipalUuidFromKey("aeic026-principal");
  request.context.database_path = "/tmp/aeic026/protected.sbdb";
  request.option_envelopes.push_back("engine_authorized_support_export:true");
  return request;
}

void TestSupportBundleTriageRoute(agents::DurableAgentCatalogImage* catalog) {
  impl::SupportBundleTriageSnapshot snapshot;
  snapshot.completeness_ratio_per_mille = 950;
  snapshot.agent_actions_total = 4;
  snapshot.evidence_catalog_authoritative = true;
  snapshot.tamper_evidence_valid = true;
  snapshot.redaction_policy_valid = true;
  snapshot.protected_material_present = true;
  snapshot.support_bundle_sink_available = true;
  const auto triage = impl::EvaluateSupportBundleTriage(snapshot);
  Require(triage.ok() &&
              triage.decision ==
                  impl::SupportBundleTriageDecisionKind::prepare_redacted_bundle,
          "AEIC-026 support triage did not prepare redacted bundle");

  api::AgentSupportBundleTriageRouteRequest route;
  route.triage_result = triage;
  route.support_request = SupportRequest();
  route.agent_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic026-support-agent");
  route.evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic026-support-evidence");
  route.durable_evidence_store_authority = true;
  route.tamper_chain_verified = true;
  route.redaction_profile_authoritative = true;
  route.support_export_authorized_by_engine = true;
  const auto routed = api::ApplySupportBundleTriageAgentRoute(route);
  Require(routed.ok && routed.support_bundle_prepared &&
              routed.protected_material_suppressed &&
              routed.support_result.redaction_applied &&
              routed.support_result.forbidden_fields_absent &&
              routed.support_result.flush_required_before_export,
          "AEIC-026 support triage route did not prepare safe bundle");
  PersistDecision(catalog,
                  "support_bundle_triage_agent",
                  "support_bundle_triage.route_prepare",
                  impl::SupportBundleTriageDecisionKindName(triage.decision),
                  routed.diagnostic_code,
                  routed.evidence);

  route.sidecar_authority = true;
  const auto sidecar = api::ApplySupportBundleTriageAgentRoute(route);
  Require(!sidecar.ok &&
              sidecar.diagnostic_code ==
                  "SB_AGENT_SUPPORT_TRIAGE_ROUTE.UNSAFE_AUTHORITY",
          "AEIC-026 support triage accepted sidecar authority");

  route.sidecar_authority = false;
  route.support_request.option_envelopes.clear();
  const auto unauth = api::ApplySupportBundleTriageAgentRoute(route);
  Require(!unauth.ok &&
              unauth.diagnostic_code ==
                  "SB_AGENT_SUPPORT_TRIAGE_ROUTE.ENGINE_AUTH_REQUIRED",
          "AEIC-026 support triage accepted missing engine authorization");

  impl::SupportBundleTriageSnapshot low_completeness = snapshot;
  low_completeness.support_bundle_sink_available = false;
  low_completeness.completeness_ratio_per_mille = 200;
  const auto recommended = impl::EvaluateSupportBundleTriage(low_completeness);
  Require(recommended.ok() &&
              recommended.decision ==
                  impl::SupportBundleTriageDecisionKind::recommend_support_bundle,
          "AEIC-026 support triage did not recommend missing bundle");
  route.triage_result = recommended;
  route.support_request = SupportRequest();
  const auto recommendation_route =
      api::ApplySupportBundleTriageAgentRoute(route);
  Require(recommendation_route.ok &&
              !recommendation_route.support_bundle_prepared &&
              recommendation_route.diagnostic_code ==
                  "SB_AGENT_SUPPORT_TRIAGE_ROUTE.RECOMMENDED",
          "AEIC-026 support recommendation route attempted bundle mutation");
}

}  // namespace

int main() {
  auto catalog = DurableCatalog();
  TestParserInterfaceRoute(&catalog);
  TestSupportBundleTriageRoute(&catalog);
  Require(catalog.evidence.size() == 2,
          "AEIC-026 durable evidence count mismatch");
  Require(catalog.actions.size() == 2,
          "AEIC-026 action count mismatch");
  Require(catalog.retained_history.size() == 2,
          "AEIC-026 retained history count mismatch");
  Require(agents::ValidateDurableAgentCatalogForProduction(catalog).ok,
          "AEIC-026 durable catalog invalid");
  std::cout << "AEIC026_PARSER_INTERFACE_ROUTE "
            << "AEIC026_SUPPORT_BUNDLE_TRIAGE_ROUTE ok\n";
  return EXIT_SUCCESS;
}
