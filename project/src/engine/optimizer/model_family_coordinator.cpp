// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "model_family_coordinator.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_set>

namespace scratchbird::engine::optimizer {
namespace {

struct ActiveMultilegDescriptorDispatchV1 {
  std::string statement_uuid;
  std::vector<MultilegDescriptorProfileV1> profiles;
};

thread_local std::optional<ActiveMultilegDescriptorDispatchV1>
    g_active_multileg_descriptor_dispatch_v1;

bool CanonicalUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-' ||
      value == "00000000-0000-0000-0000-000000000000") {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) return false;
  }
  return true;
}

std::string DerivedUuid(const std::string_view seed) {
  std::uint64_t high = 1469598103934665603ULL;
  std::uint64_t low = 1099511628211ULL;
  for (const auto ch : seed) {
    high = (high ^ static_cast<unsigned char>(ch)) * 1099511628211ULL;
    low = (low + static_cast<unsigned char>(ch)) * 1469598103934665603ULL;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string raw(32, '0');
  for (std::size_t index = 0; index < 16; ++index) {
    raw[index] = kHex[(high >> ((15 - index) * 4)) & 0xf];
    raw[16 + index] = kHex[(low >> ((15 - index) * 4)) & 0xf];
  }
  raw[12] = '7';
  raw[16] = kHex[(static_cast<unsigned>(raw[16] <= '9'
                                            ? raw[16] - '0'
                                            : raw[16] - 'a' + 10) &
                  0x3) |
                 0x8];
  return raw.substr(0, 8) + "-" + raw.substr(8, 4) + "-" +
         raw.substr(12, 4) + "-" + raw.substr(16, 4) + "-" +
         raw.substr(20, 12);
}

bool Add(std::uint64_t value, std::uint64_t* total) {
  if (std::numeric_limits<std::uint64_t>::max() - *total < value) return false;
  *total += value;
  return true;
}

std::string JsonEscape(const std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char ch : value) {
    if (ch == '\\' || ch == '"') escaped.push_back('\\');
    escaped.push_back(ch);
  }
  return escaped;
}

bool CandidateScore(const ModelFamilyCandidateV1& candidate,
                    std::uint64_t* score) {
  const auto scalar_score = ScalarizeModelFamilyCostVectorV1(candidate.cost);
  if (!scalar_score.has_value()) return false;
  *score = *scalar_score;
  const bool optimizer_owned =
      !candidate.candidate_inventory_receipt_uuid.empty();
  return !optimizer_owned ||
          (candidate.cost.scalarization_policy_id ==
               "model-family.complete-unit-sum-minus-cache-benefit.v1" &&
           candidate.cost.complete_dimension_vector &&
           candidate.cost.scalar_score == *score);
}

void RetainModelFamilyCostVector(
    const ModelFamilyCostVectorV1& source,
    scratchbird::engine::executor::PhysicalCostVectorReceipt* retained) {
  if (retained == nullptr) return;
  retained->cost_vector_uuid = source.cost_vector_uuid;
  retained->calibration_profile_uuid = source.calibration_profile_uuid;
  retained->scalarization_policy_id = source.scalarization_policy_id;
  retained->scalar_score = source.scalar_score;
  retained->cpu_units = source.cpu_units;
  retained->page_read_sequential_units = source.sequential_read_units;
  retained->page_read_random_units = source.random_read_units;
  retained->page_write_units = source.page_write_units;
  retained->memory_bytes_required = source.memory_bytes_required;
  retained->spill_bytes_expected = source.spill_units;
  retained->network_bytes_expected = source.network_units;
  retained->mga_visibility_checks_expected = source.mga_units;
  retained->archive_fetches_expected = source.archive_fetch_units;
  retained->uncertainty_penalty = source.uncertainty_penalty;
  retained->risk_penalty = source.risk_penalty;
  retained->cache_units = source.cache_units;
  retained->memory_grant_units = source.memory_grant_units;
  retained->spill_units = source.spill_units;
  retained->network_units = source.network_units;
  retained->compression_units = source.compression_units;
  retained->encryption_units = source.encryption_units;
  retained->predicate_evaluation_units =
      source.predicate_evaluation_units;
  retained->vector_distance_units = source.vector_distance_units;
  retained->text_scoring_units = source.text_scoring_units;
  retained->spatial_evaluation_units = source.spatial_evaluation_units;
  retained->udr_invocation_units = source.udr_invocation_units;
  retained->mga_units = source.mga_units;
  retained->index_maintenance_units = source.index_maintenance_units;
  retained->cache_miss_units = source.cache_miss_units;
  retained->cache_residency_benefit_units =
      source.cache_residency_benefit_units;
  retained->memory_allocation_units = source.memory_allocation_units;
  retained->memory_grant_opportunity_units =
      source.memory_grant_opportunity_units;
  retained->spill_write_units = source.spill_write_units;
  retained->spill_read_units = source.spill_read_units;
  retained->temp_space_pressure_units = source.temp_space_pressure_units;
  retained->decompression_units = source.decompression_units;
  retained->decryption_units = source.decryption_units;
  retained->expression_evaluation_units = source.expression_evaluation_units;
  retained->domain_cast_units = source.domain_cast_units;
  retained->datatype_conversion_units = source.datatype_conversion_units;
  retained->collation_comparison_units = source.collation_comparison_units;
  retained->mga_version_traversal_units =
      source.mga_version_traversal_units;
  retained->mga_visibility_check_units = source.mga_visibility_check_units;
  retained->archive_fetch_units = source.archive_fetch_units;
  retained->garbage_retention_pressure_units =
      source.garbage_retention_pressure_units;
  retained->lock_latch_wait_risk_units = source.lock_latch_wait_risk_units;
  retained->network_latency_units = source.network_latency_units;
  retained->network_bandwidth_units = source.network_bandwidth_units;
  retained->remote_execution_startup_units =
      source.remote_execution_startup_units;
  retained->cluster_coordination_units = source.cluster_coordination_units;
  retained->repartition_units = source.repartition_units;
  retained->broadcast_units = source.broadcast_units;
  retained->replica_staleness_risk_units =
      source.replica_staleness_risk_units;
  retained->quorum_availability_risk_units =
      source.quorum_availability_risk_units;
  retained->donor_compatibility_enforcement_units =
      source.donor_compatibility_enforcement_units;
  retained->result_ordering_enforcement_units =
      source.result_ordering_enforcement_units;
  retained->plan_instability_penalty = source.plan_instability_penalty;
  retained->complete_dimension_vector = source.complete_dimension_vector;
  retained->confidence = source.confidence_basis_points >= 9500
                             ? 0
                             : source.confidence_basis_points >= 8500
                                   ? 1
                                   : source.confidence_basis_points >= 7000
                                         ? 2
                                         : 3;
}

bool DependencyCostValid(const ModelFamilyCostVectorV1& cost) {
  const bool optimizer_owned = !cost.scalarization_policy_id.empty();
  const auto scalar_score = ScalarizeModelFamilyCostVectorV1(cost);
  return CanonicalUuid(cost.cost_vector_uuid) &&
         CanonicalUuid(cost.provenance_uuid) &&
         cost.provenance_generation != 0 &&
         cost.confidence_basis_points != 0 &&
         cost.confidence_basis_points <= 10'000 &&
         scalar_score.has_value() &&
         (!optimizer_owned ||
          (CanonicalUuid(cost.property_snapshot_uuid) &&
           CanonicalUuid(cost.calibration_profile_uuid) &&
           cost.scalarization_policy_id ==
               "model-family.complete-unit-sum-minus-cache-benefit.v1" &&
           cost.complete_dimension_vector &&
           cost.scalar_score == *scalar_score));
}

bool DependencyCostEqual(const ModelFamilyCostVectorV1& left,
                         const ModelFamilyCostVectorV1& right) {
  return left == right;
}

bool DependencyAlternativeLess(
    const ModelFamilyDependencyAlternativeV1& left,
    const ModelFamilyDependencyAlternativeV1& right) {
  return left.authority_approved_comparison_rank <
             right.authority_approved_comparison_rank ||
         (left.authority_approved_comparison_rank ==
              right.authority_approved_comparison_rank &&
          left.alternative_uuid < right.alternative_uuid);
}

bool DependencyOperationValid(const std::string_view family_id,
                              const std::vector<std::string>& operation_ids,
                              const std::string_view operation_id) {
  if (family_id == "relational") {
    return operation_ids.empty() && operation_id == "RELATIONAL_HEAP_SCAN";
  }
  if (family_id == "document") {
    return operation_ids.empty() &&
           (operation_id == "DOCUMENT_FIND" ||
            operation_id == "DOCUMENT_PATH" ||
            operation_id == "DOCUMENT_UNNEST");
  }
  if (family_id == "graph") {
    return operation_ids.empty() &&
           (operation_id == "GRAPH_MATCH" || operation_id == "GRAPH_EXPAND");
  }
  if (family_id == "key_value") {
    return operation_ids.empty() &&
           (operation_id == "KEY_VALUE_GET" ||
            operation_id == "KEY_VALUE_MULTI_GET" ||
            operation_id == "KEY_VALUE_PREFIX_RANGE");
  }
  if (family_id == "time_series") {
    return operation_ids.empty() &&
           (operation_id == "TIME_SERIES_RANGE_READ" ||
            operation_id == "TIME_SERIES_BUCKET" ||
            operation_id == "TIME_SERIES_DOWNSAMPLE");
  }
  if (family_id == "vector") {
    return operation_ids.empty() &&
           (operation_id == "VECTOR_EXACT_SEARCH" ||
            operation_id == "VECTOR_ANN_SEARCH" ||
            operation_id == "VECTOR_FILTERED_SEARCH");
  }
  if (family_id == "search") {
    return operation_ids.empty() &&
           (operation_id == "SEARCH_RANKED_QUERY" ||
            operation_id == "SEARCH_PHRASE_QUERY" ||
            operation_id == "SEARCH_FUZZY_QUERY");
  }
  const auto exact_projection = [&](const std::string_view source) {
    if (operation_ids.empty() || operation_ids.front() != source) return false;
    if (operation_ids.size() == 1) return operation_id == source;
    if (operation_ids.size() == 2) return operation_id == operation_ids.back();
    return operation_ids.size() == 3 && operation_id.empty();
  };
  if (family_id == "spatial") {
    return exact_projection("SPATIAL_SOURCE") &&
           (operation_ids == std::vector<std::string>{"SPATIAL_SOURCE"} ||
            operation_ids == std::vector<std::string>{"SPATIAL_SOURCE", "SPATIAL_MATCH"} ||
            operation_ids == std::vector<std::string>{"SPATIAL_SOURCE", "SPATIAL_NEAREST"} ||
            operation_ids == std::vector<std::string>{"SPATIAL_SOURCE", "SPATIAL_MATCH", "SPATIAL_NEAREST"});
  }
  if (family_id == "columnar") {
    return exact_projection("COLUMNAR_SOURCE") &&
           (operation_ids == std::vector<std::string>{"COLUMNAR_SOURCE"} ||
            operation_ids == std::vector<std::string>{"COLUMNAR_SOURCE", "COLUMNAR_FILTER"} ||
            operation_ids == std::vector<std::string>{"COLUMNAR_SOURCE", "COLUMNAR_PROJECT"} ||
            operation_ids == std::vector<std::string>{"COLUMNAR_SOURCE", "COLUMNAR_FILTER", "COLUMNAR_PROJECT"});
  }
  return false;
}

bool CheckedMultiply(const std::uint64_t left,
                     const std::uint64_t right,
                     std::uint64_t* product) {
  if (product == nullptr ||
      (right != 0 &&
       left > std::numeric_limits<std::uint64_t>::max() / right)) {
    return false;
  }
  *product = left * right;
  return true;
}

bool CanonicalDescriptorLineage(
    const std::vector<std::uint32_t>& descriptor_ids,
    const std::vector<std::string>& descriptor_uuids) {
  if (descriptor_ids.empty() || descriptor_ids.size() != descriptor_uuids.size()) {
    return false;
  }
  std::set<std::uint32_t> ids;
  std::set<std::string> uuids;
  for (std::size_t index = 0; index < descriptor_ids.size(); ++index) {
    if (descriptor_ids[index] == 0 || !ids.insert(descriptor_ids[index]).second ||
        !CanonicalUuid(descriptor_uuids[index]) ||
        !uuids.insert(descriptor_uuids[index]).second) {
      return false;
    }
  }
  return true;
}

