// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_durable_catalog_store_api.hpp"
#include "agents/agent_enterprise_decision_store_api.hpp"

#include "agent_enterprise_evidence.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

namespace agents = scratchbird::core::agents;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace mga = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

struct TestDatabase {
  std::filesystem::path path;
  std::string database_uuid;
  std::string transaction_uuid;
  std::uint64_t local_transaction_id = 0;
};

void Cleanup(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const char* suffix : {".dirty.manifest",
                             ".sb.mga_event_sequence_allocator",
                             ".sb.mga_index_entries",
                             ".sb.mga_large_values",
                             ".sb.mga_relation_descriptors",
                             ".sb.mga_relation_metadata",
                             ".sb.mga_row_versions",
                             ".sb.mga_savepoints",
                             ".sb.mga_secondary_index_delta_ledger"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

TestDatabase CreateActiveDatabase(const char* basename) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / basename;
  Cleanup(path);

  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::database, 1800000002001ull);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::filespace, 1800000002002ull);
  Require(database_uuid.ok(), "database UUID generation failed");
  Require(filespace_uuid.ok(), "filespace UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1800000002003ull;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  Require(db::CreateDatabaseFile(create).ok(), "database creation failed");

  auto inventory = mga::MakeEmptyLocalTransactionInventory();
  const auto transaction_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::transaction, 1800000002004ull);
  Require(transaction_uuid.ok(), "transaction UUID generation failed");
  auto begun = mga::BeginLocalTransaction(std::move(inventory),
                                          transaction_uuid.value,
                                          1800000002005ull);
  Require(begun.ok(), "local transaction begin failed");
  Require(db::PersistLocalTransactionInventoryToDatabase(path.string(),
                                                         begun.inventory)
              .ok(),
          "local transaction inventory persist failed");

  TestDatabase result;
  result.path = path;
  result.database_uuid = uuid::UuidToString(database_uuid.value.value);
  result.transaction_uuid = uuid::UuidToString(transaction_uuid.value.value);
  result.local_transaction_id = begun.entry.identity.local_id.value;
  return result;
}

api::EngineRequestContext Context(const TestDatabase& database) {
  api::EngineRequestContext context;
  context.request_id = "aeic-enterprise-decision-store";
  context.database_path = database.path.string();
  context.database_uuid.canonical = database.database_uuid;
  context.transaction_uuid.canonical = database.transaction_uuid;
  context.local_transaction_id = database.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      database.local_transaction_id;
  context.security_context_present = true;
  context.principal_uuid.canonical =
      "018f0000-0000-7000-8000-00000000ee10";
  return context;
}

void SeedCatalog(const api::EngineRequestContext& context) {
  api::AgentDurableCatalogStoreRequest seed;
  seed.context = context;
  seed.image = agents::DurableAgentCatalogImage{};
  seed.evidence_uuid = "018f0000-0000-7000-8000-00000000ee11";
  seed.production_live_path = true;
  seed.fsync_or_checkpoint_evidence = true;
  Require(api::PersistAgentDurableCatalogImage(seed).ok,
          "initial enterprise decision catalog seed failed");
}

