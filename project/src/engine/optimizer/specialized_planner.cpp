// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "specialized_planner.hpp"

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace internal_api = scratchbird::engine::internal_api;
namespace planner = scratchbird::engine::planner;
namespace {

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool HasDescriptor(const planner::LogicalPlanNode& node, std::string_view descriptor) {
  return std::find(node.required_descriptors.begin(),
                   node.required_descriptors.end(),
                   descriptor) != node.required_descriptors.end();
}

bool HasDescriptorContaining(const planner::LogicalPlanNode& node,
                             std::string_view token) {
  return std::any_of(node.required_descriptors.begin(),
                     node.required_descriptors.end(),
                     [&](const std::string& descriptor) {
                       return descriptor.find(token) != std::string::npos;
                     });
}

std::optional<std::string> DescriptorValue(const planner::LogicalPlanNode& node,
                                           std::string_view prefix) {
  for (const auto& descriptor : node.required_descriptors) {
    if (StartsWith(descriptor, prefix)) {
      return descriptor.substr(prefix.size());
    }
  }
  return std::nullopt;
}

std::uint64_t DescriptorU64(const planner::LogicalPlanNode& node,
                            std::string_view prefix,
                            std::uint64_t fallback) {
  const auto value = DescriptorValue(node, prefix);
  if (!value) {
    return fallback;
  }
  try {
    return static_cast<std::uint64_t>(std::stoull(*value));
  } catch (...) {
    return fallback;
  }
}

internal_api::EngineNoSqlProviderFamily FamilyForAccessKind(
    planner::PhysicalAccessKind access_kind) {
  switch (access_kind) {
    case planner::PhysicalAccessKind::kFullTextProbe:
      return internal_api::EngineNoSqlProviderFamily::kSearch;
    case planner::PhysicalAccessKind::kVectorExactSearch:
    case planner::PhysicalAccessKind::kVectorApproximateWithFallback:
      return internal_api::EngineNoSqlProviderFamily::kVector;
    case planner::PhysicalAccessKind::kDocumentPathProbe:
      return internal_api::EngineNoSqlProviderFamily::kDocument;
    case planner::PhysicalAccessKind::kGraphTraversalSeed:
      return internal_api::EngineNoSqlProviderFamily::kGraph;
    case planner::PhysicalAccessKind::kTimeSeriesAppendPath:
      return internal_api::EngineNoSqlProviderFamily::kTimeSeries;
    default:
      return internal_api::EngineNoSqlProviderFamily::kUnknown;
  }
}

planner::PhysicalAccessKind AccessKindForFamily(
    internal_api::EngineNoSqlProviderFamily family,
    bool exact_fallback_available) {
  switch (family) {
    case internal_api::EngineNoSqlProviderFamily::kKeyValue:
    case internal_api::EngineNoSqlProviderFamily::kDocument:
      return planner::PhysicalAccessKind::kDocumentPathProbe;
    case internal_api::EngineNoSqlProviderFamily::kSearch:
      return planner::PhysicalAccessKind::kFullTextProbe;
    case internal_api::EngineNoSqlProviderFamily::kVector:
      return exact_fallback_available
                 ? planner::PhysicalAccessKind::kVectorApproximateWithFallback
                 : planner::PhysicalAccessKind::kVectorExactSearch;
    case internal_api::EngineNoSqlProviderFamily::kGraph:
      return planner::PhysicalAccessKind::kGraphTraversalSeed;
    case internal_api::EngineNoSqlProviderFamily::kTimeSeries:
      return planner::PhysicalAccessKind::kTimeSeriesAppendPath;
    case internal_api::EngineNoSqlProviderFamily::kSpatial:
      return planner::PhysicalAccessKind::kScalarBtreeRange;
    case internal_api::EngineNoSqlProviderFamily::kColumnar:
      return planner::PhysicalAccessKind::kTableScan;
    case internal_api::EngineNoSqlProviderFamily::kUnknown:
      return planner::PhysicalAccessKind::kNone;
  }
  return planner::PhysicalAccessKind::kNone;
}

std::string CandidateIdForFamily(internal_api::EngineNoSqlProviderFamily family) {
  switch (family) {
    case internal_api::EngineNoSqlProviderFamily::kKeyValue:
    case internal_api::EngineNoSqlProviderFamily::kDocument:
      return "CAND-OPT-SPECIALIZED-DOCUMENT-KV";
    case internal_api::EngineNoSqlProviderFamily::kSearch:
      return "CAND-OPT-SPECIALIZED-SEARCH";
    case internal_api::EngineNoSqlProviderFamily::kVector:
      return "CAND-OPT-SPECIALIZED-VECTOR";
    case internal_api::EngineNoSqlProviderFamily::kGraph:
      return "CAND-OPT-SPECIALIZED-GRAPH";
    case internal_api::EngineNoSqlProviderFamily::kTimeSeries:
      return "CAND-OPT-SPECIALIZED-TIMESERIES";
    case internal_api::EngineNoSqlProviderFamily::kSpatial:
      return "CAND-OPT-SPECIALIZED-SPATIAL";
    case internal_api::EngineNoSqlProviderFamily::kColumnar:
      return "CAND-OPT-SPECIALIZED-COLUMNAR";
    case internal_api::EngineNoSqlProviderFamily::kUnknown:
      return "CAND-OPT-SPECIALIZED-UNKNOWN";
  }
  return "CAND-OPT-SPECIALIZED-UNKNOWN";
}

std::vector<std::string> AllDiagnostics(
    const internal_api::EngineNoSqlPhysicalProviderSelection& selection) {
  auto diagnostics = selection.missing_diagnostics;
  diagnostics.insert(diagnostics.end(),
                     selection.refusal_diagnostics.begin(),
                     selection.refusal_diagnostics.end());
  return diagnostics;
}

PlanCandidate MakeSpecialized(
    std::string id,
    planner::PhysicalAccessKind kind,
    const internal_api::EngineNoSqlPhysicalProviderContract& contract) {
  const auto selection = internal_api::SelectLocalNoSqlPhysicalProvider(contract);
  PlanCandidate candidate;
  candidate.candidate_id = std::move(id);
  candidate.access_kind = kind;
  candidate.scope = internal_api::EngineNoSqlProviderScopeName(selection.scope);
  candidate.required_facts = selection.required_facts;
  candidate.missing_facts = selection.missing_diagnostics;
  candidate.refusal_reasons = selection.refusal_diagnostics;
  candidate.runtime_evidence = selection.evidence;
  candidate.estimated_rows = selection.estimated_rows;
  if (!selection.selected) {
    const auto diagnostics = AllDiagnostics(selection);
    candidate.cost = RejectedCost(diagnostics.empty()
                                      ? "SB_NOSQL_PROVIDER_REFUSED"
                                      : diagnostics.front());
    return candidate;
  }
  candidate.acceptance_reasons = selection.required_facts;
  candidate.cost = EstimateNodeCost(planner::MakeLogicalPlanNode(
      planner::LogicalPlanNodeKind::kNoSqlOperation,
      kind,
      "nosql." + std::string(internal_api::EngineNoSqlProviderFamilyName(selection.family)),
      internal_api::EngineNoSqlProviderFamilyName(selection.family)));
  candidate.cost.row_cost += selection.estimated_rows / 10;
  if (selection.family == internal_api::EngineNoSqlProviderFamily::kVector &&
      !contract.exact_fallback_available) {
    candidate.cost.uncertainty_cost += 10000;
  }
  candidate.cost.total_cost = candidate.cost.startup_cost + candidate.cost.row_cost + candidate.cost.io_cost + candidate.cost.memory_cost + candidate.cost.uncertainty_cost;
  candidate.cost.confidence = contract.exact_fallback_available ? CostConfidence::kMedium : CostConfidence::kLow;
  candidate.runtime_evidence.push_back("specialized_family_coverage=" +
                                       std::string(internal_api::EngineNoSqlProviderFamilyName(selection.family)));
  candidate.runtime_evidence.push_back("specialized_route_gate_present=true");
  candidate.runtime_evidence.push_back("specialized_exact_recheck_required=true");
  candidate.runtime_evidence.push_back("specialized_mga_recheck_required=true");
  candidate.runtime_evidence.push_back("specialized_security_recheck_required=true");
  candidate.runtime_evidence.push_back("descriptor_scan_fallback=false");
  candidate.runtime_evidence.push_back("behavior_store_scan_fallback=false");
  return candidate;
}

SpecializedProviderCapability CapabilityFromLogicalNode(
    const planner::LogicalPlanNode& node,
    const OptimizerStatisticsCatalog& statistics) {
  SpecializedProviderCapability capability;
  capability.family = internal_api::EngineNoSqlProviderFamilyName(FamilyForAccessKind(node.access_kind));
  if (const auto value = DescriptorValue(node, "nosql.provider.family=")) {
    capability.family = *value;
  } else if (HasDescriptor(node, "specialized.search")) {
    capability.family = "search";
  } else if (HasDescriptor(node, "specialized.vector")) {
    capability.family = "vector";
  } else if (HasDescriptor(node, "specialized.document")) {
    capability.family = "document";
  } else if (HasDescriptor(node, "specialized.graph")) {
    capability.family = "graph";
  }

  capability.provider_id =
      DescriptorValue(node, "nosql.provider.id=").value_or("nosql.local.provider");
  capability.fallback_provider_id =
      DescriptorValue(node, "nosql.provider.fallback_id=").value_or("");
  capability.local_provider_available =
      HasDescriptor(node, "nosql.provider.local") ||
      HasDescriptor(node, "nosql.local_provider.available");
  capability.index_available =
      HasDescriptor(node, "nosql.index.available") ||
      HasDescriptor(node, "nosql.index_generation.proof");
  capability.exact_fallback_available =
      HasDescriptor(node, "nosql.exact_fallback.available");
  capability.descriptor_compatible =
      HasDescriptor(node, "nosql.descriptor.compatible") ||
      HasDescriptor(node, "descriptor_compatibility");
  capability.policy_allowed = HasDescriptor(node, "nosql.policy.allowed");
  capability.descriptor_visibility_proof_present =
      HasDescriptor(node, "nosql.descriptor_visibility.proof");
  capability.descriptor_visible_to_snapshot =
      HasDescriptor(node, "nosql.descriptor_visibility.visible") ||
      capability.descriptor_visibility_proof_present;
  capability.security_redaction_proof_present =
      HasDescriptor(node, "nosql.security.proof") ||
      HasDescriptor(node, "nosql.security.redaction_proof");
  capability.security_snapshot_proof_present =
      HasDescriptor(node, "nosql.security.proof") ||
      HasDescriptor(node, "nosql.security.snapshot_proof");
  capability.index_generation_proof_present =
      HasDescriptor(node, "nosql.index_generation.proof");
  capability.index_generation_visible_to_snapshot =
      HasDescriptor(node, "nosql.index_generation.visible") ||
      capability.index_generation_proof_present;
  capability.index_covers_predicate =
      HasDescriptor(node, "nosql.index.covers_predicate") ||
      HasDescriptor(node, "nosql.index_generation.covers_predicate");
  capability.required_index_generation =
      DescriptorU64(node, "nosql.index_generation.required=", 1);
  capability.available_index_generation =
      DescriptorU64(node,
                    "nosql.index_generation.available=",
                    capability.index_generation_proof_present ? capability.required_index_generation : 0);
  capability.delta_overlay_required =
      HasDescriptor(node, "nosql.delta_overlay.required");
  capability.delta_overlay_proof_present =
      HasDescriptor(node, "nosql.delta_overlay.proof");
  capability.delta_overlay_covers_snapshot =
      HasDescriptor(node, "nosql.delta_overlay.covers_snapshot") ||
      capability.delta_overlay_proof_present;
  capability.policy_proof_present = HasDescriptor(node, "nosql.policy.proof");
  capability.mga_recheck_proof_present =
      HasDescriptor(node, "nosql.mga_recheck.proof");
  capability.row_mga_recheck_required =
      !HasDescriptor(node, "nosql.row_mga_recheck.not_required");
  capability.row_security_recheck_required =
      !HasDescriptor(node, "nosql.security_recheck.not_required");
  capability.requires_cluster_provider =
      HasDescriptor(node, "nosql.provider.cluster_required") ||
      HasDescriptor(node, "nosql.cluster.required") ||
      HasDescriptorContaining(node, "cluster_only");
  capability.requires_distributed_provider =
      HasDescriptor(node, "nosql.provider.distributed_required") ||
      HasDescriptor(node, "nosql.distributed.required");
  capability.descriptor_scan_path =
      HasDescriptor(node, "nosql.descriptor_scan.path") ||
      HasDescriptor(node, "nosql.descriptor_scan.selected");
  capability.behavior_store_scan_path =
      HasDescriptor(node, "nosql.behavior_store_scan.path") ||
      HasDescriptor(node, "nosql.behavior_store_scan.selected");
  capability.provider_claims_transaction_finality_authority =
      HasDescriptor(node, "nosql.provider.transaction_finality_authority");
  capability.provider_claims_visibility_authority =
      HasDescriptor(node, "nosql.provider.visibility_authority");
  capability.index_claims_transaction_finality_authority =
      HasDescriptor(node, "nosql.index.transaction_finality_authority");
  capability.delta_overlay_claims_transaction_finality_authority =
      HasDescriptor(node, "nosql.delta_overlay.transaction_finality_authority");
  capability.parser_claims_transaction_finality_authority =
      HasDescriptor(node, "nosql.parser.transaction_finality_authority");
  capability.write_ahead_log_claims_transaction_finality_authority =
      HasDescriptor(node, "nosql.write_ahead_log.transaction_finality_authority");

  const auto object_uuid = node.required_object_uuids.empty()
                               ? "local.default"
                               : node.required_object_uuids.front();
  capability.estimated_rows =
      statistics.EstimateUnsigned("visible_row_count",
                                  object_uuid,
                                  statistics.EstimateUnsigned("row_count",
                                                              object_uuid,
                                                              1000));
  if (capability.estimated_rows == 0) {
    capability.estimated_rows = 1000;
  }
  return capability;
}

}  // namespace

