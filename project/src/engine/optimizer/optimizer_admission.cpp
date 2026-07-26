// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_catalog_backed_planning.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

constexpr std::string_view kCanonicalRouteId =
    "native.sblr.query.execute.v2";

bool IsCanonicalUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) return false;
  }
  return true;
}

template <typename T>
bool IsUnique(const std::vector<T>& values) {
  std::unordered_set<T> seen;
  return std::ranges::all_of(values,
                             [&](const auto& value) {
                               return seen.insert(value).second;
                             });
}

std::vector<std::uint32_t> RequiredDescriptors(
    const planner::CanonicalLogicalRelationalGraph& graph) {
  std::vector<std::uint32_t> descriptors = graph.result_descriptor_ids;
  for (const auto& node : graph.nodes) {
    descriptors.insert(descriptors.end(), node.output_descriptor_ids.begin(),
                       node.output_descriptor_ids.end());
  }
  std::ranges::sort(descriptors);
  descriptors.erase(std::unique(descriptors.begin(), descriptors.end()),
                    descriptors.end());
  return descriptors;
}

std::vector<std::string> RequiredObjects(
    const planner::CanonicalLogicalRelationalGraph& graph) {
  std::vector<std::string> objects;
  for (const auto& node : graph.nodes) {
    objects.insert(objects.end(), node.required_object_uuids.begin(),
                   node.required_object_uuids.end());
  }
  std::ranges::sort(objects);
  objects.erase(std::unique(objects.begin(), objects.end()), objects.end());
  return objects;
}

std::vector<planner::CanonicalLogicalRelationalNodeKind> CoreNodeKinds() {
  std::vector<planner::CanonicalLogicalRelationalNodeKind> kinds;
  for (auto raw = static_cast<std::uint8_t>(
           planner::CanonicalLogicalRelationalNodeKind::kRelationSource);
       raw <= static_cast<std::uint8_t>(
                  planner::CanonicalLogicalRelationalNodeKind::
                      kTableFunctionInvoke);
       ++raw) {
    kinds.push_back(
        static_cast<planner::CanonicalLogicalRelationalNodeKind>(raw));
  }
  return kinds;
}

}  // namespace

