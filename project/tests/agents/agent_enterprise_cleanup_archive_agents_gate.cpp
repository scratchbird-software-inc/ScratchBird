// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/cleanup_archive_manager.hpp"
#include "agents/storage_version_cleanup_agent.hpp"
#include "agent_durable_catalog.hpp"
#include "agent_enterprise_evidence.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace agent = scratchbird::core::agents;
namespace impl = scratchbird::core::agents::implemented_agents;
namespace mga = scratchbird::transaction::mga;
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
      uuid::GenerateEngineIdentityV7(kind, 1910000000000ull + seed);
  return generated.ok() ? generated.value : platform::TypedUuid{};
}

mga::TransactionIdentity Identity(platform::u64 local_id) {
  const auto identity = mga::MakeTransactionIdentity(
      mga::MakeLocalTransactionId(local_id),
      MakeUuid(platform::UuidKind::transaction, 100 + local_id),
      mga::TransactionScope::local_node);
  Require(identity.ok(), "transaction identity creation failed");
  return identity.identity;
}

mga::TransactionInventoryEntry Entry(platform::u64 local_id,
                                     mga::TransactionState state) {
  mga::TransactionInventoryEntry entry;
  entry.identity = Identity(local_id);
  entry.state = state;
  entry.begin_unix_epoch_millis = 1910000000000ull + local_id;
  if (mga::IsTerminalTransactionState(state)) {
    entry.final_unix_epoch_millis = entry.begin_unix_epoch_millis + 1;
    entry.evidence_record_written = true;
  }
  return entry;
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

mga::RowIdentity Row(platform::u64 seed) {
  mga::RowIdentity row;
  row.row_uuid = MakeUuid(platform::UuidKind::row, 500 + seed);
  return row;
}

mga::RowVersionMetadata Version(const mga::RowIdentity& row,
                                const mga::TransactionInventoryEntry& creator,
                                mga::RowVersionState state,
                                platform::u64 sequence,
                                platform::u64 next_sequence = 0,
                                platform::u64 successor_local_id = 0) {
  mga::RowVersionMetadata metadata;
  metadata.identity.row = row;
  metadata.identity.creator_transaction = creator.identity;
  metadata.identity.version_sequence = sequence;
  metadata.state = state;
  metadata.creator_transaction_state = creator.state;
  metadata.payload_present = state != mga::RowVersionState::rolled_back;
  if (next_sequence != 0) {
    metadata.chain.next_version_sequence = next_sequence;
  }
  if (successor_local_id != 0) {
    metadata.successor_transaction_local_id =
        mga::MakeLocalTransactionId(successor_local_id);
  }
  return metadata;
}

agent::DurableAgentCatalogImage DurableCatalog() {
  agent::DurableAgentCatalogImage image;
  image.source = agent::AgentCatalogStateSource::durable_catalog_image;
  image.schema_version = 1;
  image.authority.durable_catalog_authority = true;
  image.authority.mga_transaction_evidence = true;
  image.authority.mga_transaction_uuid =
      agent::DeterministicAgentRuntimeObjectUuidFromKey("aeic-cleanup-mga");
  image.authority.transaction_generation = 22;
  image.authority.evidence_uuid =
      agent::DeterministicAgentRuntimeObjectUuidFromKey("aeic-cleanup-open");
  image.authority.database_uuid =
      agent::DeterministicAgentRuntimeObjectUuidFromKey("aeic-cleanup-db");
  image.authority.catalog_storage_uuid =
      agent::DeterministicAgentRuntimeObjectUuidFromKey("aeic-cleanup-storage");
  image.authority.storage_commit_evidence_uuid =
      agent::DeterministicAgentRuntimeObjectUuidFromKey("aeic-cleanup-commit");
  image.authority.catalog_generation = 1;
  image.authority.local_transaction_id = 85;
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
    snapshot.generation = 12;
    snapshot.observed_wall_microseconds = observed_wall_microseconds;
    snapshot.scope_uuid = scope_uuid;
    snapshot.digest = "sha256:aeic-cleanup:" + dependency.metric_family;
    snapshot.source_quality = agent::AgentMetricSourceQuality::trusted;
    snapshot.present = true;
    snapshot.trusted = true;
    snapshot.schema_compatible = true;
    snapshot.trust_provenance = "cleanup_archive_gate";
    snapshot.evidence_uuid =
        agent::DeterministicAgentRuntimeObjectUuidFromKey(
            "aeic-cleanup-metric|" + dependency.metric_family);
    snapshot.snapshot_id = "aeic-cleanup:" + dependency.metric_family;
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
  agent::AgentEnterpriseDecisionEvidenceRequest request;
  request.catalog = catalog;
  request.agent_type_id = agent_type_id;
  request.instance_uuid =
      agent::DeterministicAgentRuntimeObjectUuidFromKey(agent_type_id + "-cleanup");
  request.operation_id = operation_id;
  request.principal_uuid =
      agent::DeterministicAgentRuntimePrincipalUuidFromKey("aeic-cleanup-principal");
  request.rights_used = {"agent.execute", "agent.observe"};
  request.scope_uuids = {catalog->authority.database_uuid};
  request.policy_generation = 22;
  request.decision_kind = decision_kind;
  request.result_state = "completed";
  request.diagnostic_code = diagnostic_code;
  request.decision_fields = std::move(fields);
  request.outcome_verification_evidence_uuid =
      agent::DeterministicAgentRuntimeObjectUuidFromKey(agent_type_id + "-cleanup-verify");
  request.created_at_microseconds = before_generation + 300;
  request.metric_context.database_uuid = request.scope_uuids.front();
  request.metric_context.principal_uuid = request.principal_uuid;
  request.metric_context.security_context_present = true;
  request.metric_context.wall_now_microseconds = request.created_at_microseconds;
  request.metric_snapshot_options.expected_scope_uuid = request.scope_uuids.front();
  request.observed_metric_snapshots =
      ObservedSnapshotsFor(agent_type_id, request.scope_uuids.front(),
                           request.created_at_microseconds);
  const auto persisted = agent::AppendEnterpriseAgentDecisionEvidence(request);
  Require(persisted.status.ok, persisted.status.diagnostic_code);
  Require(persisted.evidence_written && persisted.action_written &&
              persisted.history_written && persisted.catalog_root_refreshed,
          agent_type_id + " enterprise cleanup evidence did not persist");
  Require(catalog->authority.catalog_generation > before_generation,
          agent_type_id + " catalog generation did not advance");
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

void TestStorageVersionCleanup(agent::DurableAgentCatalogImage* catalog) {
  auto old = Entry(1, mga::TransactionState::committed);
  auto successor = Entry(2, mga::TransactionState::committed);
  auto rolled_back = Entry(3, mga::TransactionState::rolled_back);
  mga::LocalTransactionInventory inventory;
  inventory.entries = {old, successor, rolled_back};
  inventory.next_local_transaction_id = 4;
  const auto row = Row(1);

  impl::StorageVersionCleanupAgentRequest request;
  request.horizon_request = HorizonRequest(std::move(inventory));
  request.row_versions = {
      Version(row, old, mga::RowVersionState::committed, 10, 20, 2),
      Version(row, successor, mga::RowVersionState::committed, 20),
      Version(Row(2), rolled_back, mga::RowVersionState::rolled_back, 30)};
  request.max_candidate_row_versions = 32;
  request.engine_mga_authoritative = true;
  const auto result = impl::RunStorageVersionCleanupAgentBatch(request);
  Require(result.ok() &&
              result.decision == impl::StorageVersionCleanupDecisionKind::success &&
              result.sweep.cleanup.reclaimed_row_version_count == 2,
          "storage version cleanup did not reclaim eligible versions");
  PersistDecision(
      catalog,
      "storage_version_cleanup_agent",
      "storage_version_cleanup.run_batch",
      impl::StorageVersionCleanupDecisionKindName(result.decision),
      result.diagnostic.diagnostic_code,
      EvidencePairs(result.evidence));

  auto non_authoritative = request;
  non_authoritative.engine_mga_authoritative = false;
  const auto refused = impl::RunStorageVersionCleanupAgentBatch(non_authoritative);
  Require(!refused.ok() &&
              refused.decision ==
                  impl::StorageVersionCleanupDecisionKind::refused_non_authoritative,
          "storage version cleanup accepted non-authoritative MGA cleanup");
}

bool HasEvidence(const impl::CleanupArchiveManagerResult& result,
                 const std::string& key,
                 const std::string& value) {
  for (const auto& field : result.evidence) {
    if (field.key == key && field.value == value) {
      return true;
    }
  }
  return false;
}

void TestCleanupArchive(agent::DurableAgentCatalogImage* catalog) {
  impl::CleanupArchiveManagerSnapshot snapshot;
  snapshot.cleanup_horizon_authoritative = true;
  snapshot.archive_metadata_authoritative = true;
  snapshot.authoritative_cleanup_horizon = 5000;
  snapshot.current_cleanup_lwm = 1000;
  auto result = impl::EvaluateCleanupArchiveManager(snapshot);
  Require(result.ok() &&
              result.decision ==
                  impl::CleanupArchiveManagerDecisionKind::advance_cleanup_lwm &&
              result.proposed_cleanup_lwm > snapshot.current_cleanup_lwm,
          "cleanup archive did not advance bounded cleanup LWM");
  Require(HasEvidence(result, "cleanup_horizon_authoritative", "true"),
          "cleanup archive missing cleanup horizon authority evidence");
  PersistDecision(
      catalog,
      "cleanup_archive_manager",
      "cleanup_archive.advance_lwm",
      impl::CleanupArchiveManagerDecisionKindName(result.decision),
      result.diagnostic.diagnostic_code,
      EvidencePairs(result.evidence));

  snapshot.legal_hold_active = true;
  auto held = impl::EvaluateCleanupArchiveManager(snapshot);
  Require(held.ok() &&
              held.decision == impl::CleanupArchiveManagerDecisionKind::no_action &&
              HasEvidence(held, "legal_hold_active", "true") &&
              HasEvidence(held, "recovery_authority", "false"),
          "cleanup archive legal hold did not block with non-authority evidence");

  snapshot.legal_hold_active = false;
  snapshot.recovery_authority = true;
  auto refused = impl::EvaluateCleanupArchiveManager(snapshot);
  Require(!refused.ok() &&
              refused.diagnostic.diagnostic_code ==
                  "SB_AGENT_CLEANUP_ARCHIVE_AUTHORITY_UNTRUSTED",
          "cleanup archive accepted recovery authority");
}

}  // namespace

int main() {
  auto catalog = DurableCatalog();
  TestStorageVersionCleanup(&catalog);
  TestCleanupArchive(&catalog);
  Require(catalog.evidence.size() == 2, "cleanup agent evidence count mismatch");
  Require(catalog.actions.size() == 2, "cleanup agent action count mismatch");
  Require(catalog.health.size() == 2, "cleanup agent health count mismatch");
  Require(catalog.retained_history.size() == 2,
          "cleanup agent history count mismatch");
  Require(agent::ValidateDurableAgentCatalogForProduction(catalog).ok,
          "cleanup agent durable catalog invalid after evidence writes");
  return EXIT_SUCCESS;
}
