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

std::uint64_t SaturatingAdd(const std::uint64_t left,
                            const std::uint64_t right) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left + right;
}

std::uint64_t SaturatingMultiply(const std::uint64_t left,
                                 const std::uint64_t right) {
  if (left != 0 &&
      right > std::numeric_limits<std::uint64_t>::max() / left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left * right;
}

std::uint64_t CpuMultiplier(
    const planner::CanonicalLogicalRelationalNodeKind kind) {
  using Kind = planner::CanonicalLogicalRelationalNodeKind;
  switch (kind) {
    case Kind::kRelationSource:
    case Kind::kProject:
    case Kind::kLimit:
    case Kind::kValues: return 1;
    case Kind::kFilter:
    case Kind::kCte: return 2;
    case Kind::kUnpivot: return 3;
    case Kind::kJoin:
    case Kind::kAggregate:
    case Kind::kSetOperation:
    case Kind::kSubquery:
    case Kind::kPivot: return 4;
    case Kind::kWindow:
    case Kind::kTableFunctionInvoke: return 5;
    case Kind::kSort:
    case Kind::kRecursiveCte: return 6;
    case Kind::kMatchRecognize: return 8;
  }
  return 1;
}

std::string ModelFamilyId(
    const planner::CanonicalLogicalRelationalNode& node) {
  using Family = planner::CanonicalLogicalModelFamilyIdentity;
  switch (node.model_family_identity) {
    case Family::kDocument: return "document.local.v1";
    case Family::kGraph: return "graph.local.v1";
    case Family::kKeyValue: return "key_value.local.v1";
    case Family::kTimeSeries: return "time_series.local.v1";
    case Family::kVector: return "vector.local.v1";
    case Family::kSearch: return "search.local.v1";
    case Family::kSpatial: return "spatial.local.v1";
    case Family::kColumnar: return "columnar.local.v1";
    case Family::kUnspecified: break;
  }
  return "relational.local.v1";
}

bool HasCostlyProperty(
    const planner::CanonicalLogicalRelationalNode& node,
    const std::unordered_map<
        std::string, const planner::CanonicalLogicalPropertyRecord*>&
        properties,
    const planner::CanonicalLogicalPropertyKind kind) {
  const auto contains = [&](const auto& values) {
    return std::ranges::any_of(values, [&](const auto& uuid) {
      const auto property = properties.find(uuid);
      return property != properties.end() &&
             property->second->property_kind == kind;
    });
  };
  return contains(node.required_property_uuids) ||
         contains(node.delivered_property_uuids);
}

}  // namespace

