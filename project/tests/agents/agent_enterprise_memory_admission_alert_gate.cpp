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
#include "agent_durable_catalog.hpp"
#include "agent_enterprise_evidence.hpp"
#include "agent_memory_coupling.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace impl = scratchbird::core::agents::implemented_agents;

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
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic023-mga");
  image.authority.transaction_generation = 23;
  image.authority.evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic023-open");
  image.authority.database_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic023-db");
  image.authority.catalog_storage_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic023-storage");
  image.authority.storage_commit_evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic023-commit");
  image.authority.catalog_generation = 1;
  image.authority.local_transaction_id = 2300;
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
    snapshot.generation = 2300;
    snapshot.observed_wall_microseconds = observed_wall_microseconds;
    snapshot.scope_uuid = scope_uuid;
    snapshot.digest = "sha256:aeic023:" + dependency.metric_family;
    snapshot.source_quality = agents::AgentMetricSourceQuality::trusted;
    snapshot.present = true;
    snapshot.trusted = true;
    snapshot.schema_compatible = true;
    snapshot.trust_provenance = "enterprise_memory_admission_alert_gate";
    snapshot.evidence_uuid =
        agents::DeterministicAgentRuntimeObjectUuidFromKey(
            "aeic023-metric-evidence|" + dependency.metric_family);
    snapshot.snapshot_id = "aeic023:" + dependency.metric_family;
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
      agents::DeterministicAgentRuntimeObjectUuidFromKey(agent_type_id + "-aeic023");
  request.operation_id = operation_id;
  request.principal_uuid =
      agents::DeterministicAgentRuntimePrincipalUuidFromKey("aeic023-principal");
  request.rights_used = {"agent.execute", "agent.observe"};
  request.scope_uuids = {
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic023-scope")};
  request.policy_generation = 23;
  request.decision_kind = decision_kind;
  request.result_state = "completed";
  request.diagnostic_code = diagnostic_code;
  request.decision_fields = fields;
  request.outcome_verification_evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(agent_type_id + "-aeic023-verification");
  request.created_at_microseconds = before_generation + 230000;
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
          "AEIC-023 decision evidence was not fully durable");
  Require(catalog->authority.catalog_generation > before_generation,
          "AEIC-023 durable catalog generation did not advance");
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

void RegisterMemoryScopes(agents::HierarchicalMemoryBudgetLedger* ledger) {
  agents::HierarchicalMemoryBudgetScope database;
  database.scope_id = "database:aeic023";
  database.kind = agents::HierarchicalMemoryBudgetScopeKind::kDatabase;
  database.limit_bytes = 4ull * 1024ull * 1024ull;
  Require(ledger->RegisterScope(database).ok,
          "AEIC-023 database memory scope registration failed");

  agents::HierarchicalMemoryBudgetScope background;
  background.scope_id = "background:aeic023";
  background.parent_scope_id = database.scope_id;
  background.kind = agents::HierarchicalMemoryBudgetScopeKind::kBackground;
  background.limit_bytes = 2ull * 1024ull * 1024ull;
  Require(ledger->RegisterScope(background).ok,
          "AEIC-023 background memory scope registration failed");
}

agents::AgentMemoryReservationRequest ReservationRequest(
    const std::string& operation_id,
    agents::u64 bytes) {
  agents::AgentMemoryReservationRequest request;
  request.agent_type_id = "memory_governor";
  request.action_id = "memory_governor.evaluate_grant";
  request.operation_id = operation_id;
  request.owner_scope = "agent:memory_governor";
  request.leaf_memory_scope_id = "background:aeic023";
  request.memory_bytes = bytes;
  request.live_action = true;
  request.allocation_or_memory_action = true;
  return request;
}

