// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/cleanup_archive_manager.hpp"
#include "agents/index_health_manager.hpp"
#include "agents/parser_interface_manager.hpp"
#include "agents/policy_recommendation_manager.hpp"
#include "agents/runtime_learning_agent.hpp"
#include "agents/support_bundle_triage_agent.hpp"
#include "agent_durable_catalog.hpp"
#include "agent_enterprise_evidence.hpp"
#include "agent_production_classification.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

namespace {

namespace agents = scratchbird::core::agents;
namespace impl = scratchbird::core::agents::implemented_agents;

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
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic-advisory-mga");
  image.authority.transaction_generation = 12;
  image.authority.evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic-advisory-open");
  image.authority.database_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic-advisory-db");
  image.authority.catalog_storage_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic-advisory-storage");
  image.authority.storage_commit_evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic-advisory-commit");
  image.authority.catalog_generation = 1;
  image.authority.local_transaction_id = 77;
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
    snapshot.generation = 9;
    snapshot.observed_wall_microseconds = observed_wall_microseconds;
    snapshot.scope_uuid = scope_uuid;
    snapshot.digest = "sha256:aeic-advisory:" + dependency.metric_family;
    snapshot.source_quality = agents::AgentMetricSourceQuality::trusted;
    snapshot.present = true;
    snapshot.trusted = true;
    snapshot.schema_compatible = true;
    snapshot.trust_provenance = "test_metric_registry";
    snapshot.evidence_uuid =
        agents::DeterministicAgentRuntimeObjectUuidFromKey(
            "aeic-advisory-metric-evidence|" + dependency.metric_family);
    snapshot.snapshot_id = "aeic-advisory:" + dependency.metric_family;
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
      agents::DeterministicAgentRuntimePrincipalUuidFromKey("aeic-advisory-principal");
  request.rights_used = {"agent.observe", "agent.recommend"};
  request.scope_uuids = {
      agents::DeterministicAgentRuntimeObjectUuidFromKey("aeic-advisory-scope")};
  request.policy_generation = 12;
  request.decision_kind = decision_kind;
  request.result_state = "advisory_completed";
  request.diagnostic_code = diagnostic_code;
  request.decision_fields = fields;
  request.outcome_verification_evidence_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey(agent_type_id + "-verification");
  request.created_at_microseconds = before_generation + 200;
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
          "enterprise advisory evidence not fully durable");
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

void TestIndexHealthManager(agents::DurableAgentCatalogImage* catalog) {
  impl::IndexHealthManagerSnapshot snapshot;
  snapshot.index_uuid = "index-1";
  snapshot.index_metrics_authoritative = true;
  snapshot.filespace_metrics_authoritative = true;
  snapshot.read_amplification_ratio = 8;
  auto result = impl::EvaluateIndexHealthManager(snapshot);
  Require(result.ok() &&
              result.decision ==
                  impl::IndexHealthManagerDecisionKind::recommend_index_rebuild,
          "index health did not recommend rebuild for high amplification");
  PersistDecision(catalog,
                  "index_health_manager",
                  "index_health.evaluate",
                  impl::IndexHealthManagerDecisionKindName(result.decision),
                  result.diagnostic.diagnostic_code,
                  EvidencePairs(result.evidence));

  snapshot.parser_authority = true;
  auto refused = impl::EvaluateIndexHealthManager(snapshot);
  Require(!refused.ok() &&
              refused.diagnostic.diagnostic_code ==
                  "SB_AGENT_INDEX_HEALTH_AUTHORITY_UNTRUSTED",
          "index health accepted parser authority");
}

void TestCleanupArchiveManager(agents::DurableAgentCatalogImage* catalog) {
  impl::CleanupArchiveManagerSnapshot snapshot;
  snapshot.cleanup_horizon_authoritative = true;
  snapshot.authoritative_cleanup_horizon = 5000;
  snapshot.current_cleanup_lwm = 1000;
  auto result = impl::EvaluateCleanupArchiveManager(snapshot);
  Require(result.ok() &&
              result.decision ==
                  impl::CleanupArchiveManagerDecisionKind::advance_cleanup_lwm,
          "cleanup archive did not advance safe LWM");
  Require(result.proposed_cleanup_lwm > snapshot.current_cleanup_lwm,
          "cleanup archive proposed no LWM movement");
  PersistDecision(catalog,
                  "cleanup_archive_manager",
                  "cleanup_archive.evaluate",
                  impl::CleanupArchiveManagerDecisionKindName(result.decision),
                  result.diagnostic.diagnostic_code,
                  EvidencePairs(result.evidence));

  snapshot.legal_hold_active = true;
  auto held = impl::EvaluateCleanupArchiveManager(snapshot);
  Require(held.ok() &&
              held.decision == impl::CleanupArchiveManagerDecisionKind::no_action,
          "cleanup archive ignored legal hold");
}

