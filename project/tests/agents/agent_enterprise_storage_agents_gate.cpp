// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/filespace_capacity_manager.hpp"
#include "agents/page_allocation_manager.hpp"
#include "agents/storage_health_manager.hpp"
#include "agent_durable_catalog.hpp"
#include "agent_enterprise_evidence.hpp"
#include "page_allocation_lifecycle.hpp"
#include "page_filespace_handoff.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace agent = scratchbird::core::agents;
namespace impl = scratchbird::core::agents::implemented_agents;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

platform::TypedUuid MakeUuid(platform::UuidKind kind, platform::u64 seed) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, 1900000000000ull + seed);
  return generated.ok() ? generated.value : platform::TypedUuid{};
}

struct FixtureIds {
  platform::TypedUuid database_uuid;
  platform::TypedUuid filespace_uuid;
  platform::TypedUuid policy_uuid;
  platform::TypedUuid transaction_uuid;
  platform::TypedUuid evidence_uuid;
};

FixtureIds MakeIds(platform::u64 seed) {
  return {MakeUuid(platform::UuidKind::database, 100 + seed),
          MakeUuid(platform::UuidKind::filespace, 200 + seed),
          MakeUuid(platform::UuidKind::object, 300 + seed),
          MakeUuid(platform::UuidKind::transaction, 400 + seed),
          MakeUuid(platform::UuidKind::object, 500 + seed)};
}

std::string UuidString(const platform::TypedUuid& value) {
  return uuid::UuidToString(value.value);
}

agent::DurableAgentCatalogImage DurableCatalog(const FixtureIds& ids) {
  agent::DurableAgentCatalogImage image;
  image.source = agent::AgentCatalogStateSource::durable_catalog_image;
  image.schema_version = 1;
  image.authority.durable_catalog_authority = true;
  image.authority.mga_transaction_evidence = true;
  image.authority.mga_transaction_uuid = UuidString(ids.transaction_uuid);
  image.authority.transaction_generation = 21;
  image.authority.evidence_uuid = UuidString(ids.evidence_uuid);
  image.authority.database_uuid = UuidString(ids.database_uuid);
  image.authority.catalog_storage_uuid =
      agent::DeterministicAgentRuntimeObjectUuidFromKey("aeic-storage-catalog");
  image.authority.storage_commit_evidence_uuid =
      agent::DeterministicAgentRuntimeObjectUuidFromKey("aeic-storage-commit");
  image.authority.catalog_generation = 1;
  image.authority.local_transaction_id = 84;
  image.authority.storage_catalog_record_evidence = true;
  image.authority.transaction_inventory_bound = true;
  image.authority.fsync_or_checkpoint_evidence = true;
  const auto refreshed =
      agent::RefreshDurableAgentCatalogAuthorityDigest(&image,
                                                       image.authority.evidence_uuid);
  Require(refreshed.ok, refreshed.diagnostic_code);
  return image;
}