SpecializedProviderCapability MakeSpecializedProviderCapabilityFromContract(
    const internal_api::EngineNoSqlPhysicalProviderContract& contract) {
  SpecializedProviderCapability capability;
  capability.family = internal_api::EngineNoSqlProviderFamilyName(contract.family);
  capability.local_provider_available = contract.local_provider_available;
  capability.index_available = contract.index_generation.proof_present;
  capability.exact_fallback_available = contract.exact_fallback_available;
  capability.descriptor_compatible =
      contract.descriptor_visibility.descriptor_shape_compatible;
  capability.policy_allowed = contract.policy.allowed;
  capability.descriptor_visibility_proof_present =
      contract.descriptor_visibility.proof_present;
  capability.descriptor_visible_to_snapshot =
      contract.descriptor_visibility.visible_to_snapshot;
  capability.security_redaction_proof_present =
      contract.security_redaction.proof_present &&
      contract.security_redaction.redaction_policy_bound;
  capability.security_snapshot_proof_present =
      contract.security_redaction.security_snapshot_bound;
  capability.index_generation_proof_present =
      contract.index_generation.proof_present;
  capability.index_generation_visible_to_snapshot =
      contract.index_generation.visible_to_snapshot;
  capability.index_covers_predicate =
      contract.index_generation.covers_predicate;
  capability.required_index_generation =
      contract.index_generation.required_generation;
  capability.available_index_generation =
      contract.index_generation.available_generation;
  capability.delta_overlay_required = contract.delta_overlay.required;
  capability.delta_overlay_proof_present = contract.delta_overlay.proof_present;
  capability.delta_overlay_covers_snapshot = contract.delta_overlay.covers_snapshot;
  capability.policy_proof_present = contract.policy.proof_present;
  capability.mga_recheck_proof_present = contract.mga_recheck.proof_present;
  capability.row_mga_recheck_required =
      contract.mga_recheck.row_mga_recheck_required;
  capability.row_security_recheck_required =
      contract.mga_recheck.row_security_recheck_required;
  capability.requires_cluster_provider =
      contract.scope == internal_api::EngineNoSqlProviderScope::kClusterOnly;
  capability.requires_distributed_provider =
      contract.scope == internal_api::EngineNoSqlProviderScope::kDistributed;
  capability.descriptor_scan_path =
      contract.descriptor_visibility.descriptor_scan_selected;
  capability.behavior_store_scan_path =
      contract.descriptor_visibility.behavior_store_scan_selected;
  capability.provider_claims_transaction_finality_authority =
      contract.mga_recheck.provider_claims_transaction_finality_authority;
  capability.provider_claims_visibility_authority =
      contract.mga_recheck.provider_claims_visibility_authority;
  capability.index_claims_transaction_finality_authority =
      contract.mga_recheck.index_claims_transaction_finality_authority;
  capability.delta_overlay_claims_transaction_finality_authority =
      contract.mga_recheck.delta_overlay_claims_transaction_finality_authority;
  capability.parser_claims_transaction_finality_authority =
      contract.mga_recheck.parser_claims_transaction_finality_authority;
  capability.write_ahead_log_claims_transaction_finality_authority =
      contract.mga_recheck.write_ahead_log_claims_transaction_finality_authority;
  capability.provider_id = contract.provider_id;
  capability.fallback_provider_id = contract.fallback_provider_id;
  capability.estimated_rows = contract.estimated_rows;
  return capability;
}

