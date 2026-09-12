// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_profile_factory.hpp"

#include <algorithm>
#include <array>
#include <type_traits>
#include <set>
#include <limits>
#include <map>
#include <ranges>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace scratchbird::engine::optimizer {
namespace planner = scratchbird::engine::planner;
namespace executor = scratchbird::engine::executor;
namespace {

// Fixed-schema binary content binding; this is not an issued identity.
template<class T>
void ScopeField(planner::CanonicalPlannerBindingBytes& out, const T& value) {
  if constexpr (std::is_same_v<T, planner::CanonicalPlannerUuid>) out.Identity(value);
  else if constexpr (std::is_same_v<T, bool>) out.Flag(value);
  else if constexpr (std::is_same_v<T, std::string>) out.Text(value);
  else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>)
    out.Number(static_cast<std::uint64_t>(value));
  else {
    out.Number(value.size());
    for (const auto& item : value) ScopeField(out, item);
  }
}
template<class... T>
void ScopeFields(planner::CanonicalPlannerBindingBytes& out, const T&... values) {
  (ScopeField(out, values), ...);
}
template<class T, class Key>
std::vector<const T*> OrderedRecords(const std::vector<T>& values, Key key) {
  std::vector<const T*> result;
  result.reserve(values.size());
  for (const auto& value : values) result.push_back(&value);
  std::ranges::sort(result, [&](const auto* left, const auto* right) {
    return key(*left) < key(*right);
  });
  return result;
}