bool CanonicalStatementTimestamp(const std::string_view value) {
  if (value.size() != 20 && (value.size() < 22 || value.size() > 30)) {
    return false;
  }
  if (value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value.back() != 'Z') {
    return false;
  }
  constexpr std::size_t kDigits[] = {
      0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
  for (const auto index : kDigits) {
    if (value[index] < '0' || value[index] > '9') return false;
  }
  if (value.size() > 20) {
    if (value[19] != '.') return false;
    for (std::size_t index = 20; index + 1 < value.size(); ++index) {
      if (value[index] < '0' || value[index] > '9') return false;
    }
  }
  const auto decimal = [&](const std::size_t begin,
                           const std::size_t count) {
    unsigned out = 0;
    for (std::size_t index = 0; index < count; ++index) {
      out = out * 10 + static_cast<unsigned>(value[begin + index] - '0');
    }
    return out;
  };
  const auto year = decimal(0, 4);
  const auto month = decimal(5, 2);
  const auto day = decimal(8, 2);
  const auto hour = decimal(11, 2);
  const auto minute = decimal(14, 2);
  const auto second = decimal(17, 2);
  if (year == 0 || month == 0 || month > 12 || hour > 23 || minute > 59 ||
      second > 59) {
    return false;
  }
  constexpr unsigned kDays[] = {
      0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  auto maximum_day = kDays[month];
  if (month == 2 &&
      ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
    ++maximum_day;
  }
  return day != 0 && day <= maximum_day;
}

bool ExactOrderedOperationChain(const std::string_view family_id,
                                const std::vector<std::string>& operation_ids,
                                const std::string_view operation_id) {
  const auto exact_projection = [&](const std::string_view source) {
    if (operation_ids.empty() || operation_ids.front() != source) return false;
    if (operation_ids.size() == 1) return operation_id == source;
    if (operation_ids.size() == 2) return operation_id == operation_ids.back();
    return operation_ids.size() == 3 && operation_id.empty();
  };
  if (family_id == "spatial") {
    const bool exact_chain =
        operation_ids == std::vector<std::string>{"SPATIAL_SOURCE"} ||
        operation_ids ==
            std::vector<std::string>{"SPATIAL_SOURCE", "SPATIAL_MATCH"} ||
        operation_ids ==
            std::vector<std::string>{"SPATIAL_SOURCE", "SPATIAL_NEAREST"} ||
        operation_ids == std::vector<std::string>{
                             "SPATIAL_SOURCE", "SPATIAL_MATCH",
                             "SPATIAL_NEAREST"};
    return exact_chain && exact_projection("SPATIAL_SOURCE");
  }
  if (family_id == "columnar") {
    const bool exact_chain =
        operation_ids == std::vector<std::string>{"COLUMNAR_SOURCE"} ||
        operation_ids ==
            std::vector<std::string>{"COLUMNAR_SOURCE", "COLUMNAR_FILTER"} ||
        operation_ids ==
            std::vector<std::string>{"COLUMNAR_SOURCE", "COLUMNAR_PROJECT"} ||
        operation_ids == std::vector<std::string>{
                             "COLUMNAR_SOURCE", "COLUMNAR_FILTER",
                             "COLUMNAR_PROJECT"};
    return exact_chain && exact_projection("COLUMNAR_SOURCE");
  }
  return operation_ids.empty();
}

}  // namespace

void RetainModelFamilyCostVectorV1(
    const ModelFamilyCostVectorV1& source,
    scratchbird::engine::executor::PhysicalCostVectorReceipt* retained) {
  RetainModelFamilyCostVector(source, retained);
}

std::optional<std::uint64_t> ScalarizeModelFamilyCostVectorV1(
    const ModelFamilyCostVectorV1& cost) {
  if (cost.complete_dimension_vector &&
      cost.scalarization_policy_id !=
          "model-family.complete-unit-sum-minus-cache-benefit.v1") {
    return std::nullopt;
  }
  std::uint64_t score = 0;
  const auto add = [&](const std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - score) {
      return false;
    }
    score += value;
    return true;
  };
  const std::uint64_t complete_dimensions[] = {
      cost.cpu_units,
      cost.sequential_read_units,
      cost.random_read_units,
      cost.page_write_units,
      cost.cache_miss_units,
      cost.memory_allocation_units,
      cost.memory_grant_opportunity_units,
      cost.spill_write_units,
      cost.spill_read_units,
      cost.temp_space_pressure_units,
      cost.compression_units,
      cost.decompression_units,
      cost.encryption_units,
      cost.decryption_units,
      cost.predicate_evaluation_units,
      cost.expression_evaluation_units,
      cost.udr_invocation_units,
      cost.vector_distance_units,
      cost.text_scoring_units,
      cost.spatial_evaluation_units,
      cost.domain_cast_units,
      cost.datatype_conversion_units,
      cost.collation_comparison_units,
      cost.mga_version_traversal_units,
      cost.mga_visibility_check_units,
      cost.archive_fetch_units,
      cost.garbage_retention_pressure_units,
      cost.index_maintenance_units,
      cost.lock_latch_wait_risk_units,
      cost.network_latency_units,
      cost.network_bandwidth_units,
      cost.remote_execution_startup_units,
      cost.cluster_coordination_units,
      cost.repartition_units,
      cost.broadcast_units,
      cost.replica_staleness_risk_units,
      cost.quorum_availability_risk_units,
      cost.donor_compatibility_enforcement_units,
      cost.result_ordering_enforcement_units,
      cost.uncertainty_penalty,
      cost.plan_instability_penalty,
  };
  const std::uint64_t legacy_dimensions[] = {
      cost.startup_units,
      cost.cpu_units,
      cost.sequential_read_units,
      cost.random_read_units,
      cost.page_write_units,
      cost.cache_units,
      cost.memory_bytes_required,
      cost.memory_grant_units,
      cost.spill_units,
      cost.network_units,
      cost.compression_units,
      cost.encryption_units,
      cost.predicate_evaluation_units,
      cost.vector_distance_units,
      cost.text_scoring_units,
      cost.spatial_evaluation_units,
      cost.udr_invocation_units,
      cost.mga_units,
      cost.index_maintenance_units,
      cost.uncertainty_penalty,
      cost.risk_penalty,
  };
  if (cost.complete_dimension_vector) {
    for (const auto dimension : complete_dimensions) {
      if (!add(dimension)) return std::nullopt;
    }
    score = score >= cost.cache_residency_benefit_units
                ? score - cost.cache_residency_benefit_units
                : 0;
  } else {
    for (const auto dimension : legacy_dimensions) {
      if (!add(dimension)) return std::nullopt;
    }
  }
  return score;
}

std::string SerializeModelFamilyCostVectorToJsonV1(
    const ModelFamilyCostVectorV1& cost) {
  std::ostringstream out;
  out << "{\"cost_vector_uuid\":\"" << JsonEscape(cost.cost_vector_uuid)
      << "\",\"provenance_uuid\":\"" << JsonEscape(cost.provenance_uuid)
      << "\",\"property_snapshot_uuid\":\""
      << JsonEscape(cost.property_snapshot_uuid)
      << "\",\"calibration_profile_uuid\":\""
      << JsonEscape(cost.calibration_profile_uuid)
      << "\",\"scalarization_policy_id\":\""
      << JsonEscape(cost.scalarization_policy_id)
      << "\",\"provenance_generation\":" << cost.provenance_generation
      << ",\"confidence_basis_points\":" << cost.confidence_basis_points
      << ",\"scalar_score\":" << cost.scalar_score
      << ",\"startup_units\":" << cost.startup_units
      << ",\"cpu_units\":" << cost.cpu_units
      << ",\"sequential_read_units\":" << cost.sequential_read_units
      << ",\"random_read_units\":" << cost.random_read_units
      << ",\"page_write_units\":" << cost.page_write_units
      << ",\"cache_units\":" << cost.cache_units
      << ",\"memory_bytes_required\":" << cost.memory_bytes_required
      << ",\"memory_grant_units\":" << cost.memory_grant_units
      << ",\"spill_units\":" << cost.spill_units
      << ",\"network_units\":" << cost.network_units
      << ",\"compression_units\":" << cost.compression_units
      << ",\"encryption_units\":" << cost.encryption_units
      << ",\"predicate_evaluation_units\":"
      << cost.predicate_evaluation_units
      << ",\"vector_distance_units\":" << cost.vector_distance_units
      << ",\"text_scoring_units\":" << cost.text_scoring_units
      << ",\"spatial_evaluation_units\":" << cost.spatial_evaluation_units
      << ",\"udr_invocation_units\":" << cost.udr_invocation_units
      << ",\"mga_units\":" << cost.mga_units
      << ",\"index_maintenance_units\":"
      << cost.index_maintenance_units
      << ",\"cache_miss_units\":" << cost.cache_miss_units
      << ",\"cache_residency_benefit_units\":"
      << cost.cache_residency_benefit_units
      << ",\"memory_allocation_units\":"
      << cost.memory_allocation_units
      << ",\"memory_grant_opportunity_units\":"
      << cost.memory_grant_opportunity_units
      << ",\"spill_write_units\":" << cost.spill_write_units
      << ",\"spill_read_units\":" << cost.spill_read_units
      << ",\"temp_space_pressure_units\":"
      << cost.temp_space_pressure_units
      << ",\"decompression_units\":" << cost.decompression_units
      << ",\"decryption_units\":" << cost.decryption_units
      << ",\"expression_evaluation_units\":"
      << cost.expression_evaluation_units
      << ",\"domain_cast_units\":" << cost.domain_cast_units
      << ",\"datatype_conversion_units\":"
      << cost.datatype_conversion_units
      << ",\"collation_comparison_units\":"
      << cost.collation_comparison_units
      << ",\"mga_version_traversal_units\":"
      << cost.mga_version_traversal_units
      << ",\"mga_visibility_check_units\":"
      << cost.mga_visibility_check_units
      << ",\"archive_fetch_units\":" << cost.archive_fetch_units
      << ",\"garbage_retention_pressure_units\":"
      << cost.garbage_retention_pressure_units
      << ",\"lock_latch_wait_risk_units\":"
      << cost.lock_latch_wait_risk_units
      << ",\"network_latency_units\":" << cost.network_latency_units
      << ",\"network_bandwidth_units\":"
      << cost.network_bandwidth_units
      << ",\"remote_execution_startup_units\":"
      << cost.remote_execution_startup_units
      << ",\"cluster_coordination_units\":"
      << cost.cluster_coordination_units
      << ",\"repartition_units\":" << cost.repartition_units
      << ",\"broadcast_units\":" << cost.broadcast_units
      << ",\"replica_staleness_risk_units\":"
      << cost.replica_staleness_risk_units
      << ",\"quorum_availability_risk_units\":"
      << cost.quorum_availability_risk_units
      << ",\"donor_compatibility_enforcement_units\":"
      << cost.donor_compatibility_enforcement_units
      << ",\"result_ordering_enforcement_units\":"
      << cost.result_ordering_enforcement_units
      << ",\"uncertainty_penalty\":" << cost.uncertainty_penalty
      << ",\"risk_penalty\":" << cost.risk_penalty
      << ",\"plan_instability_penalty\":"
      << cost.plan_instability_penalty
      << ",\"complete_dimension_vector\":"
      << (cost.complete_dimension_vector ? "true" : "false") << '}';
  return out.str();
}

