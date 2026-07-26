// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::executor {

enum class PhysicalNodeKind : std::uint8_t {
  kScan = 1,
  kFilter,
  kProject,
  kJoin,
  kAggregate,
  kSort,
  kLimit,
  kWindow,
  kSetOperation,
  kSubquery,
  kCte,
  kRecursiveCte,
  kValues,
  kPivot,
  kUnpivot,
  kMatchRecognize,
  kTableFunctionInvoke,
};

enum class PhysicalAdmissionStage : std::uint8_t {
  kBoundRequest = 1,
  kCatalogEpoch,
  kSecurity,
  kMgaStatementBoundary,
  kPolicyCapability,
  kResource,
  kStatisticsProvenance,
  kCanonicalRoute,
};

struct PhysicalAdmissionEvidence {
  PhysicalAdmissionStage stage{PhysicalAdmissionStage::kBoundRequest};
  std::string evidence_uuid;
};

struct PhysicalNodeRecord {
  std::uint64_t physical_node_id{0};
  std::uint32_t relational_node_id{0};
  PhysicalNodeKind node_kind{PhysicalNodeKind::kValues};
  std::string implementation_id;
  std::vector<std::uint64_t> input_physical_node_ids;
  std::vector<std::uint32_t> output_descriptor_ids;
  bool shareable{false};
  std::uint64_t causal_counter_id{0};
  std::string selected_alternative_uuid;
  std::string executor_capability_uuid;
  std::uint32_t executor_capability_abi_version{0};
  std::string cost_vector_uuid;
  std::vector<std::string> required_property_uuids;
  std::vector<std::string> delivered_property_uuids;
  std::uint64_t memory_bytes_required{0};
  std::uint64_t spill_bytes_expected{0};
  bool engine_capability_validated{false};
};

