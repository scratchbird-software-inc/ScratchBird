// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_optimizer_integration.hpp"

#include "policy_blocked_index_admission.hpp"

#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status WarnStatus() { return {StatusCode::ok, Severity::warning, Subsystem::engine}; }
Status RefuseStatus() { return {StatusCode::platform_required_feature_missing, Severity::warning, Subsystem::engine}; }

u64 PageSpanForSummary(const PageExtentSummaryMetadata& metadata) {
  if (metadata.range.page_count != 0) {
    return metadata.range.page_count;
  }
  return metadata.range.extent_count;
}

bool SameScalarType(const PageExtentSummaryMetadata& metadata,
                    const PageExtentSummaryPrunePredicate& predicate) {
  return predicate.scalar_type_key.empty() ||
         metadata.boundary.scalar_type_key.empty() ||
         metadata.boundary.scalar_type_key == predicate.scalar_type_key;
}

bool EncodedLess(std::string_view left, std::string_view right) {
  return left.compare(right) < 0;
}

bool EncodedGreater(std::string_view left, std::string_view right) {
  return left.compare(right) > 0;
}

bool EncodedEqual(std::string_view left, std::string_view right) {
  return left == right;
}

bool UnsafePlanAdmissionAuthority(
    const IndexReadinessPlanAdmissionEvidence& evidence) {
  return evidence.parser_authority ||
         evidence.client_authority ||
         evidence.donor_authority ||
         evidence.wal_authority ||
         evidence.recovery_authority ||
         evidence.transaction_finality_authority ||
         evidence.visibility_authority ||
         evidence.security_authority ||
         evidence.provider_finality_authority ||
         evidence.index_finality_authority ||
         evidence.optimizer_plan_authority ||
         evidence.agent_authority ||
         evidence.local_cluster_authority ||
         evidence.external_cluster_runtime_overclaim;
}

bool GeneratedReadinessEvidenceCurrent(
    const IndexReadinessPlanAdmissionEvidence& evidence) {
  return evidence.generated_manifest_present &&
         evidence.generated_manifest_current &&
         evidence.generated_manifest_validated &&
         evidence.source_digest_matches &&
         evidence.manifest_epoch != 0 &&
         evidence.registry_epoch != 0 &&
         evidence.route_proof_epoch != 0 &&
         !evidence.source_evidence_digest.empty() &&
         !evidence.generated_by.empty() &&
         !evidence.static_registry_only &&
         !evidence.smoke_only &&
         !evidence.drifted &&
         !evidence.placeholder_epoch &&
         !evidence.local_default_evidence &&
         !evidence.policy_default_evidence &&
         !evidence.synthetic_evidence &&
         !evidence.test_fixture_evidence;
}

bool PersistentRouteAdmissionFamily(IndexFamily family) {
  const auto* descriptor = FindBuiltinIndexFamily(family);
  return descriptor != nullptr &&
         descriptor->persistence == IndexPersistenceClass::persistent;
}

bool CategoryImpliesEquality(IndexPlanCategory category) {
  return category == IndexPlanCategory::point_lookup;
}

bool CategoryImpliesRange(IndexPlanCategory category) {
  return category == IndexPlanCategory::range_scan;
}

bool CategoryImpliesSummaryPrune(IndexPlanCategory category) {
  return category == IndexPlanCategory::summary_prune;
}

bool CategoryImpliesCandidateSet(IndexPlanCategory category) {
  return category == IndexPlanCategory::bitmap_combine ||
         category == IndexPlanCategory::inverted_search ||
         category == IndexPlanCategory::spatial_search ||
         category == IndexPlanCategory::vector_search ||
         category == IndexPlanCategory::graph_search;
}

bool NegativePruneFamily(IndexFamily family) {
  return family == IndexFamily::bloom;
}

IndexOptimizerPlan MakeOptimizerRefusal(
    const IndexOptimizerRequest& request,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail,
    std::string step) {
  IndexOptimizerPlan plan;
  plan.route = request.route;
  plan.status = RefuseStatus();
  plan.fallback_full_scan = true;
  plan.diagnostic = MakeIndexOptimizerIntegrationDiagnostic(
      plan.status, std::move(diagnostic_code), std::move(message_key),
      std::move(detail));
  plan.steps.push_back(std::move(step));
  return plan;
}

