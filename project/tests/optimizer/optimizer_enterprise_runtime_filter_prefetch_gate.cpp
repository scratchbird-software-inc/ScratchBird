// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_runtime_filter_prefetch_enterprise.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace planner = scratchbird::engine::planner;
namespace page = scratchbird::storage::page;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OEIC-052 gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool Contains(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

opt::PhysicalPlanNode PhysicalPlanRoot() {
  opt::PhysicalPlanNode node;
  node.node_id = "oeic052.scan.customer";
  node.access_kind = planner::PhysicalAccessKind::kTableScan;
  node.executor_capability_id = "table_scan";
  node.descriptor_digest = "sha256:oeic052-scan-descriptor";
  node.estimated_rows = 10'000;
  node.storage_backed = true;
  node.preserves_visibility = true;
  return node;
}

opt::RuntimeFilterDescriptor RuntimeFilter(bool exact_fallback = true) {
  opt::RuntimeFilterDescriptor descriptor;
  descriptor.family = opt::RuntimeFilterFamily::kJoin;
  descriptor.route = opt::RuntimeFilterRoute::kScan;
  descriptor.filter_id = "oeic052.join-filter";
  descriptor.plan_node_id = "oeic052.scan.customer";
  descriptor.predicate_digest = "sha256:oeic052-join-predicate";
  descriptor.descriptor_generation = 52;
  descriptor.required_descriptor_generation = 52;
  descriptor.input_rows = 10'000;
  descriptor.estimated_candidate_rows = 1'500;
  descriptor.estimated_pruned_rows = 8'500;
  descriptor.baseline_cost_units = 10'000;
  descriptor.filter_cost_units = 800;
  descriptor.exact_recheck_cost_units = 600;
  descriptor.plan_shape_supported = true;
  descriptor.candidate_set_available = true;
  descriptor.security_context_present = true;
  descriptor.security_snapshot_bound = true;
  descriptor.grants_proven = true;
  descriptor.engine_mga_authoritative = true;
  descriptor.exact_recheck_available = true;
  descriptor.exact_fallback_available = exact_fallback;
  descriptor.mga_visibility_recheck_required = true;
  descriptor.security_authorization_recheck_required = true;
  return descriptor;
}

page::PlanAwarePrefetchDescriptor PrefetchDescriptor(bool late_materialization) {
  page::PlanAwarePrefetchDescriptor descriptor;
  descriptor.family = late_materialization
                          ? page::PlanAwarePrefetchFamily::kLargePayload
                          : page::PlanAwarePrefetchFamily::kHeapPage;
  descriptor.item_id = late_materialization ? "payload:customer.bio" : "heap:customer:42";
  descriptor.physical_plan_node_id = "oeic052.scan.customer";
  descriptor.physical_plan_descriptor_digest = "sha256:oeic052-scan-descriptor";
  descriptor.physical_plan_generation = 52;
  descriptor.descriptor_generation = 52;
  descriptor.byte_cost = late_materialization ? 8192 : 4096;
  descriptor.page_cost = late_materialization ? 2 : 1;
  descriptor.full_payload_prefetch = late_materialization;
  descriptor.late_materialization_proof_present = late_materialization;
  descriptor.late_materialization_source =
      late_materialization ? "covering_index_row_id_stream" : "";
  descriptor.diagnostic_only_authority = true;
  return descriptor;
}

opt::PhysicalPlanPrefetchInput PrefetchInput(bool late_materialization = true) {
  opt::PhysicalPlanPrefetchInput input;
  input.physical_plan_generation = 52;
  input.descriptors = {PrefetchDescriptor(false), PrefetchDescriptor(late_materialization)};
  input.budget.max_bytes = 64 * 1024;
  input.budget.max_pages = 32;
  input.budget.max_items = 8;
  input.budget.max_outstanding = 4;
  return input;
}

opt::EnterpriseRuntimeFilterPrefetchMetricSnapshot Metrics() {
  opt::EnterpriseRuntimeFilterPrefetchMetricSnapshot metrics;
  metrics.metric_snapshot_id = "metrics:oeic052:v1";
  metrics.route_label = "embedded.local.oeic052";
  metrics.result_contract_hash = "sha256:oeic052-result-contract";
  metrics.source_provenance = "engine_runtime_metrics";
  metrics.trust_provenance = "engine_metric_registry";
  metrics.generation = 5201;
  metrics.route_epoch = 5202;
  metrics.stats_epoch = 5203;
  metrics.security_epoch = 5204;
  metrics.redaction_epoch = 5205;
  metrics.memory_epoch = 5206;
  metrics.runtime_filter_effectiveness_ppm = 850'000;
  metrics.prefetch_hit_rate_ppm = 920'000;
  metrics.prefetch_waste_ppm = 25'000;
  metrics.prefetch_latency_saved_units = 10'000;
  metrics.prefetch_io_cost_units = 900;
  metrics.late_materialization_recheck_rows = 1'500;
  metrics.fresh = true;
  metrics.trusted = true;
  metrics.engine_runtime_scope = true;
  metrics.redacted_for_explain = true;
  metrics.exact_fallback_available = true;
  return metrics;
}

opt::EnterpriseRuntimeFilterPrefetchRequest Request(bool late_materialization = true) {
  opt::EnterpriseRuntimeFilterPrefetchRequest request;
  request.plan_id = "oeic052.plan";
  request.runtime_filter_request.plan_id = "oeic052.plan";
  request.runtime_filter_request.candidates = {RuntimeFilter(true)};
  request.physical_plan_root = PhysicalPlanRoot();
  request.prefetch_input = PrefetchInput(late_materialization);
  request.metrics = Metrics();
  request.runtime_filter_requested = true;
  request.prefetch_requested = true;
  request.late_materialization_requested = late_materialization;
  return request;
}

bool SelectsRuntimeFilterPrefetchAndLateMaterialization() {
  const auto decision = opt::PlanEnterpriseRuntimeFilterPrefetch(Request(true));
  if (!Require(decision.ok, "enterprise runtime filter/prefetch was refused")) {
    return false;
  }
  return Require(decision.runtime_filter_selected,
                 "runtime filter was not selected from strong metrics") &&
         Require(decision.prefetch_scheduled,
                 "prefetch was not scheduled from strong metrics") &&
         Require(decision.late_materialization_planned,
                 "late materialization proof was not consumed") &&
         Require(Contains(decision.evidence, "OEIC_RUNTIME_FILTER_PREFETCH_CLOSURE"),
                 "closure evidence missing");
}

bool SuppressesPrefetchFromBadEffectivenessFeedback() {
  auto request = Request(false);
  request.late_materialization_requested = false;
  request.metrics.prefetch_hit_rate_ppm = 50'000;
  request.metrics.prefetch_waste_ppm = 900'000;
  request.metrics.prefetch_latency_saved_units = 1;
  request.metrics.prefetch_io_cost_units = 100;
  const auto decision = opt::PlanEnterpriseRuntimeFilterPrefetch(request);
  return Require(decision.ok, "weak prefetch feedback should select exact fallback") &&
         Require(decision.runtime_filter_selected,
                 "runtime filter should remain selected with strong filter feedback") &&
         Require(decision.prefetch_suppressed_by_feedback,
                 "prefetch was not suppressed by weak feedback") &&
         Require(decision.exact_fallback_selected,
                 "exact fallback was not selected for weak prefetch feedback");
}

bool RefusesWeakRuntimeFilterWithoutFallback() {
  auto request = Request(false);
  request.prefetch_requested = false;
  request.late_materialization_requested = false;
  request.runtime_filter_request.candidates = {RuntimeFilter(false)};
  request.metrics.exact_fallback_available = false;
  request.metrics.runtime_filter_effectiveness_ppm = 0;
  const auto decision = opt::PlanEnterpriseRuntimeFilterPrefetch(request);
  return Require(!decision.ok, "weak runtime filter without fallback was accepted") &&
         Require(decision.diagnostic_code ==
                     "SB_OPT_RUNTIME_FILTER_PREFETCH.FILTER_FEEDBACK_WEAK",
                 "weak filter diagnostic changed");
}

bool RefusesUnsafeMetricAuthority() {
  auto request = Request(false);
  request.metrics.cluster_route_or_metric_projection = true;
  const auto decision = opt::PlanEnterpriseRuntimeFilterPrefetch(request);
  return Require(!decision.ok, "cluster metric projection was accepted") &&
         Require(decision.diagnostic_code ==
                     "SB_OPT_RUNTIME_FILTER_PREFETCH.METRICS_REQUIRED",
                 "unsafe metric diagnostic changed");
}

bool RefusesLateMaterializationWithoutProof() {
  auto request = Request(true);
  request.prefetch_input.descriptors[1].late_materialization_proof_present = false;
  const auto decision = opt::PlanEnterpriseRuntimeFilterPrefetch(request);
  return Require(!decision.ok, "late materialization without proof was accepted") &&
         Require(decision.diagnostic_code ==
                     "SB_OPT_RUNTIME_FILTER_PREFETCH.LATE_MATERIALIZATION_PROOF_REQUIRED",
                 "late materialization proof diagnostic changed");
}

}  // namespace

int main() {
  // SEARCH_KEY: OEIC_RUNTIME_FILTER_PREFETCH_CLOSURE
  if (!SelectsRuntimeFilterPrefetchAndLateMaterialization()) return EXIT_FAILURE;
  if (!SuppressesPrefetchFromBadEffectivenessFeedback()) return EXIT_FAILURE;
  if (!RefusesWeakRuntimeFilterWithoutFallback()) return EXIT_FAILURE;
  if (!RefusesUnsafeMetricAuthority()) return EXIT_FAILURE;
  if (!RefusesLateMaterializationWithoutProof()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
