// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define QOW_OPT_016_FIXTURE_ONLY
#include "qow_opt_016.cpp"
#include "optimizer_plan_cache.hpp"
#include "query/plan_api.hpp"

#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace cache = scratchbird::engine::optimizer;

template <typename T>
concept CarriesStoredStatementAuthority = requires(T value) {
  value.mga_statement_context;
  value.statement_snapshot_uuid;
  value.resolve_current;
};

template <typename T>
concept CarriesExecutablePhysicalPlan = requires(T value) {
  value.prepared_plan;
  value.execution_physical_dag;
};

static_assert(
    !CarriesStoredStatementAuthority<cache::CanonicalPreparedPhysicalPlan>);
static_assert(
    !CarriesStoredStatementAuthority<cache::CanonicalExecutablePlanCacheEntry>);
static_assert(!CarriesExecutablePhysicalPlan<cache::CachedOptimizerPlan>);

bool Require009(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-OPT-009-V1: " << detail << '\n';
  return condition;
}

exec::TypedPhysicalNodeDag PublishedDag009() {
  const auto inputs = Inputs();
  const auto published = opt::PublishCanonicalPhysicalDag(
      inputs.request, inputs.admission, inputs.alternatives, inputs.search,
      inputs.capabilities, PublicationIdentity());
  return published.accepted && published.published
             ? published.physical_dag
             : exec::TypedPhysicalNodeDag{};
}

cache::CanonicalPreparedPlanParameterDescriptor Parameter009() {
  cache::CanonicalPreparedPlanParameterDescriptor descriptor;
  descriptor.ordinal = 1;
  descriptor.descriptor_id = 9001;
  descriptor.descriptor_uuid = Uuid(960);
  descriptor.type_uuid = Uuid(961);
  descriptor.type_modifier_digest = std::string(64, 'a');
  descriptor.encoded_descriptor =
      "type_uuid=" + descriptor.type_uuid + ";nullability=non_null";
  descriptor.nullable = false;
  return descriptor;
}

cache::CanonicalPreparedPlanResultDescriptor ResultDescriptor009(
    const std::uint32_t descriptor_id) {
  cache::CanonicalPreparedPlanResultDescriptor descriptor;
  descriptor.ordinal = 1;
  descriptor.descriptor_id = descriptor_id;
  descriptor.name_utf8 = "projected_value";
  descriptor.descriptor_uuid = Uuid(980);
  descriptor.type_uuid = Uuid(981);
  descriptor.type_modifier_digest = std::string(64, 'b');
  descriptor.encoded_descriptor =
      "type_uuid=" + descriptor.type_uuid + ";nullability=non_null";
  descriptor.nullable = false;
  return descriptor;
}

cache::CanonicalPreparePhysicalPlanRequest PrepareRequest009() {
  cache::CanonicalPreparePhysicalPlanRequest request;
  request.prepared_plan_uuid = Uuid(950);
  request.prepare_generation = 7;
  request.parameter_shape_uuid = Uuid(951);
  request.result_schema_uuid = Uuid(952);
  request.selected_physical_dag = PublishedDag009();
  request.parameters = {Parameter009()};
  const auto root = std::ranges::find_if(
      request.selected_physical_dag.nodes, [&](const auto& node) {
        return node.physical_node_id ==
               request.selected_physical_dag.root_physical_node_id;
      });
  if (root != request.selected_physical_dag.nodes.end() &&
      root->output_descriptor_ids.size() == 1) {
    request.result_descriptors = {
        ResultDescriptor009(root->output_descriptor_ids.front())};
  }
  request.dependencies = {
      {cache::CanonicalPreparedPlanDependencyKind::kObject, Uuid(970), 11,
       std::string(64, '1')},
      {cache::CanonicalPreparedPlanDependencyKind::kFunction, Uuid(971), 12,
       std::string(64, '2')},
      {cache::CanonicalPreparedPlanDependencyKind::kIndex, Uuid(972), 13,
       std::string(64, '3')},
      {cache::CanonicalPreparedPlanDependencyKind::kFilespace, Uuid(973), 14,
       std::string(64, '4')},
      {cache::CanonicalPreparedPlanDependencyKind::kDescriptor, Uuid(974), 15,
       std::string(64, '5')},
      {cache::CanonicalPreparedPlanDependencyKind::kDatatype, Uuid(975), 16,
       std::string(64, '6')},
      {cache::CanonicalPreparedPlanDependencyKind::kDomain, Uuid(976), 17,
       std::string(64, '7')},
      {cache::CanonicalPreparedPlanDependencyKind::kCollation, Uuid(977), 18,
       std::string(64, '8')},
  };
  request.engine_prepare_authorized = true;
  return request;
}

std::vector<cache::CanonicalExecutablePlanGeneration> Project009(
    const cache::CanonicalPreparedPhysicalPlan& plan,
    const std::initializer_list<cache::CanonicalPreparedPlanDependencyKind>
        kinds) {
  return cache::CanonicalExecutablePlanDependencyProjection(plan.dependencies,
                                                            kinds);
}