IndexOptimizerPlan ValidateReadinessPlanAdmission(
    const IndexOptimizerRequest& request,
    const IndexRouteCapabilityState& route_state) {
  const auto* evidence = request.readiness_evidence;
  if (evidence == nullptr) {
    return MakeOptimizerRefusal(
        request,
        "INDEX.OPTIMIZER_READINESS_EVIDENCE.MISSING",
        "index.optimizer_readiness_evidence.missing",
        "generated CEIC-030/042 readiness evidence is required",
        "readiness_evidence_missing_fail_closed");
  }
  if (!GeneratedReadinessEvidenceCurrent(*evidence)) {
    return MakeOptimizerRefusal(
        request,
        "INDEX.OPTIMIZER_READINESS_EVIDENCE.STALE_OR_STATIC",
        "index.optimizer_readiness_evidence.stale_or_static",
        "manifest evidence must be generated current non-smoke non-synthetic proof",
        "readiness_manifest_not_current_fail_closed");
  }
  if (evidence->family != request.family ||
      evidence->route != request.route ||
      !evidence->runtime_registry_family_matches ||
      !evidence->runtime_registry_route_matches) {
    return MakeOptimizerRefusal(
        request,
        "INDEX.OPTIMIZER_READINESS_EVIDENCE.ROUTE_FAMILY_MISMATCH",
        "index.optimizer_readiness_evidence.route_family_mismatch",
        std::string("route=") + IndexRouteKindName(request.route) +
            ";family=" + IndexFamilyName(request.family),
        "readiness_route_family_mismatch_fail_closed");
  }
  if (evidence->donor_emulated ||
      evidence->policy_blocked ||
      evidence->contract_only_family) {
    return MakeOptimizerRefusal(
        request,
        "INDEX.OPTIMIZER_READINESS_EVIDENCE.NON_RUNTIME_FAMILY",
        "index.optimizer_readiness_evidence.non_runtime_family",
        IndexFamilyName(request.family),
        "readiness_non_runtime_family_fail_closed");
  }
  if (UnsafePlanAdmissionAuthority(*evidence) ||
      !evidence->external_cluster_provider_only) {
    return MakeOptimizerRefusal(
        request,
        "INDEX.OPTIMIZER_READINESS_EVIDENCE.UNSAFE_AUTHORITY",
        "index.optimizer_readiness_evidence.unsafe_authority",
        "parser/client/donor/WAL/recovery/finality/visibility/security/provider/index/optimizer/agent/local-cluster authority refused",
        "readiness_unsafe_authority_fail_closed");
  }
  if (!evidence->runtime_family_available ||
      !evidence->runtime_route_complete ||
      !evidence->supports_read ||
      evidence->supports_read != route_state.supports_read ||
      evidence->supports_equality_lookup != route_state.supports_equality_lookup ||
      evidence->supports_ordered_range != route_state.supports_ordered_range ||
      evidence->supports_negative_prune != route_state.supports_negative_prune ||
      evidence->supports_summary_segment_prune !=
          route_state.supports_summary_segment_prune ||
      evidence->produces_candidate_set != route_state.produces_candidate_set ||
      evidence->approximate_candidate_source !=
          route_state.approximate_candidate_source ||
      evidence->requires_exact_recheck != route_state.requires_exact_recheck ||
      evidence->requires_mga_recheck != route_state.requires_mga_recheck ||
      evidence->requires_security_recheck !=
          route_state.requires_security_recheck ||
      evidence->requires_exact_rerank != route_state.requires_exact_rerank) {
    return MakeOptimizerRefusal(
        request,
        "INDEX.OPTIMIZER_READINESS_EVIDENCE.RUNTIME_CAPABILITY_DRIFT",
        "index.optimizer_readiness_evidence.runtime_capability_drift",
        "generated readiness route proof does not match runtime route capability",
        "readiness_runtime_capability_drift_fail_closed");
  }

  const bool requires_equality =
      request.requires_equality_lookup || CategoryImpliesEquality(request.category);
  const bool requires_range =
      request.requires_range_scan || CategoryImpliesRange(request.category);
  const bool requires_summary =
      request.requires_summary_segment_prune ||
      CategoryImpliesSummaryPrune(request.category);
  const bool requires_negative =
      request.requires_negative_prune || NegativePruneFamily(request.family);
  const bool requires_candidate =
      request.requires_candidate_set ||
      CategoryImpliesCandidateSet(request.category) ||
      request.approximate;
  if ((requires_equality && !evidence->supports_equality_lookup) ||
      (requires_range && !evidence->supports_ordered_range) ||
      (request.requires_order &&
       (!request.order_proven || !evidence->supports_ordered_range)) ||
      (requires_negative && !evidence->supports_negative_prune) ||
      (requires_summary && !evidence->supports_summary_segment_prune) ||
      (requires_candidate && !evidence->produces_candidate_set) ||
      (request.approximate && !evidence->approximate_candidate_source)) {
    return MakeOptimizerRefusal(
        request,
        "INDEX.OPTIMIZER_READINESS_EVIDENCE.PLAN_SEMANTIC_MISMATCH",
        "index.optimizer_readiness_evidence.plan_semantic_mismatch",
        "requested index plan semantics are not supported by the route proof",
        "readiness_plan_semantic_mismatch_fail_closed");
  }
  if (request.requires_exact_rows &&
      (evidence->approximate_candidate_source ||
       evidence->produces_candidate_set ||
       evidence->supports_negative_prune ||
       evidence->supports_summary_segment_prune) &&
      (!evidence->exact_recheck_proven ||
       !evidence->mga_recheck_proven ||
       !evidence->security_recheck_proven)) {
    return MakeOptimizerRefusal(
        request,
        "INDEX.OPTIMIZER_READINESS_EVIDENCE.EXACT_RECHECK_REQUIRED",
        "index.optimizer_readiness_evidence.exact_recheck_required",
        "candidate/prune/approximate routes require exact MGA security recheck proof",
        "readiness_exact_recheck_missing_fail_closed");
  }
  if ((route_state.requires_exact_recheck ||
       route_state.requires_mga_recheck ||
       route_state.requires_security_recheck) &&
      (!evidence->exact_recheck_proven ||
       !evidence->mga_recheck_proven ||
       !evidence->security_recheck_proven)) {
    return MakeOptimizerRefusal(
        request,
        "INDEX.OPTIMIZER_READINESS_EVIDENCE.RECHECK_PROOF_REQUIRED",
        "index.optimizer_readiness_evidence.recheck_proof_required",
        "route requires exact, MGA, and security recheck proof",
        "readiness_route_recheck_missing_fail_closed");
  }
  if (route_state.requires_exact_rerank &&
      (!request.exact_rerank_available || !evidence->exact_rerank_proven)) {
    return MakeOptimizerRefusal(
        request,
        "INDEX.OPTIMIZER_READINESS_EVIDENCE.EXACT_RERANK_REQUIRED",
        "index.optimizer_readiness_evidence.exact_rerank_required",
        "route requires exact rerank proof",
        "readiness_exact_rerank_missing_fail_closed");
  }
  if (!evidence->operation_metrics_producer_proven ||
      !evidence->support_bundle_producer_proven) {
    return MakeOptimizerRefusal(
        request,
        "INDEX.OPTIMIZER_READINESS_EVIDENCE.METRICS_PROOF_REQUIRED",
        "index.optimizer_readiness_evidence.metrics_proof_required",
        "CEIC-040 operation metrics and support-bundle producer proof required",
        "readiness_metrics_proof_missing_fail_closed");
  }
  if (PersistentRouteAdmissionFamily(request.family) &&
      (!evidence->crash_reopen_proven ||
       !evidence->corruption_cleanup_proven ||
       !evidence->cleanup_horizon_proven ||
       !evidence->storage_integration_proven)) {
    return MakeOptimizerRefusal(
        request,
        "INDEX.OPTIMIZER_READINESS_EVIDENCE.PERSISTENT_PROOF_REQUIRED",
        "index.optimizer_readiness_evidence.persistent_proof_required",
        "persistent route admission requires storage, crash, corruption, and cleanup proof",
        "readiness_persistent_proof_missing_fail_closed");
  }

  IndexOptimizerPlan ok;
  ok.status = OkStatus();
  ok.route = request.route;
  ok.admitted = true;
  return ok;
}