std::vector<agent::AgentObservedMetricSnapshot> ObservedSnapshotsFor(
    const std::string& agent_type_id,
    const std::string& scope_uuid,
    agent::u64 observed_wall_microseconds) {
  const auto descriptor = agent::FindAgentType(agent_type_id);
  Require(descriptor.has_value(), "agent descriptor missing: " + agent_type_id);
  std::vector<agent::AgentObservedMetricSnapshot> snapshots;
  for (const auto& dependency : descriptor->metric_dependencies) {
    agent::AgentObservedMetricSnapshot snapshot;
    snapshot.metric_family = dependency.metric_family;
    snapshot.namespace_path = dependency.namespace_prefix.empty()
                                  ? dependency.metric_family
                                  : dependency.namespace_prefix + ".observed";
    snapshot.generation = 11;
    snapshot.observed_wall_microseconds = observed_wall_microseconds;
    snapshot.scope_uuid = scope_uuid;
    snapshot.digest = "sha256:aeic-storage:" + dependency.metric_family;
    snapshot.source_quality = agent::AgentMetricSourceQuality::trusted;
    snapshot.present = true;
    snapshot.trusted = true;
    snapshot.schema_compatible = true;
    snapshot.trust_provenance = "storage_agent_gate";
    snapshot.evidence_uuid =
        agent::DeterministicAgentRuntimeObjectUuidFromKey(
            "aeic-storage-metric|" + dependency.metric_family);
    snapshot.snapshot_id = "aeic-storage:" + dependency.metric_family;
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

void PersistDecision(agent::DurableAgentCatalogImage* catalog,
                     const std::string& agent_type_id,
                     const std::string& operation_id,
                     const std::string& decision_kind,
                     const std::string& diagnostic_code,
                     std::vector<std::pair<std::string, std::string>> fields) {
  const auto before_generation = catalog->authority.catalog_generation;
  const std::string scope_uuid = catalog->authority.database_uuid;
  agent::AgentEnterpriseDecisionEvidenceRequest request;
  request.catalog = catalog;
  request.agent_type_id = agent_type_id;
  request.instance_uuid =
      agent::DeterministicAgentRuntimeObjectUuidFromKey(agent_type_id + "-storage");
  request.operation_id = operation_id;
  request.principal_uuid =
      agent::DeterministicAgentRuntimePrincipalUuidFromKey("aeic-storage-principal");
  request.rights_used = {"agent.execute", "agent.observe"};
  request.scope_uuids = {scope_uuid};
  request.policy_generation = 21;
  request.decision_kind = decision_kind;
  request.result_state = "completed";
  request.diagnostic_code = diagnostic_code;
  request.decision_fields = std::move(fields);
  request.outcome_verification_evidence_uuid =
      agent::DeterministicAgentRuntimeObjectUuidFromKey(agent_type_id + "-storage-verify");
  request.created_at_microseconds = before_generation + 200;
  request.metric_context.database_uuid = scope_uuid;
  request.metric_context.principal_uuid = request.principal_uuid;
  request.metric_context.security_context_present = true;
  request.metric_context.wall_now_microseconds = request.created_at_microseconds;
  request.metric_snapshot_options.expected_scope_uuid = scope_uuid;
  request.observed_metric_snapshots =
      ObservedSnapshotsFor(agent_type_id, scope_uuid, request.created_at_microseconds);
  const auto persisted = agent::AppendEnterpriseAgentDecisionEvidence(request);
  Require(persisted.status.ok, persisted.status.diagnostic_code);
  Require(persisted.evidence_written && persisted.action_written &&
              persisted.history_written && persisted.catalog_root_refreshed,
          agent_type_id + " enterprise evidence did not persist");
  Require(catalog->authority.catalog_generation > before_generation,
          agent_type_id + " catalog generation did not advance");
}

impl::PageAllocationManagerPolicy PagePolicy(const FixtureIds& ids) {
  auto policy = impl::DefaultPageAllocationManagerPolicy();
  policy.database_uuid = ids.database_uuid;
  policy.filespace_uuid = ids.filespace_uuid;
  policy.policy_uuid = ids.policy_uuid;
  policy.live_preallocation_allowed = true;
  policy.live_preallocation_policy_explicit = true;
  policy.capacity_request_allowed = false;
  policy.capacity_request_policy_explicit = false;
  policy.capacity_evidence_required = true;
  return policy;
}

impl::PageAllocationManagerMetricSnapshot PageSnapshot(const FixtureIds& ids) {
  impl::PageAllocationManagerMetricSnapshot snapshot;
  snapshot.database_uuid = ids.database_uuid;
  snapshot.filespace_uuid = ids.filespace_uuid;
  snapshot.policy_uuid = ids.policy_uuid;
  snapshot.page_family = "data";
  snapshot.free_pages = 12;
  snapshot.released_pages = 2;
  snapshot.reserved_pages = 2;
  snapshot.preallocated_pages = 1;
  snapshot.allocated_pages = 128;
  snapshot.preallocation_target_pages = 7;
  snapshot.preallocation_deficit_pages = 6;
  return snapshot;
}

impl::PageAllocationManagerActionContext PageContext(const FixtureIds& ids) {
  impl::PageAllocationManagerActionContext context;
  context.present = true;
  context.engine_authoritative = true;
  context.transaction_uuid = ids.transaction_uuid;
  context.local_transaction_id = 91;
  context.page_generation = 3;
  context.durability_fence_satisfied = true;
  context.capacity_evidence_present = true;
  context.capacity_evidence_fresh = true;
  context.capacity_evidence_scope_compatible = true;
  context.capacity_evidence_uuid = ids.evidence_uuid;
  context.capacity_evidence_free_pages = 16;
  return context;
}

page::PageAllocationLedger PageLedger(const FixtureIds& ids) {
  page::PageAllocationLedger ledger;
  ledger.database_uuid = ids.database_uuid;
  ledger.filespace_uuid = ids.filespace_uuid;
  ledger.free_extents.push_back({500, 16});
  return ledger;
}

void TestPageAllocation(agent::DurableAgentCatalogImage* catalog,
                        const FixtureIds& ids) {
  auto ledger = PageLedger(ids);
  page::PageFilespaceAgentRequestQueue queue;
  const auto result = impl::EvaluatePageAllocationManagerTick(
      &queue, &ledger, PageSnapshot(ids), PagePolicy(ids), PageContext(ids));
  Require(result.ok(), "page allocation failed: " + result.diagnostic.diagnostic_code);
  Require(result.direct_action_attempted && result.ledger_state_changed &&
              result.accepted_evidence && ledger.allocations.size() == 1 &&
              ledger.evidence.size() == 1,
          "page allocation did not mutate ledger with durable evidence");
  const auto recovery = page::ClassifyPageAllocationLedgerForRecovery(ledger);
  Require(recovery.ok() && !recovery.classifications.empty(),
          "page allocation recovery classification failed");
  PersistDecision(
      catalog,
      "page_allocation_manager",
      "page_allocation.preallocate_page_family",
      impl::PageAllocationManagerDecisionKindName(result.decision),
      result.diagnostic.diagnostic_code,
      {{"requested_pages", std::to_string(result.requested_pages)},
       {"preallocated_pages", std::to_string(result.preallocated_pages)},
       {"ledger_state_changed", result.ledger_state_changed ? "true" : "false"},
       {"recovery_classifications", std::to_string(recovery.classifications.size())}});
}

page::PageFilespaceAgentRequest MakeFilespaceRequest(const FixtureIds& ids) {
  page::PageFilespaceAgentRequest request;
  request.request_uuid = MakeUuid(platform::UuidKind::object, 10000);
  request.database_uuid = ids.database_uuid;
  request.filespace_uuid = ids.filespace_uuid;
  request.policy_uuid = ids.policy_uuid;
  request.kind = page::PageFilespaceAgentRequestKind::extend_filespace;
  request.state = page::PageFilespaceAgentRequestState::created;
  request.requesting_agent = "page_allocation_manager";
  request.responding_agent = "filespace_capacity_manager";
  request.page_family = "data";
  request.requested_pages = 6;
  request.released_free_pages = 2;
  request.target_reserve_pages = 8;
  request.threshold_pages = 4;
  request.free_pages = 2;
  request.allocated_pages = 128;
  request.reason = "aeic storage capacity request";
  return request;
}

impl::FilespaceCapacityManagerMetricSnapshot FilespaceSnapshot(
    const FixtureIds& ids) {
  impl::FilespaceCapacityManagerMetricSnapshot snapshot;
  snapshot.database_uuid = ids.database_uuid;
  snapshot.filespace_uuid = ids.filespace_uuid;
  snapshot.policy_uuid = ids.policy_uuid;
  snapshot.total_pages = 256;
  snapshot.used_pages = 192;
  snapshot.free_pages = 4;
  snapshot.reserved_pages = 2;
  snapshot.available_capacity_window_pages = 8;
  snapshot.health_state = impl::FilespaceCapacityHealthState::healthy;
  snapshot.role_state = impl::FilespaceCapacityRoleState::active_primary;
  return snapshot;
}

impl::FilespaceCapacityManagerPolicy FilespacePolicy(const FixtureIds& ids) {
  auto policy = impl::DefaultFilespaceCapacityManagerPolicy();
  policy.database_uuid = ids.database_uuid;
  policy.filespace_uuid = ids.filespace_uuid;
  policy.policy_uuid = ids.policy_uuid;
  policy.capacity_window_allowed = true;
  policy.capacity_processing_policy_explicit = true;
  policy.expand_allowed = true;
  policy.expand_request_policy_explicit = true;
  policy.max_capacity_window_pages = 8;
  return policy;
}

void TestFilespaceCapacity(agent::DurableAgentCatalogImage* catalog,
                           const FixtureIds& ids) {
  page::PageFilespaceAgentRequestQueue queue;
  page::PageFilespaceAgentQueueRecord record;
  record.sequence = queue.next_sequence++;
  record.request = MakeFilespaceRequest(ids);
  record.request.state =
      page::PageFilespaceAgentRequestState::waiting_filespace_agent;
  record.allowed = true;
  record.filespace_agent_action_required = true;
  record.target_free_pages = 8;
  record.low_water_pages = 4;
  record.diagnostic_code = "ok";
  record.evidence_state = "request_waiting_for_owner";
  record.evidence_id = MakeUuid(platform::UuidKind::object, 10001);
  record.explicit_evidence = true;
  page::PageFilespaceAgentTransitionRecord transition;
  transition.sequence = queue.next_sequence++;
  transition.previous_state = page::PageFilespaceAgentRequestState::created;
  transition.new_state =
      page::PageFilespaceAgentRequestState::waiting_filespace_agent;
  transition.evidence_id = MakeUuid(platform::UuidKind::object, 10002);
  transition.diagnostic_code = "ok";
  transition.reason = "queued for filespace agent";
  transition.explicit_evidence = true;
  record.transitions.push_back(transition);
  queue.records.push_back(record);
  const auto result = impl::EvaluateFilespaceCapacityManagerTick(
      &queue, FilespaceSnapshot(ids), FilespacePolicy(ids));
  Require(result.ok() && result.approved && result.queue_mutated &&
              result.evidence.durable_state_changed,
          "filespace capacity manager did not approve durable capacity window");
  const auto restored =
      page::RestorePageFilespaceAgentRequestQueue(
          page::SerializePageFilespaceAgentRequestQueue(queue));
  Require(restored.ok(), "filespace queue restore failed");
  const auto classified =
      page::ClassifyPageFilespaceAgentRequestQueueForRecovery(restored.queue);
  Require(!classified.ok() && !classified.classifications.empty() &&
              classified.classifications.front().fail_closed &&
              classified.classifications.front().diagnostic_code ==
                  "page_filespace_agent_recovery_partial_state",
          "filespace approved queue recovery did not fail closed on partial owner work");
  PersistDecision(
      catalog,
      "filespace_capacity_manager",
      "filespace_capacity.approve_capacity_window",
      impl::FilespaceCapacityManagerDecisionKindName(result.decision),
      result.diagnostic.diagnostic_code,
      {{"requested_pages", std::to_string(result.requested_pages)},
       {"granted_pages", std::to_string(result.granted_pages)},
       {"queue_mutated", result.queue_mutated ? "true" : "false"},
       {"recovery_classifications", std::to_string(classified.classifications.size())}});
}

impl::StorageHealthManagerPolicy StoragePolicy(const FixtureIds& ids) {
  auto policy = impl::DefaultStorageHealthManagerPolicy();
  policy.database_uuid = ids.database_uuid;
  policy.filespace_uuid = ids.filespace_uuid;
  policy.policy_uuid = ids.policy_uuid;
  policy.critical_automatic_quarantine_policy = true;
  return policy;
}

impl::StorageHealthManagerMetricSnapshot StorageSnapshot(const FixtureIds& ids) {
  impl::StorageHealthManagerMetricSnapshot snapshot;
  snapshot.database_uuid = ids.database_uuid;
  snapshot.filespace_uuid = ids.filespace_uuid;
  snapshot.policy_uuid = ids.policy_uuid;
  snapshot.filespace_health = impl::StorageHealthSeverity::critical;
  snapshot.device_error_count = 1;
  snapshot.checksum_failure_count = 1;
  snapshot.unknown_page_count = 1;
  snapshot.page_allocation_failure_count = 1;
  snapshot.fsync_latency_p99_microseconds = 5000;
  return snapshot;
}

void TestStorageHealth(agent::DurableAgentCatalogImage* catalog,
                       const FixtureIds& ids) {
  impl::StorageHealthManagerActionRequest request;
  request.action =
      impl::StorageHealthManagerActionKind::request_filespace_quarantine;
  request.evidence_kind = impl::StorageHealthEvidenceKind::checksum_failure;
  request.request_uuid = MakeUuid(platform::UuidKind::object, 20000);
  request.evidence_uuid = ids.evidence_uuid;
  request.database_uuid = ids.database_uuid;
  request.filespace_uuid = ids.filespace_uuid;
  request.policy_uuid = ids.policy_uuid;
  request.metric_evidence_uuid = MakeUuid(platform::UuidKind::object, 20001);
  const auto result = impl::EvaluateStorageHealthManagerAction(
      request, StorageSnapshot(ids), StoragePolicy(ids));
  Require(result.ok() && result.route_recommended &&
              result.evidence.durable_state_changed &&
              result.evidence.route_target == "filespace_capacity_manager",
          "storage health manager did not emit durable quarantine route evidence");
  Require(!result.physical_filespace_mutation_attempted &&
              !result.page_ledger_mutation_attempted &&
              !result.index_mutation_attempted,
          "storage health manager attempted direct mutation");
  PersistDecision(
      catalog,
      "storage_health_manager",
      "storage_health.request_filespace_quarantine",
      impl::StorageHealthManagerDecisionKindName(result.decision),
      result.diagnostic.diagnostic_code,
      {{"route_target", result.evidence.route_target},
       {"durable_state_changed",
        result.evidence.durable_state_changed ? "true" : "false"},
       {"direct_mutation_attempted",
        result.physical_filespace_mutation_attempted ? "true" : "false"}});
}

}  // namespace

int main() {
  const auto ids = MakeIds(1);
  auto catalog = DurableCatalog(ids);
  TestPageAllocation(&catalog, ids);
  TestFilespaceCapacity(&catalog, ids);
  TestStorageHealth(&catalog, ids);
  Require(catalog.evidence.size() == 3, "storage agent evidence count mismatch");
  Require(catalog.actions.size() == 3, "storage agent action count mismatch");
  Require(catalog.health.size() == 3, "storage agent health count mismatch");
  Require(catalog.retained_history.size() == 3,
          "storage agent history count mismatch");
  Require(agent::ValidateDurableAgentCatalogForProduction(catalog).ok,
          "storage agent durable catalog invalid after evidence writes");
  return EXIT_SUCCESS;
}
