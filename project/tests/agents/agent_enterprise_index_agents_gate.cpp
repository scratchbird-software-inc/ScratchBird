// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/index_garbage_cleanup_agent.hpp"
#include "agents/index_health_manager.hpp"
#include "agents/shadow_index_build_agent.hpp"
#include "agent_durable_catalog.hpp"
#include "agent_enterprise_evidence.hpp"
#include "shadow_index_build_lifecycle.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace impl = scratchbird::core::agents::implemented_agents;
namespace idx = scratchbird::core::index;
namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

platform::u64 NextMillis() {
  static platform::u64 next = 1779525000000ull;
  return ++next;
}

platform::TypedUuid NewUuid(platform::UuidKind kind) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NextMillis());
  Require(generated.ok(), "AEIC-024 generated UUID creation failed");
  return generated.value;
}

mga::TransactionIdentity NewIdentity(platform::u64 local_id) {
  const auto identity = mga::MakeTransactionIdentity(
      mga::MakeLocalTransactionId(local_id),
      NewUuid(platform::UuidKind::transaction),
      mga::TransactionScope::local_node);
  Require(identity.ok(), "AEIC-024 transaction identity creation failed");
  return identity.identity;
}

mga::TransactionInventoryEntry Entry(platform::u64 local_id,
                                     mga::TransactionState state) {
  mga::TransactionInventoryEntry entry;
  entry.identity = NewIdentity(local_id);
  entry.state = state;
  entry.begin_unix_epoch_millis = NextMillis();
  if (mga::IsTerminalTransactionState(state)) {
    entry.final_unix_epoch_millis = NextMillis();
    entry.evidence_record_written = true;
  }
  return entry;
}

mga::LocalTransactionInventory Inventory(
    std::vector<mga::TransactionInventoryEntry> entries,
    platform::u64 next_local_transaction_id) {
  mga::LocalTransactionInventory inventory;
  inventory.entries = std::move(entries);
  inventory.next_local_transaction_id = next_local_transaction_id;
  return inventory;
}

mga::AuthoritativeCleanupHorizonRequest HorizonRequest(
    mga::LocalTransactionInventory inventory) {
  mga::AuthoritativeCleanupHorizonRequest request;
  request.inventory = std::move(inventory);
  request.inventory_authoritative = true;
  request.inventory_complete = true;
  request.active_snapshot_inventory_authoritative = true;
  return request;
}