bool SummaryOutsidePredicate(const PageExtentSummaryMetadata& metadata,
                             const PageExtentSummaryPrunePredicate& predicate) {
  const bool has_non_null_bounds =
      metadata.boundary.min_present && metadata.boundary.max_present;
  if (!has_non_null_bounds) {
    return predicate.lower_present || predicate.upper_present;
  }
  if (predicate.lower_present) {
    if (EncodedLess(metadata.boundary.encoded_max, predicate.encoded_lower)) {
      return true;
    }
    if (EncodedEqual(metadata.boundary.encoded_max, predicate.encoded_lower) &&
        (!metadata.boundary.max_inclusive || !predicate.lower_inclusive)) {
      return true;
    }
  }
  if (predicate.upper_present) {
    if (EncodedGreater(metadata.boundary.encoded_min, predicate.encoded_upper)) {
      return true;
    }
    if (EncodedEqual(metadata.boundary.encoded_min, predicate.encoded_upper) &&
        (!metadata.boundary.min_inclusive || !predicate.upper_inclusive)) {
      return true;
    }
  }
  return false;
}

PageExtentSummaryPrunePlan MakePruneFallback(
    Status status,
    PageExtentSummaryFallbackReason reason,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  PageExtentSummaryPrunePlan plan;
  plan.status = status;
  plan.selected_category = IndexPlanCategory::fallback_full_scan;
  plan.selected_access = "full_scan";
  plan.prune_reason = "none";
  plan.fallback_reason = PageExtentSummaryFallbackReasonName(reason);
  plan.full_scan_fallback = true;
  plan.summary_prune_selected = false;
  plan.base_row_mga_recheck_required = true;
  plan.base_row_security_recheck_required = true;
  plan.authority_source = "engine_mga_base_pages";
  plan.diagnostic = MakeIndexOptimizerIntegrationDiagnostic(
      plan.status, std::move(diagnostic_code), std::move(message_key),
      std::move(detail));
  plan.actions.push_back("select_full_scan_fallback");
  plan.actions.push_back("do_not_use_summary_metadata_as_visibility_or_finality_authority");
  return plan;
}
}  // namespace

