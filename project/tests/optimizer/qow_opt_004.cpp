// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/plan_api.hpp"
#include "query/canonical_relational_bridge.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace plan = scratchbird::engine::planner;

namespace scratchbird::engine::internal_api {

struct CanonicalAccessIndexMetadataV1 {
  std::string index_uuid;
  std::string relation_uuid;
  std::string alternative_uuid;
  std::string capability_uuid;
  std::string implementation_id;
  std::vector<std::uint32_t> key_expression_ids;
  std::uint64_t catalog_generation{0};
  std::uint64_t relation_descriptor_generation{0};
  std::uint64_t index_generation{0};
  std::uint64_t statistics_generation{0};
  std::uint64_t statistics_index_generation{0};
  std::uint64_t statistics_catalog_generation{0};
  std::uint64_t visible_generation{0};
  bool catalog_record_current{false};
  bool lifecycle_ready{false};
  bool build_validation_complete{false};
  bool profile_authoritative{false};
  bool profile_supports_mga_visibility{false};
  bool profile_supports_generation_visibility{false};
  bool supports_exact_lookup{false};
  bool supports_range_scan{false};
  bool statistics_present{false};
  bool statistics_current{false};
  bool statistics_stale{false};
  bool statistics_profile_coupled{false};
  bool statistics_mga_visible{false};
  bool visibility_evidence_engine_owned{false};
  bool visible_to_statement_snapshot{false};
  bool approximate{false};
  bool exact_fallback{false};
  bool residual_recheck_required{false};
  bool data_access_observed{false};
};

struct CanonicalAccessCandidateReceiptV1 {
  std::string alternative_uuid;
  std::string index_uuid;
  std::string implementation_id;
  std::string capability_uuid;
  std::uint32_t logical_node_id{0};
  std::uint64_t catalog_generation{0};
  std::uint64_t relation_descriptor_generation{0};
  std::uint64_t index_generation{0};
  std::uint64_t statistics_generation{0};
  std::uint64_t visible_generation{0};
  bool available{false};
  bool heap_fallback{false};
  bool capability_validated{false};
  bool generation_validated{false};
  bool statistics_validated{false};
  bool visibility_validated{false};
  bool residual_recheck_required{false};
  std::string refusal_diagnostic_id;
};

struct CanonicalAccessCandidatePlanningRequestV1 {
  scratchbird::engine::planner::CanonicalLogicalRelationalGraph logical_graph;
  std::uint32_t logical_node_id{0};
  std::string relation_uuid;
  std::vector<std::uint32_t> predicate_expression_ids;
  std::string predicate_kind;
  std::string heap_alternative_uuid;
  std::string heap_capability_uuid;
  std::string statistics_snapshot_uuid;
  std::uint64_t current_catalog_generation{0};
  std::uint64_t current_relation_descriptor_generation{0};
  std::uint64_t current_statistics_generation{0};
  std::size_t maximum_candidate_count{0};
  bool metadata_snapshot_engine_owned{false};
  bool storage_descriptor_engine_owned{false};
  bool statistics_snapshot_engine_owned{false};
  bool data_access_observed{false};
  bool parser_planning_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  std::vector<CanonicalAccessIndexMetadataV1> indexes;
};

struct CanonicalAccessCandidatePlanningResultV1 {
  bool accepted{false};
  bool planning_complete_before_access{false};
  bool data_access_allowed{false};
  scratchbird::engine::planner::CanonicalPhysicalAlternativeCatalog catalog;
  std::vector<CanonicalAccessCandidateReceiptV1> receipts;
  std::string diagnostic_id;
  std::string field_id;
};

CanonicalAccessCandidatePlanningResultV1
QowGenerateCanonicalAccessCandidatesV1(
    const CanonicalAccessCandidatePlanningRequestV1& request);

bool QowValidateCanonicalSelectedAccessExecutionV1(
    const CanonicalAccessCandidatePlanningResultV1& planning,
    const std::string& selected_alternative_uuid,
    const scratchbird::engine::executor::TypedPhysicalNodeDag& physical_dag,
    const CanonicalOptimizerSelectedExecutionResult& execution,
    std::string* diagnostic_id,
    std::string* field_id);

}  // namespace scratchbird::engine::internal_api

