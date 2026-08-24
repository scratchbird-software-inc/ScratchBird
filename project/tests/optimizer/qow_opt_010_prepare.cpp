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

#include <algorithm>
#include <concepts>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <string_view>

namespace {

namespace cache = scratchbird::engine::optimizer;

template <typename T>
concept CarriesPrepareMgaAuthority = requires(T value) {
  value.mga_statement_context;
  value.local_transaction_id;
  value.statement_snapshot_id;
};

template <typename T>
concept CarriesPreparedParameterValue = requires(T value) {
  value.typed_value;
  value.encoded_value;
};

static_assert(
    !CarriesPrepareMgaAuthority<cache::CanonicalPreparedPhysicalPlan>);
static_assert(
    !CarriesPrepareMgaAuthority<cache::CanonicalPreparedPhysicalNode>);
static_assert(!CarriesPrepareMgaAuthority<
              cache::CanonicalPreparedMetricCoordinatorReceipt>);
static_assert(!CarriesPrepareMgaAuthority<
              cache::CanonicalPreparedMetricCollectionReceipt>);
static_assert(!CarriesPrepareMgaAuthority<
              cache::CanonicalPreparedLegPlanReceipt>);
static_assert(!CarriesPrepareMgaAuthority<
              cache::CanonicalPlannerContextAuthority>);
static_assert(!CarriesPrepareMgaAuthority<
              cache::CanonicalPlannerContinuationContext>);
static_assert(!CarriesPrepareMgaAuthority<
              cache::CanonicalPlannerContinuationReceipt>);
static_assert(!CarriesPrepareMgaAuthority<
              cache::CanonicalPlannerWhatIfContext>);
static_assert(!CarriesPrepareMgaAuthority<
              cache::CanonicalPlannerWhatIfReceipt>);
static_assert(!CarriesPreparedParameterValue<
              cache::CanonicalPreparedPlanParameterDescriptor>);

bool RequirePrepare(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-OPT-010-PREPARE-V1: " << detail << '\n';
  }
  return condition;
}

exec::TypedPhysicalNodeDag PublishedDag() {
  const auto inputs = Inputs();
  const auto published = opt::PublishCanonicalPhysicalDag(
      inputs.request, inputs.admission, inputs.alternatives, inputs.search,
      inputs.capabilities, PublicationIdentity());
  if (!published.accepted || !published.published) return {};
  return published.physical_dag;
}

cache::CanonicalPreparedPlanParameterDescriptor Parameter(
    const std::uint32_t ordinal, const std::uint32_t descriptor_id,
    const std::uint64_t uuid_seed, const bool nullable) {
  cache::CanonicalPreparedPlanParameterDescriptor descriptor;
  descriptor.ordinal = ordinal;
  descriptor.descriptor_id = descriptor_id;
  descriptor.descriptor_uuid = Uuid(uuid_seed);
  descriptor.type_uuid = Uuid(uuid_seed + 10);
  descriptor.domain_uuid = ordinal == 2 ? Uuid(uuid_seed + 20) : "";
  descriptor.collation_uuid = ordinal == 2 ? Uuid(uuid_seed + 30) : "";
  descriptor.timezone_uuid = "";
  descriptor.type_modifier_digest = std::string(64, ordinal == 1 ? 'a' : 'b');
  descriptor.nullable = nullable;
  return descriptor;
}

cache::CanonicalPreparedPlanResultDescriptor ResultDescriptor(
    const std::uint32_t descriptor_id) {
  cache::CanonicalPreparedPlanResultDescriptor descriptor;
  descriptor.ordinal = 1;
  descriptor.descriptor_id = descriptor_id;
  descriptor.descriptor_uuid = Uuid(980);
  descriptor.type_uuid = Uuid(981);
  descriptor.type_modifier_digest = std::string(64, 'c');
  descriptor.nullable = false;
  return descriptor;
}

