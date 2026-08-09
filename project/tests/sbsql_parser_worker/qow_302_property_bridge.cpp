// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_dispatch.hpp"
#include "query/canonical_relational_bridge.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>

namespace scratchbird::engine::sblr {
SblrDispatchResult DispatchTextualRelationalQueryForContractTest(
    SblrDispatchRequest request);
}

namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

namespace {

constexpr std::string_view kCatalogEpochUuid =
    "019f0000-0000-7100-8000-000000003002";
constexpr std::string_view kSecurityContextUuid =
    "019f0000-0000-7110-8000-000000003003";
constexpr std::string_view kOrderingOneUuid =
    "019f0000-0000-7200-8000-000000003101";
constexpr std::string_view kGroupingUuid =
    "019f0000-0000-7200-8000-000000003102";
constexpr std::string_view kOrderingTwoUuid =
    "019f0000-0000-7200-8000-000000003103";
constexpr std::string_view kPartitioningUuid =
    "019f0000-0000-7200-8000-000000003104";
constexpr std::string_view kWindowUuid =
    "019f0000-0000-7200-8000-000000003105";
constexpr std::string_view kEquivalenceUuid =
    "019f0000-0000-7200-8000-000000003106";

bool Require(const bool condition, const std::string_view message) {
  if (!condition) std::cerr << "QOW-TEST-302-PROPERTY-BRIDGE-V1: "
                            << message << '\n';
  return condition;
}

bool HasApiDiagnostic(const sblr::SblrDispatchResult& result,
                      const std::string_view code) {
  for (const auto& diagnostic : result.api_result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.statement_uuid.canonical =
      "019f0000-0000-7120-8000-000000003001";
  context.transaction_uuid.canonical =
      "019f0000-0000-7130-8000-000000003004";
  context.statement_snapshot_uuid.canonical =
      "019f0000-0000-7140-8000-000000003005";
  context.catalog_epoch_uuid.canonical = kCatalogEpochUuid;
  context.local_transaction_id = 3001;
  context.snapshot_visible_through_local_transaction_id = 2999;
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_metadata_snapshot_uuid.canonical =
      "019f0000-0000-7150-8000-000000003006";
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      kSecurityContextUuid;
  context.catalog_generation_id = 3001;
  context.security_epoch = 3002;
  context.resource_epoch = 3003;
  context.optimizer_capability_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000006001";
  context.optimizer_resource_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000006002";
  context.optimizer_route_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000006003";
  context.optimizer_route_epoch = 3004;
  context.optimizer_route_generation = 3005;
  context.optimizer_memory_budget_bytes = 64 * 1024 * 1024;
  context.optimizer_maximum_candidate_count = 131072;
  context.optimizer_maximum_memo_groups = 131072;
  context.optimizer_maximum_search_steps = 1048576;
  context.optimizer_maximum_planning_time_ns = 5'000'000'000;
  context.optimizer_spill_allowed = true;
  context.current_monotonic_ns = "3001000";
  context.authorization_context.security_epoch = 3002;
  context.authorization_context.policy_epoch = 3003;
  context.authorization_context.catalog_generation_id = 3001;
  return context;
}

sblr::SblrOperationEnvelope PropertyEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "qow.302.property.bridge");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-000000003001"},
      {"uuid", "relational_catalog_epoch_uuid",
       std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uuid", "relational_statement_uuid",
       "019f0000-0000-7120-8000-000000003001"},
      {"uuid", "relational_owning_transaction_uuid",
       "019f0000-0000-7130-8000-000000003004"},
      {"uuid", "relational_statement_snapshot_uuid",
       "019f0000-0000-7140-8000-000000003005"},
      {"uuid", "relational_statement_metadata_snapshot_uuid",
       "019f0000-0000-7150-8000-000000003006"},
      {"uint64", "relational_local_transaction_id", "3001"},
      {"uint64",
       "relational_snapshot_visible_through_local_transaction_id", "2999"},
      {"uint32", "relational_root_node_id", "4"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-000000003201|"
       "019f0000-0000-7400-8000-000000003202|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-000000003203|"
       "019f0000-0000-7400-8000-000000003204|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       "019f0000-0000-7300-8000-000000003205|"
       "019f0000-0000-7400-8000-000000003206|1|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "2", "1|-|2|-|-|1|-|32"},
      {"relational_expression_v1", "3",
       "4|-|3|019de5fc-2400-7539-bcce-00eef3ae7220|-|-|-|-"},
      {"relational_output_v1", "1", "1|1|1|1|0|67726f75705f6b6579"},
      {"relational_output_v1", "2", "1|2|2|1|1|6d656173757265"},
      {"relational_output_v1", "3", "4|3|3|1|0|726f775f6e756d626572"},
      {"relational_values_row_v1", "1", "1,2"},
      {"relational_window_definition_v1", "1",
       "4|-|-|1|2:1:2:-|-|-|-|1"},
      {"relational_window_invocation_v1", "1",
       "4|3|1|1|73622e77696e646f772e726f775f6e756d626572|"
       "019de5fc-2400-7539-bcce-00eef3ae7220|3|726f775f6e756d626572|-"},
      {"relational_node_v1", "1", "13|0|-|1,2|1"},
      {"relational_node_v1", "2", "5|0|1|1,2|-"},
      {"relational_node_v1", "3", "6|0|2|1,2|-"},
      {"relational_node_v1", "4", "8|0|3|3|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|1,2|-|-|"
       "019f0000-0000-7200-8000-000000003101,"
       "019f0000-0000-7200-8000-000000003106"},
      {"relational_node_binding_v1", "2",
       "6167677265676174652e67726f75702e7631|1,2|-|"
       "019f0000-0000-7200-8000-000000003101|"
       "019f0000-0000-7200-8000-000000003102"},
      {"relational_node_binding_v1", "3",
       "6f72646572696e672e6c6f676963616c2e7631|1,2|-|"
       "019f0000-0000-7200-8000-000000003103|"
       "019f0000-0000-7200-8000-000000003103"},
      {"relational_node_binding_v1", "4",
       "77696e646f772e6f7665722e7631|1,2,3|-|"
       "019f0000-0000-7200-8000-000000003103,"
       "019f0000-0000-7200-8000-000000003104|"
       "019f0000-0000-7200-8000-000000003103,"
       "019f0000-0000-7200-8000-000000003104,"
       "019f0000-0000-7200-8000-000000003105"},
      {"relational_property_v1", std::string(kOrderingOneUuid),
       "1|1|-|1:1:2:-|-|-"},
      {"relational_property_v1", std::string(kGroupingUuid),
       "2|2|1|-|-|-"},
      {"relational_property_v1", std::string(kOrderingTwoUuid),
       "1|3|-|2:1:2:-|-|-"},
      {"relational_property_v1", std::string(kPartitioningUuid),
       "3|4|1|-|-|-"},
      {"relational_property_v1", std::string(kWindowUuid),
       "4|4|-|-|019f0000-0000-7200-8000-000000003103,"
       "019f0000-0000-7200-8000-000000003104|"
       "019f0000-0000-7500-8000-000000003106"},
      {"relational_property_v1", std::string(kEquivalenceUuid),
       "5|1|1,2|-|-|-"},
  };
  return envelope;
}