// QOW-SOURCE-OPT-006-CATALOG-V1
CanonicalOptimizerAdmissionResult AdmitCanonicalOptimizerPlanningRequest(
    const CanonicalOptimizerAdmissionRequest& request) {
  CanonicalOptimizerAdmissionResult result;
  const auto refuse = [&](const CanonicalOptimizerAdmissionStage stage,
                          std::string diagnostic_id,
                          std::string field_id) {
    result.admitted = false;
    result.planning_allowed = false;
    result.benchmark_clean_ready = false;
    result.data_access_allowed = false;
    result.issues.push_back(
        {stage, std::move(diagnostic_id), std::move(field_id)});
    return result;
  };
  const auto record = [&](const CanonicalOptimizerAdmissionStage stage,
                          std::string evidence_id) {
    result.evidence.push_back({stage, std::move(evidence_id)});
  };

  if (request.abi_version != 1 ||
      !request.populated_from_admitted_typed_sblr ||
      request.data_access_observed ||
      request.parser_planning_authority_claimed) {
    return refuse(CanonicalOptimizerAdmissionStage::kBoundRequest,
                  "QOW-DIAG-OPTIMIZER-ADMISSION-BOUND-REQUEST-V1",
                  "admitted_typed_sblr_boundary");
  }
  const auto graph_validation =
      planner::ValidateCanonicalLogicalRelationalGraph(request.logical_graph);
  if (!graph_validation.accepted) {
    return refuse(CanonicalOptimizerAdmissionStage::kBoundRequest,
                  graph_validation.issues.front().diagnostic_id,
                  graph_validation.issues.front().field_id);
  }
  const auto property_validation =
      planner::ValidateCanonicalLogicalPropertyCatalog(
          request.logical_graph, request.logical_properties);
  if (!property_validation.accepted) {
    return refuse(CanonicalOptimizerAdmissionStage::kBoundRequest,
                  property_validation.issues.front().diagnostic_id,
                  property_validation.issues.front().field_id);
  }
  record(CanonicalOptimizerAdmissionStage::kBoundRequest,
         "canonical_bound_request=validated");

  const auto required_descriptors = RequiredDescriptors(request.logical_graph);
  const auto required_objects = RequiredObjects(request.logical_graph);
  if (!request.catalog.engine_owned ||
      !IsCanonicalUuid(request.catalog.snapshot_uuid) ||
      request.catalog.catalog_epoch_uuid !=
          request.logical_graph.catalog_epoch_uuid ||
      request.catalog.snapshot_uuid != request.catalog.catalog_epoch_uuid ||
      request.catalog.catalog_generation == 0 ||
      !IsUnique(request.catalog.object_uuids) ||
      !IsUnique(request.catalog.descriptor_ids)) {
    return refuse(CanonicalOptimizerAdmissionStage::kCatalogEpoch,
                  "QOW-DIAG-OPTIMIZER-ADMISSION-CATALOG-V1",
                  "catalog_snapshot_identity");
  }
  for (const auto descriptor_id : required_descriptors) {
    if (std::ranges::find(request.catalog.descriptor_ids, descriptor_id) ==
        request.catalog.descriptor_ids.end()) {
      return refuse(CanonicalOptimizerAdmissionStage::kCatalogEpoch,
                    "QOW-DIAG-OPTIMIZER-ADMISSION-CATALOG-V1",
                    "descriptor_snapshot_coverage");
    }
  }
  for (const auto& object_uuid : required_objects) {
    if (!IsCanonicalUuid(object_uuid) ||
        std::ranges::find(request.catalog.object_uuids, object_uuid) ==
            request.catalog.object_uuids.end()) {
      return refuse(CanonicalOptimizerAdmissionStage::kCatalogEpoch,
                    "QOW-DIAG-OPTIMIZER-ADMISSION-CATALOG-V1",
                    "object_snapshot_coverage");
    }
  }
  record(CanonicalOptimizerAdmissionStage::kCatalogEpoch,
         "catalog_snapshot=validated");

  if (!request.security.engine_owned ||
      request.security.security_context_uuid !=
          request.logical_graph.security_context_uuid ||
      request.security.security_epoch == 0 ||
      request.security.policy_epoch == 0 ||
      request.security.catalog_generation !=
          request.catalog.catalog_generation ||
      !IsUnique(request.security.authorized_object_uuids)) {
    return refuse(CanonicalOptimizerAdmissionStage::kSecurity,
                  "QOW-DIAG-OPTIMIZER-ADMISSION-SECURITY-V1",
                  "security_snapshot_identity");
  }
  for (const auto& object_uuid : required_objects) {
    if (std::ranges::find(request.security.authorized_object_uuids,
                          object_uuid) ==
        request.security.authorized_object_uuids.end()) {
      return refuse(CanonicalOptimizerAdmissionStage::kSecurity,
                    "QOW-DIAG-OPTIMIZER-ADMISSION-SECURITY-V1",
                    "authorized_object_coverage");
    }
  }
  record(CanonicalOptimizerAdmissionStage::kSecurity,
         "security_snapshot=validated");

  if (!request.mga.engine_owned || !request.mga.transaction_active ||
      !request.mga.statement_snapshot_fixed ||
      request.mga.finality_authority_claimed ||
      request.mga.local_transaction_id !=
          request.logical_graph.local_transaction_id ||
      request.mga.statement_snapshot_id !=
          request.logical_graph.statement_snapshot_id ||
      request.mga.metadata_snapshot_uuid != request.catalog.snapshot_uuid) {
    return refuse(CanonicalOptimizerAdmissionStage::kMgaStatementBoundary,
                  "QOW-DIAG-OPTIMIZER-ADMISSION-MGA-V1",
                  "mga_statement_boundary");
  }
  record(CanonicalOptimizerAdmissionStage::kMgaStatementBoundary,
         "mga_statement_boundary=engine_owned");

  if (!request.policy_capability.engine_owned ||
      !IsCanonicalUuid(request.policy_capability.policy_snapshot_uuid) ||
      !IsCanonicalUuid(request.policy_capability.capability_snapshot_uuid) ||
      request.policy_capability.policy_snapshot_uuid !=
          request.security.security_context_uuid ||
      request.policy_capability.policy_epoch != request.security.policy_epoch ||
      request.policy_capability.capability_abi_version != 1 ||
      request.policy_capability.cluster_capability_claimed ||
      !IsUnique(request.policy_capability.supported_node_kinds)) {
    return refuse(CanonicalOptimizerAdmissionStage::kPolicyCapability,
                  "QOW-DIAG-OPTIMIZER-ADMISSION-POLICY-CAPABILITY-V1",
                  "policy_capability_snapshot");
  }
  for (const auto& node : request.logical_graph.nodes) {
    if (std::ranges::find(request.policy_capability.supported_node_kinds,
                          node.node_kind) ==
        request.policy_capability.supported_node_kinds.end()) {
      return refuse(CanonicalOptimizerAdmissionStage::kPolicyCapability,
                    "QOW-DIAG-OPTIMIZER-ADMISSION-POLICY-CAPABILITY-V1",
                    "logical_node_capability");
    }
  }
  record(CanonicalOptimizerAdmissionStage::kPolicyCapability,
         "policy_capability_snapshot=validated");

  if (!request.resource.engine_owned ||
      !IsCanonicalUuid(request.resource.resource_snapshot_uuid) ||
      request.resource.resource_epoch == 0 ||
      request.resource.memory_budget_bytes == 0 ||
      request.resource.maximum_candidate_count <
          request.logical_graph.nodes.size() ||
      request.resource.maximum_memo_groups <
          request.logical_graph.nodes.size() ||
      request.resource.maximum_search_steps <
          request.logical_graph.nodes.size() ||
      request.resource.maximum_planning_time_ns == 0) {
    return refuse(CanonicalOptimizerAdmissionStage::kResource,
                  "QOW-DIAG-OPTIMIZER-ADMISSION-RESOURCE-V1",
                  "resource_snapshot");
  }
  record(CanonicalOptimizerAdmissionStage::kResource,
         "resource_snapshot=validated");

  const auto statistics = AdmitCanonicalOptimizerStatisticsBeforeAccess(
      request.logical_graph, request.statistics);
  if (!statistics.accepted) {
    return refuse(CanonicalOptimizerAdmissionStage::kStatisticsProvenance,
                  statistics.issues.front().diagnostic_id,
                  statistics.issues.front().field_id);
  }
  result.degraded_for_unknown_statistics =
      statistics.degraded_for_unknown_statistics;
  result.benchmark_clean_ready = statistics.benchmark_clean_ready;
  record(CanonicalOptimizerAdmissionStage::kStatisticsProvenance,
         result.degraded_for_unknown_statistics
             ? "statistics_snapshot=explicit_unknown_degraded"
             : "statistics_snapshot=catalog_qualified");

  if (!request.route.engine_owned ||
      !IsCanonicalUuid(request.route.route_snapshot_uuid) ||
      request.route.route_epoch == 0 || request.route.route_generation == 0 ||
      request.route.operation_id != "query.execute" ||
      request.route.route_id != kCanonicalRouteId ||
      !request.route.native_local_route ||
      request.route.cluster_route_claimed) {
    return refuse(CanonicalOptimizerAdmissionStage::kCanonicalRoute,
                  "QOW-DIAG-OPTIMIZER-ADMISSION-ROUTE-V1",
                  "canonical_route_snapshot");
  }
  record(CanonicalOptimizerAdmissionStage::kCanonicalRoute,
         "canonical_route=native.sblr.query.execute.v2");

  result.admitted = true;
  result.planning_allowed = true;
  result.data_access_allowed = false;
  result.bound_sblr_tree_uuid = request.logical_graph.bound_sblr_tree_uuid;
  result.catalog_epoch_uuid = request.logical_graph.catalog_epoch_uuid;
  result.security_context_uuid =
      request.logical_graph.security_context_uuid;
  result.capability_snapshot_uuid =
      request.policy_capability.capability_snapshot_uuid;
  result.resource_snapshot_uuid = request.resource.resource_snapshot_uuid;
  result.statistics_snapshot_uuid =
      request.statistics.statistics_snapshot_uuid;
  result.route_snapshot_uuid = request.route.route_snapshot_uuid;
  result.local_transaction_id = request.logical_graph.local_transaction_id;
  result.statement_snapshot_id = request.logical_graph.statement_snapshot_id;
  result.catalog_generation = request.catalog.catalog_generation;
  result.security_epoch = request.security.security_epoch;
  result.policy_epoch = request.security.policy_epoch;
  result.resource_epoch = request.resource.resource_epoch;
  result.statistics_generation = request.statistics.statistics_generation;
  result.route_epoch = request.route.route_epoch;
  result.route_generation = request.route.route_generation;
  return result;
}