cache::CanonicalPreparePhysicalPlanRequest PrepareRequest() {
  cache::CanonicalPreparePhysicalPlanRequest request;
  request.prepared_plan_uuid = Uuid(950);
  request.prepare_generation = 7;
  request.parameter_shape_uuid = Uuid(951);
  request.result_schema_uuid = Uuid(952);
  request.selected_physical_dag = PublishedDag();
  request.parameters = {
      Parameter(1, 9001, 960, false),
      Parameter(2, 9002, 961, true),
  };
  const auto root = std::ranges::find_if(
      request.selected_physical_dag.nodes, [&](const auto& node) {
        return node.physical_node_id ==
               request.selected_physical_dag.root_physical_node_id;
      });
  if (root != request.selected_physical_dag.nodes.end() &&
      !root->output_descriptor_ids.empty()) {
    request.result_descriptors = {
        ResultDescriptor(root->output_descriptor_ids.front())};
  }
  request.dependencies = {
      {cache::CanonicalPreparedPlanDependencyKind::kObject, Uuid(970), 11,
       std::string(64, 'd')},
      {cache::CanonicalPreparedPlanDependencyKind::kDatatype, Uuid(971), 4,
       std::string(64, 'e')},
  };
  request.engine_prepare_authorized = true;
  return request;
}

bool ExactPreparedNode(const cache::CanonicalPreparedPhysicalNode& prepared,
                       const exec::PhysicalNodeRecord& published) {
  return prepared.physical_node_id == published.physical_node_id &&
         prepared.relational_node_id == published.relational_node_id &&
         prepared.node_kind == published.node_kind &&
         prepared.logical_semantic_variant_id ==
             published.logical_semantic_variant_id &&
         prepared.implementation_id == published.implementation_id &&
         prepared.input_physical_node_ids ==
             published.input_physical_node_ids &&
         prepared.output_descriptor_ids == published.output_descriptor_ids &&
         prepared.shareable == published.shareable &&
         prepared.publication_ordinal == published.publication_ordinal &&
         prepared.causal_counter_id == published.causal_counter_id &&
         prepared.selected_alternative_uuid ==
             published.selected_alternative_uuid &&
         prepared.transformation_uuid == published.transformation_uuid &&
         prepared.transformation_rule_id == published.transformation_rule_id &&
         prepared.executor_capability_uuid ==
             published.executor_capability_uuid &&
         prepared.executor_capability_abi_version ==
             published.executor_capability_abi_version &&
         prepared.cost_vector_uuid == published.cost_vector_uuid &&
         prepared.required_property_uuids ==
             published.required_property_uuids &&
         prepared.delivered_property_uuids ==
             published.delivered_property_uuids &&
         prepared.enforced_property_uuids ==
             published.enforced_property_uuids &&
         prepared.retained_cost.cost_vector_uuid ==
             published.retained_cost.cost_vector_uuid &&
         prepared.retained_cost.scalar_score ==
             published.retained_cost.scalar_score &&
         prepared.memory_bytes_required == published.memory_bytes_required &&
         prepared.spill_bytes_expected == published.spill_bytes_expected;
}

bool ExpectPrepareRefusal(
    cache::CanonicalPreparePhysicalPlanRequest request,
    const std::string_view detail) {
  cache::CanonicalPreparedPlanStore prepared_plan_store;
  const auto result =
      cache::PrepareCanonicalPhysicalPlan(request, &prepared_plan_store);
  return RequirePrepare(
      !result.accepted && !result.prepared && !result.persisted &&
          !result.prepared_plan && prepared_plan_store.Size() == 0 &&
          !result.execution_authority_granted && result.issues.size() == 1 &&
          result.issues.front().diagnostic_id ==
              "QOW-DIAG-OPT-010-PREPARE-REFUSAL-V1" &&
          !result.issues.front().field_id.empty(),
      detail);
}

bool ValidatePreparedPhysicalPlan() {
  auto request = PrepareRequest();
  const auto published = request.selected_physical_dag;
  cache::CanonicalPreparedPlanStore prepared_plan_store;
  const auto result =
      cache::PrepareCanonicalPhysicalPlan(request, &prepared_plan_store);
  bool passed = true;
  passed &= RequirePrepare(
      exec::ValidateTypedPhysicalNodeDag(published).accepted,
      "RCP-065 fixture did not supply a valid complete physical DAG");
  passed &= RequirePrepare(
      result.accepted && result.prepared && result.persisted &&
          result.issues.empty() && prepared_plan_store.Size() == 1 &&
          result.immutable_physical_identity_retained &&
          result.complete_parameter_typing_retained &&
          result.complete_dependency_generations_retained &&
          result.result_schema_retained && !result.parameter_values_retained &&
          !result.prepare_statement_authority_retained &&
          !result.execution_authority_granted && result.prepared_plan,
      "PREPARE did not return one authority-free immutable plan");
  if (!result.prepared_plan) return false;
  const auto& prepared = *result.prepared_plan;
  passed &= RequirePrepare(
      prepared_plan_store.Find(request.prepared_plan_uuid) ==
          result.prepared_plan,
      "PREPARE did not persist the validated immutable artifact");
  const auto duplicate_result =
      cache::PrepareCanonicalPhysicalPlan(request, &prepared_plan_store);
  passed &= RequirePrepare(
      !duplicate_result.accepted && !duplicate_result.prepared &&
          !duplicate_result.persisted && !duplicate_result.prepared_plan &&
          prepared_plan_store.Size() == 1 &&
          prepared_plan_store.Find(request.prepared_plan_uuid) ==
              result.prepared_plan,
      "duplicate PREPARE replaced an already persisted immutable artifact");
  passed &= RequirePrepare(
      prepared.prepared_plan_uuid == Uuid(950) &&
          prepared.prepare_generation == 7 &&
          prepared.parameter_shape_uuid == Uuid(951) &&
          prepared.result_schema_uuid == Uuid(952) &&
          prepared.selected_plan_uuid == published.selected_plan_uuid &&
          prepared.selected_plan_signature ==
              published.selected_plan_signature &&
          prepared.selected_scalar_score == published.selected_scalar_score &&
          prepared.root_physical_node_id ==
              published.root_physical_node_id &&
          prepared.published_node_count == published.published_node_count &&
          prepared.first_causal_counter_id ==
              published.first_causal_counter_id &&
          prepared.nodes.size() == published.nodes.size() &&
          prepared.parameters.size() == 2 &&
          prepared.dependencies.size() == 2 &&
          prepared.result_descriptors.size() == 1 &&
          prepared.catalog_epoch_uuid == published.catalog_epoch_uuid &&
          prepared.catalog_generation == published.catalog_generation &&
          prepared.statistics_generation ==
              published.statistics_generation &&
          prepared.route_generation == published.route_generation &&
          prepared.immutable_physical_identity_retained &&
          prepared.complete_cost_vectors_retained &&
          !prepared.parameter_values_retained &&
          !prepared.prepare_statement_authority_retained &&
          !prepared.execution_authority_granted,
      "prepared identity, schema, parameters, or dependency generations drifted");
  for (std::size_t index = 0; index < published.nodes.size(); ++index) {
    passed &= RequirePrepare(
        ExactPreparedNode(prepared.nodes[index], published.nodes[index]),
        "prepared physical node differs from the published selection");
  }

  request.selected_physical_dag.nodes.front().implementation_id =
      "mutated.after.prepare.v1";
  request.parameters.front().type_uuid = Uuid(999);
  request.dependencies.front().generation = 999;
  passed &= RequirePrepare(
      prepared.nodes.front().implementation_id !=
              request.selected_physical_dag.nodes.front().implementation_id &&
          prepared.parameters.front().type_uuid !=
              request.parameters.front().type_uuid &&
          prepared.dependencies.front().generation !=
              request.dependencies.front().generation,
      "prepared immutable receipt aliased mutable caller storage");

  auto no_parameters = PrepareRequest();
  no_parameters.parameters.clear();
  no_parameters.parameter_shape_uuid = Uuid(953);
  cache::CanonicalPreparedPlanStore no_parameter_store;
  const auto no_parameter_result =
      cache::PrepareCanonicalPhysicalPlan(no_parameters, &no_parameter_store);
  passed &= RequirePrepare(
      no_parameter_result.accepted && no_parameter_result.persisted &&
          no_parameter_store.Size() == 1 &&
          no_parameter_result.prepared_plan &&
          no_parameter_result.prepared_plan->parameters.empty(),
      "zero-parameter PREPARE did not retain an exact empty shape");
  return passed;
}