namespace {

constexpr std::uint64_t kOwner = 0xffff'ffff'ffff'ff00ULL;
constexpr std::uint64_t kOldestActive = 0xffff'ffff'ffff'fee8ULL;
constexpr std::uint64_t kHorizon = 0xffff'ffff'ffff'fed0ULL;
constexpr std::uint64_t kInDoubt = 0xffff'ffff'ffff'fef0ULL;
constexpr std::uint64_t kInventoryNext = 0xffff'ffff'ffff'fff0ULL;
constexpr std::uint64_t kCatalogGeneration = 11;
constexpr std::uint64_t kDescriptorGeneration = 7;
constexpr std::uint64_t kStatisticsGeneration = 17;

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-OPT-004-V1: " << detail << '\n';
  return condition;
}

std::string Uuid(const std::uint64_t suffix) {
  auto text = std::string("019f0000-0000-7400-8000-000000000000");
  const auto digits = std::to_string(suffix);
  text.replace(text.size() - digits.size(), digits.size(), digits);
  return text;
}

plan::CanonicalMgaStatementContext MgaContext() {
  plan::CanonicalMgaStatementContext context;
  context.statement_uuid = Uuid(9001);
  context.owning_transaction_uuid = Uuid(9002);
  context.statement_snapshot_uuid = Uuid(9003);
  context.statement_metadata_snapshot_uuid = Uuid(9004);
  context.owning_local_transaction_id = kOwner;
  context.visible_committed_high_watermark = 0;
  context.oldest_active_transaction_id = kOldestActive;
  context.oldest_interesting_transaction_id = kHorizon;
  context.oldest_snapshot_transaction_id = kHorizon;
  context.retention_horizon_transaction_id = kHorizon;
  context.active_excluded_local_transaction_ids = {kOldestActive, kOwner};
  context.in_doubt_excluded_local_transaction_ids = {kInDoubt};
  context.snapshot_kind = "statement_stable";
  context.publication_inventory_next_local_transaction_id = kInventoryNext;
  context.inventory_authoritative = true;
  context.complete = true;
  context.current = true;
  return context;
}

plan::CanonicalLogicalRelationalGraph Graph() {
  plan::CanonicalLogicalRelationalGraph graph;
  graph.bound_sblr_tree_uuid = Uuid(1);
  graph.catalog_epoch_uuid = Uuid(2);
  graph.security_context_uuid = Uuid(3);
  graph.local_transaction_id = kOwner;
  graph.statement_snapshot_id = 0;
  graph.mga_statement_context = MgaContext();
  graph.root_logical_node_id = 1;
  graph.result_descriptor_ids = {101};
  plan::CanonicalLogicalRelationalNode source;
  source.logical_node_id = 1;
  source.node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
  source.output_descriptor_ids = {101};
  source.bound_expression_ids = {501};
  source.origin_relational_node_ids = {1};
  source.required_object_uuids = {Uuid(10)};
  source.semantic_variant_id = "relation.source.v1";
  graph.nodes.push_back(std::move(source));
  return graph;
}

api::CanonicalAccessIndexMetadataV1 ReadyIndex(
    const std::uint64_t ordinal,
    std::string implementation_id = "scan.index.btree.v1") {
  api::CanonicalAccessIndexMetadataV1 index;
  index.index_uuid = Uuid(100 + ordinal);
  index.relation_uuid = Uuid(10);
  index.alternative_uuid = Uuid(200 + ordinal);
  index.capability_uuid = Uuid(300 + ordinal);
  index.implementation_id = std::move(implementation_id);
  index.key_expression_ids = {501, 502};
  index.catalog_generation = kCatalogGeneration;
  index.relation_descriptor_generation = kDescriptorGeneration;
  index.index_generation = 5;
  index.statistics_generation = kStatisticsGeneration;
  index.statistics_index_generation = 5;
  index.statistics_catalog_generation = kCatalogGeneration;
  index.visible_generation = 4;
  index.catalog_record_current = true;
  index.lifecycle_ready = true;
  index.build_validation_complete = true;
  index.profile_authoritative = true;
  index.profile_supports_mga_visibility = true;
  index.profile_supports_generation_visibility = true;
  index.supports_exact_lookup = true;
  index.supports_range_scan = true;
  index.statistics_present = true;
  index.statistics_current = true;
  index.statistics_profile_coupled = true;
  index.statistics_mga_visible = true;
  index.visibility_evidence_engine_owned = true;
  index.visible_to_statement_snapshot = true;
  index.residual_recheck_required = true;
  return index;
}

