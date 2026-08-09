// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "../executor/descriptor_value_runtime.hpp"
#include "../executor/physical_node_abi.hpp"
#include "optimizer_request.hpp"
#include "result_cursor_plan_memory_governance.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::optimizer {

namespace memory = scratchbird::core::memory;

// QOW-SOURCE-OPT-010-PREPARE-V1
// PREPARE retains the complete immutable physical structure selected by the
// optimizer, but never the PREPARE statement's MGA context, parameter values,
// current-authority resolver, or any execution/finality authority. A later
// EXECUTE must bind a fresh engine-owned statement context and revalidate all
// identities; this artifact alone cannot read a row or dispatch a node.
enum class CanonicalPreparedPlanDependencyKind : std::uint8_t {
  kObject = 1,
  kFunction,
  kIndex,
  kFilespace,
  kDescriptor,
  kDatatype,
  kDomain,
  kCollation,
};

struct CanonicalPreparedPlanParameterDescriptor {
  std::uint32_t ordinal{0};
  std::uint32_t descriptor_id{0};
  std::string descriptor_uuid;
  std::string type_uuid;
  std::string domain_uuid;
  std::string collation_uuid;
  std::string timezone_uuid;
  std::string type_modifier_digest;
  std::string encoded_descriptor;
  bool nullable{false};

  bool operator==(
      const CanonicalPreparedPlanParameterDescriptor&) const = default;
};

struct CanonicalPreparedPlanResultDescriptor {
  std::uint32_t ordinal{0};
  std::uint32_t descriptor_id{0};
  std::string name_utf8;
  std::string descriptor_uuid;
  std::string type_uuid;
  std::string domain_uuid;
  std::string collation_uuid;
  std::string timezone_uuid;
  std::string type_modifier_digest;
  std::string encoded_descriptor;
  bool nullable{false};

  bool operator==(
      const CanonicalPreparedPlanResultDescriptor&) const = default;
};

struct CanonicalPreparedPlanDependency {
  CanonicalPreparedPlanDependencyKind dependency_kind{
      CanonicalPreparedPlanDependencyKind::kObject};
  std::string dependency_uuid;
  std::uint64_t generation{0};
  std::string definition_digest;

  bool operator==(const CanonicalPreparedPlanDependency&) const = default;
};

struct CanonicalPreparedPhysicalNode {
  std::uint64_t physical_node_id{0};
  std::uint32_t relational_node_id{0};
  executor::PhysicalNodeKind node_kind{executor::PhysicalNodeKind::kValues};
  std::string logical_semantic_variant_id;
  std::string implementation_id;
  std::vector<std::uint64_t> input_physical_node_ids;
  std::vector<std::uint32_t> output_descriptor_ids;
  bool shareable{false};
  std::uint64_t publication_ordinal{0};
  std::uint64_t causal_counter_id{0};
  std::string selected_alternative_uuid;
  std::string transformation_uuid;
  std::string transformation_rule_id;
  std::string executor_capability_uuid;
  std::uint32_t executor_capability_abi_version{0};
  std::string cost_vector_uuid;
  std::vector<std::string> required_property_uuids;
  std::vector<std::string> delivered_property_uuids;
  std::vector<std::string> enforced_property_uuids;
  executor::PhysicalCostVectorReceipt retained_cost;
  std::uint64_t memory_bytes_required{0};
  std::uint64_t spill_bytes_expected{0};
};

struct CanonicalPreparedPhysicalPlan {
  std::uint16_t abi_version{1};
  std::string prepared_plan_uuid;
  std::uint64_t prepare_generation{0};
  std::string parameter_shape_uuid;
  std::string result_schema_uuid;
  std::string selected_plan_uuid;
  std::string selected_plan_signature;
  std::uint64_t selected_scalar_score{0};
  std::uint64_t root_physical_node_id{0};
  std::uint64_t published_node_count{0};
  std::uint64_t first_causal_counter_id{0};
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
  std::vector<CanonicalPreparedPlanParameterDescriptor> parameters;
  std::vector<CanonicalPreparedPlanResultDescriptor> result_descriptors;
  std::vector<CanonicalPreparedPlanDependency> dependencies;
  std::vector<CanonicalPreparedPhysicalNode> nodes;
  bool immutable_physical_identity_retained{false};
  bool complete_cost_vectors_retained{false};
  bool parameter_values_retained{false};
  bool prepare_statement_authority_retained{false};
  bool execution_authority_granted{false};
};

struct CanonicalPreparePhysicalPlanRequest {
  std::string prepared_plan_uuid;
  std::uint64_t prepare_generation{0};
  std::string parameter_shape_uuid;
  std::string result_schema_uuid;
  executor::TypedPhysicalNodeDag selected_physical_dag;
  std::vector<CanonicalPreparedPlanParameterDescriptor> parameters;
  std::vector<CanonicalPreparedPlanResultDescriptor> result_descriptors;
  std::vector<CanonicalPreparedPlanDependency> dependencies;
  bool engine_prepare_authorized{false};
  bool parameter_values_supplied{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_visibility_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
};

struct CanonicalPreparePhysicalPlanIssue {
  std::string diagnostic_id;
  std::string field_id;
};

struct CanonicalPreparePhysicalPlanResult {
  bool accepted{false};
  bool prepared{false};
  bool persisted{false};
  bool immutable_physical_identity_retained{false};
  bool complete_parameter_typing_retained{false};
  bool complete_dependency_generations_retained{false};
  bool result_schema_retained{false};
  bool parameter_values_retained{false};
  bool prepare_statement_authority_retained{false};
  bool execution_authority_granted{false};
  std::shared_ptr<const CanonicalPreparedPhysicalPlan> prepared_plan;
  std::vector<CanonicalPreparePhysicalPlanIssue> issues;
};

class CanonicalPreparedPlanStore {
 public:
  std::shared_ptr<const CanonicalPreparedPhysicalPlan> Find(
      const std::string& prepared_plan_uuid) const {
    std::lock_guard lock(mutex_);
    const auto found = plans_.find(prepared_plan_uuid);
    return found == plans_.end() ? nullptr : found->second;
  }

  std::size_t Size() const {
    std::lock_guard lock(mutex_);
    return plans_.size();
  }

 private:
  friend CanonicalPreparePhysicalPlanResult PrepareCanonicalPhysicalPlan(
      const CanonicalPreparePhysicalPlanRequest& request,
      CanonicalPreparedPlanStore* prepared_plan_store);

