// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "model_family_profile_factory.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <type_traits>
#include "../../core/uuid/uuid.hpp"
#include "../planner/logical_plan.hpp"
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>

namespace scratchbird::engine::optimizer {
namespace {

using Uuid = scratchbird::core::platform::Uuid;
bool CanonicalUuid(const Uuid& value) {
  return scratchbird::core::uuid::IsEngineIdentityUuid(value);
}


bool FamilyOperationValid(const ModelFamilyCoordinatorRequestV1& request) {
  if (request.family_id == "document") {
    return request.operation_ids.empty() &&
           (request.operation_id == "DOCUMENT_FIND" ||
            request.operation_id == "DOCUMENT_PATH" ||
            request.operation_id == "DOCUMENT_UNNEST");
  }
  if (request.family_id == "graph") {
    return request.operation_ids.empty() &&
           (request.operation_id == "GRAPH_MATCH" ||
            request.operation_id == "GRAPH_EXPAND");
  }
  if (request.family_id == "key_value") {
    return request.operation_ids.empty() &&
           (request.operation_id == "KEY_VALUE_GET" ||
            request.operation_id == "KEY_VALUE_MULTI_GET" ||
            request.operation_id == "KEY_VALUE_PREFIX_RANGE");
  }
  if (request.family_id == "time_series") {
    return request.operation_ids.empty() &&
           (request.operation_id == "TIME_SERIES_RANGE_READ" ||
            request.operation_id == "TIME_SERIES_BUCKET" ||
            request.operation_id == "TIME_SERIES_DOWNSAMPLE");
  }
  if (request.family_id == "vector") {
    return request.operation_ids.empty() &&
           (request.operation_id == "VECTOR_EXACT_SEARCH" ||
            request.operation_id == "VECTOR_ANN_SEARCH" ||
            request.operation_id == "VECTOR_FILTERED_SEARCH");
  }
  if (request.family_id == "search") {
    return request.operation_ids.empty() &&
           (request.operation_id == "SEARCH_RANKED_QUERY" ||
            request.operation_id == "SEARCH_PHRASE_QUERY" ||
            request.operation_id == "SEARCH_FUZZY_QUERY");
  }
  if (request.family_id == "spatial") {
    const bool exact_chain =
        request.operation_ids == std::vector<std::string>{"SPATIAL_SOURCE"} ||
        request.operation_ids ==
            std::vector<std::string>{"SPATIAL_SOURCE", "SPATIAL_MATCH"} ||
        request.operation_ids ==
            std::vector<std::string>{"SPATIAL_SOURCE", "SPATIAL_NEAREST"} ||
        request.operation_ids == std::vector<std::string>{
                                     "SPATIAL_SOURCE", "SPATIAL_MATCH",
                                     "SPATIAL_NEAREST"};
    const bool exact_projection =
        request.operation_ids.size() == 1
            ? request.operation_id == "SPATIAL_SOURCE"
            : request.operation_ids.size() == 2
                  ? request.operation_id == request.operation_ids.back()
                  : request.operation_id.empty();
    return exact_chain && exact_projection;
  }
  if (request.family_id == "columnar") {
    const bool exact_chain =
        request.operation_ids == std::vector<std::string>{"COLUMNAR_SOURCE"} ||
        request.operation_ids ==
            std::vector<std::string>{"COLUMNAR_SOURCE", "COLUMNAR_FILTER"} ||
        request.operation_ids ==
            std::vector<std::string>{"COLUMNAR_SOURCE", "COLUMNAR_PROJECT"} ||
        request.operation_ids == std::vector<std::string>{
                                     "COLUMNAR_SOURCE", "COLUMNAR_FILTER",
                                     "COLUMNAR_PROJECT"};
    const bool exact_projection =
        request.operation_ids.size() == 1
            ? request.operation_id == "COLUMNAR_SOURCE"
            : request.operation_ids.size() == 2
                  ? request.operation_id == request.operation_ids.back()
                  : request.operation_id.empty();
    return exact_chain && exact_projection;
  }
  return false;
}

std::string ImplementationId(const std::string_view family_id) {
  if (family_id == "document") return "physical_document_path_scan_v1";
  if (family_id == "graph") return "physical_graph_adjacency_scan_v1";
  if (family_id == "key_value") return "physical_key_value_scan_v1";
  if (family_id == "time_series") return "physical_time_series_range_scan_v1";
  if (family_id == "vector") return "physical_vector_search_v1";
  if (family_id == "search") return "physical_search_rank_scan_v1";
  if (family_id == "spatial") return "physical_spatial_index_scan_v1";
  if (family_id == "columnar") return "physical_columnar_zone_scan_v1";
  return {};
}

bool FamilyMetricShapeValid(const ModelFamilyCapabilitySnapshotV1& snapshot) {
  const auto& metric = snapshot.metrics;
  // Counts are observations, not capability-presence sentinels. A known empty
  // source may have zero rows, IO, family work, and resident state.
  return CanonicalUuid(metric.statistics_snapshot_uuid) &&
      CanonicalUuid(metric.property_snapshot_uuid) &&
      CanonicalUuid(metric.calibration_profile_uuid) &&
      metric.statistics_generation != 0 && metric.confidence_basis_points != 0 &&
      metric.confidence_basis_points <= 10'000;
}

std::optional<ModelFamilyCostVectorV1> CostFromSnapshot(
    const Uuid& cost_identity,
    const ModelFamilyCapabilitySnapshotV1& snapshot) {
  const auto& metric = snapshot.metrics;
  ModelFamilyCostVectorV1 cost;
  constexpr std::string_view kScalarizationPolicy =
      "model-family.complete-unit-sum-minus-cache-benefit.v1";
  cost.cost_vector_uuid = cost_identity;
  cost.provenance_uuid = metric.statistics_snapshot_uuid;
  cost.property_snapshot_uuid = metric.property_snapshot_uuid;
  cost.calibration_profile_uuid = metric.calibration_profile_uuid;
  cost.scalarization_policy_id = kScalarizationPolicy;
  cost.provenance_generation = metric.statistics_generation;
  cost.confidence_basis_points = metric.confidence_basis_points;
  cost.startup_units = metric.startup_events;
  const auto add = [](std::uint64_t value, std::uint64_t& total) {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
    total += value;
    return true;
  };
  for (const auto value : {metric.estimated_rows, metric.startup_events,
      metric.predicate_evaluations, metric.vector_distance_evaluations,
      metric.text_score_evaluations, metric.spatial_evaluations, metric.udr_invocations}) {
    if (!add(value, cost.cpu_units)) return {};
  }
  cost.sequential_read_units = metric.sequential_pages;
  cost.random_read_units = metric.random_page_lookups;
  cost.page_write_units = metric.page_writes;
  cost.cache_units = metric.cache_operations;
  cost.memory_bytes_required = metric.working_set_bytes;
  cost.memory_grant_units = metric.memory_grant_units;
  cost.spill_units = metric.spill_bytes;
  cost.network_units = metric.network_bytes;
  cost.compression_units = metric.compressed_bytes;
  cost.encryption_units = metric.encrypted_bytes;
  cost.predicate_evaluation_units = metric.predicate_evaluations;
  cost.vector_distance_units = metric.vector_distance_evaluations;
  cost.text_scoring_units = metric.text_score_evaluations;
  cost.spatial_evaluation_units = metric.spatial_evaluations;
  cost.udr_invocation_units = metric.udr_invocations;
  cost.mga_units = metric.mga_rechecks;
  cost.index_maintenance_units = metric.index_maintenance_operations;
  cost.uncertainty_penalty = metric.uncertainty_events;
  if (!add(10'000 - metric.confidence_basis_points, cost.uncertainty_penalty)) return {};
  cost.risk_penalty = metric.risk_events;
  cost.cache_miss_units = metric.cache_operations;
  cost.memory_allocation_units = metric.working_set_bytes;
  cost.memory_grant_opportunity_units = metric.memory_grant_units;
  cost.spill_write_units = metric.spill_bytes;
  cost.network_bandwidth_units = metric.network_bytes;
  cost.mga_visibility_check_units = metric.mga_rechecks;
  cost.plan_instability_penalty = metric.risk_events;
  cost.complete_dimension_vector = true;
  return cost;
}

template<class T>
void BindField(planner::CanonicalPlannerBindingBytes& out, const T& value) {
  if constexpr (std::is_same_v<T, Uuid>) out.Identity(value);
  else if constexpr (std::is_same_v<T, bool>) out.Flag(value);
  else if constexpr (std::is_same_v<T, std::string>) out.Text(value);
  else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>)
    out.Number(static_cast<std::uint64_t>(value));
  else {
    out.Number(value.size());
    for (const auto& item : value) BindField(out, item);
  }
}
template<class... T>
void BindFields(planner::CanonicalPlannerBindingBytes& out, const T&... value) {
  (BindField(out, value), ...);
}

// Full content binding, not a UUID, hash, or permission receipt.
std::string ProfileBinding(const ModelFamilyProfileFactoryRequestV1& request,
    const std::vector<ModelFamilyCapabilitySnapshotV1>& snapshots) {
  planner::CanonicalPlannerBindingBytes out("model-family-profile-scope-v1");
  const auto& logical = request.logical_request;
  BindFields(out, request.abi_version, request.engine_owned,
      request.parser_profile_authority_claimed, logical.abi_version, logical.family_id,
      logical.operation_ids, logical.operation_id, logical.logical_operator_id,
      logical.composition_profile_id, logical.composition_lexical_source_ordinal,
      logical.composition_arity, logical.logical_node_id, logical.object_uuid,
      logical.output_descriptor_ids, logical.bound_sblr_tree_uuid, logical.catalog_epoch_uuid,
      logical.security_context_uuid, logical.capability_snapshot_uuid, logical.resource_snapshot_uuid,
      logical.statistics_snapshot_uuid, logical.route_snapshot_uuid, logical.catalog_generation,
      logical.current_catalog_generation, logical.security_epoch, logical.policy_epoch,
      logical.resource_epoch, logical.statistics_generation, logical.route_epoch,
      logical.route_generation, logical.memory_budget_bytes, logical.security_admitted,
      logical.parser_planning_authority_claimed, logical.transaction_finality_authority_claimed);
  const auto& mga = logical.mga_statement_context;
  BindFields(out, mga.statement_uuid, mga.owning_transaction_uuid, mga.statement_snapshot_uuid,
      mga.statement_metadata_snapshot_uuid, mga.owning_local_transaction_id,
      mga.visible_committed_high_watermark, mga.oldest_active_transaction_id,
      mga.oldest_interesting_transaction_id, mga.oldest_snapshot_transaction_id,
      mga.retention_horizon_transaction_id, mga.active_excluded_local_transaction_ids,
      mga.in_doubt_excluded_local_transaction_ids, mga.snapshot_kind,
      mga.publication_inventory_next_local_transaction_id, mga.inventory_authoritative,
      mga.complete, mga.current, mga.statement_timestamp);
  out.Number(snapshots.size());
  for (const auto& snapshot : snapshots) {
    BindFields(out, snapshot.route_class, snapshot.provider_uuid, snapshot.capability_uuid,
        snapshot.provider_generation, snapshot.available, snapshot.exact,
        snapshot.residual_recheck_required, snapshot.base_row_mga_recheck_required,
        snapshot.security_recheck_required, snapshot.engine_owned, snapshot.local_scope,
        snapshot.parser_planning_authority_claimed, snapshot.transaction_finality_authority_claimed);
    const auto& metric = snapshot.metrics;
    BindFields(out, metric.statistics_snapshot_uuid, metric.property_snapshot_uuid,
        metric.calibration_profile_uuid, metric.statistics_generation, metric.confidence_basis_points,
        metric.startup_events, metric.estimated_rows, metric.sequential_pages,
        metric.random_page_lookups, metric.page_writes, metric.cache_operations,
        metric.working_set_bytes, metric.memory_grant_units, metric.spill_bytes,
        metric.network_bytes, metric.compressed_bytes, metric.encrypted_bytes,
        metric.predicate_evaluations, metric.vector_distance_evaluations,
        metric.text_score_evaluations, metric.spatial_evaluations, metric.udr_invocations,
        metric.mga_rechecks, metric.index_maintenance_operations, metric.uncertainty_events,
        metric.risk_events);
  }
  out.Text("model-family.complete-unit-sum-minus-cache-benefit.v1");
  return std::move(out).Take();
}

}  // namespace

std::shared_ptr<const ModelFamilyProfileIdentityOwnerV1>
ModelFamilyProfileIdentityOwnerV1::Create(
    std::string binding, std::vector<Key> keys,
    std::uint64_t maximum_binding_bytes) noexcept {
  try {
    if (binding.empty() || binding.size() > maximum_binding_bytes ||
        keys.empty() || keys.size() > 64) return {};
    std::sort(keys.begin(), keys.end());
    if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) return {};
    for (const auto& [route, provider, capability] : keys) {
      if ((route != ModelFamilyAlternativeRouteClassV1::kNative &&
           route != ModelFamilyAlternativeRouteClassV1::kExactCollectionFallback) ||
          !CanonicalUuid(provider) || !CanonicalUuid(capability)) return {};
    }
    auto owner = std::shared_ptr<ModelFamilyProfileIdentityOwnerV1>(
        new ModelFamilyProfileIdentityOwnerV1);
    const auto inventory = scratchbird::core::uuid::IssueRuntimeIdentityV7();
    if (!inventory) return {};
    owner->inventory_uuid_ = *inventory;
    owner->binding_ = std::move(binding);
    owner->identities_.reserve(keys.size());
    std::set<Uuid> issued{*inventory};
    for (const auto& key : keys) {
      const auto alternative = scratchbird::core::uuid::IssueRuntimeIdentityV7();
      const auto cost = scratchbird::core::uuid::IssueRuntimeIdentityV7();
      if (!alternative || !cost || !issued.insert(*alternative).second ||
          !issued.insert(*cost).second) return {};
      owner->identities_.push_back({key, *alternative, *cost});
    }
    return owner;
  } catch (...) { return {}; }
}

const ModelFamilyProfileIdentityOwnerV1::Identities*
ModelFamilyProfileIdentityOwnerV1::Find(const Key& key) const noexcept {
  const auto found = std::lower_bound(identities_.begin(), identities_.end(), key,
      [](const auto& identities, const auto& wanted) { return identities.key < wanted; });
  return found != identities_.end() && found->key == key ? &*found : nullptr;
}

const char* ModelFamilyAlternativeRouteClassNameV1(
    const ModelFamilyAlternativeRouteClassV1 route_class) {
  switch (route_class) {
    case ModelFamilyAlternativeRouteClassV1::kNative: return "native";
    case ModelFamilyAlternativeRouteClassV1::kExactCollectionFallback:
      return "exact_collection_fallback";
  }
  return "invalid";
}

ModelFamilyProfileFactoryResultV1 BuildModelFamilyAlternativeProfilesV1(
    const ModelFamilyProfileFactoryRequestV1& request) {
  ModelFamilyProfileFactoryResultV1 result;
  const auto refuse = [&](std::string diagnostic, std::string detail) {
    result = {};
    result.deterministic = true;
    result.diagnostic_id = std::move(diagnostic);
    result.detail = std::move(detail);
    return result;
  };
  const auto& logical = request.logical_request;
  if (request.abi_version != 1 || logical.memory_budget_bytes == 0 ||
      !request.engine_owned || request.parser_profile_authority_claimed ||
      !logical.candidates.empty() || logical.parser_planning_authority_claimed ||
      logical.transaction_finality_authority_claimed ||
      !FamilyOperationValid(logical) || ImplementationId(logical.family_id).empty() ||
      !CanonicalUuid(logical.statistics_snapshot_uuid) ||
      logical.statistics_generation == 0 ||
      request.capability_snapshots.empty() ||
      request.capability_snapshots.size() > 64) {
    return refuse("SB_MODEL_PROFILE_FACTORY_ADMISSION_REFUSED_V1",
                  "model-family logical request or factory authority is invalid");
  }

  auto snapshots = request.capability_snapshots;
  std::ranges::sort(snapshots, [](const auto& left, const auto& right) {
    if (left.route_class != right.route_class) {
      return left.route_class < right.route_class;
    }
    if (left.provider_uuid != right.provider_uuid) {
      return left.provider_uuid < right.provider_uuid;
    }
    return left.capability_uuid < right.capability_uuid;
  });
  auto owner = request.identity_owner;
  const auto binding = ProfileBinding(request, snapshots);
  if (owner) {
    if (!owner->Matches(binding) || owner->Size() != snapshots.size())
      return refuse("SB_MODEL_PROFILE_FACTORY_ADMISSION_REFUSED_V1",
                    "model-family profile owner scope does not match");
  } else {
    std::vector<ModelFamilyProfileIdentityOwnerV1::Key> keys;
    keys.reserve(snapshots.size());
    for (const auto& snapshot : snapshots)
      keys.emplace_back(snapshot.route_class, snapshot.provider_uuid, snapshot.capability_uuid);
    owner = ModelFamilyProfileIdentityOwnerV1::Create(
        binding, std::move(keys), logical.memory_budget_bytes);
    if (!owner)
      return refuse("SB_MODEL_PROFILE_FACTORY_ADMISSION_REFUSED_V1",
                    "model-family profile identity issuance failed");
  }
  std::set<ModelFamilyProfileIdentityOwnerV1::Key> capability_keys;
  const auto implementation_id = ImplementationId(logical.family_id);
  for (std::size_t ordinal = 0; ordinal < snapshots.size(); ++ordinal) {
    const auto& snapshot = snapshots[ordinal];
    const auto capability_key = ModelFamilyProfileIdentityOwnerV1::Key{
        snapshot.route_class, snapshot.provider_uuid, snapshot.capability_uuid};
    if (!CanonicalUuid(snapshot.provider_uuid) ||
        !CanonicalUuid(snapshot.capability_uuid) ||
        snapshot.provider_generation == 0 || !snapshot.engine_owned ||
        !snapshot.local_scope || snapshot.parser_planning_authority_claimed ||
        snapshot.transaction_finality_authority_claimed ||
        !snapshot.exact || !snapshot.residual_recheck_required ||
        !snapshot.base_row_mga_recheck_required ||
        !snapshot.security_recheck_required ||
        snapshot.metrics.statistics_snapshot_uuid !=
            logical.statistics_snapshot_uuid ||
        snapshot.metrics.statistics_generation !=
            logical.statistics_generation ||
        !FamilyMetricShapeValid(snapshot) ||
        !capability_keys.insert(capability_key).second) {
      return refuse("SB_MODEL_PROFILE_FACTORY_SNAPSHOT_REFUSED_V1",
                    "model-family capability, statistics, or property snapshot is invalid");
    }

    ModelFamilyCandidateV1 candidate;
    candidate.route_class = snapshot.route_class;
    const auto* identities = owner->Find(capability_key);
    if (!identities)
      return refuse("SB_MODEL_PROFILE_FACTORY_ADMISSION_REFUSED_V1",
                    "model-family profile identity binding is absent");
    candidate.alternative_uuid = identities->alternative_uuid;
    candidate.provider_uuid = snapshot.provider_uuid;
    candidate.capability_uuid = snapshot.capability_uuid;
    candidate.implementation_id = implementation_id;
    candidate.provider_generation = snapshot.provider_generation;
    candidate.available = snapshot.available;
    candidate.exact = snapshot.exact;
    candidate.exact_collection_fallback =
        snapshot.route_class ==
        ModelFamilyAlternativeRouteClassV1::kExactCollectionFallback;
    candidate.residual_recheck_required = snapshot.residual_recheck_required;
    candidate.base_row_mga_recheck_required =
        snapshot.base_row_mga_recheck_required;
    candidate.security_recheck_required = snapshot.security_recheck_required;
    candidate.engine_owned = snapshot.engine_owned;
    candidate.local_scope = snapshot.local_scope;
    const auto cost = CostFromSnapshot(identities->cost_vector_uuid, snapshot);
    if (!cost)
      return refuse("SB_MODEL_PROFILE_FACTORY_COST_OVERFLOW_V1",
                    "model-family cost dimension exceeded uint64 range");
    candidate.cost = *cost;
    const auto scalar_score = ScalarizeModelFamilyCostVectorV1(candidate.cost);
    if (!scalar_score.has_value()) {
      return refuse("SB_MODEL_PROFILE_FACTORY_COST_OVERFLOW_V1",
                    "model-family scalarization exceeded uint64 range");
    }
    candidate.cost.scalar_score = *scalar_score;
    result.candidates.push_back(std::move(candidate));
    if (snapshot.route_class == ModelFamilyAlternativeRouteClassV1::kNative) {
      ++result.native_alternative_count;
    } else {
      ++result.exact_fallback_alternative_count;
    }
  }
  result.candidate_inventory_receipt_uuid = owner->InventoryUuid();
  for (auto& candidate : result.candidates) {
    candidate.candidate_inventory_receipt_uuid =
        result.candidate_inventory_receipt_uuid;
  }
  result.identity_owner = std::move(owner);
  result.accepted = true;
  result.optimizer_owned_enumeration = true;
  result.deterministic = true;
  result.data_access_allowed = false;
  result.diagnostic_id = "SB_EXECUTOR_OK";
  return result;
}

ModelFamilyCoordinatorResultV1 PlanOptimizerOwnedModelFamilySourceV1(
    const ModelFamilyProfileFactoryRequestV1& request) {
  const auto inventory = BuildModelFamilyAlternativeProfilesV1(request);
  if (!inventory.accepted || !inventory.optimizer_owned_enumeration ||
      inventory.data_access_allowed || inventory.candidates.empty() ||
      !CanonicalUuid(inventory.candidate_inventory_receipt_uuid)) {
    ModelFamilyCoordinatorResultV1 result;
    result.deterministic = true;
    result.diagnostic_id = inventory.diagnostic_id.empty()
                               ? "SB_MODEL_PROFILE_FACTORY_ADMISSION_REFUSED_V1"
                               : inventory.diagnostic_id;
    result.detail = inventory.detail;
    return result;
  }
  auto logical = request.logical_request;
  logical.candidates = inventory.candidates;
  auto result = CoordinateModelFamilySourceV1(logical);
  if (!result.accepted || !result.selected) return result;
  const auto& selected = result.selected_candidate;
  const auto* retained = inventory.identity_owner->Find(
      {selected.route_class, selected.provider_uuid, selected.capability_uuid});
  if (selected.candidate_inventory_receipt_uuid != inventory.candidate_inventory_receipt_uuid ||
      !retained || retained->alternative_uuid != selected.alternative_uuid ||
      retained->cost_vector_uuid != selected.cost.cost_vector_uuid) {
    result = {};
    result.diagnostic_id = "SB_MODEL_PROFILE_FACTORY_ADMISSION_REFUSED_V1";
    result.detail = "selected model candidate is not owned by the exact factory inventory";
    return result;
  }
  result.physical_dag.model_profile_identity_owner = inventory.identity_owner;
  result.optimizer_owned_enumeration = true;
  result.candidate_inventory_receipt_uuid =
      inventory.candidate_inventory_receipt_uuid;
  return result;
}

}  // namespace scratchbird::engine::optimizer