std::string ProfileScopeBinding(
    const CanonicalOptimizerAdmissionRequest& request,
    const CanonicalOptimizerAdmissionResult& admission,
    const CanonicalOptimizerExecutorAvailability& availability,
    const planner::CanonicalPlannerUuid& calibration,
    const std::string& properties) {
  planner::CanonicalPlannerBindingBytes out("optimizer-profile-scope-v1");
  ScopeFields(out, calibration, request.abi_version,
      request.populated_from_admitted_typed_sblr, request.data_access_observed,
      request.parser_planning_authority_claimed);
  const auto& graph = request.logical_graph;
  ScopeFields(out, graph.abi_version, graph.bound_sblr_tree_uuid, graph.catalog_epoch_uuid,
      graph.security_context_uuid, graph.local_transaction_id, graph.statement_snapshot_id,
      planner::SerializeCanonicalMgaStatementContext(graph.mga_statement_context),
      graph.root_logical_node_id, graph.result_descriptor_ids, graph.raw_sql_text_present,
      graph.parser_execution_authority_claimed, graph.transaction_finality_authority_claimed,
      properties);
  out.Number(graph.nodes.size());
  for (const auto* node : OrderedRecords(graph.nodes, [](const auto& value) { return value.logical_node_id; })) {
    ScopeFields(out, node->logical_node_id, node->node_kind, node->input_logical_node_ids,
        node->output_descriptor_ids, node->bound_expression_ids, node->argument_expression_ids,
        node->origin_relational_node_ids, node->required_object_uuids, node->semantic_variant_id,
        node->shareable, node->required_property_uuids, node->delivered_property_uuids,
        node->model_family_identity);
  }
  const auto& catalog = request.catalog;
  ScopeFields(out, catalog.snapshot_uuid, catalog.catalog_epoch_uuid, catalog.catalog_generation,
      catalog.object_uuids, catalog.descriptor_ids, catalog.engine_owned);
  const auto& security = request.security;
  ScopeFields(out, security.security_context_uuid, security.security_epoch, security.policy_epoch,
      security.catalog_generation, security.authorized_object_uuids, security.engine_owned);
  const auto& mga = request.mga;
  ScopeFields(out, mga.local_transaction_id, mga.statement_snapshot_id,
      planner::SerializeCanonicalMgaStatementContext(mga.statement_context), mga.metadata_snapshot_uuid,
      mga.transaction_active, mga.statement_snapshot_fixed, mga.engine_owned, mga.finality_authority_claimed);
  const auto& policy = request.policy_capability;
  ScopeFields(out, policy.policy_snapshot_uuid, policy.policy_epoch, policy.capability_snapshot_uuid,
      policy.capability_abi_version, policy.supported_node_kinds, policy.engine_owned,
      policy.cluster_capability_claimed);
  const auto& resource = request.resource;
  ScopeFields(out, resource.resource_snapshot_uuid, resource.resource_epoch, resource.memory_budget_bytes,
      resource.maximum_candidate_count, resource.maximum_memo_groups, resource.maximum_search_steps,
      resource.maximum_planning_time_ns, resource.spill_allowed, resource.engine_owned);
  const auto& stats = request.statistics;
  ScopeFields(out, stats.abi_version, stats.statistics_snapshot_uuid, stats.catalog_epoch_uuid,
      stats.statistics_generation, stats.admitted_at_monotonic_ns, stats.captured_before_data_access,
      stats.data_access_observed, stats.runtime_actuals_present, stats.parser_statistics_authority_claimed);
  out.Number(stats.node_estimates.size());
  for (const auto* estimate : OrderedRecords(stats.node_estimates, [](const auto& value) { return value.logical_node_id; })) {
    ScopeFields(out, estimate->logical_node_id, estimate->object_uuid, estimate->state,
        estimate->source, estimate->catalog_epoch_uuid, estimate->statistics_snapshot_uuid,
        estimate->statistics_generation, estimate->collected_at_monotonic_ns,
        estimate->admitted_at_monotonic_ns, estimate->maximum_age_ns, estimate->confidence,
        estimate->row_count_present, estimate->row_count, estimate->page_count_present,
        estimate->page_count, estimate->derived_from_runtime_actuals,
        estimate->benchmark_clean_authority_claimed);
  }
  const auto& route = request.route;
  ScopeFields(out, route.route_snapshot_uuid, route.route_epoch, route.route_generation, route.operation_id,
      route.route_id, route.native_local_route, route.engine_owned, route.cluster_route_claimed);
  ScopeFields(out, admission.admitted, admission.planning_allowed, admission.degraded_for_unknown_statistics,
      admission.benchmark_clean_ready, admission.data_access_allowed, admission.bound_sblr_tree_uuid,
      admission.catalog_epoch_uuid, admission.security_context_uuid, admission.capability_snapshot_uuid,
      admission.resource_snapshot_uuid, admission.statistics_snapshot_uuid, admission.route_snapshot_uuid,
      admission.local_transaction_id, admission.statement_snapshot_id,
      planner::SerializeCanonicalMgaStatementContext(admission.mga_statement_context),
      admission.catalog_generation, admission.security_epoch, admission.policy_epoch,
      admission.resource_epoch, admission.statistics_generation, admission.route_epoch, admission.route_generation);
  out.Number(admission.evidence.size());
  for (const auto& evidence : admission.evidence) ScopeFields(out, evidence.stage, evidence.evidence_id);
  out.Number(admission.issues.size());
  for (const auto& issue : admission.issues) ScopeFields(out, issue.stage, issue.diagnostic_id, issue.field_id);
  const auto& capabilities = availability.capability_catalog;
  ScopeFields(out, availability.engine_owned, availability.parser_profile_authority_claimed,
      capabilities.abi_version, capabilities.capability_snapshot_uuid, capabilities.policy_epoch,
      capabilities.engine_owned, capabilities.cluster_catalog_claimed, capabilities.parser_capability_authority_claimed);
  out.Number(capabilities.capabilities.size());
  for (const auto* cap : OrderedRecords(capabilities.capabilities, [](const auto& value) { return value.capability_uuid; })) {
    ScopeFields(out, cap->capability_uuid, cap->capability_abi_version, cap->implementation_id,
        cap->logical_node_kind, cap->physical_node_kind, cap->minimum_input_count, cap->maximum_input_count,
        cap->supported_property_kinds, cap->maximum_memory_bytes, cap->spill_supported,
        cap->storage_read_capable, cap->mga_visibility_capable, cap->available, cap->refusal_diagnostic_id,
        cap->engine_owned, cap->cluster_capability_claimed, cap->parser_execution_authority_claimed,
        cap->transaction_finality_authority_claimed);
  }
  out.Number(availability.node_bindings.size());
  for (const auto* binding : OrderedRecords(availability.node_bindings, [](const auto& value) {
      return std::pair{value.logical_node_id, value.capability_uuid}; })) {
    ScopeFields(out, binding->logical_node_id, binding->capability_uuid, binding->memory_bytes_required,
        binding->available, binding->refusal_diagnostic_id);
  }
  return std::move(out).Take();
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
    const std::map<
        planner::CanonicalPlannerUuid, const planner::CanonicalLogicalPropertyRecord*>&
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

std::shared_ptr<const CanonicalOptimizerProfileIdentityOwner>
CanonicalOptimizerProfileIdentityOwner::Create(
    std::string binding, std::vector<Key> keys,
    std::uint64_t maximum_count, std::uint64_t maximum_binding_bytes) noexcept {
  try {
    if (binding.empty() || binding.size() > maximum_binding_bytes ||
        keys.empty() || keys.size() > maximum_count) return {};
    std::sort(keys.begin(), keys.end());
    if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) return {};
    for (const auto& [node, capability] : keys)
      if (node == 0 || !scratchbird::core::uuid::IsEngineIdentityUuid(capability)) return {};
    auto owner = std::shared_ptr<CanonicalOptimizerProfileIdentityOwner>(
        new CanonicalOptimizerProfileIdentityOwner);
    const auto scope = scratchbird::core::uuid::IssueRuntimeIdentityV7();
    if (!scope) return {};
    owner->scope_uuid_ = *scope;
    owner->binding_ = std::move(binding);
    owner->identities_.reserve(keys.size());
    std::set<planner::CanonicalPlannerUuid> issued{*scope};
    for (const auto& [node, capability] : keys) {
      const auto alternative = scratchbird::core::uuid::IssueRuntimeIdentityV7();
      const auto transformation = scratchbird::core::uuid::IssueRuntimeIdentityV7();
      const auto cost = scratchbird::core::uuid::IssueRuntimeIdentityV7();
      if (!alternative || !transformation || !cost ||
          !issued.insert(*alternative).second || !issued.insert(*transformation).second ||
          !issued.insert(*cost).second) return {};
      owner->identities_.push_back({node, capability, *alternative, *transformation, *cost});
    }
    return owner;
  } catch (...) { return {}; }
}