api::CanonicalAccessCandidatePlanningRequestV1 Request() {
  api::CanonicalAccessCandidatePlanningRequestV1 request;
  request.logical_graph = Graph();
  request.logical_node_id = 1;
  request.relation_uuid = Uuid(10);
  request.predicate_expression_ids = {501};
  request.predicate_kind = "exact";
  request.heap_alternative_uuid = Uuid(20);
  request.heap_capability_uuid = Uuid(21);
  request.statistics_snapshot_uuid = Uuid(22);
  request.current_catalog_generation = kCatalogGeneration;
  request.current_relation_descriptor_generation = kDescriptorGeneration;
  request.current_statistics_generation = kStatisticsGeneration;
  request.maximum_candidate_count = 32;
  request.metadata_snapshot_engine_owned = true;
  request.storage_descriptor_engine_owned = true;
  request.statistics_snapshot_engine_owned = true;
  request.indexes = {ReadyIndex(1)};
  return request;
}

const api::CanonicalAccessCandidateReceiptV1* Receipt(
    const api::CanonicalAccessCandidatePlanningResultV1& result,
    const std::string_view implementation_id) {
  const auto found = std::ranges::find_if(
      result.receipts, [&](const auto& receipt) {
        return receipt.implementation_id == implementation_id;
      });
  return found == result.receipts.end() ? nullptr : &*found;
}

bool ValidateCurrentCandidatesAndExplicitRefusals() {
  auto request = Request();
  auto stale = ReadyIndex(2, "scan.index.stale.v1");
  stale.statistics_stale = true;
  auto invisible = ReadyIndex(3, "scan.index.invisible.v1");
  invisible.visible_to_statement_snapshot = false;
  auto generation = ReadyIndex(4, "scan.index.generation.v1");
  ++generation.statistics_index_generation;
  auto capability = ReadyIndex(5, "scan.index.capability.v1");
  capability.supports_exact_lookup = false;
  auto predicate = ReadyIndex(6, "scan.index.predicate.v1");
  predicate.key_expression_ids = {999};
  auto late = ReadyIndex(7, "scan.index.late.v1");
  late.data_access_observed = true;
  request.indexes.insert(request.indexes.end(),
                         {stale, invisible, generation, capability,
                          predicate, late});

  const auto result = api::QowGenerateCanonicalAccessCandidatesV1(request);
  const auto* heap = Receipt(result, "scan.heap.v1");
  const auto* ready = Receipt(result, "scan.index.btree.v1");
  const auto* stale_receipt = Receipt(result, "scan.index.stale.v1");
  const auto* invisible_receipt =
      Receipt(result, "scan.index.invisible.v1");
  const auto* generation_receipt =
      Receipt(result, "scan.index.generation.v1");
  const auto* capability_receipt =
      Receipt(result, "scan.index.capability.v1");
  const auto* predicate_receipt =
      Receipt(result, "scan.index.predicate.v1");
  const auto* late_receipt = Receipt(result, "scan.index.late.v1");
  bool passed = true;
  passed &= Require(result.accepted &&
                        result.planning_complete_before_access &&
                        !result.data_access_allowed &&
                        result.catalog.alternatives.size() == 8 &&
                        result.receipts.size() == 8,
                    "complete catalog-backed candidate set was not retained");
  passed &= Require(heap && heap->available && heap->heap_fallback &&
                        ready && ready->available &&
                        ready->capability_validated &&
                        ready->generation_validated &&
                        ready->statistics_validated &&
                        ready->visibility_validated &&
                        ready->residual_recheck_required,
                    "heap fallback or current index lost planning evidence");
  passed &= Require(
      stale_receipt && !stale_receipt->available &&
          stale_receipt->refusal_diagnostic_id ==
              "QOW-DIAG-OPT-004-STATISTICS-V1" &&
          invisible_receipt && !invisible_receipt->available &&
          invisible_receipt->refusal_diagnostic_id ==
              "QOW-DIAG-OPT-004-VISIBILITY-V1" &&
          generation_receipt && !generation_receipt->available &&
          generation_receipt->refusal_diagnostic_id ==
              "QOW-DIAG-OPT-004-STATISTICS-V1" &&
          capability_receipt && !capability_receipt->available &&
          capability_receipt->refusal_diagnostic_id ==
              "QOW-DIAG-OPT-004-CAPABILITY-V1" &&
          predicate_receipt && !predicate_receipt->available &&
          predicate_receipt->refusal_diagnostic_id ==
              "QOW-DIAG-OPT-004-PREDICATE-V1" &&
          late_receipt && !late_receipt->available &&
          late_receipt->refusal_diagnostic_id ==
              "QOW-DIAG-OPT-004-PHASE-V1",
      "unselectable indexes were not preserved with exact refusal causes");
  passed &= Require(
      plan::ValidateCanonicalLogicalPhysicalBoundary(
          request.logical_graph, result.catalog)
          .accepted,
      "generated access alternatives failed the canonical boundary");
  return passed;
}

