// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_binary_identity_fixture.hpp"
using scratchbird::tests::BinaryFixtureIdentity;
using scratchbird::tests::NativeFixtureIdentity;
using scratchbird::tests::FixtureIdentityForLabel;
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
#include "../support/metric_projection_fixture.hpp"
#include "../support/owned_temp_directory.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <limits>
#include <stdexcept>
#include <thread>
#include <set>
#include <string>

namespace {

namespace agents = scratchbird::core::agents;
namespace impl = scratchbird::core::agents::implemented_agents;
namespace metrics = scratchbird::core::metrics;

[[noreturn]] void Fail(const std::string& message) {
  throw std::runtime_error(message);
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
      FixtureIdentityForLabel("aeic-local-mga");
  image.authority.transaction_generation = 9;
  image.authority.evidence_uuid =
      FixtureIdentityForLabel("aeic-local-open");
  image.authority.database_uuid =
      FixtureIdentityForLabel("aeic-local-db");
  image.authority.catalog_storage_uuid =
      FixtureIdentityForLabel("aeic-local-storage");
  image.authority.storage_commit_evidence_uuid =
      FixtureIdentityForLabel("aeic-local-commit");
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
    snapshot.scope_uuid = NativeFixtureIdentity(scope_uuid);
    snapshot.digest = "sha256:aeic-local-resource:" + dependency.metric_family;
    snapshot.source_quality = agents::AgentMetricSourceQuality::trusted;
    snapshot.present = true;
    snapshot.trusted = true;
    snapshot.schema_compatible = true;
    snapshot.trust_provenance = "test_metric_registry";
    snapshot.evidence_uuid =
        NativeFixtureIdentity(FixtureIdentityForLabel(
            "aeic-local-resource-metric-evidence|" + dependency.metric_family));
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
    source_a.evidence_uuid = NativeFixtureIdentity(FixtureIdentityForLabel(BinaryFixtureIdentity(snapshot.evidence_uuid) + ":source-a"));
    source_a.snapshot_id += ":source-a";
    snapshots.push_back(std::move(source_a));

    auto source_b = snapshot;
    source_b.source_id = "source-b";
    source_b.source_sequence = snapshot.generation * 2 + 2;
    source_b.previous_source_sequence = source_b.source_sequence - 1;
    source_b.attestation_key_id = "metric-key:" + source_b.source_id;
    source_b.attestation_digest = "attestation:" + source_b.metric_family +
                                  ":" + source_b.source_id;
    source_b.evidence_uuid = NativeFixtureIdentity(FixtureIdentityForLabel(BinaryFixtureIdentity(snapshot.evidence_uuid) + ":source-b"));
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
      FixtureIdentityForLabel(agent_type_id + "-instance");
  request.operation_id = operation_id;
  request.principal_uuid =
      FixtureIdentityForLabel("aeic-local-principal");
  request.rights_used = {"agent.execute", "agent.observe"};
  request.scope_uuids = {
      FixtureIdentityForLabel("aeic-local-scope")};
  request.policy_generation = 11;
  request.decision_kind = decision_kind;
  request.result_state = "completed";
  request.diagnostic_code = diagnostic_code;
  request.decision_fields = fields;
  request.outcome_verification_evidence_uuid =
      FixtureIdentityForLabel(agent_type_id + "-verification");
  request.created_at_microseconds = before_generation + 100;
  request.metric_context.database_uuid = NativeFixtureIdentity(request.scope_uuids.front());
  request.metric_context.principal_uuid = NativeFixtureIdentity(request.principal_uuid);
  request.metric_context.security_context_present = true;
  request.metric_context.wall_now_microseconds = request.created_at_microseconds;
  request.metric_snapshot_options.expected_scope_uuid = NativeFixtureIdentity(request.scope_uuids.front());
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
  scratchbird::tests::OwnedTempDirectory directory;
  const auto history_path = directory.path() / "metrics.history";
  const auto database = scratchbird::tests::FixtureUuid(233, 1);
  const auto node = scratchbird::tests::FixtureUuid(233, 2);
  scratchbird::tests::MetricProjectionFixture fixture(database, node, 233);
  metrics::MetricRetentionPolicyDefinition retention;
  retention.policy_name = "manager component history";
  retention.mode = metrics::MetricRetentionMode::raw_and_rollup;
  retention.raw_retention_seconds = 86400;
  retention.rollup_retention_seconds = 604800;
  retention.rollup_grains = {metrics::MetricRollupGrain::one_minute};
  fixture.ConfigureRetention(retention);
  auto& registry = metrics::DefaultMetricRegistry();
  impl::MetricsRegistryManagerSample sample;
  sample.metric_family = "sb_metric_samples_rejected_total";
  sample.namespace_path = "sys.metrics.registry";
  sample.sample_count = 10;
  impl::MetricsRegistryManagerActionRequest action;
  action.sample = sample;
  action.registry = &registry;
  action.database_uuid = database;
  action.node_uuid = node;
  action.actor_uuid = scratchbird::tests::FixtureUuid(233, 3);
  action.transaction_uuid = scratchbird::tests::FixtureUuid(233, 4);
  action.history_path = history_path.string();
  action.observation_time_microseconds = 120000000;
  action.labels = {{"metric_family", "sb_memory_allocated_bytes"},
                   {"reason", "aeic020_accept"}};
  fixture.Admit(sample.metric_family, action.labels);
  fixture.Admit("sb_metric_samples_rejected_total",
                {{"metric_family", sample.metric_family},
                 {"reason", "SB_AGENT_METRICS_REGISTRY_SAMPLE_REJECTED"}});
  const metrics::MetricLabelSet shed_gauge_labels = {
      {"component", "core.metrics.export"}, {"operation", "shed_export"},
      {"metric_family", "sb_export_adapter_queue_depth"}};
  const metrics::MetricLabelSet shed_counter_labels = {
      {"metric_family", "sb_export_adapter_queue_depth"}, {"reason", "queue_pressure"}};
  fixture.Admit("sb_export_adapter_queue_depth", shed_gauge_labels);
  fixture.Admit("sb_metric_export_shed_total", shed_counter_labels);
  const auto descriptor = *registry.FindDescriptor(sample.metric_family);
  const auto& series = fixture.RetainedSeries(sample.metric_family, action.labels);
  metrics::MetricRetentionPolicy policy;
  static_cast<metrics::MetricRetentionPolicyDefinition&>(policy) = retention;
  policy.policy_uuid = series.retention_policy_uuid;
  policy.generation = series.retention_policy_generation;
  metrics::MetricHistoryStore seed;
  seed.policies.push_back(policy);
  Require(metrics::WriteMetricHistoryStore(history_path.string(), seed).ok, "history policy seed failed");
  Require(metrics::RegisterMetricHistorySeries(history_path.string(), descriptor, series, policy).ok,
          "history series admission failed");
  auto accepted = impl::ApplyMetricsRegistryManagerAction(action);
  Require(accepted.ok() && accepted.sample_accepted &&
              accepted.registry_mutation_written &&
              accepted.history_sample_written,
          "metrics registry did not accept and persist trusted sample: " + accepted.diagnostic.diagnostic_code);
  fixture.ExpectProduced(1);
  const auto history_after_accept =
      metrics::LoadMetricHistoryStore(history_path.string());
  Require(history_after_accept.load_status.ok && history_after_accept.raw_samples.size() == 1 &&
              std::get<agents::u64>(history_after_accept.raw_samples.front().value.value) == 10,
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
  fixture.ExpectProduced(1);

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
          "metrics registry did not generate persistent rollup rows: " + rollup.diagnostic.diagnostic_code);
  const auto reopened = metrics::LoadMetricHistoryStore(history_path.string());
  Require(reopened.load_status.ok && reopened.rollups.size() == 1 &&
              reopened.rollups.front().sum_value == 10,
          "rollup did not preserve the stored measurement");

  sample.rollup_backlog = 0;
  sample.metric_family = "sb_export_adapter_queue_depth";
  sample.namespace_path = "sys.metrics.export";
  sample.export_queue_depth = 9000;
  action.sample = sample;
  action.labels = {{"component", "core.metrics.export"},
                   {"operation", "shed_export"}};
  const auto no_buffer = impl::ApplyMetricsRegistryManagerAction(action);
  Require(!no_buffer.ok() && !no_buffer.export_shed_written && !no_buffer.registry_mutation_written &&
              no_buffer.diagnostic.diagnostic_code == "SB_AGENT_METRICS_REGISTRY_EXPORT_BUFFER_REQUIRED",
          "export counters substituted for a real buffer");
  impl::MetricExportBuffer buffer(database, node, 9000, 16 * 1024 * 1024);
  const auto original = history_after_accept.raw_samples.front();
  for (agents::u64 index = 0; index < 9000; ++index) {
    auto copy = original;
    copy.sample_uuid = scratchbird::tests::FixtureUuid(234, index + 1);
    copy.source_sequence = index + 1;
    Require(buffer.Enqueue(descriptor, series, copy), "export copy admission failed");
  }
  const auto bytes_per_sample = buffer.Bytes() / 9000;
  Require(bytes_per_sample > 0 && buffer.Depth() == 9000, "export buffer accounting invalid");
  action.export_buffer = &buffer;
  auto shed = impl::ApplyMetricsRegistryManagerAction(action);
  Require(shed.ok() && shed.export_shed_requested &&
              shed.export_shed_written &&
              shed.export_queue_depth_after_shed <=
                  action.policy.export_queue_depth_threshold,
          "metrics registry did not execute export shed handler");
  fixture.ExpectProduced(2);
  Require(shed.export_samples_removed.size() == 4000 && buffer.Depth() == 5000 &&
              buffer.Bytes() == 5000 * bytes_per_sample &&
              shed.export_samples_removed.front() == scratchbird::tests::FixtureUuid(234, 1) &&
              shed.export_samples_removed.back() == scratchbird::tests::FixtureUuid(234, 4000),
          "export shedding did not remove and receipt exactly the excess copies");
  const auto taken = buffer.Take();
  Require(taken && taken->sample_uuid == scratchbird::tests::FixtureUuid(234, 4001) &&
              metrics::DecodeMetricRawSample(descriptor, series, taken->bytes).ok() &&
              buffer.Bytes() == 4999 * bytes_per_sample,
          "export take lost native framing or byte accounting");
  Require(metrics::LoadMetricHistoryStore(history_path.string()).raw_samples.size() == 1,
          "optional export shedding removed retained raw observations");

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

  // Independent optional copies are bounded by both bytes and rows. Neither
  // duplicate identities nor a foreign owner's sample can consume capacity.
  impl::MetricExportBuffer one(database, node, 1, bytes_per_sample);
  Require(one.Enqueue(descriptor, series, original) &&
              !one.Enqueue(descriptor, series, original), "duplicate export copy accepted");
  auto another = original;
  another.sample_uuid = scratchbird::tests::FixtureUuid(234, 10000);
  Require(!one.Enqueue(descriptor, series, another), "full export buffer accepted a copy");
  impl::MetricExportBuffer too_small(database, node, 2, bytes_per_sample - 1);
  Require(!too_small.Enqueue(descriptor, series, original) && too_small.Bytes() == 0,
          "export buffer exceeded byte bound");
  impl::MetricExportBuffer foreign(database, scratchbird::tests::FixtureUuid(233, 99), 2, 4096);
  Require(!foreign.Enqueue(descriptor, series, original) && foreign.Depth() == 0,
          "foreign owner accepted into export buffer");
  Require(one.Take().has_value() && !one.Take().has_value() && one.Bytes() == 0,
          "draining export buffer leaked bytes");
  Require(one.ShedTo(0, 0).removed.empty(), "empty buffer manufactured removed identities");
  impl::MetricExportBuffer concurrent(database, node, 128, 128 * bytes_per_sample);
  for (unsigned index = 0; index < 128; ++index) {
    auto copy = original;
    copy.sample_uuid = scratchbird::tests::FixtureUuid(235, index + 1);
    Require(concurrent.Enqueue(descriptor, series, copy), "concurrent shed setup failed");
  }
  std::array<impl::MetricExportBuffer::ShedResult, 4> partitions;
  std::vector<std::jthread> workers;
  for (unsigned index = 0; index < 4; ++index)
    workers.emplace_back([&, index] { partitions[index] = concurrent.ShedTo(0, 32); });
  workers.clear();  // All bounded operations finish before receipts are read.
  std::set<metrics::MetricUuid> removed_ids;
  for (const auto& partition : partitions) {
    Require(partition.ok && partition.removed.size() == 32, "concurrent shed ignored action cap");
    for (const auto& identity : partition.removed)
      Require(removed_ids.insert(identity).second, "concurrent shed receipted a duplicate removal");
  }
  Require(removed_ids.size() == 128 && concurrent.Depth() == 0 && concurrent.Bytes() == 0,
          "concurrent shed leaked copies or accounting");

  // The gauge is admitted but its corresponding shed counter intentionally
  // is not: a telemetry refusal must retain the already performed removals.
  action.sample = {};
  action.sample.metric_family = descriptor.family;
  action.sample.namespace_path = descriptor.namespace_path;
  action.policy.export_queue_depth_threshold = 4998;
  const metrics::MetricLabelSet partial_gauge_labels = {
      {"component", "core.metrics.export"}, {"operation", "shed_export"},
      {"metric_family", descriptor.family}};
  fixture.Admit("sb_export_adapter_queue_depth", partial_gauge_labels);
  const auto partial = impl::ApplyMetricsRegistryManagerAction(action);
  Require(!partial.ok() && partial.export_shed_written && partial.registry_mutation_written &&
              partial.export_samples_removed.size() == 1 && buffer.Depth() == 4998 &&
              partial.export_queue_depth_after_shed == 4998,
          "failed telemetry erased physical shed or preceding gauge effects");
  fixture.ExpectProduced(1);
  Require(std::any_of(partial.evidence.begin(), partial.evidence.end(), [](const auto& field) {
    return field.key == "failed_closed" && field.value == "true";
  }), "partial failure receipt contradicted its failure status");

  action.export_buffer = nullptr;
  action.policy = {};
  action.labels = series.labels;
  action.sample.sample_count = 0;
  // Zero is a real counter delta, never a sentinel selecting numeric_value.
  action.sample.numeric_value = agents::u64{123};
  auto zero = impl::ApplyMetricsRegistryManagerAction(action);
  Require(zero.ok() && zero.history_sample_written, "zero counter delta rejected");
  fixture.ExpectProduced(1);
  auto exact_history = metrics::LoadMetricHistoryStore(history_path.string());
  Require(std::get<agents::u64>(exact_history.raw_samples.back().value.value) == 10,
          "zero counter delta substituted a different value");
  action.sample.sample_count = (agents::u64{1} << 53) + 1;
  const auto exact = impl::ApplyMetricsRegistryManagerAction(action);
  Require(exact.ok() && exact.history_sample_written, "native counter delta above 2^53 rejected");
  fixture.ExpectProduced(1);
  exact_history = metrics::LoadMetricHistoryStore(history_path.string());
  Require(std::get<agents::u64>(exact_history.raw_samples.back().value.value) ==
              (agents::u64{1} << 53) + 11, "native cumulative counter rounded through double");
  const auto current_value = [&]() -> agents::u64 {
    const auto key = metrics::MakeMetricSeriesKey(descriptor.family, action.labels);
    for (const auto& value : registry.SnapshotCurrent(false))
      if (metrics::MakeMetricSeriesKey(value.family, value.labels) == key)
        return std::get<agents::u64>(value.value);
    Fail("current native sample missing");
  };
  const auto before = current_value();
  action.sample.sample_count = 1;
  action.history_path.clear();
  auto missing = impl::ApplyMetricsRegistryManagerAction(action);
  Require(!missing.ok() && !missing.registry_mutation_written && current_value() == before,
          "missing required history mutated current state");
  action.history_path = history_path.string();
  action.node_uuid = scratchbird::tests::FixtureUuid(233, 99);
  auto owner = impl::ApplyMetricsRegistryManagerAction(action);
  Require(!owner.ok() && !owner.registry_mutation_written && current_value() == before,
          "foreign owner mutated current state");
  action.node_uuid = node;
  action.sample.namespace_path = "sys.metrics.invalid";
  auto wrong_namespace = impl::ApplyMetricsRegistryManagerAction(action);
  Require(!wrong_namespace.ok() && !wrong_namespace.registry_mutation_written,
          "mismatched descriptor namespace accepted");
  action.sample.namespace_path = descriptor.namespace_path;

  // A valid current-only store must not be reported as a raw history write.
  auto current_only = exact_history;
  current_only.policies.front().mode = metrics::MetricRetentionMode::current_only;
  current_only.policies.front().raw_retention_seconds = 0;
  current_only.policies.front().rollup_retention_seconds = 0;
  current_only.policies.front().rollup_grains.clear();
  const auto no_raw_path = (directory.path() / "current-only.history").string();
  Require(metrics::WriteMetricHistoryStore(no_raw_path, current_only).ok, "current-only seed failed");
  action.history_path = no_raw_path;
  auto no_raw = impl::ApplyMetricsRegistryManagerAction(action);
  Require(!no_raw.ok() && !no_raw.registry_mutation_written && !no_raw.history_sample_written &&
              no_raw.diagnostic.diagnostic_code == "SB_AGENT_METRICS_REGISTRY_RAW_HISTORY_POLICY_REQUIRED" &&
              current_value() == before, "no-op history append misreported success");
  action.history_path = history_path.string();
  action.observation_time_microseconds = std::numeric_limits<agents::u64>::max();
  const auto late_failure = impl::ApplyMetricsRegistryManagerAction(action);
  Require(!late_failure.ok() && late_failure.registry_mutation_written && !late_failure.history_sample_written &&
              current_value() == before + 1 &&
              metrics::LoadMetricHistoryStore(history_path.string()).raw_samples.size() == 3,
          "history failure erased the preceding actual registry effect");
  fixture.ExpectProduced(1);
  action.observation_time_microseconds = 240000000;

  action.labels = {{"metric_family", "sb_memory_allocated_bytes"}, {"reason", "native_max"}};
  fixture.Admit(descriptor.family, action.labels);
  Require(metrics::RegisterMetricHistorySeries(history_path.string(), descriptor,
              fixture.RetainedSeries(descriptor.family, action.labels), policy).ok,
          "maximum counter series admission failed");
  action.sample.sample_count = std::numeric_limits<agents::u64>::max();
  const auto maximum = impl::ApplyMetricsRegistryManagerAction(action);
  Require(maximum.ok() && maximum.history_sample_written && current_value() == action.sample.sample_count &&
              std::get<agents::u64>(metrics::LoadMetricHistoryStore(history_path.string()).raw_samples.back().value.value)
                  == action.sample.sample_count,
          "UINT64_MAX did not survive publication and native history reopen");
  fixture.ExpectProduced(1);
  action.sample.sample_count = 1;
  const auto overflow = impl::ApplyMetricsRegistryManagerAction(action);
  Require(!overflow.ok() && !overflow.registry_mutation_written && !overflow.history_sample_written &&
              current_value() == std::numeric_limits<agents::u64>::max(), "overflow changed the exact counter");

  action.sample.metric_family = "sb_export_adapter_queue_depth";
  action.sample.namespace_path = "sys.metrics.export";
  action.labels = shed_gauge_labels;
  action.history_path.clear();
  action.durable_history_required = false;
  action.sample.numeric_value = 1.0;
  const auto wrong_type = impl::ApplyMetricsRegistryManagerAction(action);
  Require(!wrong_type.ok() && !wrong_type.registry_mutation_written, "wrong native gauge scalar accepted");
  fixture.Seal();
  fixture.VerifyAndDrain();
  directory.Cleanup();
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

int main() try {
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
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return EXIT_FAILURE;
}
