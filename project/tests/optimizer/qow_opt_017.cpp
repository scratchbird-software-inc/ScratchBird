// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define QOW_OPT_016_FIXTURE_ONLY
#include "qow_opt_016.cpp"
#include "optimizer_plan_cache.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string_view>

namespace {

namespace cache = scratchbird::engine::optimizer;
namespace api = scratchbird::engine::internal_api;

bool Require017(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-OPT-017-V1: " << detail << '\n';
  return condition;
}

exec::TypedPhysicalNodeDag ExplainDag() {
  const auto inputs = Inputs();
  const auto published = opt::PublishCanonicalPhysicalDag(
      inputs.request, inputs.admission, inputs.alternatives, inputs.search,
      inputs.capabilities, PublicationIdentity());
  return published.accepted ? published.physical_dag
                            : exec::TypedPhysicalNodeDag{};
}

cache::CanonicalPreparedExplainEvidence ExplainEvidence(
    const exec::TypedPhysicalNodeDag& dag) {
  cache::CanonicalPreparedExplainEvidence evidence;
  evidence.selected_plan_uuid = dag.selected_plan_uuid;
  evidence.selected_plan_signature = dag.selected_plan_signature;
  evidence.bound_sblr_tree_uuid = dag.bound_sblr_tree_uuid;
  evidence.statistics_snapshot_uuid = dag.statistics_snapshot_uuid;
  evidence.statistics_generation = dag.statistics_generation;
  evidence.search_strategy_id = "bounded.memo.exact.v1";
  evidence.stages = {
      {1, "admission", "accepted", "engine-bound", false},
      {2, "search", "selected", "secret-security-stage", true},
  };
  std::vector<const exec::PhysicalNodeRecord*> nodes;
  for (const auto& node : dag.nodes) nodes.push_back(&node);
  std::ranges::sort(nodes, {}, &exec::PhysicalNodeRecord::physical_node_id);
  for (const auto* node : nodes) {
    const auto rows = std::max<std::uint64_t>(1, node->retained_cost.cpu_units);
    evidence.node_estimates.push_back(
        {node->physical_node_id, node->relational_node_id, rows, rows,
         "high"});
    cache::CanonicalPreparedExplainCandidateRecord selected;
    selected.candidate_family_id = "selected.family";
    selected.alternative_uuid = node->selected_alternative_uuid;
    selected.logical_node_id = node->relational_node_id;
    selected.disposition =
        cache::CanonicalPreparedExplainCandidateDisposition::kSelected;
    selected.retained_cost = node->retained_cost;
    selected.estimated_rows = rows;
    selected.confidence_id = "high";
    evidence.candidates.push_back(std::move(selected));
    if (node == nodes.front()) {
      cache::CanonicalPreparedExplainCandidateRecord rejected;
      rejected.candidate_family_id = "zz.secret.candidate";
      rejected.alternative_uuid = Uuid(9'990);
      rejected.logical_node_id = node->relational_node_id;
      rejected.disposition =
          cache::CanonicalPreparedExplainCandidateDisposition::kRejected;
      rejected.reason_id = "secret-rejection-detail";
      rejected.retained_cost = node->retained_cost;
      rejected.estimated_rows = rows;
      rejected.confidence_id = "high";
      rejected.protected_detail = true;
      evidence.candidates.push_back(std::move(rejected));
      cache::CanonicalPreparedExplainCandidateRecord pruned;
      pruned.candidate_family_id = "zzz.pruned.candidate";
      pruned.alternative_uuid = Uuid(9'991);
      pruned.logical_node_id = node->relational_node_id;
      pruned.disposition =
          cache::CanonicalPreparedExplainCandidateDisposition::kPruned;
      pruned.reason_id = "dominated-cost-vector";
      pruned.retained_cost = node->retained_cost;
      pruned.estimated_rows = rows;
      pruned.confidence_id = "high";
      evidence.candidates.push_back(std::move(pruned));
    }
  }
  evidence.barriers = {
      {dag.nodes.front().relational_node_id, "security", "secret-barrier", true}};
  evidence.statistics = {
      {Uuid(9'980), cache::CanonicalPreparedExplainStatisticState::kUsed,
       "high", true}};
  evidence.assumptions = {
      {"archive", "secret-archive-route", true},
      {"security", "secret-security-policy", true},
  };
  evidence.complete = true;
  evidence.engine_planning_evidence = true;
  return evidence;
}

cache::CanonicalPreparePhysicalPlanRequest ExplainPrepareRequest() {
  cache::CanonicalPreparePhysicalPlanRequest request;
  request.prepared_plan_uuid = Uuid(9'500);
  request.prepare_generation = 7;
  request.parameter_shape_uuid = Uuid(9'501);
  request.result_schema_uuid = Uuid(9'502);
  request.selected_physical_dag = ExplainDag();
  const auto root = std::ranges::find_if(
      request.selected_physical_dag.nodes, [&](const auto& node) {
        return node.physical_node_id ==
               request.selected_physical_dag.root_physical_node_id;
      });
  cache::CanonicalPreparedPlanResultDescriptor descriptor;
  descriptor.ordinal = 1;
  descriptor.descriptor_id = root->output_descriptor_ids.front();
  descriptor.name_utf8 = "secret-result-column";
  descriptor.descriptor_uuid = Uuid(9'510);
  descriptor.type_uuid = Uuid(9'511);
  descriptor.type_modifier_digest = std::string(64, 'a');
  descriptor.encoded_descriptor = "secret-result-descriptor";
  request.result_descriptors = {descriptor};
  request.dependencies = {
      {cache::CanonicalPreparedPlanDependencyKind::kObject, Uuid(9'520), 1,
       std::string(64, 'b')},
      {cache::CanonicalPreparedPlanDependencyKind::kDatatype, Uuid(9'521), 1,
       std::string(64, 'c')},
  };
  request.explain_evidence = ExplainEvidence(request.selected_physical_dag);
  request.engine_prepare_authorized = true;
  return request;
}

cache::CanonicalExplainDisclosurePolicy FullDisclosure(
    const cache::CanonicalPreparedPhysicalPlan& plan) {
  cache::CanonicalExplainDisclosurePolicy disclosure;
  disclosure.request_uuid = Uuid(9'530);
  disclosure.subject_uuid = Uuid(9'531);
  disclosure.redaction_policy_uuid = Uuid(9'532);
  disclosure.security_epoch = plan.security_epoch;
  disclosure.policy_epoch = plan.policy_epoch;
  disclosure.engine_plan_read_authorized = true;
  disclosure.plan_shape_authorized = true;
  disclosure.object_existence_authorized = true;
  disclosure.object_names_authorized = true;
  disclosure.cardinality_cost_authorized = true;
  disclosure.actual_profile_authorized = true;
  disclosure.security_detail_authorized = true;
  disclosure.route_detail_authorized = true;
  disclosure.cluster_detail_authorized = true;
  disclosure.archive_detail_authorized = true;
  disclosure.donor_detail_authorized = true;
  disclosure.invalidation_detail_authorized = true;
  return disclosure;
}

exec::CanonicalPhysicalNodeRuntimeObservation Runtime(const std::uint64_t n) {
  const auto observed = [](const std::uint64_t value) {
    return exec::CanonicalObservedUint64{
        exec::CanonicalRuntimeMetricState::kObserved, value};
  };
  const auto na = [] {
    return exec::CanonicalObservedUint64{
        exec::CanonicalRuntimeMetricState::kNotApplicable, 0};
  };
  exec::CanonicalPhysicalNodeRuntimeObservation runtime;
  runtime.elapsed_ns = observed(100 + n);
  runtime.operator_wait_ns = observed(0);
  runtime.current_memory_bytes = observed(10);
  runtime.peak_memory_bytes = observed(20);
  runtime.decoded_bytes = na();
  runtime.bytes_read = na();
  runtime.bytes_written = na();
  runtime.pages_read = na();
  runtime.pages_written = na();
  runtime.spill_bytes_read = observed(0);
  runtime.spill_bytes_written = observed(0);
  runtime.visibility_recheck_count = na();
  runtime.security_recheck_count = na();
  runtime.storage_recheck_count = na();
  runtime.index_recheck_count = na();
  runtime.residual_recheck_count = na();
  runtime.compatibility_recheck_count = na();
  runtime.archive_bytes_read = na();
  runtime.cluster_bytes_sent = na();
  runtime.cluster_bytes_received = na();
  runtime.authority.engine_execution_observation = true;
  runtime.producer_receipt_complete = true;
  runtime.dispatcher_elapsed_frozen = true;
  runtime.counters_frozen_after_finish = true;
  return runtime;
}

void FingerprintString(std::ostringstream& out, const std::string_view value) {
  out << value.size() << ':' << value << ';';
}

void FingerprintU64(std::ostringstream& out, const std::uint64_t value) {
  out << value << ';';
}

void FingerprintBool(std::ostringstream& out, const bool value) {
  out << (value ? '1' : '0') << ';';
}

template <typename Enum>
void FingerprintEnum(std::ostringstream& out, const Enum value) {
  FingerprintU64(out, static_cast<std::uint64_t>(value));
}

template <typename Range, typename Emit>
void FingerprintRange(std::ostringstream& out, const Range& values,
                      Emit emit) {
  FingerprintU64(out, values.size());
  for (const auto& value : values) emit(out, value);
}

void FingerprintCost(std::ostringstream& out,
                     const exec::PhysicalCostVectorReceipt& cost) {
  FingerprintString(out, cost.cost_vector_uuid);
  FingerprintString(out, cost.calibration_profile_uuid);
  FingerprintU64(out, cost.scalar_score);
  FingerprintU64(out, cost.cpu_units);
  FingerprintU64(out, cost.page_read_sequential_units);
  FingerprintU64(out, cost.page_read_random_units);
  FingerprintU64(out, cost.page_write_units);
  FingerprintU64(out, cost.memory_bytes_required);
  FingerprintU64(out, cost.spill_bytes_expected);
  FingerprintU64(out, cost.network_bytes_expected);
  FingerprintU64(out, cost.mga_visibility_checks_expected);
  FingerprintU64(out, cost.archive_fetches_expected);
  FingerprintU64(out, cost.uncertainty_penalty);
  FingerprintU64(out, cost.risk_penalty);
  FingerprintU64(out, cost.confidence);
}

void FingerprintMetric(std::ostringstream& out,
                       const exec::CanonicalObservedUint64& metric) {
  FingerprintEnum(out, metric.state);
  FingerprintU64(out, metric.value);
}

void FingerprintRuntime(
    std::ostringstream& out,
    const exec::CanonicalPhysicalNodeRuntimeObservation& runtime) {
  FingerprintU64(out, runtime.abi_version);
  const exec::CanonicalObservedUint64* metrics[] = {
      &runtime.elapsed_ns,
      &runtime.operator_wait_ns,
      &runtime.current_memory_bytes,
      &runtime.peak_memory_bytes,
      &runtime.decoded_bytes,
      &runtime.bytes_read,
      &runtime.bytes_written,
      &runtime.pages_read,
      &runtime.pages_written,
      &runtime.spill_bytes_read,
      &runtime.spill_bytes_written,
      &runtime.visibility_recheck_count,
      &runtime.security_recheck_count,
      &runtime.storage_recheck_count,
      &runtime.index_recheck_count,
      &runtime.residual_recheck_count,
      &runtime.compatibility_recheck_count,
      &runtime.archive_bytes_read,
      &runtime.cluster_bytes_sent,
      &runtime.cluster_bytes_received,
  };
  for (const auto* metric : metrics) FingerprintMetric(out, *metric);
  FingerprintBool(out, runtime.authority.engine_execution_observation);
  FingerprintBool(out, runtime.authority.owns_execution);
  FingerprintBool(out, runtime.authority.owns_visibility);
  FingerprintBool(out, runtime.authority.owns_transaction_finality);
  FingerprintBool(out, runtime.authority.owns_recovery);
  FingerprintBool(out, runtime.authority.owns_feedback);
  FingerprintBool(out, runtime.authority.owns_benchmark);
  FingerprintBool(out, runtime.authority.owns_parser_execution);
  FingerprintBool(out,
                  runtime.authority.wal_is_transaction_or_recovery_authority);
  FingerprintBool(out, runtime.producer_receipt_complete);
  FingerprintBool(out, runtime.dispatcher_elapsed_frozen);
  FingerprintBool(out, runtime.counters_frozen_after_finish);
}

std::string RuntimeFingerprint(
    const exec::CanonicalPhysicalNodeRuntimeObservation& runtime) {
  std::ostringstream out;
  FingerprintRuntime(out, runtime);
  return out.str();
}

void FingerprintMga(std::ostringstream& out,
                    const exec::PhysicalMgaStatementContext& context) {
  FingerprintString(out, context.statement_uuid);
  FingerprintString(out, context.owning_transaction_uuid);
  FingerprintString(out, context.statement_snapshot_uuid);
  FingerprintString(out, context.statement_metadata_snapshot_uuid);
  FingerprintU64(out, context.owning_local_transaction_id);
  FingerprintU64(out, context.visible_committed_high_watermark);
  FingerprintU64(out, context.oldest_active_transaction_id);
  FingerprintU64(out, context.oldest_interesting_transaction_id);
  FingerprintU64(out, context.oldest_snapshot_transaction_id);
  FingerprintU64(out, context.retention_horizon_transaction_id);
  FingerprintRange(out, context.active_excluded_local_transaction_ids,
                   [](auto& stream, const auto value) {
                     FingerprintU64(stream, value);
                   });
  FingerprintRange(out, context.in_doubt_excluded_local_transaction_ids,
                   [](auto& stream, const auto value) {
                     FingerprintU64(stream, value);
                   });
  FingerprintString(out, context.snapshot_kind);
  FingerprintU64(out,
                 context.publication_inventory_next_local_transaction_id);
  FingerprintBool(out, context.inventory_authoritative);
  FingerprintBool(out, context.complete);
  FingerprintBool(out, context.current);
}

std::string ExplainDocumentFingerprint(
    const cache::CanonicalExplainDocument& document) {
  std::ostringstream out;
  FingerprintU64(out, document.abi_version);
  FingerprintEnum(out, document.mode);
  FingerprintString(out, document.prepared_plan_uuid);
  FingerprintU64(out, document.prepare_generation);
  FingerprintString(out, document.selected_plan_uuid);
  FingerprintString(out, document.selected_plan_signature);
  FingerprintString(out, document.bound_sblr_tree_uuid);
  FingerprintString(out, document.result_schema_uuid);
  FingerprintU64(out, document.root_physical_node_id);
  FingerprintU64(out, document.published_node_count);
  FingerprintString(out, document.search_strategy_id);
  FingerprintRange(out, document.stages, [](auto& stream, const auto& stage) {
    FingerprintU64(stream, stage.ordinal);
    FingerprintString(stream, stage.stage_id);
    FingerprintString(stream, stage.outcome_id);
    FingerprintString(stream, stage.detail_id);
    FingerprintBool(stream, stage.protected_detail);
  });
  FingerprintRange(out, document.candidates,
                   [](auto& stream, const auto& candidate) {
                     FingerprintU64(stream, candidate.logical_node_id);
                     FingerprintEnum(stream, candidate.disposition);
                     FingerprintString(stream,
                                       candidate.candidate_family_id);
                     FingerprintString(stream, candidate.alternative_uuid);
                     FingerprintString(stream, candidate.reason_id);
                     FingerprintU64(stream, candidate.estimated_rows);
                     FingerprintCost(stream, candidate.retained_cost);
                     FingerprintEnum(stream, candidate.identity_state);
                     FingerprintEnum(stream, candidate.estimate_state);
                   });
  FingerprintRange(out, document.barriers,
                   [](auto& stream, const auto& barrier) {
                     FingerprintU64(stream, barrier.logical_node_id);
                     FingerprintString(stream, barrier.barrier_kind_id);
                     FingerprintString(stream, barrier.reason_id);
                     FingerprintBool(stream, barrier.protected_detail);
                   });
  FingerprintRange(out, document.statistics,
                   [](auto& stream, const auto& statistic) {
                     FingerprintString(stream, statistic.statistic_uuid);
                     FingerprintEnum(stream, statistic.state);
                     FingerprintString(stream, statistic.confidence_id);
                     FingerprintBool(stream, statistic.protected_detail);
                   });
  FingerprintRange(out, document.assumptions,
                   [](auto& stream, const auto& assumption) {
                     FingerprintString(stream, assumption.category_id);
                     FingerprintString(stream, assumption.assumption_id);
                     FingerprintBool(stream, assumption.protected_detail);
                   });
  FingerprintRange(out, document.result_descriptors,
                   [](auto& stream, const auto& descriptor) {
                     FingerprintU64(stream, descriptor.ordinal);
                     FingerprintU64(stream, descriptor.descriptor_id);
                     FingerprintString(stream, descriptor.name_utf8);
                     FingerprintString(stream, descriptor.descriptor_uuid);
                     FingerprintString(stream, descriptor.type_uuid);
                     FingerprintString(stream, descriptor.domain_uuid);
                     FingerprintString(stream, descriptor.collation_uuid);
                     FingerprintString(stream, descriptor.timezone_uuid);
                     FingerprintString(stream,
                                       descriptor.type_modifier_digest);
                     FingerprintString(stream, descriptor.encoded_descriptor);
                     FingerprintBool(stream, descriptor.nullable);
                   });
  FingerprintRange(out, document.dependencies,
                   [](auto& stream, const auto& dependency) {
                     FingerprintEnum(stream, dependency.dependency_kind);
                     FingerprintString(stream, dependency.dependency_uuid);
                     FingerprintU64(stream, dependency.generation);
                     FingerprintString(stream, dependency.definition_digest);
                   });
  FingerprintRange(out, document.nodes, [](auto& stream, const auto& node) {
    FingerprintU64(stream, node.physical_node_id);
    FingerprintU64(stream, node.logical_node_id);
    FingerprintU64(stream, node.causal_counter_id);
    FingerprintU64(stream, node.execution_ordinal);
    FingerprintString(stream, node.implementation_id);
    FingerprintString(stream, node.logical_semantic_variant_id);
    FingerprintString(stream, node.selected_alternative_uuid);
    FingerprintString(stream, node.transformation_uuid);
    FingerprintString(stream, node.transformation_rule_id);
    FingerprintString(stream, node.executor_capability_uuid);
    FingerprintU64(stream, node.executor_capability_abi_version);
    FingerprintRange(stream, node.input_physical_node_ids,
                     [](auto& nested, const auto value) {
                       FingerprintU64(nested, value);
                     });
    FingerprintRange(stream, node.output_descriptor_ids,
                     [](auto& nested, const auto value) {
                       FingerprintU64(nested, value);
                     });
    const auto append_property = [](auto& nested, const auto& value) {
      FingerprintString(nested, value);
    };
    FingerprintRange(stream, node.required_property_uuids, append_property);
    FingerprintRange(stream, node.delivered_property_uuids, append_property);
    FingerprintRange(stream, node.enforced_property_uuids, append_property);
    FingerprintCost(stream, node.estimated_cost);
    FingerprintU64(stream, node.memory_bytes_required);
    FingerprintU64(stream, node.spill_bytes_expected);
    FingerprintU64(stream, node.estimated_input_rows);
    FingerprintU64(stream, node.estimated_output_rows);
    FingerprintU64(stream, node.actual_input_rows);
    FingerprintU64(stream, node.actual_output_rows);
    FingerprintU64(stream, node.actual_rows_examined);
    FingerprintBool(stream, node.data_access_observation_known);
    FingerprintBool(stream, node.data_access_observed);
    FingerprintEnum(stream, node.data_access_state);
    FingerprintRuntime(stream, node.runtime_observation);
    FingerprintEnum(stream, node.runtime_route_state);
    FingerprintEnum(stream, node.runtime_security_state);
    FingerprintEnum(stream, node.runtime_archive_state);
    FingerprintEnum(stream, node.runtime_cluster_state);
    FingerprintEnum(stream, node.runtime_donor_state);
    FingerprintEnum(stream, node.identity_state);
    FingerprintEnum(stream, node.estimate_state);
    FingerprintEnum(stream, node.actual_state);
  });
  FingerprintEnum(out, document.lifecycle_status);
  FingerprintBool(out, document.invalidation.has_value());
  if (document.invalidation.has_value()) {
    const auto& invalidation = *document.invalidation;
    FingerprintBool(out, invalidation.invalidated);
    FingerprintBool(out, invalidation.duplicate_invalidation);
    FingerprintU64(out, invalidation.invalidation_generation);
    FingerprintString(out, invalidation.prepared_plan_uuid);
    FingerprintString(out, invalidation.field_id);
    FingerprintBool(out, invalidation.protected_detail);
    FingerprintBool(out, invalidation.stale_execution_observed);
  }
  FingerprintBool(out, document.cache_entry_present);
  FingerprintBool(out, document.cache_hit);
  FingerprintBool(out, document.reprepare_attempted);
  FingerprintBool(out, document.reprepare_succeeded);
  FingerprintU64(out, document.reprepare_attempt_count);
  FingerprintString(out, document.replacement_prepared_plan_uuid);
  FingerprintBool(out, document.data_access_observation_known);
  FingerprintBool(out, document.data_access_observed);
  FingerprintEnum(out, document.data_access_state);
  FingerprintBool(out, document.analyzed_mga_statement_context_present);
  FingerprintMga(out, document.analyzed_mga_statement_context);
  FingerprintEnum(out, document.analyzed_mga_statement_context_state);
  FingerprintBool(out, document.analyzed);
  FingerprintBool(out, document.redacted);
  FingerprintBool(out, document.immutable_stored_plan_rendered);
  FingerprintBool(out, document.completed_engine_execution_consumed);
  return out.str();
}

exec::CanonicalPhysicalDagDispatchResult Dispatch(
    const cache::CanonicalPreparedPhysicalPlan& plan,
    const exec::PhysicalMgaStatementContext& context) {
  exec::CanonicalPhysicalDagDispatchResult dispatch;
  dispatch.execution_started = true;
  dispatch.selected_plan_uuid = plan.selected_plan_uuid;
  dispatch.executed_root_physical_node_id = plan.root_physical_node_id;
  dispatch.mga_statement_context = context;
  dispatch.authority.engine_mga_snapshot_bound = true;
  std::vector<const cache::CanonicalPreparedPhysicalNode*> nodes;
  for (const auto& node : plan.nodes) nodes.push_back(&node);
  std::ranges::sort(nodes, {},
                    &cache::CanonicalPreparedPhysicalNode::publication_ordinal);
  for (const auto* node : nodes) {
    exec::CanonicalPhysicalDispatchStepResult step;
    step.selected_plan_uuid = plan.selected_plan_uuid;
    step.executed_physical_node_id = node->physical_node_id;
    step.executed_relational_node_id = node->relational_node_id;
    step.executed_implementation_id = node->implementation_id;
    step.executed_input_physical_node_ids = node->input_physical_node_ids;
    step.causal_counter_id = node->causal_counter_id;
    step.result_handle_id = node->physical_node_id;
    step.output_descriptor_ids = node->output_descriptor_ids;
    step.authority.engine_mga_snapshot_bound = true;
    step.execution_ordinal = node->publication_ordinal + 1;
    step.execution_started = true;
    step.execution_finished = true;
    step.counters_captured_after_finish = true;
    step.data_access_observation_known = true;
    step.mga_statement_context = context;
    step.runtime_observation = Runtime(node->physical_node_id);
    dispatch.executed_steps.push_back(std::move(step));
  }
  const auto root = std::ranges::find_if(plan.nodes, [&](const auto& node) {
    return node.physical_node_id == plan.root_physical_node_id;
  });
  dispatch.root_result_handle_id = root->physical_node_id;
  dispatch.root_output_descriptor_ids = root->output_descriptor_ids;
  dispatch.root_causal_counter_id = root->causal_counter_id;
  return dispatch;
}

bool ValidateExplain() {
  auto prepare_request = ExplainPrepareRequest();
  cache::CanonicalPreparedPlanStore store;
  const auto prepared =
      cache::PrepareCanonicalPhysicalPlan(prepare_request, &store);
  if (!Require017(prepared.accepted && prepared.prepared_plan &&
                      prepared.prepared_plan->explain_evidence.has_value(),
                  "complete planning evidence was not retained")) {
    return false;
  }
  cache::CanonicalExplainRequest plain;
  plain.mode = cache::CanonicalExplainMode::kPlain;
  plain.prepared_plan = prepared.prepared_plan;
  plain.disclosure = FullDisclosure(*prepared.prepared_plan);
  const auto first = api::RenderCanonicalStoredPlanExplain(plain);
  const auto second = api::RenderCanonicalStoredPlanExplain(plain);
  bool passed = true;
  passed &= Require017(
      first.accepted && !first.analyzed && first.issues.empty() &&
          first.document.immutable_stored_plan_rendered &&
          first.document.nodes.size() == prepared.prepared_plan->nodes.size() &&
          first.document.stages.size() == 2 &&
          std::ranges::any_of(first.document.candidates, [](const auto& item) {
            return item.disposition ==
                   cache::CanonicalPreparedExplainCandidateDisposition::kRejected;
          }) &&
          std::ranges::any_of(first.document.candidates, [](const auto& item) {
            return item.disposition ==
                   cache::CanonicalPreparedExplainCandidateDisposition::kPruned;
          }) &&
          !first.document.barriers.empty() &&
          !first.document.statistics.empty() &&
          !first.document.assumptions.empty() &&
          first.document.dependencies == prepared.prepared_plan->dependencies &&
          first.document.result_descriptors ==
              prepared.prepared_plan->result_descriptors &&
          second.accepted && second.issues.empty() &&
          ExplainDocumentFingerprint(second.document) ==
              ExplainDocumentFingerprint(first.document),
      "plain EXPLAIN did not deterministically render the stored plan");

  auto dispatch = Dispatch(*prepared.prepared_plan,
                           prepare_request.selected_physical_dag
                               .mga_statement_context);
  cache::CanonicalExplainRequest analyze = plain;
  analyze.mode = cache::CanonicalExplainMode::kAnalyze;
  analyze.completed_dispatch = &dispatch;
  analyze.completed_result_schema_uuid =
      prepared.prepared_plan->result_schema_uuid;
  analyze.engine_result_schema_evidence = true;
  analyze.mga_authority.origin =
      exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  analyze.mga_authority.statement_context = dispatch.mga_statement_context;
  const auto analyzed = api::RenderCanonicalStoredPlanExplain(analyze);
  const auto analyzed_repeat = api::RenderCanonicalStoredPlanExplain(analyze);
  bool complete_node_actuals =
      analyzed.document.nodes.size() == dispatch.executed_steps.size();
  for (const auto& node : analyzed.document.nodes) {
    const auto step = std::ranges::find_if(
        dispatch.executed_steps, [&](const auto& item) {
          return item.executed_physical_node_id == node.physical_node_id;
        });
    complete_node_actuals &=
        step != dispatch.executed_steps.end() &&
        node.actual_state == cache::CanonicalExplainFieldState::kVisible &&
        node.data_access_state ==
            cache::CanonicalExplainFieldState::kVisible &&
        node.runtime_route_state ==
            cache::CanonicalExplainFieldState::kVisible &&
        node.runtime_security_state ==
            cache::CanonicalExplainFieldState::kVisible &&
        node.runtime_archive_state ==
            cache::CanonicalExplainFieldState::kVisible &&
        node.runtime_cluster_state ==
            cache::CanonicalExplainFieldState::kVisible &&
        node.runtime_donor_state ==
            cache::CanonicalExplainFieldState::kVisible;
    if (step != dispatch.executed_steps.end()) {
      complete_node_actuals &=
          node.actual_input_rows == step->input_row_count &&
          node.actual_output_rows == step->output_row_count &&
          node.actual_rows_examined == step->rows_examined &&
          node.data_access_observation_known ==
              step->data_access_observation_known &&
          node.data_access_observed == step->data_access_observed &&
          RuntimeFingerprint(node.runtime_observation) ==
              RuntimeFingerprint(step->runtime_observation);
    }
  }
  passed &= Require017(
      analyzed.accepted && analyzed.analyzed && analyzed.issues.empty() &&
          analyzed_repeat.accepted && analyzed_repeat.analyzed &&
          analyzed_repeat.issues.empty() &&
          ExplainDocumentFingerprint(analyzed_repeat.document) ==
              ExplainDocumentFingerprint(analyzed.document) &&
          complete_node_actuals &&
          analyzed.document.completed_engine_execution_consumed &&
          analyzed.document.data_access_observation_known &&
          !analyzed.document.data_access_observed &&
          analyzed.document.analyzed_mga_statement_context_present &&
          exec::PhysicalMgaStatementContextEqual(
              analyzed.document.analyzed_mga_statement_context,
              dispatch.mga_statement_context) &&
          std::ranges::all_of(analyzed.document.nodes, [](const auto& node) {
            return node.data_access_observation_known &&
                   !node.data_access_observed &&
                   node.runtime_observation.elapsed_ns.state ==
                       exec::CanonicalRuntimeMetricState::kObserved &&
                   node.runtime_observation.residual_recheck_count.state ==
                       exec::CanonicalRuntimeMetricState::kNotApplicable;
          }),
      "ANALYZE did not consume complete frozen engine actuals");

  auto cancelled_dispatch = dispatch;
  cancelled_dispatch.cancellation_observed = true;
  auto cancelled_request = analyze;
  cancelled_request.completed_dispatch = &cancelled_dispatch;
  const auto cancelled =
      api::RenderCanonicalStoredPlanExplain(cancelled_request);
  passed &= Require017(
      !cancelled.accepted && cancelled.issues.size() == 1 &&
          cancelled.issues.front().diagnostic_id ==
              "QOW-DIAG-QRY-004-PHYSICAL-CANCELLED-V1" &&
          ExplainDocumentFingerprint(cancelled.document) ==
              ExplainDocumentFingerprint(cache::CanonicalExplainDocument{}),
      "cancelled completed dispatch returned a partial ANALYZE document");

  auto forbidden_request = plain;
  forbidden_request.feedback_authority_claimed = true;
  const auto forbidden =
      api::RenderCanonicalStoredPlanExplain(forbidden_request);
  passed &= Require017(
      !forbidden.accepted && forbidden.issues.size() == 1 &&
          forbidden.issues.front().field_id ==
              "renderer_authority_or_request" &&
          ExplainDocumentFingerprint(forbidden.document) ==
              ExplainDocumentFingerprint(cache::CanonicalExplainDocument{}),
      "forbidden renderer request authority reached EXPLAIN");

  auto mixed_policy = analyze;
  mixed_policy.disclosure.security_detail_authorized = false;
  mixed_policy.disclosure.archive_detail_authorized = false;
  mixed_policy.disclosure.cluster_detail_authorized = false;
  mixed_policy.disclosure.donor_detail_authorized = false;
  const auto mixed = api::RenderCanonicalStoredPlanExplain(mixed_policy);
  passed &= Require017(
      mixed.accepted && mixed.document.redacted &&
          !mixed.document.analyzed_mga_statement_context_present &&
          mixed.document.analyzed_mga_statement_context_state ==
              cache::CanonicalExplainFieldState::kRedacted &&
          mixed.document.nodes.front().runtime_security_state ==
              cache::CanonicalExplainFieldState::kRedacted &&
          mixed.document.nodes.front().runtime_archive_state ==
              cache::CanonicalExplainFieldState::kRedacted &&
          mixed.document.nodes.front().runtime_cluster_state ==
              cache::CanonicalExplainFieldState::kRedacted &&
          mixed.document.nodes.front().runtime_donor_state ==
              cache::CanonicalExplainFieldState::kRedacted,
      "mixed policy exposed protected analyzed metric groups");

  auto access_policy = analyze;
  access_policy.disclosure.route_detail_authorized = false;
  const auto access_redacted =
      api::RenderCanonicalStoredPlanExplain(access_policy);
  passed &= Require017(
      access_redacted.accepted && access_redacted.document.redacted &&
          !access_redacted.document.data_access_observation_known &&
          access_redacted.document.data_access_state ==
              cache::CanonicalExplainFieldState::kRedacted &&
          access_redacted.document.nodes.front().data_access_state ==
              cache::CanonicalExplainFieldState::kRedacted &&
          !access_redacted.document.nodes.front()
               .data_access_observation_known,
      "route-denied ANALYZE exposed physical access truth");

  auto disclosure_contradiction = analyze;
  disclosure_contradiction.disclosure.cardinality_cost_authorized = false;
  const auto contradictory =
      api::RenderCanonicalStoredPlanExplain(disclosure_contradiction);
  passed &= Require017(!contradictory.accepted,
                       "actual profile bypassed cardinality disclosure");

  auto missing_metric = dispatch;
  missing_metric.executed_steps.front().runtime_observation.decoded_bytes = {};
  analyze.completed_dispatch = &missing_metric;
  const auto missing = api::RenderCanonicalStoredPlanExplain(analyze);
  passed &= Require017(
      !missing.accepted && missing.document.nodes.empty() &&
          missing.issues.size() == 1 &&
          missing.issues.front().diagnostic_id ==
              "QOW-DIAG-OPT-017-REFUSAL-V1",
      "unavailable runtime metric reached ANALYZE");

  auto missing_residual = dispatch;
  missing_residual.executed_steps.front()
      .runtime_observation.residual_recheck_count = {};
  analyze.completed_dispatch = &missing_residual;
  const auto missing_residual_result =
      api::RenderCanonicalStoredPlanExplain(analyze);
  passed &= Require017(!missing_residual_result.accepted,
                       "unavailable residual metric reached ANALYZE");

  auto resource_contradiction = dispatch;
  resource_contradiction.executed_steps.front()
      .runtime_observation.peak_memory_bytes.value =
      prepared.prepared_plan->memory_budget_bytes + 1;
  analyze.completed_dispatch = &resource_contradiction;
  const auto resource_result =
      api::RenderCanonicalStoredPlanExplain(analyze);
  passed &= Require017(!resource_result.accepted,
                       "runtime resource contradiction reached ANALYZE");

  auto failed_dispatch = dispatch;
  failed_dispatch.diagnostic.ok = false;
  failed_dispatch.diagnostic.diagnostic_code = "ENGINE.EXECUTION.FAILURE";
  failed_dispatch.diagnostic.detail = "secret-engine-failure";
  analyze.completed_dispatch = &failed_dispatch;
  analyze.disclosure.security_detail_authorized = false;
  const auto failed = api::RenderCanonicalStoredPlanExplain(analyze);
  passed &= Require017(
      !failed.accepted && failed.document.nodes.empty() &&
          failed.issues.front().diagnostic_id == "ENGINE.EXECUTION.FAILURE" &&
          failed.issues.front().field_id.find("secret") == std::string::npos,
      "failed engine dispatch leaked detail or returned a partial document");

  auto forged_plan =
      std::make_shared<cache::CanonicalPreparedPhysicalPlan>(
          *prepared.prepared_plan);
  forged_plan->explain_evidence->stages.front().ordinal = 99;
  auto forged_request = plain;
  forged_request.prepared_plan = forged_plan;
  const auto forged_render =
      api::RenderCanonicalStoredPlanExplain(forged_request);
  passed &= Require017(!forged_render.accepted,
                       "copied-plan structural mutation reached renderer");

  auto forged_evidence_authority_plan =
      std::make_shared<cache::CanonicalPreparedPhysicalPlan>(
          *prepared.prepared_plan);
  forged_evidence_authority_plan->explain_evidence
      ->feedback_authority_claimed = true;
  auto forged_evidence_authority_request = plain;
  forged_evidence_authority_request.prepared_plan =
      forged_evidence_authority_plan;
  passed &= Require017(
      !api::RenderCanonicalStoredPlanExplain(
           forged_evidence_authority_request)
           .accepted,
      "copied-plan forbidden planning authority reached renderer");

  auto forged_identity_plan =
      std::make_shared<cache::CanonicalPreparedPhysicalPlan>(
          *prepared.prepared_plan);
  forged_identity_plan->prepared_plan_uuid = "not-a-canonical-uuid";
  auto forged_identity_request = plain;
  forged_identity_request.prepared_plan = forged_identity_plan;
  passed &= Require017(
      !api::RenderCanonicalStoredPlanExplain(forged_identity_request)
           .accepted,
      "copied-plan noncanonical identity reached renderer");

  auto forged_selection_plan =
      std::make_shared<cache::CanonicalPreparedPhysicalPlan>(
          *prepared.prepared_plan);
  const auto forged_logical_node_id =
      forged_selection_plan->nodes.front().relational_node_id;
  const auto forged_alternative_uuid = Uuid(9'997);
  forged_selection_plan->nodes.front().selected_alternative_uuid =
      forged_alternative_uuid;
  const auto selected_candidate = std::ranges::find_if(
      forged_selection_plan->explain_evidence->candidates,
      [&](const auto& candidate) {
        return candidate.logical_node_id == forged_logical_node_id &&
               candidate.disposition ==
                   cache::CanonicalPreparedExplainCandidateDisposition::
                       kSelected;
      });
  selected_candidate->alternative_uuid = forged_alternative_uuid;
  auto forged_selection_request = plain;
  forged_selection_request.prepared_plan = forged_selection_plan;
  passed &= Require017(
      !api::RenderCanonicalStoredPlanExplain(forged_selection_request)
           .accepted,
      "copied-plan selected memo signature mutation reached renderer");

  auto forged_score_plan =
      std::make_shared<cache::CanonicalPreparedPhysicalPlan>(
          *prepared.prepared_plan);
  const auto forged_score_logical_node_id =
      forged_score_plan->nodes.front().relational_node_id;
  ++forged_score_plan->nodes.front().retained_cost.scalar_score;
  const auto scored_candidate = std::ranges::find_if(
      forged_score_plan->explain_evidence->candidates,
      [&](const auto& candidate) {
        return candidate.logical_node_id == forged_score_logical_node_id &&
               candidate.disposition ==
                   cache::CanonicalPreparedExplainCandidateDisposition::
                       kSelected;
      });
  ++scored_candidate->retained_cost.scalar_score;
  auto forged_score_request = plain;
  forged_score_request.prepared_plan = forged_score_plan;
  passed &= Require017(
      !api::RenderCanonicalStoredPlanExplain(forged_score_request).accepted,
      "copied-plan selected scalar sum mutation reached renderer");

  auto forged_scan_mga_plan =
      std::make_shared<cache::CanonicalPreparedPhysicalPlan>(
          *prepared.prepared_plan);
  const auto forged_scan = std::ranges::find_if(
      forged_scan_mga_plan->nodes, [](const auto& node) {
        return node.node_kind == exec::PhysicalNodeKind::kScan;
      });
  bool forged_scan_refused = false;
  if (forged_scan != forged_scan_mga_plan->nodes.end() &&
      forged_scan->retained_cost.mga_visibility_checks_expected != 0) {
    const auto removed_checks =
        forged_scan->retained_cost.mga_visibility_checks_expected;
    forged_scan->retained_cost.mga_visibility_checks_expected = 0;
    forged_scan->retained_cost.scalar_score -= removed_checks;
    forged_scan_mga_plan->selected_scalar_score -= removed_checks;
    const auto scan_candidate = std::ranges::find_if(
        forged_scan_mga_plan->explain_evidence->candidates,
        [&](const auto& candidate) {
          return candidate.logical_node_id == forged_scan->relational_node_id &&
                 candidate.disposition ==
                     cache::CanonicalPreparedExplainCandidateDisposition::
                         kSelected;
        });
    if (scan_candidate !=
        forged_scan_mga_plan->explain_evidence->candidates.end()) {
      scan_candidate->retained_cost = forged_scan->retained_cost;
      auto forged_scan_request = plain;
      forged_scan_request.prepared_plan = forged_scan_mga_plan;
      forged_scan_refused =
          !api::RenderCanonicalStoredPlanExplain(forged_scan_request)
               .accepted;
    }
  }
  passed &= Require017(
      forged_scan_refused,
      "copied scan without MGA visibility cost reached renderer");

  auto back_edge_plan =
      std::make_shared<cache::CanonicalPreparedPhysicalPlan>(
          *prepared.prepared_plan);
  auto first_node = std::ranges::min_element(
      back_edge_plan->nodes, {},
      &cache::CanonicalPreparedPhysicalNode::publication_ordinal);
  first_node->input_physical_node_ids = {
      back_edge_plan->root_physical_node_id};
  auto back_edge_request = plain;
  back_edge_request.prepared_plan = back_edge_plan;
  passed &= Require017(
      !api::RenderCanonicalStoredPlanExplain(back_edge_request).accepted,
      "back-edge/disconnected stored graph reached renderer");

  auto duplicate_descriptor_plan =
      std::make_shared<cache::CanonicalPreparedPhysicalPlan>(
          *prepared.prepared_plan);
  duplicate_descriptor_plan->nodes.front().output_descriptor_ids.push_back(
      duplicate_descriptor_plan->nodes.front().output_descriptor_ids.front());
  auto duplicate_descriptor_request = plain;
  duplicate_descriptor_request.prepared_plan = duplicate_descriptor_plan;
  passed &= Require017(
      !api::RenderCanonicalStoredPlanExplain(duplicate_descriptor_request)
           .accepted,
      "duplicate stored output descriptor reached renderer");

  auto unknown_candidate = ExplainPrepareRequest();
  unknown_candidate.explain_evidence->candidates.back().logical_node_id =
      999'999;
  cache::CanonicalPreparedPlanStore unknown_store;
  const auto unknown_prepared =
      cache::PrepareCanonicalPhysicalPlan(unknown_candidate, &unknown_store);
  passed &= Require017(!unknown_prepared.accepted,
                       "unknown candidate logical node reached PREPARE");

  auto invalid_plain = plain;
  invalid_plain.lifecycle_status =
      cache::CanonicalExecutablePlanStatus::kInvalid;
  const auto invalid_without_receipt =
      api::RenderCanonicalStoredPlanExplain(invalid_plain);
  passed &= Require017(!invalid_without_receipt.accepted,
                       "invalid lifecycle rendered without invalidation");
  cache::CanonicalExecutablePlanInvalidationReceipt invalidation;
  invalidation.invalidated = true;
  invalidation.invalidation_generation = 1;
  invalidation.prepared_plan_uuid = prepared.prepared_plan->prepared_plan_uuid;
  invalidation.field_id = "catalog_generation";
  invalid_plain.invalidation = invalidation;
  const auto invalid_with_receipt =
      api::RenderCanonicalStoredPlanExplain(invalid_plain);
  passed &= Require017(invalid_with_receipt.accepted,
                       "plain invalidation lifecycle did not render");
  auto redacted_invalidation = invalid_plain;
  redacted_invalidation.cache_entry_present = true;
  redacted_invalidation.reprepare_attempted = true;
  redacted_invalidation.reprepare_succeeded = true;
  redacted_invalidation.reprepare_attempt_count = 1;
  redacted_invalidation.replacement_prepared_plan_uuid = Uuid(9'998);
  redacted_invalidation.disclosure.invalidation_detail_authorized = false;
  const auto redacted_invalidation_result =
      api::RenderCanonicalStoredPlanExplain(redacted_invalidation);
  passed &= Require017(
      redacted_invalidation_result.accepted &&
          redacted_invalidation_result.document.redacted &&
          redacted_invalidation_result.document.invalidation.has_value() &&
          redacted_invalidation_result.document.invalidation->field_id ==
              "protected_generation" &&
          redacted_invalidation_result.document.invalidation
              ->prepared_plan_uuid.empty() &&
          redacted_invalidation_result.document
              .replacement_prepared_plan_uuid.empty(),
      "invalidation/reprepare detail was not fully redacted");
  auto stale_analyze = analyze;
  stale_analyze.completed_dispatch = &dispatch;
  stale_analyze.lifecycle_status =
      cache::CanonicalExecutablePlanStatus::kInvalid;
  stale_analyze.invalidation = invalidation;
  passed &= Require017(
      !api::RenderCanonicalStoredPlanExplain(stale_analyze).accepted,
      "ANALYZE consumed an invalidated plan");

  auto redacted_request = plain;
  redacted_request.disclosure.object_existence_authorized = false;
  redacted_request.disclosure.object_names_authorized = false;
  redacted_request.disclosure.cardinality_cost_authorized = false;
  redacted_request.disclosure.actual_profile_authorized = false;
  redacted_request.disclosure.security_detail_authorized = false;
  redacted_request.disclosure.route_detail_authorized = false;
  redacted_request.disclosure.archive_detail_authorized = false;
  redacted_request.disclosure.cluster_detail_authorized = false;
  redacted_request.disclosure.donor_detail_authorized = false;
  redacted_request.disclosure.invalidation_detail_authorized = false;
  const auto redacted = api::RenderCanonicalStoredPlanExplain(redacted_request);
  std::string flattened = redacted.document.selected_plan_signature +
                          redacted.document.bound_sblr_tree_uuid +
                          redacted.document.result_schema_uuid +
                          redacted.document.replacement_prepared_plan_uuid;
  for (const auto& stage : redacted.document.stages)
    flattened += stage.stage_id + stage.outcome_id + stage.detail_id;
  for (const auto& candidate : redacted.document.candidates) {
    flattened += candidate.candidate_family_id + candidate.alternative_uuid +
                 candidate.reason_id +
                 candidate.retained_cost.cost_vector_uuid +
                 candidate.retained_cost.calibration_profile_uuid;
  }
  for (const auto& barrier : redacted.document.barriers)
    flattened += barrier.reason_id;
  for (const auto& assumption : redacted.document.assumptions)
    flattened += assumption.assumption_id;
  for (const auto& statistic : redacted.document.statistics)
    flattened += statistic.statistic_uuid + statistic.confidence_id;
  for (const auto& descriptor : redacted.document.result_descriptors)
    flattened += descriptor.name_utf8 + descriptor.descriptor_uuid +
                 descriptor.encoded_descriptor;
  for (const auto& node : redacted.document.nodes) {
    flattened += node.implementation_id + node.logical_semantic_variant_id +
                 node.selected_alternative_uuid + node.transformation_uuid +
                 node.transformation_rule_id + node.executor_capability_uuid +
                 node.estimated_cost.cost_vector_uuid +
                 node.estimated_cost.calibration_profile_uuid;
    for (const auto& property : node.required_property_uuids)
      flattened += property;
    for (const auto& property : node.delivered_property_uuids)
      flattened += property;
    for (const auto& property : node.enforced_property_uuids)
      flattened += property;
  }
  passed &= Require017(
      redacted.accepted && redacted.document.redacted &&
          redacted.document.dependencies.empty() &&
          redacted.document.selected_plan_signature.empty() &&
          redacted.document.bound_sblr_tree_uuid.empty() &&
          redacted.document.result_schema_uuid.empty() &&
          !redacted.document.analyzed_mga_statement_context_present &&
          flattened.find("secret") == std::string::npos &&
          std::ranges::any_of(redacted.issues, [](const auto& issue) {
            return issue.diagnostic_id == "SB_DIAG_OPT_EXPLAIN_REDACTED";
          }),
      "redaction leaked protected existence, UUID, name, or detail");

#ifdef SB_QOW_PLAN_API_SOURCE_FILE
  std::ifstream source(SB_QOW_PLAN_API_SOURCE_FILE);
  const std::string text((std::istreambuf_iterator<char>(source)),
                         std::istreambuf_iterator<char>());
  const auto begin = text.find("QOW-SOURCE-QRY-022-V1");
  const auto end = text.find("QOW-SOURCE-IAS-005-V1", begin);
  const auto renderer = text.substr(begin, end - begin);
  passed &= Require017(
      begin != std::string::npos && end != std::string::npos &&
          renderer.find("ExecuteCanonical") == std::string::npos &&
          renderer.find("resolve_current") == std::string::npos &&
          renderer.find("steady_clock") == std::string::npos,
      "renderer source gained execution, current-authority, or clock access");
#endif
  return passed;
}

}  // namespace

// QOW-TEST-OPT-017-V1
#ifndef QOW_OPT_017_FIXTURE_ONLY
int main() {
  return ValidateExplain() ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif
