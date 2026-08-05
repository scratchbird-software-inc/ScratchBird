// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_dispatch.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::sblr {
SblrDispatchResult DispatchTextualRelationalQueryForContractTest(
    SblrDispatchRequest request);
void ArmCanonicalQueryPreResultRevocationForContractTest();
void ArmCanonicalQuerySecurityBoundaryDriftForContractTest();
void ArmCanonicalQueryResourceBoundaryDriftForContractTest();
std::size_t CanonicalQueryContractRevalidationCountForTest();
}

namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

namespace {

constexpr std::string_view kCatalogEpochUuid =
    "019f0000-0000-7100-8000-000000008102";
constexpr std::string_view kSecurityContextUuid =
    "019f0000-0000-7110-8000-000000008103";

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-INTEGRATION-306-211-LIVE-VALUES-V1: "
              << detail << '\n';
  }
  return condition;
}

bool HasApiDiagnostic(const sblr::SblrDispatchResult& result,
                      const std::string_view code) {
  for (const auto& diagnostic : result.api_result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasApiDiagnosticToken(const sblr::SblrDispatchResult& result,
                           const std::string_view token) {
  for (const auto& diagnostic : result.api_result.diagnostics) {
    if (diagnostic.code == token || diagnostic.detail.find(token) !=
                                        std::string::npos) {
      return true;
    }
  }
  return false;
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.statement_uuid.canonical =
      "019f0000-0000-7120-8000-000000008101";
  context.transaction_uuid.canonical =
      "019f0000-0000-7130-8000-000000008107";
  context.statement_snapshot_uuid.canonical =
      "019f0000-0000-7140-8000-000000008108";
  context.catalog_epoch_uuid.canonical = kCatalogEpochUuid;
  context.local_transaction_id = 8101;
  context.snapshot_visible_through_local_transaction_id = 8099;
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_metadata_snapshot_uuid.canonical =
      "019f0000-0000-7150-8000-000000008109";
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      kSecurityContextUuid;
  context.catalog_generation_id = 8101;
  context.security_epoch = 8102;
  context.resource_epoch = 8103;
  context.optimizer_capability_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000008104";
  context.optimizer_resource_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000008105";
  context.optimizer_route_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000008106";
  context.optimizer_route_epoch = 8104;
  context.optimizer_route_generation = 8105;
  context.optimizer_memory_budget_bytes = 1024 * 1024;
  context.optimizer_maximum_candidate_count = 1024;
  context.optimizer_maximum_memo_groups = 64;
  context.optimizer_maximum_search_steps = 4096;
  context.optimizer_maximum_planning_time_ns = 1'000'000;
  context.current_monotonic_ns = "8101000";
  context.authorization_context.security_epoch = 8102;
  context.authorization_context.policy_epoch = 8103;
  context.authorization_context.catalog_generation_id = 8101;
  return context;
}

sblr::SblrOperationEnvelope FinalizeStatementContextEnvelope(
    sblr::SblrOperationEnvelope envelope) {
  const auto already_bound = std::ranges::find_if(
      envelope.operands, [](const auto& operand) {
        return operand.type == "uuid" &&
               operand.name == "relational_statement_uuid";
      });
  if (already_bound != envelope.operands.end()) return envelope;
  const auto root = std::ranges::find_if(
      envelope.operands, [](const auto& operand) {
        return operand.type == "uint32" &&
               operand.name == "relational_root_node_id";
      });
  if (root == envelope.operands.end()) return envelope;
  envelope.operands.insert(
      root,
      {{"uuid", "relational_statement_uuid",
        "019f0000-0000-7120-8000-000000008101"},
       {"uuid", "relational_owning_transaction_uuid",
        "019f0000-0000-7130-8000-000000008107"},
       {"uuid", "relational_statement_snapshot_uuid",
        "019f0000-0000-7140-8000-000000008108"},
       {"uuid", "relational_statement_metadata_snapshot_uuid",
        "019f0000-0000-7150-8000-000000008109"},
       {"uint64", "relational_local_transaction_id", "8101"},
       {"uint64",
        "relational_snapshot_visible_through_local_transaction_id", "8099"}});
  return sblr::SblrOperationEnvelope(std::move(envelope));
}

bool HasExactStatementContextHeader(
    const sblr::SblrOperationEnvelope& envelope) {
  if (envelope.operands.size() < 10) return false;
  const std::array<std::array<std::string_view, 3>, 7> expected{{
      {"uuid", "relational_catalog_epoch_uuid", kCatalogEpochUuid},
      {"uuid", "relational_statement_uuid",
       "019f0000-0000-7120-8000-000000008101"},
      {"uuid", "relational_owning_transaction_uuid",
       "019f0000-0000-7130-8000-000000008107"},
      {"uuid", "relational_statement_snapshot_uuid",
       "019f0000-0000-7140-8000-000000008108"},
      {"uuid", "relational_statement_metadata_snapshot_uuid",
       "019f0000-0000-7150-8000-000000008109"},
      {"uint64", "relational_local_transaction_id", "8101"},
      {"uint64",
       "relational_snapshot_visible_through_local_transaction_id", "8099"},
  }};
  const std::array<std::size_t, 7> indexes{2, 4, 5, 6, 7, 8, 9};
  for (std::size_t ordinal = 0; ordinal < expected.size(); ++ordinal) {
    const auto& operand = envelope.operands[indexes[ordinal]];
    if (operand.type != expected[ordinal][0] ||
        operand.name != expected[ordinal][1] ||
        operand.value != expected[ordinal][2]) {
      return false;
    }
    if (std::ranges::count_if(envelope.operands, [&](const auto& candidate) {
          return candidate.name == expected[ordinal][1];
        }) != 1) {
      return false;
    }
  }
  return envelope.operands[3].name == "relational_security_context_uuid" &&
         envelope.operands[10].name == "relational_root_node_id";
}

bool RefusedStatementContextAtomically(
    const sblr::SblrDispatchResult& result) {
  return !result.envelope_validated && !result.accepted &&
         !result.logical_graph_populated &&
         !result.logical_properties_populated &&
         !result.optimizer_admitted && !result.optimizer_selected &&
         !result.physical_dag_published && !result.physical_dag_executed &&
         !result.runtime_actuals_attached &&
         !result.canonical_result_published && !result.api_result.ok &&
         result.physical_node_count == 0 &&
         result.canonical_result_column_count == 0 &&
         result.canonical_result_row_count == 0 &&
         result.selected_plan_uuid.empty() &&
         result.canonical_result_bytes.empty() &&
         HasApiDiagnostic(result, "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1");
}

sblr::SblrOperationEnvelope ValuesEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "qow.live.values.spine");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-000000008100"},
      {"uuid", "relational_catalog_epoch_uuid",
       std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "1"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-000000008201|"
       "019f0000-0000-7400-8000-000000008202|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-000000008203|"
       "019f0000-0000-7400-8000-000000008204|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|37"},
      {"relational_expression_v1", "2",
       "1|-|2|-|-|2|-|616c706861"},
      {"relational_expression_v1", "3", "1|-|1|-|-|1|-|3131"},
      {"relational_expression_v1", "4", "1|-|2|-|-|7|-|2d"},
      {"relational_output_v1", "1", "1|1|1|1|0|6964"},
      {"relational_output_v1", "2", "1|2|2|1|1|6c6162656c"},
      {"relational_values_row_v1", "1", "1,2"},
      {"relational_values_row_v1", "2", "3,4"},
      {"relational_node_v1", "1", "13|0|-|1,2|1,2"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|1,2,3,4|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope RuntimeBreadthValuesEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "qow.live.values.type-breadth");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-000000008250"},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "1"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-000000008251|"
       "019f0000-0000-7400-8000-000000008252|1|-|-|8|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-000000008253|"
       "019f0000-0000-7400-8000-000000008254|1|-|-|64|-|-"},
      {"relational_descriptor_v1", "3",
       "019f0000-0000-7300-8000-000000008255|"
       "019f0000-0000-7400-8000-000000008256|1|-|-|-|6|2"},
      {"relational_descriptor_v1", "4",
       "019f0000-0000-7300-8000-000000008257|"
       "019f0000-0000-7400-8000-000000008258|1|-|-|16|-|-"},
      {"relational_descriptor_v1", "5",
       "019f0000-0000-7300-8000-000000008259|"
       "019f0000-0000-7400-8000-000000008260|1|-|-|128|-|-"},
      {"relational_descriptor_v1", "6",
       "019f0000-0000-7300-8000-000000008261|"
       "019f0000-0000-7400-8000-000000008262|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "7",
       "019f0000-0000-7300-8000-000000008263|"
       "019f0000-0000-7400-8000-000000008264|1|-|"
       "74696d657374616d705f74696d657a6f6e655f70726f66696c65|-|6|-"},
      {"relational_descriptor_v1", "8",
       "019f0000-0000-7300-8000-000000008265|"
       "019f0000-0000-7400-8000-000000008266|2|"
       "019f0000-0000-7400-8000-000000008267|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|2d313238"},
      {"relational_expression_v1", "2", "1|-|2|-|-|1|-|312e3235"},
      {"relational_expression_v1", "3", "1|-|3|-|-|1|-|31322e3334"},
      {"relational_expression_v1", "4", "1|-|4|-|-|3|-|61"},
      {"relational_expression_v1", "5",
       "1|-|5|-|-|5|-|30313966303030302d303030302d373230302d383030302d"
       "303030303030303038323031"},
      {"relational_expression_v1", "6",
       "1|-|6|-|-|4|-|323032362d30382d3033"},
      {"relational_expression_v1", "7",
       "1|-|7|-|-|4|-|323032362d30382d30335430393a30303a30302d30343a3030"},
      {"relational_expression_v1", "8", "1|-|8|-|-|2|-|416c706861"},
      {"relational_expression_v1", "9", "1|-|1|-|-|1|-|2d313238"},
      {"relational_expression_v1", "10", "1|-|2|-|-|1|-|312e3235"},
      {"relational_expression_v1", "11", "1|-|3|-|-|1|-|31322e3334"},
      {"relational_expression_v1", "12", "1|-|4|-|-|3|-|61"},
      {"relational_expression_v1", "13",
       "1|-|5|-|-|5|-|30313966303030302d303030302d373230302d383030302d"
       "303030303030303038323031"},
      {"relational_expression_v1", "14",
       "1|-|6|-|-|4|-|323032362d30382d3033"},
      {"relational_expression_v1", "15",
       "1|-|7|-|-|4|-|323032362d30382d30335430393a30303a30302d30343a3030"},
      {"relational_expression_v1", "16", "1|-|8|-|-|7|-|2d"},
      {"relational_output_v1", "1", "1|1|1|1|0|74696e79"},
      {"relational_output_v1", "2", "1|2|2|1|1|726174696f"},
      {"relational_output_v1", "3", "1|3|3|1|2|616d6f756e74"},
      {"relational_output_v1", "4", "1|4|4|1|3|7061796c6f6164"},
      {"relational_output_v1", "5", "1|5|5|1|4|6964656e74697479"},
      {"relational_output_v1", "6", "1|6|6|1|5|6f6e5f64617465"},
      {"relational_output_v1", "7", "1|7|7|1|6|6f627365727665645f6174"},
      {"relational_output_v1", "8", "1|8|8|1|7|6c6162656c"},
      {"relational_values_row_v1", "1", "1,2,3,4,5,6,7,8"},
      {"relational_values_row_v1", "2", "9,10,11,12,13,14,15,16"},
      {"relational_node_v1", "1", "13|0|-|1,2,3,4,5,6,7,8|1,2"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope ComposedValuesEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "qow.live.values.composed");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-000000008300"},
      {"uuid", "relational_catalog_epoch_uuid",
       std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "1"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-000000008301|"
       "019f0000-0000-7400-8000-000000008302|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-000000008303|"
       "019f0000-0000-7400-8000-000000008304|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       "019f0000-0000-7300-8000-000000008305|"
       "019f0000-0000-7400-8000-000000008306|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "4",
       "019f0000-0000-7300-8000-000000008307|"
       "019f0000-0000-7400-8000-000000008308|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "5",
       "019f0000-0000-7300-8000-000000008309|"
       "019f0000-0000-7400-8000-000000008310|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "6",
       "019f0000-0000-7300-8000-000000008311|"
       "019f0000-0000-7400-8000-000000008312|1|"
       "019f0000-0000-7400-8000-000000008321|-|-|-|-"},
      {"relational_descriptor_v1", "7",
       "019f0000-0000-7300-8000-000000008313|"
       "019f0000-0000-7400-8000-000000008314|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "8",
       "019f0000-0000-7300-8000-000000008315|"
       "019f0000-0000-7400-8000-000000008316|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "9",
       "019f0000-0000-7300-8000-000000008317|"
       "019f0000-0000-7400-8000-000000008318|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "10",
       "019f0000-0000-7300-8000-000000008319|"
       "019f0000-0000-7400-8000-000000008320|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "2", "1|-|1|-|-|1|-|32"},
      {"relational_expression_v1", "3", "6|1,2|1|-|-|-|2b|-"},
      {"relational_expression_v1", "4", "1|-|2|-|-|1|-|33"},
      {"relational_expression_v1", "5", "5|4|2|-|-|-|2d|-"},
      {"relational_expression_v1", "6", "1|-|5|-|-|1|-|32"},
      {"relational_expression_v1", "7", "1|-|5|-|-|1|-|31"},
      {"relational_expression_v1", "8", "6|6,7|3|-|-|-|3e|-"},
      {"relational_expression_v1", "9", "1|-|4|-|-|6|-|54525545"},
      {"relational_expression_v1", "10", "1|-|4|-|-|7|-|2d"},
      {"relational_expression_v1", "11", "6|9,10|4|-|-|-|414e44|-"},
      {"relational_expression_v1", "12", "7|3|1|-|-|-|-|-"},
      {"relational_expression_v1", "13", "1|-|6|-|-|2|-|61"},
      {"relational_expression_v1", "14", "1|-|6|-|-|2|-|62"},
      {"relational_expression_v1", "15", "6|13,14|6|-|-|-|7c7c|-"},
      {"relational_expression_v1", "16", "1|-|7|-|-|6|-|46414c5345"},
      {"relational_expression_v1", "17", "1|-|7|-|-|6|-|54525545"},
      {"relational_expression_v1", "18", "6|16,17|7|-|-|-|4f52|-"},
      {"relational_expression_v1", "19", "1|-|5|-|-|1|-|31"},
      {"relational_expression_v1", "20", "1|-|10|-|-|7|-|2d"},
      {"relational_expression_v1", "21", "6|19,20|8|-|-|-|4953|-"},
      {"relational_expression_v1", "22", "1|-|9|-|-|6|-|46414c5345"},
      {"relational_expression_v1", "23", "5|22|9|-|-|-|4e4f54|-"},
      {"relational_output_v1", "1", "1|12|1|1|0|73756d"},
      {"relational_output_v1", "2", "1|5|2|1|1|6e656761746564"},
      {"relational_output_v1", "3", "1|8|3|1|2|67726561746572"},
      {"relational_output_v1", "4", "1|11|4|1|3|7472757468"},
      {"relational_output_v1", "5", "1|15|6|1|4|636f6e636174"},
      {"relational_output_v1", "6", "1|18|7|1|5|6469736a756e6374"},
      {"relational_output_v1", "7", "1|21|8|1|6|69735f6e756c6c"},
      {"relational_output_v1", "8", "1|23|9|1|7|6e65676174696f6e"},
      {"relational_values_row_v1", "1", "12,5,8,11,15,18,21,23"},
      {"relational_node_v1", "1", "13|0|-|1,2,3,4,6,7,8,9|1"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope UnionAllValuesEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "qow.live.values.union-all");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-000000008400"},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "3"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-000000008401|"
       "019f0000-0000-7400-8000-000000008402|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-000000008403|"
       "019f0000-0000-7400-8000-000000008402|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       "019f0000-0000-7300-8000-000000008405|"
       "019f0000-0000-7400-8000-000000008402|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "2", "1|-|1|-|-|1|-|32"},
      {"relational_expression_v1", "3", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "4", "1|-|2|-|-|1|-|32"},
      {"relational_expression_v1", "5", "1|-|2|-|-|1|-|33"},
      {"relational_expression_v1", "6", "1|-|2|-|-|7|-|2d"},
      {"relational_output_v1", "1", "1|1|1|1|0|6e"},
      {"relational_output_v1", "2", "2|4|2|1|0|6e"},
      {"relational_values_row_v1", "1", "1"},
      {"relational_values_row_v1", "2", "2"},
      {"relational_values_row_v1", "3", "3"},
      {"relational_values_row_v1", "4", "4"},
      {"relational_values_row_v1", "5", "5"},
      {"relational_values_row_v1", "6", "6"},
      {"relational_node_v1", "1", "13|0|-|1|1,2,3"},
      {"relational_node_v1", "2", "13|0|-|2|4,5,6"},
      {"relational_node_v1", "3", "9|0|1,2|3|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|1,2,3|-|-|-"},
      {"relational_node_binding_v1", "2",
       "76616c7565732e6c69746572616c2d7461626c652e7631|4,5,6|-|-|-"},
      {"relational_node_binding_v1", "3",
       "7365742d6f7065726174696f6e2e756e696f6e2d616c6c2e7631|-|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

// RCP-049-TEST-NODE-DRIVEN-UNION-ALL-COMPOSITION-V1
sblr::SblrOperationEnvelope NodeDrivenUnionAllLimitEnvelope() {
  auto envelope = UnionAllValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000c930";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "4";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "4",
       "019f0000-0000-7300-8000-00000000c931|"
       "019f0000-0000-7400-8000-000000008402|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "7", "1|-|4|-|-|1|-|34"});
  envelope.operands.push_back(
      {"relational_node_v1", "4", "7|0|3|3|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "4",
       "6c696d69742e626f756e642d636f756e742e7631|7|-|-|-"});
  return envelope;
}

// RCP-046-TEST-LIVE-SET-OPERATION-PROFILES-V1
sblr::SblrOperationEnvelope SetOperationValuesEnvelope(
    const std::string& semantic_variant_hex,
    const std::string& tree_uuid) {
  auto envelope = UnionAllValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = tree_uuid;
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "3") {
      operand.value = semantic_variant_hex + "|-|-|-|-";
    }
  }
  return envelope;
}

// RCP-049-TEST-NODE-DRIVEN-EXACT-SET-PROFILE-COMPOSITION-V1
sblr::SblrOperationEnvelope NodeDrivenExactSetProfileLimitEnvelope(
    const std::string& semantic_variant_hex,
    const std::string& tree_uuid) {
  auto envelope = SetOperationValuesEnvelope(semantic_variant_hex, tree_uuid);
  for (auto& operand : envelope.operands) {
    if (operand.type == "uint32" &&
        operand.name == "relational_root_node_id") {
      operand.value = "4";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "4",
       "019f0000-0000-7300-8000-00000000c941|"
       "019f0000-0000-7400-8000-000000008402|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "7", "1|-|4|-|-|1|-|3130"});
  envelope.operands.push_back(
      {"relational_node_v1", "4", "7|0|3|3|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "4",
       "6c696d69742e626f756e642d636f756e742e7631|7|-|-|-"});
  return envelope;
}

// RCP-049-TEST-NODE-DRIVEN-TYPE-RECONCILED-SET-COMPOSITION-V1
sblr::SblrOperationEnvelope NodeDrivenTypeReconciledSetLimitEnvelope() {
  auto envelope = UnionAllValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000c970";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "4";
    } else if (operand.type == "relational_descriptor_v1" &&
               operand.name == "1") {
      operand.value =
          "019f0000-0000-7300-8000-00000000c971|"
          "019f0000-0000-7400-8000-00000000c972|2|-|-|8|-|-";
    } else if (operand.type == "relational_descriptor_v1" &&
               operand.name == "2") {
      operand.value =
          "019f0000-0000-7300-8000-00000000c973|"
          "019f0000-0000-7400-8000-00000000c974|2|-|-|64|-|-";
    } else if (operand.type == "relational_descriptor_v1" &&
               operand.name == "3") {
      operand.value =
          "019f0000-0000-7300-8000-00000000c975|"
          "019f0000-0000-7400-8000-00000000c974|2|-|-|64|-|-";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "3") {
      operand.value =
          "7365742d6f7065726174696f6e2e756e696f6e2d616c6c2e"
          "747970652d7265636f6e63696c65642e7631|-|-|-|-";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "4",
       "019f0000-0000-7300-8000-00000000c976|"
       "019f0000-0000-7400-8000-00000000c974|1|-|-|64|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "7", "1|-|4|-|-|1|-|34"});
  envelope.operands.push_back(
      {"relational_node_v1", "4", "7|0|3|3|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "4",
       "6c696d69742e626f756e642d636f756e742e7631|7|-|-|-"});
  return envelope;
}

// RCP-046-TEST-LIVE-SET-OPERATION-BY-NAME-V1
sblr::SblrOperationEnvelope SetOperationByNameValuesEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      "qow.live.values.union-distinct-by-name");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-00000000b100"},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "3"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-00000000b101|"
       "019f0000-0000-7400-8000-00000000b111|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-00000000b102|"
       "019f0000-0000-7400-8000-00000000b111|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       "019f0000-0000-7300-8000-00000000b103|"
       "019f0000-0000-7400-8000-00000000b111|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "4",
       "019f0000-0000-7300-8000-00000000b104|"
       "019f0000-0000-7400-8000-00000000b111|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "5",
       "019f0000-0000-7300-8000-00000000b105|"
       "019f0000-0000-7400-8000-00000000b111|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "6",
       "019f0000-0000-7300-8000-00000000b106|"
       "019f0000-0000-7400-8000-00000000b111|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "2", "1|-|2|-|-|1|-|3130"},
      {"relational_expression_v1", "3", "1|-|1|-|-|1|-|32"},
      {"relational_expression_v1", "4", "1|-|2|-|-|1|-|3230"},
      {"relational_expression_v1", "5", "1|-|3|-|-|1|-|3230"},
      {"relational_expression_v1", "6", "1|-|4|-|-|1|-|32"},
      {"relational_expression_v1", "7", "1|-|3|-|-|1|-|3330"},
      {"relational_expression_v1", "8", "1|-|4|-|-|1|-|33"},
      {"relational_output_v1", "1", "1|1|1|1|0|6e"},
      {"relational_output_v1", "2", "1|2|2|1|1|6c6162656c"},
      {"relational_output_v1", "3", "2|5|3|1|0|6c6162656c"},
      {"relational_output_v1", "4", "2|6|4|1|1|6e"},
      {"relational_values_row_v1", "1", "1,2"},
      {"relational_values_row_v1", "2", "3,4"},
      {"relational_values_row_v1", "3", "5,6"},
      {"relational_values_row_v1", "4", "7,8"},
      {"relational_node_v1", "1", "13|0|-|1,2|1,2"},
      {"relational_node_v1", "2", "13|0|-|3,4|3,4"},
      {"relational_node_v1", "3", "9|0|1,2|5,6|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|1,2,3,4|-|-|-"},
      {"relational_node_binding_v1", "2",
       "76616c7565732e6c69746572616c2d7461626c652e7631|5,6,7,8|-|-|-"},
      {"relational_node_binding_v1", "3",
       "7365742d6f7065726174696f6e2e756e696f6e2d64697374696e63742e"
       "62792d6e616d652e7631|-|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

// RCP-049-TEST-NODE-DRIVEN-BY-NAME-SET-COMPOSITION-V1
sblr::SblrOperationEnvelope NodeDrivenByNameSetLimitEnvelope() {
  auto envelope = SetOperationByNameValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000c960";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "4";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "7",
       "019f0000-0000-7300-8000-00000000c961|"
       "019f0000-0000-7400-8000-00000000b111|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "9", "1|-|7|-|-|1|-|32"});
  envelope.operands.push_back(
      {"relational_node_v1", "4", "7|0|3|5,6|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "4",
       "6c696d69742e626f756e642d636f756e742e7631|9|-|-|-"});
  return envelope;
}

// RCP-046-TEST-LIVE-SET-OPERATION-NESTING-V1
sblr::SblrOperationEnvelope SetOperationNestedValuesEnvelope(
    const bool explicit_right_grouping) {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      explicit_right_grouping ? "qow.live.values.set-nested-right"
                              : "qow.live.values.set-nested-left");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       explicit_right_grouping
           ? "019f0000-0000-7000-8000-00000000b301"
           : "019f0000-0000-7000-8000-00000000b300"},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "5"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-00000000b311|"
       "019f0000-0000-7400-8000-00000000b321|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-00000000b312|"
       "019f0000-0000-7400-8000-00000000b321|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       "019f0000-0000-7300-8000-00000000b313|"
       "019f0000-0000-7400-8000-00000000b321|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "4",
       "019f0000-0000-7300-8000-00000000b314|"
       "019f0000-0000-7400-8000-00000000b321|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "5",
       "019f0000-0000-7300-8000-00000000b315|"
       "019f0000-0000-7400-8000-00000000b321|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "2", "1|-|1|-|-|1|-|32"},
      {"relational_expression_v1", "3", "1|-|2|-|-|1|-|32"},
      {"relational_expression_v1", "4", "1|-|2|-|-|1|-|33"},
      {"relational_expression_v1", "5", "1|-|3|-|-|1|-|32"},
      {"relational_output_v1", "1", "1|1|1|1|0|6e"},
      {"relational_output_v1", "2", "2|3|2|1|0|6e"},
      {"relational_output_v1", "3", "3|5|3|1|0|6e"},
      {"relational_values_row_v1", "1", "1"},
      {"relational_values_row_v1", "2", "2"},
      {"relational_values_row_v1", "3", "3"},
      {"relational_values_row_v1", "4", "4"},
      {"relational_values_row_v1", "5", "5"},
      {"relational_node_v1", "1", "13|0|-|1|1,2"},
      {"relational_node_v1", "2", "13|0|-|2|3,4"},
      {"relational_node_v1", "3", "13|0|-|3|5"},
      {"relational_node_v1", "4",
       explicit_right_grouping ? "9|0|2,3|4|-" : "9|0|1,2|4|-"},
      {"relational_node_v1", "5",
       explicit_right_grouping ? "9|0|1,4|5|-" : "9|0|4,3|5|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|1,2|-|-|-"},
      {"relational_node_binding_v1", "2",
       "76616c7565732e6c69746572616c2d7461626c652e7631|3,4|-|-|-"},
      {"relational_node_binding_v1", "3",
       "76616c7565732e6c69746572616c2d7461626c652e7631|5|-|-|-"},
      {"relational_node_binding_v1", "4",
       explicit_right_grouping
           ? "7365742d6f7065726174696f6e2e6578636570742d64697374696e63742e7631|-|-|-|-"
           : "7365742d6f7065726174696f6e2e756e696f6e2d64697374696e63742e7631|-|-|-|-"},
      {"relational_node_binding_v1", "5",
       explicit_right_grouping
           ? "7365742d6f7065726174696f6e2e756e696f6e2d64697374696e63742e7631|-|-|-|-"
           : "7365742d6f7065726174696f6e2e6578636570742d64697374696e63742e7631|-|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

// RCP-049-TEST-NODE-DRIVEN-NESTED-EXACT-SET-COMPOSITION-V1
sblr::SblrOperationEnvelope NodeDrivenNestedExactSetLimitEnvelope(
    const bool explicit_right_grouping) {
  auto envelope = SetOperationNestedValuesEnvelope(explicit_right_grouping);
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = explicit_right_grouping
          ? "019f0000-0000-7000-8000-00000000c951"
          : "019f0000-0000-7000-8000-00000000c950";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "6";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "6",
       "019f0000-0000-7300-8000-00000000c951|"
       "019f0000-0000-7400-8000-00000000b321|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "6", "1|-|6|-|-|1|-|32"});
  envelope.operands.push_back(
      {"relational_node_v1", "6", "7|0|5|5|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "6",
       "6c696d69742e626f756e642d636f756e742e7631|6|-|-|-"});
  return envelope;
}

sblr::SblrOperationEnvelope InnerJoinValuesEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "qow.live.values.inner-join");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-000000008500"},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "3"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-000000008501|"
       "019f0000-0000-7400-8000-000000008502|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-000000008503|"
       "019f0000-0000-7400-8000-000000008504|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       "019f0000-0000-7300-8000-000000008505|"
       "019f0000-0000-7400-8000-000000008506|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "2", "1|-|1|-|-|1|-|32"},
      {"relational_expression_v1", "3", "1|-|2|-|-|2|-|61"},
      {"relational_expression_v1", "4", "1|-|2|-|-|2|-|62"},
      {"relational_expression_v1", "5", "1|-|3|-|-|6|-|54525545"},
      {"relational_output_v1", "1", "1|1|1|1|0|6c6566745f6e"},
      {"relational_output_v1", "2", "2|3|2|1|0|72696768745f74"},
      {"relational_values_row_v1", "1", "1"},
      {"relational_values_row_v1", "2", "2"},
      {"relational_values_row_v1", "3", "3"},
      {"relational_values_row_v1", "4", "4"},
      {"relational_node_v1", "1", "13|0|-|1|1,2"},
      {"relational_node_v1", "2", "13|0|-|2|3,4"},
      {"relational_node_v1", "3", "4|0|1,2|1,2|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|1,2|-|-|-"},
      {"relational_node_binding_v1", "2",
       "76616c7565732e6c69746572616c2d7461626c652e7631|3,4|-|-|-"},
      {"relational_node_binding_v1", "3",
       "6a6f696e2e696e6e65722e7631|5|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

enum class JoinPredicateTruth : std::uint8_t {
  kTrue = 1,
  kFalse,
  kUnknown,
};

sblr::SblrOperationEnvelope AcceptedJoinValuesEnvelope(
    const std::string_view semantic_variant_hex,
    const std::string_view root_output_descriptors,
    const bool conditionless,
    const JoinPredicateTruth predicate_truth,
    const bool left_nullable,
    const bool right_nullable) {
  auto envelope = InnerJoinValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "relational_node_v1" && operand.name == "3") {
      operand.value = "4|0|1,2|" + std::string(root_output_descriptors) + "|-";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "3") {
      operand.value = std::string(semantic_variant_hex) +
                      (conditionless ? "|-|-|-|-" : "|5|-|-|-");
    } else if (operand.type == "relational_descriptor_v1" &&
               operand.name == "1" && left_nullable) {
      const auto marker = operand.value.find("|1|-|-|-|-|-");
      if (marker != std::string::npos) operand.value[marker + 1] = '2';
    } else if (operand.type == "relational_descriptor_v1" &&
               operand.name == "2" && right_nullable) {
      const auto marker = operand.value.find("|1|-|-|-|-|-");
      if (marker != std::string::npos) operand.value[marker + 1] = '2';
    } else if (!conditionless &&
               operand.type == "relational_expression_v1" &&
               operand.name == "5") {
      switch (predicate_truth) {
        case JoinPredicateTruth::kTrue:
          operand.value = "1|-|3|-|-|6|-|54525545";
          break;
        case JoinPredicateTruth::kFalse:
          operand.value = "1|-|3|-|-|6|-|46414c5345";
          break;
        case JoinPredicateTruth::kUnknown:
          operand.value = "1|-|3|-|-|7|-|2d";
          break;
      }
    }
  }
  return envelope;
}

// RCP-049-TEST-NODE-DRIVEN-ACCEPTED-JOIN-KINDS-COMPOSITION-V1
sblr::SblrOperationEnvelope NodeDrivenAcceptedJoinLimitEnvelope(
    const std::string_view semantic_variant_hex,
    const std::string_view join_output_descriptors,
    const bool conditionless,
    const JoinPredicateTruth predicate_truth,
    const bool left_nullable,
    const bool right_nullable,
    const std::string_view bound_tree_uuid) {
  auto envelope = AcceptedJoinValuesEnvelope(
      semantic_variant_hex, join_output_descriptors, conditionless,
      predicate_truth, left_nullable, right_nullable);
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = std::string(bound_tree_uuid);
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "4";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "4",
       "019f0000-0000-7300-8000-00000000c920|"
       "019f0000-0000-7400-8000-000000008502|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "6", "1|-|4|-|-|1|-|3130"});
  envelope.operands.push_back(
      {"relational_node_v1", "4",
       "7|0|3|" + std::string(join_output_descriptors) + "|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "4",
       "6c696d69742e626f756e642d636f756e742e7631|6|-|-|-"});
  return envelope;
}

// RCP-030-TEST-LIVE-ROW-DEPENDENT-JOIN-PREDICATE-V1
sblr::SblrOperationEnvelope RowDependentJoinValuesEnvelope() {
  auto envelope = InnerJoinValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "3") {
      operand.value = "1|-|2|-|-|1|-|32";
    } else if (operand.type == "relational_expression_v1" &&
               operand.name == "4") {
      operand.value = "1|-|2|-|-|1|-|33";
    } else if (operand.type == "relational_expression_v1" &&
               operand.name == "5") {
      operand.value = "6|6,7|3|-|-|-|3d|-";
    }
  }
  const auto first_output = std::ranges::find_if(
      envelope.operands, [](const auto& operand) {
        return operand.type == "relational_output_v1";
      });
  envelope.operands.insert(
      first_output,
      {{"relational_expression_v1", "6",
        "3|-|1|-|019f0000-0000-7500-8000-000000008511|-|-|-"},
       {"relational_expression_v1", "7",
        "3|-|2|-|019f0000-0000-7500-8000-000000008512|-|-|-"}});
  return envelope;
}

// RCP-041-TEST-LIVE-INNER-JOIN-FILTER-PROJECT-COMPOSITION-V1
sblr::SblrOperationEnvelope InnerJoinFilterProjectValuesEnvelope() {
  auto envelope = InnerJoinValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000a100";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "5";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "4",
       "019f0000-0000-7300-8000-00000000a101|"
       "019f0000-0000-7400-8000-000000008502|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "6",
       "3|-|1|-|019f0000-0000-7500-8000-00000000a102|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "7", "1|-|1|-|-|1|-|31"});
  envelope.operands.push_back(
      {"relational_expression_v1", "8", "6|6,7|3|-|-|-|3e|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "11", "6|6,7|1|-|-|-|2d|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "12", "1|-|1|-|-|1|-|3130"});
  envelope.operands.push_back(
      {"relational_expression_v1", "13", "6|12,11|4|-|-|-|2f|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "14",
       "3|-|2|-|019f0000-0000-7500-8000-00000000a103|-|-|-"});
  envelope.operands.push_back(
      {"relational_output_v1", "3",
       "5|13|4|1|0|736166655f71756f7469656e74"});
  envelope.operands.push_back(
      {"relational_output_v1", "4", "5|14|2|1|1|72696768745f74"});
  envelope.operands.push_back(
      {"relational_node_v1", "4", "2|0|3|1,2|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "4",
       "66696c7465722e77686572652e7631|8|-|-|-"});
  envelope.operands.push_back(
      {"relational_node_v1", "5", "3|0|4|4,2|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "5",
       "70726f6a6563742e73656c6563742d6c6973742e7631|13,14|-|-|-"});
  return envelope;
}

// RCP-042-TEST-LIVE-INNER-JOIN-FILTER-PROJECT-SORT-COMPOSITION-V1
sblr::SblrOperationEnvelope InnerJoinFilterProjectSortValuesEnvelope() {
  auto envelope = InnerJoinFilterProjectValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000a200";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "6";
    } else if (operand.type == "relational_expression_v1" &&
               operand.name == "1") {
      operand.value = "1|-|1|-|-|1|-|30";
    } else if (operand.type == "relational_expression_v1" &&
               operand.name == "2") {
      operand.value = "1|-|1|-|-|1|-|31";
    } else if (operand.type == "relational_expression_v1" &&
               operand.name == "3") {
      operand.value = "1|-|2|-|-|2|-|62";
    } else if (operand.type == "relational_expression_v1" &&
               operand.name == "4") {
      operand.value = "1|-|2|-|-|2|-|61";
    } else if (operand.type == "relational_expression_v1" &&
               operand.name == "7") {
      operand.value = "1|-|1|-|-|1|-|30";
    } else if (operand.type == "relational_node_v1" &&
               operand.name == "1") {
      operand.value = "13|0|-|1|1,2,5";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "1") {
      operand.value =
          "76616c7565732e6c69746572616c2d7461626c652e7631|1,2,15|-|-|-";
    }
  }
  envelope.operands.push_back(
      {"relational_expression_v1", "15", "1|-|1|-|-|1|-|32"});
  envelope.operands.push_back(
      {"relational_values_row_v1", "5", "15"});
  envelope.operands.push_back(
      {"relational_node_v1", "6", "6|0|5|4,2|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "6",
       "736f72742e72657175697265642d6f726465722e7631|13|-|"
       "019f0000-0000-7200-8000-00000000a201|"
       "019f0000-0000-7200-8000-00000000a201"});
  envelope.operands.push_back(
      {"relational_property_v1",
       "019f0000-0000-7200-8000-00000000a201",
       "1|6|-|13:1:2:-|-|-"});
  return envelope;
}

// RCP-043-TEST-LIVE-INNER-JOIN-FILTER-PROJECT-SORT-LIMIT-COMPOSITION-V1
sblr::SblrOperationEnvelope InnerJoinFilterProjectSortLimitValuesEnvelope() {
  auto envelope = InnerJoinFilterProjectSortValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000a300";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "7";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "5",
       "019f0000-0000-7300-8000-00000000a301|"
       "019f0000-0000-7400-8000-00000000a302|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "16", "1|-|5|-|-|1|-|31"});
  envelope.operands.push_back(
      {"relational_node_v1", "7", "7|0|6|4,2|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "7",
       "6c696d69742e626f756e642d636f756e742e7631|16|-|-|-"});
  return envelope;
}

// RCP-044-TEST-LIVE-INNER-JOIN-FILTER-PROJECT-DISTINCT-SORT-LIMIT-V1
sblr::SblrOperationEnvelope
InnerJoinFilterProjectDistinctSortLimitValuesEnvelope() {
  auto envelope = InnerJoinFilterProjectSortLimitValuesEnvelope();
  std::erase_if(envelope.operands, [](const auto& operand) {
    return operand.type == "relational_output_v1" && operand.name == "4";
  });
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000a400";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "8";
    } else if (operand.type == "relational_expression_v1" &&
               operand.name == "16") {
      operand.value = "1|-|5|-|-|1|-|32";
    } else if (operand.type == "relational_node_v1" &&
               operand.name == "5") {
      operand.value = "3|0|4|4|-";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "5") {
      operand.value =
          "70726f6a6563742e73656c6563742d6c6973742e7631|13|-|-|-";
    } else if (operand.type == "relational_node_v1" &&
               operand.name == "6") {
      operand.name = "7";
      operand.value = "6|0|6|4|-";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "6") {
      operand.name = "7";
    } else if (operand.type == "relational_node_v1" &&
               operand.name == "7") {
      operand.name = "8";
      operand.value = "7|0|7|4|-";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "7") {
      operand.name = "8";
    } else if (operand.type == "relational_property_v1") {
      operand.value = "1|7|-|13:1:2:-|-|-";
    }
  }
  envelope.operands.push_back(
      {"relational_node_v1", "6", "5|0|5|4|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "6",
       "6167677265676174652e71756572792d64697374696e63742e7631|13|-|-|-"});
  return envelope;
}

// RCP-045-TEST-LIVE-INNER-JOIN-FILTER-PROJECT-DISTINCT-SORT-OFFSET-FETCH-V1
sblr::SblrOperationEnvelope
InnerJoinFilterProjectDistinctSortOffsetValuesEnvelope(
    const bool fetch_first_rows_only) {
  auto envelope = InnerJoinFilterProjectDistinctSortLimitValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value =
          fetch_first_rows_only
              ? "019f0000-0000-7000-8000-00000000a600"
              : "019f0000-0000-7000-8000-00000000a500";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "8") {
      operand.value =
          fetch_first_rows_only
              ? "66657463682e66697273742d726f77732d6f6e6c792d6f66667365742e7631|"
                "16,17|-|-|-"
              : "6c696d69742e626f756e642d636f756e742d6f66667365742e7631|"
                "16,17|-|-|-";
    }
  }
  envelope.operands.push_back(
      {"relational_expression_v1", "17", "1|-|5|-|-|1|-|31"});
  return envelope;
}

sblr::SblrOperationEnvelope FilterValuesEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "qow.live.values.filter");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-000000008600"},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-000000008601|"
       "019f0000-0000-7400-8000-000000008602|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-000000008603|"
       "019f0000-0000-7400-8000-000000008604|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "2", "1|-|1|-|-|1|-|32"},
      {"relational_expression_v1", "3", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "4", "1|-|2|-|-|6|-|54525545"},
      {"relational_output_v1", "1", "1|1|1|1|0|6e"},
      {"relational_values_row_v1", "1", "1"},
      {"relational_values_row_v1", "2", "2"},
      {"relational_values_row_v1", "3", "3"},
      {"relational_node_v1", "1", "13|0|-|1|1,2,3"},
      {"relational_node_v1", "2", "2|0|1|1|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|1,2,3|-|-|-"},
      {"relational_node_binding_v1", "2",
       "66696c7465722e77686572652e7631|4|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

// RCP-031-TEST-LIVE-ROW-DEPENDENT-FILTER-PREDICATE-V1
sblr::SblrOperationEnvelope RowDependentFilterValuesEnvelope() {
  auto envelope = FilterValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "4") {
      operand.value = "6|5,6|2|-|-|-|3e|-";
    }
  }
  const auto first_output = std::ranges::find_if(
      envelope.operands, [](const auto& operand) {
        return operand.type == "relational_output_v1";
      });
  envelope.operands.insert(
      first_output,
      {{"relational_expression_v1", "5",
        "3|-|1|-|019f0000-0000-7500-8000-000000008611|-|-|-"},
       {"relational_expression_v1", "6", "1|-|1|-|-|1|-|31"}});
  return envelope;
}

sblr::SblrOperationEnvelope ProjectValuesEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "qow.live.values.project");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-000000008700"},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-000000008701|"
       "019f0000-0000-7400-8000-000000008702|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-000000008703|"
       "019f0000-0000-7400-8000-000000008704|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "2", "1|-|2|-|-|2|-|616c706861"},
      {"relational_expression_v1", "3", "1|-|1|-|-|1|-|32"},
      {"relational_expression_v1", "4", "1|-|2|-|-|7|-|2d"},
      {"relational_output_v1", "1", "1|1|1|1|0|6964"},
      {"relational_output_v1", "2", "1|2|2|1|1|6c6162656c"},
      {"relational_values_row_v1", "1", "1,2"},
      {"relational_values_row_v1", "2", "3,4"},
      {"relational_node_v1", "1", "13|0|-|1,2|1,2"},
      {"relational_node_v1", "2", "3|0|1|2,1|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|1,2,3,4|-|-|-"},
      {"relational_node_binding_v1", "2",
       "70726f6a6563742e73656c6563742d6c6973742e7631|-|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

// RCP-032-TEST-LIVE-ROW-DEPENDENT-PROJECT-EXPRESSION-V1
sblr::SblrOperationEnvelope RowDependentProjectValuesEnvelope() {
  auto envelope = ProjectValuesEnvelope();
  const auto first_output = std::ranges::find_if(
      envelope.operands, [](const auto& operand) {
        return operand.type == "relational_output_v1";
      });
  envelope.operands.insert(
      first_output,
      {{"relational_descriptor_v1", "3",
        "019f0000-0000-7300-8000-000000008705|"
        "019f0000-0000-7400-8000-000000008706|1|-|-|-|-|-"},
       {"relational_expression_v1", "5", "6|6,7|3|-|-|-|2b|-"},
       {"relational_expression_v1", "6",
        "3|-|1|-|019f0000-0000-7500-8000-000000008707|-|-|-"},
       {"relational_expression_v1", "7", "1|-|3|-|-|1|-|3130"},
       {"relational_expression_v1", "8",
        "3|-|2|-|019f0000-0000-7500-8000-000000008708|-|-|-"},
       {"relational_expression_v1", "9", "1|-|1|-|-|1|-|33"},
       {"relational_expression_v1", "10", "1|-|2|-|-|2|-|62657461"}});
  const auto first_node = std::ranges::find_if(
      envelope.operands, [](const auto& operand) {
        return operand.type == "relational_node_v1";
      });
  envelope.operands.insert(
      first_node,
      {{"relational_output_v1", "3",
        "2|5|3|1|0|69645f706c75735f74656e"},
       {"relational_output_v1", "4", "2|8|2|1|1|6c6162656c"},
       {"relational_values_row_v1", "3", "9,10"}});
  for (auto& operand : envelope.operands) {
    if (operand.type == "relational_node_v1" && operand.name == "1") {
      operand.value = "13|0|-|1,2|1,2,3";
    } else if (operand.type == "relational_node_v1" && operand.name == "2") {
      operand.value = "3|0|1|3,2|-";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "1") {
      operand.value =
          "76616c7565732e6c69746572616c2d7461626c652e7631|"
          "1,2,3,4,9,10|-|-|-";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "2") {
      operand.value =
          "70726f6a6563742e73656c6563742d6c6973742e7631|5,8|-|-|-";
    }
  }
  return envelope;
}

// RCP-034-TEST-LIVE-PROJECT-SORT-COMPOSITION-V1
sblr::SblrOperationEnvelope ProjectedExpressionSortValuesEnvelope() {
  auto envelope = RowDependentProjectValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uint32" &&
        operand.name == "relational_root_node_id") {
      operand.value = "3";
    }
  }
  envelope.operands.push_back(
      {"relational_node_v1", "3", "6|0|2|3,2|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "3",
       "736f72742e72657175697265642d6f726465722e7631|5|-|"
       "019f0000-0000-7200-8000-000000008712|"
       "019f0000-0000-7200-8000-000000008712"});
  envelope.operands.push_back(
      {"relational_property_v1",
       "019f0000-0000-7200-8000-000000008712",
       "1|3|-|5:2:2:-|-|-"});
  return envelope;
}

// RCP-035-TEST-LIVE-FILTER-PROJECT-COMPOSITION-V1
sblr::SblrOperationEnvelope FilteredExpressionProjectValuesEnvelope() {
  auto envelope = RowDependentProjectValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-000000008a00";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "3";
    } else if (operand.type == "relational_output_v1" &&
               operand.value.starts_with("2|")) {
      operand.value[0] = '3';
      if (operand.name == "3") {
        operand.value = "3|5|3|1|0|736166655f71756f7469656e74";
      }
    } else if (operand.type == "relational_expression_v1" &&
               operand.name == "5") {
      operand.value = "6|7,14|3|-|-|-|2f|-";
    } else if (operand.type == "relational_node_v1" &&
               operand.name == "2") {
      operand.name = "3";
      operand.value = "3|0|2|3,2|-";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "2") {
      operand.name = "3";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "4",
       "019f0000-0000-7300-8000-000000008a01|"
       "019f0000-0000-7400-8000-000000008a02|2|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "11", "6|12,13|4|-|-|-|3e|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "12",
       "3|-|1|-|019f0000-0000-7500-8000-000000008a03|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "13", "1|-|1|-|-|1|-|31"});
  envelope.operands.push_back(
      {"relational_expression_v1", "14", "6|6,13|1|-|-|-|2d|-"});
  envelope.operands.push_back(
      {"relational_node_v1", "2", "2|0|1|1,2|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "2",
       "66696c7465722e77686572652e7631|11|-|-|-"});
  return envelope;
}

// RCP-036-TEST-LIVE-FILTER-PROJECT-SORT-COMPOSITION-V1
sblr::SblrOperationEnvelope FilteredProjectedSortValuesEnvelope() {
  auto envelope = FilteredExpressionProjectValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-000000008b00";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "4";
    }
  }
  envelope.operands.push_back(
      {"relational_node_v1", "4", "6|0|3|3,2|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "4",
       "736f72742e72657175697265642d6f726465722e7631|5|-|"
       "019f0000-0000-7200-8000-000000008b01|"
       "019f0000-0000-7200-8000-000000008b01"});
  envelope.operands.push_back(
      {"relational_property_v1",
       "019f0000-0000-7200-8000-000000008b01",
       "1|4|-|5:1:2:-|-|-"});
  return envelope;
}

// RCP-037-TEST-LIVE-FILTER-PROJECT-SORT-LIMIT-COMPOSITION-V1
sblr::SblrOperationEnvelope FilteredProjectedSortLimitValuesEnvelope() {
  auto envelope = FilteredProjectedSortValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-000000008c00";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "5";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "5",
       "019f0000-0000-7300-8000-000000008c01|"
       "019f0000-0000-7400-8000-000000008c02|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "15", "1|-|5|-|-|1|-|31"});
  envelope.operands.push_back(
      {"relational_node_v1", "5", "7|0|4|3,2|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "5",
       "6c696d69742e626f756e642d636f756e742e7631|15|-|-|-"});
  return envelope;
}

// RCP-038-TEST-LIVE-FILTER-PROJECT-DISTINCT-SORT-LIMIT-COMPOSITION-V1
sblr::SblrOperationEnvelope
FilteredProjectedDistinctSortLimitValuesEnvelope() {
  auto envelope = FilteredProjectedSortLimitValuesEnvelope();
  std::erase_if(envelope.operands, [](const auto& operand) {
    return (operand.type == "relational_output_v1" && operand.name == "4") ||
           (operand.type == "relational_expression_v1" &&
            operand.name == "8");
  });
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-000000008d00";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "6";
    } else if (operand.type == "relational_node_v1" &&
               operand.name == "1") {
      operand.value = "13|0|-|1,2|1,2,3,4";
    } else if (operand.type == "relational_node_v1" &&
               operand.name == "3") {
      operand.value = "3|0|2|3|-";
    } else if (operand.type == "relational_node_v1" &&
               operand.name == "4") {
      operand.name = "5";
      operand.value = "6|0|4|3|-";
    } else if (operand.type == "relational_node_v1" &&
               operand.name == "5") {
      operand.name = "6";
      operand.value = "7|0|5|3|-";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "1") {
      operand.value =
          "76616c7565732e6c69746572616c2d7461626c652e7631|"
          "1,2,3,4,9,10,16,17|-|-|-";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "3") {
      operand.value =
          "70726f6a6563742e73656c6563742d6c6973742e7631|5|-|-|-";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "4") {
      operand.name = "5";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "5") {
      operand.name = "6";
    } else if (operand.type == "relational_expression_v1" &&
               operand.name == "15") {
      operand.value = "1|-|5|-|-|1|-|32";
    } else if (operand.type == "relational_property_v1") {
      operand.value = "1|5|-|5:1:2:-|-|-";
    }
  }
  envelope.operands.push_back(
      {"relational_expression_v1", "16", "1|-|1|-|-|1|-|33"});
  envelope.operands.push_back(
      {"relational_expression_v1", "17", "1|-|2|-|-|2|-|62657461"});
  envelope.operands.push_back(
      {"relational_values_row_v1", "4", "16,17"});
  envelope.operands.push_back(
      {"relational_node_v1", "4", "5|0|3|3|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "4",
       "6167677265676174652e71756572792d64697374696e63742e7631|5|-|-|-"});
  return envelope;
}

// RCP-039-TEST-LIVE-FILTER-PROJECT-DISTINCT-SORT-OFFSET-FETCH-COMPOSITION-V1
sblr::SblrOperationEnvelope
FilteredProjectedDistinctSortOffsetValuesEnvelope(
    const bool fetch_first_rows_only) {
  auto envelope = FilteredProjectedDistinctSortLimitValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value =
          fetch_first_rows_only
              ? "019f0000-0000-7000-8000-000000008f00"
              : "019f0000-0000-7000-8000-000000008e00";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "6") {
      operand.value =
          (fetch_first_rows_only
               ? "66657463682e66697273742d726f77732d6f6e6c792d6f66667365742e7631"
               : "6c696d69742e626f756e642d636f756e742d6f66667365742e7631") +
          std::string("|15,18|-|-|-");
    }
  }
  envelope.operands.push_back(
      {"relational_expression_v1", "18", "1|-|5|-|-|1|-|31"});
  return envelope;
}

// RCP-049-TEST-NODE-DRIVEN-UNARY-COMPOSITION-V1
// Exercise a legal order that no enumerated whole-query route recognizes:
// VALUES -> LIMIT -> FILTER -> PROJECT -> DISTINCT -> SORT.  The logical node
// IDs deliberately remain non-topological in the carrier; optimizer
// publication and physical dispatch must follow the typed dependency edges.
sblr::SblrOperationEnvelope NodeDrivenUnaryCompositionEnvelope() {
  auto envelope = FilteredProjectedDistinctSortLimitValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000c900";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "5";
    } else if (operand.type == "relational_node_v1" &&
               operand.name == "2") {
      operand.value = "2|0|6|1,2|-";
    } else if (operand.type == "relational_node_v1" &&
               operand.name == "6") {
      operand.value = "7|0|1|1,2|-";
    }
  }
  return envelope;
}

// RCP-049-TEST-NODE-DRIVEN-BRANCHING-JOIN-COMPOSITION-V1
// Exercise a branching order that no enumerated whole-query route recognizes:
// two VALUES leaves -> INNER JOIN -> LIMIT -> FILTER -> PROJECT -> SORT.
// The LIMIT node retains its higher carrier ID while executing immediately
// above the JOIN, proving dependency-edge scheduling rather than ID ordering.
sblr::SblrOperationEnvelope NodeDrivenJoinCompositionEnvelope() {
  auto envelope = InnerJoinFilterProjectSortLimitValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000c910";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "6";
    } else if (operand.type == "relational_expression_v1" &&
               operand.name == "16") {
      operand.value = "1|-|5|-|-|1|-|33";
    } else if (operand.type == "relational_node_v1" &&
               operand.name == "4") {
      operand.value = "2|0|7|1,2|-";
    } else if (operand.type == "relational_node_v1" &&
               operand.name == "7") {
      operand.value = "7|0|3|1,2|-";
    }
  }
  return envelope;
}

// RCP-040-TEST-LIVE-EMPTY-FILTERED-EXPRESSION-PROJECTION-V1
sblr::SblrOperationEnvelope RejectAllFilteredRows(
    sblr::SblrOperationEnvelope envelope) {
  for (auto& operand : envelope.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "13") {
      operand.value = "1|-|1|-|-|1|-|39";
    }
  }
  return envelope;
}

sblr::SblrOperationEnvelope LimitValuesEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "qow.live.values.limit");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-000000008800"},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-000000008801|"
       "019f0000-0000-7400-8000-000000008802|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-000000008803|"
       "019f0000-0000-7400-8000-000000008804|1|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "2", "1|-|1|-|-|1|-|32"},
      {"relational_expression_v1", "3", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "4", "1|-|1|-|-|1|-|34"},
      {"relational_expression_v1", "5", "1|-|2|-|-|1|-|32"},
      {"relational_output_v1", "1", "1|1|1|1|0|6e"},
      {"relational_values_row_v1", "1", "1"},
      {"relational_values_row_v1", "2", "2"},
      {"relational_values_row_v1", "3", "3"},
      {"relational_values_row_v1", "4", "4"},
      {"relational_node_v1", "1", "13|0|-|1|1,2,3,4"},
      {"relational_node_v1", "2", "7|0|1|1|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|1,2,3,4|-|-|-"},
      {"relational_node_binding_v1", "2",
       "6c696d69742e626f756e642d636f756e742e7631|5|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope SortValuesEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "qow.live.values.sort");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-000000008900"},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-000000008901|"
       "019f0000-0000-7400-8000-000000008902|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-000000008903|"
       "019f0000-0000-7400-8000-000000008904|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       "019f0000-0000-7300-8000-000000008905|"
       "019f0000-0000-7400-8000-000000008906|1|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|32"},
      {"relational_expression_v1", "2", "1|-|2|-|-|1|-|32"},
      {"relational_expression_v1", "3", "1|-|3|-|-|1|-|31"},
      {"relational_expression_v1", "4", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "5", "1|-|2|-|-|1|-|31"},
      {"relational_expression_v1", "6", "1|-|3|-|-|1|-|32"},
      {"relational_expression_v1", "7", "1|-|1|-|-|1|-|32"},
      {"relational_expression_v1", "8", "1|-|2|-|-|1|-|31"},
      {"relational_expression_v1", "9", "1|-|3|-|-|1|-|33"},
      {"relational_expression_v1", "10", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "11", "1|-|2|-|-|1|-|39"},
      {"relational_expression_v1", "12", "1|-|3|-|-|1|-|34"},
      {"relational_expression_v1", "13", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "14", "1|-|2|-|-|1|-|30"},
      {"relational_expression_v1", "15", "1|-|3|-|-|1|-|35"},
      {"relational_expression_v1", "16", "1|-|1|-|-|1|-|32"},
      {"relational_expression_v1", "17", "1|-|2|-|-|1|-|32"},
      {"relational_expression_v1", "18", "1|-|3|-|-|1|-|36"},
      {"relational_output_v1", "1", "1|1|1|1|0|6b6579"},
      {"relational_output_v1", "2", "1|2|2|1|1|746965"},
      {"relational_output_v1", "3", "1|3|3|1|2|6964"},
      {"relational_values_row_v1", "1", "1,2,3"},
      {"relational_values_row_v1", "2", "4,5,6"},
      {"relational_values_row_v1", "3", "7,8,9"},
      {"relational_values_row_v1", "4", "10,11,12"},
      {"relational_values_row_v1", "5", "13,14,15"},
      {"relational_values_row_v1", "6", "16,17,18"},
      {"relational_node_v1", "1", "13|0|-|1,2,3|1,2,3,4,5,6"},
      {"relational_node_v1", "2", "6|0|1|1,2,3|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18|-|-|-"},
      {"relational_node_binding_v1", "2",
       "736f72742e72657175697265642d6f726465722e7631|1,2|-|"
       "019f0000-0000-7200-8000-000000008907|"
       "019f0000-0000-7200-8000-000000008907"},
      {"relational_property_v1",
       "019f0000-0000-7200-8000-000000008907",
       "1|2|-|1:2:1:-,2:1:2:-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

// RCP-033-TEST-LIVE-ROW-DEPENDENT-SORT-EXPRESSION-V1
sblr::SblrOperationEnvelope RowDependentSortValuesEnvelope() {
  auto envelope = SortValuesEnvelope();
  const auto first_output = std::ranges::find_if(
      envelope.operands, [](const auto& operand) {
        return operand.type == "relational_output_v1";
      });
  envelope.operands.insert(
      first_output,
      {{"relational_descriptor_v1", "4",
        "019f0000-0000-7300-8000-000000008908|"
        "019f0000-0000-7400-8000-000000008909|2|-|-|-|-|-"},
       {"relational_expression_v1", "19", "6|20,21|4|-|-|-|2b|-"},
       {"relational_expression_v1", "20",
        "3|-|1|-|019f0000-0000-7500-8000-000000008910|-|-|-"},
       {"relational_expression_v1", "21",
        "3|-|3|-|019f0000-0000-7500-8000-000000008911|-|-|-"}});
  for (auto& operand : envelope.operands) {
    if (operand.type == "relational_node_binding_v1" &&
        operand.name == "2") {
      operand.value =
          "736f72742e72657175697265642d6f726465722e7631|19|-|"
          "019f0000-0000-7200-8000-000000008907|"
          "019f0000-0000-7200-8000-000000008907";
    } else if (operand.type == "relational_property_v1") {
      operand.value = "1|2|-|19:2:1:-|-|-";
    }
  }
  return envelope;
}

sblr::SblrOperationEnvelope DistinctSortLimitValuesEnvelope(
    const bool fetch_first_rows_only) {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      fetch_first_rows_only ? "qow.live.values.distinct-sort-fetch"
                            : "qow.live.values.distinct-sort-limit");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-000000009000"},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "4"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-000000009001|"
       "019f0000-0000-7400-8000-000000009002|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-000000009003|"
       "019f0000-0000-7400-8000-000000009004|1|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|32"},
      {"relational_expression_v1", "2", "1|-|2|-|-|1|-|3230"},
      {"relational_expression_v1", "3", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "4", "1|-|2|-|-|1|-|3130"},
      {"relational_expression_v1", "5", "1|-|1|-|-|1|-|32"},
      {"relational_expression_v1", "6", "1|-|2|-|-|1|-|3230"},
      {"relational_expression_v1", "7", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "8", "1|-|2|-|-|1|-|3131"},
      {"relational_expression_v1", "9", "1|-|1|-|-|1|-|33"},
      {"relational_expression_v1", "10", "1|-|2|-|-|1|-|3330"},
      {"relational_expression_v1", "11", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "12", "1|-|2|-|-|1|-|3130"},
      {"relational_expression_v1", "13", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "14", "1|-|2|-|-|1|-|3939"},
      {"relational_expression_v1", "15", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "16", "1|-|2|-|-|1|-|3939"},
      {"relational_expression_v1", "17", "1|-|2|-|-|1|-|32"},
      {"relational_expression_v1", "18", "1|-|2|-|-|1|-|31"},
      {"relational_output_v1", "1", "1|1|1|1|0|6b6579"},
      {"relational_output_v1", "2", "1|2|2|1|1|746965"},
      {"relational_values_row_v1", "1", "1,2"},
      {"relational_values_row_v1", "2", "3,4"},
      {"relational_values_row_v1", "3", "5,6"},
      {"relational_values_row_v1", "4", "7,8"},
      {"relational_values_row_v1", "5", "9,10"},
      {"relational_values_row_v1", "6", "11,12"},
      {"relational_values_row_v1", "7", "13,14"},
      {"relational_values_row_v1", "8", "15,16"},
      {"relational_node_v1", "1", "13|0|-|1,2|1,2,3,4,5,6,7,8"},
      {"relational_node_v1", "2", "5|0|1|1,2|-"},
      {"relational_node_v1", "3", "6|0|2|1,2|-"},
      {"relational_node_v1", "4", "7|0|3|1,2|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16|-|-|-"},
      {"relational_node_binding_v1", "2",
       "6167677265676174652e71756572792d64697374696e63742e7631|"
       "1,2|-|-|-"},
      {"relational_node_binding_v1", "3",
       "736f72742e72657175697265642d6f726465722e7631|1,2|-|"
       "019f0000-0000-7200-8000-000000009007|"
       "019f0000-0000-7200-8000-000000009007"},
      {"relational_node_binding_v1", "4",
       (fetch_first_rows_only
            ? "66657463682e66697273742d726f77732d6f6e6c792d6f66667365742e7631"
            : "6c696d69742e626f756e642d636f756e742d6f66667365742e7631") +
           std::string("|17,18|-|-|-")},
      {"relational_property_v1",
       "019f0000-0000-7200-8000-000000009007",
       "1|3|-|1:1:2:-,2:2:2:-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope GlobalCountStarValuesEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "qow.live.values.count-star");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-000000009000"},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-000000009001|"
       "019f0000-0000-7400-8000-000000009002|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-000000009003|"
       "019f0000-0000-7400-8000-000000009004|1|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "2", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "3", "1|-|1|-|-|1|-|39"},
      {"relational_expression_v1", "4",
       "4|-|2|019de5fc-2400-784a-9aec-371f8b95b7ea|-|-|-|-"},
      {"relational_output_v1", "1", "1|1|1|1|0|76616c7565"},
      {"relational_output_v1", "2", "2|4|2|1|0|726f775f636f756e74"},
      {"relational_values_row_v1", "1", "1"},
      {"relational_values_row_v1", "2", "2"},
      {"relational_values_row_v1", "3", "3"},
      {"relational_node_v1", "1", "13|0|-|1|1,2,3"},
      {"relational_node_v1", "2", "5|0|1|2|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|1,2,3|-|-|-"},
      {"relational_node_binding_v1", "2",
       "6167677265676174652e676c6f62616c2d636f756e742d737461722e7631|"
       "4|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

// RCP-049-TEST-NODE-DRIVEN-COUNT-STAR-COMPOSITION-V1
sblr::SblrOperationEnvelope NodeDrivenCountStarLimitEnvelope() {
  auto envelope = GlobalCountStarValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000c980";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "3";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "3",
       "019f0000-0000-7300-8000-00000000c981|"
       "019f0000-0000-7400-8000-000000009004|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "5", "1|-|3|-|-|1|-|31"});
  envelope.operands.push_back(
      {"relational_node_v1", "3", "7|0|2|2|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "3",
       "6c696d69742e626f756e642d636f756e742e7631|5|-|-|-"});
  return envelope;
}

sblr::SblrOperationEnvelope GlobalCountExpressionValuesEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      "qow.live.values.count-expression");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-000000009100"},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-000000009101|"
       "019f0000-0000-7400-8000-000000009102|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-000000009103|"
       "019f0000-0000-7400-8000-000000009104|1|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "2", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "3", "1|-|1|-|-|1|-|39"},
      {"relational_expression_v1", "4", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "5",
       "3|-|1|-|019f0000-0000-7500-8000-000000009105|-|-|-"},
      {"relational_expression_v1", "6",
       "4|5|2|019de5fc-2400-784a-9aec-371f8b95b7ea|-|-|-|-"},
      {"relational_output_v1", "1", "1|1|1|1|0|76616c7565"},
      {"relational_output_v1", "2",
       "2|6|2|1|0|6e6f6e5f6e756c6c5f636f756e74"},
      {"relational_values_row_v1", "1", "1"},
      {"relational_values_row_v1", "2", "2"},
      {"relational_values_row_v1", "3", "3"},
      {"relational_values_row_v1", "4", "4"},
      {"relational_node_v1", "1", "13|0|-|1|1,2,3,4"},
      {"relational_node_v1", "2", "5|0|1|2|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|1,2,3,4|-|-|-"},
      {"relational_node_binding_v1", "2",
       "6167677265676174652e676c6f62616c2d636f756e742d65787072657373696f6e2e7631|"
       "6|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

// RCP-049-TEST-NODE-DRIVEN-COUNT-EXPRESSION-COMPOSITION-V1
sblr::SblrOperationEnvelope NodeDrivenCountExpressionLimitEnvelope() {
  auto envelope = GlobalCountExpressionValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000c990";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "3";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "3",
       "019f0000-0000-7300-8000-00000000c991|"
       "019f0000-0000-7400-8000-000000009104|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "7", "1|-|3|-|-|1|-|31"});
  envelope.operands.push_back(
      {"relational_node_v1", "3", "7|0|2|2|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "3",
       "6c696d69742e626f756e642d636f756e742e7631|7|-|-|-"});
  return envelope;
}

sblr::SblrOperationEnvelope GlobalSumExpressionValuesEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      "qow.live.values.sum-expression");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-000000009200"},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-000000009201|"
       "019f0000-0000-7400-8000-000000009202|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-000000009203|"
       "019f0000-0000-7400-8000-000000009204|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|35"},
      {"relational_expression_v1", "2", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "3", "1|-|1|-|-|1|-|2d32"},
      {"relational_expression_v1", "4", "1|-|1|-|-|1|-|37"},
      {"relational_expression_v1", "5",
       "3|-|1|-|019f0000-0000-7500-8000-000000009205|-|-|-"},
      {"relational_expression_v1", "6",
       "4|5|2|019de5fc-2400-72e4-8549-82b2eef5a777|-|-|-|-"},
      {"relational_output_v1", "1", "1|1|1|1|0|76616c7565"},
      {"relational_output_v1", "2", "2|6|2|1|0|73756d5f76616c7565"},
      {"relational_values_row_v1", "1", "1"},
      {"relational_values_row_v1", "2", "2"},
      {"relational_values_row_v1", "3", "3"},
      {"relational_values_row_v1", "4", "4"},
      {"relational_node_v1", "1", "13|0|-|1|1,2,3,4"},
      {"relational_node_v1", "2", "5|0|1|2|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|1,2,3,4|-|-|-"},
      {"relational_node_binding_v1", "2",
       "6167677265676174652e676c6f62616c2d73756d2d65787072657373696f6e2e7631|"
       "6|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

// RCP-049-TEST-NODE-DRIVEN-SUM-EXPRESSION-COMPOSITION-V1
sblr::SblrOperationEnvelope NodeDrivenSumExpressionLimitEnvelope() {
  auto envelope = GlobalSumExpressionValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000c9a0";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "3";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "3",
       "019f0000-0000-7300-8000-00000000c9a1|"
       "019f0000-0000-7400-8000-000000009204|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "7", "1|-|3|-|-|1|-|31"});
  envelope.operands.push_back(
      {"relational_node_v1", "3", "7|0|2|2|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "3",
       "6c696d69742e626f756e642d636f756e742e7631|7|-|-|-"});
  return envelope;
}

std::string EncodeHex(std::string_view value);

enum class UnaryAggregateKind {
  kCount,
  kSum,
  kAvg,
  kMin,
  kMax,
  kBoolAnd,
  kBoolOr,
  kEvery,
  kStddevPop,
  kVariancePop,
  kStddev,
  kVariance,
  kStddevSamp,
  kVarianceSamp,
};

struct UnaryAggregateTestProfile {
  UnaryAggregateKind kind;
  std::string_view name;
  std::string_view stem;
  std::string_view uuid_family;
  std::string_view function_uuid;
  std::string_view output_name;
  std::string_view result_type;
  bool boolean_input;
  bool result_nullable;
};

constexpr std::array<UnaryAggregateTestProfile, 14>
    kUnaryAggregateTestProfiles = {{
        {UnaryAggregateKind::kCount,
         "COUNT",
         "count",
         "b0",
         "019de5fc-2400-784a-9aec-371f8b95b7ea",
         "count_value",
         "int64",
         false,
         false},
        {UnaryAggregateKind::kSum,
         "SUM",
         "sum",
         "b1",
         "019de5fc-2400-72e4-8549-82b2eef5a777",
         "sum_value",
         "int64",
         false,
         true},
        {UnaryAggregateKind::kAvg,
         "AVG",
         "avg",
         "b2",
         "019de5fc-2400-78ac-b50c-45b832831004",
         "avg_value",
         "real64",
         false,
         true},
        {UnaryAggregateKind::kMin,
         "MIN",
         "min",
         "b3",
         "019de5fc-2400-781c-881b-4af4d55d402b",
         "min_value",
         "int64",
         false,
         true},
        {UnaryAggregateKind::kMax,
         "MAX",
         "max",
         "b4",
         "019de5fc-2400-7d1e-8aa4-80bc647fbd9a",
         "max_value",
         "int64",
         false,
         true},
        {UnaryAggregateKind::kBoolAnd,
         "BOOL_AND",
         "bool-and",
         "b5",
         "019de5fc-2400-78b0-ad98-a681e93b4c49",
         "bool_and_value",
         "boolean",
         true,
         true},
        {UnaryAggregateKind::kBoolOr,
         "BOOL_OR",
         "bool-or",
         "b6",
         "019de5fc-2400-7c2a-a3f2-e4b9d36df403",
         "bool_or_value",
         "boolean",
         true,
         true},
        {UnaryAggregateKind::kEvery,
         "EVERY",
         "every",
         "b7",
         "019dffbb-f000-7876-9644-ae83b363d3bc",
         "every_value",
         "boolean",
         true,
         true},
        {UnaryAggregateKind::kStddevPop,
         "STDDEV_POP",
         "stddev-pop",
         "b8",
         "019de5fc-2400-73c9-ba10-4665f741215d",
         "stddev_pop_value",
         "real64",
         false,
         true},
        {UnaryAggregateKind::kVariancePop,
         "VARIANCE_POP",
         "variance-pop",
         "b9",
         "019de5fc-2400-7fda-b470-e85414dcb314",
         "variance_pop_value",
         "real64",
         false,
         true},
        {UnaryAggregateKind::kStddev,
         "STDDEV",
         "stddev",
         "ba",
         "019dffbb-f000-7475-8516-ff003b2bdad9",
         "stddev_value",
         "real64",
         false,
         true},
        {UnaryAggregateKind::kVariance,
         "VARIANCE",
         "variance",
         "bb",
         "019dffbb-f000-7968-82c5-04cffbeb971b",
         "variance_value",
         "real64",
         false,
         true},
        {UnaryAggregateKind::kStddevSamp,
         "STDDEV_SAMP",
         "stddev-samp",
         "bc",
         "019dffbb-f000-7d99-a495-70f9c3b1b587",
         "stddev_samp_value",
         "real64",
         false,
         true},
        {UnaryAggregateKind::kVarianceSamp,
         "VARIANCE_SAMP",
         "variance-samp",
         "bd",
         "019dffbb-f000-732b-8a0c-2aa88b04f3c5",
         "variance_samp_value",
         "real64",
         false,
         true},
    }};

enum class AggregateModifierProfile {
  kFilter,
  kDistinct,
  kDistinctFilter,
};

std::string_view AggregateModifierName(
    const AggregateModifierProfile profile) {
  switch (profile) {
    case AggregateModifierProfile::kFilter:
      return "FILTER";
    case AggregateModifierProfile::kDistinct:
      return "DISTINCT";
    case AggregateModifierProfile::kDistinctFilter:
      return "DISTINCT FILTER";
  }
  return "unknown";
}

sblr::SblrOperationEnvelope GlobalUnaryAggregateModifierValuesEnvelope(
    const UnaryAggregateTestProfile& target,
    const AggregateModifierProfile profile) {
  const bool has_filter = profile != AggregateModifierProfile::kDistinct;
  const std::string modifier_uuid =
      profile == AggregateModifierProfile::kFilter
          ? "01"
          : profile == AggregateModifierProfile::kDistinct
                ? "02"
                : "03";
  const std::string modifier_suffix =
      profile == AggregateModifierProfile::kFilter
          ? "-filter"
          : profile == AggregateModifierProfile::kDistinct
                ? "-distinct"
                : "-distinct-filter";
  const auto fixture_uuid = [&](const std::string_view family,
                                const std::string_view ordinal) {
    return "019f0000-0000-" + std::string(family) +
           "-8000-00000000" + std::string(target.uuid_family) +
           std::string(ordinal);
  };
  const std::string semantic_variant =
      "aggregate.global-" + std::string(target.stem) + modifier_suffix +
      "-expression.v1";
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      "qow.live.values." + std::string(target.stem) + "-modifier");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       fixture_uuid("7000", modifier_uuid)},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       fixture_uuid("7300", "11") + "|" + fixture_uuid("7400", "12") +
           "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       fixture_uuid("7300", "13") + "|" + fixture_uuid("7400", "14") +
           "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       fixture_uuid("7300", "15") + "|" + fixture_uuid("7400", "16") +
           (target.result_nullable ? "|2|-|-|-|-|-" : "|1|-|-|-|-|-")},
      {"relational_expression_v1", "1",
       target.boolean_input ? "1|-|1|-|-|6|-|74727565"
                            : "1|-|1|-|-|1|-|3130"},
      {"relational_expression_v1", "2", "1|-|2|-|-|6|-|54525545"},
      {"relational_expression_v1", "3",
       target.boolean_input ? "1|-|1|-|-|6|-|66616c7365"
                            : "1|-|1|-|-|1|-|3230"},
      {"relational_expression_v1", "4", "1|-|2|-|-|6|-|46414c5345"},
      {"relational_expression_v1", "5",
       target.boolean_input ? "1|-|1|-|-|6|-|66616c7365"
                            : "1|-|1|-|-|1|-|3230"},
      {"relational_expression_v1", "6", "1|-|2|-|-|6|-|54525545"},
      {"relational_expression_v1", "7",
       target.boolean_input ? "1|-|1|-|-|7|-|2d"
                            : "1|-|1|-|-|1|-|3330"},
      {"relational_expression_v1", "8", "1|-|2|-|-|7|-|2d"},
      {"relational_expression_v1", "9",
       target.boolean_input ? "1|-|1|-|-|6|-|74727565"
                            : "1|-|1|-|-|1|-|3430"},
      {"relational_expression_v1", "10", "1|-|2|-|-|6|-|54525545"},
      {"relational_expression_v1", "11",
       target.boolean_input ? "1|-|1|-|-|6|-|74727565"
                            : "1|-|1|-|-|1|-|3430"},
      {"relational_expression_v1", "12", "1|-|2|-|-|6|-|54525545"},
      {"relational_expression_v1", "13",
       "3|-|1|-|" + fixture_uuid("7500", "17") + "|-|-|-"},
      {"relational_expression_v1", "14",
       "3|-|2|-|" + fixture_uuid("7500", "18") + "|-|-|-"},
      {"relational_expression_v1", "15",
       std::string("4|") + (has_filter ? "13,14" : "13") +
           "|3|" + std::string(target.function_uuid) + "|-|-|-|-"},
      {"relational_output_v1", "1", "1|1|1|1|0|76616c7565"},
      {"relational_output_v1", "2", "1|2|2|1|1|73656c6563746564"},
      {"relational_output_v1", "3",
       "2|15|3|1|0|" + EncodeHex(target.output_name)},
      {"relational_values_row_v1", "1", "1,2"},
      {"relational_values_row_v1", "2", "3,4"},
      {"relational_values_row_v1", "3", "5,6"},
      {"relational_values_row_v1", "4", "7,8"},
      {"relational_values_row_v1", "5", "9,10"},
      {"relational_values_row_v1", "6", "11,12"},
      {"relational_node_v1", "1", "13|0|-|1,2|1,2,3,4,5,6"},
      {"relational_node_v1", "2", "5|0|1|3|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5,6,7,8,9,10,11,12|-|-|-"},
      {"relational_node_binding_v1", "2",
       EncodeHex(semantic_variant) + "|15|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope GlobalAvgExpressionValuesEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      "qow.live.values.avg-expression");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-000000009300"},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-000000009301|"
       "019f0000-0000-7400-8000-000000009302|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-000000009303|"
       "019f0000-0000-7400-8000-000000009304|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|32"},
      {"relational_expression_v1", "2", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "3", "1|-|1|-|-|1|-|35"},
      {"relational_expression_v1", "4", "1|-|1|-|-|1|-|38"},
      {"relational_expression_v1", "5",
       "3|-|1|-|019f0000-0000-7500-8000-000000009305|-|-|-"},
      {"relational_expression_v1", "6",
       "4|5|2|019de5fc-2400-78ac-b50c-45b832831004|-|-|-|-"},
      {"relational_output_v1", "1", "1|1|1|1|0|76616c7565"},
      {"relational_output_v1", "2", "2|6|2|1|0|6176675f76616c7565"},
      {"relational_values_row_v1", "1", "1"},
      {"relational_values_row_v1", "2", "2"},
      {"relational_values_row_v1", "3", "3"},
      {"relational_values_row_v1", "4", "4"},
      {"relational_node_v1", "1", "13|0|-|1|1,2,3,4"},
      {"relational_node_v1", "2", "5|0|1|2|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|1,2,3,4|-|-|-"},
      {"relational_node_binding_v1", "2",
       "6167677265676174652e676c6f62616c2d6176672d65787072657373696f6e2e7631|"
       "6|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

// RCP-049-TEST-NODE-DRIVEN-AVG-EXPRESSION-COMPOSITION-V1
sblr::SblrOperationEnvelope NodeDrivenAvgExpressionLimitEnvelope() {
  auto envelope = GlobalAvgExpressionValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000ca00";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "3";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "3",
       "019f0000-0000-7300-8000-00000000ca01|"
       "019f0000-0000-7400-8000-000000009304|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "7", "1|-|3|-|-|1|-|31"});
  envelope.operands.push_back(
      {"relational_node_v1", "3", "7|0|2|2|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "3",
       "6c696d69742e626f756e642d636f756e742e7631|7|-|-|-"});
  return envelope;
}

sblr::SblrOperationEnvelope GlobalExtremumExpressionValuesEnvelope(
    const bool maximum) {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      maximum ? "qow.live.values.max-expression"
              : "qow.live.values.min-expression");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       maximum ? "019f0000-0000-7000-8000-000000009500"
               : "019f0000-0000-7000-8000-000000009400"},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       maximum
           ? "019f0000-0000-7300-8000-000000009501|"
             "019f0000-0000-7400-8000-000000009502|2|-|-|-|-|-"
           : "019f0000-0000-7300-8000-000000009401|"
             "019f0000-0000-7400-8000-000000009402|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       maximum
           ? "019f0000-0000-7300-8000-000000009503|"
             "019f0000-0000-7400-8000-000000009504|2|-|-|-|-|-"
           : "019f0000-0000-7300-8000-000000009403|"
             "019f0000-0000-7400-8000-000000009404|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|2d34"},
      {"relational_expression_v1", "2", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "3", "1|-|1|-|-|1|-|39"},
      {"relational_expression_v1", "4", "1|-|1|-|-|1|-|33"},
      {"relational_expression_v1", "5",
       maximum
           ? "3|-|1|-|019f0000-0000-7500-8000-000000009505|-|-|-"
           : "3|-|1|-|019f0000-0000-7500-8000-000000009405|-|-|-"},
      {"relational_expression_v1", "6",
       maximum
           ? "4|5|2|019de5fc-2400-7d1e-8aa4-80bc647fbd9a|-|-|-|-"
           : "4|5|2|019de5fc-2400-781c-881b-4af4d55d402b|-|-|-|-"},
      {"relational_output_v1", "1", "1|1|1|1|0|76616c7565"},
      {"relational_output_v1", "2",
       maximum ? "2|6|2|1|0|6d61785f76616c7565"
               : "2|6|2|1|0|6d696e5f76616c7565"},
      {"relational_values_row_v1", "1", "1"},
      {"relational_values_row_v1", "2", "2"},
      {"relational_values_row_v1", "3", "3"},
      {"relational_values_row_v1", "4", "4"},
      {"relational_node_v1", "1", "13|0|-|1|1,2,3,4"},
      {"relational_node_v1", "2", "5|0|1|2|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|1,2,3,4|-|-|-"},
      {"relational_node_binding_v1", "2",
       maximum
           ? "6167677265676174652e676c6f62616c2d6d61782d65787072657373696f6e2e7631|"
             "6|-|-|-"
           : "6167677265676174652e676c6f62616c2d6d696e2d65787072657373696f6e2e7631|"
             "6|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

// RCP-049-TEST-NODE-DRIVEN-EXTREMUM-EXPRESSION-COMPOSITION-V1
sblr::SblrOperationEnvelope NodeDrivenExtremumExpressionLimitEnvelope(
    const bool maximum) {
  auto envelope = GlobalExtremumExpressionValuesEnvelope(maximum);
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = maximum
                          ? "019f0000-0000-7000-8000-00000000c9c0"
                          : "019f0000-0000-7000-8000-00000000c9b0";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "3";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "3",
       (maximum ? "019f0000-0000-7300-8000-00000000c9c1|"
                  "019f0000-0000-7400-8000-000000009504|1|-|-|-|-|-"
                : "019f0000-0000-7300-8000-00000000c9b1|"
                  "019f0000-0000-7400-8000-000000009404|1|-|-|-|-|-")});
  envelope.operands.push_back(
      {"relational_expression_v1", "7", "1|-|3|-|-|1|-|31"});
  envelope.operands.push_back(
      {"relational_node_v1", "3", "7|0|2|2|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "3",
       "6c696d69742e626f756e642d636f756e742e7631|7|-|-|-"});
  return envelope;
}

enum class BooleanAggregateKind {
  kBoolAnd,
  kBoolOr,
  kEvery,
};

std::string_view BooleanAggregateName(const BooleanAggregateKind kind) {
  switch (kind) {
    case BooleanAggregateKind::kBoolAnd:
      return "BOOL_AND";
    case BooleanAggregateKind::kBoolOr:
      return "BOOL_OR";
    case BooleanAggregateKind::kEvery:
      return "EVERY";
  }
  return "UNKNOWN";
}

sblr::SblrOperationEnvelope GlobalBooleanAggregateExpressionValuesEnvelope(
    const BooleanAggregateKind kind) {
  const bool bool_and = kind == BooleanAggregateKind::kBoolAnd;
  const bool bool_or = kind == BooleanAggregateKind::kBoolOr;
  const std::string operation =
      bool_and ? "qow.live.values.bool-and-expression"
               : (bool_or ? "qow.live.values.bool-or-expression"
                          : "qow.live.values.every-expression");
  const std::string tree_uuid =
      bool_and ? "019f0000-0000-7000-8000-000000009600"
               : (bool_or ? "019f0000-0000-7000-8000-000000009700"
                          : "019f0000-0000-7000-8000-000000009800");
  const std::string input_descriptor =
      bool_and ? "019f0000-0000-7300-8000-000000009601"
               : (bool_or ? "019f0000-0000-7300-8000-000000009701"
                          : "019f0000-0000-7300-8000-000000009801");
  const std::string input_type =
      bool_and ? "019f0000-0000-7400-8000-000000009602"
               : (bool_or ? "019f0000-0000-7400-8000-000000009702"
                          : "019f0000-0000-7400-8000-000000009802");
  const std::string result_descriptor =
      bool_and ? "019f0000-0000-7300-8000-000000009603"
               : (bool_or ? "019f0000-0000-7300-8000-000000009703"
                          : "019f0000-0000-7300-8000-000000009803");
  const std::string result_type =
      bool_and ? "019f0000-0000-7400-8000-000000009604"
               : (bool_or ? "019f0000-0000-7400-8000-000000009704"
                          : "019f0000-0000-7400-8000-000000009804");
  const std::string bound_name =
      bool_and ? "019f0000-0000-7500-8000-000000009605"
               : (bool_or ? "019f0000-0000-7500-8000-000000009705"
                          : "019f0000-0000-7500-8000-000000009805");
  const std::string function_uuid =
      bool_and ? "019de5fc-2400-78b0-ad98-a681e93b4c49"
               : (bool_or ? "019de5fc-2400-7c2a-a3f2-e4b9d36df403"
                          : "019dffbb-f000-7876-9644-ae83b363d3bc");
  const std::string output_name =
      bool_and ? "626f6f6c5f616e645f76616c7565"
               : (bool_or ? "626f6f6c5f6f725f76616c7565"
                          : "65766572795f76616c7565");
  const std::string semantic_variant =
      bool_and
          ? "6167677265676174652e676c6f62616c2d626f6f6c2d616e642d65787072657373696f6e2e7631"
          : (bool_or
                 ? "6167677265676174652e676c6f62616c2d626f6f6c2d6f722d65787072657373696f6e2e7631"
                 : "6167677265676174652e676c6f62616c2d65766572792d65787072657373696f6e2e7631");

  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", operation);
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid", tree_uuid},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       input_descriptor + "|" + input_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       result_descriptor + "|" + result_type + "|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|6|-|74727565"},
      {"relational_expression_v1", "2", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "3", "1|-|1|-|-|6|-|66616c7365"},
      {"relational_expression_v1", "4", "1|-|1|-|-|6|-|74727565"},
      {"relational_expression_v1", "5",
       "3|-|1|-|" + bound_name + "|-|-|-"},
      {"relational_expression_v1", "6",
       "4|5|2|" + function_uuid + "|-|-|-|-"},
      {"relational_output_v1", "1", "1|1|1|1|0|666c6167"},
      {"relational_output_v1", "2", "2|6|2|1|0|" + output_name},
      {"relational_values_row_v1", "1", "1"},
      {"relational_values_row_v1", "2", "2"},
      {"relational_values_row_v1", "3", "3"},
      {"relational_values_row_v1", "4", "4"},
      {"relational_node_v1", "1", "13|0|-|1|1,2,3,4"},
      {"relational_node_v1", "2", "5|0|1|2|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|1,2,3,4|-|-|-"},
      {"relational_node_binding_v1", "2",
       semantic_variant + "|6|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

// RCP-049-TEST-NODE-DRIVEN-BOOLEAN-AGGREGATE-COMPOSITION-V1
sblr::SblrOperationEnvelope NodeDrivenBooleanAggregateLimitEnvelope(
    const BooleanAggregateKind kind) {
  auto envelope = GlobalBooleanAggregateExpressionValuesEnvelope(kind);
  const bool bool_and = kind == BooleanAggregateKind::kBoolAnd;
  const bool bool_or = kind == BooleanAggregateKind::kBoolOr;
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value =
          bool_and ? "019f0000-0000-7000-8000-00000000c9d0"
                   : (bool_or ? "019f0000-0000-7000-8000-00000000c9e0"
                              : "019f0000-0000-7000-8000-00000000c9f0");
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "3";
    }
  }
  const std::string descriptor =
      bool_and ? "019f0000-0000-7300-8000-00000000c9d1"
               : (bool_or ? "019f0000-0000-7300-8000-00000000c9e1"
                          : "019f0000-0000-7300-8000-00000000c9f1");
  const std::string type =
      bool_and ? "019f0000-0000-7400-8000-000000009604"
               : (bool_or ? "019f0000-0000-7400-8000-000000009704"
                          : "019f0000-0000-7400-8000-000000009804");
  envelope.operands.push_back(
      {"relational_descriptor_v1", "3",
       descriptor + "|" + type + "|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "7", "1|-|3|-|-|1|-|31"});
  envelope.operands.push_back(
      {"relational_node_v1", "3", "7|0|2|2|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "3",
       "6c696d69742e626f756e642d636f756e742e7631|7|-|-|-"});
  return envelope;
}

struct StatisticalAggregateProfile {
  std::string_view name;
  std::string_view operation;
  std::string_view semantic_variant;
  std::string_view tree_uuid;
  std::string_view input_descriptor_uuid;
  std::string_view input_type_uuid;
  std::string_view result_descriptor_uuid;
  std::string_view result_type_uuid;
  std::string_view bound_name_uuid;
  std::string_view function_uuid;
  std::string_view output_name;
  double expected;
};

constexpr StatisticalAggregateProfile kStatisticalAggregateProfiles[] = {
    {"STDDEV_POP",
     "qow.live.values.stddev-pop-expression",
     "aggregate.global-stddev-pop-expression.v1",
     "019f0000-0000-7000-8000-000000009900",
     "019f0000-0000-7300-8000-000000009901",
     "019f0000-0000-7400-8000-000000009902",
     "019f0000-0000-7300-8000-000000009903",
     "019f0000-0000-7400-8000-000000009904",
     "019f0000-0000-7500-8000-000000009905",
     "019de5fc-2400-73c9-ba10-4665f741215d",
     "stddev_pop_value",
     0.816496580927726},
    {"VARIANCE_POP",
     "qow.live.values.variance-pop-expression",
     "aggregate.global-variance-pop-expression.v1",
     "019f0000-0000-7000-8000-000000009a00",
     "019f0000-0000-7300-8000-000000009a01",
     "019f0000-0000-7400-8000-000000009a02",
     "019f0000-0000-7300-8000-000000009a03",
     "019f0000-0000-7400-8000-000000009a04",
     "019f0000-0000-7500-8000-000000009a05",
     "019de5fc-2400-7fda-b470-e85414dcb314",
     "variance_pop_value",
     2.0 / 3.0},
    {"STDDEV",
     "qow.live.values.stddev-expression",
     "aggregate.global-stddev-expression.v1",
     "019f0000-0000-7000-8000-000000009b00",
     "019f0000-0000-7300-8000-000000009b01",
     "019f0000-0000-7400-8000-000000009b02",
     "019f0000-0000-7300-8000-000000009b03",
     "019f0000-0000-7400-8000-000000009b04",
     "019f0000-0000-7500-8000-000000009b05",
     "019dffbb-f000-7475-8516-ff003b2bdad9",
     "stddev_value",
     1.0},
    {"VARIANCE",
     "qow.live.values.variance-expression",
     "aggregate.global-variance-expression.v1",
     "019f0000-0000-7000-8000-000000009c00",
     "019f0000-0000-7300-8000-000000009c01",
     "019f0000-0000-7400-8000-000000009c02",
     "019f0000-0000-7300-8000-000000009c03",
     "019f0000-0000-7400-8000-000000009c04",
     "019f0000-0000-7500-8000-000000009c05",
     "019dffbb-f000-7968-82c5-04cffbeb971b",
     "variance_value",
     1.0},
    {"STDDEV_SAMP",
     "qow.live.values.stddev-samp-expression",
     "aggregate.global-stddev-samp-expression.v1",
     "019f0000-0000-7000-8000-000000009d00",
     "019f0000-0000-7300-8000-000000009d01",
     "019f0000-0000-7400-8000-000000009d02",
     "019f0000-0000-7300-8000-000000009d03",
     "019f0000-0000-7400-8000-000000009d04",
     "019f0000-0000-7500-8000-000000009d05",
     "019dffbb-f000-7d99-a495-70f9c3b1b587",
     "stddev_samp_value",
     1.0},
    {"VARIANCE_SAMP",
     "qow.live.values.variance-samp-expression",
     "aggregate.global-variance-samp-expression.v1",
     "019f0000-0000-7000-8000-000000009e00",
     "019f0000-0000-7300-8000-000000009e01",
     "019f0000-0000-7400-8000-000000009e02",
     "019f0000-0000-7300-8000-000000009e03",
     "019f0000-0000-7400-8000-000000009e04",
     "019f0000-0000-7500-8000-000000009e05",
     "019dffbb-f000-732b-8a0c-2aa88b04f3c5",
     "variance_samp_value",
     1.0},
};

std::string EncodeHex(const std::string_view value) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2);
  for (const auto byte : value) {
    const auto octet = static_cast<unsigned char>(byte);
    encoded.push_back(kHex[octet >> 4]);
    encoded.push_back(kHex[octet & 0x0f]);
  }
  return encoded;
}

sblr::SblrOperationEnvelope GroupedCountSumValuesEnvelope(
    const bool one_group = false) {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      one_group ? "qow.live.values.grouped-count-sum-one-group"
                : "qow.live.values.grouped-count-sum-many-groups");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       one_group ? "019f0000-0000-7000-8000-00000000e100"
                 : "019f0000-0000-7000-8000-00000000e000"},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-00000000e001|"
       "019f0000-0000-7400-8000-00000000e002|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-00000000e003|"
       "019f0000-0000-7400-8000-00000000e004|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       "019f0000-0000-7300-8000-00000000e005|"
       "019f0000-0000-7400-8000-00000000e006|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "4",
       "019f0000-0000-7300-8000-00000000e007|"
       "019f0000-0000-7400-8000-00000000e008|2|-|-|-|-|-"},
  };

  constexpr std::string_view kManyGroupKeys[] = {
      "31", "31", "32", "2d", "32", "31"};
  constexpr std::string_view kOneGroupKeys[] = {
      "31", "31", "31", "31", "31", "31"};
  constexpr std::string_view kAmounts[] = {
      "3130", "3230", "35", "37", "2d", "2d35"};
  for (std::size_t row = 0; row < std::size(kAmounts); ++row) {
    const auto key_expression_id = row * 2 + 1;
    const auto amount_expression_id = key_expression_id + 1;
    const auto key = one_group ? kOneGroupKeys[row] : kManyGroupKeys[row];
    const bool null_key = !one_group && row == 3;
    const bool null_amount = row == 4;
    envelope.operands.push_back(
        {"relational_expression_v1", std::to_string(key_expression_id),
         "1|-|1|-|-|" + std::string(null_key ? "7" : "1") + "|-|" +
             std::string(key)});
    envelope.operands.push_back(
        {"relational_expression_v1", std::to_string(amount_expression_id),
         "1|-|2|-|-|" + std::string(null_amount ? "7" : "1") + "|-|" +
             std::string(kAmounts[row])});
    envelope.operands.push_back(
        {"relational_values_row_v1", std::to_string(row + 1),
         std::to_string(key_expression_id) + "," +
             std::to_string(amount_expression_id)});
  }
  envelope.operands.insert(
      envelope.operands.end(),
      {{"relational_expression_v1", "13",
        "3|-|1|-|019f0000-0000-7500-8000-00000000e009|-|-|-"},
       {"relational_expression_v1", "14",
        "4|-|3|019de5fc-2400-784a-9aec-371f8b95b7ea|-|-|-|-"},
       {"relational_expression_v1", "15",
        "3|-|2|-|019f0000-0000-7500-8000-00000000e00a|-|-|-"},
       {"relational_expression_v1", "16",
        "4|15|4|019de5fc-2400-72e4-8549-82b2eef5a777|-|-|-|-"},
       {"relational_output_v1", "1", "1|1|1|1|0|67726f75705f6b6579"},
       {"relational_output_v1", "2", "1|2|2|1|1|616d6f756e74"},
       {"relational_output_v1", "3", "2|13|1|1|0|67726f75705f6b6579"},
       {"relational_output_v1", "4", "2|14|3|1|1|726f775f636f756e74"},
       {"relational_output_v1", "5", "2|16|4|1|2|746f74616c5f616d6f756e74"},
       {"relational_node_v1", "1", "13|0|-|1,2|1,2,3,4,5,6"},
       {"relational_node_v1", "2", "5|0|1|1,3,4|-"},
       {"relational_node_binding_v1", "1",
        "76616c7565732e6c69746572616c2d7461626c652e7631|"
        "1,2,3,4,5,6,7,8,9,10,11,12|-|-|-"},
       {"relational_node_binding_v1", "2",
        EncodeHex("aggregate.grouped-int64-key-count-sum.v1") +
            "|13,14,16|-|-|-"}});
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

// RCP-049-TEST-NODE-DRIVEN-GROUPED-COUNT-SUM-COMPOSITION-V1
sblr::SblrOperationEnvelope NodeDrivenGroupedCountSumLimitEnvelope() {
  auto envelope = GroupedCountSumValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000cc00";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "3";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "5",
       "019f0000-0000-7300-8000-00000000cc01|"
       "019f0000-0000-7400-8000-00000000e008|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "17", "1|-|5|-|-|1|-|31"});
  envelope.operands.push_back(
      {"relational_node_v1", "3", "7|0|2|1,3,4|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "3",
       "6c696d69742e626f756e642d636f756e742e7631|17|-|-|-"});
  return envelope;
}

sblr::SblrOperationEnvelope RollupCountSumValuesEnvelope();

sblr::SblrOperationEnvelope TwoKeyGroupedCountSumValuesEnvelope() {
  auto envelope = RollupCountSumValuesEnvelope();
  envelope.trace_key = "qow.live.values.two-key-grouped-count-sum";
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000cd00";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "2") {
      operand.value =
          EncodeHex("aggregate.grouped-int64-keys-count-sum.v1") +
          "|19,20,21,23|-|-|-";
    }
  }
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

// RCP-049-TEST-NODE-DRIVEN-GROUPING-EXPANSION-COMPOSITION-V1
sblr::SblrOperationEnvelope NodeDrivenGroupedExpansionLimitEnvelope(
    sblr::SblrOperationEnvelope envelope,
    const std::string_view tree_uuid,
    const std::string_view output_descriptor_ids) {
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = tree_uuid;
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "3";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "9",
       "019f0000-0000-7300-8000-00000000cd01|"
       "019f0000-0000-7400-8000-00000000e208|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "27", "1|-|9|-|-|1|-|31"});
  envelope.operands.push_back(
      {"relational_node_v1", "3",
       "7|0|2|" + std::string(output_descriptor_ids) + "|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "3",
       "6c696d69742e626f756e642d636f756e742e7631|27|-|-|-"});
  return envelope;
}

sblr::SblrOperationEnvelope TwoKeyGroupedCountSumHavingValuesEnvelope() {
  auto envelope = TwoKeyGroupedCountSumValuesEnvelope();
  envelope.trace_key = "qow.live.values.two-key-grouped-count-sum-having";
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000ce00";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "3";
    }
  }
  envelope.operands.insert(
      envelope.operands.end(),
      {{"relational_descriptor_v1", "9",
        "019f0000-0000-7300-8000-00000000ce01|"
        "019f0000-0000-7400-8000-00000000e208|1|-|-|-|-|-"},
       {"relational_descriptor_v1", "10",
        "019f0000-0000-7300-8000-00000000ce02|"
        "019f0000-0000-7400-8000-00000000ce03|2|-|-|-|-|-"},
       {"relational_expression_v1", "24",
        "3|-|3|-|019f0000-0000-7500-8000-00000000e20d|-|-|-"},
       {"relational_expression_v1", "25",
        "4|24|5|019de5fc-2400-72e4-8549-82b2eef5a777|-|-|-|-"},
       {"relational_expression_v1", "26", "1|-|9|-|-|1|-|36"},
       {"relational_expression_v1", "27", "6|25,26|10|-|-|-|3e|-"},
       {"relational_output_v1", "8", "3|19|1|1|0|6b65795f61"},
       {"relational_output_v1", "9", "3|20|2|1|1|6b65795f62"},
       {"relational_output_v1", "10",
        "3|21|4|1|2|726f775f636f756e74"},
       {"relational_output_v1", "11",
        "3|23|5|1|3|746f74616c5f616d6f756e74"},
       {"relational_node_v1", "3", "2|0|2|1,2,4,5|-"},
       {"relational_node_binding_v1", "3",
        "66696c7465722e686176696e672d73756d2d67742d696e7436342d6c69746572616c2e7631|27|-|-|-"}});
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

// RCP-049-TEST-NODE-DRIVEN-GROUPED-HAVING-COMPOSITION-V1
sblr::SblrOperationEnvelope NodeDrivenGroupedHavingLimitEnvelope() {
  auto envelope = TwoKeyGroupedCountSumHavingValuesEnvelope();
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000ce10";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "4";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "11",
       "019f0000-0000-7300-8000-00000000ce11|"
       "019f0000-0000-7400-8000-00000000e208|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "28", "1|-|11|-|-|1|-|31"});
  envelope.operands.push_back(
      {"relational_node_v1", "4", "7|0|3|1,2,4,5|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "4",
       "6c696d69742e626f756e642d636f756e742e7631|28|-|-|-"});
  return envelope;
}

sblr::SblrOperationEnvelope RollupCountSumValuesEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      "qow.live.values.rollup-count-sum");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-00000000e200"},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-00000000e201|"
       "019f0000-0000-7400-8000-00000000e202|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-00000000e203|"
       "019f0000-0000-7400-8000-00000000e204|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       "019f0000-0000-7300-8000-00000000e205|"
       "019f0000-0000-7400-8000-00000000e206|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "4",
       "019f0000-0000-7300-8000-00000000e207|"
       "019f0000-0000-7400-8000-00000000e208|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "5",
       "019f0000-0000-7300-8000-00000000e209|"
       "019f0000-0000-7400-8000-00000000e20a|2|-|-|-|-|-"},
  };

  struct RollupRow {
    std::string_view key_a;
    std::string_view key_b;
    std::string_view amount;
    bool null_a{false};
    bool null_b{false};
    bool null_amount{false};
  };
  constexpr std::array<RollupRow, 6> kRows = {
      RollupRow{"31", "3130", "35"},
      RollupRow{"31", "3230", "37"},
      RollupRow{"31", "2d", "33", false, true, false},
      RollupRow{"32", "3130", "34"},
      RollupRow{"2d", "3130", "38", true, false, false},
      RollupRow{"31", "3130", "2d", false, false, true},
  };
  for (std::size_t row = 0; row < kRows.size(); ++row) {
    const auto first_expression_id = row * 3 + 1;
    const auto& value = kRows[row];
    envelope.operands.push_back(
        {"relational_expression_v1", std::to_string(first_expression_id),
         "1|-|1|-|-|" + std::string(value.null_a ? "7" : "1") + "|-|" +
             std::string(value.key_a)});
    envelope.operands.push_back(
        {"relational_expression_v1",
         std::to_string(first_expression_id + 1),
         "1|-|2|-|-|" + std::string(value.null_b ? "7" : "1") + "|-|" +
             std::string(value.key_b)});
    envelope.operands.push_back(
        {"relational_expression_v1",
         std::to_string(first_expression_id + 2),
         "1|-|3|-|-|" +
             std::string(value.null_amount ? "7" : "1") + "|-|" +
             std::string(value.amount)});
    envelope.operands.push_back(
        {"relational_values_row_v1", std::to_string(row + 1),
         std::to_string(first_expression_id) + "," +
             std::to_string(first_expression_id + 1) + "," +
             std::to_string(first_expression_id + 2)});
  }
  envelope.operands.insert(
      envelope.operands.end(),
      {{"relational_expression_v1", "19",
        "3|-|1|-|019f0000-0000-7500-8000-00000000e20b|-|-|-"},
       {"relational_expression_v1", "20",
        "3|-|2|-|019f0000-0000-7500-8000-00000000e20c|-|-|-"},
       {"relational_expression_v1", "21",
        "4|-|4|019de5fc-2400-784a-9aec-371f8b95b7ea|-|-|-|-"},
       {"relational_expression_v1", "22",
        "3|-|3|-|019f0000-0000-7500-8000-00000000e20d|-|-|-"},
       {"relational_expression_v1", "23",
        "4|22|5|019de5fc-2400-72e4-8549-82b2eef5a777|-|-|-|-"},
       {"relational_output_v1", "1", "1|1|1|1|0|6b65795f61"},
       {"relational_output_v1", "2", "1|2|2|1|1|6b65795f62"},
       {"relational_output_v1", "3", "1|3|3|1|2|616d6f756e74"},
       {"relational_output_v1", "4", "2|19|1|1|0|6b65795f61"},
       {"relational_output_v1", "5", "2|20|2|1|1|6b65795f62"},
       {"relational_output_v1", "6", "2|21|4|1|2|726f775f636f756e74"},
       {"relational_output_v1", "7",
        "2|23|5|1|3|746f74616c5f616d6f756e74"},
       {"relational_node_v1", "1", "13|0|-|1,2,3|1,2,3,4,5,6"},
       {"relational_node_v1", "2", "5|0|1|1,2,4,5|-"},
       {"relational_node_binding_v1", "1",
        "76616c7565732e6c69746572616c2d7461626c652e7631|"
        "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18|-|-|-"},
       {"relational_node_binding_v1", "2",
        EncodeHex("aggregate.rollup-int64-keys-count-sum.v1") +
            "|19,20,21,23|-|-|-"}});
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope RollupCountSumGroupingValuesEnvelope() {
  auto envelope = RollupCountSumValuesEnvelope();
  envelope.operands.insert(
      envelope.operands.end(),
      {{"relational_descriptor_v1", "6",
        "019f0000-0000-7300-8000-00000000e20e|"
        "019f0000-0000-7400-8000-00000000e20f|1|-|-|-|-|-"},
       {"relational_descriptor_v1", "7",
        "019f0000-0000-7300-8000-00000000e210|"
        "019f0000-0000-7400-8000-00000000e211|1|-|-|-|-|-"},
       {"relational_descriptor_v1", "8",
        "019f0000-0000-7300-8000-00000000e212|"
        "019f0000-0000-7400-8000-00000000e213|1|-|-|-|-|-"},
       {"relational_expression_v1", "24",
        "5|19|6|-|-|-|67726f7570696e67|-"},
       {"relational_expression_v1", "25",
        "5|20|7|-|-|-|67726f7570696e67|-"},
       {"relational_expression_v1", "26",
        "6|19,20|8|-|-|-|67726f7570696e675f6964|-"},
       {"relational_output_v1", "8",
        "2|24|6|1|4|67726f7570696e675f61"},
       {"relational_output_v1", "9",
        "2|25|7|1|5|67726f7570696e675f62"},
       {"relational_output_v1", "10",
        "2|26|8|1|6|67726f7570696e675f6964"}});
  for (auto& operand : envelope.operands) {
    if (operand.type == "relational_node_v1" && operand.name == "2") {
      operand.value = "5|0|1|1,2,4,5,6,7,8|-";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "2") {
      operand.value =
          EncodeHex(
              "aggregate.rollup-int64-keys-count-sum-grouping.v1") +
          "|19,20,21,23,24,25,26|-|-|-";
    }
  }
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope CubeCountSumValuesEnvelope() {
  auto envelope = RollupCountSumValuesEnvelope();
  envelope.trace_key = "qow.live.values.cube-count-sum";
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000e300";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "2") {
      operand.value =
          EncodeHex("aggregate.cube-int64-keys-count-sum.v1") +
          "|19,20,21,23|-|-|-";
    }
  }
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope CubeCountSumGroupingValuesEnvelope() {
  auto envelope = RollupCountSumGroupingValuesEnvelope();
  envelope.trace_key = "qow.live.values.cube-count-sum-grouping";
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000e301";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "2") {
      operand.value = EncodeHex(
                          "aggregate.cube-int64-keys-count-sum-grouping.v1") +
                      "|19,20,21,23,24,25,26|-|-|-";
    }
  }
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope GroupingSetsCountSumValuesEnvelope() {
  auto envelope = RollupCountSumValuesEnvelope();
  envelope.trace_key = "qow.live.values.grouping-sets-count-sum";
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000e400";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "2") {
      operand.value =
          EncodeHex("aggregate.grouping-sets-int64-keys-count-sum.v1") +
          "|19,20,21,23|-|-|-";
    }
  }
  envelope.operands.insert(
      envelope.operands.end(),
      {{"relational_grouping_set_v1", "0", "2|20"},
       {"relational_grouping_set_v1", "1", "2|-"},
       {"relational_grouping_set_v1", "2", "2|19,20"},
       {"relational_grouping_set_v1", "3", "2|20"}});
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope GroupingSetsCountSumGroupingValuesEnvelope() {
  auto envelope = RollupCountSumGroupingValuesEnvelope();
  envelope.trace_key = "qow.live.values.grouping-sets-count-sum-grouping";
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000e401";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "2") {
      operand.value = EncodeHex(
                          "aggregate.grouping-sets-int64-keys-count-sum-"
                          "grouping.v1") +
                      "|19,20,21,23,24,25,26|-|-|-";
    }
  }
  envelope.operands.insert(
      envelope.operands.end(),
      {{"relational_grouping_set_v1", "0", "2|20"},
       {"relational_grouping_set_v1", "1", "2|-"},
       {"relational_grouping_set_v1", "2", "2|19,20"},
       {"relational_grouping_set_v1", "3", "2|20"}});
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope GlobalStatisticalAggregateExpressionValuesEnvelope(
    const StatisticalAggregateProfile& profile) {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", std::string(profile.operation));
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid", std::string(profile.tree_uuid)},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       std::string(profile.input_descriptor_uuid) + "|" +
           std::string(profile.input_type_uuid) + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       std::string(profile.result_descriptor_uuid) + "|" +
           std::string(profile.result_type_uuid) + "|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "2", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "3", "1|-|1|-|-|1|-|32"},
      {"relational_expression_v1", "4", "1|-|1|-|-|1|-|33"},
      {"relational_expression_v1", "5",
       "3|-|1|-|" + std::string(profile.bound_name_uuid) + "|-|-|-"},
      {"relational_expression_v1", "6",
       "4|5|2|" + std::string(profile.function_uuid) + "|-|-|-|-"},
      {"relational_output_v1", "1", "1|1|1|1|0|76616c7565"},
      {"relational_output_v1", "2",
       "2|6|2|1|0|" + EncodeHex(profile.output_name)},
      {"relational_values_row_v1", "1", "1"},
      {"relational_values_row_v1", "2", "2"},
      {"relational_values_row_v1", "3", "3"},
      {"relational_values_row_v1", "4", "4"},
      {"relational_node_v1", "1", "13|0|-|1|1,2,3,4"},
      {"relational_node_v1", "2", "5|0|1|2|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|1,2,3,4|-|-|-"},
      {"relational_node_binding_v1", "2",
       EncodeHex(profile.semantic_variant) + "|6|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

// RCP-049-TEST-NODE-DRIVEN-STATISTICAL-AGGREGATE-COMPOSITION-V1
sblr::SblrOperationEnvelope NodeDrivenStatisticalAggregateLimitEnvelope(
    const StatisticalAggregateProfile& profile,
    const std::string_view fixture_family) {
  auto envelope = GlobalStatisticalAggregateExpressionValuesEnvelope(profile);
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000" +
                      std::string(fixture_family) + "0";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "3";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "3",
       "019f0000-0000-7300-8000-00000000" +
           std::string(fixture_family) + "1|" +
           std::string(profile.result_type_uuid) + "|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "7", "1|-|3|-|-|1|-|31"});
  envelope.operands.push_back(
      {"relational_node_v1", "3", "7|0|2|2|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "3",
       "6c696d69742e626f756e642d636f756e742e7631|7|-|-|-"});
  return envelope;
}

enum class PairStatisticalAggregateKind {
  kCorr,
  kCovarPop,
  kCovarSamp,
  kRegrCount,
  kRegrAvgx,
  kRegrAvgy,
  kRegrIntercept,
  kRegrR2,
  kRegrSlope,
  kRegrSxx,
  kRegrSxy,
  kRegrSyy,
};

struct PairStatisticalAggregateProfile {
  PairStatisticalAggregateKind kind;
  std::string_view name;
  std::string_view operation;
  std::string_view semantic_variant;
  std::string_view uuid_family;
  std::string_view function_uuid;
  std::string_view output_name;
  double expected;
  bool count_result;
};

constexpr PairStatisticalAggregateProfile
    kPairStatisticalAggregateProfiles[] = {
        {PairStatisticalAggregateKind::kCorr,
         "CORR",
         "qow.live.values.corr-expression",
         "aggregate.global-corr-expression.v1",
         "a0",
         "019dffbb-f000-77bb-ba9b-2e78acf84521",
         "corr_value",
         1.0,
         false},
        {PairStatisticalAggregateKind::kCovarPop,
         "COVAR_POP",
         "qow.live.values.covar-pop-expression",
         "aggregate.global-covar-pop-expression.v1",
         "a1",
         "019dffbb-f000-7f09-8ceb-17ad4c70e99f",
         "covar_pop_value",
         4.0 / 3.0,
         false},
        {PairStatisticalAggregateKind::kCovarSamp,
         "COVAR_SAMP",
         "qow.live.values.covar-samp-expression",
         "aggregate.global-covar-samp-expression.v1",
         "a2",
         "019dffbb-f000-747d-bc01-caad9137d070",
         "covar_samp_value",
         2.0,
         false},
        {PairStatisticalAggregateKind::kRegrCount,
         "REGR_COUNT",
         "qow.live.values.regr-count-expression",
         "aggregate.global-regr-count-expression.v1",
         "a3",
         "019dffbb-f000-75aa-bbe6-a4a67dacb81f",
         "regr_count_value",
         3.0,
         true},
        {PairStatisticalAggregateKind::kRegrAvgx,
         "REGR_AVGX",
         "qow.live.values.regr-avgx-expression",
         "aggregate.global-regr-avgx-expression.v1",
         "a4",
         "019dffbb-f000-7662-a816-d1df50e9b664",
         "regr_avgx_value",
         2.0,
         false},
        {PairStatisticalAggregateKind::kRegrAvgy,
         "REGR_AVGY",
         "qow.live.values.regr-avgy-expression",
         "aggregate.global-regr-avgy-expression.v1",
         "a5",
         "019dffbb-f000-7d03-ac2d-753cdb7744c0",
         "regr_avgy_value",
         4.0,
         false},
        {PairStatisticalAggregateKind::kRegrIntercept,
         "REGR_INTERCEPT",
         "qow.live.values.regr-intercept-expression",
         "aggregate.global-regr-intercept-expression.v1",
         "a6",
         "019dffbb-f000-7c7c-b576-d67ea9d4bcbb",
         "regr_intercept_value",
         0.0,
         false},
        {PairStatisticalAggregateKind::kRegrR2,
         "REGR_R2",
         "qow.live.values.regr-r2-expression",
         "aggregate.global-regr-r2-expression.v1",
         "a7",
         "019dffbb-f000-7a43-9a28-a119b31d9c20",
         "regr_r2_value",
         1.0,
         false},
        {PairStatisticalAggregateKind::kRegrSlope,
         "REGR_SLOPE",
         "qow.live.values.regr-slope-expression",
         "aggregate.global-regr-slope-expression.v1",
         "a8",
         "019dffbb-f000-7f80-b81a-5240a6dbab55",
         "regr_slope_value",
         2.0,
         false},
        {PairStatisticalAggregateKind::kRegrSxx,
         "REGR_SXX",
         "qow.live.values.regr-sxx-expression",
         "aggregate.global-regr-sxx-expression.v1",
         "a9",
         "019dffbb-f000-735e-9e55-5f9243786403",
         "regr_sxx_value",
         2.0,
         false},
        {PairStatisticalAggregateKind::kRegrSxy,
         "REGR_SXY",
         "qow.live.values.regr-sxy-expression",
         "aggregate.global-regr-sxy-expression.v1",
         "aa",
         "019dffbb-f000-788b-a249-866547a43ebe",
         "regr_sxy_value",
         4.0,
         false},
        {PairStatisticalAggregateKind::kRegrSyy,
         "REGR_SYY",
         "qow.live.values.regr-syy-expression",
         "aggregate.global-regr-syy-expression.v1",
         "ab",
         "019dffbb-f000-74f7-98ba-c24ead6d30df",
         "regr_syy_value",
         8.0,
         false},
};

std::string PairProfileUuid(const std::string_view group,
                            const std::string_view family,
                            const std::string_view ordinal) {
  return "019f0000-0000-" + std::string(group) + "-8000-00000000" +
         std::string(family) + std::string(ordinal);
}

sblr::SblrOperationEnvelope
GlobalPairStatisticalAggregateExpressionValuesEnvelope(
    const PairStatisticalAggregateProfile& profile) {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", std::string(profile.operation));
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  const auto tree_uuid = PairProfileUuid("7000", profile.uuid_family, "00");
  const auto y_descriptor =
      PairProfileUuid("7300", profile.uuid_family, "01");
  const auto y_type = PairProfileUuid("7400", profile.uuid_family, "02");
  const auto x_descriptor =
      PairProfileUuid("7300", profile.uuid_family, "03");
  const auto x_type = PairProfileUuid("7400", profile.uuid_family, "04");
  const auto result_descriptor =
      PairProfileUuid("7300", profile.uuid_family, "05");
  const auto result_type =
      PairProfileUuid("7400", profile.uuid_family, "06");
  const auto y_bound_name =
      PairProfileUuid("7500", profile.uuid_family, "07");
  const auto x_bound_name =
      PairProfileUuid("7500", profile.uuid_family, "08");
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid", tree_uuid},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       y_descriptor + "|" + y_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       x_descriptor + "|" + x_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       result_descriptor + "|" + result_type +
           (profile.count_result ? "|1|-|-|-|-|-" : "|2|-|-|-|-|-")},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|32"},
      {"relational_expression_v1", "2", "1|-|2|-|-|1|-|31"},
      {"relational_expression_v1", "3", "1|-|1|-|-|1|-|34"},
      {"relational_expression_v1", "4", "1|-|2|-|-|1|-|32"},
      {"relational_expression_v1", "5", "1|-|1|-|-|1|-|36"},
      {"relational_expression_v1", "6", "1|-|2|-|-|1|-|33"},
      {"relational_expression_v1", "7", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "8", "1|-|2|-|-|1|-|34"},
      {"relational_expression_v1", "9",
       "3|-|1|-|" + y_bound_name + "|-|-|-"},
      {"relational_expression_v1", "10",
       "3|-|2|-|" + x_bound_name + "|-|-|-"},
      {"relational_expression_v1", "11",
       "4|9,10|3|" + std::string(profile.function_uuid) + "|-|-|-|-"},
      {"relational_output_v1", "1", "1|1|1|1|0|79"},
      {"relational_output_v1", "2", "1|2|2|1|1|78"},
      {"relational_output_v1", "3",
       "2|11|3|1|0|" + EncodeHex(profile.output_name)},
      {"relational_values_row_v1", "1", "1,2"},
      {"relational_values_row_v1", "2", "3,4"},
      {"relational_values_row_v1", "3", "5,6"},
      {"relational_values_row_v1", "4", "7,8"},
      {"relational_node_v1", "1", "13|0|-|1,2|1,2,3,4"},
      {"relational_node_v1", "2", "5|0|1|3|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5,6,7,8|-|-|-"},
      {"relational_node_binding_v1", "2",
       EncodeHex(profile.semantic_variant) + "|11|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

// RCP-049-TEST-NODE-DRIVEN-PAIR-STATISTICAL-COMPOSITION-V1
sblr::SblrOperationEnvelope NodeDrivenPairStatisticalLimitEnvelope(
    const PairStatisticalAggregateProfile& profile,
    const std::string_view fixture_family) {
  auto envelope =
      GlobalPairStatisticalAggregateExpressionValuesEnvelope(profile);
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = "019f0000-0000-7000-8000-00000000" +
                      std::string(fixture_family) + "0";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "3";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "4",
       "019f0000-0000-7300-8000-00000000" +
           std::string(fixture_family) + "1|" +
           PairProfileUuid("7400", profile.uuid_family, "06") +
           "|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "12", "1|-|4|-|-|1|-|31"});
  envelope.operands.push_back(
      {"relational_node_v1", "3", "7|0|2|3|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "3",
       "6c696d69742e626f756e642d636f756e742e7631|12|-|-|-"});
  return envelope;
}

sblr::SblrOperationEnvelope
GlobalPairStatisticalAggregateModifierValuesEnvelope(
    const PairStatisticalAggregateProfile& profile,
    const AggregateModifierProfile modifier) {
  const bool has_filter = modifier != AggregateModifierProfile::kDistinct;
  const std::string modifier_digit =
      modifier == AggregateModifierProfile::kFilter
          ? "1"
          : modifier == AggregateModifierProfile::kDistinct ? "2" : "3";
  const std::string modifier_suffix =
      modifier == AggregateModifierProfile::kFilter
          ? "-filter"
          : modifier == AggregateModifierProfile::kDistinct
                ? "-distinct"
                : "-distinct-filter";
  constexpr std::string_view kExpressionSuffix = "-expression.v1";
  const std::string semantic_base = std::string(profile.semantic_variant).substr(
      0, profile.semantic_variant.size() - kExpressionSuffix.size());
  const std::string semantic_variant =
      semantic_base + modifier_suffix + std::string(kExpressionSuffix);
  const auto fixture_uuid = [&](const std::string_view group_prefix,
                                const std::string_view ordinal) {
    return PairProfileUuid(std::string(group_prefix) + modifier_digit,
                           profile.uuid_family, ordinal);
  };
  const auto tree_uuid = fixture_uuid("710", "00");
  const auto y_descriptor = fixture_uuid("730", "01");
  const auto y_type = fixture_uuid("740", "02");
  const auto x_descriptor = fixture_uuid("730", "03");
  const auto x_type = fixture_uuid("740", "04");
  const auto filter_descriptor = fixture_uuid("730", "05");
  const auto filter_type = fixture_uuid("740", "06");
  const auto result_descriptor = fixture_uuid("730", "07");
  const auto result_type = fixture_uuid("740", "08");
  const auto y_bound_name = fixture_uuid("750", "09");
  const auto x_bound_name = fixture_uuid("750", "0a");
  const auto filter_bound_name = fixture_uuid("750", "0b");
  const std::string function_children =
      has_filter ? "22,23,24" : "22,23";

  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      std::string(profile.operation) + ".modifier");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid", tree_uuid},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       y_descriptor + "|" + y_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       x_descriptor + "|" + x_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       filter_descriptor + "|" + filter_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "4",
       result_descriptor + "|" + result_type +
           (profile.count_result ? "|1|-|-|-|-|-" : "|2|-|-|-|-|-")},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|32"},
      {"relational_expression_v1", "2", "1|-|2|-|-|1|-|31"},
      {"relational_expression_v1", "3", "1|-|3|-|-|6|-|74727565"},
      {"relational_expression_v1", "4", "1|-|1|-|-|1|-|34"},
      {"relational_expression_v1", "5", "1|-|2|-|-|1|-|32"},
      {"relational_expression_v1", "6", "1|-|3|-|-|6|-|66616c7365"},
      {"relational_expression_v1", "7", "1|-|1|-|-|1|-|34"},
      {"relational_expression_v1", "8", "1|-|2|-|-|1|-|32"},
      {"relational_expression_v1", "9", "1|-|3|-|-|6|-|74727565"},
      {"relational_expression_v1", "10", "1|-|1|-|-|1|-|38"},
      {"relational_expression_v1", "11", "1|-|2|-|-|1|-|33"},
      {"relational_expression_v1", "12", "1|-|3|-|-|7|-|2d"},
      {"relational_expression_v1", "13", "1|-|1|-|-|1|-|3130"},
      {"relational_expression_v1", "14", "1|-|2|-|-|1|-|34"},
      {"relational_expression_v1", "15", "1|-|3|-|-|6|-|74727565"},
      {"relational_expression_v1", "16", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "17", "1|-|2|-|-|1|-|35"},
      {"relational_expression_v1", "18", "1|-|3|-|-|6|-|74727565"},
      {"relational_expression_v1", "19", "1|-|1|-|-|1|-|3130"},
      {"relational_expression_v1", "20", "1|-|2|-|-|1|-|34"},
      {"relational_expression_v1", "21", "1|-|3|-|-|6|-|74727565"},
      {"relational_expression_v1", "22",
       "3|-|1|-|" + y_bound_name + "|-|-|-"},
      {"relational_expression_v1", "23",
       "3|-|2|-|" + x_bound_name + "|-|-|-"},
      {"relational_expression_v1", "24",
       "3|-|3|-|" + filter_bound_name + "|-|-|-"},
      {"relational_expression_v1", "25",
       "4|" + function_children + "|4|" +
           std::string(profile.function_uuid) + "|-|-|-|-"},
      {"relational_output_v1", "1", "1|1|1|1|0|79"},
      {"relational_output_v1", "2", "1|2|2|1|1|78"},
      {"relational_output_v1", "3", "1|3|3|1|2|73656c6563746564"},
      {"relational_output_v1", "4",
       "2|25|4|1|0|" + EncodeHex(profile.output_name)},
      {"relational_values_row_v1", "1", "1,2,3"},
      {"relational_values_row_v1", "2", "4,5,6"},
      {"relational_values_row_v1", "3", "7,8,9"},
      {"relational_values_row_v1", "4", "10,11,12"},
      {"relational_values_row_v1", "5", "13,14,15"},
      {"relational_values_row_v1", "6", "16,17,18"},
      {"relational_values_row_v1", "7", "19,20,21"},
      {"relational_node_v1", "1", "13|0|-|1,2,3|1,2,3,4,5,6,7"},
      {"relational_node_v1", "2", "5|0|1|4|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21|-|-|-"},
      {"relational_node_binding_v1", "2",
       EncodeHex(semantic_variant) + "|25|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

struct OrderedSetAggregateProfile {
  std::string_view name;
  std::string_view operation;
  std::string_view semantic_variant;
  std::string_view uuid_family;
  std::string_view function_uuid;
  std::string_view output_name;
  double expected;
  bool direct_argument;
  bool percentile_fraction;
  bool real_result;
  bool nullable_result;
};

constexpr OrderedSetAggregateProfile kOrderedSetAggregateProfiles[] = {
    {"RANK",
     "qow.live.values.rank-hypothetical-expression",
     "aggregate.global-rank-hypothetical-expression.v1",
     "b0",
     "019dffbb-f000-7336-ab53-fef5316220d7",
     "rank_value",
     4.0,
     true,
     false,
     false,
     false},
    {"DENSE_RANK",
     "qow.live.values.dense-rank-hypothetical-expression",
     "aggregate.global-dense-rank-hypothetical-expression.v1",
     "b1",
     "019dffbb-f000-7bd3-a731-1734581eb8ce",
     "dense_rank_value",
     3.0,
     true,
     false,
     false,
     false},
    {"PERCENT_RANK",
     "qow.live.values.percent-rank-hypothetical-expression",
     "aggregate.global-percent-rank-hypothetical-expression.v1",
     "b2",
     "019dffbb-f000-7817-911f-9f8b2e66ebec",
     "percent_rank_value",
     0.6,
     true,
     false,
     true,
     false},
    {"CUME_DIST",
     "qow.live.values.cume-dist-hypothetical-expression",
     "aggregate.global-cume-dist-hypothetical-expression.v1",
     "b3",
     "019dffbb-f000-7244-89fd-8fa66ae930d5",
     "cume_dist_value",
     2.0 / 3.0,
     true,
     false,
     true,
     false},
    {"MODE",
     "qow.live.values.mode-ordered-expression",
     "aggregate.global-mode-ordered-expression.v1",
     "b4",
     "019dffbb-f000-7150-9be6-bcf97f8facf5",
     "mode_value",
     20.0,
     false,
     false,
     false,
     true},
    {"PERCENTILE_CONT",
     "qow.live.values.percentile-cont-ordered-expression",
     "aggregate.global-percentile-cont-ordered-expression.v1",
     "b5",
     "019dffbb-f000-7cfd-83dd-15435fe55bf5",
     "percentile_cont_value",
     20.0,
     true,
     true,
     true,
     true},
    {"PERCENTILE_DISC",
     "qow.live.values.percentile-disc-ordered-expression",
     "aggregate.global-percentile-disc-ordered-expression.v1",
     "b6",
     "019dffbb-f000-7081-b766-7db818a89c04",
     "percentile_disc_value",
     20.0,
     true,
     true,
     true,
     true},
};

std::string OrderedSetProfileUuid(const std::string_view group,
                                  const std::string_view family,
                                  const std::string_view ordinal) {
  return "019f0000-0000-" + std::string(group) + "-8000-00000000" +
         std::string(family) + std::string(ordinal);
}

sblr::SblrOperationEnvelope GlobalOrderedSetAggregateValuesEnvelope(
    const OrderedSetAggregateProfile& profile) {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", std::string(profile.operation));
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  const auto tree_uuid =
      OrderedSetProfileUuid("7000", profile.uuid_family, "00");
  const auto input_descriptor =
      OrderedSetProfileUuid("7300", profile.uuid_family, "01");
  const auto input_type =
      OrderedSetProfileUuid("7400", profile.uuid_family, "02");
  const auto direct_descriptor =
      OrderedSetProfileUuid("7300", profile.uuid_family, "03");
  const auto direct_type =
      OrderedSetProfileUuid("7400", profile.uuid_family, "04");
  const auto result_descriptor =
      OrderedSetProfileUuid("7300", profile.uuid_family, "05");
  const auto result_type =
      OrderedSetProfileUuid("7400", profile.uuid_family, "06");
  const auto bound_name =
      OrderedSetProfileUuid("7500", profile.uuid_family, "07");
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid", tree_uuid},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       input_descriptor + "|" + input_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       result_descriptor + "|" + result_type +
           (profile.nullable_result ? "|2|-|-|-|-|-" : "|1|-|-|-|-|-")},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|3130"},
      {"relational_expression_v1", "2", "1|-|1|-|-|1|-|3230"},
      {"relational_expression_v1", "3", "1|-|1|-|-|1|-|3230"},
      {"relational_expression_v1", "4", "1|-|1|-|-|1|-|3330"},
      {"relational_expression_v1", "5", "1|-|1|-|-|1|-|3430"},
      {"relational_expression_v1", "6",
       "3|-|1|-|" + bound_name + "|-|-|-"},
      {"relational_output_v1", "1", "1|1|1|1|0|76616c7565"},
      {"relational_values_row_v1", "1", "1"},
      {"relational_values_row_v1", "2", "2"},
      {"relational_values_row_v1", "3", "3"},
      {"relational_values_row_v1", "4", "4"},
      {"relational_values_row_v1", "5", "5"},
      {"relational_node_v1", "1", "13|0|-|1|1,2,3,4,5"},
      {"relational_node_v1", "2", "5|0|1|3|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5|-|-|-"},
  };
  if (profile.direct_argument) {
    envelope.operands.push_back(
        {"relational_descriptor_v1", "2",
         direct_descriptor + "|" + direct_type + "|1|-|-|-|-|-"});
    envelope.operands.push_back(
        {"relational_expression_v1", "7",
         profile.percentile_fraction ? "1|-|2|-|-|1|-|302e3235"
                                     : "1|-|2|-|-|1|-|3235"});
  }
  envelope.operands.push_back(
      {"relational_expression_v1", "8",
       "4|" + std::string(profile.direct_argument ? "7,6" : "6") +
           "|3|" + std::string(profile.function_uuid) + "|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_output_v1", "2",
       "2|8|3|1|0|" + EncodeHex(profile.output_name)});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "2",
       EncodeHex(profile.semantic_variant) + "|8|-|-|-"});
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope GlobalOrderedSetAggregateModifierValuesEnvelope(
    const OrderedSetAggregateProfile& profile,
    const AggregateModifierProfile modifier) {
  const bool has_filter = modifier != AggregateModifierProfile::kDistinct;
  const std::string modifier_digit =
      modifier == AggregateModifierProfile::kFilter
          ? "1"
          : modifier == AggregateModifierProfile::kDistinct ? "2" : "3";
  const std::string modifier_suffix =
      modifier == AggregateModifierProfile::kFilter
          ? "-filter"
          : modifier == AggregateModifierProfile::kDistinct
                ? "-distinct"
                : "-distinct-filter";
  const auto fixture_uuid = [&](const std::string_view ordinal) {
    return OrderedSetProfileUuid("91" + modifier_digit + "0",
                                 profile.uuid_family, ordinal);
  };
  constexpr std::string_view kExpressionSuffix = "-expression.v1";
  const std::string semantic_base =
      std::string(profile.semantic_variant).substr(
          0, profile.semantic_variant.size() - kExpressionSuffix.size());
  const std::string semantic_variant =
      semantic_base + modifier_suffix + std::string(kExpressionSuffix);
  const auto tree_uuid = fixture_uuid("00");
  const auto input_descriptor = fixture_uuid("01");
  const auto input_type = fixture_uuid("02");
  const auto direct_descriptor = fixture_uuid("03");
  const auto direct_type = fixture_uuid("04");
  const auto filter_descriptor = fixture_uuid("05");
  const auto filter_type = fixture_uuid("06");
  const auto result_descriptor = fixture_uuid("07");
  const auto result_type = fixture_uuid("08");
  const auto input_bound_name = fixture_uuid("09");
  const auto filter_bound_name = fixture_uuid("0a");

  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      std::string(profile.operation) + modifier_suffix);
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid", tree_uuid},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       input_descriptor + "|" + input_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       filter_descriptor + "|" + filter_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "4",
       result_descriptor + "|" + result_type +
           (profile.nullable_result ? "|2|-|-|-|-|-" : "|1|-|-|-|-|-")},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|3130"},
      {"relational_expression_v1", "2", "1|-|3|-|-|6|-|74727565"},
      {"relational_expression_v1", "3", "1|-|1|-|-|1|-|3135"},
      {"relational_expression_v1", "4", "1|-|3|-|-|6|-|66616c7365"},
      {"relational_expression_v1", "5", "1|-|1|-|-|1|-|3230"},
      {"relational_expression_v1", "6", "1|-|3|-|-|6|-|74727565"},
      {"relational_expression_v1", "7", "1|-|1|-|-|1|-|3230"},
      {"relational_expression_v1", "8", "1|-|3|-|-|6|-|74727565"},
      {"relational_expression_v1", "9", "1|-|1|-|-|1|-|3330"},
      {"relational_expression_v1", "10", "1|-|3|-|-|7|-|2d"},
      {"relational_expression_v1", "11", "1|-|1|-|-|1|-|3430"},
      {"relational_expression_v1", "12", "1|-|3|-|-|6|-|74727565"},
      {"relational_expression_v1", "13", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "14", "1|-|3|-|-|6|-|74727565"},
      {"relational_expression_v1", "15",
       "3|-|1|-|" + input_bound_name + "|-|-|-"},
      {"relational_expression_v1", "17",
       "3|-|3|-|" + filter_bound_name + "|-|-|-"},
      {"relational_output_v1", "1", "1|1|1|1|0|76616c7565"},
      {"relational_output_v1", "2", "1|2|3|1|1|73656c6563746564"},
      {"relational_values_row_v1", "1", "1,2"},
      {"relational_values_row_v1", "2", "3,4"},
      {"relational_values_row_v1", "3", "5,6"},
      {"relational_values_row_v1", "4", "7,8"},
      {"relational_values_row_v1", "5", "9,10"},
      {"relational_values_row_v1", "6", "11,12"},
      {"relational_values_row_v1", "7", "13,14"},
      {"relational_node_v1", "1", "13|0|-|1,3|1,2,3,4,5,6,7"},
      {"relational_node_v1", "2", "5|0|1|4|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5,6,7,8,9,10,11,12,13,14|-|-|-"},
  };
  std::string function_children = "15";
  if (profile.direct_argument) {
    envelope.operands.push_back(
        {"relational_descriptor_v1", "2",
         direct_descriptor + "|" + direct_type + "|1|-|-|-|-|-"});
    envelope.operands.push_back(
        {"relational_expression_v1", "16",
         profile.percentile_fraction ? "1|-|2|-|-|1|-|302e3235"
                                     : "1|-|2|-|-|1|-|3235"});
    function_children = "16,15";
  }
  if (has_filter) function_children += ",17";
  envelope.operands.push_back(
      {"relational_expression_v1", "18",
       "4|" + function_children + "|4|" + std::string(profile.function_uuid) +
           "|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_output_v1", "3",
       "2|18|4|1|0|" + EncodeHex(profile.output_name)});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "2",
       EncodeHex(semantic_variant) + "|18|-|-|-"});
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

struct ApproximateAggregateProfile {
  std::string_view name;
  std::string_view operation;
  std::string_view semantic_variant;
  std::string_view uuid_family;
  std::string_view function_uuid;
  std::string_view output_name;
  std::string_view result_type;
  std::string_view expected_encoded;
  std::string_view direct_literal_hex;
  std::string_view invalid_direct_literal_hex;
  bool text_input;
  bool nullable_result;
};

constexpr ApproximateAggregateProfile kApproximateAggregateProfiles[] = {
    {"APPROX_COUNT_DISTINCT",
     "qow.live.values.approx-count-distinct-expression",
     "aggregate.global-approx-count-distinct-expression.v1",
     "c0",
     "019dffbb-f000-7736-96f3-e20cbd532ba5",
     "approx_count_distinct_value",
     "int64",
     "3",
     "",
     "",
     true,
     false},
    {"APPROX_MEDIAN",
     "qow.live.values.approx-median-expression",
     "aggregate.global-approx-median-expression.v1",
     "c1",
     "019dffbb-f000-7ce0-85a6-cbcd71f2c86e",
     "approx_median_value",
     "real64",
     "35",
     "",
     "",
     false,
     true},
    {"APPROX_PERCENTILE_CONT",
     "qow.live.values.approx-percentile-cont-ordered-expression",
     "aggregate.global-approx-percentile-cont-ordered-expression.v1",
     "c2",
     "019dffbb-f000-76df-98a6-aa77d1a342f8",
     "approx_percentile_cont_value",
     "real64",
     "47.5",
     "302e3735",
     "312e35",
     false,
     true},
    {"APPROX_PERCENTILE_DISC",
     "qow.live.values.approx-percentile-disc-ordered-expression",
     "aggregate.global-approx-percentile-disc-ordered-expression.v1",
     "c3",
     "019dffbb-f000-7578-a88f-8db4bb649755",
     "approx_percentile_disc_value",
     "real64",
     "50",
     "302e3735",
     "312e35",
     false,
     true},
    {"APPROX_TOP_K",
     "qow.live.values.approx-top-k-expression",
     "aggregate.global-approx-top-k-expression.v1",
     "c4",
     "019dffbb-f000-7f47-8fe1-0c5e0ec87bf0",
     "approx_top_k_value",
     "json",
     "[{\"value\":\"b\",\"count\":3},{\"value\":\"a\",\"count\":2}]",
     "32",
     "30",
     true,
     true},
};

std::string ApproximateProfileUuid(const std::string_view group,
                                   const std::string_view family,
                                   const std::string_view ordinal) {
  return "019f0000-0000-" + std::string(group) + "-8000-00000000" +
         std::string(family) + std::string(ordinal);
}

sblr::SblrOperationEnvelope GlobalApproximateAggregateValuesEnvelope(
    const ApproximateAggregateProfile& profile) {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", std::string(profile.operation));
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  const auto tree_uuid =
      ApproximateProfileUuid("7000", profile.uuid_family, "00");
  const auto input_descriptor =
      ApproximateProfileUuid("7300", profile.uuid_family, "01");
  const auto input_type =
      ApproximateProfileUuid("7400", profile.uuid_family, "02");
  const auto direct_descriptor =
      ApproximateProfileUuid("7300", profile.uuid_family, "03");
  const auto direct_type =
      ApproximateProfileUuid("7400", profile.uuid_family, "04");
  const auto result_descriptor =
      ApproximateProfileUuid("7300", profile.uuid_family, "05");
  const auto result_type =
      ApproximateProfileUuid("7400", profile.uuid_family, "06");
  const auto bound_name =
      ApproximateProfileUuid("7500", profile.uuid_family, "07");
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid", tree_uuid},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       input_descriptor + "|" + input_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       result_descriptor + "|" + result_type +
           (profile.nullable_result ? "|2|-|-|-|-|-" : "|1|-|-|-|-|-")},
  };
  if (!profile.direct_literal_hex.empty()) {
    envelope.operands.push_back(
        {"relational_descriptor_v1", "2",
         direct_descriptor + "|" + direct_type + "|1|-|-|-|-|-"});
  }
  constexpr std::string_view kNumericValues[] = {
      "3130", "3230", "3330", "3430", "3530", "3630"};
  constexpr std::string_view kTextValues[] = {
      "62", "61", "62", "63", "61", "62"};
  for (std::size_t index = 0; index < std::size(kNumericValues); ++index) {
    const auto literal_kind = profile.text_input ? "2" : "1";
    const auto literal =
        profile.text_input ? kTextValues[index] : kNumericValues[index];
    envelope.operands.push_back(
        {"relational_expression_v1", std::to_string(index + 1),
         "1|-|1|-|-|" + std::string(literal_kind) + "|-|" +
             std::string(literal)});
  }
  envelope.operands.push_back(
      {"relational_expression_v1", "7",
       "3|-|1|-|" + bound_name + "|-|-|-"});
  if (!profile.direct_literal_hex.empty()) {
    envelope.operands.push_back(
        {"relational_expression_v1", "8",
         "1|-|2|-|-|1|-|" + std::string(profile.direct_literal_hex)});
  }
  envelope.operands.push_back(
      {"relational_expression_v1", "9",
       "4|" + std::string(profile.direct_literal_hex.empty() ? "7" : "8,7") +
           "|3|" + std::string(profile.function_uuid) + "|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_output_v1", "1", "1|1|1|1|0|76616c7565"});
  envelope.operands.push_back(
      {"relational_output_v1", "2",
       "2|9|3|1|0|" + EncodeHex(profile.output_name)});
  for (std::size_t index = 0; index < std::size(kNumericValues); ++index) {
    envelope.operands.push_back(
        {"relational_values_row_v1", std::to_string(index + 1),
         std::to_string(index + 1)});
  }
  envelope.operands.push_back(
      {"relational_node_v1", "1", "13|0|-|1|1,2,3,4,5,6"});
  envelope.operands.push_back(
      {"relational_node_v1", "2", "5|0|1|3|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5,6|-|-|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "2",
       EncodeHex(profile.semantic_variant) + "|9|-|-|-"});
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope GlobalApproximateAggregateModifierValuesEnvelope(
    const ApproximateAggregateProfile& profile,
    const AggregateModifierProfile modifier) {
  const bool has_filter = modifier != AggregateModifierProfile::kDistinct;
  const std::string modifier_digit =
      modifier == AggregateModifierProfile::kFilter
          ? "1"
          : modifier == AggregateModifierProfile::kDistinct ? "2" : "3";
  const std::string modifier_suffix =
      modifier == AggregateModifierProfile::kFilter
          ? "-filter"
          : modifier == AggregateModifierProfile::kDistinct
                ? "-distinct"
                : "-distinct-filter";
  const auto fixture_uuid = [&](const std::string_view ordinal) {
    return ApproximateProfileUuid("92" + modifier_digit + "0",
                                  profile.uuid_family, ordinal);
  };
  constexpr std::string_view kExpressionSuffix = "-expression.v1";
  const std::string semantic_base =
      std::string(profile.semantic_variant).substr(
          0, profile.semantic_variant.size() - kExpressionSuffix.size());
  const std::string semantic_variant =
      semantic_base + modifier_suffix + std::string(kExpressionSuffix);
  const auto tree_uuid = fixture_uuid("00");
  const auto input_descriptor = fixture_uuid("01");
  const auto input_type = fixture_uuid("02");
  const auto direct_descriptor = fixture_uuid("03");
  const auto direct_type = fixture_uuid("04");
  const auto filter_descriptor = fixture_uuid("05");
  const auto filter_type = fixture_uuid("06");
  const auto result_descriptor = fixture_uuid("07");
  const auto result_type = fixture_uuid("08");
  const auto input_bound_name = fixture_uuid("09");
  const auto filter_bound_name = fixture_uuid("0a");

  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      std::string(profile.operation) + modifier_suffix);
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid", tree_uuid},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       input_descriptor + "|" + input_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       filter_descriptor + "|" + filter_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "4",
       result_descriptor + "|" + result_type +
           (profile.nullable_result ? "|2|-|-|-|-|-" : "|1|-|-|-|-|-")},
  };
  if (!profile.direct_literal_hex.empty()) {
    envelope.operands.push_back(
        {"relational_descriptor_v1", "2",
         direct_descriptor + "|" + direct_type + "|1|-|-|-|-|-"});
  }
  constexpr std::string_view kNumericValues[] = {
      "3130", "3130", "3130", "3230", "3330", "3430", "2d"};
  constexpr std::string_view kTextValues[] = {
      "62", "61", "62", "62", "63", "61", "2d"};
  constexpr std::string_view kFilterValues[] = {
      "6|-|74727565", "6|-|66616c7365", "6|-|74727565",
      "6|-|74727565", "7|-|2d",       "6|-|74727565",
      "6|-|74727565"};
  for (std::size_t index = 0; index < std::size(kNumericValues); ++index) {
    const bool is_null = index + 1 == std::size(kNumericValues);
    const auto value_expression_id = index * 2 + 1;
    const auto filter_expression_id = value_expression_id + 1;
    const auto literal_kind = is_null ? "7" : profile.text_input ? "2" : "1";
    const auto literal = profile.text_input ? kTextValues[index]
                                            : kNumericValues[index];
    envelope.operands.push_back(
        {"relational_expression_v1", std::to_string(value_expression_id),
         "1|-|1|-|-|" + std::string(literal_kind) + "|-|" +
             std::string(literal)});
    envelope.operands.push_back(
        {"relational_expression_v1", std::to_string(filter_expression_id),
         "1|-|3|-|-|" + std::string(kFilterValues[index])});
    envelope.operands.push_back(
        {"relational_values_row_v1", std::to_string(index + 1),
         std::to_string(value_expression_id) + "," +
             std::to_string(filter_expression_id)});
  }
  envelope.operands.push_back(
      {"relational_expression_v1", "15",
       "3|-|1|-|" + input_bound_name + "|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "17",
       "3|-|3|-|" + filter_bound_name + "|-|-|-"});
  std::string function_children = "15";
  if (!profile.direct_literal_hex.empty()) {
    envelope.operands.push_back(
        {"relational_expression_v1", "16",
         "1|-|2|-|-|1|-|" + std::string(profile.direct_literal_hex)});
    function_children = "16,15";
  }
  if (has_filter) function_children += ",17";
  envelope.operands.push_back(
      {"relational_expression_v1", "18",
       "4|" + function_children + "|4|" + std::string(profile.function_uuid) +
           "|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_output_v1", "1", "1|1|1|1|0|76616c7565"});
  envelope.operands.push_back(
      {"relational_output_v1", "2", "1|2|3|1|1|73656c6563746564"});
  envelope.operands.push_back(
      {"relational_output_v1", "3",
       "2|18|4|1|0|" + EncodeHex(profile.output_name)});
  envelope.operands.push_back(
      {"relational_node_v1", "1", "13|0|-|1,3|1,2,3,4,5,6,7"});
  envelope.operands.push_back(
      {"relational_node_v1", "2", "5|0|1|4|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5,6,7,8,9,10,11,12,13,14|-|-|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "2",
       EncodeHex(semantic_variant) + "|18|-|-|-"});
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope GlobalStringAggExpressionValuesEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      "qow.live.values.string-agg-expression");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7c00-8000-000000000100"},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7c01-8000-000000000101|"
       "019f0000-0000-7c02-8000-000000000102|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7c01-8000-000000000103|"
       "019f0000-0000-7c02-8000-000000000104|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       "019f0000-0000-7c01-8000-000000000105|"
       "019f0000-0000-7c02-8000-000000000106|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|2|-|62"},
      {"relational_expression_v1", "2", "1|-|1|-|-|2|-|64"},
      {"relational_expression_v1", "3", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "4", "1|-|1|-|-|2|-|61"},
      {"relational_expression_v1", "5",
       "3|-|1|-|019f0000-0000-7c03-8000-000000000107|-|-|-"},
      {"relational_expression_v1", "6", "1|-|2|-|-|2|-|7c"},
      {"relational_expression_v1", "7",
       "4|5,6|3|019de5fc-2400-7243-abc6-4f6a777dff00|-|-|-|-"},
      {"relational_output_v1", "1", "1|1|1|1|0|76616c7565"},
      {"relational_output_v1", "2",
       "2|7|3|1|0|737472696e675f6167675f76616c7565"},
      {"relational_values_row_v1", "1", "1"},
      {"relational_values_row_v1", "2", "2"},
      {"relational_values_row_v1", "3", "3"},
      {"relational_values_row_v1", "4", "4"},
      {"relational_node_v1", "1", "13|0|-|1|1,2,3,4"},
      {"relational_node_v1", "2", "5|0|1|3|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|1,2,3,4|-|-|-"},
      {"relational_node_binding_v1", "2",
       "6167677265676174652e676c6f62616c2d737472696e672d6167672d657870"
       "72657373696f6e2e7631|7|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope OrderedStringAggExpressionValuesEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      "qow.live.values.string-agg-ordered-expression");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  const auto tree_uuid = PairProfileUuid("7f00", "b3", "00");
  const auto value_descriptor = PairProfileUuid("7f10", "b3", "01");
  const auto value_type = PairProfileUuid("7f20", "b3", "02");
  const auto separator_descriptor = PairProfileUuid("7f10", "b3", "03");
  const auto separator_type = PairProfileUuid("7f20", "b3", "04");
  const auto order_descriptor = PairProfileUuid("7f10", "b3", "05");
  const auto order_type = PairProfileUuid("7f20", "b3", "06");
  const auto result_descriptor = PairProfileUuid("7f10", "b3", "07");
  const auto result_type = PairProfileUuid("7f20", "b3", "08");
  const auto value_bound_name = PairProfileUuid("7f30", "b3", "09");
  const auto order_bound_name = PairProfileUuid("7f30", "b3", "0a");
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid", tree_uuid},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       value_descriptor + "|" + value_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       separator_descriptor + "|" + separator_type + "|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       order_descriptor + "|" + order_type + "|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "4",
       result_descriptor + "|" + result_type + "|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|2|-|62"},
      {"relational_expression_v1", "2", "1|-|3|-|-|1|-|32"},
      {"relational_expression_v1", "3", "1|-|1|-|-|2|-|64"},
      {"relational_expression_v1", "4", "1|-|3|-|-|1|-|34"},
      {"relational_expression_v1", "5", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "6", "1|-|3|-|-|1|-|33"},
      {"relational_expression_v1", "7", "1|-|1|-|-|2|-|61"},
      {"relational_expression_v1", "8", "1|-|3|-|-|1|-|31"},
      {"relational_expression_v1", "9",
       "3|-|1|-|" + value_bound_name + "|-|-|-"},
      {"relational_expression_v1", "10", "1|-|2|-|-|2|-|7c"},
      {"relational_expression_v1", "11",
       "3|-|3|-|" + order_bound_name + "|-|-|-"},
      {"relational_expression_v1", "12",
       "4|9,10,11|4|019de5fc-2400-7243-abc6-4f6a777dff00|-|-|-|-"},
      {"relational_output_v1", "1", "1|1|1|1|0|76616c7565"},
      {"relational_output_v1", "2", "1|2|3|1|1|6f72646572"},
      {"relational_output_v1", "3",
       "2|12|4|1|0|737472696e675f6167675f6f7264657265645f76616c7565"},
      {"relational_values_row_v1", "1", "1,2"},
      {"relational_values_row_v1", "2", "3,4"},
      {"relational_values_row_v1", "3", "5,6"},
      {"relational_values_row_v1", "4", "7,8"},
      {"relational_node_v1", "1", "13|0|-|1,3|1,2,3,4"},
      {"relational_node_v1", "2", "5|0|1|4|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5,6,7,8|-|-|-"},
      {"relational_node_binding_v1", "2",
       "6167677265676174652e676c6f62616c2d737472696e672d6167672d6f7264"
       "657265642d65787072657373696f6e2e7631|12|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

// RCP-049-TEST-NODE-DRIVEN-STRING-AGG-COMPOSITION-V1
sblr::SblrOperationEnvelope NodeDrivenStringAggLimitEnvelope(
    const bool ordered) {
  auto envelope = ordered ? OrderedStringAggExpressionValuesEnvelope()
                          : GlobalStringAggExpressionValuesEnvelope();
  const std::string descriptor_id = ordered ? "5" : "4";
  const std::string expression_id = ordered ? "13" : "8";
  const std::string output_descriptor_id = ordered ? "4" : "3";
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = ordered
                          ? "019f0000-0000-7000-8000-00000000cf10"
                          : "019f0000-0000-7000-8000-00000000cf00";
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "3";
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", descriptor_id,
       (ordered ? "019f0000-0000-7300-8000-00000000cf11|"
                : "019f0000-0000-7300-8000-00000000cf01|") +
           std::string(
               "019f0000-0000-7400-8000-00000000e208|1|-|-|-|-|-")});
  envelope.operands.push_back(
      {"relational_expression_v1", expression_id,
       "1|-|" + descriptor_id + "|-|-|1|-|31"});
  envelope.operands.push_back(
      {"relational_node_v1", "3",
       "7|0|2|" + output_descriptor_id + "|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "3",
       "6c696d69742e626f756e642d636f756e742e7631|" +
           expression_id + "|-|-|-"});
  return envelope;
}

sblr::SblrOperationEnvelope StringAggModifierValuesEnvelope(
    const bool ordered, const AggregateModifierProfile modifier) {
  const bool has_filter = modifier != AggregateModifierProfile::kDistinct;
  const std::string modifier_digit =
      modifier == AggregateModifierProfile::kFilter
          ? "1"
          : modifier == AggregateModifierProfile::kDistinct ? "2" : "3";
  const std::string modifier_suffix =
      modifier == AggregateModifierProfile::kFilter
          ? "-filter"
          : modifier == AggregateModifierProfile::kDistinct
                ? "-distinct"
                : "-distinct-filter";
  const std::string order_suffix = ordered ? "-ordered" : "";
  const auto fixture_uuid = [&](const std::string_view family_prefix,
                                const std::string_view ordinal) {
    return PairProfileUuid(std::string(family_prefix) +
                               (ordered ? "2" : "1") + modifier_digit,
                           "c3", ordinal);
  };
  const auto tree_uuid = fixture_uuid("7b", "00");
  const auto value_descriptor = fixture_uuid("7c", "01");
  const auto value_type = fixture_uuid("7d", "02");
  const auto order_descriptor = fixture_uuid("7c", "03");
  const auto order_type = fixture_uuid("7d", "04");
  const auto filter_descriptor = fixture_uuid("7c", "05");
  const auto filter_type = fixture_uuid("7d", "06");
  const auto separator_descriptor = fixture_uuid("7c", "07");
  const auto separator_type = fixture_uuid("7d", "08");
  const auto result_descriptor = fixture_uuid("7c", "09");
  const auto result_type = fixture_uuid("7d", "0a");
  const auto value_bound_name = fixture_uuid("7e", "0b");
  const auto order_bound_name = fixture_uuid("7e", "0c");
  const auto filter_bound_name = fixture_uuid("7e", "0d");
  const std::string function_children =
      ordered ? (has_filter ? "22,25,23,24" : "22,25,23")
              : (has_filter ? "22,25,24" : "22,25");
  const std::string semantic_variant =
      "aggregate.global-string-agg" + order_suffix + modifier_suffix +
      "-expression.v1";

  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      "qow.live.values.string-agg" + order_suffix + modifier_suffix +
          "-expression");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid", tree_uuid},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       value_descriptor + "|" + value_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       order_descriptor + "|" + order_type + "|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       filter_descriptor + "|" + filter_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "4",
       separator_descriptor + "|" + separator_type + "|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "5",
       result_descriptor + "|" + result_type + "|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|2|-|62657461"},
      {"relational_expression_v1", "2", "1|-|2|-|-|1|-|34"},
      {"relational_expression_v1", "3", "1|-|3|-|-|6|-|74727565"},
      {"relational_expression_v1", "4", "1|-|1|-|-|2|-|616c706861"},
      {"relational_expression_v1", "5", "1|-|2|-|-|1|-|32"},
      {"relational_expression_v1", "6", "1|-|3|-|-|6|-|66616c7365"},
      {"relational_expression_v1", "7", "1|-|1|-|-|2|-|616c706861"},
      {"relational_expression_v1", "8", "1|-|2|-|-|1|-|35"},
      {"relational_expression_v1", "9", "1|-|3|-|-|6|-|74727565"},
      {"relational_expression_v1", "10", "1|-|1|-|-|2|-|67616d6d61"},
      {"relational_expression_v1", "11", "1|-|2|-|-|1|-|31"},
      {"relational_expression_v1", "12", "1|-|3|-|-|7|-|2d"},
      {"relational_expression_v1", "13", "1|-|1|-|-|2|-|64656c7461"},
      {"relational_expression_v1", "14", "1|-|2|-|-|1|-|36"},
      {"relational_expression_v1", "15", "1|-|3|-|-|6|-|74727565"},
      {"relational_expression_v1", "16", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "17", "1|-|2|-|-|1|-|30"},
      {"relational_expression_v1", "18", "1|-|3|-|-|6|-|74727565"},
      {"relational_expression_v1", "19", "1|-|1|-|-|2|-|64656c7461"},
      {"relational_expression_v1", "20", "1|-|2|-|-|1|-|33"},
      {"relational_expression_v1", "21", "1|-|3|-|-|6|-|74727565"},
      {"relational_expression_v1", "22",
       "3|-|1|-|" + value_bound_name + "|-|-|-"},
      {"relational_expression_v1", "23",
       "3|-|2|-|" + order_bound_name + "|-|-|-"},
      {"relational_expression_v1", "24",
       "3|-|3|-|" + filter_bound_name + "|-|-|-"},
      {"relational_expression_v1", "25", "1|-|4|-|-|2|-|7c"},
      {"relational_expression_v1", "26",
       "4|" + function_children +
           "|5|019de5fc-2400-7243-abc6-4f6a777dff00|-|-|-|-"},
      {"relational_output_v1", "1", "1|1|1|1|0|76616c7565"},
      {"relational_output_v1", "2", "1|2|2|1|1|6f72646572"},
      {"relational_output_v1", "3", "1|3|3|1|2|73656c6563746564"},
      {"relational_output_v1", "4",
       "2|26|5|1|0|" +
           EncodeHex(ordered ? "string_agg_ordered_modifier_value"
                             : "string_agg_modifier_value")},
      {"relational_values_row_v1", "1", "1,2,3"},
      {"relational_values_row_v1", "2", "4,5,6"},
      {"relational_values_row_v1", "3", "7,8,9"},
      {"relational_values_row_v1", "4", "10,11,12"},
      {"relational_values_row_v1", "5", "13,14,15"},
      {"relational_values_row_v1", "6", "16,17,18"},
      {"relational_values_row_v1", "7", "19,20,21"},
      {"relational_node_v1", "1", "13|0|-|1,2,3|1,2,3,4,5,6,7"},
      {"relational_node_v1", "2", "5|0|1|5|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21|-|-|-"},
      {"relational_node_binding_v1", "2",
       EncodeHex(semantic_variant) + "|26|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

enum class LiveListaggProfile {
  kOrdered,
  kOverflowError,
  kOverflowTruncateWithCount,
  kOverflowTruncateWithoutCount,
};

sblr::SblrOperationEnvelope OrderedListaggExpressionValuesEnvelope(
    const LiveListaggProfile profile) {
  const bool overflow_error = profile == LiveListaggProfile::kOverflowError;
  const bool overflow_truncate =
      profile == LiveListaggProfile::kOverflowTruncateWithCount ||
      profile == LiveListaggProfile::kOverflowTruncateWithoutCount;
  const bool with_count =
      profile != LiveListaggProfile::kOverflowTruncateWithoutCount;
  const std::string semantic_variant =
      overflow_error
          ? "aggregate.global-listagg-ordered-overflow-error-expression.v1"
      : overflow_truncate
          ? "aggregate.global-listagg-ordered-overflow-truncate-expression.v1"
          : "aggregate.global-listagg-ordered-expression.v1";
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      "qow.live.values.listagg-ordered-expression");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  const auto tree_uuid = PairProfileUuid("7f00", "b4", "00");
  const auto value_descriptor = PairProfileUuid("7f10", "b4", "01");
  const auto value_type = PairProfileUuid("7f20", "b4", "02");
  const auto separator_descriptor = PairProfileUuid("7f10", "b4", "03");
  const auto separator_type = PairProfileUuid("7f20", "b4", "04");
  const auto order_descriptor = PairProfileUuid("7f10", "b4", "05");
  const auto order_type = PairProfileUuid("7f20", "b4", "06");
  const auto maximum_descriptor = PairProfileUuid("7f10", "b4", "07");
  const auto maximum_type = PairProfileUuid("7f20", "b4", "08");
  const auto indicator_descriptor = PairProfileUuid("7f10", "b4", "09");
  const auto indicator_type = PairProfileUuid("7f20", "b4", "0a");
  const auto count_descriptor = PairProfileUuid("7f10", "b4", "0b");
  const auto count_type = PairProfileUuid("7f20", "b4", "0c");
  const auto result_descriptor = PairProfileUuid("7f10", "b4", "0d");
  const auto result_type = PairProfileUuid("7f20", "b4", "0e");
  const auto value_bound_name = PairProfileUuid("7f30", "b4", "0f");
  const auto order_bound_name = PairProfileUuid("7f30", "b4", "10");
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid", tree_uuid},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       value_descriptor + "|" + value_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       separator_descriptor + "|" + separator_type + "|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       order_descriptor + "|" + order_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "7",
       result_descriptor + "|" + result_type + "|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|2|-|6e6f727468"},
      {"relational_expression_v1", "2", "1|-|3|-|-|1|-|33"},
      {"relational_expression_v1", "3", "1|-|1|-|-|2|-|65617374"},
      {"relational_expression_v1", "4", "1|-|3|-|-|1|-|31"},
      {"relational_expression_v1", "5", "1|-|1|-|-|2|-|736f757468"},
      {"relational_expression_v1", "6", "1|-|3|-|-|7|-|2d"},
      {"relational_expression_v1", "7", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "8", "1|-|3|-|-|1|-|32"},
      {"relational_expression_v1", "9",
       "3|-|1|-|" + value_bound_name + "|-|-|-"},
      {"relational_expression_v1", "10", "1|-|2|-|-|2|-|7c"},
      {"relational_expression_v1", "11",
       "3|-|3|-|" + order_bound_name + "|-|-|-"},
  };
  std::string function_children = "9,10,11";
  if (overflow_error || overflow_truncate) {
    envelope.operands.push_back(
        {"relational_descriptor_v1", "4",
         maximum_descriptor + "|" + maximum_type + "|1|-|-|-|-|-"});
    envelope.operands.push_back(
        {"relational_expression_v1", "12", "1|-|4|-|-|1|-|3132"});
    function_children += ",12";
  }
  if (overflow_truncate) {
    envelope.operands.push_back(
        {"relational_descriptor_v1", "5",
         indicator_descriptor + "|" + indicator_type + "|1|-|-|-|-|-"});
    envelope.operands.push_back(
        {"relational_descriptor_v1", "6",
         count_descriptor + "|" + count_type + "|1|-|-|-|-|-"});
    envelope.operands.push_back(
        {"relational_expression_v1", "13", "1|-|5|-|-|2|-|2e2e2e"});
    envelope.operands.push_back(
        {"relational_expression_v1", "14",
         with_count ? "1|-|6|-|-|6|-|74727565"
                    : "1|-|6|-|-|6|-|66616c7365"});
    function_children += ",13,14";
  }
  envelope.operands.push_back(
      {"relational_expression_v1", "15",
       "4|" + function_children +
           "|7|019dffbb-f000-7e93-8e4d-6063849de049|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_output_v1", "1", "1|1|1|1|0|76616c7565"});
  envelope.operands.push_back(
      {"relational_output_v1", "2", "1|2|3|1|1|6f72646572"});
  envelope.operands.push_back(
      {"relational_output_v1", "3",
       "2|15|7|1|0|6c6973746167675f76616c7565"});
  envelope.operands.push_back(
      {"relational_values_row_v1", "1", "1,2"});
  envelope.operands.push_back(
      {"relational_values_row_v1", "2", "3,4"});
  envelope.operands.push_back(
      {"relational_values_row_v1", "3", "5,6"});
  envelope.operands.push_back(
      {"relational_values_row_v1", "4", "7,8"});
  envelope.operands.push_back(
      {"relational_node_v1", "1", "13|0|-|1,3|1,2,3,4"});
  envelope.operands.push_back(
      {"relational_node_v1", "2", "5|0|1|7|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5,6,7,8|-|-|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "2",
       EncodeHex(semantic_variant) + "|15|-|-|-"});
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope OrderedListaggModifierExpressionValuesEnvelope(
    const LiveListaggProfile profile,
    const AggregateModifierProfile modifier) {
  const bool overflow_error = profile == LiveListaggProfile::kOverflowError;
  const bool overflow_truncate =
      profile == LiveListaggProfile::kOverflowTruncateWithCount;
  const bool has_filter = modifier != AggregateModifierProfile::kDistinct;
  const std::string form_digit =
      overflow_error ? "2" : overflow_truncate ? "3" : "1";
  const std::string modifier_digit =
      modifier == AggregateModifierProfile::kFilter
          ? "1"
          : modifier == AggregateModifierProfile::kDistinct ? "2" : "3";
  const std::string modifier_suffix =
      modifier == AggregateModifierProfile::kFilter
          ? "-filter"
          : modifier == AggregateModifierProfile::kDistinct
                ? "-distinct"
                : "-distinct-filter";
  const std::string form_suffix =
      overflow_error ? "-overflow-error"
                     : overflow_truncate ? "-overflow-truncate" : "";
  const std::string semantic_variant =
      "aggregate.global-listagg-ordered" + form_suffix + modifier_suffix +
      "-expression.v1";
  const auto fixture_uuid = [&](const std::string_view ordinal) {
    return PairProfileUuid("90" + form_digit + modifier_digit, "c6", ordinal);
  };
  const auto tree_uuid = fixture_uuid("00");
  const auto value_descriptor = fixture_uuid("01");
  const auto value_type = fixture_uuid("02");
  const auto separator_descriptor = fixture_uuid("03");
  const auto separator_type = fixture_uuid("04");
  const auto order_descriptor = fixture_uuid("05");
  const auto order_type = fixture_uuid("06");
  const auto filter_descriptor = fixture_uuid("07");
  const auto filter_type = fixture_uuid("08");
  const auto maximum_descriptor = fixture_uuid("09");
  const auto maximum_type = fixture_uuid("0a");
  const auto indicator_descriptor = fixture_uuid("0b");
  const auto indicator_type = fixture_uuid("0c");
  const auto count_descriptor = fixture_uuid("0d");
  const auto count_type = fixture_uuid("0e");
  const auto result_descriptor = fixture_uuid("0f");
  const auto result_type = fixture_uuid("10");
  const auto value_bound_name = fixture_uuid("11");
  const auto order_bound_name = fixture_uuid("12");
  const auto filter_bound_name = fixture_uuid("13");

  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      "qow.live.values.listagg-ordered" + form_suffix + modifier_suffix);
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid", tree_uuid},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       value_descriptor + "|" + value_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       separator_descriptor + "|" + separator_type + "|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       order_descriptor + "|" + order_type + "|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "4",
       filter_descriptor + "|" + filter_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "8",
       result_descriptor + "|" + result_type + "|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|2|-|62657461"},
      {"relational_expression_v1", "2", "1|-|3|-|-|1|-|34"},
      {"relational_expression_v1", "3", "1|-|4|-|-|6|-|74727565"},
      {"relational_expression_v1", "4", "1|-|1|-|-|2|-|616c706861"},
      {"relational_expression_v1", "5", "1|-|3|-|-|1|-|32"},
      {"relational_expression_v1", "6", "1|-|4|-|-|6|-|66616c7365"},
      {"relational_expression_v1", "7", "1|-|1|-|-|2|-|616c706861"},
      {"relational_expression_v1", "8", "1|-|3|-|-|1|-|35"},
      {"relational_expression_v1", "9", "1|-|4|-|-|6|-|74727565"},
      {"relational_expression_v1", "10", "1|-|1|-|-|2|-|67616d6d61"},
      {"relational_expression_v1", "11", "1|-|3|-|-|1|-|31"},
      {"relational_expression_v1", "12", "1|-|4|-|-|7|-|2d"},
      {"relational_expression_v1", "13", "1|-|1|-|-|2|-|64656c7461"},
      {"relational_expression_v1", "14", "1|-|3|-|-|1|-|36"},
      {"relational_expression_v1", "15", "1|-|4|-|-|6|-|74727565"},
      {"relational_expression_v1", "16", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "17", "1|-|3|-|-|1|-|30"},
      {"relational_expression_v1", "18", "1|-|4|-|-|6|-|74727565"},
      {"relational_expression_v1", "19", "1|-|1|-|-|2|-|64656c7461"},
      {"relational_expression_v1", "20", "1|-|3|-|-|1|-|33"},
      {"relational_expression_v1", "21", "1|-|4|-|-|6|-|74727565"},
      {"relational_expression_v1", "22",
       "3|-|1|-|" + value_bound_name + "|-|-|-"},
      {"relational_expression_v1", "23", "1|-|2|-|-|2|-|7c"},
      {"relational_expression_v1", "24",
       "3|-|3|-|" + order_bound_name + "|-|-|-"},
      {"relational_expression_v1", "28",
       "3|-|4|-|" + filter_bound_name + "|-|-|-"},
  };
  std::string function_children = "22,23,24";
  if (overflow_error || overflow_truncate) {
    envelope.operands.push_back(
        {"relational_descriptor_v1", "5",
         maximum_descriptor + "|" + maximum_type + "|1|-|-|-|-|-"});
    envelope.operands.push_back(
        {"relational_expression_v1", "25",
         overflow_error ? "1|-|5|-|-|1|-|3634" : "1|-|5|-|-|1|-|3132"});
    function_children += ",25";
  }
  if (overflow_truncate) {
    envelope.operands.push_back(
        {"relational_descriptor_v1", "6",
         indicator_descriptor + "|" + indicator_type + "|1|-|-|-|-|-"});
    envelope.operands.push_back(
        {"relational_descriptor_v1", "7",
         count_descriptor + "|" + count_type + "|1|-|-|-|-|-"});
    envelope.operands.push_back(
        {"relational_expression_v1", "26", "1|-|6|-|-|2|-|2e2e2e"});
    envelope.operands.push_back(
        {"relational_expression_v1", "27", "1|-|7|-|-|6|-|74727565"});
    function_children += ",26,27";
  }
  if (has_filter) function_children += ",28";
  envelope.operands.push_back(
      {"relational_expression_v1", "29",
       "4|" + function_children +
           "|8|019dffbb-f000-7e93-8e4d-6063849de049|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_output_v1", "1", "1|1|1|1|0|76616c7565"});
  envelope.operands.push_back(
      {"relational_output_v1", "2", "1|2|3|1|1|6f72646572"});
  envelope.operands.push_back(
      {"relational_output_v1", "3", "1|3|4|1|2|73656c6563746564"});
  envelope.operands.push_back(
      {"relational_output_v1", "4",
       "2|29|8|1|0|6c6973746167675f6d6f6469666965725f76616c7565"});
  for (std::size_t row = 0; row < 7; ++row) {
    const auto first_expression = row * 3 + 1;
    envelope.operands.push_back(
        {"relational_values_row_v1", std::to_string(row + 1),
         std::to_string(first_expression) + "," +
             std::to_string(first_expression + 1) + "," +
             std::to_string(first_expression + 2)});
  }
  envelope.operands.push_back(
      {"relational_node_v1", "1", "13|0|-|1,3,4|1,2,3,4,5,6,7"});
  envelope.operands.push_back(
      {"relational_node_v1", "2", "5|0|1|8|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21|-|-|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "2",
       EncodeHex(semantic_variant) + "|29|-|-|-"});
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

struct OrderedSingleCollectionProfile {
  std::string_view name;
  std::string_view operation;
  std::string_view semantic_variant;
  std::string_view uuid_family;
  std::string_view function_uuid;
  std::string_view output_name;
  std::string_view result_type;
  std::string_view expected;
};

constexpr OrderedSingleCollectionProfile
    kOrderedSingleCollectionProfiles[] = {
        {"ARRAY_AGG",
         "qow.live.values.array-agg-ordered-expression",
         "aggregate.global-array-agg-ordered-expression.v1",
         "b0",
         "019de5fc-2400-7159-9f7b-915513b8c0d4",
         "array_agg_value",
         "list<text nullable>",
         "list[text:a;text:b;NULL;text:d]"},
        {"JSON_AGG",
         "qow.live.values.json-agg-ordered-expression",
         "aggregate.global-json-agg-ordered-expression.v1",
         "b1",
         "019dffbb-f001-7021-8a00-000000000023",
         "json_agg_value",
         "json",
         R"(["a","b",null,"d"])"},
};

sblr::SblrOperationEnvelope OrderedSingleCollectionValuesEnvelope(
    const OrderedSingleCollectionProfile& profile) {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", std::string(profile.operation));
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  const auto tree_uuid = PairProfileUuid("7d00", profile.uuid_family, "00");
  const auto value_descriptor =
      PairProfileUuid("7d10", profile.uuid_family, "01");
  const auto value_type = PairProfileUuid("7d20", profile.uuid_family, "02");
  const auto order_descriptor =
      PairProfileUuid("7d10", profile.uuid_family, "03");
  const auto order_type = PairProfileUuid("7d20", profile.uuid_family, "04");
  const auto result_descriptor =
      PairProfileUuid("7d10", profile.uuid_family, "05");
  const auto result_type = PairProfileUuid("7d20", profile.uuid_family, "06");
  const auto value_bound_name =
      PairProfileUuid("7d30", profile.uuid_family, "07");
  const auto order_bound_name =
      PairProfileUuid("7d30", profile.uuid_family, "08");
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid", tree_uuid},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       value_descriptor + "|" + value_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       order_descriptor + "|" + order_type + "|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       result_descriptor + "|" + result_type + "|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|2|-|62"},
      {"relational_expression_v1", "2", "1|-|2|-|-|1|-|32"},
      {"relational_expression_v1", "3", "1|-|1|-|-|2|-|64"},
      {"relational_expression_v1", "4", "1|-|2|-|-|1|-|34"},
      {"relational_expression_v1", "5", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "6", "1|-|2|-|-|1|-|33"},
      {"relational_expression_v1", "7", "1|-|1|-|-|2|-|61"},
      {"relational_expression_v1", "8", "1|-|2|-|-|1|-|31"},
      {"relational_expression_v1", "9",
       "3|-|1|-|" + value_bound_name + "|-|-|-"},
      {"relational_expression_v1", "10",
       "3|-|2|-|" + order_bound_name + "|-|-|-"},
      {"relational_expression_v1", "11",
       "4|9,10|3|" + std::string(profile.function_uuid) + "|-|-|-|-"},
      {"relational_output_v1", "1", "1|1|1|1|0|76616c7565"},
      {"relational_output_v1", "2", "1|2|2|1|1|6f72646572"},
      {"relational_output_v1", "3",
       "2|11|3|1|0|" + EncodeHex(profile.output_name)},
      {"relational_values_row_v1", "1", "1,2"},
      {"relational_values_row_v1", "2", "3,4"},
      {"relational_values_row_v1", "3", "5,6"},
      {"relational_values_row_v1", "4", "7,8"},
      {"relational_node_v1", "1", "13|0|-|1,2|1,2,3,4"},
      {"relational_node_v1", "2", "5|0|1|3|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5,6,7,8|-|-|-"},
      {"relational_node_binding_v1", "2",
       EncodeHex(profile.semantic_variant) + "|11|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope OrderedSingleCollectionModifierValuesEnvelope(
    const OrderedSingleCollectionProfile& profile,
    const AggregateModifierProfile modifier) {
  const bool has_filter = modifier != AggregateModifierProfile::kDistinct;
  const bool is_array_agg = profile.name == "ARRAY_AGG";
  const std::string modifier_digit =
      modifier == AggregateModifierProfile::kFilter
          ? "1"
          : modifier == AggregateModifierProfile::kDistinct ? "2" : "3";
  const std::string modifier_suffix =
      modifier == AggregateModifierProfile::kFilter
          ? "-filter"
          : modifier == AggregateModifierProfile::kDistinct
                ? "-distinct"
                : "-distinct-filter";
  const auto fixture_uuid = [&](const std::string_view family_prefix,
                                const std::string_view ordinal) {
    return PairProfileUuid(std::string(family_prefix) +
                               (is_array_agg ? "1" : "2") + modifier_digit,
                           "c4", ordinal);
  };
  const auto tree_uuid = fixture_uuid("8b", "00");
  const auto value_descriptor = fixture_uuid("8c", "01");
  const auto value_type = fixture_uuid("8d", "02");
  const auto order_descriptor = fixture_uuid("8c", "03");
  const auto order_type = fixture_uuid("8d", "04");
  const auto filter_descriptor = fixture_uuid("8c", "05");
  const auto filter_type = fixture_uuid("8d", "06");
  const auto result_descriptor = fixture_uuid("8c", "07");
  const auto result_type = fixture_uuid("8d", "08");
  const auto value_bound_name = fixture_uuid("8e", "09");
  const auto order_bound_name = fixture_uuid("8e", "0a");
  const auto filter_bound_name = fixture_uuid("8e", "0b");
  const std::string function_children =
      has_filter ? "22,23,24" : "22,23";
  constexpr std::string_view kExpressionSuffix = "-expression.v1";
  const std::string semantic_base =
      std::string(profile.semantic_variant).substr(
          0, profile.semantic_variant.size() - kExpressionSuffix.size());
  const std::string semantic_variant =
      semantic_base + modifier_suffix + std::string(kExpressionSuffix);

  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      std::string(profile.operation) + modifier_suffix);
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid", tree_uuid},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       value_descriptor + "|" + value_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       order_descriptor + "|" + order_type + "|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       filter_descriptor + "|" + filter_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "4",
       result_descriptor + "|" + result_type + "|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|2|-|62657461"},
      {"relational_expression_v1", "2", "1|-|2|-|-|1|-|34"},
      {"relational_expression_v1", "3", "1|-|3|-|-|6|-|74727565"},
      {"relational_expression_v1", "4", "1|-|1|-|-|2|-|616c706861"},
      {"relational_expression_v1", "5", "1|-|2|-|-|1|-|32"},
      {"relational_expression_v1", "6", "1|-|3|-|-|6|-|66616c7365"},
      {"relational_expression_v1", "7", "1|-|1|-|-|2|-|616c706861"},
      {"relational_expression_v1", "8", "1|-|2|-|-|1|-|35"},
      {"relational_expression_v1", "9", "1|-|3|-|-|6|-|74727565"},
      {"relational_expression_v1", "10", "1|-|1|-|-|2|-|67616d6d61"},
      {"relational_expression_v1", "11", "1|-|2|-|-|1|-|31"},
      {"relational_expression_v1", "12", "1|-|3|-|-|7|-|2d"},
      {"relational_expression_v1", "13", "1|-|1|-|-|2|-|64656c7461"},
      {"relational_expression_v1", "14", "1|-|2|-|-|1|-|36"},
      {"relational_expression_v1", "15", "1|-|3|-|-|6|-|74727565"},
      {"relational_expression_v1", "16", "1|-|1|-|-|7|-|2d"},
      {"relational_expression_v1", "17", "1|-|2|-|-|1|-|30"},
      {"relational_expression_v1", "18", "1|-|3|-|-|6|-|74727565"},
      {"relational_expression_v1", "19", "1|-|1|-|-|2|-|64656c7461"},
      {"relational_expression_v1", "20", "1|-|2|-|-|1|-|33"},
      {"relational_expression_v1", "21", "1|-|3|-|-|6|-|74727565"},
      {"relational_expression_v1", "22",
       "3|-|1|-|" + value_bound_name + "|-|-|-"},
      {"relational_expression_v1", "23",
       "3|-|2|-|" + order_bound_name + "|-|-|-"},
      {"relational_expression_v1", "24",
       "3|-|3|-|" + filter_bound_name + "|-|-|-"},
      {"relational_expression_v1", "25",
       "4|" + function_children + "|4|" +
           std::string(profile.function_uuid) + "|-|-|-|-"},
      {"relational_output_v1", "1", "1|1|1|1|0|76616c7565"},
      {"relational_output_v1", "2", "1|2|2|1|1|6f72646572"},
      {"relational_output_v1", "3", "1|3|3|1|2|73656c6563746564"},
      {"relational_output_v1", "4",
       "2|25|4|1|0|" + EncodeHex(profile.output_name)},
      {"relational_values_row_v1", "1", "1,2,3"},
      {"relational_values_row_v1", "2", "4,5,6"},
      {"relational_values_row_v1", "3", "7,8,9"},
      {"relational_values_row_v1", "4", "10,11,12"},
      {"relational_values_row_v1", "5", "13,14,15"},
      {"relational_values_row_v1", "6", "16,17,18"},
      {"relational_values_row_v1", "7", "19,20,21"},
      {"relational_node_v1", "1", "13|0|-|1,2,3|1,2,3,4,5,6,7"},
      {"relational_node_v1", "2", "5|0|1|4|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21|-|-|-"},
      {"relational_node_binding_v1", "2",
       EncodeHex(semantic_variant) + "|25|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope OrderedJsonObjectAggValuesEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      "qow.live.values.json-object-agg-ordered-expression");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  const auto tree_uuid = PairProfileUuid("7e00", "b2", "00");
  const auto key_descriptor = PairProfileUuid("7e10", "b2", "01");
  const auto key_type = PairProfileUuid("7e20", "b2", "02");
  const auto value_descriptor = PairProfileUuid("7e10", "b2", "03");
  const auto value_type = PairProfileUuid("7e20", "b2", "04");
  const auto order_descriptor = PairProfileUuid("7e10", "b2", "05");
  const auto order_type = PairProfileUuid("7e20", "b2", "06");
  const auto result_descriptor = PairProfileUuid("7e10", "b2", "07");
  const auto result_type = PairProfileUuid("7e20", "b2", "08");
  const auto key_bound_name = PairProfileUuid("7e30", "b2", "09");
  const auto value_bound_name = PairProfileUuid("7e30", "b2", "0a");
  const auto order_bound_name = PairProfileUuid("7e30", "b2", "0b");
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid", tree_uuid},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       key_descriptor + "|" + key_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       value_descriptor + "|" + value_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       order_descriptor + "|" + order_type + "|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "4",
       result_descriptor + "|" + result_type + "|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|2|-|7461696c"},
      {"relational_expression_v1", "2", "1|-|2|-|-|1|-|34"},
      {"relational_expression_v1", "3", "1|-|3|-|-|1|-|34"},
      {"relational_expression_v1", "4", "1|-|1|-|-|2|-|647570"},
      {"relational_expression_v1", "5", "1|-|2|-|-|7|-|2d"},
      {"relational_expression_v1", "6", "1|-|3|-|-|1|-|33"},
      {"relational_expression_v1", "7", "1|-|1|-|-|2|-|647570"},
      {"relational_expression_v1", "8", "1|-|2|-|-|1|-|31"},
      {"relational_expression_v1", "9", "1|-|3|-|-|1|-|31"},
      {"relational_expression_v1", "10", "1|-|1|-|-|2|-|6f74686572"},
      {"relational_expression_v1", "11", "1|-|2|-|-|1|-|32"},
      {"relational_expression_v1", "12", "1|-|3|-|-|1|-|32"},
      {"relational_expression_v1", "13",
       "3|-|1|-|" + key_bound_name + "|-|-|-"},
      {"relational_expression_v1", "14",
       "3|-|2|-|" + value_bound_name + "|-|-|-"},
      {"relational_expression_v1", "15",
       "3|-|3|-|" + order_bound_name + "|-|-|-"},
      {"relational_expression_v1", "16",
       "4|13,14,15|4|019dffbb-f001-7021-8a00-000000000024|-|-|-|-"},
      {"relational_output_v1", "1", "1|1|1|1|0|6b6579"},
      {"relational_output_v1", "2", "1|2|2|1|1|76616c7565"},
      {"relational_output_v1", "3", "1|3|3|1|2|6f72646572"},
      {"relational_output_v1", "4",
       "2|16|4|1|0|6a736f6e5f6f626a6563745f6167675f76616c7565"},
      {"relational_values_row_v1", "1", "1,2,3"},
      {"relational_values_row_v1", "2", "4,5,6"},
      {"relational_values_row_v1", "3", "7,8,9"},
      {"relational_values_row_v1", "4", "10,11,12"},
      {"relational_node_v1", "1", "13|0|-|1,2,3|1,2,3,4"},
      {"relational_node_v1", "2", "5|0|1|4|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5,6,7,8,9,10,11,12|-|-|-"},
      {"relational_node_binding_v1", "2",
       "6167677265676174652e676c6f62616c2d6a736f6e2d6f626a6563742d"
       "6167672d6f7264657265642d65787072657373696f6e2e7631|16|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope OrderedJsonObjectAggModifierValuesEnvelope(
    const AggregateModifierProfile modifier) {
  const bool has_filter = modifier != AggregateModifierProfile::kDistinct;
  const std::string modifier_digit =
      modifier == AggregateModifierProfile::kFilter
          ? "1"
          : modifier == AggregateModifierProfile::kDistinct ? "2" : "3";
  const std::string modifier_suffix =
      modifier == AggregateModifierProfile::kFilter
          ? "-filter"
          : modifier == AggregateModifierProfile::kDistinct
                ? "-distinct"
                : "-distinct-filter";
  const auto fixture_uuid = [&](const std::string_view ordinal) {
    return PairProfileUuid("8f0" + modifier_digit, "c5", ordinal);
  };
  const auto tree_uuid = fixture_uuid("00");
  const auto key_descriptor = fixture_uuid("01");
  const auto key_type = fixture_uuid("02");
  const auto value_descriptor = fixture_uuid("03");
  const auto value_type = fixture_uuid("04");
  const auto order_descriptor = fixture_uuid("05");
  const auto order_type = fixture_uuid("06");
  const auto filter_descriptor = fixture_uuid("07");
  const auto filter_type = fixture_uuid("08");
  const auto result_descriptor = fixture_uuid("09");
  const auto result_type = fixture_uuid("0a");
  const auto key_bound_name = fixture_uuid("0b");
  const auto value_bound_name = fixture_uuid("0c");
  const auto order_bound_name = fixture_uuid("0d");
  const auto filter_bound_name = fixture_uuid("0e");
  const std::string function_children =
      has_filter ? "33,34,35,36" : "33,34,35";
  const std::string semantic_variant =
      "aggregate.global-json-object-agg-ordered" + modifier_suffix +
      "-expression.v1";

  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      "qow.live.values.json-object-agg-ordered" + modifier_suffix);
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid", tree_uuid},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       key_descriptor + "|" + key_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       value_descriptor + "|" + value_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       order_descriptor + "|" + order_type + "|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "4",
       filter_descriptor + "|" + filter_type + "|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "5",
       result_descriptor + "|" + result_type + "|2|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|2|-|647570"},
      {"relational_expression_v1", "2", "1|-|2|-|-|1|-|3130"},
      {"relational_expression_v1", "3", "1|-|3|-|-|1|-|37"},
      {"relational_expression_v1", "4", "1|-|4|-|-|6|-|74727565"},
      {"relational_expression_v1", "5", "1|-|1|-|-|2|-|70616972"},
      {"relational_expression_v1", "6", "1|-|2|-|-|1|-|3230"},
      {"relational_expression_v1", "7", "1|-|3|-|-|1|-|32"},
      {"relational_expression_v1", "8", "1|-|4|-|-|6|-|66616c7365"},
      {"relational_expression_v1", "9", "1|-|1|-|-|2|-|70616972"},
      {"relational_expression_v1", "10", "1|-|2|-|-|1|-|3230"},
      {"relational_expression_v1", "11", "1|-|3|-|-|1|-|36"},
      {"relational_expression_v1", "12", "1|-|4|-|-|6|-|74727565"},
      {"relational_expression_v1", "13", "1|-|1|-|-|2|-|6f74686572"},
      {"relational_expression_v1", "14", "1|-|2|-|-|1|-|3330"},
      {"relational_expression_v1", "15", "1|-|3|-|-|1|-|33"},
      {"relational_expression_v1", "16", "1|-|4|-|-|7|-|2d"},
      {"relational_expression_v1", "17", "1|-|1|-|-|2|-|726570656174"},
      {"relational_expression_v1", "18", "1|-|2|-|-|1|-|3430"},
      {"relational_expression_v1", "19", "1|-|3|-|-|1|-|38"},
      {"relational_expression_v1", "20", "1|-|4|-|-|6|-|74727565"},
      {"relational_expression_v1", "21", "1|-|1|-|-|2|-|726570656174"},
      {"relational_expression_v1", "22", "1|-|2|-|-|1|-|3430"},
      {"relational_expression_v1", "23", "1|-|3|-|-|1|-|31"},
      {"relational_expression_v1", "24", "1|-|4|-|-|6|-|74727565"},
      {"relational_expression_v1", "25", "1|-|1|-|-|2|-|647570"},
      {"relational_expression_v1", "26", "1|-|2|-|-|1|-|3530"},
      {"relational_expression_v1", "27", "1|-|3|-|-|1|-|34"},
      {"relational_expression_v1", "28", "1|-|4|-|-|6|-|74727565"},
      {"relational_expression_v1", "29", "1|-|1|-|-|2|-|6e756c6c76"},
      {"relational_expression_v1", "30", "1|-|2|-|-|7|-|2d"},
      {"relational_expression_v1", "31", "1|-|3|-|-|1|-|30"},
      {"relational_expression_v1", "32", "1|-|4|-|-|6|-|74727565"},
      {"relational_expression_v1", "33",
       "3|-|1|-|" + key_bound_name + "|-|-|-"},
      {"relational_expression_v1", "34",
       "3|-|2|-|" + value_bound_name + "|-|-|-"},
      {"relational_expression_v1", "35",
       "3|-|3|-|" + order_bound_name + "|-|-|-"},
      {"relational_expression_v1", "36",
       "3|-|4|-|" + filter_bound_name + "|-|-|-"},
      {"relational_expression_v1", "37",
       "4|" + function_children +
           "|5|019dffbb-f001-7021-8a00-000000000024|-|-|-|-"},
      {"relational_output_v1", "1", "1|1|1|1|0|6b6579"},
      {"relational_output_v1", "2", "1|2|2|1|1|76616c7565"},
      {"relational_output_v1", "3", "1|3|3|1|2|6f72646572"},
      {"relational_output_v1", "4", "1|4|4|1|3|73656c6563746564"},
      {"relational_output_v1", "5",
       "2|37|5|1|0|6a736f6e5f6f626a6563745f6167675f76616c7565"},
      {"relational_values_row_v1", "1", "1,2,3,4"},
      {"relational_values_row_v1", "2", "5,6,7,8"},
      {"relational_values_row_v1", "3", "9,10,11,12"},
      {"relational_values_row_v1", "4", "13,14,15,16"},
      {"relational_values_row_v1", "5", "17,18,19,20"},
      {"relational_values_row_v1", "6", "21,22,23,24"},
      {"relational_values_row_v1", "7", "25,26,27,28"},
      {"relational_values_row_v1", "8", "29,30,31,32"},
      {"relational_node_v1", "1", "13|0|-|1,2,3,4|1,2,3,4,5,6,7,8"},
      {"relational_node_v1", "2", "5|0|1|5|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,"
       "25,26,27,28,29,30,31,32|-|-|-"},
      {"relational_node_binding_v1", "2",
       EncodeHex(semantic_variant) + "|37|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

// RCP-049-TEST-NODE-DRIVEN-COMPLEX-AGGREGATE-COMPOSITION-V1
sblr::SblrOperationEnvelope NodeDrivenComplexAggregateLimitEnvelope(
    sblr::SblrOperationEnvelope envelope, const std::size_t ordinal) {
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string ordinal_hex(2, '0');
  ordinal_hex[0] = kHex[(ordinal >> 4U) & 0x0fU];
  ordinal_hex[1] = kHex[ordinal & 0x0fU];
  const auto tree_uuid = PairProfileUuid("cf00", "d0", ordinal_hex);
  std::string aggregate_output_descriptor;
  for (auto& operand : envelope.operands) {
    if (operand.type == "uuid" &&
        operand.name == "relational_bound_sblr_tree_uuid") {
      operand.value = tree_uuid;
    } else if (operand.type == "uint32" &&
               operand.name == "relational_root_node_id") {
      operand.value = "3";
    } else if (operand.type == "relational_node_v1" &&
               operand.name == "2") {
      std::size_t output_begin = 0;
      for (std::size_t field = 0; field < 3; ++field) {
        output_begin = operand.value.find('|', output_begin);
        if (output_begin == std::string::npos) break;
        ++output_begin;
      }
      if (output_begin != std::string::npos) {
        const auto output_end = operand.value.find('|', output_begin);
        aggregate_output_descriptor = operand.value.substr(
            output_begin, output_end - output_begin);
      }
    }
  }
  envelope.operands.push_back(
      {"relational_descriptor_v1", "99",
       "019f0000-0000-7300-8000-00000000cf99|"
       "019f0000-0000-7400-8000-00000000e208|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "99", "1|-|99|-|-|1|-|31"});
  envelope.operands.push_back(
      {"relational_node_v1", "3",
       "7|0|2|" + aggregate_output_descriptor + "|-"});
  envelope.operands.push_back(
      {"relational_node_binding_v1", "3",
       "6c696d69742e626f756e642d636f756e742e7631|99|-|-|-"});
  return envelope;
}

bool ValidateLiveValuesSpine() {
  const auto first =
      sblr::DispatchTextualRelationalQueryForContractTest({Context(), ValuesEnvelope(), {}});
  const auto repeated =
      sblr::DispatchTextualRelationalQueryForContractTest({Context(), ValuesEnvelope(), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  passed &= Require(
      HasExactStatementContextHeader(ValuesEnvelope()),
      "VALUES finalizer changed, reordered, duplicated, or aliased statement context");
  passed &= Require(
      first.accepted && first.envelope_validated && first.dispatched_to_api &&
          first.logical_graph_populated &&
          first.logical_properties_populated && first.optimizer_admitted &&
          first.optimizer_selected && first.physical_dag_published &&
          first.physical_dag_executed && first.runtime_actuals_attached &&
          first.canonical_result_published && first.api_result.ok &&
          first.diagnostics.empty(),
      "live VALUES did not traverse every canonical spine stage");
  passed &= Require(
      first.optimizer_admission_stage_count == 8 &&
          first.logical_node_count == 1 && first.logical_property_count == 0 &&
          first.physical_node_count == 1 &&
          first.canonical_result_column_count == 2 &&
          first.canonical_result_row_count == 2 &&
          !first.selected_plan_uuid.empty() &&
          !first.canonical_result_bytes.empty(),
      "live VALUES stage evidence or canonical result cardinality differs");
  passed &= Require(
      first.api_result.result_shape.result_kind == "rows" &&
          first.api_result.result_shape.columns.size() == 2 &&
          first.api_result.result_shape.rows.size() == 2 &&
          first.api_result.result_shape.rows[0].fields.size() == 2 &&
          first.api_result.result_shape.rows[0].fields[0].first == "id" &&
          first.api_result.result_shape.rows[0].fields[0].second.encoded_value ==
              "7" &&
          first.api_result.result_shape.rows[0].fields[1].first == "label" &&
          first.api_result.result_shape.rows[0].fields[1].second.encoded_value ==
              "alpha" &&
          first.api_result.result_shape.rows[1].fields[0].second.encoded_value ==
              "11" &&
          first.api_result.result_shape.rows[1].fields[1].second.state ==
              api::EngineValueState::sql_null,
      "canonical VALUES rows did not reach the EngineApiResult adapter");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical admitted VALUES input changed canonical plan/result bytes");
  return passed;
}

bool ValidateRuntimeBreadthValuesSpine() {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), RuntimeBreadthValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), RuntimeBreadthValuesEnvelope(), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached &&
          first.canonical_result_published && first.api_result.ok &&
          first.logical_node_count == 1 && first.physical_node_count == 1 &&
          first.canonical_result_column_count == 8 &&
          first.canonical_result_row_count == 2,
      "expanded descriptor VALUES did not traverse the canonical result route");
  const auto& columns = first.api_result.result_shape.columns;
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      columns.size() == 8 && rows.size() == 2 &&
          columns[0].canonical_type_name == "int8" &&
          columns[1].canonical_type_name == "real64" &&
          columns[2].canonical_type_name == "decimal" &&
          columns[3].canonical_type_name == "binary" &&
          columns[4].canonical_type_name == "uuid" &&
          columns[5].canonical_type_name == "date" &&
          columns[6].canonical_type_name == "timestamp" &&
          columns[7].canonical_type_name == "text" &&
          rows[0].fields[0].second.encoded_value == "-128" &&
          rows[0].fields[1].second.encoded_value == "1.25" &&
          rows[0].fields[2].second.encoded_value == "12.34" &&
          rows[0].fields[3].second.encoded_value == "a" &&
          rows[0].fields[4].second.encoded_value ==
              "019f0000-0000-7200-8000-000000008201" &&
          rows[0].fields[5].second.encoded_value == "2026-08-03" &&
          rows[0].fields[6].second.encoded_value ==
              "2026-08-03T09:00:00-04:00" &&
          rows[1].fields[7].second.state ==
              api::EngineValueState::sql_null &&
          rows[1].fields[7].second.encoded_value.empty(),
      "expanded descriptor values or SQL NULL lost canonical type/value state");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "expanded descriptor route changed plan/result bytes on replay");
  return passed;
}

bool ValidateLiveValuesPreResultRevocationIsAtomic() {
  sblr::ArmCanonicalQueryPreResultRevocationForContractTest();
  const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), ValuesEnvelope(), {}});
  return Require(
      result.accepted && result.optimizer_admitted &&
          !result.optimizer_selected && !result.physical_dag_published &&
          !result.physical_dag_executed &&
          !result.runtime_actuals_attached &&
          !result.canonical_result_published && !result.api_result.ok &&
          result.physical_node_count == 0 &&
          result.canonical_result_column_count == 0 &&
          result.canonical_result_row_count == 0 &&
          result.selected_plan_uuid.empty() &&
          result.canonical_result_bytes.empty() &&
          sblr::CanonicalQueryContractRevalidationCountForTest() == 8 &&
          HasApiDiagnostic(result, "QOW-DIAG-MGA-RUNTIME-CURRENT-V1"),
      "pre-result MGA revocation exposed an internal route DAG, actual, row, or result");
}

// RCP-022-TEST-GENERAL-SELECT-EXECUTION-BOUNDARY-V1
bool ValidateGeneralSelectExecutionBoundary() {
  const auto refused_atomically = [](const sblr::SblrDispatchResult& result) {
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_column_count == 0 &&
           result.canonical_result_row_count == 0 &&
           result.selected_plan_uuid.empty() &&
           result.canonical_result_bytes.empty();
  };

  bool passed = true;
  const std::array<sblr::SblrOperationEnvelope, 32> live_shapes{
      ValuesEnvelope(),
      FilterValuesEnvelope(),
      ProjectValuesEnvelope(),
      LimitValuesEnvelope(),
      SortValuesEnvelope(),
      GlobalCountStarValuesEnvelope(),
      UnionAllValuesEnvelope(),
      SetOperationValuesEnvelope(
          "7365742d6f7065726174696f6e2e756e696f6e2d64697374696e63742e7631",
          "019f0000-0000-7000-8000-00000000b200"),
      SetOperationValuesEnvelope(
          "7365742d6f7065726174696f6e2e696e746572736563742d616c6c2e7631",
          "019f0000-0000-7000-8000-00000000b201"),
      SetOperationValuesEnvelope(
          "7365742d6f7065726174696f6e2e696e746572736563742d64697374696e63742e7631",
          "019f0000-0000-7000-8000-00000000b202"),
      SetOperationValuesEnvelope(
          "7365742d6f7065726174696f6e2e6578636570742d616c6c2e7631",
          "019f0000-0000-7000-8000-00000000b203"),
      SetOperationValuesEnvelope(
          "7365742d6f7065726174696f6e2e6578636570742d64697374696e63742e7631",
          "019f0000-0000-7000-8000-00000000b204"),
      SetOperationValuesEnvelope(
          "7365742d6f7065726174696f6e2e756e696f6e2d616c6c2e747970652d7265636f6e63696c65642e7631",
          "019f0000-0000-7000-8000-00000000b205"),
      SetOperationByNameValuesEnvelope(),
      SetOperationNestedValuesEnvelope(false),
      SetOperationNestedValuesEnvelope(true),
      InnerJoinValuesEnvelope(),
      DistinctSortLimitValuesEnvelope(false),
      ProjectedExpressionSortValuesEnvelope(),
      FilteredExpressionProjectValuesEnvelope(),
      FilteredProjectedSortValuesEnvelope(),
      FilteredProjectedSortLimitValuesEnvelope(),
      FilteredProjectedDistinctSortLimitValuesEnvelope(),
      FilteredProjectedDistinctSortOffsetValuesEnvelope(false),
      FilteredProjectedDistinctSortOffsetValuesEnvelope(true),
      RejectAllFilteredRows(
          FilteredProjectedDistinctSortOffsetValuesEnvelope(true)),
      InnerJoinFilterProjectValuesEnvelope(),
      InnerJoinFilterProjectSortValuesEnvelope(),
      InnerJoinFilterProjectSortLimitValuesEnvelope(),
      InnerJoinFilterProjectDistinctSortLimitValuesEnvelope(),
      InnerJoinFilterProjectDistinctSortOffsetValuesEnvelope(false),
      InnerJoinFilterProjectDistinctSortOffsetValuesEnvelope(true),
  };
  for (std::size_t shape = 0; shape < live_shapes.size(); ++shape) {
    const auto& envelope = live_shapes[shape];
    auto context = Context();
    context.query_cancellation_requested = [] { return true; };
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), envelope, {}});
    passed &= Require(
        refused_atomically(result) &&
            HasApiDiagnostic(
                result,
                "QOW-DIAG-QRY-004-PHYSICAL-DISPATCH-CANCELLED-V1"),
        std::string("general SELECT shape bypassed the engine cancellation boundary: ") +
            std::to_string(shape));
  }

  for (std::size_t shape = 0; shape < live_shapes.size(); ++shape) {
    const auto& envelope = live_shapes[shape];
    sblr::ArmCanonicalQueryPreResultRevocationForContractTest();
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), envelope, {}});
    const bool shape_refused =
        refused_atomically(result) &&
        sblr::CanonicalQueryContractRevalidationCountForTest() >= 8 &&
        HasApiDiagnosticToken(result, "QOW-DIAG-MGA-RUNTIME-CURRENT-V1");
    if (!shape_refused) {
      std::cerr << "RCP-022 MGA shape=" << shape
                << " revalidations="
                << sblr::CanonicalQueryContractRevalidationCountForTest()
                << '\n';
      for (const auto& diagnostic : result.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    passed &= Require(
        shape_refused,
        std::string("general SELECT shape bypassed current MGA visibility revalidation: ") +
            std::to_string(shape));
  }

  std::size_t cancellation_probe_count = 0;
  auto mid_dag_context = Context();
  mid_dag_context.query_cancellation_requested =
      [&cancellation_probe_count] {
        return ++cancellation_probe_count == 3;
      };
  const auto mid_dag = sblr::DispatchTextualRelationalQueryForContractTest(
      {std::move(mid_dag_context), UnionAllValuesEnvelope(), {}});
  passed &= Require(
      refused_atomically(mid_dag) && cancellation_probe_count == 3 &&
          HasApiDiagnostic(
              mid_dag,
              "QOW-DIAG-QRY-004-PHYSICAL-DISPATCH-CANCELLED-V1"),
      "mid-DAG cancellation exposed a completed internal SELECT batch");

  sblr::ArmCanonicalQuerySecurityBoundaryDriftForContractTest();
  const auto security_drift =
      sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(), ValuesEnvelope(), {}});
  passed &= Require(
      refused_atomically(security_drift) &&
          HasApiDiagnostic(
              security_drift,
              "QOW-DIAG-QRY-004-SELECT-EXECUTION-BOUNDARY-V1"),
      "stale selected-plan security authority reached SELECT execution");

  sblr::ArmCanonicalQueryResourceBoundaryDriftForContractTest();
  const auto resource_drift =
      sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(), ValuesEnvelope(), {}});
  passed &= Require(
      refused_atomically(resource_drift) &&
          HasApiDiagnostic(
              resource_drift,
              "QOW-DIAG-QRY-004-SELECT-EXECUTION-BOUNDARY-V1"),
      "stale selected-plan resource authority reached SELECT execution");
  return passed;
}

bool ValidateLiveStatementContextRefusalIsAtomic() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };

  bool passed = true;
  const std::array<std::size_t, 7> context_operand_indexes{
      2, 4, 5, 6, 7, 8, 9};
  for (const auto index : context_operand_indexes) {
    auto missing = ValuesEnvelope();
    missing.operands.erase(missing.operands.begin() + index);
    passed &= Require(
        RefusedStatementContextAtomically(dispatch(std::move(missing))),
        "missing carried context exposed a plan, executor, row, or result");

    auto duplicate = ValuesEnvelope();
    duplicate.operands.push_back(duplicate.operands[index]);
    passed &= Require(
        RefusedStatementContextAtomically(dispatch(std::move(duplicate))),
        "duplicate carried context exposed a plan, executor, row, or result");
  }

  for (const auto index : std::array<std::size_t, 5>{2, 4, 5, 6, 7}) {
    auto malformed = ValuesEnvelope();
    malformed.operands[index].value = "not-a-uuid";
    passed &= Require(
        RefusedStatementContextAtomically(dispatch(std::move(malformed))),
        "malformed carried UUID exposed a plan, executor, row, or result");

    auto nil = ValuesEnvelope();
    nil.operands[index].value = "00000000-0000-0000-0000-000000000000";
    passed &= Require(
        RefusedStatementContextAtomically(dispatch(std::move(nil))),
        "nil carried UUID exposed a plan, executor, row, or result");
  }

  auto reordered = ValuesEnvelope();
  std::swap(reordered.operands[6], reordered.operands[7]);
  passed &= Require(
      RefusedStatementContextAtomically(dispatch(std::move(reordered))),
      "reordered snapshot identities exposed a plan, executor, row, or result");

  auto narrowed = ValuesEnvelope();
  narrowed.operands[8].type = "uint32";
  passed &= Require(
      RefusedStatementContextAtomically(dispatch(std::move(narrowed))),
      "narrowed local transaction identity exposed a plan, executor, row, or result");

  auto context = Context();
  context.statement_uuid.canonical =
      "019f0000-0000-7120-8000-000000008199";
  passed &= Require(
      RefusedStatementContextAtomically(
          dispatch(ValuesEnvelope(), std::move(context))),
      "statement mismatch exposed a plan, executor, row, or result");
  context = Context();
  context.transaction_uuid.canonical =
      "019f0000-0000-7130-8000-000000008199";
  passed &= Require(
      RefusedStatementContextAtomically(
          dispatch(ValuesEnvelope(), std::move(context))),
      "transaction mismatch exposed a plan, executor, row, or result");
  context = Context();
  context.statement_snapshot_uuid.canonical =
      "019f0000-0000-7140-8000-000000008199";
  passed &= Require(
      RefusedStatementContextAtomically(
          dispatch(ValuesEnvelope(), std::move(context))),
      "data snapshot mismatch exposed a plan, executor, row, or result");
  context = Context();
  context.statement_metadata_snapshot_uuid.canonical =
      "019f0000-0000-7150-8000-000000008199";
  passed &= Require(
      RefusedStatementContextAtomically(
          dispatch(ValuesEnvelope(), std::move(context))),
      "metadata snapshot mismatch exposed a plan, executor, row, or result");
  context = Context();
  context.catalog_epoch_uuid.canonical =
      "019f0000-0000-7100-8000-000000008199";
  passed &= Require(
      RefusedStatementContextAtomically(
          dispatch(ValuesEnvelope(), std::move(context))),
      "catalog mismatch exposed a plan, executor, row, or result");
  context = Context();
  ++context.local_transaction_id;
  passed &= Require(
      RefusedStatementContextAtomically(
          dispatch(ValuesEnvelope(), std::move(context))),
      "local transaction mismatch exposed a plan, executor, row, or result");
  context = Context();
  ++context.snapshot_visible_through_local_transaction_id;
  passed &= Require(
      RefusedStatementContextAtomically(
          dispatch(ValuesEnvelope(), std::move(context))),
      "visibility mismatch exposed a plan, executor, row, or result");

  auto zero_highwater = ValuesEnvelope();
  zero_highwater.operands[9].value = "0";
  context = Context();
  context.snapshot_visible_through_local_transaction_id = 0;
  const auto zero_result =
      dispatch(std::move(zero_highwater), std::move(context));
  passed &= Require(
      zero_result.accepted && zero_result.physical_dag_executed &&
          zero_result.canonical_result_published && zero_result.api_result.ok,
      "exact zero visibility high-water did not reach canonical publication");
  return passed;
}

bool ValidateComposedScalarValuesSpine() {
  const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), ComposedValuesEnvelope(), {}});
  bool passed = true;
  passed &= Require(
      result.accepted && result.optimizer_admitted && result.optimizer_selected &&
          result.physical_dag_published && result.physical_dag_executed &&
          result.runtime_actuals_attached && result.canonical_result_published &&
          result.api_result.ok && result.diagnostics.empty(),
      "composed scalar VALUES did not traverse the canonical spine");
  passed &= Require(
      result.api_result.result_shape.rows.size() == 1 &&
          result.api_result.result_shape.rows[0].fields.size() == 8 &&
          result.api_result.result_shape.rows[0].fields[0].first == "sum" &&
          result.api_result.result_shape.rows[0].fields[0].second.encoded_value ==
              "3" &&
          result.api_result.result_shape.rows[0].fields[1].first == "negated" &&
          result.api_result.result_shape.rows[0].fields[1].second.encoded_value ==
              "-3" &&
          result.api_result.result_shape.rows[0].fields[2].first == "greater" &&
          result.api_result.result_shape.rows[0].fields[2].second.encoded_value ==
              "true" &&
          result.api_result.result_shape.rows[0].fields[3].first == "truth" &&
          result.api_result.result_shape.rows[0].fields[3].second.state ==
              api::EngineValueState::sql_null &&
          result.api_result.result_shape.rows[0].fields[4].first == "concat" &&
          result.api_result.result_shape.rows[0].fields[4].second.encoded_value ==
              "ab" &&
          result.api_result.result_shape.rows[0].fields[5].first == "disjunct" &&
          result.api_result.result_shape.rows[0].fields[5].second.encoded_value ==
              "true" &&
          result.api_result.result_shape.rows[0].fields[6].first == "is_null" &&
          result.api_result.result_shape.rows[0].fields[6].second.encoded_value ==
              "false" &&
          result.api_result.result_shape.rows[0].fields[7].first == "negation" &&
          result.api_result.result_shape.rows[0].fields[7].second.encoded_value ==
              "true",
      "composed arithmetic, comparison, logical, NULL, or text result differs");
  return passed;
}

bool ValidateUnionAllValuesSpine() {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), UnionAllValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), UnionAllValuesEnvelope(), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached && first.canonical_result_published &&
          first.api_result.ok && first.diagnostics.empty() &&
          first.logical_node_count == 3 && first.physical_node_count == 3 &&
          first.canonical_result_column_count == 1 &&
          first.canonical_result_row_count == 6,
      "VALUES UNION ALL did not traverse the selected multi-node DAG");
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      rows.size() == 6 && rows[0].fields[0].first == "n" &&
          rows[0].fields[0].second.encoded_value == "1" &&
          rows[1].fields[0].second.encoded_value == "2" &&
          rows[2].fields[0].second.state == api::EngineValueState::sql_null &&
          rows[3].fields[0].second.encoded_value == "2" &&
          rows[4].fields[0].second.encoded_value == "3" &&
          rows[5].fields[0].second.state == api::EngineValueState::sql_null,
      "UNION ALL did not preserve typed input order and multiplicity");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical UNION ALL input changed canonical plan/result bytes");
  return passed;
}

bool ValidateUnionAllRefusalIsAtomic() {
  auto incompatible = UnionAllValuesEnvelope();
  for (auto& operand : incompatible.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "5") {
      operand.value = "1|-|2|-|-|2|-|7468726565";
    }
  }
  const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), std::move(incompatible), {}});
  return Require(
      result.accepted && result.optimizer_admitted &&
          !result.optimizer_selected && !result.physical_dag_published &&
          !result.physical_dag_executed && !result.runtime_actuals_attached &&
          !result.canonical_result_published && !result.api_result.ok &&
          result.physical_node_count == 0 &&
          result.canonical_result_bytes.empty() &&
          HasApiDiagnostic(result,
                           "QOW-DIAG-RELATIONAL-LIVE-SET-PAYLOAD-V1"),
      "incompatible UNION ALL input published partial plan/result evidence");
}

// RCP-046-TEST-LIVE-SET-OPERATION-PROFILES-V1
bool ValidateSetOperationProfilesSpine() {
  struct ProfileExpectation {
    std::string semantic_hex;
    std::string tree_uuid;
    std::vector<std::string> values;
  };
  const std::array<ProfileExpectation, 6> profiles{{
      {"7365742d6f7065726174696f6e2e756e696f6e2d64697374696e63742e7631",
       "019f0000-0000-7000-8000-00000000b200",
       {"1", "2", "null", "3"}},
      {"7365742d6f7065726174696f6e2e696e746572736563742d616c6c2e7631",
       "019f0000-0000-7000-8000-00000000b201", {"2", "null"}},
      {"7365742d6f7065726174696f6e2e696e746572736563742d64697374696e63742e7631",
       "019f0000-0000-7000-8000-00000000b202", {"2", "null"}},
      {"7365742d6f7065726174696f6e2e6578636570742d616c6c2e7631",
       "019f0000-0000-7000-8000-00000000b203", {"1"}},
      {"7365742d6f7065726174696f6e2e6578636570742d64697374696e63742e7631",
       "019f0000-0000-7000-8000-00000000b204", {"1"}},
      {"7365742d6f7065726174696f6e2e756e696f6e2d616c6c2e747970652d7265636f6e63696c65642e7631",
       "019f0000-0000-7000-8000-00000000b205",
       {"1", "2", "null", "2", "3", "null"}},
  }};
  const auto completed = [](const sblr::SblrDispatchResult& result,
                            const std::size_t rows) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 3 &&
           result.physical_node_count == 3 &&
           result.canonical_result_column_count == 1 &&
           result.canonical_result_row_count == rows &&
           result.api_result.result_shape.rows.size() == rows;
  };

  bool passed = true;
  std::unordered_set<std::string> selected_plans;
  for (const auto& profile : profiles) {
    const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(),
         SetOperationValuesEnvelope(profile.semantic_hex, profile.tree_uuid),
         {}});
    const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(),
         SetOperationValuesEnvelope(profile.semantic_hex, profile.tree_uuid),
         {}});
    if (!first.api_result.ok) {
      for (const auto& diagnostic : first.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    bool values_match = completed(first, profile.values.size());
    if (values_match) {
      for (std::size_t row = 0; row < profile.values.size(); ++row) {
        const auto& value =
            first.api_result.result_shape.rows[row].fields[0].second;
        values_match &= profile.values[row] == "null"
                            ? value.state == api::EngineValueState::sql_null
                            : value.encoded_value == profile.values[row];
      }
    }
    passed &= Require(
        values_match,
        "UNION/INTERSECT/EXCEPT ALL/DISTINCT profile changed canonical "
        "multiplicity or NULL equality");
    passed &= Require(
        completed(repeated, profile.values.size()) &&
            repeated.selected_plan_uuid == first.selected_plan_uuid &&
            repeated.canonical_result_bytes == first.canonical_result_bytes,
        "set-operation profile changed deterministic plan/result bytes");
    selected_plans.insert(first.selected_plan_uuid);
  }
  passed &= Require(
      selected_plans.size() == profiles.size(),
      "set-operation semantic profiles did not retain distinct plan identity");

  const auto by_name = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), SetOperationByNameValuesEnvelope(), {}});
  const auto by_name_repeated =
      sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(), SetOperationByNameValuesEnvelope(), {}});
  const auto& by_name_rows = by_name.api_result.result_shape.rows;
  passed &= Require(
      by_name.api_result.ok && by_name.optimizer_selected &&
          by_name.physical_dag_executed &&
          by_name.canonical_result_published &&
          by_name.canonical_result_column_count == 2 &&
          by_name.canonical_result_row_count == 3 &&
          by_name_rows.size() == 3 &&
          by_name_rows[0].fields[0].first == "n" &&
          by_name_rows[0].fields[1].first == "label" &&
          by_name_rows[0].fields[0].second.encoded_value == "1" &&
          by_name_rows[0].fields[1].second.encoded_value == "10" &&
          by_name_rows[1].fields[0].second.encoded_value == "2" &&
          by_name_rows[1].fields[1].second.encoded_value == "20" &&
          by_name_rows[2].fields[0].second.encoded_value == "3" &&
          by_name_rows[2].fields[1].second.encoded_value == "30",
      "UNION DISTINCT BY NAME did not align the reversed right schema before "
      "duplicate elimination");
  passed &= Require(
      by_name_repeated.api_result.ok &&
          by_name_repeated.selected_plan_uuid == by_name.selected_plan_uuid &&
          by_name_repeated.canonical_result_bytes ==
              by_name.canonical_result_bytes,
      "UNION DISTINCT BY NAME changed deterministic plan/result bytes");

  auto duplicate_name = SetOperationByNameValuesEnvelope();
  for (auto& operand : duplicate_name.operands) {
    if (operand.type == "relational_output_v1" && operand.name == "4") {
      operand.value = "2|6|4|1|1|6c6162656c";
    }
  }
  const auto refused = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), std::move(duplicate_name), {}});
  passed &= Require(
      refused.accepted && refused.optimizer_admitted &&
          !refused.optimizer_selected && !refused.physical_dag_published &&
          !refused.physical_dag_executed &&
          !refused.runtime_actuals_attached &&
          !refused.canonical_result_published && !refused.api_result.ok &&
          refused.physical_node_count == 0 &&
          refused.canonical_result_bytes.empty() &&
          HasApiDiagnostic(refused,
                           "QOW-DIAG-RELATIONAL-LIVE-SET-PAYLOAD-V1"),
      "duplicate BY NAME input published partial plan/result evidence");
  return passed;
}

// RCP-046-TEST-LIVE-SET-OPERATION-NESTING-V1
bool ValidateSetOperationNestingSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result,
                            const std::vector<std::string>& values) {
    if (!result.accepted || !result.optimizer_admitted ||
        !result.optimizer_selected || !result.physical_dag_published ||
        !result.physical_dag_executed || !result.runtime_actuals_attached ||
        !result.canonical_result_published || !result.api_result.ok ||
        !result.diagnostics.empty() || result.logical_node_count != 5 ||
        result.physical_node_count != 5 ||
        result.canonical_result_column_count != 1 ||
        result.canonical_result_row_count != values.size() ||
        result.api_result.result_shape.rows.size() != values.size()) {
      return false;
    }
    for (std::size_t row = 0; row < values.size(); ++row) {
      if (result.api_result.result_shape.rows[row]
              .fields[0]
              .second.encoded_value != values[row]) {
        return false;
      }
    }
    return true;
  };

  const auto left_grouped =
      dispatch(SetOperationNestedValuesEnvelope(false));
  const auto left_repeated =
      dispatch(SetOperationNestedValuesEnvelope(false));
  const auto right_grouped =
      dispatch(SetOperationNestedValuesEnvelope(true));
  const auto right_repeated =
      dispatch(SetOperationNestedValuesEnvelope(true));
  if (!left_grouped.api_result.ok) {
    for (const auto& diagnostic : left_grouped.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  if (!right_grouped.api_result.ok) {
    for (const auto& diagnostic : right_grouped.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  passed &= Require(
      completed(left_grouped, {"1", "3"}),
      "(A UNION B) EXCEPT C did not consume the left-grouped selected DAG");
  passed &= Require(
      completed(right_grouped, {"1", "2", "3"}),
      "A UNION (B EXCEPT C) did not consume the right-grouped selected DAG");
  passed &= Require(
      left_grouped.selected_plan_uuid != right_grouped.selected_plan_uuid &&
          left_repeated.selected_plan_uuid ==
              left_grouped.selected_plan_uuid &&
          left_repeated.canonical_result_bytes ==
              left_grouped.canonical_result_bytes &&
          right_repeated.selected_plan_uuid ==
              right_grouped.selected_plan_uuid &&
          right_repeated.canonical_result_bytes ==
              right_grouped.canonical_result_bytes,
      "set-operation nesting lost grouping identity or deterministic replay");

  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 40;
  const auto exhausted = dispatch(SetOperationNestedValuesEnvelope(false),
                                  std::move(bounded_context));
  passed &= Require(
      exhausted.accepted && exhausted.optimizer_admitted &&
          !exhausted.optimizer_selected &&
          !exhausted.physical_dag_published &&
          !exhausted.physical_dag_executed &&
          !exhausted.runtime_actuals_attached &&
          !exhausted.canonical_result_published &&
          !exhausted.api_result.ok && exhausted.physical_node_count == 0 &&
          exhausted.canonical_result_bytes.empty() &&
          HasApiDiagnostic(exhausted,
                           "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "candidate-exhausted nested set operation published partial evidence");
  return passed;
}

bool ValidateInnerJoinValuesSpine() {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), InnerJoinValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), InnerJoinValuesEnvelope(), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached && first.canonical_result_published &&
          first.api_result.ok && first.diagnostics.empty() &&
          first.logical_node_count == 3 && first.physical_node_count == 3 &&
          first.canonical_result_column_count == 2 &&
          first.canonical_result_row_count == 4,
      "VALUES INNER JOIN did not traverse the selected multi-node DAG");
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      rows.size() == 4 && rows[0].fields.size() == 2 &&
          rows[0].fields[0].first == "left_n" &&
          rows[0].fields[1].first == "right_t" &&
          rows[0].fields[0].second.encoded_value == "1" &&
          rows[0].fields[1].second.encoded_value == "a" &&
          rows[1].fields[0].second.encoded_value == "1" &&
          rows[1].fields[1].second.encoded_value == "b" &&
          rows[2].fields[0].second.encoded_value == "2" &&
          rows[2].fields[1].second.encoded_value == "a" &&
          rows[3].fields[0].second.encoded_value == "2" &&
          rows[3].fields[1].second.encoded_value == "b",
      "INNER JOIN did not preserve canonical pair order and row shape");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical INNER JOIN input changed canonical plan/result bytes");
  return passed;
}

bool ValidateInnerJoinThreeValuedPredicate() {
  auto false_predicate = InnerJoinValuesEnvelope();
  auto unknown_predicate = InnerJoinValuesEnvelope();
  for (auto& operand : false_predicate.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "5") {
      operand.value = "1|-|3|-|-|6|-|46414c5345";
    }
  }
  for (auto& operand : unknown_predicate.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "5") {
      operand.value = "1|-|3|-|-|7|-|2d";
    }
  }
  const auto false_result = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), std::move(false_predicate), {}});
  const auto unknown_result = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), std::move(unknown_predicate), {}});
  const auto empty_success = [](const sblr::SblrDispatchResult& result) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.canonical_result_column_count == 2 &&
           result.canonical_result_row_count == 0 &&
           result.api_result.result_shape.columns.size() == 2 &&
           result.api_result.result_shape.rows.empty();
  };
  return Require(empty_success(false_result) && empty_success(unknown_result),
                 "INNER JOIN FALSE/UNKNOWN did not produce a typed empty result");
}

bool ValidateInnerJoinRefusalIsAtomic() {
  auto invalid_output = InnerJoinValuesEnvelope();
  auto unbound_identifier = InnerJoinValuesEnvelope();
  for (auto& operand : invalid_output.operands) {
    if (operand.type == "relational_node_v1" && operand.name == "3") {
      operand.value = "4|0|1,2|2,1|-";
    }
  }
  for (auto& operand : unbound_identifier.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "5") {
      operand.value =
          "3|-|3|-|019f0000-0000-7500-8000-000000008507|-|-|-";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-JOIN-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(invalid_output)) &&
          refused_atomically(std::move(unbound_identifier)),
      "invalid or source-unbound INNER JOIN published partial evidence");
}

// RCP-029-TEST-LIVE-ACCEPTED-JOIN-KINDS-V1
bool ValidateAcceptedJoinKindsSpine() {
  using Truth = JoinPredicateTruth;
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result,
                            const std::size_t columns,
                            const std::size_t rows) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed &&
           result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 3 &&
           result.physical_node_count == 3 &&
           result.canonical_result_column_count == columns &&
           result.canonical_result_row_count == rows &&
           result.api_result.result_shape.columns.size() == columns &&
           result.api_result.result_shape.rows.size() == rows;
  };

  const auto cross = dispatch(AcceptedJoinValuesEnvelope(
      "6a6f696e2e63726f73732e7631", "1,2", true,
      Truth::kTrue, false, false));
  const auto left = dispatch(AcceptedJoinValuesEnvelope(
      "6a6f696e2e6c6566742d6f757465722e7631", "1,2", false,
      Truth::kFalse, false, true));
  const auto right = dispatch(AcceptedJoinValuesEnvelope(
      "6a6f696e2e72696768742d6f757465722e7631", "1,2", false,
      Truth::kFalse, true, false));
  const auto full = dispatch(AcceptedJoinValuesEnvelope(
      "6a6f696e2e66756c6c2d6f757465722e7631", "1,2", false,
      Truth::kFalse, true, true));
  const auto semi = dispatch(AcceptedJoinValuesEnvelope(
      "6a6f696e2e6c6566742d73656d692e7631", "1", false,
      Truth::kTrue, false, false));
  const auto anti = dispatch(AcceptedJoinValuesEnvelope(
      "6a6f696e2e6c6566742d616e74692e7631", "1", false,
      Truth::kFalse, false, false));
  const auto anti_unknown = dispatch(AcceptedJoinValuesEnvelope(
      "6a6f696e2e6c6566742d616e74692e7631", "1", false,
      Truth::kUnknown, false, false));

  bool passed = true;
  passed &= Require(completed(cross, 2, 4),
                    "live CROSS JOIN did not publish its Cartesian result");
  passed &= Require(
      completed(left, 2, 2) &&
          left.api_result.result_shape.rows[0].fields[0].second.encoded_value ==
              "1" &&
          left.api_result.result_shape.rows[0].fields[1].second.state ==
              api::EngineValueState::sql_null &&
          left.api_result.result_shape.rows[1].fields[0].second.encoded_value ==
              "2" &&
          left.api_result.result_shape.rows[1].fields[1].second.state ==
              api::EngineValueState::sql_null,
      "live LEFT OUTER JOIN lost left order or typed NULL extension");
  passed &= Require(
      completed(right, 2, 2) &&
          right.api_result.result_shape.rows[0].fields[0].second.state ==
              api::EngineValueState::sql_null &&
          right.api_result.result_shape.rows[0].fields[1].second.encoded_value ==
              "a" &&
          right.api_result.result_shape.rows[1].fields[0].second.state ==
              api::EngineValueState::sql_null &&
          right.api_result.result_shape.rows[1].fields[1].second.encoded_value ==
              "b",
      "live RIGHT OUTER JOIN lost right order or typed NULL extension");
  passed &= Require(
      completed(full, 2, 4) &&
          full.api_result.result_shape.rows[0].fields[1].second.state ==
              api::EngineValueState::sql_null &&
          full.api_result.result_shape.rows[1].fields[1].second.state ==
              api::EngineValueState::sql_null &&
          full.api_result.result_shape.rows[2].fields[0].second.state ==
              api::EngineValueState::sql_null &&
          full.api_result.result_shape.rows[3].fields[0].second.state ==
              api::EngineValueState::sql_null,
      "live FULL OUTER JOIN did not preserve both unmatched sides");
  passed &= Require(
      completed(semi, 1, 2) &&
          semi.api_result.result_shape.rows[0].fields[0].second.encoded_value ==
              "1" &&
          semi.api_result.result_shape.rows[1].fields[0].second.encoded_value ==
              "2",
      "live LEFT SEMI JOIN did not emit each matching left row once");
  passed &= Require(
      completed(anti, 1, 2) &&
          anti.api_result.result_shape.rows[0].fields[0].second.encoded_value ==
              "1" &&
          anti.api_result.result_shape.rows[1].fields[0].second.encoded_value ==
              "2",
      "live LEFT ANTI JOIN did not emit each unmatched left row once");
  passed &= Require(
      completed(anti_unknown, 1, 2) &&
          anti_unknown.api_result.result_shape.rows[0]
                  .fields[0]
                  .second.encoded_value == "1" &&
          anti_unknown.api_result.result_shape.rows[1]
                  .fields[0]
                  .second.encoded_value == "2",
      "live LEFT ANTI JOIN treated SQL UNKNOWN as a match");

  const auto missing_nullable_authority = dispatch(AcceptedJoinValuesEnvelope(
      "6a6f696e2e6c6566742d6f757465722e7631", "1,2", false,
      Truth::kFalse, false, false));
  passed &= Require(
      missing_nullable_authority.accepted &&
          missing_nullable_authority.optimizer_admitted &&
          !missing_nullable_authority.optimizer_selected &&
          !missing_nullable_authority.physical_dag_published &&
          !missing_nullable_authority.physical_dag_executed &&
          !missing_nullable_authority.runtime_actuals_attached &&
          !missing_nullable_authority.canonical_result_published &&
          !missing_nullable_authority.api_result.ok &&
          missing_nullable_authority.physical_node_count == 0 &&
          missing_nullable_authority.canonical_result_bytes.empty() &&
          HasApiDiagnostic(missing_nullable_authority,
                           "QOW-DIAG-RELATIONAL-LIVE-JOIN-PAYLOAD-V1"),
      "outer join without nullable descriptor authority published evidence");
  return passed;
}

// RCP-030-TEST-LIVE-ROW-DEPENDENT-JOIN-PREDICATE-V1
bool ValidateRowDependentJoinPredicateSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result,
                            const std::size_t columns,
                            const std::size_t rows) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 3 &&
           result.physical_node_count == 3 &&
           result.canonical_result_column_count == columns &&
           result.canonical_result_row_count == rows &&
           result.api_result.result_shape.columns.size() == columns &&
           result.api_result.result_shape.rows.size() == rows;
  };

  const auto inner = dispatch(RowDependentJoinValuesEnvelope());
  const auto repeated = dispatch(RowDependentJoinValuesEnvelope());
  bool passed = true;
  passed &= Require(
      completed(inner, 2, 1) &&
          inner.api_result.result_shape.rows[0].fields[0]
                  .second.encoded_value == "2" &&
          inner.api_result.result_shape.rows[0].fields[1]
                  .second.encoded_value == "2",
      "row-dependent INNER JOIN did not select its descriptor-bound pair");
  passed &= Require(
      completed(repeated, 2, 1) &&
          repeated.selected_plan_uuid == inner.selected_plan_uuid &&
          repeated.canonical_result_bytes == inner.canonical_result_bytes,
      "row-dependent INNER JOIN changed deterministic plan/result bytes");

  auto duplicates = RowDependentJoinValuesEnvelope();
  for (auto& operand : duplicates.operands) {
    if (operand.type == "relational_expression_v1" &&
        (operand.name == "1" || operand.name == "2" ||
         operand.name == "3" || operand.name == "4")) {
      const auto descriptor =
          operand.name == "1" || operand.name == "2" ? "1" : "2";
      operand.value = "1|-|" + std::string(descriptor) + "|-|-|1|-|32";
    }
  }
  const auto duplicate_result = dispatch(std::move(duplicates));
  passed &= Require(
      completed(duplicate_result, 2, 4) &&
          std::ranges::all_of(
              duplicate_result.api_result.result_shape.rows,
              [](const auto& row) {
                return row.fields.size() == 2 &&
                       row.fields[0].second.encoded_value == "2" &&
                       row.fields[1].second.encoded_value == "2";
              }),
      "row-dependent INNER JOIN lost duplicate pair multiplicity");

  auto left_outer = RowDependentJoinValuesEnvelope();
  for (auto& operand : left_outer.operands) {
    if (operand.type == "relational_node_binding_v1" &&
        operand.name == "3") {
      operand.value =
          "6a6f696e2e6c6566742d6f757465722e7631|5|-|-|-";
    } else if (operand.type == "relational_descriptor_v1" &&
               operand.name == "2") {
      const auto marker = operand.value.find("|1|-|-|-|-|-");
      if (marker != std::string::npos) operand.value[marker + 1] = '2';
    } else if (operand.type == "relational_expression_v1" &&
               operand.name == "4") {
      operand.value = "1|-|2|-|-|7|-|2d";
    }
  }
  const auto outer_result = dispatch(left_outer);
  const auto typed_unmatched = std::ranges::find_if(
      outer_result.api_result.result_shape.rows, [](const auto& row) {
        return row.fields.size() == 2 &&
               row.fields[0].second.encoded_value == "1" &&
               row.fields[1].second.state ==
                   api::EngineValueState::sql_null;
      });
  passed &= Require(
      completed(outer_result, 2, 2) &&
          typed_unmatched != outer_result.api_result.result_shape.rows.end(),
      "row-dependent LEFT OUTER JOIN lost FALSE/UNKNOWN nonmatch extension");

  auto anti = std::move(left_outer);
  for (auto& operand : anti.operands) {
    if (operand.type == "relational_node_v1" && operand.name == "3") {
      operand.value = "4|0|1,2|1|-";
    } else if (operand.type == "relational_node_binding_v1" &&
               operand.name == "3") {
      operand.value =
          "6a6f696e2e6c6566742d616e74692e7631|5|-|-|-";
    }
  }
  const auto anti_result = dispatch(std::move(anti));
  passed &= Require(
      completed(anti_result, 1, 1) &&
          anti_result.api_result.result_shape.rows[0].fields[0]
                  .second.encoded_value == "1",
      "row-dependent LEFT ANTI JOIN treated SQL UNKNOWN as a match");

  auto unbound = RowDependentJoinValuesEnvelope();
  for (auto& operand : unbound.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "7") {
      operand.value =
          "3|-|3|-|019f0000-0000-7500-8000-000000008512|-|-|-";
    }
  }
  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 3;
  const auto unbound_result = dispatch(std::move(unbound));
  const auto exhausted_result = dispatch(RowDependentJoinValuesEnvelope(),
                                         std::move(bounded_context));
  const auto refused_before_publication = [](const auto& result,
                                             const std::string_view code) {
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(result, code);
  };
  passed &= Require(
      refused_before_publication(
          unbound_result, "QOW-DIAG-RELATIONAL-LIVE-JOIN-PAYLOAD-V1") &&
          refused_before_publication(
              exhausted_result,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "unbound or resource-exhausted row predicate published partial evidence");
  return passed;
}

// RCP-041-TEST-LIVE-INNER-JOIN-FILTER-PROJECT-COMPOSITION-V1
bool ValidateInnerJoinFilterProjectCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 5 &&
           result.logical_property_count == 0 &&
           result.physical_node_count == 5 &&
           result.canonical_result_column_count == 2 &&
           result.canonical_result_row_count == 2 &&
           result.api_result.result_shape.columns.size() == 2 &&
           result.api_result.result_shape.rows.size() == 2;
  };

  const auto first = dispatch(InnerJoinFilterProjectValuesEnvelope());
  const auto repeated = dispatch(InnerJoinFilterProjectValuesEnvelope());
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      completed(first) &&
          rows[0].fields[0].first == "safe_quotient" &&
          rows[0].fields[0].second.encoded_value == "10" &&
          rows[0].fields[1].first == "right_t" &&
          rows[0].fields[1].second.encoded_value == "a" &&
          rows[1].fields[0].second.encoded_value == "10" &&
          rows[1].fields[1].second.encoded_value == "b",
      "INNER JOIN/FILTER/PROJECT did not filter joined rows before computed "
      "projection");
  passed &= Require(
      completed(repeated) &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "INNER JOIN/FILTER/PROJECT changed deterministic plan/result bytes");

  auto source_only_project = InnerJoinFilterProjectValuesEnvelope();
  for (auto& operand : source_only_project.operands) {
    if (operand.type == "relational_node_binding_v1" &&
        operand.name == "5") {
      operand.value =
          "70726f6a6563742e73656c6563742d6c6973742e7631|6,14|-|-|-";
    }
  }
  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 11;
  const auto source_only_result = dispatch(std::move(source_only_project));
  const auto exhausted_result = dispatch(
      InnerJoinFilterProjectValuesEnvelope(), std::move(bounded_context));
  const auto refused_before_publication = [](const auto& result,
                                             const std::string_view code) {
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(result, code);
  };
  passed &= Require(
      refused_before_publication(
          source_only_result,
          "QOW-DIAG-RELATIONAL-LIVE-JOIN-FILTER-PROJECT-PAYLOAD-V1") &&
          refused_before_publication(
              exhausted_result,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "invalid projection or exhausted JOIN/FILTER/PROJECT published "
      "evidence");
  return passed;
}

// RCP-042-TEST-LIVE-INNER-JOIN-FILTER-PROJECT-SORT-COMPOSITION-V1
bool ValidateInnerJoinFilterProjectSortCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 6 &&
           result.logical_property_count == 1 &&
           result.physical_node_count == 6 &&
           result.canonical_result_column_count == 2 &&
           result.canonical_result_row_count == 4 &&
           result.api_result.result_shape.columns.size() == 2 &&
           result.api_result.result_shape.rows.size() == 4;
  };

  const auto first = dispatch(InnerJoinFilterProjectSortValuesEnvelope());
  const auto repeated =
      dispatch(InnerJoinFilterProjectSortValuesEnvelope());
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      completed(first) &&
          rows[0].fields[0].first == "safe_quotient" &&
          rows[0].fields[0].second.encoded_value == "5" &&
          rows[0].fields[1].first == "right_t" &&
          rows[0].fields[1].second.encoded_value == "b" &&
          rows[1].fields[0].second.encoded_value == "5" &&
          rows[1].fields[1].second.encoded_value == "a" &&
          rows[2].fields[0].second.encoded_value == "10" &&
          rows[2].fields[1].second.encoded_value == "b" &&
          rows[3].fields[0].second.encoded_value == "10" &&
          rows[3].fields[1].second.encoded_value == "a",
      "INNER JOIN/FILTER/PROJECT/SORT did not order the safe projected "
      "joined rows");
  passed &= Require(
      completed(repeated) &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "INNER JOIN/FILTER/PROJECT/SORT changed deterministic plan/result "
      "bytes");

  auto source_only_order = InnerJoinFilterProjectSortValuesEnvelope();
  for (auto& operand : source_only_order.operands) {
    if (operand.type == "relational_node_binding_v1" &&
        operand.name == "6") {
      operand.value =
          "736f72742e72657175697265642d6f726465722e7631|6|-|"
          "019f0000-0000-7200-8000-00000000a201|"
          "019f0000-0000-7200-8000-00000000a201";
    } else if (operand.type == "relational_property_v1") {
      operand.value = "1|6|-|6:1:2:-|-|-";
    }
  }
  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 19;
  const auto source_only_result = dispatch(std::move(source_only_order));
  const auto exhausted_result = dispatch(
      InnerJoinFilterProjectSortValuesEnvelope(),
      std::move(bounded_context));
  const auto refused_before_publication = [](const auto& result,
                                             const std::string_view code) {
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(result, code);
  };
  passed &= Require(
      refused_before_publication(
          source_only_result,
          "QOW-DIAG-RELATIONAL-LIVE-JOIN-FILTER-PROJECT-PAYLOAD-V1") &&
          refused_before_publication(
              exhausted_result,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "invalid ordering or exhausted JOIN/FILTER/PROJECT/SORT published "
      "evidence");
  return passed;
}

// RCP-043-TEST-LIVE-INNER-JOIN-FILTER-PROJECT-SORT-LIMIT-COMPOSITION-V1
bool ValidateInnerJoinFilterProjectSortLimitCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 7 &&
           result.logical_property_count == 1 &&
           result.physical_node_count == 7 &&
           result.canonical_result_column_count == 2 &&
           result.canonical_result_row_count == 1 &&
           result.api_result.result_shape.columns.size() == 2 &&
           result.api_result.result_shape.rows.size() == 1;
  };

  const auto limited =
      dispatch(InnerJoinFilterProjectSortLimitValuesEnvelope());
  const auto repeated =
      dispatch(InnerJoinFilterProjectSortLimitValuesEnvelope());
  if (!limited.api_result.ok) {
    for (const auto& diagnostic : limited.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  const auto& rows = limited.api_result.result_shape.rows;
  passed &= Require(
      completed(limited) &&
          rows[0].fields[0].first == "safe_quotient" &&
          rows[0].fields[0].second.encoded_value == "5" &&
          rows[0].fields[1].first == "right_t" &&
          rows[0].fields[1].second.encoded_value == "b",
      "INNER JOIN/FILTER/PROJECT/SORT/LIMIT did not retain the first "
      "projected sorted row");
  passed &= Require(
      completed(repeated) &&
          repeated.selected_plan_uuid == limited.selected_plan_uuid &&
          repeated.canonical_result_bytes == limited.canonical_result_bytes,
      "INNER JOIN/FILTER/PROJECT/SORT/LIMIT changed deterministic "
      "plan/result bytes");

  auto negative_limit = InnerJoinFilterProjectSortLimitValuesEnvelope();
  for (auto& operand : negative_limit.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "16") {
      operand.value = "1|-|5|-|-|1|-|2d31";
    }
  }
  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 20;
  const auto negative_result = dispatch(std::move(negative_limit));
  const auto exhausted_result = dispatch(
      InnerJoinFilterProjectSortLimitValuesEnvelope(),
      std::move(bounded_context));
  const auto refused_before_publication = [](const auto& result,
                                             const std::string_view code) {
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(result, code);
  };
  passed &= Require(
      refused_before_publication(
          negative_result,
          "QOW-DIAG-RELATIONAL-LIVE-JOIN-FILTER-PROJECT-PAYLOAD-V1") &&
          refused_before_publication(
              exhausted_result,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "negative or exhausted JOIN/FILTER/PROJECT/SORT/LIMIT published "
      "evidence");
  return passed;
}

// RCP-044-TEST-LIVE-INNER-JOIN-FILTER-PROJECT-DISTINCT-SORT-LIMIT-V1
bool ValidateInnerJoinFilterProjectDistinctSortLimitSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 8 &&
           result.logical_property_count == 1 &&
           result.physical_node_count == 8 &&
           result.canonical_result_column_count == 1 &&
           result.canonical_result_row_count == 2 &&
           result.api_result.result_shape.columns.size() == 1 &&
           result.api_result.result_shape.rows.size() == 2;
  };

  const auto distinct_limited =
      dispatch(InnerJoinFilterProjectDistinctSortLimitValuesEnvelope());
  const auto repeated =
      dispatch(InnerJoinFilterProjectDistinctSortLimitValuesEnvelope());
  if (!distinct_limited.api_result.ok) {
    for (const auto& diagnostic : distinct_limited.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  const auto& rows = distinct_limited.api_result.result_shape.rows;
  passed &= Require(
      completed(distinct_limited) &&
          rows[0].fields[0].first == "safe_quotient" &&
          rows[0].fields[0].second.encoded_value == "5" &&
          rows[1].fields[0].second.encoded_value == "10",
      "INNER JOIN/FILTER/PROJECT/DISTINCT/SORT/LIMIT did not eliminate "
      "projected duplicates before row limiting");
  passed &= Require(
      completed(repeated) &&
          repeated.selected_plan_uuid ==
              distinct_limited.selected_plan_uuid &&
          repeated.canonical_result_bytes ==
              distinct_limited.canonical_result_bytes,
      "INNER JOIN/FILTER/PROJECT/DISTINCT/SORT/LIMIT changed deterministic "
      "plan/result bytes");

  auto source_only_distinct =
      InnerJoinFilterProjectDistinctSortLimitValuesEnvelope();
  for (auto& operand : source_only_distinct.operands) {
    if (operand.type == "relational_node_binding_v1" &&
        operand.name == "6") {
      operand.value =
          "6167677265676174652e71756572792d64697374696e63742e7631|6|-|-|-";
    }
  }
  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 16;
  const auto source_only_result =
      dispatch(std::move(source_only_distinct));
  const auto exhausted_result = dispatch(
      InnerJoinFilterProjectDistinctSortLimitValuesEnvelope(),
      std::move(bounded_context));
  const auto refused_before_publication = [](const auto& result,
                                             const std::string_view code) {
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(result, code);
  };
  passed &= Require(
      refused_before_publication(
          source_only_result,
          "QOW-DIAG-RELATIONAL-LIVE-JOIN-FILTER-PROJECT-PAYLOAD-V1") &&
          refused_before_publication(
              exhausted_result,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "invalid DISTINCT or exhausted joined SQL tail published evidence");
  return passed;
}

// RCP-045-TEST-LIVE-INNER-JOIN-FILTER-PROJECT-DISTINCT-SORT-OFFSET-FETCH-V1
bool ValidateInnerJoinFilterProjectDistinctSortOffsetFetchSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 8 &&
           result.logical_property_count == 1 &&
           result.physical_node_count == 8 &&
           result.canonical_result_column_count == 1 &&
           result.canonical_result_row_count == 1 &&
           result.api_result.result_shape.columns.size() == 1 &&
           result.api_result.result_shape.rows.size() == 1;
  };
  const auto refused_before_publication = [](const auto& result,
                                             const std::string_view code) {
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(result, code);
  };

  bool passed = true;
  std::array<std::string, 2> selected_plan_uuids;
  for (std::size_t profile = 0; profile < 2; ++profile) {
    const bool fetch_first_rows_only = profile == 1;
    const auto first = dispatch(
        InnerJoinFilterProjectDistinctSortOffsetValuesEnvelope(
            fetch_first_rows_only));
    const auto repeated = dispatch(
        InnerJoinFilterProjectDistinctSortOffsetValuesEnvelope(
            fetch_first_rows_only));
    if (!first.api_result.ok) {
      for (const auto& diagnostic : first.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    selected_plan_uuids[profile] = first.selected_plan_uuid;
    passed &= Require(
        completed(first) &&
            first.api_result.result_shape.rows[0].fields[0].first ==
                "safe_quotient" &&
            first.api_result.result_shape.rows[0]
                    .fields[0]
                    .second.encoded_value == "10",
        "joined DISTINCT OFFSET/FETCH did not skip the first unique sorted "
        "row");
    passed &= Require(
        completed(repeated) &&
            repeated.selected_plan_uuid == first.selected_plan_uuid &&
            repeated.canonical_result_bytes == first.canonical_result_bytes,
        "joined DISTINCT OFFSET/FETCH changed deterministic plan/result "
        "bytes");

    auto negative_offset =
        InnerJoinFilterProjectDistinctSortOffsetValuesEnvelope(
            fetch_first_rows_only);
    for (auto& operand : negative_offset.operands) {
      if (operand.type == "relational_expression_v1" &&
          operand.name == "17") {
        operand.value = "1|-|5|-|-|1|-|2d31";
      }
    }
    auto bounded_context = Context();
    bounded_context.optimizer_maximum_candidate_count = 17;
    const auto negative_result = dispatch(std::move(negative_offset));
    const auto exhausted_result = dispatch(
        InnerJoinFilterProjectDistinctSortOffsetValuesEnvelope(
            fetch_first_rows_only),
        std::move(bounded_context));
    passed &= Require(
        refused_before_publication(
            negative_result,
            "QOW-DIAG-RELATIONAL-LIVE-JOIN-FILTER-PROJECT-PAYLOAD-V1") &&
            refused_before_publication(
                exhausted_result,
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
        "negative offset or exhausted joined DISTINCT OFFSET/FETCH "
        "published evidence");
  }
  passed &= Require(
      !selected_plan_uuids[0].empty() &&
          !selected_plan_uuids[1].empty() &&
          selected_plan_uuids[0] != selected_plan_uuids[1],
      "joined LIMIT/OFFSET and FETCH FIRST selected the same plan identity");
  return passed;
}

bool ValidateFilterValuesSpine() {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), FilterValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), FilterValuesEnvelope(), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached && first.canonical_result_published &&
          first.api_result.ok && first.diagnostics.empty() &&
          first.logical_node_count == 2 && first.physical_node_count == 2 &&
          first.canonical_result_column_count == 1 &&
          first.canonical_result_row_count == 3,
      "VALUES FILTER did not traverse the selected two-node DAG");
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      first.api_result.result_shape.columns.size() == 1 && rows.size() == 3 &&
          rows[0].fields.size() == 1 && rows[0].fields[0].first == "n" &&
          rows[0].fields[0].second.encoded_value == "1" &&
          rows[1].fields[0].second.encoded_value == "2" &&
          rows[2].fields[0].second.state == api::EngineValueState::sql_null,
      "FILTER TRUE did not preserve typed input order and NULL state");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical FILTER input changed canonical plan/result bytes");
  return passed;
}

bool ValidateFilterThreeValuedPredicate() {
  auto false_predicate = FilterValuesEnvelope();
  auto unknown_predicate = FilterValuesEnvelope();
  for (auto& operand : false_predicate.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "4") {
      operand.value = "1|-|2|-|-|6|-|46414c5345";
    }
  }
  for (auto& operand : unknown_predicate.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "4") {
      operand.value = "1|-|2|-|-|7|-|2d";
    }
  }
  const auto false_result = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), std::move(false_predicate), {}});
  const auto unknown_result = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), std::move(unknown_predicate), {}});
  const auto empty_success = [](const sblr::SblrDispatchResult& result) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.logical_node_count == 2 && result.physical_node_count == 2 &&
           result.canonical_result_column_count == 1 &&
           result.canonical_result_row_count == 0 &&
           result.api_result.result_shape.columns.size() == 1 &&
           result.api_result.result_shape.rows.empty();
  };
  return Require(empty_success(false_result) && empty_success(unknown_result),
                 "FILTER FALSE/UNKNOWN did not produce a typed empty result");
}

// RCP-031-TEST-LIVE-ROW-DEPENDENT-FILTER-PREDICATE-V1
bool ValidateRowDependentFilterPredicateSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result,
                            const std::size_t rows) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 2 &&
           result.physical_node_count == 2 &&
           result.canonical_result_column_count == 1 &&
           result.canonical_result_row_count == rows &&
           result.api_result.result_shape.columns.size() == 1 &&
           result.api_result.result_shape.rows.size() == rows;
  };

  const auto filtered = dispatch(RowDependentFilterValuesEnvelope());
  const auto repeated = dispatch(RowDependentFilterValuesEnvelope());
  bool passed = true;
  passed &= Require(
      completed(filtered, 1) &&
          filtered.api_result.result_shape.rows[0].fields[0]
                  .second.encoded_value == "2",
      "row-dependent FILTER did not retain only its TRUE row");
  passed &= Require(
      completed(repeated, 1) &&
          repeated.selected_plan_uuid == filtered.selected_plan_uuid &&
          repeated.canonical_result_bytes == filtered.canonical_result_bytes,
      "row-dependent FILTER changed deterministic plan/result bytes");

  auto unbound = RowDependentFilterValuesEnvelope();
  for (auto& operand : unbound.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "5") {
      operand.value =
          "3|-|2|-|019f0000-0000-7500-8000-000000008611|-|-|-";
    }
  }
  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 2;
  const auto unbound_result = dispatch(std::move(unbound));
  const auto exhausted_result = dispatch(RowDependentFilterValuesEnvelope(),
                                         std::move(bounded_context));
  const auto refused_before_publication = [](const auto& result,
                                             const std::string_view code) {
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(result, code);
  };
  passed &= Require(
      refused_before_publication(
          unbound_result, "QOW-DIAG-RELATIONAL-LIVE-FILTER-PAYLOAD-V1") &&
          refused_before_publication(
              exhausted_result,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "unbound or resource-exhausted row FILTER published partial evidence");
  return passed;
}

bool ValidateFilterRefusalIsAtomic() {
  auto invalid_output = FilterValuesEnvelope();
  auto unbound_identifier = FilterValuesEnvelope();
  for (auto& operand : invalid_output.operands) {
    if (operand.type == "relational_node_v1" && operand.name == "2") {
      operand.value = "2|0|1|2|-";
    }
  }
  for (auto& operand : unbound_identifier.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "4") {
      operand.value =
          "3|-|2|-|019f0000-0000-7500-8000-000000008605|-|-|-";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-FILTER-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(invalid_output)) &&
          refused_atomically(std::move(unbound_identifier)),
      "invalid or source-unbound FILTER published partial evidence");
}

bool ValidateProjectValuesSpine() {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), ProjectValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), ProjectValuesEnvelope(), {}});
  auto dropped_envelope = ProjectValuesEnvelope();
  for (auto& operand : dropped_envelope.operands) {
    if (operand.type == "relational_node_v1" && operand.name == "2") {
      operand.value = "3|0|1|2|-";
    }
  }
  const auto dropped = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), std::move(dropped_envelope), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached && first.canonical_result_published &&
          first.api_result.ok && first.diagnostics.empty() &&
          first.logical_node_count == 2 && first.physical_node_count == 2 &&
          first.canonical_result_column_count == 2 &&
          first.canonical_result_row_count == 2,
      "VALUES PROJECT did not traverse the selected two-node DAG");
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      first.api_result.result_shape.columns.size() == 2 && rows.size() == 2 &&
          rows[0].fields.size() == 2 && rows[0].fields[0].first == "label" &&
          rows[0].fields[1].first == "id" &&
          rows[0].fields[0].second.encoded_value == "alpha" &&
          rows[0].fields[1].second.encoded_value == "1" &&
          rows[1].fields[0].second.state == api::EngineValueState::sql_null &&
          rows[1].fields[1].second.encoded_value == "2",
      "PROJECT did not reorder typed columns or preserve SQL NULL");
  passed &= Require(
      dropped.api_result.ok && dropped.canonical_result_column_count == 1 &&
          dropped.canonical_result_row_count == 2 &&
          dropped.api_result.result_shape.columns.size() == 1 &&
          dropped.api_result.result_shape.rows[0].fields.size() == 1 &&
          dropped.api_result.result_shape.rows[0].fields[0].first == "label" &&
          dropped.api_result.result_shape.rows[0]
                  .fields[0]
                  .second.encoded_value == "alpha" &&
          dropped.api_result.result_shape.rows[1].fields[0].second.state ==
              api::EngineValueState::sql_null,
      "descriptor-direct PROJECT did not drop the unselected column");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical PROJECT input changed canonical plan/result bytes");
  return passed;
}

bool ValidateProjectRefusalIsAtomic() {
  auto root_lineage = ProjectValuesEnvelope();
  root_lineage.operands.push_back(
      {"relational_output_v1", "3", "2|2|2|1|0|6c6162656c"});
  root_lineage.operands.push_back(
      {"relational_output_v1", "4", "2|1|1|1|1|6964"});
  auto bound_expression = ProjectValuesEnvelope();
  bound_expression.operands.push_back(
      {"relational_expression_v1", "5",
       "3|-|2|-|019f0000-0000-7500-8000-000000008705|-|-|-"});
  for (auto& operand : bound_expression.operands) {
    if (operand.type == "relational_node_binding_v1" &&
        operand.name == "2") {
      operand.value =
          "70726f6a6563742e73656c6563742d6c6973742e7631|5|-|-|-";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-PROJECT-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(root_lineage)) &&
          refused_atomically(std::move(bound_expression)),
      "lineage-bearing or expression-bound PROJECT published partial evidence");
}

// RCP-032-TEST-LIVE-ROW-DEPENDENT-PROJECT-EXPRESSION-V1
bool ValidateRowDependentProjectExpressionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 2 &&
           result.physical_node_count == 2 &&
           result.canonical_result_column_count == 2 &&
           result.canonical_result_row_count == 3 &&
           result.api_result.result_shape.columns.size() == 2 &&
           result.api_result.result_shape.rows.size() == 3;
  };

  const auto projected = dispatch(RowDependentProjectValuesEnvelope());
  const auto repeated = dispatch(RowDependentProjectValuesEnvelope());
  bool passed = true;
  passed &= Require(
      completed(projected) &&
          projected.api_result.result_shape.rows[0].fields[0].first ==
              "id_plus_ten" &&
          projected.api_result.result_shape.rows[0].fields[1].first ==
              "label" &&
          projected.api_result.result_shape.rows[0].fields[0]
                  .second.encoded_value == "11" &&
          projected.api_result.result_shape.rows[0].fields[1]
                  .second.encoded_value == "alpha" &&
          projected.api_result.result_shape.rows[1].fields[0]
                  .second.encoded_value == "12" &&
          projected.api_result.result_shape.rows[1].fields[1].second.state ==
              api::EngineValueState::sql_null &&
          projected.api_result.result_shape.rows[2].fields[0]
                  .second.encoded_value == "13" &&
          projected.api_result.result_shape.rows[2].fields[1]
                  .second.encoded_value == "beta",
      "row-dependent PROJECT did not evaluate arithmetic or preserve NULL");
  passed &= Require(
      completed(repeated) &&
          repeated.selected_plan_uuid == projected.selected_plan_uuid &&
          repeated.canonical_result_bytes == projected.canonical_result_bytes,
      "row-dependent PROJECT changed deterministic plan/result bytes");

  auto unbound = RowDependentProjectValuesEnvelope();
  for (auto& operand : unbound.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "6") {
      operand.value =
          "3|-|3|-|019f0000-0000-7500-8000-000000008707|-|-|-";
    }
  }
  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 2;
  const auto unbound_result = dispatch(std::move(unbound));
  const auto exhausted_result = dispatch(RowDependentProjectValuesEnvelope(),
                                         std::move(bounded_context));
  const auto refused_before_publication = [](const auto& result,
                                             const std::string_view code) {
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(result, code);
  };
  passed &= Require(
      refused_before_publication(
          unbound_result, "QOW-DIAG-RELATIONAL-LIVE-PROJECT-PAYLOAD-V1") &&
          refused_before_publication(
              exhausted_result,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "unbound or resource-exhausted row PROJECT published partial evidence");
  return passed;
}

// RCP-034-TEST-LIVE-PROJECT-SORT-COMPOSITION-V1
bool ValidateProjectedExpressionSortCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 3 &&
           result.logical_property_count == 1 &&
           result.physical_node_count == 3 &&
           result.canonical_result_column_count == 2 &&
           result.canonical_result_row_count == 3 &&
           result.api_result.result_shape.columns.size() == 2 &&
           result.api_result.result_shape.rows.size() == 3;
  };

  const auto sorted = dispatch(ProjectedExpressionSortValuesEnvelope());
  const auto repeated = dispatch(ProjectedExpressionSortValuesEnvelope());
  bool passed = true;
  if (!sorted.api_result.ok) {
    for (const auto& diagnostic : sorted.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  const auto& rows = sorted.api_result.result_shape.rows;
  passed &= Require(
      completed(sorted) &&
          rows[0].fields[0].second.encoded_value == "13" &&
          rows[0].fields[1].second.encoded_value == "beta" &&
          rows[1].fields[0].second.encoded_value == "12" &&
          rows[1].fields[1].second.state ==
              api::EngineValueState::sql_null &&
          rows[2].fields[0].second.encoded_value == "11" &&
          rows[2].fields[1].second.encoded_value == "alpha",
      "PROJECT/SORT did not order the materialized SELECT-list expression");
  passed &= Require(
      completed(repeated) &&
          repeated.selected_plan_uuid == sorted.selected_plan_uuid &&
          repeated.canonical_result_bytes == sorted.canonical_result_bytes,
      "PROJECT/SORT changed deterministic plan/result bytes");

  auto source_only_order = ProjectedExpressionSortValuesEnvelope();
  for (auto& operand : source_only_order.operands) {
    if (operand.type == "relational_node_binding_v1" &&
        operand.name == "3") {
      operand.value =
          "736f72742e72657175697265642d6f726465722e7631|6|-|"
          "019f0000-0000-7200-8000-000000008712|"
          "019f0000-0000-7200-8000-000000008712";
    } else if (operand.type == "relational_property_v1") {
      operand.value = "1|3|-|6:2:2:-|-|-";
    }
  }
  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 3;
  const auto source_only_result = dispatch(std::move(source_only_order));
  const auto exhausted_result = dispatch(
      ProjectedExpressionSortValuesEnvelope(), std::move(bounded_context));
  const auto refused_before_publication = [](const auto& result,
                                             const std::string_view code) {
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(result, code);
  };
  passed &= Require(
      refused_before_publication(
          source_only_result,
          "QOW-DIAG-RELATIONAL-LIVE-PROJECT-SORT-PAYLOAD-V1") &&
          refused_before_publication(
              exhausted_result,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "source-only or resource-exhausted PROJECT/SORT published evidence");
  return passed;
}

// RCP-035-TEST-LIVE-FILTER-PROJECT-COMPOSITION-V1
bool ValidateFilteredExpressionProjectCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 3 &&
           result.physical_node_count == 3 &&
           result.canonical_result_column_count == 2 &&
           result.canonical_result_row_count == 2 &&
           result.api_result.result_shape.columns.size() == 2 &&
           result.api_result.result_shape.rows.size() == 2;
  };

  const auto projected = dispatch(FilteredExpressionProjectValuesEnvelope());
  const auto repeated = dispatch(FilteredExpressionProjectValuesEnvelope());
  bool passed = true;
  if (!projected.api_result.ok) {
    for (const auto& diagnostic : projected.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  const auto& rows = projected.api_result.result_shape.rows;
  passed &= Require(
      completed(projected) &&
          rows[0].fields[0].first == "safe_quotient" &&
          rows[0].fields[0].second.encoded_value == "10" &&
          rows[0].fields[1].second.state ==
              api::EngineValueState::sql_null &&
          rows[1].fields[0].second.encoded_value == "5" &&
          rows[1].fields[1].second.encoded_value == "beta",
      "FILTER/PROJECT evaluated a rejected divide-by-zero row or changed "
      "the surviving projection");
  passed &= Require(
      completed(repeated) &&
          repeated.selected_plan_uuid == projected.selected_plan_uuid &&
          repeated.canonical_result_bytes == projected.canonical_result_bytes,
      "FILTER/PROJECT changed deterministic plan/result bytes");

  auto unbound_predicate = FilteredExpressionProjectValuesEnvelope();
  for (auto& operand : unbound_predicate.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "12") {
      operand.value =
          "3|-|3|-|019f0000-0000-7500-8000-000000008a03|-|-|-";
    }
  }
  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 6;
  const auto unbound_result = dispatch(std::move(unbound_predicate));
  const auto exhausted_result = dispatch(
      FilteredExpressionProjectValuesEnvelope(), std::move(bounded_context));
  const auto refused_before_publication = [](const auto& result,
                                             const std::string_view code) {
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(result, code);
  };
  passed &= Require(
      refused_before_publication(
          unbound_result,
          "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-PAYLOAD-V1") &&
          refused_before_publication(
              exhausted_result,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "unbound or resource-exhausted FILTER/PROJECT published evidence");
  return passed;
}

// RCP-036-TEST-LIVE-FILTER-PROJECT-SORT-COMPOSITION-V1
bool ValidateFilteredProjectedSortCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 4 &&
           result.logical_property_count == 1 &&
           result.physical_node_count == 4 &&
           result.canonical_result_column_count == 2 &&
           result.canonical_result_row_count == 2 &&
           result.api_result.result_shape.columns.size() == 2 &&
           result.api_result.result_shape.rows.size() == 2;
  };

  const auto sorted = dispatch(FilteredProjectedSortValuesEnvelope());
  const auto repeated = dispatch(FilteredProjectedSortValuesEnvelope());
  bool passed = true;
  if (!sorted.api_result.ok) {
    for (const auto& diagnostic : sorted.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  const auto& rows = sorted.api_result.result_shape.rows;
  passed &= Require(
      completed(sorted) &&
          rows[0].fields[0].first == "safe_quotient" &&
          rows[0].fields[0].second.encoded_value == "5" &&
          rows[0].fields[1].second.encoded_value == "beta" &&
          rows[1].fields[0].second.encoded_value == "10" &&
          rows[1].fields[1].second.state ==
              api::EngineValueState::sql_null,
      "FILTER/PROJECT/SORT did not sort only the safe projected rows");
  passed &= Require(
      completed(repeated) &&
          repeated.selected_plan_uuid == sorted.selected_plan_uuid &&
          repeated.canonical_result_bytes == sorted.canonical_result_bytes,
      "FILTER/PROJECT/SORT changed deterministic plan/result bytes");

  auto source_only_order = FilteredProjectedSortValuesEnvelope();
  for (auto& operand : source_only_order.operands) {
    if (operand.type == "relational_node_binding_v1" &&
        operand.name == "4") {
      operand.value =
          "736f72742e72657175697265642d6f726465722e7631|6|-|"
          "019f0000-0000-7200-8000-000000008b01|"
          "019f0000-0000-7200-8000-000000008b01";
    } else if (operand.type == "relational_property_v1") {
      operand.value = "1|4|-|6:1:2:-|-|-";
    }
  }
  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 6;
  const auto source_only_result = dispatch(std::move(source_only_order));
  const auto exhausted_result = dispatch(
      FilteredProjectedSortValuesEnvelope(), std::move(bounded_context));
  const auto refused_before_publication = [](const auto& result,
                                             const std::string_view code) {
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(result, code);
  };
  passed &= Require(
      refused_before_publication(
          source_only_result,
          "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-PAYLOAD-V1") &&
          refused_before_publication(
              exhausted_result,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "source-only or exhausted FILTER/PROJECT/SORT published evidence");
  return passed;
}

// RCP-037-TEST-LIVE-FILTER-PROJECT-SORT-LIMIT-COMPOSITION-V1
bool ValidateFilteredProjectedSortLimitCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 5 &&
           result.logical_property_count == 1 &&
           result.physical_node_count == 5 &&
           result.canonical_result_column_count == 2 &&
           result.canonical_result_row_count == 1 &&
           result.api_result.result_shape.columns.size() == 2 &&
           result.api_result.result_shape.rows.size() == 1;
  };

  const auto limited = dispatch(FilteredProjectedSortLimitValuesEnvelope());
  const auto repeated = dispatch(FilteredProjectedSortLimitValuesEnvelope());
  bool passed = true;
  if (!limited.api_result.ok) {
    for (const auto& diagnostic : limited.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  const auto& rows = limited.api_result.result_shape.rows;
  passed &= Require(
      completed(limited) && rows[0].fields[0].first == "safe_quotient" &&
          rows[0].fields[0].second.encoded_value == "5" &&
          rows[0].fields[1].second.encoded_value == "beta",
      "FILTER/PROJECT/SORT/LIMIT did not retain only the first sorted row");
  passed &= Require(
      completed(repeated) &&
          repeated.selected_plan_uuid == limited.selected_plan_uuid &&
          repeated.canonical_result_bytes == limited.canonical_result_bytes,
      "FILTER/PROJECT/SORT/LIMIT changed deterministic plan/result bytes");

  auto negative_limit = FilteredProjectedSortLimitValuesEnvelope();
  for (auto& operand : negative_limit.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "15") {
      operand.value = "1|-|5|-|-|1|-|2d31";
    }
  }
  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 7;
  const auto negative_result = dispatch(std::move(negative_limit));
  const auto exhausted_result = dispatch(
      FilteredProjectedSortLimitValuesEnvelope(),
      std::move(bounded_context));
  const auto refused_before_publication = [](const auto& result,
                                             const std::string_view code) {
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(result, code);
  };
  passed &= Require(
      refused_before_publication(
          negative_result,
          "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-PAYLOAD-V1") &&
          refused_before_publication(
              exhausted_result,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "negative or exhausted FILTER/PROJECT/SORT/LIMIT published evidence");
  return passed;
}

// RCP-038-TEST-LIVE-FILTER-PROJECT-DISTINCT-SORT-LIMIT-COMPOSITION-V1
bool ValidateFilteredProjectedDistinctSortLimitCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 6 &&
           result.logical_property_count == 1 &&
           result.physical_node_count == 6 &&
           result.canonical_result_column_count == 1 &&
           result.canonical_result_row_count == 2 &&
           result.api_result.result_shape.columns.size() == 1 &&
           result.api_result.result_shape.rows.size() == 2;
  };

  const auto distinct =
      dispatch(FilteredProjectedDistinctSortLimitValuesEnvelope());
  const auto repeated =
      dispatch(FilteredProjectedDistinctSortLimitValuesEnvelope());
  bool passed = true;
  if (!distinct.api_result.ok) {
    for (const auto& diagnostic : distinct.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  const auto& rows = distinct.api_result.result_shape.rows;
  passed &= Require(
      completed(distinct) &&
          rows[0].fields[0].first == "safe_quotient" &&
          rows[0].fields[0].second.encoded_value == "5" &&
          rows[1].fields[0].second.encoded_value == "10",
      "FILTER/PROJECT/DISTINCT/SORT/LIMIT retained a projected duplicate or "
      "changed SQL-tail order");
  passed &= Require(
      completed(repeated) &&
          repeated.selected_plan_uuid == distinct.selected_plan_uuid &&
          repeated.canonical_result_bytes == distinct.canonical_result_bytes,
      "FILTER/PROJECT/DISTINCT/SORT/LIMIT changed deterministic bytes");

  auto duplicate_coverage =
      FilteredProjectedDistinctSortLimitValuesEnvelope();
  for (auto& operand : duplicate_coverage.operands) {
    if (operand.type == "relational_node_binding_v1" &&
        operand.name == "4") {
      operand.value =
          "6167677265676174652e71756572792d64697374696e63742e7631|7|-|-|-";
    }
  }
  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 7;
  const auto duplicate_result = dispatch(std::move(duplicate_coverage));
  const auto exhausted_result = dispatch(
      FilteredProjectedDistinctSortLimitValuesEnvelope(),
      std::move(bounded_context));
  const auto refused_before_publication = [](const auto& result,
                                             const std::string_view code) {
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(result, code);
  };
  passed &= Require(
      refused_before_publication(
          duplicate_result,
          "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-DISTINCT-SORT-LIMIT-PAYLOAD-V1") &&
          refused_before_publication(
              exhausted_result,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "invalid DISTINCT or exhausted full SQL tail published evidence");
  return passed;
}

// RCP-039-TEST-LIVE-FILTER-PROJECT-DISTINCT-SORT-OFFSET-FETCH-COMPOSITION-V1
bool ValidateFilteredProjectedDistinctSortOffsetFetchCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 6 &&
           result.logical_property_count == 1 &&
           result.physical_node_count == 6 &&
           result.canonical_result_column_count == 1 &&
           result.canonical_result_row_count == 1 &&
           result.api_result.result_shape.columns.size() == 1 &&
           result.api_result.result_shape.rows.size() == 1;
  };

  bool passed = true;
  std::string limit_selected_plan;
  std::string fetch_selected_plan;
  for (const bool fetch_first_rows_only : {false, true}) {
    const auto first = dispatch(
        FilteredProjectedDistinctSortOffsetValuesEnvelope(
            fetch_first_rows_only));
    const auto repeated = dispatch(
        FilteredProjectedDistinctSortOffsetValuesEnvelope(
            fetch_first_rows_only));
    if (!first.api_result.ok) {
      for (const auto& diagnostic : first.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    const auto& rows = first.api_result.result_shape.rows;
    passed &= Require(
        completed(first) &&
            rows[0].fields[0].first == "safe_quotient" &&
            rows[0].fields[0].second.encoded_value == "10",
        std::string(fetch_first_rows_only ? "FETCH" : "LIMIT/OFFSET") +
            " did not eliminate projected duplicates before ordering and "
            "offset/count application");
    passed &= Require(
        completed(repeated) &&
            repeated.selected_plan_uuid == first.selected_plan_uuid &&
            repeated.canonical_result_bytes == first.canonical_result_bytes,
        std::string(fetch_first_rows_only ? "FETCH" : "LIMIT/OFFSET") +
            " changed deterministic plan/result bytes");
    if (fetch_first_rows_only) {
      fetch_selected_plan = first.selected_plan_uuid;
    } else {
      limit_selected_plan = first.selected_plan_uuid;
    }
  }
  passed &= Require(
      !limit_selected_plan.empty() && !fetch_selected_plan.empty() &&
          limit_selected_plan != fetch_selected_plan,
      "full-tail LIMIT/OFFSET and FETCH collapsed to one selected plan");

  auto negative_offset =
      FilteredProjectedDistinctSortOffsetValuesEnvelope(true);
  for (auto& operand : negative_offset.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "18") {
      operand.value = "1|-|5|-|-|1|-|2d31";
    }
  }
  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 8;
  const auto negative_result = dispatch(std::move(negative_offset));
  const auto exhausted_result = dispatch(
      FilteredProjectedDistinctSortOffsetValuesEnvelope(false),
      std::move(bounded_context));
  const auto refused_before_publication = [](const auto& result,
                                             const std::string_view code) {
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(result, code);
  };
  passed &= Require(
      refused_before_publication(
          negative_result,
          "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-DISTINCT-SORT-LIMIT-PAYLOAD-V1") &&
          refused_before_publication(
              exhausted_result,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "negative OFFSET or exhausted full FETCH tail published evidence");
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-UNARY-COMPOSITION-V1
bool ValidateNodeDrivenUnaryCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 6 &&
           result.logical_property_count == 1 &&
           result.physical_node_count == 6 &&
           result.canonical_result_column_count == 1 &&
           result.canonical_result_row_count == 1 &&
           result.api_result.result_shape.columns.size() == 1 &&
           result.api_result.result_shape.rows.size() == 1;
  };

  const auto first = dispatch(NodeDrivenUnaryCompositionEnvelope());
  const auto repeated = dispatch(NodeDrivenUnaryCompositionEnvelope());
  bool passed = true;
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  passed &= Require(
      completed(first) &&
          first.api_result.result_shape.rows[0].fields[0].first ==
              "safe_quotient" &&
          first.api_result.result_shape.rows[0]
                  .fields[0]
                  .second.encoded_value == "10",
      "node-driven LIMIT/FILTER/PROJECT/DISTINCT/SORT did not execute "
      "dependency order and canonical semantics");
  passed &= Require(
      completed(repeated) &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "node-driven unary composition changed deterministic plan/result bytes");

  auto malformed = NodeDrivenUnaryCompositionEnvelope();
  for (auto& operand : malformed.operands) {
    if (operand.type == "relational_node_v1" && operand.name == "2") {
      operand.value = "2|0|999|1,2|-";
    }
  }
  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 10;
  const auto malformed_result = dispatch(std::move(malformed));
  const auto exhausted_result = dispatch(
      NodeDrivenUnaryCompositionEnvelope(), std::move(bounded_context));
  const auto no_publication = [](const auto& result) {
    return !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty();
  };
  passed &= Require(
      no_publication(malformed_result) &&
          no_publication(exhausted_result) &&
          HasApiDiagnostic(
              exhausted_result,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "malformed or resource-exhausted node-driven composition published "
      "partial evidence");
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-BRANCHING-JOIN-COMPOSITION-V1
bool ValidateNodeDrivenJoinCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 7 &&
           result.logical_property_count == 1 &&
           result.physical_node_count == 7 &&
           result.canonical_result_column_count == 2 &&
           result.canonical_result_row_count == 1 &&
           result.api_result.result_shape.columns.size() == 2 &&
           result.api_result.result_shape.rows.size() == 1;
  };

  const auto first = dispatch(NodeDrivenJoinCompositionEnvelope());
  const auto repeated = dispatch(NodeDrivenJoinCompositionEnvelope());
  bool passed = true;
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  passed &= Require(
      completed(first) &&
          first.api_result.result_shape.rows[0].fields[0].first ==
              "safe_quotient" &&
          first.api_result.result_shape.rows[0]
                  .fields[0]
                  .second.encoded_value == "10" &&
          first.api_result.result_shape.rows[0].fields[1].first ==
              "right_t" &&
          first.api_result.result_shape.rows[0]
                  .fields[1]
                  .second.encoded_value == "b",
      "node-driven JOIN/LIMIT/FILTER/PROJECT/SORT did not execute branch "
      "dependencies and canonical semantics");
  passed &= Require(
      completed(repeated) &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "node-driven branching composition changed deterministic plan/result "
      "bytes");

  auto malformed = NodeDrivenJoinCompositionEnvelope();
  for (auto& operand : malformed.operands) {
    if (operand.type == "relational_node_v1" && operand.name == "3") {
      operand.value = "4|0|1,1|1,2|-";
    }
  }
  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 10;
  const auto malformed_result = dispatch(std::move(malformed));
  const auto exhausted_result = dispatch(
      NodeDrivenJoinCompositionEnvelope(), std::move(bounded_context));
  const auto no_publication = [](const auto& result) {
    return !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty();
  };
  passed &= Require(
      no_publication(malformed_result) &&
          no_publication(exhausted_result) &&
          HasApiDiagnostic(
              exhausted_result,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "malformed or resource-exhausted node-driven JOIN composition "
      "published partial evidence");
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-ACCEPTED-JOIN-KINDS-COMPOSITION-V1
bool ValidateNodeDrivenAcceptedJoinKindsCompositionSpine() {
  using Truth = JoinPredicateTruth;
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result,
                            const std::size_t columns,
                            const std::size_t rows) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 4 &&
           result.physical_node_count == 4 &&
           result.canonical_result_column_count == columns &&
           result.canonical_result_row_count == rows &&
           result.api_result.result_shape.columns.size() == columns &&
           result.api_result.result_shape.rows.size() == rows;
  };

  const auto inner = dispatch(NodeDrivenAcceptedJoinLimitEnvelope(
      "6a6f696e2e696e6e65722e7631", "1,2", false, Truth::kTrue,
      false, false, "019f0000-0000-7000-8000-00000000c921"));
  const auto cross = dispatch(NodeDrivenAcceptedJoinLimitEnvelope(
      "6a6f696e2e63726f73732e7631", "1,2", true, Truth::kTrue,
      false, false, "019f0000-0000-7000-8000-00000000c922"));
  const auto left = dispatch(NodeDrivenAcceptedJoinLimitEnvelope(
      "6a6f696e2e6c6566742d6f757465722e7631", "1,2", false,
      Truth::kFalse, false, true,
      "019f0000-0000-7000-8000-00000000c923"));
  const auto right = dispatch(NodeDrivenAcceptedJoinLimitEnvelope(
      "6a6f696e2e72696768742d6f757465722e7631", "1,2", false,
      Truth::kFalse, true, false,
      "019f0000-0000-7000-8000-00000000c924"));
  const auto full = dispatch(NodeDrivenAcceptedJoinLimitEnvelope(
      "6a6f696e2e66756c6c2d6f757465722e7631", "1,2", false,
      Truth::kFalse, true, true,
      "019f0000-0000-7000-8000-00000000c925"));
  const auto semi = dispatch(NodeDrivenAcceptedJoinLimitEnvelope(
      "6a6f696e2e6c6566742d73656d692e7631", "1", false,
      Truth::kTrue, false, false,
      "019f0000-0000-7000-8000-00000000c926"));
  const auto anti = dispatch(NodeDrivenAcceptedJoinLimitEnvelope(
      "6a6f696e2e6c6566742d616e74692e7631", "1", false,
      Truth::kUnknown, false, false,
      "019f0000-0000-7000-8000-00000000c927"));

  for (const auto* result : {&inner, &cross, &left, &right, &full, &semi,
                             &anti}) {
    if (!result->api_result.ok) {
      for (const auto& diagnostic : result->api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
  }
  bool passed = true;
  passed &= Require(completed(inner, 2, 4) && completed(cross, 2, 4),
                    "node-driven INNER/CROSS JOIN composition was incomplete");
  passed &= Require(
      completed(left, 2, 2) &&
          left.api_result.result_shape.rows[0].fields[1].second.state ==
              api::EngineValueState::sql_null &&
          completed(right, 2, 2) &&
          right.api_result.result_shape.rows[0].fields[0].second.state ==
              api::EngineValueState::sql_null &&
          completed(full, 2, 4) &&
          full.api_result.result_shape.rows[0].fields[1].second.state ==
              api::EngineValueState::sql_null &&
          full.api_result.result_shape.rows[2].fields[0].second.state ==
              api::EngineValueState::sql_null,
      "node-driven outer JOIN composition lost typed NULL extension");
  passed &= Require(
      completed(semi, 1, 2) && completed(anti, 1, 2) &&
          semi.api_result.result_shape.rows[0]
                  .fields[0]
                  .second.encoded_value == "1" &&
          anti.api_result.result_shape.rows[1]
                  .fields[0]
                  .second.encoded_value == "2",
      "node-driven LEFT SEMI/ANTI composition lost left-row semantics");
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-UNION-ALL-COMPOSITION-V1
bool ValidateNodeDrivenUnionAllCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 4 &&
           result.logical_property_count == 0 &&
           result.physical_node_count == 4 &&
           result.canonical_result_column_count == 1 &&
           result.canonical_result_row_count == 4 &&
           result.api_result.result_shape.columns.size() == 1 &&
           result.api_result.result_shape.rows.size() == 4;
  };

  const auto first = dispatch(NodeDrivenUnionAllLimitEnvelope());
  const auto repeated = dispatch(NodeDrivenUnionAllLimitEnvelope());
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      completed(first) &&
          rows[0].fields[0].second.encoded_value == "1" &&
          rows[1].fields[0].second.encoded_value == "2" &&
          rows[2].fields[0].second.state ==
              api::EngineValueState::sql_null &&
          rows[3].fields[0].second.encoded_value == "2",
      "node-driven UNION ALL/LIMIT composition lost operand order, "
      "multiplicity, or typed NULL state");
  passed &= Require(
      completed(repeated) &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "node-driven UNION ALL composition changed deterministic plan/result "
      "bytes");

  auto malformed = NodeDrivenUnionAllLimitEnvelope();
  for (auto& operand : malformed.operands) {
    if (operand.type == "relational_node_v1" && operand.name == "3") {
      operand.value = "9|0|1,1|3|-";
    }
  }
  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 11;
  const auto malformed_result = dispatch(std::move(malformed));
  const auto exhausted_result = dispatch(
      NodeDrivenUnionAllLimitEnvelope(), std::move(bounded_context));
  const auto no_publication = [](const auto& result) {
    return !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty();
  };
  passed &= Require(
      no_publication(malformed_result) &&
          no_publication(exhausted_result) &&
          HasApiDiagnostic(
              exhausted_result,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "malformed or resource-exhausted node-driven UNION ALL composition "
      "published partial evidence");
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-EXACT-SET-PROFILE-COMPOSITION-V1
bool ValidateNodeDrivenExactSetProfilesCompositionSpine() {
  struct ProfileExpectation {
    std::string semantic_hex;
    std::string tree_uuid;
    std::vector<std::string> values;
  };
  const std::array<ProfileExpectation, 5> profiles{{
      {"7365742d6f7065726174696f6e2e756e696f6e2d64697374696e63742e7631",
       "019f0000-0000-7000-8000-00000000c940",
       {"1", "2", "null", "3"}},
      {"7365742d6f7065726174696f6e2e696e746572736563742d616c6c2e7631",
       "019f0000-0000-7000-8000-00000000c941", {"2", "null"}},
      {"7365742d6f7065726174696f6e2e696e746572736563742d64697374696e63742e7631",
       "019f0000-0000-7000-8000-00000000c942", {"2", "null"}},
      {"7365742d6f7065726174696f6e2e6578636570742d616c6c2e7631",
       "019f0000-0000-7000-8000-00000000c943", {"1"}},
      {"7365742d6f7065726174696f6e2e6578636570742d64697374696e63742e7631",
       "019f0000-0000-7000-8000-00000000c944", {"1"}},
  }};
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result,
                            const std::size_t rows) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 4 &&
           result.physical_node_count == 4 &&
           result.canonical_result_column_count == 1 &&
           result.canonical_result_row_count == rows &&
           result.api_result.result_shape.rows.size() == rows;
  };

  bool passed = true;
  std::unordered_set<std::string> selected_plans;
  for (const auto& profile : profiles) {
    const auto first = dispatch(NodeDrivenExactSetProfileLimitEnvelope(
        profile.semantic_hex, profile.tree_uuid));
    const auto repeated = dispatch(NodeDrivenExactSetProfileLimitEnvelope(
        profile.semantic_hex, profile.tree_uuid));
    if (!first.api_result.ok) {
      for (const auto& diagnostic : first.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    bool values_match = completed(first, profile.values.size());
    if (values_match) {
      for (std::size_t row = 0; row < profile.values.size(); ++row) {
        const auto& value =
            first.api_result.result_shape.rows[row].fields[0].second;
        values_match &= profile.values[row] == "null"
                            ? value.state == api::EngineValueState::sql_null
                            : value.encoded_value == profile.values[row];
      }
    }
    passed &= Require(
        values_match && completed(repeated, profile.values.size()) &&
            repeated.selected_plan_uuid == first.selected_plan_uuid &&
            repeated.canonical_result_bytes == first.canonical_result_bytes &&
            selected_plans.insert(first.selected_plan_uuid).second,
        "node-driven exact quantified set profile lost semantics, identity, "
        "or deterministic replay");
  }
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-TYPE-RECONCILED-SET-COMPOSITION-V1
bool ValidateNodeDrivenTypeReconciledSetCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto first = dispatch(NodeDrivenTypeReconciledSetLimitEnvelope());
  const auto repeated =
      dispatch(NodeDrivenTypeReconciledSetLimitEnvelope());
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  const auto& rows = first.api_result.result_shape.rows;
  bool values_match =
      first.accepted && first.optimizer_admitted &&
      first.optimizer_selected && first.physical_dag_published &&
      first.physical_dag_executed && first.runtime_actuals_attached &&
      first.canonical_result_published && first.api_result.ok &&
      first.diagnostics.empty() && first.logical_node_count == 4 &&
      first.physical_node_count == 4 &&
      first.canonical_result_column_count == 1 &&
      first.canonical_result_row_count == 4 && rows.size() == 4;
  if (values_match) {
    const std::array<std::string_view, 4> expected{"1", "2", "null", "2"};
    for (std::size_t row = 0; row < expected.size(); ++row) {
      const auto& value = rows[row].fields[0].second;
      values_match &= value.descriptor.canonical_type_name == "int64" &&
                      (expected[row] == "null"
                           ? value.state == api::EngineValueState::sql_null
                           : value.encoded_value == expected[row]);
    }
  }
  bool passed = true;
  passed &= Require(
      values_match && repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "node-driven type-reconciled set composition did not losslessly cast "
      "int8 to int64 before LIMIT or changed deterministic replay");

  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 18;
  const auto exhausted = dispatch(
      NodeDrivenTypeReconciledSetLimitEnvelope(),
      std::move(bounded_context));
  passed &= Require(
      !exhausted.optimizer_selected &&
          !exhausted.physical_dag_published &&
          !exhausted.physical_dag_executed &&
          !exhausted.runtime_actuals_attached &&
          !exhausted.canonical_result_published &&
          !exhausted.api_result.ok && exhausted.physical_node_count == 0 &&
          exhausted.canonical_result_bytes.empty() &&
          HasApiDiagnostic(
              exhausted,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "exhausted type-reconciled set composition published partial "
      "evidence");
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-BY-NAME-SET-COMPOSITION-V1
bool ValidateNodeDrivenByNameSetCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto first = dispatch(NodeDrivenByNameSetLimitEnvelope());
  const auto repeated = dispatch(NodeDrivenByNameSetLimitEnvelope());
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  const auto& rows = first.api_result.result_shape.rows;
  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted &&
          first.optimizer_selected && first.physical_dag_published &&
          first.physical_dag_executed && first.runtime_actuals_attached &&
          first.canonical_result_published && first.api_result.ok &&
          first.diagnostics.empty() && first.logical_node_count == 4 &&
          first.physical_node_count == 4 &&
          first.canonical_result_column_count == 2 &&
          first.canonical_result_row_count == 2 && rows.size() == 2 &&
          rows[0].fields[0].first == "n" &&
          rows[0].fields[1].first == "label" &&
          rows[0].fields[0].second.encoded_value == "1" &&
          rows[0].fields[1].second.encoded_value == "10" &&
          rows[1].fields[0].second.encoded_value == "2" &&
          rows[1].fields[1].second.encoded_value == "20" &&
          repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "node-driven BY NAME set composition lost aligned schema, LIMIT "
      "semantics, or deterministic replay");

  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 15;
  const auto exhausted = dispatch(NodeDrivenByNameSetLimitEnvelope(),
                                  std::move(bounded_context));
  passed &= Require(
      !exhausted.optimizer_selected &&
          !exhausted.physical_dag_published &&
          !exhausted.physical_dag_executed &&
          !exhausted.runtime_actuals_attached &&
          !exhausted.canonical_result_published &&
          !exhausted.api_result.ok && exhausted.physical_node_count == 0 &&
          exhausted.canonical_result_bytes.empty() &&
          HasApiDiagnostic(
              exhausted,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "exhausted node-driven BY NAME set composition published partial "
      "evidence");
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-COUNT-STAR-COMPOSITION-V1
bool ValidateNodeDrivenCountStarCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto first = dispatch(NodeDrivenCountStarLimitEnvelope());
  const auto repeated = dispatch(NodeDrivenCountStarLimitEnvelope());
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted &&
          first.optimizer_selected && first.physical_dag_published &&
          first.physical_dag_executed && first.runtime_actuals_attached &&
          first.canonical_result_published && first.api_result.ok &&
          first.diagnostics.empty() && first.logical_node_count == 3 &&
          first.physical_node_count == 3 &&
          first.canonical_result_column_count == 1 &&
          first.canonical_result_row_count == 1 &&
          first.api_result.result_shape.rows.size() == 1 &&
          first.api_result.result_shape.rows[0]
                  .fields[0]
                  .second.encoded_value == "3" &&
          repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "node-driven COUNT(*) composition lost aggregate value, LIMIT "
      "semantics, or deterministic replay");

  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 6;
  const auto exhausted = dispatch(NodeDrivenCountStarLimitEnvelope(),
                                  std::move(bounded_context));
  passed &= Require(
      !exhausted.optimizer_selected &&
          !exhausted.physical_dag_published &&
          !exhausted.physical_dag_executed &&
          !exhausted.runtime_actuals_attached &&
          !exhausted.canonical_result_published &&
          !exhausted.api_result.ok && exhausted.physical_node_count == 0 &&
          exhausted.canonical_result_bytes.empty() &&
          HasApiDiagnostic(
              exhausted,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "exhausted node-driven COUNT(*) composition published partial "
      "evidence");
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-COUNT-EXPRESSION-COMPOSITION-V1
bool ValidateNodeDrivenCountExpressionCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto first = dispatch(NodeDrivenCountExpressionLimitEnvelope());
  const auto repeated =
      dispatch(NodeDrivenCountExpressionLimitEnvelope());
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted &&
          first.optimizer_selected && first.physical_dag_published &&
          first.physical_dag_executed && first.runtime_actuals_attached &&
          first.canonical_result_published && first.api_result.ok &&
          first.diagnostics.empty() && first.logical_node_count == 3 &&
          first.physical_node_count == 3 &&
          first.canonical_result_column_count == 1 &&
          first.canonical_result_row_count == 1 &&
          first.api_result.result_shape.rows.size() == 1 &&
          first.api_result.result_shape.rows[0]
                  .fields[0]
                  .second.encoded_value == "2" &&
          repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "node-driven COUNT(expression) composition did not exclude typed NULL "
      "before LIMIT or changed deterministic replay");

  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 8;
  const auto exhausted = dispatch(
      NodeDrivenCountExpressionLimitEnvelope(),
      std::move(bounded_context));
  passed &= Require(
      !exhausted.optimizer_selected &&
          !exhausted.physical_dag_published &&
          !exhausted.physical_dag_executed &&
          !exhausted.runtime_actuals_attached &&
          !exhausted.canonical_result_published &&
          !exhausted.api_result.ok && exhausted.physical_node_count == 0 &&
          exhausted.canonical_result_bytes.empty() &&
          HasApiDiagnostic(
              exhausted,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "exhausted node-driven COUNT(expression) composition published "
      "partial evidence");
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-SUM-EXPRESSION-COMPOSITION-V1
bool ValidateNodeDrivenSumExpressionCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto first = dispatch(NodeDrivenSumExpressionLimitEnvelope());
  const auto repeated = dispatch(NodeDrivenSumExpressionLimitEnvelope());
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted &&
          first.optimizer_selected && first.physical_dag_published &&
          first.physical_dag_executed && first.runtime_actuals_attached &&
          first.canonical_result_published && first.api_result.ok &&
          first.diagnostics.empty() && first.logical_node_count == 3 &&
          first.physical_node_count == 3 &&
          first.canonical_result_column_count == 1 &&
          first.canonical_result_row_count == 1 &&
          first.api_result.result_shape.rows.size() == 1 &&
          first.api_result.result_shape.rows[0]
                  .fields[0]
                  .second.encoded_value == "10" &&
          repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "node-driven SUM(expression) composition lost aggregate value, LIMIT "
      "semantics, or deterministic replay");

  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 8;
  const auto exhausted = dispatch(
      NodeDrivenSumExpressionLimitEnvelope(),
      std::move(bounded_context));
  passed &= Require(
      !exhausted.optimizer_selected &&
          !exhausted.physical_dag_published &&
          !exhausted.physical_dag_executed &&
          !exhausted.runtime_actuals_attached &&
          !exhausted.canonical_result_published &&
          !exhausted.api_result.ok && exhausted.physical_node_count == 0 &&
          exhausted.canonical_result_bytes.empty() &&
          HasApiDiagnostic(
              exhausted,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "exhausted node-driven SUM(expression) composition published partial "
      "evidence");
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-AVG-EXPRESSION-COMPOSITION-V1
bool ValidateNodeDrivenAvgExpressionCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto first = dispatch(NodeDrivenAvgExpressionLimitEnvelope());
  const auto repeated = dispatch(NodeDrivenAvgExpressionLimitEnvelope());
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted &&
          first.optimizer_selected && first.physical_dag_published &&
          first.physical_dag_executed && first.runtime_actuals_attached &&
          first.canonical_result_published && first.api_result.ok &&
          first.diagnostics.empty() && first.logical_node_count == 3 &&
          first.physical_node_count == 3 &&
          first.canonical_result_column_count == 1 &&
          first.canonical_result_row_count == 1 &&
          first.api_result.result_shape.rows.size() == 1 &&
          first.api_result.result_shape.rows[0]
                  .fields[0]
                  .second.encoded_value == "5" &&
          repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "node-driven AVG(expression) composition lost canonical average, "
      "LIMIT, or deterministic replay semantics");

  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 8;
  const auto exhausted = dispatch(
      NodeDrivenAvgExpressionLimitEnvelope(),
      std::move(bounded_context));
  passed &= Require(
      !exhausted.optimizer_selected &&
          !exhausted.physical_dag_published &&
          !exhausted.physical_dag_executed &&
          !exhausted.runtime_actuals_attached &&
          !exhausted.canonical_result_published &&
          !exhausted.api_result.ok && exhausted.physical_node_count == 0 &&
          exhausted.canonical_result_bytes.empty() &&
          HasApiDiagnostic(
              exhausted,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "exhausted node-driven AVG(expression) composition published partial "
      "evidence");
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-STATISTICAL-AGGREGATE-COMPOSITION-V1
bool ValidateNodeDrivenStatisticalAggregateCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  constexpr std::array<std::string_view, 6> kFixtureFamilies = {
      "ca1", "ca2", "ca3", "ca4", "ca5", "ca6"};
  bool passed = true;
  for (std::size_t index = 0;
       index < std::size(kStatisticalAggregateProfiles); ++index) {
    const auto& profile = kStatisticalAggregateProfiles[index];
    const auto first = dispatch(NodeDrivenStatisticalAggregateLimitEnvelope(
        profile, kFixtureFamilies[index]));
    const auto repeated = dispatch(
        NodeDrivenStatisticalAggregateLimitEnvelope(
            profile, kFixtureFamilies[index]));
    if (!first.api_result.ok) {
      for (const auto& diagnostic : first.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    bool exact_result =
        first.api_result.result_shape.columns.size() == 1 &&
        first.api_result.result_shape.columns[0].canonical_type_name ==
            "real64" &&
        first.api_result.result_shape.rows.size() == 1 &&
        first.api_result.result_shape.rows[0].fields.size() == 1 &&
        first.api_result.result_shape.rows[0]
                .fields[0]
                .second.state == api::EngineValueState::value;
    if (exact_result) {
      const auto& encoded = first.api_result.result_shape.rows[0]
                                .fields[0]
                                .second.encoded_value;
      char* end = nullptr;
      const double observed = std::strtod(encoded.c_str(), &end);
      exact_result = end == encoded.c_str() + encoded.size() &&
                     std::abs(observed - profile.expected) <= 1e-12;
    }
    passed &= Require(
        first.accepted && first.optimizer_admitted &&
            first.optimizer_selected && first.physical_dag_published &&
            first.physical_dag_executed && first.runtime_actuals_attached &&
            first.canonical_result_published && first.api_result.ok &&
            first.diagnostics.empty() && first.logical_node_count == 3 &&
            first.physical_node_count == 3 &&
            first.canonical_result_column_count == 1 &&
            first.canonical_result_row_count == 1 && exact_result &&
            repeated.api_result.ok &&
            repeated.selected_plan_uuid == first.selected_plan_uuid &&
            repeated.canonical_result_bytes == first.canonical_result_bytes,
        "node-driven " + std::string(profile.name) +
            " composition lost statistical, LIMIT, or deterministic replay "
            "semantics");

    auto bounded_context = Context();
    bounded_context.optimizer_maximum_candidate_count = 8;
    const auto exhausted = dispatch(
        NodeDrivenStatisticalAggregateLimitEnvelope(
            profile, kFixtureFamilies[index]),
        std::move(bounded_context));
    passed &= Require(
        !exhausted.optimizer_selected &&
            !exhausted.physical_dag_published &&
            !exhausted.physical_dag_executed &&
            !exhausted.runtime_actuals_attached &&
            !exhausted.canonical_result_published &&
            !exhausted.api_result.ok && exhausted.physical_node_count == 0 &&
            exhausted.canonical_result_bytes.empty() &&
            HasApiDiagnostic(
                exhausted,
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
        "exhausted node-driven " + std::string(profile.name) +
            " composition published partial evidence");
  }
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-PAIR-STATISTICAL-COMPOSITION-V1
bool ValidateNodeDrivenPairStatisticalCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  constexpr std::array<std::string_view, 12> kFixtureFamilies = {
      "cb0", "cb1", "cb2", "cb3", "cb4", "cb5",
      "cb6", "cb7", "cb8", "cb9", "cba", "cbb"};
  bool passed = true;
  for (std::size_t index = 0;
       index < std::size(kPairStatisticalAggregateProfiles); ++index) {
    const auto& profile = kPairStatisticalAggregateProfiles[index];
    const auto first = dispatch(NodeDrivenPairStatisticalLimitEnvelope(
        profile, kFixtureFamilies[index]));
    const auto repeated = dispatch(NodeDrivenPairStatisticalLimitEnvelope(
        profile, kFixtureFamilies[index]));
    if (!first.api_result.ok) {
      for (const auto& diagnostic : first.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    bool exact_result =
        first.api_result.result_shape.columns.size() == 1 &&
        first.api_result.result_shape.rows.size() == 1 &&
        first.api_result.result_shape.rows[0].fields.size() == 1 &&
        first.api_result.result_shape.rows[0]
                .fields[0]
                .second.state == api::EngineValueState::value;
    if (exact_result) {
      const auto& encoded = first.api_result.result_shape.rows[0]
                                .fields[0]
                                .second.encoded_value;
      char* end = nullptr;
      const double observed = std::strtod(encoded.c_str(), &end);
      exact_result = end == encoded.c_str() + encoded.size() &&
                     std::abs(observed - profile.expected) <= 1e-12;
    }
    passed &= Require(
        first.accepted && first.optimizer_admitted &&
            first.optimizer_selected && first.physical_dag_published &&
            first.physical_dag_executed && first.runtime_actuals_attached &&
            first.canonical_result_published && first.api_result.ok &&
            first.diagnostics.empty() && first.logical_node_count == 3 &&
            first.physical_node_count == 3 &&
            first.canonical_result_column_count == 1 &&
            first.canonical_result_row_count == 1 && exact_result &&
            repeated.api_result.ok &&
            repeated.selected_plan_uuid == first.selected_plan_uuid &&
            repeated.canonical_result_bytes == first.canonical_result_bytes,
        "node-driven " + std::string(profile.name) +
            " composition lost pair-statistical, LIMIT, or deterministic "
            "replay semantics");

    auto bounded_context = Context();
    bounded_context.optimizer_maximum_candidate_count = 8;
    const auto exhausted = dispatch(
        NodeDrivenPairStatisticalLimitEnvelope(
            profile, kFixtureFamilies[index]),
        std::move(bounded_context));
    passed &= Require(
        !exhausted.optimizer_selected &&
            !exhausted.physical_dag_published &&
            !exhausted.physical_dag_executed &&
            !exhausted.runtime_actuals_attached &&
            !exhausted.canonical_result_published &&
            !exhausted.api_result.ok && exhausted.physical_node_count == 0 &&
            exhausted.canonical_result_bytes.empty() &&
            HasApiDiagnostic(
                exhausted,
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
        "exhausted node-driven " + std::string(profile.name) +
            " composition published partial evidence");
  }
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-GROUPED-COUNT-SUM-COMPOSITION-V1
bool ValidateNodeDrivenGroupedCountSumCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto first = dispatch(NodeDrivenGroupedCountSumLimitEnvelope());
  const auto repeated = dispatch(NodeDrivenGroupedCountSumLimitEnvelope());
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  const auto& rows = first.api_result.result_shape.rows;
  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted &&
          first.optimizer_selected && first.physical_dag_published &&
          first.physical_dag_executed && first.runtime_actuals_attached &&
          first.canonical_result_published && first.api_result.ok &&
          first.diagnostics.empty() && first.logical_node_count == 3 &&
          first.physical_node_count == 3 &&
          first.canonical_result_column_count == 3 &&
          first.canonical_result_row_count == 1 && rows.size() == 1 &&
          rows[0].fields.size() == 3 &&
          rows[0].fields[0].first == "group_key" &&
          rows[0].fields[0].second.encoded_value == "1" &&
          rows[0].fields[1].first == "row_count" &&
          rows[0].fields[1].second.encoded_value == "3" &&
          rows[0].fields[2].first == "total_amount" &&
          rows[0].fields[2].second.encoded_value == "25" &&
          repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "node-driven grouped COUNT/SUM composition lost grouping, LIMIT, "
      "or deterministic replay semantics");

  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 48;
  const auto exhausted = dispatch(
      NodeDrivenGroupedCountSumLimitEnvelope(),
      std::move(bounded_context));
  passed &= Require(
      !exhausted.optimizer_selected &&
          !exhausted.physical_dag_published &&
          !exhausted.physical_dag_executed &&
          !exhausted.runtime_actuals_attached &&
          !exhausted.canonical_result_published &&
          !exhausted.api_result.ok && exhausted.physical_node_count == 0 &&
          exhausted.canonical_result_bytes.empty() &&
          HasApiDiagnostic(
              exhausted,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "exhausted node-driven grouped COUNT/SUM composition published "
      "partial evidence");
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-GROUPING-EXPANSION-COMPOSITION-V1
bool ValidateNodeDrivenGroupingExpansionCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto validate = [&](sblr::SblrOperationEnvelope reference_envelope,
                            const std::string_view tree_uuid,
                            const std::string_view output_descriptor_ids,
                            const std::uint64_t exhausted_budget,
                            const std::string_view name) {
    auto exhausted_envelope = reference_envelope;
    const auto first = dispatch(NodeDrivenGroupedExpansionLimitEnvelope(
        reference_envelope, tree_uuid, output_descriptor_ids));
    const auto repeated = dispatch(NodeDrivenGroupedExpansionLimitEnvelope(
        reference_envelope, tree_uuid, output_descriptor_ids));
    const auto reference = dispatch(std::move(reference_envelope));
    if (!first.api_result.ok) {
      for (const auto& diagnostic : first.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    bool same_first_row = reference.api_result.ok &&
                          !reference.api_result.result_shape.rows.empty() &&
                          first.api_result.result_shape.rows.size() == 1;
    if (same_first_row) {
      const auto& expected =
          reference.api_result.result_shape.rows.front().fields;
      const auto& actual =
          first.api_result.result_shape.rows.front().fields;
      same_first_row = actual.size() == expected.size();
      for (std::size_t field = 0;
           same_first_row && field < expected.size(); ++field) {
        same_first_row =
            actual[field].first == expected[field].first &&
            actual[field].second.state == expected[field].second.state &&
            actual[field].second.is_null == expected[field].second.is_null &&
            actual[field].second.encoded_value ==
                expected[field].second.encoded_value &&
            actual[field].second.binary_value ==
                expected[field].second.binary_value;
      }
    }
    bool profile_passed = Require(
        first.accepted && first.optimizer_admitted &&
            first.optimizer_selected && first.physical_dag_published &&
            first.physical_dag_executed && first.runtime_actuals_attached &&
            first.canonical_result_published && first.api_result.ok &&
            first.diagnostics.empty() && first.logical_node_count == 3 &&
            first.physical_node_count == 3 &&
            first.canonical_result_column_count ==
                reference.canonical_result_column_count &&
            first.canonical_result_row_count == 1 && same_first_row &&
            repeated.api_result.ok &&
            repeated.selected_plan_uuid == first.selected_plan_uuid &&
            repeated.canonical_result_bytes == first.canonical_result_bytes,
        "node-driven " + std::string(name) +
            " composition lost grouping expansion, metadata, LIMIT, or "
            "deterministic replay semantics");

    auto bounded_context = Context();
    bounded_context.optimizer_maximum_candidate_count = exhausted_budget;
    const auto exhausted = dispatch(
        NodeDrivenGroupedExpansionLimitEnvelope(
            std::move(exhausted_envelope), tree_uuid,
            output_descriptor_ids),
        std::move(bounded_context));
    profile_passed &= Require(
        !exhausted.optimizer_selected &&
            !exhausted.physical_dag_published &&
            !exhausted.physical_dag_executed &&
            !exhausted.runtime_actuals_attached &&
            !exhausted.canonical_result_published &&
            !exhausted.api_result.ok && exhausted.physical_node_count == 0 &&
            exhausted.canonical_result_bytes.empty() &&
            HasApiDiagnostic(
                exhausted,
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
        "exhausted node-driven " + std::string(name) +
            " composition published partial evidence");
    return profile_passed;
  };

  bool passed = true;
  passed &= validate(TwoKeyGroupedCountSumValuesEnvelope(),
                     "019f0000-0000-7000-8000-00000000cd10",
                     "1,2,4,5", 84, "two-key GROUP BY");
  passed &= validate(RollupCountSumValuesEnvelope(),
                     "019f0000-0000-7000-8000-00000000cd20",
                     "1,2,4,5", 240, "ROLLUP");
  passed &= validate(RollupCountSumGroupingValuesEnvelope(),
                     "019f0000-0000-7000-8000-00000000cd30",
                     "1,2,4,5,6,7,8", 240,
                     "ROLLUP with GROUPING metadata");
  passed &= validate(CubeCountSumValuesEnvelope(),
                     "019f0000-0000-7000-8000-00000000cd40",
                     "1,2,4,5", 318, "CUBE");
  passed &= validate(CubeCountSumGroupingValuesEnvelope(),
                     "019f0000-0000-7000-8000-00000000cd50",
                     "1,2,4,5,6,7,8", 318,
                     "CUBE with GROUPING metadata");
  passed &= validate(GroupingSetsCountSumValuesEnvelope(),
                     "019f0000-0000-7000-8000-00000000cd60",
                     "1,2,4,5", 318, "GROUPING SETS");
  passed &= validate(GroupingSetsCountSumGroupingValuesEnvelope(),
                     "019f0000-0000-7000-8000-00000000cd70",
                     "1,2,4,5,6,7,8", 318,
                     "GROUPING SETS with GROUPING metadata");
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-GROUPED-HAVING-COMPOSITION-V1
bool ValidateNodeDrivenGroupedHavingCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto reference = dispatch(TwoKeyGroupedCountSumHavingValuesEnvelope());
  const auto first = dispatch(NodeDrivenGroupedHavingLimitEnvelope());
  const auto repeated = dispatch(NodeDrivenGroupedHavingLimitEnvelope());
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  const auto& rows = first.api_result.result_shape.rows;
  bool passed = true;
  passed &= Require(
      reference.api_result.ok &&
          reference.canonical_result_row_count == 2 && first.accepted &&
          first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached &&
          first.canonical_result_published && first.api_result.ok &&
          first.diagnostics.empty() && first.logical_node_count == 4 &&
          first.physical_node_count == 4 &&
          first.canonical_result_column_count == 4 &&
          first.canonical_result_row_count == 1 && rows.size() == 1 &&
          rows[0].fields.size() == 4 &&
          rows[0].fields[0].second.encoded_value == "1" &&
          rows[0].fields[1].second.encoded_value == "20" &&
          rows[0].fields[2].second.encoded_value == "1" &&
          rows[0].fields[3].second.encoded_value == "7" &&
          repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "node-driven grouped HAVING composition lost 3VL filtering, LIMIT, "
      "or deterministic replay semantics");

  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 89;
  const auto exhausted = dispatch(NodeDrivenGroupedHavingLimitEnvelope(),
                                  std::move(bounded_context));
  passed &= Require(
      !exhausted.optimizer_selected &&
          !exhausted.physical_dag_published &&
          !exhausted.physical_dag_executed &&
          !exhausted.runtime_actuals_attached &&
          !exhausted.canonical_result_published &&
          !exhausted.api_result.ok && exhausted.physical_node_count == 0 &&
          exhausted.canonical_result_bytes.empty() &&
          HasApiDiagnostic(
              exhausted,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "exhausted node-driven grouped HAVING composition published partial "
      "evidence");
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-STRING-AGG-COMPOSITION-V1
bool ValidateNodeDrivenStringAggCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  bool passed = true;
  for (const bool ordered : {false, true}) {
    const auto first = dispatch(NodeDrivenStringAggLimitEnvelope(ordered));
    const auto repeated = dispatch(NodeDrivenStringAggLimitEnvelope(ordered));
    if (!first.api_result.ok) {
      for (const auto& diagnostic : first.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    const auto expected = ordered ? "a|b|d" : "b|d|a";
    passed &= Require(
        first.accepted && first.optimizer_admitted &&
            first.optimizer_selected && first.physical_dag_published &&
            first.physical_dag_executed && first.runtime_actuals_attached &&
            first.canonical_result_published && first.api_result.ok &&
            first.diagnostics.empty() && first.logical_node_count == 3 &&
            first.physical_node_count == 3 &&
            first.canonical_result_column_count == 1 &&
            first.canonical_result_row_count == 1 &&
            first.api_result.result_shape.rows.size() == 1 &&
            first.api_result.result_shape.rows[0].fields.size() == 1 &&
            first.api_result.result_shape.rows[0]
                    .fields[0]
                    .second.encoded_value == expected &&
            repeated.api_result.ok &&
            repeated.selected_plan_uuid == first.selected_plan_uuid &&
            repeated.canonical_result_bytes == first.canonical_result_bytes,
        "node-driven " + std::string(ordered ? "ordered " : "") +
            "STRING_AGG composition lost aggregation, LIMIT, or "
            "deterministic replay semantics");

    auto bounded_context = Context();
    bounded_context.optimizer_maximum_candidate_count = ordered ? 24 : 8;
    const auto exhausted = dispatch(NodeDrivenStringAggLimitEnvelope(ordered),
                                    std::move(bounded_context));
    passed &= Require(
        !exhausted.optimizer_selected &&
            !exhausted.physical_dag_published &&
            !exhausted.physical_dag_executed &&
            !exhausted.runtime_actuals_attached &&
            !exhausted.canonical_result_published &&
            !exhausted.api_result.ok && exhausted.physical_node_count == 0 &&
            exhausted.canonical_result_bytes.empty() &&
            HasApiDiagnostic(
                exhausted,
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
        "exhausted node-driven STRING_AGG composition published partial "
        "evidence");
  }
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-COMPLEX-AGGREGATE-COMPOSITION-V1
bool ValidateNodeDrivenComplexAggregateCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  std::size_t ordinal = 0;
  const auto validate_case = [&](sblr::SblrOperationEnvelope envelope,
                                 const std::string& label,
                                 const std::uint64_t exhausted_bound) {
    const auto reference = dispatch(envelope);
    const auto composed = NodeDrivenComplexAggregateLimitEnvelope(
        std::move(envelope), ordinal++);
    const auto first = dispatch(composed);
    const auto repeated = dispatch(composed);
    if (!first.api_result.ok) {
      std::cerr << "complex aggregate composition case: " << label << '\n';
      for (const auto& diagnostic : first.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    const bool has_reference_value =
        reference.api_result.ok &&
        reference.api_result.result_shape.rows.size() == 1 &&
        reference.api_result.result_shape.rows[0].fields.size() == 1;
    const bool has_composed_value =
        first.api_result.result_shape.rows.size() == 1 &&
        first.api_result.result_shape.rows[0].fields.size() == 1;
    bool exact_value = false;
    if (has_reference_value && has_composed_value) {
      const auto& expected =
          reference.api_result.result_shape.rows[0].fields[0];
      const auto& observed = first.api_result.result_shape.rows[0].fields[0];
      exact_value =
          observed.first == expected.first &&
          observed.second.descriptor.canonical_type_name ==
              expected.second.descriptor.canonical_type_name &&
          observed.second.descriptor.encoded_descriptor ==
              expected.second.descriptor.encoded_descriptor &&
          observed.second.state == expected.second.state &&
          observed.second.is_null == expected.second.is_null &&
          observed.second.encoded_value == expected.second.encoded_value &&
          observed.second.binary_value == expected.second.binary_value;
    }
    bool passed = Require(
        first.accepted && first.optimizer_admitted &&
            first.optimizer_selected && first.physical_dag_published &&
            first.physical_dag_executed && first.runtime_actuals_attached &&
            first.canonical_result_published && first.api_result.ok &&
            first.diagnostics.empty() && first.logical_node_count == 3 &&
            first.logical_property_count == 0 &&
            first.physical_node_count == 3 &&
            first.canonical_result_column_count == 1 &&
            first.canonical_result_row_count == 1 && exact_value &&
            repeated.api_result.ok &&
            repeated.selected_plan_uuid == first.selected_plan_uuid &&
            repeated.canonical_result_bytes == first.canonical_result_bytes,
        "node-driven " + label +
            " composition lost its aggregate value, LIMIT, descriptor, or "
            "deterministic replay semantics");

    auto bounded_context = Context();
    bounded_context.optimizer_maximum_candidate_count = exhausted_bound;
    const auto exhausted = dispatch(composed, std::move(bounded_context));
    passed &= Require(
        !exhausted.optimizer_selected &&
            !exhausted.physical_dag_published &&
            !exhausted.physical_dag_executed &&
            !exhausted.runtime_actuals_attached &&
            !exhausted.canonical_result_published &&
            !exhausted.api_result.ok && exhausted.physical_node_count == 0 &&
            exhausted.canonical_result_bytes.empty() &&
            HasApiDiagnostic(
                exhausted,
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
        "exhausted node-driven " + label +
            " composition published partial evidence");
    return passed;
  };

  bool passed = true;
  for (const bool ordered : {false, true}) {
    for (const auto modifier : {AggregateModifierProfile::kFilter,
                                AggregateModifierProfile::kDistinct,
                                AggregateModifierProfile::kDistinctFilter}) {
      passed &= validate_case(
          StringAggModifierValuesEnvelope(ordered, modifier),
          std::string(ordered ? "ordered " : "") + "STRING_AGG " +
              std::string(AggregateModifierName(modifier)),
          ordered || modifier != AggregateModifierProfile::kFilter ? 63 : 14);
    }
  }
  for (const auto& profile : kOrderedSingleCollectionProfiles) {
    passed &= validate_case(OrderedSingleCollectionValuesEnvelope(profile),
                            std::string(profile.name), 24);
    for (const auto modifier : {AggregateModifierProfile::kFilter,
                                AggregateModifierProfile::kDistinct,
                                AggregateModifierProfile::kDistinctFilter}) {
      passed &= validate_case(
          OrderedSingleCollectionModifierValuesEnvelope(profile, modifier),
          std::string(profile.name) + " " +
              std::string(AggregateModifierName(modifier)),
          63);
    }
  }
  passed &= validate_case(OrderedJsonObjectAggValuesEnvelope(),
                          "JSON_OBJECT_AGG", 40);
  for (const auto modifier : {AggregateModifierProfile::kFilter,
                              AggregateModifierProfile::kDistinct,
                              AggregateModifierProfile::kDistinctFilter}) {
    passed &= validate_case(OrderedJsonObjectAggModifierValuesEnvelope(modifier),
                            "JSON_OBJECT_AGG " +
                                std::string(AggregateModifierName(modifier)),
                            144);
  }
  for (const auto profile : {LiveListaggProfile::kOrdered,
                             LiveListaggProfile::kOverflowTruncateWithCount,
                             LiveListaggProfile::kOverflowTruncateWithoutCount}) {
    passed &= validate_case(OrderedListaggExpressionValuesEnvelope(profile),
                            "LISTAGG", 24);
  }
  for (const auto profile : {LiveListaggProfile::kOrdered,
                             LiveListaggProfile::kOverflowError,
                             LiveListaggProfile::kOverflowTruncateWithCount}) {
    for (const auto modifier : {AggregateModifierProfile::kFilter,
                                AggregateModifierProfile::kDistinct,
                                AggregateModifierProfile::kDistinctFilter}) {
      passed &= validate_case(
          OrderedListaggModifierExpressionValuesEnvelope(profile, modifier),
          "LISTAGG " + std::string(AggregateModifierName(modifier)), 63);
    }
  }
  for (const auto& profile : kOrderedSetAggregateProfiles) {
    passed &= validate_case(GlobalOrderedSetAggregateValuesEnvelope(profile),
                            std::string(profile.name), 35);
    for (const auto modifier : {AggregateModifierProfile::kFilter,
                                AggregateModifierProfile::kDistinct,
                                AggregateModifierProfile::kDistinctFilter}) {
      passed &= validate_case(
          GlobalOrderedSetAggregateModifierValuesEnvelope(profile, modifier),
          std::string(profile.name) + " " +
              std::string(AggregateModifierName(modifier)),
          63);
    }
  }
  for (const auto& profile : kApproximateAggregateProfiles) {
    passed &= validate_case(GlobalApproximateAggregateValuesEnvelope(profile),
                            std::string(profile.name), 48);
    for (const auto modifier : {AggregateModifierProfile::kFilter,
                                AggregateModifierProfile::kDistinct,
                                AggregateModifierProfile::kDistinctFilter}) {
      passed &= validate_case(
          GlobalApproximateAggregateModifierValuesEnvelope(profile, modifier),
          std::string(profile.name) + " " +
              std::string(AggregateModifierName(modifier)),
          63);
    }
  }
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-EXTREMUM-EXPRESSION-COMPOSITION-V1
bool ValidateNodeDrivenExtremumExpressionCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  bool passed = true;
  for (const bool maximum : {false, true}) {
    const auto first =
        dispatch(NodeDrivenExtremumExpressionLimitEnvelope(maximum));
    const auto repeated =
        dispatch(NodeDrivenExtremumExpressionLimitEnvelope(maximum));
    if (!first.api_result.ok) {
      for (const auto& diagnostic : first.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    const std::string_view expected = maximum ? "9" : "-4";
    passed &= Require(
        first.accepted && first.optimizer_admitted &&
            first.optimizer_selected && first.physical_dag_published &&
            first.physical_dag_executed && first.runtime_actuals_attached &&
            first.canonical_result_published && first.api_result.ok &&
            first.diagnostics.empty() && first.logical_node_count == 3 &&
            first.physical_node_count == 3 &&
            first.canonical_result_column_count == 1 &&
            first.canonical_result_row_count == 1 &&
            first.api_result.result_shape.rows.size() == 1 &&
            first.api_result.result_shape.rows[0]
                    .fields[0]
                    .second.encoded_value == expected &&
            repeated.api_result.ok &&
            repeated.selected_plan_uuid == first.selected_plan_uuid &&
            repeated.canonical_result_bytes == first.canonical_result_bytes,
        std::string("node-driven ") + (maximum ? "MAX" : "MIN") +
            "(expression) composition lost its extremum, LIMIT semantics, "
            "or deterministic replay");

    auto bounded_context = Context();
    bounded_context.optimizer_maximum_candidate_count = 8;
    const auto exhausted = dispatch(
        NodeDrivenExtremumExpressionLimitEnvelope(maximum),
        std::move(bounded_context));
    passed &= Require(
        !exhausted.optimizer_selected &&
            !exhausted.physical_dag_published &&
            !exhausted.physical_dag_executed &&
            !exhausted.runtime_actuals_attached &&
            !exhausted.canonical_result_published &&
            !exhausted.api_result.ok && exhausted.physical_node_count == 0 &&
            exhausted.canonical_result_bytes.empty() &&
            HasApiDiagnostic(
                exhausted,
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
        std::string("exhausted node-driven ") +
            (maximum ? "MAX" : "MIN") +
            "(expression) composition published partial evidence");
  }
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-BOOLEAN-AGGREGATE-COMPOSITION-V1
bool ValidateNodeDrivenBooleanAggregateCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  bool passed = true;
  for (const auto kind : {BooleanAggregateKind::kBoolAnd,
                          BooleanAggregateKind::kBoolOr,
                          BooleanAggregateKind::kEvery}) {
    const auto first = dispatch(NodeDrivenBooleanAggregateLimitEnvelope(kind));
    const auto repeated =
        dispatch(NodeDrivenBooleanAggregateLimitEnvelope(kind));
    if (!first.api_result.ok) {
      for (const auto& diagnostic : first.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    const bool bool_or = kind == BooleanAggregateKind::kBoolOr;
    passed &= Require(
        first.accepted && first.optimizer_admitted &&
            first.optimizer_selected && first.physical_dag_published &&
            first.physical_dag_executed && first.runtime_actuals_attached &&
            first.canonical_result_published && first.api_result.ok &&
            first.diagnostics.empty() && first.logical_node_count == 3 &&
            first.physical_node_count == 3 &&
            first.canonical_result_column_count == 1 &&
            first.canonical_result_row_count == 1 &&
            first.api_result.result_shape.rows.size() == 1 &&
            first.api_result.result_shape.rows[0]
                    .fields[0]
                    .second.encoded_value == (bool_or ? "true" : "false") &&
            repeated.api_result.ok &&
            repeated.selected_plan_uuid == first.selected_plan_uuid &&
            repeated.canonical_result_bytes == first.canonical_result_bytes,
        "node-driven " + std::string(BooleanAggregateName(kind)) +
            " composition lost three-valued aggregate, LIMIT, or "
            "deterministic replay semantics");

    auto bounded_context = Context();
    bounded_context.optimizer_maximum_candidate_count = 8;
    const auto exhausted = dispatch(
        NodeDrivenBooleanAggregateLimitEnvelope(kind),
        std::move(bounded_context));
    passed &= Require(
        !exhausted.optimizer_selected &&
            !exhausted.physical_dag_published &&
            !exhausted.physical_dag_executed &&
            !exhausted.runtime_actuals_attached &&
            !exhausted.canonical_result_published &&
            !exhausted.api_result.ok && exhausted.physical_node_count == 0 &&
            exhausted.canonical_result_bytes.empty() &&
            HasApiDiagnostic(
                exhausted,
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
        "exhausted node-driven " + std::string(BooleanAggregateName(kind)) +
            " composition published partial evidence");
  }
  return passed;
}

// RCP-049-TEST-NODE-DRIVEN-NESTED-EXACT-SET-COMPOSITION-V1
bool ValidateNodeDrivenNestedExactSetCompositionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result,
                            const std::array<std::string_view, 2>& values) {
    if (!result.accepted || !result.optimizer_admitted ||
        !result.optimizer_selected || !result.physical_dag_published ||
        !result.physical_dag_executed || !result.runtime_actuals_attached ||
        !result.canonical_result_published || !result.api_result.ok ||
        !result.diagnostics.empty() || result.logical_node_count != 6 ||
        result.physical_node_count != 6 ||
        result.canonical_result_column_count != 1 ||
        result.canonical_result_row_count != values.size() ||
        result.api_result.result_shape.rows.size() != values.size()) {
      return false;
    }
    for (std::size_t row = 0; row < values.size(); ++row) {
      if (result.api_result.result_shape.rows[row]
              .fields[0]
              .second.encoded_value != values[row]) {
        return false;
      }
    }
    return true;
  };
  const auto no_publication = [](const auto& result) {
    return !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty();
  };

  const auto left =
      dispatch(NodeDrivenNestedExactSetLimitEnvelope(false));
  const auto left_repeated =
      dispatch(NodeDrivenNestedExactSetLimitEnvelope(false));
  const auto right =
      dispatch(NodeDrivenNestedExactSetLimitEnvelope(true));
  const auto right_repeated =
      dispatch(NodeDrivenNestedExactSetLimitEnvelope(true));
  if (!left.api_result.ok) {
    for (const auto& diagnostic : left.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  if (!right.api_result.ok) {
    for (const auto& diagnostic : right.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  passed &= Require(
      completed(left, {"1", "3"}) && completed(right, {"1", "2"}) &&
          left.selected_plan_uuid != right.selected_plan_uuid &&
          completed(left_repeated, {"1", "3"}) &&
          completed(right_repeated, {"1", "2"}) &&
          left_repeated.selected_plan_uuid == left.selected_plan_uuid &&
          right_repeated.selected_plan_uuid == right.selected_plan_uuid &&
          left_repeated.canonical_result_bytes ==
              left.canonical_result_bytes &&
          right_repeated.canonical_result_bytes ==
              right.canonical_result_bytes,
      "node-driven nested set composition lost grouping, LIMIT semantics, "
      "plan identity, or deterministic replay");

  auto malformed = NodeDrivenNestedExactSetLimitEnvelope(false);
  for (auto& operand : malformed.operands) {
    if (operand.type == "relational_node_v1" && operand.name == "4") {
      operand.value = "9|0|1,1|4|-";
    }
  }
  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 20;
  const auto malformed_result = dispatch(std::move(malformed));
  const auto exhausted_result = dispatch(
      NodeDrivenNestedExactSetLimitEnvelope(false),
      std::move(bounded_context));
  passed &= Require(
      no_publication(malformed_result) &&
          no_publication(exhausted_result) &&
          HasApiDiagnostic(
              exhausted_result,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "malformed or exhausted node-driven nested set composition published "
      "partial evidence");
  return passed;
}

// RCP-040-TEST-LIVE-EMPTY-FILTERED-EXPRESSION-PROJECTION-V1
bool ValidateEmptyFilteredExpressionProjectionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
  };
  struct EmptyShape {
    sblr::SblrOperationEnvelope envelope;
    std::size_t node_count = 0;
    std::size_t column_count = 0;
    std::string_view name;
  };
  std::array<EmptyShape, 5> shapes{
      EmptyShape{RejectAllFilteredRows(
                     FilteredExpressionProjectValuesEnvelope()),
                 3, 2, "FILTER/PROJECT"},
      EmptyShape{RejectAllFilteredRows(
                     FilteredProjectedSortValuesEnvelope()),
                 4, 2, "FILTER/PROJECT/SORT"},
      EmptyShape{RejectAllFilteredRows(
                     FilteredProjectedSortLimitValuesEnvelope()),
                 5, 2, "FILTER/PROJECT/SORT/LIMIT"},
      EmptyShape{RejectAllFilteredRows(
                     FilteredProjectedDistinctSortOffsetValuesEnvelope(false)),
                 6, 1, "full LIMIT/OFFSET tail"},
      EmptyShape{RejectAllFilteredRows(
                     FilteredProjectedDistinctSortOffsetValuesEnvelope(true)),
                 6, 1, "full FETCH tail"},
  };

  bool passed = true;
  for (auto& shape : shapes) {
    const auto result = dispatch(std::move(shape.envelope));
    if (!result.api_result.ok) {
      for (const auto& diagnostic : result.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    passed &= Require(
        result.accepted && result.optimizer_admitted &&
            result.optimizer_selected && result.physical_dag_published &&
            result.physical_dag_executed &&
            result.runtime_actuals_attached &&
            result.canonical_result_published && result.api_result.ok &&
            result.diagnostics.empty() &&
            result.logical_node_count == shape.node_count &&
            result.physical_node_count == shape.node_count &&
            result.canonical_result_column_count == shape.column_count &&
            result.canonical_result_row_count == 0 &&
            result.api_result.result_shape.columns.size() ==
                shape.column_count &&
            result.api_result.result_shape.rows.empty(),
        std::string(shape.name) +
            " did not publish a typed empty result after FILTER rejected "
            "every row");
  }

  const auto repeated_first = dispatch(RejectAllFilteredRows(
      FilteredProjectedDistinctSortOffsetValuesEnvelope(true)));
  const auto repeated_second = dispatch(RejectAllFilteredRows(
      FilteredProjectedDistinctSortOffsetValuesEnvelope(true)));
  passed &= Require(
      repeated_first.api_result.ok && repeated_second.api_result.ok &&
          repeated_first.canonical_result_row_count == 0 &&
          repeated_first.selected_plan_uuid ==
              repeated_second.selected_plan_uuid &&
          repeated_first.canonical_result_bytes ==
              repeated_second.canonical_result_bytes,
      "typed empty full-tail result changed deterministic plan/result bytes");
  return passed;
}

bool ValidateLimitValuesSpine() {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), LimitValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), LimitValuesEnvelope(), {}});
  auto zero_envelope = LimitValuesEnvelope();
  auto oversized_envelope = LimitValuesEnvelope();
  for (auto& operand : zero_envelope.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "5") {
      operand.value = "1|-|2|-|-|1|-|30";
    }
  }
  for (auto& operand : oversized_envelope.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "5") {
      operand.value = "1|-|2|-|-|1|-|3939";
    }
  }
  const auto zero = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), std::move(zero_envelope), {}});
  const auto oversized = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), std::move(oversized_envelope), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached && first.canonical_result_published &&
          first.api_result.ok && first.diagnostics.empty() &&
          first.logical_node_count == 2 && first.physical_node_count == 2 &&
          first.canonical_result_column_count == 1 &&
          first.canonical_result_row_count == 2,
      "VALUES LIMIT did not traverse the selected two-node DAG");
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      rows.size() == 2 && rows[0].fields.size() == 1 &&
          rows[0].fields[0].first == "n" &&
          rows[0].fields[0].second.encoded_value == "1" &&
          rows[1].fields[0].second.encoded_value == "2",
      "LIMIT did not preserve the bounded input prefix");
  passed &= Require(
      zero.api_result.ok && zero.canonical_result_column_count == 1 &&
          zero.canonical_result_row_count == 0 &&
          zero.api_result.result_shape.columns.size() == 1 &&
          zero.api_result.result_shape.rows.empty(),
      "LIMIT zero did not publish a typed empty result");
  passed &= Require(
      oversized.api_result.ok && oversized.canonical_result_row_count == 4 &&
          oversized.api_result.result_shape.rows.size() == 4 &&
          oversized.api_result.result_shape.rows[2].fields[0].second.state ==
              api::EngineValueState::sql_null &&
          oversized.api_result.result_shape.rows[3]
                  .fields[0]
                  .second.encoded_value == "4",
      "oversized LIMIT did not stop safely at end-of-input");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical LIMIT input changed canonical plan/result bytes");
  return passed;
}

bool ValidateLimitRefusalIsAtomic() {
  auto negative = LimitValuesEnvelope();
  auto null_count = LimitValuesEnvelope();
  auto schema_drift = LimitValuesEnvelope();
  for (auto& operand : negative.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "5") {
      operand.value = "1|-|2|-|-|1|-|2d31";
    }
  }
  for (auto& operand : null_count.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "5") {
      operand.value = "1|-|2|-|-|7|-|2d";
    }
  }
  for (auto& operand : schema_drift.operands) {
    if (operand.type == "relational_node_v1" && operand.name == "2") {
      operand.value = "7|0|1|2|-";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-LIMIT-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(negative)) &&
          refused_atomically(std::move(null_count)) &&
          refused_atomically(std::move(schema_drift)),
      "invalid bound-count LIMIT published partial evidence");
}

bool ValidateGroupedCountSumValuesSpine() {
  const auto many = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GroupedCountSumValuesEnvelope(), {}});
  const auto many_repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GroupedCountSumValuesEnvelope(), {}});
  const auto one = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GroupedCountSumValuesEnvelope(true), {}});
  const auto one_repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GroupedCountSumValuesEnvelope(true), {}});
  if (!many.api_result.ok) {
    for (const auto& diagnostic : many.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }

  const auto traversed = [](const auto& result,
                            const std::size_t expected_rows) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed &&
           result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 2 &&
           result.logical_property_count == 0 &&
           result.physical_node_count == 2 &&
           result.canonical_result_column_count == 3 &&
           result.canonical_result_row_count == expected_rows;
  };
  bool passed = true;
  passed &= Require(
      traversed(many, 3) && traversed(one, 1),
      "VALUES grouped COUNT/SUM did not traverse its selected physical DAG");

  const auto& columns = many.api_result.result_shape.columns;
  const auto& rows = many.api_result.result_shape.rows;
  passed &= Require(
      columns.size() == 3 && rows.size() == 3 &&
          columns[0].canonical_type_name == "int64" &&
          columns[0].encoded_descriptor.find("nullability=nullable") !=
              std::string::npos &&
          columns[1].canonical_type_name == "int64" &&
          columns[1].encoded_descriptor.find("nullability=non_null") !=
              std::string::npos &&
          columns[2].canonical_type_name == "int64" &&
          columns[2].encoded_descriptor.find("nullability=nullable") !=
              std::string::npos,
      "grouped COUNT/SUM result descriptors lost key or aggregate identity");
  passed &= Require(
      rows.size() == 3 && rows[0].fields.size() == 3 &&
          rows[0].fields[0].first == "group_key" &&
          rows[0].fields[0].second.encoded_value == "1" &&
          rows[0].fields[1].first == "row_count" &&
          rows[0].fields[1].second.encoded_value == "3" &&
          rows[0].fields[2].first == "total_amount" &&
          rows[0].fields[2].second.encoded_value == "25" &&
          rows[1].fields.size() == 3 &&
          rows[1].fields[0].second.encoded_value == "2" &&
          rows[1].fields[1].second.encoded_value == "2" &&
          rows[1].fields[2].second.encoded_value == "5" &&
          rows[2].fields.size() == 3 &&
          rows[2].fields[0].second.state ==
              api::EngineValueState::sql_null &&
          rows[2].fields[0].second.is_null &&
          rows[2].fields[1].second.encoded_value == "1" &&
          rows[2].fields[2].second.encoded_value == "7",
      "grouped COUNT/SUM did not preserve NULL grouping, COUNT(*), or SUM "
      "state");

  const auto& one_rows = one.api_result.result_shape.rows;
  passed &= Require(
      one_rows.size() == 1 && one_rows[0].fields.size() == 3 &&
          one_rows[0].fields[0].second.encoded_value == "1" &&
          one_rows[0].fields[1].second.encoded_value == "6" &&
          one_rows[0].fields[2].second.encoded_value == "37",
      "single-group COUNT/SUM did not share one physical group state");
  passed &= Require(
      many_repeated.api_result.ok && one_repeated.api_result.ok &&
          many_repeated.selected_plan_uuid == many.selected_plan_uuid &&
          many_repeated.canonical_result_bytes ==
              many.canonical_result_bytes &&
          one_repeated.selected_plan_uuid == one.selected_plan_uuid &&
          one_repeated.canonical_result_bytes == one.canonical_result_bytes,
      "identical grouped COUNT/SUM input changed canonical plan/result bytes");
  return passed;
}

bool ValidateGroupedCountSumRefusalIsAtomic() {
  auto missing_sum = GroupedCountSumValuesEnvelope();
  auto bound_order_drift = GroupedCountSumValuesEnvelope();
  auto count_function_drift = GroupedCountSumValuesEnvelope();
  auto sum_function_drift = GroupedCountSumValuesEnvelope();
  auto non_identifier_key = GroupedCountSumValuesEnvelope();
  auto output_order_drift = GroupedCountSumValuesEnvelope();
  for (auto& operand : missing_sum.operands) {
    if (operand.type == "relational_node_binding_v1" &&
        operand.name == "2") {
      operand.value =
          EncodeHex("aggregate.grouped-int64-key-count-sum.v1") +
          "|13,14|-|-|-";
    }
  }
  for (auto& operand : bound_order_drift.operands) {
    if (operand.type == "relational_node_binding_v1" &&
        operand.name == "2") {
      operand.value =
          EncodeHex("aggregate.grouped-int64-key-count-sum.v1") +
          "|14,13,16|-|-|-";
    }
  }
  for (auto& operand : count_function_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "14") {
      operand.value =
          "4|-|3|019de5fc-2400-72e4-8549-82b2eef5a777|-|-|-|-";
    }
  }
  for (auto& operand : sum_function_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "16") {
      operand.value =
          "4|15|4|019de5fc-2400-784a-9aec-371f8b95b7ea|-|-|-|-";
    }
  }
  for (auto& operand : non_identifier_key.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "13") {
      operand.value = "1|-|1|-|-|1|-|31";
    }
  }
  for (auto& operand : output_order_drift.operands) {
    if (operand.type == "relational_node_v1" && operand.name == "2") {
      operand.value = "5|0|1|3,1,4|-";
    }
  }

  const auto refused_atomically = [](const std::string_view label,
                                     sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    const bool refused =
        result.accepted && result.optimizer_admitted &&
        !result.optimizer_selected && !result.physical_dag_published &&
        !result.physical_dag_executed && !result.runtime_actuals_attached &&
        !result.canonical_result_published && !result.api_result.ok &&
        result.physical_node_count == 0 &&
        result.canonical_result_bytes.empty() &&
        HasApiDiagnostic(
            result,
            "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-PAYLOAD-V1");
    if (!refused) {
      std::cerr << "grouped COUNT/SUM refusal mismatch (" << label
                << "): accepted=" << result.accepted
                << ", admitted=" << result.optimizer_admitted
                << ", selected=" << result.optimizer_selected
                << ", published=" << result.physical_dag_published
                << ", executed=" << result.physical_dag_executed
                << ", api_ok=" << result.api_result.ok << '\n';
      for (const auto& diagnostic : result.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    return refused;
  };
  const auto refused_before_admission =
      [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return !result.accepted && !result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(result, "SBLR.PLAN_TREE.INVALID_HANDLE");
  };
  return Require(
      refused_atomically("missing_sum", std::move(missing_sum)) &&
          refused_atomically("bound_order", std::move(bound_order_drift)) &&
          refused_atomically("count_function",
                             std::move(count_function_drift)) &&
          refused_atomically("sum_function", std::move(sum_function_drift)) &&
          refused_atomically("non_identifier_key",
                             std::move(non_identifier_key)) &&
          refused_before_admission(std::move(output_order_drift)),
      "shape-, lineage-, function-, key-, or output-order-drifted grouped "
      "COUNT/SUM published partial evidence");
}

bool ValidateRollupCountSumValuesSpine() {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), RollupCountSumValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), RollupCountSumValuesEnvelope(), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }

  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached &&
          first.canonical_result_published && first.api_result.ok &&
          first.diagnostics.empty() && first.logical_node_count == 2 &&
          first.logical_property_count == 0 &&
          first.physical_node_count == 2 &&
          first.canonical_result_column_count == 4 &&
          first.canonical_result_row_count == 9,
      "VALUES two-key ROLLUP did not traverse its selected physical DAG");

  const auto& columns = first.api_result.result_shape.columns;
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      columns.size() == 4 && rows.size() == 9 &&
          columns[0].canonical_type_name == "int64" &&
          columns[0].encoded_descriptor.find("nullability=nullable") !=
              std::string::npos &&
          columns[1].canonical_type_name == "int64" &&
          columns[1].encoded_descriptor.find("nullability=nullable") !=
              std::string::npos &&
          columns[2].canonical_type_name == "int64" &&
          columns[2].encoded_descriptor.find("nullability=non_null") !=
              std::string::npos &&
          columns[3].canonical_type_name == "int64" &&
          columns[3].encoded_descriptor.find("nullability=nullable") !=
              std::string::npos,
      "two-key ROLLUP result descriptors lost key or aggregate identity");

  const auto value_matches = [](
                                 const api::EngineTypedValue& value,
                                 const std::optional<std::string_view> expected) {
    if (!expected.has_value()) {
      return value.state == api::EngineValueState::sql_null && value.is_null;
    }
    return value.state == api::EngineValueState::value && !value.is_null &&
           value.encoded_value == *expected;
  };
  const auto row_matches = [&](const std::size_t ordinal,
                               const std::optional<std::string_view> key_a,
                               const std::optional<std::string_view> key_b,
                               const std::string_view count,
                               const std::string_view sum) {
    return ordinal < rows.size() && rows[ordinal].fields.size() == 4 &&
           rows[ordinal].fields[0].first == "key_a" &&
           rows[ordinal].fields[1].first == "key_b" &&
           rows[ordinal].fields[2].first == "row_count" &&
           rows[ordinal].fields[3].first == "total_amount" &&
           value_matches(rows[ordinal].fields[0].second, key_a) &&
           value_matches(rows[ordinal].fields[1].second, key_b) &&
           value_matches(rows[ordinal].fields[2].second, count) &&
           value_matches(rows[ordinal].fields[3].second, sum);
  };
  passed &= Require(
      row_matches(0, "1", "10", "2", "5") &&
          row_matches(1, "1", "20", "1", "7") &&
          row_matches(2, "1", std::nullopt, "1", "3") &&
          row_matches(3, "2", "10", "1", "4") &&
          row_matches(4, std::nullopt, "10", "1", "8") &&
          row_matches(5, "1", std::nullopt, "4", "15") &&
          row_matches(6, "2", std::nullopt, "1", "4") &&
          row_matches(7, std::nullopt, std::nullopt, "1", "8") &&
          row_matches(8, std::nullopt, std::nullopt, "6", "27"),
      "two-key ROLLUP lost full-key, prefix, grand-total, data-NULL, or "
      "aggregate state");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical two-key ROLLUP input changed canonical plan/result bytes");
  return passed;
}

bool ValidateRollupCountSumRefusalIsAtomic() {
  auto missing_key = RollupCountSumValuesEnvelope();
  auto key_order_drift = RollupCountSumValuesEnvelope();
  auto duplicate_key = RollupCountSumValuesEnvelope();
  auto count_function_drift = RollupCountSumValuesEnvelope();
  auto sum_function_drift = RollupCountSumValuesEnvelope();
  auto non_nullable_rollup_key = RollupCountSumValuesEnvelope();
  auto semantic_shape_drift = RollupCountSumValuesEnvelope();
  auto output_order_drift = RollupCountSumValuesEnvelope();
  const auto set_root_binding = [](sblr::SblrOperationEnvelope* envelope,
                                   const std::string& value) {
    for (auto& operand : envelope->operands) {
      if (operand.type == "relational_node_binding_v1" &&
          operand.name == "2") {
        operand.value = value;
      }
    }
  };
  const auto rollup_semantic =
      EncodeHex("aggregate.rollup-int64-keys-count-sum.v1");
  set_root_binding(&missing_key, rollup_semantic + "|19,21,23|-|-|-");
  set_root_binding(&key_order_drift,
                   rollup_semantic + "|20,19,21,23|-|-|-");
  set_root_binding(&duplicate_key,
                   rollup_semantic + "|19,19,21,23|-|-|-");
  set_root_binding(
      &semantic_shape_drift,
      EncodeHex("aggregate.grouped-int64-key-count-sum.v1") +
          "|19,20,21,23|-|-|-");
  for (auto& operand : count_function_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "21") {
      operand.value =
          "4|-|4|019de5fc-2400-72e4-8549-82b2eef5a777|-|-|-|-";
    }
  }
  for (auto& operand : sum_function_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "23") {
      operand.value =
          "4|22|5|019de5fc-2400-784a-9aec-371f8b95b7ea|-|-|-|-";
    }
  }
  for (auto& operand : non_nullable_rollup_key.operands) {
    if (operand.type == "relational_descriptor_v1" && operand.name == "2") {
      operand.value =
          "019f0000-0000-7300-8000-00000000e203|"
          "019f0000-0000-7400-8000-00000000e204|1|-|-|-|-|-";
    }
  }
  for (auto& operand : output_order_drift.operands) {
    if (operand.type == "relational_node_v1" && operand.name == "2") {
      operand.value = "5|0|1|2,1,4,5|-";
    }
  }

  const auto refused_atomically = [](const std::string_view label,
                                     sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    const bool refused =
        result.accepted && result.optimizer_admitted &&
        !result.optimizer_selected && !result.physical_dag_published &&
        !result.physical_dag_executed && !result.runtime_actuals_attached &&
        !result.canonical_result_published && !result.api_result.ok &&
        result.physical_node_count == 0 &&
        result.canonical_result_bytes.empty() &&
        HasApiDiagnostic(
            result,
            "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-PAYLOAD-V1");
    if (!refused) {
      std::cerr << "two-key ROLLUP refusal mismatch (" << label
                << "): accepted=" << result.accepted
                << ", admitted=" << result.optimizer_admitted
                << ", selected=" << result.optimizer_selected
                << ", published=" << result.physical_dag_published
                << ", executed=" << result.physical_dag_executed
                << ", api_ok=" << result.api_result.ok << '\n';
      for (const auto& diagnostic : result.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    return refused;
  };
  const auto refused_before_admission =
      [](sblr::SblrOperationEnvelope envelope) {
        const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
            {Context(), std::move(envelope), {}});
        return !result.accepted && !result.optimizer_admitted &&
               !result.optimizer_selected &&
               !result.physical_dag_published &&
               !result.physical_dag_executed &&
               !result.runtime_actuals_attached &&
               !result.canonical_result_published && !result.api_result.ok &&
               result.physical_node_count == 0 &&
               result.canonical_result_bytes.empty() &&
               HasApiDiagnostic(result, "SBLR.PLAN_TREE.INVALID_HANDLE");
      };
  return Require(
      refused_atomically("missing_key", std::move(missing_key)) &&
          refused_atomically("key_order", std::move(key_order_drift)) &&
          refused_atomically("count_function",
                             std::move(count_function_drift)) &&
          refused_atomically("sum_function", std::move(sum_function_drift)) &&
          refused_atomically("non_nullable_rollup_key",
                             std::move(non_nullable_rollup_key)) &&
          refused_atomically("semantic_shape",
                             std::move(semantic_shape_drift)) &&
          refused_before_admission(std::move(duplicate_key)) &&
          refused_before_admission(std::move(output_order_drift)),
      "shape-, key-, function-, nullability-, semantic-, or output-order-"
      "drifted two-key ROLLUP published partial evidence");
}

bool ValidateRollupCountSumGroupingValuesSpine() {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), RollupCountSumGroupingValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), RollupCountSumGroupingValuesEnvelope(), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }

  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached &&
          first.canonical_result_published && first.api_result.ok &&
          first.diagnostics.empty() && first.logical_node_count == 2 &&
          first.logical_property_count == 0 &&
          first.physical_node_count == 2 &&
          first.canonical_result_column_count == 7 &&
          first.canonical_result_row_count == 9,
      "VALUES ROLLUP grouping projections did not traverse the selected DAG");

  const auto& columns = first.api_result.result_shape.columns;
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      columns.size() == 7 && rows.size() == 9 &&
          columns[4].canonical_type_name == "int64" &&
          columns[4].encoded_descriptor.find("nullability=non_null") !=
              std::string::npos &&
          columns[5].canonical_type_name == "int64" &&
          columns[5].encoded_descriptor.find("nullability=non_null") !=
              std::string::npos &&
          columns[6].canonical_type_name == "int64" &&
          columns[6].encoded_descriptor.find("nullability=non_null") !=
              std::string::npos,
      "GROUPING/GROUPING_ID result descriptors lost non-null int64 identity");

  const auto value_matches = [](
                                 const api::EngineTypedValue& value,
                                 const std::optional<std::string_view> expected) {
    if (!expected.has_value()) {
      return value.state == api::EngineValueState::sql_null && value.is_null;
    }
    return value.state == api::EngineValueState::value && !value.is_null &&
           value.encoded_value == *expected;
  };
  const auto row_matches = [&](const std::size_t ordinal,
                               const std::optional<std::string_view> key_a,
                               const std::optional<std::string_view> key_b,
                               const std::string_view count,
                               const std::string_view sum,
                               const std::string_view grouping_a,
                               const std::string_view grouping_b,
                               const std::string_view grouping_id) {
    return ordinal < rows.size() && rows[ordinal].fields.size() == 7 &&
           rows[ordinal].fields[0].first == "key_a" &&
           rows[ordinal].fields[1].first == "key_b" &&
           rows[ordinal].fields[2].first == "row_count" &&
           rows[ordinal].fields[3].first == "total_amount" &&
           rows[ordinal].fields[4].first == "grouping_a" &&
           rows[ordinal].fields[5].first == "grouping_b" &&
           rows[ordinal].fields[6].first == "grouping_id" &&
           value_matches(rows[ordinal].fields[0].second, key_a) &&
           value_matches(rows[ordinal].fields[1].second, key_b) &&
           value_matches(rows[ordinal].fields[2].second, count) &&
           value_matches(rows[ordinal].fields[3].second, sum) &&
           value_matches(rows[ordinal].fields[4].second, grouping_a) &&
           value_matches(rows[ordinal].fields[5].second, grouping_b) &&
           value_matches(rows[ordinal].fields[6].second, grouping_id);
  };
  passed &= Require(
      row_matches(0, "1", "10", "2", "5", "0", "0", "0") &&
          row_matches(1, "1", "20", "1", "7", "0", "0", "0") &&
          row_matches(2, "1", std::nullopt, "1", "3", "0", "0", "0") &&
          row_matches(3, "2", "10", "1", "4", "0", "0", "0") &&
          row_matches(4, std::nullopt, "10", "1", "8", "0", "0", "0") &&
          row_matches(5, "1", std::nullopt, "4", "15", "0", "1", "1") &&
          row_matches(6, "2", std::nullopt, "1", "4", "0", "1", "1") &&
          row_matches(7, std::nullopt, std::nullopt, "1", "8", "0", "1",
                      "1") &&
          row_matches(8, std::nullopt, std::nullopt, "6", "27", "1", "1",
                      "3"),
      "GROUPING/GROUPING_ID lost data-NULL versus structural-NULL identity");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical grouping projections changed canonical plan/result bytes");
  return passed;
}

bool ValidateRollupCountSumGroupingRefusalIsAtomic() {
  auto missing_projection = RollupCountSumGroupingValuesEnvelope();
  auto grouping_operator_drift = RollupCountSumGroupingValuesEnvelope();
  auto grouping_child_drift = RollupCountSumGroupingValuesEnvelope();
  auto grouping_id_order_drift = RollupCountSumGroupingValuesEnvelope();
  auto nullable_indicator = RollupCountSumGroupingValuesEnvelope();
  auto semantic_shape_drift = RollupCountSumGroupingValuesEnvelope();
  auto output_order_drift = RollupCountSumGroupingValuesEnvelope();
  const auto grouping_semantic = EncodeHex(
      "aggregate.rollup-int64-keys-count-sum-grouping.v1");
  for (auto* envelope : {&missing_projection, &semantic_shape_drift}) {
    for (auto& operand : envelope->operands) {
      if (operand.type == "relational_node_binding_v1" &&
          operand.name == "2") {
        operand.value =
            (envelope == &missing_projection
                 ? grouping_semantic
                 : EncodeHex("aggregate.rollup-int64-keys-count-sum.v1")) +
            "|19,20,21,23,24,25|-|-|-";
      }
    }
  }
  for (auto& operand : grouping_operator_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "24") {
      operand.value = "5|19|6|-|-|-|67726f7570696e675f6964|-";
    }
  }
  for (auto& operand : grouping_child_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "24") {
      operand.value = "5|20|6|-|-|-|67726f7570696e67|-";
    }
  }
  for (auto& operand : grouping_id_order_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "26") {
      operand.value = "6|20,19|8|-|-|-|67726f7570696e675f6964|-";
    }
  }
  for (auto& operand : nullable_indicator.operands) {
    if (operand.type == "relational_descriptor_v1" && operand.name == "6") {
      operand.value =
          "019f0000-0000-7300-8000-00000000e20e|"
          "019f0000-0000-7400-8000-00000000e20f|2|-|-|-|-|-";
    }
  }
  for (auto& operand : output_order_drift.operands) {
    if (operand.type == "relational_node_v1" && operand.name == "2") {
      operand.value = "5|0|1|1,2,4,5,7,6,8|-";
    }
  }

  const auto refused_atomically = [](const std::string_view label,
                                     sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    const bool refused =
        result.accepted && result.optimizer_admitted &&
        !result.optimizer_selected && !result.physical_dag_published &&
        !result.physical_dag_executed && !result.runtime_actuals_attached &&
        !result.canonical_result_published && !result.api_result.ok &&
        result.physical_node_count == 0 &&
        result.canonical_result_bytes.empty() &&
        HasApiDiagnostic(
            result,
            "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-PAYLOAD-V1");
    if (!refused) {
      std::cerr << "grouping projection refusal mismatch (" << label
                << "): accepted=" << result.accepted
                << ", admitted=" << result.optimizer_admitted
                << ", selected=" << result.optimizer_selected
                << ", published=" << result.physical_dag_published
                << ", executed=" << result.physical_dag_executed
                << ", api_ok=" << result.api_result.ok << '\n';
      for (const auto& diagnostic : result.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    return refused;
  };
  const auto refused_before_admission =
      [](sblr::SblrOperationEnvelope envelope) {
        const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
            {Context(), std::move(envelope), {}});
        return !result.accepted && !result.optimizer_admitted &&
               !result.optimizer_selected &&
               !result.physical_dag_published &&
               !result.physical_dag_executed &&
               !result.runtime_actuals_attached &&
               !result.canonical_result_published && !result.api_result.ok &&
               result.physical_node_count == 0 &&
               result.canonical_result_bytes.empty() &&
               HasApiDiagnostic(result, "SBLR.PLAN_TREE.INVALID_HANDLE");
      };
  return Require(
      refused_atomically("missing_projection", std::move(missing_projection)) &&
          refused_atomically("grouping_operator",
                             std::move(grouping_operator_drift)) &&
          refused_atomically("grouping_child",
                             std::move(grouping_child_drift)) &&
          refused_atomically("grouping_id_order",
                             std::move(grouping_id_order_drift)) &&
          refused_atomically("nullable_indicator",
                             std::move(nullable_indicator)) &&
          refused_atomically("semantic_shape",
                             std::move(semantic_shape_drift)) &&
          refused_before_admission(std::move(output_order_drift)),
      "shape-, operator-, child-, descriptor-, semantic-, or output-drifted "
      "grouping projection published partial evidence");
}

bool ValidateCubeCountSumValuesSpine() {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), CubeCountSumValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), CubeCountSumValuesEnvelope(), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }

  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached && first.canonical_result_published &&
          first.api_result.ok && first.diagnostics.empty() &&
          first.logical_node_count == 2 && first.logical_property_count == 0 &&
          first.physical_node_count == 2 &&
          first.canonical_result_column_count == 4 &&
          first.canonical_result_row_count == 12,
      "VALUES two-key CUBE did not traverse its selected physical DAG");

  const auto& columns = first.api_result.result_shape.columns;
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      columns.size() == 4 && rows.size() == 12 &&
          columns[0].canonical_type_name == "int64" &&
          columns[0].encoded_descriptor.find("nullability=nullable") !=
              std::string::npos &&
          columns[1].canonical_type_name == "int64" &&
          columns[1].encoded_descriptor.find("nullability=nullable") !=
              std::string::npos &&
          columns[2].canonical_type_name == "int64" &&
          columns[2].encoded_descriptor.find("nullability=non_null") !=
              std::string::npos &&
          columns[3].canonical_type_name == "int64" &&
          columns[3].encoded_descriptor.find("nullability=nullable") !=
              std::string::npos,
      "two-key CUBE result descriptors lost key or aggregate identity");

  const auto value_matches = [](
                                 const api::EngineTypedValue& value,
                                 const std::optional<std::string_view> expected) {
    if (!expected.has_value()) {
      return value.state == api::EngineValueState::sql_null && value.is_null;
    }
    return value.state == api::EngineValueState::value && !value.is_null &&
           value.encoded_value == *expected;
  };
  const auto row_matches = [&](const std::size_t ordinal,
                               const std::optional<std::string_view> key_a,
                               const std::optional<std::string_view> key_b,
                               const std::string_view count,
                               const std::string_view sum) {
    return ordinal < rows.size() && rows[ordinal].fields.size() == 4 &&
           rows[ordinal].fields[0].first == "key_a" &&
           rows[ordinal].fields[1].first == "key_b" &&
           rows[ordinal].fields[2].first == "row_count" &&
           rows[ordinal].fields[3].first == "total_amount" &&
           value_matches(rows[ordinal].fields[0].second, key_a) &&
           value_matches(rows[ordinal].fields[1].second, key_b) &&
           value_matches(rows[ordinal].fields[2].second, count) &&
           value_matches(rows[ordinal].fields[3].second, sum);
  };
  passed &= Require(
      row_matches(0, "1", "10", "2", "5") &&
          row_matches(1, "1", "20", "1", "7") &&
          row_matches(2, "1", std::nullopt, "1", "3") &&
          row_matches(3, "2", "10", "1", "4") &&
          row_matches(4, std::nullopt, "10", "1", "8") &&
          row_matches(5, "1", std::nullopt, "4", "15") &&
          row_matches(6, "2", std::nullopt, "1", "4") &&
          row_matches(7, std::nullopt, std::nullopt, "1", "8") &&
          row_matches(8, std::nullopt, "10", "4", "17") &&
          row_matches(9, std::nullopt, "20", "1", "7") &&
          row_matches(10, std::nullopt, std::nullopt, "1", "3") &&
          row_matches(11, std::nullopt, std::nullopt, "6", "27"),
      "two-key CUBE lost full-key, both subtotal dimensions, grand total, "
      "data NULL, or aggregate state");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical two-key CUBE input changed canonical plan/result bytes");
  return passed;
}

bool ValidateCubeCountSumRefusalIsAtomic() {
  auto missing_key = CubeCountSumValuesEnvelope();
  auto key_order_drift = CubeCountSumValuesEnvelope();
  auto duplicate_key = CubeCountSumValuesEnvelope();
  auto non_nullable_cube_key = CubeCountSumValuesEnvelope();
  auto semantic_shape_drift = CubeCountSumValuesEnvelope();
  auto output_order_drift = CubeCountSumValuesEnvelope();
  const auto set_root_binding = [](sblr::SblrOperationEnvelope* envelope,
                                   const std::string& value) {
    for (auto& operand : envelope->operands) {
      if (operand.type == "relational_node_binding_v1" &&
          operand.name == "2") {
        operand.value = value;
      }
    }
  };
  const auto cube_semantic =
      EncodeHex("aggregate.cube-int64-keys-count-sum.v1");
  set_root_binding(&missing_key, cube_semantic + "|19,21,23|-|-|-");
  set_root_binding(&key_order_drift,
                   cube_semantic + "|20,19,21,23|-|-|-");
  set_root_binding(&duplicate_key,
                   cube_semantic + "|19,19,21,23|-|-|-");
  set_root_binding(
      &semantic_shape_drift,
      EncodeHex("aggregate.grouped-int64-key-count-sum.v1") +
          "|19,20,21,23|-|-|-");
  for (auto& operand : non_nullable_cube_key.operands) {
    if (operand.type == "relational_descriptor_v1" && operand.name == "1") {
      operand.value =
          "019f0000-0000-7300-8000-00000000e201|"
          "019f0000-0000-7400-8000-00000000e202|1|-|-|-|-|-";
    }
  }
  for (auto& operand : output_order_drift.operands) {
    if (operand.type == "relational_node_v1" && operand.name == "2") {
      operand.value = "5|0|1|2,1,4,5|-";
    }
  }

  const auto refused_atomically = [](const std::string_view label,
                                     sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    const bool refused =
        result.accepted && result.optimizer_admitted &&
        !result.optimizer_selected && !result.physical_dag_published &&
        !result.physical_dag_executed && !result.runtime_actuals_attached &&
        !result.canonical_result_published && !result.api_result.ok &&
        result.physical_node_count == 0 &&
        result.canonical_result_bytes.empty() &&
        HasApiDiagnostic(
            result,
            "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-PAYLOAD-V1");
    if (!refused) {
      std::cerr << "two-key CUBE refusal mismatch (" << label
                << "): accepted=" << result.accepted
                << ", admitted=" << result.optimizer_admitted
                << ", selected=" << result.optimizer_selected
                << ", published=" << result.physical_dag_published
                << ", executed=" << result.physical_dag_executed
                << ", api_ok=" << result.api_result.ok << '\n';
      for (const auto& diagnostic : result.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    return refused;
  };
  const auto refused_before_admission =
      [](sblr::SblrOperationEnvelope envelope) {
        const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
            {Context(), std::move(envelope), {}});
        return !result.accepted && !result.optimizer_admitted &&
               !result.optimizer_selected &&
               !result.physical_dag_published &&
               !result.physical_dag_executed &&
               !result.runtime_actuals_attached &&
               !result.canonical_result_published && !result.api_result.ok &&
               result.physical_node_count == 0 &&
               result.canonical_result_bytes.empty() &&
               HasApiDiagnostic(result, "SBLR.PLAN_TREE.INVALID_HANDLE");
      };
  return Require(
      refused_atomically("missing_key", std::move(missing_key)) &&
          refused_atomically("key_order", std::move(key_order_drift)) &&
          refused_atomically("non_nullable_cube_key",
                             std::move(non_nullable_cube_key)) &&
          refused_atomically("semantic_shape",
                             std::move(semantic_shape_drift)) &&
          refused_before_admission(std::move(duplicate_key)) &&
          refused_before_admission(std::move(output_order_drift)),
      "shape-, key-, nullability-, semantic-, or output-order-drifted "
      "two-key CUBE published partial evidence");
}

bool ValidateCubeCountSumGroupingValuesSpine() {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), CubeCountSumGroupingValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), CubeCountSumGroupingValuesEnvelope(), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }

  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached && first.canonical_result_published &&
          first.api_result.ok && first.diagnostics.empty() &&
          first.logical_node_count == 2 && first.logical_property_count == 0 &&
          first.physical_node_count == 2 &&
          first.canonical_result_column_count == 7 &&
          first.canonical_result_row_count == 12,
      "VALUES CUBE grouping projections did not traverse the selected DAG");

  const auto& columns = first.api_result.result_shape.columns;
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      columns.size() == 7 && rows.size() == 12 &&
          columns[4].canonical_type_name == "int64" &&
          columns[4].encoded_descriptor.find("nullability=non_null") !=
              std::string::npos &&
          columns[5].canonical_type_name == "int64" &&
          columns[5].encoded_descriptor.find("nullability=non_null") !=
              std::string::npos &&
          columns[6].canonical_type_name == "int64" &&
          columns[6].encoded_descriptor.find("nullability=non_null") !=
              std::string::npos,
      "CUBE GROUPING/GROUPING_ID descriptors lost non-null int64 identity");

  const auto value_matches = [](
                                 const api::EngineTypedValue& value,
                                 const std::optional<std::string_view> expected) {
    if (!expected.has_value()) {
      return value.state == api::EngineValueState::sql_null && value.is_null;
    }
    return value.state == api::EngineValueState::value && !value.is_null &&
           value.encoded_value == *expected;
  };
  const auto row_matches = [&](const std::size_t ordinal,
                               const std::optional<std::string_view> key_a,
                               const std::optional<std::string_view> key_b,
                               const std::string_view count,
                               const std::string_view sum,
                               const std::string_view grouping_a,
                               const std::string_view grouping_b,
                               const std::string_view grouping_id) {
    return ordinal < rows.size() && rows[ordinal].fields.size() == 7 &&
           rows[ordinal].fields[0].first == "key_a" &&
           rows[ordinal].fields[1].first == "key_b" &&
           rows[ordinal].fields[2].first == "row_count" &&
           rows[ordinal].fields[3].first == "total_amount" &&
           rows[ordinal].fields[4].first == "grouping_a" &&
           rows[ordinal].fields[5].first == "grouping_b" &&
           rows[ordinal].fields[6].first == "grouping_id" &&
           value_matches(rows[ordinal].fields[0].second, key_a) &&
           value_matches(rows[ordinal].fields[1].second, key_b) &&
           value_matches(rows[ordinal].fields[2].second, count) &&
           value_matches(rows[ordinal].fields[3].second, sum) &&
           value_matches(rows[ordinal].fields[4].second, grouping_a) &&
           value_matches(rows[ordinal].fields[5].second, grouping_b) &&
           value_matches(rows[ordinal].fields[6].second, grouping_id);
  };
  passed &= Require(
      row_matches(0, "1", "10", "2", "5", "0", "0", "0") &&
          row_matches(1, "1", "20", "1", "7", "0", "0", "0") &&
          row_matches(2, "1", std::nullopt, "1", "3", "0", "0", "0") &&
          row_matches(3, "2", "10", "1", "4", "0", "0", "0") &&
          row_matches(4, std::nullopt, "10", "1", "8", "0", "0", "0") &&
          row_matches(5, "1", std::nullopt, "4", "15", "0", "1", "1") &&
          row_matches(6, "2", std::nullopt, "1", "4", "0", "1", "1") &&
          row_matches(7, std::nullopt, std::nullopt, "1", "8", "0", "1",
                      "1") &&
          row_matches(8, std::nullopt, "10", "4", "17", "1", "0", "2") &&
          row_matches(9, std::nullopt, "20", "1", "7", "1", "0", "2") &&
          row_matches(10, std::nullopt, std::nullopt, "1", "3", "1", "0",
                      "2") &&
          row_matches(11, std::nullopt, std::nullopt, "6", "27", "1", "1",
                      "3"),
      "CUBE GROUPING metadata lost data-NULL versus either structural-NULL "
      "dimension");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical CUBE grouping projections changed plan/result bytes");
  return passed;
}

bool ValidateCubeCountSumGroupingRefusalIsAtomic() {
  auto missing_projection = CubeCountSumGroupingValuesEnvelope();
  auto grouping_operator_drift = CubeCountSumGroupingValuesEnvelope();
  auto grouping_child_drift = CubeCountSumGroupingValuesEnvelope();
  auto grouping_id_order_drift = CubeCountSumGroupingValuesEnvelope();
  auto nullable_indicator = CubeCountSumGroupingValuesEnvelope();
  auto semantic_shape_drift = CubeCountSumGroupingValuesEnvelope();
  auto output_order_drift = CubeCountSumGroupingValuesEnvelope();
  const auto grouping_semantic =
      EncodeHex("aggregate.cube-int64-keys-count-sum-grouping.v1");
  for (auto* envelope : {&missing_projection, &semantic_shape_drift}) {
    for (auto& operand : envelope->operands) {
      if (operand.type == "relational_node_binding_v1" &&
          operand.name == "2") {
        operand.value =
            (envelope == &missing_projection
                 ? grouping_semantic
                 : EncodeHex("aggregate.cube-int64-keys-count-sum.v1")) +
            "|19,20,21,23,24,25|-|-|-";
      }
    }
  }
  for (auto& operand : grouping_operator_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "25") {
      operand.value = "5|20|7|-|-|-|67726f7570696e675f6964|-";
    }
  }
  for (auto& operand : grouping_child_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "25") {
      operand.value = "5|19|7|-|-|-|67726f7570696e67|-";
    }
  }
  for (auto& operand : grouping_id_order_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "26") {
      operand.value = "6|20,19|8|-|-|-|67726f7570696e675f6964|-";
    }
  }
  for (auto& operand : nullable_indicator.operands) {
    if (operand.type == "relational_descriptor_v1" && operand.name == "7") {
      operand.value =
          "019f0000-0000-7300-8000-00000000e210|"
          "019f0000-0000-7400-8000-00000000e211|2|-|-|-|-|-";
    }
  }
  for (auto& operand : output_order_drift.operands) {
    if (operand.type == "relational_node_v1" && operand.name == "2") {
      operand.value = "5|0|1|1,2,4,5,6,8,7|-";
    }
  }

  const auto refused_atomically = [](const std::string_view label,
                                     sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    const bool refused =
        result.accepted && result.optimizer_admitted &&
        !result.optimizer_selected && !result.physical_dag_published &&
        !result.physical_dag_executed && !result.runtime_actuals_attached &&
        !result.canonical_result_published && !result.api_result.ok &&
        result.physical_node_count == 0 &&
        result.canonical_result_bytes.empty() &&
        HasApiDiagnostic(
            result,
            "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-PAYLOAD-V1");
    if (!refused) {
      std::cerr << "CUBE grouping projection refusal mismatch (" << label
                << "): accepted=" << result.accepted
                << ", admitted=" << result.optimizer_admitted
                << ", selected=" << result.optimizer_selected
                << ", published=" << result.physical_dag_published
                << ", executed=" << result.physical_dag_executed
                << ", api_ok=" << result.api_result.ok << '\n';
      for (const auto& diagnostic : result.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    return refused;
  };
  const auto refused_before_admission =
      [](sblr::SblrOperationEnvelope envelope) {
        const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
            {Context(), std::move(envelope), {}});
        return !result.accepted && !result.optimizer_admitted &&
               !result.optimizer_selected &&
               !result.physical_dag_published &&
               !result.physical_dag_executed &&
               !result.runtime_actuals_attached &&
               !result.canonical_result_published && !result.api_result.ok &&
               result.physical_node_count == 0 &&
               result.canonical_result_bytes.empty() &&
               HasApiDiagnostic(result, "SBLR.PLAN_TREE.INVALID_HANDLE");
      };
  return Require(
      refused_atomically("missing_projection", std::move(missing_projection)) &&
          refused_atomically("grouping_operator",
                             std::move(grouping_operator_drift)) &&
          refused_atomically("grouping_child",
                             std::move(grouping_child_drift)) &&
          refused_atomically("grouping_id_order",
                             std::move(grouping_id_order_drift)) &&
          refused_atomically("nullable_indicator",
                             std::move(nullable_indicator)) &&
          refused_atomically("semantic_shape",
                             std::move(semantic_shape_drift)) &&
          refused_before_admission(std::move(output_order_drift)),
      "shape-, operator-, child-, descriptor-, semantic-, or output-drifted "
      "CUBE grouping projection published partial evidence");
}

bool ValidateGroupingSetsCountSumValuesSpine() {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GroupingSetsCountSumValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GroupingSetsCountSumValuesEnvelope(), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }

  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached && first.canonical_result_published &&
          first.api_result.ok && first.diagnostics.empty() &&
          first.logical_node_count == 2 && first.logical_property_count == 0 &&
          first.physical_node_count == 2 &&
          first.canonical_result_column_count == 4 &&
          first.canonical_result_row_count == 12,
      "typed arbitrary GROUPING SETS did not traverse its selected DAG");

  const auto& columns = first.api_result.result_shape.columns;
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      columns.size() == 4 && rows.size() == 12 &&
          columns[0].canonical_type_name == "int64" &&
          columns[0].encoded_descriptor.find("nullability=nullable") !=
              std::string::npos &&
          columns[1].canonical_type_name == "int64" &&
          columns[1].encoded_descriptor.find("nullability=nullable") !=
              std::string::npos &&
          columns[2].canonical_type_name == "int64" &&
          columns[2].encoded_descriptor.find("nullability=non_null") !=
              std::string::npos &&
          columns[3].canonical_type_name == "int64" &&
          columns[3].encoded_descriptor.find("nullability=nullable") !=
              std::string::npos,
      "GROUPING SETS result descriptors lost key or aggregate identity");

  const auto value_matches = [](
                                 const api::EngineTypedValue& value,
                                 const std::optional<std::string_view> expected) {
    if (!expected.has_value()) {
      return value.state == api::EngineValueState::sql_null && value.is_null;
    }
    return value.state == api::EngineValueState::value && !value.is_null &&
           value.encoded_value == *expected;
  };
  const auto row_matches = [&](const std::size_t ordinal,
                               const std::optional<std::string_view> key_a,
                               const std::optional<std::string_view> key_b,
                               const std::string_view count,
                               const std::string_view sum) {
    return ordinal < rows.size() && rows[ordinal].fields.size() == 4 &&
           rows[ordinal].fields[0].first == "key_a" &&
           rows[ordinal].fields[1].first == "key_b" &&
           rows[ordinal].fields[2].first == "row_count" &&
           rows[ordinal].fields[3].first == "total_amount" &&
           value_matches(rows[ordinal].fields[0].second, key_a) &&
           value_matches(rows[ordinal].fields[1].second, key_b) &&
           value_matches(rows[ordinal].fields[2].second, count) &&
           value_matches(rows[ordinal].fields[3].second, sum);
  };
  passed &= Require(
      row_matches(0, std::nullopt, "10", "4", "17") &&
          row_matches(1, std::nullopt, "20", "1", "7") &&
          row_matches(2, std::nullopt, std::nullopt, "1", "3") &&
          row_matches(3, std::nullopt, std::nullopt, "6", "27") &&
          row_matches(4, "1", "10", "2", "5") &&
          row_matches(5, "1", "20", "1", "7") &&
          row_matches(6, "1", std::nullopt, "1", "3") &&
          row_matches(7, "2", "10", "1", "4") &&
          row_matches(8, std::nullopt, "10", "1", "8") &&
          row_matches(9, std::nullopt, "10", "4", "17") &&
          row_matches(10, std::nullopt, "20", "1", "7") &&
          row_matches(11, std::nullopt, std::nullopt, "1", "3"),
      "GROUPING SETS lost declared order, repeated-set multiplicity, data "
      "NULL, grand total, or aggregate state");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical typed GROUPING SETS changed canonical plan/result bytes");
  return passed;
}

bool ValidateGroupingSetsCountSumGroupingValuesSpine() {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GroupingSetsCountSumGroupingValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GroupingSetsCountSumGroupingValuesEnvelope(), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }

  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached && first.canonical_result_published &&
          first.api_result.ok && first.diagnostics.empty() &&
          first.logical_node_count == 2 && first.logical_property_count == 0 &&
          first.physical_node_count == 2 &&
          first.canonical_result_column_count == 7 &&
          first.canonical_result_row_count == 12,
      "typed GROUPING SETS metadata projection did not traverse its DAG");

  const auto& columns = first.api_result.result_shape.columns;
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      columns.size() == 7 && rows.size() == 12 &&
          columns[4].canonical_type_name == "int64" &&
          columns[4].encoded_descriptor.find("nullability=non_null") !=
              std::string::npos &&
          columns[5].canonical_type_name == "int64" &&
          columns[5].encoded_descriptor.find("nullability=non_null") !=
              std::string::npos &&
          columns[6].canonical_type_name == "int64" &&
          columns[6].encoded_descriptor.find("nullability=non_null") !=
              std::string::npos,
      "GROUPING SETS metadata descriptors lost non-null int64 identity");

  const auto value_matches = [](
                                 const api::EngineTypedValue& value,
                                 const std::optional<std::string_view> expected) {
    if (!expected.has_value()) {
      return value.state == api::EngineValueState::sql_null && value.is_null;
    }
    return value.state == api::EngineValueState::value && !value.is_null &&
           value.encoded_value == *expected;
  };
  const auto row_matches = [&](const std::size_t ordinal,
                               const std::optional<std::string_view> key_a,
                               const std::optional<std::string_view> key_b,
                               const std::string_view count,
                               const std::string_view sum,
                               const std::string_view grouping_a,
                               const std::string_view grouping_b,
                               const std::string_view grouping_id) {
    return ordinal < rows.size() && rows[ordinal].fields.size() == 7 &&
           rows[ordinal].fields[0].first == "key_a" &&
           rows[ordinal].fields[1].first == "key_b" &&
           rows[ordinal].fields[2].first == "row_count" &&
           rows[ordinal].fields[3].first == "total_amount" &&
           rows[ordinal].fields[4].first == "grouping_a" &&
           rows[ordinal].fields[5].first == "grouping_b" &&
           rows[ordinal].fields[6].first == "grouping_id" &&
           value_matches(rows[ordinal].fields[0].second, key_a) &&
           value_matches(rows[ordinal].fields[1].second, key_b) &&
           value_matches(rows[ordinal].fields[2].second, count) &&
           value_matches(rows[ordinal].fields[3].second, sum) &&
           value_matches(rows[ordinal].fields[4].second, grouping_a) &&
           value_matches(rows[ordinal].fields[5].second, grouping_b) &&
           value_matches(rows[ordinal].fields[6].second, grouping_id);
  };
  passed &= Require(
      row_matches(0, std::nullopt, "10", "4", "17", "1", "0", "2") &&
          row_matches(1, std::nullopt, "20", "1", "7", "1", "0", "2") &&
          row_matches(2, std::nullopt, std::nullopt, "1", "3", "1", "0",
                      "2") &&
          row_matches(3, std::nullopt, std::nullopt, "6", "27", "1", "1",
                      "3") &&
          row_matches(4, "1", "10", "2", "5", "0", "0", "0") &&
          row_matches(5, "1", "20", "1", "7", "0", "0", "0") &&
          row_matches(6, "1", std::nullopt, "1", "3", "0", "0", "0") &&
          row_matches(7, "2", "10", "1", "4", "0", "0", "0") &&
          row_matches(8, std::nullopt, "10", "1", "8", "0", "0", "0") &&
          row_matches(9, std::nullopt, "10", "4", "17", "1", "0", "2") &&
          row_matches(10, std::nullopt, "20", "1", "7", "1", "0", "2") &&
          row_matches(11, std::nullopt, std::nullopt, "1", "3", "1", "0",
                      "2"),
      "GROUPING/GROUPING_ID lost structural NULL, data NULL, declared order, "
      "or repeated-set multiplicity");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical GROUPING SETS metadata changed plan/result bytes");
  return passed;
}

bool ValidateGroupingSetsCountSumRefusalIsAtomic() {
  auto missing_sets = GroupingSetsCountSumValuesEnvelope();
  auto aggregate_member = GroupingSetsCountSumValuesEnvelope();
  auto unused_key = GroupingSetsCountSumValuesEnvelope();
  auto fixed_profile_payload = CubeCountSumValuesEnvelope();
  auto duplicate_ordinal = GroupingSetsCountSumValuesEnvelope();
  auto sparse_ordinal = GroupingSetsCountSumValuesEnvelope();
  auto duplicate_member = GroupingSetsCountSumValuesEnvelope();
  auto reversed_members = GroupingSetsCountSumValuesEnvelope();

  std::erase_if(missing_sets.operands, [](const auto& operand) {
    return operand.type == "relational_grouping_set_v1";
  });
  for (auto& operand : aggregate_member.operands) {
    if (operand.type == "relational_grouping_set_v1" &&
        operand.name == "0") {
      operand.value = "2|21";
    }
  }
  for (auto& operand : unused_key.operands) {
    if (operand.type == "relational_grouping_set_v1" &&
        operand.name == "2") {
      operand.value = "2|20";
    }
  }
  fixed_profile_payload.operands.insert(
      fixed_profile_payload.operands.end(),
      {{"relational_grouping_set_v1", "0", "2|19,20"},
       {"relational_grouping_set_v1", "1", "2|19"},
       {"relational_grouping_set_v1", "2", "2|20"},
       {"relational_grouping_set_v1", "3", "2|-"}});
  for (auto& operand : duplicate_ordinal.operands) {
    if (operand.type == "relational_grouping_set_v1" &&
        operand.name == "1") {
      operand.name = "0";
    }
  }
  for (auto& operand : sparse_ordinal.operands) {
    if (operand.type == "relational_grouping_set_v1" &&
        operand.name == "1") {
      operand.name = "4";
    }
  }
  for (auto& operand : duplicate_member.operands) {
    if (operand.type == "relational_grouping_set_v1" &&
        operand.name == "2") {
      operand.value = "2|19,19";
    }
  }
  for (auto& operand : reversed_members.operands) {
    if (operand.type == "relational_grouping_set_v1" &&
        operand.name == "2") {
      operand.value = "2|20,19";
    }
  }

  const auto refused_after_admission = [](
                                          const std::string_view label,
                                          sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    const bool refused =
        result.accepted && result.optimizer_admitted &&
        !result.optimizer_selected && !result.physical_dag_published &&
        !result.physical_dag_executed && !result.runtime_actuals_attached &&
        !result.canonical_result_published && !result.api_result.ok &&
        result.physical_node_count == 0 &&
        result.canonical_result_bytes.empty() &&
        HasApiDiagnostic(
            result,
            "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-PAYLOAD-V1");
    if (!refused) {
      std::cerr << "GROUPING SETS post-admission refusal mismatch (" << label
                << ")\n";
    }
    return refused;
  };
  const auto refused_before_admission = [](
                                           const std::string_view label,
                                           sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    const bool refused =
        !result.accepted && !result.optimizer_admitted &&
        !result.optimizer_selected && !result.physical_dag_published &&
        !result.physical_dag_executed && !result.runtime_actuals_attached &&
        !result.canonical_result_published && !result.api_result.ok &&
        result.physical_node_count == 0 &&
        result.canonical_result_bytes.empty() &&
        HasApiDiagnostic(result, "SBLR.PLAN_TREE.INVALID_HANDLE");
    if (!refused) {
      std::cerr << "GROUPING SETS pre-admission refusal mismatch (" << label
                << ")\n";
    }
    return refused;
  };
  return Require(
      refused_after_admission("missing_sets", std::move(missing_sets)) &&
          refused_after_admission("aggregate_member",
                                  std::move(aggregate_member)) &&
          refused_after_admission("unused_key", std::move(unused_key)) &&
          refused_after_admission("fixed_profile_payload",
                                  std::move(fixed_profile_payload)) &&
          refused_before_admission("duplicate_ordinal",
                                   std::move(duplicate_ordinal)) &&
          refused_before_admission("sparse_ordinal",
                                   std::move(sparse_ordinal)) &&
          refused_before_admission("duplicate_member",
                                   std::move(duplicate_member)) &&
          refused_before_admission("reversed_members",
                                   std::move(reversed_members)),
      "missing, misplaced, unused, duplicate, sparse, or reordered GROUPING "
      "SETS payload published partial evidence");
}

bool ValidateGlobalCountStarValuesSpine() {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GlobalCountStarValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GlobalCountStarValuesEnvelope(), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached && first.canonical_result_published &&
          first.api_result.ok && first.diagnostics.empty() &&
          first.logical_node_count == 2 && first.logical_property_count == 0 &&
          first.physical_node_count == 2 &&
          first.canonical_result_column_count == 1 &&
          first.canonical_result_row_count == 1,
      "VALUES global COUNT(*) did not traverse its selected physical DAG");
  const auto& columns = first.api_result.result_shape.columns;
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      columns.size() == 1 && rows.size() == 1 &&
          columns[0].canonical_type_name == "int64" &&
          rows[0].fields.size() == 1 &&
          rows[0].fields[0].first == "row_count" &&
          rows[0].fields[0].second.state == api::EngineValueState::value &&
          !rows[0].fields[0].second.is_null &&
          rows[0].fields[0].second.encoded_value == "3",
      "global COUNT(*) did not count all physical rows including SQL NULL");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical global COUNT(*) input changed canonical plan/result bytes");
  return passed;
}

bool ValidateGlobalCountStarRefusalIsAtomic() {
  auto function_drift = GlobalCountStarValuesEnvelope();
  auto result_nullability_drift = GlobalCountStarValuesEnvelope();
  auto argument_drift = GlobalCountStarValuesEnvelope();
  for (auto& operand : function_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "4") {
      operand.value =
          "4|-|2|019de5fc-2400-78ac-b50c-45b832831004|-|-|-|-";
    }
  }
  for (auto& operand : result_nullability_drift.operands) {
    if (operand.type == "relational_descriptor_v1" && operand.name == "2") {
      operand.value =
          "019f0000-0000-7300-8000-000000009003|"
          "019f0000-0000-7400-8000-000000009004|2|-|-|-|-|-";
    }
  }
  for (auto& operand : argument_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "4") {
      operand.value =
          "4|1|2|019de5fc-2400-784a-9aec-371f8b95b7ea|-|-|-|-";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(function_drift)) &&
          refused_atomically(std::move(result_nullability_drift)) &&
          refused_atomically(std::move(argument_drift)),
      "function-, descriptor-, or arity-drifted COUNT(*) published evidence");
}

bool ValidateGlobalCountExpressionValuesSpine() {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GlobalCountExpressionValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GlobalCountExpressionValuesEnvelope(), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached && first.canonical_result_published &&
          first.api_result.ok && first.diagnostics.empty() &&
          first.logical_node_count == 2 && first.logical_property_count == 0 &&
          first.physical_node_count == 2 &&
          first.canonical_result_column_count == 1 &&
          first.canonical_result_row_count == 1,
      "VALUES global COUNT(expression) did not traverse its selected "
      "physical DAG");
  const auto& columns = first.api_result.result_shape.columns;
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      columns.size() == 1 && rows.size() == 1 &&
          columns[0].canonical_type_name == "int64" &&
          rows[0].fields.size() == 1 &&
          rows[0].fields[0].first == "non_null_count" &&
          rows[0].fields[0].second.state == api::EngineValueState::value &&
          !rows[0].fields[0].second.is_null &&
          rows[0].fields[0].second.encoded_value == "2",
      "global COUNT(expression) did not exclude SQL NULL input values");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical global COUNT(expression) input changed canonical plan/result "
      "bytes");
  return passed;
}

bool ValidateGlobalCountExpressionRefusalIsAtomic() {
  auto function_drift = GlobalCountExpressionValuesEnvelope();
  auto descriptor_drift = GlobalCountExpressionValuesEnvelope();
  auto non_identifier_argument = GlobalCountExpressionValuesEnvelope();
  for (auto& operand : function_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "6") {
      operand.value =
          "4|5|2|019de5fc-2400-78ac-b50c-45b832831004|-|-|-|-";
    }
  }
  for (auto& operand : descriptor_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "5") {
      operand.value =
          "3|-|2|-|019f0000-0000-7500-8000-000000009105|-|-|-";
    }
  }
  for (auto& operand : non_identifier_argument.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "5") {
      operand.value = "1|-|1|-|-|1|-|31";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(function_drift)) &&
          refused_atomically(std::move(descriptor_drift)) &&
          refused_atomically(std::move(non_identifier_argument)),
      "function-, descriptor-, or argument-kind-drifted COUNT(expression) "
      "published evidence");
}

bool ValidateGlobalSumExpressionValuesSpine() {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GlobalSumExpressionValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GlobalSumExpressionValuesEnvelope(), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached && first.canonical_result_published &&
          first.api_result.ok && first.diagnostics.empty() &&
          first.logical_node_count == 2 && first.logical_property_count == 0 &&
          first.physical_node_count == 2 &&
          first.canonical_result_column_count == 1 &&
          first.canonical_result_row_count == 1,
      "VALUES global SUM(expression) did not traverse its selected physical "
      "DAG");
  const auto& columns = first.api_result.result_shape.columns;
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      columns.size() == 1 && rows.size() == 1 &&
          columns[0].canonical_type_name == "int64" &&
          columns[0].encoded_descriptor.find("nullability=nullable") !=
              std::string::npos &&
          rows[0].fields.size() == 1 &&
          rows[0].fields[0].first == "sum_value" &&
          rows[0].fields[0].second.state == api::EngineValueState::value &&
          !rows[0].fields[0].second.is_null &&
          rows[0].fields[0].second.encoded_value == "10",
      "global SUM(expression) did not ignore SQL NULL and preserve signed "
      "int64 state");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical global SUM(expression) input changed canonical plan/result "
      "bytes");
  return passed;
}

bool ValidateGlobalSumExpressionRefusalIsAtomic() {
  auto function_drift = GlobalSumExpressionValuesEnvelope();
  auto result_nullability_drift = GlobalSumExpressionValuesEnvelope();
  auto non_integer_input = GlobalSumExpressionValuesEnvelope();
  for (auto& operand : function_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "6") {
      operand.value =
          "4|5|2|019de5fc-2400-784a-9aec-371f8b95b7ea|-|-|-|-";
    }
  }
  for (auto& operand : result_nullability_drift.operands) {
    if (operand.type == "relational_descriptor_v1" && operand.name == "2") {
      operand.value =
          "019f0000-0000-7300-8000-000000009203|"
          "019f0000-0000-7400-8000-000000009204|1|-|-|-|-|-";
    }
  }
  for (auto& operand : non_integer_input.operands) {
    if (operand.type == "relational_expression_v1" &&
        (operand.name == "1" || operand.name == "3" ||
         operand.name == "4")) {
      if (operand.name == "1") operand.value = "1|-|1|-|-|2|-|35";
      if (operand.name == "3") operand.value = "1|-|1|-|-|2|-|2d32";
      if (operand.name == "4") operand.value = "1|-|1|-|-|2|-|37";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(function_drift)) &&
          refused_atomically(std::move(result_nullability_drift)) &&
          refused_atomically(std::move(non_integer_input)),
      "function-, result-, or input-type-drifted SUM(expression) published "
      "evidence");
}

bool UnaryAggregateModifierOutputMatches(
    const UnaryAggregateTestProfile& target,
    const AggregateModifierProfile profile,
    const std::string& actual) {
  if (target.result_type == "real64") {
    char* parse_end = nullptr;
    const double decoded = std::strtod(actual.c_str(), &parse_end);
    const std::array<double, 4> values =
        profile == AggregateModifierProfile::kFilter
            ? std::array<double, 4>{10.0, 20.0, 40.0, 40.0}
            : profile == AggregateModifierProfile::kDistinct
                  ? std::array<double, 4>{10.0, 20.0, 30.0, 40.0}
                  : std::array<double, 4>{10.0, 20.0, 40.0, 0.0};
    const std::size_t value_count =
        profile == AggregateModifierProfile::kDistinctFilter ? 3 : 4;
    double sum = 0.0;
    for (std::size_t index = 0; index < value_count; ++index) {
      sum += values[index];
    }
    const double mean = sum / static_cast<double>(value_count);
    double squared_deviation_sum = 0.0;
    for (std::size_t index = 0; index < value_count; ++index) {
      const double deviation = values[index] - mean;
      squared_deviation_sum += deviation * deviation;
    }
    const bool population =
        target.kind == UnaryAggregateKind::kStddevPop ||
        target.kind == UnaryAggregateKind::kVariancePop;
    const bool deviation =
        target.kind == UnaryAggregateKind::kStddevPop ||
        target.kind == UnaryAggregateKind::kStddev ||
        target.kind == UnaryAggregateKind::kStddevSamp;
    const double variance = squared_deviation_sum /
                            static_cast<double>(population ? value_count
                                                           : value_count - 1);
    const double expected =
        target.kind == UnaryAggregateKind::kAvg
            ? mean
            : (deviation ? std::sqrt(variance) : variance);
    const double tolerance =
        target.kind == UnaryAggregateKind::kAvg ? 1e-12 : 1e-9;
    return parse_end == actual.c_str() + actual.size() &&
           std::abs(decoded - expected) <= tolerance;
  }
  std::string_view expected;
  switch (target.kind) {
    case UnaryAggregateKind::kCount:
      expected = profile == AggregateModifierProfile::kDistinctFilter
                     ? "3"
                     : "4";
      break;
    case UnaryAggregateKind::kSum:
      expected = profile == AggregateModifierProfile::kFilter
                     ? "110"
                     : profile == AggregateModifierProfile::kDistinct
                           ? "100"
                           : "70";
      break;
    case UnaryAggregateKind::kMin:
      expected = "10";
      break;
    case UnaryAggregateKind::kMax:
      expected = "40";
      break;
    case UnaryAggregateKind::kBoolAnd:
    case UnaryAggregateKind::kEvery:
      expected = "false";
      break;
    case UnaryAggregateKind::kBoolOr:
      expected = "true";
      break;
    case UnaryAggregateKind::kAvg:
    case UnaryAggregateKind::kStddevPop:
    case UnaryAggregateKind::kVariancePop:
    case UnaryAggregateKind::kStddev:
    case UnaryAggregateKind::kVariance:
    case UnaryAggregateKind::kStddevSamp:
    case UnaryAggregateKind::kVarianceSamp:
      return false;
  }
  return actual == expected;
}

bool ValidateGlobalUnaryAggregateModifierValuesSpine() {
  bool passed = true;
  for (const auto& target : kUnaryAggregateTestProfiles) {
    for (const auto profile : {AggregateModifierProfile::kFilter,
                               AggregateModifierProfile::kDistinct,
                               AggregateModifierProfile::kDistinctFilter}) {
      const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(),
           GlobalUnaryAggregateModifierValuesEnvelope(target, profile), {}});
      const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(),
           GlobalUnaryAggregateModifierValuesEnvelope(target, profile), {}});
      if (!first.api_result.ok) {
        for (const auto& diagnostic : first.api_result.diagnostics) {
          std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
        }
      }
      const auto& columns = first.api_result.result_shape.columns;
      const auto& rows = first.api_result.result_shape.rows;
      const bool exact_output =
          columns.size() == 1 && rows.size() == 1 &&
          columns[0].canonical_type_name == target.result_type &&
          columns[0].encoded_descriptor.find(
              target.result_nullable ? "nullability=nullable"
                                     : "nullability=non_null") !=
              std::string::npos &&
          rows[0].fields.size() == 1 &&
          rows[0].fields[0].first == target.output_name &&
          rows[0].fields[0].second.state == api::EngineValueState::value &&
          !rows[0].fields[0].second.is_null &&
          UnaryAggregateModifierOutputMatches(
              target, profile, rows[0].fields[0].second.encoded_value);
      passed &= Require(
          first.accepted && first.optimizer_admitted &&
              first.optimizer_selected && first.physical_dag_published &&
              first.physical_dag_executed &&
              first.runtime_actuals_attached &&
              first.canonical_result_published && first.api_result.ok &&
              first.diagnostics.empty() && first.logical_node_count == 2 &&
              first.logical_property_count == 0 &&
              first.physical_node_count == 2 && exact_output,
          "global " + std::string(target.name) + " " +
              std::string(AggregateModifierName(profile)) +
              " did not execute through the selected canonical aggregate "
              "spine");
      passed &= Require(
          repeated.api_result.ok &&
              repeated.selected_plan_uuid == first.selected_plan_uuid &&
              repeated.canonical_result_bytes == first.canonical_result_bytes,
          "identical global " + std::string(target.name) + " " +
              std::string(AggregateModifierName(profile)) +
              " input changed canonical plan/result bytes");
    }
  }
  return passed;
}

bool ValidateGlobalUnaryAggregateModifierRefusalIsAtomic() {
  const auto& sum_target = kUnaryAggregateTestProfiles[1];
  const auto& count_target = kUnaryAggregateTestProfiles[0];
  const auto& bool_and_target = kUnaryAggregateTestProfiles[5];
  const auto& stddev_pop_target = kUnaryAggregateTestProfiles[8];
  auto non_boolean_filter =
      GlobalUnaryAggregateModifierValuesEnvelope(
          sum_target, AggregateModifierProfile::kFilter);
  auto non_identifier_filter =
      GlobalUnaryAggregateModifierValuesEnvelope(
          sum_target, AggregateModifierProfile::kFilter);
  auto filter_arity_drift =
      GlobalUnaryAggregateModifierValuesEnvelope(
          sum_target, AggregateModifierProfile::kFilter);
  auto distinct_arity_drift =
      GlobalUnaryAggregateModifierValuesEnvelope(
          sum_target, AggregateModifierProfile::kDistinct);
  auto count_function_drift =
      GlobalUnaryAggregateModifierValuesEnvelope(
          count_target, AggregateModifierProfile::kDistinctFilter);
  auto boolean_value_type_drift =
      GlobalUnaryAggregateModifierValuesEnvelope(
          bool_and_target, AggregateModifierProfile::kFilter);
  auto statistical_result_nullability_drift =
      GlobalUnaryAggregateModifierValuesEnvelope(
          stddev_pop_target, AggregateModifierProfile::kDistinctFilter);
  for (auto& operand : non_boolean_filter.operands) {
    if (operand.type != "relational_expression_v1") continue;
    if (operand.name == "2" || operand.name == "4" ||
        operand.name == "6" || operand.name == "10") {
      operand.value = "1|-|2|-|-|1|-|31";
    }
    if (operand.name == "12") {
      operand.value = "1|-|2|-|-|1|-|30";
    }
  }
  for (auto& operand : non_identifier_filter.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "15") {
      operand.value =
          "4|13,2|3|019de5fc-2400-72e4-8549-82b2eef5a777|-|-|-|-";
    }
  }
  for (auto& operand : filter_arity_drift.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "15") {
      operand.value =
          "4|13|3|019de5fc-2400-72e4-8549-82b2eef5a777|-|-|-|-";
    }
  }
  for (auto& operand : distinct_arity_drift.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "15") {
      operand.value =
          "4|13,14|3|019de5fc-2400-72e4-8549-82b2eef5a777|-|-|-|-";
    }
  }
  for (auto& operand : count_function_drift.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "15") {
      operand.value =
          "4|13,14|3|019de5fc-2400-78ac-b50c-45b832831004|-|-|-|-";
    }
  }
  for (auto& operand : boolean_value_type_drift.operands) {
    if (operand.type == "relational_expression_v1" &&
        (operand.name == "1" || operand.name == "3" ||
         operand.name == "5" || operand.name == "7" ||
         operand.name == "9" || operand.name == "11")) {
      operand.value = "1|-|1|-|-|1|-|31";
    }
  }
  for (auto& operand : statistical_result_nullability_drift.operands) {
    if (operand.type == "relational_descriptor_v1" &&
        operand.name == "3") {
      const auto separator = operand.value.find("|2|");
      if (separator != std::string::npos) {
        operand.value.replace(separator, 3, "|1|");
      }
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(non_boolean_filter)) &&
          refused_atomically(std::move(non_identifier_filter)) &&
          refused_atomically(std::move(filter_arity_drift)) &&
          refused_atomically(std::move(distinct_arity_drift)) &&
          refused_atomically(std::move(count_function_drift)) &&
          refused_atomically(std::move(boolean_value_type_drift)) &&
          refused_atomically(
              std::move(statistical_result_nullability_drift)),
      "type-, function-identity-, result-, or arity-drifted global unary "
      "aggregate "
      "modifiers published evidence");
}

bool ValidateGlobalAvgExpressionValuesSpine() {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GlobalAvgExpressionValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GlobalAvgExpressionValuesEnvelope(), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached && first.canonical_result_published &&
          first.api_result.ok && first.diagnostics.empty() &&
          first.logical_node_count == 2 && first.logical_property_count == 0 &&
          first.physical_node_count == 2 &&
          first.canonical_result_column_count == 1 &&
          first.canonical_result_row_count == 1,
      "VALUES global AVG(expression) did not traverse its selected physical "
      "DAG");
  const auto& columns = first.api_result.result_shape.columns;
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      columns.size() == 1 && rows.size() == 1 &&
          columns[0].canonical_type_name == "real64" &&
          columns[0].encoded_descriptor.find("nullability=nullable") !=
              std::string::npos &&
          rows[0].fields.size() == 1 &&
          rows[0].fields[0].first == "avg_value" &&
          rows[0].fields[0].second.state == api::EngineValueState::value &&
          !rows[0].fields[0].second.is_null &&
          rows[0].fields[0].second.encoded_value == "5",
      "global AVG(expression) did not ignore SQL NULL or publish real64 "
      "average state");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical global AVG(expression) input changed canonical plan/result "
      "bytes");
  return passed;
}

bool ValidateGlobalAvgExpressionRefusalIsAtomic() {
  auto function_drift = GlobalAvgExpressionValuesEnvelope();
  auto result_nullability_drift = GlobalAvgExpressionValuesEnvelope();
  auto non_integer_input = GlobalAvgExpressionValuesEnvelope();
  for (auto& operand : function_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "6") {
      operand.value =
          "4|5|2|019de5fc-2400-72e4-8549-82b2eef5a777|-|-|-|-";
    }
  }
  for (auto& operand : result_nullability_drift.operands) {
    if (operand.type == "relational_descriptor_v1" && operand.name == "2") {
      operand.value =
          "019f0000-0000-7300-8000-000000009303|"
          "019f0000-0000-7400-8000-000000009304|1|-|-|-|-|-";
    }
  }
  for (auto& operand : non_integer_input.operands) {
    if (operand.type == "relational_expression_v1" &&
        (operand.name == "1" || operand.name == "3" ||
         operand.name == "4")) {
      if (operand.name == "1") operand.value = "1|-|1|-|-|2|-|32";
      if (operand.name == "3") operand.value = "1|-|1|-|-|2|-|35";
      if (operand.name == "4") operand.value = "1|-|1|-|-|2|-|38";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(function_drift)) &&
          refused_atomically(std::move(result_nullability_drift)) &&
          refused_atomically(std::move(non_integer_input)),
      "function-, result-, or input-type-drifted AVG(expression) published "
      "evidence");
}

bool ValidateGlobalExtremumExpressionValuesSpine(const bool maximum) {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GlobalExtremumExpressionValuesEnvelope(maximum), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GlobalExtremumExpressionValuesEnvelope(maximum), {}});
  const std::string aggregate_name = maximum ? "MAX" : "MIN";
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached && first.canonical_result_published &&
          first.api_result.ok && first.diagnostics.empty() &&
          first.logical_node_count == 2 && first.logical_property_count == 0 &&
          first.physical_node_count == 2 &&
          first.canonical_result_column_count == 1 &&
          first.canonical_result_row_count == 1,
      "VALUES global " + aggregate_name +
          "(expression) did not traverse its selected physical DAG");
  const auto& columns = first.api_result.result_shape.columns;
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      columns.size() == 1 && rows.size() == 1 &&
          columns[0].canonical_type_name == "int64" &&
          columns[0].encoded_descriptor.find("nullability=nullable") !=
              std::string::npos &&
          rows[0].fields.size() == 1 &&
          rows[0].fields[0].first ==
              (maximum ? "max_value" : "min_value") &&
          rows[0].fields[0].second.state == api::EngineValueState::value &&
          !rows[0].fields[0].second.is_null &&
          rows[0].fields[0].second.encoded_value == (maximum ? "9" : "-4"),
      "global " + aggregate_name +
          "(expression) did not ignore SQL NULL or publish its typed "
          "extremum");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical global " + aggregate_name +
          "(expression) input changed canonical plan/result bytes");
  return passed;
}

bool ValidateGlobalExtremumExpressionRefusalIsAtomic(const bool maximum) {
  auto function_drift = GlobalExtremumExpressionValuesEnvelope(maximum);
  auto result_nullability_drift =
      GlobalExtremumExpressionValuesEnvelope(maximum);
  auto non_integer_input = GlobalExtremumExpressionValuesEnvelope(maximum);
  for (auto& operand : function_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "6") {
      operand.value =
          "4|5|2|019de5fc-2400-78ac-b50c-45b832831004|-|-|-|-";
    }
  }
  for (auto& operand : result_nullability_drift.operands) {
    if (operand.type == "relational_descriptor_v1" && operand.name == "2") {
      operand.value =
          maximum
              ? "019f0000-0000-7300-8000-000000009503|"
                "019f0000-0000-7400-8000-000000009504|1|-|-|-|-|-"
              : "019f0000-0000-7300-8000-000000009403|"
                "019f0000-0000-7400-8000-000000009404|1|-|-|-|-|-";
    }
  }
  for (auto& operand : non_integer_input.operands) {
    if (operand.type == "relational_expression_v1" &&
        (operand.name == "1" || operand.name == "3" ||
         operand.name == "4")) {
      if (operand.name == "1") operand.value = "1|-|1|-|-|2|-|2d34";
      if (operand.name == "3") operand.value = "1|-|1|-|-|2|-|39";
      if (operand.name == "4") operand.value = "1|-|1|-|-|2|-|33";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(function_drift)) &&
          refused_atomically(std::move(result_nullability_drift)) &&
          refused_atomically(std::move(non_integer_input)),
      "function-, result-, or input-type-drifted " +
          std::string(maximum ? "MAX" : "MIN") +
          "(expression) published evidence");
}

bool ValidateGlobalBooleanAggregateExpressionValuesSpine(
    const BooleanAggregateKind kind) {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GlobalBooleanAggregateExpressionValuesEnvelope(kind), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GlobalBooleanAggregateExpressionValuesEnvelope(kind), {}});
  const auto aggregate_name = BooleanAggregateName(kind);
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached && first.canonical_result_published &&
          first.api_result.ok && first.diagnostics.empty() &&
          first.logical_node_count == 2 && first.logical_property_count == 0 &&
          first.physical_node_count == 2 &&
          first.canonical_result_column_count == 1 &&
          first.canonical_result_row_count == 1,
      "VALUES global " + std::string(aggregate_name) +
          "(expression) did not traverse its selected physical DAG");
  const auto& columns = first.api_result.result_shape.columns;
  const auto& rows = first.api_result.result_shape.rows;
  const bool bool_or = kind == BooleanAggregateKind::kBoolOr;
  const std::string expected_name =
      kind == BooleanAggregateKind::kBoolAnd
          ? "bool_and_value"
          : (bool_or ? "bool_or_value" : "every_value");
  passed &= Require(
      columns.size() == 1 && rows.size() == 1 &&
          columns[0].canonical_type_name == "boolean" &&
          columns[0].encoded_descriptor.find("nullability=nullable") !=
              std::string::npos &&
          rows[0].fields.size() == 1 &&
          rows[0].fields[0].first == expected_name &&
          rows[0].fields[0].second.state == api::EngineValueState::value &&
          !rows[0].fields[0].second.is_null &&
          rows[0].fields[0].second.encoded_value ==
              (bool_or ? "true" : "false"),
      "global " + std::string(aggregate_name) +
          "(expression) did not ignore SQL NULL or publish its typed boolean "
          "state");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical global " + std::string(aggregate_name) +
          "(expression) input changed canonical plan/result bytes");
  return passed;
}

bool ValidateGlobalBooleanAggregateExpressionRefusalIsAtomic(
    const BooleanAggregateKind kind) {
  auto function_drift = GlobalBooleanAggregateExpressionValuesEnvelope(kind);
  auto result_nullability_drift =
      GlobalBooleanAggregateExpressionValuesEnvelope(kind);
  auto non_boolean_input =
      GlobalBooleanAggregateExpressionValuesEnvelope(kind);
  for (auto& operand : function_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "6") {
      operand.value =
          "4|5|2|019de5fc-2400-78ac-b50c-45b832831004|-|-|-|-";
    }
  }
  const bool bool_and = kind == BooleanAggregateKind::kBoolAnd;
  const bool bool_or = kind == BooleanAggregateKind::kBoolOr;
  for (auto& operand : result_nullability_drift.operands) {
    if (operand.type == "relational_descriptor_v1" && operand.name == "2") {
      operand.value =
          (bool_and ? "019f0000-0000-7300-8000-000000009603|"
                      "019f0000-0000-7400-8000-000000009604|1|-|-|-|-|-"
                    : (bool_or
                           ? "019f0000-0000-7300-8000-000000009703|"
                             "019f0000-0000-7400-8000-000000009704|1|-|-|-|-|-"
                           : "019f0000-0000-7300-8000-000000009803|"
                             "019f0000-0000-7400-8000-000000009804|1|-|-|-|-|-"));
    }
  }
  for (auto& operand : non_boolean_input.operands) {
    if (operand.type == "relational_expression_v1" &&
        (operand.name == "1" || operand.name == "3" ||
         operand.name == "4")) {
      operand.value = operand.name == "3" ? "1|-|1|-|-|1|-|30"
                                           : "1|-|1|-|-|1|-|31";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(function_drift)) &&
          refused_atomically(std::move(result_nullability_drift)) &&
          refused_atomically(std::move(non_boolean_input)),
      "function-, result-, or input-type-drifted " +
          std::string(BooleanAggregateName(kind)) +
          "(expression) published evidence");
}

bool ValidateGlobalStatisticalAggregateExpressionValuesSpine() {
  bool passed = true;
  for (const auto& profile : kStatisticalAggregateProfiles) {
    const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(),
         GlobalStatisticalAggregateExpressionValuesEnvelope(profile), {}});
    const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(),
         GlobalStatisticalAggregateExpressionValuesEnvelope(profile), {}});
    if (!first.api_result.ok) {
      for (const auto& diagnostic : first.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    passed &= Require(
        first.accepted && first.optimizer_admitted &&
            first.optimizer_selected && first.physical_dag_published &&
            first.physical_dag_executed &&
            first.runtime_actuals_attached &&
            first.canonical_result_published && first.api_result.ok &&
            first.diagnostics.empty() && first.logical_node_count == 2 &&
            first.logical_property_count == 0 &&
            first.physical_node_count == 2 &&
            first.canonical_result_column_count == 1 &&
            first.canonical_result_row_count == 1,
        "VALUES global " + std::string(profile.name) +
            "(expression) did not traverse its selected physical DAG");

    const auto& columns = first.api_result.result_shape.columns;
    const auto& rows = first.api_result.result_shape.rows;
    bool typed_result =
        columns.size() == 1 && rows.size() == 1 &&
        columns[0].canonical_type_name == "real64" &&
        columns[0].encoded_descriptor.find("nullability=nullable") !=
            std::string::npos &&
        rows[0].fields.size() == 1 &&
        rows[0].fields[0].first == profile.output_name &&
        rows[0].fields[0].second.state == api::EngineValueState::value &&
        !rows[0].fields[0].second.is_null;
    if (typed_result) {
      const auto& encoded = rows[0].fields[0].second.encoded_value;
      char* end = nullptr;
      const double observed = std::strtod(encoded.c_str(), &end);
      const double delta = observed - profile.expected;
      typed_result = end == encoded.c_str() + encoded.size() &&
                     delta >= -1e-12 && delta <= 1e-12;
    }
    passed &= Require(
        typed_result,
        "global " + std::string(profile.name) +
            "(expression) did not ignore SQL NULL or publish its typed "
            "statistical result");
    passed &= Require(
        repeated.api_result.ok &&
            repeated.selected_plan_uuid == first.selected_plan_uuid &&
            repeated.canonical_result_bytes == first.canonical_result_bytes,
        "identical global " + std::string(profile.name) +
            "(expression) input changed canonical plan/result bytes");
  }
  return passed;
}

bool ValidateGlobalStatisticalAggregateExpressionRefusalIsAtomic() {
  bool passed = true;
  for (const auto& profile : kStatisticalAggregateProfiles) {
    auto function_drift =
        GlobalStatisticalAggregateExpressionValuesEnvelope(profile);
    auto result_nullability_drift =
        GlobalStatisticalAggregateExpressionValuesEnvelope(profile);
    auto non_integer_input =
        GlobalStatisticalAggregateExpressionValuesEnvelope(profile);
    for (auto& operand : function_drift.operands) {
      if (operand.type == "relational_expression_v1" &&
          operand.name == "6") {
        operand.value =
            "4|5|2|019de5fc-2400-78ac-b50c-45b832831004|-|-|-|-";
      }
    }
    for (auto& operand : result_nullability_drift.operands) {
      if (operand.type == "relational_descriptor_v1" &&
          operand.name == "2") {
        operand.value = std::string(profile.result_descriptor_uuid) + "|" +
                        std::string(profile.result_type_uuid) +
                        "|1|-|-|-|-|-";
      }
    }
    for (auto& operand : non_integer_input.operands) {
      if (operand.type == "relational_expression_v1" &&
          (operand.name == "1" || operand.name == "3" ||
           operand.name == "4")) {
        const std::string encoded = operand.name == "1"
                                        ? "31"
                                        : (operand.name == "3" ? "32" : "33");
        operand.value = "1|-|1|-|-|2|-|" + encoded;
      }
    }
    const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
      const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(), std::move(envelope), {}});
      return result.accepted && result.optimizer_admitted &&
             !result.optimizer_selected && !result.physical_dag_published &&
             !result.physical_dag_executed &&
             !result.runtime_actuals_attached &&
             !result.canonical_result_published && !result.api_result.ok &&
             result.physical_node_count == 0 &&
             result.canonical_result_bytes.empty() &&
             HasApiDiagnostic(
                 result, "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
    };
    passed &= Require(
        refused_atomically(std::move(function_drift)) &&
            refused_atomically(std::move(result_nullability_drift)) &&
            refused_atomically(std::move(non_integer_input)),
        "function-, result-, or input-type-drifted " +
            std::string(profile.name) + "(expression) published evidence");
  }
  return passed;
}

long double PairStatisticalModifierExpected(
    const PairStatisticalAggregateProfile& profile,
    const AggregateModifierProfile modifier) {
  using Pair = std::pair<long double, long double>;
  const std::vector<Pair> values =
      modifier == AggregateModifierProfile::kFilter
          ? std::vector<Pair>{{2.0L, 1.0L}, {4.0L, 2.0L},
                              {10.0L, 4.0L}, {10.0L, 4.0L}}
          : modifier == AggregateModifierProfile::kDistinct
                ? std::vector<Pair>{{2.0L, 1.0L}, {4.0L, 2.0L},
                                    {8.0L, 3.0L}, {10.0L, 4.0L}}
                : std::vector<Pair>{{2.0L, 1.0L}, {4.0L, 2.0L},
                                    {10.0L, 4.0L}};
  long double sum_y = 0.0L;
  long double sum_x = 0.0L;
  for (const auto& [y, x] : values) {
    sum_y += y;
    sum_x += x;
  }
  const auto count = static_cast<long double>(values.size());
  const long double mean_y = sum_y / count;
  const long double mean_x = sum_x / count;
  long double m2_y = 0.0L;
  long double m2_x = 0.0L;
  long double comoment = 0.0L;
  for (const auto& [y, x] : values) {
    const long double delta_y = y - mean_y;
    const long double delta_x = x - mean_x;
    m2_y += delta_y * delta_y;
    m2_x += delta_x * delta_x;
    comoment += delta_y * delta_x;
  }
  switch (profile.kind) {
    case PairStatisticalAggregateKind::kCorr:
      return comoment / std::sqrt(m2_x * m2_y);
    case PairStatisticalAggregateKind::kCovarPop:
      return comoment / count;
    case PairStatisticalAggregateKind::kCovarSamp:
      return comoment / (count - 1.0L);
    case PairStatisticalAggregateKind::kRegrCount:
      return count;
    case PairStatisticalAggregateKind::kRegrAvgx:
      return mean_x;
    case PairStatisticalAggregateKind::kRegrAvgy:
      return mean_y;
    case PairStatisticalAggregateKind::kRegrIntercept:
      return mean_y - mean_x * comoment / m2_x;
    case PairStatisticalAggregateKind::kRegrR2:
      return comoment * comoment / (m2_x * m2_y);
    case PairStatisticalAggregateKind::kRegrSlope:
      return comoment / m2_x;
    case PairStatisticalAggregateKind::kRegrSxx:
      return m2_x;
    case PairStatisticalAggregateKind::kRegrSxy:
      return comoment;
    case PairStatisticalAggregateKind::kRegrSyy:
      return m2_y;
  }
  return std::numeric_limits<long double>::quiet_NaN();
}

bool PairStatisticalModifierOutputMatches(
    const PairStatisticalAggregateProfile& profile,
    const AggregateModifierProfile modifier,
    const std::string& actual) {
  const long double expected =
      PairStatisticalModifierExpected(profile, modifier);
  if (profile.count_result) {
    return actual == std::to_string(static_cast<std::size_t>(expected));
  }
  char* parse_end = nullptr;
  const double decoded = std::strtod(actual.c_str(), &parse_end);
  return parse_end == actual.c_str() + actual.size() &&
         std::abs(decoded - static_cast<double>(expected)) <= 1e-9;
}

bool ValidateGlobalPairStatisticalAggregateModifierValuesSpine() {
  bool passed = true;
  for (const auto& profile : kPairStatisticalAggregateProfiles) {
    for (const auto modifier : {AggregateModifierProfile::kFilter,
                                AggregateModifierProfile::kDistinct,
                                AggregateModifierProfile::kDistinctFilter}) {
      const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(), GlobalPairStatisticalAggregateModifierValuesEnvelope(
                          profile, modifier),
           {}});
      const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(), GlobalPairStatisticalAggregateModifierValuesEnvelope(
                          profile, modifier),
           {}});
      if (!first.api_result.ok) {
        for (const auto& diagnostic : first.api_result.diagnostics) {
          std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
        }
      }
      const auto& columns = first.api_result.result_shape.columns;
      const auto& rows = first.api_result.result_shape.rows;
      const bool exact_output =
          columns.size() == 1 && rows.size() == 1 &&
          columns[0].canonical_type_name ==
              (profile.count_result ? "int64" : "real64") &&
          columns[0].encoded_descriptor.find(
              profile.count_result ? "nullability=non_null"
                                   : "nullability=nullable") !=
              std::string::npos &&
          rows[0].fields.size() == 1 &&
          rows[0].fields[0].first == profile.output_name &&
          rows[0].fields[0].second.state == api::EngineValueState::value &&
          !rows[0].fields[0].second.is_null &&
          PairStatisticalModifierOutputMatches(
              profile, modifier, rows[0].fields[0].second.encoded_value);
      passed &= Require(
          first.accepted && first.optimizer_admitted &&
              first.optimizer_selected && first.physical_dag_published &&
              first.physical_dag_executed &&
              first.runtime_actuals_attached &&
              first.canonical_result_published && first.api_result.ok &&
              first.diagnostics.empty() && first.logical_node_count == 2 &&
              first.logical_property_count == 0 &&
              first.physical_node_count == 2 && exact_output,
          "global " + std::string(profile.name) + " " +
              std::string(AggregateModifierName(modifier)) +
              " did not execute through the selected canonical pair "
              "statistical spine");
      passed &= Require(
          repeated.api_result.ok &&
              repeated.selected_plan_uuid == first.selected_plan_uuid &&
              repeated.canonical_result_bytes == first.canonical_result_bytes,
          "identical global " + std::string(profile.name) + " " +
              std::string(AggregateModifierName(modifier)) +
              " input changed canonical plan/result bytes");
    }
  }
  return passed;
}

bool ValidateGlobalPairStatisticalAggregateModifierRefusalIsAtomic() {
  const auto& profile = kPairStatisticalAggregateProfiles[0];
  auto non_boolean_filter =
      GlobalPairStatisticalAggregateModifierValuesEnvelope(
          profile, AggregateModifierProfile::kFilter);
  auto non_identifier_filter =
      GlobalPairStatisticalAggregateModifierValuesEnvelope(
          profile, AggregateModifierProfile::kFilter);
  auto filter_arity_drift =
      GlobalPairStatisticalAggregateModifierValuesEnvelope(
          profile, AggregateModifierProfile::kFilter);
  auto distinct_arity_drift =
      GlobalPairStatisticalAggregateModifierValuesEnvelope(
          profile, AggregateModifierProfile::kDistinct);
  for (auto& operand : non_boolean_filter.operands) {
    if (operand.type == "relational_expression_v1" &&
        (operand.name == "3" || operand.name == "6" ||
         operand.name == "9" || operand.name == "12" ||
         operand.name == "15" || operand.name == "18" ||
         operand.name == "21")) {
      operand.value = "1|-|3|-|-|1|-|31";
    }
  }
  for (auto& operand : non_identifier_filter.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "25") {
      operand.value = "4|22,23,3|4|" +
                      std::string(profile.function_uuid) + "|-|-|-|-";
    }
  }
  for (auto& operand : filter_arity_drift.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "25") {
      operand.value = "4|22,23|4|" + std::string(profile.function_uuid) +
                      "|-|-|-|-";
    }
  }
  for (auto& operand : distinct_arity_drift.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "25") {
      operand.value = "4|22,23,24|4|" +
                      std::string(profile.function_uuid) + "|-|-|-|-";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(non_boolean_filter)) &&
          refused_atomically(std::move(non_identifier_filter)) &&
          refused_atomically(std::move(filter_arity_drift)) &&
          refused_atomically(std::move(distinct_arity_drift)),
      "type-, expression-shape-, or arity-drifted global pair statistical "
      "modifiers published evidence");
}

bool ValidateGlobalPairStatisticalAggregateExpressionValuesSpine() {
  bool passed = true;
  for (const auto& profile : kPairStatisticalAggregateProfiles) {
    const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(),
         GlobalPairStatisticalAggregateExpressionValuesEnvelope(profile), {}});
    const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(),
         GlobalPairStatisticalAggregateExpressionValuesEnvelope(profile), {}});
    if (!first.api_result.ok) {
      for (const auto& diagnostic : first.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    passed &= Require(
        first.accepted && first.optimizer_admitted &&
            first.optimizer_selected && first.physical_dag_published &&
            first.physical_dag_executed &&
            first.runtime_actuals_attached &&
            first.canonical_result_published && first.api_result.ok &&
            first.diagnostics.empty() && first.logical_node_count == 2 &&
            first.logical_property_count == 0 &&
            first.physical_node_count == 2 &&
            first.canonical_result_column_count == 1 &&
            first.canonical_result_row_count == 1,
        "VALUES global " + std::string(profile.name) +
            "(Y, X) did not traverse its selected physical DAG");

    const auto& columns = first.api_result.result_shape.columns;
    const auto& rows = first.api_result.result_shape.rows;
    bool typed_result =
        columns.size() == 1 && rows.size() == 1 &&
        columns[0].canonical_type_name ==
            (profile.count_result ? "int64" : "real64") &&
        columns[0].encoded_descriptor.find(
            profile.count_result ? "nullability=non_null"
                                 : "nullability=nullable") !=
            std::string::npos &&
        rows[0].fields.size() == 1 &&
        rows[0].fields[0].first == profile.output_name &&
        rows[0].fields[0].second.state == api::EngineValueState::value &&
        !rows[0].fields[0].second.is_null;
    if (typed_result && profile.count_result) {
      typed_result = rows[0].fields[0].second.encoded_value == "3";
    } else if (typed_result) {
      const auto& encoded = rows[0].fields[0].second.encoded_value;
      char* end = nullptr;
      const double observed = std::strtod(encoded.c_str(), &end);
      const double delta = observed - profile.expected;
      typed_result = end == encoded.c_str() + encoded.size() &&
                     delta >= -1e-12 && delta <= 1e-12;
    }
    passed &= Require(
        typed_result,
        "global " + std::string(profile.name) +
            "(Y, X) did not preserve argument order, ignore an incomplete "
            "pair, or publish its typed result");
    passed &= Require(
        repeated.api_result.ok &&
            repeated.selected_plan_uuid == first.selected_plan_uuid &&
            repeated.canonical_result_bytes == first.canonical_result_bytes,
        "identical global " + std::string(profile.name) +
            "(Y, X) input changed canonical plan/result bytes");
  }
  return passed;
}

bool ValidateGlobalPairStatisticalAggregateExpressionRefusalIsAtomic() {
  bool passed = true;
  for (const auto& profile : kPairStatisticalAggregateProfiles) {
    auto function_drift =
        GlobalPairStatisticalAggregateExpressionValuesEnvelope(profile);
    auto result_nullability_drift =
        GlobalPairStatisticalAggregateExpressionValuesEnvelope(profile);
    auto non_numeric_y =
        GlobalPairStatisticalAggregateExpressionValuesEnvelope(profile);
    auto non_numeric_x =
        GlobalPairStatisticalAggregateExpressionValuesEnvelope(profile);
    auto arity_drift =
        GlobalPairStatisticalAggregateExpressionValuesEnvelope(profile);
    for (auto& operand : function_drift.operands) {
      if (operand.type == "relational_expression_v1" &&
          operand.name == "11") {
        operand.value =
            "4|9,10|3|019de5fc-2400-78ac-b50c-45b832831004|-|-|-|-";
      }
    }
    for (auto& operand : result_nullability_drift.operands) {
      if (operand.type == "relational_descriptor_v1" &&
          operand.name == "3") {
        operand.value = PairProfileUuid("7300", profile.uuid_family, "05") +
                        "|" +
                        PairProfileUuid("7400", profile.uuid_family, "06") +
                        (profile.count_result ? "|2|-|-|-|-|-"
                                              : "|1|-|-|-|-|-");
      }
    }
    for (auto& operand : non_numeric_y.operands) {
      if (operand.type == "relational_expression_v1" &&
          (operand.name == "1" || operand.name == "3" ||
           operand.name == "5")) {
        const std::string encoded = operand.name == "1"
                                        ? "32"
                                        : (operand.name == "3" ? "34" : "36");
        operand.value = "1|-|1|-|-|2|-|" + encoded;
      }
    }
    for (auto& operand : non_numeric_x.operands) {
      if (operand.type == "relational_expression_v1" &&
          (operand.name == "2" || operand.name == "4" ||
           operand.name == "6" || operand.name == "8")) {
        const std::string encoded =
            operand.name == "2"   ? "31"
            : operand.name == "4" ? "32"
            : operand.name == "6" ? "33"
                                    : "34";
        operand.value = "1|-|2|-|-|2|-|" + encoded;
      }
    }
    for (auto& operand : arity_drift.operands) {
      if (operand.type == "relational_expression_v1" &&
          operand.name == "11") {
        operand.value = "4|9|3|" + std::string(profile.function_uuid) +
                        "|-|-|-|-";
      }
    }
    const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
      const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(), std::move(envelope), {}});
      return result.accepted && result.optimizer_admitted &&
             !result.optimizer_selected && !result.physical_dag_published &&
             !result.physical_dag_executed &&
             !result.runtime_actuals_attached &&
             !result.canonical_result_published && !result.api_result.ok &&
             result.physical_node_count == 0 &&
             result.canonical_result_bytes.empty() &&
             HasApiDiagnostic(
                 result, "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
    };
    passed &= Require(
        refused_atomically(std::move(function_drift)) &&
            refused_atomically(std::move(result_nullability_drift)) &&
            refused_atomically(std::move(non_numeric_y)) &&
            refused_atomically(std::move(non_numeric_x)) &&
            refused_atomically(std::move(arity_drift)),
        "function-, result-, Y/X-type-, or arity-drifted " +
            std::string(profile.name) + "(Y, X) published evidence");
  }
  return passed;
}

bool ValidateGlobalOrderedSetAggregateValuesSpine() {
  bool passed = true;
  for (const auto& profile : kOrderedSetAggregateProfiles) {
    const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), GlobalOrderedSetAggregateValuesEnvelope(profile), {}});
    const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), GlobalOrderedSetAggregateValuesEnvelope(profile), {}});
    if (!first.api_result.ok) {
      for (const auto& diagnostic : first.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    passed &= Require(
        first.accepted && first.optimizer_admitted &&
            first.optimizer_selected && first.physical_dag_published &&
            first.physical_dag_executed &&
            first.runtime_actuals_attached &&
            first.canonical_result_published && first.api_result.ok &&
            first.diagnostics.empty() && first.logical_node_count == 2 &&
            first.logical_property_count == 0 &&
            first.physical_node_count == 2 &&
            first.canonical_result_column_count == 1 &&
            first.canonical_result_row_count == 1,
        "VALUES global " + std::string(profile.name) +
            " ordered-set expression did not traverse its selected physical "
            "DAG");

    const auto& columns = first.api_result.result_shape.columns;
    const auto& rows = first.api_result.result_shape.rows;
    bool typed_result =
        columns.size() == 1 && rows.size() == 1 &&
        columns[0].canonical_type_name ==
            (profile.real_result ? "real64" : "int64") &&
        columns[0].encoded_descriptor.find(
            profile.nullable_result ? "nullability=nullable"
                                    : "nullability=non_null") !=
            std::string::npos &&
        rows[0].fields.size() == 1 &&
        rows[0].fields[0].first == profile.output_name &&
        rows[0].fields[0].second.state == api::EngineValueState::value &&
        !rows[0].fields[0].second.is_null;
    if (typed_result && profile.real_result) {
      const auto& encoded = rows[0].fields[0].second.encoded_value;
      char* end = nullptr;
      const double observed = std::strtod(encoded.c_str(), &end);
      const double delta = observed - profile.expected;
      typed_result = end == encoded.c_str() + encoded.size() &&
                     delta >= -1e-12 && delta <= 1e-12;
    } else if (typed_result) {
      typed_result = rows[0].fields[0].second.encoded_value ==
                     std::to_string(
                         static_cast<std::int64_t>(profile.expected));
    }
    passed &= Require(
        typed_result,
        "global " + std::string(profile.name) +
            " ordered-set expression did not publish its exact typed result");
    passed &= Require(
        repeated.api_result.ok &&
            repeated.selected_plan_uuid == first.selected_plan_uuid &&
            repeated.canonical_result_bytes == first.canonical_result_bytes,
        "identical global " + std::string(profile.name) +
            " ordered-set input changed canonical plan/result bytes");
  }
  return passed;
}

bool ValidateGlobalOrderedSetAggregateRefusalIsAtomic() {
  bool passed = true;
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope,
                                     const std::string_view diagnostic_id) {
    const auto result =
        sblr::DispatchTextualRelationalQueryForContractTest({Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(result, diagnostic_id);
  };
  for (const auto& profile : kOrderedSetAggregateProfiles) {
    auto function_drift = GlobalOrderedSetAggregateValuesEnvelope(profile);
    auto result_nullability_drift =
        GlobalOrderedSetAggregateValuesEnvelope(profile);
    auto non_integer_input = GlobalOrderedSetAggregateValuesEnvelope(profile);
    auto non_identifier_order =
        GlobalOrderedSetAggregateValuesEnvelope(profile);
    auto arity_drift = GlobalOrderedSetAggregateValuesEnvelope(profile);
    for (auto& operand : function_drift.operands) {
      if (operand.type == "relational_expression_v1" &&
          operand.name == "8") {
        operand.value =
            "4|" + std::string(profile.direct_argument ? "7,6" : "6") +
            "|3|019de5fc-2400-78ac-b50c-45b832831004|-|-|-|-";
      }
    }
    for (auto& operand : result_nullability_drift.operands) {
      if (operand.type == "relational_descriptor_v1" &&
          operand.name == "3") {
        operand.value =
            OrderedSetProfileUuid("7300", profile.uuid_family, "05") + "|" +
            OrderedSetProfileUuid("7400", profile.uuid_family, "06") +
            (profile.nullable_result ? "|1|-|-|-|-|-" : "|2|-|-|-|-|-");
      }
    }
    for (auto& operand : non_integer_input.operands) {
      if (operand.type == "relational_expression_v1" &&
          (operand.name == "1" || operand.name == "2" ||
           operand.name == "3" || operand.name == "4" ||
           operand.name == "5")) {
        const std::string encoded =
            operand.name == "1"   ? "3130"
            : operand.name == "2" ? "3230"
            : operand.name == "3" ? "3230"
            : operand.name == "4" ? "3330"
                                    : "3430";
        operand.value = "1|-|1|-|-|2|-|" + encoded;
      }
    }
    for (auto& operand : non_identifier_order.operands) {
      if (operand.type == "relational_expression_v1" &&
          operand.name == "6") {
        operand.value = "1|-|1|-|-|1|-|3230";
      }
    }
    for (auto& operand : arity_drift.operands) {
      if (operand.type == "relational_expression_v1" &&
          operand.name == "8") {
        operand.value =
            "4|" + std::string(profile.direct_argument ? "7" : "6,6") +
            "|3|" + std::string(profile.function_uuid) + "|-|-|-|-";
      }
    }
    bool profile_refused =
        refused_atomically(
            std::move(function_drift),
            "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1") &&
        refused_atomically(
            std::move(result_nullability_drift),
            "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1") &&
        refused_atomically(
            std::move(non_integer_input),
            "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1") &&
        refused_atomically(
            std::move(non_identifier_order),
            "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1") &&
        refused_atomically(
            std::move(arity_drift),
            "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
    if (profile.direct_argument) {
      auto direct_literal_drift =
          GlobalOrderedSetAggregateValuesEnvelope(profile);
      for (auto& operand : direct_literal_drift.operands) {
        if (operand.type == "relational_descriptor_v1" &&
            operand.name == "2") {
          operand.value =
              OrderedSetProfileUuid("7300", profile.uuid_family, "03") +
              "|" +
              OrderedSetProfileUuid("7400", profile.uuid_family, "04") +
              "|2|-|-|-|-|-";
        }
      }
      profile_refused &= refused_atomically(
          std::move(direct_literal_drift),
          "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
    }
    passed &= Require(
        profile_refused,
        "identity-, descriptor-, order-, direct-argument-, input-, or "
        "arity-drifted global " +
            std::string(profile.name) +
            " ordered-set expression published evidence");
  }

  for (const auto& profile : kOrderedSetAggregateProfiles) {
    if (!profile.percentile_fraction) continue;
    auto invalid_fraction = GlobalOrderedSetAggregateValuesEnvelope(profile);
    for (auto& operand : invalid_fraction.operands) {
      if (operand.type == "relational_expression_v1" &&
          operand.name == "7") {
        operand.value = "1|-|2|-|-|1|-|312e35";
      }
    }
    passed &= Require(
        refused_atomically(std::move(invalid_fraction),
                           "QOW-DIAG-QRY-011-REGISTRY-DIRECT-V1"),
        "out-of-range global " + std::string(profile.name) +
            " fraction did not preserve shared-runtime atomic refusal");
  }
  return passed;
}

double OrderedSetAggregateModifierExpected(
    const OrderedSetAggregateProfile& profile,
    const AggregateModifierProfile modifier) {
  if (profile.name == "RANK") {
    return modifier == AggregateModifierProfile::kDistinctFilter ? 3.0 : 4.0;
  }
  if (profile.name == "DENSE_RANK") {
    return modifier == AggregateModifierProfile::kDistinct ? 4.0 : 3.0;
  }
  if (profile.name == "PERCENT_RANK") {
    if (modifier == AggregateModifierProfile::kFilter) return 0.75;
    if (modifier == AggregateModifierProfile::kDistinct) return 0.6;
    return 2.0 / 3.0;
  }
  if (profile.name == "CUME_DIST") {
    if (modifier == AggregateModifierProfile::kFilter) return 0.8;
    if (modifier == AggregateModifierProfile::kDistinct) return 2.0 / 3.0;
    return 0.75;
  }
  if (profile.name == "MODE") {
    return modifier == AggregateModifierProfile::kFilter ? 20.0 : 10.0;
  }
  if (profile.name == "PERCENTILE_CONT") {
    return modifier == AggregateModifierProfile::kFilter ? 17.5 : 15.0;
  }
  if (profile.name == "PERCENTILE_DISC") {
    return modifier == AggregateModifierProfile::kDistinct ? 15.0 : 10.0;
  }
  return 0.0;
}

bool ValidateGlobalOrderedSetAggregateModifierValuesSpine() {
  bool passed = true;
  for (const auto& profile : kOrderedSetAggregateProfiles) {
    for (const auto modifier : {AggregateModifierProfile::kFilter,
                                AggregateModifierProfile::kDistinct,
                                AggregateModifierProfile::kDistinctFilter}) {
      const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(),
           GlobalOrderedSetAggregateModifierValuesEnvelope(profile, modifier),
           {}});
      const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(),
           GlobalOrderedSetAggregateModifierValuesEnvelope(profile, modifier),
           {}});
      if (!first.api_result.ok) {
        for (const auto& diagnostic : first.api_result.diagnostics) {
          std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
        }
      }
      const auto& columns = first.api_result.result_shape.columns;
      const auto& rows = first.api_result.result_shape.rows;
      bool exact_output =
          columns.size() == 1 && rows.size() == 1 &&
          columns[0].canonical_type_name ==
              (profile.real_result ? "real64" : "int64") &&
          columns[0].encoded_descriptor.find(
              profile.nullable_result ? "nullability=nullable"
                                      : "nullability=non_null") !=
              std::string::npos &&
          rows[0].fields.size() == 1 &&
          rows[0].fields[0].first == profile.output_name &&
          rows[0].fields[0].second.state == api::EngineValueState::value &&
          !rows[0].fields[0].second.is_null;
      if (exact_output && profile.real_result) {
        const auto& encoded = rows[0].fields[0].second.encoded_value;
        char* end = nullptr;
        const double observed = std::strtod(encoded.c_str(), &end);
        const double delta =
            observed - OrderedSetAggregateModifierExpected(profile, modifier);
        exact_output = end == encoded.c_str() + encoded.size() &&
                       delta >= -1e-12 && delta <= 1e-12;
      } else if (exact_output) {
        exact_output =
            rows[0].fields[0].second.encoded_value ==
            std::to_string(static_cast<std::int64_t>(
                OrderedSetAggregateModifierExpected(profile, modifier)));
      }
      passed &= Require(
          first.accepted && first.optimizer_admitted &&
              first.optimizer_selected && first.physical_dag_published &&
              first.physical_dag_executed &&
              first.runtime_actuals_attached &&
              first.canonical_result_published && first.api_result.ok &&
              first.diagnostics.empty() && first.logical_node_count == 2 &&
              first.logical_property_count == 0 &&
              first.physical_node_count == 2 && exact_output,
          "global " + std::string(profile.name) + " " +
              std::string(AggregateModifierName(modifier)) +
              " did not execute through the selected canonical spine");
      passed &= Require(
          repeated.api_result.ok &&
              repeated.selected_plan_uuid == first.selected_plan_uuid &&
              repeated.canonical_result_bytes == first.canonical_result_bytes,
          "identical global " + std::string(profile.name) + " " +
              std::string(AggregateModifierName(modifier)) +
              " input changed canonical plan/result bytes");
    }
  }
  return passed;
}

bool ValidateGlobalOrderedSetAggregateModifierRefusalIsAtomic() {
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
  };
  const auto set_function_children =
      [](sblr::SblrOperationEnvelope& envelope,
         const OrderedSetAggregateProfile& profile,
         const std::string& children) {
        for (auto& operand : envelope.operands) {
          if (operand.type == "relational_expression_v1" &&
              operand.name == "18") {
            operand.value = "4|" + children + "|4|" +
                            std::string(profile.function_uuid) + "|-|-|-|-";
          }
        }
      };

  bool passed = true;
  for (const auto& profile : kOrderedSetAggregateProfiles) {
    auto non_boolean_filter =
        GlobalOrderedSetAggregateModifierValuesEnvelope(
            profile, AggregateModifierProfile::kFilter);
    auto non_identifier_filter =
        GlobalOrderedSetAggregateModifierValuesEnvelope(
            profile, AggregateModifierProfile::kFilter);
    auto missing_filter = GlobalOrderedSetAggregateModifierValuesEnvelope(
        profile, AggregateModifierProfile::kFilter);
    auto distinct_extra_filter =
        GlobalOrderedSetAggregateModifierValuesEnvelope(
            profile, AggregateModifierProfile::kDistinct);
    auto direct_order_filter_inversion =
        GlobalOrderedSetAggregateModifierValuesEnvelope(
            profile, AggregateModifierProfile::kDistinctFilter);
    for (auto& operand : non_boolean_filter.operands) {
      if (operand.type == "relational_expression_v1" &&
          (operand.name == "2" || operand.name == "4" ||
           operand.name == "6" || operand.name == "8" ||
           operand.name == "10" || operand.name == "12" ||
           operand.name == "14")) {
        operand.value = "1|-|3|-|-|1|-|31";
      }
    }
    if (profile.direct_argument) {
      set_function_children(non_identifier_filter, profile, "16,15,2");
      set_function_children(missing_filter, profile, "16,15");
      set_function_children(distinct_extra_filter, profile, "16,15,17");
      set_function_children(direct_order_filter_inversion, profile,
                            "16,17,15");
    } else {
      set_function_children(non_identifier_filter, profile, "15,2");
      set_function_children(missing_filter, profile, "15");
      set_function_children(distinct_extra_filter, profile, "15,17");
      set_function_children(direct_order_filter_inversion, profile, "17,15");
    }
    passed &= refused_atomically(std::move(non_boolean_filter)) &&
              refused_atomically(std::move(non_identifier_filter)) &&
              refused_atomically(std::move(missing_filter)) &&
              refused_atomically(std::move(distinct_extra_filter)) &&
              refused_atomically(std::move(direct_order_filter_inversion));
  }
  return Require(
      passed,
      "type-, expression-shape-, arity-, or direct/order/FILTER-drifted "
      "ordered-set modifiers published evidence");
}

bool ValidateGlobalApproximateAggregateValuesSpine() {
  bool passed = true;
  for (const auto& profile : kApproximateAggregateProfiles) {
    const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), GlobalApproximateAggregateValuesEnvelope(profile), {}});
    const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), GlobalApproximateAggregateValuesEnvelope(profile), {}});
    if (!first.api_result.ok) {
      for (const auto& diagnostic : first.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    passed &= Require(
        first.accepted && first.optimizer_admitted &&
            first.optimizer_selected && first.physical_dag_published &&
            first.physical_dag_executed &&
            first.runtime_actuals_attached &&
            first.canonical_result_published && first.api_result.ok &&
            first.diagnostics.empty() && first.logical_node_count == 2 &&
            first.logical_property_count == 0 &&
            first.physical_node_count == 2 &&
            first.canonical_result_column_count == 1 &&
            first.canonical_result_row_count == 1,
        "VALUES global " + std::string(profile.name) +
            " expression did not traverse its selected physical DAG");

    const auto& columns = first.api_result.result_shape.columns;
    const auto& rows = first.api_result.result_shape.rows;
    passed &= Require(
        columns.size() == 1 && rows.size() == 1 &&
            columns[0].canonical_type_name == profile.result_type &&
            columns[0].encoded_descriptor.find(
                profile.nullable_result ? "nullability=nullable"
                                        : "nullability=non_null") !=
                std::string::npos &&
            rows[0].fields.size() == 1 &&
            rows[0].fields[0].first == profile.output_name &&
            rows[0].fields[0].second.state ==
                api::EngineValueState::value &&
            !rows[0].fields[0].second.is_null &&
            rows[0].fields[0].second.encoded_value ==
                profile.expected_encoded,
        "global " + std::string(profile.name) +
            " expression did not publish its exact typed result");
    passed &= Require(
        repeated.api_result.ok &&
            repeated.selected_plan_uuid == first.selected_plan_uuid &&
            repeated.canonical_result_bytes == first.canonical_result_bytes,
        "identical global " + std::string(profile.name) +
            " input changed canonical plan/result bytes");
  }
  return passed;
}

bool ValidateGlobalApproximateAggregateRefusalIsAtomic() {
  bool passed = true;
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope,
                                     const std::string_view diagnostic_id) {
    const auto result =
        sblr::DispatchTextualRelationalQueryForContractTest({Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(result, diagnostic_id);
  };
  for (const auto& profile : kApproximateAggregateProfiles) {
    auto function_drift = GlobalApproximateAggregateValuesEnvelope(profile);
    auto result_nullability_drift =
        GlobalApproximateAggregateValuesEnvelope(profile);
    auto input_type_drift =
        GlobalApproximateAggregateValuesEnvelope(profile);
    auto non_identifier_input =
        GlobalApproximateAggregateValuesEnvelope(profile);
    auto arity_drift = GlobalApproximateAggregateValuesEnvelope(profile);
    for (auto& operand : function_drift.operands) {
      if (operand.type == "relational_expression_v1" &&
          operand.name == "9") {
        operand.value =
            "4|" +
            std::string(profile.direct_literal_hex.empty() ? "7" : "8,7") +
            "|3|019de5fc-2400-78ac-b50c-45b832831004|-|-|-|-";
      }
    }
    for (auto& operand : result_nullability_drift.operands) {
      if (operand.type == "relational_descriptor_v1" &&
          operand.name == "3") {
        operand.value =
            ApproximateProfileUuid("7300", profile.uuid_family, "05") + "|" +
            ApproximateProfileUuid("7400", profile.uuid_family, "06") +
            (profile.nullable_result ? "|1|-|-|-|-|-" : "|2|-|-|-|-|-");
      }
    }
    for (auto& operand : input_type_drift.operands) {
      if (operand.type == "relational_expression_v1" &&
          operand.name >= "1" && operand.name <= "6") {
        operand.value = profile.text_input
                            ? "1|-|1|-|-|1|-|31"
                            : "1|-|1|-|-|2|-|61";
      }
    }
    for (auto& operand : non_identifier_input.operands) {
      if (operand.type == "relational_expression_v1" &&
          operand.name == "7") {
        operand.value = profile.text_input ? "1|-|1|-|-|2|-|61"
                                           : "1|-|1|-|-|1|-|31";
      }
    }
    for (auto& operand : arity_drift.operands) {
      if (operand.type == "relational_expression_v1" &&
          operand.name == "9") {
        operand.value =
            "4|" +
            std::string(profile.direct_literal_hex.empty() ? "7,7" : "8") +
            "|3|" + std::string(profile.function_uuid) + "|-|-|-|-";
      }
    }
    bool profile_refused =
        refused_atomically(
            std::move(function_drift),
            "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1") &&
        refused_atomically(
            std::move(result_nullability_drift),
            "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1") &&
        refused_atomically(
            std::move(input_type_drift),
            "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1") &&
        refused_atomically(
            std::move(non_identifier_input),
            "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1") &&
        refused_atomically(
            std::move(arity_drift),
            "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
    if (!profile.direct_literal_hex.empty()) {
      auto direct_nullability_drift =
          GlobalApproximateAggregateValuesEnvelope(profile);
      for (auto& operand : direct_nullability_drift.operands) {
        if (operand.type == "relational_descriptor_v1" &&
            operand.name == "2") {
          operand.value =
              ApproximateProfileUuid("7300", profile.uuid_family, "03") +
              "|" +
              ApproximateProfileUuid("7400", profile.uuid_family, "04") +
              "|2|-|-|-|-|-";
        }
      }
      profile_refused &= refused_atomically(
          std::move(direct_nullability_drift),
          "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
    }
    passed &= Require(
        profile_refused,
        "identity-, descriptor-, input-type-, direct-argument-, or "
        "arity-drifted global " +
            std::string(profile.name) +
            " expression published physical/result evidence");

    if (!profile.invalid_direct_literal_hex.empty()) {
      auto invalid_direct =
          GlobalApproximateAggregateValuesEnvelope(profile);
      for (auto& operand : invalid_direct.operands) {
        if (operand.type == "relational_expression_v1" &&
            operand.name == "8") {
          operand.value = "1|-|2|-|-|1|-|" +
                          std::string(profile.invalid_direct_literal_hex);
        }
      }
      passed &= Require(
          refused_atomically(std::move(invalid_direct),
                             "QOW-DIAG-QRY-011-REGISTRY-DIRECT-V1"),
          "invalid global " + std::string(profile.name) +
              " direct argument did not preserve shared-runtime atomic "
              "refusal");
    }
  }
  return passed;
}

std::string GlobalApproximateAggregateModifierExpected(
    const ApproximateAggregateProfile& profile,
    const AggregateModifierProfile modifier) {
  if (profile.name == "APPROX_COUNT_DISTINCT") {
    return modifier == AggregateModifierProfile::kDistinct ? "3" : "2";
  }
  if (profile.name == "APPROX_MEDIAN") {
    if (modifier == AggregateModifierProfile::kFilter) return "15";
    if (modifier == AggregateModifierProfile::kDistinct) return "25";
    return "20";
  }
  if (profile.name == "APPROX_PERCENTILE_CONT") {
    if (modifier == AggregateModifierProfile::kFilter) return "25";
    if (modifier == AggregateModifierProfile::kDistinct) return "32.5";
    return "30";
  }
  if (profile.name == "APPROX_PERCENTILE_DISC") {
    if (modifier == AggregateModifierProfile::kFilter) return "20";
    if (modifier == AggregateModifierProfile::kDistinct) return "30";
    return "40";
  }
  if (modifier == AggregateModifierProfile::kFilter) {
    return "[{\"value\":\"b\",\"count\":3},"
           "{\"value\":\"a\",\"count\":1}]";
  }
  return "[{\"value\":\"a\",\"count\":1},"
         "{\"value\":\"b\",\"count\":1}]";
}

bool ValidateGlobalApproximateAggregateModifierValuesSpine() {
  bool passed = true;
  for (const auto& profile : kApproximateAggregateProfiles) {
    for (const auto modifier : {AggregateModifierProfile::kFilter,
                                AggregateModifierProfile::kDistinct,
                                AggregateModifierProfile::kDistinctFilter}) {
      const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(),
           GlobalApproximateAggregateModifierValuesEnvelope(profile, modifier),
           {}});
      const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(),
           GlobalApproximateAggregateModifierValuesEnvelope(profile, modifier),
           {}});
      if (!first.api_result.ok) {
        for (const auto& diagnostic : first.api_result.diagnostics) {
          std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
        }
      }
      const auto& columns = first.api_result.result_shape.columns;
      const auto& rows = first.api_result.result_shape.rows;
      const bool exact_output =
          columns.size() == 1 && rows.size() == 1 &&
          columns[0].canonical_type_name == profile.result_type &&
          columns[0].encoded_descriptor.find(
              profile.nullable_result ? "nullability=nullable"
                                      : "nullability=non_null") !=
              std::string::npos &&
          rows[0].fields.size() == 1 &&
          rows[0].fields[0].first == profile.output_name &&
          rows[0].fields[0].second.state == api::EngineValueState::value &&
          !rows[0].fields[0].second.is_null &&
          rows[0].fields[0].second.encoded_value ==
              GlobalApproximateAggregateModifierExpected(profile, modifier);
      passed &= Require(
          first.accepted && first.optimizer_admitted &&
              first.optimizer_selected && first.physical_dag_published &&
              first.physical_dag_executed &&
              first.runtime_actuals_attached &&
              first.canonical_result_published && first.api_result.ok &&
              first.diagnostics.empty() && first.logical_node_count == 2 &&
              first.logical_property_count == 0 &&
              first.physical_node_count == 2 && exact_output,
          "global " + std::string(profile.name) + " " +
              std::string(AggregateModifierName(modifier)) +
              " did not execute through the selected canonical spine");
      passed &= Require(
          repeated.api_result.ok &&
              repeated.selected_plan_uuid == first.selected_plan_uuid &&
              repeated.canonical_result_bytes == first.canonical_result_bytes,
          "identical global " + std::string(profile.name) + " " +
              std::string(AggregateModifierName(modifier)) +
              " input changed canonical plan/result bytes");
    }
  }
  return passed;
}

bool ValidateGlobalApproximateAggregateModifierRefusalIsAtomic() {
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
  };
  const auto set_function_children =
      [](sblr::SblrOperationEnvelope& envelope,
         const ApproximateAggregateProfile& profile,
         const std::string& children) {
        for (auto& operand : envelope.operands) {
          if (operand.type == "relational_expression_v1" &&
              operand.name == "18") {
            operand.value = "4|" + children + "|4|" +
                            std::string(profile.function_uuid) + "|-|-|-|-";
          }
        }
      };

  bool passed = true;
  for (const auto& profile : kApproximateAggregateProfiles) {
    auto non_boolean_filter =
        GlobalApproximateAggregateModifierValuesEnvelope(
            profile, AggregateModifierProfile::kFilter);
    auto non_identifier_filter =
        GlobalApproximateAggregateModifierValuesEnvelope(
            profile, AggregateModifierProfile::kFilter);
    auto missing_filter = GlobalApproximateAggregateModifierValuesEnvelope(
        profile, AggregateModifierProfile::kFilter);
    auto distinct_extra_filter =
        GlobalApproximateAggregateModifierValuesEnvelope(
            profile, AggregateModifierProfile::kDistinct);
    auto direct_value_filter_inversion =
        GlobalApproximateAggregateModifierValuesEnvelope(
            profile, AggregateModifierProfile::kDistinctFilter);
    for (auto& operand : non_boolean_filter.operands) {
      if (operand.type == "relational_expression_v1" &&
          (operand.name == "2" || operand.name == "4" ||
           operand.name == "6" || operand.name == "8" ||
           operand.name == "10" || operand.name == "12" ||
           operand.name == "14")) {
        operand.value = "1|-|3|-|-|1|-|31";
      }
    }
    if (!profile.direct_literal_hex.empty()) {
      set_function_children(non_identifier_filter, profile, "16,15,2");
      set_function_children(missing_filter, profile, "16,15");
      set_function_children(distinct_extra_filter, profile, "16,15,17");
      set_function_children(direct_value_filter_inversion, profile,
                            "16,17,15");
    } else {
      set_function_children(non_identifier_filter, profile, "15,2");
      set_function_children(missing_filter, profile, "15");
      set_function_children(distinct_extra_filter, profile, "15,17");
      set_function_children(direct_value_filter_inversion, profile, "17,15");
    }
    passed &= refused_atomically(std::move(non_boolean_filter)) &&
              refused_atomically(std::move(non_identifier_filter)) &&
              refused_atomically(std::move(missing_filter)) &&
              refused_atomically(std::move(distinct_extra_filter)) &&
              refused_atomically(std::move(direct_value_filter_inversion));
  }
  return Require(
      passed,
      "type-, expression-shape-, arity-, or direct/value/FILTER-drifted "
      "approximate aggregate modifiers published evidence");
}

bool ValidateGlobalStringAggExpressionValuesSpine() {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GlobalStringAggExpressionValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), GlobalStringAggExpressionValuesEnvelope(), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = Require(
      first.accepted && first.optimizer_admitted &&
          first.optimizer_selected && first.physical_dag_published &&
          first.physical_dag_executed && first.runtime_actuals_attached &&
          first.canonical_result_published && first.api_result.ok &&
          first.diagnostics.empty() && first.logical_node_count == 2 &&
          first.logical_property_count == 0 &&
          first.physical_node_count == 2 &&
          first.canonical_result_column_count == 1 &&
          first.canonical_result_row_count == 1,
      "VALUES global STRING_AGG(value, '|') did not traverse its selected "
      "physical DAG");

  const auto& columns = first.api_result.result_shape.columns;
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      columns.size() == 1 && rows.size() == 1 &&
          columns[0].canonical_type_name == "text" &&
          columns[0].encoded_descriptor.find("nullability=nullable") !=
              std::string::npos &&
          rows[0].fields.size() == 1 &&
          rows[0].fields[0].first == "string_agg_value" &&
          rows[0].fields[0].second.state == api::EngineValueState::value &&
          !rows[0].fields[0].second.is_null &&
          rows[0].fields[0].second.encoded_value == "b|d|a",
      "global STRING_AGG did not ignore SQL NULL or preserve its literal "
      "separator and input order");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical global STRING_AGG input changed canonical plan/result bytes");
  return passed;
}

bool ValidateGlobalStringAggExpressionRefusalIsAtomic() {
  auto function_drift = GlobalStringAggExpressionValuesEnvelope();
  auto result_nullability_drift = GlobalStringAggExpressionValuesEnvelope();
  auto non_text_input = GlobalStringAggExpressionValuesEnvelope();
  auto numeric_separator = GlobalStringAggExpressionValuesEnvelope();
  auto null_separator = GlobalStringAggExpressionValuesEnvelope();
  auto arity_drift = GlobalStringAggExpressionValuesEnvelope();
  for (auto& operand : function_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "7") {
      operand.value =
          "4|5,6|3|019de5fc-2400-78ac-b50c-45b832831004|-|-|-|-";
    }
  }
  for (auto& operand : result_nullability_drift.operands) {
    if (operand.type == "relational_descriptor_v1" && operand.name == "3") {
      operand.value =
          "019f0000-0000-7c01-8000-000000000105|"
          "019f0000-0000-7c02-8000-000000000106|1|-|-|-|-|-";
    }
  }
  for (auto& operand : non_text_input.operands) {
    if (operand.type == "relational_expression_v1" &&
        (operand.name == "1" || operand.name == "2" ||
         operand.name == "4")) {
      operand.value =
          operand.name == "1"   ? "1|-|1|-|-|1|-|31"
          : operand.name == "2" ? "1|-|1|-|-|1|-|32"
                                  : "1|-|1|-|-|1|-|33";
    }
  }
  for (auto& operand : numeric_separator.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "6") {
      operand.value = "1|-|2|-|-|1|-|31";
    }
  }
  for (auto& operand : null_separator.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "6") {
      operand.value = "1|-|2|-|-|7|-|2d";
    }
  }
  for (auto& operand : arity_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "7") {
      operand.value =
          "4|5|3|019de5fc-2400-7243-abc6-4f6a777dff00|-|-|-|-";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(function_drift)) &&
          refused_atomically(std::move(result_nullability_drift)) &&
          refused_atomically(std::move(non_text_input)) &&
          refused_atomically(std::move(numeric_separator)) &&
          refused_atomically(std::move(null_separator)) &&
          refused_atomically(std::move(arity_drift)),
      "function-, result-, input-, separator-, or arity-drifted "
      "STRING_AGG published evidence");
}

bool ValidateOrderedStringAggExpressionValuesSpine() {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), OrderedStringAggExpressionValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), OrderedStringAggExpressionValuesEnvelope(), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = Require(
      first.accepted && first.optimizer_admitted &&
          first.optimizer_selected && first.physical_dag_published &&
          first.physical_dag_executed && first.runtime_actuals_attached &&
          first.canonical_result_published && first.api_result.ok &&
          first.diagnostics.empty() && first.logical_node_count == 2 &&
          first.logical_property_count == 0 &&
          first.physical_node_count == 2 &&
          first.canonical_result_column_count == 1 &&
          first.canonical_result_row_count == 1,
      "VALUES global ordered STRING_AGG(value, '|', order) did not traverse "
      "its selected physical DAG");

  const auto& columns = first.api_result.result_shape.columns;
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      columns.size() == 1 && rows.size() == 1 &&
          columns[0].canonical_type_name == "text" &&
          columns[0].encoded_descriptor.find("nullability=nullable") !=
              std::string::npos &&
          rows[0].fields.size() == 1 &&
          rows[0].fields[0].first == "string_agg_ordered_value" &&
          rows[0].fields[0].second.state == api::EngineValueState::value &&
          !rows[0].fields[0].second.is_null &&
          rows[0].fields[0].second.encoded_value == "a|b|d",
      "ordered STRING_AGG did not apply its bound order, ignore SQL NULL, or "
      "preserve its literal separator");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical ordered STRING_AGG input changed canonical plan/result "
      "bytes");
  return passed;
}

bool ValidateOrderedStringAggExpressionRefusalIsAtomic() {
  auto function_drift = OrderedStringAggExpressionValuesEnvelope();
  auto result_nullability_drift = OrderedStringAggExpressionValuesEnvelope();
  auto non_text_input = OrderedStringAggExpressionValuesEnvelope();
  auto numeric_separator = OrderedStringAggExpressionValuesEnvelope();
  auto null_separator = OrderedStringAggExpressionValuesEnvelope();
  auto non_integer_order = OrderedStringAggExpressionValuesEnvelope();
  auto arity_drift = OrderedStringAggExpressionValuesEnvelope();
  for (auto& operand : function_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "12") {
      operand.value =
          "4|9,10,11|4|019de5fc-2400-78ac-b50c-45b832831004|-|-|-|-";
    }
  }
  for (auto& operand : result_nullability_drift.operands) {
    if (operand.type == "relational_descriptor_v1" && operand.name == "4") {
      operand.value = PairProfileUuid("7f10", "b3", "07") + "|" +
                      PairProfileUuid("7f20", "b3", "08") +
                      "|1|-|-|-|-|-";
    }
  }
  for (auto& operand : non_text_input.operands) {
    if (operand.type == "relational_expression_v1" &&
        (operand.name == "1" || operand.name == "3" ||
         operand.name == "7")) {
      operand.value =
          operand.name == "1"   ? "1|-|1|-|-|1|-|32"
          : operand.name == "3" ? "1|-|1|-|-|1|-|34"
                                  : "1|-|1|-|-|1|-|31";
    }
  }
  for (auto& operand : numeric_separator.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "10") {
      operand.value = "1|-|2|-|-|1|-|31";
    }
  }
  for (auto& operand : null_separator.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "10") {
      operand.value = "1|-|2|-|-|7|-|2d";
    }
  }
  for (auto& operand : non_integer_order.operands) {
    if (operand.type == "relational_expression_v1" &&
        (operand.name == "2" || operand.name == "4" ||
         operand.name == "6" || operand.name == "8")) {
      operand.value =
          operand.name == "2"   ? "1|-|3|-|-|2|-|62"
          : operand.name == "4" ? "1|-|3|-|-|2|-|64"
          : operand.name == "6" ? "1|-|3|-|-|2|-|63"
                                  : "1|-|3|-|-|2|-|61";
    }
  }
  for (auto& operand : arity_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "12") {
      operand.value =
          "4|9,10|4|019de5fc-2400-7243-abc6-4f6a777dff00|-|-|-|-";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(function_drift)) &&
          refused_atomically(std::move(result_nullability_drift)) &&
          refused_atomically(std::move(non_text_input)) &&
          refused_atomically(std::move(numeric_separator)) &&
          refused_atomically(std::move(null_separator)) &&
          refused_atomically(std::move(non_integer_order)) &&
          refused_atomically(std::move(arity_drift)),
      "function-, result-, input-, separator-, order-, or arity-drifted "
      "ordered STRING_AGG published evidence");
}

std::string_view StringAggModifierExpected(
    const bool ordered, const AggregateModifierProfile modifier) {
  if (!ordered) {
    switch (modifier) {
      case AggregateModifierProfile::kFilter:
        return "beta|alpha|delta|delta";
      case AggregateModifierProfile::kDistinct:
        return "beta|alpha|gamma|delta";
      case AggregateModifierProfile::kDistinctFilter:
        return "beta|alpha|delta";
    }
  }
  switch (modifier) {
    case AggregateModifierProfile::kFilter:
      return "delta|beta|alpha|delta";
    case AggregateModifierProfile::kDistinct:
      return "gamma|alpha|beta|delta";
    case AggregateModifierProfile::kDistinctFilter:
      return "beta|alpha|delta";
  }
  return {};
}

bool ValidateStringAggModifierValuesSpine() {
  bool passed = true;
  for (const bool ordered : {false, true}) {
    for (const auto modifier : {AggregateModifierProfile::kFilter,
                                AggregateModifierProfile::kDistinct,
                                AggregateModifierProfile::kDistinctFilter}) {
      const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(), StringAggModifierValuesEnvelope(ordered, modifier), {}});
      const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(), StringAggModifierValuesEnvelope(ordered, modifier), {}});
      if (!first.api_result.ok) {
        for (const auto& diagnostic : first.api_result.diagnostics) {
          std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
        }
      }
      const auto& columns = first.api_result.result_shape.columns;
      const auto& rows = first.api_result.result_shape.rows;
      const bool exact_output =
          columns.size() == 1 && rows.size() == 1 &&
          columns[0].canonical_type_name == "text" &&
          columns[0].encoded_descriptor.find("nullability=nullable") !=
              std::string::npos &&
          rows[0].fields.size() == 1 &&
          rows[0].fields[0].first ==
              (ordered ? "string_agg_ordered_modifier_value"
                       : "string_agg_modifier_value") &&
          rows[0].fields[0].second.state == api::EngineValueState::value &&
          !rows[0].fields[0].second.is_null &&
          rows[0].fields[0].second.encoded_value ==
              StringAggModifierExpected(ordered, modifier);
      passed &= Require(
          first.accepted && first.optimizer_admitted &&
              first.optimizer_selected && first.physical_dag_published &&
              first.physical_dag_executed &&
              first.runtime_actuals_attached &&
              first.canonical_result_published && first.api_result.ok &&
              first.diagnostics.empty() && first.logical_node_count == 2 &&
              first.logical_property_count == 0 &&
              first.physical_node_count == 2 && exact_output,
          std::string(ordered ? "ordered" : "unordered") + " STRING_AGG " +
              std::string(AggregateModifierName(modifier)) +
              " did not execute through the selected canonical spine");
      passed &= Require(
          repeated.api_result.ok &&
              repeated.selected_plan_uuid == first.selected_plan_uuid &&
              repeated.canonical_result_bytes == first.canonical_result_bytes,
          "identical " + std::string(ordered ? "ordered" : "unordered") +
              " STRING_AGG " +
              std::string(AggregateModifierName(modifier)) +
              " input changed canonical plan/result bytes");
    }
  }
  return passed;
}

bool ValidateStringAggModifierRefusalIsAtomic() {
  auto non_boolean_filter = StringAggModifierValuesEnvelope(
      false, AggregateModifierProfile::kFilter);
  auto non_identifier_filter = StringAggModifierValuesEnvelope(
      false, AggregateModifierProfile::kFilter);
  auto filter_arity_drift = StringAggModifierValuesEnvelope(
      false, AggregateModifierProfile::kFilter);
  auto distinct_arity_drift = StringAggModifierValuesEnvelope(
      false, AggregateModifierProfile::kDistinct);
  auto ordered_argument_inversion = StringAggModifierValuesEnvelope(
      true, AggregateModifierProfile::kDistinctFilter);
  for (auto& operand : non_boolean_filter.operands) {
    if (operand.type == "relational_expression_v1" &&
        (operand.name == "3" || operand.name == "6" ||
         operand.name == "9" || operand.name == "12" ||
         operand.name == "15" || operand.name == "18" ||
         operand.name == "21")) {
      operand.value = "1|-|3|-|-|1|-|31";
    }
  }
  for (auto& operand : non_identifier_filter.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "26") {
      operand.value =
          "4|22,25,3|5|019de5fc-2400-7243-abc6-4f6a777dff00|-|-|-|-";
    }
  }
  for (auto& operand : filter_arity_drift.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "26") {
      operand.value =
          "4|22,25|5|019de5fc-2400-7243-abc6-4f6a777dff00|-|-|-|-";
    }
  }
  for (auto& operand : distinct_arity_drift.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "26") {
      operand.value =
          "4|22,25,24|5|019de5fc-2400-7243-abc6-4f6a777dff00|-|-|-|-";
    }
  }
  for (auto& operand : ordered_argument_inversion.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "26") {
      operand.value =
          "4|22,25,24,23|5|019de5fc-2400-7243-abc6-4f6a777dff00|-|-|-|-";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(non_boolean_filter)) &&
          refused_atomically(std::move(non_identifier_filter)) &&
          refused_atomically(std::move(filter_arity_drift)) &&
          refused_atomically(std::move(distinct_arity_drift)) &&
          refused_atomically(std::move(ordered_argument_inversion)),
      "type-, expression-shape-, arity-, or argument-order-drifted "
      "STRING_AGG modifiers published evidence");
}

bool ValidateOrderedListaggExpressionValuesSpine() {
  struct SuccessCase {
    LiveListaggProfile profile;
    const char* expected;
    const char* label;
  };
  static constexpr SuccessCase kSuccessCases[] = {
      {LiveListaggProfile::kOrdered, "east|north|south", "ordered"},
      {LiveListaggProfile::kOverflowTruncateWithCount, "east|...(2)",
       "overflow truncate with count"},
      {LiveListaggProfile::kOverflowTruncateWithoutCount, "east|...",
       "overflow truncate without count"},
  };
  bool passed = true;
  for (const auto& test_case : kSuccessCases) {
    const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), OrderedListaggExpressionValuesEnvelope(test_case.profile),
         {}});
    const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), OrderedListaggExpressionValuesEnvelope(test_case.profile),
         {}});
    if (!first.api_result.ok) {
      for (const auto& diagnostic : first.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    passed &= Require(
        first.accepted && first.optimizer_admitted &&
            first.optimizer_selected && first.physical_dag_published &&
            first.physical_dag_executed && first.runtime_actuals_attached &&
            first.canonical_result_published && first.api_result.ok &&
            first.diagnostics.empty() && first.logical_node_count == 2 &&
            first.logical_property_count == 0 &&
            first.physical_node_count == 2 &&
            first.canonical_result_column_count == 1 &&
            first.canonical_result_row_count == 1,
        "VALUES global " + std::string(test_case.label) +
            " LISTAGG did not traverse its selected physical DAG");
    const auto& columns = first.api_result.result_shape.columns;
    const auto& rows = first.api_result.result_shape.rows;
    passed &= Require(
        columns.size() == 1 && rows.size() == 1 &&
            columns[0].canonical_type_name == "text" &&
            columns[0].encoded_descriptor.find("nullability=nullable") !=
                std::string::npos &&
            rows[0].fields.size() == 1 &&
            rows[0].fields[0].first == "listagg_value" &&
            rows[0].fields[0].second.state ==
                api::EngineValueState::value &&
            !rows[0].fields[0].second.is_null &&
            rows[0].fields[0].second.encoded_value == test_case.expected,
        "global " + std::string(test_case.label) +
            " LISTAGG ordering or overflow result drifted");
    passed &= Require(
        repeated.api_result.ok &&
            repeated.selected_plan_uuid == first.selected_plan_uuid &&
            repeated.canonical_result_bytes == first.canonical_result_bytes,
        "identical global " + std::string(test_case.label) +
            " LISTAGG input changed canonical plan/result bytes");
  }

  const auto overflow_error = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), OrderedListaggExpressionValuesEnvelope(
                      LiveListaggProfile::kOverflowError),
       {}});
  passed &= Require(
      overflow_error.accepted && overflow_error.optimizer_admitted &&
          !overflow_error.optimizer_selected &&
          !overflow_error.physical_dag_published &&
          !overflow_error.physical_dag_executed &&
          !overflow_error.runtime_actuals_attached &&
          !overflow_error.canonical_result_published &&
          !overflow_error.api_result.ok &&
          overflow_error.physical_node_count == 0 &&
          overflow_error.canonical_result_bytes.empty() &&
          HasApiDiagnostic(
              overflow_error,
              "QOW-DIAG-QRY-011-REGISTRY-LISTAGG-OVERFLOW-V1"),
      "LISTAGG ON OVERFLOW ERROR published partial selected-plan or result "
      "evidence");
  return passed;
}

bool ValidateOrderedListaggExpressionRefusalIsAtomic() {
  auto function_drift = OrderedListaggExpressionValuesEnvelope(
      LiveListaggProfile::kOverflowTruncateWithCount);
  auto result_nullability_drift = OrderedListaggExpressionValuesEnvelope(
      LiveListaggProfile::kOverflowTruncateWithCount);
  auto non_text_input = OrderedListaggExpressionValuesEnvelope(
      LiveListaggProfile::kOverflowTruncateWithCount);
  auto numeric_separator = OrderedListaggExpressionValuesEnvelope(
      LiveListaggProfile::kOverflowTruncateWithCount);
  auto null_separator = OrderedListaggExpressionValuesEnvelope(
      LiveListaggProfile::kOverflowTruncateWithCount);
  auto non_integer_order = OrderedListaggExpressionValuesEnvelope(
      LiveListaggProfile::kOverflowTruncateWithCount);
  auto zero_overflow_bound = OrderedListaggExpressionValuesEnvelope(
      LiveListaggProfile::kOverflowTruncateWithCount);
  auto null_indicator = OrderedListaggExpressionValuesEnvelope(
      LiveListaggProfile::kOverflowTruncateWithCount);
  auto numeric_count_option = OrderedListaggExpressionValuesEnvelope(
      LiveListaggProfile::kOverflowTruncateWithCount);
  auto arity_drift = OrderedListaggExpressionValuesEnvelope(
      LiveListaggProfile::kOverflowTruncateWithCount);
  for (auto& operand : function_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "15") {
      operand.value =
          "4|9,10,11,12,13,14|7|"
          "019de5fc-2400-78ac-b50c-45b832831004|-|-|-|-";
    }
  }
  for (auto& operand : result_nullability_drift.operands) {
    if (operand.type == "relational_descriptor_v1" && operand.name == "7") {
      operand.value = PairProfileUuid("7f10", "b4", "0d") + "|" +
                      PairProfileUuid("7f20", "b4", "0e") +
                      "|1|-|-|-|-|-";
    }
  }
  for (auto& operand : non_text_input.operands) {
    if (operand.type == "relational_expression_v1" &&
        (operand.name == "1" || operand.name == "3" ||
         operand.name == "5")) {
      operand.value =
          operand.name == "1"   ? "1|-|1|-|-|1|-|31"
          : operand.name == "3" ? "1|-|1|-|-|1|-|32"
                                  : "1|-|1|-|-|1|-|33";
    }
  }
  for (auto& operand : numeric_separator.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "10") {
      operand.value = "1|-|2|-|-|1|-|31";
    }
  }
  for (auto& operand : null_separator.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "10") {
      operand.value = "1|-|2|-|-|7|-|2d";
    }
  }
  for (auto& operand : non_integer_order.operands) {
    if (operand.type == "relational_expression_v1" &&
        (operand.name == "2" || operand.name == "4" ||
         operand.name == "8")) {
      operand.value =
          operand.name == "2"   ? "1|-|3|-|-|2|-|63"
          : operand.name == "4" ? "1|-|3|-|-|2|-|61"
                                  : "1|-|3|-|-|2|-|62";
    }
  }
  for (auto& operand : zero_overflow_bound.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "12") {
      operand.value = "1|-|4|-|-|1|-|30";
    }
  }
  for (auto& operand : null_indicator.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "13") {
      operand.value = "1|-|5|-|-|7|-|2d";
    }
  }
  for (auto& operand : numeric_count_option.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "14") {
      operand.value = "1|-|6|-|-|1|-|31";
    }
  }
  for (auto& operand : arity_drift.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "15") {
      operand.value =
          "4|9,10,11,12,13|7|"
          "019dffbb-f000-7e93-8e4d-6063849de049|-|-|-|-";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(function_drift)) &&
          refused_atomically(std::move(result_nullability_drift)) &&
          refused_atomically(std::move(non_text_input)) &&
          refused_atomically(std::move(numeric_separator)) &&
          refused_atomically(std::move(null_separator)) &&
          refused_atomically(std::move(non_integer_order)) &&
          refused_atomically(std::move(zero_overflow_bound)) &&
          refused_atomically(std::move(null_indicator)) &&
          refused_atomically(std::move(numeric_count_option)) &&
          refused_atomically(std::move(arity_drift)),
      "function-, result-, input-, separator-, order-, overflow-option-, or "
      "arity-drifted LISTAGG published evidence");
}

std::string_view OrderedListaggModifierExpected(
    const LiveListaggProfile profile,
    const AggregateModifierProfile modifier) {
  if (profile == LiveListaggProfile::kOverflowTruncateWithCount) {
    switch (modifier) {
      case AggregateModifierProfile::kFilter:
        return "delta|...(3)";
      case AggregateModifierProfile::kDistinct:
        return "gamma|...(3)";
      case AggregateModifierProfile::kDistinctFilter:
        return "beta|...(2)";
    }
  }
  return StringAggModifierExpected(true, modifier);
}

bool ValidateOrderedListaggModifierValuesSpine() {
  bool passed = true;
  for (const auto profile : {LiveListaggProfile::kOrdered,
                             LiveListaggProfile::kOverflowError,
                             LiveListaggProfile::kOverflowTruncateWithCount}) {
    const std::string_view form =
        profile == LiveListaggProfile::kOrdered
            ? "ordered"
            : profile == LiveListaggProfile::kOverflowError
                  ? "overflow error"
                  : "overflow truncate";
    for (const auto modifier : {AggregateModifierProfile::kFilter,
                                AggregateModifierProfile::kDistinct,
                                AggregateModifierProfile::kDistinctFilter}) {
      const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(),
           OrderedListaggModifierExpressionValuesEnvelope(profile, modifier),
           {}});
      const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(),
           OrderedListaggModifierExpressionValuesEnvelope(profile, modifier),
           {}});
      if (!first.api_result.ok) {
        for (const auto& diagnostic : first.api_result.diagnostics) {
          std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
        }
      }
      const auto& columns = first.api_result.result_shape.columns;
      const auto& rows = first.api_result.result_shape.rows;
      const bool exact_output =
          columns.size() == 1 && rows.size() == 1 &&
          columns[0].canonical_type_name == "text" &&
          columns[0].encoded_descriptor.find("nullability=nullable") !=
              std::string::npos &&
          rows[0].fields.size() == 1 &&
          rows[0].fields[0].first == "listagg_modifier_value" &&
          rows[0].fields[0].second.state == api::EngineValueState::value &&
          !rows[0].fields[0].second.is_null &&
          rows[0].fields[0].second.encoded_value ==
              OrderedListaggModifierExpected(profile, modifier);
      passed &= Require(
          first.accepted && first.optimizer_admitted &&
              first.optimizer_selected && first.physical_dag_published &&
              first.physical_dag_executed &&
              first.runtime_actuals_attached &&
              first.canonical_result_published && first.api_result.ok &&
              first.diagnostics.empty() && first.logical_node_count == 2 &&
              first.logical_property_count == 0 &&
              first.physical_node_count == 2 && exact_output,
          "ordered LISTAGG " + std::string(form) + " " +
              std::string(AggregateModifierName(modifier)) +
              " did not execute through the selected canonical spine");
      passed &= Require(
          repeated.api_result.ok &&
              repeated.selected_plan_uuid == first.selected_plan_uuid &&
              repeated.canonical_result_bytes == first.canonical_result_bytes,
          "identical ordered LISTAGG " + std::string(form) + " " +
              std::string(AggregateModifierName(modifier)) +
              " input changed canonical plan/result bytes");
    }
  }
  return passed;
}

bool ValidateOrderedListaggModifierRefusalIsAtomic() {
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
  };
  const auto set_function_children =
      [](sblr::SblrOperationEnvelope& envelope,
         const std::string& children) {
        for (auto& operand : envelope.operands) {
          if (operand.type == "relational_expression_v1" &&
              operand.name == "29") {
            operand.value =
                "4|" + children +
                "|8|019dffbb-f000-7e93-8e4d-6063849de049|-|-|-|-";
          }
        }
      };

  bool passed = true;
  for (const auto profile : {LiveListaggProfile::kOrdered,
                             LiveListaggProfile::kOverflowError,
                             LiveListaggProfile::kOverflowTruncateWithCount}) {
    auto non_boolean_filter = OrderedListaggModifierExpressionValuesEnvelope(
        profile, AggregateModifierProfile::kFilter);
    auto non_identifier_filter =
        OrderedListaggModifierExpressionValuesEnvelope(
            profile, AggregateModifierProfile::kFilter);
    auto missing_filter = OrderedListaggModifierExpressionValuesEnvelope(
        profile, AggregateModifierProfile::kFilter);
    auto distinct_extra_filter =
        OrderedListaggModifierExpressionValuesEnvelope(
            profile, AggregateModifierProfile::kDistinct);
    auto option_filter_inversion =
        OrderedListaggModifierExpressionValuesEnvelope(
            profile, AggregateModifierProfile::kDistinctFilter);
    for (auto& operand : non_boolean_filter.operands) {
      if (operand.type == "relational_expression_v1" &&
          (operand.name == "3" || operand.name == "6" ||
           operand.name == "9" || operand.name == "12" ||
           operand.name == "15" || operand.name == "18" ||
           operand.name == "21")) {
        operand.value = "1|-|4|-|-|1|-|31";
      }
    }
    if (profile == LiveListaggProfile::kOrdered) {
      set_function_children(non_identifier_filter, "22,23,24,3");
      set_function_children(missing_filter, "22,23,24");
      set_function_children(distinct_extra_filter, "22,23,24,28");
      set_function_children(option_filter_inversion, "22,23,28,24");
    } else if (profile == LiveListaggProfile::kOverflowError) {
      set_function_children(non_identifier_filter, "22,23,24,25,3");
      set_function_children(missing_filter, "22,23,24,25");
      set_function_children(distinct_extra_filter, "22,23,24,25,28");
      set_function_children(option_filter_inversion, "22,23,24,28,25");
    } else {
      set_function_children(non_identifier_filter,
                            "22,23,24,25,26,27,3");
      set_function_children(missing_filter, "22,23,24,25,26,27");
      set_function_children(distinct_extra_filter,
                            "22,23,24,25,26,27,28");
      set_function_children(option_filter_inversion,
                            "22,23,24,25,26,28,27");
    }
    passed &= refused_atomically(std::move(non_boolean_filter)) &&
              refused_atomically(std::move(non_identifier_filter)) &&
              refused_atomically(std::move(missing_filter)) &&
              refused_atomically(std::move(distinct_extra_filter)) &&
              refused_atomically(std::move(option_filter_inversion));
  }
  return Require(
      passed,
      "type-, expression-shape-, arity-, or option/filter-order-drifted "
      "ordered LISTAGG modifiers published evidence");
}

bool ValidateOrderedSingleCollectionValuesSpine() {
  bool passed = true;
  for (const auto& profile : kOrderedSingleCollectionProfiles) {
    const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), OrderedSingleCollectionValuesEnvelope(profile), {}});
    const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), OrderedSingleCollectionValuesEnvelope(profile), {}});
    if (!first.api_result.ok) {
      for (const auto& diagnostic : first.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    passed &= Require(
        first.accepted && first.optimizer_admitted &&
            first.optimizer_selected && first.physical_dag_published &&
            first.physical_dag_executed && first.runtime_actuals_attached &&
            first.canonical_result_published && first.api_result.ok &&
            first.diagnostics.empty() && first.logical_node_count == 2 &&
            first.logical_property_count == 0 &&
            first.physical_node_count == 2 &&
            first.canonical_result_column_count == 1 &&
            first.canonical_result_row_count == 1,
        "VALUES global ordered " + std::string(profile.name) +
            " did not traverse its selected physical DAG");

    const auto& columns = first.api_result.result_shape.columns;
    const auto& rows = first.api_result.result_shape.rows;
    passed &= Require(
        columns.size() == 1 && rows.size() == 1 &&
            columns[0].canonical_type_name == profile.result_type &&
            columns[0].encoded_descriptor.find("nullability=nullable") !=
                std::string::npos &&
            rows[0].fields.size() == 1 &&
            rows[0].fields[0].first == profile.output_name &&
            rows[0].fields[0].second.state == api::EngineValueState::value &&
            !rows[0].fields[0].second.is_null &&
            rows[0].fields[0].second.encoded_value == profile.expected,
        "global ordered " + std::string(profile.name) +
            " did not apply its bound order or include SQL NULL");
    passed &= Require(
        repeated.api_result.ok &&
            repeated.selected_plan_uuid == first.selected_plan_uuid &&
            repeated.canonical_result_bytes == first.canonical_result_bytes,
        "identical global ordered " + std::string(profile.name) +
            " input changed canonical plan/result bytes");
  }
  return passed;
}

bool ValidateOrderedSingleCollectionRefusalIsAtomic() {
  bool passed = true;
  for (const auto& profile : kOrderedSingleCollectionProfiles) {
    auto function_drift = OrderedSingleCollectionValuesEnvelope(profile);
    auto result_nullability_drift =
        OrderedSingleCollectionValuesEnvelope(profile);
    auto non_text_value = OrderedSingleCollectionValuesEnvelope(profile);
    auto non_integer_order = OrderedSingleCollectionValuesEnvelope(profile);
    auto arity_drift = OrderedSingleCollectionValuesEnvelope(profile);
    for (auto& operand : function_drift.operands) {
      if (operand.type == "relational_expression_v1" &&
          operand.name == "11") {
        operand.value =
            "4|9,10|3|019de5fc-2400-78ac-b50c-45b832831004|-|-|-|-";
      }
    }
    for (auto& operand : result_nullability_drift.operands) {
      if (operand.type == "relational_descriptor_v1" &&
          operand.name == "3") {
        operand.value = PairProfileUuid("7d10", profile.uuid_family, "05") +
                        "|" +
                        PairProfileUuid("7d20", profile.uuid_family, "06") +
                        "|1|-|-|-|-|-";
      }
    }
    for (auto& operand : non_text_value.operands) {
      if (operand.type == "relational_expression_v1" &&
          (operand.name == "1" || operand.name == "3" ||
           operand.name == "7")) {
        operand.value =
            operand.name == "1"   ? "1|-|1|-|-|1|-|32"
            : operand.name == "3" ? "1|-|1|-|-|1|-|34"
                                    : "1|-|1|-|-|1|-|31";
      }
    }
    for (auto& operand : non_integer_order.operands) {
      if (operand.type == "relational_expression_v1" &&
          (operand.name == "2" || operand.name == "4" ||
           operand.name == "6" || operand.name == "8")) {
        operand.value =
            operand.name == "2"   ? "1|-|2|-|-|2|-|62"
            : operand.name == "4" ? "1|-|2|-|-|2|-|64"
            : operand.name == "6" ? "1|-|2|-|-|2|-|63"
                                    : "1|-|2|-|-|2|-|61";
      }
    }
    for (auto& operand : arity_drift.operands) {
      if (operand.type == "relational_expression_v1" &&
          operand.name == "11") {
        operand.value = "4|9|3|" + std::string(profile.function_uuid) +
                        "|-|-|-|-";
      }
    }
    const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
      const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(), std::move(envelope), {}});
      return result.accepted && result.optimizer_admitted &&
             !result.optimizer_selected && !result.physical_dag_published &&
             !result.physical_dag_executed &&
             !result.runtime_actuals_attached &&
             !result.canonical_result_published && !result.api_result.ok &&
             result.physical_node_count == 0 &&
             result.canonical_result_bytes.empty() &&
             HasApiDiagnostic(
                 result, "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
    };
    passed &= Require(
        refused_atomically(std::move(function_drift)) &&
            refused_atomically(std::move(result_nullability_drift)) &&
            refused_atomically(std::move(non_text_value)) &&
            refused_atomically(std::move(non_integer_order)) &&
            refused_atomically(std::move(arity_drift)),
        "function-, result-, value-, order-, or arity-drifted ordered " +
            std::string(profile.name) + " published evidence");
  }
  return passed;
}

std::string_view OrderedSingleCollectionModifierExpected(
    const OrderedSingleCollectionProfile& profile,
    const AggregateModifierProfile modifier) {
  const bool is_array_agg = profile.name == "ARRAY_AGG";
  if (is_array_agg) {
    switch (modifier) {
      case AggregateModifierProfile::kFilter:
        return "list[NULL;text:delta;text:beta;text:alpha;text:delta]";
      case AggregateModifierProfile::kDistinct:
        return "list[NULL;text:gamma;text:alpha;text:beta;text:delta]";
      case AggregateModifierProfile::kDistinctFilter:
        return "list[NULL;text:beta;text:alpha;text:delta]";
    }
  }
  switch (modifier) {
    case AggregateModifierProfile::kFilter:
      return R"([null,"delta","beta","alpha","delta"])";
    case AggregateModifierProfile::kDistinct:
      return R"([null,"gamma","alpha","beta","delta"])";
    case AggregateModifierProfile::kDistinctFilter:
      return R"([null,"beta","alpha","delta"])";
  }
  return {};
}

bool ValidateOrderedSingleCollectionModifierValuesSpine() {
  bool passed = true;
  for (const auto& profile : kOrderedSingleCollectionProfiles) {
    for (const auto modifier : {AggregateModifierProfile::kFilter,
                                AggregateModifierProfile::kDistinct,
                                AggregateModifierProfile::kDistinctFilter}) {
      const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(), OrderedSingleCollectionModifierValuesEnvelope(
                          profile, modifier),
           {}});
      const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(), OrderedSingleCollectionModifierValuesEnvelope(
                          profile, modifier),
           {}});
      if (!first.api_result.ok) {
        for (const auto& diagnostic : first.api_result.diagnostics) {
          std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
        }
      }
      passed &= Require(
          first.accepted && first.optimizer_admitted &&
              first.optimizer_selected && first.physical_dag_published &&
              first.physical_dag_executed &&
              first.runtime_actuals_attached &&
              first.canonical_result_published && first.api_result.ok &&
              first.diagnostics.empty() && first.logical_node_count == 2 &&
              first.logical_property_count == 0 &&
              first.physical_node_count == 2 &&
              first.canonical_result_column_count == 1 &&
              first.canonical_result_row_count == 1,
          "VALUES global ordered " + std::string(profile.name) + " " +
              std::string(AggregateModifierName(modifier)) +
              " did not traverse its selected physical DAG");

      const auto& columns = first.api_result.result_shape.columns;
      const auto& rows = first.api_result.result_shape.rows;
      passed &= Require(
          columns.size() == 1 && rows.size() == 1 &&
              columns[0].canonical_type_name == profile.result_type &&
              columns[0].encoded_descriptor.find("nullability=nullable") !=
                  std::string::npos &&
              rows[0].fields.size() == 1 &&
              rows[0].fields[0].first == profile.output_name &&
              rows[0].fields[0].second.state ==
                  api::EngineValueState::value &&
              !rows[0].fields[0].second.is_null &&
              rows[0].fields[0].second.encoded_value ==
                  OrderedSingleCollectionModifierExpected(profile, modifier),
          "global ordered " + std::string(profile.name) + " " +
              std::string(AggregateModifierName(modifier)) +
              " did not apply FILTER-before-DISTINCT-before-order");
      passed &= Require(
          repeated.api_result.ok &&
              repeated.selected_plan_uuid == first.selected_plan_uuid &&
              repeated.canonical_result_bytes ==
                  first.canonical_result_bytes,
          "identical global ordered " + std::string(profile.name) + " " +
              std::string(AggregateModifierName(modifier)) +
              " input changed canonical plan/result bytes");
    }
  }
  return passed;
}

bool ValidateOrderedSingleCollectionModifierRefusalIsAtomic() {
  bool passed = true;
  for (const auto& profile : kOrderedSingleCollectionProfiles) {
    auto non_boolean_filter =
        OrderedSingleCollectionModifierValuesEnvelope(
            profile, AggregateModifierProfile::kFilter);
    auto non_identifier_filter =
        OrderedSingleCollectionModifierValuesEnvelope(
            profile, AggregateModifierProfile::kFilter);
    auto filter_arity_drift =
        OrderedSingleCollectionModifierValuesEnvelope(
            profile, AggregateModifierProfile::kFilter);
    auto distinct_arity_drift =
        OrderedSingleCollectionModifierValuesEnvelope(
            profile, AggregateModifierProfile::kDistinct);
    auto order_filter_inversion =
        OrderedSingleCollectionModifierValuesEnvelope(
            profile, AggregateModifierProfile::kDistinctFilter);
    for (auto& operand : non_boolean_filter.operands) {
      if (operand.type == "relational_expression_v1" &&
          (operand.name == "3" || operand.name == "6" ||
           operand.name == "9" || operand.name == "12" ||
           operand.name == "15" || operand.name == "18" ||
           operand.name == "21")) {
        operand.value = "1|-|3|-|-|1|-|31";
      }
    }
    for (auto& operand : non_identifier_filter.operands) {
      if (operand.type == "relational_expression_v1" &&
          operand.name == "25") {
        operand.value = "4|22,23,3|4|" +
                        std::string(profile.function_uuid) + "|-|-|-|-";
      }
    }
    for (auto& operand : filter_arity_drift.operands) {
      if (operand.type == "relational_expression_v1" &&
          operand.name == "25") {
        operand.value = "4|22,23|4|" +
                        std::string(profile.function_uuid) + "|-|-|-|-";
      }
    }
    for (auto& operand : distinct_arity_drift.operands) {
      if (operand.type == "relational_expression_v1" &&
          operand.name == "25") {
        operand.value = "4|22,23,24|4|" +
                        std::string(profile.function_uuid) + "|-|-|-|-";
      }
    }
    for (auto& operand : order_filter_inversion.operands) {
      if (operand.type == "relational_expression_v1" &&
          operand.name == "25") {
        operand.value = "4|22,24,23|4|" +
                        std::string(profile.function_uuid) + "|-|-|-|-";
      }
    }
    const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
      const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(), std::move(envelope), {}});
      return result.accepted && result.optimizer_admitted &&
             !result.optimizer_selected && !result.physical_dag_published &&
             !result.physical_dag_executed &&
             !result.runtime_actuals_attached &&
             !result.canonical_result_published && !result.api_result.ok &&
             result.physical_node_count == 0 &&
             result.canonical_result_bytes.empty() &&
             HasApiDiagnostic(
                 result, "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
    };
    passed &= Require(
        refused_atomically(std::move(non_boolean_filter)) &&
            refused_atomically(std::move(non_identifier_filter)) &&
            refused_atomically(std::move(filter_arity_drift)) &&
            refused_atomically(std::move(distinct_arity_drift)) &&
            refused_atomically(std::move(order_filter_inversion)),
        "type-, expression-shape-, arity-, or argument-order-drifted " +
            std::string(profile.name) + " modifiers published evidence");
  }
  return passed;
}

bool ValidateOrderedJsonObjectAggValuesSpine() {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), OrderedJsonObjectAggValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), OrderedJsonObjectAggValuesEnvelope(), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = Require(
      first.accepted && first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached &&
          first.canonical_result_published && first.api_result.ok &&
          first.diagnostics.empty() && first.logical_node_count == 2 &&
          first.logical_property_count == 0 && first.physical_node_count == 2 &&
          first.canonical_result_column_count == 1 &&
          first.canonical_result_row_count == 1,
      "VALUES global ordered JSON_OBJECT_AGG did not traverse its selected "
      "physical DAG");

  const auto& columns = first.api_result.result_shape.columns;
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      columns.size() == 1 && rows.size() == 1 &&
          columns[0].canonical_type_name == "json" &&
          columns[0].encoded_descriptor.find("nullability=nullable") !=
              std::string::npos &&
          rows[0].fields.size() == 1 &&
          rows[0].fields[0].first == "json_object_agg_value" &&
          rows[0].fields[0].second.state == api::EngineValueState::value &&
          !rows[0].fields[0].second.is_null &&
          rows[0].fields[0].second.encoded_value ==
              R"({"other":2,"dup":null,"tail":4})",
      "global ordered JSON_OBJECT_AGG did not apply bound order, replace its "
      "duplicate key, or render SQL NULL as JSON null");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical global ordered JSON_OBJECT_AGG input changed canonical "
      "plan/result bytes");
  return passed;
}

bool ValidateOrderedJsonObjectAggRefusalIsAtomic() {
  auto function_drift = OrderedJsonObjectAggValuesEnvelope();
  auto result_nullability_drift = OrderedJsonObjectAggValuesEnvelope();
  auto non_text_key = OrderedJsonObjectAggValuesEnvelope();
  auto non_integer_value = OrderedJsonObjectAggValuesEnvelope();
  auto non_integer_order = OrderedJsonObjectAggValuesEnvelope();
  auto arity_drift = OrderedJsonObjectAggValuesEnvelope();
  auto null_key = OrderedJsonObjectAggValuesEnvelope();
  for (auto& operand : function_drift.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "16") {
      operand.value =
          "4|13,14,15|4|019de5fc-2400-78ac-b50c-45b832831004|-|-|-|-";
    }
  }
  for (auto& operand : result_nullability_drift.operands) {
    if (operand.type == "relational_descriptor_v1" && operand.name == "4") {
      operand.value = PairProfileUuid("7e10", "b2", "07") + "|" +
                      PairProfileUuid("7e20", "b2", "08") +
                      "|1|-|-|-|-|-";
    }
  }
  for (auto& operand : non_text_key.operands) {
    if (operand.type == "relational_expression_v1" &&
        (operand.name == "1" || operand.name == "4" ||
         operand.name == "7" || operand.name == "10")) {
      operand.value =
          operand.name == "1"   ? "1|-|1|-|-|1|-|34"
          : operand.name == "4" ? "1|-|1|-|-|1|-|33"
          : operand.name == "7" ? "1|-|1|-|-|1|-|31"
                                  : "1|-|1|-|-|1|-|32";
    }
  }
  for (auto& operand : non_integer_value.operands) {
    if (operand.type == "relational_expression_v1" &&
        (operand.name == "2" || operand.name == "8" ||
         operand.name == "11")) {
      operand.value =
          operand.name == "2"   ? "1|-|2|-|-|2|-|64"
          : operand.name == "8" ? "1|-|2|-|-|2|-|61"
                                  : "1|-|2|-|-|2|-|62";
    }
  }
  for (auto& operand : non_integer_order.operands) {
    if (operand.type == "relational_expression_v1" &&
        (operand.name == "3" || operand.name == "6" ||
         operand.name == "9" || operand.name == "12")) {
      operand.value =
          operand.name == "3"   ? "1|-|3|-|-|2|-|64"
          : operand.name == "6" ? "1|-|3|-|-|2|-|63"
          : operand.name == "9" ? "1|-|3|-|-|2|-|61"
                                  : "1|-|3|-|-|2|-|62";
    }
  }
  for (auto& operand : arity_drift.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "16") {
      operand.value =
          "4|13,14|4|019dffbb-f001-7021-8a00-000000000024|-|-|-|-";
    }
  }
  for (auto& operand : null_key.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "1") {
      operand.value = "1|-|1|-|-|7|-|2d";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope,
                                     const std::string_view diagnostic_code) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(result, diagnostic_code);
  };
  return Require(
      refused_atomically(
          std::move(function_drift),
          "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1") &&
          refused_atomically(
              std::move(result_nullability_drift),
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1") &&
          refused_atomically(
              std::move(non_text_key),
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1") &&
          refused_atomically(
              std::move(non_integer_value),
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1") &&
          refused_atomically(
              std::move(non_integer_order),
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1") &&
          refused_atomically(
              std::move(arity_drift),
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1") &&
          refused_atomically(
              std::move(null_key),
              "QOW-DIAG-QRY-011-REGISTRY-JSON-KEY-V1"),
      "function-, result-, key-, value-, order-, arity-, or NULL-key-drifted "
      "ordered JSON_OBJECT_AGG published evidence");
}

std::string_view OrderedJsonObjectAggModifierExpected(
    const AggregateModifierProfile modifier) {
  switch (modifier) {
    case AggregateModifierProfile::kFilter:
    case AggregateModifierProfile::kDistinctFilter:
      return R"({"nullv":null,"pair":20,"dup":10,"repeat":40})";
    case AggregateModifierProfile::kDistinct:
      return R"({"nullv":null,"pair":20,"other":30,"dup":10,"repeat":40})";
  }
  return {};
}

bool ValidateOrderedJsonObjectAggModifierValuesSpine() {
  bool passed = true;
  for (const auto modifier : {AggregateModifierProfile::kFilter,
                              AggregateModifierProfile::kDistinct,
                              AggregateModifierProfile::kDistinctFilter}) {
    const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), OrderedJsonObjectAggModifierValuesEnvelope(modifier), {}});
    const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), OrderedJsonObjectAggModifierValuesEnvelope(modifier), {}});
    if (!first.api_result.ok) {
      for (const auto& diagnostic : first.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    passed &= Require(
        first.accepted && first.optimizer_admitted &&
            first.optimizer_selected && first.physical_dag_published &&
            first.physical_dag_executed &&
            first.runtime_actuals_attached &&
            first.canonical_result_published && first.api_result.ok &&
            first.diagnostics.empty() && first.logical_node_count == 2 &&
            first.logical_property_count == 0 &&
            first.physical_node_count == 2 &&
            first.canonical_result_column_count == 1 &&
            first.canonical_result_row_count == 1,
        "VALUES global ordered JSON_OBJECT_AGG " +
            std::string(AggregateModifierName(modifier)) +
            " did not traverse its selected physical DAG");

    const auto& columns = first.api_result.result_shape.columns;
    const auto& rows = first.api_result.result_shape.rows;
    passed &= Require(
        columns.size() == 1 && rows.size() == 1 &&
            columns[0].canonical_type_name == "json" &&
            columns[0].encoded_descriptor.find("nullability=nullable") !=
                std::string::npos &&
            rows[0].fields.size() == 1 &&
            rows[0].fields[0].first == "json_object_agg_value" &&
            rows[0].fields[0].second.state == api::EngineValueState::value &&
            !rows[0].fields[0].second.is_null &&
            rows[0].fields[0].second.encoded_value ==
                OrderedJsonObjectAggModifierExpected(modifier),
        "global ordered JSON_OBJECT_AGG " +
            std::string(AggregateModifierName(modifier)) +
            " did not apply FILTER-before-complete-tuple-DISTINCT-before-"
            "order-before-last-key-wins");
    passed &= Require(
        repeated.api_result.ok &&
            repeated.selected_plan_uuid == first.selected_plan_uuid &&
            repeated.canonical_result_bytes == first.canonical_result_bytes,
        "identical global ordered JSON_OBJECT_AGG " +
            std::string(AggregateModifierName(modifier)) +
            " input changed canonical plan/result bytes");
  }
  return passed;
}

bool ValidateOrderedJsonObjectAggModifierRefusalIsAtomic() {
  auto non_boolean_filter = OrderedJsonObjectAggModifierValuesEnvelope(
      AggregateModifierProfile::kFilter);
  auto non_identifier_filter = OrderedJsonObjectAggModifierValuesEnvelope(
      AggregateModifierProfile::kFilter);
  auto filter_arity_drift = OrderedJsonObjectAggModifierValuesEnvelope(
      AggregateModifierProfile::kFilter);
  auto distinct_arity_drift = OrderedJsonObjectAggModifierValuesEnvelope(
      AggregateModifierProfile::kDistinct);
  auto order_filter_inversion = OrderedJsonObjectAggModifierValuesEnvelope(
      AggregateModifierProfile::kDistinctFilter);
  for (auto& operand : non_boolean_filter.operands) {
    if (operand.type == "relational_expression_v1" &&
        (operand.name == "4" || operand.name == "8" ||
         operand.name == "12" || operand.name == "16" ||
         operand.name == "20" || operand.name == "24" ||
         operand.name == "28" || operand.name == "32")) {
      operand.value = "1|-|4|-|-|1|-|31";
    }
  }
  for (auto& operand : non_identifier_filter.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "37") {
      operand.value =
          "4|33,34,35,4|5|019dffbb-f001-7021-8a00-000000000024|-|-|-|-";
    }
  }
  for (auto& operand : filter_arity_drift.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "37") {
      operand.value =
          "4|33,34,35|5|019dffbb-f001-7021-8a00-000000000024|-|-|-|-";
    }
  }
  for (auto& operand : distinct_arity_drift.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "37") {
      operand.value =
          "4|33,34,35,36|5|019dffbb-f001-7021-8a00-000000000024|-|-|-|-";
    }
  }
  for (auto& operand : order_filter_inversion.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "37") {
      operand.value =
          "4|33,34,36,35|5|019dffbb-f001-7021-8a00-000000000024|-|-|-|-";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(non_boolean_filter)) &&
          refused_atomically(std::move(non_identifier_filter)) &&
          refused_atomically(std::move(filter_arity_drift)) &&
          refused_atomically(std::move(distinct_arity_drift)) &&
          refused_atomically(std::move(order_filter_inversion)),
      "type-, expression-shape-, arity-, or argument-order-drifted ordered "
      "JSON_OBJECT_AGG modifiers published evidence");
}

bool ValidateSortValuesSpine() {
  const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), SortValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), SortValuesEnvelope(), {}});
  if (!first.api_result.ok) {
    for (const auto& diagnostic : first.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  bool passed = true;
  passed &= Require(
      first.accepted && first.optimizer_admitted && first.optimizer_selected &&
          first.physical_dag_published && first.physical_dag_executed &&
          first.runtime_actuals_attached && first.canonical_result_published &&
          first.api_result.ok && first.diagnostics.empty() &&
          first.logical_node_count == 2 && first.logical_property_count == 1 &&
          first.physical_node_count == 2 &&
          first.canonical_result_column_count == 3 &&
          first.canonical_result_row_count == 6,
      "VALUES SORT did not traverse the property-enforcing physical DAG");
  const auto& rows = first.api_result.result_shape.rows;
  passed &= Require(
      first.api_result.result_shape.columns.size() == 3 && rows.size() == 6 &&
          rows[0].fields[0].second.state == api::EngineValueState::sql_null &&
          rows[0].fields[1].second.encoded_value == "0" &&
          rows[0].fields[2].second.encoded_value == "5" &&
          rows[1].fields[0].second.state == api::EngineValueState::sql_null &&
          rows[1].fields[2].second.encoded_value == "2" &&
          rows[2].fields[2].second.encoded_value == "3" &&
          rows[3].fields[2].second.encoded_value == "1" &&
          rows[4].fields[2].second.encoded_value == "6" &&
          rows[5].fields[0].second.encoded_value == "1" &&
          rows[5].fields[2].second.encoded_value == "4",
      "SORT did not apply DESC, NULLS FIRST, ASC, and stable tie ordering");
  passed &= Require(
      repeated.api_result.ok &&
          repeated.selected_plan_uuid == first.selected_plan_uuid &&
          repeated.canonical_result_bytes == first.canonical_result_bytes,
      "identical SORT input changed canonical plan/result bytes");
  return passed;
}

bool ValidateSortRefusalIsAtomic() {
  auto schema_drift = SortValuesEnvelope();
  auto invalid_collation = SortValuesEnvelope();
  for (auto& operand : schema_drift.operands) {
    if (operand.type == "relational_node_v1" && operand.name == "2") {
      operand.value = "6|0|1|2,1,3|-";
    }
  }
  for (auto& operand : invalid_collation.operands) {
    if (operand.type == "relational_property_v1") {
      operand.value =
          "1|2|-|1:2:1:019f0000-0000-7200-8000-000000008908,"
          "2:1:2:-|-|-";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-SORT-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(schema_drift)) &&
          refused_atomically(std::move(invalid_collation)),
      "schema-drifted or invalid-collation SORT published partial evidence");
}

// RCP-033-TEST-LIVE-ROW-DEPENDENT-SORT-EXPRESSION-V1
bool ValidateRowDependentSortExpressionSpine() {
  const auto dispatch = [](sblr::SblrOperationEnvelope envelope,
                           api::EngineRequestContext context = Context()) {
    return sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), std::move(envelope), {}});
  };
  const auto completed = [](const sblr::SblrDispatchResult& result) {
    return result.accepted && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.diagnostics.empty() && result.logical_node_count == 2 &&
           result.logical_property_count == 1 &&
           result.physical_node_count == 2 &&
           result.canonical_result_column_count == 3 &&
           result.canonical_result_row_count == 6 &&
           result.api_result.result_shape.columns.size() == 3 &&
           result.api_result.result_shape.rows.size() == 6;
  };

  const auto sorted = dispatch(RowDependentSortValuesEnvelope());
  const auto repeated = dispatch(RowDependentSortValuesEnvelope());
  bool passed = true;
  if (!sorted.api_result.ok) {
    for (const auto& diagnostic : sorted.api_result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
    }
  }
  const auto& rows = sorted.api_result.result_shape.rows;
  passed &= Require(
      completed(sorted) && rows[0].fields[2].second.encoded_value == "2" &&
          rows[1].fields[2].second.encoded_value == "5" &&
          rows[2].fields[2].second.encoded_value == "6" &&
          rows[3].fields[2].second.encoded_value == "3" &&
          rows[4].fields[2].second.encoded_value == "4" &&
          rows[5].fields[2].second.encoded_value == "1" &&
          sorted.api_result.result_shape.columns.size() == 3,
      "row-dependent SORT did not evaluate arithmetic, preserve NULLS FIRST, "
      "or remove its internal ordering key");
  passed &= Require(
      completed(repeated) &&
          repeated.selected_plan_uuid == sorted.selected_plan_uuid &&
          repeated.canonical_result_bytes == sorted.canonical_result_bytes,
      "row-dependent SORT changed deterministic plan/result bytes");

  auto unbound = RowDependentSortValuesEnvelope();
  for (auto& operand : unbound.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "20") {
      operand.value =
          "3|-|4|-|019f0000-0000-7500-8000-000000008910|-|-|-";
    }
  }
  auto bounded_context = Context();
  bounded_context.optimizer_maximum_candidate_count = 5;
  const auto unbound_result = dispatch(std::move(unbound));
  const auto exhausted_result = dispatch(RowDependentSortValuesEnvelope(),
                                         std::move(bounded_context));
  const auto refused_before_publication = [](const auto& result,
                                             const std::string_view code) {
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(result, code);
  };
  passed &= Require(
      refused_before_publication(
          unbound_result, "QOW-DIAG-RELATIONAL-LIVE-SORT-PAYLOAD-V1") &&
          refused_before_publication(
              exhausted_result,
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"),
      "unbound or resource-exhausted row SORT published partial evidence");
  return passed;
}

bool ValidateDistinctSortLimitValuesSpine() {
  bool passed = true;
  std::string limit_selected_plan;
  std::string fetch_selected_plan;
  for (const bool fetch_first_rows_only : {false, true}) {
    const auto first = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(),
         DistinctSortLimitValuesEnvelope(fetch_first_rows_only), {}});
    const auto repeated = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(),
         DistinctSortLimitValuesEnvelope(fetch_first_rows_only), {}});
    if (!first.api_result.ok) {
      for (const auto& diagnostic : first.api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
    passed &= Require(
        first.accepted && first.optimizer_admitted &&
            first.optimizer_selected && first.physical_dag_published &&
            first.physical_dag_executed &&
            first.runtime_actuals_attached &&
            first.canonical_result_published && first.api_result.ok &&
            first.diagnostics.empty() && first.logical_node_count == 4 &&
            first.logical_property_count == 1 &&
            first.physical_node_count == 4 &&
            first.canonical_result_column_count == 2 &&
            first.canonical_result_row_count == 2,
        std::string(fetch_first_rows_only ? "FETCH" : "LIMIT") +
            " DISTINCT/ORDER/OFFSET composition did not traverse its exact "
            "four-node selected DAG");
    const auto& columns = first.api_result.result_shape.columns;
    const auto& rows = first.api_result.result_shape.rows;
    passed &= Require(
        columns.size() == 2 && rows.size() == 2 &&
            rows[0].fields.size() == 2 && rows[1].fields.size() == 2 &&
            rows[0].fields[0].second.encoded_value == "1" &&
            rows[0].fields[1].second.encoded_value == "10" &&
            rows[1].fields[0].second.encoded_value == "2" &&
            rows[1].fields[1].second.encoded_value == "20",
        std::string(fetch_first_rows_only ? "FETCH" : "LIMIT") +
            " composition did not eliminate complete-row duplicates before "
            "typed ordering and offset/count application");
    passed &= Require(
        repeated.api_result.ok &&
            repeated.selected_plan_uuid == first.selected_plan_uuid &&
            repeated.canonical_result_bytes == first.canonical_result_bytes,
        std::string(fetch_first_rows_only ? "FETCH" : "LIMIT") +
            " composition changed canonical plan/result bytes on replay");
    if (fetch_first_rows_only) {
      fetch_selected_plan = first.selected_plan_uuid;
    } else {
      limit_selected_plan = first.selected_plan_uuid;
    }
  }
  passed &= Require(!limit_selected_plan.empty() &&
                        !fetch_selected_plan.empty() &&
                        limit_selected_plan != fetch_selected_plan,
                    "LIMIT and FETCH profiles collapsed to one selected-plan "
                    "identity");
  return passed;
}

bool ValidateDistinctSortLimitRefusalIsAtomic() {
  auto incomplete_distinct = DistinctSortLimitValuesEnvelope(false);
  auto negative_offset = DistinctSortLimitValuesEnvelope(true);
  for (auto& operand : incomplete_distinct.operands) {
    if (operand.type == "relational_node_binding_v1" &&
        operand.name == "2") {
      operand.value =
          "6167677265676174652e71756572792d64697374696e63742e7631|"
          "1|-|-|-";
    }
  }
  for (auto& operand : negative_offset.operands) {
    if (operand.type == "relational_expression_v1" &&
        operand.name == "18") {
      operand.value = "1|-|2|-|-|1|-|2d31";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result,
               "QOW-DIAG-RELATIONAL-LIVE-COMPOSITION-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(incomplete_distinct)) &&
          refused_atomically(std::move(negative_offset)),
      "incomplete DISTINCT coverage or negative OFFSET published partial "
      "composition evidence");
}

sblr::SblrOperationEnvelope PivotValuesEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "qow.live.values.pivot");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-000000019400"},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-000000019401|"
       "019f0000-0000-7400-8000-000000019411|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-000000019402|"
       "019f0000-0000-7400-8000-000000019412|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       "019f0000-0000-7300-8000-000000019403|"
       "019f0000-0000-7400-8000-000000019413|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "4",
       "019f0000-0000-7300-8000-000000019404|"
       "019f0000-0000-7400-8000-000000019414|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "5",
       "019f0000-0000-7300-8000-000000019405|"
       "019f0000-0000-7400-8000-000000019415|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "6",
       "019f0000-0000-7300-8000-000000019406|"
       "019f0000-0000-7400-8000-000000019416|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "7",
       "019f0000-0000-7300-8000-000000019407|"
       "019f0000-0000-7400-8000-000000019417|1|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "2", "1|-|2|-|-|1|-|31"},
      {"relational_expression_v1", "3", "1|-|3|-|-|1|-|3130"},
      {"relational_expression_v1", "4", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "5", "1|-|2|-|-|1|-|32"},
      {"relational_expression_v1", "6", "1|-|3|-|-|1|-|3230"},
      {"relational_expression_v1", "7", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "8", "1|-|2|-|-|1|-|31"},
      {"relational_expression_v1", "9", "1|-|3|-|-|1|-|35"},
      {"relational_expression_v1", "10", "1|-|1|-|-|1|-|32"},
      {"relational_expression_v1", "11", "1|-|2|-|-|1|-|32"},
      {"relational_expression_v1", "12", "1|-|3|-|-|1|-|37"},
      {"relational_expression_v1", "13", "1|-|1|-|-|1|-|32"},
      {"relational_expression_v1", "14", "1|-|2|-|-|1|-|33"},
      {"relational_expression_v1", "15", "1|-|3|-|-|1|-|313030"},
      {"relational_expression_v1", "16",
       "3|-|1|-|019f0000-0000-7500-8000-000000019421|-|-|-"},
      {"relational_expression_v1", "17",
       "3|-|2|-|019f0000-0000-7500-8000-000000019422|-|-|-"},
      {"relational_expression_v1", "18",
       "3|-|3|-|019f0000-0000-7500-8000-000000019423|-|-|-"},
      {"relational_expression_v1", "19",
       "4|18|4|019de5fc-2400-72e4-8549-82b2eef5a777|-|-|-|-"},
      {"relational_expression_v1", "20",
       "4|18|5|019de5fc-2400-784a-9aec-371f8b95b7ea|-|-|-|-"},
      {"relational_expression_v1", "21",
       "4|18|6|019de5fc-2400-72e4-8549-82b2eef5a777|-|-|-|-"},
      {"relational_expression_v1", "22",
       "4|18|7|019de5fc-2400-784a-9aec-371f8b95b7ea|-|-|-|-"},
      {"relational_expression_v1", "23", "1|-|2|-|-|1|-|31"},
      {"relational_expression_v1", "24", "1|-|2|-|-|1|-|32"},
      {"relational_output_v1", "1", "1|1|1|1|0|67726f75705f6b6579"},
      {"relational_output_v1", "2", "1|2|2|1|1|666f725f6b6579"},
      {"relational_output_v1", "3", "1|3|3|1|2|616d6f756e74"},
      {"relational_output_v1", "4", "2|16|1|1|0|67726f75705f6b6579"},
      {"relational_output_v1", "5", "2|19|4|1|1|66697273745f73756d"},
      {"relational_output_v1", "6", "2|20|5|1|2|66697273745f636f756e74"},
      {"relational_output_v1", "7", "2|21|6|1|3|7365636f6e645f73756d"},
      {"relational_output_v1", "8", "2|22|7|1|4|7365636f6e645f636f756e74"},
      {"relational_values_row_v1", "1", "1,2,3"},
      {"relational_values_row_v1", "2", "4,5,6"},
      {"relational_values_row_v1", "3", "7,8,9"},
      {"relational_values_row_v1", "4", "10,11,12"},
      {"relational_values_row_v1", "5", "13,14,15"},
      {"relational_node_v1", "1", "13|0|-|1,2,3|1,2,3,4,5"},
      {"relational_node_v1", "2", "14|0|1|1,4,5,6,7|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15|-|-|-"},
      {"relational_node_binding_v1", "2",
       EncodeHex("pivot.fixed-aggregate-list-one-for.exclude-nulls.v1") +
           "|16,17,19,20,21,22,23,24|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

sblr::SblrOperationEnvelope UnpivotValuesEnvelope(const bool include_nulls) {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "qow.live.values.unpivot");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-000000019500"},
      {"uuid", "relational_catalog_epoch_uuid", std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "2"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-000000019501|"
       "019f0000-0000-7400-8000-000000019511|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-000000019502|"
       "019f0000-0000-7400-8000-000000019512|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "3",
       "019f0000-0000-7300-8000-000000019503|"
       "019f0000-0000-7400-8000-000000019513|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "4",
       "019f0000-0000-7300-8000-000000019504|"
       "019f0000-0000-7400-8000-000000019514|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "5",
       "019f0000-0000-7300-8000-000000019505|"
       "019f0000-0000-7400-8000-000000019515|2|-|-|-|-|-"},
      {"relational_descriptor_v1", "6",
       "019f0000-0000-7300-8000-000000019506|"
       "019f0000-0000-7400-8000-000000019516|1|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "2", "1|-|2|-|-|1|-|3130"},
      {"relational_expression_v1", "3", "1|-|3|-|-|1|-|313030"},
      {"relational_expression_v1", "4", "1|-|4|-|-|1|-|3230"},
      {"relational_expression_v1", "5", "1|-|5|-|-|1|-|323030"},
      {"relational_expression_v1", "6", "1|-|1|-|-|1|-|32"},
      {"relational_expression_v1", "7", "1|-|2|-|-|7|-|2d"},
      {"relational_expression_v1", "8", "1|-|3|-|-|7|-|2d"},
      {"relational_expression_v1", "9", "1|-|4|-|-|7|-|2d"},
      {"relational_expression_v1", "10", "1|-|5|-|-|7|-|2d"},
      {"relational_expression_v1", "11",
       "3|-|1|-|019f0000-0000-7500-8000-000000019521|-|-|-"},
      {"relational_expression_v1", "12",
       "3|-|2|-|019f0000-0000-7500-8000-000000019522|-|-|-"},
      {"relational_expression_v1", "13",
       "3|-|3|-|019f0000-0000-7500-8000-000000019523|-|-|-"},
      {"relational_expression_v1", "14",
       "3|-|4|-|019f0000-0000-7500-8000-000000019524|-|-|-"},
      {"relational_expression_v1", "15",
       "3|-|5|-|019f0000-0000-7500-8000-000000019525|-|-|-"},
      {"relational_expression_v1", "16", "1|-|6|-|-|2|-|7131"},
      {"relational_expression_v1", "17", "1|-|6|-|-|2|-|7132"},
      {"relational_output_v1", "1", "1|1|1|1|0|67726f75705f6b6579"},
      {"relational_output_v1", "2", "1|2|2|1|1|713161"},
      {"relational_output_v1", "3", "1|3|3|1|2|713162"},
      {"relational_output_v1", "4", "1|4|4|1|3|713261"},
      {"relational_output_v1", "5", "1|5|5|1|4|713262"},
      {"relational_output_v1", "6", "2|11|1|1|0|67726f75705f6b6579"},
      {"relational_output_v1", "7", "2|16|6|1|1|71756172746572"},
      {"relational_output_v1", "8", "2|12|2|1|2|616d6f756e74"},
      {"relational_output_v1", "9", "2|13|3|1|3|62616c616e6365"},
      {"relational_values_row_v1", "1", "1,2,3,4,5"},
      {"relational_values_row_v1", "2", "6,7,8,9,10"},
      {"relational_node_v1", "1", "13|0|-|1,2,3,4,5|1,2"},
      {"relational_node_v1", "2", "15|0|1|1,6,2,3|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|"
       "1,2,3,4,5,6,7,8,9,10|-|-|-"},
      {"relational_node_binding_v1", "2",
       EncodeHex(include_nulls
                     ? "unpivot.fixed-value-list.include-nulls.v1"
                     : "unpivot.fixed-value-list.exclude-nulls.v1") +
           "|11,12,13,14,15,16,17|-|-|-"},
  };
  return FinalizeStatementContextEnvelope(std::move(envelope));
}

bool ValidatePivotUnpivotValuesSpine() {
  const auto pivot = sblr::DispatchTextualRelationalQueryForContractTest(
      {Context(), PivotValuesEnvelope(), {}});
  const auto pivot_replay =
      sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(), PivotValuesEnvelope(), {}});
  const auto unpivot_exclude =
      sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(), UnpivotValuesEnvelope(false), {}});
  const auto unpivot_include =
      sblr::DispatchTextualRelationalQueryForContractTest(
          {Context(), UnpivotValuesEnvelope(true), {}});
  for (const auto* result : {&pivot, &unpivot_exclude, &unpivot_include}) {
    if (!result->api_result.ok) {
      for (const auto& diagnostic : result->api_result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.detail << '\n';
      }
    }
  }
  const auto traversed = [](const auto& value, const std::size_t columns,
                            const std::size_t rows) {
    return value.accepted && value.optimizer_admitted &&
           value.optimizer_selected && value.physical_dag_published &&
           value.physical_dag_executed && value.runtime_actuals_attached &&
           value.canonical_result_published && value.api_result.ok &&
           value.diagnostics.empty() && value.logical_node_count == 2 &&
           value.physical_node_count == 2 &&
           value.canonical_result_column_count == columns &&
           value.canonical_result_row_count == rows;
  };
  bool passed = true;
  passed &= Require(traversed(pivot, 5, 2),
                    "multi-aggregate PIVOT did not traverse its selected DAG");
  const auto& pivot_rows = pivot.api_result.result_shape.rows;
  passed &= Require(
      pivot_rows.size() == 2 && pivot_rows[0].fields.size() == 5 &&
          pivot_rows[0].fields[0].second.encoded_value == "1" &&
          pivot_rows[0].fields[1].second.encoded_value == "15" &&
          pivot_rows[0].fields[2].second.encoded_value == "2" &&
          pivot_rows[0].fields[3].second.encoded_value == "20" &&
          pivot_rows[0].fields[4].second.encoded_value == "1" &&
          pivot_rows[1].fields[0].second.encoded_value == "2" &&
          pivot_rows[1].fields[1].second.state ==
              api::EngineValueState::sql_null &&
          pivot_rows[1].fields[2].second.encoded_value == "0" &&
          pivot_rows[1].fields[3].second.encoded_value == "7" &&
          pivot_rows[1].fields[4].second.encoded_value == "1",
      "PIVOT did not publish canonical SUM/COUNT state per fixed IN item");
  passed &= Require(
      pivot_replay.api_result.ok &&
          pivot_replay.selected_plan_uuid == pivot.selected_plan_uuid &&
          pivot_replay.canonical_result_bytes == pivot.canonical_result_bytes,
      "identical PIVOT input changed canonical plan/result bytes");
  passed &= Require(
      traversed(unpivot_exclude, 4, 2) && traversed(unpivot_include, 4, 4),
      "multi-value UNPIVOT INCLUDE/EXCLUDE NULLS did not traverse selected DAGs");
  const auto& excluded_rows = unpivot_exclude.api_result.result_shape.rows;
  const auto& included_rows = unpivot_include.api_result.result_shape.rows;
  passed &= Require(
      excluded_rows.size() == 2 && excluded_rows[0].fields.size() == 4 &&
          excluded_rows[0].fields[0].second.encoded_value == "1" &&
          excluded_rows[0].fields[1].second.encoded_value == "q1" &&
          excluded_rows[0].fields[2].second.encoded_value == "10" &&
          excluded_rows[0].fields[3].second.encoded_value == "100" &&
          excluded_rows[1].fields[1].second.encoded_value == "q2" &&
          excluded_rows[1].fields[2].second.encoded_value == "20" &&
          excluded_rows[1].fields[3].second.encoded_value == "200" &&
          included_rows.size() == 4 &&
          included_rows[2].fields[2].second.state ==
              api::EngineValueState::sql_null &&
          included_rows[3].fields[3].second.state ==
              api::EngineValueState::sql_null,
      "UNPIVOT did not preserve multi-value rows or NULL policy");

  auto malformed_pivot = PivotValuesEnvelope();
  auto malformed_unpivot = UnpivotValuesEnvelope(false);
  for (auto& operand : malformed_pivot.operands) {
    if (operand.type == "relational_node_binding_v1" && operand.name == "2") {
      operand.value =
          EncodeHex("pivot.fixed-aggregate-list-one-for.exclude-nulls.v1") +
          "|16,17,19,20,21,22|-|-|-";
    }
  }
  for (auto& operand : malformed_unpivot.operands) {
    if (operand.type == "relational_node_binding_v1" && operand.name == "2") {
      operand.value =
          EncodeHex("unpivot.fixed-value-list.exclude-nulls.v1") +
          "|11,12,13,14,15,16|-|-|-";
    }
  }
  const auto refused_payload = [](sblr::SblrOperationEnvelope envelope,
                                  const std::string_view diagnostic) {
    const auto value = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return value.accepted && value.optimizer_admitted &&
           !value.optimizer_selected && !value.physical_dag_published &&
           !value.physical_dag_executed &&
           !value.canonical_result_published && !value.api_result.ok &&
           value.canonical_result_bytes.empty() &&
           HasApiDiagnostic(value, diagnostic);
  };
  passed &= Require(
      refused_payload(std::move(malformed_pivot),
                      "QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1") &&
          refused_payload(std::move(malformed_unpivot),
                          "QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1"),
      "malformed PIVOT/UNPIVOT carrier published partial evidence");
  return passed;
}

bool ValidatePayloadRefusalIsAtomic() {
  auto malformed = ValuesEnvelope();
  malformed.operands[13].value = "1|-|1|-|-|1|-|6e6f74";
  auto invalid_uuid = ValuesEnvelope();
  invalid_uuid.operands[13].value = "1|-|1|-|-|5|-|6e6f74";
  auto invalid_temporal = ValuesEnvelope();
  invalid_temporal.operands[13].value =
      "1|-|1|-|-|4|-|323032362d39392d39395430303a30303a30305a";
  auto int8_overflow = RuntimeBreadthValuesEnvelope();
  auto decimal_overflow = RuntimeBreadthValuesEnvelope();
  for (auto& operand : int8_overflow.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "1") {
      operand.value = "1|-|1|-|-|1|-|313238";
    }
  }
  for (auto& operand : decimal_overflow.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "3") {
      operand.value = "1|-|3|-|-|1|-|31323334352e3637";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-VALUES-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(malformed)) &&
          refused_atomically(std::move(invalid_uuid)) &&
          refused_atomically(std::move(invalid_temporal)) &&
          refused_atomically(std::move(int8_overflow)) &&
          refused_atomically(std::move(decimal_overflow)),
      "invalid, width-overflow, or precision-overflow VALUES payload "
      "published partial evidence");
}

bool ValidateComposedScalarRefusalIsAtomic() {
  auto overflow = ComposedValuesEnvelope();
  auto divide_by_zero = ComposedValuesEnvelope();
  for (auto& operand : overflow.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "1") {
      operand.value = "1|-|1|-|-|1|-|39323233333732303336383534373735383037";
    }
  }
  for (auto& operand : divide_by_zero.operands) {
    if (operand.type == "relational_expression_v1" && operand.name == "2") {
      operand.value = "1|-|1|-|-|1|-|30";
    } else if (operand.type == "relational_expression_v1" &&
               operand.name == "3") {
      operand.value = "6|1,2|1|-|-|-|2f|-";
    }
  }
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {Context(), std::move(envelope), {}});
    return result.accepted && result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_bytes.empty() &&
           HasApiDiagnostic(
               result, "QOW-DIAG-RELATIONAL-LIVE-VALUES-PAYLOAD-V1");
  };
  return Require(
      refused_atomically(std::move(overflow)) &&
          refused_atomically(std::move(divide_by_zero)),
      "overflow or divide-by-zero published partial optimizer/executor evidence");
}

}  // namespace

// QOW-TEST-INTEGRATION-306-211-LIVE-VALUES-V1
int main() {
  const bool passed = ValidateLiveValuesSpine() &&
                      ValidateRuntimeBreadthValuesSpine() &&
                      ValidateLiveValuesPreResultRevocationIsAtomic() &&
                      ValidateGeneralSelectExecutionBoundary() &&
                      ValidateLiveStatementContextRefusalIsAtomic() &&
                      ValidateComposedScalarValuesSpine() &&
                      ValidateUnionAllValuesSpine() &&
                      ValidateUnionAllRefusalIsAtomic() &&
                      ValidateSetOperationProfilesSpine() &&
                      ValidateSetOperationNestingSpine() &&
                      ValidateInnerJoinValuesSpine() &&
                      ValidateInnerJoinThreeValuedPredicate() &&
                      ValidateInnerJoinRefusalIsAtomic() &&
                      ValidateAcceptedJoinKindsSpine() &&
                      ValidateRowDependentJoinPredicateSpine() &&
                      ValidateInnerJoinFilterProjectCompositionSpine() &&
                      ValidateInnerJoinFilterProjectSortCompositionSpine() &&
                      ValidateInnerJoinFilterProjectSortLimitCompositionSpine() &&
                      ValidateInnerJoinFilterProjectDistinctSortLimitSpine() &&
                      ValidateInnerJoinFilterProjectDistinctSortOffsetFetchSpine() &&
                      ValidateFilterValuesSpine() &&
                      ValidateFilterThreeValuedPredicate() &&
                      ValidateRowDependentFilterPredicateSpine() &&
                      ValidateFilterRefusalIsAtomic() &&
                      ValidateProjectValuesSpine() &&
                      ValidateProjectRefusalIsAtomic() &&
                      ValidateRowDependentProjectExpressionSpine() &&
                      ValidateProjectedExpressionSortCompositionSpine() &&
                      ValidateFilteredExpressionProjectCompositionSpine() &&
                      ValidateFilteredProjectedSortCompositionSpine() &&
                      ValidateFilteredProjectedSortLimitCompositionSpine() &&
                      ValidateFilteredProjectedDistinctSortLimitCompositionSpine() &&
                      ValidateFilteredProjectedDistinctSortOffsetFetchCompositionSpine() &&
                      ValidateNodeDrivenUnaryCompositionSpine() &&
                      ValidateNodeDrivenJoinCompositionSpine() &&
                      ValidateNodeDrivenAcceptedJoinKindsCompositionSpine() &&
                      ValidateNodeDrivenUnionAllCompositionSpine() &&
                      ValidateNodeDrivenExactSetProfilesCompositionSpine() &&
                      ValidateNodeDrivenTypeReconciledSetCompositionSpine() &&
                      ValidateNodeDrivenByNameSetCompositionSpine() &&
                      ValidateNodeDrivenCountStarCompositionSpine() &&
                      ValidateNodeDrivenCountExpressionCompositionSpine() &&
                      ValidateNodeDrivenSumExpressionCompositionSpine() &&
                      ValidateNodeDrivenAvgExpressionCompositionSpine() &&
                      ValidateNodeDrivenStatisticalAggregateCompositionSpine() &&
                      ValidateNodeDrivenPairStatisticalCompositionSpine() &&
                      ValidateNodeDrivenGroupedCountSumCompositionSpine() &&
                      ValidateNodeDrivenGroupingExpansionCompositionSpine() &&
                      ValidateNodeDrivenGroupedHavingCompositionSpine() &&
                      ValidateNodeDrivenStringAggCompositionSpine() &&
                      ValidateNodeDrivenComplexAggregateCompositionSpine() &&
                      ValidateNodeDrivenExtremumExpressionCompositionSpine() &&
                      ValidateNodeDrivenBooleanAggregateCompositionSpine() &&
                      ValidateNodeDrivenNestedExactSetCompositionSpine() &&
                      ValidateEmptyFilteredExpressionProjectionSpine() &&
                      ValidateLimitValuesSpine() &&
                      ValidateLimitRefusalIsAtomic() &&
                      ValidatePivotUnpivotValuesSpine() &&
                      ValidateGroupedCountSumValuesSpine() &&
                      ValidateGroupedCountSumRefusalIsAtomic() &&
                      ValidateRollupCountSumValuesSpine() &&
                      ValidateRollupCountSumRefusalIsAtomic() &&
                      ValidateRollupCountSumGroupingValuesSpine() &&
                      ValidateRollupCountSumGroupingRefusalIsAtomic() &&
                      ValidateCubeCountSumValuesSpine() &&
                      ValidateCubeCountSumRefusalIsAtomic() &&
                      ValidateCubeCountSumGroupingValuesSpine() &&
                      ValidateCubeCountSumGroupingRefusalIsAtomic() &&
                      ValidateGroupingSetsCountSumValuesSpine() &&
                      ValidateGroupingSetsCountSumGroupingValuesSpine() &&
                      ValidateGroupingSetsCountSumRefusalIsAtomic() &&
                      ValidateGlobalCountStarValuesSpine() &&
                      ValidateGlobalCountStarRefusalIsAtomic() &&
                      ValidateGlobalCountExpressionValuesSpine() &&
                      ValidateGlobalCountExpressionRefusalIsAtomic() &&
                      ValidateGlobalSumExpressionValuesSpine() &&
                      ValidateGlobalSumExpressionRefusalIsAtomic() &&
                      ValidateGlobalUnaryAggregateModifierValuesSpine() &&
                      ValidateGlobalUnaryAggregateModifierRefusalIsAtomic() &&
                      ValidateGlobalAvgExpressionValuesSpine() &&
                      ValidateGlobalAvgExpressionRefusalIsAtomic() &&
                      ValidateGlobalExtremumExpressionValuesSpine(false) &&
                      ValidateGlobalExtremumExpressionRefusalIsAtomic(false) &&
                      ValidateGlobalExtremumExpressionValuesSpine(true) &&
                      ValidateGlobalExtremumExpressionRefusalIsAtomic(true) &&
                      ValidateGlobalBooleanAggregateExpressionValuesSpine(
                          BooleanAggregateKind::kBoolAnd) &&
                      ValidateGlobalBooleanAggregateExpressionRefusalIsAtomic(
                          BooleanAggregateKind::kBoolAnd) &&
                      ValidateGlobalBooleanAggregateExpressionValuesSpine(
                          BooleanAggregateKind::kBoolOr) &&
                      ValidateGlobalBooleanAggregateExpressionRefusalIsAtomic(
                          BooleanAggregateKind::kBoolOr) &&
                      ValidateGlobalBooleanAggregateExpressionValuesSpine(
                          BooleanAggregateKind::kEvery) &&
                      ValidateGlobalBooleanAggregateExpressionRefusalIsAtomic(
                          BooleanAggregateKind::kEvery) &&
                      ValidateGlobalStatisticalAggregateExpressionValuesSpine() &&
                      ValidateGlobalStatisticalAggregateExpressionRefusalIsAtomic() &&
                      ValidateGlobalPairStatisticalAggregateModifierValuesSpine() &&
                      ValidateGlobalPairStatisticalAggregateModifierRefusalIsAtomic() &&
                      ValidateGlobalPairStatisticalAggregateExpressionValuesSpine() &&
                      ValidateGlobalPairStatisticalAggregateExpressionRefusalIsAtomic() &&
                      ValidateGlobalOrderedSetAggregateValuesSpine() &&
                      ValidateGlobalOrderedSetAggregateRefusalIsAtomic() &&
                      ValidateGlobalOrderedSetAggregateModifierValuesSpine() &&
                      ValidateGlobalOrderedSetAggregateModifierRefusalIsAtomic() &&
                      ValidateGlobalApproximateAggregateValuesSpine() &&
                      ValidateGlobalApproximateAggregateRefusalIsAtomic() &&
                      ValidateGlobalApproximateAggregateModifierValuesSpine() &&
                      ValidateGlobalApproximateAggregateModifierRefusalIsAtomic() &&
                      ValidateGlobalStringAggExpressionValuesSpine() &&
                      ValidateGlobalStringAggExpressionRefusalIsAtomic() &&
                      ValidateOrderedStringAggExpressionValuesSpine() &&
                      ValidateOrderedStringAggExpressionRefusalIsAtomic() &&
                      ValidateStringAggModifierValuesSpine() &&
                      ValidateStringAggModifierRefusalIsAtomic() &&
                      ValidateOrderedListaggExpressionValuesSpine() &&
                      ValidateOrderedListaggExpressionRefusalIsAtomic() &&
                      ValidateOrderedListaggModifierValuesSpine() &&
                      ValidateOrderedListaggModifierRefusalIsAtomic() &&
                      ValidateOrderedSingleCollectionValuesSpine() &&
                      ValidateOrderedSingleCollectionRefusalIsAtomic() &&
                      ValidateOrderedSingleCollectionModifierValuesSpine() &&
                      ValidateOrderedSingleCollectionModifierRefusalIsAtomic() &&
                      ValidateOrderedJsonObjectAggValuesSpine() &&
                      ValidateOrderedJsonObjectAggRefusalIsAtomic() &&
                      ValidateOrderedJsonObjectAggModifierValuesSpine() &&
                      ValidateOrderedJsonObjectAggModifierRefusalIsAtomic() &&
                      ValidateSortValuesSpine() &&
                      ValidateSortRefusalIsAtomic() &&
                      ValidateRowDependentSortExpressionSpine() &&
                      ValidateDistinctSortLimitValuesSpine() &&
                      ValidateDistinctSortLimitRefusalIsAtomic() &&
                      ValidatePayloadRefusalIsAtomic() &&
                      ValidateComposedScalarRefusalIsAtomic();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
