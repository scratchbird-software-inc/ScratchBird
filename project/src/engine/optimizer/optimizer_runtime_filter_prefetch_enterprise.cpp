// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_runtime_filter_prefetch_enterprise.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

EnterpriseRuntimeFilterPrefetchDecision Refuse(std::string diagnostic_code,
                                               std::string evidence) {
  EnterpriseRuntimeFilterPrefetchDecision result;
  result.ok = false;
  result.fail_closed = true;
  result.diagnostic_code = std::move(diagnostic_code);
  Add(&result.evidence, std::move(evidence));
  Add(&result.evidence, "OEIC_RUNTIME_FILTER_PREFETCH_CLOSURE");
  Add(&result.evidence, "runtime_filter_prefetch.fail_closed=true");
  Add(&result.evidence, "runtime_filter_prefetch.finality_authority=false");
  Add(&result.evidence, "runtime_filter_prefetch.visibility_authority=false");
  Add(&result.evidence, "runtime_filter_prefetch.security_authority=false");
  return result;
}

bool MetricsHaveUnsafeAuthority(
    const EnterpriseRuntimeFilterPrefetchMetricSnapshot& metrics) {
  return metrics.parser_or_reference_authority ||
         metrics.client_authority ||
         metrics.provider_finality_or_visibility_authority ||
         metrics.recovery_or_wal_authority ||
         metrics.cluster_route_or_metric_projection ||
         metrics.fixture_or_test_only;
}

bool MetricsCompleteAndTrusted(
    const EnterpriseRuntimeFilterPrefetchMetricSnapshot& metrics) {
  return !metrics.metric_snapshot_id.empty() &&
         !metrics.route_label.empty() &&
         !metrics.result_contract_hash.empty() &&
         metrics.source_provenance == "engine_runtime_metrics" &&
         !metrics.trust_provenance.empty() &&
         metrics.generation != 0 &&
         metrics.route_epoch != 0 &&
         metrics.stats_epoch != 0 &&
         metrics.security_epoch != 0 &&
         metrics.redaction_epoch != 0 &&
         metrics.memory_epoch != 0 &&
         metrics.fresh &&
         metrics.trusted &&
         metrics.engine_runtime_scope &&
         metrics.redacted_for_explain &&
         !MetricsHaveUnsafeAuthority(metrics);
}

bool AnyRuntimeFilterExactFallback(
    const RuntimeFilterPushdownRequest& request) {
  return std::any_of(request.candidates.begin(), request.candidates.end(),
                     [](const RuntimeFilterDescriptor& descriptor) {
                       return descriptor.exact_fallback_available;
                     });
}

bool AnyLateMaterializationDescriptor(
    const PhysicalPlanPrefetchInput& input) {
  return std::any_of(input.descriptors.begin(), input.descriptors.end(),
                     [](const scratchbird::storage::page::PlanAwarePrefetchDescriptor& descriptor) {
                       return descriptor.full_payload_prefetch ||
                              descriptor.late_materialization_proof_present ||
                              !descriptor.late_materialization_source.empty();
                     });
}

bool AllLateMaterializationDescriptorsHaveProof(
    const PhysicalPlanPrefetchInput& input) {
  return std::all_of(input.descriptors.begin(), input.descriptors.end(),
                     [](const scratchbird::storage::page::PlanAwarePrefetchDescriptor& descriptor) {
                       if (!descriptor.full_payload_prefetch &&
                           !descriptor.late_materialization_proof_present &&
                           descriptor.late_materialization_source.empty()) {
                         return true;
                       }
                       return descriptor.late_materialization_proof_present &&
                              !descriptor.late_materialization_source.empty() &&
                              descriptor.diagnostic_only_authority &&
                              !descriptor.prefetch_evidence_used_for_mga_or_security_authority;
                     });
}

bool RuntimeFilterFeedbackTooWeak(
    const EnterpriseRuntimeFilterPrefetchRequest& request) {
  return request.metrics.runtime_filter_effectiveness_ppm <
         request.min_runtime_filter_effectiveness_ppm;
}

