// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_explain.hpp"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

std::string JsonEscape(std::string_view input) {
  std::ostringstream out;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      default: out << ch;
    }
  }
  return out.str();
}

void RenderStringArray(std::ostringstream& out, const std::vector<std::string>& values) {
  out << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out << ",";
    out << "\"" << JsonEscape(values[i]) << "\"";
  }
  out << "]";
}

std::uint64_t StableHashAppend(std::uint64_t hash, std::string_view value) {
  for (const unsigned char ch : value) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string StableRuntimePlanHash(const BoundOptimizerRequest& request,
                                  const BoundOptimizerResult& result) {
  std::uint64_t hash = 1469598103934665603ull;
  hash = StableHashAppend(hash, request.context.operation_id);
  hash = StableHashAppend(hash, request.context.sblr_digest);
  hash = StableHashAppend(hash, request.context.descriptor_set_digest);
  hash = StableHashAppend(hash, request.context.statistics_snapshot_id);
  hash = StableHashAppend(hash, request.context.executor_capability_set_id);
  hash = StableHashAppend(hash, result.plan_id);
  hash = StableHashAppend(hash, result.optimizer_profile);
  for (const auto& candidate : result.candidates) {
    hash = StableHashAppend(hash, candidate.candidate_id);
    hash = StableHashAppend(hash, scratchbird::engine::planner::PhysicalAccessKindName(candidate.access_kind));
    hash = StableHashAppend(hash, candidate.selected ? "selected" : "not_selected");
  }
  std::ostringstream out;
  out << "runtime-plan-fnv64:" << std::hex << hash;
  return out.str();
}

void AddUnique(std::vector<std::string>* values, std::string value) {
  if (values == nullptr || value.empty()) return;
  if (std::find(values->begin(), values->end(), value) == values->end()) {
    values->push_back(std::move(value));
  }
}

void AddIfPrefixed(std::vector<std::string>* values,
                   const std::string& evidence,
                   std::string_view prefix) {
  if (evidence.rfind(prefix, 0) == 0) AddUnique(values, evidence);
}

void RenderSummaryPruneEvidence(std::ostringstream& out,
                                const PlanSummaryPruneEvidence& evidence) {
  out << "{\"present\":" << (evidence.present ? "true" : "false")
      << ",\"selected_access\":\"" << JsonEscape(evidence.selected_access)
      << "\",\"prune_reason\":\"" << JsonEscape(evidence.prune_reason)
      << "\",\"fallback_reason\":\"" << JsonEscape(evidence.fallback_reason)
      << "\",\"summary_status\":\"" << JsonEscape(evidence.summary_status)
      << "\",\"summary_generation\":" << evidence.summary_generation
      << ",\"candidate_ranges\":" << evidence.candidate_ranges
      << ",\"ranges_pruned\":" << evidence.ranges_pruned
      << ",\"ranges_scanned\":" << evidence.ranges_scanned
      << ",\"pages_considered\":" << evidence.pages_considered
      << ",\"pages_pruned\":" << evidence.pages_pruned
      << ",\"pages_scanned\":" << evidence.pages_scanned
      << ",\"authority_source\":\"" << JsonEscape(evidence.authority_source)
      << "\""
      << ",\"base_row_mga_recheck_required\":"
      << (evidence.base_row_mga_recheck_required ? "true" : "false")
      << ",\"base_row_security_recheck_required\":"
      << (evidence.base_row_security_recheck_required ? "true" : "false")
      << ",\"summary_metadata_visibility_authority\":"
      << (evidence.summary_metadata_visibility_authority ? "true" : "false")
      << ",\"summary_metadata_finality_authority\":"
      << (evidence.summary_metadata_finality_authority ? "true" : "false")
      << ",\"redaction_state\":\"" << JsonEscape(evidence.redaction_state)
      << "\"}";
}

void RenderPartitionSegmentPruneEvidence(
    std::ostringstream& out,
    const PlanPartitionSegmentPruneEvidence& evidence) {
  out << "{\"present\":" << (evidence.present ? "true" : "false")
      << ",\"selected_access\":\"" << JsonEscape(evidence.selected_access)
      << "\",\"fallback_reason\":\"" << JsonEscape(evidence.fallback_reason)
      << "\",\"partitions_considered\":" << evidence.partitions_considered
      << ",\"partitions_pruned\":" << evidence.partitions_pruned
      << ",\"partitions_scanned\":" << evidence.partitions_scanned
      << ",\"segments_considered\":" << evidence.segments_considered
      << ",\"segments_pruned\":" << evidence.segments_pruned
      << ",\"segments_scanned\":" << evidence.segments_scanned
      << ",\"placements_considered\":" << evidence.placements_considered
      << ",\"placements_pruned\":" << evidence.placements_pruned
      << ",\"placements_scanned\":" << evidence.placements_scanned
      << ",\"candidate_ranges\":" << evidence.candidate_ranges
      << ",\"ranges_pruned\":" << evidence.ranges_pruned
      << ",\"ranges_scanned\":" << evidence.ranges_scanned
      << ",\"pages_considered\":" << evidence.pages_considered
      << ",\"pages_pruned\":" << evidence.pages_pruned
      << ",\"pages_scanned\":" << evidence.pages_scanned
      << ",\"authority_source\":\"" << JsonEscape(evidence.authority_source)
      << "\""
      << ",\"base_row_mga_recheck_required\":"
      << (evidence.base_row_mga_recheck_required ? "true" : "false")
      << ",\"base_row_security_recheck_required\":"
      << (evidence.base_row_security_recheck_required ? "true" : "false")
      << ",\"pruning_metadata_visibility_authority\":"
      << (evidence.pruning_metadata_visibility_authority ? "true" : "false")
      << ",\"pruning_metadata_finality_authority\":"
      << (evidence.pruning_metadata_finality_authority ? "true" : "false")
      << ",\"decisions\":[";
  for (std::size_t i = 0; i < evidence.decisions.size(); ++i) {
    const auto& decision = evidence.decisions[i];
    if (i != 0) out << ",";
    out << "{\"object_type\":\"" << JsonEscape(decision.object_type)
        << "\",\"object_uuid\":\"" << JsonEscape(decision.object_uuid)
        << "\",\"parent_uuid\":\"" << JsonEscape(decision.parent_uuid)
        << "\",\"filespace_uuid\":\"" << JsonEscape(decision.filespace_uuid)
        << "\",\"decision\":\"" << JsonEscape(decision.decision)
        << "\",\"reason\":\"" << JsonEscape(decision.reason)
        << "\",\"pages\":" << decision.pages << "}";
  }
  out << "]}";
}

void RenderMgaPageFinalityEvidence(
    std::ostringstream& out,
    const OptimizerMgaPageFinalityEvidence& evidence) {
  out << "{\"present\":" << (evidence.present ? "true" : "false")
      << ",\"evidence_name\":\"" << JsonEscape(evidence.evidence_name)
      << "\",\"accepted\":" << (evidence.accepted ? "true" : "false")
      << ",\"all_visible\":" << (evidence.all_visible ? "true" : "false")
      << ",\"all_final\":" << (evidence.all_final ? "true" : "false")
      << ",\"normal_mga_recheck_required\":"
      << (evidence.normal_mga_recheck_required ? "true" : "false")
      << ",\"finality_map_transaction_authority\":"
      << (evidence.finality_map_transaction_authority ? "true" : "false")
      << ",\"authority_source\":\"" << JsonEscape(evidence.authority_source)
      << "\",\"refusal_reason\":\"" << JsonEscape(evidence.refusal_reason)
      << "\",\"evidence_examined\":" << evidence.evidence_examined
      << ",\"accepted_count\":" << evidence.accepted_count
      << ",\"refused_count\":" << evidence.refused_count
      << ",\"stale_refusals\":" << evidence.stale_refusals
      << ",\"epoch_refusals\":" << evidence.epoch_refusals
      << ",\"horizon_refusals\":" << evidence.horizon_refusals
      << ",\"provenance_refusals\":" << evidence.provenance_refusals
      << "}";
}

}  // namespace

