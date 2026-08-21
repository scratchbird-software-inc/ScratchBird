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
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::executor {

struct PhysicalMgaStatementContext {
  std::string statement_uuid;
  std::string owning_transaction_uuid;
  std::string statement_snapshot_uuid;
  std::string statement_metadata_snapshot_uuid;
  std::uint64_t owning_local_transaction_id{0};
  std::uint64_t visible_committed_high_watermark{0};
  std::uint64_t oldest_active_transaction_id{0};
  std::uint64_t oldest_interesting_transaction_id{0};
  std::uint64_t oldest_snapshot_transaction_id{0};
  std::uint64_t retention_horizon_transaction_id{0};
  std::vector<std::uint64_t> active_excluded_local_transaction_ids;
  std::vector<std::uint64_t> in_doubt_excluded_local_transaction_ids;
  std::string snapshot_kind;
  std::uint64_t publication_inventory_next_local_transaction_id{0};
  bool inventory_authoritative{false};
  bool complete{false};
  bool current{false};
  std::string statement_timestamp;
};

inline bool PhysicalMgaStatementContextEqual(
    const PhysicalMgaStatementContext& left,
    const PhysicalMgaStatementContext& right) {
  return left.statement_uuid == right.statement_uuid &&
         left.statement_timestamp == right.statement_timestamp &&
         left.owning_transaction_uuid == right.owning_transaction_uuid &&
         left.statement_snapshot_uuid == right.statement_snapshot_uuid &&
         left.statement_metadata_snapshot_uuid ==
             right.statement_metadata_snapshot_uuid &&
         left.owning_local_transaction_id ==
             right.owning_local_transaction_id &&
         left.visible_committed_high_watermark ==
             right.visible_committed_high_watermark &&
         left.oldest_active_transaction_id ==
             right.oldest_active_transaction_id &&
         left.oldest_interesting_transaction_id ==
             right.oldest_interesting_transaction_id &&
         left.oldest_snapshot_transaction_id ==
             right.oldest_snapshot_transaction_id &&
         left.retention_horizon_transaction_id ==
             right.retention_horizon_transaction_id &&
         left.active_excluded_local_transaction_ids ==
             right.active_excluded_local_transaction_ids &&
         left.in_doubt_excluded_local_transaction_ids ==
             right.in_doubt_excluded_local_transaction_ids &&
         left.snapshot_kind == right.snapshot_kind &&
         left.publication_inventory_next_local_transaction_id ==
             right.publication_inventory_next_local_transaction_id &&
         left.inventory_authoritative == right.inventory_authoritative &&
         left.complete == right.complete && left.current == right.current;
}

inline bool PhysicalMgaStatementContextValid(
    const PhysicalMgaStatementContext& context) {
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
    return value != "00000000-0000-0000-0000-000000000000";
  };
  if (!canonical_uuid(context.statement_uuid) ||
      !canonical_uuid(context.owning_transaction_uuid) ||
      !canonical_uuid(context.statement_snapshot_uuid) ||
      !canonical_uuid(context.statement_metadata_snapshot_uuid) ||
      context.owning_local_transaction_id == 0 ||
      !context.inventory_authoritative || !context.complete ||
      !context.current || context.snapshot_kind != "statement_stable" ||
      context.publication_inventory_next_local_transaction_id == 0 ||
      context.visible_committed_high_watermark >=
          context.publication_inventory_next_local_transaction_id ||
      context.owning_local_transaction_id >=
          context.publication_inventory_next_local_transaction_id ||
      context.oldest_active_transaction_id == 0 ||
      context.oldest_interesting_transaction_id == 0 ||
      context.oldest_snapshot_transaction_id == 0 ||
      context.retention_horizon_transaction_id == 0 ||
      context.oldest_active_transaction_id >=
          context.publication_inventory_next_local_transaction_id ||
      context.oldest_interesting_transaction_id >=
          context.publication_inventory_next_local_transaction_id ||
      context.oldest_snapshot_transaction_id >=
          context.publication_inventory_next_local_transaction_id ||
      context.retention_horizon_transaction_id >=
          context.publication_inventory_next_local_transaction_id ||
      context.oldest_snapshot_transaction_id !=
          context.retention_horizon_transaction_id ||
      context.oldest_snapshot_transaction_id !=
          std::min(context.oldest_interesting_transaction_id,
                   context.owning_local_transaction_id)) {
    return false;
  }
  const auto canonical_exclusions = [&](const auto& values) {
    return std::ranges::is_sorted(values) &&
           std::adjacent_find(values.begin(), values.end()) == values.end() &&
           std::ranges::all_of(values, [&](const std::uint64_t value) {
             return value != 0 &&
                    value <
                        context.publication_inventory_next_local_transaction_id;
           });
  };
  if (!canonical_exclusions(context.active_excluded_local_transaction_ids) ||
      !canonical_exclusions(
          context.in_doubt_excluded_local_transaction_ids) ||
      !std::ranges::binary_search(
          context.active_excluded_local_transaction_ids,
          context.owning_local_transaction_id)) {
    return false;
  }
  std::vector<std::uint64_t> intersection;
  std::ranges::set_intersection(
      context.active_excluded_local_transaction_ids,
      context.in_doubt_excluded_local_transaction_ids,
      std::back_inserter(intersection));
  return intersection.empty();
}

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

