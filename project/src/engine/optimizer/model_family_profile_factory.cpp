// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "model_family_profile_factory.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>

namespace scratchbird::engine::optimizer {
namespace {

bool CanonicalUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-' ||
      value == "00000000-0000-0000-0000-000000000000") {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto byte = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(byte) || std::isupper(byte)) return false;
  }
  return true;
}

std::uint64_t Fnv1a64(const std::string_view value,
                      std::uint64_t hash = 14695981039346656037ull) {
  for (const auto byte : value) {
    hash ^= static_cast<std::uint8_t>(byte);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string DerivedUuid(const std::string_view scope,
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

std::uint64_t SaturatingAdd(const std::uint64_t left,
                            const std::uint64_t right) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left + right;
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

bool FamilyMetricShapeValid(const std::string_view family_id,
                            const ModelFamilyCapabilitySnapshotV1& snapshot) {
  const auto& metric = snapshot.metrics;
  if (!CanonicalUuid(metric.statistics_snapshot_uuid) ||
      !CanonicalUuid(metric.property_snapshot_uuid) ||
      !CanonicalUuid(metric.calibration_profile_uuid) ||
      metric.statistics_generation == 0 ||
      metric.confidence_basis_points == 0 ||
      metric.confidence_basis_points > 10'000 ||
      metric.estimated_rows == 0 || metric.working_set_bytes == 0) {
    return false;
  }
  if (snapshot.route_class ==
          ModelFamilyAlternativeRouteClassV1::kExactCollectionFallback) {
    return metric.sequential_pages != 0;
  }
  if (family_id == "vector") {
    return metric.vector_distance_evaluations != 0;
  }
  if (family_id == "search") {
    return metric.text_score_evaluations != 0;
  }
  if (family_id == "spatial") {
    return metric.spatial_evaluations != 0;
  }
  return metric.sequential_pages != 0 || metric.random_page_lookups != 0;
}

ModelFamilyCostVectorV1 CostFromSnapshot(
    const std::string_view identity_scope,
    const std::string_view family_id,
    const std::size_t ordinal,
    const ModelFamilyCapabilitySnapshotV1& snapshot) {
  const auto& metric = snapshot.metrics;
  ModelFamilyCostVectorV1 cost;
  const auto route = ModelFamilyAlternativeRouteClassNameV1(snapshot.route_class);
  constexpr std::string_view kScalarizationPolicy =
      "model-family.complete-unit-sum-minus-cache-benefit.v1";
  cost.cost_vector_uuid = DerivedUuid(
      identity_scope, "model-family.cost." + std::string(family_id) + "." +
                          route + "." + snapshot.provider_uuid + "." +
                          snapshot.capability_uuid + "." +
                          std::to_string(ordinal) + "." +
                          std::string(kScalarizationPolicy));
  cost.provenance_uuid = metric.statistics_snapshot_uuid;
  cost.property_snapshot_uuid = metric.property_snapshot_uuid;
  cost.calibration_profile_uuid = metric.calibration_profile_uuid;
  cost.scalarization_policy_id = kScalarizationPolicy;
  cost.provenance_generation = metric.statistics_generation;
  cost.confidence_basis_points = metric.confidence_basis_points;
  cost.startup_units = metric.startup_events;
  cost.cpu_units = SaturatingAdd(metric.estimated_rows,
                                 metric.startup_events);
  cost.cpu_units = SaturatingAdd(cost.cpu_units,
                                 metric.predicate_evaluations);
  cost.cpu_units = SaturatingAdd(cost.cpu_units,
                                 metric.vector_distance_evaluations);
  cost.cpu_units = SaturatingAdd(cost.cpu_units,
                                 metric.text_score_evaluations);
  cost.cpu_units = SaturatingAdd(cost.cpu_units,
                                 metric.spatial_evaluations);
  cost.cpu_units = SaturatingAdd(cost.cpu_units, metric.udr_invocations);
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
  cost.uncertainty_penalty = SaturatingAdd(
      metric.uncertainty_events, 10'000 - metric.confidence_basis_points);
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

std::string SnapshotReceiptSeed(
    const ModelFamilyCapabilitySnapshotV1& snapshot) {
  const auto& metric = snapshot.metrics;
  const auto boolean = [](const bool value) { return value ? "1" : "0"; };
  std::ostringstream out;
  out << ModelFamilyAlternativeRouteClassNameV1(snapshot.route_class) << '|'
      << snapshot.provider_uuid << '|' << snapshot.capability_uuid << '|'
      << snapshot.provider_generation << '|' << boolean(snapshot.available)
      << '|' << boolean(snapshot.exact) << '|'
      << boolean(snapshot.residual_recheck_required) << '|'
      << boolean(snapshot.base_row_mga_recheck_required) << '|'
      << boolean(snapshot.security_recheck_required) << '|'
      << metric.statistics_snapshot_uuid << '|'
      << metric.property_snapshot_uuid << '|'
      << metric.calibration_profile_uuid << '|'
      << metric.statistics_generation << '|'
      << metric.confidence_basis_points << '|' << metric.startup_events << '|'
      << metric.estimated_rows << '|' << metric.sequential_pages << '|'
      << metric.random_page_lookups << '|' << metric.page_writes << '|'
      << metric.cache_operations << '|' << metric.working_set_bytes << '|'
      << metric.memory_grant_units << '|' << metric.spill_bytes << '|'
      << metric.network_bytes << '|' << metric.compressed_bytes << '|'
      << metric.encrypted_bytes << '|' << metric.predicate_evaluations << '|'
      << metric.vector_distance_evaluations << '|'
      << metric.text_score_evaluations << '|'
      << metric.spatial_evaluations << '|' << metric.udr_invocations << '|'
      << metric.mga_rechecks << '|'
      << metric.index_maintenance_operations << '|'
      << metric.uncertainty_events << '|' << metric.risk_events << '|'
      << "model-family.complete-unit-sum-minus-cache-benefit.v1" << '|'
      << "complete-dimension-vector-v1";
  return out.str();
}

}  // namespace

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
  if (request.abi_version != 1 || request.identity_scope.empty() ||
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
  std::set<std::string> capability_keys;
  std::string receipt_seed = request.identity_scope + "|" + logical.family_id +
                             "|" + logical.operation_id + "|" +
                             logical.statistics_snapshot_uuid + "|" +
                             std::to_string(logical.statistics_generation);
  const auto implementation_id = ImplementationId(logical.family_id);
  for (std::size_t ordinal = 0; ordinal < snapshots.size(); ++ordinal) {
    const auto& snapshot = snapshots[ordinal];
    const auto route = ModelFamilyAlternativeRouteClassNameV1(snapshot.route_class);
    const auto capability_key = std::string(route) + "|" +
                                snapshot.provider_uuid + "|" +
                                snapshot.capability_uuid;
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
        !FamilyMetricShapeValid(logical.family_id, snapshot) ||
        !capability_keys.insert(capability_key).second) {
      return refuse("SB_MODEL_PROFILE_FACTORY_SNAPSHOT_REFUSED_V1",
                    "model-family capability, statistics, or property snapshot is invalid");
    }

    ModelFamilyCandidateV1 candidate;
    candidate.route_class = snapshot.route_class;
    candidate.alternative_uuid = DerivedUuid(
        request.identity_scope,
        "model-family.alternative." + logical.family_id + "." + route + "." +
            snapshot.provider_uuid + "." + snapshot.capability_uuid + "." +
            std::to_string(ordinal));
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
    candidate.cost = CostFromSnapshot(request.identity_scope, logical.family_id,
                                      ordinal, snapshot);
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
    receipt_seed += "|" + SnapshotReceiptSeed(snapshot);
  }
  result.candidate_inventory_receipt_uuid =
      DerivedUuid(receipt_seed, "model-family.inventory.v1");
  for (auto& candidate : result.candidates) {
    candidate.candidate_inventory_receipt_uuid =
        result.candidate_inventory_receipt_uuid;
  }
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
  if (!result.accepted || !result.selected ||
      result.selected_candidate.candidate_inventory_receipt_uuid !=
          inventory.candidate_inventory_receipt_uuid) {
    return result;
  }
  result.optimizer_owned_enumeration = true;
  result.candidate_inventory_receipt_uuid =
      inventory.candidate_inventory_receipt_uuid;
  return result;
}

}  // namespace scratchbird::engine::optimizer