CanonicalOptimizerProfileFactoryResult
BuildCanonicalOptimizerAlternativeProfiles(
    const CanonicalOptimizerAdmissionRequest& admission_request,
    const CanonicalOptimizerAdmissionResult& admission,
    const CanonicalOptimizerExecutorAvailability& executor_availability,
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
      !executor_availability.engine_owned ||
      executor_availability.parser_profile_authority_claimed ||
      !executor_availability.capability_catalog.engine_owned ||
      executor_availability.capability_catalog.capability_snapshot_uuid !=
          admission.capability_snapshot_uuid ||
      executor_availability.capability_catalog.policy_epoch !=
          admission.policy_epoch ||
      executor_availability.capability_catalog.capabilities.empty() ||
      executor_availability.node_bindings.empty() ||
      executor_availability.node_bindings.size() >
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

  std::unordered_map<std::string, const CanonicalExecutorCapabilityRecord*>
      capabilities_by_uuid;
  for (const auto& capability :
       executor_availability.capability_catalog.capabilities) {
    const auto [it, inserted] = capabilities_by_uuid.emplace(
        capability.capability_uuid, &capability);
    if (capability.capability_uuid.empty() ||
        capability.implementation_id.empty() ||
        capability.capability_abi_version != 1 || !capability.engine_owned ||
        capability.minimum_input_count > capability.maximum_input_count ||
        capability.maximum_memory_bytes == 0 ||
        (!inserted && !SameCapability(*it->second, capability))) {
      return refuse("QOW-DIAG-OPTIMIZER-PROFILE-FACTORY-CAPABILITY-V1", 0,
                    capability.implementation_id, "capability_catalog");
    }
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

  result.capability_catalog = executor_availability.capability_catalog;
  std::ranges::sort(result.capability_catalog.capabilities,
                    [](const auto& left, const auto& right) {
                      return left.capability_uuid < right.capability_uuid;
                    });
  std::unordered_map<std::string,
                     const CanonicalOptimizerNodeCapabilityBinding*>
      binding_by_alternative;
  std::unordered_set<std::string> node_implementations;
  std::vector<const CanonicalOptimizerNodeCapabilityBinding*> bindings;
  bindings.reserve(executor_availability.node_bindings.size());
  for (const auto& binding : executor_availability.node_bindings) {
    bindings.push_back(&binding);
  }
  std::ranges::sort(bindings, [](const auto* left, const auto* right) {
    if (left->logical_node_id != right->logical_node_id) {
      return left->logical_node_id < right->logical_node_id;
    }
    return left->capability_uuid < right->capability_uuid;
  });
  for (const auto* binding : bindings) {
    const auto node = nodes_by_id.find(binding->logical_node_id);
    const auto capability = capabilities_by_uuid.find(binding->capability_uuid);
    const auto implementation_id =
        capability == capabilities_by_uuid.end()
            ? std::string{}
            : capability->second->implementation_id;
    const auto implementation_key =
        std::to_string(binding->logical_node_id) + ":" + implementation_id;
    if (node == nodes_by_id.end() ||
        capability == capabilities_by_uuid.end() ||
        node->second->node_kind != capability->second->logical_node_kind ||
        binding->memory_bytes_required == 0 ||
        binding->memory_bytes_required >
            capability->second->maximum_memory_bytes ||
        (binding->available && !binding->refusal_diagnostic_id.empty()) ||
        (!binding->available && binding->refusal_diagnostic_id.empty()) ||
        !node_implementations.insert(implementation_key).second) {
      return refuse("QOW-DIAG-OPTIMIZER-PROFILE-FACTORY-IMPLEMENTATION-V1",
                    binding->logical_node_id, implementation_id,
                    "executor_availability_binding");
    }
    const auto property_kinds = [&](const auto& property_uuids,
                                    auto* output) {
      for (const auto& property_uuid : property_uuids) {
        const auto property = properties_by_uuid.find(property_uuid);
        if (property == properties_by_uuid.end() ||
            std::ranges::find(
                capability->second->supported_property_kinds,
                property->second->property_kind) ==
                capability->second->supported_property_kinds.end()) {
          return false;
        }
        if (std::ranges::find(*output, property->second->property_kind) ==
            output->end()) {
          output->push_back(property->second->property_kind);
        }
      }
      return true;
    };
    const auto suffix = std::to_string(binding->logical_node_id) + "." +
                        implementation_id;
    CanonicalOptimizerAlternativeDomainRecord record;
    record.alternative_uuid =
        DerivedCanonicalUuid(identity_scope, "alternative." + suffix);
    record.capability_uuid = binding->capability_uuid;
    record.logical_node_id = binding->logical_node_id;
    record.logical_node_kind = capability->second->logical_node_kind;
    record.semantic_variant_id = node->second->semantic_variant_id;
    record.implementation_id = implementation_id;
    record.minimum_input_count = capability->second->minimum_input_count;
    record.maximum_input_count = capability->second->maximum_input_count;
    std::vector<std::string> input_required_property_uuids;
    for (const auto& property_uuid : node->second->required_property_uuids) {
      if (std::ranges::find(node->second->delivered_property_uuids,
                            property_uuid) ==
          node->second->delivered_property_uuids.end()) {
        input_required_property_uuids.push_back(property_uuid);
      }
    }
    if (!property_kinds(input_required_property_uuids,
                        &record.required_property_kinds) ||
        !property_kinds(node->second->delivered_property_uuids,
                        &record.delivered_property_kinds)) {
      return refuse("QOW-DIAG-OPTIMIZER-INVENTORY-PROPERTY-V1",
                    binding->logical_node_id, implementation_id,
                    "logical_property_uuid");
    }
    record.memory_bytes_required = binding->memory_bytes_required;
    record.spill_supported = capability->second->spill_supported;
    record.parallel_safe = true;
    record.parallel_required = false;
    record.residual_predicate_required = false;
    record.storage_recheck_required = false;
    record.storage_read_capable = capability->second->storage_read_capable;
    record.mga_visibility_safe = capability->second->mga_visibility_capable;
    record.compatibility_profile_id = "native.sblr.row.v1";
    record.exact_semantics = true;
    record.native_sblr_compatible = true;
    record.available = binding->available && capability->second->available;
    record.refusal_diagnostic_id =
        !binding->available ? binding->refusal_diagnostic_id
                            : capability->second->refusal_diagnostic_id;
    record.engine_owned = true;
    domain.records.push_back(record);
    binding_by_alternative.emplace(record.alternative_uuid, binding);
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
    const auto* binding = binding_by_alternative.at(receipt.alternative_uuid);
    const auto* capability =
        capabilities_by_uuid.at(binding->capability_uuid);
    const auto* node = nodes_by_id.at(receipt.logical_node_id);
    const auto suffix = std::to_string(receipt.logical_node_id) + "." +
                        receipt.implementation_id;
    std::uint64_t estimated_rows = 1;
    std::uint64_t estimated_pages = 0;
    CostConfidence confidence = CostConfidence::kUnknown;
    const auto estimate = estimates_by_node.find(receipt.logical_node_id);
    if (estimate != estimates_by_node.end()) {
      confidence = estimate->second->confidence;
      if (estimate->second->state == CanonicalOptimizerStatisticState::kKnown &&
          estimate->second->row_count_present) {
        estimated_rows = std::max<std::uint64_t>(1,
                                                estimate->second->row_count);
      }
      if (estimate->second->state == CanonicalOptimizerStatisticState::kKnown &&
          estimate->second->page_count_present) {
        estimated_pages = estimate->second->page_count;
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
        "canonical.optimizer." +
        std::string(planner::CanonicalLogicalRelationalNodeKindName(
            node->node_kind)) +
        "." + receipt.implementation_id;
    candidate.required_property_uuids = receipt.required_property_uuids;
    candidate.delivered_property_uuids = receipt.delivered_property_uuids;
    candidate.enforced_property_uuids = receipt.enforced_property_uuids;
    candidate.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
    candidate.statistics_snapshot_uuid = admission.statistics_snapshot_uuid;
    candidate.statistics_generation = admission.statistics_generation;
    candidate.model_family_id = ModelFamilyId(*node);
    auto& cost = candidate.cost_terms;
    cost.cost_vector_uuid =
        DerivedCanonicalUuid(identity_scope, "cost-vector." + suffix);
    cost.calibration_profile_uuid = calibration_profile_uuid;
    cost.cpu_units = SaturatingMultiply(estimated_rows,
                                        CpuMultiplier(node->node_kind));
    if (node->node_kind ==
        planner::CanonicalLogicalRelationalNodeKind::kRelationSource) {
      if (receipt.implementation_id.find("index") != std::string::npos) {
        cost.page_read_random_units =
            estimated_pages == 0 ? 1 : (estimated_pages + 7) / 8;
        cost.cache_units = std::max<std::uint64_t>(1, estimated_pages / 16);
      } else {
        cost.page_read_sequential_units = estimated_pages;
      }
    }
    cost.memory_bytes_required = std::min(
        binding->memory_bytes_required,
        admission_request.resource.memory_budget_bytes);
    cost.spill_bytes_expected =
        receipt.spill_required
            ? (binding->memory_bytes_required >
                       admission_request.resource.memory_budget_bytes
                   ? binding->memory_bytes_required -
                         admission_request.resource.memory_budget_bytes
                   : 0)
            : 0;
    cost.memory_grant_units = cost.memory_bytes_required;
    cost.spill_units = (cost.spill_bytes_expected + 4095) / 4096;
    if (capability->mga_visibility_capable) {
      cost.mga_visibility_checks_expected = estimated_rows;
      cost.mga_units = estimated_rows;
    }
    using Kind = planner::CanonicalLogicalRelationalNodeKind;
    if (node->node_kind == Kind::kFilter || node->node_kind == Kind::kJoin ||
        node->node_kind == Kind::kMatchRecognize) {
      cost.predicate_evaluation_units = SaturatingMultiply(
          estimated_rows,
          std::max<std::uint64_t>(1, node->bound_expression_ids.size()));
    }
    if (node->model_family_identity ==
        planner::CanonicalLogicalModelFamilyIdentity::kVector) {
      cost.vector_distance_units = estimated_rows;
    } else if (node->model_family_identity ==
               planner::CanonicalLogicalModelFamilyIdentity::kSearch) {
      cost.text_scoring_units = estimated_rows;
    } else if (node->model_family_identity ==
               planner::CanonicalLogicalModelFamilyIdentity::kSpatial) {
      cost.spatial_evaluation_units = estimated_rows;
    }
    if (node->node_kind == Kind::kTableFunctionInvoke) {
      cost.udr_invocation_units = estimated_rows;
    }
    if (HasCostlyProperty(
            *node, properties_by_uuid,
            planner::CanonicalLogicalPropertyKind::kDistribution) ||
        HasCostlyProperty(*node, properties_by_uuid,
                          planner::CanonicalLogicalPropertyKind::kLocality)) {
      cost.network_units = std::min(
          SaturatingMultiply(
              estimated_rows,
              std::max<std::uint64_t>(1,
                                      node->output_descriptor_ids.size())),
          std::numeric_limits<std::uint64_t>::max() / 8);
      cost.network_bytes_expected = cost.network_units * 8;
    }
    if (HasCostlyProperty(
            *node, properties_by_uuid,
            planner::CanonicalLogicalPropertyKind::kSecurityVisibility)) {
      cost.encryption_units = estimated_rows;
      cost.mga_units = SaturatingAdd(cost.mga_units, estimated_rows);
    }
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
