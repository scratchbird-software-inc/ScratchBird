// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "access_path.hpp"
#include "cost_model.hpp"
#include "logical_plan.hpp"
#include "optimizer_explain.hpp"
#include "optimizer_feedback.hpp"
#include "optimizer_feedback_stability.hpp"
#include "optimizer_hint_policy.hpp"
#include "optimizer_request.hpp"
#include "optimizer_statistics_lifecycle.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), std::string(expected)) != values.end();
}

bool ContainsText(const std::vector<std::string>& values, std::string_view expected) {
  return std::any_of(values.begin(), values.end(), [&](const auto& value) {
    return value.find(expected) != std::string::npos;
  });
}

bool ContainsText(std::string_view value, std::string_view expected) {
  return value.find(expected) != std::string_view::npos;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string Id(std::string_view suffix) {
  return "pcr062." + std::string(suffix);
}

opt::OptimizerHintPolicyRequest HintPolicyRequest() {
  opt::OptimizerHintPolicyRequest request;
  request.policy_uuid = Id("hint_policy.join");
  request.request_uuid = Id("request.lookup");
  request.operation_id = Id("operation.lookup");
  request.sblr_digest = "sha256:sblr-pcr062-lookup";
  request.descriptor_set_digest = "sha256:descriptor-pcr062-customer";
  request.route_capability_digest = "sha256:route-capability-pcr062-local";
  request.security_policy_digest = "sha256:security-policy-pcr062-reader";
  request.redaction_policy_digest = "sha256:redaction-policy-pcr062";
  request.memory_policy_digest = "sha256:memory-policy-pcr062";
  request.cost_profile_id = "cost-profile:pcr062-catalog";
  request.policy_source_kind = "sblr_api";
  request.normalized_hint_tokens = {
      "hint:join_order=preserve",
      "hint:index_preference=pcr062_customer_idx"};
  request.policy_epoch = 6201;
  request.catalog_epoch = 6202;
  request.stats_epoch = 6203;
  request.security_epoch = 6204;
  request.redaction_epoch = 6205;
  request.name_resolution_epoch = 6206;
  request.resource_epoch = 6207;
  request.memory_policy_epoch = 6208;
  request.memory_feedback_generation = 6209;
  request.route_epoch = 6210;
  request.security_context_present = true;
  request.transaction_context_present = true;
  request.grants_proven = true;
  request.mga_visibility_recheck_required = true;
  request.exact_recheck_required = true;
  request.security_recheck_required = true;
  request.redaction_policy_bound = true;
  request.catalog_descriptor_bound = true;
  return request;
}

opt::OptimizerRuntimeFeedback RuntimeFeedback() {
  opt::OptimizerRuntimeFeedback feedback;
  feedback.operator_family = "btree_lookup";
  feedback.plan_shape = "index_lookup_with_feedback";
  feedback.cost_profile_id = "pcr062_cost_profile";
  feedback.estimated_rows = 10;
  feedback.actual_rows = 1000;
  feedback.actual_rows_examined = 1200;
  feedback.actual_rows_filtered = 200;
  feedback.loop_count = 3;
  feedback.estimated_pages = 4;
  feedback.actual_pages = 80;
  feedback.estimated_io_operations = 4;
  feedback.actual_io_operations = 96;
  feedback.estimated_visibility_recheck_rows = 10;
  feedback.actual_visibility_recheck_rows = 1000;
  feedback.estimated_spill_bytes = 0;
  feedback.actual_spill_bytes = 12288;
  feedback.memory_grant_bytes = 64 * 1024;
  feedback.peak_memory_bytes = 256 * 1024;
  feedback.estimated_latency_microseconds = 100;
  feedback.actual_latency_microseconds = 1800;
  feedback.estimated_resource_units = 10;
  feedback.actual_resource_units = 800;
  feedback.freshness_microseconds = 50;
  feedback.max_freshness_microseconds = 60000000;
  feedback.advisory_only = true;
  feedback.mga_visibility_recheck_preserved = true;
  feedback.parser_or_reference_authority = false;
  feedback.transaction_finality_authority = "engine_transaction_inventory";
  return feedback;
}

opt::OptimizerStatisticsLifecycleRequest StatisticsLifecycleRequest() {
  opt::OptimizerStatisticsLifecycleRequest request;
  request.trigger = opt::OptimizerStatisticsLifecycleTrigger::kAgentAutoMaintenance;
  request.relation_uuid = Id("relation.customer");
  request.column_uuids = {Id("column.customer_id"), Id("column.region")};
  request.current_stats_epoch = 6203;
  request.request_stats_epoch = 6204;
  request.catalog_epoch = 6205;
  request.security_epoch = 6206;
  request.policy_epoch = 6207;
  request.stats_visibility_epoch = 6208;
  request.current_freshness = opt::OptimizerStatsFreshnessState::kStale;
  request.sampled_rows = 10000;
  request.total_rows_estimate = 250000;
  request.page_count = 2048;
  request.average_row_bytes = 96;
  request.rows_modified_since_stats = 75000;
  request.bulk_rows_written = 10000;
  request.stale_row_threshold = 1000;
  request.histogram_bucket_target = 128;
  request.mcv_entry_target = 64;
  request.policy_enabled = true;
  request.security_context_present = true;
  request.grants_proven = true;
  request.mga_visibility_recheck_present = true;
  request.security_recheck_present = true;
  request.epoch_evidence_present = true;
  request.advisory_only = true;
  request.parser_or_reference_authority = false;
  request.agent_policy_safe = true;
  request.catalog_descriptor_present = true;
  request.catalog_write_admitted = true;
  request.agent_runtime_registered = true;
  request.agent_schedule_admitted = true;
  return request;
}

plan::OptimizerPolicyMetadata SafePolicy() {
  plan::OptimizerPolicyMetadata policy;
  policy.optimizer_policy_metadata_present = true;
  policy.policy_source_kind = "sblr_api";
  policy.policy_epoch = 6201;
  policy.normalized_controls.plan_profile_id = "plan_profile:pcr062_feedback";
  policy.normalized_controls.join_search_policy_id = "join_search:pcr062_bounded";
  policy.normalized_controls.memory_policy_id = "memory_policy:pcr062_governed";
  policy.normalized_controls.spill_policy_id = "spill_policy:pcr062_bounded";
  policy.normalized_controls.parallelism_policy_id = "parallelism:pcr062_local";
  policy.normalized_controls.what_if_policy_id = "what_if:pcr062_disabled";
  policy.normalized_controls.safe_control_ids = {
      "hint:join_order=preserve",
      "authority:sblr_uuid_bound"};
  policy.safe_control_ids = {
      "security_recheck:preserved",
      "mga_recheck:preserved"};
  return policy;
}

plan::LogicalPlan LogicalPlan() {
  plan::LogicalPlan logical;
  logical.ok = true;
  logical.plan_id = Id("logical_plan.lookup");
  logical.optimizer_policy = SafePolicy();
  auto node = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                        plan::PhysicalAccessKind::kNone,
                                        Id("operation.lookup"),
                                        "lookup");
  node.required_object_uuids.push_back(Id("relation.customer"));
  node.required_descriptors.push_back("sha256:descriptor-pcr062-customer");
  logical.nodes.push_back(std::move(node));
  return logical;
}