ModelFamilyCoordinatorResultV1 CoordinateModelFamilySourceV1(
    const ModelFamilyCoordinatorRequestV1& request) {
  // QOW-SOURCE-RCP-074-COMMON-MODEL-COORDINATOR-V1
  ModelFamilyCoordinatorResultV1 result;
  const bool document_family = request.family_id == "document";
  const bool graph_family = request.family_id == "graph";
  const bool key_value_family = request.family_id == "key_value";
  const bool time_series_family = request.family_id == "time_series";
  const bool vector_family = request.family_id == "vector";
  const bool search_family = request.family_id == "search";
  const bool spatial_family = request.family_id == "spatial";
  const bool columnar_family = request.family_id == "columnar";
  const bool unnest = request.operation_id == "DOCUMENT_UNNEST";
  const bool valid_operation =
      (document_family &&
       (request.operation_id == "DOCUMENT_FIND" ||
        request.operation_id == "DOCUMENT_PATH" || unnest)) ||
      (graph_family &&
       (request.operation_id == "GRAPH_MATCH" ||
        request.operation_id == "GRAPH_EXPAND")) ||
      (key_value_family &&
       (request.operation_id == "KEY_VALUE_GET" ||
        request.operation_id == "KEY_VALUE_MULTI_GET" ||
        request.operation_id == "KEY_VALUE_PREFIX_RANGE")) ||
      (time_series_family &&
       (request.operation_id == "TIME_SERIES_RANGE_READ" ||
        request.operation_id == "TIME_SERIES_BUCKET" ||
        request.operation_id == "TIME_SERIES_DOWNSAMPLE")) ||
      (vector_family &&
       (request.operation_id == "VECTOR_EXACT_SEARCH" ||
        request.operation_id == "VECTOR_ANN_SEARCH" ||
        request.operation_id == "VECTOR_FILTERED_SEARCH")) ||
      (search_family &&
       (request.operation_id == "SEARCH_RANKED_QUERY" ||
        request.operation_id == "SEARCH_PHRASE_QUERY" ||
        request.operation_id == "SEARCH_FUZZY_QUERY")) ||
      ((spatial_family || columnar_family) &&
       ExactOrderedOperationChain(request.family_id, request.operation_ids,
                                  request.operation_id));
  const std::string expected_logical_operator =
      graph_family ? "LOGICAL_GRAPH_SOURCE_V1"
                   : key_value_family ? "LOGICAL_KEY_VALUE_SOURCE_V1"
                   : time_series_family ? "LOGICAL_TIME_SERIES_SOURCE_V1"
                   : vector_family ? "LOGICAL_VECTOR_SOURCE_V1"
                   : search_family ? "LOGICAL_SEARCH_SOURCE_V1"
                   : spatial_family ? "LOGICAL_SPATIAL_SOURCE_V1"
                   : columnar_family ? "LOGICAL_COLUMNAR_SOURCE_V1"
                                      : "LOGICAL_DOCUMENT_SOURCE_V1";
  const std::string expected_physical_operator =
      graph_family ? "PHYSICAL_GRAPH_ADJACENCY_SCAN_V1"
                   : key_value_family ? "PHYSICAL_KEY_VALUE_SCAN_V1"
                   : time_series_family ? "PHYSICAL_TIME_SERIES_RANGE_SCAN_V1"
                   : vector_family ? "PHYSICAL_VECTOR_SEARCH_V1"
                   : search_family ? "PHYSICAL_SEARCH_RANK_SCAN_V1"
                   : spatial_family ? "PHYSICAL_SPATIAL_INDEX_SCAN_V1"
                   : columnar_family ? "PHYSICAL_COLUMNAR_ZONE_SCAN_V1"
                                      : "PHYSICAL_DOCUMENT_PATH_SCAN_V1";
  const std::string expected_implementation =
      graph_family ? "physical_graph_adjacency_scan_v1"
                   : key_value_family ? "physical_key_value_scan_v1"
                   : time_series_family ? "physical_time_series_range_scan_v1"
                   : vector_family ? "physical_vector_search_v1"
                   : search_family ? "physical_search_rank_scan_v1"
                   : spatial_family ? "physical_spatial_index_scan_v1"
                   : columnar_family ? "physical_columnar_zone_scan_v1"
                                      : "physical_document_path_scan_v1";
  result.logical_operator_id = expected_logical_operator;
  result.physical_operator_id = expected_physical_operator;
  const auto refuse = [&](const char* diagnostic, std::string detail) {
    result.diagnostic_id = diagnostic;
    result.detail = std::move(detail);
    return result;
  };

  const bool timestamp_family =
      key_value_family || time_series_family || vector_family || search_family ||
      spatial_family || columnar_family;
  const auto exact_common_composition = [&] {
    if (request.composition_profile_id == "COMP-3-LINEAR-V1") {
      return request.composition_arity == 3 &&
             request.composition_lexical_source_ordinal < 3;
    }
    if (request.composition_profile_id == "COMP-4-MIXED-V1") {
      constexpr std::array<std::string_view, 4> families = {
          "relational", "document", "graph", "vector"};
      return request.composition_arity == families.size() &&
             request.composition_lexical_source_ordinal < families.size() &&
             request.family_id ==
                 families[request.composition_lexical_source_ordinal];
    }
    if (request.composition_profile_id == "COMP-9-FULL-UNIVERSE-V1") {
      constexpr std::array<std::string_view, 9> families = {
          "relational", "document", "graph", "key_value", "time_series",
          "vector", "search", "spatial", "columnar"};
      return request.composition_arity == families.size() &&
             request.composition_lexical_source_ordinal < families.size() &&
             request.family_id ==
                 families[request.composition_lexical_source_ordinal];
    }
    return false;
  }();
  const bool has_composition_carrier =
      !request.composition_profile_id.empty() || request.composition_arity != 0;
  const bool timestamp_present =
      !request.mga_statement_context.statement_timestamp.empty();
  if ((has_composition_carrier && !exact_common_composition) ||
      (timestamp_family && !timestamp_present) ||
      (!timestamp_family && timestamp_present && !exact_common_composition) ||
      (timestamp_present &&
       !CanonicalStatementTimestamp(
           request.mga_statement_context.statement_timestamp))) {
    return refuse(time_series_family
                      ? "SB_MODEL_TIME_SERIES_TIMESTAMP_INVALID_V1"
                      : ((vector_family || search_family || spatial_family ||
                          columnar_family)
                             ? "SB_MODEL_MGA_CONTEXT_MISMATCH_V1"
                             : "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1"),
                  "model-family coordinator statement timestamp is invalid");
  }
  if (request.abi_version != 1 ||
      (!document_family && !graph_family && !key_value_family &&
       !time_series_family && !vector_family && !search_family &&
       !spatial_family && !columnar_family) ||
      !valid_operation ||
      !ExactOrderedOperationChain(request.family_id, request.operation_ids,
                                  request.operation_id) ||
      request.logical_operator_id != expected_logical_operator ||
      request.logical_node_id == 0 || request.output_descriptor_ids.empty() ||
      (!unnest && !CanonicalUuid(request.object_uuid)) ||
      (unnest && !request.object_uuid.empty()) ||
      !CanonicalUuid(request.bound_sblr_tree_uuid) ||
      !CanonicalUuid(request.catalog_epoch_uuid) ||
      !CanonicalUuid(request.security_context_uuid) ||
      !CanonicalUuid(request.capability_snapshot_uuid) ||
      !CanonicalUuid(request.resource_snapshot_uuid) ||
      !CanonicalUuid(request.statistics_snapshot_uuid) ||
      !CanonicalUuid(request.route_snapshot_uuid) ||
      !scratchbird::engine::executor::PhysicalMgaStatementContextValid(
          request.mga_statement_context) ||
      request.catalog_generation == 0 || request.security_epoch == 0 ||
      request.policy_epoch == 0 || request.resource_epoch == 0 ||
      request.statistics_generation == 0 || request.route_epoch == 0 ||
      request.route_generation == 0 || request.memory_budget_bytes == 0 ||
      request.parser_planning_authority_claimed ||
      request.transaction_finality_authority_claimed) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "model-family logical source request is incomplete");
  }
  if (request.catalog_generation != request.current_catalog_generation) {
    return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                  "model-family catalog generation is stale");
  }
  if (!request.security_admitted) {
    return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                  "model-family source security admission was refused");
  }

  std::unordered_set<std::string> alternative_ids;
  const ModelFamilyCandidateV1* selected = nullptr;
  std::uint64_t selected_score = 0;
  bool fallback_seen = false;
  bool memory_refusal_observed = false;
  std::string optimizer_inventory_receipt_uuid;
  for (const auto& candidate : request.candidates) {
    std::uint64_t score = 0;
    const bool optimizer_owned_candidate =
        !candidate.candidate_inventory_receipt_uuid.empty();
    const bool route_class_consistent =
        !optimizer_owned_candidate ||
        (candidate.exact_collection_fallback ==
         (candidate.route_class ==
          ModelFamilyAlternativeRouteClassV1::kExactCollectionFallback));
    const bool inventory_receipt_consistent =
        !optimizer_owned_candidate ||
        (CanonicalUuid(candidate.candidate_inventory_receipt_uuid) &&
         (optimizer_inventory_receipt_uuid.empty() ||
          optimizer_inventory_receipt_uuid ==
              candidate.candidate_inventory_receipt_uuid));
    if (!CanonicalUuid(candidate.alternative_uuid) ||
        !alternative_ids.insert(candidate.alternative_uuid).second ||
        !CanonicalUuid(candidate.provider_uuid) ||
        !CanonicalUuid(candidate.capability_uuid) ||
        !CanonicalUuid(candidate.cost.cost_vector_uuid) ||
        candidate.provider_generation == 0 || !candidate.engine_owned ||
        !candidate.local_scope || candidate.parser_planning_authority_claimed ||
        candidate.transaction_finality_authority_claimed ||
        candidate.implementation_id != expected_implementation ||
        !route_class_consistent || !inventory_receipt_consistent ||
        (optimizer_owned_candidate &&
         (!CanonicalUuid(candidate.cost.provenance_uuid) ||
          !CanonicalUuid(candidate.cost.property_snapshot_uuid) ||
          !CanonicalUuid(candidate.cost.calibration_profile_uuid) ||
          candidate.cost.provenance_generation == 0 ||
          candidate.cost.confidence_basis_points == 0 ||
          candidate.cost.confidence_basis_points > 10'000)) ||
        !CandidateScore(candidate, &score)) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "model-family candidate domain is incomplete or duplicated");
    }
    if (optimizer_owned_candidate && optimizer_inventory_receipt_uuid.empty()) {
      optimizer_inventory_receipt_uuid =
          candidate.candidate_inventory_receipt_uuid;
    }
    fallback_seen = fallback_seen || candidate.exact_collection_fallback;
    if (!candidate.available || !candidate.exact ||
        !candidate.residual_recheck_required ||
        !candidate.base_row_mga_recheck_required ||
        !candidate.security_recheck_required) {
      continue;
    }
    if (candidate.cost.memory_bytes_required > request.memory_budget_bytes) {
      memory_refusal_observed = true;
      continue;
    }
    if (selected == nullptr || score < selected_score ||
        (score == selected_score &&
         candidate.alternative_uuid < selected->alternative_uuid)) {
      selected = &candidate;
      selected_score = score;
    }
  }
  if (selected == nullptr) {
    if ((key_value_family || time_series_family || vector_family ||
         search_family || spatial_family || columnar_family)
            ? memory_refusal_observed
            : fallback_seen) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "no exact model-family candidate fits the memory grant");
    }
    return refuse(graph_family
                      ? "SB_MODEL_GRAPH_EXACT_FALLBACK_UNAVAILABLE_V1"
                      : key_value_family
                            ? "SB_MODEL_KEY_VALUE_EXACT_FALLBACK_UNAVAILABLE_V1"
                            : time_series_family
                                  ? "SB_MODEL_TIME_SERIES_EXACT_FALLBACK_UNAVAILABLE_V1"
                            : vector_family
                                  ? "SB_MODEL_VECTOR_EXACT_FALLBACK_UNAVAILABLE_V1"
                            : search_family
                                  ? "SB_MODEL_SEARCH_EXACT_FALLBACK_UNAVAILABLE_V1"
                            : spatial_family
                                  ? "SB_MODEL_SPATIAL_EXACT_FALLBACK_UNAVAILABLE_V1"
                            : columnar_family
                                  ? "SB_MODEL_COLUMNAR_EXACT_FALLBACK_UNAVAILABLE_V1"
                            : "SB_MODEL_DOCUMENT_EXACT_FALLBACK_UNAVAILABLE_V1",
                  "no exact model-family provider or fallback is available");
  }

  using namespace scratchbird::engine::executor;
  TypedPhysicalNodeDag dag;
  dag.abi_version = 1;
  dag.selected_plan_uuid = DerivedUuid(
      request.bound_sblr_tree_uuid + "|" + selected->alternative_uuid);
  dag.root_physical_node_id = request.logical_node_id;
  dag.local_transaction_id =
      request.mga_statement_context.owning_local_transaction_id;
  dag.statement_snapshot_id =
      request.mga_statement_context.visible_committed_high_watermark;
  dag.mga_statement_context = request.mga_statement_context;
  dag.admission_evidence = {
      {PhysicalAdmissionStage::kBoundRequest, request.bound_sblr_tree_uuid},
      {PhysicalAdmissionStage::kCatalogEpoch, request.catalog_epoch_uuid},
      {PhysicalAdmissionStage::kSecurity, request.security_context_uuid},
      {PhysicalAdmissionStage::kMgaStatementBoundary,
       request.mga_statement_context.statement_snapshot_uuid},
      {PhysicalAdmissionStage::kPolicyCapability,
       request.capability_snapshot_uuid},
      {PhysicalAdmissionStage::kResource, request.resource_snapshot_uuid},
      {PhysicalAdmissionStage::kStatisticsProvenance,
       request.statistics_snapshot_uuid},
      {PhysicalAdmissionStage::kCanonicalRoute, request.route_snapshot_uuid},
  };
  PhysicalNodeRecord node;
  node.physical_node_id = request.logical_node_id;
  node.relational_node_id = request.logical_node_id;
  node.node_kind = PhysicalNodeKind::kScan;
  node.implementation_id = selected->implementation_id;
  node.output_descriptor_ids = request.output_descriptor_ids;
  node.causal_counter_id = 1;
  node.selected_alternative_uuid = selected->alternative_uuid;
  node.executor_capability_uuid = selected->capability_uuid;
  node.executor_capability_abi_version = 1;
  node.cost_vector_uuid = selected->cost.cost_vector_uuid;
  node.memory_bytes_required = selected->cost.memory_bytes_required;
  node.engine_capability_validated = true;
  node.mga_statement_context = request.mga_statement_context;
  node.logical_semantic_variant_id =
      graph_family ? "logical_graph_source_v1"
                   : key_value_family ? "logical_key_value_source_v1"
                   : time_series_family ? "logical_time_series_source_v1"
                   : vector_family ? "logical_vector_source_v1"
                   : search_family ? "logical_search_source_v1"
                   : spatial_family ? "logical_spatial_source_v1"
                   : columnar_family ? "logical_columnar_source_v1"
                                      : "logical_document_source_v1";
  RetainModelFamilyCostVector(selected->cost, &node.retained_cost);
  dag.nodes.push_back(std::move(node));
  dag.bound_sblr_tree_uuid = request.bound_sblr_tree_uuid;
  dag.catalog_epoch_uuid = request.catalog_epoch_uuid;
  dag.security_context_uuid = request.security_context_uuid;
  dag.capability_snapshot_uuid = request.capability_snapshot_uuid;
  dag.resource_snapshot_uuid = request.resource_snapshot_uuid;
  dag.statistics_snapshot_uuid = request.statistics_snapshot_uuid;
  dag.route_snapshot_uuid = request.route_snapshot_uuid;
  dag.catalog_generation = request.catalog_generation;
  dag.security_epoch = request.security_epoch;
  dag.policy_epoch = request.policy_epoch;
  dag.resource_epoch = request.resource_epoch;
  dag.statistics_generation = request.statistics_generation;
  dag.route_epoch = request.route_epoch;
  dag.route_generation = request.route_generation;
  dag.memory_budget_bytes = request.memory_budget_bytes;
  dag.optimizer_published = true;
  dag.immutable_node_identity_validated = true;
  dag.capability_validated_before_access = true;

  const auto validation = ValidateTypedPhysicalNodeDag(dag);
  if (!validation.accepted) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "model-family physical DAG failed canonical ABI validation");
  }
  result.accepted = true;
  result.selected = true;
  result.data_access_allowed = true;
  result.deterministic = true;
  result.exact_fallback_selected = selected->exact_collection_fallback;
  result.optimizer_owned_enumeration =
      !selected->candidate_inventory_receipt_uuid.empty();
  result.selected_candidate = *selected;
  result.physical_dag = std::move(dag);
  result.candidate_inventory_receipt_uuid =
      selected->candidate_inventory_receipt_uuid;
  result.selected_cost_explain_json =
      SerializeModelFamilyCostVectorToJsonV1(selected->cost);
  return result;
}

ModelFamilyCoordinatorResultV1 CoordinateDocumentFamilySourceV1(
    const ModelFamilyCoordinatorRequestV1& request) {
  return CoordinateModelFamilySourceV1(request);
}

ModelFamilyCoordinatorResultV1 CoordinateKeyValueFamilySourceV1(
    const ModelFamilyCoordinatorRequestV1& request) {
  return CoordinateModelFamilySourceV1(request);
}