  bool PersistValidated(
      std::shared_ptr<const CanonicalPreparedPhysicalPlan> prepared_plan) {
    if (!prepared_plan) return false;
    std::lock_guard lock(mutex_);
    return plans_
        .emplace(prepared_plan->prepared_plan_uuid, std::move(prepared_plan))
        .second;
  }

  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<const CanonicalPreparedPhysicalPlan>>
      plans_;
};

inline CanonicalPreparePhysicalPlanResult PrepareCanonicalPhysicalPlan(
    const CanonicalPreparePhysicalPlanRequest& request,
    CanonicalPreparedPlanStore* prepared_plan_store) {
  CanonicalPreparePhysicalPlanResult result;
  const auto refuse = [&](std::string field_id) {
    result = {};
    result.issues.push_back(
        {"QOW-DIAG-OPT-010-PREPARE-REFUSAL-V1", std::move(field_id)});
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
  const auto optional_uuid = [&](const std::string& value) {
    return value.empty() || canonical_uuid(value);
  };
  const auto digest = [](const std::string_view value) {
    return value.size() == 64 &&
           std::ranges::all_of(value, [](const unsigned char ch) {
             return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
  };
  const auto known_dependency_kind = [](const auto kind) {
    return kind >= CanonicalPreparedPlanDependencyKind::kObject &&
           kind <= CanonicalPreparedPlanDependencyKind::kCollation;
  };

  if (!request.engine_prepare_authorized || request.parameter_values_supplied ||
      request.parser_execution_authority_claimed ||
      request.transaction_visibility_authority_claimed ||
      request.transaction_finality_authority_claimed ||
      request.recovery_authority_claimed) {
    return refuse("prepare_authority_scope");
  }
  if (!canonical_uuid(request.prepared_plan_uuid) ||
      request.prepare_generation == 0 ||
      !canonical_uuid(request.parameter_shape_uuid) ||
      !canonical_uuid(request.result_schema_uuid)) {
    return refuse("prepared_plan_identity");
  }
  const auto dag_validation =
      executor::ValidateTypedPhysicalNodeDag(request.selected_physical_dag);
  const auto& dag = request.selected_physical_dag;
  if (!dag_validation.accepted || dag.abi_version != 2 ||
      dag.publication_contract_version != 1 || !dag.optimizer_published ||
      !dag.immutable_node_identity_validated ||
      !dag.capability_validated_before_access || dag.data_access_observed ||
      dag.parser_execution_authority_claimed ||
      dag.transaction_finality_authority_claimed ||
      dag.nodes.size() != dag.published_node_count ||
      !dag.complete_cost_vectors_retained ||
      !dag.descriptor_contract_validated || !dag.property_contract_validated ||
      !dag.dependency_contract_validated || !dag.resource_contract_validated ||
      !dag.mga_contract_validated || !dag.causal_identity_validated) {
    return refuse(dag_validation.accepted
                      ? "complete_immutable_physical_publication"
                      : dag_validation.issues.front().field_id);
  }

  std::unordered_set<std::uint32_t> parameter_descriptor_ids;
  std::unordered_set<std::string> parameter_descriptor_uuids;
  for (std::size_t index = 0; index < request.parameters.size(); ++index) {
    const auto& parameter = request.parameters[index];
    if (parameter.ordinal != index + 1 || parameter.descriptor_id == 0 ||
        !parameter_descriptor_ids.insert(parameter.descriptor_id).second ||
        !canonical_uuid(parameter.descriptor_uuid) ||
        !parameter_descriptor_uuids.insert(parameter.descriptor_uuid).second ||
        !canonical_uuid(parameter.type_uuid) ||
        !optional_uuid(parameter.domain_uuid) ||
        !optional_uuid(parameter.collation_uuid) ||
        !optional_uuid(parameter.timezone_uuid) ||
        !digest(parameter.type_modifier_digest)) {
      return refuse("typed_parameter_descriptor");
    }
  }

  const auto root = std::ranges::find_if(
      dag.nodes, [&](const auto& node) {
        return node.physical_node_id == dag.root_physical_node_id;
      });
  if (root == dag.nodes.end() ||
      request.result_descriptors.size() !=
          root->output_descriptor_ids.size()) {
    return refuse("result_descriptor_coverage");
  }
  std::unordered_set<std::string> result_descriptor_uuids;
  for (std::size_t index = 0; index < request.result_descriptors.size();
       ++index) {
    const auto& descriptor = request.result_descriptors[index];
    if (descriptor.ordinal != index + 1 || descriptor.descriptor_id == 0 ||
        descriptor.descriptor_id != root->output_descriptor_ids[index] ||
        !canonical_uuid(descriptor.descriptor_uuid) ||
        !result_descriptor_uuids.insert(descriptor.descriptor_uuid).second ||
        !canonical_uuid(descriptor.type_uuid) ||
        !optional_uuid(descriptor.domain_uuid) ||
        !optional_uuid(descriptor.collation_uuid) ||
        !optional_uuid(descriptor.timezone_uuid) ||
        !digest(descriptor.type_modifier_digest)) {
      return refuse("typed_result_descriptor");
    }
  }

  if (request.dependencies.empty()) {
    return refuse("generation_qualified_dependencies");
  }
  std::string previous_dependency_key;
  for (const auto& dependency : request.dependencies) {
    const auto key =
        std::to_string(static_cast<std::uint8_t>(dependency.dependency_kind)) +
        ":" + dependency.dependency_uuid;
    if (!known_dependency_kind(dependency.dependency_kind) ||
        !canonical_uuid(dependency.dependency_uuid) ||
        dependency.generation == 0 ||
        !digest(dependency.definition_digest) ||
        (!previous_dependency_key.empty() && key <= previous_dependency_key)) {
      return refuse("generation_qualified_dependencies");
    }
    previous_dependency_key = key;
  }

  auto prepared = std::make_shared<CanonicalPreparedPhysicalPlan>();
  prepared->prepared_plan_uuid = request.prepared_plan_uuid;
  prepared->prepare_generation = request.prepare_generation;
  prepared->parameter_shape_uuid = request.parameter_shape_uuid;
  prepared->result_schema_uuid = request.result_schema_uuid;
  prepared->selected_plan_uuid = dag.selected_plan_uuid;
  prepared->selected_plan_signature = dag.selected_plan_signature;
  prepared->selected_scalar_score = dag.selected_scalar_score;
  prepared->root_physical_node_id = dag.root_physical_node_id;
  prepared->published_node_count = dag.published_node_count;
  prepared->first_causal_counter_id = dag.first_causal_counter_id;
  prepared->bound_sblr_tree_uuid = dag.bound_sblr_tree_uuid;
  prepared->catalog_epoch_uuid = dag.catalog_epoch_uuid;
  prepared->security_context_uuid = dag.security_context_uuid;
  prepared->capability_snapshot_uuid = dag.capability_snapshot_uuid;
  prepared->resource_snapshot_uuid = dag.resource_snapshot_uuid;
  prepared->statistics_snapshot_uuid = dag.statistics_snapshot_uuid;
  prepared->route_snapshot_uuid = dag.route_snapshot_uuid;
  prepared->catalog_generation = dag.catalog_generation;
  prepared->security_epoch = dag.security_epoch;
  prepared->policy_epoch = dag.policy_epoch;
  prepared->resource_epoch = dag.resource_epoch;
  prepared->statistics_generation = dag.statistics_generation;
  prepared->route_epoch = dag.route_epoch;
  prepared->route_generation = dag.route_generation;
  prepared->memory_budget_bytes = dag.memory_budget_bytes;
  prepared->spill_allowed = dag.spill_allowed;
  prepared->parameters = request.parameters;
  prepared->result_descriptors = request.result_descriptors;
  prepared->dependencies = request.dependencies;
  prepared->nodes.reserve(dag.nodes.size());
  for (const auto& node : dag.nodes) {
    prepared->nodes.push_back(
        {node.physical_node_id,
         node.relational_node_id,
         node.node_kind,
         node.logical_semantic_variant_id,
         node.implementation_id,
         node.input_physical_node_ids,
         node.output_descriptor_ids,
         node.shareable,
         node.publication_ordinal,
         node.causal_counter_id,
         node.selected_alternative_uuid,
         node.transformation_uuid,
         node.transformation_rule_id,
         node.executor_capability_uuid,
         node.executor_capability_abi_version,
         node.cost_vector_uuid,
         node.required_property_uuids,
         node.delivered_property_uuids,
         node.enforced_property_uuids,
         node.retained_cost,
         node.memory_bytes_required,
         node.spill_bytes_expected});
  }
  prepared->immutable_physical_identity_retained = true;
  prepared->complete_cost_vectors_retained = true;
  prepared->parameter_values_retained = false;
  prepared->prepare_statement_authority_retained = false;
  prepared->execution_authority_granted = false;

  if (prepared_plan_store == nullptr ||
      !prepared_plan_store->PersistValidated(prepared)) {
    return refuse("prepared_plan_store");
  }

  result.accepted = true;
  result.prepared = true;
  result.persisted = true;
  result.immutable_physical_identity_retained = true;
  result.complete_parameter_typing_retained = true;
  result.complete_dependency_generations_retained = true;
  result.result_schema_retained = true;
  result.parameter_values_retained = false;
  result.prepare_statement_authority_retained = false;
  result.execution_authority_granted = false;
  result.prepared_plan = std::move(prepared);
  return result;
}

// QOW-SOURCE-OPT-009-V1
// This is the executable cache authority. It is intentionally distinct from
// CachedOptimizerPlan below, which remains optimizer metadata only. A cache
// entry owns stable plan/key identity, never a PREPARE or EXECUTE statement
// snapshot, parameter value, current-authority resolver, or transaction
// finality capability.
enum class CanonicalExecutablePlanSnapshotClass : std::uint8_t {
  kReadCommitted = 1,
  kSnapshot,
  kSerializable,
  kHistorical,
  kClusterFinal,
  kBranchLocal,
  kDonorProfiled,
};

enum class CanonicalExecutablePlanStatus : std::uint8_t {
  kValid = 1,
  kRequiresRevalidation,
  kInvalid,
  kRetired,
  kBlocked,
};

struct CanonicalExecutablePlanGeneration {
  std::string identity_uuid;
  std::uint64_t generation{0};
  std::string definition_digest;

  bool operator==(const CanonicalExecutablePlanGeneration&) const = default;
};

struct CanonicalExecutablePlanCapabilityGeneration {
  std::string capability_uuid;
  std::uint32_t abi_version{0};
  std::uint64_t generation{0};
  std::string definition_digest;

  bool operator==(
      const CanonicalExecutablePlanCapabilityGeneration&) const = default;
};

struct CanonicalExecutablePlanCacheKey {
  std::string cache_plan_uuid;
  std::string compiled_at_uuidv7;
  CanonicalExecutablePlanStatus plan_status{
      CanonicalExecutablePlanStatus::kValid};
  std::string plan_key_digest;
  std::string database_uuid;
  std::uint64_t engine_format_generation{0};
  std::string sblr_unit_uuid;
  std::string internal_procedure_uuid;
  std::string bound_sblr_tree_uuid;
  std::string parser_compatibility_profile_uuid;
  std::uint64_t parser_compatibility_generation{0};
  std::string donor_compatibility_profile_uuid;
  std::uint64_t donor_compatibility_generation{0};
  std::string plan_policy_profile_uuid;
  std::uint64_t optimizer_configuration_generation{0};
  std::string bound_object_set_digest;
  std::string security_policy_digest;
  std::string redaction_policy_digest;
  std::string resource_policy_digest;
  std::uint64_t filespace_placement_generation{0};
  CanonicalExecutablePlanSnapshotClass snapshot_class{
      CanonicalExecutablePlanSnapshotClass::kReadCommitted};
  bool standalone_database{true};
  std::string cluster_uuid;
  std::uint64_t cluster_epoch{0};

  std::string prepared_plan_uuid;
  std::uint64_t prepare_generation{0};
  std::string parameter_shape_uuid;
  std::string result_schema_uuid;
  std::string selected_plan_uuid;
  std::string selected_plan_signature;
  std::uint64_t selected_scalar_score{0};
  std::uint64_t root_physical_node_id{0};
  std::uint64_t published_node_count{0};
  std::uint64_t first_causal_counter_id{0};

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

  std::vector<CanonicalPreparedPlanParameterDescriptor> parameters;
  std::vector<CanonicalPreparedPlanResultDescriptor> result_descriptors;
  std::vector<CanonicalPreparedPlanDependency> physical_dependencies;
  std::vector<CanonicalExecutablePlanGeneration> object_generations;
  std::vector<CanonicalExecutablePlanGeneration> function_generations;
  std::vector<CanonicalExecutablePlanGeneration> metadata_generations;
  std::vector<CanonicalExecutablePlanGeneration> datatype_generations;
  std::vector<CanonicalExecutablePlanGeneration> collation_generations;
  std::vector<CanonicalExecutablePlanGeneration> statistics_generations;
  std::vector<CanonicalExecutablePlanGeneration> index_generations;
  std::vector<CanonicalExecutablePlanGeneration> filespace_generations;
  std::vector<CanonicalExecutablePlanGeneration> route_generations;
  std::vector<CanonicalExecutablePlanCapabilityGeneration>
      capability_generations;

  bool operator==(const CanonicalExecutablePlanCacheKey&) const = default;
};

struct CanonicalExecutablePlanParameterBinding {
  CanonicalPreparedPlanParameterDescriptor descriptor;
  const scratchbird::engine::internal_api::EngineTypedValue* typed_value{
      nullptr};
};

struct CanonicalExecutablePlanCacheIssue {
  std::string diagnostic_id;
  std::string field_id;
  std::string upstream_diagnostic_id;
  std::string upstream_field_id;
};

struct CanonicalExecutablePlanCacheEntry {
  CanonicalExecutablePlanCacheKey key;
  std::shared_ptr<const CanonicalPreparedPhysicalPlan> prepared_plan;
  bool executable{false};
  bool metadata_only{true};
  bool parameter_values_retained{false};
  bool prepare_statement_authority_retained{false};
  bool execution_statement_authority_retained{false};
  bool transaction_finality_authority_granted{false};
};

struct CanonicalExecutablePlanCacheAdmissionRequest {
  CanonicalExecutablePlanCacheKey key;
  bool engine_cache_admission_authorized{false};
  bool metadata_only_entry{false};
  bool parameter_values_supplied{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_visibility_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
};

struct CanonicalExecutablePlanCacheAdmissionResult {
  bool accepted{false};
  bool cached{false};
  std::shared_ptr<const CanonicalExecutablePlanCacheEntry> entry;
  std::vector<CanonicalExecutablePlanCacheIssue> issues;
};

struct CanonicalExecutablePlanCacheLookupRequest {
  CanonicalExecutablePlanCacheKey current_key;
  std::vector<CanonicalExecutablePlanParameterBinding> parameter_bindings;
  executor::CanonicalExecutionMgaAuthority mga_authority;
  bool engine_lookup_authorized{false};
  bool engine_security_revalidated{false};
  bool engine_policy_revalidated{false};
  bool engine_authorization_revalidated{false};
  std::string authorization_revalidation_receipt_uuid;
  bool parser_execution_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
};

struct CanonicalExecutablePlanCacheHitReceipt {
  std::string plan_key_digest;
  std::string prepared_plan_uuid;
  std::string selected_plan_uuid;
  std::uint64_t root_physical_node_id{0};
  std::vector<std::uint64_t> physical_node_ids;
  std::vector<std::uint64_t> causal_counter_ids;
  std::string parameter_shape_uuid;
  std::string result_schema_uuid;
  std::size_t transient_parameter_value_count{0};
  bool immutable_stored_plan_unchanged{false};
  bool fresh_engine_mga_statement_bound{false};
  bool parameter_values_retained{false};
  bool structural_no_optimizer_search_planner_or_fallback_route{false};
  std::uint64_t optimizer_invocation_count{0};
  std::uint64_t search_invocation_count{0};
  std::uint64_t planner_invocation_count{0};
  std::uint64_t uncached_fallback_invocation_count{0};
  executor::PhysicalMgaStatementContext mga_statement_context;
};

struct CanonicalExecutablePlanCacheLookupResult {
  bool accepted{false};
  bool hit{false};
  bool reprepare_required{false};
  executor::TypedPhysicalNodeDag execution_physical_dag;
  std::shared_ptr<const CanonicalExecutablePlanCacheEntry> entry;
  CanonicalExecutablePlanCacheHitReceipt receipt;
  std::vector<CanonicalExecutablePlanCacheIssue> issues;
};

inline bool CanonicalExecutablePlanUuid(const std::string_view value) {
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
}

inline bool CanonicalExecutablePlanDigest(const std::string_view value) {
  return value.size() == 64 &&
         std::ranges::all_of(value, [](const unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

inline bool CanonicalExecutablePlanUuidV7(const std::string_view value) {
  if (!CanonicalExecutablePlanUuid(value) || value[14] != '7') return false;
  return value[19] == '8' || value[19] == '9' || value[19] == 'a' ||
         value[19] == 'b';
}

inline bool CanonicalExecutablePlanGenerationVectorValid(
    const std::vector<CanonicalExecutablePlanGeneration>& values,
    const bool empty_allowed) {
  if (!empty_allowed && values.empty()) return false;
  std::string previous;
  for (const auto& value : values) {
    if (!CanonicalExecutablePlanUuid(value.identity_uuid) ||
        value.generation == 0 ||
        !CanonicalExecutablePlanDigest(value.definition_digest) ||
        (!previous.empty() && value.identity_uuid <= previous)) {
      return false;
    }
    previous = value.identity_uuid;
  }
  return true;
}

inline std::vector<CanonicalExecutablePlanGeneration>
CanonicalExecutablePlanDependencyProjection(
    const std::vector<CanonicalPreparedPlanDependency>& dependencies,
    const std::initializer_list<CanonicalPreparedPlanDependencyKind> kinds) {
  std::vector<CanonicalExecutablePlanGeneration> projected;
  for (const auto& dependency : dependencies) {
    if (std::ranges::find(kinds, dependency.dependency_kind) == kinds.end()) {
      continue;
    }
    projected.push_back({dependency.dependency_uuid, dependency.generation,
                         dependency.definition_digest});
  }
  std::ranges::sort(projected, {},
                    &CanonicalExecutablePlanGeneration::identity_uuid);
  return projected;
}

inline bool CanonicalExecutablePlanCapabilityVectorValid(
    const std::vector<CanonicalExecutablePlanCapabilityGeneration>& values,
    const CanonicalPreparedPhysicalPlan& plan) {
  if (values.empty()) return false;
  std::string previous;
  for (const auto& value : values) {
    if (!CanonicalExecutablePlanUuid(value.capability_uuid) ||
        value.abi_version == 0 || value.generation == 0 ||
        !CanonicalExecutablePlanDigest(value.definition_digest) ||
        (!previous.empty() && value.capability_uuid <= previous)) {
      return false;
    }
    previous = value.capability_uuid;
  }
  std::vector<std::pair<std::string, std::uint32_t>> expected;
  for (const auto& node : plan.nodes) {
    expected.emplace_back(node.executor_capability_uuid,
                          node.executor_capability_abi_version);
  }
  std::ranges::sort(expected);
  expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
  if (expected.size() != values.size()) return false;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (values[index].capability_uuid != expected[index].first ||
        values[index].abi_version != expected[index].second) {
      return false;
    }
  }
  return true;
}

inline bool CanonicalExecutablePlanKeyMatchesPreparedPlan(
    const CanonicalExecutablePlanCacheKey& key,
    const CanonicalPreparedPhysicalPlan& plan) {
  const auto known_snapshot_class =
      key.snapshot_class >= CanonicalExecutablePlanSnapshotClass::kReadCommitted &&
      key.snapshot_class <= CanonicalExecutablePlanSnapshotClass::kDonorProfiled;
  const bool statement_source_valid =
      CanonicalExecutablePlanUuid(key.sblr_unit_uuid) !=
      CanonicalExecutablePlanUuid(key.internal_procedure_uuid);
  const bool donor_identity_valid =
      (key.donor_compatibility_profile_uuid.empty() &&
       key.donor_compatibility_generation == 0) ||
      (CanonicalExecutablePlanUuid(key.donor_compatibility_profile_uuid) &&
       key.donor_compatibility_generation != 0);
  const bool cluster_identity_valid =
      (key.standalone_database && key.cluster_uuid.empty() &&
       key.cluster_epoch == 0) ||
      (!key.standalone_database &&
       CanonicalExecutablePlanUuid(key.cluster_uuid) &&
       key.cluster_epoch != 0);
  if (!CanonicalExecutablePlanUuidV7(key.cache_plan_uuid) ||
      !CanonicalExecutablePlanUuidV7(key.compiled_at_uuidv7) ||
      key.plan_status != CanonicalExecutablePlanStatus::kValid ||
      !CanonicalExecutablePlanDigest(key.plan_key_digest) ||
      !CanonicalExecutablePlanUuid(key.database_uuid) ||
      key.engine_format_generation == 0 || !statement_source_valid ||
      !CanonicalExecutablePlanUuid(key.bound_sblr_tree_uuid) ||
      !CanonicalExecutablePlanUuid(key.parser_compatibility_profile_uuid) ||
      key.parser_compatibility_generation == 0 || !donor_identity_valid ||
      !CanonicalExecutablePlanUuid(key.plan_policy_profile_uuid) ||
      key.optimizer_configuration_generation == 0 ||
      !CanonicalExecutablePlanDigest(key.bound_object_set_digest) ||
      !CanonicalExecutablePlanDigest(key.security_policy_digest) ||
      !CanonicalExecutablePlanDigest(key.redaction_policy_digest) ||
      !CanonicalExecutablePlanDigest(key.resource_policy_digest) ||
      key.filespace_placement_generation == 0 ||
      !known_snapshot_class || !cluster_identity_valid ||
      key.prepared_plan_uuid != plan.prepared_plan_uuid ||
      key.prepare_generation != plan.prepare_generation ||
      key.parameter_shape_uuid != plan.parameter_shape_uuid ||
      key.result_schema_uuid != plan.result_schema_uuid ||
      key.selected_plan_uuid != plan.selected_plan_uuid ||
      key.selected_plan_signature != plan.selected_plan_signature ||
      key.selected_scalar_score != plan.selected_scalar_score ||
      key.root_physical_node_id != plan.root_physical_node_id ||
      key.published_node_count != plan.published_node_count ||
      key.first_causal_counter_id != plan.first_causal_counter_id ||
      key.bound_sblr_tree_uuid != plan.bound_sblr_tree_uuid ||
      key.catalog_epoch_uuid != plan.catalog_epoch_uuid ||
      key.security_context_uuid != plan.security_context_uuid ||
      key.capability_snapshot_uuid != plan.capability_snapshot_uuid ||
      key.resource_snapshot_uuid != plan.resource_snapshot_uuid ||
      key.statistics_snapshot_uuid != plan.statistics_snapshot_uuid ||
      key.route_snapshot_uuid != plan.route_snapshot_uuid ||
      key.catalog_generation != plan.catalog_generation ||
      key.security_epoch != plan.security_epoch ||
      key.policy_epoch != plan.policy_epoch ||
      key.resource_epoch != plan.resource_epoch ||
      key.statistics_generation != plan.statistics_generation ||
      key.route_epoch != plan.route_epoch ||
      key.route_generation != plan.route_generation ||
      key.memory_budget_bytes != plan.memory_budget_bytes ||
      key.spill_allowed != plan.spill_allowed ||
      key.parameters != plan.parameters ||
      key.result_descriptors != plan.result_descriptors ||
      key.physical_dependencies != plan.dependencies ||
      plan.abi_version != 1 ||
      !std::ranges::all_of(plan.parameters, [](const auto& descriptor) {
        return !descriptor.encoded_descriptor.empty();
      }) ||
      !std::ranges::all_of(plan.result_descriptors,
                           [](const auto& descriptor) {
                             return !descriptor.name_utf8.empty() &&
                                    !descriptor.encoded_descriptor.empty();
                           }) ||
      !plan.immutable_physical_identity_retained ||
      !plan.complete_cost_vectors_retained || plan.parameter_values_retained ||
      plan.prepare_statement_authority_retained ||
      plan.execution_authority_granted) {
    return false;
  }

  if (!CanonicalExecutablePlanGenerationVectorValid(key.object_generations,
                                                     true) ||
      !CanonicalExecutablePlanGenerationVectorValid(key.function_generations,
                                                     true) ||
      !CanonicalExecutablePlanGenerationVectorValid(key.metadata_generations,
                                                     false) ||
      !CanonicalExecutablePlanGenerationVectorValid(key.datatype_generations,
                                                     true) ||
      !CanonicalExecutablePlanGenerationVectorValid(key.collation_generations,
                                                     true) ||
      !CanonicalExecutablePlanGenerationVectorValid(key.statistics_generations,
                                                     false) ||
      !CanonicalExecutablePlanGenerationVectorValid(key.index_generations,
                                                     true) ||
      !CanonicalExecutablePlanGenerationVectorValid(key.filespace_generations,
                                                     true) ||
      !CanonicalExecutablePlanGenerationVectorValid(key.route_generations,
                                                     false) ||
      !CanonicalExecutablePlanCapabilityVectorValid(key.capability_generations,
                                                     plan)) {
    return false;
  }
  if (key.object_generations != CanonicalExecutablePlanDependencyProjection(
                                    plan.dependencies,
                                    {CanonicalPreparedPlanDependencyKind::kObject}) ||
      key.function_generations != CanonicalExecutablePlanDependencyProjection(
                                      plan.dependencies,
                                      {CanonicalPreparedPlanDependencyKind::kFunction}) ||
      key.datatype_generations != CanonicalExecutablePlanDependencyProjection(
                                      plan.dependencies,
                                      {CanonicalPreparedPlanDependencyKind::kDatatype,
                                       CanonicalPreparedPlanDependencyKind::kDomain}) ||
      key.collation_generations != CanonicalExecutablePlanDependencyProjection(
                                       plan.dependencies,
                                       {CanonicalPreparedPlanDependencyKind::kCollation}) ||
      key.index_generations != CanonicalExecutablePlanDependencyProjection(
                                   plan.dependencies,
                                   {CanonicalPreparedPlanDependencyKind::kIndex}) ||
      key.filespace_generations != CanonicalExecutablePlanDependencyProjection(
                                       plan.dependencies,
                                       {CanonicalPreparedPlanDependencyKind::kFilespace})) {
    return false;
  }
  const auto descriptor_dependencies =
      CanonicalExecutablePlanDependencyProjection(
          plan.dependencies,
          {CanonicalPreparedPlanDependencyKind::kDescriptor});
  if (key.metadata_generations.size() != descriptor_dependencies.size() + 1) {
    return false;
  }
  const auto schema_generation = std::ranges::find_if(
      key.metadata_generations, [&](const auto& value) {
        return value.identity_uuid == plan.result_schema_uuid &&
               value.generation == plan.catalog_generation;
      });
  if (schema_generation == key.metadata_generations.end()) return false;
  for (const auto& dependency : descriptor_dependencies) {
    if (std::ranges::find(key.metadata_generations, dependency) ==
        key.metadata_generations.end()) {
      return false;
    }
  }
  return key.statistics_generations.size() == 1 &&
         key.statistics_generations.front().identity_uuid ==
             plan.statistics_snapshot_uuid &&
         key.statistics_generations.front().generation ==
             plan.statistics_generation &&
         key.route_generations.size() == 1 &&
         key.route_generations.front().identity_uuid ==
             plan.route_snapshot_uuid &&
         key.route_generations.front().generation == plan.route_generation;
}

inline bool CanonicalExecutablePlanParameterBindingsValid(
    const std::vector<CanonicalExecutablePlanParameterBinding>& bindings,
    const CanonicalPreparedPhysicalPlan& plan) {
  if (bindings.size() != plan.parameters.size()) return false;
  for (std::size_t index = 0; index < bindings.size(); ++index) {
    const auto& declared = plan.parameters[index];
    const auto* value = bindings[index].typed_value;
    if (bindings[index].descriptor != declared || value == nullptr ||
        declared.encoded_descriptor.empty() ||
        value->descriptor.descriptor_uuid.canonical !=
            declared.descriptor_uuid ||
        value->descriptor.encoded_descriptor != declared.encoded_descriptor ||
        value->descriptor.encoded_descriptor.find("type_uuid=" +
                                                  declared.type_uuid) ==
            std::string::npos ||
        (value->state !=
             scratchbird::engine::internal_api::EngineValueState::value &&
         value->state != scratchbird::engine::internal_api::
                             EngineValueState::sql_null) ||
        (value->isSqlNull() && !declared.nullable) ||
        (value->isSqlNull() &&
         (!value->encoded_value.empty() || !value->binary_value.empty())) ||
        (!value->isSqlNull() && !value->hasPayload()) ||
        (!value->isSqlNull() &&
         (value->encoded_value.empty() == value->binary_value.empty()))) {
      return false;
    }
  }
  return true;
}

inline executor::TypedPhysicalNodeDag
BindCanonicalExecutablePlanToCurrentStatement(
    const CanonicalPreparedPhysicalPlan& plan,
    const executor::PhysicalMgaStatementContext& statement_context) {
  executor::TypedPhysicalNodeDag dag;
  dag.abi_version = plan.abi_version + 1;
  dag.selected_plan_uuid = plan.selected_plan_uuid;
  dag.root_physical_node_id = plan.root_physical_node_id;
  dag.local_transaction_id = statement_context.owning_local_transaction_id;
  dag.statement_snapshot_id =
      statement_context.visible_committed_high_watermark;
  dag.mga_statement_context = statement_context;
  dag.admission_evidence = {
      {executor::PhysicalAdmissionStage::kBoundRequest,
       plan.bound_sblr_tree_uuid},
      {executor::PhysicalAdmissionStage::kCatalogEpoch,
       plan.catalog_epoch_uuid},
      {executor::PhysicalAdmissionStage::kSecurity,
       plan.security_context_uuid},
      {executor::PhysicalAdmissionStage::kMgaStatementBoundary,
       statement_context.statement_snapshot_uuid},
      {executor::PhysicalAdmissionStage::kPolicyCapability,
       plan.capability_snapshot_uuid},
      {executor::PhysicalAdmissionStage::kResource,
       plan.resource_snapshot_uuid},
      {executor::PhysicalAdmissionStage::kStatisticsProvenance,
       plan.statistics_snapshot_uuid},
      {executor::PhysicalAdmissionStage::kCanonicalRoute,
       plan.route_snapshot_uuid},
  };
  dag.bound_sblr_tree_uuid = plan.bound_sblr_tree_uuid;
  dag.catalog_epoch_uuid = plan.catalog_epoch_uuid;
  dag.security_context_uuid = plan.security_context_uuid;
  dag.capability_snapshot_uuid = plan.capability_snapshot_uuid;
  dag.resource_snapshot_uuid = plan.resource_snapshot_uuid;
  dag.statistics_snapshot_uuid = plan.statistics_snapshot_uuid;
  dag.route_snapshot_uuid = plan.route_snapshot_uuid;
  dag.catalog_generation = plan.catalog_generation;
  dag.security_epoch = plan.security_epoch;
  dag.policy_epoch = plan.policy_epoch;
  dag.resource_epoch = plan.resource_epoch;
  dag.statistics_generation = plan.statistics_generation;
  dag.route_epoch = plan.route_epoch;
  dag.route_generation = plan.route_generation;
  dag.memory_budget_bytes = plan.memory_budget_bytes;
  dag.spill_allowed = plan.spill_allowed;
  dag.optimizer_published = true;
  dag.immutable_node_identity_validated = true;
  dag.capability_validated_before_access = true;
  dag.data_access_observed = false;
  dag.parser_execution_authority_claimed = false;
  dag.transaction_finality_authority_claimed = false;
  dag.publication_contract_version = 1;
  dag.selected_plan_signature = plan.selected_plan_signature;
  dag.selected_scalar_score = plan.selected_scalar_score;
  dag.published_node_count = plan.published_node_count;
  dag.first_causal_counter_id = plan.first_causal_counter_id;
  dag.complete_cost_vectors_retained = plan.complete_cost_vectors_retained;
  dag.descriptor_contract_validated = true;
  dag.property_contract_validated = true;
  dag.dependency_contract_validated = true;
  dag.resource_contract_validated = true;
  dag.mga_contract_validated = true;
  dag.causal_identity_validated = true;
  dag.nodes.reserve(plan.nodes.size());
  for (const auto& prepared : plan.nodes) {
    executor::PhysicalNodeRecord node;
    node.physical_node_id = prepared.physical_node_id;
    node.relational_node_id = prepared.relational_node_id;
    node.node_kind = prepared.node_kind;
    node.implementation_id = prepared.implementation_id;
    node.input_physical_node_ids = prepared.input_physical_node_ids;
    node.output_descriptor_ids = prepared.output_descriptor_ids;
    node.shareable = prepared.shareable;
    node.causal_counter_id = prepared.causal_counter_id;
    node.selected_alternative_uuid = prepared.selected_alternative_uuid;
    node.executor_capability_uuid = prepared.executor_capability_uuid;
    node.executor_capability_abi_version =
        prepared.executor_capability_abi_version;
    node.cost_vector_uuid = prepared.cost_vector_uuid;
    node.required_property_uuids = prepared.required_property_uuids;
    node.delivered_property_uuids = prepared.delivered_property_uuids;
    node.memory_bytes_required = prepared.memory_bytes_required;
    node.spill_bytes_expected = prepared.spill_bytes_expected;
    node.engine_capability_validated = true;
    node.mga_statement_context = statement_context;
    node.logical_semantic_variant_id = prepared.logical_semantic_variant_id;
    node.publication_ordinal = prepared.publication_ordinal;
    node.transformation_uuid = prepared.transformation_uuid;
    node.transformation_rule_id = prepared.transformation_rule_id;
    node.enforced_property_uuids = prepared.enforced_property_uuids;
    node.retained_cost = prepared.retained_cost;
    dag.nodes.push_back(std::move(node));
  }
  return dag;
}

class CanonicalExecutablePlanCache {
 public:
  CanonicalExecutablePlanCacheAdmissionResult Admit(
      const CanonicalPreparedPlanStore& prepared_plan_store,
      const CanonicalExecutablePlanCacheAdmissionRequest& request) {
    CanonicalExecutablePlanCacheAdmissionResult result;
    const auto refuse = [&](std::string field_id) {
      result = {};
      result.issues.push_back(
          {"QOW-DIAG-OPT-009-REFUSAL-V1", std::move(field_id)});
      return result;
    };
    if (!request.engine_cache_admission_authorized ||
        request.metadata_only_entry || request.parameter_values_supplied ||
        request.parser_execution_authority_claimed ||
        request.transaction_visibility_authority_claimed ||
        request.transaction_finality_authority_claimed ||
        request.recovery_authority_claimed) {
      return refuse("cache_admission_authority");
    }
    const auto prepared =
        prepared_plan_store.Find(request.key.prepared_plan_uuid);
    if (!prepared) return refuse("prepared_plan_not_found");
    if (!CanonicalExecutablePlanKeyMatchesPreparedPlan(request.key,
                                                       *prepared)) {
      return refuse("complete_executable_cache_key");
    }
    auto entry = std::make_shared<CanonicalExecutablePlanCacheEntry>();
    entry->key = request.key;
    entry->prepared_plan = prepared;
    entry->executable = true;
    entry->metadata_only = false;
    entry->parameter_values_retained = false;
    entry->prepare_statement_authority_retained = false;
    entry->execution_statement_authority_retained = false;
    entry->transaction_finality_authority_granted = false;
    {
      std::lock_guard lock(mutex_);
      if (entries_.contains(entry->key.prepared_plan_uuid) ||
          cache_plan_uuids_.contains(entry->key.cache_plan_uuid)) {
        return refuse("duplicate_executable_cache_entry");
      }
      entries_.emplace(entry->key.prepared_plan_uuid, entry);
      cache_plan_uuids_.insert(entry->key.cache_plan_uuid);
    }
    result.accepted = true;
    result.cached = true;
    result.entry = std::move(entry);
    return result;
  }

  CanonicalExecutablePlanCacheLookupResult LookupAndBind(
      const CanonicalExecutablePlanCacheLookupRequest& request) const {
    CanonicalExecutablePlanCacheLookupResult result;
    const auto refuse = [&](std::string field_id,
                            const bool reprepare_required = true) {
      result = {};
      result.reprepare_required = reprepare_required;
      result.issues.push_back(
          {"QOW-DIAG-OPT-009-REFUSAL-V1", std::move(field_id)});
      return result;
    };
    if (!request.engine_lookup_authorized ||
        !request.engine_security_revalidated ||
        !request.engine_policy_revalidated ||
        !request.engine_authorization_revalidated ||
        !CanonicalExecutablePlanUuid(
            request.authorization_revalidation_receipt_uuid) ||
        request.parser_execution_authority_claimed ||
        request.transaction_finality_authority_claimed ||
        request.recovery_authority_claimed ||
        request.mga_authority.origin !=
            executor::CanonicalMgaAuthorityOrigin::kEngineTransactionInventory ||
        !request.mga_authority.resolve_current ||
        !executor::PhysicalMgaStatementContextValid(
            request.mga_authority.statement_context)) {
      return refuse("fresh_engine_mga_statement_authority", false);
    }
    std::shared_ptr<const CanonicalExecutablePlanCacheEntry> entry;
    {
      std::lock_guard lock(mutex_);
      const auto found = entries_.find(request.current_key.prepared_plan_uuid);
      if (found == entries_.end()) return refuse("executable_cache_miss");
      entry = found->second;
    }
    if (!entry || !entry->executable || entry->metadata_only ||
        !entry->prepared_plan || entry->parameter_values_retained ||
        entry->prepare_statement_authority_retained ||
        entry->execution_statement_authority_retained ||
        entry->transaction_finality_authority_granted) {
      return refuse("metadata_or_authority_bearing_entry");
    }
    if (!(request.current_key == entry->key) ||
        !CanonicalExecutablePlanKeyMatchesPreparedPlan(
            request.current_key, *entry->prepared_plan)) {
      return refuse("executable_cache_key_mismatch");
    }
    if (!CanonicalExecutablePlanParameterBindingsValid(
            request.parameter_bindings, *entry->prepared_plan)) {
      return refuse("typed_parameter_binding_mismatch", false);
    }
    auto current = request.mga_authority.resolve_current();
    if (!current.diagnostic.ok ||
        !executor::PhysicalMgaStatementContextValid(current.statement_context) ||
        !executor::PhysicalMgaStatementContextEqual(
            current.statement_context,
            request.mga_authority.statement_context)) {
      return refuse("fresh_engine_mga_statement_revalidation", false);
    }
    auto execution_dag = BindCanonicalExecutablePlanToCurrentStatement(
        *entry->prepared_plan, current.statement_context);
    const auto validation = executor::ValidateTypedPhysicalNodeDag(execution_dag);
    if (!validation.accepted) {
      return refuse(validation.issues.empty()
                        ? "reconstructed_physical_dag"
                        : validation.issues.front().field_id,
                    false);
    }
    const auto authority_validation =
        executor::RevalidateCanonicalExecutionMgaAuthority(
            request.mga_authority, execution_dag);
    if (!authority_validation.ok) {
      return refuse("canonical_execution_mga_revalidation:" +
                        authority_validation.diagnostic_code,
                    false);
    }
    result.accepted = true;
    result.hit = true;
    result.entry = entry;
    result.execution_physical_dag = std::move(execution_dag);
    result.receipt.plan_key_digest = entry->key.plan_key_digest;
    result.receipt.prepared_plan_uuid = entry->key.prepared_plan_uuid;
    result.receipt.selected_plan_uuid = entry->key.selected_plan_uuid;
    result.receipt.root_physical_node_id =
        entry->key.root_physical_node_id;
    result.receipt.parameter_shape_uuid = entry->key.parameter_shape_uuid;
    result.receipt.result_schema_uuid = entry->key.result_schema_uuid;
    result.receipt.transient_parameter_value_count =
        request.parameter_bindings.size();
    result.receipt.immutable_stored_plan_unchanged = true;
    result.receipt.fresh_engine_mga_statement_bound = true;
    result.receipt.parameter_values_retained = false;
    result.receipt
        .structural_no_optimizer_search_planner_or_fallback_route = true;
    result.receipt.mga_statement_context = current.statement_context;
    for (const auto& node : entry->prepared_plan->nodes) {
      result.receipt.physical_node_ids.push_back(node.physical_node_id);
      result.receipt.causal_counter_ids.push_back(node.causal_counter_id);
    }
    return result;
  }

  std::size_t Size() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::map<std::string,
           std::shared_ptr<const CanonicalExecutablePlanCacheEntry>>
      entries_;
  std::unordered_set<std::string> cache_plan_uuids_;
};

struct CanonicalExecutablePlanHitExecutionRequest {
  CanonicalExecutablePlanCache* executable_plan_cache{nullptr};
  CanonicalExecutablePlanCacheLookupRequest lookup;
  executor::PhysicalNodeAbiLimits limits;
  executor::CanonicalPhysicalDagRuntimeLimits runtime_limits;
  std::function<bool()> cancellation_requested;
  std::vector<executor::CanonicalPhysicalExecutorRegistration>
      available_executors;
  executor::CanonicalResultPublicationRequest result_publication_request;
  bool engine_execution_authorized{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
};

struct CanonicalExecutablePlanExecutedNodeReceipt {
  std::uint64_t physical_node_id{0};
  std::uint64_t causal_counter_id{0};
  std::size_t execution_ordinal{0};
};

struct CanonicalExecutablePlanHitExecutionResult {
  bool accepted{false};
  bool cache_hit{false};
  bool exact_selected_nodes_executed{false};
  bool canonical_result_published{false};
  bool data_access_observed{false};
  bool reprepare_required{false};
  bool automatic_replan_attempted{false};
  bool parameter_values_retained{false};
  bool structural_no_optimizer_search_planner_or_fallback_route{false};
  std::uint64_t optimizer_invocation_count{0};
  std::uint64_t search_invocation_count{0};
  std::uint64_t planner_invocation_count{0};
  std::uint64_t uncached_fallback_invocation_count{0};
  std::string selected_plan_uuid;
  std::uint64_t executed_root_physical_node_id{0};
  std::string result_schema_uuid;
  std::vector<CanonicalExecutablePlanExecutedNodeReceipt> executed_nodes;
  CanonicalExecutablePlanCacheLookupResult checkout;
  executor::CanonicalPhysicalDagDispatchResult dispatch;
  executor::CanonicalResultPublicationResult result_publication;
  executor::PhysicalMgaStatementContext mga_statement_context;
  std::vector<CanonicalExecutablePlanCacheIssue> issues;
};

// SEARCH_KEY: SB_OPTIMIZER_PLAN_CACHE_KEY
struct OptimizerPlanCacheKeyInput {
  std::string operation_id;
  std::string sblr_digest;
  std::string descriptor_set_digest;
  std::string statistics_snapshot_id;
  std::string catalog_stats_digest;
  std::string cost_profile_id;
  std::string executor_capability_set_id;
  std::string route_capability_digest;
  std::string security_policy_digest;
  std::string redaction_route_digest;
  std::string normalized_optimizer_controls_digest;
  std::string parameter_shape_digest;
  std::string memory_grant_class;
  std::string memory_grant_digest;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t stats_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t name_resolution_epoch = 0;
  std::uint64_t memory_policy_epoch = 0;
  std::uint64_t memory_feedback_generation = 0;
  std::uint64_t compatibility_epoch = 0;
  std::uint64_t format_compatibility_epoch = 0;
  std::uint64_t route_epoch = 0;
  std::vector<std::string> object_uuids;
  std::vector<std::string> function_uuids;
  std::vector<std::string> index_uuids;
  std::vector<std::string> filespace_uuids;
  std::vector<std::string> dependency_digests;
};

struct CachedOptimizerPlan {
  std::string cache_key;
  OptimizerPlanCacheKeyInput key_input;
  BoundOptimizerResult result;
  std::uint64_t created_epoch = 0;
  bool valid = true;
  bool invalidated_by_dependency = false;
  std::string invalidation_diagnostic_code;
  std::string invalidation_event_kind;
  std::string invalidation_dependency_uuid;
  bool metadata_only = true;
  bool mga_visibility_recheck_required = true;
  bool security_recheck_required = true;
  bool parser_or_reference_finality_authority = false;
  bool memory_governed = false;
  std::uint64_t memory_reserved_bytes = 0;
  std::string memory_lease_id;
  memory::ResultCursorPlanMemoryScope memory_scope;
  std::vector<std::string> memory_governance_evidence;
};

struct OptimizerInvalidationEvent {
  std::string event_kind;
  std::string dependency_uuid;
  std::uint64_t event_epoch = 0;
};

struct OptimizerPlanCacheStats {
  std::uint64_t puts = 0;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t invalidations = 0;
};

struct OptimizerPlanCacheLookupResult {
  bool hit = false;
  std::string cache_key;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
  std::optional<CachedOptimizerPlan> plan;
};

struct OptimizerPlanCacheInvalidationResult {
  std::uint64_t invalidated_count = 0;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

// SEARCH_KEY: OEIC_PLAN_CACHE_ENTERPRISE_CLOSURE
struct OptimizerPlanCacheEnterpriseValidation {
  bool ok = false;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

struct OptimizerPlanCacheMemoryGovernanceRequest {
  memory::ResultCursorPlanMemoryGovernor* governor = nullptr;
  memory::HierarchicalMemoryBudgetLedger* ledger = nullptr;
  memory::ResultCursorPlanMemoryPolicy policy;
  memory::ResultCursorPlanMemoryScope scope;
  memory::ResultCursorPlanMemoryEpochs epochs;
  memory::HierarchicalMemoryBudgetProvenance provenance;
  std::uint64_t estimated_plan_bytes = 0;
  bool cluster_route_requested = false;
};

struct OptimizerPlanCachePersistenceRequest {
  std::string storage_scope_uuid;
  std::string persisted_by_principal_uuid;
  std::uint64_t persisted_epoch = 0;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t stats_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t route_epoch = 0;
  std::uint64_t memory_policy_epoch = 0;
  std::uint64_t memory_feedback_generation = 0;
  bool durable_catalog_persistence = true;
  bool mga_transaction_committed = true;
  bool security_redaction_evidence_present = true;
  bool fixture_or_test_only = false;
  bool cluster_route_projection_present = false;
};

struct OptimizerPlanCachePersistenceEnvelope {
  std::uint32_t schema_version = 1;
  std::string persistence_source = "engine_optimizer_plan_cache_catalog";
  OptimizerPlanCachePersistenceRequest request;
  std::vector<CachedOptimizerPlan> plans;
  std::string envelope_digest_algorithm = "sha256-v1";
  std::string envelope_digest;
  bool ok = false;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

// SEARCH_KEY: SB_OPTIMIZER_PRODUCTION_PLAN_CACHE_KEY_BUILDER
// Production cache keys require caller-supplied route, redaction, parameter,
// memory, dependency, and cost-profile digests. The compatibility builder is
// intentionally rejected by enterprise validation when it falls back to local
// defaults or unbound parameter placeholders.
struct OptimizerProductionPlanCacheKeyRequest {
  BoundOptimizerRequest bound_request;
  std::string catalog_stats_digest;
  std::string cost_profile_id;
  std::string route_capability_digest;
  std::string security_policy_digest;
  std::string redaction_route_digest;
  std::string parameter_shape_digest;
  std::string memory_grant_class;
  std::string memory_grant_digest;
  std::uint64_t compatibility_epoch = 0;
  std::uint64_t format_compatibility_epoch = 0;
  std::vector<std::string> object_uuids;
  std::vector<std::string> function_uuids;
  std::vector<std::string> index_uuids;
  std::vector<std::string> filespace_uuids;
  std::vector<std::string> dependency_digests;
  bool cluster_route_requested = false;
  bool parser_or_reference_authority_claimed = false;
};

struct OptimizerProductionPlanCacheKeyResult {
  bool ok = false;
  std::string diagnostic_code;
  OptimizerPlanCacheKeyInput input;
  std::vector<std::string> evidence;
};

class OptimizerPlanCache {
 public:
  void Put(CachedOptimizerPlan plan);
  OptimizerPlanCacheEnterpriseValidation PutEnterprise(CachedOptimizerPlan plan);
  OptimizerPlanCacheEnterpriseValidation PutEnterpriseGoverned(
      CachedOptimizerPlan plan,
      OptimizerPlanCacheMemoryGovernanceRequest governance);
  std::optional<CachedOptimizerPlan> Get(const std::string& cache_key);
  OptimizerPlanCacheLookupResult Lookup(const OptimizerPlanCacheKeyInput& input);
  OptimizerPlanCacheLookupResult LookupEnterprise(const OptimizerPlanCacheKeyInput& input);
  std::uint64_t Invalidate(const OptimizerInvalidationEvent& event);
  OptimizerPlanCacheInvalidationResult InvalidateWithEvidence(const OptimizerInvalidationEvent& event);
  OptimizerPlanCacheInvalidationResult InvalidateWithGovernedMemory(
      const OptimizerInvalidationEvent& event,
      memory::ResultCursorPlanMemoryGovernor* governor);
  OptimizerPlanCacheInvalidationResult ShrinkGovernedMemory(
      const std::string& database_id,
      std::uint64_t target_bytes,
      memory::ResultCursorPlanMemoryGovernor* governor);
  OptimizerPlanCachePersistenceEnvelope ExportPersistenceEnvelope(
      const OptimizerPlanCachePersistenceRequest& request) const;
  OptimizerPlanCacheEnterpriseValidation ImportPersistenceEnvelope(
      const OptimizerPlanCachePersistenceEnvelope& envelope);
  void Clear();
  OptimizerPlanCacheStats Stats() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, CachedOptimizerPlan> plans_;
  OptimizerPlanCacheStats stats_;
};

std::string BuildOptimizerPlanCacheKey(const OptimizerPlanCacheKeyInput& input);
std::string BuildNormalizedOptimizerPolicyControlDigest(
    const scratchbird::engine::planner::OptimizerPolicyMetadata& policy);
OptimizerPlanCacheKeyInput BuildOptimizerPlanCacheKeyInput(const BoundOptimizerRequest& request,
                                                           std::string cost_profile_id,
                                                           std::vector<std::string> object_uuids = {},
                                                           std::vector<std::string> function_uuids = {},
                                                           std::vector<std::string> index_uuids = {},
                                                           std::vector<std::string> filespace_uuids = {});
OptimizerProductionPlanCacheKeyResult BuildProductionOptimizerPlanCacheKeyInput(
    const OptimizerProductionPlanCacheKeyRequest& request);
bool OptimizerPlanDependsOnEvent(const CachedOptimizerPlan& plan, const OptimizerInvalidationEvent& event);
bool OptimizerInvalidationEventKindRecognized(const std::string& event_kind);
std::string OptimizerInvalidationDiagnosticCode(const OptimizerInvalidationEvent& event);
OptimizerInvalidationEvent OptimizerInvalidationEventForMutation(std::string mutation_source,
                                                                 std::string dependency_uuid,
                                                                 std::uint64_t event_epoch);
OptimizerPlanCacheEnterpriseValidation ValidateEnterpriseOptimizerPlanCacheKeyInput(
    const OptimizerPlanCacheKeyInput& input);
OptimizerPlanCacheEnterpriseValidation ValidateEnterpriseCachedOptimizerPlan(
    const CachedOptimizerPlan& plan);
std::string BuildOptimizerPlanCachePersistenceDigest(
    const OptimizerPlanCachePersistenceEnvelope& envelope);

}  // namespace scratchbird::engine::optimizer

namespace scratchbird::engine::internal_api {

// Implemented by the canonical query-plan API owner. The optimizer cache
// contract intentionally exposes only optimizer/executor types here, so the
// optimizer layer does not depend on an internal-API header.
scratchbird::engine::optimizer::CanonicalExecutablePlanHitExecutionResult
ExecuteCanonicalExecutablePlanCacheHit(
    const scratchbird::engine::optimizer::
        CanonicalExecutablePlanHitExecutionRequest& request);

}  // namespace scratchbird::engine::internal_api
