// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_profile_factory.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <ranges>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace scratchbird::engine::optimizer {
namespace planner = scratchbird::engine::planner;
namespace executor = scratchbird::engine::executor;
namespace {

std::uint64_t Fnv1a64(const std::string_view value,
                      std::uint64_t hash = 14695981039346656037ull) {
  for (const auto byte : value) {
    hash ^= static_cast<std::uint8_t>(byte);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string DerivedCanonicalUuid(const std::string_view scope,
                                 const std::string_view purpose) {
  const auto first = Fnv1a64(purpose, Fnv1a64(scope));
  const auto second = Fnv1a64(scope, Fnv1a64(purpose));
  std::array<std::uint8_t, 16> bytes{};
  for (std::size_t index = 0; index < 8; ++index) {
    bytes[index] = static_cast<std::uint8_t>(first >> ((7 - index) * 8));
    bytes[8 + index] =
        static_cast<std::uint8_t>(second >> ((7 - index) * 8));
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x50);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) out << '-';
    out << std::setw(2) << static_cast<unsigned>(bytes[index]);
  }
  return out.str();
}

bool SameCapability(const CanonicalExecutorCapabilityRecord& left,
                    const CanonicalExecutorCapabilityRecord& right) {
  return left.capability_uuid == right.capability_uuid &&
         left.capability_abi_version == right.capability_abi_version &&
         left.implementation_id == right.implementation_id &&
         left.logical_node_kind == right.logical_node_kind &&
         left.physical_node_kind == right.physical_node_kind &&
         left.minimum_input_count == right.minimum_input_count &&
         left.maximum_input_count == right.maximum_input_count &&
         left.supported_property_kinds == right.supported_property_kinds &&
         left.maximum_memory_bytes == right.maximum_memory_bytes &&
         left.spill_supported == right.spill_supported &&
         left.storage_read_capable == right.storage_read_capable &&
         left.mga_visibility_capable == right.mga_visibility_capable &&
         left.available == right.available &&
         left.refusal_diagnostic_id == right.refusal_diagnostic_id &&
         left.engine_owned == right.engine_owned;
}

}  // namespace