PageExtentSummaryPrunePlan PlanPageExtentSummaryPrune(
    const PageExtentSummaryPruneRequest& request) {
  if (!request.summary_prune_enabled) {
    auto plan = MakePruneFallback(
        WarnStatus(),
        PageExtentSummaryFallbackReason::missing_summary_full_scan,
        "INDEX.PAGE_SUMMARY_PRUNE.DISABLED_FULL_SCAN",
        "index.page_summary_prune.disabled_full_scan");
    plan.summary_status = "disabled";
    plan.actions.push_back("summary_prune_feature_disabled");
    return plan;
  }
  if (!request.base_row_mga_recheck_required ||
      !request.base_row_security_recheck_required) {
    auto plan = MakePruneFallback(
        WarnStatus(),
        PageExtentSummaryFallbackReason::external_finality_authority_full_scan,
        "INDEX.PAGE_SUMMARY_PRUNE.BASE_ROW_RECHECK_REQUIRED",
        "index.page_summary_prune.base_row_recheck_required");
    plan.summary_status = "authority_refused";
    plan.actions.push_back("require_base_row_mga_and_security_recheck");
    return plan;
  }
  if (request.summaries.empty()) {
    auto plan = MakePruneFallback(
        WarnStatus(),
        PageExtentSummaryFallbackReason::missing_summary_full_scan,
        "INDEX.PAGE_SUMMARY_PRUNE.MISSING_FULL_SCAN",
        "index.page_summary_prune.missing_full_scan");
    plan.summary_status = PageExtentSummaryStatusName(PageExtentSummaryStatus::missing);
    return plan;
  }

  PageExtentSummaryPrunePlan plan;
  plan.status = OkStatus();
  plan.selected_category = IndexPlanCategory::summary_prune;
  plan.selected_access = "summary_prune";
  plan.prune_reason = "summary_bounds_current";
  plan.fallback_reason = PageExtentSummaryFallbackReasonName(
      PageExtentSummaryFallbackReason::none);
  plan.summary_status = PageExtentSummaryStatusName(PageExtentSummaryStatus::current);
  plan.summary_prune_selected = true;
  plan.full_scan_fallback = false;
  plan.base_row_mga_recheck_required = true;
  plan.base_row_security_recheck_required = true;
  plan.authority_source = "engine_mga_base_pages";

  for (const auto& summary : request.summaries) {
    ++plan.counters.candidate_ranges;
    plan.counters.pages_considered += PageSpanForSummary(summary);
    if (summary.generation > plan.summary_generation) {
      plan.summary_generation = summary.generation;
    }
    const auto decision =
        ClassifyPageExtentSummaryForUse(summary, request.format);
    if (!decision.summary_usable) {
      auto fallback = MakePruneFallback(
          WarnStatus(),
          decision.fallback_reason,
          "INDEX.PAGE_SUMMARY_PRUNE.CLASSIFIER_FALLBACK",
          "index.page_summary_prune.classifier_fallback",
          decision.diagnostic.diagnostic_code);
      fallback.counters = plan.counters;
      fallback.summary_status = PageExtentSummaryStatusName(summary.status);
      fallback.summary_generation = summary.generation;
      fallback.actions.insert(fallback.actions.end(),
                              decision.actions.begin(),
                              decision.actions.end());
      return fallback;
    }
    if (!SameScalarType(summary, request.predicate)) {
      auto fallback = MakePruneFallback(
          WarnStatus(),
          PageExtentSummaryFallbackReason::incompatible_summary_full_scan,
          "INDEX.PAGE_SUMMARY_PRUNE.SCALAR_TYPE_INCOMPATIBLE",
          "index.page_summary_prune.scalar_type_incompatible");
      fallback.counters = plan.counters;
      fallback.summary_status = PageExtentSummaryStatusName(summary.status);
      fallback.summary_generation = summary.generation;
      fallback.actions.push_back("refuse_summary_prune_for_incompatible_predicate_type");
      return fallback;
    }

    const auto pages = PageSpanForSummary(summary);
    if (SummaryOutsidePredicate(summary, request.predicate)) {
      ++plan.counters.ranges_pruned;
      plan.counters.pages_pruned += pages;
    } else {
      ++plan.counters.ranges_scanned;
      plan.counters.pages_scanned += pages;
    }
  }

  plan.diagnostic = MakeIndexOptimizerIntegrationDiagnostic(
      plan.status,
      "INDEX.PAGE_SUMMARY_PRUNE.SELECTED",
      "index.page_summary_prune.selected");
  plan.actions.push_back("use_current_page_extent_summary_bounds_for_prune_admission");
  plan.actions.push_back("scan_candidate_ranges_with_base_row_mga_recheck");
  plan.actions.push_back("scan_candidate_ranges_with_security_recheck");
  plan.actions.push_back("do_not_use_summary_metadata_as_visibility_or_finality_authority");
  return plan;
}