opt::BoundOptimizerRequest BoundRequest() {
  opt::BoundOptimizerRequest request;
  request.context.request_uuid = Id("request.lookup");
  request.context.operation_id = Id("operation.lookup");
  request.context.sblr_digest = "sha256:sblr-pcr062-lookup";
  request.context.descriptor_set_digest = "sha256:descriptor-pcr062-customer";
  request.context.statistics_snapshot_id = "sha256:stats-pcr062-after-refresh";
  request.context.metric_snapshot_id = "sha256:metrics-pcr062-feedback";
  request.context.executor_capability_set_id = "executor-capability:pcr062-local-mga";
  request.context.catalog_epoch = 6205;
  request.context.stats_epoch = 6206;
  request.context.security_epoch = 6207;
  request.context.redaction_epoch = 6208;
  request.context.policy_epoch = 6209;
  request.context.resource_epoch = 6210;
  request.context.name_resolution_epoch = 6211;
  request.context.memory_policy_epoch = 6212;
  request.context.memory_feedback_generation = 6213;
  request.context.route_epoch = 6214;
  request.context.security_context_present = true;
  request.context.transaction_context_present = true;
  request.logical_plan = LogicalPlan();
  return request;
}

opt::BoundOptimizerResult FeedbackPlanResult() {
  opt::PlanCandidate candidate;
  candidate.candidate_id = Id("candidate.feedback_index");
  candidate.access_kind = plan::PhysicalAccessKind::kScalarBtreeLookup;
  candidate.scope = "local";
  candidate.cost.total_cost = 42;
  candidate.cost.row_cost = 20;
  candidate.cost.io_cost = 12;
  candidate.cost.memory_cost = 4;
  candidate.cost.uncertainty_cost = 6;
  candidate.cost.confidence = opt::CostConfidence::kHigh;
  candidate.cost.reason = "catalog_calibrated_feedback_cost";
  candidate.estimated_rows = 1000;
  candidate.selected = true;
  candidate.runtime_evidence = {
      "adaptive_feedback.feedback_generation=6213",
      "adaptive_feedback.plan_hash=plan.pcr062.after_feedback",
      "optimizer_feedback.high_misestimate",
      "optimizer_feedback.spill_observed",
      "runtime_feedback.profile_id=calibrated:pcr062_cost_profile:btree_lookup:index_lookup_with_feedback",
      "runtime_actual.actual_rows=1000",
      "actual_rows=1000",
      "memory_grant_bytes=65536",
      "route_label=pcr062.feedback",
      "result_contract_hash=result.pcr062.same",
      "plan_node_id=node.pcr062.lookup"};

  opt::BoundOptimizerResult result;
  result.ok = true;
  result.diagnostic_code = "SB_OPT_OK";
  result.plan_id = Id("logical_plan.lookup");
  result.optimizer_profile = "catalog_calibrated_feedback_v1";
  result.candidates.push_back(std::move(candidate));
  return result;
}