MultilegDescriptorAllocationResultV1 AllocateMultilegResultDescriptorsV1(
    const std::vector<MultilegDescriptorProfileV1>& profiles,
    const std::vector<MultilegDescriptorDemandV1>& demands) {
  MultilegDescriptorAllocationResultV1 result;
  const auto refuse = [&](std::string diagnostic, std::string detail) {
    result.allocations.clear();
    result.diagnostic_id = std::move(diagnostic);
    result.detail = std::move(detail);
    return result;
  };
  if (profiles.size() != 320) {
    return refuse("SB_MODEL_RESULT_DESCRIPTOR_COHORT_INVALID_V1",
                  "multileg descriptor suffix must contain exactly 320 profiles");
  }
  std::array<std::string, 24> kind_type_uuids;
  std::set<std::string> descriptor_uuids;
  for (std::size_t ordinal = 0; ordinal < profiles.size(); ++ordinal) {
    const auto& profile = profiles[ordinal];
    const auto expected_kind = static_cast<std::uint8_t>(14 + ordinal / 32);
    const auto expected_slot = static_cast<std::uint16_t>(ordinal % 32);
    const bool expected_nullable = expected_kind % 2 == 1;
    if (profile.profile_kind != expected_kind ||
        profile.slot != expected_slot ||
        profile.nullable != expected_nullable ||
        !CanonicalUuid(profile.descriptor_uuid) ||
        profile.descriptor_uuid[14] != '7' ||
        !CanonicalUuid(profile.type_uuid) ||
        !descriptor_uuids.insert(profile.descriptor_uuid).second) {
      return refuse("SB_MODEL_RESULT_DESCRIPTOR_COHORT_INVALID_V1",
                    "multileg descriptor suffix identity or order drifted");
    }
    auto& type_uuid = kind_type_uuids[profile.profile_kind];
    if (type_uuid.empty()) {
      type_uuid = profile.type_uuid;
    } else if (type_uuid != profile.type_uuid) {
      return refuse("SB_MODEL_RESULT_DESCRIPTOR_COHORT_INVALID_V1",
                    "one multileg descriptor kind carries multiple type UUIDs");
    }
  }
  const std::array<std::uint8_t, 5> nonnull_kinds = {14, 16, 18, 20, 22};
  std::set<std::string> distinct_type_uuids;
  for (const auto kind : nonnull_kinds) {
    if (kind_type_uuids[kind].empty() ||
        kind_type_uuids[kind] != kind_type_uuids[kind + 1] ||
        !distinct_type_uuids.insert(kind_type_uuids[kind]).second) {
      return refuse("SB_MODEL_RESULT_DESCRIPTOR_COHORT_INVALID_V1",
                    "multileg type pairs are absent, mismatched, or aliased");
    }
  }
  for (const auto& descriptor_uuid : descriptor_uuids) {
    if (distinct_type_uuids.contains(descriptor_uuid)) {
      return refuse(
          "SB_MODEL_RESULT_DESCRIPTOR_COHORT_INVALID_V1",
          "multileg descriptor and type identity domains are not independent");
    }
  }

  std::vector<MultilegDescriptorDemandV1> ordered = demands;
  std::sort(ordered.begin(), ordered.end(), [](const auto& left,
                                                const auto& right) {
    return std::tie(left.lexical_source_ordinal, left.field_ordinal) <
           std::tie(right.lexical_source_ordinal, right.field_ordinal);
  });
  for (std::size_t index = 0; index < ordered.size(); ++index) {
    if (ordered[index].lexical_source_ordinal !=
            demands[index].lexical_source_ordinal ||
        ordered[index].field_ordinal != demands[index].field_ordinal) {
      return refuse("SB_MODEL_RESULT_DESCRIPTOR_DEMAND_ORDER_INVALID_V1",
                    "descriptor demand is not in lexical source/field order");
    }
  }
  std::array<std::uint16_t, 24> next_slots{};
  std::set<std::pair<std::uint16_t, std::uint16_t>> demand_ordinals;
  for (const auto& demand : demands) {
    if (demand.family_id.empty() || demand.field_id.empty() ||
        !demand_ordinals
             .insert({demand.lexical_source_ordinal, demand.field_ordinal})
             .second) {
      return refuse("SB_MODEL_RESULT_DESCRIPTOR_DEMAND_ORDER_INVALID_V1",
                    "descriptor demand identity is absent or duplicated");
    }
    MultilegDescriptorAllocationV1 allocation;
    allocation.demand = demand;
    if (!demand.derived) {
      if (!CanonicalUuid(demand.persisted_descriptor_uuid) ||
          !CanonicalUuid(demand.persisted_type_uuid) ||
          demand.persisted_descriptor_uuid == demand.persisted_type_uuid) {
        return refuse("SB_MODEL_RESULT_DESCRIPTOR_DEMAND_INVALID_V1",
                      "persisted source field lacks catalog descriptor identity");
      }
      allocation.descriptor_uuid = demand.persisted_descriptor_uuid;
      allocation.type_uuid = demand.persisted_type_uuid;
      result.allocations.push_back(std::move(allocation));
      continue;
    }
    std::uint8_t kind = 0;
    if (demand.canonical_type_name == "uuid") {
      kind = demand.nullable ? 15 : 14;
    } else if (demand.canonical_type_name == "uint64") {
      kind = demand.nullable ? 17 : 16;
    } else if (demand.canonical_type_name == "real64") {
      kind = demand.nullable ? 19 : 18;
    } else if (demand.canonical_type_name == "boolean") {
      kind = demand.nullable ? 21 : 20;
    } else if (demand.canonical_type_name == "geometry") {
      kind = demand.nullable ? 23 : 22;
    } else {
      return refuse("SB_MODEL_RESULT_DESCRIPTOR_DEMAND_INVALID_V1",
                    "derived multileg field requests an unapproved type");
    }
    if (next_slots[kind] >= 32) {
      return refuse("SB_MODEL_RESULT_DESCRIPTOR_POOL_EXHAUSTED_V1",
                    "a multileg descriptor pool requires slot 32");
    }
    const auto ordinal = static_cast<std::size_t>(kind - 14) * 32 +
                         next_slots[kind]++;
    const auto& profile = profiles[ordinal];
    allocation.descriptor_uuid = profile.descriptor_uuid;
    allocation.type_uuid = profile.type_uuid;
    allocation.profile_kind = profile.profile_kind;
    allocation.slot = profile.slot;
    result.allocations.push_back(std::move(allocation));
  }
  std::set<std::string> allocated_descriptor_uuids;
  std::set<std::string> allocated_type_uuids;
  for (const auto& allocation : result.allocations) {
    allocated_descriptor_uuids.insert(allocation.descriptor_uuid);
    allocated_type_uuids.insert(allocation.type_uuid);
  }
  for (const auto& descriptor_uuid : allocated_descriptor_uuids) {
    if (allocated_type_uuids.contains(descriptor_uuid)) {
      return refuse(
          "SB_MODEL_RESULT_DESCRIPTOR_DEMAND_INVALID_V1",
          "allocated descriptor and type identity domains are not independent");
    }
  }
  result.accepted = true;
  result.preflight_complete = true;
  result.diagnostic_id = "SB_EXECUTOR_OK";
  return result;
}

MultilegDescriptorDispatchScopeV1::MultilegDescriptorDispatchScopeV1(
    const std::string& statement_uuid,
    const std::vector<MultilegDescriptorProfileV1>& profiles)
    : statement_uuid_(statement_uuid) {
  if (!CanonicalUuid(statement_uuid)) {
    diagnostic_id_ = "SB_MODEL_RESULT_DESCRIPTOR_SCOPE_INVALID_V1";
    detail_ = "multileg descriptor dispatch statement UUID is invalid";
    return;
  }
  if (g_active_multileg_descriptor_dispatch_v1.has_value()) {
    diagnostic_id_ = "SB_MODEL_RESULT_DESCRIPTOR_SCOPE_NESTED_V1";
    detail_ = g_active_multileg_descriptor_dispatch_v1->statement_uuid ==
                      statement_uuid
                  ? "multileg descriptor dispatch scope was nested"
                  : "a different statement owns the active multileg descriptor dispatch scope";
    return;
  }
  const auto validated = AllocateMultilegResultDescriptorsV1(profiles, {});
  if (!validated.accepted || !validated.preflight_complete) {
    diagnostic_id_ = validated.diagnostic_id;
    detail_ = validated.detail;
    return;
  }
  g_active_multileg_descriptor_dispatch_v1 =
      ActiveMultilegDescriptorDispatchV1{statement_uuid, profiles};
  installed_ = true;
  diagnostic_id_ = "SB_EXECUTOR_OK";
}

MultilegDescriptorDispatchScopeV1::~MultilegDescriptorDispatchScopeV1() {
  if (!installed_) return;
  if (g_active_multileg_descriptor_dispatch_v1.has_value() &&
      g_active_multileg_descriptor_dispatch_v1->statement_uuid ==
          statement_uuid_) {
    g_active_multileg_descriptor_dispatch_v1.reset();
  }
}

MultilegDescriptorDispatchLookupV1 LookupMultilegDescriptorDispatchScopeV1(
    const std::string& exact_statement_uuid) {
  MultilegDescriptorDispatchLookupV1 result;
  if (!CanonicalUuid(exact_statement_uuid)) {
    result.diagnostic_id = "SB_MODEL_RESULT_DESCRIPTOR_SCOPE_INVALID_V1";
    result.detail = "multileg descriptor lookup statement UUID is invalid";
    return result;
  }
  if (!g_active_multileg_descriptor_dispatch_v1.has_value()) {
    result.diagnostic_id = "SB_MODEL_RESULT_DESCRIPTOR_SCOPE_REQUIRED_V1";
    result.detail = "no engine-owned V10 descriptor dispatch scope is active";
    return result;
  }
  if (g_active_multileg_descriptor_dispatch_v1->statement_uuid !=
      exact_statement_uuid) {
    result.diagnostic_id = "SB_MODEL_RESULT_DESCRIPTOR_SCOPE_MISMATCH_V1";
    result.detail = "active V10 descriptor dispatch scope belongs to another statement";
    return result;
  }
  result.accepted = true;
  result.profiles = g_active_multileg_descriptor_dispatch_v1->profiles;
  result.diagnostic_id = "SB_EXECUTOR_OK";
  return result;
}

ModelFamilyCompositionResultV1 CoordinateModelFamilyCompositionV1(
    const ModelFamilyCompositionRequestV1& request) {
  ModelFamilyCompositionResultV1 result;
  const auto refuse = [&](std::string diagnostic, std::string detail) {
    result.deterministic = true;
    result.root_publication_allowed = false;
    result.no_partial_root = true;
    result.lexical_legs.clear();
    result.composition_receipt_uuid.clear();
    result.diagnostic_id = std::move(diagnostic);
    result.detail = std::move(detail);
    return result;
  };
  const std::map<std::string, std::size_t> profile_arities = {
      {"COMP-3-LINEAR-V1", 3},
      {"COMP-3-FANIN-V1", 3},
      {"COMP-3-INDEPENDENT-V1", 3},
      {"COMP-4-MIXED-V1", 4},
      {"COMP-3-LATERAL-V1", 3},
      {"COMP-3-SHARED-LEG-V1", 3},
      {"COMP-3-SHORT-CIRCUIT-V1", 3},
      {"COMP-3-CANCEL-FANOUT-V1", 3},
      {"COMP-3-FAILURE-CLEANUP-V1", 3},
      {"COMP-9-FULL-UNIVERSE-V1", 9},
  };
  const std::set<std::string> family_ids = {
      "relational", "document", "graph", "key_value", "time_series",
      "vector", "search", "spatial", "columnar"};
  const auto profile = profile_arities.find(request.composition_profile_id);
  if (request.abi_version != 1 || profile == profile_arities.end() ||
      request.legs.size() != profile->second || request.memory_budget_bytes == 0) {
    return refuse("SB_MODEL_COMPOSITION_PROFILE_REFUSED_V1",
                  "composition profile or arity is not one of the ten signed forms");
  }
  result.lexical_legs = request.legs;
  std::sort(result.lexical_legs.begin(), result.lexical_legs.end(),
            [](const auto& left, const auto& right) {
              return left.lexical_source_ordinal < right.lexical_source_ordinal;
            });
  for (std::size_t index = 0; index < result.lexical_legs.size(); ++index) {
    if (result.lexical_legs[index].lexical_source_ordinal !=
        request.legs[index].lexical_source_ordinal) {
      return refuse("SB_MODEL_COMPOSITION_PROFILE_REFUSED_V1",
                    "composition legs are not in lexical source order");
    }
  }
  std::set<std::uint16_t> source_ordinals;
  const auto& statement_context = request.legs.front().mga_statement_context;
  std::string receipt_seed = request.composition_profile_id;
  for (const auto& leg : request.legs) {
    if (!family_ids.contains(leg.family_id) ||
        !source_ordinals.insert(leg.lexical_source_ordinal).second ||
        !CanonicalUuid(leg.selected_plan_uuid) || leg.root_physical_node_id == 0 ||
        !leg.selected || !leg.exact_recheck_required ||
        !leg.security_recheck_required ||
        leg.parser_planning_authority_claimed ||
        leg.transaction_finality_authority_claimed ||
        !scratchbird::engine::executor::PhysicalMgaStatementContextValid(
            leg.mga_statement_context) ||
        !scratchbird::engine::executor::PhysicalMgaStatementContextEqual(
            statement_context, leg.mga_statement_context)) {
      return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                    "composition leg is invalid or does not share one statement context");
    }
    receipt_seed += "|" + leg.family_id + "|" + leg.selected_plan_uuid;
  }
  if (request.composition_profile_id == "COMP-4-MIXED-V1") {
    const std::array<std::string, 4> expected = {
        "relational", "document", "graph", "vector"};
    for (std::size_t i = 0; i < expected.size(); ++i) {
      if (request.legs[i].family_id != expected[i]) {
        return refuse("SB_MODEL_COMPOSITION_PROFILE_REFUSED_V1",
                      "COMP-4-MIXED family order drifted");
      }
    }
  }
  if (request.composition_profile_id == "COMP-3-LATERAL-V1" &&
      request.legs.front().family_id != "relational") {
    return refuse("SB_MODEL_COMPOSITION_PROFILE_REFUSED_V1",
                  "COMP-3-LATERAL requires relational as the lexical outer leg");
  }
  if (request.composition_profile_id == "COMP-9-FULL-UNIVERSE-V1") {
    const std::array<std::string, 9> expected = {
        "relational", "document", "graph", "key_value", "time_series",
        "vector", "search", "spatial", "columnar"};
    for (std::size_t i = 0; i < expected.size(); ++i) {
      if (request.legs[i].family_id != expected[i]) {
        return refuse("SB_MODEL_COMPOSITION_PROFILE_REFUSED_V1",
                      "COMP-9-FULL-UNIVERSE family order drifted");
      }
    }
  }
  static const std::map<std::string, std::string> lifecycle_contracts = {
      {"COMP-3-LINEAR-V1", "linear_dependency_cleanup_once.v1"},
      {"COMP-3-FANIN-V1", "fanin_sibling_cancel_cleanup_once.v1"},
      {"COMP-3-INDEPENDENT-V1", "independent_bag_cleanup_once.v1"},
      {"COMP-4-MIXED-V1", "mixed_four_leg_no_partial_root.v1"},
      {"COMP-3-LATERAL-V1", "lateral_visible_outer_invocations.v1"},
      {"COMP-3-SHARED-LEG-V1", "shared_leg_once_fanout_twice.v1"},
      {"COMP-3-SHORT-CIRCUIT-V1", "short_circuit_empty_root_started_only_cleanup.v1"},
      {"COMP-3-CANCEL-FANOUT-V1", "cancel_all_started_no_partial_root_cleanup_once.v1"},
      {"COMP-3-FAILURE-CLEANUP-V1", "failed_and_cancelled_cleanup_once_no_partial_root.v1"},
      {"COMP-9-FULL-UNIVERSE-V1", "strict_nine_leg_left_deep_cleanup_once.v1"},
  };
  result.lifecycle_contract_id =
      lifecycle_contracts.at(request.composition_profile_id);
  if (request.composition_profile_id == "COMP-3-CANCEL-FANOUT-V1") {
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "all admitted composition legs are cancelled before root publication");
  }
  if (request.composition_profile_id == "COMP-3-FAILURE-CLEANUP-V1") {
    return refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
                  "the named failed leg blocks dependents and partial root publication");
  }
  result.accepted = true;
  result.deterministic = true;
  result.root_publication_allowed = true;
  result.no_partial_root = true;
  result.empty_root_required =
      request.composition_profile_id == "COMP-3-SHORT-CIRCUIT-V1";
  result.composition_receipt_uuid = DerivedUuid(receipt_seed);
  result.diagnostic_id = "SB_EXECUTOR_OK";
  return result;
}