IndexOptimizerPlan PlanIndexOptimizerPath(const IndexOptimizerRequest& request) {
  IndexOptimizerPlan plan;
  plan.route = request.route;
  if (!request.index_uuid.valid() || request.family == IndexFamily::unknown) {
    plan.status = RefuseStatus();
    plan.fallback_full_scan = true;
    plan.diagnostic = MakeIndexOptimizerIntegrationDiagnostic(plan.status,
                                                             "SB-INDEX-OPTIMIZER-INVALID-REQUEST",
                                                             "index.optimizer.invalid_request");
    return plan;
  }
  const auto policy_blocked = EvaluatePolicyBlockedIndexAdmission(
      MakePolicyBlockedIndexRouteRequest(request.family,
                                         "",
                                         "optimizer.index_path",
                                         "full_scan",
                                         true));
  if (policy_blocked.fail_closed) {
    plan.status = policy_blocked.status;
    plan.fallback_full_scan = policy_blocked.fallback_available;
    plan.diagnostic = policy_blocked.diagnostic;
    plan.steps.push_back("policy_blocked_fail_closed_before_physical_planning");
    return plan;
  }
  const auto* capability =
      FindBuiltinIndexFamilyPhysicalCapabilityState(request.family);
  if (capability == nullptr || !capability->runtime_available) {
    plan.status = RefuseStatus();
    plan.fallback_full_scan = true;
    if (capability != nullptr) {
      plan.diagnostic =
          MakeIndexFamilyCapabilityBlockerDiagnostic(plan.status, *capability);
    } else {
      plan.diagnostic = MakeIndexFamilyCapabilityDiagnostic(
          plan.status,
          "INDEX.CAPABILITY.UNKNOWN_FAMILY",
          "index.capability.unknown_family",
          IndexFamilyName(request.family),
          "family has no physical capability state",
          IndexFamilyPhysicalCapabilityBlocker::unknown_family);
    }
    return plan;
  }
  const auto* route_state =
      FindBuiltinIndexRouteCapabilityState(request.route, request.family);
  const auto caps = CapabilitiesForFamily(request.family);
  const bool specialized_route_scan =
      request.family == IndexFamily::bitmap && route_state != nullptr &&
      route_state->route_complete() && route_state->supports_read;
  if (!caps.supports_scan && !specialized_route_scan) {
    plan.status = RefuseStatus();
    plan.fallback_full_scan = true;
    plan.diagnostic =
        MakeIndexFamilyCapabilityBlockerDiagnostic(plan.status, *capability);
    return plan;
  }
  if (route_state == nullptr || !route_state->route_complete() ||
      !route_state->supports_read) {
    plan.status = RefuseStatus();
    plan.fallback_full_scan = true;
    if (route_state != nullptr) {
      plan.diagnostic =
          MakeIndexRouteCapabilityDiagnostic(plan.status, *route_state);
    } else {
      plan.diagnostic = MakeIndexOptimizerIntegrationDiagnostic(
          plan.status,
          "INDEX.ROUTE_CAPABILITY.UNKNOWN_ROUTE",
          "index.route_capability.unknown_route",
          std::string("route=") + IndexRouteKindName(request.route) +
              ";family=" + IndexFamilyName(request.family));
    }
    plan.steps.push_back("route_capability_refused");
    return plan;
  }
  auto readiness_admission =
      ValidateReadinessPlanAdmission(request, *route_state);
  if (!readiness_admission.ok()) {
    return readiness_admission;
  }
  plan.steps.push_back("ceic_060_index_readiness_plan_admission=accepted");
  plan.steps.push_back("generated_index_readiness_manifest_validated=true");
  plan.steps.push_back("index_readiness_route_runtime_proof=true");
  plan.steps.push_back("index_readiness_exact_mga_security_recheck_proof=true");
  plan.steps.push_back("index_readiness_metrics_support_bundle_proof=true");
  plan.steps.push_back("index_readiness_crash_cleanup_storage_proof=true");
  plan.steps.push_back("cluster_external_provider_only=true");
  if (route_state->requires_exact_rerank &&
      !request.exact_rerank_available) {
    plan.status = RefuseStatus();
    plan.fallback_full_scan = true;
    plan.diagnostic = MakeIndexOptimizerIntegrationDiagnostic(
        plan.status,
        "INDEX.ROUTE_CAPABILITY.EXACT_RERANK_REQUIRED",
        "index.route_capability.exact_rerank_required",
        std::string("route=") + IndexRouteKindName(request.route) +
            ";family=" + IndexFamilyName(request.family));
    plan.steps.push_back("route_requires_exact_rerank");
    return plan;
  }
  if (request.family == IndexFamily::hash &&
      route_state->hash_requires_keyed_algorithm &&
      !request.hash_keyed_algorithm_active &&
      !request.hash_legacy_algorithm_allowed_by_policy) {
    plan.status = RefuseStatus();
    plan.fallback_full_scan = true;
    plan.diagnostic = MakeIndexOptimizerIntegrationDiagnostic(
        plan.status,
        "INDEX.ROUTE_CAPABILITY.KEYED_HASH_REQUIRED",
        "index.route_capability.keyed_hash_required",
        std::string("route=") + IndexRouteKindName(request.route) +
            ";family=" + IndexFamilyName(request.family));
    plan.steps.push_back("hash_route_requires_keyed_algorithm");
    return plan;
  }
  if (request.family == IndexFamily::hash &&
      request.hash_high_assurance_required &&
      !request.hash_high_assurance_active) {
    plan.status = RefuseStatus();
    plan.fallback_full_scan = true;
    plan.diagnostic = MakeIndexOptimizerIntegrationDiagnostic(
        plan.status,
        "INDEX.ROUTE_CAPABILITY.HIGH_ASSURANCE_HASH_REQUIRED",
        "index.route_capability.high_assurance_hash_required",
        std::string("route=") + IndexRouteKindName(request.route) +
            ";family=" + IndexFamilyName(request.family));
    plan.steps.push_back("hash_route_requires_high_assurance_fingerprint");
    return plan;
  }
  plan.status = OkStatus();
  plan.admitted = true;
  plan.route_benchmark_clean = route_state->benchmark_clean;
  plan.route_capability = "benchmark_clean";
  plan.exact_recheck = request.mga_recheck_required ||
                       request.security_recheck_required ||
                       request.approximate ||
                       route_state->requires_exact_recheck;
  plan.rerank = (request.approximate || route_state->requires_exact_rerank) &&
                request.exact_rerank_available;
  plan.fallback_sort = request.requires_order && !request.order_proven;
  plan.index_only = request.covering_requested && request.covering_payload_fresh && !request.security_recheck_required;
  plan.multi_index_allowed = request.multi_index_requested && request.multi_index_inputs >= 2;
  plan.cost_multiplier = request.stats_available && !request.stats_stale ? (1.0 + request.selectivity) : 10.0;
  if (request.stats_stale) {
    plan.steps.push_back("mark_stats_stale_and_request_refresh");
  }
  plan.steps.push_back(std::string("route_kind=") +
                       IndexRouteKindName(request.route));
  plan.steps.insert(plan.steps.end(),
                    route_state->evidence.begin(),
                    route_state->evidence.end());
  plan.steps.push_back("derive_exactness_flags");
  plan.steps.push_back(plan.fallback_sort ? "attach_fallback_sort" : "prove_order_or_no_order_required");
  plan.steps.push_back(plan.index_only ? "admit_index_only_path" : "require_base_row_fetch_or_recheck");
  if (plan.multi_index_allowed) {
    plan.steps.push_back("admit_multi_index_intersection_or_union");
  }
  return plan;
}

IndexOptimizerPlan PlanIndexExecutorDispatch(const IndexOptimizerRequest& request) {
  IndexOptimizerPlan plan = PlanIndexOptimizerPath(request);
  if (!plan.ok()) {
    return plan;
  }
  plan.steps.push_back("dispatch_to_family_access_method");
  plan.steps.push_back("publish_candidate_metrics");
  plan.steps.push_back("apply_mga_security_predicate_recheck");
  return plan;
}

DiagnosticRecord MakeIndexOptimizerIntegrationDiagnostic(Status status,
                                                         std::string diagnostic_code,
                                                         std::string message_key,
                                                         std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code, status.severity, status.subsystem,
                        std::move(diagnostic_code), std::move(message_key),
                        std::move(arguments), {}, "core.index.optimizer_integration");
}

}  // namespace scratchbird::core::index