CanonicalOptimizerProfileFactoryResult
BuildCanonicalOptimizerAlternativeProfiles(
    const CanonicalOptimizerAdmissionRequest& admission_request,
    const CanonicalOptimizerAdmissionResult& admission,
    const std::vector<CanonicalOptimizerImplementationProfile>&
        implementations,
    std::string identity_scope,
    std::string calibration_profile_uuid) {
  CanonicalOptimizerProfileFactoryResult result;
  const auto refuse = [&](std::string diagnostic_id,
                          const std::uint32_t logical_node_id,
                          std::string implementation_id,
                          std::string field_id) {
    result = {};
    result.issues.push_back({std::move(diagnostic_id), logical_node_id,
                             std::move(implementation_id),
                             std::move(field_id)});
    return result;
  };
  if (!admission.admitted || !admission.planning_allowed ||
      admission.data_access_allowed || !admission.issues.empty() ||
      identity_scope.empty() || calibration_profile_uuid.empty() ||
      implementations.empty() ||
      implementations.size() >
          admission_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-PROFILE-FACTORY-ADMISSION-V1", 0,
                  {}, "admission_or_bounds");
  }
  const auto& graph = admission_request.logical_graph;
  const auto property_validation =
      planner::ValidateCanonicalLogicalPropertyCatalog(
          graph, admission_request.logical_properties);
  if (!property_validation.accepted) {
    const auto& issue = property_validation.issues.front();
    return refuse(issue.diagnostic_id, issue.logical_node_id, {},
                  issue.field_id);
  }

  std::unordered_map<std::uint32_t,
                     const planner::CanonicalLogicalRelationalNode*>
      nodes_by_id;
  for (const auto& node : graph.nodes) {
    nodes_by_id.emplace(node.logical_node_id, &node);
  }
  std::unordered_map<std::string,
                     const planner::CanonicalLogicalPropertyRecord*>
      properties_by_uuid;
  for (const auto& property : admission_request.logical_properties.properties) {
    properties_by_uuid.emplace(property.property_uuid, &property);
  }
  std::unordered_map<std::uint32_t, const CanonicalOptimizerNodeEstimate*>
      estimates_by_node;
  for (const auto& estimate : admission_request.statistics.node_estimates) {
    estimates_by_node.emplace(estimate.logical_node_id, &estimate);
  }

  CanonicalOptimizerAlternativeDomainSnapshot domain;
  domain.capability_snapshot_uuid = admission.capability_snapshot_uuid;
  domain.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
  domain.catalog_epoch_uuid = graph.catalog_epoch_uuid;
  domain.security_context_uuid = graph.security_context_uuid;
  domain.local_transaction_id = graph.local_transaction_id;
  domain.statement_snapshot_id = graph.statement_snapshot_id;
  domain.mga_statement_context = graph.mga_statement_context;
  domain.complete_finite_domain = true;
  domain.engine_owned = true;

  result.capability_catalog.capability_snapshot_uuid =
      admission.capability_snapshot_uuid;
  result.capability_catalog.policy_epoch = admission.policy_epoch;
  result.capability_catalog.engine_owned = true;
  std::unordered_map<std::string, std::size_t> capability_indexes;
  std::unordered_map<std::string,
                     const CanonicalOptimizerImplementationProfile*>
      implementation_by_alternative;
  std::unordered_set<std::string> node_implementations;
  for (const auto& implementation : implementations) {
    const auto node = nodes_by_id.find(implementation.logical_node_id);
    const auto implementation_key =
        std::to_string(implementation.logical_node_id) + ":" +
        implementation.implementation_id;
    if (node == nodes_by_id.end() ||
        node->second->node_kind != implementation.logical_node_kind ||
        implementation.implementation_id.empty() ||
        implementation.capability_uuid.empty() ||
        implementation.transformation_rule_id.empty() ||
        implementation.memory_bytes_required == 0 ||
        implementation.minimum_input_count >
            implementation.maximum_input_count ||
        !node_implementations.insert(implementation_key).second) {
      return refuse("QOW-DIAG-OPTIMIZER-PROFILE-FACTORY-IMPLEMENTATION-V1",
                    implementation.logical_node_id,
                    implementation.implementation_id,
                    "implementation_profile");
    }
    const auto property_kinds = [&](const auto& property_uuids,
                                    auto* output) {
      for (const auto& property_uuid : property_uuids) {
        const auto property = properties_by_uuid.find(property_uuid);
        if (property == properties_by_uuid.end()) return false;
        if (std::ranges::find(*output, property->second->property_kind) ==
            output->end()) {
          output->push_back(property->second->property_kind);
        }
      }
      return true;
    };
    const auto suffix = std::to_string(implementation.logical_node_id) + "." +
                        implementation.implementation_id;
    CanonicalOptimizerAlternativeDomainRecord record;
    record.alternative_uuid =
        DerivedCanonicalUuid(identity_scope, "alternative." + suffix);
    record.capability_uuid = implementation.capability_uuid;
    record.logical_node_id = implementation.logical_node_id;
    record.logical_node_kind = implementation.logical_node_kind;
    record.semantic_variant_id = node->second->semantic_variant_id;
    record.implementation_id = implementation.implementation_id;
    record.minimum_input_count = implementation.minimum_input_count;
    record.maximum_input_count = implementation.maximum_input_count;
    if (!property_kinds(implementation.required_property_uuids,
                        &record.required_property_kinds) ||
        !property_kinds(implementation.delivered_property_uuids,
                        &record.delivered_property_kinds)) {
      return refuse("QOW-DIAG-OPTIMIZER-INVENTORY-PROPERTY-V1",
                    implementation.logical_node_id,
                    implementation.implementation_id,
                    "implementation_property_uuid");
    }
    record.memory_bytes_required = implementation.memory_bytes_required;
    record.spill_supported = implementation.spill_supported;
    record.parallel_safe = implementation.parallel_safe;
    record.parallel_required = implementation.parallel_required;
    record.residual_predicate_required =
        implementation.residual_predicate_required;
    record.storage_recheck_required =
        implementation.storage_recheck_required;
    record.storage_read_capable = implementation.storage_read_capable;
    record.mga_visibility_safe = implementation.mga_visibility_capable;
    record.compatibility_profile_id =
        implementation.compatibility_profile_id;
    record.exact_semantics = true;
    record.native_sblr_compatible = true;
    record.available = implementation.available;
    record.refusal_diagnostic_id = implementation.refusal_diagnostic_id;
    record.engine_owned = true;
    domain.records.push_back(record);
    implementation_by_alternative.emplace(record.alternative_uuid,
                                           &implementation);

    CanonicalExecutorCapabilityRecord capability;
    capability.capability_uuid = implementation.capability_uuid;
    capability.capability_abi_version = 1;
    capability.implementation_id = implementation.implementation_id;
    capability.logical_node_kind = implementation.logical_node_kind;
    capability.physical_node_kind = implementation.physical_node_kind;
    capability.minimum_input_count = implementation.minimum_input_count;
    capability.maximum_input_count = implementation.maximum_input_count;
    capability.supported_property_kinds =
        implementation.supported_property_kinds;
    capability.maximum_memory_bytes =
        admission_request.resource.memory_budget_bytes;
    capability.spill_supported = implementation.spill_supported;
    capability.storage_read_capable = implementation.storage_read_capable;
    capability.mga_visibility_capable =
        implementation.mga_visibility_capable;
    capability.available = implementation.available;
    capability.refusal_diagnostic_id =
        implementation.refusal_diagnostic_id;
    capability.engine_owned = true;
    const auto [capability_it, inserted] = capability_indexes.emplace(
        capability.capability_uuid,
        result.capability_catalog.capabilities.size());
    if (inserted) {
      result.capability_catalog.capabilities.push_back(std::move(capability));
    } else if (!SameCapability(
                   result.capability_catalog.capabilities[capability_it->second],
                   capability)) {
      return refuse("QOW-DIAG-OPTIMIZER-PROFILE-FACTORY-CAPABILITY-V1",
                    implementation.logical_node_id,
                    implementation.implementation_id,
                    "capability_identity");
    }
  }

  result.inventory = EnumerateCanonicalOptimizerAlternativeInventory(
      admission_request, admission, domain);
  if (!result.inventory.accepted ||
      !result.inventory.inventory_complete ||
      !result.inventory.issues.empty()) {
    if (result.inventory.issues.empty()) {
      return refuse("QOW-DIAG-OPTIMIZER-INVENTORY-COVERAGE-V1", 0, {},
                    "inventory_complete");
    }
    const auto& issue = result.inventory.issues.front();
    return refuse(issue.diagnostic_id, issue.logical_node_id, {},
                  issue.field_id);
  }

  result.candidates.reserve(result.inventory.legal_candidate_count);
  for (const auto& receipt : result.inventory.receipts) {
    if (!receipt.legal) continue;
    const auto* implementation =
        implementation_by_alternative.at(receipt.alternative_uuid);
    const auto suffix = std::to_string(receipt.logical_node_id) + "." +
                        receipt.implementation_id;
    std::uint64_t estimated_rows =
        std::max<std::uint64_t>(1, implementation->estimated_rows_hint);
    CostConfidence confidence = CostConfidence::kUnknown;
    const auto estimate = estimates_by_node.find(receipt.logical_node_id);
    if (estimate != estimates_by_node.end()) {
      confidence = estimate->second->confidence;
      if (estimate->second->state == CanonicalOptimizerStatisticState::kKnown &&
          estimate->second->row_count_present) {
        estimated_rows = std::max<std::uint64_t>(1,
                                                estimate->second->row_count);
      }
    }
    if (confidence == CostConfidence::kUnknown ||
        confidence == CostConfidence::kRejected) {
      confidence = CostConfidence::kMedium;
    }
    CanonicalOptimizerSearchCandidateInput candidate;
    candidate.alternative_uuid = receipt.alternative_uuid;
    candidate.logical_node_id = receipt.logical_node_id;
    candidate.semantic_variant_id = receipt.semantic_variant_id;
    candidate.transformation_uuid =
        DerivedCanonicalUuid(identity_scope, "transformation." + suffix);
    candidate.transformation_rule_id =
        implementation->transformation_rule_id;
    candidate.required_property_uuids = receipt.required_property_uuids;
    candidate.delivered_property_uuids = receipt.delivered_property_uuids;
    candidate.enforced_property_uuids = receipt.enforced_property_uuids;
    candidate.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
    candidate.statistics_snapshot_uuid = admission.statistics_snapshot_uuid;
    candidate.statistics_generation = admission.statistics_generation;
    candidate.model_family_id = implementation->model_family_id;
    auto& cost = candidate.cost_terms;
    cost.cost_vector_uuid =
        DerivedCanonicalUuid(identity_scope, "cost-vector." + suffix);
    cost.calibration_profile_uuid = calibration_profile_uuid;
    cost.cpu_units = estimated_rows;
    cost.page_read_sequential_units =
        implementation->page_read_sequential_units;
    cost.page_read_random_units = implementation->page_read_random_units;
    cost.page_write_units = implementation->page_write_units;
    cost.memory_bytes_required = implementation->memory_bytes_required;
    cost.spill_bytes_expected =
        receipt.spill_required
            ? implementation->memory_bytes_required -
                  admission_request.resource.memory_budget_bytes
            : 0;
    cost.mga_visibility_checks_expected =
        implementation->mga_visibility_checks_expected;
    cost.cache_units = implementation->cache_units;
    cost.memory_grant_units = implementation->memory_grant_units;
    cost.spill_units = implementation->spill_units;
    cost.network_units = implementation->network_units;
    cost.compression_units = implementation->compression_units;
    cost.encryption_units = implementation->encryption_units;
    cost.predicate_evaluation_units =
        implementation->predicate_evaluation_units;
    cost.vector_distance_units = implementation->vector_distance_units;
    cost.text_scoring_units = implementation->text_scoring_units;
    cost.spatial_evaluation_units =
        implementation->spatial_evaluation_units;
    cost.udr_invocation_units = implementation->udr_invocation_units;
    cost.mga_units = implementation->mga_units;
    cost.index_maintenance_units =
        implementation->index_maintenance_units;
    if (confidence == CostConfidence::kMedium ||
        confidence == CostConfidence::kLow) {
      cost.uncertainty_penalty = 1;
    }
    cost.confidence = confidence;
    candidate.semantic_preserving = true;
    candidate.transformation_preconditions_satisfied = true;
    candidate.property_enforcement_required =
        receipt.property_enforcement_required;
    candidate.derived_from_admitted_statistics = true;
    candidate.engine_coster_owned = true;
    result.candidates.push_back(std::move(candidate));
  }
  result.accepted = true;
  result.optimizer_owned_enumeration = true;
  result.snapshot_derived = true;
  result.deterministic = true;
  result.data_access_allowed = false;
  return result;
}

}  // namespace scratchbird::engine::optimizer