struct TypedPhysicalNodeDag {
  std::uint16_t abi_version{1};
  std::string selected_plan_uuid;
  std::uint64_t root_physical_node_id{0};
  std::uint64_t local_transaction_id{0};
  std::uint64_t statement_snapshot_id{0};
  std::vector<PhysicalAdmissionEvidence> admission_evidence;
  std::vector<PhysicalNodeRecord> nodes;
  std::string bound_sblr_tree_uuid;
  std::string catalog_epoch_uuid;
  std::string security_context_uuid;
  std::string capability_snapshot_uuid;
  std::string resource_snapshot_uuid;
  std::string statistics_snapshot_uuid;
  std::string route_snapshot_uuid;
  std::uint64_t catalog_generation{0};
  std::uint64_t security_epoch{0};
  std::uint64_t policy_epoch{0};
  std::uint64_t resource_epoch{0};
  std::uint64_t statistics_generation{0};
  std::uint64_t route_epoch{0};
  std::uint64_t route_generation{0};
  std::uint64_t memory_budget_bytes{0};
  bool spill_allowed{false};
  bool optimizer_published{false};
  bool immutable_node_identity_validated{false};
  bool capability_validated_before_access{false};
  bool data_access_observed{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
};

struct PhysicalNodeAbiLimits {
  std::size_t maximum_nodes{131072};
  std::size_t maximum_depth{256};
  std::size_t maximum_fanout{1024};
};

struct PhysicalNodeAbiValidationIssue {
  std::string diagnostic_id;
  std::uint64_t physical_node_id{0};
  std::string field_id;
};

struct PhysicalNodeAbiValidationResult {
  bool accepted{false};
  std::size_t validated_node_count{0};
  std::size_t maximum_observed_depth{0};
  std::vector<PhysicalNodeAbiValidationIssue> issues;
};

// QOW-SOURCE-QRY-004-NODE-ABI-V1
inline PhysicalNodeAbiValidationResult ValidateTypedPhysicalNodeDag(
    const TypedPhysicalNodeDag& dag,
    const PhysicalNodeAbiLimits& limits = {}) {
  PhysicalNodeAbiValidationResult result;
  const auto refuse = [&](std::string diagnostic_id,
                          const std::uint64_t physical_node_id,
                          std::string field_id) {
    result.accepted = false;
    result.issues.push_back({std::move(diagnostic_id), physical_node_id,
                             std::move(field_id)});
    return result;
  };
  const auto canonical_uuid = [](const std::string_view value) {
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
  };
  const auto known_kind = [](const PhysicalNodeKind kind) {
    return kind >= PhysicalNodeKind::kScan &&
           kind <= PhysicalNodeKind::kTableFunctionInvoke;
  };
  const auto valid_implementation_id = [](const std::string_view value) {
    if (value.empty() || value.size() > 128) return false;
    return std::ranges::all_of(value, [](const unsigned char ch) {
      return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
             ch == '.' || ch == '_' || ch == '-';
    });
  };

  if (dag.abi_version != 1 && dag.abi_version != 2) {
    return refuse("QOW-DIAG-PHYSICAL-NODE-ABI-VERSION", 0,
                  "abi_version");
  }
  if (!canonical_uuid(dag.selected_plan_uuid)) {
    return refuse("QOW-DIAG-PHYSICAL-NODE-ABI-ADMISSION", 0,
                  "selected_plan_uuid");
  }
  if (dag.local_transaction_id == 0 || dag.statement_snapshot_id == 0) {
    return refuse("QOW-DIAG-PHYSICAL-NODE-ABI-ADMISSION", 0,
                  "mga_statement_context");
  }
  const bool optimizer_publication_v2 = dag.abi_version == 2;
  if (optimizer_publication_v2 &&
      (!canonical_uuid(dag.bound_sblr_tree_uuid) ||
       !canonical_uuid(dag.catalog_epoch_uuid) ||
       !canonical_uuid(dag.security_context_uuid) ||
       !canonical_uuid(dag.capability_snapshot_uuid) ||
       !canonical_uuid(dag.resource_snapshot_uuid) ||
       !canonical_uuid(dag.statistics_snapshot_uuid) ||
       !canonical_uuid(dag.route_snapshot_uuid) ||
       dag.catalog_generation == 0 || dag.security_epoch == 0 ||
       dag.policy_epoch == 0 || dag.resource_epoch == 0 ||
       dag.statistics_generation == 0 || dag.route_epoch == 0 ||
       dag.route_generation == 0 || dag.memory_budget_bytes == 0 ||
       !dag.optimizer_published ||
       !dag.immutable_node_identity_validated ||
       !dag.capability_validated_before_access || dag.data_access_observed ||
       dag.parser_execution_authority_claimed ||
       dag.transaction_finality_authority_claimed)) {
    return refuse("QOW-DIAG-PHYSICAL-NODE-ABI-PUBLICATION", 0,
                  "optimizer_publication_scope");
  }
  constexpr std::size_t kAdmissionStageCount = 8;
  if (dag.admission_evidence.size() != kAdmissionStageCount) {
    return refuse("QOW-DIAG-PHYSICAL-NODE-ABI-ADMISSION", 0,
                  "admission_evidence");
  }
  std::unordered_set<std::string> admission_evidence_uuids;
  std::vector<std::string_view> expected_publication_evidence;
  if (optimizer_publication_v2) {
    expected_publication_evidence = {
        dag.bound_sblr_tree_uuid, dag.catalog_epoch_uuid,
        dag.security_context_uuid, dag.catalog_epoch_uuid,
        dag.capability_snapshot_uuid, dag.resource_snapshot_uuid,
        dag.statistics_snapshot_uuid, dag.route_snapshot_uuid};
  }
  for (std::size_t index = 0; index < kAdmissionStageCount; ++index) {
    const auto expected =
        static_cast<PhysicalAdmissionStage>(index + 1);
    const auto& evidence = dag.admission_evidence[index];
    if (evidence.stage != expected ||
        !canonical_uuid(evidence.evidence_uuid) ||
        (optimizer_publication_v2
             ? evidence.evidence_uuid != expected_publication_evidence[index]
             : !admission_evidence_uuids.insert(evidence.evidence_uuid)
                    .second)) {
      return refuse("QOW-DIAG-PHYSICAL-NODE-ABI-ADMISSION", 0,
                    "admission_order_or_evidence");
    }
  }
  if (limits.maximum_nodes == 0 || limits.maximum_depth == 0 ||
      limits.maximum_fanout == 0 || dag.nodes.empty() ||
      dag.nodes.size() > limits.maximum_nodes) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT", 0, "node_count");
  }