internal_api::EngineNoSqlPhysicalProviderContract MakeNoSqlPhysicalProviderContract(
    const SpecializedProviderCapability& capability) {
  internal_api::EngineNoSqlPhysicalProviderContract contract;
  contract.family = internal_api::EngineNoSqlProviderFamilyFromString(capability.family);
  contract.scope = capability.requires_distributed_provider
                       ? internal_api::EngineNoSqlProviderScope::kDistributed
                       : (capability.requires_cluster_provider
                              ? internal_api::EngineNoSqlProviderScope::kClusterOnly
                              : internal_api::EngineNoSqlProviderScope::kLocal);
  contract.provider_id = capability.provider_id;
  contract.fallback_provider_id = capability.fallback_provider_id;
  contract.local_provider_available = capability.local_provider_available;
  contract.exact_fallback_available = capability.exact_fallback_available;
  contract.estimated_rows = capability.estimated_rows;
  contract.descriptor_visibility.proof_present =
      capability.descriptor_visibility_proof_present;
  contract.descriptor_visibility.visible_to_snapshot =
      capability.descriptor_visible_to_snapshot;
  contract.descriptor_visibility.descriptor_shape_compatible =
      capability.descriptor_compatible;
  contract.descriptor_visibility.descriptor_scan_selected =
      capability.descriptor_scan_path;
  contract.descriptor_visibility.behavior_store_scan_selected =
      capability.behavior_store_scan_path;
  contract.security_redaction.proof_present =
      capability.security_redaction_proof_present;
  contract.security_redaction.redaction_policy_bound =
      capability.security_redaction_proof_present;
  contract.security_redaction.security_snapshot_bound =
      capability.security_snapshot_proof_present;
  contract.security_redaction.redaction_profile =
      capability.security_redaction_proof_present ? "bound" : "unverified";
  contract.index_generation.proof_present =
      capability.index_generation_proof_present && capability.index_available;
  contract.index_generation.visible_to_snapshot =
      capability.index_generation_visible_to_snapshot;
  contract.index_generation.covers_predicate = capability.index_covers_predicate;
  contract.index_generation.required_generation =
      capability.required_index_generation;
  contract.index_generation.available_generation =
      capability.available_index_generation;
  contract.delta_overlay.required = capability.delta_overlay_required;
  contract.delta_overlay.proof_present = capability.delta_overlay_proof_present;
  contract.delta_overlay.covers_snapshot =
      capability.delta_overlay_covers_snapshot;
  contract.policy.proof_present = capability.policy_proof_present;
  contract.policy.allowed = capability.policy_allowed;
  contract.mga_recheck.proof_present = capability.mga_recheck_proof_present;
  contract.mga_recheck.row_mga_recheck_required =
      capability.row_mga_recheck_required;
  contract.mga_recheck.row_security_recheck_required =
      capability.row_security_recheck_required;
  contract.mga_recheck.provider_claims_transaction_finality_authority =
      capability.provider_claims_transaction_finality_authority;
  contract.mga_recheck.provider_claims_visibility_authority =
      capability.provider_claims_visibility_authority;
  contract.mga_recheck.index_claims_transaction_finality_authority =
      capability.index_claims_transaction_finality_authority;
  contract.mga_recheck.delta_overlay_claims_transaction_finality_authority =
      capability.delta_overlay_claims_transaction_finality_authority;
  contract.mga_recheck.parser_claims_transaction_finality_authority =
      capability.parser_claims_transaction_finality_authority;
  contract.mga_recheck.write_ahead_log_claims_transaction_finality_authority =
      capability.write_ahead_log_claims_transaction_finality_authority;
  return contract;
}