agents::DurableAgentCatalogImage DurableCatalog() {
  agents::DurableAgentCatalogImage image;
  image.source = agents::AgentCatalogStateSource::durable_catalog_image;
  image.schema_version = 1;
  image.authority.durable_catalog_authority = true;
  image.authority.mga_transaction_evidence = true;
  image.authority.mga_transaction_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic024-mga");
  image.authority.transaction_generation = 24;
  image.authority.evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic024-open");
  image.authority.database_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic024-db");
  image.authority.catalog_storage_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic024-storage");
  image.authority.storage_commit_evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic024-commit");
  image.authority.catalog_generation = 1;
  image.authority.local_transaction_id = 2400;
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
  Require(descriptor.has_value(), "AEIC-024 agent descriptor missing");
  std::vector<agents::AgentObservedMetricSnapshot> snapshots;
  for (const auto& dependency : descriptor->metric_dependencies) {
    agents::AgentObservedMetricSnapshot snapshot;
    snapshot.metric_family = dependency.metric_family;
    snapshot.namespace_path = dependency.namespace_prefix.empty()
                                  ? dependency.metric_family
                                  : dependency.namespace_prefix + ".observed";
    snapshot.generation = 2400;
    snapshot.observed_wall_microseconds = observed_wall_microseconds;
    snapshot.scope_uuid = scope_uuid;
    snapshot.digest = "sha256:aeic024:" + dependency.metric_family;
    snapshot.source_quality = agents::AgentMetricSourceQuality::trusted;
    snapshot.present = true;
    snapshot.trusted = true;
    snapshot.schema_compatible = true;
    snapshot.trust_provenance = "enterprise_index_agents_gate";
    snapshot.evidence_uuid =
        agents::DeterministicAgentRuntimeObjectUuidFromKey(
            "aeic024-metric-evidence|" + dependency.metric_family);
    snapshot.snapshot_id = "aeic024:" + dependency.metric_family;
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
      agents::DeterministicAgentRuntimeObjectUuidFromKey(agent_type_id + "-aeic024");
  request.operation_id = operation_id;
  request.principal_uuid =
      agents::DeterministicAgentRuntimePrincipalUuidFromKey("aeic024-principal");
  request.rights_used = {"agent.execute", "agent.recommend", "agent.observe"};
  request.scope_uuids = {
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic024-scope")};
  request.policy_generation = 24;
  request.decision_kind = decision_kind;
  request.result_state = "completed";
  request.diagnostic_code = diagnostic_code;
  request.decision_fields = fields;
  request.outcome_verification_evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(agent_type_id + "-aeic024-verification");
  request.created_at_microseconds = before_generation + 240000;
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
          "AEIC-024 decision evidence was not fully durable");
  Require(catalog->authority.catalog_generation > before_generation,
          "AEIC-024 durable catalog generation did not advance");
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

idx::SecondaryIndexDeltaLedgerRecord MergedCleanedRecord(
    const platform::TypedUuid& index_uuid,
    const platform::TypedUuid& table_uuid,
    platform::u64 local_transaction_id,
    std::string key_payload) {
  idx::SecondaryIndexDeltaLedgerRecord record;
  record.delta.delta_id = NewUuid(platform::UuidKind::object);
  record.delta.index_uuid = index_uuid;
  record.delta.table_uuid = table_uuid;
  record.delta.row_uuid = NewUuid(platform::UuidKind::row);
  record.delta.version_uuid = NewUuid(platform::UuidKind::row);
  record.delta.transaction_uuid = NewUuid(platform::UuidKind::transaction);
  record.delta.local_transaction_id = local_transaction_id;
  record.delta.delta_kind = idx::SecondaryIndexDeltaKind::insert;
  record.delta.key_payload = std::move(key_payload);
  record.delta.committed = true;
  record.commit_state =
      idx::SecondaryIndexDeltaLedgerCommitState::merged_cleaned;
  record.source_evidence_reference = "aeic024_index_cleanup";
  return record;
}

impl::IndexGarbageCleanupAgentRequest IndexCleanupRequest(
    mga::AuthoritativeCleanupHorizonRequest horizon,
    platform::u64 ledger_local_transaction_id) {
  const auto index_uuid = NewUuid(platform::UuidKind::object);
  const auto table_uuid = NewUuid(platform::UuidKind::object);

  impl::IndexGarbageCleanupAgentRequest request;
  request.horizon_request = std::move(horizon);
  request.index_uuid = index_uuid;
  request.table_uuid = table_uuid;
  request.ledger.records.push_back(MergedCleanedRecord(index_uuid,
                                                       table_uuid,
                                                       ledger_local_transaction_id,
                                                       "alpha"));
  request.index_kind = idx::SecondaryIndexKind::non_unique;
  request.max_records_to_scan = 32;
  request.max_records_to_clean = 32;
  request.engine_mga_authoritative = true;
  return request;
}

idx::ShadowIndexBuildRequest ValidShadowRequest(platform::u64 seed) {
  idx::ShadowIndexBuildRequest request;
  request.shadow_index_uuid = NewUuid(platform::UuidKind::object);
  request.table_uuid = NewUuid(platform::UuidKind::object);
  request.index_kind = idx::SecondaryIndexKind::non_unique;
  request.engine_mga_inventory_evidence_ref =
      "engine_mga_inventory:aeic024:" + std::to_string(seed);
  request.engine_mga_horizon_evidence_ref =
      "engine_mga_horizon:aeic024:" + std::to_string(seed);
  return request;
}

void TestIndexHealthManager(agents::DurableAgentCatalogImage* catalog) {
  impl::IndexHealthManagerSnapshot snapshot;
  snapshot.index_uuid = "index-aeic024";
  snapshot.index_metrics_authoritative = true;
  snapshot.filespace_metrics_authoritative = true;
  snapshot.read_amplification_ratio = 8;
  auto rebuild = impl::EvaluateIndexHealthManager(snapshot);
  Require(rebuild.ok() &&
              rebuild.decision ==
                  impl::IndexHealthManagerDecisionKind::recommend_index_rebuild,
          "AEIC-024 index health did not recommend rebuild");
  PersistDecision(catalog,
                  "index_health_manager",
                  "index_health.evaluate",
                  impl::IndexHealthManagerDecisionKindName(rebuild.decision),
                  rebuild.diagnostic.diagnostic_code,
                  EvidencePairs(rebuild.evidence));

  snapshot.read_amplification_ratio = 0;
  snapshot.unused_for_microseconds = 604800000000ull;
  auto drop = impl::EvaluateIndexHealthManager(snapshot);
  Require(drop.ok() &&
              drop.decision ==
                  impl::IndexHealthManagerDecisionKind::recommend_index_drop,
          "AEIC-024 index health did not recommend drop");

  snapshot.unused_for_microseconds = 0;
  snapshot.index_unique_or_constraint_backed = true;
  snapshot.filespace_fsync_p99_microseconds = 4000;
  auto filespace = impl::EvaluateIndexHealthManager(snapshot);
  Require(filespace.ok() &&
              filespace.decision ==
                  impl::IndexHealthManagerDecisionKind::
                      request_fast_filespace_for_index_rebuild,
          "AEIC-024 index health did not request fast filespace");

  snapshot.filespace_fsync_p99_microseconds = 0;
  auto no_action = impl::EvaluateIndexHealthManager(snapshot);
  Require(no_action.ok() &&
              no_action.decision ==
                  impl::IndexHealthManagerDecisionKind::no_action,
          "AEIC-024 index health did not reach no-action path");

  snapshot.parser_authority = true;
  auto refused = impl::EvaluateIndexHealthManager(snapshot);
  Require(!refused.ok() && refused.fail_closed &&
              refused.diagnostic.diagnostic_code ==
                  "SB_AGENT_INDEX_HEALTH_AUTHORITY_UNTRUSTED",
          "AEIC-024 index health accepted parser authority");
}

void TestIndexGarbageCleanupAgent(agents::DurableAgentCatalogImage* catalog) {
  auto cleanup = impl::RunIndexGarbageCleanupAgentBatch(
      IndexCleanupRequest(HorizonRequest(Inventory({
                            Entry(1, mga::TransactionState::committed),
                          }, 2)),
                          1));
  Require(cleanup.ok(), "AEIC-024 index garbage cleanup refused success path");
  Require(cleanup.decision == idx::SecondaryIndexGarbageCleanupDecisionKind::success,
          "AEIC-024 index garbage cleanup decision was not success");
  Require(cleanup.validation_before_ok && cleanup.validation_after_ok,
          "AEIC-024 index garbage cleanup validation proof missing");
  Require(cleanup.after.cleaned_garbage_records == 1,
          "AEIC-024 index garbage cleanup cleaned count mismatch");
  auto cleanup_evidence = EvidencePairs(cleanup.evidence);
  cleanup_evidence.emplace_back("helper_agent_type_id",
                                "index_garbage_cleanup_agent");
  PersistDecision(catalog,
                  "index_health_manager",
                  "index_garbage_cleanup.run_batch",
                  idx::SecondaryIndexGarbageCleanupDecisionKindName(cleanup.decision),
                  cleanup.diagnostic.diagnostic_code,
                  cleanup_evidence);

  auto blocked = impl::RunIndexGarbageCleanupAgentBatch(
      IndexCleanupRequest(HorizonRequest(Inventory({
                            Entry(1, mga::TransactionState::committed),
                            Entry(2, mga::TransactionState::active),
                            Entry(3, mga::TransactionState::committed),
                          }, 4)),
                          3));
  Require(blocked.ok() && blocked.horizon_blocked,
          "AEIC-024 index garbage cleanup did not preserve horizon block");

  auto non_authoritative = IndexCleanupRequest(
      HorizonRequest(Inventory({Entry(1, mga::TransactionState::committed)}, 2)),
      1);
  non_authoritative.horizon_request.inventory_authoritative = false;
  auto refused = impl::RunIndexGarbageCleanupAgentBatch(non_authoritative);
  Require(!refused.ok() && refused.fail_closed &&
              refused.decision ==
                  idx::SecondaryIndexGarbageCleanupDecisionKind::
                      refused_non_authoritative,
          "AEIC-024 index garbage cleanup accepted non-authoritative horizon");
}

void TestShadowIndexBuildAgent(agents::DurableAgentCatalogImage* catalog) {
  idx::ShadowIndexBuildLedger ledger;
  auto requested = idx::RequestShadowIndexBuild(&ledger, ValidShadowRequest(2400));
  Require(requested.ok(), "AEIC-024 shadow index request refused");
  auto record = requested.record;
  Require(!idx::EvaluateShadowIndexPlannerVisibility(record).ok(),
          "AEIC-024 shadow index was visible before build");
  Require(idx::StartShadowIndexBuild(&ledger, &record).ok(),
          "AEIC-024 shadow index start refused");
  Require(idx::CompleteShadowIndexBuild(&ledger, &record).ok(),
          "AEIC-024 shadow index complete refused");

  idx::ShadowIndexValidationRequest validation;
  validation.validation_succeeded = true;
  validation.validation_evidence_ref = "validation_evidence:aeic024";
  validation.engine_mga_inventory_evidence_present = true;
  Require(idx::ValidateShadowIndexBuild(&ledger, &record, validation).ok(),
          "AEIC-024 shadow index validation refused");

  idx::ShadowIndexPublishBarrierRequest barrier;
  barrier.publish_barrier_evidence_ref = "publish_barrier:engine_mga:aeic024";
  barrier.engine_owned_mga_publish_barrier = true;
  Require(idx::MarkShadowIndexPublishReady(&ledger, &record, barrier).ok(),
          "AEIC-024 shadow index publish-ready refused");

  impl::ShadowIndexBuildAgentPublishRequest agent_request;
  agent_request.engine_mga_authoritative = true;
  agent_request.agent_evidence_ref = "agent_evidence:aeic024:publish";
  auto published =
      impl::PublishShadowIndexBuildAgentStep(&ledger, &record, agent_request);
  Require(published.ok(), "AEIC-024 shadow index agent refused publish");
  Require(record.state == idx::ShadowIndexBuildState::published &&
              record.planner_visible && record.read_visible,
          "AEIC-024 shadow index publish did not make index visible");
  Require(idx::EvaluateShadowIndexPlannerVisibility(record).ok(),
          "AEIC-024 published shadow index was not planner-visible");
  auto shadow_evidence = EvidencePairs(published.evidence);
  shadow_evidence.emplace_back("helper_agent_type_id",
                               "shadow_index_build_agent");
  PersistDecision(catalog,
                  "index_health_manager",
                  "shadow_index_build.publish",
                  idx::ShadowIndexBuildDecisionName(
                      published.lifecycle.decision),
                  published.diagnostic.diagnostic_code,
                  shadow_evidence);

  auto refused_record = requested.record;
  impl::ShadowIndexBuildAgentPublishRequest refused_request;
  auto refused = impl::PublishShadowIndexBuildAgentStep(
      &ledger, &refused_record, refused_request);
  Require(!refused.ok() && refused.fail_closed &&
              refused.diagnostic.diagnostic_code ==
                  "shadow_index_agent_non_authoritative_refusal",
          "AEIC-024 shadow index agent accepted non-authoritative publish");
}

}  // namespace

int main() {
  auto catalog = DurableCatalog();
  TestIndexHealthManager(&catalog);
  TestIndexGarbageCleanupAgent(&catalog);
  TestShadowIndexBuildAgent(&catalog);
  Require(catalog.evidence.size() == 3,
          "AEIC-024 decision evidence count mismatch");
  Require(catalog.actions.size() == 3,
          "AEIC-024 action count mismatch");
  Require(catalog.retained_history.size() == 3,
          "AEIC-024 retained history count mismatch");
  Require(agents::ValidateDurableAgentCatalogForProduction(catalog).ok,
          "AEIC-024 durable catalog invalid after evidence writes");
  std::cout
      << "AEIC024_INDEX_HEALTH "
      << "AEIC024_INDEX_GARBAGE_CLEANUP "
      << "AEIC024_SHADOW_INDEX_BUILD ok\n";
  return EXIT_SUCCESS;
}