OptimizerExplainDocument BuildOptimizerExplainDocument(const BoundOptimizerRequest& request,
                                                       const BoundOptimizerResult& result) {
  OptimizerExplainDocument document;
  document.request_uuid = request.context.request_uuid;
  document.operation_id = request.context.operation_id;
  document.plan_id = result.plan_id;
  document.plan_hash = StableRuntimePlanHash(request, result);
  document.optimizer_profile = result.optimizer_profile;
  document.catalog_epoch = request.context.catalog_epoch;
  document.security_epoch = request.context.security_epoch;
  document.policy_epoch = request.context.policy_epoch;
  document.statistics_snapshot_id = request.context.statistics_snapshot_id;
  document.metric_snapshot_id = request.context.metric_snapshot_id;
  document.candidates = result.candidates;
  document.diagnostics = result.diagnostics;
  document.authority_facts = ValidateBoundOptimizerRequest(request).authority_facts;
  AddUnique(&document.invalidation_dependencies,
            "catalog_epoch=" + std::to_string(request.context.catalog_epoch));
  AddUnique(&document.invalidation_dependencies,
            "security_epoch=" + std::to_string(request.context.security_epoch));
  AddUnique(&document.invalidation_dependencies,
            "policy_epoch=" + std::to_string(request.context.policy_epoch));
  AddUnique(&document.invalidation_dependencies,
            "descriptor_set_digest=" + request.context.descriptor_set_digest);
  AddUnique(&document.invalidation_dependencies,
            "statistics_snapshot_id=" + request.context.statistics_snapshot_id);
  AddUnique(&document.invalidation_dependencies,
            "metric_snapshot_id=" + request.context.metric_snapshot_id);
  AddUnique(&document.invalidation_dependencies,
            "stats_epoch=" + std::to_string(request.context.stats_epoch));
  AddUnique(&document.invalidation_dependencies,
            "redaction_epoch=" + std::to_string(request.context.redaction_epoch));
  AddUnique(&document.invalidation_dependencies,
            "resource_epoch=" + std::to_string(request.context.resource_epoch));
  AddUnique(&document.invalidation_dependencies,
            "memory_policy_epoch=" + std::to_string(request.context.memory_policy_epoch));
  AddUnique(&document.invalidation_dependencies,
            "memory_feedback_generation=" +
                std::to_string(request.context.memory_feedback_generation));
  AddUnique(&document.invalidation_dependencies,
            "route_epoch=" + std::to_string(request.context.route_epoch));
  AddUnique(&document.statistics_provenance,
            "statistics_snapshot_id=" + request.context.statistics_snapshot_id);
  AddUnique(&document.statistics_provenance,
            "stats_epoch=" + std::to_string(request.context.stats_epoch));
  AddUnique(&document.statistics_provenance,
            "metric_snapshot_id=" + request.context.metric_snapshot_id);
  for (const auto& control : request.logical_plan.optimizer_policy.normalized_controls.safe_control_ids) {
    AddUnique(&document.optimizer_controls, "normalized_safe_control_id=" + control);
  }
  for (const auto& control : request.logical_plan.optimizer_policy.safe_control_ids) {
    AddUnique(&document.optimizer_controls, "safe_control_id=" + control);
  }
  for (const auto& candidate : result.candidates) {
    if (candidate.selected) {
      document.selected_candidate_id = candidate.candidate_id;
    }
    AddUnique(&document.executor_capability_evidence,
              "candidate_executor_capability=" +
                  std::string(scratchbird::engine::planner::PhysicalAccessKindName(
                      candidate.access_kind)));
    for (const auto& reason : candidate.refusal_reasons) {
      AddUnique(&document.candidate_refusals,
                "candidate_refusal=" + candidate.candidate_id + ":" + reason);
    }
    if (!candidate.cost.rejection_reason.empty()) {
      AddUnique(&document.candidate_refusals,
                "candidate_rejection=" + candidate.candidate_id + ":" +
                    candidate.cost.rejection_reason);
    }
    for (const auto& evidence : candidate.runtime_evidence) {
      AddIfPrefixed(&document.join_search_telemetry, evidence, "join_");
      AddIfPrefixed(&document.join_search_telemetry, evidence, "SB_OPT_JOIN_");
      AddIfPrefixed(&document.adaptive_feedback_evidence, evidence, "adaptive_feedback.");
      AddIfPrefixed(&document.adaptive_feedback_evidence, evidence, "optimizer_feedback.");
      AddIfPrefixed(&document.adaptive_feedback_evidence, evidence, "runtime_feedback.");
      AddIfPrefixed(&document.adaptive_feedback_evidence, evidence, "enterprise_relational_operator.feedback_");
      AddIfPrefixed(&document.runtime_actuals, evidence, "actual_");
      AddIfPrefixed(&document.runtime_actuals, evidence, "runtime_actual.");
      AddIfPrefixed(&document.runtime_actuals, evidence, "enterprise_relational_operator.kind=");
      AddIfPrefixed(&document.memory_metric_evidence, evidence, "memory_");
      AddIfPrefixed(&document.memory_metric_evidence, evidence, "optimizer_memory");
      AddIfPrefixed(&document.memory_metric_evidence, evidence, "enterprise_memory");
      AddIfPrefixed(&document.memory_metric_evidence, evidence, "enterprise_relational_operator.memory_");
      AddIfPrefixed(&document.route_evidence, evidence, "route_label=");
      AddIfPrefixed(&document.route_evidence, evidence, "result_contract_hash=");
      AddIfPrefixed(&document.route_evidence, evidence, "plan_node_id=");
      AddIfPrefixed(&document.route_evidence, evidence, "enterprise_relational_operator.route_label=");
      AddIfPrefixed(&document.route_evidence, evidence, "enterprise_relational_operator.result_contract_hash=");
      AddIfPrefixed(&document.route_evidence, evidence, "enterprise_relational_operator.plan_node_id=");
      if (evidence.find("redact") != std::string::npos ||
          evidence.find("protected_material") != std::string::npos) {
        AddUnique(&document.redactions, evidence);
      }
    }
  }
  AddUnique(&document.route_evidence, "request_uuid=" + request.context.request_uuid);
  AddUnique(&document.route_evidence, "operation_id=" + request.context.operation_id);
  AddUnique(&document.route_evidence,
            "executor_capability_set_id=" + request.context.executor_capability_set_id);
  document.redactions.push_back("optimizer_explain.protected_material_redacted=true");
  return document;
}