CanonicalNativeAdmissionBuildResult
BuildCanonicalObjectFreeNativeOptimizerAdmissionRequest(
    const planner::CanonicalLogicalRelationalGraph& graph,
    const planner::CanonicalLogicalPropertyCatalog& properties,
    const CanonicalNativeObjectFreeAdmissionContext& context) {
  CanonicalNativeAdmissionBuildResult result;
  const auto required_objects = RequiredObjects(graph);
  if (!required_objects.empty()) {
    result.diagnostic_id =
        "QOW-DIAG-OPTIMIZER-ADMISSION-CATALOG-OBJECT-EVIDENCE-V1";
    result.field_id = "required_object_snapshot";
    return result;
  }

  auto& request = result.request;
  request.logical_graph = graph;
  request.logical_properties = properties;
  request.populated_from_admitted_typed_sblr = true;

  request.catalog.snapshot_uuid = context.catalog_snapshot_uuid;
  request.catalog.catalog_epoch_uuid = graph.catalog_epoch_uuid;
  request.catalog.catalog_generation = context.catalog_generation;
  request.catalog.descriptor_ids = RequiredDescriptors(graph);
  request.catalog.engine_owned = context.metadata_snapshot_engine_owned;

  request.security.security_context_uuid = context.security_context_uuid;
  request.security.security_epoch = context.security_epoch;
  request.security.policy_epoch = context.policy_epoch;
  request.security.catalog_generation =
      context.authorization_catalog_generation;
  request.security.engine_owned = context.authorization_context_engine_owned;

  request.mga.local_transaction_id = context.local_transaction_id;
  request.mga.statement_snapshot_id = context.statement_snapshot_id;
  request.mga.metadata_snapshot_uuid = context.catalog_snapshot_uuid;
  request.mga.transaction_active = context.local_transaction_id != 0;
  request.mga.statement_snapshot_fixed = context.statement_snapshot_id != 0;
  request.mga.engine_owned = true;

  request.policy_capability.policy_snapshot_uuid =
      context.security_context_uuid;
  request.policy_capability.policy_epoch = context.policy_epoch;
  request.policy_capability.capability_snapshot_uuid =
      context.capability_snapshot_uuid;
  request.policy_capability.capability_abi_version = 1;
  request.policy_capability.supported_node_kinds = CoreNodeKinds();
  request.policy_capability.engine_owned = true;

  request.resource.resource_snapshot_uuid =
      context.resource_snapshot_uuid;
  request.resource.resource_epoch = context.resource_epoch;
  request.resource.memory_budget_bytes = context.memory_budget_bytes;
  request.resource.maximum_candidate_count = context.maximum_candidate_count;
  request.resource.maximum_memo_groups = context.maximum_memo_groups;
  request.resource.maximum_search_steps = context.maximum_search_steps;
  request.resource.maximum_planning_time_ns =
      context.maximum_planning_time_ns;
  request.resource.spill_allowed = context.spill_allowed;
  request.resource.engine_owned = true;

  request.statistics.statistics_snapshot_uuid = context.statement_uuid;
  request.statistics.catalog_epoch_uuid = graph.catalog_epoch_uuid;
  request.statistics.statistics_generation = context.catalog_generation;
  request.statistics.admitted_at_monotonic_ns =
      context.admitted_at_monotonic_ns;
  request.statistics.captured_before_data_access = true;
  for (const auto& node : graph.nodes) {
    CanonicalOptimizerNodeEstimate estimate;
    estimate.logical_node_id = node.logical_node_id;
    estimate.state =
        node.node_kind == planner::CanonicalLogicalRelationalNodeKind::kValues
            ? CanonicalOptimizerStatisticState::kNotApplicable
            : CanonicalOptimizerStatisticState::kUnknown;
    estimate.source = CanonicalOptimizerStatisticSource::kUnavailable;
    estimate.catalog_epoch_uuid = graph.catalog_epoch_uuid;
    estimate.statistics_snapshot_uuid = context.statement_uuid;
    estimate.statistics_generation = context.catalog_generation;
    estimate.admitted_at_monotonic_ns = context.admitted_at_monotonic_ns;
    estimate.confidence = CostConfidence::kUnknown;
    request.statistics.node_estimates.push_back(std::move(estimate));
  }

  request.route.route_snapshot_uuid = context.route_snapshot_uuid;
  request.route.route_epoch = context.route_epoch;
  request.route.route_generation = context.route_generation;
  request.route.operation_id = "query.execute";
  request.route.route_id = std::string(kCanonicalRouteId);
  request.route.native_local_route = true;
  request.route.engine_owned = true;

  result.admission = AdmitCanonicalOptimizerPlanningRequest(request);
  if (!result.admission.admitted) {
    result.diagnostic_id = result.admission.issues.front().diagnostic_id;
    result.field_id = result.admission.issues.front().field_id;
    return result;
  }
  result.built = true;
  return result;
}

}  // namespace scratchbird::engine::optimizer
