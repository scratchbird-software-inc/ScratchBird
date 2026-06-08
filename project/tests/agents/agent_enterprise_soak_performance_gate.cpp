// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_durable_catalog.hpp"
#include "agent_enterprise_evidence.hpp"
#include "agent_metric_runtime.hpp"
#include "agent_runtime.hpp"

// SEARCH_KEY: AEIC_AGENT_SOAK_PERFORMANCE_TESTS

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

agents::u64 EnvU64(const char* name, agents::u64 fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') { return fallback; }
  try {
    return static_cast<agents::u64>(std::stoull(value));
  } catch (...) {
    return fallback;
  }
}

std::string ObjectId(const std::string& key) {
  return agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic043|" + key);
}

std::string PrincipalId(const std::string& key) {
  return agents::DeterministicAgentRuntimePrincipalUuidFromKey("aeic043|" + key);
}

std::vector<agents::AgentTypeDescriptor> NonClusterDescriptors() {
  std::vector<agents::AgentTypeDescriptor> descriptors;
  for (const auto& descriptor : agents::CanonicalAgentRegistry()) {
    if (descriptor.cluster_only ||
        descriptor.deployment == agents::AgentDeployment::cluster) {
      continue;
    }
    descriptors.push_back(descriptor);
  }
  Require(!descriptors.empty(), "AEIC-043 found no non-cluster agents");
  return descriptors;
}

agents::AgentRuntimeContext RuntimeContext(agents::u64 now_microseconds = 1) {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.database_uuid = ObjectId("database");
  context.principal_uuid = PrincipalId("operator");
  context.rights = {"OBS_AGENT_STATE_READ",
                    "OBS_AGENT_CONTROL",
                    "OBS_AGENT_EVIDENCE_READ",
                    "OBS_METRICS_READ_FAMILY"};
  context.wall_now_microseconds = now_microseconds == 0 ? 1 : now_microseconds;
  return context;
}

