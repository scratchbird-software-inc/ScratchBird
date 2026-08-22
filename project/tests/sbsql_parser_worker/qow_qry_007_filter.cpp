// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-QRY-007-FILTER-V1: " << detail << '\n';
  return condition;
}

std::string ContextUuid(const unsigned value) {
  char buffer[37]{};
  std::snprintf(buffer, sizeof(buffer),
                "019f0000-0000-7630-8000-%012u", value);
  return buffer;
}

exec::CanonicalExecutionMgaAuthority ClosureAuthority(
    const exec::PhysicalMgaStatementContext& carried,
    const exec::PhysicalMgaStatementContext& current) {
  exec::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = carried;
  authority.origin = exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  authority.resolve_current = [current] {
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = current;
    return resolution;
  };
  return authority;
}

exec::CanonicalExecutionMgaAuthority ClosureAuthority(
    const exec::PhysicalMgaStatementContext& context) {
  return ClosureAuthority(context, context);
}

void UpgradeToCanonicalStatementContext(exec::TypedPhysicalNodeDag* dag,
                                        const unsigned seed) {
  const auto local = (std::uint64_t{1} << 32) + dag->local_transaction_id;
  const auto highwater =
      (std::uint64_t{1} << 32) + dag->statement_snapshot_id;
  dag->abi_version = 2;
  dag->local_transaction_id = local;
  dag->statement_snapshot_id = highwater;
  dag->mga_statement_context = {
      ContextUuid(seed + 1), ContextUuid(seed + 2), ContextUuid(seed + 3),
      ContextUuid(seed + 4), local, highwater, local, local, local, local,
      {local}, {}, "statement_stable", highwater + 1, true, true, true};
  dag->bound_sblr_tree_uuid = ContextUuid(seed + 10);
  dag->catalog_epoch_uuid = ContextUuid(seed + 11);
  dag->security_context_uuid = ContextUuid(seed + 12);
  dag->capability_snapshot_uuid = ContextUuid(seed + 13);
  dag->resource_snapshot_uuid = ContextUuid(seed + 14);
  dag->statistics_snapshot_uuid = ContextUuid(seed + 15);
  dag->route_snapshot_uuid = ContextUuid(seed + 16);
  dag->catalog_generation = seed + 21;
  dag->security_epoch = seed + 22;
  dag->policy_epoch = seed + 23;
  dag->resource_epoch = seed + 24;
  dag->statistics_generation = seed + 25;
  dag->route_epoch = seed + 26;
  dag->route_generation = seed + 27;
  dag->memory_budget_bytes = 1U << 20;
  dag->optimizer_published = true;
  dag->immutable_node_identity_validated = true;
  dag->capability_validated_before_access = true;
  dag->admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest, dag->bound_sblr_tree_uuid},
      {exec::PhysicalAdmissionStage::kCatalogEpoch, dag->catalog_epoch_uuid},
      {exec::PhysicalAdmissionStage::kSecurity, dag->security_context_uuid},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       dag->mga_statement_context.statement_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       dag->capability_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kResource, dag->resource_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       dag->statistics_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       dag->route_snapshot_uuid},
  };
  for (std::size_t index = 0; index < dag->nodes.size(); ++index) {
    auto& node = dag->nodes[index];
    node.selected_alternative_uuid = ContextUuid(seed + 100 + index * 3);
    node.executor_capability_uuid = ContextUuid(seed + 101 + index * 3);
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid = ContextUuid(seed + 102 + index * 3);
    node.memory_bytes_required = 1024;
    node.engine_capability_validated = true;
    node.mga_statement_context = dag->mga_statement_context;
  }
}

template <typename Result>
bool AtomicStatementContextRefusal(const Result& result) {
  return !result.diagnostic.ok && result.output_batch.rows.empty() &&
         result.output_batch.columns.empty() &&
         result.selected_plan_uuid.empty() &&
         result.executed_physical_node_id == 0 &&
         result.causal_counter_id == 0 &&
         result.mga_statement_context.statement_uuid.empty();
}


