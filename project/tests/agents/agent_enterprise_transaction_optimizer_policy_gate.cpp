// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/policy_recommendation_manager.hpp"
#include "agents/runtime_learning_agent.hpp"
#include "agents/transaction_pressure_manager.hpp"
#include "agent_durable_catalog.hpp"
#include "agent_enterprise_evidence.hpp"
#include "agent_optimizer_recommendation_bridge.hpp"
#include "agent_policy_recommendation_application.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace impl = scratchbird::core::agents::implemented_agents;
namespace mga = scratchbird::transaction::mga;
namespace opt = scratchbird::engine::optimizer;
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
  static platform::u64 next = 1779526000000ull;
  return ++next;
}

platform::TypedUuid NewUuid(platform::UuidKind kind) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NextMillis());
  Require(generated.ok(), "AEIC-025 generated UUID creation failed");
  return generated.value;
}

mga::TransactionIdentity NewIdentity(platform::u64 local_id) {
  const auto identity = mga::MakeTransactionIdentity(
      mga::MakeLocalTransactionId(local_id),
      NewUuid(platform::UuidKind::transaction),
      mga::TransactionScope::local_node);
  Require(identity.ok(), "AEIC-025 transaction identity creation failed");
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

mga::AuthoritativeCleanupHorizonRequest StandardHorizonRequest() {
  mga::LocalTransactionInventory inventory;
  inventory.entries = {
      Entry(1, mga::TransactionState::committed),
      Entry(2, mga::TransactionState::active),
      Entry(3, mga::TransactionState::committed)};
  inventory.next_local_transaction_id = 4;

  mga::AuthoritativeCleanupHorizonRequest request;
  request.inventory = std::move(inventory);
  request.inventory_authoritative = true;
  request.inventory_complete = true;
  request.active_snapshot_inventory_authoritative = true;
  request.always_in_transaction_policy = true;
  request.always_active_session_inventory_authoritative = true;
  request.always_active_sessions.push_back(
      {"session:primary", mga::MakeLocalTransactionId(2), true});
  return request;
}

impl::TransactionPressureSessionSnapshot Session(platform::u64 local_id,
                                                 platform::u64 idle_us,
                                                 bool warned = true) {
  impl::TransactionPressureSessionSnapshot session;
  session.stable_session_id = "session:primary";
  session.stable_connection_id = "connection:primary";
  session.stable_principal_id = "principal:primary";
  session.current_local_transaction_id = mga::MakeLocalTransactionId(local_id);
  session.idle_microseconds = idle_us;
  session.warning_already_notified = warned;
  return session;
}

impl::TransactionPressureManagerPolicy TransactionPressurePolicy() {
  auto policy = impl::DefaultTransactionPressureManagerPolicy();
  policy.warn_after_idle_microseconds = 100;
  policy.request_restart_after_idle_microseconds = 200;
  policy.request_reauth_after_idle_microseconds = 250;
  policy.request_cancel_after_idle_microseconds = 275;
  policy.force_after_idle_microseconds = 300;
  return policy;
}

agents::DurableAgentCatalogImage DurableCatalog() {
  agents::DurableAgentCatalogImage image;
  image.source = agents::AgentCatalogStateSource::durable_catalog_image;
  image.schema_version = 1;
  image.authority.durable_catalog_authority = true;
  image.authority.mga_transaction_evidence = true;
  image.authority.mga_transaction_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic025-mga");
  image.authority.transaction_generation = 25;
  image.authority.evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic025-open");
  image.authority.database_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic025-db");
  image.authority.catalog_storage_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic025-storage");
  image.authority.storage_commit_evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic025-commit");
  image.authority.catalog_generation = 1;
  image.authority.local_transaction_id = 2500;
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
  Require(descriptor.has_value(), "AEIC-025 agent descriptor missing");
  std::vector<agents::AgentObservedMetricSnapshot> snapshots;
  for (const auto& dependency : descriptor->metric_dependencies) {
    agents::AgentObservedMetricSnapshot snapshot;
    snapshot.metric_family = dependency.metric_family;
    snapshot.namespace_path = dependency.namespace_prefix.empty()
                                  ? dependency.metric_family
                                  : dependency.namespace_prefix + ".observed";
    snapshot.generation = 2500;
    snapshot.observed_wall_microseconds = observed_wall_microseconds;
    snapshot.scope_uuid = scope_uuid;
    snapshot.digest = "sha256:aeic025:" + dependency.metric_family;
    snapshot.source_quality = agents::AgentMetricSourceQuality::trusted;
    snapshot.present = true;
    snapshot.trusted = true;
    snapshot.schema_compatible = true;
    snapshot.trust_provenance = "enterprise_transaction_optimizer_policy_gate";
    snapshot.evidence_uuid =
        agents::DeterministicAgentRuntimeObjectUuidFromKey(
            "aeic025-metric-evidence|" + dependency.metric_family);
    snapshot.snapshot_id = "aeic025:" + dependency.metric_family;
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
      agents::DeterministicAgentRuntimeObjectUuidFromKey(agent_type_id + "-aeic025");
  request.operation_id = operation_id;
  request.principal_uuid =
      agents::DeterministicAgentRuntimePrincipalUuidFromKey("aeic025-principal");
  request.rights_used = {"agent.execute", "agent.recommend", "agent.observe"};
  request.scope_uuids = {
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic025-scope")};
  request.policy_generation = 25;
  request.decision_kind = decision_kind;
  request.result_state = "completed";
  request.diagnostic_code = diagnostic_code;
  request.decision_fields = fields;
  request.outcome_verification_evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(agent_type_id + "-aeic025-verification");
  request.created_at_microseconds = before_generation + 250000;
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
          "AEIC-025 decision evidence was not fully durable");
  Require(catalog->authority.catalog_generation > before_generation,
          "AEIC-025 durable catalog generation did not advance");
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

std::vector<std::pair<std::string, std::string>> EvidencePairsFromStrings(
    const std::vector<std::string>& fields) {
  std::vector<std::pair<std::string, std::string>> pairs;
  for (const auto& field : fields) {
    pairs.emplace_back("evidence", field);
  }
  return pairs;
}

agents::AgentOptimizerRecommendationEvidence OptimizerAgentEvidence(
    const std::string& agent_type_id,
    const std::string& recommendation_kind) {
  agents::AgentOptimizerRecommendationEvidence evidence;
  evidence.recommendation_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "aeic025-optimizer-recommendation");
  evidence.agent_type_id = agent_type_id;
  evidence.evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "aeic025-optimizer-evidence");
  evidence.metric_digest = "sha256:aeic025-optimizer-metric";
  evidence.scope_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic025-scope");
  evidence.recommendation_kind = recommendation_kind;
  evidence.principal_uuid =
      agents::DeterministicAgentRuntimePrincipalUuidFromKey("aeic025-principal");
  evidence.policy_generation = 25;
  evidence.observed_policy_generation = 25;
  evidence.durable_catalog_state = true;
  evidence.strict_metric_snapshot = true;
  evidence.metric_trusted = true;
  evidence.metric_fresh = true;
  return evidence;
}

opt::OptimizerRuntimeFeedback OptimizerFeedback() {
  opt::OptimizerRuntimeFeedback feedback;
  feedback.operator_family = "runtime_learning";
  feedback.plan_shape = "aeic025_runtime_learning";
  feedback.cost_profile_id = "agent-recommendation-v1";
  feedback.estimated_rows = 100;
  feedback.actual_rows = 300;
  feedback.estimated_pages = 10;
  feedback.actual_pages = 30;
  feedback.estimated_io_operations = 10;
  feedback.actual_io_operations = 30;
  feedback.estimated_visibility_recheck_rows = 4;
  feedback.actual_visibility_recheck_rows = 4;
  feedback.memory_grant_bytes = 1024 * 1024;
  feedback.peak_memory_bytes = 512 * 1024;
  feedback.freshness_microseconds = 10;
  feedback.max_freshness_microseconds = 1000;
  feedback.policy_allowed = true;
  feedback.advisory_only = true;
  feedback.mga_visibility_recheck_preserved = true;
  feedback.transaction_finality_authority = "engine_transaction_inventory";
  return feedback;
}

agents::AgentIndexReadinessEvidence IndexReadiness() {
  agents::AgentIndexReadinessEvidence evidence;
  evidence.evidence_digest = "sha256:aeic025:ceic042-index-readiness";
  evidence.family_id = "btree";
  evidence.manifest_generation = 42025;
  evidence.observed_manifest_generation = 42025;
  evidence.present = true;
  evidence.ceic_042_complete = true;
  evidence.freshness_gate_complete = true;
  evidence.route_capability_complete = true;
  evidence.provider_closure_complete = true;
  evidence.metric_producer_complete = true;
  evidence.crash_cleanup_corruption_complete = true;
  evidence.artifact_registration_complete = true;
  return evidence;
}

agents::AgentOptimizerReadinessEvidence OptimizerReadiness() {
  agents::AgentOptimizerReadinessEvidence evidence;
  evidence.evidence_digest = "sha256:aeic025:ceic062-optimizer-readiness";
  evidence.manifest_generation = 62025;
  evidence.observed_manifest_generation = 62025;
  evidence.present = true;
  evidence.ceic_062_complete = true;
  evidence.live_routes_complete = true;
  evidence.benchmark_evidence_complete = true;
  evidence.correctness_oracles_complete = true;
  evidence.crash_reopen_complete = true;
  evidence.metrics_feedback_complete = true;
  evidence.transformation_memo_complete = true;
  evidence.workload_regression_complete = true;
  evidence.driver_explain_complete = true;
  evidence.reference_comparison_complete = true;
  evidence.memory_feedback_complete = true;
  evidence.index_readiness_coupling_complete = true;
  evidence.llvm_memory_accounting_complete = true;
  return evidence;
}

void TestTransactionPressureManager(agents::DurableAgentCatalogImage* catalog) {
  const auto warn = impl::EvaluateTransactionPressureManagerTick(
      StandardHorizonRequest(),
      {Session(2, 150, false)},
      TransactionPressurePolicy());
  Require(warn.ok() && warn.notification_required,
          "AEIC-025 transaction pressure did not warn");
  PersistDecision(catalog,
                  "transaction_pressure_manager",
                  "transaction_pressure.evaluate_long_idle",
                  impl::TransactionPressureManagerDecisionKindName(warn.decision),
                  warn.diagnostic.diagnostic_code,
                  EvidencePairs(warn.evidence));

  const auto restart = impl::EvaluateTransactionPressureManagerTick(
      StandardHorizonRequest(),
      {Session(2, 225, true)},
      TransactionPressurePolicy());
  Require(restart.ok() && restart.restart_requested,
          "AEIC-025 transaction pressure did not request restart");

  auto force = TransactionPressurePolicy();
  force.force_authority_gate_present = true;
  force.force_authority_gate_allows = true;
  force.force_action = impl::TransactionPressureForceAction::rollback;
  force.force_rollback_allowed = true;
  const auto rollback = impl::EvaluateTransactionPressureManagerTick(
      StandardHorizonRequest(),
      {Session(2, 350, true)},
      force);
  Require(rollback.ok() && rollback.replacement_transaction_required &&
              rollback.action_mutates_transaction_if_accepted_by_server,
          "AEIC-025 transaction pressure force replacement proof missing");

  auto bad_session = Session(2, 350, true);
  bad_session.client_state_claimed_authority = true;
  const auto refused = impl::EvaluateTransactionPressureManagerTick(
      StandardHorizonRequest(),
      {bad_session},
      TransactionPressurePolicy());
  Require(!refused.ok() && refused.denied_non_authoritative,
          "AEIC-025 transaction pressure accepted client authority");
}

void TestRuntimeLearningOptimizerConsumption(
    agents::DurableAgentCatalogImage* catalog) {
  impl::RuntimeLearningAgentSnapshot learning;
  learning.query_shape_digest = "aeic025-shape";
  learning.runtime_samples = 8;
  learning.estimate_error_ratio_per_mille = 4000;
  learning.feedback_authoritative = true;
  learning.exact_result_fallback_present = true;
  const auto learned = impl::EvaluateRuntimeLearningAgent(learning);
  Require(learned.ok() &&
              learned.decision ==
                  impl::RuntimeLearningAgentDecisionKind::
                      recommend_planner_correction,
          "AEIC-025 runtime learning did not recommend correction");
  PersistDecision(catalog,
                  "runtime_learning_agent",
                  "runtime_learning.evaluate_feedback",
                  impl::RuntimeLearningAgentDecisionKindName(learned.decision),
                  learned.diagnostic.diagnostic_code,
                  EvidencePairs(learned.evidence));

  opt::OptimizerAgentRecommendationRequest optimizer_request;
  optimizer_request.agent_evidence =
      OptimizerAgentEvidence("runtime_learning_agent", "planner_correction");
  optimizer_request.index_readiness = IndexReadiness();
  optimizer_request.optimizer_readiness = OptimizerReadiness();
  optimizer_request.requested_action =
      agents::AgentIndexOptimizerBoundaryActionKind::
          optimizer_learning_advisory_note;
  optimizer_request.workflow_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(
          "aeic025-runtime-learning-workflow");
  optimizer_request.feedback = OptimizerFeedback();
  const auto consumed =
      opt::EvaluateOptimizerAgentRecommendation(optimizer_request);
  Require(consumed.ok && consumed.benchmark_clean,
          "AEIC-025 optimizer refused durable runtime-learning recommendation");

  optimizer_request.agent_evidence.parser_authority = true;
  const auto unsafe =
      opt::EvaluateOptimizerAgentRecommendation(optimizer_request);
  Require(!unsafe.ok &&
              unsafe.diagnostic_code ==
                  "SB_AGENT_OPTIMIZER_RECOMMENDATION.UNSAFE_AUTHORITY",
          "AEIC-025 optimizer accepted unsafe agent recommendation authority");

  learning.benchmark_authority = true;
  const auto refused = impl::EvaluateRuntimeLearningAgent(learning);
  Require(!refused.ok() && refused.fail_closed &&
              refused.diagnostic.diagnostic_code ==
                  "SB_AGENT_RUNTIME_LEARNING_AUTHORITY_UNTRUSTED",
          "AEIC-025 runtime learning accepted benchmark authority");
}

void TestPolicyRecommendationConsumption(
    agents::DurableAgentCatalogImage* catalog) {
  impl::PolicyRecommendationManagerSnapshot policy;
  policy.policy_family = "admission_control_policy";
  policy.policy_evaluations_total = 25;
  policy.workload_slo_burn_rate_per_mille = 1400;
  policy.policy_metrics_authoritative = true;
  policy.recommendation_target_valid = true;
  const auto recommendation =
      impl::EvaluatePolicyRecommendationManager(policy);
  Require(recommendation.ok() &&
              recommendation.decision ==
                  impl::PolicyRecommendationManagerDecisionKind::
                      create_policy_recommendation,
          "AEIC-025 policy recommendation was not created");
  PersistDecision(catalog,
                  "policy_recommendation_manager",
                  "policy_recommendation.evaluate",
                  impl::PolicyRecommendationManagerDecisionKindName(
                      recommendation.decision),
                  recommendation.diagnostic.diagnostic_code,
                  EvidencePairs(recommendation.evidence));

  agents::AgentPolicyRecommendationApplicationRequest application;
  application.recommendation_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic025-policy-rec");
  application.evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic025-policy-evidence");
  application.policy_family = "admission_control_policy";
  application.scope_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic025-scope");
  application.metric_digest = "sha256:aeic025-policy-metric";
  application.proposed_field_name = "scheduler_queue_threshold";
  application.proposed_field_value = "4096";
  application.policy_generation = 25;
  application.observed_policy_generation = 25;
  application.durable_catalog_state = true;
  application.strict_metric_snapshot = true;
  application.metric_trusted = true;
  application.metric_fresh = true;
  const auto accepted =
      agents::EvaluateAgentPolicyRecommendationApplication(application);
  Require(accepted.ok && accepted.recommendation_record_created &&
              accepted.schema_validated && accepted.auto_apply_blocked,
          "AEIC-025 policy recommendation was not consumed as pending review");
  PersistDecision(catalog,
                  "policy_recommendation_manager",
                  "policy_recommendation.apply_schema_guard",
                  agents::AgentPolicyRecommendationApplicationDecisionName(
                      accepted.decision),
                  accepted.status.diagnostic_code,
                  EvidencePairsFromStrings(accepted.evidence));

  application.no_auto_apply_required = false;
  const auto auto_apply =
      agents::EvaluateAgentPolicyRecommendationApplication(application);
  Require(!auto_apply.ok &&
              auto_apply.status.diagnostic_code ==
                  "SB_AGENT_POLICY_RECOMMENDATION_APPLICATION.AUTO_APPLY_REFUSED",
          "AEIC-025 policy recommendation accepted auto-apply");

  application.no_auto_apply_required = true;
  application.proposed_field_value = "not-a-number";
  const auto bad_schema =
      agents::EvaluateAgentPolicyRecommendationApplication(application);
  Require(!bad_schema.ok &&
              bad_schema.status.diagnostic_code ==
                  "SB_AGENT_POLICY_RECOMMENDATION_APPLICATION.SCHEMA_REFUSED",
          "AEIC-025 policy recommendation accepted invalid schema value");

  policy.parser_authority = true;
  const auto refused = impl::EvaluatePolicyRecommendationManager(policy);
  Require(!refused.ok() && refused.fail_closed &&
              refused.diagnostic.diagnostic_code ==
                  "SB_AGENT_POLICY_RECOMMENDATION_AUTHORITY_UNTRUSTED",
          "AEIC-025 policy recommendation accepted parser authority");
}

}  // namespace

int main() {
  auto catalog = DurableCatalog();
  TestTransactionPressureManager(&catalog);
  TestRuntimeLearningOptimizerConsumption(&catalog);
  TestPolicyRecommendationConsumption(&catalog);
  Require(catalog.evidence.size() == 4,
          "AEIC-025 decision evidence count mismatch");
  Require(catalog.actions.size() == 4,
          "AEIC-025 action count mismatch");
  Require(catalog.retained_history.size() == 4,
          "AEIC-025 retained history count mismatch");
  Require(agents::ValidateDurableAgentCatalogForProduction(catalog).ok,
          "AEIC-025 durable catalog invalid after evidence writes");
  std::cout
      << "AEIC025_TRANSACTION_PRESSURE "
      << "AEIC025_RUNTIME_LEARNING_OPTIMIZER "
      << "AEIC025_POLICY_RECOMMENDATION_APPLICATION ok\n";
  return EXIT_SUCCESS;
}