PlanCandidate PlanNoSqlPhysicalProviderCandidate(
    const SpecializedProviderCapability& capability) {
  const auto contract = MakeNoSqlPhysicalProviderContract(capability);
  return MakeSpecialized(CandidateIdForFamily(contract.family),
                         AccessKindForFamily(contract.family,
                                             capability.exact_fallback_available),
                         contract);
}

PlanCandidate PlanNoSqlPhysicalProviderCandidate(
    const internal_api::EngineNoSqlPhysicalProviderContract& contract) {
  return MakeSpecialized(CandidateIdForFamily(contract.family),
                         AccessKindForFamily(contract.family,
                                             contract.exact_fallback_available),
                         contract);
}

PlanCandidate PlanNoSqlLogicalNodeCandidate(
    const planner::LogicalPlanNode& node,
    const OptimizerStatisticsCatalog& statistics) {
  return PlanNoSqlPhysicalProviderCandidate(
      CapabilityFromLogicalNode(node, statistics));
}

PlanCandidate PlanDocumentKvCandidate(const SpecializedProviderCapability& capability) {
  auto scoped = capability;
  if (scoped.family.empty()) {
    scoped.family = "document";
  }
  return PlanNoSqlPhysicalProviderCandidate(scoped);
}

PlanCandidate PlanSearchCandidate(const SpecializedProviderCapability& capability) {
  auto scoped = capability;
  if (scoped.family.empty()) {
    scoped.family = "search";
  }
  return PlanNoSqlPhysicalProviderCandidate(scoped);
}