static ModelFamilyDependencyCoordinatorResultV1
CoordinateModelFamilyDependencyDagValidatedV1(
    const ModelFamilyDependencyCoordinatorRequestV1& request) {
  // QOW-SOURCE-RCP-080-COMPLETE-DEPENDENCY-COORDINATOR-V1
  ModelFamilyDependencyCoordinatorResultV1 result;
  const auto refuse = [&](std::string diagnostic, std::string detail) {
    result.accepted = false;
    result.deterministic = true;
    result.data_access_allowed = false;
    result.root_publication_candidate = false;
    result.no_partial_root = true;
    result.stable_schedule.clear();
    result.parallel_waves.clear();
    result.dependency_edges.clear();
    result.relational_consumers.clear();
    result.rule_receipts.clear();
    result.dependency_dag_receipt_uuid.clear();
    result.composition_admission_receipt_uuid.clear();
    result.diagnostic_id = std::move(diagnostic);
    result.detail = std::move(detail);
    return result;
  };
  const std::map<std::string, std::size_t> profile_arities = {
      {"COMP-3-LINEAR-V1", 3},
      {"COMP-3-FANIN-V1", 3},
      {"COMP-3-INDEPENDENT-V1", 3},
      {"COMP-4-MIXED-V1", 4},
      {"COMP-3-LATERAL-V1", 3},
      {"COMP-3-SHARED-LEG-V1", 3},
      {"COMP-3-SHORT-CIRCUIT-V1", 3},
      {"COMP-3-CANCEL-FANOUT-V1", 3},
      {"COMP-3-FAILURE-CLEANUP-V1", 3},
      {"COMP-9-FULL-UNIVERSE-V1", 9},
  };
  const std::set<std::string> family_ids = {
      "relational", "document", "graph", "key_value", "time_series",
      "vector", "search", "spatial", "columnar"};
  const auto profile = profile_arities.find(request.composition_profile_id);
  if (request.abi_version != 1 || profile == profile_arities.end() ||
      request.legs.size() != profile->second ||
      !CanonicalUuid(request.bound_sblr_tree_uuid) ||
      request.selected_plan_generation == 0 ||
      !CanonicalUuid(request.canonical_root_physical_node_uuid) ||
      request.canonical_root_physical_node_id == 0) {
    return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                  "dependency coordinator profile, arity, or bound plan identity is invalid");
  }
  if (request.selected_plan_generation !=
      request.current_selected_plan_generation) {
    return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                  "selected dependency plan generation is stale");
  }

  using EdgePair = std::pair<std::uint16_t, std::uint16_t>;
  std::vector<EdgePair> expected_edges;
  if (request.composition_profile_id == "COMP-3-LINEAR-V1" ||
      request.composition_profile_id == "COMP-3-LATERAL-V1" ||
      request.composition_profile_id == "COMP-3-SHORT-CIRCUIT-V1") {
    expected_edges = {{0, 1}, {1, 2}};
  } else if (request.composition_profile_id == "COMP-3-FANIN-V1" ||
             request.composition_profile_id ==
                 "COMP-3-FAILURE-CLEANUP-V1") {
    expected_edges = {{0, 2}, {1, 2}};
  } else if (request.composition_profile_id == "COMP-4-MIXED-V1") {
    expected_edges = {{0, 1}, {1, 2}, {0, 3}};
  } else if (request.composition_profile_id == "COMP-3-SHARED-LEG-V1") {
    expected_edges = {{0, 1}, {0, 2}};
  } else if (request.composition_profile_id ==
             "COMP-9-FULL-UNIVERSE-V1") {
    for (std::uint16_t ordinal = 0; ordinal < 8; ++ordinal) {
      expected_edges.emplace_back(ordinal, ordinal + 1);
    }
  }

  const auto require_exact_family_order = [&](const auto& expected) {
    if (request.legs.size() != expected.size()) return false;
    for (std::size_t index = 0; index < expected.size(); ++index) {
      if (request.legs[index].family_id != expected[index]) return false;
    }
    return true;
  };
  if (request.composition_profile_id == "COMP-4-MIXED-V1" &&
      !require_exact_family_order(std::array<std::string_view, 4>{
          "relational", "document", "graph", "vector"})) {
    return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                  "COMP-4-MIXED dependency family order drifted");
  }
  if (request.composition_profile_id == "COMP-3-LATERAL-V1" &&
      request.legs.front().family_id != "relational") {
    return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                  "COMP-3-LATERAL dependency outer leg is not relational");
  }
  if (request.composition_profile_id == "COMP-9-FULL-UNIVERSE-V1" &&
      !require_exact_family_order(std::array<std::string_view, 9>{
          "relational", "document", "graph", "key_value", "time_series",
          "vector", "search", "spatial", "columnar"})) {
    return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                  "COMP-9-FULL-UNIVERSE dependency family order drifted");
  }

  std::set<std::string> node_uuids;
  std::set<std::uint64_t> root_node_ids;
  std::set<std::uint32_t> result_descriptor_ids;
  const auto& statement_context = request.legs.front().mga_statement_context;
  for (std::size_t index = 0; index < request.legs.size(); ++index) {
    const auto& leg = request.legs[index];
    if (leg.abi_version != 1 || leg.lexical_source_ordinal != index ||
        !family_ids.contains(leg.family_id) ||
        !DependencyOperationValid(leg.family_id, leg.operation_ids,
                                  leg.operation_id) ||
        !CanonicalUuid(leg.physical_node_uuid) ||
        !node_uuids.insert(leg.physical_node_uuid).second ||
        !CanonicalUuid(leg.selected_plan_uuid) ||
        !CanonicalUuid(leg.selected_alternative_uuid) ||
        !CanonicalUuid(leg.provider_uuid) ||
        !CanonicalUuid(leg.capability_uuid) ||
        !CanonicalUuid(leg.delivered_property_uuid) ||
        !CanonicalUuid(leg.bound_object_uuid) ||
        !CanonicalUuid(leg.catalog_snapshot_uuid) ||
        !CanonicalUuid(leg.descriptor_snapshot_uuid) ||
        !CanonicalUuid(leg.security_context_uuid) ||
        !CanonicalUuid(leg.policy_snapshot_uuid) ||
        !CanonicalUuid(leg.resource_contract_uuid) ||
        !CanonicalUuid(leg.family_local_cost.cost_vector_uuid) ||
        !CanonicalUuid(leg.family_local_cost.provenance_uuid) ||
        leg.family_local_cost.provenance_generation == 0 ||
        leg.family_local_cost.confidence_basis_points == 0 ||
        leg.family_local_cost.confidence_basis_points > 10'000 ||
        leg.root_physical_node_id == 0 ||
        !root_node_ids.insert(leg.root_physical_node_id).second ||
        leg.output_descriptor_ids.empty() || !leg.selected ||
        leg.parser_planning_authority_claimed ||
        leg.transaction_finality_authority_claimed) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "a dependency leg lacks exact bound UUID, descriptor, or selected-plan identity");
    }
    for (const auto descriptor_id : leg.output_descriptor_ids) {
      if (descriptor_id == 0 ||
          !result_descriptor_ids.insert(descriptor_id).second) {
        return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                      "dependency output descriptors are absent or aliased across legs");
      }
    }
    if (leg.catalog_generation == 0 ||
        leg.catalog_generation != leg.current_catalog_generation ||
        leg.descriptor_generation == 0 ||
        leg.descriptor_generation != leg.current_descriptor_generation) {
      return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                    "a dependency leg catalog or descriptor generation is stale");
    }
    if (leg.security_generation == 0 ||
        leg.security_generation != leg.current_security_generation ||
        leg.policy_generation == 0 ||
        leg.policy_generation != leg.current_policy_generation) {
      return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                    "a dependency leg security or policy generation is stale");
    }
    if (!scratchbird::engine::executor::PhysicalMgaStatementContextValid(
            leg.mga_statement_context) ||
        !scratchbird::engine::executor::PhysicalMgaStatementContextEqual(
            statement_context, leg.mga_statement_context)) {
      return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                    "dependency legs do not retain one immutable engine statement context");
    }
    if (!leg.capability_admitted || !leg.cancellation_supported ||
        !leg.cleanup_supported || leg.provider_generation == 0 ||
        leg.provider_generation != leg.current_provider_generation) {
      return refuse("SB_MODEL_CAPABILITY_UNAVAILABLE_V1",
                    "a dependency leg capability, generation, cancellation, or cleanup contract is unavailable");
    }
    if (!leg.exact ||
        (leg.exact_fallback_selected && !leg.exact_fallback_available)) {
      return refuse("SB_MODEL_CANDIDATE_SEMANTICS_MISSING_V1",
                    "a dependency leg has no selected exact family-local alternative");
    }
    if (!leg.exact_recheck_required ||
        !leg.base_row_mga_recheck_required ||
        !leg.security_recheck_required) {
      return refuse("SB_MODEL_PROPERTY_UNSATISFIED_V1",
                    "a dependency leg omits exact, MGA, or security recheck properties");
    }
    if (leg.resource_generation == 0 ||
        leg.resource_generation != leg.current_resource_generation ||
        leg.memory_grant_bytes == 0 || leg.exchange_buffer_bytes == 0) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "a dependency leg lacks a current bounded memory or exchange grant");
    }
    if (leg.maximum_rows == 0 || leg.maximum_columns == 0 ||
        leg.maximum_cells == 0 ||
        leg.output_descriptor_ids.size() > leg.maximum_columns) {
      return refuse("SB_MODEL_RESOURCE_ROW_LIMIT_V1",
                    "a dependency leg lacks nonzero row, column, or cell bounds");
    }
    if (leg.cluster_scope_required && !request.cluster_capability_available) {
      return refuse("SB_MODEL_CLUSTER_CAPABILITY_UNAVAILABLE_V1",
                    "a selected dependency leg requires unavailable cluster scope");
    }
    if (!leg.local_scope && !leg.cluster_scope_required) {
      return refuse("SB_MODEL_CLUSTER_CAPABILITY_UNAVAILABLE_V1",
                    "a non-local dependency leg has no explicit admitted cluster scope");
    }
  }

  const std::size_t expected_consumer_count =
      request.composition_profile_id == "COMP-3-SHARED-LEG-V1"
          ? request.legs.size()
          : request.legs.size() - 1;
  if (request.relational_consumers.size() != expected_consumer_count) {
    return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                  "typed relational consumer cardinality differs from the signed composition profile");
  }
  std::map<std::string, std::vector<std::uint32_t>> node_descriptors;
  std::map<std::string, std::uint64_t> node_parent_counts;
  for (const auto& leg : request.legs) {
    node_descriptors.emplace(leg.physical_node_uuid,
                             leg.output_descriptor_ids);
    node_parent_counts.emplace(leg.physical_node_uuid, 0);
  }
  const std::set<std::string> join_forms = {
      "INNER", "LEFT", "RIGHT", "FULL", "SEMI", "ANTI", "CROSS",
      "LATERAL_INNER", "LATERAL_LEFT", "ASOF"};
  std::size_t canonical_root_count = 0;
  for (const auto& consumer : request.relational_consumers) {
    if (consumer.abi_version != 1 ||
        !CanonicalUuid(consumer.physical_node_uuid) ||
        !node_uuids.insert(consumer.physical_node_uuid).second ||
        consumer.physical_node_id == 0 ||
        consumer.causal_counter_id == 0 ||
        !root_node_ids.insert(consumer.physical_node_id).second ||
        !CanonicalUuid(consumer.selected_implementation_uuid) ||
        !CanonicalUuid(consumer.expected_security_receipt_uuid) ||
        !join_forms.contains(consumer.join_form_id) ||
        consumer.input_physical_node_uuids.size() != 2 ||
        consumer.input_physical_node_uuids[0] ==
            consumer.input_physical_node_uuids[1] ||
        consumer.input_descriptor_ids.empty() ||
        consumer.output_descriptor_ids.empty() ||
        consumer.maximum_rows == 0 || consumer.maximum_columns == 0 ||
        consumer.maximum_cells == 0 || consumer.memory_grant_bytes == 0 ||
        consumer.output_descriptor_ids.size() > consumer.maximum_columns ||
        !consumer.exact || !consumer.cleanup_supported ||
        !consumer.cancellation_supported ||
        consumer.parser_execution_authority_claimed ||
        consumer.transaction_finality_authority_claimed ||
        !scratchbird::engine::executor::PhysicalMgaStatementContextValid(
            consumer.mga_statement_context) ||
        !scratchbird::engine::executor::PhysicalMgaStatementContextEqual(
            statement_context, consumer.mga_statement_context)) {
      return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                    "a typed relational consumer is incomplete or authority-overclaiming");
    }
    node_parent_counts.emplace(consumer.physical_node_uuid, 0);
    canonical_root_count += consumer.canonical_root ? 1 : 0;
    if (consumer.canonical_root &&
        (consumer.physical_node_uuid !=
             request.canonical_root_physical_node_uuid ||
         consumer.physical_node_id !=
             request.canonical_root_physical_node_id)) {
      return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                    "typed relational root identity differs from the selected canonical root");
    }
  }
  if (canonical_root_count != 1) {
    return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                  "typed relational DAG does not have exactly one canonical root");
  }
  std::vector<bool> consumer_scheduled(request.relational_consumers.size(),
                                       false);
  for (std::size_t completed = 0;
       completed < request.relational_consumers.size();) {
    std::vector<std::size_t> ready_consumers;
    for (std::size_t index = 0;
         index < request.relational_consumers.size(); ++index) {
      if (consumer_scheduled[index]) continue;
      const auto& consumer = request.relational_consumers[index];
      if (node_descriptors.contains(consumer.input_physical_node_uuids[0]) &&
          node_descriptors.contains(consumer.input_physical_node_uuids[1])) {
        ready_consumers.push_back(index);
      }
    }
    if (ready_consumers.empty()) {
      return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                    "typed relational consumer graph is cyclic or has an unresolved input");
    }
    std::sort(ready_consumers.begin(), ready_consumers.end(),
              [&](const auto left, const auto right) {
                return request.relational_consumers[left].physical_node_uuid <
                       request.relational_consumers[right].physical_node_uuid;
              });
    for (const auto index : ready_consumers) {
      const auto& consumer = request.relational_consumers[index];
      std::vector<std::uint32_t> expected_input_descriptors;
      for (const auto& input_uuid : consumer.input_physical_node_uuids) {
        const auto& descriptors = node_descriptors.at(input_uuid);
        expected_input_descriptors.insert(expected_input_descriptors.end(),
                                          descriptors.begin(),
                                          descriptors.end());
        ++node_parent_counts[input_uuid];
      }
      auto expected_output_descriptors = expected_input_descriptors;
      if (request.composition_profile_id == "COMP-3-SHARED-LEG-V1" &&
          consumer.canonical_root) {
        std::set<std::uint32_t> seen;
        std::erase_if(expected_output_descriptors,
                      [&](const auto descriptor_id) {
                        return !seen.insert(descriptor_id).second;
                      });
      }
      if (consumer.input_descriptor_ids != expected_input_descriptors ||
          consumer.output_descriptor_ids != expected_output_descriptors) {
        return refuse("SB_MODEL_PROPERTY_UNSATISFIED_V1",
                      "typed relational consumer descriptor lineage was substituted");
      }
      node_descriptors.emplace(consumer.physical_node_uuid,
                               consumer.output_descriptor_ids);
      result.relational_consumers.push_back(consumer);
      consumer_scheduled[index] = true;
      ++completed;
    }
  }
  for (const auto& [node_uuid, parent_count] : node_parent_counts) {
    const bool canonical_root =
        node_uuid == request.canonical_root_physical_node_uuid;
    if ((canonical_root && parent_count != 0) ||
        (!canonical_root && parent_count == 0)) {
      return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                    "typed relational DAG has an orphan or more than one physical root");
    }
  }

  if (request.statement_memory_budget_bytes == 0) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "statement memory bound is invalid");
  }
  if (request.backpressure_low_watermark_rows == 0 ||
      request.backpressure_high_watermark_rows <=
          request.backpressure_low_watermark_rows) {
    return refuse("SB_MODEL_BACKPRESSURE_PROTOCOL_FAILED_V1",
                  "high/low backpressure bounds are invalid");
  }
  if (!request.signed_short_circuit_enabled) {
    return refuse("SB_MODEL_SHORT_CIRCUIT_STATE_INVALID_V1",
                  "the signed composition short-circuit rule is unavailable");
  }
  if (request.spill_required &&
      (request.maximum_spill_bytes == 0 ||
       !request.engine_temporary_storage_available ||
       !request.spill_cleanup_path_available ||
       std::ranges::none_of(request.legs,
                            [](const auto& leg) { return leg.spill_eligible; }))) {
    return refuse("SB_MODEL_RESOURCE_SPILL_REFUSED_V1",
                  "bounded engine temporary storage or its cleanup path is unavailable");
  }
  if (!request.spill_required && request.maximum_spill_bytes != 0) {
    return refuse("SB_MODEL_RESOURCE_SPILL_REFUSED_V1",
                  "a spill byte grant exists without a selected spill route");
  }
  const bool feedback_absent =
      !request.feedback_observation_frozen &&
      !request.feedback_target_is_later_plan &&
      request.feedback_observation_generation == 0 &&
      request.feedback_target_plan_generation == 0;
  const bool feedback_later =
      request.feedback_observation_frozen &&
      request.feedback_target_is_later_plan &&
      request.feedback_observation_generation ==
          request.selected_plan_generation &&
      request.feedback_target_plan_generation >
          request.feedback_observation_generation;
  if (request.current_plan_mutation_requested ||
      (!feedback_absent && !feedback_later)) {
    return refuse("SB_MODEL_FEEDBACK_CURRENT_PLAN_MUTATION_REFUSED_V1",
                  "runtime feedback attempted to mutate the current plan or cross a generation boundary");
  }

  std::set<std::string> edge_uuids;
  std::set<EdgePair> actual_edge_pairs;
  std::vector<std::vector<std::uint16_t>> outgoing(request.legs.size());
  std::vector<std::vector<std::uint16_t>> incoming(request.legs.size());
  std::vector<std::uint16_t> indegree(request.legs.size(), 0);
  if (request.edges.size() != expected_edges.size()) {
    return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                  "dependency edge cardinality differs from the signed composition profile");
  }
  for (const auto& edge : request.edges) {
    const EdgePair endpoints = {edge.producer_lexical_source_ordinal,
                                edge.consumer_lexical_source_ordinal};
    const bool lateral_correlation =
        request.composition_profile_id == "COMP-3-LATERAL-V1" &&
        endpoints == EdgePair{0, 1};
    if (edge.abi_version != 1 || !CanonicalUuid(edge.edge_uuid) ||
        !edge_uuids.insert(edge.edge_uuid).second ||
        endpoints.first >= request.legs.size() ||
        endpoints.second >= request.legs.size() ||
        endpoints.first == endpoints.second ||
        !actual_edge_pairs.insert(endpoints).second ||
        (edge.edge_kind != "data_binding" &&
         edge.edge_kind != "ordering" && edge.edge_kind != "correlation") ||
        (lateral_correlation ? edge.edge_kind != "correlation"
                             : edge.edge_kind == "correlation") ||
        !CanonicalUuid(edge.required_property_uuid) ||
        !CanonicalUuid(edge.delivered_property_uuid) ||
        !CanonicalUuid(edge.descriptor_lineage_uuid) ||
        edge.required_property_uuid != edge.delivered_property_uuid ||
        edge.delivered_property_uuid !=
            request.legs[endpoints.first].delivered_property_uuid ||
        edge.producer_output_descriptor_ids !=
            request.legs[endpoints.first].output_descriptor_ids ||
        edge.consumer_input_descriptor_ids !=
            edge.producer_output_descriptor_ids ||
        !edge.descriptor_compatible || !edge.semantics_authorized ||
        edge.parser_execution_authority_claimed ||
        edge.transaction_finality_authority_claimed) {
      return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                    "a dependency edge is missing, duplicated, incompatible, or authority-overclaiming");
    }
    outgoing[endpoints.first].push_back(endpoints.second);
    incoming[endpoints.second].push_back(endpoints.first);
    ++indegree[endpoints.second];
  }
  if (actual_edge_pairs !=
      std::set<EdgePair>(expected_edges.begin(), expected_edges.end())) {
    return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                  "dependency endpoints differ from the signed composition profile");
  }

  std::vector<bool> scheduled(request.legs.size(), false);
  std::uint32_t stable_start_ordinal = 0;
  std::uint64_t causal_counter = 0;
  std::uint64_t peak_memory = 0;
  std::string dag_seed = request.composition_profile_id + "|" +
                         request.bound_sblr_tree_uuid + "|" +
                         std::to_string(request.selected_plan_generation);
  const auto composition_admission_receipt_uuid = DerivedUuid(
      dag_seed + "|composition-admission.v1");
  while (result.stable_schedule.size() < request.legs.size()) {
    std::vector<std::uint16_t> ready;
    for (std::uint16_t ordinal = 0; ordinal < request.legs.size(); ++ordinal) {
      if (!scheduled[ordinal] && indegree[ordinal] == 0) ready.push_back(ordinal);
    }
    if (ready.empty()) {
      return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                    "dependency graph is cyclic");
    }
    std::sort(ready.begin(), ready.end(), [&](const auto left,
                                               const auto right) {
      return request.legs[left].physical_node_uuid <
             request.legs[right].physical_node_uuid;
    });
    if (ready.size() > 1 &&
        std::ranges::any_of(ready, [&](const auto ordinal) {
          return !request.legs[ordinal].parallel_eligible;
        })) {
      return refuse("SB_MODEL_PARALLEL_ADMISSION_REFUSED_V1",
                    "an independent schedule wave lacks explicit parallel eligibility");
    }
    std::uint64_t wave_memory = 0;
    for (const auto ordinal : ready) {
      const auto& leg = request.legs[ordinal];
      if (!Add(leg.memory_grant_bytes, &wave_memory) ||
          !Add(leg.exchange_buffer_bytes, &wave_memory)) {
        return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "parallel wave memory accounting overflowed");
      }
    }
    peak_memory = std::max(peak_memory, wave_memory);
    if (peak_memory > request.statement_memory_budget_bytes) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "aggregate admitted wave peak exceeds the statement memory budget");
    }
    result.parallel_waves.push_back(ready);
    const auto wave = static_cast<std::uint32_t>(
        result.parallel_waves.size() - 1);
    for (const auto ordinal : ready) {
      scheduled[ordinal] = true;
      ModelFamilyScheduledLegV1 scheduled_leg;
      scheduled_leg.leg = request.legs[ordinal];
      scheduled_leg.composition_admission_receipt_uuid =
          composition_admission_receipt_uuid;
      scheduled_leg.composition_arity = request.legs.size();
      scheduled_leg.dependency_ordinals = incoming[ordinal];
      std::sort(scheduled_leg.dependency_ordinals.begin(),
                scheduled_leg.dependency_ordinals.end());
      scheduled_leg.schedule_wave = wave;
      scheduled_leg.stable_start_ordinal = stable_start_ordinal++;
      scheduled_leg.causal_counter_id = ++causal_counter;
      dag_seed += "|" + scheduled_leg.leg.physical_node_uuid + ":" +
                  std::to_string(scheduled_leg.causal_counter_id);
      result.stable_schedule.push_back(std::move(scheduled_leg));
    }
    for (const auto ordinal : ready) {
      for (const auto consumer : outgoing[ordinal]) --indegree[consumer];
    }
  }
  for (const auto& consumer : result.relational_consumers) {
    peak_memory = std::max(peak_memory, consumer.memory_grant_bytes);
    if (peak_memory > request.statement_memory_budget_bytes) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "typed relational consumer exceeds the statement memory budget");
    }
  }
  std::uint64_t retained_peak_memory = 0;
  for (const auto& leg : request.legs) {
    if (!Add(leg.memory_grant_bytes, &retained_peak_memory) ||
        !Add(leg.exchange_buffer_bytes, &retained_peak_memory)) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "retained leg and exchange memory accounting overflowed");
    }
  }
  for (const auto& consumer : result.relational_consumers) {
    if (!Add(consumer.memory_grant_bytes, &retained_peak_memory)) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "retained relational consumer memory accounting overflowed");
    }
  }
  if (retained_peak_memory > request.statement_memory_budget_bytes) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "retained dependency DAG peak exceeds the statement memory budget");
  }
  peak_memory = retained_peak_memory;

  const auto add_receipt = [&](const std::string_view rule_id,
                               const std::string_view evidence_id,
                               const bool complete) {
    ModelFamilyCoordinatorRuleReceiptV1 receipt;
    receipt.rule_id = rule_id;
    receipt.evidence_id = evidence_id;
    receipt.causal_counter_id = ++causal_counter;
    receipt.complete = complete;
    receipt.receipt_uuid = DerivedUuid(
        dag_seed + "|" + receipt.rule_id + "|" + receipt.evidence_id + "|" +
        std::to_string(receipt.causal_counter_id));
    result.rule_receipts.push_back(std::move(receipt));
  };
  static constexpr std::array<std::string_view, 24> kEvidence = {
      "bound_object_uuid_receipt",
      "catalog_generation_receipts",
      "security_admission_receipts",
      "mga_context_identity_receipt",
      "capability_receipts",
      "candidate_inventory_receipt",
      "family_cost_vector_receipt",
      "dependency_dag_receipt",
      "property_receipts",
      "selected_alternative_receipt",
      "resource_admission_receipt",
      "spill_reservation_receipt",
      "row_cell_counter_receipt",
      "parallel_admission_receipt",
      "backpressure_transition_receipt",
      "typed_exchange_receipt",
      "recheck_counter_receipt",
      "short_circuit_receipt",
      "cancellation_fanout_receipt",
      "failure_propagation_receipt",
      "cleanup_cardinality_receipt",
      "root_publication_receipt",
      "scope_refusal_receipt",
      "later_feedback_consumption_receipt",
  };
  for (std::size_t index = 0; index < kEvidence.size(); ++index) {
    const auto rule = "COORD-" +
                      (index + 1 < 10 ? std::string("00")
                                      : std::string("0")) +
                      std::to_string(index + 1) + "-V1";
    const bool admission_complete =
        index <= 10 || index == 13 || index == 22 ||
        (index == 23 && feedback_later);
    add_receipt(rule, kEvidence[index], admission_complete);
  }

  result.accepted = true;
  result.deterministic = true;
  result.data_access_allowed = true;
  result.root_publication_candidate = true;
  result.no_partial_root = true;
  result.spill_reservation_required = request.spill_required;
  result.admitted_peak_memory_bytes = peak_memory;
  result.admitted_spill_bytes = request.maximum_spill_bytes;
  result.selected_plan_generation = request.selected_plan_generation;
  result.expected_cleanup_component_count =
      static_cast<std::uint64_t>(request.legs.size()) * 2 +
      static_cast<std::uint64_t>(request.relational_consumers.size()) +
      (request.spill_required ? 2 : 0);
  result.dependency_edges = request.edges;
  result.dependency_dag_receipt_uuid = DerivedUuid(dag_seed);
  result.composition_admission_receipt_uuid =
      composition_admission_receipt_uuid;
  result.diagnostic_id = "SB_EXECUTOR_OK";
  return result;
}