void TestStrictMetricsDurableReservationsAndRedaction() {
  agents::AgentMemoryMetricSnapshot snapshot;
  snapshot.metric_family = "sb_memory_allocated_bytes";
  snapshot.namespace_path = "sys.metrics.memory";
  snapshot.generation = 2301;
  snapshot.sampled_at_microseconds = 1000000;
  snapshot.scope_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic023-scope");
  snapshot.digest = "sha256:aeic023-memory-snapshot";
  snapshot.max_freshness_microseconds = 1000000;
  snapshot.source_quality = agents::AgentMetricSourceQuality::trusted;
  snapshot.trusted = true;
  const auto metric = agents::ValidateAgentMemoryMetricSnapshot(
      snapshot, 1500000, false);
  Require(metric.ok, "AEIC-023 trusted memory metric snapshot refused");

  snapshot.trusted = false;
  const auto untrusted = agents::ValidateAgentMemoryMetricSnapshot(
      snapshot, 1500000, false);
  Require(!untrusted.ok && untrusted.fail_closed,
          "AEIC-023 untrusted memory metric snapshot accepted");

  agents::HierarchicalMemoryBudgetLedger memory_ledger("aeic023-memory");
  RegisterMemoryScopes(&memory_ledger);
  agents::ResourceGovernanceReservationLedger resource_ledger("aeic023-resource");
  const auto admitted = agents::AcquireAgentMemoryReservations(
      ReservationRequest("aeic023-reservation", 4096),
      &memory_ledger,
      &resource_ledger);
  Require(admitted.ok && admitted.hierarchical_reservation_created &&
              admitted.resource_reservation_created,
          "AEIC-023 durable memory/resource reservation not created");
  Require(memory_ledger.Release(admitted.hierarchical_reservation.token_id).ok,
          "AEIC-023 hierarchical reservation release failed");
  Require(resource_ledger.Release(admitted.resource_reservation.token_id).ok,
          "AEIC-023 resource reservation release failed");

  const auto refused = agents::AcquireAgentMemoryReservations(
      ReservationRequest("aeic023-unbounded", 0),
      &memory_ledger,
      &resource_ledger);
  Require(!refused.ok && refused.fail_closed,
          "AEIC-023 unbounded reservation accepted");

  agents::AgentMemoryPressureActionBoundaryRequest boundary;
  boundary.agent_type_id = "memory_governor";
  boundary.action_id = "shrink_cache";
  boundary.action_kind =
      agents::AgentMemoryPressureActionKind::page_cache_shrink;
  boundary.action_boundary_approved = true;
  boundary.durable_evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic023-action-evidence");
  boundary.outcome_verification_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic023-action-outcome");
  boundary.outcome_verified = true;
  const auto allowed =
      agents::EvaluateAgentMemoryPressureActionBoundary(boundary);
  Require(allowed.ok && allowed.action_allowed,
          "AEIC-023 approved memory pressure action refused");
  boundary.sidecar_only_evidence = true;
  const auto sidecar =
      agents::EvaluateAgentMemoryPressureActionBoundary(boundary);
  Require(!sidecar.ok && sidecar.fail_closed,
          "AEIC-023 sidecar-only memory pressure action accepted");

  agents::AgentMemoryEvidenceBundleRequest bundle_request;
  bundle_request.agent_type_id = "support_bundle_triage_agent";
  bundle_request.support_bundle_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic023-support-bundle");
  bundle_request.overhead_budget_bytes = 2048;
  bundle_request.fields.push_back({"safe_metric", "memory_bytes=4096", false});
  bundle_request.fields.push_back({"protected_buffer", "secret", true});
  const auto bundle = agents::BuildAgentMemoryEvidenceBundle(bundle_request);
  Require(bundle.ok && bundle.overhead_budget_evidence_present,
          "AEIC-023 support bundle memory evidence refused");
  bool redacted = false;
  bool tamper = false;
  for (const auto& row : bundle.rows) {
    redacted = redacted ||
               (row.key == "protected_buffer" && row.redacted &&
                row.value == "redacted");
    tamper = tamper || !row.tamper_evidence_digest.empty();
  }
  Require(redacted, "AEIC-023 protected memory evidence was not redacted");
  Require(tamper, "AEIC-023 memory evidence lacks tamper metadata");
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
  const auto spill = impl::EvaluateMemoryGovernorGrant(snapshot, policy);
  Require(spill.ok() && spill.spill_required && spill.bytes_to_spill > 0,
          "AEIC-023 memory governor did not force spill");
  PersistDecision(catalog,
                  "memory_governor",
                  "memory_governor.evaluate_grant",
                  impl::MemoryGovernorDecisionKindName(spill.decision),
                  spill.diagnostic.diagnostic_code,
                  EvidencePairs(spill.evidence));

  snapshot.grant_is_spillable = false;
  const auto shrink = impl::EvaluateMemoryGovernorGrant(snapshot, policy);
  Require(shrink.ok() && shrink.cache_shrink_requested &&
              shrink.bytes_to_shrink > 0,
          "AEIC-023 memory governor did not request cache shrink");

  snapshot.current_bytes = 900;
  snapshot.requested_grant_bytes = 200;
  snapshot.cache_bytes = 0;
  const auto deny = impl::EvaluateMemoryGovernorGrant(snapshot, policy);
  Require(deny.ok() &&
              deny.decision == impl::MemoryGovernorDecisionKind::deny_large_grant,
          "AEIC-023 memory governor did not deny hard-limit grant");

  snapshot.current_bytes = 200;
  snapshot.requested_grant_bytes = 100;
  const auto allow = impl::EvaluateMemoryGovernorGrant(snapshot, policy);
  Require(allow.ok() && allow.grant_allowed,
          "AEIC-023 memory governor refused safe grant");

  snapshot.memory_metrics_authoritative = false;
  const auto untrusted = impl::EvaluateMemoryGovernorGrant(snapshot, policy);
  Require(!untrusted.ok() && untrusted.fail_closed &&
              untrusted.diagnostic.diagnostic_code ==
                  "SB_AGENT_MEMORY_GOVERNOR_AUTHORITY_UNTRUSTED",
          "AEIC-023 memory governor accepted untrusted authority");
}