PlanCandidate PlanVectorCandidate(const SpecializedProviderCapability& capability) {
  auto scoped = capability;
  if (scoped.family.empty()) {
    scoped.family = "vector";
  }
  return PlanNoSqlPhysicalProviderCandidate(scoped);
}

PlanCandidate PlanSpatialCandidate(const SpecializedProviderCapability& capability) {
  auto scoped = capability;
  if (scoped.family.empty()) {
    scoped.family = "spatial";
  }
  auto candidate = PlanNoSqlPhysicalProviderCandidate(scoped);
  candidate.required_facts.push_back("spatial_operator_class");
  return candidate;
}

PlanCandidate PlanGraphCandidate(const SpecializedProviderCapability& capability) {
  auto scoped = capability;
  if (scoped.family.empty()) {
    scoped.family = "graph";
  }
  return PlanNoSqlPhysicalProviderCandidate(scoped);
}

PlanCandidate PlanTimeSeriesCandidate(const SpecializedProviderCapability& capability) {
  auto scoped = capability;
  if (scoped.family.empty()) {
    scoped.family = "time_series";
  }
  return PlanNoSqlPhysicalProviderCandidate(scoped);
}

PlanCandidate PlanColumnarCandidate(const SpecializedProviderCapability& capability) {
  auto scoped = capability;
  if (scoped.family.empty()) {
    scoped.family = "columnar";
  }
  auto candidate = PlanNoSqlPhysicalProviderCandidate(scoped);
  candidate.required_facts.push_back("columnar_page_family");
  return candidate;
}

