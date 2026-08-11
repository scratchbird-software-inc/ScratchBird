// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "model_family_coordinator.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <unordered_set>

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

}  // namespace

ModelFamilyCoordinatorResultV1 CoordinateModelFamilySourceV1(
    const ModelFamilyCoordinatorRequestV1& request) {
  // QOW-SOURCE-RCP-074-COMMON-MODEL-COORDINATOR-V1
  ModelFamilyCoordinatorResultV1 result;
  const bool document_family = request.family_id == "document";
  const bool graph_family = request.family_id == "graph";
  const bool key_value_family = request.family_id == "key_value";
  const bool time_series_family = request.family_id == "time_series";
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
        request.operation_id == "TIME_SERIES_DOWNSAMPLE"));
  const std::string expected_logical_operator =
      graph_family ? "LOGICAL_GRAPH_SOURCE_V1"
                   : key_value_family ? "LOGICAL_KEY_VALUE_SOURCE_V1"
                   : time_series_family ? "LOGICAL_TIME_SERIES_SOURCE_V1"
                                      : "LOGICAL_DOCUMENT_SOURCE_V1";
  const std::string expected_physical_operator =
      graph_family ? "PHYSICAL_GRAPH_ADJACENCY_SCAN_V1"
                   : key_value_family ? "PHYSICAL_KEY_VALUE_SCAN_V1"
                   : time_series_family ? "PHYSICAL_TIME_SERIES_RANGE_SCAN_V1"
                                      : "PHYSICAL_DOCUMENT_PATH_SCAN_V1";
  const std::string expected_implementation =
      graph_family ? "physical_graph_adjacency_scan_v1"
                   : key_value_family ? "physical_key_value_scan_v1"
                   : time_series_family ? "physical_time_series_range_scan_v1"
                                      : "physical_document_path_scan_v1";
  result.logical_operator_id = expected_logical_operator;
  result.physical_operator_id = expected_physical_operator;
  const auto refuse = [&](const char* diagnostic, std::string detail) {
    result.diagnostic_id = diagnostic;
    result.detail = std::move(detail);
    return result;
  };

  const bool timestamp_family = key_value_family || time_series_family;
  if (timestamp_family !=
          !request.mga_statement_context.statement_timestamp.empty() ||
      (timestamp_family &&
       !CanonicalStatementTimestamp(
           request.mga_statement_context.statement_timestamp))) {
    return refuse(time_series_family
                      ? "SB_MODEL_TIME_SERIES_TIMESTAMP_INVALID_V1"
                      : "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1",
                  "model-family coordinator statement timestamp is invalid");
  }
  if (request.abi_version != 1 ||
      (!document_family && !graph_family && !key_value_family &&
       !time_series_family) ||
      !valid_operation ||
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
    if ((key_value_family || time_series_family)
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

}  // namespace scratchbird::engine::optimizer