// QOW-SOURCE-RCP-065-COMPLETE-PHYSICAL-PUBLICATION-V1
struct PhysicalCostVectorReceipt {
  std::string cost_vector_uuid;
  std::string calibration_profile_uuid;
  std::uint64_t scalar_score{0};
  std::uint64_t cpu_units{0};
  std::uint64_t page_read_sequential_units{0};
  std::uint64_t page_read_random_units{0};
  std::uint64_t page_write_units{0};
  std::uint64_t memory_bytes_required{0};
  std::uint64_t spill_bytes_expected{0};
  std::uint64_t network_bytes_expected{0};
  std::uint64_t mga_visibility_checks_expected{0};
  std::uint64_t archive_fetches_expected{0};
  std::uint64_t uncertainty_penalty{0};
  std::uint64_t risk_penalty{0};
  std::uint8_t confidence{0};
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
  PhysicalMgaStatementContext mga_statement_context;
  std::string logical_semantic_variant_id;
  std::uint64_t publication_ordinal{0};
  std::string transformation_uuid;
  std::string transformation_rule_id;
  std::vector<std::string> enforced_property_uuids;
  PhysicalCostVectorReceipt retained_cost;
  // Dispatcher-owned, callback-local resource allowance. Optimizer-published
  // DAG records keep zero; the dispatcher sets it only on the transient node
  // copy passed to an executor after accounting unrelated retained payloads.
  std::uint64_t dispatcher_callback_memory_limit_bytes{0};
};

struct TypedPhysicalNodeDag {
  std::uint16_t abi_version{1};
  std::string selected_plan_uuid;
  std::uint64_t root_physical_node_id{0};
  std::uint64_t local_transaction_id{0};
  std::uint64_t statement_snapshot_id{0};
  PhysicalMgaStatementContext mga_statement_context;
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
  std::uint16_t publication_contract_version{0};
  std::string selected_plan_signature;
  std::uint64_t selected_scalar_score{0};
  std::uint64_t published_node_count{0};
  std::uint64_t first_causal_counter_id{0};
  bool complete_cost_vectors_retained{false};
  bool descriptor_contract_validated{false};
  bool property_contract_validated{false};
  bool dependency_contract_validated{false};
  bool resource_contract_validated{false};
  bool mga_contract_validated{false};
  bool causal_identity_validated{false};
};