void TestAdmissionControl(agents::DurableAgentCatalogImage* catalog) {
  impl::AdmissionControlPolicy policy;
  policy.min_emergency_reserve_bytes = 1024;
  policy.throttle_listener_queue_depth = 100;
  policy.deny_scheduler_queue_depth = 1000;
  policy.downgrade_slo_burn_rate_per_mille = 1500;

  impl::AdmissionControlSnapshot snapshot;
  snapshot.pressure_metrics_authoritative = true;
  snapshot.resource_ledger_authoritative = true;
  snapshot.foreground_database_work_active = true;
  snapshot.emergency_reserve_bytes = 512;
  const auto denied = impl::EvaluateAdmissionControlRequest(snapshot, policy);
  Require(denied.ok() && denied.denied && denied.foreground_protected,
          "AEIC-023 admission control did not deny emergency pressure");
  PersistDecision(catalog,
                  "admission_control_manager",
                  "admission_control.evaluate_request",
                  impl::AdmissionControlDecisionKindName(denied.decision),
                  denied.diagnostic.diagnostic_code,
                  EvidencePairs(denied.evidence));

  snapshot.emergency_reserve_bytes = 4096;
  snapshot.listener_queue_depth = 128;
  const auto throttle = impl::EvaluateAdmissionControlRequest(snapshot, policy);
  Require(throttle.ok() && throttle.throttled,
          "AEIC-023 admission control did not throttle listener pressure");

  snapshot.listener_queue_depth = 0;
  snapshot.slo_burn_rate_per_mille = 2000;
  const auto downgrade = impl::EvaluateAdmissionControlRequest(snapshot, policy);
  Require(downgrade.ok() && downgrade.downgraded,
          "AEIC-023 admission control did not downgrade SLO pressure");

  snapshot.slo_burn_rate_per_mille = 0;
  const auto allowed = impl::EvaluateAdmissionControlRequest(snapshot, policy);
  Require(allowed.ok() && allowed.request_allowed && !allowed.denied,
          "AEIC-023 admission control refused safe request");

  snapshot.resource_ledger_authoritative = false;
  const auto untrusted = impl::EvaluateAdmissionControlRequest(snapshot, policy);
  Require(!untrusted.ok() && untrusted.fail_closed &&
              untrusted.diagnostic.diagnostic_code ==
                  "SB_AGENT_ADMISSION_AUTHORITY_UNTRUSTED",
          "AEIC-023 admission control accepted untrusted authority");
}