PlanSummaryPruneEvidence BuildPlanSummaryPruneEvidence(
    const scratchbird::core::index::PageExtentSummaryPrunePlan& plan) {
  PlanSummaryPruneEvidence evidence;
  evidence.present = true;
  evidence.selected_access = plan.selected_access;
  evidence.prune_reason = plan.prune_reason;
  evidence.fallback_reason = plan.fallback_reason;
  evidence.summary_status = plan.summary_status;
  evidence.summary_generation = plan.summary_generation;
  evidence.candidate_ranges = plan.counters.candidate_ranges;
  evidence.ranges_pruned = plan.counters.ranges_pruned;
  evidence.ranges_scanned = plan.counters.ranges_scanned;
  evidence.pages_considered = plan.counters.pages_considered;
  evidence.pages_pruned = plan.counters.pages_pruned;
  evidence.pages_scanned = plan.counters.pages_scanned;
  evidence.authority_source = plan.authority_source;
  evidence.base_row_mga_recheck_required =
      plan.base_row_mga_recheck_required;
  evidence.base_row_security_recheck_required =
      plan.base_row_security_recheck_required;
  evidence.summary_metadata_visibility_authority =
      plan.summary_metadata_visibility_authority;
  evidence.summary_metadata_finality_authority =
      plan.summary_metadata_finality_authority;
  evidence.redaction_state = "catalog_identity_redacted";
  return evidence;
}