std::vector<agents::AgentObservedMetricSnapshot> ObservedSnapshotsFor(
    const std::string& agent_type_id,
    const std::string& scope_uuid,
    agents::u64 observed_wall_microseconds) {
  const auto descriptor = agents::FindAgentType(agent_type_id);
  Require(descriptor.has_value(), "agent descriptor missing for decision store metrics");
  std::vector<agents::AgentObservedMetricSnapshot> snapshots;
  for (const auto& dependency : descriptor->metric_dependencies) {
    agents::AgentObservedMetricSnapshot snapshot;
    snapshot.metric_family = dependency.metric_family;
    snapshot.namespace_path = dependency.namespace_prefix.empty()
                                  ? dependency.metric_family
                                  : dependency.namespace_prefix + ".observed";
    snapshot.generation = 15;
    snapshot.observed_wall_microseconds = observed_wall_microseconds;
    snapshot.scope_uuid = scope_uuid;
    snapshot.digest = "sha256:decision-store:" + dependency.metric_family;
    snapshot.source_quality = agents::AgentMetricSourceQuality::trusted;
    snapshot.present = true;
    snapshot.trusted = true;
    snapshot.schema_compatible = true;
    snapshot.trust_provenance = "test_metric_registry";
    snapshot.evidence_uuid =
        agents::DeterministicAgentRuntimeObjectUuidFromKey(
            "decision-store-metric-evidence|" + dependency.metric_family);
    snapshot.snapshot_id = "decision-store:" + dependency.metric_family;
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

agents::AgentEnterpriseDecisionEvidenceRequest Decision(
    std::string diagnostic_code = "SB_AGENT_NODE_RESOURCE_CAPABILITY_READY") {
  agents::AgentEnterpriseDecisionEvidenceRequest request;
  request.agent_type_id = "node_resource_agent";
  request.instance_uuid = "018f0000-0000-7000-8000-00000000ee20";
  request.operation_id = "node_resource.publish_capability";
  request.principal_uuid = "018f0000-0000-7000-8000-00000000ee21";
  request.rights_used = {"agent.execute", "agent.observe"};
  request.scope_uuids = {"018f0000-0000-7000-8000-00000000ee22"};
  request.policy_generation = 5;
  request.decision_kind = "publish_node_capability";
  request.result_state = "completed";
  request.diagnostic_code = std::move(diagnostic_code);
  request.decision_fields = {{"cpu_count", "16"},
                             {"memory_pressure_percent", "12"}};
  request.outcome_verification_evidence_uuid =
      "018f0000-0000-7000-8000-00000000ee23";
  request.created_at_microseconds = 1800000002010ull;
  request.metric_context.database_uuid = request.scope_uuids.front();
  request.metric_context.principal_uuid = request.principal_uuid;
  request.metric_context.security_context_present = true;
  request.metric_context.wall_now_microseconds = request.created_at_microseconds;
  request.metric_snapshot_options.expected_scope_uuid = request.scope_uuids.front();
  request.observed_metric_snapshots = ObservedSnapshotsFor(
      request.agent_type_id,
      request.scope_uuids.front(),
      request.created_at_microseconds);
  return request;
}

void TestEnterpriseDecisionPersistsAndReplaysFromStore() {
  const auto database = CreateActiveDatabase(
      "scratchbird_aeic_enterprise_decision_store.sbdb");
  const auto context = Context(database);
  SeedCatalog(context);

  api::AgentEnterpriseDecisionStoreRequest request;
  request.context = context;
  request.decision = Decision();
  request.production_live_path = true;
  request.fsync_or_checkpoint_evidence = true;

  const auto first = api::AppendEnterpriseAgentDecisionEvidenceToStore(request);
  Require(first.decision.status.ok, "enterprise decision store append failed: " +
                                      first.decision.status.diagnostic_code);
  Require(first.loaded_from_store && first.persisted_to_store,
          "enterprise decision did not load/persist durable catalog");

  const auto loaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(loaded.ok, "catalog reload after enterprise decision failed");
  Require(loaded.image.evidence.size() == 1,
          "enterprise decision evidence missing");
  Require(loaded.image.actions.size() == 1,
          "enterprise decision action missing");
  Require(loaded.image.health.size() == 1,
          "enterprise decision health row missing");
  Require(loaded.image.retained_history.size() >= 3,
          "enterprise decision history missing");
  Require(loaded.image.resource_reservations.size() == 1,
          "enterprise decision resource reservation missing");
  Require(loaded.image.resource_reservations.front().state ==
              agents::DurableAgentResourceReservationState::released,
          "enterprise decision resource reservation was not released");

  const auto duplicate = api::AppendEnterpriseAgentDecisionEvidenceToStore(request);
  Require(duplicate.decision.status.ok && duplicate.decision.idempotent_replay,
          "enterprise decision did not replay idempotently from durable catalog");
  Require(!duplicate.persisted_to_store,
          "enterprise decision replay wrote a duplicate record");

  const auto replay_loaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(replay_loaded.ok, "catalog reload after enterprise replay failed");
  Require(replay_loaded.image.actions.size() == 1,
          "enterprise decision replay duplicated action records");

  Cleanup(database.path);
}

void TestEnterpriseDecisionRequiresCheckpointEvidence() {
  const auto database = CreateActiveDatabase(
      "scratchbird_aeic_enterprise_decision_checkpoint.sbdb");
  const auto context = Context(database);
  SeedCatalog(context);

  api::AgentEnterpriseDecisionStoreRequest request;
  request.context = context;
  request.decision = Decision("SB_AGENT_NODE_RESOURCE_CHECKPOINT_TEST");
  request.production_live_path = true;
  request.fsync_or_checkpoint_evidence = false;
  const auto result = api::AppendEnterpriseAgentDecisionEvidenceToStore(request);
  Require(!result.decision.status.ok &&
              result.decision.status.diagnostic_code ==
                  "SB_AGENT_ENTERPRISE_EVIDENCE_STORE.PERSIST_FAILED",
          "enterprise decision store accepted missing checkpoint evidence");

  const auto loaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(loaded.ok, "catalog reload after enterprise checkpoint refusal failed");
  Require(loaded.image.actions.empty(),
          "failed enterprise decision persist mutated durable catalog");
  Require(loaded.image.resource_reservations.empty(),
          "failed enterprise decision persist leaked resource reservation");

  Cleanup(database.path);
}

void TestEnterpriseDecisionRequiresStrictObservedMetrics() {
  const auto database = CreateActiveDatabase(
      "scratchbird_aeic_enterprise_decision_metrics.sbdb");
  const auto context = Context(database);
  SeedCatalog(context);

  api::AgentEnterpriseDecisionStoreRequest request;
  request.context = context;
  request.decision = Decision("SB_AGENT_NODE_RESOURCE_METRIC_REQUIRED_TEST");
  request.decision.observed_metric_snapshots.clear();
  request.production_live_path = true;
  request.fsync_or_checkpoint_evidence = true;
  const auto missing = api::AppendEnterpriseAgentDecisionEvidenceToStore(request);
  Require(!missing.decision.status.ok &&
              missing.decision.status.diagnostic_code ==
                  "SB_AGENT_METRIC_SNAPSHOT.MISSING",
          "enterprise decision accepted missing observed metric snapshots");

  auto mismatch_request = request;
  mismatch_request.decision = Decision("SB_AGENT_NODE_RESOURCE_METRIC_MISMATCH_TEST");
  mismatch_request.decision.observed_metric_digest = "sha256:forged-digest";
  const auto mismatch =
      api::AppendEnterpriseAgentDecisionEvidenceToStore(mismatch_request);
  Require(!mismatch.decision.status.ok &&
              mismatch.decision.status.diagnostic_code ==
                  "SB_AGENT_ENTERPRISE_EVIDENCE.METRIC_DIGEST_MISMATCH",
          "enterprise decision accepted forged observed metric digest");

  const auto loaded = api::LoadAgentDurableCatalogImage(context, true);
  Require(loaded.ok, "catalog reload after metric refusal failed");
  Require(loaded.image.actions.empty(),
          "failed enterprise metric validation mutated durable catalog");
  Require(loaded.image.resource_reservations.empty(),
          "failed enterprise metric validation leaked resource reservation");

  Cleanup(database.path);
}

}  // namespace

int main() {
  TestEnterpriseDecisionPersistsAndReplaysFromStore();
  TestEnterpriseDecisionRequiresCheckpointEvidence();
  TestEnterpriseDecisionRequiresStrictObservedMetrics();
  return EXIT_SUCCESS;
}