bool ValidateLivePropertyPopulation() {
  const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), PropertyEnvelope(), {}});
  bool passed = true;
  passed &= Require(result.envelope_validated && result.accepted &&
                        result.dispatched_to_api,
                    "property-bearing SBLR did not reach the logical bridge");
  passed &= Require(result.logical_graph_populated &&
                        result.logical_properties_populated &&
                        result.optimizer_admitted &&
                        result.optimizer_admission_degraded &&
                        result.optimizer_admission_stage_count == 8 &&
                        result.logical_node_count == 4 &&
                        result.logical_property_count == 6,
                    "logical/property population evidence differs");
  passed &= Require(
      !result.api_result.ok &&
          HasApiDiagnostic(
              result, "QOW-DIAG-RELATIONAL-PHYSICAL-DISPATCH-PENDING"),
      "logical population selected or executed a physical plan");
  return passed;
}

bool ValidateEngineScopeAndPropertyRefusal() {
  const auto refuses_context_before_graph = [](api::EngineRequestContext context) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), PropertyEnvelope(), {}});
    return !result.envelope_validated && !result.accepted &&
           !result.logical_graph_populated &&
           !result.logical_properties_populated &&
           !result.optimizer_admitted && !result.optimizer_selected &&
           !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(result,
                            "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1");
  };

  auto stale_context = Context();
  stale_context.statement_metadata_snapshot_uuid.canonical =
      "019f0000-0000-7100-8000-000000003999";
  const auto stale = sblr::DispatchTextualRelationalQueryForContractTest(
      {std::move(stale_context), PropertyEnvelope(), {}});

  auto unknown_property = PropertyEnvelope();
  const auto aggregate_binding = std::ranges::find_if(
      unknown_property.operands, [](const auto& operand) {
        return operand.type == "relational_node_binding_v1" &&
               operand.name == "2";
      });
  aggregate_binding->value =
      "6167677265676174652e67726f75702e7631|1,2|-|"
      "019f0000-0000-7200-8000-000000003999|"
      "019f0000-0000-7200-8000-000000003102";
  const auto malformed = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), std::move(unknown_property), {}});
  auto missing_resource_context = Context();
  missing_resource_context.optimizer_memory_budget_bytes = 0;
  const auto missing_resource =
      sblr::DispatchTextualRelationalQueryForContractTest(
      {std::move(missing_resource_context), PropertyEnvelope(), {}});

  bool passed = true;
  passed &= Require(!stale.accepted && !stale.logical_graph_populated &&
                        HasApiDiagnostic(
                            stale, "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1"),
                    "stale engine metadata scope reached planning");

  auto context = Context();
  context.statement_uuid.canonical =
      "019f0000-0000-7120-8000-000000003999";
  passed &= Require(refuses_context_before_graph(std::move(context)),
                    "stale statement identity reached property planning");
  context = Context();
  context.transaction_uuid.canonical =
      "019f0000-0000-7130-8000-000000003999";
  passed &= Require(refuses_context_before_graph(std::move(context)),
                    "stale transaction identity reached property planning");
  context = Context();
  context.statement_snapshot_uuid.canonical =
      "019f0000-0000-7140-8000-000000003999";
  passed &= Require(refuses_context_before_graph(std::move(context)),
                    "stale data snapshot identity reached property planning");
  context = Context();
  context.catalog_epoch_uuid.canonical =
      "019f0000-0000-7100-8000-000000003999";
  passed &= Require(refuses_context_before_graph(std::move(context)),
                    "stale catalog identity reached property planning");
  context = Context();
  ++context.local_transaction_id;
  passed &= Require(refuses_context_before_graph(std::move(context)),
                    "local transaction mismatch reached property planning");
  context = Context();
  ++context.snapshot_visible_through_local_transaction_id;
  passed &= Require(refuses_context_before_graph(std::move(context)),
                    "visibility mismatch reached property planning");
  passed &= Require(
      !malformed.accepted && !malformed.logical_properties_populated &&
          HasApiDiagnostic(
              malformed, "QOW-DIAG-LOGICAL-PROPERTY-REFERENCE-V1"),
      "unknown property reference reached canonical logical population");
  passed &= Require(
      !missing_resource.accepted &&
          missing_resource.logical_graph_populated &&
          !missing_resource.optimizer_admitted &&
          HasApiDiagnostic(
              missing_resource,
              "QOW-DIAG-OPTIMIZER-ADMISSION-RESOURCE-V1"),
      "missing engine resource snapshot reached planning");
  return passed;
}