const CanonicalOptimizerProfileIdentities* CanonicalOptimizerProfileIdentityOwner::Find(
    std::uint32_t node, const planner::CanonicalPlannerUuid& capability) const noexcept {
  const auto key = Key{node, capability};
  const auto found = std::lower_bound(identities_.begin(), identities_.end(), key,
      [](const auto& identities, const auto& wanted) {
        return Key{identities.logical_node_id, identities.capability_uuid} < wanted;
      });
  return found != identities_.end() && found->logical_node_id == node &&
      found->capability_uuid == capability ? &*found : nullptr;
}

CanonicalOptimizerProfileFactoryResult
BuildCanonicalOptimizerAlternativeProfiles(
    const CanonicalOptimizerAdmissionRequest& admission_request,
    const CanonicalOptimizerAdmissionResult& admission,
    const CanonicalOptimizerExecutorAvailability& executor_availability,
    planner::CanonicalPlannerUuid calibration_profile_uuid,
    std::shared_ptr<const CanonicalOptimizerProfileIdentityOwner> identity_owner) {
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
      !scratchbird::core::uuid::IsEngineIdentityUuid(calibration_profile_uuid) ||
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

  const auto serialized_properties = planner::SerializeCanonicalLogicalPropertyCatalog(
      graph, admission_request.logical_properties);
  const auto statistics_validation = AdmitCanonicalOptimizerStatisticsBeforeAccess(
      graph, admission_request.statistics);
  if (!serialized_properties.accepted || !statistics_validation.accepted)
    return refuse("QOW-DIAG-OPTIMIZER-PROFILE-FACTORY-ADMISSION-V1", 0, {}, "scope_metadata");
  const auto scope_binding = ProfileScopeBinding(admission_request, admission,
      executor_availability, calibration_profile_uuid,
      serialized_properties.canonical_serialization);
  if (identity_owner) {
    if (!identity_owner->Matches(scope_binding) ||
        identity_owner->Size() != executor_availability.node_bindings.size())
      return refuse("QOW-DIAG-OPTIMIZER-PROFILE-FACTORY-ADMISSION-V1", 0, {}, "identity_owner_scope");
  } else {
    std::vector<CanonicalOptimizerProfileIdentityOwner::Key> keys;
    keys.reserve(executor_availability.node_bindings.size());
    for (const auto& binding : executor_availability.node_bindings)
      keys.emplace_back(binding.logical_node_id, binding.capability_uuid);
    identity_owner = CanonicalOptimizerProfileIdentityOwner::Create(scope_binding, std::move(keys),
        admission_request.resource.maximum_candidate_count, admission_request.resource.memory_budget_bytes);
    if (!identity_owner)
      return refuse("QOW-DIAG-OPTIMIZER-PROFILE-FACTORY-ADMISSION-V1", 0, {}, "identity_owner_issuance");
  }

  std::unordered_map<std::uint32_t,
                     const planner::CanonicalLogicalRelationalNode*>
      nodes_by_id;
  for (const auto& node : graph.nodes) {
    nodes_by_id.emplace(node.logical_node_id, &node);
  }
  std::map<planner::CanonicalPlannerUuid,
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

  std::map<planner::CanonicalPlannerUuid, const CanonicalExecutorCapabilityRecord*>
      capabilities_by_uuid;
  for (const auto& capability :
       executor_availability.capability_catalog.capabilities) {
    const auto [it, inserted] = capabilities_by_uuid.emplace(
        capability.capability_uuid, &capability);
    if (!scratchbird::core::uuid::IsEngineIdentityUuid(capability.capability_uuid) ||
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
  std::map<planner::CanonicalPlannerUuid,
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
    CanonicalOptimizerAlternativeDomainRecord record;
    const auto* identities = identity_owner->Find(binding->logical_node_id, binding->capability_uuid);
    if (!identities)
      return refuse("QOW-DIAG-OPTIMIZER-PROFILE-FACTORY-ADMISSION-V1",
                    binding->logical_node_id, implementation_id, "identity_owner_binding");
    record.alternative_uuid = identities->alternative_uuid;
    record.capability_uuid = binding->capability_uuid;
    record.logical_node_id = binding->logical_node_id;
    record.logical_node_kind = capability->second->logical_node_kind;
    record.semantic_variant_id = node->second->semantic_variant_id;
    record.implementation_id = implementation_id;
    record.minimum_input_count = capability->second->minimum_input_count;
    record.maximum_input_count = capability->second->maximum_input_count;
    std::vector<planner::CanonicalPlannerUuid> input_required_property_uuids;
    for (const auto& property_uuid : node->second->required_property_uuids) {
      // A Window carries its input ordering forward, but does not create that
      // ordering.  Retain the ordering as an input requirement even though it
      // is also present in the Window's delivered-property set.  Enforcers
      // such as Sort continue to remove properties they produce themselves.
      if (node->second->node_kind ==
              planner::CanonicalLogicalRelationalNodeKind::kWindow ||
          std::ranges::find(node->second->delivered_property_uuids,
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
    std::uint64_t estimated_rows = 1;
    std::uint64_t estimated_pages = 0;
    CostConfidence confidence = CostConfidence::kUnknown;
    const auto estimate = estimates_by_node.find(receipt.logical_node_id);
    if (estimate != estimates_by_node.end()) {
      confidence = estimate->second->confidence;
      if (estimate->second->state == CanonicalOptimizerStatisticState::kKnown &&
          estimate->second->row_count_present) {
        estimated_rows = estimate->second->row_count;
      }
      if (estimate->second->state == CanonicalOptimizerStatisticState::kKnown &&
          estimate->second->page_count_present) {
        estimated_pages = estimate->second->page_count;
      }
    }
    if (confidence == CostConfidence::kUnknown ||
        confidence == CostConfidence::kRejected) {
      confidence = CostConfidence::kLow;
    }
    CanonicalOptimizerSearchCandidateInput candidate;
    candidate.alternative_uuid = receipt.alternative_uuid;
    candidate.logical_node_id = receipt.logical_node_id;
    candidate.semantic_variant_id = receipt.semantic_variant_id;
    const auto* identities = identity_owner->Find(binding->logical_node_id, binding->capability_uuid);
    candidate.transformation_uuid = identities->transformation_uuid;
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
    cost.cost_vector_uuid = identities->cost_vector_uuid;
    cost.calibration_profile_uuid = calibration_profile_uuid;
    cost.scalarization_policy_id =
        "canonical.optimizer.complete-unit-sum-minus-cache-benefit.v1";
    cost.cpu_units = SaturatingMultiply(estimated_rows,
                                        CpuMultiplier(node->node_kind));
    if (node->node_kind ==
        planner::CanonicalLogicalRelationalNodeKind::kRelationSource) {
      if (receipt.implementation_id.find("index") != std::string::npos) {
        const bool known_pages = estimate != estimates_by_node.end() &&
            estimate->second->state == CanonicalOptimizerStatisticState::kKnown &&
            estimate->second->page_count_present;
        cost.page_read_random_units = known_pages
            ? estimated_pages / 8 + (estimated_pages % 8 != 0) : 1;
        cost.cache_units = known_pages && estimated_pages == 0
            ? 0 : std::max<std::uint64_t>(1, estimated_pages / 16);
        if (!known_pages) confidence = CostConfidence::kLow;
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
    cost.expression_evaluation_units = SaturatingMultiply(
        estimated_rows, node->bound_expression_ids.size());
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
            planner::CanonicalLogicalPropertyKind::kDistribution)) {
      cost.repartition_units = estimated_rows;
      cost.cluster_coordination_units = 1;
    }
    if (HasCostlyProperty(*node, properties_by_uuid,
                          planner::CanonicalLogicalPropertyKind::kLocality)) {
      cost.remote_execution_startup_units = 1;
    }
    if (HasCostlyProperty(*node, properties_by_uuid,
                          planner::CanonicalLogicalPropertyKind::kOrdering) ||
        HasCostlyProperty(
            *node, properties_by_uuid,
            planner::CanonicalLogicalPropertyKind::kVectorOrdering) ||
        HasCostlyProperty(
            *node, properties_by_uuid,
            planner::CanonicalLogicalPropertyKind::kTextScoreOrdering) ||
        HasCostlyProperty(
            *node, properties_by_uuid,
            planner::CanonicalLogicalPropertyKind::kTimeOrdering)) {
      cost.result_ordering_enforcement_units = estimated_rows;
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
    // Preserve the Core cost dimensions independently.  The older aggregate
    // fields remain populated for compatible readers, but complete vectors are
    // scalarized and published from these exact terms.
    cost.cache_miss_units = cost.cache_units;
    cost.memory_allocation_units = cost.memory_bytes_required;
    cost.memory_grant_opportunity_units = cost.memory_grant_units;
    cost.spill_write_units = cost.spill_bytes_expected;
    cost.spill_read_units = cost.spill_units;
    cost.mga_version_traversal_units = cost.mga_units;
    cost.mga_visibility_check_units =
        cost.mga_visibility_checks_expected;
    cost.archive_fetch_units = cost.archive_fetches_expected;
    cost.network_latency_units = cost.network_units;
    cost.network_bandwidth_units = cost.network_bytes_expected;
    cost.plan_instability_penalty = cost.risk_penalty;
    cost.complete_dimension_vector = true;
    cost.confidence = confidence;
    candidate.semantic_preserving = true;
    candidate.transformation_preconditions_satisfied = true;
    candidate.property_enforcement_required =
        receipt.property_enforcement_required;
    candidate.derived_from_admitted_statistics = true;
    candidate.engine_coster_owned = true;
    result.candidates.push_back(std::move(candidate));
  }
  result.identity_owner = std::move(identity_owner);
  result.accepted = true;
  result.optimizer_owned_enumeration = true;
  result.snapshot_derived = true;
  result.deterministic = true;
  result.data_access_allowed = false;
  return result;
}

}  // namespace scratchbird::engine::optimizer
