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
};

struct TypedPhysicalNodeDag {
  std::uint16_t abi_version{1};
  std::string selected_plan_uuid;
  std::uint64_t root_physical_node_id{0};
  std::uint64_t local_transaction_id{0};
  std::uint64_t statement_snapshot_id{0};
  std::vector<PhysicalAdmissionEvidence> admission_evidence;
  std::vector<PhysicalNodeRecord> nodes;
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

  if (dag.abi_version != 1) {
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
  constexpr std::size_t kAdmissionStageCount = 8;
  if (dag.admission_evidence.size() != kAdmissionStageCount) {
    return refuse("QOW-DIAG-PHYSICAL-NODE-ABI-ADMISSION", 0,
                  "admission_evidence");
  }
  std::unordered_set<std::string> admission_evidence_uuids;
  for (std::size_t index = 0; index < kAdmissionStageCount; ++index) {
    const auto expected =
        static_cast<PhysicalAdmissionStage>(index + 1);
    const auto& evidence = dag.admission_evidence[index];
    if (evidence.stage != expected ||
        !canonical_uuid(evidence.evidence_uuid) ||
        !admission_evidence_uuids.insert(evidence.evidence_uuid).second) {
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