std::vector<PlanCandidate> PlanAllSpecializedFamilyCandidates(const std::vector<SpecializedProviderCapability>& capabilities) {
  std::vector<PlanCandidate> candidates;
  for (const auto& capability : capabilities) {
    if (capability.family == "document" || capability.family == "kv") candidates.push_back(PlanDocumentKvCandidate(capability));
    else if (capability.family == "search") candidates.push_back(PlanSearchCandidate(capability));
    else if (capability.family == "vector") candidates.push_back(PlanVectorCandidate(capability));
    else if (capability.family == "spatial") candidates.push_back(PlanSpatialCandidate(capability));
    else if (capability.family == "graph") candidates.push_back(PlanGraphCandidate(capability));
    else if (capability.family == "time_series") candidates.push_back(PlanTimeSeriesCandidate(capability));
    else if (capability.family == "columnar") candidates.push_back(PlanColumnarCandidate(capability));
    else candidates.push_back(PlanNoSqlPhysicalProviderCandidate(capability));
  }
  return candidates;
}

SpecializedWorkloadFamilyCoverageResult ValidateSpecializedWorkloadFamilyCoverage(
    const std::vector<SpecializedWorkloadFamilyCoverage>& families) {
  SpecializedWorkloadFamilyCoverageResult result;
  if (families.empty()) {
    result.diagnostics.push_back("SB_OPT_SPECIALIZED_FAMILY_COVERAGE_REQUIRED");
    return result;
  }
  for (const auto& family : families) {
    if (family.production_claim_removed) {
      result.evidence.push_back("specialized_family_claim_removed=" + family.family);
      continue;
    }
    if (family.family.empty() || family.index_family.empty()) {
      result.diagnostics.push_back("SB_OPT_SPECIALIZED_FAMILY_IDENTITY_REQUIRED");
    }
    if (!family.cost_statistics_present || !family.selectivity_statistics_present ||
        !family.false_positive_statistics_present) {
      result.diagnostics.push_back("SB_OPT_SPECIALIZED_FAMILY_STATISTICS_REQUIRED:" +
                                   family.family);
    }
    if (!family.route_gate_present) {
      result.diagnostics.push_back("SB_OPT_SPECIALIZED_FAMILY_ROUTE_GATE_REQUIRED:" +
                                   family.family);
    }
    if (!family.exact_recheck_required || !family.mga_recheck_required ||
        !family.security_recheck_required) {
      result.diagnostics.push_back("SB_OPT_SPECIALIZED_FAMILY_RECHECK_REQUIRED:" +
                                   family.family);
    }
    if (family.false_positive_ratio < 0.0 || family.false_positive_ratio > 1.0) {
      result.diagnostics.push_back("SB_OPT_SPECIALIZED_FAMILY_FALSE_POSITIVE_INVALID:" +
                                   family.family);
    }
    result.evidence.push_back("specialized_family_covered=" + family.family +
                              "|index_family=" + family.index_family);
  }
  result.ok = result.diagnostics.empty();
  if (result.ok) {
    result.evidence.push_back("OPCH_SPECIALIZED_WORKLOAD_FAMILY_COVERAGE");
  }
  return result;
}

