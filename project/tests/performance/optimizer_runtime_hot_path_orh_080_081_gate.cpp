// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "adaptive_tuning_controller.hpp"
#include "adaptive_tuning_metrics_evidence.hpp"
#include "agent_runtime.hpp"
#include "resource_governance_admission.hpp"
#include "vector_index_generation_publication.hpp"
#include "vector_maintenance_jobs.hpp"
#include "vector_training_recall_lifecycle.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace idx = scratchbird::core::index;
namespace metrics = scratchbird::core::metrics;

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ORH-080/081 gate failure: " << message << '\n';
    std::exit(1);
  }
}

bool Has(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasPrefix(const std::vector<std::string>& values,
               const std::string& prefix) {
  return std::any_of(values.begin(), values.end(),
                     [&](const std::string& item) {
                       return item.rfind(prefix, 0) == 0;
                     });
}

std::string ObjectUuid(const std::string& key) {
  return agents::DeterministicAgentRuntimeObjectUuidFromKey("orh080|" + key);
}

std::string DatabaseUuid() {
  return agents::DeterministicAgentRuntimeDatabaseUuidFromKey("orh080|db");
}

idx::VectorTrainingRecallLifecycleProfile BaseProfile(
    idx::IndexVectorAlgorithm algorithm = idx::IndexVectorAlgorithm::hnsw) {
  return idx::DefaultVectorTrainingRecallLifecycleProfile(algorithm);
}

agents::VectorMaintenanceJobRequest JobRequest(
    const std::string& key,
    agents::VectorMaintenanceActionKind action,
    const idx::VectorTrainingRecallLifecycleDecision& decision) {
  agents::VectorMaintenanceJobRequest request;
  request.job_uuid = ObjectUuid(key + "|job");
  request.database_uuid = DatabaseUuid();
  request.target_collection_uuid = ObjectUuid(key + "|collection");
  request.target_index_uuid = ObjectUuid(key + "|index");
  request.provider_generation = 77;
  request.old_training_generation = 40;
  request.new_training_generation = 41;
  request.action_kind = action;
  request.lifecycle_decision = decision;
  request.total_units = 10;
  request.now_microseconds = 1000;
  return request;
}

agents::AdaptiveTuningSafetyPolicy SafeTuningPolicy() {
  agents::AdaptiveTuningSafetyPolicy safety;
  safety.policy_allowed = true;
  safety.semantics_neutral_proof = true;
  safety.advisory_resource_governance_only = true;
  safety.engine_mga_authoritative = true;
  safety.security_snapshot_bound = true;
  safety.grants_proven = true;
  return safety;
}

agents::ResourceGovernanceAdmissionRequest GovernanceRequest(
    const std::string& operation_id) {
  agents::ResourceGovernanceAdmissionRequest request;
  request.operation_id = operation_id;
  request.expected_family = agents::ResourceGovernanceFamily::kAdaptiveTuningKnob;
  request.descriptor.descriptor_id = operation_id + "|quota";
  request.descriptor.family = agents::ResourceGovernanceFamily::kAdaptiveTuningKnob;
  request.descriptor.source =
      agents::ResourceGovernanceDescriptorSource::kAgentRuntime;
  request.descriptor.source_path_or_label = "agent_runtime";
  request.descriptor.descriptor_generation = 8;
  request.descriptor.expected_generation = 8;
  request.descriptor.benchmark_clean = true;
  request.descriptor.runtime_dependency_present = true;
  request.descriptor.limits.memory_bytes = 1024;
  request.descriptor.limits.device_memory_bytes = 1;
  request.descriptor.limits.pinned_memory_bytes = 1;
  request.descriptor.limits.io_bytes = 1024;
  request.descriptor.limits.io_ops = 16;
  request.descriptor.limits.worker_threads = 1;
  request.descriptor.limits.backlog_items = 16;
  request.descriptor.limits.candidate_rows = 1024;
  request.descriptor.limits.cache_entries = 16;
  request.descriptor.limits.batch_rows = 16;
  request.descriptor.limits.fragments = 1;
  request.descriptor.limits.lanes = 1;
  request.descriptor.limits.time_budget_microseconds = 5000;
  request.requested.memory_bytes = 16;
  request.requested.device_memory_bytes = 1;
  request.requested.pinned_memory_bytes = 1;
  request.requested.io_bytes = 16;
  request.requested.io_ops = 1;
  request.requested.worker_threads = 1;
  request.requested.backlog_items = 1;
  request.requested.candidate_rows = 32;
  request.requested.cache_entries = 1;
  request.requested.batch_rows = 1;
  request.requested.fragments = 1;
  request.requested.lanes = 1;
  request.requested.time_budget_microseconds = 1000;
  return request;
}

metrics::AdaptiveTuningMetricEvidence MetricsFor(
    const std::string& knob_label) {
  metrics::AdaptiveTuningMetricEvidenceRequest request;
  request.knob_label = knob_label;
  request.metric_scope_label = "vector_lifecycle";
  request.evidence_epoch = 10;
  request.required_epoch = 10;
  request.evidence_age_microseconds = 1000;
  request.observed_latency_microseconds = 1000;
  request.latency_budget_microseconds = 2000;
  request.observed_memory_bytes = 512;
  request.memory_budget_bytes = 1024;
  request.backlog_units = 8;
  request.backlog_budget_units = 4;
  request.throughput_units_per_second = 100;
  return metrics::BuildAdaptiveTuningMetricEvidence(request);
}

void ProveLifecycleTriggersAndAdaptiveBeforeRebuild() {
  auto adaptive_profile = BaseProfile();
  adaptive_profile.drift.p95_latency_microseconds = 4000;
  adaptive_profile.drift.policy_p95_latency_microseconds = 2000;
  adaptive_profile.drift.tombstone_ratio = 0.12;
  adaptive_profile.drift.hnsw_degree_imbalance_ratio = 1.50;
  adaptive_profile.drift.adaptive_tuning_expected_sufficient = true;
  adaptive_profile.drift.current_ef_search = 80;
  adaptive_profile.drift.tuned_ef_search = 120;
  adaptive_profile.drift.max_ef_search = 256;
  const auto adaptive =
      idx::EvaluateVectorTrainingRecallLifecycle(adaptive_profile);
  Require(adaptive.accepted, "adaptive trigger decision was not accepted");
  Require(adaptive.action ==
              idx::VectorTrainingRecallLifecycleAction::kScheduleAdaptiveTuning,
          "latency/degree trigger did not prefer adaptive tuning first");
  Require(Has(adaptive.evidence,
              "vector_lifecycle_action=schedule_adaptive_tuning"),
          "adaptive action evidence missing");
  Require(HasPrefix(adaptive.evidence, "vector_p95_latency_microseconds="),
          "p95 latency trigger evidence missing");
  Require(HasPrefix(adaptive.evidence, "hnsw_degree_imbalance_ratio="),
          "degree imbalance trigger evidence missing");
  Require(Has(adaptive.evidence,
              "vector_runtime_correctness_blocker="
              "SB_ORH_VECTOR_INDEX_RUNTIME_CORRECTNESS_UNPROVEN"),
          "runtime correctness blocker missing");

  auto rebuild_profile = BaseProfile();
  rebuild_profile.drift.deleted_vector_count = 350;
  rebuild_profile.drift.live_vector_count = 650;
  rebuild_profile.drift.tombstone_ratio = 0.35;
  rebuild_profile.drift.max_tombstone_ratio = 0.20;
  rebuild_profile.drift.hnsw_graph_age_generations = 12;
  rebuild_profile.drift.max_hnsw_graph_age_generations = 8;
  rebuild_profile.drift.p95_latency_microseconds = 6000;
  rebuild_profile.drift.policy_p95_latency_microseconds = 2000;
  rebuild_profile.drift.adaptive_tuning_expected_sufficient = false;
  const auto rebuild =
      idx::EvaluateVectorTrainingRecallLifecycle(rebuild_profile);
  Require(rebuild.action ==
              idx::VectorTrainingRecallLifecycleAction::kScheduleRebuild,
          "tombstone/latency drift did not escalate to rebuild");
  Require(HasPrefix(rebuild.evidence, "tombstone_ratio="),
          "tombstone ratio evidence missing");
  Require(HasPrefix(rebuild.evidence, "effective_tombstone_ratio="),
          "effective tombstone ratio evidence missing");
  Require(HasPrefix(rebuild.evidence, "hnsw_graph_age_generations="),
          "HNSW graph age evidence missing");

  auto counted_tombstone_profile = BaseProfile();
  counted_tombstone_profile.drift.deleted_vector_count = 30;
  counted_tombstone_profile.drift.live_vector_count = 70;
  counted_tombstone_profile.drift.tombstone_ratio = 0.0;
  counted_tombstone_profile.drift.max_tombstone_ratio = 0.20;
  counted_tombstone_profile.drift.adaptive_tuning_expected_sufficient = false;
  const auto counted_tombstone =
      idx::EvaluateVectorTrainingRecallLifecycle(counted_tombstone_profile);
  Require(counted_tombstone.action ==
              idx::VectorTrainingRecallLifecycleAction::kScheduleRebuild,
          "deleted/live counts did not derive rebuild tombstone ratio");

  auto retrain_profile = BaseProfile(idx::IndexVectorAlgorithm::ivf_pq);
  retrain_profile.recall_probe.observed_ann_recall = 0.88;
  retrain_profile.drift.observed_recall_drift = 0.08;
  const auto retrain =
      idx::EvaluateVectorTrainingRecallLifecycle(retrain_profile);
  Require(retrain.action ==
              idx::VectorTrainingRecallLifecycleAction::kScheduleRetrain,
          "recall drift did not schedule retrain");
  Require(HasPrefix(retrain.evidence, "drift_observed_recall="),
          "recall drift evidence missing");

  auto retrain_over_tuning = BaseProfile();
  retrain_over_tuning.recall_probe.observed_ann_recall = 0.88;
  retrain_over_tuning.drift.p95_latency_microseconds = 4000;
  retrain_over_tuning.drift.policy_p95_latency_microseconds = 2000;
  retrain_over_tuning.drift.adaptive_tuning_expected_sufficient = true;
  retrain_over_tuning.drift.current_ef_search = 80;
  retrain_over_tuning.drift.tuned_ef_search = 120;
  retrain_over_tuning.drift.max_ef_search = 256;
  const auto retrain_priority =
      idx::EvaluateVectorTrainingRecallLifecycle(retrain_over_tuning);
  Require(retrain_priority.action ==
              idx::VectorTrainingRecallLifecycleAction::kScheduleRetrain,
          "recall drift was masked by adaptive tuning");
}

void ProveVectorAdaptiveKnobs() {
  for (const auto knob : {agents::AdaptiveTuningKnob::kVectorEfSearch,
                         agents::AdaptiveTuningKnob::kVectorNprobe}) {
    const std::string label = agents::AdaptiveTuningKnobName(knob);
    auto request = agents::BuildAdaptiveTuningAgentRequest(
        knob, 1, 256, 16, 32, SafeTuningPolicy(), MetricsFor(label));
    request.resource_governance = GovernanceRequest(label);
    const auto result = agents::EvaluateAdaptiveTuningController(request);
    Require(result.ok, label + " adaptive tuning was refused");
    Require(result.action == agents::AdaptiveTuningActionClass::kIncrease,
            label + " did not tune upward under safe backlog evidence");
    Require(Has(result.evidence, "knob=" + label),
            label + " knob evidence missing");
    Require(Has(result.evidence, "advisory_only=true"),
            label + " advisory-only evidence missing");
  }
}

void ProveMaintenanceJobLifecycle() {
  auto adaptive_profile = BaseProfile();
  adaptive_profile.drift.p95_latency_microseconds = 4000;
  adaptive_profile.drift.policy_p95_latency_microseconds = 2000;
  adaptive_profile.drift.adaptive_tuning_expected_sufficient = true;
  adaptive_profile.drift.current_ef_search = 80;
  adaptive_profile.drift.tuned_ef_search = 120;
  adaptive_profile.drift.max_ef_search = 256;
  const auto adaptive_decision =
      idx::EvaluateVectorTrainingRecallLifecycle(adaptive_profile);

  auto retrain_profile = BaseProfile(idx::IndexVectorAlgorithm::ivf_pq);
  retrain_profile.recall_probe.observed_ann_recall = 0.88;
  const auto retrain_decision =
      idx::EvaluateVectorTrainingRecallLifecycle(retrain_profile);

  auto rebuild_profile = BaseProfile();
  rebuild_profile.drift.tombstone_ratio = 0.40;
  rebuild_profile.drift.max_tombstone_ratio = 0.20;
  rebuild_profile.drift.adaptive_tuning_expected_sufficient = false;
  const auto rebuild_decision =
      idx::EvaluateVectorTrainingRecallLifecycle(rebuild_profile);

  agents::VectorMaintenanceJobStore store;
  auto adaptive = agents::CreateVectorMaintenanceJob(
      &store,
      JobRequest("adaptive",
                 agents::VectorMaintenanceActionKind::adaptive_tuning,
                 adaptive_decision));
  Require(adaptive.ok(), "adaptive tuning maintenance job was not created");
  Require(Has(adaptive.record.evidence,
              "vector_maintenance_action=adaptive_tuning"),
          "adaptive job action evidence missing");
  Require(Has(adaptive.record.evidence,
              "lifecycle.vector_lifecycle_action=schedule_adaptive_tuning"),
          "adaptive job lifecycle action evidence missing");

  auto retrain = agents::CreateVectorMaintenanceJob(
      &store,
      JobRequest("retrain",
                 agents::VectorMaintenanceActionKind::retrain,
                 retrain_decision));
  Require(retrain.ok(), "retrain maintenance job was not created");
  auto retry = agents::ClassifyVectorMaintenanceFailure(
      &store,
      retrain.record.job_uuid,
      agents::VectorMaintenanceFailureClass::transient_provider_unavailable,
      true,
      2000);
  Require(retry.ok(), "transient failure was not retry-scheduled");
  Require(retry.record.retry_state ==
              agents::VectorMaintenanceRetryState::retry_scheduled,
          "retry state mismatch");
  Require(Has(retry.record.evidence,
              "failure_class=transient_provider_unavailable"),
          "transient failure evidence missing");

  auto rebuild = agents::CreateVectorMaintenanceJob(
      &store,
      JobRequest("rebuild",
                 agents::VectorMaintenanceActionKind::rebuild,
                 rebuild_decision));
  Require(rebuild.ok(), "rebuild maintenance job was not created");
  Require(rebuild.record.provider_generation == 77,
          "provider generation was not recorded");
  Require(rebuild.record.old_training_generation == 40,
          "old training generation was not recorded");
  Require(rebuild.record.new_training_generation == 41,
          "new training generation was not recorded");
  Require(rebuild.record.progress.snapshot.phase ==
              agents::OnlineMaintenancePhase::running,
          "created job did not start running progress");

  auto progress = agents::RecordVectorMaintenanceProgress(
      &store, rebuild.record.job_uuid, 4, 10, "scan_tombstones", 3000);
  Require(progress.ok(), "maintenance progress failed");
  Require(progress.record.progress.snapshot.completed_units == 4,
          "progress units not recorded");

  auto cancelled = agents::CancelVectorMaintenanceJob(
      &store, rebuild.record.job_uuid, "operator_cancel", 4000);
  Require(cancelled.ok(), "maintenance cancel failed");
  Require(cancelled.record.progress.snapshot.resumable,
          "cancelled maintenance was not resumable");

  auto resumed =
      agents::ResumeVectorMaintenanceJob(&store, rebuild.record.job_uuid, 5000);
  Require(resumed.ok(), "maintenance resume failed");
  Require(resumed.record.progress.snapshot.phase ==
              agents::OnlineMaintenancePhase::running,
          "resume did not return to running");

  auto validation_ready = agents::MarkVectorMaintenanceValidationReady(
      &store, rebuild.record.job_uuid, 6000);
  Require(validation_ready.ok(), "validation-ready transition failed");
  Require(validation_ready.record.publish_state ==
              agents::VectorMaintenancePublishState::publish_after_validation,
          "publish-after-validation state missing");
  Require(validation_ready.record.progress.snapshot.publish_ready,
          "online maintenance did not become publish-ready");

  auto published = agents::PublishVectorMaintenanceAfterValidation(
      &store, rebuild.record.job_uuid, 7000);
  Require(published.ok(), "publish-after-validation failed");
  Require(published.record.publish_state ==
              agents::VectorMaintenancePublishState::published,
          "published state missing");
  Require(published.record.progress.snapshot.published_visible,
          "validated maintenance publish did not become visible");
  Require(Has(published.record.evidence,
              "runtime_correctness_blocker="
              "SB_ORH_VECTOR_INDEX_RUNTIME_CORRECTNESS_UNPROVEN"),
          "published maintenance record dropped runtime blocker");
  Require(Has(published.record.evidence,
              "exact_rerank_final_scoring_authority=true"),
          "exact rerank authority evidence missing");
  Require(Has(published.record.evidence, "ann_visibility_authority=false"),
          "ANN visibility authority refusal missing");
  Require(Has(published.record.evidence, "ann_finality_authority=false"),
          "ANN finality authority refusal missing");

  auto permanent = agents::ClassifyVectorMaintenanceFailure(
      &store,
      adaptive.record.job_uuid,
      agents::VectorMaintenanceFailureClass::permanent_validation_failed,
      false,
      8000);
  Require(!permanent.ok(), "permanent validation failure did not fail closed");
  Require(permanent.record.failure_class ==
              agents::VectorMaintenanceFailureClass::permanent_validation_failed,
          "permanent failure class not recorded");
}

void ProveExactFallbackFinalAuthority() {
  idx::VectorGenerationAccessRequest access;
  access.vector_generation_enabled = true;
  access.exact_vector_scan_fallback_available = true;
  access.base_row_mga_recheck_required = true;
  access.base_row_security_recheck_required = true;
  const auto plan = idx::PlanVectorGenerationAccess(access);
  Require(!plan.ann_generation_selected,
          "empty ANN generation unexpectedly selected");
  Require(plan.exact_vector_scan_fallback_selected,
          "exact vector fallback was not selected");
  Require(!plan.generation_metadata_visibility_authority,
          "generation metadata became visibility authority");
  Require(!plan.generation_metadata_finality_authority,
          "generation metadata became finality authority");
}

}  // namespace

int main() {
  ProveLifecycleTriggersAndAdaptiveBeforeRebuild();
  ProveVectorAdaptiveKnobs();
  ProveMaintenanceJobLifecycle();
  ProveExactFallbackFinalAuthority();
  return 0;
}