bool ValidateZeroHighwaterTypedCarriage() {
  api::TypedRelationalDag dag;
  dag.wire_version = 2;
  dag.bound_sblr_tree_uuid =
      "019f0000-0000-7000-8000-000000003401";
  dag.bound_catalog_epoch_uuid = std::string(kCatalogEpochUuid);
  dag.bound_security_context_uuid = std::string(kSecurityContextUuid);
  dag.statement_uuid = "019f0000-0000-7120-8000-000000003001";
  dag.owning_transaction_uuid =
      "019f0000-0000-7130-8000-000000003004";
  dag.statement_snapshot_uuid =
      "019f0000-0000-7140-8000-000000003005";
  dag.statement_metadata_snapshot_uuid =
      "019f0000-0000-7150-8000-000000003006";
  dag.local_transaction_id = std::numeric_limits<std::uint64_t>::max();
  dag.snapshot_visible_through_local_transaction_id = 0;
  dag.root_node_id = 1;
  dag.descriptors = {
      {1, "019f0000-0000-7200-8000-000000003401",
       "019f0000-0000-7300-8000-000000003402",
       api::RelationalNullability::kNonNull},
  };
  api::RelationalExpressionRecord expression;
  expression.expression_id = 1;
  expression.expression_kind = api::RelationalExpressionKind::kLiteral;
  expression.result_descriptor_id = 1;
  expression.literal_kind = api::RelationalLiteralKind::kNumeric;
  expression.literal_or_parameter_ref = "1";
  dag.expressions.push_back(std::move(expression));
  dag.outputs = {{1, 1, 1, "value", 1, false, 0}};
  dag.values_rows = {{1, {1}}};
  api::RelationalDagNode node;
  node.node_id = 1;
  node.node_kind = api::RelationalDagNodeKind::kValues;
  node.output_descriptor_ids = {1};
  node.values_row_ids = {1};
  node.bound_expression_ids = {1};
  node.semantic_variant_id = "values.literal-table.v1";
  dag.nodes.push_back(std::move(node));

  const auto validation = api::ValidateTypedRelationalDag(dag);
  return Require(
      validation.accepted &&
          dag.bound_catalog_epoch_uuid !=
              dag.statement_metadata_snapshot_uuid &&
          dag.local_transaction_id ==
              std::numeric_limits<std::uint64_t>::max() &&
          dag.snapshot_visible_through_local_transaction_id == 0,
      "typed DAG conflated identities, narrowed local identity, or refused zero visibility high-water");
}

}  // namespace

// QOW-TEST-302-PROPERTY-BRIDGE-V1
int main() {
  if (!ValidateLivePropertyPopulation() ||
      !ValidateEngineScopeAndPropertyRefusal() ||
      !ValidateZeroHighwaterTypedCarriage()) {
    return EXIT_FAILURE;
  }
  std::cout << "QOW-TEST-302-PROPERTY-BRIDGE-V1: PASS\n";
  return EXIT_SUCCESS;
}