opt::OptimizerFeedbackPlanChoiceObservation Observation(std::uint64_t generation,
                                                       std::uint64_t memory_generation,
                                                       std::string plan_hash) {
  opt::OptimizerFeedbackPlanChoiceObservation observation;
  observation.route_label = "pcr062.feedback";
  observation.plan_hash = std::move(plan_hash);
  observation.result_hash = "result.pcr062.same";
  observation.cache_key = "cache.pcr062.feedback." + std::to_string(generation);
  observation.cache_diagnostic_code = "SB_OPT_PLAN_CACHE.OK";
  observation.feedback_generation = generation;
  observation.memory_feedback_generation = memory_generation;
  observation.stats_epoch = 6206 + generation;
  observation.catalog_epoch = 6205 + generation;
  observation.runtime_consumed = true;
  observation.exact_fallback_available = true;
  observation.benchmark_clean_claim = true;
  return observation;
}

void HintPolicyIsFirstClassAndNonSqlAuthoritative() {
  const auto request = HintPolicyRequest();
  const auto admission = opt::EvaluateOptimizerHintPolicyAdmission(request);
  Require(admission.accepted && admission.applied,
          "normalized engine hint policy should be admitted");
  Require(admission.diagnostic_code == "SB_OPT_HINT_POLICY.OK",
          "hint policy diagnostic should be OK");
  Require(StartsWith(admission.policy_digest, "hint-policy-fnv64:"),
          "hint policy should have stable digest evidence");
  Require(admission.barrier_decision.may_apply_hint,
          "optimizer barrier should allow admitted hint");
  Require(Contains(admission.evidence, "hint_policy.parser_execution_authority=false"),
          "hint policy must reject parser execution authority");
  Require(Contains(admission.evidence,
                   "hint_policy.transaction_finality_authority=engine_transaction_inventory"),
          "hint policy must preserve MGA transaction finality authority");

  const auto same = opt::EvaluateOptimizerHintPolicyAdmission(request);
  Require(same.policy_digest == admission.policy_digest,
          "same hint policy input should produce stable digest");

  auto changed = request;
  changed.policy_epoch += 1;
  const auto changed_admission = opt::EvaluateOptimizerHintPolicyAdmission(changed);
  Require(changed_admission.policy_digest != admission.policy_digest,
          "epoch changes should affect hint policy digest");

  auto raw_sql = request;
  raw_sql.raw_sql_text_present = true;
  Require(opt::EvaluateOptimizerHintPolicyAdmission(raw_sql).diagnostic_code ==
              "SB_OPT_HINT_POLICY.PARSER_SQL_REFUSED",
          "raw SQL text must not be hint policy authority");

  auto parser = request;
  parser.parser_execution_authority_claimed = true;
  Require(opt::EvaluateOptimizerHintPolicyAdmission(parser).diagnostic_code ==
              "SB_OPT_HINT_POLICY.PARSER_SQL_REFUSED",
          "parser execution authority must be refused");

  auto reference = request;
  reference.reference_or_legacy_authority_claimed = true;
  Require(opt::EvaluateOptimizerHintPolicyAdmission(reference).diagnostic_code ==
              "SB_OPT_HINT_POLICY.UNSAFE_AUTHORITY_REFUSED",
          "reference or legacy authority must be refused");

  auto name_authority = request;
  name_authority.name_authority_claimed = true;
  Require(opt::EvaluateOptimizerHintPolicyAdmission(name_authority).diagnostic_code ==
              "SB_OPT_HINT_POLICY.UNSAFE_AUTHORITY_REFUSED",
          "name authority must be refused");

  auto unsafe_token = request;
  unsafe_token.normalized_hint_tokens = {"USE INDEX customer_idx"};
  Require(opt::EvaluateOptimizerHintPolicyAdmission(unsafe_token).diagnostic_code ==
              "SB_OPT_HINT_POLICY.UNSAFE_TOKEN",
          "hint tokens must be normalized safe control ids");

  auto missing_recheck = request;
  missing_recheck.mga_visibility_recheck_required = false;
  Require(opt::EvaluateOptimizerHintPolicyAdmission(missing_recheck).diagnostic_code ==
              "SB_OPT_HINT_POLICY.MISSING_RECHECK",
          "hint admission must require MGA visibility recheck");
}