cache::CanonicalExecutablePlanCacheKey CacheKey009(
    const cache::CanonicalPreparedPhysicalPlan& plan) {
  cache::CanonicalExecutablePlanCacheKey key;
  key.cache_plan_uuid = Uuid(1001);
  key.compiled_at_uuidv7 = Uuid(1002);
  key.plan_status = cache::CanonicalExecutablePlanStatus::kValid;
  key.plan_key_digest = std::string(64, 'c');
  key.database_uuid = Uuid(1003);
  key.engine_format_generation = 31;
  key.sblr_unit_uuid = Uuid(1004);
  key.bound_sblr_tree_uuid = plan.bound_sblr_tree_uuid;
  key.parser_compatibility_profile_uuid = Uuid(1005);
  key.parser_compatibility_generation = 32;
  key.plan_policy_profile_uuid = Uuid(1006);
  key.optimizer_configuration_generation = 33;
  key.bound_object_set_digest = std::string(64, 'd');
  key.security_policy_digest = std::string(64, 'e');
  key.redaction_policy_digest = std::string(64, 'f');
  key.resource_policy_digest = std::string(64, '0');
  key.filespace_placement_generation = 34;
  key.snapshot_class =
      cache::CanonicalExecutablePlanSnapshotClass::kReadCommitted;
  key.standalone_database = true;

  key.prepared_plan_uuid = plan.prepared_plan_uuid;
  key.prepare_generation = plan.prepare_generation;
  key.parameter_shape_uuid = plan.parameter_shape_uuid;
  key.result_schema_uuid = plan.result_schema_uuid;
  key.selected_plan_uuid = plan.selected_plan_uuid;
  key.selected_plan_signature = plan.selected_plan_signature;
  key.selected_scalar_score = plan.selected_scalar_score;
  key.root_physical_node_id = plan.root_physical_node_id;
  key.published_node_count = plan.published_node_count;
  key.first_causal_counter_id = plan.first_causal_counter_id;
  key.catalog_epoch_uuid = plan.catalog_epoch_uuid;
  key.security_context_uuid = plan.security_context_uuid;
  key.capability_snapshot_uuid = plan.capability_snapshot_uuid;
  key.resource_snapshot_uuid = plan.resource_snapshot_uuid;
  key.statistics_snapshot_uuid = plan.statistics_snapshot_uuid;
  key.route_snapshot_uuid = plan.route_snapshot_uuid;
  key.catalog_generation = plan.catalog_generation;
  key.security_epoch = plan.security_epoch;
  key.policy_epoch = plan.policy_epoch;
  key.resource_epoch = plan.resource_epoch;
  key.statistics_generation = plan.statistics_generation;
  key.route_epoch = plan.route_epoch;
  key.route_generation = plan.route_generation;
  key.memory_budget_bytes = plan.memory_budget_bytes;
  key.spill_allowed = plan.spill_allowed;
  key.parameters = plan.parameters;
  key.result_descriptors = plan.result_descriptors;
  key.physical_dependencies = plan.dependencies;
  key.object_generations =
      Project009(plan, {cache::CanonicalPreparedPlanDependencyKind::kObject});
  key.function_generations = Project009(
      plan, {cache::CanonicalPreparedPlanDependencyKind::kFunction});
  key.index_generations =
      Project009(plan, {cache::CanonicalPreparedPlanDependencyKind::kIndex});
  key.filespace_generations = Project009(
      plan, {cache::CanonicalPreparedPlanDependencyKind::kFilespace});
  key.datatype_generations = Project009(
      plan, {cache::CanonicalPreparedPlanDependencyKind::kDatatype,
             cache::CanonicalPreparedPlanDependencyKind::kDomain});
  key.collation_generations = Project009(
      plan, {cache::CanonicalPreparedPlanDependencyKind::kCollation});
  key.metadata_generations = Project009(
      plan, {cache::CanonicalPreparedPlanDependencyKind::kDescriptor});
  key.metadata_generations.push_back(
      {plan.result_schema_uuid, plan.catalog_generation, std::string(64, '9')});
  std::ranges::sort(key.metadata_generations, {},
                    &cache::CanonicalExecutablePlanGeneration::identity_uuid);
  key.statistics_generations = {
      {plan.statistics_snapshot_uuid, plan.statistics_generation,
       std::string(64, 'a')}};
  key.route_generations = {
      {plan.route_snapshot_uuid, plan.route_generation, std::string(64, 'b')}};

  for (const auto& node : plan.nodes) {
    key.capability_generations.push_back(
        {node.executor_capability_uuid,
         node.executor_capability_abi_version,
         100 + node.executor_capability_abi_version,
         std::string(64, 'c')});
  }
  std::ranges::sort(
      key.capability_generations, {},
      &cache::CanonicalExecutablePlanCapabilityGeneration::capability_uuid);
  key.capability_generations.erase(
      std::unique(key.capability_generations.begin(),
                  key.capability_generations.end(),
                  [](const auto& left, const auto& right) {
                    return left.capability_uuid == right.capability_uuid &&
                           left.abi_version == right.abi_version;
                  }),
      key.capability_generations.end());
  return key;
}

exec::PhysicalMgaStatementContext FreshStatement009(
    const std::uint64_t uuid_seed = 1100) {
  exec::PhysicalMgaStatementContext context;
  context.statement_uuid = Uuid(uuid_seed);
  context.owning_transaction_uuid = Uuid(uuid_seed + 1);
  context.statement_snapshot_uuid = Uuid(uuid_seed + 2);
  context.statement_metadata_snapshot_uuid = Uuid(uuid_seed + 3);
  context.owning_local_transaction_id = 70;
  context.visible_committed_high_watermark = 60;
  context.oldest_active_transaction_id = 20;
  context.oldest_interesting_transaction_id = 30;
  context.oldest_snapshot_transaction_id = 30;
  context.retention_horizon_transaction_id = 30;
  context.active_excluded_local_transaction_ids = {70};
  context.snapshot_kind = "statement_stable";
  context.publication_inventory_next_local_transaction_id = 100;
  context.inventory_authoritative = true;
  context.complete = true;
  context.current = true;
  return context;
}

exec::CanonicalExecutionMgaAuthority Authority009(
    const exec::PhysicalMgaStatementContext& context) {
  exec::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = context;
  authority.origin =
      exec::CanonicalMgaAuthorityOrigin::kEngineTransactionInventory;
  authority.resolve_current = [context] {
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = context;
    return resolution;
  };
  return authority;
}

struct Fixture009 {
  cache::CanonicalPreparedPlanStore prepared_store;
  cache::CanonicalExecutablePlanCache executable_cache;
  std::shared_ptr<const cache::CanonicalPreparedPhysicalPlan> prepared_plan;
  cache::CanonicalExecutablePlanCacheKey key;
  api::EngineTypedValue parameter_value;
  bool ready{false};

  Fixture009() {
    const auto prepared = cache::PrepareCanonicalPhysicalPlan(
        PrepareRequest009(), &prepared_store);
    if (!prepared.accepted || !prepared.prepared_plan) return;
    prepared_plan = prepared.prepared_plan;
    key = CacheKey009(*prepared_plan);
    cache::CanonicalExecutablePlanCacheAdmissionRequest admission;
    admission.key = key;
    admission.engine_cache_admission_authorized = true;
    const auto admitted = executable_cache.Admit(prepared_store, admission);
    if (!admitted.accepted || !admitted.entry) return;
    parameter_value.descriptor.descriptor_uuid.canonical =
        prepared_plan->parameters.front().descriptor_uuid;
    parameter_value.descriptor.descriptor_kind = "scalar";
    parameter_value.descriptor.canonical_type_name = "int64";
    parameter_value.descriptor.encoded_descriptor =
        prepared_plan->parameters.front().encoded_descriptor;
    parameter_value.encoded_value = "41";
    parameter_value.state = api::EngineValueState::value;
    ready = true;
  }
};

exec::DescriptorBatch NodeBatch009(const exec::PhysicalNodeRecord& node,
                                   const bool root,
                                   const cache::CanonicalPreparedPhysicalPlan&
                                       plan) {
  std::vector<exec::ExecutorColumnDescriptor> columns;
  for (std::size_t index = 0; index < node.output_descriptor_ids.size();
       ++index) {
    api::EngineDescriptor descriptor;
    std::string stable_name = "node_value_" + std::to_string(index);
    descriptor.descriptor_kind = "scalar";
    descriptor.canonical_type_name = "int64";
    if (root) {
      const auto& stored = plan.result_descriptors.at(index);
      stable_name = stored.name_utf8;
      descriptor.descriptor_uuid.canonical = stored.descriptor_uuid;
      descriptor.encoded_descriptor = stored.encoded_descriptor;
    } else {
      descriptor.descriptor_uuid.canonical =
          Uuid(3000 + node.output_descriptor_ids[index]);
      descriptor.encoded_descriptor =
          "type_uuid=" + Uuid(4000 + node.output_descriptor_ids[index]) +
          ";nullability=non_null";
    }
    columns.push_back({std::move(stable_name), descriptor, false,
                       node.output_descriptor_ids[index]});
  }
  exec::DescriptorTuple tuple;
  for (const auto& column : columns) {
    tuple.values.push_back(exec::MakeExecutorValue(column.descriptor, "7"));
  }
  return exec::MakeDescriptorBatch(std::move(columns), {std::move(tuple)});
}

exec::CanonicalPhysicalDispatchStepResult Step009(
    const exec::TypedPhysicalNodeDag& dag,
    const exec::PhysicalNodeRecord& node,
    const cache::CanonicalPreparedPhysicalPlan& plan) {
  exec::CanonicalPhysicalDispatchStepResult step;
  step.selected_plan_uuid = dag.selected_plan_uuid;
  step.executed_physical_node_id = node.physical_node_id;
  step.causal_counter_id = node.causal_counter_id;
  step.result_handle_id = 20'000 + node.physical_node_id;
  step.output_descriptor_ids = node.output_descriptor_ids;
  step.authority.engine_mga_snapshot_bound = true;
  step.data_access_observation_known = true;
  step.data_access_observed = true;
  step.input_row_count = node.input_physical_node_ids.size();
  step.output_row_count = 1;
  step.rows_examined = 1;
  step.mga_statement_context = dag.mga_statement_context;
  step.materialized_output_batch = NodeBatch009(
      node, node.physical_node_id == dag.root_physical_node_id, plan);
  return step;
}

exec::CanonicalResultPublicationRequest Publication009(
    const cache::CanonicalPreparedPhysicalPlan& plan,
    const exec::PhysicalMgaStatementContext& context,
    const std::uint64_t attempt_seed = 1200) {
  exec::CanonicalResultPublicationRequest publication;
  publication.statement_uuid = context.statement_uuid;
  publication.selected_catalog_epoch_uuid = plan.catalog_epoch_uuid;
  publication.execution_attempt_uuid = Uuid(attempt_seed);
  publication.transaction_effect_evidence_uuid = Uuid(attempt_seed + 1);
  publication.invocation_mode =
      exec::CanonicalResultInvocationMode::kPrepared;
  publication.result_kind = exec::CanonicalResultKind::kRows;
  for (const auto& stored : plan.result_descriptors) {
    exec::CanonicalResultColumnDescriptor descriptor;
    descriptor.ordinal = stored.ordinal - 1;
    descriptor.name_utf8 = stored.name_utf8;
    descriptor.descriptor_uuid = stored.descriptor_uuid;
    descriptor.type_uuid = stored.type_uuid;
    descriptor.nullability =
        stored.nullable ? exec::CanonicalResultNullability::kNullable
                        : exec::CanonicalResultNullability::kNonNull;
    if (!stored.collation_uuid.empty()) {
      descriptor.collation_uuid = stored.collation_uuid;
    }
    if (!stored.timezone_uuid.empty()) {
      descriptor.timezone_profile_id = stored.timezone_uuid;
    }
    publication.column_bindings.push_back(
        {stored.ordinal - 1, true, std::move(descriptor)});
  }
  return publication;
}

cache::CanonicalExecutablePlanHitExecutionRequest ExecutionRequest009(
    Fixture009* fixture, const cache::CanonicalExecutablePlanCacheKey& key,
    const exec::PhysicalMgaStatementContext& context,
    std::size_t* executor_invocations,
    bool* every_executor_received_fresh_context) {
  cache::CanonicalExecutablePlanHitExecutionRequest request;
  request.executable_plan_cache = &fixture->executable_cache;
  request.lookup.current_key = key;
  request.lookup.parameter_bindings = {
      {fixture->prepared_plan->parameters.front(), &fixture->parameter_value}};
  request.lookup.mga_authority = Authority009(context);
  request.lookup.engine_lookup_authorized = true;
  request.lookup.engine_security_revalidated = true;
  request.lookup.engine_policy_revalidated = true;
  request.lookup.engine_authorization_revalidated = true;
  request.lookup.authorization_revalidation_receipt_uuid = Uuid(1300);
  request.engine_execution_authorized = true;
  request.result_publication_request =
      Publication009(*fixture->prepared_plan, context);
  for (const auto& published : PublishedDag009().nodes) {
    exec::CanonicalPhysicalExecutorRegistration registration;
    registration.node_kind = published.node_kind;
    registration.implementation_id = published.implementation_id;
    registration.executor_capability_uuid =
        published.executor_capability_uuid;
    registration.executor_capability_abi_version =
        published.executor_capability_abi_version;
    registration.engine_owned = true;
    registration.accepts_optimizer_publication_v2 = true;
    const auto plan = fixture->prepared_plan;
    registration.execute =
        [executor_invocations, every_executor_received_fresh_context,
         context, plan](const auto& dag, const auto& node,
                        const auto&) {
          ++*executor_invocations;
          *every_executor_received_fresh_context =
              *every_executor_received_fresh_context &&
              exec::PhysicalMgaStatementContextEqual(
                  dag.mga_statement_context, context) &&
              exec::PhysicalMgaStatementContextEqual(
                  node.mga_statement_context, context);
          return Step009(dag, node, *plan);
        };
    request.available_executors.push_back(std::move(registration));
  }
  return request;
}

bool ValidateExecutableHit009() {
  Fixture009 fixture;
  if (!Require009(fixture.ready, "fixture cache admission failed")) {
    return false;
  }
  const auto fresh = FreshStatement009();
  const auto prepared_source = PublishedDag009().mga_statement_context;
  std::size_t executor_invocations = 0;
  bool every_executor_received_fresh_context = true;
  auto request = ExecutionRequest009(&fixture, fixture.key, fresh,
                                     &executor_invocations,
                                     &every_executor_received_fresh_context);
  auto result = api::ExecuteCanonicalExecutablePlanCacheHit(request);
  if (!result.accepted) {
    for (const auto& issue : result.issues) {
      std::cerr << "QOW-TEST-OPT-009-V1: refusal=" << issue.diagnostic_id
                << " field=" << issue.field_id
                << " upstream=" << issue.upstream_diagnostic_id
                << " upstream_field=" << issue.upstream_field_id << '\n';
    }
    for (const auto& issue : result.checkout.issues) {
      std::cerr << "QOW-TEST-OPT-009-V1: checkout=" << issue.diagnostic_id
                << " field=" << issue.field_id << '\n';
    }
  }
  bool passed = true;
  passed &= Require009(
      result.accepted && result.cache_hit &&
          result.exact_selected_nodes_executed &&
          result.canonical_result_published && result.data_access_observed &&
          !result.reprepare_required && !result.automatic_replan_attempted &&
          result.issues.empty(),
      "valid executable cache hit did not execute canonically");
  passed &= Require009(
      result.selected_plan_uuid == fixture.prepared_plan->selected_plan_uuid &&
          result.executed_root_physical_node_id ==
              fixture.prepared_plan->root_physical_node_id &&
          result.result_schema_uuid == fixture.prepared_plan->result_schema_uuid &&
          result.executed_nodes.size() == fixture.prepared_plan->nodes.size() &&
          executor_invocations == fixture.prepared_plan->nodes.size(),
      "cache hit changed selected plan, root, nodes, or result schema");
  passed &= Require009(
      every_executor_received_fresh_context &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context, fresh) &&
          exec::PhysicalMgaStatementContextEqual(
              result.checkout.execution_physical_dag.mga_statement_context,
              fresh) &&
          !exec::PhysicalMgaStatementContextEqual(fresh, prepared_source) &&
          result.checkout.execution_physical_dag.local_transaction_id ==
              fresh.owning_local_transaction_id &&
          result.checkout.execution_physical_dag.statement_snapshot_id ==
              fresh.visible_committed_high_watermark,
      "cache hit reused PREPARE authority instead of fresh engine MGA state");
  passed &= Require009(
      result.structural_no_optimizer_search_planner_or_fallback_route &&
          result.checkout.receipt
              .structural_no_optimizer_search_planner_or_fallback_route &&
          result.optimizer_invocation_count == 0 &&
          result.search_invocation_count == 0 &&
          result.planner_invocation_count == 0 &&
          result.uncached_fallback_invocation_count == 0,
      "valid cache hit reported optimizer/search/planner/fallback work");
  passed &= Require009(
      !result.parameter_values_retained &&
          !result.checkout.receipt.parameter_values_retained &&
          !result.checkout.entry->parameter_values_retained &&
          result.checkout.receipt.transient_parameter_value_count == 1 &&
          fixture.prepared_plan->parameters.front().descriptor_uuid ==
              Uuid(960),
      "transient EXECUTE parameter value leaked into the retained cache");
  const auto stored_implementation =
      fixture.prepared_plan->nodes.front().implementation_id;
  result.checkout.execution_physical_dag.nodes.front().implementation_id =
      "mutated.execution.copy.v1";
  fixture.parameter_value.encoded_value = "99";
  passed &= Require009(
      fixture.prepared_plan->nodes.front().implementation_id ==
              stored_implementation &&
          fixture.executable_cache.Size() == 1,
      "execution copy or transient parameter mutated the stored plan");
  return passed;
}

using KeyMutation009 =
    std::pair<std::string_view,
              std::function<void(cache::CanonicalExecutablePlanCacheKey&)>>;

bool ValidateExactKeyRefusals009() {
  Fixture009 fixture;
  if (!Require009(fixture.ready, "key-refusal fixture admission failed")) {
    return false;
  }
  const std::vector<KeyMutation009> mutations = {
      {"cache plan UUID", [](auto& key) { key.cache_plan_uuid = Uuid(9001); }},
      {"compiled UUID", [](auto& key) { key.compiled_at_uuidv7 = Uuid(9002); }},
      {"plan status", [](auto& key) { key.plan_status = cache::CanonicalExecutablePlanStatus::kInvalid; }},
      {"plan key", [](auto& key) { key.plan_key_digest = std::string(64, '1'); }},
      {"database", [](auto& key) { key.database_uuid = Uuid(9003); }},
      {"engine format", [](auto& key) { ++key.engine_format_generation; }},
      {"SBLR unit", [](auto& key) { key.sblr_unit_uuid = Uuid(9004); }},
      {"internal procedure source", [](auto& key) { key.internal_procedure_uuid = Uuid(9022); }},
      {"bound SBLR tree", [](auto& key) { key.bound_sblr_tree_uuid = Uuid(9005); }},
      {"parser profile", [](auto& key) { key.parser_compatibility_profile_uuid = Uuid(9006); }},
      {"parser generation", [](auto& key) { ++key.parser_compatibility_generation; }},
      {"donor profile", [](auto& key) { key.donor_compatibility_profile_uuid = Uuid(9007); key.donor_compatibility_generation = 1; }},
      {"plan policy", [](auto& key) { key.plan_policy_profile_uuid = Uuid(9008); }},
      {"optimizer configuration", [](auto& key) { ++key.optimizer_configuration_generation; }},
      {"bound objects", [](auto& key) { key.bound_object_set_digest = std::string(64, '2'); }},
      {"security policy", [](auto& key) { key.security_policy_digest = std::string(64, '3'); }},
      {"redaction policy", [](auto& key) { key.redaction_policy_digest = std::string(64, '4'); }},
      {"resource policy", [](auto& key) { key.resource_policy_digest = std::string(64, '5'); }},
      {"filespace placement", [](auto& key) { ++key.filespace_placement_generation; }},
      {"snapshot class", [](auto& key) { key.snapshot_class = cache::CanonicalExecutablePlanSnapshotClass::kSnapshot; }},
      {"cluster identity", [](auto& key) { key.standalone_database = false; key.cluster_uuid = Uuid(9009); key.cluster_epoch = 1; }},
      {"prepared plan", [](auto& key) { key.prepared_plan_uuid = Uuid(9010); }},
      {"prepare generation", [](auto& key) { ++key.prepare_generation; }},
      {"parameter shape", [](auto& key) { key.parameter_shape_uuid = Uuid(9011); }},
      {"result schema", [](auto& key) { key.result_schema_uuid = Uuid(9012); }},
      {"selected plan", [](auto& key) { key.selected_plan_uuid = Uuid(9013); }},
      {"selected signature", [](auto& key) { key.selected_plan_signature += ".changed"; }},
      {"selected score", [](auto& key) { ++key.selected_scalar_score; }},
      {"root", [](auto& key) { ++key.root_physical_node_id; }},
      {"node count", [](auto& key) { ++key.published_node_count; }},
      {"causal base", [](auto& key) { ++key.first_causal_counter_id; }},
      {"catalog UUID", [](auto& key) { key.catalog_epoch_uuid = Uuid(9014); }},
      {"security UUID", [](auto& key) { key.security_context_uuid = Uuid(9015); }},
      {"capability UUID", [](auto& key) { key.capability_snapshot_uuid = Uuid(9016); }},
      {"resource UUID", [](auto& key) { key.resource_snapshot_uuid = Uuid(9017); }},
      {"statistics UUID", [](auto& key) { key.statistics_snapshot_uuid = Uuid(9018); }},
      {"route UUID", [](auto& key) { key.route_snapshot_uuid = Uuid(9019); }},
      {"catalog generation", [](auto& key) { ++key.catalog_generation; }},
      {"security epoch", [](auto& key) { ++key.security_epoch; }},
      {"policy epoch", [](auto& key) { ++key.policy_epoch; }},
      {"resource epoch", [](auto& key) { ++key.resource_epoch; }},
      {"statistics generation", [](auto& key) { ++key.statistics_generation; }},
      {"route epoch", [](auto& key) { ++key.route_epoch; }},
      {"route generation", [](auto& key) { ++key.route_generation; }},
      {"memory budget", [](auto& key) { ++key.memory_budget_bytes; }},
      {"spill policy", [](auto& key) { key.spill_allowed = !key.spill_allowed; }},
      {"parameter descriptor", [](auto& key) { key.parameters.front().type_uuid = Uuid(9020); }},
      {"result descriptor", [](auto& key) { key.result_descriptors.front().type_uuid = Uuid(9021); }},
      {"physical dependency", [](auto& key) { ++key.physical_dependencies.front().generation; }},
      {"object vector", [](auto& key) { ++key.object_generations.front().generation; }},
      {"function vector", [](auto& key) { ++key.function_generations.front().generation; }},
      {"metadata vector", [](auto& key) { ++key.metadata_generations.front().generation; }},
      {"datatype vector", [](auto& key) { ++key.datatype_generations.front().generation; }},
      {"collation vector", [](auto& key) { ++key.collation_generations.front().generation; }},
      {"statistics vector", [](auto& key) { ++key.statistics_generations.front().generation; }},
      {"index vector", [](auto& key) { ++key.index_generations.front().generation; }},
      {"filespace vector", [](auto& key) { ++key.filespace_generations.front().generation; }},
      {"route vector", [](auto& key) { ++key.route_generations.front().generation; }},
      {"capability vector", [](auto& key) { ++key.capability_generations.front().generation; }},
  };
  bool passed = true;
  for (const auto& [name, mutate] : mutations) {
    auto current = fixture.key;
    mutate(current);
    std::size_t invocations = 0;
    bool fresh = true;
    auto request = ExecutionRequest009(&fixture, current, FreshStatement009(),
                                       &invocations, &fresh);
    const auto result = api::ExecuteCanonicalExecutablePlanCacheHit(request);
    passed &= Require009(
        !result.accepted && !result.exact_selected_nodes_executed &&
            !result.canonical_result_published && invocations == 0 &&
            result.reprepare_required &&
            result.dispatch.executed_steps.empty() &&
            result.planner_invocation_count == 0 &&
            result.uncached_fallback_invocation_count == 0 &&
            result.issues.size() == 1 &&
            result.issues.front().diagnostic_id ==
                "QOW-DIAG-OPT-009-REFUSAL-V1",
        std::string("cache key mismatch reached execution: ") +
            std::string(name));
  }
  return passed;
}

bool ValidateAuthorityParameterAndSchemaRefusals009() {
  Fixture009 fixture;
  if (!Require009(fixture.ready, "refusal fixture admission failed")) {
    return false;
  }
  bool passed = true;
  const auto expect_refusal = [&](auto mutate, const std::string_view detail,
                                  const bool expect_hit = false) {
    std::size_t invocations = 0;
    bool fresh = true;
    auto request = ExecutionRequest009(&fixture, fixture.key,
                                       FreshStatement009(), &invocations,
                                       &fresh);
    mutate(request);
    const auto result = api::ExecuteCanonicalExecutablePlanCacheHit(request);
    return Require009(
        !result.accepted && result.cache_hit == expect_hit &&
            !result.exact_selected_nodes_executed &&
            !result.canonical_result_published && invocations == 0 &&
            !result.reprepare_required &&
            result.dispatch.executed_steps.empty() &&
            !result.automatic_replan_attempted &&
            result.planner_invocation_count == 0 &&
            result.uncached_fallback_invocation_count == 0 &&
            result.issues.size() == 1 &&
            result.issues.front().diagnostic_id ==
                "QOW-DIAG-OPT-009-REFUSAL-V1",
        detail);
  };
  passed &= expect_refusal(
      [](auto& request) { request.executable_plan_cache = nullptr; },
      "missing executable cache reached cache hit");
  passed &= expect_refusal(
      [](auto& request) { request.engine_execution_authorized = false; },
      "missing engine execution authority reached cache hit");
  passed &= expect_refusal(
      [](auto& request) {
        request.parser_execution_authority_claimed = true;
      },
      "top-level parser execution authority reached cache hit");
  passed &= expect_refusal(
      [](auto& request) {
        request.transaction_finality_authority_claimed = true;
      },
      "top-level transaction finality authority reached cache hit");
  passed &= expect_refusal(
      [](auto& request) { request.recovery_authority_claimed = true; },
      "top-level recovery authority reached cache hit");
  passed &= expect_refusal(
      [](auto& request) {
        request.lookup.engine_security_revalidated = false;
      },
      "stale security authorization reached cache hit");
  passed &= expect_refusal(
      [](auto& request) { request.lookup.engine_policy_revalidated = false; },
      "stale policy authorization reached cache hit");
  passed &= expect_refusal(
      [](auto& request) {
        request.lookup.engine_authorization_revalidated = false;
      },
      "missing current authorization receipt reached cache hit");
  passed &= expect_refusal(
      [](auto& request) {
        request.lookup.authorization_revalidation_receipt_uuid.clear();
      },
      "malformed authorization receipt reached cache hit");
  passed &= expect_refusal(
      [](auto& request) {
        request.lookup.mga_authority.origin =
            exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
      },
      "non-inventory MGA authority reached cache hit");
  passed &= expect_refusal(
      [](auto& request) {
        auto stale = request.lookup.mga_authority.statement_context;
        stale.current = false;
        request.lookup.mga_authority.resolve_current = [stale] {
          exec::CanonicalMgaCurrentResolution resolution;
          resolution.statement_context = stale;
          return resolution;
        };
      },
      "stale current MGA resolver reached cache hit");
  passed &= expect_refusal(
      [](auto& request) {
        request.lookup.parser_execution_authority_claimed = true;
      },
      "parser execution authority reached cache hit");
  passed &= expect_refusal(
      [](auto& request) {
        request.lookup.transaction_finality_authority_claimed = true;
      },
      "transaction finality authority reached cache hit");
  passed &= expect_refusal(
      [](auto& request) { request.lookup.recovery_authority_claimed = true; },
      "recovery authority reached cache hit");
  passed &= expect_refusal(
      [](auto& request) { request.lookup.parameter_bindings.clear(); },
      "missing parameter reached cache hit");
  passed &= expect_refusal(
      [](auto& request) {
        request.lookup.parameter_bindings.front().typed_value = nullptr;
      },
      "null typed parameter pointer reached cache hit");
  passed &= expect_refusal(
      [](auto& request) {
        request.lookup.parameter_bindings.front().descriptor.type_uuid =
            Uuid(9200);
      },
      "wrong parameter type reached cache hit");
  passed &= expect_refusal(
      [](auto& request) {
        auto* value = const_cast<api::EngineTypedValue*>(
            request.lookup.parameter_bindings.front().typed_value);
        value->state = api::EngineValueState::missing;
      },
      "missing-state parameter reached cache hit");
  fixture.parameter_value.state = api::EngineValueState::value;
  passed &= expect_refusal(
      [](auto& request) {
        auto* value = const_cast<api::EngineTypedValue*>(
            request.lookup.parameter_bindings.front().typed_value);
        value->setState(api::EngineValueState::sql_null);
        value->encoded_value.clear();
        value->binary_value.clear();
      },
      "SQL NULL reached a non-null parameter");
  fixture.parameter_value.setState(api::EngineValueState::value);
  fixture.parameter_value.encoded_value = "41";
  passed &= expect_refusal(
      [](auto& request) {
        auto* value = const_cast<api::EngineTypedValue*>(
            request.lookup.parameter_bindings.front().typed_value);
        value->binary_value = {0x01};
      },
      "ambiguous text-plus-binary parameter reached cache hit");
  fixture.parameter_value.binary_value.clear();
  passed &= expect_refusal(
      [](auto& request) {
        request.result_publication_request.column_bindings.front()
            .published_descriptor->type_uuid = Uuid(9201);
      },
      "result schema mismatch reached data access", true);
  return passed;
}

bool ValidateAdmissionIsolationAndNoReplan009() {
  Fixture009 fixture;
  if (!Require009(fixture.ready, "isolation fixture admission failed")) {
    return false;
  }
  bool passed = true;
  cache::CanonicalExecutablePlanCache metadata_cache;
  cache::CanonicalExecutablePlanCacheAdmissionRequest metadata;
  metadata.key = fixture.key;
  metadata.engine_cache_admission_authorized = true;
  metadata.metadata_only_entry = true;
  const auto metadata_result =
      metadata_cache.Admit(fixture.prepared_store, metadata);
  passed &= Require009(
      !metadata_result.accepted && metadata_cache.Size() == 0 &&
          metadata_result.issues.size() == 1,
      "metadata-only optimizer record entered executable cache");

  cache::CanonicalExecutablePlanCache value_cache;
  auto with_value = metadata;
  with_value.metadata_only_entry = false;
  with_value.parameter_values_supplied = true;
  const auto value_result =
      value_cache.Admit(fixture.prepared_store, with_value);
  passed &= Require009(
      !value_result.accepted && value_cache.Size() == 0,
      "transient parameter value entered executable cache admission");

  auto second_prepare_request = PrepareRequest009();
  second_prepare_request.prepared_plan_uuid = Uuid(1350);
  second_prepare_request.prepare_generation = 8;
  second_prepare_request.parameter_shape_uuid = Uuid(1351);
  const auto second_prepared = cache::PrepareCanonicalPhysicalPlan(
      second_prepare_request, &fixture.prepared_store);
  bool duplicate_cache_plan_refused = false;
  if (second_prepared.accepted && second_prepared.prepared_plan) {
    cache::CanonicalExecutablePlanCacheAdmissionRequest duplicate_admission;
    duplicate_admission.key = CacheKey009(*second_prepared.prepared_plan);
    duplicate_admission.key.cache_plan_uuid = fixture.key.cache_plan_uuid;
    duplicate_admission.engine_cache_admission_authorized = true;
    const auto duplicate = fixture.executable_cache.Admit(
        fixture.prepared_store, duplicate_admission);
    duplicate_cache_plan_refused =
        !duplicate.accepted && fixture.executable_cache.Size() == 1 &&
        duplicate.issues.size() == 1 &&
        duplicate.issues.front().field_id ==
            "duplicate_executable_cache_entry";
  }
  passed &= Require009(
      duplicate_cache_plan_refused,
      "duplicate cache plan UUID entered under a second prepared UUID");

  std::size_t invocations = 0;
  bool fresh = true;
  auto missing_capability = ExecutionRequest009(
      &fixture, fixture.key, FreshStatement009(), &invocations, &fresh);
  missing_capability.available_executors.clear();
  const auto missing_result =
      api::ExecuteCanonicalExecutablePlanCacheHit(missing_capability);
  passed &= Require009(
      !missing_result.accepted && missing_result.cache_hit &&
          missing_result.reprepare_required &&
          !missing_result.automatic_replan_attempted && invocations == 0 &&
          missing_result.dispatch.executed_steps.empty() &&
          missing_result.planner_invocation_count == 0 &&
          missing_result.uncached_fallback_invocation_count == 0 &&
          missing_result.issues.size() == 1 &&
          missing_result.issues.front().upstream_diagnostic_id ==
              "QOW-DIAG-QRY-004-PHYSICAL-IMPLEMENTATION-UNAVAILABLE-V1",
      "unavailable executor triggered hidden replan or lost upstream refusal");
  return passed;
}