bool ValidatePrepareRefusals() {
  bool passed = true;
  auto request = PrepareRequest();
  const auto missing_store =
      cache::PrepareCanonicalPhysicalPlan(request, nullptr);
  passed &= RequirePrepare(
      !missing_store.accepted && !missing_store.prepared &&
          !missing_store.persisted && !missing_store.prepared_plan &&
          missing_store.issues.size() == 1 &&
          missing_store.issues.front().diagnostic_id ==
              "QOW-DIAG-OPT-010-PREPARE-REFUSAL-V1" &&
          missing_store.issues.front().field_id == "prepared_plan_store",
      "PREPARE accepted without an engine-owned persistence store");

  request = PrepareRequest();
  request.engine_prepare_authorized = false;
  passed &= ExpectPrepareRefusal(
      request, "PREPARE admitted missing engine authority");

  request = PrepareRequest();
  request.parameter_values_supplied = true;
  passed &= ExpectPrepareRefusal(
      request, "PREPARE retained execution-time parameter values");

  request = PrepareRequest();
  request.parser_execution_authority_claimed = true;
  passed &= ExpectPrepareRefusal(
      request, "parser execution authority reached PREPARE");

  request = PrepareRequest();
  request.transaction_visibility_authority_claimed = true;
  passed &= ExpectPrepareRefusal(
      request, "transaction visibility authority leaked into PREPARE");

  request = PrepareRequest();
  request.transaction_finality_authority_claimed = true;
  passed &= ExpectPrepareRefusal(
      request, "transaction finality authority leaked into PREPARE");

  request = PrepareRequest();
  request.recovery_authority_claimed = true;
  passed &= ExpectPrepareRefusal(
      request, "recovery authority leaked into PREPARE");

  request = PrepareRequest();
  request.selected_physical_dag.data_access_observed = true;
  passed &= ExpectPrepareRefusal(
      request, "post-access physical DAG reached PREPARE");

  request = PrepareRequest();
  request.selected_physical_dag.descriptor_contract_validated = false;
  passed &= ExpectPrepareRefusal(
      request, "incomplete physical publication reached PREPARE");

  request = PrepareRequest();
  ++request.selected_physical_dag.nodes.front().retained_cost.cpu_units;
  passed &= ExpectPrepareRefusal(
      request, "changed physical cost receipt reached PREPARE");

  request = PrepareRequest();
  std::swap(request.parameters[0], request.parameters[1]);
  passed &= ExpectPrepareRefusal(
      request, "reordered typed parameters reached PREPARE");

  request = PrepareRequest();
  request.parameters[1].descriptor_id = request.parameters[0].descriptor_id;
  passed &= ExpectPrepareRefusal(
      request, "duplicate parameter descriptor reached PREPARE");

  request = PrepareRequest();
  request.parameters[0].type_uuid = "not-a-uuid";
  passed &= ExpectPrepareRefusal(
      request, "untyped parameter reached PREPARE");

  request = PrepareRequest();
  request.prepared_plan_uuid = "00000000-0000-0000-0000-000000000000";
  passed &= ExpectPrepareRefusal(
      request, "nil prepared-plan identity reached PREPARE");

  request = PrepareRequest();
  std::reverse(request.dependencies.begin(), request.dependencies.end());
  passed &= ExpectPrepareRefusal(
      request, "noncanonical dependency order reached PREPARE");

  request = PrepareRequest();
  request.dependencies.front().generation = 0;
  passed &= ExpectPrepareRefusal(
      request, "ungenerated dependency reached PREPARE");

  request = PrepareRequest();
  request.dependencies.front().definition_digest = "not-a-digest";
  passed &= ExpectPrepareRefusal(
      request, "unversioned dependency definition reached PREPARE");

  request = PrepareRequest();
  request.dependencies.clear();
  passed &= ExpectPrepareRefusal(
      request, "dependency-free physical plan reached PREPARE");

  request = PrepareRequest();
  request.dependencies.insert(request.dependencies.begin() + 1,
                              request.dependencies.front());
  passed &= ExpectPrepareRefusal(
      request, "duplicate generation-qualified dependency reached PREPARE");

  request = PrepareRequest();
  ++request.result_descriptors.front().descriptor_id;
  passed &= ExpectPrepareRefusal(
      request, "result schema drift reached PREPARE");
  return passed;
}

}  // namespace

// QOW-TEST-OPT-010-PREPARE-V1
int main() {
  bool passed = true;
  passed &= ValidatePreparedPhysicalPlan();
  passed &= ValidatePrepareRefusals();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