ModelFamilyDependencyCoordinatorResultV1
CoordinateModelFamilyDependencyDagV1(
    const ModelFamilyDependencyCoordinatorRequestV1& request) {
  const auto refuse = [](std::string diagnostic, std::string detail) {
    ModelFamilyDependencyCoordinatorResultV1 result;
    result.deterministic = true;
    result.no_partial_root = true;
    result.diagnostic_id = std::move(diagnostic);
    result.detail = std::move(detail);
    return result;
  };
  const std::map<std::string, std::size_t> profile_arities = {
      {"COMP-3-LINEAR-V1", 3},
      {"COMP-3-FANIN-V1", 3},
      {"COMP-3-INDEPENDENT-V1", 3},
      {"COMP-4-MIXED-V1", 4},
      {"COMP-3-LATERAL-V1", 3},
      {"COMP-3-SHARED-LEG-V1", 3},
      {"COMP-3-SHORT-CIRCUIT-V1", 3},
      {"COMP-3-CANCEL-FANOUT-V1", 3},
      {"COMP-3-FAILURE-CLEANUP-V1", 3},
      {"COMP-9-FULL-UNIVERSE-V1", 9},
  };
  const std::set<std::string> family_ids = {
      "relational", "document", "graph", "key_value", "time_series",
      "vector", "search", "spatial", "columnar"};
  const auto profile = profile_arities.find(request.composition_profile_id);
  if (request.abi_version != 1 || profile == profile_arities.end() ||
      request.legs.size() != profile->second ||
      !CanonicalUuid(request.bound_sblr_tree_uuid) ||
      !CanonicalUuid(request.canonical_root_physical_node_uuid) ||
      request.canonical_root_physical_node_id == 0) {
    return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                  "dependency coordinator request is structurally invalid");
  }

  std::set<std::string> bound_node_uuids;
  std::set<std::uint64_t> bound_node_ids;
  std::set<std::string> bound_descriptor_uuids;
  std::set<std::uint32_t> bound_descriptor_ids;
  for (std::size_t index = 0; index < request.legs.size(); ++index) {
    const auto& leg = request.legs[index];
    if (leg.abi_version != 1 || leg.lexical_source_ordinal != index ||
        !family_ids.contains(leg.family_id) ||
        !CanonicalUuid(leg.physical_node_uuid) ||
        !bound_node_uuids.insert(leg.physical_node_uuid).second ||
        leg.root_physical_node_id == 0 ||
        !bound_node_ids.insert(leg.root_physical_node_id).second ||
        !CanonicalUuid(leg.selected_plan_uuid) ||
        !CanonicalUuid(leg.bound_object_uuid) ||
        !CanonicalDescriptorLineage(leg.output_descriptor_ids,
                                    leg.output_descriptor_uuids) ||
        leg.parser_planning_authority_claimed ||
        leg.transaction_finality_authority_claimed) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "COORD-001 leg UUID or output descriptor binding is incomplete");
    }
    for (std::size_t ordinal = 0;
         ordinal < leg.output_descriptor_ids.size(); ++ordinal) {
      if (!bound_descriptor_ids.insert(leg.output_descriptor_ids[ordinal]).second ||
          !bound_descriptor_uuids
               .insert(leg.output_descriptor_uuids[ordinal])
               .second) {
        return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                      "COORD-001 output descriptor identity is aliased across legs");
      }
    }
  }

  if (request.selected_plan_generation == 0 ||
      request.selected_plan_generation !=
          request.current_selected_plan_generation) {
    return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                  "COORD-002 selected plan generation is stale");
  }
  for (const auto& leg : request.legs) {
    if (!CanonicalUuid(leg.catalog_snapshot_uuid) ||
        leg.catalog_snapshot_uuid != leg.current_catalog_snapshot_uuid ||
        !CanonicalUuid(leg.descriptor_snapshot_uuid) ||
        leg.descriptor_snapshot_uuid !=
            leg.current_descriptor_snapshot_uuid ||
        leg.catalog_generation == 0 ||
        leg.catalog_generation != leg.current_catalog_generation ||
        leg.descriptor_generation == 0 ||
        leg.descriptor_generation != leg.current_descriptor_generation) {
      return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                    "COORD-002 catalog or descriptor snapshot identity is stale");
    }
  }

  for (const auto& leg : request.legs) {
    if (!leg.security_admitted ||
        !CanonicalUuid(leg.security_context_uuid) ||
        leg.security_context_uuid != leg.current_security_context_uuid ||
        !CanonicalUuid(leg.policy_snapshot_uuid) ||
        leg.policy_snapshot_uuid != leg.current_policy_snapshot_uuid ||
        leg.security_generation == 0 ||
        leg.security_generation != leg.current_security_generation ||
        leg.policy_generation == 0 ||
        leg.policy_generation != leg.current_policy_generation) {
      return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                    "COORD-003 security or policy admission identity is stale");
    }
  }

  const auto& statement_context = request.legs.front().mga_statement_context;
  for (const auto& leg : request.legs) {
    if (!scratchbird::engine::executor::PhysicalMgaStatementContextValid(
            leg.mga_statement_context) ||
        !scratchbird::engine::executor::PhysicalMgaStatementContextEqual(
            statement_context, leg.mga_statement_context)) {
      return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                    "COORD-004 legs do not share one immutable MGA statement context");
    }
  }

  for (const auto& leg : request.legs) {
    if (!CanonicalUuid(leg.capability_uuid) ||
        !CanonicalUuid(leg.provider_uuid) ||
        !CanonicalUuid(leg.operation_scope_receipt_uuid) ||
        leg.capability_abi_version != 1 ||
        leg.capability_generation == 0 ||
        leg.capability_generation != leg.current_capability_generation ||
        leg.provider_generation == 0 ||
        leg.provider_generation != leg.current_provider_generation ||
        !leg.capability_admitted || !leg.cancellation_supported ||
        !leg.cleanup_supported) {
      return refuse("SB_MODEL_CAPABILITY_UNAVAILABLE_V1",
                    "COORD-005 provider capability, operation scope, or generation is unavailable");
    }
  }

  for (const auto& leg : request.legs) {
    if (leg.candidate_alternatives.empty()) {
      return refuse("SB_MODEL_CANDIDATE_SEMANTICS_MISSING_V1",
                    "COORD-006 family-local candidate inventory is absent");
    }
    std::set<std::string> alternatives;
    for (const auto& candidate : leg.candidate_alternatives) {
      if (!CanonicalUuid(candidate.alternative_uuid) ||
          !alternatives.insert(candidate.alternative_uuid).second ||
          !CanonicalUuid(candidate.candidate_inventory_receipt_uuid) ||
          candidate.implementation_id.empty() ||
          !DependencyOperationValid(leg.family_id, candidate.operation_ids,
                                    candidate.operation_id) ||
          !CanonicalUuid(candidate.operation_scope_receipt_uuid) ||
          !CanonicalUuid(candidate.selection_policy_receipt_uuid) ||
          candidate.authority_approved_comparison_rank == 0 ||
          (!candidate.exact && candidate.admitted) ||
          (candidate.exact_fallback && !candidate.exact)) {
        return refuse("SB_MODEL_CANDIDATE_SEMANTICS_MISSING_V1",
                      "COORD-006 family-local candidate semantics are missing or duplicated");
      }
    }
  }

  for (const auto& leg : request.legs) {
    if (!DependencyCostValid(leg.family_local_cost)) {
      return refuse("SB_MODEL_COST_VECTOR_INVALID_V1",
                    "COORD-007 selected family cost confidence or provenance is invalid");
    }
    for (const auto& candidate : leg.candidate_alternatives) {
      if (!DependencyCostValid(candidate.family_local_cost)) {
        return refuse("SB_MODEL_COST_VECTOR_INVALID_V1",
                      "COORD-007 candidate cost confidence or provenance is invalid");
      }
    }
  }

  for (const auto& leg : request.legs) {
    if (!CanonicalUuid(leg.delivered_property_uuid) ||
        !leg.exact_recheck_required ||
        !leg.base_row_mga_recheck_required ||
        !leg.security_recheck_required) {
      return refuse("SB_MODEL_PROPERTY_UNSATISFIED_V1",
                    "COORD-009 selected leg properties or rechecks are incomplete");
    }
  }

  for (const auto& leg : request.legs) {
    const ModelFamilyDependencyAlternativeV1* best = nullptr;
    for (const auto& candidate : leg.candidate_alternatives) {
      if (!candidate.available || !candidate.exact || !candidate.admitted) {
        continue;
      }
      if (best == nullptr || DependencyAlternativeLess(candidate, *best)) {
        best = &candidate;
      }
    }
    if (!leg.selected || !CanonicalUuid(leg.selected_alternative_uuid) ||
        !CanonicalUuid(leg.selected_alternative_receipt_uuid) ||
        best == nullptr || best->alternative_uuid != leg.selected_alternative_uuid ||
        best->operation_ids != leg.operation_ids ||
        best->operation_id != leg.operation_id ||
        best->operation_scope_receipt_uuid !=
            leg.operation_scope_receipt_uuid ||
        best->selection_policy_receipt_uuid !=
            leg.selected_alternative_receipt_uuid ||
        !DependencyCostEqual(best->family_local_cost,
                             leg.family_local_cost) ||
        !leg.exact ||
        (leg.exact_fallback_selected &&
         (!leg.exact_fallback_available || !best->exact_fallback))) {
      return refuse("SB_MODEL_NO_ADMITTED_ALTERNATIVE_V1",
                    "COORD-010 family-local deterministic selection or tie-break receipt is invalid");
    }
  }

  std::uint64_t retained_peak_memory = 0;
  for (const auto& leg : request.legs) {
    if (!CanonicalUuid(leg.resource_contract_uuid) ||
        leg.resource_contract_uuid != leg.current_resource_contract_uuid ||
        leg.resource_generation == 0 ||
        leg.resource_generation != leg.current_resource_generation ||
        leg.memory_grant_bytes == 0 || leg.exchange_buffer_bytes == 0 ||
        !Add(leg.memory_grant_bytes, &retained_peak_memory) ||
        !Add(leg.exchange_buffer_bytes, &retained_peak_memory)) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "COORD-011 resource contract or retained memory accounting is invalid");
    }
  }
  for (const auto& consumer : request.relational_consumers) {
    if (consumer.memory_grant_bytes == 0 ||
        !Add(consumer.memory_grant_bytes, &retained_peak_memory)) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "COORD-011 relational consumer memory accounting is invalid");
    }
  }
  if (request.statement_memory_budget_bytes == 0 ||
      retained_peak_memory > request.statement_memory_budget_bytes) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "COORD-011 retained DAG peak exceeds its statement budget");
  }

  for (const auto& leg : request.legs) {
    std::uint64_t maximum_cells = 0;
    if (leg.maximum_rows == 0 || leg.maximum_columns == 0 ||
        !CheckedMultiply(leg.maximum_rows, leg.maximum_columns,
                         &maximum_cells) ||
        maximum_cells != leg.maximum_cells ||
        leg.output_descriptor_ids.size() > leg.maximum_columns) {
      return refuse("SB_MODEL_RESOURCE_ROW_LIMIT_V1",
                    "COORD-013 leg row-column-cell bound is incoherent");
    }
  }
  for (const auto& consumer : request.relational_consumers) {
    std::uint64_t maximum_cells = 0;
    if (consumer.maximum_rows == 0 || consumer.maximum_columns == 0 ||
        !CheckedMultiply(consumer.maximum_rows, consumer.maximum_columns,
                         &maximum_cells) ||
        maximum_cells != consumer.maximum_cells ||
        consumer.output_descriptor_ids.size() > consumer.maximum_columns) {
      return refuse("SB_MODEL_RESOURCE_ROW_LIMIT_V1",
                    "COORD-013 relational consumer row-column-cell bound is incoherent");
    }
  }

  std::set<std::string> edge_uuids;
  for (const auto& edge : request.edges) {
    if (edge.abi_version != 1 || !CanonicalUuid(edge.edge_uuid) ||
        !edge_uuids.insert(edge.edge_uuid).second ||
        edge.producer_lexical_source_ordinal >= request.legs.size() ||
        edge.consumer_lexical_source_ordinal >= request.legs.size() ||
        edge.producer_lexical_source_ordinal ==
            edge.consumer_lexical_source_ordinal ||
        !CanonicalUuid(edge.descriptor_lineage_uuid) ||
        !edge.semantics_authorized ||
        edge.parser_execution_authority_claimed ||
        edge.transaction_finality_authority_claimed) {
      return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                    "COORD-008 dependency edge identity or endpoint is invalid");
    }
    const auto& producer =
        request.legs[edge.producer_lexical_source_ordinal];
    if (!CanonicalUuid(edge.required_property_uuid) ||
        !CanonicalUuid(edge.delivered_property_uuid) ||
        edge.required_property_uuid != edge.delivered_property_uuid ||
        edge.delivered_property_uuid != producer.delivered_property_uuid ||
        !edge.descriptor_compatible ||
        edge.producer_output_descriptor_ids != producer.output_descriptor_ids ||
        edge.consumer_input_descriptor_ids !=
            edge.producer_output_descriptor_ids ||
        edge.producer_output_descriptor_uuids !=
            producer.output_descriptor_uuids ||
        edge.consumer_input_descriptor_uuids !=
            edge.producer_output_descriptor_uuids) {
      return refuse("SB_MODEL_PROPERTY_UNSATISFIED_V1",
                    "COORD-009 dependency property or descriptor UUID lineage was substituted");
    }
  }

  std::set<std::string> consumer_uuids;
  std::set<std::uint64_t> consumer_causal_counters;
  std::map<std::string, std::uint64_t> consumer_causal_by_node;
  std::size_t canonical_root_count = 0;
  for (const auto& consumer : request.relational_consumers) {
    const bool shared_root_projection =
        request.composition_profile_id == "COMP-3-SHARED-LEG-V1" &&
        consumer.canonical_root;
    const auto canonical_shared_input = [&]() {
      if (!shared_root_projection || consumer.input_descriptor_ids.empty() ||
          consumer.input_descriptor_ids.size() !=
              consumer.input_descriptor_uuids.size()) {
        return false;
      }
      std::map<std::uint32_t, std::string> lineage;
      for (std::size_t index = 0;
           index < consumer.input_descriptor_ids.size(); ++index) {
        if (consumer.input_descriptor_ids[index] == 0 ||
            !CanonicalUuid(consumer.input_descriptor_uuids[index])) {
          return false;
        }
        const auto [found, inserted] = lineage.emplace(
            consumer.input_descriptor_ids[index],
            consumer.input_descriptor_uuids[index]);
        if (!inserted && found->second != consumer.input_descriptor_uuids[index])
          return false;
      }
      return true;
    }();
    if (consumer.abi_version != 1 ||
        !CanonicalUuid(consumer.physical_node_uuid) ||
        !consumer_uuids.insert(consumer.physical_node_uuid).second ||
        consumer.physical_node_id == 0 ||
        consumer.causal_counter_id == 0 ||
        !consumer_causal_counters.insert(consumer.causal_counter_id).second ||
        !CanonicalUuid(consumer.selected_implementation_uuid) ||
        !CanonicalUuid(consumer.expected_security_receipt_uuid) ||
        consumer.input_physical_node_uuids.size() != 2 ||
        (!CanonicalDescriptorLineage(consumer.input_descriptor_ids,
                                     consumer.input_descriptor_uuids) &&
         !canonical_shared_input) ||
        !CanonicalDescriptorLineage(consumer.output_descriptor_ids,
                                    consumer.output_descriptor_uuids) ||
        consumer.parser_execution_authority_claimed ||
        consumer.transaction_finality_authority_claimed) {
      return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                    "COORD-008 typed relational consumer identity is invalid");
    }
    std::vector<std::uint32_t> projected_input_ids;
    std::vector<std::string> projected_input_uuids;
    if (shared_root_projection) {
      std::set<std::uint32_t> seen;
      for (std::size_t index = 0;
           index < consumer.input_descriptor_ids.size(); ++index) {
        if (seen.insert(consumer.input_descriptor_ids[index]).second) {
          projected_input_ids.push_back(consumer.input_descriptor_ids[index]);
          projected_input_uuids.push_back(
              consumer.input_descriptor_uuids[index]);
        }
      }
    }
    if ((!shared_root_projection &&
         (consumer.input_descriptor_ids != consumer.output_descriptor_ids ||
          consumer.input_descriptor_uuids !=
              consumer.output_descriptor_uuids)) ||
        (shared_root_projection &&
         (projected_input_ids != consumer.output_descriptor_ids ||
          projected_input_uuids != consumer.output_descriptor_uuids))) {
      return refuse("SB_MODEL_PROPERTY_UNSATISFIED_V1",
                    "COORD-009 typed relational descriptor UUID lineage was substituted");
    }
    if (!scratchbird::engine::executor::PhysicalMgaStatementContextValid(
            consumer.mga_statement_context) ||
        !scratchbird::engine::executor::PhysicalMgaStatementContextEqual(
            statement_context, consumer.mga_statement_context)) {
      return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                    "COORD-004 typed relational consumer MGA context drifted");
    }
    if (!consumer.exact || !consumer.cleanup_supported ||
        !consumer.cancellation_supported) {
      return refuse("SB_MODEL_CAPABILITY_UNAVAILABLE_V1",
                    "COORD-005 typed relational consumer capability is unavailable");
    }
    consumer_causal_by_node.emplace(consumer.physical_node_uuid,
                                    consumer.causal_counter_id);
    canonical_root_count += consumer.canonical_root ? 1 : 0;
  }
  if (canonical_root_count != 1) {
    return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                  "COORD-008 typed relational DAG lacks one canonical root");
  }
  for (const auto& consumer : request.relational_consumers) {
    for (const auto& input_uuid : consumer.input_physical_node_uuids) {
      const auto producer = consumer_causal_by_node.find(input_uuid);
      if (producer != consumer_causal_by_node.end() &&
          producer->second >= consumer.causal_counter_id) {
        return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                      "COORD-008 relational consumer causal counters are not topologically increasing");
      }
    }
  }

  return CoordinateModelFamilyDependencyDagValidatedV1(request);
}