void TestRuntimeLearningAndPolicyRecommendation(
    agents::DurableAgentCatalogImage* catalog) {
  impl::RuntimeLearningAgentSnapshot learning;
  learning.query_shape_digest = "shape-a";
  learning.runtime_samples = 8;
  learning.estimate_error_ratio_per_mille = 4000;
  learning.feedback_authoritative = true;
  learning.exact_result_fallback_present = true;
  auto learned = impl::EvaluateRuntimeLearningAgent(learning);
  Require(learned.ok() &&
              learned.decision ==
                  impl::RuntimeLearningAgentDecisionKind::recommend_planner_correction,
          "runtime learning did not recommend planner correction");
  PersistDecision(catalog,
                  "runtime_learning_agent",
                  "runtime_learning.evaluate_feedback",
                  impl::RuntimeLearningAgentDecisionKindName(learned.decision),
                  learned.diagnostic.diagnostic_code,
                  EvidencePairs(learned.evidence));

  learning.benchmark_authority = true;
  auto refused = impl::EvaluateRuntimeLearningAgent(learning);
  Require(!refused.ok() &&
              refused.diagnostic.diagnostic_code ==
                  "SB_AGENT_RUNTIME_LEARNING_AUTHORITY_UNTRUSTED",
          "runtime learning accepted benchmark authority");

  impl::PolicyRecommendationManagerSnapshot policy;
  policy.policy_family = "admission_control_policy";
  policy.policy_evaluations_total = 25;
  policy.workload_slo_burn_rate_per_mille = 1400;
  policy.policy_metrics_authoritative = true;
  policy.recommendation_target_valid = true;
  auto recommendation = impl::EvaluatePolicyRecommendationManager(policy);
  Require(recommendation.ok() &&
              recommendation.decision ==
                  impl::PolicyRecommendationManagerDecisionKind::
                      create_policy_recommendation,
          "policy recommendation manager did not recommend policy change");
  PersistDecision(catalog,
                  "policy_recommendation_manager",
                  "policy_recommendation.evaluate",
                  impl::PolicyRecommendationManagerDecisionKindName(
                      recommendation.decision),
                  recommendation.diagnostic.diagnostic_code,
                  EvidencePairs(recommendation.evidence));
}

void TestParserAndSupportAgents(agents::DurableAgentCatalogImage* catalog) {
  impl::ParserInterfaceManagerSnapshot parser;
  parser.parser_family = "firebird";
  parser.parser_metrics_authoritative = true;
  parser.parser_crashes_total = 5;
  auto drain = impl::EvaluateParserInterfaceManager(parser);
  Require(drain.ok() &&
              drain.decision ==
                  impl::ParserInterfaceManagerDecisionKind::drain_parser_family,
          "parser interface did not drain unhealthy parser family");
  PersistDecision(catalog,
                  "parser_interface_manager",
                  "parser_interface.evaluate_family",
                  impl::ParserInterfaceManagerDecisionKindName(drain.decision),
                  drain.diagnostic.diagnostic_code,
                  EvidencePairs(drain.evidence));

  parser.parser_finality_authority = true;
  auto refused = impl::EvaluateParserInterfaceManager(parser);
  Require(!refused.ok() &&
              refused.diagnostic.diagnostic_code ==
                  "SB_AGENT_PARSER_INTERFACE_AUTHORITY_UNTRUSTED",
          "parser interface accepted parser finality authority");

  impl::SupportBundleTriageSnapshot support;
  support.completeness_ratio_per_mille = 950;
  support.agent_actions_total = 7;
  support.evidence_catalog_authoritative = true;
  support.tamper_evidence_valid = true;
  support.redaction_policy_valid = true;
  support.protected_material_present = true;
  support.support_bundle_sink_available = true;
  auto bundle = impl::EvaluateSupportBundleTriage(support);
  Require(bundle.ok() &&
              bundle.decision ==
                  impl::SupportBundleTriageDecisionKind::prepare_redacted_bundle,
          "support triage did not prepare redacted bundle");
  Require(bundle.protected_material_suppressed,
          "support triage failed to suppress protected material");
  PersistDecision(catalog,
                  "support_bundle_triage_agent",
                  "support_bundle_triage.evaluate_bundle",
                  impl::SupportBundleTriageDecisionKindName(bundle.decision),
                  bundle.diagnostic.diagnostic_code,
                  EvidencePairs(bundle.evidence));
}

void TestProductionClassificationNoLongerAnchorOnly() {
  const auto by_agent = ExposureByAgent();
  for (const std::string agent : {
           "index_health_manager",
           "cleanup_archive_manager",
           "runtime_learning_agent",
           "policy_recommendation_manager",
           "parser_interface_manager",
           "support_bundle_triage_agent"}) {
    const auto found = by_agent.find(agent);
    Require(found != by_agent.end(), "missing exposure record: " + agent);
    Require(!found->second.implementation_anchor_only,
            "advisory handler still classified anchor-only: " + agent);
    Require(!found->second.route_evidence_kind.empty(),
            "advisory handler lacks route evidence classification: " + agent);
    Require(!found->second.production_live_route_available,
            "advisory handler exposed live mutation by default: " + agent);
  }
}

}  // namespace

int main() {
  auto catalog = DurableCatalog();
  TestIndexHealthManager(&catalog);
  TestCleanupArchiveManager(&catalog);
  TestRuntimeLearningAndPolicyRecommendation(&catalog);
  TestParserAndSupportAgents(&catalog);
  Require(catalog.evidence.size() == 6, "advisory evidence count mismatch");
  Require(catalog.actions.size() == 6, "advisory action count mismatch");
  Require(catalog.retained_history.size() == 6, "advisory history count mismatch");
  Require(agents::ValidateDurableAgentCatalogForProduction(catalog).ok,
          "advisory durable catalog invalid after evidence writes");
  TestProductionClassificationNoLongerAnchorOnly();
  return EXIT_SUCCESS;
}