void CalibrationProfilesPersistAndInvalidateAsEvidenceOnly() {
  const auto feedback = RuntimeFeedback();
  const auto status = opt::EvaluateOptimizerRuntimeFeedback(feedback);
  Require(status.ok && status.applied,
          "runtime feedback should be accepted as advisory optimizer evidence");
  Require(status.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.HIGH_MISESTIMATE",
          "high misestimate should drive the feedback diagnostic");
  Require(Contains(status.evidence, "high_misestimate"),
          "feedback should record high misestimate evidence");
  Require(Contains(status.evidence, "spill_observed"),
          "feedback should record spill evidence");
  Require(Contains(status.evidence, "memory_undergrant"),
          "feedback should record memory undergrant evidence");
  Require(status.cost_profile.apply,
          "calibrated cost profile should be applicable");
  Require(StartsWith(status.cost_profile.profile_id, "calibrated:"),
          "cost profile id should identify calibrated feedback");
  Require(status.cost_profile.spill_penalty_pages > 0,
          "spill evidence should add calibrated spill penalty");
  Require(status.cost_profile.uncertainty_penalty > 0,
          "feedback drift should add uncertainty penalty");
  Require(status.memory_grant.apply,
          "memory feedback should recommend an adjusted grant");

  opt::OptimizerRuntimeFeedbackStore store;
  opt::OptimizerRuntimeFeedbackRecord record;
  record.feedback_uuid = Id("feedback.record");
  record.scope_uuid = Id("scope.lookup");
  record.route_label = "pcr062.feedback";
  record.feedback_generation = 6213;
  record.policy_generation = 6209;
  record.catalog_epoch = 6205;
  record.security_epoch = 6207;
  record.feedback = feedback;
  const auto record_status = store.Record(record);
  Require(record_status.ok && record_status.applied,
          "accepted runtime feedback should be persisted in the advisory store");
  Require(Contains(record_status.evidence,
                   "runtime_feedback_persistence.invalidatable=true"),
          "feedback persistence should be explicitly invalidatable");

  opt::OptimizerRuntimeFeedbackInvalidation invalidation;
  invalidation.scope_uuid = Id("scope.lookup");
  invalidation.catalog_epoch = 6206;
  invalidation.security_epoch = 6207;
  invalidation.policy_generation = 6209;
  invalidation.reason = "statistics_refresh";
  Require(store.Invalidate(invalidation) == 1,
          "statistics refresh should invalidate scoped feedback evidence");
  const auto snapshot = store.Snapshot();
  Require(snapshot.total_records == 1 &&
              snapshot.valid_records == 0 &&
              snapshot.invalidated_records == 1,
          "feedback store snapshot should expose invalidated evidence only");
  const auto found = store.Find(Id("feedback.record"));
  Require(found.has_value() && !found->valid,
          "invalidated feedback record should remain audit evidence");
}