inline bool TypedPhysicalNodeDagCarrierIsExactDefault(
    const TypedPhysicalNodeDag& dag) {
  const TypedPhysicalNodeDag empty;
  const auto exact_empty_string = [](const std::string& value,
                                     const std::string& baseline) {
    return value.empty() && value.capacity() == baseline.capacity();
  };
  const auto& context = dag.mga_statement_context;
  const auto& empty_context = empty.mga_statement_context;
  const bool exact_empty_context_storage =
      exact_empty_string(context.statement_uuid,
                         empty_context.statement_uuid) &&
      exact_empty_string(context.owning_transaction_uuid,
                         empty_context.owning_transaction_uuid) &&
      exact_empty_string(context.statement_snapshot_uuid,
                         empty_context.statement_snapshot_uuid) &&
      exact_empty_string(context.statement_metadata_snapshot_uuid,
                         empty_context.statement_metadata_snapshot_uuid) &&
      exact_empty_string(context.snapshot_kind,
                         empty_context.snapshot_kind) &&
      exact_empty_string(context.statement_timestamp,
                         empty_context.statement_timestamp) &&
      context.active_excluded_local_transaction_ids.empty() &&
      context.active_excluded_local_transaction_ids.capacity() ==
          empty_context.active_excluded_local_transaction_ids.capacity() &&
      context.in_doubt_excluded_local_transaction_ids.empty() &&
      context.in_doubt_excluded_local_transaction_ids.capacity() ==
          empty_context.in_doubt_excluded_local_transaction_ids.capacity();
  return dag.abi_version == empty.abi_version &&
         exact_empty_string(dag.selected_plan_uuid,
                            empty.selected_plan_uuid) &&
         dag.root_physical_node_id == empty.root_physical_node_id &&
         dag.local_transaction_id == empty.local_transaction_id &&
         dag.statement_snapshot_id == empty.statement_snapshot_id &&
         PhysicalMgaStatementContextEqual(context, empty_context) &&
         exact_empty_context_storage && dag.admission_evidence.empty() &&
         dag.admission_evidence.capacity() ==
             empty.admission_evidence.capacity() &&
         dag.nodes.empty() && dag.nodes.capacity() == empty.nodes.capacity() &&
         exact_empty_string(dag.bound_sblr_tree_uuid,
                            empty.bound_sblr_tree_uuid) &&
         exact_empty_string(dag.catalog_epoch_uuid,
                            empty.catalog_epoch_uuid) &&
         exact_empty_string(dag.security_context_uuid,
                            empty.security_context_uuid) &&
         exact_empty_string(dag.capability_snapshot_uuid,
                            empty.capability_snapshot_uuid) &&
         exact_empty_string(dag.resource_snapshot_uuid,
                            empty.resource_snapshot_uuid) &&
         exact_empty_string(dag.statistics_snapshot_uuid,
                            empty.statistics_snapshot_uuid) &&
         exact_empty_string(dag.route_snapshot_uuid,
                            empty.route_snapshot_uuid) &&
         dag.catalog_generation == empty.catalog_generation &&
         dag.security_epoch == empty.security_epoch &&
         dag.policy_epoch == empty.policy_epoch &&
         dag.resource_epoch == empty.resource_epoch &&
         dag.statistics_generation == empty.statistics_generation &&
         dag.route_epoch == empty.route_epoch &&
         dag.route_generation == empty.route_generation &&
         dag.memory_budget_bytes == empty.memory_budget_bytes &&
         dag.spill_allowed == empty.spill_allowed &&
         dag.optimizer_published == empty.optimizer_published &&
         dag.immutable_node_identity_validated ==
             empty.immutable_node_identity_validated &&
         dag.capability_validated_before_access ==
             empty.capability_validated_before_access &&
         dag.data_access_observed == empty.data_access_observed &&
         dag.parser_execution_authority_claimed ==
             empty.parser_execution_authority_claimed &&
         dag.transaction_finality_authority_claimed ==
             empty.transaction_finality_authority_claimed &&
         dag.publication_contract_version ==
             empty.publication_contract_version &&
         exact_empty_string(dag.selected_plan_signature,
                            empty.selected_plan_signature) &&
         dag.selected_scalar_score == empty.selected_scalar_score &&
         dag.published_node_count == empty.published_node_count &&
         dag.first_causal_counter_id == empty.first_causal_counter_id &&
         dag.complete_cost_vectors_retained ==
             empty.complete_cost_vectors_retained &&
         dag.descriptor_contract_validated ==
             empty.descriptor_contract_validated &&
         dag.property_contract_validated ==
             empty.property_contract_validated &&
         dag.dependency_contract_validated ==
             empty.dependency_contract_validated &&
         dag.resource_contract_validated ==
             empty.resource_contract_validated &&
         dag.mga_contract_validated == empty.mga_contract_validated &&
         dag.causal_identity_validated == empty.causal_identity_validated;
}

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
    return value != "00000000-0000-0000-0000-000000000000";
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
  const auto checked_add = [](const std::uint64_t left,
                              const std::uint64_t right,
                              std::uint64_t* out) {
    if (out == nullptr ||
        std::numeric_limits<std::uint64_t>::max() - left < right) {
      return false;
    }
    *out = left + right;
    return true;
  };

  if (dag.abi_version != 1 && dag.abi_version != 2) {
    return refuse("QOW-DIAG-PHYSICAL-NODE-ABI-VERSION", 0,
                  "abi_version");
  }
  if (!canonical_uuid(dag.selected_plan_uuid)) {
    return refuse("QOW-DIAG-PHYSICAL-NODE-ABI-ADMISSION", 0,
                  "selected_plan_uuid");
  }
  if (dag.local_transaction_id == 0) {
    return refuse("QOW-DIAG-PHYSICAL-NODE-ABI-ADMISSION", 0,
                  "mga_statement_context");
  }
  const bool optimizer_publication_v2 = dag.abi_version == 2;
  const bool complete_optimizer_publication =
      optimizer_publication_v2 && dag.publication_contract_version == 1;
  if (optimizer_publication_v2 &&
      (!PhysicalMgaStatementContextValid(dag.mga_statement_context) ||
       dag.local_transaction_id !=
           dag.mga_statement_context.owning_local_transaction_id ||
       dag.statement_snapshot_id !=
           dag.mga_statement_context.visible_committed_high_watermark ||
       dag.catalog_epoch_uuid ==
           dag.mga_statement_context.statement_metadata_snapshot_uuid ||
       !canonical_uuid(dag.bound_sblr_tree_uuid) ||
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
  if (complete_optimizer_publication &&
      (dag.selected_plan_signature.empty() ||
       dag.published_node_count == 0 ||
       dag.first_causal_counter_id == 0 ||
       !dag.complete_cost_vectors_retained ||
       !dag.descriptor_contract_validated ||
       !dag.property_contract_validated ||
       !dag.dependency_contract_validated ||
       !dag.resource_contract_validated ||
       !dag.mga_contract_validated ||
       !dag.causal_identity_validated)) {
    return refuse("QOW-DIAG-PHYSICAL-NODE-ABI-PUBLICATION", 0,
                  "complete_publication_contract");
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
        dag.security_context_uuid,
        dag.mga_statement_context.statement_snapshot_uuid,
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
  if (optimizer_publication_v2 && dag.publication_contract_version > 1) {
    return refuse("QOW-DIAG-PHYSICAL-NODE-ABI-PUBLICATION", 0,
                  "publication_contract_version");
  }
  if (limits.maximum_nodes == 0 || limits.maximum_depth == 0 ||
      limits.maximum_fanout == 0 || dag.nodes.empty() ||
      dag.nodes.size() > limits.maximum_nodes ||
      (complete_optimizer_publication &&
       (dag.published_node_count > limits.maximum_nodes ||
        dag.nodes.size() > dag.published_node_count))) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT", 0, "node_count");
  }

  std::unordered_map<std::uint64_t, const PhysicalNodeRecord*> nodes_by_id;
  std::unordered_map<std::uint64_t, std::size_t> incoming_reference_count;
  std::unordered_set<std::uint64_t> causal_counter_ids;
  std::unordered_set<std::uint64_t> publication_ordinals;
  std::uint64_t retained_selected_scalar_score = 0;
  for (const auto& node : dag.nodes) {
    if (node.physical_node_id == 0 || node.relational_node_id == 0 ||
        !known_kind(node.node_kind) ||
        !valid_implementation_id(node.implementation_id) ||
        node.causal_counter_id == 0 ||
        node.dispatcher_callback_memory_limit_bytes != 0 ||
        !causal_counter_ids.insert(node.causal_counter_id).second ||
        !nodes_by_id.emplace(node.physical_node_id, &node).second) {
      return refuse("SBLR.PLAN_TREE.INVALID_HANDLE", node.physical_node_id,
                    "physical_node_record");
    }
    if (optimizer_publication_v2) {
      std::unordered_set<std::string> required_properties;
      std::unordered_set<std::string> delivered_properties;
      std::unordered_set<std::string> enforced_properties;
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
          !node.engine_capability_validated ||
          !PhysicalMgaStatementContextEqual(node.mga_statement_context,
                                            dag.mga_statement_context)) {
        return refuse("QOW-DIAG-PHYSICAL-NODE-ABI-CAPABILITY",
                      node.physical_node_id,
                      "selected_node_capability_contract");
      }
      if (complete_optimizer_publication) {
        const auto& cost = node.retained_cost;
        std::uint64_t retained_scalar_score = 0;
        const auto add_cost = [&](const std::uint64_t term) {
          return checked_add(retained_scalar_score, term,
                             &retained_scalar_score);
        };
        std::uint64_t expected_causal_counter = 0;
        if (node.physical_node_id != node.relational_node_id ||
            !valid_implementation_id(node.logical_semantic_variant_id) ||
            node.publication_ordinal >= dag.published_node_count ||
            !publication_ordinals.insert(node.publication_ordinal).second ||
            !checked_add(dag.first_causal_counter_id,
                         node.publication_ordinal,
                         &expected_causal_counter) ||
            node.causal_counter_id != expected_causal_counter ||
            !canonical_uuid(node.transformation_uuid) ||
            !valid_implementation_id(node.transformation_rule_id) ||
            !valid_properties(node.enforced_property_uuids,
                              &enforced_properties) ||
            !std::ranges::all_of(
                node.enforced_property_uuids,
                [&](const auto& property_uuid) {
                  return delivered_properties.contains(property_uuid);
                }) ||
            cost.cost_vector_uuid != node.cost_vector_uuid ||
            !canonical_uuid(cost.calibration_profile_uuid) ||
            cost.confidence > 3 ||
            cost.memory_bytes_required != node.memory_bytes_required ||
            cost.spill_bytes_expected != node.spill_bytes_expected ||
            !add_cost(cost.cpu_units) ||
            !add_cost(cost.page_read_sequential_units) ||
            !add_cost(cost.page_read_random_units) ||
            !add_cost(cost.page_write_units) ||
            !add_cost(cost.memory_bytes_required) ||
            !add_cost(cost.spill_bytes_expected) ||
            !add_cost(cost.network_bytes_expected) ||
            !add_cost(cost.mga_visibility_checks_expected) ||
            !add_cost(cost.archive_fetches_expected) ||
            !add_cost(cost.uncertainty_penalty) ||
            !add_cost(cost.risk_penalty) ||
            retained_scalar_score != cost.scalar_score ||
            !checked_add(retained_selected_scalar_score,
                         cost.scalar_score,
                         &retained_selected_scalar_score) ||
            (node.node_kind == PhysicalNodeKind::kScan &&
             cost.mga_visibility_checks_expected == 0)) {
          return refuse("QOW-DIAG-PHYSICAL-NODE-ABI-PUBLICATION",
                        node.physical_node_id,
                        "complete_selected_node_contract");
        }
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
  if (complete_optimizer_publication &&
      dag.nodes.size() == dag.published_node_count) {
    std::vector<const PhysicalNodeRecord*> canonical_nodes;
    canonical_nodes.reserve(dag.nodes.size());
    for (const auto& node : dag.nodes) canonical_nodes.push_back(&node);
    std::ranges::sort(canonical_nodes, {},
                      &PhysicalNodeRecord::relational_node_id);
    std::string selected_plan_signature;
    for (const auto* node : canonical_nodes) {
      selected_plan_signature +=
          std::to_string(node->relational_node_id) + "=" +
          node->selected_alternative_uuid + ";";
    }
    if (selected_plan_signature != dag.selected_plan_signature ||
        retained_selected_scalar_score != dag.selected_scalar_score) {
      return refuse("QOW-DIAG-PHYSICAL-NODE-ABI-PUBLICATION", 0,
                    "complete_selected_plan_contract");
    }
    for (const auto& node : dag.nodes) {
      const auto reference_count =
          incoming_reference_count[node.physical_node_id];
      if (node.shareable != (reference_count > 1)) {
        return refuse("QOW-DIAG-PHYSICAL-NODE-ABI-PUBLICATION",
                      node.physical_node_id,
                      "exact_dependency_shareability");
      }
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