  std::unordered_map<std::uint64_t, const PhysicalNodeRecord*> nodes_by_id;
  std::unordered_map<std::uint64_t, std::size_t> incoming_reference_count;
  std::unordered_set<std::uint64_t> causal_counter_ids;
  for (const auto& node : dag.nodes) {
    if (node.physical_node_id == 0 || node.relational_node_id == 0 ||
        !known_kind(node.node_kind) ||
        !valid_implementation_id(node.implementation_id) ||
        node.causal_counter_id == 0 ||
        !causal_counter_ids.insert(node.causal_counter_id).second ||
        !nodes_by_id.emplace(node.physical_node_id, &node).second) {
      return refuse("SBLR.PLAN_TREE.INVALID_HANDLE", node.physical_node_id,
                    "physical_node_record");
    }
    if (optimizer_publication_v2) {
      std::unordered_set<std::string> required_properties;
      std::unordered_set<std::string> delivered_properties;
      const auto valid_properties = [&](const auto& properties,
                                        auto* unique) {
        return std::ranges::all_of(properties, [&](const auto& property_uuid) {
          return canonical_uuid(property_uuid) &&
                 unique->insert(property_uuid).second;
        });
      };
      if (!canonical_uuid(node.selected_alternative_uuid) ||
          !canonical_uuid(node.executor_capability_uuid) ||
          node.executor_capability_abi_version != 1 ||
          !canonical_uuid(node.cost_vector_uuid) ||
          !valid_properties(node.required_property_uuids,
                            &required_properties) ||
          !valid_properties(node.delivered_property_uuids,
                            &delivered_properties) ||
          node.memory_bytes_required > dag.memory_budget_bytes ||
          (!dag.spill_allowed && node.spill_bytes_expected != 0) ||
          !node.engine_capability_validated) {
        return refuse("QOW-DIAG-PHYSICAL-NODE-ABI-CAPABILITY",
                      node.physical_node_id,
                      "selected_node_capability_contract");
      }
    }
    if (node.input_physical_node_ids.size() > limits.maximum_fanout) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT", node.physical_node_id,
                    "input_physical_node_ids");
    }
    std::unordered_set<std::uint32_t> descriptor_ids;
    for (const auto descriptor_id : node.output_descriptor_ids) {
      if (descriptor_id == 0 ||
          !descriptor_ids.insert(descriptor_id).second) {
        return refuse("SBLR.PLAN_TREE.INVALID_HANDLE", node.physical_node_id,
                      "output_descriptor_ids");
      }
    }
    if (node.output_descriptor_ids.empty()) {
      return refuse("SBLR.PLAN_TREE.INVALID_HANDLE", node.physical_node_id,
                    "output_descriptor_ids");
    }
  }
  if (dag.root_physical_node_id == 0 ||
      !nodes_by_id.contains(dag.root_physical_node_id)) {
    return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                  dag.root_physical_node_id, "root_physical_node_id");
  }
  for (const auto& node : dag.nodes) {
    for (const auto input_id : node.input_physical_node_ids) {
      if (input_id == 0 || !nodes_by_id.contains(input_id)) {
        return refuse("SBLR.PLAN_TREE.INVALID_HANDLE", node.physical_node_id,
                      "input_physical_node_ids");
      }
      ++incoming_reference_count[input_id];
    }
  }
  for (const auto& [node_id, reference_count] : incoming_reference_count) {
    if (reference_count > 1 && !nodes_by_id.at(node_id)->shareable) {
      return refuse("SBLR.PLAN_TREE.INVALID_HANDLE", node_id, "shareable");
    }
  }

  std::unordered_map<std::uint64_t, std::uint8_t> visit_state;
  std::unordered_set<std::uint64_t> reachable;
  std::uint64_t failing_node_id = 0;
  std::string failing_field;
  std::function<bool(std::uint64_t, std::size_t)> visit =
      [&](const std::uint64_t node_id, const std::size_t depth) {
        if (depth > limits.maximum_depth) {
          failing_node_id = node_id;
          failing_field = "maximum_depth";
          return false;
        }
        result.maximum_observed_depth =
            std::max(result.maximum_observed_depth, depth);
        const auto state = visit_state[node_id];
        if (state == 1) {
          failing_node_id = node_id;
          failing_field = "cycle";
          return false;
        }
        if (state == 2) return true;
        visit_state[node_id] = 1;
        reachable.insert(node_id);
        for (const auto input_id :
             nodes_by_id.at(node_id)->input_physical_node_ids) {
          if (!visit(input_id, depth + 1)) return false;
        }
        visit_state[node_id] = 2;
        return true;
      };
  if (!visit(dag.root_physical_node_id, 1)) {
    return refuse(failing_field == "maximum_depth"
                      ? "SBLR.PLAN_TREE.RESOURCE_LIMIT"
                      : "SBLR.PLAN_TREE.INVALID_HANDLE",
                  failing_node_id, failing_field);
  }
  if (reachable.size() != dag.nodes.size()) {
    return refuse("SBLR.PLAN_TREE.INVALID_HANDLE", 0,
                  "orphan_physical_node");
  }

  result.accepted = true;
  result.validated_node_count = reachable.size();
  return result;
}

}  // namespace scratchbird::engine::executor