PlanSummaryPruneEvidence BuildPlanSummaryPruneEvidence(
    const scratchbird::core::index::TimeRangeSummaryPrunePlan& plan) {
  PlanSummaryPruneEvidence evidence;
  evidence.present = true;
  evidence.selected_access = plan.selected_access;
  evidence.prune_reason = plan.prune_reason;
  evidence.fallback_reason = plan.fallback_reason;
  evidence.summary_status = plan.summary_status;
  evidence.summary_generation = plan.summary_generation;
  evidence.candidate_ranges = plan.counters.prune_candidates;
  evidence.ranges_pruned = plan.counters.ranges_pruned;
  evidence.ranges_scanned = plan.counters.ranges_scanned;
  evidence.pages_considered = plan.counters.pages_considered;
  evidence.pages_pruned = plan.counters.pages_pruned;
  evidence.pages_scanned = plan.counters.pages_scanned;
  evidence.authority_source = plan.authority_source;
  evidence.base_row_mga_recheck_required =
      plan.base_row_mga_recheck_required;
  evidence.base_row_security_recheck_required =
      plan.base_row_security_recheck_required;
  evidence.summary_metadata_visibility_authority =
      plan.summary_metadata_visibility_authority;
  evidence.summary_metadata_finality_authority =
      plan.summary_metadata_finality_authority;
  evidence.redaction_state = "catalog_identity_redacted";
  return evidence;
}