void TestAlertManager(agents::DurableAgentCatalogImage* catalog) {
  impl::AlertManagerRequest request;
  request.alert_key = "memory-pressure";
  request.now_microseconds = 1000000;
  request.condition_active = true;
  request.trusted_evidence_present = true;
  const auto fired = impl::EvaluateAlertManagerRequest(request);
  Require(fired.ok() && fired.alert_fired,
          "AEIC-023 alert manager did not fire trusted alert");
  PersistDecision(catalog,
                  "alert_manager",
                  "alert_manager.evaluate_alert",
                  impl::AlertManagerDecisionKindName(fired.decision),
                  fired.diagnostic.diagnostic_code,
                  EvidencePairs(fired.evidence));

  request.last_fired_microseconds = 900000;
  const auto deduped = impl::EvaluateAlertManagerRequest(request);
  Require(deduped.ok() && deduped.deduped,
          "AEIC-023 alert manager did not dedupe alert");

  request.last_fired_microseconds = 0;
  request.condition_active = false;
  const auto inactive = impl::EvaluateAlertManagerRequest(request);
  Require(inactive.ok() &&
              inactive.decision == impl::AlertManagerDecisionKind::no_action,
          "AEIC-023 alert manager acted on inactive condition");

  request.clear_condition = true;
  const auto cleared = impl::EvaluateAlertManagerRequest(request);
  Require(cleared.ok() && cleared.alert_cleared,
          "AEIC-023 alert manager did not clear alert");

  request.clear_condition = false;
  request.silence_requested = true;
  request.requested_silence_microseconds = 60000000;
  const auto silenced = impl::EvaluateAlertManagerRequest(request);
  Require(silenced.ok() && silenced.alert_silenced,
          "AEIC-023 alert manager did not silence alert");

  request.requested_silence_microseconds = 0;
  const auto bad_silence = impl::EvaluateAlertManagerRequest(request);
  Require(!bad_silence.ok() && bad_silence.fail_closed,
          "AEIC-023 alert manager accepted invalid silence");

  request.silence_requested = false;
  request.trusted_evidence_present = false;
  const auto untrusted = impl::EvaluateAlertManagerRequest(request);
  Require(!untrusted.ok() && untrusted.fail_closed &&
              untrusted.diagnostic.diagnostic_code ==
                  "SB_AGENT_ALERT_AUTHORITY_UNTRUSTED",
          "AEIC-023 alert manager accepted untrusted alert evidence");
}