bool ValidateAtomicPlanningRefusals() {
  const auto expect = [](auto mutation, const std::string_view diagnostic,
                         const std::string_view detail) {
    auto request = Request();
    mutation(request);
    const auto result = api::QowGenerateCanonicalAccessCandidatesV1(request);
    return Require(!result.accepted && !result.planning_complete_before_access &&
                       !result.data_access_allowed &&
                       result.catalog.alternatives.empty() &&
                       result.receipts.empty() &&
                       result.diagnostic_id == diagnostic,
                   detail);
  };
  bool passed = true;
  passed &= expect([](auto& request) { request.data_access_observed = true; },
                   "QOW-DIAG-OPT-004-AUTHORITY-V1",
                   "planning after data access was not refused atomically");
  passed &= expect(
      [](auto& request) { request.maximum_candidate_count = 1; },
      "SBLR.PLAN_TREE.RESOURCE_LIMIT",
      "candidate resource overflow was not refused atomically");
  passed &= expect(
      [](auto& request) {
        auto duplicate = request.indexes.front();
        duplicate.implementation_id = "scan.index.duplicate.v1";
        duplicate.alternative_uuid = Uuid(299);
        request.indexes.push_back(std::move(duplicate));
      },
      "QOW-DIAG-OPT-004-INDEX-IDENTITY-V1",
      "duplicate current index identity was accepted");
  passed &= expect(
      [](auto& request) {
        request.parser_planning_authority_claimed = true;
      },
      "QOW-DIAG-OPT-004-AUTHORITY-V1",
      "parser planning authority reached catalog access planning");
  return passed;
}

exec::PhysicalMgaStatementContext PhysicalContext() {
  const auto source = MgaContext();
  exec::PhysicalMgaStatementContext context;
  context.statement_uuid = source.statement_uuid;
  context.owning_transaction_uuid = source.owning_transaction_uuid;
  context.statement_snapshot_uuid = source.statement_snapshot_uuid;
  context.statement_metadata_snapshot_uuid =
      source.statement_metadata_snapshot_uuid;
  context.owning_local_transaction_id = source.owning_local_transaction_id;
  context.visible_committed_high_watermark =
      source.visible_committed_high_watermark;
  context.oldest_active_transaction_id = source.oldest_active_transaction_id;
  context.oldest_interesting_transaction_id =
      source.oldest_interesting_transaction_id;
  context.oldest_snapshot_transaction_id =
      source.oldest_snapshot_transaction_id;
  context.retention_horizon_transaction_id =
      source.retention_horizon_transaction_id;
  context.active_excluded_local_transaction_ids =
      source.active_excluded_local_transaction_ids;
  context.in_doubt_excluded_local_transaction_ids =
      source.in_doubt_excluded_local_transaction_ids;
  context.snapshot_kind = source.snapshot_kind;
  context.publication_inventory_next_local_transaction_id =
      source.publication_inventory_next_local_transaction_id;
  context.inventory_authoritative = source.inventory_authoritative;
  context.complete = source.complete;
  context.current = source.current;
  return context;
}