std::string RenderOptimizerExplainJson(const OptimizerExplainDocument& document) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema_version\": \"" << JsonEscape(document.schema_version) << "\",\n";
  out << "  \"request_uuid\": \"" << JsonEscape(document.request_uuid) << "\",\n";
  out << "  \"operation_id\": \"" << JsonEscape(document.operation_id) << "\",\n";
  out << "  \"plan_id\": \"" << JsonEscape(document.plan_id) << "\",\n";
  out << "  \"plan_hash\": \"" << JsonEscape(document.plan_hash) << "\",\n";
  out << "  \"optimizer_profile\": \"" << JsonEscape(document.optimizer_profile) << "\",\n";
  out << "  \"catalog_epoch\": " << document.catalog_epoch << ",\n";
  out << "  \"security_epoch\": " << document.security_epoch << ",\n";
  out << "  \"policy_epoch\": " << document.policy_epoch << ",\n";
  out << "  \"statistics_snapshot_id\": \"" << JsonEscape(document.statistics_snapshot_id) << "\",\n";
  out << "  \"metric_snapshot_id\": \"" << JsonEscape(document.metric_snapshot_id) << "\",\n";
  out << "  \"selected_candidate_id\": \"" << JsonEscape(document.selected_candidate_id) << "\",\n";
  out << "  \"invalidation_dependencies\": ";
  RenderStringArray(out, document.invalidation_dependencies);
  out << ",\n  \"statistics_provenance\": ";
  RenderStringArray(out, document.statistics_provenance);
  out << ",\n  \"candidate_refusals\": ";
  RenderStringArray(out, document.candidate_refusals);
  out << ",\n  \"optimizer_controls\": ";
  RenderStringArray(out, document.optimizer_controls);
  out << ",\n  \"join_search_telemetry\": ";
  RenderStringArray(out, document.join_search_telemetry);
  out << ",\n  \"adaptive_feedback_evidence\": ";
  RenderStringArray(out, document.adaptive_feedback_evidence);
  out << ",\n  \"runtime_actuals\": ";
  RenderStringArray(out, document.runtime_actuals);
  out << ",\n  \"memory_metric_evidence\": ";
  RenderStringArray(out, document.memory_metric_evidence);
  out << ",\n  \"route_evidence\": ";
  RenderStringArray(out, document.route_evidence);
  out << ",\n  \"executor_capability_evidence\": ";
  RenderStringArray(out, document.executor_capability_evidence);
  out << ",\n";
  out << "  \"diagnostics\": ";
  RenderStringArray(out, document.diagnostics);
  out << ",\n  \"redactions\": ";
  RenderStringArray(out, document.redactions);
  out << ",\n  \"authority_facts\": [";
  for (std::size_t i = 0; i < document.authority_facts.size(); ++i) {
    const auto& fact = document.authority_facts[i];
    if (i != 0) out << ",";
    out << "{\"fact_name\":\"" << JsonEscape(fact.fact_name) << "\",\"status\":\""
        << OptimizerAuthorityStatusName(fact.status) << "\",\"required\":" << (fact.required ? "true" : "false")
        << ",\"detail\":\"" << JsonEscape(fact.detail) << "\"}";
  }
  out << "],\n  \"candidates\": [";
  for (std::size_t i = 0; i < document.candidates.size(); ++i) {
    const auto& candidate = document.candidates[i];
    if (i != 0) out << ",";
    out << "{\"candidate_id\":\"" << JsonEscape(candidate.candidate_id) << "\",\"access_kind\":\""
        << scratchbird::engine::planner::PhysicalAccessKindName(candidate.access_kind)
        << "\",\"selectable\":" << (candidate.cost.selectable ? "true" : "false")
        << ",\"selected\":" << (candidate.selected ? "true" : "false")
        << ",\"rejection_reason\":\"" << JsonEscape(candidate.cost.rejection_reason)
        << "\",\"total_cost\":" << candidate.cost.total_cost;
    if (candidate.summary_prune_evidence.present) {
      out << ",\"summary_prune\":";
      RenderSummaryPruneEvidence(out, candidate.summary_prune_evidence);
    }
    if (candidate.partition_segment_prune_evidence.present) {
      out << ",\"partition_segment_prune\":";
      RenderPartitionSegmentPruneEvidence(out, candidate.partition_segment_prune_evidence);
    }
    if (candidate.mga_page_finality_evidence.present) {
      out << ",\"mga_page_finality\":";
      RenderMgaPageFinalityEvidence(out, candidate.mga_page_finality_evidence);
    }
    out << "}";
  }
  out << "]\n}\n";
  return out.str();
}

}  // namespace scratchbird::engine::optimizer