api::EngineDescriptor Descriptor() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-000000007101";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "decimal";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000007102;"
      "nullability=nullable;precision=12;scale=2";
  return descriptor;
}

exec::TypedPhysicalNodeDag Dag() {
  exec::TypedPhysicalNodeDag dag;
  dag.selected_plan_uuid = "019f0000-0000-7200-8000-000000007103";
  dag.root_physical_node_id = 713;
  dag.local_transaction_id = 713;
  dag.statement_snapshot_id = 714;
  dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000007111"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000007112"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000007113"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000007114"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000007115"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000007116"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000007117"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000007118"},
  };
  dag.nodes = {
      {.physical_node_id = 711,
       .relational_node_id = 711,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {711},
       .causal_counter_id = 7101},
      {.physical_node_id = 712,
       .relational_node_id = 712,
       .node_kind = exec::PhysicalNodeKind::kFilter,
       .implementation_id = "filter.3vl.row.v1",
       .input_physical_node_ids = {711},
       .output_descriptor_ids = {711},
       .causal_counter_id = 7102},
      {.physical_node_id = 713,
       .relational_node_id = 713,
       .node_kind = exec::PhysicalNodeKind::kProject,
       .implementation_id = "project.typed.row.v1",
       .input_physical_node_ids = {712},
       .output_descriptor_ids = {711},
       .causal_counter_id = 7103},
  };
  UpgradeToCanonicalStatementContext(&dag, 7100);
  return dag;
}

exec::CanonicalDescriptorFilterRequest Request() {
  const auto descriptor = Descriptor();
  const auto value = [&](const std::string& encoded) {
    api::EngineTypedValue typed;
    typed.descriptor = descriptor;
    typed.encoded_value = encoded;
    return typed;
  };
  exec::CanonicalDescriptorFilterRequest request;
  request.physical_dag = Dag();
  request.selected_physical_node_id = 712;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"amount", descriptor, true, 711}},
      {{{value("1.00")}}, {{value("2.00")}}, {{value("3.00")}}});
  request.row_truth_values = {
      api::EngineSqlTruthValue::true_value,
      api::EngineSqlTruthValue::false_value,
      api::EngineSqlTruthValue::unknown,
  };
  request.mga_authority =
      ClosureAuthority(request.physical_dag.mga_statement_context);
  return request;
}

// QOW-TEST-QRY-007-FILTER-V1
// Positive FILTER execution is exercised through the canonical query route,
// which alone can issue the predicate receipt after evaluating the bound
// expression. This direct executor boundary proves that legacy truth-value
// sidecars cannot bypass that authority.
bool ValidateLegacyFilterSidecarRefusal() {
  bool passed = true;
  const auto require_refusal =
      [&](const exec::CanonicalDescriptorFilterRequest& request,
          const std::string_view detail) {
    const auto result = exec::ExecuteCanonicalDescriptorFilter(request);
    passed &= Require(
        result.diagnostic.diagnostic_code ==
                "QOW-DIAG-QRY-007-FILTER-PREDICATE-RECEIPT-REFUSAL-V1" &&
            AtomicStatementContextRefusal(result),
        detail);
  };

  require_refusal(Request(),
                  "legacy WHERE truth sidecar reached descriptor execution");

  auto having = Request();
  having.consumer = api::EnginePredicateConsumer::having;
  require_refusal(having,
                  "legacy HAVING truth sidecar reached descriptor execution");

  auto invalid = Request();
  invalid.row_truth_values[1] = api::EngineSqlTruthValue::unspecified;
  require_refusal(invalid,
                  "unbound legacy truth state reached descriptor execution");

  invalid = Request();
  invalid.consumer = api::EnginePredicateConsumer::join_on;
  require_refusal(invalid,
                  "legacy join-ON sidecar reached descriptor execution");

  invalid = Request();
  invalid.row_truth_values.pop_back();
  require_refusal(invalid,
                  "legacy predicate cardinality drift reached execution");
  return passed;
}

}  // namespace

int main() {
  if (!ValidateLegacyFilterSidecarRefusal()) return 1;
  std::cout << "QOW-TEST-QRY-007-FILTER-V1: PASS\n";
  return 0;
}