exec::TypedPhysicalNodeDag SelectedDag(
    const api::CanonicalAccessCandidatePlanningResultV1& planning) {
  const auto* selected = Receipt(planning, "scan.index.btree.v1");
  exec::TypedPhysicalNodeDag dag;
  dag.abi_version = 2;
  dag.selected_plan_uuid = Uuid(800);
  dag.root_physical_node_id = 1;
  dag.local_transaction_id = kOwner;
  dag.statement_snapshot_id = 0;
  dag.mga_statement_context = PhysicalContext();
  dag.bound_sblr_tree_uuid = Uuid(1);
  dag.catalog_epoch_uuid = Uuid(2);
  dag.security_context_uuid = Uuid(3);
  dag.capability_snapshot_uuid = Uuid(801);
  dag.resource_snapshot_uuid = Uuid(802);
  dag.statistics_snapshot_uuid = Uuid(22);
  dag.route_snapshot_uuid = Uuid(803);
  dag.catalog_generation = kCatalogGeneration;
  dag.security_epoch = 2;
  dag.policy_epoch = 3;
  dag.resource_epoch = 4;
  dag.statistics_generation = kStatisticsGeneration;
  dag.route_epoch = 5;
  dag.route_generation = 6;
  dag.memory_budget_bytes = 1 << 20;
  dag.optimizer_published = true;
  dag.immutable_node_identity_validated = true;
  dag.capability_validated_before_access = true;
  dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest, dag.bound_sblr_tree_uuid},
      {exec::PhysicalAdmissionStage::kCatalogEpoch, dag.catalog_epoch_uuid},
      {exec::PhysicalAdmissionStage::kSecurity, dag.security_context_uuid},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       dag.mga_statement_context.statement_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       dag.capability_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kResource, dag.resource_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       dag.statistics_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kCanonicalRoute, dag.route_snapshot_uuid},
  };
  exec::PhysicalNodeRecord node;
  node.physical_node_id = 1;
  node.relational_node_id = selected->logical_node_id;
  node.node_kind = exec::PhysicalNodeKind::kScan;
  node.implementation_id = selected->implementation_id;
  node.output_descriptor_ids = {101};
  node.causal_counter_id = 90001;
  node.selected_alternative_uuid = selected->alternative_uuid;
  node.executor_capability_uuid = selected->capability_uuid;
  node.executor_capability_abi_version = 1;
  node.cost_vector_uuid = Uuid(804);
  node.memory_bytes_required = 1024;
  node.engine_capability_validated = true;
  node.mga_statement_context = dag.mga_statement_context;
  dag.nodes.push_back(std::move(node));
  return dag;
}

exec::CanonicalExecutionMgaAuthority MgaAuthority(
    const exec::TypedPhysicalNodeDag& dag) {
  exec::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = dag.mga_statement_context;
  authority.origin =
      exec::CanonicalMgaAuthorityOrigin::kEngineTransactionInventory;
  const auto current = authority.statement_context;
  authority.resolve_current = [current] {
    exec::CanonicalMgaCurrentResolution result;
    result.statement_context = current;
    return result;
  };
  return authority;
}

api::EngineDescriptor ResultDescriptor() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = Uuid(820);
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=" + Uuid(821) + ";nullability=non_null";
  return descriptor;
}

exec::DescriptorBatch ResultBatch() {
  auto descriptor = ResultDescriptor();
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.state = api::EngineValueState::value;
  value.encoded_value = "7";
  return exec::MakeDescriptorBatch(
      {{"id", descriptor, false, 101}}, {{{std::move(value)}}});
}