void StatisticsLifecyclePlansRefreshAndCatalogPersistence() {
  const auto request = StatisticsLifecycleRequest();
  const auto result = opt::EvaluateOptimizerStatisticsLifecycle(request);
  Require(result.accepted && result.refresh_needed,
          "stale agent statistics lifecycle should admit refresh");
  Require(result.agent_schedule_planned,
          "agent auto-maintenance refresh should plan agent schedule");
  Require(result.catalog_update_planned,
          "statistics lifecycle should plan catalog update");
  Require(result.next_stats_epoch > request.request_stats_epoch,
          "statistics refresh should advance stats epoch");
  Require(result.next_stats_visibility_epoch > request.stats_visibility_epoch,
          "statistics refresh should advance metadata visibility epoch");
  Require(result.has_planned_table_stats,
          "statistics lifecycle should produce planned table stats");
  Require(result.planned_table_stats.identity.object_uuid == request.relation_uuid,
          "planned stats should remain UUID scoped");
  Require(result.planned_table_stats.identity.transaction_visibility_epoch ==
              result.next_stats_visibility_epoch,
          "planned stats should carry visibility epoch metadata");
  Require(result.histogram_rebuild && result.mcv_rebuild,
          "statistics lifecycle should plan histogram and MCV rebuilds");
  Require(!result.row_visibility_semantics_changed &&
              !result.transaction_finality_semantics_changed,
          "statistics lifecycle must not alter row visibility or transaction finality");
  Require(Contains(result.evidence, "catalog_stats_epoch_persist=planned"),
          "statistics lifecycle should prove catalog stats epoch persistence");
  Require(Contains(result.evidence,
                   "mga_finality_authority=engine_transaction_inventory"),
          "statistics lifecycle should preserve MGA finality authority");

  const auto serialized = opt::SerializeOptimizerStatisticsLifecycleEvidence(result);
  Require(ContainsText(serialized, "catalog_update_planned=true"),
          "serialized lifecycle evidence should expose catalog persistence plan");
  Require(ContainsText(serialized, "row_visibility_semantics_changed=false"),
          "serialized lifecycle evidence should reject visibility overclaim");
  Require(ContainsText(serialized, "transaction_finality_semantics_changed=false"),
          "serialized lifecycle evidence should reject finality overclaim");

  auto unsafe = request;
  unsafe.parser_or_reference_authority = true;
  Require(opt::EvaluateOptimizerStatisticsLifecycle(unsafe).diagnostic_code ==
              "SB_OPT_STATS_LIFECYCLE.UNSAFE_PARSER_REFERENCE_AUTHORITY",
          "statistics lifecycle must refuse parser or reference authority");
}