bool PrefetchFeedbackTooWeak(
    const EnterpriseRuntimeFilterPrefetchRequest& request) {
  return request.metrics.prefetch_hit_rate_ppm <
             request.min_prefetch_hit_rate_ppm ||
         request.metrics.prefetch_waste_ppm >
             request.max_prefetch_waste_ppm ||
         request.metrics.prefetch_latency_saved_units <=
             request.metrics.prefetch_io_cost_units;
}

void AddCommonEvidence(EnterpriseRuntimeFilterPrefetchDecision* decision,
                       const EnterpriseRuntimeFilterPrefetchRequest& request) {
  Add(&decision->evidence, "OEIC_RUNTIME_FILTER_PREFETCH_CLOSURE");
  Add(&decision->evidence, "runtime_filter_prefetch.route_label=" +
                           request.metrics.route_label);
  Add(&decision->evidence, "runtime_filter_prefetch.result_contract_hash=" +
                           request.metrics.result_contract_hash);
  Add(&decision->evidence, "runtime_filter_prefetch.metric_snapshot_id=" +
                           request.metrics.metric_snapshot_id);
  Add(&decision->evidence,
      "runtime_filter_prefetch.metric_generation=" +
          std::to_string(request.metrics.generation));
  Add(&decision->evidence,
      "runtime_filter_prefetch.filter_effectiveness_ppm=" +
          std::to_string(request.metrics.runtime_filter_effectiveness_ppm));
  Add(&decision->evidence,
      "runtime_filter_prefetch.prefetch_hit_rate_ppm=" +
          std::to_string(request.metrics.prefetch_hit_rate_ppm));
  Add(&decision->evidence,
      "runtime_filter_prefetch.prefetch_waste_ppm=" +
          std::to_string(request.metrics.prefetch_waste_ppm));
  Add(&decision->evidence, "runtime_filter_prefetch.exact_fallback_available=" +
                           std::string(request.metrics.exact_fallback_available
                                           ? "true"
                                           : "false"));
  Add(&decision->evidence, "runtime_filter_prefetch.candidate_rows_only=true");
  Add(&decision->evidence, "runtime_filter_prefetch.exact_recheck_required=true");
  Add(&decision->evidence,
      "runtime_filter_prefetch.mga_visibility_recheck_required=true");
  Add(&decision->evidence,
      "runtime_filter_prefetch.security_recheck_required=true");
}

}  // namespace

