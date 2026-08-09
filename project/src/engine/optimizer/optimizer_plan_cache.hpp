// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

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
  bool nullable{false};
};

struct CanonicalPreparedPlanResultDescriptor {
  std::uint32_t ordinal{0};
  std::uint32_t descriptor_id{0};
  std::string descriptor_uuid;
  std::string type_uuid;
  std::string domain_uuid;
  std::string collation_uuid;
  std::string timezone_uuid;
  std::string type_modifier_digest;
  bool nullable{false};
};

struct CanonicalPreparedPlanDependency {
  CanonicalPreparedPlanDependencyKind dependency_kind{
      CanonicalPreparedPlanDependencyKind::kObject};
  std::string dependency_uuid;
  std::uint64_t generation{0};
  std::string definition_digest;
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