void FeedbackPlanChangesRequireStabilityAndExplainLineage() {
  opt::OptimizerFeedbackStabilityRequest request;
  request.observations = {
      Observation(6212, 6301, "plan.pcr062.before_feedback"),
      Observation(6213, 6302, "plan.pcr062.after_feedback")};
  request.latest_feedback_generation = 6213;
  request.latest_memory_feedback_generation = 6302;
  request.latest_stats_epoch = 6219;
  request.latest_catalog_epoch = 6218;
  request.max_plan_switches = 1;
  request.require_benchmark_clean = true;
  const auto stability = opt::EvaluateOptimizerFeedbackStability(request);
  Require(stability.accepted && stability.stable,
          "feedback-driven plan change should be admitted after stability proof");
  Require(stability.plan_switches == 1,
          "stability proof should count the feedback plan switch");
  Require(stability.benchmark_clean_permitted,
          "safe runtime feedback with exact fallback may be benchmark clean");
  Require(Contains(stability.evidence,
                   "feedback_stability.parser_or_reference_authority=false"),
          "feedback stability must reject parser or reference authority");
  Require(Contains(stability.evidence,
                   "feedback_stability.metric_finality_or_visibility_authority=false"),
          "feedback stability must keep metrics out of finality and visibility");

  auto result_mismatch = request;
  result_mismatch.observations[1].result_hash = "result.pcr062.changed";
  Require(opt::EvaluateOptimizerFeedbackStability(result_mismatch).diagnostic_code ==
              "SB_OPT_FEEDBACK_STABILITY_RESULT_MISMATCH",
          "feedback plan changes must preserve result equivalence");

  auto parser_bad = request;
  parser_bad.observations[0].parser_or_reference_authority = true;
  Require(opt::EvaluateOptimizerFeedbackStability(parser_bad).diagnostic_code ==
              "SB_OPT_FEEDBACK_STABILITY_UNSAFE_AUTHORITY",
          "feedback stability must refuse parser or reference authority");

  const auto bound_request = BoundRequest();
  const auto validation = opt::ValidateBoundOptimizerRequest(bound_request);
  Require(validation.ok,
          "explain lineage request should satisfy optimizer authority boundary");
  const auto explain =
      opt::BuildOptimizerExplainDocument(bound_request, FeedbackPlanResult());
  Require(StartsWith(explain.plan_hash, "runtime-plan-fnv64:"),
          "explain should emit deterministic runtime plan hash evidence");
  Require(ContainsText(explain.invalidation_dependencies,
                       "memory_feedback_generation=6213"),
          "explain should bind memory feedback generation dependency");
  Require(ContainsText(explain.invalidation_dependencies,
                       "stats_epoch=6206"),
          "explain should bind stats epoch dependency");
  Require(ContainsText(explain.optimizer_controls,
                       "hint:join_order=preserve"),
          "explain should expose normalized optimizer hint control only");
  Require(ContainsText(explain.adaptive_feedback_evidence,
                       "adaptive_feedback.feedback_generation=6213"),
          "explain should include adaptive feedback lineage");
  Require(ContainsText(explain.adaptive_feedback_evidence,
                       "optimizer_feedback.high_misestimate"),
          "explain should include optimizer feedback lineage");
  Require(ContainsText(explain.runtime_actuals,
                       "runtime_actual.actual_rows=1000"),
          "explain should include runtime actuals");
  Require(ContainsText(explain.route_evidence,
                       "result_contract_hash=result.pcr062.same"),
          "explain should bind route result contract hash");

  const auto json = opt::RenderOptimizerExplainJson(explain);
  Require(ContainsText(json, "\"adaptive_feedback_evidence\""),
          "rendered explain JSON should expose feedback evidence");
  Require(ContainsText(json, "\"runtime_actuals\""),
          "rendered explain JSON should expose runtime actuals");
}

}  // namespace

int main() {
  HintPolicyIsFirstClassAndNonSqlAuthoritative();
  CalibrationProfilesPersistAndInvalidateAsEvidenceOnly();
  StatisticsLifecyclePlansRefreshAndCatalogPersistence();
  FeedbackPlanChangesRequireStabilityAndExplainLineage();
  return EXIT_SUCCESS;
}
