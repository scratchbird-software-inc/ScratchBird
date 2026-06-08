// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/admission_control_manager.hpp"
#include "agents/alert_manager.hpp"
#include "agents/memory_governor.hpp"
#include "agents/metrics_registry_manager.hpp"
#include "agents/node_resource_agent.hpp"
#include "agent_durable_catalog.hpp"
#include "agent_enterprise_evidence.hpp"
#include "agent_production_classification.hpp"
#include "metric_history.hpp"
#include "metric_registry.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>

namespace {

namespace agents = scratchbird::core::agents;
namespace impl = scratchbird::core::agents::implemented_agents;
namespace metrics = scratchbird::core::metrics;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

std::map<std::string, agents::AgentProductionExposureRecord> ExposureByAgent() {
  std::map<std::string, agents::AgentProductionExposureRecord> by_agent;
  for (const auto& record : agents::ClassifyAllCanonicalAgentProductionExposures()) {
    by_agent.emplace(record.agent_type_id, record);
  }
  return by_agent;
}

agents::DurableAgentCatalogImage DurableCatalog() {
  agents::DurableAgentCatalogImage image;
  image.source = agents::AgentCatalogStateSource::durable_catalog_image;
  image.schema_version = 1;
  image.authority.durable_catalog_authority = true;
  image.authority.mga_transaction_evidence = true;
  image.authority.mga_transaction_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic-local-mga");
  image.authority.transaction_generation = 9;
  image.authority.evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic-local-open");
  image.authority.database_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic-local-db");
  image.authority.catalog_storage_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic-local-storage");
  image.authority.storage_commit_evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic-local-commit");
  image.authority.catalog_generation = 1;
  image.authority.local_transaction_id = 42;
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
  Require(descriptor.has_value(), "agent descriptor missing for metric snapshot");
  std::vector<agents::AgentObservedMetricSnapshot> snapshots;
  for (const auto& dependency : descriptor->metric_dependencies) {
    agents::AgentObservedMetricSnapshot snapshot;
    snapshot.metric_family = dependency.metric_family;
    snapshot.namespace_path = dependency.namespace_prefix.empty()
                                  ? dependency.metric_family
                                  : dependency.namespace_prefix + ".observed";
    snapshot.generation = 7;
    snapshot.observed_wall_microseconds = observed_wall_microseconds;
    snapshot.scope_uuid = scope_uuid;
    snapshot.digest = "sha256:aeic-local-resource:" + dependency.metric_family;
    snapshot.source_quality = agents::AgentMetricSourceQuality::trusted;
    snapshot.present = true;
    snapshot.trusted = true;
    snapshot.schema_compatible = true;
    snapshot.trust_provenance = "test_metric_registry";
    snapshot.evidence_uuid =
        agents::DeterministicAgentRuntimeObjectUuidFromKey(
            "aeic-local-resource-metric-evidence|" + dependency.metric_family);
    snapshot.snapshot_id = "aeic-local-resource:" + dependency.metric_family;
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

void PersistDecision(agents::DurableAgentCatalogImage* catalog,
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
      agents::DeterministicAgentRuntimeObjectUuidFromKey(agent_type_id + "-instance");
  request.operation_id = operation_id;
  request.principal_uuid =
      agents::DeterministicAgentRuntimePrincipalUuidFromKey("aeic-local-principal");
  request.rights_used = {"agent.execute", "agent.observe"};
  request.scope_uuids = {
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic-local-scope")};
  request.policy_generation = 11;
  request.decision_kind = decision_kind;
  request.result_state = "completed";
  request.diagnostic_code = diagnostic_code;
  request.decision_fields = fields;
  request.outcome_verification_evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(agent_type_id + "-verification");
  request.created_at_microseconds = before_generation + 100;
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
          "enterprise local resource evidence not fully durable");
  Require(catalog->authority.catalog_generation > before_generation,
          "enterprise local resource catalog generation did not advance");
}

template <typename EvidenceField>
std::vector<std::pair<std::string, std::string>> EvidencePairs(
    const std::vector<EvidenceField>& fields) {
  std::vector<std::pair<std::string, std::string>> pairs;
  for (const auto& field : fields) {
    pairs.emplace_back(field.key, field.value);
  }
  return pairs;
}

void TestNodeResourceAgent(agents::DurableAgentCatalogImage* catalog) {
  impl::NodeResourceAgentSnapshot snapshot;
  snapshot.cpu_count = 16;
  snapshot.total_memory_bytes = 64ull * 1024ull * 1024ull * 1024ull;
  snapshot.available_memory_bytes = 48ull * 1024ull * 1024ull * 1024ull;
  snapshot.page_size_bytes = 16384;
  snapshot.scheduler_queue_depth = 4;
  snapshot.memory_pressure_percent = 15;
  snapshot.os_probe_authoritative = true;
  snapshot.metric_registry_authoritative = true;
  const auto result = impl::EvaluateNodeResourceAgentSnapshot(snapshot);
  Require(result.ok(), "node resource agent rejected trusted local snapshot");
  Require(result.publish_node_capability, "node capability publish missing");
  Require(result.publish_role_suitability, "role suitability publish missing");
  Require(result.role_suitability_score > 0, "role suitability score missing");
  PersistDecision(catalog,
                  "node_resource_agent",
                  "node_resource.publish_capability",
                  impl::NodeResourceAgentDecisionKindName(result.decision),
                  result.diagnostic.diagnostic_code,
                  EvidencePairs(result.evidence));

  snapshot.cluster_metric_route_requested = true;
  const auto cluster = impl::EvaluateNodeResourceAgentSnapshot(snapshot);
  Require(!cluster.ok() &&
              cluster.diagnostic.diagnostic_code ==
                  "SB_AGENT_CLUSTER_PROVIDER_REQUIRED",
          "node resource agent accepted core cluster metric route");
}

void TestMetricsRegistryManager(agents::DurableAgentCatalogImage* catalog) {
  const std::filesystem::path history_path =
      std::filesystem::temp_directory_path() /
      "scratchbird_agent_enterprise_metrics_registry.history";
  std::filesystem::remove(history_path);
  metrics::MetricRegistry registry;
  impl::MetricsRegistryManagerSample sample;
  sample.metric_family = "sb_metric_samples_rejected_total";
  sample.namespace_path = "sys.metrics.registry";
  sample.sample_count = 10;
  impl::MetricsRegistryManagerActionRequest action;
  action.sample = sample;
  action.registry = &registry;
  action.history_path = history_path.string();
  action.observation_time_microseconds = 120000000;
  action.labels = {{"metric_family", "sb_memory_allocated_bytes"},
                   {"reason", "aeic020_accept"}};
  auto accepted = impl::ApplyMetricsRegistryManagerAction(action);
  Require(accepted.ok() && accepted.sample_accepted &&
              accepted.registry_mutation_written &&
              accepted.history_sample_written,
          "metrics registry did not accept and persist trusted sample");
  const auto history_after_accept =
      metrics::LoadMetricHistoryStore(history_path.string());
  Require(!history_after_accept.raw_samples.empty(),
          "metrics registry accept handler did not write raw history");
  PersistDecision(catalog,
                  "metrics_registry_manager",
                  "metrics_registry.accept_sample",
                  impl::MetricsRegistryManagerDecisionKindName(accepted.decision),
                  accepted.diagnostic.diagnostic_code,
                  EvidencePairs(accepted.evidence));

  sample.schema_compatible = false;
  action.sample = sample;
  auto rejected = impl::ApplyMetricsRegistryManagerAction(action);
  Require(rejected.ok() && rejected.sample_rejected &&
              rejected.registry_mutation_written,
          "metrics registry did not reject and record bad sample");

  sample.schema_compatible = true;
  sample.sidecar_authority = true;
  action.sample = sample;
  auto untrusted = impl::ApplyMetricsRegistryManagerAction(action);
  Require(!untrusted.ok() &&
              untrusted.diagnostic.diagnostic_code ==
                  "SB_AGENT_METRICS_REGISTRY_AUTHORITY_UNTRUSTED",
          "metrics registry accepted sidecar authority");

  sample.sidecar_authority = false;
  sample.rollup_backlog = 20000;
  action.sample = sample;
  action.rollup_grain = metrics::MetricRollupGrain::one_minute;
  auto rollup = impl::ApplyMetricsRegistryManagerAction(action);
  Require(rollup.ok() && rollup.rollup_requested && rollup.rollup_written &&
              rollup.rollup_rows_created > 0,
          "metrics registry did not generate persistent rollup rows");

  sample.rollup_backlog = 0;
  sample.metric_family = "sb_export_adapter_queue_depth";
  sample.namespace_path = "sys.metrics.export";
  sample.export_queue_depth = 9000;
  action.sample = sample;
  action.labels = {{"component", "core.metrics.export"},
                   {"operation", "shed_export"}};
  auto shed = impl::ApplyMetricsRegistryManagerAction(action);
  Require(shed.ok() && shed.export_shed_requested &&
              shed.export_shed_written &&
              shed.export_queue_depth_after_shed <=
                  action.policy.export_queue_depth_threshold,
          "metrics registry did not execute export shed handler");

  sample.export_queue_depth = 0;
  sample.metric_family = "sb_cluster_node_role_state";
  sample.namespace_path = "cluster.sys.metrics.node";
  sample.cluster_metric_route_requested = true;
  action.sample = sample;
  auto cluster = impl::ApplyMetricsRegistryManagerAction(action);
  Require(!cluster.ok() &&
              cluster.diagnostic.diagnostic_code ==
                  "SB_AGENT_CLUSTER_PROVIDER_REQUIRED",
          "metrics registry manager accepted core cluster metric mutation");
  std::filesystem::remove(history_path);
}

void TestMemoryGovernor(agents::DurableAgentCatalogImage* catalog) {
  impl::MemoryGovernorPolicy policy;
  policy.hard_limit_bytes = 1024;
  policy.soft_limit_bytes = 768;
  policy.cache_shrink_floor_bytes = 128;
  impl::MemoryGovernorSnapshot snapshot;
  snapshot.current_bytes = 700;
  snapshot.requested_grant_bytes = 200;
  snapshot.spillable_bytes = 300;
  snapshot.cache_bytes = 256;
  snapshot.memory_metrics_authoritative = true;
  snapshot.resource_reservation_authoritative = true;
  snapshot.grant_is_spillable = true;
  auto spill = impl::EvaluateMemoryGovernorGrant(snapshot, policy);
  Require(spill.ok() && spill.spill_required && spill.bytes_to_spill > 0,
          "memory governor did not force spill above soft limit");
  PersistDecision(catalog,
                  "memory_governor",
                  "memory_governor.evaluate_grant",
                  impl::MemoryGovernorDecisionKindName(spill.decision),
                  spill.diagnostic.diagnostic_code,
                  EvidencePairs(spill.evidence));

  snapshot.requested_grant_bytes = 400;
  snapshot.grant_is_spillable = false;
  auto deny = impl::EvaluateMemoryGovernorGrant(snapshot, policy);
  Require(deny.ok() &&
              deny.decision == impl::MemoryGovernorDecisionKind::deny_large_grant,
          "memory governor did not deny grant above hard limit");
}

void TestAdmissionControlManager(agents::DurableAgentCatalogImage* catalog) {
  impl::AdmissionControlPolicy policy;
  policy.min_emergency_reserve_bytes = 1024;
  impl::AdmissionControlSnapshot snapshot;
  snapshot.emergency_reserve_bytes = 512;
  snapshot.pressure_metrics_authoritative = true;
  snapshot.resource_ledger_authoritative = true;
  snapshot.foreground_database_work_active = true;
  auto denied = impl::EvaluateAdmissionControlRequest(snapshot, policy);
  Require(denied.ok() && denied.denied && denied.foreground_protected,
          "admission control did not deny below emergency reserve");
  PersistDecision(catalog,
                  "admission_control_manager",
                  "admission_control.evaluate_request",
                  impl::AdmissionControlDecisionKindName(denied.decision),
                  denied.diagnostic.diagnostic_code,
                  EvidencePairs(denied.evidence));

  snapshot.emergency_reserve_bytes = 4096;
  snapshot.listener_queue_depth = 2048;
  auto throttled = impl::EvaluateAdmissionControlRequest(snapshot, policy);
  Require(throttled.ok() && throttled.throttled,
          "admission control did not throttle listener pressure");
}

void TestAlertManager(agents::DurableAgentCatalogImage* catalog) {
  impl::AlertManagerRequest request;
  request.alert_key = "filespace-health";
  request.now_microseconds = 1000000;
  request.condition_active = true;
  request.trusted_evidence_present = true;
  auto fired = impl::EvaluateAlertManagerRequest(request);
  Require(fired.ok() && fired.alert_fired,
          "alert manager did not fire trusted active alert");
  PersistDecision(catalog,
                  "alert_manager",
                  "alert_manager.evaluate_alert",
                  impl::AlertManagerDecisionKindName(fired.decision),
                  fired.diagnostic.diagnostic_code,
                  EvidencePairs(fired.evidence));

  request.last_fired_microseconds = 900000;
  auto deduped = impl::EvaluateAlertManagerRequest(request);
  Require(deduped.ok() && deduped.deduped,
          "alert manager did not dedupe repeated alert");

  request.silence_requested = true;
  request.requested_silence_microseconds = 60000000;
  auto silenced = impl::EvaluateAlertManagerRequest(request);
  Require(silenced.ok() && silenced.alert_silenced &&
              silenced.silence_until_microseconds > request.now_microseconds,
          "alert manager did not apply bounded silence");
}

void TestProductionClassificationNoLongerAnchorOnly() {
  const auto by_agent = ExposureByAgent();
  for (const std::string agent : {
           "node_resource_agent",
           "metrics_registry_manager",
           "memory_governor",
           "admission_control_manager",
           "alert_manager"}) {
    const auto found = by_agent.find(agent);
    Require(found != by_agent.end(), "missing exposure record: " + agent);
    Require(!found->second.implementation_anchor_only,
            "enterprise local handler still classified anchor-only: " + agent);
    Require(!found->second.route_evidence_kind.empty(),
            "enterprise local handler lacks route evidence classification: " +
                agent);
    Require(!found->second.production_live_route_available,
            "enterprise local handler exposed live mutation by default: " + agent);
  }
}

}  // namespace

int main() {
  auto catalog = DurableCatalog();
  TestNodeResourceAgent(&catalog);
  TestMetricsRegistryManager(&catalog);
  TestMemoryGovernor(&catalog);
  TestAdmissionControlManager(&catalog);
  TestAlertManager(&catalog);
  Require(catalog.evidence.size() == 5, "local resource evidence count mismatch");
  Require(catalog.actions.size() == 5, "local resource action count mismatch");
  Require(catalog.health.size() == 5, "local resource health count mismatch");
  Require(catalog.retained_history.size() == 5,
          "local resource history count mismatch");
  Require(agents::ValidateDurableAgentCatalogForProduction(catalog).ok,
          "local resource durable catalog invalid after evidence writes");
  TestProductionClassificationNoLongerAnchorOnly();
  return EXIT_SUCCESS;
}