std::vector<agents::AgentObservedMetricSnapshot> ObservedSnapshotsFor(
    const agents::AgentTypeDescriptor& descriptor,
    const std::string& scope_uuid,
    agents::u64 generation,
    agents::u64 now_microseconds) {
  std::vector<agents::AgentObservedMetricSnapshot> snapshots;
  for (const auto& dependency : descriptor.metric_dependencies) {
    if (dependency.cluster_only) { continue; }
    agents::AgentObservedMetricSnapshot snapshot;
    snapshot.metric_family = dependency.metric_family;
    snapshot.namespace_path = dependency.namespace_prefix.empty()
                                  ? dependency.metric_family
                                  : dependency.namespace_prefix + ".observed";
    snapshot.generation = generation;
    snapshot.observed_wall_microseconds = now_microseconds;
    snapshot.scope_uuid = scope_uuid;
    snapshot.digest = "sha256:aeic043:" + descriptor.type_id + ":" +
                      dependency.metric_family;
    snapshot.source_quality = agents::AgentMetricSourceQuality::trusted;
    snapshot.present = true;
    snapshot.trusted = true;
    snapshot.schema_compatible = true;
    snapshot.trust_provenance = "aeic043_enterprise_soak_metric_registry";
    snapshot.evidence_uuid =
        ObjectId("metric-evidence|" + descriptor.type_id + "|" +
                 dependency.metric_family);
    snapshot.snapshot_id = "aeic043:" + descriptor.type_id + ":" +
                           dependency.metric_family;
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

double Percentile(std::vector<double> samples, double percentile) {
  Require(!samples.empty(), "AEIC-043 percentile called with no samples");
  std::sort(samples.begin(), samples.end());
  const double rank = (percentile / 100.0) *
                      static_cast<double>(samples.size() - 1);
  const auto index = static_cast<std::size_t>(rank + 0.5);
  return samples[std::min(index, samples.size() - 1)];
}

agents::DurableAgentCatalogImage DurableCatalog() {
  agents::DurableAgentCatalogImage image;
  image.source = agents::AgentCatalogStateSource::durable_catalog_image;
  image.schema_version = 1;
  image.authority.durable_catalog_authority = true;
  image.authority.mga_transaction_evidence = true;
  image.authority.mga_transaction_uuid = ObjectId("mga-transaction");
  image.authority.transaction_generation = 43;
  image.authority.evidence_uuid = ObjectId("catalog-open");
  image.authority.database_uuid = ObjectId("database");
  image.authority.catalog_storage_uuid = ObjectId("catalog-storage");
  image.authority.storage_commit_evidence_uuid = ObjectId("storage-commit");
  image.authority.catalog_generation = 1;
  image.authority.local_transaction_id = 4300;
  image.authority.storage_catalog_record_evidence = true;
  image.authority.transaction_inventory_bound = true;
  image.authority.fsync_or_checkpoint_evidence = true;

  for (const auto& descriptor : NonClusterDescriptors()) {
    agents::AgentInstanceRecord instance;
    instance.instance_uuid = ObjectId("instance|" + descriptor.type_id);
    instance.agent_type_id = descriptor.type_id;
    instance.policy_uuid = ObjectId("policy|" + descriptor.type_id);
    instance.scope = descriptor.scope.empty() ? "database" : descriptor.scope;
    instance.state = agents::AgentLifecycleState::running;
    instance.policy_generation = 43;
    instance.instance_generation = 1;
    image.instances.push_back(std::move(instance));
  }
  const auto refreshed =
      agents::RefreshDurableAgentCatalogAuthorityDigest(
          &image, image.authority.evidence_uuid);
  Require(refreshed.ok, "AEIC-043 catalog root digest failed");
  Require(agents::ValidateDurableAgentCatalogForProduction(image).ok,
          "AEIC-043 durable catalog fixture is not production-valid");
  return image;
}

void ValidateStrictMetricSweep(
    const std::vector<agents::AgentTypeDescriptor>& descriptors) {
  std::size_t accepted = 0;
  for (std::size_t i = 0; i < descriptors.size(); ++i) {
    const auto& descriptor = descriptors[i];
    auto context = RuntimeContext(4300000 + i);
    const auto availability =
        agents::EvaluateAgentFeatureAvailability(descriptor, context);
    Require(availability == agents::AgentFeatureAvailability::available,
            "AEIC-043 non-cluster descriptor unavailable: " +
                descriptor.type_id);
    agents::AgentMetricSnapshotEvaluationOptions options;
    options.mode = agents::AgentMetricRuntimeMode::production_strict;
    options.expected_scope_uuid = context.database_uuid;
    const auto evaluation = agents::EvaluateAgentObservedMetricSnapshots(
        descriptor,
        context,
        ObservedSnapshotsFor(descriptor,
                             context.database_uuid,
                             4300 + i,
                             context.wall_now_microseconds),
        options);
    Require(evaluation.accepted,
            "AEIC-043 strict metric sweep refused " + descriptor.type_id +
                ": " + evaluation.status.diagnostic_code);
    Require(!evaluation.input_digest.empty(),
            "AEIC-043 strict metric sweep produced empty digest");
    ++accepted;
  }
  Require(accepted == descriptors.size(),
          "AEIC-043 strict metric sweep did not cover every local agent");
}

struct SoakStats {
  std::vector<double> samples_us;
  std::atomic<agents::u64> success_count{0};
  std::atomic<agents::u64> refusal_count{0};
  std::atomic<agents::u64> failure_count{0};
};

void RunConcurrentReservationSoak(
    const std::vector<agents::AgentTypeDescriptor>& descriptors,
    SoakStats* stats) {
  agents::AgentResourceReservationLimits limits;
  limits.max_memory_bytes = 128 * 1024;
  limits.max_worker_slots = 4;
  limits.foreground_reserved_worker_slots = 1;
  limits.max_overhead_microseconds = 100000;
  agents::AgentResourceReservationLedger ledger("aeic043-resource", limits);

  const auto threads = static_cast<int>(
      EnvU64("SCRATCHBIRD_AEIC_AGENT_SOAK_THREADS", 8));
  const auto iterations = static_cast<int>(
      EnvU64("SCRATCHBIRD_AEIC_AGENT_SOAK_ITERATIONS", 160));
  std::mutex samples_mutex;
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(threads));

  for (int thread_index = 0; thread_index < threads; ++thread_index) {
    workers.emplace_back([&, thread_index]() {
      for (int iteration = 0; iteration < iterations; ++iteration) {
        const std::size_t descriptor_index =
            static_cast<std::size_t>(thread_index * iterations + iteration) %
            descriptors.size();
        const auto& descriptor = descriptors[descriptor_index];
        auto policy = agents::BaselinePolicyForAgent(descriptor);
        auto context = RuntimeContext(4400000 + thread_index * iterations +
                                      iteration);

        agents::AgentResourceReservationRequest request;
        request.reservation_key = "aeic043|" + descriptor.type_id + "|" +
                                  std::to_string(thread_index) + "|" +
                                  std::to_string(iteration);
        request.owner_scope = "worker:" + std::to_string(thread_index);
        request.memory_bytes = (iteration % 23 == 0)
                                   ? limits.max_memory_bytes + 1
                                   : 4096;
        request.worker_slots = 1;
        request.overhead_microseconds = 100;
        request.foreground_database_work_active = (iteration % 29 == 0);
        const auto start = std::chrono::steady_clock::now();
        const auto acquired = ledger.Acquire(descriptor, policy, context, request);
        if (!acquired.ok) {
          if (acquired.failed_closed) {
            ++stats->refusal_count;
          } else {
            ++stats->failure_count;
          }
          const auto elapsed =
              std::chrono::duration<double, std::micro>(
                  std::chrono::steady_clock::now() - start)
                  .count();
          std::lock_guard<std::mutex> guard(samples_mutex);
          stats->samples_us.push_back(std::max(1.0, elapsed));
          continue;
        }

        std::this_thread::sleep_for(std::chrono::microseconds(50));
        const auto released = ledger.Release(acquired.reservation.token_id);
        const auto elapsed =
            std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - start)
                .count();
        {
          std::lock_guard<std::mutex> guard(samples_mutex);
          stats->samples_us.push_back(std::max(1.0, elapsed));
        }
        if (!released.ok) {
          ++stats->failure_count;
          continue;
        }
        ++stats->success_count;
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  const auto snapshot = ledger.Snapshot();
  Require(snapshot.active_reservation_count == 0,
          "AEIC-043 reservation soak leaked active reservations");
  Require(stats->failure_count.load() == 0,
          "AEIC-043 reservation soak had non-fail-closed failures");
  Require(stats->success_count.load() > 0,
          "AEIC-043 reservation soak had no accepted reservations");
  Require(stats->refusal_count.load() > 0,
          "AEIC-043 reservation soak did not exercise fail-closed pressure");
  Require(stats->samples_us.size() ==
              static_cast<std::size_t>(threads * iterations),
          "AEIC-043 reservation soak sample count mismatch");
}

void RunEnterpriseEvidenceOverhead(
    agents::DurableAgentCatalogImage* catalog,
    const std::vector<agents::AgentTypeDescriptor>& descriptors,
    std::vector<double>* samples_us) {
  const auto writes = EnvU64("SCRATCHBIRD_AEIC_AGENT_EVIDENCE_WRITES", 64);
  const std::size_t limit =
      std::min<std::size_t>(static_cast<std::size_t>(writes),
                            descriptors.size() * 4);
  for (std::size_t i = 0; i < limit; ++i) {
    const auto& descriptor = descriptors[i % descriptors.size()];
    const auto scope_uuid = ObjectId("evidence-scope|" + descriptor.type_id);
    agents::AgentEnterpriseDecisionEvidenceRequest request;
    request.catalog = catalog;
    request.agent_type_id = descriptor.type_id;
    request.instance_uuid = ObjectId("instance|" + descriptor.type_id);
    request.operation_id = "aeic043.enterprise_soak_decision";
    request.principal_uuid = PrincipalId("enterprise-evidence");
    request.rights_used = {"OBS_AGENT_CONTROL", "OBS_AGENT_EVIDENCE_READ"};
    request.scope_uuids = {scope_uuid};
    request.policy_generation = 43;
    request.decision_kind = "soak_telemetry_sample";
    request.result_state = "completed";
    request.diagnostic_code = "SB_AGENT_AEIC043.SOAK_SAMPLE";
    request.decision_fields = {{"sample_ordinal", std::to_string(i)},
                               {"agent_type", descriptor.type_id}};
    request.outcome_verification_evidence_uuid =
        ObjectId("outcome|" + descriptor.type_id + "|" + std::to_string(i));
    request.created_at_microseconds = 4500000 + i;
    request.metric_context = RuntimeContext(request.created_at_microseconds);
    request.metric_context.database_uuid = scope_uuid;
    request.metric_snapshot_options.expected_scope_uuid = scope_uuid;
    request.observed_metric_snapshots =
        ObservedSnapshotsFor(descriptor,
                             scope_uuid,
                             4500 + i,
                             request.created_at_microseconds);
    const auto start = std::chrono::steady_clock::now();
    const auto persisted =
        agents::AppendEnterpriseAgentDecisionEvidence(request);
    const auto elapsed =
        std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start)
            .count();
    samples_us->push_back(std::max(1.0, elapsed));
    Require(persisted.status.ok,
            "AEIC-043 enterprise evidence write failed for " +
                descriptor.type_id + ": " + persisted.status.diagnostic_code);
    Require(persisted.evidence_written && persisted.action_written &&
                persisted.history_written && persisted.catalog_root_refreshed,
            "AEIC-043 enterprise evidence write was incomplete");
  }
  Require(catalog->evidence.size() == limit &&
              catalog->actions.size() == limit &&
              catalog->retained_history.size() == limit,
          "AEIC-043 evidence/action/history cardinality mismatch");
}

struct BacklogSummary {
  agents::u64 pending_or_running = 0;
  agents::u64 replay_pending = 0;
  agents::u64 quarantined = 0;
  agents::u64 queue_depth = 0;
};

BacklogSummary SummarizeBacklog(
    const agents::DurableAgentCatalogImage& catalog) {
  BacklogSummary summary;
  summary.queue_depth = static_cast<agents::u64>(catalog.leases.size());
  for (const auto& action : catalog.actions) {
    if (action.state == agents::DurableAgentActionState::pending ||
        action.state == agents::DurableAgentActionState::running ||
        action.state == agents::DurableAgentActionState::replay_pending) {
      ++summary.pending_or_running;
      ++summary.queue_depth;
    }
    if (action.state == agents::DurableAgentActionState::replay_pending) {
      ++summary.replay_pending;
    }
    if (action.state == agents::DurableAgentActionState::quarantined) {
      ++summary.quarantined;
    }
  }
  return summary;
}

void ValidateBacklogTelemetry(agents::DurableAgentCatalogImage* catalog) {
  for (int i = 0; i < 48; ++i) {
    agents::DurableAgentActionRecord action;
    action.action_uuid = ObjectId("backlog-action|" + std::to_string(i));
    action.instance_uuid = catalog->instances[i % catalog->instances.size()].instance_uuid;
    action.owner_uuid = PrincipalId("backlog-owner");
    action.operation_id = "aeic043.backlog";
    action.idempotency_key = "aeic043-backlog-" + std::to_string(i);
    action.evidence_uuid = ObjectId("backlog-evidence|" + std::to_string(i));
    action.diagnostic_code = "SB_AGENT_AEIC043.BACKLOG";
    if (i < 16) {
      action.state = agents::DurableAgentActionState::pending;
    } else if (i < 32) {
      action.state = agents::DurableAgentActionState::running;
    } else if (i < 40) {
      action.state = agents::DurableAgentActionState::replay_pending;
    } else if (i < 44) {
      action.state = agents::DurableAgentActionState::quarantined;
    } else {
      action.state = agents::DurableAgentActionState::completed;
    }
    catalog->actions.push_back(std::move(action));
  }
  for (int i = 0; i < 3; ++i) {
    agents::DurableAgentLeaseRecord lease;
    lease.lease_uuid = ObjectId("backlog-lease|" + std::to_string(i));
    lease.instance_uuid = catalog->instances[i % catalog->instances.size()].instance_uuid;
    lease.owner_uuid = PrincipalId("lease-owner");
    lease.state = agents::DurableAgentLeaseState::acquired;
    lease.evidence_uuid = ObjectId("backlog-lease-evidence|" + std::to_string(i));
    catalog->leases.push_back(std::move(lease));
  }
  const auto refreshed =
      agents::RefreshDurableAgentCatalogAuthorityDigest(
          catalog, ObjectId("backlog-refresh"));
  Require(refreshed.ok, "AEIC-043 backlog catalog refresh failed");
  const auto summary = SummarizeBacklog(*catalog);
  Require(summary.pending_or_running == 40,
          "AEIC-043 backlog pending/running count mismatch");
  Require(summary.replay_pending == 8,
          "AEIC-043 replay backlog count mismatch");
  Require(summary.quarantined == 4,
          "AEIC-043 quarantine backlog count mismatch");
  Require(summary.queue_depth == 43,
          "AEIC-043 queue depth telemetry mismatch");
}

}  // namespace

int main() {
  const auto registry_status = agents::ValidateCanonicalAgentRegistry();
  Require(registry_status.ok,
          "AEIC-043 canonical registry invalid: " +
              registry_status.diagnostic_code);
  const auto descriptors = NonClusterDescriptors();
  ValidateStrictMetricSweep(descriptors);

  SoakStats soak;
  RunConcurrentReservationSoak(descriptors, &soak);

  auto catalog = DurableCatalog();
  std::vector<double> evidence_samples;
  RunEnterpriseEvidenceOverhead(&catalog, descriptors, &evidence_samples);
  ValidateBacklogTelemetry(&catalog);

  const double reservation_p50 = Percentile(soak.samples_us, 50.0);
  const double reservation_p95 = Percentile(soak.samples_us, 95.0);
  const double reservation_p99 = Percentile(soak.samples_us, 99.0);
  const double evidence_p95 = Percentile(evidence_samples, 95.0);
  const double evidence_p99 = Percentile(evidence_samples, 99.0);
  Require(reservation_p50 > 0.0 && reservation_p50 <= reservation_p95 &&
              reservation_p95 <= reservation_p99,
          "AEIC-043 reservation percentile ordering invalid");
  Require(evidence_p95 > 0.0 && evidence_p95 <= evidence_p99,
          "AEIC-043 evidence percentile ordering invalid");
  Require(reservation_p99 <=
              static_cast<double>(
                  EnvU64("SCRATCHBIRD_AEIC_AGENT_RESERVATION_P99_BUDGET_US",
                         250000)),
          "AEIC-043 reservation p99 exceeded enterprise gate budget");
  Require(evidence_p99 <=
              static_cast<double>(
                  EnvU64("SCRATCHBIRD_AEIC_AGENT_EVIDENCE_P99_BUDGET_US",
                         250000)),
          "AEIC-043 evidence p99 exceeded enterprise gate budget");

  std::cout << "AEIC_AGENT_SOAK_PERFORMANCE_TESTS "
            << "noncluster_agents=" << descriptors.size()
            << " reservation_success=" << soak.success_count.load()
            << " reservation_refusal=" << soak.refusal_count.load()
            << " reservation_p50_us=" << reservation_p50
            << " reservation_p95_us=" << reservation_p95
            << " reservation_p99_us=" << reservation_p99
            << " evidence_writes=" << evidence_samples.size()
            << " evidence_p95_us=" << evidence_p95
            << " evidence_p99_us=" << evidence_p99 << '\n';
  return EXIT_SUCCESS;
}