EnterpriseRuntimeFilterPrefetchDecision PlanEnterpriseRuntimeFilterPrefetch(
    const EnterpriseRuntimeFilterPrefetchRequest& request) {
  if (request.plan_id.empty() || request.plan_id != request.runtime_filter_request.plan_id) {
    return Refuse("SB_OPT_RUNTIME_FILTER_PREFETCH.PLAN_ID_REQUIRED",
                  "runtime_filter_prefetch_plan_identity_required");
  }
  if (!MetricsCompleteAndTrusted(request.metrics)) {
    return Refuse("SB_OPT_RUNTIME_FILTER_PREFETCH.METRICS_REQUIRED",
                  "runtime_filter_prefetch_fresh_trusted_metrics_required");
  }

  EnterpriseRuntimeFilterPrefetchDecision decision;
  decision.ok = true;
  decision.fail_closed = false;
  decision.diagnostic_code = "SB_OPT_RUNTIME_FILTER_PREFETCH.OK";
  AddCommonEvidence(&decision, request);

  const bool runtime_filter_has_exact_fallback =
      request.metrics.exact_fallback_available ||
      AnyRuntimeFilterExactFallback(request.runtime_filter_request);

  if (request.runtime_filter_requested) {
    if (RuntimeFilterFeedbackTooWeak(request)) {
      if (!runtime_filter_has_exact_fallback) {
        return Refuse("SB_OPT_RUNTIME_FILTER_PREFETCH.FILTER_FEEDBACK_WEAK",
                      "runtime_filter_feedback_weak_without_exact_fallback");
      }
      decision.runtime_filter_suppressed_by_feedback = true;
      decision.exact_fallback_selected = true;
      Add(&decision.evidence,
          "runtime_filter_prefetch.runtime_filter_suppressed_by_feedback=true");
      Add(&decision.evidence,
          "runtime_filter_prefetch.exact_fallback_selected=true");
    } else {
      decision.runtime_filter_decision =
          EvaluateRuntimeFilterPushdown(request.runtime_filter_request);
      if (decision.runtime_filter_decision.fail_closed) {
        decision.ok = false;
        decision.fail_closed = true;
        decision.diagnostic_code =
            decision.runtime_filter_decision.diagnostic_code;
        decision.evidence.insert(decision.evidence.end(),
                                 decision.runtime_filter_decision.evidence.begin(),
                                 decision.runtime_filter_decision.evidence.end());
        return decision;
      }
      decision.runtime_filter_selected =
          !decision.runtime_filter_decision.selected_filters.empty();
      decision.exact_fallback_selected =
          decision.runtime_filter_decision.diagnostic_code ==
          "SB_RUNTIME_FILTER.EXACT_FALLBACK_ONLY";
      decision.evidence.insert(decision.evidence.end(),
                               decision.runtime_filter_decision.evidence.begin(),
                               decision.runtime_filter_decision.evidence.end());
    }
  }

  if (request.late_materialization_requested) {
    if (!AnyLateMaterializationDescriptor(request.prefetch_input) ||
        !AllLateMaterializationDescriptorsHaveProof(request.prefetch_input) ||
        request.metrics.late_materialization_recheck_rows == 0) {
      return Refuse("SB_OPT_RUNTIME_FILTER_PREFETCH.LATE_MATERIALIZATION_PROOF_REQUIRED",
                    "late_materialization_recheck_and_fetch_proof_required");
    }
    decision.late_materialization_planned = true;
    Add(&decision.evidence,
        "runtime_filter_prefetch.late_materialization_planned=true");
    Add(&decision.evidence,
        "runtime_filter_prefetch.late_materialization_exact_recheck_rows=" +
            std::to_string(request.metrics.late_materialization_recheck_rows));
  }

  if (request.prefetch_requested) {
    if (PrefetchFeedbackTooWeak(request)) {
      if (!request.metrics.exact_fallback_available) {
        return Refuse("SB_OPT_RUNTIME_FILTER_PREFETCH.PREFETCH_FEEDBACK_WEAK",
                      "prefetch_feedback_weak_without_exact_fallback");
      }
      decision.prefetch_suppressed_by_feedback = true;
      decision.exact_fallback_selected = true;
      Add(&decision.evidence,
          "runtime_filter_prefetch.prefetch_suppressed_by_feedback=true");
      Add(&decision.evidence,
          "runtime_filter_prefetch.exact_fallback_selected=true");
    } else {
      decision.prefetch_result = ExecutePhysicalPlanDrivenPrefetch(
          request.physical_plan_root, request.prefetch_input);
      if (!decision.prefetch_result.ok()) {
        decision.ok = false;
        decision.fail_closed = true;
        decision.diagnostic_code =
            decision.prefetch_result.diagnostic.diagnostic_code.empty()
                ? "SB_OPT_RUNTIME_FILTER_PREFETCH.PREFETCH_REFUSED"
                : decision.prefetch_result.diagnostic.diagnostic_code;
        decision.evidence.insert(decision.evidence.end(),
                                 decision.prefetch_result.evidence.begin(),
                                 decision.prefetch_result.evidence.end());
        return decision;
      }
      decision.prefetch_scheduled =
          decision.prefetch_result.counters.scheduled_items != 0;
      decision.evidence.insert(decision.evidence.end(),
                               decision.prefetch_result.evidence.begin(),
                               decision.prefetch_result.evidence.end());
      Add(&decision.evidence,
          "runtime_filter_prefetch.prefetch_scheduled=" +
              std::string(decision.prefetch_scheduled ? "true" : "false"));
    }
  }

  if (!decision.runtime_filter_selected &&
      !decision.runtime_filter_suppressed_by_feedback &&
      !decision.prefetch_scheduled &&
      !decision.prefetch_suppressed_by_feedback &&
      !decision.late_materialization_planned) {
    return Refuse("SB_OPT_RUNTIME_FILTER_PREFETCH.NO_SAFE_WORK",
                  "runtime_filter_prefetch_no_safe_work_selected");
  }

  return decision;
}

}  // namespace scratchbird::engine::optimizer