NoSqlSqlFusionRouteResult ValidateNoSqlSqlFusionRoute(
    const NoSqlSqlFusionRouteRequest& request) {
  NoSqlSqlFusionRouteResult result;
  auto refuse = [&](std::string code) {
    result.ok = false;
    result.diagnostic_code = std::move(code);
    result.evidence.push_back("OPCH_NOSQL_SQL_FUSION_OPTIMIZER_ROUTES");
    result.evidence.push_back("nosql_sql_fusion.accepted=false");
    result.evidence.push_back("nosql_sql_fusion.parser_authority=false");
    result.evidence.push_back("nosql_sql_fusion.donor_authority=false");
    result.evidence.push_back("nosql_sql_fusion.visibility_authority=false");
    result.evidence.push_back("nosql_sql_fusion.finality_authority=false");
  };
  if (request.route_label.empty()) {
    refuse("SB_OPT_NOSQL_SQL_FUSION.ROUTE_LABEL_REQUIRED");
    return result;
  }
  if (request.parser_or_donor_authority) {
    refuse("SB_OPT_NOSQL_SQL_FUSION.UNSAFE_AUTHORITY");
    return result;
  }
  if (request.sql_result_hash.empty() ||
      request.sql_result_hash != request.fusion_result_hash) {
    refuse("SB_OPT_NOSQL_SQL_FUSION.RESULT_EQUIVALENCE_REQUIRED");
    return result;
  }
  if (!request.sql_route_consumed ||
      !request.document_route_consumed ||
      !request.vector_route_consumed ||
      !request.search_route_consumed ||
      !request.graph_route_consumed ||
      !request.candidate_set_route_consumed) {
    refuse("SB_OPT_NOSQL_SQL_FUSION.ROUTE_CONSUMPTION_REQUIRED");
    return result;
  }
  if (!request.exact_recheck_required ||
      !request.mga_recheck_required ||
      !request.security_recheck_required) {
    refuse("SB_OPT_NOSQL_SQL_FUSION.RECHECK_REQUIRED");
    return result;
  }
  if (request.descriptor_scan_fallback ||
      request.behavior_store_scan_fallback) {
    refuse("SB_OPT_NOSQL_SQL_FUSION.DESCRIPTOR_SCAN_FALLBACK_FORBIDDEN");
    return result;
  }
  if (request.candidates.empty()) {
    refuse("SB_OPT_NOSQL_SQL_FUSION.CANDIDATE_REQUIRED");
    return result;
  }
  for (const auto& candidate : request.candidates) {
    if (!candidate.cost.selectable || !candidate.missing_facts.empty() ||
        !candidate.refusal_reasons.empty()) {
      refuse("SB_OPT_NOSQL_SQL_FUSION.CANDIDATE_REFUSED");
      return result;
    }
  }
  result.ok = true;
  result.diagnostic_code = "SB_OPT_NOSQL_SQL_FUSION.OK";
  result.evidence.push_back("OPCH_NOSQL_SQL_FUSION_OPTIMIZER_ROUTES");
  result.evidence.push_back("nosql_sql_fusion.accepted=true");
  result.evidence.push_back("nosql_sql_fusion.route_label=" + request.route_label);
  result.evidence.push_back("nosql_sql_fusion.result_equivalence=true");
  result.evidence.push_back("nosql_sql_fusion.sql_route_consumed=true");
  result.evidence.push_back("nosql_sql_fusion.document_route_consumed=true");
  result.evidence.push_back("nosql_sql_fusion.vector_route_consumed=true");
  result.evidence.push_back("nosql_sql_fusion.search_route_consumed=true");
  result.evidence.push_back("nosql_sql_fusion.graph_route_consumed=true");
  result.evidence.push_back("nosql_sql_fusion.candidate_set_route_consumed=true");
  result.evidence.push_back("nosql_sql_fusion.descriptor_scan_fallback=false");
  result.evidence.push_back("nosql_sql_fusion.behavior_store_scan_fallback=false");
  result.evidence.push_back("nosql_sql_fusion.exact_recheck_required=true");
  result.evidence.push_back("nosql_sql_fusion.mga_recheck_required=true");
  result.evidence.push_back("nosql_sql_fusion.security_recheck_required=true");
  result.evidence.push_back("nosql_sql_fusion.parser_authority=false");
  result.evidence.push_back("nosql_sql_fusion.donor_authority=false");
  result.evidence.push_back("nosql_sql_fusion.visibility_authority=false");
  result.evidence.push_back("nosql_sql_fusion.finality_authority=false");
  return result;
}

}  // namespace scratchbird::engine::optimizer