void RunConcurrentPressureReservations() {
  agents::HierarchicalMemoryBudgetLedger memory_ledger(
      "aeic023-concurrent-memory");
  RegisterMemoryScopes(&memory_ledger);
  agents::ResourceGovernanceReservationLedger resource_ledger(
      "aeic023-concurrent-resource");

  constexpr int kThreads = 8;
  constexpr int kIterations = 64;
  std::atomic<int> success_count{0};
  std::atomic<int> refused_count{0};
  std::atomic<int> failure_count{0};
  std::vector<std::thread> workers;
  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    workers.emplace_back([thread_index, &memory_ledger, &resource_ledger,
                          &success_count, &refused_count, &failure_count]() {
      for (int iteration = 0; iteration < kIterations; ++iteration) {
        const auto operation_id = "aeic023-pressure-" +
                                  std::to_string(thread_index) + "-" +
                                  std::to_string(iteration);
        const auto admitted = agents::AcquireAgentMemoryReservations(
            ReservationRequest(operation_id, 1024),
            &memory_ledger,
            &resource_ledger);
        if (!admitted.ok) {
          if (admitted.fail_closed) {
            ++refused_count;
          } else {
            ++failure_count;
          }
          continue;
        }
        const auto memory_release =
            memory_ledger.Release(admitted.hierarchical_reservation.token_id);
        const auto resource_release =
            resource_ledger.Release(admitted.resource_reservation.token_id);
        if (!memory_release.ok || !resource_release.ok) {
          ++failure_count;
          continue;
        }
        ++success_count;

        impl::MemoryGovernorPolicy policy;
        policy.hard_limit_bytes = 8192;
        policy.soft_limit_bytes = 4096;
        policy.cache_shrink_floor_bytes = 512;
        impl::MemoryGovernorSnapshot snapshot;
        snapshot.current_bytes = 3000;
        snapshot.requested_grant_bytes =
            static_cast<agents::u64>(thread_index + iteration + 512);
        snapshot.spillable_bytes = 2048;
        snapshot.cache_bytes = 1024;
        snapshot.memory_metrics_authoritative = true;
        snapshot.resource_reservation_authoritative = true;
        snapshot.grant_is_spillable = (iteration % 2) == 0;
        const auto pressure = impl::EvaluateMemoryGovernorGrant(snapshot, policy);
        if (!pressure.ok()) {
          ++failure_count;
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  Require(failure_count.load() == 0,
          "AEIC-023 concurrent memory pressure path had non-deterministic failures");
  Require(success_count.load() > 0,
          "AEIC-023 concurrent memory pressure had no successful reservations");
  Require(refused_count.load() > 0,
          "AEIC-023 concurrent memory pressure did not exercise fail-closed pressure refusals");
  Require(success_count.load() + refused_count.load() == kThreads * kIterations,
          "AEIC-023 concurrent memory pressure did not account for every iteration");
  Require(resource_ledger.Snapshot().active_reservation_count == 0,
          "AEIC-023 resource ledger leaked concurrent reservations");
  bool memory_leak = false;
  for (const auto& snapshot : memory_ledger.Snapshot()) {
    memory_leak = memory_leak || snapshot.current_bytes != 0 ||
                  snapshot.active_reservation_count != 0;
  }
  Require(!memory_leak, "AEIC-023 memory ledger leaked concurrent reservations");
}

}  // namespace

int main() {
  auto catalog = DurableCatalog();
  TestStrictMetricsDurableReservationsAndRedaction();
  TestMemoryGovernor(&catalog);
  TestAdmissionControl(&catalog);
  TestAlertManager(&catalog);
  RunConcurrentPressureReservations();
  Require(catalog.evidence.size() == 3,
          "AEIC-023 decision evidence count mismatch");
  Require(catalog.actions.size() == 3,
          "AEIC-023 action count mismatch");
  Require(catalog.health.size() == 3,
          "AEIC-023 health count mismatch");
  Require(catalog.retained_history.size() == 3,
          "AEIC-023 retained history count mismatch");
  Require(agents::ValidateDurableAgentCatalogForProduction(catalog).ok,
          "AEIC-023 durable catalog invalid after evidence writes");
  std::cout
      << "AEIC023_MEMORY_GOVERNOR_ADMISSION_ALERT "
      << "AEIC023_STRICT_METRICS_DURABLE_RESERVATIONS "
      << "AEIC023_SUPPORT_BUNDLE_REDACTION "
      << "AEIC023_HIGH_CONCURRENCY_PRESSURE ok\n";
  return EXIT_SUCCESS;
}
