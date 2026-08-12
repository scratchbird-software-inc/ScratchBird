// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "model_family_coordinator.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <map>
#include <set>
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

bool CandidateScore(const ModelFamilyCandidateV1& candidate,
                    std::uint64_t* score) {
  *score = 0;
  return Add(candidate.cost.cpu_units, score) &&
         Add(candidate.cost.sequential_read_units, score) &&
         Add(candidate.cost.random_read_units, score) &&
         Add(candidate.cost.memory_bytes_required, score) &&
         Add(candidate.cost.uncertainty_penalty, score) &&
         Add(candidate.cost.risk_penalty, score);
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
  if (timestamp_family !=
          !request.mga_statement_context.statement_timestamp.empty() ||
      (timestamp_family &&
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
  for (const auto& candidate : request.candidates) {
    std::uint64_t score = 0;
    if (!CanonicalUuid(candidate.alternative_uuid) ||
        !alternative_ids.insert(candidate.alternative_uuid).second ||
        !CanonicalUuid(candidate.provider_uuid) ||
        !CanonicalUuid(candidate.capability_uuid) ||
        !CanonicalUuid(candidate.cost.cost_vector_uuid) ||
        candidate.provider_generation == 0 || !candidate.engine_owned ||
        !candidate.local_scope || candidate.parser_planning_authority_claimed ||
        candidate.transaction_finality_authority_claimed ||
        candidate.implementation_id != expected_implementation ||
        !CandidateScore(candidate, &score)) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "model-family candidate domain is incomplete or duplicated");
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
  result.selected_candidate = *selected;
  result.physical_dag = std::move(dag);
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
          !CanonicalUuid(demand.persisted_type_uuid)) {
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