ModelFamilyJoinAdmissionResultV1 CoordinateModelFamilyJoinAdmissionV1(
    const ModelFamilyJoinAdmissionRequestV1& request) {
  ModelFamilyJoinAdmissionResultV1 result;
  const auto refuse = [&](const char* diagnostic, const char* detail) {
    result.accepted = false;
    result.deterministic = true;
    result.root_publication_allowed = false;
    result.left_provider_route_id.clear();
    result.right_provider_route_id.clear();
    result.relational_consumer_route_id.clear();
    result.condition_lowering_route_id.clear();
    result.diagnostic_id = diagnostic;
    result.detail = detail;
    return result;
  };
  const std::set<std::string> family_ids = {
      "relational", "document", "graph", "key_value", "time_series",
      "vector", "search", "spatial", "columnar"};
  const std::set<std::string> regular_join_forms = {
      "INNER", "LEFT", "RIGHT", "FULL", "SEMI", "ANTI", "CROSS",
      "LATERAL_INNER", "LATERAL_LEFT"};
  const std::set<std::string> predicate_join_forms = {
      "INNER", "LEFT", "RIGHT", "FULL", "SEMI", "ANTI",
      "LATERAL_INNER", "LATERAL_LEFT"};
  const std::set<std::string> scenario_profiles = {
      "JOIN-SCENARIO-BASELINE-V1",
      "JOIN-SCENARIO-NULL-V1",
      "JOIN-SCENARIO-MISSING-V1",
      "JOIN-SCENARIO-DUPLICATE-V1",
      "JOIN-SCENARIO-LOSSLESS-COERCION-V1",
      "JOIN-SCENARIO-LOSSY-COERCION-REFUSAL-V1",
      "JOIN-SCENARIO-COLLATION-V1",
      "JOIN-SCENARIO-TIMEZONE-V1",
      "JOIN-SCENARIO-SCORE-IMPLICIT-KEY-REFUSAL-V1",
      "JOIN-SCENARIO-SPATIAL-CRS-REFUSAL-V1",
      "JOIN-SCENARIO-EMPTY-LEFT-V1",
      "JOIN-SCENARIO-EMPTY-RIGHT-V1",
      "JOIN-SCENARIO-LEFT-FAILURE-V1",
      "JOIN-SCENARIO-RIGHT-FAILURE-V1",
      "JOIN-SCENARIO-CANCELLATION-V1"};
  if (request.abi_version != 1 ||
      !family_ids.contains(request.left_family_id) ||
      !family_ids.contains(request.right_family_id)) {
    return refuse("SB_MODEL_JOIN_SEMANTIC_PRECONDITION_REFUSED_V1",
                  "model-family join direction is outside the signed 81-pair matrix");
  }

  const bool asof = request.join_form_id == "ASOF";
  const bool regular = regular_join_forms.contains(request.join_form_id);
  if (!regular && !asof) {
    return refuse("SB_MODEL_JOIN_FORM_REFUSED_V1",
                  "join form is outside the signed ten-form matrix");
  }

  // Pair disposition has precedence over condition and scenario disposition.
  if (asof && request.left_family_id != "time_series" &&
      request.right_family_id != "time_series") {
    return refuse("SB_MODEL_ASOF_REQUIRES_TIME_SERIES_DIRECTION_V1",
                  "ASOF requires a time-series endpoint in this direction");
  }

  const bool predicate_form =
      predicate_join_forms.contains(request.join_form_id);
  if (request.condition_form_id == "ON") {
    if (!predicate_form) {
      return refuse("SB_MODEL_JOIN_CONDITION_FORM_REFUSED_V1",
                    "ON is not accepted for this join form");
    }
  } else if (request.condition_form_id == "USING") {
    if (!predicate_form) {
      return refuse("SB_MODEL_JOIN_USING_BINDING_REFUSED_V1",
                    "USING is not accepted for this join form");
    }
  } else if (request.condition_form_id == "NATURAL") {
    if (!predicate_form) {
      return refuse("SB_MODEL_JOIN_NATURAL_BINDING_REFUSED_V1",
                    "NATURAL is not accepted for this join form");
    }
  } else if (request.condition_form_id == "NONE") {
    if (request.join_form_id != "CROSS") {
      return refuse("SB_MODEL_JOIN_CONDITION_FORM_REFUSED_V1",
                    "NONE is accepted only for CROSS");
    }
  } else if (request.condition_form_id == "ASOF_KEY") {
    if (!asof) {
      return refuse("SB_MODEL_ASOF_BINDING_REFUSED_V1",
                    "ASOF_KEY is accepted only for ASOF");
    }
  } else {
    return refuse("SB_MODEL_JOIN_CONDITION_FORM_REFUSED_V1",
                  "condition form is outside the signed five-form matrix");
  }

  if (!scenario_profiles.contains(request.scenario_profile_id)) {
    return refuse("SB_MODEL_JOIN_SCENARIO_PROFILE_REFUSED_V1",
                  "join scenario is outside the signed fifteen-profile matrix");
  }
  if (request.scenario_profile_id ==
          "JOIN-SCENARIO-LOSSY-COERCION-REFUSAL-V1" &&
      request.join_form_id != "CROSS") {
    return refuse("SB_MODEL_JOIN_LOSSY_COERCION_REFUSED_V1",
                  "lossy join-key coercion is forbidden");
  }
  if (request.scenario_profile_id ==
          "JOIN-SCENARIO-SCORE-IMPLICIT-KEY-REFUSAL-V1" &&
      request.join_form_id != "CROSS") {
    return refuse("SB_MODEL_JOIN_SCORE_IMPLICIT_KEY_REFUSED_V1",
                  "score, distance, and rank are not implicit join keys");
  }
  if (request.scenario_profile_id ==
          "JOIN-SCENARIO-SPATIAL-CRS-REFUSAL-V1" &&
      request.join_form_id != "CROSS") {
    return refuse("SB_MODEL_JOIN_SPATIAL_CRS_REFUSED_V1",
                  "spatial join keys require a matched CRS or signed transform");
  }
  if (request.scenario_profile_id == "JOIN-SCENARIO-LEFT-FAILURE-V1") {
    return refuse("SB_MODEL_JOIN_LEFT_LEG_FAILED_V1",
                  "left model-family leg failed before join publication");
  }
  if (request.scenario_profile_id == "JOIN-SCENARIO-RIGHT-FAILURE-V1") {
    return refuse("SB_MODEL_JOIN_RIGHT_LEG_FAILED_V1",
                  "right model-family leg failed before join publication");
  }
  if (request.scenario_profile_id == "JOIN-SCENARIO-CANCELLATION-V1") {
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "model-family composition was cancelled before publication");
  }

  result.accepted = true;
  result.deterministic = true;
  result.root_publication_allowed = true;
  const auto provider_route_id = [](const std::string_view family) {
    return family == "relational"
               ? std::string("canonical.relational.heap-source.v1")
               : std::string("canonical.model-provider.") +
                     std::string(family) + ".v1";
  };
  result.left_provider_route_id = provider_route_id(request.left_family_id);
  result.right_provider_route_id = provider_route_id(request.right_family_id);
  if (request.join_form_id == "LATERAL_INNER" ||
      request.join_form_id == "LATERAL_LEFT") {
    result.relational_consumer_route_id =
        "canonical.relational.lateral-correlated.v1";
  } else if (request.join_form_id == "ASOF") {
    result.relational_consumer_route_id =
        "canonical.relational.time-series-asof.v1";
  } else {
    result.relational_consumer_route_id =
        "canonical.relational.join-3vl-nested.v1";
  }
  if (request.condition_form_id == "ON") {
    result.condition_lowering_route_id =
        "canonical.relational.on-typed-predicate.v1";
  } else if (request.condition_form_id == "USING") {
    result.condition_lowering_route_id =
        "canonical.relational.using-descriptor-equality.v1";
  } else if (request.condition_form_id == "NATURAL") {
    result.condition_lowering_route_id =
        "canonical.relational.natural-to-using.v1";
  } else if (request.condition_form_id == "NONE") {
    result.condition_lowering_route_id =
        "canonical.relational.cross-no-condition.v1";
  } else {
    result.condition_lowering_route_id =
        "canonical.relational.asof-key-binding.v1";
  }
  result.diagnostic_id = "SB_EXECUTOR_OK";
  return result;
}

}  // namespace scratchbird::engine::optimizer