api::CanonicalOptimizerSelectedExecutionRequest ExecutionRequest(
    const exec::TypedPhysicalNodeDag& dag,
    std::size_t* invocation_count) {
  api::CanonicalOptimizerSelectedExecutionRequest request;
  request.selected_physical_dag = dag;
  request.pre_access_statistics_snapshot_uuid = dag.statistics_snapshot_uuid;
  request.mga_authority = MgaAuthority(dag);
  request.engine_execution_authorized = true;

  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kScan;
  registration.implementation_id = dag.nodes.front().implementation_id;
  registration.executor_capability_uuid =
      dag.nodes.front().executor_capability_uuid;
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute = [invocation_count](const auto& selected_dag,
                                             const auto& node,
                                             const auto& inputs) {
    exec::CanonicalPhysicalDispatchStepResult step;
    ++*invocation_count;
    if (!inputs.empty()) {
      step.diagnostic.ok = false;
      step.diagnostic.diagnostic_code = "TEST_OPT004_SCAN_INPUT";
      return step;
    }
    step.selected_plan_uuid = selected_dag.selected_plan_uuid;
    step.executed_physical_node_id = node.physical_node_id;
    step.causal_counter_id = node.causal_counter_id;
    step.result_handle_id = 4001;
    step.output_descriptor_ids = node.output_descriptor_ids;
    step.authority.engine_mga_snapshot_bound = true;
    step.output_row_count = 1;
    step.rows_examined = 1;
    step.pages_read = 1;
    step.data_access_observation_known = true;
    step.data_access_observed = true;
    step.materialized_output_batch = ResultBatch();
    step.mga_statement_context = selected_dag.mga_statement_context;
    return step;
  };
  request.available_executors.push_back(std::move(registration));

  auto& publication = request.result_publication_request;
  publication.statement_uuid = dag.mga_statement_context.statement_uuid;
  publication.execution_attempt_uuid = Uuid(830);
  publication.transaction_effect_evidence_uuid = Uuid(831);
  publication.selected_catalog_epoch_uuid = dag.catalog_epoch_uuid;
  publication.result_kind = exec::CanonicalResultKind::kRows;
  publication.column_bindings = {{
      0,
      true,
      exec::CanonicalResultColumnDescriptor{
          0, "id", Uuid(820), Uuid(821),
          exec::CanonicalResultNullability::kNonNull, std::nullopt,
          std::nullopt},
  }};
  return request;
}

bool ValidateCausalSelectedAccessExecution() {
  const auto planning =
      api::QowGenerateCanonicalAccessCandidatesV1(Request());
  auto dag = SelectedDag(planning);
  std::size_t invocation_count = 0;
  const auto execution = api::ExecuteCanonicalOptimizerSelectedDag(
      ExecutionRequest(dag, &invocation_count));
  std::string diagnostic;
  std::string field;
  bool passed = true;
  passed &= Require(exec::ValidateTypedPhysicalNodeDag(dag).accepted,
                    "selected index DAG fixture was not canonical");
  passed &= Require(
      invocation_count == 1 && execution.accepted &&
          execution.data_access_observed &&
          api::QowValidateCanonicalSelectedAccessExecutionV1(
              planning, Uuid(201), dag, execution, &diagnostic, &field) &&
          diagnostic.empty() && field.empty(),
      "selected index did not produce a causal canonical execution receipt");

  auto wrong_alternative = dag;
  wrong_alternative.nodes.front().selected_alternative_uuid = Uuid(20);
  passed &= Require(
      !api::QowValidateCanonicalSelectedAccessExecutionV1(
          planning, Uuid(201), wrong_alternative, execution, &diagnostic,
          &field) &&
          diagnostic == "QOW-DIAG-OPT-004-PUBLICATION-V1",
      "fixed hidden access route passed selected-alternative validation");

  auto wrong_counter = execution;
  ++wrong_counter.dispatch.executed_steps.front().causal_counter_id;
  passed &= Require(
      !api::QowValidateCanonicalSelectedAccessExecutionV1(
          planning, Uuid(201), dag, wrong_counter, &diagnostic, &field) &&
          diagnostic == "QOW-DIAG-OPT-004-CAUSAL-RECEIPT-V1",
      "mutated causal counter passed execution receipt validation");

  passed &= Require(
      !api::QowValidateCanonicalSelectedAccessExecutionV1(
          planning, Uuid(20), dag, execution, &diagnostic, &field) &&
          diagnostic == "QOW-DIAG-OPT-004-PUBLICATION-V1",
      "heap receipt accepted index execution identity");
  return passed;
}

}  // namespace

// QOW-TEST-OPT-004-V1
int main() {
  bool passed = true;
  passed &= ValidateCurrentCandidatesAndExplicitRefusals();
  passed &= ValidateAtomicPlanningRefusals();
  passed &= ValidateCausalSelectedAccessExecution();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