bool ValidateBinaryTransientParameter009() {
  Fixture009 fixture;
  if (!Require009(fixture.ready, "binary fixture admission failed")) {
    return false;
  }
  fixture.parameter_value.encoded_value.clear();
  fixture.parameter_value.binary_value = {0x00, 0x7f, 0xff};
  std::size_t invocations = 0;
  bool fresh = true;
  auto request = ExecutionRequest009(&fixture, fixture.key,
                                     FreshStatement009(1400), &invocations,
                                     &fresh);
  request.result_publication_request.execution_attempt_uuid = Uuid(1410);
  const auto result = api::ExecuteCanonicalExecutablePlanCacheHit(request);
  return Require009(
      result.accepted && result.cache_hit &&
          !result.parameter_values_retained &&
          !result.checkout.entry->parameter_values_retained &&
          invocations == fixture.prepared_plan->nodes.size(),
      "binary transient parameter was rejected or retained");
}

bool ValidateProductionRouteIsolation009() {
  std::ifstream source_file(SB_QOW_PLAN_API_SOURCE_FILE);
  const std::string source((std::istreambuf_iterator<char>(source_file)),
                           std::istreambuf_iterator<char>());
  const auto begin = source.find("QOW-ROUTE-STAGE-OPT-009-V1-BEGIN");
  const auto end = source.find("QOW-ROUTE-STAGE-OPT-009-V1-END", begin);
  bool passed = true;
  passed &= Require009(source_file.good() || source_file.eof(),
                       "plan API source could not be read");
  passed &= Require009(begin != std::string::npos &&
                           end != std::string::npos && end > begin,
                       "production cache-hit route markers are absent");
  if (begin != std::string::npos && end != std::string::npos && end > begin) {
    const auto route = source.substr(begin, end - begin);
    const auto selected_call =
        route.find("ExecuteCanonicalOptimizerSelectedDag(selected)");
    passed &= Require009(
        selected_call != std::string::npos &&
            route.find("ExecuteCanonicalOptimizerSelectedDag(selected)",
                       selected_call + 1) == std::string::npos &&
            route.find("EnginePlanOperationUncachedImpl(") ==
                std::string::npos &&
            route.find("EnginePlanOperation(") == std::string::npos &&
            route.find("OptimizeLogicalPlan(") == std::string::npos &&
            route.find("OptimizeLogicalPlanWithStatistics(") ==
                std::string::npos &&
            route.find("OptimizeBoundRequest(") == std::string::npos &&
            route.find("OptimizeCatalogBackedProductionPlan(") ==
                std::string::npos &&
            route.find("SearchCanonicalRelationalMemo(") ==
                std::string::npos &&
            route.find("PrepareCanonicalPhysicalPlan(") ==
                std::string::npos &&
            route.find("DecodeSblrEnvelope(") == std::string::npos &&
            route.find("DecodeAndDispatchSblrOperation(") ==
                std::string::npos &&
            route.find("ExecuteCanonicalObjectFreeValuesQuery(") ==
                std::string::npos &&
            route.find("ExecuteCanonicalCurrentHeapQuery(") ==
                std::string::npos &&
            route.find("automatic_replan_attempted = true") ==
                std::string::npos,
        "production cache hit can reach planner/search/uncached fallback");
  }
  return passed;
}

}  // namespace

// QOW-TEST-OPT-009-V1
int main() {
  bool passed = true;
  passed &= ValidateExecutableHit009();
  passed &= ValidateExactKeyRefusals009();
  passed &= ValidateAuthorityParameterAndSchemaRefusals009();
  passed &= ValidateAdmissionIsolationAndNoReplan009();
  passed &= ValidateBinaryTransientParameter009();
  passed &= ValidateProductionRouteIsolation009();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
